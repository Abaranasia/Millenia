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
                       ),
#else
     :
#endif
       apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    // Phase 5: cache raw parameter pointers once, after apvts exists --
    // never call apvts.getRawParameterValue() (a string lookup) anywhere
    // inside processBlock(). bypassParam's underlying value is still a
    // float (0.0/1.0) even though it's an AudioParameterBool.
    pitchShiftParam = apvts.getRawParameterValue (ParamIDs::pitchShift);
    feedbackParam   = apvts.getRawParameterValue (ParamIDs::feedback);
    shimmerAmountParam = apvts.getRawParameterValue (ParamIDs::shimmerAmount);
    shimmerSustainParam = apvts.getRawParameterValue (ParamIDs::shimmerSustain);
    dampingParam    = apvts.getRawParameterValue (ParamIDs::damping);
    widthParam      = apvts.getRawParameterValue (ParamIDs::width);
    mixParam        = apvts.getRawParameterValue (ParamIDs::mix);
    bypassParam     = apvts.getRawParameterValue (ParamIDs::bypass);
    freezeParam     = apvts.getRawParameterValue (ParamIDs::freeze);
    loopFreezeParam = apvts.getRawParameterValue (ParamIDs::loopFreeze);
    loopLengthParam = apvts.getRawParameterValue (ParamIDs::loopLength);
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
    shimmerReverbEngine.prepare (spec);
    shimmerReverbEngine.reset();

    // Phase 5: 50ms ramp -- a standard default for click-free parameter
    // smoothing, not arbitrarily huge or tiny -- then seed each smoother
    // with the host's actual current parameter value so playback doesn't
    // start with an audible ramp-in from some default toward wherever the
    // host actually has each parameter set.
    smoothedPitchShift.reset (sampleRate, 0.05);
    smoothedFeedback.reset (sampleRate, 0.05);
    smoothedShimmerAmount.reset (sampleRate, 0.05);
    smoothedShimmerSustain.reset (sampleRate, 0.05);
    smoothedDamping.reset (sampleRate, 0.05);
    smoothedWidth.reset (sampleRate, 0.05);
    smoothedMix.reset (sampleRate, 0.05);

    // 2026-08-29: tried giving Freeze its own longer ramp (0.3s, then 1.0s)
    // after a by-ear report that the freezeQuickToggle checkbox produced "a
    // quick initial highly-pitched noise... like a DJ scratch effect" that
    // manual dial drags didn't. REVERTED -- both attempts made it WORSE, not
    // better ("even worse because... it takes longer"): the problem isn't
    // sweep SPEED at all, it's that the pitch-ratio glide itself is
    // unwanted for a toggle -- the user wants a clean swap between "live"
    // and "frozen" output, not an audible pitch-bend transition of any
    // duration, so stretching the transition out just prolongs an
    // unwanted characteristic. See docs/shimmer-reverb-implementation-
    // plan.md's Phase 10 section for the real fix this points toward (a
    // buffer capture-and-loop redesign that crossfades OUTPUT, not the
    // shifter's own pitch ratio) and Engram topic_key
    // millenia/freeze-toggle-pitch-artifact for the full investigation.
    smoothedFreeze.reset (sampleRate, 0.05);

    smoothedPitchShift.setCurrentAndTargetValue (pitchShiftParam->load());
    smoothedFeedback.setCurrentAndTargetValue (feedbackParam->load());
    smoothedShimmerAmount.setCurrentAndTargetValue (shimmerAmountParam->load());
    smoothedShimmerSustain.setCurrentAndTargetValue (shimmerSustainParam->load());
    smoothedDamping.setCurrentAndTargetValue (dampingParam->load());
    smoothedWidth.setCurrentAndTargetValue (widthParam->load());
    smoothedFreeze.setCurrentAndTargetValue (freezeParam->load());

    // Bypass folds into the mix smoother's seed value too -- see
    // processBlock()'s comment for why bypass drives smoothedMix rather than
    // calling ShimmerReverbEngine::setBypassed().
    {
        const bool isBypassed = bypassParam->load() > 0.5f;
        smoothedMix.setCurrentAndTargetValue (isBypassed ? 0.0f : mixParam->load());
    }
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

    // Phase 3 committed topology: ShimmerReverbEngine (Dattorro tank +
    // pitch shifter feedback loop) is the sole, permanent reverb path (see
    // docs/shimmer-reverb-implementation-plan.md, Phase 3). Phase 5 closes
    // the "100% wet, no dry/wet mix" gap this comment used to describe:
    // every DSP-affecting value is now driven by an APVTS parameter, read
    // here via the cached atomics below (never apvts.getRawParameterValue()
    // by string -- that's a needless lookup on the audio thread) and pushed
    // through per-block SmoothedValues for click-free automation. All
    // mono-summing/processing/write-back still happens inside
    // ShimmerReverbEngine::process(); this method just drives its
    // forwarding setters and forwards the block.
    const auto numSamples = buffer.getNumSamples();

    smoothedPitchShift.setTargetValue (pitchShiftParam->load());
    smoothedFeedback.setTargetValue (feedbackParam->load());
    smoothedShimmerAmount.setTargetValue (shimmerAmountParam->load());
    smoothedShimmerSustain.setTargetValue (shimmerSustainParam->load());
    smoothedDamping.setTargetValue (dampingParam->load());
    smoothedWidth.setTargetValue (widthParam->load());
    smoothedFreeze.setTargetValue (freezeParam->load());

    // Bypass integration: bypass does NOT call
    // ShimmerReverbEngine::setBypassed() at all. That method forces its own
    // effective-mix override instantly with no smoothing (deliberately left
    // for PluginProcessor to smooth, per ShimmerReverbEngine.h's
    // setBypassed() comment) -- if this smoothed mix via smoothedMix AND
    // also called engine.setBypassed(), the instant override would fight the
    // smoothed value and reintroduce exactly the click this phase exists to
    // eliminate. Instead, bypass drives the SAME mix smoother toward 0,
    // giving a properly smoothed transition in both directions (engaging
    // and disengaging bypass) with no new smoothing mechanism needed.
    // ShimmerReverbEngine::setBypassed() stays valid public API for other
    // callers/tests; PluginProcessor deliberately never calls it.
    const bool isBypassed = bypassParam->load() > 0.5f;
    const float mixTarget = isBypassed ? 0.0f : mixParam->load();
    smoothedMix.setTargetValue (mixTarget);

    shimmerReverbEngine.setPitchShiftSemitones (smoothedPitchShift.skip ((int) numSamples));
    shimmerReverbEngine.setFeedback (smoothedFeedback.skip ((int) numSamples));
    shimmerReverbEngine.setShimmerAmount (smoothedShimmerAmount.skip ((int) numSamples));
    shimmerReverbEngine.setShimmerSustain (smoothedShimmerSustain.skip ((int) numSamples));
    shimmerReverbEngine.setDamping (smoothedDamping.skip ((int) numSamples));
    shimmerReverbEngine.setWidth (smoothedWidth.skip ((int) numSamples));
    shimmerReverbEngine.setMix (smoothedMix.skip ((int) numSamples));

    // Phase 9: Freeze gets the exact same block-granularity smoothing
    // treatment as every other continuous parameter above -- originally a
    // bool at the APVTS level (now a continuous float dial, 2026-08-22, see
    // Parameters.cpp), but even a hard 0->1 jump on decayGain/dry-mute
    // inside the tank would click, same reasoning as bypass driving
    // smoothedMix instead of a hard switch (see the comment above).
    shimmerReverbEngine.setFreezeAmount (smoothedFreeze.skip ((int) numSamples));

    // Phase 10 Loop Freeze: read RAW, no smoother here -- moved into
    // LoopCapture itself (2026-09-06 fix, see
    // LoopCapture::setLoopFreezeAmount()'s comment) so the engage/disengage
    // ramp is genuinely sample-accurate regardless of host buffer size,
    // instead of the block-granularity smoothing this used to do (which
    // could click on a large-enough buffer). loopLengthParam is read RAW
    // too, for the unrelated "only matters at a discrete instant" reason --
    // LoopCapture itself only ever applies a new length at its own next
    // rising-edge capture (see LoopCapture::setLoopLengthMs()'s comment), so
    // smoothing this value would add nothing but latency to when a length
    // change is picked up.
    shimmerReverbEngine.setLoopFreezeAmount (loopFreezeParam->load());
    shimmerReverbEngine.setLoopLengthMs (loopLengthParam->load());

    juce::dsp::AudioBlock<float> block (buffer);
    shimmerReverbEngine.process (block);
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
    // Phase 5: persist the APVTS ValueTree (not ad-hoc member variables),
    // stamped with the current schema version for future preset
    // compatibility -- see currentSchemaVersion's declaration for what a
    // future schema bump needs to do here.
    auto state = apvts.copyState();
    state.setProperty ("schemaVersion", currentSchemaVersion, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void MilleniaAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // hasType() is checked against apvts.state.getType(), i.e. the exact
    // "PARAMETERS" identifier passed to the apvts constructor above -- a
    // corrupt/foreign state blob (wrong root tag) is rejected rather than
    // blindly loaded via replaceState().
    if (auto xmlState = getXmlFromBinary (data, sizeInBytes))
    {
        auto state = juce::ValueTree::fromXml (*xmlState);
        if (state.isValid() && state.hasType (apvts.state.getType()))
            apvts.replaceState (state);
    }
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MilleniaAudioProcessor();
}
