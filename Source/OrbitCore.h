#pragma once
// The Orbit:
//  - Tube stage (true bypass when off): triode-style grid/plate curve - the positive swing compresses
//    early (grid conduction), the negative swing runs longer (cutoff) -> even harmonics; the bias drifts
//    with level (blocking distortion), the supply sags on loud passages, coupling cap + Miller roll-off.
//  - Regen Throttle loop: a clocked feedback delay whose loop runs at a variable sample rate (bucket-
//    brigade style: lower clock = darker, grittier, aliasing repeats). The throttle sets the feedback.
#include <juce_dsp/juce_dsp.h>
#include "Dialogue.h"

namespace SolutionDSP
{
    class OrbitCore
    {
    public:
        struct Params { bool tubeOn = false; float drive = 0.4f, bias = 0.0f, tubeMix = 1.0f;
                        float regenMix = 0.0f, regen = 0.0f, regenMs = 375.0f, clockHz = 12000.0f; };

        static float triode (float x, float bias)
        {
            // Koren-flavoured asymmetric curve: soft early limit on the positive side, longer run on the negative
            const float u = x + bias;
            const float pos = u > 0 ? u / (1.0f + 1.6f * u) : u;
            const float y = pos > -1.4f ? std::tanh (pos * 1.15f) : -0.987f + (pos + 1.4f) * 0.02f;
            return y - std::tanh (bias * 1.15f) * 0.85f;          // remove most of the DC the bias adds
        }

        void prepare (double fs, int)
        {
            sampleRate = fs;
            loop.assign (2, std::vector<float> ((size_t) (fs * 2.5) + 2, 0.0f));
            reset();
        }
        void reset()
        {
            for (auto& c : loop) std::fill (c.begin(), c.end(), 0.0f);
            for (auto& f : couple) f.clear(); for (auto& f : miller) f.clear(); for (auto& f : aa) f.clear(); for (auto& f : recon) f.clear(); lastClock = -1;
            biasDrift = 0; sag = 0; wp = 0; clockPhase = 0; held[0] = held[1] = 0; dcPrev.fill (0); dcOut.fill (0); set = false;
        }
        bool isNeutral (const Params& p) const { return ! p.tubeOn && p.regenMix <= 0.0f; }

        void process (float* l, float* r, int n, const Params& p)
        {
            if (isNeutral (p)) { if (! idle) { reset(); idle = true; } return; }
            idle = false;
            if (! set) { for (auto& f : couple) f.set (1, sampleRate, 18.0, 0.707); for (auto& f : miller) f.set (0, sampleRate, 14000.0, 0.6); set = true; }
            float* io[2] = { l, r };
            // ---- tube ----
            if (p.tubeOn)
            {
                const float gain = 1.0f + p.drive * 11.0f, makeup = 1.0f / std::tanh (std::max (0.3f, gain * 0.35f));
                const double sagA = 1.0 - std::exp (-1.0 / (0.05 * sampleRate)), sagR = 1.0 - std::exp (-1.0 / (0.4 * sampleRate));
                const double drA = 1.0 - std::exp (-1.0 / (0.02 * sampleRate)), drR = 1.0 - std::exp (-1.0 / (0.3 * sampleRate));
                for (int i = 0; i < n; ++i)
                {
                    const float lvl = std::max (std::abs (l[i]), std::abs (r[i])) * gain;
                    sag += (lvl - sag) * (lvl > sag ? sagA : sagR);                      // supply sag: loud passages lose a little gain
                    biasDrift += (lvl - biasDrift) * (lvl > biasDrift ? drA : drR);       // grid charge shifts the bias
                    const float g = gain / (1.0f + 0.25f * (float) sag), b = p.bias * 0.5f + 0.12f * (float) std::min (2.0, biasDrift);
                    for (int c = 0; c < 2; ++c)
                    {
                        const float x = io[c][i], y = triode ((float) couple[c].run (x) * g, b) * makeup;
                        // DC blocker after the stage (output transformer / coupling), then the Miller roll-off
                        const float d = y - dcPrev[(size_t) c] + 0.9995f * dcOut[(size_t) c]; dcPrev[(size_t) c] = y; dcOut[(size_t) c] = d;
                        const float wet = (float) miller[c].run (d);
                        io[c][i] = x + (wet - x) * p.tubeMix;
                    }
                }
            }
            // ---- Regen Throttle clocked loop ----
            if (p.regenMix > 0.0f)
            {
                const int size = (int) loop[0].size();
                const double ratio = juce::jlimit (0.02, 1.0, p.clockHz / sampleRate);         // loop clock vs host rate
                if (std::abs (p.clockHz - lastClock) > 1.0f) { lastClock = p.clockHz; for (auto& f : recon) { const double a = f.z1, b = f.z2; f.set (0, sampleRate, std::min (0.45 * sampleRate, 0.42 * p.clockHz), 0.707); f.z1 = a; f.z2 = b; } for (auto& f : aa) { const double a = f.z1, b = f.z2; f.set (0, sampleRate, std::min (0.45 * sampleRate, 0.42 * p.clockHz), 0.707); f.z1 = a; f.z2 = b; } }
                const int d = juce::jlimit (1, size - 2, (int) (p.regenMs * 0.001 * sampleRate));
                const float fb = juce::jlimit (0.0f, 1.02f, p.regen * 1.05f);                    // past 1.0 it runs away - softly limited
                for (int i = 0; i < n; ++i)
                {
                    const int rd = (wp - d + size) % size;
                    float out[2];
                    for (int c = 0; c < 2; ++c) out[c] = (float) recon[c].run (loop[(size_t) c][(size_t) rd]);   // reconstruction filter, like a real BBD
                    // the loop is written only on clock ticks (sample-and-hold at the loop clock) -> BBD grit
                    float filt[2];
                    for (int c = 0; c < 2; ++c) filt[c] = (float) aa[c].run (io[c][i] + std::tanh (out[c] * fb * 1.2f) / 1.2f);   // anti-alias at the full rate
                    clockPhase += ratio;
                    if (clockPhase >= 1.0) { clockPhase -= 1.0; held[0] = filt[0]; held[1] = filt[1]; }
                    for (int c = 0; c < 2; ++c) { loop[(size_t) c][(size_t) wp] = held[c]; io[c][i] += out[c] * p.regenMix; }
                    wp = (wp + 1) % size;
                }
            }
        }
    private:
        double sampleRate = 48000.0, biasDrift = 0, sag = 0, clockPhase = 0;
        DBiquad couple[2], miller[2], aa[2], recon[2];
        std::array<float, 2> dcPrev {}, dcOut {};
        std::vector<std::vector<float>> loop;
        float held[2] {}; float lastClock = -1;
        int wp = 0; bool set = false, idle = true;
    };
}
