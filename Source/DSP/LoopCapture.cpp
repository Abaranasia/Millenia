#include "LoopCapture.h"

void LoopCapture::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;

    maxCapacitySamples = juce::jmax (1, juce::roundToInt (maxLoopLengthMs * 0.001 * sampleRate));
    crossfadeSamplesConstant = juce::jmax (0, juce::roundToInt (crossfadeMs * 0.001 * sampleRate));

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

void LoopCapture::captureLoop() noexcept
{
    // Second clamp (defense in depth): even though setLoopLengthMs() already
    // clamps pendingLoopLengthMs to [minLoopLengthMs, maxLoopLengthMs], this
    // final jlimit against maxCapacitySamples is the actual hard safety net
    // against ever reading or writing past the fixed-size captured-loop
    // buffers, regardless of how pendingLoopLengthMs got its value.
    const int requestedSamples = juce::roundToInt (pendingLoopLengthMs * 0.001 * sampleRate);
    currentLoopLengthSamples = juce::jlimit (1, maxCapacitySamples, requestedSamples);

    activeCrossfadeSamples = juce::jmin (crossfadeSamplesConstant, currentLoopLengthSamples / 2);

    // Bounded circular copy: rollingWriteIndex already points one PAST the
    // most recently written sample (this call's own input sample included,
    // since process() writes to the rolling buffers before checking for a
    // rising edge -- see process() below). capturedLoop[N-1] is therefore
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

    // 1b. Advance the engage/disengage ramp by exactly one sample -- see
    // setLoopFreezeAmount()'s comment for why this class owns the ramp
    // itself instead of trusting a pre-smoothed caller value.
    const float loopFreezeAmount = smoothedLoopFreezeAmount.getNextValue();

    // 2. Rising-edge trigger.
    if (previousLoopFreezeAmount == 0.0f && loopFreezeAmount > 0.0f)
        captureLoop();

    previousLoopFreezeAmount = loopFreezeAmount;

    // 3. Read (and advance/wrap) the loop-playback position.
    float loopLeft = 0.0f;
    float loopRight = 0.0f;

    if (currentLoopLengthSamples > 0)
    {
        loopLeft = readLoopChannel (capturedLoopLeft, readPosition);
        loopRight = readLoopChannel (capturedLoopRight, readPosition);

        ++readPosition;
        if (readPosition >= currentLoopLengthSamples)
            readPosition = activeCrossfadeSamples; // NOT 0 -- see the class comment in LoopCapture.h
    }

    return { (1.0f - loopFreezeAmount) * wetLeft + loopFreezeAmount * loopLeft,
             (1.0f - loopFreezeAmount) * wetRight + loopFreezeAmount * loopRight };
}

float LoopCapture::peekCapturedLoopSample (int channel, int index) const noexcept
{
    if (index < 0 || index >= (int) capturedLoopLeft.size())
        return 0.0f;

    return channel == 0 ? capturedLoopLeft[(size_t) index] : capturedLoopRight[(size_t) index];
}
