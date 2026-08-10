---
plugin: JUCE
date: 2026-08-10
problem_type: api_misuse
component: juce_dsp
symptoms:
  - "error: no member named 'setSampleRate' in 'juce::dsp::Reverb'"
  - "error: no member named 'processMono' in 'juce::dsp::Reverb'"
  - "error: no member named 'processStereo' in 'juce::dsp::Reverb'"
root_cause: wrong_api
juce_version: 8.0.0
resolution_type: api_migration
severity: critical
tags: [dsp, reverb, api-migration, juce-8, processblock, imported-reference]
---

# Troubleshooting: `juce::dsp::Reverb` API mismatch (JUCE 8)

**Provenance note:** this is imported reference knowledge from
[glittercowboy/plugin-freedom-system](https://github.com/glittercowboy/plugin-freedom-system)
(troubleshooting doc for its "FlutterVerb" example plugin), not an incident that happened
in Millenia. `Source/DSP/DattorroTank.cpp` is a fully custom tank and doesn't call
`juce::dsp::Reverb` today, so this doesn't currently apply — filed here because it's the
exact trap this project would fall into if `juce::dsp::Reverb` is ever pulled in for a
secondary reverb voice.

## Problem
Code mixed the legacy `juce::Reverb` API (`setSampleRate()`, `processMono()`,
`processStereo()`) with a `juce::dsp::Reverb` declaration, which uses a different API.

## Symptoms
```
error: no member named 'setSampleRate' in 'juce::dsp::Reverb'
error: no member named 'processMono' in 'juce::dsp::Reverb'
error: no member named 'processStereo' in 'juce::dsp::Reverb'
```
Compiles fine if `juce::Reverb` (non-DSP) is used instead — which is the giveaway that
the two APIs got crossed.

## What didn't work
N/A — root cause was visible directly from the compiler errors (member-not-found on a
`juce::dsp::` type calling non-DSP method names).

## Solution

```cpp
// prepareToPlay() — before (broken, old juce::Reverb API):
reverb.setSampleRate(sampleRate);
reverb.reset();

// prepareToPlay() — after (juce::dsp::Reverb API):
juce::dsp::ProcessSpec spec;
spec.sampleRate = sampleRate;
spec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock);
spec.numChannels = static_cast<juce::uint32>(getTotalNumOutputChannels());
reverb.prepare(spec);
reverb.reset();
```

```cpp
// processBlock() — before (broken):
if (buffer.getNumChannels() == 1)
    reverb.processMono(buffer.getWritePointer(0), buffer.getNumSamples());
else if (buffer.getNumChannels() == 2)
    reverb.processStereo(buffer.getWritePointer(0), buffer.getWritePointer(1), buffer.getNumSamples());

// processBlock() — after:
juce::dsp::AudioBlock<float> block(buffer);
juce::dsp::ProcessContextReplacing<float> context(block);
reverb.process(context);
```

## Why this works
There are two distinct reverb classes in JUCE:

1. **`juce::Reverb`** (non-DSP module) — `setSampleRate()`, `processMono()`, `processStereo()`.
2. **`juce::dsp::Reverb`** (DSP module) — `prepare(spec)`, `process(context)`, channel-agnostic
   via `AudioBlock`/`ProcessContextReplacing`.

Declaring the `dsp::` type but calling the non-DSP methods doesn't compile because they're
genuinely different classes with no shared API surface — this isn't a deprecation, it's
two independent implementations that happen to share the word "Reverb".

## Prevention
1. If the include is `<juce_dsp/juce_dsp.h>` and the member type is `juce::dsp::X`, every
   call on it must be the `prepare(spec)` / `process(context)` pattern — no per-parameter
   setters, no raw-pointer `processMono`/`processStereo`.
2. `ProcessSpec` (sampleRate, maximumBlockSize, numChannels) is the one shape every
   `juce::dsp::*` component expects in `prepare()`.
3. Cross-check against the JUCE docs for the exact class in use, not the sibling
   non-DSP class of the same name.

## Related issues
None yet in this project.

## References
- `juce::dsp::Reverb`: https://docs.juce.com/master/classdsp_1_1Reverb.html
- `juce::dsp::ProcessSpec`: https://docs.juce.com/master/structdsp_1_1ProcessSpec.html
- JUCE DSP introduction tutorial: https://docs.juce.com/master/tutorial_dsp_introduction.html
