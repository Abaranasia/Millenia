#include "SafetyLimiter.h"

// Genuinely stateless -- no delay lines, no envelope follower -- so
// prepare()/reset() are intentionally empty. Kept as real (non-inline)
// methods purely for interface consistency with every other DSP class here,
// all of which get prepare()/reset() called uniformly from
// ShimmerReverbEngine.
void SafetyLimiter::prepare (const juce::dsp::ProcessSpec&)
{
}

void SafetyLimiter::reset()
{
}

float SafetyLimiter::processSample (float input)
{
    float absInput = std::abs (input);

    if (absInput <= threshold)
        return input;

    float sign = input < 0.0f ? -1.0f : 1.0f;
    float excess = (absInput - threshold) / (1.0f - threshold);
    return sign * (threshold + (1.0f - threshold) * std::tanh (excess));
}

void SafetyLimiter::setThreshold (float newThreshold)
{
    // Must stay strictly below 1.0 so (1 - threshold) never divides by zero.
    threshold = juce::jlimit (0.0f, 0.99f, newThreshold);
}
