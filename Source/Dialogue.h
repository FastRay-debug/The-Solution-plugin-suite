#pragma once
// The Solution - voice-activated dialogue ducking and the transient shaper.
// No allocation, no locks: everything here runs on the audio thread.
#include <juce_core/juce_core.h>
#include <array>
#include <cmath>
#include <algorithm>

namespace SolutionDSP
{
    // Small double-precision RBJ biquad.
    struct DBiquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double run (double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
        void clear() { z1 = z2 = 0; }
        void set (int type, double fs, double f, double q, double gainDb = 0.0)   // 0 LP, 1 HP, 2 peak
        {
            const double w = juce::MathConstants<double>::twoPi * std::min (f, 0.45 * fs) / fs, s = std::sin (w), c = std::cos (w);
            const double al = s / (2 * q), A = std::pow (10.0, gainDb / 40.0);
            double n0, n1, n2, d0, d1, d2;
            if (type == 0)      { n0 = (1 - c) / 2; n1 = 1 - c; n2 = n0; d0 = 1 + al; d1 = -2 * c; d2 = 1 - al; }
            else if (type == 1) { n0 = (1 + c) / 2; n1 = -(1 + c); n2 = n0; d0 = 1 + al; d1 = -2 * c; d2 = 1 - al; }
            else                { n0 = 1 + al * A; n1 = -2 * c; n2 = 1 - al * A; d0 = 1 + al / A; d1 = -2 * c; d2 = 1 - al / A; }
            b0 = n0 / d0; b1 = n1 / d0; b2 = n2 / d0; a1 = d1 / d0; a2 = d2 / d0;
        }
    };

    //==========================================================================
    // Voice activity detector for a dialogue track. Speech = most energy in the speech band
    // (300 Hz - 3.4 kHz), clearly above the track's own noise floor, and moving at syllable rate
    // (big frame-to-frame level swings). Steady music, pads and noise fail the last two tests.
    class VoiceDetector
    {
    public:
        void prepare (double fs)
        {
            sampleRate = fs; frameLen = std::max (1, (int) std::round (0.010 * fs));
            hp.set (1, fs, 300.0, 0.707); lp.set (0, fs, 3400.0, 0.707);
            reset();
        }
        void reset()
        {
            hp.clear(); lp.clear(); frameBand = frameTotal = 0; frameCount = 0; floorDb = -60.0; hold = 0; confidence = 0;
            history.fill (-100.0); histPos = 0;
        }
        void process (const float* l, const float* r, int n)
        {
            for (int i = 0; i < n; ++i)
            {
                const double x = 0.5 * ((double) l[i] + (double) (r != nullptr ? r[i] : l[i]));
                const double b = lp.run (hp.run (x));
                frameBand += b * b; frameTotal += x * x;
                if (++frameCount >= frameLen) endFrame();
            }
        }
        float getConfidence() const { return (float) confidence; }   // 0..1, with a 300 ms hangover

    private:
        void endFrame()
        {
            const double bandDb = 10.0 * std::log10 (frameBand / frameLen + 1e-12), totalDb = 10.0 * std::log10 (frameTotal / frameLen + 1e-12);
            frameBand = frameTotal = 0; frameCount = 0;
            // Noise floor: drops at once, creeps up 3 dB/s, so pauses between words keep it honest.
            floorDb = bandDb < floorDb ? bandDb : std::min (bandDb, floorDb + 0.03);
            history[(size_t) histPos] = bandDb; histPos = (histPos + 1) % (int) history.size();
            double mean = 0; for (auto v : history) mean += v; mean /= (double) history.size();
            double var = 0;  for (auto v : history) var += (v - mean) * (v - mean); var /= (double) history.size();
            const bool loudEnough = bandDb > -55.0 && bandDb > floorDb + 9.0;
            const bool speechBand = bandDb - totalDb > -3.5;           // at least ~45% of the energy is in 300 Hz - 3.4 kHz
            const bool syllabic   = std::sqrt (var) > 4.0;             // level swings of syllables over the last 0.5 s
            if (loudEnough && speechBand && syllabic) hold = 30;       // 300 ms hangover covers gaps between words
            else if (hold > 0) --hold;
            const double target = hold > 0 ? 1.0 : 0.0;
            confidence += (target - confidence) * (target > confidence ? 0.5 : 0.1);
        }
        double sampleRate = 48000.0;
        int frameLen = 480, frameCount = 0, hold = 0, histPos = 0;
        double frameBand = 0, frameTotal = 0, floorDb = -60.0, confidence = 0;
        std::array<double, 50> history {};
        DBiquad hp, lp;
    };

    //==========================================================================
    // Ducks a music track under dialogue: mostly the speech band (a wide bell at 2.2 kHz) plus a
    // little broadband, smoothly (40 ms down, 450 ms back up), only while a voice is detected.
    class DialogueDucker
    {
    public:
        void prepare (double fs) { sampleRate = fs; g = 0; lastBellDb = 1000; for (auto& b : bell) b.clear(); }
        void reset() { g = 0; lastBellDb = 1000; for (auto& b : bell) b.clear(); }
        void process (float* l, float* r, int n, float voice, float depthDb)
        {
            const double down = std::exp (-1.0 / (0.040 * sampleRate)), up = std::exp (-1.0 / (0.450 * sampleRate));
            for (int start = 0; start < n; start += 32)
            {
                const int len = std::min (32, n - start);
                for (int i = 0; i < len; ++i) { const double c = voice > g ? down : up; g = voice + (g - voice) * c; }
                const double bellDb = depthDb * g, broad = std::pow (10.0, depthDb * 0.3 * g / 20.0);
                if (std::abs (bellDb - lastBellDb) > 0.01) { for (auto& b : bell) { const double z1 = b.z1, z2 = b.z2; b.set (2, sampleRate, 2200.0, 0.6, bellDb); b.z1 = z1; b.z2 = z2; } lastBellDb = bellDb; }
                for (int i = start; i < start + len; ++i)
                {
                    l[i] = (float) (bell[0].run (l[i]) * broad);
                    if (r != nullptr) r[i] = (float) (bell[1].run (r[i]) * broad);
                }
            }
            reductionDb = (float) (depthDb * g);
        }
        float getReductionDb() const { return reductionDb; }
    private:
        double sampleRate = 48000.0, g = 0, lastBellDb = 1000;
        float reductionDb = 0.0f;
        DBiquad bell[2];
    };

    //==========================================================================
    // Transient shaper: two envelope pairs on the stereo-linked level. Attack = fast vs slow attack
    // follower (positive at onsets); sustain = slow vs fast release follower (positive in tails).
    class TransientShaper
    {
    public:
        void prepare (double fs)
        {
            auto co = [fs] (double ms) { return std::exp (-1.0 / (ms * 0.001 * fs)); };
            fastAtk = co (0.5); slowAtk = co (25.0); relShort = co (40.0); relLong = co (400.0); gainSmooth = co (1.0);
            reset();
        }
        void reset() { eFast = eSlow = eRelS = eRelL = 0; gain = 1.0; }
        // attack / sustain in -1..+1 (-100%..+100%)
        void process (float* l, float* r, int n, float attack, float sustain)
        {
            for (int i = 0; i < n; ++i)
            {
                const double x = std::max (std::abs ((double) l[i]), r != nullptr ? std::abs ((double) r[i]) : 0.0);
                eFast = x > eFast ? x + (eFast - x) * fastAtk : x + (eFast - x) * relShort;
                eSlow = eFast + (eSlow - eFast) * slowAtk;                                         // lags the fast follower by ~25 ms
                eRelS = x > eRelS ? x + (eRelS - x) * fastAtk : x + (eRelS - x) * relShort;   // both jump to the peak, so the
                eRelL = x > eRelL ? x + (eRelL - x) * fastAtk : x + (eRelL - x) * relLong;    // hit itself reads 0 dB sustain
                const double tdb = std::max (0.0, 20.0 * std::log10 ((eFast + 1e-6) / (eSlow + 1e-6)));   // >0 only while a hit rises
                const double sdb = 20.0 * std::log10 ((eRelL + 1e-6) / (eRelS + 1e-6));          // >0 in tails
                const double inHit = std::clamp (tdb / 3.0, 0.0, 1.0);                         // sustain steps aside while a hit is rising
                const double gdb = std::clamp (attack * 1.2 * tdb + sustain * 0.8 * sdb * (1.0 - inHit), -15.0, 15.0);
                const double target = std::pow (10.0, gdb / 20.0);
                gain = target < gain ? target : target + (gain - target) * gainSmooth;   // drop at once on a hit, rise smoothly
                l[i] = (float) (l[i] * gain);
                if (r != nullptr) r[i] = (float) (r[i] * gain);
            }
        }
    private:
        double fastAtk = 0, slowAtk = 0, relShort = 0, relLong = 0, gainSmooth = 0;
        double eFast = 0, eSlow = 0, eRelS = 0, eRelL = 0, gain = 1.0;
    };
}
