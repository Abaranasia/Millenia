#pragma once

#include <JuceHeader.h>
#include <utility>
#include <vector>

// Stage 1: a fully standalone, self-contained, UNWIRED DSP building block.
// Not referenced from ShimmerReverbEngine, PluginProcessor, PluginEditor, or
// Parameters yet -- that wiring is explicitly a later stage. This class only
// needs to be correct and real-time-safe in isolation; LoopCaptureTests.cpp
// drives it directly.
//
// Captures a window of recent stereo audio and repeats it as a static,
// crossfaded loop, gated by a continuous 0..1 "loop freeze amount" value.
//
// Unlike every other freeze-aware class in this project (see
// DattorroTank::setFreezeAmount()/FreezeLeveler.h, both of which trust the
// caller to arrive pre-smoothed), this class owns its OWN per-sample
// smoothing ramp for loopFreezeAmount -- see setLoopFreezeAmount()'s comment
// for why: it's a deliberate, narrow exception found necessary by a real
// 2026-09-06 by-ear bug report ("small glitch noise upon toggling").
//
// ---------------------------------------------------------------------------
// Mechanism
// ---------------------------------------------------------------------------
// Two mono ring buffers (rollingLeft/rollingRight) are written unconditionally
// on every process() call, regardless of loopFreezeAmount -- so whatever is
// in them at any instant is exactly the most recent maxLoopLengthMs of
// history, always available to capture from. A capture event runs a bounded,
// allocation-free circular copy that pulls the most recent
// currentLoopLengthSamples out of those rolling buffers into a second pair
// of captured-loop buffers, which then play back on repeat for as long as
// loopFreezeAmount stays > 0. Since 2026-10-10 there are three trigger
// paths: (1) the original exact 0.0f -> >0.0f edge of loopFreezeAmount,
// (2) an armed rise above retriggerThreshold after the dial parked near
// zero, and (3) the deferred landing of a capture that fired via (1) or (2)
// before enough AUDIBLE history existed -- see "Re-arm and deferred
// capture" and "Wait for sound" below.
//
// The loop LENGTH is a two-stage value on purpose: setLoopLengthMs() only
// ever updates a PENDING millisecond value; the actual
// currentLoopLengthSamples (and the captured content itself) only changes at
// the next capture event (any of the three trigger paths above). This is the same "no live resize
// mid-flight" real-time-safety discipline this project's
// .claude/skills/juce-plugin-dev conventions require for anything that would
// otherwise need to resize or reinterpret a buffer while it's actively being
// read from inside process() -- changing the loop length while a loop is
// already playing would have to either resize (real-time-unsafe) or
// reinterpret the existing captured content at a different sample count
// (audibly nonsensical, since the content was captured from a fixed-length
// window). Deferring to the next capture sidesteps both problems entirely.
//
// ---------------------------------------------------------------------------
// The seam splice: why "blend tail with head" alone is not enough
// ---------------------------------------------------------------------------
// A naive loop -- just wrapping the read position from N-1 back to 0 every
// N samples -- has an audible click unless the captured content happens to
// be perfectly periodic at exactly N samples, which recorded audio never is.
// The obvious fix, crossfading the last few samples of the buffer against
// the first few before wrapping to 0, still has a subtler problem: it makes
// the FIRST pass smooth, but every subsequent pass would crossfade the SAME
// tail region against the SAME head region again, which only hides the
// click if the blended region itself is treated as consumed, not replayed.
//
// This class's fix: after capture, N = currentLoopLengthSamples and
// X = min(crossfadeSamplesConstant, N/2) (X computed once per capture, so a
// short capture never blends more than half its own length against itself).
// The very first pass across the loop plays positions [0, N) address once,
// with the raised-cosine blend applied only in the final X samples
// (positions [N-X, N)) against the FIRST X samples (positions [0, X)) of the
// same captured buffer -- same raised-cosine shape as PitchShifter's own
// grainWindow() (see PitchShifter.cpp), i.e. 0.5*(1-cos(pi*t)) fading in and
// its complement (which always sums to exactly 1.0f, by construction, not
// just approximately) fading out. Critically, once the read position
// advances past N-1, it wraps to X, NOT 0 -- so every subsequent pass only
// ever replays positions [X, N), an EXACT period of (N - X) samples that is
// bit-identical every single repeat (the captured buffer content is static;
// nothing in the read/blend computation depends on which pass it is). The
// first X samples (positions [0, X)) are only ever used as the fixed blend
// target for the wrap, never played back as their own standalone segment
// again after the first pass. This is what makes the loop genuinely
// periodic at (N - X) samples -- not merely "smoothed but still drifting" --
// while still being smooth (no discontinuity) at the wrap itself.
//
// ---------------------------------------------------------------------------
// Re-arm and deferred capture (fixed 2026-10-10)
// ---------------------------------------------------------------------------
// By-ear report, 2026-10-10: "at Loop Mix = 1 the loop is sometimes silent
// (only the dry part of the main Mix, or total silence at Mix = 1); turning
// Loop Mix to 0 and back up fixes it, but not always". The loop records the
// WET OUTPUT, so Freeze/Infinite tails were captured fine -- they were not
// the cause. The original trigger was an EXACT rising edge
// (previousLoopFreezeAmount == 0.0f && loopFreezeAmount > 0.0f), which had
// three root causes behind it:
//   1. Restart capture of an empty history. prepare()/reset() zero the
//      rolling history and restart the smoothed/previous amounts at 0.0f,
//      so a Loop Mix already > 0 (restored plugin state at Standalone
//      launch, an audio-device restart, a sample-rate change) formed a
//      "rising edge" on the very first sample and captured an ALL-ZERO
//      history: a silent loop for as long as Loop Mix stayed up.
//   2. The dial doesn't land exactly on 0. A slider stopping at e.g. 0.003
//      never made previousLoopFreezeAmount exactly 0.0f, so capture never
//      re-armed and the old (possibly silent) loop kept playing.
//   3. A quick down-and-up faster than the 50ms ramp never reaches 0, so
//      there was no edge either.
//
// Fix A -- re-arm with hysteresis. Capture re-ARMS once the RAMPED amount
// AND its target are both <= rearmThreshold (0.01, i.e. the old loop is
// blended at <= -40dB: the user has genuinely parked the dial near zero,
// not just swept through it). An armed capture then FIRES once the ramped
// amount rises above retriggerThreshold (0.02, -34dB) -- deliberately a
// separate, higher threshold rather than "rises above rearmThreshold
// again": with a single threshold, a dial (or automation lane) dithering
// around 0.01 would re-arm and re-fire on every crossing, re-capturing and
// rewinding the loop each time. The 0.01-wide hysteresis band means a new
// capture needs a real move of the dial, never just jitter, and a capture
// fired inside the band is at <= 2% blend anyway. Firing disarms; parking
// near zero again is the only way to re-arm. Coming from EXACTLY 0.0f the
// original edge rule is kept verbatim as its own OR term, so the classic
// engage captures on the same sample as before (the first sample > 0) --
// every pre-2026-10-10 LoopCaptureTests case passes unchanged.
// Cause 3 (a quick touch faster than the ramp) is intentionally left as
// "keeps the old loop": the ramped amount never fell below -40dB, so the
// loop never actually faded out and replacing it would be the surprise.
//
// Fix B -- deferred capture. A capture that fires before enough history
// exists (see "Wait for sound" below for what counts as history) goes
// PENDING instead of capturing it; while pending the output is the live
// wet signal (effective loop blend 0). The sample whose write completes the
// required history performs the capture (N recomputed from
// pendingLoopLengthMs at that moment, same as a normal capture), then snaps
// the smoothed amount to 0.0f and re-sets its target -- re-running the exact
// same 50ms engage ramp a normal capture uses, so the deferred engage is
// click-free by the same mechanism. Parking the dial back near zero (the
// same condition that re-arms) cancels a pending capture.
//
// ---------------------------------------------------------------------------
// Wait for sound (2026-10-10 follow-up -- reverses an earlier decision)
// ---------------------------------------------------------------------------
// Fix B as first written counted SAMPLES written since prepare()/reset()
// (validHistorySamples) and deliberately did NOT special-case silence: "a
// real but silent history (nothing playing yet) is captured as silence,
// which is honest". That decision is reversed. A diagnostic replay of the
// user's actual launch state (Mix 1, Loop Mix 1, Loop Length 3225.8ms,
// 48kHz, 480-sample blocks) showed it reproduced the very bug Fix B was
// meant to cure: the source app is rarely playing during the first ~3.2s
// after a Standalone launch/device restart, so the deferred capture fired
// anyway on pure silence, and at Loop Mix 1 the output was (1-1)*wet +
// 1*loop = digital zero (-200dB) for as long as Loop Mix stayed up -- even
// with -15dBFS input present. With only the first 1s silent, every loop
// cycle had a 1s digital-zero gap instead. "Honest" silence is never what a
// user raising Loop Mix wants repeated.
//
// The fix replaces validHistorySamples with audibleHistorySamples: the
// number of samples since the most recent SOUND ONSET, which the single
// predicate hasEnoughAudibleHistory() compares against N for every capture
// path (immediate or pending -- one shared check, so they can never
// disagree). Per sample, after the rolling-history write:
//   - "Sound" = max(|wetLeft|, |wetRight|) > signalGateLinear (-60dBFS).
//     A sound sample clears consecutiveSilentSamples and increments
//     audibleHistorySamples (saturating at maxCapacitySamples).
//   - A sub-gate sample increments consecutiveSilentSamples. Once that run
//     is LONGER than silenceResetSamples (250ms), audibleHistorySamples
//     resets to 0 and stays 0 until the next sound sample (the new onset).
//     A shorter run -- a zero crossing, a brief pause, a gap between notes
//     -- is part of the music, so it keeps counting, but only after an
//     onset: leading silence after prepare()/reset() never counts.
// So a capture can only happen once the last N samples are all post-onset,
// i.e. the captured window contains no sub-gate run longer than
// silenceResetSamples. Pending/cancel/deferred-ramp semantics are exactly
// Fix B's. audibleHistorySamples <= samples written since reset() by
// construction, so the original "never capture unwritten zeros" guarantee
// still holds. While nothing is playing, a raised Loop Mix just stays
// pending and passes the live (silent) wet through -- nothing is lost, and
// the loop engages via the same click-free deferred ramp N samples after
// the source actually starts.
class LoopCapture
{
public:
    LoopCapture() = default;

    // Allocates the rolling and captured-loop buffers to maxLoopLengthMs
    // worth of samples at the live sample rate. No allocation happens
    // anywhere else in this class (process()/reset() only index into and
    // clear these fixed-size buffers).
    void prepare (const juce::dsp::ProcessSpec& spec);

    // Clears both buffer sets to silence, and resets currentLoopLengthSamples
    // to 0 and previousLoopFreezeAmount to 0.0f (so the very next
    // loopFreezeAmount > 0.0f call is seen as a fresh rising edge, not a
    // continuation of whatever was frozen before reset()). Also zeroes
    // audibleHistorySamples and consecutiveSilentSamples, so a capture fired
    // right after reset() is deferred until N samples of real sound exist
    // (2026-10-10 fix, see the class comment's "Re-arm and deferred capture"
    // and "Wait for sound" sections). Does NOT reset
    // the pending loop length set via setLoopLengthMs() -- same convention as
    // DattorroTank::reset() leaving decayGain/dampingCoefficient untouched:
    // reset() clears transient DSP state, not a caller-configured setting.
    void reset() noexcept;

    // Stores a PENDING loop length in milliseconds only -- does NOT change
    // currentLoopLengthSamples or resize/reinterpret any buffer. Takes effect
    // only at the next capture event -- exact edge, armed rise, or deferred
    // landing (see the class comment above). Clamped to [minLoopLengthMs, maxLoopLengthMs] here (unlike,
    // say, DattorroTank::setDecay()'s deliberately-unclamped convention):
    // there, an out-of-APVTS-range value is only an audio/tuning concern the
    // parameter layer is responsible for; here, an unclamped value would
    // become a real out-of-bounds buffer index at the next capture, which is
    // a real-time-safety/memory-safety property this class must guarantee on
    // its own, independent of whatever range a later Parameters.cpp
    // enforces upstream.
    void setLoopLengthMs (float newLoopLengthMs) noexcept;

    // Sets the TARGET loop-freeze amount; the actual value process() uses
    // ramps toward it over loopFreezeRampSeconds (50ms), sample-accurately,
    // regardless of how often/abruptly this setter itself is called.
    //
    // Found necessary 2026-09-06 (by-ear report: "small glitch noise upon
    // toggling"). The original design (see this file's own prior revision)
    // left ALL smoothing to the caller, same "arrives pre-smoothed" trust
    // every other freeze-aware class in this project extends -- but
    // PluginProcessor's caller-side smoothing only applied ONE value per
    // host audio BLOCK (juce::SmoothedValue::skip(numSamples)), holding it
    // constant across every sample in that block. Loop Freeze is driven by a
    // discrete on/off toggle every single time it's used (unlike a
    // continuously-dragged dial, which never produces a hard full-range
    // target jump under normal use) -- so if a host's buffer size is
    // comparable to or larger than the 50ms ramp, that block-granularity
    // smoothing could fast-forward the entire ramp within a single block,
    // applying a near-instant full-strength switch between the live wet
    // signal and the captured loop for that block's samples. Owning the
    // ramp here, applied per sample inside process() itself, makes the
    // engage/disengage transition genuinely click-free at any host buffer
    // size -- the caller (ShimmerReverbEngine/PluginProcessor) can now just
    // forward the raw APVTS toggle value straight through.
    void setLoopFreezeAmount (float newAmount) noexcept;

    // Writes wetLeft/wetRight into the rolling history buffers unconditionally
    // (see the class comment above), advances the internal loopFreezeAmount
    // ramp by one sample (see setLoopFreezeAmount()), runs the re-arm/
    // trigger/deferred-capture logic (see the class comment's "Re-arm and
    // deferred capture" section) and captures a fresh loop if due, reads (and
    // crossfades, if applicable) the current loop-playback sample, and
    // returns the loopFreezeAmount-weighted blend of the live and looped
    // signal: { (1-loopFreezeAmount)*wetLeft + loopFreezeAmount*loopLeft,
    // (1-loopFreezeAmount)*wetRight + loopFreezeAmount*loopRight }. If a
    // capture is PENDING (waiting for enough audible history), or no loop has ever been captured
    // (currentLoopLengthSamples == 0), the effective blend is 0.0f and the
    // live wet signal passes through untouched. (Before 2026-10-10 the
    // no-loop case blended toward a 0.0f loop signal instead, i.e. dipped
    // the wet by loopFreezeAmount; that was unreachable then, since a
    // capture always fired on the first sample > 0, but a cancelled
    // deferred capture now leaves the ramp > 0 with no loop -- see the
    // class comment.)
    std::pair<float, float> process (float wetLeft, float wetRight) noexcept;

    // Test-only introspection -- same "peek" idiom as
    // DattorroTank::peekFeedbackSignal() and PitchShifter::getPrimaryLastOffset().
    int getCurrentLoopLengthSamples() const noexcept { return currentLoopLengthSamples; }

    // Test-only introspection: raw captured-loop content at a given sample
    // index (channel 0 = left, 1 = right), before any crossfade blending is
    // applied. Returns 0.0f for an out-of-range index rather than reading
    // past the captured-loop buffers' own fixed capacity.
    float peekCapturedLoopSample (int channel, int index) const noexcept;

    // Test-only introspection (2026-10-10 re-arm/deferred-capture fix):
    // number of captureLoop() runs since the last prepare()/reset(), and
    // whether a capture is currently deferred waiting for enough audible
    // history.
    int getCaptureCount() const noexcept { return captureCount; }
    bool isCapturePending() const noexcept { return capturePending; }

private:
    // Starting points, reasoned rather than ear-tuned -- refine after this
    // class is actually wired into the signal chain (Stage 2+) and can be
    // listened to in context. Same "reasoned-not-ear-tuned" convention as
    // e.g. FreezeLeveler.h's/DattorroTank.h's constants.

    // Half a second: long enough to capture a musically meaningful phrase
    // (a chord, a short riff) but short enough that the rolling-history
    // buffer (sized to maxLoopLengthMs, not this default) stays a modest
    // allocation, and short enough to feel responsive when a player engages
    // Freeze/Loop on a whim rather than pre-planning a long capture.
    static constexpr float defaultLoopLengthMs = 500.0f;

    // 50ms: short enough to capture a single transient/drum hit as a tight
    // rhythmic loop, but not so short that crossfadeMs (25ms, see below)
    // would have to eat more than half the loop's own length -- at exactly
    // 50ms, X = min(25ms, 25ms) = 25ms, i.e. this is also the shortest loop
    // length at which the crossfade window doesn't get capped down by the
    // N/2 rule.
    static constexpr float minLoopLengthMs = 50.0f;

    // 4000ms (4s): comfortably covers a multi-bar musical phrase at slow
    // tempos without the rolling-history buffer becoming an unreasonably
    // large fixed allocation. MUST EXACTLY MATCH the upper bound of the
    // `loopLength` APVTS parameter a later stage adds in Source/Parameters.cpp
    // -- that file does not exist yet as of this class; whoever adds it must
    // keep this constant and that parameter's range upper bound in sync, or
    // this class's buffers will either be too small (silently truncating a
    // requested max-length capture) or the parameter will offer a length
    // this class can never actually honor.
    static constexpr float maxLoopLengthMs = 4000.0f;

    // 25ms, fixed -- deliberately NOT scaled with the loop length (unlike
    // currentLoopLengthSamples itself, which does scale with
    // setLoopLengthMs()). Same "20-30ms range" convention as PitchShifter's
    // original grain crossfade window (see PitchShifter.h's grainLengthMs
    // history/PitchShifter.cpp's grainWindow()) -- long enough to mask a
    // splice between two unrelated points in captured audio, short enough
    // to stay a small, fixed fraction of even the shortest loop length
    // (minLoopLengthMs) rather than eating into the loop's own perceived
    // rhythm.
    static constexpr float crossfadeMs = 25.0f;

    // 50ms -- see setLoopFreezeAmount()'s comment for the full rationale.
    // Matches PluginProcessor's original (now-redundant) block-granularity
    // ramp duration for this same parameter, so the audible fade time is
    // unchanged, just now genuinely sample-accurate.
    static constexpr double loopFreezeRampSeconds = 0.05;

    // 2026-10-10 re-arm hysteresis -- see the class comment's "Re-arm and
    // deferred capture" section. 0.01 = the old loop at -40dB: parked there
    // (ramp AND target), the loop has effectively faded out, so the next
    // real raise may replace it. 0.02 (-34dB) is the fire level for an
    // armed capture; the gap between the two is what keeps dial jitter
    // around 0.01 from re-capturing repeatedly. MUST stay
    // retriggerThreshold > rearmThreshold, or the band collapses back to a
    // single jitter-prone threshold.
    static constexpr float rearmThreshold = 0.01f;
    static constexpr float retriggerThreshold = 0.02f;
    static_assert (retriggerThreshold > rearmThreshold, "Re-arm hysteresis band must be non-empty");

    // 2026-10-10 "wait for sound" gate -- see the class comment's "Wait for
    // sound" section. 0.001f = -60dBFS. The measured wet levels feeding this
    // class with a source playing were about -2 to -7dBFS (diagnostic replay
    // of the user's launch state), so the gate sits ~53-58dB under real
    // program material: any audible wet clears it by a wide margin, while
    // digital silence (exactly 0.0f from a stopped source) stays under it.
    // -60dB is also the RT60 convention -- a reverb tail 60dB down is "gone"
    // by definition -- so a tail decaying out after the source stops counts
    // as sound until it is genuinely inaudible, and no longer.
    static constexpr float signalGateLinear = 0.001f;

    // 250ms (converted to silenceResetSamples in prepare()): a sub-gate run
    // longer than this means nothing is playing, so the audible history
    // restarts at the next onset. Long enough that zero crossings, rests
    // between notes and short phrase gaps (in a reverb's WET signal usually
    // bridged by the tail anyway) never reset it -- a loop of real music
    // keeps its natural pauses -- yet short enough that a source stopped for
    // longer than a beat is not captured as a gap. It is also the longest
    // sub-gate run a captured loop can ever contain.
    static constexpr float silenceResetMs = 250.0f;

    // Raised-cosine (Hann-shaped) fade-in used at the capture's own wrap
    // seam -- identical shape to PitchShifter::grainWindow()'s ramp-in half
    // (see PitchShifter.cpp), so this class's splice character matches the
    // rest of the codebase's existing crossfade convention rather than
    // introducing a second, differently-shaped window function. Returns 0.0f
    // at t=0.0f and approaches (but does not exactly reach, same as
    // grainWindow()'s own known behavior at its last sample) 1.0f as t
    // approaches 1.0f. fadeOut(t) == 1.0f - fadeIn(t) always, by
    // construction -- so fadeIn(t) + fadeOut(t) == 1.0f identically, exactly
    // (not merely approximately) at every t, which is what keeps the
    // crossfade from ever dipping or peaking in level at the splice.
    static float fadeIn (float t) noexcept
    {
        return 0.5f * (1.0f - std::cos (juce::MathConstants<float>::pi * t));
    }

    // Performs the actual capture for any of the three trigger paths (exact
    // edge, armed rise, deferred landing -- see the class comment): computes N =
    // currentLoopLengthSamples from the pending ms value (clamped, and
    // additionally clamped to the fixed buffer capacity as a last-resort
    // safety net -- see setLoopLengthMs()'s comment), computes X =
    // activeCrossfadeSamples = min(crossfadeSamplesConstant, N/2), and
    // copies the most recent N samples out of the rolling buffers into the
    // captured-loop buffers via bounded index arithmetic only (no resize,
    // ever). Resets the loop read position to 0. This copy's cost is
    // bounded by maxCapacitySamples (a fixed worst case, not unbounded), and
    // only ever runs once per capture event -- never every sample -- matching
    // the "bounded circular copy" requirement this method exists to satisfy.
    void captureLoop() noexcept;

    // N for a capture happening NOW: pendingLoopLengthMs at the live sample
    // rate, clamped to [1, maxCapacitySamples]. Shared by captureLoop() and
    // hasEnoughAudibleHistory() so both always agree on N.
    int requestedLoopLengthSamples() const noexcept;

    // The single capture-readiness predicate (2026-10-10 "wait for sound";
    // the previous inline check was written out twice in process()): true
    // once the last requestedLoopLengthSamples() samples are all post-onset
    // audible history (see the class comment). Used by BOTH the immediate
    // (exact edge / armed rise) and the pending (deferred landing) paths in
    // process(), so the two can never drift apart.
    bool hasEnoughAudibleHistory() const noexcept { return audibleHistorySamples >= requestedLoopLengthSamples(); }

    // Reads one channel's captured-loop content at a given read position,
    // applying the seam-splice crossfade (see the class comment above) when
    // the position falls in the final activeCrossfadeSamples samples of the
    // current pass.
    float readLoopChannel (const std::vector<float>& capturedLoop, int position) const noexcept;

    double sampleRate = 44100.0;

    // Fixed capacity (in samples) of every buffer below, computed once in
    // prepare() from maxLoopLengthMs at the live sample rate. Never changes
    // afterward, so every index computation elsewhere in this class can
    // treat it as a hard upper bound.
    int maxCapacitySamples = 0;

    // crossfadeMs converted to samples at the live sample rate, computed once
    // in prepare() -- the fixed upper bound captureLoop() caps against N/2
    // (see activeCrossfadeSamples below) every capture.
    int crossfadeSamplesConstant = 0;

    // silenceResetMs converted to samples at the live sample rate, computed
    // once in prepare() (see silenceResetMs and the class comment's "Wait for
    // sound" section).
    int silenceResetSamples = 0;

    // Rolling (unconditionally-written-every-sample) mono history buffers --
    // this is the sole SOURCE captureLoop() ever copies from. Sized to
    // maxCapacitySamples in prepare(), never resized afterward.
    std::vector<float> rollingLeft, rollingRight;
    int rollingWriteIndex = 0;

    // Captured-loop mono buffers -- only the first currentLoopLengthSamples
    // entries of each are meaningful at any given time; the rest is stale
    // content from a previous (shorter) capture, never read past
    // currentLoopLengthSamples by process()/readLoopChannel(). Sized to
    // maxCapacitySamples in prepare(), never resized afterward.
    std::vector<float> capturedLoopLeft, capturedLoopRight;

    // How many samples of capturedLoopLeft/Right are actually in use. 0.0f
    // means "nothing captured yet" (see process()'s defensive handling).
    int currentLoopLengthSamples = 0;

    // X from the class comment above -- the active crossfade length for the
    // CURRENT capture, computed once per capture (not per sample) since it
    // depends on currentLoopLengthSamples via the N/2 cap.
    int activeCrossfadeSamples = 0;

    // Where playback is reading from inside the captured-loop buffers. Reset
    // to 0 on every fresh capture; wraps to activeCrossfadeSamples (not 0)
    // once it advances past currentLoopLengthSamples - 1 -- see the class
    // comment above for why.
    int readPosition = 0;

    // Pending value set by setLoopLengthMs(); only takes effect at the next
    // capture event, via any trigger path (see captureLoop()).
    float pendingLoopLengthMs = defaultLoopLengthMs;

    // Last-seen (already-ramped) loopFreezeAmount, used to detect the
    // original exact 0.0f -> >0.0f rising edge on the NEXT process() call
    // (kept verbatim as one trigger term -- see the class comment).
    float previousLoopFreezeAmount = 0.0f;

    // 2026-10-10 re-arm/deferred-capture state (see the class comment).
    // captureArmed: parked near zero since the last capture, so the next
    // rise above retriggerThreshold captures. capturePending: a capture
    // fired before enough audible history existed and is waiting for it.
    // audibleHistorySamples: samples since the most recent sound onset
    // (sub-gate runs <= silenceResetSamples included; 0 before any onset),
    // saturating at maxCapacitySamples -- replaced the earlier
    // validHistorySamples ("samples written since reset"), see "Wait for
    // sound". consecutiveSilentSamples: length of the current, unbroken run
    // of sub-gate samples ending at the latest process() call (0 right after
    // a sound sample), saturating at silenceResetSamples + 1 -- the first
    // value that resets the audible history -- so endless silence can never
    // overflow it. captureCount: test-only counter (see getCaptureCount()),
    // saturating at INT_MAX.
    bool captureArmed = false;
    bool capturePending = false;
    int audibleHistorySamples = 0;
    int consecutiveSilentSamples = 0;
    int captureCount = 0;

    // Owns the actual per-sample ramp -- see setLoopFreezeAmount()'s comment.
    juce::SmoothedValue<float> smoothedLoopFreezeAmount;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoopCapture)
};
