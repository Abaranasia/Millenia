#include "ShimmerReverbEngine.h"

ShimmerReverbEngine::ShimmerReverbEngine() = default;
ShimmerReverbEngine::~ShimmerReverbEngine() = default;

void ShimmerReverbEngine::prepare (const juce::dsp::ProcessSpec& spec)
{
    tank.prepare (spec);
    shifter.prepare (spec);
    feedbackDcBlocker.prepare (spec);
    safetyLimiter.prepare (spec);
    quadratureDcBlocker.prepare (spec);
    freezeLeveler.prepare (spec);
    formantCorrector.prepare (spec);
    loopCapture.prepare (spec);

    // Phase 5: seed the live-settable pitch shift with the same value that
    // used to be a one-time hardcoded constant, so behavior is unchanged
    // until a caller actually calls setPitchShiftSemitones() with something
    // different. Goes through the member (not shifter directly) so a
    // re-prepare() while already frozen (e.g. a host sample-rate change)
    // re-derives the correct freeze-crossfaded ratio instead of silently
    // snapping back to the un-crossfaded default -- see
    // pitchShiftSemitones' header comment.
    pitchShiftSemitones = defaultPitchShiftSemitones;
    updateShifterRatio();

    monoScratch.setSize (1, (int) spec.maximumBlockSize);

    // mix/bypassed/shimmerWidthGain are NOT re-applied here (unlike the
    // pitch shift line above): they already have correct member-
    // initializer defaults in the header, and re-assigning them on every
    // prepare() call would silently wipe out a caller's prior
    // setMix()/setBypassed()/setWidth() if the host re-triggers prepare()
    // (e.g. a sample-rate change) after those were already set -- that's a
    // real regression the pitch-shift line above doesn't have, since
    // resetting pitch shift on every prepare() call is the pre-existing
    // behavior this replaces (the old code unconditionally called
    // shifter.setPitchShiftSemitones() here too), not new behavior.
    reset();
}

void ShimmerReverbEngine::reset()
{
    tank.reset();
    shifter.reset();
    feedbackDcBlocker.reset();
    safetyLimiter.reset();
    quadratureDcBlocker.reset();
    freezeLeveler.reset();
    formantCorrector.reset();
    loopCapture.reset();
}

void ShimmerReverbEngine::setPitchShiftSemitones (float semitones)
{
    // Stored so setFreezeAmount() can recompute the shifter's actual ratio
    // against the current freeze crossfade without needing this value passed
    // back in -- see pitchShiftSemitones' header comment.
    pitchShiftSemitones = semitones;
    updateShifterRatio();
}

void ShimmerReverbEngine::setFeedback (float newFeedback)
{
    tank.setDecay (newFeedback);
}

void ShimmerReverbEngine::setDamping (float newDamping)
{
    tank.setDamping (newDamping);
}

void ShimmerReverbEngine::setShimmerAmount (float newShimmerAmount)
{
    shimmerAmount = juce::jlimit (0.0f, 1.0f, newShimmerAmount);
    tank.setShimmerFeedbackGain (shimmerAmount);
}

void ShimmerReverbEngine::setWidth (float newWidth)
{
    shimmerWidthGain = juce::jlimit (0.0f, 1.0f, newWidth);
}

void ShimmerReverbEngine::setMix (float newMix)
{
    mix = juce::jlimit (0.0f, 1.0f, newMix);
}

void ShimmerReverbEngine::setBypassed (bool shouldBypass)
{
    bypassed = shouldBypass;
}

void ShimmerReverbEngine::setFreezeAmount (float amount)
{
    freezeAmount = juce::jlimit (0.0f, 1.0f, amount);
    tank.setFreezeAmount (freezeAmount);
    freezeLeveler.setFreezeAmount (freezeAmount);
    shifter.setFreezeAmount (freezeAmount);

    // Re-derive the shifter's ratio against the just-updated freezeAmount --
    // PluginProcessor calls setPitchShiftSemitones() then setFreezeAmount()
    // every block (see pitchShiftSemitones' header comment for why this
    // crossfade exists at all), so this call is what actually applies each
    // block's freeze amount rather than lagging one block behind it.
    updateShifterRatio();
}

void ShimmerReverbEngine::setLoopFreezeAmount (float newAmount)
{
    loopCapture.setLoopFreezeAmount (juce::jlimit (0.0f, 1.0f, newAmount));
}

void ShimmerReverbEngine::setLoopLengthMs (float newLoopLengthMs)
{
    loopCapture.setLoopLengthMs (newLoopLengthMs);
}

void ShimmerReverbEngine::updateShifterRatio()
{
    // See pitchShiftCrossfadeCurve's header comment for why this is raised
    // to a power instead of a plain linear crossfade -- keeps the shift
    // amount essentially untouched through most of the dial's travel and
    // only pulls it toward unity near full freeze, while still reaching
    // EXACTLY 0 semitones (ratio 1.0) at freezeAmount=1.0, same as before.
    //
    // BUG, found and fixed 2026-08-22 (by-ear report: "a pitch variation is
    // noticeable when moving the freeze dial" -- still present after the
    // original fix, plus an unrelated-seeming Width loss at high freeze):
    // this used to be std::pow(1.0f - freezeAmount, pitchShiftCrossfadeCurve)
    // -- (1-f)^4 -- which is a real arithmetic error, not the curve shape
    // described above. (1-f)^4 is STEEPEST right at f=0 and FLATTENS out
    // near f=1: at f=0.5 it evaluates to 0.0625 (94% of the shift already
    // gone at the HALFWAY point of the dial, not "essentially untouched").
    // That is the exact opposite of the intended shape, and explains both
    // symptoms: pitch dropping audibly as soon as the dial leaves 0 (not
    // concentrated near full freeze), and reduced Width through most of the
    // range too (Width's sideShift term is the difference between the
    // primary/quadrature shifted signals -- less actual shift happening
    // means less spectral difference between them, hence a narrower-sounding
    // result, independent of anything specific to Width's own code). The
    // correct shape for "flat near 0, steep drop near 1, exactly 0 at 1" is
    // 1 - f^4, not (1-f)^4 -- e.g. at f=0.5 this evaluates to 0.9375 (94%
    // RETAINED, matching what was originally intended/described), still
    // reaching exactly 0 at f=1.0 (the anti-cascade guarantee is unaffected).
    const float crossfade = 1.0f - std::pow (freezeAmount, pitchShiftCrossfadeCurve);
    const float effectiveSemitones = pitchShiftSemitones * crossfade;
    shifter.setPitchShiftSemitones (effectiveSemitones);

    // Chipmunk-mitigation, LPC-based formant correction (see
    // FormantEnvelopeCorrector.h): keyed to the same EFFECTIVE shift actually
    // reaching the shifter right now (post freeze crossfade), not the raw
    // user-facing knob value, so the correction arms/disarms with whatever
    // shift is really happening -- and stays a bit-identical passthrough at
    // effectiveSemitones<=0, same convention as every other conditional DSP
    // stage in this codebase.
    formantCorrector.setPitchShiftSemitones (effectiveSemitones);
}

void ShimmerReverbEngine::process (juce::dsp::AudioBlock<float>& block)
{
    const auto numSamples = block.getNumSamples();
    const auto numChannels = block.getNumChannels();

    auto* monoData = monoScratch.getWritePointer (0);

    // Mono-sum the input into the scratch buffer first, exactly the same
    // channel-averaging contract DattorroTank::process() applies per
    // sample -- kept as its own pass so the per-sample tank/shifter
    // interleaving loop below only ever touches a single channel of data.
    for (size_t i = 0; i < numSamples; ++i)
    {
        float monoIn = 0.0f;

        for (size_t ch = 0; ch < numChannels; ++ch)
            monoIn += block.getSample ((int) ch, (int) i);

        if (numChannels > 1)
            monoIn /= (float) numChannels;

        monoData[i] = monoIn;
    }

    for (size_t i = 0; i < numSamples; ++i)
    {
        // Shift the tank's OWN recirculating signal before it re-enters the
        // tank, instead of running a separate parallel feedback path -- this
        // matches shimmer-reverb-architecture.md's documented topology
        // (tank output -> pitch shifter -> back into tank input) and makes
        // the shimmer the tank's actual sustain mechanism rather than a
        // weak side-loop competing with the tank's own unshifted decayGain
        // recirculation.
        float dryFeedback = tank.peekFeedbackSignal();
        float shiftedFeedback = shifter.processSample (dryFeedback);

        // Chipmunk-mitigation, LPC-based formant correction (see
        // FormantEnvelopeCorrector.h and docs/formant-preserving-pitch-shifter-
        // research.md sections 8-9): estimates the spectral envelope of the
        // signal entering the shifter (dryFeedback) and of the signal leaving it
        // (shiftedFeedback), then pulls the shifted signal's envelope back
        // toward the original -- replacing SpectralTiltCompensator's cruder
        // darkening-only approach, which this project's own research proved
        // structurally unable to fix the complaint on high notes. Applied here,
        // immediately after the shift and before DC blocking/limiting, so it
        // shapes exactly the signal that's about to re-enter the tank -- same
        // position in the chain SpectralTiltCompensator occupied.
        shiftedFeedback = formantCorrector.processSample (dryFeedback, shiftedFeedback);

        // Phase 4: remove DC/subsonic bias from the recirculating signal
        // before it hits the tanh soft-clip below -- DC removal has to
        // precede nonlinear shaping (a DC-biased signal clips asymmetrically
        // through tanh), and has to happen here, inside the loop, rather
        // than only at the final output, since the shifter's grain-
        // crossfade interpolation can introduce subsonic bias on every
        // recirculation (see DCBlocker.h and shimmer-reverb-architecture.md's
        // "DC offset accumulation" pitfall).
        float dcBlockedFeedback = feedbackDcBlocker.processSample (shiftedFeedback);

        // Phase 4: real permanent safety net, replacing the bare
        // std::tanh(...) stopgap that used to live here. SafetyLimiter is a
        // memoryless soft-knee shaper (see SafetyLimiter.h for why a
        // lookahead peak limiter was rejected for this exact spot inside
        // the feedback loop) that is EXACTLY unity below its threshold
        // (0.8f by default) -- unlike bare tanh, which very slightly
        // compresses every sample, even small ones. Only the genuinely
        // loud tail gets shaped now; small/moderate recirculating signal
        // passes through untouched.
        float safeFeedback = safetyLimiter.processSample (dcBlockedFeedback);

        // The shimmerAmount gain now lives inside DattorroTank itself (see
        // setShimmerFeedbackGain()) -- it scales safeFeedback as an
        // ADDITIVE term layered on top of the tank's own natural
        // decayGain-scaled recirculation, not a replacement for it. This is
        // NOT the Width direct-injection term below (safeFeedback/
        // quadratureSafe there stay full-strength, untouched by
        // shimmerAmount).
        // Phase 9 Freeze (see docs/shimmer-reverb-implementation-plan.md):
        // scale the fresh dry input by (1.0f - freezeAmount) so freezing
        // stops injecting NEW dry signal into the loop while decayGain is
        // separately pinned near-unity inside DattorroTank (see
        // setFreezeAmount()) -- muting only one of those two things isn't
        // enough: pinning decayGain alone while still feeding fresh input
        // would keep adding fresh energy into an already near-losslessly
        // sustaining loop (a slow but real runaway), and muting input alone
        // without pinning decayGain would just silence the plugin as the
        // existing tail decays at its normal, un-frozen rate.
        float tankOut = tank.processSample (monoData[i] * (1.0f - freezeAmount), safeFeedback);

        // Phase 4 stereo decorrelation: everything above this line is
        // unchanged from Phase 3/4's tuned recirculating loop (tank input,
        // shifter feedback, DC blocking, safety limiting) -- decorrelation
        // is added purely as a feed-forward, output-only stage below, so it
        // cannot disturb that already-tuned loop. shifter.processSample()
        // above (called to shift the tank's own feedback) also advanced a
        // second, quadrature-offset voice pair internally; read its
        // crossfaded output here.
        float quadratureRaw = shifter.getQuadratureOutput();

        // Runs its own independent LPC analysis of this signal (2026-08-29
        // stereo-width fix -- see FormantEnvelopeCorrector.h's
        // processQuadratureSample() comment) while still reusing
        // formantCorrector's dry-signal analysis from the primary call
        // above (dry genuinely is one shared signal between the two grain
        // pools, unlike their actual shifted output).
        quadratureRaw = formantCorrector.processQuadratureSample (quadratureRaw);

        // Own DCBlocker instance -- this class holds per-sample state
        // (previousInput/previousOutput) that would corrupt both signals if
        // shared with feedbackDcBlocker above.
        float quadratureDcBlocked = quadratureDcBlocker.processSample (quadratureRaw);

        // safetyLimiter is genuinely stateless (only a fixed threshold), so
        // reusing the same instance for both signals is safe.
        float quadratureSafe = safetyLimiter.processSample (quadratureDcBlocked);

        // L draws from the primary A/B voice pair's already-computed
        // safeFeedback, R from the quadrature C/D pair's safe signal -- both
        // pairs pitch-shift the identical tank content by the identical
        // ratio, so the decorrelation comes purely from the different
        // grain-phase offsets between the pairs, not from any difference in
        // source content or shift amount.
        // Phase 8 rework: inject only the L/R *difference* between the
        // primary and quadrature shifted signals (pure side-channel
        // decorrelation seasoning), not a raw full-strength copy summed into
        // both channels -- the old formula's common-mode component competed
        // with/masked the properly-diffused shimmer cascade already baked
        // into tankOut (see docs/shimmer-reverb-implementation-plan.md's
        // Phase 8 "Real gap 2" note). At width=0 this collapses to
        // wetLeft == wetRight == tankOut exactly, same as before.
        //
        // Bug fix (found by ear, 2026-08-21): sideShift is derived straight
        // from the shifter's output, which keeps running every sample
        // regardless of shimmerAmount -- so this term used to keep injecting
        // audible shifted content into the wet signal even at
        // shimmerAmount=0.0f (the tank's own recirculation was correctly
        // silenced, but this separate feed-forward term wasn't gated by the
        // same knob). Multiplying by shimmerAmount here makes "no shimmer"
        // mean no shimmer character anywhere in the output, not just in the
        // tank's recirculating budget.
        float sideShift = safeFeedback - quadratureSafe;
        float wetLeft = tankOut + shimmerWidthGain * shimmerAmount * 0.5f * sideShift;
        float wetRight = tankOut - shimmerWidthGain * shimmerAmount * 0.5f * sideShift;

        // Phase 9 Freeze follow-up (see FreezeLeveler.h): compensate the
        // wet signal's measured decay under sustained freeze, applied to
        // BOTH channels identically from a mono-summed measurement -- using
        // each channel's own independent envelope would let the Width
        // decorrelation term drift the stereo image as the two channels'
        // gains diverged, which isn't this stage's job. At freezeAmount=0.0f
        // this multiplies by exactly 1.0f (see FreezeLeveler's class
        // comment), so it's a no-op unless Freeze is actually engaged.
        const float freezeLevelerGain = freezeLeveler.computeGain (0.5f * (wetLeft + wetRight));
        wetLeft *= freezeLevelerGain;
        wetRight *= freezeLevelerGain;

        // Phase 10 Loop Freeze (see docs/shimmer-reverb-implementation-plan.md
        // and LoopCapture.h): a NEW, purely additive feature, deliberately
        // wired in strictly AFTER freezeLeveler's gain application above and
        // strictly BEFORE the dry/wet mix below -- so Loop Freeze's "live"
        // input is whatever Phase 9's Freeze mechanism currently outputs.
        // This is what lets the two features layer sensibly with zero
        // special-case interaction code: engaging both means Loop Freeze
        // captures and repeats a static snapshot of whatever the classic
        // drone currently sounds like, including its own gain compensation.
        // At loopFreezeAmount=0.0f (default) loopCapture.process() returns
        // its inputs completely unchanged, so this is a bit-identical no-op
        // unless Loop Freeze is actually engaged.
        const auto looped = loopCapture.process (wetLeft, wetRight);
        wetLeft = looped.first;
        wetRight = looped.second;

        // Phase 5: dry/wet mix + bypass. bypassed overrides mix rather than
        // combining with it -- forcing the EFFECTIVE mix to 0.0f (fully
        // dry) while bypassed, regardless of what setMix() was last set
        // to. This deliberately does NOT skip tank.processSample()/
        // shifter.processSample() above -- the whole recirculating chain
        // keeps running every sample even while bypassed, so an existing
        // tail rings out naturally through the blend below (fades toward
        // fully dry) instead of being hard-cut. Per-sample-accurate
        // smoothing of this transition is PluginProcessor's job (a later
        // Phase 5 step), not duplicated here -- see setBypassed()'s
        // comment in the header.
        const float effectiveMix = bypassed ? 0.0f : mix;

        // Dry samples are read directly off `block` here, immediately
        // before each channel's own sample is overwritten below -- `block`
        // still holds the untouched original per-channel input at this
        // point (the mono-sum pass above only READ from block into
        // monoScratch, it never wrote back into block; monoData[i] is a
        // mono-summed copy, not the original stereo signal). This preserves
        // the original stereo image for the dry portion of the blend
        // without needing a second buffer -- reading `block.getSample`
        // right before writing that exact (channel, sample) cell is
        // real-time-safe (no allocation) and correct because each channel's
        // write only touches that channel's own data.
        if (numChannels > 0)
        {
            const float dryLeft = block.getSample (0, (int) i);
            const float outLeft = dryLeft * (1.0f - effectiveMix) + wetLeft * effectiveMix;
            block.setSample (0, (int) i, outLeft);
        }

        for (size_t ch = 1; ch < numChannels; ++ch)
        {
            const float dryChannel = block.getSample ((int) ch, (int) i);
            const float outChannel = dryChannel * (1.0f - effectiveMix) + wetRight * effectiveMix;
            block.setSample ((int) ch, (int) i, outChannel);
        }
    }
}
