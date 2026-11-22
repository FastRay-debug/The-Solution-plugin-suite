#pragma once
// The Land: outdoor acoustics.
//  - Ground reflection: image source under the ground. The reflected path is longer by
//    sqrt(d^2 + (hs+hl)^2) - sqrt(d^2 + (hs-hl)^2); the bounce interferes with the direct sound (the classic
//    ground-effect notch) and is coloured by the surface (grass, concrete, snow, water, sand, forest, gravel).
//  - Air absorption: high frequencies fade with distance (both paths).
//  - Wind: the bounce drifts and flutters (path length and level), the way it does outdoors.
//  - Terrain: Open field / Valley (a far-slope slap) / Canyon (two walls) / Urban street (flutter echo).
#include <juce_dsp/juce_dsp.h>
#include "Dialogue.h"

namespace SolutionDSP
{
    class LandCore
    {
    public:
        struct Params { int ground = 0, terrain = 0; float srcH = 1.5f, lisH = 1.7f, dist = 10.0f, wind = 0.0f, mix = 1.0f; bool air = true; };
        struct Surface { const char* name; float gain, lpHz; bool invertHigh; };
        static Surface surface (int g)
        {
            static const Surface s[] = { { "Grass", 0.75f, 1500.0f, true }, { "Concrete", 0.97f, 16000.0f, false }, { "Snow", 0.5f, 600.0f, true },
                                         { "Water", 0.99f, 18000.0f, false }, { "Sand", 0.8f, 2500.0f, false }, { "Forest floor", 0.55f, 800.0f, true }, { "Gravel", 0.82f, 4000.0f, false } };
            return s[juce::jlimit (0, 6, g)];
        }
        static float pathDifference (float d, float hs, float hl) { return std::sqrt (d * d + (hs + hl) * (hs + hl)) - std::sqrt (d * d + (hs - hl) * (hs - hl)); }
        static float firstNotchHz (float d, float hs, float hl) { return 343.0f / (2.0f * std::max (1e-4f, pathDifference (d, hs, hl))); }   // hard ground

        void prepare (double fs, int)
        {
            sampleRate = fs;
            line.assign ((size_t) (fs * 1.6) + 2, 0.0f);
            reset();
        }
        void reset() { std::fill (line.begin(), line.end(), 0.0f); wp = 0; for (auto& f : airD) f.clear(); for (auto& f : airR) f.clear(); for (auto& f : groundLp) f.clear(); windPh = 0; windA = windB = 0; lastKey = -1; gust = 0; }

        void process (float* l, float* r, int n, const Params& p)
        {
            const auto surf = surface (p.ground);
            const float dr = pathDifference (p.dist, p.srcH, p.lisH), r1 = std::sqrt (p.dist * p.dist + (p.srcH - p.lisH) * (p.srcH - p.lisH)), r2 = r1 + dr;
            const float key = p.ground * 1000.0f + p.dist + (p.air ? 0.5f : 0.0f);
            if (key != lastKey)
            {
                lastKey = key;
                const double airHz = p.air ? juce::jlimit (1500.0, 20000.0, 20000.0 / (1.0 + p.dist / 60.0)) : 20000.0;
                for (auto& f : airD) { const double a = f.z1, b = f.z2; f.set (0, sampleRate, airHz, 0.707); f.z1 = a; f.z2 = b; }
                const double airHz2 = p.air ? juce::jlimit (1200.0, 20000.0, 20000.0 / (1.0 + r2 / 60.0)) : 20000.0;
                for (auto& f : airR) { const double a = f.z1, b = f.z2; f.set (0, sampleRate, std::min (airHz2, (double) surf.lpHz), 0.707); f.z1 = a; f.z2 = b; }
            }
            const double baseDelay = dr / 343.0 * sampleRate;
            const float baseGain = surf.gain * r1 / r2;
            const int size = (int) line.size();
            // terrain echoes: delay (s) and gain of each extra path
            float tDel[4] = { 0, 0, 0, 0 }, tGain[4] = { 0, 0, 0, 0 }; int tN = 0;
            switch (p.terrain)
            {
                case 1: tDel[0] = (2.0f * (60.0f + p.dist * 0.5f)) / 343.0f; tGain[0] = 0.22f; tN = 1; break;                 // valley: far slope
                case 2: tDel[0] = 0.052f; tGain[0] = 0.35f; tDel[1] = 0.087f; tGain[1] = 0.3f; tDel[2] = 0.139f; tGain[2] = 0.18f; tN = 3; break;   // canyon walls
                case 3: for (int k = 0; k < 4; ++k) { tDel[k] = 0.058f * (k + 1); tGain[k] = 0.32f * std::pow (0.6f, (float) k); } tN = 4; break;  // street flutter
                default: break;
            }
            const double windInc = (0.15 + 0.6 * p.wind) / sampleRate;
            for (int i = 0; i < n; ++i)
            {
                const float m = 0.5f * (l[i] + r[i]);
                line[(size_t) wp] = m;
                // wind: two slow random walks move the bounce's path and level
                windPh += windInc;
                if (windPh >= 1.0) { windPh -= 1.0; windA = windB; windB = rng.nextFloat() * 2.0f - 1.0f; }
                const float wv = windA + (windB - windA) * (float) (windPh * windPh * (3 - 2 * windPh));
                const double del = baseDelay + p.wind * wv * 0.0006 * sampleRate;           // up to ±0.6 ms
                const float gain = baseGain * (1.0f + p.wind * 0.35f * wv);
                auto tap = [&] (double d) { double rp = wp - d; while (rp < 0) rp += size; const int i0 = (int) rp, i1 = (i0 + 1) % size; const float fr = (float) (rp - i0); return line[(size_t) i0] + fr * (line[(size_t) i1] - line[(size_t) i0]); };
                const float bounce = (float) airR[0].run (tap (std::max (1.0, del))) * gain * (surf.invertHigh ? 1.0f : 1.0f);
                float echoes = 0;
                for (int k = 0; k < tN; ++k) echoes += tap (tDel[k] * sampleRate) * tGain[k];
                const float dL = (float) airD[0].run (l[i]), dR = (float) airD[1].run (r[i]);
                const float wetL = dL + bounce + echoes * 0.9f, wetR = dR + bounce + echoes * 1.1f;
                l[i] = l[i] + (wetL - l[i]) * p.mix; r[i] = r[i] + (wetR - r[i]) * p.mix;
                wp = (wp + 1) % size;
            }
        }
    private:
        double sampleRate = 48000.0, windPh = 0;
        std::vector<float> line; int wp = 0;
        DBiquad airD[2], airR[1], groundLp[1];
        float windA = 0, windB = 0, lastKey = -1, gust = 0;
        juce::Random rng { 99 };
    };
}
