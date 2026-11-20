#include "RoomCorrection.h"
#include <complex>

namespace SolutionDSP
{
namespace
{
    // Fractional-octave smoothing of a magnitude spectrum (dB in, dB out) at log-spaced points.
    void smoothToPoints (const std::vector<float>& magDb, double binHz, RoomAnalysis& out)
    {
        const double width = 1.0 / 6.0;                    // 1/6 octave
        for (int i = 0; i < RoomAnalysis::numPoints; ++i)
        {
            const double f = RoomAnalysis::pointFreq (i);
            out.freqs[(size_t) i] = (float) f;
            const double lo = f * std::pow (2.0, -width / 2.0), hi = f * std::pow (2.0, width / 2.0);
            int b0 = std::max (1, (int) std::floor (lo / binHz)), b1 = std::min ((int) magDb.size() - 1, (int) std::ceil (hi / binHz));
            if (b1 < b0) b1 = b0;
            double power = 0.0;
            for (int b = b0; b <= b1; ++b) power += std::pow (10.0, magDb[(size_t) b] / 10.0);
            out.rawDb[(size_t) i] = (float) (10.0 * std::log10 (power / (b1 - b0 + 1) + 1e-20));
        }
    }

}

void refitRoomAnalysis (RoomAnalysis& a, const MicProfile& mic, double fs)
{
    constexpr int N = RoomAnalysis::numPoints;
    constexpr double pointsPerOctave = (N - 1) / (3.0 * 3.3219280948873622);   // 10 octaves-ish over 20 Hz-20 kHz
    a.micName = mic.name;
    a.fitLowHz = std::max (25.0f, mic.reliableLowHz);
    a.fitHighHz = std::min (5000.0f, mic.reliableHighHz);
    a.bands.clear();

    // 1. Remove the mic's own colouration.
    std::array<float, N> comp {};
    for (int i = 0; i < N; ++i)
        comp[(size_t) i] = a.rawDb[(size_t) i] - mic.responseAt (a.freqs[(size_t) i]);

    // 2. Extra smoothing for less trustworthy mics (beyond the 1/6 octave already applied).
    const int half = (int) std::round (std::max (0.0, (mic.smoothingOctaves - 1.0 / 6.0)) * pointsPerOctave / 2.0);
    for (int i = 0; i < N; ++i)
    {
        double power = 0.0; int n = 0;
        for (int k = std::max (0, i - half); k <= std::min (N - 1, i + half); ++k) { power += std::pow (10.0, comp[(size_t) k] / 10.0); ++n; }
        a.measuredDb[(size_t) i] = (float) (10.0 * std::log10 (power / n + 1e-20));
    }

    // 3. Normalise so the trusted mid-band average sits at 0 dB.
    const float refLo = std::max (200.0f, a.fitLowHz), refHi = std::min (2000.0f, a.fitHighHz);
    double ref = 0.0; int n = 0;
    for (int i = 0; i < N; ++i)
        if (a.freqs[(size_t) i] >= refLo && a.freqs[(size_t) i] <= refHi) { ref += a.measuredDb[(size_t) i]; ++n; }
    ref /= std::max (1, n);
    for (auto& v : a.measuredDb) v -= (float) ref;

    // 4. Fit peaking filters only where this mic can be trusted.
    std::array<float, N> corr {};
    auto inRange = [&] (int i) { return a.freqs[(size_t) i] >= a.fitLowHz && a.freqs[(size_t) i] <= a.fitHighHz; };
    for (int iter = 0; iter < RoomCorrector::maxBands; ++iter)
    {
        int best = -1; float bestScore = 0.0f;
        for (int i = 0; i < N; ++i)
        {
            if (! inRange (i)) continue;
            const float err = a.measuredDb[(size_t) i] + corr[(size_t) i];
            const float score = err > 0.0f ? err : -0.5f * err;     // dips count half: boosting nulls rarely works
            if (score > bestScore) { bestScore = score; best = i; }
        }
        if (best < 0 || bestScore < 1.5f) break;

        const float err = a.measuredDb[(size_t) best] + corr[(size_t) best];
        const float gain = juce::jlimit (-mic.maxCutDb, 3.0f, -err);
        int lo = best, hi = best;
        while (lo > 0 && std::abs (a.measuredDb[(size_t) lo] + corr[(size_t) lo]) > std::abs (err) * 0.5f) --lo;
        while (hi < N - 1 && std::abs (a.measuredDb[(size_t) hi] + corr[(size_t) hi]) > std::abs (err) * 0.5f) ++hi;
        const double octaves = std::max (0.1, std::log2 ((double) a.freqs[(size_t) hi] / a.freqs[(size_t) lo]));
        const double q = juce::jlimit (0.5, 12.0, std::sqrt (std::pow (2.0, octaves)) / (std::pow (2.0, octaves) - 1.0));

        EqBand band { a.freqs[(size_t) best], gain, (float) q };
        a.bands.push_back (band);
        const auto c = RoomCorrector::peaking (fs, band.freq, band.gainDb, band.q);
        for (int i = 0; i < N; ++i)
            corr[(size_t) i] += (float) RoomCorrector::magnitudeDb (c, fs, a.freqs[(size_t) i]);
    }
    for (int i = 0; i < N; ++i)
    {
        a.correctionDb[(size_t) i] = corr[(size_t) i];
        a.resultDb[(size_t) i] = a.measuredDb[(size_t) i] + corr[(size_t) i];
    }
    a.message = juce::String (a.bands.size()) + " correction filter" + (a.bands.size() == 1 ? "" : "s") + " fitted ("
              + juce::String (juce::roundToInt (a.fitLowHz)) + " Hz-" + juce::String (a.fitHighHz / 1000.0f, 1) + " kHz, " + mic.name + ")";
}

RoomAnalysis RoomMeasurement::analyse (const MicProfile& mic) const
{
    RoomAnalysis result;
    if (state.load() != State::finished || sweep.empty())
    {
        result.message = "No finished measurement";
        return result;
    }

    // Check the mic actually heard something.
    float recPeak = 0.0f;
    for (float v : recording) recPeak = std::max (recPeak, std::abs (v));
    if (recPeak < 1.0e-4f)
    {
        result.message = "The input was silent - is the mic on this track's input, and the output going to your speakers?";
        return result;
    }
    if (recPeak > 0.999f)
        result.message = "Warning: the mic input clipped - lower the input gain and measure again. ";

    // Deconvolve: H = Y * conj(X) / (|X|^2 + eps)
    const int total = (int) recording.size() + (int) sweep.size();
    int order = 1; while ((1 << order) < total) ++order;
    const int N = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<std::complex<float>> X ((size_t) N), Y ((size_t) N), tmp ((size_t) N);
    for (size_t i = 0; i < sweep.size(); ++i) tmp[i] = sweep[i];
    fft.perform (tmp.data(), X.data(), false);
    std::fill (tmp.begin(), tmp.end(), std::complex<float>());
    for (size_t i = 0; i < recording.size(); ++i) tmp[i] = recording[i];
    fft.perform (tmp.data(), Y.data(), false);

    float maxX = 0.0f;
    for (auto& v : X) maxX = std::max (maxX, std::norm (v));
    const float eps = maxX * 1.0e-6f;
    for (int k = 0; k < N; ++k)
        tmp[(size_t) k] = Y[(size_t) k] * std::conj (X[(size_t) k]) / (std::norm (X[(size_t) k]) + eps);
    std::vector<std::complex<float>> ir ((size_t) N);
    fft.perform (tmp.data(), ir.data(), true);

    // Find the direct sound and keep ~300 ms after it (drops noise and the very late tail).
    int peakIdx = 0; float peakVal = 0.0f;
    for (int k = 0; k < N; ++k) if (std::abs (ir[(size_t) k].real()) > peakVal) { peakVal = std::abs (ir[(size_t) k].real()); peakIdx = k; }
    const int pre = (int) (0.002 * fs), post = (int) (0.300 * fs);
    int winOrder = 1; while ((1 << winOrder) < pre + post) ++winOrder;
    const int M = 1 << winOrder;
    juce::dsp::FFT fft2 (winOrder);
    std::vector<std::complex<float>> win ((size_t) M), spec ((size_t) M);
    for (int k = -pre; k < post; ++k)
    {
        const int src = (peakIdx + k + N) % N;
        const double t = k < 0 ? 0.5 * (1.0 - std::cos (juce::MathConstants<double>::pi * (k + pre) / pre))
                               : (k > post * 0.8 ? 0.5 * (1.0 + std::cos (juce::MathConstants<double>::pi * (k - post * 0.8) / (post * 0.2))) : 1.0);
        win[(size_t) (k + pre)] = (float) t * ir[(size_t) src].real();
    }
    fft2.perform (win.data(), spec.data(), false);

    std::vector<float> magDb ((size_t) M / 2);
    for (int k = 0; k < M / 2; ++k) magDb[(size_t) k] = (float) (20.0 * std::log10 (std::abs (spec[(size_t) k]) + 1e-12));
    const auto warning = result.message;
    smoothToPoints (magDb, fs / M, result);
    refitRoomAnalysis (result, mic, fs);
    result.valid = true;
    result.message = warning + result.message;
    return result;
}
}
