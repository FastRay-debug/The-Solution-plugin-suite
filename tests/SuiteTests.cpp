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
    { "TheMatrix", ModuleId::Matrix }, { "TheScope", ModuleId::Scope } };

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

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    testEngines();
    testProcessorState();
    testPresets();
    testMetering();
    testLatencyAndCeiling();
    std::printf ("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
