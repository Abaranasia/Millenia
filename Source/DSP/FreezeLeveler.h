#pragma once

#include <JuceHeader.h>

// Phase 9 Freeze follow-up (2026-08-22): output-stage, feed-forward-only
// auto-leveler that compensates Freeze's measured amplitude decay (see
// docs/shimmer-reverb-implementation-plan.md's Phase 9 section and Engram
// topic architecture/millenia-freeze -- the shipped shimmerWeight fix still
// leaves ~-1.94dB/15s decay at freezeAmount=1.0) WITHOUT touching the
// recirculating loop's stability. Compensating INSIDE the loop (e.g.
// boosting externalFeedback or feedbackFromB directly) was explicitly
// rejected: that would push the loop's own gain back toward the near-unity
// ceiling this project spent considerable effort proving fragile (see
// DattorroTank.h's frozenMaxShimmerBlendWeight sweep). This class only ever
// scales ShimmerReverbEngine's already-computed wet output, strictly after
// the tank/shifter chain and strictly before the dry/wet mix -- it cannot
// feed back into anything.
//
// Mechanism: two one-pole envelope followers track the same (mono-summed)
// wet signal.
//   - `live` always uses a fast, freezeAmount-INDEPENDENT time constant, so
//     it reflects the wet signal's REAL current level at all times.
//   - `reference` uses a time constant that itself crossfades from that same
//     fast value (freezeAmount=0.0f) toward a very slow one
//     (freezeAmount=1.0f, see frozenAttackSeconds) -- at freezeAmount=0.0f
//     the two followers are literally the same recurrence fed the same
//     input from the same initial state, so they stay bit-identical forever
//     and the computed gain is exactly 1.0f (no behavior change at all,
//     same bit-identical-at-default convention every other Freeze mechanism
//     in this codebase already follows). As freezeAmount rises, `reference`
//     increasingly lags behind `live`'s decay, approximately preserving the
//     loudness the signal had before Freeze took hold.
// The compensating gain is reference/live, clamped to [1.0, maxBoostLinear]
// (never REDUCES level, never boosts more than +12dB) and smoothed by its
// own slow follower to avoid audible gain-jumps riding on top of the
// oscillation this is trying to make less noticeable, not worse.
class FreezeLeveler
{
public:
    FreezeLeveler() = default;

    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        fastCoefficient = coefficientForSeconds (fastAttackSeconds, sampleRate);
        referenceCoefficient = fastCoefficient; // matches freezeAmount's 0.0f default
        gainSmoothingCoefficient = coefficientForSeconds (gainSmoothingSeconds, sampleRate);
        reset();
    }

    void reset() noexcept
    {
        live = 0.0f;
        reference = 0.0f;
        smoothedGain = 1.0f;
    }

    // Recomputes the reference follower's time constant from the current
    // freezeAmount. Cheap enough to call once per block -- same
    // per-block-not-per-sample convention ShimmerReverbEngine::setFreezeAmount()
    // and DattorroTank::setFreezeAmount() already use.
    void setFreezeAmount (float freezeAmount) noexcept
    {
        const float referenceSeconds = fastAttackSeconds + freezeAmount * (frozenAttackSeconds - fastAttackSeconds);
        referenceCoefficient = coefficientForSeconds (referenceSeconds, sampleRate);
    }

    // Updates both envelope followers from a mono-summed wet sample and
    // returns the compensating gain to apply to the wet signal (both
    // channels, identically -- see the class comment's stereo-image
    // rationale in ShimmerReverbEngine.cpp's call site).
    float computeGain (float monoWetSample) noexcept
    {
        const float absInput = std::abs (monoWetSample);

        live = fastCoefficient * live + (1.0f - fastCoefficient) * absInput;
        reference = referenceCoefficient * reference + (1.0f - referenceCoefficient) * absInput;

        const float targetGain = live > 1.0e-6f ? juce::jlimit (1.0f, maxBoostLinear, reference / live) : 1.0f;
        smoothedGain = gainSmoothingCoefficient * smoothedGain + (1.0f - gainSmoothingCoefficient) * targetGain;

        return smoothedGain;
    }

private:
    // Fast enough to track real level changes (a genuine new sound arriving,
    // Feedback/Shimmer Amount changes) without being so fast it responds to
    // individual waveform cycles; slow enough not to pump on the reverb
    // tail's own natural envelope fluctuation.
    static constexpr float fastAttackSeconds = 0.3f;

    // Deliberately much slower than the ~15s decay window this compensates --
    // a REASONED starting point (not ear-tuned), picked to comfortably
    // outlast that decay so `reference` tracks "loudness before Freeze"
    // rather than following the decay itself.
    static constexpr float frozenAttackSeconds = 30.0f;

    // Smooths the computed gain itself, separate from the two envelope
    // followers -- avoids an audible step in gain if freezeAmount or the
    // signal level changes abruptly.
    static constexpr float gainSmoothingSeconds = 0.5f;

    // +12dB ceiling -- bounds how much this can ever amplify, regardless of
    // how far live/reference diverge (e.g. very long freeze holds).
    static constexpr float maxBoostLinear = 4.0f;

    static float coefficientForSeconds (float seconds, double rate) noexcept
    {
        return std::exp (-1.0f / (seconds * (float) rate));
    }

    double sampleRate = 44100.0;
    float fastCoefficient = 0.0f;
    float referenceCoefficient = 0.0f;
    float gainSmoothingCoefficient = 0.0f;

    float live = 0.0f;
    float reference = 0.0f;
    float smoothedGain = 1.0f;
};
