#include "Parameters.h"
#include "DSP/DattorroTank.h"

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

    // Slice 2 (docs/reverb-space-research.md section 9). Decay replaces
    // the old Feedback gain (see ParamIDs::decay): RT60 in seconds, backing
    // ShimmerReverbEngine::setDecaySeconds(). Log-like skew (geometric
    // midpoint ~2.45 s at the knob's centre). Default 7.0 s gives g = 0.6993
    // at Size 1, the old 0.7 Feedback default to within 0.1%
    // (DattorroTank::defaultDecaySeconds). g stays clamped to the old 0.85
    // ceiling, so the longest reachable RT60 depends on Size (~7.7 s at
    // 0.5, ~15.4 s at 1); settings above it clamp silently, as the editor
    // tooltip says.
    {
        juce::NormalisableRange<float> decayRange (0.3f, 20.0f);
        decayRange.setSkewForCentre (2.449f);
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::decay, 1 }, "Decay", decayRange, 7.0f));
    }

    // Size: scales the tank's delay lines and output taps (not the input
    // diffusers), 0.5x..2x, default 1.0 (the unscaled Dattorro tank). Skew
    // puts 1.0 at the knob's centre. Glides per sample inside the tank, so
    // PluginProcessor passes it raw.
    {
        juce::NormalisableRange<float> sizeRange (0.5f, 2.0f);
        sizeRange.setSkewForCentre (1.0f);
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::size, 1 }, "Size", sizeRange, 1.0f));
    }

    // Modulation: 0..1 -> 0..1 ms of allpass1 delay modulation in the tank
    // (DattorroTank::setModulation()); 0 is bit-identical to the
    // unmodulated tank. Default 0.1 (0.1 ms), chosen by ear 2026-10-11 over
    // 0.3: at 0.3 Shimmer Sustain's tail-decay effect vanished (2.4 dB gap
    // -> ~0) and PitchShifter offset divergence rose (up to x3.4 at 330 Hz),
    // see docs/reverb-dev-journal.md slice 2.
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::modulation, 1 },
        "Modulation",
        juce::NormalisableRange<float> (0.0f, 1.0f),
        DattorroTank::defaultModulation));

    // Backs ShimmerReverbEngine::setShimmerAmount() -> DattorroTank::
    // setShimmerAmountGain(). Independently gains the pitch-shifted signal
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

    // Backs ShimmerReverbEngine::setShimmerFeedback() -- see that method's
    // header comment for its two effects (DattorroTank's recirculation cap,
    // direct since 2026-10-24 -- it was inverted before -- plus a direct gate
    // on process()'s audible Width/decorrelation injection). Shown as
    // "Shimmer Feedback" since 2026-10-24; the ID was renamed from
    // shimmerSustain to shimmerFeedback on 2026-10-25. Old states carrying
    // the removed IDs (shimmerSustain, feedback, loopFreeze) are NOT
    // migrated, by decision (nothing has shipped): APVTS ignores the unknown
    // IDs and the new ones keep their defaults, e.g. this one stays at 0.55.
    // Independently controls how present and long-lived the shimmer layer
    // is, decoupled from both Decay (decayGain) and Shimmer Amount
    // (shimmerAmountGain, the overall shimmer level).
    //
    // Default (was 0.85f) matches ShimmerReverbEngine::defaultShimmerFeedbackAmount
    // -- a freshly-chosen default for this control's now-widened scope, NOT
    // a preserved match to any prior hardcoded constant (that guarantee was
    // dropped 2026-09-06 once this parameter's scope grew to also gate the
    // always-live Width path, which the old hardcoded behavior never touched
    // at all -- see ShimmerReverbEngine.h's defaultShimmerFeedbackAmount
    // comment). 2026-10-24: 0.85f -> 0.55f with the un-inverted mapping
    // (docs/shimmer-character-research.md section 7.1 item 2).
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::shimmerFeedback, 1 },
        "Shimmer Feedback",
        juce::NormalisableRange<float> (0.0f, 1.0f),
        0.55f));

    // Damping coefficient for DattorroTank's one-pole leaky-integrator
    // damping filter, y[n] = a*y[n-1] + (1-a)*x[n] (DC gain 1, stable for
    // any |a| < 1), applied twice per loop pass. The stored value is the
    // coefficient itself; the filter's -3 dB corner is
    // acos ((1 + a^2 - 2 (1-a)^2) / (2a)) * fs / 2pi (a ~= 0.3 -> 9.7 kHz,
    // 0.5 -> 5.1 kHz, 0.7 -> 2.5 kHz at 44.1 kHz).
    //
    // History: 0..0.05 (swept only content above 21 kHz), then 0..0.3
    // (2026-08-22), then a 0.08 knob centre (2026-11). Measured 2026-11-28
    // at 44.1 kHz, Size 1, Decay 7 s, +12 st, that 0..0.3 travel was still
    // inaudible: at the maximum the per-pass gain was only -2.3 dB at 8 kHz
    // and -5.4 dB at Nyquist, the steady white-noise spectrum moved at most
    // 3.4 dB at 16 kHz and 1 dB at 4 kHz, and the tail RT60 at 1k/4k/8k/16k
    // went from 6.6/6.3/5.8/5.7 s (default) to only 6.9/5.5/3.8/2.7 s.
    //
    // Current law: range 0..0.7, centre a = 0.5. At 0.7 the engine tail has
    // RT60 5.9/3.1/1.8/1.5 s at 1k/4k/8k/16k (8 kHz at ~30% of the 1 kHz
    // time), so the whole travel is audible and Damping can tame a dense
    // tail at small Size. The centre gives about half the maximum high-
    // frequency loss per loop pass in dB (8 kHz: -5.2 dB at 0.5 vs -10.0 dB at
    // 0.7). The default stays 0.0005 ("bright continuous wash", effectively no
    // damping, bit-identical to the engine/tank default); it sits at the far
    // left of the knob. Only the knob's range and skew changed: the stored
    // value is still the coefficient and the parameter ID/version are
    // unchanged (nothing has shipped, so no migration).
    {
        juce::NormalisableRange<float> dampingRange (0.0f, 0.7f);
        dampingRange.setSkewForCentre (0.5f);
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::damping, 1 }, "Damping", dampingRange, 0.0005f));
    }

    // Backs ShimmerReverbEngine::setWidth(), an M/S width on the stereo tank
    // tail since 2026-10-11 (docs/reverb-space-research.md section 8.2):
    // 0 = mono, 1 = natural Dattorro stereo (default, matches
    // ShimmerReverbEngine::defaultWidth), 2 = 200%. Was 0..1, default 0.15,
    // as a gain on the shimmer side term only. Same ID, range widened in
    // place: nothing has shipped, so no saved preset or automation depends
    // on the old range (see the loopMix retype for the same reasoning).
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::width, 1 },
        "Width",
        juce::NormalisableRange<float> (0.0f, 2.0f),
        1.0f));

    // Slice 1 space controls (docs/reverb-space-research.md sections
    // 8.3-8.4). Defaults are the "off" endpoints -- 0 ms pre-delay, 20 Hz
    // low cut, 20 kHz high cut -- where ShimmerReverbEngine's output
    // equals no pre-delay / no EQ exactly, so the default sound is unchanged. The cut frequencies
    // use a skew that puts the range's geometric midpoint at the knob's
    // centre (~141 Hz and ~4.5 kHz), the usual log-like feel for frequency.
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::preDelay, 1 },
        "Pre-Delay",
        juce::NormalisableRange<float> (0.0f, 250.0f),
        0.0f));

    {
        juce::NormalisableRange<float> lowCutRange (20.0f, 1000.0f);
        lowCutRange.setSkewForCentre (141.4f);
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::lowCut, 1 }, "Low Cut", lowCutRange, 20.0f));

        juce::NormalisableRange<float> highCutRange (1000.0f, 20000.0f);
        highCutRange.setSkewForCentre (4472.0f);
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::highCut, 1 }, "High Cut", highCutRange, 20000.0f));
    }

    // Air (see ParamIDs::air): 0 dB default is the off state, where the
    // engine's output equals no shelf exactly.
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::air, 1 },
        "Air",
        juce::NormalisableRange<float> (0.0f, 6.0f),
        0.0f));

    // Shimmer Tone (see ParamIDs::shimmerTone): 1.5-10 kHz, default 5 kHz
    // (ShimmerReverbEngine::defaultShimmerToneHz), skewed like the wet EQ
    // corners so the range's geometric midpoint (~3.87 kHz) sits at the
    // knob's centre. The top endpoint is Off -- the engine applies only the
    // pitch-ratio alias cap there, no Tone darkening -- and the Hz value is
    // scaled by that cap when the cap is below 10 kHz (see
    // ShimmerReverbEngine::setShimmerTone()), so the value text says "Off"
    // at 10 kHz, in the editor (SliderAttachment shows the
    // parameter's own text) and in host automation lanes alike. The text
    // carries its own " Hz" unit, so the editor sets no suffix. Text entry
    // accepts "Off" (-> 10 kHz) or a number in Hz.
    {
        constexpr float minToneHz = 1500.0f, maxToneHz = 10000.0f;
        juce::NormalisableRange<float> shimmerToneRange (minToneHz, maxToneHz);
        shimmerToneRange.setSkewForCentre (3873.0f);
        auto attributes = juce::AudioParameterFloatAttributes()
                              .withStringFromValueFunction ([maxToneHz] (float value, int)
                              {
                                  return value >= maxToneHz ? juce::String ("Off") : juce::String (juce::roundToInt (value)) + " Hz";
                              })
                              .withValueFromStringFunction ([maxToneHz] (const juce::String& text)
                              {
                                  return text.trim().equalsIgnoreCase ("off") ? maxToneHz : text.getFloatValue();
                              });
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::shimmerTone, 1 }, "Shimmer Tone", shimmerToneRange, 5000.0f, attributes));
    }

    // Dry/wet mix. NOT called directly against ShimmerReverbEngine::setMix()
    // from PluginProcessor::processBlock() -- see the bypass parameter below
    // and PluginProcessor's smoothing wiring for why. Deliberately does NOT
    // match ShimmerReverbEngine::defaultMix (1.0f, fully wet, which the engine
    // tests and golden hashes rely on): the plugin opens at 40% wet, a typical
    // insert setting, so the dry signal stays audible on a fresh instance.
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::mix, 1 },
        "Mix",
        juce::NormalisableRange<float> (0.0f, 1.0f),
        0.4f));

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

    // Infinite mode (see ParamIDs::infinite). Default false matches
    // ShimmerReverbEngine's infiniteAmount default (0.0f), which is
    // bit-identical to the pre-Infinite sound. Raw 0/1 -- the ramp lives in
    // DattorroTank (infiniteRampSeconds), not in PluginProcessor.
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { ParamIDs::infinite, 1 },
        "Infinite",
        false));

    // Formant Correction (see ParamIDs::formantCorrection). Default false
    // matches ShimmerReverbEngine::defaultFormantCorrection: the corrector is
    // numerically unstable on tonal input inside the feedback loop (measured
    // 2026-10-25: ~59 dB of the 880 Hz octave removed after a +12 st shift
    // of a 440 Hz sine, ~2.6e6 internal peaks clipped into broadband noise by
    // the SafetyLimiter), so it is opt-in until the corrector itself is fixed
    // -- see docs/shimmer-character-research.md. Raw 0/1; the engine
    // crossfades the switch over 20 ms.
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { ParamIDs::formantCorrection, 1 },
        "Formant Correction",
        false));

    // Phase 10 (see docs/shimmer-reverb-implementation-plan.md and
    // Source/DSP/LoopCapture.h), retyped 2026-09-26 ("Loop Mix dial" backlog
    // item): continuous blend between the live processing chain and the
    // captured loop, replacing the old on/off toggle -- see ParamIDs::loopMix's
    // header comment for the full rationale. Default 0.0f, same no-op default
    // as the old bool's `false` (loopFreezeAmount=0.0f never reads back the
    // captured loop; LoopCapture::process()'s rising-edge capture trigger fires
    // the first time this moves off 0.0f, same as it did for the old toggle's
    // false->true transition).
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamIDs::loopMix, 1 },
        "Loop Mix",
        juce::NormalisableRange<float> (0.0f, 1.0f),
        0.0f));

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
