#include "SolutionProcessor.h"
#include "SolutionEditor.h"
#include "BinaryData.h"
#include "Vendor.h"
#include "HostTransport.h"

#ifndef SOLUTION_BUNDLE_SKINS
#define SOLUTION_BUNDLE_SKINS 1
#endif

namespace
{
    juce::WebBrowserComponent::Resource makeResource (const void* data, size_t size, const char* mimeType)
    {
        juce::WebBrowserComponent::Resource resource;
        const auto* bytes = static_cast<const std::byte*> (data);
        resource.data.assign (bytes, bytes + size);
        resource.mimeType = mimeType;
        return resource;
    }

    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url, const juce::File& sharedSkinsDir)
    {
        const auto cleanUrl = url.upToFirstOccurrenceOf ("?", false, false);
        if (cleanUrl == "/" || cleanUrl == "/index.html" || cleanUrl.isEmpty())
            return makeResource (BinaryData::index_html, (size_t) BinaryData::index_htmlSize, "text/html");

        if (cleanUrl == "/stage3d.bundle.js")
            return makeResource (BinaryData::stage3d_bundle_js, (size_t) BinaryData::stage3d_bundle_jsSize, "text/javascript");

        if (cleanUrl.startsWith ("/skins/") && cleanUrl.endsWith (".jpg"))
        {
            const auto baseName = cleanUrl.fromLastOccurrenceOf ("/", false, false).upToLastOccurrenceOf (".", false, false);

            // Only plain skin names - never let a URL reach outside the Skins folder.
            if (baseName.isEmpty() || ! baseName.containsOnly ("abcdefghijklmnopqrstuvwxyz0123456789_-"))
                return std::nullopt;

            // 0. Your own skins from the phone's Skin Studio.
            if (baseName.startsWith ("custom_"))
            {
                juce::MemoryBlock fileData;
                if (SolutionVendor::userFolder ("Skins").getChildFile (baseName + ".jpg").loadFileAsData (fileData))
                    return makeResource (fileData.getData(), fileData.getSize(), "image/jpeg");
                return std::nullopt;
            }

            // 1. Shared Skins folder installed once for the whole suite.
            if (sharedSkinsDir.isDirectory())
            {
                juce::MemoryBlock fileData;
                if (sharedSkinsDir.getChildFile (baseName + ".jpg").loadFileAsData (fileData))
                    return makeResource (fileData.getData(), fileData.getSize(), "image/jpeg");
            }

            // 2. Skins embedded in this plugin (only when built with BUNDLE_SKINS=ON).
            int size = 0;
            if (const char* ptr = BinaryData::getNamedResource ((baseName + "_jpg").toRawUTF8(), size))
                return makeResource (ptr, (size_t) size, "image/jpeg");
        }
        return std::nullopt;
    }
}

juce::File SolutionAudioProcessorEditor::findSharedSkinsFolder()
{
    // Per-user install first, then the machine-wide one (Windows: %APPDATA% then %ProgramData%;
    // macOS: ~/Library then /Library). Folders from builds before the Billet Audio rename still work.
    for (auto vendor : { SolutionVendor::name, SolutionVendor::legacyName })
        for (auto location : { juce::File::userApplicationDataDirectory, juce::File::commonApplicationDataDirectory })
        {
            const auto dir = juce::File::getSpecialLocation (location).getChildFile (SolutionVendor::prefix() + vendor + "/The Solution/Skins");
            if (dir.isDirectory() && dir.getNumberOfChildFiles (juce::File::findFiles, "*.jpg") > 0)
                return dir;
        }
    return {};
}

juce::WebBrowserComponent::Options SolutionAudioProcessorEditor::makeOptions()
{
    return juce::WebBrowserComponent::Options{}
           #if JUCE_WINDOWS
           .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
           .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2{}
                                        .withUserDataFolder (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("TheSolutionWebView2")))
           #endif
           .withNativeIntegrationEnabled()
           .withResourceProvider ([skinsDir = sharedSkinsDir] (const auto& url) { return getResource (url, skinsDir); })
           .withEventListener ("setParam", [this] (const juce::var& data)
           {
               if (auto* obj = data.getDynamicObject())
               {
                   const auto paramId = obj->getProperty ("id").toString();
                   const float value  = static_cast<float> (obj->getProperty ("value"));
                   if (voice != nullptr) (*voice)->noteActive (audioProcessor.getRackSlot());   // "the last plugin you touched"
                   if (auto* param = audioProcessor.apvts.getParameter (paramId))
                   {
                       audioProcessor.undoManager.beginNewTransaction (paramId);
                       param->setValueNotifyingHost (param->convertTo0to1 (value));
                       audioProcessor.markTouched (paramId);
                   }
               }
           })
           .withEventListener ("captureRoomSweep", [this] (const juce::var& data)
           {
               float secs = 2.0f;
               if (auto* obj = data.getDynamicObject())
                   if (obj->hasProperty ("seconds"))
                       secs = static_cast<float> (obj->getProperty ("seconds"));
               audioProcessor.triggerRoomSweep (secs);
           })
           .withEventListener ("undoAction", [this] (const juce::var&)
           {
               audioProcessor.undoManager.undo();
           })
           .withEventListener ("redoAction", [this] (const juce::var&)
           {
               audioProcessor.undoManager.redo();
           })
           .withEventListener ("midiLearn", [this] (const juce::var& data)
           {
               if (auto* obj = data.getDynamicObject())
                   audioProcessor.armMidiLearn (obj->getProperty ("id").toString());
           })
           // Page finished loading: send it the real parameter values + saved UI settings
           .withEventListener ("uiReady", [this] (const juce::var&)
           {
               sendParamInfo();
               sendParamState();
               sendPresetList();
               sendRoomResult();
               sendCharacterState();
               sendRemoteState();
               {
                   juce::Array<juce::var> rooms;
                   for (const auto& room : SolutionDSP::listeningRooms())
                   {
                       juce::DynamicObject::Ptr o = new juce::DynamicObject();
                       o->setProperty ("name", juce::String (room.name));
                       o->setProperty ("category", juce::String (room.category));
                       rooms.add (juce::var (o.get()));
                   }
                   emitToViews ("roomList", juce::var (rooms));
               }
           })
           .withEventListener ("setUiProp", [this] (const juce::var& data)
           {
               if (auto* obj = data.getDynamicObject())
                   audioProcessor.setUiProperty (obj->getProperty ("key").toString(), obj->getProperty ("value").toString());
           })
           .withEventListener ("setMic", [this] (const juce::var& data)
           {
               if (auto* obj = data.getDynamicObject())
                   audioProcessor.setMicProfile (obj->getProperty ("id").toString());
               sendRoomResult();
           })
           .withEventListener ("loadMicCal", [this] (const juce::var&)
           {
               micFileChooser = std::make_unique<juce::FileChooser> ("Choose your mic's calibration file", juce::File(), "*.cal;*.txt;*.frd;*.csv");
               micFileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                            [this] (const juce::FileChooser& fc)
               {
                   const auto file = fc.getResult();
                   if (file.existsAsFile() && ! audioProcessor.loadMicCalibration (file))
                       emitToViews ("roomNotice", juce::var ("That file doesn't look like a mic calibration file (expected lines of: frequency dB)."));
                   sendRoomResult();
               });
           })
           .withEventListener ("stageMove",    [this] (const juce::var& d) { handleStage ("move", d); })
           .withEventListener ("stageGesture", [this] (const juce::var& d) { handleStage ("gesture", d); })
           .withEventListener ("motionPath",   [this] (const juce::var& d) { handleStage ("path", d); })
           .withEventListener ("unmaskPair",   [this] (const juce::var& d) { handleStage ("unmask", d); })
           .withEventListener ("monitorOff", [this] (const juce::var& data)
           {
               if ((bool) data.getProperty ("all", false)) audioProcessor.turnOffMonitoringEverywhere();
               else                                         audioProcessor.turnOffMonitoring();
           })
           .withEventListener ("alignNow", [this] (const juce::var&)
           {
               emitToViews ("alignResult", juce::var (audioProcessor.alignNow()));
           })
           .withEventListener ("autoStage", [this] (const juce::var&) { audioProcessor.startAutoGainStage(); })
           .withEventListener ("diag", [this] (const juce::var& data)
           {
               const auto cmd = data.getProperty ("cmd", {}).toString();
               if (cmd == "open")       { if (systemMonitor == nullptr) systemMonitor = std::make_unique<juce::SharedResourcePointer<SolutionDiag::SystemMonitor>>(); sendDiagnostics(); }
               else if (cmd == "close") systemMonitor.reset();
               else if (cmd == "export") exportDiagnostics (data.getProperty ("format", "csv").toString());
               else if (cmd == "clearLog") { diagLog.clear(); sendDiagnostics(); }
               else if (cmd == "slotParam")
                   audioProcessor.requestParamOnSlot ((int) data.getProperty ("slot", -1), data.getProperty ("id", {}).toString(), (float) data.getProperty ("value", 0.0));
               else if (cmd == "panicSlot")
               {
                   const int slot = (int) data.getProperty ("slot", -1);
                   if (slot == audioProcessor.getRackSlot()) audioProcessor.panicReset();
                   else if (slot >= 0 && slot < (int) audioProcessor.getRackBus().slots.size()) audioProcessor.getRackBus().slots[(size_t) slot].panicRequest = true;
               }
           })
           .withEventListener ("voice", [this] (const juce::var& d) { handleVoice (d); })
           .withEventListener ("curveWanted", [this] (const juce::var& d) { audioProcessor.curveWanted = (bool) d.getProperty ("on", false); lastCurveGen = -1; })
           .withEventListener ("aiPanel", [this] (const juce::var& d)
           {
               aiPanelOpen = (bool) d.getProperty ("open", false);
               audioProcessor.moodWatch = aiPanelOpen;
               if (aiPanelOpen) sendAiState();
           })
           .withEventListener ("aiCues", [this] (const juce::var& d)
           {
               const auto cmd = d.getProperty ("cmd", {}).toString();
               const double t = d.getProperty ("t", -1.0);
               if (cmd == "add") audioProcessor.addCue (d.getProperty ("name", {}).toString(), t, (double) d.getProperty ("fade", 1.0));
               else if (cmd == "remove") audioProcessor.removeCue (t);
               else if (cmd == "edit") audioProcessor.editCue (t, d.getProperty ("name", {}).toString(), (double) d.getProperty ("fade", -1.0), (double) d.getProperty ("newT", -1.0));
               sendAiState();
           })
           .withEventListener ("aiMeta", [this] (const juce::var& d)
           {
               const auto cmd = d.getProperty ("cmd", {}).toString();
               const bool rack = (bool) d.getProperty ("rack", false);
               if (cmd == "clear") { audioProcessor.clearTrajectory(); emitMeta ("Recording cleared."); return; }
               if (cmd == "embed")
               {
                   micFileChooser = std::make_unique<juce::FileChooser> ("Choose the bounced WAV to add spatial metadata to", juce::File(), "*.wav");
                   micFileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, rack] (const juce::FileChooser& fc)
                   {
                       const auto f = fc.getResult(); if (! f.existsAsFile()) return;
                       juce::File out; const auto err = audioProcessor.embedMetadata (f, rack, out);
                       emitMeta (err.isNotEmpty() ? err : "Saved " + out.getFileName() + " next to the original (ADM axml" + juce::String (rack ? "" : " + chna") + " + iXML).");
                   });
               }
               else if (cmd == "sidecar")
               {
                   micFileChooser = std::make_unique<juce::FileChooser> ("Save the spatial metadata (JSON + ADM XML)", juce::File(), "*.json");
                   micFileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles, [this, rack] (const juce::FileChooser& fc)
                   {
                       const auto f = fc.getResult(); if (f == juce::File()) return;
                       const auto err = audioProcessor.exportMetadataSidecars (f.withFileExtension (""), rack);
                       emitMeta (err.isNotEmpty() ? err : "Saved " + f.withFileExtension ("json").getFileName() + " and " + f.withFileExtension ("adm.xml").getFileName() + ".");
                   });
               }
               else emitMeta ({});
           })
           .withEventListener ("toneAnalyze", [this] (const juce::var& d)
           {
               const float hz = (float) (double) d.getProperty ("hz", 1000.0), db = (float) (double) d.getProperty ("db", -18.0);
               const bool ok = audioProcessor.startToneAnalysis (juce::jlimit (20.0f, 20000.0f, hz), juce::jlimit (-60.0f, 6.0f, db));
               emitToViews ("toneProgress", ok ? 0.02 : -1.0);
           })
           .withEventListener ("history", [this] (const juce::var& d)
           {
               const auto cmd = d.getProperty ("cmd", {}).toString();
               if (cmd == "revert")  audioProcessor.revertHistory ((int) d.getProperty ("seq", 0));
               else if (cmd == "restore")
               {
                   juce::StringArray keep;
                   if (auto* arr = d.getProperty ("keep", {}).getArray()) for (auto& k : *arr) keep.add (k.toString());
                   audioProcessor.restoreHistory ((int) d.getProperty ("seq", 0), keep);
               }
               else if (cmd == "clear") audioProcessor.clearHistory();
               else if (cmd == "rackRevert") rackRevert ((int) d.getProperty ("slot", -1), (int) d.getProperty ("seq", 0));
               if (cmd == "rack" || cmd == "rackRevert") sendRackHistory();
               sendHistory();
           })
           .withEventListener ("midimap", [this] (const juce::var& d)
           {
               const auto cmd = d.getProperty ("cmd", {}).toString();
               if (cmd == "set") audioProcessor.setCcMapping ((int) d.getProperty ("cc", -1), d.getProperty ("id", {}).toString());
               else if (cmd == "replace") audioProcessor.setCcMapString (d.getProperty ("map", {}).toString());
               emitToViews ("midimapState", juce::var (audioProcessor.getCcMapString()));
           })
           .withEventListener ("offload", [this] (const juce::var& d)
           {
               const auto cmd = d.getProperty ("cmd", {}).toString();
               if (cmd == "discover")
               {
                   juce::Component::SafePointer<SolutionAudioProcessorEditor> safe (this);
                   std::thread ([safe] {
                       auto nodes = SolutionNet::discoverNodes (600);
                       juce::MessageManager::callAsync ([safe, nodes] { if (safe != nullptr) safe->emitToViews ("offloadNodes", juce::var (nodes)); });
                   }).detach();
               }
               else if (cmd == "connect")
               {
                   juce::String err;
                   const auto host = d.getProperty ("address", "127.0.0.1").toString();
                   if (! audioProcessor.connectOffload (host, (int) d.getProperty ("port", SolutionNet::nodePort), (int) d.getProperty ("latency", 2048), err))
                       emitToViews ("roomNotice", juce::var (err));
                   else emitToViews ("roomNotice", juce::var ("Processing on " + d.getProperty ("name", host).toString() + " - extra latency reported to your DAW"));
               }
               else if (cmd == "disconnect") audioProcessor.disconnectOffload();
           })
           .withEventListener ("learn", [this] (const juce::var& d)
           {
               // Adaptive Learning profile: one file per user, shared by every module (never leaves this computer).
               const auto file = SolutionVendor::userFolder ("Learning").getChildFile ("profile.json");
               const auto cmd = d.getProperty ("cmd", {}).toString();
               if (cmd == "load") emitToViews ("learnData", juce::var (file.loadFileAsString()));
               else if (cmd == "forget") file.deleteFile();
               else if (cmd == "save")
               {
                   const auto module = d.getProperty ("module", {}).toString().retainCharacters ("abcdefghijklmnopqrstuvwxyz ");
                   auto all = juce::JSON::parse (file);
                   if (! all.isObject()) all = juce::var (new juce::DynamicObject());
                   auto modules = all.getProperty ("modules", {});
                   if (! modules.isObject()) { modules = juce::var (new juce::DynamicObject()); all.getDynamicObject()->setProperty ("modules", modules); }
                   modules.getDynamicObject()->setProperty (module, juce::JSON::parse (d.getProperty ("data", {}).toString()));
                   all.getDynamicObject()->setProperty ("never", juce::JSON::parse (d.getProperty ("never", {}).toString()));
                   all.getDynamicObject()->setProperty ("v", 1);
                   file.getParentDirectory().createDirectory();
                   file.replaceWithText (juce::JSON::toString (all, true));
               }
           })
           .withEventListener ("head", [this] (const juce::var& d)
           {
               auto& ht = *audioProcessor.headTracker;
               const auto cmd = d.getProperty ("cmd", {}).toString();
               if (cmd == "enable") { ht.enabled = (bool) d.getProperty ("on", false); audioProcessor.setUiProperty ("headOn", ht.enabled ? "1" : "0"); }
               else if (cmd == "osc")
               {
                   const int port = (int) d.getProperty ("port", 0);
                   if (! ht.listenOsc (port)) emitToViews ("roomNotice", juce::var ("Couldn't listen for OSC on port " + juce::String (port) + " - another app may be using it."));
                   audioProcessor.setUiProperty ("oscPort", juce::String (port));
               }
               else if (cmd == "recenter") ht.recenter();
               else if (cmd == "pose") ht.setPose ((float) d.getProperty ("yaw", 0.0), (float) d.getProperty ("pitch", 0.0), (int) d.getProperty ("src", 3));
           })
           .withEventListener ("theme", [this] (const juce::var& d)
           {
               const auto id = d.getProperty ("id", {}).toString().retainCharacters ("abcdefghijklmnopqrstuvwxyz");
               audioProcessor.setUiProperty ("theme", id);
               if (! (bool) d.getProperty ("local", false)) { auto& bus = audioProcessor.getRackBus(); bus.suiteTheme = id; ++bus.suiteThemeGen; }
           })
           .withEventListener ("openSkinEditor", [this] (const juce::var&)
           {
               // The Skin Editor is its own program (separate installer). Look where it installs.
               juce::Array<juce::File> candidates {
                   juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory).getChildFile ("Billet Audio/The Solution/Skin Editor/Skin Editor.exe"),
                   juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory).getChildFile ("The Solution/Skin Editor/Skin Editor.exe"),
                   juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory).getChildFile ("Billet Audio Skin Editor.app") };
               for (auto& f : candidates) if (f.exists()) { f.startAsProcess(); return; }
               emitToViews ("roomNotice", juce::var ("The Skin Editor isn't installed yet - it comes with the separate Skins & Skin Editor installer."));
           })
           .withEventListener ("restoreSkipped", [this] (const juce::var&) { audioProcessor.restoreSkippedState(); sendParamState(); sendPresetList(); })
           .withEventListener ("panic", [this] (const juce::var& data)
           {
               if ((bool) data.getProperty ("all", false)) audioProcessor.panicResetEverywhere();
               else                                         audioProcessor.panicReset();
               emitToViews ("panicDone", data);
           })
           .withEventListener ("remote", [this] (const juce::var& data)
           {
               const auto cmd = data.getProperty ("cmd", {}).toString();
               juce::String error;
               if (cmd == "start" && ! audioProcessor.startRemote())
                   error = "Couldn't open a network port (47800-47999). Another app may be using them, or your firewall/host blocks it.";
               else if (cmd == "stop")
                   audioProcessor.stopRemote();
               sendRemoteState (error);
           })
           .withEventListener ("character", [this] (const juce::var& data)
           {
               auto* obj = data.getDynamicObject();
               const auto cmd = obj != nullptr ? obj->getProperty ("cmd").toString() : juce::String();
               juce::String message;
               if (cmd == "select")
               {
                   const auto name = obj->getProperty ("name").toString();
                   message = audioProcessor.setCharacterModel (name);
                   if (message.isEmpty()) message = name.isEmpty() ? "Character off" : "Loaded " + name;
               }
               else if (cmd == "reveal")
               {
                   SolutionDSP::NeuralCharacter::getModelsFolder().createDirectory();
                   SolutionDSP::NeuralCharacter::getModelsFolder().startAsProcess();
               }
               else if (cmd == "browse")      // pick a model file anywhere; it's copied into the models folder and loaded
               {
                   micFileChooser = std::make_unique<juce::FileChooser> ("Choose a neural Character model (RTNeural .json)", juce::File(), "*.json");
                   micFileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                                [this] (const juce::FileChooser& fc)
                   {
                       const auto f = fc.getResult();
                       if (! f.existsAsFile()) return;
                       const auto dir = SolutionDSP::NeuralCharacter::getModelsFolder(); dir.createDirectory();
                       const auto dest = dir.getChildFile (f.getFileName());
                       if (dest != f) f.copyFileTo (dest);
                       auto msg = audioProcessor.setCharacterModel (dest.getFileNameWithoutExtension());
                       sendCharacterState (msg.isEmpty() ? "Loaded " + dest.getFileNameWithoutExtension() : msg);
                   });
               }
               sendCharacterState (message);
           })
           .withEventListener ("assistant", [this] (const juce::var& data) { handleAssistantCommand (data); })
           .withEventListener ("resetLoudness", [this] (const juce::var&) { audioProcessor.loudness.requestReset(); })
           .withEventListener ("presetCommand", [this] (const juce::var& data) { handlePresetCommand (data); })
        .withEventListener ("fullscreen", [this] (const juce::var& data) { handleFullscreen (data); });
}

// ---------- Ctrl+` : send a panel fullscreen to another display ----------
struct SolutionAudioProcessorEditor::PopoutWindow final : public juce::Component
{
    PopoutWindow (juce::WebBrowserComponent::Options options) : view (std::move (options))
    {
        setOpaque (true);
        addAndMakeVisible (view);
    }
    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff05070a)); }
    void resized() override { view.setBounds (getLocalBounds()); }
    juce::WebBrowserComponent view;
};

void SolutionAudioProcessorEditor::emitToViews (const juce::Identifier& name, const juce::var& value)
{
    webView.emitEventIfBrowserIsVisible (name, value);
    if (popout != nullptr)
        popout->view.emitEventIfBrowserIsVisible (name, value);
}

void SolutionAudioProcessorEditor::handleFullscreen (const juce::var& data)
{
    const auto cmd = data.getProperty ("cmd", {}).toString();
    // Never delete a browser from inside its own callback: finish on the next message-loop turn.
    juce::Component::SafePointer<SolutionAudioProcessorEditor> safe (this);
    juce::MessageManager::callAsync ([safe, cmd, panel = data.getProperty ("panel", "stage").toString()]
    {
        if (safe == nullptr) return;
        auto& ed = *safe;
        ed.popout.reset();
        if (cmd == "popout")
        {
            const auto& displays = juce::Desktop::getInstance().getDisplays();
            const auto* here = displays.getDisplayForRect (ed.getScreenBounds());
            const juce::Displays::Display* target = nullptr;
            for (const auto& d : displays.displays)                 // prefer a display the plugin isn't on
                if (here == nullptr || d.totalArea != here->totalArea) { target = &d; break; }
            if (target == nullptr) target = here != nullptr ? here : displays.getPrimaryDisplay();
            if (target == nullptr) return;

            ed.popout = std::make_unique<PopoutWindow> (ed.makeOptions());
            ed.popout->setName (juce::String (SOLUTION_MODULE_NAME) + " - Fullscreen");
            ed.popout->addToDesktop (juce::ComponentPeer::windowAppearsOnTaskbar);
            ed.popout->setBounds (target->totalArea);
            ed.popout->setVisible (true);
            ed.popout->toFront (true);
            const auto panelSafe = panel.retainCharacters ("abcdefghijklmnopqrstuvwxyz");
            ed.popout->view.goToURL (ed.baseUrl() + "&popout=1&panel=" + panelSafe);
            ed.emitToViews ("fullscreenState", juce::var (true));
        }
        else
            ed.webView.emitEventIfBrowserIsVisible ("fullscreenState", juce::var (false));
    });
}

// Custom transport keys are remembered per DAW, so the same session opened in another DAW uses that DAW's keys.
juce::String SolutionAudioProcessorEditor::voiceKeysProp()
{
    return "voiceKeys_" + juce::String::toHexString (SolutionVoice::hostIdentity().trim().toLowerCase().hashCode64());
}

juce::String SolutionAudioProcessorEditor::baseUrl() const
{
    const auto escapedName = juce::URL::addEscapeChars (juce::String (SOLUTION_MODULE_NAME), true);
    const auto mods = juce::ModifierKeys::getCurrentModifiersRealtime();
    const bool safe = mods.isCtrlDown() && mods.isShiftDown();          // Safe Load: no 3D/GPU, plain drawing
    return juce::WebBrowserComponent::getResourceProviderRoot() + "?module=" + escapedName + "&skins=" + juce::String (skinsAvailable ? "1" : "0")
           + (safe ? "&safe=1" : "");
}

SolutionAudioProcessorEditor::SolutionAudioProcessorEditor (SolutionAudioProcessor& p)
    : AudioProcessorEditor (&p),
      audioProcessor (p),
      sharedSkinsDir (findSharedSkinsFolder()),
      skinsAvailable (SOLUTION_BUNDLE_SKINS != 0 || sharedSkinsDir.isDirectory()),
      webView (makeOptions())
{
    addAndMakeVisible (webView);
    audioProcessor.scopeTap.setEnabled (true);
    webView.goToURL (baseUrl());

    setSize (1080, 640);
    setResizable (true, true);
    startTimerHz (60);
}

void SolutionAudioProcessorEditor::resized()
{
    webView.setBounds (getLocalBounds());
}

// Names, ranges and choices of every control (for the Vanilla list view). Sent once per page load.
void SolutionAudioProcessorEditor::sendParamInfo()
{
    juce::Array<juce::var> list;
    for (auto* p : audioProcessor.getParameters())
        if (auto* q = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            const auto& r = q->getNormalisableRange();
            o->setProperty ("id", q->getParameterID()); o->setProperty ("name", q->getName (60)); o->setProperty ("label", q->getLabel());
            o->setProperty ("min", r.start); o->setProperty ("max", r.end); o->setProperty ("step", r.interval);
            o->setProperty ("def", q->convertFrom0to1 (q->getDefaultValue()));
            if (dynamic_cast<juce::AudioParameterBool*> (q) != nullptr) o->setProperty ("bool", true);
            if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (q)) { juce::Array<juce::var> ch; for (auto& s : c->choices) ch.add (s); o->setProperty ("choices", ch); }
            list.add (juce::var (o.get()));
        }
    emitToViews ("paramInfo", list);
}

void SolutionAudioProcessorEditor::sendParamState()
{
    juce::DynamicObject::Ptr values = new juce::DynamicObject();
    for (auto* p : audioProcessor.getParameters())
        if (auto* param = dynamic_cast<juce::RangedAudioParameter*> (p))
            values->setProperty (param->getParameterID(), param->convertFrom0to1 (param->getValue()));

    juce::DynamicObject::Ptr ui = new juce::DynamicObject();
    ui->setProperty ("skin", audioProcessor.getUiProperty ("skin"));
    // Every saved UI setting (beginner, key map, looks, look mode, MIDI/joystick maps, meter orientation, voice...)
    const auto& st = audioProcessor.apvts.state;
    for (int i = 0; i < st.getNumProperties(); ++i)
    {
        const auto name = st.getPropertyName (i).toString();
        if (name.startsWith ("ui_")) ui->setProperty (name.substring (3), st.getProperty (name));
    }

    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty ("values", juce::var (values.get()));
    payload->setProperty ("ui", juce::var (ui.get()));
    payload->setProperty ("preset", audioProcessor.presets.getCurrentPresetName());
    emitToViews ("paramState", juce::var (payload.get()));
}

void SolutionAudioProcessorEditor::sendPresetList (const juce::String& message)
{
    juce::Array<juce::var> list;
    for (const auto& info : audioProcessor.presets.getAllPresets())
    {
        juce::DynamicObject::Ptr item = new juce::DynamicObject();
        item->setProperty ("name", info.name);
        item->setProperty ("tag", info.tag);
        item->setProperty ("factory", info.factory);
        list.add (juce::var (item.get()));
    }
    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty ("presets", list);
    payload->setProperty ("current", audioProcessor.presets.getCurrentPresetName());
    payload->setProperty ("folder", audioProcessor.presets.getFolder().getFullPathName());
    payload->setProperty ("message", message);
    emitToViews ("presetList", juce::var (payload.get()));
}

void SolutionAudioProcessorEditor::handlePresetCommand (const juce::var& data)
{
    auto* obj = data.getDynamicObject();
    if (obj == nullptr)
        return;
    const auto cmd  = obj->getProperty ("cmd").toString();
    const auto name = obj->getProperty ("name").toString();
    auto& pm = audioProcessor.presets;

    juce::String message;
    if (cmd == "load")
        message = pm.loadPreset (name) ? "Loaded " + name : "Couldn't load " + name;
    else if (cmd == "save")
        message = pm.savePreset (name, obj->getProperty ("tag").toString()) ? "Saved " + name : "Couldn't save " + name + " (factory names are reserved)";
    else if (cmd == "delete")
        message = pm.deletePreset (name) ? "Deleted " + name : "Couldn't delete " + name;
    else if (cmd == "reveal")
    {
        pm.getFolder().createDirectory();
        pm.getFolder().startAsProcess();
    }

    sendPresetList (message);
    sendParamState();
}

void SolutionAudioProcessorEditor::sendRackState()
{
    auto& bus = audioProcessor.getRackBus();
    juce::Array<juce::var> list;
    for (int i = 0; i < SolutionDSP::RackLinkBus::maxSlots; ++i)
    {
        const auto& slot = bus.slots[(size_t) i];
        if (! slot.active.load (std::memory_order_relaxed))
            continue;
        juce::DynamicObject::Ptr item = new juce::DynamicObject();
        item->setProperty ("self",   i == audioProcessor.getRackSlot());
        item->setProperty ("name",   slot.trackName);
        item->setProperty ("module", juce::String (SolutionDSP::moduleDisplayName (static_cast<SolutionDSP::ModuleId> (slot.moduleId.load()))));
        item->setProperty ("group",  slot.group.load());
        item->setProperty ("source", slot.duckSource.load());
        item->setProperty ("lufs",   slot.lufsShort.load());
        item->setProperty ("tp",     slot.truePeak.load());
        item->setProperty ("gr",     slot.gainReduct.load());
        item->setProperty ("slot",   i);
        item->setProperty ("az",     slot.posAz.load());
        item->setProperty ("el",     slot.posEl.load());
        item->setProperty ("dist",   slot.posDist.load());
        item->setProperty ("width",  slot.posWidth.load());
        item->setProperty ("tone",   slot.toneHz.load());
        item->setProperty ("creature", slot.creature.load());
        juce::Array<juce::var> bands;
        for (auto& bnd : slot.bandDb) bands.add (std::round (bnd.load() * 10.0f) / 10.0f);
        item->setProperty ("bands",  bands);
        item->setProperty ("unmask", slot.unmaskRole.load());
        item->setProperty ("dialogue", slot.dialogueSource.load());
        list.add (juce::var (item.get()));
    }
    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty ("instances", list);
    payload->setProperty ("suiteGlue", bus.suiteGlue.load());
    payload->setProperty ("selfSlot", audioProcessor.getRackSlot());
    payload->setProperty ("suiteTheme", audioProcessor.getRackBus().suiteTheme);
    payload->setProperty ("suiteThemeGen", audioProcessor.getRackBus().suiteThemeGen);
    payload->setProperty ("animAz", audioProcessor.animAz.load());
    payload->setProperty ("animEl", audioProcessor.animEl.load());
    payload->setProperty ("animDist", audioProcessor.animDist.load());
    {
        const int room = (int) audioProcessor.apvts.getRawParameterValue ("listen_room")->load();
        const auto& rooms = SolutionDSP::listeningRooms();
        if (room > 0 && room <= (int) rooms.size() && rooms[(size_t) room - 1].length > 0.0f)
        {
            const auto& r = rooms[(size_t) room - 1];
            juce::Array<juce::var> dims; dims.add (r.width); dims.add (r.length); dims.add (r.height);
            payload->setProperty ("roomDims", dims);
            payload->setProperty ("roomName", juce::String (r.name));
        }
    }
    {
        juce::Array<juce::var> path;
        for (const auto& pt : audioProcessor.getMotionPath()) { juce::Array<juce::var> a; a.add (pt.az); a.add (pt.el); a.add (pt.dist); path.add (a); }
        payload->setProperty ("path", path);
    }
    payload->setProperty ("glueGr", audioProcessor.glueGainReductionDb.load());
    payload->setProperty ("duckGr", audioProcessor.duckGainReductionDb.load());
    juce::Array<juce::var> dips;
    for (auto& d : audioProcessor.unmaskDipDb) dips.add (std::round (d.load() * 10.0f) / 10.0f);
    payload->setProperty ("unmaskDips", dips);
    emitToViews ("rack", juce::var (payload.get()));
}

SolutionAudioProcessorEditor::~SolutionAudioProcessorEditor()
{
    if (voice != nullptr) (*voice)->removeClient (this);     // the hub must never call a closed window
    audioProcessor.scopeTap.setEnabled (false);
    if (stageGestureOpen)
        for (const char* id : { "stage_azimuth", "stage_elevation", "stage_distance", "stage_width" })
            if (auto* q = audioProcessor.apvts.getParameter (id)) q->endChangeGesture();
}

void SolutionAudioProcessorEditor::handleStage (const juce::String& type, const juce::var& d)
{
    const int own = audioProcessor.getRackSlot();
    const int slot = d.hasProperty ("slot") ? (int) d.getProperty ("slot", own) : own;
    static constexpr const char* stageIds[] = { "stage_azimuth", "stage_elevation", "stage_distance", "stage_width" };

    if (type == "gesture" && (slot == own || own < 0))
    {
        // One gesture per drag so the DAW records clean automation.
        const bool begin = d.getProperty ("phase", "").toString() == "begin";
        if (begin == stageGestureOpen) return;
        for (const char* id : stageIds)
            if (auto* q = audioProcessor.apvts.getParameter (id)) begin ? q->beginChangeGesture() : q->endChangeGesture();
        stageGestureOpen = begin;
    }
    else if (type == "move")
    {
        static constexpr const char* keys[] = { "az", "el", "dist", "width" };
        for (int k = 0; k < 4; ++k)
        {
            if (! d.hasProperty (keys[k])) continue;
            const float v = (float) d.getProperty (keys[k], 0.0);
            if (slot == own || own < 0)
            {
                if (auto* q = audioProcessor.apvts.getParameter (stageIds[k]))
                {
                    const auto& range = q->getNormalisableRange();
                    const float n = q->convertTo0to1 (juce::jlimit (range.start, range.end, v));
                    if (stageGestureOpen) q->setValueNotifyingHost (n);
                    else { q->beginChangeGesture(); q->setValueNotifyingHost (n); q->endChangeGesture(); }
                }
            }
            else
                audioProcessor.requestParamOnSlot (slot, stageIds[k], v);
        }
    }
    else if (type == "path")
    {
        std::vector<SolutionDSP::MotionPoint> pts;
        if (auto* arr = d.getProperty ("points", {}).getArray())
            for (const auto& p : *arr)
                if (p.isArray() && p.size() >= 3)
                    pts.push_back ({ (float) p[0], (float) p[1], (float) p[2] });
        audioProcessor.setMotionPath (pts);
        if (pts.size() >= 2) audioProcessor.setParamReal ("motion_on", 1.0f);
    }
    else if (type == "unmask")
    {
        // One click on a clash: the chosen track becomes the priority, the other makes room.
        const int prio = (int) d.getProperty ("priority", -1), other = (int) d.getProperty ("other", -1);
        auto& bus = audioProcessor.getRackBus();
        if (! juce::isPositiveAndBelow (prio, SolutionDSP::RackLinkBus::maxSlots) || ! juce::isPositiveAndBelow (other, SolutionDSP::RackLinkBus::maxSlots)) return;
        int group = bus.slots[(size_t) prio].group.load();
        if (group <= 0) group = bus.slots[(size_t) other].group.load();
        if (group <= 0)
        {
            bool used[5] = {};
            for (const auto& sl : bus.slots) if (sl.active.load()) used[juce::jlimit (0, 4, sl.group.load())] = true;
            group = 1; while (group < 4 && used[group]) ++group;
        }
        audioProcessor.requestParamOnSlot (prio, "link_group", (float) group);
        audioProcessor.requestParamOnSlot (prio, "unmask_role", 1.0f);
        audioProcessor.requestParamOnSlot (other, "link_group", (float) group);
        audioProcessor.requestParamOnSlot (other, "unmask_role", 2.0f);
        audioProcessor.pollSessionRequests();
    }
}

void SolutionAudioProcessorEditor::sendScope()
{
    if (scopeL.empty()) { scopeL.assign (4096, 0.0f); scopeR.assign (4096, 0.0f); fftWork.assign (4096, 0.0f); }
    audioProcessor.scopeTap.drain ([this] (const float* l, const float* r, int n)
    {
        for (int i = 0; i < n; ++i) { scopeL[(size_t) scopeWrite] = l[i]; scopeR[(size_t) scopeWrite] = r[i]; scopeWrite = (scopeWrite + 1) % 4096; }
    });
    auto at = [this] (const std::vector<float>& v, int back) { return v[(size_t) ((scopeWrite - back + 4096 * 2) % 4096)]; };

    // Vectorscope: the latest 768 sample pairs (every 2nd), as (L, R).
    juce::Array<juce::var> pts;
    double sll = 0, srr = 0, slr = 0;
    for (int i = 768; i > 0; i -= 2)
    {
        const float l = at (scopeL, i), r = at (scopeR, i);
        pts.add (std::round (l * 1000.0f) / 1000.0f); pts.add (std::round (r * 1000.0f) / 1000.0f);
        sll += l * l; srr += r * r; slr += l * r;
    }
    // Spectrum: 2048-point FFT of the mid signal, 72 log-spaced bands 20 Hz - 20 kHz.
    std::fill (fftWork.begin(), fftWork.end(), 0.0f);
    for (int i = 0; i < 2048; ++i)
    {
        const float w = 0.5f * (1.0f - std::cos (juce::MathConstants<float>::twoPi * i / 2047.0f));
        fftWork[(size_t) i] = 0.5f * (at (scopeL, 2048 - i) + at (scopeR, 2048 - i)) * w;
    }
    scopeFft.performFrequencyOnlyForwardTransform (fftWork.data(), true);
    const double fs = juce::jmax (1.0, audioProcessor.getSampleRate());
    juce::Array<juce::var> spec;
    for (int b = 0; b < 72; ++b)
    {
        const double f0 = 20.0 * std::pow (1000.0, b / 72.0), f1 = 20.0 * std::pow (1000.0, (b + 1) / 72.0);
        const int k0 = juce::jlimit (1, 1023, (int) (f0 * 2048 / fs)), k1 = juce::jlimit (k0, 1023, (int) (f1 * 2048 / fs));
        float m = 0.0f; for (int k = k0; k <= k1; ++k) m = std::max (m, fftWork[(size_t) k]);
        spec.add (std::round (juce::Decibels::gainToDecibels (m / 512.0f, -90.0f) * 10.0f) / 10.0f);
    }
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("lr", pts);
    o->setProperty ("sr", audioProcessor.getSampleRate());       // the precision grid locks to the calibration tone
    o->setProperty ("step", 2);
    o->setProperty ("spec", spec);
    o->setProperty ("corr", (sll + srr) > 1e-9 ? slr / (std::sqrt (sll * srr) + 1e-12) : 1.0);
    emitToViews ("scope", juce::var (o.get()));
}

void SolutionAudioProcessorEditor::sendRoomResult()
{
    const auto& a = audioProcessor.getRoomAnalysis();
    auto toArray = [] (const auto& arr) { juce::Array<juce::var> out; for (float v : arr) out.add (std::round (v * 100.0f) / 100.0f); return out; };
    juce::Array<juce::var> bands;
    for (const auto& b : a.bands)
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("freq", b.freq); o->setProperty ("gain", b.gainDb); o->setProperty ("q", b.q);
        bands.add (juce::var (o.get()));
    }
    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty ("valid", a.valid);
    payload->setProperty ("message", a.message);
    payload->setProperty ("freqs", toArray (a.freqs));
    payload->setProperty ("measured", toArray (a.measuredDb));
    payload->setProperty ("correction", toArray (a.correctionDb));
    payload->setProperty ("result", toArray (a.resultDb));
    payload->setProperty ("bands", bands);

    juce::Array<juce::var> mics;
    for (const auto& m : SolutionDSP::builtInMicProfiles())
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("id", m.id); o->setProperty ("name", m.name);
        mics.add (juce::var (o.get()));
    }
    const auto& mic = audioProcessor.getMicProfile();
    payload->setProperty ("mics", mics);
    payload->setProperty ("micId", mic.id);
    payload->setProperty ("micName", mic.name);
    payload->setProperty ("fitLow", juce::jmax (25.0f, mic.reliableLowHz));
    payload->setProperty ("fitHigh", juce::jmin (5000.0f, mic.reliableHighHz));
    emitToViews ("roomResult", juce::var (payload.get()));
}

void SolutionAudioProcessorEditor::handleAssistantCommand (const juce::var& data)
{
    auto* obj = data.getDynamicObject();
    if (obj == nullptr) return;
    const auto cmd = obj->getProperty ("cmd").toString();
    if (cmd == "listen")       audioProcessor.assistantStartListening();
    else if (cmd == "stop")    audioProcessor.assistantStopListening();
    else if (cmd == "genre")   audioProcessor.assistantUseGenre (obj->getProperty ("name").toString());
    else if (cmd == "apply")   audioProcessor.assistantApply ((int) obj->getProperty ("index"));
    else if (cmd == "loadRef")
    {
        referenceChooser = std::make_unique<juce::FileChooser> ("Choose a reference track", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        referenceChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                       [this] (const juce::FileChooser& fc)
        {
            if (fc.getResult().existsAsFile())
                audioProcessor.assistantLoadReference (fc.getResult());
            sendAssistantState();
        });
    }
    sendAssistantState();
}

void SolutionAudioProcessorEditor::sendAssistantState()
{
    audioProcessor.assistantPoll();
    const auto& a = audioProcessor.assistant;
    auto profileVar = [] (const SolutionDSP::MixProfile& p)
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("valid", p.valid);
        o->setProperty ("name", p.name);
        o->setProperty ("lufs", p.lufs);
        o->setProperty ("plr", p.plr());
        o->setProperty ("width", p.widthDb());
        o->setProperty ("seconds", p.seconds);
        return juce::var (o.get());
    };
    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty ("listening", a.tap.isEnabled());
    payload->setProperty ("loading", a.loadingReference.load());
    payload->setProperty ("error", a.referenceError);
    payload->setProperty ("reference", profileVar (a.reference));
    payload->setProperty ("mix", profileVar (audioProcessor.assistantMixProfile()));
    juce::Array<juce::var> diff, freqs, suggestions, genres;
    for (int b = 0; b < SolutionDSP::MixProfile::numBands; ++b)
    {
        diff.add (std::round (a.comparison.diffDb[(size_t) b] * 10.0f) / 10.0f);
        freqs.add (SolutionDSP::MixProfile::bandCentre (b));
    }
    for (const auto& sgn : a.comparison.suggestions)
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("text", sgn.text);
        o->setProperty ("canApply", sgn.paramId.isNotEmpty());
        suggestions.add (juce::var (o.get()));
    }
    for (const auto& g : SolutionDSP::genreTargetNames()) genres.add (g);
    payload->setProperty ("diff", diff);
    payload->setProperty ("freqs", freqs);
    payload->setProperty ("suggestions", suggestions);
    payload->setProperty ("genres", genres);
    emitToViews ("assistantState", juce::var (payload.get()));
}

void SolutionAudioProcessorEditor::sendCharacterState (const juce::String& message)
{
    juce::Array<juce::var> models;
    for (const auto& m : SolutionDSP::NeuralCharacter::listModels()) models.add (m);
    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty ("models", models);
    payload->setProperty ("current", audioProcessor.getCharacterModelName());
    payload->setProperty ("folder", SolutionDSP::NeuralCharacter::getModelsFolder().getFullPathName());
    payload->setProperty ("message", message);
    const double trained = audioProcessor.character.getTrainedSampleRate(), host = audioProcessor.getSampleRate();
    payload->setProperty ("rateWarning", trained > 0.0 && host > 0.0 && std::abs (trained / host - 1.0) > 0.01
                                             ? "This model was trained at " + juce::String (trained / 1000.0, 1) + " kHz; your session runs at "
                                               + juce::String (host / 1000.0, 1) + " kHz, so its tone may differ slightly."
                                             : juce::String());
    emitToViews ("characterState", juce::var (payload.get()));
}

void SolutionAudioProcessorEditor::sendRemoteState (const juce::String& error)
{
    auto* r = audioProcessor.getRemote();
    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    const bool running = r != nullptr && r->isRunning();
    payload->setProperty ("running", running);
    payload->setProperty ("url", running ? r->getUrl() : juce::String());
    payload->setProperty ("code", running ? r->getPairingCode() : juce::String());
    payload->setProperty ("devices", running ? r->getConnectedDevices() : 0);
    payload->setProperty ("error", error);
    emitToViews ("remoteState", juce::var (payload.get()));
}

void SolutionAudioProcessorEditor::sendAiState()
{
    emitToViews ("cuesState", audioProcessor.cuesJson());
    emitToViews ("moodState", audioProcessor.moodJson());
    emitMeta ({});
}

void SolutionAudioProcessorEditor::emitMeta (const juce::String& message)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("points", audioProcessor.trajectoryPoints());
    o->setProperty ("seconds", audioProcessor.trajectoryPoints() / SolutionCinema::Trajectory::rate);
    if (message.isNotEmpty()) o->setProperty ("message", message);
    emitToViews ("metaState", juce::var (o.get()));
}

void SolutionAudioProcessorEditor::sendHistory()
{
    lastHistoryGen = audioProcessor.getHistoryGen();
    emitToViews ("historyState", audioProcessor.historyJson());
}

// Every other instance's recent edits (the Rack tab of the History panel).
void SolutionAudioProcessorEditor::sendRackHistory()
{
    auto& bus = audioProcessor.getRackBus();
    juce::Array<juce::var> list;
    for (int i = 0; i < SolutionDSP::RackLinkBus::maxSlots; ++i)
    {
        const auto& slot = bus.slots[(size_t) i];
        if (! slot.active.load() || i == audioProcessor.getRackSlot() || slot.historyText.isEmpty()) continue;
        SolutionHistory::EditHistory h; h.fromString (slot.historyText);
        const auto who = juce::String (SolutionDSP::moduleDisplayName ((SolutionDSP::ModuleId) slot.moduleId.load()))
                         + (slot.trackName.isNotEmpty() ? " (" + slot.trackName + ")" : juce::String());
        for (auto& e : h.getEntries())
        {
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            o->setProperty ("slot", i); o->setProperty ("who", who); o->setProperty ("seq", e.seq); o->setProperty ("time", (double) e.timeMs);
            juce::StringArray names;
            for (auto& c : e.changes) if (auto* q = audioProcessor.apvts.getParameter (c.id)) names.add (q->getName (30) + " " + q->getText (c.from, 12) + " → " + q->getText (c.to, 12));
            o->setProperty ("label", e.label.isNotEmpty() ? e.label : names.joinIntoString (", "));
            o->setProperty ("detail", names.joinIntoString (", "));
            list.add (juce::var (o.get()));
        }
    }
    emitToViews ("rackHistoryState", list);
}

// Undo one edit in another instance: every module shares the same parameter layout, so its values convert here.
void SolutionAudioProcessorEditor::rackRevert (int slot, int seq)
{
    auto& bus = audioProcessor.getRackBus();
    if (! juce::isPositiveAndBelow (slot, SolutionDSP::RackLinkBus::maxSlots)) return;
    SolutionHistory::EditHistory h; h.fromString (bus.slots[(size_t) slot].historyText);
    for (auto& [id, norm] : h.revertOne (seq))
        if (auto* q = audioProcessor.apvts.getParameter (id))
            audioProcessor.requestParamOnSlot (slot, id, q->convertFrom0to1 (norm));
}

void SolutionAudioProcessorEditor::timerCallback()
{
    if (audioProcessor.getHistoryGen() != lastHistoryGen) sendHistory();
    if (audioProcessor.getCurveGen() != lastCurveGen && ! audioProcessor.getResponseCurve().isVoid()) { lastCurveGen = audioProcessor.getCurveGen(); emitToViews ("responseCurve", audioProcessor.getResponseCurve()); }
    if (aiPanelOpen && frameCounter % 12 == 0) sendAiState();
    if (audioProcessor.toneAnalysisRunning() && frameCounter % 6 == 0) emitToViews ("toneProgress", (double) audioProcessor.toneAnalysisProgress());
    if (auto res = audioProcessor.takeToneAnalysisResult(); ! res.isVoid()) emitToViews ("toneAnalysis", res);
    audioProcessor.pollAutoGainStage();          // sets the trims when the 3 s listen is done
    if (frameCounter % 120 == 60 && audioProcessor.getRemote() != nullptr && audioProcessor.getRemote()->isRunning())
        sendRemoteState();

    if (frameCounter % 120 == 0)
        audioProcessor.character.housekeeping();   // free models the audio thread has swapped out

    if (frameCounter % 60 == 30)
    {
        const bool busy = audioProcessor.assistant.tap.isEnabled() || audioProcessor.assistant.loadingReference.load();
        if (busy || assistantWasBusy)
            sendAssistantState();
        assistantWasBusy = busy;
    }

    if (audioProcessor.pollRoomAnalysis())
        sendRoomResult();

    if (audioProcessor.consumeParamsChanged())
        sendParamState();
    if (frameCounter % 15 == 0) updateDiagLog();          // keep the event log even while the panel is closed
    if (frameCounter % 60 == 45)                          // Skin Studio: pick up skins sent from any phone, in every plugin
    {
        const auto meta = SolutionVendor::userFolder ("Skins").getChildFile ("custom_skins.json");
        const auto stamp = meta.getLastModificationTime().toMilliseconds();
        if (stamp != customSkinsStamp) { customSkinsStamp = stamp; emitToViews ("customSkins", juce::JSON::parse (meta)); }
        if (audioProcessor.pendingSkin.isNotEmpty()) { emitToViews ("applySkin", juce::var (audioProcessor.pendingSkin)); audioProcessor.pendingSkin.clear(); }
    }
    if (systemMonitor != nullptr && frameCounter % 30 == 0) sendDiagnostics();
    if (++frameCounter % 6 == 0)      // 10 times a second (the 3D stage follows every track)
        sendRackState();
    if (frameCounter % 2 == 0)        // real scopes at 30 fps
        sendScope();

    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty ("module",        juce::String (SOLUTION_MODULE_NAME));
    payload->setProperty ("skinsEnabled",  skinsAvailable);
    payload->setProperty ("rmsL",          audioProcessor.rmsLeft.load (std::memory_order_relaxed));
    payload->setProperty ("rmsR",          audioProcessor.rmsRight.load (std::memory_order_relaxed));
    payload->setProperty ("phase",         audioProcessor.phaseCorrelation.load (std::memory_order_relaxed));
    payload->setProperty ("peak",          audioProcessor.peakDb.load (std::memory_order_relaxed));
    payload->setProperty ("lufs",          audioProcessor.loudness.momentary.load (std::memory_order_relaxed));
    payload->setProperty ("lufsShort",     audioProcessor.loudness.shortTerm.load (std::memory_order_relaxed));
    payload->setProperty ("lufsInt",       audioProcessor.loudness.integrated.load (std::memory_order_relaxed));
    payload->setProperty ("truePeak",      audioProcessor.loudness.truePeakDb.load (std::memory_order_relaxed));
    payload->setProperty ("limiterGr",     audioProcessor.limiterGainReductionDb.load (std::memory_order_relaxed));
    payload->setProperty ("rackLinks",     audioProcessor.getRackLinkCount());
    payload->setProperty ("capturing",     audioProcessor.isRoomCapturing());
    payload->setProperty ("captureProgress", audioProcessor.getRoomCaptureProgress());
    payload->setProperty ("analysing",     audioProcessor.isRoomAnalysing());
    payload->setProperty ("notchAuto",     audioProcessor.getAutoNotchDepth());
    payload->setProperty ("monitorMask",   audioProcessor.getMonitorMask());
    payload->setProperty ("diagClips",     (int) audioProcessor.diag.inputClips.load());
    payload->setProperty ("diagBad",       (int) audioProcessor.diag.badSamples.load());
    payload->setProperty ("diagPhase",     (int) audioProcessor.diag.phaseEvents.load());
    payload->setProperty ("diagOverloads", (int) audioProcessor.diag.overloads.load());
    payload->setProperty ("diagDc",        (int) audioProcessor.diag.dcEvents.load());
    payload->setProperty ("vanilla",       audioProcessor.vanillaLoaded.load());
    payload->setProperty ("learnedCc",     audioProcessor.getLastLearnedCc());
    payload->setProperty ("learnSeq",      audioProcessor.learnSeq.load());
    if (auto* off = audioProcessor.getOffload(); off != nullptr && audioProcessor.isOffloaded())
    {
        payload->setProperty ("offload", true);
        payload->setProperty ("offloadNode", off->getNodeInfo().getProperty ("name", "node"));
        payload->setProperty ("offloadLatencyMs", off->getLatency() * 1000.0 / std::max (1.0, audioProcessor.getSampleRate()));
        payload->setProperty ("offloadLate", off->getLateBlocks());
    }
    else payload->setProperty ("offload", false);
    if (audioProcessor.offloadNotice.isNotEmpty()) { emitToViews ("roomNotice", juce::var (audioProcessor.offloadNotice)); audioProcessor.offloadNotice.clear(); }
    {
        auto& ht = *audioProcessor.headTracker;
        payload->setProperty ("headOn", ht.enabled.load());
        payload->setProperty ("headFresh", ht.isFresh());
        payload->setProperty ("headYaw", ht.yaw());
        payload->setProperty ("headPitch", ht.pitch());
        payload->setProperty ("headSource", ht.source());
        payload->setProperty ("oscPort", ht.getOscPort());
    }
    payload->setProperty ("voice",         audioProcessor.voiceConfidence.load());
    payload->setProperty ("dialogueDuckDb", audioProcessor.dialogueDuckDb.load());
    payload->setProperty ("dialogueSources", audioProcessor.dialogueSources.load());
    payload->setProperty ("anchorDist", std::round (audioProcessor.anchorDistPct.load() * 10.0f) / 10.0f);
    payload->setProperty ("anchorCrest", std::round (audioProcessor.anchorCrestLost.load() * 10.0f) / 10.0f);
    payload->setProperty ("anchorVerdict", audioProcessor.anchorVerdict.load());
    payload->setProperty ("aetherGrains", audioProcessor.aetherGrains.load());
    payload->setProperty ("aetherDuck", std::round (audioProcessor.aetherDuck.load() * 100.0f) / 100.0f);
    payload->setProperty ("bpm", audioProcessor.hostBpm.load());
    payload->setProperty ("bunkerShelf", std::round (audioProcessor.bunkerShelfDb.load() * 10.0f) / 10.0f);
    payload->setProperty ("autoSafe", std::round (audioProcessor.autoSafeHolding.load() * 100.0f));
    payload->setProperty ("dialogueCarveHz", std::round (audioProcessor.dialogueCarveHz.load()));
    payload->setProperty ("latency",       audioProcessor.getLatencySamples());
    payload->setProperty ("dqsLevel",      audioProcessor.dqsLevel.load());
    payload->setProperty ("dqsSuiteLoad",  audioProcessor.dqsSuiteLoad.load());
    payload->setProperty ("dqsOwnLoad",    audioProcessor.dqsOwnLoad.load());
    payload->setProperty ("sessionMonitors", audioProcessor.getSessionMonitorCount());
    payload->setProperty ("abMatch",       audioProcessor.abMatchDb.load());
    payload->setProperty ("smoothDb",      audioProcessor.smoothDb.load());
    payload->setProperty ("guardDb",       audioProcessor.guardDb.load());
    payload->setProperty ("streamGain",    audioProcessor.streamGainDb.load());
    payload->setProperty ("autoStage",     audioProcessor.getAutoGainStageState());
    {
        juce::Array<juce::var> services;
        const float lufsI = audioProcessor.loudness.integrated.load(), tp = audioProcessor.loudness.truePeakDb.load();
        for (const auto& svc : SolutionDSP::streamingServices())
        {
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            o->setProperty ("name", juce::String (svc.name));
            o->setProperty ("target", svc.targetLufs);
            o->setProperty ("gain", SolutionDSP::streamingGainDb (svc, lufsI, tp));
            services.add (juce::var (o.get()));
        }
        payload->setProperty ("streaming", services);
    }
    payload->setProperty ("cvLfo",         audioProcessor.getCvLfo());
    payload->setProperty ("envFollower",   audioProcessor.getEnvFollower());
    payload->setProperty ("learnedCc",     audioProcessor.getLastLearnedCc());
    payload->setProperty ("learnSeq",      audioProcessor.learnSeq.load());

    emitToViews ("telemetry", juce::var (payload.get()));
}


//==============================================================================
// Diagnostic Utility
juce::var SolutionAudioProcessorEditor::diagPluginList()
{
    return SolutionDiag::describeRackSlots (audioProcessor.getRackBus(), audioProcessor.getRackSlot(), audioProcessor.getSampleRate());
}

void SolutionAudioProcessorEditor::updateDiagLog()
{
    auto add = [this] (const juce::String& level, const juce::String& text)
    {
        juce::DynamicObject::Ptr e = new juce::DynamicObject();
        e->setProperty ("time", juce::Time::getCurrentTime().toString (false, true, true, true));
        e->setProperty ("level", level);
        e->setProperty ("text", text);
        diagLog.insert (0, juce::var (e.get()));
        if (diagLog.size() > 200) diagLog.removeLast();
    };
    auto& d = audioProcessor.diag;
    const auto clips = d.inputClips.load(), bad = d.badSamples.load(), phase = d.phaseEvents.load();
    const int panic = audioProcessor.panicCount.load();
    if (clips > lastClips) add ("warn", "Input clipping on this track (" + juce::String ((int) (clips - lastClips)) + " blocks at or over 0 dBFS)");
    if (bad > lastBad)     add ("error", "Broken samples (NaN/Inf) arrived on this track and were silenced");
    if (phase > lastPhase) add ("warn", "Phase cancellation on this track: left and right are fighting (correlation below -0.3)");
    const auto dcEv = d.dcEvents.load();
    if (dcEv > lastDc) add ("info", "DC offset arrived on this track's input - DC Guard removed it"
                                    + juce::String (audioProcessor.apvts.getRawParameterValue ("dc_guard")->load() > 0.5f ? "" : " (DC Guard is OFF!)"));
    lastDc = dcEv;
    if (panic > lastPanic) add ("info", "Panic reset: every tail, delay and filter on this track was flushed");
    lastClips = clips; lastBad = bad; lastPhase = phase; lastPanic = panic;

    auto& bus = audioProcessor.getRackBus();
    for (size_t i = 0; i < bus.slots.size() && i < lastSlotCounts.size(); ++i)
    {
        auto& sl = bus.slots[i]; auto& last = lastSlotCounts[i];
        if (! sl.active.load()) { last = {}; continue; }
        const auto ov = sl.overloads.load(), er = sl.errors.load();
        const auto who = juce::String (SolutionDSP::moduleDisplayName (static_cast<SolutionDSP::ModuleId> (sl.moduleId.load())))
                       + (sl.trackName.isNotEmpty() ? " (" + sl.trackName + ")" : juce::String());
        if (last.seen && ov > last.overloads)
            add ("error", who + ": " + juce::String ((int) (ov - last.overloads)) + " audio block(s) ran past real time - raise the buffer size or let DQS step down");
        if (last.seen && er > last.errors && (int) i != audioProcessor.getRackSlot())
            add ("warn", who + ": " + juce::String ((int) (er - last.errors)) + " new signal event(s) (clipping / phase / broken samples)");
        last = { ov, er, true };
    }
}

void SolutionAudioProcessorEditor::sendDiagnostics()
{
    if (systemMonitor == nullptr) return;
    const auto s = (*systemMonitor)->getSnapshot();
    juce::var payload = SolutionDiag::snapshotToVar (s);      // keeps the object alive
    auto* o = payload.getDynamicObject();
    o->setProperty ("plugins", diagPluginList());
    o->setProperty ("log", diagLog);
    o->setProperty ("host", SolutionVoice::hostIdentity().trim());
    o->setProperty ("sampleRate", audioProcessor.getSampleRate());
    o->setProperty ("blockSize", audioProcessor.getBlockSize());
    emitToViews ("diagData", payload);
}

void SolutionAudioProcessorEditor::exportDiagnostics (const juce::String& format)
{
    if (systemMonitor == nullptr) systemMonitor = std::make_unique<juce::SharedResourcePointer<SolutionDiag::SystemMonitor>>();
    const auto s = (*systemMonitor)->getSnapshot();
    const bool xml = format == "xml";
    const auto text = xml ? SolutionDiag::makeXmlReport (s, diagPluginList(), juce::var (diagLog))
                          : SolutionDiag::makeCsvReport (s, diagPluginList(), juce::var (diagLog));
    const auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("The Solution").getChildFile ("Diagnostics");
    dir.createDirectory();
    const auto file = dir.getChildFile ("Solution-Diagnostics-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + (xml ? ".xml" : ".csv"));
    const bool ok = file.replaceWithText (text);
    if (ok) file.revealToUser();
    emitToViews ("diagExported", ok ? juce::var (file.getFullPathName()) : juce::var ("Couldn't write to " + dir.getFullPathName()));
}


//==============================================================================
// Voice control
std::vector<SolutionVoice::ParamInfo> SolutionAudioProcessorEditor::voiceParamTable() const
{
    std::vector<SolutionVoice::ParamInfo> out;
    for (auto* p : audioProcessor.getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            SolutionVoice::ParamInfo pi;
            pi.id = r->getParameterID(); pi.name = r->getName (64);
            pi.min = r->getNormalisableRange().start; pi.max = r->getNormalisableRange().end;
            pi.isBool = dynamic_cast<juce::AudioParameterBool*> (r) != nullptr;
            if (auto* ch = dynamic_cast<juce::AudioParameterChoice*> (r)) { pi.isChoice = true; pi.choices = ch->choices; }
            // shown as % in the UI though stored 0-1 (or 0-2)
            for (auto* id : { "stage_width", "global_mix", "stage_depth", "glue_amount", "room_match_mix", "ultra_wide", "smooth_amount", "phone_bass",
                              "char_amount", "room_correct_amount", "cv_mod_depth", "regen_throttle" })
                if (pi.id == id) pi.percentUnit = true;
            out.push_back (pi);
        }
    return out;
}

void SolutionAudioProcessorEditor::handleVoice (const juce::var& data)
{
    const auto cmd = data.getProperty ("cmd", {}).toString();
    if (cmd == "on")
    {
        if (voice == nullptr) voice = std::make_unique<juce::SharedResourcePointer<SolutionVoice::VoiceHub>>();
        auto& hub = **voice;
        hub.setParams (voiceParamTable());
        hub.setInput (audioProcessor.getUiProperty ("voiceMic"));
        hub.setWakeWordRequired (audioProcessor.getUiProperty ("voiceWake") != "0");
        SolutionVoice::VoiceHub::Client c;
        c.slot = audioProcessor.getRackSlot();
        c.execute = [this] (const SolutionVoice::Command& k) { return executeVoice (k); };
        c.notify = [this] (const juce::var& v) { emitToViews ("voiceHeard", v); };
        const auto why = hub.addClient (this, c);
        hub.noteActive (audioProcessor.getRackSlot());
        emitToViews ("voiceState", juce::var (why.isEmpty() ? juce::String ("listening") : why));
        if (why.isNotEmpty()) voice.reset();
    }
    else if (cmd == "off")
    {
        if (voice != nullptr) (*voice)->removeClient (this);
        voice.reset();
        emitToViews ("voiceState", juce::var ("off"));
    }
    else if (cmd == "wake")
    {
        const bool on = (bool) data.getProperty ("value", true);
        audioProcessor.setUiProperty ("voiceWake", on ? "1" : "0");
        if (voice != nullptr) (*voice)->setWakeWordRequired (on);
    }
    else if (cmd == "keys")
    {
        audioProcessor.setUiProperty (voiceKeysProp(), juce::JSON::toString (data.getProperty ("value", {}), true));
    }
    else if (cmd == "defaults")
    {
        const auto k = SolutionVoice::defaultTransportKeys (SolutionVoice::hostIdentity());
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("host", SolutionVoice::hostIdentity().trim());
        o->setProperty ("play", k.play); o->setProperty ("stop", k.stop); o->setProperty ("record", k.record); o->setProperty ("start", k.start); o->setProperty ("loop", k.loop);
        o->setProperty ("saved", juce::JSON::parse (audioProcessor.getUiProperty (voiceKeysProp())));
        o->setProperty ("wake", audioProcessor.getUiProperty ("voiceWake") != "0");
        emitToViews ("voiceDefaults", juce::var (o.get()));
    }
    else if (cmd == "inputs")
    {
        juce::Array<juce::var> list; for (auto& n : SolutionVoice::SpeechInput::listInputs()) list.add (n);
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("list", list); o->setProperty ("current", audioProcessor.getUiProperty ("voiceMic"));
        emitToViews ("voiceInputs", juce::var (o.get()));
    }
    else if (cmd == "input")
    {
        const auto name = data.getProperty ("name", {}).toString();
        audioProcessor.setUiProperty ("voiceMic", name);
        if (voice != nullptr) { const auto why = (*voice)->setInput (name); if (why.isNotEmpty()) emitToViews ("voiceState", juce::var (why)); }
    }
    else if (cmd == "try")       // typed test phrase from the Voice panel
    {
        if (voice != nullptr) (*voice)->simulate (data.getProperty ("text", {}).toString());
    }
}

juce::String SolutionAudioProcessorEditor::executeVoice (const SolutionVoice::Command& c)
{
    using K = SolutionVoice::Command;
    const int own = audioProcessor.getRackSlot();
    const bool forMe = c.slot < 0 || c.slot == own;
    auto uiAction = [this] (const juce::String& action, float value)
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("action", action); o->setProperty ("value", value);
        emitToViews ("voiceUi", juce::var (o.get()));
        return juce::String();
    };
    switch (c.kind)
    {
        case K::SetParam:
        case K::NudgeParam:
        {
            auto* q = audioProcessor.apvts.getParameter (c.paramId);
            if (q == nullptr) return "unknown control";
            const auto range = q->getNormalisableRange();
            float v = c.value;
            if (c.kind == K::NudgeParam)
            {
                if (! forMe) return "say a value for other tracks, e.g. '... 80 percent'";
                v = range.convertFrom0to1 (q->getValue()) + c.value * (range.end - range.start);
            }
            else if (v < -0.5f && (dynamic_cast<juce::AudioParameterBool*> (q) != nullptr || dynamic_cast<juce::AudioParameterChoice*> (q) != nullptr))
            {
                if (! forMe) v = range.end;                                  // can't read another track's switch: "toggle" = on
                else if (auto* ch = dynamic_cast<juce::AudioParameterChoice*> (q)) v = (float) ((ch->getIndex() + 1) % ch->choices.size());
                else v = q->getValue() > 0.5f ? range.start : range.end;
            }
            v = juce::jlimit (range.start, range.end, v);
            if (forMe) audioProcessor.setParamReal (c.paramId, v);
            else if (! audioProcessor.requestParamOnSlot (c.slot, c.paramId, v)) return "that track didn't answer";
            return {};
        }
        case K::Transport:
        {
            const bool playing = audioProcessor.hostPlaying.load();
            if ((c.action == "play" && playing) || (c.action == "stop" && ! playing)) return {};   // already there
            const auto keys = SolutionVoice::defaultTransportKeys (SolutionVoice::hostIdentity());
            const auto saved = juce::JSON::parse (audioProcessor.getUiProperty (voiceKeysProp()));
            const auto custom = saved.getProperty (c.action, {}).toString();
            const auto key = custom.isNotEmpty() ? custom
                           : c.action == "play" ? keys.play : c.action == "stop" ? keys.stop : c.action == "record" ? keys.record : c.action == "start" ? keys.start : keys.loop;
            juce::String err;
            if (! SolutionVoice::pressHostShortcut (key, getPeer() != nullptr ? getPeer()->getNativeHandle() : nullptr, err)) return err;
            return {};
        }
        case K::Panic:
            if (c.everywhere) audioProcessor.panicResetEverywhere();
            else if (forMe) audioProcessor.panicReset();
            else audioProcessor.getRackBus().slots[(size_t) c.slot].panicRequest = true;
            return {};
        case K::Ui:
            if (c.action == "monitorsOff") { audioProcessor.turnOffMonitoringEverywhere(); return {}; }
            if (! forMe) return "open that plugin's window to do that";
            return uiAction (c.action, c.value);
        case K::Macro:
            if (! forMe) return "open that plugin's window to use its snapshots";
            return uiAction (c.action == "store" ? "macroStore" : "macroRecall", c.value);
        case K::None: break;
    }
    return "not a command I know";
}
