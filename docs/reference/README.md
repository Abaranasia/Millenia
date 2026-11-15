# External Reference Material

Design and DSP documentation pulled from other JUCE reverb plugins for comparison while
building Millenia's shimmer reverb. This is **not** design history for this project —
it's imported research to sanity-check our own decisions in `Source/DSP/` against
independently-designed alternatives.

**Source:** [glittercowboy/plugin-freedom-system](https://github.com/glittercowboy/plugin-freedom-system)
(a multi-agent JUCE plugin scaffolding system). Only the design/DSP specs of two of its
example plugins were extracted — the orchestration tooling itself (stage agents, WebView
UI pipeline, aesthetic system) doesn't apply to Millenia's architecture and was left out.

| Doc | Plugin | Why it's relevant |
|---|---|---|
| [lushverb-shimmer-reverb.md](lushverb-shimmer-reverb.md) | LushVerb | Closest analog to Millenia: reverb + octave-up shimmer + built-in modulation + dry/wet. Uses a phase-vocoder (FFT) pitch shifter and `juce::dsp::Reverb`, vs. our `DattorroTank` + custom `PitchShifter` — useful contrast on latency, CPU, and modulation depth tradeoffs. |
| [flutterverb-tape-reverb.md](flutterverb-tape-reverb.md) | FlutterVerb | Not a shimmer reverb, but its wow/flutter dual-LFO modulation and `juce::dsp::Reverb` API usage (see [../../troubleshooting/api-usage/juce-dsp-reverb-api-mismatch.md](../../troubleshooting/api-usage/juce-dsp-reverb-api-mismatch.md)) are relevant if we ever add pitch-modulated flutter/chorus to the tank. |
