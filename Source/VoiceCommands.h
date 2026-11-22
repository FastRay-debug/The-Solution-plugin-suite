#pragma once
// The Solution - voice command parser. Turns what the recognizer heard into a command.
// Pure text processing (no audio, no OS calls) so it's fully unit-tested.
#include <juce_core/juce_core.h>
#include <vector>

namespace SolutionVoice
{
    struct ParamInfo { juce::String id, name; float min = 0, max = 1; bool isBool = false, isChoice = false, percentUnit = false; juce::StringArray choices; };
    struct Target    { juce::String phrase; int slot = -1; };      // module or track name -> Rack Link slot

    struct Command
    {
        enum Kind { None, SetParam, NudgeParam, Transport, Ui, Panic, Macro };
        Kind kind = None;
        juce::String action;        // Transport: play/stop/record/start/loop; Ui: fullscreen/diag/...; Macro: recall/store
        juce::String paramId;
        float value = 0;            // real units for SetParam; signed fraction of range for NudgeParam; number for Macro
        int slot = -1;              // -1 = the active plugin
        bool everywhere = false;    // "all", "everything", "session"
        juce::String describe;      // human feedback, e.g. "Stage Width -> 120%"
    };

    // ---------------------------------------------------------------- text helpers
    inline juce::String normalise (juce::String s)
    {
        s = s.toLowerCase().replace ("-", " ").replace ("/", " ").retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789 .%");
        while (s.contains ("  ")) s = s.replace ("  ", " ");
        return s.trim();
    }

    // "minus twenty five point five" -> "-25.5", "one hundred and twenty" -> "120"
    inline juce::StringArray wordsToNumbers (const juce::StringArray& in)
    {
        static const char* units[] = { "zero","one","two","three","four","five","six","seven","eight","nine","ten","eleven","twelve",
                                       "thirteen","fourteen","fifteen","sixteen","seventeen","eighteen","nineteen" };
        static const char* tens[]  = { "", "", "twenty","thirty","forty","fifty","sixty","seventy","eighty","ninety" };
        auto unitOf = [&] (const juce::String& w) { for (int i = 0; i < 20; ++i) if (w == units[i]) return i; if (w == "oh") return 0; return -1; };
        auto tenOf  = [&] (const juce::String& w) { for (int i = 2; i < 10; ++i) if (w == tens[i]) return i * 10; return -1; };
        juce::StringArray out;
        for (int i = 0; i < in.size();)
        {
            bool neg = false; int j = i;
            if ((in[j] == "minus" || in[j] == "negative") && j + 1 < in.size()) { neg = true; ++j; }
            double v = 0; bool any = false; int k = j;
            if (k < in.size() && in[k].containsOnly ("0123456789.") && in[k].isNotEmpty()) { v = in[k].getDoubleValue(); any = true; ++k; }
            else
            {
                double current = 0;
                while (k < in.size())
                {
                    const auto& w = in[k];
                    if (const int t = tenOf (w); t >= 0) { current += t; any = true; ++k; continue; }
                    if (const int u = unitOf (w); u >= 0) { current += u; any = true; ++k; continue; }
                    if (w == "hundred" && any) { current *= 100; ++k; continue; }
                    if (w == "and" && any && k + 1 < in.size() && (tenOf (in[k + 1]) >= 0 || unitOf (in[k + 1]) >= 0)) { ++k; continue; }
                    break;
                }
                v = current;
            }
            if (any && k + 1 < in.size() && in[k] == "point")      // decimals: "point five"
            {
                int d = 0; double frac = 0, scale = 0.1;
                while (k + 1 + d < in.size() && unitOf (in[k + 1 + d]) >= 0 && unitOf (in[k + 1 + d]) < 10) { frac += unitOf (in[k + 1 + d]) * scale; scale *= 0.1; ++d; }
                if (d > 0) { v += frac; k += 1 + d; }
            }
            if (any) { out.add (juce::String (neg ? -v : v)); i = k; }
            else     { out.add (in[i]); ++i; }
        }
        return out;
    }

    // Spoken aliases -> parameter id (checked before fuzzy name matching).
    inline const std::vector<std::pair<const char*, const char*>>& aliases()
    {
        static const std::vector<std::pair<const char*, const char*>> a {
            { "width", "stage_width" }, { "stage width", "stage_width" }, { "azimuth", "stage_azimuth" }, { "pan", "stage_azimuth" },
            { "elevation", "stage_elevation" }, { "height", "stage_elevation" }, { "depth", "stage_depth" }, { "drive", "stage_depth" },
            { "distance", "stage_distance" }, { "haas", "stage_haas_itd" }, { "anchor", "stage_anchor_hz" }, { "mid", "stage_ms_mid" }, { "side", "stage_ms_side" },
            { "mix", "global_mix" }, { "wet dry", "global_mix" }, { "dry wet", "global_mix" }, { "room mix", "room_match_mix" },
            { "infra lift", "infra_lift" }, { "infrasonic lift", "infra_lift" }, { "infra", "infra_lift" }, { "sub boost", "infra_lift" },
            { "infra lift wide", "infra_lift_wide" }, { "infrasonic wide", "infra_lift_wide" }, { "wide lift", "infra_lift_wide" },
            { "lookahead", "limiter_lookahead" }, { "look ahead", "limiter_lookahead" }, { "dc guard", "dc_guard" }, { "dc", "dc_guard" },
            { "transient shaper", "transient_sculpt" }, { "transients", "transient_sculpt" }, { "attack", "transient_attack" }, { "sustain", "transient_sustain" },
            { "duck under dialogue", "dialogue_duck" }, { "dialogue ducking", "dialogue_duck" }, { "ducking", "dialogue_duck" },
            { "dialogue", "dialogue_source" }, { "duck depth", "dialogue_duck_db" },
            { "binaural", "binaural_mode" }, { "headphones", "binaural_mode" }, { "glue", "glue_amount" }, { "smooth", "smooth_amount" },
            { "phone bass", "phone_bass" }, { "input", "trim_in_db" }, { "input trim", "trim_in_db" }, { "output", "trim_out_db" }, { "output trim", "trim_out_db" },
            { "ceiling", "stage_ceiling_guard" }, { "ceiling guard", "stage_ceiling_guard" }, { "safe bounce", "safe_bounce" },
            { "ultra wide", "ultra_wide" }, { "mono lows", "mono_lows_hz" }, { "phase guard", "phase_guard" }, { "notch", "mode_notch" },
            { "character", "char_amount" }, { "character drive", "char_drive" }, { "room correction", "room_correct_on" }, { "motion", "motion_on" },
            { "quality", "dqs_mode" }, { "stem focus", "stem_focus" }, { "unmask", "unmask_depth" }, { "creature", "stage_creature" },
        };
        return a;
    }

    // ---------------------------------------------------------------- the parser
    class Parser
    {
    public:
        std::vector<ParamInfo> params;
        std::vector<Target> targets;
        bool requireWakeWord = true;
        juce::String wakeWord { "solution" };

        // Every word the recognizer should know (its grammar is built from this).
        juce::StringArray vocabulary() const
        {
            juce::StringArray v;
            const char* fixed[] = { "solution","play","stop","pause","record","recording","rewind","start","beginning","go","to","return","zero","loop",
                "panic","reset","everything","all","session","this","track","on","off","enable","disable","turn","set","up","down","more","less",
                "increase","decrease","raise","lower","percent","db","decibels","degrees","hertz","meters","ms","minus","negative","point","hundred","and",
                "one","two","three","four","five","six","seven","eight","nine","ten","eleven","twelve","thirteen","fourteen","fifteen","sixteen",
                "seventeen","eighteen","nineteen","twenty","thirty","forty","fifty","sixty","seventy","eighty","ninety",
                "bypass","compare","original","delta","mono","check","fullscreen","full","screen","maximize","diagnostics","fly","exit","close","escape",
                "presets","vault","next","previous","preset","room","rooms","snapshot","store","save","recall","macro","beginner","mode",
                "pre","render","shortcuts","help","theme","monitoring","monitors","hub","the","a","b","utility","cancel","never","mind","undo","redo","history","midi","controllers","controller" };
            for (auto* w : fixed) v.addIfNotAlreadyThere (w);
            for (auto& a : aliases()) for (auto& w : juce::StringArray::fromTokens (a.first, " ", "")) v.addIfNotAlreadyThere (w);
            for (auto& p : params) for (auto& w : juce::StringArray::fromTokens (normalise (p.name), " ", "")) if (w.containsOnly ("abcdefghijklmnopqrstuvwxyz")) v.addIfNotAlreadyThere (w);
            for (auto& t : targets) for (auto& w : juce::StringArray::fromTokens (normalise (t.phrase), " ", "")) v.addIfNotAlreadyThere (w);
            return v;
        }

        Command parse (const juce::String& heard) const
        {
            Command c;
            auto words = wordsToNumbers (juce::StringArray::fromTokens (normalise (heard), " ", ""));
            words.removeEmptyStrings();
            if (requireWakeWord)
            {
                if (words.isEmpty() || words[0] != wakeWord) return c;
                words.remove (0);
            }
            else if (! words.isEmpty() && words[0] == wakeWord) words.remove (0);
            for (auto filler : { "please", "the", "to", "set", "turn", "at", "a" })     // "a" only as filler when not "a b"
                for (int i = words.size(); --i >= 0;)
                    if (words[i] == filler && ! (juce::String (filler) == "a" && i + 1 < words.size() && words[i + 1] == "b")) words.remove (i);
            if (words.isEmpty()) return c;
            const auto text = words.joinIntoString (" ");
            auto has = [&] (const char* w) { return words.contains (w); };
            auto starts = [&] (const char* p) { return text == p || text.startsWith (juce::String (p) + " "); };

            if (has ("cancel") || text == "never mind") return c;
            // transport
            if (text == "play" || text == "start playback" || text == "start")  return transport (c, "play", "Play");
            if (text == "stop" || text == "pause")                               return transport (c, "stop", "Stop");
            if (text == "record" || text == "start recording")                   return transport (c, "record", "Record");
            if (text == "rewind" || text == "go start" || text == "go beginning" || text == "return zero" || text == "beginning") return transport (c, "start", "Go to start");
            if (text == "loop")                                                  return transport (c, "loop", "Loop on/off");
            // session-wide
            if (has ("panic") || starts ("reset everything"))
            { c.kind = Command::Panic; c.everywhere = has ("everything") || has ("all") || has ("session"); c.describe = c.everywhere ? "Panic reset: whole session" : "Panic reset: this track"; return c; }
            if ((has ("monitoring") || has ("monitors")) && has ("off")) return ui (c, "monitorsOff", "Monitoring tools off everywhere");
            // macros / snapshots
            if ((has ("snapshot") || has ("store") || has ("save") || has ("recall") || has ("macro")) && firstNumber (words) >= 1 && firstNumber (words) <= 20)
            {
                c.kind = Command::Macro; c.value = (float) firstNumber (words);
                c.action = (has ("store") || has ("save") || has ("snapshot")) && ! has ("recall") ? "store" : "recall";
                c.describe = (c.action == "store" ? "Stored snapshot " : "Recalled snapshot ") + juce::String ((int) c.value); return c;
            }
            // one-word UI actions
            if (text == "a b" || text == "compare" || text == "original")      return ui (c, "ab", "A/B compare");
            if (text == "delta")                                               return ui (c, "delta", "Delta (hear what changed)");
            if (text == "mono check" || text == "mono")                        return ui (c, "mono", "Mono check");
            if (has ("fullscreen") || text == "full screen" || text == "maximize") return ui (c, "fullscreen", "Fullscreen");
            if (has ("diagnostics") || text == "utility")                      return ui (c, "diag", "Diagnostics");
            if (text == "fly" || text == "fly mode")                           return ui (c, "fly", "Fly mode");
            if (text == "exit" || text == "close" || text == "escape")         return ui (c, "close", "Close");
            if (text == "presets" || text == "vault")                          return ui (c, "presets", "Presets");
            if (text == "next preset")                                         return ui (c, "nextPreset", "Next preset");
            if (text == "previous preset")                                     return ui (c, "prevPreset", "Previous preset");
            if (text == "next room")                                           return ui (c, "nextRoom", "Next listening room");
            if (text == "previous room")                                       return ui (c, "prevRoom", "Previous listening room");
            if (text == "rooms")                                               return ui (c, "rooms", "Listening rooms");
            if (text == "next theme" || text == "theme")                       return ui (c, "nextTheme", "Next theme");
            if (text == "previous theme")                                      return ui (c, "prevTheme", "Previous theme");
            if (has ("beginner"))                                              return ui (c, "beginner", "Beginner mode");
            if (text == "pre render" || text == "pre render check" || text == "check") return ui (c, "prerender", "Pre-render check");
            if (text == "shortcuts" || text == "help")                         return ui (c, "help", "Shortcuts & voice commands");
            if (text == "hub")                                                 return ui (c, "hub", "Command hub");
            if (text == "undo")                                                return ui (c, "undo", "Undo");
            if (text == "redo")                                                return ui (c, "redo", "Redo");
            if (has ("history"))                                               return ui (c, "history", "History (selective undo)");
            if (has ("midi") || has ("controllers") || has ("controller"))     return ui (c, "midi", "MIDI controllers");

            // target: a module or track name at the start ("bunker ...", "lead vocal ...")
            int bestLen = 0;
            for (auto& t : targets)
            {
                const auto tw = juce::StringArray::fromTokens (normalise (t.phrase), " ", "");
                if (tw.size() > bestLen && tw.size() <= words.size())
                {
                    bool match = true; for (int i = 0; i < tw.size(); ++i) match = match && words[i] == tw[i];
                    if (match) { bestLen = tw.size(); c.slot = t.slot; }
                }
            }
            if (bestLen > 0) words.removeRange (0, bestLen);
            if (has ("this") && has ("track")) { words.removeString ("this"); words.removeString ("track"); }
            if (words.isEmpty()) return {};
            if (words.size() == 1 && (words[0] == "bypass")) return ui (c, "ab", "A/B compare");

            // parameter commands
            const bool on = words.contains ("on") || words.contains ("enable"), off = words.contains ("off") || words.contains ("disable");
            const bool upWord = words.contains ("up") || words.contains ("more") || words.contains ("increase") || words.contains ("raise");
            const bool downWord = words.contains ("down") || words.contains ("less") || words.contains ("decrease") || words.contains ("lower");
            const auto* p = findParam (words);
            if (p == nullptr) return {};
            if (p->isBool && hasNumber (words))           // "infra lift 8 db" -> its amount control, not the switch
                if (const auto* amount = amountFor (p->id)) p = amount;
            c.paramId = p->id;
            if (p->isBool || ((on || off) && ! hasNumber (words)))
            {
                if (! on && ! off) { c.kind = Command::SetParam; c.value = -1; c.describe = p->name + " toggled"; return c; }   // -1 = toggle
                c.kind = Command::SetParam; c.value = on ? p->max : p->min; c.describe = p->name + (on ? " on" : " off"); return c;
            }
            if (hasNumber (words))
            {
                double v = firstNumber (words);
                const bool pct = words.contains ("percent") || words.contains ("%");
                if (p->isChoice) { const int idx = juce::jlimit (0, juce::jmax (0, p->choices.size() - 1), (int) v - 1); c.kind = Command::SetParam; c.value = (float) idx; c.describe = p->name + " -> " + p->choices[idx]; return c; }
                if (p->percentUnit || (pct && p->max <= 2.0f)) v /= 100.0;     // "width 120 percent" -> 1.2
                c.kind = Command::SetParam; c.value = (float) juce::jlimit ((double) p->min, (double) p->max, v);
                c.describe = p->name + " -> " + juce::String (c.value, (std::abs (c.value) < 10 ? 2 : 1)); return c;
            }
            if (upWord || downWord) { c.kind = Command::NudgeParam; c.value = upWord ? 0.1f : -0.1f; c.describe = p->name + (upWord ? " up" : " down"); return c; }
            if (p->isChoice || p->isBool) { c.kind = Command::SetParam; c.value = -1; c.describe = p->name + " toggled"; return c; }
            return {};
        }

    private:
        static Command& transport (Command& c, const char* a, const char* d) { c.kind = Command::Transport; c.action = a; c.describe = d; return c; }
        static Command& ui (Command& c, const char* a, const char* d) { c.kind = Command::Ui; c.action = a; c.describe = d; return c; }
        static bool hasNumber (const juce::StringArray& w) { for (auto& x : w) if (x.isNotEmpty() && x.containsOnly ("-0123456789.") && x != "-" && x != ".") return true; return false; }
        static double firstNumber (const juce::StringArray& w) { for (auto& x : w) if (x.isNotEmpty() && x.containsOnly ("-0123456789.") && x != "-" && x != ".") return x.getDoubleValue(); return -1e9; }

        const ParamInfo* amountFor (const juce::String& switchId) const
        {
            static const std::pair<const char*, const char*> pairs[] = {
                { "infra_lift", "infra_lift_db" }, { "dialogue_duck", "dialogue_duck_db" }, { "dialogue_source", "dialogue_duck_db" },
                { "room_correct_on", "room_correct_amount" }, { "motion_on", "motion_rate" }, { "transient_sculpt", "transient_attack" } };
            for (auto& pr : pairs) if (switchId == pr.first) for (auto& p : params) if (p.id == pr.second) return &p;
            return nullptr;
        }

        const ParamInfo* findParam (const juce::StringArray& words) const
        {
            juce::StringArray content;
            for (auto& w : words)
                if (! w.containsOnly ("-0123456789.") && ! juce::StringArray { "on","off","enable","disable","up","down","more","less","increase","decrease","raise","lower","percent","db","decibels","degrees","hertz","meters","ms","%" }.contains (w))
                    content.add (w);
            const auto phrase = content.joinIntoString (" ");
            if (phrase.isEmpty()) return nullptr;
            // longest alias that matches the start of what's left wins
            const char* best = nullptr; int bestLen = 0;
            for (auto& a : aliases())
            {
                const juce::String al (a.first);
                if ((phrase == al || phrase.startsWith (al + " ")) && al.length() > bestLen) { best = a.second; bestLen = al.length(); }
            }
            if (best != nullptr) for (auto& p : params) if (p.id == best) return &p;
            // otherwise the parameter whose name shares the most words
            const ParamInfo* found = nullptr; int bestScore = 0;
            for (auto& p : params)
            {
                const auto pw = juce::StringArray::fromTokens (normalise (p.name), " ", "");
                int score = 0; for (auto& w : content) if (w.length() > 2 && pw.contains (w)) ++score;
                if (score > bestScore) { bestScore = score; found = &p; }
            }
            return found;
        }
    };
}
