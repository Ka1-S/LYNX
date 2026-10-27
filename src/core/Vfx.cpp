#include "core/Vfx.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>

#include "core/FlSynths.h"
#include "core/Song.h"
#include "core/VfxDetail.h"
#include "flp/Flp.h"

namespace lynx::vfx
{
using detail::Handoff;
using detail::Reader;
using detail::put;

namespace
{
constexpr std::uint32_t lynxMagic = 0x5846564c; // "LVFX"
} // namespace

int inputId (int key, int colour) noexcept { return (colour & 15) * 128 + (key & 127); }

Event programEvent (int program, int offset) noexcept
{
    Event e;
    e.type = Event::Type::program;
    e.offset = offset;
    e.value = (float) std::clamp (program, 0, 127);
    return e;
}

// ---------------------------------------------------------------------------------------------------------------

const char* flName (Kind kind) noexcept
{
    switch (kind)
    {
        case Kind::keyMapper: return "VFX Key Mapper";
        case Kind::colorMapper: return "VFX Color Mapper";
        case Kind::envelope: return "VFX Envelope";
        case Kind::keyboardSplitter: return "VFX Keyboard Splitter";
        case Kind::levelScaler: return "VFX Level Scaler";
        case Kind::sequencer: return "VFX Sequencer";
        case Kind::script: return "VFX Script";
    }
    return "VFX";
}

std::optional<Kind> kindOfFlName (std::string_view name) noexcept
{
    auto same = [] (std::string_view a, std::string_view b) {
        return a.size() == b.size() && std::equal (a.begin(), a.end(), b.begin(), [] (char x, char y) {
                   return std::tolower ((unsigned char) x) == std::tolower ((unsigned char) y);
               });
    };
    for (const auto k : allKinds)
        if (same (name, flName (k)))
            return k;
    return std::nullopt;
}

void EventList::sortByOffset() noexcept
{
    // stable insertion sort in place (std::stable_sort may allocate; the events arrive nearly in order); an "off"
    // goes before an "on" on the same sample
    const auto before = [] (const Event& a, const Event& b) {
        if (a.offset != b.offset)
            return a.offset < b.offset;
        return a.type == Event::Type::off && b.type != Event::Type::off;
    };
    for (int i = 1; i < count; ++i)
    {
        const auto e = events[(std::size_t) i];
        auto j = i;
        for (; j > 0 && before (e, events[(std::size_t) j - 1]); --j)
            events[(std::size_t) j] = events[(std::size_t) j - 1];
        events[(std::size_t) j] = e;
    }
}

void EventList::removeFirst (int n) noexcept
{
    n = std::clamp (n, 0, count);
    std::move (events.begin() + n, events.begin() + count, events.begin());
    count -= n;
}

// ---------------------------------------------------------------------------------------------------------------
// VFX Key Mapper

KeyMapperSettings defaultKeyMapper()
{
    KeyMapperSettings s;
    for (int i = 0; i < 12; ++i)
        s.rows[(std::size_t) i] = { i };
    return s;
}

std::optional<KeyMapperSettings> parseKeyMapper (const std::vector<std::uint8_t>& fl)
{
    Reader r { fl.data(), fl.size() };
    KeyMapperSettings s;
    s.version = r.i32();
    if (r.failed || s.version < 1 || s.version > 16)
        return std::nullopt;
    s.enabled = r.i32() != 0;
    s.baseKey = r.i32();
    s.offset = r.i32();
    s.multimode = r.i32();
    s.slide = r.i32();
    for (auto& row : s.rows)
    {
        const auto n = r.i32();
        if (r.failed || n < 0 || n > 128)
            return std::nullopt;
        row.clear();
        for (int k = 0; k < n; ++k)
            row.push_back (r.get<std::int16_t>());
    }
    if (r.failed)
        return std::nullopt;
    s.extra.assign (fl.begin() + (std::ptrdiff_t) r.pos, fl.end());
    return s;
}

std::vector<std::uint8_t> writeKeyMapper (const KeyMapperSettings& s)
{
    std::vector<std::uint8_t> out;
    put<std::int32_t> (out, s.version);
    put<std::int32_t> (out, s.enabled ? 1 : 0);
    put<std::int32_t> (out, s.baseKey);
    put<std::int32_t> (out, s.offset);
    put<std::int32_t> (out, s.multimode);
    put<std::int32_t> (out, s.slide);
    for (const auto& row : s.rows)
    {
        put<std::int32_t> (out, (std::int32_t) row.size());
        for (const auto v : row)
            put<std::int16_t> (out, (std::int16_t) v);
    }
    out.insert (out.end(), s.extra.begin(), s.extra.end());
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// VFX Color Mapper

ColorMapperSettings defaultColorMapper()
{
    ColorMapperSettings s;
    for (int i = 0; i < 16; ++i)
        s.port[(std::size_t) i] = s.colour[(std::size_t) i] = i;
    return s;
}

std::optional<ColorMapperSettings> parseColorMapper (const std::vector<std::uint8_t>& fl)
{
    Reader r { fl.data(), fl.size() };
    ColorMapperSettings s;
    s.header = r.u32();
    for (auto& p : s.port)
        p = std::clamp (r.i32(), 0, 15);
    for (auto& c : s.colour)
        c = std::clamp (r.i32(), 0, 15);
    if (r.failed)
        return std::nullopt;
    return s;
}

std::vector<std::uint8_t> writeColorMapper (const ColorMapperSettings& s)
{
    std::vector<std::uint8_t> out;
    put<std::uint32_t> (out, s.header);
    for (const auto p : s.port)
        put<std::int32_t> (out, p);
    for (const auto c : s.colour)
        put<std::int32_t> (out, c);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// VFX Level Scaler (REA_rodada164b §Level Scaler)

bool LevelScalerSettings::identity (int tab) const noexcept
{
    return at (tab, multiply) == 8192 && at (tab, offset) == 0 && at (tab, tension) == 0 && at (tab, humanize) == 0;
}

void resetLevelScalerTab (LevelScalerSettings& s, int tab)
{
    // the editor's Reset: bipolar properties are centred
    s.at (tab, LevelScalerSettings::center) = tab >= LevelScalerSettings::finePitch ? 4096 : 0;
    s.at (tab, LevelScalerSettings::multiply) = 8192;
    s.at (tab, LevelScalerSettings::offset) = 0;
    s.at (tab, LevelScalerSettings::tension) = 0;
    s.at (tab, LevelScalerSettings::humanize) = 0;
    s.at (tab, LevelScalerSettings::humanizeMode) = 1;
}

LevelScalerSettings defaultLevelScaler()
{
    LevelScalerSettings s;
    for (int t = 0; t < LevelScalerSettings::tabCount; ++t)
        resetLevelScalerTab (s, t);
    return s;
}

std::optional<LevelScalerSettings> parseLevelScaler (const std::vector<std::uint8_t>& fl)
{
    Reader r { fl.data(), fl.size() };
    auto s = defaultLevelScaler();
    s.version = r.u32();
    if (r.failed)
        return std::nullopt;
    if (s.version >= 2)
        return defaultLevelScaler(); // (FL ignores a state it does not know)
    // chunks: id, 64-bit size, data (unknown ids skipped)
    while (r.left() >= 12)
    {
        const auto id = r.u32();
        const auto size = r.get<std::uint64_t>();
        if (size > r.left())
            return std::nullopt;
        const auto end = r.pos + (std::size_t) size;
        if (id == 0x3ea && size >= 1)
            s.selectedTab = std::clamp ((int) r.get<std::uint8_t>(), 0, 5);
        else if (id == 0x3e9 && size >= 144)
        {
            static constexpr std::array<std::pair<int, int>, 6> range { { { 0, 8192 }, { -16384, 16384 }, { -8192, 8192 }, { -8192, 8192 }, { 0, 16384 }, { 0, 2 } } };
            for (int i = 0; i < 36; ++i)
                s.p[(std::size_t) i] = std::clamp (r.i32(), range[(std::size_t) (i % 6)].first, range[(std::size_t) (i % 6)].second);
        }
        r.pos = end;
    }
    if (r.failed)
        return std::nullopt;
    return s;
}

std::vector<std::uint8_t> writeLevelScaler (const LevelScalerSettings& s)
{
    std::vector<std::uint8_t> out;
    put<std::uint32_t> (out, 1);
    put<std::uint32_t> (out, 0x3ea);
    put<std::uint64_t> (out, 1);
    put<std::uint8_t> (out, (std::uint8_t) std::clamp (s.selectedTab, 0, 5));
    put<std::uint32_t> (out, 0x3e9);
    put<std::uint64_t> (out, 144);
    for (const auto v : s.p)
        put<std::int32_t> (out, v);
    return out;
}

double levelScale (const LevelScalerSettings& s, int tab, double x, double lo, double hi, double random) noexcept
{
    using L = LevelScalerSettings;
    if (s.identity (tab) || ! (hi > lo))
        return x;
    const auto c = s.at (tab, L::center) / 8192.0;
    const auto scale = 1.0 / std::max (c, 1.0 - c);
    const auto tension = s.at (tab, L::tension);
    const auto g = std::pow (8193.0, std::abs (tension) / 8192.0) - 1.0;
    const auto mul = s.at (tab, L::multiply) / (8192.0 * scale);
    const auto off = s.at (tab, L::offset) / 8192.0 + c;
    const auto u = (x - lo) / (hi - lo);
    const auto d = u - c;
    const auto m = std::min (1.0, std::abs (d) * scale);
    auto shape = m;
    if (tension > 0 && g > 0.0)
        shape = 1.0 - (std::pow (g + 1.0, 1.0 - m) - 1.0) / g;
    else if (tension < 0 && g > 0.0)
        shape = (std::pow (g + 1.0, m) - 1.0) / g;
    const auto y = std::clamp ((d < 0.0 ? -shape : shape) * mul + off, 0.0, 1.0);
    auto out = std::clamp (lo + y * (hi - lo), lo, hi);
    if (const auto h = s.at (tab, L::humanize) / 16384.0; h > 0.0)
    {
        const auto mode = s.at (tab, L::humanizeMode);
        const auto a = mode == 2 ? out : out - (out - lo) * h;
        const auto b = mode == 0 ? out : out + (hi - out) * h;
        out = a + std::clamp (random, 0.0, 1.0) * (b - a);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// FL's mapping curves and VFX Keyboard Splitter (REA_rodada164b §Keyboard Splitter)

MapCurve MapCurve::flat (double level)
{
    MapCurve m;
    m.points.push_back ({ 0.0, level });
    m.points.push_back ({ 1.0, level });
    return m;
}

double MapCurve::at (double x) const noexcept
{
    if (points.empty())
        return 0.0;
    if (x <= points.front().x)
        return points.front().y;
    for (std::size_t i = 1; i < points.size(); ++i)
        if (x < points[i].x)
        {
            const auto& a = points[i - 1];
            const auto& b = points[i];
            if (b.curve == 2) // hold: the previous level up to this point
                return a.y;
            auto f = b.x > a.x ? (x - a.x) / (b.x - a.x) : 1.0;
            if (b.tension != 0.0f) // (the automation clips' bend, FL's sign)
                f = std::pow (f, std::pow (2.0, (double) -b.tension * 3.0));
            return a.y + (b.y - a.y) * f;
        }
    return points.back().y;
}

bool MapCurve::isFlat (double level) const noexcept
{
    return std::all_of (points.begin(), points.end(), [level] (const Point& p) { return std::abs (p.y - level) < 1e-9; });
}

namespace
{
bool readMap (Reader& r, MapCurve& m)
{
    r.i32(); // kind (3)
    const auto version = r.i32();
    const auto n = r.i32();
    if (r.failed || n < 0 || n > 4096 || version < 2 || version > 3)
        return false;
    m.points.clear();
    double x = 0.0;
    for (int k = 0; k < n; ++k)
    {
        MapCurve::Point p;
        const auto dx = r.get<double>();
        x += std::isfinite (dx) ? dx : 0.0;
        p.x = x;
        p.y = r.get<double>();
        if (! std::isfinite (p.y))
            p.y = 0.0;
        p.tension = r.get<float>();
        if (! std::isfinite (p.tension))
            p.tension = 0.0f;
        p.curve = r.get<std::uint8_t>();
        p.b13 = r.get<std::uint8_t>();
        p.b14 = r.get<std::uint8_t>();
        p.b15 = r.get<std::uint8_t>();
        m.points.push_back (p);
    }
    for (auto& f : m.footer)
        f = r.i32();
    return ! r.failed;
}

void writeMap (std::vector<std::uint8_t>& out, const MapCurve& m, int kind = 3)
{
    put<std::int32_t> (out, kind);
    put<std::int32_t> (out, 3);
    put<std::int32_t> (out, (std::int32_t) m.points.size());
    double x = 0.0;
    for (const auto& p : m.points)
    {
        put<double> (out, p.x - x);
        x = p.x;
        put<double> (out, p.y);
        put<float> (out, p.tension);
        put<std::uint8_t> (out, p.curve);
        put<std::uint8_t> (out, p.b13);
        put<std::uint8_t> (out, p.b14);
        put<std::uint8_t> (out, p.b15); // (FL's shape flag: 2 straight, 1 / 0xff bent up / down)
    }
    for (const auto f : m.footer)
        put<std::int32_t> (out, f);
}
} // namespace

KeyboardSplitterSettings defaultKeyboardSplitter()
{
    KeyboardSplitterSettings s;
    s.zones[0].keyboard = MapCurve::flat (1.0); // zone 1: the whole keyboard
    return s;
}

std::optional<KeyboardSplitterSettings> parseKeyboardSplitter (const std::vector<std::uint8_t>& fl)
{
    Reader r { fl.data(), fl.size() };
    KeyboardSplitterSettings s;
    s.header = r.u32();
    if (r.failed || s.header >= 0x100004 || (s.header & 0xfffff) == 0)
        return std::nullopt;
    if ((s.header >> 20) != 0)
        s.page = std::clamp ((int) r.get<std::uint8_t>(), 0, 1);
    for (auto& z : s.zones)
    {
        if (! readMap (r, z.keyboard) || ! readMap (r, z.velocity))
            return std::nullopt;
        z.transpose = std::clamp (r.i32(), -48, 48);
        z.absolute = r.get<std::uint8_t>() != 0;
        z.random = r.get<std::uint8_t>() != 0;
        // name: u8 length (255: a u32 follows), UTF-16 characters
        std::uint32_t length = r.get<std::uint8_t>();
        if (length == 255)
            length = r.u32();
        if (r.failed || length > 4096 || length * 2 > r.left())
            return std::nullopt;
        std::u16string name;
        for (std::uint32_t k = 0; k < length; ++k)
            name.push_back ((char16_t) r.get<std::uint16_t>());
        z.name.clear();
        for (std::size_t k = 0; k < name.size(); ++k)
        {
            std::uint32_t c = name[k];
            if (c >= 0xd800 && c < 0xdc00 && k + 1 < name.size())
                c = 0x10000 + ((c - 0xd800) << 10) + ((std::uint32_t) name[++k] - 0xdc00);
            if (c < 0x80)
                z.name.push_back ((char) c);
            else if (c < 0x800)
                z.name += { (char) (0xc0 | (c >> 6)), (char) (0x80 | (c & 0x3f)) };
            else if (c < 0x10000)
                z.name += { (char) (0xe0 | (c >> 12)), (char) (0x80 | ((c >> 6) & 0x3f)), (char) (0x80 | (c & 0x3f)) };
            else
                z.name += { (char) (0xf0 | (c >> 18)), (char) (0x80 | ((c >> 12) & 0x3f)), (char) (0x80 | ((c >> 6) & 0x3f)), (char) (0x80 | (c & 0x3f)) };
        }
    }
    if (r.failed)
        return std::nullopt;
    return s;
}

std::vector<std::uint8_t> writeKeyboardSplitter (const KeyboardSplitterSettings& s)
{
    std::vector<std::uint8_t> out;
    put<std::uint32_t> (out, 0x100003);
    put<std::uint8_t> (out, (std::uint8_t) std::clamp (s.page, 0, 1));
    for (const auto& z : s.zones)
    {
        writeMap (out, z.keyboard);
        writeMap (out, z.velocity);
        put<std::int32_t> (out, z.transpose);
        put<std::uint8_t> (out, z.absolute ? 1 : 0);
        put<std::uint8_t> (out, z.random ? 1 : 0);
        std::u16string name;
        for (std::size_t k = 0; k < z.name.size();)
        {
            const auto b = (std::uint8_t) z.name[k];
            std::uint32_t c = b;
            int more = b >= 0xf0 ? 3 : b >= 0xe0 ? 2 : b >= 0xc0 ? 1 : 0;
            c = more == 3 ? b & 0x07 : more == 2 ? b & 0x0f : more == 1 ? b & 0x1f : b;
            ++k;
            for (; more > 0 && k < z.name.size(); --more, ++k)
                c = (c << 6) | ((std::uint8_t) z.name[k] & 0x3f);
            if (c >= 0x10000)
            {
                c -= 0x10000;
                name.push_back ((char16_t) (0xd800 + (c >> 10)));
                name.push_back ((char16_t) (0xdc00 + (c & 0x3ff)));
            }
            else
                name.push_back ((char16_t) c);
        }
        if (name.size() < 255)
            put<std::uint8_t> (out, (std::uint8_t) name.size());
        else
        {
            put<std::uint8_t> (out, 255);
            put<std::uint32_t> (out, (std::uint32_t) name.size());
        }
        for (const auto c : name)
            put<std::uint16_t> (out, (std::uint16_t) c);
    }
    return out;
}

double splitterGain (double m) noexcept
{
    return m >= 1.0 ? 1.0 : (std::pow (11.0, m) - 1.0) / 10.0;
}

// ---------------------------------------------------------------------------------------------------------------
// VFX Envelope (REA_rodada164b §Envelope)

namespace
{
MapCurve curveOf (std::initializer_list<std::tuple<double, double, float>> points, std::array<std::int32_t, 5> footer)
{
    MapCurve m;
    for (const auto& [x, y, t] : points)
        m.points.push_back ({ x, y, t });
    m.footer = footer;
    return m;
}
} // namespace

EnvelopeSettings defaultEnvelope()
{
    EnvelopeSettings s;
    for (int g = 0; g < 16; ++g)
    {
        auto& a = s.articulators[(std::size_t) g];
        for (int t = 0; t < EnvelopeSettings::targetCount; ++t)
        {
            auto& p = a.parts[(std::size_t) t];
            p.lfoAmp = curveOf ({ { 0.0, 0.5, 0.0f }, { 1.0, 0.5, 0.0f }, { 1.25, 1.0, 0.0f }, { 2.25, 0.5, 0.0f } }, { 0, 0, -1, -1, -1 });
            if (t == EnvelopeSettings::velocity)
            {
                p.base = 0.0f;
                // a volume envelope: attack, a peak, sustain at half (= the note's own level), release
                p.envelope = curveOf ({ { 0.0, 0.0, 0.0f }, { 0.75, 1.0, 0.2f }, { 2.25, 0.5, 0.2f }, { 2.5, 0.0, 0.2f } },
                                      { 0, g == 0 ? -1 : 0, 1, -1, 2 });
            }
            else
            {
                p.base = 0.5f;
                p.env = t == EnvelopeSettings::pitch ? 0.25f : 1.0f;
                p.envelope = curveOf ({ { 0.0, 0.5, 0.0f }, { 0.25, 0.5, 0.0f } }, { 1, -1, -1, -1, -1 }); // (on, neutral)
            }
        }
    }
    return s;
}

namespace
{
bool readPart (Reader& r, EnvelopeSettings::Part& p)
{
    p.version = r.i32();
    p.base = r.get<float>();
    p.env = r.get<float>();
    p.lfo = r.get<float>();
    if (! readMap (r, p.envelope))
        return false;
    for (auto& k : p.adsr)
        k = r.i32();
    if (! readMap (r, p.lfoAmp))
        return false;
    p.speed = r.i32();
    p.tension = r.i32();
    p.skew = r.i32();
    p.pulseWidth = r.i32();
    p.phase = p.version >= 3 ? r.i32() : 0;
    for (auto* f : { &p.base, &p.env, &p.lfo })
        if (! std::isfinite (*f))
            *f = 0.0f;
    return ! r.failed;
}

void writePart (std::vector<std::uint8_t>& out, const EnvelopeSettings::Part& p)
{
    put<std::int32_t> (out, 3);
    put<float> (out, p.base);
    put<float> (out, p.env);
    put<float> (out, p.lfo);
    writeMap (out, p.envelope, 1);
    for (const auto k : p.adsr)
        put<std::int32_t> (out, k);
    writeMap (out, p.lfoAmp, 2);
    for (const auto v : { p.speed, p.tension, p.skew, p.pulseWidth, p.phase })
        put<std::int32_t> (out, v);
}

void writeChunk (std::vector<std::uint8_t>& out, std::int32_t id, const std::vector<std::uint8_t>& data)
{
    put<std::int32_t> (out, id);
    put<std::int64_t> (out, (std::int64_t) data.size());
    out.insert (out.end(), data.begin(), data.end());
}
} // namespace

std::optional<EnvelopeSettings> parseEnvelope (const std::vector<std::uint8_t>& fl)
{
    Reader r { fl.data(), fl.size() };
    auto s = defaultEnvelope();
    s.header = r.u32();
    if (r.failed || s.header >= 0x10004)
        return std::nullopt;
    std::vector<std::int32_t> params;
    int group = 0, target = 0;
    while (r.left() >= 12)
    {
        const auto id = r.i32();
        const auto size = r.get<std::int64_t>();
        if (size < 0 || (std::uint64_t) size > r.left())
            return std::nullopt;
        const auto end = r.pos + (std::size_t) size;
        Reader c { fl.data(), end, r.pos };
        switch (id)
        {
            case 1003:
                s.enabled = c.i32() != 0;
                s.shownArticulator = std::clamp (c.i32(), 0, 15);
                s.shownTarget = std::clamp (c.i32(), 0, 4);
                s.shownPart = std::clamp (c.i32(), 0, 1);
                break;
            case 1008:
                params.clear();
                while (c.left() >= 4)
                    params.push_back (c.i32());
                break;
            case 1007:
                for (auto& t : s.toggles)
                    t = c.left() >= 4 ? c.i32() : 0;
                break;
            case 1004:
            {
                group = std::clamp (c.i32(), 0, 15);
                auto& a = s.articulators[(std::size_t) group];
                a.colour = std::clamp (c.i32(), 0, 15);
                a.low = std::clamp (c.i32(), 0, 132);
                a.high = std::clamp (c.i32(), 0, 132);
                break;
            }
            case 1005:
                group = std::clamp (c.i32(), 0, 15);
                target = std::clamp (c.i32(), 0, 4);
                break;
            case 1006:
                if (! readPart (c, s.articulators[(std::size_t) group].parts[(std::size_t) target]))
                    return std::nullopt;
                break;
            default: break;
        }
        r.pos = end;
    }
    if (r.failed)
        return std::nullopt;
    // the parameter values set the base / envelope / LFO levels (FL pushes them after loading)
    if (params.size() >= 880)
        for (int g = 0; g < 16; ++g)
            for (int t = 0; t < 5; ++t)
            {
                auto& p = s.articulators[(std::size_t) g].parts[(std::size_t) t];
                const auto at = (std::size_t) ((g * 5 + t) * 11);
                p.base = std::clamp (params[at + 8], 0, 256) / 256.0f;
                p.env = std::clamp (params[at + 9], -256, 256) / 256.0f;
                p.lfo = std::clamp (params[at + 10], -256, 256) / 256.0f;
            }
    return s;
}

std::vector<std::uint8_t> writeEnvelope (const EnvelopeSettings& s)
{
    std::vector<std::uint8_t> out, chunk;
    put<std::uint32_t> (out, 0x10003);
    for (const auto v : { s.enabled ? 1 : 0, s.shownArticulator, s.shownTarget, s.shownPart })
        put<std::int32_t> (chunk, v);
    writeChunk (out, 1003, chunk);
    chunk.clear();
    for (const auto& a : s.articulators)
        for (const auto& p : a.parts)
            for (const auto v : { p.adsr[0], p.adsr[1], p.adsr[2], p.adsr[3], p.speed, p.tension, p.skew, p.pulseWidth,
                                  (std::int32_t) std::lround (p.base * 256.0f), (std::int32_t) std::lround (p.env * 256.0f),
                                  (std::int32_t) std::lround (p.lfo * 256.0f) })
                put<std::int32_t> (chunk, v);
    writeChunk (out, 1008, chunk);
    chunk.clear();
    for (const auto t : s.toggles)
        put<std::int32_t> (chunk, t);
    writeChunk (out, 1007, chunk);
    for (int g = 0; g < 16; ++g)
    {
        const auto& a = s.articulators[(std::size_t) g];
        chunk.clear();
        for (const auto v : { g, a.colour, a.low, a.high })
            put<std::int32_t> (chunk, v);
        writeChunk (out, 1004, chunk);
        for (int t = 0; t < 5; ++t)
        {
            chunk.clear();
            for (const auto v : { g, t, 0 })
                put<std::int32_t> (chunk, v);
            writeChunk (out, 1005, chunk);
            chunk.clear();
            writePart (chunk, a.parts[(std::size_t) t]);
            writeChunk (out, 1006, chunk);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Presets

std::vector<Preset> builtInPresets (Kind kind)
{
    std::vector<Preset> out;
    if (kind == Kind::keyMapper)
    {
        // scales: each key plays the nearest scale note at or below it (as FL's scale presets do)
        struct Scale
        {
            const char* name;
            std::vector<int> notes;
        };
        const std::vector<Scale> scales {
            { "Major", { 0, 2, 4, 5, 7, 9, 11 } },
            { "Natural minor", { 0, 2, 3, 5, 7, 8, 10 } },
            { "Harmonic minor", { 0, 2, 3, 5, 7, 8, 11 } },
            { "Melodic minor", { 0, 2, 3, 5, 7, 9, 11 } },
            { "Dorian", { 0, 2, 3, 5, 7, 9, 10 } },
            { "Phrygian", { 0, 1, 3, 5, 7, 8, 10 } },
            { "Lydian", { 0, 2, 4, 6, 7, 9, 11 } },
            { "Mixolydian", { 0, 2, 4, 5, 7, 9, 10 } },
            { "Locrian", { 0, 1, 3, 5, 6, 8, 10 } },
            { "Major pentatonic", { 0, 2, 4, 7, 9 } },
            { "Minor pentatonic", { 0, 3, 5, 7, 10 } },
            { "Blues", { 0, 3, 5, 6, 7, 10 } },
            { "Whole tone", { 0, 2, 4, 6, 8, 10 } },
            { "Diminished", { 0, 2, 3, 5, 6, 8, 9, 11 } },
        };
        for (const auto& scale : scales)
        {
            auto s = defaultKeyMapper();
            for (int pc = 0; pc < 12; ++pc)
            {
                int below = 0;
                for (const auto n : scale.notes)
                    if (n <= pc)
                        below = n;
                s.rows[(std::size_t) pc] = { below };
            }
            out.push_back ({ "Scales", scale.name, writeKeyMapper (s) });
        }
        auto shifted = [&] (const char* name, std::vector<int> intervals) {
            auto s = defaultKeyMapper();
            for (int pc = 0; pc < 12; ++pc)
            {
                s.rows[(std::size_t) pc].clear();
                for (const auto i : intervals)
                    s.rows[(std::size_t) pc].push_back (pc + i);
            }
            out.push_back ({ "Harmony", name, writeKeyMapper (s) });
        };
        shifted ("Octave up", { 0, 12 });
        shifted ("Octave down", { -12, 0 });
        shifted ("Fifth up", { 0, 7 });
        shifted ("Fifth down", { -5, 0 });
        shifted ("Power chord", { 0, 7, 12 });
        shifted ("Major triad", { 0, 4, 7 });
        shifted ("Minor triad", { 0, 3, 7 });
        auto inverted = defaultKeyMapper();
        for (int pc = 0; pc < 12; ++pc)
            inverted.rows[(std::size_t) pc] = { 11 - pc };
        out.push_back ({ "Special", "Inverted", writeKeyMapper (inverted) });
    }
    if (kind == Kind::levelScaler)
    {
        using L = LevelScalerSettings;
        auto make = [&] (const char* folder, const char* name, std::function<void (L&)> f) {
            auto s = defaultLevelScaler();
            f (s);
            out.push_back ({ folder, name, writeLevelScaler (s) });
        };
        make ("Velocity", "Humanize (subtle)", [] (L& s) { s.at (L::velocity, L::humanize) = 2400; });
        make ("Velocity", "Humanize (strong)", [] (L& s) { s.at (L::velocity, L::humanize) = 6000; });
        make ("Velocity", "Softer", [] (L& s) { s.at (L::velocity, L::multiply) = 6144; });
        make ("Velocity", "Compress", [] (L& s) {
            s.at (L::velocity, L::center) = 6400;
            s.at (L::velocity, L::multiply) = 4096;
        });
        make ("Velocity", "Expand", [] (L& s) {
            s.at (L::velocity, L::center) = 6400;
            s.at (L::velocity, L::multiply) = 12288;
        });
        make ("Velocity", "Fixed", [] (L& s) {
            s.at (L::velocity, L::multiply) = 0;
            s.at (L::velocity, L::offset) = 6400;
        });
        make ("Velocity", "Inverted", [] (L& s) {
            s.at (L::velocity, L::center) = 4096;
            s.at (L::velocity, L::multiply) = -8192;
        });
        make ("Other", "Random pan", [] (L& s) { s.at (L::pan, L::humanize) = 8192; });
        make ("Other", "Detune (random fine pitch)", [] (L& s) { s.at (L::finePitch, L::humanize) = 300; });
        make ("Other", "Short releases", [] (L& s) { s.at (L::release, L::offset) = -4096; });
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> stateFromFlPreset (const std::vector<std::uint8_t>& bytes, Kind kind)
{
    try
    {
        const auto project = flp::parseProject (bytes, true);
        if (project.channels.empty() || kindOfFlName (project.channels.front().pluginName) != kind)
            return std::nullopt;
        return project.channels.front().pluginState;
    }
    catch (const flp::Error&)
    {
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// VFX Script

namespace
{
// the INI's lines (CRLF or LF) with their section
template <typename F>
void scriptLines (const std::string& text, F&& f)
{
    std::string section;
    std::size_t at = 0;
    while (at <= text.size())
    {
        auto end = text.find ('\n', at);
        if (end == std::string::npos)
            end = text.size();
        auto from = at, to = end; // (the line without its BOM / CR)
        if (to > from && text[to - 1] == '\r')
            --to;
        if (from == 0 && text.rfind ("\xEF\xBB\xBF", 0) == 0)
            from = std::min<std::size_t> (3, to);
        const auto line = text.substr (from, to - from);
        if (line.size() >= 2 && line.front() == '[' && line.back() == ']')
            section = line.substr (1, line.size() - 2);
        else if (! f (section, line, from, to))
            return;
        if (end == text.size())
            break;
        at = end + 1;
    }
}
} // namespace

std::string ScriptSettings::source() const
{
    std::string out;
    scriptLines (text, [&] (const std::string& section, const std::string& line, std::size_t, std::size_t) {
        if (section != "Script" || line.rfind ("Source=", 0) != 0)
            return true;
        auto s = line.substr (7);
        for (std::size_t k = 0; (k = s.find ("<linebreak>", k)) != std::string::npos;)
            s.replace (k, 11, "\n");
        out = s;
        return false;
    });
    return out;
}

void ScriptSettings::setSource (const std::string& source)
{
    std::string flat;
    for (std::size_t i = 0; i < source.size(); ++i)
    {
        if (source[i] == '\r')
            continue;
        if (source[i] == '\n')
            flat += "<linebreak>";
        else
            flat += source[i];
    }
    bool done = false;
    scriptLines (text, [&] (const std::string& section, const std::string& line, std::size_t from, std::size_t to) {
        if (section != "Script" || line.rfind ("Source=", 0) != 0)
            return true;
        text.replace (from, to - from, "Source=" + flat);
        done = true;
        return false;
    });
    if (! done)
        text = "\xEF\xBB\xBF[Script]\r\nSource=" + flat + "\r\n" + (text.rfind ("\xEF\xBB\xBF", 0) == 0 ? text.substr (3) : text);
}

std::vector<std::pair<std::string, std::string>> ScriptSettings::parameters() const
{
    std::vector<std::pair<std::string, std::string>> out;
    scriptLines (text, [&] (const std::string& section, const std::string& line, std::size_t, std::size_t) {
        if (section == "Parameters")
            if (const auto eq = line.find ('='); eq != std::string::npos)
                out.emplace_back (line.substr (0, eq), line.substr (eq + 1));
        return true;
    });
    return out;
}

bool ScriptSettings::hasSurface() const { return text.find ("\r\n[Surface]") != std::string::npos || text.find ("\n[Surface]") != std::string::npos; }

ScriptSettings defaultScript()
{
    ScriptSettings s;
    s.text = "\xEF\xBB\xBF[Script]\r\nSource=import flvfx as vfx\r\n";
    return s;
}

std::optional<ScriptSettings> parseScript (const std::vector<std::uint8_t>& fl)
{
    Reader r { fl.data(), fl.size() };
    const auto n = r.u32();
    if (r.failed || n != r.left())
        return std::nullopt;
    ScriptSettings s;
    s.text.assign ((const char*) fl.data() + 4, n);
    return s;
}

std::vector<std::uint8_t> writeScript (const ScriptSettings& s)
{
    std::vector<std::uint8_t> out;
    put<std::uint32_t> (out, (std::uint32_t) s.text.size());
    out.insert (out.end(), s.text.begin(), s.text.end());
    return out;
}

// ---------------------------------------------------------------------------------------------------------------

std::vector<std::uint8_t> defaultState (Kind kind)
{
    switch (kind)
    {
        case Kind::keyMapper: return writeKeyMapper (defaultKeyMapper());
        case Kind::colorMapper: return writeColorMapper (defaultColorMapper());
        case Kind::levelScaler: return writeLevelScaler (defaultLevelScaler());
        case Kind::keyboardSplitter: return writeKeyboardSplitter (defaultKeyboardSplitter());
        case Kind::envelope: return writeEnvelope (defaultEnvelope());
        case Kind::sequencer: return writeSequencer (defaultSequencer());
        case Kind::script: return writeScript (defaultScript());
    }
    return {};
}

int outputCount (Kind kind) noexcept
{
    return kind == Kind::colorMapper || kind == Kind::keyboardSplitter ? 16 : 1;
}

int outputsShown (Kind kind) noexcept
{
    return kind == Kind::colorMapper || kind == Kind::keyboardSplitter ? 4 : 1;
}

std::string outputName (Kind kind, int output)
{
    if (kind == Kind::colorMapper)
        return "Voice " + std::to_string (output + 1);
    if (kind == Kind::keyboardSplitter)
        return "Zone " + std::to_string (output + 1);
    return "Voice output";
}

std::optional<State> readState (const std::vector<std::uint8_t>& pluginState)
{
    Reader r { pluginState.data(), pluginState.size() };
    if (r.u32() != lynxMagic)
        return std::nullopt;
    const auto kind = r.u32();
    const auto port = r.i32();
    r.u32(); // (reserved)
    if (r.failed || kind >= (std::uint32_t) kindCount)
        return std::nullopt;
    State s;
    s.kind = (Kind) kind;
    s.port = std::clamp (port, 0, 255);
    s.fl.assign (pluginState.begin() + (std::ptrdiff_t) r.pos, pluginState.end());
    return s;
}

std::vector<std::uint8_t> writeState (const State& state)
{
    std::vector<std::uint8_t> out;
    put<std::uint32_t> (out, lynxMagic);
    put<std::uint32_t> (out, (std::uint32_t) state.kind);
    put<std::int32_t> (out, state.port);
    put<std::uint32_t> (out, 0);
    out.insert (out.end(), state.fl.begin(), state.fl.end());
    return out;
}

void makeChannel (ChannelDef& channel, Kind kind)
{
    channel.instrument = InstrumentKind::vfx;
    channel.name = flName (kind);
    State s;
    s.kind = kind;
    s.fl = defaultState (kind);
    channel.pluginState = writeState (s);
}

std::optional<Kind> kindOf (const ChannelDef& channel)
{
    if (channel.instrument != InstrumentKind::vfx)
        return std::nullopt;
    if (auto s = readState (channel.pluginState))
        return s->kind;
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------------------------
// Processors

namespace
{
// What every processor keeps: the voices it sent out for each input voice, so an input's "off" ends exactly them
// (even after the settings changed) and releaseAll ends everything.
struct VoiceTable
{
    struct Entry
    {
        int input = -1;  // input id
        int id = 0;      // output voice id
        int output = 0;
        Voice voice;
        float slide = 0.0f; // Key Mapper's pitch slide: cents still to go (0 = none)
    };
    std::array<Entry, 512> entries {};
    int count = 0;
    int nextId = 1;

    int add (int input, int output, const Voice& v) noexcept
    {
        if (count >= (int) entries.size())
            return -1;
        auto& e = entries[(std::size_t) count++];
        e.input = input;
        e.output = output;
        e.voice = v;
        e.slide = 0.0f;
        e.id = nextId++;
        if (nextId > 1 << 30)
            nextId = 1;
        return e.id;
    }
    // sends the "off" of every voice of `input` (all with input < 0)
    void release (int input, int offset, float releaseVelocity, EventList& out) noexcept
    {
        int kept = 0;
        for (int i = 0; i < count; ++i)
        {
            auto& e = entries[(std::size_t) i];
            if (input >= 0 && e.input != input)
            {
                entries[(std::size_t) kept++] = e;
                continue;
            }
            Event off;
            off.type = Event::Type::off;
            off.offset = offset;
            off.output = e.output;
            off.id = e.id;
            off.voice = e.voice;
            off.voice.release = releaseVelocity;
            out.add (off);
        }
        count = kept;
    }
};

Event onEvent (int offset, int output, int id, const Voice& v) noexcept
{
    Event e;
    e.type = Event::Type::on;
    e.offset = offset;
    e.output = output;
    e.id = id;
    e.voice = v;
    return e;
}

// A processor whose settings are FL's parsed state
template <typename Settings>
class Base : public Processor
{
public:
    explicit Base (Settings initial) : settings (std::move (initial)) {}

    void releaseAll (EventList& out) noexcept override { voices.release (-1, 0, 0.5f, out); }
    bool busy() const noexcept override { return voices.count > 0; }

protected:
    const Settings& current() noexcept
    {
        settings.take();
        return settings.get();
    }
    void publish (Settings s) { settings.publish (std::move (s)); }

    VoiceTable voices;

private:
    Handoff<Settings> settings;
};

// Notes pass unchanged (a plugin whose behaviour LYNX does not reproduce yet keeps the notes flowing)
class ThroughProcessor final : public Processor
{
public:
    explicit ThroughProcessor (Kind k) : which (k) {}
    Kind kind() const noexcept override { return which; }
    void releaseAll (EventList& out) noexcept override { voices.release (-1, 0, 0.5f, out); }
    void process (const Event* in, int count, int, const Transport&, EventList& out) noexcept override
    {
        for (int i = 0; i < count; ++i)
        {
            const auto& e = in[i];
            if (e.type == Event::Type::on)
            {
                if (const auto id = voices.add (e.id, 0, e.voice); id >= 0)
                    out.add (onEvent (e.offset, 0, id, e.voice));
            }
            else if (e.type == Event::Type::off)
                voices.release (e.id, e.offset, e.voice.release, out);
        }
    }
    bool configure (const std::vector<std::uint8_t>&) override { return true; }

private:
    Kind which;
    VoiceTable voices;
};

class KeyMapper final : public Base<KeyMapperSettings>
{
public:
    explicit KeyMapper (KeyMapperSettings s) : Base (std::move (s)) {}
    Kind kind() const noexcept override { return Kind::keyMapper; }
    bool configure (const std::vector<std::uint8_t>& fl) override
    {
        auto s = parseKeyMapper (fl);
        if (! s)
            return false;
        publish (std::move (*s));
        return true;
    }

    void process (const Event* in, int count, int n, const Transport& transport, EventList& out) noexcept override
    {
        const auto& s = current();
        // the slides under way move on (FL: the voice's pitch covers 99 % of the way in `slide` / 192 beats)
        if (s.slide > 0)
        {
            const auto beats = (double) n * transport.bpm / (60.0 * std::max (1.0, transport.sampleRate));
            const auto keep = (float) std::pow (0.01, beats * 192.0 / (double) s.slide);
            for (int i = 0; i < voices.count; ++i)
                slideStep (voices.entries[(std::size_t) i], keep, out);
        }
        else
            for (int i = 0; i < voices.count; ++i)
                slideStep (voices.entries[(std::size_t) i], 0.0f, out);

        for (int i = 0; i < count; ++i)
        {
            const auto& e = in[i];
            if (e.type == Event::Type::off)
            {
                voices.release (e.id, e.offset, e.voice.release, out);
                continue;
            }
            if (e.type != Event::Type::on)
                continue;
            if (! s.enabled)
            {
                send (e, e.voice, 0, out);
                continue;
            }
            // FL: the row of the input's pitch class counted from the base key; its values are semitones from the start
            // of that octave (on the base key); the offset transposes what comes out
            const auto k = std::max (0, e.voice.key - s.baseKey + 12);
            const auto rowIndex = k % 12;
            const auto octaveStart = e.voice.key - rowIndex;
            const auto& row = s.rows[(std::size_t) rowIndex];
            const auto size = (int) row.size();
            if (size == 0)
                continue; // (the note is muted)
            auto play = [&] (int value, float gain) {
                auto v = e.voice;
                v.key = octaveStart + value + s.offset;
                if (v.key < 0 || v.key > 127)
                    return;
                v.velocity *= gain;
                // with a pitch slide the new note starts at the played pitch and glides to its own
                const auto slide = s.slide > 0 ? (float) (e.voice.key - v.key) * 100.0f : 0.0f;
                send (e, v, slide, out);
            };
            switch (s.multimode)
            {
                case 1: // all, gain-compensated (1 / sqrt n)
                    for (const auto value : row)
                        play (value, 1.0f / std::sqrt ((float) size));
                    break;
                case 2: // one of them at random
                    play (row[(std::size_t) (nextRandom() % (std::uint32_t) size)], 1.0f);
                    break;
                case 3: // the next one in the row's order (the first first)
                {
                    auto& c = cycle[(std::size_t) rowIndex];
                    const auto at = ((c % size) + size) % size;
                    c = at + 1;
                    play (row[(std::size_t) at], 1.0f);
                    break;
                }
                case 4: // backwards (the last first)
                {
                    auto& c = cycle[(std::size_t) rowIndex];
                    c = (((c + size - 1) % size) + size) % size;
                    play (row[(std::size_t) c], 1.0f);
                    break;
                }
                default:
                    for (const auto value : row)
                        play (value, 1.0f);
                    break;
            }
        }
    }

    void releaseAll (EventList& out) noexcept override
    {
        voices.release (-1, 0, 0.5f, out);
        cycle.fill (0); // (FL: a flush starts the cycles again)
    }

private:
    void send (const Event& input, Voice v, float slide, EventList& out) noexcept
    {
        const auto cents = v.cents;
        v.cents += slide;
        if (const auto id = voices.add (input.id, 0, v); id >= 0)
        {
            out.add (onEvent (input.offset, 0, id, v));
            auto& entry = voices.entries[(std::size_t) voices.count - 1];
            entry.slide = slide;
            entry.voice.cents = cents;
        }
    }
    // one block of a voice's slide: `keep` of the way left stays (a prop event with its pitch)
    static void slideStep (VoiceTable::Entry& e, float keep, EventList& out) noexcept
    {
        if (e.slide == 0.0f)
            return;
        e.slide *= keep;
        if (std::abs (e.slide) < 0.5f)
            e.slide = 0.0f;
        Event p;
        p.type = Event::Type::prop;
        p.output = e.output;
        p.id = e.id;
        p.voice = e.voice;
        p.prop = Prop::pitch;
        p.value = e.voice.cents + e.slide;
        out.add (p);
    }
    std::uint32_t nextRandom() noexcept
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    }
    std::array<int, 12> cycle {};
    std::uint32_t seed = 0x9e3779b9u;
};

class ColorMapper final : public Base<ColorMapperSettings>
{
public:
    explicit ColorMapper (ColorMapperSettings s) : Base (std::move (s)) {}
    Kind kind() const noexcept override { return Kind::colorMapper; }
    bool configure (const std::vector<std::uint8_t>& fl) override
    {
        auto s = parseColorMapper (fl);
        if (! s)
            return false;
        publish (*s);
        return true;
    }
    void process (const Event* in, int count, int, const Transport&, EventList& out) noexcept override
    {
        const auto& s = current();
        for (int i = 0; i < count; ++i)
        {
            const auto& e = in[i];
            if (e.type == Event::Type::off)
            {
                voices.release (e.id, e.offset, e.voice.release, out);
                continue;
            }
            if (e.type != Event::Type::on)
                continue;
            const auto c = e.voice.colour & 15;
            auto v = e.voice;
            v.colour = s.colour[(std::size_t) c];
            const auto port = s.port[(std::size_t) c];
            if (const auto id = voices.add (e.id, port, v); id >= 0)
                out.add (onEvent (e.offset, port, id, v));
        }
    }
};
} // namespace

namespace
{
// One voice in, one voice out, its velocity, fine pitch, pan, Mod X / Y and release velocity through the curves.
// FL re-evaluates the curves every tick from the parent's live levels (round 165c): LYNX re-evaluates a held voice
// whenever its levels change on the way here (a VFX before it sends them) and whenever the settings change (the
// editor, an automation or a Patcher control cable on one of its 36 parameters), and sends what changed. Each voice
// keeps its six random numbers (FL: R[0…5], fixed per note); the release velocity draws a fresh one.
class LevelScaler final : public Processor
{
public:
    explicit LevelScaler (LevelScalerSettings s) : settings (s), live (s) {}
    Kind kind() const noexcept override { return Kind::levelScaler; }
    bool configure (const std::vector<std::uint8_t>& fl) override
    {
        auto s = parseLevelScaler (fl);
        if (! s)
            return false;
        settings.publish (std::move (*s));
        return true;
    }
    bool busy() const noexcept override { return count > 0; }
    void releaseAll (EventList& out) noexcept override
    {
        for (int i = 0; i < count; ++i)
            out.add (offEvent (voices[(std::size_t) i], 0, voices[(std::size_t) i].sent.release));
        count = 0;
    }
    void setParameter (int index, float value) noexcept override
    {
        if (index < 0 || index >= 36 || ! std::isfinite (value))
            return;
        // FL's FromMIDI: min + round (v · (max − min))
        static constexpr int lo[6] = { 0, -16384, -8192, -8192, 0, 0 }, hi[6] = { 8192, 16384, 8192, 8192, 16384, 2 };
        const auto f = index % 6;
        const auto v = lo[f] + (int) std::lround (std::clamp ((double) value, 0.0, 1.0) * (hi[f] - lo[f]));
        if (live.p[(std::size_t) index] != v)
        {
            live.p[(std::size_t) index] = v;
            dirty = true;
        }
    }

    void process (const Event* in, int n, int, const Transport&, EventList& out) noexcept override
    {
        if (settings.take())
        {
            live = settings.get();
            dirty = true;
        }
        if (dirty) // (the curves moved: the held voices follow)
        {
            dirty = false;
            for (int i = 0; i < count; ++i)
                update (voices[(std::size_t) i], 0, out);
        }
        for (int i = 0; i < n; ++i)
        {
            const auto& e = in[i];
            if (e.type == Event::Type::on)
            {
                if (count >= (int) voices.size())
                    continue;
                auto& v = voices[(std::size_t) count];
                v.input = e.id;
                v.id = nextId++;
                if (nextId > 1 << 30)
                    nextId = 1;
                v.source = e.voice;
                for (auto& r : v.random)
                    r = random();
                v.sent = scaled (v, true);
                out.add (onEvent (e.offset, 0, v.id, v.sent));
                ++count;
            }
            else if (e.type == Event::Type::off)
            {
                for (int k = 0; k < count; ++k)
                    if (voices[(std::size_t) k].input == e.id)
                    {
                        const auto x = (double) e.voice.release;
                        const auto r = (float) levelScale (live, LevelScalerSettings::release, x, 0.0, 1.0, random());
                        show (LevelScalerSettings::release, x, r);
                        out.add (offEvent (voices[(std::size_t) k], e.offset, r));
                        voices[(std::size_t) k] = voices[(std::size_t) --count];
                        --k;
                    }
            }
            else if (e.type == Event::Type::prop) // (a VFX before it moves the voice: the curves follow)
            {
                for (int k = 0; k < count; ++k)
                {
                    auto& v = voices[(std::size_t) k];
                    if (v.input != e.id)
                        continue;
                    switch (e.prop)
                    {
                        case Prop::velocity: v.source.velocity = e.value; break;
                        case Prop::pitch: v.source.cents = e.value; break;
                        case Prop::pan: v.source.pan = e.value; break;
                        case Prop::modX: v.source.modX = e.value; break;
                        case Prop::modY: v.source.modY = e.value; break;
                    }
                    update (v, e.offset, out);
                }
            }
        }
    }

private:
    struct Voice
    {
        int input = -1, id = 0;
        vfx::Voice source, sent;
        std::array<double, 6> random {};
    };

    // the voice through the curves (FL's order: pan, Mod X, Mod Y, fine pitch, velocity)
    vfx::Voice scaled (const Voice& v, bool shown) noexcept
    {
        using L = LevelScalerSettings;
        auto out = v.source;
        auto through = [&] (int tab, double x, double lo, double hi) {
            const auto y = levelScale (live, tab, x, lo, hi, v.random[(std::size_t) tab]);
            if (shown)
                show (tab, (x - lo) / (hi - lo), (y - lo) / (hi - lo));
            return y;
        };
        out.pan = (float) through (L::pan, v.source.pan, -1.0, 1.0);
        out.modX = (float) (through (L::modX, v.source.modX * 2.0 - 1.0, -1.0, 1.0) * 0.5 + 0.5);
        out.modY = (float) (through (L::modY, v.source.modY * 2.0 - 1.0, -1.0, 1.0) * 0.5 + 0.5);
        out.cents = (float) (through (L::finePitch, v.source.cents / 1200.0, -1.0, 1.0) * 1200.0);
        out.velocity = (float) through (L::velocity, v.source.velocity, 0.0, 1.0);
        return out;
    }

    void update (Voice& v, int offset, EventList& out) noexcept
    {
        const auto now = scaled (v, false);
        auto send = [&] (Prop prop, float before, float after) {
            if (std::abs (after - before) < 1e-5f)
                return;
            Event p;
            p.type = Event::Type::prop;
            p.offset = offset;
            p.id = v.id;
            p.voice = v.sent;
            p.prop = prop;
            p.value = after;
            out.add (p);
        };
        send (Prop::velocity, v.sent.velocity, now.velocity);
        send (Prop::pitch, v.sent.cents, now.cents);
        send (Prop::pan, v.sent.pan, now.pan);
        send (Prop::modX, v.sent.modX, now.modX);
        send (Prop::modY, v.sent.modY, now.modY);
        v.sent = now;
    }

    // the editor's "event persistence" display: what came in and what went out (0..1 on the tab's range)
    void show (int tab, double x, double y) noexcept
    {
        if (live_.watching.load (std::memory_order_relaxed))
            live_.push ({ (std::uint8_t) tab, (float) x, (float) y });
    }

    static Event offEvent (const Voice& v, int offset, float release) noexcept
    {
        Event e;
        e.type = Event::Type::off;
        e.offset = offset;
        e.id = v.id;
        e.voice = v.sent;
        e.voice.release = release;
        return e;
    }

    double random() noexcept
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return (double) (seed >> 8) / 16777216.0;
    }

    Handoff<LevelScalerSettings> settings;
    LevelScalerSettings live; // (the audio thread's copy: automation moves it)
    Monitor& live_ = monitor();
    std::array<Voice, 256> voices {};
    int count = 0, nextId = 1;
    bool dirty = false;
    std::uint32_t seed = 0x2545f491u;
};
} // namespace

namespace
{
// Each note on every zone whose keyboard × velocity level is above zero, on that zone's output (FL: the level
// scales the note's volume, or replaces it with Absolute; Random zones play one of them, picked by their keyboard
// level). LYNX works in velocities: vol = (11^(1.28 v) − 1) / 10 both ways.
class KeyboardSplitter final : public Base<KeyboardSplitterSettings>
{
public:
    explicit KeyboardSplitter (KeyboardSplitterSettings s) : Base (std::move (s)) {}
    Kind kind() const noexcept override { return Kind::keyboardSplitter; }
    bool configure (const std::vector<std::uint8_t>& fl) override
    {
        auto s = parseKeyboardSplitter (fl);
        if (! s)
            return false;
        publish (std::move (*s));
        return true;
    }
    void process (const Event* in, int count, int, const Transport&, EventList& out) noexcept override
    {
        const auto& s = current();
        for (int i = 0; i < count; ++i)
        {
            const auto& e = in[i];
            if (e.type == Event::Type::off)
            {
                voices.release (e.id, e.offset, e.voice.release, out);
                continue;
            }
            if (e.type != Event::Type::on)
                continue;
            const auto x = ((double) e.voice.key + e.voice.cents / 100.0) / 132.0;
            const auto velocity = std::min (1.0, (double) e.voice.velocity);
            std::array<double, 16> w {};
            bool anyRandom = false;
            for (int z = 0; z < 16; ++z)
            {
                const auto& zone = s.zones[(std::size_t) z];
                if (! zone.active())
                    continue;
                w[(std::size_t) z] = splitterGain (zone.keyboard.at (x));
                anyRandom = anyRandom || zone.random;
            }
            if (anyRandom) // one weighted pick among the random zones, which then plays at full level
            {
                double total = 0.0;
                for (int z = 0; z < 16; ++z)
                    if (s.zones[(std::size_t) z].random && s.zones[(std::size_t) z].active())
                        total += w[(std::size_t) z];
                const auto r = random() * total;
                int chosen = -1;
                double acc = 0.0;
                for (int z = 0; z < 16; ++z)
                    if (s.zones[(std::size_t) z].random && s.zones[(std::size_t) z].active() && w[(std::size_t) z] > 0.0)
                    {
                        if (acc <= r)
                            chosen = z;
                        acc += w[(std::size_t) z];
                        w[(std::size_t) z] = 0.0;
                    }
                if (chosen >= 0) // (FL then falls back on zone 1; LYNX plays no random zone)
                    w[(std::size_t) chosen] = 1.0;
            }
            const auto volumeIn = velocityToVolume (velocity);
            for (int z = 0; z < 16; ++z)
            {
                const auto& zone = s.zones[(std::size_t) z];
                if (w[(std::size_t) z] <= 0.0)
                    continue;
                const auto level = w[(std::size_t) z] * splitterGain (zone.velocity.at (velocity));
                if (level <= 1e-5)
                    continue;
                auto v = e.voice;
                v.velocity = (float) volumeToVelocity (zone.absolute ? level : volumeIn * level);
                v.key = e.voice.key + zone.transpose;
                if (v.key < 0 || v.key > 127)
                    continue;
                if (const auto id = voices.add (e.id, z, v); id >= 0)
                    out.add (onEvent (e.offset, z, id, v));
            }
        }
    }

private:
    static double velocityToVolume (double v) noexcept { return (std::pow (11.0, 1.28 * v) - 1.0) / 10.0; }
    static double volumeToVelocity (double vol) noexcept { return std::clamp (std::log (10.0 * vol + 1.0) / std::log (11.0) / 1.28, 0.0, 1.0); }
    double random() noexcept
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return (double) (seed >> 8) / 16777216.0;
    }
    std::uint32_t seed = 0x12bd6aau;
};
} // namespace

namespace
{
// An FL curve as the Envelope Controller's (core/FlSynths.h): accumulated points, LYNX's tension sign on the segment
// starting at a point, a hold as a step
void toArticulator (const MapCurve& m, ControllerArticulator& a, int* loopPoint = nullptr)
{
    a.count = 0;
    a.sustain = -1;
    a.decayPoint = -1;
    if (loopPoint != nullptr)
        *loopPoint = -1;
    const auto sustain = m.footer[4], decay = m.footer[2], loop = m.footer[3];
    for (std::size_t i = 0; i < m.points.size() && a.count < (int) a.time.size(); ++i)
    {
        const auto& p = m.points[i];
        if (i > 0)
        {
            if (p.curve == 2 && p.x > m.points[i - 1].x && a.count + 1 < (int) a.time.size())
            {
                a.time[(std::size_t) a.count] = (float) p.x;
                a.level[(std::size_t) a.count] = a.level[(std::size_t) a.count - 1];
                a.tension[(std::size_t) a.count] = 0.0f;
                ++a.count;
            }
            else if (p.curve <= 1)
                a.tension[(std::size_t) a.count - 1] = std::clamp (-p.tension, -1.0f, 1.0f);
        }
        if ((int) i == sustain)
            a.sustain = a.count;
        if ((int) i == decay)
            a.decayPoint = a.count;
        if ((int) i == loop && loopPoint != nullptr)
            *loopPoint = a.count;
        a.time[(std::size_t) a.count] = (float) p.x;
        a.level[(std::size_t) a.count] = (float) std::clamp (p.y, 0.0, 1.0);
        a.tension[(std::size_t) a.count] = 0.0f;
        ++a.count;
    }
}

// FL's envelope loop (round 165c): while the key is held, the time between the loop point and the sustain point
// repeats (the sustain point ends the loop); the release then goes on from the sustain point as before
double envelopeLoopTime (const ControllerArticulator& a, int loop, double t, double sinceRelease) noexcept
{
    if (loop < 0 || a.sustain <= loop || a.sustain >= a.count || sinceRelease >= 0.0)
        return t;
    const auto start = (double) a.time[(std::size_t) loop], end = (double) a.time[(std::size_t) a.sustain];
    if (end - start <= 1e-9 || t < end)
        return t;
    return start + std::fmod (t - start, end - start);
}

// What the audio thread evaluates: per articulator and target the envelope, the LFO amplitude and the LFO wave
struct EnvelopeRuntime
{
    struct Part
    {
        bool envOn = false, lfoOn = false, envTempo = false, lfoTempo = false;
        float base = 0.5f, env = 0.0f, lfo = 0.0f, offset = 0.5f;
        ControllerArticulator envelope, amplitude, wave; // (levels 0…1 with base 0: their curves' values)
        double rate = 1.0, startPhase = 0.0;            // LFO: cycles per unit, phase at the trigger
        int envLoop = -1, ampLoop = -1;                 // FL's loop point (start loop; the sustain point ends it)
    };
    struct Articulator
    {
        int colour = 0, low = 0, high = 132;
        std::array<Part, 5> parts;
    };
    bool enabled = true;
    std::array<Articulator, 16> articulators;

    // (big: on the heap, never on a stack)
    static std::unique_ptr<EnvelopeRuntime> from (const EnvelopeSettings& s)
    {
        auto made = std::make_unique<EnvelopeRuntime>();
        auto& rt = *made;
        rt.enabled = s.enabled;
        for (int g = 0; g < 16; ++g)
        {
            const auto& a = s.articulators[(std::size_t) g];
            auto& r = rt.articulators[(std::size_t) g];
            r.colour = a.colour;
            r.low = a.low;
            r.high = a.high;
            for (int t = 0; t < 5; ++t)
            {
                const auto& p = a.parts[(std::size_t) t];
                auto& q = r.parts[(std::size_t) t];
                q.envOn = p.envOn() && ! p.envelope.points.empty();
                q.lfoOn = p.lfoOn();
                q.envTempo = (p.envelope.footer[0] & 1) != 0;
                q.lfoTempo = (p.lfoAmp.footer[0] & 1) != 0;
                q.base = p.base;
                q.env = p.env;
                q.lfo = p.lfo;
                q.offset = t == EnvelopeSettings::velocity ? 0.0f : 0.5f;
                toArticulator (p.envelope, q.envelope, &q.envLoop);
                q.envelope.adsr = p.adsr;
                q.envelope = controllerWithKnobs (q.envelope);
                for (auto* c : { &q.envelope, &q.amplitude })
                {
                    c->base = 0.0f;
                    c->envLevel = 1.0f;
                    c->bipolar = false;
                    c->envOn = true;
                }
                toArticulator (p.lfoAmp, q.amplitude, &q.ampLoop);
                q.wave.lfoSpeed = p.speed;
                q.wave.lfoTension = p.tension;
                q.wave.lfoSkew = p.skew;
                q.wave.lfoPulseWidth = p.pulseWidth;
                q.wave.lfoPhase = p.phase;
                q.rate = controllerLfoRate (q.wave);
                q.startPhase = controllerLfoStartPhase (q.wave);
            }
        }
        return made;
    }
};

// One note in, the same note out, its volume, fine pitch, pan and Mod X / Y moved by the envelopes and LFOs of every
// articulator whose note colour and key range it matches (FL re-applies them each tick; LYNX each block).
class Envelope final : public Processor
{
public:
    explicit Envelope (const EnvelopeSettings& s) : settings (EnvelopeRuntime::from (s)) {}
    Kind kind() const noexcept override { return Kind::envelope; }
    bool configure (const std::vector<std::uint8_t>& fl) override
    {
        auto s = parseEnvelope (fl);
        if (! s)
            return false;
        settings.publish (EnvelopeRuntime::from (*s));
        return true;
    }
    bool busy() const noexcept override { return count > 0; }

    void releaseAll (EventList& out) noexcept override
    {
        for (int i = 0; i < count; ++i)
            if (! voices[(std::size_t) i].released)
                out.add (offEvent (voices[(std::size_t) i], 0));
        count = 0;
    }

    void process (const Event* in, int n, int samples, const Transport& transport, EventList& out) noexcept override
    {
        settings.take();
        const auto& rt = settings.get();
        const auto seconds = samples / std::max (1.0, transport.sampleRate);
        const auto beats = seconds * transport.bpm / 60.0;
        // the voices sounding move on (and send what changed)
        for (int i = 0; i < count; ++i)
        {
            auto& v = voices[(std::size_t) i];
            v.seconds += seconds;
            v.beats += beats;
            if (v.released)
            {
                v.releaseSeconds += seconds;
                v.releaseBeats += beats;
            }
            apply (rt, v, 0, &out);
        }
        // the finished ones go (their output was released and their envelopes have ended, or 30 s after the release)
        int kept = 0;
        for (int i = 0; i < count; ++i)
        {
            const auto& v = voices[(std::size_t) i];
            if (v.released && (v.releaseSeconds > 30.0 || finished (rt, v)))
                continue;
            voices[(std::size_t) kept++] = v;
        }
        count = kept;

        for (int i = 0; i < n; ++i)
        {
            const auto& e = in[i];
            if (e.type == Event::Type::on)
            {
                Voice v;
                v.input = e.id;
                v.id = nextId++;
                if (nextId > 1 << 30)
                    nextId = 1;
                v.source = e.voice;
                v.sent = e.voice;
                for (int g = 0; g < 16; ++g)
                {
                    const auto& a = rt.articulators[(std::size_t) g];
                    const auto key = e.voice.key + 1;
                    if (a.colour == e.voice.colour && key >= a.low && key <= a.high)
                        v.matched |= (std::uint16_t) (1u << g);
                }
                Voice playing = v;
                apply (rt, playing, e.offset, nullptr);
                out.add (onEvent (e.offset, 0, playing.id, playing.sent));
                if (count < (int) voices.size())
                    voices[(std::size_t) count++] = playing;
            }
            else if (e.type == Event::Type::off)
                for (int k = 0; k < count; ++k)
                {
                    auto& v = voices[(std::size_t) k];
                    if (v.input == e.id && ! v.released)
                    {
                        v.released = true;
                        v.source.release = e.voice.release;
                        out.add (offEvent (v, e.offset));
                    }
                }
        }
    }

private:
    struct Voice
    {
        int input = -1, id = 0;
        Voice() = default;
        vfx::Voice source, sent;
        std::uint16_t matched = 0;
        bool released = false;
        double seconds = 0.0, beats = 0.0, releaseSeconds = 0.0, releaseBeats = 0.0;
    };

    static Event offEvent (const Voice& v, int offset) noexcept
    {
        Event e;
        e.type = Event::Type::off;
        e.offset = offset;
        e.id = v.id;
        e.voice = v.sent;
        e.voice.release = v.source.release;
        return e;
    }

    // the voice's levels now (FL's per-voice apply); with `out`, prop events for what changed
    static void apply (const EnvelopeRuntime& rt, Voice& v, int offset, EventList* out) noexcept
    {
        auto now = v.source;
        if (rt.enabled && v.matched != 0)
        {
            const auto sourceVolume = (std::pow (11.0, 1.28 * (double) v.source.velocity) - 1.0) / 10.0;
            auto volume = sourceVolume;
            double cents = v.source.cents, pan = v.source.pan, modX = v.source.modX * 2.0 - 1.0, modY = v.source.modY * 2.0 - 1.0;
            for (int g = 15; g >= 0; --g) // (FL: the last articulator first)
            {
                if ((v.matched & (1u << g)) == 0)
                    continue;
                for (int t = 4; t >= 0; --t)
                {
                    const auto& p = rt.articulators[(std::size_t) g].parts[(std::size_t) t];
                    if (! p.envOn && ! p.lfoOn)
                        continue;
                    const auto envRelease = ! v.released ? -1.0 : p.envTempo ? v.releaseBeats : v.releaseSeconds;
                    const auto envTime = envelopeLoopTime (p.envelope, p.envLoop, p.envTempo ? v.beats : v.seconds, envRelease);
                    const auto e = p.envOn ? controllerValue (p.envelope, envTime, envRelease) - p.offset : 0.0f;
                    double l = 0.0;
                    if (p.lfoOn)
                    {
                        const auto lfoTime = p.lfoTempo ? v.beats : v.seconds;
                        const auto lfoRelease = ! v.released ? -1.0 : p.lfoTempo ? v.releaseBeats : v.releaseSeconds;
                        const auto amp = controllerValue (p.amplitude, envelopeLoopTime (p.amplitude, p.ampLoop, lfoTime, lfoRelease), lfoRelease) - 0.5;
                        l = amp * controllerLfoWave (p.wave, p.startPhase + p.rate * lfoTime);
                    }
                    if (t == EnvelopeSettings::velocity)
                    {
                        double m = p.base;
                        if (p.envOn)
                            m = p.env >= 0.0f ? p.base + 2.0 * p.env * e : (1.0 - p.base) + 2.0 * std::abs (p.env) * (1.0 - e);
                        if (p.lfoOn)
                            m *= 2.0 * p.lfo * l;
                        volume = std::clamp (volume * m, 0.0, 4.0);
                        continue;
                    }
                    auto d = (double) p.base - 0.5;
                    if (p.envOn)
                        d += 2.0 * p.env * e;
                    if (p.lfoOn)
                        d += 2.0 * p.lfo * l;
                    if (t == EnvelopeSettings::pitch)
                        cents += d * 4800.0;
                    else if (t == EnvelopeSettings::pan)
                        pan = std::clamp (pan + d, -1.0, 1.0);
                    else if (t == EnvelopeSettings::modX)
                        modX = std::clamp (modX + d, -2.0, 2.0);
                    else
                        modY = std::clamp (modY + d, -2.0, 2.0);
                }
            }
            if (volume != sourceVolume)
                now.velocity = (float) std::clamp (std::log (10.0 * volume + 1.0) / std::log (11.0) / 1.28, 0.0, 1.0);
            now.cents = (float) std::clamp (cents, -v.source.key * 100.0, 12000.0 - v.source.key * 100.0); // (FL: Init.Pitch 0…12000)
            now.pan = (float) pan;
            now.modX = (float) std::clamp (modX * 0.5 + 0.5, 0.0, 1.0);
            now.modY = (float) std::clamp (modY * 0.5 + 0.5, 0.0, 1.0);
        }
        if (out != nullptr)
        {
            auto send = [&] (Prop prop, float before, float after) {
                if (std::abs (after - before) < 1e-4f)
                    return;
                Event p;
                p.type = Event::Type::prop;
                p.offset = offset;
                p.id = v.id;
                p.voice = v.sent;
                p.prop = prop;
                p.value = after;
                out->add (p);
            };
            send (Prop::velocity, v.sent.velocity, now.velocity);
            send (Prop::pitch, v.sent.cents, now.cents);
            send (Prop::pan, v.sent.pan, now.pan);
            send (Prop::modX, v.sent.modX, now.modX);
            send (Prop::modY, v.sent.modY, now.modY);
        }
        now.key = v.source.key;
        v.sent = now;
    }

    static bool finished (const EnvelopeRuntime& rt, const Voice& v) noexcept
    {
        if (! rt.enabled || v.matched == 0)
            return true;
        for (int g = 0; g < 16; ++g)
            if ((v.matched & (1u << g)) != 0)
                for (const auto& p : rt.articulators[(std::size_t) g].parts)
                {
                    if (p.envOn && ! controllerVoiceFinished (p.envelope, p.envTempo ? v.beats : v.seconds, p.envTempo ? v.releaseBeats : v.releaseSeconds))
                        return false;
                    if (p.lfoOn && ! controllerVoiceFinished (p.amplitude, p.lfoTempo ? v.beats : v.seconds, p.lfoTempo ? v.releaseBeats : v.releaseSeconds))
                        return false;
                }
        return true;
    }

    Handoff<EnvelopeRuntime> settings;
    std::array<Voice, 256> voices {};
    int count = 0, nextId = 1;
};
} // namespace

std::unique_ptr<Processor> makeProcessor (Kind kind, const std::vector<std::uint8_t>& fl)
{
    switch (kind)
    {
        case Kind::keyMapper:
            return std::make_unique<KeyMapper> (parseKeyMapper (fl).value_or (defaultKeyMapper()));
        case Kind::colorMapper:
            return std::make_unique<ColorMapper> (parseColorMapper (fl).value_or (defaultColorMapper()));
        case Kind::levelScaler:
            return std::make_unique<LevelScaler> (parseLevelScaler (fl).value_or (defaultLevelScaler()));
        case Kind::keyboardSplitter:
            return std::make_unique<KeyboardSplitter> (parseKeyboardSplitter (fl).value_or (defaultKeyboardSplitter()));
        case Kind::envelope:
            return std::make_unique<Envelope> (parseEnvelope (fl).value_or (defaultEnvelope()));
        case Kind::sequencer: return makeSequencer (fl);
        default: break;
    }
    return std::make_unique<ThroughProcessor> (kind);
}
} // namespace lynx::vfx
