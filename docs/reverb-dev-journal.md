# Shimmer Reverb — Development Journal

Running log of what's actually been built against [`shimmer-reverb-implementation-plan.md`](./shimmer-reverb-implementation-plan.md), phase by phase. Each entry says what changed, what was verified and how, and what's explicitly still open — no phase gets marked done here on the strength of "it compiles," and nothing gets claimed as ear-verified unless a human actually listened to it.

## Phase 0 — Project scaffolding

**Status: complete.** Turned out to already be done before this pass started — verified, not (re)implemented.

- `Millenia.jucer`: `juce_dsp` is present in `<MODULES>` and has a `<MODULEPATH>` entry under the VS2026 `<EXPORTFORMATS>` block.
- `Source/DSP/` exists with skeleton classes already declared and grouped in the `.jucer`: `ShimmerReverbEngine.h/.cpp`, `DattorroTank.h/.cpp`, `PitchShifter.h/.cpp`, `DCBlocker.h`, `SafetyLimiter.h` — all still empty stubs (`prepare`/`reset`/`process` bodies do nothing), which is exactly Phase 0's spec.
- Plugin formats/channel config, previously flagged in the plan as an unreviewed default, were reviewed this pass: `pluginFormats="buildVST3,buildAU,buildStandalone"` is kept as-is — VST3 and Standalone are what the VS2026 exporter actually produces (confirmed: no `Millenia_AU.vcxproj` exists in `Builds/VisualStudio2026`), `buildAU` is harmless/inert on this exporter and was left in rather than stripped, in case a macOS/Xcode exporter is ever added. The stereo-in/stereo-out `BusesProperties` already in `PluginProcessor`'s constructor (JUCE template default) was confirmed as the deliberate choice for a reverb effect — not changed.
- **Verification**: built `Millenia.sln` (Debug/x64) via MSBuild — `Millenia_VST3.vcxproj` and `Millenia_StandalonePlugin.vcxproj` both produced their artifacts (`Millenia.dll`/`.vst3`, `Millenia.exe`) with zero errors, only pre-existing unused-parameter warnings from the untouched template code.

## Phase 1 — Mono Schroeder/Freeverb validation tank

**Status: code complete and builds clean. Ear/live-DAW verification explicitly not done — see "Still open" below.**

### What was built

- **`Source/DSP/ScratchSchroederTank.h/.cpp`** (new) — a mono, throwaway Freeverb-style tank: 6 parallel feedback comb filters summed and scaled by 1/6, feeding 2 series allpass filters. Explicitly not wired into `ShimmerReverbEngine`; the header comments it as removed-at-Phase-2 scaffolding.
  - Comb filter: `output = delayLine.pop(); dampState = output*damp + dampState*(1-damp); delayLine.push(input + dampState*feedback); return output;` — the standard Freeverb comb, chosen because it's a well-understood, provably-stable structure (a single real pole inside the loop, magnitude `feedback < 1`), not something invented for this pass.
  - Allpass filter: `output = -input + delayLine.pop(); delayLine.push(input + pop*allpassFeedback);` — the classic Schroeder allpass, unconditionally stable for `|allpassFeedback| < 1`.
  - Constants used: comb delays `35.3, 36.7, 33.9, 30.5, 28.9, 25.3` ms; allpass delays `5.0, 1.7` ms (deliberately non-multiple lengths, so metallic ringing doesn't show up even in throwaway code); `dampAmount = 0.2`, `feedbackAmount = 0.84`, `allpassFeedback = 0.5`. All named constants, explicitly not final tuning.
  - Every `DelayLine` is sized from `spec.sampleRate` inside `prepare()` (millisecond constants → samples, computed fresh every call) — never a hardcoded sample count, so it's sample-rate independent by construction.
- **`Millenia.jucer`**: added the two new files to the existing `DSP` `<GROUP>`.
- **`Source/PluginProcessor.h`**: added a `ScratchSchroederTank scratchTank` member and a pre-sized `juce::AudioBuffer<float> monoScratch` member, both commented as Phase 1 throwaway.
- **`Source/PluginProcessor.cpp`**:
  - `prepareToPlay`: builds a `juce::dsp::ProcessSpec`, calls `scratchTank.prepare()`/`reset()`, sizes `monoScratch` to `(1, samplesPerBlock)`.
  - `processBlock`: a `constexpr bool kPhase1ScratchTankTestMode = true` block sums the input to mono into the pre-sized `monoScratch`, runs it through `scratchTank.process()`, and broadcasts the mono result to every output channel, then returns early — before ever reaching the original do-nothing template loop (left in place, now unreachable by design, for easy deletion in Phase 2).

### How it was verified, and how it wasn't

- **Build**: `MSBuild Millenia.sln -p:Configuration=Debug -p:Platform=x64` — clean, twice independently (once during implementation, once as an independent re-check). Both VST3 and Standalone artifacts produced. Only new warnings are two expected C4702 ("unreachable code") on the now-dead original template loop, plus the same pre-existing unused-parameter warnings from Phase 0.
- **No-allocation-in-processBlock**: verified by code review, not a runtime profiler — every `DelayLine`/buffer resize happens only in `prepare()`/`prepareToPlay()`; `process()`/`processBlock()` only call `popSample`/`pushSample`/`AudioBuffer` copy-helpers on already-sized storage.
- **Stability by construction, not by empirical stress-test**: comb feedback (0.84) and allpass feedback (0.5) are both `< 1` in magnitude, which is what makes these specific difference equations unconditionally stable — this is a property of the well-known Freeverb/Schroeder structures being used correctly, confirmed by reading the implementation against the reference equations, not by running the plugin and watching a meter.
- **Explicitly NOT done — needs a human**:
  - Listening to it. Nobody has fed an impulse/transient through the Standalone or a DAW and confirmed the tail actually sounds dense and smooth with no clicks/crackling. This is Phase 1's actual "Definition of done" and it's still open.
  - Live sample-rate/block-size switching while the plugin is loaded in a host. Code review confirms `prepare()` always resizes from the live `spec` rather than a cached value, which is the correct *implementation*, but that's not the same as having actually watched it survive a live 44.1→48→96 kHz switch.

## Still open (carried into Phase 2+)

- **Ear pass for Phase 1** (see above) — do this before or alongside starting Phase 2; if the scratch tank sounds wrong, better to know before the real Dattorro topology is built on the same JUCE plumbing.
- **Live sample-rate/block-size switch test for Phase 1** — same, quick to do once the ear pass happens.
- Phase 2 onward proceeds exactly as scoped in the implementation plan — nothing about this pass changed any of the "Committed, non-negotiable" decisions at the top of that doc.

## References

- [`shimmer-reverb-implementation-plan.md`](./shimmer-reverb-implementation-plan.md) — the plan this journal tracks against
- [`shimmer-reverb-concepts.md`](./shimmer-reverb-concepts.md)
- [`../.claude/skills/juce-plugin-dev/references/shimmer-reverb-architecture.md`](../.claude/skills/juce-plugin-dev/references/shimmer-reverb-architecture.md)
