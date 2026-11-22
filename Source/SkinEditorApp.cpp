// Billet Audio Skin Editor - its own program (installed with the Skins pack, launchable from every plugin).
// Make a skin from any photo: frame it, adjust it, pick (or auto-pick) the accent colours, preview the
// plugin's controls on top, save. Saved skins land in the shared Skins folder, so every open Solution
// plugin offers them immediately. Manage your skins and share them as .solutionskin packs.
#include <juce_gui_extra/juce_gui_extra.h>
#include "SkinStore.h"
#include "SkinEditorData.h"

namespace
{
    juce::WebBrowserComponent::Resource makeResource (const void* data, size_t size, const char* mime)
    {
        juce::WebBrowserComponent::Resource r;
        const auto* b = static_cast<const std::byte*> (data);
        r.data.assign (b, b + size); r.mimeType = mime;
        return r;
    }
    juce::File sharedSkinsDir()
    {
        for (auto vendor : { SolutionVendor::name, SolutionVendor::legacyName })
            for (auto location : { juce::File::userApplicationDataDirectory, juce::File::commonApplicationDataDirectory })
            {
                const auto dir = juce::File::getSpecialLocation (location).getChildFile (SolutionVendor::prefix() + vendor + "/The Solution/Skins");
                if (dir.isDirectory() && dir.getNumberOfChildFiles (juce::File::findFiles, "*.jpg") > 0) return dir;
            }
        return {};
    }
}

class SkinEditorComponent final : public juce::Component
{
public:
    SkinEditorComponent()
        : webView (juce::WebBrowserComponent::Options{}
                     #if JUCE_WINDOWS
                       .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
                       .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2{}
                                                    .withUserDataFolder (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("BilletSkinEditorWebView2")))
                     #endif
                       .withNativeIntegrationEnabled()
                       .withResourceProvider ([this] (const juce::String& url) { return serve (url); })
                       .withEventListener ("skins", [this] (const juce::var& d) { handle (d); }))
    {
        addAndMakeVisible (webView);
        webView.goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
    }
    void resized() override { webView.setBounds (getLocalBounds()); }

private:
    std::optional<juce::WebBrowserComponent::Resource> serve (const juce::String& url)
    {
        const auto u = url.upToFirstOccurrenceOf ("?", false, false);
        if (u == "/" || u == "/index.html" || u.isEmpty()) return makeResource (SkinEditorData::editor_html, (size_t) SkinEditorData::editor_htmlSize, "text/html");
        if (u.startsWith ("/skin/") && u.endsWith (".jpg"))
        {
            const auto key = u.fromLastOccurrenceOf ("/", false, false).upToLastOccurrenceOf (".", false, false);
            if (key.isEmpty() || ! key.containsOnly ("abcdefghijklmnopqrstuvwxyz0123456789_-")) return std::nullopt;
            juce::MemoryBlock mb;
            const auto f = key.startsWith ("custom_") ? SolutionSkins::userDir().getChildFile (key + ".jpg") : sharedSkinsDir().getChildFile (key + ".jpg");
            if (f.loadFileAsData (mb)) return makeResource (mb.getData(), mb.getSize(), "image/jpeg");
        }
        return std::nullopt;
    }

    void send (const juce::String& message = {})
    {
        juce::Array<juce::var> list;
        const auto custom = SolutionSkins::readList (SolutionSkins::userDir());
        if (auto* a = custom.getArray()) for (auto v : *a) { if (auto* o = v.getDynamicObject()) o->setProperty ("custom", true); list.add (v); }
        const auto shared = sharedSkinsDir();
        if (shared.isDirectory())
        {
            const auto meta = juce::JSON::parse (shared.getChildFile ("skins.json"));
            for (auto& f : shared.findChildFiles (juce::File::findFiles, false, "*.jpg"))
            {
                juce::DynamicObject::Ptr o = new juce::DynamicObject();
                const auto key = f.getFileNameWithoutExtension();
                o->setProperty ("key", key); o->setProperty ("name", meta.getProperty (key, {}).getProperty ("name", key));
                o->setProperty ("custom", false);
                list.add (juce::var (o.get()));
            }
        }
        juce::DynamicObject::Ptr p = new juce::DynamicObject();
        p->setProperty ("skins", list); p->setProperty ("folder", SolutionSkins::userDir().getFullPathName());
        if (message.isNotEmpty()) p->setProperty ("message", message);
        webView.emitEventIfBrowserIsVisible ("skinList", juce::var (p.get()));
    }

    void handle (const juce::var& d)
    {
        const auto cmd = d.getProperty ("cmd", {}).toString();
        const auto dir = SolutionSkins::userDir();
        if (cmd == "save")
        {
            juce::MemoryOutputStream jpeg;
            juce::Base64::convertFromBase64 (jpeg, d.getProperty ("jpeg", {}).toString().fromFirstOccurrenceOf (",", false, false));
            const auto key = SolutionSkins::save (dir, d.getProperty ("name", {}).toString(), jpeg.getData(), jpeg.getDataSize(),
                                                  d.getProperty ("accent", {}).toString(), d.getProperty ("accent2", {}).toString());
            send (key.isNotEmpty() ? "Saved - every open Solution plugin can use it now (Appearance menu)." : "Give it a name first.");
        }
        else if (cmd == "delete") { SolutionSkins::remove (dir, d.getProperty ("key", {}).toString()); send ("Deleted."); }
        else if (cmd == "rename") { SolutionSkins::rename (dir, d.getProperty ("key", {}).toString(), d.getProperty ("name", {}).toString()); send(); }
        else if (cmd == "export")
        {
            juce::StringArray keys; const auto ks = d.getProperty ("keys", {}); if (auto* a = ks.getArray()) for (auto& k : *a) keys.add (k.toString());
            chooser = std::make_unique<juce::FileChooser> ("Save skin pack", juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile ("My skins.solutionskin"), "*.solutionskin");
            chooser->launchAsync (juce::FileBrowserComponent::saveMode, [this, keys, dir] (const juce::FileChooser& fc)
            { const auto f = fc.getResult(); if (f != juce::File()) send (SolutionSkins::exportPack (dir, keys, f.withFileExtension ("solutionskin")) ? "Pack saved: " + f.getFileName() : "Nothing to export - pick your own skins."); });
        }
        else if (cmd == "import")
        {
            chooser = std::make_unique<juce::FileChooser> ("Open a skin pack", juce::File(), "*.solutionskin");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, dir] (const juce::FileChooser& fc)
            { const auto f = fc.getResult(); if (f.existsAsFile()) send (juce::String (SolutionSkins::importPack (dir, f)) + " skin(s) added."); });
        }
        else if (cmd == "reveal") { dir.createDirectory(); dir.revealToUser(); }
        else send();
    }

    std::unique_ptr<juce::FileChooser> chooser;
    juce::WebBrowserComponent webView;
};

class SkinEditorApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "Skin Editor"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return false; }
    void initialise (const juce::String&) override { window = std::make_unique<MainWindow>(); }
    void shutdown() override { window.reset(); }
    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted (const juce::String&) override { if (window != nullptr) window->toFront (true); }
private:
    struct MainWindow final : public juce::DocumentWindow
    {
        MainWindow() : DocumentWindow ("Skin Editor - Billet Audio · The Solution", juce::Colour (0xff080a0e), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true); setContentOwned (new SkinEditorComponent(), false);
            setResizable (true, true); setResizeLimits (1000, 640, 10000, 10000); centreWithSize (1400, 860); setVisible (true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (SkinEditorApplication)
