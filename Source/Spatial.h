#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>
#include <vector>

// Binaural headphone rendering. The stereo input becomes two virtual speakers (default +/-30 deg)
// around the listener's head; azimuth rotates the pair, elevation raises it, width spreads it.
// Model: Woodworth interaural time difference + Brown-Duda spherical-head shadow filter,
// plus simple pinna (elevation) and front/back cues. No latency: only the far ear is delayed.
namespace SolutionDSP
{
    class BinauralRenderer
    {
    public:
        void prepare (double sampleRate)
        {
            fs = sampleRate;
            maxDelay = (int) std::ceil (0.0012 * fs) + 4;
            for (auto& l : lines) l.assign ((size_t) maxDelay + 1, 0.0f);
            reset();
        }

        void reset()
        {
            for (auto& l : lines) std::fill (l.begin(), l.end(), 0.0f);
            for (auto& s : shadowState) s = {};
            for (auto& s : pinnaState) s = {};
            for (auto& s : rearState) s = {};
            writePos = 0; first = true;
        }

        // In-place: stereo in -> binaural L/R out.
        void process (float* left, float* right, int numSamples, float azimuthDeg, float elevationDeg, float width)
        {
            const float spread = 30.0f * juce::jlimit (0.0f, 2.0f, width);
            const float targetAz[2] = { azimuthDeg - spread, azimuthDeg + spread };

            // Smooth angle changes per block (avoids zipper noise when automating).
            for (int s = 0; s < 2; ++s)
            {
                az[(size_t) s] = first ? targetAz[s] : az[(size_t) s] + 0.35f * (targetAz[s] - az[(size_t) s]);
                setupSource (s, az[(size_t) s], elevationDeg);
            }
            first = false;

            const int len = (int) lines[0].size();
            for (int i = 0; i < numSamples; ++i)
            {
                const float in[2] = { left[i], right[i] };
                float ear[2] = { 0.0f, 0.0f };
                for (int s = 0; s < 2; ++s)
                {
                    lines[(size_t) s][(size_t) writePos] = in[s];
                    for (int e = 0; e < 2; ++e)
                    {
                        const auto& sp = paths[(size_t) (s * 2 + e)];
                        // Fractional delay (linear interpolation is fine for <1.2 ms ITDs).
                        const float d = sp.delay;
                        const int di = (int) d; const float frac = d - (float) di;
                        const float a = lines[(size_t) s][(size_t) ((writePos - di + len) % len)];
                        const float b = lines[(size_t) s][(size_t) ((writePos - di - 1 + len) % len)];
                        float x = a + frac * (b - a);
                        x = biquad (sp.shadow, shadowState[(size_t) (s * 2 + e)], x);
                        x = biquad (sp.pinna,  pinnaState[(size_t) (s * 2 + e)], x);
                        x = biquad (sp.rear,   rearState[(size_t) (s * 2 + e)], x);
                        ear[e] += x;
                    }
                }
                writePos = (writePos + 1) % len;
                left[i]  = ear[0] * 0.7071f;
                right[i] = ear[1] * 0.7071f;
            }
        }

        // Woodworth ITD in seconds for a source at azimuth (deg, 0 = front, +90 = right).
        static double itdSeconds (double azDeg)
        {
            constexpr double headRadius = 0.0875, c = 343.0;
            double th = std::fmod (std::abs (azDeg), 360.0); if (th > 180.0) th = 360.0 - th;
            if (th > 90.0) th = 180.0 - th;                                        // front/back symmetric
            const double r = juce::degreesToRadians (th);
            return headRadius / c * (r + std::sin (r));
        }

    private:
        struct Path { float delay = 0.0f; std::array<float, 5> shadow {}, pinna {}, rear {}; };

        static float biquad (const std::array<float, 5>& c, std::array<float, 2>& z, float x)
        {
            const float y = c[0] * x + z[0];
            z[0] = c[1] * x - c[3] * y + z[1];
            z[1] = c[2] * x - c[4] * y;
            return y;
        }

        void setupSource (int s, float azDeg, float elDeg)
        {
            // Wrap to -180..180
            double a = std::fmod ((double) azDeg + 540.0, 360.0) - 180.0;
            const double itd = itdSeconds (a);
            const bool sourceRight = a >= 0.0;

            for (int e = 0; e < 2; ++e)
            {
                auto& p = paths[(size_t) (s * 2 + e)];
                const bool farEar = (e == 0) == sourceRight;     // left ear is far when the source is on the right
                p.delay = farEar ? (float) (itd * fs) : 0.0f;

                // Brown-Duda head shadow: angle between the source and this ear (ear at +/-90 deg).
                const double earAz = e == 0 ? -90.0 : 90.0;
                double theta = std::abs (std::fmod (a - earAz + 540.0, 360.0) - 180.0);   // 0..180
                const double alphaMin = 0.1, thetaMin = 150.0;
                const double alpha = (1.0 + alphaMin / 2.0) + (1.0 - alphaMin / 2.0) * std::cos (juce::degreesToRadians (theta / thetaMin * 180.0));
                const double w0 = 343.0 / 0.0875;                // c / head radius
                // H(s) = (alpha*s + 2*w0) / (s + 2*w0) -> bilinear transform
                const double K = 2.0 * fs, b0 = alpha * K + 2.0 * w0, b1 = -alpha * K + 2.0 * w0, a0 = K + 2.0 * w0, a1 = -K + 2.0 * w0;
                p.shadow = { (float) (b0 / a0), (float) (b1 / a0), 0.0f, (float) (a1 / a0), 0.0f };

                // Pinna: notch slides 6 -> 10 kHz as the source rises; slight lift overhead.
                const double el = juce::jlimit (-90.0, 90.0, (double) elDeg);
                const double notchHz = std::min (0.45 * fs, 7500.0 + 25.0 * el);
                const double notchDb = -6.0 + 3.0 * std::max (0.0, el / 90.0);
                p.pinna = peaking (notchHz, notchDb, 3.0);

                // Behind the head: the outer ear shades high frequencies.
                const double rear = std::max (0.0, (std::abs (a) - 90.0) / 90.0);   // 0 front .. 1 directly behind
                p.rear = highShelf (4000.0, -4.0 * rear);
            }
        }

        std::array<float, 5> peaking (double f, double gainDb, double q) const
        {
            const double A = std::pow (10.0, gainDb / 40.0), w = juce::MathConstants<double>::twoPi * f / fs;
            const double alpha = std::sin (w) / (2.0 * q), cw = std::cos (w), a0 = 1.0 + alpha / A;
            return { (float) ((1.0 + alpha * A) / a0), (float) (-2.0 * cw / a0), (float) ((1.0 - alpha * A) / a0), (float) (-2.0 * cw / a0), (float) ((1.0 - alpha / A) / a0) };
        }

        std::array<float, 5> highShelf (double f, double gainDb) const
        {
            const double A = std::pow (10.0, gainDb / 40.0), w = juce::MathConstants<double>::twoPi * f / fs;
            const double alpha = std::sin (w) / 2.0 * std::sqrt (2.0), cw = std::cos (w), sq = 2.0 * std::sqrt (A) * alpha;
            const double a0 = (A + 1) - (A - 1) * cw + sq;
            return { (float) (A * ((A + 1) + (A - 1) * cw + sq) / a0), (float) (-2 * A * ((A - 1) + (A + 1) * cw) / a0),
                     (float) (A * ((A + 1) + (A - 1) * cw - sq) / a0), (float) (2 * ((A - 1) - (A + 1) * cw) / a0),
                     (float) (((A + 1) - (A - 1) * cw - sq) / a0) };
        }

        double fs = 48000.0;
        int maxDelay = 64, writePos = 0;
        bool first = true;
        std::array<std::vector<float>, 2> lines;
        std::array<Path, 4> paths;
        std::array<std::array<float, 2>, 4> shadowState {}, pinnaState {}, rearState {};
        std::array<float, 2> az {};
    };
}
