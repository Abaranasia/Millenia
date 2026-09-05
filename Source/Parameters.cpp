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

    // Backs ShimmerReverbEngine::setShimmerAmount() -> DattorroTank::
    // setShimmerFeedbackGain(). Independently gains the pitch-shifted signal
    // layered additively on top of the tank's own natural (decayGain-scaled)
    // recirculation -- decoupling "how much shimmer cascade gets added" from
    // "how long the plain tail sustains" (decayGain). At 0.0f the tank is a
    // plain (unshifted) reverb only; at 1.0f (default) the shimmer cascade
    // is added at full strength.
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::shimmerAmount, 1 },
        "Shimmer Amount",
        juce::NormalisableRange<float> (0.0f, 1.0f),
        1.0f));

    // Damping coefficient for DattorroTank's one-pole leaky-integrator
    // damping filter, y[n] = a*y[n-1] + (1-a)*x[n]. Range fixed 2026-08-22
    // (by-ear report: "the damping dial provides no noticeable difference") --
    // the previous 0.0..0.05 range was a real bug, not just an over-cautious
    // choice: the coefficient-to-cutoff relationship this filter actually
    // implements is fc = -(sampleRate / 2*pi) * ln(a) (the same relationship
    // DattorroTank.h's defaultDampingCoefficient comment already uses to
    // derive "an 8000Hz cutoff maps to a~=0.32"). At the OLD range's own
    // maximum (0.05), fc ~= 21kHz; at the default (0.0005), fc is already
    // above Nyquist (~53kHz) -- so the entire old slider only ever swept
    // cutoff from "no filtering" to "still only rolling off content above
    // 21kHz," nowhere near the ~8kHz reference point where damping actually
    // becomes audible. Measured directly (not just derived): a diagnostic
    // spectral-tilt test (DattorroTankTests.cpp, "Damping's APVTS range
    // produces almost no audible spectral difference") found the old range
    // covered only ~20% of the brightness swing between the default and that
    // ~0.32 reference. New range extends the maximum to 0.3f -- just under
    // the documented "sounds like a delay, not a reverb" 0.32 extreme, so the
    // full knob throw actually reaches a genuinely dark/damped tail at full
    // clockwise, not just a barely-perceptible one. Default (0.0005f,
    // "bright continuous wash") and skew (0.3, biasing resolution toward the
    // low end where the tested-good default lives) are UNCHANGED -- only the
    // upper bound was wrong. Still a REASONED value, not an ear-tuned final
    // one -- an actual listening pass across the new range is still open.
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::damping, 1 },
        "Damping",
        juce::NormalisableRange<float> (0.0f, 0.3f, 0.0f, 0.3f),
        0.0005f));

    // Backs ShimmerReverbEngine::setWidth() (shimmerWidthGain). Default
    // matches ShimmerReverbEngine::defaultShimmerWidthGain (0.15f).
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::width, 1 },
        "Width",
        juce::NormalisableRange<float> (0.0f, 1.0f),
        0.15f));

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

    // Phase 9 Freeze (see docs/shimmer-reverb-implementation-plan.md). Was
    // originally an AudioParameterBool driving smoothedFreeze's 0/1 target,
    // same convention as bypass above -- switched to a continuous float
    // (2026-08-22, replacing the on/off toggle with a dial in the editor) so
    // the user can dial in a partial freeze amount live over a sounding
    // signal, not just snap between the two extremes. PluginProcessor's
    // wiring (freezeParam/smoothedFreeze/setFreezeAmount()) is completely
    // unchanged by this -- it already treated freezeParam as a float and
    // freezeAmount throughout the DSP chain was always continuous [0, 1];
    // only the APVTS parameter TYPE and the editor control change here.
    // Default 0.0f, same as before. Display name is "Freeze Amount" (renamed
    // from plain "Freeze", 2026-08-22) so it reads distinctly from the
    // editor's separate freezeQuickToggle checkbox, which is also just
    // labelled "Freeze" -- see PluginEditor.cpp.
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::freeze, 1 },
        "Freeze Amount",
        juce::NormalisableRange<float> (0.0f, 1.0f),
        0.0f));

    // Phase 10 (see docs/shimmer-reverb-implementation-plan.md and
    // Source/DSP/LoopCapture.h): Loop Freeze -- additive, fully independent
    // of Freeze above. Default false, matching LoopCapture's own "default is
    // a true no-op" convention (loopFreezeAmount=0.0f never reads back the
    // captured loop).
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { ParamIDs::loopFreeze, 1 },
        "Loop",
        false));

    // Range 50-4000ms MUST EXACTLY MATCH LoopCapture::minLoopLengthMs (50.0f)
    // and LoopCapture::maxLoopLengthMs (4000.0f) -- an upper bound here
    // higher than LoopCapture's actual buffer capacity would silently
    // truncate a requested max-length capture; a mismatch either way is a
    // real bug, not just a tuning concern (see LoopCapture.h's
    // maxLoopLengthMs comment for the full rationale). Default 500ms matches
    // LoopCapture::defaultLoopLengthMs.
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::loopLength, 1 },
        "Loop Length",
        juce::NormalisableRange<float> (50.0f, 4000.0f),
        500.0f));

    return layout;
}
