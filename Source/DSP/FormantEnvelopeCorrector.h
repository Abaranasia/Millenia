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
    //
    // ALSO tracks `semitones` itself now (2026-08-29, see
    // ratioStabilityToleranceSemitones' comment) so processSample()/
    // processQuadratureSample() can detect whether the target ratio has
    // drifted since the coefficients currently in use were fit, and bypass
    // correction while it has -- see docs/formant-preserving-pitch-shifter-
    // research.md section 10's "Freeze-dial pitch shift" known issue.
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

    // Applies a correction filter to a DIFFERENT shifted signal (the
    // quadrature pool's output) than the one processSample() analyzed.
    //
    // REVISED 2026-08-29 (stereo-width fix, see
    // docs/formant-preserving-pitch-shifter-research.md section 10's
    // "residual lo-fi/reduced width" known issue): this USED TO simply reuse
    // processSample()'s own aShifted/aDry coefficients outright (a deliberate
    // CPU-saving shortcut from this class's original design, on the
    // reasoning that both grain pools read the identical shared delay line).
    // A first DIAGNOSTIC measurement (hand-rolled parallel prototype, not
    // the real class) suggested a dramatic fix -- 48.9% of uncorrected width
    // survived shared, vs 83.3% independently-fit. That number turned out to
    // be a measurement artifact: the prototype used one STATIC one-shot fit
    // with never-refit history, while the real corrector this compares
    // against keeps re-fitting and crossfading every hop -- a filter that
    // never changes is trivially less correlated with one that keeps
    // changing. Measured HONESTLY once implemented for real (both signals
    // through the real, dynamically-refitting corrector,
    // FormantEnvelopeCorrectorTests.cpp's "Quadrature's independent LPC fit
    // measurably (if modestly) improves..." test): independent fitting
    // retains only ~50% of uncorrected width, barely above the shared-
    // coefficient baseline. Conclusion: coefficient sharing was NOT the
    // dominant cause of the width loss after all -- most of it is intrinsic
    // to correcting toward dry's envelope at all, regardless of whose
    // coefficients are used (primary's and quadrature's own NATURAL formant
    // structure is nearly identical to begin with, so the fine time-domain
    // decorrelation the correction destroys was never really about a
    // formant-frequency mismatch between the two channels). Kept anyway (a
    // small real improvement, and a more principled per-channel analysis)
    // but this does NOT close the known issue -- see section 10's updated
    // note. Runs its own independent LPC analysis of quadrature's own
    // shifted signal (its own ring buffer, on the SAME hop schedule
    // processSample() already drives, see quadratureAnalysisPending's
    // comment) while still REUSING aDry/aDryIncoming from processSample() --
    // the dry reference genuinely is one shared signal, unlike the shifted
    // one, so only the shifted-side analysis needed independence. Measured
    // cost of the added analysis: RTF dropped from 40.40x (single analysis)
    // to a still-comfortable 20.13x worst-case (two fully independent
    // analyses, more pessimistic than this actual implementation, which
    // shares the dry analysis).
    //
    // Uses its own independent filter-state HISTORY (as before) so the
    // quadrature signal's recent samples don't corrupt the primary path's
    // history or vice versa. Respects the same armed/passthrough gate as
    // processSample().
    float processQuadratureSample (float shifted) noexcept;

    // TEST-ONLY: whether quadrature's own LPC fit has diverged from its
    // reset()-time identity/trivial state -- lets a test verify
    // processQuadratureSample() is performing its own independent analysis
    // (see that method's 2026-08-29 revision comment) by driving quadrature
    // alone with real content and confirming this becomes true, without
    // exposing the raw coefficient arrays themselves.
    bool debugQuadratureCoefficientsAreNonTrivial() const noexcept
    {
        for (int i = 1; i <= lpcOrder; ++i)
            if (std::abs (aShiftedQuadrature[(size_t) i]) > 1.0e-9)
                return true;
        return false;
    }

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

    // Shared LPC-fit machinery factored out of runAnalysis() (2026-08-29,
    // stereo-width fix) so processQuadratureSample() can run its own
    // independent fit on quadratureShiftedRing using the exact same
    // math/scratch buffers as runAnalysis() uses for dryRing/shiftedRing --
    // see processQuadratureSample()'s own comment for why this exists.
    void estimateLpcFit (const std::vector<float>& ring, std::array<double, lpcOrder + 1>& aOut, double& gainOut) noexcept;

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

    // Freeze-dial pitch-tracking fix (2026-08-29, see
    // docs/formant-preserving-pitch-shifter-research.md section 10's
    // "Freeze-dial pitch shift" known issue). MEASURED (not assumed): a
    // continuously-sweeping ratio produced a 12x larger instantaneous-
    // frequency tracking error in the corrected output than in the raw
    // (uncorrected) shifted signal over the same sweep
    // (FormantEnvelopeCorrectorTests.cpp's "does a continuously-sweeping
    // pitch ratio..." DIAGNOSTIC test: 9.55Hz raw vs 115.07Hz corrected RMS
    // deviation from the expected curve). Root cause: this class's LPC
    // coefficients are only re-fit once per hopSamples from a windowSamples
    // analysis window (see class comment) -- while a ratio is actively
    // moving (e.g. mid-Freeze-dial-drag), those coefficients' resonant poles
    // are fit for wherever the ratio USED to be, not where the signal's
    // actual fundamental has since moved to, and imposing a stale envelope
    // onto a signal whose real pitch has moved distorts its zero-crossing
    // pattern.
    //
    // currentSemitones: the live target, updated on every
    // setPitchShiftSemitones() call (previously that call only updated
    // `armed`). lastFitSemitones: a snapshot of currentSemitones taken the
    // moment runAnalysis() last ran (i.e. what the CURRENTLY-LIVE
    // coefficients were actually fit for) -- see processSample()'s .cpp
    // comment for exactly where this is snapshotted.
    float currentSemitones = 0.0f;
    float lastFitSemitones = 0.0f;

    // How far currentSemitones may drift from lastFitSemitones before
    // correction fades out completely -- a CONTINUOUS blend toward
    // passthrough as drift grows from 0 (full correction) to
    // ratioStabilityToleranceSemitones (zero correction), not a hard
    // on/off switch. A hard switch was tried first: it DID reduce the
    // measured tracking error vs no fix at all (115.07Hz -> 26.95Hz at
    // tolerance=0.02), but tightening the tolerance further made the error
    // go back UP, not down (41.06Hz at tolerance=0.001) -- a non-monotonic,
    // suspicious result that traced to the hard switch itself: an
    // uncrossfaded jump between corrected and raw output, repeated every
    // single hop throughout a sweep, introduces its own discontinuity/click
    // artifact -- exactly the failure mode this class's own crossfadeMs
    // mechanism already exists to avoid for coefficient updates (see
    // crossfadeMs's own comment). Switched to this continuous blend instead
    // (see getCorrectionBlendAmount()'s comment for the math): at the SAME
    // tolerance=0.02, measured 27.76Hz -- matching the hard switch's best
    // result, but via a click-free fade instead of a discontinuous jump.
    // Value is a REASONED starting point (a fast dial sweep in the
    // DIAGNOSTIC test moved ~0.16 semitones/hop; this sits below that so a
    // genuinely fast sweep visibly fades toward passthrough, not just
    // barely dents it), NOT exhaustively tuned or confirmed by ear -- needs
    // a by-ear pass to validate the fade itself doesn't read as its own
    // artifact, and the residual gap to the raw shifter's own 9.55Hz
    // baseline (a real but smaller remaining tracking error, likely from
    // the LPC analysis window itself spanning already-non-stationary
    // content even right after a fresh hop) is not further addressed here.
    static constexpr float ratioStabilityToleranceSemitones = 0.02f;

    // 0.0 (drift >= tolerance, full passthrough) to 1.0 (drift == 0, full
    // correction) -- see ratioStabilityToleranceSemitones' comment.
    float getCorrectionBlendAmount() const noexcept
    {
        const float drift = std::abs (currentSemitones - lastFitSemitones);
        return juce::jlimit (0.0f, 1.0f, 1.0f - drift / ratioStabilityToleranceSemitones);
    }

    int windowSamples = 0;
    int hopSamples = 0;
    int hopCounter = 0;
    int ringWriteIndex = 0;

    std::vector<float> dryRing, shiftedRing;           // size windowSamples, sized in prepare()

    // Quadrature's OWN shifted-signal ring (2026-08-29, stereo-width fix) --
    // written by processQuadratureSample() at the SAME ring index
    // processSample() just wrote dryRing/shiftedRing into this exact audio
    // sample (processSample() always runs first and has already advanced
    // ringWriteIndex by the time this is written, see processQuadratureSample()'s
    // .cpp comment), keeping all three rings time-aligned on one shared
    // hop/window schedule. There is no separate quadratureDryRing: dry
    // genuinely is one shared signal between primary and quadrature, so
    // dryRing/aDry are reused as-is.
    std::vector<float> quadratureShiftedRing;          // size windowSamples, sized in prepare()

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

    // Quadrature's OWN shifted-signal coefficients (2026-08-29, stereo-width
    // fix -- see processQuadratureSample()'s comment for the measured
    // before/after numbers). aDry/aDryIncoming above ARE still reused
    // as-is for quadrature (dry genuinely is one shared signal); only the
    // shifted side needed its own fit, since that's where primary and
    // quadrature's actual sample streams genuinely differ.
    std::array<double, lpcOrder + 1> aShiftedQuadrature;
    std::array<double, lpcOrder + 1> aShiftedQuadratureIncoming;

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
    // gainShiftedQuadrature/gainShiftedQuadratureIncoming (2026-08-29) are
    // quadrature's own pair, computed alongside aShiftedQuadrature/
    // aShiftedQuadratureIncoming -- gainDry/gainDryIncoming are still
    // shared, same reasoning as the coefficients above.
    double gainDry = 1.0;
    double gainShifted = 1.0;
    double gainDryIncoming = 1.0;
    double gainShiftedIncoming = 1.0;
    double gainShiftedQuadrature = 1.0;
    double gainShiftedQuadratureIncoming = 1.0;

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
    // counterpart for the same reason as above. Before 2026-08-29 the
    // quadrature signal shared aShifted/aShiftedIncoming (the coefficients)
    // with the primary path too, and only its HISTORY was independent; now
    // it has its own aShiftedQuadrature/aShiftedQuadratureIncoming as well
    // (see processQuadratureSample()'s comment) -- dry's coefficients
    // (aDry/aDryIncoming) remain the one genuinely-shared piece, since dry
    // really is a single common signal.
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

    // Set by processSample() whenever its own hop-boundary analysis just
    // ran (mirrors quadratureCrossfadeSettled's need for a cross-call flag,
    // same root cause: processQuadratureSample() runs AFTER processSample()
    // within one audio sample, so it cannot itself detect "a hop just
    // completed" -- it can only be told). Consumed by processQuadratureSample(),
    // which fits aShiftedQuadratureIncoming/gainShiftedQuadratureIncoming
    // from quadratureShiftedRing the first time it observes this true, then
    // clears it. 2026-08-29, stereo-width fix.
    bool quadratureAnalysisPending = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FormantEnvelopeCorrector)
};
