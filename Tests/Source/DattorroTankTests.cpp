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

        beginTest ("High setMaxShimmerBlendWeight() (near unclamped ceiling) stays finite and bounded at both partial and full Freeze");
        {
            // "Shimmer Sustain" task (see Source/DSP/DattorroTank.h's comment
            // above maxShimmerBlendWeight): this constant just became a
            // runtime-adjustable, UNCLAMPED (same convention as
            // setDecaySeconds()/setDamping()/setShimmerFeedbackGain()/
            // setFreezeAmount()) instance member via setMaxShimmerBlendWeight().
            // The header's own safety argument is that shimmerWeight+plainWeight
            // == 1.0 exactly for ANY value in [0, 1], so the combined feedback
            // gain reaching the tank stays bounded by decayGain alone --
            // proven safe at decayGain <= 0.85 (Phase 4) -- regardless of
            // maxShimmerBlendWeight. This test verifies that argument
            // empirically rather than trusting the algebra alone, at a value
            // (0.99f) near the new setter's unclamped ceiling, driven at BOTH
            // a partial freeze amount and full freeze -- same duration (3
            // minutes) and same safety bound (10.0) as the freeze-boundedness
            // test directly above, since effectiveMaxShimmerBlendWeight's
            // formula collapses to exactly frozenMaxShimmerBlendWeight at
            // freezeAmount==1.0 (the new dial's value cancels out
            // algebraically) but linearly interpolates toward it at
            // intermediate freezeAmount -- the partial-freeze case is the one
            // the algebra alone doesn't fully settle.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr double durationSeconds = 180.0; // 3 minutes, same as the freeze-boundedness test above
            constexpr int numBlocks = (int) (durationSeconds * sampleRate / blockSize);
            constexpr float highMaxShimmerBlendWeight = 0.99f;

            auto runBoundednessCheck = [&] (float freezeAmountToUse, juce::int64 randomSeed) -> float
            {
                DattorroTank tank;
                juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
                tank.prepare (spec);
                tank.reset();
                tank.setMaxShimmerBlendWeight (highMaxShimmerBlendWeight);
                tank.setFreezeAmount (freezeAmountToUse);

                juce::AudioBuffer<float> buffer (numChannels, blockSize);
                juce::Random random (randomSeed);

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
                                                                  + " (maxShimmerBlendWeight=" + juce::String (highMaxShimmerBlendWeight)
                                                                  + ", freezeAmount=" + juce::String (freezeAmountToUse) + ")");
                            maxPeak = juce::jmax (maxPeak, std::abs (data[i]));
                        }
                    }

                    expect (maxPeak <= 10.0f, "Output exceeded safety bound of 10.0 at block " + juce::String (b)
                                                   + " (maxShimmerBlendWeight=" + juce::String (highMaxShimmerBlendWeight)
                                                   + ", freezeAmount=" + juce::String (freezeAmountToUse)
                                                   + ", peak so far: " + juce::String (maxPeak) + ")");
                }

                return maxPeak;
            };

            const float partialFreezePeak = runBoundednessCheck (0.5f, 13579246);
            const float fullFreezePeak    = runBoundednessCheck (1.0f, 97531864);

            logMessage ("setMaxShimmerBlendWeight(" + juce::String (highMaxShimmerBlendWeight) + "): "
                            + "partial freeze (0.5) measured peak=" + juce::String (partialFreezePeak)
                            + ", full freeze (1.0) measured peak=" + juce::String (fullFreezePeak));
        }

        //==============================================================================
        // Shimmer cascade slice 1 (2026-10-24, docs/shimmer-character-research.md
        // section 7.1 item 1): setShimmerBlendLaw(). The external feed is the
        // tank's own feedbackFromB delayed by externalDelaySamples (100 ms)
        // through a plain circular buffer -- a stand-in for the engine's
        // shifter path (its grain pool lags ~80 ms) that is broadband and,
        // on a noise tail, nearly uncorrelated with the undelayed copy, like
        // a +12 st shift. Noise burst for burstSeconds, then silence.
        // freezeFromSample (if >= 0) steps setFreezeAmount (1.0f) there.
        auto runBlendLawScenario = [] (float equalPowerAmount, bool setLaw, double seconds, int freezeFromSample) -> std::vector<float>
        {
            constexpr double sampleRate = 44100.0;
            constexpr int externalDelaySamples = 4410;
            constexpr double burstSeconds = 1.0;

            DattorroTank tank;
            tank.prepare ({ sampleRate, 512, 2 });
            tank.reset();
            tank.setMaxShimmerBlendWeight (0.5f);
            if (setLaw)
                tank.setShimmerBlendLaw (equalPowerAmount);

            std::vector<float> delay ((size_t) externalDelaySamples, 0.0f);
            size_t delayIndex = 0;
            juce::Random random (27182818);

            const int numSamples = (int) (seconds * sampleRate);
            const int burstSamples = (int) (burstSeconds * sampleRate);
            std::vector<float> out ((size_t) numSamples, 0.0f);

            for (int n = 0; n < numSamples; ++n)
            {
                if (n == freezeFromSample)
                    tank.setFreezeAmount (1.0f);

                const float input = n < burstSamples ? random.nextFloat() * 0.6f - 0.3f : 0.0f;
                const float external = delay[delayIndex];
                out[(size_t) n] = tank.processSample (input, external);
                delay[delayIndex] = tank.peekFeedbackSignal();
                delayIndex = (delayIndex + 1) % delay.size();
            }

            return out;
        };

        auto rmsDbOver = [] (const std::vector<float>& x, double fromSeconds, double toSeconds) -> double
        {
            const size_t from = (size_t) (fromSeconds * 44100.0), to = juce::jmin (x.size(), (size_t) (toSeconds * 44100.0));
            double sum = 0.0;
            for (size_t i = from; i < to; ++i)
                sum += (double) x[i] * (double) x[i];
            return 10.0 * std::log10 (juce::jmax (1.0e-30, sum / (double) juce::jmax ((size_t) 1, to - from)));
        };

        beginTest ("setShimmerBlendLaw (0.0f) is bit-identical to an untouched tank (linear law), with a live external feed, before and after Freeze");
        {
            const int freezeFrom = (int) (3.0 * 44100.0);
            const auto untouched = runBlendLawScenario (0.0f, false, 6.0, freezeFrom);
            const auto explicitLinear = runBlendLawScenario (0.0f, true, 6.0, freezeFrom);
            expect (untouched == explicitLinear, "setShimmerBlendLaw (0.0f) changed the output -- the linear law must be the old arithmetic exactly");
        }

        beginTest ("At Freeze 1 the equal-power law is inert: setShimmerBlendLaw (1.0f) is bit-identical to the linear law");
        {
            // The (1 - freezeAmount) factor in processSample(): when frozen the
            // engine feeds an UNSHIFTED copy, correlated with feedbackFromB,
            // and equal-power weights on correlated feeds sum above unity. So
            // a fully frozen tank must not use them at all.
            const auto linear = runBlendLawScenario (0.0f, true, 4.0, 0);
            const auto equalPower = runBlendLawScenario (1.0f, true, 4.0, 0);
            expect (linear == equalPower, "setShimmerBlendLaw (1.0f) changed the fully frozen output -- Freeze must force the linear law");
        }

        beginTest ("Equal-power law keeps more loop energy than the linear law when the external feed is uncorrelated");
        {
            // shimmerWeight 0.5 (Amount 1, cap 0.5): linear keeps 0.5^2 +
            // 0.5^2 = 0.5 of the cross-feed energy per pass, equal-power
            // 0.75 + 0.25 = 1.0, so the equal-power tail must be clearly
            // louder a few seconds after the burst. Bounded too.
            const auto linear = runBlendLawScenario (0.0f, true, 6.0, -1);
            const auto equalPower = runBlendLawScenario (1.0f, true, 6.0, -1);
            const double linearDb = rmsDbOver (linear, 3.0, 4.0);
            const double equalPowerDb = rmsDbOver (equalPower, 3.0, 4.0);

            float peak = 0.0f;
            for (float v : equalPower)
            {
                expect (std::isfinite (v), "Non-finite sample with the equal-power law");
                peak = juce::jmax (peak, std::abs (v));
            }

            logMessage ("Blend law, delayed-feedback tank, RMS 3-4 s: linear " + juce::String (linearDb, 2) + " dB, equal-power "
                            + juce::String (equalPowerDb, 2) + " dB (gap " + juce::String (equalPowerDb - linearDb, 2)
                            + " dB), equal-power peak " + juce::String (peak, 4));
            expect (equalPowerDb > linearDb + 3.0, "Equal-power tail is not clearly longer than the linear one: linear "
                                                       + juce::String (linearDb, 2) + " dB, equal-power " + juce::String (equalPowerDb, 2) + " dB");
            expect (peak <= 10.0f, "Equal-power law exceeded the 10.0 bound: " + juce::String (peak, 4));
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

        //==============================================================================
        // Infinite mode (see docs/shimmer-reverb-implementation-plan.md's
        // "Infinite mode" section and DattorroTank::setInfiniteAmount()):
        // unlike Freeze, Infinite keeps feeding fresh input into a tank that
        // never decays, so new material layers on top of the sustained tail.
        // T2-T4 below zero shimmerFeedbackGain on purpose: the single-arg
        // processSample() overload always passes externalFeedback=0.0f, so
        // the class default (shimmerFeedbackGain=1.0f, i.e. shimmerWeight=0.85)
        // would leave only a 0.15 plainWeight share of the loop recirculating
        // -- a tail that dies within a second no matter what Infinite does.
        // Zeroing it gives plainWeight=1.0, the exact "bare tank, unity decay,
        // no limiter" configuration the header documents as unstable without
        // Infinite's modulation + level controller, i.e. the worst case.

        beginTest ("Infinite OFF is bit-identical: an untouched tank matches an explicit setInfiniteAmount(0.0f) sample-for-sample");
        {
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr int numBlocks = (int) (10.0 * sampleRate / blockSize); // 10s

            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };

            DattorroTank untouchedTank;
            untouchedTank.prepare (spec);
            untouchedTank.reset();

            DattorroTank explicitOffTank;
            explicitOffTank.prepare (spec);
            explicitOffTank.reset();
            explicitOffTank.setInfiniteAmount (0.0f);

            juce::AudioBuffer<float> untouchedBuffer (numChannels, blockSize);
            juce::AudioBuffer<float> explicitOffBuffer (numChannels, blockSize);
            juce::Random random (31415926);

            int numMismatches = 0;

            for (int b = 0; b < numBlocks; ++b)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* a = untouchedBuffer.getWritePointer (ch);
                    auto* e = explicitOffBuffer.getWritePointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        const float sample = random.nextFloat() * 0.6f - 0.3f; // uniform in [-0.3, 0.3]
                        a[i] = sample;
                        e[i] = sample;
                    }
                }

                juce::dsp::AudioBlock<float> untouchedBlock (untouchedBuffer);
                juce::dsp::AudioBlock<float> explicitOffBlock (explicitOffBuffer);
                untouchedTank.process (untouchedBlock);
                explicitOffTank.process (explicitOffBlock);

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* a = untouchedBuffer.getReadPointer (ch);
                    auto* e = explicitOffBuffer.getReadPointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                        if (a[i] != e[i])
                            ++numMismatches;
                }
            }

            expectEquals (numMismatches, 0, "setInfiniteAmount(0.0f) changed the output -- Infinite OFF must be bit-identical");
        }

        beginTest ("Infinite: continuous fresh input stays finite and bounded (peak <= 2.0) over several minutes of sustained input");
        {
            // Deliberately a much tighter bound than the 10.0 every other
            // boundedness test in this file uses: unity decay plus fresh input
            // grows slowly, and a 10.0 bound would let several minutes of that
            // growth hide. 2.0 is what the in-loop level controller has to hold.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr double durationSeconds = 180.0; // 3 minutes, same as the freeze-boundedness test above
            constexpr int numBlocks = (int) (durationSeconds * sampleRate / blockSize);

            DattorroTank tank;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            tank.prepare (spec);
            tank.reset();
            tank.setShimmerFeedbackGain (0.0f); // plainWeight = 1.0, see the section comment above
            tank.setInfiniteAmount (1.0f);

            juce::AudioBuffer<float> buffer (numChannels, blockSize);
            juce::Random random (27182818);

            float maxPeak = 0.0f;
            int numNonFinite = 0;
            int firstOverBoundBlock = -1;

            for (int b = 0; b < numBlocks; ++b)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* data = buffer.getWritePointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                        data[i] = random.nextFloat() * 0.6f - 0.3f;
                }

                juce::dsp::AudioBlock<float> block (buffer);
                tank.process (block);

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* data = buffer.getReadPointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        if (! std::isfinite (data[i]))
                            ++numNonFinite;
                        else
                            maxPeak = juce::jmax (maxPeak, std::abs (data[i]));
                    }
                }

                if (firstOverBoundBlock < 0 && maxPeak > 2.0f)
                    firstOverBoundBlock = b;
            }

            logMessage ("Infinite tank, 180s continuous noise: peak=" + juce::String (maxPeak, 4)
                            + (firstOverBoundBlock >= 0 ? ", first exceeded 2.0 at "
                                   + juce::String (firstOverBoundBlock * blockSize / sampleRate, 1) + "s" : juce::String()));

            expectEquals (numNonFinite, 0, "Infinite tank produced non-finite samples");
            expect (maxPeak <= 2.0f, "Infinite tank exceeded the 2.0 bound (peak: " + juce::String (maxPeak, 4) + ")");
        }

        // Shared by the two tail tests below: fills every channel with the
        // same fixed-seed +-0.3 uniform noise the rest of this file uses.
        auto fillNoise = [] (juce::AudioBuffer<float>& buffer, juce::Random& random)
        {
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                auto* data = buffer.getWritePointer (ch);
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                    data[i] = random.nextFloat() * 0.6f - 0.3f;
            }
        };

        beginTest ("Infinite: tail does not decay -- last second of 30s silence is within 3dB of seconds 3-4");
        {
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr int blocksPerSecond = (int) (sampleRate / blockSize);
            constexpr int burstBlocks = 2 * blocksPerSecond;
            constexpr int silenceBlocks = 30 * blocksPerSecond;
            constexpr int totalBlocks = burstBlocks + silenceBlocks;

            DattorroTank tank;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            tank.prepare (spec);
            tank.reset();
            tank.setShimmerFeedbackGain (0.0f);
            tank.setInfiniteAmount (1.0f);

            juce::AudioBuffer<float> buffer (numChannels, blockSize);
            juce::Random random (16180339);

            double earlySumSquares = 0.0, lateSumSquares = 0.0;
            int earlyCount = 0, lateCount = 0;

            for (int b = 0; b < totalBlocks; ++b)
            {
                if (b < burstBlocks)
                    fillNoise (buffer, random);
                else
                    buffer.clear();

                juce::dsp::AudioBlock<float> block (buffer);
                tank.process (block);

                const bool inEarlyWindow = b >= 3 * blocksPerSecond && b < 4 * blocksPerSecond;
                const bool inLateWindow  = b >= totalBlocks - blocksPerSecond;

                if (inEarlyWindow || inLateWindow)
                {
                    auto* data = buffer.getReadPointer (0);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        const double sq = (double) data[i] * (double) data[i];
                        if (inEarlyWindow) { earlySumSquares += sq; ++earlyCount; }
                        else               { lateSumSquares += sq;  ++lateCount; }
                    }
                }
            }

            const float earlyRms = (float) std::sqrt (earlySumSquares / (double) earlyCount);
            const float lateRms  = (float) std::sqrt (lateSumSquares / (double) lateCount);
            const float changeDb = (earlyRms > 0.0f && lateRms > 0.0f)
                                       ? 20.0f * std::log10 (lateRms / earlyRms) : -999.0f;

            logMessage ("Infinite tail: RMS seconds 3-4=" + juce::String (earlyRms, 5) + ", last second (31-32)="
                            + juce::String (lateRms, 5) + ", change=" + juce::String (changeDb, 2) + "dB");

            expect (std::isfinite (earlyRms) && std::isfinite (lateRms), "Non-finite RMS");
            expect (earlyRms > 1.0e-3f, "Infinite tail was already near-silent at seconds 3-4 (RMS " + juce::String (earlyRms, 6) + ")");
            expect (std::abs (changeDb) <= 3.0f, "Infinite tail drifted by " + juce::String (changeDb, 2)
                                                     + "dB over 28s (bound: +-3dB)");
        }

        beginTest ("Infinite: new input layers on top of the sustained tail instead of being muted");
        {
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr int blocksPerSecond = (int) (sampleRate / blockSize);

            // 0-2s burst, 2-10s silence (tail sustains), 10-11s second burst,
            // 11-12s silence. Sustained RMS measured 9-10s, "after" RMS 11-12s
            // -- both windows are silence-input, so the only way the second
            // can be louder is if the second burst actually entered the loop.
            constexpr int firstBurstEnd = 2 * blocksPerSecond;
            constexpr int secondBurstStart = 10 * blocksPerSecond;
            constexpr int secondBurstEnd = 11 * blocksPerSecond;
            constexpr int totalBlocks = 12 * blocksPerSecond;

            DattorroTank tank;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            tank.prepare (spec);
            tank.reset();
            tank.setShimmerFeedbackGain (0.0f);
            tank.setInfiniteAmount (1.0f);

            juce::AudioBuffer<float> buffer (numChannels, blockSize);
            juce::Random random (14142135);

            double beforeSumSquares = 0.0, afterSumSquares = 0.0;
            int beforeCount = 0, afterCount = 0;

            for (int b = 0; b < totalBlocks; ++b)
            {
                if (b < firstBurstEnd || (b >= secondBurstStart && b < secondBurstEnd))
                    fillNoise (buffer, random);
                else
                    buffer.clear();

                juce::dsp::AudioBlock<float> block (buffer);
                tank.process (block);

                const bool inBefore = b >= secondBurstStart - blocksPerSecond && b < secondBurstStart;
                const bool inAfter  = b >= secondBurstEnd;

                if (inBefore || inAfter)
                {
                    auto* data = buffer.getReadPointer (0);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        const double sq = (double) data[i] * (double) data[i];
                        if (inBefore) { beforeSumSquares += sq; ++beforeCount; }
                        else          { afterSumSquares += sq;  ++afterCount; }
                    }
                }
            }

            const float beforeRms = (float) std::sqrt (beforeSumSquares / (double) beforeCount);
            const float afterRms  = (float) std::sqrt (afterSumSquares / (double) afterCount);

            logMessage ("Infinite layering: sustained RMS before second burst (9-10s)=" + juce::String (beforeRms, 5)
                            + ", RMS just after it (11-12s)=" + juce::String (afterRms, 5)
                            + " (" + juce::String (20.0f * std::log10 (juce::jmax (afterRms, 1.0e-9f) / juce::jmax (beforeRms, 1.0e-9f)), 2) + "dB)");

            expect (std::isfinite (beforeRms) && std::isfinite (afterRms), "Non-finite RMS");
            expect (afterRms > beforeRms, "Second burst did not raise the sustained level (before "
                                              + juce::String (beforeRms, 5) + ", after " + juce::String (afterRms, 5)
                                              + ") -- Infinite must not mute fresh input");
        }

        beginTest ("Stereo taps: left output is bit-identical to the pre-stereo (left-only) tap set");
        {
            // Golden FNV-1a hash of processSample()'s return value (impulse,
            // then 3s of +-0.3 noise, then 2s of silence, default settings),
            // RECORDED by running this exact test against the pre-stereo
            // tank (commit dbc784c, Debug x64, MSVC VS2026) before the right
            // tap set was added. Adding output-only right taps must not move
            // a single bit of the left output. Since slice 2 the run pins
            // Modulation 0 and the legacy g = 0.7f (see hashLeftOutput()),
            // so the same value also proves Size/Decay/Modulation are exact
            // no-ops at those settings.
            constexpr juce::uint64 expectedLeftHash = 0x475497026ccf0cebull;
            const auto hash = hashLeftOutput();
            logMessage ("DattorroTank left-output FNV-1a fingerprint: " + juce::String::toHexString ((juce::int64) hash));
            expect (hash == expectedLeftHash, "Left output fingerprint changed: got " + juce::String::toHexString ((juce::int64) hash)
                                                  + ", expected " + juce::String::toHexString ((juce::int64) expectedLeftHash));
        }

        beginTest ("Stereo taps: impulse response L != R, normalized L/R correlation over the first 2s below 0.5");
        {
            constexpr double sampleRate = 44100.0;
            DattorroTank tank;
            tank.prepare ({ sampleRate, 512, 2 });
            tank.reset();

            double sumLL = 0.0, sumRR = 0.0, sumLR = 0.0;
            int numDifferent = 0;
            const int numSamples = (int) (2.0 * sampleRate);

            for (int n = 0; n < numSamples; ++n)
            {
                const float left = tank.processSample (n == 0 ? 1.0f : 0.0f);
                const float right = tank.getLastRightOutput();
                expect (std::isfinite (right), "Right output not finite at sample " + juce::String (n));
                sumLL += (double) left * left;
                sumRR += (double) right * right;
                sumLR += (double) left * right;
                if (left != right)
                    ++numDifferent;
            }

            const double correlation = sumLR / std::sqrt (juce::jmax (1.0e-30, sumLL * sumRR));
            logMessage ("Tank impulse response: normalized L/R correlation over 2s = " + juce::String (correlation, 6)
                            + ", L/R RMS ratio = " + juce::String (std::sqrt (sumLL / juce::jmax (1.0e-30, sumRR)), 3));

            expect (numDifferent > numSamples / 2, "L and R are mostly identical (" + juce::String (numDifferent) + " samples differ)");
            expect (sumRR > 1.0e-3 * sumLL, "Right output is near-silent");
            expect (std::abs (correlation) < 0.5, "L/R correlation too high: " + juce::String (correlation, 4));
        }
    }

    static juce::uint64 hashLeftOutput()
    {
        constexpr double sampleRate = 44100.0;
        DattorroTank tank;
        tank.prepare ({ sampleRate, 512, 2 });

        // Slice 2 (docs/reverb-space-research.md section 9): the tank's
        // defaults now mirror the new APVTS defaults (Decay 7.0 s -> g
        // 0.6993, Modulation 0.3), which move the output by design. The
        // golden hash was recorded at the old fixed g = 0.7f with no
        // modulation, so pin exactly that: Modulation 0 and the RT60 that
        // maps back to 0.7f bit-for-bit at Size 1 (asserted in
        // DattorroTankSpaceTests).
        tank.setModulation (0.0f);
        tank.setDecaySeconds (DattorroTank::decaySecondsForGain (0.7f, 1.0f));
        tank.reset();

        juce::Random random (271828);
        juce::uint64 hash = 1469598103934665603ull;
        const int numSamples = (int) (5.0 * sampleRate);

        for (int n = 0; n < numSamples; ++n)
        {
            float input = 0.0f;
            if (n == 0)
                input = 1.0f;
            else if (n < (int) (3.0 * sampleRate))
                input = random.nextFloat() * 0.6f - 0.3f;

            const float left = tank.processSample (input);
            juce::uint32 bits;
            std::memcpy (&bits, &left, sizeof (bits));
            hash = (hash ^ bits) * 1099511628211ull;
        }

        return hash;
    }
};

static DattorroTankTests dattorroTankTests;

// Slice 2 of docs/reverb-space-research.md (section 9): Decay as RT60
// seconds, Size, and always-on Modulation, on the bare tank.
class DattorroTankSpaceTests : public juce::UnitTest
{
public:
    DattorroTankSpaceTests() : juce::UnitTest ("DattorroTankSpaceTests", "DSP") {}

    void runTest() override
    {
        beginTest ("decaySecondsForGain (0.7f, 1) maps back to exactly 0.7f (golden-hash runs depend on it)");
        {
            const float legacySeconds = DattorroTank::decaySecondsForGain (0.7f, 1.0f);
            logMessage ("RT60 reproducing g = 0.7f at Size 1: " + juce::String (legacySeconds, 6) + " s; default 7.0 s gives g = "
                            + juce::String (DattorroTank::decayGainFor (7.0f, 1.0f), 6));
            expect (DattorroTank::decayGainFor (legacySeconds, 1.0f) == 0.7f, "Legacy RT60 does not map back to exactly 0.7f");
            expect (std::abs (DattorroTank::decayGainFor (7.0f, 1.0f) - 0.7f) < 0.002f, "Default 7 s should land near the old g = 0.7");
        }

        beginTest ("RT60 mapping: bare tank decay at Size 1 and 2, RT60 2 s and 5 s, within 15%");
        {
            for (float size : { 1.0f, 2.0f })
            {
                for (float rt60 : { 2.0f, 5.0f })
                {
                    const double measured = measureRt60 (size, rt60);
                    logMessage ("Size " + juce::String (size, 1) + ", RT60 " + juce::String (rt60, 1) + " s -> measured "
                                    + juce::String (measured, 3) + " s (" + juce::String (100.0 * (measured / rt60 - 1.0), 1) + "%)");
                    expect (std::abs (measured / rt60 - 1.0) <= 0.15,
                            "Measured RT60 " + juce::String (measured, 3) + " s, expected " + juce::String (rt60, 1)
                                + " s +-15% at Size " + juce::String (size, 1));
                }
            }
        }

        beginTest ("RT60 independent of Size: Size 0.5 vs 2 within 15% at RT60 4 s (below every Size's ceiling)");
        {
            const double small = measureRt60 (0.5f, 4.0f);
            const double large = measureRt60 (2.0f, 4.0f);
            logMessage ("RT60 4 s: Size 0.5 -> " + juce::String (small, 3) + " s, Size 2 -> " + juce::String (large, 3)
                            + " s (ratio " + juce::String (large / small, 3) + ")");
            expect (std::abs (large / small - 1.0) <= 0.15, "RT60 moved with Size: " + juce::String (small, 3) + " vs " + juce::String (large, 3));
        }

        beginTest ("Decay ceiling: g never exceeds 0.85 for any Decay/Size, statically or while Size glides");
        {
            float maxGain = 0.0f;
            for (int d = 0; d <= 60; ++d)
            {
                const float rt60 = DattorroTank::minDecaySeconds
                                   * std::pow (DattorroTank::maxDecaySeconds / DattorroTank::minDecaySeconds, (float) d / 60.0f);
                for (int s = 0; s <= 30; ++s)
                {
                    const float size = DattorroTank::minSize + (DattorroTank::maxSize - DattorroTank::minSize) * (float) s / 30.0f;
                    maxGain = juce::jmax (maxGain, DattorroTank::decayGainFor (rt60, size));
                }
            }
            maxGain = juce::jmax (maxGain, DattorroTank::decayGainFor (1.0e6f, 0.5f));
            expect (maxGain <= DattorroTank::maxDecayGain, "Static sweep: g reached " + juce::String (maxGain, 6));
            expect (DattorroTank::decayGainFor (DattorroTank::maxDecaySeconds, 1.0f) == DattorroTank::maxDecayGain,
                    "20 s at Size 1 is above the ceiling and must clamp to exactly 0.85");

            DattorroTank tank;
            tank.prepare ({ sampleRate, 512, 2 });
            tank.setDecaySeconds (DattorroTank::maxDecaySeconds);
            tank.reset();
            float runtimeMax = 0.0f;
            for (int n = 0; n < (int) (4.0 * sampleRate); ++n)
            {
                if (n % (int) (0.5 * sampleRate) == 0)
                    tank.setSize ((n / (int) (0.5 * sampleRate)) % 2 == 0 ? DattorroTank::minSize : DattorroTank::maxSize);
                tank.processSample (0.0f);
                runtimeMax = juce::jmax (runtimeMax, tank.getCurrentDecayGain());
            }
            logMessage ("Ceiling: static sweep max g = " + juce::String (maxGain, 6) + ", runtime max g during Size glides = " + juce::String (runtimeMax, 6));
            expect (runtimeMax <= DattorroTank::maxDecayGain, "Runtime g reached " + juce::String (runtimeMax, 6));
        }

        beginTest ("Size scales the space: first output-tap arrival doubles from Size 1 to Size 2");
        {
            const int onset1 = firstOutputSample (1.0f);
            const int onset2 = firstOutputSample (2.0f);
            const double ratio = (double) onset2 / (double) juce::jmax (1, onset1);
            logMessage ("First tap arrival: Size 1 -> sample " + juce::String (onset1) + " (" + juce::String (1000.0 * onset1 / sampleRate, 2)
                            + " ms), Size 2 -> sample " + juce::String (onset2) + " (" + juce::String (1000.0 * onset2 / sampleRate, 2)
                            + " ms), ratio " + juce::String (ratio, 4));
            expect (onset1 > 0 && onset2 > 0, "No onset found");
            expect (std::abs (ratio - 2.0) < 0.05, "Onset ratio " + juce::String (ratio, 4) + ", expected ~2");
        }

        beginTest ("Size automation (0.5 <-> 2 every 0.35 s, Modulation 1, Decay at ceiling) stays finite and bounded");
        {
            DattorroTank tank;
            tank.prepare ({ sampleRate, 512, 2 });
            tank.setShimmerFeedbackGain (0.0f); // plainWeight = 1.0, the tank's harshest recirculation case
            tank.setModulation (1.0f);
            tank.setDecaySeconds (ceilingDecaySeconds()); // g = 0.85 at every Size
            tank.reset();

            juce::Random random (31415);
            float maxPeak = 0.0f;
            int numNonFinite = 0;
            const int toggleEvery = (int) (0.35 * sampleRate);
            for (int n = 0; n < (int) (20.0 * sampleRate); ++n)
            {
                if (n % toggleEvery == 0)
                    tank.setSize ((n / toggleEvery) % 2 == 0 ? DattorroTank::maxSize : DattorroTank::minSize);
                const float left = tank.processSample (random.nextFloat() * 0.6f - 0.3f);
                const float right = tank.getLastRightOutput();
                if (! std::isfinite (left) || ! std::isfinite (right))
                    ++numNonFinite;
                else
                    maxPeak = juce::jmax (maxPeak, std::abs (left), std::abs (right));
            }
            logMessage ("Size automation sweep, 20 s noise: peak " + juce::String (maxPeak, 4));
            expectEquals (numNonFinite, 0, "Non-finite output during Size automation");
            expect (maxPeak <= 10.0f, "Size automation exceeded the 10.0 bound: " + juce::String (maxPeak, 4));
        }

        beginTest ("Size glide 1 -> 2 is click-free: max sample delta during the 300 ms glide stays within 3x steady state");
        {
            // Measures what the Thiran-interpolated allpass1 lines do at the
            // fastest delay change Size can produce (see sizeRampSeconds in
            // DattorroTank.h). A 220 Hz tone builds a steady tail at Size 1,
            // then Size jumps to 2 (smoothed over 300 ms). A pitch glide
            // raises the sample-to-sample delta only by the glide's
            // frequency ratio; a Thiran state transient at each integer
            // crossing would show up as spikes well above that.
            DattorroTank tank;
            tank.prepare ({ sampleRate, 512, 2 });
            tank.setModulation (0.0f);
            tank.reset();

            std::vector<float> left, right;
            const int switchAt = (int) (3.0 * sampleRate);
            const int total = switchAt + (int) (0.4 * sampleRate);
            const double phaseIncrement = juce::MathConstants<double>::twoPi * 220.0 / sampleRate;
            for (int n = 0; n < total; ++n)
            {
                if (n == switchAt)
                    tank.setSize (2.0f);
                left.push_back (tank.processSample (0.3f * (float) std::sin (phaseIncrement * n)));
                right.push_back (tank.getLastRightOutput());
            }

            auto maxDelta = [&] (int from, int to)
            {
                float m = 0.0f;
                for (int i = from; i < to; ++i)
                    m = juce::jmax (m, std::abs (left[(size_t) i] - left[(size_t) i - 1]), std::abs (right[(size_t) i] - right[(size_t) i - 1]));
                return m;
            };

            const float steady = maxDelta (switchAt - (int) (0.5 * sampleRate), switchAt);
            const float glide = maxDelta (switchAt, switchAt + (int) (0.3 * sampleRate));
            logMessage ("Size glide 1 -> 2: max steady delta " + juce::String (steady, 6) + ", max glide delta " + juce::String (glide, 6)
                            + ", ratio " + juce::String (glide / juce::jmax (1.0e-9f, steady), 3));
            expect (steady > 0.0f, "No steady tail");
            expect (glide <= 3.0f * steady, "Size glide delta " + juce::String (glide, 6) + " exceeds 3x steady " + juce::String (steady, 6));
        }

        beginTest ("Freeze at Size 2 + Modulation 1 + Decay at ceiling stays finite and bounded over 3 minutes of sustained input");
        {
            // Same scenario as DattorroTankTests' 3-minute freeze test, at the
            // longest lines, deepest modulation and the 0.85 decay ceiling
            // (which Freeze then overrides toward 0.999f exactly as before).
            constexpr int blockSize = 512;
            constexpr int numBlocks = (int) (180.0 * sampleRate / blockSize);
            DattorroTank tank;
            tank.prepare ({ sampleRate, (juce::uint32) blockSize, 2 });
            tank.setSize (2.0f);
            tank.setModulation (1.0f);
            tank.setDecaySeconds (ceilingDecaySeconds());
            tank.reset();
            tank.setFreezeAmount (1.0f);

            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::Random random (24681357);
            float maxPeak = 0.0f;
            int numNonFinite = 0;

            for (int b = 0; b < numBlocks; ++b)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < blockSize; ++i)
                        buffer.setSample (ch, i, random.nextFloat() * 0.6f - 0.3f);

                juce::dsp::AudioBlock<float> block (buffer);
                tank.process (block);

                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < blockSize; ++i)
                    {
                        const float v = buffer.getSample (ch, i);
                        if (! std::isfinite (v)) ++numNonFinite;
                        else maxPeak = juce::jmax (maxPeak, std::abs (v));
                    }
            }

            logMessage ("Frozen tank, Size 2 + Modulation 1, 180 s noise: peak " + juce::String (maxPeak, 4));
            expectEquals (numNonFinite, 0, "Non-finite frozen output");
            expect (maxPeak <= 10.0f, "Frozen output exceeded the 10.0 bound: " + juce::String (maxPeak, 4));
        }

        beginTest ("Modulation > 0 actually changes the output");
        {
            auto runImpulse = [] (float modulation)
            {
                DattorroTank tank;
                tank.prepare ({ sampleRate, 512, 2 });
                tank.setModulation (modulation);
                tank.reset();
                std::vector<float> out;
                for (int n = 0; n < (int) (2.0 * sampleRate); ++n)
                    out.push_back (tank.processSample (n == 0 ? 1.0f : 0.0f));
                return out;
            };
            const auto off = runImpulse (0.0f);
            const auto on = runImpulse (1.0f);
            int differing = 0;
            for (size_t i = 0; i < off.size(); ++i)
                differing += off[i] != on[i] ? 1 : 0;
            expect (differing > 0, "Modulation 1 did not change the output");
        }
    }

private:
    static constexpr double sampleRate = 44100.0;

    // An RT60 long enough that g sits at the 0.85 ceiling at every Size,
    // Size 2 included (~30.8 s there -- beyond the Decay parameter's 20 s
    // maximum, which the bare tank does not clamp).
    static float ceilingDecaySeconds()
    {
        return DattorroTank::decaySecondsForGain (DattorroTank::maxDecayGain, DattorroTank::maxSize);
    }

    // RT60 of the bare tank (no shimmer path, no modulation) from its
    // impulse response, by Schroeder backward integration: E(t) = sum of
    // h^2 from t to the end, in dB, with a least-squares line fitted over
    // -5..-35 dB (a T30-style fit) and extrapolated to -60 dB. The IR runs
    // 2 * RT60 + 1 s, so truncation sits ~120 dB down and cannot bend the
    // fitted range. L and R energies are summed.
    static double measureRt60 (float size, float rt60)
    {
        DattorroTank tank;
        tank.prepare ({ sampleRate, 512, 2 });
        tank.setShimmerFeedbackGain (0.0f); // plainWeight = 1.0: the loop gain is exactly g
        tank.setModulation (0.0f);
        tank.setSize (size);
        tank.setDecaySeconds (rt60);
        tank.reset();

        const int numSamples = (int) ((2.0 * rt60 + 1.0) * sampleRate);
        std::vector<double> energy ((size_t) numSamples);
        for (int n = 0; n < numSamples; ++n)
        {
            const double l = tank.processSample (n == 0 ? 1.0f : 0.0f);
            const double r = tank.getLastRightOutput();
            energy[(size_t) n] = l * l + r * r;
        }

        for (int n = numSamples - 2; n >= 0; --n)
            energy[(size_t) n] += energy[(size_t) n + 1];

        const double total = energy[0];
        double sumT = 0.0, sumY = 0.0, sumTT = 0.0, sumTY = 0.0;
        int count = 0;
        for (int n = 0; n < numSamples; n += 16)
        {
            const double db = 10.0 * std::log10 (juce::jmax (1.0e-300, energy[(size_t) n] / total));
            if (db > -5.0 || db < -35.0)
                continue;
            const double t = n / sampleRate;
            sumT += t; sumY += db; sumTT += t * t; sumTY += t * db;
            ++count;
        }

        const double slope = (count * sumTY - sumT * sumY) / juce::jmax (1.0e-30, count * sumTT - sumT * sumT);
        return slope < 0.0 ? -60.0 / slope : 1.0e9;
    }

    static int firstOutputSample (float size)
    {
        DattorroTank tank;
        tank.prepare ({ sampleRate, 512, 2 });
        tank.setModulation (0.0f);
        tank.setSize (size);
        tank.reset();
        for (int n = 0; n < (int) sampleRate; ++n)
        {
            const float left = tank.processSample (n == 0 ? 1.0f : 0.0f);
            if (std::abs (left) > 1.0e-7f || std::abs (tank.getLastRightOutput()) > 1.0e-7f)
                return n;
        }
        return -1;
    }
};

static DattorroTankSpaceTests dattorroTankSpaceTests;
