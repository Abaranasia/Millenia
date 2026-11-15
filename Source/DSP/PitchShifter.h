#pragma once

#include <JuceHeader.h>

// Hand-rolled dual-delay-line crossfade pitch shifter: own circular
// buffer, two read pointers advancing at a rate derived from the pitch
// ratio, crossfaded near wrap discontinuities. Takes a pitch-ratio (or
// semitone) value per block; owns no parameter knowledge itself.
class PitchShifter
{
public:
    PitchShifter();
    ~PitchShifter();

    void prepare (const juce::dsp::ProcessSpec& spec);
    void reset();
    void setPitchSemitones (float semitones);
    void process (juce::dsp::AudioBlock<float>& block);

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchShifter)
};
