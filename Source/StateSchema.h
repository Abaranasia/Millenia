#pragma once

#include <JuceHeader.h>

// Saved-state schema gate, kept free of any processor dependency so it can be
// unit-tested (Tests/Source/StateSchemaTests.cpp) with a bare APVTS.
namespace StateSchema
{
    // Schema v1 -- the first version ever. Bump this only together with an
    // explicit migration step in applyState() (before replaceState()) that
    // reads the OLD layout.
    constexpr int currentVersion = 1;

    inline const juce::Identifier versionProperty { "schemaVersion" };

    // True when `state` may be loaded into a tree whose root type is
    // `expectedRootType`: right root tag, and not written by a newer schema
    // than this build knows. A missing schemaVersion (or an older/equal one)
    // is accepted: unknown/removed param IDs are ignored by APVTS and new
    // params keep their defaults. There is deliberately no value migration.
    inline bool isLoadable (const juce::ValueTree& state, const juce::Identifier& expectedRootType)
    {
        if (! state.isValid() || ! state.hasType (expectedRootType))
            return false;

        return static_cast<int> (state.getProperty (versionProperty, 0)) <= currentVersion;
    }

    // Loads `state` into `apvts` if loadable; returns whether it was applied.
    // On rejection the current parameter state is left untouched.
    inline bool applyState (juce::AudioProcessorValueTreeState& apvts, const juce::ValueTree& state)
    {
        if (! isLoadable (state, apvts.state.getType()))
            return false;

        apvts.replaceState (state);
        return true;
    }
}
