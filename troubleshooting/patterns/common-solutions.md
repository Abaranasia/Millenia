# Common Solutions / Critical Patterns

Cross-cutting patterns that apply beyond a single incident — generic enough to bite any
future JUCE work in this project, not tied to one DSP class or one bug. Each entry links
back to its full troubleshooting doc for the detailed symptoms/stack traces.

## Diagnosing a Standalone crash with no visible error dialog

**Applies to**: any JUCE Standalone build on Windows that silently disappears with no
on-screen message.

**Pattern**:
1. Check `%LOCALAPPDATA%\CrashDumps\<exe>.<pid>.dmp` first — Windows Error Reporting
   captures a full minidump by default on an unhandled exception, with no extra setup.
2. Cross-check the Windows Event Viewer "Application" log for `Application Error` events
   for the exe name — gives the exception code (e.g. `0xc0000005` = access violation) and
   faulting module/offset immediately, before even opening a dump.
3. Install WinDbg (`winget install Microsoft.WinDbg`) if no command-line debugger is
   available — despite being a GUI-first app, it bundles a fully headless `cdb.exe` under
   its install directory (`...\WindowsApps\Microsoft.WinDbg_<version>_x64__.../amd64/`)
   that resolves symbols against the project's own Debug PDBs. Run:
   ```
   cdb.exe -z <path-to-dump> -c "!analyze -v; q"
   ```
   This gives a symbolized stack trace and root-cause bucket without needing a live
   debugger attach or a reproducible repro case — critical when the crash is a
   timing-dependent race that doesn't reproduce on demand.
4. Before assuming the crash is in project-owned code (DSP classes, `PluginProcessor`,
   etc.), check whether the crashing stack frames are actually inside `juce::` namespace
   code. A stack entirely within JUCE's own classes (`AudioProcessorPlayer`,
   `AudioDeviceManager`, `WasapiClasses::*`, etc.) with zero project frames means the bug
   is in the framework's Standalone wrapper, not the plugin's DSP — and specifically means
   it **cannot** affect the real VST3/AU build, since a DAW host never touches
   `AudioDeviceManager`/`AudioProcessorPlayer` (those only exist in the Standalone target).

**First seen**: [Standalone crashes when switching audio device live](../runtime-issues/standalone-audio-device-switch-crash-20260811.md) (2026-08-11) — a JUCE 9.0.0 `AudioProcessorPlayer` race during a live output-device switch, not a Millenia DSP bug.

**Prevention checklist**:
- [ ] Reproduce, but don't stop there if it doesn't reproduce on demand — check for
      existing crash dumps from earlier attempts first.
- [ ] Confirm whether the fault is inside `juce::` code or project code before writing a
      code-level fix.
- [ ] File one troubleshooting doc per distinct symptom under `troubleshooting/<category>/`
      even when (like here) the actual fix is a workflow change, not a code diff.
