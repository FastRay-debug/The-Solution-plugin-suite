#pragma once
// The Anchor's clipper suite.
//  - Single or 4-band (120 Hz / 1 kHz / 6 kHz Linkwitz-Riley) clipping, optional dynamic multiband drive
//  - Curves: Soft, Gold (smooth knee), Tube, Hard, Arctan, Sine Fold; knee; drive up to +24 dB (1585 %)
//  - Clip, or a transparent look-ahead limiter at the ceiling
//  - 1x / 2x / 4x / 8x / 16x oversampling
//  - Blend (parallel) with PAM - Phase Alignment Matrix: the dry path gets the same crossover phase and delay,
//    so blending never comb-filters
//  - Squash / crest detector: how much crest factor was lost and how much distortion was added (wanted vs not)
// Latency is fixed (the 16x path + 1.5 ms look-ahead) whatever you choose, so switching never upsets the DAW.
#include <juce_dsp/juce_dsp.h>
#include <array>

namespace SolutionDSP
{
    class AnchorCore
    {
    public:
        enum Curve { Soft = 0, Gold, Tube, Hard, Arctan, SineFold };
        struct Params
        {
            bool on = false;
            float driveDb = 0.0f, knee = 0.5f, ceilingDb = -0.3f, outDb = 0.0f, blend = 1.0f;
            int curve = Gold, osIndex = 2;                 // 0 = 1x ... 4 = 16x
            bool multiband = false, dynamic = false, limitMode = false, pam = true;
        };

        static float shape (float u, int curve, float knee)
        {
            const float hard = juce::jlimit (-1.0f, 1.0f, u);
            float c;
            switch (curve)
            {
                case Gold:
                {
                    const float h = 0.02f + 0.48f * knee, a = std::abs (u), sg = u < 0 ? -1.0f : 1.0f;
                    if (a <= 1.0f - h) return u;
                    if (a >= 1.0f + h) return sg;
                    const float d = a - (1.0f - h); return sg * (a - d * d / (4.0f * h));
                }
                case Tube:     c = u >= 0 ? std::tanh (u) : std::tanh (1.25f * u) / 1.25f; c += 0.04f * (1.0f - c * c) * (u >= 0 ? 1.0f : 0.5f); c = juce::jlimit (-1.0f, 1.0f, c); break;
                case Hard:     return hard;
                case Arctan:   c = 0.63662f * std::atan (u * 1.5708f); break;
                case SineFold: c = std::sin (u * 1.5708f); break;              // folds back past the ceiling: deliberate grit
                default:       c = std::tanh (u); break;
            }
            return hard + (c - hard) * knee;
        }

        void prepare (double fs, int maxBlock)
        {
            sampleRate = fs; maxN = std::max (16, maxBlock);
            for (int b = 0; b < 4; ++b)
                for (int k = 0; k < 4; ++k)
                {
                    os[(size_t) b][(size_t) k] = std::make_unique<juce::dsp::Oversampling<float>> (2, k + 1, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
                    os[(size_t) b][(size_t) k]->initProcessing ((size_t) maxN);
                }
            osLatency[0] = 0;
            for (int k = 0; k < 4; ++k) osLatency[(size_t) k + 1] = (int) std::lround (os[0][(size_t) k]->getLatencyInSamples());
            lookahead = (int) std::lround (0.0015 * fs);
            latency = osLatency[4] + lookahead;
            juce::dsp::ProcessSpec spec { fs, (juce::uint32) maxN, 2 };
            const float xo[3] = { 120.0f, 1000.0f, 6000.0f };
            for (int i = 0; i < 3; ++i)
            {
                for (auto* f : { &lp[i], &hp[i], &apA[i], &apB[i], &apDry[i] }) { f->prepare (spec); f->setCutoffFrequency (xo[i]); }
                lp[i].setType (juce::dsp::LinkwitzRileyFilterType::lowpass); hp[i].setType (juce::dsp::LinkwitzRileyFilterType::highpass);
                apA[i].setType (juce::dsp::LinkwitzRileyFilterType::allpass); apB[i].setType (juce::dsp::LinkwitzRileyFilterType::allpass); apDry[i].setType (juce::dsp::LinkwitzRileyFilterType::allpass);
            }
            for (auto& b : bands) b.setSize (2, maxN);
            dry.setSize (2, maxN); wet.setSize (2, maxN);
            for (auto& d : bandPad) d.assign (2, std::vector<float> ((size_t) latency + 1, 0.0f));
            padLine.assign (2, std::vector<float> ((size_t) latency + 1, 0.0f));
            dryLine.assign (2, std::vector<float> ((size_t) latency + 1, 0.0f));
            laLine.assign (2, std::vector<float> ((size_t) lookahead + 1, 0.0f));
            reset();
        }
        void reset()
        {
            for (auto& row : os) for (auto& o : row) if (o) o->reset();
            for (int i = 0; i < 3; ++i) for (auto* f : { &lp[i], &hp[i], &apA[i], &apB[i], &apDry[i] }) f->reset();
            for (auto& d : bandPad) for (auto& v : d) std::fill (v.begin(), v.end(), 0.0f);
            for (auto* L : { &padLine, &dryLine, &laLine }) for (auto& v : *L) std::fill (v.begin(), v.end(), 0.0f);
            padPos = dryPos = laPos = 0; bandPadPos.fill (0);
            env.fill (1e-4f); limGain = 1.0f; pkIn = pkOut = msIn = msOut = 0; holdMin.clear(); crestIn = crestOut = 0; distPct = 0; peakRed = 0;
        }
        int getLatency() const { return latency; }

        void process (float* l, float* r, int n, const Params& p)
        {
            if (n > maxN) { for (int off = 0; off < n; off += maxN) process (l + off, r + off, std::min (maxN, n - off), p); return; }
            float* io[2] = { l, r };
            if (! p.on) { delayLine (dryLine, dryPos, io, n, latency); distPct = 0; crestIn = crestOut = 0; peakRed = 0; return; }   // original sound, time-aligned

            const float drive = juce::Decibels::decibelsToGain (p.driveDb), ceil = juce::Decibels::decibelsToGain (p.ceilingDb);
            // dry copy for blend / detector
            for (int c = 0; c < 2; ++c) dry.copyFrom (c, 0, io[c], n);
            // ---- split ----
            const int nb = p.multiband ? 4 : 1;
            for (int c = 0; c < 2; ++c) for (int b = 0; b < nb; ++b) bands[(size_t) b].copyFrom (c, 0, io[c], n);
            if (p.multiband)
            {
                auto blk = [n] (juce::AudioBuffer<float>& b) { return juce::dsp::AudioBlock<float> (b.getArrayOfWritePointers(), 2, (size_t) n); };
                auto run = [&] (juce::dsp::LinkwitzRileyFilter<float>& f, juce::AudioBuffer<float>& b) { auto bl = blk (b); f.process (juce::dsp::ProcessContextReplacing<float> (bl)); };
                // tree: b0 = LP120 ; rest = HP120 -> b1 = LP1k ; rest2 = HP1k -> b2 = LP6k, b3 = HP6k
                run (lp[0], bands[0]); run (hp[0], bands[1]);
                for (int c = 0; c < 2; ++c) { bands[2].copyFrom (c, 0, bands[1], c, 0, n); }
                run (lp[1], bands[1]); run (hp[1], bands[2]);
                for (int c = 0; c < 2; ++c) { bands[3].copyFrom (c, 0, bands[2], c, 0, n); }
                run (lp[2], bands[2]); run (hp[2], bands[3]);
                run (apA[1], bands[0]); run (apA[2], bands[0]); run (apB[2], bands[1]);   // phase-match the lower bands
            }
            // ---- dynamic multiband drive ----
            std::array<float, 4> bandDrive { drive, drive, drive, drive };
            if (p.multiband && p.dynamic)
            {
                const double a = 1.0 - std::exp (-(double) n / (0.12 * sampleRate));
                float mean = 0;
                for (int b = 0; b < 4; ++b) { const float e = std::max (bands[(size_t) b].getRMSLevel (0, 0, n), bands[(size_t) b].getRMSLevel (1, 0, n)); env[(size_t) b] += (float) ((e - env[(size_t) b]) * a); mean += env[(size_t) b] * 0.25f; }
                for (int b = 0; b < 4; ++b) bandDrive[(size_t) b] = drive * juce::jlimit (0.25f, 1.0f, (mean + 1e-6f) / (env[(size_t) b] + 1e-6f));   // only the hot bands are eased (up to -12 dB)
                lastBandDrive = bandDrive;
            }
            // ---- shape, oversampled ----
            for (int c = 0; c < 2; ++c) juce::FloatVectorOperations::clear (wet.getWritePointer (c), n);
            for (int b = 0; b < nb; ++b)
            {
                auto& B = bands[(size_t) b];
                const float g = bandDrive[(size_t) b] / ceil;
                const bool clip = ! p.limitMode;
                auto shapeBlock = [&] (float* const* ch, size_t len) {
                    for (int c = 0; c < 2; ++c) for (size_t i = 0; i < len; ++i) { const float u = ch[c][i] * g; ch[c][i] = (clip ? shape (u, p.curve, p.knee) : u) * ceil; } };
                if (p.osIndex <= 0) { float* ch[2] = { B.getWritePointer (0), B.getWritePointer (1) }; shapeBlock (ch, (size_t) n); }
                else
                {
                    auto& o = *os[(size_t) b][(size_t) juce::jlimit (1, 4, p.osIndex) - 1];
                    juce::dsp::AudioBlock<float> blk (B.getArrayOfWritePointers(), 2, (size_t) n);
                    auto up = o.processSamplesUp (blk);
                    float* ch[2] = { up.getChannelPointer (0), up.getChannelPointer (1) }; shapeBlock (ch, up.getNumSamples());
                    o.processSamplesDown (blk);
                }
                // pad every path to the 16x latency so the switch is seamless and the bands stay aligned
                float* bp[2] = { B.getWritePointer (0), B.getWritePointer (1) };
                delayLine (bandPad[(size_t) b], bandPadPos[(size_t) b], bp, n, osLatency[4] - osLatency[(size_t) juce::jlimit (0, 4, p.osIndex)]);
                const float makeup = p.dynamic && p.multiband ? drive / bandDrive[(size_t) b] : 1.0f;   // same level, less saturation
                for (int c = 0; c < 2; ++c) wet.addFrom (c, 0, B, c, 0, n, makeup);
            }
            // ---- transparent limiter (look-ahead) or plain delay so latency never changes ----
            float* wp[2] = { wet.getWritePointer (0), wet.getWritePointer (1) };
            if (p.limitMode) limit (wp, n, ceil); else delayLine (laLine, laPos, wp, n, lookahead);
            // ---- dry path: same delay, and with PAM the same crossover phase ----
            float* dp[2] = { dry.getWritePointer (0), dry.getWritePointer (1) };
            if (p.multiband && p.pam)
            {
                juce::dsp::AudioBlock<float> bl (dry.getArrayOfWritePointers(), 2, (size_t) n);
                for (auto& f : apDry) f.process (juce::dsp::ProcessContextReplacing<float> (bl));
            }
            delayLine (padLine, padPos, dp, n, latency);
            // ---- detector: crest in/out, added distortion ----
            measure (dp, wp, n, ceil);
            // ---- blend + output ----
            const float og = juce::Decibels::decibelsToGain (p.outDb), w = juce::jlimit (0.0f, 1.0f, p.blend);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i) io[c][i] = og * (w * wp[c][i] + (1.0f - w) * dp[c][i]);
        }

        // Detector results (audio thread writes, any thread reads)
        float getCrestLostDb() const { return crestIn - crestOut; }
        float getDistortionPct() const { return distPct; }
        float getPeakReductionDb() const { return peakRed; }
        std::array<float, 4> lastBandDrive {};
        // 0 clean, 1 glue (wanted), 2 crunch, 3 squashed (probably unwanted)
        int verdict() const { const float d = distPct, c = crestIn - crestOut; return d < 1.0f && c < 1.0f ? 0 : (d < 4.0f && c < 4.0f ? 1 : (d < 12.0f && c < 8.0f ? 2 : 3)); }

    private:
        double sampleRate = 48000.0;
        int maxN = 512, latency = 0, lookahead = 0;
        std::array<int, 5> osLatency {};
        std::array<std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, 4>, 4> os;
        juce::dsp::LinkwitzRileyFilter<float> lp[3], hp[3], apA[3], apB[3], apDry[3];
        std::array<juce::AudioBuffer<float>, 4> bands;
        juce::AudioBuffer<float> dry, wet;
        std::array<std::vector<std::vector<float>>, 4> bandPad;
        std::array<int, 4> bandPadPos {};
        std::vector<std::vector<float>> padLine, dryLine, laLine;
        int padPos = 0, dryPos = 0, laPos = 0;
        std::array<float, 4> env {};
        float limGain = 1.0f;
        std::vector<float> holdMin;
        float crestIn = 0, crestOut = 0, distPct = 0, peakRed = 0;

        static void delayLine (std::vector<std::vector<float>>& line, int& pos, float* const* io, int n, int d)
        {
            if (d <= 0 || line.empty()) return;
            const int size = (int) line[0].size();
            int p = pos;
            for (int i = 0; i < n; ++i)
            {
                const int rd = (p - d + size) % size;
                for (int c = 0; c < 2; ++c) { const float x = io[c][i]; io[c][i] = line[(size_t) c][(size_t) rd]; line[(size_t) c][(size_t) p] = x; }
                p = (p + 1) % size;
            }
            pos = p;
        }
        // Look-ahead peak limiter: the gain reaches its target before the peak arrives (no clipping, no overs).
        void limit (float* const* io, int n, float ceil)
        {
            const int size = (int) laLine[0].size();
            const double rel = std::exp (-1.0 / (0.060 * sampleRate)), att = std::exp (-1.0 / (0.25 * lookahead + 1.0));
            float red = 1.0f;
            for (int i = 0; i < n; ++i)
            {
                const float pk = std::max (std::abs (io[0][i]), std::abs (io[1][i]));
                const float need = pk > ceil ? ceil / pk : 1.0f;
                // target = smallest gain needed within the look-ahead window (simple sliding minimum via decay)
                holdTarget = std::min (need, holdTarget + (1.0f - holdTarget) * (float) (1.0 - rel));
                limGain = holdTarget < limGain ? (float) (holdTarget + (limGain - holdTarget) * att) : holdTarget;
                const int rd = (laPos - lookahead + size) % size;
                for (int c = 0; c < 2; ++c) { const float x = io[c][i]; io[c][i] = juce::jlimit (-ceil, ceil, laLine[(size_t) c][(size_t) rd] * limGain); laLine[(size_t) c][(size_t) laPos] = x; }
                laPos = (laPos + 1) % size;
                red = std::min (red, limGain);
            }
            peakRedLim = juce::Decibels::gainToDecibels (red);
        }
        float holdTarget = 1.0f, peakRedLim = 0.0f;
        double pkIn = 0, pkOut = 0, msIn = 0, msOut = 0;
        void measure (float* const* d, float* const* w, int n, float)
        {
            double pi = 0, ri = 0, po = 0, ro = 0, dd = 0, dw = 0, ww = 0;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i)
                {
                    const double x = d[c][i], y = w[c][i];
                    pi = std::max (pi, std::abs (x)); po = std::max (po, std::abs (y)); ri += x * x; ro += y * y; dd += x * x; dw += x * y; ww += y * y;
                }
            const double a = 1.0 - std::exp (-(double) n / (0.3 * sampleRate)), pkDecay = std::exp (-(double) n / (0.5 * sampleRate));
            // crest over ~0.5 s: held peak vs 300 ms RMS (per-block crest would hide squashing)
            pkIn = std::max (pi, pkIn * pkDecay); pkOut = std::max (po, pkOut * pkDecay);
            msIn += (ri / (2.0 * n) - msIn) * a; msOut += (ro / (2.0 * n) - msOut) * a;
            if (ri > 1e-9 && ro > 1e-9)
            {
                crestIn = (float) (20 * std::log10 (pkIn / std::sqrt (msIn + 1e-12) + 1e-9)); crestOut = (float) (20 * std::log10 (pkOut / std::sqrt (msOut + 1e-12) + 1e-9));
                const double g = dw / dd, res = std::max (0.0, ww - 2 * g * dw + g * g * dd);     // what isn't a scaled copy of the input
                distPct += (float) ((100.0 * std::sqrt (res / ww) - distPct) * a);
                peakRed += (float) ((20 * std::log10 ((po + 1e-9) / (pi * std::abs (g) + 1e-9)) - peakRed) * a);
            }
        }
    };
}
