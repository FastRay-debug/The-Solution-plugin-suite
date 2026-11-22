#pragma once

#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <complex>
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <cstring>
#include "Metering.h"
#include <utility>

namespace SolutionDSP
{
    // Crown and Chimera are original-design modules not (yet) built as plugins; their tuning is kept.
    enum class ModuleId { Bunker, Stage, Land, Loft, Orbit, Bridge, Void, Anchor, Aether, Mosaic, Innerspace, Matrix, Scope, Crown, Chimera, Unknown };

    inline ModuleId moduleIdFromName (const char* name)
    {
        static constexpr std::pair<const char*, ModuleId> table[] = {
            { "TheBunker", ModuleId::Bunker }, { "TheStage", ModuleId::Stage },   { "TheLand", ModuleId::Land },
            { "TheLoft", ModuleId::Loft },     { "TheOrbit", ModuleId::Orbit },   { "TheBridge", ModuleId::Bridge },
            { "TheVoid", ModuleId::Void },     { "TheAnchor", ModuleId::Anchor }, { "TheAether", ModuleId::Aether },
            { "TheMosaic", ModuleId::Mosaic }, { "Innerspace", ModuleId::Innerspace },
            { "TheMatrix", ModuleId::Matrix }, { "TheScope", ModuleId::Scope },
            { "TheCrown", ModuleId::Crown },   { "Chimera", ModuleId::Chimera } };
        for (const auto& [n, id] : table)
            if (std::strcmp (n, name) == 0)
                return id;
        return ModuleId::Unknown;
    }

    inline const char* moduleDisplayName (ModuleId id)
    {
        static constexpr const char* names[] = { "The Bunker", "The Stage", "The Land", "The Loft", "The Orbit", "The Bridge",
                                                 "The Void", "The Anchor", "The Aether", "The Mosaic", "Innerspace", "The Matrix", "The Scope",
                                                 "The Crown", "Chimera", "Unknown" };
        return names[std::clamp (static_cast<int> (id), 0, static_cast<int> (ModuleId::Unknown))];
    }

    // Biquad coefficients are rewritten in place on the audio thread. Giving every filter
    // a full-size biquad up front means those rewrites never allocate memory.
    template <typename Duplicator>
    inline void preallocateBiquad (Duplicator& filter, double sampleRate)
    {
        *filter.state = juce::dsp::IIR::ArrayCoefficients<float>::makeAllPass (sampleRate, 1000.0f);
    }
    // Shared across all plugin instances in the same DAW process (Rack Link + Matrix CV Bus).
    // Note: hosts that run each plugin in its own process (some sandbox modes) keep instances apart.
    struct RackSlot
    {
        std::atomic<bool>  active      { false };
        std::atomic<int>   moduleId    { 0 };
        std::atomic<int>   group       { 0 };        // 0 = not linked, 1-4 = groups A-D
        std::atomic<bool>  duckSource  { false };    // this instance ducks the rest of its group
        std::atomic<float> keyLevel    { 0.0f };     // 0..1 duck key (sources only)
        std::atomic<float> lufsShort   { -70.0f };
        std::atomic<float> truePeak    { -70.0f };
        std::atomic<float> gainReduct  { 0.0f };
        std::atomic<std::uint32_t> heartbeat { 0 };  // bumps every audio block
        std::atomic<int>   unmaskRole  { 0 };        // 0 off, 1 priority, 2 make room
        std::array<std::atomic<float>, 12> bandDb;   // priority's level per Unmask band (dBFS)

        // Auto-Align: recent raw input (mono) stamped with the host timeline, plus requests
        // from another instance asking this one to change its alignment delay / polarity.
        static constexpr int alignRingSize = 16384;
        std::atomic<int>   alignRole { 0 };          // 0 off, 1 reference, 2 align to reference
        std::array<float, alignRingSize> alignRing {};
        std::atomic<long long> alignRingEnd { -1 };  // timeline index one past the newest sample (-1 = not valid)
        std::atomic<bool>  alignRequest { false };
        std::atomic<float> alignRequestDelaySamples { 0.0f };
        std::atomic<bool>  alignRequestInvert { false };

        // Monitoring-tool safety: which listening aids are on here (bitmask), and a request
        // from another instance to switch them all off.
        std::atomic<int>   monitorMask { 0 };
        std::atomic<bool>  monitorOffRequest { false };
        std::atomic<bool>  panicRequest { false };      // Billet Audio hub: Emergency Panic Reset from another instance

        std::atomic<float> cpuLoad { 0.0f };             // this instance's share of the real-time budget (0..1)
        std::atomic<uint32_t> overloads { 0 }, errors { 0 };   // diagnostics counters
        std::atomic<int>   latency { 0 }, dqsLevel { 0 };

        // 3D stage: where this track sits and what it sounds like.
        std::atomic<float> posAz { 0.0f }, posEl { 0.0f }, posDist { 2.0f }, posWidth { 1.0f }, toneHz { 1000.0f };
        std::atomic<int> creature { 0 };      // 0 = auto, 1..6 = the creature picked for this track
        std::atomic<bool>  dialogueSource { false };   // this track is dialogue (its voice detector feeds the duckers)
        std::atomic<float> voice { 0.0f };             // its live voice-activity confidence 0..1
        // Parameter change requests from another instance's stage (index into getParameters(), real value).
        static constexpr int maxRequests = 32;     // enough for a whole preset revert from the rack history
        std::array<std::atomic<int>, maxRequests> reqParam;
        std::array<std::atomic<float>, maxRequests> reqValue;
        bool pushRequest (int paramIndex, float value)
        {
            for (int i = 0; i < maxRequests; ++i)
            {
                int expected = -1;
                if (reqParam[(size_t) i].load() == -1)
                {
                    reqValue[(size_t) i].store (value);
                    if (reqParam[(size_t) i].compare_exchange_strong (expected, paramIndex)) return true;
                }
            }
            return false;
        }
        juce::String trackName;                       // message thread only
        juce::String historyText;                     // this instance's recent edits (EditHistory::toString), message thread only
    };

    struct RackLinkBus
    {
        juce::String suiteTheme;            // hardware theme the whole session follows (message thread only)
        int suiteThemeGen = 0;
        std::atomic<int>   activeInstances { 0 };
        std::atomic<float> masterSubRms    { 0.0f };
        std::atomic<float> masterPhase     { 1.0f };
        std::atomic<float> cvLfoValue      { 0.0f };
        std::atomic<float> envFollower     { 0.0f };
        std::atomic<float> lufsMomentary   { -60.0f };
        std::atomic<float> truePeakDb      { -60.0f };
        std::atomic<float> suiteGlue       { 0.0f };    // set by The Scope, followed by others

        static constexpr int maxSlots = 64;
        std::array<RackSlot, maxSlots> slots;

        int claimSlot (int moduleId)
        {
            for (int i = 0; i < maxSlots; ++i)
            {
                bool expected = false;
                if (slots[(size_t) i].active.compare_exchange_strong (expected, true))
                {
                    auto& s = slots[(size_t) i];
                    s.moduleId = moduleId; s.group = 0; s.duckSource = false; s.keyLevel = 0.0f;
                    s.lufsShort = -70.0f; s.truePeak = -70.0f; s.gainReduct = 0.0f; s.unmaskRole = 0;
                    for (auto& b : s.bandDb) b = -100.0f;
                    s.alignRole = 0; s.alignRingEnd = -1; s.alignRequest = false;
                    s.monitorMask = 0; s.monitorOffRequest = false;
                    s.posAz = 0.0f; s.posEl = 0.0f; s.posDist = 2.0f; s.posWidth = 1.0f; s.toneHz = 1000.0f; s.creature = 0; s.dialogueSource = false; s.voice = 0.0f; s.panicRequest = false; s.overloads = 0; s.errors = 0; s.latency = 0; s.dqsLevel = 0;
                    for (auto& q : s.reqParam) q = -1;
                    return i;
                }
            }
            return -1;   // more than 64 instances: still works, just not shown/linked
        }

        void releaseSlot (int index)
        {
            if (juce::isPositiveAndBelow (index, maxSlots))
            {
                slots[(size_t) index].keyLevel = 0.0f;
                slots[(size_t) index].duckSource = false;
                slots[(size_t) index].active = false;
            }
        }

        // Loudest level per Unmask band among the *priority* instances in a group.
        void groupPriorityBands (int group, int excludeSlot, std::array<float, 12>& out) const
        {
            out.fill (-100.0f);
            if (group <= 0) return;
            for (int i = 0; i < maxSlots; ++i)
            {
                const auto& s = slots[(size_t) i];
                if (i != excludeSlot && s.active.load (std::memory_order_relaxed) && s.unmaskRole.load (std::memory_order_relaxed) == 1
                    && s.group.load (std::memory_order_relaxed) == group)
                    for (size_t b = 0; b < out.size(); ++b) out[b] = std::max (out[b], s.bandDb[b].load (std::memory_order_relaxed));
            }
        }

        // Loudest duck key among the *other* sources in a group (0..1).
        float groupKey (int group, int excludeSlot) const
        {
            if (group <= 0) return 0.0f;
            float key = 0.0f;
            for (int i = 0; i < maxSlots; ++i)
            {
                const auto& s = slots[(size_t) i];
                if (i != excludeSlot && s.active.load (std::memory_order_relaxed)
                    && s.duckSource.load (std::memory_order_relaxed) && s.group.load (std::memory_order_relaxed) == group)
                    key = std::max (key, s.keyLevel.load (std::memory_order_relaxed));
            }
            return key;
        }
    };

    struct ProcessParams
    {
        float azimuthDeg      = 0.0f;
        float elevationDeg    = 0.0f;
        float depth           = 0.25f;
        float haasMs          = 0.0f;
        float width           = 1.0f;
        float anchorHz        = 90.0f;
        float midDb           = 0.0f;
        float sideDb          = 0.0f;
        float roomMix         = 0.0f;
        int   roomProfile     = 0;
        int   patchOrder      = 0; // 0: Module -> Room, 1: Room -> Module
        float wetMix          = 1.0f;
        bool  transientSculpt = false;
        float ultraWide       = 0.0f;
        bool  tapeCalTone     = false;
        float cvModDepth      = 0.0f;
        int   clipperShape    = 0; // 0: Soft Knee, 1: Warm Tube, 2: Hard Brickwall
        float regenThrottle   = 0.0f;
        bool  dialogueDuck    = false;
        int   modeNotch       = 2;       // fixed 115 Hz notch in the room profiles: 0 off, 1 always on, 2 auto (when 115 Hz builds up)
        bool  infraLift       = false;   // The Bunker: Infrasonic Lift is on, so the sub band skips the de-rumble cut
    };

    //==========================================================================
    // DC Guard: 1st-order DC blocker at ~1 Hz in double precision. Removes DC offset (and the slow drift
    // asymmetric saturation creates) while leaving even 7 Hz infrasonics essentially untouched (-0.04 dB).
    class DcGuard
    {
    public:
        static constexpr double cornerHz = 1.0;
        void prepare (double fs) { r = std::exp (-juce::MathConstants<double>::twoPi * cornerHz / fs); reset(); }
        void reset() { for (auto& s : st) s = {}; dcEstimate = 0.0; }
        void process (float* l, float* r_, int n)
        {
            float* ch[2] = { l, r_ };
            double sum = 0.0;
            for (int c = 0; c < 2; ++c)
                if (ch[c] != nullptr)
                    for (int i = 0; i < n; ++i)
                    {
                        const double x = ch[c][i], y = x - st[c].x1 + r * st[c].y1;
                        st[c].x1 = x; st[c].y1 = y; sum += x - y;            // x - y = the DC it removed
                        ch[c][i] = (float) y;
                    }
            const double blockDc = sum / std::max (1, 2 * n);
            dcEstimate += 0.05 * (blockDc - dcEstimate);
        }
        double removedDc() const { return dcEstimate; }   // smoothed average offset being removed (linear)
    private:
        struct State { double x1 = 0.0, y1 = 0.0; };
        State st[2];
        double r = 0.9999, dcEstimate = 0.0;
    };

    //==========================================================================
    // Infrasonic Lift (The Bunker): a bell centred on 7.63 Hz - narrow by default (true infrasonics only,
    // 15-30 Hz left essentially alone), or wide so its skirt also fills in 15-30 Hz - plus a 3 Hz guard so DC and sub-3 Hz drift never reach the speakers. Double precision:
    // a 7.63 Hz biquad at 48-192 kHz is too close to z = 1 for float coefficients.
    class InfraLift
    {
    public:
        static constexpr double centreHz = 7.63, narrowQ = 2.2, wideQ = 0.6, guardHz = 3.0;
        void prepare (double fs) { sampleRate = fs; lastDb = -1000.0f; reset(); }
        void process (float* l, float* r, int n, float gainDb, bool wide)
        {
            if (wide != lastWide) { lastWide = wide; lastDb = -1000.0f; }
            process (l, r, n, gainDb);
        }
        void reset() { for (auto* b : { &bell[0], &bell[1], &guard[0], &guard[1] }) b->z1 = b->z2 = 0.0; }
        void process (float* l, float* r, int n, float gainDb)
        {
            if (std::abs (gainDb - lastDb) > 0.005f) { setCoefficients (gainDb); lastDb = gainDb; }
            float* ch[2] = { l, r };
            for (int c = 0; c < 2; ++c)
                if (ch[c] != nullptr)
                    for (int i = 0; i < n; ++i)
                        ch[c][i] = (float) guard[c].run (bell[c].run ((double) ch[c][i]));
        }
        // Magnitude of the whole stage at f (Hz), for tests and the UI curve.
        double magnitudeAt (double f) const { return mag (bell[0], f) * mag (guard[0], f); }

    private:
        struct BQ
        {
            double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
            double run (double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
        };
        void setCoefficients (float gainDb)
        {
            const double q = lastWide ? wideQ : narrowQ;
            const double w = juce::MathConstants<double>::twoPi * centreHz / sampleRate, al = std::sin (w) / (2 * q), c = std::cos (w);
            const double A = std::pow (10.0, gainDb / 40.0), a0 = 1 + al / A;
            for (auto& b : bell) { const double z1 = b.z1, z2 = b.z2;
                b = { (1 + al * A) / a0, -2 * c / a0, (1 - al * A) / a0, -2 * c / a0, (1 - al / A) / a0 }; b.z1 = z1; b.z2 = z2; }
            const double wg = juce::MathConstants<double>::twoPi * guardHz / sampleRate, ag = std::sin (wg) / (2 * 0.7071), cg = std::cos (wg), g0 = 1 + ag;
            for (auto& b : guard) { const double z1 = b.z1, z2 = b.z2;
                b = { (1 + cg) / 2 / g0, -(1 + cg) / g0, (1 + cg) / 2 / g0, -2 * cg / g0, (1 - ag) / g0 }; b.z1 = z1; b.z2 = z2; }
        }
        double mag (const BQ& b, double f) const
        {
            const std::complex<double> z = std::polar (1.0, juce::MathConstants<double>::twoPi * f / sampleRate), zi = 1.0 / z;
            return std::abs ((b.b0 + b.b1 * zi + b.b2 * zi * zi) / (1.0 + b.a1 * zi + b.a2 * zi * zi));
        }
        double sampleRate = 48000.0;
        float lastDb = -1000.0f;
        bool lastWide = false;
        BQ bell[2], guard[2];
    };

    class RoomMatcherEngine
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec)
        {
            sampleRate = spec.sampleRate;
            wetBuffer.setSize (std::max (2, static_cast<int> (spec.numChannels)), static_cast<int> (spec.maximumBlockSize), false, true, true);
            delayL.prepare (spec);
            delayR.prepare (spec);
            delayL.reset();
            delayR.reset();
            lowShelf.prepare (spec);
            midPeak.prepare (spec);
            highShelf.prepare (spec);
            modeNotch.prepare (spec);
            setupAutoNotch();
            for (auto* f : { &lowShelf, &midPeak, &highShelf, &modeNotch })
                preallocateBiquad (*f, sampleRate);
            setProfile (currentProfile);
        }

        void reset()
        {
            delayL.reset();
            delayR.reset();
            lowShelf.reset();
            midPeak.reset();
            highShelf.reset();
            modeNotch.reset();
            clearAutoNotch();
        }

        void setProfile (int profileIndex)
        {
            currentProfile = profileIndex;
            if (sampleRate <= 0.0)
                return;

            if (profileIndex == 1) // Mastering Studio
            {
                *lowShelf.state = juce::dsp::IIR::ArrayCoefficients<float>::makeLowShelf (sampleRate, 85.0f, 0.7f, 1.08f);
                *midPeak.state = juce::dsp::IIR::ArrayCoefficients<float>::makePeakFilter (sampleRate, 320.0f, 1.4f, 0.88f);
                *highShelf.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighShelf (sampleRate, 8500.0f, 0.7f, 1.12f);
                tapMsL = 7.2f; tapMsR = 9.4f;
            }
            else if (profileIndex == 2) // '69 Lincoln Cabin
            {
                *lowShelf.state = juce::dsp::IIR::ArrayCoefficients<float>::makeLowShelf (sampleRate, 110.0f, 0.8f, 1.25f);
                *midPeak.state = juce::dsp::IIR::ArrayCoefficients<float>::makePeakFilter (sampleRate, 650.0f, 1.1f, 0.82f);
                *highShelf.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighShelf (sampleRate, 5500.0f, 0.7f, 0.85f);
                tapMsL = 3.1f; tapMsR = 4.6f;
            }
            else if (profileIndex == 3) // Club System
            {
                *lowShelf.state = juce::dsp::IIR::ArrayCoefficients<float>::makeLowShelf (sampleRate, 65.0f, 0.9f, 1.38f);
                *midPeak.state = juce::dsp::IIR::ArrayCoefficients<float>::makePeakFilter (sampleRate, 450.0f, 1.2f, 0.85f);
                *highShelf.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighShelf (sampleRate, 10000.0f, 0.7f, 1.15f);
                tapMsL = 14.5f; tapMsR = 19.2f;
            }
            else
            {
                *lowShelf.state = juce::dsp::IIR::ArrayCoefficients<float>::makeLowShelf (sampleRate, 80.0f, 0.7f, 1.0f);
                *midPeak.state = juce::dsp::IIR::ArrayCoefficients<float>::makePeakFilter (sampleRate, 1000.0f, 1.0f, 1.0f);
                *highShelf.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighShelf (sampleRate, 8000.0f, 0.7f, 1.0f);
                tapMsL = 0.0f; tapMsR = 0.0f;
            }

            // Original suite tuning: fixed 115 Hz "mode notch" (-2.85 dB, Q 4) in every translation profile.
            *modeNotch.state = juce::dsp::IIR::ArrayCoefficients<float>::makePeakFilter (sampleRate, 115.0f, 4.0f, 0.72f);
        }

        void process (juce::AudioBuffer<float>& buffer, float mix, int profileIndex, int notchMode = 2)
        {
            const int numSamples = buffer.getNumSamples();

            if (mix <= 0.001f || profileIndex == 0)
                return;
            if (profileIndex != currentProfile)
                setProfile (profileIndex);

            if (wetBuffer.getNumSamples() < numSamples)
                wetBuffer.setSize (2, numSamples, false, false, true);
            for (int ch = 0; ch < 2; ++ch)
                wetBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);

            juce::dsp::AudioBlock<float> block (wetBuffer.getArrayOfWritePointers(), 2, static_cast<size_t> (numSamples));
            juce::dsp::ProcessContextReplacing<float> ctx (block);
            lowShelf.process (ctx);
            midPeak.process (ctx);
            highShelf.process (ctx);
            if (notchMode == 1)
                modeNotch.process (ctx);                       // original: always on
            else
            {
                modeNotch.reset();                             // start clean if switched back to "on"
                if (notchMode == 2)
                    autoNotch (wetBuffer, numSamples);         // only when 115 Hz is building up
                else
                    autoDepth = 0.0f;
            }

            const float dL = static_cast<float> ((tapMsL * 0.001) * sampleRate);
            const float dR = static_cast<float> ((tapMsR * 0.001) * sampleRate);
            delayL.setDelay (dL);
            delayR.setDelay (dR);

            auto* dryL = buffer.getWritePointer (0);
            auto* dryR = buffer.getWritePointer (1);
            auto* wetL = wetBuffer.getWritePointer (0);
            auto* wetR = wetBuffer.getWritePointer (1);

            for (int i = 0; i < numSamples; ++i)
            {
                float wl = wetL[i];
                float wr = wetR[i];
                if (dL > 0.0f)
                {
                    const float reflL = delayL.popSample (0);
                    const float reflR = delayR.popSample (1);
                    delayL.pushSample (0, wl);
                    delayR.pushSample (1, wr);
                    wl = wl * 0.85f + reflL * 0.15f;
                    wr = wr * 0.85f + reflR * 0.15f;
                }
                dryL[i] = dryL[i] * (1.0f - mix) + wl * mix;
                dryR[i] = dryR[i] * (1.0f - mix) + wr * mix;
            }
        }

    private:
        double sampleRate = 48000.0;
        int currentProfile = 0;
        float tapMsL = 0.0f, tapMsR = 0.0f;
        juce::AudioBuffer<float> wetBuffer;
        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayL { 16384 };
        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayR { 16384 };
        juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>> lowShelf, midPeak, highShelf, modeNotch;

    public:
        // How far the auto notch is engaged right now: 0 = off .. 1 = full -2.85 dB (any thread).
        float getAutoNotchDepth() const { return autoDepthShared.load (std::memory_order_relaxed); }

    private:
        // ---- Auto 115 Hz notch: a dynamic version of the fixed notch.
        // Detector: energy in a 115 Hz band vs. the average of 80 Hz and 165 Hz bands (constant-Q,
        // so a balanced mix reads ~0 dB). +3 dB buildup starts it, +6 dB gives the full original depth.
        // Applied as x - k * bandpass115(x): at 115 Hz the gain is (1 - k), so k = 0.28 equals 0.72 (-2.85 dB).
        struct Bp { double b0 = 0, b2 = 0, a1 = 0, a2 = 0; double z1 = 0, z2 = 0;
                    void set (double fs, double f, double q) { const double w = juce::MathConstants<double>::twoPi * f / fs, al = std::sin (w) / (2 * q), a0 = 1 + al;
                                                               b0 = al / a0; b2 = -al / a0; a1 = -2 * std::cos (w) / a0; a2 = (1 - al) / a0; }
                    double run (double x) { const double y = b0 * x + z1; z1 = -a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
                    void clear() { z1 = z2 = 0; } };

        void setupAutoNotch()
        {
            det115.set (sampleRate, 115.0, 4.0); det80.set (sampleRate, 80.0, 4.0); det165.set (sampleRate, 165.0, 4.0);
            for (auto& b : apply115) b.set (sampleRate, 115.0, 4.0);
            envCoeff   = std::exp (-1.0 / (0.050 * sampleRate));       // 50 ms energy window
            attackCoef = (float) std::exp (-1.0 / (0.020 * sampleRate));
            releaseCoef = (float) std::exp (-1.0 / (0.300 * sampleRate));
            clearAutoNotch();
        }
        void clearAutoNotch()
        {
            det115.clear(); det80.clear(); det165.clear();
            for (auto& b : apply115) b.clear();
            e115 = e80 = e165 = 0.0; autoDepth = 0.0f; autoDepthShared = 0.0f;
        }

        void autoNotch (juce::AudioBuffer<float>& buf, int numSamples)
        {
            auto* l = buf.getWritePointer (0);
            auto* r = buf.getWritePointer (1);
            for (int i = 0; i < numSamples; ++i)
            {
                const double mono = 0.5 * ((double) l[i] + (double) r[i]);
                const double a = det115.run (mono), b = det80.run (mono), c = det165.run (mono);
                e115 = envCoeff * e115 + (1 - envCoeff) * a * a;
                e80  = envCoeff * e80  + (1 - envCoeff) * b * b;
                e165 = envCoeff * e165 + (1 - envCoeff) * c * c;
                float target = 0.0f;
                if (e115 > 1.0e-7)                                     // ignore near-silence (below ~-70 dBFS)
                {
                    const double buildupDb = 10.0 * std::log10 (e115 / (0.5 * (e80 + e165) + 1.0e-12));
                    target = (float) juce::jlimit (0.0, 1.0, (buildupDb - 3.0) / 3.0);
                }
                autoDepth = target > autoDepth ? attackCoef * autoDepth + (1 - attackCoef) * target
                                               : releaseCoef * autoDepth + (1 - releaseCoef) * target;
                const double k = 0.28 * autoDepth;
                l[i] = (float) (l[i] - k * apply115[0].run (l[i]));
                r[i] = (float) (r[i] - k * apply115[1].run (r[i]));
            }
            autoDepthShared.store (autoDepth, std::memory_order_relaxed);
        }

        Bp det115, det80, det165;
        std::array<Bp, 2> apply115;
        double e115 = 0, e80 = 0, e165 = 0, envCoeff = 0.99;
        float autoDepth = 0.0f, attackCoef = 0.99f, releaseCoef = 0.999f;
        std::atomic<float> autoDepthShared { 0.0f };
    };

    class SuiteModuleEngine
    {
    public:
        void prepare (const juce::dsp::ProcessSpec& spec)
        {
            sampleRate = spec.sampleRate;
            lowBandBuffer.setSize (std::max (2, static_cast<int> (spec.numChannels)), static_cast<int> (spec.maximumBlockSize), false, true, true);
            highBandBuffer.setSize (std::max (2, static_cast<int> (spec.numChannels)), static_cast<int> (spec.maximumBlockSize), false, true, true);

            haasDelay.prepare (spec);
            haasDelay.reset();
            ultraWideDelay.prepare (spec);
            ultraWideDelay.reset();

            lowCross.prepare (spec);
            lowCross.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
            highCross.prepare (spec);
            highCross.setType (juce::dsp::LinkwitzRileyFilterType::highpass);

            airShelf.prepare (spec);
            depthLowPass.prepare (spec);
            deRumbleHPF.prepare (spec);
            for (auto* f : { &airShelf, &depthLowPass, &deRumbleHPF })
                preallocateBiquad (*f, sampleRate);
            roomMatcher.prepare (spec);
            oversampler.initProcessing (static_cast<size_t> (spec.maximumBlockSize));
            oversampler.reset();
            os2.initProcessing (static_cast<size_t> (spec.maximumBlockSize));
            os2.reset();
            // Every quality level ends up with the 4x path's latency, so the DAW never sees a change.
            const int l4 = getLatencySamples(), l2 = (int) std::lround (os2.getLatencyInSamples());
            pad2.prepare (2, std::max (0, l4 - l2));
            pad1.prepare (2, l4);
            dqsScratch.setSize (2, static_cast<int> (spec.maximumBlockSize), false, true, true);
            dqsPrime = l4 + 32;
            dqsFade = std::max (64, (int) (0.010 * spec.sampleRate));
            qualityLevel = targetQuality = 0; transitionPos = -1;
            calPhase = 0.0;
            lfoPhase = 0.0;
        }

        void reset()
        {
            haasDelay.reset();
            ultraWideDelay.reset();
            lowCross.reset();
            highCross.reset();
            airShelf.reset();
            depthLowPass.reset();
            deRumbleHPF.reset();
            roomMatcher.reset();
            oversampler.reset();
            os2.reset(); pad2.reset(); pad1.reset();
            qualityLevel = targetQuality; transitionPos = -1;
        }

        // DQS: 0 = 4x oversampling (best), 1 = 2x, 2 = no oversampling (lightest). Audio thread.
        // Changes are crossfaded after the new path has warmed up, at constant latency.
        void setQualityLevel (int level) { if (transitionPos < 0) targetQuality = juce::jlimit (0, 2, level); }
        int getQualityLevel() const { return qualityLevel; }

        // Fixed processing delay (from the oversampling filters) - reported to the DAW.
        int getLatencySamples() const { return static_cast<int> (std::lround (oversampler.getLatencyInSamples())); }

        RoomMatcherEngine& getRoomMatcher() { return roomMatcher; }
        const RoomMatcherEngine& getRoomMatcher() const { return roomMatcher; }

        void process (ModuleId moduleId, juce::AudioBuffer<float>& buffer, const ProcessParams& p, RackLinkBus& bus)
        {
            const int numSamples = buffer.getNumSamples();
            if (buffer.getNumChannels() < 2 || numSamples == 0)
                return;

            // Golden Egg: Tape Saturation Calibration Mode (-18 dBFS 1 kHz test tone)
            if (p.tapeCalTone)
            {
                const float calAmp = juce::Decibels::decibelsToGain (-18.0f);
                const double phaseInc = 1000.0 / std::max (1.0, sampleRate);
                auto* l = buffer.getWritePointer (0);
                auto* r = buffer.getWritePointer (1);
                for (int i = 0; i < numSamples; ++i)
                {
                    const float s = static_cast<float> (std::sin (calPhase * juce::MathConstants<double>::twoPi)) * calAmp;
                    l[i] = s;
                    r[i] = s;
                    calPhase += phaseInc;
                    if (calPhase >= 1.0) calPhase -= 1.0;
                }
                return;
            }

            // Matrix CV LFO + Envelope Follower update
            const double lfoInc = (0.5 + p.depth * 3.5) / std::max (1.0, sampleRate);
            float blockEnv = 0.0f;
            for (int i = 0; i < numSamples; ++i)
            {
                blockEnv = std::max (blockEnv, std::abs (buffer.getSample (0, i)));
                lfoPhase += lfoInc;
                if (lfoPhase >= 1.0) lfoPhase -= 1.0;
            }
            const float lfoVal = static_cast<float> (std::sin (lfoPhase * juce::MathConstants<double>::twoPi));
            if (moduleId == ModuleId::Matrix)
            {
                bus.cvLfoValue.store (lfoVal, std::memory_order_relaxed);
                bus.envFollower.store (blockEnv, std::memory_order_relaxed);
            }
            const float cvOffset = bus.cvLfoValue.load (std::memory_order_relaxed) * p.cvModDepth;

            if (p.patchOrder == 1)
                roomMatcher.process (buffer, p.roomMix, p.roomProfile, p.modeNotch);

            const float effectiveDepth = std::clamp (p.depth + cvOffset * 0.25f, 0.0f, 1.0f);
            const float effectiveWidth = std::clamp (p.width + cvOffset * 0.35f, 0.0f, 2.0f);

            lowCross.setCutoffFrequency (std::clamp (p.anchorHz, 20.0f, 250.0f));
            highCross.setCutoffFrequency (std::clamp (p.anchorHz, 20.0f, 250.0f));

            const float airDb = std::clamp (p.elevationDeg / 90.0f * 4.5f, -6.0f, 6.0f);
            *airShelf.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighShelf (
                sampleRate, 6500.0f, 0.7f, juce::Decibels::decibelsToGain (airDb));

            const float depthCutoff = 20000.0f - effectiveDepth * 14000.0f;
            *depthLowPass.state = juce::dsp::IIR::ArrayCoefficients<float>::makeLowPass (
                sampleRate, std::clamp (depthCutoff, 1800.0f, 20000.0f));

            *deRumbleHPF.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass (
                sampleRate, 18.0f + effectiveDepth * 24.0f);

            auto applyClipper = [&] (float x, float drive) -> float
            {
                if (p.clipperShape == 2) // Hard Brickwall
                    return std::clamp (x * drive, -0.944f, 0.944f);
                if (p.clipperShape == 1) // Warm Tube (asymmetric)
                    return std::tanh (x * drive + 0.08f * x * x) / std::max (1.0f, std::tanh (drive));
                return std::tanh (x * drive) / std::max (1.0f, std::tanh (drive)); // Soft Knee
            };

            // Linear, per-module tone shaping runs at the normal sample rate.
            if (moduleId == ModuleId::Bunker)
            {
                if (lowBandBuffer.getNumSamples() < numSamples)
                {
                    lowBandBuffer.setSize (2, numSamples, false, false, true);
                    highBandBuffer.setSize (2, numSamples, false, false, true);
                }
                for (int ch = 0; ch < 2; ++ch)
                {
                    lowBandBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);
                    highBandBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);
                }

                juce::dsp::AudioBlock<float> lb (lowBandBuffer.getArrayOfWritePointers(), 2, static_cast<size_t> (numSamples));
                juce::dsp::AudioBlock<float> hb (highBandBuffer.getArrayOfWritePointers(), 2, static_cast<size_t> (numSamples));
                juce::dsp::ProcessContextReplacing<float> lctx (lb), hctx (hb);
                lowCross.process (lctx);
                highCross.process (hctx);
                if (! p.infraLift) deRumbleHPF.process (lctx);      // Infrasonic Lift replaces the de-rumble cut

                // Sub-band saturation: only low frequencies, so no oversampling needed.
                const float drive = 1.0f + effectiveDepth * 3.5f;
                for (int i = 0; i < numSamples; ++i)
                {
                    const float monoSub = 0.5f * (lowBandBuffer.getSample (0, i) + lowBandBuffer.getSample (1, i));
                    const float satSub = applyClipper (monoSub, drive);
                    buffer.setSample (0, i, satSub + highBandBuffer.getSample (0, i));
                    buffer.setSample (1, i, satSub + highBandBuffer.getSample (1, i));
                }
            }
            else if (moduleId == ModuleId::Loft || moduleId == ModuleId::Aether || moduleId == ModuleId::Mosaic
                     || moduleId == ModuleId::Crown || moduleId == ModuleId::Chimera
                     || moduleId == ModuleId::Stage || moduleId == ModuleId::Land || moduleId == ModuleId::Orbit || moduleId == ModuleId::Void)
            {
                auto blk = juce::dsp::AudioBlock<float> (buffer).getSubsetChannelBlock (0, 2);
                juce::dsp::ProcessContextReplacing<float> ctx (blk);
                airShelf.process (ctx);
                if (moduleId == ModuleId::Stage || moduleId == ModuleId::Land || moduleId == ModuleId::Orbit || moduleId == ModuleId::Void)
                    depthLowPass.process (ctx);
            }
            else if (moduleId == ModuleId::Innerspace)
            {
                auto blk = juce::dsp::AudioBlock<float> (buffer).getSubsetChannelBlock (0, 2);
                juce::dsp::ProcessContextReplacing<float> ctx (blk);
                depthLowPass.process (ctx);
            }

            // Non-linear stages (saturation, clipping, transient sculpting) run 4x oversampled
            // so they don't fold harsh aliasing back into the audible range. Every module goes
            // through the oversampler so the latency reported to the DAW is always the same.
            {
                const float airDrive = 1.0f + effectiveDepth * (moduleId == ModuleId::Aether ? 2.1f : 1.6f);
                const float clipDrive = 1.0f + (effectiveDepth + p.regenThrottle * 0.4f)
                                        * (moduleId == ModuleId::Anchor ? 2.5f : (moduleId == ModuleId::Crown ? 1.8f : 1.4f));
                const float morphDrive = 1.0f + effectiveDepth * (moduleId == ModuleId::Chimera ? 2.4f : 2.0f);

                auto shape = [&] (float x)
                {
                    switch (moduleId)
                    {
                        case ModuleId::Loft:
                        case ModuleId::Aether:  x = 0.7f * x + 0.3f * std::tanh (x * airDrive); break;
                        case ModuleId::Anchor:
                        case ModuleId::Bridge:
                        case ModuleId::Crown:   x = applyClipper (x, clipDrive); break;
                        case ModuleId::Mosaic:
                        case ModuleId::Chimera: x = 0.6f * x + 0.4f * applyClipper (x, morphDrive); break;
                        default: break;
                    }
                    return x;
                };
                auto shapeBlock = [&] (juce::dsp::AudioBlock<float> blk)
                {
                    for (size_t ch = 0; ch < 2; ++ch)
                    {
                        auto* data = blk.getChannelPointer (ch);
                        for (size_t i = 0; i < blk.getNumSamples(); ++i) data[i] = shape (data[i]);
                    }
                };
                auto runPath = [&] (int level, juce::AudioBuffer<float>& buf)
                {
                    juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), 2, numSamples);
                    auto blk = juce::dsp::AudioBlock<float> (view);
                    if (level == 0)      { auto up = oversampler.processSamplesUp (blk); shapeBlock (up); oversampler.processSamplesDown (blk); }
                    else if (level == 1) { auto up = os2.processSamplesUp (blk); shapeBlock (up); os2.processSamplesDown (blk); pad2.process (view, numSamples); }
                    else                 { shapeBlock (blk); pad1.process (view, numSamples); }
                };
                auto resetPath = [&] (int level)
                {
                    if (level == 0) oversampler.reset(); else if (level == 1) { os2.reset(); pad2.reset(); } else pad1.reset();
                };

                if (transitionPos < 0 && targetQuality == qualityLevel)
                    runPath (qualityLevel, buffer);
                else
                {
                    if (transitionPos < 0) { transitionPos = 0; resetPath (targetQuality); }
                    for (int ch = 0; ch < 2; ++ch) dqsScratch.copyFrom (ch, 0, buffer, ch, 0, numSamples);
                    runPath (qualityLevel, buffer);       // current quality
                    runPath (targetQuality, dqsScratch);  // new quality, warming up then fading in
                    for (int i = 0; i < numSamples; ++i)
                    {
                        const float g = juce::jlimit (0.0f, 1.0f, (float) (transitionPos + i - dqsPrime) / (float) dqsFade);
                        for (int ch = 0; ch < 2; ++ch)
                        {
                            auto* d = buffer.getWritePointer (ch);
                            d[i] += g * (dqsScratch.getSample (ch, i) - d[i]);
                        }
                    }
                    transitionPos += numSamples;
                    if (transitionPos >= dqsPrime + dqsFade) { qualityLevel = targetQuality; transitionPos = -1; }
                }
            }

            const float midGain  = juce::Decibels::decibelsToGain (p.midDb);
            const float sideGain = juce::Decibels::decibelsToGain (p.sideDb) * effectiveWidth;
            const float panNorm  = std::clamp (std::sin (juce::degreesToRadians (p.azimuthDeg)), -1.0f, 1.0f);
            const float delaySamples = static_cast<float> ((p.haasMs * 0.001) * sampleRate);
            haasDelay.setDelay (delaySamples);

            const float uwSamples = static_cast<float> ((0.014 * sampleRate)); // 14 ms Haas-Kempf decorrelation
            ultraWideDelay.setDelay (uwSamples);

            auto* left  = buffer.getWritePointer (0);
            auto* right = buffer.getWritePointer (1);

            for (int i = 0; i < numSamples; ++i)
            {
                float l = left[i];
                float r = right[i];

                if (delaySamples > 0.0f)
                {
                    if (panNorm >= 0.0f)
                    {
                        const float d = haasDelay.popSample (0);
                        haasDelay.pushSample (0, l);
                        l = d;
                    }
                    else
                    {
                        const float d = haasDelay.popSample (1);
                        haasDelay.pushSample (1, r);
                        r = d;
                    }
                }

                if (p.ultraWide > 0.001f)
                {
                    const float uwR = ultraWideDelay.popSample (1);
                    ultraWideDelay.pushSample (1, r);
                    const float sideDecor = (l - uwR) * 0.5f * p.ultraWide;
                    l += sideDecor * 0.35f;
                    r -= sideDecor * 0.35f;
                }

                const float mid  = 0.5f * (l + r) * midGain;
                const float side = 0.5f * (l - r) * sideGain;

                left[i]  = (mid + side) * (1.0f - 0.25f * std::max (0.0f,  panNorm));
                right[i] = (mid - side) * (1.0f - 0.25f * std::max (0.0f, -panNorm));
            }

            if (p.patchOrder != 1)
                roomMatcher.process (buffer, p.roomMix, p.roomProfile, p.modeNotch);
        }

    private:
        double sampleRate = 48000.0;
        double calPhase = 0.0;
        double lfoPhase = 0.0;
        juce::AudioBuffer<float> lowBandBuffer, highBandBuffer;
        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd> haasDelay { 16384 };
        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> ultraWideDelay { 16384 };
        juce::dsp::LinkwitzRileyFilter<float> lowCross, highCross;
        juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>> airShelf, depthLowPass, deRumbleHPF;
        RoomMatcherEngine roomMatcher;
        juce::dsp::Oversampling<float> oversampler { 2, 2, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true };
        juce::dsp::Oversampling<float> os2 { 2, 1, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true };
        IntegerDelay pad2, pad1;
        juce::AudioBuffer<float> dqsScratch;
        int qualityLevel = 0, targetQuality = 0, transitionPos = -1, dqsPrime = 64, dqsFade = 480;
    };
}
