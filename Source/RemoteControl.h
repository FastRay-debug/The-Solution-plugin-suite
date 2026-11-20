#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <random>

// Phone / tablet remote: a tiny HTTP server on the local network. Off by default.
// The phone opens http://<computer-ip>:<port>/, enters the 6-digit code shown in the plugin,
// and gets a 128-bit token; every API call needs that token.
namespace SolutionDSP
{
    class RemoteServer : private juce::Thread
    {
    public:
        struct Host
        {
            std::function<juce::String()> getPage;                               // mobile HTML
            std::function<juce::var()> getState;                                 // any thread
            std::function<void (const juce::String&, float)> setParam;           // called on the message thread
            std::function<void (const juce::String&)> loadPreset;                // called on the message thread
            std::function<void (std::function<void()>)> runOnMessageThread;
        };

        explicit RemoteServer (Host h) : juce::Thread ("Solution Remote"), host (std::move (h)) {}
        ~RemoteServer() override { stop(); }

        // Returns false if no port in the range could be opened.
        bool start (int firstPort = 47800, int numPorts = 100, const juce::String& bindAddress = {});
        void stop();
        bool isRunning() const { return isThreadRunning() && port > 0; }
        int getPort() const { return port; }
        juce::String getPairingCode() const { return code; }
        juce::String getUrl() const;            // http://192.168.x.x:port/
        int getConnectedDevices() const { return pairedCount.load(); }

        static juce::String findLanAddress();

    private:
        void run() override;
        void handle (juce::StreamingSocket& client);
        void respond (juce::StreamingSocket& s, int status, const juce::String& contentType, const juce::String& body);

        Host host;
        juce::StreamingSocket listener;
        int port = 0;
        juce::String code;
        juce::StringArray tokens;               // server thread only
        std::atomic<int> pairedCount { 0 };
        int failedAttempts = 0;
        juce::uint32 lockedUntilMs = 0;
    };
}
