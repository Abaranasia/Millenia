#pragma once

#include <JuceHeader.h>

// Input diffuser (4x series allpass) feeding two cross-feeding tank
// branches with damping, per the Dattorro (1997) plate topology. Owns
// its own DelayLine<Lagrange3rd> and IIR::Filter instances. Exposes
// tunable points as plain setters, not parameters itself — parameter
// routing lives in Phase 5 (see docs/shimmer-reverb-implementation-plan.md).
class DattorroTank
{
public:
    DattorroTank();
    ~DattorroTank();

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    void process (juce::dsp::AudioBlock<float>& block);

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DattorroTank)
};
