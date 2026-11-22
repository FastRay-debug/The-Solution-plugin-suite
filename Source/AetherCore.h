#pragma once
// The Aether:
//  - Particle cloud: granular grains taken from the last few seconds of audio. XY pad: X = texture
//    (grain size), Y = density. Scatter, stereo spread, pitch (octave up/down, fifth, shimmer), freeze.
//  - Delay: tempo-synced or free time, feedback, ping-pong; Focus (repeats narrow to the mids),
//    Soften (repeats darken), Tape (wow/flutter + saturation in the loop), Duck (repeats step aside
//    while you play, or while the Rack Link sidechain plays).
// Both mixes default to 0: the Aether's original sound passes untouched.
#include <juce_dsp/juce_dsp.h>
#include "Dialogue.h"   // DBiquad
#include <array>

namespace SolutionDSP
{
    class AetherCore
    {
    public:
        struct Params
        {
            float cloudMix = 0.0f, x = 0.5f, y = 0.5f, scatter = 0.6f, spread = 0.7f; int pitch = 0; bool freeze = false;
            float dlyMix = 0.0f, dlyMs = 375.0f, fb = 0.35f, focus = 0.0f, soften = 0.0f, duck = 0.0f, tape = 0.0f; bool pingPong = false;
            float sidechain = 0.0f;      // external key level 0..1 (Rack Link group), ducks the delay like the input does
        };
        static constexpr int maxGrains = 64;

        void prepare (double fs, int)
        {
            sampleRate = fs;
            cloud.assign (2, std::vector<float> ((size_t) (fs * 4.0) + 1, 0.0f));
            dly.assign (2, std::vector<float> ((size_t) (fs * 4.0) + 1, 0.0f));
            reset();
        }
        void reset()
        {
            for (auto& c : cloud) std::fill (c.begin(), c.end(), 0.0f);
            for (auto& c : dly) std::fill (c.begin(), c.end(), 0.0f);
            cw = dw = 0; for (auto& g : grains) g.active = false; spawnAcc = 0; duckEnv = 0; wow = 0; flutter = 0;
            for (auto& f : fbHp) f.clear(); for (auto& f : fbLp) f.clear(); lastFocus = -1; lastSoften = -1; delayNow = -1;
        }
        static bool isNeutral (const Params& p) { return p.cloudMix <= 0.0f && p.dlyMix <= 0.0f; }

        void process (float* l, float* r, int n, const Params& p)
        {
            if (isNeutral (p)) { if (! idle) { reset(); idle = true; } return; }
            idle = false;
            float* io[2] = { l, r };
            const int cs = (int) cloud[0].size(), ds = (int) dly[0].size();
            // ---- delay settings ----
            const double target = juce::jlimit (1.0, ds - 4.0, p.dlyMs * 0.001 * sampleRate);
            if (delayNow < 0) delayNow = target;
            if (std::abs (p.focus - lastFocus) > 1e-3f || std::abs (p.soften - lastSoften) > 1e-3f)
            {
                lastFocus = p.focus; lastSoften = p.soften;
                const double hpHz = 40.0 + p.focus * 700.0, lpHz = std::min (18000.0 - p.focus * 13000.0, 16000.0 - p.soften * 14500.0);
                for (auto& f : fbHp) { const double a = f.z1, b = f.z2; f.set (1, sampleRate, hpHz, 0.707); f.z1 = a; f.z2 = b; }
                for (auto& f : fbLp) { const double a = f.z1, b = f.z2; f.set (0, sampleRate, std::max (500.0, lpHz), 0.707); f.z1 = a; f.z2 = b; }
            }
            const double duckA = std::exp (-1.0 / (0.005 * sampleRate)), duckR = std::exp (-1.0 / (0.250 * sampleRate));
            const double wowInc = 0.6 / sampleRate, flutInc = 6.3 / sampleRate;
            // ---- grains ----
            const float density = 3.0f + p.y * 70.0f;                                  // grains per second
            const double grainLen = (0.02 + p.x * 0.38) * sampleRate;
            const double maxBack = (0.05 + p.scatter * 2.8) * sampleRate;
            const float norm = 1.0f / std::sqrt (std::max (1.0f, density * (float) (grainLen / sampleRate) * 0.5f));
            for (int i = 0; i < n; ++i)
            {
                const float inL = l[i], inR = r[i];
                // cloud: write (unless frozen), spawn, play
                if (! p.freeze) { cloud[0][(size_t) cw] = inL; cloud[1][(size_t) cw] = inR; cw = (cw + 1) % cs; }
                float cl = 0, cr = 0;
                if (p.cloudMix > 0.0f)
                {
                    spawnAcc += density / sampleRate;
                    while (spawnAcc >= 1.0) { spawnAcc -= 1.0; spawn (grainLen, maxBack, p); }
                    for (auto& g : grains)
                    {
                        if (! g.active) continue;
                        const double ph = g.age / g.len, w = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * ph);   // Hann
                        double rp = g.pos; while (rp < 0) rp += cs; while (rp >= cs) rp -= cs;
                        const int i0 = (int) rp, i1 = (i0 + 1) % cs; const float fr = (float) (rp - i0);
                        const float sL = cloud[0][(size_t) i0] + fr * (cloud[0][(size_t) i1] - cloud[0][(size_t) i0]);
                        const float sR = cloud[1][(size_t) i0] + fr * (cloud[1][(size_t) i1] - cloud[1][(size_t) i0]);
                        const float m = 0.5f * (sL + sR) * (float) w;
                        cl += m * g.gl; cr += m * g.gr;
                        g.pos += g.rate; g.age += 1.0; if (g.age >= g.len) g.active = false;
                    }
                    cl *= norm; cr *= norm;
                }
                // delay
                float wl = 0, wr = 0;
                if (p.dlyMix > 0.0f)
                {
                    delayNow += (target - delayNow) * 0.0005;                                  // glides when the tempo changes
                    wow += wowInc; if (wow >= 1) wow -= 1; flutter += flutInc; if (flutter >= 1) flutter -= 1;
                    const double mod = p.tape * (0.0018 * sampleRate * std::sin (juce::MathConstants<double>::twoPi * wow) + 0.00025 * sampleRate * std::sin (juce::MathConstants<double>::twoPi * flutter));
                    double rp = dw - (delayNow + mod); while (rp < 0) rp += ds;
                    const int i0 = (int) rp, i1 = (i0 + 1) % ds; const float fr = (float) (rp - i0);
                    float yl = dly[0][(size_t) i0] + fr * (dly[0][(size_t) i1] - dly[0][(size_t) i0]);
                    float yr = dly[1][(size_t) i0] + fr * (dly[1][(size_t) i1] - dly[1][(size_t) i0]);
                    // feedback: focus / soften filters, tape saturation
                    float fl = (float) fbLp[0].run (fbHp[0].run (yl)), fr2 = (float) fbLp[1].run (fbHp[1].run (yr));
                    if (p.tape > 0.0f) { const float d = 1.0f + p.tape * 3.0f; fl = std::tanh (fl * d) / d * (1.0f + p.tape * 0.5f); fr2 = std::tanh (fr2 * d) / d * (1.0f + p.tape * 0.5f); }
                    const float fbG = juce::jlimit (0.0f, 0.97f, p.fb);
                    const float writeL = (p.pingPong ? 0.5f * (inL + inR) : inL) + (p.pingPong ? fr2 : fl) * fbG;
                    const float writeR = (p.pingPong ? 0.0f : inR) + (p.pingPong ? fl : fr2) * fbG;
                    dly[0][(size_t) dw] = writeL; dly[1][(size_t) dw] = writeR; dw = (dw + 1) % ds;
                    // ducking: the repeats step aside while you play (or the sidechain does)
                    const double key = std::max ((double) std::max (std::abs (inL), std::abs (inR)), (double) p.sidechain);
                    duckEnv = key > duckEnv ? key + (duckEnv - key) * duckA : key + (duckEnv - key) * duckR;
                    const float dg = 1.0f - p.duck * (float) std::min (1.0, duckEnv * 4.0);
                    wl = yl * dg; wr = yr * dg;
                    lastDuck = dg;
                }
                l[i] = inL * (1.0f - 0.5f * p.cloudMix) + cl * p.cloudMix + wl * p.dlyMix;
                r[i] = inR * (1.0f - 0.5f * p.cloudMix) + cr * p.cloudMix + wr * p.dlyMix;
            }
            juce::ignoreUnused (io);
        }
        int activeGrains() const { int k = 0; for (auto& g : grains) k += g.active ? 1 : 0; return k; }
        float duckGain() const { return lastDuck; }
        // For the UI particle view: position (0..1 back in time) and pan of each live grain.
        template <typename Fn> void forEachGrain (Fn&& f) const { const double cs = (double) cloud[0].size(); for (auto& g : grains) if (g.active) f ((float) std::fmod ((cw - g.pos + cs) , cs) / (float) (sampleRate * 3.0), g.gr - g.gl, (float) (g.age / g.len)); }

        static double tempoMs (int division, double bpm)
        {   // 0 1/16, 1 1/8, 2 1/8 dotted, 3 1/4, 4 1/4 dotted, 5 1/2, 6 1 bar
            static const double beats[] = { 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 4.0 };
            return 60000.0 / std::max (30.0, bpm) * beats[juce::jlimit (0, 6, division)];
        }

    private:
        struct Grain { bool active = false; double pos = 0, rate = 1, age = 0, len = 1; float gl = 0.7f, gr = 0.7f; };
        void spawn (double len, double maxBack, const Params& p)
        {
            for (auto& g : grains)
            {
                if (g.active) continue;
                const double back = len * 1.5 + rng.nextDouble() * maxBack;
                static const double shimmer[] = { 1.0, 2.0 };
                const double rate = p.pitch == 1 ? 2.0 : p.pitch == 2 ? 0.5 : p.pitch == 3 ? 1.5 : p.pitch == 4 ? shimmer[rng.nextInt (2)] : 1.0;
                g.pos = cw - back - (rate > 1.0 ? len * (rate - 1.0) : 0.0); g.rate = rate; g.len = len; g.age = 0; g.active = true;
                const float pan = (rng.nextFloat() * 2.0f - 1.0f) * p.spread;
                g.gl = std::cos ((pan + 1.0f) * 0.25f * juce::MathConstants<float>::pi); g.gr = std::sin ((pan + 1.0f) * 0.25f * juce::MathConstants<float>::pi);
                return;
            }
        }
        double sampleRate = 48000.0;
        std::vector<std::vector<float>> cloud, dly;
        int cw = 0, dw = 0;
        std::array<Grain, maxGrains> grains;
        double spawnAcc = 0, duckEnv = 0, wow = 0, flutter = 0, delayNow = -1;
        DBiquad fbHp[2], fbLp[2];
        float lastFocus = -1, lastSoften = -1, lastDuck = 1.0f;
        bool idle = true;
        juce::Random rng { 1234 };
    };
}
