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
    //
    // Phase 4 stereo decorrelation: everything above (tank input, shifter
    // feeding back into the tank, feedbackDcBlocker, safetyLimiter,
    // tankOut) is completely unchanged -- this is deliberate, since that
    // recirculating loop's decayGain/threshold/cutoff were all empirically
    // tuned in Phases 2-4 and must not be disturbed. Decorrelation is added
    // purely as a feed-forward, output-only stage: shifter.processSample()
    // (called above to shift the tank's feedback) also advances a second,
    // quadrature-offset voice pair inside PitchShifter -- see
    // PitchShifter.h's class comment -- whose crossfaded output is read via
    // shifter.getQuadratureOutput() after tankOut is computed. That signal
    // gets its own DCBlocker (DC-blocking filter state, previousInput/
    // previousOutput, cannot be shared with feedbackDcBlocker without
    // corrupting both signals) and reuses the same safetyLimiter (stateless
    // -- only a fixed threshold, so sharing it across both signals is safe).
    // L is mixed from tankOut + shimmerWidthGain * safeFeedback (the primary
    // pair, already computed above), R from tankOut + shimmerWidthGain *
    // the quadrature-safe signal -- decorrelated because L/R draw from
    // different grain-phase pairs reading the identical tank content. Chosen
    // over per-channel tank duplication specifically because it needs no
    // second DattorroTank/delay line and cannot touch the already-tuned
    // recirculating loop above. See
    // docs/shimmer-reverb-implementation-plan.md's Phase 4 section.
    void process (juce::dsp::AudioBlock<float>& block);

    // Phase 5 (see docs/shimmer-reverb-implementation-plan.md): forwarding
    // setters exposing the tunables Phases 2-4 already implemented inside
    // DattorroTank/PitchShifter but that ShimmerReverbEngine never routed
    // to its own public API. Plain setters for empirical tuning by ear /
    // future APVTS parameter callbacks, same convention as
    // DattorroTank::setDecay()/SafetyLimiter::setThreshold() -- this class
    // still owns no parameter knowledge itself; APVTS/PluginProcessor
    // wiring is a later Phase 5 step, not this one.
    void setPitchShiftSemitones (float semitones);
    void setFeedback (float newFeedback);
    void setDamping (float newDamping);

    // Clamped to [0, 1]. Replaces the old setShimmerWidthGain() name now
    // that this is genuinely public API rather than an internal-only
    // setter -- kept as a single setter rather than two names for the same
    // knob.
    void setWidth (float newWidth);

    // NEW (Phase 5): dry/wet blend. 0 = fully dry (bypassed engine sounds
    // identical to unprocessed input), 1 = fully wet (today's existing
    // 100%-wet behavior, unchanged). Clamped to [0, 1]. Default 1.0f so
    // behavior is unchanged unless a caller explicitly sets it lower --
    // see process()'s per-channel blend formula.
    void setMix (float newMix);

    // NEW (Phase 5): smooth-to-dry bypass, not a hard gate. When bypassed,
    // the tank/shifter/DC-blocker/limiter chain keeps running exactly as
    // always underneath (never skipped) so any existing tail rings out
    // naturally instead of clicking/cutting off -- only the EFFECTIVE mix
    // value used in process()'s blend collapses to 0.0f (fully dry) while
    // bypassed, overriding whatever setMix() was last set to. Per-block/
    // sample-accurate smoothing of the mix transition (including this
    // bypass-driven jump) is explicitly PluginProcessor's job in a later
    // Phase 5 step (juce::SmoothedValue wiring against a smoothed
    // setMix()/setBypassed() call rate) -- this class intentionally does
    // not duplicate that smoothing itself, per
    // docs/shimmer-reverb-implementation-plan.md's Phase 5 task list
    // framing ("Add parameter smoothing ... in MilleniaAudioProcessor").
    void setBypassed (bool shouldBypass);

private:
    // Phase 5 default -- was Phase 3's hardcoded shiftSemitones constant,
    // now just the value setPitchShiftSemitones() is seeded with once in
    // prepare() so behavior is unchanged until a caller actually changes
    // it live.
    static constexpr float defaultPitchShiftSemitones = 12.0f; // classic shimmer octave-up default

    // How much of the (safety-netted) shifted signal is mixed directly into
    // each output channel to create width -- see the process() comment
    // above for the mix formula. Phase 5 wires this to a real parameter.
    static constexpr float defaultShimmerWidthGain = 0.3f;

    // Phase 5 defaults for the new dry/wet mix and bypass knobs -- fully
    // wet, not bypassed, so existing tests/behavior are unchanged unless a
    // caller explicitly calls setMix()/setBypassed().
    static constexpr float defaultMix = 1.0f;
    static constexpr bool defaultBypassed = false;

    DattorroTank tank;
    PitchShifter shifter;
    DCBlocker feedbackDcBlocker;
    SafetyLimiter safetyLimiter;

    // Phase 4: own DCBlocker instance for the quadrature (R-channel) signal
    // -- DCBlocker holds per-sample state (previousInput/previousOutput) so
    // it cannot be shared with feedbackDcBlocker without corrupting both
    // signals. safetyLimiter above is genuinely stateless (only a fixed
    // threshold) and is reused for both signals.
    DCBlocker quadratureDcBlocker;

    float shimmerWidthGain = defaultShimmerWidthGain;

    // Phase 5: dry/wet mix (see setMix()) and bypass (see setBypassed()).
    // bypassed does not gate/skip any processing -- it only forces the
    // EFFECTIVE mix used in process()'s blend to 0.0f, overriding mix,
    // so the tank's tail keeps ringing out naturally underneath instead of
    // being hard-cut.
    float mix = defaultMix;
    bool bypassed = defaultBypassed;

    // Mono scratch buffer, pre-sized in prepare() so process() never
    // allocates.
    juce::AudioBuffer<float> monoScratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShimmerReverbEngine)
};
