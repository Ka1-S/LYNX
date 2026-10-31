#include "core/UpdateCheck.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>

namespace lynx::update
{
// ================================================================================================================
// versions
// ================================================================================================================
std::string Version::text() const
{
    std::string s = std::to_string (major) + "." + std::to_string (minor) + "." + std::to_string (patch);
    for (std::size_t i = 0; i < pre.size(); ++i)
        s += (i == 0 ? "-" : ".") + pre[i];
    return s;
}

namespace
{
bool allDigits (const std::string& s)
{
    return ! s.empty() && std::all_of (s.begin(), s.end(), [] (char c) { return c >= '0' && c <= '9'; });
}

std::vector<std::string> split (const std::string& s, char by)
{
    std::vector<std::string> parts;
    std::string part;
    for (char c : s)
    {
        if (c == by)
        {
            parts.push_back (part);
            part.clear();
        }
        else
            part += c;
    }
    parts.push_back (part);
    return parts;
}

std::string trim (const std::string& s)
{
    const auto a = s.find_first_not_of (" \t\r\n");
    if (a == std::string::npos)
        return {};
    const auto b = s.find_last_not_of (" \t\r\n");
    return s.substr (a, b - a + 1);
}

std::string lower (std::string s)
{
    for (auto& c : s)
        c = (char) std::tolower ((unsigned char) c);
    return s;
}

bool endsWith (const std::string& s, const std::string& end)
{
    return s.size() >= end.size() && lower (s.substr (s.size() - end.size())) == lower (end);
}
} // namespace

Version parseVersion (const std::string& input)
{
    Version v;
    auto s = trim (input);
    if (! s.empty() && (s[0] == 'v' || s[0] == 'V'))
        s.erase (0, 1);
    if (const auto plus = s.find ('+'); plus != std::string::npos)
        s.erase (plus);
    std::string core = s, pre;
    if (const auto dash = s.find ('-'); dash != std::string::npos)
    {
        core = s.substr (0, dash);
        pre = s.substr (dash + 1);
        if (pre.empty())
            return v;
    }
    const auto numbers = split (core, '.');
    if (numbers.size() < 2 || numbers.size() > 3)
        return v;
    int values[3] = { 0, 0, 0 };
    for (std::size_t i = 0; i < numbers.size(); ++i)
    {
        if (! allDigits (numbers[i]) || numbers[i].size() > 6)
            return v;
        values[i] = std::atoi (numbers[i].c_str());
    }
    if (! pre.empty())
        for (const auto& id : split (pre, '.'))
        {
            if (id.empty() || ! std::all_of (id.begin(), id.end(), [] (char c) { return std::isalnum ((unsigned char) c) || c == '-'; }))
                return v;
            v.pre.push_back (id);
        }
    v.major = values[0];
    v.minor = values[1];
    v.patch = values[2];
    v.valid = true;
    return v;
}

int compare (const Version& a, const Version& b)
{
    for (auto [x, y] : { std::pair { a.major, b.major }, { a.minor, b.minor }, { a.patch, b.patch } })
        if (x != y)
            return x < y ? -1 : 1;
    if (a.pre.empty() || b.pre.empty())
        return a.pre.empty() == b.pre.empty() ? 0 : (a.pre.empty() ? 1 : -1);
    for (std::size_t i = 0; i < a.pre.size() && i < b.pre.size(); ++i)
    {
        const auto& x = a.pre[i];
        const auto& y = b.pre[i];
        const bool nx = allDigits (x), ny = allDigits (y);
        if (nx && ny)
        {
            const auto vx = std::strtoll (x.c_str(), nullptr, 10), vy = std::strtoll (y.c_str(), nullptr, 10);
            if (vx != vy)
                return vx < vy ? -1 : 1;
        }
        else if (nx != ny)
            return nx ? -1 : 1;
        else if (x != y)
            return x < y ? -1 : 1;
    }
    return a.pre.size() == b.pre.size() ? 0 : (a.pre.size() < b.pre.size() ? -1 : 1);
}

// ================================================================================================================
// a small JSON reader (enough for GitHub's answers)
// ================================================================================================================
namespace
{
struct Json
{
    enum class Type { null, boolean, number, string, array, object } type = Type::null;
    bool flag = false;
    double number = 0;
    std::string text;
    std::vector<Json> items;
    std::map<std::string, Json> members;

    const Json* get (const std::string& key) const
    {
        const auto it = members.find (key);
        return it != members.end() ? &it->second : nullptr;
    }
    std::string str (const std::string& key) const
    {
        const auto* j = get (key);
        return j != nullptr && j->type == Type::string ? j->text : std::string();
    }
    bool boolean (const std::string& key) const
    {
        const auto* j = get (key);
        return j != nullptr && j->type == Type::boolean && j->flag;
    }
    double num (const std::string& key) const
    {
        const auto* j = get (key);
        return j != nullptr && j->type == Type::number ? j->number : 0.0;
    }
};

class Reader
{
public:
    explicit Reader (const std::string& s) : text (s) {}

    bool parse (Json& out, std::string& error)
    {
        skip();
        if (! value (out, 0))
        {
            error = problem.empty() ? "not JSON" : problem;
            return false;
        }
        skip();
        if (at != text.size())
        {
            error = "text after the JSON value";
            return false;
        }
        return true;
    }

private:
    const std::string& text;
    std::size_t at = 0;
    std::string problem;

    void skip()
    {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r'))
            ++at;
    }

    bool fail (const char* what)
    {
        if (problem.empty())
            problem = std::string (what) + " at " + std::to_string (at);
        return false;
    }

    bool literal (const char* word)
    {
        const std::string w (word);
        if (text.compare (at, w.size(), w) != 0)
            return fail ("unknown word");
        at += w.size();
        return true;
    }

    static void appendUtf8 (std::string& out, std::uint32_t cp)
    {
        if (cp < 0x80)
            out += (char) cp;
        else if (cp < 0x800)
        {
            out += (char) (0xC0 | (cp >> 6));
            out += (char) (0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += (char) (0xE0 | (cp >> 12));
            out += (char) (0x80 | ((cp >> 6) & 0x3F));
            out += (char) (0x80 | (cp & 0x3F));
        }
        else
        {
            out += (char) (0xF0 | (cp >> 18));
            out += (char) (0x80 | ((cp >> 12) & 0x3F));
            out += (char) (0x80 | ((cp >> 6) & 0x3F));
            out += (char) (0x80 | (cp & 0x3F));
        }
    }

    bool hex4 (std::uint32_t& v)
    {
        if (at + 4 > text.size())
            return fail ("short \\u escape");
        v = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char c = text[at++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (std::uint32_t) (c - '0');
            else if (c >= 'a' && c <= 'f') v |= (std::uint32_t) (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (std::uint32_t) (c - 'A' + 10);
            else return fail ("bad \\u escape");
        }
        return true;
    }

    bool string (std::string& out)
    {
        ++at; // "
        while (at < text.size())
        {
            const char c = text[at++];
            if (c == '"')
                return true;
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (at >= text.size())
                break;
            const char e = text[at++];
            switch (e)
            {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u':
                {
                    std::uint32_t cp = 0;
                    if (! hex4 (cp))
                        return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && at + 6 <= text.size() && text[at] == '\\' && text[at + 1] == 'u')
                    {
                        at += 2;
                        std::uint32_t low = 0;
                        if (! hex4 (low))
                            return false;
                        if (low >= 0xDC00 && low <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        else
                            cp = 0xFFFD;
                    }
                    else if (cp >= 0xD800 && cp <= 0xDFFF)
                        cp = 0xFFFD;
                    appendUtf8 (out, cp);
                    break;
                }
                default: return fail ("bad escape");
            }
        }
        return fail ("unterminated string");
    }

    bool value (Json& out, int depth)
    {
        if (depth > 64)
            return fail ("nested too deep");
        if (at >= text.size())
            return fail ("unexpected end");
        const char c = text[at];
        if (c == '{')
        {
            out.type = Json::Type::object;
            ++at;
            skip();
            if (at < text.size() && text[at] == '}')
            {
                ++at;
                return true;
            }
            while (true)
            {
                skip();
                if (at >= text.size() || text[at] != '"')
                    return fail ("expected a key");
                std::string key;
                if (! string (key))
                    return false;
                skip();
                if (at >= text.size() || text[at] != ':')
                    return fail ("expected ':'");
                ++at;
                skip();
                Json member;
                if (! value (member, depth + 1))
                    return false;
                out.members[key] = std::move (member);
                skip();
                if (at < text.size() && text[at] == ',')
                {
                    ++at;
                    continue;
                }
                if (at < text.size() && text[at] == '}')
                {
                    ++at;
                    return true;
                }
                return fail ("expected ',' or '}'");
            }
        }
        if (c == '[')
        {
            out.type = Json::Type::array;
            ++at;
            skip();
            if (at < text.size() && text[at] == ']')
            {
                ++at;
                return true;
            }
            while (true)
            {
                skip();
                Json item;
                if (! value (item, depth + 1))
                    return false;
                out.items.push_back (std::move (item));
                skip();
                if (at < text.size() && text[at] == ',')
                {
                    ++at;
                    continue;
                }
                if (at < text.size() && text[at] == ']')
                {
                    ++at;
                    return true;
                }
                return fail ("expected ',' or ']'");
            }
        }
        if (c == '"')
        {
            out.type = Json::Type::string;
            return string (out.text);
        }
        if (c == 't' || c == 'f')
        {
            out.type = Json::Type::boolean;
            out.flag = c == 't';
            return literal (c == 't' ? "true" : "false");
        }
        if (c == 'n')
            return literal ("null");
        if (c == '-' || (c >= '0' && c <= '9'))
        {
            const auto start = at;
            while (at < text.size() && (std::isdigit ((unsigned char) text[at]) || text[at] == '-' || text[at] == '+' || text[at] == '.' || text[at] == 'e' || text[at] == 'E'))
                ++at;
            out.type = Json::Type::number;
            out.number = std::strtod (text.substr (start, at - start).c_str(), nullptr);
            return true;
        }
        return fail ("unexpected character");
    }
};

Release toRelease (const Json& j)
{
    Release r;
    r.tag = j.str ("tag_name");
    r.name = j.str ("name");
    r.body = j.str ("body");
    r.page = j.str ("html_url");
    r.draft = j.boolean ("draft");
    r.prerelease = j.boolean ("prerelease");
    r.version = parseVersion (r.tag);
    if (const auto* assets = j.get ("assets"); assets != nullptr && assets->type == Json::Type::array)
        for (const auto& a : assets->items)
            if (a.type == Json::Type::object)
            {
                Asset asset;
                asset.name = a.str ("name");
                asset.url = a.str ("browser_download_url");
                asset.size = (std::int64_t) a.num ("size");
                if (! asset.name.empty() && ! asset.url.empty())
                    r.assets.push_back (std::move (asset));
            }
    return r;
}
} // namespace

std::vector<Release> parseReleases (const std::string& json, std::string* error)
{
    std::vector<Release> releases;
    Json root;
    std::string problem;
    if (! Reader (json).parse (root, problem))
    {
        if (error != nullptr)
            *error = problem;
        return releases;
    }
    std::vector<const Json*> list;
    if (root.type == Json::Type::array)
        for (const auto& item : root.items)
            list.push_back (&item);
    else if (root.type == Json::Type::object)
    {
        if (root.get ("tag_name") == nullptr)
        {
            // GitHub's error answer: {"message": "API rate limit exceeded ..."}
            if (error != nullptr)
                *error = root.str ("message").empty() ? std::string ("no releases in the answer") : root.str ("message");
            return releases;
        }
        list.push_back (&root);
    }
    else if (error != nullptr)
        *error = "no releases in the answer";
    for (const auto* item : list)
        if (item->type == Json::Type::object)
            if (auto r = toRelease (*item); r.version.valid)
                releases.push_back (std::move (r));
    return releases;
}

int newestIndex (const std::vector<Release>& releases, const Version& running)
{
    int best = -1;
    for (int i = 0; i < (int) releases.size(); ++i)
    {
        const auto& r = releases[(std::size_t) i];
        if (r.draft || ! r.version.valid || compare (r.version, running) <= 0)
            continue;
        if (best < 0 || compare (r.version, releases[(std::size_t) best].version) > 0)
            best = i;
    }
    return best;
}

// ================================================================================================================
// flavours
// ================================================================================================================
const char* flavourId (Flavour f)
{
    switch (f)
    {
        case Flavour::windowsInstaller: return "installer";
        case Flavour::windowsPortable: return "portable";
        case Flavour::store: return "store";
        case Flavour::macos: return "macos";
        case Flavour::linuxAppImage: return "appimage";
        case Flavour::linuxTarball: return "tarball";
        case Flavour::unknown: break;
    }
    return "unknown";
}

Flavour flavourFromId (const std::string& id)
{
    for (auto f : { Flavour::windowsInstaller, Flavour::windowsPortable, Flavour::store, Flavour::macos, Flavour::linuxAppImage, Flavour::linuxTarball })
        if (lower (trim (id)) == flavourId (f))
            return f;
    return Flavour::unknown;
}

Flavour detectFlavour (const Facts& facts)
{
    if (const auto forced = flavourFromId (facts.forced); forced != Flavour::unknown)
        return forced;
    if (facts.os == "windows")
        return facts.packaged ? Flavour::store : facts.uninstallerBesideExe ? Flavour::windowsInstaller : Flavour::windowsPortable;
    if (facts.os == "macos")
        return Flavour::macos;
    if (facts.os == "linux")
        return facts.appImagePath.empty() ? Flavour::linuxTarball : Flavour::linuxAppImage;
    return Flavour::unknown;
}

bool checksForUpdates (Flavour f) { return f != Flavour::store && f != Flavour::unknown; }

Method methodFor (Flavour f)
{
    switch (f)
    {
        case Flavour::windowsInstaller: return Method::runInstaller;
        case Flavour::windowsPortable:
        case Flavour::macos: return Method::downloadAndReveal;
        case Flavour::linuxAppImage: return Method::appImageUpdate;
        case Flavour::linuxTarball: return Method::openPage;
        case Flavour::store:
        case Flavour::unknown: break;
    }
    return Method::none;
}

int assetFor (Flavour f, const std::vector<Asset>& assets)
{
    const char* suffix = nullptr;
    switch (f)
    {
        case Flavour::windowsInstaller: suffix = "-setup-x64.exe"; break;
        case Flavour::windowsPortable: suffix = "-windows-x64.zip"; break;
        case Flavour::macos: suffix = ".dmg"; break;
        case Flavour::linuxAppImage: suffix = "-x86_64.AppImage"; break;
        case Flavour::linuxTarball: suffix = "-linux-x86_64.tar.gz"; break;
        case Flavour::store:
        case Flavour::unknown: return -1;
    }
    for (int i = 0; i < (int) assets.size(); ++i)
        if (endsWith (assets[(std::size_t) i].name, suffix) && lower (assets[(std::size_t) i].name).rfind ("lynx", 0) == 0)
            return i;
    return -1;
}

int checksumsAsset (const std::vector<Asset>& assets)
{
    for (int i = 0; i < (int) assets.size(); ++i)
        if (lower (assets[(std::size_t) i].name) == "sha256sums.txt")
            return i;
    return -1;
}

// ================================================================================================================
// checksums
// ================================================================================================================
namespace
{
bool isHex64 (const std::string& s)
{
    return s.size() == 64 && std::all_of (s.begin(), s.end(), [] (char c) { return std::isxdigit ((unsigned char) c) != 0; });
}
} // namespace

std::string checksumFor (const std::string& sumsText, const std::string& fileName)
{
    std::istringstream in (sumsText);
    std::string line;
    while (std::getline (in, line))
    {
        line = trim (line);
        const auto space = line.find_first_of (" \t");
        if (space == std::string::npos)
            continue;
        const auto hash = line.substr (0, space);
        auto name = trim (line.substr (space));
        if (! name.empty() && name[0] == '*') // sha256sum -b
            name.erase (0, 1);
        if (name.rfind ("./", 0) == 0)
            name.erase (0, 2);
        if (name == fileName && isHex64 (hash))
            return lower (hash);
    }
    return {};
}

Sha256Stream::Sha256Stream()
{
    static constexpr std::uint32_t init[8] { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    std::memcpy (h, init, sizeof (h));
}

void Sha256Stream::block (const std::uint8_t* p)
{
    static constexpr std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
    auto rotr = [] (std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (std::uint32_t) p[4 * i] << 24 | (std::uint32_t) p[4 * i + 1] << 16 | (std::uint32_t) p[4 * i + 2] << 8 | (std::uint32_t) p[4 * i + 3];
    for (int i = 16; i < 64; ++i)
    {
        const auto s0 = rotr (w[i - 15], 7) ^ rotr (w[i - 15], 18) ^ (w[i - 15] >> 3);
        const auto s1 = rotr (w[i - 2], 17) ^ rotr (w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i)
    {
        const auto t1 = hh + (rotr (e, 6) ^ rotr (e, 11) ^ rotr (e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        const auto t2 = (rotr (a, 2) ^ rotr (a, 13) ^ rotr (a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

void Sha256Stream::add (const void* data, std::size_t size)
{
    auto* p = static_cast<const std::uint8_t*> (data);
    total += size;
    while (size > 0)
    {
        if (pendingSize == 0 && size >= 64)
        {
            block (p);
            p += 64;
            size -= 64;
            continue;
        }
        const auto n = std::min (size, 64 - pendingSize);
        std::memcpy (pending + pendingSize, p, n);
        pendingSize += n;
        p += n;
        size -= n;
        if (pendingSize == 64)
        {
            block (pending);
            pendingSize = 0;
        }
    }
}

std::string Sha256Stream::finish()
{
    const auto bits = total * 8;
    const std::uint8_t one = 0x80, zero = 0;
    add (&one, 1);
    while (pendingSize != 56)
        add (&zero, 1);
    std::uint8_t length[8];
    for (int i = 0; i < 8; ++i)
        length[i] = (std::uint8_t) (bits >> (8 * (7 - i)));
    add (length, 8);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (auto v : h)
        for (int i = 7; i >= 0; --i)
            out += hex[(v >> (4 * i)) & 15];
    return out;
}

bool checksumMatches (const std::string& expected, const std::string& actual)
{
    return isHex64 (expected) && isHex64 (actual) && lower (expected) == lower (actual);
}

// ================================================================================================================
// release notes
// ================================================================================================================
namespace
{
// inline Markdown to text: ![alt](url) goes, [text](url) keeps the text, <tags> go, ** __ ` go, \* escapes
std::string inlineText (const std::string& s)
{
    // *emphasis* / _emphasis_: the pairs of single marks around words go (a lone '*' or snake_case stays)
    std::vector<bool> drop (s.size(), false);
    for (char mark : { '*', '_' })
        for (std::size_t i = 0; i < s.size(); ++i)
        {
            const bool doubled = (i + 1 < s.size() && s[i + 1] == mark) || (i > 0 && s[i - 1] == mark);
            if (s[i] != mark || doubled || (i > 0 && s[i - 1] == '\\') || i + 1 >= s.size() || std::isspace ((unsigned char) s[i + 1]))
                continue;
            if (mark == '_' && i > 0 && std::isalnum ((unsigned char) s[i - 1]))
                continue;
            for (std::size_t j = i + 2; j < s.size(); ++j)
                if (s[j] == mark && ! std::isspace ((unsigned char) s[j - 1]) && s[j - 1] != '\\'
                    && ! (j + 1 < s.size() && s[j + 1] == mark)
                    && (mark == '*' || j + 1 >= s.size() || ! std::isalnum ((unsigned char) s[j + 1])))
                {
                    drop[i] = drop[j] = true;
                    i = j;
                    break;
                }
        }
    std::string out;
    for (std::size_t i = 0; i < s.size();)
    {
        if (drop[i])
        {
            ++i;
            continue;
        }
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size() && std::ispunct ((unsigned char) s[i + 1]))
        {
            out += s[i + 1];
            i += 2;
            continue;
        }
        if (c == '!' && i + 1 < s.size() && s[i + 1] == '[')
        {
            const auto close = s.find ("](", i + 2);
            const auto end = close != std::string::npos ? s.find (')', close + 2) : std::string::npos;
            if (end != std::string::npos)
            {
                i = end + 1;
                continue;
            }
        }
        if (c == '[')
        {
            const auto close = s.find ("](", i + 1);
            const auto end = close != std::string::npos ? s.find (')', close + 2) : std::string::npos;
            if (end != std::string::npos && s.find ('\n', i) > end)
            {
                out += inlineText (s.substr (i + 1, close - i - 1));
                i = end + 1;
                continue;
            }
        }
        if (c == '<')
        {
            const auto end = s.find ('>', i + 1);
            if (end != std::string::npos && end > i + 1 && (std::isalpha ((unsigned char) s[i + 1]) || s[i + 1] == '/' || s[i + 1] == '!'))
            {
                const auto inner = s.substr (i + 1, end - i - 1);
                if (inner.rfind ("http", 0) == 0) // <https://...> autolink: keep the address
                    out += inner;
                i = end + 1;
                continue;
            }
        }
        if ((c == '*' || c == '_') && i + 1 < s.size() && s[i + 1] == c)
        {
            i += 2;
            continue;
        }
        if (c == '`')
        {
            ++i;
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}
} // namespace

std::vector<NoteLine> releaseNotes (const std::string& markdown)
{
    std::vector<NoteLine> lines;
    auto push = [&lines] (NoteLine line) {
        if (line.kind == NoteLine::Kind::text && trim (line.text).empty())
            line.kind = NoteLine::Kind::blank; // e.g. a line of HTML only (a centred logo)
        if (line.kind == NoteLine::Kind::blank && (lines.empty() || lines.back().kind == NoteLine::Kind::blank))
            return;
        lines.push_back (std::move (line));
    };
    std::istringstream in (markdown);
    std::string raw;
    bool fence = false;
    bool paragraph = false, bullet = false; // the last line was a paragraph's / a bullet's (the next one may join it)
    while (std::getline (in, raw))
    {
        const bool joinParagraph = paragraph, joinBullet = bullet;
        paragraph = bullet = false;
        if (! raw.empty() && raw.back() == '\r')
            raw.pop_back();
        const auto t = trim (raw);
        if (t.rfind ("```", 0) == 0 || t.rfind ("~~~", 0) == 0)
        {
            fence = ! fence;
            continue;
        }
        if (fence)
        {
            push ({ NoteLine::Kind::text, 0, raw });
            continue;
        }
        if (t.empty())
        {
            push ({ NoteLine::Kind::blank, 0, {} });
            continue;
        }
        if (t == "---" || t == "***" || t == "___" || (t.find_first_not_of ("-| :") == std::string::npos && t.find ('-') != std::string::npos))
        {
            push ({ NoteLine::Kind::blank, 0, {} }); // a rule, or a table's separator row
            continue;
        }
        if (t[0] == '#')
        {
            const auto level = (int) std::min<std::size_t> (6, t.find_first_not_of ('#'));
            auto text = trim (inlineText (t.substr ((std::size_t) level)));
            while (! text.empty() && text.back() == '#')
                text.pop_back();
            push ({ NoteLine::Kind::blank, 0, {} }); // (dropped before the first line)
            push ({ NoteLine::Kind::heading, level, trim (text) });
            continue;
        }
        const auto indent = raw.find_first_not_of (" \t");
        if ((t[0] == '-' || t[0] == '*' || t[0] == '+') && t.size() > 1 && t[1] == ' ')
        {
            push ({ NoteLine::Kind::bullet, (int) std::min<std::size_t> (3, indent / 2), trim (inlineText (t.substr (2))) });
            bullet = true;
            continue;
        }
        if (const auto dot = t.find (". "); dot != std::string::npos && dot <= 3 && allDigits (t.substr (0, dot)))
        {
            push ({ NoteLine::Kind::bullet, (int) std::min<std::size_t> (3, indent / 2), trim (inlineText (t)) });
            bullet = true;
            continue;
        }
        auto text = t;
        if (text[0] == '>')
        {
            text = trim (text.substr (1));
            // a GitHub alert marker ("> [!IMPORTANT]") is not text; an empty quote line ends the paragraph
            if (text.empty() || (text.size() > 3 && text.compare (0, 2, "[!") == 0 && text.back() == ']'))
                continue;
        }
        if (text.front() == '|' && text.back() == '|') // a table row: cells apart
        {
            std::string row;
            for (const auto& cell : split (text.substr (1, text.size() - 2), '|'))
                row += (row.empty() ? "" : "  ·  ") + trim (inlineText (cell));
            push ({ NoteLine::Kind::text, 0, row });
            continue;
        }
        // a paragraph's next line joins the previous text line (Markdown wraps them)
        if (joinParagraph && ! lines.empty())
        {
            lines.back().text += " " + inlineText (text);
            paragraph = raw.size() < 2 || raw.compare (raw.size() - 2, 2, "  ") != 0; // (two spaces: a hard break)
            continue;
        }
        if (joinBullet && ! lines.empty() && indent >= 2)
        {
            lines.back().text += " " + inlineText (text); // a bullet's continuation line
            bullet = true;
            continue;
        }
        push ({ NoteLine::Kind::text, 0, inlineText (text) });
        paragraph = raw.size() < 2 || raw.compare (raw.size() - 2, 2, "  ") != 0;
    }
    while (! lines.empty() && lines.back().kind == NoteLine::Kind::blank)
        lines.pop_back();
    if (! lines.empty() && lines.front().kind == NoteLine::Kind::blank)
        lines.erase (lines.begin());
    return lines;
}

std::string releaseNotesText (const std::vector<NoteLine>& lines)
{
    std::string out;
    for (const auto& l : lines)
    {
        switch (l.kind)
        {
            case NoteLine::Kind::heading: out += l.text; break;
            case NoteLine::Kind::bullet: out += std::string ((std::size_t) l.level * 4, ' ') + "\xE2\x80\xA2 " + l.text; break;
            case NoteLine::Kind::text: out += l.text; break;
            case NoteLine::Kind::blank: break;
        }
        out += "\n";
    }
    return out;
}

// ================================================================================================================
// settings
// ================================================================================================================
Settings parseSettings (const std::string& text)
{
    Settings s;
    std::istringstream in (text);
    std::string line;
    while (std::getline (in, line))
    {
        const auto eq = line.find ('=');
        if (eq == std::string::npos)
            continue;
        const auto key = trim (line.substr (0, eq)), value = trim (line.substr (eq + 1));
        if (key == "automatic")
            s.automatic = ! (value == "0" || value == "false" || value == "no" || value == "off");
        else if (key == "skipped")
            s.skipped = parseVersion (value).valid ? value : std::string();
        else if (key == "lastCheck")
            s.lastCheckMs = allDigits (value) ? std::strtoll (value.c_str(), nullptr, 10) : 0;
    }
    return s;
}

std::string formatSettings (const Settings& s)
{
    std::string out = "automatic=" + std::string (s.automatic ? "1" : "0") + "\n";
    if (! s.skipped.empty())
        out += "skipped=" + s.skipped + "\n";
    if (s.lastCheckMs > 0)
        out += "lastCheck=" + std::to_string (s.lastCheckMs) + "\n";
    return out;
}

bool shouldShow (const Release& release, const Settings& settings, bool manual)
{
    if (manual)
        return true;
    const auto skipped = parseVersion (settings.skipped);
    return ! (skipped.valid && compare (release.version, skipped) <= 0);
}

bool cacheFresh (std::int64_t lastCheckMs, std::int64_t nowMs)
{
    return lastCheckMs > 0 && nowMs >= lastCheckMs && nowMs - lastCheckMs < cacheLifetimeMs;
}
} // namespace lynx::update
