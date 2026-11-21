#pragma once
// The Solution - real system readings for the Diagnostic Utility.
// Everything here is measured; anything the machine doesn't expose without a driver is reported as
// unavailable (NaN / empty), never invented.
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <cmath>
#include <limits>
#include <vector>

namespace SolutionDiag
{
    inline constexpr float notAvailable = std::numeric_limits<float>::quiet_NaN();

    struct GpuReading
    {
        juce::String name, source;               // source: "NVML" (NVIDIA driver) or "Windows" (any vendor)
        float loadPct = notAvailable, tempC = notAvailable, fanPct = notAvailable,
              clockMhz = notAvailable, memUsedMb = notAvailable, memTotalMb = notAvailable, powerW = notAvailable;
    };

    struct Snapshot
    {
        juce::String cpuModel, cpuVendor, os, tempSource;
        int logicalCores = 0, physicalCores = 0;
        std::vector<float> coreLoadPct;          // per logical core, 0..100
        float cpuTotalPct = notAvailable;
        float cpuTempC = notAvailable;           // hottest CPU / thermal-zone sensor the OS exposes
        std::vector<float> fanRpm;               // only where the OS exposes fans (Linux hwmon)
        float ramTotalMb = notAvailable, ramUsedMb = notAvailable, processRamMb = notAvailable;
        std::vector<GpuReading> gpus;
        double uptimeSeconds = 0.0;
        // last 60 seconds, oldest first
        std::vector<float> historyCpuPct, historyTempC, historyGpuTempC;
    };

    // One per process (shared by every editor that shows diagnostics). Polls twice a second on its own thread.
    class SystemMonitor final : private juce::Thread
    {
    public:
        SystemMonitor();
        ~SystemMonitor() override;
        Snapshot getSnapshot() const { const juce::ScopedLock sl (lock); return snapshot; }

    private:
        void run() override;
        void poll();
        struct Platform;
        std::unique_ptr<Platform> platform;
        juce::CriticalSection lock;
        Snapshot snapshot;
        int historyTick = 0;
    };

    juce::var snapshotToVar (const Snapshot& s);              // what the Diagnostics page draws
    // Text reports for the Export buttons (message thread).
    juce::String makeCsvReport (const Snapshot& s, const juce::var& plugins, const juce::var& log);
    juce::String makeXmlReport (const Snapshot& s, const juce::var& plugins, const juce::var& log);
}
