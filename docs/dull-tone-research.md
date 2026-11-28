# Dull Tone Research — Where High Frequencies Are Lost or Never Generated

Status: research written 2026-11-18, on `docs/dull-tone-research` (based on `fix/shimmer-tone-dead-zone`, audit fixes 1-9). No code changed. Agenda item 3 (items 1 and 2 were [`reverb-space-research.md`](./reverb-space-research.md) and [`shimmer-character-research.md`](./shimmer-character-research.md)).
Scope: every stage from input to output, for the report "the overall tone sounds dull/muffled compared with comparable effects".

Line numbers are as of 2026-11-18. Symbol names are stable.

## 1. Summary

1. **The plain reverb is not dull.** With Shimmer Amount 0, wet band levels are flat within about 1 dB up to 8 kHz and 2.4 dB down at 11-22 kHz (white noise, section 4.1). Default Damping 0.0005 is inert (identical to Damping 0). HF decays at nearly the same rate as LF. A dull plain tank is not what the measurements show.
2. **The shimmer layer is where HF is lost or never generated.** The Shimmer Tone filter is a 4th-order low-pass on the shifter input. Since audit fix 9 the default cuts at **2.76 kHz** (it cut at about 5 kHz before). On a dark source this removes about 9 dB at 8 kHz and 23 dB at 16 kHz of the shimmer's own octave-up content compared with Tone Off (section 4.5).
3. **Nothing brightens.** No knob, stage or default adds HF. Every control either leaves the spectrum alone or darkens it. Even the brightest reachable setting (Tone Off, Damping 0) only reaches "flat".
4. **Size != 1 darkens the loop.** The tank's fixed lines read through Lagrange3rd at fractional delays. At Size 0.5 the 16 kHz band decays in 2.4 s vs 7.1 s at Size 1 (section 4.4). A real bug, but only off the default Size.
5. **Damping's top 40% of travel is the only audible part**, and it can only darken. Not a cause at default; a UX issue (section 4.2).

A caveat on evidence: a flat spectrum on noise cannot prove or disprove "dull" on real material. The perceived dullness is most likely (inferred, not measured) the missing sparkle in the shimmer layer plus the +6 dB wet-only transient smearing, not a loss in the plain tank. Section 7 asks for an A/B ear check to confirm.

## 2. Signal path and every stage that can lose HF

```
input (L+R mono-summed)                              ShimmerReverbEngine.cpp:406-417
  -> pre-delay (Lagrange3rd, 0 ms default = bypass)    :610-613
  -> DattorroTank input diffusers: 4 Schroeder allpass DattorroTank.cpp:333-336
  -> branch A/B: allpass1 (Thiran) -> delay1 -> DAMPING -> allpass2 -> delay2   :280-298
       cross-feed g = RT60 gain, plain/shimmer blend                           :446-452
  -> 14 output taps (Lagrange3rd, fractional)          DattorroTank.cpp:516-540
  shimmer loop (parallel to the plain cross-feed):
  feedbackFromB -> Tone LPF A -> Tone LPF B (TPT, 2x 2-pole)  ShimmerReverbEngine.cpp:520
                -> PitchShifter (+12 st, Lagrange3rd reads, Hann splices)  PitchShifter.cpp:108-118, :421
                -> [Formant corrector, OFF by default]  :566-572
                -> unity crossfade (Freeze only) -> DC blocker -> SafetyLimiter  :581-602
  -> M/S width -> Freeze leveler (1.0 unless Freeze) -> loop capture (no-op) :751-786
  -> wet low cut / high cut (bypassed at 20 Hz / 20 kHz)       :803-817
  -> dry/wet mix (default 100% wet)                            :830-853
```

| Stage | Where | HF effect at defaults | Verdict |
|---|---|---|---|
| Input bandwidth filter (Dattorro's 0.9995) | absent | none | Not a cause. The paper's value is transparent anyway |
| Input diffusers | `DattorroTank.h:344-345` (0.75/0.75/0.625/0.625) | allpass, flat | Not a cause. Time smearing only |
| Branch allpasses | `h:382-383` (-0.7, 0.5) | allpass, flat | Not a cause. Match the paper's decay diffusion |
| Delay-line interpolation | `h:243` Lagrange3rd | **none at Size 1** (integer delays); **low-pass at Size != 1** | Bug off-default (section 4.4) |
| allpass1 modulation | `h:289` Thiran, 0.1 ms | flat | Not a cause (Modulation 0 vs 0.1 identical, section 4.1) |
| Damping | `DattorroTank.cpp:292` | default 0.0005: -0.009 dB at Nyquist | Inert at default |
| Output taps | `DattorroTank.cpp:308` fractional offsets | -2.4 dB in the 11-22 kHz band (tank alone: 3.0 vs 5.5 dB gain) | Minor |
| Shimmer Tone | `ShimmerReverbEngine.cpp:246-264, :520` | 4th-order LP at 2756 Hz (6 dB down there, 15 dB at 4 kHz, 26 dB at 5.5 kHz) | **Main cause of a dull shimmer layer** |
| PitchShifter | `PitchShifter.cpp` | measured transparent: -0.1 dB gain at +12 st on every band (section 4.3) | Not a cause |
| Formant corrector | `ShimmerReverbEngine.cpp:566` | off by default | Not a cause |
| SafetyLimiter | `SafetyLimiter.cpp:20-25` | exactly unity below 0.8; tanh above (adds harmonics, never removes HF) | Not a cause |
| DC blocker | `DCBlocker.h` | 20 Hz high-pass | Not a cause |
| Wet high cut | `ShimmerReverbEngine.cpp:811-817` | inactive at 20 kHz | Not a cause. Output-only, not in the loop |
| Sample rate | T6 | 44.1 kHz: 16 kHz band 4.3 dB vs 6.5 dB at 96 kHz | Minor (the taps' fractional reads). Fix 10 only changes Damping |

Defaults checked in `Source/Parameters.cpp`: Damping 0.0005 on 0..0.3 with skew 0.3 (`:122-126`), High Cut 20 kHz (`:159-162`), Shimmer Tone 5000 Hz on 1.5-10 kHz, scaled by the alias cap (`:176-191`), Modulation 0.1, Feedback 0.55, Mix 100% wet (since changed to 40% at the plugin level, 2026-11-28).

**Damping range, verified current:** the APVTS range is `0..0.3` (`Parameters.cpp:125`). The diagnostic's name in `DattorroTankTests.cpp:791` still says `0..0.05` and is stale; the figures it quotes (brightness 1.26 / 1.24 / 1.15) are from the old range. The fix-10 agent's "0 to 0.3" is correct.

**Update (2026-11-21):** the Damping skew of 0.3 quoted above is superseded on `fix/damping-knob-range`: the knob centre now maps to a = 0.08 (about 17.8 kHz at 44.1 kHz) via `setSkewForCentre`. Range, default and stored values are unchanged, so the measurements in this document still hold.

## 3. How the Tone filter works since fix 9

`computeShimmerToneCutoffHz()` (`ShimmerReverbEngine.cpp:246-264`): `cap = (fs/4) / max(ratio, 1)`; `scale = min(1, cap / 10 kHz)`; `cutoff = Tone * scale`. At 44.1 kHz and +12 st: cap 5512.5 Hz, scale 0.55125, default cutoff **2756 Hz**, Off cutoff 5512 Hz. Before fix 9 the default was `min(5000, cap)` = 5000 Hz, so **fix 9 moved the default shimmer cutoff down by almost an octave**. That may be the change the user hears as "duller" (hypothesis; not A/B-tested).

Two cascaded TPT low-passes (`:90-91`, `:520`) = 4th order, 24 dB/oct, -6 dB at the cutoff. The filter has to exist at all because the shifter reads at ratio 2: content above fs/4 folds. It sits before the shifter, so shifted output is limited to 2x the cutoff.

## 4. Measurements (Release, 44.1 kHz, 512-sample blocks, mix 100%, L channel)

Method: throwaway test class (removed; `git status` clean of `Tests/`), Welch spectra (FFT 8192, Hann), octave bands as mean power density. Tables are "dB re the dry input's band level" unless stated. The harness ran the real `ShimmerReverbEngine` with the shipped defaults plus the listed overrides.

### 4.1 Steady-state white noise, last 3 s of 9 s (flat dry = 0 dB)

| Config | 125 | 250 | 500 | 1k | 2k | 4k | 8k | 16k | centroid Hz | %>8k |
|---|---|---|---|---|---|---|---|---|---|---|
| Dry input | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10012 | 60.2 |
| DEFAULT (+12 st) | 7.3 | 7.4 | 6.6 | 7.1 | 6.9 | 6.8 | 6.4 | 4.3 | 8602 | 50.9 |
| Amount 0 (plain tank) | 7.5 | 7.8 | 6.9 | 7.4 | 7.2 | 7.2 | 7.0 | 4.9 | 8715 | 51.9 |
| Tank only (`DattorroTank`) | 5.9 | 6.1 | 5.2 | 5.5 | 5.4 | 5.5 | 5.2 | 3.0 | 8686 | 51.4 |
| Tone Off | 7.3 | 7.5 | 6.6 | 7.1 | 7.0 | 7.0 | 6.5 | 4.3 | 8574 | 50.6 |
| Damping 0 | 7.3 | 7.4 | 6.6 | 7.1 | 6.9 | 6.8 | 6.4 | 4.3 | 8606 | 50.9 |
| Damping 0.05 | 7.3 | 7.4 | 6.6 | 7.0 | 6.9 | 6.7 | 6.0 | 3.5 | 8244 | 47.9 |
| Damping 0.1 | 7.3 | 7.5 | 6.6 | 7.1 | 6.8 | 6.6 | 5.6 | 2.9 | 7949 | 45.4 |
| Damping 0.3 (max) | 7.3 | 7.5 | 6.6 | 7.0 | 6.6 | 5.8 | 4.0 | 0.9 | 7124 | 38.2 |
| Tone Off + Damping 0 | 7.3 | 7.5 | 6.6 | 7.1 | 7.0 | 7.0 | 6.5 | 4.3 | 8578 | 50.6 |
| 0 st | 6.6 | 7.1 | 6.2 | 6.5 | 6.2 | 6.1 | 5.5 | 3.4 | 8461 | 49.7 |
| Feedback 1.0 | 6.6 | 6.7 | 6.2 | 6.3 | 6.3 | 6.0 | 5.2 | 3.0 | 8326 | 48.4 |
| Modulation 0 | 7.2 | 7.5 | 6.9 | 7.2 | 6.8 | 6.9 | 6.4 | 4.3 | 8586 | 50.8 |
| Size 0.5 | 8.7 | 10.2 | 9.3 | 8.9 | 8.8 | 8.6 | 7.1 | 1.8 | 6613 | 34.6 |
| Size 1.3 | 6.4 | 6.7 | 6.4 | 6.6 | 6.4 | 6.2 | 5.2 | 0.5 | 7102 | 39.1 |
| High Cut 8k (a darker reference) | 7.3 | 7.4 | 6.6 | 7.1 | 6.9 | 6.5 | 3.0 | -11.3 | 4407 | 12.9 |

- The wet is a uniform +6 to +7 dB over the dry (a reverb sum, not tone). Relative shape is flat to 8 kHz; the 11-22 kHz band is 2.4 dB lower (output taps).
- The default is 0.4-0.6 dB darker above 4 kHz than the plain tank (shimmer share), and Tone Off recovers most of it.
- Damping and Tone Off/Damping 0 move the white-noise spectrum by under 1 dB. Only High Cut, Damping above 0.1 and off-default Size have large effects on it.
- Pink noise gives the same deltas (e.g. default 16k band 4.5 vs plain 5.1; Damping 0.3 gives 1.1).

### 4.2 Damping: where its range sits

The one-pole cutoff is `fc = -(fs / 2 pi) ln(a)` and the knob maps `value = 0.3 * p^(1/0.3)` (JUCE skew 0.3). At 44.1 kHz:

| Damping a | fc | knob position p | measured RT60 16 kHz (s) |
|---|---|---|---|
| 0.0005 (default) | 53 kHz (above Nyquist) | 0.15 | 5.7 (shimmer on), 7.1 (plain) |
| 0.05 | 21 kHz | 0.58 | 4.9 |
| 0.1 | 16 kHz | 0.72 | 4.3 |
| 0.3 (max) | 8.4 kHz | 1.00 | 2.8 |

So about the first 58% of the knob's travel keeps the cutoff above 21 kHz (inaudible). Damping 0.05, 0.1 and 0.3 cost only 0.8, 1.4 and 3.4 dB in the 16 kHz band (0.4, 0.8 and 2.4 dB at 8 kHz) in steady state. It never brightens, and the default is already the brightest value (`Damping 0` is identical).

### 4.3 Shifter alone (does a +12 st shift regenerate HF?)

White noise through `PitchShifter` alone, output band level re input (dB): 0 st: 0.0 on every band; +12 st: 0.3 to -0.3; +7 st: -0.9 to -1.9; -12 st: +2.8 below 4 kHz, -11.9 at 16 kHz (the top octave is empty, expected). **The shifter is spectrally transparent**; its HF loss comes only from content it is never given.

Octave-band noise in, +12 st (centroid in -> out; share of output above 4 kHz):

| Input band | no Tone filter | Tone cut 2756 (default) | Tone cut 5512 (Off) |
|---|---|---|---|
| 1 kHz | 1057 -> 2113 Hz, 0% | 1050 -> 2099, 0% | 1057 -> 2112, 0% |
| 2 kHz | 2122 -> 4248, 58% | 1970 -> 3942, 42% | 2110 -> 4222, 57% |
| 4 kHz | 4245 -> 8488, 100% | 3387 -> 6766, 100% | 3944 -> 7884, 100% |
| 8 kHz | 8499 -> 15680, 100% | 6404 -> 12776, 100% | 6627 -> 13195, 100% |

The shift does regenerate HF (it doubles every frequency). The default Tone filter removes a large part of the 4 kHz and 8 kHz input bands' energy before the shift (centroids drop 4245 -> 3387 and 8499 -> 6404), i.e. the exact band whose octave would land at 8-16 kHz.

### 4.4 Decay per band (RT60, 40 s impulse response, fit -5..-25 dB)

| Config | 125 | 250 | 500 | 1k | 2k | 4k | 8k | 16k |
|---|---|---|---|---|---|---|---|---|
| DEFAULT +12 | 6.7 | 7.4 | 6.7 | 6.7 | 6.7 | 6.3 | 5.9 | 5.7 |
| Amount 0 (plain) | 7.7 | 7.8 | 7.4 | 7.4 | 7.2 | 7.2 | 7.2 | 7.1 |
| 0 st, default amount | 7.0 | 7.7 | 6.4 | 6.4 | 6.1 | 4.9 | 3.6 | 3.5 |
| +7 st | 6.5 | 7.2 | 7.2 | 7.0 | 6.8 | 6.4 | 5.9 | 5.7 |
| -12 st | 9.3 | 9.1 | 8.5 | 7.7 | 6.7 | 5.9 | 5.7 | 5.7 |
| Tone Off | 6.7 | 7.4 | 6.7 | 6.7 | 6.7 | 6.5 | 6.4 | 5.7 |
| Feedback 1.0 | 3.9 | 5.0 | 5.0 | 5.3 | 5.3 | 4.9 | 2.5 | 1.7 |
| Amount 0, Size 0.5 | 8.1 | 7.5 | 7.5 | 7.5 | 7.5 | 7.1 | 5.7 | 2.4 |
| Amount 0, Size 0.75 | 8.4 | 8.1 | 7.5 | 7.7 | 7.4 | 7.2 | 6.1 | 3.8 |
| Amount 0, Size 1.3 | 7.9 | 7.5 | 7.4 | 7.5 | 7.2 | 7.1 | 6.3 | 3.8 |
| Amount 0, Size 2.0 | 8.1 | 7.5 | 7.4 | 7.2 | 7.2 | 7.2 | 7.4 | 7.4 |

- **The plain tank has a near-flat decay** (7.1-7.8 s). It is brighter in the tail than a damped commercial reverb, not duller.
- At default +12 st the shimmer share shortens everything by about 1 s, HF about 10% more than mid.
- **0 st is the worst case for the loop:** the Tone filter then cuts at 5 kHz (cap 11 kHz, scale 1) in the recirculating shifted share, so 8 kHz and 16 kHz die in 3.5 s vs 7 s plain.
- **Size != 1 is a real HF loss in the loop:** 16 kHz RT60 drops from 7.1 s (Size 1) to 2.4 s (0.5), 3.8 s (0.75 and 1.3). Size 2.0 is fine because all delays stay whole samples. Cause: `applySize()` (`DattorroTank.cpp:106-121`) puts fractional values into Lagrange3rd lines (`h:243`) inside the recirculating loop; a half-sample read nulls Nyquist (the header's own Infinite comment, `h:262-270`, documents the same effect).
- Impulse response, time-sliced band energy (default): from 0.5-1 s to 6-10 s, 16 kHz falls 9-12 dB more than 125 Hz (e.g. -61.6 vs -49.5 dB at 6-10 s), vs 7 dB more in the plain tank (-48.7 vs -41.4). Mild.

### 4.5 Does the shimmer add HF? Dark source (pink, 4th-order LP 1.5 kHz, 1 s burst, tail 1.5-4 s)

| Config | 2k | 4k | 8k | 16k | centroid Hz | %>4k |
|---|---|---|---|---|---|---|
| Dry burst | 17.3 | -5.2 | -34.0 | -69.5 | 266 | 0.001 |
| Amount 0 | 12.7 | -9.3 | -37.7 | -58.2 | 264 | 0.001 |
| DEFAULT +12 | 19.1 | 7.6 | -14.2 | -53.6 | 407 | 0.127 |
| Tone Off | 19.4 | 9.1 | -5.3 | -30.4 | 436 | 0.355 |
| Feedback 1.0 | 22.9 | 14.8 | -3.8 | -43.7 | 708 | 1.003 |
| Feedback 1.0 + Tone Off | 23.2 | 16.7 | 6.0 | -15.5 | 861 | 3.100 |
| 0 st | 9.8 | -13.2 | -44.1 | -63.3 | 283 | 0.000 |

On a dark source the plain tank adds no HF (it only reverberates what is there); the shimmer at +12 st does (4 kHz +17 dB, 8 kHz +23 dB vs plain). **The default Tone filter costs 9 dB at 8 kHz and 23 dB at 16 kHz of that regenerated HF** vs Tone Off, and Feedback 1.0 + Tone Off has 20 dB more at 8 kHz than the default. This is the clearest quantified HF loss in the plugin. (Absolute dB values depend on the burst level.)

## 5. Comparison with reference designs

- **Dattorro (1997)** (values as carried in `DattorroTank.h:6-40, :344-383, :403`, and the reference port cited there): input bandwidth 0.9995, damping 0.0005, input diffusion 0.75/0.625, decay diffusion 0.7/0.5. Millenia matches all of them and omits only the input bandwidth stage, which at 0.9995 is transparent. The measured plain tank confirms it is the paper's brightness.
- **LushVerb** (`docs/reference/lushverb-shimmer-reverb.md`): shimmer is applied BEFORE the reverb (phase vocoder, no loop low-pass), `juce::dsp::Reverb` Damping default 30% (a Freeverb HF roll-off, so it is darker than Millenia's plain tank by design), Shimmer default 30%, Mix default 30%. It generates its octave from the full-band input. Millenia's octave is band-limited by Tone.
- **Valhalla Shimmer** (public manual/FAQ): High Cut is a low-pass in the feedback path, so the tail darkens as it decays; the "dark" colour mode is recommended for shimmer because the natural HF roll-off removes most aliasing in the shifter's feedback path. Classic shimmers therefore also band-limit the loop (consistent with Millenia's Tone filter), but they offer a user High Cut with a documented, usable range and tails that still sparkle at +12 st with Feedback 0.5 or higher. I could not retrieve the controls page (HTTP 403) so exact defaults are unverified.
- `docs/project-audit-2026-11.md` "Tone (dull)" listed three candidates: 30% of loop energy through a low-pass at 5.5 kHz or below (**confirmed and worse since fix 9**), Lagrange3rd on every pass (**confirmed only for Size != 1 and the taps; the shifter is clean**), no early reflections (not measured; a perceptual cue, not an HF loss).

Sources: [ValhallaShimmer manual (PDF)](https://www.valhalladsp.com/shimmer/ValhallaShimmerNotes.pdf), [ValhallaShimmer: tips and tricks](https://valhalladsp.com/2010/12/02/valhallashimmer-tips-and-tricks-shimmering/), [ValhallaShimmer: the controls](https://valhalladsp.com/?p=553). Only ideas are used; no code.

## 6. Ranked root causes

### (i) Bugs or wrong approach

| # | Cause | Evidence | Effort |
|---|---|---|---|
| 1 | Default Shimmer Tone (4th order, 2.76 kHz at +12 st after fix 9) throws away the HF the shimmer is meant to add | 4.3, 4.5: -9 dB at 8 kHz, -23 dB at 16 kHz vs Tone Off; 4.4: 0 st loop 8 kHz RT60 3.6 s vs 7.2 s | Small (default / slope), medium (restructure) |
| 2 | Fractional delays in the recirculating tank lines at Size != 1 (Lagrange3rd) | 4.4: 16 kHz RT60 2.4 s at Size 0.5, 3.8 s at 0.75 and 1.3 | Medium (Thiran or integer-rounded delays; Size-glide interaction documented at `DattorroTank.h:278-288`) |
| 3 | Output taps at fractional offsets (Lagrange3rd) | 4.1: -2.4 dB in the 11-22 kHz band even at Size 1 | Small (round tap offsets to whole samples at Size 1, or Thiran) |

### (ii) Defaults the user can change right now

| # | Default | Effect | Effort |
|---|---|---|---|
| 4 | Shimmer Tone 5 kHz, which fix 9 turned into 2.76 kHz | Turn Tone to Off: 8 kHz +9 dB, 16 kHz +23 dB in the shimmer layer | Knob / one constant |
| 5 | Shimmer Feedback 0.55 | 1.0 + Tone Off: 8 kHz +20 dB vs default (4.5). Trade-off: RT60 falls (4.4: 3.9-5.3 s mids at 1.0) | Knob / one constant |
| 6 | Mix 100% wet by default (plugin default changed to 40% on 2026-11-28) | Dry attack HF is entirely replaced by a diffused wet (inferred; not measured). Not a spectrum issue | Knob |
| 7 | Size away from 1 | See cause 2 | Knob |

### (iii) Design gaps

| # | Gap | Effort |
|---|---|---|
| 8 | No brightening/air/tilt stage anywhere. Every control only keeps or lowers HF; the best case is "flat". Comparable effects often have a tone/shelf control | Small-medium (wet high shelf or tilt) |
| 9 | The octave is limited by an anti-alias filter in the loop rather than by a higher-rate shifter. Better anti-aliasing (2x oversampled shifter, or a shifter that reads at an alias-free rate) would remove the need for Tone to cut so low | Large |
| 10 | Damping UX: 58% of the travel is dead and the control cannot brighten | Small (remap the range; interacts with audit fix 10) |
| 11 | No early reflections (audit candidate). Affects perceived attack and clarity, not the measured spectrum | Large; out of scope here |

### (iv) Not the cause (measured or by construction)

Input diffusers and branch allpasses (allpass, flat). Dattorro input bandwidth (transparent in the paper). Default Damping (inert, `Damping 0` identical). Thiran modulation (Modulation 0 vs 0.1 identical). PitchShifter itself (transparent, 4.3). Formant corrector (off). SafetyLimiter, DC blocker, wet low/high cut at their defaults. Sample rate (44.1 vs 96 kHz moves the 16 kHz band 2.2 dB, via the taps).

## 7. Proposed slices

**Slice 1: make the shimmer sparkle (small, default changes plus tests first).**
1. Move the default Shimmer Tone so the default cutoff is not 2.76 kHz at +12 st. Candidate A: default to Off (cutoff = alias cap, 5.5 kHz). Candidate B: keep a Tone value but halve the slope (one 2-pole filter instead of two), so the default is about -3 dB at 4 kHz instead of -15 dB.
2. Re-measure with the harness in section 4: target is a dark-source tail with at least 20 dB more 8 kHz than Tone 5 kHz and no new instability (re-run the boundedness and decay-to-silence tests; the loop changed).
3. Ear A/B on a real guitar/pad source, default vs Tone Off vs Feedback 0.8 (also confirms whether this is the dullness the user hears).

**Slice 2: remove the Size != 1 darkening (medium).** Replace the fractional Lagrange3rd reads on `delay1`, `delay2` and `allpass2` with Thiran (as `allpass1` already is) or snap the delays to whole samples when Size is not gliding, and tackle the output taps' fractional reads. Needs the Size-glide click test and the golden-hash tests re-run.

**Slice 3: a Brightness / Air control (small-medium, new parameter).** A wet-only high shelf (or tilt) after the tank, before the high cut. Requires the state schema and the APVTS layout change (`StateSchema.h`), not just DSP.

**Slice 4: Damping remap (small).** Re-skew or re-range Damping so the whole travel is audible, or express it in Hz. Coordinate with fix 10 (damping compensated by sample rate, branch `fix/damping-sample-rate`; same coefficient, rescaled `c^(44100/fs)`, bit-identical at 44.1 kHz).

**Deferred:** oversampled or alias-free shifter (cause 9) and early reflections (cause 11).

### Decisions for the user

1. **Tone default:** Off, or a gentler 2nd-order filter, or keep the 4th-order but raise the default? (Slice 1.)
2. **Feedback default:** keep 0.55, or raise it (more octave, shorter tail)?
3. **Brightness / Air control:** add one? Wet-only shelf (name "Air", fixed corner around 6-8 kHz, 0 to +6 dB) or a full tilt EQ? Default 0 dB (unchanged sound) or slightly positive?
4. **Size fix scope:** Thiran for everything in the tank (flat, but a short transient at each integer crossing during the Size glide) or whole-sample snapping when static?
5. **Damping:** remap to Hz and drop the dead zone, or leave it?
6. **Mix default:** leave at 100% wet or move to a typical insert value (audit item)?
7. **Ear check first?** Please A/B Tone Off, Feedback 0.8 and Size 1.0 before approving; if the tone is still dull with those, the cause is outside the loop (transient smearing, no early reflections) and is not measurable with spectra alone.

## 8. Reproducing the measurements

The harness was a throwaway `juce::UnitTest` class included temporarily from `Tests/Source/DCBlockerTests.cpp` (built as the existing Release `MilleniaTests` project, run with a name filter) and then removed; no repo files other than this doc and the `AGENTS.md` index line changed. It drove `ShimmerReverbEngine` (all parameters through the public setters), `DattorroTank` and `PitchShifter` directly. To redo a number: noise bursts (white, pink via Kellet) or a unit impulse, Welch spectrum for steady states, STFT band energy with Schroeder backward integration for RT60. Caveat: RT60 uses a -5..-25 dB fit extrapolated to 60 dB on a shimmering, non-exponential tail; read the values as relative.
