#include <JuceHeader.h>
#include "../../Source/DSP/DattorroTank.h"

// Phase 7 test-runner target — unit tests for the Phase 2 DattorroTank.
// Unlike ScratchSchroederTankTests (single impulse), this specifically
// reproduces the human-reported bug: sustained, continuous input (as from
// a live mic) rather than a single impulse. See
// docs/shimmer-reverb-implementation-plan.md, Phase 2 and Phase 7.
class DattorroTankTests : public juce::UnitTest
{
public:
    DattorroTankTests() : juce::UnitTest ("DattorroTankTests", "DSP") {}

    void runTest() override
    {
        beginTest ("Sustained moderate-amplitude noise stays finite and bounded for several seconds");
        {
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            // ~4.6s at 44.1kHz/512 samples per block -- comfortably past the
            // ~2s the human reported before the blowup/crash.
            constexpr int numBlocks = 400;

            DattorroTank tank;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            tank.prepare (spec);
            tank.reset();

            juce::AudioBuffer<float> buffer (numChannels, blockSize);

            // Fixed-seed pseudo-random noise, not real randomness, so the
            // test is deterministic. Scaled to a moderate live-mic-ish
            // amplitude (+-0.3), continuous across every block/sample --
            // this is the key difference from the impulse tests: the tank
            // never gets a chance to settle before the next sample arrives.
            juce::Random random (12345);

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
                tank.process (block);

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

        beginTest ("Sustained noise with occasional near-full-scale transients stays finite and bounded");
        {
            // Real mic input isn't a clean, uniformly-bounded signal -- pops
            // and transients can spike well above the "moderate" RMS level.
            // If the previous test passes clean but this one blows up, the
            // bug is amplitude-dependent.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr int numBlocks = 400; // ~4.6s

            DattorroTank tank;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            tank.prepare (spec);
            tank.reset();

            juce::AudioBuffer<float> buffer (numChannels, blockSize);
            juce::Random random (67890);

            float maxPeak = 0.0f;
            int sampleCounter = 0;

            for (int b = 0; b < numBlocks; ++b)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* data = buffer.getWritePointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        float sample = random.nextFloat() * 0.6f - 0.3f;

                        // Every ~1000 samples, inject a near-full-scale
                        // transient spike (deterministic cadence, fixed seed).
                        if ((sampleCounter % 1000) == 0)
                            sample = (random.nextBool() ? 1.0f : -1.0f) * 0.95f;

                        data[i] = sample;
                        ++sampleCounter;
                    }
                }

                juce::dsp::AudioBlock<float> block (buffer);
                tank.process (block);

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

        beginTest ("Impulse response has energy spread across several early time regions, not one sparse repeat");
        {
            // Reproduces the human-reported "sounds like a delay with one
            // repetition" complaint and confirms the multi-tap output fix
            // (branchOutputTaps()/peekTap() in DattorroTank.cpp): the old
            // single-tap design's only arrival inside the first ~200ms was
            // silence followed by one impulse near the diffuser + branch-A
            // full-chain time (~388ms, i.e. nothing at all within 200ms).
            //
            // NB: this deliberately measures occupied time *buckets*, not
            // debounced threshold-crossing "edges". A first attempt at this
            // test counted rising edges above the threshold, gated by a
            // minimum silent gap -- but the fixed tank's tail turns out to
            // ring continuously (bursts every ~1-2ms) once the first tap
            // arrives, with no gap ever long enough to separate "arrivals".
            // That continuous ringing *is* the desired dense-tail behaviour,
            // so an edge counter that requires silence in between actually
            // penalises the fix for working. Bucketing time into windows and
            // counting how many windows have any energy above threshold
            // captures the intended property directly: a sparse single
            // repeat lights up one narrow region near ~388ms (zero buckets
            // inside a 200ms window), while a dense tail lights up many
            // buckets spread across the window.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;

            constexpr int windowMs = 200;
            constexpr int windowSamples = (int) (sampleRate * windowMs / 1000.0);
            constexpr int numBlocks = (windowSamples / blockSize) + 2; // comfortably cover the window

            constexpr int numBuckets = 8; // 25ms buckets across the 200ms window
            constexpr int bucketSamples = windowSamples / numBuckets;

            DattorroTank tank;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            tank.prepare (spec);
            tank.reset();

            juce::AudioBuffer<float> buffer (numChannels, blockSize);

            constexpr float arrivalThreshold = 1e-4f;

            std::array<float, (size_t) numBuckets> bucketPeaks {};
            int samplesProcessed = 0;

            for (int b = 0; b < numBlocks && samplesProcessed < windowSamples; ++b)
            {
                buffer.clear();

                if (b == 0)
                {
                    buffer.setSample (0, 0, 1.0f);
                    buffer.setSample (1, 0, 1.0f);
                }

                juce::dsp::AudioBlock<float> block (buffer);
                tank.process (block);

                auto* data = buffer.getReadPointer (0);

                for (int i = 0; i < blockSize && samplesProcessed < windowSamples; ++i, ++samplesProcessed)
                {
                    expect (std::isfinite (data[i]), "Sample is not finite at sample " + juce::String (samplesProcessed));

                    auto bucketIndex = juce::jlimit (0, numBuckets - 1, samplesProcessed / bucketSamples);
                    bucketPeaks[(size_t) bucketIndex] = juce::jmax (bucketPeaks[(size_t) bucketIndex], std::abs (data[i]));
                }
            }

            int numOccupiedBuckets = 0;

            for (auto peak : bucketPeaks)
                if (peak > arrivalThreshold)
                    ++numOccupiedBuckets;

            expect (numOccupiedBuckets >= 3, "Expected energy spread across at least 3 of the " + juce::String (numBuckets)
                                                  + " 25ms buckets in the first " + juce::String (windowMs)
                                                  + "ms, found " + juce::String (numOccupiedBuckets));
        }
    }
};

static DattorroTankTests dattorroTankTests;
