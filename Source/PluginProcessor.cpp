/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
MilleniaAudioProcessor::MilleniaAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       )
#endif
{
}

MilleniaAudioProcessor::~MilleniaAudioProcessor()
{
}

//==============================================================================
const juce::String MilleniaAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool MilleniaAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool MilleniaAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool MilleniaAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double MilleniaAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int MilleniaAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int MilleniaAudioProcessor::getCurrentProgram()
{
    return 0;
}

void MilleniaAudioProcessor::setCurrentProgram (int index)
{
}

const juce::String MilleniaAudioProcessor::getProgramName (int index)
{
    return {};
}

void MilleniaAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
}

//==============================================================================
void MilleniaAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // Use this method as the place to do any pre-playback
    // initialisation that you need..

    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) samplesPerBlock, (juce::uint32) getTotalNumOutputChannels() };
    dattorroTank.prepare (spec);
    dattorroTank.reset();

    monoScratch.setSize (1, samplesPerBlock);
}

void MilleniaAudioProcessor::releaseResources()
{
    // When playback stops, you can use this as an opportunity to free up any
    // spare memory, etc.
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool MilleniaAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    // Some plugin hosts, such as certain GarageBand versions, will only
    // load plugins that support stereo bus layouts.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}
#endif

void MilleniaAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    // In case we have more outputs than inputs, this code clears any output
    // channels that didn't contain input data, (because these aren't
    // guaranteed to be empty - they may contain garbage).
    // This is here to avoid people getting screaming feedback
    // when they first compile a plugin, but obviously you don't need to keep
    // this code if your algorithm always overwrites all the output channels.
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    // Phase 2 committed topology: the Dattorro tank is the sole, permanent
    // reverb path (see docs/shimmer-reverb-implementation-plan.md, Phase 2).
    // This is still 100% wet with no dry/wet mix yet — Phase 5 adds real
    // parameters, including mix.
    const auto numSamples = buffer.getNumSamples();

    // Sum (or pass through) the input to mono in the pre-sized scratch buffer.
    auto* monoData = monoScratch.getWritePointer (0);

    if (totalNumInputChannels <= 1)
    {
        auto* inData = totalNumInputChannels == 1 ? buffer.getReadPointer (0) : nullptr;

        for (int i = 0; i < numSamples; ++i)
            monoData[i] = inData != nullptr ? inData[i] : 0.0f;
    }
    else
    {
        monoScratch.copyFrom (0, 0, buffer, 0, 0, numSamples);

        for (int channel = 1; channel < totalNumInputChannels; ++channel)
            monoScratch.addFrom (0, 0, buffer, channel, 0, numSamples);

        monoScratch.applyGain (0, 0, numSamples, 1.0f / (float) totalNumInputChannels);
    }

    juce::dsp::AudioBlock<float> monoBlock (monoScratch);
    monoBlock = monoBlock.getSubBlock (0, (size_t) numSamples);

    dattorroTank.process (monoBlock);

    for (int channel = 0; channel < totalNumOutputChannels; ++channel)
        buffer.copyFrom (channel, 0, monoScratch, 0, 0, numSamples);
}

//==============================================================================
bool MilleniaAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* MilleniaAudioProcessor::createEditor()
{
    return new MilleniaAudioProcessorEditor (*this);
}

//==============================================================================
void MilleniaAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // You should use this method to store your parameters in the memory block.
    // You could do that either as raw data, or use the XML or ValueTree classes
    // as intermediaries to make it easy to save and load complex data.
}

void MilleniaAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // You should use this method to restore your parameters from this memory block,
    // whose contents will have been created by the getStateInformation() call.
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MilleniaAudioProcessor();
}
