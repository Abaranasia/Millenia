#pragma once

#include <algorithm>
#include <cmath>

// Rate-limited, log-domain glide for the tank's Size.
//
// Why this exists. Resizing a recirculating delay loop resamples the audio
// already circulating in it, so a Size move retunes the wet tail by
//
//     pitch ratio = 1 - d ln(size) / dt
//
// (shrinking raises the pitch, growing lowers it). The old glide was a
// 300 ms linear ramp, so a 1 -> 0.5 jump bent the tail by hundreds of cents
// whatever the knob did. Here the glide moves ln(size) at a bounded rate,
// |d ln(size) / dt| <= maxRatePerSecond, so the instantaneous bend is bounded
// no matter how fast or how far the knob is moved:
//
//     bend (cents) <= 1200 * log2 (1 + maxRatePerSecond)   (shrinking)
//     bend (cents) <= 1200 * |log2 (1 - maxRatePerSecond)| (growing)
//
// Shape. The log-rate (velocity) v follows a saturated proportional command
//
//     vCommand = clamp (k * (ln target - ln current), -maxRate, +maxRate)
//
// through a one-pole of time constant `velocityTauSeconds`, with
// k = 1 / (4 * tau). That makes the unsaturated part exactly critically
// damped (the damping ratio is 1 for k = 1 / (4 tau)), so the glide never
// overshoots the target, and because v itself is smoothed it starts and stops
// without a velocity discontinuity (no hard ramp edge). When the saturated
// phase hands over to the linear one the state (e = maxRate / k, v = maxRate)
// has a strictly positive critically-damped coefficient, so the error never
// crosses zero: the glide is monotonic.
//
// Real-time safety: no allocation, no locks; one std::exp per sample while
// moving and nothing but a flag test while idle. While idle next() returns the
// target exactly as the float that was set, so a constant Size is bit-identical
// to a plain value (the golden-hash tests rely on that).
class SizeGlide
{
public:
    // Both limits are in the units documented above. Re-calling prepare()
    // keeps the current target and snaps to it (like juce::SmoothedValue::reset).
    void prepare (double newSampleRate, double newMaxRatePerSecond, double newVelocityTauSeconds) noexcept
    {
        dt = 1.0 / newSampleRate;
        maxRate = newMaxRatePerSecond;
        velocityCoeff = 1.0 - std::exp (-dt / newVelocityTauSeconds);
        positionGain = 1.0 / (4.0 * newVelocityTauSeconds);
        snapTo (targetSize);
    }

    // Jump straight to `size` with no glide (prepare()/reset()).
    void snapTo (float size) noexcept
    {
        targetSize = size;
        lnTarget = std::log ((double) size);
        lnCurrent = lnTarget;
        velocity = 0.0;
        moving = false;
    }

    void setTarget (float size) noexcept
    {
        if (size == targetSize)
            return;

        targetSize = size;
        lnTarget = std::log ((double) size);
        moving = true;
    }

    float getTarget() const noexcept { return targetSize; }
    bool isMoving() const noexcept { return moving; }

    // Current log-rate in 1/s (diagnostics and tests).
    double getLogRate() const noexcept { return velocity; }

    // Advances one sample and returns the glided Size.
    float next() noexcept
    {
        if (! moving)
            return targetSize;

        const double error = lnTarget - lnCurrent;
        const double command = std::clamp (positionGain * error, -maxRate, maxRate);
        velocity += (command - velocity) * velocityCoeff;
        lnCurrent += velocity * dt;

        // Close enough that the remaining distance is far below one-hundredth
        // of a sample of delay: land exactly on the target and go idle.
        if (std::abs (lnTarget - lnCurrent) < snapDistance && std::abs (velocity) < snapRate)
        {
            snapTo (targetSize);
            return targetSize;
        }

        return (float) std::exp (lnCurrent);
    }

private:
    static constexpr double snapDistance = 1.0e-6;
    static constexpr double snapRate = 1.0e-4;

    double dt = 1.0 / 44100.0;
    double maxRate = 0.1;
    double velocityCoeff = 0.0;
    double positionGain = 1.0;

    float targetSize = 1.0f;
    double lnTarget = 0.0;
    double lnCurrent = 0.0;
    double velocity = 0.0;
    bool moving = false;
};
