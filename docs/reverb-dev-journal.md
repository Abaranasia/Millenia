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

## Phase 7 (partial) — test runner, pulled forward

**Status: complete for the runner itself; test coverage is currently limited to `ScratchSchroederTank`.**

The user asked to make sure tests get added "if needed" going forward. Rather than force-fit tests onto Phase 1's throwaway tank under time pressure, or silently defer everything to Phase 7 as originally planned, the test *runner* itself was pulled forward now so every DSP class from Phase 2 onward can get a test the moment it's implemented, instead of waiting for a dedicated infrastructure phase.

### What was built

- **`Tests/MilleniaTests.jucer`** (new) — a separate Projucer `consoleapp` project, sibling to the main plugin project. References `Source/DSP/*.cpp` by relative path (not copies), so tests always compile against the real, current DSP code. Module closure: `juce_core`, `juce_audio_basics`, `juce_audio_formats`, `juce_dsp` — verified against JUCE's own module dependency declarations (`juce_dsp` → `juce_audio_formats` → `juce_audio_basics` → `juce_core`), not guessed.
- **`Tests/Source/Main.cpp`** (new) — a `juce::UnitTestRunner` subclass that logs to stdout (the default logs to the platform debugger output, invisible outside an attached debugger) and returns a non-zero process exit code on any failure, so the suite is checkable from a plain shell.
- **`Tests/Source/ScratchSchroederTankTests.cpp`** (new) — two tests:
  1. *Impulse response is bounded, decaying, and finite* — feeds a single-sample impulse, runs 40 blocks (~460ms), asserts every sample stays finite, every block's peak stays under a safety bound (10.0, well above what feedback coefficients `<1.0` should ever produce), and the tail decays past its peak rather than sustaining or growing.
  2. *Tail settles to near-silence well before feedback decay would require it* — same excitation, run out to 300 blocks (~3.5s), asserts the final peak is below `1e-4`. (First draft of this test only fed silence to an already-silent, freshly-reset tank — trivially true and not really testing anything. Caught and fixed before treating this as done: it now excites the tank first, same as test 1, so it actually exercises the decay-to-silence property.)
- **Build quirk hit and fixed**: Projucer only adds `JuceLibraryCode` and module paths to the include search path, not the folder of every individually-referenced source file — `#include "ScratchSchroederTank.h"` had to become `#include "../../Source/DSP/ScratchSchroederTank.h"` in the test file.

### Verification

Built `Tests/Builds/VisualStudio2026/MilleniaTests.sln` via MSBuild, ran the resulting `MilleniaTests.exe` directly (independently, twice — once after the initial implementation, once after strengthening test 2):

```
Starting tests in: ScratchSchroederTankTests / Impulse response is bounded, decaying, and finite...
Completed tests in ScratchSchroederTankTests / Impulse response is bounded, decaying, and finite
Starting tests in: ScratchSchroederTankTests / Tail settles to near-silence well before feedback decay would require it...
Completed tests in ScratchSchroederTankTests / Tail settles to near-silence well before feedback decay would require it

ALL TESTS PASSED (0 failures)
```
Exit code `0`. This closes the objective half of Phase 1's "impulse response" Definition of Done bullet (see above) — the subjective "does it actually sound good" half is still ear-only.

**Note**: standing up this project also lazily initialized a `.codegraph/` index at the repo root (per this machine's CodeGraph policy for real project roots) — it ships its own self-contained `.gitignore` so the database file itself never gets committed, only the ignore rule does. Not something this pass did deliberately, just a side effect worth recording.

## Still open (carried into Phase 2+)

- **Ear pass for Phase 1** — do this before or alongside starting Phase 2; if the scratch tank sounds wrong, better to know before the real Dattorro topology is built on the same JUCE plumbing.
- **Live sample-rate/block-size switch test for Phase 1** — same, quick to do once the ear pass happens.
- **Add a `DattorroTankTests.cpp`/`PitchShifterTests.cpp` etc. to `Tests/` as each class lands in Phase 2+** — the runner now exists specifically so this is a small increment per class, not a project.
- Phase 2 onward proceeds exactly as scoped in the implementation plan — nothing about this pass changed any of the "Committed, non-negotiable" decisions at the top of that doc.

## 2026-10-10 — Freeze toggle pitch glide fixed (level crossfade)

**Status: code complete, unit-tested, plugin builds. Not yet ear-tested.** Branch `fix/freeze-glide`.

### What was built

- **`Source/DSP/ShimmerReverbEngine.h/.cpp`**:
  - Added a second in-loop `PitchShifter unityShifter`, permanently at 0 st. It is prepared and reset with `shifter` and gets the same `setFreezeAmount()` drift.
  - The main `shifter` now always runs at the full `pitchShiftSemitones`, and `formantCorrector` is keyed to that same value.
  - `updateShifterRatio()` was renamed `applyPitchAndFreezeCrossfade()`, since it no longer sets a crossfaded ratio. It computes `unityWeightTarget = freeze^16` and pushes the full `pitchShiftSemitones` to the main shifter and the formant corrector. The curve constant was renamed `pitchShiftCrossfadeCurve` → `freezeUnityCrossfadeCurve`; it is engine-private, so the rename stays contained.
  - `process()` linearly blends main and unity output, for both the primary signal (after formant correction) and the quadrature signal (after its formant correction, before its DC blocker).
  - `unityShifter.processSample()` runs every sample, even at freeze 0.
- **Per-sample freeze ramps (new, not in the original plan).** At 512-sample blocks the first version still scored +15.75 dB on T1. The cause was not the glide: it was the per-block steps of the tank's `freezeAmount` and of the input mute (`1 - freeze`), together a zipper. `process()` now ramps both linearly per sample from the previous block's value (`processedFreezeAmount`), and ramps the crossfade weight the same way. When start equals target, every sample's value is exactly that target, so all constant-freeze behaviour is bit-identical.
- **`Tests/Source/ShimmerReverbEngineTests.cpp`**: new `ShimmerReverbEngineFreezeCrossfadeTests` class in the same .cpp, so no Projucer resave was needed. Run it alone with the `FreezeCrossfade` filter.

### Verification

| Test | Old ratio glide | New level crossfade |
|---|---|---|
| T1, intermediate-pitch excess in L/R around a toggle (128 / 512-sample blocks), threshold 8 dB | +16.76 / +20.97 dB, **FAILS** | +3.20 / +4.65 dB, passes |
| T2, worst 10 ms level dip (128 / 512-sample blocks), limit -3 dB | -0.16 / -0.21 dB | -0.17 / -0.22 dB |
| T3, 880 Hz growth over a 60 s freeze hold, limit +1 dB | -2.15 dB | -3.94 dB (a mutation that keeps the main shifter at +12 st: +2.74 dB, **FAILS**) |
| Default-settings output fingerprint | `be7b4e1487c0ce8b` | `be7b4e1487c0ce8b` (bit-identical) |
| Engine processing/audio ratio (Release) | ~0.037 | ~0.052 |

The first T1 draft measured the mid channel over a 1.4 s tail. It could not tell the two designs apart (+9.90 vs +9.19 dB). The glide's audible path turned out to be the feed-forward Width term (`sideShift`), not the loop. At freeze, `DattorroTank` caps the shimmer path at 0.01 of the loop. The first T3 draft used a brightness proxy and was blind to the cascade mutation. Both were replaced before being trusted.

### Still open

- Ear test of the toggle and of the 75→100% dial sweep.
- At full freeze the output now collapses to mono: `unityShifter`'s two grain pools are identical at ratio 1.0, so `sideShift` is 0. The old code kept a side channel about 28 dB below the mid, left over from grain offsets of the glided shifter. Worth a listen.
- At Shimmer Sustain 0, freezing dips the level by about 12–14 dB (10 ms windows) in **both** old and new code. This is pre-existing and unrelated to the crossfade.

## 2026-10-10 — Unity shifter idles at Freeze 0 (CPU)

**Status: code complete, unit-tested, plugin builds. Not yet ear-tested.** Branch `perf/unity-shifter-idle`.

### What was built

- **`Source/DSP/ShimmerReverbEngine.h/.cpp`**: `unityShifter` now runs only while active. `process()` decides once per block: active if the freeze ramp's start (`processedFreezeAmount`) or target (`freezeAmount`) is above 0, or the crossfade weight is (redundant, but it makes "weight exactly 0 while idle" hold by construction). While idle, `processSample()` is not called and 0.0f stands in for its primary and quadrature outputs, which are weighted by exactly 0.0f there.
- **No ghost audio.** A new `unityShifterActive` member tracks the state. On the idle → active edge `unityShifter.reset()` runs first: an idle shifter's delay line still holds ~80 ms of loop audio from its last engagement. `PitchShifter::reset()` only zeroes buffers and rewinds indices (`juce::dsp::DelayLine::reset()` is `std::fill` + `AudioBuffer::clear()`), so it is audio-thread safe. It keeps `pitchRatio` (0 st), the freeze drift depth and the configured delay, so nothing needs re-applying. Only the drift LFO phase restarts at 0 on each engagement. `ShimmerReverbEngine::reset()` marks the shifter idle.
- **Test hooks**: `peekUnityShifterOutput()` (raw, unweighted unity output of the last sample) and `forceUnityShifterActiveForTesting()` (restores the always-on behavior for same-build comparisons).
- **Tests** in `ShimmerReverbEngineFreezeCrossfadeTests`: a CPU assertion, a ghost-audio test, and the golden hash now checked for both the idle and the forced-active engine.

### Verification

| Test | Always-on (before) | Idle skip (after) |
|---|---|---|
| T1, intermediate-pitch excess (128 / 512-sample blocks), threshold 8 dB | +3.20 / +4.65 dB | +3.20 / +4.65 dB |
| T2, worst 10 ms level dip (128 / 512-sample blocks), limit -3 dB | -0.17 / -0.22 dB | -0.17 / -0.22 dB |
| T3, 880 Hz growth over a 60 s freeze hold, limit +1 dB | -3.94 dB | -3.74 dB |
| Default-settings output fingerprint | `be7b4e1487c0ce8b` | `be7b4e1487c0ce8b` (idle and forced-active) |
| Freeze 0 real-time factor, idle / forced-active, best of 5 | 1.006, **FAILS** the 0.9 assertion | 0.69 (0.037 vs 0.053) |
| Ghost test: tone-A excess over always-on, first 70 ms after a re-engage, limit +3 dB | 0.00 dB (guard, passes trivially) | -186.64 dB (silent cold start). Naive skip without `reset()`: +45.29 dB, **FAILS** |

The cold start was the worry, since T2's earlier comment noted it could not see one. It still cannot: at the current blend caps `DattorroTank` holds the shimmer path to ~1–2% of the loop once the unity weight matters. Over the full 150 ms window the reset path reads +5.55 dB of "tone A". That is leakage from the cold path's abrupt onset of tone B, not stale audio: over 80–150 ms alone it matches always-on to 0.01 dB. That is why the assertion uses only the first 70 ms.

### Still open

- Ear test of a quick re-engage. If `frozenMaxShimmerBlendWeight` or the ^16 curve ever changes, re-check T2 for a cold-start dip.

## 2026-10-10 — Loop Mix silent loop fixed (re-arm + deferred capture)

**Status: code complete, unit-tested, plugin builds. Not yet ear-tested.** Branch `fix/loop-capture-rearm`.

By-ear report: at Loop Mix = 1 the loop was sometimes silent, leaving only the dry part of the main Mix, or total silence at Mix = 1. Turning Loop Mix to 0 and back up fixed it, "but not always". The loop records the wet output, so Freeze/Infinite tails were captured fine and were not the cause. The capture trigger was an exact rising edge, `previousLoopFreezeAmount == 0.0f && loopFreezeAmount > 0.0f`, and it had three root causes:

1. **Restart capture of an empty history.** `prepare()`/`reset()` zero the history and restart the ramp at 0. With Loop Mix already up (restored state at Standalone launch, a device restart, a sample-rate change), the first sample was an "edge" and captured all zeros: a silent loop for as long as Loop Mix stayed up.
2. **The dial doesn't land exactly on 0.** At e.g. 0.003 the capture never re-armed, so the old (possibly silent) loop kept playing.
3. **A quick down-and-up** faster than the 50 ms ramp never reaches 0, so there is no edge.

### What was built

- **`Source/DSP/LoopCapture.h/.cpp`** only. No engine, processor or parameter changes.
  - **Re-arm with hysteresis.** Capture re-arms when the ramped amount and its target are both <= `rearmThreshold` (0.01, -40 dB of old-loop blend). An armed capture fires when the ramped amount rises above `retriggerThreshold` (0.02, -34 dB); firing disarms. A single threshold would re-capture on every jitter crossing of 0.01. The exact-zero edge is kept verbatim as a separate OR term, so the classic engage captures on the same sample as before.
  - **Deferred capture.** A capture fired with less history than the requested N goes pending. While pending the output is the live wet signal. On the sample that completes N samples of history, the capture runs (N recomputed from `pendingLoopLengthMs` then) and the smoothed amount snaps to 0 and is re-targeted, so the normal 50 ms engage ramp runs again. Parking near zero cancels a pending capture. "History" first meant `validHistorySamples`, samples written since the last reset, with real-but-silent history deliberately captured as is. That decision was reversed the same day; see "Follow-up: wait for sound" below.
  - **No-loop passthrough.** With no loop captured, `process()` now returns the wet signal untouched. Before, it blended toward a 0.0f loop, which was unreachable then but is reachable now after a cancelled deferred capture.
  - Cause 3 is intentionally left as "keeps the old loop": the ramp never fell below -40 dB, so the loop never faded out.
  - Test hooks: `getCaptureCount()`, `isCapturePending()`. Only ints, bools and floats were added; nothing allocates, locks or throws per sample.
- **`Tests/Source/LoopCaptureTests.cpp`**: four new tests, written first and run RED against the old code.

### Verification

| Test | Old code (RED) | New code (GREEN) |
|---|---|---|
| Restart path: pending-window passthrough error (limit 1e-6) | 0.5 (output dips toward a zero loop) | 0.000000 |
| Restart path: engaged level vs input (limit ±3 dB) | -200 dB (silent loop) | 0.00 dB |
| Restart path: deferred engage delta vs steady delta (limit 3x) | n/a (nothing engages) | 0.031348 vs 0.031340 |
| Near-zero re-arm (park at 0.005, switch to tone B, raise): B − A Goertzel | -153.95 dB (loop still tone A), captures 1 → 1 | +151.66 dB, captures 1 → 2 |
| Jitter guard: captures after 2 s wobble below 0.01 / straddling 0.004–0.016 / then a raise | 1 / 1 / 1 (raise did not capture) | 1 / 1 / 2 |
| Pending cancel: captures, passthrough error | 1 capture, 0.5 | 0 captures, 0.000000 |
| Default-settings output fingerprint | `be7b4e1487c0ce8b` | `be7b4e1487c0ce8b` |

All seven pre-existing `LoopCaptureTests` pass unchanged, with identical logged numbers (seam 0.034192/0.034007, engage 0.018806/0.018814). A mutation that collapses the hysteresis band to a single threshold fails the jitter guard with 16 spurious captures.

### Follow-up: wait for sound (reverses "silence is not special-cased")

A diagnostic replay of the user's actual launch state (Mix 1, Loop Mix 1, Loop Length 3225.8 ms, 48 kHz, 480-sample blocks) showed the deferred capture counted samples, not sound:

- If the source app wasn't playing during the first ~3.2 s after launch, the capture fired anyway on pure silence. At Loop Mix 1 the output is then (1-1)·wet + 1·loop = digital zero (-200 dB) forever, even with -15 dBFS input present.
- If only the first 1 s was silent, every loop cycle had a 1 s digital-zero gap.

So "capturing silence is honest" reproduced the very bug the fix was for, and was dropped. What changed, still only in `LoopCapture.h/.cpp`:

- **`audibleHistorySamples` replaces `validHistorySamples`.** It counts samples since the most recent sound onset, saturating at capacity.
  - A wet sample is sound when max(|L|, |R|) > `signalGateLinear` = 0.001 (-60 dBFS). Measured wet levels are about -2 to -7 dBFS, so real material clears it by 53–58 dB. -60 dB is also the RT60 "gone" point, so a decaying tail counts until it is inaudible.
  - `consecutiveSilentSamples` is the current unbroken run of sub-gate samples. Once it is longer than `silenceResetSamples` (250 ms, converted at prepare), the counter resets to 0 and stays 0 until the next sound sample. Shorter runs (zero crossings, rests, brief pauses) keep counting, but only after an onset: leading silence never counts.
  - Both are zeroed by `reset()`/`prepare()`. The silent-run counter saturates at `silenceResetSamples + 1`.
- **One readiness predicate**, `hasEnoughAudibleHistory()` (`audibleHistorySamples >= requestedLoopLengthSamples()`), now used by both the immediate and the pending paths. Before, the check was written out twice.
- A captured loop therefore never contains a sub-gate run longer than 250 ms. Pending, cancel and deferred-ramp semantics are unchanged. Since the audible count can never exceed the samples written, the "never capture unwritten zeros" guarantee still holds.
- Stale "rising-edge capture" comments in `LoopCapture.h` now describe all three trigger paths: the exact edge, the armed rise, and the deferred landing.

Four more tests, written first and run RED against the samples-only counter:

| Test | Samples-only (RED) | Wait for sound (GREEN) |
|---|---|---|
| Launch with no sound, 500 ms @ 44.1 kHz, 1 s silence: capture index / engaged level | 22049 (inside the silence) / -200 dB | 66150 (silence end + N) / 0.00 dB |
| Launch with no sound, 3225.8 ms @ 48 kHz, 480-sample blocks, 4 s silence | 154837 / -200 dB | 346838 / 0.00 dB |
| Same: passthrough error while waiting; engage delta vs steady | 0.000000; n/a | 0.000000; 0.031348 vs 0.031340 (44.1k), 0.031395 vs 0.031395 (48k) |
| Partial silence (1 s silence, 1.5 s loop @ 48 kHz): min 10 ms-window RMS over two loop cycles vs tone | -200 dB (gap every cycle) | -0.12 dB |
| Short pauses (100 ms gaps every 200 ms): capture index, continuous vs gapped | 22049 vs 22049 | 22050 vs 22050 |
| Long pause (1 s tone, 400 ms gap, then raise): pending right after the rise / capture index / longest sub-gate run in loop | no / 61740 (immediate) / 17641 | yes / 83790 (gap end + N) / 1 |

The short-pause test passes on both: the samples-only counter never resets, so it can't fail there. It guards against a gate that resets too eagerly.

All 11 earlier `LoopCaptureTests` pass with unchanged assertions and identical logged numbers. One comment in the restart test now notes that its deferred capture lands at index N rather than N-1, because the sine's first sample, sin(0) = 0, is sub-gate. Its assertions only cover indices below N-1, so they still hold. The default-settings fingerprint is still `be7b4e1487c0ce8b`.

### Still open

- Ear test: launch with Loop Mix up and the source stopped, then start the source. The loop should engage ~N + 50 ms later carrying the source. Also check a near-zero dial return.
- Known, bounded edge: if an old loop exists when a capture goes pending (Loop Length lengthened past the audible history, or a park-and-raise within N samples of an onset after more than 250 ms of silence), the switch to passthrough, and back if cancelled, is a step of at most -34 dB of the loop-vs-wet difference.
- Known behavior: with the source stopped, a raised Loop Mix stays pending and passes the silent wet through, dropping any old loop at the -34 dB fire level. That is intended: a silent loop is never what the user wants.

## 2026-10-11 — Reverb space, slice 1: stereo tail, pre-delay, wet EQ

Branch `feat/reverb-stereo-space`. Research and plan: [`reverb-space-research.md`](./reverb-space-research.md) sections 7–8.

### What changed

- **Stereo tail.** `DattorroTank` now has Dattorro's right-channel tap set (7 taps, ×0.6) and exposes it via `getLastRightOutput()`. Before this, the wet tail was effectively mono: one left tap sum copied to both channels. The measured L/R correlation of the impulse response is now 0.000013.
- **Width.** Width is now M/S on the real L/R, range 0–2, default 1. The old shimmer side term keeps its 0.15 gain as `shimmerSideGain`, so width 1 reproduces the old default left channel bit-for-bit.
- **Pre-delay.** New `preDelay` parameter, 0–250 ms, default 0. It is applied to the tank input before the Freeze mute and smoothed per sample.
- **Wet EQ.** New `lowCut` (20–1000 Hz) and `highCut` (1–20 kHz) TPT filters, applied after loop capture and before the mix. Both filters always run so their state stays warm; their output is used only off the endpoints, which keeps the default output bit-identical.
- **Freeze leveler.** Detection stays on `tankOut` (the old signal), not the new stereo mid.
- **Editor.** Three new knobs (Pre-Delay, Low Cut, High Cut). The window grew to 1020×470.

### Measured

| Check | Result |
|---|---|
| Left channel vs pre-change fingerprints (tank, engine) | bit-identical |
| Pre-delay 100 ms onset shift | 100.000 ms |
| High cut 2 kHz, energy above 8 kHz | −32.4 dB |
| Low cut 1 kHz, energy below 200 Hz | −32.8 dB |
| High cut activation at 96 kHz (transition/steady delta), with reset-on-activation vs warm filters | 32.5× (click) → 0.64× |
| Full suite, Debug and Release | ALL TESTS PASSED |

The default-settings sine fingerprint changed by design, from `be7b4e1487c0ce8b` to `106b8609081503f9`: it hashes L+R, and R is new.

### Still open

- Ear test: compare width 0/1/2, pre-delay 0 vs 30–60 ms, and both cuts.
- The width parameter's range changed in place under the same ID. This is acceptable because no presets have shipped.
- Next slices: always-on modulation, RT60 decay, and a Size control (section 7, items 3–5).

## 2026-10-11 — Reverb space, slice 2: Decay in seconds, Size, Modulation

Branch `feat/reverb-size`. Plan: [`reverb-space-research.md`](./reverb-space-research.md) section 9; deviations are in 9.7.

### What changed

- **Decay (RT60).** A new `decay` parameter (0.3–20 s, log skew, default 7.0 s) replaces `feedback`. The tank derives `g = 10^(−3·L_half·size/RT60)` from it, with `L_half` = 362.70 ms (the mean branch length, since the gain is applied once per branch). g is clamped to 0.85 and recomputed whenever Decay or the smoothed Size changes. Freeze and Infinite override it as before. 7.0 s gives g = 0.6993.
- **Size.** A new `size` parameter (0.5–2×, default 1). It scales both branches' allpass1/delay1/allpass2/delay2 and all 14 output taps. The input diffusers are not scaled. Lines are allocated for 2× plus modulation headroom in `prepare()`. Size glides per sample over 300 ms (linear), and only `setDelay()` runs while it moves.
- **Modulation.** A new `modulation` parameter (0–1 → 0–1 ms, default 0.3) on both allpass1 lines, reusing Infinite's Thiran lines and its 0.5/0.7 Hz LFOs. Effective depth is `max(user, infinite × 0.5 ms)`. It is ramped per sample over 50 ms.
- **Tap restore.** `peekTap()` now restores the configured delay from the branch's stored value instead of `getDelay()`. At fractional delays, `getDelay()` can be off by one ulp.
- **Editor.** Feedback became Decay (seconds, with a tooltip about the ceiling). Size and Modulation joined the second row. The window size is unchanged.

### Measured

| Check | Result |
|---|---|
| Golden hashes at Modulation 0 + `decaySecondsForGain(0.7f)` (7.024514 s) | tank `475497026ccf0ceb`, engine L `918d1cec9419a789`, engine sine `106b8609081503f9`: all unchanged |
| Bare-tank RT60, Schroeder T30 fit (Size 1: 2 s / 5 s) | 2.248 s (+12.4%) / 5.522 s (+10.4%) |
| Bare-tank RT60 (Size 2: 2 s / 5 s) | 2.274 s (+13.7%) / 5.611 s (+12.2%) |
| RT60 4 s at Size 0.5 vs Size 2 | 4.164 s vs 4.522 s (ratio 1.086) |
| Max g over a Decay × Size sweep, and while Size glides | 0.850000 / 0.850000 |
| First tap arrival, Size 1 → 2 | 392 → 786 samples (ratio 2.005) |
| Size glide 1→2 (Thiran), max sample delta glide/steady | 1.046× |
| Tank Size automation, Modulation 1, g 0.85, 20 s noise | peak 2.08 |
| Frozen tank at Size 2 + Modulation 1 + g 0.85, 180 s noise | peak 8.85 (bound 10) |
| Engine Size automation / 180 s stress (Size 2, Modulation 1, Decay 20 s, Freeze from 60 s) | peak 1.89 / 1.96 |
| Engine decay to silence at those settings | final-second peak 0 at 40 s (3.05e-4 at 20 s) |
| Full-engine RT60, Decay 2 s / 7 s | 2.07 s / 6.20 s |
| Real-time factor at defaults (Release) | 0.043 (slice 1: ~0.037) |

The bare tank runs 10–14% long against the nominal value. The cause is allpass energy storage: the energy-weighted delay of an allpass is `M/(1−g²)`, which gives an effective `L_half` of about 413 ms. Correcting for it would move the 7 s default to g ≈ 0.666, so that change is left open.

**PitchShifter offset stability** (block size 1, last 5 s of 10 s, Shimmer Amount 1). Each cell is the mean primary−quadrature offset divergence / the number of primary-offset jumps over 10 samples.

| Tone | Mod 0 | Mod 0.3 | Mod 1.0 |
|---|---|---|---|
| 220 Hz | 164.3 / 6 | 185.8 / 7 | 70.0 / 89 |
| 330 Hz | 27.4 / 16 | 91.9 / 13 | 24.0 / 70 |

At 0.3, the jump count matches Mod 0, but the divergence rises (×1.1 at 220 Hz, ×3.4 at 330 Hz). At 1.0, jumps rise 5–15×.

**Shimmer Sustain vs Modulation.** The tail-decay gap between Sustain 0 and Sustain 1 is 2.4 dB at Modulation 0, about 0 at 0.2–0.3, and −0.6 dB at 1.0. Modulation decorrelates the direct and shifter-delayed loop copies, and the comb loss Sustain relies on goes with that correlation. The Sustain test is now pinned to Modulation 0, and the dependence is logged.

### Still open

- **Resolved, 2026-10-11:** the user compared 0 / 0.1 / 0.3 by ear and chose 0.1 as the Modulation default. 0.3 had removed Sustain's decay effect and raised the shifter's offset divergence. The diagnostics at 0.1 have not been measured yet.
- Whether to calibrate `L_half` for allpass storage (+14%).
- The Thiran glide measured as click-free (1.05×). Lagrange reads at fractional Size add a slight per-pass HF loss off Size 1.

## 2026-10-25 — Formant Correction toggle (default off)

**Status: code complete, unit-tested in Debug and Release. Not ear-verified.**

### Why

A measured diagnosis showed that `FormantEnvelopeCorrector` is numerically unstable on tonal input inside the feedback loop. After a +12 st shift of a 440 Hz sine it removed about 59 dB of the 880 Hz octave. It also rang at 440 Hz with internal peaks near 2.6e6, and `SafetyLimiter` clipped that into broadband noise inside the loop. With the corrector bypassed, the octave cascade is clean: 880 → 1760 → 3520 Hz, about one octave per 0.5 s. The user chose a toggle, off by default. Fixing the corrector itself is a separate task.

### What changed

- **Engine.** `ShimmerReverbEngine::setFormantCorrection (bool)`, with `defaultFormantCorrection = false`. A 20 ms per-sample linear crossfade (`juce::SmoothedValue`) blends the raw shifted signal with the corrected one, on both the primary and the quadrature paths, at the corrector's old chain position. While the weight rests at 0, the corrector is not run at all. That makes "off" bit-identical to having no corrector in the chain. On the idle → active edge the corrector is `reset()` first, which is allocation-free (`std::fill` and array fills only). The setting survives `prepare()`/`reset()`, and `reset()` snaps the crossfade to its target.
- **Parameter.** `formantCorrection` (`AudioParameterBool`, version 1, "Formant Correction", default false). PluginProcessor reads it raw every block and before `prepare()`, the same way as Infinite.
- **Editor.** A "Formant Correction" toggle in the top bar, to the left of Infinite, with a `ButtonAttachment`.

### Measured

| Check | Result |
|---|---|
| 880 Hz band share, 1.0–1.5 s after a 1 s 440 Hz burst (+12 st, Shimmer Feedback 1), off / on | −5.45 dB / −19.79 dB (bound > −15 dB) |
| Peak in that run, off | 0.83 |
| Golden hashes with correction on (mono `933a970f88302962`, left `e30cadee8d7121fb`, legacy T6 `106b8609081503f9` / `918d1cec9419a789`) | all unchanged, no re-baseline |
| On → off → `reset()` vs a never-enabled engine with the same history | bit-identical after the reset |
| Switch click test (max delta in the 30 ms after the switch / in the 0.5 s before it), on / off | 0.79 / 0.86 (bound 3) |

The click test is a guard, not proof that the ramp is needed. A hard switch measured the same, because the tank's delay network smooths the in-loop step before it reaches the output. The ramp is kept anyway, since a hard step between two in-loop signals is a discontinuity by construction.

Opted into correction on, to keep the pre-change measurements they encode: the three golden-hash tests (via `legacySlice1Settings()`), and the legacy-path "Shimmer Sustain: a HIGH value decays slower" test. With correction off, that test measured −31.34 / −31.38 dB, so its gap disappears.

### Still open

- Fix `FormantEnvelopeCorrector`'s instability on tonal input (separate bug).
- Ear check of the default-off cascade and of the toggle.
- `FormantEnvelopeCorrector::reset()` zeroes `currentSemitones` but keeps `armed`. That matches the state after `prepare()` that the golden hashes encode, so it was left as is.

## 2026-10-25 — Rename: Shimmer Sustain → Shimmer Feedback identifiers

**Status: rename only, no behaviour change. Unit-tested in Debug and Release; golden hashes unchanged.**

The dial has been un-inverted and labelled "Shimmer Feedback" since 2026-10-24, but its identifiers still said "Sustain", which suggested the old inverted meaning. Nothing has shipped, so they were renamed now.

- **Parameter.** `ParamIDs::shimmerSustain` → `ParamIDs::shimmerFeedback`, string ID `"shimmerSustain"` → `"shimmerFeedback"` (`ParameterID` version stays 1). A saved state from before (including the user's Standalone state) loses this one value and falls back to the default, 0.55.
- **Engine and plugin.** `setShimmerSustain` → `setShimmerFeedback`, `shimmerSustainAmount` → `shimmerFeedbackAmount`, `defaultShimmerSustainAmount` → `defaultShimmerFeedbackAmount`; `shimmerFeedbackParam`, `smoothedShimmerFeedback` and the editor's `shimmerFeedback*` slider, label and attachment.
- **Tank.** `DattorroTank::setShimmerFeedbackGain` / `shimmerFeedbackGain` / `defaultShimmerFeedbackGain` → `setShimmerAmountGain` / `shimmerAmountGain` / `defaultShimmerAmountGain`. They are driven by Shimmer Amount, and the old names would now read as the Feedback dial.
- **Tests.** Calls and test names updated. Legacy-path tests are named "Shimmer Feedback (legacy path)"; their inverted mapping is untouched. Dated comments that say "Shimmer Sustain" are kept as history.

## 2026-10-25 — Shimmer Tone (slice 1, step C)

**Status: code complete, unit-tested in Debug and Release, plugin builds. Not ear-verified.**

### Why

Step C of [`shimmer-character-research.md`](./shimmer-character-research.md) section 7.1 item 3. The shifted path had no band-limiting. Content above fs / (2 · ratio) folded back as aliases, and nothing made the upper generations of the cascade roll off. That becomes a problem now that the shifted share is large (step B). The user chose the ratio-tracking alias cap (section 8, decision 6).

### What changed

- **Engine.** `ShimmerReverbEngine::setShimmerTone (float hz)`, clamped to 1.5–10 kHz, default 5 kHz (`min/max/defaultShimmerToneHz`). Two cascaded mono `StateVariableTPTFilter` lowpass sections (4th order) filter `dryFeedback` before it reaches `shifter` and the formant corrector's dry analysis. `unityShifter` still gets the raw signal, so Freeze is untouched. The effective cutoff is `min(Tone, fs / (4 · max(ratio, 1)), 0.49 fs)` with ratio = 2^(st / 12), recomputed once per block. At 44.1 kHz that is 5512.5 Hz at +12 st and 2756.25 Hz at +24 st. Downward shifts count as ratio 1, so their cap is fs / 4, which is above the range from 40 kHz up.
- **Off endpoint.** At 10 kHz the filters keep running warm, but the shifter gets the raw signal. This is the same pattern as the wet EQ endpoints. Off is the legacy path and applies no alias cap. The setting survives `prepare()`/`reset()`.
- **Parameter.** `shimmerTone` ("Shimmer Tone", version 1), skewed so the geometric midpoint (~3.87 kHz) sits at the knob's centre, like the wet EQ corners. Its value text reads "5000 Hz" and "Off" at the top. That text lives in the parameter (`AudioParameterFloatAttributes`), not in an editor lambda: `SliderAttachment` overwrites the slider's `textFromValueFunction` with the parameter's own text, and a slider suffix would read "Off Hz". So hosts show "Off" too. PluginProcessor caches the raw pointer, pushes the value before `prepare()`, and runs it through a 50 ms block-rate `SmoothedValue` every block, like Low Cut/High Cut.
- **Editor.** A "Shimmer Tone" knob right after Shimmer Feedback in the first row, with a tooltip about the cap. The window grows from 1020 to 1120 px to keep ~100 px per knob. The second row now uses the 11-knob width.
- **Tests.** A new `ShimmerReverbEngineToneTests` class (8 tests). T6 and the legacy "decays slower" test now set Tone Off explicitly. T2 is asserted with Tone Off (see below).

### Measured (Release)

| Check | Result |
|---|---|
| Effective cutoff at 44.1 kHz: +12 / +24 / −12 (Tone 3 kHz) / Off | 5512.5 / 2756.25 / 3000 Hz / inactive |
| T3 alias band, Tone 9999 Hz vs Off, Modulation 0: (a) +12, 15 kHz → 14.1 kHz | 17.87 vs 66.73 dB (−48.86 dB); floor with Amount 0: 13.21 dB |
| T3 (b) +24, 8 kHz → 12.1 kHz | −1.49 vs 38.13 dB (−39.62 dB); floor 0.10 dB |
| Darkening, 440 Hz burst, Feedback 1, +12: 7040 Hz+ bands at 2.75 s, 2 kHz vs Off | 6.96 vs 33.18 dB (−26.22 dB) |
| Same run, 3520 Hz+ bands | 47.63 vs 48.20 dB (−0.57 dB) |
| 880 Hz band at 1.25 s, Off / 5 kHz / 2 kHz | 65.04 / 64.85 / 64.86 dB |
| Off after a 2 kHz excursion + `reset()` vs Off throughout | 0 mismatching samples |
| Full Freeze, Tone Off → 1.5 kHz | 0 mismatching samples (control at Freeze 0: 74997) |
| Tone 1.5 kHz, Feedback 1, +24 / −24 st, 180 s noise | peak 1.52 / 1.65; final-second peak after 40 s of silence 0 / 0 |
| Click test, Off → 1.5 kHz / 1.5 kHz → Off (ratio of max deltas, bound 3) | 1.07 / 1.29 |
| Golden hashes, default (mono / left) | `933a970f88302962` → `7149627bc88d4fcf`, `e30cadee8d7121fb` → `380d36dfca706ba2` (by design, 5 kHz default) |
| Legacy T6 hashes (Tone Off + legacy blend + formant on) | `106b8609081503f9` / `918d1cec9419a789`, unchanged |
| T1 (440−220) improvement at 1.5 s | 11.18 dB (bound 9) |
| Real-time factor at Freeze 0 (unity idle) | 0.0347 |

Notes on the measurements:

- **The 3520 Hz band barely moves at 2 kHz.** Each pass filters before it shifts, so an output octave is attenuated by the filter's response at the octave below it. 3520 Hz comes from a 1760 Hz input, just under the cutoff. The darkening test therefore asserts on 7040 Hz and up.
- **T3 needs Modulation 0.** At the 0.1 default, the tank's allpass modulation spreads the 15 kHz input into a skirt. That put the alias band's floor at 34.65 dB even with no shifter, only 26.5 dB under the Off alias. The capped run already sat on that floor.
- **T2 (energy-neutral blend) failed at the default Tone.** RT60 at shimmerWeight 0 / 0.5 / 0.85 measured 5.22 / 4.79 / 4.17 s, against a ±10% bound. The low-pass removes the impulse's top end from the shifted share on every pass. That is an in-loop, frequency-dependent loss that grows with the share, like Damping applied to the shifted path only. T2 is now asserted with Tone Off, so it stays a test of the blend law: 5.22 / 5.29 / 5.40 s, within ±10%. The Tone-on figures are logged.
- **Spec T3 asked for "Tone at its 10 kHz maximum (not Off)".** 10 kHz is Off, so 9999 Hz stands in for it.

### Still open

- Ear check of builds B → C: the 3 / 5 / 8 kHz sweep, and +19/+24, where the cap darkens the shimmer.
- Whether the shorter broadband tail at high Shimmer Feedback with the Tone filter (−20% RT60 at 0.85) is acceptable by ear.
- No APVTS round-trip test for `shimmerTone`: the test project has no `juce_audio_processors`, a documented gap. pluginval was not run.

## References

- [`shimmer-reverb-implementation-plan.md`](./shimmer-reverb-implementation-plan.md) — the plan this journal tracks against
- [`shimmer-reverb-concepts.md`](./shimmer-reverb-concepts.md)
- [`../.claude/skills/juce-plugin-dev/references/shimmer-reverb-architecture.md`](../.claude/skills/juce-plugin-dev/references/shimmer-reverb-architecture.md)
