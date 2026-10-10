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

        //==============================================================================
        // Infinite mode through the full engine (see DattorroTankTests.cpp's
        // Infinite section for the bare-tank coverage). Same tight 2.0 bound
        // as the tank-level test: the existing 10.0 bound would hide slow
        // growth from unity decay plus continuous fresh input.
        // alsoFreeze: Infinite runs alone for prefillSeconds first, THEN Freeze
        // engages on top for durationSeconds. Engaging Freeze from t=0 would
        // mute every input sample (1 - freezeAmount = 0) and measure an
        // all-zero, trivially bounded output; pre-filling makes it a real
        // held drone at unity decay.
        auto runInfiniteEngineBoundedness = [this] (double durationSeconds, bool alsoFreeze, juce::int64 seed)
        {
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;
            constexpr double prefillSeconds = 5.0;
            const int prefillBlocks = alsoFreeze ? (int) (prefillSeconds * sampleRate / blockSize) : 0;
            const int numBlocks = prefillBlocks + (int) (durationSeconds * sampleRate / blockSize);

            ShimmerReverbEngine engine; // defaults: +12st, shimmer amount 1.0
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
            engine.prepare (spec);
            engine.reset();
            engine.setInfiniteAmount (1.0f);

            juce::AudioBuffer<float> buffer (numChannels, blockSize);
            juce::Random random (seed);

            float maxPeak = 0.0f;
            int numNonFinite = 0;
            int firstOverBoundBlock = -1;
            double lastSecondSumSquares = 0.0;
            int lastSecondCount = 0;
            const int lastSecondStart = numBlocks - (int) (sampleRate / blockSize);

            for (int b = 0; b < numBlocks; ++b)
            {
                if (alsoFreeze && b == prefillBlocks)
                    engine.setFreezeAmount (1.0f);

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
                        if (! std::isfinite (data[i]))
                        {
                            ++numNonFinite;
                        }
                        else
                        {
                            maxPeak = juce::jmax (maxPeak, std::abs (data[i]));

                            if (b >= lastSecondStart)
                            {
                                lastSecondSumSquares += (double) data[i] * (double) data[i];
                                ++lastSecondCount;
                            }
                        }
                    }
                }

                if (firstOverBoundBlock < 0 && maxPeak > 2.0f)
                    firstOverBoundBlock = b;
            }

            const float lastSecondRms = (float) std::sqrt (lastSecondSumSquares / (double) juce::jmax (1, lastSecondCount));

            logMessage (juce::String (alsoFreeze ? "Infinite + Freeze" : "Infinite") + " engine, "
                            + (alsoFreeze ? juce::String (prefillSeconds, 0) + "s Infinite-only prefill + " : juce::String())
                            + juce::String (durationSeconds, 0) + "s continuous noise: peak=" + juce::String (maxPeak, 4)
                            + ", last-second RMS=" + juce::String (lastSecondRms, 5)
                            + (firstOverBoundBlock >= 0 ? ", first exceeded 2.0 at "
                                   + juce::String (firstOverBoundBlock * blockSize / sampleRate, 1) + "s" : juce::String()));

            expectEquals (numNonFinite, 0, "Infinite engine produced non-finite samples");
            expect (maxPeak <= 2.0f, "Infinite engine exceeded the 2.0 bound (peak: " + juce::String (maxPeak, 4) + ")");
            expect (lastSecondRms > 1.0e-3f, "Infinite engine output went near-silent (last-second RMS "
                                                 + juce::String (lastSecondRms, 6) + ") -- bounded only trivially");
        };

        beginTest ("Infinite through the full engine stays finite and bounded (peak <= 2.0) over several minutes of sustained input");
        {
            runInfiniteEngineBoundedness (180.0, false, 55443322);
        }

        beginTest ("Infinite + Freeze both on stays finite and bounded (peak <= 2.0) over a minute of sustained input");
        {
            // Freeze's dry-input mute still applies when both are engaged
            // (ShimmerReverbEngine::process() scales the tank input by
            // 1 - freezeAmount regardless of Infinite), so this is a frozen
            // drone held at unity decay rather than an accumulating one --
            // the remaining risk is the decay pin itself (0.999 -> 1.0) plus
            // FreezeLeveler's make-up gain stacked on the level controller.
            runInfiniteEngineBoundedness (60.0, true, 66778899);
        }
    }
};

static ShimmerReverbEngineTests shimmerReverbEngineTests;

//==============================================================================
// Freeze toggle glide fix (2026-10-10, see docs/shimmer-reverb-implementation-
// plan.md's Freeze-toggle known issue and ShimmerReverbEngine::
// applyPitchAndFreezeCrossfade()). Kept as its own test class (same .cpp, so no
// Projucer resave) purely so it can be run in isolation via Main.cpp's
// name filter ("FreezeCrossfade") -- ShimmerReverbEngineTests above includes
// several multi-minute boundedness runs.
//
// Shared scenario: steady 440Hz sine into the default engine (+12st, Shimmer
// Amount 1.0), tail built for prefillSeconds with Freeze off, then Freeze
// ramped 0 -> 1 exactly the way PluginProcessor does it for the
// freezeQuickToggle checkbox: a juce::SmoothedValue<float> reset to 0.05s,
// advanced with skip(blockSize) once per block, its value forwarded through
// setFreezeAmount() before each process() call.
class ShimmerReverbEngineFreezeCrossfadeTests : public juce::UnitTest
{
public:
    ShimmerReverbEngineFreezeCrossfadeTests() : juce::UnitTest ("ShimmerReverbEngineFreezeCrossfadeTests", "DSP") {}

    static constexpr double sampleRate = 44100.0;
    // 128 -> ~17 ramp steps; T1 also runs 512 (a typical host buffer, ~4
    // steps), where per-block weight steps would click.
    static constexpr int defaultBlockSize = 128;
    static constexpr int numChannels = 2;
    static constexpr double prefillSeconds = 4.0;
    static constexpr double sineHz = 440.0;
    static constexpr float sineAmplitude = 0.3f;

    // Returns the mono-summed (0.5 * (L + R)) wet output for the whole run,
    // plus the raw L/R channels when leftOut/rightOut are given.
    // rampStartSample is set to the first sample of the block in which the
    // freeze ramp begins (or would have begun, when doRamp is false).
    static std::vector<float> runFreezeToggleScenario (bool doRamp, double postSeconds, int& rampStartSample,
                                                       std::vector<float>* leftOut = nullptr,
                                                       std::vector<float>* rightOut = nullptr,
                                                       int blockSize = defaultBlockSize,
                                                       bool forceUnityActive = false)
    {
        ShimmerReverbEngine engine;
        juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
        engine.prepare (spec);
        engine.reset();
        engine.forceUnityShifterActiveForTesting (forceUnityActive);

        juce::SmoothedValue<float> smoothedFreeze;
        smoothedFreeze.reset (sampleRate, 0.05); // matches PluginProcessor::prepareToPlay()
        smoothedFreeze.setCurrentAndTargetValue (0.0f);

        const int prefillBlocks = (int) (prefillSeconds * sampleRate / blockSize);
        const int numBlocks = prefillBlocks + (int) (postSeconds * sampleRate / blockSize);
        rampStartSample = prefillBlocks * blockSize;

        std::vector<float> wet ((size_t) (numBlocks * blockSize), 0.0f);
        if (leftOut != nullptr)
            leftOut->assign (wet.size(), 0.0f);
        if (rightOut != nullptr)
            rightOut->assign (wet.size(), 0.0f);
        juce::AudioBuffer<float> buffer (numChannels, blockSize);
        const double phaseInc = juce::MathConstants<double>::twoPi * sineHz / sampleRate;
        double phase = 0.0;

        for (int b = 0; b < numBlocks; ++b)
        {
            if (doRamp && b == prefillBlocks)
                smoothedFreeze.setTargetValue (1.0f);

            engine.setFreezeAmount (smoothedFreeze.skip (blockSize));

            for (int i = 0; i < blockSize; ++i)
            {
                const float s = sineAmplitude * (float) std::sin (phase);
                phase += phaseInc;
                for (int ch = 0; ch < numChannels; ++ch)
                    buffer.setSample (ch, i, s);
            }

            juce::dsp::AudioBlock<float> block (buffer);
            engine.process (block);

            for (int i = 0; i < blockSize; ++i)
            {
                const auto idx = (size_t) (b * blockSize + i);
                wet[idx] = 0.5f * (buffer.getSample (0, i) + buffer.getSample (1, i));
                if (leftOut != nullptr)
                    (*leftOut)[idx] = buffer.getSample (0, i);
                if (rightOut != nullptr)
                    (*rightOut)[idx] = buffer.getSample (1, i);
            }
        }

        return wet;
    }

    // Hann-windowed Goertzel power at `hz` over x[start, start + n).
    static double goertzelPower (const std::vector<float>& x, int start, int n, double hz)
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

        return s1 * s1 + s2 * s2 - coeff * s1 * s2;
    }

    // Summed Hann-windowed Goertzel power (dB, arbitrary but fixed reference)
    // at the INTERMEDIATE pitches a 12st -> 0st ratio glide sweeps every
    // tank partial through: +3/+6/+9 semitones above both the 440Hz
    // fundamental and its first shimmer octave (880Hz). A level crossfade
    // between a +12st and a 0st shifter never synthesizes these; a ratio
    // glide does. Only ever compared against the same measure on the
    // no-ramp baseline run, so the reference level cancels out.
    static float intermediateBandPowerDb (const std::vector<float>& x, int start, int n)
    {
        double bandPower = 0.0;
        for (double base : { sineHz, 2.0 * sineHz })
            for (int st : { 3, 6, 9 })
                bandPower += goertzelPower (x, start, n, base * std::pow (2.0, st / 12.0));

        return (float) (10.0 * std::log10 (juce::jmax (1.0e-18, bandPower)));
    }

    static float rmsDb (const std::vector<float>& x, int start, int n)
    {
        double sum = 0.0;
        for (int i = 0; i < n; ++i)
            sum += (double) x[(size_t) (start + i)] * (double) x[(size_t) (start + i)];
        return (float) (20.0 * std::log10 (juce::jmax (1.0e-12, std::sqrt (sum / n))));
    }

    // Ghost-audio scenario (2026-10-10, unity-shifter idle skip, see
    // ShimmerReverbEngine::unityShifterActive): tone A (440Hz) builds the
    // tail with Freeze off, Freeze is toggled on and held for
    // ghostHoldSeconds, toggled off again while the input switches to tone
    // B (660Hz), tone B plays for ghostToneBSeconds, then Freeze is toggled on
    // again -- all with the 0.05s toggle smoother at 512-sample blocks, like
    // PluginProcessor. Returns the raw, UNWEIGHTED unity-path output
    // (peekUnityShifterOutput()) for the first ghostWindowSeconds after the
    // re-engage. That window is rendered in 1-sample blocks (smoother
    // advanced per sample, same ramp) purely so the per-sample peek sees
    // every sample; the idle -> active decision is per block either way.
    static constexpr double ghostHoldSeconds = 0.5;
    static constexpr double ghostToneBSeconds = 4.0;
    static constexpr double ghostWindowSeconds = 0.15;
    static constexpr double toneBHz = 660.0;

    static std::vector<float> runUnityGhostScenario (bool forceUnityActive)
    {
        constexpr int blockSize = 512;
        ShimmerReverbEngine engine;
        juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
        engine.prepare (spec);
        engine.reset();
        engine.forceUnityShifterActiveForTesting (forceUnityActive);

        juce::SmoothedValue<float> smoothedFreeze;
        smoothedFreeze.reset (sampleRate, 0.05);
        smoothedFreeze.setCurrentAndTargetValue (0.0f);

        const int engageBlock = (int) (prefillSeconds * sampleRate / blockSize);
        const int disengageBlock = engageBlock + (int) (ghostHoldSeconds * sampleRate / blockSize);
        const int numBlocks = disengageBlock + (int) (ghostToneBSeconds * sampleRate / blockSize);

        juce::AudioBuffer<float> buffer (numChannels, blockSize);
        double phase = 0.0;
        auto nextInputSample = [&phase] (double hz)
        {
            const float s = sineAmplitude * (float) std::sin (phase);
            phase += juce::MathConstants<double>::twoPi * hz / sampleRate;
            return s;
        };

        for (int b = 0; b < numBlocks; ++b)
        {
            if (b == engageBlock)
                smoothedFreeze.setTargetValue (1.0f);
            if (b == disengageBlock)
                smoothedFreeze.setTargetValue (0.0f);

            engine.setFreezeAmount (smoothedFreeze.skip (blockSize));

            const double hz = b >= disengageBlock ? toneBHz : sineHz;
            for (int i = 0; i < blockSize; ++i)
            {
                const float s = nextInputSample (hz);
                for (int ch = 0; ch < numChannels; ++ch)
                    buffer.setSample (ch, i, s);
            }

            juce::dsp::AudioBlock<float> block (buffer);
            engine.process (block);
        }

        smoothedFreeze.setTargetValue (1.0f);
        std::vector<float> unityOut ((size_t) (ghostWindowSeconds * sampleRate), 0.0f);
        juce::AudioBuffer<float> one (numChannels, 1);

        for (auto& out : unityOut)
        {
            engine.setFreezeAmount (smoothedFreeze.getNextValue());
            const float s = nextInputSample (toneBHz);
            for (int ch = 0; ch < numChannels; ++ch)
                one.setSample (ch, 0, s);

            juce::dsp::AudioBlock<float> block (one);
            engine.process (block);
            out = engine.peekUnityShifterOutput();
        }

        return unityOut;
    }

    // Summed Hann-windowed Goertzel power (dB) over x[start, start + n) at a
    // tone's fundamental and its +12st shimmer octave.
    static float toneBandPowerDb (const std::vector<float>& x, int start, int n, double hz)
    {
        const double p = goertzelPower (x, start, n, hz) + goertzelPower (x, start, n, 2.0 * hz);
        return (float) (10.0 * std::log10 (juce::jmax (1.0e-18, p)));
    }

    // Wall-clock processing/audio ratio of a default engine at Freeze 0 over
    // ~3s of noise at 512-sample blocks.
    static double measureDefaultRealTimeFactor (bool forceUnityActive)
    {
        constexpr int blockSize = 512;
        ShimmerReverbEngine engine;
        juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, (juce::uint32) numChannels };
        engine.prepare (spec);
        engine.reset();
        engine.forceUnityShifterActiveForTesting (forceUnityActive);
        juce::AudioBuffer<float> buffer (numChannels, blockSize);
        juce::Random random (424242);
        constexpr int numBlocks = 258; // ~3s

        const double startMs = juce::Time::getMillisecondCounterHiRes();
        for (int b = 0; b < numBlocks; ++b)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                for (int i = 0; i < blockSize; ++i)
                    buffer.setSample (ch, i, random.nextFloat() * 0.6f - 0.3f);
            juce::dsp::AudioBlock<float> block (buffer);
            engine.process (block);
        }
        const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - startMs;
        return elapsedMs / (1000.0 * numBlocks * blockSize / sampleRate);
    }

    void runTest() override
    {
        beginTest ("T1: a Freeze toggle ramp does not glide the shimmer's pitch through intermediate intervals");
        {
            // Measured on the L and R channels separately (what is actually
            // heard), over short windows covering only the ramp and the
            // ~40ms after it. Why not the mono sum or the whole frozen tail:
            // a first version of this test measured the MID channel's
            // intermediate-pitch energy over 1.4s and could not tell the two
            // designs apart (excess +9.90dB old vs +9.19dB new) -- at freeze
            // DattorroTank cuts its shimmer blend to 0.01 (so the in-loop
            // glide barely reaches the mid) and the frozen tank's own
            // spectrum differs from the live one regardless of the shifter.
            // The glide's audible path is the feed-forward Width term
            // (sideShift = primary - quadrature shifter output, added to L
            // and subtracted from R), which carries the shifter's output
            // straight to the speakers, and it is concentrated in the ~5ms
            // where freezeAmount^16 moves (f = 0.9 -> 1.0 at the end of the
            // 50ms ramp). A timeline of the old code showed the side channel's
            // intermediate-pitch energy jumping ~40dB above its steady state
            // in exactly the windows spanning the ramp's end.
            //
            // Run at 128 AND 512-sample blocks: at 512 the first level-
            // crossfade version still measured +15.75dB, from the per-block
            // freezeAmount steps in the input mute and DattorroTank (a
            // zipper, not a glide) -- fixed by ramping those per sample, see
            // ShimmerReverbEngine::processedFreezeAmount.
            //
            // Not run at Shimmer Sustain 0 (shimmer path up to 100% of the
            // loop): that setting's no-ramp baseline already carries ~25dB
            // more intermediate-pitch energy from the cascade itself, so the
            // measure reads ~0dB excess for BOTH designs there -- blind, not
            // a pass.
            for (int blockSize : { defaultBlockSize, 512 })
            {
                int rampStart = 0;
                std::vector<float> rampedL, rampedR, baselineL, baselineR;
                runFreezeToggleScenario (true, 0.5, rampStart, &rampedL, &rampedR, blockSize);
                int baselineRampStart = 0;
                runFreezeToggleScenario (false, 0.5, baselineRampStart, &baselineL, &baselineR, blockSize);

                constexpr int windowSize = 2048; // ~46ms, ~21.5Hz bins: 3st steps are >= 83Hz apart here
                constexpr int hop = 256;
                const int firstStart = rampStart - windowSize / 2;
                const int lastStart = rampStart + windowSize / 2; // last window ends ~70ms after the ramp starts

                float worstExcessDb = -200.0f;
                float worstRampedDb = -200.0f;
                float worstBaselineDb = -200.0f;

                for (auto* pair : { &rampedL, &rampedR })
                {
                    const auto& ramped = *pair;
                    const auto& baseline = (pair == &rampedL) ? baselineL : baselineR;

                    float rampedWorst = -200.0f;
                    float baselineWorst = -200.0f;
                    for (int start = firstStart; start <= lastStart; start += hop)
                    {
                        rampedWorst = juce::jmax (rampedWorst, intermediateBandPowerDb (ramped, start, windowSize));
                        baselineWorst = juce::jmax (baselineWorst, intermediateBandPowerDb (baseline, start, windowSize));
                    }

                    if (rampedWorst - baselineWorst > worstExcessDb)
                    {
                        worstExcessDb = rampedWorst - baselineWorst;
                        worstRampedDb = rampedWorst;
                        worstBaselineDb = baselineWorst;
                    }
                }

                logMessage ("Block size " + juce::String (blockSize) + ": intermediate-pitch (+3/+6/+9st) energy, worst L/R window around the ramp: ramped="
                                + juce::String (worstRampedDb, 2) + " dB, no-ramp baseline=" + juce::String (worstBaselineDb, 2)
                                + " dB -> excess " + juce::String (worstExcessDb, 2) + " dB");

                // Measured 2026-10-10 (RED -> GREEN): old ratio-glide code
                // +16.76dB (128-sample blocks) / +20.97dB (512) -- FAILS;
                // level crossfade + per-sample freeze ramps +3.20dB / +4.65dB
                // -- passes. The residual few dB is ordinary amplitude-
                // modulation splatter from a 50ms level change, not a pitch
                // glide. 8dB sits ~3-5dB above the new figures and ~9-13dB
                // below the old ones.
                expect (worstExcessDb <= 8.0f, "Block size " + juce::String (blockSize) + ": Freeze ramp injected " + juce::String (worstExcessDb, 2)
                                                   + " dB of extra intermediate-pitch energy over the no-ramp baseline "
                                                   "-- the shimmer is gliding in pitch instead of crossfading in level");
            }
        }

        beginTest ("T2: wet level stays continuous (no dip) across a Freeze toggle ramp");
        {
            // Guards the linear crossfade against a level hole while the main
            // and unity paths trade places. Measured 2026-10-10, worst 10ms
            // window vs the 200ms pre-ramp level: old code -0.16dB (128) /
            // -0.21dB (512), new code -0.17dB / -0.22dB (the pre-ramp
            // windows themselves already swing -0.19 / -0.21dB). -3dB
            // threshold per the agreed spec.
            //
            // Honest limit: this is a regression guard, not a red test --
            // the old code never dipped here either. It also could NOT
            // detect a cold (not-yet-filled) unityShifter: a mutation that
            // skipped unityShifter.processSample() while its weight was below
            // 1e-3 produced the same numbers to 0.01dB, because by the time
            // unityWeight is significant (f > 0.9) DattorroTank has already
            // capped the whole shimmer path at ~0.01-0.02 of the loop
            // (frozenMaxShimmerBlendWeight) and unityShifter's own
            // primary/quadrature pair is identical (sideShift == 0). Running
            // it every sample was first kept as a cheap defensive choice; the
            // same day it became the idle skip (see ShimmerReverbEngine::
            // unityShifterActive), which makes a cold unity path the NORMAL
            // case on every engagement -- re-measured with it: -0.17dB (128)
            // / -0.22dB (512), unchanged to 0.01dB. Also not run at Shimmer Sustain
            // 0: there BOTH designs dip ~-12 to -14dB (vs a -6.5 to -7.3dB
            // pre-ramp swing), a pre-existing level transition from the
            // frozen tank collapsing a 100%-shimmer loop to its plain path,
            // unrelated to this crossfade.
            for (int blockSize : { defaultBlockSize, 512 })
            {
                int rampStart = 0;
                const auto ramped = runFreezeToggleScenario (true, 1.0, rampStart, nullptr, nullptr, blockSize);

                const int window = (int) (0.010 * sampleRate); // 10ms
                const int preLen = (int) (0.200 * sampleRate);
                const float preLevelDb = rmsDb (ramped, rampStart - preLen, preLen);

                float preMinDb = 200.0f;
                for (int start = rampStart - preLen; start + window <= rampStart; start += window)
                    preMinDb = juce::jmin (preMinDb, rmsDb (ramped, start, window) - preLevelDb);

                float minDb = 200.0f;
                const int end = rampStart + (int) (0.5 * sampleRate);
                for (int start = rampStart; start + window <= end; start += window)
                    minDb = juce::jmin (minDb, rmsDb (ramped, start, window) - preLevelDb);

                logMessage ("Block size " + juce::String (blockSize) + ": wet RMS across Freeze ramp (10ms windows, relative to 200ms pre-ramp level "
                                + juce::String (preLevelDb, 2) + " dBFS): pre-ramp min=" + juce::String (preMinDb, 2)
                                + " dB, ramp + 500ms min=" + juce::String (minDb, 2) + " dB");

                expect (minDb >= -3.0f, "Block size " + juce::String (blockSize) + ": wet level dipped " + juce::String (-minDb, 2)
                                             + " dB below the pre-ramp level during the Freeze toggle ramp");
            }
        }

        beginTest ("T3: at Freeze = 1.0 the held loop does not cascade upward in pitch");
        {
            // Anti-cascade guarantee: at freeze=1 nothing pitch-shifted may
            // keep re-entering the near-lossless loop, or every pass would
            // move more of the 440Hz drone up to 880Hz (and on up). No
            // existing test covered this -- the full-engine Freeze tests
            // only assert boundedness. Invariant used: with the input muted
            // and decay <= 1, NO partial of the held loop may GROW; only a
            // shifter still feeding the loop can create new 880Hz energy.
            //
            // Why 60s and the 880Hz band: DattorroTank already caps the
            // shimmer path at frozenMaxShimmerBlendWeight=0.01 of the loop
            // at full freeze, so even a cascading shifter only feeds ~1% per
            // pass -- the growth is slow. Measured 2026-10-10 (880Hz band
            // energy, seconds 58.6-59.4 minus 0.6-1.4 after the ramp):
            //   level crossfade (this code): -3.9dB  (decays -- passes)
            //   old ratio crossfade:         -2.1dB  (decays -- passes)
            //   mutation, unityWeight forced to 0 (main shifter stays at
            //   +12st in the frozen loop):   +2.8dB  (grows -- FAILS)
            // An earlier version of this test used a first-difference
            // brightness proxy over 10s; it could not see that mutation at
            // all (439.8 -> 440.0Hz either way: the held drone is ~84dB
            // 440Hz-dominated), so it was replaced.
            int rampStart = 0;
            constexpr double holdSeconds = 60.0;
            const auto held = runFreezeToggleScenario (true, holdSeconds, rampStart);

            constexpr int n = 8192;
            auto bandDb = [&] (int start, double hz)
            {
                double p = 0.0;
                for (int k = 0; k < 4; ++k) // ~0.74s: 4 x 8192 Hann windows
                    p += goertzelPower (held, start + k * n, n, hz);
                return (float) (10.0 * std::log10 (juce::jmax (1.0e-18, p)));
            };

            const int oneSecond = (int) sampleRate;
            const int earlyStart = rampStart + 1 * oneSecond - 2 * n;
            const int lateStart = rampStart + 59 * oneSecond - 2 * n;

            const float early880 = bandDb (earlyStart, 2.0 * sineHz);
            const float late880 = bandDb (lateStart, 2.0 * sineHz);
            const float early440 = bandDb (earlyStart, sineHz);
            const float late440 = bandDb (lateStart, sineHz);
            const float growthDb = late880 - early880;

            logMessage ("Freeze=1 held loop, 880Hz band: t=1s " + juce::String (early880, 2) + " dB, t=59s "
                            + juce::String (late880, 2) + " dB (growth " + juce::String (growthDb, 2)
                            + " dB); 440Hz band: " + juce::String (early440, 2) + " -> " + juce::String (late440, 2) + " dB");

            expect (growthDb <= 1.0f, "880Hz content of the frozen loop GREW by " + juce::String (growthDb, 2)
                                          + " dB over 58s with the input muted -- pitch-shifted content is still "
                                          "cascading upward through the loop at freeze=1");
        }

        beginTest ("default (Freeze 0) output is bit-identical to the pre-crossfade engine");
        {
            // Golden FNV-1a hash of the raw float bits of the default engine's
            // 4.5s sine output at freezeAmount=0 (runFreezeToggleScenario with
            // the toggle never engaged). 2026-10-10: be7b4e1487c0ce8b with BOTH
            // the old ratio-glide code and the new level crossfade -- the
            // evidence that the default sound is bit-identical at Freeze 0.
            // Promoted from a logged DIAGNOSTIC to an assertion the same day so
            // a regression in the default path fails the suite. Verified
            // identical in the Debug AND Release test builds (MSVC, VS2026,
            // x64), so it is asserted unconditionally, no per-config guard.
            // Update it ONLY deliberately: an intentional change to the
            // default-path DSP, or a compiler/toolchain change, can
            // legitimately move it -- confirm the new sound is intended (by
            // ear or by a targeted test) before replacing the value, and record
            // why here.
            //
            // 2026-10-10, unity-shifter idle skip: hashed twice -- the shipping
            // engine (unityShifter idle at Freeze 0) and the same engine with
            // forceUnityShifterActiveForTesting(true) (the always-on behavior
            // the golden value was recorded with). Both must match: the idle
            // path substitutes 0.0f for a unity output that was weighted by
            // exactly 0.0f anyway.
            constexpr juce::uint64 expectedDefaultHash = 0xbe7b4e1487c0ce8bull;

            for (bool forceUnityActive : { false, true })
            {
                int rampStart = 0;
                const auto wet = runFreezeToggleScenario (false, 0.5, rampStart, nullptr, nullptr, defaultBlockSize, forceUnityActive);
                juce::uint64 hash = 1469598103934665603ull;
                for (float v : wet)
                {
                    juce::uint32 bits;
                    std::memcpy (&bits, &v, sizeof (bits));
                    hash = (hash ^ bits) * 1099511628211ull;
                }
                const juce::String label (forceUnityActive ? "unity shifter forced active" : "unity shifter idle");
                logMessage ("Default-settings 4.5s sine output FNV-1a fingerprint (" + label + "): " + juce::String::toHexString ((juce::int64) hash));

                expect (hash == expectedDefaultHash,
                        "Default (Freeze 0) output fingerprint changed (" + label + "): got "
                            + juce::String::toHexString ((juce::int64) hash) + ", expected "
                            + juce::String::toHexString ((juce::int64) expectedDefaultHash)
                            + " -- the default-path DSP is no longer bit-identical");
            }
        }

        beginTest ("DIAGNOSTIC: engine real-time factor at default settings");
        {
            // 2026-10-10, Release, this dev machine: ~0.037 before the second
            // (unity) PitchShifter, ~0.052-0.058 after (~+40-55%) -- still far
            // under the ~0.15 budget juce-plugin-testing sets for this
            // reverb. Back to ~0.037 at Freeze 0 with the unity-shifter idle
            // skip (same day; 0.053 -> 0.037 in this test). Logged only:
            // wall-clock numbers are machine-dependent -- the relative
            // assertion is the next test.
            ShimmerReverbEngine engine;
            juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) 512, (juce::uint32) numChannels };
            engine.prepare (spec);
            engine.reset();
            juce::AudioBuffer<float> buffer (numChannels, 512);
            juce::Random random (424242);
            constexpr int numBlocks = 860; // ~10s

            const double startMs = juce::Time::getMillisecondCounterHiRes();
            for (int b = 0; b < numBlocks; ++b)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                    for (int i = 0; i < 512; ++i)
                        buffer.setSample (ch, i, random.nextFloat() * 0.6f - 0.3f);
                juce::dsp::AudioBlock<float> block (buffer);
                engine.process (block);
            }
            const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - startMs;
            const double audioMs = 1000.0 * numBlocks * 512 / sampleRate;
            logMessage ("Engine: " + juce::String (audioMs, 0) + "ms audio in " + juce::String (elapsedMs, 1)
                            + "ms -> processing/audio = " + juce::String (elapsedMs / audioMs, 3));
        }

        beginTest ("Freeze 0 idles the unity shifter: engine real-time factor drops measurably vs the always-on path");
        {
            // 2026-10-10, unity-shifter idle skip. Relative, same-build,
            // same-process comparison against forceUnityShifterActiveForTesting
            // (true) -- not an absolute wall-clock threshold, which would be
            // machine-dependent and flaky (juce-plugin-testing's rule).
            // Interleaved rounds, best (minimum) of each, so a scheduler
            // hiccup in one run cannot decide the result.
            // Measured 2026-10-10 (Release, this dev machine, best of 5):
            //   always-on code (before the skip existed): idle 0.0528 vs
            //   forced 0.0526 -> 1.006 -- FAILS (RED, as expected);
            //   idle skip: idle 0.0365-0.0369 vs forced 0.0528-0.0531 ->
            //   0.692-0.697 -- passes.
            // 0.9 leaves ~0.2 of headroom for timing noise while still
            // failing anything that keeps the unity shifter running.
            constexpr int rounds = 5;
            double idleBest = std::numeric_limits<double>::max();
            double forcedBest = std::numeric_limits<double>::max();
            for (int r = 0; r < rounds; ++r)
            {
                forcedBest = juce::jmin (forcedBest, measureDefaultRealTimeFactor (true));
                idleBest = juce::jmin (idleBest, measureDefaultRealTimeFactor (false));
            }
            const double ratio = idleBest / forcedBest;
            logMessage ("Freeze 0 real-time factor, best of " + juce::String (rounds) + ": unity idle " + juce::String (idleBest, 4)
                            + ", unity forced active " + juce::String (forcedBest, 4) + " -> idle/forced = " + juce::String (ratio, 3));

            expect (ratio <= 0.9, "Freeze 0 is not measurably cheaper than the always-on unity shifter (idle/forced = "
                                      + juce::String (ratio, 3) + ") -- unityShifter is still being processed while unused");
        }

        beginTest ("Re-engaging Freeze does not replay stale unity-shifter audio from the previous engagement (no ghost)");
        {
            // 2026-10-10, unity-shifter idle skip: the unity path's ~80ms
            // delay line is frozen in place while idle, so without
            // unityShifter.reset() on the idle -> active edge the first ~80ms
            // after a re-engage would read back tone A from the LAST
            // engagement, seconds after the input moved on to tone B.
            // Measured on the raw (unweighted) unity output, i.e. what that
            // path contributes at full unityWeight (a slow Freeze dial move,
            // or a future curve change, can reach it while the stale audio is
            // still playing). Reference: the same scenario with the unity
            // shifter forced active, whose tone-A content is only the tank's
            // own genuine residue after the tone-B stretch.
            //
            // A GUARD, not a red test: on the old always-on code it passed
            // trivially (excess 0.00dB -- idle and forced were the same
            // path). Measured 2026-10-10, first 70ms, tone-A excess over the
            // always-on reference:
            //   idle skip WITH reset (this code): -186.64dB (digital silence:
            //   the cold path has nothing to read yet) -- passes;
            //   naive idle skip, reset() disabled: +45.29dB -- FAILS (tone-A
            //   51.93dB vs 6.64dB, tone-B -18.76dB vs 48.27dB: the stale
            //   region is almost pure tone A).
            // Over the full 150ms: +5.55dB with reset vs +39.80dB naive; the
            // +5.55dB is the cold start's onset leakage, not tone A -- over
            // 80-150ms alone the reset path matches always-on to 0.01dB
            // (6.86 vs 6.86dB tone A, 48.86 vs 48.87dB tone B).
            const auto idle = runUnityGhostScenario (false);
            const auto forced = runUnityGhostScenario (true);

            auto report = [&] (const juce::String& label, int start, int n)
            {
                const float idleA = toneBandPowerDb (idle, start, n, sineHz);
                const float idleB = toneBandPowerDb (idle, start, n, toneBHz);
                const float forcedA = toneBandPowerDb (forced, start, n, sineHz);
                const float forcedB = toneBandPowerDb (forced, start, n, toneBHz);
                logMessage ("Unity output, " + label + " after re-engaging Freeze: tone-A (440+880Hz) " + juce::String (idleA, 2)
                                + " dB idle-skip vs " + juce::String (forcedA, 2) + " dB always-on (excess " + juce::String (idleA - forcedA, 2)
                                + " dB); tone-B (660+1320Hz) " + juce::String (idleB, 2) + " vs " + juce::String (forcedB, 2) + " dB");
                return idleA - forcedA;
            };

            // Asserted on the first 70ms only: the stale region (the 80ms
            // base delay at ratio 1.0, minus margin for the 0.4ms drift and
            // the alignment search). Beyond it the reset path is a cold start
            // whose abrupt onset of tone B leaks into the tone-A bins, which
            // is not ghost audio -- logged for the full window too.
            const float ghostExcessDb = report ("first 70ms", 0, (int) (0.070 * sampleRate));
            report ("first " + juce::String (ghostWindowSeconds * 1000.0, 0) + "ms", 0, (int) idle.size());
            const int afterStale = (int) (0.080 * sampleRate);
            report ("80-" + juce::String (ghostWindowSeconds * 1000.0, 0) + "ms", afterStale, (int) idle.size() - afterStale);

            expect (ghostExcessDb <= 3.0f, "Unity path carried " + juce::String (ghostExcessDb, 2)
                                               + " dB more tone-A energy than the always-on reference right after re-engaging "
                                               "Freeze -- stale audio from the previous engagement is being replayed");
        }
    }
};

static ShimmerReverbEngineFreezeCrossfadeTests shimmerReverbEngineFreezeCrossfadeTests;
