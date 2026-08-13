#include <JuceHeader.h>
#include "../../Source/DSP/DCBlocker.h"

// Phase 4 test-runner target -- unit tests for the one-pole DC-blocking
// filter placed inside the shimmer feedback loop. See
// docs/shimmer-reverb-implementation-plan.md, Phase 4.
class DCBlockerTests : public juce::UnitTest
{
public:
    DCBlockerTests() : juce::UnitTest ("DCBlockerTests", "DSP") {}

    void runTest() override
    {
        beginTest ("DC input converges to near-zero output after settling");
        {
            constexpr double sampleRate = 44100.0;

            DCBlocker blocker;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, 1 };
            blocker.prepare (spec);
            blocker.reset();

            constexpr int numSamples = 5000; // well past the ~20Hz/44.1kHz settling time
            float output = 0.0f;

            for (int i = 0; i < numSamples; ++i)
                output = blocker.processSample (0.5f);

            expect (std::abs (output) < 1.0e-3f, "DC input did not settle to near-zero output (final value: "
                                                      + juce::String (output) + ")");
        }

        beginTest ("Audio-band content (440Hz) passes through close to unity gain");
        {
            constexpr double sampleRate = 44100.0;
            constexpr float frequencyHz = 440.0f;

            DCBlocker blocker;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, 1 };
            blocker.prepare (spec);
            blocker.reset();

            constexpr int numSamples = 44100; // 1s, several hundred cycles at 440Hz -- reaches steady state
            constexpr int measureFromSample = numSamples / 2; // skip the initial highpass settling transient

            float inputPeak = 0.0f;
            float outputPeak = 0.0f;

            for (int i = 0; i < numSamples; ++i)
            {
                float input = std::sin (juce::MathConstants<float>::twoPi * frequencyHz * (float) i / (float) sampleRate);
                float output = blocker.processSample (input);

                if (i >= measureFromSample)
                {
                    inputPeak = juce::jmax (inputPeak, std::abs (input));
                    outputPeak = juce::jmax (outputPeak, std::abs (output));
                }
            }

            expect (inputPeak > 0.0f, "Expected non-zero input peak during the measurement window");
            expectWithinAbsoluteError (outputPeak / inputPeak, 1.0f, 0.03f,
                                       "440Hz content was attenuated by more than a few percent (input peak "
                                           + juce::String (inputPeak) + ", output peak " + juce::String (outputPeak) + ")");
        }
    }
};

static DCBlockerTests dcBlockerTests;
