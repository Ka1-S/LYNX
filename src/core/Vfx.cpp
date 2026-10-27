#include "core/Vfx.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>

#include "core/Song.h"
#include "flp/Flp.h"

namespace lynx::vfx
{
namespace
{
constexpr std::uint32_t lynxMagic = 0x5846564c; // "LVFX"

struct Reader
{
    const std::uint8_t* data;
    std::size_t size, pos = 0;
    bool failed = false;

    template <typename T>
    T get() noexcept
    {
        T v {};
        if (failed || sizeof (T) > size - pos)
        {
            failed = true;
            return v;
        }
        std::memcpy (&v, data + pos, sizeof (T));
        pos += sizeof (T);
        return v;
    }
    std::int32_t i32() noexcept { return get<std::int32_t>(); }
    std::uint32_t u32() noexcept { return get<std::uint32_t>(); }
    std::size_t left() const noexcept { return failed ? 0 : size - pos; }
};

template <typename T>
void put (std::vector<std::uint8_t>& out, T v)
{
    const auto* p = reinterpret_cast<const std::uint8_t*> (&v);
    out.insert (out.end(), p, p + sizeof (T));
}

// Settings handed from the UI thread to the audio thread without locks or allocation on the audio side (the
// Patcher graph's pattern): the UI publishes a new object, the audio thread swaps it in and parks the old one, the
// UI frees it on its next call.
template <typename T>
class Handoff
{
public:
    explicit Handoff (T initial) : active (std::make_unique<T> (std::move (initial))) {}
    ~Handoff()
    {
        delete pending.exchange (nullptr);
        delete retired.exchange (nullptr);
    }
    void publish (T value)
    {
        delete retired.exchange (nullptr);
        delete pending.exchange (new T (std::move (value)));
    }
    // audio thread: true when new settings arrived
    bool take() noexcept
    {
        auto* incoming = pending.exchange (nullptr);
        if (incoming == nullptr)
            return false;
        auto* old = active.release();
        active.reset (incoming);
        delete retired.exchange (old); // (normally null: the UI frees it on its next call)
        return true;
    }
    const T& get() const noexcept { return *active; }

private:
    std::unique_ptr<T> active;
    std::atomic<T*> pending { nullptr };
    std::atomic<T*> retired { nullptr };
};

} // namespace

int inputId (int key, int colour) noexcept { return (colour & 15) * 128 + (key & 127); }

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

std::vector<std::uint8_t> defaultState (Kind kind)
{
    switch (kind)
    {
        case Kind::keyMapper: return writeKeyMapper (defaultKeyMapper());
        case Kind::colorMapper: return writeColorMapper (defaultColorMapper());
        case Kind::levelScaler: return writeLevelScaler (defaultLevelScaler());
        default: break;
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
// (FL re-evaluates the curves every tick from the parent's live levels; LYNX applies them when the note starts and
// to its release velocity when it ends: the voices' levels do not change on their way here.)
class LevelScaler final : public Base<LevelScalerSettings>
{
public:
    explicit LevelScaler (LevelScalerSettings s) : Base (std::move (s)) {}
    Kind kind() const noexcept override { return Kind::levelScaler; }
    bool configure (const std::vector<std::uint8_t>& fl) override
    {
        auto s = parseLevelScaler (fl);
        if (! s)
            return false;
        publish (std::move (*s));
        return true;
    }
    void process (const Event* in, int count, int, const Transport&, EventList& out) noexcept override
    {
        using L = LevelScalerSettings;
        const auto& s = current();
        for (int i = 0; i < count; ++i)
        {
            const auto& e = in[i];
            if (e.type == Event::Type::on)
            {
                auto v = e.voice;
                // (FL's order: pan, Mod X, Mod Y, fine pitch, velocity; each with its own random, fixed for the note)
                v.pan = (float) levelScale (s, L::pan, v.pan, -1.0, 1.0, random());
                v.modX = (float) (levelScale (s, L::modX, v.modX * 2.0 - 1.0, -1.0, 1.0, random()) * 0.5 + 0.5);
                v.modY = (float) (levelScale (s, L::modY, v.modY * 2.0 - 1.0, -1.0, 1.0, random()) * 0.5 + 0.5);
                v.cents = (float) (levelScale (s, L::finePitch, v.cents / 1200.0, -1.0, 1.0, random()) * 1200.0);
                v.velocity = (float) levelScale (s, L::velocity, v.velocity, 0.0, 1.0, random());
                if (const auto id = voices.add (e.id, 0, v); id >= 0)
                    out.add (onEvent (e.offset, 0, id, v));
            }
            else if (e.type == Event::Type::off)
                voices.release (e.id, e.offset, (float) levelScale (s, L::release, e.voice.release, 0.0, 1.0, random()), out);
        }
    }

private:
    double random() noexcept
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return (double) (seed >> 8) / 16777216.0;
    }
    std::uint32_t seed = 0x2545f491u;
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
        default: break;
    }
    return std::make_unique<ThroughProcessor> (kind);
}
} // namespace lynx::vfx
