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
#include "MixAssistant.h"
#include "StemSplitter.h"
#include "NeuralCharacter.h"
#include "RemoteControl.h"
#include "ProTricks.h"
#include "RoomLibrary.h"
#include "Unmask.h"
#include "AutoAlign.h"
#include <atomic>
#include <array>

class SolutionAudioProcessor final : public juce::AudioProcessor,
                                     private juce::AudioProcessorValueTreeState::Listener,
                                     private juce::AsyncUpdater,
                                     private juce::Timer
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
    float getAutoNotchDepth() const { return moduleEngine.getRoomMatcher().getAutoNotchDepth(); }
    // Call regularly from the message thread; returns true when a new analysis was applied.
    bool pollRoomAnalysis();
    const SolutionDSP::RoomAnalysis& getRoomAnalysis() const { return roomAnalysis; }
    bool isRoomAnalysing() const { return analysisBusy.load(); }

    // Mix Assistant (message thread, except the audio tap).
    struct Assistant
    {
        SolutionDSP::AudioTap tap;                                // audio thread -> analyser
        std::unique_ptr<SolutionDSP::MixAnalyser> live;
        SolutionDSP::MixProfile reference;
        SolutionDSP::MixComparison comparison;
        std::atomic<bool> loadingReference { false };
        juce::String referenceError;
    } assistant;
    void assistantStartListening();
    void assistantStopListening() { assistant.tap.setEnabled (false); }
    void assistantPoll();                                         // drain tap + refresh comparison
    void assistantLoadReference (const juce::File&);              // analysed on a background thread
    void assistantUseGenre (const juce::String& genre);
    bool assistantApply (int suggestionIndex);                    // -1 = apply all
    SolutionDSP::CurrentSettings currentSettingsForAssistant() const;
    SolutionDSP::MixProfile assistantMixProfile() const { return assistant.live != nullptr ? assistant.live->finish ("Your mix") : SolutionDSP::MixProfile {}; }

    // Neural Character (message thread). Empty name = no model. Returns an error message or empty.
    juce::String setCharacterModel (const juce::String& modelName);
    juce::String getCharacterModelName() const { return apvts.state.getProperty ("charModel").toString(); }
    SolutionDSP::NeuralCharacter character;

    // Phone / tablet remote (message thread). Off until started; never saved as "on".
    bool startRemote (const juce::String& bindAddress = {});
    void stopRemote() { if (remote != nullptr) remote->stop(); }
    SolutionDSP::RemoteServer* getRemote() { return remote.get(); }
    std::function<void (std::function<void()>)> remoteExecutor;   // tests run commands inline
    juce::var getRemoteState() const;

    // Pro Tricks
    void startAutoGainStage();                 // message thread: measure ~3 s of input
    bool pollAutoGainStage();                  // message thread: true when it just set the trims
    int getAutoGainStageState() const { return autoStageState.load(); }   // 0 idle, 1 listening
    std::atomic<float> abMatchDb { 0.0f }, smoothDb { 0.0f }, guardDb { 0.0f }, streamGainDb { 0.0f };

    // Monitoring tools (A/B, Delta, streaming preview, rooms, room correction, stem solo, cal tone).
    enum MonitorBits { monAB = 1, monDelta = 2, monStream = 4, monRoom = 8, monCorrection = 16, monStemSolo = 32, monCalTone = 64, monBinaural = 128 };
    static constexpr int monitorAutoBypassMask = 127;           // everything except binaural (may be intended)
    int getMonitorMask() const { return monitorMask.load(); }
    int getSessionMonitorCount() const;                          // other instances with monitoring tools on
    void turnOffMonitoring();                                    // message thread
    void turnOffMonitoringEverywhere();                          // every instance in the session
    void pollSessionRequests()                                   // requests from other instances (timer, message thread)
    {
        pollAlignRequests();
        if (rackSlot >= 0 && rackLinkBus->slots[(size_t) rackSlot].monitorOffRequest.exchange (false))
            turnOffMonitoring();
    }

    // Auto-Align (message thread): measure against the group's reference and fix timing/polarity.
    juce::String alignNow();
    void pollAlignRequests();                  // applies a request from another instance (timer)

    // Stem Focus adds latency only while it's on; the host is told on the message thread.
    void updateLatencyNow();
    int getBaseLatency() const { return moduleEngine.getLatencySamples() + ceilingLimiter.getLatency(); }

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
    static constexpr int stateVersion = 12;  // v12: Safe Bounce  // v7 115 Hz notch, v8 Pro Tricks, v9 listening rooms, v10 Unmask, v11 Auto-Align

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
    SolutionDSP::MonoLows monoLows;
    SolutionDSP::ListeningRoomSim listeningRoom;
    SolutionDSP::Unmask unmask;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd> alignDelayLine { 4096 };
    long long ownTimeline = 0;
    std::atomic<int> monitorMask { 0 };
    void timerCallback() override { pollSessionRequests(); }
public:
    std::array<std::atomic<float>, SolutionDSP::Unmask::numBands> unmaskDipDb {};
private:
    SolutionDSP::Smoother smoother;
    SolutionDSP::PhoneBass phoneBass;
    SolutionDSP::LoudnessMatcher abMatcher;
    juce::SmoothedValue<float> trimInGain { 1.0f }, trimOutGain { 1.0f }, previewGain { 1.0f };
    std::atomic<int> autoStageState { 0 };
    std::atomic<float> autoStageResultDb { -100.0f };
    double autoStageSum = 0.0; long long autoStageCount = 0;
    SolutionDSP::StemSplitter stemSplitter;
    juce::AudioBuffer<float> stemRest;
    SolutionDSP::IntegerDelay restDelay;
    int lastStemMode = 0;
    std::atomic<bool> stemActive { false };
    void handleAsyncUpdate() override { updateLatencyNow(); }
    juce::AudioBuffer<float> measureOut;
    SolutionDSP::RoomAnalysis roomAnalysis;                      // message thread
    std::unique_ptr<SolutionDSP::RoomAnalysis> finishedAnalysis; // handed over from the worker
    juce::CriticalSection analysisLock;
    std::atomic<bool> analysisBusy { false };
    void applyRoomAnalysis (const SolutionDSP::RoomAnalysis&, bool saveToState);
    void saveMicToState();
    void refitForCurrentMic();
    SolutionDSP::MicProfile micProfile { SolutionDSP::findMicProfile ("generic") };
    std::unique_ptr<SolutionDSP::RemoteServer> remote;
    juce::StringArray remotePresetNames;
    juce::CriticalSection remotePresetLock;
    std::shared_ptr<bool> aliveFlag = std::make_shared<bool> (true);   // lets async callbacks check we still exist
    juce::ThreadPool analysisPool { 1 };                         // declared last: joins first on destruction
    int rackSlot = -1;
    double preparedSampleRate = 0.0;

    std::atomic<int> learnArmedIndex { -1 };
    std::array<std::atomic<int>, 128> ccToParamIndex;
    std::vector<juce::String> learnableParamIds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SolutionAudioProcessor)
};
