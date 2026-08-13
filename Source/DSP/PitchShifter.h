#pragma once

#include <JuceHeader.h>

// Hand-rolled dual-delay-line crossfade pitch shifter (Phase 3, see
// docs/shimmer-reverb-implementation-plan.md). One shared DelayLine is
// written once per sample; two independent read "voices" (A/B) peek into it
// at delay offsets that are a direct function of a per-voice grain phase,
// crossfaded with a raised-cosine (Hann) window so the two voices' envelopes
// always sum to exactly 1.0 (see processSample()). Mono only -- this class
// only ever sees DattorroTank's mono output, one sample at a time from
// ShimmerReverbEngine's feedback loop. Owns no parameter knowledge itself;
// Phase 5 wires setPitchShiftSemitones() to a real APVTS parameter.
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

private:
    using DelayLineType = juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd>;

    struct Voice
    {
        float grainPhase = 0.0f;
    };

    //==============================================================================
    // Grain length: middle of the architecture doc's ~20-30ms range.
    static constexpr float grainLengthMs = 25.0f;

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
    // gives comfortable margin above that minimum.
    static constexpr float baseDelayGrainMultiple = 4.0f;

    // Maximum delay-line capacity must comfortably exceed the largest value
    // voiceDelaySamples(phase) can reach, which happens at the most extreme
    // *downward* ratio (pitchRatio = 0.25 at -24 st, phase=1 gives
    // baseDelaySamples + grainLengthSamples*0.75) -- extra margin of 2x the
    // grain length beyond that strict minimum, plus the usual +1 sample
    // headroom convention already used by ScratchSchroederTank/DattorroTank.
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
    float voiceDelaySamples (float phase) const noexcept;

    // Raised-cosine (Hann) envelope: 0 at phase 0 (masking the read-position
    // discontinuity when a voice's delay value jumps at wrap), 1 at
    // phase 0.5. With voice B's phase locked exactly 0.5 ahead of voice A's,
    // Hann(p) + Hann(p+0.5) == 1 identically for every phase value, so the
    // two voices' contributions always crossfade to unity gain by
    // construction -- no extra normalization needed.
    static float hannEnvelope (float phase) noexcept;

    DelayLineType delayLine;

    // Computed once in prepare() from the live sample rate, not recomputed
    // per sample.
    float grainLengthSamples = 0.0f;
    float baseDelaySamples = 0.0f;

    // Cached so processSample() never calls std::pow (updated only when
    // setPitchShiftSemitones() is called).
    float pitchRatio = 1.0f;

    Voice voiceA, voiceB;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchShifter)
};
