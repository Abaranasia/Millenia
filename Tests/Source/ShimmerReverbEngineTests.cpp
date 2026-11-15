#include <JuceHeader.h>
#include "../../Source/DSP/ShimmerReverbEngine.h"

// Phase 3/4 test-runner target -- automated regression for the plan's own
// explicit Definition-of-Done requirement: "No runaway/growing-without-
// bound output ... over at least 30 seconds of sustained input." See
// docs/shimmer-reverb-implementation-plan.md, Phase 3.
//
// Rewritten for the Phase 3/4 structural fix: ShimmerReverbEngine no longer
// runs the pitch shifter as a separate, externally-gained parallel feedback
// path (that mechanism -- feedbackGain/setFeedbackGain/previousShifterOutput
// -- has been removed entirely). The shifter now sits directly inside
// DattorroTank's own recirculation path via
// DattorroTank::peekFeedbackSignal()/the two-argument processSample()
// overload, matching shimmer-reverb-architecture.md's documented topology.
// There is no longer a separate "feedback gain" to set -- the only knob is
// DattorroTank's own decayGain (default 0.6f), which these tests exercise
// at its default since ShimmerReverbEngine exposes no setter for it.
//
// These tests keep the exact same two safety properties the old
// feedbackGain-based tests checked (boundedness over 30s+, decay-to-silence
// after input stops), now verified against the new signal path.
class ShimmerReverbEngineTests : public juce::UnitTest
{
public:
    ShimmerReverbEngineTests() : juce::UnitTest ("ShimmerReverbEngineTests", "DSP") {}

    void runTest() override
    {
        beginTest ("Sustained noise stays finite and bounded for at least 30 seconds with the shifter inside the tank's own recirculation path");
        {
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            // ~30.2s at 44.1kHz/512 samples per block -- comfortably past
            // the plan's own explicit 30s minimum.
            constexpr int numBlocks = 2600;

            ShimmerReverbEngine engine;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            engine.prepare (spec);
            engine.reset();

            juce::AudioBuffer<float> buffer (numChannels, blockSize);

            // Fixed-seed pseudo-random noise, same convention as
            // DattorroTankTests: moderate live-mic-ish amplitude (+-0.3),
            // continuous across every block/sample.
            juce::Random random (13579);

            float maxPeak = 0.0f;

            for (int b = 0; b < numBlocks; ++b)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* data = buffer.getWritePointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                        data[i] = random.nextFloat() * 0.6f - 0.3f; // uniform in [-0.3, 0.3]
                }

                juce::dsp::AudioBlock<float> block (buffer);
                engine.process (block);

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* data = buffer.getReadPointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        expect (std::isfinite (data[i]), "Sample is not finite (NaN/Inf) at block "
                                                              + juce::String (b) + ", sample " + juce::String (i));
                        maxPeak = juce::jmax (maxPeak, std::abs (data[i]));
                    }
                }

                expect (maxPeak <= 10.0f, "Output exceeded safety bound of 10.0 at block " + juce::String (b)
                                               + " (peak so far: " + juce::String (maxPeak) + ")");
            }
        }

        beginTest ("Output decays to near-silence after input stops, instead of self-oscillating");
        {
            // Same tail-decay property as before the structural fix, now
            // checked with the shifter actually inside the tank's own
            // decayGain=0.6f recirculation loop rather than a separate,
            // weaker external path. If this loop turns out to sustain
            // itself indefinitely (self-oscillation) at the tank's default
            // decay, this test will fail and that is itself the important
            // finding -- not something to silently patch decayGain down
            // for without evidence, since 0.6f is inside the architecture
            // doc's documented-safe range for a plain (unshifted) loop.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;

            constexpr double burstSeconds = 2.0;
            constexpr double silenceSeconds = 8.0;
            constexpr int burstBlocks = (int) (burstSeconds * sampleRate / blockSize);
            constexpr int silenceBlocks = (int) (silenceSeconds * sampleRate / blockSize);

            ShimmerReverbEngine engine;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            engine.prepare (spec);
            engine.reset();

            juce::AudioBuffer<float> buffer (numChannels, blockSize);
            juce::Random random (24680);

            for (int b = 0; b < burstBlocks; ++b)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* data = buffer.getWritePointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                        data[i] = random.nextFloat() * 0.6f - 0.3f;
                }

                juce::dsp::AudioBlock<float> block (buffer);
                engine.process (block);
            }

            float lastSecondPeak = 0.0f;
            constexpr int lastSecondBlocks = (int) (sampleRate / blockSize);

            for (int b = 0; b < silenceBlocks; ++b)
            {
                buffer.clear();

                juce::dsp::AudioBlock<float> block (buffer);
                engine.process (block);

                if (b >= silenceBlocks - lastSecondBlocks)
                {
                    auto* data = buffer.getReadPointer (0);
                    for (int i = 0; i < blockSize; ++i)
                        lastSecondPeak = juce::jmax (lastSecondPeak, std::abs (data[i]));
                }
            }

            expect (lastSecondPeak < 1e-3f, "Tail did not decay to near-silence " + juce::String (silenceSeconds)
                                                 + "s after input stopped (peak in final second: "
                                                 + juce::String (lastSecondPeak) + ") -- loop is self-oscillating, not decaying");
        }

        beginTest ("Both stereo output channels carry the same mono tank result even with different L/R input");
        {
            // Regression check for the architecture change moving the
            // mono-summing/write-back contract from PluginProcessor up into
            // ShimmerReverbEngine::process() -- exactly mirrors
            // DattorroTank::process()'s own mono-handling contract. Not
            // affected by the feedback-path restructuring above.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;

            ShimmerReverbEngine engine;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            engine.prepare (spec);
            engine.reset();

            juce::AudioBuffer<float> buffer (numChannels, blockSize);
            juce::Random random (2020);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* data = buffer.getWritePointer (ch);
                for (int i = 0; i < blockSize; ++i)
                    data[i] = random.nextFloat() * 0.6f - 0.3f;
            }

            juce::dsp::AudioBlock<float> block (buffer);
            engine.process (block);

            auto* left = buffer.getReadPointer (0);
            auto* right = buffer.getReadPointer (1);

            for (int i = 0; i < blockSize; ++i)
                expectEquals (left[i], right[i], "Output channels diverged at sample " + juce::String (i));
        }
    }
};

static ShimmerReverbEngineTests shimmerReverbEngineTests;
