#pragma once
// The Bunker's low-end core:
//  1. Sub-harmonic synthesizer - follows the bass fundamental (40-160 Hz) and adds a clean octave below
//     (zero-crossing flip-flop divider shaped by the band's envelope, then low-passed to a near-sine).
//  2. Dynamic low-end filter  - a 120 Hz shelf that only dips when the lows boom past the rest of the signal.
//  3. Split-Core M/S          - three Linkwitz-Riley bands (sum flat), each with its own stereo width.
// Everything defaults to neutral so the original Bunker sound is untouched until you turn something.
#include <juce_dsp/juce_dsp.h>
#include "Dialogue.h"      // DBiquad

namespace SolutionDSP
{
    class BunkerCore
    {
    public:
        struct Params
        {
            float subAmount = 0.0f;      // 0..1  (level of the octave-down voice)
            float subTrackHz = 110.0f;   // highest fundamental the divider follows
            float dynLow = 0.0f;         // 0..1  dynamic low-shelf amount
            float wLow = 1.0f, wMid = 1.0f, wHigh = 1.0f;   // 0..2 per-band width
            float x1 = 120.0f, x2 = 2500.0f;                // crossovers
        };

        void prepare (double fs, int maxBlock)
        {
            sampleRate = fs;
            juce::dsp::ProcessSpec spec { fs, (juce::uint32) maxBlock, 2 };
            for (auto* f : { &lp1, &hp1, &lp2, &hp2, &ap2 }) { f->prepare (spec); }
            for (auto& d : detLo) d.set (0, fs, 120.0, 0.707); for (auto& d : detHi) d.set (1, fs, 300.0, 0.707);
            lp1.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);  hp1.setType (juce::dsp::LinkwitzRileyFilterType::highpass);
            lp2.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);  hp2.setType (juce::dsp::LinkwitzRileyFilterType::highpass);
            ap2.setType (juce::dsp::LinkwitzRileyFilterType::allpass);   // keeps the low band in phase with mid+high
            low.setSize (2, maxBlock); mid.setSize (2, maxBlock); high.setSize (2, maxBlock);
            reset();
        }
        void reset()
        {
            for (auto* f : { &lp1, &hp1, &lp2, &hp2, &ap2 }) f->reset(); for (auto& d : detLo) d.clear(); for (auto& d : detHi) d.clear();
            subBand[0].clear(); subBand[1].clear(); subLp[0].clear(); subLp[1].clear();
            sign = 1.0f; lastBand = 0; env = 0; subTrackSet = -1;
            lowEnv = highEnv = 1e-6; shelfDb = 0; lastShelf = 1000; for (auto& s : shelf) s.clear();
            subOut = 0.0f;
        }

        // Neutral settings do nothing at all (bit-exact pass-through).
        static bool isNeutral (const Params& p)
        {
            return p.subAmount <= 0.0f && p.dynLow <= 0.0f && std::abs (p.wLow - 1.0f) < 1e-4f && std::abs (p.wMid - 1.0f) < 1e-4f && std::abs (p.wHigh - 1.0f) < 1e-4f;
        }

        void process (float* l, float* r, int n, const Params& p)
        {
            if (isNeutral (p)) { reset(); return; }
            if (n > low.getNumSamples())          // a host block bigger than announced: work in pieces
            { for (int off = 0; off < n; off += low.getNumSamples()) process (l + off, r + off, std::min (low.getNumSamples(), n - off), p); return; }
            // ---- 1. sub-harmonic synth (mono, from the low band) ----
            if (p.subAmount > 0.0f)
            {
                if (std::abs (p.subTrackHz - subTrackSet) > 0.5f)
                {
                    subTrackSet = p.subTrackHz;
                    for (auto& b : subBand) { const double z1 = b.z1, z2 = b.z2; b.set (0, sampleRate, p.subTrackHz, 0.707, 0); b.z1 = z1; b.z2 = z2; }   // LP: the fundamental region
                    for (auto& b : subLp)   { const double z1 = b.z1, z2 = b.z2; b.set (0, sampleRate, p.subTrackHz * 0.55, 0.707, 0); b.z1 = z1; b.z2 = z2; }
                }
                const double envA = std::exp (-1.0 / (0.004 * sampleRate)), envR = std::exp (-1.0 / (0.080 * sampleRate));
                const float amt = juce::jlimit (0.0f, 1.0f, p.subAmount) * 2.0f;     // up to +6 dB relative to the band
                for (int i = 0; i < n; ++i)
                {
                    const double x = 0.5 * (l[i] + r[i]);
                    const double band = subBand[1].run (subBand[0].run (x));
                    const double a = std::abs (band);
                    env = a > env ? a + (env - a) * envA : a + (env - a) * envR;
                    if (lastBand <= 0.0 && band > 0.0 && env > 1e-4) sign = -sign;             // every rising zero crossing: half the frequency
                    lastBand = band;
                    const double sq = sign * env * 1.27;                                        // square at f/2, follows the level
                    const float s = (float) subLp[1].run (subLp[0].run (sq)) * amt;            // two LP stages: near-sine
                    subOut = s; l[i] += s; r[i] += s;
                }
            }
            // ---- 2. dynamic low-end filter ----
            if (p.dynLow > 0.0f)
            {
                const double a = std::exp (-1.0 / (0.010 * sampleRate)), rel = std::exp (-1.0 / (0.200 * sampleRate));
                for (int start = 0; start < n; start += 32)
                {
                    const int len = std::min (32, n - start);
                    for (int i = start; i < start + len; ++i)
                    {
                        const double m = 0.5 * (l[i] + r[i]);
                        const double lo = detLo[1].run (detLo[0].run (m)), hi = detHi[1].run (detHi[0].run (m));
                        const double lp = lo * lo, hp = hi * hi;
                        lowEnv = lp > lowEnv ? lp + (lowEnv - lp) * a : lp + (lowEnv - lp) * rel;
                        highEnv = hp > highEnv ? hp + (highEnv - hp) * a : hp + (highEnv - hp) * rel;
                    }
                    const double ratioDb = 10.0 * std::log10 ((lowEnv + 1e-12) / (highEnv + 1e-12));
                    const double over = std::max (0.0, ratioDb - 6.0);                       // lows more than 6 dB above the rest = boom
                    const double target = -std::min (12.0, over * 0.8) * p.dynLow;
                    shelfDb += (target - shelfDb) * 0.2;
                    if (std::abs (shelfDb - lastShelf) > 0.05)
                    { for (auto& s : shelf) { const double z1 = s.z1, z2 = s.z2; s.set (3, sampleRate, 120.0, 0.7, shelfDb); s.z1 = z1; s.z2 = z2; } lastShelf = shelfDb; }
                    for (int i = start; i < start + len; ++i) { l[i] = (float) shelf[0].run (l[i]); r[i] = (float) shelf[1].run (r[i]); }
                }
            }
            else shelfDb = 0;
            // ---- 3. split-core M/S ----
            if (std::abs (p.wLow - 1.0f) > 1e-4f || std::abs (p.wMid - 1.0f) > 1e-4f || std::abs (p.wHigh - 1.0f) > 1e-4f)
            {
                lp1.setCutoffFrequency (juce::jlimit (40.0f, 400.0f, p.x1)); hp1.setCutoffFrequency (juce::jlimit (40.0f, 400.0f, p.x1));
                lp2.setCutoffFrequency (juce::jlimit (600.0f, 8000.0f, p.x2)); hp2.setCutoffFrequency (juce::jlimit (600.0f, 8000.0f, p.x2)); ap2.setCutoffFrequency (juce::jlimit (600.0f, 8000.0f, p.x2));
                for (int c = 0; c < 2; ++c) { const float* src = c ? r : l; low.copyFrom (c, 0, src, n); mid.copyFrom (c, 0, src, n); }
                auto blk = [n] (juce::AudioBuffer<float>& b) { return juce::dsp::AudioBlock<float> (b.getArrayOfWritePointers(), 2, (size_t) n); };
                { auto b = blk (low); lp1.process (juce::dsp::ProcessContextReplacing<float> (b)); }
                { auto b = blk (mid); hp1.process (juce::dsp::ProcessContextReplacing<float> (b)); }
                for (int c = 0; c < 2; ++c) high.copyFrom (c, 0, mid, c, 0, n);
                { auto b = blk (mid); lp2.process (juce::dsp::ProcessContextReplacing<float> (b)); }
                { auto b = blk (high); hp2.process (juce::dsp::ProcessContextReplacing<float> (b)); }
                { auto b = blk (low); ap2.process (juce::dsp::ProcessContextReplacing<float> (b)); }  // low gets x2's all-pass so all three line up
                const float w[3] = { p.wLow, p.wMid, p.wHigh };
                juce::AudioBuffer<float>* bands[3] = { &low, &mid, &high };
                for (int i = 0; i < n; ++i)
                {
                    float sl = 0, sr = 0;
                    for (int b = 0; b < 3; ++b)
                    {
                        const float bl = bands[b]->getSample (0, i), br = bands[b]->getSample (1, i);
                        const float m = 0.5f * (bl + br), s = 0.5f * (bl - br) * w[b];
                        sl += m + s; sr += m - s;
                    }
                    l[i] = sl; r[i] = sr;
                }
            }
        }
        float getShelfDb() const { return (float) shelfDb; }

    private:
        double sampleRate = 48000.0;
        DBiquad subBand[2], subLp[2], shelf[2];
        DBiquad detLo[2], detHi[2];            // lows below 120 Hz vs everything above 300 Hz
        double env = 0, lastBand = 0, lowEnv = 1e-6, highEnv = 1e-6, shelfDb = 0, lastShelf = 1000;
        float sign = 1.0f, subTrackSet = -1, subOut = 0;
        juce::dsp::LinkwitzRileyFilter<float> lp1, hp1, lp2, hp2, ap2;
        juce::AudioBuffer<float> low, mid, high;
    };
}
