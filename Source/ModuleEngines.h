#pragma once

#include <juce_dsp/juce_dsp.h>
#include <RTNeural/RTNeural.h>
#include <cmath>
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <fstream>

namespace SolutionDSP
{
    // Shared across all plugin instances in the same DAW process (Rack Link + Matrix CV Bus)
    struct RackLinkBus
    {
        std::atomic<int>   activeInstances { 0 };
        std::atomic<float> masterSubRms    { 0.0f };
        std::atomic<float> masterPhase     { 1.0f };
        std::atomic<float> cvLfoValue      { 0.0f };
        std::atomic<float> envFollower     { 0.0f };
        std::atomic<float> lufsMomentary   { -60.0f };
        std::atomic<float> truePeakDb      { -60.0f };
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
                lowShelf.state  = juce::dsp::IIR::Coefficients<float>::makeLowShelf (sampleRate, 85.0f, 0.7f, 1.08f);
                midPeak.state   = juce::dsp::IIR::Coefficients<float>::makePeakFilter (sampleRate, 320.0f, 1.4f, 0.88f);
                highShelf.state = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sampleRate, 8500.0f, 0.7f, 1.12f);
                tapMsL = 7.2f; tapMsR = 9.4f;
            }
            else if (profileIndex == 2) // '69 Lincoln Cabin
            {
                lowShelf.state  = juce::dsp::IIR::Coefficients<float>::makeLowShelf (sampleRate, 110.0f, 0.8f, 1.25f);
                midPeak.state   = juce::dsp::IIR::Coefficients<float>::makePeakFilter (sampleRate, 650.0f, 1.1f, 0.82f);
                highShelf.state = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sampleRate, 5500.0f, 0.7f, 0.85f);
                tapMsL = 3.1f; tapMsR = 4.6f;
            }
            else if (profileIndex == 3) // Club System
            {
                lowShelf.state  = juce::dsp::IIR::Coefficients<float>::makeLowShelf (sampleRate, 65.0f, 0.9f, 1.38f);
                midPeak.state   = juce::dsp::IIR::Coefficients<float>::makePeakFilter (sampleRate, 450.0f, 1.2f, 0.85f);
                highShelf.state = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sampleRate, 10000.0f, 0.7f, 1.15f);
                tapMsL = 14.5f; tapMsR = 19.2f;
            }
            else
            {
                lowShelf.state  = juce::dsp::IIR::Coefficients<float>::makeLowShelf (sampleRate, 80.0f, 0.7f, 1.0f);
                midPeak.state   = juce::dsp::IIR::Coefficients<float>::makePeakFilter (sampleRate, 1000.0f, 1.0f, 1.0f);
                highShelf.state = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sampleRate, 8000.0f, 0.7f, 1.0f);
                tapMsL = 0.0f; tapMsR = 0.0f;
            }

            roomNotch.state = juce::dsp::IIR::Coefficients<float>::makePeakFilter (sampleRate, capturedModeHz, 4.0f, 0.72f);
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
            roomMatcher.prepare (spec);
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
        }

        RoomMatcherEngine& getRoomMatcher() { return roomMatcher; }
        const RoomMatcherEngine& getRoomMatcher() const { return roomMatcher; }

        void process (const juce::String& moduleId, juce::AudioBuffer<float>& buffer, const ProcessParams& p, RackLinkBus& bus)
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
            if (moduleId == "TheMatrix")
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
            airShelf.state = juce::dsp::IIR::Coefficients<float>::makeHighShelf (
                sampleRate, 6500.0f, 0.7f, juce::Decibels::decibelsToGain (airDb));

            const float depthCutoff = 20000.0f - effectiveDepth * 14000.0f;
            depthLowPass.state = juce::dsp::IIR::Coefficients<float>::makeLowPass (
                sampleRate, std::clamp (depthCutoff, 1800.0f, 20000.0f));

            deRumbleHPF.state = juce::dsp::IIR::Coefficients<float>::makeHighPass (
                sampleRate, 18.0f + effectiveDepth * 24.0f);

            auto applyClipper = [&] (float x, float drive) -> float
            {
                if (p.clipperShape == 2) // Hard Brickwall
                    return std::clamp (x * drive, -0.944f, 0.944f);
                if (p.clipperShape == 1) // Warm Tube (asymmetric)
                    return std::tanh (x * drive + 0.08f * x * x) / std::max (1.0f, std::tanh (drive));
                return std::tanh (x * drive) / std::max (1.0f, std::tanh (drive)); // Soft Knee
            };

            if (moduleId == "TheBunker")
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

                const float drive = 1.0f + effectiveDepth * 3.5f;
                for (int i = 0; i < numSamples; ++i)
                {
                    const float monoSub = 0.5f * (lowBandBuffer.getSample (0, i) + lowBandBuffer.getSample (1, i));
                    const float satSub = applyClipper (monoSub, drive);
                    buffer.setSample (0, i, satSub + highBandBuffer.getSample (0, i));
                    buffer.setSample (1, i, satSub + highBandBuffer.getSample (1, i));
                }
            }
            else if (moduleId == "TheLoft" || moduleId == "TheAether")
            {
                juce::dsp::AudioBlock<float> blk (buffer);
                juce::dsp::ProcessContextReplacing<float> ctx (blk);
                airShelf.process (ctx);
                const float airDrive = 1.0f + effectiveDepth * (moduleId == "TheAether" ? 2.1f : 1.6f);
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto* data = buffer.getWritePointer (ch);
                    for (int i = 0; i < numSamples; ++i)
                        data[i] = 0.7f * data[i] + 0.3f * std::tanh (data[i] * airDrive);
                }
            }
            else if (moduleId == "TheAnchor" || moduleId == "TheBridge" || moduleId == "TheCrown")
            {
                const float drive = 1.0f + (effectiveDepth + p.regenThrottle * 0.4f) * (moduleId == "TheAnchor" ? 2.5f : (moduleId == "TheCrown" ? 1.8f : 1.4f));
                if (moduleId == "TheCrown")
                {
                    juce::dsp::AudioBlock<float> blk (buffer);
                    juce::dsp::ProcessContextReplacing<float> ctx (blk);
                    airShelf.process (ctx);
                }
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto* data = buffer.getWritePointer (ch);
                    for (int i = 0; i < numSamples; ++i)
                        data[i] = applyClipper (data[i], drive);
                }
            }
            else if (moduleId == "TheMosaic" || moduleId == "Chimera")
            {
                juce::dsp::AudioBlock<float> blk (buffer);
                juce::dsp::ProcessContextReplacing<float> ctx (blk);
                airShelf.process (ctx);
                const float morphDrive = 1.0f + effectiveDepth * (moduleId == "Chimera" ? 2.4f : 2.0f);
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto* data = buffer.getWritePointer (ch);
                    for (int i = 0; i < numSamples; ++i)
                        data[i] = 0.6f * data[i] + 0.4f * applyClipper (data[i], morphDrive);
                }
            }
            else if (moduleId == "TheStage" || moduleId == "TheLand" || moduleId == "TheOrbit" || moduleId == "TheVoid")
            {
                juce::dsp::AudioBlock<float> blk (buffer);
                juce::dsp::ProcessContextReplacing<float> ctx (blk);
                airShelf.process (ctx);
                depthLowPass.process (ctx);
            }
            else if (moduleId == "Innerspace")
            {
                juce::dsp::AudioBlock<float> blk (buffer);
                juce::dsp::ProcessContextReplacing<float> ctx (blk);
                depthLowPass.process (ctx);
            }

            // Automated Dialogue Ducking (gentle mid-band duck when enabled)
            if (p.dialogueDuck)
            {
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto* data = buffer.getWritePointer (ch);
                    for (int i = 0; i < numSamples; ++i)
                        data[i] *= 0.82f;
                }
            }

            // Predictive Transient Sculpting (Scope / global peak tamer)
            if (p.transientSculpt)
            {
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto* data = buffer.getWritePointer (ch);
                    for (int i = 0; i < numSamples; ++i)
                    {
                        const float ax = std::abs (data[i]);
                        if (ax > 0.72f)
                            data[i] = std::copysign (0.72f + 0.28f * std::tanh ((ax - 0.72f) * 2.5f), data[i]);
                    }
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
    };
}
