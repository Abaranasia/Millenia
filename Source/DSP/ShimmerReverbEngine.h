#pragma once

#include <JuceHeader.h>
#include "DattorroTank.h"
#include "PitchShifter.h"
#include "DCBlocker.h"
#include "SafetyLimiter.h"

// Top-level DSP object: composes the Dattorro tank and the pitch shifter
// into the actual shimmer reverb feedback loop (Phase 3, see
// docs/shimmer-reverb-implementation-plan.md). Owns the mono scratch buffer
// too -- moved here from PluginProcessor now that this class is the one
// doing the mono-summing/processing/write-back, PluginProcessor just
// forwards the raw block. This is the only DSP class PluginProcessor talks
// to directly.
class ShimmerReverbEngine
{
public:
    ShimmerReverbEngine();
    ~ShimmerReverbEngine();

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();

    // Mono-handling contract identical to DattorroTank::process(): a
    // possibly multi-channel input block is averaged to mono internally,
    // and the tank's own mono output (not the shifter's) is written back to
    // every output channel -- per shimmer-reverb-concepts.md, what's
    // monitored is the tank's own reverb tail; the shifter only exists in
    // the recirculating feedback path.
    //
    // The shifter now sits directly inside the tank's own recirculation
    // path (DattorroTank::peekFeedbackSignal() -> shifter.processSample()
    // -> DattorroTank::processSample(input, shiftedFeedback)), per
    // shimmer-reverb-architecture.md's documented topology, rather than
    // running as a separate, weaker parallel feedback loop (see
    // docs/shimmer-reverb-implementation-plan.md's Phase 3/4 correction
    // note for the root cause this replaced).
    //
    // Phase 4 adds a one-pole DCBlocker (feedbackDcBlocker) into that same
    // recirculation path, between the shifter and the SafetyLimiter
    // (safetyLimiter): DC removal has to precede the limiter's nonlinear
    // soft-knee shaping (a DC-biased signal clips asymmetrically), and has
    // to happen inside the loop itself (not just at the final output) since
    // the pitch shifter's grain-crossfade interpolation can introduce
    // subsonic bias on every recirculation, per
    // shimmer-reverb-architecture.md's "DC offset accumulation" pitfall.
    // safetyLimiter itself replaces an earlier bare std::tanh(...) stopgap
    // with a real, memoryless soft-knee limiter -- see SafetyLimiter.h for
    // the full design rationale. Because of this, the tank's
    // own decayGain (DattorroTank::setDecay(), default 0.6f, already inside
    // the architecture doc's documented 0.6-0.85 "feedback gain" range) is
    // now the single knob controlling both how much sustains AND how much
    // of that sustain is pitch-shifted -- there is no longer a separate
    // "shimmer amount" vs. "reverb decay" knob, since after this change
    // there's only one recirculation path. Reconciling this with Phase 5's
    // originally-planned separate "feedback" parameter is an open decision
    // for that phase, not resolved here.
    void process (juce::dsp::AudioBlock<float>& block);

private:
    // Phase 3 hardcoded constant -- real parameter control is Phase 5's
    // job; this phase is about the signal path being correct.
    static constexpr float shiftSemitones = 12.0f; // classic shimmer octave-up default

    DattorroTank tank;
    PitchShifter shifter;
    DCBlocker feedbackDcBlocker;
    SafetyLimiter safetyLimiter;

    // Mono scratch buffer, pre-sized in prepare() so process() never
    // allocates.
    juce::AudioBuffer<float> monoScratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShimmerReverbEngine)
};
