#include <JuceHeader.h>
#include "../../Source/DSP/LoopCapture.h"

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
    }
};

static LoopCaptureTests loopCaptureTests;
