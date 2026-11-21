#include <JuceHeader.h>
#include "../../Source/DSP/LoopCapture.h"
#include <limits>

// Stage 1 test-runner target -- unit tests for the new, standalone, unwired
// LoopCapture class (captures a window of recent stereo audio and repeats it
// as a static, crossfaded loop, gated by a continuous 0..1 "loop freeze
// amount"). This class is NOT wired into ShimmerReverbEngine/PluginProcessor/
// PluginEditor/Parameters yet -- that is explicitly a later stage -- so every
// test here drives LoopCapture directly and in isolation. See LoopCapture.h
// for the full seam-splice/rising-edge design rationale.
class LoopCaptureTests : public juce::UnitTest
{
public:
    LoopCaptureTests() : juce::UnitTest ("LoopCaptureTests", "DSP") {}

    // Independent re-derivation of LoopCapture's own read-position sequence
    // and blend-zone test, used by several tests below to know, from the
    // OUTSIDE, which output sample corresponds to which position inside the
    // captured loop -- without reaching into the class's private state. Must
    // stay in lockstep with LoopCapture::process()'s own wrap rule ("after
    // advancing past p == N-1, wrap the read position to X, not 0") for the
    // periodicity/click-free tests below to mean anything.
    static int positionAtStep (int step, int loopLengthSamples, int crossfadeSamples) noexcept
    {
        if (step < loopLengthSamples)
            return step;

        const int period = loopLengthSamples - crossfadeSamples;
        const int stepsPastFirstPass = step - loopLengthSamples;
        return crossfadeSamples + (period > 0 ? stepsPastFirstPass % period : 0);
    }

    static bool isInBlendZone (int position, int loopLengthSamples, int crossfadeSamples) noexcept
    {
        return crossfadeSamples > 0 && position >= loopLengthSamples - crossfadeSamples;
    }

    // Steady sine source for the 2026-10-10 re-arm/deferred-capture tests
    // below -- same phase-accumulator shape as the nextSineSample lambdas
    // in the older tests, just reusable across several tests.
    struct SineSource
    {
        SineSource (double sampleRateIn, double frequencyHz, float amplitudeIn)
            : increment (juce::MathConstants<double>::twoPi * frequencyHz / sampleRateIn), amplitude (amplitudeIn) {}

        float next() noexcept
        {
            const float sample = amplitude * (float) std::sin (phase);
            phase += increment;
            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;
            return sample;
        }

        double phase = 0.0;
        double increment;
        float amplitude;
    };

    // Hann-windowed Goertzel power at `hz` over x[start, start + n) -- same
    // estimator as ShimmerReverbEngineTests.cpp's goertzelPower(), returned
    // in dB (arbitrary but fixed reference) so tone-A vs tone-B content of a
    // captured loop can be compared and logged directly.
    static double goertzelDb (const std::vector<float>& x, int start, int n, double hz, double sampleRate)
    {
        const double coeff = 2.0 * std::cos (juce::MathConstants<double>::twoPi * hz / sampleRate);
        double s1 = 0.0, s2 = 0.0;

        for (int i = 0; i < n; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / (n - 1));
            const double s0 = w * (double) x[(size_t) (start + i)] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }

        return 10.0 * std::log10 (s1 * s1 + s2 * s2 - coeff * s1 * s2 + 1.0e-20);
    }

    static float rms (const std::vector<float>& x, int start, int n)
    {
        double sum = 0.0;
        for (int i = start; i < start + n; ++i)
            sum += (double) x[(size_t) i] * (double) x[(size_t) i];
        return (float) std::sqrt (sum / (double) juce::jmax (1, n));
    }

    void runTest() override
    {
        beginTest ("Bounded/finite sanity: fixed-seed noise, toggled freeze, stays finite and bounded");
        {
            constexpr double sampleRate = 44100.0;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, (juce::uint32) 2 };

            LoopCapture loopCapture;
            loopCapture.prepare (spec);
            loopCapture.reset();
            loopCapture.setLoopLengthMs (250.0f);

            juce::Random random (112358);

            // Several seconds, toggling loopFreezeAmount between 0 and 1 a
            // few times across the run (each toggle to 1.0 is a fresh
            // rising-edge capture event; each toggle back to 0.0 arms the
            // next one).
            constexpr int totalSamples = (int) (4.0 * sampleRate); // ~4s
            constexpr int toggleEverySamples = (int) (0.7 * sampleRate);

            for (int i = 0; i < totalSamples; ++i)
            {
                const float freeze = ((i / toggleEverySamples) % 2 == 0) ? 0.0f : 1.0f;
                const float left  = random.nextFloat() * 2.0f - 1.0f;  // uniform in [-1, 1]
                const float right = random.nextFloat() * 2.0f - 1.0f;

                loopCapture.setLoopFreezeAmount (freeze);
                auto out = loopCapture.process (left, right);

                expect (std::isfinite (out.first), "Left output not finite at sample " + juce::String (i));
                expect (std::isfinite (out.second), "Right output not finite at sample " + juce::String (i));
                expect (std::abs (out.first) <= 2.0f, "Left output exceeded safety bound at sample " + juce::String (i)
                                                           + " (value: " + juce::String (out.first) + ")");
                expect (std::abs (out.second) <= 2.0f, "Right output exceeded safety bound at sample " + juce::String (i)
                                                            + " (value: " + juce::String (out.second) + ")");
            }
        }

        beginTest ("Exact periodicity after capture (outside the crossfade blend zone)");
        {
            constexpr double sampleRate = 44100.0;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, (juce::uint32) 2 };

            constexpr float loopLengthMs = 200.0f;
            constexpr float crossfadeMsConstant = 25.0f; // must match LoopCapture::crossfadeMs
            const int loopLengthSamples = juce::roundToInt (loopLengthMs * 0.001 * sampleRate);
            const int crossfadeSamples = juce::jmin (juce::roundToInt (crossfadeMsConstant * 0.001 * sampleRate),
                                                       loopLengthSamples / 2);
            const int period = loopLengthSamples - crossfadeSamples;

            LoopCapture loopCapture;
            loopCapture.prepare (spec);
            loopCapture.reset();
            loopCapture.setLoopLengthMs (loopLengthMs);

            juce::Random random (2718281);

            // Feed a noise burst comfortably longer than the loop length so
            // the rolling buffer has real history before capture engages.
            const int historySamples = loopLengthSamples + 4000;
            loopCapture.setLoopFreezeAmount (0.0f);
            for (int i = 0; i < historySamples; ++i)
                loopCapture.process (random.nextFloat() * 2.0f - 1.0f, random.nextFloat() * 2.0f - 1.0f);

            // Play for several full loop periods after the rising-edge
            // capture, recording every output sample from the capture call
            // onward. Comparisons below only start at global index
            // loopLengthSamples, well past LoopCapture's own ~2205-sample
            // (50ms) engage ramp -- see LoopCapture::setLoopFreezeAmount()'s
            // comment -- so the ramp itself never falls inside the compared
            // region.
            loopCapture.setLoopFreezeAmount (1.0f); // rising edge on the very first process() call below (previous was 0.0f)

            const int numStepsToRecord = loopLengthSamples + 4 * period;
            std::vector<float> outputs;
            outputs.reserve ((size_t) numStepsToRecord);

            for (int step = 0; step < numStepsToRecord; ++step)
            {
                auto out = loopCapture.process (0.0f, 0.0f); // input irrelevant once loopFreezeAmount reaches 1.0
                outputs.push_back (out.first);
            }

            expectEquals (loopCapture.getCurrentLoopLengthSamples(), loopLengthSamples);

            int numCompared = 0;
            for (int step = loopLengthSamples; step + period < numStepsToRecord; ++step)
            {
                const int position = positionAtStep (step, loopLengthSamples, crossfadeSamples);
                if (isInBlendZone (position, loopLengthSamples, crossfadeSamples))
                    continue; // deliberately-blended region, skip per test design

                expectWithinAbsoluteError (outputs[(size_t) (step + period)], outputs[(size_t) step], 1.0e-6f,
                                            "Output at step " + juce::String (step) + " and step "
                                                + juce::String (step + period) + " (one period later) differ "
                                                  "outside the crossfade blend zone");
                ++numCompared;
            }

            expect (numCompared > 100, "Too few positions compared for a meaningful periodicity check ("
                                            + juce::String (numCompared) + ")");
        }

        beginTest ("Click-free seam: crossfade suppresses the wrap discontinuity on a steady sine tone");
        {
            constexpr double sampleRate = 44100.0;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, (juce::uint32) 2 };

            // Loop length in samples is deliberately NOT an integer multiple
            // of the sine's own period: at 300Hz the period is exactly 147.0
            // samples at 44100Hz; 333ms -> 14685 samples, and
            // 14685 / 147 = 99.898..., not an integer -- so a naive
            // (unfaded) loop would produce a large phase-discontinuity jump
            // at every wrap.
            constexpr float sineFreqHz = 300.0f;
            constexpr float loopLengthMs = 333.0f;
            constexpr float crossfadeMsConstant = 25.0f;
            const int loopLengthSamples = juce::roundToInt (loopLengthMs * 0.001 * sampleRate);
            const int crossfadeSamples = juce::jmin (juce::roundToInt (crossfadeMsConstant * 0.001 * sampleRate),
                                                       loopLengthSamples / 2);
            const int period = loopLengthSamples - crossfadeSamples;

            LoopCapture loopCapture;
            loopCapture.prepare (spec);
            loopCapture.reset();
            loopCapture.setLoopLengthMs (loopLengthMs);

            double phase = 0.0;
            const double phaseIncrement = juce::MathConstants<double>::twoPi * sineFreqHz / sampleRate;
            constexpr float amplitude = 0.8f;

            auto nextSineSample = [&]() -> float
            {
                const float sample = amplitude * (float) std::sin (phase);
                phase += phaseIncrement;
                if (phase >= juce::MathConstants<double>::twoPi)
                    phase -= juce::MathConstants<double>::twoPi;
                return sample;
            };

            // History before capture.
            const int historySamples = loopLengthSamples + 4000;
            loopCapture.setLoopFreezeAmount (0.0f);
            for (int i = 0; i < historySamples; ++i)
            {
                const float s = nextSineSample();
                loopCapture.process (s, s);
            }

            // Capture and play for several periods. Analysis below only
            // examines step >= loopLengthSamples + 1, well past LoopCapture's
            // own ~2205-sample engage ramp, so the ramp itself is never
            // mistaken for a wrap-seam discontinuity.
            loopCapture.setLoopFreezeAmount (1.0f); // rising edge on the very first process() call below

            const int numStepsToRecord = loopLengthSamples + 10 * period;
            std::vector<float> outputs;
            outputs.reserve ((size_t) numStepsToRecord);

            for (int step = 0; step < numStepsToRecord; ++step)
            {
                const float s = nextSineSample(); // input irrelevant once loopFreezeAmount reaches 1.0, kept flowing for realism
                auto out = loopCapture.process (s, s);
                outputs.push_back (out.first);
            }

            // Margin (samples) kept clear of both edges of the "interior"
            // window, so the interior measurement can't accidentally catch
            // the tail end of the blend zone or the sample right after a wrap.
            constexpr int marginSamples = 8;

            float maxInteriorDelta = 0.0f;
            float maxWrapDelta = 0.0f;

            for (int step = loopLengthSamples + 1; step < numStepsToRecord; ++step)
            {
                const int position = positionAtStep (step, loopLengthSamples, crossfadeSamples);
                const int previousPosition = positionAtStep (step - 1, loopLengthSamples, crossfadeSamples);
                const float delta = std::abs (outputs[(size_t) step] - outputs[(size_t) (step - 1)]);

                // "Near the wrap": inside the blend zone, or the sample
                // immediately following a wrap (previousPosition > position
                // means the read position just wrapped from N-1 back to X).
                const bool nearWrap = isInBlendZone (position, loopLengthSamples, crossfadeSamples)
                                      || previousPosition > position;

                if (nearWrap)
                {
                    maxWrapDelta = juce::jmax (maxWrapDelta, delta);
                }
                else if (position >= crossfadeSamples + marginSamples
                         && position < loopLengthSamples - crossfadeSamples - marginSamples)
                {
                    maxInteriorDelta = juce::jmax (maxInteriorDelta, delta);
                }
            }

            expect (maxInteriorDelta > 0.0f, "Interior delta measurement was zero -- test setup is broken");

            logMessage ("Click-free seam: maxInteriorDelta=" + juce::String (maxInteriorDelta, 6)
                            + ", maxWrapDelta=" + juce::String (maxWrapDelta, 6));

            expect (maxWrapDelta <= 2.0f * maxInteriorDelta,
                    "Wrap-point delta (" + juce::String (maxWrapDelta, 6) + ") was more than 2x the steady "
                        "interior delta (" + juce::String (maxInteriorDelta, 6) + ") -- crossfade did not "
                          "suppress the seam discontinuity");
        }

        beginTest ("Engage/disengage transition is click-free even given an instantaneous full-range target jump");
        {
            // Regression test for the 2026-09-06 by-ear report: "small
            // glitch noise upon toggling" Loop Freeze. Root cause: this
            // class used to trust the caller to arrive pre-smoothed, but
            // PluginProcessor's caller-side smoothing applied only ONE
            // value per host audio BLOCK (juce::SmoothedValue::skip), so a
            // host buffer comparable to or larger than the 50ms ramp could
            // fast-forward the whole ramp within a single block -- a
            // near-instant switch between the live wet signal and the
            // captured loop. Fix: LoopCapture now owns its own per-sample
            // ramp (see setLoopFreezeAmount()'s header comment). This test
            // drives setLoopFreezeAmount() with the worst-case
            // instantaneous jump -- exactly what a large host buffer would
            // have produced under the old design -- and checks the actual
            // per-sample output has no outsized delta at the transition,
            // same "interior vs. transition delta" bounded-multiple
            // methodology as the "Click-free seam" test above.
            constexpr double sampleRate = 44100.0;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, (juce::uint32) 2 };

            constexpr float loopLengthMs = 300.0f;
            const int loopLengthSamples = juce::roundToInt (loopLengthMs * 0.001 * sampleRate);

            LoopCapture loopCapture;
            loopCapture.prepare (spec);
            loopCapture.reset();
            loopCapture.setLoopLengthMs (loopLengthMs);
            loopCapture.setLoopFreezeAmount (0.0f);

            constexpr float sineFreqHz = 220.0f;
            double phase = 0.0;
            const double phaseIncrement = juce::MathConstants<double>::twoPi * sineFreqHz / sampleRate;
            constexpr float amplitude = 0.6f;

            auto nextSineSample = [&]() -> float
            {
                const float sample = amplitude * (float) std::sin (phase);
                phase += phaseIncrement;
                if (phase >= juce::MathConstants<double>::twoPi)
                    phase -= juce::MathConstants<double>::twoPi;
                return sample;
            };

            // History, disengaged, long enough for a real capture window.
            const int historySamples = loopLengthSamples + 4000;
            for (int i = 0; i < historySamples; ++i)
                loopCapture.process (nextSineSample(), 0.0f);

            // Steady-state interior delta baseline: live sine only, no
            // capture/blend happening yet.
            float prevSample = 0.0f;
            float maxSteadyDelta = 0.0f;
            constexpr int steadySamples = 2000;
            for (int i = 0; i < steadySamples; ++i)
            {
                auto out = loopCapture.process (nextSineSample(), 0.0f);
                if (i > 0)
                    maxSteadyDelta = juce::jmax (maxSteadyDelta, std::abs (out.first - prevSample));
                prevSample = out.first;
            }

            // Worst-case instantaneous engage: a single hard 0 -> 1 target
            // jump, exactly matching what PluginProcessor's old block-
            // granularity smoothing could produce in one call if the host
            // buffer were as large as (or larger than) the ramp itself.
            loopCapture.setLoopFreezeAmount (1.0f);

            float maxEngageDelta = 0.0f;
            constexpr int engageSamplesToWatch = 4000; // comfortably covers the ~2205-sample (50ms) ramp
            for (int i = 0; i < engageSamplesToWatch; ++i)
            {
                auto out = loopCapture.process (nextSineSample(), 0.0f);
                maxEngageDelta = juce::jmax (maxEngageDelta, std::abs (out.first - prevSample));
                prevSample = out.first;
            }

            logMessage ("Engage click test: maxSteadyDelta=" + juce::String (maxSteadyDelta, 6)
                            + ", maxEngageDelta=" + juce::String (maxEngageDelta, 6));

            expect (maxEngageDelta <= 3.0f * maxSteadyDelta,
                    "Engage transition delta (" + juce::String (maxEngageDelta, 6) + ") was more than 3x the "
                        "steady-state interior delta (" + juce::String (maxSteadyDelta, 6) + ") -- toggling Loop "
                          "Freeze ON is not click-free");

            // Same check for disengage (a hard 1 -> 0 target jump).
            loopCapture.setLoopFreezeAmount (0.0f);

            float maxDisengageDelta = 0.0f;
            constexpr int disengageSamplesToWatch = 4000;
            for (int i = 0; i < disengageSamplesToWatch; ++i)
            {
                auto out = loopCapture.process (nextSineSample(), 0.0f);
                maxDisengageDelta = juce::jmax (maxDisengageDelta, std::abs (out.first - prevSample));
                prevSample = out.first;
            }

            logMessage ("Disengage click test: maxDisengageDelta=" + juce::String (maxDisengageDelta, 6));

            expect (maxDisengageDelta <= 3.0f * maxSteadyDelta,
                    "Disengage transition delta (" + juce::String (maxDisengageDelta, 6) + ") was more than 3x the "
                        "steady-state interior delta (" + juce::String (maxSteadyDelta, 6) + ") -- toggling Loop "
                          "Freeze OFF is not click-free");
        }

        beginTest ("Loop length change mid-loop is deferred until the next rising edge");
        {
            constexpr double sampleRate = 44100.0;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, (juce::uint32) 2 };

            constexpr float lengthA = 150.0f;
            constexpr float lengthB = 400.0f;
            const int samplesA = juce::roundToInt (lengthA * 0.001 * sampleRate);
            const int samplesB = juce::roundToInt (lengthB * 0.001 * sampleRate);

            LoopCapture loopCapture;
            loopCapture.prepare (spec);
            loopCapture.reset();
            loopCapture.setLoopLengthMs (lengthA);

            juce::Random random (99887766);

            // History, then engage (capture at length A).
            loopCapture.setLoopFreezeAmount (0.0f);
            for (int i = 0; i < samplesA + 4000; ++i)
                loopCapture.process (random.nextFloat() * 2.0f - 1.0f, random.nextFloat() * 2.0f - 1.0f);
            loopCapture.setLoopFreezeAmount (1.0f);
            loopCapture.process (random.nextFloat() * 2.0f - 1.0f, random.nextFloat() * 2.0f - 1.0f);

            expectEquals (loopCapture.getCurrentLoopLengthSamples(), samplesA);

            // Change the pending length while still frozen -- must NOT take
            // effect yet.
            loopCapture.setLoopLengthMs (lengthB);

            for (int i = 0; i < 1000; ++i)
                loopCapture.process (0.0f, 0.0f);

            expectEquals (loopCapture.getCurrentLoopLengthSamples(), samplesA,
                          "Loop length changed mid-loop without a re-engagement");

            // Disengage, then feed enough history for the new length before
            // re-engaging -- ample settle time (samplesB+4000 samples) for
            // LoopCapture's own ~2205-sample disengage ramp to fully
            // complete before the next rising edge is checked.
            loopCapture.setLoopFreezeAmount (0.0f);
            loopCapture.process (0.0f, 0.0f);
            for (int i = 0; i < samplesB + 4000; ++i)
                loopCapture.process (random.nextFloat() * 2.0f - 1.0f, random.nextFloat() * 2.0f - 1.0f);

            // Re-engage: rising edge, should now capture at length B.
            loopCapture.setLoopFreezeAmount (1.0f);
            loopCapture.process (random.nextFloat() * 2.0f - 1.0f, random.nextFloat() * 2.0f - 1.0f);

            expectEquals (loopCapture.getCurrentLoopLengthSamples(), samplesB,
                          "Re-engagement did not pick up the new pending loop length");
        }

        beginTest ("Re-engagement captures fresh content, not the previous capture's stale content");
        {
            constexpr double sampleRate = 44100.0;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, (juce::uint32) 2 };

            LoopCapture loopCapture;
            loopCapture.prepare (spec);
            loopCapture.reset();
            loopCapture.setLoopLengthMs (100.0f);

            constexpr float contentA = 0.4f;
            constexpr float contentB = -0.7f;

            // Feed constant content A, long enough to fill real history,
            // then engage.
            const int loopSamples = juce::roundToInt (100.0f * 0.001 * sampleRate);
            loopCapture.setLoopFreezeAmount (0.0f);
            for (int i = 0; i < loopSamples + 4000; ++i)
                loopCapture.process (contentA, contentA);
            loopCapture.setLoopFreezeAmount (1.0f);
            loopCapture.process (contentA, contentA);

            expectEquals (loopCapture.getCurrentLoopLengthSamples(), loopSamples);
            expectWithinAbsoluteError (loopCapture.peekCapturedLoopSample (0, 0), contentA, 1.0e-6f,
                                        "Captured content A not found in captured loop buffer");
            expectWithinAbsoluteError (loopCapture.peekCapturedLoopSample (1, loopSamples - 1), contentA, 1.0e-6f,
                                        "Captured content A not found in captured loop buffer (right channel)");

            // Disengage, then flush the ENTIRE rolling buffer (longer than
            // maxLoopLengthMs) with new, distinct content B before
            // re-engaging -- comfortably longer than LoopCapture's own
            // ~2205-sample disengage ramp, so it's fully settled by the time
            // re-engagement is checked.
            loopCapture.setLoopFreezeAmount (0.0f);
            loopCapture.process (contentA, contentA);

            constexpr float maxLoopLengthMsConstant = 4000.0f; // must match LoopCapture::maxLoopLengthMs
            const int maxCapacitySamples = juce::roundToInt (maxLoopLengthMsConstant * 0.001 * sampleRate);
            for (int i = 0; i < maxCapacitySamples + 4000; ++i)
                loopCapture.process (contentB, contentB);

            loopCapture.setLoopFreezeAmount (1.0f);
            loopCapture.process (contentB, contentB);

            expectEquals (loopCapture.getCurrentLoopLengthSamples(), loopSamples);
            expectWithinAbsoluteError (loopCapture.peekCapturedLoopSample (0, 0), contentB, 1.0e-6f,
                                        "Re-engagement did not capture fresh content B (left channel, start)");
            expectWithinAbsoluteError (loopCapture.peekCapturedLoopSample (1, loopSamples - 1), contentB, 1.0e-6f,
                                        "Re-engagement did not capture fresh content B (right channel, end)");
        }

        beginTest ("Max-length capture is not silently truncated");
        {
            constexpr double sampleRate = 44100.0;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, (juce::uint32) 2 };

            constexpr float maxLoopLengthMsConstant = 4000.0f; // must match LoopCapture::maxLoopLengthMs
            const int expectedSamples = juce::roundToInt (maxLoopLengthMsConstant * 0.001 * sampleRate);

            LoopCapture loopCapture;
            loopCapture.prepare (spec);
            loopCapture.reset();
            loopCapture.setLoopLengthMs (maxLoopLengthMsConstant);

            juce::Random random (13571113);
            loopCapture.setLoopFreezeAmount (0.0f);
            for (int i = 0; i < expectedSamples + 4000; ++i)
                loopCapture.process (random.nextFloat() * 2.0f - 1.0f, random.nextFloat() * 2.0f - 1.0f);

            loopCapture.setLoopFreezeAmount (1.0f);
            loopCapture.process (random.nextFloat() * 2.0f - 1.0f, random.nextFloat() * 2.0f - 1.0f);

            expectEquals (loopCapture.getCurrentLoopLengthSamples(), expectedSamples,
                          "Max-length capture was truncated to fewer samples than requested");
        }

        // -------------------------------------------------------------------
        // 2026-10-10 by-ear report: "at Loop Mix = 1 the loop is sometimes
        // silent; turning Loop Mix to 0 and back fixes it, but not always".
        // Three root causes, one test each below (plus a jitter guard for the
        // fix itself) -- see LoopCapture.h's "Re-arm and deferred capture"
        // section for the full trail.
        // -------------------------------------------------------------------

        beginTest ("Restart with Loop Mix already up defers capture until real history exists (no silent loop)");
        {
            // Root cause 1: prepare()/reset() zero the rolling history, and
            // the smoothed/previous amounts both restart at 0.0f -- so a Loop
            // Mix restored > 0 (plugin state at Standalone launch, an audio
            // device restart, a sample-rate change) forms a "rising edge" on
            // the very first sample and used to capture an ALL-ZERO history:
            // a silent loop for as long as Loop Mix stayed up.
            constexpr double sampleRate = 44100.0;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, (juce::uint32) 2 };

            constexpr float loopLengthMs = 500.0f;
            const int loopLengthSamples = juce::roundToInt (loopLengthMs * 0.001 * sampleRate);
            const int rampSamples = juce::roundToInt (0.05 * sampleRate); // must match LoopCapture::loopFreezeRampSeconds

            LoopCapture loopCapture;
            loopCapture.prepare (spec);
            loopCapture.setLoopLengthMs (loopLengthMs);
            loopCapture.setLoopFreezeAmount (1.0f); // BEFORE any audio -- the restored-state case

            SineSource sine (sampleRate, 440.0, 0.5f);

            const int totalSamples = loopLengthSamples + rampSamples + (int) (0.5 * sampleRate);
            std::vector<float> inputs, outputs;
            inputs.reserve ((size_t) totalSamples);
            outputs.reserve ((size_t) totalSamples);

            for (int i = 0; i < totalSamples; ++i)
            {
                const float s = sine.next();
                inputs.push_back (s);
                outputs.push_back (loopCapture.process (s, s).first);

                if (i == loopLengthSamples / 2)
                    expect (loopCapture.isCapturePending(), "Capture should be PENDING while history is shorter than the loop");
            }

            // (a) Pending window: the deferred capture lands on the sample
            // whose write completes loopLengthSamples of audible history --
            // index loopLengthSamples since the "wait for sound" gate (the
            // sine's own first sample, sin(0) = 0.0f, is sub-gate and
            // precedes the onset), index loopLengthSamples - 1 before it.
            // Every sample before index loopLengthSamples - 1 must be the
            // untouched live wet signal (assertion unchanged by the gate).
            float maxPassthroughError = 0.0f;
            for (int i = 0; i < loopLengthSamples - 1; ++i)
                maxPassthroughError = juce::jmax (maxPassthroughError, std::abs (outputs[(size_t) i] - inputs[(size_t) i]));

            // (b) Deferred engage is click-free: same "transition delta vs.
            // steady-state delta" bounded-multiple metric as the engage
            // test above, with the steady baseline taken from the (live,
            // passthrough) pending window itself.
            float maxSteadyDelta = 0.0f;
            for (int i = 1; i < loopLengthSamples / 2; ++i)
                maxSteadyDelta = juce::jmax (maxSteadyDelta, std::abs (inputs[(size_t) i] - inputs[(size_t) (i - 1)]));

            float maxEngageDelta = 0.0f;
            const int engageStart = loopLengthSamples - 2;
            for (int i = engageStart; i < engageStart + 4000; ++i)
                maxEngageDelta = juce::jmax (maxEngageDelta, std::abs (outputs[(size_t) i] - outputs[(size_t) (i - 1)]));

            // (c) Fully engaged loop carries the tone at a level comparable
            // to the input (a 25ms-crossfaded loop of a steady sine).
            const int measureStart = loopLengthSamples + rampSamples + 2000;
            const int measureLength = totalSamples - measureStart;
            const float inputRms = rms (inputs, measureStart, measureLength);
            const float outputRms = rms (outputs, measureStart, measureLength);
            const float levelRatioDb = juce::Decibels::gainToDecibels (outputRms / inputRms, -200.0f);

            logMessage ("Restart path: maxPassthroughError=" + juce::String (maxPassthroughError, 6)
                            + ", maxSteadyDelta=" + juce::String (maxSteadyDelta, 6)
                            + ", maxEngageDelta=" + juce::String (maxEngageDelta, 6)
                            + ", engaged level vs input=" + juce::String (levelRatioDb, 2) + " dB"
                            + ", captureCount=" + juce::String (loopCapture.getCaptureCount()));

            expect (maxPassthroughError <= 1.0e-6f,
                    "Pending window was not the live wet signal (max error " + juce::String (maxPassthroughError, 6) + ")");
            expect (maxEngageDelta <= 3.0f * maxSteadyDelta,
                    "Deferred engage delta (" + juce::String (maxEngageDelta, 6) + ") was more than 3x the steady "
                        "delta (" + juce::String (maxSteadyDelta, 6) + ") -- deferred engage is not click-free");
            expect (levelRatioDb > -3.0f && levelRatioDb < 3.0f,
                    "Engaged loop level " + juce::String (levelRatioDb, 2) + " dB vs input -- loop is silent/wrong");
            expectEquals (loopCapture.getCaptureCount(), 1, "Expected exactly one (deferred) capture");
        }

        beginTest ("Near-zero dial re-arms capture (Loop Mix parked at 0.005 then raised captures fresh content)");
        {
            // Root cause 2: the dial doesn't land exactly on 0. The old
            // trigger (previous == 0.0f exactly) never re-armed, so the old
            // loop kept playing after a "down and back up".
            constexpr double sampleRate = 44100.0;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, (juce::uint32) 2 };

            constexpr float loopLengthMs = 300.0f;
            const int loopLengthSamples = juce::roundToInt (loopLengthMs * 0.001 * sampleRate);
            constexpr double toneAHz = 300.0;
            constexpr double toneBHz = 1200.0;

            LoopCapture loopCapture;
            loopCapture.prepare (spec);
            loopCapture.reset();
            loopCapture.setLoopLengthMs (loopLengthMs);

            SineSource toneA (sampleRate, toneAHz, 0.5f);
            SineSource toneB (sampleRate, toneBHz, 0.5f);

            // 1. Engage at 1 with tone A (real history first).
            loopCapture.setLoopFreezeAmount (0.0f);
            for (int i = 0; i < loopLengthSamples + 4000; ++i)
                { const float s = toneA.next(); loopCapture.process (s, s); }
            loopCapture.setLoopFreezeAmount (1.0f);
            for (int i = 0; i < (int) (0.5 * sampleRate); ++i)
                { const float s = toneA.next(); loopCapture.process (s, s); }

            const int capturesAfterA = loopCapture.getCaptureCount();

            // 2. Park at 0.005 (NOT exactly 0), hold 150ms, then switch the
            // input to tone B for longer than the loop length.
            loopCapture.setLoopFreezeAmount (0.005f);
            for (int i = 0; i < (int) (0.15 * sampleRate); ++i)
                { const float s = toneA.next(); loopCapture.process (s, s); }
            for (int i = 0; i < loopLengthSamples + 4000; ++i)
                { const float s = toneB.next(); loopCapture.process (s, s); }

            // 3. Raise back to 1; the engaged loop must be tone B.
            loopCapture.setLoopFreezeAmount (1.0f);
            std::vector<float> outputs;
            const int recordSamples = (int) (0.5 * sampleRate);
            outputs.reserve ((size_t) recordSamples);
            for (int i = 0; i < recordSamples; ++i)
            {
                const float s = toneB.next(); // live input irrelevant at a=1; only the LOOP content is measured
                outputs.push_back (loopCapture.process (s, s).first);
            }

            const int measureStart = 4410; // past the 50ms engage ramp
            const int measureLength = 8192;
            const double powerA = goertzelDb (outputs, measureStart, measureLength, toneAHz, sampleRate);
            const double powerB = goertzelDb (outputs, measureStart, measureLength, toneBHz, sampleRate);

            logMessage ("Near-zero re-arm: toneA=" + juce::String (powerA, 2) + " dB, toneB=" + juce::String (powerB, 2)
                            + " dB, B-A=" + juce::String (powerB - powerA, 2) + " dB, captures " + juce::String (capturesAfterA)
                            + " -> " + juce::String (loopCapture.getCaptureCount()));

            expect (powerB - powerA > 20.0,
                    "Loop after re-raise still holds tone A (B-A = " + juce::String (powerB - powerA, 2) + " dB)");
            expectEquals (loopCapture.getCaptureCount(), capturesAfterA + 1, "Re-raise from 0.005 should capture exactly once");
        }

        beginTest ("Jitter guard: a dial wobbling near 0 does not spam captures");
        {
            constexpr double sampleRate = 44100.0;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, (juce::uint32) 2 };

            constexpr float loopLengthMs = 200.0f;
            const int loopLengthSamples = juce::roundToInt (loopLengthMs * 0.001 * sampleRate);
            constexpr int blockSize = 512; // setLoopFreezeAmount() is called once per host block in the real engine

            LoopCapture loopCapture;
            loopCapture.prepare (spec);
            loopCapture.reset();
            loopCapture.setLoopLengthMs (loopLengthMs);

            juce::Random random (424242);
            auto noise = [&random]() { return random.nextFloat() - 0.5f; };

            loopCapture.setLoopFreezeAmount (0.0f);
            for (int i = 0; i < loopLengthSamples + 4000; ++i)
                loopCapture.process (noise(), noise());
            loopCapture.setLoopFreezeAmount (1.0f);
            for (int i = 0; i < (int) (0.3 * sampleRate); ++i)
                loopCapture.process (noise(), noise());

            // Park near 0 (never exactly 0).
            loopCapture.setLoopFreezeAmount (0.005f);
            for (int i = 0; i < (int) (0.2 * sampleRate); ++i)
                loopCapture.process (noise(), noise());

            const int capturesWhenParked = loopCapture.getCaptureCount();

            // 2s of per-block wobble strictly below the 0.01 re-arm threshold,
            // then 2s straddling it (0.004..0.016) -- dial noise around the
            // threshold itself, the case a single-threshold design would spam.
            auto wobble = [&] (float low, float high)
            {
                for (int block = 0; block < (int) (2.0 * sampleRate) / blockSize; ++block)
                {
                    loopCapture.setLoopFreezeAmount (low + (high - low) * random.nextFloat());
                    for (int i = 0; i < blockSize; ++i)
                        loopCapture.process (noise(), noise());
                }
            };

            wobble (0.001f, 0.009f);
            const int capturesAfterBelowWobble = loopCapture.getCaptureCount();
            wobble (0.004f, 0.016f);
            const int capturesAfterStraddleWobble = loopCapture.getCaptureCount();

            // A real raise afterwards captures exactly once (not once per
            // ramp sample, not repeatedly while it climbs).
            loopCapture.setLoopFreezeAmount (1.0f);
            for (int i = 0; i < (int) (0.3 * sampleRate); ++i)
                loopCapture.process (noise(), noise());
            const int capturesAfterRaise = loopCapture.getCaptureCount();

            logMessage ("Jitter guard: captures parked=" + juce::String (capturesWhenParked)
                            + ", after below-threshold wobble=" + juce::String (capturesAfterBelowWobble)
                            + ", after straddling wobble=" + juce::String (capturesAfterStraddleWobble)
                            + ", after raise=" + juce::String (capturesAfterRaise));

            expectEquals (capturesAfterBelowWobble, capturesWhenParked, "Wobble below the re-arm threshold captured");
            expectEquals (capturesAfterStraddleWobble, capturesWhenParked, "Wobble straddling the re-arm threshold captured");
            expectEquals (capturesAfterRaise, capturesWhenParked + 1, "Raise after the wobble should capture exactly once");
        }

        beginTest ("Pending capture is cancelled if Loop Mix drops back before enough history exists");
        {
            constexpr double sampleRate = 44100.0;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, (juce::uint32) 2 };

            constexpr float loopLengthMs = 500.0f;
            const int loopLengthSamples = juce::roundToInt (loopLengthMs * 0.001 * sampleRate);

            LoopCapture loopCapture;
            loopCapture.prepare (spec);
            loopCapture.reset();
            loopCapture.setLoopLengthMs (loopLengthMs);

            SineSource sine (sampleRate, 440.0, 0.5f);
            float maxPassthroughError = 0.0f;

            auto run = [&] (int numSamples)
            {
                for (int i = 0; i < numSamples; ++i)
                {
                    const float s = sine.next();
                    const auto out = loopCapture.process (s, s);
                    maxPassthroughError = juce::jmax (maxPassthroughError, std::abs (out.first - s), std::abs (out.second - s));
                }
            };

            // Raise right after reset, 100ms -- well short of 500ms history.
            loopCapture.setLoopFreezeAmount (1.0f);
            run ((int) (0.1 * sampleRate));
            const bool pendingWhileRaised = loopCapture.isCapturePending();

            // Drop back to 0 before enough history exists, then keep running
            // well past the point where history WOULD have sufficed.
            loopCapture.setLoopFreezeAmount (0.0f);
            run ((int) (0.2 * sampleRate));
            const bool pendingAfterDrop = loopCapture.isCapturePending();
            run (loopLengthSamples * 2);

            logMessage ("Pending cancel: pendingWhileRaised=" + juce::String ((int) pendingWhileRaised)
                            + ", pendingAfterDrop=" + juce::String ((int) pendingAfterDrop)
                            + ", captureCount=" + juce::String (loopCapture.getCaptureCount())
                            + ", maxPassthroughError=" + juce::String (maxPassthroughError, 6));

            expect (pendingWhileRaised, "Capture should be pending while raised with too little history");
            expect (! pendingAfterDrop, "Pending capture was not cancelled after Loop Mix dropped back");
            expectEquals (loopCapture.getCaptureCount(), 0, "A capture happened despite the cancel");
            expectEquals (loopCapture.getCurrentLoopLengthSamples(), 0, "A loop exists despite the cancel");
            expect (maxPassthroughError <= 1.0e-6f,
                    "Output was not the unchanged live wet signal (max error " + juce::String (maxPassthroughError, 6) + ")");
        }

        // -------------------------------------------------------------------
        // 2026-10-10 follow-up, "wait for sound": a diagnostic replay of the
        // user's actual launch state (Mix 1, Loop Mix 1, Loop Length
        // 3225.8ms, 48kHz, 480-sample blocks) showed the deferred capture
        // above counted SAMPLES, not SOUND -- if the source app wasn't
        // playing yet during the first N samples after prepare(), the
        // capture fired anyway on pure silence, and at Loop Mix 1 the
        // output was (1-1)*wet + 1*loop = digital zero (-200dB) forever,
        // even with -15dBFS input present. A partially silent first N gave
        // a digital-zero gap in every loop cycle instead. These four tests
        // pin the fix (an audible-history counter gated at -60dBFS, reset
        // only by a sub-gate run longer than 250ms) -- see LoopCapture.h's
        // "Wait for sound" section.
        // -------------------------------------------------------------------

        beginTest ("Launch with no sound: capture waits for real sound, not just elapsed samples");
        {
            struct LaunchCase { double sampleRate; float loopLengthMs; double silenceSeconds; int blockSize; double toneHz; };

            // Short case, plus the user's exact launch geometry (3225.8ms
            // loop at 48kHz, 480-sample blocks, setLoopFreezeAmount() once
            // per block like the real engine).
            const LaunchCase cases[] = { { 44100.0, 500.0f, 1.0, 512, 440.0 },
                                         { 48000.0, 3225.8f, 4.0, 480, 480.0 } };

            for (const auto& c : cases)
            {
                juce::dsp::ProcessSpec spec { c.sampleRate, (juce::uint32) c.blockSize, (juce::uint32) 2 };

                const int loopLengthSamples = juce::roundToInt (c.loopLengthMs * 0.001 * c.sampleRate);
                const int rampSamples = juce::roundToInt (0.05 * c.sampleRate); // must match LoopCapture::loopFreezeRampSeconds
                const int silenceSamples = (int) (c.silenceSeconds * c.sampleRate);

                LoopCapture loopCapture;
                loopCapture.prepare (spec);
                loopCapture.setLoopLengthMs (c.loopLengthMs);
                loopCapture.setLoopFreezeAmount (1.0f); // BEFORE any audio -- the restored-state launch

                SineSource sine (c.sampleRate, c.toneHz, 0.5f);

                const int totalSamples = silenceSamples + loopLengthSamples + rampSamples + (int) (1.0 * c.sampleRate);
                std::vector<float> inputs, outputs;
                inputs.reserve ((size_t) totalSamples);
                outputs.reserve ((size_t) totalSamples);
                int captureIndex = -1;

                for (int i = 0; i < totalSamples; ++i)
                {
                    if (i % c.blockSize == 0)
                        loopCapture.setLoopFreezeAmount (1.0f);

                    const float s = i < silenceSamples ? 0.0f : sine.next();
                    inputs.push_back (s);
                    outputs.push_back (loopCapture.process (s, s).first);

                    if (captureIndex < 0 && loopCapture.getCaptureCount() > 0)
                        captureIndex = i;
                }

                const juce::String tag = juce::String (c.loopLengthMs, 1) + "ms@" + juce::String ((int) c.sampleRate);

                // (a) Passthrough while waiting: every sample before the
                // capture is the untouched live wet signal (silence, then
                // the tone).
                const int passthroughEnd = captureIndex < 0 ? totalSamples : captureIndex;
                float maxPassthroughError = 0.0f;
                for (int i = 0; i < passthroughEnd; ++i)
                    maxPassthroughError = juce::jmax (maxPassthroughError, std::abs (outputs[(size_t) i] - inputs[(size_t) i]));

                // (b) Click-free deferred engage: same "transition delta vs.
                // steady-state delta" bounded-multiple metric as the restart
                // test above, steady baseline from the live tone before capture.
                float maxSteadyDelta = 0.0f;
                for (int i = silenceSamples + 1; i < silenceSamples + loopLengthSamples / 2; ++i)
                    maxSteadyDelta = juce::jmax (maxSteadyDelta, std::abs (inputs[(size_t) i] - inputs[(size_t) (i - 1)]));

                float maxEngageDelta = 0.0f;
                const int engageStart = juce::jmax (1, (captureIndex < 0 ? silenceSamples : captureIndex) - 1);
                for (int i = engageStart; i < juce::jmin (totalSamples, engageStart + 4000); ++i)
                    maxEngageDelta = juce::jmax (maxEngageDelta, std::abs (outputs[(size_t) i] - outputs[(size_t) (i - 1)]));

                // (c) The engaged loop carries the tone at a level comparable
                // to the input, measured after N + ramp + margin of tone.
                const int measureStart = silenceSamples + loopLengthSamples + rampSamples + 2000;
                const int measureLength = totalSamples - measureStart;
                const float inputRms = rms (inputs, measureStart, measureLength);
                const float outputRms = rms (outputs, measureStart, measureLength);
                const float levelRatioDb = juce::Decibels::gainToDecibels (outputRms / inputRms, -200.0f);

                logMessage ("Launch with no sound [" + tag + "]: captureIndex=" + juce::String (captureIndex)
                                + " (silence ends at " + juce::String (silenceSamples) + ", N=" + juce::String (loopLengthSamples) + ")"
                                + ", maxPassthroughError=" + juce::String (maxPassthroughError, 6)
                                + ", maxSteadyDelta=" + juce::String (maxSteadyDelta, 6)
                                + ", maxEngageDelta=" + juce::String (maxEngageDelta, 6)
                                + ", engaged level vs input=" + juce::String (levelRatioDb, 2) + " dB");

                expect (captureIndex >= silenceSamples + loopLengthSamples - 1,
                        "[" + tag + "] Capture fired at " + juce::String (captureIndex) + ", before N samples of sound existed");
                expect (maxPassthroughError <= 1.0e-6f,
                        "[" + tag + "] Waiting window was not the live wet signal (max error " + juce::String (maxPassthroughError, 6) + ")");
                expect (maxEngageDelta <= 3.0f * maxSteadyDelta,
                        "[" + tag + "] Deferred engage delta (" + juce::String (maxEngageDelta, 6) + ") was more than 3x the steady "
                            "delta (" + juce::String (maxSteadyDelta, 6) + ") -- deferred engage is not click-free");
                expect (levelRatioDb > -3.0f && levelRatioDb < 3.0f,
                        "[" + tag + "] Engaged loop level " + juce::String (levelRatioDb, 2) + " dB vs input -- loop is silent/wrong");
                expectEquals (loopCapture.getCaptureCount(), 1, "[" + tag + "] Expected exactly one (deferred) capture");
            }
        }

        beginTest ("Partial silence at the start: no digital-zero gap inside any loop cycle");
        {
            // 1s of silence, then a tone, Loop Mix 1 from the start, with a
            // 1.5s loop -- so a samples-only wait would capture 1s of silence
            // + 0.5s of tone and gap every cycle. 480Hz at 48kHz is exactly
            // 100 samples per period and N - X = 72000 - 1200 = 70800 is a
            // whole number of periods, so the seam crossfade blends the tone
            // with itself in phase -- any short-window dip measured below can
            // only be captured silence, never seam cancellation.
            constexpr double sampleRate = 48000.0;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 480, (juce::uint32) 2 };

            constexpr float loopLengthMs = 1500.0f;
            const int loopLengthSamples = juce::roundToInt (loopLengthMs * 0.001 * sampleRate);
            const int rampSamples = juce::roundToInt (0.05 * sampleRate);
            const int silenceSamples = (int) (1.0 * sampleRate);

            LoopCapture loopCapture;
            loopCapture.prepare (spec);
            loopCapture.setLoopLengthMs (loopLengthMs);
            loopCapture.setLoopFreezeAmount (1.0f);

            SineSource sine (sampleRate, 480.0, 0.5f);

            const int totalSamples = silenceSamples + 2 * loopLengthSamples + rampSamples + 2 * loopLengthSamples + 4000;
            std::vector<float> outputs;
            outputs.reserve ((size_t) totalSamples);
            int captureIndex = -1;

            for (int i = 0; i < totalSamples; ++i)
            {
                const float s = i < silenceSamples ? 0.0f : sine.next();
                outputs.push_back (loopCapture.process (s, s).first);

                if (captureIndex < 0 && loopCapture.getCaptureCount() > 0)
                    captureIndex = i;
            }

            const float toneRms = 0.5f / std::sqrt (2.0f);
            const int windowSamples = juce::roundToInt (0.01 * sampleRate); // 10ms, 4.8 tone periods
            const int analysisStart = (captureIndex < 0 ? 0 : captureIndex) + rampSamples + 1000;
            const int analysisEnd = juce::jmin (totalSamples - windowSamples, analysisStart + 2 * loopLengthSamples);

            float minWindowRms = std::numeric_limits<float>::max();
            for (int start = analysisStart; start < analysisEnd; start += windowSamples / 2)
                minWindowRms = juce::jmin (minWindowRms, rms (outputs, start, windowSamples));

            const float minWindowDb = juce::Decibels::gainToDecibels (minWindowRms / toneRms, -200.0f);

            logMessage ("Partial silence: captureIndex=" + juce::String (captureIndex)
                            + " (silence ends at " + juce::String (silenceSamples) + ", N=" + juce::String (loopLengthSamples) + ")"
                            + ", min 10ms-window RMS over two loop cycles vs tone=" + juce::String (minWindowDb, 2) + " dB");

            expect (captureIndex >= 0, "No capture happened at all");
            expect (minWindowDb > -3.0f,
                    "A 10ms window inside the engaged loop is " + juce::String (minWindowDb, 2)
                        + " dB vs the tone -- the loop captured part of the leading silence");
        }

        beginTest ("Short sub-gate pauses do not reset the audible history (same capture schedule as continuous input)");
        {
            // 100ms exact-zero gaps (well under the 250ms silence reset) every
            // 200ms of tone must not delay the capture at all: a brief pause
            // or a phrase gap is part of the music, not "nothing playing".
            constexpr double sampleRate = 44100.0;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, (juce::uint32) 2 };

            constexpr float loopLengthMs = 500.0f;
            const int loopLengthSamples = juce::roundToInt (loopLengthMs * 0.001 * sampleRate);
            const int toneRunSamples = (int) (0.2 * sampleRate);
            const int gapSamples = (int) (0.1 * sampleRate);

            auto captureIndexFor = [&] (bool withGaps)
            {
                LoopCapture loopCapture;
                loopCapture.prepare (spec);
                loopCapture.setLoopLengthMs (loopLengthMs);
                loopCapture.setLoopFreezeAmount (1.0f);

                SineSource sine (sampleRate, 440.0, 0.5f);

                for (int i = 0; i < 3 * loopLengthSamples; ++i)
                {
                    const bool inGap = withGaps && (i % (toneRunSamples + gapSamples)) >= toneRunSamples;
                    const float tone = sine.next(); // phase keeps running through the gaps
                    const float s = inGap ? 0.0f : tone;
                    loopCapture.process (s, s);

                    if (loopCapture.getCaptureCount() > 0)
                        return i;
                }

                return -1;
            };

            const int continuousIndex = captureIndexFor (false);
            const int gappedIndex = captureIndexFor (true);

            logMessage ("Short pauses: capture index continuous=" + juce::String (continuousIndex)
                            + ", with 100ms gaps=" + juce::String (gappedIndex) + " (N=" + juce::String (loopLengthSamples) + ")");

            expect (continuousIndex >= 0, "Continuous input never captured");
            expectEquals (gappedIndex, continuousIndex, "100ms gaps delayed (or advanced) the capture");
        }

        beginTest ("Long pause resets the audible history: capture waits N samples after the gap and never captures it");
        {
            constexpr double sampleRate = 44100.0;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, (juce::uint32) 2 };

            constexpr float loopLengthMs = 500.0f;
            constexpr float signalGateLinearConstant = 0.001f; // must match LoopCapture::signalGateLinear
            const int loopLengthSamples = juce::roundToInt (loopLengthMs * 0.001 * sampleRate);
            const int toneSamples = (int) (1.0 * sampleRate);
            const int gapSamples = (int) (0.4 * sampleRate); // > 250ms silence reset, < N

            LoopCapture loopCapture;
            loopCapture.prepare (spec);
            loopCapture.setLoopLengthMs (loopLengthMs);
            loopCapture.setLoopFreezeAmount (0.0f);

            SineSource sine (sampleRate, 440.0, 0.5f);

            for (int i = 0; i < toneSamples; ++i)
                { const float s = sine.next(); loopCapture.process (s, s); }
            for (int i = 0; i < gapSamples; ++i)
                loopCapture.process (0.0f, 0.0f);

            // Loop Mix rises exactly as the tone resumes.
            loopCapture.setLoopFreezeAmount (1.0f);

            const int gapEnd = toneSamples + gapSamples;
            int captureIndex = -1;
            float maxPassthroughError = 0.0f;
            bool pendingRightAfterRise = false;

            for (int i = gapEnd; i < gapEnd + 2 * loopLengthSamples; ++i)
            {
                const float s = sine.next();
                const auto out = loopCapture.process (s, s);

                if (i == gapEnd)
                    pendingRightAfterRise = loopCapture.isCapturePending();

                if (captureIndex < 0 && loopCapture.getCaptureCount() > 0)
                    captureIndex = i;

                if (captureIndex < 0)
                    maxPassthroughError = juce::jmax (maxPassthroughError, std::abs (out.first - s));
            }

            // Longest sub-gate run inside the captured loop: a sine's zero
            // crossings give isolated single samples at most; a captured
            // piece of the 400ms gap would be thousands.
            int longestSubGateRun = 0;
            int run = 0;
            for (int k = 0; k < loopCapture.getCurrentLoopLengthSamples(); ++k)
            {
                const float level = juce::jmax (std::abs (loopCapture.peekCapturedLoopSample (0, k)),
                                                std::abs (loopCapture.peekCapturedLoopSample (1, k)));
                run = level > signalGateLinearConstant ? 0 : run + 1;
                longestSubGateRun = juce::jmax (longestSubGateRun, run);
            }

            logMessage ("Long pause: captureIndex=" + juce::String (captureIndex) + " (gap ends at " + juce::String (gapEnd)
                            + ", N=" + juce::String (loopLengthSamples) + ")"
                            + ", pendingRightAfterRise=" + juce::String ((int) pendingRightAfterRise)
                            + ", longest sub-gate run in loop=" + juce::String (longestSubGateRun)
                            + ", maxPassthroughError=" + juce::String (maxPassthroughError, 6));

            expect (pendingRightAfterRise, "Rise right after a long gap should go PENDING, not capture the gap");
            expect (captureIndex >= gapEnd + loopLengthSamples - 1 && captureIndex <= gapEnd + loopLengthSamples + 1,
                    "Capture landed at " + juce::String (captureIndex) + ", expected ~" + juce::String (gapEnd + loopLengthSamples)
                        + " (N samples of sound after the gap)");
            expect (longestSubGateRun < 8,
                    "Captured loop contains a " + juce::String (longestSubGateRun) + "-sample sub-gate run -- it captured the gap");
            expect (maxPassthroughError <= 1.0e-6f,
                    "Waiting window was not the live wet signal (max error " + juce::String (maxPassthroughError, 6) + ")");
            expectEquals (loopCapture.getCaptureCount(), 1, "Expected exactly one (deferred) capture");
        }
    }
};

static LoopCaptureTests loopCaptureTests;
