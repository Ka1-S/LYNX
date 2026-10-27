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
        if (k < maxOutputs)
            slot.params[(std::size_t) k] = c.value;
        if (c.kind == SurfaceControl::Kind::xy && k + 1 < maxOutputs)
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
    if (firstOutput (slot.patcher.surface, (int) slot.patcher.surface.size()) + control.outputs() > maxOutputs)
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

namespace
{
class SurfaceEffect final : public Effect
{
public:
    void prepare (double) noexcept override {}
    void reset() noexcept override {}
    void process (float*, float*, int, const EffectParams& params, double) noexcept override
    {
        for (int k = 0; k < maxOutputs; ++k)
            values[(std::size_t) k].store (params[(std::size_t) k], std::memory_order_relaxed);
    }
    float controlOutput (int k) const noexcept override
    {
        return k >= 0 && k < maxOutputs ? values[(std::size_t) k].load (std::memory_order_relaxed) : 0.0f;
    }

private:
    std::array<std::atomic<float>, maxOutputs> values {};
};

class DashboardInstrument final : public Instrument
{
public:
    void prepare (double) noexcept override {}
    void noteOn (int, float) noexcept override {}
    void noteOff (int) noexcept override {}
    void allNotesOff() noexcept override {}
    void render (float*, float*, int, float, float) noexcept override {}
    bool idle() const noexcept override { return true; }
    void configure (const ChannelDef& channel, double) noexcept override
    {
        int k = 0;
        for (const auto& c : channel.patcher.surface)
        {
            if (k < maxOutputs)
                values[(std::size_t) k].store (c.value, std::memory_order_relaxed);
            if (c.kind == SurfaceControl::Kind::xy && k + 1 < maxOutputs)
                values[(std::size_t) k + 1].store (c.valueY, std::memory_order_relaxed);
            k += c.outputs();
        }
    }
    float controlOutput (int k) const noexcept override
    {
        return k >= 0 && k < maxOutputs ? values[(std::size_t) k].load (std::memory_order_relaxed) : 0.0f;
    }

private:
    std::array<std::atomic<float>, maxOutputs> values {};
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
        if (firstOutput (controls, (int) controls.size()) + s.outputs() > maxOutputs)
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
                return k + i < maxOutputs ? std::optional<int> (k + i) : std::nullopt;
        k += outputs;
    }
    return std::nullopt;
}

} // namespace lynx::surface
