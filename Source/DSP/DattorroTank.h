#pragma once

#include <JuceHeader.h>
#include <array>

// Phase 2 committed topology: Dattorro (1997) plate reverb tank. Input
// diffuser (4x series hand-rolled Schroeder allpass) feeding two
// cross-feeding tank branches with damping, per the corrected
// shimmer-reverb-architecture.md (the diffusion/tank allpass row is a
// hand-rolled delay-line Schroeder allpass, NOT IIR::Filter::makeAllPass() —
// that class is a frequency-domain biquad with no meaningful time delay and
// cannot produce Dattorro's diffusion). Owns its own DelayLine<Lagrange3rd>
// instances; damping is a plain one-pole leaky integrator (see
// defaultDampingCoefficient below), not an IIR::Filter. Exposes tunable
// points as plain setters, not parameters itself — parameter routing lives
// in Phase 5 (see docs/shimmer-reverb-implementation-plan.md).
//
// Mono-only, like Phase 1's ScratchSchroederTank: true stereo decorrelation
// is deferred to Phase 4. Delay lengths below are Dattorro's published
// values converted from the paper's reference sample rate of 29761 Hz to
// milliseconds (ms = samples / 29761 * 1000); prepare() converts ms to
// actual samples at the live sample rate, same as ScratchSchroederTank.
//
// The tank's own figure-eight recirculating signal (feedbackFromB) always
// contributes to the cross-feed sum (see processSample()), but the
// two-argument overload lets an externally-supplied signal (by default
// 0.0f, via the single-arg overload below) claim a capped SHARE of that
// same fixed recirculation budget instead of adding an independent gain on
// top (see maxShimmerBlendWeight's comment for why an independent additive
// gain was a real stability bug -- combined loop gain could exceed the
// validated-safe decayGain <= 0.85 ceiling). This crossfade is what lets
// ShimmerReverbEngine blend a pitch-shifted cascade into the tank's own
// sustain without ever pushing the total feedback gain past what Phase 4
// already validated as stable -- see
// docs/shimmer-reverb-implementation-plan.md's Phase 8 structural-fix note
// (2026-08-18) for the full history (an initial pure-additive attempt caused
// "oscillating, unnatural, psychedelic" artifacts from the combined loop
// gain exceeding unity).
class DattorroTank
{
public:
    DattorroTank();
    ~DattorroTank();

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();

    // Mono tank: if given a stereo (or multi-channel) block, the input
    // channels are averaged to mono internally and the same mono result is
    // written back to every output channel.
    void process (juce::dsp::AudioBlock<float>& block);

    // Per-sample form of the same tank math as process(), for callers (e.g.
    // ShimmerReverbEngine) that need to interleave this tank with another
    // per-sample process (a pitch shifter in the feedback loop) rather than
    // run it as a whole-block operation. Takes one mono input sample and an
    // externally-supplied signal that claims a capped SHARE of the tank's
    // fixed recirculation budget (see maxShimmerBlendWeight and
    // setShimmerFeedbackGain()), crossfaded against the tank's own natural
    // feedbackFromB -- NOT added independently on top of it. Passing the
    // tank's own shifted feedback here (e.g.
    // shifter.processSample(peekFeedbackSignal())) is what lets
    // ShimmerReverbEngine blend a pitch-shifted cascade into the tank's own
    // sustain while keeping the combined total feedback gain bounded by
    // decayGain alone -- see
    // docs/shimmer-reverb-implementation-plan.md's Phase 8 structural-fix
    // note (2026-08-18).
    float processSample (float input, float externalFeedback);

    // Convenience overload for callers that want no external injection at
    // all (e.g. process() below, or DattorroTankTests.cpp): 0.0f means the
    // externalFeedback term above contributes nothing, leaving only the
    // tank's own natural feedbackFromB recirculation -- identical behavior
    // to the plain (no-shimmer) tank, since shimmerFeedbackGain * 0.0f is
    // always exactly zero regardless of its value.
    float processSample (float input) { return processSample (input, 0.0f); }

    // Tunable points, exposed as plain setters for empirical tuning by ear.
    // Phase 5 wires these up to real APVTS parameters; this class owns no
    // parameter knowledge itself.
    void setDamping (float newDampingCoefficient);
    void setDecay (float newDecayGain);

    // Crossfade weight (0..1) controlling how much of the tank's fixed
    // recirculation budget (see maxShimmerBlendWeight and processSample())
    // goes to the externally-supplied signal vs. the tank's own natural
    // feedbackFromB -- NOT an independent additive gain (see
    // maxShimmerBlendWeight's comment for why that was a real bug). The
    // COMBINED total feedback gain reaching the tank stays bounded by
    // decayGain alone at every setting. Not clamped here (same convention
    // as setDecay()/setDamping() -- the APVTS parameter range is the source
    // of truth); ShimmerReverbEngine::setShimmerAmount() clamps to [0, 1]
    // before calling this.
    void setShimmerFeedbackGain (float newShimmerFeedbackGain);

    // "Shimmer Sustain" task: runtime setter for maxShimmerBlendWeight (see
    // that member's comment above and processSample()'s
    // effectiveMaxShimmerBlendWeight) -- how long the shimmer layer itself
    // sustains, independent of decayGain (Feedback) and shimmerFeedbackGain
    // (Shimmer Amount). Not clamped here, same convention as setDecay()/
    // setDamping()/setShimmerFeedbackGain()/setFreezeAmount() -- the APVTS
    // parameter range is the source of truth; ShimmerReverbEngine::
    // setShimmerSustain() clamps to [0, 1] before calling this.
    void setMaxShimmerBlendWeight (float newMaxShimmerBlendWeight);

    // Phase 9 (see docs/shimmer-reverb-implementation-plan.md): 0..1 crossfade
    // between the live, APVTS-driven decayGain (0.0f) and frozenDecayGain
    // (1.0f, near-unity), computed per-sample as effectiveDecayGain inside
    // processSample() -- see that method and frozenDecayGain's comment
    // below. Arrives pre-smoothed from the caller (PluginProcessor's
    // smoothedFreeze), same contract as every other tunable here, so no
    // additional smoothing is needed inside this class. Muting the fresh
    // dry input while frozen is ShimmerReverbEngine's job (it scales the
    // input sample before this class ever sees it); this setter only owns
    // the decay-pinning half of the freeze mechanism.
    void setFreezeAmount (float newFreezeAmount);

    // Read-only, non-destructive peek at the tank's own recirculating
    // signal (branch B's output from the last processSample() call) without
    // consuming or mutating anything. Callable any time after
    // processSample(); lets ShimmerReverbEngine pitch-shift the tank's own
    // sustain signal before feeding it back in via the two-argument
    // processSample() overload above, instead of running a separate
    // parallel feedback path (see docs/shimmer-reverb-implementation-plan.md
    // Phase 3/4 correction note).
    float peekFeedbackSignal() const noexcept { return feedbackFromB; }

private:
    // Lagrange3rd (not Linear, unlike Phase 1's scratch tank) is required
    // here: the architecture doc calls out that this interpolation choice
    // must already match what Phase 3's pitch shifter needs (modulation-safe,
    // low coloration), so it's chosen now rather than switched later.
    using DelayLineType = juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd>;

    // Same hand-rolled Schroeder allpass structure as
    // ScratchSchroederTank::processAllpass, but with a per-stage feedback
    // gain field since the diffuser and tank stages here each use different
    // gains (rather than one shared constant).
    struct AllpassStage
    {
        DelayLineType delayLine;
        float feedback = 0.0f;
    };

    // One cross-feeding tank branch: allpass -> delay -> damping -> allpass -> delay.
    struct TankBranch
    {
        AllpassStage allpass1;
        DelayLineType delay1;
        float dampState = 0.0f;
        AllpassStage allpass2;
        DelayLineType delay2;
    };

    float processAllpass (float input, AllpassStage& stage);
    float processDelay (float input, DelayLineType& delay);
    float processBranch (float input, TankBranch& branch);

    // Non-consuming "peek" read at an absolute sample offset within a delay
    // line's own buffer, used by the output tap formula below. IMPORTANT:
    // DelayLineType::popSample's updateReadPointer=false only skips
    // advancing the line's *read pointer* -- passing an explicit
    // delayInSamples argument still calls setDelay() internally (see
    // juce_DelayLine.cpp), which overwrites the line's *configured* delay
    // used by every subsequent default-argument popSample() call, i.e. the
    // recirculating read in processDelay(). If peekTap() didn't restore the
    // original configured delay afterwards, it would silently corrupt the
    // tank's recirculating path on the very next sample -- exactly the kind
    // of regression this additive feature must not reintroduce.
    float peekTap (DelayLineType& delay, float offsetSamples);

    void prepareBranch (TankBranch& branch, const juce::dsp::ProcessSpec& monoSpec,
                         float allpass1Ms, float delay1Ms, float allpass2Ms, float delay2Ms);
    void resetBranch (TankBranch& branch);

    //==============================================================================
    // Input diffuser: 4 series allpass stages, mono, applied once before the
    // signal splits into the two tank branches.
    static constexpr int numDiffuserStages = 4;
    static constexpr std::array<float, numDiffuserStages> diffuserDelayMs { 4.7714f, 3.5947f, 12.7345f, 9.3081f };
    static constexpr std::array<float, numDiffuserStages> diffuserFeedback { 0.75f, 0.75f, 0.625f, 0.625f };

    // Tank branch A/B delay-length tables (ms), converted from Dattorro's
    // (1997) published sample lengths at the paper's 29761 Hz reference rate.
    static constexpr float branchAAllpass1Ms = 22.5798f;
    static constexpr float branchADelay1Ms   = 149.6259f;
    static constexpr float branchAAllpass2Ms = 60.4879f;
    static constexpr float branchADelay2Ms   = 125.0008f;

    static constexpr float branchBAllpass1Ms = 30.5090f;
    static constexpr float branchBDelay1Ms   = 141.6752f;
    static constexpr float branchBAllpass2Ms = 89.2451f;
    static constexpr float branchBDelay2Ms   = 106.2837f;

    static constexpr float branchAllpass1Feedback = -0.7f;
    static constexpr float branchAllpass2Feedback = 0.5f;

    // Starting points, not final tuning (Phase 2 hasn't done empirical
    // tuning yet) — kept as named constants and exposed via setters above.
    //
    // Corrected from an earlier IIR::Filter::makeFirstOrderLowPass(8000Hz)
    // implementation, which was the wrong primitive entirely: Dattorro's
    // actual damping stage (verified against a reference port whose delay
    // lengths/coefficients match this class exactly -- see
    // louiscouka.com/code/datorro-reverb-implementation) is a plain one-pole
    // leaky integrator applied directly to the delay line's samples,
    // y[k] = damping*y[k-1] + (1-damping)*x[k], with damping ~= 0.0005 -- an
    // extremely light amount of smoothing (the recursive/previous-sample
    // term barely contributes). An 8000Hz IIR::Filter cutoff at 44.1kHz
    // corresponds to a leaky-integrator coefficient of roughly 0.32 -- over
    // 600x more aggressive than the reference value -- which was stripping
    // far more high-frequency content on every pass through the tank than
    // intended, making each recirculation sound like a duller, more
    // discrete "thud" instead of a bright, continuous wash (reported as
    // "sounds like a delay, not a reverb").
    static constexpr float defaultDampingCoefficient = 0.0005f;

    // Bumped from 0.5f to 0.7f, then walked back down to 0.6f during Phase 2
    // (plain, unshifted tank): a live listening pass after the 0.7f bump
    // reported still hearing distinct spaced-out echoes -- 0.7f means the
    // ~725ms full cross-feed round trip needs ~10 repeats to decay past
    // audibility, i.e. a long, clearly countable train of discrete repeats,
    // which made the discreteness *worse*, not better at the time.
    //
    // Re-raised during Phase 3/4's structural fix (see
    // docs/shimmer-reverb-implementation-plan.md): now that the pitch
    // shifter sits inside this exact recirculation path (ShimmerReverbEngine
    // shifts peekFeedbackSignal() before it re-enters via the two-argument
    // processSample() overload), the Phase 2 concern above doesn't transfer
    // directly -- each of those countable repeats is now progressively
    // pitched up rather than an identical-pitch echo, so more distinct
    // repeats surviving longer is closer to the intended ascending-shimmer
    // character than to Phase 2's "sounds like a delay" complaint.
    //
    // 0.8f was tried first and rejected by ShimmerReverbEngineTests' decay-
    // to-silence test: peak in the final second of an 8s silence window was
    // 0.00245, over the 1e-3 safety margin -- not unbounded runaway (the
    // separate 30s+ boundedness test still passed), but the tail genuinely
    // outlasts that test's window, i.e. real margin was gone. Settled on
    // 0.7f: still inside the architecture doc's documented 0.6-0.85 tuned
    // range, meaningfully higher than Phase 2/3's 0.6f, and re-verified
    // stable (bounded + decays to silence with real margin) with the
    // shifter in the loop via ShimmerReverbEngineTests before shipping.
    static constexpr float defaultDecayGain = 0.7f;

    // Default for the new independent shimmer-injection gain (see
    // setShimmerFeedbackGain()). 1.0f matches
    // ShimmerReverbEngine::defaultShimmerAmount's own default so "shimmer at
    // full strength" is the out-of-the-box behavior, same convention as
    // decayGain/dampingCoefficient's defaults mirroring their APVTS
    // parameter defaults.
    static constexpr float defaultShimmerFeedbackGain = 1.0f;

    // Structural-fix correction (docs/shimmer-reverb-implementation-plan.md's
    // Phase 8 note, 2026-08-18, second pass): caps how much of the tank's
    // recirculating budget the shimmer path can ever claim, so the COMBINED
    // total feedback gain reaching the tank (see processSample()) stays
    // bounded by decayGain alone -- the same ceiling Phase 4 already
    // validated safe (decayGain <= 0.85) -- regardless of shimmerFeedbackGain's
    // value. The first attempt at this fix (now corrected) let decayGain and
    // shimmerFeedbackGain add independently, so the EFFECTIVE combined loop
    // gain could reach decayGain + shimmerFeedbackGain (e.g. ~1.7 at
    // defaults) -- well past the validated-safe ceiling, kept only
    // technically bounded by SafetyLimiter's hard clipping, which is exactly
    // what produced the reported "oscillating, unnatural, psychedelic"
    // character (a feedback loop with gain > 1 constantly getting caught and
    // clipped, not a smooth reverb tail).
    //
    // Raised 0.5f -> 0.85f, 2026-08-21, after an ear pass with the 0.5f cap
    // (plus the by-then-fixed Width leak, see ShimmerReverbEngine.cpp) found
    // the shimmer character sustained noticeably shorter than commercial
    // shimmer reverbs even with Feedback/Shimmer Amount/Mix all at their
    // maximum. The crossfade's gain-safety property (plainWeight +
    // shimmerWeight == 1.0 exactly, always) holds for ANY value up to 1.0,
    // not just 0.5 -- the cap's only real job is reserving *some* budget for
    // the plain path so full geometric pitch-compounding ("chipmunk") can't
    // completely take over at shimmerAmount's maximum, not gain safety
    // itself. 0.85f mirrors decayGain's own validated-safe ceiling
    // (Phase 4's decayGain <= 0.85f) as a reasoned upper bound, still leaving
    // 15% of the budget on the plain path at shimmerAmount's maximum. Not
    // exhaustively ear-tuned beyond this one pass; may need revisiting.
    //
    // "Shimmer Sustain" task: promoted from a compile-time constant to a
    // plain, runtime-adjustable instance member (see setMaxShimmerBlendWeight()
    // below) so the user can directly control how long the shimmer layer
    // itself sustains, independent of decayGain (Feedback) and
    // shimmerFeedbackGain (Shimmer Amount). The safety argument above --
    // shimmerWeight + plainWeight == 1.0 exactly, for ANY value in [0, 1] --
    // holds unchanged regardless of who sets this value or when.
    float maxShimmerBlendWeight = 0.85f;

    // Freeze-time shimmerWeight cap (2026-08-22): crossfades
    // maxShimmerBlendWeight's effective value from 0.85f (freezeAmount=0.0f,
    // bit-identical to the pre-existing formula) toward this smaller target
    // as freezeAmount goes 0->1 -- see processSample()'s
    // effectiveMaxShimmerBlendWeight. NOT the same fix as the first attempt
    // at this (reverted -- see git history / Engram topic
    // architecture/millenia-freeze), which crossfaded the cap all the way to
    // 0.0f (plainWeight->1.0 at full freeze) and broke this file's own
    // 3-minute freeze-boundedness test: that plainWeight=1.0 configuration
    // is provably unstable at frozenDecayGain's near-unity decay (peak
    // exceeded the 10.0 safety bound within ~1 minute), because the
    // shimmerWeight/plainWeight blend turns out to double as what keeps the
    // tank's own two-branch cross-feed resonance detuned/bounded at that
    // decay, not just as the shimmer-character knob. A separate diagnostic
    // (ShimmerReverbEngineTests.cpp's "Freeze = 1.0's actual sustained tail")
    // found the ORIGINAL 0.85f cap causes a different, also-real problem at
    // full freeze: comb filtering between the tank's direct feedbackFromB and
    // the separately-delayed (via PitchShifter's own ~80ms internal delay
    // line) shimmer-path copy measurably bleeds energy every pass, which is
    // invisible at ordinary (<=0.85) decayGain (masked by its own much
    // larger attenuation) but becomes the dominant, audible ("still finishes
    // soon, oscillates, sometimes sounds like a motor") decay mechanism at
    // Freeze's near-unity decay.
    //
    // Swept against both this file's 3-minute freeze-boundedness test AND
    // ShimmerReverbEngineTests.cpp's 15s decay diagnostic (2026-08-22), one
    // value at a time, decay measured in dB over that diagnostic's 15s
    // window (baseline at the unmodified 0.85f cap: -6.7dB):
    //   0.6f  -> -7.69dB (WORSE than baseline -- stable)
    //   0.4f  -> -7.74dB (WORSE than baseline -- stable)
    //   0.2f  -> -7.45dB (worse than baseline -- stable)
    //   0.05f -> -5.19dB (better than baseline -- stable)
    //   0.02f -> -3.11dB (much better -- stable)
    //   0.01f  -> -1.94dB (much better -- stable over the committed 3-minute
    //           test AND a 10-MINUTE extended run)
    //   0.007f -> -1.53dB (marginally better than 0.01f on the SHORT 3-minute
    //           test -- but FAILS the 10-minute extended run: 40055 assertion
    //           failures. This is the important finding: the instability
    //           cliff is not a single sharp point catchable by any fixed-
    //           duration test -- there is a real band (roughly 0.007f-0.01f)
    //           where a SHORT test gives false confidence and only a longer
    //           run reveals the slow-building runaway. Do not trust a
    //           candidate here without re-running at least a 10-minute
    //           boundedness check, not just the committed 3-minute one.
    //   0.005f -> UNSTABLE even on the short test: 8045 assertion failures,
    //           peak blew past the 10.0 safety bound almost immediately.
    // Also tried and found NOT to help (2026-08-22, second investigation):
    // replacing the shifter's grain machinery with a plain fixed-delay tap at
    // ratio==1.0 (sidesteps grain-hop/alignment-search artifacts entirely) --
    // tested at short (0.2-5ms), matched (80ms, i.e. baseDelaySamples), and
    // long (110ms) tap lengths, both alone (shimmerWeight restored to 0.85f)
    // and combined with the 0.01f weight fix above. Short taps made decay
    // WORSE (-9.9dB, worse than doing nothing); matched/long taps reproduced
    // the same numbers as the normal grain machinery (~-6.7dB alone, ~-1.94dB
    // combined with 0.01f) -- i.e. no measurable effect either way. The
    // decay/oscillation is NOT caused by the grain mechanism specifically,
    // only by shimmerWeight itself (see the sweep above) -- this rules out
    // "just bypass the grains" as a free additional win.
    // The weight/decay relationship is NOT monotonic -- decay gets WORSE than
    // the 0.85f baseline through the 0.2f-0.6f range before improving sharply
    // below ~0.05f, consistent with genuine comb-filtering interference
    // (whose depth depends on the specific phase relationship between the two
    // summed paths at a given weight, not simply "more weight = more loss")
    // rather than a simple monotonic gain trade-off. 0.01f is chosen as the
    // most aggressive value verified safe over a 10-minute run, not just a
    // convenient round number -- see the 0.007f row above for why shorter
    // verification would have been misleading here.
    static constexpr float frozenMaxShimmerBlendWeight = 0.01f; // see comment above for the swept numbers behind this value

    // Phase 9 Freeze (see docs/shimmer-reverb-implementation-plan.md): the
    // near-unity decayGain target while Freeze is engaged, per the plan's
    // committed mechanism (a) -- "pin decayGain to unity (or just under,
    // e.g. 0.999f, to sidestep an exact-1.0 edge case)". Deliberately NOT
    // 1.0f: at exactly 1.0 the tank's cross-feed sum (see processSample())
    // would recirculate its own past output with zero loss on every single
    // pass, which is an idealized-only case that real floating-point
    // accumulation (denormal-adjacent tiny errors, the diffuser's own
    // allpass gain, damping's leaky-integrator rounding) can push either
    // side of -- 0.999f keeps the loop provably strictly-decaying-toward-
    // its-own-past-energy in the mathematical sense (a genuine, if
    // extremely slow, contraction) while still reading as "indefinitely
    // sustained" on any human timescale (each recirculation loses only
    // 0.1% of its prior amplitude, i.e. a -60dB decay would take thousands
    // of tank round-trips -- see DattorroTankTests.cpp's freeze-specific
    // stability test for the actual measured numbers).
    static constexpr float frozenDecayGain = 0.999f;

    // Real Dattorro (1997) output tap formula -- replaces an earlier
    // ad-hoc scheme (a dominant 0.5f*(tankA_out+tankB_out) "main path" plus
    // small extra peeks) that still let the two full-branch-length taps
    // dominate the output, which is exactly why it kept sounding like
    // discrete echoes no matter how much decay/damping/extra-tap tuning was
    // applied. This is the paper's actual mono-equivalent output mix
    // (adapted from its published left-channel formula; the right-channel
    // formula is a mirrored tap set not needed until Phase 4's stereo
    // work), verified against a reference port whose delay lengths and
    // diffusion/decay coefficients already match this class exactly (see
    // louiscouka.com/code/datorro-reverb-implementation): seven taps of
    // roughly equal weight with alternating signs, summed and scaled by
    // outputScale -- no single tap dominates, unlike the old scheme.
    // Offsets converted from the reference's native sample counts to
    // milliseconds (same 29761 Hz convention as every other delay length in
    // this class) so they scale correctly at any runtime sample rate.
    static constexpr float outputTapBDelay1aMs  = 8.9366f;   // dl[5] tap +266
    static constexpr float outputTapBDelay1bMs  = 99.9295f;  // dl[5] tap +2974
    static constexpr float outputTapBAllpass2Ms = 64.2743f;  // dl[6] tap +1913, negative sign
    static constexpr float outputTapBDelay2Ms   = 67.0662f;  // dl[7] tap +1996
    static constexpr float outputTapADelay1Ms   = 66.8646f;  // dl[1] tap +1990, negative sign
    static constexpr float outputTapAAllpass2Ms = 6.2827f;   // dl[2] tap +187, negative sign
    static constexpr float outputTapADelay2Ms   = 35.8180f;  // dl[3] tap +1066, negative sign
    static constexpr float outputScale = 0.6f;

    std::array<AllpassStage, numDiffuserStages> diffuserStages;
    TankBranch branchA, branchB;

    float dampingCoefficient = defaultDampingCoefficient;
    float decayGain = defaultDecayGain;
    float shimmerFeedbackGain = defaultShimmerFeedbackGain;

    // Phase 9 Freeze crossfade weight (see setFreezeAmount() and
    // frozenDecayGain above); 0.0f default means processSample()'s
    // effectiveDecayGain equals the live decayGain exactly, i.e. no
    // behavior change until a caller actually engages Freeze.
    float freezeAmount = 0.0f;

    // Figure-eight cross-feed: branch B's output from the previous sample,
    // fed back into branch A's input this sample by default (via the
    // single-arg processSample() overload above). Also readable externally
    // via peekFeedbackSignal() so ShimmerReverbEngine can pitch-shift this
    // exact signal before it's fed back in through the two-argument
    // processSample() overload, instead of running a separate, weaker
    // parallel feedback path (see
    // docs/shimmer-reverb-implementation-plan.md's Phase 3/4 correction
    // note).
    float feedbackFromB = 0.0f;

    // Output tap offsets converted to samples at the live sample rate,
    // computed once in prepare() rather than every sample.
    float outputTapBDelay1aSamples = 0.0f;
    float outputTapBDelay1bSamples = 0.0f;
    float outputTapBAllpass2Samples = 0.0f;
    float outputTapBDelay2Samples = 0.0f;
    float outputTapADelay1Samples = 0.0f;
    float outputTapAAllpass2Samples = 0.0f;
    float outputTapADelay2Samples = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DattorroTank)
};
