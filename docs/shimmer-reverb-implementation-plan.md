# Shimmer Reverb — Implementation Plan

This is the phased, checklist-level build plan for Millenia's shimmer reverb DSP and plugin shell. It does not re-argue architecture — it assumes and builds on the decisions already made in:

- [`shimmer-reverb-concepts.md`](./shimmer-reverb-concepts.md) — conceptual guide, tank/shifter rationale, roadmap sketch
- [`../.claude/skills/juce-plugin-dev/references/shimmer-reverb-architecture.md`](../.claude/skills/juce-plugin-dev/references/shimmer-reverb-architecture.md) — JUCE class mapping, starting parameter values, pitfall→mitigation table, real-time safety rules
- [`shimmer-reverb-open-source-survey.md`](./shimmer-reverb-open-source-survey.md) — eight external reference implementations, used here only as inspiration/cross-checks, not as a reason to change topology
- [`reverb-design-research-notes.md`](./reverb-design-research-notes.md) — academic-paper bibliography and the Eno/Lanois shimmer origin story, background reading only

**Committed, non-negotiable for this plan**: Dattorro plate tank topology, dual-delay-line crossfade pitch shifter inserted at the tank's feedback junction, bipolar shift range −24..+24 semitones, hand-written `juce::dsp::DelayLine` + manual read/write (never `juce::dsp::ProcessorChain`) for anything inside the feedback loop.

**Explicitly out of scope**: the goniometer/Lissajous-curve stereo visualizer. That work is deliberately deferred to a later, separate plan and is not referenced or scheduled here.

## Current state

*Updated after Phase 0 and Phase 1 — see [`reverb-dev-journal.md`](./reverb-dev-journal.md) for the full account.*

Phase 0 is complete: `juce_dsp` is in `Millenia.jucer`'s `<MODULES>`/VS2026 `<MODULEPATHS>`, and `Source/DSP/` exists with `ShimmerReverbEngine`, `DattorroTank`, `PitchShifter`, `DCBlocker`, `SafetyLimiter` skeletons (all still empty stubs — Phase 2+ work). `pluginFormats="buildVST3,buildAU,buildStandalone"` and the stereo-in/stereo-out `BusesProperties` in `PluginProcessor`'s constructor have been reviewed and confirmed as this project's actual targets (VST3 + Standalone are what the VS2026 exporter builds; `buildAU` is declared but inert with no Xcode exporter configured, left in place in case one's added later) — no longer an unexamined default.

Phase 1 is fully complete, including its live-session items: `Source/DSP/ScratchSchroederTank.h/.cpp` (a mono Freeverb-style 6-comb + 2-allpass tank) exists and is wired into `PluginProcessor::processBlock`. A live Standalone session confirmed sample-rate changes (44.1/48/96 kHz) don't crash or corrupt the tail, and mic input through the tank has no clicks/crackling.

Phase 2 is in progress: `Source/DSP/DattorroTank.h/.cpp` implements the committed Dattorro plate topology (4-stage input diffuser, two cross-feeding tank branches with one-pole damping filters in the feedback path, figure-eight cross-feed) and is now wired into `processBlock` as the default (`kPhase2DattorroTankTestMode = true`, `kPhase1ScratchTankTestMode = false`) — the scratch tank stays available for A/B comparison, not yet removed. Builds clean. **Still needed to close Phase 2**: a live ear pass confirming the Dattorro tank actually sounds like a plate reverb, then removing the Phase 1 scratch tank. A real bug was caught and fixed during implementation: `shimmer-reverb-architecture.md` originally specified the wrong JUCE primitive (`IIR::Filter::makeAllPass()`, a frequency-domain biquad) for the diffusion/tank allpass stages; corrected to the delay-based Schroeder allpass Dattorro's design actually requires.

`PluginEditor` is still the unmodified "Hello World!" template — GUI work starts at Phase 6.

**Still true from the original assessment:** no `AudioProcessorValueTreeState`, no real parameters, no DC blocker/limiter wired up yet, no test target of any kind.

## Phased plan

### Phase 0 — Project scaffolding

**Goal**: Make the project buildable as a DSP project (module + file skeleton) with zero behavior change, so every later phase is additive.

Tasks:
- [x] Add the `juce_dsp` module to `Millenia.jucer` (`<MODULES>` list and `<MODULEPATH>` under the VS2026 exporter) — required for `juce::dsp::DelayLine`, `juce::dsp::IIR::Filter`, `juce::dsp::Oversampling`, `juce::dsp::ProcessSpec`.
- [x] Decide and set explicit plugin format targets and channel configuration on the `JUCERPROJECT` element (see Open Decisions) instead of leaving them at Projucer defaults. — Reviewed and ratified rather than changed: `pluginFormats="buildVST3,buildAU,buildStandalone"` with VST3+Standalone as the actual VS2026-exporter targets (AU is inert here, no Xcode exporter), stereo-in/stereo-out `BusesProperties` confirmed as the deliberate channel config for a reverb effect.
- [x] Create the `Source/DSP/` subfolder and add empty class skeletons (declared but not implemented) to establish the file layout from the start: `ShimmerReverbEngine.h/.cpp`, `DattorroTank.h/.cpp`, `PitchShifter.h/.cpp`, `DCBlocker.h`, `SafetyLimiter.h`. (See "File/class structure proposal" below for what each owns.)
- [x] Add these new files to `Millenia.jucer`'s `<GROUP>` listing (or regenerate via Projucer) so they compile in VS2026.
- [x] Re-save the `.jucer` in Projucer and regenerate the Visual Studio 2026 project so the new module and files show up in the IDE.

Definition of done:
- [x] Project builds and loads in a host (or Standalone) exactly as before — no audio behavior change, just new empty files compiling and linking. — confirmed via `MSBuild Millenia.sln` (Debug/x64): VST3 and Standalone both build clean.
- [x] `juce_dsp` headers are includable from `PluginProcessor.cpp` without error. — confirmed by the same build.

Pitfalls to watch: none DSP-specific yet; this phase exists specifically to avoid discovering the missing `juce_dsp` module mid-way through Phase 1 or 2.

### Phase 1 — Mono Schroeder/Freeverb validation tank

**Goal**: Validate the JUCE DSP plumbing (delay lines, `prepareToPlay` sizing, real-time safety) with the simplest tank design, before spending effort on Dattorro's more complex topology.

Tasks:
- [x] Implement a throwaway/reference mono comb+allpass tank (Freeverb-style: 4–8 parallel feedback comb filters + 2 series allpass filters) as a standalone class, e.g. `Source/DSP/ScratchSchroederTank.h`, *not* wired into the permanent `ShimmerReverbEngine` — this is a plumbing test, not final product code. — 6 combs (`35.3/36.7/33.9/30.5/28.9/25.3` ms) + 2 series allpasses (`5.0/1.7` ms), classic Freeverb difference equations.
- [x] Use `juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear>` for the comb delay lines (linear interpolation is fine here — this stage never does fractional-rate pitch-shift reads, so Lagrange3rd isn't required yet).
- [x] Size all delay lines via `prepare (juce::dsp::ProcessSpec)` inside `MilleniaAudioProcessor::prepareToPlay`, never inside `processBlock`.
- [x] Wire this scratch tank into `processBlock` behind a temporary "test mode" so it's audible end-to-end (mono in, wet mono tail out) through a DAW or the Standalone target. — wired and builds; **not yet listened to** (see Definition of done below).
- [x] Add `juce::ScopedNoDenormals` at the top of `processBlock` (should already exist from the template — confirm it's still there once real DSP is added). — confirmed present.
- [x] Confirm `prepareToPlay` is correctly re-invoked and re-sizes buffers on sample-rate/block-size changes (test by changing the host's sample rate/block size while the plugin is loaded). — **done, live Standalone session**: switching 44.1/48/96 kHz did not crash and did not corrupt the tail; mic input played back through the scratch tank cleanly with no clicks. Extra noise noticed on one 48→44.1 kHz transition traced to the input mic device's driver reinitializing at the new rate, not the plugin (`prepareToPlay` correctly calls `scratchTank.prepare(spec)` + `scratchTank.reset()` with the live spec every time, confirmed by code review). Block size selector only offered 480 samples — a WASAPI shared-mode device constraint on the input device used, not a plugin limitation.

Definition of done:
- [x] Feeding an impulse or short transient produces an audibly dense, smooth decaying tail with no crackling, denormal stalls, or per-block clicks. — objective half covered by `Tests/MilleniaTests` (`ScratchSchroederTankTests.cpp`); subjective half confirmed live via Standalone mic input: no clicks, no crackling.
- [x] Changing host sample rate (e.g. 44.1 kHz → 48 kHz → 96 kHz) and block size does not crash or corrupt the tail. — confirmed live in Standalone; see note above.
- [x] No allocation occurs inside `processBlock` (verify by inspection — this is also asserted structurally in Phase 7's testing pass). — verified by code review: `monoScratch` and every `DelayLine` are sized only in `prepareToPlay`/`prepare()`; `processBlock`/`process()` only call `popSample`/`pushSample`/buffer read-write helpers.

Pitfalls (`shimmer-reverb-concepts.md` §Known pitfalls, `shimmer-reverb-architecture.md` §Real-time safety):
- Denormal CPU stalls on long decaying tails — `ScopedNoDenormals` is mandatory here, not optional, because this is the first place a genuine feedback tail exists.
- Metallic ringing / comb coloration from short, regular, or harmonically related delay lengths — even in this throwaway tank, pick lengths that aren't simple multiples of each other so the validation is meaningful.
- This tank is deliberately temporary — do not over-invest in tuning it; its only job is to prove the delay-line/`prepareToPlay` plumbing before Phase 2's real topology.

### Phase 2 — Dattorro plate topology

**Goal**: Replace the scratch tank with the real, committed topology: input diffuser feeding two cross-feeding delay/allpass/damping tanks in a figure-eight.

Tasks:
- [x] Implement `Source/DSP/DattorroTank.h/.cpp`: input diffuser as 4 series hand-rolled Schroeder allpass stages (`DelayLine` + manual feedback/feedforward — **not** `IIR::Filter::makeAllPass()`; the architecture doc's original mapping table was wrong on this point and has been corrected, see below), feeding two cross-feeding delay/allpass "tank" branches (A and B) with damping filters in the feedback path.
- [x] Use `juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd>` for every delay line inside the tank (not `Linear`).
- [x] Implement the damping filter as `juce::dsp::IIR::Filter<float>` configured as a one-pole low-pass (`makeFirstOrderLowPass`), placed inside each feedback branch (between `delay1` and `allpass2`), not just at the wet output.
- [x] Choose starting delay-line lengths from Dattorro's (1997) published tank tunings as a *starting point* (converted from the paper's 29761 Hz reference rate) — empirical adjustment for this project's target decay character is still open, deferred to a live ear pass.
- [ ] Remove or archive the Phase 1 scratch tank once the Dattorro tank is audibly working; do not ship both. — **not yet**: both tanks stay wired (`kPhase1ScratchTankTestMode = false`, `kPhase2DattorroTankTestMode = true`, easy to flip back for A/B) until a human confirms the Dattorro tank sounds right in a live Standalone session.
- [x] Confirm stereo handling at this stage is at minimum "not broken" (mono-summed input into one tank) — true stereo decorrelation is deferred to Phase 4.

**Correction made during implementation**: `shimmer-reverb-architecture.md`'s JUCE class mapping table originally specified `IIR::Filter<float>` + `makeAllPass()` for the diffusion/tank allpass stages. That's a frequency-domain biquad allpass (phase shift only, no meaningful time delay) and cannot produce Dattorro's diffusion, which structurally requires a delay-based Schroeder allpass (100–900 sample delay with feedback/feedforward) — the same structure `ScratchSchroederTank::processAllpass` already used in Phase 1. The architecture doc has been corrected; `DattorroTank` implements the correct hand-rolled version.

Definition of done:
- [ ] An impulse response through the Dattorro tank sounds like a plate reverb: smooth, dense, no audible flutter/metallic ringing, tail decays cleanly to silence. — **needs a live ear pass** (Standalone session), not done yet.
- [ ] Coefficient changes to the allpass filters (if exposed while tuning) don't produce audible clicks — not yet tested; smoothing decision deferred to Phase 5 as planned.
- [x] No `juce::dsp::ProcessorChain` is used anywhere in the loop-carrying path — confirmed by code review: the whole tank is hand-written `DelayLine`/`IIR::Filter` with manual read/write, no `ProcessorChain`.

Pitfalls (`shimmer-reverb-architecture.md` §Pitfalls, §JUCE class mapping):
- `juce::dsp::Reverb` must not be used or referenced anywhere — it's closed/opaque with no injection point (confirmed dead end per the architecture doc's cited forum thread).
- Metallic ringing/comb coloration: this is the phase where delay-length tuning actually matters for the shipped product, unlike Phase 1's throwaway values.
- IIR allpass coefficient changes are not auto-smoothed by JUCE — if any parameter modulates these coefficients later, smoothing has to be added deliberately.

### Phase 3 — Pitch shifter insertion

**Goal**: Insert the hand-rolled dual-delay-line crossfade pitch shifter at the tank's feedback junction, making this an actual shimmer reverb for the first time.

Tasks:
- [ ] Implement `Source/DSP/PitchShifter.h/.cpp`: own circular buffer, two read pointers advancing at a rate derived from the pitch ratio `2^(semitones/12)`, crossfade between the two reads near wrap discontinuities.
- [ ] Use `juce::LagrangeInterpolator` or `juce::CatmullRomInterpolator` for the fractional-position reads; explicitly reset interpolator state at each discontinuity/crossfade point (the architecture doc calls this out — stale interpolator state across a jump is a known bug shape).
- [ ] Size the circular buffer in `prepareToPlay` for the *most negative* semitone value the parameter range will allow (−24 st → read pointer at 0.25× write rate), not for the +12 st default — this is a specific, previously-identified pitfall, not a generic buffer-sizing note.
- [ ] Size the crossfade window (~20–30 ms at +12 st default per the architecture doc) and confirm it scales correctly across the full −24..+24 st range rather than only being tuned at the default.
- [ ] Wire the shifter into `ShimmerReverbEngine`: tank output → `PitchShifter::process()` → back into tank input, closing the feedback loop.
- [ ] Temporarily hardcode the shift amount (e.g. +12 semitones) and feedback gain (e.g. 0.7) as constants — real parameter control is Phase 5's job; this phase is about the signal path being correct.
- [ ] Add a hard ceiling on feedback gain (e.g. clamp below 1.0 even with hardcoded constants) so this phase can be tested without a runaway risk before Phase 4's proper limiter exists.

Definition of done:
- Feeding a short transient produces the classic shimmer effect audibly: successive passes through the loop are each pitched up (or down, if testing a negative hardcoded value) from the previous one, and the tail rises (or falls) rather than just decaying flat.
- No runaway/growing-without-bound output at the hardcoded feedback gain over at least 30 seconds of sustained input.
- Switching the hardcoded shift value between a few test points (e.g. +7, +12, +19, −12) all produce audibly correct, differently-pitched shimmer character without crashes or NaNs.

Pitfalls (`shimmer-reverb-architecture.md` §Pitfalls, `shimmer-reverb-concepts.md` §Concept 2):
- Feedback runaway — this phase is the first point where the actual pitched-feedback loop exists; even with a hardcoded low gain, verify by ear and by peak-metering that levels stay bounded.
- Aliasing from fractional-rate reads — Lagrange3rd/interpolator choice should already mitigate this; only escalate to `juce::dsp::Oversampling` around this stage if aliasing is still audible after confirming interpolation is correctly applied (not a day-one requirement per the architecture doc).
- Grain-window buffer undersized for negative shift values — this is called out explicitly as a distinct bug from "buffer too small in general"; test at −24 st specifically, not just at the +12 st default.
- DC offset accumulation: crossfading and feedback can build DC bias over long tails — this is the natural place to add the DC-blocking filter (`Source/DSP/DCBlocker.h`) at the loop boundary, even though full gain-staging work is Phase 4.

### Phase 4 — Gain staging, safety limiter, stereo decorrelation

**Goal**: Turn the working-but-fragile Phase 3 signal path into something safe to leave running unattended and that sounds stereo, not collapsed-mono.

Tasks:
- [ ] Replace the Phase 3 hardcoded feedback-gain clamp with deliberate gain-staging: feedback gain in the 0.6–0.85 range (architecture doc's starting range), tuned by ear, accounting for the shifter's own gain contribution — confirm it stays strictly below 1.0 under all shift-amount settings, not just the default.
- [ ] Implement `Source/DSP/SafetyLimiter.h/.cpp`: a soft-clip or brick-wall limiter placed inside the feedback loop (not just at the final output) as a safety net independent of the gain knob.
- [ ] Implement `Source/DSP/DCBlocker.h` (if not already done opportunistically in Phase 3) as a one-pole DC-blocking filter at the loop boundary.
- [ ] Decide and implement the per-channel decorrelation approach: either (a) different delay-line lengths/modulation per channel inside the Dattorro tank, or (b) quadrature-offset pitch shifting per channel (per Airwindows `Galactic`, cited in both source docs) — this is an open decision the docs leave as an "or" (see Open Decisions below); implement whichever is chosen and document the choice in code comments.
- [ ] Confirm the plugin now runs stereo in/stereo out correctly with the decorrelation in place — process left/right through independent tank/shifter state where the chosen approach requires it.

Definition of done:
- Feeding sustained loud input (e.g. continuous pink noise at 0 dBFS) for several minutes at maximum feedback setting never clips catastrophically or produces runaway output — the limiter demonstrably catches it.
- A/B comparing L and R output channels shows measurable decorrelation (e.g. via a correlation meter or simply listening in mono vs. stereo) — the wet signal is audibly wide, not collapsed.
- DC offset measured at the output (e.g. via a DC-coupled meter or by inspecting the buffer's mean) stays near zero even after long sustained tails.

Pitfalls (`shimmer-reverb-architecture.md` §Pitfalls, `shimmer-reverb-concepts.md` §Known pitfalls):
- Feedback runaway — this is the phase where the *permanent* safety net (limiter) must exist; Phase 3's clamp was a stopgap, not the final design.
- Mono collapse — explicitly flagged as needing "per-channel decorrelation... or the wet signal sounds flat and mono"; do not skip this because Phase 1–3 tested mono-only signal paths.
- DC offset accumulation — confirm the blocker is actually in the feedback loop's path, not only at the very final output stage, since the doc specifies the accumulation happens inside allpass/comb feedback and crossfading.

### Phase 5 — APVTS parameters

**Goal**: Replace every hardcoded DSP constant with a real, host-automatable parameter routed through one `AudioProcessorValueTreeState`.

Tasks:
- [ ] Create `Source/Parameters.h/.cpp` with a single `createParameterLayout()` function returning the full `juce::AudioProcessorValueTreeState::ParameterLayout`, and stable string ID constants for every parameter (e.g. `"pitchShift"`, `"feedback"`, `"mix"`, `"decay"`, `"damping"` — finalize the actual parameter set from what Phases 2–4 exposed as tunable).
- [ ] Add the bipolar **Pitch Shift** parameter: continuous, −24..+24 semitones, default +12; expose it as one parameter (not separate "shift amount" + "direction" controls), per the concepts doc's explicit naming rationale.
- [ ] Add quick-select presets at +7/+12/+19 semitones as UI shortcuts that set the same underlying parameter value — not separate modes or parameters.
- [ ] Add feedback, mix (dry/wet), and any decay/damping controls surfaced from the Dattorro tank's tunable points in Phase 2.
- [ ] Add a `juce::AudioParameterBool` bypass parameter routed through APVTS (per the `juce-plugin-dev` skill's hard rule — not a processor-only flag invisible to automation).
- [ ] In `MilleniaAudioProcessor::processBlock`, read every parameter via `std::atomic`-backed `getRawParameterValue(id)->load()` — never the raw parameter object — and feed those values into `ShimmerReverbEngine`.
- [ ] Add parameter smoothing (e.g. `juce::SmoothedValue` or APVTS's built-in smoothing) for any parameter that maps directly onto something audible when changed abruptly (feedback gain, mix, pitch shift) to avoid clicks on automation.
- [ ] Implement `getStateInformation`/`setStateInformation` via APVTS `copyState()`/`replaceState()` (not ad-hoc member variables), including a schema version int for future preset compatibility.

Definition of done:
- Every DSP-affecting value in the engine is driven by an APVTS parameter; no hardcoded constants remain from Phases 3–4 for anything user-facing.
- Automating any parameter from a host (e.g. drawing an automation curve for Pitch Shift or Feedback) produces smooth, click-free audible changes.
- Save/reload of plugin state (close and reopen the project in a host, or explicit save/load in Standalone) restores every parameter to its exact prior value.

Pitfalls: none new from the shimmer-specific docs — this phase is where the `juce-plugin-dev` skill's general hard rules (parameter routing, state versioning) become directly load-bearing; violating them here is the actual risk, not a DSP artifact.

### Phase 6 — Editor UI

**Goal**: Build a functional (not necessarily final-polish) editor exposing every Phase 5 parameter, replacing the "Hello World!" template editor.

Tasks:
- [ ] Replace `PluginEditor.h/.cpp`'s placeholder content with real controls: at minimum a rotary slider for Pitch Shift (with the +7/+12/+19 quick-select buttons/presets), and sliders/knobs for feedback, mix, decay/damping, plus a bypass toggle.
- [ ] Wire every control through APVTS attachment classes (`juce::AudioProcessorValueTreeState::SliderAttachment`, `ButtonAttachment`, etc.) — the editor must never call `MilleniaAudioProcessor` DSP methods directly or read/write engine state itself.
- [ ] Lay out components in `resized()`; confirm the editor is resizable or fixed-size deliberately (decide, don't default).
- [ ] Confirm the Pitch Shift control visually/behaviorally reads correctly across its full bipolar range (e.g. a bipolar slider fill or center-detented knob, not a unipolar-looking control mapped onto a negative range).

Definition of done:
- Every parameter changeable from the editor produces the same audible effect as automating it from the host (Phase 5's smoothing still applies).
- No editor code path touches `AudioProcessor`/DSP state directly — grep confirms all control wiring goes through APVTS attachments.
- Opening/closing/resizing the editor repeatedly does not leak or crash (basic manual smoke test; formal leak detection is part of Phase 7).

Pitfalls: none shimmer-specific; this phase is governed entirely by the `juce-plugin-dev` skill's "GUI never touches processor state directly" hard rule.

### Phase 7 — Testing and validation

**Goal**: Confirm the finished DSP and plugin shell hold up under the testing workflow this project has already committed to (`juce-plugin-testing` skill), not just "sounds right once."

Tasks:
- [x] Stand up a `juce::UnitTest`-based test runner target (console app or debug-build hook — see Open Decisions; none exists in the project yet). — **Done ahead of schedule**, pulled forward during Phase 1: `Tests/MilleniaTests.jucer` (separate `consoleapp` project, `Tests/Source/Main.cpp` running `juce::UnitTestRunner`), with a first suite (`ScratchSchroederTankTests.cpp`) already green. See `reverb-dev-journal.md`.
- [ ] Write DSP unit tests for anything with deterministic input→output math: pitch-ratio calculation (`2^(semitones/12)` at boundary values −24/0/+24), pitch-shifter read-pointer/crossfade indexing (impulse position tracking, wrap behavior), Dattorro tank delay-line indexing (impulse response timing at known delay lengths), DC blocker (DC input → near-zero output).
- [ ] Write parameter round-trip tests: for every APVTS parameter, assert `setStateInformation(getStateInformation())` restores the same value.
- [ ] Run the DSP tests at multiple block sizes and sample rates (at minimum 1, 7, 512, 4096 samples; 44100/48000/96000 Hz) to catch buffer-boundary and off-by-one bugs the default block size would hide.
- [ ] Build the actual VST3 (and any other configured format) and run `pluginval --strictness-level 10 --validate <path-to-vst3>` before considering any build release-ready.
- [ ] Do a final ear-validation pass specifically for the things unit tests cannot catch: overall shimmer character at the default +12 st, absence of audible metallic ringing/comb coloration, perceived stereo width, and behavior at extreme parameter combinations (max feedback + extreme shift values) run for several minutes.

Definition of done:
- Unit tests exist and pass for all deterministic DSP math listed above.
- Parameter state round-trip is covered for every parameter, not a sample subset.
- `pluginval` at strictness 10 passes clean against the built VST3.
- A documented ear-validation pass has been done and any remaining tuning issues are captured as follow-up work, not silently shipped.

Pitfalls: this phase exists specifically because the earlier phases' "definition of done" criteria are mostly ear/behavior-based; Phase 7 is where that gets backed by the project's actual test discipline instead of remaining implicit.

## File/class structure proposal

```
Source/
  PluginProcessor.h/.cpp        Thin host shell: owns the APVTS, owns one ShimmerReverbEngine instance,
                                 forwards prepareToPlay/processBlock/state calls. No DSP math lives here.
  PluginEditor.h/.cpp            GUI components + APVTS attachments only.
  Parameters.h/.cpp              createParameterLayout(), parameter ID string constants, defaults/ranges.

  DSP/
    ShimmerReverbEngine.h/.cpp   Top-level DSP object: composes the diffuser, tank A/B, pitch shifter,
                                 limiter, and DC blocker into the full signal path. Exposes
                                 prepare(ProcessSpec) and process(AudioBlock<float>&); this is the
                                 only DSP class PluginProcessor talks to directly.
    DattorroTank.h/.cpp          Input diffuser (4x series allpass) + two cross-feeding tank branches
                                 with damping. Owns its own DelayLine<Lagrange3rd> and
                                 IIR::Filter instances; exposes tunable points (delay lengths,
                                 damping cutoff) as plain setters, not parameters itself.
    PitchShifter.h/.cpp          Hand-rolled dual-delay-line crossfade shifter: circular buffer,
                                 two read pointers, interpolator(s), crossfade logic. Takes a
                                 pitch-ratio (or semitone) value per block; owns no parameter
                                 knowledge itself.
    DCBlocker.h                  One-pole DC-blocking filter, used at the feedback loop boundary.
    SafetyLimiter.h/.cpp          Soft-clip/limiter safety net placed inside the feedback loop.

  (Phase 1 only, removed by Phase 2 completion)
    ScratchSchroederTank.h        Throwaway mono comb+allpass tank used solely to validate DSP
                                 plumbing before Dattorro is built; not part of the shipped signal path.
```

This keeps every class that participates in the feedback loop hand-written and explicit (no `ProcessorChain` anywhere under `DSP/`), matches the "PluginProcessor stays thin" convention implied by the `juce-plugin-dev` skill's parameter/state rules, and gives the test runner (`Tests/MilleniaTests.jucer`, standing since Phase 1 — see Phase 7's task list and "Risks / open decisions") a natural one-class-per-file target for unit tests (`PitchShifterTests.cpp`, `DattorroTankTests.cpp`, etc., one per `DSP/` class, alongside the existing `ScratchSchroederTankTests.cpp`).

## Testing strategy

Mapping phases to the `juce-plugin-testing` skill's workflow:

| Phase | Unit-testable now? | Ear/pluginval validation needed? |
|---|---|---|
| 0 — Scaffolding | No DSP yet | Build/load sanity only |
| 1 — Scratch tank | Delay-line indexing at known lengths (impulse timing) | Ear: tail sounds dense/smooth, no per-block clicks |
| 2 — Dattorro tank | Delay-line indexing, allpass coefficient generation (`makeAllPass()` output at known params) | Ear: plate character, no metallic ringing; sample-rate/block-size change resilience |
| 3 — Pitch shifter | Pitch-ratio math (`2^(semitones/12)` at boundary values), read-pointer/crossfade indexing, grain-buffer sizing at −24 st | Ear: shimmer character correct direction and pitch at test values; no runaway over sustained input |
| 4 — Gain/limiter/decorrelation | DC blocker (DC in → ~0 out), limiter threshold behavior at known input levels | Ear: L/R decorrelation, no catastrophic clipping under sustained loud input |
| 5 — APVTS parameters | Parameter round-trip (`setStateInformation(getStateInformation())`) for every parameter | pluginval automation pass (parameter automation without crashes/glitches) |
| 6 — Editor | N/A (GUI, not DSP math) | Manual: every control drives the matching parameter; no processor state touched directly |
| 7 — Testing/validation | All of the above consolidated into one test runner target, run across block sizes 1/7/512/4096 and sample rates 44100/48000/96000 | `pluginval --strictness-level 10` against the built VST3; final ear pass |

General rule from the skill (applied throughout, not just Phase 7): test DSP correctness (output values, state, indexing) — never assert on wall-clock timing behavior, that's flaky by construction.

## Risks / open decisions

These are genuinely unresolved by the three source docs; flagging them honestly rather than inventing false certainty:

- ~~**`juce_dsp` module is currently absent from `Millenia.jucer`.**~~ **Resolved in Phase 0.** Module + VS2026 `<MODULEPATH>` added, confirmed by a clean build.
- ~~**Plugin formats and channel configuration are not explicit in the `.jucer` file.**~~ **Resolved in Phase 0** (by review, not by editing): `pluginFormats="buildVST3,buildAU,buildStandalone"` — VST3+Standalone are what the VS2026 exporter actually produces; `buildAU` is harmless/inert without an Xcode exporter and was deliberately left rather than stripped, in case a macOS exporter is added later. Channel config is the JUCE template's stereo-in/stereo-out `BusesProperties`, confirmed as the intended config for a reverb effect (not touched).
- **Exact Dattorro delay-line lengths.** Both source docs are explicit that Dattorro's (1997) and Freeverb's published values are a *starting point, not a literal port*. The actual shipped lengths need empirical tuning during Phase 2 against this project's target decay character — no one has picked final numbers yet.
- **Exact safety limiter design.** The docs specify "a soft-clip/limiter safety net," not a specific algorithm (simple `tanh`/`std::clamp` soft-clip vs. a proper lookahead peak limiter vs. something else). This needs a decision during Phase 4, and it affects both CPU cost and how audibly it announces itself when it engages.
- **Decorrelation method.** `shimmer-reverb-concepts.md` explicitly presents this as an "or": different delay lengths/modulation per channel, *or* quadrature-offset pitch shifting (Airwindows `Galactic`-style). The docs don't pick one — Phase 4 needs a deliberate choice, ideally informed by a quick A/B rather than defaulting to whichever is easier to code.
- **Whether/when to add oversampling around the pitch shifter.** Both docs agree it's "not a day-one requirement" and should be added "only if aliasing is still audible" after Lagrange3rd interpolation — this is a deferred, conditional decision, not a scheduled task, and Phase 3's definition of done doesn't currently require it.
- ~~**No test runner target exists yet.**~~ **Resolved, pulled forward from Phase 7.** A separate Projucer `consoleapp` project, `Tests/MilleniaTests.jucer`, references `Source/DSP/*.cpp` by relative path (not copies) and builds its own `Tests/Builds/VisualStudio2026/MilleniaTests.sln`. `Tests/Source/Main.cpp` runs a `juce::UnitTestRunner` subclass that logs to stdout and returns a non-zero exit code on any failure — this is what future `juce::UnitTest` subclasses (per-`DSP/` class, as sketched in "File/class structure proposal" below) plug into as each real class lands, starting now instead of waiting for Phase 7. Module closure: `juce_core`, `juce_audio_basics`, `juce_audio_formats`, `juce_dsp` (the full dependency chain `juce_dsp` actually needs — verified against JUCE's own module declarations, not guessed).
- **Double-precision processing support.** The `juce-plugin-dev` skill requires a *deliberate* decision on `supportsDoublePrecisionProcessing()`/`getProcessingPrecision()`, not a silent float-only truncation. None of the shimmer docs address this for the Dattorro tank or pitch shifter specifically (e.g. whether `DelayLine<double, Lagrange3rd>` instantiations are needed). Needs an explicit choice, likely around Phase 2–3.
- **Feedback gain's final default value.** The architecture doc gives a range (0.6–0.85) and says "tune by ear," not a committed single default — Phase 4/5 needs to land on one specific number (and possibly per-preset values) rather than treating the range itself as the answer.

## References

- [`shimmer-reverb-concepts.md`](./shimmer-reverb-concepts.md)
- [`../.claude/skills/juce-plugin-dev/references/shimmer-reverb-architecture.md`](../.claude/skills/juce-plugin-dev/references/shimmer-reverb-architecture.md)
- [`shimmer-reverb-open-source-survey.md`](./shimmer-reverb-open-source-survey.md)
- [`reverb-design-research-notes.md`](./reverb-design-research-notes.md)
- [`reverb-dev-journal.md`](./reverb-dev-journal.md) — running log of what's actually been built against this plan, phase by phase
