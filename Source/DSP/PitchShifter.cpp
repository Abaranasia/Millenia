#include "PitchShifter.h"

PitchShifter::PitchShifter() = default;
PitchShifter::~PitchShifter() = default;

void PitchShifter::prepare (const juce::dsp::ProcessSpec& spec)
{
    grainLengthSamples = grainLengthMs * 0.001f * (float) spec.sampleRate;
    baseDelaySamples = grainLengthSamples * baseDelayGrainMultiple;

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

    // Primary group: 4 equally-spaced phases 0.0/0.25/0.5/0.75 -- see
    // PitchShifter.h's class-level comment for why 4 equally-spaced phases
    // (any starting point) always satisfy the COLA constant-sum identity.
    for (int k = 0; k < numVoicesPerGroup; ++k)
        primaryVoices[(size_t) k].grainPhase = (float) k * 0.25f;

    // Quadrature group: same 4 equally-spaced phases, offset by 0.125 (half
    // of the primary group's own 0.25 inter-voice spacing) -- see the
    // class-level comment for why this generalizes the old 0.25 quadrature
    // offset from the old 2-voice primary pair's 0.5 spacing.
    for (int k = 0; k < numVoicesPerGroup; ++k)
        quadratureVoices[(size_t) k].grainPhase = 0.125f + (float) k * 0.25f;
}

void PitchShifter::setPitchShiftSemitones (float semitones)
{
    pitchRatio = std::pow (2.0f, semitones / 12.0f);
}

float PitchShifter::voiceDelaySamples (float phase) const noexcept
{
    return baseDelaySamples - phase * grainLengthSamples * (pitchRatio - 1.0f);
}

float PitchShifter::hannEnvelope (float phase) noexcept
{
    return 0.5f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi * phase));
}

float PitchShifter::processSample (float input)
{
    // Every sample gets exactly one push and one implicit-argument pop, so
    // the delay line's internal readPos/writePos relationship keeps
    // advancing in lockstep (JUCE's DelayLine assumes this one push/pop
    // pair per sample -- see juce_DelayLine.cpp). The pop's return value is
    // discarded; its only purpose is that advancement. The explicit-offset
    // peeks below (8 of them, one per voice across both groups) ride on top
    // of that steadily-advancing base, the same mechanism
    // DattorroTank::peekTap() already relies on.
    delayLine.pushSample (0, input);
    delayLine.popSample (0);

    // No interpolator-reset step is needed here at any voice's grain wrap:
    // DelayLine<Lagrange3rd>'s interpolation carries no history/state
    // between calls (unlike e.g. Thiran) -- each read is a pure function of
    // the *current* delay value and the four nearby buffer samples (see
    // juce_DelayLine.cpp's interpolateSample(), Lagrange3rd branch). Jumping
    // a voice's delay value discontinuously between samples therefore
    // produces a perfectly valid interpolated read every time; the
    // content-discontinuity that jump causes is masked by the Hann envelope
    // below being exactly zero at that same instant.

    // Primary group: 4 voices, same role as the old voiceA/voiceB pair.
    // Weighted sum is normalized by 2.0/numVoicesPerGroup = 0.5 to restore
    // unity gain -- see PitchShifter.h's class-level comment for the full
    // COLA derivation and an explicit warning about why this factor matters.
    float primarySum = 0.0f;
    for (int k = 0; k < numVoicesPerGroup; ++k)
    {
        const float phase = primaryVoices[(size_t) k].grainPhase;
        const float delaySamples = voiceDelaySamples (phase);
        const float sample = delayLine.popSample (0, delaySamples, false);
        primarySum += sample * hannEnvelope (phase);
    }
    const float output = 0.5f * primarySum; // 2.0f / numVoicesPerGroup, N=4

    // Quadrature group: same Hann-crossfade and normalization math as the
    // primary group, just applied to the phase-offset voices that feed
    // ShimmerReverbEngine's stereo-width path -- reads the exact same
    // delayLine at this same instant (the push/pop above already advanced
    // the shared line for this sample, nothing extra needed here).
    float quadratureSum = 0.0f;
    for (int k = 0; k < numVoicesPerGroup; ++k)
    {
        const float phase = quadratureVoices[(size_t) k].grainPhase;
        const float delaySamples = voiceDelaySamples (phase);
        const float sample = delayLine.popSample (0, delaySamples, false);
        quadratureSum += sample * hannEnvelope (phase);
    }
    quadratureOutput = 0.5f * quadratureSum; // same normalization as above

    // Wrap via subtraction (not modulo-by-reassignment) to avoid floating
    // point drift; every voice in both groups increments at the identical
    // rate and wraps the same way, so each voice's fixed starting-phase
    // offset (set in reset()) is maintained automatically.
    for (int k = 0; k < numVoicesPerGroup; ++k)
    {
        primaryVoices[(size_t) k].grainPhase += 1.0f / grainLengthSamples;
        if (primaryVoices[(size_t) k].grainPhase >= 1.0f)
            primaryVoices[(size_t) k].grainPhase -= 1.0f;
    }

    for (int k = 0; k < numVoicesPerGroup; ++k)
    {
        quadratureVoices[(size_t) k].grainPhase += 1.0f / grainLengthSamples;
        if (quadratureVoices[(size_t) k].grainPhase >= 1.0f)
            quadratureVoices[(size_t) k].grainPhase -= 1.0f;
    }

    return output;
}
