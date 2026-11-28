#pragma once

#include <JuceHeader.h>
#include "DSP/ShimmerReverbEngine.h"

// Audit fix 6 (docs/project-audit-2026-11.md): drives the engine's
// block-rate parameters (the ones the engine holds constant for the whole
// process() call) from per-parameter linear SmoothedValues, in fixed-size
// chunks instead of once per host block.
//
// Before: each smoother was skip()'d ONCE per host block and the block-end
// value was held constant by the engine for the whole block, so the audible
// zipper step size scaled with the host block size (a 2048-sample block
// stepped 32x coarser than a 64-sample one). Now the host block is split into
// chunks of at most chunkSamples; every chunk advances the smoothers by its own
// length and pushes the value at the chunk end, so the step size is
// independent of the host block size.
//
// Header-only and engine-only (no APVTS dependency) so the unit-test project
// can drive it without linking MilleniaAudioProcessor. No allocation, no
// locks: everything is member state plus engine setter calls.
class EngineParameterSmoothing
{
public:
    // 32 samples = 0.67 ms at 48 kHz. Small enough that the largest zipper
    // step (a 50 ms linear ramp, 1 / 2400 of the range per sample at 48 kHz)
    // is 32 / 2400 = 1.3 % of the range per chunk, below audibility for every
    // parameter here; large enough that the per-chunk work in the engine
    // (four filter-cutoff updates and the per-chunk freeze/formant decisions)
    // stays a small fraction of the per-sample DSP. Also must stay <= the
    // engine's maximum block size (ShimmerReverbEngine::process() would split
    // anything larger anyway).
    static constexpr size_t chunkSamples = 32;

    // Smoothing ramp length, unchanged from the pre-fix per-block smoothers.
    static constexpr double rampSeconds = 0.05;

    // One target per smoothed parameter. Bypass is folded into `mix` by the
    // caller (bypass drives the mix smoother toward 0, never the engine's own
    // setBypassed(), see PluginProcessor::processBlock()).
    struct Targets
    {
        float pitchShift = 0.0f;
        float decay = 0.0f;
        float shimmerAmount = 0.0f;
        float shimmerFeedback = 0.0f;
        float damping = 0.0f;
        float width = 0.0f;
        float mix = 0.0f;
        float lowCut = 0.0f;
        float highCut = 0.0f;
        float air = 0.0f;
        float shimmerTone = 0.0f;
        float freeze = 0.0f;
    };

    // Resets every ramp to rampSeconds and snaps current AND target to
    // `initial` (playback must not start with a ramp-in from some default).
    // The next process() pushes every value to the engine once, whatever the
    // engine's own state, so a re-prepare() of the engine is always followed
    // by a full re-push.
    void prepare (double sampleRate, const Targets& initial)
    {
        for (auto* s : { &pitchShift, &decay, &shimmerAmount, &shimmerFeedback, &damping, &width,
                         &mix, &lowCut, &highCut, &air, &shimmerTone, &freeze })
            s->reset (sampleRate, rampSeconds);

        pitchShift.setCurrentAndTargetValue (initial.pitchShift);
        decay.setCurrentAndTargetValue (initial.decay);
        shimmerAmount.setCurrentAndTargetValue (initial.shimmerAmount);
        shimmerFeedback.setCurrentAndTargetValue (initial.shimmerFeedback);
        damping.setCurrentAndTargetValue (initial.damping);
        width.setCurrentAndTargetValue (initial.width);
        mix.setCurrentAndTargetValue (initial.mix);
        lowCut.setCurrentAndTargetValue (initial.lowCut);
        highCut.setCurrentAndTargetValue (initial.highCut);
        air.setCurrentAndTargetValue (initial.air);
        shimmerTone.setCurrentAndTargetValue (initial.shimmerTone);
        freeze.setCurrentAndTargetValue (initial.freeze);

        forceFullPush = true;
    }

    // Sets the new targets, then renders `block` through the engine chunk by
    // chunk (in place). Audio-thread safe.
    void process (ShimmerReverbEngine& engine, const Targets& targets, juce::dsp::AudioBlock<float>& block)
    {
        pitchShift.setTargetValue (targets.pitchShift);
        decay.setTargetValue (targets.decay);
        shimmerAmount.setTargetValue (targets.shimmerAmount);
        shimmerFeedback.setTargetValue (targets.shimmerFeedback);
        damping.setTargetValue (targets.damping);
        width.setTargetValue (targets.width);
        mix.setTargetValue (targets.mix);
        lowCut.setTargetValue (targets.lowCut);
        highCut.setTargetValue (targets.highCut);
        air.setTargetValue (targets.air);
        shimmerTone.setTargetValue (targets.shimmerTone);
        freeze.setTargetValue (targets.freeze);

        const auto total = block.getNumSamples();

        for (size_t offset = 0; offset < total; offset += chunkSamples)
        {
            const auto length = juce::jmin (chunkSamples, total - offset);
            advanceAndPush (engine, (int) length);

            auto chunk = block.getSubBlock (offset, length);
            engine.process (chunk);
        }
    }

private:
    // Advances every smoother by `length` samples and pushes its value at the
    // chunk end to the engine. A setter is only called when its value differs
    // from the one last pushed (the setters recompute pow/tan/coefficients and
    // the common case is a knob at rest). The value that finishes a ramp
    // differs from the previous push by definition, so the final target is
    // always applied; a smoother that is not moving returns its target
    // exactly, so an unchanged value is bit-identical to the one already in
    // the engine.
    void advanceAndPush (ShimmerReverbEngine& engine, int length)
    {
        push (pitchShift.skip (length), lastPitchShift,
              [&engine] (float v) { engine.setPitchShiftSemitones (v); });
        push (decay.skip (length), lastDecay,
              [&engine] (float v) { engine.setDecaySeconds (v); });
        push (shimmerAmount.skip (length), lastShimmerAmount,
              [&engine] (float v) { engine.setShimmerAmount (v); });
        push (shimmerFeedback.skip (length), lastShimmerFeedback,
              [&engine] (float v) { engine.setShimmerFeedback (v); });
        push (damping.skip (length), lastDamping,
              [&engine] (float v) { engine.setDamping (v); });
        push (width.skip (length), lastWidth,
              [&engine] (float v) { engine.setWidth (v); });
        push (mix.skip (length), lastMix,
              [&engine] (float v) { engine.setMix (v); });

        // Linear smoothers land exactly on their target when the ramp ends, so
        // the engine's "skip the filter at the open endpoint" check sees
        // exactly 20 Hz / 20 kHz again once a knob is returned there.
        push (lowCut.skip (length), lastLowCut,
              [&engine] (float v) { engine.setLowCutHz (v); });
        push (highCut.skip (length), lastHighCut,
              [&engine] (float v) { engine.setHighCutHz (v); });
        // Air: a linear smoother lands exactly on 0 dB, where the engine
        // gates the shelf off again.
        push (air.skip (length), lastAir,
              [&engine] (float v) { engine.setAirDb (v); });
        // Same endpoint reasoning for Shimmer Tone's Off (10 kHz).
        push (shimmerTone.skip (length), lastShimmerTone,
              [&engine] (float v) { engine.setShimmerTone (v); });

        // Freeze is chunked too: the engine ramps its own freeze state
        // linearly from the previous process() call's value to the one pushed
        // before it, so pushing the whole host block's value before the first
        // chunk would squeeze that ramp into 32 samples.
        push (freeze.skip (length), lastFreeze,
              [&engine] (float v) { engine.setFreezeAmount (v); });

        forceFullPush = false;
    }

    template <typename Setter>
    void push (float value, float& lastPushed, Setter&& setter)
    {
        if (forceFullPush || value != lastPushed)
        {
            setter (value);
            lastPushed = value;
        }
    }

    juce::SmoothedValue<float> pitchShift, decay, shimmerAmount, shimmerFeedback, damping, width,
                               mix, lowCut, highCut, air, shimmerTone, freeze;

    float lastPitchShift = 0.0f, lastDecay = 0.0f, lastShimmerAmount = 0.0f, lastShimmerFeedback = 0.0f,
          lastDamping = 0.0f, lastWidth = 0.0f, lastMix = 0.0f, lastLowCut = 0.0f, lastHighCut = 0.0f, lastAir = 0.0f,
          lastShimmerTone = 0.0f, lastFreeze = 0.0f;

    // True until the first chunk after prepare() has pushed everything.
    bool forceFullPush = true;
};
