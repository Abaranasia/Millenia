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
    // setShimmerFeedback() happened to be called at least once. Re-push the
    // CURRENT shimmerFeedbackAmount (the member-initializer default, or a
    // caller's prior customization surviving a re-prepare, e.g. a host
    // sample-rate change) through the same mapping setShimmerFeedback()
    // itself uses, so an untouched engine's tank actually reflects the
    // intended default instead of silently diverging from it. Since
    // 2026-10-24 the same call also re-seeds the tank's blend law from
    // pitchShiftSemitones. applyPitchAndFreezeCrossfade() above already
    // pushed both; this explicit re-seed is kept (two stores) so the fix
    // this comment records cannot silently regress if that call moves.
    pushShimmerBlendToTank();

    // Infinite mode: same re-seed reasoning as shimmerFeedbackAmount above --
    // push the cached value so the tank reflects it after a re-prepare.
    // DattorroTank::reset() snaps its ramp to this target rather than
    // ramping, so a host sample-rate change while Infinite is on does not
    // replay a 0.2s fade-in.
    tank.setInfiniteAmount (infiniteAmount);

    monoScratch.setSize (1, (int) spec.maximumBlockSize);

    // Pre-delay line sized for the maximum once, here; the smoother's
    // reset() keeps a caller's earlier setPreDelayMs() target (it snaps the
    // current value to it), same survive-a-re-prepare rule as width/mix.
    preDelaySamplesPerMs = (float) (spec.sampleRate * 0.001);
    preDelayLine.setMaximumDelayInSamples ((int) std::ceil (maxPreDelayMs * preDelaySamplesPerMs) + 1);
    preDelayLine.prepare ({ spec.sampleRate, spec.maximumBlockSize, 1 });
    smoothedPreDelayMs.reset (spec.sampleRate, preDelayRampSeconds);

    // Formant Correction crossfade: same survive-a-re-prepare rule -- the
    // smoother's reset() keeps the target a caller set with
    // setFormantCorrection() (before or after an earlier prepare()) and
    // snaps the current weight to it.
    smoothedFormantWeight.reset (spec.sampleRate, formantRampSeconds);

    // Wet EQ: always two channels (wetLeft/wetRight), whatever the block's
    // channel count. Cutoffs above ~0.49 fs would break the TPT prewarp,
    // so they are capped at that for low sample rates.
    const juce::dsp::ProcessSpec stereoSpec { spec.sampleRate, spec.maximumBlockSize, 2 };
    lowCutFilter.setType (juce::dsp::StateVariableTPTFilterType::highpass);
    highCutFilter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    lowCutFilter.prepare (stereoSpec);
    highCutFilter.prepare (stereoSpec);
    maxFilterCutoffHz = (float) (0.49 * spec.sampleRate);

    // mix/bypassed/width are NOT re-applied here (unlike the
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
    preDelayLine.reset();
    smoothedPreDelayMs.setCurrentAndTargetValue (smoothedPreDelayMs.getTargetValue());
    lowCutFilter.reset();
    highCutFilter.reset();
    lowCutActive = false;
    highCutActive = false;

    // Unity-shifter idle skip (2026-10-10, see unityShifterActive's header
    // comment): unityShifter was just cleared above, so mark it idle --
    // process() then decides afresh, and if Freeze is already engaged it
    // takes the idle -> active edge on the next block and starts from this
    // clean state (one redundant clear of an already-clear buffer).
    unityShifterActive = false;
    lastUnityFeedback = 0.0f;

    // Snap, don't ramp, after a reset -- same convention as
    // DattorroTank::reset()'s Infinite ramp.
    processedFreezeAmount = freezeAmount;
    unityWeight = unityWeightTarget;

    // Formant Correction: snap the crossfade to its target, and mark the
    // corrector idle (it was just cleared above) so process() decides afresh
    // -- if correction is on, the next block takes the idle -> active edge
    // and clears it once more (redundant, harmless), same as unityShifter.
    smoothedFormantWeight.setCurrentAndTargetValue (smoothedFormantWeight.getTargetValue());
    formantCorrectorActive = false;
}

void ShimmerReverbEngine::setPitchShiftSemitones (float semitones)
{
    // Stored so applyPitchAndFreezeCrossfade() (shared with prepare()/
    // setFreezeAmount()) has one source of truth for the main shifter's ratio -- see
    // pitchShiftSemitones' header comment.
    pitchShiftSemitones = semitones;
    applyPitchAndFreezeCrossfade();
}

void ShimmerReverbEngine::setDecaySeconds (float newDecaySeconds)
{
    tank.setDecaySeconds (juce::jlimit (DattorroTank::minDecaySeconds, DattorroTank::maxDecaySeconds, newDecaySeconds));
}

void ShimmerReverbEngine::setSize (float newSize)
{
    tank.setSize (juce::jlimit (DattorroTank::minSize, DattorroTank::maxSize, newSize));
}

void ShimmerReverbEngine::setModulation (float newModulation)
{
    tank.setModulation (juce::jlimit (0.0f, 1.0f, newModulation));
}

void ShimmerReverbEngine::setDamping (float newDamping)
{
    tank.setDamping (newDamping);
}

void ShimmerReverbEngine::setShimmerAmount (float newShimmerAmount)
{
    shimmerAmount = juce::jlimit (0.0f, 1.0f, newShimmerAmount);
    tank.setShimmerAmountGain (shimmerAmount);
}

void ShimmerReverbEngine::setShimmerFeedback (float newAmount)
{
    // Cached locally so process()'s Width/decorrelation term can gate on it
    // directly too -- see this method's header comment for why (same
    // "process() needs its own gating copy" reasoning as shimmerAmount).
    shimmerFeedbackAmount = juce::jlimit (0.0f, 1.0f, newAmount);

    // Direct (un-inverted since 2026-10-24) mapping to DattorroTank -- see
    // this method's header comment and pushShimmerBlendToTank().
    pushShimmerBlendToTank();
}

void ShimmerReverbEngine::setLegacyShimmerBlendForTesting (bool shouldUseLegacy)
{
    legacyShimmerBlend = shouldUseLegacy;
    pushShimmerBlendToTank();
}

void ShimmerReverbEngine::pushShimmerBlendToTank()
{
    // Shimmer cascade slice 1 (2026-10-24, docs/shimmer-character-research.md
    // section 7.1 items 1 and 2). Mapping: Shimmer Feedback x IS the cap on
    // the recirculating shifted share (maxShimmerBlendWeight = x; at Amount
    // 1.0, shimmerWeight = x). Blend law: equal-power once the shift is a
    // semitone or more, where the shifted copy is uncorrelated with the
    // plain one; linear at |st| <= 0.5, where the copies correlate and
    // equal-power weights would sum above unity; a linear ramp between
    // (|st| 0.5 -> 1). The legacy switch restores the inverted 1 - x mapping
    // and the linear law exactly (T6's golden hashes).
    const float equalPowerAmount = legacyShimmerBlend
                                       ? 0.0f
                                       : juce::jlimit (0.0f, 1.0f, (std::abs (pitchShiftSemitones) - 0.5f) / 0.5f);
    tank.setMaxShimmerBlendWeight (legacyShimmerBlend ? 1.0f - shimmerFeedbackAmount : shimmerFeedbackAmount);
    tank.setShimmerBlendLaw (equalPowerAmount);
}

void ShimmerReverbEngine::setWidth (float newWidth)
{
    width = juce::jlimit (0.0f, maxWidth, newWidth);
}

void ShimmerReverbEngine::setPreDelayMs (float newPreDelayMs)
{
    smoothedPreDelayMs.setTargetValue (juce::jlimit (0.0f, maxPreDelayMs, newPreDelayMs));
}

void ShimmerReverbEngine::setLowCutHz (float newLowCutHz)
{
    lowCutHz = juce::jlimit (minLowCutHz, maxLowCutHz, newLowCutHz);
}

void ShimmerReverbEngine::setHighCutHz (float newHighCutHz)
{
    highCutHz = juce::jlimit (minHighCutHz, maxHighCutHz, newHighCutHz);
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

void ShimmerReverbEngine::setFormantCorrection (bool shouldCorrect)
{
    // Target only: process() ramps the weight per sample (formantRampSeconds)
    // and reset() snaps it. Called every block by PluginProcessor; an
    // unchanged target leaves a ramp in progress untouched.
    smoothedFormantWeight.setTargetValue (shouldCorrect ? 1.0f : 0.0f);
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

    // The tank's blend law follows the shift (see pushShimmerBlendToTank()).
    pushShimmerBlendToTank();
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

    // Unity-shifter idle skip (2026-10-10, see unityShifterActive's header
    // comment): decided once per block. Active whenever this block's freeze
    // ramp starts or ends above 0 -- so a ramp from a non-zero start down to
    // 0 still runs it for that whole block -- or the crossfade weight does
    // (redundant: unityWeight is always freezeAmount^16 of the same block
    // edge, but it makes "the weight is exactly 0.0f on every idle sample"
    // hold by construction instead of by that invariant). The idle -> active
    // edge clears unityShifter's delay line and grain state first, so it
    // cannot replay audio frozen in it since its last engagement.
    // PitchShifter::reset() keeps pitchRatio (0 semitones, set in prepare()),
    // freezeAmount (the drift depth, set by setFreezeAmount()) and the
    // configured delay; it only zeroes buffers and rewinds indices/phases
    // (juce::dsp::DelayLine::reset() is std::fill + AudioBuffer::clear()),
    // so it is allocation-free and audio-thread safe, and nothing needs
    // re-applying afterwards. Only the drift LFO's phase restarts at 0 on
    // each engagement instead of running in step with `shifter`'s.
    const bool unityNeeded = unityShifterForcedActive
                             || processedFreezeAmount > 0.0f || freezeAmount > 0.0f
                             || unityWeightStart > 0.0f || unityWeightTarget > 0.0f;
    if (unityNeeded && ! unityShifterActive)
        unityShifter.reset();
    unityShifterActive = unityNeeded;

    // Formant Correction idle skip (2026-10-25, see setFormantCorrection()
    // and formantCorrectorActive's header comment): decided once per block,
    // same pattern as the unity shifter above. Needed while the target is 1
    // or the crossfade weight is still ramping (a ramp down to 0 keeps it
    // running until the weight lands). While idle the corrector is not run
    // at all and the raw shifted signal is used as is -- bit-identical to no
    // corrector in the chain. The idle -> active edge clears it first
    // (FormantEnvelopeCorrector::reset() only std::fill()s pre-sized rings
    // and fills fixed arrays: allocation-free, audio-thread safe), so a
    // re-engage starts from the same clean state prepare() leaves.
    const bool formantNeeded = smoothedFormantWeight.isSmoothing() || smoothedFormantWeight.getTargetValue() > 0.0f;
    if (formantNeeded && ! formantCorrectorActive)
        formantCorrector.reset();
    formantCorrectorActive = formantNeeded;

    // Wet EQ, decided once per block (see lowCutActive's header comment).
    // Both filters run on every sample whatever this decides; only their
    // OUTPUT is gated. While inactive each sits at its own endpoint cutoff
    // (low cut 20 Hz, high cut min(20 kHz, 0.49 fs)), so on activation its
    // state already tracks the wet signal and the cutoff just moves on from
    // there. The output at the open endpoint is unused, which keeps the
    // default output bit-identical.
    lowCutActive = lowCutHz > minLowCutHz;
    highCutActive = highCutHz < maxHighCutHz;
    lowCutFilter.setCutoffFrequency (juce::jmin (lowCutHz, maxFilterCutoffHz));
    highCutFilter.setCutoffFrequency (juce::jmin (highCutHz, maxFilterCutoffHz));

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

        // Freeze level crossfade (2026-10-10, see applyPitchAndFreezeCrossfade()).
        // History: unityShifter first ran EVERY sample, even at
        // freezeAmount=0.0f where its output is weighted by exactly 0.0f, so
        // its ~80ms delay line (PitchShifter's baseDelaySamples) would already
        // hold real tank content when the Freeze toggle sweeps unityWeight up
        // in 50ms. That cost a second PitchShifter's per-sample work at all
        // times (engine processing/audio ~0.037 -> ~0.052-0.058 in Release).
        // Since the same day it only runs while unityShifterActive (see the
        // per-block decision above): at Freeze 0 it is skipped and 0.0f is
        // blended at a weight of exactly 0.0f, bit-identical to the always-on
        // output (golden hash test, checked both ways). Trade-off: each
        // engagement now starts from a cold (just-reset) unity path, silent
        // for its first ~80ms. Measured harmless at the current blend caps --
        // DattorroTank holds the shimmer path to ~1-2% of the loop by the
        // time unityWeight matters (f > 0.9): T1 and T2 (the level-dip
        // guard) read the same to 0.01dB at 128 and 512-sample blocks, T3's
        // 880Hz growth -3.94 -> -3.74dB (see
        // ShimmerReverbEngineFreezeCrossfadeTests). Revisit if
        // frozenMaxShimmerBlendWeight or the ^16 curve ever changes.
        const float unityFeedback = unityShifterActive ? unityShifter.processSample (dryFeedback) : 0.0f;
        lastUnityFeedback = unityFeedback;

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
        //
        // 2026-10-25: behind the Formant Correction toggle (default off, see
        // setFormantCorrection() for the measured instability). Linear
        // crossfade raw -> corrected by formantWeight, ramped per sample;
        // the quadrature path below uses the same weight at the same chain
        // position. At formantWeight == 1 the blend is the corrected sample
        // exactly (0 * raw + 1 * corrected), so "on" is bit-identical to the
        // always-on corrector; at 0 the raw sample is used directly, never
        // 0 * corrected, so a non-finite corrector output mid-ramp-out
        // cannot leak through.
        const float formantWeight = formantCorrectorActive ? smoothedFormantWeight.getNextValue() : 0.0f;
        if (formantCorrectorActive)
        {
            const float corrected = formantCorrector.processSample (dryFeedback, shiftedFeedback);
            if (formantWeight > 0.0f)
                shiftedFeedback = (1.0f - formantWeight) * shiftedFeedback + formantWeight * corrected;
        }

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

        // Pre-delay (2026-10-11, docs/reverb-space-research.md section 8.3):
        // delays the fresh input BEFORE the Freeze mute below, so a frozen
        // tail still mutes whatever is in flight in the line. The line runs
        // (push + pop) every sample so its read/write pointers stay in step
        // whatever the setting. At 0 ms (default) the raw input is used,
        // bit-identical to no pre-delay.
        const float preDelayNowMs = smoothedPreDelayMs.getNextValue();
        preDelayLine.pushSample (0, monoData[i]);
        const float preDelayed = preDelayLine.popSample (0, preDelayNowMs * preDelaySamplesPerMs);
        const float tankInput = preDelayNowMs > 0.0f ? preDelayed : monoData[i];

        // The shimmerAmount gain now lives inside DattorroTank itself (see
        // setShimmerAmountGain()) -- it scales safeFeedback as an
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
        float tankOut = tank.processSample (tankInput * (1.0f - freezeNow), safeFeedback);
        const float tankRight = tank.getLastRightOutput();

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
        // Same Formant Correction gate and crossfade weight as the primary
        // path above (one getNextValue() per sample, taken there).
        if (formantCorrectorActive)
        {
            const float correctedQuadrature = formantCorrector.processQuadratureSample (quadratureRaw);
            if (formantWeight > 0.0f)
                quadratureRaw = (1.0f - formantWeight) * quadratureRaw + formantWeight * correctedQuadrature;
        }

        // Same linear freeze crossfade as the primary path, at the same point
        // in the chain (after formant correction, before DC blocking), so
        // sideShift below still compares like with like.
        // 0.0f while idle, same reasoning as unityFeedback above -- the stale
        // getQuadratureOutput() of an idle shifter is never read.
        const float unityQuadrature = unityShifterActive ? unityShifter.getQuadratureOutput() : 0.0f;
        quadratureRaw = (1.0f - unityWeightNow) * quadratureRaw + unityWeightNow * unityQuadrature;

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
        // Phase 8 "Real gap 2" note). Width = 0 still gives L == R exactly
        // (see the M/S comment below).
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
        // setShimmerFeedback()'s comment, effect 2): gated by
        // shimmerFeedbackAmount too, same multiplicative-gate convention
        // shimmerAmount already established here (2026-08-21) -- at
        // shimmerFeedbackAmount=0.0f the shimmer side term is exactly 0, same
        // gate shimmerAmount=0.0f produces (the tank's own L/R stereo, added
        // 2026-10-11, is unaffected by either gate).
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
        const float shimmerSide = shimmerSideGain * shimmerAmount * shimmerFeedbackAmount * 0.5f * sideShift;
        lastTankLeft = tankOut;
        lastTankRight = tankRight;

        // Width as M/S on the true stereo tail (2026-10-11, docs/reverb-
        // space-research.md section 8.2):
        //   mid = 0.5 (L + R), side = 0.5 (L - R) + shimmerSide,
        //   L = mid + width * side, R = mid - width * side.
        // Expanded into per-channel gains rather than computed literally, so
        // the endpoints are exact in float: width 0 gives 0.5/0.5 on both
        // channels (L == R bit-for-bit), and width 1 gives 1.0/0.0, i.e.
        // wetLeft = tankOut + shimmerSide -- the old default wetLeft
        // expression bit-for-bit (shimmerSideGain is the old 0.15 default).
        const float directGain = 0.5f + 0.5f * width;
        const float crossGain = 0.5f - 0.5f * width;
        float wetLeft = (directGain * tankOut + crossGain * tankRight) + width * shimmerSide;
        float wetRight = (crossGain * tankOut + directGain * tankRight) - width * shimmerSide;

        // Phase 9 Freeze follow-up (see FreezeLeveler.h): compensate the
        // wet signal's measured decay under sustained freeze, applied to
        // BOTH channels identically from a mono-summed measurement -- using
        // each channel's own independent envelope would let the Width
        // decorrelation term drift the stereo image as the two channels'
        // gains diverged, which isn't this stage's job. At freezeAmount=0.0f
        // this multiplies by exactly 1.0f (see FreezeLeveler's class
        // comment), so it's a no-op unless Freeze is actually engaged.
        // Detection is on tankOut (the tank's left output), deliberately:
        // before the 2026-10-11 stereo tank, 0.5 * (wetLeft + wetRight) was
        // exactly tankOut (the shimmer side cancels), and docs/reverb-space-
        // research.md section 8.1 keeps detection on that same signal. The
        // stereo mid, 0.5 * (tankLeft + tankRight), would silently retune
        // the leveler to a different envelope.
        const float freezeLevelerGain = freezeLeveler.computeGain (tankOut);
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

        // Wet EQ (2026-10-11, docs/reverb-space-research.md section 8.4):
        // last stage of the wet path, after loop capture so a captured loop
        // is filtered live too, and before the dry/wet mix so the dry signal
        // is never touched. Both filters always run (see the per-block
        // decision above the loop); an inactive one passes its input through.
        //
        // Click fix (2026-10-11, review): this used to run a filter only while
        // active and reset() it on activation. A TPT lowpass started from
        // zero state outputs G * x on its first sample, stepping the wet
        // signal down by (1 - G) * x -- measured 0.84 peak step vs a 0.026
        // steady-state per-sample delta (32x) moving the high cut 20 kHz ->
        // 19 kHz at 96 kHz on a 220 Hz tone tail (ShimmerReverbEngineSpaceTests'
        // high cut activation click test). Always-running state removes it.
        const float lowCutLeft = lowCutFilter.processSample (0, wetLeft);
        const float lowCutRight = lowCutFilter.processSample (1, wetRight);
        if (lowCutActive)
        {
            wetLeft = lowCutLeft;
            wetRight = lowCutRight;
        }

        const float highCutLeft = highCutFilter.processSample (0, wetLeft);
        const float highCutRight = highCutFilter.processSample (1, wetRight);
        if (highCutActive)
        {
            wetLeft = highCutLeft;
            wetRight = highCutRight;
        }

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
