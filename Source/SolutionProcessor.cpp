#include "SolutionProcessor.h"
#if ! SOLUTION_HEADLESS
 #include "SolutionEditor.h"
 #include "BinaryData.h"
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
    rackSlot = rackLinkBus->claimSlot (static_cast<int> (moduleId));
    for (auto& slot : ccToParamIndex)
        slot.store (-1, std::memory_order_relaxed);

    for (auto* p : getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
            apvts.addParameterListener (ranged->getParameterID(), this);

    startTimerHz (5);     // picks up alignment requests from other instances

    learnableParamIds = {
        "stage_azimuth", "stage_elevation", "stage_depth", "stage_haas_itd",
        "stage_width", "stage_anchor_hz", "stage_ms_mid", "stage_ms_side",
        "room_match_mix", "global_mix", "ultra_wide", "cv_mod_depth",
        "glue_amount", "link_duck_depth"
    };
}

SolutionAudioProcessor::~SolutionAudioProcessor()
{
    stopTimer();
    remote.reset();      // stop serving before anything it reads goes away
    for (auto* p : getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
            apvts.removeParameterListener (ranged->getParameterID(), this);
    rackLinkBus->releaseSlot (rackSlot);
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

    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "stage_azimuth", 1 },        "Azimuth",            juce::NormalisableRange<float> (-180.0f, 180.0f, 0.1f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "stage_elevation", 1 },      "Elevation",          juce::NormalisableRange<float> (-90.0f,  90.0f,  0.1f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "stage_depth", 1 },          "Depth / Drive",      juce::NormalisableRange<float> (0.0f,    1.0f,   0.001f), 0.25f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "stage_haas_itd", 1 },       "Haas ITD (ms)",      juce::NormalisableRange<float> (0.0f,    35.0f,  0.1f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "stage_width", 1 },          "Stage Width",        juce::NormalisableRange<float> (0.0f,    2.0f,   0.01f), 1.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "stage_anchor_hz", 1 },      "Low-End Anchor",     juce::NormalisableRange<float> (20.0f,   250.0f, 1.0f), 90.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "stage_ms_mid", 1 },         "Split-Core Mid",     juce::NormalisableRange<float> (-12.0f,  12.0f,  0.1f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "stage_ms_side", 1 },        "Split-Core Side",    juce::NormalisableRange<float> (-12.0f,  12.0f,  0.1f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "room_match_mix", 1 },       "Room Matcher Mix",   juce::NormalisableRange<float> (0.0f,    1.0f,   0.01f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterInt>    (juce::ParameterID { "room_profile", 1 },         "Room Profile",       0, 3, 1));
    params.push_back (std::make_unique<juce::AudioParameterInt>    (juce::ParameterID { "patch_order", 1 },          "Rear Patch Order",   0, 2, 0));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "global_mix", 1 },           "Global Wet/Dry",     juce::NormalisableRange<float> (0.0f,    1.0f,   0.01f), 1.0f));
    params.push_back (std::make_unique<juce::AudioParameterBool>   (juce::ParameterID { "stage_ceiling_guard", 1 },  "Sub-Ceiling Guard (-0.5 dBFS)", true));
    params.push_back (std::make_unique<juce::AudioParameterBool>   (juce::ParameterID { "transient_sculpt", 1 },     "Predictive Transient Sculpting", false));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "ultra_wide", 1 },           "Ultra Wide (Haas-Kempf)", 0.0f, 1.0f, 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterBool>   (juce::ParameterID { "tape_cal_tone", 1 },        "Tape Calibration Tone", false));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "cv_mod_depth", 1 },         "Matrix CV Mod Depth", 0.0f, 1.0f, 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterInt>    (juce::ParameterID { "clipper_shape", 1 },        "Multiband Clipper Shape", 0, 2, 0));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "regen_throttle", 1 },       "Regen Throttle", 0.0f, 1.0f, 0.0f));
    // --- v14: Dynamic Quality Scaling
    params.push_back (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "dqs_mode", 14 }, "Quality (DQS)",
                          juce::StringArray { "Auto", "Locked 4x (best)", "Eco (lightest)" }, 0));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "dqs_threshold", 14 }, "DQS CPU Threshold (%)", juce::NormalisableRange<float> (50.0f, 95.0f, 1.0f), 80.0f));
    // --- v13: 3D stage - distance and motion paths
    {
        juce::NormalisableRange<float> range (0.5f, 40.0f, 0.01f);
        range.setSkewForCentre (4.0f);
        params.push_back (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "stage_distance", 13 }, "Distance (m)", range, 2.0f));
    }
    params.push_back (std::make_unique<juce::AudioParameterBool>   (juce::ParameterID { "motion_on", 13 }, "Motion Path", false));
    params.push_back (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "motion_rate", 13 }, "Motion Rate",
                          juce::StringArray { "1/4 bar", "1/2 bar", "1 bar", "2 bars", "4 bars", "8 bars", "16 bars" }, 4));
    // --- v12: Safe Bounce - monitoring tools switch themselves off during offline bounces
    params.push_back (std::make_unique<juce::AudioParameterBool>   (juce::ParameterID { "safe_bounce", 12 }, "Safe Bounce (auto-bypass monitoring tools)", true));
    // --- v11: Auto-Align (time + polarity between mics on the same source)
    params.push_back (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "align_role", 11 }, "Auto-Align Role",
                          juce::StringArray { "Off", "Reference", "Align to reference" }, 0));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "align_delay_ms", 11 }, "Align Delay (ms)", juce::NormalisableRange<float> (0.0f, 20.0f, 0.001f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterBool>   (juce::ParameterID { "align_invert", 11 }, "Align Polarity Invert", false));
    // --- v10: Unmask (frequency-specific make-room via Rack Link)
    params.push_back (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "unmask_role", 10 }, "Unmask Role",
                          juce::StringArray { "Off", "Priority (vocal / kick)", "Make room" }, 0));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "unmask_depth", 10 }, "Unmask Depth (dB)", juce::NormalisableRange<float> (0.0f, 12.0f, 0.1f), 6.0f));
    // --- v9: listening room library (monitoring)
    {
        juce::StringArray rooms { "Off" };
        for (const auto& room : SolutionDSP::listeningRooms()) rooms.add (room.name);
        params.push_back (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "listen_room", 9 }, "Listening Room", rooms, 0));
    }
    // --- v8: Pro Tricks
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "trim_in_db", 8 },     "Input Trim (dB)",  juce::NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "trim_out_db", 8 },    "Output Trim (dB)", juce::NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterBool>   (juce::ParameterID { "ab_bypass", 8 },      "A/B: Original (Loudness Matched)", false));
    params.push_back (std::make_unique<juce::AudioParameterBool>   (juce::ParameterID { "delta_listen", 8 },   "Delta: Hear What Changed", false));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "mono_lows_hz", 8 },   "Mono The Lows Below (Hz, 0 = off)", juce::NormalisableRange<float> (0.0f, 300.0f, 1.0f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterBool>   (juce::ParameterID { "phase_guard", 8 },    "Phase Guard (Mono-Safe Width)", false));
    {
        juce::StringArray services { "Off" };
        for (const auto& svc : SolutionDSP::streamingServices()) services.add (svc.name);
        params.push_back (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "stream_preview", 8 }, "Streaming Preview", services, 0));
    }
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "smooth_amount", 8 },  "Smooth (Harshness)", juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "phone_bass", 8 },     "Phone Bass", juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.0f));
    // --- v7: fixed 115 Hz mode notch - Off / On (original, always) / Auto (only when 115 Hz builds up)
    params.push_back (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "mode_notch", 7 },      "115 Hz Mode Notch",
                          juce::StringArray { "Off", "On", "Auto" }, 2));
    // --- v6: neural character
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "char_amount", 6 },     "Character Amount", juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 1.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "char_drive", 6 },      "Character Drive (dB)", juce::NormalisableRange<float> (-12.0f, 18.0f, 0.1f), 0.0f));
    // --- v5: stem focus
    params.push_back (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "stem_focus", 5 },      "Stem Focus",
                          juce::StringArray { "Off (whole mix)", "Drums / hits", "Tonal / sustained", "Vocal / centre" }, 0));
    params.push_back (std::make_unique<juce::AudioParameterBool>   (juce::ParameterID { "stem_listen", 5 },     "Stem Focus Listen", false));
    // --- v4: binaural headphone rendering
    params.push_back (std::make_unique<juce::AudioParameterBool>   (juce::ParameterID { "binaural_mode", 4 },     "Binaural Headphone Mode", false));
    // --- v3: measured room correction
    params.push_back (std::make_unique<juce::AudioParameterBool>   (juce::ParameterID { "room_correct_on", 3 },     "Room Correction", false));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "room_correct_amount", 3 }, "Room Correction Amount", juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 1.0f));
    // --- v2: Rack Link + Glue
    params.push_back (std::make_unique<juce::AudioParameterInt>    (juce::ParameterID { "link_group", 2 },        "Rack Link Group", 0, 4, 0,
                          juce::AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) { return v == 0 ? juce::String ("Off") : juce::String::charToString ((juce::juce_wchar) ('A' + v - 1)); })));
    params.push_back (std::make_unique<juce::AudioParameterBool>   (juce::ParameterID { "link_duck_send", 2 },    "Duck Others In Group", false));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "link_duck_depth", 2 },   "Link Duck Depth (dB)", juce::NormalisableRange<float> (0.0f, 18.0f, 0.1f), 6.0f));
    params.push_back (std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "glue_amount", 2 },       "Glue", juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.0f));
    params.push_back (std::make_unique<juce::AudioParameterBool>   (juce::ParameterID { "glue_follow_suite", 2 }, "Follow Suite Glue", false));
    params.push_back (std::make_unique<juce::AudioParameterBool>   (juce::ParameterID { "dialogue_duck", 1 },        "Dialogue Ducking", false));

    return { params.begin(), params.end() };
}

void SolutionAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    preparedSampleRate = sampleRate;
    juce::dsp::ProcessSpec spec { sampleRate, static_cast<juce::uint32> (samplesPerBlock), 2 };
    moduleEngine.prepare (spec);
    dryCopy.setSize (2, samplesPerBlock, false, true, true);
    dryDelay.prepare (2, moduleEngine.getLatencySamples());
    ceilingLimiter.prepare (sampleRate);
    glue.prepare (sampleRate);
    roomCorrector.prepare (sampleRate);
    binaural.prepare (sampleRate);
    monoLows.prepare (sampleRate, samplesPerBlock);
    listeningRoom.prepare (sampleRate);
    unmask.prepare (sampleRate);
    distanceStage.prepare (sampleRate);
    alignDelayLine.prepare ({ sampleRate, (juce::uint32) samplesPerBlock, 2 });
    alignDelayLine.setMaximumDelayInSamples ((int) std::ceil (0.021 * sampleRate) + 8);
    alignDelayLine.reset();
    smoother.prepare (sampleRate);
    phoneBass.prepare (sampleRate);
    abMatcher.prepare (sampleRate);
    for (auto* g : { &trimInGain, &trimOutGain, &previewGain }) g->reset (sampleRate, 0.05);
    measureOut.setSize (2, samplesPerBlock, false, true, true);
    duckKey.prepare (sampleRate);
    ducker.prepare (sampleRate);
    loudness.prepare (sampleRate);
    stemSplitter.prepare (sampleRate, samplesPerBlock);
    stemRest.setSize (2, samplesPerBlock, false, true, true);
    restDelay.prepare (2, moduleEngine.getLatencySamples());
    lastStemMode = static_cast<int> (apvts.getRawParameterValue ("stem_focus")->load());
    stemActive = lastStemMode != 0;
    updateLatencyNow();
}

void SolutionAudioProcessor::releaseResources()
{
    moduleEngine.reset();
    dryDelay.reset();
    ceilingLimiter.reset();
}

bool SolutionAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainInputChannelSet()  == juce::AudioChannelSet::stereo();
}

void SolutionAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    const auto blockStartTicks = juce::Time::getHighResolutionTicks();
    {
        // DQS: decide this block's oversampling from the suite's real-time load.
        float summed = 0.0f; int active = 0;
        for (const auto& sl : rackLinkBus->slots)
            if (sl.active.load (std::memory_order_relaxed)) { summed += sl.cpuLoad.load (std::memory_order_relaxed); ++active; }
        const float suite = SolutionDSP::DqsController::suiteLoad (summed, active, juce::SystemStats::getNumCpus());
        dqsSuiteLoad.store (suite, std::memory_order_relaxed);
        const int level = dqs.update (static_cast<int> (apvts.getRawParameterValue ("dqs_mode")->load()), isNonRealtime(), suite,
                                      apvts.getRawParameterValue ("dqs_threshold")->load() / 100.0f,
                                      buffer.getNumSamples() / std::max (1.0, preparedSampleRate));
        moduleEngine.setQualityLevel (level);
        dqsLevel.store (moduleEngine.getQualityLevel(), std::memory_order_relaxed);
    }

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

    // Safe Bounce: during an offline render the monitoring tools step aside (settings are kept).
    const bool bouncing = isNonRealtime() && apvts.getRawParameterValue ("safe_bounce")->load() > 0.5f;
    auto monitorOn = [&] (const char* id) { return apvts.getRawParameterValue (id)->load() > 0.5f; };
    {
        int mask = 0;
        if (monitorOn ("ab_bypass"))       mask |= monAB;
        if (monitorOn ("delta_listen"))    mask |= monDelta;
        if (monitorOn ("stream_preview"))  mask |= monStream;
        if (monitorOn ("listen_room"))     mask |= monRoom;
        if (monitorOn ("room_correct_on")) mask |= monCorrection;
        if (monitorOn ("stem_listen") && apvts.getRawParameterValue ("stem_focus")->load() > 0.5f) mask |= monStemSolo;
        if (monitorOn ("tape_cal_tone"))   mask |= monCalTone;
        if (monitorOn ("binaural_mode"))   mask |= monBinaural;
        monitorMask.store (mask, std::memory_order_relaxed);
        if (rackSlot >= 0) rackLinkBus->slots[(size_t) rackSlot].monitorMask.store (mask, std::memory_order_relaxed);
    }

    // Auto-Align: share the raw input (timeline-stamped) and apply this track's alignment.
    {
        const int alignRole = static_cast<int> (apvts.getRawParameterValue ("align_role")->load());
        if (rackSlot >= 0 && alignRole != 0)
        {
            long long t = ownTimeline;
            bool valid = true;
            if (auto* ph = getPlayHead())
            {
                valid = false;
                if (auto pos = ph->getPosition())
                    if (pos->getIsPlaying())
                        if (auto ts = pos->getTimeInSamples()) { t = *ts; valid = true; }
            }
            auto& slot = rackLinkBus->slots[(size_t) rackSlot];
            slot.alignRole.store (alignRole, std::memory_order_relaxed);
            if (valid)
            {
                const auto* l = buffer.getReadPointer (0); const auto* r = buffer.getReadPointer (1);
                constexpr int N = SolutionDSP::RackSlot::alignRingSize;
                for (int i = 0; i < numSamples; ++i) slot.alignRing[(size_t) (((t + i) % N + N) % N)] = 0.5f * (l[i] + r[i]);
                slot.alignRingEnd.store (t + numSamples, std::memory_order_release);
            }
        }
        else if (rackSlot >= 0)
            rackLinkBus->slots[(size_t) rackSlot].alignRole.store (0, std::memory_order_relaxed);
        ownTimeline += numSamples;

        const float delaySamples = apvts.getRawParameterValue ("align_delay_ms")->load() * 0.001f * (float) preparedSampleRate;
        const bool invert = apvts.getRawParameterValue ("align_invert")->load() > 0.5f;
        if (delaySamples > 0.001f || invert)
        {
            alignDelayLine.setDelay (delaySamples);
            const float sign = invert ? -1.0f : 1.0f;
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* d = buffer.getWritePointer (ch);
                for (int i = 0; i < numSamples; ++i) { alignDelayLine.pushSample (ch, d[i]); d[i] = sign * alignDelayLine.popSample (ch); }
            }
        }
    }

    // Auto gain staging: listen to the raw input, then the message thread sets the trims.
    if (autoStageState.load (std::memory_order_relaxed) == 1)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto* d = buffer.getReadPointer (ch);
            for (int i = 0; i < numSamples; ++i) autoStageSum += (double) d[i] * d[i];
        }
        autoStageCount += numSamples;
        if (autoStageCount >= (long long) (3.0 * preparedSampleRate))
        {
            autoStageResultDb.store ((float) (10.0 * std::log10 (autoStageSum / (2.0 * (double) autoStageCount) + 1e-20)));
            autoStageState.store (2, std::memory_order_release);
        }
    }

    // Input trim (gain staging into the module).
    trimInGain.setTargetValue (juce::Decibels::decibelsToGain (apvts.getRawParameterValue ("trim_in_db")->load()));
    if (trimInGain.isSmoothing() || trimInGain.getTargetValue() != 1.0f)
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = trimInGain.getNextValue();
            buffer.getWritePointer (0)[i] *= g; buffer.getWritePointer (1)[i] *= g;
        }

    for (int ch = 0; ch < 2; ++ch)
        dryCopy.copyFrom (ch, 0, buffer, ch, 0, numSamples);

    // Room measurement: record the raw input (mic) and prepare the sweep for the output.
    const bool measuring = isRoomCapturing() && measureOut.getNumSamples() >= numSamples;
    if (measuring)
        measurement.process (dryCopy.getReadPointer (0), dryCopy.getReadPointer (1),
                             measureOut.getWritePointer (0), measureOut.getWritePointer (1), numSamples);

    // Rack Link: publish this instance's group / duck key (taken from its input).
    const int linkGroup   = static_cast<int> (apvts.getRawParameterValue ("link_group")->load());
    const bool duckSender = apvts.getRawParameterValue ("link_duck_send")->load() > 0.5f;
    const float keyLevel  = duckKey.process (dryCopy.getReadPointer (0), dryCopy.getReadPointer (1), numSamples);
    if (rackSlot >= 0)
    {
        auto& slot = rackLinkBus->slots[(size_t) rackSlot];
        slot.group.store (linkGroup, std::memory_order_relaxed);
        slot.duckSource.store (duckSender && linkGroup > 0, std::memory_order_relaxed);
        slot.keyLevel.store (duckSender ? keyLevel : 0.0f, std::memory_order_relaxed);
        slot.heartbeat.fetch_add (1, std::memory_order_relaxed);

        // Unmask: a priority track publishes its band levels (from its input).
        const int role = static_cast<int> (apvts.getRawParameterValue ("unmask_role")->load());
        slot.unmaskRole.store (role, std::memory_order_relaxed);
        {   // every track publishes its band levels: Unmask uses the priority's, the 3D stage colours and clash detection use all
            std::array<float, SolutionDSP::Unmask::numBands> levels;
            unmask.measure (dryCopy.getReadPointer (0), dryCopy.getReadPointer (1), numSamples, levels);
            double wsum = 0.0, psum = 0.0;
            for (size_t b = 0; b < levels.size(); ++b)
            {
                slot.bandDb[b].store (levels[b], std::memory_order_relaxed);
                const double pw = std::pow (10.0, levels[b] / 10.0);
                wsum += pw * std::log2 (SolutionDSP::Unmask::centres[b]); psum += pw;
            }
            if (psum > 1e-9) slot.toneHz.store ((float) std::pow (2.0, wsum / psum), std::memory_order_relaxed);
        }
    }

    // Stem Focus: split the input; the module processes only the focus stem, the rest passes through.
    const int stemMode = static_cast<int> (apvts.getRawParameterValue ("stem_focus")->load());
    if (stemMode != lastStemMode)
    {
        if ((stemMode != 0) != (lastStemMode != 0)) { stemSplitter.reset(); restDelay.reset(); stemActive = stemMode != 0; triggerAsyncUpdate(); }
        lastStemMode = stemMode;
    }
    const bool stemOn = stemMode != 0 && stemRest.getNumSamples() >= numSamples;
    const bool stemListen = stemOn && monitorOn ("stem_listen") && ! bouncing;
    if (stemOn)
    {
        stemSplitter.process (buffer.getWritePointer (0), buffer.getWritePointer (1),
                              stemRest.getWritePointer (0), stemRest.getWritePointer (1), numSamples,
                              static_cast<SolutionDSP::StemSplitter::Mode> (stemMode));
        for (int ch = 0; ch < 2; ++ch)          // dry = focus + rest = the input, delayed like the wet path
        {
            dryCopy.copyFrom (ch, 0, buffer, ch, 0, numSamples);
            dryCopy.addFrom (ch, 0, stemRest, ch, 0, numSamples);
        }
        juce::AudioBuffer<float> restView (stemRest.getArrayOfWritePointers(), 2, numSamples);
        restDelay.process (restView, numSamples);
    }

    dryDelay.process (dryCopy, numSamples);

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
    p.tapeCalTone     = monitorOn ("tape_cal_tone") && ! bouncing;
    p.cvModDepth      = apvts.getRawParameterValue ("cv_mod_depth")->load();
    p.clipperShape    = static_cast<int> (apvts.getRawParameterValue ("clipper_shape")->load());
    p.regenThrottle   = apvts.getRawParameterValue ("regen_throttle")->load();
    p.dialogueDuck    = apvts.getRawParameterValue ("dialogue_duck")->load() > 0.5f;
    p.modeNotch       = static_cast<int> (apvts.getRawParameterValue ("mode_notch")->load());

    // 3D stage: distance, and the motion path (locked to the host's bars when it's playing).
    float stageDistance = apvts.getRawParameterValue ("stage_distance")->load();
    if (apvts.getRawParameterValue ("motion_on")->load() > 0.5f)
    {
        bool playing = false; double ppq = 0.0, beatsPerBar = 4.0;
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
            {
                playing = pos->getIsPlaying();
                if (auto q = pos->getPpqPosition()) ppq = *q; else playing = false;
                if (auto ts = pos->getTimeSignature()) beatsPerBar = ts->numerator * 4.0 / std::max (1, ts->denominator);
            }
        const int rate = static_cast<int> (apvts.getRawParameterValue ("motion_rate")->load());
        const double bars = SolutionDSP::MotionPlayer::rateBars[(size_t) juce::jlimit (0, 6, rate)];
        SolutionDSP::MotionPoint mp;
        if (motion.evaluate (SolutionDSP::MotionPlayer::phaseFor (playing, ppq, beatsPerBar, bars, motionFreeSeconds), mp))
        {
            p.azimuthDeg = mp.az; p.elevationDeg = juce::jlimit (-90.0f, 90.0f, mp.el); stageDistance = mp.dist;
        }
        motionFreeSeconds += numSamples / std::max (1.0, preparedSampleRate);
    }
    animAz.store (p.azimuthDeg, std::memory_order_relaxed);
    animEl.store (p.elevationDeg, std::memory_order_relaxed);
    animDist.store (stageDistance, std::memory_order_relaxed);
    if (rackSlot >= 0)
    {
        auto& slot = rackLinkBus->slots[(size_t) rackSlot];
        slot.posAz.store (p.azimuthDeg, std::memory_order_relaxed); slot.posEl.store (p.elevationDeg, std::memory_order_relaxed);
        slot.posDist.store (stageDistance, std::memory_order_relaxed); slot.posWidth.store (p.width, std::memory_order_relaxed);
    }

    // Binaural mode places the sound around the head itself, so the engine's pan/Haas steps aside.
    const bool binauralOn = apvts.getRawParameterValue ("binaural_mode")->load() > 0.5f;
    const float binauralAz = p.azimuthDeg, binauralWidth = p.width;
    if (binauralOn) { p.azimuthDeg = 0.0f; p.haasMs = 0.0f; }

    if (stemListen)
        p.wetMix = 1.0f;         // listen mode: hear exactly what's being targeted, unprocessed
    else
    {
        moduleEngine.process (moduleId, buffer, p, *rackLinkBus);
        if (! p.tapeCalTone)      // neural Character (only does anything once a model is chosen)
        {
            character.process (buffer.getWritePointer (0), buffer.getWritePointer (1), numSamples,
                               apvts.getRawParameterValue ("char_drive")->load(), apvts.getRawParameterValue ("char_amount")->load());
            smoother.process (buffer.getWritePointer (0), buffer.getWritePointer (1), numSamples, apvts.getRawParameterValue ("smooth_amount")->load());
            smoothDb.store (smoother.getReductionDb(), std::memory_order_relaxed);
            phoneBass.process (buffer.getWritePointer (0), buffer.getWritePointer (1), numSamples, apvts.getRawParameterValue ("phone_bass")->load());
            distanceStage.process (buffer.getWritePointer (0), buffer.getWritePointer (1), numSamples, stageDistance);   // real distance
        }
    }
    if (stemOn && ! stemListen)
        for (int ch = 0; ch < 2; ++ch)
            buffer.addFrom (ch, 0, stemRest, ch, 0, numSamples);   // processed focus + untouched rest

    const bool ceilingOn   = apvts.getRawParameterValue ("stage_ceiling_guard")->load() > 0.5f;
    const float ceilingLin = juce::Decibels::decibelsToGain ((float) SOLUTION_CEILING_DBFS);

    auto* left  = buffer.getWritePointer (0);
    auto* right = buffer.getWritePointer (1);
    const auto* dryL = dryCopy.getReadPointer (0);
    const auto* dryR = dryCopy.getReadPointer (1);

    if (! p.tapeCalTone)
        for (int i = 0; i < numSamples; ++i)
        {
            left[i]  = dryL[i] * (1.0f - p.wetMix) + left[i]  * p.wetMix;
            right[i] = dryR[i] * (1.0f - p.wetMix) + right[i] * p.wetMix;
        }

    // Mono the lows + Phase Guard.
    if (! p.tapeCalTone)
    {
        monoLows.process (left, right, numSamples, apvts.getRawParameterValue ("mono_lows_hz")->load(),
                          apvts.getRawParameterValue ("phase_guard")->load() > 0.5f);
        guardDb.store (monoLows.getGuardReductionDb(), std::memory_order_relaxed);
    }

    // Rack Link ducking: dip when another source in my group is playing (no routing needed).
    if (! p.tapeCalTone)
    {
        // Unmask: make room only where the group's priority track is loud right now.
        if (linkGroup > 0 && static_cast<int> (apvts.getRawParameterValue ("unmask_role")->load()) == 2)
        {
            std::array<float, SolutionDSP::Unmask::numBands> priority;
            rackLinkBus->groupPriorityBands (linkGroup, rackSlot, priority);
            unmask.apply (left, right, numSamples, priority, apvts.getRawParameterValue ("unmask_depth")->load());
        }
        for (size_t b = 0; b < unmaskDipDb.size(); ++b) unmaskDipDb[b].store (unmask.getDipsDb()[b], std::memory_order_relaxed);

        const float key = (linkGroup > 0 && ! duckSender) ? rackLinkBus->groupKey (linkGroup, rackSlot) : 0.0f;
        ducker.process (left, right, numSamples, key, apvts.getRawParameterValue ("link_duck_depth")->load());
        duckGainReductionDb.store (ducker.getGainReductionDb(), std::memory_order_relaxed);

        // Glue: The Scope sets the suite-wide amount; other modules can follow it.
        const float localGlue = apvts.getRawParameterValue ("glue_amount")->load();
        if (moduleId == SolutionDSP::ModuleId::Scope)
            rackLinkBus->suiteGlue.store (localGlue, std::memory_order_relaxed);
        const bool followSuite = apvts.getRawParameterValue ("glue_follow_suite")->load() > 0.5f;
        glue.process (left, right, numSamples, followSuite ? rackLinkBus->suiteGlue.load (std::memory_order_relaxed) : localGlue);
        glueGainReductionDb.store (glue.getGainReductionDb(), std::memory_order_relaxed);
    }

    // A/B (loudness-matched original) and Delta (only what changed). Monitoring aids.
    abMatcher.measure (left, right, dryL, dryR, numSamples);
    const float matchDb = abMatcher.update (numSamples);
    abMatchDb.store (matchDb, std::memory_order_relaxed);
    const bool deltaOn = monitorOn ("delta_listen") && ! p.tapeCalTone && ! bouncing;
    const bool abOn = monitorOn ("ab_bypass") && ! p.tapeCalTone && ! bouncing;
    if (deltaOn)
        for (int i = 0; i < numSamples; ++i) { left[i] -= dryL[i]; right[i] -= dryR[i]; }
    else if (abOn)
    {
        const float g = juce::Decibels::decibelsToGain (matchDb);
        for (int i = 0; i < numSamples; ++i) { left[i] = dryL[i] * g; right[i] = dryR[i] * g; }
    }

    // Monitoring stages (after A/B and Delta so both sides are heard the same way).
    if (! p.tapeCalTone)
        listeningRoom.process (left, right, numSamples, bouncing ? -1 : static_cast<int> (apvts.getRawParameterValue ("listen_room")->load()) - 1);
    if (binauralOn && ! p.tapeCalTone)
        binaural.process (left, right, numSamples, binauralAz, p.elevationDeg, binauralWidth);

    // Measured room correction (for your monitoring chain).
    roomCorrector.process (left, right, numSamples,
                           apvts.getRawParameterValue ("room_correct_amount")->load(),
                           monitorOn ("room_correct_on") && ! p.tapeCalTone && ! bouncing);

    // Output trim.
    trimOutGain.setTargetValue (juce::Decibels::decibelsToGain (apvts.getRawParameterValue ("trim_out_db")->load()));
    if (trimOutGain.isSmoothing() || trimOutGain.getTargetValue() != 1.0f)
        for (int i = 0; i < numSamples; ++i) { const float g = trimOutGain.getNextValue(); left[i] *= g; right[i] *= g; }

    // True-peak ceiling guard (look-ahead limiter; always in the path so latency never changes).
    ceilingLimiter.process (buffer, numSamples, ceilingLin, ceilingOn && ! deltaOn);
    limiterGainReductionDb.store (ceilingLimiter.getGainReductionDb(), std::memory_order_relaxed);
    loudness.process (left, right, numSamples);

    double sumL2 = 0.0, sumR2 = 0.0, sumLR = 0.0;
    float maxPeak = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        const float outL = left[i], outR = right[i];
        sumL2 += static_cast<double> (outL) * outL;
        sumR2 += static_cast<double> (outR) * outR;
        sumLR += static_cast<double> (outL) * outR;
        maxPeak = std::max (maxPeak, std::max (std::abs (outL), std::abs (outR)));
    }

    if (measuring)    // during a measurement the speakers get only the sweep
        for (int ch = 0; ch < 2; ++ch)
            buffer.copyFrom (ch, 0, measureOut, ch, 0, numSamples);
    else
    {
        assistant.tap.push (left, right, numSamples);     // Mix Assistant listens to the final output
        scopeTap.push (left, right, numSamples);          // real scopes in the UI

        // Streaming preview: play it back the way a service's loudness normalisation would.
        const int service = bouncing ? 0 : static_cast<int> (apvts.getRawParameterValue ("stream_preview")->load());
        float previewDb = 0.0f;
        if (service > 0)
            previewDb = SolutionDSP::streamingGainDb (SolutionDSP::streamingServices()[(size_t) (service - 1)],
                                                      loudness.integrated.load (std::memory_order_relaxed), loudness.truePeakDb.load (std::memory_order_relaxed));
        streamGainDb.store (previewDb, std::memory_order_relaxed);
        previewGain.setTargetValue (juce::Decibels::decibelsToGain (previewDb));
        if (previewGain.isSmoothing() || previewGain.getTargetValue() != 1.0f)
            for (int i = 0; i < numSamples; ++i) { const float g = previewGain.getNextValue(); left[i] *= g; right[i] *= g; }
    }

    const float rmsL = static_cast<float> (std::sqrt (sumL2 / numSamples));
    const float rmsR = static_cast<float> (std::sqrt (sumR2 / numSamples));
    const double denom = std::sqrt (sumL2 * sumR2) + 1e-12;
    const float corr = static_cast<float> (std::clamp (sumLR / denom, -1.0, 1.0));
    const float peakDbVal = juce::Decibels::gainToDecibels (maxPeak, -60.0f);
    const float lufsVal = loudness.momentary.load (std::memory_order_relaxed);

    rmsLeft.store (rmsL, std::memory_order_relaxed);
    rmsRight.store (rmsR, std::memory_order_relaxed);
    phaseCorrelation.store (corr, std::memory_order_relaxed);
    peakDb.store (peakDbVal, std::memory_order_relaxed);
    lufsMomentary.store (lufsVal, std::memory_order_relaxed);

    rackLinkBus->masterSubRms.store (0.5f * (rmsL + rmsR), std::memory_order_relaxed);
    rackLinkBus->masterPhase.store (corr, std::memory_order_relaxed);
    rackLinkBus->lufsMomentary.store (lufsVal, std::memory_order_relaxed);
    rackLinkBus->truePeakDb.store (loudness.truePeakDb.load (std::memory_order_relaxed), std::memory_order_relaxed);
    {
        // How much of this block's real-time budget we used (smoothed).
        const double used = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - blockStartTicks);
        const double budget = numSamples / std::max (1.0, preparedSampleRate);
        const float a = (float) std::exp (-budget / 0.5);
        ownLoadSmoothed = a * ownLoadSmoothed + (1.0f - a) * (float) (used / budget);
        dqsOwnLoad.store (ownLoadSmoothed, std::memory_order_relaxed);
        if (rackSlot >= 0) rackLinkBus->slots[(size_t) rackSlot].cpuLoad.store (ownLoadSmoothed, std::memory_order_relaxed);
    }
    if (rackSlot >= 0)
    {
        auto& slot = rackLinkBus->slots[(size_t) rackSlot];
        slot.lufsShort.store (loudness.shortTerm.load (std::memory_order_relaxed), std::memory_order_relaxed);
        slot.truePeak.store (loudness.truePeakDb.load (std::memory_order_relaxed), std::memory_order_relaxed);
        slot.gainReduct.store (limiterGainReductionDb.load (std::memory_order_relaxed)
                               + glueGainReductionDb.load (std::memory_order_relaxed)
                               + duckGainReductionDb.load (std::memory_order_relaxed), std::memory_order_relaxed);
    }
}

juce::AudioProcessorEditor* SolutionAudioProcessor::createEditor()
{
   #if SOLUTION_HEADLESS
    return nullptr;     // test builds have no UI
   #else
    return new SolutionAudioProcessorEditor (*this);
   #endif
}

juce::var SolutionAudioProcessor::getRemoteState() const
{
    juce::Array<juce::var> params;
    for (auto* p : getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p);
        if (ranged == nullptr) continue;
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("id", ranged->getParameterID());
        o->setProperty ("name", ranged->getName (40));
        o->setProperty ("value", ranged->convertFrom0to1 (ranged->getValue()));
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (ranged))
        {
            o->setProperty ("type", "choice");
            juce::Array<juce::var> names; for (const auto& c : choice->choices) names.add (c);
            o->setProperty ("choices", names);
        }
        else if (dynamic_cast<juce::AudioParameterBool*> (ranged) != nullptr)
            o->setProperty ("type", "bool");
        else
        {
            const auto& range = ranged->getNormalisableRange();
            o->setProperty ("type", "float");
            o->setProperty ("min", range.start); o->setProperty ("max", range.end); o->setProperty ("step", range.interval);
        }
        params.add (juce::var (o.get()));
    }
    juce::DynamicObject::Ptr state = new juce::DynamicObject();
    state->setProperty ("module", juce::String (SolutionDSP::moduleDisplayName (moduleId)));
    state->setProperty ("params", params);
    state->setProperty ("lufs", loudness.shortTerm.load());
    state->setProperty ("truePeak", loudness.truePeakDb.load());
    state->setProperty ("gr", limiterGainReductionDb.load() + glueGainReductionDb.load() + duckGainReductionDb.load());
    juce::Array<juce::var> presetNames;
    {
        const juce::ScopedLock sl (remotePresetLock);
        for (const auto& n : remotePresetNames) presetNames.add (n);
    }
    state->setProperty ("presets", presetNames);
    return juce::var (state.get());
}

bool SolutionAudioProcessor::startRemote (const juce::String& bindAddress)
{
    {
        const juce::ScopedLock sl (remotePresetLock);
        remotePresetNames.clear();
        for (const auto& info : presets.getAllPresets()) remotePresetNames.add (info.name);
    }
    SolutionDSP::RemoteServer::Host host;
    host.getPage = []
    {
       #if SOLUTION_HEADLESS
        return juce::String ("<!doctype html><title>The Solution Remote</title>");
       #else
        return juce::String::fromUTF8 (BinaryData::remote_html, BinaryData::remote_htmlSize);
       #endif
    };
    host.getState = [this] { return getRemoteState(); };
    host.setParam = [this] (const juce::String& id, float value)
    {
        if (auto* param = apvts.getParameter (id))
        {
            const auto& range = param->getNormalisableRange();
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->convertTo0to1 (juce::jlimit (range.start, range.end, value)));
            param->endChangeGesture();
        }
    };
    host.loadPreset = [this] (const juce::String& name) { presets.loadPreset (name); };
    host.runOnMessageThread = [this] (std::function<void()> fn)
    {
        if (remoteExecutor) { remoteExecutor (std::move (fn)); return; }
        juce::MessageManager::callAsync ([fn = std::move (fn), alive = std::weak_ptr<bool> (aliveFlag)] { if (! alive.expired()) fn(); });
    };
    remote = std::make_unique<SolutionDSP::RemoteServer> (std::move (host));
    const int firstPort = 47800 + std::max (0, rackSlot) % 64;
    return remote->start (firstPort, 100, bindAddress);
}

juce::String SolutionAudioProcessor::setCharacterModel (const juce::String& modelName)
{
    character.housekeeping();
    apvts.state.setProperty ("charModel", modelName, nullptr);
    if (modelName.isEmpty()) { character.setModel (nullptr); return {}; }

    juce::String error;
    auto model = SolutionDSP::NeuralCharacter::load (SolutionDSP::NeuralCharacter::getModelsFolder().getChildFile (modelName + ".json"), error);
    if (model == nullptr)
        return error.isNotEmpty() ? error : "Couldn't load " + modelName;
    character.setModel (std::move (model));
    return {};
}

void SolutionAudioProcessor::setParamReal (const juce::String& id, float value)
{
    if (auto* q = apvts.getParameter (id))
    {
        q->beginChangeGesture();
        q->setValueNotifyingHost (q->convertTo0to1 (juce::jlimit (q->getNormalisableRange().start, q->getNormalisableRange().end, value)));
        q->endChangeGesture();
    }
}

bool SolutionAudioProcessor::requestParamOnSlot (int slot, const juce::String& id, float value)
{
    if (slot == rackSlot || slot < 0) { setParamReal (id, value); return true; }
    if (slot >= SolutionDSP::RackLinkBus::maxSlots || ! rackLinkBus->slots[(size_t) slot].active.load()) return false;
    const auto& params = getParameters();
    for (int i = 0; i < params.size(); ++i)
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (params[i]); ranged != nullptr && ranged->getParameterID() == id)
            return rackLinkBus->slots[(size_t) slot].pushRequest (i, value);
    return false;
}

void SolutionAudioProcessor::setMotionPath (const std::vector<SolutionDSP::MotionPoint>& points, bool saveToState)
{
    motion.setPath (points);
    if (! saveToState) return;
    apvts.state.removeChild (apvts.state.getChildWithName ("MOTION"), nullptr);
    juce::ValueTree m ("MOTION");
    for (const auto& pt : points)
        m.appendChild (juce::ValueTree ("P", { { "az", pt.az }, { "el", pt.el }, { "d", pt.dist } }), nullptr);
    apvts.state.appendChild (m, nullptr);
}

std::vector<SolutionDSP::MotionPoint> SolutionAudioProcessor::getMotionPath() const
{
    std::vector<SolutionDSP::MotionPoint> pts;
    for (const auto& c : apvts.state.getChildWithName ("MOTION"))
        pts.push_back ({ (float) c.getProperty ("az"), (float) c.getProperty ("el"), (float) c.getProperty ("d") });
    return pts;
}

int SolutionAudioProcessor::getSessionMonitorCount() const
{
    int count = 0;
    for (int i = 0; i < SolutionDSP::RackLinkBus::maxSlots; ++i)
        if (i != rackSlot && rackLinkBus->slots[(size_t) i].active.load() && (rackLinkBus->slots[(size_t) i].monitorMask.load() & monitorAutoBypassMask) != 0)
            ++count;
    return count;
}

void SolutionAudioProcessor::turnOffMonitoring()
{
    for (const char* id : { "ab_bypass", "delta_listen", "stream_preview", "listen_room", "room_correct_on", "stem_listen", "tape_cal_tone" })
        if (auto* q = apvts.getParameter (id); q != nullptr && q->getValue() != 0.0f)
        {
            q->beginChangeGesture(); q->setValueNotifyingHost (0.0f); q->endChangeGesture();
        }
}

void SolutionAudioProcessor::turnOffMonitoringEverywhere()
{
    for (int i = 0; i < SolutionDSP::RackLinkBus::maxSlots; ++i)
        if (i != rackSlot && rackLinkBus->slots[(size_t) i].active.load())
            rackLinkBus->slots[(size_t) i].monitorOffRequest = true;     // each picks it up on its own timer
    turnOffMonitoring();
}

juce::String SolutionAudioProcessor::alignNow()
{
    if (rackSlot < 0) return "Rack Link isn't available in this host session";
    auto& bus = *rackLinkBus;
    auto& me = bus.slots[(size_t) rackSlot];
    const int group = me.group.load();
    if (group <= 0) return "Put this track and the reference in the same Rack Link group (A-D) first";
    if (me.alignRole.load() != 2) return "Set this track to 'Align to reference' first";

    int refSlot = -1;
    for (int i = 0; i < SolutionDSP::RackLinkBus::maxSlots; ++i)
        if (i != rackSlot && bus.slots[(size_t) i].active.load() && bus.slots[(size_t) i].alignRole.load() == 1 && bus.slots[(size_t) i].group.load() == group)
            refSlot = i;
    if (refSlot < 0) return "No reference track found in this group - set one track to 'Reference'";

    auto& ref = bus.slots[(size_t) refSlot];
    const auto r = SolutionDSP::computeAlignment (ref.alignRing.data(), ref.alignRingEnd.load (std::memory_order_acquire),
                                                  me.alignRing.data(), me.alignRingEnd.load (std::memory_order_acquire),
                                                  SolutionDSP::RackSlot::alignRingSize, preparedSampleRate);
    if (! r.found) return r.message;

    // We can only delay, so delay whichever mic hears the sound first.
    auto setParam = [this] (const char* id, float v) { auto* q = apvts.getParameter (id); q->beginChangeGesture(); q->setValueNotifyingHost (q->convertTo0to1 (v)); q->endChangeGesture(); };
    const double ms = std::abs (r.lagSamples) / preparedSampleRate * 1000.0;
    setParam ("align_invert", r.invert ? 1.0f : 0.0f);
    juce::String what;
    if (r.lagSamples < 0)      // this track is earlier -> delay it; the reference needs none
    {
        setParam ("align_delay_ms", (float) ms);
        ref.alignRequestDelaySamples = 0.0f; ref.alignRequestInvert = false; ref.alignRequest = true;
        what = "Delayed this track by " + juce::String (ms, 2) + " ms";
    }
    else                       // the reference is earlier -> ask it to delay itself
    {
        setParam ("align_delay_ms", 0.0f);
        ref.alignRequestDelaySamples = (float) r.lagSamples; ref.alignRequestInvert = false; ref.alignRequest = true;
        what = "Delayed the reference track by " + juce::String (ms, 2) + " ms";
    }
    if (r.invert) what << ", flipped this track's polarity";
    return what + " (match " + juce::String (juce::roundToInt (r.confidence * 100.0)) + "%)";
}

void SolutionAudioProcessor::pollAlignRequests()
{
    if (rackSlot < 0) return;
    auto& me = rackLinkBus->slots[(size_t) rackSlot];
    if (! me.alignRequest.exchange (false)) return;
    const float ms = me.alignRequestDelaySamples.load() / (float) std::max (1.0, preparedSampleRate) * 1000.0f;
    for (auto [id, v] : { std::pair<const char*, float> { "align_delay_ms", ms }, { "align_invert", me.alignRequestInvert.load() ? 1.0f : 0.0f } })
    {
        auto* q = apvts.getParameter (id); q->beginChangeGesture(); q->setValueNotifyingHost (q->convertTo0to1 (v)); q->endChangeGesture();
    }
}

void SolutionAudioProcessor::startAutoGainStage()
{
    autoStageSum = 0.0; autoStageCount = 0;     // audio thread doesn't touch these until state becomes 1
    autoStageState.store (1, std::memory_order_release);
}

bool SolutionAudioProcessor::pollAutoGainStage()
{
    if (autoStageState.load (std::memory_order_acquire) != 2)
        return false;
    autoStageState.store (0);
    const float rmsDb = autoStageResultDb.load();
    if (rmsDb < -70.0f)
        return false;                            // silence: leave the trims alone
    // Sweet spot for analog-style saturation: about -18 dBFS average. Output trim restores the level.
    const float trim = juce::jlimit (-24.0f, 24.0f, -18.0f - rmsDb);
    for (auto [id, v] : { std::pair<const char*, float> { "trim_in_db", trim }, { "trim_out_db", -trim } })
    {
        auto* param = apvts.getParameter (id);
        param->beginChangeGesture(); param->setValueNotifyingHost (param->convertTo0to1 (v)); param->endChangeGesture();
    }
    return true;
}

void SolutionAudioProcessor::updateLatencyNow()
{
    setLatencySamples (getBaseLatency() + (stemActive.load() ? SolutionDSP::StemSplitter::latency : 0));
}

void SolutionAudioProcessor::updateTrackProperties (const TrackProperties& properties)
{
    if (rackSlot >= 0 && properties.name.has_value())
        rackLinkBus->slots[(size_t) rackSlot].trackName = *properties.name;   // message thread
}

void SolutionAudioProcessor::triggerRoomSweep (float seconds)
{
    if (isRoomCapturing() || analysisBusy.load() || preparedSampleRate <= 0.0)
        return;
    measurement.arm (preparedSampleRate, seconds);
}

bool SolutionAudioProcessor::pollRoomAnalysis()
{
    using State = SolutionDSP::RoomMeasurement::State;
    if (measurement.getState() == State::finished && ! analysisBusy.exchange (true))
    {
        analysisPool.addJob ([this, mic = micProfile]
        {
            auto result = std::make_unique<SolutionDSP::RoomAnalysis> (measurement.analyse (mic));
            const juce::ScopedLock sl (analysisLock);
            finishedAnalysis = std::move (result);
        });
    }

    std::unique_ptr<SolutionDSP::RoomAnalysis> done;
    {
        const juce::ScopedLock sl (analysisLock);
        done = std::move (finishedAnalysis);
    }
    if (done == nullptr)
        return false;

    measurement.markConsumed();
    analysisBusy = false;
    if (done->valid)
        applyRoomAnalysis (*done, true);
    else
        roomAnalysis.message = done->message;
    return true;
}

void SolutionAudioProcessor::applyRoomAnalysis (const SolutionDSP::RoomAnalysis& analysis, bool saveToState)
{
    roomAnalysis = analysis;
    roomCorrector.setBands (analysis.bands);
    if (! saveToState)
        return;

    // Save bands + curves with the session so correction survives reloads.
    apvts.state.removeChild (apvts.state.getChildWithName ("ROOMCORRECTION"), nullptr);
    juce::ValueTree rc ("ROOMCORRECTION");
    for (const auto& b : analysis.bands)
        rc.appendChild (juce::ValueTree ("BAND", { { "freq", b.freq }, { "gain", b.gainDb }, { "q", b.q } }), nullptr);
    auto join = [] (const auto& arr) { juce::StringArray sa; for (float v : arr) sa.add (juce::String (v, 2)); return sa.joinIntoString (","); };
    rc.setProperty ("raw", join (analysis.rawDb), nullptr);   // refit on load with the saved mic
    rc.setProperty ("message", analysis.message, nullptr);
    apvts.state.appendChild (rc, nullptr);
}

void SolutionAudioProcessor::saveMicToState()
{
    apvts.state.setProperty ("micProfile", micProfile.id, nullptr);
    if (micProfile.id == "calfile")
    {
        juce::StringArray pts;
        for (const auto& [f, db] : micProfile.response) pts.add (juce::String (f, 2) + ":" + juce::String (db, 2));
        apvts.state.setProperty ("micCalName", micProfile.name.fromFirstOccurrenceOf ("Calibrated: ", false, false), nullptr);
        apvts.state.setProperty ("micCal", pts.joinIntoString (","), nullptr);
    }
}

void SolutionAudioProcessor::refitForCurrentMic()
{
    if (! roomAnalysis.valid)
        return;
    auto updated = roomAnalysis;
    SolutionDSP::refitRoomAnalysis (updated, micProfile, preparedSampleRate > 0.0 ? preparedSampleRate : 48000.0);
    applyRoomAnalysis (updated, true);
}

void SolutionAudioProcessor::setMicProfile (const juce::String& builtInId)
{
    micProfile = SolutionDSP::findMicProfile (builtInId);
    saveMicToState();
    refitForCurrentMic();
}

bool SolutionAudioProcessor::loadMicCalibration (const juce::File& file)
{
    auto points = SolutionDSP::parseCalibrationFile (file.loadFileAsString());
    if (points.empty())
        return false;
    micProfile = SolutionDSP::makeCalibratedProfile (file.getFileNameWithoutExtension(), std::move (points));
    saveMicToState();
    refitForCurrentMic();
    return true;
}

//==============================================================================
SolutionDSP::CurrentSettings SolutionAudioProcessor::currentSettingsForAssistant() const
{
    using M = SolutionDSP::ModuleId;
    SolutionDSP::CurrentSettings now;
    now.elevation = apvts.getRawParameterValue ("stage_elevation")->load();
    now.width     = apvts.getRawParameterValue ("stage_width")->load();
    now.glue      = apvts.getRawParameterValue ("glue_amount")->load();
    now.hasAirControl = moduleId == M::Loft || moduleId == M::Aether || moduleId == M::Mosaic || moduleId == M::Stage || moduleId == M::Land
                     || moduleId == M::Orbit || moduleId == M::Void || moduleId == M::Crown || moduleId == M::Chimera;
    now.moduleName = SolutionDSP::moduleDisplayName (moduleId);
    return now;
}

void SolutionAudioProcessor::assistantStartListening()
{
    assistant.live = std::make_unique<SolutionDSP::MixAnalyser> (preparedSampleRate > 0.0 ? preparedSampleRate : 48000.0);
    assistant.comparison = {};
    assistant.tap.setEnabled (true);
}

void SolutionAudioProcessor::assistantPoll()
{
    if (assistant.live != nullptr && assistant.tap.isEnabled())
        assistant.tap.drain ([this] (const float* l, const float* r, int n) { assistant.live->addStereo (l, r, n); });
    if (assistant.live != nullptr && assistant.reference.valid && assistant.live->getSecondsAnalysed() > 3.0)
    {
        const auto mix = assistant.live->finish ("Your mix");
        if (mix.valid)
            assistant.comparison = SolutionDSP::compareMixes (mix, assistant.reference, currentSettingsForAssistant());
    }
}

void SolutionAudioProcessor::assistantLoadReference (const juce::File& file)
{
    if (assistant.loadingReference.exchange (true))
        return;
    analysisPool.addJob ([this, file]
    {
        juce::String error;
        auto profile = SolutionDSP::analyseReferenceFile (file, error);
        juce::MessageManager::callAsync ([this, profile, error, alive = std::weak_ptr<bool> (aliveFlag)]
        {
            if (alive.expired()) return;
            assistant.referenceError = error;
            if (profile.valid) assistant.reference = profile;
            assistant.loadingReference = false;
        });
    });
}

void SolutionAudioProcessor::assistantUseGenre (const juce::String& genre)
{
    assistant.reference = SolutionDSP::genreTarget (genre);
    assistant.referenceError = {};
}

bool SolutionAudioProcessor::assistantApply (int index)
{
    const auto& list = assistant.comparison.suggestions;
    bool applied = false;
    undoManager.beginNewTransaction ("Mix Assistant");
    for (int i = 0; i < (int) list.size(); ++i)
    {
        if ((index >= 0 && i != index) || list[(size_t) i].paramId.isEmpty())
            continue;
        if (auto* param = apvts.getParameter (list[(size_t) i].paramId))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->convertTo0to1 (list[(size_t) i].newValue));
            param->endChangeGesture();
            applied = true;
        }
    }
    if (applied && assistant.live != nullptr)
        assistantStartListening();   // listen again to hear the result
    return applied;
}

void SolutionAudioProcessor::setCurrentProgram (int index)
{
    if (presets.loadFactoryPreset (index))
        currentProgram = index;
}

void SolutionAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("stateVersion", stateVersion, nullptr);
   #ifdef JucePlugin_VersionString
    state.setProperty ("pluginVersion", JucePlugin_VersionString, nullptr);
   #endif
    state.setProperty ("module", SOLUTION_MODULE_ID, nullptr);
    state.setProperty ("preset", presets.getCurrentPresetName(), nullptr);
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void SolutionAudioProcessor::migrateState (juce::ValueTree& state, int fromVersion)
{
    // Version 0 = sessions saved before versioning existed (same layout as v1).
    // Future example:  if (fromVersion < 2) rename "old_id" -> "new_id" in the PARAM children.
    juce::ignoreUnused (state, fromVersion);
}

void SolutionAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState == nullptr || ! xmlState->hasTagName (apvts.state.getType()))
        return;

    auto tree = juce::ValueTree::fromXml (*xmlState);
    const int savedVersion = tree.getProperty ("stateVersion", 0);
    if (savedVersion > stateVersion)
        DBG ("Session was saved by a newer version of The Solution - loading what we understand");
    migrateState (tree, savedVersion);
    apvts.replaceState (tree);

    // Restore the measuring mic, then the saved room measurement (refitted for that mic).
    const auto micId = apvts.state.getProperty ("micProfile", "generic").toString();
    if (micId == "calfile")
    {
        std::vector<std::pair<float, float>> pts;
        for (const auto& p : juce::StringArray::fromTokens (apvts.state.getProperty ("micCal").toString(), ",", {}))
            pts.emplace_back (p.upToFirstOccurrenceOf (":", false, false).getFloatValue(), p.fromFirstOccurrenceOf (":", false, false).getFloatValue());
        micProfile = SolutionDSP::makeCalibratedProfile (apvts.state.getProperty ("micCalName").toString(), std::move (pts));
    }
    else
        micProfile = SolutionDSP::findMicProfile (micId);

    SolutionDSP::RoomAnalysis restored;
    const auto rc = apvts.state.getChildWithName ("ROOMCORRECTION");
    if (rc.isValid())
    {
        juce::StringArray raw; raw.addTokens (rc.getProperty ("raw").toString(), ",", {});
        if (raw.size() == SolutionDSP::RoomAnalysis::numPoints)
        {
            for (int i = 0; i < SolutionDSP::RoomAnalysis::numPoints; ++i)
            {
                restored.freqs[(size_t) i] = SolutionDSP::RoomAnalysis::pointFreq (i);
                restored.rawDb[(size_t) i] = raw[i].getFloatValue();
            }
            SolutionDSP::refitRoomAnalysis (restored, micProfile, preparedSampleRate > 0.0 ? preparedSampleRate : 48000.0);
            restored.valid = true;
        }
    }
    applyRoomAnalysis (restored, false);

    // 3D stage motion path.
    setMotionPath (getMotionPath(), false);

    // Neural Character model (if the file is still there).
    const auto charModel = apvts.state.getProperty ("charModel").toString();
    if (charModel.isNotEmpty() || character.getModelName().isNotEmpty())
        setCharacterModel (charModel);
    markParamsChanged();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SolutionAudioProcessor();
}
