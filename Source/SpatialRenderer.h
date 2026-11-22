#pragma once
// Multichannel output for The Solution (surround / Dolby Atmos beds / ambisonics).
// The modules process in stereo; this renders that stereo pair as two objects (left/right, spread by the
// stage width) at the 3D stage position onto whatever the track's output is:
//   5.1, 7.1, 7.1.4 (Atmos bed), 9.1.6, ... - energy-preserving 3D panning onto the real speaker directions
//   (+ optional LFE send through an 80 Hz low-pass), or AmbiX (ACN / SN3D) ambisonics up to 3rd order.
// It also folds a multichannel input down to stereo, so a plugin can sit on a surround / Atmos bed track.
#include <juce_audio_basics/juce_audio_basics.h>
#include "Dialogue.h"
#include <array>
#include <vector>

namespace SolutionDSP
{
    class SpatialRenderer
    {
    public:
        static constexpr int maxCh = 64;
        // Speaker direction (degrees, + azimuth = right, + elevation = up) for a channel type; false for LFE.
        static bool speakerDirection (juce::AudioChannelSet::ChannelType t, float& az, float& el)
        {
            using CT = juce::AudioChannelSet;
            el = 0;
            switch (t)
            {
                case CT::left: az = -30; return true;          case CT::right: az = 30; return true;
                case CT::centre: az = 0; return true;
                case CT::leftSurround: az = -110; return true;  case CT::rightSurround: az = 110; return true;
                case CT::leftSurroundSide: az = -90; return true; case CT::rightSurroundSide: az = 90; return true;
                case CT::leftSurroundRear: az = -150; return true; case CT::rightSurroundRear: az = 150; return true;
                case CT::centreSurround: az = 180; return true;
                case CT::wideLeft: az = -60; return true;      case CT::wideRight: az = 60; return true;
                case CT::leftCentre: az = -15; return true;    case CT::rightCentre: az = 15; return true;
                case CT::topFrontLeft: az = -45; el = 45; return true;  case CT::topFrontRight: az = 45; el = 45; return true;
                case CT::topFrontCentre: az = 0; el = 45; return true;
                case CT::topSideLeft: az = -90; el = 45; return true;   case CT::topSideRight: az = 90; el = 45; return true;
                case CT::topRearLeft: az = -135; el = 45; return true;  case CT::topRearRight: az = 135; el = 45; return true;
                case CT::topRearCentre: az = 180; el = 45; return true;
                case CT::topMiddle: az = 0; el = 90; return true;
                case CT::LFE: case CT::LFE2: return false;
                default: az = 0; return true;
            }
        }
        static juce::String layoutName (const juce::AudioChannelSet& s)
        {
            if (s.isDiscreteLayout()) return juce::String (s.size()) + " ch";
            if (const int order = s.getAmbisonicOrder(); order > 0) return "Ambisonics " + juce::String (order) + juce::String (order == 1 ? "st" : order == 2 ? "nd" : "rd") + " order";
            return s.getSpeakerArrangementAsString().replace (" ", "");
        }

        void prepare (const juce::AudioChannelSet& out, double fs)
        {
            layout = out; sampleRate = fs;
            ambiOrder = out.getAmbisonicOrder();
            n = std::min (maxCh, out.size());
            for (int i = 0; i < n; ++i)
            {
                float az = 0, el = 0;
                const auto t = out.getTypeOfChannel (i);
                isLfe[(size_t) i] = ! speakerDirection (t, az, el) && ambiOrder < 0;
                const float a = juce::degreesToRadians (az), e = juce::degreesToRadians (el);
                dir[(size_t) i] = { std::sin (a) * std::cos (e), std::sin (e), std::cos (a) * std::cos (e) };
            }
            for (auto& f : lfeLp) { f.set (0, fs, 80.0, 0.707); f.clear(); }
            curGains = {}; first = true;
        }
        int numChannels() const { return n; }
        bool isAmbisonic() const { return ambiOrder > 0; }

        // Fold any input layout down to stereo (L side -> L, R side -> R, centres to both at -3 dB, LFE dropped).
        static void downmix (const juce::AudioBuffer<float>& in, const juce::AudioChannelSet& inSet, juce::AudioBuffer<float>& st, int len)
        {
            st.clear (0, len);
            const int ch = in.getNumChannels();
            if (ch == 1) { st.copyFrom (0, 0, in, 0, 0, len); st.copyFrom (1, 0, in, 0, 0, len); return; }
            if (inSet.getAmbisonicOrder() > 0)                    // ambisonic in: W/Y decode to a simple M/S stereo
            {
                for (int i = 0; i < len; ++i) { const float w = in.getSample (0, i), y = ch > 1 ? in.getSample (1, i) : 0.0f; st.setSample (0, i, 0.707f * (w + y)); st.setSample (1, i, 0.707f * (w - y)); }
                return;
            }
            for (int c = 0; c < ch; ++c)
            {
                float az = 0, el = 0;
                if (! speakerDirection (inSet.getTypeOfChannel (c), az, el)) continue;   // LFE
                const float g = el > 0 ? 0.707f : 1.0f;
                if (std::abs (az) < 1.0f || std::abs (std::abs (az) - 180.0f) < 1.0f) { st.addFrom (0, 0, in, c, 0, len, 0.707f * g); st.addFrom (1, 0, in, c, 0, len, 0.707f * g); }
                else st.addFrom (az < 0 ? 0 : 1, 0, in, c, 0, len, g);
            }
        }

        // Gains for one point source (energy normalised). Spread 0 = focused, 1 = wide.
        void sourceGains (float azDeg, float elDeg, float spread, std::array<float, maxCh>& g) const
        {
            g.fill (0.0f);
            const float a = juce::degreesToRadians (azDeg), e = juce::degreesToRadians (juce::jlimit (-30.0f, 90.0f, elDeg));
            const float sx = std::sin (a) * std::cos (e), sy = std::sin (e), sz = std::cos (a) * std::cos (e);
            if (ambiOrder > 0) { encode (-azDeg, elDeg, g); return; }              // AmbiX: + azimuth = left
            const float p = 2.0f + 14.0f * (1.0f - juce::jlimit (0.0f, 1.0f, spread));   // sharper = more focused
            double sum = 0;
            for (int i = 0; i < n; ++i)
            {
                if (isLfe[(size_t) i]) continue;
                const auto& d = dir[(size_t) i];
                const float c = std::max (0.0f, sx * d[0] + sy * d[1] + sz * d[2]);
                g[(size_t) i] = std::pow (c, p); sum += (double) g[(size_t) i] * g[(size_t) i];
            }
            const float norm = sum > 1e-12 ? (float) (1.0 / std::sqrt (sum)) : 0.0f;
            for (int i = 0; i < n; ++i) g[(size_t) i] *= norm;
        }

        // Render the stereo pair (l, r) into `out` (all channels overwritten).
        void render (const float* l, const float* r, juce::AudioBuffer<float>& out, int len, float azDeg, float elDeg, float width, float lfeAmount)
        {
            const float half = 30.0f * juce::jlimit (0.0f, 2.0f, width);            // the stereo image's spread around the object
            std::array<float, maxCh> gl {}, gr {};
            sourceGains (azDeg - half, elDeg, std::min (1.0f, width * 0.5f), gl);
            sourceGains (azDeg + half, elDeg, std::min (1.0f, width * 0.5f), gr);
            if (ambiOrder <= 0)
            {   // pan law: where the two halves overlap, identical content would add coherently (+3 dB) - scale by the overlap
                double dot = 0; for (int c = 0; c < n; ++c) dot += (double) gl[(size_t) c] * gr[(size_t) c];
                const float k = (float) (1.0 / std::sqrt (1.0 + juce::jlimit (0.0, 1.0, dot)));
                for (int c = 0; c < n; ++c) { gl[(size_t) c] *= k; gr[(size_t) c] *= k; }
            }
            const int chans = std::min (n, out.getNumChannels());
            for (int c = 0; c < chans; ++c)
            {
                auto* o = out.getWritePointer (c);
                const float tl = gl[(size_t) c], tr = gr[(size_t) c];
                const float sl = first ? tl : curGains[(size_t) c][0], sr = first ? tr : curGains[(size_t) c][1];
                if (isLfe[(size_t) c])
                {
                    for (int i = 0; i < len; ++i) o[i] = (float) lfeLp[c & 1].run (0.5f * (l[i] + r[i])) * lfeAmount;
                    continue;
                }
                for (int i = 0; i < len; ++i)                               // glide the gains over the block (moving objects stay smooth)
                {
                    const float k = (float) i / (float) std::max (1, len);
                    o[i] = l[i] * (sl + (tl - sl) * k) + r[i] * (sr + (tr - sr) * k);
                }
                curGains[(size_t) c] = { tl, tr };
            }
            for (int c = chans; c < out.getNumChannels(); ++c) out.clear (c, 0, len);
            first = false;
            lastGl = gl; lastGr = gr;
        }
        float speakerLevel (int c) const { return juce::isPositiveAndBelow (c, n) ? std::sqrt (0.5f * (lastGl[(size_t) c] * lastGl[(size_t) c] + lastGr[(size_t) c] * lastGr[(size_t) c])) : 0.0f; }
        void speakerAngles (int c, float& az, float& el) const { const auto& d = dir[(size_t) c]; el = juce::radiansToDegrees (std::asin (juce::jlimit (-1.0f, 1.0f, d[1]))); az = juce::radiansToDegrees (std::atan2 (d[0], d[2])); }
        bool channelIsLfe (int c) const { return juce::isPositiveAndBelow (c, n) && isLfe[(size_t) c]; }

        // AmbiX (ACN order, SN3D normalisation) up to 3rd order. az: + = left (AmbiX convention).
        void encode (float azDeg, float elDeg, std::array<float, maxCh>& g) const
        {
            const double a = juce::degreesToRadians ((double) azDeg), e = juce::degreesToRadians ((double) elDeg);
            const double x = std::cos (a) * std::cos (e), y = std::sin (a) * std::cos (e), z = std::sin (e);
            const double sh[16] = {
                1.0,
                y, z, x,
                std::sqrt (3.0) * x * y, std::sqrt (3.0) * y * z, 0.5 * (3 * z * z - 1), std::sqrt (3.0) * x * z, std::sqrt (3.0) / 2 * (x * x - y * y),
                std::sqrt (5.0 / 8) * y * (3 * x * x - y * y), std::sqrt (15.0) * x * y * z, std::sqrt (3.0 / 8) * y * (5 * z * z - 1), 0.5 * z * (5 * z * z - 3),
                std::sqrt (3.0 / 8) * x * (5 * z * z - 1), std::sqrt (15.0) / 2 * z * (x * x - y * y), std::sqrt (5.0 / 8) * x * (x * x - 3 * y * y) };
            const int count = std::min (n, (ambiOrder + 1) * (ambiOrder + 1));
            for (int i = 0; i < count && i < 16; ++i) g[(size_t) i] = (float) sh[i];
        }

    private:
        juce::AudioChannelSet layout;
        double sampleRate = 48000.0;
        int n = 2, ambiOrder = -1;
        std::array<std::array<float, 3>, maxCh> dir {};
        std::array<bool, maxCh> isLfe {};
        std::array<std::array<float, 2>, maxCh> curGains {};
        std::array<float, maxCh> lastGl {}, lastGr {};
        DBiquad lfeLp[2];
        bool first = true;
    };
}
