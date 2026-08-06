# Stereo Goniometer / Lissajous Display — Research Notes

A goniometer (also called a phase scope or vectorscope in this context) is the classic X-Y oscilloscope display audio engineers use to see stereo imaging at a glance: the left channel drives the horizontal deflection, the right channel drives the vertical deflection, and the resulting dot traces out a Lissajous figure whose shape encodes phase correlation and stereo width. It's a natural companion metering view for a plugin editor — where a correlation meter gives you one number, a goniometer shows you the *shape* behind that number, which is much easier to read at a glance while mixing. This doc is pure research, compiled *before* any implementation — read this first; no code exists yet for this feature.

## Quick path — what this feature would involve, in order

1. **The math**: understand the X/Y mapping and rotation convention so the display reads the way engineers expect (mono = vertical line).
2. **The audio-thread → UI-thread handoff**: get samples out of `processBlock` without ever blocking the audio thread.
3. **The rendering approach**: pick how to draw a fast-moving, decaying trace of thousands of points per second without melting the CPU.
4. **Everything else** (colors, scaling, correlation-meter companion bar, presets) is cosmetic once the above three are solid.

## Concept — the math and what the shapes mean

### Parametric form

A classic Lissajous curve is the parametric pair `x(t) = A·sin(a·t + δ)`, `y(t) = B·sin(b·t)`. For stereo audio, the "signal" driving each axis isn't a fixed sinusoid — it's just the raw sample stream:

- `x[n] = L[n]` (or a rotated/derived value, see below)
- `y[n] = R[n]`

Plotted as a scatter/line of `(x[n], y[n])` points over a short rolling window (tens of milliseconds), this reproduces the same family of curves classical Lissajous figures make from two sine generators, because any two correlated-but-phase-shifted band-limited signals trace ellipse-family shapes just like two sinusoids do.

### The 45° rotation convention

Plotted raw (L on X, R on Y), a mono signal (L = R) traces a diagonal line at 45°. Every commercial goniometer instead **rotates the display 45°** so a mono signal reads as a vertical line — this matches how engineers read an oscilloscope and is the de facto standard. Two equivalent ways to get there:

1. **Trig rotation**: convert `(L, R)` to polar `(radius, angle)`, add `π/4` to the angle, convert back to cartesian. Correct but does two transcendental calls per sample.
2. **Mid/Side shortcut (used in practice)**: this rotation is algebraically identical to just plotting Mid/Side instead of Left/Right:
   - `Side = (L − R) / √2` → X axis
   - `Mid  = (L + R) / √2` → Y axis

   No trig required — it's the same mid-side math already familiar from stereo-width processing. This is the approach the JUCE forum thread on building a vectorscope converges on (Mid → Y, Side → X), and it's the cheaper of the two by a wide margin since it's pure addition/subtraction per sample.

### What the shapes mean

| Shape | Signal condition |
|---|---|
| Vertical line | Perfect mono (L = R), fully in phase |
| Horizontal line | Fully out of phase (L = −R) — the classic phase-inversion problem |
| Diagonal line (angled) | Mono-ish signal panned hard to one side |
| Narrow ellipse, mostly vertical | Strongly correlated, modest stereo width — typical well-mixed material |
| Circle | 90°-phase-shifted, decorrelated content (e.g. quadrature-processed stereo widening) — wide but with mono-compatibility risk |
| Wide, fuzzy "blob"/ball | Complex, largely decorrelated stereo content — wide image, but a blob that leans horizontal signals real phase problems, not just "width" |
| Figure-eight | Specific frequency-ratio phase relationships between channels — less common in real program material, more a diagnostic pattern from test tones |

### Relationship to a standard correlation meter

A phase correlation meter is a single scalar: the normalized cross-correlation between L and R,

`r = E[L·R] / √(E[L²] · E[R²])`

computed with a short-time exponential average (`y[n] = c·x[n] + (1−c)·y[n−1]`) rather than a true expectation. `r = +1` is identical in-phase signals (mono), `r = −1` is fully inverted, `0` is decorrelated. **The goniometer is the same information, unreduced**: the correlation meter collapses the cloud of X-Y points down to one number (essentially "how vertical/elongated is the blob"), while the goniometer shows the actual shape. They're natural companions in a UI — most commercial metering plugins (ToneBoosters GonioMeter, Audec Vectorscope 2) show both side by side, and that pairing is worth planning for even if the correlation bar comes later.

## Rendering approach comparison

| Approach | Pros | Cons | Fit for real-time audio UI |
|---|---|---|---|
| `juce::Graphics` + `juce::Path`, redrawn every frame | Simple, portable, no platform GL dependencies, easiest to get right first | JUCE's software path rasterizer is CPU-heavy for paths redrawn every frame at high point counts; can compete with the audio thread for CPU on lower-end machines | Good starting point / fallback renderer; fine at modest point counts and moderate refresh rates |
| `juce::Graphics` + direct pixel manipulation (e.g. writing into an `Image` bitmap) | Cheaper per-point than path stroking when plotting thousands of discrete dots rather than connected strokes; natural fit for a persistence/decay buffer (just fade the bitmap each frame) | More manual bookkeeping (locking an `Image::BitmapData`, blending, scaling for HiDPI) | Strong fit — this is close to how real oscilloscope/goniometer displays are usually built when phosphor-style decay is wanted |
| `juce::OpenGLContext`-accelerated rendering | Offloads compositing/rasterization to the GPU; multiple reports (JUCE forum, Medium writeup) show substantial CPU savings, especially for path-heavy or high-frequency-repaint UI | Added complexity (context attachment, per-platform GL quirks, a known JUCE bug where attaching a context to a plugin editor can scale the whole UI incorrectly in some VST2/VST3 hosts), overkill if the rest of the editor is simple | Best fit if the whole editor already uses OpenGL, or if this display alone justifies the complexity; not a "start here" choice |
| Timer-driven repaint at reduced rate (e.g. 30–60 fps) regardless of renderer | Decouples visual update rate from audio sample rate; keeps repaint cost bounded and predictable | Displayed trace is a snapshot/summary of samples since the last frame, not a truly continuous trace — decimation strategy matters (see below) | Necessary regardless of renderer choice — nothing repaints at 48 kHz |

**Working conclusion for later**: start with `juce::Graphics`/pixel-buffer + timer-driven repaint (option 2), since it directly supports the phosphor-decay look this display wants, and reach for `OpenGLContext` only if profiling shows it's needed once the rest of the editor exists.

### Decay / persistence ("phosphor") trail effect

A real CRT-based goniometer's glow fades because the phosphor itself decays; digital scopes fake this deliberately because it's genuinely useful — it lets you see recent history, not just the instantaneous point. The standard digital technique:

- Maintain an off-screen buffer (an `Image` the size of the display) storing per-pixel "intensity" or age.
- Each frame: (a) plot new points at full brightness, (b) multiply the whole buffer by a decay factor `< 1.0` (exponential fade — matches real phosphor decay curves closely enough that the difference isn't perceptible), then (c) blit to screen.
- The newest point stays the brightest pixel on screen; older points fade smoothly to background over a tunable persistence time (roughly 80 ms–a few hundred ms range, mirroring real phosphor persistence classes).

This is a well-documented technique in oscilloscope-emulation writeups and is the same shape as the decay math already used elsewhere in metering (peak-hold logic uses the same "attack fast, decay slow" idea).

### Sample decimation / downsampling

At 48 kHz+ you get far more (L,R) pairs per video frame than there are pixels to plot them on, and plotting every sample is wasted work. Options observed in the research:

- **Fixed decimation stride**: only take every Nth sample (e.g. every 4th–8th sample at 48 kHz for a 60 fps display) — simplest, cheapest, and generally fine since the eye is reading a shape/cloud, not individual points.
- **Min/max block reduction** (the same idea `ff_meters`' `OutlineBuffer` uses for waveform displays): collapse each block of N samples down to representative extrema rather than a plain stride, preserving transient peaks a naive stride could skip.
- **Buffer-and-batch-per-frame**: accumulate all samples that arrived since the last repaint into a FIFO, then on the timer tick plot the whole accumulated batch at once (decimated or not) — this is the pattern the official JUCE spectrum analyzer tutorial uses (accumulate into a fixed-size array, flip a "ready" flag, let the `Timer` consume it).

### Anti-aliasing considerations

A thin, fast-moving trace is exactly the case that's hardest to anti-alias cheaply: single-pixel-wide lines/dots alias badly and "sparkle" as they move, especially without AA. `juce::Graphics`' path stroking anti-aliases by default but costs more CPU per stroke; a hand-rolled pixel-plot approach needs to do its own (even simple: plot with soft-edged falloff over 2–3 pixels rather than a hard single pixel) or accept visible stair-stepping. This is a secondary concern relative to the decay-trail effect — with persistence/fade active, individual-frame aliasing is considerably less noticeable because no single frame is ever the "final" look of a pixel.

## Open source examples to study

| Project | Framework | License | What's notable |
|---|---|---|---|
| [ffAudio/ff_meters](https://github.com/ffAudio/ff_meters) — see [`Visualisers/StereoFieldComponent.h`](https://github.com/ffAudio/ff_meters/blob/master/Visualisers/StereoFieldComponent.h) and [`StereoFieldBuffer.h`](https://github.com/ffAudio/ff_meters/blob/master/Visualisers/StereoFieldBuffer.h) | JUCE | BSD (3-clause) | **Best direct reference** — a real, maintained JUCE metering module with a purpose-built goniometer/stereo-field component. `StereoFieldBuffer` is a lock-free circular buffer (`juce::AudioBuffer` + `std::atomic<int> writePosition`) filled from the audio thread via `pushSampleBlock()`; the UI reads from it without locks. `StereoFieldComponent` delegates actual drawing to a `LookAndFeelMethods` interface with both `drawGonioMeter()` and `drawStereoField()` modes. Worth reading end-to-end when implementation starts. |
| [blubass/FunkyMooseViz](https://github.com/blubass/FunkyMooseViz) | JUCE 8, CMake, C++17 | MIT | A "boutique-style" JUCE visualizer plugin bundling spectrum, waveform, vectorscope, and pitch detection. Vectorscope component explicitly targets "visualize stereo width and phase coherence." Modern JUCE 8 / CMake project structure, useful as a build-layout reference too. |
| [automatl/audio-dsp-multi-visualize](https://github.com/automatl/audio-dsp-multi-visualize) (project name: EasySSP) | JUCE | GPL-3.0 | Lightweight visualizer showing spectrometer + goniometer for up to four stereo pairs simultaneously, color-coded. Good example of a goniometer sharing screen space with other meters rather than being the sole editor content. |
| [DrSnuggles/jsGoniometer](https://github.com/DrSnuggles/jsGoniometer) | JavaScript / Web Audio API | Not stated in repo | Not JUCE, but a clean from-scratch goniometer/vectorscope/correlation-meter implementation against `GainNode`s in the browser — useful for the underlying math/rendering logic independent of any C++/JUCE specifics; explicitly documents its correlation-meter formula. |
| [x42/meters.lv2](https://github.com/x42/meters.lv2) — [Stereo Phase Scope doc page](https://x42.github.io/meters.lv2/http___gareus_org_oss_lv2_meters_goniometer.html) | LV2 (C), by Robin Gareus | GPLv2 | Not JUCE, but a long-established, professional-grade goniometer ("Stereo Phase Scope") shipping in a widely-used real-time LV2 meter collection on Linux. Good reference for what a mature, "done" goniometer's feature set looks like (correlation output port, adjustable UI gain, state persistence) even without reading its C source directly. |

Forum threads worth a second look once implementation starts (discussion only, not full tutorials, but they converge on the mid/side rotation approach and flag the `atan2`-vs-`atan` quadrant gotcha for anyone who does go the polar-rotation route):

- [JUCE forum — "Create a vectorscope for Stereo Image like in Izotope Ozone"](https://forum.juce.com/t/create-a-vectorscope-for-stereo-image-like-in-izotope-ozone/52197)
- [JUCE forum — "Some tutorial on how to create a vectorscope?"](https://forum.juce.com/t/some-tutorial-on-how-to-create-a-vectorscope/53449) (points at the official spectrum-analyzer tutorial as the closest official analog)
- [JUCE forum — "Goniometer component"](https://forum.juce.com/t/goniometer-component/21089)
- [KVR Audio — "Goniometer Algorithm"](https://www.kvraudio.com/forum/viewtopic.php?t=477945) (cartesian→polar→rotate 45°→cartesian approach, with the `atan2` quadrant-correction note)
- [KVR Audio — "Phase Meter/Phase Correlation question"](https://www.kvraudio.com/forum/viewtopic.php?t=179720) (correlation-meter formula and normalization detail)

## JUCE implementation notes — when we build this

Not implementation yet — just mapping the concepts above onto concrete JUCE APIs we already know we'll reach for:

- **Audio-thread → UI-thread handoff**: `juce::AbstractFifo` (or a hand-rolled lock-free circular buffer like `ff_meters`' `StereoFieldBuffer`, built on a plain `std::atomic<int>` write index) to move raw or mid/side-converted sample pairs out of `processBlock()` without ever taking a lock on the audio thread. The official JUCE spectrum-analyzer tutorial uses an even simpler pattern for a similar problem — a fixed-size array filled sample-by-sample plus a `bool` "block ready" flag checked by a `Timer` — which is worth considering too if we don't need `AbstractFifo`'s full multi-writer generality.
- **Repaint cadence**: `juce::Timer` (`startTimerHz(30)` or similar) driving `repaint()`, decoupled from the audio callback entirely — the UI thread should never be looking at "the current sample," only "whatever's accumulated in the FIFO since last tick."
- **Drawing the trace**: `juce::Path` + `juce::Graphics::strokePath()` as the simple first pass; likely graduating to direct writes into a `juce::Image` (via `Image::BitmapData`) once the phosphor-decay effect is wanted, since decay-fading a bitmap is straightforward (multiply alpha/intensity each frame) but doesn't map cleanly onto stroking a fresh `Path` every frame.
- **GPU acceleration**: `juce::OpenGLContext` attached to the editor if/when profiling shows the software renderer struggling — not a default choice, given the known scaling bug reports on some plugin hosts when a context is attached.
- **Correlation companion meter**: shares the same FIFO data pull as the goniometer; just a scalar exponential-average calculation over the same block, no separate audio-thread tap needed.
- **Real-time safety carryover**: same rules already enforced elsewhere in this codebase (per the `juce-plugin-dev` skill) apply here too — no allocation, no locking, no logging on the audio thread; the FIFO push in `processBlock` must be the only work happening there for this feature.

## Pitfalls to watch for

- [ ] **Audio-thread blocking**: any mutex, allocation, or unbounded work in the `processBlock`-side FIFO push will violate real-time safety — must be a true lock-free structure, sized once in `prepareToPlay`.
- [ ] **FIFO overrun/underrun**: if the UI timer runs slower than the audio thread fills the FIFO (e.g. host suspends the UI, or a modal dialog stalls the message thread), the buffer can wrap and silently drop or corrupt data — need an explicit overrun policy (drop oldest, or a "catch-up" read) rather than assuming it can't happen.
- [ ] **CPU cost of the redraw itself**: `juce::Graphics` path stroking at high point-density and high frame rate is a real, measured cost (multiple forum reports); decimate samples *and* consider the pixel-buffer approach before assuming a `Path`-per-frame is fine.
- [ ] **Visual aliasing/flicker on thin fast traces**: a single-pixel dot/line trace aliases and "sparkles" without either AA or the persistence/decay buffer smoothing it out — the decay trail isn't just an aesthetic choice, it materially reduces this problem.
- [ ] **Decimation hiding transients**: a naive fixed-stride decimation can skip exactly the brief peak that matters (e.g. a short phase-inversion glitch); prefer min/max-style block reduction if that turns out to matter in practice.
- [ ] **Rotation convention mismatch**: get the 45° (mid/side) rotation right from the start — a goniometer that shows mono as a diagonal instead of vertical will look "wrong" to every experienced engineer who looks at it, even though the underlying data is correct.
- [ ] **HiDPI / display scaling**: if going the `Image`/`BitmapData` pixel-buffer route for the decay effect, remember JUCE's automatic UI scaling doesn't automatically scale a manually-managed bitmap — needs explicit handling.
- [ ] **OpenGLContext plugin-host quirks**: if `OpenGLContext` is adopted later, the reported VST2/VST3 UI-scaling bug when attaching a context inside certain hosts needs to be checked against whatever hosts this plugin targets before committing to it.
- [ ] **Denormals**: any exponential decay/averaging math (bitmap fade, correlation-meter smoothing) run continuously on very quiet or silent input can hit denormals on the audio-thread side of the correlation calc — same `juce::ScopedNoDenormals` discipline already used elsewhere in this codebase applies if that calculation ends up living on the audio thread rather than being computed from already-buffered data on the UI/timer side.

## References

- [Wikipedia — Goniometer (audio)](https://en.wikipedia.org/wiki/Goniometer_(audio))
- [Wikipedia — Vectorscope](https://en.wikipedia.org/wiki/Vectorscope)
- [Audio Masterclass — Visualizing stereo information using Lissajous figures](https://www.audiomasterclass.com/blog/visualizing-stereo-information-using-lissajous-figures)
- [arXiv — "Goniometers are a Powerful Acoustic Feature for Music Information Retrieval Tasks"](https://arxiv.org/pdf/2302.01090)
- [KVR Audio forum — Goniometer Algorithm](https://www.kvraudio.com/forum/viewtopic.php?t=477945)
- [KVR Audio forum — Phase Meter/Phase Correlation question](https://www.kvraudio.com/forum/viewtopic.php?t=179720)
- [JUCE forum — Create a vectorscope for Stereo Image like in Izotope Ozone](https://forum.juce.com/t/create-a-vectorscope-for-stereo-image-like-in-izotope-ozone/52197)
- [JUCE forum — Some tutorial on how to create a vectorscope?](https://forum.juce.com/t/some-tutorial-on-how-to-create-a-vectorscope/53449)
- [JUCE forum — Goniometer component](https://forum.juce.com/t/goniometer-component/21089)
- [JUCE forum — AbstractFifo use](https://forum.juce.com/t/abstractfifo-use/16901)
- [JUCE forum — Lock-free queues and visualization of data](https://forum.juce.com/t/lock-free-queues-and-visualization-of-data/20659)
- [JUCE tutorial — Spectrum Analyser](https://docs.juce.com/master/tutorial_spectrum_analyser.html) (FIFO + `Timer` pattern for audio→UI handoff)
- [JUCE tutorial — Build an OpenGL Application](https://docs.juce.com/master/tutorial_open_gl_application.html)
- [JUCE docs — `juce::OpenGLContext`](https://docs.juce.com/master/classOpenGLContext.html)
- [JUCE docs — `juce::AudioVisualiserComponent`](https://docs.juce.com/master/classAudioVisualiserComponent.html)
- [Medium (James Johnson) — Using OpenGL for 2D graphics in a JUCE plug-in](https://medium.com/@Im_Jimmi/using-opengl-for-2d-graphics-in-a-juce-plug-in-24aa82f634ff)
- [richardandersson.net — Algorithm for simulating Phosphor Persistence of Analog Oscilloscopes](https://richardandersson.net/?p=350)
- [EDN — Oscilloscope persistence displays](https://www.edn.com/oscilloscope-persistence-displays/)
- [GitHub — ffAudio/ff_meters](https://github.com/ffAudio/ff_meters) (BSD-3), notably [`StereoFieldComponent.h`](https://github.com/ffAudio/ff_meters/blob/master/Visualisers/StereoFieldComponent.h) and [`StereoFieldBuffer.h`](https://github.com/ffAudio/ff_meters/blob/master/Visualisers/StereoFieldBuffer.h)
- [GitHub — blubass/FunkyMooseViz](https://github.com/blubass/FunkyMooseViz) (MIT)
- [GitHub — automatl/audio-dsp-multi-visualize (EasySSP)](https://github.com/automatl/audio-dsp-multi-visualize) (GPL-3.0)
- [GitHub — DrSnuggles/jsGoniometer](https://github.com/DrSnuggles/jsGoniometer)
- [GitHub — x42/meters.lv2](https://github.com/x42/meters.lv2) (GPLv2) — [Stereo Phase Scope doc page](https://x42.github.io/meters.lv2/http___gareus_org_oss_lv2_meters_goniometer.html)

## Next step

This is groundwork only — no editor component, buffer class, or DSP tap exists yet. When it's time to build, start from the `ff_meters` `StereoFieldBuffer`/`StereoFieldComponent` pair as the closest real reference, decide the FIFO mechanism (`AbstractFifo` vs. the simpler flag-based pattern from the spectrum-analyzer tutorial), and confirm the mid/side rotation convention before writing any drawing code.
