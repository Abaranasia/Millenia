#include "ShimmerReverbEngine.h"

ShimmerReverbEngine::ShimmerReverbEngine() = default;
ShimmerReverbEngine::~ShimmerReverbEngine() = default;

void ShimmerReverbEngine::prepare (const juce::dsp::ProcessSpec& spec)
{
    tank.prepare (spec);
    shifter.prepare (spec);
    feedbackDcBlocker.prepare (spec);
    safetyLimiter.prepare (spec);

    shifter.setPitchShiftSemitones (shiftSemitones);

    monoScratch.setSize (1, (int) spec.maximumBlockSize);

    reset();
}

void ShimmerReverbEngine::reset()
{
    tank.reset();
    shifter.reset();
    feedbackDcBlocker.reset();
    safetyLimiter.reset();
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
        float shiftedFeedback = shifter.processSample (tank.peekFeedbackSignal());

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

        float tankOut = tank.processSample (monoData[i], safeFeedback);

        // What's written back is the tank's own output, NOT the shifter's --
        // the shifter only exists in the recirculating feedback path so that
        // each successive pass through the tank is pitched up from the last.
        monoData[i] = tankOut;
    }

    for (size_t ch = 0; ch < numChannels; ++ch)
        for (size_t i = 0; i < numSamples; ++i)
            block.setSample ((int) ch, (int) i, monoData[i]);
}
