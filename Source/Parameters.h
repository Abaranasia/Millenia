#pragma once

#include <JuceHeader.h>

// Phase 5 (see docs/shimmer-reverb-implementation-plan.md): the single
// AudioProcessorValueTreeState parameter layout for the whole plugin, plus
// the stable string ID constants every other file (PluginProcessor, and
// eventually PluginEditor's attachments in Phase 6) must reference instead
// of duplicating a bare string literal at each call site. IDs are load-
// bearing for saved presets/host automation once anything ships -- never
// rename/remove/retype one of these after release (see the juce-plugin-dev
// skill's parameter decision-gate table).
namespace ParamIDs
{
    constexpr auto pitchShift = "pitchShift";
    constexpr auto feedback   = "feedback";
    constexpr auto shimmerAmount = "shimmerAmount";
    constexpr auto damping    = "damping";
    constexpr auto width      = "width";
    constexpr auto mix        = "mix";
    constexpr auto bypass     = "bypass";
}

// Builds the full parameter layout backing MilleniaAudioProcessor::apvts.
// Every default here must match the corresponding ShimmerReverbEngine/
// DattorroTank/PitchShifter default exactly (see those classes'
// defaultPitchShiftSemitones/defaultDecayGain/defaultDampingCoefficient/
// defaultShimmerWidthGain/defaultMix/defaultBypassed) so wiring up APVTS in
// this step is not a silent behavior change.
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
