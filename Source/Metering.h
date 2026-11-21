#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

// Real-time-safe metering and peak control. Everything allocates in prepare() only.
namespace SolutionDSP
{
    //==========================================================================
    // Fixed integer delay (used to keep the dry signal aligned with the wet path).
    class IntegerDelay
    {
    public:
        void prepare (int channels, int delaySamples)
        {
            delay = std::max (0, delaySamples);
            lines.assign ((size_t) channels, std::vector<float> ((size_t) delay + 1, 0.0f));
            pos = 0;
        }
        void reset() { for (auto& l : lines) std::fill (l.begin(), l.end(), 0.0f); pos = 0; }
        int getDelay() const { return delay; }

        void process (juce::AudioBuffer<float>& buffer, int numSamples)
        {
            if (delay == 0) return;
            const int chans = std::min ((int) lines.size(), buffer.getNumChannels());
            const int len = delay + 1;
            int p = pos;
            for (int i = 0; i < numSamples; ++i)
            {
                const int readPos = (p + 1) % len;
                for (int ch = 0; ch < chans; ++ch)
                {
                    auto* d = buffer.getWritePointer (ch);
                    lines[(size_t) ch][(size_t) p] = d[i];
                    d[i] = lines[(size_t) ch][(size_t) readPos];
                }
                p = readPos;
            }
            pos = p;
        }

    private:
        std::vector<std::vector<float>> lines;
        int delay = 0, pos = 0;
    };

    //==========================================================================
    // 4x inter-sample ("true") peak estimator using windowed-sinc interpolation.
    // push() returns the peak for the sample `latency` samples ago.
    class TruePeakDetector
    {
    public:
        static constexpr int taps = 12, latency = taps / 2, phases = 4;

        TruePeakDetector()
        {
            for (int ph = 1; ph < phases; ++ph)
            {
                const double t = (double) ph / phases;
                double sum = 0.0;
                for (int k = 0; k < taps; ++k)
                {
                    const double x = t + (latency - 1) - k;                       // distance from tap k
                    const double sinc = std::abs (x) < 1e-9 ? 1.0 : std::sin (juce::MathConstants<double>::pi * x) / (juce::MathConstants<double>::pi * x);
                    const double w = 0.5 * (1.0 + std::cos (juce::MathConstants<double>::pi * x / (latency + 0.5)));   // Hann
                    coeffs[(size_t) ph][(size_t) k] = (float) (sinc * w);
                    sum += sinc * w;
                }
                for (auto& c : coeffs[(size_t) ph]) c = (float) (c / sum);
            }
        }

        void reset() { for (auto& h : history) h.fill (0.0f); }

        // Linked stereo: returns max true peak across both channels.
        float push (float l, float r)
        {
            return std::max (pushChannel (0, l), pushChannel (1, r));
        }

    private:
        float pushChannel (int ch, float x)
        {
            auto& h = history[(size_t) ch];
            std::copy (h.begin() + 1, h.end(), h.begin());
            h[taps - 1] = x;
            float peak = std::abs (h[(size_t) latency - 1]);
            for (int ph = 1; ph < phases; ++ph)
            {
                float v = 0.0f;
                for (int k = 0; k < taps; ++k) v += h[(size_t) k] * coeffs[(size_t) ph][(size_t) k];
                peak = std::max (peak, std::abs (v));
            }
            return peak;
        }

        std::array<std::array<float, taps>, 2> history {};
        std::array<std::array<float, taps>, phases> coeffs {};
    };

    //==========================================================================
    // Look-ahead true-peak limiter: guarantees the output never exceeds the ceiling,
    // including inter-sample peaks, with smooth gain changes (no clicks).
    // Fixed latency whether or not it is engaged, so the DAW's delay compensation stays valid.
    class TruePeakLimiter
    {
    public:
        void prepare (double sampleRate, double lookaheadMs = 1.5)
        {
            lookahead = std::max (1, (int) std::round (lookaheadMs * 0.001 * sampleRate));   // 1.5 ms, or 5 ms with Lookahead on
            releaseCoeff = 1.0f - std::exp (-1.0f / (float) (0.060 * sampleRate));   // 60 ms release
            minHistory.assign ((size_t) lookahead, 1.0f);
            dequeValue.assign ((size_t) lookahead + 2, 1.0f);
            dequeIndex.assign ((size_t) lookahead + 2, 0);
            audioDelay.prepare (2, getLatency());
            reset();
        }

        void reset()
        {
            detector.reset(); audioDelay.reset();
            std::fill (minHistory.begin(), minHistory.end(), 1.0f);
            runningSum = (double) lookahead; histPos = 0; gain = 1.0f;
            dqHead = dqTail = 0; sampleIndex = 0;
        }

        int getLatency() const { return TruePeakDetector::latency + lookahead - 1; }
        float getGainReductionDb() const { return juce::Decibels::gainToDecibels (gain); }

        void process (juce::AudioBuffer<float>& buffer, int numSamples, float ceilingLin, bool enabled)
        {
            for (int start = 0; start < numSamples; start += chunk)
            {
                const int count = std::min (chunk, numSamples - start);
                juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), 2, start, count);
                auto* l = view.getWritePointer (0);
                auto* r = view.getWritePointer (1);

                // 1. Gain each upcoming sample needs (detected `latency` samples ahead of the audio).
                for (int i = 0; i < count; ++i)
                {
                    const float peak = detector.push (l[i], r[i]);
                    const float target = (enabled && peak > ceilingLin) ? ceilingLin / peak : 1.0f;
                    const float windowMin = pushSlidingMin (target);           // min over look-ahead window
                    runningSum += (double) windowMin - (double) minHistory[(size_t) histPos];
                    minHistory[(size_t) histPos] = windowMin;                  // ...then smoothed (boxcar)
                    histPos = (histPos + 1) % lookahead;
                    const float smooth = (float) std::min (1.0, runningSum / lookahead);
                    gain = smooth < gain ? smooth : gain + (smooth - gain) * releaseCoeff;
                    gains[(size_t) i] = gain;
                }

                // 2. Delay the audio by the look-ahead and apply the gain computed for it.
                audioDelay.process (view, count);
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto* d = view.getWritePointer (ch);
                    for (int i = 0; i < count; ++i)
                    {
                        d[i] *= gains[(size_t) i];
                        if (enabled) d[i] = juce::jlimit (-ceilingLin, ceilingLin, d[i]);   // final safety net
                    }
                }
            }
        }

    private:
        // Monotonic deque: O(1) sliding-window minimum.
        float pushSlidingMin (float v)
        {
            const int cap = lookahead + 2;   // room for a full window + the new value
            while (dqHead != dqTail && dequeValue[(size_t) ((dqTail - 1 + cap) % cap)] >= v)
                dqTail = (dqTail - 1 + cap) % cap;
            dequeValue[(size_t) dqTail] = v;
            dequeIndex[(size_t) dqTail] = sampleIndex;
            dqTail = (dqTail + 1) % cap;
            while (dequeIndex[(size_t) dqHead] <= sampleIndex - lookahead)
                dqHead = (dqHead + 1) % cap;
            ++sampleIndex;
            return dequeValue[(size_t) dqHead];
        }

        static constexpr int chunk = 256;
        TruePeakDetector detector;
        IntegerDelay audioDelay;
        std::vector<float> minHistory, dequeValue;
        std::vector<long long> dequeIndex;
        std::array<float, chunk> gains {};
        double runningSum = 0.0;
        long long sampleIndex = 0;
        int lookahead = 1, histPos = 0, dqHead = 0, dqTail = 0;
        float gain = 1.0f, releaseCoeff = 0.001f;
    };

    //==========================================================================
    // ITU-R BS.1770-4 / EBU R128 loudness: momentary (400 ms), short-term (3 s),
    // gated integrated loudness, plus true peak. Results are published as atomics.
    class LoudnessMeter
    {
    public:
        void prepare (double sampleRate)
        {
            fs = sampleRate;
            setupKWeighting();
            binLength = std::max (1, (int) std::round (fs * 0.1));    // 100 ms bins
            reset();
        }

        void reset()
        {
            for (auto& s : shelfState) s = {};
            for (auto& s : hpState) s = {};
            bins.fill (0.0); binPos = 0; binsFilled = 0; binAccum = 0.0; binCount = 0;
            histCount.fill (0); histEnergy.fill (0.0);
            peakDetector.reset(); truePeakMax = 0.0f;
            momentary.store (-70.0f); shortTerm.store (-70.0f); integrated.store (-70.0f); truePeakDb.store (-70.0f);
        }

        void requestReset() { resetRequested.store (true); }

        void process (const float* l, const float* r, int numSamples)
        {
            if (resetRequested.exchange (false)) reset();
            for (int i = 0; i < numSamples; ++i)
            {
                truePeakMax = std::max (truePeakMax, peakDetector.push (l[i], r[i]));
                const double kl = kWeight (0, l[i]), kr = kWeight (1, r[i]);
                binAccum += kl * kl + kr * kr;
                if (++binCount == binLength) completeBin();
            }
        }

        std::atomic<float> momentary { -70.0f }, shortTerm { -70.0f }, integrated { -70.0f }, truePeakDb { -70.0f };

    private:
        static double toLufs (double meanSquare) { return meanSquare > 1e-12 ? -0.691 + 10.0 * std::log10 (meanSquare) : -70.0; }

        double kWeight (int ch, float x)
        {
            auto biquad = [] (const std::array<double, 5>& c, std::array<double, 2>& z, double in)
            {
                const double out = c[0] * in + z[0];
                z[0] = c[1] * in - c[3] * out + z[1];
                z[1] = c[2] * in - c[4] * out;
                return out;
            };
            return biquad (hp, hpState[(size_t) ch], biquad (shelf, shelfState[(size_t) ch], (double) x));
        }

        void setupKWeighting()
        {
            // Coefficients for any sample rate (same derivation as libebur128).
            double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            double K = std::tan (juce::MathConstants<double>::pi * f0 / fs);
            const double Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
            double a0 = 1.0 + K / Q + K * K;
            shelf = { (Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                      2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
            f0 = 38.13547087602444; Q = 0.5003270373238773;
            K = std::tan (juce::MathConstants<double>::pi * f0 / fs);
            a0 = 1.0 + K / Q + K * K;
            hp = { 1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
        }

        double meanOfLastBins (int n) const
        {
            double sum = 0.0;
            for (int k = 0; k < n; ++k)
                sum += bins[(size_t) ((binPos - 1 - k + numBins) % numBins)];
            return sum / ((double) n * binLength);
        }

        void completeBin()
        {
            bins[(size_t) binPos] = binAccum;
            binPos = (binPos + 1) % numBins;
            binsFilled = std::min (binsFilled + 1, numBins);
            binAccum = 0.0; binCount = 0;

            if (binsFilled >= 4)
            {
                const double ms = meanOfLastBins (4);           // 400 ms block, 75% overlap
                const double lufs = toLufs (ms);
                momentary.store ((float) lufs);
                if (lufs > -70.0)                               // absolute gate
                {
                    const int h = juce::jlimit (0, histSize - 1, (int) ((lufs + 70.0) * 10.0));
                    histCount[(size_t) h]++; histEnergy[(size_t) h] += ms;
                }
                updateIntegrated();
            }
            if (binsFilled >= 30)
                shortTerm.store ((float) toLufs (meanOfLastBins (30)));
            truePeakDb.store (juce::Decibels::gainToDecibels (truePeakMax, -70.0f));
        }

        void updateIntegrated()
        {
            double e = 0.0; long n = 0;
            for (int h = 0; h < histSize; ++h) { e += histEnergy[(size_t) h]; n += histCount[(size_t) h]; }
            if (n == 0) return;
            const double relGate = toLufs (e / (double) n) - 10.0;
            const int firstBin = juce::jlimit (0, histSize, (int) std::ceil ((relGate + 70.0) * 10.0));
            e = 0.0; n = 0;
            for (int h = firstBin; h < histSize; ++h) { e += histEnergy[(size_t) h]; n += histCount[(size_t) h]; }
            if (n > 0) integrated.store ((float) toLufs (e / (double) n));
        }

        static constexpr int numBins = 30, histSize = 800;    // 3 s of bins; -70..+10 LUFS in 0.1 LU steps
        double fs = 48000.0;
        std::array<double, 5> shelf {}, hp {};
        std::array<std::array<double, 2>, 2> shelfState {}, hpState {};
        std::array<double, numBins> bins {};
        std::array<long, histSize> histCount {};
        std::array<double, histSize> histEnergy {};
        int binLength = 4800, binPos = 0, binsFilled = 0, binCount = 0;
        double binAccum = 0.0;
        TruePeakDetector peakDetector;
        float truePeakMax = 0.0f;
        std::atomic<bool> resetRequested { false };
    };
}
