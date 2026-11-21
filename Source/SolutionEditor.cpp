#include "SolutionProcessor.h"
#include "SolutionEditor.h"
#include "BinaryData.h"

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
   #if JUCE_MAC
    const juce::String subPath ("Application Support/Fast Ray Media/The Solution/Skins");
   #else
    const juce::String subPath ("Fast Ray Media/The Solution/Skins");
   #endif

    // Per-user install first, then the machine-wide one
    // (Windows: %APPDATA% then %ProgramData%; macOS: ~/Library then /Library).
    for (auto location : { juce::File::userApplicationDataDirectory, juce::File::commonApplicationDataDirectory })
    {
        const auto dir = juce::File::getSpecialLocation (location).getChildFile (subPath);
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
                   if (auto* param = audioProcessor.apvts.getParameter (paramId))
                   {
                       audioProcessor.undoManager.beginNewTransaction (paramId);
                       param->setValueNotifyingHost (param->convertTo0to1 (value));
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

juce::String SolutionAudioProcessorEditor::baseUrl() const
{
    const auto escapedName = juce::URL::addEscapeChars (juce::String (SOLUTION_MODULE_NAME), true);
    return juce::WebBrowserComponent::getResourceProviderRoot() + "?module=" + escapedName + "&skins=" + juce::String (skinsAvailable ? "1" : "0");
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

void SolutionAudioProcessorEditor::sendParamState()
{
    juce::DynamicObject::Ptr values = new juce::DynamicObject();
    for (auto* p : audioProcessor.getParameters())
        if (auto* param = dynamic_cast<juce::RangedAudioParameter*> (p))
            values->setProperty (param->getParameterID(), param->convertFrom0to1 (param->getValue()));

    juce::DynamicObject::Ptr ui = new juce::DynamicObject();
    ui->setProperty ("skin", audioProcessor.getUiProperty ("skin"));

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
        list.add (juce::var (item.get()));
    }
    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty ("instances", list);
    payload->setProperty ("suiteGlue", bus.suiteGlue.load());
    payload->setProperty ("selfSlot", audioProcessor.getRackSlot());
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

void SolutionAudioProcessorEditor::timerCallback()
{
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

    emitToViews ("telemetry", juce::var (payload.get()));
}
