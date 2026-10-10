#include "DattorroTank.h"

DattorroTank::DattorroTank() = default;
DattorroTank::~DattorroTank() = default;

void DattorroTank::prepare (const juce::dsp::ProcessSpec& spec)
{
    // Every delay line/filter here is mono, same as ScratchSchroederTank —
    // true stereo decorrelation is deferred to Phase 4.
    juce::dsp::ProcessSpec monoSpec { spec.sampleRate, spec.maximumBlockSize, 1 };

    for (int i = 0; i < numDiffuserStages; ++i)
    {
        auto delayInSamples = juce::roundToInt (diffuserDelayMs[(size_t) i] * 0.001 * spec.sampleRate);
        diffuserStages[(size_t) i].delayLine.setMaximumDelayInSamples (delayInSamples + 1);
        diffuserStages[(size_t) i].delayLine.prepare (monoSpec);
        diffuserStages[(size_t) i].delayLine.setDelay ((float) delayInSamples);
        diffuserStages[(size_t) i].feedback = diffuserFeedback[(size_t) i];
    }

    prepareBranch (branchA, monoSpec, branchAAllpass1Ms, branchADelay1Ms, branchAAllpass2Ms, branchADelay2Ms);
    prepareBranch (branchB, monoSpec, branchBAllpass1Ms, branchBDelay1Ms, branchBAllpass2Ms, branchBDelay2Ms);

    auto msToSamples = [&spec] (float ms) { return ms * 0.001f * (float) spec.sampleRate; };
    outputTapBDelay1aSamples  = msToSamples (outputTapBDelay1aMs);
    outputTapBDelay1bSamples  = msToSamples (outputTapBDelay1bMs);
    outputTapBAllpass2Samples = msToSamples (outputTapBAllpass2Ms);
    outputTapBDelay2Samples   = msToSamples (outputTapBDelay2Ms);
    outputTapADelay1Samples   = msToSamples (outputTapADelay1Ms);
    outputTapAAllpass2Samples = msToSamples (outputTapAAllpass2Ms);
    outputTapADelay2Samples   = msToSamples (outputTapADelay2Ms);

    // Infinite mode (see setInfiniteAmount() and the header's infinite*/
    // mod*/level* constants). prepareBranch() above already gave both
    // allpass1 lines enough headroom for +modDepthSamples; their configured
    // (unmodulated) delay is captured here straight from the line itself so
    // processSample()'s "base + 0.0f" at infinite == 0 re-applies exactly the
    // same value setDelay() was given -- bit-identical by construction.
    allpass1BaseDelayA = branchA.allpass1.delayLine.getDelay();
    allpass1BaseDelayB = branchB.allpass1.delayLine.getDelay();
    modDepthSamples = msToSamples (modDepthMs);
    modPhaseIncrementA = juce::MathConstants<float>::twoPi * modRateHzA / (float) spec.sampleRate;
    modPhaseIncrementB = juce::MathConstants<float>::twoPi * modRateHzB / (float) spec.sampleRate;

    auto onePoleCoeff = [&spec] (float seconds) { return (float) std::exp (-1.0 / ((double) seconds * spec.sampleRate)); };
    levelAttackCoeff  = onePoleCoeff (levelAttackSeconds);
    levelReleaseCoeff = onePoleCoeff (levelReleaseSeconds);
    levelGainCoeff    = onePoleCoeff (levelGainSmoothingSeconds);

    // reset(sampleRate, ramp) snaps the current value to the existing
    // target, so a caller's setInfiniteAmount() survives a re-prepare (e.g.
    // a host sample-rate change) -- ShimmerReverbEngine::prepare() re-seeds
    // it anyway, mirroring its shimmerSustain re-seed.
    smoothedInfiniteAmount.reset (spec.sampleRate, infiniteRampSeconds);

    reset();
}

void DattorroTank::prepareBranch (TankBranch& branch, const juce::dsp::ProcessSpec& monoSpec,
                                   float allpass1Ms, float delay1Ms, float allpass2Ms, float delay2Ms)
{
    auto sizeDelay = [&monoSpec] (auto& delay, float delayMs, int extraHeadroomSamples = 0)
    {
        auto delayInSamples = juce::roundToInt (delayMs * 0.001 * monoSpec.sampleRate);
        delay.setMaximumDelayInSamples (delayInSamples + 1 + extraHeadroomSamples);
        delay.prepare (monoSpec);
        delay.setDelay ((float) delayInSamples);
    };

    // Infinite mode modulates allpass1's read delay by up to +modDepthMs
    // (see processSample()), so its maximum must cover delay + depth. A
    // larger buffer does not change what any read returns (reads are
    // relative to the write pointer), so the default sound is unaffected.
    const int modHeadroomSamples = (int) std::ceil (modDepthMs * 0.001 * monoSpec.sampleRate) + 1;
    sizeDelay (branch.allpass1.delayLine, allpass1Ms, modHeadroomSamples);
    branch.allpass1.feedback = branchAllpass1Feedback;

    sizeDelay (branch.delay1, delay1Ms);

    sizeDelay (branch.allpass2.delayLine, allpass2Ms);
    branch.allpass2.feedback = branchAllpass2Feedback;

    sizeDelay (branch.delay2, delay2Ms);
}

void DattorroTank::reset()
{
    for (auto& stage : diffuserStages)
        stage.delayLine.reset();

    resetBranch (branchA);
    resetBranch (branchB);

    feedbackFromB = 0.0f;

    // Infinite mode state: snap the ramp to its target (keeps a caller's
    // setting, drops any in-flight ramp), restart both LFOs at their fixed
    // phases, and clear the level controller back to unity gain.
    smoothedInfiniteAmount.setCurrentAndTargetValue (smoothedInfiniteAmount.getTargetValue());
    modPhaseA = 0.0f;
    modPhaseB = modPhaseOffsetB;
    levelEnvelope = 0.0f;
    levelGain = 1.0f;
}

void DattorroTank::resetBranch (TankBranch& branch)
{
    branch.allpass1.delayLine.reset();
    branch.delay1.reset();
    branch.dampState = 0.0f;
    branch.allpass2.delayLine.reset();
    branch.delay2.reset();
}

void DattorroTank::setDamping (float newDampingCoefficient)
{
    dampingCoefficient = newDampingCoefficient;
}

void DattorroTank::setDecay (float newDecayGain)
{
    decayGain = newDecayGain;
}

void DattorroTank::setShimmerFeedbackGain (float newShimmerFeedbackGain)
{
    shimmerFeedbackGain = newShimmerFeedbackGain;
}

void DattorroTank::setMaxShimmerBlendWeight (float newMaxShimmerBlendWeight)
{
    maxShimmerBlendWeight = newMaxShimmerBlendWeight;
}

void DattorroTank::setFreezeAmount (float newFreezeAmount)
{
    freezeAmount = newFreezeAmount;
}

void DattorroTank::setInfiniteAmount (float newInfiniteAmount)
{
    smoothedInfiniteAmount.setTargetValue (newInfiniteAmount);
}

template <typename Stage>
float DattorroTank::processAllpass (float input, Stage& stage)
{
    // Correct one-multiply Schroeder allpass (Julius O. Smith's "Schroeder
    // Allpass Sections" form): w[n] = x[n] + g*w[n-M]; y[n] = -g*w[n] + w[n-M].
    // This differs from the Freeverb-style "-input + bufOut" formula (used by
    // Phase 1's ScratchSchroederTank::processAllpass, copied here) which is
    // ONLY a true allpass at one specific feedback value (g = (sqrt(5)-1)/2);
    // for any other g it has magnitude > 1 at frequencies near the delay
    // line's Nyquist-within-period point, which is harmless in Phase 1's
    // feed-forward-only use but explodes once these allpasses sit inside
    // DattorroTank's recirculating branch/cross-feed loop (see
    // DattorroTankTests.cpp for the reproduction and the class-level comment
    // above for the failure mode).
    float bufOut = stage.delayLine.popSample (0);
    float w = input + bufOut * stage.feedback;
    float output = bufOut - w * stage.feedback;
    stage.delayLine.pushSample (0, w);
    return output;
}

float DattorroTank::processDelay (float input, DelayLineType& delay)
{
    float delayed = delay.popSample (0);
    delay.pushSample (0, input);
    return delayed;
}

float DattorroTank::processBranch (float input, TankBranch& branch)
{
    float out = processAllpass (input, branch.allpass1);
    out = processDelay (out, branch.delay1);

    // One-pole leaky integrator, matching Dattorro's actual damping stage
    // (see the defaultDampingCoefficient comment in the header): the
    // *previous* smoothed sample is weighted by dampingCoefficient, the
    // *new* incoming sample by (1 - dampingCoefficient). At the reference's
    // ~0.0005 default this is barely more than a pass-through -- most of
    // the tail's eventual darkening comes from many recirculations, not
    // heavy loss on any single pass.
    branch.dampState = dampingCoefficient * branch.dampState + (1.0f - dampingCoefficient) * out;
    out = branch.dampState;

    out = processAllpass (out, branch.allpass2);
    out = processDelay (out, branch.delay2);
    return out;
}

float DattorroTank::peekTap (DelayLineType& delay, float offsetSamples)
{
    // See the declaration-site comment: popSample's explicit-delayInSamples
    // overload calls setDelay() internally even when updateReadPointer is
    // false, so it would otherwise permanently shorten this delay line's
    // configured length for every later recirculating popSample(-1) call.
    // Save/restore keeps this strictly a non-destructive output-only peek.
    const float configuredDelay = delay.getDelay();
    float tapped = delay.popSample (0, offsetSamples, false);
    delay.setDelay (configuredDelay);
    return tapped;
}

float DattorroTank::processSample (float input, float externalFeedback)
{
    float diffused = input;

    for (auto& stage : diffuserStages)
        diffused = processAllpass (diffused, stage);

    // Structural fix, second pass (docs/shimmer-reverb-implementation-plan.md's
    // Phase 8 note, 2026-08-18): externalFeedback claims a capped SHARE of
    // the tank's fixed recirculation budget, crossfaded against the tank's
    // own natural feedbackFromB, rather than being added as an independent
    // extra gain on top. Since shimmerWeight + plainWeight is always exactly
    // 1.0, the combined magnitude of (plainWeight*feedbackFromB +
    // shimmerWeight*externalFeedback) can never exceed
    // max(|feedbackFromB|, |externalFeedback|) -- so the TOTAL feedback gain
    // reaching the tank stays bounded by decayGain alone (the same ceiling
    // Phase 4 already validated safe, decayGain <= 0.85), for ANY
    // shimmerFeedbackGain setting. The first attempt at this fix (a plain
    // additive sum, decayGain*feedbackFromB + shimmerFeedbackGain*
    // externalFeedback) let the two gains add independently, so combined
    // loop gain could reach decayGain + shimmerFeedbackGain (~1.7 at
    // defaults) -- past unity, kept only technically bounded by
    // SafetyLimiter's hard clipping, which produced the reported
    // "oscillating, unnatural, psychedelic" character. capping
    // shimmerWeight at maxShimmerBlendWeight (0.5) also guarantees at least
    // half the recirculating budget always stays on the plain/unshifted
    // path even at shimmerAmount's maximum, which is what prevents full
    // geometric pitch-compounding (the original "chipmunk" bug) from
    // reappearing.
    // Phase 9 Freeze (see docs/shimmer-reverb-implementation-plan.md and
    // frozenDecayGain's comment in the header): blends the live decayGain
    // toward frozenDecayGain as freezeAmount goes 0 -> 1, so at
    // freezeAmount=0.0f effectiveDecayGain is bit-identical to decayGain
    // (no behavior change) and at 1.0f both cross-feed sums below
    // recirculate at the near-unity frozen gain instead. Computed once and
    // reused for both the A and B cross-feed sums -- Phase 4's validated
    // decayGain<=0.85 stability work assumed strictly <1.0 and never
    // exercised this near-unity case, hence the dedicated freeze stability
    // test in DattorroTankTests.cpp.
    float effectiveDecayGain = decayGain + freezeAmount * (frozenDecayGain - decayGain);

    // Infinite mode (see setInfiniteAmount() and the header's infinite*
    // constants). One ramp value per sample drives all three mechanisms.
    // Every one is written so infinite == 0.0f is an exact no-op:
    // x + 0.0f*(...) == x, 1.0f + 0.0f*(...) == 1.0f, and a*1.0f == a are all
    // exact in IEEE float, so the arithmetic below reproduces the
    // pre-Infinite values bit-for-bit (verified by DattorroTankTests.cpp's
    // "Infinite OFF is bit-identical" test, not just argued).
    const float infiniteAmount = smoothedInfiniteAmount.getNextValue();

    // (a) decay pin toward exactly 1.0, applied on top of Freeze's own pin.
    effectiveDecayGain = effectiveDecayGain + infiniteAmount * (infiniteDecayGain - effectiveDecayGain);

    // (c) level controller gain, from the follower state updated at the end
    // of the previous sample (see below).
    const float levelGainEffective = 1.0f + infiniteAmount * (levelGain - 1.0f);
    const float crossFeedGain = effectiveDecayGain * levelGainEffective;

    // (b) allpass1 read-delay modulation. The offset is only computed when
    // Infinite is actually ramped in, so at 0.0f each line gets back
    // exactly the base delay prepare() configured (base + 0.0f == base).
    float modOffsetA = 0.0f, modOffsetB = 0.0f;

    if (infiniteAmount > 0.0f)
    {
        const float depth = infiniteAmount * modDepthSamples;
        modOffsetA = depth * std::sin (modPhaseA);
        modOffsetB = depth * std::sin (modPhaseB);
    }

    branchA.allpass1.delayLine.setDelay (allpass1BaseDelayA + modOffsetA);
    branchB.allpass1.delayLine.setDelay (allpass1BaseDelayB + modOffsetB);

    // Freeze, second mechanism (see frozenMaxShimmerBlendWeight's header
    // comment for the full measured history): crossfades the shimmerWeight
    // CAP toward a smaller value as freezeAmount goes 0->1, reducing (not
    // eliminating) how much of the loop's energy takes the comb-filtering-
    // prone detour through the shimmer path once frozen, without forcing
    // plainWeight all the way to the proven-unstable 1.0.
    const float effectiveMaxShimmerBlendWeight = maxShimmerBlendWeight
                                                      + freezeAmount * (frozenMaxShimmerBlendWeight - maxShimmerBlendWeight);
    const float shimmerWeight = shimmerFeedbackGain * effectiveMaxShimmerBlendWeight;
    const float plainWeight = 1.0f - shimmerWeight;
    float inputToA = diffused + crossFeedGain * (plainWeight * feedbackFromB + shimmerWeight * externalFeedback);
    float tankA_out = processBranch (inputToA, branchA);

    float inputToB = diffused + crossFeedGain * tankA_out;
    float tankB_out = processBranch (inputToB, branchB);

    feedbackFromB = tankB_out;

    // (c) Infinite's level controller, updated after feedbackFromB is
    // latched so next sample's crossFeedGain sees it. Runs unconditionally
    // (cheap, and keeps the follower warm so engaging Infinite on an
    // already-loud tail reacts at once), but only reaches the output through
    // levelGainEffective above, which is exactly 1.0f at infinite == 0.0f.
    {
        const float absFeedback = std::abs (feedbackFromB);
        const float envCoeff = absFeedback > levelEnvelope ? levelAttackCoeff : levelReleaseCoeff;
        levelEnvelope = envCoeff * levelEnvelope + (1.0f - envCoeff) * absFeedback;

        const float targetGain = levelEnvelope > levelCeiling ? levelCeiling / levelEnvelope : 1.0f;
        levelGain = levelGainCoeff * levelGain + (1.0f - levelGainCoeff) * targetGain;
    }

    // (b) advance both LFOs, wrapped to keep float phase precision.
    modPhaseA += modPhaseIncrementA;
    modPhaseB += modPhaseIncrementB;

    if (modPhaseA >= juce::MathConstants<float>::twoPi) modPhaseA -= juce::MathConstants<float>::twoPi;
    if (modPhaseB >= juce::MathConstants<float>::twoPi) modPhaseB -= juce::MathConstants<float>::twoPi;

    // Real Dattorro output tap formula (see the outputTap* constants in
    // the header): seven roughly-equal-weight taps with alternating
    // signs, called after both processBranch() calls and after
    // feedbackFromB is latched so they can never influence tankA_out,
    // tankB_out, or the recirculating cross-feed -- output-only.
    // tankA_out/tankB_out are used purely for recirculation now, not
    // summed directly into the output.
    float tankOut = outputScale * (
          peekTap (branchB.delay1, outputTapBDelay1aSamples)
        + peekTap (branchB.delay1, outputTapBDelay1bSamples)
        - peekTap (branchB.allpass2.delayLine, outputTapBAllpass2Samples)
        + peekTap (branchB.delay2, outputTapBDelay2Samples)
        - peekTap (branchA.delay1, outputTapADelay1Samples)
        - peekTap (branchA.allpass2.delayLine, outputTapAAllpass2Samples)
        - peekTap (branchA.delay2, outputTapADelay2Samples)
    );

    return tankOut;
}

void DattorroTank::process (juce::dsp::AudioBlock<float>& block)
{
    const auto numSamples = block.getNumSamples();
    const auto numChannels = block.getNumChannels();

    for (size_t i = 0; i < numSamples; ++i)
    {
        float monoIn = 0.0f;

        for (size_t ch = 0; ch < numChannels; ++ch)
            monoIn += block.getSample ((int) ch, (int) i);

        if (numChannels > 1)
            monoIn /= (float) numChannels;

        float tankOut = processSample (monoIn);

        for (size_t ch = 0; ch < numChannels; ++ch)
            block.setSample ((int) ch, (int) i, tankOut);
    }
}
