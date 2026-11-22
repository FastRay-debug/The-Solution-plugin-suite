#pragma once
// Edit history with selective undo.
// Every change the user makes is logged once it settles (a knob drag is one entry, a preset load is one entry
// with many changes). Any entry can be reverted on its own, or the module can go back to any point while
// keeping chosen controls as they are now. Values are normalised 0..1 so the log is plugin-agnostic.
#include <juce_core/juce_core.h>
#include <map>
#include <set>
#include <vector>

namespace SolutionHistory
{
    struct Change { juce::String id; float from = 0, to = 0; };
    struct Entry  { int seq = 0; juce::int64 timeMs = 0; juce::String label; std::vector<Change> changes; };

    class EditHistory
    {
    public:
        static constexpr size_t maxEntries = 300;
        static constexpr float eps = 1.0e-5f;

        // Forget the baseline (after a session load): the next poll adopts the current values silently.
        void rebaseline() { needBaseline = true; }

        // Call regularly with every parameter's normalised value. A control is logged once it has stopped
        // moving for one poll. isUserTouch(id) says whether a person moved it (UI / MIDI / voice); while the
        // host is playing, untouched changes are automation and only move the baseline.
        // Returns true if a new entry was added.
        template <typename TouchFn>
        bool poll (const std::vector<std::pair<juce::String, float>>& values, juce::int64 nowMs, bool hostPlaying,
                   TouchFn&& isUserTouch, const juce::String& groupLabel = {})
        {
            if (needBaseline)
            {
                baseline.clear(); prev.clear();
                for (auto& [id, v] : values) { baseline[id] = v; prev[id] = v; }
                needBaseline = false; return false;
            }
            Entry e;
            for (auto& [id, v] : values)
            {
                const bool touched = isUserTouch (id);
                auto pit = prev.find (id);
                if (pit == prev.end()) { prev[id] = v; baseline[id] = v; continue; }
                const bool moving = std::abs (pit->second - v) > eps;
                pit->second = v;
                if (touched) pendingTouch.insert (id);
                if (moving) continue;                                     // wait until it settles
                auto& base = baseline[id];
                if (std::abs (base - v) <= eps) { pendingTouch.erase (id); continue; }
                const bool byUser = pendingTouch.erase (id) > 0;
                if (hostPlaying && ! byUser) { base = v; continue; }       // automation playback, not an edit
                e.changes.push_back ({ id, base, v });
                base = v;
            }
            if (e.changes.empty()) return false;
            e.seq = ++lastSeq; e.timeMs = nowMs;
            e.label = groupLabel.isNotEmpty() ? groupLabel : (e.changes.size() > 1 ? juce::String ("Multiple changes") : juce::String());
            entries.push_back (std::move (e));
            if (entries.size() > maxEntries) entries.erase (entries.begin());
            return true;
        }

        const std::vector<Entry>& getEntries() const { return entries; }
        const Entry* find (int seq) const { for (auto& e : entries) if (e.seq == seq) return &e; return nullptr; }

        // Selective undo of one entry: just those controls go back. Later edits of other controls are untouched.
        std::map<juce::String, float> revertOne (int seq) const
        {
            std::map<juce::String, float> out;
            if (auto* e = find (seq)) for (auto& c : e->changes) out[c.id] = c.from;
            return out;
        }

        // Go back to just after entry `seq` (seq 0 = before everything logged), keeping the controls in `keep`.
        // current = every control's value now. Returns only the controls that actually need to move.
        std::map<juce::String, float> restoreTo (int seq, const std::set<juce::String>& keep,
                                                 const std::map<juce::String, float>& current) const
        {
            std::map<juce::String, float> target = current;
            for (auto it = entries.rbegin(); it != entries.rend() && it->seq > seq; ++it)
                for (auto& c : it->changes)
                    if (! keep.count (c.id)) target[c.id] = c.from;
            std::map<juce::String, float> out;
            for (auto& [id, v] : target) { auto cit = current.find (id); if (cit == current.end() || std::abs (cit->second - v) > eps) out[id] = v; }
            return out;
        }

        // Controls changed after entry `seq` (so the UI can warn before a single revert overrides them).
        std::set<juce::String> changedAfter (int seq) const
        {
            std::set<juce::String> s;
            for (auto& e : entries) if (e.seq > seq) for (auto& c : e.changes) s.insert (c.id);
            return s;
        }

        void clear() { entries.clear(); }

        // Compact text form for saving with the session: one entry per line.
        juce::String toString (size_t lastN = 120) const
        {
            juce::StringArray lines;
            const size_t start = entries.size() > lastN ? entries.size() - lastN : 0;
            for (size_t i = start; i < entries.size(); ++i)
            {
                auto& e = entries[i];
                juce::StringArray ch;
                for (auto& c : e.changes) ch.add (c.id + ":" + juce::String (c.from, 6) + ":" + juce::String (c.to, 6));
                lines.add (juce::String (e.seq) + "|" + juce::String (e.timeMs) + "|"
                           + e.label.removeCharacters ("|\r\n") + "|" + ch.joinIntoString (","));
            }
            return lines.joinIntoString ("\n");
        }
        void fromString (const juce::String& s)
        {
            entries.clear();
            for (auto& line : juce::StringArray::fromLines (s))
            {
                auto f = juce::StringArray::fromTokens (line, "|", "");
                if (f.size() < 4) continue;
                Entry e; e.seq = f[0].getIntValue(); e.timeMs = f[1].getLargeIntValue(); e.label = f[2];
                for (auto& c : juce::StringArray::fromTokens (f[3], ",", ""))
                {
                    auto p = juce::StringArray::fromTokens (c, ":", "");
                    if (p.size() == 3 && p[0].isNotEmpty()) e.changes.push_back ({ p[0], p[1].getFloatValue(), p[2].getFloatValue() });
                }
                if (e.seq > 0 && ! e.changes.empty()) { lastSeq = std::max (lastSeq, e.seq); entries.push_back (std::move (e)); }
            }
            rebaseline();
        }

    private:
        std::vector<Entry> entries;
        std::map<juce::String, float> baseline, prev;
        std::set<juce::String> pendingTouch;
        int lastSeq = 0;
        bool needBaseline = true;
    };
}
