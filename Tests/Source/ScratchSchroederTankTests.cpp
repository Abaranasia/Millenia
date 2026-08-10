#include <JuceHeader.h>
#include "../../Source/DSP/ScratchSchroederTank.h"

// Phase 7 test-runner target — unit tests for the Phase 1 throwaway
// ScratchSchroederTank (mono Freeverb-style 6-comb + 2-allpass tank).
// See docs/shimmer-reverb-implementation-plan.md, Phase 1 and Phase 7.
class ScratchSchroederTankTests : public juce::UnitTest
{
public:
    ScratchSchroederTankTests() : juce::UnitTest ("ScratchSchroederTankTests", "DSP") {}

    void runTest() override
    {
        beginTest ("Impulse response is bounded, decaying, and finite");
        {
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr int numBlocks = 40; // ~460ms at 44.1kHz/512 samples per block

            ScratchSchroederTank tank;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            tank.prepare (spec);
            tank.reset();

            juce::AudioBuffer<float> buffer (numChannels, blockSize);

            // NB: the shortest comb delay (~25ms => ~1116 samples) is longer
            // than one block (512 samples), so block 0's output is exactly
            // silent — the impulse hasn't had time to return through any
            // feedback path yet. Comparing the tail against block 0 would
            // therefore always trivially "decay" (or worse, be an impossible
            // comparison against a peak of 0). Track the maximum peak seen
            // across the whole run instead, and confirm the tail has moved
            // past that peak by the end — the actual signal of a decaying,
            // non-sustaining tail.
            float maxBlockPeak = 0.0f;
            float lastBlockPeak = 0.0f;

            for (int b = 0; b < numBlocks; ++b)
            {
                buffer.clear();

                if (b == 0)
                {
                    buffer.setSample (0, 0, 1.0f);
                    buffer.setSample (1, 0, 1.0f);
                }

                juce::dsp::AudioBlock<float> block (buffer);
                tank.process (block);

                float blockPeak = 0.0f;
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* data = buffer.getReadPointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        expect (std::isfinite (data[i]), "Sample is not finite");
                        blockPeak = juce::jmax (blockPeak, std::abs (data[i]));
                    }
                }

                expect (blockPeak <= 10.0f, "Block peak exceeded safety bound");

                maxBlockPeak = juce::jmax (maxBlockPeak, blockPeak);
                lastBlockPeak = blockPeak;
            }

            expect (lastBlockPeak < maxBlockPeak, "Tail did not decay over time");
        }

        beginTest ("Tail settles to near-silence well before feedback decay would require it");
        {
            // Deliberately NOT just "silence in -> silence out from a freshly
            // reset tank" (trivially true and barely worth testing). This
            // excites the tank first, same as the previous test, then runs
            // long enough for the feedback decay (0.84 comb / 0.5 allpass,
            // both well below 1.0) to have driven the tail below the noise
            // floor -- the actual "does it ever ring on forever" property.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr int numBlocks = 300; // ~3.5s -- comfortably past decay-to-1e-4 for these coefficients

            ScratchSchroederTank tank;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            tank.prepare (spec);
            tank.reset();

            juce::AudioBuffer<float> buffer (numChannels, blockSize);
            float lastBlockPeak = 0.0f;

            for (int b = 0; b < numBlocks; ++b)
            {
                buffer.clear();

                if (b == 0)
                {
                    buffer.setSample (0, 0, 1.0f);
                    buffer.setSample (1, 0, 1.0f);
                }

                juce::dsp::AudioBlock<float> block (buffer);
                tank.process (block);

                float blockPeak = 0.0f;
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* data = buffer.getReadPointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                        blockPeak = juce::jmax (blockPeak, std::abs (data[i]));
                }

                lastBlockPeak = blockPeak;
            }

            expect (lastBlockPeak < 1e-4f, "Tail did not settle to near-silence within 3.5s: " + juce::String (lastBlockPeak));
        }
    }
};

static ScratchSchroederTankTests scratchSchroederTankTests;
