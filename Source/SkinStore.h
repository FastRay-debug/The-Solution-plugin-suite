#pragma once
// Skins on disk, shared by the plugins (Skin Studio on the phone) and the Skin Editor program.
//  - Bundled skins: <shared Skins folder>/<key>.jpg (+ optional skins.json with names and accents)
//  - Your skins:    <user Skins folder>/custom_<name>.jpg, listed in custom_skins.json
//  - Skin packs:    .solutionskin = zip of the jpg(s) + a skins.json, to share or back up
#include <juce_core/juce_core.h>
#include "Vendor.h"

namespace SolutionSkins
{
    inline juce::File userDir() { return SolutionVendor::userFolder ("Skins"); }

    inline juce::String safeName (const juce::String& n) { return n.toLowerCase().replaceCharacter (' ', '_').retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789_-").substring (0, 40); }
    inline juce::String safeColour (const juce::String& c, const char* fallback)
    { return c.matchesWildcard ("#??????", true) && c.substring (1).containsOnly ("0123456789abcdefABCDEF") ? c : juce::String (fallback); }

    inline juce::var readList (const juce::File& dir) { auto v = juce::JSON::parse (dir.getChildFile ("custom_skins.json")); return v.isArray() ? v : juce::var (juce::Array<juce::var>()); }
    inline void writeList (const juce::File& dir, const juce::Array<juce::var>& list) { dir.getChildFile ("custom_skins.json").replaceWithText (juce::JSON::toString (juce::var (list))); }

    // Save (or replace) one of your skins. Returns its key ("custom_<name>").
    inline juce::String save (const juce::File& dir, const juce::String& displayName, const void* jpeg, size_t bytes, const juce::String& accent, const juce::String& accent2,
                              const juce::String& fileName = {})
    {
        const auto name = safeName (fileName.isNotEmpty() ? fileName : displayName);
        if (name.isEmpty() || bytes < 200) return {};
        dir.createDirectory();
        const juce::String key = "custom_" + name;
        dir.getChildFile (key + ".jpg").replaceWithData (jpeg, bytes);
        const auto current = readList (dir); juce::Array<juce::var> list; if (auto* a = current.getArray()) list = *a;
        for (int i = list.size(); --i >= 0;) if (list[i]["key"].toString() == key) list.remove (i);
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("key", key); o->setProperty ("name", displayName.substring (0, 40));
        o->setProperty ("accent", safeColour (accent, "#F59E0B")); o->setProperty ("accent2", safeColour (accent2, "#38BDF8"));
        list.insert (0, juce::var (o.get()));
        writeList (dir, list);
        return key;
    }
    inline bool remove (const juce::File& dir, const juce::String& key)
    {
        if (! key.startsWith ("custom_") || ! key.containsOnly ("abcdefghijklmnopqrstuvwxyz0123456789_-")) return false;
        const auto current = readList (dir); juce::Array<juce::var> list; if (auto* a = current.getArray()) list = *a;
        for (int i = list.size(); --i >= 0;) if (list[i]["key"].toString() == key) list.remove (i);
        writeList (dir, list);
        return dir.getChildFile (key + ".jpg").deleteFile();
    }
    inline bool rename (const juce::File& dir, const juce::String& key, const juce::String& newName)
    {
        const auto current = readList (dir); juce::Array<juce::var> list; if (auto* a = current.getArray()) list = *a;
        for (auto& v : list) if (v["key"].toString() == key) { if (auto* o = v.getDynamicObject()) o->setProperty ("name", newName.substring (0, 40)); writeList (dir, list); return true; }
        return false;
    }

    // Pack: zip with the jpg files and their skins.json entries.
    inline bool exportPack (const juce::File& dir, const juce::StringArray& keys, const juce::File& out)
    {
        juce::ZipFile::Builder zip; juce::Array<juce::var> meta;
        const auto current = readList (dir);
        if (auto* a = current.getArray())
            for (auto& v : *a) if (keys.contains (v["key"].toString())) { zip.addFile (dir.getChildFile (v["key"].toString() + ".jpg"), 6); meta.add (v); }
        if (meta.isEmpty()) return false;
        const auto tmp = juce::File::createTempFile ("json"); tmp.replaceWithText (juce::JSON::toString (juce::var (meta)));
        zip.addFile (tmp, 6, "skins.json");
        out.deleteFile(); juce::FileOutputStream os (out); if (! os.openedOk()) return false;
        const bool ok = zip.writeToStream (os, nullptr); tmp.deleteFile(); return ok;
    }
    inline int importPack (const juce::File& dir, const juce::File& pack)
    {
        juce::ZipFile zip (pack); const int idx = zip.getIndexOfFileName ("skins.json");
        if (idx < 0) return 0;
        std::unique_ptr<juce::InputStream> js (zip.createStreamForEntry (idx));
        const auto meta = juce::JSON::parse (js->readEntireStreamAsString());
        int n = 0;
        if (auto* a = meta.getArray())
            for (auto& v : *a)
            {
                const auto key = v["key"].toString(); const int e = zip.getIndexOfFileName (key + ".jpg");
                if (e < 0 || ! key.startsWith ("custom_")) continue;
                std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (e)); juce::MemoryBlock mb; in->readIntoMemoryBlock (mb);
                const auto file = key.fromFirstOccurrenceOf ("custom_", false, false);
                if (save (dir, v["name"].toString().isNotEmpty() ? v["name"].toString() : file, mb.getData(), mb.getSize(), v["accent"].toString(), v["accent2"].toString(), file).isNotEmpty()) ++n;
            }
        return n;
    }
}
