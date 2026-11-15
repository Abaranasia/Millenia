#include <JuceHeader.h>
#include "../../Source/DSP/SafetyLimiter.h"

// Phase 4 test-runner target -- unit tests for the memoryless soft-knee
// limiter placed inside the shimmer feedback loop, replacing the earlier
// bare std::tanh(...) stopgap. See
// docs/shimmer-reverb-implementation-plan.md, Phase 4, and SafetyLimiter.h
// for the full design rationale.
class SafetyLimiterTests : public juce::UnitTest
{
public:
    SafetyLimiterTests() : juce::UnitTest ("SafetyLimiterTests", "DSP") {}

    void runTest() override
    {
        beginTest ("Exact passthrough below threshold");
        {
            SafetyLimiter limiter;
            juce::dsp::ProcessSpec spec { 44100.0, (juce::uint32) 512, 1 };
            limiter.prepare (spec);
            limiter.reset();

            // Passthrough branch is a literal "return input" -- no
            // tolerance needed, these must be exact.
            expectEquals (limiter.processSample (0.0f), 0.0f);
            expectEquals (limiter.processSample (0.3f), 0.3f);
            expectEquals (limiter.processSample (-0.5f), -0.5f);
            expectEquals (limiter.processSample (0.8f), 0.8f);
            expectEquals (limiter.processSample (-0.8f), -0.8f);
        }

        beginTest ("Bounded, never reaches the asymptote, for large inputs");
        {
            SafetyLimiter limiter;
            juce::dsp::ProcessSpec spec { 44100.0, (juce::uint32) 512, 1 };
            limiter.prepare (spec);
            limiter.reset();

            constexpr float threshold = 0.8f; // matches SafetyLimiter's defaultThreshold

            // NOTE: values are "large" relative to threshold but deliberately
            // stay below ~2.5f. Beyond that magnitude, excess exceeds ~8.7 and
            // std::tanh's float32 result rounds to exactly 1.0f (double-
            // precision tanh(8.7) is already within half a float32 ULP of 1),
            // making the limiter's output exactly 1.0f rather than merely
            // close to it -- still never *exceeding* the asymptote (the
            // real-time safety property), but no longer strictly *less than*
            // it either. That is a float32-precision property of tanh
            // itself, not a bug in the limiter's algorithm, so this test
            // sticks to magnitudes where the mathematical "never reaches 1.0"
            // guarantee is also observable in float32.
            for (float input : { 1.0f, 1.5f, -2.3f })
            {
                float output = limiter.processSample (input);

                expect (std::abs (output) < 1.0f, "Output reached or exceeded the 1.0 asymptote for input "
                                                       + juce::String (input) + " (output: " + juce::String (output) + ")");
                expect (std::abs (output) > threshold, "Output did not get pushed up from the threshold for input "
                                                            + juce::String (input) + " (output: " + juce::String (output) + ")");
            }
        }

        beginTest ("Continuous at the knee (no audible click)");
        {
            SafetyLimiter limiterBelow;
            juce::dsp::ProcessSpec spec { 44100.0, (juce::uint32) 512, 1 };
            limiterBelow.prepare (spec);
            limiterBelow.reset();
            float below = limiterBelow.processSample (0.79f);

            SafetyLimiter limiterAbove;
            limiterAbove.prepare (spec);
            limiterAbove.reset();
            float above = limiterAbove.processSample (0.81f);

            expect (std::abs (above - below) < 0.05f, "Discontinuity detected right at the threshold (0.79f -> "
                                                            + juce::String (below) + ", 0.81f -> " + juce::String (above) + ")");
        }
    }
};

static SafetyLimiterTests safetyLimiterTests;
