#pragma once

#include "ProTricks.h"
#include <array>

// Unmask: a "priority" track (vocal, kick...) publishes how loud it is in 12 bands; tracks set to
// "make room" dip only those bands, only while the priority is loud there. No sidechain routing.
namespace SolutionDSP
{
    class Unmask
    {
    public:
        static constexpr int numBands = 12;
        static constexpr std::array<float, numBands> centres { 100, 150, 220, 330, 500, 750, 1100, 1600, 2400, 3600, 5400, 8000 };

        void prepare (double fs)
        {
            for (int b = 0; b < numBands; ++b)
            {
                detect[(size_t) b].set (fs, centres[(size_t) b], 2.0);
                detect2[(size_t) b].set (fs, centres[(size_t) b], 2.0);      // two in series: sharper band edges
                for (int c = 0; c < 2; ++c) cut[(size_t) (b * 2 + c)].set (fs, centres[(size_t) b], 2.0);
            }
            envCoeff = std::exp (-1.0 / (0.030 * fs));
            att = (float) std::exp (-1.0 / (0.010 * fs)); rel = (float) std::exp (-1.0 / (0.150 * fs));
            reset();
        }
        void reset()
        {
            for (auto& d : detect) d.clear();
            for (auto& d : detect2) d.clear();
            for (auto& c : cut) c.clear();
            energy.fill (0.0); dipDb.fill (0.0f);
        }

        // Priority side: measure band levels of this track (dBFS).
        void measure (const float* l, const float* r, int n, std::array<float, numBands>& outDb)
        {
            for (int i = 0; i < n; ++i)
            {
                const double m = 0.5 * ((double) l[i] + r[i]);
                for (int b = 0; b < numBands; ++b)
                {
                    const double y = detect2[(size_t) b].run (detect[(size_t) b].run (m));
                    energy[(size_t) b] = envCoeff * energy[(size_t) b] + (1 - envCoeff) * y * y;
                }
            }
            for (int b = 0; b < numBands; ++b)
                outDb[(size_t) b] = (float) (10.0 * std::log10 (2.0 * energy[(size_t) b] + 1e-12));   // sine-RMS -> peak-ish dBFS
        }

        // Make-room side: dip the bands where the priority is loud right now.
        //  - how loud overall: strongest band -45 dBFS -> nothing, -20 dBFS or louder -> full depth
        //  - which bands: only those within 15 dB of the priority's strongest band
        void apply (float* l, float* r, int n, const std::array<float, numBands>& priorityDb, float depthDb)
        {
            std::array<float, numBands> target {};
            float strongest = -100.0f;
            for (float v : priorityDb) strongest = std::max (strongest, v);
            const float overall = juce::jlimit (0.0f, 1.0f, (strongest + 45.0f) / 25.0f);
            bool any = false;
            for (int b = 0; b < numBands; ++b)
            {
                const float relevance = juce::jlimit (0.0f, 1.0f, (priorityDb[(size_t) b] - strongest + 15.0f) / 15.0f);
                target[(size_t) b] = depthDb * overall * relevance;
                any |= target[(size_t) b] > 0.01f || dipDb[(size_t) b] > 0.01f;
            }
            if (! any) return;
            for (int i = 0; i < n; ++i)
                for (int b = 0; b < numBands; ++b)
                {
                    float& d = dipDb[(size_t) b];
                    const float t = target[(size_t) b];
                    d = t > d ? att * d + (1 - att) * t : rel * d + (1 - rel) * t;
                    const double yl = cut[(size_t) (b * 2)].run (l[i]), yr = cut[(size_t) (b * 2 + 1)].run (r[i]);   // keep filters warm
                    if (d < 0.01f) continue;
                    const double k = 1.0 - std::exp (-d * 0.11512925464970229);      // 1 - 10^(-d/20)
                    l[i] = (float) (l[i] - k * yl);
                    r[i] = (float) (r[i] - k * yr);
                }
        }

        const std::array<float, numBands>& getDipsDb() const { return dipDb; }

    private:
        std::array<BandPass, numBands> detect, detect2;
        std::array<BandPass, numBands * 2> cut;
        std::array<double, numBands> energy {};
        std::array<float, numBands> dipDb {};
        double envCoeff = 0.99;
        float att = 0.9f, rel = 0.99f;
    };
}
