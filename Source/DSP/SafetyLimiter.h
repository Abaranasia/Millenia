#pragma once

#include <JuceHeader.h>

// Soft-clip/limiter safety net placed inside the feedback loop,
// independent of the feedback-gain knob, so runaway levels can never
// build up unbounded.
class SafetyLimiter
{
public:
    SafetyLimiter() = default;

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    float processSample (float input);

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SafetyLimiter)
};
