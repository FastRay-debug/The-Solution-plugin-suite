#pragma once

#include <juce_dsp/juce_dsp.h>
#include <cmath>

namespace SolutionDSP
{
    //==========================================================================
    // "Glue": gentle, slow, stereo-linked bus compression. amount 0 = exact bypass.
    class GlueCompressor
    {
    public:
        void prepare (double sampleRate)
        {
            fs = sampleRate;
            attackCoeff  = std::exp (-1.0 / (0.030 * fs));    // 30 ms
            releaseCoeff = std::exp (-1.0 / (0.250 * fs));    // 250 ms
            detCoeff     = std::exp (-1.0 / (0.010 * fs));    // 10 ms RMS window
            reset();
        }
        void reset() { meanSquare = 0.0; envDb = 0.0; lastGrDb = 0.0f; }
        float getGainReductionDb() const { return lastGrDb; }

        void process (float* l, float* r, int numSamples, float amount)
        {
            if (amount <= 0.0005f) { lastGrDb = 0.0f; envDb = 0.0; return; }
            const double threshold = -10.0 - 14.0 * amount;     // -10 .. -24 dBFS
            const double ratio     = 1.0 + 1.5 * amount;        // 1:1 .. 2.5:1
            const double knee      = 6.0;
            const double makeupDb  = 0.5 * staticReduction (-16.0, threshold, ratio, knee);   // half-compensate typical level
            double maxGr = 0.0;
            for (int i = 0; i < numSamples; ++i)
            {
                const double x2 = 0.5 * ((double) l[i] * l[i] + (double) r[i] * r[i]);
                meanSquare = detCoeff * meanSquare + (1.0 - detCoeff) * x2;
                const double levelDb = 10.0 * std::log10 (meanSquare + 1e-12);
                const double target = staticReduction (levelDb, threshold, ratio, knee);     // <= 0 dB
                const double coeff = target < envDb ? attackCoeff : releaseCoeff;
                envDb = coeff * envDb + (1.0 - coeff) * target;
                const float g = (float) std::pow (10.0, (envDb + makeupDb) / 20.0);
                l[i] *= g; r[i] *= g;
                maxGr = std::min (maxGr, envDb);
            }
            lastGrDb = (float) maxGr;
        }

    private:
        static double staticReduction (double levelDb, double t, double ratio, double knee)
        {
            const double over = levelDb - t;
            if (over <= -knee / 2.0) return 0.0;
            if (over >= knee / 2.0)  return -(over - over / ratio);
            const double x = over + knee / 2.0;                       // soft knee
            return -((1.0 - 1.0 / ratio) * x * x / (2.0 * knee));
        }

        double fs = 48000.0, attackCoeff = 0.0, releaseCoeff = 0.0, detCoeff = 0.0;
        double meanSquare = 0.0, envDb = 0.0;
        float lastGrDb = 0.0f;
    };

    //==========================================================================
    // Duck key: turns a source's audio into a 0..1 level (-36 dBFS -> 0, -12 dBFS -> 1).
    class DuckKeyFollower
    {
    public:
        void prepare (double fs)
        {
            attack  = (float) std::exp (-1.0 / (0.005 * fs));     // 5 ms
            release = (float) std::exp (-1.0 / (0.180 * fs));     // 180 ms
            env = 0.0f;
        }
        float process (const float* l, const float* r, int numSamples)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float x = std::max (std::abs (l[i]), std::abs (r[i]));
                env = x > env ? attack * env + (1.0f - attack) * x : release * env + (1.0f - release) * x;
            }
            const float db = juce::Decibels::gainToDecibels (env, -100.0f);
            return juce::jlimit (0.0f, 1.0f, (db + 36.0f) / 24.0f);
        }
    private:
        float attack = 0.0f, release = 0.0f, env = 0.0f;
    };

    // Applies the group's duck key to this instance with smooth gain changes.
    class LinkDucker
    {
    public:
        void prepare (double fs) { smoothing = (float) std::exp (-1.0 / (0.010 * fs)); gain = 1.0f; }
        float getGainReductionDb() const { return juce::Decibels::gainToDecibels (gain); }

        void process (float* l, float* r, int numSamples, float key, float depthDb)
        {
            const float target = juce::Decibels::decibelsToGain (-depthDb * juce::jlimit (0.0f, 1.0f, key));
            if (gain == 1.0f && target == 1.0f) return;
            for (int i = 0; i < numSamples; ++i)
            {
                gain = smoothing * gain + (1.0f - smoothing) * target;
                l[i] *= gain; r[i] *= gain;
            }
            if (std::abs (gain - 1.0f) < 1e-6f && target == 1.0f) gain = 1.0f;
        }
    private:
        float smoothing = 0.99f, gain = 1.0f;
    };
}
