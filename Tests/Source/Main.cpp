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

int main (int, char**)
{
    ConsoleUnitTestRunner runner;
    runner.runAllTests();

    int numFailures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
        numFailures += runner.getResult (i)->failures;

    std::cout << "\n" << (numFailures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED")
               << " (" << numFailures << " failures)" << std::endl;
    return numFailures == 0 ? 0 : 1;
}
