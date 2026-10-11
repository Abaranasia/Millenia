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

    // Channel contract identical to DattorroTank::process(): a possibly
    // multi-channel input block is averaged to mono internally, and the
    // tank's own L/R tap-set outputs (not the shifter's) are written back,
    // L to channel 0, R to the rest -- per shimmer-reverb-concepts.md, what's
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
    // Their difference (sideShift) is added to the wet side channel with
    // shimmerSideGain (see setWidth()), the primary pair toward L and the
    // quadrature pair toward R -- decorrelated because L/R draw from
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

    // M/S width of the wet signal, clamped to [0, 2] (2026-10-11, docs/
    // reverb-space-research.md section 8.2): 0 = mono, 1 = the tank's
    // natural Dattorro stereo (default), 2 = side doubled. Scales the
    // tank's own L/R difference AND the shimmer side term together. Used to
    // be a 0..1 gain on the shimmer side term alone (default 0.15), which
    // could not widen the then-mono tail itself.
    void setWidth (float newWidth);

    // Pre-delay before the tank, clamped to [0, maxPreDelayMs]. Smoothed per
    // sample inside process() (preDelayRampSeconds), so callers forward the
    // raw parameter value. 0 (default) is bit-identical to no pre-delay.
    void setPreDelayMs (float newPreDelayMs);

    // Wet EQ on the final wet signal (after loop capture, before the dry/wet
    // mix), 2nd-order TPT highpass/lowpass per channel. Clamped to
    // [minLowCutHz, maxLowCutHz] / [minHighCutHz, maxHighCutHz]; each filter
    // is skipped entirely at its open endpoint (20 Hz / 20 kHz), so the
    // defaults are bit-identical to no EQ. Applied once per block.
    void setLowCutHz (float newLowCutHz);
    void setHighCutHz (float newHighCutHz);

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
    // (mute new input + pin decayGain near unity + level-crossfade the
    // in-loop shifter's output toward a 0-semitone unityShifter, see
    // pitchShiftSemitones' comment below), and the tank only ever owns the
    // second of those.
    void setFreezeAmount (float amount);

    // Infinite mode (see docs/shimmer-reverb-implementation-plan.md's
    // "Infinite mode" section and DattorroTank::setInfiniteAmount()):
    // clamped to [0, 1], cached, and forwarded to the tank, which owns the
    // whole mechanism (sample-accurate ramp, decay pin to 1.0, allpass
    // modulation, in-loop level controller). Deliberately does NOT touch the
    // input: Infinite's point is that fresh input keeps layering on top of
    // a tail that never decays, the opposite of Freeze's input mute. If both
    // are engaged, Freeze's input mute (process() scales the tank input by
    // 1 - freezeAmount) still applies unchanged -- the result is a frozen
    // drone held at exactly unity decay. That interaction is intentionally
    // left alone (no special-case code); ShimmerReverbEngineTests.cpp's
    // "Infinite + Freeze both on" test covers its boundedness.
    void setInfiniteAmount (float newAmount);

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

    // Test-only introspection, same idiom (2026-10-10, unity-shifter idle
    // skip): the raw, UNWEIGHTED unityShifter primary output of the last
    // processed sample (0.0f while the unity path is idle). Lets
    // ShimmerReverbEngineFreezeCrossfadeTests' ghost-audio test see exactly
    // what that path would contribute at full unityWeight.
    float peekUnityShifterOutput() const noexcept { return lastUnityFeedback; }

    // Test-only switch (2026-10-10): true runs unityShifter on every sample
    // regardless of Freeze -- the pre-idle-skip, always-on behavior -- so a
    // test can compare the idle skip against it in the same build (CPU cost,
    // ghost audio, bit-identity at Freeze 0). Never called by
    // PluginProcessor; default false.
    void forceUnityShifterActiveForTesting (bool shouldForce) noexcept { unityShifterForcedActive = shouldForce; }

    // Test-only introspection, same idiom (2026-10-11, stereo tank): the
    // last processed sample's raw tank left/right outputs, before Width.
    // With the tank now stereo, |L - R| at the output also carries the
    // tank's own side, so a test subtracts width * (tankL - tankR) from the
    // real output's L - R to isolate the shimmer side term as heard.
    float peekTankLeft() const noexcept { return lastTankLeft; }
    float peekTankRight() const noexcept { return lastTankRight; }

    // Test-only: the private shimmerSideGain, so a test can divide the
    // shimmer side term's known scale back out of an output measurement.
    static constexpr float getShimmerSideGainForTesting() noexcept { return shimmerSideGain; }

private:
    // Applies pitchShiftSemitones to the main shifter (always the full user
    // value) and formantCorrector, and recomputes unityWeightTarget from
    // freezeAmount -- shared by prepare(), setPitchShiftSemitones(), and
    // setFreezeAmount() so the crossfade curve (see freezeUnityCrossfadeCurve
    // below) only lives in one place. Was updateShifterRatio() until
    // 2026-10-10, when it still set a freeze-crossfaded ratio on the main
    // shifter; renamed once that ratio crossfade was replaced by the level
    // crossfade, since it no longer sets any crossfaded ratio.
    void applyPitchAndFreezeCrossfade();

    // Phase 9 Freeze LEVEL crossfade curve: unityWeight = freezeAmount^16 is
    // how much of the in-loop shimmer feedback comes from unityShifter (0
    // semitones) instead of the main shifter (full user shift). process()
    // blends LINEARLY (see its comment for why not equal-power). 16.0 puts
    // nearly all of the fade into the last ~10% of the dial's travel
    // (weight 0.01 at f=0.75, 0.19 at f=0.9, 0.44 at f=0.95), the same feel
    // the old ratio crossfade had, and reaches exactly 1.0 at f=1.0 (the
    // anti-cascade guarantee) and exactly 0.0 at f=0.0 (bit-identical
    // default sound).
    //
    // History. This exponent used to shape a RATIO crossfade on a single
    // shifter (semitones * (1 - f^curve)). 2026-08-22: the first curve was a
    // real arithmetic bug, (1-f)^4, which dropped 94% of the shift by the
    // dial's halfway point (audible "pitch down" plus lost Width); fixed to
    // 1 - f^4. 2026-08-29: steepened 4 -> 16 because 4 still lost ~32% of a
    // +12st shift by f=0.75. 2026-10-10: the ratio crossfade itself was
    // replaced by this level crossfade -- the Freeze quick-toggle sweeps f
    // 0 -> 1 in 50ms, so every toggle GLIDED the in-loop pitch from +12st
    // to 0st (an audible "chipmunk / DJ-scratch" bend, baked into the
    // frozen loop), and no ramp time fixed it (0.3s and 1.0s ramps were
    // tried 2026-08-29 and sounded worse: a longer glide). Measured by
    // ShimmerReverbEngineFreezeCrossfadeTests' T1 (intermediate-pitch
    // energy around a toggle vs a no-ramp baseline, L/R): glide +16.76dB /
    // +20.97dB (128 / 512-sample blocks), level crossfade +3.20dB / +4.65dB.
    static constexpr float freezeUnityCrossfadeCurve = 16.0f;

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
    //
    // 2026-10-11: no longer the Width parameter. Width became an M/S control
    // over the now-stereo tank (see setWidth()); this old default is baked
    // into the shimmer side term instead, so at Width = 1 the shimmer side
    // contribution equals the old default exactly.
    static constexpr float shimmerSideGain = 0.15f;

    static constexpr float defaultWidth = 1.0f;
    static constexpr float maxWidth = 2.0f;

    // Pre-delay and wet EQ ranges -- must match Parameters.cpp.
    static constexpr float maxPreDelayMs = 250.0f;
    static constexpr double preDelayRampSeconds = 0.05; // same 50ms as PluginProcessor's smoothers
    static constexpr float minLowCutHz = 20.0f;
    static constexpr float maxLowCutHz = 1000.0f;
    static constexpr float minHighCutHz = 1000.0f;
    static constexpr float maxHighCutHz = 20000.0f;

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

    // Freeze level crossfade (2026-10-10, see applyPitchAndFreezeCrossfade()): a second
    // in-loop PitchShifter permanently at 0 semitones (ratio 1.0), fed the
    // same dryFeedback every sample and faded in by unityWeight as Freeze
    // engages. Gets the same setFreezeAmount() drift as `shifter`, so at full
    // freeze it has the same ratio (1.0) and drift the old single shifter
    // had there. One measured difference: having never run at another
    // ratio, its primary and quadrature grain pools sit at identical
    // offsets, so sideShift (and with it the Width term) is exactly 0 at
    // full freeze -- the old glided shifter kept a side channel ~28dB below
    // the mid from leftover grain offsets.
    // Processed only while Freeze is (or is ramping) above 0 since
    // 2026-10-10 -- see unityShifterActive below; it originally ran every
    // sample.
    PitchShifter unityShifter;

    // Unity-shifter idle skip (2026-10-10): whether unityShifter was
    // processed in the last block. process() sets it once per block, true
    // while that block's freeze ramp start or target (or the crossfade
    // weight) is above 0; while false, unityShifter is not run at all and
    // 0.0f stands in for its primary and quadrature outputs (weighted by
    // exactly 0.0f there, so Freeze 0 stays bit-identical). Measured reason:
    // running it always cost ~+40-55% engine CPU (processing/audio ~0.037 ->
    // ~0.052-0.058, Release) for a path that is silent at the default
    // setting. On the false -> true edge process() calls
    // unityShifter.reset() first: an idle shifter's delay line still holds
    // the ~80ms of loop audio from its LAST engagement, and without the
    // clear a re-engage would replay it (the ghost-audio test in
    // ShimmerReverbEngineFreezeCrossfadeTests). The accepted cost is a cold
    // start -- silent for its first ~80ms -- harmless at the current blend
    // caps (see the comment above unityFeedback in process()). reset()
    // clears it to false so the next engagement starts clean.
    bool unityShifterActive = false;

    // Test-only state, see peekUnityShifterOutput()/
    // forceUnityShifterActiveForTesting().
    float lastUnityFeedback = 0.0f;
    bool unityShifterForcedActive = false;

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

    float width = defaultWidth;

    // Pre-delay (see setPreDelayMs()): mono line on the tank input, ahead of
    // Freeze's input mute, sized for maxPreDelayMs in prepare(). Linear
    // interpolation is enough: the read delay only moves while the smoother
    // glides, never as a steady modulation.
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> preDelayLine;
    juce::SmoothedValue<float> smoothedPreDelayMs;
    float preDelaySamplesPerMs = 44.1f;

    // Wet EQ (see setLowCutHz()/setHighCutHz()). *Active is the per-block
    // "not at the open endpoint" decision, and gates only which signal is
    // used: both filters run on every sample (at their endpoint cutoff while
    // inactive), so their state always tracks the wet signal and activation
    // continues from it. Resetting on activation instead was a click -- a
    // zero-state TPT lowpass steps the signal down by (1 - G) * x on its
    // first sample (see process()'s wet EQ comment for the measurement).
    juce::dsp::StateVariableTPTFilter<float> lowCutFilter, highCutFilter;
    float lowCutHz = minLowCutHz;
    float highCutHz = maxHighCutHz;
    float maxFilterCutoffHz = maxHighCutHz;
    bool lowCutActive = false;
    bool highCutActive = false;

    // Test-only state, see peekTankLeft()/peekTankRight().
    float lastTankLeft = 0.0f;
    float lastTankRight = 0.0f;

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

    // Infinite mode (see setInfiniteAmount()): default 0.0f, i.e. off and
    // bit-identical to pre-Infinite behavior. Cached so prepare() can
    // re-seed the tank, same pattern as shimmerSustainAmount.
    float infiniteAmount = 0.0f;

    // Freeze level crossfade weight (see applyPitchAndFreezeCrossfade()/
    // freezeUnityCrossfadeCurve): unityWeightTarget = freezeAmount^16, a
    // function of freezeAmount ONLY. It is merely recomputed on every
    // applyPitchAndFreezeCrossfade() call (once per block in practice),
    // including those from setPitchShiftSemitones(), because that function
    // is shared -- a pitch change alone never moves it. 0.0f = main shifter only (default,
    // bit-identical), 1.0f = unityShifter only (full freeze, no cascade).
    // unityWeight is the value actually applied at the end of the last
    // processed block; process() ramps linearly from it to the target,
    // per sample, across each block. Measured reason (2026-10-10, T1 in
    // ShimmerReverbEngineFreezeCrossfadeTests run at 512-sample blocks):
    // the 50ms toggle ramp is only ~4 blocks long there, and the ^16 curve
    // puts the weight at ~0.31 one block before the end, so a per-block step
    // jumps 0.31 -> 1.0 in one sample. Its own measured share of T1's
    // 512-block artifact was small (~0.35dB); most came from the per-block
    // freezeAmount steps processedFreezeAmount below removes. Kept because
    // a hard step between two different signals is a discontinuity by
    // construction, and the ramp is one multiply-add per sample.
    float unityWeightTarget = 0.0f;
    float unityWeight = 0.0f;

    // freezeAmount as actually applied at the end of the last processed
    // block. process() ramps the input mute (1 - freezeAmount) and
    // DattorroTank's freezeAmount linearly from this to the current
    // freezeAmount, per sample, across each block (2026-10-10). Measured
    // reason: at 512-sample blocks the 50ms toggle ramp is ~4 steps of
    // ~0.23 each; stepping the tank's effectiveDecayGain/shimmer blend cap
    // and the input gain that coarsely amplitude-modulated the tail with a
    // zipper whose splatter alone measured +15.75dB in T1 even with the
    // pitch glide gone (+3.4dB at 128-sample blocks). Per-sample ramping
    // took the 512-block figure to the same few dB as 128. At constant
    // freeze (start == target) it is bit-identical to the old per-block
    // value, so every fixed-freeze test and the default sound are
    // unchanged. Same lesson as LoopCapture's 2026-09-06 click fix:
    // caller-side per-block smoothing alone is not enough on large buffers.
    float processedFreezeAmount = 0.0f;

    // Phase 9 Freeze follow-up (see setFreezeAmount()'s comment): the
    // user/APVTS-driven pitch shift target. Since 2026-10-10 this is ALWAYS
    // what `shifter` runs at; Freeze no longer modifies it (see the end of
    // this comment). Tried gating
    // DattorroTank's shimmerWeight/plainWeight crossfade by freezeAmount
    // first (stop feeding shifted content into the loop at all once frozen)
    // -- that broke DattorroTankTests' 3-minute frozen-boundedness test
    // (peak grew past the 10.0 safety bound within ~1 minute): Phase 8's
    // 0.85/0.15 shimmerWeight/plainWeight split isn't just an arbitrary
    // blend, it's load-bearing for stability at frozenDecayGain's near-unity
    // 0.999 -- forcing plainWeight to 1.0 (100% of the loop riding on the
    // tank's own unshifted feedbackFromB alone) let the tank's two-branch
    // cross-feed network's own resonant gain exceed unity at that decay,
    // something the pre-existing 0.15 floor was accidentally masking. A
    // DIFFERENT mechanism was used instead, left at the already-validated
    // shimmerWeight math: the shimmer-injected side of that blend is moved
    // to an unshifted signal as freezeAmount goes 0 -> 1, so the loop keeps
    // its proven-stable 0.85/0.15 blend but stops cascading the pitch
    // further with every recirculation. Originally (2026-08) that was done
    // by gliding THIS shifter's ratio toward 1.0; since 2026-10-10 it is a
    // linear level crossfade from `shifter` (always at this value) to
    // `unityShifter` (always at 0 semitones), weighted by unityWeight --
    // same endpoint at full freeze, but no audible pitch glide in between
    // (see freezeUnityCrossfadeCurve's comment).
    float pitchShiftSemitones = defaultPitchShiftSemitones;

    // Mono scratch buffer, pre-sized in prepare() so process() never
    // allocates.
    juce::AudioBuffer<float> monoScratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShimmerReverbEngine)
};
