#include "PitchShifter.h"

PitchShifter::PitchShifter() = default;
PitchShifter::~PitchShifter() = default;

void PitchShifter::prepare (const juce::dsp::ProcessSpec& spec)
{
    grainLengthSamples = grainLengthMs * 0.001f * (float) spec.sampleRate;
    baseDelaySamples = grainLengthSamples * baseDelayGrainMultiple;

    grainLengthSamplesInt = juce::roundToInt (grainLengthSamples);
    hopSamples = juce::jmax (1, juce::roundToInt (grainLengthSamples * (1.0f - crossfadeFraction)));
    crossfadeSamplesInt = juce::jmax (1, juce::roundToInt (grainLengthSamples * crossfadeFraction));

    // See alignmentSearchRadiusSamples' header comment for the two-anchor
    // search rationale; see PitchShifterTests.cpp's DIAGNOSTIC tests and
    // docs/shimmer-reverb-implementation-plan.md's Phase 7 follow-up for the
    // measured tuning trail that arrived at this value (22 samples at
    // 44.1kHz's crossfadeSamplesInt=88).
    alignmentSearchRadiusSamples = juce::jmax (16, crossfadeSamplesInt / 4);
    alignmentReferenceBuffer.assign ((size_t) crossfadeSamplesInt, 0.0f);

    auto neededSamples = baseDelaySamples + grainLengthSamples * maxDelayExtraGrainMultiple;
    delayLine.setMaximumDelayInSamples (juce::roundToInt (neededSamples) + 1);

    juce::dsp::ProcessSpec monoSpec { spec.sampleRate, spec.maximumBlockSize, 1 };
    delayLine.prepare (monoSpec);

    // The implicit-argument popSample() in processSample() needs a valid
    // configured delay to interpolate against even though its return value
    // is discarded -- the actual value doesn't matter (see header comment),
    // it just has to be in-range, so this arbitrary starting point is fine.
    delayLine.setDelay (baseDelaySamples);

    reset();
}

void PitchShifter::reset()
{
    // Only clears the delay line's internal buffer/read-write positions --
    // does not touch the configured delay set above, so the implicit pop in
    // processSample() still has a valid value to use after this call.
    delayLine.reset();

    for (auto& grain : primaryGrains)
        grain = Grain {};

    for (auto& grain : quadratureGrains)
        grain = Grain {};

    primaryNextSlot = 0;
    quadratureNextSlot = 0;

    // Primary pool launches its first grain on the very first
    // processSample() call.
    primarySamplesUntilLaunch = 0;

    // Quadrature pool is staggered by half a hop relative to primary -- this
    // is what keeps the quadrature output decorrelated from the primary
    // output despite reading the same shared delay line (see the
    // class-level comment's "grain-pool layout" section).
    quadratureSamplesUntilLaunch = hopSamples / 2;

    // Running per-pool alignment-offset estimates must rewind to the fixed
    // nominal origin along with everything else, or a stale offset from
    // before this reset() would seed the first post-reset search.
    primaryLastOffset = 0.0f;
    quadratureLastOffset = 0.0f;
}

void PitchShifter::setPitchShiftSemitones (float semitones)
{
    pitchRatio = std::pow (2.0f, semitones / 12.0f);
}

float PitchShifter::grainDelaySamples (int elapsed) const noexcept
{
    return baseDelaySamples - (float) elapsed * (pitchRatio - 1.0f);
}

float PitchShifter::grainWindow (int elapsed) const noexcept
{
    if (elapsed < crossfadeSamplesInt)
        return 0.5f * (1.0f - std::cos (juce::MathConstants<float>::pi * (float) elapsed / (float) crossfadeSamplesInt));

    const int samplesFromEnd = grainLengthSamplesInt - elapsed;
    if (samplesFromEnd < crossfadeSamplesInt)
        return 0.5f * (1.0f - std::cos (juce::MathConstants<float>::pi * (float) samplesFromEnd / (float) crossfadeSamplesInt));

    return 1.0f;
}

float PitchShifter::findAlignmentOffset (const std::array<Grain, maxConcurrentGrainsPerGroup>& grains, float previousOffset)
{
    const Grain* outgoing = nullptr;
    for (auto& grain : grains)
        if (grain.active && (outgoing == nullptr || grain.age > outgoing->age))
            outgoing = &grain;

    if (outgoing == nullptr)
        return previousOffset;

    const float outgoingBaseDelay = baseDelaySamples + outgoing->baseDelayOffset;
    const float outgoingCurrentDelay = outgoingBaseDelay - (float) outgoing->age * (pitchRatio - 1.0f);

    const int windowSamples = (int) alignmentReferenceBuffer.size();

    // NOTE: the rate used here is pitchRatio, not (pitchRatio - 1.0f) as in
    // grainDelaySamples() above. grainDelaySamples()'s rate describes how a
    // REAL grain's delay-from-now evolves as REAL TIME also advances one
    // sample per elapsed step (the write pointer moves too, so the *net*
    // rate the absolute read position gains on "now" is only
    // 1 - (-(pitchRatio-1)) = pitchRatio per real sample, but that extra "1"
    // is free -- it comes from time itself elapsing). Here we are NOT
    // advancing real time between k steps (no push/pop happens in this
    // loop) -- we're peeking multiple future instants from a single frozen
    // buffer snapshot -- so the "time elapses for free" term is unavailable
    // and the delay-from-this-fixed-instant must close the entire gap
    // itself, at the full pitchRatio rate. Using (pitchRatio - 1.0f) here
    // was verified (via the diagnostic FFT test) to degenerate at
    // pitchRatio == 1.0 -- every k read the identical sample, producing a
    // meaningless flat "reference window" and making the sideband WORSE
    // (0.22dB, i.e. louder than the fundamental) instead of better. Fixed by
    // using the correct pitchRatio rate; see PitchShifterTests.cpp's
    // DIAGNOSTIC test for the measured before/after numbers.
    for (int k = 0; k < windowSamples; ++k)
    {
        const float delaySamples = juce::jmax (0.0f, outgoingCurrentDelay - (float) k * pitchRatio);
        alignmentReferenceBuffer[(size_t) k] = delayLine.popSample (0, delaySamples, false);
    }

    float bestScore = -1.0f;
    float bestOffset = previousOffset; // fallback: carry the previous offset forward if no lag improves on it

    // Two-anchor search: re-check both a small window around the running
    // per-pool offset estimate (previousOffset -- cheap continuous drift
    // tracking, see this function's header-comment history) AND a small
    // window around the fixed nominal origin (0.0f -- the same anchor the
    // original, far more expensive fixed-origin design always used). This
    // was added after measuring that a previousOffset-ONLY incremental
    // search, however wide its radius, could not get the +24 semitone
    // spectral-sideband test below roughly -8..-23dB (never reaching the
    // required -30dB) -- an exhaustive sweep from radius=1 up to radius=800
    // (matching the old fixed-origin design's full grainLengthSamplesInt)
    // showed it PLATEAUS on a bad, self-consistent local optimum well away
    // from the actually-best (zero-anchored) alignment once pitchRatio gets
    // large (+24st, ratio=4.0): the search only ever looks near wherever it
    // already is, so once it wanders it has no way back to the known-good
    // region near zero. Re-checking the zero anchor every single hop, at the
    // SAME small radius as the incremental search (so it stays cheap -- just
    // one more small window, not a full grain-length search), guarantees the
    // search can never permanently drift away from that known-good solution,
    // while the previousOffset anchor still gives the fast continuous-drift
    // tracking the incremental design was introduced for at ordinary shift
    // amounts. See alignmentSearchRadiusSamples' own comment and
    // PitchShifterTests.cpp's DIAGNOSTIC tests for the measured numbers this
    // fix produced.
    const float anchorOffsets[] = { previousOffset, 0.0f };

    for (float anchor : anchorOffsets)
    {
        for (int lag = -alignmentSearchRadiusSamples; lag <= alignmentSearchRadiusSamples; ++lag)
        {
            const float candidateOffset = anchor + (float) lag;
            const float candidateBaseDelay = baseDelaySamples + candidateOffset;

            float dot = 0.0f, refEnergy = 0.0f, candEnergy = 0.0f;

            for (int k = 0; k < windowSamples; ++k)
            {
                // Same fixed-snapshot rate correction as the reference-window
                // loop above: pitchRatio, not (pitchRatio - 1.0f).
                const float delaySamples = juce::jmax (0.0f, candidateBaseDelay - (float) k * pitchRatio);
                const float candidateSample = delayLine.popSample (0, delaySamples, false);

                dot += alignmentReferenceBuffer[(size_t) k] * candidateSample;
                refEnergy += alignmentReferenceBuffer[(size_t) k] * alignmentReferenceBuffer[(size_t) k];
                candEnergy += candidateSample * candidateSample;
            }

            const float denom = std::sqrt (refEnergy * candEnergy);
            const float score = denom > 1.0e-8f ? dot / denom : -1.0f;

            if (score > bestScore)
            {
                bestScore = score;
                bestOffset = candidateOffset;
            }
        }
    }

    // Safety clamp: keep the cumulative offset within the delay line's
    // already-verified capacity margin (baseDelayGrainMultiple/
    // maxDelayExtraGrainMultiple headroom), regardless of how far an
    // unbounded incremental search could in principle wander over a long
    // sustained tone.
    return juce::jlimit (-(float) grainLengthSamplesInt, (float) grainLengthSamplesInt, bestOffset);
}

float PitchShifter::processSample (float input)
{
    // Every sample gets exactly one push and one implicit-argument pop, so
    // the delay line's internal readPos/writePos relationship keeps
    // advancing in lockstep (JUCE's DelayLine assumes this one push/pop
    // pair per sample -- see juce_DelayLine.cpp). The pop's return value is
    // discarded; its only purpose is that advancement. The explicit-offset
    // peeks below ride on top of that steadily-advancing base, the same
    // mechanism DattorroTank::peekTap() already relies on.
    delayLine.pushSample (0, input);
    delayLine.popSample (0);

    // No interpolator-reset step is needed here at any grain's launch:
    // DelayLine<Lagrange3rd>'s interpolation carries no history/state
    // between calls (unlike e.g. Thiran) -- each read is a pure function of
    // the *current* delay value and the four nearby buffer samples (see
    // juce_DelayLine.cpp's interpolateSample(), Lagrange3rd branch).

    // Primary pool: launch on schedule, accumulate the weighted sum over
    // every currently-active grain, then normalize by the live weight sum
    // (see the class-level comment for why this per-sample normalization
    // replaces the old fixed COLA-derived constant).
    if (primarySamplesUntilLaunch <= 0)
    {
        primaryLastOffset = findAlignmentOffset (primaryGrains, primaryLastOffset);
        primaryGrains[(size_t) primaryNextSlot] = { true, 0, primaryLastOffset };
        primaryNextSlot = (primaryNextSlot + 1) % maxConcurrentGrainsPerGroup;
        primarySamplesUntilLaunch = hopSamples;
    }

    float primaryWeightedSum = 0.0f;
    float primaryWeightSum = 0.0f;

    for (auto& grain : primaryGrains)
    {
        if (! grain.active)
            continue;

        const float delaySamples = grainDelaySamples (grain.age) + grain.baseDelayOffset;
        const float sample = delayLine.popSample (0, delaySamples, false);
        const float weight = grainWindow (grain.age);

        primaryWeightedSum += sample * weight;
        primaryWeightSum += weight;
    }

    const float output = primaryWeightSum > 1.0e-6f ? primaryWeightedSum / primaryWeightSum : 0.0f;

    for (auto& grain : primaryGrains)
    {
        if (! grain.active)
            continue;

        grain.age += 1;
        if (grain.age >= grainLengthSamplesInt)
            grain.active = false;
    }

    primarySamplesUntilLaunch -= 1;

    // Quadrature pool: identical mechanism, staggered launch schedule, same
    // shared delayLine read at this same instant.
    if (quadratureSamplesUntilLaunch <= 0)
    {
        quadratureLastOffset = findAlignmentOffset (quadratureGrains, quadratureLastOffset);
        quadratureGrains[(size_t) quadratureNextSlot] = { true, 0, quadratureLastOffset };
        quadratureNextSlot = (quadratureNextSlot + 1) % maxConcurrentGrainsPerGroup;
        quadratureSamplesUntilLaunch = hopSamples;
    }

    float quadratureWeightedSum = 0.0f;
    float quadratureWeightSum = 0.0f;

    for (auto& grain : quadratureGrains)
    {
        if (! grain.active)
            continue;

        const float delaySamples = grainDelaySamples (grain.age) + grain.baseDelayOffset;
        const float sample = delayLine.popSample (0, delaySamples, false);
        const float weight = grainWindow (grain.age);

        quadratureWeightedSum += sample * weight;
        quadratureWeightSum += weight;
    }

    quadratureOutput = quadratureWeightSum > 1.0e-6f ? quadratureWeightedSum / quadratureWeightSum : 0.0f;

    for (auto& grain : quadratureGrains)
    {
        if (! grain.active)
            continue;

        grain.age += 1;
        if (grain.age >= grainLengthSamplesInt)
            grain.active = false;
    }

    quadratureSamplesUntilLaunch -= 1;

    return output;
}
