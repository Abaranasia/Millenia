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
    //
    // Originally a clean on/off juce::AudioParameterBool ("loopFreeze"). Retyped
    // to a continuous juce::AudioParameterFloat (2026-09-26, "Loop Mix dial"
    // backlog item resolved as option "a") since LoopCapture::process() already
    // computes a continuous 0..1 crossfade between the live wet signal and the
    // captured loop -- the bool toggle was only ever driving that crossfade to
    // a hard 0 or 1. Renamed id (loopFreeze -> loopMix) rather than keeping the
    // old id with a new type, since nothing has shipped/released yet (no
    // saved-preset compatibility to preserve) and a stale bool-flavored id would
    // be confusing for a parameter that is now genuinely continuous -- see this
    // file's own header comment on why IDs are normally never retyped, which
    // does not apply pre-release. Drives ShimmerReverbEngine::setLoopFreezeAmount()
    // directly -- that method's name is intentionally left unchanged (matches
    // this project's existing precedent of keeping a DSP setter's old name
    // after its APVTS parameter was retyped, see setFreezeAmount() after Phase
    // 9's Freeze bool->float switch).
    constexpr auto loopMix = "loopMix";

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
