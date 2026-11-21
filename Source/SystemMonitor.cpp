// The Solution - real system readings for the Diagnostic Utility (see SystemMonitor.h).
#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <pdh.h>
 #include <psapi.h>
 #pragma comment (lib, "pdh.lib")
#elif defined (__APPLE__)
 #include <mach/mach.h>
 #include <mach/mach_host.h>
 #include <mach/processor_info.h>
 #include <sys/sysctl.h>
 #include <ctime>
#endif

#include "SystemMonitor.h"
#include <algorithm>
#include <map>

namespace SolutionDiag
{
// ---------------------------------------------------------------- NVIDIA (NVML ships with the driver; no extra install)
namespace
{
    struct Nvml
    {
        using Init = int (*)();
        using Count = int (*) (unsigned int*);
        using Handle = int (*) (unsigned int, void**);
        using Name = int (*) (void*, char*, unsigned int);
        using UInt = int (*) (void*, unsigned int*);
        using Temp = int (*) (void*, int, unsigned int*);
        using Clock = int (*) (void*, int, unsigned int*);
        struct Util { unsigned int gpu, memory; };
        struct Mem { unsigned long long total, free, used; };
        using UtilFn = int (*) (void*, Util*);
        using MemFn = int (*) (void*, Mem*);

        juce::DynamicLibrary lib;
        bool ok = false;
        Init init = nullptr; Count count = nullptr; Handle handle = nullptr; Name name = nullptr;
        Temp temp = nullptr; UInt fan = nullptr; UtilFn util = nullptr; MemFn mem = nullptr; Clock clock = nullptr; UInt power = nullptr;

        Nvml()
        {
           #if defined (_WIN32)
            const char* candidates[] = { "nvml.dll", "C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll" };
           #elif defined (__linux__)
            const char* candidates[] = { "libnvidia-ml.so.1", "libnvidia-ml.so" };
           #else
            const char* candidates[] = { "" };
           #endif
            for (auto* c : candidates)
                if (*c != 0 && lib.open (c)) break;
            if (lib.getNativeHandle() == nullptr) return;
            init   = (Init)   lib.getFunction ("nvmlInit_v2");
            count  = (Count)  lib.getFunction ("nvmlDeviceGetCount_v2");
            handle = (Handle) lib.getFunction ("nvmlDeviceGetHandleByIndex_v2");
            name   = (Name)   lib.getFunction ("nvmlDeviceGetName");
            temp   = (Temp)   lib.getFunction ("nvmlDeviceGetTemperature");
            fan    = (UInt)   lib.getFunction ("nvmlDeviceGetFanSpeed");
            util   = (UtilFn) lib.getFunction ("nvmlDeviceGetUtilizationRates");
            mem    = (MemFn)  lib.getFunction ("nvmlDeviceGetMemoryInfo");
            clock  = (Clock)  lib.getFunction ("nvmlDeviceGetClockInfo");
            power  = (UInt)   lib.getFunction ("nvmlDeviceGetPowerUsage");
            ok = init != nullptr && count != nullptr && handle != nullptr && init() == 0;
        }

        void read (std::vector<GpuReading>& out)
        {
            if (! ok) return;
            unsigned int n = 0;
            if (count (&n) != 0) return;
            for (unsigned int i = 0; i < std::min (n, 8u); ++i)
            {
                void* dev = nullptr;
                if (handle (i, &dev) != 0) continue;
                GpuReading g; g.source = "NVML";
                char buf[96] = {};
                if (name != nullptr && name (dev, buf, sizeof (buf)) == 0) g.name = buf;
                unsigned int v = 0;
                if (temp  != nullptr && temp (dev, 0, &v) == 0)  g.tempC = (float) v;
                if (fan   != nullptr && fan (dev, &v) == 0)      g.fanPct = (float) v;
                if (clock != nullptr && clock (dev, 0, &v) == 0) g.clockMhz = (float) v;
                if (power != nullptr && power (dev, &v) == 0)    g.powerW = (float) v / 1000.0f;
                Util u {}; if (util != nullptr && util (dev, &u) == 0) g.loadPct = (float) u.gpu;
                Mem m {};  if (mem != nullptr && mem (dev, &m) == 0) { g.memUsedMb = (float) (m.used / 1048576ull); g.memTotalMb = (float) (m.total / 1048576ull); }
                out.push_back (g);
            }
        }
    };
}

// ---------------------------------------------------------------- per-platform readers
#if defined (_WIN32)
struct SystemMonitor::Platform
{
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER cores = nullptr, zone = nullptr, gpuEngine = nullptr, gpuMem = nullptr;
    Nvml nvml;
    juce::String adapterName;

    Platform()
    {
        if (PdhOpenQueryW (nullptr, 0, &query) != ERROR_SUCCESS) { query = nullptr; return; }
        // "% Processor Utility" matches Task Manager (Windows 8+); fall back to "% Processor Time".
        if (PdhAddEnglishCounterW (query, L"\\Processor Information(*)\\% Processor Utility", 0, &cores) != ERROR_SUCCESS)
            PdhAddEnglishCounterW (query, L"\\Processor Information(*)\\% Processor Time", 0, &cores);
        PdhAddEnglishCounterW (query, L"\\Thermal Zone Information(*)\\Temperature", 0, &zone);
        PdhAddEnglishCounterW (query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpuEngine);
        PdhAddEnglishCounterW (query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &gpuMem);
        PdhCollectQueryData (query);

        DISPLAY_DEVICEW dd {}; dd.cb = sizeof (dd);
        for (DWORD i = 0; EnumDisplayDevicesW (nullptr, i, &dd, 0); ++i)
            if ((dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0) { adapterName = juce::String (dd.DeviceString); break; }
    }
    ~Platform() { if (query != nullptr) PdhCloseQuery (query); }

    static std::vector<std::pair<juce::String, double>> readArray (PDH_HCOUNTER c)
    {
        std::vector<std::pair<juce::String, double>> out;
        if (c == nullptr) return out;
        DWORD bytes = 0, items = 0;
        if (PdhGetFormattedCounterArrayW (c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &items, nullptr) != (PDH_STATUS) PDH_MORE_DATA) return out;
        std::vector<BYTE> buf (bytes);
        auto* arr = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*> (buf.data());
        if (PdhGetFormattedCounterArrayW (c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &items, arr) != ERROR_SUCCESS) return out;
        for (DWORD i = 0; i < items; ++i)
            if (arr[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || arr[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
                out.emplace_back (juce::String (arr[i].szName), arr[i].FmtValue.doubleValue);
        return out;
    }

    void poll (Snapshot& s)
    {
        if (query != nullptr && PdhCollectQueryData (query) == ERROR_SUCCESS)
        {
            std::map<std::pair<int, int>, float> perCore;          // (group, index) -> load
            for (auto& [name, v] : readArray (cores))
            {
                if (name.contains ("_Total")) { if (name == "_Total") s.cpuTotalPct = (float) juce::jlimit (0.0, 100.0, v); continue; }
                perCore[{ name.upToFirstOccurrenceOf (",", false, false).getIntValue(), name.fromFirstOccurrenceOf (",", false, false).getIntValue() }]
                    = (float) juce::jlimit (0.0, 100.0, v);
            }
            for (auto& [k, v] : perCore) s.coreLoadPct.push_back (v);

            for (auto& [name, v] : readArray (zone))
            {
                const double kelvin = v > 1000.0 ? v / 10.0 : v;
                const float c = (float) (kelvin - 273.15);
                if (c > 5.0f && c < 125.0f && ! (c <= s.cpuTempC)) { s.cpuTempC = c; s.tempSource = "ACPI thermal zone"; }
            }

            // GPU load like Task Manager: per adapter, the busiest engine type; report the busiest adapter.
            std::map<juce::String, std::map<juce::String, double>> adapters;
            for (auto& [name, v] : readArray (gpuEngine))
            {
                const auto luid = name.fromFirstOccurrenceOf ("luid_", false, false).upToFirstOccurrenceOf ("_phys", false, false);
                const auto type = name.fromFirstOccurrenceOf ("engtype_", false, false);
                adapters[luid][type] += v;
            }
            double best = -1.0;
            for (auto& [luid, types] : adapters)
                for (auto& [type, v] : types) best = std::max (best, v);
            double dedicated = -1.0;
            for (auto& [name, v] : readArray (gpuMem)) dedicated = std::max (dedicated, v);

            nvml.read (s.gpus);
            if (s.gpus.empty() && (best >= 0.0 || adapterName.isNotEmpty()))
            {
                GpuReading g; g.source = "Windows"; g.name = adapterName;
                if (best >= 0.0) g.loadPct = (float) std::min (100.0, best);
                if (dedicated >= 0.0) g.memUsedMb = (float) (dedicated / 1048576.0);
                s.gpus.push_back (g);
            }
            else if (! s.gpus.empty() && ! std::isfinite (s.gpus.front().loadPct) && best >= 0.0)
                s.gpus.front().loadPct = (float) std::min (100.0, best);
        }

        MEMORYSTATUSEX ms {}; ms.dwLength = sizeof (ms);
        if (GlobalMemoryStatusEx (&ms)) { s.ramTotalMb = (float) (ms.ullTotalPhys / 1048576ull); s.ramUsedMb = (float) ((ms.ullTotalPhys - ms.ullAvailPhys) / 1048576ull); }
        PROCESS_MEMORY_COUNTERS_EX pmc {};
        if (K32GetProcessMemoryInfo (GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*> (&pmc), sizeof (pmc)))
            s.processRamMb = (float) (pmc.PrivateUsage / 1048576.0);
        s.uptimeSeconds = (double) GetTickCount64() / 1000.0;
    }
};

#elif defined (__linux__)
struct SystemMonitor::Platform
{
    std::vector<std::pair<unsigned long long, unsigned long long>> prev;   // (busy, total) per core
    std::pair<unsigned long long, unsigned long long> prevTotal { 0, 0 };
    Nvml nvml;

    static juce::String readFile (const juce::File& f) { return f.existsAsFile() ? f.loadFileAsString() : juce::String(); }

    void poll (Snapshot& s)
    {
        juce::StringArray lines; lines.addLines (readFile (juce::File ("/proc/stat")));
        std::vector<std::pair<unsigned long long, unsigned long long>> now;
        for (auto& l : lines)
        {
            if (! l.startsWith ("cpu")) continue;
            juce::StringArray t; t.addTokens (l, " ", ""); t.removeEmptyStrings();
            if (t.size() < 8) continue;
            unsigned long long total = 0;
            for (int i = 1; i < t.size(); ++i) total += (unsigned long long) t[i].getLargeIntValue();
            const unsigned long long idle = (unsigned long long) (t[4].getLargeIntValue() + t[5].getLargeIntValue());
            const std::pair<unsigned long long, unsigned long long> v { total - idle, total };
            auto pct = [] (auto a, auto b) { const auto dt = b.second - a.second; return dt == 0 ? 0.0f : 100.0f * (float) (b.first - a.first) / (float) dt; };
            if (t[0] == "cpu") { if (prevTotal.second != 0) s.cpuTotalPct = pct (prevTotal, v); prevTotal = v; }
            else
            {
                const size_t i = now.size(); now.push_back (v);
                s.coreLoadPct.push_back (i < prev.size() ? juce::jlimit (0.0f, 100.0f, pct (prev[i], v)) : 0.0f);
            }
        }
        prev = now;

        for (auto& dir : juce::File ("/sys/class/hwmon").findChildFiles (juce::File::findDirectories, false))
        {
            const auto chip = readFile (dir.getChildFile ("name")).trim();
            const bool cpuChip = chip == "coretemp" || chip == "k10temp" || chip == "zenpower" || chip == "cpu_thermal";
            for (auto& f : dir.findChildFiles (juce::File::findFiles, false, "*_input"))
            {
                const auto fn = f.getFileName();
                const float v = (float) readFile (f).trim().getDoubleValue();
                if (cpuChip && fn.startsWith ("temp") && v > 0.0f && ! (v / 1000.0f <= s.cpuTempC)) { s.cpuTempC = v / 1000.0f; s.tempSource = chip; }
                if (fn.startsWith ("fan")) s.fanRpm.push_back (v);
            }
        }
        if (! std::isfinite (s.cpuTempC))
            for (auto& z : juce::File ("/sys/class/thermal").findChildFiles (juce::File::findDirectories, false, "thermal_zone*"))
            {
                const auto type = readFile (z.getChildFile ("type")).trim();
                if (type.containsIgnoreCase ("pkg") || type.containsIgnoreCase ("cpu") || type.containsIgnoreCase ("soc"))
                { s.cpuTempC = (float) readFile (z.getChildFile ("temp")).trim().getDoubleValue() / 1000.0f; s.tempSource = type; break; }
            }

        juce::StringArray mem; mem.addLines (readFile (juce::File ("/proc/meminfo")));
        float total = 0, avail = -1;
        for (auto& l : mem)
        {
            if (l.startsWith ("MemTotal:"))     total = (float) l.fromFirstOccurrenceOf (":", false, false).trim().getDoubleValue() / 1024.0f;
            if (l.startsWith ("MemAvailable:")) avail = (float) l.fromFirstOccurrenceOf (":", false, false).trim().getDoubleValue() / 1024.0f;
        }
        if (total > 0) { s.ramTotalMb = total; if (avail >= 0) s.ramUsedMb = total - avail; }
        juce::StringArray st; st.addLines (readFile (juce::File ("/proc/self/status")));
        for (auto& l : st) if (l.startsWith ("VmRSS:")) s.processRamMb = (float) l.fromFirstOccurrenceOf (":", false, false).trim().getDoubleValue() / 1024.0f;
        s.uptimeSeconds = readFile (juce::File ("/proc/uptime")).getDoubleValue();
        nvml.read (s.gpus);
    }
};

#elif defined (__APPLE__)
struct SystemMonitor::Platform
{
    std::vector<std::pair<unsigned long long, unsigned long long>> prev;

    void poll (Snapshot& s)
    {
        natural_t n = 0; processor_info_array_t info = nullptr; mach_msg_type_number_t count = 0;
        if (host_processor_info (mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &n, &info, &count) == KERN_SUCCESS)
        {
            std::vector<std::pair<unsigned long long, unsigned long long>> now;
            unsigned long long busySum = 0, totalSum = 0, prevBusy = 0, prevTotalSum = 0;
            for (natural_t i = 0; i < n; ++i)
            {
                const auto* t = reinterpret_cast<const unsigned int*> (info) + i * CPU_STATE_MAX;
                const unsigned long long busy = (unsigned long long) t[CPU_STATE_USER] + t[CPU_STATE_SYSTEM] + t[CPU_STATE_NICE];
                const unsigned long long total = busy + t[CPU_STATE_IDLE];
                now.emplace_back (busy, total);
                if (i < prev.size())
                {
                    const auto dt = total - prev[i].second;
                    s.coreLoadPct.push_back (dt == 0 ? 0.0f : 100.0f * (float) (busy - prev[i].first) / (float) dt);
                    busySum += busy; totalSum += total; prevBusy += prev[i].first; prevTotalSum += prev[i].second;
                }
                else s.coreLoadPct.push_back (0.0f);
            }
            if (totalSum > prevTotalSum) s.cpuTotalPct = 100.0f * (float) (busySum - prevBusy) / (float) (totalSum - prevTotalSum);
            prev = now;
            vm_deallocate (mach_task_self(), (vm_address_t) info, count * sizeof (integer_t));
        }
        vm_statistics64_data_t vm {}; mach_msg_type_number_t vmCount = HOST_VM_INFO64_COUNT;
        if (host_statistics64 (mach_host_self(), HOST_VM_INFO64, (host_info64_t) &vm, &vmCount) == KERN_SUCCESS)
        {
            const double page = (double) vm_kernel_page_size;
            s.ramUsedMb = (float) ((vm.active_count + vm.wire_count + vm.compressor_page_count) * page / 1048576.0);
        }
        timeval boot {}; size_t len = sizeof (boot); int mib[2] = { CTL_KERN, KERN_BOOTTIME };
        if (sysctl (mib, 2, &boot, &len, nullptr, 0) == 0) s.uptimeSeconds = (double) (std::time (nullptr) - boot.tv_sec);
    }
};
#else
struct SystemMonitor::Platform { void poll (Snapshot&) {} };
#endif

// ---------------------------------------------------------------- monitor thread
SystemMonitor::SystemMonitor() : juce::Thread ("Solution System Monitor"), platform (std::make_unique<Platform>())
{
    snapshot.cpuModel = juce::SystemStats::getCpuModel();
    snapshot.cpuVendor = juce::SystemStats::getCpuVendor();
    snapshot.os = juce::SystemStats::getOperatingSystemName();
    snapshot.logicalCores = juce::SystemStats::getNumCpus();
    snapshot.physicalCores = juce::SystemStats::getNumPhysicalCpus();
    snapshot.ramTotalMb = (float) juce::SystemStats::getMemorySizeInMegabytes();
    startThread (juce::Thread::Priority::low);
}

SystemMonitor::~SystemMonitor() { stopThread (2000); }

void SystemMonitor::run()
{
    while (! threadShouldExit())
    {
        poll();
        wait (500);
    }
}

void SystemMonitor::poll()
{
    Snapshot s;
    { const juce::ScopedLock sl (lock); s = snapshot; }
    s.coreLoadPct.clear(); s.gpus.clear(); s.fanRpm.clear();
    s.cpuTempC = notAvailable; s.cpuTotalPct = notAvailable;
    platform->poll (s);
    if (! std::isfinite (s.cpuTotalPct) && ! s.coreLoadPct.empty())
    {
        float sum = 0; for (auto v : s.coreLoadPct) sum += v;
        s.cpuTotalPct = sum / (float) s.coreLoadPct.size();
    }
    if (++historyTick % 2 == 0)       // one history point per second
    {
        auto push = [] (std::vector<float>& h, float v) { h.push_back (v); if (h.size() > 60) h.erase (h.begin()); };
        push (s.historyCpuPct, s.cpuTotalPct);
        push (s.historyTempC, s.cpuTempC);
        push (s.historyGpuTempC, s.gpus.empty() ? notAvailable : s.gpus.front().tempC);
    }
    const juce::ScopedLock sl (lock);
    snapshot = std::move (s);
}

// ---------------------------------------------------------------- JSON for the panel
juce::var snapshotToVar (const Snapshot& s)
{
    auto arr = [] (const std::vector<float>& v) { juce::Array<juce::var> a; for (auto x : v) a.add (std::isfinite (x) ? juce::var (x) : juce::var()); return juce::var (a); };
    auto val = [] (float x) { return std::isfinite (x) ? juce::var (x) : juce::var(); };
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("cpuModel", s.cpuModel); o->setProperty ("os", s.os);
    o->setProperty ("logical", s.logicalCores); o->setProperty ("physical", s.physicalCores);
    o->setProperty ("cores", arr (s.coreLoadPct)); o->setProperty ("cpuTotal", val (s.cpuTotalPct));
    o->setProperty ("cpuTemp", val (s.cpuTempC)); o->setProperty ("tempSource", s.tempSource);
    o->setProperty ("fans", arr (s.fanRpm));
    o->setProperty ("ramUsed", val (s.ramUsedMb)); o->setProperty ("ramTotal", val (s.ramTotalMb)); o->setProperty ("procRam", val (s.processRamMb));
    o->setProperty ("uptime", s.uptimeSeconds);
    o->setProperty ("histCpu", arr (s.historyCpuPct)); o->setProperty ("histTemp", arr (s.historyTempC)); o->setProperty ("histGpuTemp", arr (s.historyGpuTempC));
    juce::Array<juce::var> gpus;
    for (auto& g : s.gpus)
    {
        juce::DynamicObject::Ptr e = new juce::DynamicObject();
        e->setProperty ("name", g.name); e->setProperty ("source", g.source); e->setProperty ("load", val (g.loadPct));
        e->setProperty ("temp", val (g.tempC)); e->setProperty ("fan", val (g.fanPct)); e->setProperty ("clock", val (g.clockMhz));
        e->setProperty ("memUsed", val (g.memUsedMb)); e->setProperty ("memTotal", val (g.memTotalMb)); e->setProperty ("power", val (g.powerW));
        gpus.add (juce::var (e.get()));
    }
    o->setProperty ("gpus", gpus);
    return juce::var (o.get());
}

// ---------------------------------------------------------------- reports
namespace
{
    juce::String num (float v, int dp = 1) { return std::isfinite (v) ? juce::String (v, dp) : juce::String ("n/a"); }
    juce::String csvField (const juce::String& s) { return "\"" + s.replace ("\"", "\"\"") + "\""; }
}

juce::String makeCsvReport (const Snapshot& s, const juce::var& plugins, const juce::var& log)
{
    juce::String o;
    o << "Section,Item,Value,Unit\n";
    o << "System,Report time," << csvField (juce::Time::getCurrentTime().toString (true, true, true, true)) << ",\n";
    o << "System,OS," << csvField (s.os) << ",\n";
    o << "System,CPU," << csvField (s.cpuModel) << ",\n";
    o << "System,Cores," << s.logicalCores << " logical / " << s.physicalCores << " physical,\n";
    o << "System,CPU total," << num (s.cpuTotalPct) << ",%\n";
    for (size_t i = 0; i < s.coreLoadPct.size(); ++i) o << "System,Core " << (int) i + 1 << "," << num (s.coreLoadPct[i]) << ",%\n";
    o << "System,CPU temperature," << num (s.cpuTempC) << ",C\n";
    o << "System,RAM used," << num (s.ramUsedMb, 0) << " of " << num (s.ramTotalMb, 0) << ",MB\n";
    o << "System,DAW process RAM," << num (s.processRamMb, 0) << ",MB\n";
    for (auto& g : s.gpus)
    {
        o << "GPU,Name," << csvField (g.name) << ",\n";
        o << "GPU,Load," << num (g.loadPct) << ",%\n" << "GPU,Temperature," << num (g.tempC) << ",C\n" << "GPU,Fan," << num (g.fanPct) << ",%\n";
        o << "GPU,Clock," << num (g.clockMhz, 0) << ",MHz\n" << "GPU,VRAM used," << num (g.memUsedMb, 0) << ",MB\n";
    }
    if (auto* arr = plugins.getArray())
        for (auto& p : *arr)
        {
            const auto name = p["module"].toString() + (p["name"].toString().isNotEmpty() ? " - " + p["name"].toString() : juce::String());
            o << "Plugin," << csvField (name + " DSP load") << "," << num ((float) (double) p["load"] * 100.0f) << ",%\n";
            o << "Plugin," << csvField (name + " latency") << "," << (int) p["latency"] << ",samples\n";
            o << "Plugin," << csvField (name + " overruns") << "," << (int) p["overloads"] << ",count\n";
            o << "Plugin," << csvField (name + " signal events") << "," << (int) p["errors"] << ",count\n";
        }
    if (auto* arr = log.getArray())
        for (auto& e : *arr) o << "Log," << csvField (e["time"].toString()) << "," << csvField (e["text"].toString()) << ",\n";
    return o;
}

juce::String makeXmlReport (const Snapshot& s, const juce::var& plugins, const juce::var& log)
{
    juce::XmlElement root ("TheSolutionDiagnosticReport");
    root.setAttribute ("created", juce::Time::getCurrentTime().toISO8601 (true));
    auto* sys = root.createNewChildElement ("System");
    sys->setAttribute ("os", s.os); sys->setAttribute ("cpu", s.cpuModel);
    sys->setAttribute ("logicalCores", s.logicalCores); sys->setAttribute ("physicalCores", s.physicalCores);
    sys->setAttribute ("cpuTotalPct", num (s.cpuTotalPct)); sys->setAttribute ("cpuTempC", num (s.cpuTempC));
    sys->setAttribute ("ramUsedMb", num (s.ramUsedMb, 0)); sys->setAttribute ("ramTotalMb", num (s.ramTotalMb, 0));
    for (size_t i = 0; i < s.coreLoadPct.size(); ++i)
    {
        auto* c = sys->createNewChildElement ("Core"); c->setAttribute ("index", (int) i + 1); c->setAttribute ("loadPct", num (s.coreLoadPct[i]));
    }
    for (auto& g : s.gpus)
    {
        auto* e = root.createNewChildElement ("GPU");
        e->setAttribute ("name", g.name); e->setAttribute ("source", g.source); e->setAttribute ("loadPct", num (g.loadPct));
        e->setAttribute ("tempC", num (g.tempC)); e->setAttribute ("fanPct", num (g.fanPct)); e->setAttribute ("clockMhz", num (g.clockMhz, 0));
        e->setAttribute ("vramUsedMb", num (g.memUsedMb, 0)); e->setAttribute ("vramTotalMb", num (g.memTotalMb, 0));
    }
    auto* pl = root.createNewChildElement ("Plugins");
    if (auto* arr = plugins.getArray())
        for (auto& p : *arr)
        {
            auto* e = pl->createNewChildElement ("Plugin");
            e->setAttribute ("module", p["module"].toString()); e->setAttribute ("track", p["name"].toString());
            e->setAttribute ("dspLoadPct", num ((float) (double) p["load"] * 100.0f)); e->setAttribute ("latencySamples", (int) p["latency"]);
            e->setAttribute ("quality", (int) p["dqs"]); e->setAttribute ("overruns", (int) p["overloads"]); e->setAttribute ("signalEvents", (int) p["errors"]);
        }
    auto* lg = root.createNewChildElement ("Log");
    if (auto* arr = log.getArray())
        for (auto& ev : *arr) { auto* e = lg->createNewChildElement ("Event"); e->setAttribute ("time", ev["time"].toString()); e->addTextElement (ev["text"].toString()); }
    return root.toString();
}
}
