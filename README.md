# Millenia

Audio FX plugin built with [JUCE](https://juce.com/).

## Building

1. Open `Millenia.jucer` in the Projucer.
2. Save the project to (re)generate the `Builds/` and `JuceLibraryCode/` folders (both gitignored, always regenerated locally).
3. Open `Builds/VisualStudio2026/Millenia.sln` in Visual Studio and build.

## Structure

- `Source/` — plugin source (`PluginProcessor`, `PluginEditor`)
- `Millenia.jucer` — Projucer project definition
