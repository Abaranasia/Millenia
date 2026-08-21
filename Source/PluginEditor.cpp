#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
void BipolarRotaryLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                                  float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                                                  juce::Slider& slider)
{
    auto radius  = (float) juce::jmin (width / 2, height / 2) - 4.0f;
    auto centreX = (float) x + (float) width  * 0.5f;
    auto centreY = (float) y + (float) height * 0.5f;
    auto angle   = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    auto lineW      = juce::jmin (8.0f, radius * 0.5f);
    auto arcRadius  = radius - lineW * 0.5f;

    juce::Path backgroundArc;
    backgroundArc.addCentredArc (centreX, centreY, arcRadius, arcRadius, 0.0f,
                                  rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (slider.findColour (juce::Slider::rotarySliderOutlineColourId));
    g.strokePath (backgroundArc, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (slider.isEnabled())
    {
        // Bipolar fill: start the value arc at the ZERO position, not the
        // range minimum, so a positive/negative pitch shift reads as
        // "filled up/down from centre" instead of "filled from the bottom."
        auto zeroPos   = (float) slider.valueToProportionOfLength (0.0);
        auto zeroAngle = rotaryStartAngle + zeroPos * (rotaryEndAngle - rotaryStartAngle);

        juce::Path valueArc;
        valueArc.addCentredArc (centreX, centreY, arcRadius, arcRadius, 0.0f,
                                 zeroAngle, angle, true);
        g.setColour (slider.findColour (juce::Slider::rotarySliderFillColourId));
        g.strokePath (valueArc, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    auto thumbWidth = lineW * 2.0f;
    juce::Point<float> thumbPoint (centreX + arcRadius * std::cos (angle - juce::MathConstants<float>::halfPi),
                                    centreY + arcRadius * std::sin (angle - juce::MathConstants<float>::halfPi));
    g.setColour (slider.findColour (juce::Slider::thumbColourId));
    g.fillEllipse (juce::Rectangle<float> (thumbWidth, thumbWidth).withCentre (thumbPoint));
}

//==============================================================================
MilleniaAudioProcessorEditor::MilleniaAudioProcessorEditor (MilleniaAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    configureRotary (pitchShiftSlider, pitchShiftLabel, "Pitch Shift");
    pitchShiftSlider.setLookAndFeel (&bipolarLookAndFeel);
    configureRotary (feedbackSlider, feedbackLabel, "Feedback");
    configureRotary (shimmerAmountSlider, shimmerAmountLabel, "Shimmer Amount");
    configureRotary (dampingSlider,  dampingLabel,  "Damping");
    configureRotary (widthSlider,    widthLabel,    "Width");
    configureRotary (mixSlider,      mixLabel,      "Mix");

    addAndMakeVisible (bypassButton);

    for (auto* button : { &preset7Button, &preset12Button, &preset19Button })
        addAndMakeVisible (button);

    // Quick-select presets set the SAME pitchShift parameter value the knob
    // controls -- not a separate parameter/mode (Phase 5's plan explicitly
    // rules that out). Routed through the already-attached slider, never
    // through audioProcessor/apvts directly, so this stays inside the
    // "GUI never touches processor state directly" rule.
    preset7Button.onClick  = [this] { pitchShiftSlider.setValue (7.0,  juce::sendNotificationSync); };
    preset12Button.onClick = [this] { pitchShiftSlider.setValue (12.0, juce::sendNotificationSync); };
    preset19Button.onClick = [this] { pitchShiftSlider.setValue (19.0, juce::sendNotificationSync); };

    pitchShiftAttachment = std::make_unique<SliderAttachment> (audioProcessor.apvts, ParamIDs::pitchShift, pitchShiftSlider);
    feedbackAttachment   = std::make_unique<SliderAttachment> (audioProcessor.apvts, ParamIDs::feedback,   feedbackSlider);
    shimmerAmountAttachment = std::make_unique<SliderAttachment> (audioProcessor.apvts, ParamIDs::shimmerAmount, shimmerAmountSlider);
    dampingAttachment    = std::make_unique<SliderAttachment> (audioProcessor.apvts, ParamIDs::damping,    dampingSlider);
    widthAttachment       = std::make_unique<SliderAttachment> (audioProcessor.apvts, ParamIDs::width,      widthSlider);
    mixAttachment        = std::make_unique<SliderAttachment> (audioProcessor.apvts, ParamIDs::mix,        mixSlider);
    bypassAttachment     = std::make_unique<ButtonAttachment> (audioProcessor.apvts, ParamIDs::bypass,     bypassButton);

    // Phase 6's own goal is a functional editor, not final-polish (see plan
    // doc) -- fixed-size, non-resizable is a deliberate choice for this
    // milestone, not an unexamined default; a scalable/resizable layout is
    // left for a later polish pass.
    setResizable (false, false);
    setSize (620, 320);
}

MilleniaAudioProcessorEditor::~MilleniaAudioProcessorEditor()
{
    pitchShiftSlider.setLookAndFeel (nullptr);
}

void MilleniaAudioProcessorEditor::configureRotary (juce::Slider& slider, juce::Label& label, const juce::String& labelText)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 20);
    addAndMakeVisible (slider);

    label.setText (labelText, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.attachToComponent (&slider, false);
    addAndMakeVisible (label);
}

void MilleniaAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void MilleniaAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds().reduced (10);

    auto topArea = bounds.removeFromTop (24);
    bypassButton.setBounds (topArea.removeFromRight (80));

    bounds.removeFromTop (20); // headroom for the attachToComponent labels drawn above each knob

    auto presetArea = bounds.removeFromBottom (28);

    juce::FlexBox knobBox;
    knobBox.flexDirection  = juce::FlexBox::Direction::row;
    knobBox.justifyContent = juce::FlexBox::JustifyContent::spaceAround;

    for (auto* slider : { &pitchShiftSlider, &feedbackSlider, &shimmerAmountSlider, &dampingSlider, &widthSlider, &mixSlider })
        knobBox.items.add (juce::FlexItem (*slider).withMinWidth (90.0f).withMinHeight (100.0f));

    knobBox.performLayout (bounds);

    // Quick-select pitch presets sit under the Pitch Shift knob's own
    // column, not spread across the whole width -- they only ever affect
    // that one parameter.
    auto pitchColumnWidth  = bounds.getWidth() / 5;
    auto presetRow         = presetArea.removeFromLeft (pitchColumnWidth);
    auto presetButtonWidth = presetRow.getWidth() / 3;
    preset7Button.setBounds  (presetRow.removeFromLeft (presetButtonWidth).reduced (2));
    preset12Button.setBounds (presetRow.removeFromLeft (presetButtonWidth).reduced (2));
    preset19Button.setBounds (presetRow.reduced (2));
}
