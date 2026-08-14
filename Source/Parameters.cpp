#include "Parameters.h"

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Bipolar pitch shift, semitones. Range is already the log-compressed
    // unit shimmer reverbs work in, so a plain linear NormalisableRange is
    // correct here -- no additional skew needed. Default matches
    // ShimmerReverbEngine's defaultPitchShiftSemitones (12.0f, classic
    // shimmer octave-up).
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::pitchShift, 1 },
        "Pitch Shift",
        juce::NormalisableRange<float> (-24.0f, 24.0f),
        12.0f));

    // Backs ShimmerReverbEngine::setFeedback() -> DattorroTank::setDecay().
    // Upper bound of 0.85 matches shimmer-reverb-architecture.md's documented
    // safe ceiling; default matches DattorroTank::defaultDecayGain (0.7f,
    // settled during Phase 3's structural fix -- see that class's header
    // comment for the 0.6f -> 0.8f (rejected) -> 0.7f history).
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::feedback, 1 },
        "Feedback",
        juce::NormalisableRange<float> (0.0f, 0.85f),
        0.7f));

    // Damping coefficient for DattorroTank's one-pole leaky-integrator
    // damping filter. Range/skew are a REASONED STARTING POINT, not an
    // ear-tuned final value -- an actual listening pass to fine-tune this is
    // still open (see docs/shimmer-reverb-implementation-plan.md, Phase 5).
    // The two empirically-known points from real project history are
    // 0.0005 (Phase 2's tested-good default -- "bright continuous wash") and
    // ~0.32 (Phase 2's proven-bad value -- "discrete dull thuds," see
    // DattorroTank.h's defaultDampingCoefficient comment, which found an
    // 8000Hz IIR cutoff maps to ~0.32 here, over 600x too aggressive). This
    // 0.0..0.05 range sits well below that proven-bad value, leaving the
    // whole slider inside the useful creative range; the 0.3 skew factor
    // biases slider resolution toward the low end so the tested-good
    // 0.0005 default has reasonable resolution around it rather than being
    // crammed into an unusable sliver near zero on a linear slider.
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::damping, 1 },
        "Damping",
        juce::NormalisableRange<float> (0.0f, 0.05f, 0.0f, 0.3f),
        0.0005f));

    // Backs ShimmerReverbEngine::setWidth() (shimmerWidthGain). Default
    // matches ShimmerReverbEngine::defaultShimmerWidthGain (0.3f).
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::width, 1 },
        "Width",
        juce::NormalisableRange<float> (0.0f, 1.0f),
        0.3f));

    // Dry/wet mix. NOT called directly against ShimmerReverbEngine::setMix()
    // from PluginProcessor::processBlock() -- see the bypass parameter below
    // and PluginProcessor's smoothing wiring for why. Default matches
    // ShimmerReverbEngine::defaultMix (1.0f, fully wet).
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::mix, 1 },
        "Mix",
        juce::NormalisableRange<float> (0.0f, 1.0f),
        1.0f));

    // Bypass does NOT call ShimmerReverbEngine::setBypassed() -- PluginProcessor
    // instead drives the mix smoother toward 0 so bypass gets the same
    // click-free smoothing as every other mix change (see PluginProcessor.cpp's
    // processBlock() comment for the full rationale). Default matches
    // ShimmerReverbEngine::defaultBypassed (false).
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { ParamIDs::bypass, 1 },
        "Bypass",
        false));

    return layout;
}
