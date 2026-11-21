#pragma once
// The Solution - tells the standalone Solution Utility what the Solution plugins in this DAW are doing.
// One per DAW process. Twice a second it sends a small JSON summary to 127.0.0.1 (this computer only);
// if the Utility isn't running nobody is listening and nothing happens.
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "ModuleEngines.h"

namespace SolutionDiag
{
    inline constexpr int utilityPort = 47790;

    inline juce::var describeRackSlots (SolutionDSP::RackLinkBus& bus, int selfSlot, double sampleRate)
    {
        juce::Array<juce::var> list;
        const double fs = sampleRate > 0 ? sampleRate : 48000.0;
        for (size_t i = 0; i < bus.slots.size(); ++i)
        {
            auto& sl = bus.slots[i];
            if (! sl.active.load()) continue;
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            o->setProperty ("slot", (int) i);
            o->setProperty ("self", (int) i == selfSlot);
            o->setProperty ("module", juce::String (SolutionDSP::moduleDisplayName (static_cast<SolutionDSP::ModuleId> (sl.moduleId.load()))));
            o->setProperty ("name", sl.trackName);
            o->setProperty ("load", sl.cpuLoad.load());
            o->setProperty ("latency", sl.latency.load());
            o->setProperty ("latencyMs", sl.latency.load() * 1000.0 / fs);
            o->setProperty ("dqs", sl.dqsLevel.load());
            o->setProperty ("overloads", (int) sl.overloads.load());
            o->setProperty ("errors", (int) sl.errors.load());
            o->setProperty ("lufs", sl.lufsShort.load());
            o->setProperty ("tp", sl.truePeak.load());
            list.add (juce::var (o.get()));
        }
        return list;
    }

    class DiagBridge final : private juce::Timer
    {
    public:
        DiagBridge() { startTimer (500); }
        ~DiagBridge() override { stopTimer(); }
        std::atomic<double> sampleRate { 48000.0 };

    private:
        void timerCallback() override
        {
            if (bus->activeInstances.load() <= 0) return;
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            o->setProperty ("v", 1);
            o->setProperty ("pid", processKey);                 // tells DAWs apart
            o->setProperty ("host", juce::String (juce::PluginHostType().getHostDescription()));
            o->setProperty ("plugins", describeRackSlots (*bus, -1, sampleRate.load()));
            const auto json = juce::JSON::toString (juce::var (o.get()), true);
            if (json.getNumBytesAsUTF8() < 60000)
                socket.write ("127.0.0.1", utilityPort, json.toRawUTF8(), (int) json.getNumBytesAsUTF8());
        }
        juce::SharedResourcePointer<SolutionDSP::RackLinkBus> bus;
        juce::DatagramSocket socket { false };
        const juce::int64 processKey = juce::Random().nextInt64() & 0x7fffffffffffll;   // one bridge per DAW process
    };
}
