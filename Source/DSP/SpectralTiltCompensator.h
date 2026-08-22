#pragma once

#include <JuceHeader.h>

// Chipmunk-mitigation, cheap fallback (2026-08-23, see
// docs/formant-preserving-pitch-shifter-research.md section 6, item 4 --
// "Option B" in the coordinator's own framing of that document's ranked
// recommendations). NOT true formant preservation -- that would require the
// scoped LPC-based envelope-correction investigation the research doc calls
// Option C. This is a crude, zero-added-latency partial mitigation for the
// same complaint (by ear: "chipmunk effect... works nice for low notes, but
// sounds a bit ridiculous on higher notes"), diagnosed as formant/envelope
// shift, not a pitch-tracking error (PitchShifterTests.cpp's input-frequency
// sweep measured <=0.011% pitch error across 110-3520Hz).
//
// Mechanism: a naive pitch shift drags the WHOLE spectral envelope up with
// the fundamental, which is what reads as thinner/brighter/"chipmunk"-y on
// upward shifts. This darkens the shifted signal with a single one-pole
// lowpass (the exact same leaky-integrator primitive DattorroTank's own
// damping filter and DCBlocker's highpass already use in this codebase),
// whose coefficient grows with how far upward the current shift is -- NOT
// true formant correction (it darkens everything above a cutoff, it does
// not move formant peaks back to where they started), just a gross
// spectral-tilt approximation. A one-pole IIR needs no lookahead or block
// buffering, so unlike every option in the research doc's comparison table,
// this adds NO latency and cannot disturb the recirculating loop's
// already-tuned round-trip timing (see that doc's section 4 for why added
// latency inside this specific loop is a serious cost, not a minor one).
//
// Deliberately scoped to UPWARD shifts only for this first pass: at
// semitones <= 0 the coefficient is exactly 0.0f, an exact passthrough
// (bit-identical, same "no behavior change unless actually needed"
// convention every other conditional DSP stage in this codebase follows).
// Downward-shift "barely noticeable" was a SEPARATE, earlier-investigated
// complaint (docs/shimmer-reverb-implementation-plan.md's Phase 9 carried-
// over item) whose root cause was left open as likely psychoacoustic, not
// established as the same tilt mechanism -- this class does not touch that
// case, and should not be assumed to fix it.
class SpectralTiltCompensator
{
public:
    SpectralTiltCompensator() = default;

    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        maxCoefficient = std::exp (-juce::MathConstants<float>::twoPi * targetCutoffHzAtMaxTilt / (float) sampleRate);
        reset();
    }

    void reset() noexcept
    {
        filterState = 0.0f;
    }

    // Recomputes the darkening coefficient from the current EFFECTIVE pitch
    // shift (i.e. whatever is actually being applied to the shifter right
    // now, post any freeze crossfade -- not the raw user-facing knob value)
    // -- cheap enough to call once per block, same convention as every
    // other per-block DSP parameter update in this codebase.
    void setPitchShiftSemitones (float semitones) noexcept
    {
        const float upwardSemitones = juce::jlimit (0.0f, maxSemitonesForFullTilt, semitones);
        // Linear in COEFFICIENT space (not cutoff-frequency space) between
        // 0.0f at semitones<=0 (exact passthrough) and maxCoefficient at
        // maxSemitonesForFullTilt -- deliberately simple/crude, consistent
        // with this being a cheap approximation, not a precision filter
        // sweep. Guarantees the semitones<=0 case is bit-identical to no
        // filtering at all, which a naive cutoff-frequency interpolation
        // (see this project's own Damping-range bug, Parameters.cpp) would
        // not have cleanly given.
        coefficient = (upwardSemitones / maxSemitonesForFullTilt) * maxCoefficient;
    }

    float processSample (float input) noexcept
    {
        filterState = coefficient * filterState + (1.0f - coefficient) * input;
        return filterState;
    }

private:
    // Reference points for the crude linear-in-coefficient ramp above --
    // REASONED starting points, not exhaustively ear-tuned. At the classic
    // shimmer default (+12st), this coefficient corresponds to roughly an
    // 8kHz cutoff (gentle); at the full +24st extreme, roughly 3kHz
    // (noticeably darker, by design, since that's where the complaint is
    // worst).
    static constexpr float maxSemitonesForFullTilt = 24.0f;
    static constexpr float targetCutoffHzAtMaxTilt = 3000.0f;

    double sampleRate = 44100.0;
    float maxCoefficient = 0.0f;
    float coefficient = 0.0f;
    float filterState = 0.0f;
};
