#pragma once
// The Solution - head tracking for binaural monitoring. One per DAW process.
// Sources: OSC (pro head trackers, IEM/SPARTA "ypr"/quaternion, and phone apps like GyrOSC, ZIG SIM,
// Sensors2OSC), the phone remote page, and a gamepad/joystick (from the UI). Turning your head keeps the
// binaural mix fixed in the room around you, like real speakers.
#include <juce_osc/juce_osc.h>
#include <atomic>
#include <cmath>

namespace SolutionDSP
{
    class HeadTracker final : private juce::OSCReceiver::Listener<juce::OSCReceiver::RealtimeCallback>
    {
    public:
        HeadTracker() { receiver.addListener (this); }
        ~HeadTracker() override { receiver.removeListener (this); receiver.disconnect(); }

        // OSC on/off. Returns false if the port can't be opened.
        bool listenOsc (int port)
        {
            receiver.disconnect(); oscPort = 0;
            if (port <= 0) return true;
            if (! receiver.connect (port)) return false;
            oscPort = port; return true;
        }
        int getOscPort() const { return oscPort; }

        // Any source (degrees). yaw: + = head turned right; pitch: + = looking up.
        void setPose (float yawDeg, float pitchDeg, int source)
        {
            rawYaw.store (wrap (yawDeg)); rawPitch.store (juce::jlimit (-90.0f, 90.0f, pitchDeg));
            lastSource.store (source); lastUpdateMs.store (juce::Time::getMillisecondCounter());
        }
        void recenter() { yawOffset.store (rawYaw.load()); pitchOffset.store (rawPitch.load()); }
        float yaw() const   { return wrap (rawYaw.load() - yawOffset.load()); }
        float pitch() const { return juce::jlimit (-90.0f, 90.0f, rawPitch.load() - pitchOffset.load()); }
        bool isFresh() const { return juce::Time::getMillisecondCounter() - lastUpdateMs.load() < 1500; }
        int source() const { return lastSource.load(); }       // 1 OSC, 2 phone page, 3 gamepad
        std::atomic<bool> enabled { false };                    // follow the head at all

        // Parses one OSC message; public so it can be tested without a network.
        void handle (const juce::OSCMessage& m)
        {
            const auto addr = m.getAddressPattern().toString().toLowerCase();
            float v[4] {}; int n = 0;
            for (auto& a : m) { if (n >= 4) break; if (a.isFloat32()) v[n++] = a.getFloat32(); else if (a.isInt32()) v[n++] = (float) a.getInt32(); }
            if (n >= 4 && (addr.contains ("quat")))
            {
                // w x y z (IEM order). ZIG SIM sends x y z w: if the first value looks like a vector part, rotate.
                float w = v[0], x = v[1], y = v[2], z = v[3];
                if (addr.contains ("zigsim")) { x = v[0]; y = v[1]; z = v[2]; w = v[3]; }
                const float yawR = std::atan2 (2.0f * (w * z + x * y), 1.0f - 2.0f * (y * y + z * z));
                const float pitchR = std::asin (juce::jlimit (-1.0f, 1.0f, 2.0f * (w * y - z * x)));
                setPose (juce::radiansToDegrees (yawR), juce::radiansToDegrees (pitchR), 1);
            }
            else if (n >= 3 && (addr.contains ("ypr") || addr.contains ("euler") || addr.contains ("orientation") || addr.contains ("gyro") || addr.contains ("rotation") || addr.contains ("head")))
            {
                const bool radians = std::abs (v[0]) <= 6.3f && std::abs (v[1]) <= 6.3f && std::abs (v[2]) <= 6.3f && ! addr.contains ("deg");
                float yawV = v[0], pitchV = v[1];
                if (addr.contains ("gyrosc")) { pitchV = v[0]; yawV = v[2]; }     // GyrOSC: pitch roll yaw
                setPose (radians ? juce::radiansToDegrees (yawV) : yawV, radians ? juce::radiansToDegrees (pitchV) : pitchV, 1);
            }
        }

    private:
        void oscMessageReceived (const juce::OSCMessage& m) override { handle (m); }
        void oscBundleReceived (const juce::OSCBundle& b) override { for (auto& e : b) if (e.isMessage()) handle (e.getMessage()); else if (e.isBundle()) oscBundleReceived (e.getBundle()); }
        static float wrap (float d) { while (d > 180.0f) d -= 360.0f; while (d < -180.0f) d += 360.0f; return d; }
        juce::OSCReceiver receiver { "Solution OSC head tracking" };
        int oscPort = 0;
        std::atomic<float> rawYaw { 0 }, rawPitch { 0 }, yawOffset { 0 }, pitchOffset { 0 };
        std::atomic<int> lastSource { 0 };
        std::atomic<juce::uint32> lastUpdateMs { 0 };
    };
}
