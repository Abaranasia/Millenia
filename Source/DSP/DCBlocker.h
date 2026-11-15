#pragma once

#include <JuceHeader.h>

// Phase 4: classic one-pole DC-blocking filter, mono, single-sample-at-a-time
// (see docs/shimmer-reverb-implementation-plan.md, Phase 4). Difference
// equation y[n] = x[n] - x[n-1] + r*y[n-1], transfer function
// H(z) = (1 - z^-1) / (1 - r*z^-1) -- a highpass with a zero at DC (z=1) and
// a pole at r (close to 1).
//
// r is derived from a target cutoff frequency in prepare() from the live
// sample rate, never hardcoded as a rate-dependent constant -- same
// convention DattorroTank/PitchShifter already use for their own ms-based
// constants. Standard small-r-margin approximation for this filter (valid
// when r is close to 1): cutoffHz ~= sampleRate / (2*pi) * (1 - r),
// rearranged to r = 1 - (2*pi*cutoffHz / sampleRate).
//
// Cutoff is fixed at 20Hz: low enough to leave all audible bass/low-mid
// content in the reverb tail untouched, high enough to actually remove true
// DC and any subsonic bias the pitch shifter's grain-crossfade interpolation
// could introduce on each recirculation (per shimmer-reverb-architecture.md's
// "DC offset accumulation" pitfall -- the accumulation risk this class
// targets is specifically inside the feedback loop's crossfading, not just
// at the final output, which is why ShimmerReverbEngine places this inside
// the loop rather than only at the very end).
class DCBlocker
{
public:
    DCBlocker() = default;

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset() noexcept;

    float processSample (float input) noexcept;

private:
    static constexpr float cutoffHz = 20.0f;

    float r = 0.0f; // computed in prepare() from the live sample rate
    float previousInput = 0.0f;
    float previousOutput = 0.0f;
};

inline void DCBlocker::prepare (const juce::dsp::ProcessSpec& spec)
{
    r = 1.0f - (juce::MathConstants<float>::twoPi * cutoffHz / (float) spec.sampleRate);

    reset();
}

inline void DCBlocker::reset() noexcept
{
    previousInput = 0.0f;
    previousOutput = 0.0f;
}

inline float DCBlocker::processSample (float input) noexcept
{
    float output = input - previousInput + r * previousOutput;

    previousInput = input;
    previousOutput = output;

    return output;
}
