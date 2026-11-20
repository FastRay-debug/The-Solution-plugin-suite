#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include "Metering.h"
#include <array>
#include <vector>

// Mix Assistant: compares your mix (what this plugin outputs) with a reference track or a genre
// target - tonal balance in 31 third-octave bands, loudness, peak-to-loudness ratio and stereo
// width - and turns the differences into settings this module can apply plus plain advice.
namespace SolutionDSP
{
    struct MixProfile
    {
        static constexpr int numBands = 31;                  // ISO 1/3-octave, 20 Hz - 20 kHz
        static float bandCentre (int b) { return 1000.0f * std::pow (2.0f, (float) (b - 17) / 3.0f); }

        std::array<float, numBands> midDb {}, sideDb {};      // band power, dB
        float lufs = -70.0f, truePeakDb = -70.0f;
        double seconds = 0.0;
        bool valid = false;
        juce::String name;

        float plr() const { return truePeakDb - lufs; }       // punch vs density
        float widthDb (float lo = 200.0f, float hi = 8000.0f) const;   // side relative to mid
        float tonalMean (float lo, float hi) const;           // mean mid level in a range (dB)
    };

    // Accumulates a long-term spectrum + loudness from blocks of stereo audio (not real-time safe:
    // runs on a background/message thread).
    class MixAnalyser
    {
    public:
        explicit MixAnalyser (double sampleRate);
        void addStereo (const float* l, const float* r, int numSamples);
        MixProfile finish (const juce::String& name) const;
        double getSecondsAnalysed() const { return totalSamples / fs; }

    private:
        void analyseFrame();
        static constexpr int fftOrder = 13, fftSize = 1 << fftOrder, hop = fftSize / 2;
        double fs;
        juce::dsp::FFT fft { fftOrder };
        std::vector<float> window, midBuf, sideBuf, work;
        int fill = 0;
        long long totalSamples = 0, frames = 0;
        std::array<double, MixProfile::numBands> midPow {}, sidePow {};
        LoudnessMeter loudness;
    };

    // Reads an audio file and analyses it (background thread). Returns invalid on failure.
    MixProfile analyseReferenceFile (const juce::File& file, juce::String& error);

    // Built-in genre targets (approximate long-term balances of typical commercial releases).
    juce::StringArray genreTargetNames();
    MixProfile genreTarget (const juce::String& genre);

    struct MixSuggestion
    {
        juce::String text;                 // what to tell the user
        juce::String paramId;              // empty = advice only
        float newValue = 0.0f;             // real units
    };

    struct MixComparison
    {
        std::array<float, MixProfile::numBands> diffDb {};    // + = reference has more (after loudness matching)
        std::vector<MixSuggestion> suggestions;
        float lufsDiff = 0.0f, plrDiff = 0.0f, widthDiff = 0.0f;
    };

    struct CurrentSettings { float elevation = 0.0f, width = 1.0f, glue = 0.0f; bool hasAirControl = true; juce::String moduleName; };
    MixComparison compareMixes (const MixProfile& mix, const MixProfile& reference, const CurrentSettings& now);

    //==========================================================================
    // Lock-free tap so the audio thread can hand its output to the assistant without blocking.
    class AudioTap
    {
    public:
        AudioTap() : fifo (capacity) { data.setSize (2, capacity); }
        void push (const float* l, const float* r, int n)      // audio thread
        {
            if (! enabled.load (std::memory_order_relaxed)) return;
            int s1, n1, s2, n2;
            fifo.prepareToWrite (n, s1, n1, s2, n2);
            if (n1 > 0) { data.copyFrom (0, s1, l, n1); data.copyFrom (1, s1, r, n1); }
            if (n2 > 0) { data.copyFrom (0, s2, l + n1, n2); data.copyFrom (1, s2, r + n1, n2); }
            fifo.finishedWrite (n1 + n2);
        }
        template <typename Fn> void drain (Fn&& consume)       // message thread
        {
            int s1, n1, s2, n2;
            fifo.prepareToRead (fifo.getNumReady(), s1, n1, s2, n2);
            if (n1 > 0) consume (data.getReadPointer (0, s1), data.getReadPointer (1, s1), n1);
            if (n2 > 0) consume (data.getReadPointer (0, s2), data.getReadPointer (1, s2), n2);
            fifo.finishedRead (n1 + n2);
        }
        void setEnabled (bool shouldBeOn) { if (! shouldBeOn) fifo.reset(); enabled = shouldBeOn; }
        bool isEnabled() const { return enabled.load(); }

    private:
        static constexpr int capacity = 1 << 18;   // ~5 s at 48 kHz between drains
        juce::AbstractFifo fifo;
        juce::AudioBuffer<float> data;
        std::atomic<bool> enabled { false };
    };
}
