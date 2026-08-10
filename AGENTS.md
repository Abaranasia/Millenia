# Agent Skills — Millenia (JUCE Plugin)

Project-scoped skills under `.claude/skills/`, applied automatically by trigger context.

| Skill | Applies to | Covers |
|---|---|---|
| [juce-plugin-dev](.claude/skills/juce-plugin-dev/SKILL.md) | `PluginProcessor`, `PluginEditor`, DSP classes, `processBlock`, APVTS, plugin state | Real-time audio-thread safety, parameter management, state persistence, latency/bypass handling |
| [juce-plugin-testing](.claude/skills/juce-plugin-testing/SKILL.md) | Test setup, DSP unit tests, release validation | `juce::UnitTest` DSP tests, pluginval host-compatibility validation |
| [troubleshooting-docs](.claude/skills/troubleshooting-docs/SKILL.md) | Right after a non-trivial JUCE/DSP/build bug is confirmed fixed | Captures the fix as a categorized, schema-validated doc under `troubleshooting/` so it's searchable next session |

## Feature docs

- [docs/shimmer-reverb-concepts.md](docs/shimmer-reverb-concepts.md) — human-facing concepts, architecture rationale, roadmap, and citations for the shimmer reverb effect
- [.claude/skills/juce-plugin-dev/references/shimmer-reverb-architecture.md](.claude/skills/juce-plugin-dev/references/shimmer-reverb-architecture.md) — dense implementation reference (topology, JUCE class mapping, pitfalls) linked from `juce-plugin-dev`
- [troubleshooting/](troubleshooting/README.md) — solved-bug knowledge base (schema in `.claude/skills/troubleshooting-docs/schema.yaml`)
- [docs/reference/](docs/reference/README.md) — external design/DSP docs from comparable reverb plugins (LushVerb, FlutterVerb), imported for comparison against our own shimmer reverb design
