// The Solution Utility - standalone Diagnostic Utility (Billet Audio hub) for the whole computer.
// Shows real system/GPU readings plus every Solution plugin in every running DAW (they report to it
// over 127.0.0.1 - see DiagBridge.h). Uses the same page as the plugins, in utility mode.
#include <juce_gui_extra/juce_gui_extra.h>
#include "SystemMonitor.h"
#include "SolutionProcessor.h"
#include "NetOffload.h"
#include "BinaryData.h"
#include <map>

namespace
{
    constexpr int utilityPort = 47790;     // must match SolutionDiag::utilityPort in DiagBridge.h

    juce::WebBrowserComponent::Resource makeResource (const void* data, size_t size, const char* mime)
    {
        juce::WebBrowserComponent::Resource r;
        const auto* b = static_cast<const std::byte*> (data);
        r.data.assign (b, b + size); r.mimeType = mime;
        return r;
    }
}

class UtilityComponent final : public juce::Component, private juce::Timer
{
public:
    UtilityComponent()
        : webView (juce::WebBrowserComponent::Options{}
                     #if JUCE_WINDOWS
                       .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
                       .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2{}
                                                    .withUserDataFolder (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("TheSolutionUtilityWebView2")))
                     #endif
                       .withNativeIntegrationEnabled()
                       .withResourceProvider ([] (const juce::String& url) -> std::optional<juce::WebBrowserComponent::Resource>
                       {
                           const auto u = url.upToFirstOccurrenceOf ("?", false, false);
                           if (u == "/" || u == "/index.html" || u.isEmpty()) return makeResource (BinaryData::index_html, (size_t) BinaryData::index_htmlSize, "text/html");
                           if (u == "/stage3d.bundle.js") return makeResource (BinaryData::stage3d_bundle_js, (size_t) BinaryData::stage3d_bundle_jsSize, "text/javascript");
                           return std::nullopt;
                       })
                       .withEventListener ("diag", [this] (const juce::var& d) { handleDiag (d); })
                       .withEventListener ("uiReady", [this] (const juce::var&) { send(); }))
    {
        addAndMakeVisible (webView);
        listening = socket.bindToPort (utilityPort, "127.0.0.1");
        // DSP Node: Solution plugins on this PC or the network can run their processing here.
        node = std::make_unique<SolutionNet::NodeServer> ([] (const juce::String& key) { return std::make_unique<SolutionAudioProcessor> (key); },
                                                          juce::SystemStats::getComputerName());
        nodeOn = node->start();
        webView.goToURL (juce::WebBrowserComponent::getResourceProviderRoot() + "?module=Solution%20Utility&utility=1&skins=0");
        startTimer (250);
    }
    ~UtilityComponent() override { stopTimer(); socket.shutdown(); }
    void resized() override { webView.setBounds (getLocalBounds()); }

private:
    struct Daw { juce::int64 pid = 0; juce::String host; juce::var plugins; double lastSeen = 0; };

    void timerCallback() override
    {
        // Drain every report that arrived (never blocks).
        char buf[65536];
        while (listening && socket.waitUntilReady (true, 0) == 1)
        {
            const int n = socket.read (buf, (int) sizeof (buf) - 1, false);
            if (n <= 0) break;
            buf[n] = 0;
            const auto v = juce::JSON::parse (juce::String::fromUTF8 (buf, n));
            if (! v.isObject() || (int) v["v"] != 1) continue;
            auto& d = daws[(juce::int64) v["pid"]];
            d.pid = (juce::int64) v["pid"]; d.host = v["host"].toString(); d.plugins = v["plugins"];
            d.lastSeen = juce::Time::getMillisecondCounterHiRes();
        }
        const double now = juce::Time::getMillisecondCounterHiRes();
        for (auto it = daws.begin(); it != daws.end();)
            if (now - it->second.lastSeen > 4000) { log ("info", (it->second.host.isNotEmpty() ? it->second.host : juce::String ("A DAW")) + " stopped reporting (closed or no Solution plugins left)"); it = daws.erase (it); }
            else ++it;
        if (++ticks % 2 == 0) send();
    }

    juce::var allPlugins()
    {
        juce::Array<juce::var> all;
        for (auto& [pid, d] : daws)
            if (auto* arr = d.plugins.getArray())
                for (auto p : *arr)
                {
                    if (auto* o = p.getDynamicObject())
                    {
                        o->setProperty ("daw", d.host);
                        o->setProperty ("pid", pid);
                        o->setProperty ("remote", true);
                        const auto key = juce::String (pid) + ":" + p["slot"].toString();
                        auto& last = counts[key];
                        const int ov = (int) p["overloads"], er = (int) p["errors"];
                        const auto who = p["module"].toString() + (p["name"].toString().isNotEmpty() ? " (" + p["name"].toString() + ")" : juce::String()) + " in " + d.host;
                        if (last.seen && ov > last.ov) log ("error", who + ": " + juce::String (ov - last.ov) + " audio block(s) ran past real time - raise the buffer size or let DQS step down");
                        if (last.seen && er > last.er) log ("warn", who + ": " + juce::String (er - last.er) + " new signal event(s) (clipping / phase / broken samples)");
                        last = { ov, er, true };
                    }
                    all.add (p);
                }
        return all;
    }

    void send()
    {
        const auto s = monitor->getSnapshot();
        juce::var payload = SolutionDiag::snapshotToVar (s);
        auto* o = payload.getDynamicObject();
        o->setProperty ("plugins", allPlugins());
        o->setProperty ("log", logEntries);
        juce::StringArray hosts; for (auto& [pid, d] : daws) hosts.addIfNotAlreadyThere (d.host);
        o->setProperty ("host", hosts.isEmpty() ? juce::String ("no DAW reporting yet") : hosts.joinIntoString (", "));
        o->setProperty ("utility", true);
        juce::DynamicObject::Ptr n = new juce::DynamicObject();
        n->setProperty ("on", nodeOn); n->setProperty ("port", node != nullptr ? node->getPort() : 0);
        n->setProperty ("sessions", node != nullptr ? node->getActiveSessions() : 0);
        juce::Array<juce::var> mods; if (node != nullptr) for (auto& m : node->describeSessions()) mods.add (m);
        n->setProperty ("modules", mods);
        o->setProperty ("node", juce::var (n.get()));
        o->setProperty ("listening", listening);
        webView.emitEventIfBrowserIsVisible ("diagData", payload);
    }

    void handleDiag (const juce::var& d)
    {
        const auto cmd = d.getProperty ("cmd", {}).toString();
        if (cmd == "clearLog") { logEntries.clear(); send(); }
        else if (cmd == "node") { const bool on = (bool) d.getProperty ("on", true); if (node != nullptr) { if (on) nodeOn = node->start(); else { node->stop(); nodeOn = false; } } send(); }
        else if (cmd == "export")
        {
            const bool xml = d.getProperty ("format", "csv").toString() == "xml";
            const auto s = monitor->getSnapshot();
            const auto text = xml ? SolutionDiag::makeXmlReport (s, allPlugins(), juce::var (logEntries)) : SolutionDiag::makeCsvReport (s, allPlugins(), juce::var (logEntries));
            const auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("The Solution").getChildFile ("Diagnostics");
            dir.createDirectory();
            const auto file = dir.getChildFile ("Solution-Diagnostics-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + (xml ? ".xml" : ".csv"));
            const bool ok = file.replaceWithText (text);
            if (ok) file.revealToUser();
            webView.emitEventIfBrowserIsVisible ("diagExported", ok ? juce::var (file.getFullPathName()) : juce::var ("Couldn't write to " + dir.getFullPathName()));
        }
    }

    void log (const juce::String& level, const juce::String& text)
    {
        juce::DynamicObject::Ptr e = new juce::DynamicObject();
        e->setProperty ("time", juce::Time::getCurrentTime().toString (false, true, true, true));
        e->setProperty ("level", level); e->setProperty ("text", text);
        logEntries.insert (0, juce::var (e.get()));
        if (logEntries.size() > 200) logEntries.removeLast();
    }

    struct Counts { int ov = 0, er = 0; bool seen = false; };
    juce::SharedResourcePointer<SolutionDiag::SystemMonitor> monitor;
    std::unique_ptr<SolutionNet::NodeServer> node;
    bool nodeOn = false;
    juce::DatagramSocket socket { false };
    bool listening = false;
    std::map<juce::int64, Daw> daws;
    std::map<juce::String, Counts> counts;
    juce::Array<juce::var> logEntries;
    int ticks = 0;
    juce::WebBrowserComponent webView;
};

class SolutionUtilityApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "Solution Utility"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String&) override { window = std::make_unique<MainWindow>(); }
    void shutdown() override { window.reset(); }
    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted (const juce::String&) override { if (window != nullptr) window->toFront (true); }

private:
    struct MainWindow final : public juce::DocumentWindow
    {
        MainWindow() : DocumentWindow ("The Solution Utility - Billet Audio", juce::Colour (0xff05070d), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new UtilityComponent(), false);
            setResizable (true, true);
            setResizeLimits (960, 600, 10000, 10000);
            centreWithSize (1360, 820);
            setVisible (true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (SolutionUtilityApplication)
