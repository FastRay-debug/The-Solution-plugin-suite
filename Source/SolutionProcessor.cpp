#include "SolutionProcessor.h"
#if ! SOLUTION_HEADLESS
 #include "SolutionEditor.h"
#endif
#include <cmath>
#include <algorithm>

SolutionAudioProcessor::SolutionAudioProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, &undoManager, "SolutionState", createParameterLayout())
{
    rackLinkBus->activeInstances.fetch_add (1, std::memory_order_relaxed);
    for (auto& slot : ccToParamIndex)
        slot.store (-1, std::memory_order_relaxed);

    learnableParamIds = {
        "stage_azimuth", "stage_elevation", "stage_depth", "stage_haas_itd",
        "stage_width", "stage_anchor_hz", "stage_ms_mid", "stage_ms_side",
        "room_match_mix", "global_mix", "ultra_wide", "cv_mod_depth"
    };
}

SolutionAudioProcessor::~SolutionAudioProcessor()
{
    rackLinkBus->activeInstances.fetch_sub (1, std::memory_order_relaxed);
}

void SolutionAudioProcessor::armMidiLearn (const juce::String& paramId)
{
    for (size_t i = 0; i < learnableParamIds.size(); ++i)
    {
        if (learnableParamIds[i] == paramId)
        {
            learnArmedIndex.store (static_cast<int> (i), std::memory_order_relaxed);
            return;
        }
    }
    learnArmedIndex.store (2, std::memory_order_relaxed); // Default to stage_depth
}

juce::AudioProcessorValueTreeState::ParameterLayout SolutionAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<juce::AudioParameterFloat>  ("stage_azimuth",       "Azimuth",            juce::NormalisableRange<float> (-180.0f, 180.0f, 0.1f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  ("stage_elevation",     "Elevation",          juce::NormalisableRange<float> (-90.0f,  90.0f,  0.1f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  ("stage_depth",         "Depth / Drive",      juce::NormalisableRange<float> (0.0f,    1.0f,   0.001f), 0.25f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  ("stage_haas_itd",      "Haas ITD (ms)",      juce::NormalisableRange<float> (0.0f,    35.0f,  0.1f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  ("stage_width",         "Stage Width",        juce::NormalisableRange<float> (0.0f,    2.0f,   0.01f), 1.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  ("stage_anchor_hz",     "Low-End Anchor",     juce::NormalisableRange<float> (20.0f,   250.0f, 1.0f), 90.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  ("stage_ms_mid",        "Split-Core Mid",     juce::NormalisableRange<float> (-12.0f,  12.0f,  0.1f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  ("stage_ms_side",       "Split-Core Side",    juce::NormalisableRange<float> (-12.0f,  12.0f,  0.1f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  ("room_match_mix",      "Room Matcher Mix",   juce::NormalisableRange<float> (0.0f,    1.0f,   0.01f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterInt>    ("room_profile",        "Room Profile",       0, 3, 1));
    params.push_back (std::make_unique<juce::AudioParameterInt>    ("patch_order",         "Rear Patch Order",   0, 2, 0));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  ("global_mix",          "Global Wet/Dry",     juce::NormalisableRange<float> (0.0f,    1.0f,   0.01f), 1.0f));
    params.push_back (std::make_unique<juce::AudioParameterBool>   ("stage_ceiling_guard", "Sub-Ceiling Guard (-0.5 dBFS)", true));
    params.push_back (std::make_unique<juce::AudioParameterBool>   ("transient_sculpt",    "Predictive Transient Sculpting", false));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  ("ultra_wide",          "Ultra Wide (Haas-Kempf)", 0.0f, 1.0f, 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterBool>   ("tape_cal_tone",       "Tape Calibration Tone", false));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  ("cv_mod_depth",        "Matrix CV Mod Depth", 0.0f, 1.0f, 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterInt>    ("clipper_shape",       "Multiband Clipper Shape", 0, 2, 0));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  ("regen_throttle",      "Regen Throttle", 0.0f, 1.0f, 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterBool>   ("dialogue_duck",       "Dialogue Ducking", false));

    return { params.begin(), params.end() };
}

void SolutionAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec { sampleRate, static_cast<juce::uint32> (samplesPerBlock), 2 };
    moduleEngine.prepare (spec);
    dryCopy.setSize (2, samplesPerBlock, false, true, true);
}

void SolutionAudioProcessor::releaseResources()
{
    moduleEngine.reset();
}

bool SolutionAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainInputChannelSet()  == juce::AudioChannelSet::stereo();
}

void SolutionAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    // Handle MIDI CC Learn & hardware controller automation
    for (const auto meta : midiMessages)
    {
        const auto msg = meta.getMessage();
        if (msg.isController())
        {
            const int cc = msg.getControllerNumber();
            const float norm = static_cast<float> (msg.getControllerValue()) / 127.0f;
            const int armed = learnArmedIndex.exchange (-1, std::memory_order_relaxed);
            if (armed >= 0 && cc >= 0 && cc < 128)
            {
                ccToParamIndex[static_cast<size_t> (cc)].store (armed, std::memory_order_relaxed);
                lastLearnedCc.store (cc, std::memory_order_relaxed);
            }
            const int mappedIdx = (cc >= 0 && cc < 128) ? ccToParamIndex[static_cast<size_t> (cc)].load (std::memory_order_relaxed) : -1;
            if (mappedIdx >= 0 && mappedIdx < static_cast<int> (learnableParamIds.size()))
            {
                if (auto* param = apvts.getParameter (learnableParamIds[static_cast<size_t> (mappedIdx)]))
                    param->setValueNotifyingHost (norm);
            }
        }
    }

    const int numSamples = buffer.getNumSamples();
    if (buffer.getNumChannels() < 2 || numSamples == 0)
        return;

    if (dryCopy.getNumSamples() < numSamples)
        dryCopy.setSize (2, numSamples, false, false, true);

    for (int ch = 0; ch < 2; ++ch)
        dryCopy.copyFrom (ch, 0, buffer, ch, 0, numSamples);

    SolutionDSP::ProcessParams p;
    p.azimuthDeg      = apvts.getRawParameterValue ("stage_azimuth")->load();
    p.elevationDeg    = apvts.getRawParameterValue ("stage_elevation")->load();
    p.depth           = apvts.getRawParameterValue ("stage_depth")->load();
    p.haasMs          = apvts.getRawParameterValue ("stage_haas_itd")->load();
    p.width           = apvts.getRawParameterValue ("stage_width")->load();
    p.anchorHz        = apvts.getRawParameterValue ("stage_anchor_hz")->load();
    p.midDb           = apvts.getRawParameterValue ("stage_ms_mid")->load();
    p.sideDb          = apvts.getRawParameterValue ("stage_ms_side")->load();
    p.roomMix         = apvts.getRawParameterValue ("room_match_mix")->load();
    p.roomProfile     = static_cast<int> (apvts.getRawParameterValue ("room_profile")->load());
    p.patchOrder      = static_cast<int> (apvts.getRawParameterValue ("patch_order")->load());
    p.wetMix          = apvts.getRawParameterValue ("global_mix")->load();
    p.transientSculpt = apvts.getRawParameterValue ("transient_sculpt")->load() > 0.5f;
    p.ultraWide       = apvts.getRawParameterValue ("ultra_wide")->load();
    p.tapeCalTone     = apvts.getRawParameterValue ("tape_cal_tone")->load() > 0.5f;
    p.cvModDepth      = apvts.getRawParameterValue ("cv_mod_depth")->load();
    p.clipperShape    = static_cast<int> (apvts.getRawParameterValue ("clipper_shape")->load());
    p.regenThrottle   = apvts.getRawParameterValue ("regen_throttle")->load();
    p.dialogueDuck    = apvts.getRawParameterValue ("dialogue_duck")->load() > 0.5f;

    moduleEngine.process (moduleId, buffer, p, *rackLinkBus);

    const bool ceilingOn   = apvts.getRawParameterValue ("stage_ceiling_guard")->load() > 0.5f;
    const float ceilingLin = juce::Decibels::decibelsToGain (-0.5f);

    auto* left  = buffer.getWritePointer (0);
    auto* right = buffer.getWritePointer (1);
    const auto* dryL = dryCopy.getReadPointer (0);
    const auto* dryR = dryCopy.getReadPointer (1);

    double sumL2 = 0.0, sumR2 = 0.0, sumLR = 0.0;
    float maxPeak = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        float outL = p.tapeCalTone ? left[i]  : (dryL[i] * (1.0f - p.wetMix) + left[i]  * p.wetMix);
        float outR = p.tapeCalTone ? right[i] : (dryR[i] * (1.0f - p.wetMix) + right[i] * p.wetMix);

        if (ceilingOn)
        {
            outL = std::clamp (outL, -ceilingLin, ceilingLin);
            outR = std::clamp (outR, -ceilingLin, ceilingLin);
        }

        left[i]  = outL;
        right[i] = outR;

        sumL2 += static_cast<double> (outL) * outL;
        sumR2 += static_cast<double> (outR) * outR;
        sumLR += static_cast<double> (outL) * outR;
        maxPeak = std::max (maxPeak, std::max (std::abs (outL), std::abs (outR)));
    }

    const float rmsL = static_cast<float> (std::sqrt (sumL2 / numSamples));
    const float rmsR = static_cast<float> (std::sqrt (sumR2 / numSamples));
    const double denom = std::sqrt (sumL2 * sumR2) + 1e-12;
    const float corr = static_cast<float> (std::clamp (sumLR / denom, -1.0, 1.0));
    const float peakDbVal = juce::Decibels::gainToDecibels (maxPeak, -60.0f);
    const float lufsVal = juce::Decibels::gainToDecibels (0.5f * (rmsL + rmsR), -60.0f) - 0.691f;

    rmsLeft.store (rmsL, std::memory_order_relaxed);
    rmsRight.store (rmsR, std::memory_order_relaxed);
    phaseCorrelation.store (corr, std::memory_order_relaxed);
    peakDb.store (peakDbVal, std::memory_order_relaxed);
    lufsMomentary.store (lufsVal, std::memory_order_relaxed);

    rackLinkBus->masterSubRms.store (0.5f * (rmsL + rmsR), std::memory_order_relaxed);
    rackLinkBus->masterPhase.store (corr, std::memory_order_relaxed);
    rackLinkBus->lufsMomentary.store (lufsVal, std::memory_order_relaxed);
    rackLinkBus->truePeakDb.store (peakDbVal, std::memory_order_relaxed);
}

juce::AudioProcessorEditor* SolutionAudioProcessor::createEditor()
{
   #if SOLUTION_HEADLESS
    return nullptr;     // test builds have no UI
   #else
    return new SolutionAudioProcessorEditor (*this);
   #endif
}

void SolutionAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void SolutionAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState != nullptr && xmlState->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xmlState));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SolutionAudioProcessor();
}
