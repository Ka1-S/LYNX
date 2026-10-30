#include "core/CrashReport.h"

#include <algorithm>
#include <cstdio>

namespace lynx::crashreport
{
// ================================================================================================================
// consent
// ================================================================================================================
Consent parseConsent (std::string_view text)
{
    while (! text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.remove_suffix (1);
    while (! text.empty() && text.front() == ' ')
        text.remove_prefix (1);
    if (text == "always")
        return Consent::always;
    if (text == "never")
        return Consent::never;
    return Consent::ask;
}

const char* consentName (Consent c)
{
    switch (c)
    {
        case Consent::always: return "always";
        case Consent::never: return "never";
        case Consent::ask: break;
    }
    return "ask";
}

Action actionFor (Consent c)
{
    switch (c)
    {
        case Consent::always: return Action::sendSilently;
        case Consent::never: return Action::dontSend;
        case Consent::ask: break;
    }
    return Action::askUser;
}

// ================================================================================================================
// anonymising
// ================================================================================================================
namespace
{
char lowerAscii (char c)
{
    return c >= 'A' && c <= 'Z' ? (char) (c - 'A' + 'a') : c;
}

bool isAlnumAscii (unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// a byte of a word (letters, digits, any UTF-8 byte: accented names stay whole words)
bool isWordByte (unsigned char c)
{
    return isAlnumAscii (c) || c >= 0x80;
}

bool isSeparator (char c)
{
    return c == '\\' || c == '/';
}

// characters that end a path (or a file name) in a log line
bool isStop (char c)
{
    return c == '\n' || c == '\r' || c == '\t' || c == '"' || c == '\'' || c == '<' || c == '>' || c == '|' || c == '(' || c == ')'
        || c == '[' || c == ']' || c == '=' || c == '*' || c == '?';
}

// what ends a file name or a path walked backwards from its extension (file names have spaces, apostrophes, brackets)
bool isPathStop (char c)
{
    return c == '\n' || c == '\r' || c == '\t' || c == '"' || c == '|' || c == '*' || c == '?' || c == '=';
}

// lower case of byte i of a UTF-8 text: ASCII, and Latin-1's capitals (À..Þ = C3 80..9E, without ×) as in "Área"
unsigned char foldAt (std::string_view t, std::size_t i)
{
    const auto c = (unsigned char) t[i];
    if (c >= 'A' && c <= 'Z')
        return (unsigned char) (c - 'A' + 'a');
    if (i > 0 && (unsigned char) t[i - 1] == 0xC3 && c >= 0x80 && c <= 0x9E && c != 0x97)
        return (unsigned char) (c + 0x20);
    return c;
}

bool equalsNoCase (std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (foldAt (a, i) != foldAt (b, i))
            return false;
    return true;
}

std::size_t findNoCase (const std::string& text, std::string_view needle, std::size_t from)
{
    if (needle.empty() || text.size() < needle.size())
        return std::string::npos;
    for (std::size_t i = from; i + needle.size() <= text.size(); ++i)
        if (equalsNoCase (std::string_view (text).substr (i, needle.size()), needle))
            return i;
    return std::string::npos;
}

// Replaces every whole-word, case-insensitive (ASCII) occurrence of `word` by `with`.
void replaceWord (std::string& text, std::string_view word, std::string_view with)
{
    if (word.empty())
        return;
    for (std::size_t at = findNoCase (text, word, 0); at != std::string::npos; at = findNoCase (text, word, at))
    {
        const auto end = at + word.size();
        const bool before = at == 0 || ! isWordByte ((unsigned char) text[at - 1]);
        const bool after = end >= text.size() || ! isWordByte ((unsigned char) text[end]);
        if (before && after)
        {
            text.replace (at, word.size(), with);
            at += with.size();
        }
        else
            at = end;
    }
}

// The end of the path segment starting at `from`: the next separator when there is one before the line / path ends,
// else the next white space (so "/home/name is …" in prose does not swallow the sentence).
std::size_t segmentEnd (const std::string& text, std::size_t from)
{
    for (auto i = from; i < text.size(); ++i)
    {
        if (isSeparator (text[i]))
            return i;
        if (isStop (text[i]))
            break;
    }
    auto i = from;
    while (i < text.size() && ! isSeparator (text[i]) && ! isStop (text[i]) && text[i] != ' ' && text[i] != ',' && text[i] != ';')
        ++i;
    return i;
}

// after `marker` (case-insensitive), the next segment -> `with` (unless it is one of `keep`)
void replaceSegmentAfter (std::string& text, std::string_view marker, std::string_view with, std::initializer_list<std::string_view> keep)
{
    for (auto at = findNoCase (text, marker, 0); at != std::string::npos; at = findNoCase (text, marker, at))
    {
        const auto start = at + marker.size();
        const auto end = segmentEnd (text, start);
        const auto segment = std::string_view (text).substr (start, end - start);
        bool kept = segment.empty() || segment == with;
        for (auto k : keep)
            kept = kept || equalsNoCase (segment, k);
        if (! kept)
            text.replace (start, end - start, with);
        at = start + (kept ? segment.size() : with.size());
    }
}

// Common folders that say nothing about the person (kept in the paths of hidden files)
bool isCommonFolder (std::string_view segment)
{
    if (segment.empty() || (segment.front() == '<' && segment.back() == '>'))
        return true;
    if (segment.size() == 2 && segment[1] == ':') // a drive
        return true;
    static constexpr std::string_view common[] = {
        "users", "home", "documents and settings", "desktop", "documents", "my documents", "music", "my music", "downloads", "videos",
        "pictures", "onedrive", "appdata", "roaming", "local", "localLow", "temp", "tmp", "library", "application support", "lynx",
        "autosave", "projects", "samples", "audio", "recordings", "recorded", "image-line", "data", "patches", "packs", "program files",
        "program files (x86)", "common files", "vst3", "vstplugins", "public", "volumes", "mnt", "media", "run", "var", "c", "d", "e",
        // pt-BR / es / fr / de desktop and documents folders (OneDrive shows the localised names)
        "área de trabalho", "documentos", "música", "músicas", "imagens", "vídeos", "escritorio", "escritório", "bureau", "musique",
        "schreibtisch", "dokumente", "musik", "téléchargements", "descargas", "projetos", "proyectos",
    };
    for (auto c : common)
        if (equalsNoCase (segment, c))
            return true;
    return (segment.size() >= 9 && equalsNoCase (segment.substr (0, 9), "fl studio"))
        || (segment.size() >= 11 && equalsNoCase (segment.substr (0, 11), "onedrive - ")); // (its organisation is <org> already)
}

// the known prefixes of LYNX's own session steps before a file name ("opening project NAME.flp")
constexpr std::string_view namePrefixes[] = { "project ", "opening ", "loading ", "saving ", "file ", "sample ", "exporting ", "export ", "recording " };

// The start of the path whose file name begins at `nameStart` (just after a separator): a drive ("C:\"), a UNC share
// ("\\"), "~/" or a "/" after white space; npos when none (a relative path). Never before `bound`.
std::size_t pathRoot (const std::string& s, std::size_t bound, std::size_t nameStart, std::size_t& rootEnd)
{
    for (auto p = nameStart; p-- > bound;)
    {
        const char c = s[p];
        if (isPathStop (c))
            return std::string::npos;
        const bool startHere = p == bound || ! isAlnumAscii ((unsigned char) s[p - 1]);
        if (p + 2 < s.size() && isAlnumAscii ((unsigned char) c) && ! (c >= '0' && c <= '9') && s[p + 1] == ':' && isSeparator (s[p + 2]) && startHere)
        {
            rootEnd = p + 3;
            return p;
        }
        if (c == '\\' && p + 1 < s.size() && s[p + 1] == '\\' && (p == bound || s[p - 1] == ' '))
        {
            rootEnd = p + 2;
            return p;
        }
        if (c == '~' && p + 1 < s.size() && s[p + 1] == '/' && (p == bound || s[p - 1] == ' '))
        {
            rootEnd = p + 2;
            return p;
        }
        if (c == '/' && (p == bound || s[p - 1] == ' ' || s[p - 1] == ':'))
        {
            rootEnd = p + 1;
            return p;
        }
    }
    return std::string::npos;
}

void hideFileNames (std::string& s)
{
    std::size_t bound = 0; // never look back past the previous replacement
    for (std::size_t dot = s.find ('.', 0); dot != std::string::npos; dot = s.find ('.', dot + 1))
    {
        if (dot < bound || dot == 0)
            continue;
        auto extEnd = dot + 1;
        while (extEnd < s.size() && isAlnumAscii ((unsigned char) s[extEnd]) && extEnd - dot <= 9)
            ++extEnd;
        if (extEnd == dot + 1 || (extEnd < s.size() && isWordByte ((unsigned char) s[extEnd])))
            continue;
        std::string ext (s, dot + 1, extEnd - dot - 1);
        for (auto& c : ext)
            c = lowerAscii (c);
        if (! isContentExtension (ext))
            continue;
        const char before = s[dot - 1];
        if (isSeparator (before) || before == ' ' || isPathStop (before))
            continue;

        // the name: back to a separator, a stop character, the previous replacement or the line start
        auto start = dot;
        while (start > bound && ! isSeparator (s[start - 1]) && ! isPathStop (s[start - 1]) && s[start - 1] != ':' && s[start - 1] != '<' && s[start - 1] != '>')
            --start;
        const bool inPath = start > bound && isSeparator (s[start - 1]);
        if (! inPath)
        {
            // a bare name in a sentence: after one of the steps' prefixes, else the whole stretch
            std::size_t best = std::string::npos;
            for (auto prefix : namePrefixes)
            {
                std::size_t at = start, found = std::string::npos;
                while ((at = findNoCase (s, prefix, at)) != std::string::npos && at + prefix.size() <= dot)
                {
                    found = at + prefix.size();
                    ++at;
                }
                if (found != std::string::npos && (best == std::string::npos || found > best))
                    best = found;
            }
            if (best != std::string::npos)
                start = best;
            while (start < dot && (s[start] == ' ' || s[start] == ',' || s[start] == ';'))
                ++start;
        }
        if (start >= dot)
            continue;

        std::string replacement = "<file>." + std::string (s, dot + 1, extEnd - dot - 1);
        std::size_t replaceFrom = start;
        if (inPath)
        {
            // the folders of this path: common ones stay, the others become <dir>
            std::size_t rootEnd = 0;
            auto root = pathRoot (s, bound, start, rootEnd);
            if (root == std::string::npos)
            {
                rootEnd = start - 1;
                while (rootEnd > bound && ! isPathStop (s[rootEnd - 1]) && s[rootEnd - 1] != ' ')
                    --rootEnd;
            }
            std::string folders;
            std::size_t segStart = rootEnd;
            for (auto i = rootEnd; i < start; ++i)
                if (isSeparator (s[i]))
                {
                    const auto segment = std::string_view (s).substr (segStart, i - segStart);
                    folders += isCommonFolder (segment) ? std::string (segment) : std::string ("<dir>");
                    folders += s[i];
                    segStart = i + 1;
                }
            replacement = folders + replacement;
            replaceFrom = rootEnd;
        }
        s.replace (replaceFrom, extEnd - replaceFrom, replacement);
        bound = replaceFrom + replacement.size();
        dot = bound - 1;
    }
}
} // namespace

bool isContentExtension (std::string_view e)
{
    static constexpr std::string_view list[] = {
        "flp", "lynx", "fst", "fsc", "zip", "rar", "7z", "wav", "wave", "mp3", "ogg", "oga", "opus", "flac", "aif", "aiff", "aifc", "m4a",
        "aac", "wma", "wv", "caf", "mid", "midi", "rx2", "rex", "als", "ptx", "cpr", "rpp", "logicx", "song", "sf2", "sfz", "nki", "nkm",
        "mp4", "mov", "mkv", "avi", "webm", "lynxpack",
    };
    return std::find (std::begin (list), std::end (list), e) != std::end (list);
}

std::string Anonymiser::apply (std::string_view input) const
{
    std::string s = repairUtf8 (input);

    // 1. the home folder(s) as given, any slash style: the last part -> <user>
    for (auto home : homeDirs)
    {
        while (home.size() > 1 && isSeparator (home.back()))
            home.pop_back();
        const auto cut = home.find_last_of ("\\/");
        if (home.size() < 4 || cut == std::string::npos)
            continue;
        for (const char sep : { '\\', '/' })
        {
            auto h = home;
            std::replace_if (h.begin(), h.end(), isSeparator, sep);
            const auto masked = h.substr (0, cut + 1) + "<user>";
            for (auto at = findNoCase (s, h, 0); at != std::string::npos; at = findNoCase (s, h, at))
            {
                const auto end = at + h.size();
                if (end < s.size() && isWordByte ((unsigned char) s[end]))
                {
                    at = end;
                    continue;
                }
                s.replace (at + cut + 1, h.size() - cut - 1, "<user>"); // (the rest keeps its case)
                at += masked.size();
            }
        }
    }

    // 2. any account's folder in a path (other users, other drives)
    for (auto marker : { std::string_view ("\\Users\\"), std::string_view ("/Users/"), std::string_view ("/home/"),
                         std::string_view ("\\Documents and Settings\\") })
        replaceSegmentAfter (s, marker, "<user>", { "Public", "Default", "Shared", "All Users", "Default User" });
    // OneDrive for Business: "OneDrive - Company"
    replaceSegmentAfter (s, "OneDrive - ", "<org>", {});

    // 3. the names themselves anywhere (longest first, the machine before the user: "KAUAN-PC")
    std::vector<std::pair<std::string, const char*>> names;
    for (const auto& m : machineNames)
        if (m.size() >= 3)
            names.emplace_back (m, "<machine>");
    for (const auto& u : userNames)
    {
        if (u.size() >= 3)
            names.emplace_back (u, "<user>");
        // the words of a full name
        std::size_t from = 0;
        while (from < u.size())
        {
            auto to = u.find (' ', from);
            if (to == std::string::npos)
                to = u.size();
            if (to - from >= 4 && to - from < u.size())
                names.emplace_back (u.substr (from, to - from), "<user>");
            from = to + 1;
        }
    }
    std::stable_sort (names.begin(), names.end(), [] (const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    for (const auto& [name, with] : names)
        replaceWord (s, name, with);

    // 4. UNC servers: \\server\share -> \\<host>\share
    for (std::size_t at = s.find ("\\\\"); at != std::string::npos; at = s.find ("\\\\", at + 2))
    {
        if (at > 0 && (s[at - 1] == '\\' || isWordByte ((unsigned char) s[at - 1])))
            continue;
        const auto start = at + 2;
        auto end = start;
        while (end < s.size() && ! isSeparator (s[end]) && ! isStop (s[end]) && s[end] != ' ')
            ++end;
        if (end > start && std::string_view (s).substr (start, end - start) != "<host>")
            s.replace (start, end - start, "<host>");
    }

    // 5. project / audio file names and their folders
    if (! keepFileNames)
        hideFileNames (s);
    return s;
}

// ================================================================================================================
// JSON
// ================================================================================================================
std::string repairUtf8 (std::string_view in)
{
    std::string out;
    out.reserve (in.size());
    for (std::size_t i = 0; i < in.size();)
    {
        const auto c = (unsigned char) in[i];
        std::size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        bool ok = len > 0 && i + len <= in.size();
        for (std::size_t k = 1; ok && k < len; ++k)
            ok = ((unsigned char) in[i + k] >> 6) == 0x2;
        if (ok && len == 2)
            ok = c >= 0xC2; // overlong
        if (ok && len == 3)
        {
            const auto c1 = (unsigned char) in[i + 1];
            ok = ! (c == 0xE0 && c1 < 0xA0) && ! (c == 0xED && c1 >= 0xA0); // overlong / surrogates
        }
        if (ok && len == 4)
        {
            const auto c1 = (unsigned char) in[i + 1];
            ok = c <= 0xF4 && ! (c == 0xF0 && c1 < 0x90) && ! (c == 0xF4 && c1 >= 0x90);
        }
        if (ok)
        {
            out.append (in.data() + i, len);
            i += len;
        }
        else
        {
            out += "\xEF\xBF\xBD";
            ++i;
        }
    }
    return out;
}

std::string truncateUtf8 (std::string_view text, std::size_t maxBytes)
{
    if (text.size() <= maxBytes)
        return std::string (text);
    auto n = maxBytes;
    while (n > 0 && ((unsigned char) text[n] >> 6) == 0x2) // never in the middle of a character
        --n;
    return std::string (text.substr (0, n));
}

std::string jsonEscape (std::string_view utf8)
{
    const auto text = repairUtf8 (utf8);
    std::string out;
    out.reserve (text.size() + 2);
    out += '"';
    for (const char ch : text)
    {
        const auto c = (unsigned char) ch;
        switch (ch)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F)
                {
                    char buffer[8];
                    std::snprintf (buffer, sizeof (buffer), "\\u%04x", c);
                    out += buffer;
                }
                else
                    out += ch;
        }
    }
    out += '"';
    return out;
}

bool validId (std::string_view id)
{
    if (id.size() != 36)
        return false;
    for (std::size_t i = 0; i < id.size(); ++i)
    {
        const char c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23)
        {
            if (c != '-')
                return false;
        }
        else if (! ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return id[14] == '4' && (id[19] == '8' || id[19] == '9' || id[19] == 'a' || id[19] == 'b');
}

std::string formatUuid (const std::uint8_t (&b)[16])
{
    std::uint8_t v[16];
    std::copy (std::begin (b), std::end (b), v);
    v[6] = (std::uint8_t) ((v[6] & 0x0F) | 0x40); // version 4
    v[8] = (std::uint8_t) ((v[8] & 0x3F) | 0x80); // RFC 4122 variant
    std::string out;
    static constexpr char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i)
    {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out += '-';
        out += hex[v[i] >> 4];
        out += hex[v[i] & 15];
    }
    return out;
}

namespace
{
std::string number (double v)
{
    char buffer[32];
    std::snprintf (buffer, sizeof (buffer), "%.6g", v);
    return buffer;
}

// head and tail of a long text, with a marker between
std::string shorten (const std::string& text, std::size_t maxBytes)
{
    if (text.size() <= maxBytes)
        return text;
    static constexpr std::string_view marker = "\n[... cut ...]\n";
    if (maxBytes <= marker.size() + 16)
        return truncateUtf8 (text, maxBytes);
    const auto keep = maxBytes - marker.size();
    const auto head = truncateUtf8 (text, keep * 6 / 10);
    auto tailStart = text.size() - (keep - head.size());
    while (tailStart < text.size() && ((unsigned char) text[tailStart] >> 6) == 0x2)
        ++tailStart;
    return head + std::string (marker) + text.substr (tailStart);
}

struct Budget
{
    std::size_t crash = 200 * 1024, hang = 120 * 1024, steps = maxSteps, plugins = 300, user = 16 * 1024;
};

std::string build (const ReportInput& in, const Anonymiser& an, const Budget& b, const std::vector<std::string>& notes)
{
    const auto str = [&] (const std::string& s) { return jsonEscape (an.apply (s)); };
    const auto raw = [] (const std::string& s) { return jsonEscape (s); };
    std::string j = "{\n";
    j += "  \"schema\": 1,\n";
    j += "  \"id\": " + raw (in.id) + ",\n";
    j += "  \"kind\": " + raw (in.kind) + ",\n";
    j += std::string ("  \"test\": ") + (in.test ? "true" : "false") + ",\n";
    j += "  \"created\": " + raw (in.created) + ",\n";
    j += "  \"app\": {\"version\": " + raw (in.version) + ", \"build\": " + raw (in.build) + ", \"commit\": " + raw (in.commit)
       + ", \"language\": " + raw (in.language) + "},\n";

    j += "  \"system\": {\"os\": " + str (in.os) + ", \"osVersion\": " + str (in.osVersion) + ", \"cpu\": " + str (in.cpu)
       + ", \"cores\": " + std::to_string (in.cpuCores) + ", \"memoryMb\": " + std::to_string (in.memoryMb) + ",\n    \"gpus\": [";
    for (std::size_t i = 0; i < in.gpus.size() && i < 8; ++i)
        j += std::string (i ? ", " : "") + "{\"name\": " + str (in.gpus[i].name) + ", \"driver\": " + str (in.gpus[i].driver) + "}";
    j += "],\n    \"screens\": [";
    for (std::size_t i = 0; i < in.screens.size() && i < 8; ++i)
        j += std::string (i ? ", " : "") + "{\"width\": " + std::to_string (in.screens[i].width) + ", \"height\": " + std::to_string (in.screens[i].height)
           + ", \"scale\": " + number (in.screens[i].scale) + "}";
    j += "]},\n";

    j += "  \"audio\": {\"driver\": " + str (in.audioDriver) + ", \"device\": " + str (in.audioDevice) + ", \"sampleRate\": " + number (in.sampleRate)
       + ", \"bufferSize\": " + std::to_string (in.bufferSize) + "},\n";

    j += "  \"settings\": {";
    for (std::size_t i = 0; i < in.settings.size(); ++i)
        j += std::string (i ? ", " : "") + raw (in.settings[i].first) + ": " + (in.settings[i].second ? "true" : "false");
    j += "},\n";

    if (! in.crashText.empty() || ! in.crashFile.empty())
        j += "  \"crash\": {\"file\": " + str (in.crashFile) + ", \"text\": " + str (shorten (in.crashText, b.crash)) + "},\n";
    if (! in.hangText.empty() || ! in.hangFile.empty())
        j += "  \"hang\": {\"file\": " + str (in.hangFile) + ", \"text\": " + str (shorten (in.hangText, b.hang)) + "},\n";

    j += "  \"steps\": [";
    const auto firstStep = in.steps.size() > b.steps ? in.steps.size() - b.steps : 0;
    for (auto i = firstStep; i < in.steps.size(); ++i)
        j += std::string (i > firstStep ? ",\n    " : "\n    ") + str (truncateUtf8 (in.steps[i], 400));
    j += in.steps.size() > firstStep ? "\n  ],\n" : "],\n";

    j += "  \"plugins\": [";
    for (std::size_t i = 0; i < in.plugins.size() && i < b.plugins; ++i)
    {
        const auto& p = in.plugins[i];
        j += std::string (i ? ",\n    " : "\n    ") + "{\"name\": " + str (truncateUtf8 (p.name, 200)) + ", \"vendor\": " + str (truncateUtf8 (p.vendor, 200))
           + ", \"format\": " + str (truncateUtf8 (p.format, 32)) + ", \"version\": " + str (truncateUtf8 (p.version, 64)) + "}";
    }
    j += in.plugins.empty() ? "],\n" : "\n  ],\n";

    if (! in.userText.empty() || ! in.email.empty())
        j += "  \"user\": {\"text\": " + str (shorten (in.userText, b.user)) + ", \"email\": " + raw (truncateUtf8 (in.email, 200)) + "},\n";

    j += "  \"notes\": [";
    for (std::size_t i = 0; i < notes.size(); ++i)
        j += std::string (i ? ", " : "") + raw (notes[i]);
    j += "]\n}\n";
    return j;
}
} // namespace

std::string buildJson (const ReportInput& input, const Anonymiser& anonymiser, std::size_t maxBytes)
{
    Budget b;
    std::vector<std::string> notes;
    auto json = build (input, anonymiser, b, notes);
    // too large: shorten step by step, saying what was cut
    for (int round = 0; json.size() > maxBytes && round < 24; ++round)
    {
        const auto over = json.size() - maxBytes;
        if (input.crashText.size() > 4096 && b.crash > 4096)
        {
            const auto current = std::min (b.crash, input.crashText.size());
            b.crash = std::max<std::size_t> (4096, current > over + 8192 ? current - over - 1024 : current / 2);
            notes.push_back ("crash text shortened");
        }
        else if (input.hangText.size() > 4096 && b.hang > 4096)
        {
            const auto current = std::min (b.hang, input.hangText.size());
            b.hang = std::max<std::size_t> (4096, current > over + 8192 ? current - over - 1024 : current / 2);
            notes.push_back ("hang text shortened");
        }
        else if (input.userText.size() > 2048 && b.user > 2048)
        {
            b.user = std::max<std::size_t> (2048, std::min (b.user, input.userText.size()) / 2);
            notes.push_back ("user text shortened");
        }
        else if (input.plugins.size() > 20 && b.plugins > 20)
        {
            b.plugins = std::max<std::size_t> (20, std::min (b.plugins, input.plugins.size()) / 2);
            notes.push_back ("plugin list shortened");
        }
        else if (b.steps > 10)
        {
            b.steps = std::max<std::size_t> (10, b.steps / 2);
            notes.push_back ("steps shortened");
        }
        else
        {
            b.crash = b.hang = 1024;
            b.user = 512;
            b.plugins = 5;
            b.steps = 5;
            notes.push_back ("everything shortened");
            json = build (input, anonymiser, b, notes);
            break;
        }
        std::sort (notes.begin(), notes.end());
        notes.erase (std::unique (notes.begin(), notes.end()), notes.end());
        json = build (input, anonymiser, b, notes);
    }
    return json;
}

// ================================================================================================================
// GitHub
// ================================================================================================================
std::string percentEncode (std::string_view text)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve (text.size() * 3);
    for (const char ch : text)
    {
        const auto c = (unsigned char) ch;
        if (isAlnumAscii (c) || c == '-' || c == '.' || c == '_' || c == '~')
            out += ch;
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string githubOsOption (std::string_view os, std::string_view arch, bool appImage)
{
    const auto has = [&] (std::string_view what) { return findNoCase (std::string (os), what, 0) != std::string::npos; };
    if (has ("Windows 11"))
        return "Windows 11";
    if (has ("Windows 10"))
        return "Windows 10";
    if (has ("Mac") || has ("OS X"))
        return findNoCase (std::string (arch), "arm", 0) != std::string::npos ? "macOS (Apple silicon)" : "macOS (Intel)";
    if (has ("Linux"))
        return appImage ? "Linux (AppImage)" : "Linux (tar.gz)";
    return {};
}

std::string githubKindOption (std::string_view kind)
{
    if (kind == "crash" || kind == "unclean")
        return "LYNX SHARE closed (crash)";
    if (kind == "hang")
        return "LYNX SHARE froze (not responding)";
    return "Other";
}

std::string githubIssueUrl (const IssueFields& f, std::size_t maxLength)
{
    struct Field
    {
        const char* id;
        std::string value;
        bool shrinkable;
    };
    std::vector<Field> fields = { { "title", f.title, false },     { "version", f.version, false },      { "os", f.os, false },
                                  { "os-details", f.osDetails, false }, { "audio", f.audio, false },     { "kind", f.kind, false },
                                  { "steps", f.steps, true },      { "expected", f.expected, true },    { "plugins", f.plugins, true },
                                  { "logs", f.logs, true } };
    for (auto& field : fields)
        if (! field.shrinkable)
            field.value = truncateUtf8 (field.value, 300);

    const auto make = [&] {
        std::string url = std::string (issueFormBase) + "?template=bug_report.yml";
        for (const auto& field : fields)
            if (! field.value.empty())
                url += std::string ("&") + field.id + "=" + percentEncode (field.value);
        return url;
    };
    auto url = make();
    for (int guard = 0; url.size() > maxLength && guard < 64; ++guard)
    {
        // shorten the longest shrinkable field (its encoded size), by what is over plus a margin
        Field* longest = nullptr;
        std::size_t longestSize = 0;
        for (auto& field : fields)
            if (field.shrinkable && ! field.value.empty())
                if (const auto size = percentEncode (field.value).size(); size > longestSize)
                {
                    longest = &field;
                    longestSize = size;
                }
        const auto over = url.size() - maxLength;
        if (longest == nullptr || longest->value.size() <= 8)
        {
            // only the fixed fields left: cut the title and details too
            for (auto& field : fields)
                field.value = truncateUtf8 (field.value, field.value.size() / 2);
        }
        else
        {
            // encoded bytes per text byte of this field, to turn the excess into text bytes
            const auto ratio = (double) longestSize / (double) longest->value.size();
            const auto cutBytes = (std::size_t) ((double) over / ratio) + 8;
            static constexpr std::string_view ellipsis = "\n…";
            auto& v = longest->value;
            const bool hadEllipsis = v.size() >= ellipsis.size() && std::string_view (v).substr (v.size() - ellipsis.size()) == ellipsis;
            if (hadEllipsis)
                v.resize (v.size() - ellipsis.size());
            v = truncateUtf8 (v, v.size() > cutBytes + ellipsis.size() ? v.size() - cutBytes - ellipsis.size() : 0) + std::string (ellipsis);
        }
        url = make();
    }
    if (url.size() > maxLength) // (cannot happen with sane fields; never return more than allowed)
        url = std::string (issueFormBase) + "?template=bug_report.yml";
    return url;
}
} // namespace lynx::crashreport
