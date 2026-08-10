#pragma once

#include <JuceHeader.h>
#include <array>

// Phase 1 plumbing-validation tank — THROWAWAY code, removed once Phase 2's
// real Dattorro topology lands (see docs/shimmer-reverb-implementation-plan.md,
// Phase 1). Mono Freeverb-style tank: 6 parallel feedback comb filters summed
// together, followed by 2 series allpass filters. Its only job is to prove the
// juce::dsp::DelayLine / prepare() sizing / real-time-safety plumbing before
// Phase 2 spends effort on the committed Dattorro plate topology. Do not wire
// this into ShimmerReverbEngine and do not over-invest in tuning it.
class ScratchSchroederTank
{
public:
    ScratchSchroederTank();
    ~ScratchSchroederTank();

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();

    // Mono tank: if given a stereo (or multi-channel) block, the input
    // channels are averaged to mono internally and the same mono result is
    // written back to every output channel.
    void process (juce::dsp::AudioBlock<float>& block);

private:
    using DelayLineType = juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear>;

    struct DelayLineState
    {
        DelayLineType delayLine;
        float dampState = 0.0f;
    };

    static constexpr int numCombs = 6;
    static constexpr int numAllpasses = 2;

    // Comb/allpass delay lengths in milliseconds, converted to samples from
    // spec.sampleRate inside prepare(). Deliberately non-multiple/mutually
    // prime-ish lengths so metallic ringing doesn't show up even in this
    // throwaway tank.
    static constexpr std::array<float, numCombs> combDelayMs { 35.3f, 36.7f, 33.9f, 30.5f, 28.9f, 25.3f };
    static constexpr std::array<float, numAllpasses> allpassDelayMs { 5.0f, 1.7f };

    // Starting points, not final tuning — kept as named constants so they're
    // easy to tweak later.
    static constexpr float dampAmount = 0.2f;
    static constexpr float feedbackAmount = 0.84f;
    static constexpr float allpassFeedback = 0.5f;

    float processComb (float input, DelayLineState& c);
    float processAllpass (float input, DelayLineState& a);

    std::array<DelayLineState, numCombs> combs;
    std::array<DelayLineState, numAllpasses> allpasses;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScratchSchroederTank)
};
