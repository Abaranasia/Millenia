#include <JuceHeader.h>
#include <vector>
#include <limits>
#include <algorithm>
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

        beginTest ("Shimmer Sustain defaults to 0.85 and an untouched engine matches explicitly setting that default");
        {
            // "Shimmer Sustain" task: setShimmerSustain() has two effects
            // (see its header comment) -- an INVERTED forward to
            // DattorroTank::setMaxShimmerBlendWeight(), and a direct gate on
            // process()'s Width injection. Its own default
            // (ShimmerReverbEngine::defaultShimmerSustainAmount, matching
            // Parameters.cpp's APVTS default) is 0.85f -- a freshly-chosen
            // default for this control's widened scope, deliberately NOT
            // preserved bit-identical to any prior hardcoded constant (see
            // defaultShimmerSustainAmount's comment for why that guarantee
            // was dropped). This test only proves internal consistency: an
            // untouched engine matches one with the default explicitly set,
            // same discipline as the Shimmer Amount/Freeze default-regression
            // tests above use for THEIR still-preserved defaults.
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
            explicitEngine.setShimmerSustain (0.85f);

            juce::AudioBuffer<float> defaultBuffer (numChannels, blockSize);
            juce::AudioBuffer<float> explicitBuffer (numChannels, blockSize);

            juce::Random random (445566);

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
                        expectEquals (explicitData[i], defaultData[i], "setShimmerSustain(0.85f) diverged from the "
                                                                            "untouched default at channel "
                                                                            + juce::String (ch) + ", block "
                                                                            + juce::String (b) + ", sample "
                                                                            + juce::String (i));
                    }
                }
            }
        }

        beginTest ("Shimmer Sustain at 0.0f collapses the Width injection to mono, an unmistakable audible gate");
        {
            // "Shimmer Sustain" task, 2026-09-06 widening (see
            // setShimmerSustain()'s header comment, effect 2): this is the
            // deterministic regression test for the fix to the by-ear report
            // "I don't clearly notice any difference [moving the dial]" --
            // shimmerSustainAmount now directly gates process()'s
            // sideShift/wetLeft/wetRight Width injection, the same
            // multiplicative-gate convention shimmerAmount=0.0f and
            // width=0.0f already use individually to collapse to mono. Uses
            // Width and Shimmer Amount both at FULL strength (1.0f) so this
            // test isn't just re-proving one of those two pre-existing
            // mono-collapse cases -- Shimmer Sustain=0.0f must independently
            // collapse to mono even when neither of the other two gates is
            // doing it.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr int numBlocks = 20;

            ShimmerReverbEngine engine;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            engine.prepare (spec);
            engine.reset();
            engine.setWidth (1.0f);
            engine.setShimmerAmount (1.0f);
            engine.setShimmerSustain (0.0f);

            juce::AudioBuffer<float> buffer (numChannels, blockSize);
            juce::Random random (778899);

            for (int b = 0; b < numBlocks; ++b)
            {
                for (int i = 0; i < blockSize; ++i)
                {
                    float sample = random.nextFloat() * 0.6f - 0.3f;
                    buffer.setSample (0, i, sample);
                    buffer.setSample (1, i, sample);
                }

                juce::dsp::AudioBlock<float> block (buffer);
                engine.process (block);

                auto* left  = buffer.getReadPointer (0);
                auto* right = buffer.getReadPointer (1);

                for (int i = 0; i < blockSize; ++i)
                    expectEquals (right[i], left[i], "setShimmerSustain(0.0f) failed to collapse L/R to mono at "
                                                          "block " + juce::String (b) + ", sample " + juce::String (i)
                                                          + " (Width=1.0, Shimmer Amount=1.0 -- the Width injection "
                                                          + "gate must still zero out on its own)");
            }
        }

        beginTest ("Shimmer Sustain: a HIGH (dial) value decays measurably slower than a LOW value at ordinary (non-frozen, default decayGain) settings");
        {
            // "Shimmer Sustain" task: verifies the new knob actually does
            // something audible-proxy-measurable (not just that it's wired
            // up), at ordinary settings -- no Freeze involved, default
            // feedback/decayGain. Same burst-then-silence-then-measure-RMS
            // shape as the Freeze=1.0 full-chain tail diagnostic above, but
            // comparing two engines (low vs. high setShimmerSustain()) instead
            // of comparing time within one engine.
            //
            // IMPORTANT history (2026-09-06, two findings, in order):
            // (1) The underlying DattorroTank mechanism is backwards from
            // what "sustain" suggests -- a HIGHER maxShimmerBlendWeight
            // measurably decays FASTER, not slower. This is the SAME
            // comb-filtering loss DattorroTank.h's frozenMaxShimmerBlendWeight
            // comment already documents at near-unity (Freeze) decay: raising
            // maxShimmerBlendWeight increases shimmerWeight (at
            // shimmerAmount's default of 1.0, shimmerWeight ==
            // maxShimmerBlendWeight directly), which shifts MORE of the
            // tank's recirculating budget from the cheap, undelayed natural
            // feedbackFromB path onto the externally-shifted path -- a
            // SEPARATELY-delayed copy of the same recirculating content
            // (PitchShifter's own ~80ms internal delay), which measurably
            // loses energy every pass via destructive interference. That
            // loss is not exclusive to near-unity decay -- it is present at
            // ANY decayGain, just far more visible once decayGain's own
            // (much larger) attenuation is out of the way.
            // (2) By-ear follow-up, same day, two rounds: first, the user
            // found this direction unintuitive -- setShimmerSustain() now
            // DELIBERATELY INVERTS the mapping (1.0f - newAmount before
            // forwarding to DattorroTank), so the USER-FACING dial's high end
            // now maps to the LOW (slow-decay) end of the underlying
            // mechanism -- this test's low/high sample points and assertion
            // direction below are therefore the OPPOSITE of finding (1)
            // above, at the ShimmerReverbEngine::setShimmerSustain() level
            // (DattorroTank's own setMaxShimmerBlendWeight() is untouched and
            // still behaves per finding (1)). Second, the user reported not
            // clearly noticing a difference at all -- traced to
            // process()'s Width/decorrelation injection never having been
            // gated by this parameter in the first place (see
            // setShimmerSustain()'s header comment, effect 2, and the new
            // "collapses to mono" test above) -- shimmerSustainAmount now
            // ALSO gates that term directly, so this test's measured dB gap
            // below is now driven by BOTH effects together, not just the
            // tank-recirculation effect alone. Measured at the full [0, 1]
            // dial range (needed for the effect to clear the noise floor at
            // ordinary decayGain=0.7; smaller ranges near the 0.85f default produce
            // too small a difference to reliably assert on). Not asserting
            // strict monotonicity across the full range, matching this
            // codebase's established discipline for the analogous frozen-cap
            // sweep -- just this one low-vs-high comparison.
            static constexpr double sampleRate = 44100.0;
            static constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            static constexpr int blocksPerSecond = (int) (sampleRate / blockSize);

            constexpr float lowShimmerSustain = 0.0f;
            constexpr float highShimmerSustain = 1.0f;

            auto measureTailDecayDb = [&] (float shimmerSustainToUse, juce::int64 randomSeed) -> float
            {
                ShimmerReverbEngine engine;
                juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
                engine.prepare (spec);
                engine.reset();
                engine.setShimmerSustain (shimmerSustainToUse);
                // Defaults otherwise: pitchShift=12st, shimmerAmount=1.0,
                // feedback=0.7, freeze off -- ordinary, non-frozen settings.

                juce::AudioBuffer<float> buffer (numChannels, blockSize);
                juce::Random random (randomSeed);

                // Burst 2s of noise to fill the tank with real recirculating
                // content, then silence -- same shape as the Freeze full-chain
                // tail diagnostic above.
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

                constexpr double tailSeconds = 5.0;
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
                    expect (std::isfinite (v), "Non-finite RMS block in setShimmerSustain("
                                                    + juce::String (shimmerSustainToUse) + ") tail measurement");

                auto meanRmsOverBlocks = [&perBlockRms] (int startBlock, int count) -> float
                {
                    double sum = 0.0;
                    for (int i = 0; i < count; ++i)
                        sum += perBlockRms[(size_t) (startBlock + i)];
                    return (float) (sum / (double) count);
                };

                const float firstSecondMean = meanRmsOverBlocks (0, blocksPerSecond);
                const float lastSecondMean  = meanRmsOverBlocks (tailBlocks - blocksPerSecond, blocksPerSecond);
                return 20.0f * std::log10 (juce::jmax (1.0e-9f, lastSecondMean) / juce::jmax (1.0e-9f, firstSecondMean));
            };

            const float lowDecayDb  = measureTailDecayDb (lowShimmerSustain, 11223344);
            const float highDecayDb = measureTailDecayDb (highShimmerSustain, 11223344);

            logMessage ("setShimmerSustain() tail decay over 5s: low (" + juce::String (lowShimmerSustain) + ") = "
                            + juce::String (lowDecayDb, 2) + " dB, high (" + juce::String (highShimmerSustain) + ") = "
                            + juce::String (highDecayDb, 2) + " dB");

            expect (lowDecayDb < highDecayDb, "Expected setShimmerSustain(" + juce::String (lowShimmerSustain)
                                                   + ") to decay measurably faster (more negative dB) than "
                                                   + "setShimmerSustain(" + juce::String (highShimmerSustain)
                                                   + ") over the same 5s tail window (see this test's comment for the "
                                                   + "inverted dial mapping) -- got low=" + juce::String (lowDecayDb, 2)
                                                   + " dB, high=" + juce::String (highDecayDb, 2) + " dB");
        }

        beginTest ("Shimmer Amount=1, Shimmer Sustain=1 (max), Width=1 -- confirmed root cause, regression-tests the fix");
        {
            // "Shimmer Sustain" task follow-up (2026-09-06, by-ear report):
            // "when shimmer amount and sustain values are high, higher values
            // of Width returns random weird sounds, like if it's a glitch."
            //
            // ROOT-CAUSED (not guessed) via a real investigation: this
            // codebase's own already-documented "pitch oscillation" issue
            // (see PitchShifter::findAlignmentOffset()'s class comment) --
            // on near-periodic content, the primary/quadrature grain pools'
            // independent alignment searches can settle on drastically
            // different, comparably-scoring offsets. A bare PitchShifter fed
            // the same tone directly stayed perfectly well-behaved (ruling
            // out the shifter alone); a dedicated divergence diagnostic
            // (below) measured up to ~265 samples of primary/quadrature
            // offset divergence, ~178 samples average, ONLY once
            // shimmerSustainAmount drives maxShimmerBlendWeight toward 0.0 --
            // removing the shimmer-cascade content that used to keep the
            // tank's own recirculation just complex enough to avoid the
            // ambiguity. Given this project's own four prior, all-failed
            // attempts to fix that ambiguity at the alignment-search layer
            // itself, the chosen fix instead bounds where it actually reaches
            // the output: `sideShift` (the Width injection) is now passed
            // through the existing, stateless safetyLimiter before use (see
            // process()'s comment at that call site) --
            // safeFeedback/quadratureSafe were each individually bounded
            // BEFORE the subtraction, but their DIFFERENCE never was. This
            // test now asserts the fix's actual bound, not just a first
            // measurement pass.
            //
            // (Original hypothesis, confirmed correct): shimmerSustainAmount=1.0 drives maxShimmerBlendWeight to 0.0 --
            // the tank's own recirculation then carries ZERO shimmer-cascade
            // content (a plain, undamped decaying resonance only), unlike the
            // old always-0.85f-weighted tank, which always had substantial
            // shimmer energy blended into its own feedback. A cleaner,
            // more strongly periodic dry signal reaching PitchShifter could
            // plausibly make the primary/quadrature grain pools' shifted
            // outputs diverge MORE (not less) -- this codebase already has a
            // documented, unresolved "pitch oscillation" issue tied to
            // near-periodic content's correlation-search behavior (see
            // docs/shimmer-reverb-implementation-plan.md and Engram topic
            // millenia/pitch-oscillation-investigation) -- so this may be
            // that same known mechanism becoming newly reachable/audible
            // through a combination that was never reachable before this
            // parameter existed, not a brand new bug.
            //
            // Measures the ACTUAL peak of wetLeft/wetRight and of
            // |wetLeft - wetRight| (a direct proxy for the injected
            // sideShift term's own peak magnitude, without needing a new
            // test-only peek method) at the reported combination, against a
            // control run at a low Shimmer Sustain (weight close to the old
            // hardcoded 0.85f default) with the same Amount/Width -- both
            // driven by a SUSTAINED TONE (not noise), matching this
            // codebase's own established finding that periodicity is what
            // triggers this class of artifact, not broadband content.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr float toneFrequencyHz = 220.0f;

            auto measurePeaks = [&] (float shimmerSustainToUse) -> std::pair<float, float>
            {
                ShimmerReverbEngine engine;
                juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
                engine.prepare (spec);
                engine.reset();
                engine.setShimmerAmount (1.0f);
                engine.setShimmerSustain (shimmerSustainToUse);
                engine.setWidth (1.0f);
                // Defaults otherwise: pitchShift=12st, feedback=0.7, freeze off.

                juce::AudioBuffer<float> buffer (numChannels, blockSize);

                constexpr double totalSeconds = 6.0; // let the tank reach its own steady-state resonance
                const int totalBlocks = (int) (totalSeconds * sampleRate / blockSize);
                double phase = 0.0;
                const double phaseIncrement = 2.0 * juce::MathConstants<double>::pi * toneFrequencyHz / sampleRate;

                float peakWet = 0.0f;
                float peakDiff = 0.0f;

                for (int b = 0; b < totalBlocks; ++b)
                {
                    for (int i = 0; i < blockSize; ++i)
                    {
                        const float sample = 0.3f * (float) std::sin (phase);
                        phase += phaseIncrement;
                        buffer.setSample (0, i, sample);
                        buffer.setSample (1, i, sample);
                    }

                    juce::dsp::AudioBlock<float> block (buffer);
                    engine.process (block);

                    auto* left  = buffer.getReadPointer (0);
                    auto* right = buffer.getReadPointer (1);

                    for (int i = 0; i < blockSize; ++i)
                    {
                        expect (std::isfinite (left[i]) && std::isfinite (right[i]),
                                "Non-finite sample at block " + juce::String (b) + ", sample " + juce::String (i)
                                    + " (shimmerSustain=" + juce::String (shimmerSustainToUse) + ")");

                        peakWet  = juce::jmax (peakWet, std::abs (left[i]), std::abs (right[i]));
                        peakDiff = juce::jmax (peakDiff, std::abs (left[i] - right[i]));
                    }
                }

                return { peakWet, peakDiff };
            };

            const auto highSustainPeaks = measurePeaks (1.0f);
            const auto lowSustainPeaks  = measurePeaks (0.15f);

            logMessage ("Shimmer Amount=1, Width=1, 220Hz tone, 6s: "
                            + juce::String ("high Sustain (1.0) peakWet=") + juce::String (highSustainPeaks.first, 4)
                            + ", peak|L-R|=" + juce::String (highSustainPeaks.second, 4)
                            + " -- low Sustain (0.15) peakWet=" + juce::String (lowSustainPeaks.first, 4)
                            + ", peak|L-R|=" + juce::String (lowSustainPeaks.second, 4));

            expect (highSustainPeaks.first <= 10.0f, "High-Sustain peak exceeded the 10.0 safety ceiling: "
                                                          + juce::String (highSustainPeaks.first));
            expect (lowSustainPeaks.first <= 10.0f, "Low-Sustain peak exceeded the 10.0 safety ceiling: "
                                                         + juce::String (lowSustainPeaks.first));

            // The real regression assertion: peak|L-R| (the injected
            // sideShift's own contribution) used to reach 2.0 at high
            // Sustain before the fix (two independently-limited ~0.8f-ceiling
            // signals landing in full antiphase). safetyLimiter's asymptote
            // approaches but never reaches 1.0f -- 1.2f leaves a small margin
            // above that asymptote while still catching a regression back to
            // anything near the old 2.0f.
            expect (highSustainPeaks.second <= 1.2f, "High-Sustain peak|L-R| regression: expected the safetyLimiter "
                                                          "fix on sideShift to bound this well under the old unfixed "
                                                          "2.0 peak, got " + juce::String (highSustainPeaks.second));
        }

        beginTest ("DIAGNOSTIC: what peekDryFeedback() actually looks like reaching the shifter, high vs. low Sustain");
        {
            // Glitch investigation, second pass: a BARE PitchShifter fed the
            // same 220Hz tone directly stayed perfectly well-behaved
            // (peak|primary-quadrature|=0.3, matching input amplitude, see
            // Tests/Source/PitchShifterTests.cpp's new diagnostic) -- so the
            // large L-R divergence measured above is NOT inherent to
            // PitchShifter's quadrature mechanism in isolation. This test
            // inspects the ACTUAL signal reaching the shifter inside the real
            // feedback loop (via the new peekDryFeedback() test-only
            // accessor), sample-by-sample (blockSize=1, so peekDryFeedback()
            // genuinely returns every sample, not one per 512-sample block),
            // to see directly what's different about it.
            constexpr double sampleRate = 44100.0;
            constexpr int numChannels = 2;
            constexpr float toneFrequencyHz = 220.0f;
            constexpr int snippetLength = 40;

            auto captureSnippet = [&] (float shimmerSustainToUse) -> std::vector<float>
            {
                ShimmerReverbEngine engine;
                juce::dsp::ProcessSpec spec { sampleRate, 1, (juce::uint32) numChannels };
                engine.prepare (spec);
                engine.reset();
                engine.setShimmerAmount (1.0f);
                engine.setShimmerSustain (shimmerSustainToUse);
                engine.setWidth (1.0f);

                juce::AudioBuffer<float> buffer (numChannels, 1);

                constexpr double totalSeconds = 6.0;
                const int totalSamples = (int) (totalSeconds * sampleRate);
                double phase = 0.0;
                const double phaseIncrement = 2.0 * juce::MathConstants<double>::pi * toneFrequencyHz / sampleRate;

                std::vector<float> snippet;
                snippet.reserve ((size_t) snippetLength);

                for (int i = 0; i < totalSamples; ++i)
                {
                    const float sample = 0.3f * (float) std::sin (phase);
                    phase += phaseIncrement;
                    buffer.setSample (0, 0, sample);
                    buffer.setSample (1, 0, sample);

                    juce::dsp::AudioBlock<float> block (buffer);
                    engine.process (block);

                    if (i >= totalSamples - snippetLength)
                        snippet.push_back (engine.peekDryFeedback());
                }

                return snippet;
            };

            const auto highSnippet = captureSnippet (1.0f);
            const auto lowSnippet  = captureSnippet (0.15f);

            juce::String highStr, lowStr;
            for (float v : highSnippet) highStr << juce::String (v, 4) << " ";
            for (float v : lowSnippet)  lowStr  << juce::String (v, 4) << " ";

            logMessage ("peekDryFeedback() last " + juce::String (snippetLength) + " samples, high Sustain (1.0): " + highStr);
            logMessage ("peekDryFeedback() last " + juce::String (snippetLength) + " samples, low Sustain (0.15): " + lowStr);
        }

        beginTest ("DIAGNOSTIC: do primary/quadrature grain pools settle on the SAME alignment offset, high vs. low Sustain?");
        {
            // Glitch investigation, third pass. This codebase's own
            // extensively-documented "pitch oscillation" investigation
            // (see PitchShifter::findAlignmentOffset()'s class-level comment
            // and Engram topic millenia/pitch-oscillation-investigation)
            // already established that on PERIODIC content, normalized
            // cross-correlation stays close to 1.0 for almost ANY
            // phase-shifted copy of the signal against itself -- so a
            // structurally different alignment offset can score just as well
            // as the "right" one. The primary and quadrature pools search
            // INDEPENDENTLY (separate calls, staggered launch timing, see
            // PitchShifter.h's grain-pool-layout comment) -- if the dry
            // feedback signal is periodic/tied enough, each pool could settle
            // on a DIFFERENT, comparably-scoring offset, which would directly
            // explain a large primary-vs-quadrature OUTPUT divergence (this
            // test's own earlier diagnostic measured peak|wetLeft-wetRight|
            // = 2.0 at high Sustain vs. 0.3 at low) without needing either
            // pool's own boundedness to be violated at all -- two
            // individually well-behaved signals reading two different,
            // both-plausible delay offsets of the SAME periodic content can
            // still differ by up to the signal's own full peak-to-peak
            // range. Uses the new peekShifterPrimaryOffset()/
            // peekShifterQuadratureOffset() test-only accessors, driven by
            // the real feedback loop (not a synthetic standalone tone, since
            // the earlier bare-PitchShifter test did NOT reproduce this).
            constexpr double sampleRate = 44100.0;
            constexpr int numChannels = 2;
            constexpr float toneFrequencyHz = 220.0f;

            struct OffsetStats { float maxAbsDiff; float meanAbsDiff; int numSamplesOverThreshold; };

            auto measureOffsetDivergence = [&] (float shimmerSustainToUse) -> OffsetStats
            {
                ShimmerReverbEngine engine;
                juce::dsp::ProcessSpec spec { sampleRate, 1, (juce::uint32) numChannels };
                engine.prepare (spec);
                engine.reset();
                engine.setShimmerAmount (1.0f);
                engine.setShimmerSustain (shimmerSustainToUse);
                engine.setWidth (1.0f);

                juce::AudioBuffer<float> buffer (numChannels, 1);

                constexpr double totalSeconds = 6.0;
                const int totalSamples = (int) (totalSeconds * sampleRate);
                double phase = 0.0;
                const double phaseIncrement = 2.0 * juce::MathConstants<double>::pi * toneFrequencyHz / sampleRate;

                // Only measure over the LAST second -- same "let the tank
                // reach its own steady-state resonance first" reasoning as
                // the peak/snippet diagnostics above; the first few seconds
                // are still filling the loop, not representative of the
                // sustained-drone condition the by-ear report described.
                const int measureFromSample = totalSamples - (int) sampleRate;

                double sumAbsDiff = 0.0;
                int measuredCount = 0;
                float maxAbsDiff = 0.0f;
                int numOverThreshold = 0;
                constexpr float divergenceThresholdSamples = 10.0f; // well above ordinary continuous-drift jitter

                for (int i = 0; i < totalSamples; ++i)
                {
                    const float sample = 0.3f * (float) std::sin (phase);
                    phase += phaseIncrement;
                    buffer.setSample (0, 0, sample);
                    buffer.setSample (1, 0, sample);

                    juce::dsp::AudioBlock<float> block (buffer);
                    engine.process (block);

                    if (i >= measureFromSample)
                    {
                        const float primaryOffset = engine.peekShifterPrimaryOffset();
                        const float quadratureOffset = engine.peekShifterQuadratureOffset();
                        const float absDiff = std::abs (primaryOffset - quadratureOffset);

                        sumAbsDiff += absDiff;
                        maxAbsDiff = juce::jmax (maxAbsDiff, absDiff);
                        if (absDiff > divergenceThresholdSamples)
                            ++numOverThreshold;
                        ++measuredCount;
                    }
                }

                return { maxAbsDiff, (float) (sumAbsDiff / juce::jmax (1, measuredCount)), numOverThreshold };
            };

            const auto highStats = measureOffsetDivergence (1.0f);
            const auto lowStats  = measureOffsetDivergence (0.15f);

            logMessage ("Primary vs. quadrature alignment offset divergence over the final 1s (44100 samples): "
                            + juce::String ("high Sustain (1.0): maxAbsDiff=") + juce::String (highStats.maxAbsDiff, 2)
                            + ", meanAbsDiff=" + juce::String (highStats.meanAbsDiff, 2)
                            + ", samples with diff>10=" + juce::String (highStats.numSamplesOverThreshold)
                            + " -- low Sustain (0.15): maxAbsDiff=" + juce::String (lowStats.maxAbsDiff, 2)
                            + ", meanAbsDiff=" + juce::String (lowStats.meanAbsDiff, 2)
                            + ", samples with diff>10=" + juce::String (lowStats.numSamplesOverThreshold));
        }

        beginTest ("DIAGNOSTIC: sweep candidate maxShimmerBlendWeight floors -- CONCLUDED: no clean win, floor idea not pursued");
        {
            // Glitch investigation, fifth pass, CONCLUDED 2026-09-06. The
            // safetyLimiter fix (see process()'s sideShift comment) bounds
            // SEVERITY but not OCCURRENCE (user by-ear report: still sounds
            // glitchy after that fix; the saturation-fraction diagnostic
            // confirmed the large excursions are intermittent, not constant
            // or click-like). Candidate considered: never let
            // maxShimmerBlendWeight reach literal 0.0f at Sustain=1.0 --
            // floor it, so the tank's own dry recirculation always keeps SOME
            // shimmer-cascade content blended in, which is what kept the
            // alignment-search ambiguity mostly latent before this parameter
            // existed.
            //
            // RESULT: swept 0.00/0.02/0.05/0.10/0.15/0.20/0.30 (via
            // setShimmerSustain(1.0f - floor), which already reaches
            // maxShimmerBlendWeight=floor exactly -- no source change needed
            // to test this). meanOffsetDiff came back NOISY and NON-
            // MONOTONIC across that whole range (178/120/181/125/85/118/86
            // samples) -- matching this codebase's own already-documented
            // finding for the analogous frozen-cap sweep ("the weight/decay
            // relationship is NOT monotonic"). Even a floor as large as 0.30
            // (which would blunt most of the actual "long sustain" character
            // the dial exists to provide) only reached ~86 samples average
            // divergence, barely better than 0.15's 85 -- no floor in this
            // practical range gives a clean, reliable win. NOT implemented in
            // production code as a result -- `setShimmerSustain()` still maps
            // to the unfloored `1.0f - newAmount`, unchanged from before this
            // sweep. Decision (user, 2026-09-06): keep the safetyLimiter fix
            // (a real, verified severity reduction) as the stopping point;
            // document the residual intermittent stereo-image artifact at
            // extreme Amount+Sustain+Width settings as a known, open issue
            // tied to the pre-existing "pitch oscillation" ambiguity, rather
            // than continue chasing a fix in the same territory that already
            // took this codebase multiple prior sessions with no full
            // resolution. This diagnostic is KEPT (not deleted) so a future
            // session doesn't re-attempt the same floor idea without knowing
            // it was already measured and found wanting.
            //
            // `ShimmerReverbEngine::setShimmerSustain(x)` maps to
            // `maxShimmerBlendWeight = 1.0f - x`, so testing candidate FLOOR
            // values here doesn't need any source change -- setShimmerSustain
            // (1.0f - floor) reaches exactly maxShimmerBlendWeight=floor,
            // letting this sweep answer "how big a floor is actually needed"
            // with real measurements before
            // touching any production code or picking a number by feel.
            constexpr double sampleRate = 44100.0;
            constexpr int numChannels = 2;
            constexpr float toneFrequencyHz = 220.0f;

            struct SweepResult { float floorWeight; float meanOffsetDiff; float peakLR; };

            auto measureAtFloor = [&] (float floorWeight) -> SweepResult
            {
                ShimmerReverbEngine engine;
                juce::dsp::ProcessSpec spec { sampleRate, 1, (juce::uint32) numChannels };
                engine.prepare (spec);
                engine.reset();
                engine.setShimmerAmount (1.0f);
                engine.setShimmerSustain (1.0f - floorWeight); // reaches maxShimmerBlendWeight == floorWeight exactly
                engine.setWidth (1.0f);

                juce::AudioBuffer<float> buffer (numChannels, 1);

                constexpr double totalSeconds = 6.0;
                const int totalSamples = (int) (totalSeconds * sampleRate);
                double phase = 0.0;
                const double phaseIncrement = 2.0 * juce::MathConstants<double>::pi * toneFrequencyHz / sampleRate;
                const int measureFromSample = totalSamples - (int) sampleRate;

                double sumAbsDiff = 0.0;
                int measuredCount = 0;
                float peakLR = 0.0f;

                for (int i = 0; i < totalSamples; ++i)
                {
                    const float sample = 0.3f * (float) std::sin (phase);
                    phase += phaseIncrement;
                    buffer.setSample (0, 0, sample);
                    buffer.setSample (1, 0, sample);

                    juce::dsp::AudioBlock<float> block (buffer);
                    engine.process (block);

                    if (i >= measureFromSample)
                    {
                        const float absDiff = std::abs (engine.peekShifterPrimaryOffset() - engine.peekShifterQuadratureOffset());
                        sumAbsDiff += absDiff;
                        ++measuredCount;

                        const float left  = buffer.getSample (0, 0);
                        const float right = buffer.getSample (1, 0);
                        peakLR = juce::jmax (peakLR, std::abs (left - right));
                    }
                }

                return { floorWeight, (float) (sumAbsDiff / juce::jmax (1, measuredCount)), peakLR };
            };

            juce::String summary;
            for (float floorCandidate : { 0.0f, 0.02f, 0.05f, 0.10f, 0.15f, 0.20f, 0.30f })
            {
                const auto result = measureAtFloor (floorCandidate);
                summary << "floor=" << juce::String (result.floorWeight, 2)
                        << " -> meanOffsetDiff=" << juce::String (result.meanOffsetDiff, 2)
                        << ", peakLR=" << juce::String (result.peakLR, 3) << "  |  ";
            }

            logMessage ("maxShimmerBlendWeight floor sweep (Shimmer Amount=1, Width=1, 220Hz tone, final 1s of 6s): " + summary);
        }

        beginTest ("DIAGNOSTIC: sample-to-sample discontinuity ('click') size in wetLeft, post-limiter-fix, high vs. low Sustain");
        {
            // Glitch investigation, fourth pass -- user reports it STILL
            // sounds glitchy after the safetyLimiter fix on sideShift. That
            // fix bounds sideShift's PEAK amplitude (confirmed: 2.0 -> 1.0),
            // but does nothing about the RATE/shape of how it gets there --
            // the primary/quadrature offset divergence itself is unchanged
            // (still ~178 samples average, ~100% of samples over threshold
            // at high Sustain, per the diagnostic above). If that divergence
            // is jumping around erratically hop-to-hop rather than sitting at
            // a large but STABLE offset, sideShift (and therefore wetLeft/
            // wetRight) would show large sample-to-sample discontinuities --
            // audible as clicking/crackling/warble regardless of how loud the
            // limiter allows any single sample to get. Same "interior vs.
            // transition delta" methodology this codebase already used to
            // characterize the FormantEnvelopeCorrector hop-boundary click
            // and the LoopCapture engage/disengage click (both real fixes,
            // not guesses) -- measures the actual per-sample |delta| of
            // wetLeft across the whole tail, not just its peak level.
            constexpr double sampleRate = 44100.0;
            constexpr int numChannels = 2;
            constexpr float toneFrequencyHz = 220.0f;

            struct ClickStats { float maxDelta; float meanDelta; float p99Delta; float fractionSaturated; };

            auto measureClickStats = [&] (float shimmerSustainToUse) -> ClickStats
            {
                ShimmerReverbEngine engine;
                juce::dsp::ProcessSpec spec { sampleRate, 1, (juce::uint32) numChannels };
                engine.prepare (spec);
                engine.reset();
                engine.setShimmerAmount (1.0f);
                engine.setShimmerSustain (shimmerSustainToUse);
                engine.setWidth (1.0f);

                juce::AudioBuffer<float> buffer (numChannels, 1);

                constexpr double totalSeconds = 6.0;
                const int totalSamples = (int) (totalSeconds * sampleRate);
                double phase = 0.0;
                const double phaseIncrement = 2.0 * juce::MathConstants<double>::pi * toneFrequencyHz / sampleRate;

                const int measureFromSample = totalSamples - (int) sampleRate; // final 1s only, steady-state
                float previousLeft = 0.0f;
                bool havePrevious = false;
                std::vector<float> deltas;
                deltas.reserve ((size_t) sampleRate);
                int measuredCount = 0;
                int saturatedCount = 0;

                for (int i = 0; i < totalSamples; ++i)
                {
                    const float sample = 0.3f * (float) std::sin (phase);
                    phase += phaseIncrement;
                    buffer.setSample (0, 0, sample);
                    buffer.setSample (1, 0, sample);

                    juce::dsp::AudioBlock<float> block (buffer);
                    engine.process (block);

                    const float left  = buffer.getSample (0, 0);
                    const float right = buffer.getSample (1, 0);

                    if (i >= measureFromSample)
                    {
                        if (havePrevious)
                            deltas.push_back (std::abs (left - previousLeft));

                        previousLeft = left;
                        havePrevious = true;

                        // |left-right| here IS the post-limiter sideShift
                        // contribution directly (gate=1.0 at Width=Amount=
                        // Sustain=1.0, so wetLeft-wetRight == sideShift
                        // exactly) -- 0.7 is well into safetyLimiter's
                        // soft-knee compression region (threshold=0.8f,
                        // asymptote 1.0f), so this measures how much of the
                        // time the limiter is actively, heavily distorting
                        // this signal, not just occasionally catching a peak.
                        if (std::abs (left - right) > 0.7f)
                            ++saturatedCount;

                        ++measuredCount;
                    }
                }

                std::sort (deltas.begin(), deltas.end());
                const float maxDelta = deltas.empty() ? 0.0f : deltas.back();
                double sum = 0.0;
                for (float d : deltas) sum += d;
                const float meanDelta = deltas.empty() ? 0.0f : (float) (sum / (double) deltas.size());
                const float p99Delta = deltas.empty() ? 0.0f : deltas[(size_t) (0.99 * (double) (deltas.size() - 1))];
                const float fractionSaturated = measuredCount > 0 ? (float) saturatedCount / (float) measuredCount : 0.0f;

                return { maxDelta, meanDelta, p99Delta, fractionSaturated };
            };

            const auto highClick = measureClickStats (1.0f);
            const auto lowClick  = measureClickStats (0.15f);

            logMessage ("wetLeft sample-to-sample |delta| over final 1s: high Sustain (1.0): max="
                            + juce::String (highClick.maxDelta, 4) + ", mean=" + juce::String (highClick.meanDelta, 6)
                            + ", p99=" + juce::String (highClick.p99Delta, 4)
                            + ", fraction |L-R|>0.7=" + juce::String (highClick.fractionSaturated * 100.0f, 1) + "%"
                            + " -- low Sustain (0.15): max=" + juce::String (lowClick.maxDelta, 4)
                            + ", mean=" + juce::String (lowClick.meanDelta, 6) + ", p99=" + juce::String (lowClick.p99Delta, 4)
                            + ", fraction |L-R|>0.7=" + juce::String (lowClick.fractionSaturated * 100.0f, 1) + "%");
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
            // keep it bounded at near-unity decay. Landed on
            // frozenMaxShimmerBlendWeight=0.01f (see DattorroTank.h) as the
            // most aggressive value verified safe over a 10-minute run --
            // brings this test's 15s decay down to -1.94dB. Further digging
            // (tap-delay length, grain-vs-fixed-tap, a finer weight sweep)
            // found no further improvement without reintroducing instability
            // -- see DattorroTank.h's comment for that full trail.
            //
            // 2026-08-22, same day, third pass: added FreezeLeveler (see
            // that class) as an output-stage, feed-forward-only auto-leveler
            // compensating the REMAINING decay, at the user's suggestion --
            // brings this test's 15s decay down further, to ~-0.5dB.
            // Deliberately NOT unlimited: FreezeLeveler clamps its boost to
            // +12dB, so compensation degrades on longer holds (measured
            // ~-3.7dB at 60s in a one-off extended run of this same test,
            // not committed) rather than amplifying noise floor indefinitely
            // -- a real, accepted limitation, not a bug.
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

        beginTest ("DIAGNOSTIC: sustained-input RMS trend at default (non-frozen) settings -- investigating a by-ear 'volume keeps rising' report");
        {
            // By-ear complaint (2026-08-23, after wiring FormantEnvelopeCorrector
            // in place of SpectralTiltCompensator): "sounds tuned, no chipmunk
            // on the high note but highly distorted, with some glitches and a
            // lo-fi touch. Also its volume seems to raise and raise, like if it
            // was constantly amplified." The existing "Sustained noise stays
            // finite and bounded for at least 30 seconds" test above only
            // asserts peak <= 10.0f -- far too loose to catch a perceptually
            // obvious "getting louder over time" trend (e.g. climbing from
            // ~0.3 to ~3.0 would still pass that bound without ever tripping
            // it). This measures the actual RMS trend over time instead of
            // just boundedness, same discipline as the Freeze-tail diagnostic
            // above -- at DEFAULT (non-frozen) settings, which is what was
            // actually being listened to.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr int blocksPerSecond = (int) (sampleRate / blockSize);
            constexpr int totalSeconds = 30;

            ShimmerReverbEngine engine;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            engine.prepare (spec);
            engine.reset();
            // Defaults: pitchShift=12st, shimmerAmount=1.0, feedback=0.7 -- no
            // setFreezeAmount() call, this is the plain, most common use case.

            juce::AudioBuffer<float> buffer (numChannels, blockSize);
            juce::Random random (24681012);

            std::vector<float> perSecondRms ((size_t) totalSeconds, 0.0f);

            for (int sec = 0; sec < totalSeconds; ++sec)
            {
                double sumSquares = 0.0;
                juce::int64 numSamplesThisSecond = 0;

                for (int b = 0; b < blocksPerSecond; ++b)
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
                            expect (std::isfinite (data[i]), "Non-finite sample during sustained-input RMS trend measurement");
                            sumSquares += (double) data[i] * (double) data[i];
                            ++numSamplesThisSecond;
                        }
                    }
                }

                perSecondRms[(size_t) sec] = (float) std::sqrt (sumSquares / (double) numSamplesThisSecond);
            }

            juce::String trend;
            for (int sec = 0; sec < totalSeconds; sec += 5)
                trend += juce::String (perSecondRms[(size_t) sec], 4) + (sec + 5 < totalSeconds ? ", " : "");
            logMessage ("Sustained-input RMS at 5s intervals (default settings, input amplitude +-0.3): " + trend);

            const float firstSecondRms = perSecondRms[0];
            const float lastSecondRms = perSecondRms[(size_t) (totalSeconds - 1)];
            const float growthDb = 20.0f * std::log10 (juce::jmax (1.0e-9f, lastSecondRms) / juce::jmax (1.0e-9f, firstSecondRms));
            logMessage ("RMS growth over " + juce::String (totalSeconds) + "s: first-second=" + juce::String (firstSecondRms, 4)
                        + ", last-second=" + juce::String (lastSecondRms, 4) + " (" + juce::String (growthDb, 2) + " dB)");
        }

        // Phase 10 (see docs/shimmer-reverb-implementation-plan.md): Loop
        // Freeze -- a NEW, purely additive static-loop-capture feature,
        // fully independent of Phase 9's classic decay-pin Freeze above.
        // Same default-equivalence convention as the "Freeze defaults to
        // 0.0..." test above: an untouched engine and one with
        // setLoopFreezeAmount(0.0f) explicitly called must be bit-identical,
        // proving Loop Freeze is a genuine opt-in with zero behavior change
        // at its default (LoopCapture's own rolling-history buffer is
        // written every sample regardless, but never read back into the
        // output until a rising edge is seen -- see LoopCapture.h).
        beginTest ("Loop Freeze defaults to off and reproduces pre-existing behavior bit-for-bit");
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
            explicitEngine.setLoopFreezeAmount (0.0f);

            juce::AudioBuffer<float> defaultBuffer (numChannels, blockSize);
            juce::AudioBuffer<float> explicitBuffer (numChannels, blockSize);

            juce::Random random (10203040);

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
                        expectEquals (explicitData[i], defaultData[i], "setLoopFreezeAmount(0.0f) diverged from the "
                                                                            "untouched default at channel "
                                                                            + juce::String (ch) + ", block "
                                                                            + juce::String (b) + ", sample "
                                                                            + juce::String (i));
                    }
                }
            }
        }

        beginTest ("Loop Freeze alone captures and repeats a periodic loop");
        {
            // Runs at block size 1 so the exact global sample index a given
            // output value was produced at can be tracked precisely -- needed
            // to independently re-derive LoopCapture's own read-position
            // sequence (N = loop length in samples, X = active crossfade
            // samples, period = N - X, see LoopCapture.h's class comment)
            // and know exactly which sample pairs must match.
            constexpr double sampleRate = 44100.0;
            constexpr int numChannels = 2;

            ShimmerReverbEngine engine;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 1, (juce::uint32) numChannels };
            engine.prepare (spec);
            engine.reset();

            // Short loop length (well inside [minLoopLengthMs, maxLoopLengthMs])
            // so this test runs quickly -- only takes effect at the NEXT
            // rising edge (see LoopCapture::setLoopLengthMs()'s comment), so
            // setting it here, before any capture, is safe.
            constexpr float loopLengthMs = 100.0f;
            engine.setLoopLengthMs (loopLengthMs);

            // Independent re-derivation of LoopCapture's own N/X/period math
            // (LoopCapture.cpp's prepare()/captureLoop()), not a re-use of
            // its internals -- if this drifts from LoopCapture's own
            // formula, this test would need updating alongside it.
            const int N = juce::roundToInt (loopLengthMs * 0.001 * sampleRate);
            const int crossfadeSamples = juce::jmax (0, juce::roundToInt (25.0f * 0.001f * (float) sampleRate));
            const int X = juce::jmin (crossfadeSamples, N / 2);
            const int period = N - X;

            juce::AudioBuffer<float> buffer (numChannels, 1);
            juce::Random random (11223344);

            // Phase A: build up real, non-silent wet content (Loop Freeze off)
            // so the capture below pulls a genuine, non-trivial loop out of
            // the rolling history buffer, not silence.
            const int burstSamples = N + 1000;
            for (int i = 0; i < burstSamples; ++i)
            {
                buffer.setSample (0, 0, random.nextFloat() * 0.6f - 0.3f);
                buffer.setSample (1, 0, random.nextFloat() * 0.6f - 0.3f);
                juce::dsp::AudioBlock<float> block (buffer);
                engine.process (block);
            }

            // Phase B: engage Loop Freeze -- the rising edge is seen on the
            // very next process() call, capturing the most recent N samples
            // of wet output into the loop.
            engine.setLoopFreezeAmount (1.0f);

            // Keep feeding FRESH, different noise for several loop periods --
            // must be entirely ignored (loopFreezeAmount clamps to exactly
            // 1.0f, so the live signal's weight in the blend is exactly 0.0f)
            // if the loop is genuinely static rather than still leaking live
            // input through.
            constexpr int numPeriodsToRun = 5;
            const int totalSamplesAfterCapture = N + numPeriodsToRun * period;

            std::vector<float> outLeft ((size_t) totalSamplesAfterCapture);
            std::vector<float> outRight ((size_t) totalSamplesAfterCapture);

            float maxPeak = 0.0f;

            for (int i = 0; i < totalSamplesAfterCapture; ++i)
            {
                buffer.setSample (0, 0, random.nextFloat() * 0.6f - 0.3f);
                buffer.setSample (1, 0, random.nextFloat() * 0.6f - 0.3f);
                juce::dsp::AudioBlock<float> block (buffer);
                engine.process (block);

                const float left = buffer.getSample (0, 0);
                const float right = buffer.getSample (1, 0);

                expect (std::isfinite (left), "Left channel is not finite at sample " + juce::String (i) + " (Loop Freeze engaged)");
                expect (std::isfinite (right), "Right channel is not finite at sample " + juce::String (i) + " (Loop Freeze engaged)");
                maxPeak = juce::jmax (maxPeak, juce::jmax (std::abs (left), std::abs (right)));

                outLeft[(size_t) i] = left;
                outRight[(size_t) i] = right;
            }

            expect (maxPeak <= 10.0f, "Loop Freeze output exceeded the safety bound of 10.0 (peak: " + juce::String (maxPeak) + ")");

            // Exact periodicity check: once past the first full pass (global
            // index >= N), the read position only ever cycles through
            // [X, N) -- an exact period of (N - X) samples, bit-identical
            // every repeat (see LoopCapture.h's class comment). Compared
            // outside the seam's crossfade window (the final X samples of
            // each period) per this phase's own test-writing brief, though
            // the design's own contract is that even the blended region
            // repeats identically.
            for (int p = 0; p < numPeriodsToRun - 1; ++p)
            {
                const int base = N + p * period;

                for (int i = 0; i < period - X; ++i)
                {
                    expectEquals (outLeft[(size_t) (base + i)], outLeft[(size_t) (base + i + period)],
                                  "Left channel not exactly periodic at period " + juce::String (period)
                                      + " samples, period index " + juce::String (p) + ", offset " + juce::String (i));
                    expectEquals (outRight[(size_t) (base + i)], outRight[(size_t) (base + i + period)],
                                  "Right channel not exactly periodic at period " + juce::String (period)
                                      + " samples, period index " + juce::String (p) + ", offset " + juce::String (i));
                }
            }
        }

        beginTest ("Freeze and Loop Freeze together stay finite and produce a genuine layered result");
        {
            // Interaction case, previously impossible: Phase 9's classic
            // Freeze (decay-pin drone sustain) and Phase 10's Loop Freeze
            // (static loop capture) engaged TOGETHER on the same engine
            // instance. Loop Freeze is wired in strictly after
            // freezeLeveler's gain application (see ShimmerReverbEngine::
            // process()), so its "live" input is whatever Phase 9's own
            // mechanism currently outputs -- this test confirms that
            // layering actually works (bounded/finite, genuinely non-silent,
            // i.e. the loop is repeating the drone's real current output,
            // not accidentally silencing it), not just that neither feature
            // alone regresses.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr int blocksPerSecond = (int) (sampleRate / blockSize);

            ShimmerReverbEngine engine;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            engine.prepare (spec);
            engine.reset();

            juce::AudioBuffer<float> buffer (numChannels, blockSize);
            juce::Random random (99887766);

            // Burst 2s of noise (both features off) so both the tank's own
            // recirculation and LoopCapture's rolling-history buffer are
            // filled with real content before either feature engages.
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

            // Engage classic Freeze first (drone sustain), then Loop Freeze
            // (captures a snapshot of whatever the drone currently outputs,
            // per the wiring point's own design intent).
            engine.setFreezeAmount (1.0f);
            engine.setLoopFreezeAmount (1.0f);

            constexpr int tailSeconds = 5;
            constexpr int tailBlocks = tailSeconds * blocksPerSecond;

            float maxPeak = 0.0f;
            double sumSquares = 0.0;
            juce::int64 numSamplesTotal = 0;

            for (int b = 0; b < tailBlocks; ++b)
            {
                // Keep feeding fresh noise -- Freeze mutes it from reaching
                // the tank's diffuser, and Loop Freeze's blend excludes it
                // entirely at loopFreezeAmount=1.0, so it must have zero
                // effect on the output either way.
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
                                                              + ", sample " + juce::String (i) + " (Freeze + Loop Freeze engaged)");
                        maxPeak = juce::jmax (maxPeak, std::abs (data[i]));
                        sumSquares += (double) data[i] * (double) data[i];
                        ++numSamplesTotal;
                    }
                }

                expect (maxPeak <= 10.0f, "Freeze + Loop Freeze together exceeded the safety bound of 10.0 at block "
                                              + juce::String (b) + " (peak so far: " + juce::String (maxPeak) + ")");
            }

            const float overallRms = (float) std::sqrt (sumSquares / (double) numSamplesTotal);
            expect (overallRms > 1.0e-4f, "Freeze + Loop Freeze together produced effective silence (RMS: "
                                              + juce::String (overallRms, 8) + ") -- the loop should be repeating "
                                              "the drone's real current output, not silencing it");
        }
    }
};

static ShimmerReverbEngineTests shimmerReverbEngineTests;
