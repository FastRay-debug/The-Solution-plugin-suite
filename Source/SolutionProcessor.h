#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#ifndef SOLUTION_HEADLESS
 #define SOLUTION_HEADLESS 0
#endif
#include <juce_dsp/juce_dsp.h>
#include "ModuleEngines.h"
#include "PresetManager.h"
#include "Metering.h"
#include "Dynamics.h"
#include "RoomCorrection.h"
#include "Spatial.h"
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

    // Room measurement (message thread). Output plays a sweep while the input (mic) is recorded.
    void triggerRoomSweep (float seconds = 2.0f);
    bool isRoomCapturing() const { return measurement.getState() == SolutionDSP::RoomMeasurement::State::running; }
    float getRoomCaptureProgress() const { return measurement.getProgress(); }
    // Call regularly from the message thread; returns true when a new analysis was applied.
    bool pollRoomAnalysis();
    const SolutionDSP::RoomAnalysis& getRoomAnalysis() const { return roomAnalysis; }
    bool isRoomAnalysing() const { return analysisBusy.load(); }

    // Which mic did the measuring (message thread). Changing it refits the correction instantly.
    const SolutionDSP::MicProfile& getMicProfile() const { return micProfile; }
    void setMicProfile (const juce::String& builtInId);
    bool loadMicCalibration (const juce::File& file);      // REW/miniDSP style .cal/.txt
    int getRackLinkCount() const { return rackLinkBus->activeInstances.load (std::memory_order_relaxed); }
    int getRackSlot() const { return rackSlot; }
    SolutionDSP::RackLinkBus& getRackBus() { return *rackLinkBus; }
    void updateTrackProperties (const TrackProperties& properties) override;
    float getCvLfo() const { return rackLinkBus->cvLfoValue.load (std::memory_order_relaxed); }
    float getEnvFollower() const { return rackLinkBus->envFollower.load (std::memory_order_relaxed); }

    void armMidiLearn (const juce::String& paramId);
    int getLastLearnedCc() const { return lastLearnedCc.load (std::memory_order_relaxed); }

    juce::UndoManager undoManager;
    juce::AudioProcessorValueTreeState apvts;
    PresetManager presets { apvts };

    // Saved-state format version. Bump it and extend migrateState() whenever a
    // parameter is renamed, removed or changes range, so old sessions still load.
    static constexpr int stateVersion = 4;   // v2: Rack Link + Glue, v3: room correction, v4: binaural

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
    SolutionDSP::LoudnessMeter loudness;                 // BS.1770 momentary / short-term / integrated + true peak
    std::atomic<float> limiterGainReductionDb { 0.0f };
    std::atomic<float> glueGainReductionDb { 0.0f };
    std::atomic<float> duckGainReductionDb { 0.0f };
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
    SolutionDSP::IntegerDelay dryDelay;                  // keeps dry aligned with the oversampled wet path
    SolutionDSP::TruePeakLimiter ceilingLimiter;         // -0.5 dBTP ceiling guard
    SolutionDSP::GlueCompressor glue;
    SolutionDSP::DuckKeyFollower duckKey;
    SolutionDSP::LinkDucker ducker;
    SolutionDSP::RoomMeasurement measurement;
    SolutionDSP::RoomCorrector roomCorrector;
    SolutionDSP::BinauralRenderer binaural;
    juce::AudioBuffer<float> measureOut;
    SolutionDSP::RoomAnalysis roomAnalysis;                      // message thread
    std::unique_ptr<SolutionDSP::RoomAnalysis> finishedAnalysis; // handed over from the worker
    juce::CriticalSection analysisLock;
    std::atomic<bool> analysisBusy { false };
    void applyRoomAnalysis (const SolutionDSP::RoomAnalysis&, bool saveToState);
    void saveMicToState();
    void refitForCurrentMic();
    SolutionDSP::MicProfile micProfile { SolutionDSP::findMicProfile ("generic") };
    juce::ThreadPool analysisPool { 1 };                         // declared last: joins first on destruction
    int rackSlot = -1;
    double preparedSampleRate = 0.0;

    std::atomic<int> learnArmedIndex { -1 };
    std::array<std::atomic<int>, 128> ccToParamIndex;
    std::vector<juce::String> learnableParamIds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SolutionAudioProcessor)
};
