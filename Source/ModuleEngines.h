#pragma once

#include <juce_dsp/juce_dsp.h>
#include <RTNeural/RTNeural.h>
#include <cmath>
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <fstream>
#include <cstring>
#include <utility>

namespace SolutionDSP
{
    enum class ModuleId { Bunker, Stage, Land, Loft, Orbit, Bridge, Void, Anchor, Aether, Mosaic, Innerspace, Matrix, Scope, Unknown };

    inline ModuleId moduleIdFromName (const char* name)
    {
        static constexpr std::pair<const char*, ModuleId> table[] = {
            { "TheBunker", ModuleId::Bunker }, { "TheStage", ModuleId::Stage },   { "TheLand", ModuleId::Land },
            { "TheLoft", ModuleId::Loft },     { "TheOrbit", ModuleId::Orbit },   { "TheBridge", ModuleId::Bridge },
            { "TheVoid", ModuleId::Void },     { "TheAnchor", ModuleId::Anchor }, { "TheAether", ModuleId::Aether },
            { "TheMosaic", ModuleId::Mosaic }, { "Innerspace", ModuleId::Innerspace },
            { "TheMatrix", ModuleId::Matrix }, { "TheScope", ModuleId::Scope } };
        for (const auto& [n, id] : table)
            if (std::strcmp (n, name) == 0)
                return id;
        return ModuleId::Unknown;
    }

    inline const char* moduleDisplayName (ModuleId id)
    {
        static constexpr const char* names[] = { "The Bunker", "The Stage", "The Land", "The Loft", "The Orbit", "The Bridge",
                                                 "The Void", "The Anchor", "The Aether", "The Mosaic", "Innerspace", "The Matrix", "The Scope", "Unknown" };
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
        juce::String trackName;                       // message thread only
    };

    struct RackLinkBus
    {
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
                    s.lufsShort = -70.0f; s.truePeak = -70.0f; s.gainReduct = 0.0f;
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
            roomNotch.prepare (spec);
            for (auto* f : { &lowShelf, &midPeak, &highShelf, &roomNotch })
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
            roomNotch.reset();
            if (neuralModel != nullptr)
                neuralModel->reset();
        }

        void triggerSweepCapture (float seconds = 2.0f)
        {
            const float clamped = std::clamp (seconds, 1.0f, 10.0f);
            captureTotalSamples = std::max (1, static_cast<int> (sampleRate * clamped));
            captureSamplesRemaining = captureTotalSamples;
            capturedPeakEnergy = 0.0f;
            capturedModeHz = 115.0f;
        }

        bool isCapturing() const { return captureSamplesRemaining > 0; }
        float getCapturedModeHz() const { return capturedModeHz; }

        bool loadNeuralProfile (const juce::File& jsonFile)
        {
            if (! jsonFile.existsAsFile())
                return false;
            std::ifstream stream (jsonFile.getFullPathName().toStdString());
            if (! stream.is_open())
                return false;
            auto loaded = RTNeural::json_parser::parseJson<float> (stream, true);
            if (loaded == nullptr)
                return false;
            loaded->reset();
            neuralModel = std::move (loaded);
            return true;
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

            *roomNotch.state = juce::dsp::IIR::ArrayCoefficients<float>::makePeakFilter (sampleRate, capturedModeHz, 4.0f, 0.72f);
        }

        void process (juce::AudioBuffer<float>& buffer, float mix, int profileIndex)
        {
            const int numSamples = buffer.getNumSamples();

            if (captureSamplesRemaining > 0)
            {
                float blockEnergy = 0.0f;
                for (int i = 0; i < numSamples; ++i)
                    blockEnergy += std::abs (buffer.getSample (0, i));
                if (blockEnergy > capturedPeakEnergy)
                {
                    capturedPeakEnergy = blockEnergy;
                    const float progress = 1.0f - (static_cast<float> (captureSamplesRemaining) / static_cast<float> (std::max (1, captureTotalSamples)));
                    capturedModeHz = std::clamp (45.0f + progress * 220.0f, 45.0f, 265.0f);
                }
                captureSamplesRemaining = std::max (0, captureSamplesRemaining - numSamples);
                if (captureSamplesRemaining == 0)
                    setProfile (currentProfile);
            }

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
            roomNotch.process (ctx);

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
                if (neuralModel != nullptr)
                {
                    float in[2] = { wl, wr };
                    wl = neuralModel->forward (in);
                    wr = wl;
                }
                dryL[i] = dryL[i] * (1.0f - mix) + wl * mix;
                dryR[i] = dryR[i] * (1.0f - mix) + wr * mix;
            }
        }

    private:
        double sampleRate = 48000.0;
        int currentProfile = 0;
        int captureSamplesRemaining = 0;
        int captureTotalSamples = 96000;
        float capturedPeakEnergy = 0.0f;
        float capturedModeHz = 115.0f;
        float tapMsL = 0.0f, tapMsR = 0.0f;
        juce::AudioBuffer<float> wetBuffer;
        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayL { 16384 };
        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayR { 16384 };
        juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>> lowShelf, midPeak, highShelf, roomNotch;
        std::unique_ptr<RTNeural::Model<float>> neuralModel;
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
        }

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
                roomMatcher.process (buffer, p.roomMix, p.roomProfile);

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
                deRumbleHPF.process (lctx);

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
                const float duckGain = p.dialogueDuck ? 0.82f : 1.0f;    // gentle mid-band duck when enabled
                const float airDrive = 1.0f + effectiveDepth * (moduleId == ModuleId::Aether ? 2.1f : 1.6f);
                const float clipDrive = 1.0f + (effectiveDepth + p.regenThrottle * 0.4f) * (moduleId == ModuleId::Anchor ? 2.5f : 1.4f);
                const float morphDrive = 1.0f + effectiveDepth * 2.0f;

                auto base = juce::dsp::AudioBlock<float> (buffer).getSubsetChannelBlock (0, 2);
                auto up = oversampler.processSamplesUp (base);
                for (size_t ch = 0; ch < 2; ++ch)
                {
                    auto* data = up.getChannelPointer (ch);
                    for (size_t i = 0; i < up.getNumSamples(); ++i)
                    {
                        float x = data[i];
                        switch (moduleId)
                        {
                            case ModuleId::Loft:
                            case ModuleId::Aether:  x = 0.7f * x + 0.3f * std::tanh (x * airDrive); break;
                            case ModuleId::Anchor:
                            case ModuleId::Bridge:  x = applyClipper (x, clipDrive); break;
                            case ModuleId::Mosaic:  x = 0.6f * x + 0.4f * applyClipper (x, morphDrive); break;
                            default: break;
                        }
                        x *= duckGain;
                        if (p.transientSculpt)   // Predictive Transient Sculpting (peak tamer)
                        {
                            const float ax = std::abs (x);
                            if (ax > 0.72f)
                                x = std::copysign (0.72f + 0.28f * std::tanh ((ax - 0.72f) * 2.5f), x);
                        }
                        data[i] = x;
                    }
                }
                oversampler.processSamplesDown (base);
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
                roomMatcher.process (buffer, p.roomMix, p.roomProfile);
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
    };
}
