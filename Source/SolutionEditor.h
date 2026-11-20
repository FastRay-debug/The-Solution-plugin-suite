#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "SolutionProcessor.h"

class SolutionAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                           private juce::Timer
{
public:
    explicit SolutionAudioProcessorEditor (SolutionAudioProcessor&);
    ~SolutionAudioProcessorEditor() override = default;

    void resized() override;

private:
    void timerCallback() override;
    void sendParamState();
    void sendPresetList (const juce::String& message = {});
    void handlePresetCommand (const juce::var& data);
    void sendRackState();
    void sendRoomResult();
    int frameCounter = 0;
    static juce::File findSharedSkinsFolder();

    SolutionAudioProcessor& audioProcessor;
    const juce::File sharedSkinsDir;   // must be declared before webView (used to build it)
    const bool skinsAvailable;
    juce::WebBrowserComponent webView;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SolutionAudioProcessorEditor)
};
