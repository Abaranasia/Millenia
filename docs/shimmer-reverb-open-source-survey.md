# Shimmer Reverb — Open Source Implementation Survey

This is a supplementary research doc. It does **not** replace [`shimmer-reverb-concepts.md`](./shimmer-reverb-concepts.md) (the conceptual/architecture-decision guide) or [`../.claude/skills/juce-plugin-dev/references/shimmer-reverb-architecture.md`](../.claude/skills/juce-plugin-dev/references/shimmer-reverb-architecture.md) (the implementation reference) — read those first. Both already cover: the **Dattorro plate** topology (our chosen tank), the **dual-delay-line crossfade** pitch shifter living inside the feedback loop, the JUCE class mapping (`DelayLine<Lagrange3rd>`, `IIR::Filter` allpass, hand-rolled shifter), the known pitfalls (runaway, aliasing, metallic ringing, mono collapse, DC offset, denormals), and these references: **Freeverb, Chowdhury-DSP/chowdsp_utils, Airwindows Galactic, Signalsmith-Audio/reverb-example-code, Signalsmith-Audio/pitch-time-example-code, surge-synthesizer/surge-fx**.

This doc adds eight further open source projects/writeups, found via new web research, that were not already listed there — spanning a real shipped JUCE/Rust plugin, three Faust DSP implementations, an academic paper with a JUCE reference implementation, a Csound patch, an open source Eurorack module, and two alternative reverb "tank" engines worth studying as Dattorro alternatives.

## Findings

### 1. Elysiera

- **Link**: https://github.com/azur1s/elysiera
- **Framework/language**: Rust (`nih-plug` plugin framework) + Faust for the DSP graph. Ships as VST3/CLAP.
- **License**: GPL-3.0
- **Algorithm**: Input is low/high-pass filtered, then split into two pitch shifters (Faust `transpose`-style) modulated in quadrature (sine/cosine LFOs) for stereo width, then fed through Fons Adriaensen's **zita-rev1** as the reverb tank, with feedback closing the shimmer loop. Follows Valhalla's own public description of the shimmer recipe (pitch shifter set to +1 octave, feedback loop, long-decay reverb).
- **Relevance**: A real, shipped, non-JUCE shimmer plugin. Notably it does **not** build its own reverb tank — it reuses zita-rev1 (see #7 below) as an off-the-shelf tank and only adds the shifter + feedback routing around it, which is a valid alternative to hand-building a Dattorro tank if the goal were "get a shimmer sound fast" rather than "own the whole DSP chain."

### 2. `thedrgreenthumb/faust` — `shimmer.dsp`

- **Link**: https://github.com/thedrgreenthumb/faust/blob/master/shimmer.dsp (repo: https://github.com/thedrgreenthumb/faust)
- **Framework/language**: Faust DSP language. Repository license: MIT.
- **Algorithm**: A custom four-line allpass-based feedback network (`allpass_with_fdelay()`, `allpass()`, `APFB()`), explicitly written as a Faust port of **ValhallaShimmer**'s publicly described behavior. Pitch shifting uses Faust's own `transpose()` function (fractional delay line + crossfade — the same family as our dual-delay-line approach, just implemented in Faust's block library rather than hand-rolled).
- **Relevance**: Useful as a from-scratch, readable reference for "how would you describe ValhallaShimmer's topology in ~50 lines," independent of our Dattorro choice. Confirms that a dual-delay/crossfade shifter (not phase vocoder) is the common choice for real shimmer products, not just our own design decision.

### 3. FDN-Reverb-Riser

- **Link**: https://github.com/shabtronic/FDN-Reverb-Riser
- **Framework/language**: C++ (IPlug2), plus a Reaper JSFX and VST2/VST3 build. Explicitly marked **work-in-progress**; no license file found in the repo.
- **Algorithm**: 8 serial allpass filters in a feedback loop (with modulation and EQ) feeding a 4×4 FDN matrix, with a pitch-shift stage placed inside that same feedback loop (exact shifter mechanism isn't documented in the README).
- **Relevance**: The one FDN-native (not Dattorro-derived) shimmer-adjacent topology found with visible source. Its "Orchestra Swell" preset targets a rising/swelling shimmer character rather than a static ascending wash — a useful data point if a "riser" preset variant is ever wanted on top of our Pitch Shift parameter.

### 4. "Shimmer" Audio Effect: A Harmonic Reverberator (Jingjie Zhang, CCRMA/Stanford, 2018)

- **Link**: https://ccrma.stanford.edu/~jingjiez/portfolio/echoing-harmonics/pdfs/Shimmer%20Audio%20Effect%20-%20A%20Harmonic%20Reverberator.pdf
- **Framework/language**: Academic project report (Music 421A, Stanford), with a **JUCE/C++** reference implementation and a GUI.
- **License**: Not stated (course report, not a published package).
- **Algorithm**: Two **phase-vocoder** pitch shifters (STFT time-scale modification + phase-locking at spectral peaks, then linear-interpolation resample) shift the input by +1 and +2 octaves in parallel. Both shifted branches feed an order-16 Hadamard-matrix FDN ("branch reverb," ~400 ms longest delay, 9 s decay) with a 275 ms pre-delay, then the result is dry-mixed and sent into a second, shorter "master" FDN (300 ms delays, 5 s decay) that reverberates the combined signal.
- **Relevance**: This is the clearest documented case of the **phase-vocoder** alternative to dual-delay-line pitch shifting mentioned only briefly in `shimmer-reverb-concepts.md`'s comparison table — and it shows a genuinely different signal-flow choice: pitch shifting happens in **parallel branches feeding a separate reverb**, not literally inside one tank's feedback loop. It also documents a concrete practical constraint worth remembering: phase-vocoder quality "strongly depends on audio buffer size" — the author needed an internal 2048-sample buffer to get decent quality while keeping the host at a 512-sample block, i.e. real added latency, which is exactly the cost our dual-delay-line choice was made to avoid.

### 5. Csound Shimmer Reverb (Steven Yi)

- **Link**: https://kunstmusik.com/2018/08/21/shimmer-reverb/
- **Framework/language**: Csound (`.csd` project, downloadable from the post).
- **License**: Not stated (blog post + example file).
- **Algorithm**: Built from Csound's `reverbsc` opcode (an 8-delay-line stereo FDN modeled on physical damping) combined with a genuine phase-vocoder pitch shifter (`pvsanal` → `pvscale` → `pvsynth`) and `vdelay3` (cubic-interpolated variable delay) for feedback timing. Feedback ≥ 0.5 with a +12 semitone shift reproduces the classic shimmer character.
- **Relevance**: A second, independent phase-vocoder-in-FDN example (agrees with #4), but in a totally different, text-based DSP environment — useful for quickly prototyping topology variations (e.g. feedback amount vs. shift amount trade-offs) without touching C++/JUCE at all.

### 6. Mutable Instruments Clouds

- **Link**: https://github.com/pichenettes/eurorack (module firmware lives in the `clouds/` directory)
- **Framework/language**: Embedded C++ (STM32F4), Eurorack hardware module firmware.
- **License**: MIT
- **Algorithm**: Not a reverb — it's the pitch-shifter/granular half of the recipe. Runs at 32 kHz/32-bit internally and offers a time-domain WSOLA-based pitch-shifter/time-stretcher mode alongside its granular and looper modes.
- **Relevance**: The single most commonly cited real-world building block in Eurorack "shimmer" patches (pitch-shift Clouds output, feed a fraction of it back into a separate reverb module's input). It's the hardware-proven counterexample to both our dual-delay-line approach and the phase-vocoder approach above: a third pitch-shift family (granular/WSOLA) that's genuinely open source and battle-tested, worth knowing about even though we're not adopting it.

### 7. zita-rev1 (Fons Adriaensen)

- **Link**: https://github.com/PelleJuul/zita-rev1 (drop-in C++ mirror of Fons Adriaensen's original; also documented at https://faustlibraries.grame.fr/libs/reverbs/ as `zita_rev1`/`zita_rev1_stereo` in Faust)
- **Framework/language**: C++ (standalone), also available as a Faust library function (`reverbs.lib`, LGPL-with-exception).
- **License**: GPLv2 (original C++); the Faust library port is LGPL-with-exception.
- **Algorithm**: An 8-line FDN hybridized with a Schroeder-style allpass comb filter inserted in series with each individual feedback delay line (on top of the usual per-line damping filters) — i.e., a genuine third tank topology, distinct from both plain FDN and Dattorro's figure-eight cross-feed.
- **Relevance**: This is the tank Elysiera (#1) reuses instead of building its own. Worth knowing as a proven, well-documented alternative "engine" if the Dattorro tank ever needs a point of comparison for diffusion quality or CPU cost — its allpass-comb-per-delay-line trick is a specific, nameable idea distinct from what our architecture doc already covers.

### 8. Faust Greyhole

- **Link**: https://faustlibraries.grame.fr/libs/reverbs/ (source: https://github.com/grame-cncm/faustlibraries/blob/master/reverbs.lib)
- **Framework/language**: Faust DSP language, part of the standard `faustlibraries` distribution.
- **License**: LGPL (with exception), per the `reverbs.lib` license header.
- **Algorithm**: A diffuser (shared design with Faust's `jpverb`) connected in a feedback loop with one long, LFO-modulated delay line — deliberately not delay-network reverberation in the FDN/Dattorro sense. Inspired by the classic Eventide Greyhole hardware effect.
- **Relevance**: Not a shimmer implementation itself (no pitch shifter), but included because it's a real, licensed, alternative **tank topology** — a single modulated long delay + diffuser rather than a network of short delays — that produces a similarly diffuse, spacey, granular-flavored wash. Worth knowing as a topology family distinct from both FDN and Dattorro if the plate character ever needs a texturally different sibling preset.

## Comparison table

| Project | Topology | Pitch-shift method | Notable trait |
|---|---|---|---|
| Elysiera | zita-rev1 tank (Schroeder-allpass-per-line FDN) | Dual Faust `transpose` shifters, quadrature-LFO modulated | Real shipped VST3/CLAP; reuses an off-the-shelf tank instead of building one |
| `thedrgreenthumb` `shimmer.dsp` | Custom 4-line allpass feedback network | Faust `transpose()` (fractional delay + crossfade) | Direct, compact Faust port of ValhallaShimmer's public description |
| FDN-Reverb-Riser | 8 serial allpass → 4×4 FDN matrix | Pitch shift inside the feedback loop (mechanism undocumented) | Aimed at rising "swell" presets, not just static ascending shimmer |
| CCRMA Shimmer paper (Zhang, 2018) | Order-16 Hadamard FDN, dual parallel reverb stages | Phase vocoder (STFT TSM + peak phase-locking) + resample, ×1 and ×2 octave branches | Shifting happens in parallel branches feeding a *separate* reverb, not inside one tank's loop; documents real added latency cost |
| Csound Shimmer Reverb (Yi) | `reverbsc` (8-line FDN) | Phase vocoder (`pvsanal`/`pvscale`/`pvsynth`) | Same phase-vocoder family as #4, in a text-based DSP language good for fast prototyping |
| Mutable Instruments Clouds | N/A — pitch-shift/granular module only | Time-domain WSOLA pitch-shifter/time-stretcher | Third pitch-shift family (granular), hardware-proven, the de-facto Eurorack shimmer building block |
| zita-rev1 | 8-line FDN + per-line Schroeder allpass comb | N/A (tank only) | A third named tank topology, distinct from plain FDN and Dattorro; reused by Elysiera |
| Faust Greyhole | Diffuser + one long modulated delay line (feedback) | N/A (no shifter; not itself a shimmer) | Alternative diffuse-wash topology family, not a delay network at all |

## References

All links below were fetched and verified to load during this research pass.

- Elysiera — https://github.com/azur1s/elysiera
- `thedrgreenthumb/faust` (repo) — https://github.com/thedrgreenthumb/faust
- `thedrgreenthumb/faust` `shimmer.dsp` — https://github.com/thedrgreenthumb/faust/blob/master/shimmer.dsp
- FDN-Reverb-Riser — https://github.com/shabtronic/FDN-Reverb-Riser
- "Shimmer" Audio Effect: A Harmonic Reverberator (Jingjie Zhang, CCRMA, 2018) — https://ccrma.stanford.edu/~jingjiez/portfolio/echoing-harmonics/pdfs/Shimmer%20Audio%20Effect%20-%20A%20Harmonic%20Reverberator.pdf
- Csound Shimmer Reverb (Steven Yi, kunstmusik.com) — https://kunstmusik.com/2018/08/21/shimmer-reverb/
- Mutable Instruments Eurorack firmware (Clouds) — https://github.com/pichenettes/eurorack
- zita-rev1 (C++ mirror) — https://github.com/PelleJuul/zita-rev1
- Faust `reverbs.lib` (documents both `zita_rev1`/`zita_rev1_stereo` and `greyhole`) — https://faustlibraries.grame.fr/libs/reverbs/
- Faust `reverbs.lib` source — https://github.com/grame-cncm/faustlibraries/blob/master/reverbs.lib

## Related

- [`shimmer-reverb-concepts.md`](./shimmer-reverb-concepts.md) — conceptual guide and roadmap this survey supplements
- [`../.claude/skills/juce-plugin-dev/references/shimmer-reverb-architecture.md`](../.claude/skills/juce-plugin-dev/references/shimmer-reverb-architecture.md) — implementation-focused architecture reference this survey supplements
