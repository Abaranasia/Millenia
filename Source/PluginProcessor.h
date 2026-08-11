/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "DSP/ScratchSchroederTank.h"
#include "DSP/DattorroTank.h"

//==============================================================================
/**
*/
class MilleniaAudioProcessor  : public juce::AudioProcessor
{
public:
    //==============================================================================
    MilleniaAudioProcessor();
    ~MilleniaAudioProcessor() override;

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
    // Phase 1 plumbing-validation tank — kept alongside dattorroTank for A/B
    // comparison until a human confirms the Dattorro tank sounds right by
    // ear; not removed yet (see docs/shimmer-reverb-implementation-plan.md).
    ScratchSchroederTank scratchTank;

    // Phase 2 committed topology.
    DattorroTank dattorroTank;

    // Mono scratch buffer shared by both test-mode tanks above — pre-sized
    // in prepareToPlay so processBlock never allocates.
    juce::AudioBuffer<float> monoScratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MilleniaAudioProcessor)
};
