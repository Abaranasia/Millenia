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

    // Phase 4: quadrature (90-degree) offset from the primary pair -- C
    // starts a quarter-cycle after A, D keeps the same fixed 0.5 offset from
    // C that B has from A. See the class-level comment in PitchShifter.h.
    voiceC.grainPhase = 0.25f;
    voiceD.grainPhase = 0.75f;
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

    // Phase 4: quadrature pair reads the exact same delayLine at this same
    // instant, just at a different grain-phase offset (see class-level
    // comment in PitchShifter.h) -- same push/pop above already advanced the
    // shared line for this sample, nothing extra needed here.
    const float delayC = voiceDelaySamples (voiceC.grainPhase);
    const float delayD = voiceDelaySamples (voiceD.grainPhase);

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
    const float sampleC = delayLine.popSample (0, delayC, false);
    const float sampleD = delayLine.popSample (0, delayD, false);

    const float output = sampleA * hannEnvelope (voiceA.grainPhase)
                        + sampleB * hannEnvelope (voiceB.grainPhase);

    // Same Hann-crossfade math as the primary pair, just applied to C/D --
    // hannEnvelope(p) + hannEnvelope(p+0.5) == 1 identically for any phase
    // p, so this is exactly as artifact-free as the A/B crossfade above.
    quadratureOutput = sampleC * hannEnvelope (voiceC.grainPhase)
                      + sampleD * hannEnvelope (voiceD.grainPhase);

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

    // C/D advance in lockstep with A/B, same rate, same wrap-via-subtraction
    // convention -- their fixed 0.25/0.75 starting offsets are preserved
    // automatically for the same reason A/B's 0.0/0.5 offsets are.
    voiceC.grainPhase += 1.0f / grainLengthSamples;
    if (voiceC.grainPhase >= 1.0f)
        voiceC.grainPhase -= 1.0f;

    voiceD.grainPhase += 1.0f / grainLengthSamples;
    if (voiceD.grainPhase >= 1.0f)
        voiceD.grainPhase -= 1.0f;

    return output;
}
