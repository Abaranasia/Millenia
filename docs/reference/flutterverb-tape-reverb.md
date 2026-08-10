# Reference: FlutterVerb (tape-flutter plate reverb, external project)

> Imported from [glittercowboy/plugin-freedom-system](https://github.com/glittercowboy/plugin-freedom-system),
> `plugins/FlutterVerb/.ideas/{creative-brief,parameter-spec,architecture}.md`. External design reference only —
> see [README.md](README.md) for why this is here. Not a shimmer reverb — kept for its
> modulation and `juce::dsp::Reverb` API notes. Not implemented in Millenia; not verified against a working build.

## Concept

Plate reverb with tape-style wow/flutter modulation, saturation, and a DJ-style
tilt filter. Not shimmer-related in intent, but structurally close: reverb tank →
modulation stage → tone shaping → dry/wet.

## Parameters

| Parameter | Range | Default | DSP target |
|---|---|---|---|
| SIZE | 0–100% | 50% | `juce::dsp::Reverb` roomSize |
| DECAY | 0.1s–10s | 2.5s | Reverb roomSize+damping combination |
| MIX | 0–100% | 25% | `DryWetMixer` wet proportion |
| AGE | 0–100% | 20% | Scales both wow (0.5–1.5Hz) and flutter (4–8Hz) LFO depth |
| DRIVE | 0–100% | 20% | `tanh(gain * sample)`, gain = `1.0 + drive*9.0` |
| TONE | −100 to +100 | 0 | Exponential LP/HP crossfade (Butterworth, state-reset on type flip) |
| MOD_MODE | toggle | wet-only | Routes AGE modulation to wet-only or wet+dry paths |

## Wow/flutter modulation

- Two independent sine LFOs: wow 0.5–1.5 Hz, flutter 4–8 Hz, both scaled by a single
  AGE parameter, summed (`totalModulation = wowLFO + flutterLFO`) and applied to a
  `juce::dsp::DelayLine<float, Lagrange3rd>` (50ms base delay, ±20% depth at AGE=100%).
- Per-channel independent phase tracking (no shared LFO state across L/R) — this is the
  detail that keeps dual-mono modulation from collapsing stereo width.

This dual-LFO-into-a-delay-line pattern is the generic building block for *any* tape/pitch
flutter character — relevant if Millenia ever wants an optional "flutter" mode on the
shimmer voice or tank output, independent of the shimmer pitch-shift itself.

## `juce::dsp::Reverb` API pitfall

This plugin's real build failure is captured as a generalized troubleshooting entry at
[../../troubleshooting/api-usage/juce-dsp-reverb-api-mismatch.md](../../troubleshooting/api-usage/juce-dsp-reverb-api-mismatch.md):
mixing the legacy `juce::Reverb` API (`setSampleRate()`, `processMono()`/`processStereo()`)
with the modern `juce::dsp::Reverb` (`prepare(spec)` / `process(context)`) doesn't compile.
Doesn't apply to `DattorroTank` today (it's a fully custom implementation, not
`juce::dsp::Reverb`), but worth knowing if `juce::dsp::Reverb` is ever pulled in elsewhere
(e.g. a secondary short-room voice).

## Notes not directly reusable

- DJ-style tilt filter and tape saturation are tone-shaping features unrelated to shimmer;
  skip unless Millenia wants a "character" stage of its own.
- MOD_MODE (wet-only vs wet+dry routing toggle) is a reasonable pattern if a future
  flutter mode needs to affect the dry signal too — otherwise not applicable.
