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

        beginTest ("Freeze (near-unity decayGain) stays finite and bounded over several minutes of sustained input");
        {
            // Phase 9 (see docs/shimmer-reverb-implementation-plan.md): every
            // test above this one only ever exercised decayGain < 1.0
            // strictly (max APVTS value 0.85) -- Freeze's mechanism pins the
            // tank's effective decay near unity (frozenDecayGain = 0.999f,
            // see DattorroTank.h) via setFreezeAmount(1.0f), a genuinely new
            // stability case the plan's own pitfall note calls out
            // explicitly, not an assumption that Phase 4's existing coverage
            // already applies. Run long (several minutes, not seconds) since
            // a near-unity feedback loop's own growth/decay behavior can take
            // much longer than a <1.0 loop's to reveal itself.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr double durationSeconds = 180.0; // 3 minutes
            constexpr int numBlocks = (int) (durationSeconds * sampleRate / blockSize);

            DattorroTank tank;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            tank.prepare (spec);
            tank.reset();
            tank.setFreezeAmount (1.0f);

            juce::AudioBuffer<float> buffer (numChannels, blockSize);
            juce::Random random (24681357);

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
                                                              + juce::String (b) + ", sample " + juce::String (i)
                                                              + " (Freeze engaged, effectiveDecayGain=0.999f)");
                        maxPeak = juce::jmax (maxPeak, std::abs (data[i]));
                    }
                }

                expect (maxPeak <= 10.0f, "Frozen output exceeded safety bound of 10.0 at block " + juce::String (b)
                                               + " (peak so far: " + juce::String (maxPeak) + ")");
            }
        }

        beginTest ("Freeze sustains a captured tail indefinitely, unlike normal (non-frozen) processing which decays to silence");
        {
            // Phase 9: a short burst, then silence, run in parallel through
            // two tanks -- one plain (default decayGain=0.7f), one frozen
            // (setFreezeAmount(1.0f), effectiveDecayGain=0.999f) -- proves
            // Freeze actually changes behavior (the tail keeps sustaining)
            // rather than just failing to blow up. Both tanks receive
            // bit-identical input, so any difference in the silence-phase
            // tail is attributable to the freeze mechanism alone.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;

            constexpr double burstSeconds = 2.0;
            constexpr double silenceSeconds = 15.0;
            constexpr int burstBlocks = (int) (burstSeconds * sampleRate / blockSize);
            constexpr int silenceBlocks = (int) (silenceSeconds * sampleRate / blockSize);

            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };

            // DattorroTank's own class default (shimmerFeedbackGain = 1.0f,
            // see setShimmerFeedbackGain()'s comment) would otherwise claim
            // maxShimmerBlendWeight's share (0.85) of the recirculation
            // budget for an externalFeedback that's always 0.0 here (both
            // tanks are driven through the single-arg processSample()/
            // process() convenience overload) -- explicitly zeroing it makes
            // BOTH tanks' comparison isolate decayGain/effectiveDecayGain
            // alone, matching ShimmerReverbEngineTests' identical fix for
            // its own bare-tank comparison test.
            DattorroTank plainTank;
            plainTank.prepare (spec);
            plainTank.reset();
            plainTank.setShimmerFeedbackGain (0.0f);

            DattorroTank frozenTank;
            frozenTank.prepare (spec);
            frozenTank.reset();
            frozenTank.setShimmerFeedbackGain (0.0f);
            frozenTank.setFreezeAmount (1.0f);

            juce::AudioBuffer<float> plainBuffer (numChannels, blockSize);
            juce::AudioBuffer<float> frozenBuffer (numChannels, blockSize);
            juce::Random random (975300);

            for (int b = 0; b < burstBlocks; ++b)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* plainData = plainBuffer.getWritePointer (ch);
                    auto* frozenData = frozenBuffer.getWritePointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        float sample = random.nextFloat() * 0.6f - 0.3f;
                        plainData[i] = sample;
                        frozenData[i] = sample;
                    }
                }

                juce::dsp::AudioBlock<float> plainBlock (plainBuffer);
                juce::dsp::AudioBlock<float> frozenBlock (frozenBuffer);
                plainTank.process (plainBlock);
                frozenTank.process (frozenBlock);
            }

            float plainLastSecondPeak = 0.0f;
            float frozenLastSecondPeak = 0.0f;
            constexpr int lastSecondBlocks = (int) (sampleRate / blockSize);

            for (int b = 0; b < silenceBlocks; ++b)
            {
                plainBuffer.clear();
                frozenBuffer.clear();

                juce::dsp::AudioBlock<float> plainBlock (plainBuffer);
                juce::dsp::AudioBlock<float> frozenBlock (frozenBuffer);
                plainTank.process (plainBlock);
                frozenTank.process (frozenBlock);

                if (b >= silenceBlocks - lastSecondBlocks)
                {
                    auto* plainData = plainBuffer.getReadPointer (0);
                    auto* frozenData = frozenBuffer.getReadPointer (0);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        plainLastSecondPeak = juce::jmax (plainLastSecondPeak, std::abs (plainData[i]));
                        frozenLastSecondPeak = juce::jmax (frozenLastSecondPeak, std::abs (frozenData[i]));
                    }
                }
            }

            expect (std::isfinite (plainLastSecondPeak) && std::isfinite (frozenLastSecondPeak),
                    "Non-finite peak measured (plain: " + juce::String (plainLastSecondPeak)
                        + ", frozen: " + juce::String (frozenLastSecondPeak) + ")");

            // decayGain=0.7f's own several-hundred-ms round trip (see
            // DattorroTank.h's branchA/B delay-length constants) means 15s of
            // silence is comfortably past its decay-to-near-silence point --
            // same 1e-3 order-of-magnitude margin ShimmerReverbEngineTests
            // uses for its own decay-to-silence assertion.
            expect (plainLastSecondPeak < 1e-3f, "Plain (non-frozen) tank did not decay to near-silence after "
                                                      + juce::String (silenceSeconds) + "s of silence (peak: "
                                                      + juce::String (plainLastSecondPeak) + ")");

            // effectiveDecayGain=0.999f loses only ~0.1% amplitude per
            // recirculation pass -- over the same 15s window the frozen tail
            // should remain orders of magnitude louder than the plain tank's
            // near-silence floor, demonstrating Freeze actually sustains the
            // captured tail rather than merely failing to blow up.
            expect (frozenLastSecondPeak > 1e-2f, "Frozen tank decayed too much after " + juce::String (silenceSeconds)
                                                       + "s of silence (peak: " + juce::String (frozenLastSecondPeak)
                                                       + ") -- Freeze should sustain the tail, not let it fade like "
                                                         "normal processing");
        }

        beginTest ("DIAGNOSTIC: Damping's APVTS range (0..0.05) produces almost no audible spectral difference");
        {
            // Investigating a by-ear complaint (2026-08-22): "the damping dial
            // seems to not provide any noticeable difference in sound." Checked
            // the math before touching anything (per this project's "verify
            // audible-quality claims against actual runtime values" discipline):
            // the one-pole leaky integrator's actual cutoff frequency from a
            // coefficient a is fc = -(sampleRate / 2*pi) * ln(a) (this is the
            // exact relationship defaultDampingCoefficient's own header comment
            // uses to derive "an 8000Hz cutoff corresponds to a~=0.32"). At
            // a=0.05 (Parameters.cpp's current MAXIMUM), fc ~= 21kHz; at
            // a=0.0005 (the default, and this range's practical minimum), fc is
            // WAY above Nyquist (~53kHz) -- meaning the entire 0..0.05 slider
            // only ever sweeps cutoff from "no filtering at all" to "still only
            // rolling off content above 21kHz," nowhere near the documented
            // ~8kHz ("a~=0.32") reference point where damping actually becomes
            // audible as a "duller wash." Measured directly here instead of
            // trusting the hand math alone: drives the same DattorroTank with
            // identical noise input at several coefficients and compares a
            // simple high-frequency-energy proxy (RMS of the consecutive-sample
            // difference, which emphasizes high-frequency content the way a
            // differentiator/crude highpass would) relative to the raw tail's
            // own RMS -- a real spectral-tilt measurement, not just a level
            // check.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr int burstBlocks = (int) (2.0 * sampleRate / blockSize);
            constexpr int tailBlocks = (int) (1.0 * sampleRate / blockSize);

            // Returns the ratio of high-frequency-proxy RMS to total RMS over
            // one second of tail, right after a 2-second noise burst -- higher
            // means brighter/less damped.
            auto measureBrightness = [&] (float damping) -> float
            {
                DattorroTank tank;
                juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
                tank.prepare (spec);
                tank.reset();
                tank.setDamping (damping);

                juce::AudioBuffer<float> buffer (numChannels, blockSize);
                juce::Random random (13571113); // same seed for every damping value -- fair comparison

                for (int b = 0; b < burstBlocks; ++b)
                {
                    for (int ch = 0; ch < numChannels; ++ch)
                    {
                        auto* data = buffer.getWritePointer (ch);
                        for (int i = 0; i < blockSize; ++i)
                            data[i] = random.nextFloat() * 0.6f - 0.3f;
                    }

                    juce::dsp::AudioBlock<float> block (buffer);
                    tank.process (block);
                }

                double totalSumSquares = 0.0;
                double diffSumSquares = 0.0;
                float previousSample = 0.0f;

                for (int b = 0; b < tailBlocks; ++b)
                {
                    for (int ch = 0; ch < numChannels; ++ch)
                    {
                        auto* data = buffer.getWritePointer (ch);
                        for (int i = 0; i < blockSize; ++i)
                            data[i] = random.nextFloat() * 0.6f - 0.3f;
                    }

                    juce::dsp::AudioBlock<float> block (buffer);
                    tank.process (block);

                    auto* data = buffer.getReadPointer (0);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        totalSumSquares += (double) data[i] * (double) data[i];
                        float diff = data[i] - previousSample;
                        diffSumSquares += (double) diff * (double) diff;
                        previousSample = data[i];
                    }
                }

                const float totalRms = (float) std::sqrt (totalSumSquares / (double) (tailBlocks * blockSize));
                const float diffRms  = (float) std::sqrt (diffSumSquares / (double) (tailBlocks * blockSize));

                return totalRms > 1.0e-9f ? diffRms / totalRms : 0.0f;
            };

            const float brightnessAtDefault  = measureBrightness (0.0005f); // current default
            const float brightnessAtRangeMax = measureBrightness (0.05f);   // current APVTS maximum
            const float brightnessAtDocumentedDull = measureBrightness (0.32f); // documented "sounds dull" reference

            expect (std::isfinite (brightnessAtDefault) && std::isfinite (brightnessAtRangeMax)
                        && std::isfinite (brightnessAtDocumentedDull), "Non-finite brightness measurement");

            logMessage ("Damping brightness proxy (higher = brighter/less damped): default(0.0005)="
                            + juce::String (brightnessAtDefault, 6) + ", current APVTS max(0.05)="
                            + juce::String (brightnessAtRangeMax, 6) + ", documented-dull reference(0.32)="
                            + juce::String (brightnessAtDocumentedDull, 6));

            // The whole point of this diagnostic: confirm the current range's
            // min-to-max swing is a small fraction of the swing between the
            // default and the documented-dull reference -- i.e. the dial's
            // usable range barely moves the needle compared to what "damping
            // audibly working" actually looks like.
            const float currentRangeSwing = std::abs (brightnessAtDefault - brightnessAtRangeMax);
            const float fullUsefulSwing   = std::abs (brightnessAtDefault - brightnessAtDocumentedDull);

            logMessage ("Current APVTS range covers " + juce::String (fullUsefulSwing > 1.0e-9f
                            ? (currentRangeSwing / fullUsefulSwing) * 100.0f : 0.0f, 1)
                            + "% of the brightness swing between the default and the documented-dull reference");
        }
    }
};

static DattorroTankTests dattorroTankTests;
