#pragma once
// The Solution - vendor name and the per-user data folder (presets, models).
#include <juce_core/juce_core.h>

namespace SolutionVendor
{
    inline constexpr const char* name       = "Billet Audio";
    inline constexpr const char* legacyName = "Fast Ray Media";     // earlier builds used this folder

    inline juce::String prefix()
    {
       #if JUCE_MAC
        return "Application Support/";
       #else
        return {};
       #endif
    }

    // <user data>/Billet Audio/The Solution/<leaf>. The first time, an existing
    // "Fast Ray Media/The Solution" folder is moved across so nobody loses presets or models.
    inline juce::File userFolder (const juce::String& leaf)
    {
        const auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
        const auto root = base.getChildFile (prefix() + name + "/The Solution");
        const auto old  = base.getChildFile (prefix() + legacyName + "/The Solution");
        if (! root.isDirectory() && old.isDirectory())
        {
            root.getParentDirectory().createDirectory();
            if (! old.moveFileTo (root))
                old.copyDirectoryTo (root);
        }
        return root.getChildFile (leaf);
    }
}
