#include "MixAssistant.h"

namespace SolutionDSP
{
//==============================================================================
float MixProfile::widthDb (float lo, float hi) const
{
    double m = 0.0, s = 0.0;
    for (int b = 0; b < numBands; ++b)
        if (bandCentre (b) >= lo && bandCentre (b) <= hi) { m += std::pow (10.0, midDb[(size_t) b] / 10.0); s += std::pow (10.0, sideDb[(size_t) b] / 10.0); }
    return (float) (10.0 * std::log10 ((s + 1e-20) / (m + 1e-20)));
}

float MixProfile::tonalMean (float lo, float hi) const
{
    double sum = 0.0; int n = 0;
    for (int b = 0; b < numBands; ++b)
        if (bandCentre (b) >= lo && bandCentre (b) <= hi) { sum += midDb[(size_t) b]; ++n; }
    return n > 0 ? (float) (sum / n) : 0.0f;
}

//==============================================================================
MixAnalyser::MixAnalyser (double sampleRate) : fs (sampleRate)
{
    window.resize (fftSize);
    for (int i = 0; i < fftSize; ++i) window[(size_t) i] = 0.5f * (1.0f - std::cos (juce::MathConstants<float>::twoPi * i / (fftSize - 1)));
    midBuf.assign (fftSize, 0.0f); sideBuf.assign (fftSize, 0.0f); work.assign (2 * fftSize, 0.0f);
    loudness.prepare (fs);
}

void MixAnalyser::addStereo (const float* l, const float* r, int numSamples)
{
    loudness.process (l, r, numSamples);
    for (int i = 0; i < numSamples; ++i)
    {
        midBuf[(size_t) fill]  = 0.5f * (l[i] + r[i]);
        sideBuf[(size_t) fill] = 0.5f * (l[i] - r[i]);
        if (++fill == fftSize)
        {
            analyseFrame();
            std::copy (midBuf.begin() + hop, midBuf.end(), midBuf.begin());
            std::copy (sideBuf.begin() + hop, sideBuf.end(), sideBuf.begin());
            fill = fftSize - hop;
        }
    }
    totalSamples += numSamples;
}

void MixAnalyser::analyseFrame()
{
    auto accumulate = [&] (const std::vector<float>& src, std::array<double, MixProfile::numBands>& pow)
    {
        std::fill (work.begin(), work.end(), 0.0f);
        for (int i = 0; i < fftSize; ++i) work[(size_t) i] = src[(size_t) i] * window[(size_t) i];
        fft.performFrequencyOnlyForwardTransform (work.data(), true);
        for (int k = 1; k < fftSize / 2; ++k)
        {
            const double f = k * fs / fftSize;
            const int b = (int) std::lround (3.0 * std::log2 (f / 1000.0)) + 17;
            if (b >= 0 && b < MixProfile::numBands)
                pow[(size_t) b] += (double) work[(size_t) k] * work[(size_t) k];
        }
    };
    accumulate (midBuf, midPow);
    accumulate (sideBuf, sidePow);
    ++frames;
}

MixProfile MixAnalyser::finish (const juce::String& name) const
{
    MixProfile p;
    p.name = name;
    p.seconds = totalSamples / fs;
    if (frames == 0) return p;
    for (int b = 0; b < MixProfile::numBands; ++b)
    {
        p.midDb[(size_t) b]  = (float) (10.0 * std::log10 (midPow[(size_t) b] / frames + 1e-20));
        p.sideDb[(size_t) b] = (float) (10.0 * std::log10 (sidePow[(size_t) b] / frames + 1e-20));
    }
    p.lufs = loudness.integrated.load();
    p.truePeakDb = loudness.truePeakDb.load();
    p.valid = p.lufs > -69.0f;
    return p;
}

//==============================================================================
MixProfile analyseReferenceFile (const juce::File& file, juce::String& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr) { error = "Couldn't read " + file.getFileName() + " (supported: WAV, AIFF, FLAC, OGG, MP3)"; return {}; }

    MixAnalyser analyser (reader->sampleRate);
    const auto maxSamples = (juce::int64) std::min<double> ((double) reader->lengthInSamples, reader->sampleRate * 60.0 * 15.0);
    juce::AudioBuffer<float> chunk (2, 65536);
    for (juce::int64 pos = 0; pos < maxSamples; pos += chunk.getNumSamples())
    {
        const int n = (int) std::min<juce::int64> (chunk.getNumSamples(), maxSamples - pos);
        reader->read (&chunk, 0, n, pos, true, true);              // mono files are copied to both channels
        analyser.addStereo (chunk.getReadPointer (0), chunk.getReadPointer (1), n);
    }
    auto profile = analyser.finish (file.getFileNameWithoutExtension());
    if (! profile.valid) error = file.getFileName() + " seems to be silent";
    return profile;
}

//==============================================================================
juce::StringArray genreTargetNames() { return { "Hip-Hop / Trap", "Pop", "Rock", "EDM", "R&B / Soul" }; }

MixProfile genreTarget (const juce::String& genre)
{
    // Approximate long-term 1/3-octave balance of commercial releases (relative dB).
    const std::vector<std::pair<float, float>> base { { 20, -8 }, { 40, 0 }, { 60, 2 }, { 100, 2 }, { 200, 0 }, { 500, -4 }, { 1000, -7 },
                                                      { 2000, -10 }, { 4000, -13 }, { 8000, -17 }, { 12000, -21 }, { 16000, -26 }, { 20000, -34 } };
    auto interp = [] (const std::vector<std::pair<float, float>>& pts, float f)
    {
        if (f <= pts.front().first) return pts.front().second;
        for (size_t i = 1; i < pts.size(); ++i)
            if (f <= pts[i].first)
            {
                const float t = std::log (f / pts[i - 1].first) / std::log (pts[i].first / pts[i - 1].first);
                return pts[i - 1].second + t * (pts[i].second - pts[i - 1].second);
            }
        return pts.back().second;
    };
    auto bump = [] (float f, float centre, float gain, float octaves) { const float d = std::log2 (f / centre) / octaves; return gain * std::exp (-d * d); };

    float lufs = -9.0f, plr = 9.0f, side = -7.0f;
    std::function<float (float)> tweak = [] (float) { return 0.0f; };
    if (genre.startsWith ("Hip"))  { lufs = -8.0f;  plr = 8.0f;  side = -9.0f; tweak = [&] (float f) { return bump (f, 55, 4.0f, 0.8f) - bump (f, 2500, 1.0f, 1.0f); }; }
    else if (genre == "Rock")      { lufs = -9.0f;  plr = 9.0f;  side = -6.0f; tweak = [&] (float f) { return -bump (f, 40, 2.0f, 0.6f) + bump (f, 2000, 2.0f, 1.0f); }; }
    else if (genre == "EDM")       { lufs = -7.0f;  plr = 7.0f;  side = -5.0f; tweak = [&] (float f) { return bump (f, 60, 3.0f, 0.8f) + bump (f, 10000, 2.0f, 0.8f); }; }
    else if (genre.startsWith ("R&B")) { lufs = -10.0f; plr = 10.0f; side = -7.0f; tweak = [&] (float f) { return bump (f, 70, 3.0f, 0.8f) + bump (f, 10000, 1.0f, 0.8f); }; }

    MixProfile p;
    p.name = genre + " target";
    for (int b = 0; b < MixProfile::numBands; ++b)
    {
        const float f = MixProfile::bandCentre (b);
        p.midDb[(size_t) b] = interp (base, f) + tweak (f);
        p.sideDb[(size_t) b] = p.midDb[(size_t) b] + side - (f < 120.0f ? 12.0f : 0.0f);   // mono-ish lows
    }
    p.lufs = lufs; p.truePeakDb = lufs + plr; p.valid = true; p.seconds = 0.0;
    return p;
}

//==============================================================================
MixComparison compareMixes (const MixProfile& mix, const MixProfile& ref, const CurrentSettings& now)
{
    MixComparison c;
    // Loudness-match the two spectra on the 200 Hz - 4 kHz region before comparing tone.
    const float offset = ref.tonalMean (200.0f, 4000.0f) - mix.tonalMean (200.0f, 4000.0f);
    for (int b = 0; b < MixProfile::numBands; ++b)
        c.diffDb[(size_t) b] = ref.midDb[(size_t) b] - mix.midDb[(size_t) b] - offset;

    auto meanDiff = [&] (float lo, float hi)
    {
        double s = 0.0; int n = 0;
        for (int b = 0; b < MixProfile::numBands; ++b)
            if (MixProfile::bandCentre (b) >= lo && MixProfile::bandCentre (b) <= hi) { s += c.diffDb[(size_t) b]; ++n; }
        return n ? (float) (s / n) : 0.0f;
    };
    auto fmt = [] (float v) { return juce::String (std::abs (v), 1); };

    // Air / top end -> Elevation drives this module's air shelf (+/-4.5 dB at +/-90 deg).
    const float air = meanDiff (6300.0f, 16000.0f);
    if (std::abs (air) > 1.0f)
    {
        if (now.hasAirControl)
            c.suggestions.push_back ({ (air > 0 ? "Top end is " + fmt (air) + " dB duller than the reference - add air"
                                                : "Top end is " + fmt (air) + " dB brighter than the reference - soften the air"),
                                       "stage_elevation", juce::jlimit (-90.0f, 90.0f, now.elevation + air / 4.5f * 90.0f) });
        else
            c.suggestions.push_back ({ "Top end differs by " + juce::String (air, 1) + " dB from the reference - The Loft or The Aether can add or soften air", {}, 0.0f });
    }

    // Stereo width.
    c.widthDiff = ref.widthDb() - mix.widthDb();
    if (std::abs (c.widthDiff) > 1.5f)
        c.suggestions.push_back ({ c.widthDiff > 0 ? "Mix is " + fmt (c.widthDiff) + " dB narrower than the reference - widen"
                                                   : "Mix is " + fmt (c.widthDiff) + " dB wider than the reference - narrow it (also safer in mono)",
                                   "stage_width", juce::jlimit (0.0f, 2.0f, now.width * std::pow (10.0f, c.widthDiff / 20.0f)) });

    // Density: peak-to-loudness ratio.
    c.plrDiff = mix.plr() - ref.plr();
    if (c.plrDiff > 2.0f)
        c.suggestions.push_back ({ "Mix is " + fmt (c.plrDiff) + " dB more dynamic (peak-to-loudness) than the reference - add Glue",
                                   "glue_amount", juce::jlimit (0.0f, 1.0f, now.glue + c.plrDiff / 10.0f) });
    else if (c.plrDiff < -2.0f)
        c.suggestions.push_back ({ "Mix is " + fmt (c.plrDiff) + " dB more squashed than the reference - ease off limiting / Glue", {}, 0.0f });

    // Advice only (no single knob in this module for these).
    const float lows = meanDiff (30.0f, 120.0f), mud = meanDiff (200.0f, 500.0f), presence = meanDiff (2000.0f, 5000.0f);
    if (lows > 2.0f)       c.suggestions.push_back ({ "Low end is " + fmt (lows) + " dB lighter than the reference - bring up kick/bass or add The Bunker", {}, 0.0f });
    if (lows < -2.0f)      c.suggestions.push_back ({ "Low end is " + fmt (lows) + " dB heavier than the reference - pull back kick/bass or raise Anchor Hz on The Bunker", {}, 0.0f });
    if (mud < -2.0f)       c.suggestions.push_back ({ "There's " + fmt (mud) + " dB more 200-500 Hz than the reference (muddy) - try a wide cut there", {}, 0.0f });
    if (presence > 2.0f)   c.suggestions.push_back ({ "2-5 kHz is " + fmt (presence) + " dB lower than the reference - vocals/leads may sound distant", {}, 0.0f });
    if (presence < -2.0f)  c.suggestions.push_back ({ "2-5 kHz is " + fmt (presence) + " dB hotter than the reference - may sound harsh", {}, 0.0f });

    // Loudness.
    c.lufsDiff = ref.lufs - mix.lufs;
    if (std::abs (c.lufsDiff) > 1.0f)
        c.suggestions.push_back ({ c.lufsDiff > 0 ? "Reference is " + fmt (c.lufsDiff) + " LU louder - push your master limiter (streaming services turn loud masters down to about -14 LUFS)"
                                                  : "Your mix is " + fmt (c.lufsDiff) + " LU louder than the reference", {}, 0.0f });

    if (c.suggestions.empty())
        c.suggestions.push_back ({ "Very close to the reference - nothing major to change", {}, 0.0f });
    return c;
}
}
