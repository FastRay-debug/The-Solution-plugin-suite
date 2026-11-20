#pragma once

#include <juce_core/juce_core.h>
#include <cmath>
#include <utility>
#include <vector>

// Microphone compensation for room measurement. Any mic works; knowing (roughly) how it colours
// sound lets us remove that colouring, and knowing where it can't be trusted stops us
// "correcting" the mic instead of the room.
//
// Built-in curves are APPROXIMATIONS of typical manufacturer response charts (on-axis, ~30 cm),
// not individual calibrations. For accuracy, load the calibration file that ships with a
// measurement mic (REW / miniDSP style: "frequency dB" per line).
namespace SolutionDSP
{
    struct MicProfile
    {
        juce::String id, name;
        std::vector<std::pair<float, float>> response;   // (Hz, dB) - the mic's own colouration
        float reliableLowHz = 25.0f, reliableHighHz = 5000.0f;
        float maxCutDb = 12.0f;
        float smoothingOctaves = 1.0f / 6.0f;

        // Mic response at f (log-frequency interpolation, flat beyond the ends).
        float responseAt (float f) const
        {
            if (response.empty()) return 0.0f;
            if (f <= response.front().first) return response.front().second;
            if (f >= response.back().first) return response.back().second;
            for (size_t i = 1; i < response.size(); ++i)
                if (f <= response[i].first)
                {
                    const auto& [f0, d0] = response[i - 1]; const auto& [f1, d1] = response[i];
                    const float t = std::log (f / f0) / std::log (f1 / f0);
                    return d0 + t * (d1 - d0);
                }
            return 0.0f;
        }
    };

    inline const std::vector<MicProfile>& builtInMicProfiles()
    {
        static const std::vector<MicProfile> list {
            { "measurement", "Measurement mic (flat, no calibration file)", {}, 25.0f, 5000.0f, 12.0f, 1.0f / 6.0f },
            { "generic",     "Any / unknown mic (safe settings)",          {}, 60.0f, 4000.0f, 8.0f, 1.0f / 3.0f },
            { "laptop",      "Laptop built-in mic",
              { { 50, -20 }, { 100, -10 }, { 200, -3 }, { 300, 0 }, { 3000, 0 }, { 5000, 3 }, { 8000, 0 } }, 150.0f, 4000.0f, 6.0f, 1.0f / 3.0f },
            { "phone",       "Phone mic",
              { { 50, -12 }, { 100, -6 }, { 200, -1 }, { 400, 0 }, { 4000, 1 }, { 8000, -3 } }, 100.0f, 5000.0f, 8.0f, 1.0f / 3.0f },
            { "headset",     "Headset / gaming mic",
              { { 50, -15 }, { 100, -8 }, { 200, -2 }, { 400, 0 }, { 3000, 2 }, { 6000, -2 } }, 150.0f, 5000.0f, 6.0f, 1.0f / 3.0f },
            { "usb_condenser", "USB condenser (Blue Yeti / AT2020USB type)",
              { { 30, -5 }, { 50, -2 }, { 100, 0 }, { 2000, 0 }, { 5000, 2 }, { 8000, 3 }, { 12000, 1 } }, 40.0f, 5000.0f, 10.0f, 1.0f / 6.0f },
            { "sm58",        "Shure SM58 / dynamic vocal mic",
              { { 50, -10 }, { 100, -5 }, { 200, 0 }, { 1000, 0 }, { 3000, 3 }, { 5000, 5 }, { 8000, 2 }, { 10000, -2 } }, 70.0f, 5000.0f, 8.0f, 1.0f / 3.0f },
            { "sm57",        "Shure SM57 / dynamic instrument mic",
              { { 50, -8 }, { 100, -4 }, { 200, 0 }, { 2000, 0 }, { 4000, 3 }, { 6000, 5 }, { 10000, -3 } }, 60.0f, 5000.0f, 8.0f, 1.0f / 3.0f },
            { "at2020",      "Audio-Technica AT2020",
              { { 30, -3 }, { 50, -1 }, { 100, 0 }, { 3000, 0 }, { 6000, 2 }, { 10000, 2 } }, 30.0f, 5000.0f, 12.0f, 1.0f / 6.0f },
            { "nt1",         "Rode NT1 / NT1-A",
              { { 30, -1 }, { 100, 0 }, { 4000, 0 }, { 8000, 2 }, { 12000, 2 } }, 25.0f, 5000.0f, 12.0f, 1.0f / 6.0f },
        };
        return list;
    }

    inline MicProfile findMicProfile (const juce::String& id)
    {
        for (const auto& p : builtInMicProfiles())
            if (p.id == id) return p;
        return builtInMicProfiles()[1];   // unknown -> safe generic settings
    }

    // Parses REW / miniDSP / Dayton style calibration files: "freq dB [phase]" per line.
    // Header lines (quotes, '*', "Sens", text) are skipped. Returns an empty curve if unusable.
    inline std::vector<std::pair<float, float>> parseCalibrationFile (const juce::String& text)
    {
        std::vector<std::pair<float, float>> points;
        for (auto line : juce::StringArray::fromLines (text))
        {
            line = line.trim().replace (",", " ").replace (";", " ").replace ("\t", " ");
            if (line.isEmpty() || ! (juce::CharacterFunctions::isDigit (line[0]) || line[0] == '.'))
                continue;
            juce::StringArray tok; tok.addTokens (line, " ", {}); tok.removeEmptyStrings();
            if (tok.size() < 2) continue;
            const float f = tok[0].getFloatValue(), db = tok[1].getFloatValue();
            if (f > 1.0f && f < 100000.0f && std::abs (db) < 60.0f && (points.empty() || f > points.back().first))
                points.emplace_back (f, db);
        }
        if (points.size() < 5) points.clear();
        return points;
    }

    inline MicProfile makeCalibratedProfile (const juce::String& name, std::vector<std::pair<float, float>> points)
    {
        MicProfile p { "calfile", "Calibrated: " + name, std::move (points), 25.0f, 5000.0f, 12.0f, 1.0f / 6.0f };
        if (! p.response.empty())
        {
            p.reliableLowHz  = std::max (25.0f, p.response.front().first);
            p.reliableHighHz = std::min (5000.0f, p.response.back().first);
        }
        return p;
    }
}
