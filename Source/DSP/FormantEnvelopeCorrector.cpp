#include "FormantEnvelopeCorrector.h"

FormantEnvelopeCorrector::FormantEnvelopeCorrector() = default;
FormantEnvelopeCorrector::~FormantEnvelopeCorrector() = default;

void FormantEnvelopeCorrector::prepare (const juce::dsp::ProcessSpec& spec)
{
    windowSamples = juce::jmax (1, (int) std::round (analysisWindowMs * 0.001 * spec.sampleRate));
    hopSamples = juce::jmax (1, (int) std::round (hopMs * 0.001 * spec.sampleRate));
    crossfadeSamples = juce::jmax (1, (int) std::round (crossfadeMs * 0.001 * spec.sampleRate));

    dryRing.assign ((size_t) windowSamples, 0.0f);
    shiftedRing.assign ((size_t) windowSamples, 0.0f);
    quadratureShiftedRing.assign ((size_t) windowSamples, 0.0f);

    hammingWindowCoeffs.resize ((size_t) windowSamples);
    for (int i = 0; i < windowSamples; ++i)
        hammingWindowCoeffs[(size_t) i] = 0.54 - 0.46 * std::cos (juce::MathConstants<double>::twoPi * (double) i / (double) (windowSamples - 1));

    windowedScratch.resize ((size_t) windowSamples);
    autocorrelationScratch.assign ((size_t) (lpcOrder + 1), 0.0);

    reset();
}

void FormantEnvelopeCorrector::reset()
{
    std::fill (dryRing.begin(), dryRing.end(), 0.0f);
    std::fill (shiftedRing.begin(), shiftedRing.end(), 0.0f);
    std::fill (quadratureShiftedRing.begin(), quadratureShiftedRing.end(), 0.0f);
    ringWriteIndex = 0;
    hopCounter = 0;

    aDry.fill (0.0);
    aDry[0] = 1.0;
    aShifted.fill (0.0);
    aShifted[0] = 1.0;
    aShiftedQuadrature.fill (0.0);
    aShiftedQuadrature[0] = 1.0;

    aDryIncoming.fill (0.0);
    aDryIncoming[0] = 1.0;
    aShiftedIncoming.fill (0.0);
    aShiftedIncoming[0] = 1.0;
    aShiftedQuadratureIncoming.fill (0.0);
    aShiftedQuadratureIncoming[0] = 1.0;

    gainDry = 1.0;
    gainShifted = 1.0;
    gainDryIncoming = 1.0;
    gainShiftedIncoming = 1.0;
    gainShiftedQuadrature = 1.0;
    gainShiftedQuadratureIncoming = 1.0;

    crossfadeCounter = crossfadeSamples; // idle: no crossfade in progress at reset
    quadratureCrossfadeSettled = true;
    quadratureAnalysisPending = false;

    whiteningHistory.fill (0.0);
    resynthesisHistory.fill (0.0);
    whiteningHistoryIncoming.fill (0.0);
    resynthesisHistoryIncoming.fill (0.0);

    quadratureWhiteningHistory.fill (0.0);
    quadratureResynthesisHistory.fill (0.0);
    quadratureWhiteningHistoryIncoming.fill (0.0);
    quadratureResynthesisHistoryIncoming.fill (0.0);

    currentSemitones = 0.0f;
    lastFitSemitones = 0.0f;
}

void FormantEnvelopeCorrector::setPitchShiftSemitones (float semitones) noexcept
{
    armed = semitones > 0.0f;
    currentSemitones = semitones;
}

double FormantEnvelopeCorrector::levinsonDurbin (const double* r, int order, double* aOut) noexcept
{
    aOut[0] = 1.0;
    for (int i = 1; i <= order; ++i)
        aOut[i] = 0.0;

    double err = r[0];
    if (err <= 0.0)
        return err;

    std::array<double, lpcOrder + 1> prev {};
    for (int i = 1; i <= order; ++i)
    {
        double acc = r[i];
        for (int j = 1; j < i; ++j)
            acc += aOut[j] * r[i - j];
        double k = -acc / err;

        for (int j = 0; j <= order; ++j)
            prev[(size_t) j] = aOut[j];

        aOut[i] = k;
        for (int j = 1; j < i; ++j)
            aOut[j] = prev[(size_t) j] + k * prev[(size_t) (i - j)];

        err *= (1.0 - k * k);
        if (err <= 0.0)
            break;
    }
    return err;
}

void FormantEnvelopeCorrector::estimateLpcFit (const std::vector<float>& ring, std::array<double, lpcOrder + 1>& aOut, double& gainOut) noexcept
{
    for (int i = 0; i < windowSamples; ++i)
    {
        int idx = (ringWriteIndex + i) % windowSamples;
        windowedScratch[(size_t) i] = (double) ring[(size_t) idx] * hammingWindowCoeffs[(size_t) i];
    }

    for (int lag = 0; lag <= lpcOrder; ++lag)
    {
        double sum = 0.0;
        for (int i = 0; i + lag < windowSamples; ++i)
            sum += windowedScratch[(size_t) i] * windowedScratch[(size_t) (i + lag)];
        autocorrelationScratch[(size_t) lag] = sum;
    }

    double err = levinsonDurbin (autocorrelationScratch.data(), lpcOrder, aOut.data());
    gainOut = std::sqrt (juce::jmax (1.0e-12, err));
}

void FormantEnvelopeCorrector::runAnalysis() noexcept
{
    estimateLpcFit (dryRing, aDryIncoming, gainDryIncoming);
    estimateLpcFit (shiftedRing, aShiftedIncoming, gainShiftedIncoming);
}

float FormantEnvelopeCorrector::applyTwoPassFilter (float shifted,
                                                      const std::array<double, lpcOrder + 1>& aCoeffsShifted,
                                                      const std::array<double, lpcOrder + 1>& aCoeffsDry,
                                                      double gainScalar,
                                                      std::array<double, lpcOrder>& whiteningHist,
                                                      std::array<double, lpcOrder>& resynthesisHist) const noexcept
{
    // Whitening FIR pass: w[n] = shifted[n] + sum_{k=1}^{p} aCoeffsShifted[k] * shifted[n-k]
    double acc = (double) shifted;
    for (int k = 1; k <= lpcOrder; ++k)
        acc += aCoeffsShifted[(size_t) k] * whiteningHist[(size_t) (k - 1)];
    double w = acc;

    for (int k = lpcOrder - 1; k > 0; --k)
        whiteningHist[(size_t) k] = whiteningHist[(size_t) (k - 1)];
    whiteningHist[0] = (double) shifted;

    // Resynthesis IIR pass: y[n] = w[n] - sum_{k=1}^{p} aCoeffsDry[k] * y[n-k]
    double yAcc = w;
    for (int k = 1; k <= lpcOrder; ++k)
        yAcc -= aCoeffsDry[(size_t) k] * resynthesisHist[(size_t) (k - 1)];
    double y = yAcc;

    for (int k = lpcOrder - 1; k > 0; --k)
        resynthesisHist[(size_t) k] = resynthesisHist[(size_t) (k - 1)];
    resynthesisHist[0] = y;

    return (float) (y * gainScalar);
}

float FormantEnvelopeCorrector::processSample (float dry, float shifted) noexcept
{
    dryRing[(size_t) ringWriteIndex] = dry;
    shiftedRing[(size_t) ringWriteIndex] = shifted;
    ringWriteIndex = (ringWriteIndex + 1) % windowSamples;

    if (++hopCounter >= hopSamples)
    {
        hopCounter = 0;

        // If a previous crossfade was still in progress when this new hop
        // landed (not expected at the current 5ms/20ms ratio, but handled
        // correctly regardless), the incoming filter from that ramp becomes
        // the new outgoing one first, so we always crossfade from whatever
        // is CURRENTLY live, never skip/abandon an in-progress ramp.
        if (crossfadeCounter < crossfadeSamples)
        {
            aDry = aDryIncoming;
            aShifted = aShiftedIncoming;
            gainDry = gainDryIncoming;
            gainShifted = gainShiftedIncoming;
            whiteningHistory = whiteningHistoryIncoming;
            resynthesisHistory = resynthesisHistoryIncoming;
            quadratureWhiteningHistory = quadratureWhiteningHistoryIncoming;
            quadratureResynthesisHistory = quadratureResynthesisHistoryIncoming;
            // 2026-08-29: quadrature's own coefficients/gain need the same
            // early promotion in this (rare) edge case -- they are no
            // longer shared with aShifted/gainShifted above.
            aShiftedQuadrature = aShiftedQuadratureIncoming;
            gainShiftedQuadrature = gainShiftedQuadratureIncoming;
        }

        runAnalysis(); // writes fresh coefficients into aDryIncoming/aShiftedIncoming

        // Snapshot what target ratio these fresh coefficients were actually
        // fit for (2026-08-29, Freeze-dial pitch-tracking fix -- see
        // lastFitSemitones' header comment). If currentSemitones drifts away
        // from this before the NEXT hop, that's exactly the "coefficients
        // are stale relative to where the signal's real pitch has moved"
        // condition the ratioStable check below exists to catch.
        lastFitSemitones = currentSemitones;

        // Seed the incoming filter's history by copying the (now-current)
        // outgoing filter's history -- it starts the ramp from the same
        // recent-signal context, not artificial silence.
        whiteningHistoryIncoming = whiteningHistory;
        resynthesisHistoryIncoming = resynthesisHistory;
        quadratureWhiteningHistoryIncoming = quadratureWhiteningHistory;
        quadratureResynthesisHistoryIncoming = quadratureResynthesisHistory;

        crossfadeCounter = 0;
        // Quadrature has NOT yet caught its own history up through this
        // sample's seed (that happens lazily in processQuadratureSample(),
        // which runs after this function within the same audio sample) --
        // see quadratureCrossfadeSettled's declaration comment. Its own
        // coefficient fit (aShiftedQuadratureIncoming) is equally lazy, for
        // the same reason -- quadratureShiftedRing hasn't received THIS
        // sample's quadrature value yet -- see quadratureAnalysisPending.
        quadratureCrossfadeSettled = false;
        quadratureAnalysisPending = true;
    }

    if (! armed)
        return shifted;

    // 2026-08-29, Freeze-dial pitch-tracking fix: blend toward passthrough
    // (NOT a hard switch -- see getCorrectionBlendAmount()'s comment for why)
    // as the target ratio drifts from what the currently-live coefficients
    // were actually fit for. The crossfade state machine below (coefficient
    // interpolation, hop-boundary promotion) runs completely UNCHANGED
    // regardless of this blend -- it's a real-time invariant (see its own
    // comments) that must keep advancing on its own schedule; only the
    // FINAL output blends toward `shifted` at the very end.
    const float correctionBlend = getCorrectionBlendAmount();

    float correctedOutput;
    float outgoingOutput = applyTwoPassFilter (shifted, aShifted, aDry, gainDry / gainShifted, whiteningHistory, resynthesisHistory);

    if (crossfadeCounter >= crossfadeSamples)
    {
        correctedOutput = outgoingOutput;
    }
    else
    {
        float incomingOutput = applyTwoPassFilter (shifted, aShiftedIncoming, aDryIncoming, gainDryIncoming / gainShiftedIncoming, whiteningHistoryIncoming, resynthesisHistoryIncoming);

        ++crossfadeCounter;

        if (crossfadeCounter >= crossfadeSamples)
        {
            // The ramp completes on this exact sample. Promote the incoming
            // filter (coefficients + primary history) to be the new outgoing
            // one immediately -- not on the next hop -- so every sample from
            // here to the next hop continues from a single, fully-caught-up
            // filter instead of a stale pre-ramp one. (This is the fix for a
            // gap in the naive version of this logic: if promotion were
            // deferred to "whenever the next hop happens to land", the filter
            // would silently keep computing with old, abandoned coefficients
            // for the remainder of the hop once the ramp's blend weight had
            // already reached 1.0 on the incoming filter -- reintroducing
            // exactly the kind of discontinuity this fix exists to remove, the
            // moment crossfadeCounter next fails the ">= crossfadeSamples"
            // check above with the wrong coefficients loaded.)
            //
            // Quadrature's OWN history is promoted separately by
            // processQuadratureSample(), not here -- at this point in time it
            // has not yet advanced quadratureWhiteningHistoryIncoming/
            // quadratureResynthesisHistoryIncoming through this exact sample
            // (that call happens after this one), so promoting it here would
            // copy a history one sample short. See quadratureCrossfadeSettled.
            aDry = aDryIncoming;
            aShifted = aShiftedIncoming;
            gainDry = gainDryIncoming;
            gainShifted = gainShiftedIncoming;
            whiteningHistory = whiteningHistoryIncoming;
            resynthesisHistory = resynthesisHistoryIncoming;
            correctedOutput = incomingOutput; // == outgoingOutput + 1*(incomingOutput - outgoingOutput)
        }
        else
        {
            const float t = (float) crossfadeCounter / (float) crossfadeSamples;
            correctedOutput = outgoingOutput + t * (incomingOutput - outgoingOutput);
        }
    }

    return correctionBlend * correctedOutput + (1.0f - correctionBlend) * shifted;
}

float FormantEnvelopeCorrector::processQuadratureSample (float shifted) noexcept
{
    // Push into quadrature's own ring, and lazily fit its own coefficients
    // if processSample() flagged that a hop's analysis just ran (see
    // quadratureAnalysisPending's comment) -- REGARDLESS of the armed gate
    // below, mirroring processSample()'s own "always warm, even while
    // bypassed" convention. Written at the SAME index processSample() just
    // advanced ringWriteIndex past this exact sample, keeping
    // quadratureShiftedRing time-aligned with dryRing/shiftedRing.
    const int writeIndexForThisSample = (ringWriteIndex - 1 + windowSamples) % windowSamples;
    quadratureShiftedRing[(size_t) writeIndexForThisSample] = shifted;

    if (quadratureAnalysisPending)
    {
        estimateLpcFit (quadratureShiftedRing, aShiftedQuadratureIncoming, gainShiftedQuadratureIncoming);
        quadratureAnalysisPending = false;
    }

    if (! armed)
        return shifted;

    // Same continuous blend as processSample() -- see
    // getCorrectionBlendAmount()'s comment. currentSemitones/lastFitSemitones
    // are shared, class-wide state (the target ratio is one single value for
    // both grain pools), so no separate tracking is needed here.
    const float correctionBlend = getCorrectionBlendAmount();

    // processSample() always runs before this, within the same audio
    // sample (see ShimmerReverbEngine::process()), so crossfadeCounter and
    // aDry here already reflect whatever it just did THIS sample --
    // including, if the ramp completed this very sample, promoting aDry to
    // the former "incoming" value. Neither the quadrature-only HISTORY nor
    // (since 2026-08-29) quadrature's own COEFFICIENTS are promoted by
    // processSample() (it can't -- see that function's comment), so catch
    // both up here exactly once, on the first call that observes the ramp
    // already complete. This state advancement runs UNCHANGED regardless of
    // correctionBlend, same rationale as processSample()'s own comment.
    float correctedOutput;

    if (crossfadeCounter >= crossfadeSamples)
    {
        if (! quadratureCrossfadeSettled)
        {
            correctedOutput = applyTwoPassFilter (shifted, aShiftedQuadratureIncoming, aDryIncoming, gainDryIncoming / gainShiftedQuadratureIncoming, quadratureWhiteningHistoryIncoming, quadratureResynthesisHistoryIncoming);
            quadratureWhiteningHistory = quadratureWhiteningHistoryIncoming;
            quadratureResynthesisHistory = quadratureResynthesisHistoryIncoming;
            aShiftedQuadrature = aShiftedQuadratureIncoming;
            gainShiftedQuadrature = gainShiftedQuadratureIncoming;
            quadratureCrossfadeSettled = true;
        }
        else
        {
            correctedOutput = applyTwoPassFilter (shifted, aShiftedQuadrature, aDry, gainDry / gainShiftedQuadrature, quadratureWhiteningHistory, quadratureResynthesisHistory);
        }
    }
    else
    {
        float outgoingOutput = applyTwoPassFilter (shifted, aShiftedQuadrature, aDry, gainDry / gainShiftedQuadrature, quadratureWhiteningHistory, quadratureResynthesisHistory);
        float incomingOutput = applyTwoPassFilter (shifted, aShiftedQuadratureIncoming, aDryIncoming, gainDryIncoming / gainShiftedQuadratureIncoming, quadratureWhiteningHistoryIncoming, quadratureResynthesisHistoryIncoming);

        // NOTE: do not re-increment crossfadeCounter here -- processSample()
        // already owns advancing it once per audio sample; this function only
        // READS the current ramp position and advances its OWN incoming-filter
        // history via the applyTwoPassFilter call above.
        const float t = (float) crossfadeCounter / (float) crossfadeSamples;
        correctedOutput = outgoingOutput + t * (incomingOutput - outgoingOutput);
    }

    return correctionBlend * correctedOutput + (1.0f - correctionBlend) * shifted;
}
