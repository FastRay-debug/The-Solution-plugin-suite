#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "SolutionProcessor.h"
#include "SystemMonitor.h"
#include "VoiceHub.h"

class SolutionAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                           private juce::Timer
{
public:
    explicit SolutionAudioProcessorEditor (SolutionAudioProcessor&);
    ~SolutionAudioProcessorEditor() override;

    void resized() override;

private:
    void timerCallback() override;
    void sendParamState();
    void sendPresetList (const juce::String& message = {});
    void handlePresetCommand (const juce::var& data);
    void sendRackState();
    void sendRoomResult();
    std::unique_ptr<juce::FileChooser> micFileChooser, referenceChooser;
    void sendAssistantState();
    void sendCharacterState (const juce::String& message = {});
    void sendRemoteState (const juce::String& error = {});
    void sendScope();
    void handleStage (const juce::String& type, const juce::var& data);
    juce::dsp::FFT scopeFft { 11 };
    std::vector<float> scopeL, scopeR, fftWork;
    int scopeWrite = 0;
    bool stageGestureOpen = false;
    bool assistantWasBusy = false;
    void handleAssistantCommand (const juce::var& data);
    int frameCounter = 0;
    static juce::File findSharedSkinsFolder();

    SolutionAudioProcessor& audioProcessor;
    const juce::File sharedSkinsDir;   // must be declared before webView (used to build it)
    const bool skinsAvailable;
    juce::WebBrowserComponent webView;

    // Every message to the page goes to the main view and, when open, the fullscreen pop-out.
    juce::WebBrowserComponent::Options makeOptions();
    void emitToViews (const juce::Identifier& name, const juce::var& value);
    void handleFullscreen (const juce::var& data);
    juce::String baseUrl() const;
    // Diagnostic Utility (Billet Audio hub): real system + session readings while the panel is open.
    void sendDiagnostics();
    void updateDiagLog();
    void exportDiagnostics (const juce::String& format);
    juce::var diagPluginList();
    std::unique_ptr<juce::SharedResourcePointer<SolutionDiag::SystemMonitor>> systemMonitor;
    juce::Array<juce::var> diagLog;
    struct SlotCounts { uint32_t overloads = 0, errors = 0; bool seen = false; };
    std::array<SlotCounts, 64> lastSlotCounts {};
    uint32_t lastClips = 0, lastBad = 0, lastPhase = 0, lastDc = 0; int lastPanic = 0;
    juce::int64 customSkinsStamp = -1;
    // Voice control (one hub per DAW process; this editor executes what's meant for it).
    std::unique_ptr<juce::SharedResourcePointer<SolutionVoice::VoiceHub>> voice;
    void handleVoice (const juce::var& data);
    juce::String executeVoice (const SolutionVoice::Command& cmd);
    std::vector<SolutionVoice::ParamInfo> voiceParamTable() const;
    struct PopoutWindow;
    std::unique_ptr<PopoutWindow> popout;      // declared after webView: closes first

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SolutionAudioProcessorEditor)
};
