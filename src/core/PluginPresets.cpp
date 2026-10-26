#include "core/PluginPresets.h"

#include <algorithm>
#include <cstring>
#include <sstream>


namespace lynx::pluginpresets
{
namespace
{
constexpr const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode (const std::uint8_t* data, std::size_t size)
{
    std::string out;
    out.reserve ((size + 2) / 3 * 4);
    for (std::size_t i = 0; i < size; i += 3)
    {
        const std::uint32_t v = ((std::uint32_t) data[i] << 16) | (i + 1 < size ? (std::uint32_t) data[i + 1] << 8 : 0u)
                                | (i + 2 < size ? (std::uint32_t) data[i + 2] : 0u);
        out += b64[(v >> 18) & 63];
        out += b64[(v >> 12) & 63];
        out += i + 1 < size ? b64[(v >> 6) & 63] : '=';
        out += i + 2 < size ? b64[v & 63] : '=';
    }
    return out;
}

bool base64Decode (const std::string& text, std::vector<std::uint8_t>& out)
{
    out.clear();
    std::uint32_t acc = 0;
    int bits = 0;
    for (const auto c : text)
    {
        if (c == '=' || c == ' ' || c == '\r' || c == '\n')
            continue;
        const auto* at = std::strchr (b64, c);
        if (at == nullptr || c == 0)
            return false;
        acc = (acc << 6) | (std::uint32_t) (at - b64);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back ((std::uint8_t) (acc >> bits));
        }
    }
    return true;
}

std::string trim (const std::string& s)
{
    const auto a = s.find_first_not_of (" \t\r\n");
    if (a == std::string::npos)
        return {};
    const auto b = s.find_last_not_of (" \t\r\n");
    return s.substr (a, b - a + 1);
}

// one line per value: a newline in a name would end it
std::string oneLine (std::string s)
{
    std::replace (s.begin(), s.end(), '\n', ' ');
    std::replace (s.begin(), s.end(), '\r', ' ');
    return trim (s);
}

// --- big-endian reader / writer --------------------------------------------------------------------------------------
struct Reader
{
    const std::vector<std::uint8_t>& b;
    std::size_t at = 0;
    bool ok = true;

    std::int32_t i32()
    {
        if (at + 4 > b.size())
        {
            ok = false;
            return 0;
        }
        const auto v = ((std::uint32_t) b[at] << 24) | ((std::uint32_t) b[at + 1] << 16) | ((std::uint32_t) b[at + 2] << 8) | (std::uint32_t) b[at + 3];
        at += 4;
        return (std::int32_t) v;
    }

    float f32()
    {
        const auto bits = (std::uint32_t) i32();
        float f;
        std::memcpy (&f, &bits, 4);
        return f;
    }

    std::string text (std::size_t n)
    {
        if (at + n > b.size())
        {
            ok = false;
            return {};
        }
        std::string s (reinterpret_cast<const char*> (b.data() + at), n);
        at += n;
        if (const auto z = s.find ('\0'); z != std::string::npos)
            s.resize (z);
        return s;
    }

    std::vector<std::uint8_t> bytes (std::size_t n)
    {
        if (at + n > b.size())
        {
            ok = false;
            return {};
        }
        std::vector<std::uint8_t> out (b.begin() + (std::ptrdiff_t) at, b.begin() + (std::ptrdiff_t) (at + n));
        at += n;
        return out;
    }
};

struct Writer
{
    std::vector<std::uint8_t> b;

    void i32 (std::int32_t v)
    {
        const auto u = (std::uint32_t) v;
        b.push_back ((std::uint8_t) (u >> 24));
        b.push_back ((std::uint8_t) (u >> 16));
        b.push_back ((std::uint8_t) (u >> 8));
        b.push_back ((std::uint8_t) u);
    }

    void f32 (float f)
    {
        std::uint32_t bits;
        std::memcpy (&bits, &f, 4);
        i32 ((std::int32_t) bits);
    }

    void text (const std::string& s, std::size_t n) // zero-padded, always terminated
    {
        for (std::size_t i = 0; i < n; ++i)
            b.push_back (i + 1 < n && i < s.size() ? (std::uint8_t) s[i] : 0);
    }

    void bytes (const std::vector<std::uint8_t>& v) { b.insert (b.end(), v.begin(), v.end()); }

    // the chunk's byteSize field, once everything after it is written
    void patchSize (std::size_t sizeField) { patch (sizeField, (std::int32_t) (b.size() - sizeField - 4)); }

    void patch (std::size_t at, std::int32_t v)
    {
        const auto u = (std::uint32_t) v;
        b[at] = (std::uint8_t) (u >> 24);
        b[at + 1] = (std::uint8_t) (u >> 16);
        b[at + 2] = (std::uint8_t) (u >> 8);
        b[at + 3] = (std::uint8_t) u;
    }
};

const std::int32_t ccnk = fourCC ("CcnK"), fxck = fourCC ("FxCk"), fpch = fourCC ("FPCh"), fxbk = fourCC ("FxBk"), fbch = fourCC ("FBCh");
constexpr std::int32_t maxParams = 1 << 16, maxPrograms = 1 << 14;

// one fxProgram after its CcnK magic
std::optional<std::pair<FxFile::Kind, FxProgram>> readProgram (Reader& r, FxFile& file)
{
    if (r.i32() != ccnk)
        return std::nullopt;
    r.i32(); // byteSize
    const auto magic = r.i32();
    r.i32(); // format version
    file.pluginId = r.i32();
    file.pluginVersion = r.i32();
    const auto numParams = r.i32();
    FxProgram program;
    program.name = r.text (28);
    if (! r.ok || numParams < 0 || numParams > maxParams)
        return std::nullopt;
    if (magic == fxck)
    {
        program.params.reserve ((std::size_t) numParams);
        for (int i = 0; i < numParams; ++i)
            program.params.push_back (r.f32());
        if (! r.ok)
            return std::nullopt;
        return std::make_pair (FxFile::Kind::paramProgram, program);
    }
    if (magic == fpch)
    {
        const auto size = r.i32();
        if (! r.ok || size < 0)
            return std::nullopt;
        file.chunk = r.bytes ((std::size_t) size);
        if (! r.ok)
            return std::nullopt;
        return std::make_pair (FxFile::Kind::chunkProgram, program);
    }
    return std::nullopt;
}

void writeProgram (Writer& w, const FxFile& file, const FxProgram& program, bool chunk)
{
    w.i32 (ccnk);
    const auto sizeField = w.b.size();
    w.i32 (0);
    w.i32 (chunk ? fpch : fxck);
    w.i32 (1);
    w.i32 (file.pluginId);
    w.i32 (file.pluginVersion);
    w.i32 ((std::int32_t) program.params.size());
    w.text (program.name, 28);
    if (chunk)
    {
        w.i32 ((std::int32_t) file.chunk.size());
        w.bytes (file.chunk);
    }
    else
        for (const auto p : program.params)
            w.f32 (p);
    w.patchSize (sizeField);
}
} // namespace

std::int32_t fourCC (const char* t) noexcept
{
    return (std::int32_t) (((std::uint32_t) (std::uint8_t) t[0] << 24) | ((std::uint32_t) (std::uint8_t) t[1] << 16)
                           | ((std::uint32_t) (std::uint8_t) t[2] << 8) | (std::uint32_t) (std::uint8_t) t[3]);
}

std::string toText (const Preset& p)
{
    std::ostringstream out;
    out << header << "\n";
    out << "name = " << oneLine (p.name) << "\n";
    out << "plugin = " << oneLine (p.pluginName) << "\n";
    out << "id = " << oneLine (p.pluginId) << "\n";
    out << "format = " << oneLine (p.format) << "\n";
    out << "state = " << base64Encode (p.state.data(), p.state.size()) << "\n";
    return out.str();
}

std::optional<Preset> fromText (const std::string& text)
{
    std::istringstream in (text);
    std::string line;
    if (! std::getline (in, line) || trim (line) != header)
        return std::nullopt;
    Preset p;
    bool hasState = false;
    while (std::getline (in, line))
    {
        const auto eq = line.find ('=');
        if (eq == std::string::npos)
            continue;
        const auto key = trim (line.substr (0, eq));
        const auto value = trim (line.substr (eq + 1));
        if (key == "name")
            p.name = value;
        else if (key == "plugin")
            p.pluginName = value;
        else if (key == "id")
            p.pluginId = value;
        else if (key == "format")
            p.format = value;
        else if (key == "state")
            hasState = base64Decode (value, p.state);
    }
    if (! hasState)
        return std::nullopt;
    return p;
}

std::string folderNameFor (const std::string& pluginName)
{
    std::string out;
    for (const auto c : pluginName)
    {
        const auto u = (unsigned char) c;
        if (u < 32 || std::strchr ("<>:\"/\\|?*", c) != nullptr)
            out += '_';
        else
            out += c;
    }
    out = trim (out);
    while (! out.empty() && (out.back() == '.' || out.back() == ' ')) // (Windows drops trailing dots and spaces)
        out.pop_back();
    if (out.size() > 64)
    {
        out.resize (64);
        while (! out.empty() && ((unsigned char) out.back() & 0xc0) == 0x80) // don't cut a UTF-8 character
            out.pop_back();
        if (! out.empty() && ((unsigned char) out.back() & 0x80) != 0)
            out.pop_back();
        out = trim (out);
    }
    return out.empty() ? std::string ("Plugin") : out;
}

std::optional<FxFile> parseFx (const std::vector<std::uint8_t>& bytes)
{
    Reader r { bytes };
    FxFile file;
    if (bytes.size() < 12)
        return std::nullopt;
    const auto magic = Reader { bytes, 8 }.i32();
    if (magic == fxck || magic == fpch)
    {
        auto program = readProgram (r, file);
        if (! program)
            return std::nullopt;
        file.kind = program->first;
        if (file.kind == FxFile::Kind::chunkProgram)
            file.programName = program->second.name;
        else
            file.programs.push_back (std::move (program->second));
        return file;
    }
    if (magic != fxbk && magic != fbch)
        return std::nullopt;

    if (r.i32() != ccnk)
        return std::nullopt;
    r.i32(); // byteSize
    r.i32(); // magic
    const auto version = r.i32();
    file.pluginId = r.i32();
    file.pluginVersion = r.i32();
    const auto numPrograms = r.i32();
    if (version >= 2)
    {
        file.currentProgram = r.i32();
        r.bytes (124);
    }
    else
        r.bytes (128);
    if (! r.ok || numPrograms < 0 || numPrograms > maxPrograms)
        return std::nullopt;
    if (magic == fbch)
    {
        file.kind = FxFile::Kind::chunkBank;
        file.bankPrograms = numPrograms;
        const auto size = r.i32();
        if (! r.ok || size < 0)
            return std::nullopt;
        file.chunk = r.bytes ((std::size_t) size);
        if (! r.ok)
            return std::nullopt;
        return file;
    }
    file.kind = FxFile::Kind::paramBank;
    const auto id = file.pluginId, ver = file.pluginVersion;
    for (int i = 0; i < numPrograms; ++i)
    {
        FxFile scratch;
        auto program = readProgram (r, scratch);
        if (! program || program->first != FxFile::Kind::paramProgram)
            return std::nullopt;
        file.programs.push_back (std::move (program->second));
    }
    file.pluginId = id;
    file.pluginVersion = ver;
    return file;
}

std::vector<std::uint8_t> writeFx (const FxFile& file)
{
    Writer w;
    switch (file.kind)
    {
        case FxFile::Kind::paramProgram:
            writeProgram (w, file, file.programs.empty() ? FxProgram {} : file.programs.front(), false);
            break;
        case FxFile::Kind::chunkProgram:
            writeProgram (w, file, FxProgram { file.programName, {} }, true);
            break;
        case FxFile::Kind::paramBank:
        case FxFile::Kind::chunkBank:
        {
            const auto chunk = file.kind == FxFile::Kind::chunkBank;
            w.i32 (ccnk);
            const auto sizeField = w.b.size();
            w.i32 (0);
            w.i32 (chunk ? fbch : fxbk);
            w.i32 (2);
            w.i32 (file.pluginId);
            w.i32 (file.pluginVersion);
            w.i32 (chunk ? std::max<std::int32_t> (1, file.bankPrograms) : (std::int32_t) file.programs.size());
            w.i32 (file.currentProgram);
            for (int i = 0; i < 124; ++i)
                w.b.push_back (0);
            if (chunk)
            {
                w.i32 ((std::int32_t) file.chunk.size());
                w.bytes (file.chunk);
            }
            else
                for (const auto& program : file.programs)
                    writeProgram (w, file, program, false);
            w.patchSize (sizeField);
            break;
        }
    }
    return std::move (w.b);
}
} // namespace lynx::pluginpresets
