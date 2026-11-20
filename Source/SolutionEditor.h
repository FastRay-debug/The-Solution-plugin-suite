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

    SolutionAudioProcessor& audioProcessor;
    juce::WebBrowserComponent webView;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SolutionAudioProcessorEditor)
};
