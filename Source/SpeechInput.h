#pragma once
// The Solution - offline speech input for voice control.
// Windows: the built-in SAPI recogniser with a grammar made of the suite's own vocabulary (accurate,
// offline, nothing leaves the PC). Other platforms report "not available yet".
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <functional>
#include <memory>
#include <atomic>

namespace SolutionVoice
{
    class SpeechInput final : private juce::Thread
    {
    public:
        SpeechInput();
        ~SpeechInput() override;
        // Starts listening with this vocabulary. Returns false (and why) if speech isn't available here.
        bool start (const juce::StringArray& vocabulary, juce::String& whyNot);
        void stop();
        void setVocabulary (const juce::StringArray& vocabulary);    // rebuilds the grammar while running
        bool isListening() const { return isThreadRunning(); }
        // Called on the message thread with what was heard and the engine's confidence (0..1).
        std::function<void (const juce::String& phrase, float confidence)> onPhrase;

    private:
        void run() override;
        juce::CriticalSection lock;
        juce::StringArray words;
        std::atomic<bool> vocabChanged { false };
        juce::String startError;
        juce::WaitableEvent started;
        std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);   // guards late callbacks
    };
}
