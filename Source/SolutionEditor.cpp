#include "SolutionProcessor.h"
#include "SolutionEditor.h"
#include "BinaryData.h"

#ifndef SOLUTION_BUNDLE_SKINS
#define SOLUTION_BUNDLE_SKINS 1
#endif

namespace
{
    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url)
    {
        const auto cleanUrl = url.upToFirstOccurrenceOf ("?", false, false);
        if (cleanUrl == "/" || cleanUrl == "/index.html" || cleanUrl.isEmpty())
        {
            juce::WebBrowserComponent::Resource resource;
            resource.data.assign (reinterpret_cast<const std::byte*> (BinaryData::index_html),
                                  reinterpret_cast<const std::byte*> (BinaryData::index_html) + BinaryData::index_htmlSize);
            resource.mimeType = "text/html";
            return resource;
        }

        if (cleanUrl.startsWith ("/skins/") && cleanUrl.endsWith (".jpg"))
        {
            const auto baseName = cleanUrl.fromLastOccurrenceOf ("/", false, false).upToFirstOccurrenceOf (".", false, false);
            const auto resName  = baseName + "_jpg";
            int size = 0;
            if (const char* ptr = BinaryData::getNamedResource (resName.toRawUTF8(), size))
            {
                juce::WebBrowserComponent::Resource resource;
                resource.data.assign (reinterpret_cast<const std::byte*> (ptr),
                                      reinterpret_cast<const std::byte*> (ptr) + size);
                resource.mimeType = "image/jpeg";
                return resource;
            }
        }
        return std::nullopt;
    }
}

SolutionAudioProcessorEditor::SolutionAudioProcessorEditor (SolutionAudioProcessor& p)
    : AudioProcessorEditor (&p),
      audioProcessor (p),
      webView (juce::WebBrowserComponent::Options{}
                   #if JUCE_WINDOWS
                   .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
                   .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2{}
                                                .withUserDataFolder (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("TheSolutionWebView2")))
                   #endif
                   .withNativeIntegrationEnabled()
                   .withResourceProvider ([] (const auto& url) { return getResource (url); })
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
                   }))
{
    addAndMakeVisible (webView);
    const auto escapedName = juce::URL::addEscapeChars (juce::String (SOLUTION_MODULE_NAME), true);
    const auto skinsFlag   = juce::String (SOLUTION_BUNDLE_SKINS != 0 ? "1" : "0");
    webView.goToURL (juce::WebBrowserComponent::getResourceProviderRoot() + "?module=" + escapedName + "&skins=" + skinsFlag);

    setSize (1080, 640);
    setResizable (true, true);
    startTimerHz (60);
}

void SolutionAudioProcessorEditor::resized()
{
    webView.setBounds (getLocalBounds());
}

void SolutionAudioProcessorEditor::timerCallback()
{
    juce::DynamicObject::Ptr payload = new juce::DynamicObject();
    payload->setProperty ("module",        juce::String (SOLUTION_MODULE_NAME));
    payload->setProperty ("skinsEnabled",  SOLUTION_BUNDLE_SKINS != 0);
    payload->setProperty ("rmsL",          audioProcessor.rmsLeft.load (std::memory_order_relaxed));
    payload->setProperty ("rmsR",          audioProcessor.rmsRight.load (std::memory_order_relaxed));
    payload->setProperty ("phase",         audioProcessor.phaseCorrelation.load (std::memory_order_relaxed));
    payload->setProperty ("peak",          audioProcessor.peakDb.load (std::memory_order_relaxed));
    payload->setProperty ("lufs",          audioProcessor.lufsMomentary.load (std::memory_order_relaxed));
    payload->setProperty ("rackLinks",     audioProcessor.getRackLinkCount());
    payload->setProperty ("capturing",     audioProcessor.isRoomCapturing());
    payload->setProperty ("capturedHz",    audioProcessor.getCapturedModeHz());
    payload->setProperty ("cvLfo",         audioProcessor.getCvLfo());
    payload->setProperty ("envFollower",   audioProcessor.getEnvFollower());
    payload->setProperty ("learnedCc",     audioProcessor.getLastLearnedCc());

    webView.emitEventIfBrowserIsVisible ("telemetry", juce::var (payload.get()));
}
