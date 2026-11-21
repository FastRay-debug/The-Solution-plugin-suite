#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <cmath>
#include <vector>

// Motion paths for the 3D stage: a closed loop of up to 32 points (azimuth, elevation, distance)
// that the sound travels around, locked to the host's tempo and song position when available.
// Interpolation is Catmull-Rom in 3D (so passing behind the head is smooth).
namespace SolutionDSP
{
    struct MotionPoint { float az = 0.0f, el = 0.0f, dist = 2.0f; };

    class MotionPlayer
    {
    public:
        static constexpr int maxPoints = 32;
        static constexpr std::array<float, 7> rateBars { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f };

        // Message thread.
        void setPath (const std::vector<MotionPoint>& pts)
        {
            const juce::SpinLock::ScopedLockType sl (lock);
            pendingCount = (int) std::min<size_t> (pts.size(), maxPoints);
            for (int i = 0; i < pendingCount; ++i) pending[(size_t) i] = toXyz (pts[(size_t) i]);
            hasPending = true;
        }

        // Audio thread: position at a phase 0..1 around the loop. Returns false if there's no path.
        bool evaluate (double phase, MotionPoint& out)
        {
            if (hasPending.load (std::memory_order_acquire))
            {
                const juce::SpinLock::ScopedTryLockType sl (lock);
                if (sl.isLocked()) { live = pending; liveCount = pendingCount; hasPending = false; }
            }
            if (liveCount < 2) return false;
            const double t = (phase - std::floor (phase)) * liveCount;
            const int i1 = (int) t;
            const float f = (float) (t - i1);
            auto P = [&] (int i) { return live[(size_t) (((i % liveCount) + liveCount) % liveCount)]; };
            const auto p0 = P (i1 - 1), p1 = P (i1), p2 = P (i1 + 1), p3 = P (i1 + 2);
            Xyz q;
            for (int k = 0; k < 3; ++k)
            {
                const float a = p0.v[k], b = p1.v[k], c = p2.v[k], d = p3.v[k];
                q.v[k] = 0.5f * ((2 * b) + (-a + c) * f + (2 * a - 5 * b + 4 * c - d) * f * f + (-a + 3 * b - 3 * c + d) * f * f * f);
            }
            out = fromXyz (q);
            return true;
        }

        int getNumPoints() const { return liveCount; }

        // Where in the loop we are: synced to the host (ppq) when it's playing, otherwise free-running at 120 BPM.
        static double phaseFor (bool hostPlaying, double ppq, double beatsPerBar, double bars, double freeSeconds)
        {
            const double beats = std::max (0.25, beatsPerBar * bars);
            if (hostPlaying) return ppq / beats;
            return freeSeconds * 2.0 / beats;          // 120 BPM = 2 beats per second
        }

    private:
        struct Xyz { float v[3] {}; };
        static Xyz toXyz (const MotionPoint& p)
        {
            const float a = juce::degreesToRadians (p.az), e = juce::degreesToRadians (p.el);
            return { { p.dist * std::cos (e) * std::sin (a), p.dist * std::sin (e), p.dist * std::cos (e) * std::cos (a) } };
        }
        static MotionPoint fromXyz (const Xyz& q)
        {
            const float x = q.v[0], y = q.v[1], z = q.v[2];
            const float d = std::max (0.5f, std::sqrt (x * x + y * y + z * z));
            return { juce::radiansToDegrees (std::atan2 (x, z)), juce::radiansToDegrees (std::asin (juce::jlimit (-1.0f, 1.0f, y / d))), d };
        }

        juce::SpinLock lock;
        std::array<Xyz, maxPoints> pending {}, live {};
        int pendingCount = 0, liveCount = 0;
        std::atomic<bool> hasPending { false };
    };
}
