#pragma once

#include <JuceHeader.h>

// Top-level DSP object: composes the input diffuser, the Dattorro tank,
// the pitch shifter, the safety limiter, and the DC blocker into the
// full shimmer reverb signal path. This is the only DSP class
// PluginProcessor talks to directly.
class ShimmerReverbEngine
{
public:
    ShimmerReverbEngine();
    ~ShimmerReverbEngine();

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    void process (juce::dsp::AudioBlock<float>& block);

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShimmerReverbEngine)
};
