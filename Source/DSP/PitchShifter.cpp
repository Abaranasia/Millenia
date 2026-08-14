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
        primaryGrains[(size_t) primaryNextSlot] = { true, 0 };
        primaryNextSlot = (primaryNextSlot + 1) % maxConcurrentGrainsPerGroup;
        primarySamplesUntilLaunch = hopSamples;
    }

    float primaryWeightedSum = 0.0f;
    float primaryWeightSum = 0.0f;

    for (auto& grain : primaryGrains)
    {
        if (! grain.active)
            continue;

        const float delaySamples = grainDelaySamples (grain.age);
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
        quadratureGrains[(size_t) quadratureNextSlot] = { true, 0 };
        quadratureNextSlot = (quadratureNextSlot + 1) % maxConcurrentGrainsPerGroup;
        quadratureSamplesUntilLaunch = hopSamples;
    }

    float quadratureWeightedSum = 0.0f;
    float quadratureWeightSum = 0.0f;

    for (auto& grain : quadratureGrains)
    {
        if (! grain.active)
            continue;

        const float delaySamples = grainDelaySamples (grain.age);
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
