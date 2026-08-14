#include <JuceHeader.h>
#include <array>
#include <limits>
#include <vector>
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
            // property (Hann(p) + Hann(p+0.25) + Hann(p+0.5) + Hann(p+0.75)
            // == 2 identically for the 4-voice groups, normalized by 0.5 to
            // == 1 -- verified algebraically in PitchShifter.h's class-level
            // comment): feed sustained noise and bucket the output energy by
            // position within a grain cycle, for BOTH the primary
            // (processSample()'s return value) and quadrature
            // (getQuadratureOutput()) voice groups. If a group's 4 voices'
            // phases weren't correctly locked at equal 0.25 spacing (e.g. a
            // seeding bug in reset()), the summed envelope would swing away
            // from a constant at the grain rate, showing up as a large RMS
            // difference between buckets. With a correctly-locked crossfade,
            // every bucket should see roughly the same input-driven RMS.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;

            PitchShifter shifter;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, 1 };
            shifter.prepare (spec);
            shifter.reset();
            shifter.setPitchShiftSemitones (12.0f);

            juce::Random random (2468);

            // Matches PitchShifter's own grainLengthMs (20.0f) at this
            // sample rate; only used here for test bucketing, not by the
            // class itself.
            constexpr int grainLengthSamplesApprox = (int) (20.0 * 0.001 * sampleRate);

            // Warm up past several grain cycles so the delay line is full
            // of real (non-zero-initialised) signal before measuring.
            constexpr int warmupSamples = grainLengthSamplesApprox * 6;

            for (int i = 0; i < warmupSamples; ++i)
                shifter.processSample (random.nextFloat() * 2.0f - 1.0f);

            constexpr int numBins = 20;
            constexpr int numGrainCyclesToAverage = 30;
            const int binSamples = juce::jmax (1, grainLengthSamplesApprox / numBins);

            std::array<double, numBins> sumSquaresPrimary {};
            std::array<double, numBins> sumSquaresQuadrature {};
            std::array<int, numBins> counts {};

            for (int cycleSample = 0; cycleSample < grainLengthSamplesApprox * numGrainCyclesToAverage; ++cycleSample)
            {
                float out = shifter.processSample (random.nextFloat() * 2.0f - 1.0f);
                float quadrature = shifter.getQuadratureOutput();

                int binIndex = juce::jlimit (0, numBins - 1, (cycleSample % grainLengthSamplesApprox) / binSamples);
                sumSquaresPrimary[(size_t) binIndex] += (double) out * (double) out;
                sumSquaresQuadrature[(size_t) binIndex] += (double) quadrature * (double) quadrature;
                counts[(size_t) binIndex] += 1;
            }

            auto checkBucketedRms = [&] (const std::array<double, numBins>& sumSquares, const juce::String& label)
            {
                double minRms = std::numeric_limits<double>::max();
                double maxRms = 0.0;

                for (int b = 0; b < numBins; ++b)
                {
                    double rms = counts[(size_t) b] > 0 ? std::sqrt (sumSquares[(size_t) b] / counts[(size_t) b]) : 0.0;
                    minRms = juce::jmin (minRms, rms);
                    maxRms = juce::jmax (maxRms, rms);
                }

                expect (maxRms > 0.0, label + ": expected non-zero output RMS during the measurement window");
                expect (minRms / maxRms >= 0.5, label + ": crossfade envelope shows a periodic dip/pumping at the "
                                                     "grain rate (min bin RMS " + juce::String (minRms) + " vs max "
                                                     "bin RMS " + juce::String (maxRms) + ") -- check that group's "
                                                     "4 voices' grainPhase offsets in reset()");
            };

            checkBucketedRms (sumSquaresPrimary, "Primary group");
            checkBucketedRms (sumSquaresQuadrature, "Quadrature group");
        }

        beginTest ("Normalized 4-voice crossfade sum equals exactly unity gain (catches a wrong 0.5 normalization factor)");
        {
            // Direct, black-box verification of the 2.0/N=0.5 normalization
            // factor documented in PitchShifter.h's class-level comment.
            // Unlike the bucketed-RMS test above (which only checks
            // *relative* consistency across a grain cycle and would not
            // notice an overall level error, since a wrong normalization
            // scales every bucket by the same wrong factor), this test
            // feeds a sustained DC (constant) input. Once the shared
            // delayLine is entirely full of that same constant, EVERY
            // voice's interpolated read returns exactly that constant
            // (Lagrange3rd interpolation of a constant signal reproduces
            // the constant exactly, no error) regardless of which delay
            // offset/phase it reads at. So, for either voice group, the
            // combined output must equal exactly
            // input * 0.5 * sum_k hann(phaseK) for k=0..3 -- and per the
            // COLA identity (4 equally-spaced phases sum their Hann
            // envelopes to exactly 2.0, for ANY starting phase), that
            // reduces to input * 0.5 * 2.0 == input, i.e. exact unity gain,
            // at every sample across the full grain cycle. If the 0.5
            // factor were dropped, output would be input * 2.0 (double);
            // if it were mistakenly applied twice, output would be
            // input * 0.5 (half) -- either error is caught here with a
            // tight tolerance, not the loose 0.5 ratio margin used for
            // pumping detection above. This also catches a phase-seeding
            // bug in reset() (e.g. voices not actually equally spaced),
            // since the COLA identity only holds for equally-spaced phases.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr float dcInput = 0.4321f; // arbitrary non-trivial constant, not 0/1

            PitchShifter shifter;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, 1 };
            shifter.prepare (spec);
            shifter.reset();
            shifter.setPitchShiftSemitones (12.0f); // same default shift as the rest of this suite

            // Matches PitchShifter's own grainLengthMs (20.0f) at this
            // sample rate; only used here for warmup/cycle sizing, not by
            // the class itself.
            constexpr int grainLengthSamplesApprox = (int) (20.0 * 0.001 * sampleRate);

            // Warm up well past the delay line's largest possible read
            // offset (baseDelaySamples + margin, on the order of
            // ~6 * grainLengthSamples at this class's constants) so every
            // voice's read position -- across the full [0,1] phase range
            // this test also cycles through -- is reading pure DC history,
            // not the zero-initialised buffer tail.
            constexpr int warmupSamples = grainLengthSamplesApprox * 20;

            for (int i = 0; i < warmupSamples; ++i)
                shifter.processSample (dcInput);

            // Check across one full grain cycle so every relative phase
            // combination between the two groups is exercised, not just
            // one arbitrary instant.
            for (int i = 0; i < grainLengthSamplesApprox; ++i)
            {
                float primaryOut = shifter.processSample (dcInput);
                float quadratureOut = shifter.getQuadratureOutput();

                expectWithinAbsoluteError (primaryOut, dcInput, 1.0e-4f,
                                            "Primary group's normalized crossfade sum was not unity gain at "
                                            "sample " + juce::String (i) + " (expected " + juce::String (dcInput)
                                            + ", got " + juce::String (primaryOut) + ") -- check the 0.5 "
                                            "normalization factor in PitchShifter::processSample()");

                expectWithinAbsoluteError (quadratureOut, dcInput, 1.0e-4f,
                                            "Quadrature group's normalized crossfade sum was not unity gain at "
                                            "sample " + juce::String (i) + " (expected " + juce::String (dcInput)
                                            + ", got " + juce::String (quadratureOut) + ") -- check the 0.5 "
                                            "normalization factor in PitchShifter::processSample()");
            }
        }

        beginTest ("Quadrature output stays finite and bounded at extreme and default shift values");
        {
            // Mirrors the boundedness test above, but also checks
            // getQuadratureOutput() -- the Phase 4 decorrelation voice group
            // reads the exact same delayLine at voiceDelaySamples(0.125)/
            // (0.375)/(0.625)/(0.875), which the header's
            // baseDelayGrainMultiple/maxDelayExtraGrainMultiple margin
            // comments claim already cover (derived for phase spanning the
            // full [0,1] range, not just the 0.0/0.25/0.5/0.75 phases the
            // primary group happens to use) -- this test is what would
            // catch it if that claim were wrong.
            constexpr double sampleRate = 44100.0;
            constexpr int numSamples = (int) (sampleRate * 5); // ~5s per shift value

            for (float semitones : { -24.0f, 12.0f, 24.0f })
            {
                PitchShifter shifter;
                juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, 1 };
                shifter.prepare (spec);
                shifter.reset();
                shifter.setPitchShiftSemitones (semitones);

                juce::Random random (98765 + (int) semitones);

                float maxPeak = 0.0f;

                for (int i = 0; i < numSamples; ++i)
                {
                    float input = random.nextFloat() * 0.6f - 0.3f;
                    shifter.processSample (input);
                    float quadrature = shifter.getQuadratureOutput();

                    expect (std::isfinite (quadrature), "Non-finite quadrature output at semitones=" + juce::String (semitones)
                                                              + ", sample " + juce::String (i));
                    maxPeak = juce::jmax (maxPeak, std::abs (quadrature));
                }

                expect (maxPeak <= 10.0f, "Quadrature output exceeded safety bound of 10.0 at semitones=" + juce::String (semitones)
                                               + " (peak: " + juce::String (maxPeak) + ")");
            }
        }

        beginTest ("DIAGNOSTIC: error vs. shift amount sweep");
        {
            constexpr double sampleRate = 44100.0;
            constexpr float inputFreq = 220.0f;

            for (float semitones : { 0.0f, 3.0f, 5.0f, 7.0f, 12.0f, -12.0f, 24.0f })
            {
                PitchShifter shifter;
                juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, 1 };
                shifter.prepare (spec);
                shifter.reset();
                shifter.setPitchShiftSemitones (semitones);
                const float expectedFreq = inputFreq * shifter.getPitchRatio();

                constexpr int warmupSamples = (int) (sampleRate * 0.5);
                constexpr int measureSamples = (int) (sampleRate * 1.0);

                double phase = 0.0;
                const double phaseInc = juce::MathConstants<double>::twoPi * inputFreq / sampleRate;

                for (int i = 0; i < warmupSamples; ++i)
                {
                    shifter.processSample ((float) std::sin (phase) * 0.5f);
                    phase += phaseInc;
                    if (phase >= juce::MathConstants<double>::twoPi) phase -= juce::MathConstants<double>::twoPi;
                }

                std::vector<float> measured ((size_t) measureSamples);
                for (int i = 0; i < measureSamples; ++i)
                {
                    measured[(size_t) i] = shifter.processSample ((float) std::sin (phase) * 0.5f);
                    phase += phaseInc;
                    if (phase >= juce::MathConstants<double>::twoPi) phase -= juce::MathConstants<double>::twoPi;
                }

                // Precise (linearly-interpolated) positive-going zero-crossing
                // timing, averaged across the whole window -- a time-domain
                // measurement with no spectral-leakage confound (unlike the
                // windowed-FFT/Goertzel search this replaces, which was
                // unreliable for expectedFreq values that don't land on an
                // exact 1Hz DFT bin over a 1-second capture).
                std::vector<double> crossings;
                for (int i = 1; i < measureSamples; ++i)
                {
                    float prev = measured[(size_t) (i - 1)];
                    float curr = measured[(size_t) i];
                    if (prev <= 0.0f && curr > 0.0f)
                    {
                        double frac = (curr != prev) ? ((double) (0.0f - prev) / (double) (curr - prev)) : 0.0;
                        crossings.push_back ((double) (i - 1) + frac);
                    }
                }

                double measuredFreqZc = 0.0;
                if (crossings.size() >= 2)
                {
                    double totalSamples = crossings.back() - crossings.front();
                    double numCycles = (double) crossings.size() - 1.0;
                    measuredFreqZc = numCycles * sampleRate / totalSamples;
                }

                const double errPct = 100.0 * (measuredFreqZc - expectedFreq) / expectedFreq;
                logMessage ("DIAGNOSTIC sweep: semitones=" + juce::String (semitones) + ", ratio="
                            + juce::String (shifter.getPitchRatio(), 6) + ", expected=" + juce::String (expectedFreq)
                            + "Hz, measured(ZC)=" + juce::String (measuredFreqZc) + "Hz, error=" + juce::String (errPct, 3)
                            + "%, numCrossings=" + juce::String ((int) crossings.size())
                            + ", (ratio-1)=" + juce::String (shifter.getPitchRatio() - 1.0f, 6));
            }
        }

        beginTest ("DIAGNOSTIC: measured output frequency for a pure sine tone matches pitchRatio * inputFreq");
        {
            // Temporary diagnostic added while investigating a user report
            // that the shimmer "still sounds detuned" even after the 4-voice
            // fix. All prior tests check boundedness/gain/crossfade-shape --
            // none of them actually verify the shifted PITCH is correct, so
            // this measures it directly: feed a known-frequency sine tone,
            // measure the steady-state output frequency via autocorrelation,
            // and compare against inputFreq * pitchRatio.
            constexpr double sampleRate = 44100.0;
            constexpr float inputFreq = 220.0f;

            PitchShifter shifter;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, 1 };
            shifter.prepare (spec);
            shifter.reset();
            shifter.setPitchShiftSemitones (12.0f); // ratio = 2.0, the default shift
            const float expectedFreq = inputFreq * shifter.getPitchRatio();

            constexpr int warmupSamples = (int) (sampleRate * 0.5); // fill delay line + let onset settle
            constexpr int measureSamples = (int) (sampleRate * 1.0);

            double phase = 0.0;
            const double phaseInc = juce::MathConstants<double>::twoPi * inputFreq / sampleRate;

            for (int i = 0; i < warmupSamples; ++i)
            {
                shifter.processSample ((float) std::sin (phase) * 0.5f);
                phase += phaseInc;
                if (phase >= juce::MathConstants<double>::twoPi)
                    phase -= juce::MathConstants<double>::twoPi;
            }

            std::vector<float> measured ((size_t) measureSamples);
            for (int i = 0; i < measureSamples; ++i)
            {
                measured[(size_t) i] = shifter.processSample ((float) std::sin (phase) * 0.5f);
                phase += phaseInc;
                if (phase >= juce::MathConstants<double>::twoPi)
                    phase -= juce::MathConstants<double>::twoPi;
            }

            // Autocorrelation over a plausible lag range around the expected
            // period, +/-40% margin, with parabolic interpolation for
            // sub-sample lag precision.
            const int expectedPeriodSamples = juce::roundToInt ((float) sampleRate / expectedFreq);
            const int minLag = juce::jmax (2, (int) (expectedPeriodSamples * 0.6));
            const int maxLag = (int) (expectedPeriodSamples * 1.4);
            const int corrWindow = measureSamples - maxLag;

            int bestLag = minLag;
            double bestCorr = -1.0e300;

            for (int lag = minLag; lag <= maxLag; ++lag)
            {
                double sum = 0.0;
                for (int i = 0; i < corrWindow; ++i)
                    sum += (double) measured[(size_t) i] * (double) measured[(size_t) (i + lag)];

                if (sum > bestCorr)
                {
                    bestCorr = sum;
                    bestLag = lag;
                }
            }

            auto corrAt = [&] (int lag)
            {
                double sum = 0.0;
                for (int i = 0; i < corrWindow; ++i)
                    sum += (double) measured[(size_t) i] * (double) measured[(size_t) (i + lag)];
                return sum;
            };

            double refinedLag = (double) bestLag;
            if (bestLag > minLag && bestLag < maxLag)
            {
                const double cPrev = corrAt (bestLag - 1);
                const double cCurr = bestCorr;
                const double cNext = corrAt (bestLag + 1);
                const double denom = (cPrev - 2.0 * cCurr + cNext);
                if (std::abs (denom) > 1.0e-12)
                    refinedLag = (double) bestLag + 0.5 * (cPrev - cNext) / denom;
            }

            const double measuredFreq = sampleRate / refinedLag;
            const double errorPercent = 100.0 * (measuredFreq - expectedFreq) / expectedFreq;

            // Cross-check via an independent method (direct Goertzel-style
            // spectral magnitude scan, fine-grained around expectedFreq) --
            // autocorrelation can be biased by amplitude modulation/phase
            // discontinuities from the crossfade, so if this disagrees with
            // the ACF result above, the ACF number is measurement noise, not
            // a real pitch error.
            double bestMag = -1.0;
            double bestFreq = expectedFreq;
            for (double testFreq = expectedFreq * 0.6; testFreq <= expectedFreq * 1.4; testFreq += 0.25)
            {
                const double w = juce::MathConstants<double>::twoPi * testFreq / sampleRate;
                double sumCos = 0.0, sumSin = 0.0;
                for (int i = 0; i < measureSamples; ++i)
                {
                    sumCos += (double) measured[(size_t) i] * std::cos (w * (double) i);
                    sumSin += (double) measured[(size_t) i] * std::sin (w * (double) i);
                }
                const double mag = sumCos * sumCos + sumSin * sumSin;
                if (mag > bestMag)
                {
                    bestMag = mag;
                    bestFreq = testFreq;
                }
            }
            const double spectralErrorPercent = 100.0 * (bestFreq - expectedFreq) / expectedFreq;

            logMessage ("DIAGNOSTIC: inputFreq=" + juce::String (inputFreq) + "Hz, pitchRatio="
                        + juce::String (shifter.getPitchRatio()) + ", expectedFreq=" + juce::String (expectedFreq)
                        + "Hz, ACF measuredFreq=" + juce::String (measuredFreq) + "Hz (error "
                        + juce::String (errorPercent, 3) + "%), spectral-peak freq=" + juce::String (bestFreq)
                        + "Hz (error " + juce::String (spectralErrorPercent, 3) + "%)");

            expect (std::abs (errorPercent) < 1.0, "Measured output frequency (" + juce::String (measuredFreq)
                        + "Hz) deviates from expected (" + juce::String (expectedFreq) + "Hz) by "
                        + juce::String (errorPercent, 3) + "% -- this is a REAL pitch error, not just a timbral "
                        "artifact");
        }
    }
};

static PitchShifterTests pitchShifterTests;
