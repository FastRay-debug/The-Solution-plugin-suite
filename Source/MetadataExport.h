#pragma once
// Smart Metadata Embedding: the recorded 3D trajectories of the session's objects, written as
//  - ADM (ITU-R BS.2076) XML in an 'axml' chunk (+ 'chna' channel mapping for a stem),
//  - an 'iXML' chunk with a readable summary,
// into a copy of a bounced WAV, or as JSON / ADM XML sidecar files.
#include "CinemaTools.h"
#include <juce_core/juce_core.h>

namespace SolutionMeta
{
    struct Track { juce::String name; std::map<long long, SolutionCinema::Pos> points; };

    inline juce::String admTime (double s)
    {
        const int h = (int) (s / 3600), m = (int) (std::fmod (s, 3600.0) / 60);
        const double sec = std::fmod (s, 60.0);
        return juce::String::formatted ("%02d:%02d:%08.5f", h, m, sec);
    }

    // Object-based ADM. Our azimuth is + to the right; ADM's is + to the left, so it is negated.
    inline juce::String buildAdm (const std::vector<Track>& tracks, double durationSec, int channelsPerObject = 1)
    {
        auto esc = [] (const juce::String& s) { return s.replace ("&", "&amp;").replace ("<", "&lt;").replace (">", "&gt;").replace ("\"", "&quot;"); };
        juce::String x;
        x << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<ebuCoreMain xmlns=\"urn:ebu:metadata-schema:ebuCore_2015\"><coreMetadata><format><audioFormatExtended version=\"ITU-R_BS.2076-2\">\n";
        x << "<audioProgramme audioProgrammeID=\"APR_1001\" audioProgrammeName=\"Billet Audio session\" start=\"00:00:00.00000\" end=\"" << admTime (durationSec) << "\">"
          << "<audioContentIDRef>ACO_1001</audioContentIDRef></audioProgramme>\n";
        x << "<audioContent audioContentID=\"ACO_1001\" audioContentName=\"Objects\">";
        for (size_t i = 0; i < tracks.size(); ++i) x << "<audioObjectIDRef>AO_" << juce::String::toHexString (0x1001 + (int) i).toUpperCase().paddedLeft ('0', 4) << "</audioObjectIDRef>";
        x << "</audioContent>\n";
        int trackUid = 1;
        for (size_t i = 0; i < tracks.size(); ++i)
        {
            const auto id = juce::String::toHexString (0x1001 + (int) i).toUpperCase().paddedLeft ('0', 4);
            x << "<audioObject audioObjectID=\"AO_" << id << "\" audioObjectName=\"" << esc (tracks[i].name) << "\" start=\"00:00:00.00000\" duration=\"" << admTime (durationSec) << "\">"
              << "<audioPackFormatIDRef>AP_0003" << id << "</audioPackFormatIDRef>";
            for (int c = 0; c < channelsPerObject; ++c) x << "<audioTrackUIDRef>ATU_" << juce::String (trackUid + c).paddedLeft ('0', 8) << "</audioTrackUIDRef>";
            x << "</audioObject>\n";
            x << "<audioPackFormat audioPackFormatID=\"AP_0003" << id << "\" audioPackFormatName=\"" << esc (tracks[i].name) << "\" typeLabel=\"0003\" typeDefinition=\"Objects\">"
              << "<audioChannelFormatIDRef>AC_0003" << id << "</audioChannelFormatIDRef></audioPackFormat>\n";
            x << "<audioChannelFormat audioChannelFormatID=\"AC_0003" << id << "\" audioChannelFormatName=\"" << esc (tracks[i].name) << "\" typeLabel=\"0003\" typeDefinition=\"Objects\">\n";
            // one block per change (consecutive identical points merge)
            const auto& pts = tracks[i].points;
            int block = 1;
            auto it = pts.begin();
            while (it != pts.end())
            {
                auto next = std::next (it);
                while (next != pts.end() && std::abs (next->second.az - it->second.az) < 0.5f && std::abs (next->second.el - it->second.el) < 0.5f
                       && std::abs (next->second.dist - it->second.dist) < 0.02f && std::abs (next->second.width - it->second.width) < 0.01f) ++next;
                const double t0 = it->first / SolutionCinema::Trajectory::rate, t1 = next != pts.end() ? next->first / SolutionCinema::Trajectory::rate : durationSec;
                const auto& p = it->second;
                x << "  <audioBlockFormat audioBlockFormatID=\"AB_0003" << id << "_" << juce::String::toHexString (block++).paddedLeft ('0', 8) << "\" rtime=\"" << admTime (t0)
                  << "\" duration=\"" << admTime (std::max (0.0, t1 - t0)) << "\">"
                  << "<position coordinate=\"azimuth\">" << juce::String (-p.az, 2) << "</position>"
                  << "<position coordinate=\"elevation\">" << juce::String (p.el, 2) << "</position>"
                  << "<position coordinate=\"distance\">" << juce::String (juce::jlimit (0.0f, 1.0f, p.dist / 10.0f), 3) << "</position>"
                  << "<width>" << juce::String (juce::jlimit (0.0f, 360.0f, p.width * 60.0f), 1) << "</width></audioBlockFormat>\n";
                it = next;
            }
            x << "</audioChannelFormat>\n";
            for (int c = 0; c < channelsPerObject; ++c)
                x << "<audioTrackUID UID=\"ATU_" << juce::String (trackUid + c).paddedLeft ('0', 8) << "\"><audioPackFormatIDRef>AP_0003" << id << "</audioPackFormatIDRef></audioTrackUID>\n";
            trackUid += channelsPerObject;
        }
        x << "</audioFormatExtended></format></coreMetadata></ebuCoreMain>\n";
        return x;
    }

    inline juce::String buildIxml (const std::vector<Track>& tracks, const juce::String& project)
    {
        juce::String x;
        x << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<BWFXML><IXML_VERSION>1.61</IXML_VERSION><PROJECT>" << project.replace ("&", "&amp;").replace ("<", "&lt;")
          << "</PROJECT><NOTE>Billet Audio - The Solution spatial metadata (" << (int) tracks.size() << " objects, ADM in axml)</NOTE><USER>";
        for (auto& t : tracks)
        {
            if (t.points.empty()) continue;
            const auto& p = t.points.begin()->second;
            x << t.name.replace ("&", "&amp;").replace ("<", "&lt;") << ": start az " << juce::String (p.az, 1) << " el " << juce::String (p.el, 1)
              << " dist " << juce::String (p.dist, 2) << " width " << juce::String (p.width, 2) << ", " << (int) t.points.size() << " points\n";
        }
        x << "</USER></BWFXML>\n";
        return x;
    }

    inline juce::MemoryBlock buildChna (int numChannels, int channelsPerObject)
    {
        juce::MemoryOutputStream o;
        o.writeShort ((short) numChannels); o.writeShort ((short) numChannels);
        for (int c = 0; c < numChannels; ++c)
        {
            const int obj = channelsPerObject > 0 ? c / channelsPerObject : 0;
            const auto id = juce::String::toHexString (0x1001 + obj).toUpperCase().paddedLeft ('0', 4);
            o.writeShort ((short) (c + 1));
            o.write (("ATU_" + juce::String (c + 1).paddedLeft ('0', 8)).toRawUTF8(), 12);
            o.write (("AT_0003" + id + "_01").toRawUTF8(), 14);
            o.write (("AP_0003" + id).toRawUTF8(), 11);
            o.writeByte (0);
        }
        return o.getMemoryBlock();
    }

    // Copies `in` to `out`, dropping any existing axml/iXML/chna chunks and appending the new ones.
    inline bool embed (const juce::File& in, const juce::File& out, const juce::String& axml, const juce::String& ixml, const juce::MemoryBlock& chna, juce::String& error)
    {
        juce::FileInputStream src (in);
        if (! src.openedOk()) { error = "Can't open " + in.getFileName(); return false; }
        char id[5] = {};
        src.read (id, 4); const auto riffSize = (juce::uint32) src.readInt(); juce::ignoreUnused (riffSize);
        char wave[5] = {}; src.read (wave, 4);
        if (juce::String (id) != "RIFF" || juce::String (wave) != "WAVE") { error = in.getFileName() + " isn't a WAV (RIFF) file" + (juce::String (id) == "RF64" ? " - RF64 files over 4 GB aren't supported yet" : ""); return false; }
        const juce::int64 extra = (juce::int64) axml.getNumBytesAsUTF8() + ixml.getNumBytesAsUTF8() + (juce::int64) chna.getSize() + 64;
        if (in.getSize() + extra >= 0xFFFFFFFFLL) { error = "The file would pass 4 GB with the metadata"; return false; }
        out.deleteFile();
        juce::FileOutputStream dst (out);
        if (! dst.openedOk()) { error = "Can't write " + out.getFullPathName(); return false; }
        dst.write ("RIFF", 4); dst.writeInt (0); dst.write ("WAVE", 4);
        while (! src.isExhausted())
        {
            char cid[5] = {};
            if (src.read (cid, 4) != 4) break;
            const auto size = (juce::uint32) src.readInt();
            const juce::String name (cid);
            if (name == "axml" || name == "iXML" || name == "chna") { src.skipNextBytes ((juce::int64) size + (size & 1)); continue; }
            dst.write (cid, 4); dst.writeInt ((int) size);
            dst.writeFromInputStream (src, (juce::int64) size + (size & 1));
        }
        auto chunk = [&dst] (const char* cid, const void* data, size_t n) { dst.write (cid, 4); dst.writeInt ((int) n); dst.write (data, n); if (n & 1) dst.writeByte (0); };
        if (chna.getSize() > 0) chunk ("chna", chna.getData(), chna.getSize());
        chunk ("axml", axml.toRawUTF8(), axml.getNumBytesAsUTF8());
        chunk ("iXML", ixml.toRawUTF8(), ixml.getNumBytesAsUTF8());
        dst.flush();
        const auto total = dst.getPosition();
        dst.setPosition (4); dst.writeInt ((int) (total - 8)); dst.flush();
        return true;
    }

    inline juce::String toJson (const std::vector<Track>& tracks)
    {
        juce::Array<juce::var> list;
        for (auto& t : tracks)
        {
            juce::DynamicObject::Ptr o = new juce::DynamicObject(); o->setProperty ("name", t.name);
            juce::Array<juce::var> pts;
            for (auto& [k, p] : t.points) { juce::Array<juce::var> a { k / SolutionCinema::Trajectory::rate, p.az, p.el, p.dist, p.width }; pts.add (a); }
            o->setProperty ("points [t, az, el, dist, width]", pts); list.add (juce::var (o.get()));
        }
        return juce::JSON::toString (list);
    }
}
