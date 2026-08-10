---
name: juce-plugin-dev
description: "Trigger: JUCE, VST3, AU, AudioProcessor, processBlock, APVTS, PluginProcessor, PluginEditor, DSP, audio parameter. Apply real-time safety, parameter, and state rules for JUCE audio plugin code."
license: Apache-2.0
metadata:
  author: "Abaranasia"
  version: "1.0"
---

## Activation Contract
Apply when writing or editing: `PluginProcessor`, `PluginEditor`, any DSP class, `processBlock`, `AudioProcessorValueTreeState` (APVTS) setup, or plugin state (get/setStateInformation).

## Hard Rules
- **`processBlock` is real-time**: no heap allocation (`new`, `std::vector::push_back`, `String` construction), no locks/mutexes, no blocking I/O, no exceptions, no logging. Pre-allocate all buffers in `prepareToPlay`.
- **Parameters live in one `AudioProcessorValueTreeState`** owned by the processor. Read from the audio thread only via `std::atomic`-backed `getRawParameterValue(id)->load()`, never the raw parameter object.
- **GUI never touches processor state directly.** The editor reads/writes via APVTS attachments (`SliderAttachment`, etc.), not by calling processor methods.
- **State persistence**: `getStateInformation`/`setStateInformation` serialize the APVTS `ValueTree` via `copyState()`/`replaceState()`, not ad-hoc member variables. Store a schema version int so old presets don't crash newer plugin versions.
- **Denormals**: call `juce::ScopedNoDenormals` at the top of every `processBlock` that does floating-point DSP.
- **Double precision**: check `getProcessingPrecision()` / override `supportsDoublePrecisionProcessing()` deliberately — don't silently truncate a host's double-precision request to float.
- **Latency**: if DSP introduces delay (lookahead, oversampling, FIR), call `setLatencySamples()` in `prepareToPlay`.
- **Bypass**: implement as a `juce::AudioParameterBool` routed through APVTS, not a processor-only flag invisible to automation.

## Decision Gates
| Situation | Do this |
|---|---|
| Adding a new plugin parameter | Add it in one place, `createParameterLayout()`; give it a stable string ID that never changes across versions |
| Renaming/removing a parameter ID, narrowing its range, or changing its type (float→choice etc.) on a parameter that has ever shipped | Breaking change once any preset/session/automation exists — it silently invalidates saved automation and preset values. Before doing it, confirm no preset relies on the old ID/range; if unsure, add the new one alongside instead of mutating in place |
| DSP needs a buffer bigger than one block | Allocate/resize in `prepareToPlay`, never inside `processBlock` |
| GUI thread needs data from the audio thread | `std::atomic` for scalars, `AbstractFifo`/ring buffer for block data — never a `juce::CriticalSection` on the audio thread |

## Execution Steps
1. Before editing `processBlock`, scan the diff for any allocation, lock, or exception path introduced — remove it or move it to `prepareToPlay`.
2. Before adding a parameter, check `createParameterLayout()` for an existing similar one to match ID/range/skew conventions.
3. Before changing state serialization, bump the schema version and add a migration branch for older versions.

## Output Contract
Before reporting DSP/processor work done, confirm: no allocation/lock/exception on the audio-thread path touched, parameters routed through APVTS, state changes are versioned.

## References
- `references/shimmer-reverb-architecture.md` — topology, JUCE class mapping, and pitfalls for the shimmer reverb DSP
