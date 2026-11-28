# Project audit and shimmer comparison (2026-11-14)

Overall check of Millenia at `f694d79` (`feat/shimmer-cascade`), compared against how classic shimmer reverbs are built, with a prioritized list of fixes and improvements.

## How to read this

- **Scope:** read-only. Nothing was built, run, measured or changed.
- **Confirmed** means the item was re-read directly in the code after the audit. Only fix 1 has that status.
- **Reported** means it comes from the code audit and has not been re-checked. Verify before acting on it.
- **Inferred** means it is a reasoned guess with no measurement behind it.
- Line numbers are as of `f694d79`.

## 1. Verdict

The core design is right: the pitch shifter sits inside the reverb loop, which is what every classic shimmer does. What holds the plugin back is one memory-safety bug, several host-integration gaps, and a shifter and tank that are simpler than the reference designs.

## 2. Comparison with the classic designs

| Aspect | Reference designs | Millenia | Verdict |
|---|---|---|---|
| Shifter placement | In the feedback loop (Eno/Lanois, Valhalla, Eventide) [1][2][4] | In the tank loop, tapped at the end of branch B | Matches |
| Loop filtering | Low-pass and high-pass inside the loop, compounding per pass [2][8] | Tone low-pass on the shifted path only; low/high cut sit after the loop | Partial |
| Shifter type | Time-domain splice; random grain timing gives the "orchestral" smear [2][10] | Correlation-aligned grains, fixed 18 ms hop, no randomisation | Too clean and periodic |
| Formant correction | Not used by any source found | Present, default off, unstable on tonal input | Correctly off |
| Voices | Two voices, often detuned about ±10 cents or up plus down [5][6] | One voice | Gap |
| Loop gain | Shifted and plain shares conserve total loop gain; soft limit on the whole loop [8] | Equal-power weights sum up to 1.41; limiter on the shifted path only | Risk in Infinite |
| Tank | Dense, true-stereo, heavily modulated [2][6][12] | Single Dattorro plate, mono-summed input, no early reflections | Explains "small" |
| Freeze | Mute input and lock out the shifters [4] | Mutes input and crossfades to a 0-semitone shifter | Matches |

## 3. Fixes, in priority order

| # | Severity | Status | Problem | Where |
|---|---|---|---|---|
| 1 | High | **Fixed** (`07c54d5`) | Buffer overrun: `process()` wrote the host block into `monoScratch`, sized to the `prepareToPlay` hint, with no guard. `process()` now splits oversized blocks into prepared-size chunks. | `ShimmerReverbEngine.cpp:60`, `:361-378`; `PluginProcessor.cpp:341-342` |
| 2 | Medium/high | **Fixed** (this branch) | `getTailLengthSeconds()` returned 0. It now reports a finite tail from Decay, Size and pre-delay, and infinity when Infinite, Freeze (>= 0.9) or Loop Mix (> 0.001) is on. Known gap: for Freeze between 0 and 0.9 the real tail can exceed the finite estimate. | `PluginProcessor.cpp:90-93`; `ShimmerReverbEngine::computeTailSeconds` |
| 3 | Medium | **Fixed** (this branch) | No `getBypassParameter()`. The processor now returns the `bypass` parameter; the VST3 wrapper then calls `processBlock()`, which already fades the mix to dry. Verified by build and JUCE source reading only, not in a host. | `PluginProcessor.cpp`; `ShimmerReverbEngine.cpp:786-797` |
| 4 | High (mitigated) | Reported | Formant corrector is unstable on tonal input: no LPC regularisation, unbounded gain ratio. Only mitigated by defaulting to off. | `FormantEnvelopeCorrector.cpp:111-129`, `:243` |
| 5 | Medium | Reported | No state migration: `shimmerSustain` → `shimmerFeedback` is unmapped and `schemaVersion` is written but never read. | `PluginProcessor.cpp:357-381`; `Parameters.cpp:76-77` |
| 6 | Medium | **Fixed** (this branch) | Block-stepped parameters (mix, width, shimmer amount/feedback, decay, damping, pitch, low/high cut, tone, freeze) gave zipper steps that depended on block size. The 50 ms smoothers now advance in 32-sample chunks inside the host block (`EngineParameterSmoothing`), pushing a setter only when its value changed. Verified by `EngineParameterSmoothingTests`. | `Source/EngineParameterSmoothing.h`; `PluginProcessor.cpp` processBlock |
| 7 | Medium | Reported | Pitch changes apply to live grains at once, jumping the read position. | `PitchShifter.cpp:88-99` |
| 8 | Medium | **Fixed** (hold-level rider, this branch) | Infinite plus cascade: the equal-power blend stays on at loop gain 1, held only by the level rider and limiter. Measured: quiet sources swelled +8 to +13 dB, small shifts died. A latched-target, two-sided hold-level rider in `DattorroTank` now keeps the tail within +-2 dB (+-3 dB for +12 st chord) of the level it was played at, over 60 s, in 7 tank and 9 engine cases (see `reverb-dev-journal.md`). | `DattorroTank.cpp` `updateHoldRegulator()`, `processSample()`; `DattorroTank.h` hold* constants |
| 9 | Medium | Reported | Tone knob dead zone: at +12 st and 44.1 kHz the cap is 5512 Hz, so 5.5–10 kHz does nothing. "Off" also removes the alias cap. | `ShimmerReverbEngine.cpp:247-255`, `:450`, `:487` |
| 10 | Low | Reported | Damping coefficient is not sample-rate compensated. | `DattorroTank.cpp:278` |

Suggested first step: fix 1 on its own, test-first, by chunking the host block in the processor.

## 4. Improvements by feature

### Shimmer and pitch shifter
- Randomise grain timing to get the sideband smear the references rely on [2][10].
- Add a second voice: slight detune, a fifth, or up plus down [5][6].
- Remove or diffuse the feed-forward side term. It bypasses the tank, cancels in mono and doubles at Width 2 (`ShimmerReverbEngine.cpp:704-721`).
- Lengthen grains for low notes; 20 ms is short below roughly 50 Hz.
- At Amount × Feedback = 1 the plain share is 0, so the tail collapses into the Tone filter. Consider a floor on the plain share.

### Tank and space
- Keep the input stereo instead of mono-summing it.
- Add early reflections.
- Scale the input diffusers with Size.
- Modulate more of the delays, at different rates and depths [11].

### Tone ("dull")
Still unresearched. Measure before changing anything. Candidates (inferred):
- About 30% of loop energy per pass goes through a 4th-order low-pass at 5.5 kHz or below.
- Lagrange3rd fractional reads on every pass in the shifter, in the tank lines when Size ≠ 1, and on the output taps (`DattorroTank.cpp:26-28`).
- No early reflections.

### Decay
- The 0.85 gain clamp caps real RT60 at about 15.4 s at Size 1 and 7.7 s at Size 0.5.
- The engine measured 6.2 s for a nominal 7 s. Calibrate or relabel.

### Parameters
- Express Damping in Hz or percent; it is a raw coefficient with a default of 0.0005 on a 0–0.3 range (`Parameters.cpp:120-124`).
- Add host-visible units (`.withLabel`); only `shimmerTone` has value-to-text.
- Default Mix below 100% wet for insert use (`Parameters.cpp:193-197`). **Done 2026-11-28**: the plugin default is now 40% wet (`Parameters.cpp`); the engine default stays 1.0.
- Use an equal-power mix law (`ShimmerReverbEngine.cpp:813`).
- Give `pitchShift` a semitone step and `loopLength` a skew.

### Freeze, Infinite and loop capture
- `captureLoop()` copies up to 4 s × 2 channels in one sample call (`LoopCapture.cpp:81-88`).
- The grain alignment search bursts CPU and grows roughly with the square of the sample rate (`PitchShifter.cpp:251-266`).
- The Freeze quick toggle desyncs from the dial, automation and session reload (`PluginEditor.h:57-60`).
- Freeze and Infinite are not seeded in `prepareToPlay`, so a saved session ramps in (inferred).

### Editor
- Fixed 1120×470 window, not resizable, no scale factor.
- No input, output or limiter metering.
- No presets, A/B or undo (the APVTS `UndoManager` is `nullptr`).
- Most knobs lack unit suffixes and tooltips.

### Tests
- No processor-level tests: state round-trip, bus layouts, re-prepare, bypass.
- pluginval is not wired in and has never been run.
- Nearly everything runs at 44.1 kHz with 512-sample blocks. Add 96k/192k, variable and oversized blocks, and a mono bus.
- Some diagnostics depend on wall-clock timing and machine-local WAV files.

### Release preparation
- Plugin and manufacturer codes and version are Projucer defaults (`Manu`/`Jwoc`/1.0.0).
- VST3 category is "Fx", not "Fx|Reverb".
- Only the VS2026 exporter exists, so AU is declared but never built.
- No mono→stereo layout, no CI, README is a stub.

### Docs and comments
- `.claude/skills/juce-plugin-dev/references/shimmer-reverb-architecture.md` describes a dual-delay crossfade shifter, an `IIR::Filter` damper and a feedback-gain parameter that no longer exist.
- Stale comments: `DattorroTank.cpp:324-345`, `ShimmerReverbEngine.cpp:541-547` and `:684-686` describe the old blend law and Sustain mapping.
- `docs/shimmer-character-research.md` section 7.1 item 4 says the corrector "keeps running warm" when off; the code skips and resets it.
- `docs/shimmer-reverb-implementation-plan.md:138` still lists aliasing as not evaluated.

## 5. Research notes

- The Eno/Lanois original was a pitch shifter and a long hall in a feedback loop, with top and bottom rolled off [1].
- Reverb output is phase-scrambled, so splice points land near-randomly. That smears each sinusoid into a band of frequencies and is credited for the thick sound [10].
- Valhalla recommends a dark loop because the roll-off removes most aliasing from the shifter's feedback path [2].
- Oliverb splits loop gain as `decay·amount` and `decay·(1−amount)` and soft-limits each loop half [8].
- Eventide's Freeze sets infinite decay, mutes the input and locks out the shifters [4].
- No source was found for phase-vocoder or formant-preserving shimmer, or for DC blocking in the loop.

## Sources

1. https://valhalladsp.wordpress.com/2010/05/11/enolanois-shimmer-sound-how-it-is-made/
2. https://www.valhalladsp.com/shimmer/ValhallaShimmerNotes.pdf
4. https://r2.gear4music.com/media/85/858182/download_858182.pdf (Eventide ShimmerVerb User Guide)
5. https://downloads.eventideaudio.com/Product+Manuals/H9AlgorithmGuide+V12.pdf
6. https://www.strymon.net/manuals/BigSky_Plugin_UserManual_RevA.pdf
7. https://www.analoguehaven.com/meris/mercury7/manual.pdf
8. https://raw.githubusercontent.com/mqtthiqs/parasites/master/clouds/dsp/fx/oliverb.h
9. https://valhalladsp.wordpress.com/2010/05/07/pitch-shifting-the-h949-and-de-glitching/
10. https://valhalladsp.wordpress.com/2010/05/12/shimmer-modulation-auto-correlation-and-decorrelation/
11. https://ccrma.stanford.edu/~dattorro/EffectDesignPart1.pdf
12. https://signalsmith-audio.co.uk/writing/2021/lets-write-a-reverb/
