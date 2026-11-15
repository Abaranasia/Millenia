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

    voiceA.grainPhase = 0.0f;
    voiceB.grainPhase = 0.5f; // fixed 50%-of-a-grain offset from voice A
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
    // discarded; its only purpose is that advancement. The two explicit-
    // offset peeks below ride on top of that steadily-advancing base, the
    // same mechanism DattorroTank::peekTap() already relies on.
    delayLine.pushSample (0, input);
    delayLine.popSample (0);

    const float delayA = voiceDelaySamples (voiceA.grainPhase);
    const float delayB = voiceDelaySamples (voiceB.grainPhase);

    // No interpolator-reset step is needed here at either voice's grain
    // wrap: DelayLine<Lagrange3rd>'s interpolation carries no history/state
    // between calls (unlike e.g. Thiran) -- each read is a pure function of
    // the *current* delay value and the four nearby buffer samples (see
    // juce_DelayLine.cpp's interpolateSample(), Lagrange3rd branch). Jumping
    // a voice's delay value discontinuously between samples therefore
    // produces a perfectly valid interpolated read every time; the
    // content-discontinuity that jump causes is masked by the Hann envelope
    // below being exactly zero at that same instant.
    const float sampleA = delayLine.popSample (0, delayA, false);
    const float sampleB = delayLine.popSample (0, delayB, false);

    const float output = sampleA * hannEnvelope (voiceA.grainPhase)
                        + sampleB * hannEnvelope (voiceB.grainPhase);

    // Wrap via subtraction (not modulo-by-reassignment) to avoid floating
    // point drift; both voices increment at the identical rate and wrap the
    // same way, so the fixed 0.5 phase offset between them is maintained
    // automatically.
    voiceA.grainPhase += 1.0f / grainLengthSamples;
    if (voiceA.grainPhase >= 1.0f)
        voiceA.grainPhase -= 1.0f;

    voiceB.grainPhase += 1.0f / grainLengthSamples;
    if (voiceB.grainPhase >= 1.0f)
        voiceB.grainPhase -= 1.0f;

    return output;
}
