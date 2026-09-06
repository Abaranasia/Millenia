/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "DSP/ShimmerReverbEngine.h"
#include "Parameters.h"

//==============================================================================
/**
*/
class MilleniaAudioProcessor  : public juce::AudioProcessor
{
public:
    //==============================================================================
    MilleniaAudioProcessor();
    ~MilleniaAudioProcessor() override;

    // Phase 5 (see docs/shimmer-reverb-implementation-plan.md): the single
    // APVTS instance backing every host-automatable parameter, built from
    // Parameters.h's createParameterLayout(). Public (rather than
    // private+accessor) because Phase 6's editor needs to construct
    // juce::AudioProcessorValueTreeState::SliderAttachment/ButtonAttachment
    // instances directly against this member -- the juce-plugin-dev skill's
    // "GUI reads/writes via APVTS attachments" hard rule requires the editor
    // to be able to reach it.
    juce::AudioProcessorValueTreeState apvts;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

private:
    //==============================================================================
    // Phase 3 committed topology: the shimmer engine owns the tank, the
    // pitch shifter, and its own mono scratch buffer internally.
    // PluginProcessor just forwards prepare/process calls. Phase 5 adds real
    // APVTS parameters on top (see apvts above and processBlock()'s comment)
    // -- this is no longer 100% wet with no dry/wet mix, that gap is closed.
    ShimmerReverbEngine shimmerReverbEngine;

    // Phase 5: raw parameter pointers, cached ONCE in the constructor (after
    // apvts exists) rather than looked up by string every processBlock()
    // call -- apvts.getRawParameterValue() is a string lookup and must never
    // be called from the audio thread. Each points at an
    // std::atomic<float>-backed value owned by apvts; .load() it every
    // block instead. bypassParam is a bool parameter but its raw
    // APVTS-backed value is still a float (0.0/1.0) -- cached and loaded the
    // same way as the others, compared with `> 0.5f`.
    std::atomic<float>* pitchShiftParam = nullptr;
    std::atomic<float>* feedbackParam   = nullptr;
    std::atomic<float>* shimmerAmountParam = nullptr;
    std::atomic<float>* shimmerSustainParam = nullptr;
    std::atomic<float>* dampingParam    = nullptr;
    std::atomic<float>* widthParam      = nullptr;
    std::atomic<float>* mixParam        = nullptr;
    std::atomic<float>* bypassParam     = nullptr;

    // Phase 9 (see docs/shimmer-reverb-implementation-plan.md): AudioParameterFloat
    // since 2026-08-22 (was originally AudioParameterBool, same convention as
    // bypassParam above) -- switched so the editor's Freeze dial can set any
    // value in [0, 1], not just snap between the two extremes.
    std::atomic<float>* freezeParam     = nullptr;

    // Phase 10 (see docs/shimmer-reverb-implementation-plan.md): Loop Freeze
    // -- additive, fully independent of freezeParam above. loopFreezeParam
    // backs an AudioParameterBool but, like bypassParam, its raw APVTS-backed
    // value is still a float (0.0/1.0). Read RAW every block with no
    // smoother here -- unlike every other continuous parameter below, its
    // smoothing moved INTO LoopCapture itself (2026-09-06 fix, see
    // LoopCapture::setLoopFreezeAmount()'s comment): the block-granularity
    // smoothing that used to live here could click on a large-enough host
    // buffer, since Loop Freeze is always driven by a discrete on/off toggle
    // (a hard full-range target jump every time), unlike a continuously-
    // dragged dial. loopLengthParam is read RAW too, for the unrelated
    // "only matters at a discrete instant" reason described below.
    std::atomic<float>* loopFreezeParam = nullptr;
    std::atomic<float>* loopLengthParam = nullptr;

    // Phase 5: one smoother per continuous parameter, driven from the cached
    // atomics above and .skip()'d once per block in processBlock() before
    // pushing into shimmerReverbEngine -- see that method's comment for why
    // bypass has no smoother of its own (it drives smoothedMix instead).
    juce::SmoothedValue<float> smoothedPitchShift;
    juce::SmoothedValue<float> smoothedFeedback;
    juce::SmoothedValue<float> smoothedShimmerAmount;
    juce::SmoothedValue<float> smoothedShimmerSustain;
    juce::SmoothedValue<float> smoothedDamping;
    juce::SmoothedValue<float> smoothedWidth;
    juce::SmoothedValue<float> smoothedMix;

    // Phase 9: Freeze gets the exact same click-free smoothing treatment as
    // every other continuous parameter here -- even a hard 0->1 jump on
    // decayGain/dry-mute inside the tank would click, same reasoning as
    // bypass driving smoothedMix instead of a hard switch (see
    // processBlock()'s comment).
    juce::SmoothedValue<float> smoothedFreeze;

    // Schema v1 -- the first version ever; no migration logic exists yet.
    // A future schema bump needs an explicit migration branch in
    // setStateInformation() that reads the OLD version's property layout
    // before calling apvts.replaceState(), not a silent overwrite.
    static constexpr int currentSchemaVersion = 1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MilleniaAudioProcessor)
};
