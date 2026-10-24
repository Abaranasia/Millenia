# Shimmer Character Research — Why Millenia Never Sounded Like a Shimmer

Status: research written 2026-10-24. No DSP code changed. Agenda item 2 (item 1 was [`reverb-space-research.md`](./reverb-space-research.md)).
Scope: the shimmer path. That covers `PitchShifter`, `FormantEnvelopeCorrector`, the shimmer blend inside `DattorroTank`, and the parameters that drive them. The reverb core is covered by item 1.

Line numbers are as of 2026-10-24. Comments in `Source/` are being edited at the same time, so lines may drift. Symbol names are stable.

## 1. Problem

The user says the effect "never sounded like a shimmer". The project history shows a recurring loop:

1. The octave was too audible, and the user reported "chipmunk" or "HF noise wall".
2. The shifted share of the loop was cut, or the octave was corrected toward the dry timbre.
3. The shimmer became quiet or short-lived.

This doc maps the current shimmer path, compares it with how classic shimmers are built, and ranks the causes.

## 2. Current design (facts, 2026-10-24)

### 2.1 Signal path, per sample (`ShimmerReverbEngine::process()`)

```
tank.feedbackFromB (end of branch B, after damping)            ShimmerReverbEngine.cpp:372
  -> PitchShifter (+12 st default, 20 ms WSOLA grains)            :373
  -> FormantEnvelopeCorrector (LPC: re-imposes the DRY envelope)  :408
  -> unity-shifter crossfade (Freeze only)                        :417
  -> DCBlocker (20 Hz) -> SafetyLimiter (unity below 0.8)         :427, :438
  -> DattorroTank::processSample(input, safeFeedback)             :473
       inputToA = diffused + g * (plainWeight*feedbackFromB + shimmerWeight*shifted)
                                                                  DattorroTank.cpp:395-399
```

- **Placement.** The shifter sits in the loop (in-tank). The shifted signal replaces part of the branch-B to branch-A cross-feed, after the input diffusers. This is the classic topology and is not the problem.
- **Feed-forward side term.** A second, raw shifted copy reaches the output directly as a side-only signal: `0.15 · amount · sustain · 0.5 · (primary − quadrature)` (`ShimmerReverbEngine.cpp:566-567`). It cancels in mono.

### 2.2 Gain staging of the shifted path

| Item | Value | Evidence |
|---|---|---|
| `shimmerWeight` (share of the cross-feed that is shifted) | `shimmerAmount × maxShimmerBlendWeight` | `DattorroTank.cpp:395-398` |
| `maxShimmerBlendWeight` | `1 − Shimmer Sustain` (**inverted**) | `ShimmerReverbEngine.cpp:167` |
| Defaults: Amount 1.0, Sustain 0.85 | **shimmerWeight = 0.15, plainWeight = 0.85** | `Parameters.cpp:65-89` |
| Sustain = 1.0 (knob fully up) | shimmerWeight = 0: **no shifted signal recirculates** | same |
| Blend law | Linear: `plain + shimmer = 1` (amplitude, not energy) | `DattorroTank.cpp:398` |
| Decay gain g | 0.6993 at 7 s / Size 1, ceiling 0.85 | `DattorroTank.h:142`, journal slice 2 |
| Frozen cap | 0.01 | `DattorroTank.h:524` |

History of the cap:
- **0.5** on 2026-08-18.
- **0.85** on 2026-08-21, when the user said it "sounds much longer now".
- **Made user-controllable and inverted** on 2026-09-06. The reason was that a higher blend measurably decayed faster. The new default of 0.85 means an effective cap of **0.15**. See `shimmer-reverb-implementation-plan.md:421-424`.

So the default now recirculates less than a fifth of the shifted share that the Phase 8 closing ear test approved.

### 2.3 Shifter and filtering in the shifted path

| Item | Value | Evidence |
|---|---|---|
| Algorithm | SOLA/WSOLA grain pool: 20 ms grains, 10% (2 ms) splices, normalized-correlation alignment search (±22 samples, two anchors), quadrature pool for stereo | `PitchShifter.h:265, 282`; `formant-preserving-pitch-shifter-research.md` §11 |
| Base delay | 4 grains ≈ 80 ms, so the shifted copy lags the plain copy by about 80 ms | `PitchShifter.h:303` |
| Known artifact | Pitch "oscillation" at ±12 st. The alignment search pins to the window edge on 99.6% of launches on a tonal drone. Closed as a permanent limitation after 7 reverted fixes | formant doc §11 |
| Formant correction | Order-10 LPC, 40 ms window, 20 ms hop. Pulls the shifted envelope back to the dry envelope every pass, gain-normalized | `FormantEnvelopeCorrector.h:151-158` |
| Anti-alias / tone filter before or after the shift | **None.** Lagrange-3 reads at ratio 2 fold everything above fs/4 (11 kHz at 44.1 kHz) | grep of `PitchShifter.*` |
| In-loop HF loss | Damping default 0.0005, which puts the cutoff above Nyquist. Wet High Cut default 20 kHz is output-only | `Parameters.cpp:115-119, 152-155` |
| Interval | One voice, continuous −24..+24, default +12. No +7/+19 voice | `Parameters.cpp:13-17` |

## 3. Quantifying the cascade at defaults

**Model.** One figure-eight loop is about 725 ms at Size 1. The cross-feed gain g is applied twice per loop. The shifted copy (+12 st) is assumed spectrally disjoint from the plain copy, so the two are uncorrelated and their energies add. Per loop:

- Each generation keeps `(g²·plainWeight)²` of its energy.
- Each generation passes `(g²·shimmerWeight)²` up one octave.

The model ignores shifter and corrector losses and the extra 80 ms on the shifted branch. It is optimistic for the octaves. The script is reproducible from this description.

| Blend | t ≈ 0.7 s | 1.5 s | 2.2 s | 3.6 s | Predicted RT60 (Decay = 7 s) |
|---|---|---|---|---|---|
| **Current: linear, shimmerWeight 0.15** | octave −15.1 dB | −12.1 | −10.3 | −8.1 | **5.8 s** |
| Linear, 0.5 (Phase 8 era) | 0.0 | +3.0 | +4.8 | +7.0 | 4.7 s |
| Equal-power, 0.5 | −4.8 | −1.8 | 0.0 | +2.2 | **7.0 s** |
| Equal-power, 0.7 | −0.2 | +2.8 | +4.6 | +6.8 | **7.0 s** |

Octave levels are relative to the fundamental. With Decay 7 s, the whole tail is down 37 dB by 3.6 s.

**Reading.** At defaults the first octave starts 15 dB under the fundamental. It is still 8 dB under when the tail is already −37 dB, and the second octave never gets above −20 dB. That is a reverb with a faint octave haze, not a shimmer. The Eno/Lanois recipe of +12 with feedback ≥ 0.5 puts the octave at parity within the first loop.

**Cross-check against a measurement.** The full-engine RT60 at Decay 7 s measured **6.20 s** (journal, slice 2). The bare tank runs 10–14% long, which gives about 7.8 s. The ratio 6.20 / 7.8 ≈ 0.80 matches the model's 5.8 / 7.0 ≈ 0.83.

The reason "more shimmer decays faster" (`ShimmerReverbEngine.h:128-140`) is therefore mostly **linear-crossfade energy loss**. Two uncorrelated signals mixed with weights that sum to 1 keep only `plainWeight² + shimmerWeight²` of their energy, which is 0.745 at 0.15 and 0.5 at 0.5. Comb filtering is not the main cause at +12 st. Comb filtering is real at 0 st, where the Freeze unity path makes the two copies correlated, and that case is where it was measured. This is a hypothesis, and test T2 in section 7 confirms or refutes it.

## 4. How classic shimmers are built

| Design | Shift placement | Feedback / regeneration | HF handling | Shifter |
|---|---|---|---|---|
| Eno/Lanois (H910/H949/H3000 + Lexicon/AMS, 1984) | Shifter in a feedback loop around a long reverb | Loop gain, EQ and delay lengths shape it, "steel drum plucks to slow string pads" | EQ in the loop | Hardware dual-splice pitch shifter |
| Eventide ShimmerVerb | Pitch shift + delay fed back into the **reverb input** | Feedback control, with a low/mid/high crossover choosing **which bands** are fed back | Band-selective feedback | Eventide shifter |
| ValhallaShimmer | Shifter **inside** a feedback network of diffusor delays | +12 with Feedback ≥ 0.5 = the classic sound. Shift is only audible with feedback > 0 | "Dark" color mode removes **almost all aliasing noise in the feedback path** | Granular with **randomized** grains, to avoid the comb artifacts of simple shifters. Reverse-grain modes are smoother (organ); forward modes are noisier (orchestral) |
| Strymon BigSky Shimmer | Two voices generated **from the reverberated signal** (regenerating). An "Input" mode gives a non-regenerating variant | Amount, Mode | Tone knob; Mod also modulates the shimmer voices | Not documented |
| LushVerb ([reference](./reference/lushverb-shimmer-reverb.md)) | **Pre-tank only**, no regeneration | n/a | n/a | Fixed ×2 |
| CCRMA (Zhang 2018) | +12 and +24 branches pre-reverb, then a master FDN | No cascading | n/a | Phase vocoder |
| Mutable Clouds `pitch_shifter.h` (MIT) | Building block | n/a | None explicit | **Two taps half a grain apart**, triangular crossfade summing to 1, grain 128–2047 samples (≈4–64 ms at 32 kHz) |

Sources: [Valhalla, Eno/Lanois shimmer](https://valhalladsp.com/2010/05/11/enolanois-shimmer-sound-how-it-is-made/), [ValhallaShimmer controls](https://valhalladsp.com/?p=553), [ValhallaShimmer: shimmering tips](https://valhalladsp.com/2010/12/02/valhallashimmer-tips-and-tricks-shimmering/), [Introducing ValhallaShimmer](https://valhalladsp.com/2010/08/30/introducing-valhallashimmer/), [Eventide ShimmerVerb](https://www.premierguitar.com/eventide-releases-the-shimmerverb-plug-in), [Strymon "Falling Angel" preset notes](https://www.strymon.net/this-weeks-preset-bigsky-falling-angel/), [Clouds pitch_shifter.h](https://github.com/pichenettes/eurorack/blob/master/clouds/dsp/fx/pitch_shifter.h).

Valhalla pages were read through search summaries; direct fetches returned HTTP 403. The Strymon controls come from a preset write-up, not the manual.

### What the classic designs agree on

1. **The shifted feedback is large.** About 0.5 or more of the loop, so the octave reaches parity with the fundamental within roughly one loop. Ours is 0.15.
2. **The feedback path is band-limited**, through a dark mode, a tone control or crossover-selected bands. This removes aliasing and makes the upper generations roll off naturally after 2–3 octaves, instead of climbing into hiss. We have no band-limiting in the shifted path, and the Phase 8 "wall of HF noise" report is exactly that failure.
3. **Nobody formant-corrects.** The brightening octave is the effect. A corrector that pulls each pass back to the dry envelope works against the shimmer.
4. **Simple shifters are fine, even preferred.** Splice and grain artifacts are re-diffused on every pass and read as "shimmer". Valhalla randomizes grain timing to avoid comb coloration rather than chasing waveform-aligned splices. Its reverse-grain mode trades motion for smoothness.
5. **Diffusion around the shifter.** Valhalla's loop contains diffusors, and our in-tank placement already gives that. The raw feed-forward copy is what "a pitch shifter in parallel" sounds like. Our side term is exactly that, and the user described it in those words on 2026-08-16.

### Open-source references (ideas only from GPL)

| Project | License | Idea |
|---|---|---|
| [Mutable Clouds](https://github.com/pichenettes/eurorack) `pitch_shifter.h`, `reverb.h` | MIT | Minimal two-tap shifter with complementary triangular gains; compact Dattorro variant |
| [thedrgreenthumb/faust shimmer.dsp](https://github.com/thedrgreenthumb/faust/blob/master/shimmer.dsp) | MIT | ValhallaShimmer-style allpass feedback network with `transpose()` (two crossfaded taps) |
| [Elysiera](https://github.com/azur1s/elysiera) | GPL-3.0, ideas only | Pre-filtered (LP/HP) input, two quadrature-modulated shifters, zita-rev1 tank, feedback around it |
| [Parasites "Oliverb"](https://github.com/mqtthiqs/parasites) | Unverified, treat as GPL | Pitch shifter in the reverb loop, Lexicon-480L-style tank |
| Faust `reverbs.lib` (`jpverb`, `greyhole`) | LGPL with exception | Modulated diffuser loops |

See also [`shimmer-reverb-open-source-survey.md`](./shimmer-reverb-open-source-survey.md) and [`fdn-shimmer-reverb-research.md`](./fdn-shimmer-reverb-research.md) §9, which already found Valhalla's "randomized grains" technique.

## 5. Likely root causes, ranked

| # | Cause | Confidence | Evidence |
|---|---|---|---|
| 1 | **The shifted share of the loop is tiny at defaults** (0.15). The octave never catches up with the fundamental before the tail dies. With Sustain fully up there is **no cascade at all**, so the knob's top end removes the shimmer | **High** | Code (§2.2) + model (§3) + history (0.85 cap was "much longer"; inversion 2026-09-06) |
| 2 | **The linear blend loses energy** (`plainWeight² + shimmerWeight²`), so more shimmer shortens the tail. This drove the cap down and the knob inverted. It is misattributed to comb filtering | **Medium-high** | Model predicts a 0.83 RT60 ratio; measured 0.80 (6.20 s vs ≈ 7.8 s bare) |
| 3 | **No band-limiting in the shifted path.** Aliasing above fs/4 and unbounded upper generations made a large shimmer share sound like hiss ("wall of HF noise", 2026-08-18), which pushed the share down | **Medium** | No filter in the path (§2.3); Valhalla's dark mode exists for exactly this |
| 4 | **Formant correction is anti-shimmer.** It re-imposes the unshifted envelope on the octave every pass, removing the brightness lift that identifies a shimmer. It also measurably narrows the stereo image (≈ 50% side retention) | **Medium**: the mechanism is certain, the perceptual weight is not measured | `FormantEnvelopeCorrector.h:7-29`; formant doc §13 |
| 5 | **What is audible as shimmer is mostly the raw feed-forward side term**, the "parallel pitch shifter" sound. It vanishes in mono and gets louder as Sustain goes up, while the cascade gets weaker | **Medium** | `ShimmerReverbEngine.cpp:566-567`; ear report 2026-08-16; Sustain round 2 notes |
| 6 | **Shifter character.** Short 20 ms grains and correlation-aligned splices give the documented ±12 st pitch oscillation and the "grainy/low-bitrate" reports. This is a quality issue, not why the shimmer is absent | **Low-medium** | Formant doc §11; ear reports 2026-08-21 |
| 7 | Interactions with Modulation (0.1 default) and Size. They affect shifter offset stability but not the shimmer's presence | Low | Journal slice 2 |

## 6. Ranked changes

| # | Change | Where | Effort | Risk |
|---|---|---|---|---|
| 1 | **Energy-preserving blend** for shifted feedback: `plainWeight = √(1 − shimmerWeight²)` when \|shift\| ≥ 1 st. Blend back to today's linear law as the shift goes toward 0 st, because the copies are correlated there and equal-power could exceed unity | `DattorroTank::processSample` (needs the shift amount) | Low | Medium: rerun every stability test; harmonic input at +12 overlaps partially (2nd harmonic vs shifted fundamental) |
| 2 | **Restore a large shifted share and un-invert Sustain.** Once #1 removes the energy loss, the inversion has no reason to exist. Target default shimmerWeight ≈ 0.5–0.6, user tunes | Engine mapping + parameter default | Trivial | Low |
| 3 | **Shimmer Tone: a low-pass on the signal entering the shifter**, default ≈ 5 kHz, range 1.5–10 kHz. It is also the anti-alias filter: content stays at or below 2·fc after the shift, so it stays under Nyquist at 44.1 kHz and above when fc ≤ fs/4. Optional fixed high-pass ≈ 80–150 Hz | Engine, before `shifter.processSample` | Low | Low (one-pole or TPT, no latency) |
| 4 | **Formant correction off by default** (bypass in the shimmer loop), kept behind a toggle for A/B | Engine | Trivial | Low: a bit-identical bypass already exists at ≤ 0 st |
| 5 | Shimmer side term: drop it, or tie it to Width only, once the cascade carries the shimmer | Engine | Trivial | Low |
| 6 | Shifter rework: **randomized grain timing** (Valhalla idea) and/or longer grains (40–80 ms), or a plain two-tap shifter without the correlation search (Clouds idea), plus an optional reverse-grain "smooth" mode | `PitchShifter` | Medium–high | Medium: retunes everything downstream |
| 7 | Second voice (+7 or +19) or "Dual" mode | Engine | Medium | Medium (CPU, gain) |
| 8 | Band-selective feedback (Eventide crossover) or a shimmer envelope (backlog ADSR) | Engine | Medium | Low |

## 7. Proposed slice 1 — "make the cascade audible" (one PR)

Branch: `feat/shimmer-cascade`. Covers section 6 items 1–4. Target: under about 600 changed lines including tests.

Each sub-change gets its own commit, so the user can build and ear-test each step: A = today, B = A + blend and mapping, C = B + Shimmer Tone, D = C + formant off.

### 7.1 Changes

1. **Blend law.**
   - `DattorroTank` gets `setShimmerBlendLaw(float equalPowerAmount)`.
   - The engine derives the amount from `pitchShiftSemitones`: 0 at \|st\| ≤ 0.5, 1 at \|st\| ≥ 1, linear between.
   - `plainWeight = lerp(1 − shimmerWeight, √(1 − shimmerWeight²), equalPowerAmount)`.
   - Freeze keeps its 0.01 cap, so the frozen behaviour is effectively unchanged. Verify this; do not assume it.
2. **Mapping.**
   - `maxShimmerBlendWeight = Sustain`, no longer `1 − Sustain`.
   - Sustain is renamed "Shimmer Feedback" in the editor. The parameter ID stays, since nothing has shipped.
   - Default 0.55, so shimmerWeight = 0.55 at Amount 1. This is a starting point for the user's tuning.
   - The side term's `shimmerSustainAmount` gate keeps its value.
3. **Shimmer Tone.**
   - New parameter `shimmerTone`: 1.5–10 kHz, log skew, default 5 kHz.
   - It is a 2nd-order TPT low-pass on `dryFeedback` before **both** `shifter` and the corrector's dry analysis. It is not applied to `unityShifter`, so Freeze stays untouched.
4. **Formant correction.**
   - New bool parameter `formantCorrection`, default **off**.
   - When off, the shifted signal bypasses `FormantEnvelopeCorrector`. The corrector keeps running so its state stays warm, the same pattern as the wet EQ.

### 7.2 Tests (written first)

| # | Test | Assertion |
|---|---|---|
| T1 | **Cascade emergence.** 220 Hz, 0.5 s burst, defaults. Goertzel at 220/440/880 Hz in 200 ms windows at t = 1, 2, 3 s | At t = 1.5 s, (440 − 220) improves by ≥ 9 dB over a pinned legacy run. Absolute levels are logged for the user |
| T2 | **Energy-neutral blend.** Full-engine RT60 at +12 st, Decay 5 s, shimmerWeight 0 / 0.5 / 0.85 | Within ±10% of each other. Today it drops with the blend. Also tests the hypothesis in §3 |
| T3 | **Anti-alias.** 15 kHz sine into the engine at 44.1 kHz, +12 | Energy at the alias (14.1 kHz) in the wet output is more than 40 dB below the input. Log the same figure with Tone at maximum |
| T4 | **Near-unity guard.** Pitch 0 st and ±0.5 st, maximum shimmer, 180 s noise | Bounded (≤ 10), and decays to silence |
| T5 | **Harmonic stress.** Sawtooth chord, +12, Shimmer Feedback 1.0, Decay 20 s, Size 2, Modulation 1, 180 s | Bounded; freeze-boundedness and decay-to-silence still pass |
| T6 | **Toggle bit-identity.** Formant on + Tone at max + legacy mapping | Bit-identical to pre-change golden hashes |
| T7 | Existing suite | Green. Golden default hashes change by design and are recorded |

### 7.3 A/B ear-test plan (user)

- **Material:**
  - a sustained pad chord;
  - a single piano or guitar note around C4 and C5;
  - a sparse arpeggio.
- **Settings:** Mix 100%, Decay 7 s, Size 1, Width 1, Modulation 0.1.
- **Order:** builds A → B → C → D. Change nothing else between builds.

| Step | Listen for | Call to make |
|---|---|---|
| A → B | An ascending octave clearly present within about 1 s, and a tail as long as A's | Shimmer Feedback default (sweep 0.3 / 0.55 / 0.8) |
| B → C | Hiss or "wall of noise" gone; upper octaves fade after 2–3 generations | Tone default (sweep 3 / 5 / 8 kHz) |
| C → D | Brighter, more "angelic" octave; check whether high notes now read as "chipmunk" | Keep formant off or on |
| Mono check (Width 0) | The shimmer must survive in mono. Today's side term vanishes | — |

## 8. Decisions for the user

1. **Knob semantics.** Recommended: un-invert Sustain and rename it "Shimmer Feedback" (regeneration), keeping Amount as the master. Alternative: one "Shimmer" knob that sets shimmerWeight directly, with Amount removed.
2. **Formant correction.** Recommended: default off, kept as a toggle. Alternatives: remove it from the loop entirely, or keep it on. It cost a lot of work, but its job (undo the octave's timbre) conflicts with the effect's identity.
3. **Equal-power blend near 0 st.** The proposed fallback to linear near unity shift trades a slightly weaker loop at small intervals for guaranteed stability.
4. **Architectural fork (not in slice 1):**
   - (a) Keep the in-tank injection (recommended; it is the Valhalla-like topology and already diffuses the shift).
   - (b) Add an outer loop around the whole reverb (Eno/Eventide style).
   - (c) Use a pre-tank, non-regenerating shimmer as an extra mode (Strymon "Input" mode, LushVerb).
5. **Shifter rework (slice 2 candidate):**
   - Randomized grain timing in the existing grain pool.
   - A simple two-tap shifter without correlation search, which may also close the ±12 st pitch-oscillation limitation.
   - Keep as is.

## 9. Constraints carried over

- Any loop change reruns the 3-minute freeze-boundedness, decay-to-silence and engine stress tests (`reverb-space-research.md` §7).
- The Freeze unity crossfade and `frozenMaxShimmerBlendWeight = 0.01` stay untouched. The Tone filter must not sit on the unity path.
- Borrow ideas only from GPL sources; MIT code (Clouds, thedrgreenthumb) may be studied, but write our own.

## 10. Open questions

- The ValhallaShimmer manual (PDF) could not be text-extracted in this pass. Its exact Feedback scale and whether the low/high cut sits inside the loop are unverified.
- The BigSky Shimmer manual was not read directly.
- §3's model ignores the shifter's own per-pass gain and the corrector's spectral loss. T1 and T2 give the real numbers.
