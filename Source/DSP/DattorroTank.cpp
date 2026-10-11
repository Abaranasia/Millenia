#include "DattorroTank.h"
#include <cmath>
#include <limits>

DattorroTank::DattorroTank() = default;
DattorroTank::~DattorroTank() = default;

void DattorroTank::prepare (const juce::dsp::ProcessSpec& spec)
{
    // Every delay line/filter here is mono, same as ScratchSchroederTank --
    // stereo comes only from the two output tap sets reading this one tank.
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
    for (size_t i = 0; i < outputTapMs.size(); ++i)
        outputTapBaseSamples[i] = msToSamples (outputTapMs[i]);

    // Infinite mode and user Modulation (see setInfiniteAmount()/
    // setModulation() and the header's mod* constants). prepareBranch()
    // above already gave both allpass1 lines headroom for the larger of the
    // two depths at maxSize. Their unmodulated centre is the branch's
    // allpass1Samples (the whole-sample Size-1 length times Size), so
    // processSample()'s "centre + 0.0f" with no modulation re-applies
    // exactly the value setDelay() was given -- bit-identical by
    // construction.
    modDepthSamples = msToSamples (modDepthMs);
    userModDepthSamples = msToSamples (maxModulationDepthMs);
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

    // Same "keep the caller's target, snap to it" rule for Size and
    // Modulation; reset() below then scales the lines to that Size.
    smoothedSize.reset (spec.sampleRate, sizeRampSeconds);
    smoothedModulation.reset (spec.sampleRate, modulationRampSeconds);

    reset();
}

void DattorroTank::prepareBranch (TankBranch& branch, const juce::dsp::ProcessSpec& monoSpec,
                                   float allpass1Ms, float delay1Ms, float allpass2Ms, float delay2Ms)
{
    // Returns the whole-sample Size-1 length. Each line is allocated for
    // that length times maxSize (slice 2), so Size never reallocates; a
    // larger buffer does not change what any read returns (reads are
    // relative to the write pointer), so Size 1 is unaffected.
    auto sizeDelay = [&monoSpec] (auto& delay, float delayMs, int extraHeadroomSamples = 0)
    {
        auto delayInSamples = juce::roundToInt (delayMs * 0.001 * monoSpec.sampleRate);
        delay.setMaximumDelayInSamples ((int) std::ceil ((float) delayInSamples * maxSize) + 1 + extraHeadroomSamples);
        delay.prepare (monoSpec);
        delay.setDelay ((float) delayInSamples);
        return (float) delayInSamples;
    };

    // Infinite mode and user Modulation move allpass1's read delay by up
    // to +-max(modDepthMs, maxModulationDepthMs) around its centre (see
    // processSample()), so its maximum must cover the longest centre plus
    // that depth.
    const float maxDepthMs = juce::jmax (modDepthMs, maxModulationDepthMs);
    const int modHeadroomSamples = (int) std::ceil (maxDepthMs * 0.001 * monoSpec.sampleRate) + 1;
    branch.allpass1BaseSamples = sizeDelay (branch.allpass1.delayLine, allpass1Ms, modHeadroomSamples);
    branch.allpass1.feedback = branchAllpass1Feedback;

    branch.delay1BaseSamples = sizeDelay (branch.delay1, delay1Ms);

    branch.allpass2BaseSamples = sizeDelay (branch.allpass2.delayLine, allpass2Ms);
    branch.allpass2.feedback = branchAllpass2Feedback;

    branch.delay2BaseSamples = sizeDelay (branch.delay2, delay2Ms);
}

void DattorroTank::applySize (float size)
{
    // base * 1.0f is exact, so at Size 1 every line and tap gets exactly the
    // value it had before Size existed (bit-identical, see the golden-hash
    // tests). Off 1.0 the delays are fractional: delay1/delay2/allpass2
    // read through Lagrange3rd, allpass1 through Thiran (see
    // ModulatedDelayLineType for its glide-rate analysis).
    for (auto* branch : { &branchA, &branchB })
    {
        branch->allpass1Samples = branch->allpass1BaseSamples * size;
        branch->delay1Samples   = branch->delay1BaseSamples * size;
        branch->allpass2Samples = branch->allpass2BaseSamples * size;
        branch->delay2Samples   = branch->delay2BaseSamples * size;

        branch->delay1.setDelay (branch->delay1Samples);
        branch->allpass2.delayLine.setDelay (branch->allpass2Samples);
        branch->delay2.setDelay (branch->delay2Samples);
        // allpass1's delay is set every sample in processSample() (centre +
        // modulation offset).
    }

    for (size_t i = 0; i < outputTapSamples.size(); ++i)
        outputTapSamples[i] = outputTapBaseSamples[i] * size;
}

void DattorroTank::reset()
{
    for (auto& stage : diffuserStages)
        stage.delayLine.reset();

    resetBranch (branchA);
    resetBranch (branchB);

    feedbackFromB = 0.0f;
    lastRightOutput = 0.0f;

    // Infinite mode state: snap the ramp to its target (keeps a caller's
    // setting, drops any in-flight ramp), restart both LFOs at their fixed
    // phases, and clear the level controller back to unity gain.
    smoothedInfiniteAmount.setCurrentAndTargetValue (smoothedInfiniteAmount.getTargetValue());
    modPhaseA = 0.0f;
    modPhaseB = modPhaseOffsetB;
    levelEnvelope = 0.0f;
    levelGain = 1.0f;

    // Size/Modulation: same snap-to-target rule, then scale the lines to
    // the snapped Size and re-derive g from it.
    smoothedSize.setCurrentAndTargetValue (smoothedSize.getTargetValue());
    smoothedModulation.setCurrentAndTargetValue (smoothedModulation.getTargetValue());
    appliedSize = smoothedSize.getTargetValue();
    applySize (appliedSize);
    decayGainDirty = true;
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

void DattorroTank::setDecaySeconds (float newDecaySeconds)
{
    // Called every block by the processor; only a real change re-derives g.
    if (newDecaySeconds != decaySeconds)
    {
        decaySeconds = newDecaySeconds;
        decayGainDirty = true;
    }
}

void DattorroTank::setSize (float newSize)
{
    smoothedSize.setTargetValue (newSize);
}

void DattorroTank::setModulation (float newModulation)
{
    smoothedModulation.setTargetValue (newModulation);
}

float DattorroTank::decayGainFor (float newDecaySeconds, float size) noexcept
{
    // Double precision so the result is the correctly-rounded float of the
    // exact mapping (decaySecondsForGain() relies on that to hit a target
    // gain exactly). The RT60 floor only guards a division by zero.
    const double rt60 = juce::jmax (1.0e-3, (double) newDecaySeconds);
    const double g = std::pow (10.0, -3.0 * halfLoopSeconds * (double) size / rt60);
    return (float) juce::jmin ((double) maxDecayGain, g);
}

float DattorroTank::decaySecondsForGain (float gain, float size) noexcept
{
    // Analytic inverse, then a short walk over neighbouring floats until
    // decayGainFor() returns `gain` exactly (g rises monotonically with
    // RT60). Gains outside (0, maxDecayGain] have no exact preimage; the
    // closest analytic value is returned for them.
    const double analytic = -3.0 * halfLoopSeconds * (double) size / std::log10 ((double) juce::jlimit (1.0e-6f, 0.999999f, gain));
    float candidate = (float) analytic;

    for (int step = 0; step < 1024; ++step)
    {
        const float g = decayGainFor (candidate, size);
        if (g == gain)
            break;
        candidate = std::nextafter (candidate, g < gain ? std::numeric_limits<float>::max() : 0.0f);
    }

    return candidate;
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

float DattorroTank::peekTap (DelayLineType& delay, float offsetSamples, float configuredDelay)
{
    // See the declaration-site comment: popSample's explicit-delayInSamples
    // overload calls setDelay() internally even when updateReadPointer is
    // false, so it would otherwise permanently shorten this delay line's
    // configured length for every later recirculating popSample(-1) call.
    // Restoring the caller's configured value keeps this strictly a
    // non-destructive output-only peek.
    float tapped = delay.popSample (0, offsetSamples, false);
    delay.setDelay (configuredDelay);
    return tapped;
}

float DattorroTank::processSample (float input, float externalFeedback)
{
    // Size glide and Decay (slice 2, see setSize()/setDecaySeconds()). When
    // Size is not moving getNextValue() returns exactly appliedSize, so
    // nothing below runs and steady-state cost is one compare; a Decay
    // change re-derives g once.
    const float size = smoothedSize.getNextValue();
    if (size != appliedSize)
    {
        appliedSize = size;
        applySize (size);
        decayGainDirty = true;
    }

    if (decayGainDirty)
    {
        decayGain = decayGainFor (decaySeconds, appliedSize);
        decayGainDirty = false;
    }

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

    // (b) allpass1 read-delay modulation, shared by Infinite and the user
    // Modulation control (slice 2): the deeper of the two wins, so
    // Infinite alone behaves exactly as before (max(0, x) == x for x >= 0).
    // The offset is only computed when some depth is actually present, so
    // with both at 0.0f each line gets back exactly its centre delay
    // (centre + 0.0f == centre).
    const float userDepth = smoothedModulation.getNextValue() * userModDepthSamples;
    const float depth = juce::jmax (userDepth, infiniteAmount * modDepthSamples);
    float modOffsetA = 0.0f, modOffsetB = 0.0f;

    if (depth > 0.0f)
    {
        modOffsetA = depth * std::sin (modPhaseA);
        modOffsetB = depth * std::sin (modPhaseB);
    }

    branchA.allpass1.delayLine.setDelay (branchA.allpass1Samples + modOffsetA);
    branchB.allpass1.delayLine.setDelay (branchB.allpass1Samples + modOffsetB);

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
    const auto& taps = outputTapSamples;
    float tankOut = outputScale * (
          peekTap (branchB.delay1, taps[tapBDelay1a], branchB.delay1Samples)
        + peekTap (branchB.delay1, taps[tapBDelay1b], branchB.delay1Samples)
        - peekTap (branchB.allpass2.delayLine, taps[tapBAllpass2], branchB.allpass2Samples)
        + peekTap (branchB.delay2, taps[tapBDelay2], branchB.delay2Samples)
        - peekTap (branchA.delay1, taps[tapADelay1], branchA.delay1Samples)
        - peekTap (branchA.allpass2.delayLine, taps[tapAAllpass2], branchA.allpass2Samples)
        - peekTap (branchA.delay2, taps[tapADelay2], branchA.delay2Samples)
    );

    // Dattorro's right-channel tap set (see the outputTapRight* constants):
    // read exactly like the left set above, same lines' mirror nodes, same
    // outputScale, same output-only position -- so no gain stage applies to
    // one channel and not the other (Infinite's level controller acts on the
    // shared cross-feed, upstream of both tap sets). Computed after the left
    // sum so the left output's arithmetic is untouched (bit-identical).
    lastRightOutput = outputScale * (
          peekTap (branchA.delay1, taps[tapRightADelay1a], branchA.delay1Samples)
        + peekTap (branchA.delay1, taps[tapRightADelay1b], branchA.delay1Samples)
        - peekTap (branchA.allpass2.delayLine, taps[tapRightAAllpass2], branchA.allpass2Samples)
        + peekTap (branchA.delay2, taps[tapRightADelay2], branchA.delay2Samples)
        - peekTap (branchB.delay1, taps[tapRightBDelay1], branchB.delay1Samples)
        - peekTap (branchB.allpass2.delayLine, taps[tapRightBAllpass2], branchB.allpass2Samples)
        - peekTap (branchB.delay2, taps[tapRightBDelay2], branchB.delay2Samples)
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

        // Channel 0 gets the left tap set, every other channel the right.
        for (size_t ch = 0; ch < numChannels; ++ch)
            block.setSample ((int) ch, (int) i, ch == 0 ? tankOut : lastRightOutput);
    }
}
