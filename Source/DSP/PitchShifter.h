#pragma once

#include <JuceHeader.h>
#include <array>
#include <vector>

// SOLA-style (Synchronous OverLap-Add) finite-lifetime grain-pool pitch
// shifter (Phase 3; rewritten from a persistent multi-voice crossfade design
// after that design was found to have a real, large pitch-accuracy bug --
// see the postmortem below). One shared DelayLine is written once per
// sample; a small pool of finite-lifetime GRAINS is launched at a fixed hop
// rate, each grain reading the shared delay line at an offset that is a
// function of its own age since launch, crossfaded in/out with a short
// raised-cosine ramp only at its splice points. Mono only -- this class
// only ever sees DattorroTank's mono output, one sample at a time from
// ShimmerReverbEngine's feedback loop. Owns no parameter knowledge itself;
// Phase 5 wires setPitchShiftSemitones() to a real APVTS parameter.
//
// ---------------------------------------------------------------------------
// Postmortem: what the previous (2-voice, then 4-voice) design got wrong
// ---------------------------------------------------------------------------
// The shimmer was reported as "sounds detuned." The original theory was
// comb-filter amplitude coloring: with voices spaced a fixed fraction of a
// grain apart and each voice's read position in the shared delay line
// staying a CONSTANT time-offset from its partners, summing several delayed
// copies of the same recirculating signal was assumed to null certain
// frequencies (comb filtering), and the fix applied was to quadruple the
// voice count per group (2 -> 4) on the theory that more, closer-spaced
// voices would shrink the nearest-neighbor comb lag. That fix did NOT work.
// A quantitative investigation -- feeding a pure sine tone and measuring the
// actual output frequency via two independent methods (autocorrelation and
// zero-crossing timing, which agreed) -- found a REAL, large, non-monotonic
// PITCH error, not just a level/timbre artifact: at the default +12st
// (pitchRatio = 2.0, 220Hz in) the output measured ~420Hz (-4.5%); at +3st
// and +5st there was NO shift at all (output stuck at the input frequency);
// at -12st the error was -45%. Going from 2 to 4 voices made this WORSE, not
// better (N=8 measured errors up to +100%) -- the opposite of what the comb-
// filter theory predicted.
//
// The actual root cause: each voice's own most-recent-grain-restart instant
// is offset from the other voices' by a fixed fraction of the grain length,
// and because each voice's delay value moves at rate -(pitchRatio - 1) per
// sample relative to its OWN restart instant, that fixed time offset between
// voices' restarts becomes a fixed, ratio-DEPENDENT PHASE offset between
// their delay-read positions -- not merely a fixed time-domain lag as the
// comb-filter theory assumed. Summing several copies of the same tone at
// materially different phases, continuously for the ENTIRE grain cycle (the
// old design's full-cycle Hann crossfade meant 4 voices were always
// simultaneously significant), drags the composite waveform's dominant
// frequency far from the intended shifted pitch -- an interference effect,
// not an amplitude-coloring one. This predates the 2-voice design too, and
// gets worse with more voices because more simultaneously-significant,
// differently-phased copies means more interference, not less. The old
// N-voice COLA (constant-overlap-add) identity that justified the 2->4
// change only ever proved the summed ENVELOPE (gain) was constant -- it says
// nothing about phase coherence between the voices' underlying signals, so
// it could not have fixed (and did not fix) a phase-domain bug. A single
// isolated voice, with no crossfade at all, was measured accurate to within
// 0-9% (just wrap-click noise) -- confirming the per-voice delay-ramp math
// itself (voiceDelaySamples()'s old formula, now grainDelaySamples() below)
// was always correct; the bug was specifically in how multiple continuously-
// overlapping voices interfered with each other.
//
// ---------------------------------------------------------------------------
// The fix: confine the overlap window instead of trying to make it constant
// ---------------------------------------------------------------------------
// Rather than have voices persistently self-wrap and overlap for the whole
// grain cycle, this class launches finite-lifetime GRAINS from a small pool
// at a fixed HOP rate that is close to (but shorter than) the grain length
// -- crossfadeFraction (0.1, i.e. 10%) controls how much shorter. Each grain
// lives for exactly grainLengthSamplesInt samples from its own launch, and
// only its first and last crossfadeSamplesInt samples (10% of its life, at
// each end) are ramped by grainWindow() below; the middle ~80% of its life
// plays at full weight with NO other grain overlapping it. This confines the
// phase-interference window that broke the old design to a brief splice
// instead of smearing it across the entire grain -- verified in a scratch
// harness (a double-precision "infinite" history buffer, and separately
// reproduced with real JUCE dsp::DelayLine<float,Lagrange3rd>) to reduce the
// pitch error to roughly +-1-8% across -24..+24 semitones, and specifically
// ~0.5% and ~-1% at the default +12st/-12st shifts -- versus the old
// design's -45%..+27% (and worse at higher voice counts).
//
// Because grains launch on a fixed hop schedule rather than all restarting
// together, and only ~10% of any grain's lifetime overlaps a neighbor, at
// most maxConcurrentGrainsPerGroup (4, a safety margin -- in practice only
// ~2 are ever concurrently active at crossfadeFraction=0.1) grains are alive
// in a pool at once. Each active grain contributes weightedSum += sample *
// weight and weightSum += weight; the group's output is
// weightedSum / weightSum (see processSample()). This per-sample normalize-
// by-live-weight-sum is simpler than, and REPLACES, the old fixed COLA-
// derived constant (0.5 for N=4): it is provably exact for ANY window shape
// and ANY number of simultaneously-active grains, whereas the old constant
// only worked for the specific Hann/N-equally-spaced-phases case it was
// derived for. See grainWindow()'s raised-cosine-ramp definition below for
// the exact window shape used at each grain's splice points.
//
// ---------------------------------------------------------------------------
// Grain-pool layout: two independent pools, generalizing the old A/B-vs-C/D
// group roles
// ---------------------------------------------------------------------------
// primaryGrains (same role as the old primaryVoices, feeds the recirculating
// tank path via processSample()'s return value): launched every hopSamples,
// first launch scheduled immediately on reset() (primarySamplesUntilLaunch =
// 0).
//
// quadratureGrains (same role as the old quadratureVoices, feeds
// ShimmerReverbEngine's stereo-width path via getQuadratureOutput()):
// launched on the identical hopSamples schedule, but staggered by half a hop
// (quadratureSamplesUntilLaunch = hopSamples / 2 in reset()) so its grains'
// splice points fall at different instants than the primary pool's. This
// generalizes the old design's "quadrature phase offset = half of primary's
// voice spacing" role -- both pools read the exact same shared delayLine
// (same history, same instant), so they pitch-shift identical source content
// by the identical ratio; only the launch-schedule offset differs, which is
// what keeps quadratureGrains' instantaneous output decorrelated from
// primaryGrains' despite processing the same input (Airwindows Galactic-
// style quadrature-offset pitch shifting, chosen over per-channel tank
// duplication -- see docs/shimmer-reverb-implementation-plan.md's Phase 4
// section -- precisely because it needs no second tank/delay line and cannot
// disturb DattorroTank's already-tuned recirculating loop).
//
// processSample()'s signature and the meaning of its return value (the
// primary pool's output) and getQuadratureOutput() (the quadrature pool's
// output) are unchanged from before this rewrite -- only the internal
// voice/grain mechanism changed, not the public contract.
class PitchShifter
{
public:
    PitchShifter();
    ~PitchShifter();

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();

    // Recomputes the cached pitchRatio; safe to call at any time, not just
    // in prepare() -- Phase 5 will call this from a parameter listener.
    void setPitchShiftSemitones (float semitones);

    // For inspection/testing of the cached ratio -- this class owns no
    // other externally-visible DSP state.
    float getPitchRatio() const noexcept { return pitchRatio; }

    float processSample (float input);

    // Read-only peek at the quadrature voice group's crossfaded output from
    // the most recent processSample() call -- mirrors the "process, then
    // peek a simultaneously-computed side value" idiom already used by
    // DattorroTank::peekFeedbackSignal(). Not a second processSample()-like
    // method the caller has to invoke separately; the quadrature voices are
    // advanced inside processSample() itself, this just reads what that call
    // already computed.
    float getQuadratureOutput() const noexcept { return quadratureOutput; }

    // Freeze-decay mitigation, experiment 2 (2026-08-22, see
    // docs/fdn-shimmer-reverb-research.md section 9 -- Valhalla Shimmer's
    // granular pitch shifter is reported to deliberately randomize grain
    // timing to decorrelate feedback-loop artifacts; this is the analogous,
    // separately-targeted technique for THIS project's specific measured
    // bug). Crossfades in a slow, small sinusoidal dither on the shifter's
    // own mean delay (see freezeDriftDepthSamples/currentDriftSamples below)
    // as freezeAmount rises. At freezeAmount=0.0f this is bit-identical to
    // no dither at all (depth scales linearly with freezeAmount, reaching
    // exactly 0 there) -- same "no behavior change unless Freeze is actually
    // engaged" convention every other freeze-aware DSP class in this project
    // already follows.
    void setFreezeAmount (float newFreezeAmount) noexcept { freezeAmount = juce::jlimit (0.0f, 1.0f, newFreezeAmount); }

private:
    using DelayLineType = juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd>;

    // A finite-lifetime grain: launched into a pool slot, ages for exactly
    // grainLengthSamplesInt samples, then expires (active = false). See the
    // class-level comment above for why finite lifetimes (versus the old
    // persistent, continuously-self-wrapping voices) fix the pitch-accuracy
    // bug.
    struct Grain
    {
        bool active = false;
        int age = 0; // samples elapsed since this grain's own launch; grain expires when age >= grainLengthSamplesInt

        // Per-grain WSOLA phase-alignment adjustment added on top of the
        // class-wide baseDelaySamples, found once at this grain's launch by
        // findAlignmentOffset(). Fixes the "sounds metallic/robotic" bug:
        // the diagnostic FFT test in PitchShifterTests.cpp measured a
        // spurious spectral sideband at the grain hop rate (~55.5Hz at
        // 44.1kHz) at -92dB (0st, noise floor), -28.3dB (+7st), and -20.0dB
        // (+12st, ~10% amplitude) below the fundamental -- caused by grains
        // always relaunching at the fixed nominal baseDelaySamples with no
        // regard for the phase of the signal they're crossfading against.
        float baseDelayOffset = 0.0f;
    };

    // Fraction of a grain's lifetime spent ramping in (at launch) and out
    // (before expiry) -- the other ~(1 - 2*crossfadeFraction) of its life
    // plays at full weight with no overlap from a neighboring grain. This is
    // the validated value that confines the old design's whole-cycle
    // phase-interference window down to a brief splice; do not change
    // without re-running the -24..+24 semitone error sweep (see
    // PitchShifterTests.cpp).
    static constexpr float crossfadeFraction = 0.1f;

    // Grain slots per pool -- safety margin. At crossfadeFraction=0.1 only
    // ~2 grains are ever concurrently active (one finishing its fade-out
    // while the next is fading in), but 4 slots gives comfortable headroom.
    // Primary and quadrature pools each get their own independent set of 4.
    static constexpr int maxConcurrentGrainsPerGroup = 4;

    //==============================================================================
    // Grain length: 20ms. Predates, and is independent of, this class's
    // rewrite from persistent multi-voice crossfading to the finite-lifetime
    // grain pool described in the class-level comment above -- the pitch-
    // accuracy bug that rewrite fixes was a phase-interference artifact
    // between overlapping voices, not a function of grain length itself, so
    // this value did not need re-deriving when the mechanism changed. See
    // docs/shimmer-reverb-implementation-plan.md's Phase 3/5 notes for this
    // constant's original history.
    static constexpr float grainLengthMs = 20.0f;

    // Full intended eventual range (-24..+24 st, pitchRatio 0.25..4.0), even
    // though Phase 3 only ever hardcodes one shift value at a time -- buffers
    // below are sized for this whole range up front so nothing needs
    // resizing later (the plan's explicit "size for -24 st, not the +12 st
    // default" pitfall).
    static constexpr float minSemitones = -24.0f;
    static constexpr float maxSemitones = 24.0f;

    // baseDelaySamples must stay generously above the minimum needed to keep
    // grainDelaySamples(elapsed) >= 0 across elapsed in [0, grainLengthSamples]
    // for the most extreme *upward* ratio (pitchRatio = 4.0 at +24 st,
    // elapsed=grainLengthSamples needs
    // baseDelaySamples - grainLengthSamples*3.0 >= 0). 4x the grain length
    // gives comfortable margin above that minimum. This margin is governed
    // purely by the elapsed range [0, grainLengthSamples] and pitchRatio's
    // bounds via grainDelaySamples() below -- it does not depend on how many
    // grains are concurrently active, so it is unaffected by this class's
    // rewrite from persistent voices to a finite-lifetime grain pool
    // (re-verified against the new mechanism, not just assumed).
    static constexpr float baseDelayGrainMultiple = 4.0f;

    // Maximum delay-line capacity must comfortably exceed the largest value
    // grainDelaySamples(elapsed) can reach, which happens at the most extreme
    // *downward* ratio (pitchRatio = 0.25 at -24 st, elapsed=grainLengthSamples
    // gives baseDelaySamples + grainLengthSamples*0.75) -- extra margin of 2x
    // the grain length beyond that strict minimum, plus the usual +1 sample
    // headroom convention already used by ScratchSchroederTank/DattorroTank.
    // Same elapsed-range/pitchRatio-only dependency note as
    // baseDelayGrainMultiple above applies here: unaffected by grain count.
    static constexpr float maxDelayExtraGrainMultiple = 2.0f;

    // grainDelaySamples(elapsed) = baseDelaySamples - elapsed * (pitchRatio - 1.0f)
    // This makes the delay change by exactly (1 - pitchRatio) per sample as a
    // grain ages by 1 sample per sample -- precisely the rate needed for the
    // read pointer to move through recorded buffer-content at pitchRatio x
    // real-time, which is the mechanism that produces the pitch shift.
    // Computed directly as a function of the grain's own age (not
    // accumulated with +=) so drift can't creep in. Every grain in both
    // pools calls this same function -- it has no dependency on which pool a
    // grain belongs to.
    float grainDelaySamples (int elapsed) const noexcept;

    // Raised-cosine ramp: 0 at elapsed=0 ramping to 1 over the first
    // crossfadeSamplesInt samples (masking a new grain's read-position
    // discontinuity relative to whatever it's splicing in after), flat at 1
    // for the grain's middle lifetime, then 1 ramping back to 0 over its
    // last crossfadeSamplesInt samples before expiry. See the class-level
    // comment above for why confining this ramp to a short splice (rather
    // than a full-cycle Hann window shared by several always-overlapping
    // voices) is what fixes the pitch-accuracy bug, and for the per-sample
    // weight-sum normalization processSample() applies instead of a fixed
    // constant.
    float grainWindow (int elapsed) const noexcept;

    // WSOLA-style phase-alignment search (fixes the "sounds metallic/robotic"
    // artifact -- see class-level comment). Finds the outgoing (oldest active)
    // grain in `grains`, extrapolates its future trajectory from the shared
    // delay line, and searches candidate launch offsets around TWO small
    // anchor windows -- [previousOffset +- alignmentSearchRadiusSamples] (the
    // caller's running per-pool offset estimate, for cheap continuous-drift
    // tracking) AND [0 +- alignmentSearchRadiusSamples] (the fixed nominal
    // origin, re-checked every single call) -- for the one candidate whose
    // own extrapolated trajectory has the highest normalized cross-
    // correlation with the outgoing grain's, over a window of
    // alignmentReferenceBuffer.size() samples (== crossfadeSamplesInt, the
    // actual overlap length -- directly targets what's audible at the
    // splice). Returns previousOffset unchanged if there is no active grain
    // yet to align against (e.g. the very first grain launched right after
    // reset()), and otherwise returns the new running offset -- the caller
    // must both store this back for the next launch AND use it as this
    // grain's own baseDelayOffset.
    //
    // Why two anchors, not just previousOffset: seeding the search purely
    // from the previous grain's own found offset is what let
    // alignmentSearchRadiusSamples shrink from a full grain length down to a
    // small fraction of one (see its own comment) -- but a previousOffset-ONLY
    // search was measured (via an exhaustive radius sweep from 1 up to 800,
    // i.e. matching the old fixed-origin design's full search width) to
    // PLATEAU on a bad, self-consistent local optimum at extreme pitch
    // ratios (+24 semitones, pitchRatio=4.0), never reaching within 8dB of
    // the required spectral-sideband threshold no matter how wide the
    // radius: once that search has wandered to a bad region it has no way
    // back, because it only ever looks near wherever it already is.
    // Re-checking the fixed zero anchor at the SAME small radius every call
    // fixes this while staying just as cheap (one more small window, not a
    // full grain-length one) -- see PitchShifterTests.cpp's DIAGNOSTIC tests
    // for the measured before/after numbers.
    float findAlignmentOffset (const std::array<Grain, maxConcurrentGrainsPerGroup>& grains, float previousOffset);

    DelayLineType delayLine;

    // Computed once in prepare() from the live sample rate, not recomputed
    // per sample.
    float grainLengthSamples = 0.0f;
    float baseDelaySamples = 0.0f;

    // Integer-sample versions of grain timing, also computed once in
    // prepare(). grainLengthSamplesInt is the rounded grain lifetime in
    // samples; hopSamples is how often a new grain launches (slightly
    // shorter than grainLengthSamplesInt so consecutive grains overlap only
    // by crossfadeFraction); crossfadeSamplesInt is the ramp length at each
    // end of a grain's life. Both hopSamples and crossfadeSamplesInt are
    // clamped to at least 1 so a pathologically small grain length can't
    // produce a zero-length hop/window and a divide-by-zero in grainWindow().
    int grainLengthSamplesInt = 0;
    int hopSamples = 0;
    int crossfadeSamplesInt = 0;

    // Search radius (samples) used around EACH of findAlignmentOffset()'s two
    // anchor points (previousOffset and the fixed nominal zero -- see that
    // function's own comment), computed once in prepare(). The very first
    // version of this search re-derived the FULL cumulative drift from
    // scratch around the fixed nominal origin at every single grain launch,
    // which required a radius of a full grainLengthSamplesInt to catch up to
    // wherever that drift had wandered -- expensive (see the class-level
    // performance-regression history this was written to fix). Seeding the
    // search from the previous grain's own found offset (which already
    // accounts for all prior drift) let each launch cover only that single
    // hop's INCREMENTAL drift, shrinking the needed radius dramatically --
    // but a previousOffset-only search was then measured to plateau on a bad
    // local optimum at extreme pitch ratios no matter how wide the radius
    // (see findAlignmentOffset()'s comment), which the current two-anchor
    // design fixes by also re-checking the same small radius around zero
    // every call. Tuned empirically at 44.1kHz: 22 samples
    // (crossfadeSamplesInt/4, crossfadeSamplesInt=88) is the smallest value
    // that gets the +24 semitone spectral-sideband test comfortably past its
    // -30dB bar (measured -44.67dB; the search's improvement saturates by
    // radius=24 at -54.2dB, so 22 sits just below that plateau with a little
    // headroom rather than paying for the extra, no-longer-helpful width).
    // See PitchShifter.cpp's prepare() for the exact formula and
    // PitchShifterTests.cpp's DIAGNOSTIC tests for the full measured numbers.
    int alignmentSearchRadiusSamples = 0;

    // Reused scratch buffer for findAlignmentOffset()'s reference window --
    // sized once in prepare() to crossfadeSamplesInt samples, never resized in
    // the audio-thread hot path (real-time safety).
    std::vector<float> alignmentReferenceBuffer;

    // Cached so processSample() never calls std::pow (updated only when
    // setPitchShiftSemitones() is called).
    float pitchRatio = 1.0f;

    // Primary pool: same role as the old primaryVoices group, feeds the
    // recirculating tank path via processSample()'s return value. See the
    // class-level comment above.
    std::array<Grain, maxConcurrentGrainsPerGroup> primaryGrains;

    // Quadrature pool: same role as the old quadratureVoices group, feeds
    // ShimmerReverbEngine's stereo-width path via getQuadratureOutput(). Its
    // launch schedule is staggered by half a hop relative to the primary
    // pool (set in reset()) rather than offset by a fixed phase, since
    // grains no longer carry a continuous phase value. See the class-level
    // comment above.
    std::array<Grain, maxConcurrentGrainsPerGroup> quadratureGrains;

    // Next pool slot each group will launch its next grain into, and how
    // many samples remain until that next launch. Advanced/decremented in
    // processSample(); reset() rewinds both to their startup schedule (see
    // reset()'s own comment).
    int primaryNextSlot = 0;
    int quadratureNextSlot = 0;
    int primarySamplesUntilLaunch = 0;
    int quadratureSamplesUntilLaunch = 0;

    // Running alignment-offset estimate for each pool, carried forward from one
    // grain launch to the next. findAlignmentOffset() searches a SMALL window
    // around this running value (not the fixed nominal baseDelaySamples) so it
    // only needs to track each hop's INCREMENTAL drift, not the full cumulative
    // drift from scratch every time -- see findAlignmentOffset()'s own comment
    // for why this replaces the earlier (correct but far too expensive) design
    // that searched +-grainLengthSamplesInt from a fixed origin at every launch.
    float primaryLastOffset = 0.0f;
    float quadratureLastOffset = 0.0f;

    // Cached crossfaded (and normalized) output of the quadrature pool from
    // the most recent processSample() call, exposed via
    // getQuadratureOutput().
    float quadratureOutput = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchShifter)
};
