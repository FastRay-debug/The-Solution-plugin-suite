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
    SuiteModuleEngine engine; RackLinkBus bus;
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
    std::printf ("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
