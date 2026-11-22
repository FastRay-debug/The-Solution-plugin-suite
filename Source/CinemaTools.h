#pragma once
// AI Analyze - film & immersive tools:
//  - CueList: snapshots keyed to the timeline (scene changes); playback crossfades into each cue.
//  - MoodAnalyzer: psychoacoustic features -> valence / arousal (emotional resonance mapping).
//  - Trajectory: the object's 3D position over song time, for ADM / iXML metadata embedding.
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <map>
#include <vector>

namespace SolutionCinema
{
    // ---------------- cues ----------------
    struct Cue { double timeSec = 0; juce::String name; double fadeSec = 1.0; std::map<juce::String, float> values; };

    class CueList
    {
    public:
        std::vector<Cue> cues;                     // sorted by time (message thread)

        void add (Cue c) { remove (c.timeSec, 0.05); cues.push_back (std::move (c)); sort(); }
        void remove (double t, double tol = 0.0005) { cues.erase (std::remove_if (cues.begin(), cues.end(), [&] (const Cue& c) { return std::abs (c.timeSec - t) <= tol; }), cues.end()); }
        void sort() { std::sort (cues.begin(), cues.end(), [] (const Cue& a, const Cue& b) { return a.timeSec < b.timeSec; }); }
        int activeIndex (double t) const { int a = -1; for (int i = 0; i < (int) cues.size(); ++i) if (cues[(size_t) i].timeSec <= t + 1e-6) a = i; return a; }

        // Values to set at song time t. `from` holds where each control was when the active cue took over.
        // Returns the active cue index (-1 = before the first cue: nothing to do).
        int evaluate (double t, std::map<juce::String, float>& from, std::map<juce::String, float>& out, const std::map<juce::String, float>& current)
        {
            const int a = activeIndex (t);
            if (a != lastActive) { from = current; lastActive = a; }      // a new scene: crossfade from wherever we are
            out.clear();
            if (a < 0) return a;
            const auto& c = cues[(size_t) a];
            const double k = c.fadeSec <= 0.0 ? 1.0 : juce::jlimit (0.0, 1.0, (t - c.timeSec) / c.fadeSec);
            const double s = k * k * (3.0 - 2.0 * k);                     // smoothstep: no clicks at either end
            for (auto& [id, v] : c.values)
            {
                auto f = from.find (id); const float start = f != from.end() ? f->second : v;
                out[id] = (float) (start + (v - start) * s);
            }
            return a;
        }
        void resetPlayback() { lastActive = -2; }

        juce::String toString() const
        {
            juce::StringArray lines;
            for (auto& c : cues)
            {
                juce::StringArray kv; for (auto& [id, v] : c.values) kv.add (id + "=" + juce::String (v, 5));
                lines.add (juce::String (c.timeSec, 4) + "|" + juce::String (c.fadeSec, 3) + "|" + c.name.removeCharacters ("|\r\n") + "|" + kv.joinIntoString (","));
            }
            return lines.joinIntoString ("\n");
        }
        void fromString (const juce::String& s)
        {
            cues.clear();
            for (auto& line : juce::StringArray::fromLines (s))
            {
                auto f = juce::StringArray::fromTokens (line, "|", "");
                if (f.size() < 4) continue;
                Cue c; c.timeSec = f[0].getDoubleValue(); c.fadeSec = f[1].getDoubleValue(); c.name = f[2];
                for (auto& kv : juce::StringArray::fromTokens (f[3], ",", ""))
                    if (kv.containsChar ('=')) c.values[kv.upToFirstOccurrenceOf ("=", false, false)] = kv.fromFirstOccurrenceOf ("=", false, false).getFloatValue();
                cues.push_back (std::move (c));
            }
            sort(); resetPlayback();
        }
    private:
        int lastActive = -2;
    };

    inline juce::String smpte (double sec, double fps)
    {
        if (fps <= 0) fps = 25.0;
        const bool drop = std::abs (fps - 29.97) < 0.01;
        long long frames = (long long) std::floor (sec * fps + 1e-6);
        const int nominal = (int) std::round (fps);
        if (drop) { const long long d = frames / 17982, m = frames % 17982; frames += 18 * d + (m > 1 ? 2 * ((m - 2) / 1798) : 0); }
        const int ff = (int) (frames % nominal), ss = (int) ((frames / nominal) % 60), mm = (int) ((frames / (nominal * 60)) % 60), hh = (int) (frames / (nominal * 3600));
        return juce::String::formatted ("%02d:%02d:%02d%c%02d", hh, mm, ss, drop ? ';' : ':', ff);
    }

    // ---------------- mood ----------------
    // Valence (sad/tense <-> happy/warm) from key mode and brightness; arousal (calm <-> energetic) from
    // loudness, onset rate and brightness. Averaged over a few seconds so it follows scenes, not notes.
    class MoodAnalyzer
    {
    public:
        static constexpr int order = 12, N = 1 << order;
        MoodAnalyzer() : fft (order), window ((size_t) N, juce::dsp::WindowingFunction<float>::hann, false) { reset(); }
        void reset() { chroma.fill (0.0); prevMag.assign (N / 2, 0.0f); valence = 0; arousal = 0.3; fill = 0; loudDb = -60; centroid = 1000; onsetRate = 0; flux = 0; fluxAvg = 0; sinceOnset = 1; }
        void push (const float* mono, int n, double fs)
        {
            for (int i = 0; i < n; ++i)
            {
                buf[(size_t) fill++] = mono[i];
                if (fill == N) { analyse (fs); std::copy (buf.begin() + N / 2, buf.end(), buf.begin()); fill = N / 2; }
            }
        }
        double valence = 0, arousal = 0.3;          // valence -1..1, arousal 0..1
        double loudDb = -60, centroid = 1000, onsetRate = 0, majorness = 0;
        juce::String label() const
        {
            if (loudDb < -55) return "Silence";
            if (arousal > 0.55) return valence >= 0 ? "Epic / joyful" : "Tense / aggressive";
            return valence >= 0 ? "Warm / calm" : "Sad / intimate";
        }
    private:
        juce::dsp::FFT fft;
        juce::dsp::WindowingFunction<float> window;
        std::array<float, N> buf {};
        std::array<float, 2 * N> work {};
        std::array<double, 12> chroma {};
        std::vector<float> prevMag;
        int fill = 0;
        double flux = 0, fluxAvg = 0, sinceOnset = 1;

        void analyse (double fs)
        {
            std::copy (buf.begin(), buf.end(), work.begin()); std::fill (work.begin() + N, work.end(), 0.0f);
            double e = 0; for (int i = 0; i < N; ++i) e += buf[(size_t) i] * buf[(size_t) i];
            const double hopSec = (N / 2) / fs, a = 1.0 - std::exp (-hopSec / 3.0);        // ~3 s memory
            loudDb += (10.0 * std::log10 (e / N + 1e-12) - loudDb) * a;
            window.multiplyWithWindowingTable (work.data(), (size_t) N);
            fft.performFrequencyOnlyForwardTransform (work.data(), true);
            double num = 0, den = 0, fl = 0;
            std::array<double, 12> c {};
            for (int k = 1; k < N / 2; ++k)
            {
                const double f = k * fs / N, m = work[(size_t) k];
                num += f * m; den += m;
                fl += std::max (0.0, m - (double) prevMag[(size_t) k]); prevMag[(size_t) k] = (float) m;
                if (f > 60.0 && f < 5000.0) { const int pc = ((int) std::lround (12.0 * std::log2 (f / 440.0)) % 12 + 12 + 9) % 12; c[(size_t) pc] += m * m; }   // 0 = C
            }
            if (den > 1e-9) centroid += (num / den - centroid) * a;
            for (int i = 0; i < 12; ++i) chroma[(size_t) i] += (c[(size_t) i] - chroma[(size_t) i]) * a;
            // onsets: spectral flux peaks well above its running average
            fluxAvg += (fl - fluxAvg) * 0.1; sinceOnset += hopSec;
            const bool onset = fl > fluxAvg * 1.8 && sinceOnset > 0.08 && e / N > 1e-7;
            if (onset) sinceOnset = 0;
            onsetRate += ((onset ? 1.0 / hopSec : 0.0) - onsetRate) * a;
            // key mode: best major vs best minor Krumhansl-Kessler profile correlation
            static const double maj[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
            static const double mnr[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
            auto corr = [this] (const double* prof, int key) {
                double mx = 0, my = 0; for (int i = 0; i < 12; ++i) { mx += chroma[(size_t) i]; my += prof[i]; } mx /= 12; my /= 12;
                double sxy = 0, sxx = 0, syy = 0;
                for (int i = 0; i < 12; ++i) { const double x = chroma[(size_t) ((i + key) % 12)] - mx, y = prof[i] - my; sxy += x * y; sxx += x * x; syy += y * y; }
                return sxx > 0 ? sxy / std::sqrt (sxx * syy) : 0.0; };
            double bMaj = -1, bMin = -1; for (int k = 0; k < 12; ++k) { bMaj = std::max (bMaj, corr (maj, k)); bMin = std::max (bMin, corr (mnr, k)); }
            majorness = juce::jlimit (-1.0, 1.0, (bMaj - bMin) * 4.0);
            const double bright = juce::jlimit (0.0, 1.0, std::log2 (std::max (1.0, centroid) / 500.0) / 3.0);
            const double loud = juce::jlimit (0.0, 1.0, (loudDb + 45.0) / 35.0), rate = juce::jlimit (0.0, 1.0, onsetRate / 6.0);
            arousal = 0.45 * loud + 0.35 * rate + 0.20 * bright;
            valence = juce::jlimit (-1.0, 1.0, 0.75 * majorness + 0.25 * (bright * 2.0 - 1.0));
        }
    };

    // ---------------- trajectory (for metadata) ----------------
    struct Pos { float az = 0, el = 0, dist = 2, width = 1; };
    class Trajectory
    {
    public:
        static constexpr double rate = 5.0;          // points per second of song time
        void record (double t, Pos p) { if (t >= 0) points[(long long) std::llround (t * rate)] = p; }
        void clear() { points.clear(); }
        bool empty() const { return points.empty(); }
        const std::map<long long, Pos>& get() const { return points; }
        juce::String toString() const
        {
            juce::String s; s.preallocateBytes (points.size() * 28);
            for (auto& [k, p] : points) s << k << ":" << juce::String (p.az, 1) << ":" << juce::String (p.el, 1) << ":" << juce::String (p.dist, 2) << ":" << juce::String (p.width, 2) << ";";
            return s;
        }
        void fromString (const juce::String& s)
        {
            points.clear();
            for (auto& e : juce::StringArray::fromTokens (s, ";", ""))
            {
                auto f = juce::StringArray::fromTokens (e, ":", "");
                if (f.size() == 5) points[f[0].getLargeIntValue()] = { f[1].getFloatValue(), f[2].getFloatValue(), f[3].getFloatValue(), f[4].getFloatValue() };
            }
        }
    private:
        std::map<long long, Pos> points;
    };
}
