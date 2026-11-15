# DSP Stability Tests: Silent Input, Feedback, CPU Budget

> Adapted from the automated test specifications in
> [glittercowboy/plugin-freedom-system](https://github.com/glittercowboy/plugin-freedom-system)
> (`.claude/skills/plugin-testing/references/test-specifications.md`), rewritten against
> `juce::UnitTest` (per the Hard Rule in `SKILL.md` — no Catch2 dependency) and against
> our actual DSP classes (`Source/DSP/DattorroTank.h`, `PitchShifter.h`,
> `ShimmerReverbEngine.h`), which each expose `prepare(spec)` / `reset()` / `process(block)`.
> These are templates to adapt once each class's `.cpp` has a real implementation, not
> drop-in-ready tests for a stub.

These three checks aren't redundant with a plain impulse-response test — each one catches
a distinct, common failure mode in exactly the kind of DSP this plugin has:

| Test | Catches | Why it matters here |
|---|---|---|
| Silent input | Uncleared buffers, denormal build-up | `DattorroTank`'s delay lines and `PitchShifter`'s circular buffer carry state across blocks |
| Feedback/regeneration stability | Runaway gain, unstable coefficients | The tank's cross-feeding branches and any shimmer regen loop are exactly the topology that explodes when a coefficient is off by a little |
| CPU budget | Real-time factor regressions | Algorithmic reverb + pitch shifting is the higher end of the CPU budget table below — worth tracking as components are added |

## Silent input test

```cpp
class DattorroTankSilentInputTest : public juce::UnitTest
{
public:
    DattorroTankSilentInputTest() : juce::UnitTest ("DattorroTank: silent input", "DSP") {}

    void runTest() override
    {
        beginTest ("Output stays near noise floor for silent input");

        DattorroTank tank;
        juce::dsp::ProcessSpec spec { 44100.0, 512, 2 };
        tank.prepare (spec);
        tank.reset();

        juce::AudioBuffer<float> buffer (2, 512);
        buffer.clear();

        // Run several blocks — a tank/delay carries state, so one silent
        // block isn't enough to reveal buffer garbage or denormal creep.
        for (int i = 0; i < 20; ++i)
        {
            juce::dsp::AudioBlock<float> block (buffer);
            tank.process (block);
        }

        float peak = buffer.getMagnitude (0, buffer.getNumSamples());
        expect (peak < 0.0001f, "Silent input produced output peak " + juce::String (peak));
    }
};

static DattorroTankSilentInputTest dattorroTankSilentInputTest;
```

Repeat the same shape for `PitchShifter` and `ShimmerReverbEngine`. A regenerative reverb
tail is expected to decay slowly rather than go instantly silent — if the tank has real
decay time, assert the noise floor threshold only after enough blocks for the tail to
fall below it (compute from decay time × sample rate, don't guess a fixed block count).

## Feedback / regeneration stability test

```cpp
class ShimmerReverbEngineStabilityTest : public juce::UnitTest
{
public:
    ShimmerReverbEngineStabilityTest() : juce::UnitTest ("ShimmerReverbEngine: bounded output", "DSP") {}

    void runTest() override
    {
        beginTest ("Sustained input does not cause runaway output");

        ShimmerReverbEngine engine;
        const double sampleRate = 44100.0;
        juce::dsp::ProcessSpec spec { sampleRate, 512, 2 };
        engine.prepare (spec);
        engine.reset();

        juce::AudioBuffer<float> buffer (2, 512);
        float maxAbsSeen = 0.0f;

        // ~11 seconds of continuous signal — long enough for a slow feedback
        // divergence to show up, short enough to stay a fast unit test.
        for (int i = 0; i < 1000; ++i)
        {
            fillWithSineWave (buffer, 440.0f, (float) sampleRate);

            juce::dsp::AudioBlock<float> block (buffer);
            engine.process (block);

            maxAbsSeen = juce::jmax (maxAbsSeen, buffer.getMagnitude (0, buffer.getNumSamples()));

            if (maxAbsSeen > 100.0f)
            {
                expect (false, "Output exploding at iteration " + juce::String (i)
                                + ", peak " + juce::String (maxAbsSeen));
                return;
            }
        }

        expect (maxAbsSeen < 10.0f, "Sustained output peak " + juce::String (maxAbsSeen)
                                     + " exceeds safety margin");
    }

private:
    static void fillWithSineWave (juce::AudioBuffer<float>& buffer, float freqHz, float sampleRate)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                data[i] = std::sin (2.0f * juce::MathConstants<float>::pi * freqHz * (float) i / sampleRate);
        }
    }
};

static ShimmerReverbEngineStabilityTest shimmerReverbEngineStabilityTest;
```

If this fails, check in order: any regen/feedback coefficient that can exceed 1.0 at the
parameter's extreme setting, filter coefficients that go unstable (poles outside the unit
circle) at extreme cutoff/resonance, and any gain multiply that isn't paired with an
attenuation. Add a hard-clip safety net (`juce::jlimit`) as a last line of defense — it
should never be the thing keeping the plugin stable, but it should exist.

## CPU budget test

```cpp
class ShimmerReverbEngineCpuBudgetTest : public juce::UnitTest
{
public:
    ShimmerReverbEngineCpuBudgetTest() : juce::UnitTest ("ShimmerReverbEngine: CPU budget", "DSP") {}

    void runTest() override
    {
        beginTest ("Real-time factor stays within budget");

        ShimmerReverbEngine engine;
        const double sampleRate = 44100.0;
        const int blockSize = 512;
        juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) blockSize, 2 };
        engine.prepare (spec);
        engine.reset();

        juce::AudioBuffer<float> buffer (2, blockSize);
        fillWithSineWave (buffer, 1000.0f, (float) sampleRate);

        // Warm-up
        for (int i = 0; i < 100; ++i)
        {
            juce::dsp::AudioBlock<float> block (buffer);
            engine.process (block);
        }

        const auto start = juce::Time::getHighResolutionTicks();
        constexpr int iterations = 1000;
        for (int i = 0; i < iterations; ++i)
        {
            juce::dsp::AudioBlock<float> block (buffer);
            engine.process (block);
        }
        const auto end = juce::Time::getHighResolutionTicks();

        const double elapsedSeconds = juce::Time::highResolutionTicksToSeconds (end - start);
        const double audioSeconds = (iterations * blockSize) / sampleRate;
        const double realTimeFactor = elapsedSeconds / audioSeconds;

        logMessage ("Real-time factor: " + juce::String (realTimeFactor, 4));

        // Algorithmic reverb budget (see table below); Release build only —
        // Debug builds run far slower and will false-positive this.
        expect (realTimeFactor < 0.15, "Real-time factor " + juce::String (realTimeFactor, 4)
                                        + " exceeds 0.15 budget");
    }

private:
    static void fillWithSineWave (juce::AudioBuffer<float>& buffer, float freqHz, float sampleRate)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                data[i] = std::sin (2.0f * juce::MathConstants<float>::pi * freqHz * (float) i / sampleRate);
        }
    }
};

static ShimmerReverbEngineCpuBudgetTest shimmerReverbEngineCpuBudgetTest;
```

**Run this in a Release build.** A Debug build's real-time factor is meaningless — the
Hard Rule against wall-clock assertions is about flaky *absolute* timing; a relative
budget check like this is fine as long as it only ever runs against optimized code.

### Real-time factor budget by processing type

| Plugin/stage type | Budget (real-time factor) |
|---|---|
| Simple utility (gain, pan) | < 0.01 |
| Dynamics (compressor, gate) | < 0.05 |
| EQ (parametric, 4–8 bands) | < 0.08 |
| Time-based FX (delay, chorus) | < 0.10 |
| Algorithmic reverb | < 0.15 |
| Complex/spectral FX (convolution, phase vocoder) | < 0.30 |

Millenia's `ShimmerReverbEngine` combines a reverb tank *and* a pitch shifter — treat 0.15
as the target for the combined engine, not per-component; if `PitchShifter` alone is
already phase-vocoder-based (FFT), budget closer to the spectral-FX row instead.
