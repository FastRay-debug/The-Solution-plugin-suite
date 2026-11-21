#pragma once
// The Solution - one voice-control hub per DAW process. Owns the recogniser, keeps its vocabulary in
// step with the session (module and track names), and hands each command to the right plugin.
// Message thread only.
#include <juce_events/juce_events.h>
#include "VoiceCommands.h"
#include "SpeechInput.h"
#include "ModuleEngines.h"
#include <map>

namespace SolutionVoice
{
    class VoiceHub final : private juce::Timer
    {
    public:
        struct Client
        {
            int slot = -1;
            std::function<juce::String (const Command&)> execute;    // returns "" when done, else why not
            std::function<void (const juce::var&)> notify;          // caption: what was heard / what happened
        };

        VoiceHub() { speech.onPhrase = [this] (const juce::String& s, float c) { heard (s, c); }; }
        ~VoiceHub() override { stopTimer(); speech.stop(); }

        void setParams (std::vector<ParamInfo> p) { if (parser.params.empty()) parser.params = std::move (p); }
        void setWakeWordRequired (bool on) { parser.requireWakeWord = on; }
        bool wakeWordRequired() const { return parser.requireWakeWord; }
        void noteActive (int slot) { activeSlot = slot; }
        bool isListening() const { return speech.isListening(); }

        // Returns "" if listening, else the reason it can't.
        juce::String addClient (void* key, Client c)
        {
            clients[key] = std::move (c);
            refreshTargets();
            juce::String why;
            if (! speech.isListening() && ! speech.start (parser.vocabulary(), why)) { clients.erase (key); return why; }
            startTimer (2000);
            return {};
        }
        void removeClient (void* key)
        {
            clients.erase (key);
            if (clients.empty()) { stopTimer(); speech.stop(); }
        }
        // For tests and the UI's "try it" box: run a phrase as if it had been heard.
        void simulate (const juce::String& phrase) { heard (phrase, 1.0f); }
        Parser& getParser() { return parser; }

    private:
        void timerCallback() override { refreshTargets(); speech.setVocabulary (parser.vocabulary()); }

        void refreshTargets()
        {
            std::vector<Target> t;
            for (size_t i = 0; i < bus->slots.size(); ++i)
            {
                const auto& s = bus->slots[i];
                if (! s.active.load()) continue;
                const juce::String module (SolutionDSP::moduleDisplayName (static_cast<SolutionDSP::ModuleId> (s.moduleId.load())));
                t.push_back ({ module, (int) i });
                if (module.startsWithIgnoreCase ("the ")) t.push_back ({ module.substring (4), (int) i });
                if (s.trackName.isNotEmpty()) t.push_back ({ s.trackName, (int) i });
            }
            parser.targets = std::move (t);
        }

        void heard (const juce::String& phrase, float confidence)
        {
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            o->setProperty ("heard", phrase);
            const auto cmd = parser.parse (phrase);
            if (cmd.kind == Command::None)
            {
                o->setProperty ("ok", false);
                o->setProperty ("did", parser.requireWakeWord && ! phrase.trim().startsWithIgnoreCase (parser.wakeWord) ? juce::String() : juce::String ("not a command I know"));
            }
            else if (confidence < 0.3f)
            {
                o->setProperty ("ok", false); o->setProperty ("did", "didn't catch that clearly - say it again");
            }
            else
            {
                const int slot = cmd.slot >= 0 ? cmd.slot : activeSlot;
                Client* target = nullptr;
                for (auto& [k, c] : clients) if (c.slot == slot) target = &c;
                if (target == nullptr && ! clients.empty()) target = &clients.begin()->second;   // can still reach other slots via Rack Link
                const auto why = target != nullptr ? target->execute (cmd) : juce::String ("no Solution plugin is listening");
                o->setProperty ("ok", why.isEmpty());
                o->setProperty ("did", why.isEmpty() ? cmd.describe : why);
            }
            const juce::var v (o.get());
            for (auto& [k, c] : clients) if (c.notify) c.notify (v);
        }

        Parser parser;
        SpeechInput speech;
        std::map<void*, Client> clients;
        int activeSlot = -1;
        juce::SharedResourcePointer<SolutionDSP::RackLinkBus> bus;
    };
}
