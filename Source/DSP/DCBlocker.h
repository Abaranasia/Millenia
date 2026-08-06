#pragma once

#include <JuceHeader.h>

// One-pole DC-blocking filter, used at the feedback loop boundary to
// stop DC bias accumulating over long shimmer tails.
class DCBlocker
{
public:
    DCBlocker() = default;

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    float processSample (float input);

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DCBlocker)
};
