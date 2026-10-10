#include "LoopCapture.h"
#include <limits>

void LoopCapture::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;

    maxCapacitySamples = juce::jmax (1, juce::roundToInt (maxLoopLengthMs * 0.001 * sampleRate));
    crossfadeSamplesConstant = juce::jmax (0, juce::roundToInt (crossfadeMs * 0.001 * sampleRate));
    silenceResetSamples = juce::jmax (1, juce::roundToInt (silenceResetMs * 0.001 * sampleRate));

    rollingLeft.assign ((size_t) maxCapacitySamples, 0.0f);
    rollingRight.assign ((size_t) maxCapacitySamples, 0.0f);
    capturedLoopLeft.assign ((size_t) maxCapacitySamples, 0.0f);
    capturedLoopRight.assign ((size_t) maxCapacitySamples, 0.0f);

    smoothedLoopFreezeAmount.reset (sampleRate, loopFreezeRampSeconds);

    reset();
}

void LoopCapture::reset() noexcept
{
    std::fill (rollingLeft.begin(), rollingLeft.end(), 0.0f);
    std::fill (rollingRight.begin(), rollingRight.end(), 0.0f);
    std::fill (capturedLoopLeft.begin(), capturedLoopLeft.end(), 0.0f);
    std::fill (capturedLoopRight.begin(), capturedLoopRight.end(), 0.0f);

    rollingWriteIndex = 0;
    currentLoopLengthSamples = 0;
    activeCrossfadeSamples = 0;
    readPosition = 0;
    previousLoopFreezeAmount = 0.0f;
    captureArmed = false;
    capturePending = false;
    audibleHistorySamples = 0;    // history is all zeros again, and no sound onset yet --
    consecutiveSilentSamples = 0; // see the class comment's "Wait for sound" section
    captureCount = 0;
    smoothedLoopFreezeAmount.setCurrentAndTargetValue (0.0f);
}

void LoopCapture::setLoopFreezeAmount (float newAmount) noexcept
{
    smoothedLoopFreezeAmount.setTargetValue (juce::jlimit (0.0f, 1.0f, newAmount));
}

void LoopCapture::setLoopLengthMs (float newLoopLengthMs) noexcept
{
    // See this setter's header comment: clamped here (unlike
    // DattorroTank::setDecay()'s deliberately-unclamped convention) because
    // an out-of-range value here would become a real out-of-bounds buffer
    // index at the next capture, not just a differently-tuned sound.
    pendingLoopLengthMs = juce::jlimit (minLoopLengthMs, maxLoopLengthMs, newLoopLengthMs);
}

int LoopCapture::requestedLoopLengthSamples() const noexcept
{
    // Second clamp (defense in depth): even though setLoopLengthMs() already
    // clamps pendingLoopLengthMs to [minLoopLengthMs, maxLoopLengthMs], this
    // final jlimit against maxCapacitySamples is the actual hard safety net
    // against ever reading or writing past the fixed-size captured-loop
    // buffers, regardless of how pendingLoopLengthMs got its value.
    const int requestedSamples = juce::roundToInt (pendingLoopLengthMs * 0.001 * sampleRate);
    return juce::jlimit (1, maxCapacitySamples, requestedSamples);
}

void LoopCapture::captureLoop() noexcept
{
    currentLoopLengthSamples = requestedLoopLengthSamples();

    activeCrossfadeSamples = juce::jmin (crossfadeSamplesConstant, currentLoopLengthSamples / 2);

    // Bounded circular copy: rollingWriteIndex already points one PAST the
    // most recently written sample (this call's own input sample included,
    // since process() writes to the rolling buffers before checking any of
    // the three capture triggers -- see process() below). capturedLoop[N-1] is therefore
    // that most recent sample, capturedLoop[0] is (N-1) samples older than
    // it. No resize happens here -- only index arithmetic into the already
    // fixed-size rolling/captured-loop buffers, bounded by
    // currentLoopLengthSamples <= maxCapacitySamples.
    for (int i = 0; i < currentLoopLengthSamples; ++i)
    {
        int sourceIndex = rollingWriteIndex - currentLoopLengthSamples + i;
        sourceIndex = ((sourceIndex % maxCapacitySamples) + maxCapacitySamples) % maxCapacitySamples;

        capturedLoopLeft[(size_t) i] = rollingLeft[(size_t) sourceIndex];
        capturedLoopRight[(size_t) i] = rollingRight[(size_t) sourceIndex];
    }

    readPosition = 0;

    if (captureCount < std::numeric_limits<int>::max())
        ++captureCount;
}

float LoopCapture::readLoopChannel (const std::vector<float>& capturedLoop, int position) const noexcept
{
    const int N = currentLoopLengthSamples;
    const int X = activeCrossfadeSamples;

    if (X > 0 && position >= N - X)
    {
        const int k = position - (N - X);
        const float t = (float) k / (float) X;
        const float in = fadeIn (t);
        const float out = 1.0f - in; // fadeOut -- sums to exactly 1.0f with fadeIn(t) by construction

        return out * capturedLoop[(size_t) position] + in * capturedLoop[(size_t) k];
    }

    return capturedLoop[(size_t) position];
}

std::pair<float, float> LoopCapture::process (float wetLeft, float wetRight) noexcept
{
    // 1. Unconditional rolling-history write, regardless of loopFreezeAmount.
    rollingLeft[(size_t) rollingWriteIndex] = wetLeft;
    rollingRight[(size_t) rollingWriteIndex] = wetRight;
    rollingWriteIndex = (rollingWriteIndex + 1) % maxCapacitySamples;

    // 1a. Audible-history tracking -- see LoopCapture.h's "Wait for sound"
    // section (2026-10-10 follow-up). Ints/bools/floats only, no branches
    // that allocate or throw.
    const bool isSound = juce::jmax (std::abs (wetLeft), std::abs (wetRight)) > signalGateLinear;

    if (isSound)
    {
        consecutiveSilentSamples = 0;
        audibleHistorySamples = juce::jmin (audibleHistorySamples + 1, maxCapacitySamples);
    }
    else
    {
        consecutiveSilentSamples = juce::jmin (consecutiveSilentSamples + 1, silenceResetSamples + 1);

        if (consecutiveSilentSamples > silenceResetSamples)
            audibleHistorySamples = 0; // nothing playing -- restart at the next onset
        else if (audibleHistorySamples > 0)
            audibleHistorySamples = juce::jmin (audibleHistorySamples + 1, maxCapacitySamples); // short pause: part of the music
    }

    // 1b. Advance the engage/disengage ramp by exactly one sample -- see
    // setLoopFreezeAmount()'s comment for why this class owns the ramp
    // itself instead of trusting a pre-smoothed caller value.
    float loopFreezeAmount = smoothedLoopFreezeAmount.getNextValue();
    const float targetLoopFreezeAmount = smoothedLoopFreezeAmount.getTargetValue();

    // 2. Capture trigger / deferred capture -- see LoopCapture.h's "Re-arm
    // and deferred capture" section (2026-10-10 fix) for the full rationale.
    // "Parked" = ramp AND target both <= rearmThreshold: the user has
    // brought the dial near zero and the ramp has actually got there (a
    // quick touch faster than the ramp never parks -- intentionally keeps
    // the old loop).
    const bool parkedNearZero = loopFreezeAmount <= rearmThreshold && targetLoopFreezeAmount <= rearmThreshold;

    if (capturePending)
    {
        if (parkedNearZero)
        {
            capturePending = false; // cancelled -- dial returned near zero before enough audible history existed
        }
        else if (hasEnoughAudibleHistory())
        {
            // Deferred capture lands: N recomputed from pendingLoopLengthMs
            // NOW (inside captureLoop()), same as an immediate capture. Then
            // re-run the normal 50ms engage ramp from 0.0f toward the current
            // target -- same click-free mechanism as a normal engage. This
            // sample takes the ramp's first step (> 0.0f unless the target is
            // itself 0.0f), so previousLoopFreezeAmount below cannot form a
            // spurious exact-zero edge on the next sample.
            captureLoop();
            capturePending = false;
            smoothedLoopFreezeAmount.setCurrentAndTargetValue (0.0f);
            smoothedLoopFreezeAmount.setTargetValue (targetLoopFreezeAmount);
            loopFreezeAmount = smoothedLoopFreezeAmount.getNextValue();
        }
    }
    else
    {
        // The original exact 0.0f -> >0.0f edge, verbatim (same sample as
        // pre-2026-10-10 behavior), OR an armed rise above
        // retriggerThreshold (hysteresis -- see rearmThreshold's comment).
        const bool exactZeroEdge = previousLoopFreezeAmount == 0.0f && loopFreezeAmount > 0.0f;
        const bool armedRise = captureArmed && loopFreezeAmount > retriggerThreshold;

        if (exactZeroEdge || armedRise)
        {
            captureArmed = false;

            // Only capture once the last N samples are all post-onset
            // audible history -- never unwritten zeros since reset(), and
            // (2026-10-10 reversal, see LoopCapture.h's "Wait for sound")
            // never a window holding a silent run longer than
            // silenceResetSamples. Otherwise defer.
            if (hasEnoughAudibleHistory())
                captureLoop();
            else
                capturePending = true;
        }
    }

    // Re-arm whenever parked near zero (including right after a capture
    // that fired from an exact-zero edge toward a target that is itself
    // <= rearmThreshold -- that loop was captured at an inaudible blend, so
    // letting a later real raise replace it is the expected behavior).
    if (parkedNearZero)
        captureArmed = true;

    previousLoopFreezeAmount = loopFreezeAmount;

    // Pending, or nothing captured yet: no loop to blend, so the live wet
    // signal passes through untouched (see process()'s header comment).
    // Known, bounded edge: if an OLD loop exists when a capture goes pending
    // (possible by lengthening Loop Length past the audible history, or --
    // since the "wait for sound" gate -- by parking and re-raising within N
    // samples of a sound onset after a silence longer than
    // silenceResetSamples), the switch to passthrough here -- and back
    // to that old loop if the pending capture is then cancelled -- is a step
    // of at most retriggerThreshold/rearmThreshold of the loop-vs-wet
    // difference (<= -34dB), since both only happen at those blend levels.
    if (capturePending || currentLoopLengthSamples == 0)
        return { wetLeft, wetRight };

    // 3. Read (and advance/wrap) the loop-playback position.
    const float loopLeft = readLoopChannel (capturedLoopLeft, readPosition);
    const float loopRight = readLoopChannel (capturedLoopRight, readPosition);

    ++readPosition;
    if (readPosition >= currentLoopLengthSamples)
        readPosition = activeCrossfadeSamples; // NOT 0 -- see the class comment in LoopCapture.h

    return { (1.0f - loopFreezeAmount) * wetLeft + loopFreezeAmount * loopLeft,
             (1.0f - loopFreezeAmount) * wetRight + loopFreezeAmount * loopRight };
}

float LoopCapture::peekCapturedLoopSample (int channel, int index) const noexcept
{
    if (index < 0 || index >= (int) capturedLoopLeft.size())
        return 0.0f;

    return channel == 0 ? capturedLoopLeft[(size_t) index] : capturedLoopRight[(size_t) index];
}
