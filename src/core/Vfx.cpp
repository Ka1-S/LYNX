#include "core/Vfx.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

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
            if (! s.enabled)
            {
                send (e, e.voice, out);
                continue;
            }
            // the row of the input's pitch class (after the offset), its notes from that octave's C
            const auto shifted = e.voice.key - s.offset;
            const auto pc = ((shifted % 12) + 12) % 12;
            const auto octave = shifted - pc;
            const auto& row = s.rows[(std::size_t) pc];
            if (row.empty())
                continue;
            auto play = [&] (int value, float gain) {
                auto v = e.voice;
                v.key = octave + value + s.offset;
                if (v.key < 0 || v.key > 127)
                    return;
                v.velocity *= gain;
                send (e, v, out);
            };
            const auto n = (int) row.size();
            switch (s.multimode)
            {
                case 1: // all, at the level of one note
                    for (const auto value : row)
                        play (value, 1.0f / std::sqrt ((float) n));
                    break;
                case 2: // one of them at random
                    play (row[(std::size_t) (nextRandom() % (std::uint32_t) n)], 1.0f);
                    break;
                case 3:
                case 4: // the next one each time, up or down
                {
                    auto& c = cycle[(std::size_t) pc];
                    c = s.multimode == 3 ? (c + 1) % n : (c - 1 + n) % n;
                    if (c < 0 || c >= n)
                        c = 0;
                    std::array<int, 128> sorted {};
                    const auto m = std::min (n, 128);
                    std::copy (row.begin(), row.begin() + m, sorted.begin());
                    std::sort (sorted.begin(), sorted.begin() + m);
                    play (sorted[(std::size_t) std::min (c, m - 1)], 1.0f);
                    break;
                }
                default:
                    for (const auto value : row)
                        play (value, 1.0f);
                    break;
            }
        }
    }

private:
    void send (const Event& input, const Voice& v, EventList& out) noexcept
    {
        if (const auto id = voices.add (input.id, 0, v); id >= 0)
            out.add (onEvent (input.offset, 0, id, v));
    }
    std::uint32_t nextRandom() noexcept
    {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    }
    std::array<int, 12> cycle { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    std::uint32_t seed = 0x1234567u;
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

std::unique_ptr<Processor> makeProcessor (Kind kind, const std::vector<std::uint8_t>& fl)
{
    switch (kind)
    {
        case Kind::keyMapper:
            return std::make_unique<KeyMapper> (parseKeyMapper (fl).value_or (defaultKeyMapper()));
        case Kind::colorMapper:
            return std::make_unique<ColorMapper> (parseColorMapper (fl).value_or (defaultColorMapper()));
        default: break;
    }
    return std::make_unique<ThroughProcessor> (kind);
}
} // namespace lynx::vfx
