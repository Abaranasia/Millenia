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
    // 2026-08-22: replaces the old freezeButton ToggleButton -- Freeze's
    // underlying parameter became a continuous float (see Parameters.cpp's
    // comment) specifically so it can be dialed in live over a sounding
    // signal instead of only snapping between 0 and 1, so it now belongs in
    // the same rotary-knob row/pattern as every other continuous parameter,
    // not the top toggle row.
    // Labelled "Freeze Amount" (not plain "Freeze") to read distinctly from
    // freezeQuickToggle below, which is intentionally still just "Freeze".
    configureRotary (freezeSlider,   freezeLabel,   "Freeze Amount");

    // Phase 10 (see docs/shimmer-reverb-implementation-plan.md): Loop Freeze
    // -- additive, fully independent knob/toggle pair alongside Freeze
    // Amount/freezeQuickToggle above. Loop Length controls the captured
    // window's size, so it belongs in the same rotary-knob row as every
    // other continuous parameter, same as freezeSlider.
    configureRotary (loopLengthSlider, loopLengthLabel, "Loop Length");

    addAndMakeVisible (bypassButton);
    addAndMakeVisible (freezeQuickToggle);
    addAndMakeVisible (loopFreezeToggle);

    // Recovered by user request (2026-08-22): a quick full-freeze toggle
    // above the Freeze knob. Checking remembers the dial's current value
    // then jumps it to 1.0; unchecking restores the remembered value. Goes
    // through freezeSlider.setValue(), never audioProcessor/apvts directly,
    // same convention as the pitch presets below.
    freezeQuickToggle.onClick = [this]
    {
        if (freezeQuickToggle.getToggleState())
        {
            freezeValueBeforeQuickToggle = (float) freezeSlider.getValue();
            freezeSlider.setValue (1.0, juce::sendNotificationSync);
        }
        else
        {
            freezeSlider.setValue ((double) freezeValueBeforeQuickToggle, juce::sendNotificationSync);
        }
    };

    for (auto* button : { &presetNeg12Button, &preset0Button, &preset7Button, &preset12Button, &preset19Button })
        addAndMakeVisible (button);

    // Quick-select presets set the SAME pitchShift parameter value the knob
    // controls -- not a separate parameter/mode (Phase 5's plan explicitly
    // rules that out). Routed through the already-attached slider, never
    // through audioProcessor/apvts directly, so this stays inside the
    // "GUI never touches processor state directly" rule.
    // Phase 9 adds the two missing presets (0st, -12st) from the original
    // UI mock, alongside the existing +7/+12/+19 (see
    // docs/shimmer-reverb-implementation-plan.md, Phase 9).
    presetNeg12Button.onClick = [this] { pitchShiftSlider.setValue (-12.0, juce::sendNotificationSync); };
    preset0Button.onClick  = [this] { pitchShiftSlider.setValue (0.0,   juce::sendNotificationSync); };
    preset7Button.onClick  = [this] { pitchShiftSlider.setValue (7.0,  juce::sendNotificationSync); };
    preset12Button.onClick = [this] { pitchShiftSlider.setValue (12.0, juce::sendNotificationSync); };
    preset19Button.onClick = [this] { pitchShiftSlider.setValue (19.0, juce::sendNotificationSync); };

    pitchShiftAttachment = std::make_unique<SliderAttachment> (audioProcessor.apvts, ParamIDs::pitchShift, pitchShiftSlider);
    feedbackAttachment   = std::make_unique<SliderAttachment> (audioProcessor.apvts, ParamIDs::feedback,   feedbackSlider);
    shimmerAmountAttachment = std::make_unique<SliderAttachment> (audioProcessor.apvts, ParamIDs::shimmerAmount, shimmerAmountSlider);
    dampingAttachment    = std::make_unique<SliderAttachment> (audioProcessor.apvts, ParamIDs::damping,    dampingSlider);
    widthAttachment       = std::make_unique<SliderAttachment> (audioProcessor.apvts, ParamIDs::width,      widthSlider);
    mixAttachment        = std::make_unique<SliderAttachment> (audioProcessor.apvts, ParamIDs::mix,        mixSlider);
    freezeAttachment     = std::make_unique<SliderAttachment> (audioProcessor.apvts, ParamIDs::freeze,     freezeSlider);
    bypassAttachment     = std::make_unique<ButtonAttachment> (audioProcessor.apvts, ParamIDs::bypass,     bypassButton);
    loopLengthAttachment = std::make_unique<SliderAttachment> (audioProcessor.apvts, ParamIDs::loopLength, loopLengthSlider);
    loopFreezeAttachment = std::make_unique<ButtonAttachment> (audioProcessor.apvts, ParamIDs::loopFreeze, loopFreezeToggle);

    // Phase 6's own goal is a functional editor, not final-polish (see plan
    // doc) -- fixed-size, non-resizable is a deliberate choice for this
    // milestone, not an unexamined default; a scalable/resizable layout is
    // left for a later polish pass.
    setResizable (false, false);
    // Widened from 620 (6 knobs) to fit the Freeze knob, then 720 -> 820
    // (Phase 10) to fit the new Loop Length knob at the same per-knob width
    // the other 7 already use.
    setSize (820, 320);
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
    topArea.removeFromRight (8); // gap between toggles
    freezeQuickToggle.setBounds (topArea.removeFromRight (80));
    topArea.removeFromRight (8); // gap between toggles
    loopFreezeToggle.setBounds (topArea.removeFromRight (80));

    bounds.removeFromTop (20); // headroom for the attachToComponent labels drawn above each knob

    auto presetArea = bounds.removeFromBottom (28);

    juce::FlexBox knobBox;
    knobBox.flexDirection  = juce::FlexBox::Direction::row;
    knobBox.justifyContent = juce::FlexBox::JustifyContent::spaceAround;

    for (auto* slider : { &pitchShiftSlider, &feedbackSlider, &shimmerAmountSlider, &dampingSlider, &widthSlider, &mixSlider, &freezeSlider, &loopLengthSlider })
        knobBox.items.add (juce::FlexItem (*slider).withMinWidth (90.0f).withMinHeight (100.0f));

    knobBox.performLayout (bounds);

    // Quick-select pitch presets sit under the Pitch Shift knob's own
    // column, not spread across the whole width -- they only ever affect
    // that one parameter.
    // Phase 9 widens this row from 3 to 5 buttons (-12/0/+7/+12/+19 st) --
    // still just the one pitchShift column, not spread wider.
    auto pitchColumnWidth  = bounds.getWidth() / 5;
    auto presetRow         = presetArea.removeFromLeft (pitchColumnWidth);
    auto presetButtonWidth = presetRow.getWidth() / 5;
    presetNeg12Button.setBounds (presetRow.removeFromLeft (presetButtonWidth).reduced (2));
    preset0Button.setBounds     (presetRow.removeFromLeft (presetButtonWidth).reduced (2));
    preset7Button.setBounds     (presetRow.removeFromLeft (presetButtonWidth).reduced (2));
    preset12Button.setBounds    (presetRow.removeFromLeft (presetButtonWidth).reduced (2));
    preset19Button.setBounds    (presetRow.reduced (2));
}
