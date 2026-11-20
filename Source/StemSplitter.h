#pragma once

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

// Stem Focus v1 (DSP, real time, no AI model): splits the input into a "focus" stem and the rest.
//   Drums  - percussive part  (harmonic/percussive median filtering, Fitzgerald 2010)
//   Tonal  - harmonic part
//   Vocal  - phase-coherent centre content, 120 Hz - 10 kHz (Avendano-style similarity mask)
// Soft masks + a time-domain residual guarantee focus + rest == the input (delayed), so nothing is
// lost. Latency: fftSize + lookahead frames * hop samples (2048 at any sample rate).
namespace SolutionDSP
{
    class StemSplitter
    {
    public:
        enum Mode { off = 0, drums = 1, tonal = 2, vocal = 3 };
        static constexpr int order = 10, N = 1 << order, hop = N / 4, bins = N / 2 + 1;
        static constexpr int lookahead = 4, history = 2 * lookahead + 1, latency = N + lookahead * hop;

        void prepare (double sampleRate, int maxBlock)
        {
            fs = sampleRate;
            window.resize (N);
            for (int i = 0; i < N; ++i) window[(size_t) i] = std::sqrt (0.5f * (1.0f - std::cos (juce::MathConstants<float>::twoPi * i / N)));
            for (auto& b : inRing) b.assign (N, 0.0f);
            for (auto& r : outRing) r.assign (ringSize, 0.0f);
            for (auto& d : dryRing) d.assign (ringSize, 0.0f);
            spectra.assign ((size_t) history * 2 * bins, {});
            mags.assign ((size_t) history * bins, 0.0f);
            fftWork.assign (2 * N, 0.0f);
            column.resize (history); row.resize (2 * percHalf + 1);
            harm.assign (bins, 0.0f); perc.assign (bins, 0.0f); mask.assign (bins, 0.0f);
            juce::ignoreUnused (maxBlock);
            reset();
        }

        void reset()
        {
            for (auto& b : inRing) std::fill (b.begin(), b.end(), 0.0f);
            for (auto& r : outRing) std::fill (r.begin(), r.end(), 0.0f);
            for (auto& d : dryRing) std::fill (d.begin(), d.end(), 0.0f);
            std::fill (spectra.begin(), spectra.end(), std::complex<float>());
            std::fill (mags.begin(), mags.end(), 0.0f);
            inPos = 0; hopCount = 0; framesSeen = 0; sampleIndex = 0; newest = 0;
        }

        // In:  l/r = input. Out: l/r = focus stem, restL/restR = everything else (both delayed by `latency`).
        void process (float* l, float* r, float* restL, float* restR, int numSamples, Mode mode)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float in[2] = { l[i], r[i] };
                for (int ch = 0; ch < 2; ++ch)
                {
                    inRing[(size_t) ch][(size_t) inPos] = in[ch];
                    dryRing[(size_t) ch][(size_t) (sampleIndex % ringSize)] = in[ch];
                }
                inPos = (inPos + 1) % N;
                ++sampleIndex;
                if (++hopCount == hop) { hopCount = 0; analyseFrame (mode); }

                const long long outIndex = sampleIndex - 1 - latency;
                float focus[2] = { 0.0f, 0.0f }, dry[2] = { 0.0f, 0.0f };
                if (outIndex >= 0)
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        auto& o = outRing[(size_t) ch][(size_t) (outIndex % ringSize)];
                        focus[ch] = o; o = 0.0f;                                  // consume
                        dry[ch] = dryRing[(size_t) ch][(size_t) (outIndex % ringSize)];
                    }
                l[i] = focus[0]; r[i] = focus[1];
                restL[i] = dry[0] - focus[0]; restR[i] = dry[1] - focus[1];
            }
        }

    private:
        std::complex<float>& spec (int frame, int ch, int k) { return spectra[(size_t) ((frame * 2 + ch) * bins + k)]; }

        void analyseFrame (Mode mode)
        {
            // 1. FFT the newest frame (both channels) into the history ring.
            newest = (newest + 1) % history;
            for (int ch = 0; ch < 2; ++ch)
            {
                for (int i = 0; i < N; ++i) fftWork[(size_t) i] = inRing[(size_t) ch][(size_t) ((inPos + i) % N)] * window[(size_t) i];
                std::fill (fftWork.begin() + N, fftWork.end(), 0.0f);
                fft.performRealOnlyForwardTransform (fftWork.data(), true);
                for (int k = 0; k < bins; ++k) spec (newest, ch, k) = { fftWork[(size_t) (2 * k)], fftWork[(size_t) (2 * k + 1)] };
            }
            for (int k = 0; k < bins; ++k)
                mags[(size_t) (newest * bins + k)] = 0.5f * (std::abs (spec (newest, 0, k)) + std::abs (spec (newest, 1, k)));
            if (++framesSeen < history) return;

            // 2. Mask for the centre frame (lookahead frames behind the newest).
            const int c = (newest - lookahead + history) % history;
            computeMask (c, mode);

            // 3. Masked inverse FFT, overlap-added into the output ring at the frame's position.
            const long long frameStart = sampleIndex - N - (long long) lookahead * hop;   // first sample of frame c
            for (int ch = 0; ch < 2; ++ch)
            {
                for (int k = 0; k < bins; ++k)
                {
                    const auto v = spec (c, ch, k) * mask[(size_t) k];
                    fftWork[(size_t) (2 * k)] = v.real(); fftWork[(size_t) (2 * k + 1)] = v.imag();
                }
                fft.performRealOnlyInverseTransform (fftWork.data());
                for (int i = 0; i < N; ++i)
                {
                    const long long idx = frameStart + i;
                    if (idx >= 0)
                        outRing[(size_t) ch][(size_t) (idx % ringSize)] += fftWork[(size_t) i] * window[(size_t) i] * 0.5f;   // sqrt-Hann^2 at 75% overlap sums to 2
                }
            }
        }

        void computeMask (int c, Mode mode)
        {
            if (mode == vocal)
            {
                for (int k = 0; k < bins; ++k)
                {
                    const float f = (float) (k * fs / N);
                    const auto L = spec (c, 0, k), R = spec (c, 1, k);
                    const float denom = std::norm (L) + std::norm (R) + 1e-12f;
                    const float similarity = 2.0f * std::abs (L * std::conj (R)) / denom;        // 1 = identical in both channels
                    const float phaseAlign = std::max (0.0f, (L * std::conj (R)).real() / (std::abs (L) * std::abs (R) + 1e-12f));
                    float m = juce::jlimit (0.0f, 1.0f, (similarity * phaseAlign - 0.80f) / 0.18f);
                    const float band = juce::jlimit (0.0f, 1.0f, std::min ((f - 80.0f) / 80.0f, (12000.0f - f) / 4000.0f));
                    mask[(size_t) k] = m * band;
                }
                return;
            }

            // Harmonic = median across time; percussive = median across frequency.
            for (int k = 0; k < bins; ++k)
            {
                for (int t = 0; t < history; ++t) column[(size_t) t] = mags[(size_t) (t * bins + k)];
                std::nth_element (column.begin(), column.begin() + lookahead, column.end());
                harm[(size_t) k] = column[(size_t) lookahead];
            }
            const float* frameMags = &mags[(size_t) (c * bins)];
            for (int k = 0; k < bins; ++k)
            {
                int n = 0;
                for (int j = k - percHalf; j <= k + percHalf; ++j) row[(size_t) n++] = frameMags[std::clamp (j, 0, bins - 1)];
                std::nth_element (row.begin(), row.begin() + percHalf, row.end());
                perc[(size_t) k] = row[(size_t) percHalf];
            }
            for (int k = 0; k < bins; ++k)
            {
                const float h2 = harm[(size_t) k] * harm[(size_t) k], p2 = perc[(size_t) k] * perc[(size_t) k];
                const float pm = p2 / (h2 + p2 + 1e-20f);
                mask[(size_t) k] = mode == drums ? pm : 1.0f - pm;
            }
        }

        static constexpr int ringSize = 8192, percHalf = 8;
        double fs = 48000.0;
        juce::dsp::FFT fft { order };
        std::vector<float> window, fftWork, column, row, harm, perc, mask, mags;
        std::array<std::vector<float>, 2> inRing, outRing, dryRing;
        std::vector<std::complex<float>> spectra;
        int inPos = 0, hopCount = 0, framesSeen = 0, newest = 0;
        long long sampleIndex = 0;
    };
}
