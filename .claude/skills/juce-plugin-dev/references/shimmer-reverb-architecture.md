# Shimmer Reverb — Architecture Reference

Consult this while implementing the Millenia shimmer reverb DSP. Conceptual background and rationale for a human reader lives in `docs/shimmer-reverb-concepts.md` — this file is the dense, decision-focused version for implementation.

## Topology

Dattorro plate, pitch shifter inserted at the tank feedback junction:

```
input → input diffuser (4x series allpass) → tank A (delay + allpass + damping)
                                            ↘ cross-feed ↗
                                              tank B (delay + allpass + damping)
tank output → pitch shifter (dual-delay-line crossfade) → back into tank input (feedback)
```

Do not build the feedback loop with `juce::dsp::ProcessorChain` — it is strictly linear/feed-forward and cannot express a cycle. Use `juce::dsp::DelayLine` with manual push/pop and hand-managed read/write indices for every stage that participates in the loop.

## JUCE class mapping

| Stage | JUCE class | Notes |
|---|---|---|
| Input/tank diffusion allpass | Hand-rolled Schroeder allpass: `juce::dsp::DelayLine<float, Lagrange3rd>` + manual feedback/feedforward (`y[n] = -g*x[n] + delayed; delayLine.pushSample(x[n] + g*delayed)`) | **Not** `IIR::Filter::makeAllPass()` — that's a frequency-domain biquad (phase shift, no meaningful time delay) and cannot produce Dattorro's diffusion (spreading a transient across a 100–900 sample delay). Same structure as `ScratchSchroederTank::processAllpass` |
| Tank delay lines | `juce::dsp::DelayLine<float, DelayLineInterpolationTypes::Lagrange3rd>` | Lagrange3rd required — modulation-safe, low coloration; `Linear` is cheaper but dulls the signal, `Thiran` is phase-inaccurate |
| Pitch shifter | Hand-rolled: own circular buffer + two read pointers at rate = pitch ratio, crossfade near wrap discontinuities | `juce::LagrangeInterpolator`/`CatmullRomInterpolator` can supply the fractional-read interpolation; reset interpolator state on discontinuities |
| Damping filter (in feedback path) | `juce::dsp::IIR::Filter<float>` one-pole low-pass | Placed inside the loop, not just at output, to shape the decaying spectrum like Dattorro's design |
| `juce::dsp::Reverb` | **Do not use** | Closed/opaque Freeverb-derived implementation; no exposed taps for pitch-shift injection (confirmed: forum.juce.com/t/modifying-extending-juce-reverb/58804) |
| Anti-aliasing | `juce::dsp::Oversampling<float>` around the shifter stage | Add only if aliasing is audible after Lagrange3rd interpolation is in place — not a day-one requirement |

## Starting parameter values

| Parameter | Starting value | Notes |
|---|---|---|
| Pitch shift interval | Continuous parameter, −24 to +24 semitones; quick-select presets at +7/+12/+19 | One parameter, not two — presets just set the same value. Negative values trade the ascending shimmer character for a darker, descending tail; still the same shifter algorithm |
| Crossfade window | ~20–30 ms at the +12 st default | Must be sized for the *slowest* read-rate the range allows (see pitfall below), not just the default |
| Feedback gain | 0.6–0.85, strictly < 1.0 | Must account for shifter's own gain; tune by ear, never assume unity is safe |
| Delay-line lengths | Empirically tuned, mutually-prime-ish | Reference Dattorro's (1997) and Freeverb's published tunings as a *starting point*, not a literal port |

## Pitfalls → mitigations

| Pitfall | Mitigation |
|---|---|
| Feedback runaway (pitched signal re-entering repeatedly) | Feedback gain strictly < 1.0 with headroom for shifter gain; add a soft-clip/limiter safety net in the loop, not just a gain knob |
| Aliasing from fractional-rate reads | Lagrange3rd interpolation first; oversample the shifter stage only if still audible |
| Metallic ringing / comb coloration | Avoid short, regular, or harmonically related delay lengths; tune empirically |
| Mono collapse | Per-channel decorrelation: different delay lengths/modulation per channel, or quadrature-offset pitch shifting (see Airwindows `Galactic`) |
| DC offset accumulation | DC-blocking filter at loop input or output |
| Denormal CPU stalls on long decaying tails | `juce::ScopedNoDenormals` at the top of `processBlock` (already a hard rule in this skill) |
| Bidirectional pitch range makes the grain window too short at extreme downward shifts | Read rate = 2^(semitones/12); at −24 st the read pointer advances at 0.25× the write rate, so it needs proportionally more buffer to cover the same real-time grain window. Size the pitch shifter's circular buffer for the *most negative* semitone value the parameter range allows, not for the +12 st default |

## Real-time safety (ties to this skill's Hard Rules)

- All `DelayLine`/`IIR::Filter`/oversampling buffers sized via `prepare(ProcessSpec)` in `prepareToPlay`, never inside `processBlock`.
- Sample-rate or block-size changes re-trigger `prepareToPlay` — delay-line max length and oversampling factor must be recalculated there, not assumed constant.
- The pitch shifter's own circular buffer follows the same rule: allocate/resize in `prepareToPlay` only.

## Verified sources

- Dattorro, "Effect Design, Part 1," JAES 45(9), 1997 — https://ccrma.stanford.edu/~dattorro/EffectDesignPart1.pdf
- Julius O. Smith III, *Physical Audio Signal Processing*, Feedback Delay Networks chapter — https://ccrma.stanford.edu/~jos/pasp05/Feedback_Delay_Networks.html
- Freeverb — https://github.com/sinshu/freeverb, https://ccrma.stanford.edu/~jos/pasp/Freeverb.html
- Chowdhury-DSP/chowdsp_utils (Dattorro-style FDN + ring-buffer pitch shifter reference) — https://github.com/Chowdhury-DSP/chowdsp_utils
- Airwindows `Galactic` (feedback + quadrature pitch-shift widening) — https://github.com/airwindows/airwindows
- surge-synthesizer/surge-fx (JUCE FX plugin shell reference) — https://github.com/surge-synthesizer/surge-fx
- ADC21 Geraint Luff "Let's Write a Reverb" — https://www.youtube.com/watch?v=6ZK2Goiyotk, code: https://github.com/Signalsmith-Audio/reverb-example-code
- ADC22 Geraint Luff "Four Ways To Write A Pitch-Shifter" — https://www.youtube.com/watch?v=fJUmmcGKZMI, code: https://github.com/Signalsmith-Audio/pitch-time-example-code
- JUCE dsp module docs — https://docs.juce.com/master/tutorial_dsp_introduction.html, https://docs.juce.com/master/tutorial_dsp_delay_line.html
- `juce::dsp::Reverb` non-extensibility confirmed — https://forum.juce.com/t/modifying-extending-juce-reverb/58804
- Pitch-shift smoothing/Doppler artifact discussion — https://forum.juce.com/t/pitch-shifting-delay-smoothing-artifacts-and-the-doppler-effect/41488

## Related

- `../SKILL.md` — real-time safety, parameter, and state hard rules this architecture must comply with
- `docs/shimmer-reverb-concepts.md` — human-facing conceptual guide and roadmap
