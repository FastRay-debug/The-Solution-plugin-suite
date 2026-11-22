#pragma once
// The Solution - LAN / local DSP offload (AudioGridder-style).
// A "DSP Node" (the Solution Utility app, on this PC or another one on the network) runs the plugin's
// processing; the plugin streams its audio there and plays back what returns, a fixed number of samples
// later (reported to the DAW). If the node or network falls behind, the plugin goes back to local
// processing. Framing: 'SOLA' u8 type u32 length payload.
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace SolutionNet
{
    inline constexpr int nodePort = 47900, discoveryPort = 47901;
    enum FrameType : juce::uint8 { Hello = 1, State = 2, Audio = 3, HelloAck = 4, Bye = 5 };

    inline bool writeFrame (juce::StreamingSocket& s, FrameType t, const void* data, juce::uint32 len)
    {
        juce::uint8 head[9] = { 'S', 'O', 'L', 'A', (juce::uint8) t };
        std::memcpy (head + 5, &len, 4);                                          // little-endian on every platform we ship
        return s.write (head, 9) == 9 && (len == 0 || s.write (data, (int) len) == (int) len);
    }
    inline bool readExact (juce::StreamingSocket& s, void* dst, int n, int timeoutMs)
    {
        auto* p = static_cast<char*> (dst); int got = 0;
        while (got < n)
        {
            if (s.waitUntilReady (true, timeoutMs) != 1) return false;
            const int r = s.read (p + got, n - got, false);
            if (r <= 0) return false;
            got += r;
        }
        return true;
    }
    inline bool readFrame (juce::StreamingSocket& s, FrameType& t, juce::MemoryBlock& payload, int timeoutMs)
    {
        juce::uint8 head[9];
        if (! readExact (s, head, 9, timeoutMs)) return false;
        if (head[0] != 'S' || head[1] != 'O' || head[2] != 'L' || head[3] != 'A') return false;
        juce::uint32 len; std::memcpy (&len, head + 5, 4);
        if (len > 16 * 1024 * 1024) return false;
        t = (FrameType) head[4]; payload.setSize (len);
        return len == 0 || readExact (s, payload.getData(), (int) len, timeoutMs);
    }

    //==========================================================================
    // Plugin side. process() runs on the audio thread (no locks, no allocation); networking on two threads.
    class OffloadClient
    {
    public:
        ~OffloadClient() { disconnect(); }

        // Message thread. latencySamples = how far ahead the node may be (the extra latency reported).
        bool connect (const juce::String& host, int port, const juce::String& moduleKey, double sampleRate, int maxBlock, int latencySamples, juce::String& error)
        {
            disconnect();
            latency = juce::jlimit (256, 16384, latencySamples);
            const int cap = latency * 4 + maxBlock * 4 + 8192;
            sendFifo = std::make_unique<juce::AbstractFifo> (cap); recvFifo = std::make_unique<juce::AbstractFifo> (cap);
            sendBuf.setSize (2, cap); recvBuf.setSize (2, cap); scratch.setSize (2, cap);
            socket = std::make_unique<juce::StreamingSocket>();
            if (! socket->connect (host, port, 2000)) { error = "Couldn't reach the DSP node at " + host + ":" + juce::String (port); socket.reset(); return false; }
            juce::DynamicObject::Ptr hello = new juce::DynamicObject();
            hello->setProperty ("module", moduleKey); hello->setProperty ("sampleRate", sampleRate); hello->setProperty ("block", maxBlock); hello->setProperty ("v", 1);
            const auto js = juce::JSON::toString (juce::var (hello.get()), true);
            FrameType t; juce::MemoryBlock reply;
            if (! writeFrame (*socket, Hello, js.toRawUTF8(), (juce::uint32) js.getNumBytesAsUTF8()) || ! readFrame (*socket, t, reply, 3000) || t != HelloAck)
            { error = "The DSP node didn't answer (is it a Solution Utility with DSP Node on?)"; socket.reset(); return false; }
            nodeInfo = juce::JSON::parse (reply.toString());
            primed = false; failed = false; lateBlocks = 0; sentSamples = 0; returnedSamples = 0; outPos = 0; recvPos = 0; running = true;
            sender = std::thread ([this] { sendLoop(); });
            receiver = std::thread ([this] { recvLoop(); });
            return true;
        }
        void disconnect()
        {
            running = false;
            if (socket != nullptr) { const juce::ScopedLock sl (sendLock); writeFrame (*socket, Bye, nullptr, 0); socket->close(); }
            if (sender.joinable()) sender.join();
            if (receiver.joinable()) receiver.join();
            socket.reset();
        }
        bool isConnected() const { return running.load() && ! failed.load(); }
        bool hasFailed() const { return failed.load(); }
        int getLatency() const { return latency; }
        juce::var getNodeInfo() const { return nodeInfo; }
        int getLateBlocks() const { return lateBlocks.load(); }
        double getRoundTripMs (double fs) const { return (double) (sentSamples.load() - returnedSamples.load()) * 1000.0 / fs; }

        // Message thread: send the current plugin state so the node sounds identical.
        void sendState (const juce::MemoryBlock& state)
        {
            if (! running || socket == nullptr) return;
            const juce::ScopedLock sl (sendLock);
            if (! writeFrame (*socket, State, state.getData(), (juce::uint32) state.getSize())) failed = true;
        }

        // Audio thread: send this block, play what came back `latency` samples ago (silence until primed).
        // Output sample j is always processed input sample j - latency (exactly what the DAW was told).
        // Before the node's audio is due, the output is silent; if a block arrives late, the gap is silent and
        // the late samples are dropped when they turn up, so the timing never drifts.
        void process (float* l, float* r, int n)
        {
            pushInput (l, r, n);
            const long long want = outPos - latency;                    // input index that belongs at l[0]
            if (want > recvPos)                                         // late audio from earlier blocks: drop it
            {
                const int drop = (int) std::min<long long> (want - recvPos, recvFifo->getNumReady());
                if (drop > 0) { recvFifo->read (drop); recvPos += drop; }
            }
            const int lead = (int) juce::jlimit<long long> (0, n, recvPos - want);   // not due yet (start-up)
            int take = 0;
            if (recvPos >= want) take = std::min (n - lead, recvFifo->getNumReady());
            juce::FloatVectorOperations::clear (l, n); juce::FloatVectorOperations::clear (r, n);
            if (take > 0)
            {
                const auto rd = recvFifo->read (take);
                copyOut (rd.startIndex1, rd.blockSize1, l, r, lead); copyOut (rd.startIndex2, rd.blockSize2, l, r, lead + rd.blockSize1);
                recvPos += take;
            }
            if (want >= 0 && lead + take < n) { if (++lateBlocks > 8) failed = true; }   // the node can't keep up: plugin goes local
            else if (want >= 0) lateBlocks = std::max (0, lateBlocks - 1);
            outPos += n;
        }

    private:
        void pushInput (const float* l, const float* r, int n)
        {
            int s1, n1, s2, n2;
            sendFifo->prepareToWrite (n, s1, n1, s2, n2);
            if (n1 + n2 < n) { failed = true; return; }
            std::memcpy (sendBuf.getWritePointer (0, s1), l, (size_t) n1 * sizeof (float)); std::memcpy (sendBuf.getWritePointer (1, s1), r, (size_t) n1 * sizeof (float));
            if (n2 > 0) { std::memcpy (sendBuf.getWritePointer (0, s2), l + n1, (size_t) n2 * sizeof (float)); std::memcpy (sendBuf.getWritePointer (1, s2), r + n1, (size_t) n2 * sizeof (float)); }
            sendFifo->finishedWrite (n1 + n2);
        }
        void copyOut (int start, int size, float* l, float* r, int offset)
        {
            if (size <= 0) return;
            std::memcpy (l + offset, recvBuf.getReadPointer (0, start), (size_t) size * sizeof (float));
            std::memcpy (r + offset, recvBuf.getReadPointer (1, start), (size_t) size * sizeof (float));
        }
        void sendLoop()
        {
            std::vector<char> frame;
            while (running.load())
            {
                const int ready = sendFifo->getNumReady();
                if (ready < 64) { juce::Thread::sleep (1); continue; }
                const int n = std::min (ready, 1024);
                frame.resize (8 + (size_t) n * 8);
                const juce::uint32 seq = seqNo++, ns = (juce::uint32) n;
                std::memcpy (frame.data(), &seq, 4); std::memcpy (frame.data() + 4, &ns, 4);
                int s1, n1, s2, n2; sendFifo->prepareToRead (n, s1, n1, s2, n2);
                auto* L = reinterpret_cast<float*> (frame.data() + 8); auto* R = L + n;
                std::memcpy (L, sendBuf.getReadPointer (0, s1), (size_t) n1 * 4); std::memcpy (R, sendBuf.getReadPointer (1, s1), (size_t) n1 * 4);
                if (n2 > 0) { std::memcpy (L + n1, sendBuf.getReadPointer (0, s2), (size_t) n2 * 4); std::memcpy (R + n1, sendBuf.getReadPointer (1, s2), (size_t) n2 * 4); }
                sendFifo->finishedRead (n1 + n2);
                const juce::ScopedLock sl (sendLock);
                if (! writeFrame (*socket, Audio, frame.data(), (juce::uint32) frame.size())) { failed = true; running = false; }
                sentSamples += n;
            }
        }
        void recvLoop()
        {
            juce::MemoryBlock payload; FrameType t;
            while (running.load())
            {
                if (! readFrame (*socket, t, payload, 200)) { if (running.load() && ! socket->isConnected()) { failed = true; running = false; } continue; }
                if (t != Audio || payload.getSize() < 8) continue;
                juce::uint32 ns; std::memcpy (&ns, static_cast<const char*> (payload.getData()) + 4, 4);
                const int n = (int) ns;
                if (payload.getSize() < 8 + (size_t) n * 8) continue;
                const auto* L = reinterpret_cast<const float*> (static_cast<const char*> (payload.getData()) + 8); const auto* R = L + n;
                int s1, n1, s2, n2; recvFifo->prepareToWrite (n, s1, n1, s2, n2);
                std::memcpy (recvBuf.getWritePointer (0, s1), L, (size_t) n1 * 4); std::memcpy (recvBuf.getWritePointer (1, s1), R, (size_t) n1 * 4);
                if (n2 > 0) { std::memcpy (recvBuf.getWritePointer (0, s2), L + n1, (size_t) n2 * 4); std::memcpy (recvBuf.getWritePointer (1, s2), R + n1, (size_t) n2 * 4); }
                recvFifo->finishedWrite (n1 + n2);
                returnedSamples += n;
            }
        }

        std::unique_ptr<juce::StreamingSocket> socket;
        std::unique_ptr<juce::AbstractFifo> sendFifo, recvFifo;
        juce::AudioBuffer<float> sendBuf, recvBuf, scratch;
        juce::CriticalSection sendLock;
        std::thread sender, receiver;
        std::atomic<bool> running { false }, failed { false }, primed { false };
        std::atomic<int> lateBlocks { 0 };
        long long outPos = 0, recvPos = 0;            // audio thread: output index / input index of the next received sample
        std::atomic<juce::int64> sentSamples { 0 }, returnedSamples { 0 };
        juce::uint32 seqNo = 0;
        int latency = 2048;
        juce::var nodeInfo;
    };

    //==========================================================================
    // Node side: accepts plugins, runs a processor per connection. Also answers LAN discovery.
    class NodeServer final : private juce::Thread
    {
    public:
        using Factory = std::function<std::unique_ptr<juce::AudioProcessor> (const juce::String& moduleKey)>;
        explicit NodeServer (Factory f, juce::String nodeName) : juce::Thread ("Solution DSP Node"), factory (std::move (f)), name (std::move (nodeName)) {}
        ~NodeServer() override { stop(); }

        bool start (int port = nodePort)
        {
            stop();
            listener = std::make_unique<juce::StreamingSocket>();
            if (! listener->createListener (port)) { listener.reset(); return false; }
            listenPort = port;
            discovery = std::make_unique<juce::DatagramSocket> (true);
            if (! discovery->bindToPort (discoveryPort)) discovery.reset();
            startThread();
            return true;
        }
        void stop()
        {
            signalThreadShouldExit();
            if (listener) listener->close();
            if (discovery) discovery->shutdown();
            stopThread (3000);
            const juce::ScopedLock sl (sessionLock);
            for (auto& s : sessions) { s->stop = true; if (s->sock) s->sock->close(); }
            for (auto& s : sessions) if (s->thread.joinable()) s->thread.join();
            sessions.clear();
            listener.reset(); discovery.reset();
        }
        int getPort() const { return listenPort; }
        int getActiveSessions() const { const juce::ScopedLock sl (sessionLock); int n = 0; for (auto& s : sessions) n += s->alive.load() ? 1 : 0; return n; }
        juce::StringArray describeSessions() const { const juce::ScopedLock sl (sessionLock); juce::StringArray a; for (auto& s : sessions) if (s->alive.load()) a.add (s->module); return a; }

    private:
        struct Session
        {
            std::unique_ptr<juce::StreamingSocket> sock;
            std::thread thread;
            std::atomic<bool> stop { false }, alive { true };
            juce::String module;
        };

        void run() override
        {
            while (! threadShouldExit())
            {
                answerDiscovery();
                if (listener == nullptr || listener->waitUntilReady (true, 100) != 1) continue;
                std::unique_ptr<juce::StreamingSocket> client (listener->waitForNextConnection());
                if (client == nullptr) continue;
                auto session = std::make_unique<Session>();
                session->sock = std::move (client);
                auto* sp = session.get();
                session->thread = std::thread ([this, sp] { serve (*sp); });
                const juce::ScopedLock sl (sessionLock);
                for (auto it = sessions.begin(); it != sessions.end();)          // tidy finished sessions
                    if (! (*it)->alive.load()) { if ((*it)->thread.joinable()) (*it)->thread.join(); it = sessions.erase (it); } else ++it;
                sessions.push_back (std::move (session));
            }
        }

        void answerDiscovery()
        {
            if (discovery == nullptr || discovery->waitUntilReady (true, 0) != 1) return;
            char buf[64]; juce::String sender; int senderPort = 0;
            const int n = discovery->read (buf, sizeof (buf) - 1, false, sender, senderPort);
            if (n >= 5 && std::memcmp (buf, "SOLN?", 5) == 0)
            {
                const auto reply = "SOLN!" + juce::JSON::toString (juce::var (makeInfo().get()), true);
                discovery->write (sender, senderPort, reply.toRawUTF8(), (int) reply.getNumBytesAsUTF8());
            }
        }
        juce::DynamicObject::Ptr makeInfo() const
        {
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            o->setProperty ("name", name); o->setProperty ("port", listenPort);
            o->setProperty ("cores", juce::SystemStats::getNumCpus()); o->setProperty ("cpu", juce::SystemStats::getCpuModel());
            o->setProperty ("sessions", getActiveSessions());
            return o;
        }

        void serve (Session& s)
        {
            FrameType t; juce::MemoryBlock payload;
            std::unique_ptr<juce::AudioProcessor> proc;
            juce::AudioBuffer<float> buf; juce::MidiBuffer midi; std::vector<char> out;
            if (readFrame (*s.sock, t, payload, 5000) && t == Hello)
            {
                const auto hello = juce::JSON::parse (payload.toString());
                s.module = hello.getProperty ("module", {}).toString();
                const double fs = (double) hello.getProperty ("sampleRate", 48000.0);
                const int block = juce::jlimit (32, 8192, (int) hello.getProperty ("block", 512));
                proc = factory ? factory (s.module) : nullptr;
                if (proc != nullptr)
                {
                    proc->setPlayConfigDetails (2, 2, fs, 1024);
                    proc->prepareToPlay (fs, 1024);
                    buf.setSize (2, 1024);
                    const auto info = juce::JSON::toString (juce::var (makeInfo().get()), true);
                    writeFrame (*s.sock, HelloAck, info.toRawUTF8(), (juce::uint32) info.getNumBytesAsUTF8());
                    juce::ignoreUnused (block);
                }
            }
            while (proc != nullptr && ! s.stop.load())
            {
                if (! readFrame (*s.sock, t, payload, 500)) { if (! s.sock->isConnected()) break; continue; }
                if (t == Bye) break;
                if (t == State) { proc->setStateInformation (payload.getData(), (int) payload.getSize()); continue; }
                if (t != Audio || payload.getSize() < 8) continue;
                juce::uint32 ns; std::memcpy (&ns, static_cast<const char*> (payload.getData()) + 4, 4);
                const int n = (int) ns;
                if (n <= 0 || n > 1024 || payload.getSize() < 8 + (size_t) n * 8) continue;
                const auto* L = reinterpret_cast<const float*> (static_cast<const char*> (payload.getData()) + 8);
                buf.setSize (2, n, false, false, true);
                buf.copyFrom (0, 0, L, n); buf.copyFrom (1, 0, L + n, n);
                midi.clear();
                proc->processBlock (buf, midi);
                out.resize (payload.getSize());
                std::memcpy (out.data(), payload.getData(), 8);
                std::memcpy (out.data() + 8, buf.getReadPointer (0), (size_t) n * 4);
                std::memcpy (out.data() + 8 + (size_t) n * 4, buf.getReadPointer (1), (size_t) n * 4);
                if (! writeFrame (*s.sock, Audio, out.data(), (juce::uint32) out.size())) break;
            }
            if (proc) proc->releaseResources();
            s.alive = false;
        }

        Factory factory;
        juce::String name;
        std::unique_ptr<juce::StreamingSocket> listener;
        std::unique_ptr<juce::DatagramSocket> discovery;
        int listenPort = 0;
        juce::CriticalSection sessionLock;
        std::vector<std::unique_ptr<Session>> sessions;
    };

    // Plugin side: ask the LAN (and this PC) which DSP nodes are out there. Blocks up to timeoutMs.
    inline juce::Array<juce::var> discoverNodes (int timeoutMs = 400)
    {
        juce::Array<juce::var> found;
        juce::DatagramSocket sock (true);
        if (! sock.bindToPort (0)) return found;
        const char* ask = "SOLN?";
        sock.write ("255.255.255.255", discoveryPort, ask, 5);
        sock.write ("127.0.0.1", discoveryPort, ask, 5);
        const auto until = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        while (juce::Time::getMillisecondCounter() < until)
        {
            if (sock.waitUntilReady (true, 50) != 1) continue;
            char buf[1024]; juce::String from; int fromPort = 0;
            const int n = sock.read (buf, sizeof (buf) - 1, false, from, fromPort);
            if (n > 5 && std::memcmp (buf, "SOLN!", 5) == 0)
            {
                auto info = juce::JSON::parse (juce::String::fromUTF8 (buf + 5, n - 5));
                if (auto* o = info.getDynamicObject())
                {
                    o->setProperty ("host", from == "127.0.0.1" ? juce::String ("this computer") : from);
                    o->setProperty ("address", from);
                    bool dup = false; for (auto& f : found) dup = dup || (f["address"] == info["address"] && f["port"] == info["port"]) || (f["name"] == info["name"] && f["port"] == info["port"]);
                    if (! dup) found.add (info);
                }
            }
        }
        return found;
    }
}
