#include "ShimmerReverbEngine.h"

ShimmerReverbEngine::ShimmerReverbEngine() = default;
ShimmerReverbEngine::~ShimmerReverbEngine() = default;

void ShimmerReverbEngine::prepare (const juce::dsp::ProcessSpec& spec)
{
    tank.prepare (spec);
    shifter.prepare (spec);
    unityShifter.prepare (spec);
    feedbackDcBlocker.prepare (spec);
    safetyLimiter.prepare (spec);
    quadratureDcBlocker.prepare (spec);
    freezeLeveler.prepare (spec);
    formantCorrector.prepare (spec);
    loopCapture.prepare (spec);

    // Freeze level crossfade (2026-10-10, see applyPitchAndFreezeCrossfade()): the
    // unity path is permanently a plain 0-semitone (ratio 1.0) grain delay,
    // set once here and never changed. Its freeze drift is re-seeded too, so
    // a re-prepare() while frozen keeps the same dither the main shifter
    // gets.
    unityShifter.setPitchShiftSemitones (0.0f);
    unityShifter.setFreezeAmount (freezeAmount);

    // Phase 5: seed the live-settable pitch shift with the same value that
    // used to be a one-time hardcoded constant, so behavior is unchanged
    // until a caller actually calls setPitchShiftSemitones() with something
    // different. Goes through the member (not shifter directly) so a
    // re-prepare() while already frozen (e.g. a host sample-rate change)
    // also re-derives the freeze crossfade weight (unityWeight) -- see
    // pitchShiftSemitones' header comment.
    pitchShiftSemitones = defaultPitchShiftSemitones;
    applyPitchAndFreezeCrossfade();

    // "Shimmer Sustain" task (2026-09-06, found by a real test failure):
    // DattorroTank owns its OWN independent maxShimmerBlendWeight member with
    // its own hardcoded default (0.85f) -- unlike pitchShiftSemitones above,
    // tank.prepare()/reset() never touches it, so an untouched
    // ShimmerReverbEngine's tank stayed at THAT stale default forever unless
    // setShimmerSustain() happened to be called at least once. Re-push the
    // CURRENT shimmerSustainAmount (the member-initializer default, or a
    // caller's prior customization surviving a re-prepare, e.g. a host
    // sample-rate change) through the same inverted mapping
    // setShimmerSustain() itself uses, so an untouched engine's tank actually
    // reflects the intended default instead of silently diverging from it.
    tank.setMaxShimmerBlendWeight (1.0f - shimmerSustainAmount);

    // Infinite mode: same re-seed reasoning as shimmerSustainAmount above --
    // push the cached value so the tank reflects it after a re-prepare.
    // DattorroTank::reset() snaps its ramp to this target rather than
    // ramping, so a host sample-rate change while Infinite is on does not
    // replay a 0.2s fade-in.
    tank.setInfiniteAmount (infiniteAmount);

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
    unityShifter.reset();
    feedbackDcBlocker.reset();
    safetyLimiter.reset();
    quadratureDcBlocker.reset();
    freezeLeveler.reset();
    formantCorrector.reset();
    loopCapture.reset();

    // Snap, don't ramp, after a reset -- same convention as
    // DattorroTank::reset()'s Infinite ramp.
    processedFreezeAmount = freezeAmount;
    unityWeight = unityWeightTarget;
}

void ShimmerReverbEngine::setPitchShiftSemitones (float semitones)
{
    // Stored so applyPitchAndFreezeCrossfade() (shared with prepare()/
    // setFreezeAmount()) has one source of truth for the main shifter's ratio -- see
    // pitchShiftSemitones' header comment.
    pitchShiftSemitones = semitones;
    applyPitchAndFreezeCrossfade();
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

void ShimmerReverbEngine::setShimmerSustain (float newAmount)
{
    // Cached locally so process()'s Width/decorrelation term can gate on it
    // directly too -- see this method's header comment for why (same
    // "process() needs its own gating copy" reasoning as shimmerAmount).
    shimmerSustainAmount = juce::jlimit (0.0f, 1.0f, newAmount);

    // Inverted mapping to DattorroTank -- see this method's header comment
    // for why: higher shimmerSustainAmount (more user-facing "sustain") means
    // LOWER maxShimmerBlendWeight (less of the lossy, separately-delayed
    // shimmer path recirculating).
    tank.setMaxShimmerBlendWeight (1.0f - shimmerSustainAmount);
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

    // Also re-applied per sample inside process() (ramping from the previous
    // block's value, see processedFreezeAmount's header comment); set here
    // too so the tank is consistent with the target between blocks.
    // FreezeLeveler and the shifters' freeze drift stay per-block: neither
    // measurably contributed to the toggle artifact (disabling the drift
    // moved T1's 512-sample-block figure by 0.04dB).
    tank.setFreezeAmount (freezeAmount);
    freezeLeveler.setFreezeAmount (freezeAmount);
    shifter.setFreezeAmount (freezeAmount);
    unityShifter.setFreezeAmount (freezeAmount);

    // Re-derive the freeze crossfade weight (unityWeight) against the
    // just-updated freezeAmount -- PluginProcessor calls
    // setPitchShiftSemitones() then setFreezeAmount() every block (see
    // pitchShiftSemitones' header comment for why this crossfade exists at
    // all), so this call is what actually applies each block's freeze amount
    // rather than lagging one block behind it.
    applyPitchAndFreezeCrossfade();
}

void ShimmerReverbEngine::setInfiniteAmount (float newAmount)
{
    infiniteAmount = juce::jlimit (0.0f, 1.0f, newAmount);
    tank.setInfiniteAmount (infiniteAmount);
}

void ShimmerReverbEngine::setLoopFreezeAmount (float newAmount)
{
    loopCapture.setLoopFreezeAmount (juce::jlimit (0.0f, 1.0f, newAmount));
}

void ShimmerReverbEngine::setLoopLengthMs (float newLoopLengthMs)
{
    loopCapture.setLoopLengthMs (newLoopLengthMs);
}

void ShimmerReverbEngine::applyPitchAndFreezeCrossfade()
{
    // Freeze = a LEVEL crossfade between two shifters, never a ratio glide
    // (2026-10-10 -- see freezeUnityCrossfadeCurve's header comment for the
    // history, including the two 2026-08 curve bugs of the ratio-crossfade
    // design this replaced). The main shifter always runs at the full user
    // shift; process() fades its output out and unityShifter's (a 0-semitone
    // grain delay) in by unityWeight. Nothing ever passes through an
    // intermediate ratio, so a fast Freeze toggle can no longer produce the
    // old "chipmunk / DJ-scratch" pitch-bend.
    //
    // Anti-cascade guarantee unchanged: at freezeAmount=1.0, unityWeight is
    // exactly 1.0f, so only unshifted content re-enters the near-lossless
    // frozen loop -- equivalent to the old design's shifter at ratio exactly
    // 1.0 with the formant corrector in passthrough. Default sound
    // unchanged: at freezeAmount=0.0f, std::pow(0, 16) is exactly 0.0f, and
    // process()'s blend reduces to the main path bit-for-bit.
    //
    // Computed here, once per setter call (in practice once per block, via
    // PluginProcessor's smoothedFreeze.skip()), not per sample. This is the
    // block's TARGET; process() ramps unityWeight to it linearly across the
    // block (see unityWeightTarget's header comment for why a per-block step
    // was not enough).
    unityWeightTarget = std::pow (freezeAmount, freezeUnityCrossfadeCurve);

    shifter.setPitchShiftSemitones (pitchShiftSemitones);

    // Chipmunk-mitigation, LPC-based formant correction (see
    // FormantEnvelopeCorrector.h): keyed to the shift actually happening on
    // the MAIN path, which is now always the full user value (the freeze
    // crossfade no longer touches the main shifter's ratio). Still a
    // bit-identical passthrough at semitones<=0, same convention as every
    // other conditional DSP stage in this codebase. The unity path bypasses
    // the corrector entirely: a 0-semitone shift has no formant
    // displacement to correct.
    formantCorrector.setPitchShiftSemitones (pitchShiftSemitones);
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

    // Freeze per-sample ramps (2026-10-10, see processedFreezeAmount's and
    // unityWeightTarget's header comments): freezeAmount (for the input mute
    // and the tank) and the unity crossfade weight both move linearly from
    // the value the previous block ended on to this block's target, instead
    // of stepping at the block boundary. When start == target -- always,
    // outside a Freeze move -- every sample's value is exactly that value
    // (start + 0.0f * k == start), so constant-freeze behavior, including
    // freezeAmount=0's default sound, stays bit-identical.
    const float freezeStart = processedFreezeAmount;
    const float freezeDelta = freezeAmount - freezeStart;
    const float unityWeightStart = unityWeight;
    const float unityWeightDelta = unityWeightTarget - unityWeightStart;
    const float rampStep = numSamples > 0 ? 1.0f / (float) numSamples : 0.0f;

    for (size_t i = 0; i < numSamples; ++i)
    {
        // Last sample lands exactly on the targets, so no float drift carries
        // into the next block.
        const bool lastSample = (i + 1 == numSamples);
        const float rampPosition = rampStep * (float) (i + 1);
        const float freezeNow = lastSample ? freezeAmount : freezeStart + freezeDelta * rampPosition;
        const float unityWeightNow = lastSample ? unityWeightTarget : unityWeightStart + unityWeightDelta * rampPosition;

        // Per-sample, not per-block: DattorroTank::processSample() derives
        // effectiveDecayGain and the shimmer blend cap from freezeAmount on
        // every sample anyway, so this costs one store.
        tank.setFreezeAmount (freezeNow);

        // Shift the tank's OWN recirculating signal before it re-enters the
        // tank, instead of running a separate parallel feedback path -- this
        // matches shimmer-reverb-architecture.md's documented topology
        // (tank output -> pitch shifter -> back into tank input) and makes
        // the shimmer the tank's actual sustain mechanism rather than a
        // weak side-loop competing with the tank's own unshifted decayGain
        // recirculation.
        float dryFeedback = tank.peekFeedbackSignal();
        float shiftedFeedback = shifter.processSample (dryFeedback);

        // Freeze level crossfade (2026-10-10, see applyPitchAndFreezeCrossfade()):
        // unityShifter runs EVERY sample, even at freezeAmount=0.0f where its
        // output is multiplied by exactly 0.0f below. Its ~80ms internal
        // delay line (PitchShifter's baseDelaySamples) must already hold real
        // tank content when the Freeze toggle sweeps unityWeight up in 50ms
        // -- skipping it while unused would make the unity path come in
        // silent and dip the loop's level right as it takes over. The cost
        // is a second PitchShifter's per-sample work at all times: measured
        // engine processing/audio ratio ~0.037 -> ~0.052 in Release (see the
        // real-time-factor diagnostic in ShimmerReverbEngineTests.cpp).
        // Measured caveat (T2's comment): at default settings a cold unity
        // path did not show up as a dip, since DattorroTank caps the shimmer
        // path at ~1-2% of the loop by the time unityWeight matters -- kept
        // anyway as the safe default for any future blend-cap change.
        const float unityFeedback = unityShifter.processSample (dryFeedback);

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

        // Freeze level crossfade, primary path. LINEAR, not equal-power: the
        // blend's amplitude can never exceed the larger of its two inputs, so
        // the externalFeedback DattorroTank receives stays exactly as bounded
        // as with one shifter -- its shimmerWeight + plainWeight == 1
        // stability argument depends on that. An equal-power blend could
        // reach ~1.41x on correlated content. Bit-identical at
        // unityWeightNow == 0.0f: 1.0f * x + 0.0f * y == x for any finite y.
        shiftedFeedback = (1.0f - unityWeightNow) * shiftedFeedback + unityWeightNow * unityFeedback;

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
        // Infinite mode deliberately adds NO input scaling here -- fresh
        // input must keep layering on top of the never-decaying tail (see
        // setInfiniteAmount()). With Freeze also engaged, this Freeze mute
        // still wins, unchanged.
        // freezeNow, not freezeAmount: per-sample ramp, see above the loop.
        float tankOut = tank.processSample (monoData[i] * (1.0f - freezeNow), safeFeedback);

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

        // Same linear freeze crossfade as the primary path, at the same point
        // in the chain (after formant correction, before DC blocking), so
        // sideShift below still compares like with like.
        quadratureRaw = (1.0f - unityWeightNow) * quadratureRaw + unityWeightNow * unityShifter.getQuadratureOutput();

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
        // "Shimmer Sustain" task (2026-09-06 widening, see
        // setShimmerSustain()'s comment, effect 2): gated by
        // shimmerSustainAmount too, same multiplicative-gate convention
        // shimmerAmount already established here (2026-08-21) -- at
        // shimmerSustainAmount=0.0f this collapses to wetLeft == wetRight ==
        // tankOut exactly, same "gate to mono" behavior shimmerAmount=0.0f
        // and width=0.0f already produce individually.
        // Safety net, 2026-09-06 ("Shimmer Sustain" glitch investigation):
        // safeFeedback/quadratureSafe are each individually bounded by
        // safetyLimiter above, but their DIFFERENCE never was -- normally
        // harmless (well under threshold), but root-caused via a real
        // measurement pass (not guessed): at high Shimmer Sustain (which
        // drives DattorroTank's maxShimmerBlendWeight toward 0.0, removing
        // shimmer-cascade content from the tank's own recirculation) the dry
        // signal reaching PitchShifter becomes close enough to periodic that
        // the primary/quadrature grain pools' INDEPENDENT alignment searches
        // can settle on drastically different, comparably-scoring offsets
        // (measured up to ~265 samples apart, ~178 samples average, over a
        // sustained tone -- see Tests/Source/ShimmerReverbEngineTests.cpp's
        // divergence diagnostic) -- this is the same near-tied-peaks
        // correlation ambiguity PitchShifter::findAlignmentOffset()'s own
        // class comment documents as a previously-investigated,
        // NOT-fixed-at-that-layer issue (four separate fix attempts there
        // all failed or broke something else -- see that function's revert
        // history). Rather than a fifth attempt at the alignment-search
        // layer itself, safetyLimiter (already proven stateless/safe to
        // reuse per its own class comment) catches the resulting
        // near-full-scale antiphase excursion here, at the one place it
        // actually reaches the audible output -- transparent in ordinary
        // operation (typical sideShift magnitude is well under its 0.8f
        // threshold) and only engages during the pathological case.
        float sideShift = safetyLimiter.processSample (safeFeedback - quadratureSafe);
        float wetLeft = tankOut + shimmerWidthGain * shimmerAmount * shimmerSustainAmount * 0.5f * sideShift;
        float wetRight = tankOut - shimmerWidthGain * shimmerAmount * shimmerSustainAmount * 0.5f * sideShift;

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

    if (numSamples > 0)
    {
        processedFreezeAmount = freezeAmount;
        unityWeight = unityWeightTarget;
    }
}
