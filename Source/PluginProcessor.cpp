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
    decayParam      = apvts.getRawParameterValue (ParamIDs::decay);
    shimmerAmountParam = apvts.getRawParameterValue (ParamIDs::shimmerAmount);
    shimmerFeedbackParam = apvts.getRawParameterValue (ParamIDs::shimmerFeedback);
    dampingParam    = apvts.getRawParameterValue (ParamIDs::damping);
    widthParam      = apvts.getRawParameterValue (ParamIDs::width);
    mixParam        = apvts.getRawParameterValue (ParamIDs::mix);
    bypassParam     = apvts.getRawParameterValue (ParamIDs::bypass);
    freezeParam     = apvts.getRawParameterValue (ParamIDs::freeze);
    infiniteParam   = apvts.getRawParameterValue (ParamIDs::infinite);
    formantCorrectionParam = apvts.getRawParameterValue (ParamIDs::formantCorrection);
    loopMixParam = apvts.getRawParameterValue (ParamIDs::loopMix);
    loopLengthParam = apvts.getRawParameterValue (ParamIDs::loopLength);
    preDelayParam   = apvts.getRawParameterValue (ParamIDs::preDelay);
    lowCutParam     = apvts.getRawParameterValue (ParamIDs::lowCut);
    highCutParam    = apvts.getRawParameterValue (ParamIDs::highCut);
    airParam        = apvts.getRawParameterValue (ParamIDs::air);
    shimmerToneParam = apvts.getRawParameterValue (ParamIDs::shimmerTone);
    sizeParam       = apvts.getRawParameterValue (ParamIDs::size);
    modulationParam = apvts.getRawParameterValue (ParamIDs::modulation);
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
    // Host thread, not the audio thread: only reads the cached atomics.
    // "Sustained" (see ShimmerReverbEngine::isTailSustained()) means the
    // audible output does not decay on its own, so the host must not cut the
    // tail. Bypass and Mix 0 mute the wet path, so they cancel it. When not
    // sustained, the finite estimate is returned; it is finite and also
    // covers the 50 ms fade of the wet signal on a bypass or Mix change.
    const bool sustained = ShimmerReverbEngine::isTailSustained (bypassParam->load() > 0.5f,
                                                                 mixParam->load(),
                                                                 infiniteParam->load(),
                                                                 freezeParam->load(),
                                                                 loopMixParam->load());

    return ShimmerReverbEngine::computeTailSeconds (decayParam->load(), sizeParam->load(),
                                                    preDelayParam->load(), sustained);
}

juce::AudioProcessorParameter* MilleniaAudioProcessor::getBypassParameter() const
{
    // With a non-null bypass parameter the VST3 wrapper never calls
    // processBlockBypassed(): it calls processBlock() and only exposes the
    // parameter to the host with the kIsBypass flag
    // (juce_audio_plugin_client_VST3.cpp:3728-3733, :920-921). JUCE's own
    // header says the same (juce_AudioProcessor.h:947-957), and the other
    // wrappers follow it (AU, AAX, VST2, LV2, Unity). processBlock() already
    // reads this parameter and drives the mix smoother toward 0, so bypass is a
    // click-free mix-to-dry with the tank still running and the tail ringing
    // out. The dry path is undelayed, so no latency compensation is needed.
    // The default processBlockBypassed() would hard-cut and freeze the tank,
    // which is why it is not relied on and, being never called, not overridden.
    return apvts.getParameter (ParamIDs::bypass);
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

    // Seeded before prepare() so the engine's own per-sample smoothers
    // (pre-delay, Size, Modulation) start at the host's current values
    // instead of gliding in from their defaults. Decay too, so the tank's
    // g is right from the first sample.
    shimmerReverbEngine.setPreDelayMs (preDelayParam->load());
    shimmerReverbEngine.setSize (sizeParam->load());
    shimmerReverbEngine.setModulation (modulationParam->load());
    shimmerReverbEngine.setDecaySeconds (decayParam->load());
    // Formant Correction too, so its crossfade starts settled at the host's
    // value (prepare()/reset() snap it) instead of ramping in.
    shimmerReverbEngine.setFormantCorrection (formantCorrectionParam->load() > 0.5f);
    // Shimmer Tone too, so the first block already filters at the host's
    // value (the engine keeps it across prepare()/reset()).
    shimmerReverbEngine.setShimmerTone (shimmerToneParam->load());
    shimmerReverbEngine.prepare (spec);
    shimmerReverbEngine.reset();

    // Phase 5: 50ms ramp -- a standard default for click-free parameter
    // smoothing, not arbitrarily huge or tiny -- then seed each smoother
    // with the host's actual current parameter value so playback doesn't
    // start with an audible ramp-in from some default toward wherever the
    // host actually has each parameter set.
    // (EngineParameterSmoothing::prepare() below does the reset + seeding;
    // the Freeze notes that follow describe its freeze smoother.)

    // 2026-08-29: tried giving Freeze its own longer ramp (0.3s, then 1.0s)
    // after a by-ear report that the freezeQuickToggle checkbox produced "a
    // quick initial highly-pitched noise... like a DJ scratch effect" that
    // manual dial drags didn't. REVERTED -- both attempts made it WORSE, not
    // better ("even worse because... it takes longer"): the problem isn't
    // sweep SPEED at all, it's that the pitch-ratio glide itself is
    // unwanted for a toggle -- the user wants a clean swap between "live"
    // and "frozen" output, not an audible pitch-bend transition of any
    // duration, so stretching the transition out just prolongs an
    // unwanted characteristic. FIXED 2026-10-10 in the engine, not here:
    // Freeze is now a LEVEL crossfade from the main shifter to a 0-semitone
    // unityShifter (see ShimmerReverbEngine::
    // applyPitchAndFreezeCrossfade()), and the
    // engine ramps freeze per sample inside each chunk, so the 0.05s smoother
    // (stepped in 32-sample chunks) no longer steps audibly. Engram topic_key
    // millenia/freeze-toggle-pitch-artifact has the full investigation.
    // Every smoother (Freeze included) is reset to the 50 ms ramp and seeded
    // with the host's current values. Bypass folds into the mix seed too --
    // see processBlock()'s comment for why bypass drives the mix smoother
    // rather than calling ShimmerReverbEngine::setBypassed().
    engineSmoothing.prepare (sampleRate, readSmoothingTargets());
}

EngineParameterSmoothing::Targets MilleniaAudioProcessor::readSmoothingTargets() const noexcept
{
    EngineParameterSmoothing::Targets targets;
    targets.pitchShift      = pitchShiftParam->load();
    targets.decay           = decayParam->load();
    targets.shimmerAmount   = shimmerAmountParam->load();
    targets.shimmerFeedback = shimmerFeedbackParam->load();
    targets.damping         = dampingParam->load();
    targets.width           = widthParam->load();
    targets.lowCut          = lowCutParam->load();
    targets.highCut         = highCutParam->load();
    targets.air             = airParam->load();
    targets.shimmerTone     = shimmerToneParam->load();
    targets.freeze          = freezeParam->load();

    // Bypass drives the mix smoother toward 0 (see processBlock()).
    const bool isBypassed = bypassParam->load() > 0.5f;
    targets.mix = isBypassed ? 0.0f : mixParam->load();
    return targets;
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
    // through the EngineParameterSmoothing smoothers for click-free automation. All
    // mono-summing/processing/write-back still happens inside
    // ShimmerReverbEngine::process(); this method just drives its
    // forwarding setters and forwards the block.
    // Smoothed parameters (everything except the raw ones below) are pushed
    // by EngineParameterSmoothing::process() at the bottom: it advances its 11
    // smoothers in 32-sample chunks inside the host block and calls a setter
    // only when its value changed (a full push is forced after prepare).
    // Audit fix 6: they used to be skip()'d once per host block, so the
    // zipper step scaled with the host block size.
    //
    // Bypass integration: bypass does NOT call
    // ShimmerReverbEngine::setBypassed() at all. That method forces its own
    // effective-mix override instantly with no smoothing (deliberately left
    // for PluginProcessor to smooth, per ShimmerReverbEngine.h's
    // setBypassed() comment) -- if this smoothed mix via EngineParameterSmoothing AND
    // also called engine.setBypassed(), the instant override would fight the
    // smoothed value and reintroduce exactly the click this phase exists to
    // eliminate. Instead, bypass drives the SAME mix smoother toward 0,
    // giving a properly smoothed transition in both directions (engaging
    // and disengaging bypass) with no new smoothing mechanism needed.
    // ShimmerReverbEngine::setBypassed() stays valid public API for other
    // callers/tests; PluginProcessor deliberately never calls it.
    // (readSmoothingTargets() folds bypass into the mix target.)
    const auto smoothingTargets = readSmoothingTargets();

    // Raw parameters, pushed once per host block. Each is ramped per sample
    // inside the engine/tank/loop capture, and their setters are plain
    // target stores (a repeated identical value restarts nothing), so they
    // need no chunking.
    shimmerReverbEngine.setPreDelayMs (preDelayParam->load());

    // Slice 2: raw, the tank glides Size (rate-limited, log domain) and ramps Modulation (50 ms) per
    // sample -- see sizeParam's header comment.
    shimmerReverbEngine.setSize (sizeParam->load());
    shimmerReverbEngine.setModulation (modulationParam->load());

    // Freeze is not pushed here: it is one of the smoothed targets (see
    // readSmoothingTargets()), stepped in 32-sample chunks by
    // EngineParameterSmoothing like the other continuous values -- a hard
    // 0->1 jump on decayGain/dry-mute inside the tank would click.

    // Infinite mode: read RAW, no smoother here -- DattorroTank ramps the
    // 0/1 target itself over infiniteRampSeconds, sample-accurately, same
    // reasoning as Loop Mix below.
    shimmerReverbEngine.setInfiniteAmount (infiniteParam->load() > 0.5f ? 1.0f : 0.0f);

    // Formant Correction: read RAW, no smoother here -- the engine crossfades
    // raw <-> corrected per sample over 20 ms (formantRampSeconds), same
    // reasoning as Infinite above.
    shimmerReverbEngine.setFormantCorrection (formantCorrectionParam->load() > 0.5f);

    // Phase 10 Loop Mix: read RAW, no smoother here -- the engage/disengage
    // ramp lives INSIDE LoopCapture itself (2026-09-06 fix, see
    // LoopCapture::setLoopFreezeAmount()'s comment), sample-accurate
    // regardless of host buffer size. Loop Mix is itself now a continuously-
    // dragged dial (retyped 2026-09-26 from a discrete on/off toggle), which
    // makes the case for reading it raw here even clearer than before -- a
    // second, coarser block-granularity smoother on top of LoopCapture's own
    // sample-accurate ramp would be redundant at best. loopLengthParam is
    // read RAW too, for the unrelated "only matters at a discrete instant"
    // reason -- LoopCapture itself only ever applies a new length at its own
    // next rising-edge capture (see LoopCapture::setLoopLengthMs()'s comment), so
    // smoothing this value would add nothing but latency to when a length
    // change is picked up.
    shimmerReverbEngine.setLoopFreezeAmount (loopMixParam->load());
    shimmerReverbEngine.setLoopLengthMs (loopLengthParam->load());

    // Advances the smoothers and renders the block through the engine in
    // 32-sample chunks (the engine itself processes each chunk with constant
    // parameters).
    juce::dsp::AudioBlock<float> block (buffer);
    engineSmoothing.process (shimmerReverbEngine, smoothingTargets, block);
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
    // StateSchema::applyState() rejects a foreign root tag (checked against
    // apvts.state.getType(), the "PARAMETERS" identifier passed to the apvts
    // constructor) and any state stamped with a NEWER schemaVersion than this
    // build knows, leaving current state untouched. Older/unversioned states
    // load as-is (no value migration, by decision).
    if (auto xmlState = getXmlFromBinary (data, sizeInBytes))
        StateSchema::applyState (apvts, juce::ValueTree::fromXml (*xmlState));
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MilleniaAudioProcessor();
}
