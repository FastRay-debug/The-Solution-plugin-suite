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

SolutionAudioProcessorEditor::SolutionAudioProcessorEditor (SolutionAudioProcessor& p)
    : AudioProcessorEditor (&p),
      audioProcessor (p),
      sharedSkinsDir (findSharedSkinsFolder()),
      skinsAvailable (SOLUTION_BUNDLE_SKINS != 0 || sharedSkinsDir.isDirectory()),
      webView (juce::WebBrowserComponent::Options{}
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
                               webView.emitEventIfBrowserIsVisible ("roomNotice", juce::var ("That file doesn't look like a mic calibration file (expected lines of: frequency dB)."));
                           sendRoomResult();
                       });
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
                   .withEventListener ("presetCommand", [this] (const juce::var& data) { handlePresetCommand (data); }))
{
    addAndMakeVisible (webView);
    const auto escapedName = juce::URL::addEscapeChars (juce::String (SOLUTION_MODULE_NAME), true);
    const auto skinsFlag   = juce::String (skinsAvailable ? "1" : "0");
    webView.goToURL (juce::WebBrowserComponent::getResourceProviderRoot() + "?module=" + escapedName + "&skins=" + skinsFlag);

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
    webView.emitEventIfBrowserIsVisible ("paramState", juce::var (payload.get()));
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
    webView.emitEventIfBrowserIsVisible ("presetList", juce::var (payload.get()));
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
        list.add (juce::var (item.get()));
    }
    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty ("instances", list);
    payload->setProperty ("suiteGlue", bus.suiteGlue.load());
    payload->setProperty ("glueGr", audioProcessor.glueGainReductionDb.load());
    payload->setProperty ("duckGr", audioProcessor.duckGainReductionDb.load());
    webView.emitEventIfBrowserIsVisible ("rack", juce::var (payload.get()));
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
    webView.emitEventIfBrowserIsVisible ("roomResult", juce::var (payload.get()));
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
    webView.emitEventIfBrowserIsVisible ("assistantState", juce::var (payload.get()));
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
    webView.emitEventIfBrowserIsVisible ("characterState", juce::var (payload.get()));
}

void SolutionAudioProcessorEditor::timerCallback()
{
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
    if (++frameCounter % 12 == 0)     // 5 times a second
        sendRackState();

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
    payload->setProperty ("cvLfo",         audioProcessor.getCvLfo());
    payload->setProperty ("envFollower",   audioProcessor.getEnvFollower());
    payload->setProperty ("learnedCc",     audioProcessor.getLastLearnedCc());

    webView.emitEventIfBrowserIsVisible ("telemetry", juce::var (payload.get()));
}
