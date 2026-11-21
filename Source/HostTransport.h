#pragma once
// The Solution - drive the DAW's transport by pressing its own keyboard shortcuts.
// Plugins can't start/stop a host's transport directly, so voice commands bring the DAW window to the
// front and send the shortcut it already uses. Defaults per DAW, every key editable by the user.
#include <juce_core/juce_core.h>

namespace SolutionVoice
{
    struct TransportKeys { juce::String play, stop, record, start, loop; };

    // Best-known defaults for the DAW we're running in (hostDescription from PluginHostType).
    TransportKeys defaultTransportKeys (const juce::String& hostDescription);

    // Parses "Ctrl+Shift+R", "Space", "F9", "NumpadMultiply", "Home", "Enter"... and presses it in the
    // window that owns nativeWindow (the plugin's editor). Returns false if unsupported here.
    bool pressHostShortcut (const juce::String& keySpec, void* nativeWindow, juce::String& error);
}
