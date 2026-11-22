#pragma once
// The Matrix: session-wide modulation.
// Sources: LFO 1-4 (sine/triangle/saw up/saw down/square/S&H/smooth random; free Hz or tempo-locked to the
// host's bars), ENV 1 (the Matrix track's own level), ENV 2 (the Rack Link group key), MACRO 1-4, RANDOM.
// Routes: source -> any control on any Solution track in the session, depth -100..+100 %, phase offset
// (so one LFO can sweep several tracks out of step - "poly" LFOs). Each track applies its own routes.
#include <juce_core/juce_core.h>
#include <array>
#include <atomic>
#include <cmath>

namespace SolutionDSP
{
    namespace Mod
    {
        enum Source { Lfo1 = 0, Lfo2, Lfo3, Lfo4, Env1, Env2, Macro1, Macro2, Macro3, Macro4, Random, NumSources };
        inline const char* sourceName (int s)
        {
            static const char* n[] = { "LFO 1", "LFO 2", "LFO 3", "LFO 4", "ENV 1 (this track)", "ENV 2 (link key)", "MACRO 1", "MACRO 2", "MACRO 3", "MACRO 4", "RANDOM" };
            return juce::isPositiveAndBelow (s, (int) NumSources) ? n[s] : "?";
        }
        inline float hash01 (double x) { const double s = std::sin (x * 12.9898 + 78.233) * 43758.5453; return (float) (s - std::floor (s)); }
        // Bipolar -1..1 value of an LFO shape at phase (cycles; any real number)
        inline float shape (int sh, double phase)
        {
            const double ph = phase - std::floor (phase), cyc = std::floor (phase);
            switch (sh)
            {
                case 1: return (float) (ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph);                    // triangle
                case 2: return (float) (2 * ph - 1);                                            // saw up
                case 3: return (float) (1 - 2 * ph);                                            // saw down
                case 4: return ph < 0.5 ? 1.0f : -1.0f;                                         // square
                case 5: return hash01 (cyc) * 2.0f - 1.0f;                                      // sample & hold
                case 6: { const float a = hash01 (cyc), b = hash01 (cyc + 1); const float t = (float) (ph * ph * (3 - 2 * ph)); return (a + (b - a) * t) * 2 - 1; }  // smooth random
                default: return (float) std::sin (juce::MathConstants<double>::twoPi * ph);
            }
        }
        // Tempo divisions in beats (index 0 = free)
        inline double syncBeats (int s) { static const double b[] = { 0, 0.25, 0.5, 1, 2, 4, 8, 16 }; return b[juce::jlimit (0, 7, s)]; }

        struct Route { std::atomic<int> slot { -1 }, param { -1 }, src { 0 }; std::atomic<float> depth { 0 }, phase { 0 }; };
        // Sidechain over Rack Link: 4 stereo channels, written and read at the host's timeline position
        // (so track processing order doesn't matter); the reader stays one block behind the writer.
        struct Sidechain
        {
            static constexpr int size = 1 << 15;
            std::array<std::array<float, size>, 2> data {};
            std::atomic<long long> lastWrite { -1 };
            void write (const float* l, const float* r, int n, long long pos)
            {
                for (int i = 0; i < n; ++i) { const auto k = (size_t) ((pos + i) & (size - 1)); data[0][k] = l[i]; data[1][k] = r[i]; }
                lastWrite.store (pos + n, std::memory_order_release);
            }
            void read (float* l, float* r, int n, long long pos, int lag) const
            {
                if (lastWrite.load (std::memory_order_acquire) < 0) { std::fill (l, l + n, 0.0f); std::fill (r, r + n, 0.0f); return; }
                for (int i = 0; i < n; ++i) { const auto k = (size_t) ((pos + i - lag) & (size - 1)); l[i] = data[0][k]; r[i] = data[1][k]; }
            }
        };
        struct Bus
        {
            std::array<Sidechain, 4> sidechain;
            std::array<std::atomic<double>, 4> lfoPhase {};      // cycles (unwrapped is fine)
            std::array<std::atomic<int>, 4> lfoShape {};
            std::array<std::atomic<float>, NumSources> value {}; // current value of every source (-1..1 for LFOs, 0..1 others)
            static constexpr int maxRoutes = 32;
            std::array<Route, maxRoutes> routes;
            std::atomic<int> routeCount { 0 }, owner { -1 };     // owner = rack slot of the Matrix that set the routes
            float sourceValue (int src, float phaseOffset) const
            {
                if (src >= Lfo1 && src <= Lfo4) return shape (lfoShape[(size_t) src].load (std::memory_order_relaxed), lfoPhase[(size_t) src].load (std::memory_order_relaxed) + phaseOffset);
                return juce::isPositiveAndBelow (src, (int) NumSources) ? value[(size_t) src].load (std::memory_order_relaxed) : 0.0f;
            }
        };
    }
}
