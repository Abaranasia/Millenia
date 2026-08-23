#pragma once

#include <JuceHeader.h>
#include <array>
#include <vector>

// LPC-based formant-preserving correction filter for the shimmer's
// recirculating PitchShifter (2026-08-23, see
// docs/formant-preserving-pitch-shifter-research.md sections 8-9 --
// "Option C" in that document's ranked recommendations, the real fix
// SpectralTiltCompensator's crude one-pole darkening (Option B) was proven
// structurally unable to provide). A naive pitch shift drags the WHOLE
// spectral envelope/formant structure up with the fundamental, which is what
// reads as thin/bright/"chipmunk"-y on upward shifts -- SpectralTiltCompensator
// only darkens everything above a cutoff, it never moves the formant peaks
// themselves back toward where they started.
//
// Mechanism: estimate the LPC spectral envelope of the signal ENTERING the
// shifter ("dry", A_dry(z)) and of the signal LEAVING it ("shifted",
// A_shifted(z)), both over a rolling analysis window, then apply
// H(z) = A_shifted(z) / A_dry(z) to the shifted signal as two cascaded
// direct-form passes: a whitening FIR pass through the shifted signal's OWN
// envelope (flattens it), followed by an all-pole resynthesis pass through
// the dry envelope (re-imposes the original formant structure). This is a
// standard LPC cross-synthesis/vocoding technique, validated in a standalone
// (no-JUCE) scratch prototype this session against the REAL PitchShifter's
// actual output (docs/formant-preserving-pitch-shifter-research.md section
// 8) -- LPC order 10 and a 40ms analysis window are the measured safe
// operating window; see lpcOrder/analysisWindowMs below for why those exact
// values, not larger ones, are load-bearing.
//
// Trailing-analysis latency, by design: because the LPC coefficients for a
// given hop are only available once that hop's full analysis window has been
// captured, the correction filter applied to "now" is always built from
// slightly-stale (up to one analysis-window-old) envelope estimates, not the
// instantaneous one. This is an accepted tradeoff (formant envelopes move
// far slower than the underlying pitch, so a ~40ms-stale estimate still
// tracks the vowel/timbre correctly) validated by the same section 8
// investigation -- not a bug to "fix" by shrinking the window without
// re-running that validation (see analysisWindowMs's own comment for why
// shrinking it is not free either).
//
// This class is intentionally standalone: it does NOT read PitchShifter or
// DattorroTank state directly, and it is NOT wired into ShimmerReverbEngine
// as of this class's introduction -- that integration (calling this from
// ShimmerReverbEngine::process() around the existing
// shifter.processSample(tank.peekFeedbackSignal()) call) is deferred to a
// future session. See processSample()'s own comment for the exact call
// contract a future integration must satisfy.
class FormantEnvelopeCorrector
{
public:
    FormantEnvelopeCorrector();
    ~FormantEnvelopeCorrector();

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();

    // Gate: bit-identical passthrough at semitones <= 0, mirroring
    // SpectralTiltCompensator's own upward-shifts-only convention -- the
    // correction was validated (docs/formant-preserving-pitch-shifter-
    // research.md section 8, finding 3) to actively BREAK on downward
    // shifts, not just underperform, so this is a real necessity, not just
    // consistency with the sibling class.
    void setPitchShiftSemitones (float semitones) noexcept;

    // dry = the signal ENTERING the shifter (e.g. tank.peekFeedbackSignal()),
    // shifted = the shifter's own per-sample output for that same instant.
    // Always pushes both into their rolling analysis ring buffers and
    // advances the hop counter (re-fitting LPC coefficients whenever a hop
    // completes) REGARDLESS of the armed/passthrough gate above, so
    // coefficients are already warm the moment a caller re-arms after a
    // period at semitones<=0 -- no cold-start lag. Returns the corrected
    // version of `shifted` when armed, or `shifted` unchanged when not.
    float processSample (float dry, float shifted) noexcept;

    // Applies the SAME LPC coefficients already computed by processSample()'s
    // ongoing analysis of the PRIMARY dry/shifted pair, to a DIFFERENT
    // shifted signal (the quadrature pool's output) -- does NOT push into
    // the ring buffers or trigger its own analysis (see class comment for
    // why: both grain pools read the identical shared delay line, so a
    // second independent analysis would double the per-hop cost for no real
    // gain). Uses its own independent filter-state history so the
    // quadrature signal's recent samples don't corrupt the primary path's
    // history or vice versa. Respects the same armed/passthrough gate as
    // processSample().
    float processQuadratureSample (float shifted) noexcept;

private:
    // Validated-safe operating window (docs/formant-preserving-pitch-
    // shifter-research.md section 8, finding 2): LPC order >=16 or a frame
    // >=80ms was measured to make the Levinson-Durbin estimate itself
    // unreliable on higher-F0 content -- do not raise these without
    // re-running that section's sweep.
    static constexpr int lpcOrder = 10;
    static constexpr float analysisWindowMs = 40.0f;

    // Half the analysis window -- a REASONED starting point (balances
    // coefficient freshness against the per-hop analysis cost), not
    // exhaustively tuned. See docs/formant-preserving-pitch-shifter-
    // research.md section 9.
    static constexpr float hopMs = 20.0f;

    // Output-crossfade ramp length after each hop's coefficient update -- a
    // REASONED starting point (a quarter of hopMs, comfortable margin so
    // ramps never overlap the next hop), not exhaustively tuned. Replaces
    // an earlier, FAILED attempt at directly crossfading the raw LPC
    // coefficient VALUES (reverted 2026-08-23): a full-timeline scan found
    // that approach still produced a ~1.0 spike (on a 0.3-amplitude test
    // signal) partway through its ramp, because linearly interpolating LPC
    // polynomial coefficients does not guarantee every intermediate
    // interpolated set is itself a stable filter -- the straight-line path
    // between two individually-stable filters can pass through a
    // transiently unstable/highly-resonant configuration. This approach
    // instead runs two COMPLETE, independently-stable filter instances (old
    // coefficients, new coefficients) in parallel during the ramp and
    // blends their OUTPUT SAMPLES -- never running either filter under an
    // unvalidated intermediate coefficient set.
    static constexpr float crossfadeMs = 5.0f;

    void runAnalysis() noexcept;

    // r[0..order] in, aOut[0..order] out (aOut[0]=1, A(z)=1+a1 z^-1+...).
    // Returns the final prediction error. Pure function, no allocation --
    // ported verbatim (algorithm only) from the validated scratch prototype.
    static double levinsonDurbin (const double* r, int order, double* aOut) noexcept;

    // Shared two-pass correction filter math (see the .cpp for the exact
    // difference equations) -- takes an explicit coefficient set (aCoeffsDry/
    // aCoeffsShifted) rather than reading class-wide aDry/aShifted directly,
    // so the SAME function can drive both the outgoing (old-coefficient) and
    // incoming (new-coefficient) filter instances during a crossfade with
    // their own independent coefficient sets AND independent history state.
    float applyTwoPassFilter (float shifted,
                               const std::array<double, lpcOrder + 1>& aCoeffsShifted,
                               const std::array<double, lpcOrder + 1>& aCoeffsDry,
                               double gainScalar,
                               std::array<double, lpcOrder>& whiteningHist,
                               std::array<double, lpcOrder>& resynthesisHist) const noexcept;

    bool armed = false;

    int windowSamples = 0;
    int hopSamples = 0;
    int hopCounter = 0;
    int ringWriteIndex = 0;

    std::vector<float> dryRing, shiftedRing;           // size windowSamples, sized in prepare()
    std::vector<double> hammingWindowCoeffs;            // size windowSamples, precomputed in prepare()
    std::vector<double> windowedScratch;                // size windowSamples, reused every hop -- no allocation in processSample()
    std::vector<double> autocorrelationScratch;         // size lpcOrder+1, reused every hop

    // Output-crossfade state (see crossfadeMs above). aDry/aShifted are the
    // OUTGOING filter's coefficients (already in use before this hop);
    // aDryIncoming/aShiftedIncoming are the INCOMING filter's freshly-fit
    // coefficients (from the hop that just completed). Both filter
    // instances run every sample during the ramp, each with its OWN
    // independent history (whiteningHistory/resynthesisHistory for the
    // outgoing filter, whiteningHistoryIncoming/resynthesisHistoryIncoming
    // for the incoming one -- seeded by COPYING the outgoing filter's
    // current history at the moment the ramp starts, so the incoming
    // filter doesn't begin from artificial silence). Once crossfadeCounter
    // reaches crossfadeSamples, the incoming filter's coefficients/history
    // become the new outgoing ones (see processSample()'s .cpp comment).
    std::array<double, lpcOrder + 1> aDry;
    std::array<double, lpcOrder + 1> aShifted;
    std::array<double, lpcOrder + 1> aDryIncoming;
    std::array<double, lpcOrder + 1> aShiftedIncoming;

    // LPC gain-normalization scalars (see class comment / this member's own
    // history for why this exists -- 2026-08-23 by-ear regression: measured
    // +4.11dB loudness boost from this filter on realistic content, entirely
    // from H(z)=A_shifted(z)/A_dry(z) having no inherent reason to preserve
    // signal level). gainDry/gainShifted are sqrt of the outgoing filter's
    // two LPC fits' own prediction-error energy (from levinsonDurbin's
    // return value) -- applying their ratio as an output scalar keeps the
    // filter loudness-neutral, reshaping only the spectral envelope.
    // gainDryIncoming/gainShiftedIncoming are the incoming filter's own
    // pair, promoted alongside aDryIncoming/aShiftedIncoming.
    double gainDry = 1.0;
    double gainShifted = 1.0;
    double gainDryIncoming = 1.0;
    double gainShiftedIncoming = 1.0;

    int crossfadeSamples = 0;
    int crossfadeCounter = 0; // >= crossfadeSamples means "no crossfade in progress"

    // Direct-form state for the two-pass correction filter (FIR whitening
    // through aShifted, then IIR resynthesis through aDry) -- see
    // processSample()'s .cpp comment for the exact difference equations.
    // Index 0 is always the most recent sample (shift-register convention).
    std::array<double, lpcOrder> whiteningHistory;
    std::array<double, lpcOrder> resynthesisHistory;
    std::array<double, lpcOrder> whiteningHistoryIncoming;
    std::array<double, lpcOrder> resynthesisHistoryIncoming;

    // Quadrature path's own independent history, with its own "incoming"
    // counterpart for the same reason as above -- the quadrature signal
    // shares aDry/aShifted/aDryIncoming/aShiftedIncoming (the coefficients)
    // with the primary path, but must never share its HISTORY with it.
    std::array<double, lpcOrder> quadratureWhiteningHistory;
    std::array<double, lpcOrder> quadratureResynthesisHistory;
    std::array<double, lpcOrder> quadratureWhiteningHistoryIncoming;
    std::array<double, lpcOrder> quadratureResynthesisHistoryIncoming;

    // processSample() always runs before processQuadratureSample() within
    // one audio sample (see ShimmerReverbEngine::process()), and it may
    // promote aDry/aShifted/whiteningHistory/resynthesisHistory (the
    // primary path's own state) the instant a ramp completes. The
    // quadrature path's history cannot be promoted at that same instant --
    // processQuadratureSample() hasn't advanced quadratureWhiteningHistoryIncoming
    // through THIS sample yet at that point -- so this flag lets
    // processQuadratureSample() catch its own history up exactly once, on
    // the first call where it observes the ramp already complete. Reset to
    // false whenever a new hop starts a fresh ramp; true means quadrature's
    // history is fully caught up with the currently-live coefficients.
    bool quadratureCrossfadeSettled = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FormantEnvelopeCorrector)
};
