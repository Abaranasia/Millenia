#pragma once

#include <JuceHeader.h>
#include <array>

// Hand-rolled multi-voice crossfade pitch shifter (Phase 3, extended to
// 4-voices-per-group during the shimmer's "sounds detuned" investigation --
// see docs/shimmer-reverb-implementation-plan.md). One shared DelayLine is
// written once per sample; a set of independent read "voices" peek into it
// at delay offsets that are a direct function of each voice's own grain
// phase, crossfaded with a raised-cosine (Hann) window so every group's
// envelopes always sum to a CONSTANT total (see the COLA identity below and
// processSample()). Mono only -- this class only ever sees DattorroTank's
// mono output, one sample at a time from ShimmerReverbEngine's feedback
// loop. Owns no parameter knowledge itself; Phase 5 wires
// setPitchShiftSemitones() to a real APVTS parameter.
//
// ---------------------------------------------------------------------------
// Root cause this 4-voice change fixes, and why the earlier grain-length
// tweak only partially helped
// ---------------------------------------------------------------------------
// The shimmer was reported as "sounds slightly detuned." That was traced to
// a real, derivable comb-filter artifact in the ORIGINAL 2-voices-per-group
// design (voiceA/voiceB, and separately voiceC/voiceD): with only two voices
// per group spaced a half-grain apart, each voice's read position in the
// shared delayLine stays a CONSTANT time-offset from its partner --
// lag = 0.5 * grainLengthMs * |pitchRatio - 1| -- and summing two copies of
// the same recirculating signal that far apart in time is a comb filter with
// audible nulls. The first attempted fix (lowering grainLengthMs from 25.0f
// to 20.0f, see that constant's own comment below) reduced the worst-case
// lag from 12.5ms to 10ms at the default +12st shift -- a real but bounded,
// modest improvement, not a structural fix: with only 2 voices, the
// *nearest-neighbor* lag can never shrink below half a grain length no
// matter how short the grain gets, and grains can't be shortened much
// further without their own repetition-rate artifact becoming audible. The
// actual fix is what this class now does: quadruple the voice count per
// group (2 -> 4), which quarters the nearest-neighbor lag between
// consecutively-phased voices for the SAME grain length, without needing to
// shorten grains any further.
//
// ---------------------------------------------------------------------------
// The general N-voice COLA (constant-overlap-add) identity
// ---------------------------------------------------------------------------
// For a raised-cosine (Hann) envelope hann(p) = 0.5*(1 - cos(2*pi*p)), the
// sum over N EQUALLY-SPACED phases p, p+1/N, p+2/N, ..., p+(N-1)/N is:
//
//   sum_{k=0}^{N-1} hann(p + k/N)
//     = 0.5*N - 0.5 * sum_{k=0}^{N-1} cos(2*pi*p + 2*pi*k/N)
//
// The cosine sum is the real part of sum_k e^{i(theta + 2*pi*k/N)} where
// theta = 2*pi*p, which factors as e^{i*theta} * sum_k e^{i*2*pi*k/N}. The
// second factor is a sum of the Nth roots of unity, which is EXACTLY ZERO
// for any N >= 2 (standard identity). So:
//
//   sum_{k=0}^{N-1} hann(p + k/N) = 0.5*N   identically, for ANY phase p and
//                                            ANY N >= 2.
//
// The original 2-voice design's crossfade identity, hann(p) + hann(p+0.5) ==
// 1, is just this identity at N=2 (0.5*2 = 1.0, hence no extra scaling was
// ever needed there). This class now uses N=4 per group, whose constant sum
// is 0.5*4 = 2.0 -- see the normalization warning below, this is NOT the
// same constant as before.
//
// ---------------------------------------------------------------------------
// Voice layout: two groups of 4, generalizing the old A/B-vs-C/D roles
// ---------------------------------------------------------------------------
// primaryVoices (same role as the old voiceA/voiceB, feeds the recirculating
// tank path): 4 voices at equally-spaced phases 0.0, 0.25, 0.5, 0.75.
//
// quadratureVoices (same role as the old voiceC/voiceD, feeds
// ShimmerReverbEngine's stereo-width path via getQuadratureOutput()): 4
// voices at phases 0.125, 0.375, 0.625, 0.875 -- offset from primaryVoices
// by 0.125, which is HALF of primaryVoices' own inter-voice spacing (0.25).
// This directly generalizes how the old quadrature offset (0.25) was half of
// the old primary PAIR's spacing (0.5). Both groups are independently a set
// of 4 equally-spaced phases, so both independently satisfy the COLA
// identity above regardless of their starting phase p (the identity holds
// for ANY p) -- the 0.125 offset only changes which instant each group's
// envelope nulls land on, not whether either group sums to a constant, so
// this choice doesn't need re-deriving, just applying. Both groups read the
// exact same shared delayLine -- same history, same instant -- so they
// pitch-shift identical source content by the identical ratio; only the
// grain-phase offsets differ, which is what keeps quadratureVoices'
// instantaneous output decorrelated from primaryVoices' despite processing
// the same input (Airwindows Galactic-style quadrature-offset pitch
// shifting, chosen over per-channel tank duplication -- see
// docs/shimmer-reverb-implementation-plan.md's Phase 4 section -- precisely
// because it needs no second tank/delay line and cannot disturb
// DattorroTank's already-tuned recirculating loop).
//
// ---------------------------------------------------------------------------
// The 0.5 normalization factor -- READ THIS BEFORE TOUCHING THE VOICE COUNT
// ---------------------------------------------------------------------------
// Per the COLA identity above, N=4 equally-spaced voices sum their envelopes
// to a CONSTANT 0.5*4 = 2.0, not 1.0. The old N=2 code never needed explicit
// normalization because 0.5*2 happens to equal 1.0 -- that was a
// coincidence of N=2, not a general property. For N=4, the weighted sum
// sampleK * hann(phaseK) summed across the group must be multiplied by
// 2.0/N = 0.5 to normalize back to unity gain (see processSample()). Getting
// this factor wrong produces a wrong OUTPUT LEVEL (doubled if omitted
// entirely, or halved if applied twice), not just a wrong envelope shape --
// it is silent in the sense that the signal still sounds like a plausible
// shimmer, just at roughly +6dB or -6dB relative to the old 2-voice
// behavior, so it will not obviously "sound broken" by ear. This is exactly
// what PitchShifterTests' crossfade-sums-to-unity test exists to catch
// numerically. If the voice count per group ever changes again, this
// factor (2.0/N) must be recomputed, not left at 0.5.
//
// processSample()'s signature and the meaning of its return value (the
// primary group's output) and getQuadratureOutput() (the quadrature group's
// output) are unchanged from before this 4-voice change -- only the internal
// voice count per group changed, not the public contract.
class PitchShifter
{
public:
    PitchShifter();
    ~PitchShifter();

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();

    // Recomputes the cached pitchRatio; safe to call at any time, not just
    // in prepare() -- Phase 5 will call this from a parameter listener.
    void setPitchShiftSemitones (float semitones);

    // For inspection/testing of the cached ratio -- this class owns no
    // other externally-visible DSP state.
    float getPitchRatio() const noexcept { return pitchRatio; }

    float processSample (float input);

    // Read-only peek at the quadrature voice group's crossfaded output from
    // the most recent processSample() call -- mirrors the "process, then
    // peek a simultaneously-computed side value" idiom already used by
    // DattorroTank::peekFeedbackSignal(). Not a second processSample()-like
    // method the caller has to invoke separately; the quadrature voices are
    // advanced inside processSample() itself, this just reads what that call
    // already computed.
    float getQuadratureOutput() const noexcept { return quadratureOutput; }

private:
    using DelayLineType = juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd>;

    struct Voice
    {
        float grainPhase = 0.0f;
    };

    // Voices per group -- see the class-level comment above for the full
    // derivation of why 4 (not 2) and what the 2.0/N normalization factor
    // below depends on this exact value.
    static constexpr int numVoicesPerGroup = 4;

    //==============================================================================
    // Grain length: lowered from 25.0f (the architecture doc's ~20-30ms
    // range midpoint) to 20.0f (that range's low end), after the shimmer
    // being reported as "sounds slightly detuned" was traced to a real,
    // derivable artifact: with the ORIGINAL 2-voices-per-group design, each
    // group's two voices read positions in the shared delayLine that stay a
    // CONSTANT time-offset apart --
    // lag = 0.5 * grainLengthMs * |pitchRatio - 1| -- because both voices'
    // voiceDelaySamples(phase) move at the same rate, just phase-offset by
    // 0.5. At the default +12st (pitchRatio = 2.0), that lag was
    // 0.5 * 25 * 1.0 = 12.5ms at the old grain length -- summing two copies
    // of the same recirculating signal 12.5ms apart is a comb filter with
    // nulls around 40/120/200Hz, and the lag is worst near a full octave
    // (|ratio-1| large), not a coincidence that it's audible at exactly this
    // shimmer's default shift. Lowering to 20.0f reduced the same-shift lag
    // to 0.5 * 20 * 1.0 = 10ms (nulls pushed to ~50/150/250Hz) -- a real but
    // modest improvement, not a fix, because the underlying 2-voice
    // structure still had this lag by construction at any shift near an
    // octave. The actual structural fix -- quadrupling each group's voice
    // count to 4 (see the class-level comment above) -- has since landed,
    // which quarters the nearest-neighbor lag at this same grain length
    // instead of relying on shortening grains further (which has its own
    // audible floor: grain repetition rate becoming audible once grains get
    // short enough). grainLengthMs is left at 20.0f rather than reverted,
    // since the two improvements are independent and both still help. See
    // docs/shimmer-reverb-implementation-plan.md's Phase 3/5 notes for this
    // investigation's full writeup.
    static constexpr float grainLengthMs = 20.0f;

    // Full intended eventual range (-24..+24 st, pitchRatio 0.25..4.0), even
    // though Phase 3 only ever hardcodes one shift value at a time -- buffers
    // below are sized for this whole range up front so nothing needs
    // resizing later (the plan's explicit "size for -24 st, not the +12 st
    // default" pitfall).
    static constexpr float minSemitones = -24.0f;
    static constexpr float maxSemitones = 24.0f;

    // baseDelaySamples must stay generously above the minimum needed to keep
    // voiceDelaySamples(phase) >= 0 across phase in [0,1] for the most
    // extreme *upward* ratio (pitchRatio = 4.0 at +24 st, phase=1 needs
    // baseDelaySamples - grainLengthSamples*3.0 >= 0). 4x the grain length
    // gives comfortable margin above that minimum. This margin is governed
    // purely by the phase range [0,1] and pitchRatio's bounds via
    // voiceDelaySamples() below -- it does not depend on how many voices
    // per group sample from delayLine, so going from 2 to 4 voices per group
    // does not change this constant or invalidate this reasoning (re-verified
    // when the voice count changed, not just assumed).
    static constexpr float baseDelayGrainMultiple = 4.0f;

    // Maximum delay-line capacity must comfortably exceed the largest value
    // voiceDelaySamples(phase) can reach, which happens at the most extreme
    // *downward* ratio (pitchRatio = 0.25 at -24 st, phase=1 gives
    // baseDelaySamples + grainLengthSamples*0.75) -- extra margin of 2x the
    // grain length beyond that strict minimum, plus the usual +1 sample
    // headroom convention already used by ScratchSchroederTank/DattorroTank.
    // Same phase-range/pitchRatio-only dependency note as
    // baseDelayGrainMultiple above applies here: unaffected by voice count.
    static constexpr float maxDelayExtraGrainMultiple = 2.0f;

    // voiceDelaySamples(phase) = baseDelaySamples - phase * grainLengthSamples
    //                            * (pitchRatio - 1.0f)
    // This makes the delay change by exactly (1 - pitchRatio) per sample as
    // phase advances by 1/grainLengthSamples per sample -- precisely the
    // rate needed for the read pointer to move through recorded
    // buffer-content at pitchRatio x real-time, which is the mechanism that
    // produces the pitch shift. Computed directly as a function of phase
    // (not accumulated with +=) so drift can't creep in and so the
    // wrap-driven "reset" of each voice's read position is automatic/
    // implicit rather than needing special-case code at the wrap point.
    // Every voice in both groups calls this same function -- it has no
    // dependency on voice count or which group a voice belongs to.
    float voiceDelaySamples (float phase) const noexcept;

    // Raised-cosine (Hann) envelope: 0 at phase 0 (masking the read-position
    // discontinuity when a voice's delay value jumps at wrap), 1 at
    // phase 0.5. See the class-level comment above for the general N-voice
    // constant-sum (COLA) identity this envelope satisfies for any set of N
    // equally-spaced phases, and the resulting 2.0/N normalization factor
    // processSample() applies.
    static float hannEnvelope (float phase) noexcept;

    DelayLineType delayLine;

    // Computed once in prepare() from the live sample rate, not recomputed
    // per sample.
    float grainLengthSamples = 0.0f;
    float baseDelaySamples = 0.0f;

    // Cached so processSample() never calls std::pow (updated only when
    // setPitchShiftSemitones() is called).
    float pitchRatio = 1.0f;

    // Primary group: same role as the old voiceA/voiceB pair, feeds the
    // recirculating tank path via processSample()'s return value. 4 voices
    // at equally-spaced phases 0.0/0.25/0.5/0.75 -- see the class-level
    // comment above.
    std::array<Voice, numVoicesPerGroup> primaryVoices;

    // Quadrature group: same role as the old voiceC/voiceD pair, feeds
    // ShimmerReverbEngine's stereo-width path via getQuadratureOutput(). 4
    // voices at equally-spaced phases 0.125/0.375/0.625/0.875 -- offset from
    // primaryVoices by half of primaryVoices' own inter-voice spacing, same
    // relationship the old quadrature pair had to the old primary pair. See
    // the class-level comment above.
    std::array<Voice, numVoicesPerGroup> quadratureVoices;

    // Cached crossfaded (and normalized -- see the class-level 0.5 warning
    // above) output of the quadrature group from the most recent
    // processSample() call, exposed via getQuadratureOutput().
    float quadratureOutput = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchShifter)
};
