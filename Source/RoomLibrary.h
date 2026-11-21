#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>
#include <vector>

// Listening Room library (a monitoring tool, like checking your mix in other places).
// Each room = speaker/device response + early reflections from the room's shape (first-order image
// sources) + a late reverb tail (4-line feedback delay network). Names describe generic spaces;
// they are not measurements of any specific commercial studio.
namespace SolutionDSP
{
    struct EqPoint { int type; float freq, gainDb, q; };     // type: 0 peak, 1 low shelf, 2 high shelf, 3 high-pass, 4 low-pass

    struct ListeningRoom
    {
        const char* name;
        const char* category;
        bool mono;
        std::vector<EqPoint> eq;
        float width, length, height;     // metres (0 = no room: the device is at your ears)
        float absorption;                // 0 = mirror walls .. 1 = dead
        float rt60;                      // seconds
        float tailDb;                    // reverb level vs direct sound
    };

    inline const std::vector<ListeningRoom>& listeningRooms()
    {
        static const std::vector<ListeningRoom> rooms {
            // ---- Studios
            { "Modern Control Room (nearfields)", "Studios", false, { { 3, 45, 0, 0.7f } },                                   5.0f, 6.0f, 3.0f, 0.80f, 0.25f, -26.0f },
            { "Big-Mains Control Room",           "Studios", false, { { 3, 25, 0, 0.7f }, { 1, 60, 3.0f, 0.7f } },           7.0f, 9.0f, 3.5f, 0.75f, 0.35f, -23.0f },
            { "Mastering Suite (full range)",     "Studios", false, { { 3, 18, 0, 0.7f } },                                   6.0f, 8.0f, 3.5f, 0.85f, 0.20f, -30.0f },
            { "Classic Wood Tracking Room",       "Studios", false, { { 3, 35, 0, 0.7f }, { 2, 8000, -2.0f, 0.7f }, { 0, 250, 1.5f, 0.8f } }, 10.0f, 14.0f, 5.0f, 0.45f, 0.90f, -16.0f },
            { "Stone Live Room",                  "Studios", false, { { 3, 35, 0, 0.7f }, { 2, 6000, 1.5f, 0.7f } },         12.0f, 16.0f, 6.0f, 0.30f, 1.60f, -12.0f },
            { "Vocal Booth",                      "Studios", false, { { 3, 60, 0, 0.7f }, { 2, 9000, -1.5f, 0.7f } },        2.0f, 2.2f, 2.4f, 0.92f, 0.12f, -32.0f },
            { "Producer Desk (small monitors)",   "Studios", false, { { 3, 70, 0, 0.8f }, { 0, 1200, 1.0f, 1.0f } },         3.5f, 4.5f, 2.6f, 0.50f, 0.30f, -24.0f },
            { "Bedroom Studio (untreated)",       "Studios", false, { { 3, 50, 0, 0.7f }, { 0, 85, 4.0f, 4.0f }, { 0, 120, 6.0f, 4.0f }, { 0, 200, -5.0f, 3.0f } }, 3.5f, 4.0f, 2.5f, 0.25f, 0.50f, -18.0f },
            // ---- Venues
            { "Jazz Club",                        "Venues",  false, { { 3, 40, 0, 0.7f }, { 0, 2500, 2.0f, 1.0f } },         10.0f, 15.0f, 4.0f, 0.40f, 1.00f, -14.0f },
            { "Small Bar PA",                     "Venues",  false, { { 3, 60, 0, 0.8f }, { 0, 2500, 3.0f, 1.2f }, { 2, 10000, -3.0f, 0.7f } }, 8.0f, 12.0f, 3.5f, 0.35f, 0.80f, -15.0f },
            { "Concert Hall",                     "Venues",  false, { { 3, 30, 0, 0.7f } },                                   25.0f, 40.0f, 15.0f, 0.35f, 2.20f, -10.0f },
            { "Arena / Stadium PA",               "Venues",  false, { { 3, 35, 0, 0.7f }, { 1, 70, 4.0f, 0.7f }, { 2, 8000, -3.0f, 0.7f } }, 60.0f, 90.0f, 25.0f, 0.50f, 3.50f, -8.0f },
            { "Church / Cathedral",               "Venues",  false, { { 3, 40, 0, 0.7f }, { 2, 6000, -2.0f, 0.7f } },        20.0f, 50.0f, 20.0f, 0.20f, 4.50f, -6.0f },
            // ---- Everyday listening
            { "Phone Speaker",                    "Devices", true,  { { 3, 450, 0, 0.8f }, { 4, 10000, 0, 0.7f }, { 0, 1800, 5.0f, 1.5f }, { 0, 3500, 3.0f, 2.0f } }, 0, 0, 0, 1.0f, 0.0f, -99.0f },
            { "Earbuds (consumer)",               "Devices", false, { { 3, 25, 0, 0.7f }, { 1, 100, 3.0f, 0.7f }, { 0, 3000, 5.0f, 1.0f }, { 2, 10000, -4.0f, 0.7f } }, 0, 0, 0, 1.0f, 0.0f, -99.0f },
            { "Laptop Speakers",                  "Devices", false, { { 3, 250, 0, 0.8f }, { 4, 13000, 0, 0.7f }, { 0, 900, 2.0f, 1.0f }, { 0, 4000, 4.0f, 1.5f } }, 0.6f, 0.6f, 0.6f, 0.6f, 0.05f, -30.0f },
            { "Bluetooth Speaker",                "Devices", true,  { { 3, 80, 0, 1.2f }, { 0, 100, 6.0f, 1.5f }, { 4, 12000, 0, 0.7f }, { 0, 1000, -2.0f, 1.0f } }, 4.0f, 5.0f, 2.6f, 0.40f, 0.40f, -22.0f },
            { "TV Soundbar (living room)",        "Devices", false, { { 3, 90, 0, 0.8f }, { 1, 150, 2.0f, 0.7f }, { 0, 2500, 2.0f, 1.0f } }, 5.0f, 6.0f, 2.7f, 0.40f, 0.45f, -20.0f },
            { "Living Room Hi-Fi",                "Devices", false, { { 3, 35, 0, 0.7f }, { 1, 60, 2.0f, 0.7f } },           5.0f, 7.0f, 2.6f, 0.40f, 0.50f, -19.0f },
            { "Kitchen Radio",                    "Devices", true,  { { 3, 180, 0, 0.8f }, { 4, 6000, 0, 0.7f }, { 0, 1000, 4.0f, 1.0f } }, 3.0f, 4.0f, 2.5f, 0.20f, 0.45f, -18.0f },
            // ---- Cars
            { "Car - Sedan",                      "Cars",    false, { { 3, 30, 0, 0.7f }, { 0, 60, 6.0f, 1.0f }, { 0, 300, -3.0f, 1.0f }, { 2, 6000, 1.0f, 0.7f } }, 1.5f, 2.5f, 1.2f, 0.60f, 0.08f, -26.0f },
            { "Car - SUV with Subwoofer",         "Cars",    false, { { 3, 25, 0, 0.7f }, { 0, 45, 10.0f, 0.8f }, { 0, 250, -3.0f, 1.0f }, { 2, 8000, 2.0f, 0.7f } }, 1.7f, 3.0f, 1.4f, 0.55f, 0.10f, -24.0f },
            // ---- Checks
            { "Mono Check",                       "Checks",  true,  {},                                                       0, 0, 0, 1.0f, 0.0f, -99.0f },
            { "Headphones (flat reference)",      "Checks",  false, {},                                                       0, 0, 0, 1.0f, 0.0f, -99.0f },
        };
        return rooms;
    }

    class ListeningRoomSim
    {
    public:
        static constexpr int maxEq = 8, numTaps = 12, fdnLines = 4;

        void prepare (double sampleRate)
        {
            fs = sampleRate;
            const int maxDelay = (int) (fs * 1.0) + 2;
            for (auto& d : speakerDelay) d.assign ((size_t) maxDelay, 0.0f);
            for (auto& d : fdn) d.assign ((size_t) maxDelay, 0.0f);
            fadeStep = 1.0f / (float) (0.010 * fs);
            reset();
            current = -1; pending = -1;
        }

        void reset()
        {
            for (auto& d : speakerDelay) std::fill (d.begin(), d.end(), 0.0f);
            for (auto& d : fdn) std::fill (d.begin(), d.end(), 0.0f);
            for (auto& b : eqState) b = {};
            for (auto& z : erLp) z = 0.0f;
            for (auto& z : fdnLp) z = 0.0f;
            writePos = 0;
        }

        // roomIndex: -1 = off, else index into listeningRooms().
        void process (float* l, float* r, int n, int roomIndex)
        {
            pending = roomIndex;            // the latest choice wins, even mid-fade
            for (int i = 0; i < n; ++i)
            {
                // Click-free room changes: fade out, switch, fade in (10 ms each).
                if (pending != current)
                {
                    fade -= fadeStep;
                    if (fade <= 0.0f) { fade = 0.0f; current = pending; configure(); }
                }
                else if (fade < 1.0f) fade = std::min (1.0f, fade + fadeStep);

                if (current < 0) { l[i] *= fade; r[i] *= fade; continue; }

                float x[2] = { l[i], r[i] };
                if (room->mono) x[0] = x[1] = 0.5f * (x[0] + x[1]);
                for (int c = 0; c < 2; ++c)                         // speaker / device response
                    for (int k = 0; k < eqCount; ++k)
                        x[c] = runEq (k, c, x[c]);

                float out[2] = { x[0], x[1] };
                if (hasRoom)
                {
                    const int len = (int) speakerDelay[0].size();
                    speakerDelay[0][(size_t) writePos] = x[0];
                    speakerDelay[1][(size_t) writePos] = x[1];
                    // Early reflections (image sources), darkened by the walls.
                    float er[2] = { 0.0f, 0.0f };
                    for (int t = 0; t < tapCount; ++t)
                    {
                        const auto& tp = taps[(size_t) t];
                        const float v = speakerDelay[(size_t) tp.speaker][(size_t) ((writePos - tp.delay + len) % len)];
                        er[0] += v * tp.gainL; er[1] += v * tp.gainR;
                    }
                    for (int c = 0; c < 2; ++c) { erLp[(size_t) c] += erLpCoeff * (er[c] - erLp[(size_t) c]); out[c] += erLp[(size_t) c]; }

                    // Late tail: 4-line FDN with Hadamard feedback and damping.
                    const float pre = speakerDelay[0][(size_t) ((writePos - preDelay + len) % len)] + speakerDelay[1][(size_t) ((writePos - preDelay + len) % len)];
                    float y[fdnLines];
                    for (int k = 0; k < fdnLines; ++k)
                    {
                        const auto& line = fdn[(size_t) k];
                        y[k] = line[(size_t) ((writePos - fdnLen[(size_t) k] + len) % len)];
                        fdnLp[(size_t) k] += fdnDamp * (y[k] - fdnLp[(size_t) k]);
                        y[k] = fdnLp[(size_t) k] * fdnGain[(size_t) k];
                    }
                    const float h0 = y[0] + y[1] + y[2] + y[3], h1 = y[0] - y[1] + y[2] - y[3], h2 = y[0] + y[1] - y[2] - y[3], h3 = y[0] - y[1] - y[2] + y[3];
                    const float fb[fdnLines] = { 0.5f * h0, 0.5f * h1, 0.5f * h2, 0.5f * h3 };
                    for (int k = 0; k < fdnLines; ++k) fdn[(size_t) k][(size_t) writePos] = 0.5f * pre + fb[k];
                    out[0] += tailGain * (y[0] + y[2]);
                    out[1] += tailGain * (y[1] + y[3]);
                    writePos = (writePos + 1) % len;
                }
                l[i] = out[0] * outGain * fade; r[i] = out[1] * outGain * fade;
            }
        }

        int getCurrent() const { return current; }

    private:
        struct Tap { int speaker; int delay; float gainL, gainR; };

        float runEq (int k, int c, float x)
        {
            const auto& q = eqCoeffs[(size_t) k]; auto& z = eqState[(size_t) (k * 2 + c)];
            const double y = q[0] * x + z[0];
            z[0] = q[1] * x - q[3] * y + z[1];
            z[1] = q[2] * x - q[4] * y;
            return (float) y;
        }

        static std::array<double, 5> design (double fs, const EqPoint& p)
        {
            const double f = std::min ((double) p.freq, 0.45 * fs), w = juce::MathConstants<double>::twoPi * f / fs, c = std::cos (w), sn = std::sin (w);
            const double q = p.q > 0 ? p.q : 0.7071, al = sn / (2 * q), A = std::pow (10.0, p.gainDb / 40.0);
            double b0, b1, b2, a0, a1, a2;
            switch (p.type)
            {
                case 1: { const double s2 = 2 * std::sqrt (A) * al;
                          b0 = A * ((A + 1) - (A - 1) * c + s2); b1 = 2 * A * ((A - 1) - (A + 1) * c); b2 = A * ((A + 1) - (A - 1) * c - s2);
                          a0 = (A + 1) + (A - 1) * c + s2; a1 = -2 * ((A - 1) + (A + 1) * c); a2 = (A + 1) + (A - 1) * c - s2; break; }
                case 2: { const double s2 = 2 * std::sqrt (A) * al;
                          b0 = A * ((A + 1) + (A - 1) * c + s2); b1 = -2 * A * ((A - 1) + (A + 1) * c); b2 = A * ((A + 1) + (A - 1) * c - s2);
                          a0 = (A + 1) - (A - 1) * c + s2; a1 = 2 * ((A - 1) - (A + 1) * c); a2 = (A + 1) - (A - 1) * c - s2; break; }
                case 3: b0 = (1 + c) / 2; b1 = -(1 + c); b2 = b0; a0 = 1 + al; a1 = -2 * c; a2 = 1 - al; break;
                case 4: b0 = (1 - c) / 2; b1 = 1 - c; b2 = b0; a0 = 1 + al; a1 = -2 * c; a2 = 1 - al; break;
                default: b0 = 1 + al * A; b1 = -2 * c; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * c; a2 = 1 - al / A; break;
            }
            return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
        }

        void configure()
        {
            for (auto& b : eqState) b = {};
            for (auto& z : erLp) z = 0.0f;
            for (auto& z : fdnLp) z = 0.0f;
            for (auto& d : fdn) std::fill (d.begin(), d.end(), 0.0f);
            if (current < 0 || current >= (int) listeningRooms().size()) { current = -1; room = nullptr; return; }
            room = &listeningRooms()[(size_t) current];

            eqCount = std::min ((int) room->eq.size(), maxEq);
            for (int k = 0; k < eqCount; ++k) eqCoeffs[(size_t) k] = design (fs, room->eq[(size_t) k]);

            hasRoom = room->length > 0.0f;
            tapCount = 0;
            outGain = 1.0f;
            if (! hasRoom) return;

            // Listener 40% into the room, speakers 1.2 m away (or closer in tiny rooms) at +/-30 degrees.
            constexpr double c = 343.0;
            const double W = room->width, L = room->length, H = room->height;
            const double lx = W / 2, ly = L * 0.4, lz = 1.2;
            const double dist = std::min (1.2, 0.35 * std::min (W, L));
            const double refl = std::sqrt (1.0 - room->absorption);
            for (int s = 0; s < 2; ++s)
            {
                const double ang = juce::degreesToRadians (s == 0 ? -30.0 : 30.0);
                const double sx = lx + dist * std::sin (ang), sy = ly + dist * std::cos (ang), sz = lz;
                const double direct = dist;
                const double images[6][3] = { { -sx, sy, sz }, { 2 * W - sx, sy, sz }, { sx, -sy, sz }, { sx, 2 * L - sy, sz }, { sx, sy, -sz }, { sx, sy, 2 * H - sz } };
                for (const auto& im : images)
                {
                    const double dx = im[0] - lx, dy = im[1] - ly, dz = im[2] - lz, r = std::sqrt (dx * dx + dy * dy + dz * dz);
                    const int delay = (int) std::round ((r - direct) / c * fs);
                    if (delay <= 0 || delay >= (int) speakerDelay[0].size() - 1) continue;
                    const double g = refl * direct / r;
                    const double pan = juce::jlimit (-1.0, 1.0, dx / (std::sqrt (dx * dx + dy * dy) + 1e-9));   // -1 left .. +1 right
                    const double th = (pan + 1.0) * juce::MathConstants<double>::pi / 4.0;
                    taps[(size_t) tapCount++] = { s, delay, (float) (g * std::cos (th)), (float) (g * std::sin (th)) };
                }
            }
            erLpCoeff = (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * (3000.0 + 12000.0 * (1.0 - room->absorption)) / fs));

            // Late tail scaled to the room's size, decaying 60 dB in rt60 seconds.
            const double size = std::cbrt (W * L * H) / 6.0;
            const double base[fdnLines] = { 0.0297, 0.0371, 0.0411, 0.0437 };
            for (int k = 0; k < fdnLines; ++k)
            {
                fdnLen[(size_t) k] = juce::jlimit (8, (int) fdn[0].size() - 2, (int) std::round (base[k] * std::max (0.3, size) * fs));
                fdnGain[(size_t) k] = (float) std::pow (10.0, -3.0 * fdnLen[(size_t) k] / (std::max (0.05, (double) room->rt60) * fs));
            }
            fdnDamp = (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * (2500.0 + 9000.0 * (1.0 - room->absorption)) / fs));
            preDelay = juce::jlimit (1, (int) speakerDelay[0].size() - 2, (int) std::round (std::min (W, L) / 343.0 * fs));
            tailGain = juce::Decibels::decibelsToGain (room->tailDb);
            outGain = 1.0f / (1.0f + 0.5f * (float) refl);      // keep the overall level close to the dry signal
        }

        double fs = 48000.0;
        const ListeningRoom* room = nullptr;
        int current = -1, pending = -1, eqCount = 0, tapCount = 0, writePos = 0, preDelay = 1;
        bool hasRoom = false;
        float fade = 1.0f, fadeStep = 0.01f, erLpCoeff = 0.5f, fdnDamp = 0.5f, tailGain = 0.0f, outGain = 1.0f;
        std::array<std::array<double, 5>, maxEq> eqCoeffs {};
        std::array<std::array<double, 2>, maxEq * 2> eqState {};
        std::array<Tap, numTaps> taps {};
        std::array<std::vector<float>, 2> speakerDelay;
        std::array<std::vector<float>, fdnLines> fdn;
        std::array<int, fdnLines> fdnLen {};
        std::array<float, fdnLines> fdnGain {}, fdnLp {};
        std::array<float, 2> erLp {};
    };
}
