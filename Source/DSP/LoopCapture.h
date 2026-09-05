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
// crossfaded loop, gated by a continuous 0..1 "loop freeze amount" value
// (the eventual APVTS-driven parameter a later stage will pass in here every
// sample, same "arrives pre-smoothed from the caller" convention every other
// freeze-aware class in this project already follows -- see
// DattorroTank::setFreezeAmount()/FreezeLeveler.h).
//
// ---------------------------------------------------------------------------
// Mechanism
// ---------------------------------------------------------------------------
// Two mono ring buffers (rollingLeft/rollingRight) are written unconditionally
// on every process() call, regardless of loopFreezeAmount -- so whatever is
// in them at any instant is exactly the most recent maxLoopLengthMs of
// history, always available to capture from. On a RISING EDGE of
// loopFreezeAmount (0.0f -> >0.0f, checked every call), a bounded, allocation-
// free circular copy pulls the most recent currentLoopLengthSamples out of
// those rolling buffers into a second pair of captured-loop buffers, which
// then play back on repeat for as long as loopFreezeAmount stays > 0.
//
// The loop LENGTH is a two-stage value on purpose: setLoopLengthMs() only
// ever updates a PENDING millisecond value; the actual
// currentLoopLengthSamples (and the captured content itself) only changes at
// the next rising-edge capture event. This is the same "no live resize
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
    // continuation of whatever was frozen before reset()). Does NOT reset
    // the pending loop length set via setLoopLengthMs() -- same convention as
    // DattorroTank::reset() leaving decayGain/dampingCoefficient untouched:
    // reset() clears transient DSP state, not a caller-configured setting.
    void reset() noexcept;

    // Stores a PENDING loop length in milliseconds only -- does NOT change
    // currentLoopLengthSamples or resize/reinterpret any buffer. Takes effect
    // only at the next rising-edge capture event (see the class comment
    // above). Clamped to [minLoopLengthMs, maxLoopLengthMs] here (unlike,
    // say, DattorroTank::setDecay()'s deliberately-unclamped convention):
    // there, an out-of-APVTS-range value is only an audio/tuning concern the
    // parameter layer is responsible for; here, an unclamped value would
    // become a real out-of-bounds buffer index at the next capture, which is
    // a real-time-safety/memory-safety property this class must guarantee on
    // its own, independent of whatever range a later Parameters.cpp
    // enforces upstream.
    void setLoopLengthMs (float newLoopLengthMs) noexcept;

    // Writes wetLeft/wetRight into the rolling history buffers unconditionally
    // (see the class comment above), checks for a rising edge of
    // loopFreezeAmount and captures a fresh loop if one just occurred, reads
    // (and crossfades, if applicable) the current loop-playback sample, and
    // returns the loopFreezeAmount-weighted blend of the live and looped
    // signal: { (1-loopFreezeAmount)*wetLeft + loopFreezeAmount*loopLeft,
    // (1-loopFreezeAmount)*wetRight + loopFreezeAmount*loopRight }. If no
    // loop has ever been captured (currentLoopLengthSamples == 0), the loop
    // signal is treated as 0.0f -- this is only reachable if a caller somehow
    // passes loopFreezeAmount > 0.0f before any rising edge has ever occurred
    // (loopFreezeAmount starts at 0.0f in every real usage), handled
    // defensively rather than reading uninitialized/stale buffer content.
    std::pair<float, float> process (float loopFreezeAmount, float wetLeft, float wetRight) noexcept;

    // Test-only introspection -- same "peek" idiom as
    // DattorroTank::peekFeedbackSignal() and PitchShifter::getPrimaryLastOffset().
    int getCurrentLoopLengthSamples() const noexcept { return currentLoopLengthSamples; }

    // Test-only introspection: raw captured-loop content at a given sample
    // index (channel 0 = left, 1 = right), before any crossfade blending is
    // applied. Returns 0.0f for an out-of-range index rather than reading
    // past the captured-loop buffers' own fixed capacity.
    float peekCapturedLoopSample (int channel, int index) const noexcept;

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

    // Performs the actual rising-edge capture: computes N =
    // currentLoopLengthSamples from the pending ms value (clamped, and
    // additionally clamped to the fixed buffer capacity as a last-resort
    // safety net -- see setLoopLengthMs()'s comment), computes X =
    // activeCrossfadeSamples = min(crossfadeSamplesConstant, N/2), and
    // copies the most recent N samples out of the rolling buffers into the
    // captured-loop buffers via bounded index arithmetic only (no resize,
    // ever). Resets the loop read position to 0. This copy's cost is
    // bounded by maxCapacitySamples (a fixed worst case, not unbounded), and
    // only ever runs on a rising edge -- not every sample -- matching the
    // "bounded circular copy" requirement this method exists to satisfy.
    void captureLoop() noexcept;

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
    // rising-edge capture (see captureLoop()).
    float pendingLoopLengthMs = defaultLoopLengthMs;

    // Last-seen loopFreezeAmount, used purely to detect a 0.0f -> >0.0f
    // rising edge on the NEXT process() call -- not itself an output or a
    // smoothed value (the caller is responsible for arriving pre-smoothed,
    // same convention as every other freeze-aware class here).
    float previousLoopFreezeAmount = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoopCapture)
};
