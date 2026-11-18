# Reverb Space Research — Why Millenia Sounds Small

Status: research written 2026-10-11. Slice 1 (section 8) is implemented in `e4406f3`. Section 2 describes the design **before** slice 1; see [`reverb-dev-journal.md`](./reverb-dev-journal.md) for what changed.
Scope: the reverb core (`DattorroTank` + its surroundings in `ShimmerReverbEngine`). Shimmer, formant correction and loop capture are out of scope.

## 1. Problem

The reverb has sounded "smaller than expected" since day one. It is not a regression. This doc maps the current design, compares it with how algorithmic reverbs are usually built, and ranks the changes most likely to fix it.

## 2. Design before slice 1 (facts, 2026-10-11)

| Aspect | Current state | Reference |
|---|---|---|
| Input | L+R summed to mono before the tank | `ShimmerReverbEngine.cpp:240-251` |
| Input bandwidth filter | Missing (Dattorro uses 0.9995 one-pole) | — |
| Pre-delay | None | — |
| Early reflections | None | — |
| Input diffusers | 4 allpasses, 4.77/3.59/12.73/9.31 ms, g = 0.75/0.75/0.625/0.625 | `DattorroTank.h:223-225` |
| Tank | Dattorro figure-eight, lengths in ms (correctly rescaled from 29761 Hz), loop ≈ 725 ms | `DattorroTank.h:227-237, 265` |
| Size control | None, all lengths `constexpr` | — |
| Modulation | None, except allpass1 when Infinite is on | `DattorroTank.cpp:269-277` |
| Damping | One one-pole lowpass per branch, default ≈ off, max ≈ 8.4 kHz | `Parameters.cpp:86-90` |
| Low-frequency decay | Same as mids (single decay gain, no multiband) | — |
| Decay | `feedback` 0–0.85 used as raw gain, no RT60 mapping (≈7 s at 0.7, ≈15 s at 0.85, rough estimate) | `Parameters.cpp:23-27` |
| Output taps | Only Dattorro's **left** tap set (7 taps, ×0.6); right set not implemented | `DattorroTank.h:517-534` |
| Stereo | Mono `tankOut` copied to L and R; only the shimmer side term (`width·amount·sustain·0.5·sideShift`, ≈0.064 at defaults) separates them | `ShimmerReverbEngine.cpp:493-495` |
| Wet EQ | None | — |

**Key finding:** the reverb tail is effectively **mono**. With Shimmer Amount, Shimmer Sustain or Width at 0, wet L == wet R exactly. Width does not decorrelate the reverb itself. A mono tail is perceived as narrow and close, which is the "small" impression regardless of decay length.

The sample-rate scaling of Dattorro's constants, a common "small reverb" bug, is already correct here.

## 3. What makes a reverb sound big

Perceptual research (Beranek, Barron, Griesinger) identifies these cues:

1. **Initial time-delay gap (pre-delay).** Beranek ranks it as the top predictor of hall quality. About 15 ms is optimal for halls, and above about 35 ms the space starts to sound arena-like. Zero gap reads as a small, close space.
2. **Low interaural cross-correlation.** Early lateral energy (< 80 ms) gives apparent source width (ASW). A decorrelated late tail gives listener envelopment (LEV). Both require L ≠ R.
3. **Low-frequency decay ≥ mid decay.** Real halls keep the bass longer, as with Lexicon "Bass Multiply" and zita-rev1 `t60dc`. A single gain for all frequencies cannot do this.
4. **Modal and echo density.**
   - Modal density: the total delay M must satisfy M ≥ 0.15·T60·fs (Jot/Schroeder). Our ≈ 725 ms loop supports a T60 of about 4.8 s, so longer tails need a larger size.
   - Echo density: aim for 1500–4000 echoes/s.
5. **Modulation.** Slow random or LFO modulation of the tank delays (0.1–2 Hz, 0.2–1 ms) de-rings static modes. Lexicon and Valhalla treat it as essential to the "lush, open" character.

### Typical preset ranges (synthesized from product docs and literature, not one authoritative source)

| Type | RT60 mid | Pre-delay | Low mult | HF damping | ER | Size |
|---|---|---|---|---|---|---|
| Room | 0.3–1.2 s | 0–15 ms | 1.0–1.2× | moderate | dense, 5–40 ms | 0.3–0.7 |
| Chamber | 1.0–2.5 s | 10–30 ms | 1.0–1.3× | light–moderate | medium | 0.6–1.0 |
| Hall | 1.8–3.5 s | 20–50 ms | 1.2–1.5× | moderate | sparse, lateral, 15–80 ms | 1.0–1.5 |
| Cathedral | 4–10+ s | 40–100 ms | 1.3–2.0× | strong | sparse, late | 1.5–2.0+ |
| Plate | 1–5 s | 0–30 ms | ≤ 1.0× | bright | none | n/a |

## 4. Algorithm families

| Family | Character | Fit for Millenia |
|---|---|---|
| Schroeder/Moorer, Freeverb | Thin or boxy, low density | No |
| **Dattorro plate (current)** | Dense and lush, but a *plate*: fixed size, instant build-up, no ER, one decay band | Keep as the core, add around it |
| Griesinger/Lexicon 224/480L | Benchmark "big" sound, with random modulation and bass multiply | Borrow ideas (modulation, bass mult) |
| Gardner nested allpass | Separate designs per room size | Reference only |
| **Jot FDN (8–16 lines, Householder/Hadamard)** | Most scalable, controlled RT(f) | Possible second "Hall" engine later |
| Signalsmith diffuser + FDN | Large and smooth, simple to build | Good template for an FDN engine |
| Velvet noise | Efficient smooth tail, weak as an interactive size control | No |
| Convolution | Realistic but static | No |

ValhallaPlate is the closest commercial analogue. It shows that a plate with **Size up to 200%** and light modulation "sounds much more like a chamber than a plate". A scaled Dattorro can therefore reach chamber or small-hall territory, but not a true large hall.

## 5. Key papers

- **Dattorro 1997, "Effect Design Part 1", JAES 45(9).** The plate tank we use. It has modulated tank allpasses and separate L/R tap sets, both of which we omitted. [PDF](https://ccrma.stanford.edu/~dattorro/EffectDesignPart1.pdf)
- **Jot & Chaigne 1991, AES preprint 3030.** FDN with per-delay absorption filters, giving frequency-dependent RT60 without coloration.
- **Schlecht & Habets.** "On Lossless FDNs" (IEEE TSP 2017, [arXiv](https://arxiv.org/pdf/1606.07729)); time-varying feedback matrices (JASA 2015).
- **Välimäki et al. 2012, "Fifty Years of Artificial Reverberation", IEEE TASLP.** Canonical survey. [DOI](https://doi.org/10.1109/TASL.2012.2189567)
- **Gardner 1992, "The Virtual Acoustic Room" (MIT thesis).** Topology should change with room size. [PDF](https://freeverb3-vst.sourceforge.io/doc/53570860-Virtual-Acoustic-Room.pdf)
- **Perceptual cues.** [Beranek, Heyser lecture](https://aes2.org/wp-content/uploads/2023/08/AES123heyser-Beranek.pdf), [Barron](https://www.ioa.org.uk/system/files/proceedings/m_barron_spatial_impression_and_envelopment_in_concert_halls_conclusions.pdf), [Skålevik](https://www.akutek.info/Papers/MS_ISMRA2016-39_Preprint.pdf), [Griesinger](https://www.ecousticsystems.com/spaciousness-and-envelopment-musical-acoustics)
- **Mode-density rule.** [JOS PASP](https://www.dsprelated.com/freebooks/pasp/Mode_Density_Requirement.html)
- **Signalsmith, "Let's write a reverb".** [Article](https://signalsmith-audio.co.uk/writing/2021/lets-write-a-reverb/)
- **Valhalla blog.** [ValhallaPlate controls](https://valhalladsp.com/?p=74068), [Diffusion and metallic artifacts](https://valhalladsp.com/2011/01/21/reverbs-diffusion-allpass-delays-and-metallic-artifacts/)

## 6. Open-source references

| Project | License | Borrow |
|---|---|---|
| [CloudSeedCore](https://github.com/GhostNoteAudio/CloudSeedCore) | MIT | Full "around the tank" chain: pre-delay, multitap ER, seeded diffusers, multiband EQ. **Most relevant.** |
| [Surge XT Reverb2](https://github.com/surge-synthesizer/sst-effects/blob/main/include/sst/effects/Reverb2.h) | GPL-3.0 (read for ideas, do not copy) | Power-of-2 size scaling, in-loop LF/HF damping, per-block quadrature LFOs |
| [Vital reverb](https://raw.githubusercontent.com/mtytel/vital/main/src/synthesis/effects/reverb.h) | GPL-3.0 (ideas only) | Size 2^-3…2^1, pre low/high cut, in-loop shelves |
| [zita-rev1](https://kokkinizita.linuxaudio.org/linuxaudio/zita-rev1-doc/quickguide.html) / [Faust reverbs.lib](https://faustlibraries.grame.fr/libs/reverbs/) | GPL / LGPL | 2-band decay (low/mid RT60, crossover, HF half-RT point). jpverb's parameter map works as a spec. |
| [Mutable Clouds reverb.h](https://raw.githubusercontent.com/pichenettes/eurorack/master/clouds/dsp/fx/reverb.h) | MIT | Compact Dattorro variant with modulation |
| [Signalsmith example](https://github.com/Signalsmith-Audio/reverb-example-code) | Unverified | Hadamard diffuser + Householder FDN |
| [Dragonfly Reverb](https://github.com/michaelwillis/dragonfly-reverb) | GPL-3.0 | Preset tables, separate ER plugin |
| [Airwindows Galactic](https://airwindows.com/galactic) | MIT | Small Householder FDNs |

GPL code must not be copied into Millenia unless the project adopts GPL. Use those repositories for ideas only.

## 7. Ranked changes for Millenia

Ordered by expected perceived-size gain per unit of effort and risk. "Tank" means a change inside `DattorroTank`; "Around" means a new stage before or after it.

| # | Change | Where | Effort | Risk | Rationale |
|---|---|---|---|---|---|
| 1 | **True stereo tank output**: implement Dattorro's right tap set and make Width an M/S control on the real L/R tail (0–200%) | Tank output | Low | Low (check mono compatibility) | Fixes the mono tail, the largest single cause found |
| 2 | **Pre-delay** 0–250 ms, applied before the diffusers | Around | Trivial | None | Top perceptual size cue |
| 3 | **Tank modulation always on**: modulate both tank allpass1 (the Infinite code path already exists), 0.1–2 Hz, ≈ 0.5 ms | Tank | Low | Low–medium (pitch wobble on piano; must re-check shimmer-loop stability) | De-rings modes, opens the tail |
| 4 | **Decay as RT60 (seconds)**: map the user value to g = 10^(−3·L/(fs·RT60)) | Tank | Low | Low (freeze/infinite overrides must still work) | Predictable decay; required for item 5 |
| 5 | **Size** 0.5–2.0×: scale all diffuser, tank and tap lengths; allocate buffers for the maximum; keep RT60 fixed when size changes | Tank | Medium | Medium (zipper/pitch artifacts when moved live; smooth it or crossfade) | ValhallaPlate-style chamber/hall character, higher modal density |
| 6 | **Multiband decay**: low multiplier (0.5–2.5×, crossover 100–500 Hz) + high damping (2–8 kHz), scaled per segment (Jot rule) | Tank | Medium | Medium (keep low gain < 1; re-run freeze boundedness test) | Long low end is how halls and cathedrals read as large |
| 7 | **Wet EQ**: low cut 20–300 Hz, high cut 2–20 kHz on the wet output | Around | Trivial | None | Mix control; also addresses the separate "dull" agenda item |
| 8 | **Early reflections**: stereo multitap, 8–16 taps over 5–80 ms, different L/R sets, level control | Around | Medium | Low | Source width, sense of room shape |
| 9 | **Stereo input**: feed L and R into separate tank halves instead of the mono sum | Tank input | Low | Low | Preserves source stereo image |
| 10 | **Second engine (FDN "Hall")**: an 8–16 line Householder FDN with Jot absorption, Dattorro kept as "Plate" | New engine | High | High | Highest ceiling, only if the scaled plate is not enough |

**Recommended first slice:** items 1 + 2 + 7. They are small, low-risk, outside the shimmer feedback math (except the tap set), and target the strongest perceptual cues. Ear-test before continuing. Next slice: 3 + 4 + 5. Last: 6 and 8. Item 10 only if needed.

### Constraints carried over from earlier work

- The shimmer path recirculates through the tank. Any tank change (modulation, size, multiband decay) must re-run the 3-minute freeze-boundedness and decay-to-silence tests.
- The `shimmerWeight`/`plainWeight` split is load-bearing for stability; do not touch it as part of this work.
- Presets (Room/Hall/Cathedral/Plate) only make sense after items 2, 4, 5 and 6 exist, because those are the parameters presets differ on.

## 8. Slice 1 plan — stereo tail, pre-delay, wet EQ (one PR)

Branch: `feat/reverb-stereo-space` (stacked on `fix/loop-capture-rearm`). Target size is under 500 changed lines, including tests.

### 8.1 True stereo tank output

`DattorroTank` gets Dattorro's right-channel tap set (paper constants at 29761 Hz, converted to ms like the left set). The left set is unchanged, so the left output stays bit-identical.

| Sign | Samples @29761 | ms | Node (paper) | Tank element |
|---|---|---|---|---|
| + | 353 | 11.8612 | 24_30 | A.delay1 |
| + | 3627 | 121.8709 | 24_30 | A.delay1 |
| − | 1228 | 41.2620 | 31_33 | A.allpass2 |
| + | 2673 | 89.8155 | 33_39 | A.delay2 |
| − | 2111 | 70.9318 | 48_54 | B.delay1 |
| − | 335 | 11.2563 | 55_59 | B.allpass2 |
| − | 121 | 4.0657 | 59_63 | B.delay2 |

The node mapping was checked against our left set: 266 → `B.delay1` (8.9366 ms), 1990 → `A.delay1` (66.8646 ms), and so on. Every right tap fits inside its line's length.

- **Mono feedback stays mono.** `processSample()` keeps returning the left output, so existing callers are unchanged. A new `getLastRightOutput()` (or a stereo variant) exposes the right sum, computed in the same sample with the same 0.6 scale.
- **Shared level control.** Infinite's level controller and the freeze leveler must apply the same gain to L and R, with detection kept on the existing signal so behaviour doesn't change.

### 8.2 Width becomes a real M/S control

- `width` changes range from 0–1 (default 0.15) to **0–2 (0–200%), default 1.0** (natural Dattorro stereo). The parameter ID is unchanged.
- Wet stereo is built as follows:
  - `mid = 0.5·(L+R)`
  - `side = 0.5·(L−R) + shimmerSide`
  - `L = mid + width·side`, `R = mid − width·side`
- `shimmerSide` keeps its old formula, but with the old default width constant (0.15) baked in. At width = 1 the shimmer side contribution therefore equals the old default exactly.
- Width = 0 still gives an exactly mono wet signal, so the existing mono-collapse test keeps holding.

### 8.3 Pre-delay

- New parameter `preDelay`: 0–250 ms, default 0 ms, so existing behaviour is unchanged by default.
- It is a mono delay line on the tank input, placed before the `(1 − freeze)` input gate, with per-sample smoothing so live changes glide instead of clicking.

### 8.4 Wet EQ

- New parameter `lowCut`: 20–1000 Hz, default 20.
- New parameter `highCut`: 1–20 kHz, default 20 kHz.
- Each is a 2nd-order TPT filter per channel on the final wet signal, after loop capture and before the dry/wet mix.
- Both filters always run so their state stays warm. Their output is used only away from the endpoints, which keeps default behaviour bit-identical. Resetting a filter on activation instead was measured to click (32.5× the steady-state sample delta at 96 kHz).

### 8.5 Editor

Three new rotary knobs (Pre-Delay, Low Cut, High Cut) with `SliderAttachment`. Grow the fixed editor size only as much as the layout needs.

### 8.6 Tests (written first, Strict TDD)

1. Tank: impulse response L ≠ R, with normalized L/R correlation over the first 2 s below 0.5. The left output is bit-identical to a reference run of the pre-change tap set.
2. Engine: width = 0 gives L == R; width = 1 with shimmer amount 0 gives L ≠ R; width = 2 gives side RMS > width = 1 side RMS.
3. Pre-delay: the first non-silent wet sample is about 100 ms later at 100 ms than at 0 ms (± diffuser latency tolerance).
4. Wet EQ: high cut at 2 kHz reduces wet energy above 8 kHz by more than 12 dB. Endpoints are bit-identical to the filters being absent.
5. The existing 3-minute freeze-boundedness, decay-to-silence and full `MilleniaTests` suite stay green.

## 9. Slice 2 plan: modulation, decay in seconds, Size

Branch: `feat/reverb-size` (stacked on `feat/reverb-stereo-space`). Covers section 7 items 3, 4 and 5.

**Order is fixed:** decay (RT60) first, then Size, then modulation. Size needs RT60 to stay constant while the tank grows, and modulation is easiest to judge once size and decay are settled.

### 9.1 Decay as RT60 (seconds)

- Replace `feedback` (raw gain 0–0.85) with a new `decay` parameter: 0.3–20 s, log skew, default 7.0 s. At Size 1 this default reproduces today's `g ≈ 0.7`.
- Today the decay gain is applied twice per figure-eight loop (once per branch). Each application therefore covers half the loop, `L_half ≈ 362 ms × size`, which gives `g = 10^(−3·L_half/RT60)`.
  - Check: RT60 = 7.0 s at size 1 gives g = 0.700, consistent with the earlier estimate.
- **Stability ceiling stays:** g is clamped to ≤ 0.85, the value already proven with the shimmer path in the loop. The RT60 that is actually reachable therefore depends on Size:
  - about 7.6 s at Size 0.5
  - about 15 s at Size 1
  - about 30 s at Size 2

  Requests above the ceiling clamp silently. This is documented in the parameter comment and in the editor tooltip.
- **Freeze and Infinite** keep overriding the gain exactly as today.
- **The figure is nominal:** the shimmer path adds its own loss (documented comb-filter energy loss), so RT60 is the tank's value, not a guarantee for the full chain.

### 9.2 Size (0.5–2.0×)

- New `size` parameter, 0.5–2.0, default 1.0.
- It scales every length that defines the space: the 4 tank allpasses, the 4 tank delays and all 14 output taps.
- The **input diffusers stay unscaled.** They set the onset texture, not the room dimensions. This keeps the early transient tight, which is the Valhalla/Surge practice.
- Buffers are allocated once in `prepare()` for 2.0× (+ modulation headroom). Nothing is allocated when Size changes.
- Smoothing is **per sample, about 300 ms**. Moving the knob therefore produces a short pitch glide, the classic tape/"Doppler" artifact of algorithmic reverbs. **Decided 2026-10-11 (user): smooth glide.** Crossfading a second tank and applying the change on knob release were both rejected.
- **RT60 is held constant across Size**, because g is recomputed from `L_half × size` (section 9.1).
- **Modal density:** Size 2 doubles the total delay length (about 1.45 s), so the M ≥ 0.15·T60·fs rule holds up to about 9.6 s RT60, instead of the current about 4.8 s.

### 9.3 Always-on modulation

- New `modulation` parameter, 0–1, default 0.3. It maps to a depth of 0–1 ms on both tank `allpass1` lines. That reuses the existing Infinite code path: Thiran interpolation, LFOs at 0.5/0.7 Hz with a 90° offset.
- Effective depth is `max(userDepth, infiniteAmount × 0.5 ms)`, so Infinite behaves as before at modulation 0.
- **Risk:** the in-loop `PitchShifter` alignment search is already sensitive (the known pitch-oscillation issue). Modulated tank content could change how often its anchor flips.
  - Measure this with the existing offset-stability diagnostics at modulation 0 vs 0.3 vs 1.0 before choosing the default.
  - If it degrades, cap the depth or modulate only the output-side lines.

### 9.4 Editor

- Feedback becomes **Decay**, labelled in seconds.
- Add **Size** and **Modulation** knobs.

### 9.5 Tests (written first)

1. **RT60 mapping:** measured decay of the bare tank at Size 1 and 2, RT60 2 s and 5 s, within ±15%.
2. **RT60 independent of Size:** the measured RT60 at Size 0.5 and at Size 2 differs by ≤ 15% for the same setting below the ceiling.
3. **Ceiling:** g never exceeds 0.85 for any decay/size combination.
4. **Size changes the space:** the first output-tap arrival scales with Size (onset ratio ≈ 2 between Size 1 and 2). No allocation happens on Size changes, and Size automation stays bounded (no NaN or overload).
5. **Modulation:** depth 0 is bit-identical to the slice 1 left/right fingerprints, and modulated output stays bounded.
6. **Stability:** the 3-minute freeze-boundedness and decay-to-silence tests pass at Size 2 + modulation 1 + RT60 at its ceiling.
7. **Pitch-shifter offset stability** at modulation 0 / 0.3 / 1.0 is logged for a decision. This is a diagnostic, not an assertion.

### 9.6 PR size

- Estimate: about 900–1100 changed lines including tests, similar to slice 1.
- If you want smaller PRs, it splits cleanly into two:
  - **PR A:** RT60 + Size, which are coupled.
  - **PR B:** modulation, which is independent.

### 9.7 Implementation notes (2026-10-11)

Built as planned, with these deviations and findings:

- **RT60 formula kept, with a measured bias.** `L_half` is derived from the tank constants (A 357.69 ms, B 367.71 ms, mean 362.70 ms), and the gain really is applied twice per loop (B→A and A→B cross-feeds). The bare tank's Schroeder-measured RT60 still runs **10–14% long** (2 s → 2.25–2.27 s, 5 s → 5.52–5.61 s). That is within the ±15% test, and it has a known cause: a Schroeder allpass stores energy, so its energy-weighted delay is `M/(1−g²)` (1.96·M for the 0.7 allpasses, 1.33·M for the 0.5 ones), giving an effective half loop of about 413 ms (+14%). Using 413 ms in the formula would remove the bias, but it would move the 7 s default to g ≈ 0.666. That is left as a user decision.
- **The 0.85 ceiling is unreachable at Size 2 from the UI.** At Size 2 it would need about 30.8 s, beyond Decay's 20 s maximum, so the strongest reachable gain there is g ≈ 0.778. Above Size ≈ 1.3 the 20 s knob limit binds before the ceiling does. The engine stress tests use Decay 20 s. The bare-tank stress tests drive g = 0.85 at Size 2 directly.
- **Modulation smoothing.** Modulation is ramped per sample inside the tank (50 ms), like Size (300 ms, linear), so the processor passes both raw.

## 10. Open questions

- Exact Dattorro right-channel tap constants: verify against the paper's table before implementing item 1.
- Signalsmith example repo license: unverified.
- Whether live Size changes should be smooth (pitch-glide artifacts) or click-free crossfaded (heavier). Decide at design time.
