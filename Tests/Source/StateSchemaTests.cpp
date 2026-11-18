#include <JuceHeader.h>
#include "../../Source/Parameters.h"
#include "../../Source/StateSchema.h"

// Audit fix 5 (docs/project-audit-2026-11.md): "honest schema" state loading.
// No value migration exists by decision (nothing has shipped); the only
// guarantee is that a state from a NEWER schema is rejected, and that older /
// unversioned / legacy-layout states load without disturbing new parameters.
namespace
{
    class DummyProcessor : public juce::AudioProcessor
    {
    public:
        DummyProcessor() : AudioProcessor (BusesProperties()
                                               .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                               .withOutput ("Output", juce::AudioChannelSet::stereo(), true)) {}

        const juce::String getName() const override { return "Dummy"; }
        void prepareToPlay (double, int) override {}
        void releaseResources() override {}
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return {}; }
        void changeProgramName (int, const juce::String&) override {}
        void getStateInformation (juce::MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}
    };

    float readParam (juce::AudioProcessorValueTreeState& apvts, const char* id)
    {
        return apvts.getRawParameterValue (id)->load();
    }

    // Builds a state tree as getStateInformation() would, but with the given
    // PARAM entries (id, value) and an optional schemaVersion property.
    juce::ValueTree makeState (std::initializer_list<std::pair<const char*, float>> params,
                               const int* schemaVersion)
    {
        juce::ValueTree state ("PARAMETERS");
        if (schemaVersion != nullptr)
            state.setProperty (StateSchema::versionProperty, *schemaVersion, nullptr);

        for (auto& p : params)
        {
            juce::ValueTree param ("PARAM");
            param.setProperty ("id", p.first, nullptr);
            param.setProperty ("value", p.second, nullptr);
            state.appendChild (param, nullptr);
        }
        return state;
    }
}

class StateSchemaTests : public juce::UnitTest
{
public:
    StateSchemaTests() : juce::UnitTest ("StateSchemaTests", "DSP") {}

    void runTest() override
    {
        beginTest ("State from a newer schema version is rejected, params unchanged");
        {
            DummyProcessor proc;
            juce::AudioProcessorValueTreeState apvts (proc, nullptr, "PARAMETERS", createParameterLayout());
            const float mixBefore = readParam (apvts, ParamIDs::mix);

            const int newer = StateSchema::currentVersion + 1;
            auto state = makeState ({ { ParamIDs::mix, 0.123f } }, &newer);

            expect (! StateSchema::applyState (apvts, state));
            expectEquals (readParam (apvts, ParamIDs::mix), mixBefore);
        }

        beginTest ("Legacy-layout state with removed shimmerSustain entry loads; shimmerFeedback keeps default");
        {
            DummyProcessor proc;
            juce::AudioProcessorValueTreeState apvts (proc, nullptr, "PARAMETERS", createParameterLayout());
            const float feedbackDefault = readParam (apvts, ParamIDs::shimmerFeedback);
            expectWithinAbsoluteError (feedbackDefault, 0.55f, 1.0e-6f);

            const int v = StateSchema::currentVersion;
            auto state = makeState ({ { "shimmerSustain", 0.9f },
                                      { ParamIDs::mix, 0.25f } }, &v);

            expect (StateSchema::applyState (apvts, state));
            expectWithinAbsoluteError (readParam (apvts, ParamIDs::mix), 0.25f, 1.0e-6f);
            expectWithinAbsoluteError (readParam (apvts, ParamIDs::shimmerFeedback), 0.55f, 1.0e-6f);
        }

        beginTest ("State with no schemaVersion property still loads");
        {
            DummyProcessor proc;
            juce::AudioProcessorValueTreeState apvts (proc, nullptr, "PARAMETERS", createParameterLayout());

            auto state = makeState ({ { ParamIDs::mix, 0.4f } }, nullptr);

            expect (StateSchema::applyState (apvts, state));
            expectWithinAbsoluteError (readParam (apvts, ParamIDs::mix), 0.4f, 1.0e-6f);
        }

        beginTest ("Foreign root tag is still rejected");
        {
            DummyProcessor proc;
            juce::AudioProcessorValueTreeState apvts (proc, nullptr, "PARAMETERS", createParameterLayout());
            juce::ValueTree foreign ("SOMETHING_ELSE");
            expect (! StateSchema::applyState (apvts, foreign));
        }
    }
};

static StateSchemaTests stateSchemaTests;
