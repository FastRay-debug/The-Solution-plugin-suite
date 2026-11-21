#pragma once

#include <juce_dsp/juce_dsp.h>
#include "Metering.h"
#include <array>
#include <atomic>
#include <cmath>

// One-button engineer tricks. All allocation-free on the audio thread.
namespace SolutionDSP
{
    // Small helper: constant-peak bandpass, used for "x - k * band(x)" dynamic cuts.
    struct BandPass
    {
        double b0 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void set (double fs, double f, double q)
        {
            const double w = juce::MathConstants<double>::twoPi * std::min (f, 0.45 * fs) / fs, al = std::sin (w) / (2 * q), a0 = 1 + al;
            b0 = al / a0; b2 = -al / a0; a1 = -2 * std::cos (w) / a0; a2 = (1 - al) / a0;
        }
        double run (double x) { const double y = b0 * x + z1; z1 = -a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
        void clear() { z1 = z2 = 0; }
    };

    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void lowPass (double fs, double f, double q = 0.7071)  { rbj (fs, f, q, 0); }
        void highPass (double fs, double f, double q = 0.7071) { rbj (fs, f, q, 1); }
        double run (double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
        void clear() { z1 = z2 = 0; }
    private:
        void rbj (double fs, double f, double q, int type)
        {
            const double w = juce::MathConstants<double>::twoPi * std::min (f, 0.45 * fs) / fs, al = std::sin (w) / (2 * q), c = std::cos (w), a0 = 1 + al;
            if (type == 0) { b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0; }
            else           { b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = b0; }
            a1 = -2 * c / a0; a2 = (1 - al) / a0;
        }
    };

    //==========================================================================
    // Mono the lows (crossover, Linkwitz-Riley 4th order) + Phase Guard (keeps mono-safe width).
    class MonoLows
    {
    public:
        void prepare (double fs, int maxBlock)
        {
            sampleRate = fs;
            juce::dsp::ProcessSpec spec { fs, (juce::uint32) maxBlock, 2 };
            lp.prepare (spec); hp.prepare (spec);
            lp.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
            hp.setType (juce::dsp::LinkwitzRileyFilterType::highpass);
            guardSmooth = (float) std::exp (-1.0 / (0.050 * fs));
            reset();
        }
        void reset() { lp.reset(); hp.reset(); corrAvg = 1.0; sideGain = 1.0f; }
        float getGuardReductionDb() const { return juce::Decibels::gainToDecibels (sideGain); }

        void process (float* l, float* r, int n, float crossoverHz, bool guard)
        {
            if (crossoverHz >= 20.0f)
            {
                lp.setCutoffFrequency (crossoverHz); hp.setCutoffFrequency (crossoverHz);
                for (int i = 0; i < n; ++i)
                {
                    const float lowL = lp.processSample (0, l[i]), lowR = lp.processSample (1, r[i]);
                    const float hiL  = hp.processSample (0, l[i]), hiR  = hp.processSample (1, r[i]);
                    const float lowMono = 0.5f * (lowL + lowR);
                    l[i] = lowMono + hiL; r[i] = lowMono + hiR;
                }
            }
            if (! guard) { sideGain = 1.0f; return; }

            // Correlation over ~300 ms; below 0 the mix starts cancelling in mono -> pull width back.
            double sll = 0, srr = 0, slr = 0;
            for (int i = 0; i < n; ++i) { sll += (double) l[i] * l[i]; srr += (double) r[i] * r[i]; slr += (double) l[i] * r[i]; }
            if (sll + srr > 1e-9)
            {
                const double c = slr / (std::sqrt (sll * srr) + 1e-12);
                const double a = std::exp (-(double) n / (0.3 * sampleRate));
                corrAvg = a * corrAvg + (1 - a) * c;
            }
            const float target = corrAvg >= 0.2 ? 1.0f : (float) juce::jlimit (0.25, 1.0, 1.0 - (0.2 - corrAvg) * 1.5);
            for (int i = 0; i < n; ++i)
            {
                sideGain = guardSmooth * sideGain + (1 - guardSmooth) * target;
                const float m = 0.5f * (l[i] + r[i]), s = 0.5f * (l[i] - r[i]) * sideGain;
                l[i] = m + s; r[i] = m - s;
            }
        }

    private:
        juce::dsp::LinkwitzRileyFilter<float> lp, hp;
        double sampleRate = 48000.0, corrAvg = 1.0;
        float sideGain = 1.0f, guardSmooth = 0.99f;
    };

    //==========================================================================
    // Smooth: dynamic harshness / sibilance control. Watches 4-9 kHz against the 1-3 kHz body
    // and only cuts when the top gets sharp (up to -6 dB x amount).
    class Smoother
    {
    public:
        void prepare (double fs)
        {
            harsh.set (fs, 6500.0, 0.9); body.set (fs, 2000.0, 0.7);
            for (auto& c : cut) c.set (fs, 6500.0, 0.9);
            env = std::exp (-1.0 / (0.010 * fs)); att = (float) std::exp (-1.0 / (0.003 * fs)); rel = (float) std::exp (-1.0 / (0.120 * fs));
            reset();
        }
        void reset() { harsh.clear(); body.clear(); for (auto& c : cut) c.clear(); eh = eb = 0; depth = 0; }
        float getReductionDb() const { return lastDb; }

        void process (float* l, float* r, int n, float amount)
        {
            if (amount <= 0.0005f) { depth = 0; lastDb = 0; return; }
            float maxDb = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                const double m = 0.5 * ((double) l[i] + r[i]);
                const double h = harsh.run (m), b = body.run (m);
                eh = env * eh + (1 - env) * h * h; eb = env * eb + (1 - env) * b * b;
                float target = 0.0f;
                if (eh > 1e-8)
                {
                    const double ratioDb = 10.0 * std::log10 (eh / (eb + 1e-12));   // typical mixes: about -8 dB
                    target = (float) juce::jlimit (0.0, 1.0, (ratioDb + 4.0) / 6.0);
                }
                depth = target > depth ? att * depth + (1 - att) * target : rel * depth + (1 - rel) * target;
                const double dipDb = 6.0 * amount * depth;
                const double k = 1.0 - std::pow (10.0, -dipDb / 20.0);
                l[i] = (float) (l[i] - k * cut[0].run (l[i]));
                r[i] = (float) (r[i] - k * cut[1].run (r[i]));
                maxDb = std::max (maxDb, (float) dipDb);
            }
            lastDb = -maxDb;
        }

    private:
        BandPass harsh, body; std::array<BandPass, 2> cut;
        double env = 0.99, eh = 0, eb = 0;
        float att = 0.9f, rel = 0.99f, depth = 0, lastDb = 0;
    };

    //==========================================================================
    // Phone bass: generates harmonics of 40-120 Hz so bass still reads on phones/laptops/earbuds.
    class PhoneBass
    {
    public:
        void prepare (double fs)
        {
            bandHp.highPass (fs, 40.0); bandLp.lowPass (fs, 120.0);
            harmHp.highPass (fs, 140.0); harmLp.lowPass (fs, 700.0);
            harmHp2.highPass (fs, 140.0);
            reset();
        }
        void reset() { bandHp.clear(); bandLp.clear(); harmHp.clear(); harmLp.clear(); harmHp2.clear(); }

        void process (float* l, float* r, int n, float amount)
        {
            if (amount <= 0.0005f) return;
            for (int i = 0; i < n; ++i)
            {
                const double mono = 0.5 * ((double) l[i] + r[i]);
                const double bass = bandLp.run (bandHp.run (mono));
                const double shaped = std::tanh (6.0 * bass);                 // rich in harmonics of the bass notes
                const double harmonics = harmLp.run (harmHp2.run (harmHp.run (shaped)));
                const float add = (float) (amount * 0.35 * harmonics);
                l[i] += add; r[i] += add;
            }
        }

    private:
        Biquad bandHp, bandLp, harmHp, harmHp2, harmLp;
    };

    //==========================================================================
    // Loudness-matched A/B: tracks the short-term loudness of the processed and original signal.
    class LoudnessMatcher
    {
    public:
        void prepare (double fs) { wet.prepare (fs); dry.prepare (fs); smooth = (float) std::exp (-1.0 / (1.0 * fs)); gainDb = 0.0f; }
        void measure (const float* wl, const float* wr, const float* dl, const float* dr, int n) { wet.process (wl, wr, n); dry.process (dl, dr, n); }
        // Gain to apply to the original so it plays as loud as the processed sound.
        float update (int n)
        {
            const float w = wet.momentary.load(), d = dry.momentary.load();
            const float target = (w > -60.0f && d > -60.0f) ? juce::jlimit (-18.0f, 18.0f, w - d) : gainDb;
            const float a = std::pow (smooth, (float) n);
            gainDb = a * gainDb + (1 - a) * target;
            return gainDb;
        }
        float getMatchDb() const { return gainDb; }
    private:
        LoudnessMeter wet, dry;
        float smooth = 0.99f, gainDb = 0.0f;
    };

    //==========================================================================
    // Streaming preview: what each service's loudness normalisation does to this track.
    struct StreamingService { const char* name; float targetLufs; bool turnsUp; float peakCeilingDb; };
    inline const std::array<StreamingService, 6>& streamingServices()
    {
        static const std::array<StreamingService, 6> s { {
            { "Spotify",      -14.0f, true,  -1.0f },    // turns quiet tracks up only as far as -1 dBTP allows
            { "Apple Music",  -16.0f, true,  -1.0f },
            { "YouTube",      -14.0f, false,  0.0f },    // turns loud tracks down only
            { "Amazon Music", -14.0f, false,  0.0f },
            { "Tidal",        -14.0f, false,  0.0f },
            { "Deezer",       -15.0f, false,  0.0f } } };
        return s;
    }
    inline float streamingGainDb (const StreamingService& s, float integratedLufs, float truePeakDb)
    {
        if (integratedLufs <= -69.0f) return 0.0f;
        float g = s.targetLufs - integratedLufs;
        if (g > 0.0f) g = s.turnsUp ? std::max (0.0f, std::min (g, s.peakCeilingDb - truePeakDb)) : 0.0f;
        return std::max (g, -30.0f);
    }
}
