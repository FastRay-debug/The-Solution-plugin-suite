#include "RemoteControl.h"

namespace SolutionDSP
{
namespace
{
    juce::String statusText (int s)
    {
        switch (s) { case 200: return "OK"; case 400: return "Bad Request"; case 401: return "Unauthorized"; case 403: return "Forbidden";
                     case 404: return "Not Found"; case 413: return "Payload Too Large"; case 429: return "Too Many Requests"; default: return "Error"; }
    }

    // OS-backed randomness (RtlGenRandom on Windows, arc4random / urandom elsewhere) for codes and tokens.
    juce::uint32 secureRandom()
    {
        static std::random_device rd;
        return (juce::uint32) rd();
    }

    juce::String randomHex (int bytes)
    {
        juce::String s;
        for (int i = 0; i < bytes; ++i) s << juce::String::toHexString ((int) (secureRandom() & 0xff)).paddedLeft ('0', 2);
        return s;
    }
}

juce::String RemoteServer::findLanAddress()
{
    juce::String fallback;
    for (const auto& a : juce::IPAddress::getAllAddresses (false))
    {
        const auto s = a.toString();
        if (s.startsWith ("127.") || s.startsWith ("169.254.")) continue;
        if (s.startsWith ("192.168.") || s.startsWith ("10.") || s.startsWith ("172.")) return s;
        if (fallback.isEmpty()) fallback = s;
    }
    return fallback.isNotEmpty() ? fallback : juce::String ("127.0.0.1");
}

bool RemoteServer::start (int firstPort, int numPorts, const juce::String& bindAddress)
{
    stop();
    for (int p = firstPort; p < firstPort + numPorts; ++p)
        if (listener.createListener (p, bindAddress)) { port = p; break; }
    if (port == 0) return false;

    code = juce::String (100000 + (int) (secureRandom() % 900000u));
    tokens.clear(); pairedCount = 0; failedAttempts = 0; lockedUntilMs = 0;
    startThread();
    return true;
}

void RemoteServer::stop()
{
    signalThreadShouldExit();
    listener.close();                // unblocks waitForNextConnection
    stopThread (2000);
    port = 0;
    tokens.clear();
    pairedCount = 0;
}

juce::String RemoteServer::getUrl() const
{
    return "http://" + findLanAddress() + ":" + juce::String (port) + "/";
}

void RemoteServer::run()
{
    while (! threadShouldExit())
    {
        std::unique_ptr<juce::StreamingSocket> client (listener.waitForNextConnection());
        if (client == nullptr || threadShouldExit()) continue;
        handle (*client);
        client->close();
    }
}

void RemoteServer::respond (juce::StreamingSocket& s, int status, const juce::String& contentType, const juce::String& body)
{
    const auto bytes = body.toUTF8();
    const auto len = (int) std::strlen (bytes);
    juce::String head;
    head << "HTTP/1.1 " << status << " " << statusText (status) << "\r\n"
         << "Content-Type: " << contentType << "\r\nContent-Length: " << len << "\r\n"
         << "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n";
    s.write (head.toRawUTF8(), (int) head.getNumBytesAsUTF8());
    if (len > 0) s.write (bytes, len);
}

// "Lingering close": after refusing a request, swallow what the client is still sending (bounded:
// 64 KB / 300 ms) so closing doesn't reset the connection and throw away our error reply.
static void drainBriefly (juce::StreamingSocket& s)
{
    char buf[4096]; int total = 0;
    const auto until = juce::Time::getMillisecondCounter() + 300;
    while (total < 65536 && juce::Time::getMillisecondCounter() < until && s.waitUntilReady (true, 50) > 0)
    {
        const int n = s.read (buf, sizeof (buf), false);
        if (n <= 0) break;
        total += n;
    }
}

void RemoteServer::handle (juce::StreamingSocket& s)
{
    // Read the request (headers + small body), at most 16 KB, with a short timeout.
    // Skin uploads from the phone's Skin Studio may be up to 3 MB.
    juce::MemoryBlock data;
    char buf[8192];
    int headerEnd = -1, contentLength = 0;
    size_t maxRequest = 16384;
    while (data.getSize() < maxRequest)
    {
        if (s.waitUntilReady (true, 2000) <= 0) break;
        const int n = s.read (buf, sizeof (buf), false);
        if (n <= 0) break;
        data.append (buf, (size_t) n);
        if (headerEnd >= 0) { if ((int) data.getSize() >= headerEnd + 4 + contentLength) break; continue; }   // body: just count bytes
        const auto text = data.toString();
        if ((headerEnd = text.indexOf ("\r\n\r\n")) >= 0)
        {
            const auto lower = text.substring (0, headerEnd).toLowerCase();
            const int cl = lower.indexOf ("content-length:");
            contentLength = cl >= 0 ? lower.substring (cl + 15).upToFirstOccurrenceOf ("\r\n", false, false).trim().getIntValue() : 0;
            const bool skinUpload = text.startsWith ("POST /api/skin ");
            if (skinUpload) maxRequest = (size_t) 3 * 1024 * 1024 + 16384;
            if (contentLength > (skinUpload ? 3 * 1024 * 1024 : 8192)) { respond (s, 413, "text/plain", "too large"); drainBriefly (s); return; }
        }
        if (headerEnd >= 0 && (int) data.getSize() >= headerEnd + 4 + contentLength) break;
    }
    if (data.getSize() >= maxRequest) { respond (s, 413, "text/plain", "too large"); drainBriefly (s); return; }
    const auto text = data.toString();
    if (headerEnd < 0) { respond (s, 400, "text/plain", "bad request"); return; }

    const auto requestLine = text.upToFirstOccurrenceOf ("\r\n", false, false);
    const auto method = requestLine.upToFirstOccurrenceOf (" ", false, false);
    const auto path = requestLine.fromFirstOccurrenceOf (" ", false, false).upToFirstOccurrenceOf (" ", false, false).upToFirstOccurrenceOf ("?", false, false);
    const auto headers = text.substring (0, headerEnd);
    const auto body = text.substring (headerEnd + 4, headerEnd + 4 + contentLength);

    juce::String token;
    for (const auto& line : juce::StringArray::fromLines (headers))
        if (line.startsWithIgnoreCase ("Authorization: Bearer "))
            token = line.fromFirstOccurrenceOf ("Bearer ", false, false).trim();
    const bool authorised = token.length() == 32 && tokens.contains (token);

    if (method == "GET" && (path == "/" || path == "/index.html"))
        return respond (s, 200, "text/html; charset=utf-8", host.getPage ? host.getPage() : juce::String ("<h1>The Solution</h1>"));

    if (method == "POST" && path == "/api/pair")
    {
        const auto now = juce::Time::getMillisecondCounter();
        if (now < lockedUntilMs) return respond (s, 429, "application/json", "{\"error\":\"Too many wrong codes - wait a minute\"}");
        const auto json = juce::JSON::parse (body);
        if (json.getProperty ("code", {}).toString() != code)
        {
            if (++failedAttempts >= 5) { lockedUntilMs = now + 60000; failedAttempts = 0; }
            return respond (s, 403, "application/json", "{\"error\":\"Wrong code\"}");
        }
        failedAttempts = 0;
        const auto newToken = randomHex (16);
        tokens.add (newToken);
        if (tokens.size() > 8) tokens.remove (0);
        pairedCount = tokens.size();
        return respond (s, 200, "application/json", "{\"token\":\"" + newToken + "\"}");
    }

    if (! authorised)
        return respond (s, 401, "application/json", "{\"error\":\"Pair with the code shown in the plugin first\"}");

    if (method == "GET" && path == "/api/state")
        return respond (s, 200, "application/json", juce::JSON::toString (host.getState ? host.getState() : juce::var(), true));

    if (method == "POST" && path == "/api/param")
    {
        const auto json = juce::JSON::parse (body);
        const auto id = json.getProperty ("id", {}).toString();
        const float value = (float) json.getProperty ("value", 0.0);
        if (id.isEmpty() || ! std::isfinite (value)) return respond (s, 400, "application/json", "{\"error\":\"id and value needed\"}");
        if (host.setParam && host.runOnMessageThread)
            host.runOnMessageThread ([fn = host.setParam, id, value] { fn (id, value); });
        return respond (s, 200, "application/json", "{\"ok\":true}");
    }

    if (method == "POST" && path == "/api/head")
    {
        const auto json = juce::JSON::parse (body);
        const float yaw = (float) json.getProperty ("yaw", 0.0), pitch = (float) json.getProperty ("pitch", 0.0);
        if (! std::isfinite (yaw) || ! std::isfinite (pitch)) return respond (s, 400, "application/json", "{\"error\":\"yaw and pitch needed\"}");
        if (host.setHead) host.setHead (yaw, pitch);
        return respond (s, 200, "application/json", "{\"ok\":true}");
    }

    if (method == "POST" && path == "/api/skin")      // Skin Studio: {name, jpeg (base64), accent, accent2}
    {
        const auto json = juce::JSON::parse (body);
        const auto name = json.getProperty ("name", {}).toString().toLowerCase().retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789_-").substring (0, 32);
        juce::MemoryOutputStream jpeg;
        if (name.isEmpty() || ! juce::Base64::convertFromBase64 (jpeg, json.getProperty ("jpeg", {}).toString()) || jpeg.getDataSize() < 200)
            return respond (s, 400, "application/json", "{\"error\":\"name and photo needed\"}");
        const auto* d = static_cast<const juce::uint8*> (jpeg.getData());
        if (! (d[0] == 0xFF && d[1] == 0xD8)) return respond (s, 400, "application/json", "{\"error\":\"photo must be a JPEG\"}");
        if (host.saveSkin && host.runOnMessageThread)
        {
            auto block = std::make_shared<juce::MemoryBlock> (jpeg.getData(), jpeg.getDataSize());
            const auto accent = json.getProperty ("accent", "#F59E0B").toString(), accent2 = json.getProperty ("accent2", "#38BDF8").toString();
            host.runOnMessageThread ([fn = host.saveSkin, name, block, accent, accent2] { fn (name, *block, accent, accent2); });
        }
        return respond (s, 200, "application/json", "{\"ok\":true,\"skin\":\"custom_" + name + "\"}");
    }

    if (method == "POST" && path == "/api/preset")
    {
        const auto name = juce::JSON::parse (body).getProperty ("name", {}).toString();
        if (name.isNotEmpty() && host.loadPreset && host.runOnMessageThread)
            host.runOnMessageThread ([fn = host.loadPreset, name] { fn (name); });
        return respond (s, 200, "application/json", "{\"ok\":true}");
    }

    respond (s, 404, "text/plain", "not found");
}
}
