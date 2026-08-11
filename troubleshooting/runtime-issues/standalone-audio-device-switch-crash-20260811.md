---
plugin: JUCE
date: 2026-08-11
problem_type: runtime_error
component: system
symptoms:
  - "Application Error (Event Viewer): Millenia.exe, código de excepción 0xc0000005, módulo con errores Millenia.exe"
  - "Application Error (Event Viewer): Millenia.exe, código de excepción 0xc0000005, módulo con errores VCRUNTIME140D.dll"
  - "!analyze -v: INVALID_POINTER_READ_c0000005_Millenia.exe!juce::AudioProcessorPlayer::audioDeviceIOCallbackWithContext, Attempt to read from address 0000000000000000"
  - "!analyze -v (second dump): VCRUNTIME140D!memcpy_avx_ermsb_Intel crash inside juce::initialiseIoBuffers, called from juce::AudioProcessorPlayer::audioDeviceIOCallbackWithContext, Attempt to read from address 0000000000000000"
root_cause: thread_violation
juce_version: 9.0.0
resolution_type: environment_setup
severity: critical
tags: [standalone, audiodevicemanager, audioprocessorplayer, wasapi, device-switch, race-condition, crash]
---

# Troubleshooting: Millenia Standalone crashes when switching audio device live

## Problem
The `Millenia - Standalone Plugin` build (JUCE Standalone wrapper, WASAPI backend on
Windows) crashed with an access violation twice while validating Phase 2's `DattorroTank`
by ear: once while switching the Standalone's **output** device to a Focusrite interface
mid-stream (audio actively playing through the plugin at the time), and again in an
earlier session under a similar live-device-change scenario.

## Symptoms
Two Application Error events in Windows Event Viewer, both `0xc0000005` (access
violation), for `Millenia.exe`, with no on-screen error — the app just silently
disappears. Confirmed via crash dumps (`%LOCALAPPDATA%\CrashDumps\Millenia.exe.*.dmp`)
analyzed with `cdb.exe -z <dump> -c "!analyze -v; q"` (from the WinDbg package,
`winget install Microsoft.WinDbg`, which bundles a headless-usable `cdb.exe` even though
the main app is GUI-only):

```
Failure.Bucket: INVALID_POINTER_READ_c0000005_Millenia.exe!juce::AudioProcessorPlayer::audioDeviceIOCallbackWithContext
Attempt to read from address 0000000000000000
FAULTING_SOURCE_LINE: H:\Proyectos\Juce\Projucer\modules\juce_audio_utils\players\juce_AudioProcessorPlayer.cpp
FAULTING_SOURCE_LINE_NUMBER: 260
```

Second dump, same call site one frame deeper:

```
Failure.Bucket: INVALID_POINTER_READ_c0000005_VCRUNTIME140D.dll!memcpy_avx_ermsb_Intel
STACK_TEXT: ... Millenia!juce::initialiseIoBuffers+0x273 : Millenia!juce::AudioProcessorPlayer::audioDeviceIOCallbackWithContext+0x2ee ...
Attempt to read from address 0000000000000000
```

Both stacks are **entirely inside JUCE framework code** (`AudioProcessorPlayer`,
`AudioDeviceManager`, `WasapiClasses::WASAPIAudioIODevice::run`) — neither
`DattorroTank`, `PluginProcessor`, nor any other Millenia-owned code appears anywhere in
either stack. The crash happens before `AudioProcessor::processBlock` is ever reached.

## What didn't work
- Assuming it was a `DattorroTank` bug (the DSP work in progress at the time) and trying
  to reproduce it purely by testing DSP changes — the crash is unrelated to the DSP
  content and reproduces regardless of which tank (`ScratchSchroederTank`/`DattorroTank`)
  is active.
- Trying to reproduce live under a debugger on request — the race is timing-dependent and
  didn't reproduce on demand; the post-mortem crash dumps (already captured by Windows
  Error Reporting from the earlier, unattended crashes) were what actually pinned down the
  root cause, not a fresh repro.

## Solution
There is no code fix in this project — the bug lives in JUCE's `AudioProcessorPlayer`
(`juce_audio_utils/players/juce_AudioProcessorPlayer.cpp:260`, JUCE 9.0.0) and
`AudioDeviceManager`'s live device-switching path, not in Millenia's own source. The
practical mitigation is a testing-workflow change:

```
# Before (crashes):
1. Start audio playing into the Standalone (e.g. via a Stereo Mix loopback input).
2. While audio is actively streaming, open Audio/MIDI Settings and change the
   Output Device (or Input Device) live.
   -> AudioDeviceManager tears down/recreates the WASAPI device while the audio
      callback thread is still mid-flight on the old device; AudioProcessorPlayer's
      currentDevice / channel pointers go stale mid-callback -> null dereference.

# After (safe):
1. Stop the audio source (or select "No Device") before changing Input/Output Device
   in Audio/MIDI Settings.
2. Change the device.
3. Resume playback.
   Alternatively, close and relaunch the Standalone after changing the OS-level
   default playback/recording device, rather than switching devices from inside the
   Standalone's own Audio/MIDI Settings dialog while sound is live.
```

## Why this works
`AudioProcessorPlayer::audioDeviceIOCallbackWithContext` (line 260) dereferences
`currentDevice` and, one call deeper, `initialiseIoBuffers` copies from the
input-channel-data pointer array — both are only valid while `AudioDeviceManager` isn't
mid-swap. Changing the device while the WASAPI callback thread
(`WasapiClasses::WASAPIAudioIODevice::run`) is actively running creates a window where the
message thread (rebuilding the device/channel layout) and the audio thread (still reading
the old layout) briefly disagree about the current device/channel state; `jassert
(currentDevice != nullptr)` at line 237 passing does not guarantee the pointer is still
valid a few lines later once the message thread's swap completes concurrently. This is a
Standalone-wrapper-only hazard: a real VST3/AU host manages its own audio I/O and never
calls into `AudioDeviceManager`/`AudioProcessorPlayer` at all, so this cannot affect the
shipped plugin — only the Standalone build used for local development/testing.

## Prevention
1. When ear-testing a Standalone build with a loopback input (Stereo Mix, virtual audio
   cable) and a separate monitoring output, always stop the audio source before touching
   Audio/MIDI Settings, or just relaunch the Standalone after an OS-level default-device
   change instead of live-switching in-app.
2. If a crash happens with no visible error dialog, check
   `%LOCALAPPDATA%\CrashDumps\<exe>.<pid>.dmp` (Windows Error Reporting keeps these by
   default) and the Windows "Application" Event Viewer log for `Application Error`
   entries before assuming the bug is in project code — `cdb.exe -z <dump> -c "!analyze
   -v; q"` gives a fast, headless root-cause read without needing a live repro.
3. See also `troubleshooting/patterns/common-solutions.md` — this is filed there too as a
   general "how to triage a silent Standalone crash" pattern, not just a one-off entry,
   since it applies to any JUCE Standalone project, not just Millenia's DSP.

## Related issues
None yet in this project.

## References
- `juce_audio_utils/players/juce_AudioProcessorPlayer.cpp` (JUCE 9.0.0), line ~260
- WinDbg (includes headless `cdb.exe`): `winget install Microsoft.WinDbg`
