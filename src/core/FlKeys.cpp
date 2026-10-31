#include "core/FlKeys.h"
#include "core/HostSlide.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>

#include "core/FlVoiceProcessor.h"

namespace lynx
{
namespace flkeys
{
// EXEC (round 69): minimum and maximum of each knob as the plugin's wheel controls hold them
const KnobRange knobRange[15] = {
    { -1000, 1000, "Decay" },      { -1000, 1000, "Release" },  { -1000, 1000, "Hardness" },
    { -1000, 1000, "Vel > Hard" }, { 0, 1000, "Muffle" },       { 0, 2000, "Vel > Muffle" },
    { -1000, 1000, "Vel sens" },   { 0, 2000, "Stereo" },       { -1000, 1000, "Tune" },
    { 0, 500, "Detune" },          { -500, 500, "Stretch" },    { -1000, 1000, "Treble" },
    { -1000, 1000, "Pan/Trem" },   { 0, 65536, "LFO rate" },    { 0, 1000, "Overdrive" },
};

// ------------------------------------------------------------------------------------------------------- state
std::optional<State> parseState (const std::vector<std::uint8_t>& d)
{
    // SaveRestoreState (FUN_0071f860): u32 version (1 is the only one written; FL reads states below 2), the
    // wavetable name (u8 length + bytes), then the 15 knobs as int32
    if (d.size() < 5)
        return std::nullopt;
    std::uint32_t version = 0;
    std::memcpy (&version, d.data(), 4);
    if (version >= 2)
        return std::nullopt;
    const std::size_t n = d[4];
    if (d.size() < 5 + n + 60)
        return std::nullopt;
    State s;
    s.wavetable.assign (reinterpret_cast<const char*> (d.data() + 5), n);
    s.wavetable.erase (std::find (s.wavetable.begin(), s.wavetable.end(), '\0'), s.wavetable.end());
    for (std::size_t i = 0; i < 15; ++i)
        std::memcpy (&s.knobs[i], d.data() + 5 + n + 4 * i, 4);
    return s;
}

std::vector<std::uint8_t> writeState (const State& s)
{
    std::vector<std::uint8_t> d { 1, 0, 0, 0 };
    const auto n = std::min<std::size_t> (s.wavetable.size(), 255);
    d.push_back ((std::uint8_t) n);
    d.insert (d.end(), s.wavetable.begin(), s.wavetable.begin() + (std::ptrdiff_t) n);
    for (auto k : s.knobs)
        for (int b = 0; b < 4; ++b)
            d.push_back ((std::uint8_t) ((std::uint32_t) k >> (8 * b)));
    return d;
}

// ------------------------------------------------------------------------------------------------------- table
namespace
{
constexpr float tableTag = 69.0f;
}

std::vector<float> FlKeysTable::encode() const
{
    std::vector<float> p { tableTag, raw ? 1.0f : 0.0f, resample ? 1.0f : 0.0f, sampleRate, velocityFactor,
                           releaseHighNotes ? 1.0f : 0.0f, (float) thresholds.size() };
    for (auto t : thresholds)
        p.push_back ((float) t);
    p.push_back ((float) keygroups.size());
    for (const auto& k : keygroups)
        p.insert (p.end(), { (float) k.root, (float) k.high, (float) k.pos, (float) k.end, (float) k.loop,
                             (float) ((k.posSet ? 1 : 0) | (k.endSet ? 2 : 0) | (k.loopSet ? 4 : 0) | (k.canLoop ? 8 : 0)),
                             (float) k.file, (float) k.duplicateOf });
    p.push_back ((float) files.size());
    for (const auto& f : files)
        p.insert (p.end(), { (float) f.loopStart, (float) f.loopEnd, f.scale });
    return p;
}

std::optional<FlKeysTable> FlKeysTable::decode (const std::vector<float>& p)
{
    std::size_t i = 0;
    auto next = [&] (float& v) { if (i >= p.size()) return false; v = p[i++]; return true; };
    auto nextInt = [&] (int& v) { float f; if (! next (f) || ! std::isfinite (f)) return false; v = (int) f; return true; };
    float tag = 0, raw = 0, resample = 0, release = 0;
    FlKeysTable t;
    int n = 0;
    if (! next (tag) || tag != tableTag || ! next (raw) || ! next (resample) || ! next (t.sampleRate) || ! next (t.velocityFactor)
        || ! next (release) || ! nextInt (n) || n < 1 || n > 255)
        return std::nullopt;
    t.raw = raw != 0.0f, t.resample = resample != 0.0f, t.releaseHighNotes = release != 0.0f;
    t.thresholds.resize ((std::size_t) n);
    for (auto& v : t.thresholds)
        if (! nextInt (v))
            return std::nullopt;
    if (! nextInt (n) || n < 1 || n > 4096)
        return std::nullopt;
    t.keygroups.resize ((std::size_t) n);
    for (auto& k : t.keygroups)
    {
        int flags = 0;
        if (! nextInt (k.root) || ! nextInt (k.high) || ! nextInt (k.pos) || ! nextInt (k.end) || ! nextInt (k.loop) || ! nextInt (flags)
            || ! nextInt (k.file) || ! nextInt (k.duplicateOf))
            return std::nullopt;
        k.posSet = flags & 1, k.endSet = flags & 2, k.loopSet = flags & 4, k.canLoop = flags & 8;
    }
    if (! nextInt (n) || n < 0 || n > 4096)
        return std::nullopt;
    t.files.resize ((std::size_t) n);
    for (auto& f : t.files)
        if (! nextInt (f.loopStart) || ! nextInt (f.loopEnd) || ! next (f.scale))
            return std::nullopt;
    return t;
}

// ------------------------------------------------------------------------------------------------------- .wti
namespace
{
std::string lower (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return s;
}

std::string trim (const std::string& s)
{
    const auto a = s.find_first_not_of (" \t\r\n");
    if (a == std::string::npos)
        return {};
    return s.substr (a, s.find_last_not_of (" \t\r\n") - a + 1);
}

struct Ini
{
    std::map<std::string, std::map<std::string, std::string>> sections;

    explicit Ini (const std::string& text)
    {
        std::string section;
        std::size_t start = 0;
        while (start < text.size())
        {
            auto end = text.find ('\n', start);
            if (end == std::string::npos)
                end = text.size();
            const auto line = trim (text.substr (start, end - start));
            start = end + 1;
            if (line.size() >= 2 && line.front() == '[' && line.back() == ']')
                section = lower (trim (line.substr (1, line.size() - 2)));
            else if (const auto eq = line.find ('='); eq != std::string::npos && eq > 0)
            {
                auto& keys = sections[section];
                const auto key = lower (trim (line.substr (0, eq)));
                if (keys.find (key) == keys.end()) // (the first occurrence wins, as in a TStringList lookup)
                    keys[key] = trim (line.substr (eq + 1));
            }
        }
    }

    const std::string* find (const std::string& sec, const std::string& key) const
    {
        const auto s = sections.find (sec);
        if (s == sections.end())
            return nullptr;
        const auto k = s->second.find (key);
        return k == s->second.end() ? nullptr : &k->second;
    }
    std::string text (const std::string& sec, const std::string& key) const
    {
        const auto* v = find (sec, key);
        return v != nullptr ? *v : std::string();
    }
    // ReadInteger: the value if it is an integer, else the default
    int integer (const std::string& sec, const std::string& key, int fallback, bool* present = nullptr) const
    {
        const auto* v = find (sec, key);
        if (present != nullptr)
            *present = false;
        if (v == nullptr || v->empty())
            return fallback;
        char* endp = nullptr;
        const auto x = std::strtol (v->c_str(), &endp, 10);
        if (endp == v->c_str() || *endp != '\0')
            return fallback;
        if (present != nullptr)
            *present = true;
        return (int) x;
    }
    float real (const std::string& sec, const std::string& key, double fallback) const
    {
        const auto* v = find (sec, key);
        if (v == nullptr || v->empty())
            return (float) fallback;
        char* endp = nullptr;
        const auto x = std::strtod (v->c_str(), &endp);
        return (float) (endp == v->c_str() || *endp != '\0' ? fallback : x);
    }
    // FUN_00715710: an empty value gives the default, otherwise "true" (any case) is true and anything else false
    bool boolean (const std::string& sec, const std::string& key, bool fallback) const
    {
        const auto* v = find (sec, key);
        if (v == nullptr || v->empty())
            return fallback;
        auto u = *v;
        std::transform (u.begin(), u.end(), u.begin(), [] (unsigned char c) { return (char) std::toupper (c); });
        return u == "TRUE";
    }
};

std::string fileNameOf (const std::string& path)
{
    const auto slash = path.find_last_of ("\\/:");
    return slash == std::string::npos ? path : path.substr (slash + 1);
}
} // namespace

std::optional<FlKeysTable> parseWti (const std::string& text, std::vector<std::string>& fileNames)
{
    // FUN_00713920 (REA_rodada69 §8.2)
    const Ini ini (text);
    fileNames.clear();
    FlKeysTable t;
    const auto count = ini.integer ("info", "samples", 0);
    const auto sampleFile = ini.text ("info", "samplefile");
    const auto type = lower (ini.text ("info", "filetype"));
    auto sub = ini.text ("info", "path");
    if (! sub.empty() && sub.back() != '\\')
        sub += '\\';
    t.velocityFactor = ini.real ("info", "velocityfactor", 1.0);
    t.releaseHighNotes = ini.boolean ("info", "releasehighnotes", false);
    t.resample = ini.boolean ("info", "resample", true);
    const auto canLoop = ini.boolean ("info", "canloop", true);
    t.sampleRate = ini.real ("info", "samplerate", 44100.0);
    if (count < 1 || count > 4096)
        return std::nullopt;

    // LYNX supports the two layouts FL's own wavetables use: one raw file for every keygroup, or one WAV per keygroup
    if (type == "raw" && ! sampleFile.empty())
    {
        t.raw = true;
        fileNames.push_back (sub + sampleFile);
        t.files.push_back ({});
    }
    else if (type != "riff" || ! sampleFile.empty())
        return std::nullopt;

    int root = 60, high = 999;
    std::vector<std::string> names; // the file name each keygroup stored (lower case)
    for (int i = 0; i < count; ++i)
    {
        const auto sec = "sample" + std::to_string (i);
        WtKeygroup k;
        const auto name = ini.text (sec, "name");
        if (! t.raw && ! name.empty())
        {
            const auto key = lower (fileNameOf (name));
            for (int j = 0; j < i; ++j)
                if (names[(std::size_t) j] == key)
                {
                    k.duplicateOf = j;
                    break;
                }
            if (k.duplicateOf < 0)
            {
                k.file = (int) fileNames.size();
                fileNames.push_back (sub + name);
                t.files.push_back ({});
            }
        }
        names.push_back (lower (fileNameOf (name)));
        k.root = ini.integer (sec, "root", -1);
        if (k.root == -1)
            k.root = root;
        k.high = ini.integer (sec, "high", 0);
        if (k.high == -1)
            k.high = high;
        root = k.root, high = k.high;
        k.pos = ini.integer (sec, "pos", 0, &k.posSet);
        k.end = ini.integer (sec, "end", 0, &k.endSet);
        k.loop = ini.integer (sec, "loop", 0, &k.loopSet);
        k.canLoop = canLoop && ini.boolean (sec, "canloop", true);
        t.keygroups.push_back (k);
    }
    if (! t.raw && t.files.empty())
        return std::nullopt;

    auto layers = ini.integer ("velocitylayers", "count", 1) & 0xFF;
    if (layers == 0)
        layers = 1;
    for (int l = 0; l < layers; ++l)
        t.thresholds.push_back (ini.integer ("velocitylayers", std::to_string (l), 0x80));
    t.thresholds[0] = 0;
    return t;
}

WtFile wavFileInfo (const std::vector<std::uint8_t>& w)
{
    WtFile f;
    if (w.size() < 12 || std::memcmp (w.data(), "RIFF", 4) != 0 || std::memcmp (w.data() + 8, "WAVE", 4) != 0)
        return f;
    for (std::size_t pos = 12; pos + 8 <= w.size();)
    {
        std::uint32_t size = 0;
        std::memcpy (&size, w.data() + pos + 4, 4);
        const auto* body = w.data() + pos + 8;
        const auto available = std::min<std::size_t> (size, w.size() - pos - 8);
        if (std::memcmp (w.data() + pos, "fmt ", 4) == 0 && available >= 2)
        {
            std::uint16_t tag = 0;
            std::memcpy (&tag, body, 2);
            if (tag == 0x674F || tag == 0x6750 || tag == 0x6751)
                f.scale = 32767.0f;
        }
        else if (std::memcmp (w.data() + pos, "smpl", 4) == 0 && available >= 36 + 24)
        {
            std::uint32_t loops = 0, start = 0, end = 0;
            std::memcpy (&loops, body + 28, 4);
            std::memcpy (&start, body + 36 + 8, 4);
            std::memcpy (&end, body + 36 + 12, 4);
            if (loops > 0 && start < 0x7fffffffu && end < 0x7fffffffu)
                f.loopStart = (int) start, f.loopEnd = (int) end;
        }
        pos += 8 + (std::size_t) size + (size & 1);
    }
    return f;
}

namespace
{
std::vector<std::uint8_t> wav16Mono (const std::vector<std::uint8_t>& raw, int sampleRate)
{
    std::vector<std::uint8_t> wav;
    auto u32 = [&] (std::uint32_t v) { for (int i = 0; i < 4; ++i) wav.push_back ((std::uint8_t) (v >> (8 * i))); };
    auto u16 = [&] (std::uint16_t v) { wav.push_back ((std::uint8_t) v); wav.push_back ((std::uint8_t) (v >> 8)); };
    auto tag = [&] (const char* t) { wav.insert (wav.end(), t, t + 4); };
    const auto bytes = (std::uint32_t) (raw.size() & ~(std::size_t) 1); // (FL reads whole 16-bit samples)
    tag ("RIFF"); u32 (36 + bytes); tag ("WAVE");
    tag ("fmt "); u32 (16); u16 (1); u16 (1); u32 ((std::uint32_t) sampleRate); u32 ((std::uint32_t) sampleRate * 2); u16 (2); u16 (16);
    tag ("data"); u32 (bytes);
    wav.insert (wav.end(), raw.begin(), raw.begin() + (std::ptrdiff_t) bytes);
    return wav;
}
} // namespace

bool importChannel (ChannelDef& def, const std::vector<std::uint8_t>& state, const std::string& wavetables,
                    const std::function<std::vector<std::uint8_t> (const std::string&)>& readFile,
                    const std::function<std::string (const std::string&)>& existing,
                    const std::function<std::string (const std::vector<std::uint8_t>&, const std::string&)>& saveEmbedded)
{
    const auto s = parseState (state);
    if (! s || s->wavetable.empty() || s->wavetable.find_first_of ("\\/:") != std::string::npos || ! readFile || ! existing)
        return false;
    auto wtiBytes = readFile (wavetables + "\\" + s->wavetable + ".wti");
    if (wtiBytes.empty())
    {
        // a wavetable this installation doesn't ship (FL 2026 has no "Piano"): the folder's default.wti names the
        // wavetable FL Keys loads instead ("[default] file=mda piano.wti", round 142)
        const auto def = readFile (wavetables + "\\default.wti");
        const std::string text (def.begin(), def.end());
        if (const auto at = text.find ("file="); at != std::string::npos)
        {
            auto name = text.substr (at + 5, text.find_first_of ("\r\n", at) - at - 5);
            if (! name.empty() && name.find_first_of ("\\/:") == std::string::npos)
                wtiBytes = readFile (wavetables + "\\" + name);
        }
    }
    std::vector<std::string> names;
    auto table = parseWti (std::string (wtiBytes.begin(), wtiBytes.end()), names);
    if (! table)
        return false;
    std::vector<SampleZone> zones;
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        const auto full = wavetables + "\\" + names[i];
        SampleZone z;
        if (table->raw)
        {
            const auto raw = readFile (full);
            if (raw.size() < 2 || ! saveEmbedded)
                return false;
            z.path = saveEmbedded (wav16Mono (raw, (int) std::clamp (table->sampleRate, 8000.0f, 192000.0f)), "FL Keys " + s->wavetable);
        }
        else
        {
            z.path = existing (full);
            if (! z.path.empty())
                table->files[i] = wavFileInfo (readFile (z.path));
        }
        if (z.path.empty())
            return false;
        zones.push_back (std::move (z));
    }
    def.instrument = InstrumentKind::flkeys;
    def.zones = std::move (zones);
    def.modelParams = table->encode();
    def.pluginState = state;
    return true;
}

// ------------------------------------------------------------------------------------------------------- loading
FileSamples toFileSamples (const SampleData& sample, float scale)
{
    FileSamples f;
    const auto n = sample.frames();
    f.channels = sample.isStereo() ? 2 : 1;
    f.samples.resize (n * (std::size_t) f.channels);
    const auto* l = sample.leftData();
    const auto* r = sample.rightData();
    auto q = [scale] (float x) {
        const auto v = std::nearbyint ((double) x * (double) scale);
        return (std::int16_t) std::clamp (v, -32768.0, 32767.0);
    };
    for (std::size_t i = 0; i < n; ++i)
    {
        f.samples[i * (std::size_t) f.channels] = q (l[i]);
        if (f.channels == 2)
            f.samples[i * 2 + 1] = q (r[i]);
    }
    return f;
}

std::shared_ptr<const Wavetable> buildWavetable (const FlKeysTable& t, const std::vector<FileSamples>& files)
{
    if (files.size() != t.files.size() || t.keygroups.empty())
        return nullptr;

    struct Rec
    {
        int root, high, pos, end, loop;
        bool canLoop, own;      // +0x14, +0x15
        int start, length, at;  // +0x18, +0x1c, +0x20
        std::string name;
    };
    std::vector<Rec> recs;
    auto w = std::make_shared<Wavetable>();
    std::vector<std::int16_t> data;
    int channels = 1;

    if (t.raw)
    {
        // FUN_00715350: the raw file as 16-bit samples
        const auto& raw = files[0];
        if (raw.samples.empty())
            return nullptr;
        for (const auto& k : t.keygroups)
        {
            Rec r { k.root, k.high, k.pos, k.end, k.loop, k.canLoop, true, k.pos, 0, 0, {} };
            r.length = r.end - r.start + 1;
            if (! recs.empty() && r.pos == recs.back().pos)
                r.own = false, r.start = recs.back().start;
            recs.push_back (r);
        }
        // FUN_007160b0: every keygroup's part copied one after another, each followed by a guard sample
        const auto rawCount = (int) raw.samples.size();
        data.assign ((std::size_t) rawCount + recs.size() + 2, 0);
        int at = 0, lastStart = 0;
        for (auto& r : recs)
        {
            if (! r.own)
            {
                r.start = lastStart; // (FL keeps the shared keygroup's original start here)
                r.pos = r.start + r.at;
                r.end = r.start + r.length - 1;
                continue;
            }
            for (int i = 0; i < r.length; ++i)
            {
                const auto from = r.start + i;
                if (from >= 0 && from < rawCount && at + i >= 0 && at + i < (int) data.size())
                    data[(std::size_t) (at + i)] = raw.samples[(std::size_t) from];
            }
            lastStart = r.start;
            r.start = at;
            r.pos = at + r.at;
            r.end = at + r.length - 1;
            const auto g = (r.end - r.loop) + 1;
            if (r.end + 1 >= 0 && r.end + 1 < (int) data.size())
                data[(std::size_t) (r.end + 1)] = g >= 0 && g < (int) data.size() ? data[(std::size_t) g] : (std::int16_t) 0;
            at += r.length + 1;
        }
        data.resize ((std::size_t) rawCount + recs.size());
    }
    else
    {
        // one WAV per keygroup (riff)
        int next = 0, previous = 0, defaultLoop = 0, loopEnd = 0;
        for (std::size_t i = 0; i < t.keygroups.size(); ++i)
        {
            const auto& k = t.keygroups[i];
            if (k.duplicateOf >= 0)
            {
                // FL copies the earlier keygroup's whole record, then reads root and high again
                auto r = recs[(std::size_t) k.duplicateOf];
                r.own = false, r.root = k.root, r.high = k.high;
                recs.push_back (r);
                continue;
            }
            Rec r { k.root, k.high, 0, 0, 0, k.canLoop, false, -1, 0, 0, {} };
            const bool opens = k.file >= 0;
            int frames = 0;
            if (opens)
            {
                const auto& f = files[(std::size_t) k.file];
                frames = (int) (f.samples.size() / (std::size_t) std::max (1, f.channels));
                if (frames < 1)
                    return nullptr; // (FL abandons the whole wavetable)
                channels = f.channels;
                // FUN_00715440: the WAV's loop, clamped (no loop: the whole file)
                const auto& wf = t.files[(std::size_t) k.file];
                const auto ls = wf.loopStart >= 0 ? wf.loopStart : 0;
                const auto le = wf.loopEnd >= 0 ? wf.loopEnd : frames - 1;
                const auto loopStart = std::max (0, std::min (frames, ls));
                loopEnd = std::max (loopStart, std::min (frames - 1, le));
                defaultLoop = loopEnd - loopStart + 1;
                r.own = true, r.start = next;
                data.resize ((std::size_t) (next + frames + 1) * (std::size_t) channels, 0);
                std::copy (f.samples.begin(), f.samples.begin() + (std::ptrdiff_t) ((std::size_t) frames * (std::size_t) channels),
                           data.begin() + (std::ptrdiff_t) ((std::size_t) next * (std::size_t) channels));
            }
            const auto base = opens ? next : previous;
            r.at = k.pos;
            r.pos = base + r.at;
            if (r.start == -1)
                r.start = r.pos;
            r.end = base + k.end;
            if (r.end == base)
                r.end = base + loopEnd;
            r.length = r.end - r.start + 1;
            r.loop = k.loopSet ? k.loop : defaultLoop;
            if (opens)
                for (int c = 0; c < channels; ++c)
                {
                    const auto dst = (std::size_t) (r.end + 1) * (std::size_t) channels + (std::size_t) c;
                    const auto src = (long long) (r.end - r.loop) * channels + c;
                    if (dst < data.size())
                        data[dst] = src >= 0 && src < (long long) data.size() ? data[(std::size_t) src] : (std::int16_t) 0;
                }
            if (! recs.empty() && r.pos == recs.back().pos)
                r.own = false, r.start = recs.back().start;
            if (opens)
            {
                previous = next;
                next += frames + 1;
            }
            recs.push_back (r);
        }
    }

    // FUN_00716470: clamp everything into the data; a keygroup that may not loop ends where its loop would start
    const auto total = (int) (data.size() / (std::size_t) channels);
    for (auto& r : recs)
    {
        r.root = std::clamp (r.root, 0, 127);
        r.high = std::clamp (r.high, 0, 999);
        r.pos = std::clamp (r.pos, 0, std::max (0, total - 1));
        r.end = std::min (std::max (r.end, r.pos), total - 1);
        r.loop = std::clamp (r.loop, 0, std::max (0, total - r.pos));
        if (! r.canLoop)
            r.loop = r.end;
        w->keygroups.push_back ({ r.root, r.high, r.pos, r.end, r.loop });
    }
    data.resize (data.size() + 4 * (std::size_t) channels, 0); // (reads never pass end + 1)
    w->data = std::move (data);
    w->stereo = channels == 2;
    w->thresholds = t.thresholds;
    w->sampleRate = t.sampleRate;
    w->velocityFactor = t.velocityFactor;
    w->releaseHighNotes = t.releaseHighNotes;
    return w;
}

// ------------------------------------------------------------------------------------------------------- engine
namespace
{
float asFloat (std::int32_t b) { float f; std::memcpy (&f, &b, 4); return f; }
std::int32_t asInt (float f) { std::int32_t b; std::memcpy (&b, &f, 4); return b; }
std::int64_t delphiRound (double x) { return (std::int64_t) std::nearbyint (x); }

// Delphi's Power (FUN_0071f170): integral exponents through IntPower
double delphiPower (double x, double y)
{
    if (y == 0.0)
        return 1.0;
    if (x == 0.0 && y > 0.0)
        return 0.0;
    if (y - std::trunc (y) == 0.0 && std::fabs (y) <= 2147483647.0)
    {
        const auto e = (int) y;
        auto a = (unsigned) (e < 0 ? -e : e);
        double b = x, r = 1.0;
        while (a > 0)
        {
            while ((a & 1) == 0)
            {
                a >>= 1;
                b *= b;
            }
            --a;
            r *= b;
        }
        return e < 0 ? 1.0 / r : r;
    }
    return std::exp (y * std::log (x));
}

constexpr int maxVoices = 128;

struct Voice
{
    bool active = false, listed = false; // listed: still in FL's voice list (ended voices leave it at the block end)
    std::uint64_t order = 0;
    int key = 0;
    bool released = false;
    float pitch = 0.0f;               // +0x18: Final.Pitch + 6000 when the step was computed
    std::int32_t delta = 0, frac = 0; // 16.16
    int pos = 0, end = 0, loop = 0;
    float env = 0.0f, dec = 1.0f;
    float f0 = 0.0f, f1 = 0.0f, r0 = 0.0f, r1 = 0.0f, ff = 0.0f; // muffle filter
    float outl = 0.0f, outr = 0.0f, note = 0.0f, velSens = 1.0f, L = 0.0f, R = 0.0f;
    float initVol = 1.0f, finalVol = 1.0f, finalPan = 0.0f, finalPitch = 0.0f;
    HostSlide slide; // a slide note moving Final.Pitch (issue #14); the render retunes the voice when it moves
};

class Engine
{
public:
    std::shared_ptr<const Wavetable> wt;
    bool panLawA = true;
    int maxPoly = 32;
    std::array<Voice, maxVoices> voices {};

    void prepare (double sampleRate)
    {
        iFs = (float) (1.0 / sampleRate); // dispatcher 4 (FUN_0071f260)
        buffer.reserve (2 * 8192);
        updateParams();
    }

    void setKnobs (const std::array<int, 15>& knobs)
    {
        for (int i = 0; i < 15; ++i)
            p[i] = (float) (knobs[(std::size_t) i] - knobRange[i].min) / (float) (knobRange[i].max - knobRange[i].min);
        updateParams();
    }

    Voice* trigger (int key, float initVol, float finalVol, float finalPan, float finalPitch)
    {
        if (wt == nullptr || wt->keygroups.empty())
            return nullptr;
        // TriggerVoice: with as many voices as the plugin's MaxPoly (32, set when it is created), the quietest
        // sounding one is ended
        if (maxPoly > 0 && maxPoly <= listCount)
        {
            int sounding = 0;
            Voice* quietest = nullptr;
            auto least = std::numeric_limits<float>::max();
            for (int i = 0; i < listCount; ++i)
            {
                auto* x = list[(std::size_t) i];
                if (! x->active)
                    continue;
                ++sounding;
                if (least > x->env)
                {
                    least = x->env;
                    quietest = x;
                }
            }
            if (sounding >= maxPoly && quietest != nullptr)
                quietest->active = false;
        }
        Voice* v = nullptr;
        for (auto& x : voices)
            if (! x.listed)
            {
                v = &x;
                break;
            }
        if (v == nullptr) // (only when the list holds every slot: LYNX drops the oldest voice)
        {
            v = &*std::min_element (voices.begin(), voices.end(), [] (const Voice& a, const Voice& b) { return a.order < b.order; });
            v->active = false;
            removeEnded();
        }
        *v = Voice {};
        v->listed = true;
        list[(std::size_t) listCount++] = v;
        v->order = ++serial, v->key = key;
        v->initVol = initVol, v->finalVol = finalVol, v->finalPan = finalPan, v->finalPitch = finalPitch;
        noteOn (*v);
        return v;
    }

    // Voice_Release (FUN_00722140)
    void release (Voice& v)
    {
        if (! v.active || v.released)
            return;
        v.released = true;
        double a;
        if (wt->releaseHighNotes || ! (v.note >= 94.0f) || v.note == 128.0f)
            a = wt->releaseHighNotes ? std::exp (((double) v.note * 0.01 + 6.0) - (double) p[1] * 5.0)
                                     : std::exp (((double) v.note * 0.017 + 2.0) - (double) p[1] * 2.0);
        else
            a = std::exp (((double) v.note * 0.01 + 6.0) - (double) p[1] * 5.0);
        v.dec = (float) std::exp ((double) (-iFs) * a);
    }

    void releaseAll()
    {
        for (auto& v : voices)
            release (v);
    }

    bool idle() const { return listCount == 0; }

    // Gen_Render (FUN_00722360): one host block, added into out (interleaved L/R)
    void render (float* left, float* right, int n)
    {
        if (wt == nullptr || n <= 0)
            return;
        const auto count = listCount;
        if (count == 0) // (FL's host removes ended voices; with none left, Gen_Render does nothing at all)
            return;

        for (int i = 0; i < count; ++i)
        {
            auto& v = *list[(std::size_t) i];
            if (! v.active)
                continue;
            if (v.finalPitch + 6000.0f != v.pitch)
                retune (v);
            const auto vol = 0.0f > v.finalVol ? 0.0f : v.finalVol;
            const auto g = (float) delphiPower ((double) vol, (double) v.velSens);
            if (panLawA)
                flvp::panLaw (v.finalPan, g, v.L, v.R);
            else
                v.L = v.R = g;
        }

        buffer.assign ((std::size_t) n * 2, 0.0f);
        const auto* d = wt->data.data();
        const bool stereo = wt->stereo;
        for (int i = 0; i < count; ++i)
        {
            auto& v = *list[(std::size_t) i];
            if (! v.active)
                continue;
            for (int s = 0; s < n; ++s)
            {
                v.frac += v.delta;
                v.pos += (int) ((std::uint32_t) v.frac >> 16);
                v.frac &= 0xffff;
                if (v.pos > v.end)
                {
                    if (v.loop >= v.end)
                    {
                        v.active = false; // (the voice asks the host to end it)
                        break;
                    }
                    v.pos -= v.loop;
                }
                auto xl = (float) ((double) v.env * ((double) sample (d, v.pos, v.frac, stereo ? 2 : 1, 0) - 3.0));
                if (stereo)
                {
                    auto xr = (float) ((double) v.env * ((double) sample (d, v.pos, v.frac, 2, 1) - 3.0));
                    v.env = v.env * v.dec;
                    xl = overdrive (xl);
                    xr = overdrive (xr);
                    v.f0 = v.f0 + v.ff * ((xl + v.f1) - v.f0);
                    v.f1 = xl;
                    v.r0 = v.r0 + v.ff * ((xr + v.r1) - v.r0);
                    v.r1 = xr;
                }
                else
                {
                    v.env = v.env * v.dec;
                    xl = overdrive (xl);
                    v.f0 = v.f0 + v.ff * ((xl + v.f1) - v.f0);
                    v.f1 = xl;
                    v.r1 = v.f1;
                    v.r0 = v.f0;
                }
                buffer[(std::size_t) (2 * s)] += (v.L * v.outl) * v.f0;
                buffer[(std::size_t) (2 * s + 1)] += (v.R * v.outr) * v.r0;
            }
        }

        for (int s = 0; s < n; ++s)
        {
            auto l = buffer[(std::size_t) (2 * s)], r = buffer[(std::size_t) (2 * s + 1)];
            // treble shelf
            trebleL = trebleL + trebleCoef * (l - trebleL);
            trebleR = trebleR + trebleCoef * (r - trebleR);
            r = r + trebleGain * (r - trebleR);
            l = l + trebleGain * (l - trebleL);
            // tremolo / auto-pan (a rotating sine/cosine pair)
            lfoSin = lfoSin + lfoRate * lfoCos;
            lfoCos = lfoCos - lfoRate * lfoSin;
            l = l + (l * tremL) * lfoCos;
            r = r + (r * tremR) * lfoCos;
            // stereo: (l + r) delayed by 128 samples, added to the left and taken from the right
            delay[(std::size_t) delayIndex] = l + r;
            delayIndex = (delayIndex & 0x7f) + 1;
            const auto x = stereoWidth * delay[(std::size_t) delayIndex];
            left[s] += l + x;
            right[s] += r - x;
        }

        for (int i = count - 1; i >= 0; --i)
            if (asInt (list[(std::size_t) i]->env) < 0x38D1B717) // (env below 1e-4, compared as integers)
                list[(std::size_t) i]->active = false;
        removeEnded();
        if (std::fabs ((double) trebleL) < 1e-10)
            trebleL = 0.0f;
        if (std::fabs ((double) trebleR) < 1e-10)
            trebleR = 0.0f;
    }

private:
    // The host ends the voices the plugin flags (Voice_Kill, FUN_00722300): FL's voice list drops an entry by moving
    // its last entry into the hole (FUN_005de9f0), from the end of the list down, so the summing order changes
    void removeEnded()
    {
        for (int i = listCount - 1; i >= 0; --i)
            if (! list[(std::size_t) i]->active)
            {
                list[(std::size_t) i]->listed = false;
                list[(std::size_t) i] = list[(std::size_t) --listCount];
            }
    }

    static float sample (const std::int16_t* d, int pos, std::int32_t frac, int ch, int c)
    {
        // linear interpolation in integers, turned into a float by building its bits: 3 + s·2^-22
        const std::int32_t s0 = d[(std::size_t) pos * (std::size_t) ch + (std::size_t) c];
        const std::int32_t s1 = d[((std::size_t) pos + 1) * (std::size_t) ch + (std::size_t) c];
        const auto bits = (std::uint32_t) s0 * 128u + ((std::uint32_t) frac >> 9) * (std::uint32_t) (s1 - s0) + 0x40400000u;
        return asFloat ((std::int32_t) bits);
    }

    float overdrive (float x) const
    {
        if ((double) x > 0.0 && drive != 0.0f)
        {
            x = x - drive * x * x;
            if (0.0 > (double) x)
                x = (float) ((double) x + (double) x * 0.5 * (double) x);
        }
        return x;
    }

    // FUN_00721230
    void updateParams()
    {
        offset = (int) delphiRound ((double) p[2] * 12.0 - 6.0);
        velHard = (float) ((double) p[3] * 0.12);
        velMuffle = (float) ((double) (p[5] * p[5]) * 5.0);
        trebleGain = (float) ((double) p[11] * 4.0 * (double) p[11] - 1.0);
        trebleCoef = (double) p[11] > 0.5 ? asFloat (0x3e8b442a) : asFloat (0x3ddb851f);
        tremR = (float) ((double) (p[12] + p[12]) - 1.0);
        tremL = tremR;
        if (0.5 > (double) p[12])
            tremR = -tremR;
        lfoRate = (float) ((double) iFs * 6.283 * std::exp ((double) p[13] * 9.22 - 4.61));
        velSens = (float) (((double) p[6] + 1.0) + (double) p[6]);
        if (0.25 > (double) p[6])
            velSens = (float) ((double) velSens - (0.75 - (double) p[6] * 3.0));
        tune = (float) ((double) (p[8] + p[8]) - 1.0);
        detune = (float) ((double) p[9] * 0.077 * (double) p[9]);
        stretch = (float) (((double) p[10] - 0.5) * 0.000434);
        drive = (float) ((double) p[14] * 1.8);
        stereoWidth = p[7] * p[7];
        stereoGain = (float) (1.0 - (double) stereoWidth * 0.53);
        stereoSpread = (float) ((double) p[7] * 0.04);
        if ((double) stereoSpread > 0.03)
            stereoSpread = asFloat (0x3cf5c28f);
    }

    static float clampNote (float n) { return 0.0f > n ? 0.0f : (n > 127.0f ? 127.0f : n); }

    float velocityOf (float initVol) const
    {
        // the host's Init.Vol back to a MIDI-like velocity: round(100·log11(10·vol + 1)), at most 127
        static const float invLn11 = 1.0f / (float) std::log (11.0);
        auto v = delphiRound ((double) ((float) (std::log ((double) (initVol * 10.0f + 1.0f)) * (double) invLn11) * 100.0f));
        if (v > 127)
            v = 127;
        return (float) v;
    }

    float tuning (float note) const
    {
        const auto k = (int) delphiRound ((double) ((note - 60.0f) * (note - 60.0f)));
        auto t = (float) ((double) tune + (double) detune * ((double) (k % 13) - 6.5));
        if (note > 60.0f)
            t += stretch * (float) k;
        return t;
    }

    int keyOffset (float vel) const
    {
        auto o = offset;
        if (vel > 40.0f)
            o += (int) delphiRound ((double) (velHard * (vel - 40.0f)));
        return o;
    }

    int keygroupFor (float note, int offs) const
    {
        const auto& kg = wt->keygroups;
        const auto layers = (int) wt->thresholds.size();
        int i = 0;
        while (i < (int) kg.size() && (float) (kg[(std::size_t) i].high + offs) < note)
            i += layers;
        return std::min (i, (int) kg.size() - 1);
    }

    std::int32_t stepFor (float t, float note, int i) const
    {
        t += note - (float) wt->keygroups[(std::size_t) i].root;
        const auto e = std::exp ((double) t * 0.05776226505);
        auto d = (float) ((double) (wt->sampleRate * iFs) * e);
        d *= 65536.0f;
        return (std::int32_t) delphiRound ((double) d);
    }

    // TriggerVoice (FUN_00721660)
    void noteOn (Voice& v)
    {
        v.pitch = v.finalPitch + 6000.0f;
        const auto note = clampNote (v.pitch * 0.01f);
        const auto vel = velocityOf (v.initVol);
        const auto t = tuning (note);
        auto i = keygroupFor (note, keyOffset (vel));
        for (int l = 1; l < (int) wt->thresholds.size() && (float) wt->thresholds[(std::size_t) l] < vel
                        && i + 1 < (int) wt->keygroups.size(); ++l)
            ++i;
        const auto& k = wt->keygroups[(std::size_t) i];
        v.delta = stepFor (t, note, i);
        v.frac = 0, v.pos = k.pos, v.end = k.end, v.loop = k.loop;
        v.env = wt->velocityFactor;
        v.velSens = velSens;
        auto f = (float) ((double) (p[4] * p[4] * muffleBase) + 50.0 + (double) (velMuffle * (vel - 64.0f)));
        if ((double) note * 0.4 + 55.0 > (double) f)
            f = (float) ((double) note * 0.4 + 55.0);
        if ((double) f > 210.0)
            f = 210.0f;
        v.ff = f * f * iFs;
        v.note = note;
        auto n2 = note;
        if (12.0f > n2)
            n2 = 12.0f;
        if (n2 > 108.0f)
            n2 = 108.0f;
        const auto w = outGain * stereoGain;
        v.outr = w + (w * stereoSpread) * (n2 - 60.0f);
        v.outl = (w + w) - v.outr;
        if (44.0f > n2)
            n2 = 44.0f;
        if (p[0] == 0.0f)
            v.dec = 1.0f;
        else
        {
            auto q = (float) ((double) p[0] * 2.0);
            if ((double) q < 1.0)
                q = (float) (((double) q + 0.25) - (double) p[0] * 0.5);
            const auto a = std::exp (((double) n2 * 0.033 + (-0.6)) - (double) q);
            v.dec = (float) std::exp ((double) (-iFs) * a);
        }
        v.f0 = v.f1 = v.r0 = v.r1 = 0.0f;
        v.active = true;
    }

    // FUN_00722e20: a new pitch keeps the voice's keygroup search on its first note and layer 0
    void retune (Voice& v)
    {
        v.pitch = v.finalPitch + 6000.0f;
        const auto note = clampNote (v.pitch * 0.01f);
        const auto vel = velocityOf (v.initVol);
        const auto t = tuning (note);
        v.delta = stepFor (t, note, keygroupFor (v.note, keyOffset (vel)));
    }

    float p[15] {};
    float iFs = 1.0f / 44100.0f;
    int offset = 0;
    float tune = 0, detune = 0, stretch = 0, drive = 0, muffleBase = 160.0f, velMuffle = 0, velHard = 0, velSens = 1;
    float outGain = 0.2f, stereoWidth = 0, stereoSpread = 0, stereoGain = 1;
    float lfoSin = 0, lfoCos = 1, lfoRate = 0, tremL = 0, tremR = 0, trebleGain = 0, trebleCoef = 0;
    float trebleL = 0, trebleR = 0;
    std::array<float, 256> delay {};
    int delayIndex = 0;
    std::uint64_t serial = 0;
    std::vector<float> buffer;
    std::array<Voice*, maxVoices> list {}; // FL's voice list order
    int listCount = 0;
};

// ------------------------------------------------------------------------------------------------------- front-end
//
// Levels as the other FL voice generators (REA_rodada47 §2.1): Final.Vol = channel volume × Init.Vol (Init.Vol =
// (11^(1.28·vel) − 1)/10 from LYNX's velocity), Final.Pan = note pan + channel pan, Final.Pitch = 100·(key − 60) +
// channel pitch + note cents; LYNX's channel gains gl = √2·V·cos((1 + p)π/4), gr = √2·V·sin(…) give V and p back.
class FlKeysInstrument final : public Instrument
{
public:
    explicit FlKeysInstrument (std::shared_ptr<const Wavetable> w)
    {
        engine.wt = std::move (w);
        engine.prepare (44100.0);
    }

    void prepare (double sr) noexcept override
    {
        engine.prepare (sr);
        engine.setKnobs (loadKnobs());
    }

    void configure (const ChannelDef& channel, double) noexcept override
    {
        pitchCents.store (channel.sampler.pitch * 100.0f);
        if (const auto s = parseState (channel.pluginState))
            for (std::size_t i = 0; i < 15; ++i)
                knobs[i].store (s->knobs[i]);
    }

    void setNextNoteExpression (float pan, float cents, bool) noexcept override
    {
        nextPan = pan;
        nextCents = cents;
    }

    void noteOn (int key, float velocity) noexcept override
    {
        sync();
        const auto vel = std::clamp ((double) velocity, 0.0, 1.0);
        const auto initVol = (std::pow (11.0, 1.28 * vel) - 1.0) / 10.0;
        const auto k = std::clamp (key, 0, 127);
        const auto finalPitch = (float) (100.0 * (k - 60) + (double) pitchCents.load() + (double) nextCents);
        if (auto* v = engine.trigger (k, (float) initVol, (float) (channelVol * initVol),
                                      (float) std::clamp ((double) nextPan + channelPan, -1.0, 1.0), finalPitch))
        {
            const auto slot = (std::size_t) (v - engine.voices.data());
            volFactor[slot] = (float) initVol;
            notePan[slot] = nextPan;
        }
        nextPan = 0.0f;
        nextCents = 0.0f;
    }

    void noteOff (int key) noexcept override
    {
        for (auto& v : engine.voices)
            if (v.active && v.key == key && ! v.released)
                engine.release (v);
    }

    void allNotesOff() noexcept override { engine.releaseAll(); }

    void slideTo (int key, float cents, float samples) noexcept override
    {
        const auto to = (float) (100.0 * (std::clamp (key, 0, 127) - 60) + (double) pitchCents.load() + (double) cents);
        for (auto& v : engine.voices)
            if (v.active && ! v.released)
                v.slide.start (to - (v.finalPitch - v.slide.cents), samples);
    }

    bool idle() const noexcept override { return engine.idle(); }

    void render (float* left, float* right, int n, float gl, float gr) noexcept override
    {
        sync();
        const auto vol = std::sqrt (((double) gl * gl + (double) gr * gr) * 0.5);
        auto pan = vol > 0.0 ? std::atan2 ((double) gr, (double) gl) * (4.0 / 3.141592653589793) - 1.0 : 0.0;
        if (std::fabs (pan) < 1e-9)
            pan = 0.0;
        if (vol != channelVol || pan != channelPan)
        {
            channelVol = vol, channelPan = pan;
            for (std::size_t i = 0; i < engine.voices.size(); ++i)
                if (engine.voices[i].active)
                {
                    engine.voices[i].finalVol = (float) (vol * (double) volFactor[i]);
                    engine.voices[i].finalPan = (float) std::clamp ((double) notePan[i] + pan, -1.0, 1.0);
                }
        }
        for (int done = 0; done < n;)
        {
            const auto len = std::min (512, n - done);
            engine.render (left + done, right + done, len);
            for (auto& v : engine.voices) // a slide note moves the voices' Final.Pitch (issue #14)
                if (v.active && v.slide.moving())
                {
                    const auto was = v.slide.cents;
                    v.finalPitch += v.slide.advance ((float) len) - was;
                }
            done += len;
        }
    }

private:
    void sync() noexcept
    {
        const auto k = loadKnobs();
        if (k != current)
        {
            current = k;
            engine.setKnobs (k);
        }
    }

    std::array<int, 15> loadKnobs() const noexcept
    {
        std::array<int, 15> k {};
        for (std::size_t i = 0; i < 15; ++i)
            k[i] = knobs[i].load();
        return k;
    }

    Engine engine;
    std::array<std::atomic<int>, 15> knobs {};
    std::array<int, 15> current {};
    std::atomic<float> pitchCents { 0.0f };
    std::array<float, maxVoices> volFactor {}, notePan {};
    double channelVol = 1.0, channelPan = 0.0;
    float nextPan = 0.0f, nextCents = 0.0f;
};
} // namespace

std::unique_ptr<Instrument> makeFlKeysInstrument (std::shared_ptr<const Wavetable> wavetable)
{
    return std::make_unique<FlKeysInstrument> (std::move (wavetable));
}

std::vector<float> renderReference (const Wavetable& wavetable, const State& state, const std::vector<RefNote>& notes,
                                    double sampleRate, int length, int block, bool panLawA)
{
    if (length <= 0)
        return {};
    auto engine = std::make_unique<Engine>();
    engine->wt = std::shared_ptr<const Wavetable> (&wavetable, [] (const Wavetable*) {});
    engine->panLawA = panLawA;
    engine->prepare (sampleRate);
    engine->setKnobs (state.knobs);
    block = std::clamp (block, 1, 8192);
    std::vector<float> out ((std::size_t) length * 2, 0.0f), L ((std::size_t) block), R ((std::size_t) block);
    std::vector<Voice*> handle (notes.size(), nullptr);
    std::vector<std::uint64_t> serials (notes.size(), 0); // (a slot is reused once its voice has ended)
    for (int n0 = 0; n0 < length;)
    {
        auto len = std::min (block, length - n0);
        for (const auto& e : notes)
        {
            if (e.start > n0 && e.start < n0 + len)
                len = e.start - n0;
            if (e.release > n0 && e.release < n0 + len)
                len = e.release - n0;
        }
        for (std::size_t i = 0; i < notes.size(); ++i)
        {
            const auto& e = notes[i];
            if (e.start == n0)
            {
                handle[i] = engine->trigger (e.key, (float) e.initVol, (float) e.finalVol, (float) e.finalPan,
                                             (float) (100.0 * e.key - 6000.0 + e.cents));
                serials[i] = handle[i] != nullptr ? handle[i]->order : 0;
            }
            if (e.release == n0 && handle[i] != nullptr && handle[i]->order == serials[i])
                engine->release (*handle[i]);
        }
        std::fill (L.begin(), L.end(), 0.0f);
        std::fill (R.begin(), R.end(), 0.0f);
        engine->render (L.data(), R.data(), len);
        for (int i = 0; i < len; ++i)
        {
            out[(std::size_t) (2 * (n0 + i))] = L[(std::size_t) i];
            out[(std::size_t) (2 * (n0 + i) + 1)] = R[(std::size_t) i];
        }
        n0 += len;
    }
    return out;
}
} // namespace flkeys
} // namespace lynx
