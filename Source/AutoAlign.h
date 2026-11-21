#pragma once

#include <juce_core/juce_core.h>
#include <cmath>
#include <vector>

// Auto-Align: finds the timing offset and polarity between two mics on the same source
// (kick in/out, snare top/bottom, bass DI/amp...) by cross-correlating timeline-aligned audio.
namespace SolutionDSP
{
    struct AlignResult
    {
        bool found = false;
        double lagSamples = 0.0;     // + = the follower hears the sound later than the reference
        bool invert = false;         // polarity is flipped between them
        double confidence = 0.0;     // 0..1 (normalised correlation peak)
        juce::String message;
    };

    // ref/fol: rings of size N where timeline sample t is stored at index t mod N;
    // refEnd/folEnd = timeline index one past the newest sample written.
    inline AlignResult computeAlignment (const float* ref, long long refEnd, const float* fol, long long folEnd, int N, double sampleRate)
    {
        AlignResult r;
        if (refEnd < 0 || folEnd < 0) { r.message = "Press play first - both tracks need to be playing"; return r; }
        const int maxLag = std::min ((int) std::round (0.020 * sampleRate), 3000);   // up to 20 ms
        const int guard = 2048;                                                     // skip samples that may be mid-write
        const long long end = std::min (refEnd, folEnd) - guard - maxLag;
        const long long start = std::max (refEnd, folEnd) - N + maxLag;
        const int window = (int) std::min<long long> (8192, end - start);
        if (window < 2048) { r.message = "Not enough overlapping audio yet - let both tracks play for a moment"; return r; }
        const long long t0 = end - window;

        auto at = [N] (const float* ring, long long, long long t) { return ring[(size_t) ((t % N + N) % N)]; };
        // Copy windows (follower with +/- maxLag margin) so the correlation loop is simple.
        std::vector<float> x ((size_t) window), y ((size_t) (window + 2 * maxLag));
        double ex = 0.0;
        for (int i = 0; i < window; ++i) { x[(size_t) i] = at (ref, refEnd, t0 + i); ex += (double) x[(size_t) i] * x[(size_t) i]; }
        for (int i = 0; i < window + 2 * maxLag; ++i) y[(size_t) i] = at (fol, folEnd, t0 - maxLag + i);
        if (ex < 1e-9) { r.message = "The reference track is silent"; return r; }

        std::vector<double> c ((size_t) (2 * maxLag + 1));
        double best = 0.0; int bestIdx = maxLag;
        for (int L = 0; L <= 2 * maxLag; ++L)
        {
            double acc = 0.0, ey = 0.0;
            for (int i = 0; i < window; ++i) { const double v = y[(size_t) (i + L)]; acc += x[(size_t) i] * v; ey += v * v; }
            c[(size_t) L] = acc / (std::sqrt (ex * ey) + 1e-20);
            if (std::abs (c[(size_t) L]) > std::abs (best)) { best = c[(size_t) L]; bestIdx = L; }
        }
        r.confidence = std::abs (best);
        if (r.confidence < 0.3) { r.message = "Couldn't find a clear match - are both tracks recording the same source?"; return r; }

        // Parabolic interpolation for sub-sample accuracy.
        double frac = 0.0;
        if (bestIdx > 0 && bestIdx < 2 * maxLag)
        {
            const double a = std::abs (c[(size_t) (bestIdx - 1)]), b = std::abs (c[(size_t) bestIdx]), d = std::abs (c[(size_t) (bestIdx + 1)]);
            const double denom = a - 2 * b + d;
            if (std::abs (denom) > 1e-12) frac = juce::jlimit (-0.5, 0.5, 0.5 * (a - d) / denom);
        }
        r.found = true;
        r.lagSamples = (bestIdx - maxLag) + frac;
        r.invert = best < 0.0;
        return r;
    }
}
