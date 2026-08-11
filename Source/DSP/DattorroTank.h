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

    // Tunable points, exposed as plain setters for empirical tuning by ear.
    // Phase 5 wires these up to real APVTS parameters; this class owns no
    // parameter knowledge itself.
    void setDamping (float newDampingCoefficient);
    void setDecay (float newDecayGain);

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

    // Bumped from 0.5f to 0.7f, then walked back down to 0.6f: a live
    // listening pass after the 0.7f bump reported still hearing distinct
    // spaced-out echoes -- 0.7f means the ~725ms full cross-feed round trip
    // needs ~10 repeats to decay past audibility, i.e. a long, clearly
    // countable train of discrete repeats, which made the discreteness
    // *worse*, not better. 0.6f (still inside the architecture doc's
    // 0.6-0.85 tuned range) roughly halves how many of those repeats stay
    // audible; the real fix for smoothing out any one repeat is
    // branchOutputTaps' density below, not this constant.
    static constexpr float defaultDecayGain = 0.6f;

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

    // Figure-eight cross-feed: branch B's output from the previous sample,
    // fed back into branch A's input this sample.
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
