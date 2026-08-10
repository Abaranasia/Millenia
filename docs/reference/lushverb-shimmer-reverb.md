# Reference: LushVerb (shimmer reverb, external project)

> Imported from [glittercowboy/plugin-freedom-system](https://github.com/glittercowboy/plugin-freedom-system),
> `plugins/LushVerb/.ideas/{creative-brief,parameter-spec,architecture}.md`. External design reference only —
> see [README.md](README.md) for why this is here. Not implemented in Millenia; not verified against a working build.

## Concept

Single-purpose "beautiful by default" reverb inspired by Strymon BigSky: four knobs
(Size, Damping, Shimmer, Mix), always-on subtle modulation, optional +1 octave shimmer
layer. Priority order stated in the brief: **lush/clean/stunning sound over CPU efficiency**.

## Parameters

| Parameter | Range | Default | DSP target |
|---|---|---|---|
| SIZE | 0.5s–20s (log skew) | 2.5s | `roomSize = 0.1 + (sizeSeconds / 20.0) * 0.9` fed to `juce::dsp::Reverb` |
| DAMPING | 0–100% | 30% | Reverb `damping` directly (HF rolloff only, independent of decay) |
| SHIMMER | 0–100% | 30% | Wet amount of the +1 octave pitch-shifted signal; 0% fully bypasses the shifter (no CPU cost) |
| MIX | 0–100% | 30% | `juce::dsp::DryWetMixer`, linear mixing rule |

## Signal chain

```
Input → DryWetMixer.pushDrySamples()
      → Shimmer pitch shifter (+1 octave, applied to input BEFORE reverb)
      → juce::dsp::Reverb (SIZE, DAMPING)
      → dual-LFO delay-line modulation (always on, not a parameter)
      → DryWetMixer.mixWetSamples() (MIX)
      → Output
```

Notable routing choice: shimmer is mixed in **before** the reverb tank, so the shifted
signal gets reverberated too (pitch-shifted reverb tail), rather than reverberating first
and shifting the tail afterward. That's a real architectural fork worth comparing against
our own `ShimmerReverbEngine` routing.

## Shimmer pitch shifter

- Phase vocoder: FFT size 2048, 4× overlap (75%), Hann window, STFT → shift → ISTFT.
- Fixed ×2.0 ratio (exactly +1 octave, not continuously tunable).
- Latency ~46ms @ 44.1kHz — compensated via the `DryWetMixer`'s built-in latency
  compensation (dry signal captured pre-shift, mixer handles the delay match).

**Contrast with Millenia's `PitchShifter`:** if ours is a granular/delay-line design rather
than an FFT phase vocoder, the tradeoff is roughly: phase vocoder gives cleaner sustained
tones but higher fixed latency and CPU (FFT+windowing every block); granular/delay-line
gives lower latency and cheaper CPU but more grain/warble artifacts on transients. Worth
confirming which side of that line `Source/DSP/PitchShifter.cpp` actually sits on and
whether the reported latency is declared via `setLatencySamples()`.

## Always-on modulation (not user-facing)

- Two independent LFOs (0.3 Hz and 0.5 Hz) modulating a `juce::dsp::DelayLine`
  (Lagrange3rd interpolation, 10ms max delay) applied **post-reverb, pre-mix**.
- Depth: ±3ms — deliberately subtle, framed as "not obvious pitch wobble," just
  movement/width. Compare against any modulation already in `DattorroTank`.

## Open questions worth carrying into our own design review

1. Do we shimmer pre-tank or post-tank in `ShimmerReverbEngine`? LushVerb chose pre-tank
   (shifted signal gets reverberated). Confirm this was a deliberate choice for us, not
   an accident of implementation order.
2. Is our shimmer amount hard-bypassed at 0% (skip the shifter's DSP work entirely) the
   way LushVerb explicitly does for CPU savings, or does it always run?
3. Do we have *any* built-in low-depth modulation independent of the shimmer voice
   (LushVerb's dual 0.3/0.5 Hz ±3ms layer), or does all movement come from the shimmer
   pitch-shift itself? A tiny always-on modulation layer is cheap and reads as "lush."
