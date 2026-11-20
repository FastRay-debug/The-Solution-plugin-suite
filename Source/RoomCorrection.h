#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <vector>

// Real room measurement + correction.
//  1. Measure: the plugin plays an exponential sine sweep to the speakers while recording
//     its input (the measurement mic), then deconvolves the recording into an impulse response.
//  2. Analyse (background thread): window the IR, take its frequency response, 1/6-octave smooth,
//     and fit up to 8 peaking filters - mostly cuts, small boosts only - to flatten 25 Hz-5 kHz.
//  3. Correct: the fitted filters run on the audio thread, scaled by "amount".
namespace SolutionDSP
{
    struct EqBand { float freq = 1000.0f, gainDb = 0.0f, q = 1.0f; };

    struct RoomAnalysis
    {
        static constexpr int numPoints = 240;               // log-spaced 20 Hz - 20 kHz
        std::array<float, numPoints> freqs {}, measuredDb {}, correctionDb {}, resultDb {};
        std::vector<EqBand> bands;
        bool valid = false;
        juce::String message;

        static float pointFreq (int i) { return 20.0f * std::pow (1000.0f, (float) i / (numPoints - 1)); }
    };

    //==========================================================================
    // Sweep generator + recorder. Audio-thread side is allocation-free once armed.
    class RoomMeasurement
    {
    public:
        enum class State { idle, running, finished };

        // Message thread: allocate and arm.
        void arm (double sampleRate, float sweepSeconds)
        {
            fs = sampleRate;
            sweepLen = (int) (std::clamp (sweepSeconds, 1.0f, 10.0f) * fs);
            const int tailLen = (int) (1.5 * fs);
            sweep.assign ((size_t) sweepLen, 0.0f);
            const double f1 = 20.0, f2 = std::min (20000.0, 0.45 * fs), T = sweepLen / fs, R = std::log (f2 / f1);
            for (int n = 0; n < sweepLen; ++n)
            {
                const double t = n / fs;
                const double fade = std::min ({ 1.0, n / (0.02 * fs), (sweepLen - n) / (0.005 * fs) });   // de-click
                sweep[(size_t) n] = (float) (0.25 * fade * std::sin (juce::MathConstants<double>::twoPi * f1 * T / R * (std::exp (t * R / T) - 1.0)));
            }
            recording.assign ((size_t) (sweepLen + tailLen), 0.0f);
            position = 0;
            state.store (State::running);
        }

        void cancel() { if (state.load() == State::running) state.store (State::idle); }
        State getState() const { return state.load(); }
        float getProgress() const { return recording.empty() ? 0.0f : (float) position.load() / (float) recording.size(); }

        // Audio thread: record the (mono-summed) input and replace the output with the sweep.
        void process (const float* inL, const float* inR, float* outL, float* outR, int numSamples)
        {
            if (state.load (std::memory_order_acquire) != State::running) return;
            int pos = position.load (std::memory_order_relaxed);
            const int total = (int) recording.size();
            for (int i = 0; i < numSamples; ++i, ++pos)
            {
                if (pos < total) recording[(size_t) pos] = 0.5f * (inL[i] + inR[i]);
                const float s = pos < sweepLen ? sweep[(size_t) pos] : 0.0f;
                outL[i] = s; outR[i] = s;
            }
            position.store (std::min (pos, total), std::memory_order_relaxed);
            if (pos >= total) state.store (State::finished, std::memory_order_release);
        }

        // Background thread, after finished.
        RoomAnalysis analyse() const;
        void markConsumed() { state.store (State::idle); }

    private:
        double fs = 48000.0;
        int sweepLen = 0;
        std::vector<float> sweep, recording;
        std::atomic<int> position { 0 };
        std::atomic<State> state { State::idle };
    };

    //==========================================================================
    // Applies the fitted correction filters (stereo, double precision, allocation-free).
    class RoomCorrector
    {
    public:
        static constexpr int maxBands = 8;

        void prepare (double sampleRate) { fs = sampleRate; reset(); lastAmount = -1.0f; }
        void reset() { for (auto& s : state) s = {}; }

        // Message thread: hand over new bands (never blocks the audio thread).
        void setBands (const std::vector<EqBand>& newBands)
        {
            const juce::SpinLock::ScopedLockType sl (lock);
            pending = {};
            pendingCount = (int) std::min<size_t> (newBands.size(), maxBands);
            for (int i = 0; i < pendingCount; ++i) pending[(size_t) i] = newBands[(size_t) i];
            hasPending = true;
        }

        void process (float* l, float* r, int numSamples, float amount, bool enabled)
        {
            if (hasPending.load (std::memory_order_acquire))
            {
                const juce::SpinLock::ScopedTryLockType sl (lock);
                if (sl.isLocked())
                {
                    live = pending; liveCount = pendingCount; hasPending = false; lastAmount = -1.0f;
                }
            }
            if (! enabled || liveCount == 0 || amount <= 0.0005f) return;
            if (amount != lastAmount) { updateCoefficients (amount); lastAmount = amount; }

            for (int b = 0; b < liveCount; ++b)
            {
                const auto& c = coeffs[(size_t) b];
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto* d = ch == 0 ? l : r;
                    auto& z = state[(size_t) (b * 2 + ch)];
                    for (int i = 0; i < numSamples; ++i)
                    {
                        const double x = d[i];
                        const double y = c[0] * x + z[0];
                        z[0] = c[1] * x - c[3] * y + z[1];
                        z[1] = c[2] * x - c[4] * y;
                        d[i] = (float) y;
                    }
                }
            }
        }

        // RBJ peaking EQ, normalised {b0, b1, b2, a1, a2}.
        static std::array<double, 5> peaking (double fs, double f, double gainDb, double q)
        {
            const double A = std::pow (10.0, gainDb / 40.0), w = juce::MathConstants<double>::twoPi * f / fs;
            const double alpha = std::sin (w) / (2.0 * q), cw = std::cos (w);
            const double a0 = 1.0 + alpha / A;
            return { (1.0 + alpha * A) / a0, -2.0 * cw / a0, (1.0 - alpha * A) / a0, -2.0 * cw / a0, (1.0 - alpha / A) / a0 };
        }

        static double magnitudeDb (const std::array<double, 5>& c, double fs, double f)
        {
            const std::complex<double> z = std::polar (1.0, -juce::MathConstants<double>::twoPi * f / fs);
            const auto h = (c[0] + c[1] * z + c[2] * z * z) / (1.0 + c[3] * z + c[4] * z * z);
            return 20.0 * std::log10 (std::abs (h) + 1e-12);
        }

    private:
        void updateCoefficients (float amount)
        {
            for (int b = 0; b < liveCount; ++b)
            {
                const auto& band = live[(size_t) b];
                coeffs[(size_t) b] = peaking (fs, std::min ((double) band.freq, 0.45 * fs), band.gainDb * amount, band.q);
            }
        }

        double fs = 48000.0;
        juce::SpinLock lock;
        std::array<EqBand, maxBands> pending {}, live {};
        int pendingCount = 0, liveCount = 0;
        std::atomic<bool> hasPending { false };
        std::array<std::array<double, 5>, maxBands> coeffs {};
        std::array<std::array<double, 2>, maxBands * 2> state {};
        float lastAmount = -1.0f;
    };
}
