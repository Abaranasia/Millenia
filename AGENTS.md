# Agent Skills — Millenia (JUCE Plugin)

Project-scoped skills under `.claude/skills/`, applied automatically by trigger context.

| Skill | Applies to | Covers |
|---|---|---|
| [juce-plugin-dev](.claude/skills/juce-plugin-dev/SKILL.md) | `PluginProcessor`, `PluginEditor`, DSP classes, `processBlock`, APVTS, plugin state | Real-time audio-thread safety, parameter management, state persistence, latency/bypass handling |
| [juce-plugin-testing](.claude/skills/juce-plugin-testing/SKILL.md) | Test setup, DSP unit tests, release validation | `juce::UnitTest` DSP tests, pluginval host-compatibility validation |

## Feature docs

- [docs/shimmer-reverb-concepts.md](docs/shimmer-reverb-concepts.md) — human-facing concepts, architecture rationale, roadmap, and citations for the shimmer reverb effect
- [.claude/skills/juce-plugin-dev/references/shimmer-reverb-architecture.md](.claude/skills/juce-plugin-dev/references/shimmer-reverb-architecture.md) — dense implementation reference (topology, JUCE class mapping, pitfalls) linked from `juce-plugin-dev`
