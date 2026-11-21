#pragma once

#include <juce_core/juce_core.h>
#include <algorithm>

// Dynamic Quality Scaling: picks the oversampling level from how much of the real-time budget
// the whole suite is using. Steps down quickly when overloaded, back up slowly when there's room.
namespace SolutionDSP
{
    struct DqsController
    {
        enum Mode { autoMode = 0, locked4x = 1, eco = 2 };

        // Suite load: summed instance loads spread over the cores the DAW can use.
        static float suiteLoad (float summedLoads, int activeInstances, int cores)
        {
            return summedLoads / (float) std::max (1, std::min (std::max (1, cores), std::max (1, activeInstances)));
        }

        // Call once per block. Returns the quality level to use (0 = 4x, 1 = 2x, 2 = 1x).
        int update (int mode, bool offline, float load, float threshold, double blockSeconds)
        {
            if (offline || mode == locked4x) { level = 0; over = under = 0.0; return level; }
            if (mode == eco)                 { level = 2; over = under = 0.0; return level; }
            if (load > threshold)            { over += blockSeconds; under = 0.0; }
            else if (load < threshold * 0.6f) { under += blockSeconds; over = 0.0; }
            else                             { over = under = 0.0; }
            if (over >= 0.25 && level < 2)   { ++level; over = 0.0; }        // overloaded for 1/4 s -> lighter
            if (under >= 3.0 && level > 0)   { --level; under = 0.0; }       // 3 s of headroom -> better
            return level;
        }

        int level = 0;
        double over = 0.0, under = 0.0;
    };
}
