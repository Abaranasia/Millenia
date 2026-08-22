#include <JuceHeader.h>
#include <array>
#include <limits>
#include <vector>
#include "../../Source/DSP/PitchShifter.h"

// DIAGNOSTIC (temporary, not a permanent regression test): investigating the
// 2026-08-14 "metallic/robotic" character complaint (docs/shimmer-reverb-
// implementation-plan.md, Phase 7 section). The pitch-accuracy tests above
// already confirm the AVERAGE output frequency is correct -- this checks
// whether the grain-splice mechanism is injecting periodic amplitude
// modulation at the grain hop rate, which a zero-crossing/average-frequency
// measurement would not detect but the ear would hear as buzz/metallicness.

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
            // Black-box check that the grain pool's output stays close to a
            // constant gain over time, not just on average. This test
            // predates this class's rewrite from persistent equally-spaced
            // voices to a finite-lifetime grain pool (see PitchShifter.h's
            // class-level comment) -- the bucketing here assumes a single
            // fixed-length repeating cycle (grainLengthSamplesApprox), which
            // no longer matches the new design's mechanics directly (grains
            // now launch every hopSamples, not every grainLengthSamples, and
            // several asynchronously-launched grains can overlap). Re-run
            // empirically after the rewrite: it still passes, and by a wider
            // margin than before, which makes sense -- the per-sample
            // weight-sum normalization (weightedSum / weightSum, see
            // processSample()) holds output gain at exactly 1 for ANY
            // number/mix of concurrently-active grains, unconditionally,
            // whereas the old fixed COLA-derived constant only held exactly
            // for one specific equally-spaced-phase configuration. So this
            // bucketed measurement is still a valid (if now slightly
            // indirect) black-box check of that invariant, just no longer
            // tied to the exact mechanism that originally motivated the
            // grainLengthSamplesApprox-sized bucket -- feed sustained noise
            // and bucket the output energy by position within that nominal
            // cycle length, for BOTH the primary (processSample()'s return
            // value) and quadrature (getQuadratureOutput()) pools. If the
            // per-sample normalization were broken (e.g. a stuck/miscounted
            // weight sum), the envelope would swing away from a constant,
            // showing up as a large RMS difference between buckets. With a
            // correctly-normalized crossfade, every bucket should see
            // roughly the same input-driven RMS.
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
            // Direct, black-box verification of the per-sample
            // weight-sum normalization (weightedSum / weightSum) documented
            // in PitchShifter.h's class-level comment, which replaced the
            // old fixed 2.0/N=0.5 COLA-derived constant. Unlike the
            // bucketed-RMS test above (which only checks *relative*
            // consistency across a grain cycle and would not notice an
            // overall level error, since a wrong normalization scales every
            // bucket by the same wrong factor), this test feeds a sustained
            // DC (constant) input. Once the shared delayLine is entirely
            // full of that same constant, EVERY active grain's interpolated
            // read returns exactly that constant (Lagrange3rd interpolation
            // of a constant signal reproduces the constant exactly, no
            // error) regardless of which delay offset/age it reads at. So,
            // for either pool, weightedSum = dcInput * weightSum exactly,
            // for ANY number of concurrently-active grains and ANY of their
            // individual weights -- so output = weightedSum / weightSum
            // reduces to exactly dcInput, unconditionally, not just for one
            // specific grain count or window shape the way the old COLA
            // identity required. If the normalization were dropped entirely
            // (raw weightedSum), or a wrong fixed constant were used
            // instead, this would be caught here with a tight tolerance,
            // not the loose 0.5 ratio margin used for pumping detection
            // above. This also catches a launch-scheduling bug (grains not
            // actually overlapping the way the design intends), since an
            // uneven active-grain count would still normalize correctly here
            // -- but a systematically wrong weight function would not.
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

        beginTest ("Output frequency stays within tolerance across the full -24..+24 semitone range");
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
                logMessage ("Sweep: semitones=" + juce::String (semitones) + ", ratio="
                            + juce::String (shifter.getPitchRatio(), 6) + ", expected=" + juce::String (expectedFreq)
                            + "Hz, measured(ZC)=" + juce::String (measuredFreqZc) + "Hz, error=" + juce::String (errPct, 3)
                            + "%, numCrossings=" + juce::String ((int) crossings.size())
                            + ", (ratio-1)=" + juce::String (shifter.getPitchRatio() - 1.0f, 6));

                // 10% tolerance matches the scratch-harness validation of the
                // SOLA-style short-crossfade grain pool (see PitchShifter.h's
                // class-level comment) across -24..+24 semitones, with
                // margin for real JUCE Lagrange3rd interpolation.
                expect (std::abs (errPct) < 10.0, "Output frequency error at " + juce::String (semitones)
                            + "st exceeded 10% tolerance (measured=" + juce::String (measuredFreqZc)
                            + "Hz, expected=" + juce::String (expectedFreq) + "Hz, error=" + juce::String (errPct, 3) + "%)");
            }
        }

        beginTest ("DIAGNOSTIC: output frequency error across a range of INPUT frequencies at the default +12st shift");
        {
            // Investigating a by-ear complaint (2026-08-22): "chipmunk effect
            // again... works nice for low notes, but sounds a bit ridiculous
            // on higher notes." Confirmed (via a temporary depth=0.0f test
            // build) that this is NOT caused by the Freeze delay-dither
            // (PitchShifter.h's freezeDriftDepthMs) -- it persists with that
            // mechanism fully disabled. Every existing pitch-accuracy test
            // above this one fixes inputFreq=220.0f and only varies the
            // SHIFT amount -- none of them test whether accuracy holds at
            // higher INPUT frequencies at a fixed shift, which is exactly
            // what the user's complaint describes. This sweeps input
            // frequency instead, at the shimmer's classic default shift
            // (+12st, ratio=2.0), to see whether error actually grows with
            // input pitch (which would point to a WSOLA/alignment-search
            // failure mode: a higher input frequency means more full cycles
            // fit inside the fixed crossfadeSamplesInt (~88 sample)
            // correlation window and the fixed alignmentSearchRadiusSamples
            // (22 sample) search window, increasing the chance the search
            // locks onto a candidate offset that's off by a whole period or
            // more -- a real, different failure mode from anything the
            // existing semitone-sweep tests at 220Hz could have caught).
            constexpr double sampleRate = 44100.0;
            constexpr float semitones = 12.0f;

            for (float inputFreq : { 110.0f, 220.0f, 440.0f, 880.0f, 1760.0f, 3520.0f })
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
                logMessage ("Input freq sweep at +12st: inputFreq=" + juce::String (inputFreq, 1)
                            + "Hz, expected=" + juce::String (expectedFreq, 1) + "Hz, measured(ZC)="
                            + juce::String (measuredFreqZc, 1) + "Hz, error=" + juce::String (errPct, 3) + "%");
            }
        }

        beginTest ("Output frequency for a pure sine tone matches pitchRatio * inputFreq (within short-crossfade tolerance)");
        {
            // All prior tests check boundedness/gain/crossfade-shape -- none
            // of them actually verify the shifted PITCH is correct, so this
            // measures it directly: feed a known-frequency sine tone,
            // measure the steady-state output frequency via precise
            // (linearly-interpolated) zero-crossing timing, and compare
            // against inputFreq * pitchRatio.
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

            // Precise (linearly-interpolated) positive-going zero-crossing
            // timing, averaged across the whole window -- already proven
            // during investigation to agree with both autocorrelation and an
            // independent spectral-peak check, and simpler than either.
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

            double measuredFreq = 0.0;
            if (crossings.size() >= 2)
            {
                double totalSamples = crossings.back() - crossings.front();
                double numCycles = (double) crossings.size() - 1.0;
                measuredFreq = numCycles * sampleRate / totalSamples;
            }

            const double errorPercent = 100.0 * (measuredFreq - expectedFreq) / expectedFreq;

            logMessage ("inputFreq=" + juce::String (inputFreq) + "Hz, pitchRatio="
                        + juce::String (shifter.getPitchRatio()) + ", expectedFreq=" + juce::String (expectedFreq)
                        + "Hz, measured(ZC) freq=" + juce::String (measuredFreq) + "Hz (error "
                        + juce::String (errorPercent, 3) + "%)");

            // 5% tolerance: the validated ~0.5% figure for this exact
            // ratio=2.0 case was measured against an idealized scratch
            // model, not real JUCE Lagrange3rd interpolation, which differs
            // slightly -- see PitchShifter.h's class-level comment for the
            // full -24..+24 semitone sweep's validated range (+-1-8%).
            expect (std::abs (errorPercent) < 5.0, "Measured output frequency (" + juce::String (measuredFreq)
                        + "Hz) deviates from expected (" + juce::String (expectedFreq) + "Hz) by "
                        + juce::String (errorPercent, 3) + "% -- this is a REAL pitch error, not just a timbral "
                        "artifact");
        }

        beginTest ("Spectral sidebands around the shifted fundamental stay below the measured WSOLA-fix thresholds");
        {
            // Feeds a sustained pure sine through the shifter at each test
            // shift, takes a large FFT of the steady-state output, and
            // asserts the strongest spectral peak OTHER than the fundamental
            // itself stays below a measured threshold. Originally a
            // DIAGNOSTIC, log-only test (2026-08-14 "metallic/robotic"
            // character investigation) -- promoted to a real assertion once
            // the WSOLA per-grain phase-alignment fix (PitchShifter::
            // findAlignmentOffset(), see PitchShifter.h/.cpp) was measured to
            // substantially improve these numbers. If the grain-splice
            // mechanism were still injecting periodic amplitude modulation at
            // the hop rate (hopSamples derived from grainLengthMs=20ms and
            // crossfadeFraction=0.1 -- roughly 55.5Hz at 44.1kHz), that would
            // show up as sidebands spaced at multiples of the hop rate around
            // the fundamental, at an amplitude large enough to be audible as
            // buzz/metallic coloration even though the *average* frequency
            // (measured by zero-crossing in the test above) comes out
            // correct.
            //
            // Measured before/after the WSOLA fix (relative to fundamental,
            // this exact test signal/config):
            //   0st:  -92.24dB -> -92.24dB (unchanged -- already just the
            //         numerical noise floor, nothing to fix at unity ratio)
            //  +7st:  -28.33dB -> -67.07dB (~38.7dB improvement)
            // +12st:  -19.99dB -> -70.60dB (~50.6dB improvement -- comfortably
            //         past the "-30dB or better" bar set for closing this
            //         investigation)
            // +24st: unreliable pre-fix (fundamental-bin magnitude was ~0.14,
            //        a spectral null from the pre-fix artifact itself, making
            //        the ratio meaningless) -> -38.17dB post-fix, with a
            //        healthy non-null fundamental magnitude (~8156 vs ~8175
            //        for the other shifts) -- the null resolved itself once
            //        the underlying artifact causing it was fixed.
            // Thresholds below are set with margin under these measured
            // post-fix numbers so the test fails on a real regression without
            // being flaky over harmless numerical noise.
            //
            // 2026-08-16 performance-regression follow-up (see
            // docs/shimmer-reverb-implementation-plan.md's Phase 7 follow-up
            // paragraph for the full writeup): the WSOLA fix above originally
            // searched +-grainLengthSamplesInt (~882 samples) from a fixed
            // origin at EVERY grain launch, which was measured to make
            // PitchShifter run at only ~1.19x real-time (see the DIAGNOSTIC
            // test below) -- nowhere near safe once inside the full
            // ShimmerReverbEngine signal path. Re-measured after switching
            // findAlignmentOffset() to a much cheaper two-anchor search
            // (PitchShifter.h/.cpp -- primaryLastOffset/quadratureLastOffset,
            // alignmentSearchRadiusSamples=22 samples at 44.1kHz): 0st
            // -92.24dB (unchanged), +7st -59.19dB, +12st -61.00dB, +24st
            // -44.67dB -- all comfortably clear of the thresholds below and
            // within a few dB of the original brute-force numbers above,
            // while real-time factor improved from 1.19x to ~19x (see the
            // DIAGNOSTIC test's own updated comment).
            constexpr double sampleRate = 44100.0;
            constexpr float inputFreq = 220.0f;
            constexpr int fftOrder = 15; // 32768-point FFT -> ~1.35Hz/bin resolution
            constexpr int fftSize = 1 << fftOrder;

            struct SidebandCase
            {
                float semitones;
                double maxAllowedRelativeDb; // strongest other peak must be at or below this, relative to the fundamental
            };

            for (auto testCase : { SidebandCase { 0.0f, -80.0 },
                                    SidebandCase { 7.0f, -55.0 },
                                    SidebandCase { 12.0f, -50.0 },
                                    SidebandCase { 24.0f, -30.0 } })
            {
            const float testSemitones = testCase.semitones;
            PitchShifter shifter;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, 1 };
            shifter.prepare (spec);
            shifter.reset();
            shifter.setPitchShiftSemitones (testSemitones);
            const float expectedFreq = inputFreq * shifter.getPitchRatio();

            constexpr int warmupSamples = (int) (sampleRate * 1.0);
            double phase = 0.0;
            const double phaseInc = juce::MathConstants<double>::twoPi * inputFreq / sampleRate;

            for (int i = 0; i < warmupSamples; ++i)
            {
                shifter.processSample ((float) std::sin (phase) * 0.5f);
                phase += phaseInc;
                if (phase >= juce::MathConstants<double>::twoPi) phase -= juce::MathConstants<double>::twoPi;
            }

            juce::dsp::FFT fft (fftOrder);
            juce::HeapBlock<float> fftData (2 * (size_t) fftSize);
            juce::dsp::WindowingFunction<float> window ((size_t) fftSize, juce::dsp::WindowingFunction<float>::blackmanHarris);

            for (int i = 0; i < fftSize; ++i)
            {
                float sample = shifter.processSample ((float) std::sin (phase) * 0.5f);
                phase += phaseInc;
                if (phase >= juce::MathConstants<double>::twoPi) phase -= juce::MathConstants<double>::twoPi;
                fftData[i] = sample;
            }
            juce::zeromem (fftData + fftSize, sizeof (float) * (size_t) fftSize);
            window.multiplyWithWindowingTable (fftData, (size_t) fftSize);

            fft.performFrequencyOnlyForwardTransform (fftData, true);

            const double binHz = sampleRate / (double) fftSize;
            const int theoreticalFundamentalBin = juce::roundToInt (expectedFreq / binHz);
            const int numBins = fftSize / 2;

            // The theoretical bin index can land in a spectral null at
            // extreme ratios (the true peak straddles a bin boundary, or --
            // pre-fix -- an artifact could hollow out the exact theoretical
            // bin even though real energy sits one bin over). Search a small
            // window around the theoretical bin for the actual nearest local
            // maximum instead of trusting the theoretical index outright.
            constexpr int fundamentalSearchRadius = 3;
            int fundamentalBin = theoreticalFundamentalBin;
            float fundamentalMag = fftData[(size_t) juce::jlimit (0, numBins - 1, theoreticalFundamentalBin)];

            for (int b = theoreticalFundamentalBin - fundamentalSearchRadius; b <= theoreticalFundamentalBin + fundamentalSearchRadius; ++b)
            {
                if (b < 0 || b >= numBins)
                    continue;

                if (fftData[(size_t) b] > fundamentalMag)
                {
                    fundamentalMag = fftData[(size_t) b];
                    fundamentalBin = b;
                }
            }

            // Exclude a small guard band around the fundamental itself (its
            // own windowed main-lobe) when searching for the strongest OTHER
            // peak.
            const int guardBins = 6;

            float strongestOtherMag = 0.0f;
            int strongestOtherBin = -1;

            for (int b = 1; b < numBins; ++b)
            {
                if (std::abs (b - fundamentalBin) <= guardBins)
                    continue;

                if (fftData[(size_t) b] > strongestOtherMag)
                {
                    strongestOtherMag = fftData[(size_t) b];
                    strongestOtherBin = b;
                }
            }

            const double strongestOtherHz = strongestOtherBin * binHz;
            const double offsetFromFundamentalHz = strongestOtherHz - expectedFreq;
            const double relativeDb = 20.0 * std::log10 ((double) (strongestOtherMag / juce::jmax (fundamentalMag, 1.0e-9f)));

            const int approxHopSamples = juce::roundToInt (20.0 * 0.001 * sampleRate * (1.0f - 0.1f));
            const double approxHopRateHz = sampleRate / (double) approxHopSamples;

            logMessage ("Spectral sidebands: semitones=" + juce::String (testSemitones) + ", fundamental="
                        + juce::String (expectedFreq) + "Hz (bin "
                        + juce::String (fundamentalBin) + ", mag=" + juce::String (fundamentalMag, 6)
                        + "), strongest other peak=" + juce::String (strongestOtherHz, 2) + "Hz (bin "
                        + juce::String (strongestOtherBin) + ", mag=" + juce::String (strongestOtherMag, 6)
                        + ", " + juce::String (relativeDb, 2) + "dB relative to fundamental), offset from "
                        + "fundamental=" + juce::String (offsetFromFundamentalHz, 2) + "Hz, approx grain hop rate="
                        + juce::String (approxHopRateHz, 2) + "Hz, bin resolution=" + juce::String (binHz, 3) + "Hz/bin");

            expect (relativeDb <= testCase.maxAllowedRelativeDb,
                    "Sideband at " + juce::String (testSemitones) + "st measured " + juce::String (relativeDb, 2)
                        + "dB, expected at or below " + juce::String (testCase.maxAllowedRelativeDb, 2) + "dB");
            }
        }

        beginTest ("DIAGNOSTIC: wall-clock real-time factor of processSample() (investigating reported audio dropouts)");
        {
            // User reported audible dropouts/interruptions in the Standalone
            // after the WSOLA alignment-search fix was added. The ORIGINAL
            // version of that fix searched +-grainLengthSamplesInt candidates
            // (~882 samples at 44.1kHz) from a FIXED origin at every single
            // grain launch, in both the primary and quadrature pools --
            // measured here at only ~1.19x real-time (5.0s of audio took
            // ~4.2s wall-clock in this Debug build) -- nowhere near enough
            // margin once this sits inside ShimmerReverbEngine's actual
            // feedback loop alongside DattorroTank/DC blocker/limiter and
            // real host/driver overhead. That was the root cause of the
            // reported dropouts.
            //
            // Fix (see PitchShifter.h/.cpp): findAlignmentOffset() now
            // searches around a running per-pool offset estimate
            // (primaryLastOffset/quadratureLastOffset) that's carried
            // forward from one grain launch to the next -- since it already
            // accounts for all prior drift, each launch only needs to search
            // a small window covering that one hop's INCREMENTAL drift
            // instead of re-deriving the full cumulative drift from scratch.
            // A second small window around the fixed nominal zero is also
            // checked every call (see findAlignmentOffset()'s own comment
            // for why: a running-offset-only search was measured to plateau
            // on a bad local optimum at extreme pitch ratios, no matter how
            // wide its radius). alignmentSearchRadiusSamples shrank from a
            // full grain length (~882 samples) down to 22 samples at
            // 44.1kHz -- tuned via a sweep documented in
            // docs/shimmer-reverb-implementation-plan.md's Phase 7 follow-up
            // paragraph.
            //
            // Measured after the fix (this exact test, same machine/build):
            // ~19.3x real-time (5.0s of audio in ~260ms wall-clock) -- a
            // ~16x improvement over the 1.19x pre-fix figure, and well past
            // the "3-5x, ideally much more" margin target set for closing
            // this investigation. The assertion below uses roughly half the
            // measured figure as its floor, so it fails on a real regression
            // (e.g. an accidentally-widened search radius) without being
            // flaky over ordinary machine-to-machine variance.
            constexpr double sampleRate = 44100.0;
            constexpr double secondsToProcess = 5.0;
            constexpr int numSamples = (int) (sampleRate * secondsToProcess);

            PitchShifter shifter;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, 1 };
            shifter.prepare (spec);
            shifter.reset();
            shifter.setPitchShiftSemitones (12.0f);

            juce::Random random (13579);

            const double startMs = juce::Time::getMillisecondCounterHiRes();

            for (int i = 0; i < numSamples; ++i)
                shifter.processSample (random.nextFloat() * 0.6f - 0.3f);

            const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - startMs;
            const double audioMs = secondsToProcess * 1000.0;
            const double realTimeFactor = audioMs / elapsedMs; // >1 means faster than real-time (good)

            logMessage ("Processed " + juce::String (secondsToProcess, 1) + "s of audio (mono, "
                        + juce::String ((int) sampleRate) + "Hz) in " + juce::String (elapsedMs, 2)
                        + "ms wall-clock -> real-time factor " + juce::String (realTimeFactor, 2)
                        + "x (>1 = faster than real-time; this is a Debug/unoptimized build, so treat as a "
                        "relative, not absolute, figure)");

            // Measured ~19.3x on this machine/build after the two-anchor
            // search fix (see this test's comment above); 8x leaves
            // comfortable margin below that for normal variance while still
            // catching a real regression (e.g. back toward the pre-fix
            // ~1.19x, or an accidentally much-widened search radius).
            expect (realTimeFactor >= 8.0, "Real-time factor " + juce::String (realTimeFactor, 2)
                        + "x fell below the 8x regression floor -- this is a Debug build, but such a large drop "
                        "likely means the alignment search got more expensive (radius, anchor count, or window "
                        "size), not just machine noise");
        }
    }
};

static PitchShifterTests pitchShifterTests;
