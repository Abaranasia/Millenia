#include <JuceHeader.h>
#include "../../Source/DSP/ShimmerReverbEngine.h"

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
            // decayGain=0.6f recirculation loop rather than a separate,
            // weaker external path. If this loop turns out to sustain
            // itself indefinitely (self-oscillation) at the tank's default
            // decay, this test will fail and that is itself the important
            // finding -- not something to silently patch decayGain down
            // for without evidence, since 0.6f is inside the architecture
            // doc's documented-safe range for a plain (unshifted) loop.
            constexpr double sampleRate = 44100.0;
            constexpr int blockSize = 512;
            constexpr int numChannels = 2;

            constexpr double burstSeconds = 2.0;
            constexpr double silenceSeconds = 8.0;
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
    }
};

static ShimmerReverbEngineTests shimmerReverbEngineTests;
