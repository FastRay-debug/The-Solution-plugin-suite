#pragma once
// The Mosaic: STFT spectral morphing.
//  Four nodes (corners of an XY pad) each hold a spectral fingerprint (31 third-octave bands, dB around 0).
//  The pad blends them; MORPH reshapes the input's spectrum toward that blend (its own envelope is measured
//  live and replaced, so the result follows the target whatever goes in). TRANSIENTS keeps hits untouched.
//  FFT 1024 / 2048 / 4096, 75 % overlap; latency is fixed at 4096 while on (smaller sizes are padded).
#include <juce_dsp/juce_dsp.h>
#include <array>

namespace SolutionDSP
{
    class MosaicCore
    {
    public:
        static constexpr int maxN = 4096, numBands = 31;
        using Print = std::array<float, numBands>;
        struct Params { float morph = 0.0f, x = 0.5f, y = 0.5f, preserve = 0.5f; int sizeIndex = 1; };

        static float bandHz (int b) { return 20.0f * std::pow (2.0f, b / 3.0f); }      // 20 Hz .. ~20 kHz
        static Print preset (int which)
        {
            Print p {};
            for (int b = 0; b < numBands; ++b)
            {
                const float oct = std::log2 (bandHz (b) / 1000.0f);
                switch (which)
                {
                    case 0: p[(size_t) b] = 3.0f * oct; break;                                             // Air: tilt up
                    case 1: p[(size_t) b] = -3.0f * oct; break;                                            // Warm: tilt down
                    case 2: p[(size_t) b] = -9.0f * std::exp (-oct * oct * 1.2f); break;                  // Hollow: scooped mids
                    default: { const float f = bandHz (b); p[(size_t) b] = f < 300 ? -18.0f * std::log2 (300.0f / f) / 4.0f : f > 3400 ? -18.0f * std::log2 (f / 3400.0f) / 2.0f : 3.0f; } // Radio
                }
            }
            return normalise (p);
        }
        static Print normalise (Print p) { float m = 0; for (auto v : p) m += v; m /= numBands; for (auto& v : p) v -= m; return p; }

        MosaicCore() { for (int i = 0; i < 4; ++i) nodes[(size_t) i] = preset (i); }
        void setNode (int i, const Print& p) { if (i >= 0 && i < 4) { const juce::SpinLock::ScopedLockType sl (nodeLock); nodes[(size_t) i] = normalise (p); } }
        Print getNode (int i) const { const juce::SpinLock::ScopedLockType sl (nodeLock); return nodes[(size_t) juce::jlimit (0, 3, i)]; }
        Print liveFingerprint() const { Print p; for (int b = 0; b < numBands; ++b) p[(size_t) b] = longEnv[(size_t) b].load(); return normalise (p); }

        void prepare (double fs, int)
        {
            sampleRate = fs;
            for (int k = 0; k < 3; ++k) { ffts[(size_t) k] = std::make_unique<juce::dsp::FFT> (10 + k); }
            for (auto& c : inFifo) c.assign (maxN * 2, 0.0f);
            for (auto& c : outAcc) c.assign (maxN * 2, 0.0f);
            for (auto& c : pad) c.assign (maxN + 1, 0.0f);
            work.assign (maxN * 2, 0.0f); for (auto& c : chSpec) c.assign (maxN * 2 + 2, 0.0f);
            prevMag.assign (maxN / 2 + 1, 0.0f); envDb.assign (numBands, -100.0f);
            reset();
        }
        void reset()
        {
            for (auto& c : inFifo) std::fill (c.begin(), c.end(), 0.0f);
            for (auto& c : outAcc) std::fill (c.begin(), c.end(), 0.0f);
            for (auto& c : pad) std::fill (c.begin(), c.end(), 0.0f);
            std::fill (prevMag.begin(), prevMag.end(), 0.0f); std::fill (envDb.begin(), envDb.end(), -100.0f);
            fill = 0; hopCount = 0; padPos = 0; fluxAvg = 0; curN = -1; longFrames = 0;
        }
        static int latency() { return maxN; }

        void process (float* l, float* r, int n, const Params& p)
        {
            const int N = 1024 << juce::jlimit (0, 2, p.sizeIndex), hop = N / 4;
            if (N != curN) { curN = N; for (auto& c : inFifo) std::fill (c.begin(), c.end(), 0.0f); for (auto& c : outAcc) std::fill (c.begin(), c.end(), 0.0f); fill = 0; }
            float* io[2] = { l, r };
            for (int i = 0; i < n; ++i)
            {
                // the output written now was finished N samples ago (frame latency N)
                for (int c = 0; c < 2; ++c) { inFifo[(size_t) c][(size_t) fill] = io[c][i]; io[c][i] = outAcc[(size_t) c][(size_t) fill]; outAcc[(size_t) c][(size_t) fill] = 0.0f; }
                if (++fill >= N) fill = 0;
                if (++hopCount >= hop) { hopCount = 0; frame (N, p); }
            }
            // pad smaller frames up to the fixed 4096 latency
            const int d = maxN - N;
            if (d > 0)
                for (int i = 0; i < n; ++i)
                {
                    const int rd = (padPos - d + (maxN + 1)) % (maxN + 1);
                    for (int c = 0; c < 2; ++c) { const float x = io[c][i]; io[c][i] = pad[(size_t) c][(size_t) rd]; pad[(size_t) c][(size_t) padPos] = x; }
                    padPos = (padPos + 1) % (maxN + 1);
                }
        }
        float lastTransient() const { return transient; }

    private:
        void frame (int N, const Params& p)
        {
            auto& fft = *ffts[(size_t) (N == 1024 ? 0 : N == 2048 ? 1 : 2)];
            const int bins = N / 2;
            // mid spectrum -> envelope + flux (gain is computed once and applied to both channels: stereo image kept)
            for (int c = 0; c < 2; ++c)
            {
                for (int i = 0; i < N; ++i) { const float w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * i / N); work[(size_t) i] = inFifo[(size_t) c][(size_t) ((fill + i) % N)] * w; }
                std::fill (work.begin() + N, work.begin() + 2 * N, 0.0f);
                fft.performRealOnlyForwardTransform (work.data(), true);
                std::copy (work.begin(), work.begin() + 2 * N, chSpec[(size_t) c].begin());
            }
            // band envelope of the mid, flux for transients
            std::array<double, numBands> bandPow {}; std::array<int, numBands> bandCnt {};
            double flux = 0;
            for (int k = 1; k < bins; ++k)
            {
                const float re = 0.5f * (chSpec[0][(size_t) (2 * k)] + chSpec[1][(size_t) (2 * k)]), im = 0.5f * (chSpec[0][(size_t) (2 * k + 1)] + chSpec[1][(size_t) (2 * k + 1)]);
                const float mag = std::sqrt (re * re + im * im);
                flux += std::max (0.0f, mag - prevMag[(size_t) k]); prevMag[(size_t) k] = mag;
                const float f = (float) (k * sampleRate / N);
                const int b = juce::jlimit (0, numBands - 1, (int) std::lround (3.0f * std::log2 (std::max (20.0f, f) / 20.0f)));
                bandPow[(size_t) b] += mag * mag; bandCnt[(size_t) b]++;
            }
            fluxAvg += (flux - fluxAvg) * 0.05; longFrames += 1.0;
            transient = (float) juce::jlimit (0.0, 1.0, (flux / (fluxAvg + 1e-9) - 1.3) / 1.5);
            // smoothed envelope (~150 ms) in dB; bands with no bins borrow their neighbour
            const double a = 1.0 - std::exp (-(N / 4.0) / (0.15 * sampleRate)), aLong = 1.0 - std::exp (-(N / 4.0) / (3.0 * sampleRate));
            for (int b = 0; b < numBands; ++b)
            {
                const double db = bandCnt[(size_t) b] ? 10.0 * std::log10 (bandPow[(size_t) b] / bandCnt[(size_t) b] + 1e-12) : (b ? envDb[(size_t) b - 1] : -100.0);
                envDb[(size_t) b] += (float) ((db - envDb[(size_t) b]) * a);
                const double aL = std::max (aLong, 1.0 / (longFrames + 1.0));                   // running average until 3 s have passed
                longEnv[(size_t) b].store (longEnv[(size_t) b].load() + (float) ((db - longEnv[(size_t) b].load()) * aL));
            }
            // target = bilinear blend of the 4 nodes; gains = target - (own envelope around its mean)
            Print target {};
            { const juce::SpinLock::ScopedLockType sl (nodeLock);
              const float wA = (1 - p.x) * p.y, wB = p.x * p.y, wC = (1 - p.x) * (1 - p.y), wD = p.x * (1 - p.y);
              for (int b = 0; b < numBands; ++b) target[(size_t) b] = wA * nodes[0][(size_t) b] + wB * nodes[1][(size_t) b] + wC * nodes[2][(size_t) b] + wD * nodes[3][(size_t) b]; }
            float mean = 0; int used = 0; for (int b = 0; b < numBands; ++b) if (bandHz (b) < sampleRate * 0.45 && envDb[(size_t) b] > -90) { mean += envDb[(size_t) b]; ++used; }
            mean = used ? mean / used : 0;
            const float amt = p.morph * (1.0f - p.preserve * transient);
            Print gainDb {};
            for (int b = 0; b < numBands; ++b)
                gainDb[(size_t) b] = envDb[(size_t) b] > -90 ? juce::jlimit (-18.0f, 18.0f, amt * (target[(size_t) b] - (envDb[(size_t) b] - mean))) : 0.0f;
            // apply (interpolated between band centres), inverse FFT, synthesis window, overlap-add
            for (int c = 0; c < 2; ++c)
            {
                auto& S = chSpec[(size_t) c];
                for (int k = 0; k <= bins; ++k)
                {
                    const float f = std::max (20.0f, (float) (k * sampleRate / N)), pos = juce::jlimit (0.0f, (float) numBands - 1.001f, 3.0f * std::log2 (f / 20.0f));
                    const int b0 = (int) pos; const float t = pos - b0;
                    const float g = juce::Decibels::decibelsToGain (gainDb[(size_t) b0] + t * (gainDb[(size_t) b0 + 1] - gainDb[(size_t) b0]));
                    S[(size_t) (2 * k)] *= g; S[(size_t) (2 * k + 1)] *= g;
                }
                std::copy (S.begin(), S.begin() + 2 * N, work.begin());
                fft.performRealOnlyInverseTransform (work.data());
                for (int i = 0; i < N; ++i)
                {
                    const float w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * i / N);
                    outAcc[(size_t) c][(size_t) ((fill + i) % N)] += work[(size_t) i] * w / 1.5f;       // Hann x Hann at 75 % overlap sums to 1.5
                }
            }
        }

        double sampleRate = 48000.0;
        std::array<std::unique_ptr<juce::dsp::FFT>, 3> ffts;
        std::array<std::vector<float>, 2> inFifo, outAcc, pad;
        std::vector<float> work, prevMag, envDb;
        std::array<std::vector<float>, 2> chSpec;           // per-channel spectrum of the current frame (no allocation per frame)
        std::array<std::atomic<float>, numBands> longEnv {};
        int fill = 0, hopCount = 0, padPos = 0, curN = -1;
        double fluxAvg = 0, longFrames = 0; float transient = 0;
        std::array<Print, 4> nodes;
        mutable juce::SpinLock nodeLock;
    };
}
