#include "ScratchSchroederTank.h"

ScratchSchroederTank::ScratchSchroederTank() = default;
ScratchSchroederTank::~ScratchSchroederTank() = default;

void ScratchSchroederTank::prepare (const juce::dsp::ProcessSpec& spec)
{
    // Every delay line here is mono — one channel is all this throwaway tank
    // needs, regardless of what the host's channel layout is.
    juce::dsp::ProcessSpec monoSpec { spec.sampleRate, spec.maximumBlockSize, 1 };

    for (int i = 0; i < numCombs; ++i)
    {
        auto delayInSamples = juce::roundToInt (combDelayMs[(size_t) i] * 0.001 * spec.sampleRate);
        combs[(size_t) i].delayLine.setMaximumDelayInSamples (delayInSamples + 1);
        combs[(size_t) i].delayLine.prepare (monoSpec);
        combs[(size_t) i].delayLine.setDelay ((float) delayInSamples);
    }

    for (int i = 0; i < numAllpasses; ++i)
    {
        auto delayInSamples = juce::roundToInt (allpassDelayMs[(size_t) i] * 0.001 * spec.sampleRate);
        allpasses[(size_t) i].delayLine.setMaximumDelayInSamples (delayInSamples + 1);
        allpasses[(size_t) i].delayLine.prepare (monoSpec);
        allpasses[(size_t) i].delayLine.setDelay ((float) delayInSamples);
    }

    reset();
}

void ScratchSchroederTank::reset()
{
    for (auto& c : combs)
    {
        c.delayLine.reset();
        c.dampState = 0.0f;
    }

    for (auto& a : allpasses)
    {
        a.delayLine.reset();
        a.dampState = 0.0f;
    }
}

float ScratchSchroederTank::processComb (float input, DelayLineState& c)
{
    float delayed = c.delayLine.popSample (0);
    c.dampState = delayed * dampAmount + c.dampState * (1.0f - dampAmount);
    c.delayLine.pushSample (0, input + c.dampState * feedbackAmount);
    return delayed;
}

float ScratchSchroederTank::processAllpass (float input, DelayLineState& a)
{
    float bufOut = a.delayLine.popSample (0);
    float output = -input + bufOut;
    a.delayLine.pushSample (0, input + bufOut * allpassFeedback);
    return output;
}

void ScratchSchroederTank::process (juce::dsp::AudioBlock<float>& block)
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

        float combSum = 0.0f;

        for (auto& c : combs)
            combSum += processComb (monoIn, c);

        combSum *= (1.0f / (float) numCombs);

        float tankOut = combSum;

        for (auto& a : allpasses)
            tankOut = processAllpass (tankOut, a);

        for (size_t ch = 0; ch < numChannels; ++ch)
            block.setSample ((int) ch, (int) i, tankOut);
    }
}
