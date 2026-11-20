#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "ModuleEngines.h"
#include <atomic>
#include <array>

class SolutionAudioProcessor final : public juce::AudioProcessor
{
public:
    SolutionAudioProcessor();
    ~SolutionAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.05; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    void triggerRoomSweep (float seconds = 2.0f) { moduleEngine.getRoomMatcher().triggerSweepCapture (seconds); }
    bool isRoomCapturing() const { return moduleEngine.getRoomMatcher().isCapturing(); }
    float getCapturedModeHz() const { return moduleEngine.getRoomMatcher().getCapturedModeHz(); }
    int getRackLinkCount() const { return rackLinkBus->activeInstances.load (std::memory_order_relaxed); }
    float getCvLfo() const { return rackLinkBus->cvLfoValue.load (std::memory_order_relaxed); }
    float getEnvFollower() const { return rackLinkBus->envFollower.load (std::memory_order_relaxed); }

    void armMidiLearn (const juce::String& paramId);
    int getLastLearnedCc() const { return lastLearnedCc.load (std::memory_order_relaxed); }

    juce::UndoManager undoManager;
    juce::AudioProcessorValueTreeState apvts;

    std::atomic<float> rmsLeft { 0.0f };
    std::atomic<float> rmsRight { 0.0f };
    std::atomic<float> phaseCorrelation { 1.0f };
    std::atomic<float> peakDb { -60.0f };
    std::atomic<float> lufsMomentary { -60.0f };
    std::atomic<int>   lastLearnedCc { -1 };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    SolutionDSP::SuiteModuleEngine moduleEngine;
    juce::SharedResourcePointer<SolutionDSP::RackLinkBus> rackLinkBus;
    juce::AudioBuffer<float> dryCopy;

    std::atomic<int> learnArmedIndex { -1 };
    std::array<std::atomic<int>, 128> ccToParamIndex;
    std::vector<juce::String> learnableParamIds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SolutionAudioProcessor)
};
