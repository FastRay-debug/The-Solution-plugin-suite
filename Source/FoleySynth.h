#pragma once
// Interactive Foley from picture (The Aether): the UI reads the video's motion and sends a few numbers
// ~30 times a second; this turns them into sound in real time.
//  motion   0..1  how much is moving          -> wind / air texture level and brightness
//  pan     -1..1  where the motion is          -> where the sound sits
//  speed    0..1  fast sweeping motion          -> whooshes (filtered noise sweeps)
//  impact   0..1  a sudden hit this frame       -> a thump + crack (impact)
//  wet      0..1  blue / dark / sparkly picture -> rain / water droplet texture density
#include <juce_dsp/juce_dsp.h>
#include "Dialogue.h"

namespace SolutionDSP
{
    class FoleySynth
    {
    public:
        struct Control { float motion = 0, pan = 0, speed = 0, impact = 0, wet = 0; };
        void prepare (double fs) { sampleRate = fs; reset(); }
        void reset() { for (auto& f : windBp) f.clear(); for (auto& f : whooshBp) f.clear(); for (auto& f : rainHp) f.clear(); env = {}; thumpPhase = 0; thumpEnv = 0; crackEnv = 0; whooshEnv = 0; sweep = 0; lastImpact = 0; }
        void setControl (const Control& c)
        {
            const juce::SpinLock::ScopedLockType sl (lock);
            target = c;
            if (c.impact > 0.2f && c.impact > lastImpact + 0.1f) pendingImpact = std::max (pendingImpact, c.impact);     // rising edge = a new hit
            lastImpact = c.impact;
            if (c.speed > 0.35f) pendingWhoosh = std::max (pendingWhoosh, c.speed);
        }
        void process (float* l, float* r, int n, float mix)
        {
            Control c; float hit = 0, wh = 0;
            { const juce::SpinLock::ScopedTryLockType sl (lock); if (sl.isLocked()) { c = target; hit = pendingImpact; pendingImpact = 0; wh = pendingWhoosh; pendingWhoosh = 0; lastControl = c; } else c = lastControl; }
            if (hit > 0) { thumpEnv = hit; crackEnv = hit; thumpPhase = 0; thumpPan = c.pan; }
            if (wh > 0 && whooshEnv < 0.3f) { whooshEnv = wh; sweep = 0; whooshPan = c.pan; }
            const double a = 1.0 - std::exp (-1.0 / (0.08 * sampleRate));                       // smooth the video-rate controls
            for (int i = 0; i < n; ++i)
            {
                env.motion += (c.motion - env.motion) * a; env.pan += (c.pan - env.pan) * a; env.wet += (c.wet - env.wet) * a;
                // wind: band-passed noise, brighter and louder with motion
                if ((i & 31) == 0) { const double fc = 250.0 + 1800.0 * env.motion; { auto& h = windBp[0]; const double z1 = h.z1, z2 = h.z2; h.set (1, sampleRate, fc * 0.4, 0.707); h.z1 = z1; h.z2 = z2; }
                                     { auto& lo = windBp[1]; const double z1 = lo.z1, z2 = lo.z2; lo.set (0, sampleRate, fc * 1.6, 0.9); lo.z1 = z1; lo.z2 = z2; }
                                     const double wf = 300.0 + 4500.0 * sweep; for (auto& f : whooshBp) { const double z1 = f.z1, z2 = f.z2; f.set (0, sampleRate, wf, 2.5); f.z1 = z1; f.z2 = z2; } }
                const float n1 = rng.nextFloat() * 2 - 1, n2 = rng.nextFloat() * 2 - 1;
                const float wind = (float) windBp[1].run (windBp[0].run (n1)) * env.motion * 0.9f;
                // whoosh: a resonant low-pass sweeping up then fading
                float whoosh = 0;
                if (whooshEnv > 0.001f) { sweep = std::min (1.0, sweep + 1.6 / sampleRate); whooshEnv *= (float) std::exp (-1.0 / (0.25 * sampleRate)); whoosh = (float) whooshBp[0].run (n2) * whooshEnv * 0.8f; }
                // impact: a falling-pitch thump + a short noise crack
                float imp = 0;
                if (thumpEnv > 0.001f) { const double f = 55.0 + 60.0 * thumpEnv; thumpPhase += f / sampleRate; imp += (float) std::sin (juce::MathConstants<double>::twoPi * thumpPhase) * thumpEnv; thumpEnv *= (float) std::exp (-1.0 / (0.18 * sampleRate)); }
                if (crackEnv > 0.001f) { imp += n2 * crackEnv * 0.6f; crackEnv *= (float) std::exp (-1.0 / (0.012 * sampleRate)); }
                // rain / water: sparse bright droplets
                float rain = 0;
                if (rng.nextFloat() < env.wet * env.wet * 0.004f) dropEnv = 0.4f + rng.nextFloat() * 0.6f;
                if (dropEnv > 0.001f) { rain = (float) rainHp[0].run (n1) * dropEnv; dropEnv *= 0.985f; }
                rain += (float) rainHp[1].run (n2) * env.wet * 0.08f;
                // place it: wind + rain follow the motion's side, whoosh / impact where they happened
                auto panL = [] (float p) { return std::cos ((p + 1.0f) * juce::MathConstants<float>::pi * 0.25f); };
                auto panR = [] (float p) { return std::sin ((p + 1.0f) * juce::MathConstants<float>::pi * 0.25f); };
                const float bed = wind + rain;
                const float outL = bed * panL (env.pan * 0.6f) + whoosh * panL (whooshPan) + imp * panL (thumpPan);
                const float outR = bed * panR (env.pan * 0.6f) + whoosh * panR (whooshPan) + imp * panR (thumpPan);
                l[i] += outL * mix; r[i] += outR * mix;
                lastLevel = std::max (lastLevel * 0.9995f, std::abs (outL) + std::abs (outR));
            }
            if (! rainSet) { for (auto& f : rainHp) f.set (1, sampleRate, 3500.0, 0.707); rainSet = true; }
        }
        float level() const { return lastLevel; }
    private:
        double sampleRate = 48000.0, thumpPhase = 0, sweep = 0;
        DBiquad windBp[2], whooshBp[1], rainHp[2];
        struct { float motion = 0, pan = 0, wet = 0; } env;
        Control target, lastControl; float pendingImpact = 0, pendingWhoosh = 0, lastImpact = 0;
        float thumpEnv = 0, crackEnv = 0, whooshEnv = 0, dropEnv = 0, thumpPan = 0, whooshPan = 0, lastLevel = 0;
        bool rainSet = false;
        juce::SpinLock lock; juce::Random rng { 17 };
    };
}
