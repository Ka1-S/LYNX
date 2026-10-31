#include "core/FruityBasicFx.h"

// Fruity PanOMatic, Fruity Balance and Fruity Blood Overdrive, exact engines (docs/analise/REA_rodada58.md).
// Clean-room: written from the behaviour, formulas and data layouts described there; nothing of Image-Line's code or
// data is copied. Arithmetic follows the plugins operation by operation (float/double steps in their order, one
// rounding each, no contraction); the renderer's FTZ/DAZ matches the plugins' MXCSR.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

namespace lynx::flbasic
{
namespace
{
constexpr double kPi = 3.141592653589793;
constexpr int engineBlock = 512;

std::int32_t i32At (const std::vector<std::uint8_t>& d, std::size_t o) noexcept
{
    std::int32_t v = 0;
    std::memcpy (&v, d.data() + o, 4);
    return v;
}
std::uint32_t bitsOf (float f) noexcept
{
    std::uint32_t b = 0;
    std::memcpy (&b, &f, 4);
    return b;
}
float fromBits (std::uint32_t b) noexcept
{
    float f = 0;
    std::memcpy (&f, &b, 4);
    return f;
}
int rawFrom (float v, int lo, int hi) noexcept
{
    return lo + (int) std::lround (std::clamp ((double) v, 0.0, 1.0) * (hi - lo));
}
float norm (int raw, int lo, int hi) noexcept { return (float) std::clamp ((double) (raw - lo) / (hi - lo), 0.0, 1.0); }

// FL's level law L11 with a float ln 11 and a float 0.1 (volume 0…320, 256 = 1 exactly)
float volumeGain (int raw) noexcept
{
    const float f = (float) raw * 0.00390625f;
    if (f == 1.0f)
        return f;
    return (float) ((std::exp ((double) (f * 2.39789534f)) - 1.0) * (double) 0.1f);
}

// The host's gain-ramp step: 0.001 per sample at 44.1 kHz, scaled by 44100/fs (Balance uses 10×)
float rampStep (int rate) noexcept { return (float) (44100.0 / (double) rate * 0.001); }

// One gain's per-call ramp (§1.4): start, per-sample delta and the value reached at the end of the call
void ramp (float& cur, float target, int n, float maxStep, float& start, float& delta) noexcept
{
    start = cur;
    float d = target - start;
    if (bitsOf (d) == 0)
    {
        delta = 0.0f;
        return;
    }
    cur = target;
    d = d / (float) n;
    if (std::fabs (d) > maxStep)
    {
        d = fromBits ((bitsOf (d) & 0x80000000u) | bitsOf (maxStep));
        cur = start + d * (float) n;
        if ((bitsOf (cur) & 0x7fffffffu) < 0x33800000u) // below 2^-24: lands on 0
        {
            cur = 0.0f;
            d = -start / (float) n;
        }
    }
    delta = d;
}

// The stereo gain-matrix kernel: outL = L·g0 + R·g2, outR = R·g1 + L·g3, gains stepping by their deltas per frame
void matrix (float* left, float* right, int n, const float* g0, const float* d) noexcept
{
    float g[4] = { g0[0], g0[1], g0[2], g0[3] };
    if ((bitsOf (d[0]) | bitsOf (d[1]) | bitsOf (d[2]) | bitsOf (d[3])) != 0)
    {
        for (int i = 0; i < n; ++i)
        {
            const float l = left[i], r = right[i];
            left[i] = l * g[0] + r * g[2];
            right[i] = r * g[1] + l * g[3];
            for (int k = 0; k < 4; ++k)
                g[k] = g[k] + d[k];
        }
        return;
    }
    if ((bitsOf (g[2]) | bitsOf (g[3])) != 0)
    {
        for (int i = 0; i < n; ++i)
        {
            const float l = left[i], r = right[i];
            left[i] = l * g[0] + r * g[2];
            right[i] = r * g[1] + l * g[3];
        }
        return;
    }
    if (bitsOf (g[0]) == 0x3f800000u && bitsOf (g[1]) == 0x3f800000u)
        return; // in place with unit gains: untouched
    for (int i = 0; i < n; ++i)
    {
        left[i] = left[i] * g[0];
        right[i] = right[i] * g[1];
    }
}

// FL's host LFO tables (WaveTables[0…2] of the plugin host, 16384 points each)
struct HostTables
{
    std::array<std::array<float, 16384>, 3> t {};
    HostTables()
    {
        for (int i = 0; i < 16384; ++i)
            t[0][(std::size_t) i] = (float) std::sin ((double) (2 * i) * kPi / 16384.0);
        for (int i = 0; i < 0x2000; ++i)
            t[1][(std::size_t) ((i + 0x3000) & 0x3fff)] = (float) ((double) (i * 4) / 16384.0 + -1.0);
        for (int i = 0x2000; i < 0x4000; ++i)
            t[1][(std::size_t) ((i + 0x3000) & 0x3fff)] = (float) (1.0 - (double) ((i - 0x2000) * 4) / 16384.0);
        for (int i = 0; i < 16384; ++i)
            t[2][(std::size_t) i] = i < 0x2000 ? 1.0f : -1.0f;
    }
};
const HostTables& hostTables()
{
    static const HostTables tables;
    return tables;
}

// Blood Overdrive's shaper: 65536 points, a half sine from −1 to 1 (index 0x8000 + k = sin(kπ/65536))
struct OverdriveTable
{
    std::array<float, 65536> t {};
    OverdriveTable()
    {
        for (int k = 0; k < 0x8000; ++k)
        {
            const auto s = (float) std::sin ((double) k * kPi / 65536.0);
            t[(std::size_t) (0x8000 + k)] = s;
            t[(std::size_t) (0x7fff - k)] = fromBits (bitsOf (s) ^ 0x80000000u);
        }
    }
};
const OverdriveTable& overdriveTable()
{
    static const OverdriveTable table;
    return table;
}
} // namespace

// ================================================================================================== state parsing
std::optional<PanOMaticState> parsePanOMatic (const std::vector<std::uint8_t>& d)
{
    if (d.size() < 24)
        return std::nullopt;
    return PanOMaticState { i32At (d, 0), i32At (d, 4), i32At (d, 8), i32At (d, 12), i32At (d, 16), i32At (d, 20) };
}

std::optional<BalanceState> parseBalance (const std::vector<std::uint8_t>& d)
{
    if (d.size() < 8)
        return std::nullopt;
    return BalanceState { i32At (d, 0), i32At (d, 4) };
}

std::optional<SendState> parseSend (const std::vector<std::uint8_t>& d)
{
    // pan, volume, dry (the plugin's three controls) and the insert it sends to (REA_rodada105_send §1)
    if (d.size() < 16)
        return std::nullopt;
    return SendState { i32At (d, 0), i32At (d, 4), i32At (d, 8), i32At (d, 12) };
}

std::optional<BloodOverdriveState> parseBloodOverdrive (const std::vector<std::uint8_t>& d)
{
    // i32 version (the plugin reads versions < 4; 3 today), 6 × i32 controls, then (version > 2) an editor-only i32
    if (d.size() < 28 || i32At (d, 0) >= 4)
        return std::nullopt;
    BloodOverdriveState s;
    for (int i = 0; i < 6; ++i)
        s.raw[(std::size_t) i] = i32At (d, 4 + 4 * (std::size_t) i);
    return s;
}

namespace
{
constexpr int pomLo[panOMaticParams] = { 0, 0, 0, 0, 0, 0 };
constexpr int pomHi[panOMaticParams] = { 256, 320, 2, 2, 128, 55000 };
constexpr int odHi[bloodOverdriveParams] = { 10000, 10000, 10000, 1, 10000, 10000 };
} // namespace

void panOMaticToParams (const PanOMaticState& s, float* p) noexcept
{
    const int v[6] = { s.pan, s.vol, s.shape, s.target, s.amount, s.speed };
    for (int i = 0; i < 6; ++i)
        p[i] = norm (v[i], pomLo[i], pomHi[i]);
}
PanOMaticState panOMaticFromParams (const float* p) noexcept
{
    int v[6];
    for (int i = 0; i < 6; ++i)
        v[i] = rawFrom (p[i], pomLo[i], pomHi[i]);
    return { v[0], v[1], v[2], v[3], v[4], v[5] };
}
void balanceToParams (const BalanceState& s, float* p) noexcept
{
    p[0] = norm (s.pan, -128, 128);
    p[1] = norm (s.vol, 0, 320);
}
BalanceState balanceFromParams (const float* p) noexcept { return { rawFrom (p[0], -128, 128), rawFrom (p[1], 0, 320) }; }
float sendTrackValue (int track) noexcept { return norm (std::clamp (track, 0, maxMixerTracks - 1), 0, maxMixerTracks - 1); }
int sendTrack (float value) noexcept { return rawFrom (value, 0, maxMixerTracks - 1); }
float rescaleSendTrackValue (float value, int savedSpan) noexcept
{
    if (savedSpan <= 0 || savedSpan == maxMixerTracks - 1)
        return value;
    return sendTrackValue (rawFrom (value, 0, savedSpan));
}
void sendToParams (const SendState& s, float* p) noexcept
{
    p[0] = norm (s.pan, -128, 128);
    p[1] = norm (s.vol, 0, 320);
    p[2] = norm (s.dry, 0, 256);
    p[3] = sendTrackValue (s.track);
}
SendState sendFromParams (const float* p) noexcept
{
    return { rawFrom (p[0], -128, 128), rawFrom (p[1], 0, 320), rawFrom (p[2], 0, 256), sendTrack (p[3]) };
}
void bloodOverdriveToParams (const BloodOverdriveState& s, float* p) noexcept
{
    for (int i = 0; i < 6; ++i)
        p[i] = norm (s.raw[(std::size_t) i], 0, odHi[i]);
}
BloodOverdriveState bloodOverdriveFromParams (const float* p) noexcept
{
    BloodOverdriveState s;
    for (int i = 0; i < 6; ++i)
        s.raw[(std::size_t) i] = rawFrom (p[i], 0, odHi[i]);
    return s;
}

// =================================================================================================== param texts
namespace
{
std::string dbText (float g)
{
    if (g <= 0.0f)
        return "-inf dB";
    char t[32];
    std::snprintf (t, sizeof (t), "%+.1f dB", 20.0 * std::log10 ((double) g));
    return t;
}
std::string panText (int v, int centre, int half)
{
    char t[32];
    if (v == centre)
        return "Centre";
    std::snprintf (t, sizeof (t), "%d%% %s", (int) std::lround (std::abs (v - centre) * 100.0 / half), v < centre ? "L" : "R");
    return t;
}
// the LFO period at 44.1 kHz (samples → seconds), the law of §1.2
double lfoPeriodSeconds (int speed)
{
    const int s = 65536 - speed;
    const double x = (double) (float) ((double) s / 65536.0) * std::log (50001.0);
    const float per = ((float) (std::exp (x) - 1.0) * (float) s) * (float) (2048.0 / 44100.0);
    return (double) std::llrint ((double) per) / 44100.0;
}
} // namespace

std::string panOMaticParamText (int param, float v01)
{
    if (param < 0 || param >= panOMaticParams)
        return {};
    const int v = rawFrom (v01, pomLo[param], pomHi[param]);
    char t[48];
    switch (param)
    {
        case 0: return panText (v, 128, 128);
        case 1: return dbText (volumeGain (v));
        case 2: return v == 0 ? "Sine" : v == 1 ? "Triangle" : "Square";
        case 3: return v == 0 ? "Off" : v == 1 ? "Pan" : "Volume";
        case 4: std::snprintf (t, sizeof (t), "%d%%", (int) std::lround (v * 100.0 / 128.0)); return t;
        default: std::snprintf (t, sizeof (t), "%.2f s", lfoPeriodSeconds (v)); return t;
    }
}

std::string balanceParamText (int param, float v01)
{
    if (param == 0)
        return panText (rawFrom (v01, -128, 128), 0, 128);
    return dbText (volumeGain (rawFrom (v01, 0, 320)));
}

std::string sendParamText (int param, float v01)
{
    char t[32];
    switch (param)
    {
        case 0: return panText (rawFrom (v01, -128, 128), 0, 128);
        case 1: return dbText (volumeGain (rawFrom (v01, 0, 320)));
        case 2:
            std::snprintf (t, sizeof (t), "%.0f%%", rawFrom (v01, 0, 256) * 100.0 / 256.0);
            return t;
        case 3:
            if (sendTrack (v01) <= 0)
                return "None";
            std::snprintf (t, sizeof (t), "Insert %d", sendTrack (v01));
            return t;
        default: return {};
    }
}

std::string bloodOverdriveParamText (int param, float v01)
{
    char t[32];
    if (param == 3)
        return v01 >= 0.5f ? "On" : "Off";
    std::snprintf (t, sizeof (t), "%.0f%%", std::clamp ((double) v01, 0.0, 1.0) * 100.0);
    return t;
}

// ===================================================================================================== PanOMatic
void PanOMaticEngine::prepare (int sampleRate) noexcept
{
    rate = std::max (1, sampleRate);
    step = rampStep (rate);
    if (known)
    {
        auto s = raw;
        known = false;
        set (s); // the rate notification recomputes the LFO increment
    }
}

void PanOMaticEngine::set (const PanOMaticState& in) noexcept
{
    PanOMaticState s = in;
    s.pan = std::clamp (s.pan, 0, 256);
    s.vol = std::clamp (s.vol, 0, 320);
    s.shape = std::clamp (s.shape, 0, 2);
    s.target = std::clamp (s.target, 0, 2);
    s.amount = std::clamp (s.amount, 0, 128);
    s.speed = std::clamp (s.speed, 0, 55000);
    const bool all = ! known;
    if (all || s.pan != raw.pan)
        pan = (float) (s.pan - 128) * 0.0078125f;
    if (all || s.vol != raw.vol)
        vol = volumeGain (s.vol);
    if (all || s.shape != raw.shape)
        table = hostTables().t[(std::size_t) s.shape].data();
    if (all || s.target != raw.target || s.amount != raw.amount)
        amount = (float) s.amount * (s.target == 1 ? 0.0078125f : 0.00390625f);
    if (all || s.speed != raw.speed)
    {
        // LFO period in samples: (50001^(s/65536) − 1)·s·(2048/fs), s = 65536 − speed; the phase step is 2^32/period
        const int sp = 65536 - s.speed;
        const double x = (double) (float) ((double) sp / 65536.0) * std::log (50001.0);
        const float per = ((float) (std::exp (x) - 1.0) * (float) sp) * (float) (2048.0 / (double) rate);
        const auto samples = (std::uint32_t) std::max (2LL, std::llrint ((double) per));
        inc = (std::uint32_t) ((1ULL << 32) / samples);
    }
    if (all || s.pan != raw.pan || s.vol != raw.vol || s.shape != raw.shape || s.target != raw.target
        || s.amount != raw.amount || s.speed != raw.speed)
        dirty = true;
    raw = s;
    known = true;
}

void PanOMaticEngine::gains (float p, float v) noexcept
{
    if (p >= 0.0f)
    {
        const float a = 1.0f - p;
        target[3] = ((1.0f - a) * 0.5f) * v;
        target[0] = a * v;
        target[1] = v - target[3];
        target[2] = 0.0f;
    }
    else
    {
        const float b = 1.0f + p;
        target[2] = ((1.0f - b) * 0.5f) * v;
        target[1] = b * v;
        target[0] = v - target[2];
        target[3] = 0.0f;
    }
    dirty = false;
}

void PanOMaticEngine::flush() noexcept
{
    cur = target;
    phase = 0;
}

void PanOMaticEngine::prime() noexcept
{
    float l = 0.0f, r = 0.0f;
    process (&l, &r, 1);
    flush();
}

void PanOMaticEngine::process (float* left, float* right, int n) noexcept
{
    if (n <= 0)
        return;
    if (raw.target > 0 && table != nullptr)
    {
        phase += (std::uint32_t) ((std::uint64_t) inc * (std::uint64_t) n);
        const float l = table[phase >> 18] * amount;
        if (raw.target == 1)
        {
            float p = l + pan;
            if (! (-1.0f <= p))
                p = -1.0f;
            else if (p > 1.0f)
                p = 1.0f;
            gains (p, vol);
        }
        else
        {
            float v = l + vol;
            if (! (0.0f <= v))
                v = 0.0f;
            gains (pan, v);
        }
    }
    else if (dirty)
        gains (pan, vol);

    float start[4], delta[4];
    for (int k = 0; k < 4; ++k)
        ramp (cur[(std::size_t) k], target[(std::size_t) k], n, step, start[k], delta[k]);
    matrix (left, right, n, start, delta);
}

// ======================================================================================================= Balance
void BalanceEngine::prepare (int sampleRate) noexcept { step = rampStep (std::max (1, sampleRate)) * 10.0f; }

void BalanceEngine::set (const BalanceState& in) noexcept
{
    BalanceState s { std::clamp (in.pan, -128, 128), std::clamp (in.vol, 0, 320) };
    if (known && s.pan == raw.pan && s.vol == raw.vol)
        return;
    raw = s;
    known = true;
    const double a = (double) ((float) (s.pan + 128) * 0.00390625f);
    const double b = (double) ((float) s.pan * 0.0078125f);
    const double c190 = 1.0 / (std::cos (0.78539816339744828) * 10.0);
    const double k = (1.0 - std::sin (std::fabs (a - 0.5) * kPi) * 0.5) * c190;
    const double r = (double) s.vol / 320.0;
    const auto x = (float) (r * 1.25);
    const auto G = (float) (std::exp ((double) x * std::log (11.0)) - 1.0);
    const double g = (double) G * k;
    const double angle = (b + 1.0) * 0.78539816339744828;
    const double sn = std::sin (angle), cs = std::cos (angle);
    target = { g * cs, g * sn, 0.0, 0.0 };
    if (a >= 0.5)
        target[3] = target[1] - target[0];
    else
        target[2] = target[0] - target[1];
}

void BalanceEngine::flush() noexcept
{
    for (int k = 0; k < 4; ++k)
        cur[(std::size_t) k] = (float) target[(std::size_t) k];
}

void BalanceEngine::process (float* left, float* right, int n) noexcept
{
    if (n <= 0)
        return;
    float start[4], delta[4];
    for (int k = 0; k < 4; ++k)
        ramp (cur[(std::size_t) k], (float) target[(std::size_t) k], n, step, start[k], delta[k]);
    matrix (left, right, n, start, delta);
}

// ========================================================================================================== Send
// REA_rodada105_send §2: the pan and the volume give a 2×2 matrix (one side keeps its gain, the other fades and
// half of what it loses is folded into the kept side); the send is the input through that matrix, ramped like
// Balance (host step +0x34, 0.001 per sample at 44.1 kHz), added into the destination's buffer; then the signal
// continues × dry, without a ramp. Both levels use the L11 law.
void SendEngine::prepare (int sampleRate) noexcept { step = rampStep (std::max (1, sampleRate)); }

void SendEngine::set (const SendState& in) noexcept
{
    const SendState s { std::clamp (in.pan, -128, 128), std::clamp (in.vol, 0, 320), std::clamp (in.dry, 0, 256), in.track };
    if (known && s.pan == raw.pan && s.vol == raw.vol && s.dry == raw.dry && s.track == raw.track)
        return;
    if (! known || s.pan != raw.pan)
    {
        pan = (float) s.pan * 0.0078125f;
        dirty = true;
    }
    if (! known || s.vol != raw.vol)
    {
        volume = volumeGain (s.vol);
        dirty = true;
    }
    if (! known || s.dry != raw.dry)
        dry = volumeGain (s.dry);
    raw = s;
    known = true;
}

void SendEngine::flush() noexcept
{
    dirty = true;
    // the targets now (only computed while an insert is chosen, as in FL), the current gains at them
    process (nullptr, nullptr, nullptr, nullptr, 0);
    cur = target;
}

void SendEngine::process (const float* left, const float* right, float* sendLeft, float* sendRight, int n) noexcept
{
    if (raw.track <= 0)
        return;
    if (dirty)
    {
        if (pan < 0.0f)
        {
            const float kept = pan + 1.0f;
            target[2] = ((1.0f - kept) * 0.5f) * volume;
            target[1] = kept * volume;
            target[0] = volume - target[2];
            target[3] = 0.0f;
        }
        else
        {
            const float kept = 1.0f - pan;
            target[3] = ((1.0f - kept) * 0.5f) * volume;
            target[0] = kept * volume;
            target[1] = volume - target[3];
            target[2] = 0.0f;
        }
        dirty = false;
    }
    if (n <= 0 || left == nullptr || sendLeft == nullptr)
        return;
    float g[4], d[4];
    for (int k = 0; k < 4; ++k)
        ramp (cur[(std::size_t) k], target[(std::size_t) k], n, step, g[k], d[k]);
    if ((bitsOf (d[0]) | bitsOf (d[1]) | bitsOf (d[2]) | bitsOf (d[3])) != 0)
    {
        for (int i = 0; i < n; ++i)
        {
            const float l = left[i], r = right[i];
            sendLeft[i] = l * g[0] + r * g[2] + sendLeft[i];
            sendRight[i] = r * g[1] + l * g[3] + sendRight[i];
            for (int k = 0; k < 4; ++k)
                g[k] = g[k] + d[k];
        }
    }
    else if ((bitsOf (g[2]) | bitsOf (g[3])) != 0)
    {
        for (int i = 0; i < n; ++i)
        {
            const float l = left[i], r = right[i];
            sendLeft[i] = l * g[0] + r * g[2] + sendLeft[i];
            sendRight[i] = r * g[1] + l * g[3] + sendRight[i];
        }
    }
    else if ((bitsOf (g[0]) | bitsOf (g[1])) != 0)
    {
        for (int i = 0; i < n; ++i)
        {
            sendLeft[i] = left[i] * g[0] + sendLeft[i];
            sendRight[i] = right[i] * g[1] + sendRight[i];
        }
    }
}

// =============================================================================================== Blood Overdrive
namespace
{
const float kSmooth = fromBits (0x3f7fef9eu), kSmoothIn = fromBits (0x3983126fu), kClip = fromBits (0x3f7ffcb9u);

float shape (float x) noexcept
{
    const double v = (double) (x * 32767.0f);
    const long long t = std::fabs (v) < 9.2233720368547758e18 ? (long long) v : (long long) (1ULL << 63); // cvttsd2si
    const auto i = (std::int32_t) ((std::uint32_t) t + 0x8000u);
    return overdriveTable().t[(std::size_t) std::clamp (i, 0, 0xffff)];
}
} // namespace

void BloodOverdriveEngine::derive() noexcept
{
    const float pb = p[0], x100 = p[3], col = p[1], pre = p[2], filt = p[4], post = p[5];
    const float c148 = (pre * pre) * pre;
    c17c = ((col * col) * col) * kSmoothIn;
    const float sw = (double) x100 <= 0.5 ? 25.0f : 2500.0f;
    c14c = (sw * c148 + 1.0f) * kSmoothIn;
    c150 = (1.0f - pb) * kSmoothIn;
    c154 = (post * post) * kSmoothIn;
    c164 = filt * 0.7f + fromBits (0x3e4ccccdu);
    c168 = 1.0f - c164;
}

void BloodOverdriveEngine::set (const BloodOverdriveState& s) noexcept
{
    for (int i = 0; i < 6; ++i)
    {
        const double d = (double) s.raw[(std::size_t) i] * (i == 3 ? 1.0 : 1.0 / 10000.0);
        p[(std::size_t) i] = (float) std::clamp (d, 0.0, 1.0);
    }
    raw = s;
    derive();
}

void BloodOverdriveEngine::start (int sampleRate, const BloodOverdriveState& s) noexcept
{
    s158 = s15c = s160 = 1.0f;
    s178 = 0.0f;
    std::fill (std::begin (L), std::end (L), 0.0f);
    std::fill (std::begin (R), std::end (R), 0.0f);
    std::fill (std::begin (lp), std::end (lp), 0.0f);
    set (s);
    for (int i = 0; i < sampleRate; ++i)
        smooth();
}

void BloodOverdriveEngine::smooth() noexcept
{
    s158 = s158 * kSmooth + c14c;
    s15c = s15c * kSmooth + c150;
    s160 = s160 * kSmooth + c154;
    s178 = s178 * kSmooth + c17c;
}

float BloodOverdriveEngine::channel (float* st, float x) noexcept
{
    // st: low, band, high (state-variable filter, coefficient s178), then a, b, c (the shaper's three taps)
    st[0] = st[0] + s178 * st[1];
    st[2] = (x - st[0]) - st[1];
    st[1] = st[1] + s178 * st[2];
    st[3] = st[3] + st[4] * 0.25f;
    st[5] = ((((st[2] + st[0]) * s15c + st[1]) * s158) - st[3]) - st[4];
    st[4] = st[4] + st[5] * 0.25f;
    float y = ((shape (st[3]) + shape (st[4])) + shape (st[5])) * s160;
    if (y > kClip)
        y = kClip;
    if (y < -kClip)
        y = -kClip;
    return y;
}

void BloodOverdriveEngine::process (float* left, float* right, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        smooth();
        const float yl = channel (L, left[i]);
        lp[0] = yl * c168 + lp[0] * c164;
        lp[1] = lp[0] * c168 + lp[1] * c164;
        left[i] = lp[1];
        const float yr = channel (R, right[i]);
        lp[2] = yr * c168 + lp[2] * c164;
        lp[3] = lp[2] * c168 + lp[3] * c164;
        right[i] = lp[3];
    }
}

// ================================================================================================ mixer effects
namespace
{
class PanOMaticEffect final : public Effect
{
public:
    void prepare (double sampleRate) noexcept override
    {
        rate = (int) std::lround (sampleRate);
        ready = false;
    }
    void reset() noexcept override
    {
        if (ready)
            engine.flush();
    }
    void process (float* left, float* right, int n, const EffectParams& params, double) noexcept override
    {
        if (! ready)
        {
            engine = PanOMaticEngine();
            engine.prepare (rate);
            engine.set (panOMaticFromParams (params.data()));
            engine.prime();
            ready = true;
        }
        else
            engine.set (panOMaticFromParams (params.data()));
        for (int done = 0; done < n;)
        {
            const int m = std::min (engineBlock, n - done);
            engine.process (left + done, right + done, m);
            done += m;
        }
    }

private:
    PanOMaticEngine engine;
    int rate = 44100;
    bool ready = false;
};

class BalanceEffect final : public Effect
{
public:
    void prepare (double sampleRate) noexcept override
    {
        rate = (int) std::lround (sampleRate);
        ready = false;
    }
    void reset() noexcept override
    {
        if (ready)
            engine.flush();
    }
    void process (float* left, float* right, int n, const EffectParams& params, double) noexcept override
    {
        engine.set (balanceFromParams (params.data()));
        if (! ready)
        {
            engine.prepare (rate);
            engine.flush();
            ready = true;
        }
        for (int done = 0; done < n;)
        {
            const int m = std::min (engineBlock, n - done);
            engine.process (left + done, right + done, m);
            done += m;
        }
    }

private:
    BalanceEngine engine;
    int rate = 44100;
    bool ready = false;
};

class SendEffect final : public Effect
{
public:
    void prepare (double sampleRate) noexcept override
    {
        rate = (int) std::lround (sampleRate);
        ready = false;
    }
    void reset() noexcept override
    {
        if (ready)
            engine.flush();
    }
    void setSendOutput (float* left, float* right) noexcept override
    {
        outL = left;
        outR = right;
    }
    void process (float* left, float* right, int n, const EffectParams& params, double) noexcept override
    {
        engine.set (sendFromParams (params.data()));
        if (! ready)
        {
            engine.prepare (rate);
            engine.flush();
            ready = true;
        }
        for (int done = 0; done < n;)
        {
            const int m = std::min (engineBlock, n - done);
            engine.process (left + done, right + done, outL != nullptr ? outL + done : nullptr, outR != nullptr ? outR + done : nullptr, m);
            done += m;
        }
        outL = outR = nullptr; // given again before every block
        const float g = engine.dryGain();
        if (bitsOf (g) == 0x3f800000u)
            return;
        for (int i = 0; i < n; ++i)
        {
            left[i] = left[i] * g;
            right[i] = right[i] * g;
        }
    }

private:
    SendEngine engine;
    float* outL = nullptr;
    float* outR = nullptr;
    int rate = 44100;
    bool ready = false;
};

class BloodOverdriveEffect final : public Effect
{
public:
    void prepare (double sampleRate) noexcept override
    {
        rate = (int) std::lround (sampleRate);
        ready = false;
    }
    void reset() noexcept override {} // the plugin has no flush: its filters keep their state
    void process (float* left, float* right, int n, const EffectParams& params, double) noexcept override
    {
        const auto s = bloodOverdriveFromParams (params.data());
        if (! ready)
        {
            engine.start (rate, s);
            current = s.raw;
            ready = true;
        }
        else if (s.raw != current)
        {
            engine.set (s);
            current = s.raw;
        }
        engine.process (left, right, n);
    }

private:
    BloodOverdriveEngine engine;
    std::array<int, 6> current {};
    int rate = 44100;
    bool ready = false;
};
} // namespace

std::unique_ptr<Effect> makePanOMatic() { return std::make_unique<PanOMaticEffect>(); }
std::unique_ptr<Effect> makeBalance() { return std::make_unique<BalanceEffect>(); }
std::unique_ptr<Effect> makeBloodOverdrive() { return std::make_unique<BloodOverdriveEffect>(); }
std::unique_ptr<Effect> makeSend() { return std::make_unique<SendEffect>(); }
} // namespace lynx::flbasic
