#pragma once
// Test-tone analyser (PluginDoctor-style), run offline on a private copy of the module.
//  - Response: exponential sine sweep -> transfer function (magnitude + phase), latency removed.
//  - Harmonics: a pure tone in, the output spectrum out, harmonics 2..10, THD and THD+N.
//  - Transfer: input level vs output level for a 1 kHz tone from -60 to +6 dBFS (saturation/compression curve).
//  - Before/after: the stimulus spectrum and the processed spectrum, for stacking in the UI.
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <complex>
#include <functional>
#include <vector>

namespace SolutionAnalyzer
{
    // Processes the whole buffer in place (the caller splits it into host-sized blocks). reset() clears all state.
    struct Device
    {
        std::function<void (juce::AudioBuffer<float>&)> process;
        std::function<void()> reset;
        int latency = 0;
        double sampleRate = 48000.0;
    };

    struct Curve { std::vector<float> hz, l, r; };                  // dB (or degrees) per frequency, left/right
    struct Harmonics
    {
        float fundamentalHz = 1000.0f, fundamentalDb = -100.0f;
        std::vector<float> levelsDb;                                  // h2..h10 relative to the fundamental (dBc)
        float thdPct = 0.0f, thdnPct = 0.0f;
        Curve inSpectrum, outSpectrum;                                // dBFS, left/right (input is mono so l == r)
    };
    struct Transfer { std::vector<float> inDb, outDbL, outDbR; };

    inline std::vector<float> logFreqs (int n, double lo, double hi)
    {
        std::vector<float> f ((size_t) n);
        for (int i = 0; i < n; ++i) f[(size_t) i] = (float) (lo * std::pow (hi / lo, i / (double) (n - 1)));
        return f;
    }

    // Complex spectrum of a real signal (zero padded to 2^order).
    inline std::vector<std::complex<float>> spectrum (const float* x, int n, int order)
    {
        const int N = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<std::complex<float>> in ((size_t) N), out ((size_t) N);
        for (int i = 0; i < std::min (n, N); ++i) in[(size_t) i] = x[i];
        fft.perform (in.data(), out.data(), false);
        return out;
    }

    // ---------- frequency & phase response ----------
    inline void response (Device& d, Curve& mag, Curve& phase, int points = 320, float levelDb = -18.0f)
    {
        const double fs = d.sampleRate;
        const int order = fs > 100000.0 ? 18 : 17, N = 1 << order, sweepLen = N / 2;
        const double f1 = 10.0, f2 = std::min (22000.0, fs * 0.47), T = sweepLen / fs, L = T / std::log (f2 / f1);
        const float amp = juce::Decibels::decibelsToGain (levelDb);
        std::vector<float> x ((size_t) N, 0.0f);
        for (int i = 0; i < sweepLen; ++i)
        {
            const double t = i / fs, fade = std::min (1.0, std::min (i, sweepLen - 1 - i) / (0.005 * fs));
            x[(size_t) i] = (float) (amp * fade * std::sin (2.0 * juce::MathConstants<double>::pi * f1 * L * (std::exp (t / L) - 1.0)));
        }
        d.reset();
        juce::AudioBuffer<float> b (2, N);
        b.copyFrom (0, 0, x.data(), N); b.copyFrom (1, 0, x.data(), N);
        d.process (b);
        const auto X = spectrum (x.data(), N, order);
        mag.hz = phase.hz = logFreqs (points, 20.0, std::min (20000.0, fs * 0.45));
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto Y = spectrum (b.getReadPointer (ch), N, order);
            auto& m = ch ? mag.r : mag.l; auto& p = ch ? phase.r : phase.l; m.clear(); p.clear();
            for (size_t i = 0; i < mag.hz.size(); ++i)
            {
                // Average H over a 1/24-octave band around each point (smooths the deconvolution ripple).
                const double fc = mag.hz[i], lo = fc * std::pow (2.0, -1.0 / 48), hi = fc * std::pow (2.0, 1.0 / 48);
                const int k0 = std::max (1, (int) std::floor (lo * N / fs)), k1 = std::max (k0, (int) std::ceil (hi * N / fs));
                std::complex<double> acc = 0; double pw = 0;
                for (int k = k0; k <= k1 && k < N / 2; ++k)
                {
                    const std::complex<double> xk (X[(size_t) k].real(), X[(size_t) k].imag()), yk (Y[(size_t) k].real(), Y[(size_t) k].imag());
                    // remove the reported latency so the phase shows the module, not the delay compensation
                    const double w = 2.0 * juce::MathConstants<double>::pi * k * d.latency / N;
                    acc += yk * std::conj (xk) * std::polar (1.0, w); pw += std::norm (xk);
                }
                const auto h = pw > 1e-12 ? acc / pw : std::complex<double> (0.0);
                m.push_back ((float) juce::Decibels::gainToDecibels (std::abs (h), -120.0));
                p.push_back ((float) (std::arg (h) * 180.0 / juce::MathConstants<double>::pi));
            }
        }
    }

    // ---------- harmonics / THD with before-after spectra ----------
    inline std::vector<float> windowedPowerSpectrum (const float* x, int order)
    {
        const int N = 1 << order;
        std::vector<float> w ((size_t) N);
        for (int i = 0; i < N; ++i)   // 4-term Blackman-Harris: -92 dB sidelobes
        { const double a = 2.0 * juce::MathConstants<double>::pi * i / (N - 1);
          w[(size_t) i] = (float) (0.35875 - 0.48829 * std::cos (a) + 0.14128 * std::cos (2 * a) - 0.01168 * std::cos (3 * a)) * x[i]; }
        const auto S = spectrum (w.data(), N, order);
        std::vector<float> p ((size_t) N / 2);
        const double norm = 2.0 / (0.35875 * N);                      // coherent gain -> a full-scale sine reads 0 dBFS peak
        for (int k = 0; k < N / 2; ++k) p[(size_t) k] = (float) (std::norm (S[(size_t) k]) * norm * norm);
        return p;
    }
    static constexpr double bhEnbw = 2.0044;                          // equivalent noise bandwidth of the window, in bins
    inline float bandPower (const std::vector<float>& p, double hz, double fs, int N, int halfWidth = 5)
    {
        const int c = (int) std::round (hz * N / fs); double s = 0;
        for (int k = std::max (1, c - halfWidth); k <= std::min ((int) p.size() - 1, c + halfWidth); ++k) s += p[(size_t) k];
        return (float) (s / bhEnbw);                                    // = (amplitude)^2 of a sine in that band
    }
    inline Curve binSpectrum (const std::vector<float>& pl, const std::vector<float>& pr, double fs, int N, int points)
    {
        Curve c; c.hz = logFreqs (points, 20.0, std::min (20000.0, fs * 0.47));
        for (size_t i = 0; i < c.hz.size(); ++i)
        {
            const double lo = c.hz[i] * std::pow (2.0, -1.0 / 64), hi = c.hz[i] * std::pow (2.0, 1.0 / 64);
            const int k0 = std::max (1, (int) (lo * N / fs)), k1 = std::max (k0, (int) std::ceil (hi * N / fs));
            float ml = 0, mr = 0;
            for (int k = k0; k <= k1 && k < (int) pl.size(); ++k) { ml = std::max (ml, pl[(size_t) k]); mr = std::max (mr, pr[(size_t) k]); }
            c.l.push_back ((float) juce::Decibels::gainToDecibels (std::sqrt (ml), -160.0f));
            c.r.push_back ((float) juce::Decibels::gainToDecibels (std::sqrt (mr), -160.0f));
        }
        return c;
    }
    inline Harmonics harmonics (Device& d, float hz = 1000.0f, float levelDb = -18.0f, int points = 480)
    {
        const double fs = d.sampleRate;
        const int order = 15, N = 1 << order, settle = (int) (0.5 * fs) + d.latency, total = settle + N;
        // Put the tone exactly on an FFT bin so the window leaks as little as possible.
        const double f0 = std::max (1.0, std::round (hz * N / fs)) * fs / N;
        const float amp = juce::Decibels::decibelsToGain (levelDb);
        juce::AudioBuffer<float> b (2, total);
        for (int i = 0; i < total; ++i)
        {
            const float v = (float) (amp * std::min (1.0, i / (0.01 * fs)) * std::sin (2.0 * juce::MathConstants<double>::pi * f0 * i / fs));
            b.setSample (0, i, v); b.setSample (1, i, v);
        }
        std::vector<float> in ((size_t) N);
        for (int i = 0; i < N; ++i) in[(size_t) i] = b.getSample (0, settle + i);
        d.reset(); d.process (b);
        const auto pin = windowedPowerSpectrum (in.data(), order);
        const auto pl = windowedPowerSpectrum (b.getReadPointer (0, settle), order), pr = windowedPowerSpectrum (b.getReadPointer (1, settle), order);
        Harmonics h; h.fundamentalHz = (float) f0;
        std::vector<float> pm (pl.size());                            // analyse the louder of L/R per bin (mono sum would hide panning)
        for (size_t k = 0; k < pl.size(); ++k) pm[k] = std::max (pl[k], pr[k]);
        const float fund = bandPower (pm, f0, fs, N);
        h.fundamentalDb = juce::Decibels::gainToDecibels (std::sqrt (fund), -160.0f);
        double harmSum = 0;
        for (int n = 2; n <= 10; ++n)
        {
            const double fh = f0 * n;
            const float pw = fh < fs * 0.49 ? bandPower (pm, fh, fs, N) : 0.0f;
            harmSum += pw;
            h.levelsDb.push_back (juce::Decibels::gainToDecibels (std::sqrt (pw / std::max (1e-30f, fund)), -160.0f));
        }
        double total2 = 0; for (size_t k = 1; k < pm.size(); ++k) if ((double) k * fs / N > 10.0) total2 += pm[k];
        total2 /= bhEnbw;
        const double noise = std::max (0.0, total2 - fund);
        h.thdPct = (float) (100.0 * std::sqrt (harmSum / std::max (1e-30, (double) fund)));
        h.thdnPct = (float) (100.0 * std::sqrt (noise / std::max (1e-30, (double) fund)));
        h.inSpectrum = binSpectrum (pin, pin, fs, N, points);
        h.outSpectrum = binSpectrum (pl, pr, fs, N, points);
        return h;
    }

    // ---------- static transfer curve ----------
    inline Transfer transfer (Device& d, float hz = 1000.0f)
    {
        Transfer t; const double fs = d.sampleRate;
        const int hold = (int) (0.3 * fs), measure = (int) (0.1 * fs);
        d.reset();
        double ph = 0;
        for (float lvl = -60.0f; lvl <= 6.01f; lvl += 3.0f)
        {
            const float amp = juce::Decibels::decibelsToGain (lvl);
            juce::AudioBuffer<float> b (2, hold);
            for (int i = 0; i < hold; ++i) { const float v = (float) (amp * std::sin (ph)); ph += 2.0 * juce::MathConstants<double>::pi * hz / fs; b.setSample (0, i, v); b.setSample (1, i, v); }
            d.process (b);
            const float rl = b.getRMSLevel (0, hold - measure, measure), rr = b.getRMSLevel (1, hold - measure, measure);
            t.inDb.push_back (lvl);                                                         // peak dBFS of the tone going in
            t.outDbL.push_back (juce::Decibels::gainToDecibels (rl * std::sqrt (2.0f), -120.0f));   // as peak-equivalent dBFS
            t.outDbR.push_back (juce::Decibels::gainToDecibels (rr * std::sqrt (2.0f), -120.0f));
        }
        return t;
    }
}
