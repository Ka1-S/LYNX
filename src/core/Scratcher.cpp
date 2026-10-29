#include "core/Scratcher.h"

// Fruity Scratcher (docs/analise/REA_rodada165g-flscratch165.md). The [DECOMP addr] notes name the function of FL's
// Fruity Scratcher_x64.dll whose behaviour each part reproduces; the code is LYNX's own.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/Effects.h"
#include "core/Instruments.h"

namespace lynx::scratcher
{
namespace
{
constexpr std::array<Range, count> ranges { {
    { 0, 30000 }, // Wave position (FL's endless platter wheel)
    { 0, 1 },     // Mute
    { 0, 100 },   // Speed
    { 0, 100 },   // Acceleration
    { -128, 128 },// Pan
    { 0, 255 },   // Volume
    { 0, 1 },     // Hold
    { 0, 100 },   // Sensitivity
    { -1, 1 },    // Transport
    { 0, 1000 }, { 0, 1000 }, { 0, 1000 }, { 0, 1000 }, // Smooth 1…4
    { 0, 18 },    // Rotations
} };

// Delphi's Round (to nearest, ties to even) and Trunc
long delphiRound (double x) noexcept { return (long) std::nearbyint (x); }

// FL's index of a position in frames [DECOMP 0x72ab70]: Trunc (x − 0.5) + (Frac (x − 0.5) > 0.5)
inline int frameIndex (float x) noexcept
{
    const auto y = (double) (x - 0.5f);
    const auto i = (int) y; // CVTTSD2SI
    return y - (double) i > 0.5 ? i + 1 : i;
}

// FL flushes tiny values to 0 [DECOMP 0x72c740]
inline void flush (float& v) noexcept
{
    if (std::fabs ((double) v) < 1.0e-14)
        v = 0.0f;
}

std::optional<std::uint32_t> u32At (const std::vector<std::uint8_t>& s, std::size_t at)
{
    if (at + 4 > s.size())
        return std::nullopt;
    std::uint32_t v = 0;
    std::memcpy (&v, s.data() + at, 4);
    return v;
}
} // namespace

Range controlRange (int param) noexcept
{
    return param >= 0 && param < count ? ranges[(std::size_t) param] : Range {};
}

float valueOfRaw (int param, int raw) noexcept
{
    const auto r = controlRange (param);
    raw = std::clamp (raw, r.lo, r.hi);
    // FL's normalised value: (raw − min) · (1 / (max − min)), in double, stored as a float [DECOMP 0x729fe0]
    return (float) ((double) (raw - r.lo) * (1.0 / (double) (r.hi - r.lo)));
}

int rawOfValue (int param, float value) noexcept
{
    const auto r = controlRange (param);
    // FL's control integer of a value: min + Round (v · (max − min)) [DECOMP 0x72a630]
    return std::clamp (r.lo + (int) delphiRound ((double) value * (double) (r.hi - r.lo)), r.lo, r.hi);
}

float quantize (int param, float value) noexcept
{
    return valueOfRaw (param, rawOfValue (param, std::clamp (value, 0.0f, 1.0f)));
}

int transportOf (float value) noexcept
{
    return rawOfValue (transport, value);
}

float transportValue (int direction) noexcept
{
    return valueOfRaw (transport, direction);
}

int rotationsOf (float value) noexcept
{
    return rawOfValue (rotations, value);
}

std::array<float, count> defaults() noexcept
{
    std::array<float, count> d {};
    const int raw[count] = { 690, 0, 50, 25, 0, 199, 0, 50, 0, 500, 908, 999, 400, 9 };
    for (int i = 0; i < count; ++i)
        d[(std::size_t) i] = valueOfRaw (i, raw[i]);
    return d;
}

std::string paramText (int param, float value)
{
    char b[48] {};
    const auto raw = rawOfValue (param, value);
    switch (param)
    {
        case wavePos: std::snprintf (b, sizeof b, "%.1f%%", 100.0 * raw / 30000.0); break;
        case mute:
        case hold: return raw != 0 ? "On" : "Off";
        case speed: std::snprintf (b, sizeof b, "%.2fx", 2.0 * raw / 100.0); break;
        case accel:
        case sensitivity: std::snprintf (b, sizeof b, "%d%%", raw); break;
        case pan:
            if (raw == 0)
                return "Centered";
            std::snprintf (b, sizeof b, "%d%% %s", (int) std::lround (std::abs (raw) * 100.0 / 128.0), raw < 0 ? "left" : "right");
            break;
        case volume:
        {
            const auto g = raw / 255.0;
            if (g <= 0.0)
                return "-inf dB";
            std::snprintf (b, sizeof b, "%.1f dB", 20.0 * std::log10 (g));
            break;
        }
        case transport: return raw < 0 ? "Backwards" : raw > 0 ? "Forwards" : "Stopped";
        case smooth1:
        case smooth2:
        case smooth3:
        case smooth4: std::snprintf (b, sizeof b, "%.1f%%", raw / 10.0); break;
        case rotations: std::snprintf (b, sizeof b, "%+d", raw - 9); break;
        default: return {};
    }
    return b;
}

// ------------------------------------------------------------------------------------------------ FL's state

std::optional<State> parse (const std::vector<std::uint8_t>& s)
{
    const auto version = u32At (s, 0);
    // 0xF4628 (FL's render test, presets) … 0xF462A (what FL 2026 writes); the integer layout of older versions is
    // not in any file seen
    if (! version || *version < 0xF4628u || *version > 0xF462Au || s.size() < 4 + 14 * 4 + 4)
        return std::nullopt;

    State st;
    st.version = *version;
    float fl[count] {};
    std::memcpy (fl, s.data() + 4, sizeof fl);

    // the internal values as the plugin applies them after a load, through its controls [DECOMP 0x728d80, 0x72a630]:
    // the continuous ones are rounded to their control's step; Mute / Hold / Transport / Rotations are truncated
    // integers. (Mute's internal value 1 means sound; the load re-applies the controls twice, which keeps it.)
    for (int i = 0; i < count; ++i)
    {
        const auto v = fl[i];
        switch (i)
        {
            case mute: st.values[(std::size_t) i] = (int) v == 0 ? 1.0f : 0.0f; break;
            case hold: st.values[(std::size_t) i] = (int) v != 0 ? 1.0f : 0.0f; break;
            case transport: st.values[(std::size_t) i] = valueOfRaw (i, (int) v); break;
            case rotations: st.values[(std::size_t) i] = valueOfRaw (i, (int) v); break;
            default: st.values[(std::size_t) i] = valueOfRaw (i, rawOfValue (i, std::clamp (v, 0.0f, 1.0f))); break;
        }
    }

    std::size_t at = 4 + 14 * 4;
    const auto len = (std::int32_t) *u32At (s, at);
    at += 4;
    if (len > 0)
    {
        if (at + (std::size_t) len > s.size())
            return std::nullopt;
        st.samplePath.assign (reinterpret_cast<const char*> (s.data() + at), (std::size_t) len);
        at += (std::size_t) len;
    }
    if (*version > 0xF4628u)
        ++at; // the editor's size option
    if (*version >= 0xF462Au)
        if (const auto tc = u32At (s, at))
            st.timecode = (std::int32_t) *tc;
    return st;
}

std::vector<std::uint8_t> toFl (const std::array<float, count>& values, const std::string& samplePath)
{
    std::vector<std::uint8_t> out;
    auto put = [&out] (const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*> (p);
        out.insert (out.end(), b, b + n);
    };
    const std::uint32_t version = 0xF462Au;
    put (&version, 4);
    for (int i = 0; i < count; ++i)
    {
        const auto v = values[(std::size_t) i];
        float fl = 0.0f;
        switch (i)
        {
            case mute: fl = rawOfValue (i, v) != 0 ? 0.0f : 1.0f; break;
            case hold: fl = (float) rawOfValue (i, v); break;
            case transport:
            case rotations: fl = (float) rawOfValue (i, v); break;
            default: fl = quantize (i, v); break;
        }
        put (&fl, 4);
    }
    const auto len = (std::int32_t) samplePath.size();
    put (&len, 4);
    put (samplePath.data(), samplePath.size());
    const std::uint8_t editor = 0, riaa = 0;
    const std::int32_t timecode = -1;
    put (&editor, 1);
    put (&timecode, 4);
    put (&riaa, 1);
    return out;
}

// ------------------------------------------------------------------------------------------------ the sample

std::shared_ptr<const Turntable> prepareSample (const float* left, const float* right, std::size_t frames, double sampleRate)
{
    if (left == nullptr || frames < 2)
        return nullptr;
    if (right == nullptr)
        right = left;

    // the host's 16-bit stereo copy: values already on the 16-bit grid stay; others are rounded as FL's sample class
    // rounds a decoded file (round(x · 32767), REA_rodada77)
    bool onGrid = true;
    for (std::size_t i = 0; i < frames && onGrid; ++i)
    {
        const auto a = (double) left[i] * 32768.0, b = (double) right[i] * 32768.0;
        onGrid = a == std::floor (a) && b == std::floor (b) && std::fabs (a) <= 32768.0 && std::fabs (b) <= 32768.0;
    }
    auto to16 = [onGrid] (float x) -> float {
        if (onGrid)
            return x;
        const auto k = std::clamp (std::lround ((double) x * 32767.0), -32768L, 32767L);
        return (float) ((double) k / 32768.0);
    };

    auto t = std::make_shared<Turntable>();
    const bool resample = sampleRate > 0.0 && std::fabs (sampleRate - 44100.0) > 0.5;
    const auto n = resample ? (std::size_t) std::floor ((double) frames * 44100.0 / sampleRate) : frames;
    if (n < 2 || n > (std::size_t) 0x3FFFFFFF)
        return nullptr;
    t->n = (int) n;
    t->frames.resize ((n + 1) * 2);
    for (std::size_t i = 0; i < n; ++i)
    {
        float l = 0.0f, r = 0.0f;
        if (! resample)
            l = left[i], r = right[i];
        else
        {
            // [HYP] linear interpolation to 44.1 kHz (FL's host conversion was not traced)
            const auto pos = (double) i * sampleRate / 44100.0;
            const auto k = std::min ((std::size_t) pos, frames - 1);
            const auto k1 = std::min (k + 1, frames - 1);
            const auto f = (float) (pos - (double) k);
            l = left[k] + (left[k1] - left[k]) * f;
            r = right[k] + (right[k1] - right[k]) * f;
        }
        t->frames[2 * i] = to16 (l);
        t->frames[2 * i + 1] = to16 (r);
    }
    // the extra frame: FL copies frame 0's right channel into both sides [DECOMP 0x72c214]
    t->frames[2 * n] = t->frames[1];
    t->frames[2 * n + 1] = t->frames[1];
    return t;
}

std::shared_ptr<const Turntable> prepareSample (const SampleData& sample)
{
    return prepareSample (sample.leftData(), sample.isStereo() ? sample.rightData() : nullptr, sample.frames(), sample.sampleRate);
}

// ------------------------------------------------------------------------------------------------ the engine

Engine::Engine()
{
    reset();
}

void Engine::reset() noexcept
{
    // FL's reset [DECOMP 0x729f10] (plus the motion and the filters, which FL zeroes when it creates the plugin)
    wavePosIn = wavePosUsed = 0.0f;
    audible = true;
    holdOn = false;
    direction = 0;
    rotationCount = 9;
    smooth[0] = 0.5f;
    smooth[1] = 0.908f;
    smooth[2] = 0.999f;
    smooth[3] = 0.4f;
    volTarget = vol = volSet = 200.0f / 255.0f;
    panTarget = panNow = panSet = 0.0f;
    sen = 0.5f;
    acc = 0.25f;
    spd = 0.5f;
    phi = velocity = lastPos = 0.0f;
    dcL = dcLIn = dcR = dcRIn = 0.0f;
}

void Engine::setSample (std::shared_ptr<const Turntable> sample) noexcept
{
    pending = std::move (sample);
    swapPending = true;
}

void Engine::setParam (int param, float value) noexcept
{
    // [DECOMP 0x72a630] the control's step, then the field the render reads
    const auto v = quantize (param, value);
    switch (param)
    {
        case wavePos: wavePosIn = v; break;
        case mute: audible = rawOfValue (mute, value) == 0; break;
        case speed: spd = v; break;
        case accel: acc = v; break;
        case pan: panSet = v * 2.0f - 1.0f; break;
        case volume: volSet = v; break;
        case hold: holdOn = rawOfValue (hold, value) != 0; break;
        case sensitivity: sen = v; break;
        case transport: direction = transportOf (value); break;
        case smooth1:
        case smooth2:
        case smooth3:
        case smooth4: smooth[param - smooth1] = v; break;
        case rotations: rotationCount = rotationsOf (value); break;
        default: break;
    }
}

void Engine::process (float* left, float* right, int n) noexcept
{
    // [DECOMP 0x72add0]
    if (playing != nullptr && n > 0)
    {
        const float* buf = playing->frames.data();

        // the output stage, common to both modes: DC blocker per side [0x72c660 / 0x72c6d0], smoothed gain and pan,
        // linear balance, added to the input
        auto emit = [&] (int i, float sl, float sr) {
            dcL = (float) ((double) (sl - dcLIn) + (double) dcL * 0.99);
            dcLIn = sl;
            flush (dcL);
            dcR = (float) ((double) (sr - dcRIn) + (double) dcR * 0.99);
            dcRIn = sr;
            flush (dcR);
            vol = (float) ((double) vol * 0.99 + (double) volTarget * 0.01);
            flush (vol);
            const auto l = dcL * vol, r = dcR * vol;
            panNow = (float) ((double) panNow * 0.99 + (double) panTarget * 0.01);
            flush (panNow);
            if (panNow <= 0.0f)
            {
                left[i] = l + left[i];
                right[i] = r * (panNow + 1.0f) + right[i];
            }
            else
            {
                left[i] = l * (1.0f - panNow) + left[i];
                right[i] = r + right[i];
            }
        };
        auto read = [&] (float w, float& sl, float& sr) {
            const auto x = w * (float) (span + 1);
            const auto k = frameIndex (x);
            const auto f = x - (float) k;
            const float* a = buf + 2 * k;
            sl = a[0] + (a[2] - a[0]) * f;
            sr = a[1] + (a[3] - a[1]) * f;
        };

        if (! holdOn)
        {
            // motor: the platter's speed eases toward 2 · Speed / (n − 1) turns per sample in the transport's direction
            const auto target = motorScale * spd * (float) direction;
            const auto a = (float) (0.99999 - (double) acc * 0.0005);
            for (int i = 0; i < n; ++i)
            {
                velocity = a * (velocity - target) + target;
                flush (velocity);
                while (phi > 1.0f)
                    phi -= 1.0f;
                while (phi < 0.0f)
                    phi += 1.0f;
                float sl, sr;
                const auto w = phi;
                lastPos = phi;
                phi += velocity;
                read (w, sl, sr);
                emit (i, sl, sr);
                wavePosIn = phi; // the platter drives Wave position (FL shows it turning)
            }
        }
        else
        {
            // hold: the platter follows Wave position + whole turns through the 2nd-order smoother
            const auto n1 = frameIndex (smooth[0] * 2500.0f);
            const auto c2 = (float) ((double) smooth[1] * 0.0099 + 0.99);
            const auto c3 = (float) ((double) smooth[2] * 0.00999 + 0.99);
            const auto s4 = smooth[3];
            const auto gain = (1.0f - c2) / (float) (n1 + 250);
            const auto direct = 1.0f - c3, rest = 1.0f - s4;
            const auto turns = (float) (rotationCount - 9);
            for (int i = 0; i < n; ++i)
            {
                const auto e = (wavePosUsed + turns) - phi;
                velocity = c2 * velocity + gain * e;
                phi += s4 * velocity + e * direct * rest;
                auto w = phi;
                while (w > 1.0f)
                    w -= 1.0f;
                while (w < 0.0f)
                    w += 1.0f;
                float sl, sr;
                read (w, sl, sr);
                lastPos = w;
                emit (i, sl, sr);
            }
        }
    }

    // the end of FL's block: Wave position, Pan and Volume (or 0 when muted) become the next block's
    wavePosUsed = wavePosIn;
    panTarget = panSet;
    volTarget = audible ? volSet : 0.0f;
    if (swapPending)
    {
        swapPending = false;
        playing = std::move (pending);
        pending = nullptr;
        if (playing != nullptr)
        {
            span = playing->n - 1;
            invSpan = (float) (1.0 / (double) span);
            motorScale = invSpan * 2.0f;
        }
        volTarget = 0.0f;
    }
}

// ------------------------------------------------------------------------------------------------ the slot's effect
namespace
{
class ScratcherEffect final : public Effect
{
public:
    explicit ScratcherEffect (std::shared_ptr<const SampleData> sample)
    {
        if (sample != nullptr)
            engine.setSample (prepareSample (*sample));
    }

    void prepare (double) noexcept override {}
    void reset() noexcept override
    {
        engine.reset();
        first = true;
    }

    void process (float* left, float* right, int n, const EffectParams& params, double) noexcept override
    {
        // FL hands the plugin a parameter only when it changes (ProcessParam): the motor's own Wave position stays
        // until the platter is moved
        for (int i = 0; i < count; ++i)
            if (first || params[(std::size_t) i] != last[(std::size_t) i])
            {
                last[(std::size_t) i] = params[(std::size_t) i];
                engine.setParam (i, params[(std::size_t) i]);
            }
        first = false;
        engine.process (left, right, n);
        position.store (engine.position(), std::memory_order_relaxed);
    }

    // output 0: the platter's position (0…1), for the editor
    float controlOutput (int output) const noexcept override
    {
        return output == 0 ? position.load (std::memory_order_relaxed) : 0.0f;
    }

private:
    Engine engine;
    std::array<float, count> last {};
    bool first = true;
    std::atomic<float> position { 0.0f };
};
} // namespace

std::unique_ptr<Effect> make (std::shared_ptr<const SampleData> sample)
{
    return std::make_unique<ScratcherEffect> (std::move (sample));
}
} // namespace lynx::scratcher
