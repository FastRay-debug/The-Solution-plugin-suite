#pragma once
// The Bridge: hosts third-party plugins (VST3; AU on macOS) inside The Solution.
//  - Up to 4 plugins in series, each with bypass and dry/wet mix (the dry path is delayed to stay aligned).
//  - DAW sync: every hosted plugin gets the host's play head (tempo, position, transport).
//  - Latency: the chain's total is reported to the DAW (re-reported when the chain changes).
//  - CV routing: 8 Bridge knobs, each learnable onto any parameter of any hosted plugin - so The Matrix,
//    MIDI Learn and DAW automation can drive third-party plugins.
//  - Everything (which plugins, their own state, the knob mappings) is saved in the session.
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>

namespace SolutionBridge
{
    class Host
    {
    public:
        static constexpr int maxSlots = 4, numKnobs = 8;
        struct Slot
        {
            std::unique_ptr<juce::AudioPluginInstance> inst;
            juce::PluginDescription desc;
            std::atomic<bool> bypass { false };
            std::atomic<float> mix { 1.0f };
            int latency = 0;
            std::vector<std::vector<float>> dryLine; int dryPos = 0;
            struct Listener : juce::AudioProcessorParameter::Listener
            {
                Host* host = nullptr; int slot = 0;
                void parameterValueChanged (int index, float) override { host->learnFrom (slot, index); }
                void parameterGestureChanged (int, bool) override {}
            } listener;
        };
        struct Map { int slot = -1, param = -1; float lastSent = -1.0f; };

        Host()
        {
           #if JUCE_PLUGINHOST_VST3
            formats.addFormat (new juce::VST3PluginFormat());
           #endif
           #if JUCE_PLUGINHOST_AU && JUCE_MAC
            formats.addFormat (new juce::AudioUnitPluginFormat());
           #endif
        }
        ~Host() { for (auto& s : slots) detach (s); }

        void prepare (double sr, int bs)
        {
            sampleRate = sr; blockSize = std::max (16, bs);
            scratch.setSize (8, blockSize * 2); dry.setSize (2, blockSize * 2);
            for (auto& s : slots) if (s.inst) { s.inst->setRateAndBufferSizeDetails (sr, blockSize); s.inst->prepareToPlay (sr, blockSize); prepareSlotLines (s); }
        }
        void release() { for (auto& s : slots) if (s.inst) s.inst->releaseResources(); }

        // ---------- message thread: loading (call with processing suspended) ----------
        juce::StringArray candidateFiles() const   // plugin files in the usual places (not loaded until chosen)
        {
            juce::StringArray out;
            for (auto* f : formats.getFormats())
                for (auto& path : f->searchPathsForPlugins (f->getDefaultLocationsToSearch(), true, true)) out.addIfNotAlreadyThere (path);
            return out;
        }
        bool load (int slot, const juce::String& fileOrId, juce::String& error)
        {
            if (! juce::isPositiveAndBelow (slot, maxSlots)) { error = "No such slot"; return false; }
            juce::OwnedArray<juce::PluginDescription> found;
            for (auto* f : formats.getFormats()) if (f->fileMightContainThisPluginType (fileOrId)) f->findAllTypesForFile (found, fileOrId);
            if (found.isEmpty()) { error = "No plugin found in " + fileOrId; return false; }
            return loadDescription (slot, *found[0], error);
        }
        bool loadDescription (int slot, const juce::PluginDescription& d, juce::String& error)
        {
            auto inst = formats.createPluginInstance (d, sampleRate, blockSize, error);
            if (inst == nullptr) { if (error.isEmpty()) error = "Could not open " + d.name; return false; }
            // stereo in / out where the plugin allows it
            auto layout = inst->getBusesLayout();
            if (layout.inputBuses.size() > 0) layout.inputBuses.getReference (0) = juce::AudioChannelSet::stereo();
            if (layout.outputBuses.size() > 0) layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();
            if (layout.inputBuses.size() > 1) layout.inputBuses.getReference (1) = juce::AudioChannelSet::stereo();   // its sidechain input, if it has one
            if (! inst->setBusesLayout (layout) && layout.inputBuses.size() > 1) { layout.inputBuses.getReference (1) = juce::AudioChannelSet::disabled(); inst->setBusesLayout (layout); }
            inst->setRateAndBufferSizeDetails (sampleRate, blockSize);
            inst->prepareToPlay (sampleRate, blockSize);
            auto& s = slots[(size_t) slot];
            detach (s);
            s.inst = std::move (inst); s.desc = d; s.bypass = false; s.mix = 1.0f;
            s.listener.host = this; s.listener.slot = slot;
            for (auto* p : s.inst->getParameters()) p->addListener (&s.listener);
            prepareSlotLines (s);
            return true;
        }
        void unload (int slot) { if (juce::isPositiveAndBelow (slot, maxSlots)) { detach (slots[(size_t) slot]); for (auto& m : maps) if (m.slot == slot) m = {}; } }
        void move (int from, int to) { if (juce::isPositiveAndBelow (from, maxSlots) && juce::isPositiveAndBelow (to, maxSlots)) { swapSlots (from, to); } }

        int totalLatency() const { int t = 0; for (auto& s : slots) if (s.inst) t += s.inst->getLatencySamples(); return t; }
        juce::AudioPluginInstance* instance (int slot) { return juce::isPositiveAndBelow (slot, maxSlots) ? slots[(size_t) slot].inst.get() : nullptr; }
        Slot& slot (int i) { return slots[(size_t) i]; }

        // ---------- CV knobs ----------
        void armLearn (int knob) { learnKnob = juce::isPositiveAndBelow (knob, numKnobs) ? knob : -1; }
        int learning() const { return learnKnob; }
        void clearKnob (int k) { if (juce::isPositiveAndBelow (k, numKnobs)) maps[(size_t) k] = {}; }
        const Map& knobMap (int k) const { return maps[(size_t) k]; }
        juce::String knobTarget (int k) const
        {
            const auto& m = maps[(size_t) k];
            if (m.slot < 0 || slots[(size_t) m.slot].inst == nullptr) return {};
            auto& ps = slots[(size_t) m.slot].inst->getParameters();
            return juce::isPositiveAndBelow (m.param, ps.size()) ? slots[(size_t) m.slot].desc.name + ": " + ps[m.param]->getName (40) : juce::String();
        }

        // ---------- audio thread ----------
        void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi, juce::AudioPlayHead* playHead, const float* knobValues,
                      const float* scL = nullptr, const float* scR = nullptr)
        {
            const int n = buffer.getNumSamples();
            if (n > scratch.getNumSamples()) return;                             // host gave more than announced: pass through
            for (int k = 0; k < numKnobs; ++k)                                   // CV knobs -> hosted parameters
            {
                auto& m = maps[(size_t) k];
                if (m.slot < 0 || knobValues == nullptr) continue;
                auto* inst = slots[(size_t) m.slot].inst.get();
                if (inst == nullptr) continue;
                auto& ps = inst->getParameters();
                if (juce::isPositiveAndBelow (m.param, ps.size()) && std::abs (knobValues[k] - m.lastSent) > 1e-5f) { ps[m.param]->setValue (knobValues[k]); m.lastSent = knobValues[k]; }
            }
            for (auto& s : slots)
            {
                auto* inst = s.inst.get();
                if (inst == nullptr) continue;
                inst->setPlayHead (playHead);                                    // DAW sync
                const int chans = std::max (2, std::max (inst->getTotalNumInputChannels(), inst->getTotalNumOutputChannels()));
                if (scratch.getNumChannels() < chans) continue;                  // can't run it safely here
                for (int c = 0; c < 2; ++c) dry.copyFrom (c, 0, buffer, c, 0, n);
                juce::AudioBuffer<float> io (scratch.getArrayOfWritePointers(), chans, n);
                io.clear();
                for (int c = 0; c < 2; ++c) io.copyFrom (c, 0, buffer, c, 0, n);
                if (scL != nullptr && inst->getBusCount (true) > 1 && inst->getChannelCountOfBus (true, 1) >= 2 && chans >= 4)
                {   // Rack Link sidechain into the plugin's sidechain input
                    const int sc0 = inst->getChannelIndexInProcessBlockBuffer (true, 1, 0);
                    io.copyFrom (sc0, 0, scL, n); io.copyFrom (sc0 + 1, 0, scR, n);
                }
                if (s.bypass.load()) inst->processBlockBypassed (io, midi); else inst->processBlock (io, midi);
                // dry path delayed by the plugin's latency so the mix never combs
                const int lat = inst->getLatencySamples();
                float* dp[2] = { dry.getWritePointer (0), dry.getWritePointer (1) };
                delay (s, dp, n, lat);
                const float w = s.bypass.load() ? 1.0f : s.mix.load();
                for (int c = 0; c < 2; ++c)
                {
                    auto* out = buffer.getWritePointer (c); const auto* wet = io.getReadPointer (c); const auto* d = dry.getReadPointer (c);
                    for (int i = 0; i < n; ++i) out[i] = d[i] + (wet[i] - d[i]) * w;
                }
            }
        }

        // ---------- session ----------
        std::unique_ptr<juce::XmlElement> toXml() const
        {
            auto x = std::make_unique<juce::XmlElement> ("BRIDGE");
            for (int i = 0; i < maxSlots; ++i)
            {
                auto& s = slots[(size_t) i];
                if (s.inst == nullptr) continue;
                auto* e = x->createNewChildElement ("SLOT");
                e->setAttribute ("index", i); e->setAttribute ("bypass", s.bypass.load()); e->setAttribute ("mix", s.mix.load());
                e->addChildElement (s.desc.createXml().release());
                juce::MemoryBlock st; s.inst->getStateInformation (st); e->setAttribute ("state", st.toBase64Encoding());
            }
            for (int k = 0; k < numKnobs; ++k) if (maps[(size_t) k].slot >= 0) { auto* m = x->createNewChildElement ("MAP"); m->setAttribute ("knob", k); m->setAttribute ("slot", maps[(size_t) k].slot); m->setAttribute ("param", maps[(size_t) k].param); }
            return x;
        }
        juce::StringArray fromXml (const juce::XmlElement* x)          // returns plugins that could not be found
        {
            juce::StringArray missing;
            for (auto& s : slots) detach (s);
            for (auto& m : maps) m = {};
            if (x == nullptr) return missing;
            for (auto* e : x->getChildWithTagNameIterator ("SLOT"))
            {
                juce::PluginDescription d;
                if (auto* dx = e->getChildByName ("PLUGIN"); dx == nullptr || ! d.loadFromXml (*dx)) continue;
                juce::String err;
                const int i = e->getIntAttribute ("index");
                if (! loadDescription (i, d, err)) { missing.add (d.name); continue; }
                juce::MemoryBlock st; st.fromBase64Encoding (e->getStringAttribute ("state"));
                if (st.getSize() > 0) slots[(size_t) i].inst->setStateInformation (st.getData(), (int) st.getSize());
                slots[(size_t) i].bypass = e->getBoolAttribute ("bypass"); slots[(size_t) i].mix = (float) e->getDoubleAttribute ("mix", 1.0);
            }
            for (auto* m : x->getChildWithTagNameIterator ("MAP"))
            {
                const int k = m->getIntAttribute ("knob");
                if (juce::isPositiveAndBelow (k, numKnobs)) maps[(size_t) k] = { m->getIntAttribute ("slot", -1), m->getIntAttribute ("param", -1), -1.0f };
            }
            return missing;
        }

        std::function<void (int knob)> onLearned;                        // message thread notification

        // A hosted plugin's control moved while a knob is armed: map it (may come from any thread).
        void learnFrom (int slot, int index)
        {
            const int k = learnKnob.exchange (-1);
            if (k < 0) return;
            maps[(size_t) k] = { slot, index, -1.0f };
            juce::MessageManager::callAsync ([this, k] { if (onLearned) onLearned (k); });
        }
    private:
        void detach (Slot& s)
        {
            if (s.inst == nullptr) return;
            for (auto* p : s.inst->getParameters()) p->removeListener (&s.listener);
            s.inst->releaseResources(); s.inst.reset();
        }
        void prepareSlotLines (Slot& s) { s.dryLine.assign (2, std::vector<float> ((size_t) std::max (1, s.inst ? s.inst->getLatencySamples() : 0) + 1, 0.0f)); s.dryPos = 0; }
        static void delay (Slot& s, float* const* io, int n, int d)
        {
            if (d <= 0) return;
            if ((int) s.dryLine[0].size() < d + 1) return;                   // latency grew without a re-prepare: skip alignment
            const int size = (int) s.dryLine[0].size();
            for (int i = 0; i < n; ++i)
            {
                const int rd = (s.dryPos - d + size) % size;
                for (int c = 0; c < 2; ++c) { const float x = io[c][i]; io[c][i] = s.dryLine[(size_t) c][(size_t) rd]; s.dryLine[(size_t) c][(size_t) s.dryPos] = x; }
                s.dryPos = (s.dryPos + 1) % size;
            }
        }
        void swapSlots (int a, int b)
        {
            for (int i : { a, b }) if (slots[(size_t) i].inst) for (auto* p : slots[(size_t) i].inst->getParameters()) p->removeListener (&slots[(size_t) i].listener);
            std::swap (slots[(size_t) a].inst, slots[(size_t) b].inst); std::swap (slots[(size_t) a].desc, slots[(size_t) b].desc);
            for (int i : { a, b }) if (slots[(size_t) i].inst) { slots[(size_t) i].listener.host = this; slots[(size_t) i].listener.slot = i; for (auto* p : slots[(size_t) i].inst->getParameters()) p->addListener (&slots[(size_t) i].listener); }
            const bool ba = slots[(size_t) a].bypass, bb = slots[(size_t) b].bypass; slots[(size_t) a].bypass = bb; slots[(size_t) b].bypass = ba;
            const float ma = slots[(size_t) a].mix, mb = slots[(size_t) b].mix; slots[(size_t) a].mix = mb; slots[(size_t) b].mix = ma;
            prepareSlotLines (slots[(size_t) a]); prepareSlotLines (slots[(size_t) b]);
            for (auto& m : maps) { if (m.slot == a) m.slot = b; else if (m.slot == b) m.slot = a; }
        }

        juce::AudioPluginFormatManager formats;
        std::array<Slot, maxSlots> slots;
        std::array<Map, numKnobs> maps;
        std::atomic<int> learnKnob { -1 };
        juce::AudioBuffer<float> scratch, dry;
        double sampleRate = 48000.0; int blockSize = 512;
    };
}
