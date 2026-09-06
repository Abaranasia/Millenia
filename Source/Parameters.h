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

    // "Shimmer Sustain" task: backs ShimmerReverbEngine::setShimmerSustain()
    // -- see that method's header comment for its two effects (DattorroTank's
    // recirculation cap, inverted, plus a direct gate on the audible Width
    // injection). Default (0.85f) matches
    // ShimmerReverbEngine::defaultShimmerSustainAmount, a freshly-chosen
    // default for this control, not a preserved match to any prior hardcoded
    // constant.
    constexpr auto shimmerSustain = "shimmerSustain";

    constexpr auto damping    = "damping";
    constexpr auto width      = "width";
    constexpr auto mix        = "mix";
    constexpr auto bypass     = "bypass";

    // Phase 9 (see docs/shimmer-reverb-implementation-plan.md): classic
    // infinite-sustain trick -- mutes fresh dry input into the diffuser and
    // pins DattorroTank's decayGain near unity while engaged, so whatever
    // was already recirculating just sustains forever instead of decaying.
    // A juce::AudioParameterBool, not a processor-only flag, per the
    // juce-plugin-dev skill's bypass hard rule (same reasoning applies to
    // any control that changes audible behavior and must be host-
    // automatable/state-saveable).
    constexpr auto freeze     = "freeze";

    // Phase 10 (see docs/shimmer-reverb-implementation-plan.md and
    // Source/DSP/LoopCapture.h): a NEW, fully independent feature alongside
    // Freeze above -- captures a window of recent wet output and repeats it
    // as a static, crossfaded loop, rather than pinning the tank's own decay.
    // A clean on/off juce::AudioParameterBool (unlike freeze's continuous
    // dial): Loop Length below controls the captured window's SIZE, not a
    // continuous live/frozen blend amount, so there is no "how much" concept
    // here to expose as a float.
    constexpr auto loopFreeze = "loopFreeze";

    // Loop Length in milliseconds -- backs ShimmerReverbEngine::
    // setLoopLengthMs() -> LoopCapture::setLoopLengthMs(). Range MUST
    // exactly match LoopCapture::minLoopLengthMs/maxLoopLengthMs (see
    // LoopCapture.h) -- there is no compile-time-shared-constant mechanism
    // in this codebase, same as e.g. DattorroTank::defaultDecayGain vs. the
    // feedback parameter's default above, so keep both in sync by hand if
    // either ever changes.
    constexpr auto loopLength = "loopLength";
}

// Builds the full parameter layout backing MilleniaAudioProcessor::apvts.
// Every default here must match the corresponding ShimmerReverbEngine/
// DattorroTank/PitchShifter default exactly (see those classes'
// defaultPitchShiftSemitones/defaultDecayGain/defaultDampingCoefficient/
// defaultShimmerWidthGain/defaultMix/defaultBypassed) so wiring up APVTS in
// this step is not a silent behavior change.
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
