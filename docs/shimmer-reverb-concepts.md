# Shimmer Reverb — Concepts & Architecture Guide

A shimmer reverb is a normal reverb whose feedback signal gets pitch-shifted upward (usually +12 semitones, sometimes +7) before it re-enters the reverb tank. Each pass through the loop adds another, higher-pitched layer, so the tail slowly rises into an ethereal, ascending wash instead of just decaying. This doc explains the concepts and the architecture decisions behind our implementation *before* we touch code — read this first, then check `shimmer-reverb-architecture.md` (the dense technical reference Claude uses while coding) when we're actually implementing a stage.

## Quick path — what we're building, in order

1. A **reverb tank**: a network of delay lines and allpass filters that turns a short input into a smooth, dense decaying tail. This alone is just... a reverb.
2. A **pitch shifter** inserted into the tank's feedback loop, so the signal that re-enters is shifted up in pitch every cycle.
3. **Gain staging and stereo width**, so the loop doesn't run away and the sound isn't collapsed to mono.
4. **Parameters and UI**, once the DSP core actually sounds right.

## Concept 1 — The reverb tank

Three classic tank designs exist, and they all solve the same problem (turn one impulse into a dense, smooth cloud of reflections) differently:

| Design | How it works | Why it matters here |
|---|---|---|
| Schroeder/Moorer (e.g. Freeverb) | Parallel comb filters (feedback delays) + series allpass filters for diffusion | Simplest to understand; good first build to validate your DSP plumbing before adding complexity |
| Feedback Delay Network (FDN) | N delay lines cross-connected through an N×N mixing matrix | Scales to denser, longer tails; harder to tune (matrix + delay lengths) |
| Dattorro plate | Input diffuser (4 series allpass) feeding a figure-eight of two cross-feeding delay/allpass tanks with damping | **Our chosen base** — it has one clear, well-defined point in the loop to insert a pitch shifter, and it's proven to sound clean with few parameters |

We're using the **Dattorro topology** because real shimmer plugins (Valhalla Shimmer and others) are built on it, and because it gives us one obvious injection point for the pitch shifter rather than N ambiguous ones like a raw FDN.

## Concept 2 — The pitch shifter

Real-time pitch shifting inside a feedback loop has two competing approaches:

- **Dual-delay-line / crossfading read pointers** (time-domain): two read pointers move through a circular buffer at a rate proportional to the pitch ratio; when one nears a discontinuity, you crossfade to the other. Near-zero latency, cheap, but introduces small "grain" glitches at each crossfade.
- **Phase vocoder / FFT-based**: analyze frames, shift bin frequencies, resynthesize. Smoother pitch quality, but adds real latency (one or more FFT windows) and CPU cost — both a poor fit for something living inside a tight feedback loop.

**We're using the dual-delay-line approach.** Not just because it's cheaper — the grain artifacts it introduces get re-diffused by the tank's allpass stages on every pass, and that texture is actually part of what makes shimmer reverbs sound "shimmery" rather than a defect to eliminate.

## Architecture decisions

| Decision | Choice | Why |
|---|---|---|
| Tank topology | Dattorro plate | Single clear feedback injection point, proven low-parameter design |
| Pitch shift method | Dual-delay-line crossfade | Zero added latency, cheap, artifact character fits the genre |
| Default shift interval | +12 semitones (octave) | Most common shimmer setting; +7 (fifth) as a secondary preset later |
| Delay-line interpolation | 3rd-order Lagrange | Needed because the pitch shifter reads at a fractional, constantly-changing rate — linear interpolation would dull the signal too much |
| Feedback graph in JUCE | Hand-written `DelayLine` + manual read/write, **not** `dsp::ProcessorChain` | `ProcessorChain` is strictly linear/feed-forward — it cannot express a loop |

## Known pitfalls — check these as we build

- [ ] **Feedback runaway**: pitched signal keeps re-entering the loop — feedback gain must stay strictly below 1.0, with headroom for the shifter's own gain. Add a soft-clip/limiter safety net, don't just trust the gain knob.
- [ ] **Aliasing**: cheap interpolation on the pitch-shift read can alias. Lagrange3rd interpolation mitigates this; revisit oversampling only if it's still audible.
- [ ] **Metallic ringing**: delay lengths that are too short, too regular, or share common factors cause comb-filter coloration. Use empirically-tuned, mutually-prime-ish lengths (Dattorro's own published values are a good starting point, not a strict port).
- [ ] **Mono collapse**: shimmer reverbs need per-channel decorrelation (different delay lengths/modulation per channel, or quadrature-offset pitch shifting) or the wet signal sounds flat and mono.
- [ ] **DC offset**: allpass/comb feedback and crossfading can accumulate DC bias over long tails — a DC-blocking filter at the loop boundary is standard practice.
- [ ] **Denormals**: decaying tails are the classic case where denormal floats stall the CPU — `juce::ScopedNoDenormals` is mandatory (already enforced by the `juce-plugin-dev` skill).

## Roadmap

1. Build a plain Freeverb/Schroeder-style mono tank first, just to validate the JUCE DSP plumbing (delay lines, `prepareToPlay` sizing, real-time safety) — no pitch shifting yet.
2. Replace/extend it with the proper Dattorro topology.
3. Insert the dual-delay-line pitch shifter at the tank's feedback junction.
4. Tune gain staging, add the safety limiter, and add stereo decorrelation.
5. Wire up parameters through `AudioProcessorValueTreeState` and build the editor.

## References

- Dattorro, "Effect Design, Part 1: Reverberator and Other Filters," JAES 45(9), 1997 — [full PDF](https://ccrma.stanford.edu/~dattorro/EffectDesignPart1.pdf)
- Moorer, "About This Reverberation Business," Computer Music Journal 3(2), 1979
- Julius O. Smith III, *Physical Audio Signal Processing* (free online) — [ccrma.stanford.edu/~jos/pasp/](https://ccrma.stanford.edu/~jos/pasp/), [Feedback Delay Networks chapter](https://ccrma.stanford.edu/~jos/pasp05/Feedback_Delay_Networks.html)
- Freeverb (public domain reference tank) — [source](https://github.com/sinshu/freeverb), [walkthrough](https://ccrma.stanford.edu/~jos/pasp/Freeverb.html)
- Chowdhury-DSP/chowdsp_utils — JUCE module with a Dattorro-style FDN reverb and a ring-buffer pitch shifter — [github.com/Chowdhury-DSP/chowdsp_utils](https://github.com/Chowdhury-DSP/chowdsp_utils)
- Airwindows `Galactic` (feedback reverb + quadrature pitch-shift stereo widening, shimmer-adjacent) — [github.com/airwindows/airwindows](https://github.com/airwindows/airwindows)
- ADC21, Geraint Luff, "Let's Write a Reverb" — [talk](https://www.youtube.com/watch?v=6ZK2Goiyotk), [code](https://github.com/Signalsmith-Audio/reverb-example-code)
- ADC22, Geraint Luff, "Four Ways To Write A Pitch-Shifter" — [talk](https://www.youtube.com/watch?v=fJUmmcGKZMI), [code](https://github.com/Signalsmith-Audio/pitch-time-example-code)
- JUCE tutorial: [DSP module introduction](https://docs.juce.com/master/tutorial_dsp_introduction.html), [delay lines & feedback](https://docs.juce.com/master/tutorial_dsp_delay_line.html)

## Next step

Once you're ready to start coding, say so and we'll build step 1 of the roadmap (the plain mono tank) together — Claude will follow the real-time/parameter rules in the `juce-plugin-dev` skill and the concrete class mapping in `shimmer-reverb-architecture.md` while implementing it.
