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

        beginTest ("DIAGNOSTIC: primary vs. quadrature difference on a pure periodic tone, bypassing DattorroTank entirely");
        {
            // "Shimmer Sustain" glitch investigation follow-up (2026-09-06):
            // ShimmerReverbEngineTests.cpp's own diagnostic measured
            // peak|wetLeft-wetRight| = 2.0 (safeFeedback and quadratureSafe
            // landing at full opposite-polarity extremes) at high Shimmer
            // Sustain + high Width + high Shimmer Amount, vs. only 0.3 at low
            // Sustain, for the SAME 220Hz tone input through the WHOLE
            // engine. Question this test answers: is that difference
            // inherent to PitchShifter's primary/quadrature pair on a clean
            // periodic tone BY ITSELF (i.e. Sustain=high just lets
            // DattorroTank's own recirculation become closer to a clean tone,
            // exposing a pre-existing PitchShifter property), or does it only
            // appear once DattorroTank's feedback loop is involved? Bypasses
            // DattorroTank/ShimmerReverbEngine completely -- feeds a bare
            // 220Hz sine directly into PitchShifter::processSample() at the
            // default 12st shift, same as the engine's own default.
            constexpr double sampleRate = 44100.0;
            constexpr float toneFrequencyHz = 220.0f;
            constexpr double totalSeconds = 6.0; // matches the engine-level diagnostic's own duration

            PitchShifter shifter;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, 1 };
            shifter.prepare (spec);
            shifter.reset();
            shifter.setPitchShiftSemitones (12.0f);

            const int totalSamples = (int) (totalSeconds * sampleRate);
            double phase = 0.0;
            const double phaseIncrement = 2.0 * juce::MathConstants<double>::pi * toneFrequencyHz / sampleRate;

            float peakPrimary = 0.0f;
            float peakQuadrature = 0.0f;
            float peakDiff = 0.0f;

            for (int i = 0; i < totalSamples; ++i)
            {
                const float input = 0.3f * (float) std::sin (phase);
                phase += phaseIncrement;

                float primary = shifter.processSample (input);
                float quadrature = shifter.getQuadratureOutput();

                expect (std::isfinite (primary) && std::isfinite (quadrature),
                        "Non-finite output at sample " + juce::String (i));

                peakPrimary    = juce::jmax (peakPrimary, std::abs (primary));
                peakQuadrature = juce::jmax (peakQuadrature, std::abs (quadrature));
                peakDiff       = juce::jmax (peakDiff, std::abs (primary - quadrature));
            }

            logMessage ("Bare PitchShifter, 220Hz tone direct input (no tank), 12st, 6s: peakPrimary="
                            + juce::String (peakPrimary, 4) + ", peakQuadrature=" + juce::String (peakQuadrature, 4)
                            + ", peak|primary-quadrature|=" + juce::String (peakDiff, 4));
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

        beginTest ("DIAGNOSTIC: primary alignment-offset stability at +-12st vs +-24st (investigating a by-ear "
                   "'pitch oscillation' report, worse at +-12st than +-24st)");
        {
            // Investigating a 2026-08-23 by-ear report (docs/formant-preserving-
            // pitch-shifter-research.md section 10): "pitch oscillation" heard
            // at +-12st, MORE noticeable than at +-24st -- the opposite of what
            // you'd naively expect if artifacts simply worsened with shift
            // amount.
            //
            // Hypothesis: findAlignmentOffset() (PitchShifter.cpp) picks each
            // grain's launch offset via normalized cross-correlation, checked
            // around TWO competing anchors every single hop -- the running
            // previousOffset (continuous-drift tracking) and the fixed nominal
            // zero (see that function's own comment). For a sustained, purely
            // periodic tone (a held note), the correlation window can score
            // near-identically at both anchors whenever the underlying dry
            // buffer's own period lines them up -- and at exactly an octave
            // (+-12st, pitchRatio=2.0 or 0.5), that per-hop tie is the most
            // likely to recur, hop after hop, because an octave is the
            // simplest possible pitch relationship. If true, the chosen offset
            // should visibly FLIP-FLOP between the two anchors' neighborhoods
            // at +-12st, while drifting more smoothly (or settling) at +-24st.
            //
            // This test does NOT assert a pass/fail bound yet -- it exists to
            // confirm or refute the mechanism with real measurement before any
            // fix is attempted, per this project's own "measure, don't
            // guess-and-patch" convention (see section 10's decision note).
            constexpr double sampleRate = 44100.0;
            constexpr float inputFreq = 220.0f;

            for (float semitones : { 12.0f, -12.0f, 24.0f, -24.0f })
            {
                PitchShifter shifter;
                juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, 1 };
                shifter.prepare (spec);
                shifter.reset();
                shifter.setPitchShiftSemitones (semitones);

                constexpr int warmupSamples = (int) (sampleRate * 0.5);
                // 10s (not the usual 1-3s) -- the first pass at 3s only caught
                // 1-2 big-jump events at -12st/-24st, not enough to tell a
                // genuinely periodic reset apart from a one-off settling
                // transient. This gives room for several cycles even at a
                // slow reset rate.
                constexpr int measureSamples = (int) (sampleRate * 10.0);

                double phase = 0.0;
                const double phaseInc = juce::MathConstants<double>::twoPi * inputFreq / sampleRate;

                for (int i = 0; i < warmupSamples; ++i)
                {
                    shifter.processSample ((float) std::sin (phase) * 0.5f);
                    phase += phaseInc;
                    if (phase >= juce::MathConstants<double>::twoPi) phase -= juce::MathConstants<double>::twoPi;
                }

                // primaryLastOffset only changes at each grain launch (once
                // per hopSamples); sampling every processSample() call and
                // recording only the VALUE CHANGES reconstructs the actual
                // per-launch offset sequence without needing to know the
                // exact hop timing here.
                std::vector<float> launchOffsets;
                std::vector<std::array<float, 2>> launchAnchorScores;
                float lastSeen = shifter.getPrimaryLastOffset();
                launchOffsets.push_back (lastSeen);
                launchAnchorScores.push_back (shifter.getPrimaryAnchorScores());

                for (int i = 0; i < measureSamples; ++i)
                {
                    shifter.processSample ((float) std::sin (phase) * 0.5f);
                    phase += phaseInc;
                    if (phase >= juce::MathConstants<double>::twoPi) phase -= juce::MathConstants<double>::twoPi;

                    const float current = shifter.getPrimaryLastOffset();
                    if (current != lastSeen)
                    {
                        launchOffsets.push_back (current);
                        launchAnchorScores.push_back (shifter.getPrimaryAnchorScores());
                        lastSeen = current;
                    }
                }

                // Flip-flop indicator: a smoothly-drifting (or settled)
                // offset keeps the SAME sign of successive deltas for many
                // launches in a row; a search alternating between two
                // competing anchors reverses sign almost every launch. Count
                // sign reversals as a fraction of all delta pairs.
                int signReversals = 0;
                int deltaPairs = 0;
                float maxAbsDelta = 0.0f;

                for (size_t i = 2; i < launchOffsets.size(); ++i)
                {
                    const float deltaPrev = launchOffsets[i - 1] - launchOffsets[i - 2];
                    const float deltaCurr = launchOffsets[i] - launchOffsets[i - 1];
                    maxAbsDelta = juce::jmax (maxAbsDelta, std::abs (deltaCurr));

                    if (std::abs (deltaPrev) > 1.0e-3f && std::abs (deltaCurr) > 1.0e-3f)
                    {
                        ++deltaPairs;
                        if ((deltaPrev > 0.0f) != (deltaCurr > 0.0f))
                            ++signReversals;
                    }
                }

                const double reversalFraction = deltaPairs > 0 ? (double) signReversals / (double) deltaPairs : 0.0;

                // Large-jump recurrence: distinguishes a one-off settling
                // transient (e.g. right after warmup, before the running
                // offset has converged) from a swing that keeps recurring
                // throughout the measurement window -- only the latter could
                // explain a perceived, ongoing "oscillation" on a sustained
                // held note. Threshold of 100 samples is well above
                // alignmentSearchRadiusSamples (22) -- a jump this size means
                // the search actually switched which anchor's neighborhood it
                // landed in, not just noise within one anchor's own window.
                std::vector<int> bigJumpLaunchIndices;
                for (size_t i = 1; i < launchOffsets.size(); ++i)
                    if (std::abs (launchOffsets[i] - launchOffsets[i - 1]) > 100.0f)
                        bigJumpLaunchIndices.push_back ((int) i);

                juce::String bigJumpList;
                for (int idx : bigJumpLaunchIndices)
                    bigJumpList += juce::String (idx) + " ";

                logMessage ("Offset stability at " + juce::String (semitones) + "st: " + juce::String ((int) launchOffsets.size())
                            + " launches, " + juce::String (signReversals) + "/" + juce::String (deltaPairs)
                            + " sign reversals (" + juce::String (reversalFraction * 100.0, 1)
                            + "%), max per-launch delta=" + juce::String (maxAbsDelta, 2)
                            + " samples, big jumps (>100 samples) at launch indices: [" + bigJumpList.trim() + "]");

                // Anchor-crossover confirmation: for the first big jump found,
                // print BOTH anchors' scores for a small window of launches
                // straddling it. If the hypothesis is right, the zero anchor
                // ([1]) should be losing (lower score) right before the jump
                // and become the actual winner (its score >= the previousOffset
                // anchor's, [0]) right at/after it -- a genuine crossover, not
                // some other cause (e.g. a discontinuity in the reference
                // window itself).
                if (! bigJumpLaunchIndices.empty())
                {
                    const int jumpIdx = bigJumpLaunchIndices.front();
                    const int windowStart = juce::jmax (0, jumpIdx - 3);
                    const int windowEnd = juce::jmin ((int) launchOffsets.size() - 1, jumpIdx + 2);

                    logMessage ("  Anchor scores around first jump (launch " + juce::String (jumpIdx)
                                + ") at " + juce::String (semitones) + "st:");
                    for (int i = windowStart; i <= windowEnd; ++i)
                    {
                        const auto& scores = launchAnchorScores[(size_t) i];
                        logMessage ("    launch " + juce::String (i) + ": offset=" + juce::String (launchOffsets[(size_t) i], 2)
                                    + ", previousOffset-anchor score=" + juce::String (scores[0], 5)
                                    + ", zero-anchor score=" + juce::String (scores[1], 5)
                                    + (i == jumpIdx ? "  <-- jump lands here" : ""));
                    }
                }
            }
        }

        beginTest ("DIAGNOSTIC: correlation score-curve shape around the primary pool's live decision, at +-12st vs "
                   "+-24st (testing the octave-tie hypothesis for the 'pitch oscillation' report)");
        {
            // Directly measures the SHAPE of findAlignmentOffset()'s
            // underlying normalized cross-correlation score as a function of
            // candidate offset -- not just the two anchors' own small
            // +-alignmentSearchRadiusSamples windows -- via
            // debugScorePrimaryCandidateOffset() (TEST-ONLY, see
            // PitchShifter.h). Tests the DIAGNOSTIC offset-stability test's
            // own hypothesis above: that +-12st's flip-flopping is caused by
            // the score curve having multiple near-tied local maxima, one
            // per near-integer multiple of the 220Hz input's own period
            // (~200.45 samples) -- and that +-24st either doesn't have this
            // problem, or has it much less.
            constexpr double sampleRate = 44100.0;
            constexpr float inputFreq = 220.0f;
            const double periodSamples = sampleRate / (double) inputFreq;

            for (float semitones : { 12.0f, -12.0f, 24.0f, -24.0f })
            {
                PitchShifter shifter;
                juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, 1 };
                shifter.prepare (spec);
                shifter.reset();
                shifter.setPitchShiftSemitones (semitones);

                constexpr int warmupSamples = (int) (sampleRate * 0.5);
                double phase = 0.0;
                const double phaseInc = juce::MathConstants<double>::twoPi * inputFreq / sampleRate;

                for (int i = 0; i < warmupSamples; ++i)
                {
                    shifter.processSample ((float) std::sin (phase) * 0.5f);
                    phase += phaseInc;
                    if (phase >= juce::MathConstants<double>::twoPi) phase -= juce::MathConstants<double>::twoPi;
                }

                logMessage ("Score-curve sweep at " + juce::String (semitones) + "st (input period="
                            + juce::String ((float) periodSamples, 2) + " samples):");

                constexpr int hopsToSample = 6;
                constexpr int sweepRangeSamples = 900; // a bit over one grainLengthSamplesInt at 44.1kHz
                constexpr int sweepStepSamples = 4;

                for (int hopIndex = 0; hopIndex < hopsToSample; ++hopIndex)
                {
                    // Advance to the next launch boundary (getPrimaryLastOffset()
                    // changes at every launch, same detection idiom as the
                    // offset-stability test above) so each sweep happens right
                    // when a real decision is about to be made.
                    const float before = shifter.getPrimaryLastOffset();
                    int guard = 0;
                    while (shifter.getPrimaryLastOffset() == before && guard < 5000)
                    {
                        shifter.processSample ((float) std::sin (phase) * 0.5f);
                        phase += phaseInc;
                        if (phase >= juce::MathConstants<double>::twoPi) phase -= juce::MathConstants<double>::twoPi;
                        ++guard;
                    }

                    const float currentOffset = shifter.getPrimaryLastOffset();

                    std::vector<std::pair<float, float>> curve;
                    for (int s = -sweepRangeSamples; s <= sweepRangeSamples; s += sweepStepSamples)
                        curve.push_back ({ (float) s, shifter.debugScorePrimaryCandidateOffset ((float) s) });

                    float globalMax = -1.0f;
                    for (auto& p : curve)
                        globalMax = juce::jmax (globalMax, p.second);

                    juce::String peakList;
                    int peakCount = 0;
                    for (size_t i = 1; i + 1 < curve.size(); ++i)
                    {
                        if (curve[i].second >= curve[i - 1].second && curve[i].second >= curve[i + 1].second
                            && curve[i].second >= globalMax - 0.01f)
                        {
                            ++peakCount;
                            peakList += juce::String (curve[i].first, 0) + "(" + juce::String (curve[i].second, 4) + ") ";
                        }
                    }

                    logMessage ("  hop " + juce::String (hopIndex) + ": currentOffset=" + juce::String (currentOffset, 1)
                                + ", global max score=" + juce::String (globalMax, 4) + ", near-tied peaks (>=max-0.01): "
                                + juce::String (peakCount) + " -> " + peakList.trim());
                }
            }
        }

        // 2026-08-29 continuation of the "pitch oscillation" investigation
        // (docs/formant-preserving-pitch-shifter-research.md section 10). Every
        // measurement above this point uses a pure sine tone, which has an
        // exact, unbreakable correlation tie at any window length or ratio --
        // structurally unable to confirm (or refute) a mechanism that depends
        // on real, non-perfectly-periodic program material actually differing
        // from a lab tone. Reruns the exact same offset-stability measurement
        // as the sine-tone DIAGNOSTIC test above, driven by a real recording,
        // to see whether the anchor-flip-flop mechanism looks any different on
        // real material. Factored into a lambda so multiple real-world test
        // files can be run through it -- whisper8.wav (aperiodic breath
        // content, confirmed by ear NOT to reproduce the original "oscillation"
        // percept -- no fundamental pitch to wobble, though it did surface a
        // real, distinct glitch at +24st) and a held tonal drone (the kind of
        // sustained, pitched material the original complaint was actually
        // reported against).
        auto runRealMaterialAlignmentDiagnostic = [this] (const juce::File& wavFile)
        {
            if (! wavFile.existsAsFile())
            {
                logMessage ("SKIPPED: " + wavFile.getFullPathName() + " not found on this machine -- this "
                            "DIAGNOSTIC test only runs where the user's own test recording is present.");
            }
            else
            {
                juce::AudioFormatManager formatManager;
                formatManager.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (wavFile));

                expect (reader != nullptr, "Failed to create an AudioFormatReader for " + wavFile.getFullPathName());

                if (reader != nullptr)
                {
                    const double sampleRate = reader->sampleRate;
                    const int numSourceSamples = (int) reader->lengthInSamples;

                    juce::AudioBuffer<float> source ((int) reader->numChannels, numSourceSamples);
                    reader->read (&source, 0, numSourceSamples, 0, true, true);

                    std::vector<float> mono ((size_t) numSourceSamples, 0.0f);
                    for (int ch = 0; ch < source.getNumChannels(); ++ch)
                    {
                        const float* channelData = source.getReadPointer (ch);
                        for (int i = 0; i < numSourceSamples; ++i)
                            mono[(size_t) i] += channelData[i];
                    }
                    if (source.getNumChannels() > 1)
                        for (auto& s : mono)
                            s /= (float) source.getNumChannels();

                    logMessage ("Loaded " + wavFile.getFullPathName() + ": " + juce::String (numSourceSamples)
                                + " samples, " + juce::String (source.getNumChannels()) + " ch, "
                                + juce::String (sampleRate, 0) + "Hz (" + juce::String ((float) (numSourceSamples / sampleRate), 2)
                                + "s)");

                    // 2026-08-29 new hypothesis (see findAlignmentOffset()'s
                    // REVERTED comment in PitchShifter.cpp): the previousOffset
                    // anchor's own +-alignmentSearchRadiusSamples local search
                    // was measured unstable in isolation, independent of the
                    // zero anchor entirely -- possibly because more than one
                    // real cycle of this content's own pitch period fits
                    // inside that single window, creating a genuine second
                    // competing peak there. Estimate the dry signal's own
                    // fundamental period via normalized autocorrelation (same
                    // idiom as PitchShifter's own scoreCandidateOffset(), just
                    // applied directly to the source instead of a shifted
                    // copy) and compare it against the live
                    // alignmentSearchRadiusSamples value, before touching any
                    // production code again.
                    {
                        const int periodAnalysisStart = juce::jmin ((int) (sampleRate * 0.5), numSourceSamples / 4);
                        const int periodAnalysisLength = juce::jmin ((int) (sampleRate * 0.3), numSourceSamples - periodAnalysisStart);

                        if (periodAnalysisLength > 0)
                        {
                            constexpr int minLagSamples = 20;
                            const int maxLagSamples = juce::jmin (2000, periodAnalysisLength / 2);

                            double bestCorrelation = -1.0;
                            int bestLag = minLagSamples;

                            for (int lag = minLagSamples; lag <= maxLagSamples; ++lag)
                            {
                                double dot = 0.0, energyA = 0.0, energyB = 0.0;
                                for (int i = 0; i < periodAnalysisLength - lag; ++i)
                                {
                                    const float a = mono[(size_t) (periodAnalysisStart + i)];
                                    const float b = mono[(size_t) (periodAnalysisStart + i + lag)];
                                    dot += (double) a * (double) b;
                                    energyA += (double) a * (double) a;
                                    energyB += (double) b * (double) b;
                                }
                                const double denom = std::sqrt (energyA * energyB);
                                const double correlation = denom > 1.0e-9 ? dot / denom : -1.0;

                                if (correlation > bestCorrelation)
                                {
                                    bestCorrelation = correlation;
                                    bestLag = lag;
                                }
                            }

                            PitchShifter radiusProbe;
                            juce::dsp::ProcessSpec radiusSpec { sampleRate, (juce::uint32) 512, 1 };
                            radiusProbe.prepare (radiusSpec);
                            const int searchRadius = radiusProbe.getAlignmentSearchRadiusSamples();
                            const int windowSpan = 2 * searchRadius;

                            logMessage ("Dry signal's own estimated fundamental period: " + juce::String (bestLag)
                                        + " samples (~" + juce::String (sampleRate / (double) bestLag, 1) + "Hz, autocorrelation="
                                        + juce::String (bestCorrelation, 4) + ") vs alignmentSearchRadiusSamples="
                                        + juce::String (searchRadius) + " (a single anchor's window spans " + juce::String (windowSpan)
                                        + " samples end-to-end) -- "
                                        + (bestLag < windowSpan
                                               ? "period is SMALLER than one anchor's own window span: more than one "
                                                 "real cycle can fit inside a single local search, a plausible "
                                                 "internal-multimodality mechanism"
                                               : "period is LARGER than one anchor's own window span: this specific "
                                                 "theory does not obviously apply here"));
                        }
                    }

                    for (float semitones : { 12.0f, -12.0f, 24.0f, -24.0f })
                    {
                        PitchShifter shifter;
                        juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, 1 };
                        shifter.prepare (spec);
                        shifter.reset();
                        shifter.setPitchShiftSemitones (semitones);

                        // Warm up on the file's own leading ~0.5s (or half the
                        // file for a very short recording) so the delay line
                        // holds real signal history before trusting alignment
                        // offsets -- same rationale as the sine-tone version of
                        // this test above.
                        const int warmupSamples = juce::jmin ((int) (sampleRate * 0.5), numSourceSamples / 2);

                        // Render the FULL shifted output (warmup included) so
                        // the user can actually listen to it and report which
                        // timestamp(s) sound like the reported oscillation --
                        // the measurement below only tracks offsets from
                        // warmupSamples onward, but the audio itself should
                        // start from the top so timestamps line up with the
                        // rendered file.
                        std::vector<float> renderedOutput;
                        renderedOutput.reserve ((size_t) numSourceSamples);

                        for (int i = 0; i < warmupSamples; ++i)
                            renderedOutput.push_back (shifter.processSample (mono[(size_t) i]));

                        std::vector<float> launchOffsets;
                        std::vector<std::array<float, 2>> launchAnchorScores;
                        std::vector<int> launchSampleIndices;
                        // Independent trace of ONLY the previousOffset
                        // anchor's own local search result at each launch
                        // (debugFindPreviousOffsetAnchorLocalBest(), called
                        // with the SAME previousOffset value that hop's real
                        // findAlignmentOffset() call used) -- decoupled from
                        // whichever anchor the gated fix actually chose, to
                        // test whether this one anchor is stable on its own.
                        std::vector<float> anchorZeroOnlyOffsets;
                        float lastSeen = shifter.getPrimaryLastOffset();
                        launchOffsets.push_back (lastSeen);
                        launchAnchorScores.push_back (shifter.getPrimaryAnchorScores());
                        launchSampleIndices.push_back (warmupSamples);
                        anchorZeroOnlyOffsets.push_back (shifter.debugFindPreviousOffsetAnchorLocalBest (lastSeen));

                        // Dense (every-launch, not sparse-sampled) flatness/
                        // argmax trace of the correlation surface INSIDE the
                        // previousOffset anchor's own +-alignmentSearchRadiusSamples
                        // window (2026-08-29). A sparse (every-40th-launch)
                        // version of this found the argmax consistently
                        // pinned at the EDGE of the window (not a flat
                        // plateau -- there's a real gradient, just clipped)
                        // at +-12st on drone1.wav, but not at +-24st -- too
                        // sparse a sample to tell whether that's a
                        // persistent effect or coincidence, or whether it
                        // correlates with the previously-measured ~50%
                        // sign-reversal rate. Records argmaxLag/flatness at
                        // EVERY launch instead, then cross-tabulates
                        // edge-pinning against the SAME sign-reversal
                        // definition anchorZeroOnlyOffsets already uses.
                        const int searchRadius = shifter.getAlignmentSearchRadiusSamples();
                        std::vector<float> argmaxLags;
                        std::vector<float> flatnessValues;

                        // Whenever the narrow (+-alignmentSearchRadiusSamples)
                        // sweep is edge-pinned, also sweep a MUCH wider range
                        // (2026-08-29, sizing a possible search-radius widen
                        // fix from measurement instead of a guess) to find
                        // where the TRUE, unclipped optimum actually sits.
                        // Coarser step (2 samples) to keep this affordable
                        // given how often drone1.wav's +-12st case is
                        // edge-pinned (measured 99.6% of launches).
                        constexpr int wideSweepRadius = 300;
                        constexpr int wideSweepStep = 2;
                        std::vector<float> wideArgmaxLags;
                        std::vector<bool> wideStillClipped;

                        auto sweepFlatness = [&] (float centerOffset)
                        {
                            float minScore = std::numeric_limits<float>::max();
                            float maxScore = -std::numeric_limits<float>::max();
                            float argmaxLag = 0.0f;

                            for (int lag = -searchRadius; lag <= searchRadius; ++lag)
                            {
                                const float score = shifter.debugScorePrimaryCandidateOffset (centerOffset + (float) lag);
                                if (score < minScore) minScore = score;
                                if (score > maxScore) { maxScore = score; argmaxLag = (float) lag; }
                            }

                            argmaxLags.push_back (argmaxLag);
                            flatnessValues.push_back (maxScore - minScore);

                            if (std::abs (std::abs (argmaxLag) - (float) searchRadius) < 0.5f)
                            {
                                float wideBestScore = -std::numeric_limits<float>::max();
                                float wideArgmaxLag = 0.0f;

                                for (int lag = -wideSweepRadius; lag <= wideSweepRadius; lag += wideSweepStep)
                                {
                                    const float score = shifter.debugScorePrimaryCandidateOffset (centerOffset + (float) lag);
                                    if (score > wideBestScore) { wideBestScore = score; wideArgmaxLag = (float) lag; }
                                }

                                wideArgmaxLags.push_back (wideArgmaxLag);
                                wideStillClipped.push_back (std::abs (std::abs (wideArgmaxLag) - (float) wideSweepRadius) < (float) wideSweepStep);
                            }
                        };

                        sweepFlatness (lastSeen);

                        for (int i = warmupSamples; i < numSourceSamples; ++i)
                        {
                            renderedOutput.push_back (shifter.processSample (mono[(size_t) i]));

                            const float current = shifter.getPrimaryLastOffset();
                            if (current != lastSeen)
                            {
                                launchOffsets.push_back (current);
                                launchAnchorScores.push_back (shifter.getPrimaryAnchorScores());
                                launchSampleIndices.push_back (i);
                                anchorZeroOnlyOffsets.push_back (shifter.debugFindPreviousOffsetAnchorLocalBest (lastSeen));
                                sweepFlatness (lastSeen);
                                lastSeen = current;
                            }
                        }

                        int edgePinnedCount = 0;
                        double flatnessSum = 0.0;
                        constexpr float edgeEpsilon = 0.5f;
                        for (float lag : argmaxLags)
                        {
                            if (std::abs (std::abs (lag) - (float) searchRadius) < edgeEpsilon)
                                ++edgePinnedCount;
                        }
                        for (float f : flatnessValues)
                            flatnessSum += f;
                        const double edgePinnedFraction = ! argmaxLags.empty() ? (double) edgePinnedCount / (double) argmaxLags.size() : 0.0;
                        const double avgFlatness = ! flatnessValues.empty() ? flatnessSum / (double) flatnessValues.size() : 0.0;

                        int reversalsWhenEdgePinned = 0, totalWhenEdgePinned = 0;
                        int reversalsWhenNotEdgePinned = 0, totalWhenNotEdgePinned = 0;
                        for (size_t i = 2; i < anchorZeroOnlyOffsets.size(); ++i)
                        {
                            const float deltaPrev = anchorZeroOnlyOffsets[i - 1] - anchorZeroOnlyOffsets[i - 2];
                            const float deltaCurr = anchorZeroOnlyOffsets[i] - anchorZeroOnlyOffsets[i - 1];

                            if (std::abs (deltaPrev) > 1.0e-3f && std::abs (deltaCurr) > 1.0e-3f)
                            {
                                const bool isReversal = (deltaPrev > 0.0f) != (deltaCurr > 0.0f);
                                const bool edgePinned = std::abs (std::abs (argmaxLags[i]) - (float) searchRadius) < edgeEpsilon;

                                if (edgePinned) { ++totalWhenEdgePinned; if (isReversal) ++reversalsWhenEdgePinned; }
                                else { ++totalWhenNotEdgePinned; if (isReversal) ++reversalsWhenNotEdgePinned; }
                            }
                        }

                        logMessage ("Dense flatness trace at " + juce::String (semitones) + "st (real material): "
                                    + juce::String ((int) argmaxLags.size()) + " launches, edge-pinned " + juce::String (edgePinnedCount)
                                    + "/" + juce::String ((int) argmaxLags.size()) + " (" + juce::String (edgePinnedFraction * 100.0, 1)
                                    + "%), avg flatness=" + juce::String (avgFlatness, 4) + "; reversal rate WHEN edge-pinned: "
                                    + juce::String (reversalsWhenEdgePinned) + "/" + juce::String (totalWhenEdgePinned) + " ("
                                    + juce::String (totalWhenEdgePinned > 0 ? 100.0 * reversalsWhenEdgePinned / totalWhenEdgePinned : 0.0, 1)
                                    + "%), reversal rate when NOT edge-pinned: " + juce::String (reversalsWhenNotEdgePinned) + "/"
                                    + juce::String (totalWhenNotEdgePinned) + " ("
                                    + juce::String (totalWhenNotEdgePinned > 0 ? 100.0 * reversalsWhenNotEdgePinned / totalWhenNotEdgePinned : 0.0, 1)
                                    + "%)");

                        if (! wideArgmaxLags.empty())
                        {
                            float minAbsWideLag = std::numeric_limits<float>::max();
                            float maxAbsWideLag = 0.0f;
                            double sumAbsWideLag = 0.0;
                            int stillClippedCount = 0;

                            for (size_t w = 0; w < wideArgmaxLags.size(); ++w)
                            {
                                const float absLag = std::abs (wideArgmaxLags[w]);
                                minAbsWideLag = juce::jmin (minAbsWideLag, absLag);
                                maxAbsWideLag = juce::jmax (maxAbsWideLag, absLag);
                                sumAbsWideLag += absLag;
                                if (wideStillClipped[w])
                                    ++stillClippedCount;
                            }

                            logMessage ("  Wide sweep (+-" + juce::String (wideSweepRadius) + " samples) at "
                                        + juce::String (semitones) + "st, over " + juce::String ((int) wideArgmaxLags.size())
                                        + " edge-pinned launches: true optimum's |offset| from center ranges "
                                        + juce::String (minAbsWideLag, 1) + ".." + juce::String (maxAbsWideLag, 1) + " samples (avg "
                                        + juce::String (sumAbsWideLag / (double) wideArgmaxLags.size(), 1) + "), vs the narrow "
                                        + juce::String (searchRadius) + "-sample radius -- still clipped even at the wide radius: "
                                        + juce::String (stillClippedCount) + "/" + juce::String ((int) wideArgmaxLags.size()));
                        }

                        // Write the rendered output next to the source file so
                        // the user can listen and report exact timestamps of
                        // audible artifacts, cross-referenced against the jump
                        // log below.
                        {
                            const juce::String suffix = (semitones >= 0.0f ? "+" : "") + juce::String ((int) semitones) + "st";
                            const juce::File outFile = wavFile.getSiblingFile (
                                wavFile.getFileNameWithoutExtension() + "_shifted_" + suffix + ".wav");
                            std::unique_ptr<juce::FileOutputStream> outStream (outFile.createOutputStream());

                            if (outStream != nullptr)
                            {
                                outStream->setPosition (0);
                                outStream->truncate();

                                juce::WavAudioFormat wavFormat;
                                std::unique_ptr<juce::AudioFormatWriter> writer (
                                    wavFormat.createWriterFor (outStream.get(), sampleRate, 1, 32, {}, 0));

                                if (writer != nullptr)
                                {
                                    outStream.release(); // writer now owns the stream

                                    juce::AudioBuffer<float> outBuffer (1, (int) renderedOutput.size());
                                    outBuffer.copyFrom (0, 0, renderedOutput.data(), (int) renderedOutput.size());
                                    writer->writeFromAudioSampleBuffer (outBuffer, 0, outBuffer.getNumSamples());

                                    logMessage ("Rendered shifted output to " + outFile.getFullPathName());
                                }
                            }
                        }

                        int signReversals = 0;
                        int deltaPairs = 0;
                        float maxAbsDelta = 0.0f;

                        for (size_t i = 2; i < launchOffsets.size(); ++i)
                        {
                            const float deltaPrev = launchOffsets[i - 1] - launchOffsets[i - 2];
                            const float deltaCurr = launchOffsets[i] - launchOffsets[i - 1];
                            maxAbsDelta = juce::jmax (maxAbsDelta, std::abs (deltaCurr));

                            if (std::abs (deltaPrev) > 1.0e-3f && std::abs (deltaCurr) > 1.0e-3f)
                            {
                                ++deltaPairs;
                                if ((deltaPrev > 0.0f) != (deltaCurr > 0.0f))
                                    ++signReversals;
                            }
                        }

                        const double reversalFraction = deltaPairs > 0 ? (double) signReversals / (double) deltaPairs : 0.0;

                        // Tracing which mechanism drives the small-scale
                        // reversal: findAlignmentOffset() picks anchor 1
                        // (fixed zero) whenever its score beats anchor 0
                        // (previousOffset) -- see PitchShifter.cpp line ~274.
                        // getPrimaryAnchorScores() already gives both anchors'
                        // scores at EVERY launch (not just around big jumps),
                        // so the winning anchor per launch, and how often it
                        // FLIPS from one launch to the next, can be reconstructed
                        // here with no new production-code instrumentation. A
                        // low anchor-flip fraction alongside a HIGH offset
                        // sign-reversal fraction would mean the same anchor
                        // keeps winning every hop, but that anchor's own
                        // +-alignmentSearchRadiusSamples local search itself
                        // returns a different best lag almost every time --
                        // i.e. the instability lives INSIDE one anchor's own
                        // window, not in anchor-vs-anchor switching.
                        int anchorFlips = 0;
                        for (size_t i = 1; i < launchAnchorScores.size(); ++i)
                        {
                            const int prevWinner = launchAnchorScores[i - 1][1] > launchAnchorScores[i - 1][0] ? 1 : 0;
                            const int currWinner = launchAnchorScores[i][1] > launchAnchorScores[i][0] ? 1 : 0;
                            if (prevWinner != currWinner)
                                ++anchorFlips;
                        }
                        const double anchorFlipFraction = launchAnchorScores.size() > 1
                                                               ? (double) anchorFlips / (double) (launchAnchorScores.size() - 1)
                                                               : 0.0;

                        // Direct test of the "instability lives INSIDE one
                        // anchor's own window" theory above: sign-reversal
                        // rate of anchorZeroOnlyOffsets, an INDEPENDENT trace
                        // of the previousOffset anchor's own local search
                        // result at each launch, decoupled from whichever
                        // anchor the applied fix actually chose. If this
                        // reversal rate is comparably high on its own, that
                        // anchor is unstable by itself -- no anchor-switching
                        // gate (score-margin or dwell) could ever help.
                        int anchorZeroSignReversals = 0;
                        int anchorZeroDeltaPairs = 0;
                        for (size_t i = 2; i < anchorZeroOnlyOffsets.size(); ++i)
                        {
                            const float deltaPrev = anchorZeroOnlyOffsets[i - 1] - anchorZeroOnlyOffsets[i - 2];
                            const float deltaCurr = anchorZeroOnlyOffsets[i] - anchorZeroOnlyOffsets[i - 1];

                            if (std::abs (deltaPrev) > 1.0e-3f && std::abs (deltaCurr) > 1.0e-3f)
                            {
                                ++anchorZeroDeltaPairs;
                                if ((deltaPrev > 0.0f) != (deltaCurr > 0.0f))
                                    ++anchorZeroSignReversals;
                            }
                        }
                        const double anchorZeroReversalFraction = anchorZeroDeltaPairs > 0
                                                                       ? (double) anchorZeroSignReversals / (double) anchorZeroDeltaPairs
                                                                       : 0.0;

                        std::vector<int> bigJumpLaunchIndices;
                        for (size_t i = 1; i < launchOffsets.size(); ++i)
                            if (std::abs (launchOffsets[i] - launchOffsets[i - 1]) > 100.0f)
                                bigJumpLaunchIndices.push_back ((int) i);

                        juce::String bigJumpList;
                        for (int idx : bigJumpLaunchIndices)
                            bigJumpList += juce::String (idx) + "@" + juce::String (launchSampleIndices[(size_t) idx] / sampleRate, 2) + "s ";

                        logMessage ("Offset stability at " + juce::String (semitones) + "st (real material): "
                                    + juce::String ((int) launchOffsets.size()) + " launches, " + juce::String (signReversals)
                                    + "/" + juce::String (deltaPairs) + " sign reversals (" + juce::String (reversalFraction * 100.0, 1)
                                    + "%), anchor flips " + juce::String (anchorFlips) + "/" + juce::String ((int) launchAnchorScores.size() - 1)
                                    + " (" + juce::String (anchorFlipFraction * 100.0, 1) + "%), previousOffset-anchor-ALONE reversals "
                                    + juce::String (anchorZeroSignReversals) + "/" + juce::String (anchorZeroDeltaPairs) + " ("
                                    + juce::String (anchorZeroReversalFraction * 100.0, 1) + "%), max per-launch delta=" + juce::String (maxAbsDelta, 2)
                                    + " samples, big jumps (>100 samples) at launch index@time: [" + bigJumpList.trim() + "]");

                        if (! bigJumpLaunchIndices.empty())
                        {
                            const int jumpIdx = bigJumpLaunchIndices.front();
                            const int windowStart = juce::jmax (0, jumpIdx - 3);
                            const int windowEnd = juce::jmin ((int) launchOffsets.size() - 1, jumpIdx + 2);

                            logMessage ("  Anchor scores around first jump (launch " + juce::String (jumpIdx)
                                        + " @ " + juce::String (launchSampleIndices[(size_t) jumpIdx] / sampleRate, 2)
                                        + "s) at " + juce::String (semitones) + "st (real material):");
                            for (int i = windowStart; i <= windowEnd; ++i)
                            {
                                const auto& scores = launchAnchorScores[(size_t) i];
                                logMessage ("    launch " + juce::String (i) + " @ " + juce::String (launchSampleIndices[(size_t) i] / sampleRate, 2)
                                            + "s: offset=" + juce::String (launchOffsets[(size_t) i], 2)
                                            + ", previousOffset-anchor score=" + juce::String (scores[0], 5)
                                            + ", zero-anchor score=" + juce::String (scores[1], 5)
                                            + (i == jumpIdx ? "  <-- jump lands here" : ""));
                            }
                        }
                    }
                }
            }
        };

        beginTest ("DIAGNOSTIC: primary alignment-offset stability on REAL recorded material (whisper8.wav) at "
                   "+-12st vs +-24st");
        runRealMaterialAlignmentDiagnostic (juce::File ("C:\\Etheral\\whisper8.wav"));

        beginTest ("DIAGNOSTIC: primary alignment-offset stability on a REAL held tonal drone (Lo Drone 6.wav) at "
                   "+-12st vs +-24st");
        {
            // The user confirmed by ear that whisper8.wav's aperiodic content
            // does NOT reproduce the originally-reported "oscillation" percept
            // (no fundamental pitch to perceive as wobbling), though it did
            // surface a real, distinct glitch at +24st. This drone is a
            // sustained, tonal recording -- the kind of material the original
            // complaint was actually reported against -- to test whether the
            // anchor-flip-flop mechanism looks different here, closer to (or
            // still unlike) the pure-sine tests' own octave-tie pattern.
            runRealMaterialAlignmentDiagnostic (juce::File ("C:\\Etheral\\Lo Drone 6.wav"));
        }

        beginTest ("DIAGNOSTIC: primary alignment-offset stability on a second REAL held tonal drone (drone1.wav) "
                   "at +-12st vs +-24st");
        {
            // Lo Drone 6.wav's +24st "ring bell / duplicated" report turned
            // out to be confounded -- the user confirmed the unshifted source
            // itself may already carry a bell-like/inharmonic quality, so that
            // specific test couldn't tell PitchShifter-introduced doubling
            // apart from source coloration. This is a second recording, made
            // deliberately simple and harmonically clean by the user
            // specifically to remove that confound.
            runRealMaterialAlignmentDiagnostic (juce::File ("C:\\Etheral\\drone1.wav"));
        }
    }
};

static PitchShifterTests pitchShifterTests;
