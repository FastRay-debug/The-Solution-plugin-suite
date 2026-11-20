#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#ifndef SOLUTION_HEADLESS
 #define SOLUTION_HEADLESS 0
#endif
#include <juce_dsp/juce_dsp.h>
#include "ModuleEngines.h"
#include "PresetManager.h"
#include <atomic>
#include <array>

class SolutionAudioProcessor final : public juce::AudioProcessor,
                                     private juce::AudioProcessorValueTreeState::Listener
{
public:
    SolutionAudioProcessor();
    ~SolutionAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return ! SOLUTION_HEADLESS; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.05; }

    // Factory presets double as the DAW's program list
    int getNumPrograms() override { return std::max (1, presets.getNumFactoryPresets()); }
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override { return presets.getFactoryPresetName (index); }
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
    PresetManager presets { apvts };

    // Saved-state format version. Bump it and extend migrateState() whenever a
    // parameter is renamed, removed or changes range, so old sessions still load.
    static constexpr int stateVersion = 1;

    // UI-only settings (e.g. chosen skin) saved with the session. Message thread only.
    juce::String getUiProperty (const juce::String& key) const { return apvts.state.getProperty ("ui_" + key).toString(); }
    void setUiProperty (const juce::String& key, const juce::String& value) { apvts.state.setProperty ("ui_" + key, value, nullptr); }

    // True when any parameter changed since the last call (host automation, preset, undo...).
    bool consumeParamsChanged() { return paramsChanged.exchange (false, std::memory_order_acq_rel); }
    void markParamsChanged() { paramsChanged.store (true, std::memory_order_release); }

    std::atomic<float> rmsLeft { 0.0f };
    std::atomic<float> rmsRight { 0.0f };
    std::atomic<float> phaseCorrelation { 1.0f };
    std::atomic<float> peakDb { -60.0f };
    std::atomic<float> lufsMomentary { -60.0f };
    std::atomic<int>   lastLearnedCc { -1 };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    static void migrateState (juce::ValueTree& state, int fromVersion);
    void parameterChanged (const juce::String&, float) override { markParamsChanged(); }

    std::atomic<bool> paramsChanged { true };
    int currentProgram = 0;

    const SolutionDSP::ModuleId moduleId { SolutionDSP::moduleIdFromName (SOLUTION_MODULE_ID) };
    SolutionDSP::SuiteModuleEngine moduleEngine;
    juce::SharedResourcePointer<SolutionDSP::RackLinkBus> rackLinkBus;
    juce::AudioBuffer<float> dryCopy;

    std::atomic<int> learnArmedIndex { -1 };
    std::array<std::atomic<int>, 128> ccToParamIndex;
    std::vector<juce::String> learnableParamIds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SolutionAudioProcessor)
};
