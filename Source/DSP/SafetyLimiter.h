#pragma once

#include <JuceHeader.h>

// Phase 4: permanent safety net placed inside the shimmer feedback loop,
// replacing the bare std::tanh(...) stopgap that briefly lived in
// ShimmerReverbEngine::process() during Phase 3/4 (see
// docs/shimmer-reverb-implementation-plan.md, Phase 4).
//
// Design decision -- memoryless soft-knee waveshaper, NOT a lookahead peak
// limiter with an envelope follower:
//   - A lookahead limiter needs an internal delay buffer to see peaks before
//     they arrive. Adding delay HERE, inside the recirculating loop, would
//     corrupt DattorroTank's own delay-line lengths, which were empirically
//     tuned across Phases 2-3 against exact round-trip timing (see
//     DattorroTank.h's defaultDecayGain comment history) -- this is not the
//     plugin's final output stage, where a few extra samples of latency
//     would be free.
//   - An envelope follower's attack/release time constants could interact
//     unpredictably with the loop's own decay dynamics (DattorroTank's
//     decayGain), effectively adding a second, uncoordinated set of time
//     constants fighting the tuned one.
//   - A memoryless shaper has zero added latency and no time-constant of its
//     own -- it reacts identically to a given instantaneous sample value
//     regardless of the loop's own dynamics, which is exactly what a safety
//     net should do: never itself become part of what needs tuning.
//
// Algorithm -- soft-knee clipper, unity/transparent below threshold, smoothly
// saturating toward an asymptote of 1.0 above it, continuous in both value
// AND slope at the knee (no audible click when a signal crosses threshold):
//
//   absInput = |input|
//   if absInput <= threshold:
//       output = input                                     // exact passthrough
//   else:
//       sign = input < 0 ? -1 : 1
//       excess = (absInput - threshold) / (1 - threshold)
//       output = sign * (threshold + (1 - threshold) * tanh(excess))
//
// Continuity: at absInput == threshold, both branches evaluate to exactly
// input (excess = 0, tanh(0) = 0, so the else-branch collapses to
// sign * threshold == input). Slope continuity (C1): the passthrough
// branch has slope 1 everywhere; tanh's derivative at 0 is 1, so the
// else-branch's slope at the knee is also exactly 1 -- the two pieces meet
// with matching slope, not just matching value, so there is no audible kink
// when a signal crosses threshold.
//
// threshold defaults to 0.8f: leaves headroom below the 1.0 asymptote, and
// matches this codebase's general "strictly < 1.0" ceiling convention used
// elsewhere (e.g. ShimmerReverbEngine's removed feedbackGain clamp and
// DattorroTank's decayGain range).
//
// IMPORTANT distinction from the std::tanh(...) this replaces: bare tanh(x)
// slightly compresses every sample, even small ones (tanh(0.1) ~= 0.0997,
// not exactly 0.1) -- it shapes the whole signal, always. This limiter is
// EXACTLY unity below threshold, so small/moderate recirculating signal
// passes completely untouched; only the genuinely loud tail gets shaped.
// This is a real behavior change to the loop's small-signal gain, not just
// a refactor -- see ShimmerReverbEngine.cpp's call site comment and
// docs/shimmer-reverb-implementation-plan.md's Phase 4 notes for the
// re-verification this required against DattorroTank's tuned decayGain.
class SafetyLimiter
{
public:
    SafetyLimiter() = default;

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    float processSample (float input);

    // Plain setter for empirical tuning by ear, same convention as
    // DattorroTank::setDecay()/setDamping() -- Phase 5 wires this to a real
    // parameter; this class owns no parameter knowledge itself.
    void setThreshold (float newThreshold);

private:
    static constexpr float defaultThreshold = 0.8f;
    float threshold = defaultThreshold;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SafetyLimiter)
};
