#pragma once
// The Void: procedural acoustic modeller.
//  - Room = a floor plan you draw (polygon, metres) + ceiling height. Source and listener positions inside it.
//  - Early reflections: image sources off every wall up to 2nd order (+ floor and ceiling), with real
//    path delays, distance loss, wall absorption and direction (equal-power pan + head shadow).
//  - Late tail: 8-line feedback delay network sized from the room's mean free path, decay from Sabine's
//    formula (RT60 = 0.161 V / S·alpha).
//  - Warp: non-Euclidean space - path lengths are bent (d^(1+warp)), so reflections arrive in orders and
//    spacings no real room can produce. Scramble: phase-scrambling all-pass diffusion.
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <vector>

namespace SolutionDSP
{
    struct VoidTap { float delay = 0, gl = 0, gr = 0; int order = 0; float x = 0, y = 0; };

    struct VoidRoom
    {
        std::vector<juce::Point<float>> poly { { 0, 0 }, { 8, 0 }, { 8, 6 }, { 0, 6 } };   // metres
        float height = 3.0f;
        juce::Point<float> src { 2.5f, 3.0f }, lis { 5.5f, 3.0f };
        float absorb = 0.3f, warp = 0.0f, size = 1.0f;

        static juce::Point<float> mirror (juce::Point<float> p, juce::Point<float> a, juce::Point<float> b)
        {
            const auto d = b - a; const float len2 = d.x * d.x + d.y * d.y;
            if (len2 < 1e-9f) return p;
            const float t = ((p - a).x * d.x + (p - a).y * d.y) / len2;
            const auto foot = a + d * t; return foot * 2.0f - p;
        }
        float area() const { float s = 0; for (size_t i = 0; i < poly.size(); ++i) { const auto& a = poly[i]; const auto& b = poly[(i + 1) % poly.size()]; s += a.x * b.y - b.x * a.y; } return std::abs (s) * 0.5f * size * size; }
        float perimeter() const { float s = 0; for (size_t i = 0; i < poly.size(); ++i) s += poly[i].getDistanceFrom (poly[(i + 1) % poly.size()]); return s * size; }
        float volume() const { return area() * height * size; }
        float surface() const { return 2.0f * area() + perimeter() * height * size; }
        float rt60() const { return juce::jlimit (0.08f, 12.0f, 0.161f * volume() / std::max (0.5f, surface() * juce::jlimit (0.02f, 0.99f, absorb))); }
        float meanFreePath() const { return 4.0f * volume() / std::max (0.5f, surface()); }

        // Image-source taps (direct + up to 2nd order on the walls, 1st order floor/ceiling).
        std::vector<VoidTap> taps (int maxTaps = 64) const
        {
            std::vector<VoidTap> out;
            const auto S = src * size, L = lis * size;
            const size_t n = poly.size();
            auto wall = [&] (size_t i) { return std::make_pair (poly[i] * size, poly[(i + 1) % n] * size); };
            auto add = [&] (juce::Point<float> img, float extraZ, int order, float refl)
            {
                const float dx = img.x - L.x, dy = img.y - L.y, dist = std::sqrt (dx * dx + dy * dy + extraZ * extraZ) + 0.05f;
                const float bent = std::pow (dist, 1.0f + warp) / std::pow (1.0f, warp);          // non-Euclidean: path lengths bend
                const float delayMs = bent / 343.0f * 1000.0f, gain = refl / dist;
                // listener faces +y: azimuth from the direction of arrival
                const float az = std::atan2 (dx, dy), pan = std::sin (az);
                const float gl = gain * std::cos ((pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f), gr = gain * std::sin ((pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f);
                out.push_back ({ delayMs, gl, gr, order, img.x, img.y });
            };
            const float r1 = std::sqrt (1.0f - juce::jlimit (0.0f, 0.99f, absorb)), r2 = r1 * r1;
            add (S, 0, 0, 1.0f);
            for (size_t i = 0; i < n; ++i)
            {
                const auto [a, b] = wall (i); const auto i1 = mirror (S, a, b);
                add (i1, 0, 1, r1);
                for (size_t j = 0; j < n; ++j) if (j != i) { const auto [c, d] = wall (j); add (mirror (i1, c, d), 0, 2, r2); }
            }
            const float h = height * size;
            add (S, 2.0f * 1.2f, 1, r1);                  // floor (ears ~1.2 m up)
            add (S, 2.0f * (h - 1.2f), 1, r1);            // ceiling
            std::sort (out.begin(), out.end(), [] (const VoidTap& a, const VoidTap& b) { return std::abs (a.gl) + std::abs (a.gr) > std::abs (b.gl) + std::abs (b.gr); });
            if ((int) out.size() > maxTaps) out.resize ((size_t) maxTaps);
            return out;
        }

        juce::String toString() const
        {
            juce::StringArray pts; for (auto& p : poly) pts.add (juce::String (p.x, 2) + "," + juce::String (p.y, 2));
            return pts.joinIntoString (";") + "|" + juce::String (height, 2);
        }
        void fromString (const juce::String& s)
        {
            if (s.isEmpty()) return;
            std::vector<juce::Point<float>> pts;
            for (auto& t : juce::StringArray::fromTokens (s.upToFirstOccurrenceOf ("|", false, false), ";", ""))
                pts.push_back ({ t.upToFirstOccurrenceOf (",", false, false).getFloatValue(), t.fromFirstOccurrenceOf (",", false, false).getFloatValue() });
            if (pts.size() >= 3) poly = pts;
            if (s.containsChar ('|')) height = juce::jlimit (1.5f, 40.0f, s.fromFirstOccurrenceOf ("|", false, false).getFloatValue());
        }
    };

    class VoidCore
    {
    public:
        static constexpr int maxTaps = 64, numLines = 8;
        void prepare (double fs, int)
        {
            sampleRate = fs;
            const int maxDelay = (int) (fs * 1.2);
            line.assign (2, std::vector<float> ((size_t) maxDelay + 1, 0.0f));
            for (int i = 0; i < numLines; ++i) fdn[(size_t) i].assign ((size_t) (fs * 0.25) + 1, 0.0f);
            for (int c = 0; c < 2; ++c) for (int k = 0; k < 4; ++k) ap[(size_t) c][(size_t) k].assign ((size_t) (fs * 0.02) + 1, 0.0f);
            reset();
        }
        void reset()
        {
            for (auto& l : line) std::fill (l.begin(), l.end(), 0.0f);
            for (auto& f : fdn) std::fill (f.begin(), f.end(), 0.0f);
            for (auto& c : ap) for (auto& a : c) std::fill (a.begin(), a.end(), 0.0f);
            wp = 0; fdnPos.fill (0); apPos = {}; lp.fill (0.0f);
        }
        // Message thread: new geometry -> new taps and tail (swapped in without locking the audio thread).
        void setRoom (const VoidRoom& room)
        {
            auto t = room.taps (maxTaps);
            Snapshot s;
            s.n = (int) t.size();
            for (int i = 0; i < s.n; ++i) { s.d[(size_t) i] = juce::jlimit (0.0f, 1150.0f, t[(size_t) i].delay) * 0.001f * (float) sampleRate; s.gl[(size_t) i] = t[(size_t) i].gl; s.gr[(size_t) i] = t[(size_t) i].gr; }
            const float mfp = room.meanFreePath() * std::pow (std::max (0.1f, room.meanFreePath()), room.warp);
            static const float ratios[numLines] = { 1.0f, 1.13f, 1.27f, 1.41f, 1.53f, 1.69f, 1.83f, 1.97f };
            const float rt = room.rt60();
            for (int i = 0; i < numLines; ++i)
            {
                const float len = juce::jlimit (0.003f, 0.24f, mfp / 343.0f * ratios[i]);
                s.fdnLen[(size_t) i] = len * (float) sampleRate;
                s.fdnG[(size_t) i] = std::pow (10.0f, -3.0f * len / rt);            // -60 dB after RT60 seconds
            }
            s.tailStart = 0.0f; if (s.n > 0) { s.tailStart = s.d[0]; for (int i = 1; i < s.n; ++i) s.tailStart = std::min (s.tailStart, s.d[(size_t) i]); }
            s.damp = juce::jlimit (0.05f, 0.9f, 0.15f + room.absorb * 0.6f);
            { const juce::SpinLock::ScopedLockType sl (lock); pending = s; hasPending = true; }
        }

        void process (float* l, float* r, int n, float mix, float scramble)
        {
            { const juce::SpinLock::ScopedTryLockType sl (lock); if (sl.isLocked() && hasPending) { cur = pending; hasPending = false; } }
            if (mix <= 0.0f && scramble <= 0.0f) return;
            const int size = (int) line[0].size();
            static const int apLen[2][4] = { { 142, 379, 107, 277 }, { 151, 353, 113, 269 } };
            for (int i = 0; i < n; ++i)
            {
                const float inL = l[i], inR = r[i], m = 0.5f * (inL + inR);
                line[0][(size_t) wp] = inL; line[1][(size_t) wp] = inR;
                // early reflections (mono source in the room, each path panned)
                float el = 0, er = 0;
                for (int t = 0; t < cur.n; ++t)
                {
                    const float rp = (float) wp - cur.d[(size_t) t];
                    float pos = rp < 0 ? rp + size : rp; const int i0 = (int) pos; const float fr = pos - i0; const int i1 = (i0 + 1) % size;
                    const float s = 0.5f * ((line[0][(size_t) i0] + line[1][(size_t) i0]) * (1 - fr) + (line[0][(size_t) i1] + line[1][(size_t) i1]) * fr);
                    el += s * cur.gl[(size_t) t]; er += s * cur.gr[(size_t) t];
                }
                // late tail: FDN fed with the mono input (Householder feedback, damped)
                float outs[numLines], sum = 0;
                for (int k = 0; k < numLines; ++k)
                {
                    const int len = std::max (1, (int) cur.fdnLen[(size_t) k]), fsz = (int) fdn[(size_t) k].size();
                    const int rd = (fdnPos[(size_t) k] - len + fsz) % fsz;
                    outs[k] = fdn[(size_t) k][(size_t) rd]; sum += outs[k];
                }
                const float hh = sum * (2.0f / numLines);
                // the tail is fed from the moment the sound reaches the listener (never before the direct sound)
                const int tr0 = ((int) wp - (int) cur.tailStart + size) % size;
                const float tailIn = 0.5f * (line[0][(size_t) tr0] + line[1][(size_t) tr0]);
                float tl = 0, tr = 0;
                for (int k = 0; k < numLines; ++k)
                {
                    float v = (outs[k] - hh) * cur.fdnG[(size_t) k];
                    lp[(size_t) k] += (v - lp[(size_t) k]) * (1.0f - cur.damp); v = lp[(size_t) k];      // high frequencies die first
                    fdn[(size_t) k][(size_t) fdnPos[(size_t) k]] = v + tailIn * 0.35f;
                    fdnPos[(size_t) k] = (fdnPos[(size_t) k] + 1) % (int) fdn[(size_t) k].size();
                    (k & 1 ? tr : tl) += outs[k];
                }
                float wl = el + tl * 0.3f, wr = er + tr * 0.3f;
                // phase scrambling: 4 all-passes per side (decorrelates, smears phase, keeps the spectrum)
                if (scramble > 0.0f)
                {
                    float sl = wl * mix + inL * (1 - mix), sr = wr * mix + inR * (1 - mix);
                    for (int k = 0; k < 4; ++k)
                    {
                        auto step = [&] (int c, float x) { auto& a = ap[(size_t) c][(size_t) k]; const int L = apLen[c][k] * (int) std::max (1.0, sampleRate / 48000.0);
                            const int rd = (apPos[(size_t) c][(size_t) k] - L + (int) a.size()) % (int) a.size(); const float d = a[(size_t) rd], g = 0.6f;
                            const float v = x + g * d; a[(size_t) apPos[(size_t) c][(size_t) k]] = v; apPos[(size_t) c][(size_t) k] = (apPos[(size_t) c][(size_t) k] + 1) % (int) a.size(); return d - g * v; };
                        sl = step (0, sl); sr = step (1, sr);
                    }
                    l[i] = (wl * mix + inL * (1 - mix)) * (1 - scramble) + sl * scramble;
                    r[i] = (wr * mix + inR * (1 - mix)) * (1 - scramble) + sr * scramble;
                }
                else { l[i] = inL * (1 - mix) + wl * mix; r[i] = inR * (1 - mix) + wr * mix; }
                wp = (wp + 1) % size;
            }
        }
    private:
        struct Snapshot { int n = 0; std::array<float, maxTaps> d {}, gl {}, gr {}; std::array<float, numLines> fdnLen {}, fdnG {}; float tailStart = 0, damp = 0.3f; };
        Snapshot cur, pending; bool hasPending = false; juce::SpinLock lock;
        double sampleRate = 48000.0;
        std::vector<std::vector<float>> line;
        std::array<std::vector<float>, numLines> fdn;
        std::array<int, numLines> fdnPos {};
        std::array<float, numLines> lp {};
        std::array<std::array<std::vector<float>, 4>, 2> ap;
        std::array<std::array<int, 4>, 2> apPos {};
        int wp = 0;
    };
}
