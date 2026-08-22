#include <JuceHeader.h>
#include <vector>
#include <limits>
#include "../../Source/DSP/ShimmerReverbEngine.h"
#include "../../Source/DSP/DattorroTank.h"

// Phase 3/4 test-runner target -- automated regression for the plan's own
// explicit Definition-of-Done requirement: "No runaway/growing-without-
// bound output ... over at least 30 seconds of sustained input." See
// docs/shimmer-reverb-implementation-plan.md, Phase 3.
//
// Rewritten for the Phase 3/4 structural fix: ShimmerReverbEngine no longer
// runs the pitch shifter as a separate, externally-gained parallel feedback
// path (that mechanism -- feedbackGain/setFeedbackGain/previousShifterOutput
// -- has been removed entirely). The shifter now sits directly inside
// DattorroTank's own recirculation path via
// DattorroTank::peekFeedbackSignal()/the two-argument processSample()
// overload, matching shimmer-reverb-architecture.md's documented topology.
// There is no longer a separate "feedback gain" to set -- the only knob is
// DattorroTank's own decayGain (default 0.6f), which these tests exercise
// at its default since ShimmerReverbEngine exposes no setter for it.
//
// These tests keep the exact same two safety properties the old
// feedbackGain-based tests checked (boundedness over 30s+, decay-to-silence
// after input stops), now verified against the new signal path.
class ShimmerReverbEngineTests : public juce::UnitTest
{
public:
    ShimmerReverbEngineTests() : juce::UnitTest ("ShimmerReverbEngineTests", "DSP") {}

    void runTest() override
    {
        beginTest ("Sustained noise stays finite and bounded for at least 30 seconds with the shifter inside the tank's own recirculation path");
        {
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            // ~30.2s at 44.1kHz/512 samples per block -- comfortably past
            // the plan's own explicit 30s minimum.
            constexpr int numBlocks = 2600;

            ShimmerReverbEngine engine;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            engine.prepare (spec);
            engine.reset();

            juce::AudioBuffer<float> buffer (numChannels, blockSize);

            // Fixed-seed pseudo-random noise, same convention as
            // DattorroTankTests: moderate live-mic-ish amplitude (+-0.3),
            // continuous across every block/sample.
            juce::Random random (13579);

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
                engine.process (block);

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

        beginTest ("Output decays to near-silence after input stops, instead of self-oscillating");
        {
            // Same tail-decay property as before the structural fix, now
            // checked with the shifter actually inside the tank's own
            // decayGain=0.7f recirculation loop rather than a separate,
            // weaker external path. If this loop turns out to sustain
            // itself indefinitely (self-oscillation) at the tank's default
            // decay, this test will fail and that is itself the important
            // finding -- not something to silently patch decayGain down
            // for without evidence.
            //
            // silenceSeconds was widened from 12.0 to 40.0 after the Phase 8
            // structural fix (docs/shimmer-reverb-implementation-plan.md,
            // 2026-08-18) made the tank's own plain (unshifted) decayGain
            // recirculation ALWAYS active, independent of shimmer -- before
            // this fix, 100% of the tank's sustain was routed through the
            // external shift path, so decay timing was governed by that
            // (lossier, differently-tuned) path. Measured directly (via a
            // temporary per-second diagnostic, since removed): with this
            // fix, the tail crosses the 1e-3 threshold around t=35.9s
            // (peak-per-second trace was strictly monotonically decreasing
            // the entire way from t=1s to t=60s, e.g. 1.362 -> 0.137 (t=12s)
            // -> 0.00298 (t=30s) -> 0.00089 (t=35.9s, first sub-threshold
            // second) -> 0.0000068 (t=60s)) -- a real, continuously-decaying
            // exponential tail, not self-oscillation, just slower now that
            // the plain tail persists on its own. 40.0s gives a real ~4s
            // margin past the measured crossing point. decayGain=0.7f is
            // unchanged by this fix (see DattorroTank.h); the slower decay
            // is the correct, expected consequence of the plain recirculation
            // no longer being replaced by the (previously lossier) shift
            // path, not a regression to paper over.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;

            constexpr double burstSeconds = 2.0;
            constexpr double silenceSeconds = 40.0;
            constexpr int burstBlocks = (int) (burstSeconds * sampleRate / blockSize);
            constexpr int silenceBlocks = (int) (silenceSeconds * sampleRate / blockSize);

            ShimmerReverbEngine engine;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            engine.prepare (spec);
            engine.reset();

            juce::AudioBuffer<float> buffer (numChannels, blockSize);
            juce::Random random (24680);

            for (int b = 0; b < burstBlocks; ++b)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* data = buffer.getWritePointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                        data[i] = random.nextFloat() * 0.6f - 0.3f;
                }

                juce::dsp::AudioBlock<float> block (buffer);
                engine.process (block);
            }

            float lastSecondPeak = 0.0f;
            constexpr int lastSecondBlocks = (int) (sampleRate / blockSize);

            for (int b = 0; b < silenceBlocks; ++b)
            {
                buffer.clear();

                juce::dsp::AudioBlock<float> block (buffer);
                engine.process (block);

                if (b >= silenceBlocks - lastSecondBlocks)
                {
                    auto* data = buffer.getReadPointer (0);
                    for (int i = 0; i < blockSize; ++i)
                        lastSecondPeak = juce::jmax (lastSecondPeak, std::abs (data[i]));
                }
            }

            expect (lastSecondPeak < 1e-3f, "Tail did not decay to near-silence " + juce::String (silenceSeconds)
                                                 + "s after input stopped (peak in final second: "
                                                 + juce::String (lastSecondPeak) + ") -- loop is self-oscillating, not decaying");
        }

        beginTest ("Stereo output channels stay finite and are decorrelated (not identical) with quadrature pitch shifting");
        {
            // Phase 4 stereo decorrelation replaces the old mono-collapse
            // contract this test used to check (L==R was correct before
            // Phase 4, when both channels only ever carried the tank's own
            // mono result -- see the removed test this replaces). Now L/R
            // deliberately draw from different quadrature-offset voice
            // pairs in PitchShifter (see PitchShifter.h's class comment and
            // ShimmerReverbEngine::process()'s mix-formula comment), so L==R
            // would mean decorrelation silently isn't happening. This test
            // asserts both halves of the new contract: (a) output stays
            // finite over the whole buffer, and (b) the channels actually
            // differ for a nontrivial fraction of samples given noise input.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            // Long enough to clear DattorroTank's own feedback round-trip
            // (branch A+B's combined allpass/delay lengths are several
            // hundred ms -- see DattorroTank.h's branchA/branchBDelay*Ms
            // constants) before measuring: until real signal has actually
            // travelled through peekFeedbackSignal() and back, both
            // safeFeedback and the quadrature-pair's safe signal are
            // exactly 0.0, so L and R are trivially identical -- not a
            // decorrelation failure, just nothing to decorrelate yet. ~3s
            // gives comfortable margin above that round-trip.
            constexpr int numBlocks = 260;

            ShimmerReverbEngine engine;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            engine.prepare (spec);
            engine.reset();

            juce::AudioBuffer<float> buffer (numChannels, blockSize);
            juce::Random random (2020);

            double sumAbsDifference = 0.0;

            for (int b = 0; b < numBlocks; ++b)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* data = buffer.getWritePointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                        data[i] = random.nextFloat() * 0.6f - 0.3f;
                }

                juce::dsp::AudioBlock<float> block (buffer);
                engine.process (block);

                auto* left = buffer.getReadPointer (0);
                auto* right = buffer.getReadPointer (1);

                for (int i = 0; i < blockSize; ++i)
                {
                    expect (std::isfinite (left[i]), "Left channel is not finite at block " + juce::String (b)
                                                          + ", sample " + juce::String (i));
                    expect (std::isfinite (right[i]), "Right channel is not finite at block " + juce::String (b)
                                                           + ", sample " + juce::String (i));

                    sumAbsDifference += std::abs ((double) left[i] - (double) right[i]);
                }
            }

            expect (sumAbsDifference > 1.0e-3, "Left and right channels are effectively identical (sum of abs "
                                                    "differences: " + juce::String (sumAbsDifference) + ") -- "
                                                    "stereo decorrelation is not happening");
        }

        // Phase 5 (see docs/shimmer-reverb-implementation-plan.md): dry/wet
        // mix and bypass tests. All three feed a deliberately distinct
        // stereo signal (different fixed per-channel values, not a
        // mono-identical burst) so a bug that collapsed the dry path to
        // mono, or that used the wrong channel's dry sample, would show up
        // as a mismatch rather than being masked by L==R inputs.
        beginTest ("Mix = 0 produces exact dry passthrough");
        {
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr int numBlocks = 20;

            ShimmerReverbEngine engine;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            engine.prepare (spec);
            engine.reset();
            engine.setMix (0.0f);

            juce::AudioBuffer<float> buffer (numChannels, blockSize);
            juce::Random random (112233);

            for (int b = 0; b < numBlocks; ++b)
            {
                // Distinct per-channel values (not the same noise on both
                // channels) so a dry path that accidentally used the
                // mono-summed signal, or swapped L/R, would fail this test.
                auto* left = buffer.getWritePointer (0);
                auto* right = buffer.getWritePointer (1);
                for (int i = 0; i < blockSize; ++i)
                {
                    left[i] = random.nextFloat() * 0.6f - 0.3f;
                    right[i] = random.nextFloat() * 0.4f - 0.2f; // different range from left, on purpose
                }

                juce::AudioBuffer<float> dryReference;
                dryReference.makeCopyOf (buffer);

                juce::dsp::AudioBlock<float> block (buffer);
                engine.process (block);

                // No epsilon needed: with mix clamped to exactly 0.0f, the
                // blend formula is dry * (1 - 0.0f) + wet * 0.0f, i.e.
                // dry * 1.0f + wet * 0.0f. Multiplying a finite float by
                // exactly 1.0f returns the identical bit pattern, and
                // multiplying any finite, non-NaN wet by exactly 0.0f
                // returns exactly 0.0f (IEEE 754), so dry + 0.0f reproduces
                // the original dry sample exactly -- this is a case where
                // an epsilon would hide a real bug (e.g. dry captured from
                // the wrong channel/sample) rather than tolerate float
                // noise, so exact equality is used.
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* processed = buffer.getReadPointer (ch);
                    auto* dry = dryReference.getReadPointer (ch);

                    for (int i = 0; i < blockSize; ++i)
                    {
                        expectEquals (processed[i], dry[i], "Mix=0 output did not exactly match dry input at "
                                                                 "channel " + juce::String (ch) + ", block "
                                                                 + juce::String (b) + ", sample " + juce::String (i));
                    }
                }
            }
        }

        beginTest ("Mix = 1 (default) stays finite and bounded, matching the pre-existing fully-wet behavior");
        {
            // Regression guard: confirms the default engine (no setMix()
            // call at all, relying purely on the 1.0f member default) is
            // still finite/bounded exactly like it was before this mix
            // feature existed -- same input convention as the file's first
            // ("30s+ boundedness") test above, just over a shorter window
            // since this test only needs to prove "unchanged", not
            // re-prove the full 30s safety margin.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr int numBlocks = 200;

            ShimmerReverbEngine engine;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            engine.prepare (spec);
            engine.reset();

            juce::AudioBuffer<float> buffer (numChannels, blockSize);
            juce::Random random (998877);

            float maxPeak = 0.0f;

            for (int b = 0; b < numBlocks; ++b)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* data = buffer.getWritePointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                        data[i] = random.nextFloat() * 0.6f - 0.3f;
                }

                juce::dsp::AudioBlock<float> block (buffer);
                engine.process (block);

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
            }

            expect (maxPeak <= 10.0f, "Default (mix=1, fully wet) output exceeded safety bound of 10.0 "
                                       "(peak: " + juce::String (maxPeak) + ") -- default behavior regressed");
        }

        beginTest ("Bypass forces dry passthrough even when mix is fully wet");
        {
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr int numBlocks = 20;

            ShimmerReverbEngine engine;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            engine.prepare (spec);
            engine.reset();

            // Deliberately set mix fully wet FIRST, then bypass -- proves
            // bypass overrides the stored mix value in the blend rather
            // than combining with it (e.g. averaging or adding).
            engine.setMix (1.0f);
            engine.setBypassed (true);

            juce::AudioBuffer<float> buffer (numChannels, blockSize);
            juce::Random random (445566);

            for (int b = 0; b < numBlocks; ++b)
            {
                auto* left = buffer.getWritePointer (0);
                auto* right = buffer.getWritePointer (1);
                for (int i = 0; i < blockSize; ++i)
                {
                    left[i] = random.nextFloat() * 0.6f - 0.3f;
                    right[i] = random.nextFloat() * 0.4f - 0.2f;
                }

                juce::AudioBuffer<float> dryReference;
                dryReference.makeCopyOf (buffer);

                juce::dsp::AudioBlock<float> block (buffer);
                engine.process (block);

                // Same exact-equality reasoning as the mix=0 test above:
                // bypassed forces effectiveMix to exactly 0.0f regardless
                // of the stored mix value, so the blend collapses to dry
                // bit-exactly.
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* processed = buffer.getReadPointer (ch);
                    auto* dry = dryReference.getReadPointer (ch);

                    for (int i = 0; i < blockSize; ++i)
                    {
                        expectEquals (processed[i], dry[i], "Bypassed output did not exactly match dry input at "
                                                                 "channel " + juce::String (ch) + ", block "
                                                                 + juce::String (b) + ", sample " + juce::String (i));
                    }
                }
            }
        }

        beginTest ("Shimmer Amount defaults to 1.0 and reproduces the pre-existing fully-recirculating behavior bit-for-bit");
        {
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr int numBlocks = 100;

            ShimmerReverbEngine defaultEngine;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            defaultEngine.prepare (spec);
            defaultEngine.reset();

            ShimmerReverbEngine explicitEngine;
            explicitEngine.prepare (spec);
            explicitEngine.reset();
            explicitEngine.setShimmerAmount (1.0f);

            juce::AudioBuffer<float> defaultBuffer (numChannels, blockSize);
            juce::AudioBuffer<float> explicitBuffer (numChannels, blockSize);

            juce::Random random (334455);

            for (int b = 0; b < numBlocks; ++b)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* defaultData = defaultBuffer.getWritePointer (ch);
                    auto* explicitData = explicitBuffer.getWritePointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        float sample = random.nextFloat() * 0.6f - 0.3f;
                        defaultData[i] = sample;
                        explicitData[i] = sample;
                    }
                }

                juce::dsp::AudioBlock<float> defaultBlock (defaultBuffer);
                juce::dsp::AudioBlock<float> explicitBlock (explicitBuffer);
                defaultEngine.process (defaultBlock);
                explicitEngine.process (explicitBlock);

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* defaultData = defaultBuffer.getReadPointer (ch);
                    auto* explicitData = explicitBuffer.getReadPointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        expectEquals (explicitData[i], defaultData[i], "setShimmerAmount(1.0f) diverged from the "
                                                                            "untouched default at channel "
                                                                            + juce::String (ch) + ", block "
                                                                            + juce::String (b) + ", sample "
                                                                            + juce::String (i));
                    }
                }
            }
        }

        beginTest ("Shimmer Amount = 0.0 stays finite and bounded (extreme value, feedback path fully cut before the tank)");
        {
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr int numBlocks = 200;

            ShimmerReverbEngine engine;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            engine.prepare (spec);
            engine.reset();
            engine.setShimmerAmount (0.0f);

            juce::AudioBuffer<float> buffer (numChannels, blockSize);
            juce::Random random (556677);

            float maxPeak = 0.0f;

            for (int b = 0; b < numBlocks; ++b)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* data = buffer.getWritePointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                        data[i] = random.nextFloat() * 0.6f - 0.3f;
                }

                juce::dsp::AudioBlock<float> block (buffer);
                engine.process (block);

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* data = buffer.getReadPointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        expect (std::isfinite (data[i]), "Sample is not finite (NaN/Inf) at block " + juce::String (b)
                                                              + ", sample " + juce::String (i));
                        maxPeak = juce::jmax (maxPeak, std::abs (data[i]));
                    }
                }
            }

            expect (maxPeak <= 10.0f, "setShimmerAmount(0.0f) exceeded the safety bound of 10.0 (peak: "
                                           + juce::String (maxPeak) + ")");
        }

        beginTest ("Shimmer Amount clamps to [0, 1] like the engine's other setters");
        {
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr int numBlocks = 20;

            ShimmerReverbEngine engine;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            engine.prepare (spec);
            engine.reset();
            engine.setShimmerAmount (5.0f); // should clamp to 1.0f, not amplify beyond unity

            juce::AudioBuffer<float> buffer (numChannels, blockSize);
            juce::Random random (778899);

            float maxPeak = 0.0f;

            for (int b = 0; b < numBlocks; ++b)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* data = buffer.getWritePointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                        data[i] = random.nextFloat() * 0.6f - 0.3f;
                }

                juce::dsp::AudioBlock<float> block (buffer);
                engine.process (block);

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* data = buffer.getReadPointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        expect (std::isfinite (data[i]), "Sample is not finite (NaN/Inf) at block " + juce::String (b)
                                                              + ", sample " + juce::String (i));
                        maxPeak = juce::jmax (maxPeak, std::abs (data[i]));
                    }
                }
            }

            expect (maxPeak <= 10.0f, "setShimmerAmount(5.0f) exceeded the safety bound of 10.0 (peak: "
                                           + juce::String (maxPeak) + ") -- value may not be clamped to 1.0f");
        }

        beginTest ("Shimmer Amount = 0.0 makes the engine's tail behave identically to a bare DattorroTank (no shimmer contribution at all)");
        {
            // Structural-fix regression: proves shimmerFeedbackGain = 0.0
            // genuinely means ZERO shimmer contribution (the tank's own
            // natural recirculation, completely independent of the shift
            // path), not just "small" -- by cross-checking
            // ShimmerReverbEngine against a bare DattorroTank fed the exact
            // same input. Width is also zeroed so the engine's wet output
            // is exactly tankOut with nothing else added (see
            // ShimmerReverbEngine::process()'s wetLeft/wetRight formula).
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr int numBlocks = 200;

            ShimmerReverbEngine engine;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            engine.prepare (spec);
            engine.reset();
            engine.setShimmerAmount (0.0f);
            engine.setWidth (0.0f);

            DattorroTank plainTank;
            plainTank.prepare (spec);
            plainTank.reset();
            // DattorroTank's own class default (shimmerFeedbackGain = 1.0f,
            // matching ShimmerReverbEngine's default so untouched use "just
            // works") would otherwise make this comparison tank claim half
            // its recirculation budget for an externalFeedback that's always
            // 0.0 (see setShimmerFeedbackGain()'s crossfade comment) --
            // explicitly zero it so this reference truly represents "zero
            // shimmer, full decayGain", matching engine.setShimmerAmount(0.0f)
            // above.
            plainTank.setShimmerFeedbackGain (0.0f);

            juce::AudioBuffer<float> buffer (numChannels, blockSize);
            juce::Random random (135790);

            for (int b = 0; b < numBlocks; ++b)
            {
                // Identical L/R (genuinely mono input) so ShimmerReverbEngine's
                // internal mono-sum exactly matches feeding the same sample
                // straight into the bare tank, sample for sample.
                auto* left = buffer.getWritePointer (0);
                auto* right = buffer.getWritePointer (1);
                for (int i = 0; i < blockSize; ++i)
                {
                    float sample = random.nextFloat() * 0.6f - 0.3f;
                    left[i] = sample;
                    right[i] = sample;
                }

                juce::AudioBuffer<float> plainReference;
                plainReference.makeCopyOf (buffer);

                juce::dsp::AudioBlock<float> block (buffer);
                engine.process (block);

                auto* plainData = plainReference.getWritePointer (0);
                for (int i = 0; i < blockSize; ++i)
                    plainData[i] = plainTank.processSample (plainData[i]);

                auto* engineLeft = buffer.getReadPointer (0);
                auto* engineRight = buffer.getReadPointer (1);
                auto* plainOut = plainReference.getReadPointer (0);

                for (int i = 0; i < blockSize; ++i)
                {
                    expectEquals (engineLeft[i], plainOut[i], "Engine's left channel diverged from the bare "
                                                                   "DattorroTank at shimmerAmount=0.0, block "
                                                                   + juce::String (b) + ", sample " + juce::String (i));
                    expectEquals (engineRight[i], plainOut[i], "Engine's right channel diverged from the bare "
                                                                    "DattorroTank at shimmerAmount=0.0, block "
                                                                    + juce::String (b) + ", sample " + juce::String (i));
                }
            }
        }

        beginTest ("Shimmer Amount = 0.0 also silences the Width decorrelation term (L/R identical even at Width=1.0)");
        {
            // Bug found by ear (2026-08-21): turning Shimmer Amount fully
            // down did not silence the shimmer character. Root cause:
            // process()'s Width/decorrelation term (sideShift = safeFeedback
            // - quadratureSafe) is computed from the pitch shifter's output
            // unconditionally, on every sample, regardless of shimmerAmount
            // -- shimmerAmount only ever gated DattorroTank's own internal
            // recirculation crossfade (see setShimmerFeedbackGain()), never
            // this separate feed-forward term. The prior regression test
            // above ("...behave identically to a bare DattorroTank...")
            // masked this exact bug by always zeroing Width alongside
            // shimmerAmount, so it never exercised this interaction. Width
            // is deliberately set to its maximum (1.0f) here, not left at
            // the default, to give the leaking term maximum amplitude to be
            // caught by if the bug regresses.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr int numBlocks = 200;

            ShimmerReverbEngine engine;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            engine.prepare (spec);
            engine.reset();
            engine.setShimmerAmount (0.0f);
            engine.setWidth (1.0f);

            juce::AudioBuffer<float> buffer (numChannels, blockSize);
            juce::Random random (246810);

            for (int b = 0; b < numBlocks; ++b)
            {
                auto* left = buffer.getWritePointer (0);
                auto* right = buffer.getWritePointer (1);
                for (int i = 0; i < blockSize; ++i)
                {
                    float sample = random.nextFloat() * 0.6f - 0.3f;
                    left[i] = sample;
                    right[i] = sample;
                }

                juce::dsp::AudioBlock<float> block (buffer);
                engine.process (block);

                auto* engineLeft = buffer.getReadPointer (0);
                auto* engineRight = buffer.getReadPointer (1);

                for (int i = 0; i < blockSize; ++i)
                {
                    expectEquals (engineLeft[i], engineRight[i], "At shimmerAmount=0.0, L/R must be identical "
                                                                       "regardless of Width -- any difference means "
                                                                       "the shift path is still leaking into the "
                                                                       "output at block " + juce::String (b)
                                                                       + ", sample " + juce::String (i));
                }
            }
        }

        beginTest ("DIAGNOSTIC: downward pitch-shift cascades die into DCBlocker's subsonic cutoff far sooner than upward cascades of equal magnitude");
        {
            // Investigating docs/shimmer-reverb-implementation-plan.md's Phase 9
            // carried-over item: "negative/downward pitch shift's shimmer being
            // barely noticeable (not yet investigated)". Hypothesis: the shifter
            // sits INSIDE the tank's own recirculation loop
            // (ShimmerReverbEngine::process(): shiftedFeedback =
            // shifter.processSample(tank.peekFeedbackSignal()), fed back in via
            // DattorroTank::processSample(input, externalFeedback)), so every
            // recirculation re-applies the same pitchRatio. An upward shift
            // (ratio > 1) marches toward higher frequencies with several octaves
            // of headroom before Nyquist; a downward shift (ratio < 1) marches
            // toward DC with only 1-2 octaves of headroom before running into
            // feedbackDcBlocker's fixed 20Hz one-pole highpass (DCBlocker.h,
            // cutoffHz), which sits in that SAME loop (right after the shift, per
            // ShimmerReverbEngine.cpp) -- so a downward cascade should get
            // progressively eaten by that highpass far sooner than an upward
            // cascade runs out of audible range on the top end. This is measured
            // directly here (bucketed per-second RMS over a long silent tail),
            // not asserted by ear, so it can be checked without an audio device.
            //
            // Same burst-then-silence shape as the "decays to near-silence" test
            // above, but tracking RMS per second (not just the final second's
            // peak) so the tail's actual survival time above a fixed noise floor
            // can be compared directly between a positive/negative pair of equal
            // magnitude.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;

            constexpr double burstSeconds = 2.0;
            constexpr double silenceSeconds = 40.0;
            constexpr int burstBlocks = (int) (burstSeconds * sampleRate / blockSize);
            constexpr int silenceBlocks = (int) (silenceSeconds * sampleRate / blockSize);
            constexpr int blocksPerSecond = (int) (sampleRate / blockSize);
            constexpr int numSecondBuckets = (int) (silenceSeconds);

            // Measures the tail: same fixed-seed burst (so +N/-N are driven by
            // bit-identical input, an apples-to-apples comparison), full default
            // shimmer strength (shimmerAmount=1.0, the class default -- the
            // setting where the shimmer cascade should be most audible), then
            // silence while per-second RMS is tracked across both channels.
            // Returns one RMS value per elapsed second of silence.
            auto measureTailRmsPerSecond = [&] (float semitones) -> std::vector<float>
            {
                ShimmerReverbEngine engine;
                juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
                engine.prepare (spec);
                engine.reset();
                engine.setPitchShiftSemitones (semitones);

                juce::AudioBuffer<float> buffer (numChannels, blockSize);
                juce::Random random (975319); // same seed for every direction/magnitude -- fair comparison

                for (int b = 0; b < burstBlocks; ++b)
                {
                    for (int ch = 0; ch < numChannels; ++ch)
                    {
                        auto* data = buffer.getWritePointer (ch);
                        for (int i = 0; i < blockSize; ++i)
                            data[i] = random.nextFloat() * 0.6f - 0.3f;
                    }

                    juce::dsp::AudioBlock<float> block (buffer);
                    engine.process (block);
                }

                std::vector<float> rmsPerSecond ((size_t) numSecondBuckets, 0.0f);

                for (int b = 0; b < silenceBlocks; ++b)
                {
                    buffer.clear();

                    juce::dsp::AudioBlock<float> block (buffer);
                    engine.process (block);

                    const int bucket = juce::jmin (numSecondBuckets - 1, b / blocksPerSecond);
                    double sumSquares = 0.0;

                    for (int ch = 0; ch < numChannels; ++ch)
                    {
                        auto* data = buffer.getReadPointer (ch);
                        for (int i = 0; i < blockSize; ++i)
                            sumSquares += (double) data[i] * (double) data[i];
                    }

                    // Accumulate into a running mean-square per bucket by
                    // reusing rmsPerSecond as a mean-square accumulator until
                    // the final sqrt pass below -- avoids a second buffer.
                    rmsPerSecond[(size_t) bucket] += (float) (sumSquares / (double) (blockSize * numChannels));
                }

                const int blocksInLastBucket = silenceBlocks - (numSecondBuckets - 1) * blocksPerSecond;
                for (int s = 0; s < numSecondBuckets; ++s)
                {
                    const int blocksInThisBucket = (s == numSecondBuckets - 1) ? blocksInLastBucket : blocksPerSecond;
                    rmsPerSecond[(size_t) s] = std::sqrt (rmsPerSecond[(size_t) s] / (float) juce::jmax (1, blocksInThisBucket));
                }

                return rmsPerSecond;
            };

            // Counts how many of the FIRST-second-onward buckets stay above
            // -60dB relative to that same trace's own peak (i.e. the loudest
            // second anywhere in its tail) -- a self-relative measure, so it's
            // comparing "how long does this direction's own cascade stay audible
            // above its own noise floor", not comparing absolute levels between
            // directions (which setShimmerAmount/decayGain don't claim to
            // equalize).
            auto countAudibleSeconds = [] (const std::vector<float>& rmsPerSecond) -> int
            {
                float peak = 0.0f;
                for (float v : rmsPerSecond)
                    peak = juce::jmax (peak, v);

                if (peak <= 0.0f)
                    return 0;

                const float threshold = peak * 0.001f; // -60dB
                int audibleSeconds = 0;
                for (float v : rmsPerSecond)
                    if (v > threshold)
                        ++audibleSeconds;

                return audibleSeconds;
            };

            for (float magnitude : { 12.0f, 24.0f })
            {
                auto upRms = measureTailRmsPerSecond (magnitude);
                auto downRms = measureTailRmsPerSecond (-magnitude);

                for (float v : upRms)
                    expect (std::isfinite (v), "Non-finite RMS bucket at +" + juce::String (magnitude) + "st");
                for (float v : downRms)
                    expect (std::isfinite (v), "Non-finite RMS bucket at -" + juce::String (magnitude) + "st");

                const int upAudibleSeconds = countAudibleSeconds (upRms);
                const int downAudibleSeconds = countAudibleSeconds (downRms);

                logMessage ("Pitch-direction asymmetry at +-" + juce::String (magnitude) + "st: "
                                + "+" + juce::String (magnitude) + "st stays above -60dB (of its own peak) for "
                                + juce::String (upAudibleSeconds) + "/" + juce::String (numSecondBuckets) + "s; "
                                + juce::String (magnitude) + "st stays above -60dB for "
                                + juce::String (downAudibleSeconds) + "/" + juce::String (numSecondBuckets) + "s"
                                + " (down: " + juce::String (downRms.empty() ? 0.0f : downRms.front(), 6)
                                + " -> ...; up: " + juce::String (upRms.empty() ? 0.0f : upRms.front(), 6) + " -> ...)");
            }
        }

        beginTest ("Freeze defaults to 0.0 and reproduces the pre-existing behavior bit-for-bit");
        {
            // Phase 9 (see docs/shimmer-reverb-implementation-plan.md): same
            // default-equivalence convention as the "Shimmer Amount defaults
            // to 1.0..." test above -- an untouched engine and one with
            // setFreezeAmount(0.0f) explicitly called must be bit-identical,
            // proving Freeze is a genuine opt-in with zero behavior change
            // at its default.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr int numBlocks = 100;

            ShimmerReverbEngine defaultEngine;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            defaultEngine.prepare (spec);
            defaultEngine.reset();

            ShimmerReverbEngine explicitEngine;
            explicitEngine.prepare (spec);
            explicitEngine.reset();
            explicitEngine.setFreezeAmount (0.0f);

            juce::AudioBuffer<float> defaultBuffer (numChannels, blockSize);
            juce::AudioBuffer<float> explicitBuffer (numChannels, blockSize);

            juce::Random random (192837);

            for (int b = 0; b < numBlocks; ++b)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* defaultData = defaultBuffer.getWritePointer (ch);
                    auto* explicitData = explicitBuffer.getWritePointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        float sample = random.nextFloat() * 0.6f - 0.3f;
                        defaultData[i] = sample;
                        explicitData[i] = sample;
                    }
                }

                juce::dsp::AudioBlock<float> defaultBlock (defaultBuffer);
                juce::dsp::AudioBlock<float> explicitBlock (explicitBuffer);
                defaultEngine.process (defaultBlock);
                explicitEngine.process (explicitBlock);

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* defaultData = defaultBuffer.getReadPointer (ch);
                    auto* explicitData = explicitBuffer.getReadPointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        expectEquals (explicitData[i], defaultData[i], "setFreezeAmount(0.0f) diverged from the "
                                                                            "untouched default at channel "
                                                                            + juce::String (ch) + ", block "
                                                                            + juce::String (b) + ", sample "
                                                                            + juce::String (i));
                    }
                }
            }
        }

        beginTest ("Freeze = 1.0 stays finite and bounded (near-unity decayGain, dry input fully muted)");
        {
            // Phase 9: the DattorroTank-level stability case (effectiveDecayGain
            // near 0.999f) is already covered at length by
            // DattorroTankTests.cpp's own freeze tests -- this test's job is
            // narrower, confirming the SAME safety property holds once
            // ShimmerReverbEngine's full chain (shifter, DC blockers, safety
            // limiter, dry-input muting) is wired around it, not re-proving
            // DattorroTank's own math from scratch.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr int numBlocks = 2600; // ~30s, same order as the file's main boundedness test

            ShimmerReverbEngine engine;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            engine.prepare (spec);
            engine.reset();
            engine.setFreezeAmount (1.0f);

            juce::AudioBuffer<float> buffer (numChannels, blockSize);
            juce::Random random (918273);

            float maxPeak = 0.0f;

            for (int b = 0; b < numBlocks; ++b)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* data = buffer.getWritePointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                        data[i] = random.nextFloat() * 0.6f - 0.3f;
                }

                juce::dsp::AudioBlock<float> block (buffer);
                engine.process (block);

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* data = buffer.getReadPointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        expect (std::isfinite (data[i]), "Sample is not finite (NaN/Inf) at block " + juce::String (b)
                                                              + ", sample " + juce::String (i) + " (Freeze engaged)");
                        maxPeak = juce::jmax (maxPeak, std::abs (data[i]));
                    }
                }

                expect (maxPeak <= 10.0f, "setFreezeAmount(1.0f) exceeded the safety bound of 10.0 at block "
                                               + juce::String (b) + " (peak so far: " + juce::String (maxPeak) + ")");
            }
        }

        beginTest ("DIAGNOSTIC: Freeze = 1.0's actual sustained tail through the full engine chain -- decay rate and short-window oscillation");
        {
            // Investigating a by-ear complaint (2026-08-22, after the pitch-ratio
            // crossfade fix above shipped): at freezeAmount=1.0 the held tail
            // "still finishes soon, oscillates too much, and sometimes sounds
            // like a motor" -- not ethereal/static as intended. The existing
            // DattorroTankTests.cpp freeze-sustain test only proves the BARE
            // tank sustains (with setShimmerFeedbackGain(0.0f), i.e. the shifter/
            // DCBlocker/SafetyLimiter chain excluded entirely) -- it never
            // measured whether the REAL product signal path (through
            // ShimmerReverbEngine, default shimmerAmount=1.0, so 85% of the
            // loop's energy runs through that chain every single pass even
            // after the pitch-ratio crossfade neutralizes the shift itself)
            // actually sustains at the same rate. Measured here instead of
            // guessed at, same discipline as the pitch-accuracy and
            // pitch-direction-asymmetry investigations above.
            //
            // Findings (2026-08-22, isolated one variable at a time): NOT
            // caused by frozenDecayGain being insufficiently close to unity
            // (0.999 -> 0.9999, a 10x reduction in per-pass loss, barely
            // moved this test's numbers at all). NOT SafetyLimiter alone
            // (temporarily raising its threshold to 100.0f, effectively
            // disabling it, changed nothing). NOT DCBlocker alone
            // (temporarily lowering its cutoff to 0.01Hz, effectively
            // disabling it, also changed nothing). Setting shimmerAmount to
            // 0.0f for this same measurement (removing the ENTIRE shifter/
            // DCBlocker/SafetyLimiter path from the recirculation blend,
            // i.e. plainWeight=1.0) dropped the 15s decay from ~-6.7dB to
            // ~-0.15dB -- proving the loss is structural, not a single lossy
            // component: DattorroTank::processSample() sums the tank's OWN
            // direct feedbackFromB with a SEPARATELY-delayed copy that took a
            // detour through PitchShifter's own internal delay line
            // (baseDelaySamples, ~80ms, entirely independent of the tank's
            // own branch delay lengths). Summing two correlated copies of the
            // same recirculating content at DIFFERENT delays is comb
            // filtering -- destructive interference at whichever frequencies
            // the two paths' delays put out of phase, which measurably
            // reduces total recirculating energy even though neither path
            // alone is lossy. This is normally masked by decayGain's own
            // (much larger) attenuation at ordinary settings; Freeze's
            // near-unity effectiveDecayGain is what makes it the dominant,
            // now-audible decay mechanism. IMPORTANT: plainWeight=1.0 (the
            // config that eliminates this decay) is the EXACT configuration
            // DattorroTankTests.cpp's freeze-boundedness test proved
            // UNSTABLE (peak exceeded the safety bound within ~1 minute) --
            // the shimmerWeight/plainWeight blend appears to be doing double
            // duty, both injecting shimmer character AND incidentally
            // detuning the raw tank network's own resonant modes enough to
            // keep it bounded at near-unity decay. Not resolved in this
            // investigation -- left as a real architectural tension for a
            // future session (see docs/shimmer-reverb-implementation-plan.md
            // Phase 9/10 notes).
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr int blocksPerSecond = (int) (sampleRate / blockSize);

            ShimmerReverbEngine engine;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            engine.prepare (spec);
            engine.reset();
            // Defaults: pitchShift=12st, shimmerAmount=1.0, feedback=0.7 -- the
            // out-of-the-box settings the user was actually listening to.

            juce::AudioBuffer<float> buffer (numChannels, blockSize);
            juce::Random random (55667788);

            // Burst 2s of noise (freeze off) to fill the tank with real
            // recirculating content, same shape as the pitch-direction-
            // asymmetry test above.
            constexpr int burstBlocks = 2 * blocksPerSecond;
            for (int b = 0; b < burstBlocks; ++b)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* data = buffer.getWritePointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                        data[i] = random.nextFloat() * 0.6f - 0.3f;
                }

                juce::dsp::AudioBlock<float> block (buffer);
                engine.process (block);
            }

            // Engage Freeze, then silence -- this is the actual "freeze a live
            // sound and let it hold" scenario, unlike the boundedness test
            // above (which keeps feeding noise throughout).
            engine.setFreezeAmount (1.0f);

            constexpr double tailSeconds = 15.0;
            constexpr int tailBlocks = (int) (tailSeconds * sampleRate / blockSize);
            std::vector<float> perBlockRms ((size_t) tailBlocks, 0.0f);

            for (int b = 0; b < tailBlocks; ++b)
            {
                buffer.clear();
                juce::dsp::AudioBlock<float> block (buffer);
                engine.process (block);

                double sumSquares = 0.0;
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto* data = buffer.getReadPointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                        sumSquares += (double) data[i] * (double) data[i];
                }

                perBlockRms[(size_t) b] = (float) std::sqrt (sumSquares / (double) (blockSize * numChannels));
            }

            for (float v : perBlockRms)
                expect (std::isfinite (v), "Non-finite RMS block in Freeze=1.0 tail measurement");

            // Decay rate: compare the first second's mean RMS (right after
            // freeze engages) against the last second's, in dB -- quantifies
            // "still finishes soon" instead of just asserting it.
            auto meanRmsOverBlocks = [&perBlockRms] (int startBlock, int count) -> float
            {
                double sum = 0.0;
                for (int i = 0; i < count; ++i)
                    sum += perBlockRms[(size_t) (startBlock + i)];
                return (float) (sum / (double) count);
            };

            const float firstSecondMean = meanRmsOverBlocks (0, blocksPerSecond);
            const float lastSecondMean  = meanRmsOverBlocks (tailBlocks - blocksPerSecond, blocksPerSecond);
            const float decayDb = 20.0f * std::log10 (juce::jmax (1.0e-9f, lastSecondMean)
                                                       / juce::jmax (1.0e-9f, firstSecondMean));

            // Short-window oscillation: within the second second of the frozen
            // tail (skipping the first second's post-engagement transient),
            // how much does block-to-block RMS swing? Expressed as the
            // max/min ratio in dB across that one-second window -- a genuinely
            // steady/ethereal hold should show only a small swing; audible
            // "motor"/warble would show a large one.
            const int oscWindowStart = blocksPerSecond; // second 1..2
            float oscMin = std::numeric_limits<float>::max();
            float oscMax = 0.0f;
            for (int i = 0; i < blocksPerSecond; ++i)
            {
                const float v = perBlockRms[(size_t) (oscWindowStart + i)];
                oscMin = juce::jmin (oscMin, v);
                oscMax = juce::jmax (oscMax, v);
            }
            const float oscillationDb = 20.0f * std::log10 (juce::jmax (1.0e-9f, oscMax) / juce::jmax (1.0e-9f, oscMin));

            logMessage ("Freeze=1.0 full-chain tail: first-second RMS=" + juce::String (firstSecondMean, 6)
                            + ", last-second (t=" + juce::String ((int) tailSeconds) + "s) RMS="
                            + juce::String (lastSecondMean, 6) + " (" + juce::String (decayDb, 2) + " dB), "
                            + "block-to-block oscillation in second 1-2: " + juce::String (oscillationDb, 2)
                            + " dB (min=" + juce::String (oscMin, 6) + ", max=" + juce::String (oscMax, 6) + ")");
        }
    }
};

static ShimmerReverbEngineTests shimmerReverbEngineTests;
