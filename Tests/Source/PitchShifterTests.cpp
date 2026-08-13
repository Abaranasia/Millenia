#include <JuceHeader.h>
#include <array>
#include <limits>
#include "../../Source/DSP/PitchShifter.h"

// Phase 3 test-runner target -- unit tests for the hand-rolled dual-delay-
// line crossfade PitchShifter. See
// docs/shimmer-reverb-implementation-plan.md, Phase 3.
class PitchShifterTests : public juce::UnitTest
{
public:
    PitchShifterTests() : juce::UnitTest ("PitchShifterTests", "DSP") {}

    void runTest() override
    {
        beginTest ("Cached pitch ratio matches 2^(semitones/12) at boundary values");
        {
            PitchShifter shifter;
            juce::dsp::ProcessSpec spec { 44100.0, (juce::uint32) 512, 1 };
            shifter.prepare (spec);

            for (float semitones : { -24.0f, -12.0f, 0.0f, 7.0f, 12.0f, 19.0f, 24.0f })
            {
                shifter.setPitchShiftSemitones (semitones);
                float expected = std::pow (2.0f, semitones / 12.0f);

                expectWithinAbsoluteError (shifter.getPitchRatio(), expected, 1.0e-6f,
                                            "Pitch ratio mismatch at " + juce::String (semitones) + " semitones");
            }
        }

        beginTest ("Sustained noise stays finite and bounded at extreme and default shift values");
        {
            // This is the test that would have caught a buffer-sizing
            // mistake at the extreme ends of the -24..+24 st range -- the
            // exact pitfall this class exists to avoid (see the header's
            // baseDelayGrainMultiple/maxDelayExtraGrainMultiple comments).
            constexpr double sampleRate = 44100.0;
            constexpr int numSamples = (int) (sampleRate * 5); // ~5s per shift value

            for (float semitones : { -24.0f, 12.0f, 24.0f })
            {
                PitchShifter shifter;
                juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, 1 };
                shifter.prepare (spec);
                shifter.reset();
                shifter.setPitchShiftSemitones (semitones);

                juce::Random random (54321 + (int) semitones);

                float maxPeak = 0.0f;

                for (int i = 0; i < numSamples; ++i)
                {
                    float input = random.nextFloat() * 0.6f - 0.3f;
                    float output = shifter.processSample (input);

                    expect (std::isfinite (output), "Non-finite output at semitones=" + juce::String (semitones)
                                                         + ", sample " + juce::String (i));
                    maxPeak = juce::jmax (maxPeak, std::abs (output));
                }

                expect (maxPeak <= 10.0f, "Output exceeded safety bound of 10.0 at semitones=" + juce::String (semitones)
                                               + " (peak: " + juce::String (maxPeak) + ")");
            }
        }

        beginTest ("Crossfade envelope stays close to unity gain across the grain cycle (no periodic dips/pumping)");
        {
            // Black-box check of the crossfade's constant-unity-gain
            // property (Hann(p) + Hann(p+0.5) == 1 identically, verified
            // algebraically -- see PitchShifter.cpp): feed sustained noise
            // and bucket the output energy by position within a grain
            // cycle. If the two voices' phases weren't correctly locked
            // 0.5 apart (e.g. voice B not offset at reset()), the summed
            // envelope would swing between 0 and 2 at the grain rate,
            // showing up as a large RMS difference between buckets. With a
            // correctly-locked crossfade, every bucket should see roughly
            // the same input-driven RMS.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;

            PitchShifter shifter;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, 1 };
            shifter.prepare (spec);
            shifter.reset();
            shifter.setPitchShiftSemitones (12.0f);

            juce::Random random (2468);

            // Matches PitchShifter's own grainLengthMs (25.0f) at this
            // sample rate; only used here for test bucketing, not by the
            // class itself.
            constexpr int grainLengthSamplesApprox = (int) (25.0 * 0.001 * sampleRate);

            // Warm up past several grain cycles so the delay line is full
            // of real (non-zero-initialised) signal before measuring.
            constexpr int warmupSamples = grainLengthSamplesApprox * 6;

            for (int i = 0; i < warmupSamples; ++i)
                shifter.processSample (random.nextFloat() * 2.0f - 1.0f);

            constexpr int numBins = 20;
            constexpr int numGrainCyclesToAverage = 30;
            const int binSamples = juce::jmax (1, grainLengthSamplesApprox / numBins);

            std::array<double, numBins> sumSquares {};
            std::array<int, numBins> counts {};

            for (int cycleSample = 0; cycleSample < grainLengthSamplesApprox * numGrainCyclesToAverage; ++cycleSample)
            {
                float out = shifter.processSample (random.nextFloat() * 2.0f - 1.0f);

                int binIndex = juce::jlimit (0, numBins - 1, (cycleSample % grainLengthSamplesApprox) / binSamples);
                sumSquares[(size_t) binIndex] += (double) out * (double) out;
                counts[(size_t) binIndex] += 1;
            }

            double minRms = std::numeric_limits<double>::max();
            double maxRms = 0.0;

            for (int b = 0; b < numBins; ++b)
            {
                double rms = counts[(size_t) b] > 0 ? std::sqrt (sumSquares[(size_t) b] / counts[(size_t) b]) : 0.0;
                minRms = juce::jmin (minRms, rms);
                maxRms = juce::jmax (maxRms, rms);
            }

            expect (maxRms > 0.0, "Expected non-zero output RMS during the measurement window");
            expect (minRms / maxRms >= 0.5, "Crossfade envelope shows a periodic dip/pumping at the grain rate "
                                                 "(min bin RMS " + juce::String (minRms) + " vs max bin RMS "
                                                 + juce::String (maxRms) + ") -- check the two voices' grainPhase "
                                                 "offset in reset()");
        }
    }
};

static PitchShifterTests pitchShifterTests;
