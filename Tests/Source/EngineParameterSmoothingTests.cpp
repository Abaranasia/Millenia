#include <JuceHeader.h>
#include <cmath>
#include <cstring>
#include <vector>
#include "../../Source/EngineParameterSmoothing.h"

// Audit fix 6 (docs/project-audit-2026-11.md): the block-rate parameters must
// ramp in fixed 32-sample chunks, so the zipper step size does not depend on
// the host block size. The tests drive ShimmerReverbEngine through
// EngineParameterSmoothing exactly as PluginProcessor::processBlock() does
// (MilleniaAudioProcessor itself is not linked into this project).
namespace
{
    constexpr double testSampleRate = 48000.0;
    constexpr size_t maxBlock = 512;

    // Step lands on a multiple of 512, so for the host sizes 32/64/512 the
    // step happens at the same sample AND at a host block boundary.
    constexpr size_t stepAt = 512 * 94;      // ~1 s pre-roll: the wet tail has built up
    constexpr size_t totalSamples = 512 * 104; // 5120 samples after the step (50 ms ramp = 2400)
    constexpr size_t rampSamples = 2400;

    EngineParameterSmoothing::Targets baseTargets()
    {
        EngineParameterSmoothing::Targets t;
        t.pitchShift = 12.0f;
        t.decay = 7.0f;
        t.shimmerAmount = 0.5f;
        t.shimmerFeedback = 0.55f;
        t.damping = 0.0005f;
        t.width = 1.0f;
        t.mix = 1.0f;
        t.lowCut = 20.0f;
        t.highCut = 20000.0f;
        t.shimmerTone = 10000.0f;
        t.freeze = 0.0f;
        return t;
    }

    struct Rendered
    {
        std::vector<float> left, right;
    };

    // `before` until the first host block that starts at or after stepAt, then
    // `after`. Input: identical 220 Hz sine on both channels.
    Rendered render (size_t hostBlock,
                     const EngineParameterSmoothing::Targets& before,
                     const EngineParameterSmoothing::Targets& after)
    {
        ShimmerReverbEngine engine;
        engine.prepare ({ testSampleRate, (juce::uint32) maxBlock, 2 });
        EngineParameterSmoothing smoothing;
        smoothing.prepare (testSampleRate, before);

        Rendered out;
        out.left.reserve (totalSamples);
        out.right.reserve (totalSamples);

        juce::AudioBuffer<float> buffer (2, (int) hostBlock);
        size_t pos = 0;

        while (pos < totalSamples)
        {
            const auto n = juce::jmin (hostBlock, totalSamples - pos);

            for (size_t i = 0; i < n; ++i)
            {
                const float s = 0.5f * std::sin (juce::MathConstants<float>::twoPi * 220.0f
                                                 * (float) (pos + i) / (float) testSampleRate);
                buffer.setSample (0, (int) i, s);
                buffer.setSample (1, (int) i, s);
            }

            juce::dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers(), 2, n);
            smoothing.process (engine, pos >= stepAt ? after : before, block);

            for (size_t i = 0; i < n; ++i)
            {
                out.left.push_back (buffer.getSample (0, (int) i));
                out.right.push_back (buffer.getSample (1, (int) i));
            }
            pos += n;
        }
        return out;
    }

    // Max |y[n] - y[n-1]| of `signal` over the ramp window.
    float maxStep (const std::vector<float>& signal)
    {
        float m = 0.0f;
        for (size_t n = stepAt + 1; n < stepAt + rampSamples + 64; ++n)
            m = juce::jmax (m, std::abs (signal[n] - signal[n - 1]));
        return m;
    }

    std::vector<float> side (const Rendered& r)
    {
        std::vector<float> s (r.left.size());
        for (size_t i = 0; i < s.size(); ++i)
            s[i] = r.left[i] - r.right[i];
        return s;
    }

    double rmsDiff (const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to)
    {
        double acc = 0.0;
        for (size_t i = from; i < to; ++i)
        {
            const double d = (double) a[i] - (double) b[i];
            acc += d * d;
        }
        return std::sqrt (acc / (double) (to - from));
    }

    bool bitIdentical (const std::vector<float>& a, const std::vector<float>& b)
    {
        return a.size() == b.size() && std::memcmp (a.data(), b.data(), a.size() * sizeof (float)) == 0;
    }
}

class EngineParameterSmoothingTests : public juce::UnitTest
{
public:
    EngineParameterSmoothingTests() : juce::UnitTest ("EngineParameterSmoothingTests", "DSP") {}

    void runTest() override
    {
        // 5 % agreement on the largest sample-to-sample step, and a tiny RMS
        // distance to the 32-sample reference. After the fix 32/64/512 hosts
        // run identical chunks, so the real figures are ~0; the tolerances
        // only leave room for float noise.
        constexpr float stepTolerance = 0.05f;
        constexpr double rmsTolerance = 1.0e-4;

        beginTest ("Steady state output is bit-identical across host block sizes");
        {
            const auto t = baseTargets();
            const auto ref = render (32, t, t);
            for (size_t host : { (size_t) 64, (size_t) 512 })
            {
                const auto r = render (host, t, t);
                expect (bitIdentical (ref.left, r.left) && bitIdentical (ref.right, r.right),
                        "steady state differs at host block " + juce::String ((int) host));
            }
        }

        beginTest ("Mix 0 -> 1 step: ramp step size independent of host block size");
        {
            auto before = baseTargets();
            before.mix = 0.0f;
            const auto after = baseTargets();

            const auto ref = render (32, before, after);
            const auto r64 = render (64, before, after);
            const auto r512 = render (512, before, after);

            checkScenario ("mix", ref.left, r64.left, r512.left, stepTolerance, rmsTolerance);
        }

        beginTest ("Width 0 -> 1.5 step (L-R): ramp step size independent of host block size");
        {
            auto before = baseTargets();
            before.width = 0.0f;
            auto after = baseTargets();
            after.width = 1.5f;

            const auto ref = render (32, before, after);
            const auto r64 = render (64, before, after);
            const auto r512 = render (512, before, after);

            checkScenario ("width", side (ref), side (r64), side (r512), stepTolerance, rmsTolerance);
        }

        beginTest ("Host block not a multiple of the chunk size: ramp lands exactly on the target");
        {
            auto before = baseTargets();
            before.mix = 0.0f;
            before.width = 0.0f;
            auto after = baseTargets();
            after.width = 1.5f;

            const auto ref = render (32, before, after);
            // 100 = 32 + 32 + 32 + 4 samples per host block; its step lands up
            // to 99 samples later than the reference's, so compare only once
            // both ramps have finished.
            const auto r100 = render (100, before, after);

            const auto from = stepAt + rampSamples + 256;
            const double rmsL = rmsDiff (ref.left, r100.left, from, totalSamples);
            const double rmsR = rmsDiff (ref.right, r100.right, from, totalSamples);
            logMessage ("landing: tail RMS L " + juce::String (rmsL, 9) + " R " + juce::String (rmsR, 9));
            expectLessThan (rmsL, 1.0e-5);
            expectLessThan (rmsR, 1.0e-5);
        }
    }

private:
    void checkScenario (const juce::String& scenario,
                        const std::vector<float>& ref, const std::vector<float>& r64, const std::vector<float>& r512,
                        float stepTolerance, double rmsTolerance)
    {
        const float step32 = maxStep (ref);
        const float step64 = maxStep (r64);
        const float step512 = maxStep (r512);
        const double rms64 = rmsDiff (ref, r64, stepAt, totalSamples);
        const double rms512 = rmsDiff (ref, r512, stepAt, totalSamples);

        logMessage (scenario + ": max|dy| over ramp  32: " + juce::String (step32, 6)
                    + "  64: " + juce::String (step64, 6) + "  512: " + juce::String (step512, 6)
                    + " | ratio 512/64: " + juce::String (step512 / step64, 4));
        logMessage (scenario + ": RMS vs 32-ref  64: " + juce::String (rms64, 9) + "  512: " + juce::String (rms512, 9));

        expect (step64 > 0.0f, scenario + ": no signal in ramp window");
        expectLessThan (std::abs (step512 - step64) / step64, stepTolerance);
        expectLessThan (rms64, rmsTolerance);
        expectLessThan (rms512, rmsTolerance);
    }
};

static EngineParameterSmoothingTests engineParameterSmoothingTests;
