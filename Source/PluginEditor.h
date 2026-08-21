#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
// Phase 6: the Pitch Shift knob is bipolar (-24..+24 st, centred on 0), so the
// default LookAndFeel_V4 rotary fill (which always fills from the range
// MINIMUM) would visually read as "mostly cranked up" even at the neutral
// default of +12st. This override fills from the ZERO-value angle instead,
// matching how a bipolar EQ-style knob is expected to read. Only applied to
// the Pitch Shift slider -- every other Phase 5 parameter is naturally
// unipolar (0..something) and needs no special handling.
class BipolarRotaryLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                            float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                            juce::Slider& slider) override;
};

//==============================================================================
class MilleniaAudioProcessorEditor  : public juce::AudioProcessorEditor
{
public:
    MilleniaAudioProcessorEditor (MilleniaAudioProcessor&);
    ~MilleniaAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void configureRotary (juce::Slider& slider, juce::Label& label, const juce::String& labelText);

    MilleniaAudioProcessor& audioProcessor;

    BipolarRotaryLookAndFeel bipolarLookAndFeel;

    juce::Slider pitchShiftSlider, feedbackSlider, shimmerAmountSlider, dampingSlider, widthSlider, mixSlider;
    juce::Label  pitchShiftLabel,  feedbackLabel,  shimmerAmountLabel,  dampingLabel,  widthLabel,  mixLabel;
    juce::ToggleButton bypassButton { "Bypass" };

    juce::TextButton preset7Button  { "+7 st" };
    juce::TextButton preset12Button { "+12 st" };
    juce::TextButton preset19Button { "+19 st" };

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    std::unique_ptr<SliderAttachment> pitchShiftAttachment;
    std::unique_ptr<SliderAttachment> feedbackAttachment;
    std::unique_ptr<SliderAttachment> shimmerAmountAttachment;
    std::unique_ptr<SliderAttachment> dampingAttachment;
    std::unique_ptr<SliderAttachment> widthAttachment;
    std::unique_ptr<SliderAttachment> mixAttachment;
    std::unique_ptr<ButtonAttachment> bypassAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MilleniaAudioProcessorEditor)
};
