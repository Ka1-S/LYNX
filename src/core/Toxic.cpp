#include "core/Toxic.h"
#include "core/HostSlide.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "core/Sha256.h"
#include "core/ToxicFx.h"

// 1:1 port of our round-45 reference model (r45/py/toxmodel.py + toxwave.py; effects in core/ToxicFx). The model
// emulates the DLL's float32 arithmetic with numpy float32 values and uses Python floats (doubles) where the DLL computes
// in double; this file keeps those types — `float` where the model holds float32, `double` + an explicit (float) where
// it rounds a double — and the operation order of every expression, so contraction into FMAs stays off.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")
#endif

namespace lynx
{
namespace
{
constexpr int paramCount = 340;
constexpr int chunk = 64;      // engine chunk: buffer size − 4 (REA_rodada45 §1.1, EXEC)
constexpr int voiceCount = 32; // the wrapper's max polyphony (§1.3)
constexpr int maxLanes = 48;   // 6 operators × 8 unison lanes
constexpr int blockLen = 128;  // voice control block (§1.4)
constexpr const char* dllSha256 = "5c326637be38fcea7148e170b83e6bec8600c87f00fa774eb610ed68f136cb46";
constexpr std::size_t stateSize = 10050, paramsAt = 8 + 4 + 0x40 + 8192; // "SSLF", size, tag, CC header, spectra

// cvttss2si / cvttsd2si: truncation toward zero; out of range or NaN → INT_MIN
int i32 (double x) noexcept
{
    if (! (x == x) || x >= 2147483648.0 || x < -2147483648.0)
        return std::numeric_limits<int>::min();
    return (int) x;
}

// the DLL's CRT powf (not correctly rounded; emulated in core/ToxicFx.cpp)
float powf32 (float x, float y) noexcept { return toxicPowf (x, y); }

// ------------------------------------------------------------------------------------------------------- parameter laws

int acb (float x, int lo, int hi) noexcept // FUN_18003acb0
{
    const auto v = i32 ((double) ((float) (hi - lo) * x + (float) lo));
    return std::min (std::max (v, lo), hi);
}

double tlaw (float x) noexcept // FUN_18003d480, seconds
{
    auto d = (double) x;
    d = d * 50.0 * d * d * d;
    return std::max (d, 0.00101);
}

double shiftLaw (float x) noexcept // FUN_18003d4b0, Hz
{
    const auto d = (double) ((x + x) - 1.0f);
    return d * d * d * d * 4000.0 * d;
}

float lfoAmountA (float x) noexcept { return (float) acb (x, -99, 100) * 0.00999999978f; } // operators 1 and 5
float lfoAmountB (float x) noexcept // operators 2, 3, 4 and 6 (an inlined variant with its own rounding)
{
    auto i = -99 - i32 ((double) (x * -199.0f));
    if ((double) i > 100.0)
        i = 100;
    if ((float) i < -99.0f)
        i = -99;
    return (float) i * 0.00999999978f;
}

float fmCell (float x) noexcept
{
    const auto v = x * 100.0f;
    const auto c = (float) ((double) i32 ((double) v) * 0.01);
    return c * c;
}

// constructor values of the 340 raw parameters (EXEC: peek of a fresh instance)
std::array<float, paramCount> defaults() noexcept
{
    std::array<float, paramCount> d {};
    const float head[31] = { 0.5f, 0, 0.5f, 0, 1, 1, 0, 0, 0.2f, 0, 0, 0.5f, 0, 0.4f, 0.4f, 0.5f, 0, 0, 0, 0, 1, 1, 1, 0.5f, 0, 1, 0, 0, 0, 0, 0 };
    std::copy (std::begin (head), std::end (head), d.begin());
    auto fill = [&] (int from, int count, float v) { std::fill (d.begin() + from, d.begin() + from + count, v); };
    fill (31, 18, 0.5f);  // coarse, fine, frequency shift
    fill (49, 6, 1.0f);   // free run
    fill (61, 6, 1.0f);   // velocity sensitivity
    fill (79, 6, 1.0f);   // decay
    fill (85, 6, 1.0f);   // sustain
    fill (91, 6, 1.0f);   // release
    fill (97, 6, 0.5f);   // pan
    d[103] = 1.0f;        // O1 mix
    fill (145, 12, 0.5f); // LFO amounts
    d[162] = 0.25f;       // sequencer steps
    fill (228, 8, 0.5f);  // EQ bands
    const float fx[58] = { 0, 0, 0.22f, 0.22f, 0.8f, 0.8f, 1, 1, 0.15f, 0.15f, 0.8f, 0.8f, 1, 1, 1, 1, 0.55f, 0.55f, 0.2f, 0.2f,
                           0.4f, 0.4f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.6f, 0.6f, 0.24f, 0.24f,
                           0.2f, 0.2f, 0.5f, 0.5f, 0.35f, 0.35f, 1, 1, 0.4f, 0.4f, 1, 1, 0.4f, 0.4f, 0.4f, 0.4f, 0.5f, 0.5f,
                           0.5f, 0.5f, 0.7f, 0.7f };
    std::copy (std::begin (fx), std::end (fx), d.begin() + 236);
    const float internal[12] = { 0, 0.175f, 0, 1, 0, 1, 0, 8, 0, 0, 0, 0 };
    std::copy (std::begin (internal), std::end (internal), d.begin() + 294);
    fill (306, 8, 1.0f); // waves
    d[314] = 1.0f;       // effect slot 1 type
    fill (332, 8, 0.5f); // modulation amounts
    return d;
}

// ------------------------------------------------------------------------------------------------------- state

struct Spectrum
{
    std::array<float, 128> amp {}, phase {}; // phase in cycles
};

struct Patch
{
    std::array<float, paramCount> P {};
    std::array<Spectrum, 8> drawn {}; // wave 0 of operators 1…6 and LFOs 1/2
};

std::optional<Patch> parseState (const std::vector<std::uint8_t>& s)
{
    if (s.size() < paramsAt + 4 * paramCount || std::memcmp (s.data(), "SSLF", 4) != 0)
        return std::nullopt;
    const auto* tag = s.data() + 8;
    if (std::memcmp (tag, "2iBT", 4) != 0 && std::memcmp (tag, "oiBT", 4) != 0 && std::memcmp (tag, "DiBT", 4) != 0)
        return std::nullopt;
    Patch p;
    std::memcpy (p.P.data(), s.data() + paramsAt, 4 * paramCount);
    for (int k = 0; k < 8; ++k)
    {
        const auto at = 12 + 0x40 + 1024 * (std::size_t) k;
        std::memcpy (p.drawn[(std::size_t) k].amp.data(), s.data() + at, 512);
        std::memcpy (p.drawn[(std::size_t) k].phase.data(), s.data() + at + 512, 512);
    }
    for (auto& v : p.P)
        if (! std::isfinite (v))
            v = 0.0f;
    return p;
}

int waveOf (const Patch& p, int slot) noexcept { return i32 ((double) p.P[(std::size_t) (306 + slot)]); }

// ------------------------------------------------------------------------------------------------ built-in spectra

#include "core/ToxicWaves.inc"

std::vector<std::uint8_t> readFile (std::string path)
{
    std::ifstream in (path, std::ios::binary);
    if (! in) // FL-style path on a system with '/' separators (the command-line tools on Linux)
    {
        std::replace (path.begin(), path.end(), '\\', '/');
        in.open (path, std::ios::binary);
    }
    if (! in)
        return {};
    return std::vector<std::uint8_t> ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
}

// waves 1…32 as stored in the decoded build of the DLL (§2.1); the sine is also built without it
struct BuiltinSpectra
{
    std::array<Spectrum, 33> waves {};
    bool fromDll = false;
};

std::shared_ptr<const BuiltinSpectra> builtinSpectra (const std::string& dllPath)
{
    static std::mutex m;
    static std::map<std::string, std::shared_ptr<const BuiltinSpectra>> cache;
    std::lock_guard<std::mutex> lock (m);
    if (auto it = cache.find (dllPath); it != cache.end())
        return it->second;
    auto out = std::make_shared<BuiltinSpectra>();
    out->waves[1].amp[0] = 1.0f;
    const auto b = dllPath.empty() ? std::vector<std::uint8_t> {} : readFile (dllPath);
    if (b.size() > 0x1000 && sha256Hex (b.data(), b.size()) == dllSha256)
    {
        out->fromDll = true;
        for (int w = 1; w <= 32; ++w)
            for (int c = 0; c < 64; ++c)
            {
                const auto at = (std::size_t) waveChunks[w - 1][c];
                float v[4] = {};
                if (at != 0 && at + 16 <= b.size())
                    std::memcpy (v, b.data() + at, 16);
                auto& dst = c < 32 ? out->waves[(std::size_t) w].amp : out->waves[(std::size_t) w].phase;
                std::copy (v, v + 4, dst.begin() + 4 * (c % 32));
            }
    }
    cache[dllPath] = out;
    return out;
}

// ------------------------------------------------------------------------------------------------- wave tables

// 128 levels × 256 segments × 4 cubic coefficients; level L holds harmonics 1…128 − L (FUN_18002fa50, §2.2)
struct WaveTable
{
    std::vector<float> c = std::vector<float> (128 * 1024);
    const float* level (int l) const noexcept { return c.data() + 1024 * (std::size_t) l; }
};

std::shared_ptr<const WaveTable> buildTable (const Spectrum& s)
{
    static std::mutex m;
    static std::map<std::vector<float>, std::weak_ptr<const WaveTable>> cache;
    std::vector<float> key (s.amp.begin(), s.amp.end());
    key.insert (key.end(), s.phase.begin(), s.phase.end());
    {
        std::lock_guard<std::mutex> lock (m);
        if (auto it = cache.find (key); it != cache.end())
            if (auto t = it->second.lock())
                return t;
    }
    auto t = std::make_shared<WaveTable>();
    std::vector<float> rows (128 * 260);
    float buf[260] {};
    for (int h = 0; h < 128; ++h)
    {
        const auto a = (double) s.amp[(std::size_t) h];
        const auto w = (double) (h + 1) * 0.024543692606171875;
        const auto ph = (double) s.phase[(std::size_t) h] * 6.28318530718;
        for (int n = 0; n < 256; ++n)
            buf[n] = (float) (std::sin ((double) n * w + ph) * a + (double) buf[n]);
        for (int n = 0; n < 4; ++n)
            buf[256 + n] = buf[n];
        std::copy (buf, buf + 260, rows.begin() + 260 * h);
    }
    for (int l = 0; l < 128; ++l)
    {
        const float* y = rows.data() + 260 * (127 - l);
        float* o = t->c.data() + 1024 * l;
        for (int i = 0; i < 256; ++i)
        {
            const auto ym1 = i == 0 ? y[255] : y[i - 1], y0 = y[i], y1 = y[i + 1], y2 = y[i + 2];
            const auto c1 = (y1 - ym1) * 0.5f;
            const auto c3 = (y2 - ym1) * 0.5f + (y0 - y1) * 1.5f;
            const auto c2 = ((ym1 - y0) + c1) - c3;
            o[4 * i] = y0, o[4 * i + 1] = c1, o[4 * i + 2] = c2, o[4 * i + 3] = c3;
        }
    }
    std::lock_guard<std::mutex> lock (m);
    cache[key] = t;
    return t;
}

// ------------------------------------------------------------------------------------------------ ladder table

// FUN_180038890 (REA_rodada26 §2.1): 65 536 entries of (k, p, R) for fc = 2^(9u + 5) Hz at 44.1 kHz
struct LadderTable
{
    std::vector<float> t = std::vector<float> (3 * 65536);
    LadderTable()
    {
        for (int i = 0; i < 65536; ++i)
        {
            const auto u = i / 65535.0;
            const auto f = 2.0 * std::pow (2.0, 9.0 * u + 5.0) / 44100.0;
            const auto k = 3.6 * f - 1.6 * f * f - 1.0;
            const auto p = (k + 1.0) * 0.5;
            auto sc = 0.95 * std::exp ((1.0 - p) * 1.386249);
            if (u > 0.7)
                sc += (u - 0.7) * (u - 0.7) * (0.26666666666666666 * 3.3333333333333333);
            const auto r = 1.96 * std::pow (1.0 - u, 1.5) + 0.98 * sc;
            t[3 * (std::size_t) i] = (float) k, t[3 * (std::size_t) i + 1] = (float) p, t[3 * (std::size_t) i + 2] = (float) r;
        }
    }
};

const LadderTable& ladderTable()
{
    static const LadderTable table;
    return table;
}

float saturate (float v) noexcept
{
    auto a = std::fabs (v) * 0.0666666701f;
    if (a >= 1.0f)
        a = 1.0f;
    return v * ((((4.0f - a * 2.0f) * a) - 3.0f) * a + 1.0f);
}

// --------------------------------------------------------------------------------------------- operator records

struct Op
{
    float coarse = 0, fine = 0;
    double shift = 0, shiftS = 0;
    float mix = 0, pan = 0.5f, phase = 0;
    bool free = false, enable = false;
    float vs = 0, init = 0, A = 0, D = 0, S = 0, R = 0;
    std::array<float, 6> fm {};
    float l1 = 0, l2 = 0;
};

// ------------------------------------------------------------------------------------------------ oscillator bank

struct Osc // FUN_180029da0 / FUN_180029ec0 / FUN_180029cc0 / FUN_180029fa0 (§3.2)
{
    float k = 1.0f, t = 0.0f, dt = 0.0f;
    std::array<std::uint32_t, maxLanes> ph {};
    std::array<std::int32_t, maxLanes> inc {}, dinc {};
    std::array<float, maxLanes> etgt {}, e {}, ecoef {}, L0 {}, L1 {}, L2 {}, L3 {}, gLt {}, gL {}, gRt {}, gR {}, M {}, out {};
    std::array<const float*, maxLanes> tab {};
    struct Route { int dst, src; float amount; };
    std::vector<Route> routes;

    Osc() { routes.reserve (6 * 6 * 8); }

    void initLane (int i, float init, float phase, bool random, std::uint32_t& rng) noexcept
    {
        if (init <= 1e-7f)
            init = 1e-7f;
        rng = rng * 0xbb38435u + 0x3619636bu;
        ph[(std::size_t) i] = random ? rng : (std::uint32_t) (std::int64_t) ((double) phase * 4294967295.0);
        inc[(std::size_t) i] = dinc[(std::size_t) i] = 0;
        etgt[(std::size_t) i] = e[(std::size_t) i] = init;
        ecoef[(std::size_t) i] = 0.0f;
        L0[(std::size_t) i] = L1[(std::size_t) i] = L2[(std::size_t) i] = L3[(std::size_t) i] = 0.0f;
        gLt[(std::size_t) i] = gL[(std::size_t) i] = gRt[(std::size_t) i] = gR[(std::size_t) i] = 0.0f;
        M[(std::size_t) i] = 20.0f;
        out[(std::size_t) i] = 0.0f;
    }

    void setLane (int i, float gl, float gr, float tg, float coef, float h1, float c1, float c2, float c3, double freq,
                  const float* table, float fs, int blk) noexcept
    {
        const auto e7 = 1e-7f;
        const auto I = (std::size_t) i;
        gl = gl > e7 ? gl : e7;
        tg = tg > e7 ? tg : e7;
        gr = gr > e7 ? gr : e7;
        etgt[I] = tg, ecoef[I] = coef;
        L0[I] = h1, L1[I] = c1, L2[I] = c2, L3[I] = c3;
        gLt[I] = gl;
        if (gL[I] < e7)
            gL[I] = gl;
        gRt[I] = gr;
        if (gR[I] < e7)
            gR[I] = gr;
        auto f = freq > 0.0 ? freq : 0.0;
        const auto lim = (double) fs * 0.49;
        if (f > lim)
            f = lim;
        tab[I] = table;
        const auto target = -i32 ((f / (double) fs) * -4294967295.0);
        const auto d = (std::int64_t) target - (std::int64_t) inc[I];
        dinc[I] = (std::int32_t) (d / blk);
    }

    void render (int n, int lanes, float* outL, float* outR) noexcept
    {
        const auto g = (lanes + 3) / 4 * 4;
        const auto kk = k * 0.001f;
        float y[maxLanes] {}, gl[maxLanes] {}, gr[maxLanes] {};
        for (int s = 0; s < n; ++s)
        {
            const auto tt = t;
            for (int i = 0; i < g; ++i)
            {
                const auto I = (std::size_t) i;
                const auto rd = (std::uint32_t) ((std::uint32_t) i32 ((double) (16777215.0f * M[I])) * 256u) + ph[I];
                ph[I] += (std::uint32_t) inc[I];
                inc[I] = (std::int32_t) ((std::uint32_t) inc[I] + (std::uint32_t) dinc[I]);
                const auto seg = rd >> 24;
                const auto fr = (float) (std::int32_t) (rd & 0xffffffu) * 5.96046448e-08f;
                const auto eo = e[I];
                auto en = (etgt[I] - eo) * ecoef[I] + eo;
                if (en > 1.0f)
                    en = 1.0f;
                e[I] = en;
                float w = 0.0f;
                if (const auto* c = tab[I])
                {
                    c += 4 * seg;
                    w = ((c[3] * fr + c[2]) * fr + c[1]) * fr + c[0];
                }
                const auto lev = ((tt * L3[I] + L2[I]) * tt + L1[I]) * tt + L0[I];
                y[i] = (w * eo) * lev;
                out[I] = y[i];
                gl[i] = gL[I], gr[i] = gR[I];
                gL[I] = (gLt[I] - gl[i]) * kk + gl[i];
                gR[I] = (gRt[I] - gr[i]) * kk + gr[i];
            }
            float accL = 0.0f, accR = 0.0f;
            for (int q = 0; q < g; q += 4)
            {
                const auto sl = ((gl[q + 1] * y[q + 1] + gl[q] * y[q]) + gl[q + 2] * y[q + 2]) + gl[q + 3] * y[q + 3];
                const auto sr = ((y[q + 1] * gr[q + 1] + y[q] * gr[q]) + y[q + 2] * gr[q + 2]) + y[q + 3] * gr[q + 3];
                if (q == 0)
                    accL = sl, accR = sr;
                else
                    accL = sl + accL, accR = sr + accR;
            }
            outL[s] = accL, outR[s] = accR;
            for (int i = 0; i < g; ++i)
                M[(std::size_t) i] = 20.0f;
            t = tt + dt;
            for (const auto& r : routes)
                M[(std::size_t) r.dst] = out[(std::size_t) r.src] * r.amount + M[(std::size_t) r.dst];
        }
    }
};

class Engine;

// --------------------------------------------------------------------------------------------------------- voice

struct Voice
{
    Engine* s = nullptr;
    int note = -1;
    float rdet = 0.0f, fs = 44100.0f, vel = 0.0f, glrate = 0.0f, glp = 48.0f;
    bool released = false;
    std::uint32_t counter = 0;
    Osc osc;
    float bufL[blockLen] {}, bufR[blockLen] {};

    float c0 = -20, c1 = -20, c2 = -20, beta = -20;
    std::array<float, 4> ct { 0, 0, 0, 1 };
    float amp = 0, ampt = 0, ampk = 0, fe = 0, fet = 0, fek = 0, EA = 0, res = 0;
    std::array<float, 16> st {};
    double U = -1.0, Uinc = 0.0, el = 0.0, rc = 0.0;
    std::array<std::array<float, 6>, 4> hist {};
    float velS = -20.0f;
    bool start = true;
    int blk = blockLen, fade = 0, nl = 0, nuni = 1;
    std::array<double, 2> lfo {}, lfov {};
    float aA = 0, aD = 0, aS = 0, aR = 0, fA = 0, fD = 0, fS = 0, fR = 0;
    std::array<bool, 6> oen {};
    std::array<float, 6> oinit {}, oA {}, oD {}, oS {}, oR {}, ovs {};

    void reset() noexcept;
    void begin() noexcept;
    void release() noexcept;
    float pitch() const noexcept;
    void glide (int n) noexcept;
    bool block (int n) noexcept;
    bool ampStage (double bt) noexcept;
    void opEnv (int i, float& tg, float& coef) const noexcept;
    void opsSetup (int n) noexcept;
    void filterSetup (int n) noexcept;
    float ladder (float x, int o, float k, float p, float R, float oneB, float b) noexcept;
    void render (float* outL, float* outR, int n) noexcept;
};

// --------------------------------------------------------------------------------------------------------- engine

class Engine // ToxicSynth (FUN_1800406d0 and its setters, §1)
{
public:
    double fs = 44100.0, tempo = 125.0;
    std::array<float, paramCount> raw {};
    std::array<Op, 6> ops {};
    std::array<std::shared_ptr<const WaveTable>, 8> tables {};
    std::shared_ptr<const BuiltinSpectra> builtins;
    Patch patch {};
    std::array<std::array<float, 2>, 128> LR {};
    std::array<float, 128> ptab {};
    float ccvel = 0.0f;
    std::uint32_t rng = 22222; // FUN_180029ec0's generator (process-global in FL: shared by every instance)

    // parameters
    float gain = 1, transpose = 0, uniDtn = 0, uniPan = 0, glideT = 0, velcurve = 1, pbrange = 2;
    double ampA = 0, ampD = 0, ampS = 0, ampR = 0, fA = 0, fD = 0, fS = 0, fR = 0, egf2p = 0, l2p = 0;
    std::array<double, 2> lfohz { 0.01, 0.01 };
    std::array<int, 2> lfosync {};
    std::array<bool, 2> lfofree {};
    enum { kCut, kRes, kKt, kVt, kEga, kL1a, kCount };
    std::array<double, kCount> tg {};
    std::array<float, kCount> sm {};
    int fmode = 0, poly = 32, unison = 1, glmode = 0, npolyEvent = 0;
    std::array<int, 8> cctype {}, cctgt {};
    std::array<float, 8> ccamt {};

    // run time
    std::array<double, 2> lfoph {}, lforate {};
    std::array<Voice, voiceCount> voices {};
    int lastnote = -1;
    std::uint32_t counter = 0;
    std::array<int, 128> vtab {};
    float gL = 0, gR = 0;
    ToxicFxRack fx;
    bool effects = true;

    Engine()
    {
        for (auto& v : voices)
            v.s = this;
    }

    // FUN_180030110
    float lfoRead (int j, double phase) const noexcept
    {
        const auto* tb = tables[(std::size_t) (6 + j)]->level (0);
        const auto x = (float) phase * 255.0f;
        const auto i = std::min (std::max (i32 ((double) x), 0), 255);
        const auto fr = x - (float) i;
        const auto* c = tb + 4 * i;
        return (((fr * c[3] + c[2]) * fr) + c[1]) * fr + c[0];
    }

    float ccSource (int t) const noexcept
    {
        if (t == 1)
            return ccvel;
        if (t == 2)
            return lfoRead (0, (double) (float) lfoph[0]);
        if (t == 3)
            return lfoRead (1, (double) (float) lfoph[1]);
        if (t >= 7 && t < 135) // MIDI CC table: LYNX sends no CCs; the constructor's CC7 = 1, CC10 = 0.5
            return t - 7 == 7 ? 1.0f : t - 7 == 10 ? 0.5f : 0.0f;
        return 0.0f; // 4 and 5 (aftertouch, pitch bend): 0
    }

    float modded (int idx, float x) const noexcept // FUN_1800413f0
    {
        if (idx < 0 || idx >= 294)
            return x;
        for (int k = 0; k < 8; ++k)
            if (cctype[(std::size_t) k] != 0 && cctgt[(std::size_t) k] == idx)
                x = ccSource (cctype[(std::size_t) k]) * ccamt[(std::size_t) k] + x;
        if (x <= 0.0f)
            x = 0.0f;
        if (1.0f <= x)
            x = 1.0f;
        return x;
    }

    void setWave (int k, int w)
    {
        if (w == 0)
            tables[(std::size_t) k] = buildTable (patch.drawn[(std::size_t) k]);
        else if (w >= 1 && w <= 32)
            tables[(std::size_t) k] = buildTable (builtins->waves[(std::size_t) w]);
    }

    void setOp (int idx, float x) noexcept
    {
        const auto r = (idx - 25) % 6, base = idx - r;
        auto& op = ops[(std::size_t) r];
        switch (base)
        {
            case 25: op.enable = x > 0.5f; return;
            case 31: op.coarse = (float) acb (x, -99, 99); return;
            case 37: op.fine = (float) ((double) acb (x, -99, 99) * 0.01); return;
            case 43: op.shift = shiftLaw (x); return;
            case 49: op.free = x != 0.0f; return;
            case 55: op.phase = x; return;
            case 61: op.vs = x; return;
            case 67: op.init = x * x; return;
            case 73: op.A = (float) tlaw (x); return;
            case 79: op.D = (float) tlaw (x); return;
            case 85: op.S = x * x; return;
            case 91: op.R = (float) tlaw (x); return;
            case 97: op.pan = x; return;
            case 103: op.mix = x * x; return;
            default: break;
        }
        if (idx >= 109 && idx < 145)
            op.fm[(std::size_t) ((idx - 109) / 6)] = fmCell (x);
        else if (idx >= 145 && idx < 151)
            op.l1 = (r == 0 || r == 4) ? lfoAmountA (x) : lfoAmountB (x);
        else if (idx >= 151 && idx < 157)
            op.l2 = (r == 0 || r == 4) ? lfoAmountA (x) : lfoAmountB (x);
    }

    void setParam (int idx, float x) // FUN_18003dcc0
    {
        x = modded (idx, x);
        switch (idx)
        {
            case 0: gain = powf32 (10.0f, x * 3.0f - 1.5f); return;
            case 2: transpose = (float) acb (x, -24, 24); return;
            case 3: ampA = tlaw (x); return;
            case 4: ampD = tlaw (x); return;
            case 5: ampS = (double) (x * x); return;
            case 6: ampR = tlaw (x) * 0.34999999999999998 + 0.02; return;
            case 7: velcurve = std::max ((x + x) + 1.0f, 0.0f); return;
            case 8: uniDtn = x; return;
            case 9: uniPan = std::min (std::max (x, 0.0f), 1.0f); return;
            case 10:
            {
                const auto v = (x * x) * x;
                glideT = (double) v >= 0.0001 ? v : 9.99999975e-05f;
                return;
            }
            case 11: egf2p = (double) acb (x, -24, 24) * 0.083333333333333329; return;
            case 12: l2p = (double) (x * x) * 0.083333333333333329; return;
            case 13:
            case 14: lfohz[(std::size_t) (idx - 13)] = (double) ((x * x) * x) * 20.0 + 0.01; return;
            case 15: tg[kCut] = (double) x; return;
            case 16: tg[kRes] = (double) x; return;
            case 17: tg[kKt] = (double) x; return;
            case 18: tg[kVt] = (double) x; return;
            case 19: fA = tlaw (x); return;
            case 20: fD = tlaw (x); return;
            case 21: fS = (double) (x * x); return;
            case 22: fR = tlaw (x); return;
            case 23:
            {
                const auto v = (x + x) - 1.0f, w = v * v;
                tg[kEga] = (double) (v < 0.0f ? -w : w);
                return;
            }
            case 24:
            {
                const auto v = x * (float) std::sqrt ((double) x), w = v * v;
                tg[kL1a] = (double) (v < 0.0f ? -w : w);
                return;
            }
            case 295: pbrange = (float) acb (x, 0, 12); return;
            case 296: lfosync[0] = i32 ((double) x); return;
            case 297: lfofree[0] = x != 0.0f; return;
            case 298: lfosync[1] = i32 ((double) x); return;
            case 299: lfofree[1] = x != 0.0f; return;
            case 300: fmode = std::min (i32 ((double) x), 3); return;
            case 301: poly = i32 ((double) x); return;
            case 302: unison = std::min (std::max (i32 ((double) x) + 1, 1), 8); return;
            case 303: glmode = i32 ((double) x) & 0xff; return;
            default: break;
        }
        if (idx >= 25 && idx < 157)
            setOp (idx, x);
        else if (idx >= 306 && idx <= 313)
            setWave (idx - 306, i32 ((double) x));
        else if (idx >= 316 && idx <= 323)
            cctype[(std::size_t) (idx - 316)] = i32 ((double) x);
        else if (idx >= 324 && idx <= 331)
            cctgt[(std::size_t) (idx - 324)] = i32 ((double) x) - 1;
        else if (idx >= 332 && idx <= 339)
            ccamt[(std::size_t) (idx - 332)] = (float) ((double) acb (x, -99, 100) * 0.01);
        else if (idx == 1 || (idx >= 227 && idx < 294) || idx == 304 || idx == 305 || idx == 314 || idx == 315)
            fxParam (idx, x);
    }

    void fxParam (int idx, float x)
    {
        if (! effects && (idx == 1 || idx == 227 || idx == 236 || idx == 237))
            x = 0.0f; // voices only: distortion, EQ and both slots off (the DC blocker stays)
        fx.setParam (idx, x);
    }

    void load (const std::array<float, paramCount>& P)
    {
        raw = P;
        tg = {};
        lfohz = { 0.01, 0.01 };
        lfosync = {};
        lfofree = {};
        cctype = {};
        cctgt.fill (-1);
        ccamt = {};
        pbrange = 2;
        fmode = 0, poly = 32, unison = 1, glmode = 0;
        const auto sine = buildTable (builtins->waves[1]);
        tables.fill (sine);
        for (int i = 316; i < 340; ++i)
            setParam (i, P[(std::size_t) i]);
        for (int i = 0; i < 316; ++i)
            setParam (i, P[(std::size_t) i]);
    }

    void reapplyCc()
    {
        for (int k = 0; k < 8; ++k)
            if (cctype[(std::size_t) k] != 0 && cctgt[(std::size_t) k] >= 0)
                setParam (cctgt[(std::size_t) k], raw[(std::size_t) cctgt[(std::size_t) k]]);
    }

    float outGain() const noexcept { return (gain * 1.0f) * 0.5f; } // FUN_180025c90 (CC7 = 1, centred pan)

    // FUN_180040ff0 + FUN_180024e80: smoothers snapped, voices reset, output gains snapped, last note forgotten
    void rateSet (double rate)
    {
        fs = rate;
        for (int k = 0; k < kCount; ++k)
            sm[(std::size_t) k] = (float) tg[(std::size_t) k];
        for (auto& op : ops)
            op.shiftS = op.shift;
        lfoph = {};
        for (auto& v : voices)
        {
            v.fs = (float) fs;
            v.reset();
            v.glp = 48.0f;
        }
        gL = gR = outGain();
        lastnote = -1;
        fx.setSampleRate (fs);
    }

    // instance as FL creates it: constructor values, the rate, then the saved state (§1.2)
    void init (double rate, const Patch& p, std::shared_ptr<const BuiltinSpectra> spectra, bool withEffects)
    {
        builtins = std::move (spectra);
        patch = p;
        effects = withEffects;
        fx.reset();
        rng = 22222;
        counter = 0;
        vtab = {};
        for (auto& v : voices)
            v.counter = 0, v.released = false;
        load (defaults());
        rateSet (rate);
        load (p.P);
    }

    // FUN_18002f9f0
    const float* mip (int k, double f, double rate) const noexcept
    {
        int lvl = 0;
        if (rate * 0.00390625 < f)
            lvl = std::min (std::max (128 - i32 ((0.5 / f) * rate), 0), 127);
        return tables[(std::size_t) k]->level (lvl);
    }

    static float hzPitch (float p) noexcept { return (float) (std::pow (2.0, (double) ((p - 57.0f) * 0.0833333358f)) * 220.0); }

    int maxVoices() const noexcept // FUN_180024bd0 with the polyphony event
    {
        const auto np = npolyEvent != 0 ? npolyEvent : 32;
        auto p = poly >= 1 ? poly : 1;
        if (p < 1 || p > 32)
            p = 32;
        return std::min (std::min (p, np), voiceCount);
    }

    void noteOn (int key, int vel7) // FUN_180007a20 → FUN_1800250b0 / FUN_1800256a0
    {
        const auto vel = (float) vel7 * 0.00787401572f;
        vtab[(std::size_t) key] = i32 ((double) (vel * 127.0f));
        if (vel == 0.0f)
        {
            noteOff (key);
            return;
        }
        auto start = (float) key;
        if (0.0f <= (float) lastnote)
            start = (float) lastnote;
        if (poly == 0)
        {
            auto& head = voices[0];
            if ((glmode == 1 || glmode == 3) && (head.note < 0 || head.released))
                start = (float) key;
            auto& v = head;
            const auto old = v.note;
            const auto wasReleased = v.released;
            v.counter = counter++;
            v.note = key;
            v.rdet = 0.0f;
            v.released = false;
            v.glrate = (glmode == 0 || glmode == 1) ? (float) (12.0 / (fs * (double) glideT))
                                                    : (float) ((double) std::fabs ((float) v.note - v.glp) / (fs * (double) glideT));
            if (old < 0 || wasReleased)
            {
                v.vel = powf32 (vel, velcurve);
                v.glp = start;
                if ((double) glideT < 0.001)
                    v.glp = (float) v.note;
                v.begin();
            }
            lastnote = key;
            return;
        }
        if (glmode == 1 || glmode == 3)
        {
            start = (float) key;
            int best = 128;
            for (int nt = 0; nt < 128; ++nt)
                if (vtab[(std::size_t) nt] != 0 && nt != key && std::abs (key - nt) < best)
                    best = std::abs (key - nt), start = (float) nt;
        }
        const auto lim = maxVoices();
        Voice* oldest = &voices[0];
        Voice* oldRel = &voices[0];
        Voice* chosen = nullptr;
        for (int i = 0; i < lim; ++i)
        {
            auto& v = voices[(std::size_t) i];
            if (v.note < 0)
            {
                chosen = &v;
                break;
            }
            if (! v.released)
            {
                if (v.counter < oldest->counter)
                    oldest = &v;
            }
            else if (v.counter < oldRel->counter)
                oldRel = &v;
        }
        if (chosen == nullptr)
            chosen = oldRel->released ? oldRel : oldest;
        auto& v = *chosen;
        v.counter = counter++;
        v.note = key;
        v.rdet = 0.0f;
        v.vel = powf32 (vel, velcurve);
        v.glp = start;
        v.released = false;
        if ((double) glideT < 0.001)
            v.glp = (float) v.note;
        v.glrate = (glmode == 0 || glmode == 1) ? (float) (12.0 / (fs * (double) glideT))
                                                : (float) ((double) std::fabs ((float) v.note - v.glp) / (fs * (double) glideT));
        v.begin();
        lastnote = key;
    }

    void noteOff (int key)
    {
        vtab[(std::size_t) key] = 0;
        if (poly == 0) // the voice falls back to the nearest held key
        {
            int best = 128, found = -1;
            for (int nt = 0; nt < 128; ++nt)
                if (vtab[(std::size_t) nt] != 0 && std::abs (key - nt) < best)
                    best = std::abs (key - nt), found = nt;
            for (auto& v : voices)
                if (v.note == key)
                {
                    if (found >= 0)
                    {
                        v.counter = counter++;
                        v.note = found;
                        v.glrate = (float) (12.0 / (fs * (double) glideT));
                        lastnote = found;
                    }
                    else
                        v.released = true, v.release();
                }
            return;
        }
        for (auto& v : voices)
            if (v.note == key)
                v.released = true, v.release();
    }

    // a free-running LFO keeps moving while no voice plays (§1.3); when it modulates something the engine must keep
    // running between notes so the next note starts at FL's phase
    bool lfoRunning() const noexcept
    {
        const auto used = std::any_of (ops.begin(), ops.end(), [] (const Op& o) { return o.l1 != 0.0f || o.l2 != 0.0f; })
                          || tg[kL1a] > 1.0000000116860974e-07 || l2p != 0.0
                          || std::any_of (cctype.begin(), cctype.end(), [] (int t) { return t == 2 || t == 3; });
        return used && (lfofree[0] || lfofree[1]);
    }

    bool anyActive() const noexcept
    {
        return std::any_of (voices.begin(), voices.end(), [] (const Voice& v) { return v.note >= 0; });
    }

    void process (int n, float* L, float* R) // FUN_1800406d0
    {
        const auto c = (double) (float) (4410.0 / fs);
        for (int k = 0; k < kCount; ++k)
        {
            auto& t = tg[(std::size_t) k];
            if (k != kEga && t < 1.0000000116860974e-07)
                t = 1.0000000116860974e-07;
            auto v = (float) ((t - (double) sm[(std::size_t) k]) * c + (double) sm[(std::size_t) k]);
            if (-1e-9f < v && v < 1e-9f)
                v = 0.0f;
            sm[(std::size_t) k] = v;
        }
        for (auto& op : ops)
            op.shiftS = (op.shift - op.shiftS) * c + op.shiftS;
        const auto active = anyActive();
        static constexpr double mult[10] = { 0.0, 0.066666666666666666, 0.033333333333333333, 0.022222222222222223,
                                             0.016666666666666666, 0.0083333333333333332, 0.0055555555555555558,
                                             0.0041666666666666666, 0.0020833333333333333, 0.0010416666666666667 };
        for (int j = 0; j < 2; ++j)
        {
            if (! active && ! lfofree[(std::size_t) j])
                lfoph[(std::size_t) j] = 0.0;
            auto hz = lfohz[(std::size_t) j];
            const auto m = lfosync[(std::size_t) j];
            if (m >= 1 && m <= 9)
                hz = tempo * mult[m];
            lforate[(std::size_t) j] = hz / fs;
            auto ph = (double) n * lforate[(std::size_t) j] + lfoph[(std::size_t) j];
            if (ph > 1.0)
                ph -= 1.0;
            lfoph[(std::size_t) j] = ph;
        }
        for (int k = 0; k < 8; ++k)
            if ((cctype[(std::size_t) k] == 2 || cctype[(std::size_t) k] == 3) && cctgt[(std::size_t) k] >= 0)
                setParam (cctgt[(std::size_t) k], raw[(std::size_t) cctgt[(std::size_t) k]]);
        std::fill (L, L + n, 0.0f);
        std::fill (R, R + n, 0.0f);
        for (auto& v : voices)
            if (v.note >= 0)
                v.render (L, R, n);
        fx.setTempo (tempo);
        fx.process (L, R, n);
        const auto g = outGain();
        for (int i = 0; i < n; ++i)
        {
            L[i] = L[i] * gL, R[i] = R[i] * gR;
            gL = (float) ((double) (g - gL) * 0.0050000000000000001 + (double) gL);
            gR = (float) ((double) (g - gR) * 0.0050000000000000001 + (double) gR);
        }
    }
};

// ------------------------------------------------------------------------------------------------ voice methods

void Voice::reset() noexcept // FUN_18003a730
{
    note = -1;
    c0 = c1 = c2 = beta = -20.0f;
    ct = { 0, 0, 0, 1 };
    amp = ampt = ampk = fe = fet = fek = EA = 0.0f;
    st = {};
    Uinc = 0.0;
    U = -1.0;
    for (auto& h : hist)
        h = {};
    el = rc = 0.0;
    velS = -20.0f;
    start = true;
    blk = blockLen;
    fade = 0;
    lfo = {};
    lfov = {};
    res = 0.0f;
    nl = 0;
}

void Voice::begin() noexcept // FUN_18003a3c0
{
    s->ccvel = vel;
    s->reapplyCc();
    fade = 0;
    if (el != 0.0)
        fade = i32 ((double) (fs * 9.07029462e-05f)) & 0xff;
    rc = -1.0;
    el = 0.0;
    const auto lim = 0.001f;
    auto floor1 = [lim] (float v) { return v >= lim ? v : 0.001f; };
    aA = floor1 ((float) s->ampA);
    aD = floor1 ((float) s->ampD);
    aS = (float) s->ampS;
    fA = floor1 ((float) s->fA);
    fD = floor1 ((float) s->fD);
    fS = (float) s->fS;
    nuni = s->unison;
    for (std::size_t i = 0; i < 6; ++i)
    {
        const auto& op = s->ops[i];
        oen[i] = op.enable;
        oinit[i] = op.init;
        oA[i] = floor1 (op.A);
        oD[i] = floor1 (op.D);
        oS[i] = op.S;
        ovs[i] = op.vs;
    }
    lfo = s->lfoph;
}

void Voice::release() noexcept // FUN_18003a660
{
    aR = (double) (float) s->ampR >= 0.01 ? (float) s->ampR : 0.01f;
    fR = (float) s->fR >= 0.001f ? (float) s->fR : 0.001f;
    for (std::size_t i = 0; i < 6; ++i)
        oR[i] = s->ops[i].R >= 0.001f ? s->ops[i].R : 0.001f;
    rc = 0.0;
}

float Voice::pitch() const noexcept // FUN_180026a80
{
    auto p = s->transpose + glp;
    p = p + 0.0f;
    p = p + s->ptab[(std::size_t) note];
    p = p + 0.0f;
    p = p + rdet;
    p = p + s->pbrange * 0.0f;
    return p + 0.0f;
}

void Voice::glide (int n) noexcept // FUN_180026980
{
    const auto f2 = (float) note;
    auto f1 = glp;
    if (f1 < f2)
    {
        f1 = (float) n * glrate + f1;
        glp = f1;
        if (f2 < f1)
        {
            glp = f2;
            return;
        }
    }
    if (f1 <= f2)
        return;
    f1 = f1 - (float) n * glrate;
    glp = f1;
    if (f2 <= f1)
        return;
    glp = f2;
}

bool Voice::block (int n) noexcept // FUN_180039880
{
    glide (n);
    osc.k = 44100.0f / fs, osc.t = 0.0f, osc.dt = 1.0f / (float) n;
    for (int j = 0; j < 2; ++j)
    {
        auto ph = (double) n * s->lforate[(std::size_t) j] + lfo[(std::size_t) j];
        if (ph >= 1.0)
            ph -= 1.0;
        lfo[(std::size_t) j] = ph;
        lfov[(std::size_t) j] = (double) s->lfoRead (j, ph);
    }
    if (start)
    {
        int lane = 0;
        for (std::size_t i = 0; i < 6; ++i)
            if (oen[i])
                for (int u = 0; u < nuni; ++u)
                    osc.initLane (lane++, oinit[i], s->ops[i].phase, s->ops[i].free, s->rng);
    }
    auto vt = vel;
    if (velS >= 0.0f)
        vt = (float) ((double) (vt - velS) * 0.05 + (double) velS);
    velS = vt;
    if (std::fabs (vt) < 1e-9f)
        velS = 0.0f;
    const auto bt = (double) n * 2.2675736961451248e-05;
    if (! ampStage (bt))
        return false;
    opsSetup (n);
    filterSetup (n);
    if (fade == 0)
        el += bt;
    if (nl == 0)
    {
        reset();
        return false;
    }
    start = false;
    if (fade != 0)
        --fade;
    return true;
}

bool Voice::ampStage (double bt) noexcept // FUN_1800393b0
{
    const auto f = (double) fs;
    if (fade != 0)
    {
        ampt = 0.0f, ampk = (float) (147.00000000000003 / f);
        return true;
    }
    if (rc > -1.0)
    {
        if ((double) aR < rc)
        {
            reset();
            return false;
        }
        rc += bt;
        ampk = (float) (8.82 / (double) (aR * fs));
        ampt = 1e-7f;
        return true;
    }
    if (el < (double) aA)
    {
        ampt = 5.0f, ampk = (float) (8.82 / ((double) aA * 39.441975 * f));
        return true;
    }
    ampt = aS;
    ampk = (float) (8.82 / (double) (fs * aD));
    if (aS < 1e-7f)
        ampt = 1e-7f;
    return true;
}

void Voice::opEnv (int i, float& tgt, float& coef) const noexcept // FUN_1800394d0
{
    const auto f = (double) fs;
    const auto I = (std::size_t) i;
    if (fade != 0)
    {
        tgt = oinit[I], coef = (float) (147.00000000000003 / f);
        return;
    }
    if (rc <= -1.0)
    {
        if (el < (double) oA[I])
            tgt = 5.0f, coef = (float) (8.82 / ((double) oA[I] * 39.441975 * f));
        else
            tgt = oS[I], coef = (float) (8.82 / (double) (oD[I] * fs));
        return;
    }
    tgt = oinit[I], coef = (float) (8.82 / (double) (oR[I] * fs));
}

void Voice::opsSetup (int n) noexcept // FUN_180039b30
{
    osc.routes.clear();
    const auto nu = nuni;
    const auto r = (float) ((double) s->uniDtn * (0.083333333333333329 / nu) + 1.0);
    const auto base = powf32 (r, (float) (nu - 1) * -0.5f);
    std::array<int, 6> lanesOf {};
    int idx = 0;
    for (std::size_t i = 0; i < 6; ++i)
        if (oen[i])
            lanesOf[i] = idx, idx += nu;
    for (std::size_t i = 0; i < 6; ++i)
    {
        if (! oen[i])
            continue;
        const auto& op = s->ops[i];
        auto gl2 = (1.0f - op.pan) * op.mix;
        gl2 = gl2 + gl2;
        auto gr2 = op.mix * op.pan;
        gr2 = gr2 + gr2;
        float etg = 0.0f, ecoef = 0.0f;
        opEnv ((int) i, etg, ecoef);
        auto lv = (float) (((double) op.l1 * lfov[0] + 1.0 + (double) op.l2 * lfov[1]) * (double) ((velS - 0.5f) * ovs[i] + 0.5f));
        if (! op.enable)
            lv = 9.99999975e-06f;
        float h0, h1, h2;
        if (start)
            h0 = 0.0f, h1 = 0.0f, h2 = lv;
        else
            h0 = hist[1][i], h1 = hist[2][i], h2 = hist[3][i];
        hist[0][i] = h0, hist[1][i] = h1, hist[2][i] = h2, hist[3][i] = lv;
        const auto c1v = (h2 - h0) * 0.5f;
        const auto c3v = (h1 - h2) * 1.5f + (lv - h0) * 0.5f;
        const auto c2v = ((h0 - h1) + c1v) - c3v;
        auto d = s->l2p * lfov[1] + (double) (op.fine + op.coarse) * 0.083333333333333329;
        d = std::pow (2.0, d + (double) fe * s->egf2p);
        const auto pf = Engine::hzPitch (pitch());
        const auto f0 = d * (double) pf * (double) base;
        const auto fsh = op.shiftS;
        const auto* tab = s->mip ((int) i, f0 + fsh, (double) fs);
        float mul = 1.0f;
        for (int u = 0; u < nu; ++u)
        {
            double f;
            if (u == 0)
                f = f0 + fsh;
            else
            {
                mul = u > 1 ? mul * r : r;
                f = (double) mul * f0 + fsh;
            }
            auto sp = 0.0f;
            if (nu > 1)
                sp = ((float) u / (float) (nu - 1)) * 2.0f, sp = sp - 1.0f;
            sp = sp * s->uniPan;
            auto cr = 1.0f - sp;
            cr = cr > 0.0f ? std::min (cr, 1.0f) : 0.0f;
            auto cl = sp + 1.0f;
            cl = cl > 0.0f ? std::min (cl, 1.0f) : 0.0f;
            const auto gR = ((cr * s->LR[(std::size_t) note][1]) * 0.5f) * gr2;
            const auto gL = ((cl * s->LR[(std::size_t) note][0]) * 0.5f) * gl2;
            const auto ln = lanesOf[i] + u;
            osc.setLane (ln, gL, gR, etg, ecoef, h1, c1v, c2v, c3v, f, tab, fs, n);
            for (std::size_t j = 0; j < 6; ++j)
                if (oen[j] && (double) op.fm[j] > 1e-05)
                    osc.routes.push_back ({ lanesOf[j] + u, ln, op.fm[j] + op.fm[j] });
        }
    }
    nl = idx;
    for (int ln = idx; ln < (idx + 3) / 4 * 4; ++ln)
    {
        osc.initLane (ln, 0.0f, 0.0f, false, s->rng); // padding lanes also draw from the generator
        osc.tab[(std::size_t) ln] = nullptr;
    }
}

void Voice::filterSetup (int n) noexcept // FUN_1800395b0
{
    const auto& sm = s->sm;
    auto rs = (double) sm[Engine::kRes];
    EA = sm[Engine::kEga] * 65535.0f;
    auto d = ((double) pitch() - 60.0) * 0.01;
    d = d * (double) sm[Engine::kKt];
    d = d + (double) sm[Engine::kVt] * ((double) velS - 1.0) * 0.5;
    const auto d7 = (double) sm[Engine::kL1a] * lfov[0];
    const auto f = (double) fs;
    float tgv;
    double kk;
    if (fade == 0)
    {
        if (rc > -1.0)
            tgv = 0.0f, kk = 8.82 / (double) (fs * fR);
        else if ((double) fA <= el)
            tgv = fS + 1e-7f, kk = 8.82 / (double) (fs * fD);
        else
            tgv = 5.0f, kk = 8.82 / ((double) fA * 39.441975 * f);
    }
    else
        tgv = 0.0f, kk = 147.00000000000003 / f;
    fet = tgv, fek = (float) kk;
    const auto target = (d + (d7 + (double) sm[Engine::kCut])) * 65535.0;
    if (U < 0.0)
        U = target;
    ct = { 0, 0, 0, 1 };
    rs = rs > 0.0 ? std::min (rs, 1.0) : 0.0;
    Uinc = (target - U) / (double) n;
    res = (float) rs;
    switch (s->fmode)
    {
        case 1: ct = { 0, 0, 1, 1e-7f }; break;
        case 2: ct = { 0, 1, -1, 1e-7f }; break;
        case 3: ct = { 1, 0, -1, 1e-7f }; break;
        default: break;
    }
    if (c0 < -2.0f)
        c0 = ct[0];
    if (c1 < -2.0f)
        c1 = ct[1];
    if (c2 < -2.0f)
        c2 = ct[2];
    if (beta < -2.0f)
        beta = ct[3];
}

float Voice::ladder (float x, int o, float k, float p, float R, float oneB, float b) noexcept
{
    const auto v = x - st[(std::size_t) o + 7] * (res * R); // s4·(res·R), asm 0x180038ef6
    const auto u = saturate (v);
    auto* S = st.data() + o;
    const auto p0 = S[0];
    S[0] = u;
    const auto s1 = (u + p0) * p - k * S[4];
    S[4] = s1;
    const auto p1 = S[1];
    S[1] = s1;
    const auto s2 = (s1 + p1) * p - k * S[5];
    S[5] = s2;
    const auto p2 = S[2];
    S[2] = s2;
    const auto s3 = (s2 + p2) * p - k * S[6];
    S[6] = s3;
    const auto p3 = S[3];
    S[3] = s3;
    const auto s4 = (s3 + p3) * p - S[7] * k;
    S[7] = s4;
    return (((c0 * u + c1 * s3) + c2 * s4) * oneB) + b * x;
}

void Voice::render (float* outL, float* outR, int n) noexcept // FUN_180038d00
{
    const auto& T = ladderTable().t;
    int p = 0;
    while (n > 0 && note >= 0)
    {
        if (blk == blockLen && ! block (blockLen))
            return;
        const auto m = std::min (blk, n);
        blk -= m;
        if (blk == 0)
            blk = blockLen;
        n -= m;
        osc.render (m, nl, bufL, bufR);
        for (int i = 0; i < m; ++i)
        {
            const auto xl = bufL[i], xr = bufR[i];
            const auto b = beta;
            auto e = fe;
            float yl = xl, yr = xr;
            if (! (b >= 0.999998987f))
            {
                const auto oneB = 1.0f - b;
                const auto ix = std::min (std::max (i32 ((double) (e * EA) + U), 0), 65535);
                const auto* c = T.data() + 3 * (std::size_t) ix;
                U = U + Uinc;
                yl = ladder (xl, 0, c[0], c[1], c[2], oneB, b);
                yr = ladder (xr, 8, c[0], c[1], c[2], oneB, b);
            }
            e = (fet - e) * fek + e;
            fe = e > 1.0f ? 1.0f : e;
            const auto k5 = 0.00499999989f;
            c0 = (ct[0] - c0) * k5 + c0;
            c1 = (ct[1] - c1) * k5 + c1;
            c2 = (ct[2] - c2) * k5 + c2;
            beta = (ct[3] - b) * k5 + b;
            if (-1e-9f < c0 && c0 < 1e-9f)
                c0 = 0.0f;
            if (-1e-9f < c1 && c1 < 1e-9f)
                c1 = 0.0f;
            if (-1e-9f < c2 && c2 < 1e-9f)
                c2 = 0.0f;
            const auto a = amp;
            const auto na = (ampt - a) * ampk + a;
            amp = na > 1.0f ? 1.0f : na;
            outL[p] = yl * a + outL[p];
            outR[p] = yr * a + outR[p];
            ++p;
        }
    }
}

// ------------------------------------------------------------------------------------------------------- wrapper

int velFromInitVol (double iv) noexcept // CFLWrapper TriggerVoice (round 28 §4.2)
{
    const auto x = (float) iv * 10.0f + 1.0f;
    const auto v = (float) std::log ((double) x) * 0.3284587264060974f * 127.0f;
    return std::clamp (i32 ((double) v), 0, 127);
}

void lawA (double pan, double vol, float& l, float& r) noexcept // FL host ComputeLRVol, law A (round 28 §4.1)
{
    const auto t = (1.0 + std::clamp (pan, -1.0, 1.0)) * 0.78539816339744831;
    l = (float) (vol * std::cos (t));
    r = (float) (vol * std::sin (t));
}

class Plugin
{
public:
    struct Held
    {
        bool on = false;
        double fvol = 1.0, fpan = 0.0, pitchCents = 0.0;
        bool released = false;
        HostSlide slide; // a slide note moving Final.Pitch (issue #14)
    };
    Engine eng;
    Held held[128];
    bool sliding = false;
    float chunkL[chunk] {}, chunkR[chunk] {};

    void init (double fs, const Patch& p, std::shared_ptr<const BuiltinSpectra> spectra, bool withEffects)
    {
        eng.init (fs, p, std::move (spectra), withEffects);
        for (auto& h : held)
            h = Held {};
    }

    void noteOn (int key, double initVol, double fvol, double fpan, double pitchCents)
    {
        key = std::clamp (key, 0, 127);
        if (held[key].on)
            eng.noteOff (key);
        held[key] = { true, fvol, fpan, pitchCents, false, {} };
        eng.noteOn (key, velFromInitVol (initVol));
    }
    void noteOff (int key)
    {
        if (key >= 0 && key < 128)
        {
            held[key].released = true;
            eng.noteOff (key);
        }
    }
    // a slide note: every held, unreleased key glides to `toCents` (Final.Pitch, cents from C5) over `samples`
    void slideTo (double toCents, float samples) noexcept
    {
        for (int i = 0; i < 128; ++i)
            if (held[i].on && ! held[i].released)
            {
                held[i].slide.start ((float) (toCents - (100.0 * i - 6000.0 + held[i].pitchCents)), samples);
                sliding = true;
            }
    }
    void setLevels (int key, double fvol, double fpan) noexcept
    {
        if (key >= 0 && key < 128 && held[key].on)
            held[key].fvol = fvol, held[key].fpan = fpan;
    }

    void tables() noexcept
    {
        float cl = 0.0f, cr = 0.0f, cp = 0.0f;
        bool first = true;
        for (int i = 0; i < 128; ++i)
        {
            if (held[i].on)
            {
                const auto& h = held[i];
                lawA ((double) (float) h.fpan, (double) (float) h.fvol, cl, cr); // TVoiceParams hold floats
                const auto fp = (100.0f * (float) i - 6000.0f)
                                + (h.slide.cents != 0.0f ? (float) (h.pitchCents + (double) h.slide.cents) : (float) h.pitchCents);
                cp = (float) ((double) (float) fp * 0.01 - (double) i + 60.0);
                if (first)
                {
                    for (int j = 0; j < i; ++j)
                        eng.LR[(std::size_t) j] = { cl, cr }, eng.ptab[(std::size_t) j] = cp;
                    first = false;
                }
            }
            eng.LR[(std::size_t) i] = { cl, cr }, eng.ptab[(std::size_t) i] = cp;
        }
    }

    bool idle() const noexcept
    {
        if (eng.anyActive() || eng.fx.active() || eng.lfoRunning())
            return false;
        return std::none_of (std::begin (held), std::end (held), [] (const Held& h) { return h.on; });
    }

    void render (float* outL, float* outR, int n)
    {
        tables();
        for (int c0 = 0; c0 < n; c0 += chunk)
        {
            const auto cn = std::min (chunk, n - c0);
            eng.process (cn, chunkL, chunkR);
            for (int i = 0; i < cn; ++i)
                outL[c0 + i] += chunkL[i], outR[c0 + i] += chunkR[i];
            if (sliding) // (issue #14) the slide moves the pitch table chunk by chunk
            {
                sliding = false;
                for (auto& h : held)
                    if (h.slide.moving())
                    {
                        h.slide.advance ((float) cn);
                        sliding = sliding || h.slide.moving();
                    }
                tables();
            }
        }
        for (int key = 0; key < 128; ++key)
            if (held[key].on && eng.vtab[(std::size_t) key] == 0
                && std::none_of (eng.voices.begin(), eng.voices.end(), [key] (const Voice& v) { return v.note == key; }))
                held[key].on = false;
    }
};

bool needsDll (const Patch& p) noexcept
{
    for (int k = 0; k < 8; ++k)
        if (waveOf (p, k) >= 2 && waveOf (p, k) <= 32)
            return true;
    return false;
}

// ------------------------------------------------------------------------------------------------------- LYNX front-end
//
// Levels, velocity and tempo as the GMS front-end (core/Gms.cpp): FL hands the plugin Final.Vol = channel volume and
// Final.Pan = note pan + channel pan through ComputeLRVol, so LYNX recovers V and p from its channel gains and does not
// apply them a second time; Init.Vol = (11^(1.28·vel) − 1)/10 from LYNX's velocity, then the wrapper's MIDI velocity.
// The max-polyphony event is taken as value 0 (→ 32, FL's value not confirmed). The tempo is the song tempo.

struct Setup
{
    Patch patch {};
    std::shared_ptr<const BuiltinSpectra> spectra;
    bool valid = false;
};

class ToxicInstrument final : public Instrument
{
public:
    ToxicInstrument()
    {
        setup = new Setup();
        setup->spectra = builtinSpectra ({});
        plugin = std::make_unique<Plugin>();
        plugin->init (44100.0, defaultPatch(), setup->spectra, true);
    }
    ~ToxicInstrument() override
    {
        delete incoming.exchange (nullptr);
        delete retired.exchange (nullptr);
        delete setup;
    }

    void prepare (double sr) noexcept override
    {
        take();
        rate = sr;
        applySetup();
    }

    void configure (const ChannelDef& channel, double bpm) noexcept override
    {
        delete retired.exchange (nullptr);
        pitchCents.store (channel.sampler.pitch * 100.0f);
        tempo.store (bpm > 0.0 ? (float) bpm : 125.0f);
        if (channel.pluginState == state && channel.samplePath == dllPath)
            return;
        state = channel.pluginState;
        dllPath = channel.samplePath;
        auto s = std::make_unique<Setup>();
        s->spectra = builtinSpectra (dllPath);
        if (auto p = parseState (state); p && (! needsDll (*p) || s->spectra->fromDll))
        {
            s->patch = *p;
            s->valid = true;
            // build the tables here (UI thread); the audio thread then finds them in the cache
            for (int k = 0; k < 8; ++k)
                if (const auto w = waveOf (s->patch, k); w >= 0 && w <= 32)
                    warm[(std::size_t) k] = buildTable (w == 0 ? s->patch.drawn[(std::size_t) k] : s->spectra->waves[(std::size_t) w]);
        }
        else
            s->patch = defaultPatch();
        delete incoming.exchange (s.release());
    }

    void setNextNoteExpression (float pan, float cents, bool) noexcept override
    {
        nextPan = pan;
        nextCents = cents;
    }

    void noteOn (int key, float velocity) noexcept override
    {
        take();
        const auto vel = std::clamp ((double) velocity, 0.0, 1.0);
        const auto initVol = (std::pow (11.0, 1.28 * vel) - 1.0) / 10.0;
        const auto k = std::clamp (key, 0, 127);
        notePan[k] = nextPan;
        plugin->noteOn (k, initVol, channelVol, std::clamp ((double) nextPan + channelPan, -1.0, 1.0),
                        (double) pitchCents.load() + (double) nextCents);
        nextPan = 0.0f;
        nextCents = 0.0f;
    }

    void noteOff (int key) noexcept override
    {
        take();
        plugin->noteOff (key);
    }

    void slideTo (int key, float cents, float samples) noexcept override
    {
        take();
        plugin->slideTo (100.0 * (std::clamp (key, 0, 127) - 60) + (double) pitchCents.load() + (double) cents, samples);
    }

    void allNotesOff() noexcept override
    {
        for (int k = 0; k < 128; ++k)
            if (plugin->held[k].on)
                plugin->noteOff (k);
    }

    bool idle() const noexcept override { return plugin->idle(); }

    void render (float* left, float* right, int n, float gl, float gr) noexcept override
    {
        take();
        plugin->eng.tempo = (double) tempo.load();
        const auto vol = std::sqrt (((double) gl * gl + (double) gr * gr) * 0.5);
        auto pan = vol > 0.0 ? std::atan2 ((double) gr, (double) gl) * (4.0 / 3.141592653589793) - 1.0 : 0.0;
        if (std::fabs (pan) < 1e-9)
            pan = 0.0;
        if (vol != channelVol || pan != channelPan)
        {
            channelVol = vol, channelPan = pan;
            for (int k = 0; k < 128; ++k)
                plugin->setLevels (k, vol, std::clamp ((double) notePan[k] + pan, -1.0, 1.0));
        }
        if (n > 0)
            plugin->render (left, right, n);
    }

private:
    static Patch defaultPatch()
    {
        Patch p;
        p.P = defaults();
        return p;
    }
    void applySetup() noexcept
    {
        // the engine is created like FL creates the plugin: constructor values, the rate, then the state (§1.2)
        plugin->init (rate, setup->patch, setup->spectra, true);
    }
    void take() noexcept
    {
        if (retired.load() != nullptr)
            return;
        if (auto* s = incoming.exchange (nullptr))
        {
            retired.store (setup);
            setup = s;
            applySetup();
        }
    }

    std::unique_ptr<Plugin> plugin;
    Setup* setup = nullptr;
    std::atomic<Setup*> incoming { nullptr }, retired { nullptr };
    std::array<std::shared_ptr<const WaveTable>, 8> warm {};
    std::atomic<float> pitchCents { 0.0f }, tempo { 125.0f };
    std::vector<std::uint8_t> state;
    std::string dllPath;
    double rate = 44100.0, channelVol = 1.0, channelPan = 0.0;
    float notePan[128] {};
    float nextPan = 0.0f, nextCents = 0.0f;
};
} // namespace

bool toxicStateUsable (const std::vector<std::uint8_t>& state) { return parseState (state).has_value(); }

bool toxicNeedsDll (const std::vector<std::uint8_t>& state)
{
    const auto p = parseState (state);
    return p && needsDll (*p);
}

bool toxicDllUsable (const std::string& dllPath) { return builtinSpectra (dllPath)->fromDll; }

int toxicParamOffset (const std::vector<std::uint8_t>& state, int index)
{
    if (index < 0 || index >= paramCount || ! parseState (state))
        return -1;
    return (int) paramsAt + 4 * index;
}

std::vector<std::uint8_t> toxicDefaultState()
{
    std::vector<std::uint8_t> s (stateSize, 0);
    std::memcpy (s.data(), "SSLF", 4);
    const std::uint32_t size = (std::uint32_t) stateSize - 8;
    std::memcpy (s.data() + 4, &size, 4);
    std::memcpy (s.data() + 8, "2iBT", 4);
    const auto d = defaults();
    std::memcpy (s.data() + paramsAt, d.data(), 4 * paramCount);
    std::fill (s.begin() + (std::ptrdiff_t) (paramsAt + 4 * paramCount), s.begin() + (std::ptrdiff_t) (paramsAt + 4 * paramCount + 294), 0xff);
    return s;
}

std::unique_ptr<Instrument> makeToxicInstrument() { return std::make_unique<ToxicInstrument>(); }

namespace toxic
{
std::vector<float> renderReference (const std::vector<std::uint8_t>& state, const std::vector<RefNote>& notes, double sampleRate,
                                    int length, const std::string& dllPath, int block, double bpm, int maxPoly, bool effects)
{
    const auto p = parseState (state);
    const auto spectra = builtinSpectra (dllPath);
    if (! p || length <= 0 || (needsDll (*p) && ! spectra->fromDll))
        return {};
    auto plugin = std::make_unique<Plugin>();
    plugin->init (sampleRate, *p, spectra, effects);
    plugin->eng.tempo = bpm;
    plugin->eng.npolyEvent = maxPoly;
    std::vector<int> evs;
    for (const auto& nt : notes)
    {
        evs.push_back (nt.start);
        if (nt.release >= 0)
            evs.push_back (nt.release);
    }
    std::sort (evs.begin(), evs.end());
    block = std::max (1, block);
    std::vector<float> out (2 * (std::size_t) length, 0.0f), l ((std::size_t) block), r (l.size());
    int n0 = 0;
    while (n0 < length)
    {
        for (const auto& nt : notes)
        {
            if (nt.start == n0)
                plugin->noteOn (nt.key, nt.initVol, nt.finalVol, nt.finalPan, nt.pitchCents);
            if (nt.release == n0)
                plugin->noteOff (std::clamp (nt.key, 0, 127));
        }
        auto ln = std::min (block, length - n0);
        for (auto e : evs)
            if (n0 < e && e < n0 + ln)
                ln = e - n0;
        std::fill (l.begin(), l.begin() + ln, 0.0f);
        std::fill (r.begin(), r.begin() + ln, 0.0f);
        plugin->render (l.data(), r.data(), ln);
        for (int j = 0; j < ln; ++j)
        {
            out[2 * (std::size_t) (n0 + j)] = l[(std::size_t) j];
            out[2 * (std::size_t) (n0 + j) + 1] = r[(std::size_t) j];
        }
        n0 += ln;
    }
    return out;
}
} // namespace toxic
} // namespace lynx
