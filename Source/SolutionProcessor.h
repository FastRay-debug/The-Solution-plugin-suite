#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#ifndef SOLUTION_HEADLESS
 #define SOLUTION_HEADLESS 0
#endif
#if ! SOLUTION_HEADLESS
 #include "DiagBridge.h"
#endif
#include <juce_dsp/juce_dsp.h>
#include "ModuleEngines.h"
#include "Dialogue.h"
#include "HeadTracker.h"
#include "NetOffload.h"
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
#include "Distance.h"
#include "MotionPath.h"
#include "Dqs.h"
#include "EditHistory.h"
#include "ToneAnalyzer.h"
#include <thread>
#include <atomic>
#include <array>

class SolutionAudioProcessor final : public juce::AudioProcessor,
                                     private juce::AudioProcessorValueTreeState::Listener,
                                     private juce::AsyncUpdater,
                                     private juce::Timer
{
public:
    // moduleOverride: run as a specific module (the DSP Node hosts every module in one app).
    explicit SolutionAudioProcessor (const juce::String& moduleOverride = {}, bool measurementCopy = false);
    const bool isMeasurementCopy;                                // private copy for the test-tone analyser: no rack slot, no timers
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
    void applyPendingChanges() { handleUpdateNowIfNeeded(); }
    // LAN / local DSP offload (message thread).
    bool connectOffload (const juce::String& host, int port, int latencyBudget, juce::String& error);
    void disconnectOffload();
    bool isOffloaded() const { return offloadActive.load(); }
    const SolutionNet::OffloadClient* getOffload() const { return offload.get(); }
    juce::String offloadNotice;
    // Safe / Vanilla Load (Ctrl+Shift while loading): the saved settings wait here until restored.
    std::atomic<bool> vanillaLoaded { false };
    bool restoreSkippedState()
    {
        if (skippedState.getSize() == 0) return false;
        restoringSkipped = true;
        setStateInformation (skippedState.getData(), (int) skippedState.getSize());
        restoringSkipped = false;
        skippedState.reset(); vanillaLoaded = false;
        return true;
    }  // run queued message-thread work now (tests, offline hosts)
    // Billet Audio hub - Emergency Panic Reset: flush every tail/delay/filter/limiter, then fade back in.
    void panicReset();                                           // message thread
    void panicResetEverywhere();                                 // every Solution instance in the session
    std::atomic<int> panicCount { 0 };
    std::atomic<int> panicFadeRemaining { 0 };
    void pollSessionRequests()                                   // requests from other instances (timer, message thread)
    {
        pollAlignRequests();
        if (rackSlot < 0) return;
        auto& me = rackLinkBus->slots[(size_t) rackSlot];
        if (me.monitorOffRequest.exchange (false))
            turnOffMonitoring();
        if (me.panicRequest.exchange (false))
            panicReset();
        const auto& params = getParameters();
        for (size_t i = 0; i < me.reqParam.size(); ++i)                    // moves from another track's stage
        {
            const int index = me.reqParam[i].load();
            if (index < 0) continue;
            const float value = me.reqValue[i].load();
            me.reqParam[i].store (-1);
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (params[index]))
                setParamReal (ranged->getParameterID(), value);
        }
    }

    // 3D stage (message thread).
    void setMotionPath (const std::vector<SolutionDSP::MotionPoint>& points, bool saveToState = true);
    std::vector<SolutionDSP::MotionPoint> getMotionPath() const;
    void setParamReal (const juce::String& id, float value);                    // with a host gesture
    bool requestParamOnSlot (int slot, const juce::String& id, float value);    // own slot or another instance's
    std::atomic<float> animAz { 0.0f }, animEl { 0.0f }, animDist { 2.0f };     // where the sound is right now
    SolutionDSP::AudioTap scopeTap;                                            // real-audio scopes in the UI
    std::atomic<int> dqsLevel { 0 };                                           // 0 = 4x, 1 = 2x, 2 = 1x oversampling
    std::atomic<float> dqsSuiteLoad { 0.0f }, dqsOwnLoad { 0.0f };
    std::atomic<float> voiceConfidence { 0.0f }, dialogueDuckDb { 0.0f };    // dialogue source / music ducking readouts
    std::atomic<int>   dialogueSources { 0 };
    std::atomic<bool>  hostPlaying { false }, hostRecording { false };

    // Diagnostics: real events counted on the audio thread (the utility turns them into a log).
    struct DiagCounters
    {
        std::atomic<uint32_t> overloads { 0 };    // blocks that took longer than real time allows
        std::atomic<uint32_t> inputClips { 0 };   // blocks whose input hit 0 dBFS or above
        std::atomic<uint32_t> badSamples { 0 };   // blocks containing NaN/Inf (silenced to protect speakers)
        std::atomic<uint32_t> phaseEvents { 0 };  // half-second stretches of strong phase cancellation
        std::atomic<uint32_t> dcEvents { 0 };     // times a DC offset showed up on the input (DC Guard removed it)
        std::atomic<float>    lastBlockLoad { 0.0f };
    } diag;
    float phaseBadSeconds = 0.0f;
    SolutionDSP::InfraLift infraLift;
    SolutionDSP::DcGuard dcGuardIn, dcGuardOut;
    juce::SharedResourcePointer<SolutionDSP::HeadTracker> headTracker;      // binaural follows your head
    SolutionDSP::VoiceDetector voiceDetector;
    SolutionDSP::DialogueDucker dialogueDucker;
    SolutionDSP::TransientShaper transientShaper;
    bool inputDcFlag = false;       // The Bunker: infrasonic lift, just before the limiter
   #if ! SOLUTION_HEADLESS
    juce::SharedResourcePointer<SolutionDiag::DiagBridge> diagBridge;   // feeds the standalone Solution Utility
   #endif

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
    // MIDI mappings (CC -> parameter), saved with the session. Message thread.
    // Edit history with selective undo (message thread).
    juce::var historyJson() const;                                   // [{seq,time,label,changes:[{id,from,to,later}]}]
    int revertHistory (int seq);                                     // undo just that entry; returns controls moved
    int restoreHistory (int seq, const juce::StringArray& keepIds);  // go back to there, keep chosen controls
    void clearHistory() { history.clear(); historyGen++; publishHistory(); }
    int getHistoryGen() const { return historyGen; }
    void markTouched (const juce::String& id);                       // a person moved it (UI, voice, remote)
    juce::String historyText() const { return history.toString (40); }
    void pollHistoryForTest() { pollHistory(); }
    void setHostPlayingForTest (bool on) { hostPlaying.store (on); }
    // Test-tone analyser: measures a private copy of this module with the current settings (message thread).
    bool startToneAnalysis (float toneHz, float levelDb);        // false if one is already running
    float toneAnalysisProgress() const { return toneProgress.load(); }
    bool toneAnalysisRunning() const { return toneJob != nullptr; }
    juce::var takeToneAnalysisResult();                          // non-void once, when a run has finished
    juce::var runToneAnalysisNow (float toneHz, float levelDb);  // blocking (tests, offline)
    juce::String getCcMapString() const;
    void setCcMapString (const juce::String& map);
    bool setCcMapping (int cc, const juce::String& paramId);

    juce::UndoManager undoManager;
    juce::AudioProcessorValueTreeState apvts;
    PresetManager presets { apvts };

    // Saved-state format version. Bump it and extend migrateState() whenever a
    // parameter is renamed, removed or changes range, so old sessions still load.
    static constexpr int stateVersion = 18;  // v18: dialogue ducking + transient shaper, v17: lookahead limiter, v16: Infrasonic Lift, v15: stage creature, v14: DQS  // v12: Safe Bounce, v13: 3D stage (distance, motion)  // v7 115 Hz notch, v8 Pro Tricks, v9 listening rooms, v10 Unmask, v11 Auto-Align

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
public:
    std::atomic<int>   learnSeq { 0 };          // counts completed MIDI learns (wizard progress)
private:

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    static void migrateState (juce::ValueTree& state, int fromVersion);
    void parameterChanged (const juce::String& id, float) override
    {
        markParamsChanged();
        offloadStateDirty = true;
        if (id == "limiter_lookahead") { lookaheadChanged = true; triggerAsyncUpdate(); }
    }
    std::atomic<bool> lookaheadChanged { false };
    double limiterLookaheadMs() const { return apvts.getRawParameterValue ("limiter_lookahead")->load() > 0.5f ? 5.0 : 1.5; }

    std::atomic<bool> paramsChanged { true };
    int currentProgram = 0;

    const juce::String moduleKey;                     // e.g. "TheBunker"
    const SolutionDSP::ModuleId moduleId { SolutionDSP::moduleIdFromName (moduleKey.toRawUTF8()) };
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
    SolutionDSP::DistanceStage distanceStage;
    SolutionDSP::DqsController dqs;
    float ownLoadSmoothed = 0.0f;
    SolutionDSP::MotionPlayer motion;
    double motionFreeSeconds = 0.0;
    struct ToneJob { std::unique_ptr<SolutionAudioProcessor> copy; std::thread worker; std::atomic<bool> done { false }; juce::var result; float hz = 1000, db = -18; };
    std::unique_ptr<ToneJob> toneJob;
    std::atomic<float> toneProgress { 0.0f };
    juce::var toneResultReady;
    std::unique_ptr<SolutionAudioProcessor> makeMeasurementCopy();
    static juce::var measure (SolutionAudioProcessor& copy, float hz, float db, std::atomic<float>& progress);
    void pollToneJob();
    SolutionHistory::EditHistory history;
    std::unique_ptr<std::atomic<bool>[]> touchFlags;               // indexed like learnableParamIds (MIDI touches on the audio thread)
    std::map<juce::String, int> learnIndex;
    int historyGen = 0;
    juce::String historyLabel;
    void pollHistory();
    void publishHistory();
    int applyHistoryValues (const std::map<juce::String, float>& values, const juce::String& label);
    void timerCallback() override
    {
        pollSessionRequests();
        pollHistory();
        pollToneJob();
        if (offloadActive.load() && offload != nullptr && offloadStateDirty.exchange (false))
        {
            juce::MemoryBlock st; getStateInformation (st); offload->sendState (st);       // the node sounds identical
        }
        if (offloadFellBack.exchange (false)) { disconnectOffload(); offloadNotice = "The DSP node fell behind - processing locally again"; }
    }
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
    void handleAsyncUpdate() override
    {
        // Lookahead length changed: re-prepare the limiter while the host outputs silence, then report the new latency.
        if (lookaheadChanged.exchange (false) && preparedSampleRate > 0.0)
        {
            suspendProcessing (true);
            ceilingLimiter.prepare (preparedSampleRate, limiterLookaheadMs());
            suspendProcessing (false);
        }
        updateLatencyNow();
    }
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
    std::unique_ptr<SolutionNet::OffloadClient> offload;
    std::atomic<bool> offloadActive { false }, offloadStateDirty { false }, offloadFellBack { false };
    juce::MemoryBlock skippedState;
public:
    juce::String pendingSkin;          // Skin Studio upload waiting to be shown (message thread only)
private:
    bool restoringSkipped = false;
    double preparedSampleRate = 0.0;
    int preparedBlockSize = 0;

    std::atomic<int> learnArmedIndex { -1 };
    std::array<std::atomic<int>, 128> ccToParamIndex;
    std::vector<juce::String> learnableParamIds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SolutionAudioProcessor)
};
