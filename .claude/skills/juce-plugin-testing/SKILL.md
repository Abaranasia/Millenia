---
name: juce-plugin-testing
description: "Trigger: pluginval, JUCE tests, unit test, plugin validation, CI, DSP test. Apply JUCE plugin testing workflow: pluginval host validation and DSP unit tests."
license: Apache-2.0
metadata:
  author: "Abaranasia"
  version: "1.0"
---

## Activation Contract
Apply when setting up, writing, or running tests for this JUCE plugin: unit tests for DSP/processor logic, or host-compatibility validation.

## Hard Rules
- **Validate the built plugin with pluginval** (Tracktion/pluginval) at strictness level 10 before calling a build release-ready: `pluginval --strictness-level 10 --validate <path-to-vst3>`. It catches threading, parameter-automation, and state-restore bugs unit tests miss.
- **Unit-test DSP logic, not JUCE itself.** Use `juce::UnitTest` (built into JUCE, no extra dependency) unless the project already depends on Catch2 — don't add a second test framework.
- **Test parameter round-trips**: for every parameter, assert `setStateInformation(getStateInformation())` restores the same value.
- **Test at multiple block sizes and sample rates** (e.g. 1, 7, 512, 4096 samples; 44100/48000/96000 Hz) — DSP tested at only one block size hides off-by-one and buffer-boundary bugs.
- **Test feedback stability on any regenerative path** — `DattorroTank`'s cross-feeding branches and `PitchShifter`-in-a-loop shimmer regen can both diverge if a gain/damping coefficient is wrong. Process a sustained signal for hundreds of blocks and assert peak amplitude stays bounded (see `references/dsp-stability-tests.md`); this is the one test category a plain impulse-response check won't catch.
- **Test silent input on every DSP component that has internal state** (tank delay lines, pitch-shifter buffers) — assert output stays near the noise floor. Catches uninitialized/uncleared buffers and denormal build-up that pluginval won't always surface.
- **Never assert on real-time timing behavior via wall-clock in unit tests** — that's flaky. Test correctness (output values, state), not speed. The exception is a *relative* CPU budget check (real-time factor = processing time / audio time), which is a correctness property for a real-time system, not a timing flake — algorithmic reverbs should stay under ~0.15 real-time factor in a Release build.

## Decision Gates
| Situation | Do this |
|---|---|
| New DSP algorithm added | Add a `juce::UnitTest` subclass exercising known input→output cases (impulse, silence, DC, Nyquist) |
| DSP component has feedback/regeneration (tank, shimmer regen) | Add the sustained-input stability test — do this before wiring it into `ShimmerReverbEngine`, not after |
| New parameter added | Add a state round-trip test and a pluginval automation pass |
| Changing an existing parameter's ID, range, or type | Treat as breaking once any preset/session exists — see the equivalent gate in `juce-plugin-dev` |
| Preparing a release build | Run `pluginval --strictness-level 10` against every plugin format target (VST3, Standalone) before tagging |

## Execution Steps
1. Locate or create a test target running `juce::UnitTestRunner` (standalone console app or debug-build hook).
2. For DSP changes, write the failing test first, then implement.
3. Before marking a build release-ready, run pluginval against the actual built binary, not just unit tests.

## Output Contract
Before reporting testing work done, confirm: new DSP/parameters have unit tests, state round-trip is covered, any regenerative path has a bounded-output stability test, pluginval has been run against the built plugin binary.

## References
- `references/dsp-stability-tests.md` — `juce::UnitTest` templates for the silent-input, feedback-stability, and CPU-budget checks called out above, written against `DattorroTank`/`PitchShifter`/`ShimmerReverbEngine`
