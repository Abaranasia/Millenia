#include <JuceHeader.h>
#include <iostream>

class ConsoleUnitTestRunner : public juce::UnitTestRunner
{
public:
    void logMessage (const juce::String& message) override
    {
        std::cout << message << std::endl;
    }
};

int main (int argc, char** argv)
{
    ConsoleUnitTestRunner runner;

    // Optional argv[1]: run only juce::UnitTest classes whose NAME (the
    // first constructor argument, e.g. "PitchShifterTests") contains this
    // substring (case-insensitive) -- every test class in this project shares
    // the same "DSP" category, so category filtering alone can't isolate one
    // class. Lets a targeted diagnostic run in seconds instead of minutes,
    // without commenting out other test classes by hand.
    if (argc > 1)
    {
        juce::Array<juce::UnitTest*> matching;
        const juce::String filter (argv[1]);

        for (auto* test : juce::UnitTest::getAllTests())
            if (test->getName().containsIgnoreCase (filter))
                matching.add (test);

        if (matching.isEmpty())
        {
            std::cout << "No test class name contains \"" << argv[1] << "\"" << std::endl;
            return 1;
        }

        runner.runTests (matching);
    }
    else
    {
        runner.runAllTests();
    }

    int numFailures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
        numFailures += runner.getResult (i)->failures;

    std::cout << "\n" << (numFailures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED")
               << " (" << numFailures << " failures)" << std::endl;
    return numFailures == 0 ? 0 : 1;
}
