#include "core/ControlSurface.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cmath>
#include <functional>
#include <map>

#include "core/Effects.h"
#include "core/Instruments.h"

namespace lynx::surface
{
int firstOutput (const std::vector<SurfaceControl>& controls, int index) noexcept
{
    int k = 0;
    for (int i = 0; i < index && i < (int) controls.size(); ++i)
        k += controls[(std::size_t) i].outputs();
    return k;
}

std::string outputName (const std::vector<SurfaceControl>& controls, int k)
{
    for (const auto& c : controls)
    {
        if (k < c.outputs())
            return c.kind == SurfaceControl::Kind::xy ? c.name + (k == 0 ? " X" : " Y") : c.name;
        k -= c.outputs();
    }
    return {};
}

void setValue (std::vector<SurfaceControl>& controls, int index, float value, float valueY)
{
    if (index < 0 || index >= (int) controls.size())
        return;
    controls[(std::size_t) index].value = std::clamp (value, 0.0f, 1.0f);
    controls[(std::size_t) index].valueY = std::clamp (valueY, 0.0f, 1.0f);
}

void syncParams (EffectSlot& slot) noexcept
{
    int k = 0;
    for (const auto& c : slot.patcher.surface)
    {
        if (k < maxSurfaceOutputs)
            slot.params[(std::size_t) k] = c.value;
        if (c.kind == SurfaceControl::Kind::xy && k + 1 < maxSurfaceOutputs)
            slot.params[(std::size_t) k + 1] = c.valueY;
        k += c.outputs();
    }
}

void setValue (EffectSlot& slot, int index, float value, float valueY)
{
    setValue (slot.patcher.surface, index, value, valueY);
    syncParams (slot);
}

int add (EffectSlot& slot, SurfaceControl control)
{
    if (firstOutput (slot.patcher.surface, (int) slot.patcher.surface.size()) + control.outputs() > maxSurfaceOutputs)
        return -1;
    slot.patcher.surface.push_back (std::move (control));
    syncParams (slot);
    return (int) slot.patcher.surface.size() - 1;
}

void remove (EffectSlot& slot, int index)
{
    if (index < 0 || index >= (int) slot.patcher.surface.size())
        return;
    slot.patcher.surface.erase (slot.patcher.surface.begin() + index);
    syncParams (slot);
}

std::vector<SurfaceControl> starterControls()
{
    SurfaceControl a, b;
    a.name = "Knob 1";
    a.value = 0.5f;
    a.x = 20.0f;
    a.y = 20.0f;
    b = a;
    b.name = "Knob 2";
    b.x = 90.0f;
    return { a, b };
}

int midiValueOf (const SurfaceControl& c, float value) noexcept
{
    const auto v = std::isfinite (value) ? std::clamp (value, 0.0f, 1.0f) : 0.0f;
    const auto raw = (int) std::lround ((double) c.midiLow + (double) (c.midiHigh - c.midiLow) * (double) v);
    return std::clamp (raw, std::min (c.midiLow, c.midiHigh), std::max (c.midiLow, c.midiHigh));
}

namespace
{
// FL's Dashboard record send [DECOMP Dashboard_x64.dll 0x7b79b0; the RPN / NRPN select CCs from its table 0x81fefe]
int midiMessages (const SurfaceControl& c, int value, int channel, MidiBytes out[6]) noexcept
{
    int n = 0;
    auto put = [&] (int status, int d1, int d2, int size) {
        out[n].bytes[0] = (std::uint8_t) status;
        out[n].bytes[1] = (std::uint8_t) (d1 & 127);
        out[n].bytes[2] = (std::uint8_t) (d2 & 127);
        out[n].size = (std::uint8_t) size;
        ++n;
    };
    const auto ch = channel & 15;
    const auto wide = c.midiHigh > 127;
    value = std::max (0, value);
    switch (c.midi)
    {
        case SurfaceControl::Midi::cc:
            if (wide) // (FL sends both halves on the same CC)
            {
                put (0xB0 | ch, c.midiNumber, value >> 7, 3);
                put (0xB0 | ch, c.midiNumber, value, 3);
            }
            else
                put (0xB0 | ch, c.midiNumber, value, 3);
            break;
        case SurfaceControl::Midi::rpn:
        case SurfaceControl::Midi::nrpn:
        {
            const auto rpn = c.midi == SurfaceControl::Midi::rpn;
            put (0xB0 | ch, rpn ? 101 : 99, c.midiNumber >> 7, 3);
            put (0xB0 | ch, rpn ? 100 : 98, c.midiNumber, 3);
            if (wide)
            {
                put (0xB0 | ch, 6, value >> 7, 3);
                put (0xB0 | ch, 38, value, 3);
            }
            else
                put (0xB0 | ch, 6, value, 3);
            break;
        }
        case SurfaceControl::Midi::pressure: put (0xD0 | ch, value, 0, 2); break;
        case SurfaceControl::Midi::note:
            if (value == 0)
                put (0x80 | ch, c.midiNumber, 0, 3);
            else
                put (0x90 | ch, c.midiNumber, value, 3);
            break;
        case SurfaceControl::Midi::none: break;
    }
    return n;
}
} // namespace

int midiOf (const SurfaceControl& c, float value, int channel, MidiBytes out[6]) noexcept
{
    return midiMessages (c, midiValueOf (c, value), channel, out);
}

// FL's Dashboard state starts: i32 version 2, 6 × i32 (0 MIDI channel, 1 −1, 2 bank MSB, 3 bank LSB, 4, 5 MIDI port
// [DECOMP 0x7b50a0 → +0x15c…+0x170, 0x7b7d40: port << 24 | channel, 0x7b82e0: the bank CCs]), i32 n, n × i32
// parameters (0 = the patch selector: program + 1, 0 = none [DECOMP 0x7b8100]), …
DashboardMidi dashboardMidi (const std::vector<std::uint8_t>& d) noexcept
{
    DashboardMidi s;
    auto at = [&] (std::size_t o) {
        std::int32_t v = 0;
        std::memcpy (&v, d.data() + o, 4);
        return v;
    };
    if (d.size() < 32 || at (0) != 2)
        return s;
    s.channel = std::clamp (at (4), 0, 15);
    s.bankMsb = at (12) >= 0 && at (12) < 128 ? at (12) : -1;
    s.bankLsb = at (16) >= 0 && at (16) < 128 ? at (16) : -1;
    s.port = std::clamp (at (24), 0, 255);
    if (at (28) >= 1 && d.size() >= 36)
        s.program = std::clamp (at (32), 0, 128);
    return s;
}

void writeDashboardMidi (std::vector<std::uint8_t>& d, const DashboardMidi& s)
{
    auto put = [&] (std::size_t o, std::int32_t v) { std::memcpy (d.data() + o, &v, 4); };
    std::int32_t version = 0, params = 0;
    if (d.size() >= 32)
        std::memcpy (&version, d.data(), 4), std::memcpy (&params, d.data() + 28, 4);
    if (version != 2 || params < 1 || d.size() < 36)
    {
        // LYNX's short state: FL's header with one parameter and no records (no panel: LYNX's controls are its own)
        d.assign (40, 0);
        put (0, 2);
        put (8, -1);
        put (28, 1);
    }
    put (4, std::clamp (s.channel, 0, 15));
    put (12, s.bankMsb >= 0 && s.bankMsb < 128 ? s.bankMsb : -1);
    put (16, s.bankLsb >= 0 && s.bankLsb < 128 ? s.bankLsb : -1);
    put (24, std::clamp (s.port, 0, 255));
    put (32, std::clamp (s.program, 0, 128));
}

namespace
{
class SurfaceEffect final : public Effect
{
public:
    void prepare (double) noexcept override {}
    void reset() noexcept override {}
    void process (float*, float*, int, const EffectParams& params, double) noexcept override
    {
        for (int k = 0; k < maxSurfaceOutputs; ++k)
            values[(std::size_t) k].store (params[(std::size_t) k], std::memory_order_relaxed);
    }
    float controlOutput (int k) const noexcept override
    {
        return k >= 0 && k < maxSurfaceOutputs ? values[(std::size_t) k].load (std::memory_order_relaxed) : 0.0f;
    }

private:
    std::array<std::atomic<float>, maxSurfaceOutputs> values {};
};

// A MIDI-sending control's wanted message, packed so one compare tells "changed": the raw value (bits 0…15), the
// number (16…29), the type + 1 (30…33, 0 = sends nothing), the MIDI channel (34…37), the range's high end over 127
// (bit 38: 14-bit)
constexpr std::uint64_t packMidi (int type, int number, int value, int channel, bool wide) noexcept
{
    return (std::uint64_t) (value & 0xffff) | ((std::uint64_t) (number & 0x3fff) << 16) | ((std::uint64_t) ((type + 1) & 15) << 30)
           | ((std::uint64_t) (channel & 15) << 34) | ((std::uint64_t) (wide ? 1 : 0) << 38);
}

class DashboardInstrument final : public Instrument
{
public:
    DashboardInstrument()
    {
        for (auto& w : wanted)
            w.store (0, std::memory_order_relaxed);
        sent.fill (0);
        configuredValue.fill (-1.0f);
        configuredKey.fill (0);
    }

    void prepare (double) noexcept override {}
    void noteOn (int, float) noexcept override {}
    void noteOff (int) noexcept override {}
    void allNotesOff() noexcept override {}
    void render (float*, float*, int, float, float) noexcept override {}
    bool idle() const noexcept override { return true; }

    // (the build thread; the audio thread may run meanwhile: everything it reads is atomic)
    void configure (const ChannelDef& channel, double) noexcept override
    {
        const auto settings = dashboardMidi (channel.pluginState);
        const auto header = packHeader (settings);
        auto changed = header != wantedHeader.load (std::memory_order_relaxed);
        wantedHeader.store (header, std::memory_order_relaxed);
        int k = 0;
        for (const auto& c : channel.patcher.surface)
        {
            if (k + c.outputs() > maxOutputs)
                break;
            // a value the song changed (not one automation set since): the output and its MIDI follow it
            setFromSong (k, c.value, c, settings.channel, changed);
            if (c.kind == SurfaceControl::Kind::xy)
                setFromSong (k + 1, c.valueY, SurfaceControl {}, settings.channel, changed);
            k += c.outputs();
        }
        for (int i = k; i < count.load (std::memory_order_relaxed); ++i) // (controls removed: nothing more to send)
            if (wanted[(std::size_t) i].exchange (0, std::memory_order_relaxed) != 0)
                configuredKey[(std::size_t) i] = 0;
        count.store (k, std::memory_order_relaxed);
        if (changed)
            dirty.store (true, std::memory_order_release);
    }

    // automation / a link on output k (channelParam k): the output, and its MIDI when it sends some
    void setParameter (int k, float v) noexcept override
    {
        if (k < 0 || k >= maxOutputs || ! std::isfinite (v))
            return;
        v = std::clamp (v, 0.0f, 1.0f);
        values[(std::size_t) k].store (v, std::memory_order_relaxed);
        const auto key = wanted[(std::size_t) k].load (std::memory_order_relaxed);
        if (((key >> 30) & 15) == 0)
            return;
        const auto r = range[(std::size_t) k].load (std::memory_order_relaxed);
        const auto low = (int) (r & 0xffff), high = (int) (r >> 16);
        const auto raw = (int) std::lround ((double) low + (double) (high - low) * (double) v);
        const auto next = (key & ~(std::uint64_t) 0xffff) | (std::uint64_t) (raw & 0xffff);
        if (next != key)
        {
            wanted[(std::size_t) k].store (next, std::memory_order_relaxed);
            dirty.store (true, std::memory_order_release);
        }
    }

    float controlOutput (int k) const noexcept override
    {
        return k >= 0 && k < maxOutputs ? values[(std::size_t) k].load (std::memory_order_relaxed) : 0.0f;
    }

    int takeMidiMessages (std::uint32_t* out, int max) noexcept override
    {
        if (! dirty.exchange (false, std::memory_order_acquire))
            return 0;
        int n = 0;
        auto put = [&] (int status, int d1, int d2) { out[n++] = (std::uint32_t) (status & 0xff) | ((std::uint32_t) (d1 & 127) << 8) | ((std::uint32_t) (d2 & 127) << 16); };
        // the bank select and program first (FL sends them at load and when the patch changes)
        const auto header = wantedHeader.load (std::memory_order_relaxed);
        if (header != sentHeader && max >= 3)
        {
            sentHeader = header;
            const auto channel = (int) (header & 15);
            const auto program = (int) ((header >> 4) & 0xff), msb = (int) ((header >> 12) & 0xff), lsb = (int) ((header >> 20) & 0xff);
            if (msb < 128)
                put (0xB0 | channel, 0, msb);
            if (lsb < 128)
                put (0xB0 | channel, 32, lsb);
            if (program > 0)
                put (0xC0 | channel, program - 1, 0);
        }
        const auto total = count.load (std::memory_order_relaxed);
        for (int k = resume; k < total; ++k)
        {
            const auto key = wanted[(std::size_t) k].load (std::memory_order_relaxed);
            if (key == sent[(std::size_t) k])
                continue;
            if (n + 6 > max) // the rest next time
            {
                resume = k;
                dirty.store (true, std::memory_order_relaxed);
                return n;
            }
            sent[(std::size_t) k] = key;
            if (((key >> 30) & 15) == 0)
                continue;
            SurfaceControl c;
            c.midi = (SurfaceControl::Midi) ((int) ((key >> 30) & 15) - 1);
            c.midiNumber = (int) ((key >> 16) & 0x3fff);
            c.midiLow = 0;
            c.midiHigh = ((key >> 38) & 1) != 0 ? 16383 : 127;
            MidiBytes m[6];
            const auto value = (int) (key & 0xffff);
            const auto made = midiMessages (c, value, (int) ((key >> 34) & 15), m);
            for (int i = 0; i < made; ++i)
                put (m[i].bytes[0], m[i].bytes[1], m[i].bytes[2]);
        }
        resume = 0;
        return n;
    }

private:
    static std::uint64_t packHeader (const DashboardMidi& s) noexcept
    {
        const auto byte = [] (int v) { return (std::uint64_t) (v < 0 || v > 127 ? 0xff : v); };
        return (std::uint64_t) (s.channel & 15) | ((std::uint64_t) std::clamp (s.program, 0, 128) << 4) | (byte (s.bankMsb) << 12) | (byte (s.bankLsb) << 20);
    }

    void setFromSong (int k, float value, const SurfaceControl& c, int channel, bool& changed) noexcept
    {
        const auto key = c.midi == SurfaceControl::Midi::none
                             ? 0
                             : packMidi ((int) c.midi, c.midiNumber, midiValueOf (c, value), channel, c.midiHigh > 127);
        if (value != configuredValue[(std::size_t) k] || key != configuredKey[(std::size_t) k])
        {
            configuredValue[(std::size_t) k] = value;
            configuredKey[(std::size_t) k] = key;
            values[(std::size_t) k].store (value, std::memory_order_relaxed);
            range[(std::size_t) k].store ((std::uint32_t) std::clamp (c.midiLow, 0, 0xffff) | ((std::uint32_t) std::clamp (c.midiHigh, 0, 0xffff) << 16),
                                          std::memory_order_relaxed);
            wanted[(std::size_t) k].store (key, std::memory_order_relaxed);
            changed = true;
        }
    }

    std::array<std::atomic<float>, maxOutputs> values {};
    std::array<std::atomic<std::uint64_t>, maxOutputs> wanted;
    std::array<std::atomic<std::uint32_t>, maxOutputs> range {};
    std::atomic<std::uint64_t> wantedHeader { 0 };
    std::atomic<int> count { 0 };
    std::atomic<bool> dirty { false };
    // the build thread's own (what configure last saw)
    std::array<float, maxOutputs> configuredValue;
    std::array<std::uint64_t, maxOutputs> configuredKey;
    // the audio thread's own (what it sent)
    std::array<std::uint64_t, maxOutputs> sent;
    std::uint64_t sentHeader = ~(std::uint64_t) 0;
    int resume = 0;
};
} // namespace

std::unique_ptr<Effect> makeEffect()
{
    return std::make_unique<SurfaceEffect>();
}

std::unique_ptr<Instrument> makeDashboard()
{
    return std::make_unique<DashboardInstrument>();
}

namespace
{
struct FlControl
{
    int type = -1;
    std::string name;
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    std::vector<std::pair<int, float>> outputs; // (FL's output index, value)
};

// FL's chunk tree (REA_rodada164a §5): u32 id + u64 size; 0 = group, 0x834 type, 0x837 name (UTF-16), 0x838 rect
// (4 × f32), 0x836 a published value (f32 value, f32, i32 output index), 0x835 end of a control
std::vector<FlControl> readFlControls (const std::vector<std::uint8_t>& d)
{
    std::vector<FlControl> controls;
    FlControl current;
    std::function<void (std::size_t, std::size_t, int)> walk = [&] (std::size_t pos, std::size_t end, int depth) {
        while (pos + 12 <= end)
        {
            std::uint32_t id = 0;
            std::uint64_t size = 0;
            std::memcpy (&id, d.data() + pos, 4);
            std::memcpy (&size, d.data() + pos + 4, 8);
            if (size > end - pos - 12)
                return;
            const auto body = pos + 12;
            const auto n = (std::size_t) size;
            if (id == 0 && depth < 8)
                walk (body, body + n, depth + 1);
            else if (id == 0x834 && n >= 4)
            {
                current = {};
                std::memcpy (&current.type, d.data() + body, 4);
            }
            else if (id == 0x837)
            {
                std::u16string text;
                for (std::size_t k = 0; k + 1 < n; k += 2)
                    text.push_back ((char16_t) (d[body + k] | (d[body + k + 1] << 8)));
                std::string utf8;
                for (const auto c : text) // (names are ASCII in practice; others become '?')
                    if (c != 0)
                        utf8.push_back (c < 0x80 ? (char) c : '?');
                current.name = utf8;
            }
            else if (id == 0x838 && n >= 16)
            {
                std::memcpy (&current.x, d.data() + body, 4);
                std::memcpy (&current.y, d.data() + body + 4, 4);
                std::memcpy (&current.w, d.data() + body + 8, 4);
                std::memcpy (&current.h, d.data() + body + 12, 4);
            }
            else if (id == 0x836 && n >= 12)
            {
                float v = 0.0f;
                std::int32_t index = 0;
                std::memcpy (&v, d.data() + body, 4);
                std::memcpy (&index, d.data() + body + 8, 4);
                current.outputs.push_back ({ index, std::isfinite (v) ? std::clamp (v, 0.0f, 1.0f) : 0.0f });
            }
            else if (id == 0x835)
            {
                if (! current.outputs.empty())
                    controls.push_back (current);
                current = {};
            }
            pos = body + n;
        }
    };
    if (d.size() >= 16)
        walk (4, d.size(), 0);
    std::stable_sort (controls.begin(), controls.end(), [] (const FlControl& a, const FlControl& b) { return a.outputs[0].first < b.outputs[0].first; });
    return controls;
}
} // namespace

std::optional<std::vector<SurfaceControl>> parseFlControlSurface (const std::vector<std::uint8_t>& state)
{
    const auto fl = readFlControls (state);
    if (fl.empty())
        return std::nullopt;
    // FL's control types seen in FL's own projects (REA_rodada164a §5): 0 knob (TVectorWheel), 2 slider, 3 button;
    // 5 bevel and 6 label publish nothing (dropped: LYNX's panel has no decorations yet). A control with two values is
    // an XY pad; any other one with a value becomes a knob.
    std::vector<SurfaceControl> controls;
    std::map<SurfaceControl::Kind, int> count;
    float minX = 1.0e9f, minY = 1.0e9f;
    for (const auto& c : fl)
        minX = std::min (minX, c.x), minY = std::min (minY, c.y);
    for (const auto& c : fl)
    {
        SurfaceControl s;
        s.kind = c.outputs.size() >= 2 ? SurfaceControl::Kind::xy
                 : c.type == 2         ? SurfaceControl::Kind::slider
                 : c.type == 3         ? SurfaceControl::Kind::button
                                       : SurfaceControl::Kind::knob;
        if (firstOutput (controls, (int) controls.size()) + s.outputs() > maxSurfaceOutputs)
            break;
        static const char* names[] = { "Knob", "Slider", "Button", "Check box", "XY" };
        const auto number = ++count[s.kind];
        s.name = c.name.find_first_not_of (' ') != std::string::npos ? c.name : std::string (names[(int) s.kind]) + " " + std::to_string (number);
        s.value = c.outputs[0].second;
        s.valueY = c.outputs.size() >= 2 ? c.outputs[1].second : 0.5f;
        // FL's place (its panel's own pixels, from the top-left control), on LYNX's 10 px grid
        s.x = 20.0f + std::round ((c.x - minX) / 10.0f) * 10.0f;
        s.y = 20.0f + std::round ((c.y - minY) / 10.0f) * 10.0f;
        if (s.kind == SurfaceControl::Kind::knob && c.w > 0.0f)
            s.size = std::clamp (c.w / 56.0f, 0.5f, 3.0f);
        controls.push_back (std::move (s));
    }
    if (controls.empty())
        return std::nullopt;
    return controls;
}

std::optional<int> flOutputToLynx (const std::vector<std::uint8_t>& state, int flOutput)
{
    int k = 0;
    for (const auto& c : readFlControls (state))
    {
        const auto outputs = c.outputs.size() >= 2 ? 2 : 1;
        for (int i = 0; i < outputs && i < (int) c.outputs.size(); ++i)
            if (c.outputs[(std::size_t) i].first == flOutput)
                return k + i < maxSurfaceOutputs ? std::optional<int> (k + i) : std::nullopt;
        k += outputs;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------------------------
// FL's Dashboard (REA_rodada164f-flimport [DECOMP Dashboard_x64.dll 0x7b67c0, 0x7b8160, 0x7ab6f0, 0x7b50a0; DATA the
// presets in Data/Patches/Plugin presets/Generators/Dashboard]):
//   i32 version (2), 6 × i32, i32 n + n × i32 parameters (0 = the patch selector, k = record k − 1, raw value),
//   i32 m + m × 81-byte records (string[47] name, string[15] short name, i32 number at +0x40 — bit 31 off, low 30
//   bits the CC / output number —, i32 type at +0x44 — 4 = controller output, 0 = CC, 3 = program… —, i32 min +0x48,
//   i32 max +0x4C, u8), then the panel: i32 version, i32 size, string name, string bitmap, u8, u8, i32 colour,
//   width / height (i32 before version 5, f32 after), i32 + blob, (v > 2) 2 strings, (v > 3) i32 count + strings +
//   i32, 64 bytes, i32 control count; each control: u8 type, string skin, i32 length + data (string name, string
//   caption, i32 x, y, w, h, …), 16 bytes. Types: 0 digit, 1 slider, 2 wheel, 3 panel, 4 switch, 7 selector hold a
//   value (the panel's value list, in order = the records); 5 label, 6 image, 8 patch selector, 9 page selector don't.
namespace
{
struct Reader
{
    const std::vector<std::uint8_t>& d;
    std::size_t p = 0;
    bool ok = true;
    bool need (std::size_t n)
    {
        if (! ok || n > d.size() - std::min (p, d.size()))
            ok = false;
        return ok;
    }
    std::int32_t i32()
    {
        std::int32_t v = 0;
        if (need (4))
            std::memcpy (&v, d.data() + p, 4), p += 4;
        return v;
    }
    float f32()
    {
        float v = 0.0f;
        if (need (4))
            std::memcpy (&v, d.data() + p, 4), p += 4;
        return v;
    }
    std::uint8_t u8() { return need (1) ? d[p++] : 0; }
    std::string str() // Delphi short string
    {
        const auto n = (std::size_t) u8();
        std::string s;
        if (need (n))
            s.assign ((const char*) d.data() + p, n), p += n;
        return s;
    }
    void skip (std::size_t n)
    {
        if (need (n))
            p += n;
    }
};

struct DashControl
{
    int type = -1;
    std::string name;
    int x = 0, y = 0, w = 0, h = 0;
};

struct DashRecord
{
    std::string name;
    std::int32_t number = -1, type = 0, low = 0, high = 0;
};

struct DashState
{
    std::vector<std::int32_t> params;
    std::vector<DashRecord> records;
    std::vector<DashControl> values; // the value controls, in order
};

std::string shortString (const std::uint8_t* at, std::size_t room)
{
    const auto n = std::min<std::size_t> (at[0], room - 1);
    return std::string ((const char*) at + 1, n);
}

std::optional<DashState> readDashboard (const std::vector<std::uint8_t>& state)
{
    Reader r { state };
    DashState s;
    if (r.i32() != 2)
        return std::nullopt;
    r.skip (24);
    const auto n = r.i32();
    if (! r.ok || n < 0 || n > 4096)
        return std::nullopt;
    for (int i = 0; i < n; ++i)
        s.params.push_back (r.i32());
    const auto m = r.i32();
    if (! r.ok || m < 0 || m > 4096 || ! r.need ((std::size_t) m * 81))
        return std::nullopt;
    for (int i = 0; i < m; ++i)
    {
        const auto* at = state.data() + r.p;
        DashRecord rec;
        rec.name = shortString (at, 48);
        std::memcpy (&rec.number, at + 0x40, 4);
        std::memcpy (&rec.type, at + 0x44, 4);
        std::memcpy (&rec.low, at + 0x48, 4);
        std::memcpy (&rec.high, at + 0x4C, 4);
        s.records.push_back (std::move (rec));
        r.p += 81;
    }
    // the panel
    const auto version = r.i32();
    r.i32(); // size
    r.str();
    r.str();
    r.u8();
    r.u8();
    r.i32(); // colour
    r.skip (8); // width, height (i32 or f32)
    const auto blob = r.i32();
    if (! r.ok || blob < 0)
        return std::nullopt;
    r.skip ((std::size_t) blob);
    if (version > 2)
        r.str(), r.str();
    if (version > 3)
    {
        const auto count = r.i32();
        if (! r.ok || count < 0 || count > 4096)
            return std::nullopt;
        for (int i = 0; i < count; ++i)
            r.str();
        r.i32();
    }
    r.skip (64);
    const auto controls = r.i32();
    if (! r.ok || controls < 0 || controls > 4096)
        return std::nullopt;
    for (int i = 0; i < controls && r.ok; ++i)
    {
        DashControl c;
        c.type = r.u8();
        r.str(); // skin
        const auto length = r.i32();
        if (! r.ok || length < 0 || ! r.need ((std::size_t) length + 16))
            return std::nullopt;
        const std::vector<std::uint8_t> data (state.begin() + (std::ptrdiff_t) r.p, state.begin() + (std::ptrdiff_t) (r.p + (std::size_t) length));
        r.p += (std::size_t) length + 16;
        Reader dr { data };
        c.name = dr.str();
        dr.str(); // caption
        c.x = dr.i32();
        c.y = dr.i32();
        c.w = dr.i32();
        c.h = dr.i32();
        const auto holdsValue = c.type == 0 || c.type == 1 || c.type == 2 || c.type == 3 || c.type == 4 || c.type == 7;
        if (holdsValue)
            s.values.push_back (std::move (c));
    }
    if (! r.ok)
        return std::nullopt;
    return s;
}
} // namespace

std::optional<FlDashboard> parseFlDashboard (const std::vector<std::uint8_t>& state)
{
    const auto s = readDashboard (state);
    if (! s)
        return std::nullopt;
    FlDashboard out;
    std::map<SurfaceControl::Kind, int> count;
    struct Placed
    {
        SurfaceControl control;
        const DashControl* fl;
    };
    std::vector<Placed> placed;
    for (std::size_t k = 0; k < s->values.size(); ++k)
    {
        const auto& c = s->values[k];
        SurfaceControl sc;
        sc.kind = c.type == 1 ? SurfaceControl::Kind::slider : c.type == 4 ? SurfaceControl::Kind::checkBox : SurfaceControl::Kind::knob;
        if (firstOutput (out.controls, (int) out.controls.size()) + sc.outputs() > maxOutputs)
            break;
        const auto* rec = k < s->records.size() ? &s->records[k] : nullptr;
        static const char* names[] = { "Knob", "Slider", "Button", "Check box", "XY" };
        const auto number = ++count[sc.kind];
        sc.name = rec != nullptr && rec->name.find_first_not_of (' ') != std::string::npos ? rec->name
                  : c.name.find_first_not_of (' ') != std::string::npos                    ? c.name
                                                                                            : std::string (names[(int) sc.kind]) + " " + std::to_string (number);
        // the record's value (parameter k + 1) within its range
        if (rec != nullptr && k + 1 < s->params.size() && rec->high != rec->low)
            sc.value = std::clamp ((float) (s->params[k + 1] - rec->low) / (float) (rec->high - rec->low), 0.0f, 1.0f);
        if (sc.kind == SurfaceControl::Kind::knob)
            sc.size = std::clamp ((float) c.w / 56.0f, 0.5f, 3.0f);
        else if (sc.kind == SurfaceControl::Kind::slider)
            sc.size = std::clamp ((float) c.h / 140.0f, 0.5f, 3.0f);
        else
            sc.size = 0.5f;
        // a record of another type sends MIDI on the Dashboard's port when active (number ≥ 0; bit 31 = deactivated
        // until the user moves the control in FL — LYNX leaves those silent) [DECOMP 0x7b79b0, 0x7b5f70]
        if (rec != nullptr && rec->number >= 0 && (rec->type == 0 || rec->type == 1 || rec->type == 2 || rec->type == 3 || rec->type == 5))
        {
            sc.midi = (SurfaceControl::Midi) rec->type;
            sc.midiNumber = (int) (rec->number & 0x3fff);
            sc.midiLow = std::clamp (rec->low, 0, 16383);
            sc.midiHigh = std::clamp (rec->high, 0, 16383);
        }
        out.flOutputs.push_back (rec != nullptr && rec->type == 4 && rec->number != -1 ? (int) (rec->number & 0x3fffffff) : -1);
        out.controls.push_back (sc);
        placed.push_back ({ sc, &c });
    }
    if (out.controls.empty())
        return out;
    // FL's places, scaled so that each LYNX control (bigger than FL's small skins) fits in its FL box scaled the same
    // way (then no two overlap when FL's didn't), at most × 3; from the top-left control, on LYNX's 10 px grid
    auto lynxSize = [] (const SurfaceControl& c) -> std::pair<float, float> {
        const auto sz = std::max (0.5f, c.size);
        switch (c.kind)
        {
            case SurfaceControl::Kind::slider: return { 40.0f, 140.0f * sz };
            case SurfaceControl::Kind::checkBox: return { 110.0f * sz, 24.0f };
            default: return { 56.0f * sz, 56.0f * sz + 14.0f };
        }
    };
    float scale = 1.0f;
    int minX = 1 << 30, minY = 1 << 30;
    for (const auto& p : placed)
    {
        const auto [w, h] = lynxSize (p.control);
        scale = std::max ({ scale, w / (float) std::max (1, p.fl->w), h / (float) std::max (1, p.fl->h) });
        minX = std::min (minX, p.fl->x);
        minY = std::min (minY, p.fl->y);
    }
    scale = std::min (scale, 3.0f);
    for (std::size_t i = 0; i < placed.size(); ++i)
    {
        out.controls[i].x = 20.0f + std::round ((float) (placed[i].fl->x - minX) * scale / 10.0f) * 10.0f;
        out.controls[i].y = 20.0f + std::round ((float) (placed[i].fl->y - minY) * scale / 10.0f) * 10.0f;
    }
    return out;
}

std::optional<int> flDashboardOutputToLynx (const std::vector<std::uint8_t>& state, int flOutput)
{
    const auto d = parseFlDashboard (state);
    if (! d)
        return std::nullopt;
    for (std::size_t k = 0; k < d->flOutputs.size(); ++k)
        if (d->flOutputs[k] == flOutput)
            return (int) k;
    return std::nullopt;
}

} // namespace lynx::surface
