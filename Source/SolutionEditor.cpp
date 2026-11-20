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
    const juce::String subPath ("Application Support/FastRayMedia/The Solution/Skins");
   #else
    const juce::String subPath ("FastRayMedia/The Solution/Skins");
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
                   })
                   .withEventListener ("setUiProp", [this] (const juce::var& data)
                   {
                       if (auto* obj = data.getDynamicObject())
                           audioProcessor.setUiProperty (obj->getProperty ("key").toString(), obj->getProperty ("value").toString());
                   })
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

void SolutionAudioProcessorEditor::timerCallback()
{
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
    payload->setProperty ("capturedHz",    audioProcessor.getCapturedModeHz());
    payload->setProperty ("cvLfo",         audioProcessor.getCvLfo());
    payload->setProperty ("envFollower",   audioProcessor.getEnvFollower());
    payload->setProperty ("learnedCc",     audioProcessor.getLastLearnedCc());

    webView.emitEventIfBrowserIsVisible ("telemetry", juce::var (payload.get()));
}
