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

            // 0.7 is deliberately not the Mix default, so a no-op applyState cannot pass.
            auto state = makeState ({ { ParamIDs::mix, 0.7f } }, nullptr);

            expect (StateSchema::applyState (apvts, state));
            expectWithinAbsoluteError (readParam (apvts, ParamIDs::mix), 0.7f, 1.0e-6f);
        }

        beginTest ("Mix parameter defaults to 0.4 (typical insert), a fresh APVTS reads it, and a state without mix resets it to that default");
        {
            DummyProcessor proc;
            juce::AudioProcessorValueTreeState apvts (proc, nullptr, "PARAMETERS", createParameterLayout());
            auto* param = apvts.getParameter (ParamIDs::mix);
            expect (param != nullptr);
            if (param == nullptr)
                return;

            expectWithinAbsoluteError (param->convertFrom0to1 (param->getDefaultValue()), 0.4f, 1.0e-6f);
            expectWithinAbsoluteError (readParam (apvts, ParamIDs::mix), 0.4f, 1.0e-6f);

            // APVTS::replaceState resets a parameter missing from the loaded state to its
            // default. Move mix off the default first so this fails if that ever stops
            // being true, i.e. an older save without mix would no longer land on 0.4.
            param->setValueNotifyingHost (param->convertTo0to1 (0.9f));
            expectWithinAbsoluteError (readParam (apvts, ParamIDs::mix), 0.9f, 1.0e-6f);

            const int v = StateSchema::currentVersion;
            expect (StateSchema::applyState (apvts, makeState ({ { ParamIDs::air, 1.0f } }, &v)));
            expectWithinAbsoluteError (readParam (apvts, ParamIDs::mix), 0.4f, 1.0e-6f);
        }

        beginTest ("Foreign root tag is still rejected");
        {
            DummyProcessor proc;
            juce::AudioProcessorValueTreeState apvts (proc, nullptr, "PARAMETERS", createParameterLayout());
            juce::ValueTree foreign ("SOMETHING_ELSE");
            expect (! StateSchema::applyState (apvts, foreign));
        }

        beginTest ("Damping knob: 0..0.7 coefficient range, centre maps to ~0.5, default stays 0.0005");
        {
            // The parameter stores the one-pole coefficient directly. The
            // range tops out at 0.7 (-3 dB at ~2.5 kHz at 44.1 kHz, 16 kHz
            // tail RT60 ~1.5 s) so the whole travel is audible, and the
            // centre sits at a = 0.5 (~half the maximum high-frequency loss
            // per loop pass, in dB).
            DummyProcessor proc;
            juce::AudioProcessorValueTreeState apvts (proc, nullptr, "PARAMETERS", createParameterLayout());
            auto* param = apvts.getParameter (ParamIDs::damping);
            expect (param != nullptr);
            if (param == nullptr)
                return;

            const auto& range = param->getNormalisableRange();
            expectWithinAbsoluteError (range.start, 0.0f, 0.0f);
            expectWithinAbsoluteError (range.end, 0.7f, 0.0f);
            expectWithinAbsoluteError (param->convertFrom0to1 (0.0f), 0.0f, 0.0f);
            expectWithinAbsoluteError (param->convertFrom0to1 (1.0f), 0.7f, 1.0e-6f);
            expectWithinAbsoluteError (param->convertFrom0to1 (param->getDefaultValue()), 0.0005f, 1.0e-6f);

            const float centre = param->convertFrom0to1 (0.5f);
            expect (centre >= 0.48f && centre <= 0.52f,
                    "Damping knob centre maps to " + juce::String (centre) + ", expected 0.48..0.52");

            for (const float v : { 0.0f, 0.0005f, 0.01f, 0.08f, 0.2f, 0.5f, 0.7f })
                expectWithinAbsoluteError (param->convertFrom0to1 (param->convertTo0to1 (v)), v, 1.0e-6f);
        }

        beginTest ("Air parameter: 0..+6 dB range, 0 dB default, state round-trips, older state keeps the default");
        {
            DummyProcessor proc;
            juce::AudioProcessorValueTreeState apvts (proc, nullptr, "PARAMETERS", createParameterLayout());
            auto* param = apvts.getParameter (ParamIDs::air);
            expect (param != nullptr);
            if (param == nullptr)
                return;

            const auto& range = param->getNormalisableRange();
            expectWithinAbsoluteError (range.start, 0.0f, 0.0f);
            expectWithinAbsoluteError (range.end, 6.0f, 0.0f);
            expectWithinAbsoluteError (param->convertFrom0to1 (param->getDefaultValue()), 0.0f, 0.0f);
            expectWithinAbsoluteError (readParam (apvts, ParamIDs::air), 0.0f, 0.0f);
            expectEquals (param->getName (32), juce::String ("Air"));

            // Round trip through a saved tree.
            const int v = StateSchema::currentVersion;
            expect (StateSchema::applyState (apvts, makeState ({ { ParamIDs::air, 4.5f } }, &v)));
            expectWithinAbsoluteError (readParam (apvts, ParamIDs::air), 4.5f, 1.0e-6f);

            auto saved = apvts.copyState();
            DummyProcessor proc2;
            juce::AudioProcessorValueTreeState restored (proc2, nullptr, "PARAMETERS", createParameterLayout());
            expect (StateSchema::applyState (restored, saved));
            expectWithinAbsoluteError (readParam (restored, ParamIDs::air), 4.5f, 1.0e-6f);

            // A state saved before Air existed leaves it at 0 dB.
            DummyProcessor proc3;
            juce::AudioProcessorValueTreeState legacy (proc3, nullptr, "PARAMETERS", createParameterLayout());
            expect (StateSchema::applyState (legacy, makeState ({ { ParamIDs::mix, 0.3f } }, &v)));
            expectWithinAbsoluteError (readParam (legacy, ParamIDs::air), 0.0f, 0.0f);
        }
    }
};

static StateSchemaTests stateSchemaTests;
