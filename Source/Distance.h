#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>
#include <vector>

// Real distance for the 3D stage. 2 m is the reference (no change). Farther away:
//  - level falls about 6 dB per doubling of distance (inverse distance law)
//  - air absorbs high frequencies (gentle low-pass that closes as distance grows)
//  - more of the room is heard relative to the direct sound (small diffuse ambience)
// Closer than 2 m: up to +6 dB proximity, no filtering.
namespace SolutionDSP
{
    class DistanceStage
    {
    public:
        static constexpr float referenceMetres = 2.0f;

        void prepare (double sampleRate)
        {
            fs = sampleRate;
            for (auto& l : lines) l.assign ((size_t) (0.25 * fs) + 4, 0.0f);
            const double base[4] = { 0.0373, 0.0447, 0.0521, 0.0613 };      // ~medium room
            for (int k = 0; k < 4; ++k)
            {
                len[(size_t) k] = (int) std::round (base[k] * fs);
                fb[(size_t) k] = (float) std::pow (10.0, -3.0 * len[(size_t) k] / (0.9 * fs));   // ~0.9 s decay
            }
            smoothing = (float) std::exp (-1.0 / (0.030 * fs));
            reset();
        }

        void reset()
        {
            for (auto& l : lines) std::fill (l.begin(), l.end(), 0.0f);
            lpState.fill (0.0f); damp.fill (0.0f);
            pos = 0; gain = 1.0f; air = 0.0f; send = 0.0f;
        }

        void process (float* l, float* r, int n, float metres)
        {
            float d = juce::jlimit (0.5f, 40.0f, metres);
            if (std::abs (d - referenceMetres) < 0.01f) d = referenceMetres;   // parameter rounding: treat as the reference
            const float targetGain = juce::jlimit (0.03f, 2.0f, referenceMetres / d);
            const float cutoff = 20000.0f * std::pow (referenceMetres / std::max (d, referenceMetres), 0.6f);
            const float lpCoeff = (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * std::min (cutoff, (float) (0.45 * fs)) / fs));
            // Fade the air absorption in over the first 3 m past the reference (no sudden switch).
            const float targetAir = juce::jlimit (0.0f, 1.0f, (d - referenceMetres) / 3.0f);
            const float targetSend = d <= referenceMetres ? 0.0f : 0.5f * std::pow ((d - referenceMetres) / 38.0f, 0.6f);

            if (targetGain == 1.0f && gain == 1.0f && targetSend == 0.0f && send < 1.0e-5f && targetAir == 0.0f && air < 1.0e-5f)
            {
                gain = 1.0f; send = 0.0f; air = 0.0f;
                return;                                                     // at the reference distance: exactly transparent
            }

            const int L = (int) lines[0].size();
            for (int i = 0; i < n; ++i)
            {
                gain = smoothing * gain + (1 - smoothing) * targetGain;
                air = smoothing * air + (1 - smoothing) * targetAir;
                send = smoothing * send + (1 - smoothing) * targetSend;

                float x[2] = { l[i], r[i] };
                for (int c = 0; c < 2; ++c)                                  // air absorption
                {
                    lpState[(size_t) c] += lpCoeff * (x[c] - lpState[(size_t) c]);
                    x[c] += air * (lpState[(size_t) c] - x[c]);
                }

                // Diffuse ambience (4-line FDN), fed by the (absorbed) source.
                float y[4];
                for (int k = 0; k < 4; ++k)
                {
                    const float v = lines[(size_t) k][(size_t) ((pos - len[(size_t) k] + L) % L)];
                    damp[(size_t) k] += 0.35f * (v - damp[(size_t) k]);
                    y[k] = damp[(size_t) k] * fb[(size_t) k];
                }
                const float in = 0.5f * (x[0] + x[1]);
                const float h[4] = { 0.5f * (y[0] + y[1] + y[2] + y[3]), 0.5f * (y[0] - y[1] + y[2] - y[3]),
                                     0.5f * (y[0] + y[1] - y[2] - y[3]), 0.5f * (y[0] - y[1] - y[2] + y[3]) };
                for (int k = 0; k < 4; ++k) lines[(size_t) k][(size_t) pos] = in + h[k];
                pos = (pos + 1) % L;

                l[i] = gain * x[0] + send * (y[0] + y[2]);
                r[i] = gain * x[1] + send * (y[1] + y[3]);
            }
        }

    private:
        double fs = 48000.0;
        std::array<std::vector<float>, 4> lines;
        std::array<int, 4> len {};
        std::array<float, 4> fb {}, damp {};
        std::array<float, 2> lpState {};
        int pos = 0;
        float gain = 1.0f, air = 0.0f, send = 0.0f, smoothing = 0.99f;
    };
}
