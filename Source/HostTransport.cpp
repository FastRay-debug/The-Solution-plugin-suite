#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif
#include "HostTransport.h"
#include <vector>

namespace SolutionVoice
{
TransportKeys defaultTransportKeys (const juce::String& host)
{
    const auto h = host.toLowerCase();
    // play/stop are both the play-stop toggle in most DAWs; the voice command checks whether the
    // host is already playing before pressing it, so "stop" never starts playback.
    if (h.contains ("ableton") || h.contains ("live"))  return { "Space", "Space", "F9", "Home", "Ctrl+L" };
    if (h.contains ("fl studio") || h.contains ("fruity")) return { "Space", "Space", "R", "Home", "" };
    if (h.contains ("reaper"))                          return { "Space", "Space", "Ctrl+R", "Home", "R" };
    if (h.contains ("cubase") || h.contains ("nuendo") || h.contains ("steinberg")) return { "Space", "Space", "NumpadMultiply", "NumpadDecimal", "NumpadDivide" };
    if (h.contains ("studio one") || h.contains ("presonus")) return { "Space", "Space", "NumpadMultiply", "NumpadComma", "NumpadDivide" };
    if (h.contains ("pro tools"))                       return { "Space", "Space", "Ctrl+Space", "Enter", "Ctrl+Shift+L" };
    if (h.contains ("bitwig"))                          return { "Space", "Space", "Shift+Space", "Home", "Ctrl+L" };
    return { "Space", "Space", "R", "Home", "L" };
}

#if defined (_WIN32)
namespace
{
    WORD vkFor (const juce::String& k)
    {
        const auto u = k.toUpperCase();
        if (u.length() == 1) { const auto c = u[0]; if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return (WORD) c; }
        if (u.startsWith ("F") && u.substring (1).containsOnly ("0123456789")) { const int n = u.substring (1).getIntValue(); if (n >= 1 && n <= 24) return (WORD) (VK_F1 + n - 1); }
        struct { const char* n; WORD vk; } map[] = {
            { "SPACE", VK_SPACE }, { "ENTER", VK_RETURN }, { "RETURN", VK_RETURN }, { "HOME", VK_HOME }, { "END", VK_END }, { "TAB", VK_TAB },
            { "ESC", VK_ESCAPE }, { "ESCAPE", VK_ESCAPE }, { "NUMPADMULTIPLY", VK_MULTIPLY }, { "NUMPADDIVIDE", VK_DIVIDE }, { "NUMPADDECIMAL", VK_DECIMAL },
            { "NUMPADCOMMA", VK_SEPARATOR }, { "NUMPADADD", VK_ADD }, { "NUMPADSUBTRACT", VK_SUBTRACT }, { "NUMPADENTER", VK_RETURN },
            { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT }, { "UP", VK_UP }, { "DOWN", VK_DOWN }, { "PERIOD", VK_OEM_PERIOD }, { "COMMA", VK_OEM_COMMA } };
        for (auto& m : map) if (u == m.n) return m.vk;
        if (u.startsWith ("NUMPAD") && u.length() == 7 && juce::CharacterFunctions::isDigit (u[6])) return (WORD) (VK_NUMPAD0 + (u[6] - '0'));
        return 0;
    }
}

bool pressHostShortcut (const juce::String& keySpec, void* nativeWindow, juce::String& error)
{
    if (keySpec.trim().isEmpty()) { error = "No shortcut set for that in your DAW (secret menu > Voice)."; return false; }
    auto parts = juce::StringArray::fromTokens (keySpec, "+", ""); parts.trim(); parts.removeEmptyStrings();
    std::vector<WORD> mods; WORD key = 0;
    for (auto& p : parts)
    {
        const auto u = p.toUpperCase();
        if (u == "CTRL" || u == "CONTROL") mods.push_back (VK_CONTROL);
        else if (u == "SHIFT") mods.push_back (VK_SHIFT);
        else if (u == "ALT") mods.push_back (VK_MENU);
        else key = vkFor (p);
    }
    if (key == 0) { error = "Don't know the key '" + keySpec + "'."; return false; }
    // Bring the DAW's main window (the owner of the plugin window) to the front so it gets the key.
    if (auto hwnd = (HWND) nativeWindow)
        if (auto root = GetAncestor (hwnd, GA_ROOTOWNER)) SetForegroundWindow (root);
    std::vector<INPUT> in;
    auto add = [&] (WORD vk, bool up) { INPUT i {}; i.type = INPUT_KEYBOARD; i.ki.wVk = vk; i.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0; in.push_back (i); };
    for (auto m : mods) add (m, false);
    add (key, false); add (key, true);
    for (auto it = mods.rbegin(); it != mods.rend(); ++it) add (*it, true);
    if (SendInput ((UINT) in.size(), in.data(), sizeof (INPUT)) != (UINT) in.size()) { error = "Windows blocked the key press."; return false; }
    return true;
}
#else
bool pressHostShortcut (const juce::String&, void*, juce::String& error) { error = "Voice transport control is Windows-only for now."; return false; }
#endif
}
