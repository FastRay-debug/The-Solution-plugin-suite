// Automated checks for The Solution Suite DSP. Built with -DSOLUTION_BUILD_TESTS=ON
// and run on every push by GitHub Actions. Exit code 0 = all passed.
//
// What it checks, for all 13 modules:
//   * produces sound (catches "silent module" bugs) and never blows up
//   * no NaN / infinity, at 44.1k-192k and buffer sizes 16-2048 (incl. ragged sizes)
//   * output decays back to silence after input stops (no stuck feedback / DC)
//   * the audio thread never allocates memory (causes clicks/dropouts in DAWs)
// And for the plugin processor: saving and restoring settings round-trips exactly.

#include "../Source/ModuleEngines.h"
#include "../Source/SolutionProcessor.h"
#include "../Source/SystemMonitor.h"
#include "../Source/Vendor.h"
#include "../Source/VoiceCommands.h"
#include <atomic>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>

//==============================================================================
// Allocation tracker: counts heap allocations made while `trackAllocs` is on.
static thread_local bool trackAllocs = false;
static std::atomic<int> allocCount { 0 };

void* operator new (std::size_t n)
{
    if (trackAllocs) allocCount.fetch_add (1);
    if (void* p = std::malloc (n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t n)                        { return operator new (n); }
void operator delete (void* p) noexcept                     { std::free (p); }
void operator delete[] (void* p) noexcept                   { std::free (p); }
void operator delete (void* p, std::size_t) noexcept        { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept      { std::free (p); }

//==============================================================================
static int failures = 0, checks = 0;
#define EXPECT(cond, ...) do { ++checks; if (! (cond)) { ++failures; std::printf ("  FAIL: " __VA_ARGS__); std::printf ("\n"); } } while (0)

using namespace SolutionDSP;

static const std::pair<const char*, ModuleId> modules[] = {
    { "TheBunker", ModuleId::Bunker }, { "TheStage", ModuleId::Stage }, { "TheLand", ModuleId::Land },
    { "TheLoft", ModuleId::Loft }, { "TheOrbit", ModuleId::Orbit }, { "TheBridge", ModuleId::Bridge },
    { "TheVoid", ModuleId::Void }, { "TheAnchor", ModuleId::Anchor }, { "TheAether", ModuleId::Aether },
    { "TheMosaic", ModuleId::Mosaic }, { "Innerspace", ModuleId::Innerspace },
    { "TheMatrix", ModuleId::Matrix }, { "TheScope", ModuleId::Scope },
    { "TheCrown", ModuleId::Crown }, { "Chimera", ModuleId::Chimera } };

static ProcessParams randomParams (std::mt19937& rng)
{
    std::uniform_real_distribution<float> u (0.0f, 1.0f);
    ProcessParams p;
    p.azimuthDeg = -180.0f + 360.0f * u (rng);  p.elevationDeg = -90.0f + 180.0f * u (rng);
    p.depth = u (rng);                           p.haasMs = 35.0f * u (rng);
    p.width = 2.0f * u (rng);                    p.anchorHz = 20.0f + 230.0f * u (rng);
    p.midDb = -12.0f + 24.0f * u (rng);          p.sideDb = -12.0f + 24.0f * u (rng);
    p.roomMix = u (rng);                         p.roomProfile = (int) (u (rng) * 3.99f);
    p.patchOrder = u (rng) > 0.5f;               p.wetMix = u (rng);
    p.transientSculpt = u (rng) > 0.5f;          p.ultraWide = u (rng);
    p.cvModDepth = u (rng);                      p.clipperShape = (int) (u (rng) * 2.99f);
    p.regenThrottle = u (rng);                   p.dialogueDuck = u (rng) > 0.5f;
    return p;
}

struct RunResult { double rmsDb = -300.0; double peak = 0.0; bool finite = true; double tailDb = -300.0; int allocs = 0; };

static RunResult runEngine (ModuleId id, double sr, int maxBlock, bool raggedBlocks, const ProcessParams& p, unsigned seed)
{
    SuiteModuleEngine engine; auto busPtr = std::make_unique<RackLinkBus>(); auto& bus = *busPtr;
    engine.prepare ({ sr, (juce::uint32) maxBlock, 2 });
    std::mt19937 rng (seed);
    std::normal_distribution<float> noise (0.0f, 0.1f);    // about -20 dBFS RMS
    std::uniform_int_distribution<int> blockDist (1, maxBlock);
    juce::AudioBuffer<float> buf (2, maxBlock);

    RunResult r; double sum = 0.0; long n = 0;
    const long totalSamples = (long) (sr * 1.5), quietSamples = (long) (sr * 1.5);

    auto processBlock = [&] (int len, bool silent, bool measure, bool tail)
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < len; ++i)
                buf.setSample (ch, i, silent ? 0.0f : noise (rng));
        juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), 2, len);
        allocCount = 0; trackAllocs = true;
        engine.process (id, view, p, bus);
        trackAllocs = false; r.allocs += allocCount.load();
        double tailSum = 0.0;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < len; ++i)
            {
                const float v = view.getSample (ch, i);
                if (! std::isfinite (v)) r.finite = false;
                r.peak = std::max (r.peak, (double) std::abs (v));
                if (measure) { sum += (double) v * v; ++n; }
                if (tail) tailSum += (double) v * v;
            }
        if (tail) r.tailDb = 10.0 * std::log10 (tailSum / (2.0 * len) + 1e-30);
    };

    int warmupAllocs = -1;
    for (long done = 0; done < totalSamples; )
    {
        const int len = raggedBlocks ? blockDist (rng) : maxBlock;
        processBlock (len, false, done > (long) (sr * 0.25), false);
        if (warmupAllocs < 0) { warmupAllocs = r.allocs; r.allocs = 0; }   // first block may lazily set things up
        done += len;
    }
    for (long done = 0; done < quietSamples; )
    {
        const int len = raggedBlocks ? blockDist (rng) : maxBlock;
        const bool last = done + len >= quietSamples;
        processBlock (len, true, false, last);
        done += len;
    }
    r.rmsDb = 10.0 * std::log10 (sum / std::max (1L, n) + 1e-30) + 20.0;   // relative to the -20 dB input
    return r;
}

static void testEngines()
{
    std::printf ("== DSP engine: 13 modules x sample rates x buffer sizes\n");
    const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
    const int blocks[]   = { 16, 64, 256, 512, 2048 };

    for (const auto& [name, id] : modules)
    {
        int moduleFails = failures;
        // Default settings must make sound at a sensible level.
        const auto def = runEngine (id, 48000.0, 512, false, ProcessParams{}, 1);
        EXPECT (def.finite, "%s: NaN/inf at default settings", name);
        EXPECT (def.rmsDb > -12.0 && def.rmsDb < 12.0, "%s: default output level %.1f dB relative to input (expected within +/-12 dB)", name, def.rmsDb);

        for (double sr : rates)
            for (int bs : blocks)
            {
                const auto r = runEngine (id, sr, bs, false, ProcessParams{}, 2);
                EXPECT (r.finite, "%s @ %.0f Hz / %d: NaN/inf", name, sr, bs);
                EXPECT (r.rmsDb > -40.0, "%s @ %.0f Hz / %d: nearly silent (%.1f dB)", name, sr, bs, r.rmsDb);
                EXPECT (r.allocs == 0, "%s @ %.0f Hz / %d: %d heap allocations on the audio thread", name, sr, bs, r.allocs);
                EXPECT (r.tailDb < -100.0, "%s @ %.0f Hz / %d: output didn't decay after input stopped (%.1f dB)", name, sr, bs, r.tailDb);
            }

        // Random knob settings with ragged (host-like) block sizes.
        std::mt19937 rng (100 + (unsigned) id);
        for (int trial = 0; trial < 12; ++trial)
        {
            const auto p = randomParams (rng);
            const auto r = runEngine (id, trial % 2 ? 44100.0 : 96000.0, 1024, true, p, 50 + trial);
            EXPECT (r.finite, "%s random settings #%d: NaN/inf", name, trial);
            EXPECT (r.peak < 8.0, "%s random settings #%d: output peak %.1f (runaway gain)", name, trial, r.peak);
            EXPECT (r.allocs == 0, "%s random settings #%d: %d heap allocations on the audio thread", name, trial, r.allocs);
            EXPECT (r.tailDb < -100.0, "%s random settings #%d: output didn't decay (%.1f dB)", name, trial, r.tailDb);
        }
        std::printf ("  %-11s default %+6.1f dB   %s\n", name, def.rmsDb, failures == moduleFails ? "ok" : "FAILED");
    }
}

static void testProcessorState()
{
    std::printf ("== Plugin processor: settings save / restore\n");
    SolutionAudioProcessor a;
    std::mt19937 rng (7);
    std::uniform_real_distribution<float> u (0.0f, 1.0f);
    for (auto* param : a.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))   // snap to values the host could really send
            ranged->setValueNotifyingHost (ranged->convertTo0to1 (ranged->convertFrom0to1 (u (rng))));

    juce::MemoryBlock state;
    a.getStateInformation (state);

    SolutionAudioProcessor b;
    b.setStateInformation (state.getData(), (int) state.getSize());

    const auto pa = a.getParameters(), pb = b.getParameters();
    EXPECT (pa.size() == pb.size(), "parameter count differs after restore");
    for (int i = 0; i < std::min (pa.size(), pb.size()); ++i)
        EXPECT (std::abs (pa[i]->getValue() - pb[i]->getValue()) < 1e-6f, "parameter '%s' not restored (%f vs %f)",
                pa[i]->getName (64).toRawUTF8(), pa[i]->getValue(), pb[i]->getValue());

    // Garbage or empty state must never crash and must leave defaults intact.
    const char junk[] = "not a valid plugin state";
    SolutionAudioProcessor c;
    c.setStateInformation (junk, (int) sizeof (junk));
    c.setStateInformation (nullptr, 0);

    // Processor-level audio: stereo block through prepare/process at an odd buffer size.
    SolutionAudioProcessor d;
    d.prepareToPlay (48000.0, 333);
    juce::AudioBuffer<float> buf (2, 333);
    juce::MidiBuffer midi;
    float maxOut = 0.0f;
    for (int k = 0; k < 200; ++k)
    {
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 333; ++i) buf.setSample (ch, i, 0.1f * std::sin (0.05f * (float) (k * 333 + i)));
        allocCount = 0; trackAllocs = true;
        d.processBlock (buf, midi);
        trackAllocs = false;
        if (k > 0) EXPECT (allocCount.load() == 0, "processBlock allocated %d times (block %d)", allocCount.load(), k);
        maxOut = std::max (maxOut, buf.getMagnitude (0, 333));
    }
    EXPECT (maxOut <= juce::Decibels::decibelsToGain (-0.5f) + 1e-4f, "ceiling guard let %.3f through", maxOut);
    d.releaseResources();
    std::printf ("  done\n");
}

static void testPresets()
{
    std::printf ("== Presets\n");
    const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("SolutionPresetTest_" + juce::String (juce::Random::getSystemRandom().nextInt()));
    SolutionAudioProcessor proc;
    PresetManager pm (proc.apvts, tmp);

    EXPECT (pm.getNumFactoryPresets() >= 5, "expected factory presets, got %d", pm.getNumFactoryPresets());
    EXPECT (proc.getNumPrograms() == pm.getNumFactoryPresets(), "DAW program list doesn't match factory presets");

    // Every factory preset loads and only uses real parameter IDs.
    for (int i = 0; i < pm.getNumFactoryPresets(); ++i)
        EXPECT (pm.loadFactoryPreset (i), "factory preset %d failed to load", i);

    auto* width = proc.apvts.getParameter ("stage_width");
    EXPECT (pm.loadPreset ("Wide Chorus Lift"), "load by name failed");
    EXPECT (std::abs (width->convertFrom0to1 (width->getValue()) - 1.5f) < 0.011f, "factory preset value not applied");

    // Save -> change -> load restores; reserved names refused; delete works.
    width->setValueNotifyingHost (width->convertTo0to1 (0.42f));
    EXPECT (pm.savePreset ("My Test <Preset>", "#User"), "save failed");
    EXPECT (! pm.savePreset ("Init", "#User"), "saving over a factory preset name should be refused");
    width->setValueNotifyingHost (width->convertTo0to1 (1.9f));
    EXPECT (pm.loadPreset ("My Test <Preset>"), "user preset failed to load");
    EXPECT (std::abs (width->convertFrom0to1 (width->getValue()) - 0.42f) < 0.011f, "user preset value not restored");
    bool listed = false;
    for (const auto& info : pm.getAllPresets()) listed |= (info.name == "My Test <Preset>" && ! info.factory);
    EXPECT (listed, "saved preset missing from the list");
    EXPECT (pm.deletePreset ("My Test <Preset>"), "delete failed");
    EXPECT (! pm.deletePreset ("Init"), "factory presets must not be deletable");
    EXPECT (! pm.loadPreset ("does not exist"), "loading a missing preset should fail cleanly");

    // A preset missing parameters (e.g. made by an older version) falls back to defaults.
    tmp.createDirectory();
    tmp.getChildFile ("Old.solpreset").replaceWithText ("<SolutionPreset name=\"Old\" tag=\"#User\"><PARAM id=\"stage_depth\" value=\"0.9\"/></SolutionPreset>");
    EXPECT (pm.loadPreset ("Old"), "old-format preset failed to load");
    EXPECT (std::abs (width->getValue() - width->getDefaultValue()) < 1e-6f, "missing parameter didn't fall back to default");

    // Saved session carries a state version and survives a round trip.
    juce::MemoryBlock block;
    proc.getStateInformation (block);
    auto xml = juce::AudioProcessor::getXmlFromBinary (block.getData(), (int) block.getSize());
    EXPECT (xml != nullptr && xml->getIntAttribute ("stateVersion") == SolutionAudioProcessor::stateVersion, "session state is missing its version number");

    proc.setUiProperty ("skin", "giza");
    proc.getStateInformation (block);
    SolutionAudioProcessor restored;
    restored.setStateInformation (block.getData(), (int) block.getSize());
    EXPECT (restored.getUiProperty ("skin") == "giza", "skin choice not saved with the session");

    tmp.deleteRecursively();
    std::printf ("  done\n");
}

// Independent true-peak measurement (16x oversampled) - deliberately not the plugin's own detector.
static float measureTruePeakDb (const juce::AudioBuffer<float>& in, int skip)
{
    juce::dsp::Oversampling<float> os (2, 4, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true);
    os.initProcessing ((size_t) in.getNumSamples());
    juce::AudioBuffer<float> copy (in);
    juce::dsp::AudioBlock<float> blk (copy);
    auto up = os.processSamplesUp (blk);
    float peak = 0.0f;
    const auto osSkip = (size_t) (skip * 16 + 512);
    for (size_t ch = 0; ch < up.getNumChannels(); ++ch)
        for (size_t i = osSkip; i < up.getNumSamples(); ++i)
            peak = std::max (peak, std::abs (up.getSample ((int) ch, (int) i)));
    return juce::Decibels::gainToDecibels (peak, -100.0f);
}

static void fillSine (juce::AudioBuffer<float>& b, double fs, double freq, float amp, double phase = 0.0, int offset = 0)
{
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (ch, i, amp * (float) std::sin (juce::MathConstants<double>::twoPi * freq * (i + offset) / fs + phase));
}

static void testMetering()
{
    std::printf ("== Loudness meter (ITU-R BS.1770) and true-peak limiter\n");
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        SolutionDSP::LoudnessMeter m; m.prepare (fs);
        juce::AudioBuffer<float> b (2, (int) fs * 5);
        fillSine (b, fs, 997.0, 0.1f);                 // -20 dBFS sine in both channels = -20.0 LUFS
        m.process (b.getReadPointer (0), b.getReadPointer (1), b.getNumSamples());
        EXPECT (std::abs (m.momentary.load() + 20.0f) < 0.1f, "momentary %.2f LUFS at %.0f Hz (expected -20.0)", m.momentary.load(), fs);
        EXPECT (std::abs (m.shortTerm.load() + 20.0f) < 0.1f, "short-term %.2f LUFS at %.0f Hz (expected -20.0)", m.shortTerm.load(), fs);
        EXPECT (std::abs (m.integrated.load() + 20.0f) < 0.1f, "integrated %.2f LUFS at %.0f Hz (expected -20.0)", m.integrated.load(), fs);

        // Relative gate: a long quiet section (-50) must not drag the integrated reading down.
        fillSine (b, fs, 997.0, 0.00316f);
        m.process (b.getReadPointer (0), b.getReadPointer (1), b.getNumSamples());
        EXPECT (std::abs (m.integrated.load() + 20.0f) < 0.2f, "gating: integrated %.2f LUFS (expected about -20.0)", m.integrated.load());

        m.requestReset();
        juce::AudioBuffer<float> silence (2, (int) fs); silence.clear();
        m.process (silence.getReadPointer (0), silence.getReadPointer (1), silence.getNumSamples());
        EXPECT (m.integrated.load() <= -69.9f, "after reset + silence, integrated should be -70 (got %.1f)", m.integrated.load());
    }

    // Limiter: +6 dBFS signal whose real peaks fall *between* samples.
    for (double fs : { 44100.0, 48000.0, 192000.0 })
    {
        SolutionDSP::TruePeakLimiter lim; lim.prepare (fs);
        const float ceiling = juce::Decibels::decibelsToGain (-0.5f);
        juce::AudioBuffer<float> b (2, (int) fs * 2);
        fillSine (b, fs, fs / 4.0 + 37.0, 2.0f, juce::MathConstants<double>::pi / 4.0);
        lim.process (b, b.getNumSamples(), ceiling, true);
        const float tp = measureTruePeakDb (b, lim.getLatency() + (int) (fs * 0.1));
        EXPECT (tp <= -0.4f, "limiter let a true peak of %.2f dBTP through at %.0f Hz (ceiling -0.5)", tp, fs);
        EXPECT (tp > -3.0f, "limiter over-reduced: %.2f dBTP at %.0f Hz", tp, fs);

        // Disengaged: bit-exact pass-through, delayed by exactly its latency.
        SolutionDSP::TruePeakLimiter off; off.prepare (fs);
        juce::AudioBuffer<float> x (2, 4096), y (2, 4096);
        juce::Random rnd (3);
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 4096; ++i) x.setSample (ch, i, rnd.nextFloat() * 2.0f - 1.0f);
        y.makeCopyOf (x);
        off.process (y, 4096, ceiling, false);
        bool exact = true;
        for (int i = off.getLatency(); i < 4096; ++i)
            exact &= y.getSample (0, i) == x.getSample (0, i - off.getLatency());
        EXPECT (exact, "bypassed limiter is not a clean delay of %d samples at %.0f Hz", off.getLatency(), fs);
    }
    std::printf ("  done\n");
}

static void testLatencyAndCeiling()
{
    std::printf ("== Plugin latency report and ceiling guard\n");
    for (double fs : { 44100.0, 96000.0 })
    {
        // Dry-only + guard off: an impulse must come out exactly getLatencySamples() later.
        SolutionAudioProcessor proc;
        proc.apvts.getParameter ("global_mix")->setValueNotifyingHost (0.0f);
        proc.apvts.getParameter ("stage_ceiling_guard")->setValueNotifyingHost (0.0f);
        proc.prepareToPlay (fs, 512);
        const int latency = proc.getLatencySamples();
        juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer midi;
        int found = -1;
        for (int blockIdx = 0; blockIdx < 4 && found < 0; ++blockIdx)
        {
            b.clear();
            if (blockIdx == 0) { b.setSample (0, 0, 0.5f); b.setSample (1, 0, 0.5f); }
            proc.processBlock (b, midi);
            for (int i = 0; i < 512; ++i) if (std::abs (b.getSample (0, i)) > 0.25f) { found = blockIdx * 512 + i; break; }
        }
        EXPECT (latency > 0, "plugin reports no latency at %.0f Hz", fs);
        EXPECT (found == latency, "reported latency %d but audio arrived after %d samples at %.0f Hz", latency, found, fs);

        // Hot wet signal with the guard on: true peak stays under -0.5 dBTP.
        SolutionAudioProcessor hot;
        hot.apvts.getParameter ("stage_width")->setValueNotifyingHost (1.0f);   // max width
        hot.prepareToPlay (fs, 512);
        juce::AudioBuffer<float> all (2, (int) fs);
        for (int start = 0; start < all.getNumSamples(); start += 512)
        {
            const int len = std::min (512, all.getNumSamples() - start);
            juce::AudioBuffer<float> blk (2, len);
            fillSine (blk, fs, 3000.0, 1.6f, 0.3, start);
            hot.processBlock (blk, midi);
            for (int ch = 0; ch < 2; ++ch) all.copyFrom (ch, start, blk, ch, 0, len);
        }
        const float tp = measureTruePeakDb (all, (int) (fs * 0.2));
        EXPECT (tp <= -0.4f, "ceiling guard: output true peak %.2f dBTP at %.0f Hz", tp, fs);
    }
    std::printf ("  done\n");
}

static void setParam (SolutionAudioProcessor& p, const char* id, float realValue)
{
    auto* param = p.apvts.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (realValue));
}

static double blockRmsDb (const juce::AudioBuffer<float>& b)
{
    return juce::Decibels::gainToDecibels ((double) (0.5f * (b.getRMSLevel (0, 0, b.getNumSamples()) + b.getRMSLevel (1, 0, b.getNumSamples()))), -120.0);
}

static void testRackLink()
{
    std::printf ("== Rack Link: session registry, cross-instance ducking, glue\n");
    const double fs = 48000.0; const int bs = 480;
    juce::MidiBuffer midi;

    SolutionAudioProcessor vocal, music, other;
    for (auto* p : { &vocal, &music, &other }) { p->prepareToPlay (fs, bs); setParam (*p, "global_mix", 0.0f); }   // dry: isolate link effects
    vocal.updateTrackProperties ({ juce::String ("Lead Vocal"), std::nullopt });
    EXPECT (vocal.getRackSlot() >= 0 && music.getRackSlot() >= 0 && vocal.getRackSlot() != music.getRackSlot(), "instances didn't get their own Rack Link slots");
    EXPECT (vocal.getRackBus().slots[(size_t) vocal.getRackSlot()].trackName == "Lead Vocal", "track name not registered");

    // Group A: vocal ducks music. "other" is in group B and must not duck.
    setParam (vocal, "link_group", 1.0f); setParam (vocal, "link_duck_send", 1.0f);
    setParam (music, "link_group", 1.0f); setParam (music, "link_duck_depth", 9.0f);
    setParam (other, "link_group", 2.0f); setParam (other, "link_duck_depth", 9.0f);

    auto run = [&] (bool vocalActive, double& musicDb, double& otherDb)
    {
        for (int k = 0; k < 100; ++k)    // 1 s
        {
            juce::AudioBuffer<float> v (2, bs), m (2, bs), o (2, bs);
            fillSine (v, fs, 440.0, vocalActive ? 0.5f : 0.0f, 0.0, k * bs);
            fillSine (m, fs, 220.0, 0.25f, 0.0, k * bs);
            fillSine (o, fs, 330.0, 0.25f, 0.0, k * bs);
            vocal.processBlock (v, midi); music.processBlock (m, midi); other.processBlock (o, midi);
            musicDb = blockRmsDb (m); otherDb = blockRmsDb (o);
        }
    };
    double mQuiet, oQuiet, mDucked, oDucked;
    run (false, mQuiet, oQuiet);
    run (true, mDucked, oDucked);
    EXPECT (mQuiet - mDucked > 6.0 && mQuiet - mDucked < 10.0, "music should duck ~9 dB under the vocal, ducked by %.1f dB", mQuiet - mDucked);
    EXPECT (std::abs (oQuiet - oDucked) < 0.1, "a different group must not duck (changed %.2f dB)", oQuiet - oDucked);
    double mBack, oBack;
    run (false, mBack, oBack);
    EXPECT (std::abs (mBack - mQuiet) < 0.2, "music didn't recover after the vocal stopped (%.1f dB off)", mBack - mQuiet);

    // Glue: 0 = exact bypass; more glue = less dynamic range. The Scope drives "follow suite".
    SolutionAudioProcessor g;
    g.prepareToPlay (fs, bs); setParam (g, "global_mix", 0.0f); setParam (g, "stage_ceiling_guard", 0.0f);
    auto dynamicRange = [&] (SolutionAudioProcessor& proc)
    {
        double loud = 0, quiet = 0;
        for (int k = 0; k < 200; ++k)
        {
            juce::AudioBuffer<float> b (2, bs);
            const bool isLoud = (k / 25) % 2 == 0;
            fillSine (b, fs, 500.0, isLoud ? 0.7f : 0.07f, 0.0, k * bs);
            proc.processBlock (b, midi);
            if (k % 25 == 24) (isLoud ? loud : quiet) = blockRmsDb (b);
        }
        return loud - quiet;
    };
    const double drOff = dynamicRange (g);
    setParam (g, "glue_amount", 1.0f);
    const double drOn = dynamicRange (g);
    EXPECT (std::abs (drOff - 20.0) < 0.3, "glue off should leave the 20 dB swing untouched (got %.1f)", drOff);
    EXPECT (drOn < drOff - 3.0, "glue at 100%% should reduce dynamic range (%.1f -> %.1f dB)", drOff, drOn);

    SolutionAudioProcessor follower;
    follower.prepareToPlay (fs, bs); setParam (follower, "global_mix", 0.0f); setParam (follower, "stage_ceiling_guard", 0.0f);
    setParam (follower, "glue_follow_suite", 1.0f);
    follower.getRackBus().suiteGlue.store (1.0f);   // as if The Scope set 100%
    const double drFollow = dynamicRange (follower);
    EXPECT (drFollow < drOff - 3.0, "'follow suite glue' didn't pick up the suite amount (%.1f dB)", drFollow);
    follower.getRackBus().suiteGlue.store (0.0f);
    std::printf ("  done\n");
}

static void testRoomCorrection()
{
    std::printf ("== Room measurement + correction (simulated room loopback)\n");
    const double fs = 48000.0; const int bs = 512;
    juce::MidiBuffer midi;

    // A fake room: 60 Hz +8 dB, 120 Hz +6 dB boom, a 300 Hz dip, 3 ms latency and an echo.
    using SolutionDSP::RoomCorrector;
    const std::array<std::array<double, 5>, 3> room { RoomCorrector::peaking (fs, 60, 8, 4), RoomCorrector::peaking (fs, 120, 6, 3), RoomCorrector::peaking (fs, 300, -5, 2) };
    std::array<std::array<double, 2>, 3> z {};
    std::vector<float> delayLine ((size_t) (0.003 * fs) + 1, 0.0f), echo ((size_t) (0.011 * fs) + 1, 0.0f);
    size_t dp = 0, ep = 0;
    auto roomSample = [&] (float x)
    {
        double y = x;
        for (size_t b = 0; b < room.size(); ++b)
        {
            const auto& c = room[b]; auto& st = z[b];
            const double out = c[0] * y + st[0]; st[0] = c[1] * y - c[3] * out + st[1]; st[1] = c[2] * y - c[4] * out; y = out;
        }
        const float delayed = delayLine[dp]; delayLine[dp] = (float) y; dp = (dp + 1) % delayLine.size();
        const float e = echo[ep]; echo[ep] = delayed; ep = (ep + 1) % echo.size();
        return 0.5f * (delayed + 0.3f * e);
    };

    SolutionAudioProcessor proc;
    proc.prepareToPlay (fs, bs);
    proc.setMicProfile ("measurement");
    proc.triggerRoomSweep (3.0f);
    EXPECT (proc.isRoomCapturing(), "measurement didn't start");

    juce::AudioBuffer<float> io (2, bs); io.clear();
    std::vector<float> mic ((size_t) bs, 0.0f);
    int guard = 0;
    while (proc.isRoomCapturing() && ++guard < 2000)
    {
        for (int i = 0; i < bs; ++i) { io.setSample (0, i, mic[(size_t) i]); io.setSample (1, i, mic[(size_t) i]); }   // mic -> input
        proc.processBlock (io, midi);                                                                                 // output -> speakers
        for (int i = 0; i < bs; ++i) mic[(size_t) i] = roomSample (io.getSample (0, i));
    }
    for (int k = 0; k < 500 && ! proc.pollRoomAnalysis(); ++k) juce::Thread::sleep (20);
    const auto& a = proc.getRoomAnalysis();
    EXPECT (a.valid, "analysis failed: %s", a.message.toRawUTF8());

    auto rangeMax = [&] (const auto& arr, float lo, float hi) {
        float m = 0.0f;
        for (int i = 0; i < SolutionDSP::RoomAnalysis::numPoints; ++i)
            if (a.freqs[(size_t) i] >= lo && a.freqs[(size_t) i] <= hi) m = std::max (m, std::abs (arr[(size_t) i]));
        return m;
    };
    const float before = rangeMax (a.measuredDb, 40.0f, 200.0f), after = rangeMax (a.resultDb, 40.0f, 200.0f);
    bool found60 = false, found120 = false;
    for (const auto& b : a.bands) { found60 |= (b.freq > 50 && b.freq < 72 && b.gainDb < -4); found120 |= (b.freq > 100 && b.freq < 145 && b.gainDb < -3); }
    std::printf ("  measured 40-200 Hz worst %.1f dB -> corrected %.1f dB, %d filters\n", before, after, (int) a.bands.size());
    EXPECT (before > 6.0f, "measurement didn't see the simulated room boom (%.1f dB)", before);
    EXPECT (after < 3.0f, "correction left %.1f dB of error between 40-200 Hz", after);
    EXPECT (found60 && found120, "didn't fit cuts at the 60 Hz and 120 Hz room modes");
    for (const auto& b : a.bands) EXPECT (b.gainDb <= 3.01f && b.gainDb >= -12.01f, "band gain %.1f dB out of range", b.gainDb);

    const auto measuredBands = a.bands;   // keep: switching mic below refits the live analysis

    // Mic compensation: switching mic refits instantly from the stored measurement, within that mic's limits.
    proc.setMicProfile ("laptop");
    for (const auto& b : proc.getRoomAnalysis().bands)
        EXPECT (b.freq >= 149.0f && b.gainDb >= -6.01f, "laptop mic: band at %.0f Hz / %.1f dB is outside its trusted range or cut limit", b.freq, b.gainDb);
    EXPECT (proc.getRoomAnalysis().micName.contains ("Laptop"), "mic name not recorded");

    // A calibration file that says "this mic has +6 dB at 60 Hz" should shrink the 60 Hz cut by about 6 dB.
    const auto calFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("solution_test_mic.cal");
    calFile.replaceWithText ("\"Sens Factor =-1.2dB\"\n* comment\n20 0\n40 0\n50 3\n60 6\n70 3\n90 0\n1000 0\n20000 0\n");
    EXPECT (proc.loadMicCalibration (calFile), "calibration file not accepted");
    float cut60 = 0.0f;
    for (const auto& b : proc.getRoomAnalysis().bands) if (b.freq > 50 && b.freq < 72) cut60 = std::min (cut60, b.gainDb);
    float cut60Before = 0.0f;
    for (const auto& b : measuredBands) if (b.freq > 50 && b.freq < 72) cut60Before = std::min (cut60Before, b.gainDb);
    EXPECT (cut60 > cut60Before + 3.0f, "calibration didn't reduce the 60 Hz cut (%.1f -> %.1f dB)", cut60Before, cut60);
    calFile.deleteFile();
    EXPECT (! proc.loadMicCalibration (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("missing.cal")), "missing calibration file should be rejected");
    proc.setMicProfile ("measurement");

    // Correction survives a session save/reload.
    juce::MemoryBlock state; proc.getStateInformation (state);
    SolutionAudioProcessor reloaded; reloaded.setStateInformation (state.getData(), (int) state.getSize());
    EXPECT (reloaded.getRoomAnalysis().valid && reloaded.getRoomAnalysis().bands.size() == measuredBands.size(), "room correction not restored from the session");

    // Correction actually filters audio when switched on (and not when off).
    reloaded.prepareToPlay (fs, bs);
    setParam (reloaded, "global_mix", 0.0f); setParam (reloaded, "stage_ceiling_guard", 0.0f);
    auto level60 = [&] { double db = 0; for (int k = 0; k < 60; ++k) { juce::AudioBuffer<float> b (2, bs); fillSine (b, fs, 60.0, 0.1f, 0.0, k * bs); reloaded.processBlock (b, midi); db = blockRmsDb (b); } return db; };
    const double off = level60();
    setParam (reloaded, "room_correct_on", 1.0f);
    const double on = level60();
    EXPECT (off - on > 4.0, "60 Hz should be cut when correction is on (cut %.1f dB)", off - on);

    // A silent mic gives a clear message instead of a bogus correction.
    SolutionAudioProcessor deaf; deaf.prepareToPlay (fs, bs); deaf.triggerRoomSweep (1.0f);
    guard = 0;
    while (deaf.isRoomCapturing() && ++guard < 2000) { io.clear(); deaf.processBlock (io, midi); }
    for (int k = 0; k < 500 && ! deaf.pollRoomAnalysis(); ++k) juce::Thread::sleep (20);
    EXPECT (! deaf.getRoomAnalysis().valid && deaf.getRoomAnalysis().message.contains ("silent"), "silent mic should report an error");
    std::printf ("  done\n");
}

static void testBinaural()
{
    std::printf ("== Binaural headphone rendering\n");
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        auto render = [&] (float az, float el, juce::AudioBuffer<float>& out)
        {
            SolutionDSP::BinauralRenderer r; r.prepare (fs);
            juce::Random rnd (11);
            out.setSize (2, (int) fs);
            for (int i = 0; i < out.getNumSamples(); ++i) { const float v = rnd.nextFloat() * 0.5f - 0.25f; out.setSample (0, i, v); out.setSample (1, i, v); }
            r.process (out.getWritePointer (0), out.getWritePointer (1), out.getNumSamples(), az, el, 0.0f);   // width 0: one point source
        };
        auto hfEnergyDb = [&] (const juce::AudioBuffer<float>& b, int ch) {   // simple first-difference high-pass
            double e = 0; for (int i = 1; i < b.getNumSamples(); ++i) { const double d = b.getSample (ch, i) - b.getSample (ch, i - 1); e += d * d; }
            return 10.0 * std::log10 (e + 1e-20);
        };
        auto lagOfMaxXcorr = [&] (const juce::AudioBuffer<float>& b) {   // >0 = left ear late
            int best = 0; double bestV = -1e9;
            for (int lag = -80; lag <= 80; ++lag) {
                double v = 0; for (int i = 200; i < b.getNumSamples() - 200; ++i) v += (double) b.getSample (1, i) * b.getSample (0, i + lag);
                if (v > bestV) { bestV = v; best = lag; }
            }
            return best;
        };

        juce::AudioBuffer<float> right90, front, back;
        render (90.0f, 0.0f, right90);
        const double ild = hfEnergyDb (right90, 1) - hfEnergyDb (right90, 0);
        const int lag = lagOfMaxXcorr (right90);
        const double expected = SolutionDSP::BinauralRenderer::itdSeconds (90.0) * fs;
        EXPECT (ild > 6.0, "source at +90 deg: right ear should be much brighter (ILD %.1f dB) at %.0f Hz", ild, fs);
        EXPECT (std::abs (lag - expected) <= 2.0, "source at +90 deg: left ear delay %d samples, expected ~%.1f at %.0f Hz", lag, expected, fs);

        render (0.0f, 0.0f, front);
        float maxDiff = 0.0f;
        for (int i = 0; i < front.getNumSamples(); ++i) maxDiff = std::max (maxDiff, std::abs (front.getSample (0, i) - front.getSample (1, i)));
        EXPECT (maxDiff < 1e-5f, "a source straight ahead should reach both ears identically (diff %g)", maxDiff);

        render (180.0f, 0.0f, back);
        EXPECT (hfEnergyDb (back, 0) < hfEnergyDb (front, 0) - 1.0, "a source behind should sound duller than in front at %.0f Hz", fs);
    }

    // Through the plugin: finite, no audio-thread allocation, and actually changes the image.
    SolutionAudioProcessor proc; proc.prepareToPlay (48000.0, 256);
    setParam (proc, "binaural_mode", 1.0f); setParam (proc, "stage_azimuth", 60.0f);
    juce::MidiBuffer midi; bool finite = true; int allocs = 0;
    for (int k = 0; k < 100; ++k)
    {
        juce::AudioBuffer<float> b (2, 256); fillSine (b, 48000.0, 2000.0, 0.2f, 0.0, k * 256);
        allocCount = 0; trackAllocs = true; proc.processBlock (b, midi); trackAllocs = false;
        if (k > 0) allocs += allocCount.load();
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 256; ++i) finite &= std::isfinite (b.getSample (ch, i));
        if (k == 99) EXPECT (blockRmsDb (b) > -40.0, "binaural mode went silent");
    }
    EXPECT (finite && allocs == 0, "binaural processing: finite=%d allocations=%d", (int) finite, allocs);
    std::printf ("  done\n");
}

static void testMixAssistant()
{
    std::printf ("== Mix Assistant\n");
    using namespace SolutionDSP;
    const double fs = 48000.0;
    juce::Random rnd (5);

    // White noise rises 1 dB per third-octave band; L=R has no side; L=-R is all side.
    {
        MixAnalyser an (fs);
        juce::AudioBuffer<float> b (2, (int) fs * 6);
        for (int i = 0; i < b.getNumSamples(); ++i) { const float v = rnd.nextFloat() * 0.4f - 0.2f; b.setSample (0, i, v); b.setSample (1, i, v); }
        an.addStereo (b.getReadPointer (0), b.getReadPointer (1), b.getNumSamples());
        const auto p = an.finish ("noise");
        const float slope = (p.midDb[27] - p.midDb[12]) / 15.0f;
        EXPECT (std::abs (slope - 1.0f) < 0.2f, "white noise should rise ~1 dB per third-octave band (got %.2f)", slope);
        EXPECT (p.widthDb() < -40.0f, "identical channels should have no side signal (width %.1f dB)", p.widthDb());
        EXPECT (std::abs (p.lufs - (-10.0f)) < 3.0f && p.valid, "noise loudness looks wrong (%.1f LUFS)", p.lufs);
    }

    // Reference track from a real WAV file.
    const auto wav = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("solution_ref_test.wav");
    {
        juce::AudioBuffer<float> b (2, (int) fs * 8);
        for (int i = 0; i < b.getNumSamples(); ++i) { const float m = rnd.nextFloat() * 0.3f - 0.15f, sd = rnd.nextFloat() * 0.2f - 0.1f; b.setSample (0, i, m + sd); b.setSample (1, i, m - sd); }
        wav.deleteFile();
        std::unique_ptr<juce::FileOutputStream> os (wav.createOutputStream());
        std::unique_ptr<juce::AudioFormatWriter> w (juce::WavAudioFormat().createWriterFor (os.get(), fs, 2, 24, {}, 0));
        if (w != nullptr) { os.release(); w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples()); }
    }
    juce::String err;
    const auto ref = analyseReferenceFile (wav, err);
    EXPECT (ref.valid && std::abs (ref.seconds - 8.0) < 0.1, "reference WAV analysis failed: %s", err.toRawUTF8());
    wav.deleteFile();
    EXPECT (! analyseReferenceFile (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("nope.wav"), err).valid && err.isNotEmpty(), "missing file should give an error");

    // A dull, narrow, very dynamic mix vs that bright, wide, dense reference.
    SolutionAudioProcessor proc; proc.prepareToPlay (fs, 512);
    setParam (proc, "global_mix", 0.0f); setParam (proc, "stage_ceiling_guard", 0.0f);
    proc.assistant.reference = ref;
    proc.assistantStartListening();
    juce::MidiBuffer midi;
    float lp = 0.0f;
    for (int k = 0; k < (int) (fs * 8 / 512); ++k)
    {
        juce::AudioBuffer<float> b (2, 512);
        const float level = (k / 40) % 2 == 0 ? 0.3f : 0.03f;              // very dynamic
        for (int i = 0; i < 512; ++i) { lp += 0.08f * ((rnd.nextFloat() * 2.0f - 1.0f) - lp); b.setSample (0, i, level * lp); b.setSample (1, i, level * lp * 0.97f); }   // dull + narrow
        proc.processBlock (b, midi);
        if (k % 50 == 0) proc.assistantPoll();
    }
    proc.assistantPoll();
    const auto& sug = proc.assistant.comparison.suggestions;
    float elevation = 0, width = 0, glue = 0; bool hasE = false, hasW = false, hasG = false;
    for (const auto& sgn : sug)
    {
        if (sgn.paramId == "stage_elevation") { hasE = true; elevation = sgn.newValue; }
        if (sgn.paramId == "stage_width")     { hasW = true; width = sgn.newValue; }
        if (sgn.paramId == "glue_amount")     { hasG = true; glue = sgn.newValue; }
    }
    EXPECT (hasE && elevation > 10.0f, "dull mix: should suggest more air via Elevation (got %d / %.1f)", (int) hasE, elevation);
    EXPECT (hasW && width > 1.2f, "narrow mix: should suggest more width (got %d / %.2f)", (int) hasW, width);
    EXPECT (hasG && glue > 0.1f, "dynamic mix: should suggest Glue (got %d / %.2f)", (int) hasG, glue);
    EXPECT (proc.assistantApply (-1), "apply all did nothing");
    auto real = [&] (const char* id) { auto* p = proc.apvts.getParameter (id); return p->convertFrom0to1 (p->getValue()); };
    EXPECT (std::abs (real ("stage_width") - width) < 0.02f && std::abs (real ("glue_amount") - glue) < 0.02f, "apply didn't set the suggested values");

    // Genre targets exist and compare without crashing.
    for (const auto& g : genreTargetNames())
        EXPECT (genreTarget (g).valid, "genre target %s invalid", g.toRawUTF8());
    std::printf ("  done\n");
}

static void testStemFocus()
{
    std::printf ("== Stem Focus (drums / tonal / vocal split)\n");
    using SolutionDSP::StemSplitter;
    const double fs = 48000.0; const int bs = 300;   // odd block size on purpose
    const int D = StemSplitter::latency;

    auto run = [&] (StemSplitter::Mode mode, const juce::AudioBuffer<float>& in, juce::AudioBuffer<float>& focus, juce::AudioBuffer<float>& rest, int* allocs = nullptr)
    {
        StemSplitter sp; sp.prepare (fs, bs);
        focus.makeCopyOf (in); rest.setSize (2, in.getNumSamples()); rest.clear();
        for (int start = 0; start < in.getNumSamples(); start += bs)
        {
            const int n = std::min (bs, in.getNumSamples() - start);
            if (allocs) { allocCount = 0; trackAllocs = true; }
            sp.process (focus.getWritePointer (0) + start, focus.getWritePointer (1) + start, rest.getWritePointer (0) + start, rest.getWritePointer (1) + start, n, mode);
            if (allocs) { trackAllocs = false; *allocs += allocCount.load(); }
        }
    };
    auto energyDb = [] (const juce::AudioBuffer<float>& b, int from) {
        double e = 0; for (int ch = 0; ch < 2; ++ch) for (int i = from; i < b.getNumSamples(); ++i) e += (double) b.getSample (ch, i) * b.getSample (ch, i);
        return 10.0 * std::log10 (e + 1e-20);
    };

    // 1. Lossless: focus + rest == input delayed by exactly `latency`, in every mode.
    juce::Random rnd (21);
    juce::AudioBuffer<float> noise (2, (int) fs);
    for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < noise.getNumSamples(); ++i) noise.setSample (ch, i, rnd.nextFloat() - 0.5f);
    for (auto mode : { StemSplitter::drums, StemSplitter::tonal, StemSplitter::vocal })
    {
        juce::AudioBuffer<float> f, r; int allocs = 0;
        run (mode, noise, f, r, &allocs);
        float err = 0.0f;
        for (int ch = 0; ch < 2; ++ch) for (int i = D; i < noise.getNumSamples(); ++i)
            err = std::max (err, std::abs (f.getSample (ch, i) + r.getSample (ch, i) - noise.getSample (ch, i - D)));
        EXPECT (err < 1e-5f, "mode %d: focus + rest differs from the input by %g", (int) mode, err);
        EXPECT (allocs == 0, "mode %d: %d allocations on the audio thread", (int) mode, allocs);
    }

    // 2. Drums vs tonal on a steady tone and on isolated clicks.
    juce::AudioBuffer<float> tone (2, (int) fs * 2), clicks (2, (int) fs * 2);
    fillSine (tone, fs, 440.0, 0.3f);
    clicks.clear();
    for (int i = 6000; i < clicks.getNumSamples(); i += 12000) for (int k = 0; k < 24; ++k) { const float v = 0.8f * std::exp (-k / 6.0f) * (k % 2 ? -1.0f : 1.0f); clicks.setSample (0, i + k, v); clicks.setSample (1, i + k, v); }
    juce::AudioBuffer<float> f, r;
    const int skip = D + 4096;
    run (StemSplitter::drums, tone, f, r);   const double toneInDrums = energyDb (f, skip) - energyDb (tone, skip);
    run (StemSplitter::drums, clicks, f, r); const double clicksInDrums = energyDb (f, skip) - energyDb (clicks, skip);
    run (StemSplitter::tonal, tone, f, r);   const double toneInTonal = energyDb (f, skip) - energyDb (tone, skip);
    run (StemSplitter::tonal, clicks, f, r); const double clicksInTonal = energyDb (f, skip) - energyDb (clicks, skip);
    std::printf ("  drums stem: clicks %.1f dB, tone %.1f dB | tonal stem: tone %.1f dB, clicks %.1f dB\n", clicksInDrums, toneInDrums, toneInTonal, clicksInTonal);
    EXPECT (clicksInDrums > -4.0 && toneInDrums < -15.0, "drums focus should take the clicks and leave the tone");
    EXPECT (toneInTonal > -1.0 && clicksInTonal < -6.0, "tonal focus should take the tone and leave the clicks");

    // 3. Vocal/centre: centred 500 Hz kept, hard-left 1200 Hz left out.
    juce::AudioBuffer<float> mixIn (2, (int) fs * 2);
    for (int i = 0; i < mixIn.getNumSamples(); ++i)
    {
        const float c = 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 500.0 * i / fs);
        const float side = 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 1200.0 * i / fs);
        mixIn.setSample (0, i, c + side); mixIn.setSample (1, i, c);
    }
    run (StemSplitter::vocal, mixIn, f, r);
    auto toneLevel = [&] (const juce::AudioBuffer<float>& b, int ch, double hz) {   // single-bin DFT
        std::complex<double> acc; const int start = skip, n = b.getNumSamples() - skip;
        for (int i = start; i < b.getNumSamples(); ++i) acc += (double) b.getSample (ch, i) * std::polar (1.0, -juce::MathConstants<double>::twoPi * hz * i / fs);
        return 20.0 * std::log10 (std::abs (acc) / n * 2.0 + 1e-12);
    };
    const double centreKept = toneLevel (f, 1, 500.0) - toneLevel (mixIn, 1, 500.0);
    const double sideLeaked = toneLevel (f, 0, 1200.0) - toneLevel (mixIn, 0, 1200.0);
    std::printf ("  vocal stem: centre tone %.1f dB, hard-panned tone %.1f dB\n", centreKept, sideLeaked);
    EXPECT (centreKept > -3.0 && sideLeaked < -12.0, "vocal/centre focus should keep the centred tone and drop the panned one");

    // 4. Plugin: reports the extra latency only while on, and stays aligned to it.
    SolutionAudioProcessor proc; proc.prepareToPlay (fs, 512);
    const int base = proc.getLatencySamples();
    setParam (proc, "stem_focus", 1.0f); setParam (proc, "global_mix", 0.0f); setParam (proc, "stage_ceiling_guard", 0.0f);
    juce::MidiBuffer midi; juce::AudioBuffer<float> b (2, 512);
    int found = -1;
    for (int k = 0; k < 12 && found < 0; ++k)
    {
        b.clear(); if (k == 0) { b.setSample (0, 0, 0.5f); b.setSample (1, 0, 0.5f); }
        proc.processBlock (b, midi);
        if (k == 0) proc.updateLatencyNow();          // the async update would do this on the message thread
        for (int i = 0; i < 512; ++i) if (std::abs (b.getSample (0, i)) > 0.25f) { found = k * 512 + i; break; }
    }
    EXPECT (proc.getLatencySamples() == base + D, "latency with Stem Focus on: %d, expected %d", proc.getLatencySamples(), base + D);
    EXPECT (found == proc.getLatencySamples(), "audio arrived after %d samples but %d was reported", found, proc.getLatencySamples());
    setParam (proc, "stem_focus", 0.0f); proc.processBlock (b, midi); proc.updateLatencyNow();
    EXPECT (proc.getLatencySamples() == base, "latency should drop back to %d when Stem Focus is off (got %d)", base, proc.getLatencySamples());
    std::printf ("  done\n");
}

static void testNeuralCharacter()
{
    std::printf ("== Neural Character models\n");
    using SolutionDSP::NeuralCharacter;
    const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("solution_models_test");
    tmp.createDirectory();
    const auto half = tmp.getChildFile ("Half.json"), soft = tmp.getChildFile ("Soft.json"), bad = tmp.getChildFile ("Broken.json"), stereo = tmp.getChildFile ("TwoIn.json");
    half.replaceWithText (R"({"in_shape":[null,null,1],"samplerate":48000,"layers":[{"type":"dense","activation":"","shape":[null,null,1],"weights":[[[0.5]],[0.0]]}]})");
    soft.replaceWithText (R"({"in_shape":[null,null,1],"layers":[{"type":"dense","activation":"tanh","shape":[null,null,1],"weights":[[[2.0]],[0.0]]}]})");
    bad.replaceWithText ("{ this is not json");
    stereo.replaceWithText (R"({"in_shape":[null,null,2],"layers":[{"type":"dense","activation":"","shape":[null,null,1],"weights":[[[1.0],[1.0]],[0.0]]}]})");

    juce::String err;
    EXPECT (NeuralCharacter::load (half, err) != nullptr, "valid model failed to load: %s", err.toRawUTF8());
    err = {}; EXPECT (NeuralCharacter::load (bad, err) == nullptr && err.isNotEmpty(), "broken JSON should be rejected with a message");
    err = {}; EXPECT (NeuralCharacter::load (stereo, err) == nullptr && err.contains ("1 input"), "2-input model should be rejected");

    NeuralCharacter nc;
    juce::AudioBuffer<float> b (2, 256);
    auto runBlock = [&] (float drive, float amount) { fillSine (b, 48000.0, 300.0, 0.4f); allocCount = 0; trackAllocs = true; nc.process (b.getWritePointer (0), b.getWritePointer (1), 256, drive, amount); trackAllocs = false; return allocCount.load(); };

    nc.setModel (NeuralCharacter::load (half, err));
    int allocs = runBlock (0.0f, 1.0f);
    juce::AudioBuffer<float> ref (2, 256); fillSine (ref, 48000.0, 300.0, 0.4f);
    float maxErr = 0.0f; for (int i = 0; i < 256; ++i) maxErr = std::max (maxErr, std::abs (b.getSample (0, i) - 0.5f * ref.getSample (0, i)));
    EXPECT (maxErr < 1e-5f, "x0.5 model output wrong (err %g)", maxErr);
    EXPECT (std::abs (nc.getTrainedSampleRate() - 48000.0) < 1.0, "model sample rate not read");
    allocs += runBlock (6.0206f, 1.0f);   // +6 dB drive -> 0.5 * 2x = unity
    maxErr = 0.0f; for (int i = 0; i < 256; ++i) maxErr = std::max (maxErr, std::abs (b.getSample (1, i) - ref.getSample (1, i)));
    EXPECT (maxErr < 1e-4f, "drive into model wrong (err %g)", maxErr);
    allocs += runBlock (0.0f, 0.0f);
    maxErr = 0.0f; for (int i = 0; i < 256; ++i) maxErr = std::max (maxErr, std::abs (b.getSample (0, i) - ref.getSample (0, i)));
    EXPECT (maxErr == 0.0f, "amount 0 must be an exact bypass");

    nc.setModel (NeuralCharacter::load (soft, err));                 // live swap
    allocs += runBlock (0.0f, 1.0f);
    for (int i = 0; i < 256; ++i) maxErr = std::max (maxErr, std::abs (b.getSample (0, i) - std::tanh (2.0f * ref.getSample (0, i))));
    EXPECT (maxErr < 1e-4f && nc.getModelName() == "Soft", "model swap failed");
    EXPECT (allocs == 0, "Character processing / model swap allocated %d times on the audio thread", allocs);
    nc.housekeeping();
    nc.setModel (nullptr); runBlock (0.0f, 1.0f); nc.housekeeping();
    EXPECT (nc.getModelName().isEmpty(), "clearing the model failed");

    // Through the plugin: model chosen by name from the Models folder, restored with the session.
    const auto folder = NeuralCharacter::getModelsFolder();
    const bool folderExisted = folder.isDirectory();
    folder.createDirectory();
    const auto installed = folder.getChildFile ("SolutionTestHalf.json");
    half.copyFileTo (installed);
    SolutionAudioProcessor proc; proc.prepareToPlay (48000.0, 256);
    EXPECT (proc.setCharacterModel ("SolutionTestHalf").isEmpty(), "plugin failed to load a model from the Models folder");
    EXPECT (proc.setCharacterModel ("DoesNotExist").isNotEmpty(), "missing model should report an error");
    proc.setCharacterModel ("SolutionTestHalf");
    juce::MemoryBlock state; proc.getStateInformation (state);
    SolutionAudioProcessor reloaded; reloaded.setStateInformation (state.getData(), (int) state.getSize());
    EXPECT (reloaded.getCharacterModelName() == "SolutionTestHalf", "Character model not restored with the session");
    installed.deleteFile();
    if (! folderExisted) folder.deleteRecursively();
    tmp.deleteRecursively();
    std::printf ("  done\n");
}

struct HttpResult { int status = 0; juce::String body; };
static HttpResult http (int port, const juce::String& method, const juce::String& path, const juce::String& body = {}, const juce::String& token = {})
{
    juce::StreamingSocket s;
    HttpResult r;
    if (! s.connect ("127.0.0.1", port, 2000)) return r;
    juce::String req;
    req << method << " " << path << " HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: " << (int) body.getNumBytesAsUTF8() << "\r\n";
    if (token.isNotEmpty()) req << "Authorization: Bearer " << token << "\r\n";
    req << "\r\n" << body;
    s.write (req.toRawUTF8(), (int) req.getNumBytesAsUTF8());
    juce::MemoryBlock got; char buf[4096];
    while (s.waitUntilReady (true, 3000) > 0) { const int n = s.read (buf, sizeof (buf), false); if (n <= 0) break; got.append (buf, (size_t) n); }
    const auto text = got.toString();
    r.status = text.fromFirstOccurrenceOf (" ", false, false).getIntValue();
    r.body = text.fromFirstOccurrenceOf ("\r\n\r\n", false, false);
    return r;
}

static void testRemote()
{
    std::printf ("== Phone / tablet remote\n");
    SolutionAudioProcessor proc;
    proc.remoteExecutor = [] (std::function<void()> fn) { fn(); };    // run commands immediately in the test
    EXPECT (proc.startRemote ("127.0.0.1"), "remote server didn't start");
    auto* srv = proc.getRemote();
    const int port = srv->getPort();
    EXPECT (srv->getPairingCode().length() == 6, "pairing code should be 6 digits");

    EXPECT (http (port, "GET", "/").status == 200, "page not served");
    EXPECT (http (port, "GET", "/api/state").status == 401, "state must need pairing");
    EXPECT (http (port, "POST", "/api/param", R"({"id":"stage_width","value":2})", "0123456789abcdef0123456789abcdef").status == 401, "made-up token must be rejected");

    const auto paired = http (port, "POST", "/api/pair", "{\"code\":\"" + srv->getPairingCode() + "\"}");
    const auto token = juce::JSON::parse (paired.body).getProperty ("token", {}).toString();
    EXPECT (paired.status == 200 && token.length() == 32, "pairing with the right code failed (%d)", paired.status);
    EXPECT (srv->getConnectedDevices() == 1, "connected device count wrong");

    const auto state = http (port, "GET", "/api/state", {}, token);
    const auto json = juce::JSON::parse (state.body);
    EXPECT (state.status == 200 && json.getProperty ("params", {}).size() == proc.getParameters().size(), "state should list every parameter");

    auto realValue = [&] (const char* id) { auto* p = proc.apvts.getParameter (id); return p->convertFrom0to1 (p->getValue()); };
    EXPECT (http (port, "POST", "/api/param", R"({"id":"stage_width","value":1.5})", token).status == 200 && std::abs (realValue ("stage_width") - 1.5f) < 0.011f, "phone couldn't set a parameter");
    http (port, "POST", "/api/param", R"({"id":"stage_width","value":99})", token);
    EXPECT (std::abs (realValue ("stage_width") - 2.0f) < 0.011f, "out-of-range values must be clamped");
    EXPECT (http (port, "POST", "/api/param", R"({"id":"","value":1})", token).status == 400, "bad request should be rejected");
    http (port, "POST", "/api/preset", R"({"name":"Wide Chorus Lift"})", token);
    EXPECT (proc.presets.getCurrentPresetName() == "Wide Chorus Lift", "phone couldn't load a preset");
    EXPECT (http (port, "POST", "/api/param", juce::String::repeatedString ("x", 9000), token).status == 413, "oversized request must be refused");
    {   // Skin Studio: a phone photo (base64 JPEG) becomes custom_<name>.jpg and the plugin switches to it.
        juce::Image img (juce::Image::RGB, 270, 160, true); juce::Graphics g (img); g.fillAll (juce::Colours::darkorange);
        juce::MemoryOutputStream jpg; juce::JPEGImageFormat fmt; fmt.writeImageToStream (img, jpg);
        juce::String bodyJson; bodyJson << "{\"name\":\"Test Skin!\",\"jpeg\":\"" << juce::Base64::toBase64 (jpg.getData(), jpg.getDataSize()) << "\",\"accent\":\"#ff6a1a\",\"accent2\":\"bad\"}";
        const auto r = http (port, "POST", "/api/skin", bodyJson, token);
        const auto file = SolutionVendor::userFolder ("Skins").getChildFile ("custom_testskin.jpg");
        const auto meta = juce::JSON::parse (SolutionVendor::userFolder ("Skins").getChildFile ("custom_skins.json"));
        EXPECT (r.status == 200 && file.existsAsFile() && proc.pendingSkin == "custom_testskin", "skin upload saved (%d) and queued", r.status);
        if (r.status != 200) std::printf ("  skin upload reply %d: %s\n", r.status, r.body.toRawUTF8());
        EXPECT (meta.isArray() && meta.size() > 0 && meta[0]["accent"].toString() == "#ff6a1a" && meta[0]["accent2"].toString() == "#38BDF8", "accent kept, bad colour replaced");
        EXPECT (http (port, "POST", "/api/skin", "{\"name\":\"x\",\"jpeg\":\"aGVsbG8=\"}", token).status == 400, "non-JPEG refused");
        file.deleteFile();
    }

    // Wrong codes: 5 strikes then a lockout (even the right code is refused for a minute).
    for (int i = 0; i < 5; ++i) http (port, "POST", "/api/pair", R"({"code":"000000"})");
    EXPECT (http (port, "POST", "/api/pair", "{\"code\":\"" + srv->getPairingCode() + "\"}").status == 429, "brute-force lockout not applied");

    proc.stopRemote();
    EXPECT (http (port, "GET", "/").status == 0, "server still answering after stop");
    std::printf ("  done\n");
}

static void testModeNotchSwitch()
{
    std::printf ("== 115 Hz mode notch: Off / On / Auto\n");
    juce::Random rnd (77);
    // Through a translation profile at 100% Room Matcher mix. Returns {level at 115 Hz, auto depth}.
    auto run = [&] (int mode, bool boomy, double probeHz)
    {
        SolutionAudioProcessor proc; proc.prepareToPlay (48000.0, 512);
        EXPECT (proc.apvts.getParameter ("mode_notch")->convertFrom0to1 (proc.apvts.getParameter ("mode_notch")->getValue()) == 2.0f, "notch should default to Auto");
        setParam (proc, "room_profile", 1.0f); setParam (proc, "room_match_mix", 1.0f);
        setParam (proc, "stage_ceiling_guard", 0.0f); setParam (proc, "mode_notch", (float) mode);
        juce::MidiBuffer midi; double db = 0.0; float depth = 0.0f; std::complex<double> acc115;
        // Pink-ish noise bed (balanced bass) plus, if boomy, a strong 115 Hz build-up. Measure a probe tone level.
        double b0 = 0, b1 = 0, b2 = 0;
        for (int k = 0; k < 150; ++k)
        {
            juce::AudioBuffer<float> b (2, 512), probe (2, 512);
            fillSine (probe, 48000.0, probeHz, 0.02f, 0.0, k * 512);
            for (int i = 0; i < 512; ++i)
            {
                const double w = rnd.nextFloat() * 2.0 - 1.0;          // Paul Kellet's cheap pink filter
                b0 = 0.99765 * b0 + w * 0.0990460; b1 = 0.96300 * b1 + w * 0.2965164; b2 = 0.57000 * b2 + w * 1.0526913;
                float v = (float) (0.02 * (b0 + b1 + b2 + w * 0.1848));
                if (boomy) v += 0.15f * (float) std::sin (juce::MathConstants<double>::twoPi * 115.0 * (k * 512 + i) / 48000.0);
                b.setSample (0, i, v + probe.getSample (0, i)); b.setSample (1, i, v + probe.getSample (1, i));
            }
            proc.processBlock (b, midi);
            if (k >= 100)    // level of the 115 Hz component itself (single-bin DFT over the last 50 blocks)
                for (int i = 0; i < 512; ++i) acc115 += (double) b.getSample (0, i) * std::polar (1.0, -juce::MathConstants<double>::twoPi * 115.0 * (k * 512 + i) / 48000.0);
            depth = proc.getAutoNotchDepth();
        }
        db = 20.0 * std::log10 (std::abs (acc115) / (50.0 * 512) * 2.0 + 1e-12);
        return std::pair<double, float> { db, depth };
    };
    const auto calm = run (2, false, 1000.0);
    const auto boom = run (2, true, 1000.0);
    std::printf ("  auto depth: balanced mix %.2f, 115 Hz build-up %.2f\n", calm.second, boom.second);
    EXPECT (calm.second < 0.15f, "auto notch should stay out of a balanced mix (depth %.2f)", calm.second);
    EXPECT (boom.second > 0.9f, "auto notch should fully engage on a 115 Hz build-up (depth %.2f)", boom.second);

    // Level of the 115 Hz boom itself: On and full Auto both cut it by the original 2.85 dB.
    const double off = run (0, true, 1000.0).first, on = run (1, true, 1000.0).first, autoLvl = boom.first;
    std::printf ("  115 Hz level: off %.2f dB, on %.2f dB, auto %.2f dB\n", off, on, autoLvl);
    EXPECT (std::abs ((off - on) - 2.85) < 0.3, "On should cut 115 Hz by 2.85 dB (got %.2f)", off - on);
    EXPECT (std::abs (autoLvl - on) < 0.3, "Auto at full depth should match On at 115 Hz (%.2f vs %.2f dB)", autoLvl, on);
    std::printf ("  done\n");
}

static double toneDb (const juce::AudioBuffer<float>& b, int ch, double hz, double fs, int from)
{
    std::complex<double> acc; const int n = b.getNumSamples() - from;
    for (int i = from; i < b.getNumSamples(); ++i) acc += (double) b.getSample (ch, i) * std::polar (1.0, -juce::MathConstants<double>::twoPi * hz * i / fs);
    return 20.0 * std::log10 (std::abs (acc) / n * 2.0 + 1e-12);
}

static void testProTricks()
{
    std::printf ("== Pro Tricks\n");
    using namespace SolutionDSP;
    const double fs = 48000.0; const int bs = 480;
    juce::MidiBuffer midi; juce::Random rnd (99);

    // Run a processor over `seconds` of generated audio; returns the last second.
    auto render = [&] (SolutionAudioProcessor& proc, double seconds, std::function<void (juce::AudioBuffer<float>&, int)> gen)
    {
        const int blocks = (int) (seconds * fs / bs);
        juce::AudioBuffer<float> tail (2, (int) fs);
        for (int k = 0; k < blocks; ++k)
        {
            juce::AudioBuffer<float> b (2, bs); gen (b, k * bs); proc.processBlock (b, midi);
            const int pos = k * bs - (blocks * bs - (int) fs);
            if (pos >= 0) for (int ch = 0; ch < 2; ++ch) tail.copyFrom (ch, pos, b, ch, 0, bs);
        }
        return tail;
    };
    auto noise = [&] (juce::AudioBuffer<float>& b, int) { for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < b.getNumSamples(); ++i) b.setSample (ch, i, 0.1f * (rnd.nextFloat() * 2 - 1)); };

    // A/B: processed is +6 dB louder; the loudness-matched original must play at the processed level.
    {
        SolutionAudioProcessor proc; proc.prepareToPlay (fs, bs);
        setParam (proc, "stage_ms_mid", 6.0f); setParam (proc, "stage_ms_side", 6.0f); setParam (proc, "stage_ceiling_guard", 0.0f);
        const double processedDb = blockRmsDb (render (proc, 5.0, noise));
        setParam (proc, "ab_bypass", 1.0f);
        const double originalDb = blockRmsDb (render (proc, 4.0, noise));
        std::printf ("  A/B: processed %.1f dB, matched original %.1f dB (match %+.1f dB)\n", processedDb, originalDb, proc.abMatchDb.load());
        EXPECT (proc.abMatchDb.load() > 4.0f && proc.abMatchDb.load() < 8.0f, "A/B match should be about +6 dB (got %.1f)", proc.abMatchDb.load());
        EXPECT (std::abs (processedDb - originalDb) < 1.5, "A/B original should play as loud as the processed sound");
    }
    // Delta: with Global Wet/Dry at 0 nothing changes, so Delta must be silent.
    {
        SolutionAudioProcessor proc; proc.prepareToPlay (fs, bs);
        setParam (proc, "global_mix", 0.0f); setParam (proc, "delta_listen", 1.0f);
        EXPECT (blockRmsDb (render (proc, 1.0, noise)) < -90.0, "Delta should be silent when the module changes nothing");
    }
    // Mono the lows: 45 Hz in the left only becomes centred (crossover at 150 Hz); 3 kHz in the left stays left.
    {
        SolutionAudioProcessor proc; proc.prepareToPlay (fs, bs);
        setParam (proc, "global_mix", 0.0f); setParam (proc, "stage_ceiling_guard", 0.0f); setParam (proc, "mono_lows_hz", 150.0f);
        auto tail = render (proc, 2.0, [&] (juce::AudioBuffer<float>& b, int off) {
            for (int i = 0; i < b.getNumSamples(); ++i) {
                const double t = (off + i) / fs;
                b.setSample (0, i, (float) (0.3 * std::sin (juce::MathConstants<double>::twoPi * 45 * t) + 0.3 * std::sin (juce::MathConstants<double>::twoPi * 3000 * t)));
                b.setSample (1, i, 0.0f); } });
        const double lowL = toneDb (tail, 0, 45, fs, 0), lowR = toneDb (tail, 1, 45, fs, 0), hiR = toneDb (tail, 1, 3000, fs, 0), hiL = toneDb (tail, 0, 3000, fs, 0);
        EXPECT (std::abs (lowL - lowR) < 0.5, "45 Hz should be equal in both channels (L %.1f / R %.1f dB)", lowL, lowR);
        EXPECT (hiL - hiR > 40.0, "3 kHz should stay in the left channel only (L %.1f / R %.1f dB)", hiL, hiR);
    }
    // Phase Guard: an anti-phase mix (cancels in mono) gets its width pulled back.
    {
        SolutionAudioProcessor proc; proc.prepareToPlay (fs, bs);
        setParam (proc, "global_mix", 0.0f); setParam (proc, "stage_ceiling_guard", 0.0f); setParam (proc, "phase_guard", 1.0f);
        auto tail = render (proc, 2.0, [&] (juce::AudioBuffer<float>& b, int) { for (int i = 0; i < b.getNumSamples(); ++i) { const float m = 0.03f * (rnd.nextFloat() * 2 - 1), sd = 0.1f * (rnd.nextFloat() * 2 - 1); b.setSample (0, i, m + sd); b.setSample (1, i, m - sd); } });
        double sll = 0, srr = 0, slr = 0;
        for (int i = 0; i < tail.getNumSamples(); ++i) { const double l = tail.getSample (0, i), r = tail.getSample (1, i); sll += l * l; srr += r * r; slr += l * r; }
        const double corr = slr / std::sqrt (sll * srr);
        std::printf ("  Phase Guard: correlation %.2f (input about -0.8), width %.1f dB\n", corr, proc.guardDb.load());
        EXPECT (corr > -0.2 && proc.guardDb.load() < -3.0f, "Phase Guard should pull an out-of-phase mix back toward mono-safe");
    }
    // Smooth: harsh material is tamed, dull material untouched.
    {
        Smoother sm; sm.prepare (fs);
        juce::AudioBuffer<float> harsh (2, (int) fs), dull (2, (int) fs);
        BandPass emph; emph.set (fs, 6500.0, 1.0); Biquad lp; lp.lowPass (fs, 1500.0);
        for (int i = 0; i < (int) fs; ++i) { const double w = rnd.nextFloat() * 2 - 1; const float h = (float) (0.05 * w + 0.4 * emph.run (w)), d = (float) (0.3 * lp.run (w)); harsh.setSample (0, i, h); harsh.setSample (1, i, h); dull.setSample (0, i, d); dull.setSample (1, i, d); }
        sm.process (harsh.getWritePointer (0), harsh.getWritePointer (1), harsh.getNumSamples(), 1.0f);
        const float harshCut = sm.getReductionDb();
        sm.reset(); sm.process (dull.getWritePointer (0), dull.getWritePointer (1), dull.getNumSamples(), 1.0f);
        std::printf ("  Smooth: harsh cut %.1f dB, dull cut %.1f dB\n", harshCut, sm.getReductionDb());
        EXPECT (harshCut < -3.0f && sm.getReductionDb() > -0.5f, "Smooth should act on harsh material only");
    }
    // Phone Bass: a 60 Hz note gains audible harmonics (180 Hz) only when on.
    {
        PhoneBass pb; pb.prepare (fs);
        juce::AudioBuffer<float> on (2, (int) fs), off (2, (int) fs);
        fillSine (on, fs, 60.0, 0.3f); fillSine (off, fs, 60.0, 0.3f);
        pb.process (on.getWritePointer (0), on.getWritePointer (1), on.getNumSamples(), 1.0f);
        const double h3on = toneDb (on, 0, 180.0, fs, 4800), h3off = toneDb (off, 0, 180.0, fs, 4800);
        std::printf ("  Phone Bass: 180 Hz harmonic %.1f dB (off: %.1f dB)\n", h3on, h3off);
        EXPECT (h3on > -30.0 && h3on - h3off > 40.0, "Phone Bass should add harmonics of the bass");
    }
    // Streaming preview maths.
    {
        const auto& sv = streamingServices();
        EXPECT (std::abs (streamingGainDb (sv[0], -8.0f, -0.5f) + 6.0f) < 0.01f, "Spotify should turn a -8 LUFS master down 6 dB");
        EXPECT (std::abs (streamingGainDb (sv[0], -20.0f, -6.0f) - 5.0f) < 0.01f, "Spotify should turn a quiet master up only to -1 dBTP (5 dB)");
        EXPECT (streamingGainDb (sv[2], -20.0f, -6.0f) == 0.0f, "YouTube never turns quiet tracks up");
        EXPECT (std::abs (streamingGainDb (sv[1], -20.0f, -2.0f) - 1.0f) < 0.01f, "Apple Music limited by peak headroom");
        EXPECT (streamingGainDb (sv[0], -20.0f, 0.0f) == 0.0f, "quiet but peaky track must not be turned down");
    }
    // Auto gain stage: -30 dBFS input -> +12 dB in, -12 dB out.
    {
        SolutionAudioProcessor proc; proc.prepareToPlay (fs, bs);
        proc.startAutoGainStage();
        render (proc, 3.5, [&] (juce::AudioBuffer<float>& b, int off) { fillSine (b, fs, 440.0, 0.04472f, 0.0, off); });   // -30 dBFS RMS
        EXPECT (proc.pollAutoGainStage(), "auto gain stage didn't finish");
        auto realValue = [&] (const char* id) { auto* q = proc.apvts.getParameter (id); return q->convertFrom0to1 (q->getValue()); };
        EXPECT (std::abs (realValue ("trim_in_db") - 12.0f) < 0.3f && std::abs (realValue ("trim_out_db") + 12.0f) < 0.3f,
                "auto gain stage set %.1f / %.1f dB (expected +12 / -12)", realValue ("trim_in_db"), realValue ("trim_out_db"));
    }
    std::printf ("  done\n");
}

static void testListeningRooms()
{
    std::printf ("== Listening Room library\n");
    using namespace SolutionDSP;
    const auto& rooms = listeningRooms();
    EXPECT (rooms.size() >= 20, "expected 20+ rooms, got %d", (int) rooms.size());
    juce::Random rnd (31);
    for (double fs : { 48000.0, 96000.0 })
        for (int idx = 0; idx < (int) rooms.size(); ++idx)
        {
            ListeningRoomSim sim; sim.prepare (fs);
            const int bs = 256, total = (int) (fs * 1.0), tailLen = (int) (fs * (rooms[(size_t) idx].rt60 * 1.6 + 0.6));
            juce::AudioBuffer<float> b (2, bs);
            bool finite = true, mono = true; int allocs = 0; double energy = 0, tailEnergy = 0;
            for (int pos = 0; pos < total + tailLen; pos += bs)
            {
                for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < bs; ++i) b.setSample (ch, i, pos < total ? 0.1f * (rnd.nextFloat() * 2 - 1) : 0.0f);
                allocCount = 0; trackAllocs = true;
                sim.process (b.getWritePointer (0), b.getWritePointer (1), bs, idx);
                trackAllocs = false; allocs += allocCount.load();
                for (int i = 0; i < bs; ++i)
                {
                    const float l = b.getSample (0, i), r = b.getSample (1, i);
                    finite &= std::isfinite (l) && std::isfinite (r);
                    const bool strictMono = rooms[(size_t) idx].mono && rooms[(size_t) idx].length <= 0.0f;   // in a room, reflections reach each ear differently
                    if (strictMono && pos > (int) (0.02 * fs)) mono &= std::abs (l - r) < 1e-6f;
                    if (pos < total) energy += (double) l * l; else if (pos + bs >= total + tailLen) tailEnergy += (double) l * l;
                }
            }
            const char* nm = rooms[(size_t) idx].name;
            EXPECT (finite, "%s @ %.0f: NaN/inf", nm, fs);
            EXPECT (allocs == 0, "%s @ %.0f: %d allocations on the audio thread", nm, fs, allocs);
            EXPECT (10 * std::log10 (energy / total + 1e-20) > -40.0, "%s @ %.0f: nearly silent", nm, fs);
            EXPECT (10 * std::log10 (tailEnergy / bs + 1e-30) < -90.0, "%s @ %.0f: reverb didn't decay", nm, fs);
            if (rooms[(size_t) idx].mono && rooms[(size_t) idx].length <= 0.0f) EXPECT (mono, "%s should be mono", nm);
        }

    auto find = [&] (const char* name) { for (int i = 0; i < (int) rooms.size(); ++i) if (juce::String (rooms[(size_t) i].name) == name) return i; return -1; };
    const double fs = 48000.0;
    auto toneThrough = [&] (int idx, double hz) {
        ListeningRoomSim sim; sim.prepare (fs);
        juce::AudioBuffer<float> b (2, (int) fs); fillSine (b, fs, hz, 0.1f);
        sim.process (b.getWritePointer (0), b.getWritePointer (1), b.getNumSamples(), idx);
        return toneDb (b, 0, hz, fs, 4800);
    };
    const int phone = find ("Phone Speaker");
    EXPECT (phone >= 0 && toneThrough (phone, 2000) - toneThrough (phone, 100) > 15.0, "phone speaker should lose its deep bass");
    const int sub = find ("Car - SUV with Subwoofer");
    EXPECT (sub >= 0 && toneThrough (sub, 45) - toneThrough (sub, 1000) > 6.0, "SUV subwoofer should boost the low end");

    // Reverb length: a cathedral rings far longer than a vocal booth.
    auto tailAfter = [&] (int idx, double seconds) {
        ListeningRoomSim sim; sim.prepare (fs);
        juce::AudioBuffer<float> b (2, (int) (fs * (seconds + 0.6)));
        b.clear(); for (int i = 0; i < 2400; ++i) { const float v = rnd.nextFloat() * 2 - 1; b.setSample (0, i, v); b.setSample (1, i, v); }
        sim.process (b.getWritePointer (0), b.getWritePointer (1), b.getNumSamples(), idx);
        double e = 0; for (int i = (int) (fs * seconds); i < (int) (fs * (seconds + 0.1)); ++i) e += (double) b.getSample (0, i) * b.getSample (0, i);
        return 10 * std::log10 (e + 1e-30);
    };
    const double cathedral = tailAfter (find ("Church / Cathedral"), 1.0), booth = tailAfter (find ("Vocal Booth"), 1.0);
    std::printf ("  energy 1 s after a burst: cathedral %.0f dB, vocal booth %.0f dB\n", cathedral, booth);
    EXPECT (cathedral - booth > 40.0, "a cathedral should ring much longer than a vocal booth");

    // Switching rooms while audio plays: no blow-ups, no big jumps.
    {
        ListeningRoomSim sim; sim.prepare (fs);
        juce::AudioBuffer<float> b (2, 256); float maxJump = 0, last = 0; bool finite = true;
        for (int k = 0; k < 400; ++k)
        {
            fillSine (b, fs, 220.0, 0.2f, 0.0, k * 256);
            sim.process (b.getWritePointer (0), b.getWritePointer (1), 256, (k / 20) % ((int) rooms.size() + 1) - 1);
            for (int i = 0; i < 256; ++i) { const float v = b.getSample (0, i); finite &= std::isfinite (v); maxJump = std::max (maxJump, std::abs (v - last)); last = v; }
        }
        EXPECT (finite && maxJump < 0.3f, "room switching produced a jump of %.2f", maxJump);
    }

    // Through the plugin.
    SolutionAudioProcessor proc; proc.prepareToPlay (fs, 512);
    setParam (proc, "listen_room", (float) (find ("Mono Check") + 1));
    juce::MidiBuffer midi; juce::AudioBuffer<float> b (2, 512);
    for (int k = 0; k < 20; ++k) { for (int i = 0; i < 512; ++i) { b.setSample (0, i, 0.2f * (float) std::sin (0.05 * (k * 512 + i))); b.setSample (1, i, 0.0f); } proc.processBlock (b, midi); }
    float diff = 0; for (int i = 0; i < 512; ++i) diff = std::max (diff, std::abs (b.getSample (0, i) - b.getSample (1, i)));
    EXPECT (diff < 1e-4f, "plugin Listening Room 'Mono Check' isn't mono (diff %g)", diff);
    std::printf ("  done\n");
}

static void testUnmask()
{
    std::printf ("== Unmask (frequency-specific make-room)\n");
    const double fs = 48000.0; const int bs = 480;
    juce::MidiBuffer midi; juce::Random rnd (4);
    auto run = [&] (int musicRole, int musicGroup, bool vocalLoud)
    {
        SolutionAudioProcessor vocal, music;
        for (auto* p : { &vocal, &music }) { p->prepareToPlay (fs, bs); setParam (*p, "global_mix", 0.0f); setParam (*p, "stage_ceiling_guard", 0.0f); }
        setParam (vocal, "link_group", 1.0f); setParam (vocal, "unmask_role", 1.0f);
        setParam (music, "link_group", (float) musicGroup); setParam (music, "unmask_role", (float) musicRole); setParam (music, "unmask_depth", 9.0f);
        juce::AudioBuffer<float> tail (2, (int) fs);
        for (int k = 0; k < (int) (3 * fs / bs); ++k)
        {
            juce::AudioBuffer<float> v (2, bs), m (2, bs);
            fillSine (v, fs, 1100.0, vocalLoud ? 0.3f : 0.0f, 0.0, k * bs);         // the "vocal" lives at 1.1 kHz
            for (int i = 0; i < bs; ++i) { const double t = (k * bs + i) / fs;       // the "music": tones at 220 Hz and 1.1 kHz
                const float x = (float) (0.1 * std::sin (juce::MathConstants<double>::twoPi * 220 * t) + 0.1 * std::sin (juce::MathConstants<double>::twoPi * 1100 * t));
                m.setSample (0, i, x); m.setSample (1, i, x); }
            vocal.processBlock (v, midi); music.processBlock (m, midi);
            const int pos = k * bs - (int) (2 * fs);
            if (pos >= 0) for (int ch = 0; ch < 2; ++ch) tail.copyFrom (ch, pos, m, ch, 0, bs);
        }
        return std::pair<double, double> { toneDb (tail, 0, 1100, fs, 0), toneDb (tail, 0, 220, fs, 0) };
    };
    const auto base = run (0, 1, true), room = run (2, 1, true), quiet = run (2, 1, false), otherGroup = run (2, 2, true);
    std::printf ("  music @1.1 kHz: %.1f dB normally, %.1f dB making room | @220 Hz: %.1f -> %.1f dB\n", base.first, room.first, base.second, room.second);
    EXPECT (base.first - room.first > 6.0, "make-room should dip 1.1 kHz under the priority (dipped %.1f dB)", base.first - room.first);
    EXPECT (std::abs (base.second - room.second) < 1.0, "220 Hz should be left alone (changed %.1f dB)", base.second - room.second);
    EXPECT (std::abs (quiet.first - base.first) < 0.3, "no dip when the priority is silent");
    EXPECT (std::abs (otherGroup.first - base.first) < 0.3, "a different group must not be affected");
    std::printf ("  done\n");
}

static void testAutoAlign()
{
    std::printf ("== Auto-Align (time + polarity between mics)\n");
    const double fs = 48000.0; const int bs = 512;
    juce::MidiBuffer midi; juce::Random rnd (12);
    // A shared "drum" source; the follower mic hears it `offset` samples later (negative = earlier), maybe inverted.
    auto scenario = [&] (int offset, bool inverted)
    {
        SolutionAudioProcessor ref, fol;
        for (auto* p : { &ref, &fol }) { p->prepareToPlay (fs, bs); setParam (*p, "global_mix", 0.0f); setParam (*p, "stage_ceiling_guard", 0.0f); setParam (*p, "link_group", 3.0f); }
        setParam (ref, "align_role", 1.0f); setParam (fol, "align_role", 2.0f);
        std::vector<float> src ((size_t) (fs * 4));
        for (size_t i = 0; i < src.size(); ++i) src[i] = (i % 6000 < 800 ? 0.6f : 0.05f) * (rnd.nextFloat() * 2 - 1);   // hits + bleed
        auto sample = [&] (long long i) { return (i >= 0 && i < (long long) src.size()) ? src[(size_t) i] : 0.0f; };
        auto feed = [&] (int blocks, long long start, double& coherence)
        {
            double eSum = 0, eA = 0, eB = 0;
            for (int k = 0; k < blocks; ++k)
            {
                juce::AudioBuffer<float> a (2, bs), b (2, bs);
                for (int i = 0; i < bs; ++i)
                {
                    const long long t = start + (long long) k * bs + i;
                    const float x = sample (t), y = (inverted ? -1.0f : 1.0f) * sample (t - offset);
                    a.setSample (0, i, x); a.setSample (1, i, x); b.setSample (0, i, y); b.setSample (1, i, y);
                }
                ref.processBlock (a, midi); fol.processBlock (b, midi);
                for (int i = 0; i < bs; ++i) { const double s = a.getSample (0, i) + b.getSample (0, i); eSum += s * s; eA += (double) a.getSample (0, i) * a.getSample (0, i); eB += (double) b.getSample (0, i) * b.getSample (0, i); }
            }
            coherence = eSum / (2 * (eA + eB) + 1e-20);      // 1.0 = perfectly aligned & same polarity
        };
        double before = 0, after = 0;
        feed (100, 0, before);
        const auto msg = fol.alignNow();
        ref.pollAlignRequests(); fol.pollAlignRequests();
        feed (100, 100LL * bs, after);
        auto realValue = [&] (SolutionAudioProcessor& p, const char* id) { auto* q = p.apvts.getParameter (id); return q->convertFrom0to1 (q->getValue()); };
        std::printf ("  offset %+d%s: %s | coherence %.2f -> %.2f (ref delay %.3f ms, follower delay %.3f ms)\n", offset, inverted ? " inverted" : "", msg.toRawUTF8(),
                     before, after, realValue (ref, "align_delay_ms"), realValue (fol, "align_delay_ms"));
        return after;
    };
    EXPECT (scenario (37, false) > 0.97, "follower 37 samples late: should end up coherent");
    EXPECT (scenario (-23, true) > 0.97, "follower 23 samples early and inverted: should end up coherent");
    EXPECT (scenario (0, true) > 0.97, "polarity-only problem should be fixed");

    SolutionAudioProcessor lonely; lonely.prepareToPlay (fs, bs);
    setParam (lonely, "link_group", 1.0f); setParam (lonely, "align_role", 2.0f);
    juce::AudioBuffer<float> b (2, bs); b.clear(); lonely.processBlock (b, midi);
    EXPECT (lonely.alignNow().contains ("No reference"), "aligning without a reference should explain why");
    std::printf ("  done\n");
}

static void testSafeBounce()
{
    std::printf ("== Safe Bounce / monitoring warnings\n");
    const double fs = 48000.0; const int bs = 512;
    juce::MidiBuffer midi;
    auto render = [&] (bool monitorsOn, bool offline, bool safeBounce)
    {
        SolutionAudioProcessor proc; proc.prepareToPlay (fs, bs);
        proc.setNonRealtime (offline);
        setParam (proc, "safe_bounce", safeBounce ? 1.0f : 0.0f);
        if (monitorsOn) { setParam (proc, "ab_bypass", 1.0f); setParam (proc, "listen_room", 11.0f); setParam (proc, "stream_preview", 1.0f); setParam (proc, "room_correct_on", 1.0f); }
        juce::Random rnd (8); juce::AudioBuffer<float> all (2, bs * 200);
        for (int k = 0; k < 200; ++k)
        {
            juce::AudioBuffer<float> b (2, bs);
            for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < bs; ++i) b.setSample (ch, i, 0.2f * (rnd.nextFloat() * 2 - 1));
            proc.processBlock (b, midi);
            for (int ch = 0; ch < 2; ++ch) all.copyFrom (ch, k * bs, b, ch, 0, bs);
        }
        return std::pair<juce::AudioBuffer<float>, int> { all, proc.getMonitorMask() };
    };
    auto maxDiff = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b) {
        float d = 0; for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < a.getNumSamples(); ++i) d = std::max (d, std::abs (a.getSample (ch, i) - b.getSample (ch, i))); return d; };

    const auto clean = render (false, true, true), forgot = render (true, true, true);
    EXPECT (maxDiff (clean.first, forgot.first) == 0.0f, "offline bounce with monitoring tools left on should be identical to a clean bounce (diff %g)", maxDiff (clean.first, forgot.first));
    EXPECT ((forgot.second & SolutionAudioProcessor::monitorAutoBypassMask) != 0, "monitoring tools should still be reported (for the warning)");
    const auto noSafety = render (true, true, false), live = render (true, false, true);
    EXPECT (maxDiff (clean.first, noSafety.first) > 1e-3f, "with Safe Bounce off the tools should stay active");
    EXPECT (maxDiff (clean.first, live.first) > 1e-3f, "during normal playback the tools must work");

    // One click: this module, and every module in the session.
    SolutionAudioProcessor a, b; a.prepareToPlay (fs, bs); b.prepareToPlay (fs, bs);
    setParam (a, "delta_listen", 1.0f); setParam (b, "listen_room", 5.0f); setParam (b, "ab_bypass", 1.0f);
    juce::AudioBuffer<float> buf (2, bs); buf.clear(); a.processBlock (buf, midi); buf.clear(); b.processBlock (buf, midi);
    EXPECT (a.getSessionMonitorCount() == 1, "module A should see that one other module has monitoring on");
    a.turnOffMonitoringEverywhere();
    b.pollSessionRequests();
    buf.clear(); a.processBlock (buf, midi); buf.clear(); b.processBlock (buf, midi);
    EXPECT ((a.getMonitorMask() & SolutionAudioProcessor::monitorAutoBypassMask) == 0 && (b.getMonitorMask() & SolutionAudioProcessor::monitorAutoBypassMask) == 0,
            "TURN ALL OFF IN EVERY MODULE didn't clear everything (A %d, B %d)", a.getMonitorMask(), b.getMonitorMask());
    std::printf ("  done\n");
}

static void testStageEngine()
{
    std::printf ("== 3D stage engine: distance, motion paths, cross-track moves\n");
    using namespace SolutionDSP;
    const double fs = 48000.0;
    // Distance: transparent at the 2 m reference, -6 dB at 4 m, darker and roomier far away.
    auto through = [&] (float metres, double hz, bool tailOnly)
    {
        DistanceStage d; d.prepare (fs);
        juce::AudioBuffer<float> b (2, (int) fs * 2);
        fillSine (b, fs, hz, 0.2f);
        if (tailOnly) for (int ch = 0; ch < 2; ++ch) b.clear (ch, (int) fs, (int) fs);
        d.process (b.getWritePointer (0), b.getWritePointer (1), b.getNumSamples(), metres);
        if (tailOnly) { double e = 0; for (int i = (int) (fs * 1.2); i < (int) (fs * 1.4); ++i) e += (double) b.getSample (0, i) * b.getSample (0, i); return 10 * std::log10 (e / (0.2 * fs) + 1e-30); }
        return toneDb (b, 0, hz, fs, (int) (fs * 0.5));
    };
    {
        DistanceStage d; d.prepare (fs);
        juce::AudioBuffer<float> b (2, 4096), ref (2, 4096); fillSine (b, fs, 440.0, 0.3f); ref.makeCopyOf (b);
        d.process (b.getWritePointer (0), b.getWritePointer (1), 4096, 2.0f);
        float diff = 0; for (int i = 0; i < 4096; ++i) diff = std::max (diff, std::abs (b.getSample (0, i) - ref.getSample (0, i)));
        EXPECT (diff == 0.0f, "2 m must be exactly transparent (diff %g)", diff);
    }
    const double at2 = through (2.0f, 1000.0, false), at4 = through (4.0f, 1000.0, false);
    EXPECT (std::abs ((at2 - at4) - 6.0) < 1.0, "doubling distance should drop ~6 dB (got %.1f)", at2 - at4);
    const double tilt2 = through (2.0f, 500.0, false) - through (2.0f, 8000.0, false), tilt30 = through (30.0f, 500.0, false) - through (30.0f, 8000.0, false);
    EXPECT (tilt30 - tilt2 > 6.0, "far away should lose high frequencies (extra loss %.1f dB)", tilt30 - tilt2);
    const double roomNear = through (2.0f, 1000.0, true), roomFar = through (20.0f, 1000.0, true);
    std::printf ("  distance: 4 m %.1f dB, 30 m extra HF loss %.1f dB, room tail far %.0f dB vs near %.0f dB\n", at4 - at2, tilt30 - tilt2, roomFar, roomNear);
    EXPECT (roomFar > roomNear + 40.0, "more room sound should be heard far away");

    // Motion: a 4-point loop is followed in order; host sync maps song position to the loop.
    MotionPlayer mp;
    mp.setPath ({ { 0, 0, 2 }, { 90, 0, 2 }, { 180, 0, 2 }, { -90, 0, 2 } });
    MotionPoint at;
    EXPECT (mp.evaluate (0.0, at) && std::abs (at.az) < 1.0f, "phase 0 should be at the first point (az %.1f)", at.az);
    mp.evaluate (0.25, at); EXPECT (std::abs (at.az - 90.0f) < 1.0f, "phase 0.25 should be at +90 (az %.1f)", at.az);
    mp.evaluate (0.5, at);  EXPECT (std::abs (std::abs (at.az) - 180.0f) < 1.0f, "phase 0.5 should be behind (az %.1f)", at.az);
    mp.evaluate (0.125, at); EXPECT (at.az > 20.0f && at.az < 70.0f && std::abs (at.dist - 2.0f) < 0.6f, "between points should glide smoothly (az %.1f, d %.2f)", at.az, at.dist);
    EXPECT (std::abs (MotionPlayer::phaseFor (true, 6.0, 4.0, 1.0, 0.0) - 1.5) < 1e-9, "host sync: beat 6 in a 1-bar loop is phase 1.5");

    SolutionAudioProcessor proc; proc.prepareToPlay (fs, 512);
    proc.setMotionPath ({ { -90, 0, 3 }, { 0, 30, 3 }, { 90, 0, 3 }, { 0, -10, 3 } });
    setParam (proc, "motion_on", 1.0f); setParam (proc, "motion_rate", 0.0f);   // 1/4 bar = 0.5 s at 120 BPM
    juce::MidiBuffer midi; float minAz = 999, maxAz = -999;
    for (int k = 0; k < 60; ++k) { juce::AudioBuffer<float> b (2, 512); fillSine (b, fs, 300.0, 0.1f, 0.0, k * 512); proc.processBlock (b, midi);
                                   minAz = std::min (minAz, proc.animAz.load()); maxAz = std::max (maxAz, proc.animAz.load()); }
    EXPECT (minAz < -60.0f && maxAz > 60.0f, "the sound should travel along the path (az %.0f..%.0f)", minAz, maxAz);
    juce::MemoryBlock state; proc.getStateInformation (state);
    SolutionAudioProcessor reloaded; reloaded.setStateInformation (state.getData(), (int) state.getSize());
    EXPECT (reloaded.getMotionPath().size() == 4, "motion path not saved with the session");

    // Dragging another track's orb on my stage moves that track.
    SolutionAudioProcessor a, b2; a.prepareToPlay (fs, 512); b2.prepareToPlay (fs, 512);
    EXPECT (a.requestParamOnSlot (b2.getRackSlot(), "stage_distance", 10.0f) && a.requestParamOnSlot (b2.getRackSlot(), "stage_azimuth", -45.0f), "move request refused");
    b2.pollSessionRequests();
    auto realValue = [&] (SolutionAudioProcessor& pr, const char* id) { auto* q = pr.apvts.getParameter (id); return q->convertFrom0to1 (q->getValue()); };
    EXPECT (std::abs (realValue (b2, "stage_distance") - 10.0f) < 0.05f && std::abs (realValue (b2, "stage_azimuth") + 45.0f) < 0.2f, "the other track didn't move");
    std::printf ("  done\n");
}

static void testDqs()
{
    std::printf ("== DQS (dynamic quality scaling)\n");
    using namespace SolutionDSP;
    const double fs = 48000.0; const int bs = 256;
    // Same latency at every quality level (an impulse lands on the same sample).
    auto impulsePeak = [&] (int level)
    {
        SuiteModuleEngine e; auto busPtr = std::make_unique<RackLinkBus>(); auto& bus = *busPtr; e.prepare ({ fs, (juce::uint32) bs, 2 });
        e.setQualityLevel (level);
        juce::AudioBuffer<float> b (2, bs);
        for (int k = 0; k < 20; ++k) { b.clear(); e.process (ModuleId::Matrix, b, ProcessParams{}, bus); }   // let the switch finish
        int peakAt = -1; float peak = 0.0f;
        for (int k = 0; k < 8; ++k)
        {
            b.clear(); if (k == 0) { b.setSample (0, 0, 1.0f); b.setSample (1, 0, 1.0f); }
            e.process (ModuleId::Matrix, b, ProcessParams{}, bus);
            for (int i = 0; i < bs; ++i) if (std::abs (b.getSample (0, i)) > peak) { peak = std::abs (b.getSample (0, i)); peakAt = k * bs + i; }
        }
        EXPECT (e.getQualityLevel() == level, "engine didn't reach quality level %d", level);
        return peakAt;
    };
    const int p0 = impulsePeak (0), p1 = impulsePeak (1), p2 = impulsePeak (2);
    std::printf ("  impulse arrives at sample %d / %d / %d (4x / 2x / 1x)\n", p0, p1, p2);
    EXPECT (std::abs (p0 - p1) <= 1 && std::abs (p0 - p2) <= 1, "latency must not change with quality level");

    // Switching while audio plays: no clicks.
    {
        SuiteModuleEngine e; auto busPtr = std::make_unique<RackLinkBus>(); auto& bus = *busPtr; e.prepare ({ fs, (juce::uint32) bs, 2 });
        juce::AudioBuffer<float> b (2, bs); float last = 0.0f, maxJump = 0.0f; bool finite = true;
        const int seq[] = { 2, 1, 0, 2, 0 };
        for (int k = 0; k < 400; ++k)
        {
            if (k % 80 == 40) e.setQualityLevel (seq[(k / 80) % 5]);
            fillSine (b, fs, 220.0, 0.5f, 0.0, k * bs);
            e.process (ModuleId::Matrix, b, ProcessParams{}, bus);
            for (int i = 0; i < bs; ++i) { const float v = b.getSample (0, i); finite &= std::isfinite (v); if (k > 4) maxJump = std::max (maxJump, std::abs (v - last)); last = v; }
        }
        const float sineStep = 0.5f * (float) (juce::MathConstants<double>::twoPi * 220.0 / fs);
        std::printf ("  largest sample-to-sample step while switching: %.4f (a 220 Hz sine's own max step: %.4f)\n", maxJump, sineStep);
        EXPECT (finite && maxJump < sineStep * 1.5f, "quality switching produced a click (step %.4f)", maxJump);
    }

    // Controller behaviour.
    DqsController c; const double blk = bs / fs;
    for (int i = 0; i < 60; ++i) c.update (DqsController::autoMode, false, 0.95f, 0.8f, blk);       // ~0.3 s overloaded
    EXPECT (c.level >= 1, "should step down after 1/4 s of overload");
    for (int i = 0; i < 200; ++i) c.update (DqsController::autoMode, false, 0.95f, 0.8f, blk);
    EXPECT (c.level == 2, "sustained overload should reach the lightest level");
    for (int i = 0; i < 1300; ++i) c.update (DqsController::autoMode, false, 0.2f, 0.8f, blk);     // ~7 s of headroom
    EXPECT (c.level == 0, "headroom should bring full quality back (level %d)", c.level);
    c.update (DqsController::autoMode, false, 0.99f, 0.8f, 1.0);
    EXPECT (c.update (DqsController::autoMode, true, 0.99f, 0.8f, 1.0) == 0, "offline bounces always use full quality");
    EXPECT (c.update (DqsController::locked4x, false, 0.99f, 0.8f, 1.0) == 0, "Locked 4x must stay at full quality");
    EXPECT (c.update (DqsController::eco, false, 0.0f, 0.8f, 1.0) == 2, "Eco must use the lightest level");

    // Plugin: Eco mode really switches, latency reported to the DAW never changes.
    SolutionAudioProcessor proc; proc.prepareToPlay (fs, 512);
    const int lat = proc.getLatencySamples();
    setParam (proc, "dqs_mode", 2.0f);
    juce::MidiBuffer midi; juce::AudioBuffer<float> b (2, 512);
    for (int k = 0; k < 20; ++k) { fillSine (b, fs, 440.0, 0.2f, 0.0, k * 512); proc.processBlock (b, midi); }
    EXPECT (proc.dqsLevel.load() == 2 && proc.getLatencySamples() == lat, "Eco mode: level %d, latency %d (was %d)", proc.dqsLevel.load(), proc.getLatencySamples(), lat);
    std::printf ("  done\n");
}

static void testInfraLift()
{
    std::printf ("Infrasonic Lift (The Bunker)\n");
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        InfraLift lift; lift.prepare (fs);
        float l[64] = {}, r[64] = {};
        auto db = [&] (double f) { return 20 * std::log10 (lift.magnitudeAt (f)); };
        lift.process (l, r, 64, 6.0f, false);               // default: narrow, true infrasonics only
        const double n763 = db (7.63), n15 = db (15.0), n20 = db (20.0), n30 = db (30.0);
        EXPECT (std::abs (n763 - 6.0) < 1.0, "%.0f Hz narrow: +%.2f dB at 7.63 Hz (want ~+6)", fs, n763);
        EXPECT (n15 < 1.2 && n20 < 0.6 && std::abs (n30) < 0.3, "%.0f Hz narrow: leaves 15-30 Hz alone (%.2f / %.2f / %.2f dB)", fs, n15, n20, n30);
        lift.process (l, r, 64, 6.0f, true);                // WIDE: skirt fills 15-30 Hz
        const double g763 = db (7.63), g20 = db (20.0), g60 = db (60.0), g1k = db (1000.0), g1 = db (1.0);
        EXPECT (std::abs (g763 - 6.0) < 1.0, "%.0f Hz wide: +%.2f dB at 7.63 Hz (want ~+6)", fs, g763);
        EXPECT (g20 > 1.0 && g20 < 5.5, "%.0f Hz wide: skirt lifts 20 Hz by %.2f dB", fs, g20);
        EXPECT (std::abs (g60) < 0.6 && std::abs (g1k) < 0.05, "%.0f Hz: leaves 60 Hz (%.2f) and 1 kHz (%.3f) alone", fs, g60, g1k);
        EXPECT (g1 < -6.0, "%.0f Hz: 3 Hz guard cuts 1 Hz (%.1f dB)", fs, g1);
    }
    // Real signal: DC never gets through, and the plugin chain stays stable and under the ceiling.
    {
        InfraLift lift; lift.prepare (48000.0);
        std::vector<float> dc (96000, 0.5f), dc2 (96000, 0.5f);
        lift.process (dc.data(), dc2.data(), 96000, 12.0f);
        EXPECT (std::abs (dc.back()) < 0.002f, "DC must be blocked (%.4f left after 2 s)", dc.back());
    }
    {
        SolutionAudioProcessor proc; proc.prepareToPlay (48000.0, 512);
        setParam (proc, "infra_lift", 1.0f); setParam (proc, "infra_lift_db", 12.0f);
        juce::MidiBuffer midi; juce::AudioBuffer<float> b (2, 512);
        float peak = 0; bool finite = true;
        for (int k = 0; k < 200; ++k)
        {
            fillSine (b, 48000.0, 9.0, 0.6f, 0.0, k * 512);
            proc.processBlock (b, midi);
            peak = std::max (peak, b.getMagnitude (0, 512));
            for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 512; ++i) finite = finite && std::isfinite (b.getSample (ch, i));
        }
        EXPECT (finite && peak <= juce::Decibels::decibelsToGain (-0.5f) * 1.02f, "+12 dB lift on a hot 9 Hz tone stays finite and under the ceiling (peak %.3f)", peak);
    }
    std::printf ("  done\n");
}

static void testLookaheadAndLearn()
{
    std::printf ("Lookahead limiter + MIDI Learn on every control\n");
    SolutionAudioProcessor proc; proc.prepareToPlay (48000.0, 512);
    const int before = proc.getLatencySamples();
    setParam (proc, "limiter_lookahead", 1.0f);
    proc.applyPendingChanges();                    // the host's message loop would run this
    const int after = proc.getLatencySamples();
    EXPECT (after - before == (int) std::round (0.005 * 48000.0) - (int) std::round (0.0015 * 48000.0),
            "lookahead adds exactly 3.5 ms of reported latency (%d -> %d)", before, after);
    juce::MidiBuffer midi; juce::AudioBuffer<float> b (2, 512); float peak = 0;
    for (int k = 0; k < 40; ++k) { fillSine (b, 48000.0, 100.0, 2.0f, 0.0, k * 512); proc.processBlock (b, midi); peak = std::max (peak, b.getMagnitude (0, 512)); }
    EXPECT (peak <= juce::Decibels::decibelsToGain (-0.5f) * 1.02f, "ceiling still holds with lookahead on (peak %.3f)", peak);

    // Learn: arm the Infrasonic Lift amount (not in the old list) and move CC 21.
    proc.armMidiLearn ("infra_lift_db");
    juce::MidiBuffer cc; cc.addEvent (juce::MidiMessage::controllerEvent (1, 21, 64), 0);
    proc.processBlock (b, cc);
    juce::MidiBuffer cc2; cc2.addEvent (juce::MidiMessage::controllerEvent (1, 21, 127), 0);
    proc.processBlock (b, cc2);
    const float v = proc.apvts.getRawParameterValue ("infra_lift_db")->load();
    EXPECT (std::abs (v - 12.0f) < 0.05f, "CC 21 learned onto Infrasonic Lift dB (now %.2f)", v);
    EXPECT (proc.learnSeq.load() >= 1, "learn counter advanced for the setup wizard");

    // MIDI map: presets replace the map, and it survives a save/reload.
    proc.setCcMapString ("16=stage_azimuth;17=global_mix");
    EXPECT (proc.setCcMapping (18, "glue_amount"), "single CC mapping accepted");
    EXPECT (! proc.setCcMapping (19, "no_such_param"), "unknown parameter rejected");
    juce::MemoryBlock saved; proc.getStateInformation (saved);
    SolutionAudioProcessor proc2; proc2.prepareToPlay (48000.0, 512);
    proc2.setStateInformation (saved.getData(), (int) saved.getSize());
    const auto m2 = proc2.getCcMapString();
    EXPECT (m2.contains ("16=stage_azimuth") && m2.contains ("17=global_mix") && m2.contains ("18=glue_amount") && ! m2.contains ("21="),
            "MIDI map restored with the session, preset replace dropped old CC 21 (%s)", m2.toRawUTF8());
    juce::MidiBuffer cc3; cc3.addEvent (juce::MidiMessage::controllerEvent (1, 17, 0), 0);
    proc2.processBlock (b, cc3);
    EXPECT (proc2.apvts.getRawParameterValue ("global_mix")->load() < 0.01f, "restored mapping drives the control");
    std::printf ("  done\n");
}

static void testDcGuard()
{
    std::printf ("DC Guard\n");
    {
        DcGuard g; g.prepare (48000.0);
        std::vector<float> l (48000 * 3), r (48000 * 3);
        for (size_t i = 0; i < l.size(); ++i) { l[i] = 0.25f + 0.1f * (float) std::sin (2 * juce::MathConstants<double>::pi * 8.0 * (double) i / 48000.0); r[i] = l[i]; }   // 8 Hz: whole cycles per second
        g.process (l.data(), r.data(), (int) l.size());
        double mean = 0, peak = 0; const size_t from = l.size() - 48000;
        for (size_t i = from; i < l.size(); ++i) { mean += l[i]; peak = std::max (peak, (double) std::abs (l[i])); }
        mean /= 48000.0;
        EXPECT (std::abs (mean) < 0.002, "0.25 DC offset removed (mean now %.4f)", mean);
        EXPECT (std::abs (peak - 0.1) < 0.004, "8 Hz infrasonic content survives (peak %.4f, want 0.100)", peak);
    }
    {
        // Through the plugin: DC on the input is gone at the output, and it's logged.
        SolutionAudioProcessor proc; proc.prepareToPlay (48000.0, 512);
        juce::MidiBuffer midi; juce::AudioBuffer<float> b (2, 512); double mean = 0;
        for (int k = 0; k < 400; ++k)
        {
            fillSine (b, 48000.0, 220.0, 0.2f, 0.0, k * 512);
            for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 512; ++i) b.setSample (ch, i, b.getSample (ch, i) + 0.2f);
            proc.processBlock (b, midi);
            if (k >= 300) for (int i = 0; i < 512; ++i) mean += b.getSample (0, i);
        }
        mean /= 100.0 * 512.0;
        EXPECT (std::abs (mean) < 0.003 && proc.diag.dcEvents.load() >= 1, "plugin output mean %.4f with 0.2 DC in, %u event(s) logged", mean, proc.diag.dcEvents.load());
    }
    std::printf ("  done\n");
}

// Speech-like test signal: a 130 Hz voiced buzz with vowel-ish peaks; gated at syllable rate (4/s) or held.
static float speechLike (int i, double fs, bool syllables, float level = 0.3f)
{
    const double t = i / fs;
    double v = 0;
    for (int h = 1; h <= 25; ++h)
    {
        const double f = 130.0 * h, form = std::exp (-std::pow ((f - 700) / 250, 2)) + 0.7 * std::exp (-std::pow ((f - 1200) / 300, 2)) + 0.4 * std::exp (-std::pow ((f - 2500) / 400, 2));
        v += (0.05 + form) / h * std::sin (2 * juce::MathConstants<double>::pi * f * t);
    }
    double env = 1.0;
    if (syllables) { const double ph = std::fmod (t * 4.0, 1.0); env = ph < 0.6 ? std::pow (std::sin (juce::MathConstants<double>::pi * ph / 0.6), 2) : 0.0; }
    return (float) (level * env * v);
}

static void testDialogueAndTransients()
{
    std::printf ("Voice-activated dialogue ducking + transient shaper\n");
    const double fs = 48000.0;
    auto detect = [&] (int kind) {   // 0 speech, 1 held vowel/pad, 2 noise, 3 silence
        VoiceDetector vd; vd.prepare (fs); std::mt19937 rng (7); std::normal_distribution<float> nd (0.0f, 0.1f);
        std::vector<float> x (512); float maxConf = 0, endConf = 0;
        for (int b = 0; b < (int) (3 * fs / 512); ++b)
        {
            for (int i = 0; i < 512; ++i) { const int n = b * 512 + i; x[(size_t) i] = kind == 0 ? speechLike (n, fs, true) : kind == 1 ? speechLike (n, fs, false) : kind == 2 ? nd (rng) : 0.0f; }
            vd.process (x.data(), x.data(), 512);
            if (b * 512 > fs) maxConf = std::max (maxConf, vd.getConfidence());
            endConf = vd.getConfidence();
        }
        return std::make_pair (maxConf, endConf);
    };
    const auto sp = detect (0), held = detect (1), noise = detect (2), quiet = detect (3);
    std::printf ("  voice confidence: speech %.2f, held vowel %.2f, noise %.2f, silence %.2f\n", sp.second, held.first, noise.first, quiet.first);
    EXPECT (sp.second > 0.8f, "speech must be detected (%.2f)", sp.second);
    EXPECT (held.first < 0.2f && noise.first < 0.2f && quiet.first < 0.05f, "held tones, noise and silence must not count as speech");

    // Session: track A is dialogue, track B ducks under it.
    {
        auto a = std::make_unique<SolutionAudioProcessor>(), b = std::make_unique<SolutionAudioProcessor>();
        a->prepareToPlay (fs, 512); b->prepareToPlay (fs, 512);
        setParam (*a, "dialogue_source", 1.0f); setParam (*b, "dialogue_duck", 1.0f); setParam (*b, "dialogue_duck_db", -6.0f);
        juce::MidiBuffer midi; juce::AudioBuffer<float> ba (2, 512), bb (2, 512);
        auto runFor = [&] (double secs, bool talking) {
            double e = 0; int n = 0;
            for (int k = 0; k < (int) (secs * fs / 512); ++k)
            {
                for (int i = 0; i < 512; ++i) { const float v = talking ? speechLike (k * 512 + i, fs, true) : 0.0f; ba.setSample (0, i, v); ba.setSample (1, i, v); }
                fillSine (bb, fs, 2200.0, 0.2f, 0.0, k * 512);
                a->processBlock (ba, midi); b->processBlock (bb, midi);
                if (k * 512 > (secs - 0.5) * fs) { for (int i = 0; i < 512; ++i) e += bb.getSample (0, i) * bb.getSample (0, i); ++n; }
            }
            return 10 * std::log10 (e / (n * 512.0) + 1e-12);
        };
        const double quietDb = runFor (1.5, false), talkDb = runFor (2.0, true), afterDb = runFor (2.0, false);
        std::printf ("  music at 2.2 kHz: %.1f dB before, %.1f dB while talking, %.1f dB after\n", quietDb, talkDb, afterDb);
        EXPECT (quietDb - talkDb > 4.5 && quietDb - talkDb < 9.5, "music ducks ~6-8 dB in the speech band while someone talks (%.1f dB)", quietDb - talkDb);
        EXPECT (std::abs (afterDb - quietDb) < 0.5, "music comes back when the talking stops (%.1f vs %.1f)", afterDb, quietDb);
    }

    // Transient shaper: +attack makes hits stand out, -attack softens them, sustain lengthens tails.
    {
        auto hitRatio = [&] (float atk, float sus) {
            TransientShaper ts; ts.prepare (fs);
            std::vector<float> l ((size_t) fs), r ((size_t) fs);
            for (size_t i = 0; i < l.size(); ++i) { const double t = std::fmod ((double) i / fs, 0.25); l[i] = r[i] = (float) (0.5 * std::exp (-t * 30.0) * std::sin (2 * juce::MathConstants<double>::pi * 180.0 * (double) i / fs)); }
            ts.process (l.data(), r.data(), (int) l.size(), atk, sus);
            double head = 0, tail = 0;     // last hit: first 10 ms vs 80-200 ms
            const size_t s0 = (size_t) (0.75 * fs);
            for (size_t i = s0; i < s0 + (size_t) (0.010 * fs); ++i) head = std::max (head, (double) std::abs (l[i]));
            for (size_t i = s0 + (size_t) (0.080 * fs); i < s0 + (size_t) (0.200 * fs); ++i) tail = std::max (tail, (double) std::abs (l[i]));
            return 20 * std::log10 (head / tail);
        };
        const double flat = hitRatio (0, 0), punch = hitRatio (1, 0), soft = hitRatio (-1, 0), longer = hitRatio (0, 1);
        std::printf ("  hit-to-tail: flat %.1f dB, +attack %.1f dB, -attack %.1f dB, +sustain %.1f dB\n", flat, punch, soft, longer);
        EXPECT (std::abs (flat - hitRatio (0, 0)) < 1e-9 && punch > flat + 3 && soft < flat - 3 && longer < flat - 3, "attack and sustain must reshape the hits");
    }
    std::printf ("  done\n");
}

static void testVoiceParser()
{
    std::printf ("Voice command parser\n");
    using namespace SolutionVoice;
    SolutionAudioProcessor proc;
    Parser vp;
    for (auto* p : proc.getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            ParamInfo pi; pi.id = r->getParameterID(); pi.name = r->getName (64);
            pi.min = r->getNormalisableRange().start; pi.max = r->getNormalisableRange().end;
            pi.isBool = dynamic_cast<juce::AudioParameterBool*> (r) != nullptr;
            if (auto* ch = dynamic_cast<juce::AudioParameterChoice*> (r)) { pi.isChoice = true; pi.choices = ch->choices; }
            pi.percentUnit = pi.id == "stage_width" || pi.id == "global_mix" || pi.id == "stage_depth" || pi.id == "glue_amount";
            vp.params.push_back (pi);
        }
    vp.targets = { { "the bunker", 3 }, { "bunker", 3 }, { "lead vocal", 5 }, { "the stage", 1 } };
    struct Case { const char* said; Command::Kind kind; const char* what; float value; int slot; };
    const Case cases[] = {
        { "Solution play", Command::Transport, "play", 0, -1 },
        { "solution stop", Command::Transport, "stop", 0, -1 },
        { "solution go to the beginning", Command::Transport, "start", 0, -1 },
        { "solution record", Command::Transport, "record", 0, -1 },
        { "solution panic everything", Command::Panic, "", 0, -1 },
        { "solution width one hundred and twenty percent", Command::SetParam, "stage_width", 1.2f, -1 },
        { "solution lead vocal width 80 percent", Command::SetParam, "stage_width", 0.8f, 5 },
        { "solution azimuth minus thirty", Command::SetParam, "stage_azimuth", -30.0f, -1 },
        { "solution bunker infra lift on", Command::SetParam, "infra_lift", 1.0f, 3 },
        { "solution the bunker infrasonic lift eight point five db", Command::SetParam, "infra_lift_db", 8.5f, 3 },
        { "solution dc guard off", Command::SetParam, "dc_guard", 0.0f, -1 },
        { "solution lookahead on", Command::SetParam, "limiter_lookahead", 1.0f, -1 },
        { "solution transient attack sixty", Command::SetParam, "transient_attack", 60.0f, -1 },
        { "solution duck depth minus nine", Command::SetParam, "dialogue_duck_db", -9.0f, -1 },
        { "solution more width", Command::NudgeParam, "stage_width", 0.1f, -1 },
        { "solution recall three", Command::Macro, "recall", 3.0f, -1 },
        { "solution snapshot two", Command::Macro, "store", 2.0f, -1 },
        { "solution a b", Command::Ui, "ab", 0, -1 },
        { "solution full screen", Command::Ui, "fullscreen", 0, -1 },
        { "solution next preset", Command::Ui, "nextPreset", 0, -1 },
        { "solution undo", Command::Ui, "undo", 0, -1 },
        { "solution analyzer", Command::Ui, "analyzer", 0, -1 },
        { "solution show history", Command::Ui, "history", 0, -1 },
        { "solution midi controllers", Command::Ui, "midi", 0, -1 },
        { "solution turn the monitors off", Command::Ui, "monitorsOff", 0, -1 },
        { "play", Command::None, "", 0, -1 },                         // no wake word: ignored
        { "solution what's for lunch", Command::None, "", 0, -1 },   // nonsense: ignored
    };
    int ok = 0;
    for (auto& cs : cases)
    {
        const auto c = vp.parse (cs.said);
        const juce::String what = c.kind == Command::SetParam || c.kind == Command::NudgeParam ? c.paramId : c.action;
        const bool good = c.kind == cs.kind && what == cs.what && (cs.kind != Command::SetParam && cs.kind != Command::NudgeParam && cs.kind != Command::Macro || std::abs (c.value - cs.value) < 1e-3f) && c.slot == cs.slot;
        EXPECT (good, "'%s' -> kind %d '%s' %.3f slot %d", cs.said, (int) c.kind, what.toRawUTF8(), c.value, c.slot);
        ok += good;
    }
    std::printf ("  %d/%d phrases understood correctly, vocabulary %d words\n", ok, (int) std::size (cases), vp.vocabulary().size());
}

static void testHeadTracking()
{
    std::printf ("Head tracking (OSC / phone / gamepad)\n");
    HeadTracker ht;
    ht.handle (juce::OSCMessage ("/SceneRotator/ypr", 30.0f, -10.0f, 0.0f));
    EXPECT (std::abs (ht.yaw() - 30.0f) < 0.01f && std::abs (ht.pitch() + 10.0f) < 0.01f && ht.isFresh(), "IEM ypr degrees (%.1f, %.1f)", ht.yaw(), ht.pitch());
    ht.handle (juce::OSCMessage ("/head/ypr", 0.5f, 0.2f, 0.0f));                           // radians
    EXPECT (std::abs (ht.yaw() - 28.65f) < 0.1f, "radians converted (%.2f)", ht.yaw());
    const float a = juce::MathConstants<float>::pi / 4.0f;                                 // 90 degree yaw quaternion (w x y z)
    ht.handle (juce::OSCMessage ("/SceneRotator/quaternions", std::cos (a), 0.0f, 0.0f, std::sin (a)));
    EXPECT (std::abs (ht.yaw() - 90.0f) < 0.5f, "quaternion yaw (%.2f)", ht.yaw());
    ht.handle (juce::OSCMessage ("/gyrosc/gyro", 0.1f, 0.0f, 1.0f));                         // GyrOSC: pitch roll yaw
    EXPECT (std::abs (ht.yaw() - 57.3f) < 0.2f && std::abs (ht.pitch() - 5.73f) < 0.1f, "GyrOSC order (%.1f, %.1f)", ht.yaw(), ht.pitch());
    ht.recenter();
    EXPECT (std::abs (ht.yaw()) < 0.01f && std::abs (ht.pitch()) < 0.01f, "recenter zeroes the pose");

    // Binaural: a source straight ahead is centred; turn your head 90 deg right and it moves to your left ear.
    SolutionAudioProcessor proc; proc.prepareToPlay (48000.0, 512);
    setParam (proc, "binaural_mode", 1.0f);
    auto lr = [&] (float yaw) {
        proc.headTracker->enabled = true; proc.headTracker->setPose (yaw, 0, 1);
        juce::MidiBuffer midi; juce::AudioBuffer<float> b (2, 512); double el = 0, er = 0;
        for (int k = 0; k < 60; ++k) { fillSine (b, 48000.0, 1000.0, 0.2f, 0.0, k * 512); proc.processBlock (b, midi);
            if (k > 30) for (int i = 0; i < 512; ++i) { el += b.getSample (0, i) * b.getSample (0, i); er += b.getSample (1, i) * b.getSample (1, i); } }
        return 10 * std::log10 (el / er);
    };
    proc.headTracker->recenter();
    const double ahead = lr (0.0f), right = lr (90.0f), left = lr (-90.0f);
    std::printf ("  L-R balance: head ahead %.1f dB, turned right %.1f dB, turned left %.1f dB\n", ahead, right, left);
    EXPECT (std::abs (ahead) < 1.5 && right > 4.0 && left < -4.0, "turning your head moves the mix the other way");
    proc.headTracker->enabled = false;
}

static void testNetworkOffload()
{
    std::printf ("LAN / local DSP offload (node <-> plugin over TCP)\n");
    const double fs = 48000.0;
    SolutionNet::NodeServer node ([] (const juce::String& key) { return std::make_unique<SolutionAudioProcessor> (key); }, "Test Node");
    if (! node.start (47950)) { std::printf ("  (couldn't open a port - skipped)\n"); return; }
    const auto found = SolutionNet::discoverNodes (300);
    std::printf ("  discovery found %d node(s)\n", found.size());

    auto remoteProc = std::make_unique<SolutionAudioProcessor>(), localProc = std::make_unique<SolutionAudioProcessor>();
    remoteProc->prepareToPlay (fs, 512); localProc->prepareToPlay (fs, 512);
    for (auto* p : { remoteProc.get(), localProc.get() }) { setParam (*p, "stage_width", 1.6f); setParam (*p, "stage_azimuth", 30.0f); }
    const int baseLatency = remoteProc->getLatencySamples();
    juce::String err;
    const bool ok = remoteProc->connectOffload ("127.0.0.1", 47950, 2048, err);
    EXPECT (ok, "connect to node: %s", err.toRawUTF8());
    if (! ok) return;
    remoteProc->applyPendingChanges();
    EXPECT (remoteProc->getLatencySamples() == baseLatency + 2048, "offload latency reported (%d)", remoteProc->getLatencySamples());
    juce::Thread::sleep (200);                                   // node receives the settings
    juce::MidiBuffer midi; juce::AudioBuffer<float> a (2, 512), b (2, 512);
    std::vector<float> outRemote, outLocal;
    for (int k = 0; k < 200; ++k)
    {
        fillSine (a, fs, 330.0, 0.25f, 0.0, k * 512); b.makeCopyOf (a);
        remoteProc->processBlock (a, midi); localProc->processBlock (b, midi);
        outRemote.insert (outRemote.end(), a.getReadPointer (0), a.getReadPointer (0) + 512);
        outLocal.insert (outLocal.end(), b.getReadPointer (0), b.getReadPointer (0) + 512);
        juce::Thread::sleep (3);                                 // roughly real time for the network threads
    }
    EXPECT (remoteProc->isOffloaded() && node.getActiveSessions() == 1, "still offloaded, 1 session on the node");
    // Compare the node's output (2048 samples later) with local processing, after things settle.
    double err2 = 0, sig2 = 0;
    for (size_t i = 60000; i < outLocal.size() - 2048 && i + 2048 < outRemote.size(); ++i) { const double d = outRemote[i + 2048] - outLocal[i]; err2 += d * d; sig2 += outLocal[i] * outLocal[i]; }
    const double snr = 10 * std::log10 (sig2 / (err2 + 1e-20));
    std::printf ("  node output vs local processing: %.1f dB match\n", snr);
    EXPECT (snr > 40.0, "the node must sound like local processing (%.1f dB)", snr);
    remoteProc->disconnectOffload();
    EXPECT (! remoteProc->isOffloaded() && remoteProc->getLatencySamples() == baseLatency, "back to local, latency restored");
    node.stop();
}



static void testToneAnalyzer()
{
    std::printf ("Test-tone analyser: known devices give known answers, and the module can be measured\n");
    using namespace SolutionAnalyzer;
    auto at = [] (const Curve& c, float hz, bool right = false) { size_t best = 0; for (size_t i = 0; i < c.hz.size(); ++i) if (std::abs (std::log (c.hz[i] / hz)) < std::abs (std::log (c.hz[best] / hz))) best = i; return (right ? c.r : c.l)[best]; };
    // 1) -6 dB gain on L, +0 on R, with a 64-sample delay that the device reports as latency.
    {
        Device d; d.sampleRate = 48000.0; d.latency = 64;
        std::vector<float> hl (64, 0.0f), hr (64, 0.0f); int pos = 0;
        d.reset = [&] { std::fill (hl.begin(), hl.end(), 0.0f); std::fill (hr.begin(), hr.end(), 0.0f); pos = 0; };
        d.process = [&] (juce::AudioBuffer<float>& b) { for (int i = 0; i < b.getNumSamples(); ++i) { const float l = b.getSample (0, i), r = b.getSample (1, i);
            b.setSample (0, i, hl[(size_t) pos] * 0.5f); b.setSample (1, i, hr[(size_t) pos]); hl[(size_t) pos] = l; hr[(size_t) pos] = r; pos = (pos + 1) % 64; } };
        Curve mag, ph; response (d, mag, ph);
        EXPECT (std::abs (at (mag, 100) + 6.02f) < 0.2f && std::abs (at (mag, 1000) + 6.02f) < 0.2f && std::abs (at (mag, 10000) + 6.02f) < 0.2f,
                "-6 dB gain reads -6 dB flat (%.2f / %.2f / %.2f)", at (mag, 100), at (mag, 1000), at (mag, 10000));
        EXPECT (std::abs (at (mag, 1000, true)) < 0.2f, "unity right channel reads 0 dB (%.2f)", at (mag, 1000, true));
        EXPECT (std::abs (at (ph, 1000)) < 3.0f && std::abs (at (ph, 8000)) < 3.0f, "reported latency removed from phase (%.1f / %.1f deg)", at (ph, 1000), at (ph, 8000));
        const auto h = harmonics (d, 1000.0f, -18.0f);
        EXPECT (h.thdPct < 0.01f, "linear device THD below 0.01%% (%.5f%%)", h.thdPct);
        EXPECT (std::abs (h.fundamentalDb - (-18.0f)) < 0.3f, "fundamental level reads right: louder channel at -18 dBFS (%.2f)", h.fundamentalDb);
    }
    // 2) tanh saturation: odd harmonics, real THD; transfer curve flattens.
    {
        Device d; d.sampleRate = 48000.0; d.reset = [] {};
        d.process = [] (juce::AudioBuffer<float>& b) { for (int c = 0; c < 2; ++c) for (int i = 0; i < b.getNumSamples(); ++i) b.setSample (c, i, std::tanh (4.0f * b.getSample (c, i)) / 4.0f); };
        const auto h = harmonics (d, 1000.0f, -6.0f);
        EXPECT (h.thdPct > 1.0f, "tanh drive shows real THD (%.2f%%)", h.thdPct);
        EXPECT (h.levelsDb[1] > h.levelsDb[0] + 30.0f, "odd harmonic (3rd %.1f dBc) dominates even (2nd %.1f dBc)", h.levelsDb[1], h.levelsDb[0]);
        const auto t = transfer (d);
        EXPECT (std::abs (t.outDbL.front() - t.inDb.front()) < 0.5f, "transfer: quiet signal passes at unity (%.2f vs %.2f)", t.outDbL.front(), t.inDb.front());
        EXPECT (t.outDbL.back() < t.inDb.back() - 6.0f, "transfer: hot signal is squashed (%.2f for %.2f in)", t.outDbL.back(), t.inDb.back());
        EXPECT (h.outSpectrum.l.size() == h.outSpectrum.hz.size() && h.inSpectrum.l.size() == h.outSpectrum.l.size(), "before/after spectra line up");
    }
    // 3) The real module, measured on a private copy (no rack slot taken, live instance untouched).
    {
        SolutionAudioProcessor proc; proc.prepareToPlay (48000.0, 512);
        const int active = proc.getRackBus().activeInstances.load();
        setParam (proc, "global_mix", 0.0f);                     // fully dry: the module must measure as a straight wire
        auto r = proc.runToneAnalysisNow (1000.0f, -18.0f);
        EXPECT (proc.getRackBus().activeInstances.load() == active, "measurement copy is invisible to Rack Link");
        const auto* m = r["magnitude"]["l"].getArray();
        EXPECT (m != nullptr && m->size() > 100, "magnitude curve returned");
        float worst = 0; if (m) for (int i = 20; i < m->size() - 20; ++i) worst = std::max (worst, std::abs ((float) (*m)[i]));
        EXPECT (worst < 1.0f, "dry module measures flat within 1 dB (worst %.2f dB)", worst);
        EXPECT ((float) r["harmonics"]["thd"] < 0.1f, "dry module THD tiny (%.4f%%)", (float) r["harmonics"]["thd"]);
        setParam (proc, "global_mix", 1.0f);
        EXPECT (proc.startToneAnalysis (1000.0f, -18.0f), "background analysis starts");
        EXPECT (! proc.startToneAnalysis (1000.0f, -18.0f), "only one runs at a time");
        juce::var res; for (int i = 0; i < 600 && res.isVoid(); ++i) { juce::Thread::sleep (20); res = proc.takeToneAnalysisResult(); }
        EXPECT (! res.isVoid() && res["transfer"]["in"].size() == 23, "background analysis finished with a transfer curve");
    }
    std::printf ("  done\n");
}

static void testEditHistory()
{
    std::printf ("Edit history: selective undo, go back but keep, automation ignored, saved with the session\n");
    SolutionAudioProcessor proc; proc.prepareToPlay (48000.0, 512);
    auto tick = [&] (int n = 2) { for (int i = 0; i < n; ++i) proc.pollHistoryForTest(); };
    auto norm = [&] (const char* id) { return proc.apvts.getParameter (id)->getValue(); };
    tick();                                                      // adopt the starting values
    const float w0 = norm ("stage_width"), g0 = norm ("glue_amount"), m0 = norm ("global_mix");
    proc.setParamReal ("stage_width", 1.5f); tick();
    proc.setParamReal ("glue_amount", 0.7f); tick();
    proc.setParamReal ("global_mix", 0.4f); tick();
    auto list = proc.historyJson();
    EXPECT (list.size() == 3, "three edits logged (%d)", list.size());
    const int sWidth = list[0]["seq"], sGlue = list[1]["seq"];
    EXPECT ((bool) list[0]["changes"][0]["later"] == false, "width not changed later");

    // Selective undo: only the glue edit goes back.
    EXPECT (proc.revertHistory (sGlue) == 1, "revert one moves one control"); tick();
    EXPECT (std::abs (norm ("glue_amount") - g0) < 1e-4f && std::abs (norm ("stage_width") - w0) > 0.01f && std::abs (norm ("global_mix") - m0) > 0.01f,
            "glue reverted, width and mix kept");
    list = proc.historyJson();
    EXPECT (list.size() == 4 && list[3]["label"].toString().startsWith ("Reverted"), "the revert is itself in the history (%s)", list[3]["label"].toString().toRawUTF8());

    // Go back to before everything, but keep the mix as it is now.
    proc.restoreHistory (0, juce::StringArray ("global_mix")); tick();
    EXPECT (std::abs (norm ("stage_width") - w0) < 1e-4f && std::abs (norm ("glue_amount") - g0) < 1e-4f, "went back to the start");
    EXPECT (std::abs (norm ("global_mix") - m0) > 0.01f, "kept the chosen control (mix)");
    (void) sWidth;

    // Automation while playing is not an edit; a hand on a control while playing is.
    const auto before = proc.historyJson().size();
    proc.setHostPlayingForTest (true);
    proc.apvts.getParameter ("stage_depth")->setValueNotifyingHost (0.9f); tick();
    EXPECT (proc.historyJson().size() == before, "automation playback not logged");
    proc.setParamReal ("stage_depth", 0.1f); tick();
    EXPECT (proc.historyJson().size() == before + 1, "a user move during playback is logged");
    proc.setHostPlayingForTest (false);

    // Saved with the session; loading is not itself an edit.
    juce::MemoryBlock saved; proc.getStateInformation (saved);
    SolutionAudioProcessor p2; p2.prepareToPlay (48000.0, 512);
    for (int i = 0; i < 2; ++i) p2.pollHistoryForTest();
    p2.setStateInformation (saved.getData(), (int) saved.getSize());
    for (int i = 0; i < 3; ++i) p2.pollHistoryForTest();
    EXPECT (p2.historyJson().size() == proc.historyJson().size(), "history restored, load not logged (%d vs %d)", p2.historyJson().size(), proc.historyJson().size());
    EXPECT (p2.revertHistory (sGlue) == 1, "old entries still revertible after reload");
    std::printf ("  done\n");
}

static void testDiagnostics()
{
    std::printf ("Diagnostics: real counters + real system readings\n");
    const double fs = 48000.0;
    {
        SolutionAudioProcessor proc; proc.prepareToPlay (fs, 512);
        juce::MidiBuffer midi; juce::AudioBuffer<float> b (2, 512);
        for (int k = 0; k < 10; ++k) { fillSine (b, fs, 440.0, 0.2f, 0.0, k * 512); proc.processBlock (b, midi); }
        EXPECT (proc.diag.inputClips.load() == 0 && proc.diag.badSamples.load() == 0 && proc.diag.phaseEvents.load() == 0,
                "a clean sine must log no events (clips %u, bad %u, phase %u)", proc.diag.inputClips.load(), proc.diag.badSamples.load(), proc.diag.phaseEvents.load());
        fillSine (b, fs, 440.0, 1.2f, 0.0, 0); proc.processBlock (b, midi);
        EXPECT (proc.diag.inputClips.load() == 1, "input over 0 dBFS must be counted once per block (%u)", proc.diag.inputClips.load());
        fillSine (b, fs, 440.0, 0.2f, 0.0, 0); b.setSample (0, 100, std::numeric_limits<float>::quiet_NaN()); proc.processBlock (b, midi);
        bool finite = true; for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 512; ++i) finite = finite && std::isfinite (b.getSample (ch, i));
        EXPECT (proc.diag.badSamples.load() == 1 && finite, "NaN input must be counted and never reach the output");
        for (int k = 0; k < 120; ++k)       // ~1.3 s of left = -right
        {
            fillSine (b, fs, 300.0, 0.3f, 0.0, k * 512);
            b.copyFrom (1, 0, b, 0, 0, 512); b.applyGain (1, 0, 512, -1.0f);
            proc.processBlock (b, midi);
        }
        EXPECT (proc.diag.phaseEvents.load() >= 1, "sustained out-of-phase audio must be logged (%u)", proc.diag.phaseEvents.load());
    }
    {
        SolutionDiag::SystemMonitor monitor;
        auto s = monitor.getSnapshot();
        for (int waited = 0; waited < 5000 && (s.historyCpuPct.empty() || (int) s.coreLoadPct.size() != s.logicalCores); waited += 100)
        {
            juce::Thread::sleep (100);                 // first readings need two polls; a busy machine can be slow
            s = monitor.getSnapshot();
        }
        EXPECT (s.logicalCores > 0 && (int) s.coreLoadPct.size() == s.logicalCores, "per-core loads: %d of %d cores", (int) s.coreLoadPct.size(), s.logicalCores);
        bool inRange = true; for (auto v : s.coreLoadPct) inRange = inRange && v >= 0.0f && v <= 100.0f;
        EXPECT (inRange && std::isfinite (s.cpuTotalPct), "core loads must be real percentages");
        EXPECT (std::isfinite (s.ramUsedMb) && s.ramUsedMb > 0 && s.ramUsedMb <= s.ramTotalMb, "RAM used %.0f of %.0f MB", s.ramUsedMb, s.ramTotalMb);
        EXPECT (s.historyCpuPct.size() >= 1, "history must fill once a second");
        const auto csv = SolutionDiag::makeCsvReport (s, {}, {});
        const auto xml = juce::parseXML (SolutionDiag::makeXmlReport (s, {}, {}));
        EXPECT (csv.contains ("Core 1,") && xml != nullptr && xml->getChildByName ("System") != nullptr, "CSV/XML reports must be well-formed");
        std::printf ("  %s, %d cores, CPU %.0f%%, temp %s, RAM %.0f/%.0f MB, %d GPU(s)\n", s.cpuModel.toRawUTF8(), s.logicalCores, s.cpuTotalPct,
                     std::isfinite (s.cpuTempC) ? juce::String (s.cpuTempC, 1).toRawUTF8() : "n/a", s.ramUsedMb, s.ramTotalMb, (int) s.gpus.size());
    }
    std::printf ("  done\n");
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    testEngines();
    testProcessorState();
    testPresets();
    testMetering();
    testLatencyAndCeiling();
    testRackLink();
    testRoomCorrection();
    testBinaural();
    testMixAssistant();
    testStemFocus();
    testNeuralCharacter();
    testRemote();
    testModeNotchSwitch();
    testProTricks();
    testListeningRooms();
    testUnmask();
    testAutoAlign();
    testSafeBounce();
    testStageEngine();
    testDqs();
    testInfraLift();
    testLookaheadAndLearn();
    testDcGuard();
    testDialogueAndTransients();
    testVoiceParser();
    testHeadTracking();
    testNetworkOffload();
    testEditHistory();
    testToneAnalyzer();
    testDiagnostics();
    std::printf ("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
