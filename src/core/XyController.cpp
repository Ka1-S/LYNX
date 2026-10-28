#include "core/XyController.h"

#include "core/FlCurve.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/Effects.h"

namespace lynx::xy
{
int axesOf (EffectKind kind) noexcept
{
    return kind == EffectKind::xyController ? 2 : kind == EffectKind::xyzController ? 3 : 0;
}

int paramCount (EffectKind kind) noexcept
{
    return kind == EffectKind::xyzController ? curveMode (2) + 1 : axesOf (kind) * 4 + 2;
}

int curveModeOf (float value) noexcept
{
    return std::clamp ((int) std::lround (std::isfinite (value) ? value * 12.0f : 0.0f), 0, flcurve::modeCount - 1);
}

EffectParams defaults (EffectKind kind) noexcept
{
    EffectParams p {};
    for (int a = 0; a < axesOf (kind); ++a)
    {
        p[(std::size_t) level (a)] = 0.5f;
        p[(std::size_t) minimum (a)] = 0.0f;
        p[(std::size_t) maximum (a)] = 1.0f;
        p[(std::size_t) tension (a)] = 0.5f;
    }
    if (axesOf (kind) > 0)
    {
        p[(std::size_t) speed (kind)] = 0.5f;
        p[(std::size_t) acceleration (kind)] = 0.5f;
    }
    return p;
}

double shape (double p, float tension01) noexcept
{
    p = std::clamp (p, 0.0, 1.0);
    const auto t = std::clamp (((double) tension01 - 0.5) * 256.0, -128.0, 128.0);
    if (std::abs (t) < 0.5)
        return p;
    const auto k = 0.1 * (std::pow (1001.0, std::abs (t) / 128.0) - 1.0);
    return t > 0.0 ? 1.0 - (std::pow (k + 1.0, 1.0 - p) - 1.0) / k : (std::pow (k + 1.0, p) - 1.0) / k;
}

float output (EffectKind kind, const EffectParams& params, int axis, double position) noexcept
{
    const auto lo = (double) params[(std::size_t) minimum (axis)], hi = (double) params[(std::size_t) maximum (axis)];
    const auto f = kind == EffectKind::xyzController
                       ? flcurve::shape (curveModeOf (params[(std::size_t) curveMode (axis)]), params[(std::size_t) tension (axis)] * 2.0f - 1.0f,
                                         std::clamp (position, 0.0, 1.0))
                       : shape (position, params[(std::size_t) tension (axis)]);
    return (float) std::clamp (lo + (hi - lo) * f, 0.0, 1.0);
}

namespace
{
// FL's tick: the laws below are per tick of a 96 PPQ song (FL scales them by 96 / PPQ), so they follow the tempo
constexpr double ticksPerBeat = 96.0;
}

void Mover::step (const EffectParams& params, EffectKind kind, double seconds, double bpm) noexcept
{
    const auto axes = axesOf (kind);
    if (! started)
    {
        for (int a = 0; a < axes; ++a)
            position[(std::size_t) a] = stage1[(std::size_t) a] = stage2[(std::size_t) a] = params[(std::size_t) level (a)];
        velocity = {};
        started = true;
        return;
    }
    ticks += seconds * std::max (10.0, bpm) / 60.0 * ticksPerBeat;
    const auto whole = (int) std::min (ticks, 4096.0);
    ticks -= std::floor (ticks);
    if (whole <= 0)
        return;

    if (kind == EffectKind::xyController)
    {
        // FL's X-Y Controller [REA_rodada164a §3b, DECOMP 0x6C6D90]: one 2-D point with a velocity. Speed (FL 1024…2048)
        // sets the pull A, acceleration (256…1024) the inertia D; near the goal (< 0.3) the inertia fades out so it
        // lands without overshoot.
        const auto speedRaw = 1024.0 + 1024.0 * (double) params[(std::size_t) speed (kind)];
        const auto accelRaw = 256.0 + 768.0 * (double) params[(std::size_t) acceleration (kind)];
        const auto pull = speedRaw >= 2047.5 ? 1.0 : (std::pow (1.0e8, speedRaw / 2304.0) - 1.0) * 1.0e-8 + 1.0e-4;
        const auto inertia = accelRaw >= 1023.5 ? 0.0 : std::max (0.0, 1.0 - (std::pow (1001.0, accelRaw / 1024.0) - 1.0) * 0.001);
        const auto gx = (double) params[(std::size_t) level (0)], gy = (double) params[(std::size_t) level (1)];
        for (int t = 0; t < whole; ++t)
        {
            const auto dx = gx - position[0], dy = gy - position[1];
            const auto dist = std::sqrt (dx * dx + dy * dy);
            if (dist < 1.0e-9 && std::abs (velocity[0]) + std::abs (velocity[1]) < 1.0e-9)
            {
                position[0] = gx, position[1] = gy, velocity = {};
                break;
            }
            const auto d = dist < 0.3 ? inertia * (1.0 - (std::pow (10.0, 10.0 * (1.0 - dist / 0.3)) - 1.0) * 1.0e-10) : inertia;
            const auto stepLength = std::min (pull * (1.0 - inertia), dist);
            const auto ux = dist > 0.0 ? dx / dist : 0.0, uy = dist > 0.0 ? dy / dist : 0.0;
            velocity[0] = velocity[0] * d + ux * stepLength;
            velocity[1] = velocity[1] * d + uy * stepLength;
            position[0] = std::clamp (position[0] + velocity[0], 0.0, 1.0);
            position[1] = std::clamp (position[1] + velocity[1], 0.0, 1.0);
        }
        return;
    }

    // FL's X-Y-Z Controller [REA_rodada164a §3, DECOMP 0x83C400 / 0x604A90]: per axis, up to three one-pole stages in
    // series. Time T = (17^(P9/4096) − 1) · 24 ticks (LYNX's Speed s: P9 = 4096 (1 − s)); acceleration acc (LYNX's
    // Accel a: acc = 1 + 4a) splits it: n = round (acc + 0.5) stages, the first n − 1 of T / √acc, the last of that
    // × (1 − (n − acc)); a stage's coefficient is 0.01^(1 / Ts) per tick.
    const auto p9 = 4096.0 * (1.0 - (double) params[(std::size_t) speed (kind)]);
    const auto total = (std::pow (17.0, p9 / 4096.0) - 1.0) * 24.0;
    const auto acc = 1.0 + 4.0 * (double) params[(std::size_t) acceleration (kind)];
    const auto n = std::clamp ((int) std::lround (acc + 0.49999), 1, 3);
    std::array<double, 3> coefficient {};
    for (int i = 0; i < 3; ++i)
    {
        auto ts = total / std::sqrt (acc);
        if (i == n - 1)
            ts *= 1.0 - ((double) n - acc);
        else if (i > n - 1)
            ts = 0.0;
        coefficient[(std::size_t) i] = ts > 0.0 ? std::pow (0.01, 1.0 / ts) : 0.0;
    }
    for (int a = 0; a < axes; ++a)
    {
        const auto goal = (double) params[(std::size_t) level (a)];
        auto& y0 = stage1[(std::size_t) a];
        auto& y1 = stage2[(std::size_t) a];
        auto& y2 = position[(std::size_t) a];
        if (total <= 0.0)
        {
            y0 = y1 = y2 = goal;
            continue;
        }
        for (int t = 0; t < whole; ++t)
        {
            y0 = goal + (y0 - goal) * coefficient[0];
            y1 = y0 + (y1 - y0) * coefficient[1];
            y2 = y1 + (y2 - y1) * coefficient[2];
        }
        if (std::abs (y2 - goal) < 1.526e-6 && std::abs (y0 - goal) < 1.526e-6)
            y0 = y1 = y2 = goal;
    }
}

namespace
{
class XyEffect final : public Effect
{
public:
    explicit XyEffect (EffectKind k) : kind (k) {}

    void prepare (double sr) noexcept override { rate = sr; }
    void reset() noexcept override { mover.started = false; }

    void process (float*, float*, int n, const EffectParams& params, double bpm) noexcept override
    {
        mover.step (params, kind, (double) n / rate, bpm);
        for (int a = 0; a < axesOf (kind); ++a)
            outputs[(std::size_t) a].store (output (kind, params, a, mover.position[(std::size_t) a]), std::memory_order_relaxed);
        for (int a = 0; a < axesOf (kind); ++a)
            positions[(std::size_t) a].store ((float) mover.position[(std::size_t) a], std::memory_order_relaxed);
    }

    // outputs 0…axes−1: the axes; outputs 4…6: the target's position (the editor draws it)
    float controlOutput (int k) const noexcept override
    {
        if (k >= 0 && k < 3)
            return outputs[(std::size_t) k].load (std::memory_order_relaxed);
        if (k >= 4 && k < 7)
            return positions[(std::size_t) (k - 4)].load (std::memory_order_relaxed);
        return k == 7 ? 1.0f : 0.0f; // 7: "running" (the editor's meters read 0 once the slot stops)
    }

private:
    EffectKind kind;
    double rate = 44100.0;
    Mover mover;
    std::array<std::atomic<float>, 3> outputs {}, positions {};
};

std::int32_t i32At (const std::vector<std::uint8_t>& d, std::size_t at) noexcept
{
    std::int32_t v = 0;
    if (at + 4 <= d.size())
        std::memcpy (&v, d.data() + at, 4);
    return v;
}
} // namespace

std::unique_ptr<Effect> makeEffect (EffectKind kind)
{
    return std::make_unique<XyEffect> (kind);
}

std::optional<EffectSlot> fromFl (const std::string& pluginName, const std::vector<std::uint8_t>& d)
{
    if (pluginName == "Fruity X-Y Controller")
    {
        if (d.size() < 44)
            return std::nullopt;
        EffectSlot slot;
        slot.kind = EffectKind::xyController;
        slot.params = defaults (slot.kind);
        auto raw = [&] (int i) { return i32At (d, 4 + 4 * (std::size_t) i); };
        for (int a = 0; a < 2; ++a)
        {
            slot.params[(std::size_t) level (a)] = std::clamp (raw (4 * a) / 65536.0f, 0.0f, 1.0f);
            slot.params[(std::size_t) minimum (a)] = std::clamp (raw (4 * a + 1) / 65536.0f, 0.0f, 1.0f);
            slot.params[(std::size_t) maximum (a)] = std::clamp (raw (4 * a + 2) / 65536.0f, 0.0f, 1.0f);
            slot.params[(std::size_t) tension (a)] = std::clamp (raw (4 * a + 3) / 256.0f + 0.5f, 0.0f, 1.0f);
        }
        // SPD 1024…2048, ACC 256…1024 (REA_rodada15 §11.1)
        slot.params[(std::size_t) speed (slot.kind)] = std::clamp ((raw (8) - 1024) / 1024.0f, 0.0f, 1.0f);
        slot.params[(std::size_t) acceleration (slot.kind)] = std::clamp ((raw (9) - 256) / 768.0f, 0.0f, 1.0f);
        slot.state = d; // FL's own bytes (grid name, joystick settings) kept
        return slot;
    }
    if (pluginName == "Fruity X-Y-Z Controller")
    {
        // version 3: 11 × i32 (0…2 levels 0…65536, 3…5 per-axis speed, 6…8 per-axis acceleration (1024 = 1), 9 the
        // smoothing time, 10 joystick), flags, combo, then per axis: joystick, flags and two 80-byte curves (input,
        // output: i32 3, i32 3, i32 points, 24 bytes per point: f64 x, f64 y, f32 tension, …) [REA_rodada164a §3]
        if (d.size() < 48)
            return std::nullopt;
        EffectSlot slot;
        slot.kind = EffectKind::xyzController;
        slot.params = defaults (slot.kind);
        auto raw = [&] (int i) { return i32At (d, 4 + 4 * (std::size_t) i); };
        for (int a = 0; a < 3; ++a)
            slot.params[(std::size_t) level (a)] = std::clamp (raw (a) / 65536.0f, 0.0f, 1.0f);
        slot.params[(std::size_t) speed (slot.kind)] = std::clamp (1.0f - raw (9) / 4096.0f, 0.0f, 1.0f);
        slot.params[(std::size_t) acceleration (slot.kind)] = std::clamp ((raw (6) / 1024.0f - 1.0f) / 4.0f, 0.0f, 1.0f);
        // each axis' output curve (2 points: f64 x, f64 y, f32 tension, u8 mode, 2 bytes, i8 sign): its ends become
        // Min / Max, the second point's mode and tension the axis' curve (FL's point law, core/FlCurve.h). The input
        // curves are identity in every FL file seen [DATA]: not mapped.
        for (int a = 0; a < 3; ++a)
        {
            const auto curve = (std::size_t) 56 + (std::size_t) a * (8 + 160) + 8 + 80;
            if (curve + 80 > d.size() || i32At (d, curve + 8) < 2)
                continue;
            const auto points = (std::size_t) std::min (i32At (d, curve + 8), 2);
            double y0 = 0.0, y1 = 1.0;
            float tension = 0.0f;
            const auto last = curve + 12 + 24 * (points - 1);
            std::memcpy (&y0, d.data() + curve + 12 + 8, 8);
            std::memcpy (&y1, d.data() + last + 8, 8);
            std::memcpy (&tension, d.data() + last + 16, 4);
            const auto mode = (int) d[last + 20];
            if (std::isfinite (y0) && std::isfinite (y1))
            {
                slot.params[(std::size_t) minimum (a)] = (float) std::clamp (y0, 0.0, 1.0);
                slot.params[(std::size_t) maximum (a)] = (float) std::clamp (y1, 0.0, 1.0);
            }
            if (std::isfinite (tension) && mode < flcurve::modeCount)
            {
                slot.params[(std::size_t) xy::tension (a)] = std::clamp (tension, -1.0f, 1.0f) * 0.5f + 0.5f;
                slot.params[(std::size_t) curveMode (a)] = curveModeValue (mode);
            }
        }
        slot.state = d;
        return slot;
    }
    return std::nullopt;
}

std::string paramText (EffectKind kind, int param, float v)
{
    char text[48];
    const auto axes = axesOf (kind);
    if (kind == EffectKind::xyzController && param >= curveMode (0) && param <= curveMode (2))
        return flcurve::modeName (curveModeOf (v));
    if (param >= 0 && param < axes * 4)
    {
        if (param % 4 == 3 && kind == EffectKind::xyzController) // FL's point tension −1…1 as ±100 %
            std::snprintf (text, sizeof (text), "%+d%%", (int) std::lround ((v - 0.5f) * 200.0f));
        else if (param % 4 == 3)
            std::snprintf (text, sizeof (text), "%+d", (int) std::lround ((v - 0.5f) * 256.0f));
        else
            std::snprintf (text, sizeof (text), "%.0f%%", v * 100.0f);
        return text;
    }
    std::snprintf (text, sizeof (text), "%.0f%%", v * 100.0f);
    return text;
}
} // namespace lynx::xy
