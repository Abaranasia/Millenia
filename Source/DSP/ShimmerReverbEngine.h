#pragma once

#include <JuceHeader.h>
#include "DattorroTank.h"
#include "PitchShifter.h"
#include "DCBlocker.h"
#include "SafetyLimiter.h"
#include "FreezeLeveler.h"
#include "FormantEnvelopeCorrector.h"
#include "LoopCapture.h"

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

    // Forwards to DattorroTank::setShimmerFeedbackGain() (see that method's
    // comment), clamped to [0, 1] here -- never amplifies above unity, so
    // Phase 4's runaway-safety margin (decayGain < 0.85) on the tank's OWN
    // natural recirculation is never affected by this parameter at all (it
    // only gains the separate, additive shimmer-injection term). Also kept
    // locally (see shimmerAmount below) so process()'s Width/decorrelation
    // term can be gated by it too -- that term reads the shifter's output
    // directly, not through DattorroTank, so DattorroTank owning the value
    // alone isn't enough to silence it (bug found by ear, 2026-08-21: Width
    // kept the shimmer character audible even at shimmerAmount=0.0f).
    void setShimmerAmount (float newShimmerAmount);

    // "Shimmer Sustain" task: clamped to [0, 1], same convention as
    // setShimmerAmount()/setWidth()/setFreezeAmount(). Controls how present
    // and how long-lived the shimmer layer is, independent of Feedback
    // (decayGain) and Shimmer Amount (shimmerFeedbackGain, the OVERALL
    // shimmer level). Two effects, both driven by this single value:
    //
    // 1. Forwards to DattorroTank::setMaxShimmerBlendWeight(), INVERTED (see
    // that method's comment) -- DELIBERATELY INVERTED from
    // maxShimmerBlendWeight's own sense (2026-09-06, by-ear follow-up):
    // measured behavior is that a HIGHER internal maxShimmerBlendWeight
    // actually decays FASTER (more of the recirculating budget rides the
    // externally-shifted, separately-delayed path, which loses energy every
    // pass via destructive interference -- see
    // Tests/Source/ShimmerReverbEngineTests.cpp's low-vs-high decay test for
    // the measured numbers). So newAmount=1.0 (dial fully up, "maximum
    // sustain") maps to maxShimmerBlendWeight=0.0 (shimmer content never
    // re-enters the recirculating loop, so the tank's own natural, uncombed
    // decay dominates -- genuinely the SLOWEST/most-sustained option), and
    // newAmount=0.0 to maxShimmerBlendWeight=1.0 (fastest decay).
    //
    // 2. ALSO gates process()'s Width/decorrelation sideShift injection
    // directly (2026-09-06, same-day widening after the by-ear report "I
    // don't clearly notice any difference"): effect (1) alone only shapes
    // whether shifted content re-enters DattorroTank's OWN internal mono
    // recirculation -- a slow, cumulative, easy-to-miss effect on the tail's
    // shape over many seconds. The actually-obvious, audible "shimmer is
    // there" character comes from a SEPARATE feed-forward term (see
    // process()'s sideShift/wetLeft/wetRight comment) that effect (1) never
    // touched at all. Gating that term too (same multiplicative-gate
    // convention shimmerAmount already uses there, 2026-08-21) makes turning
    // this dial down toward 0 produce an unmistakable, immediate reduction
    // in the shimmer's audible presence (collapsing to near-mono at 0.0f),
    // not just a subtle long-term tail-shape change.
    void setShimmerSustain (float newAmount);

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

    // Phase 9 Freeze (see docs/shimmer-reverb-implementation-plan.md):
    // forwards to DattorroTank::setFreezeAmount(), clamped to [0, 1] here
    // (same convention as setShimmerAmount()/setWidth()/setMix()). Also
    // stored locally so process() can mute the fresh dry input reaching the
    // tank's diffuser -- Freeze's mechanism is now three coordinated things
    // (mute new input + pin decayGain near unity + crossfade the shifter's
    // own ratio toward unity, see pitchShiftSemitones' comment below), and
    // the tank only ever owns the second of those.
    void setFreezeAmount (float amount);

    // Phase 10 (see docs/shimmer-reverb-implementation-plan.md): Loop
    // Freeze -- a NEW, fully independent, additive feature alongside Phase
    // 9's Freeze above (that mechanism is completely untouched by this one).
    // Forwards to loopCapture's own setter; clamped to [0, 1] here, same
    // convention as every other continuous setter in this class. Wired into
    // process() strictly after freezeLeveler's gain application and strictly
    // before the dry/wet mix, so Loop Freeze's "live" input is whatever
    // Phase 9's Freeze mechanism currently outputs -- see process()'s
    // comment at that exact spot for why, and LoopCapture.h for the capture/
    // playback mechanism itself.
    //
    // 2026-09-06 fix: this class used to store its own loopFreezeAmount
    // member and pass it into loopCapture.process() every sample, trusting
    // PluginProcessor to have already smoothed it -- but that caller-side
    // smoothing only applied once per host BLOCK, which clicked on a
    // large-enough buffer (see LoopCapture::setLoopFreezeAmount()'s comment
    // for the full root-cause trail). LoopCapture now owns a genuine
    // per-sample ramp itself, so this is a thin, un-smoothed passthrough --
    // the caller (PluginProcessor) can forward the raw APVTS toggle value
    // directly, same "read raw" convention as setLoopLengthMs() below.
    void setLoopFreezeAmount (float newAmount);

    // Thin passthrough to loopCapture.setLoopLengthMs() -- see that method's
    // own comment for why it only stores a PENDING length, applied at the
    // next rising edge, and why clamping happens inside LoopCapture itself
    // rather than here.
    void setLoopLengthMs (float newLoopLengthMs);

    // Test-only introspection -- same "peek" idiom as DattorroTank::
    // peekFeedbackSignal() (which this just forwards) and LoopCapture::
    // peekCapturedLoopSample(). Added 2026-09-06 for the "Shimmer Amount +
    // Sustain + Width glitch" investigation, to inspect the ACTUAL signal
    // reaching PitchShifter inside the real feedback loop (not a synthetic
    // standalone tone), which is what process() feeds to
    // shifter.processSample() every sample.
    float peekDryFeedback() const noexcept { return tank.peekFeedbackSignal(); }

    // Test-only introspection, same idiom -- forwards PitchShifter's own
    // getPrimaryLastOffset()/getQuadratureLastOffset() so a test can check
    // whether the two independently-searching grain pools are settling on
    // the SAME alignment offset or diverging, while driven by the real
    // feedback loop (not a synthetic standalone tone).
    float peekShifterPrimaryOffset() const noexcept { return shifter.getPrimaryLastOffset(); }
    float peekShifterQuadratureOffset() const noexcept { return shifter.getQuadratureLastOffset(); }

private:
    // Recomputes and applies the shifter's actual pitch ratio from the
    // current pitchShiftSemitones/freezeAmount -- shared by prepare(),
    // setPitchShiftSemitones(), and setFreezeAmount() so the crossfade curve
    // (see pitchShiftCrossfadeCurve below) only lives in one place.
    void updateShifterRatio();

    // Phase 9 Freeze pitch-ratio crossfade, curve fix (2026-08-22, by-ear
    // report: "increasing freeze seems to produce a pitch down change that
    // wasn't noticeable previously"). The original fix (linear
    // `pitchShiftSemitones * (1.0f - freezeAmount)`, see
    // pitchShiftSemitones' comment below) is CORRECT at freezeAmount=1.0
    // (ratio must reach exactly unity there, or the endless upward-cascading
    // drone this was built to fix comes back) but was audibly reducing the
    // shimmer's pitch shift far too early in the dial's travel -- at
    // freezeAmount=0.5 it had ALREADY cut a +12st shift in half (+6st),
    // which is exactly the "pitch down" the user heard well before Freeze
    // was anywhere near fully engaged. Raising (1.0f - freezeAmount) to this
    // power instead keeps the reduction negligible through most of the
    // dial's travel and concentrates it near the top; the curve still
    // reaches exactly 0 at freezeAmount=1.0 regardless of the exponent, so
    // the anti-cascade guarantee is unaffected by tuning this value.
    //
    // Steepened further, 2026-08-29 (by-ear report continued: the original
    // 2026-08-22 fix concentrated most of the reduction near the top of the
    // dial's travel, but a real fraction of it was still landing across
    // roughly the last quarter, e.g. at 4.0 a +12st shift is already down to
    // ~+8.2st by freezeAmount=0.75 -- audible, and easy to land on while
    // using the dial normally, not just at the very top). Raised 4.0 -> 16.0
    // to push nearly all of the reduction into the last ~10% of travel: at
    // 16.0, freezeAmount=0.75 retains ~+11.9st (a 1% cut, essentially
    // inaudible, vs 4.0's ~32% cut at the same point); freezeAmount=0.9
    // retains ~+9.8st (82%, vs 4.0's ~66%); freezeAmount=0.95 retains ~+6.7st
    // (56%, vs 4.0's ~46%) -- the glide is still real and still reaches
    // exactly 0 at freezeAmount=1.0, just compressed into a narrower final
    // stretch of the dial instead of starting around three-quarters of the
    // way through it. A REASONED value from this session's own numbers, NOT
    // exhaustively ear-tuned -- open to revisiting after another listening
    // pass (this exact convention already flagged the prior 4.0 value the
    // same way).
    static constexpr float pitchShiftCrossfadeCurve = 16.0f;

    // Phase 5 default -- was Phase 3's hardcoded shiftSemitones constant,
    // now just the value setPitchShiftSemitones() is seeded with once in
    // prepare() so behavior is unchanged until a caller actually changes
    // it live.
    static constexpr float defaultPitchShiftSemitones = 12.0f; // classic shimmer octave-up default

    // Phase 8 rework: this term used to inject a raw, full-strength copy of
    // the shifted signal into the wet output (see process()), which at high
    // settings read as a "parallel pitch shifter" artifact rather than
    // blended shimmer. It now injects only the L/R *difference* between the
    // primary/quadrature shifted signals (pure stereo-decorrelation
    // seasoning), so its default is lowered accordingly. Value is a REASONED
    // STARTING POINT pending a by-ear pass, not a final tuned constant.
    static constexpr float defaultShimmerWidthGain = 0.15f;

    // Phase 5 defaults for the new dry/wet mix and bypass knobs -- fully
    // wet, not bypassed, so existing tests/behavior are unchanged unless a
    // caller explicitly calls setMix()/setBypassed().
    static constexpr float defaultMix = 1.0f;
    static constexpr bool defaultBypassed = false;

    // Matches DattorroTank::defaultShimmerFeedbackGain so "shimmer at full
    // strength" is the out-of-the-box behavior, same convention as the
    // other defaults above.
    static constexpr float defaultShimmerAmount = 1.0f;

    // "Shimmer Sustain" task (2026-09-06, by-ear follow-up -- see
    // setShimmerSustain()'s comment): 0.85f is a fresh, deliberately-chosen
    // default for this control's now-widened scope (both the tank
    // recirculation cap AND the audible width-injection gate below), NOT a
    // preserved bit-identical match to any prior hardcoded constant --
    // that guarantee was dropped on purpose once this parameter's scope
    // grew to also touch the always-live width path, which the OLD
    // hardcoded behavior never gated at all.
    static constexpr float defaultShimmerSustainAmount = 0.85f;

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

    // LPC-based formant-preserving correction (2026-08-23, see
    // docs/formant-preserving-pitch-shifter-research.md sections 8-9),
    // replacing SpectralTiltCompensator -- that cheap one-pole fallback was
    // proven structurally unable to fix the "chipmunk on high notes"
    // complaint (the frequency band carrying the artifact and the wanted
    // shimmer effect are the same band on high notes, so no static filter
    // could separate them -- see SpectralTiltCompensator.h's own updated
    // class comment). Only ONE instance: the quadrature path shares its
    // coefficients via processQuadratureSample() rather than running a
    // second independent analysis (both grain pools read the identical
    // shared delay line -- see PitchShifter.h's class comment) -- see
    // FormantEnvelopeCorrector.h's own comment for the full rationale.
    FormantEnvelopeCorrector formantCorrector;

    // Phase 9 Freeze follow-up (2026-08-22, see FreezeLeveler.h): feed-forward
    // output-stage compensation for Freeze's measured amplitude decay --
    // scales the already-computed wetLeft/wetRight, never anything inside
    // the recirculating loop.
    FreezeLeveler freezeLeveler;

    // Phase 10 (see setLoopFreezeAmount()/LoopCapture.h): additive, fully
    // independent of every DSP member above -- captures and repeats a static
    // loop of whatever freezeLeveler's output currently is. Its rolling-
    // history buffer is written unconditionally every sample regardless of
    // loopFreezeAmount, same "always running, only gated at the read/blend
    // stage" convention as freezeLeveler itself.
    LoopCapture loopCapture;

    float shimmerWidthGain = defaultShimmerWidthGain;

    // Local copy of the value forwarded to DattorroTank::setShimmerFeedbackGain()
    // -- see setShimmerAmount()'s comment for why process()'s Width term
    // needs its own gating copy rather than trusting the tank alone.
    float shimmerAmount = defaultShimmerAmount;

    // Local copy of setShimmerSustain()'s USER-FACING (not yet inverted)
    // value -- same "process()'s Width term needs its own gating copy"
    // reasoning as shimmerAmount immediately above, see setShimmerSustain()'s
    // comment for the full rationale (2026-09-06 widening: this now ALSO
    // gates the width-injection term, not just DattorroTank's recirculation
    // cap).
    float shimmerSustainAmount = defaultShimmerSustainAmount;

    // Phase 5: dry/wet mix (see setMix()) and bypass (see setBypassed()).
    // bypassed does not gate/skip any processing -- it only forces the
    // EFFECTIVE mix used in process()'s blend to 0.0f, overriding mix,
    // so the tank's tail keeps ringing out naturally underneath instead of
    // being hard-cut.
    float mix = defaultMix;
    bool bypassed = defaultBypassed;

    // Phase 9 Freeze (see setFreezeAmount()): default 0.0f so an untouched
    // engine is bit-identical to pre-Phase-9 behavior. process() scales the
    // fresh dry input by (1.0f - freezeAmount) before it reaches the tank
    // -- at 1.0f no NEW dry energy enters the diffuser at all, leaving only
    // whatever's already recirculating (now sustained near-losslessly via
    // DattorroTank::setFreezeAmount()'s effectiveDecayGain).
    float freezeAmount = 0.0f;

    // Phase 9 Freeze follow-up (see setFreezeAmount()'s comment): the
    // user/APVTS-driven pitch shift target, stored separately from whatever
    // is actually loaded into `shifter` right now. Tried gating
    // DattorroTank's shimmerWeight/plainWeight crossfade by freezeAmount
    // first (stop feeding shifted content into the loop at all once frozen)
    // -- that broke DattorroTankTests' 3-minute frozen-boundedness test
    // (peak grew past the 10.0 safety bound within ~1 minute): Phase 8's
    // 0.85/0.15 shimmerWeight/plainWeight split isn't just an arbitrary
    // blend, it's load-bearing for stability at frozenDecayGain's near-unity
    // 0.999 -- forcing plainWeight to 1.0 (100% of the loop riding on the
    // tank's own unshifted feedbackFromB alone) let the tank's two-branch
    // cross-feed network's own resonant gain exceed unity at that decay,
    // something the pre-existing 0.15 floor was accidentally masking. This
    // member drives a DIFFERENT mechanism instead, left at the
    // already-validated shimmerWeight math: the shifter's own ratio
    // crossfades toward 1.0 (0 semitones, i.e. a plain delay tap) as
    // freezeAmount goes 0 -> 1, so the loop keeps its proven-stable
    // 0.85/0.15 blend but the shimmer-injected side of that blend stops
    // cascading the pitch further with every recirculation -- this is what
    // actually stops the frozen drone from endlessly climbing/falling in
    // pitch, without touching Phase 8's stability-critical weight split.
    float pitchShiftSemitones = defaultPitchShiftSemitones;

    // Mono scratch buffer, pre-sized in prepare() so process() never
    // allocates.
    juce::AudioBuffer<float> monoScratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShimmerReverbEngine)
};
