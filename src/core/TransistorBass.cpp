// FL Studio's Transistor Bass engine (docs/analise/REA_rodada57.md §2). Clean-room: our own code from the behaviour, data
// layouts and formulas described in that document. Section numbers (§) below refer to it.

#include "core/TransistorBass.h"
#include "core/HostSlide.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>

#include "core/Sha256.h"
#include "core/Song.h"

#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__) || defined(__aarch64__) || defined(_M_ARM64) // x86 SSE, or ARM64 through core/Simd.h (sse2neon)
#include "core/Simd.h"
#define LYNX_TBASS_MXCSR 1
#endif

// the engine's float operations, one rounding each, in its order: no contraction, no reordering
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

namespace lynx
{
namespace
{
// The render flushes denormals (MXCSR | 0x8040) and keeps round-to-nearest (§2.5).
#ifdef LYNX_TBASS_MXCSR
struct RenderMode
{
    unsigned saved;
    RenderMode() noexcept : saved (_mm_getcsr()) { _mm_setcsr ((saved | 0x8040u) & ~0x6000u); }
    ~RenderMode() { _mm_setcsr (saved); }
};
#else
struct RenderMode
{
};
#endif

double powD (double a, double b) noexcept { return a == 2.0 ? std::exp2 (b) : (double) std::pow ((long double) a, (long double) b); }
float powF (float a, float b) noexcept { return (float) powD ((double) a, (double) b); }
float sinF (float x) noexcept { return (float) std::sin ((double) x); }
float expF (float x) noexcept { return (float) std::exp ((double) x); }
float lnF (float x) noexcept { return (float) std::log ((double) x); }
float bitsF (std::uint32_t u) noexcept
{
    float f;
    std::memcpy (&f, &u, 4);
    return f;
}

// ------------------------------------------------------------------------------------------------ installation data (§2.3)
constexpr const char* engineSha256 = "4cc72470063a0a9f8f126102415cbb1ef2b91abb2995cd9fbdf2c8e676c6907f";

struct Wave
{
    std::vector<float> data; // n points + 1 guard (data[n] = data[0])
    int length = 0;
};

struct DllData
{
    std::vector<Wave> recorded;  // engine V1's 57 "303 Pulse" cycles (192 kHz), longest first
    std::vector<Wave> recorded2; // engine V2's 55 cycles (44.1 kHz), longest first
};

// 16-bit mono RIFF/WAVE → floats x/32768
bool decodeWav (const std::uint8_t* p, std::size_t n, std::vector<float>& out)
{
    if (n < 12 || std::memcmp (p, "RIFF", 4) != 0 || std::memcmp (p + 8, "WAVE", 4) != 0)
        return false;
    std::size_t at = 12;
    int bits = 0, channels = 0;
    while (at + 8 <= n)
    {
        std::uint32_t size = 0;
        std::memcpy (&size, p + at + 4, 4);
        if (std::memcmp (p + at, "fmt ", 4) == 0 && size >= 16)
        {
            std::uint16_t c = 0, b = 0;
            std::memcpy (&c, p + at + 10, 2);
            std::memcpy (&b, p + at + 22, 2);
            channels = c, bits = b;
        }
        else if (std::memcmp (p + at, "data", 4) == 0)
        {
            if (bits != 16 || channels != 1 || at + 8 + size > n)
                return false;
            out.resize (size / 2);
            for (std::size_t i = 0; i < out.size(); ++i)
            {
                std::int16_t s = 0;
                std::memcpy (&s, p + at + 8 + 2 * i, 2);
                out[i] = (float) s / 32768.0f;
            }
            return true;
        }
        at += 8 + size + (size & 1);
    }
    return false;
}

std::shared_ptr<const DllData> loadDll (const std::string& path)
{
    static std::mutex lock;
    static std::map<std::string, std::weak_ptr<const DllData>> cache;
    std::lock_guard<std::mutex> g (lock);
    if (auto t = cache[path].lock())
        return t;
    std::ifstream in (path, std::ios::binary);
    if (! in)
    {
        auto local = path;
        std::replace (local.begin(), local.end(), '\\', '/');
        in.open (local, std::ios::binary);
    }
    if (! in)
        return nullptr;
    const std::vector<std::uint8_t> b ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    if (b.size() < 0x1000 || sha256Hex (b.data(), b.size()) != engineSha256)
        return nullptr;
    auto rd = [&] (std::size_t off, auto& v) { if (off + sizeof (v) <= b.size()) std::memcpy (&v, b.data() + off, sizeof (v)); };
    std::uint32_t pe = 0;
    rd (0x3c, pe);
    std::uint16_t sections = 0, optSize = 0;
    rd (pe + 6, sections);
    rd (pe + 20, optSize);
    struct Section { std::uint32_t va, vs, ro, rs; };
    std::vector<Section> secs;
    for (std::uint32_t i = 0, o = pe + 24 + optSize; i < sections; ++i, o += 40)
    {
        Section s {};
        rd (o + 8, s.vs);
        rd (o + 12, s.va);
        rd (o + 16, s.rs);
        rd (o + 20, s.ro);
        secs.push_back (s);
    }
    auto offset = [&] (std::uint32_t rva) -> std::size_t {
        for (const auto& s : secs)
            if (rva >= s.va && rva < s.va + std::min (s.vs, s.rs))
                return (std::size_t) (s.ro + rva - s.va);
        return b.size();
    };
    auto t = std::make_shared<DllData>();
    // recorded cycles: count, then [size][RIFF bytes] (§2.3); each: (x − 0.63)·6.3157892, mean removed, guard point
    auto readSet = [&] (std::uint32_t rva, int expected, std::vector<Wave>& out) {
        auto at = offset (rva);
        std::int32_t count = 0;
        rd (at, count);
        if (count != expected)
            return false;
        at += 4;
        for (int k = 0; k < count; ++k)
        {
            std::int32_t size = 0;
            rd (at, size);
            if (size <= 0 || at + 4 + (std::size_t) size > b.size())
                return false;
            Wave w;
            if (! decodeWav (b.data() + at + 4, (std::size_t) size, w.data))
                return false;
            w.length = (int) w.data.size();
            const auto scale = 6.315789222717285f; // float constant
            for (auto& x : w.data)
                x = (float) ((double) x - 0.63) * scale;
            double sum = 0.0; // the mean is taken in double
            for (int i = 0; i < w.length; ++i)
                sum += (double) w.data[(std::size_t) i];
            const auto mean = sum / (double) w.length;
            for (int i = 0; i < w.length; ++i)
                w.data[(std::size_t) i] = (float) ((double) w.data[(std::size_t) i] - mean);
            w.data.push_back (w.data[0]);
            out.push_back (std::move (w));
            at += 4 + (std::size_t) size;
        }
        return true;
    };
    if (! readSet (0xFEAF0, 57, t->recorded) || ! readSet (0x15B2F0, 55, t->recorded2))
        return nullptr;
    cache[path] = t;
    return t;
}

// ----------------------------------------------------------------------------------- band-limited saw tables (§2.4)
// 48 tables: a 1024-point falling ramp resampled by 2^(−1/4) per level with a 64-tap windowed-sinc kernel (16 384
// phases), each then upsampled ×16 with the same kernel; one guard point before, two after.
struct SawTables
{
    std::vector<std::vector<float>> data; // per level: [guard, n points, 2 guards]
    std::vector<int> length;
};

const SawTables& sawTables()
{
    static const SawTables tables = [] {
        constexpr int phases = 0x4000, taps = 64;
        std::vector<float> kernel ((std::size_t) phases * taps);
        const auto q = std::pow (2.0, 0.25);
        const auto pi = 3.141592653589793;
        for (int p = 0; p < phases; ++p)
        {
            const auto off = (double) p * 6.103515625e-05;
            for (int j = 0; j < taps; ++j)
            {
                auto u = std::max (0.0, ((double) j - off) * 0.015625);
                auto w = 0.5 - std::cos ((u + u) * pi) * 0.5;
                w = w * w;
                auto t = std::max (-32.0, (double) (j - 32) - off) / q * pi;
                const auto s = t == 0.0 ? 1.0 : std::sin (t) / t;
                kernel[(std::size_t) p * taps + (std::size_t) j] = (float) (s * w / q);
            }
        }
        auto resample = [&] (const std::vector<float>& src, int n, double x) {
            const auto i = (int) x;
            const auto ph = (int) ((x - (double) i) * 16384.0);
            const auto* k = kernel.data() + (std::size_t) ph * taps;
            auto base = i - 30 + n * 100;
            double acc = 0.0;
            for (int j = 0; j < taps; ++j)
                acc += (double) src[(std::size_t) ((base + j - 2) % n)] * (double) k[j];
            return (float) acc;
        };
        SawTables t;
        std::vector<std::vector<float>> raw (48);
        std::vector<int> len (48);
        raw[0].resize (1024);
        for (int i = 0; i < 1024; ++i)
            raw[0][(std::size_t) i] = (float) -(((double) i * 2.0) * 0.0009765625 - 1.0);
        len[0] = 1024;
        const auto ratio = std::pow (2.0, 1.0 / 4.0);
        auto size = 1024.0;
        for (int lv = 1; lv < 48; ++lv)
        {
            size /= ratio;
            auto n = (int) (size + 0.5);
            if (n < 2)
                n = 2;
            raw[(std::size_t) lv].resize ((std::size_t) n);
            const auto step = (double) len[(std::size_t) lv - 1] / (double) n;
            auto x = 0.0;
            for (int k = 0; k < n; ++k, x += step)
                raw[(std::size_t) lv][(std::size_t) k] = resample (raw[(std::size_t) lv - 1], len[(std::size_t) lv - 1], x);
            len[(std::size_t) lv] = n;
        }
        t.data.resize (48);
        t.length.resize (48);
        for (int lv = 0; lv < 48; ++lv)
        {
            const auto n = len[(std::size_t) lv] * 16;
            std::vector<float> up ((std::size_t) n + 3);
            auto x = 0.0;
            for (int k = 0; k < n; ++k, x += 1.0 / 16.0)
                up[(std::size_t) k + 1] = resample (raw[(std::size_t) lv], len[(std::size_t) lv], x);
            up[0] = up[(std::size_t) n];
            up[(std::size_t) n + 1] = up[1];
            up[(std::size_t) n + 2] = up[2];
            t.data[(std::size_t) lv] = std::move (up);
            t.length[(std::size_t) lv] = n;
        }
        return t;
    }();
    return tables;
}

// ------------------------------------------------------------------------------------------------------- one-pole (§2.5)
struct OnePole
{
    float y = 0.0f, x1 = 0.0f, b0 = 0.0f, b1 = 0.0f, a1 = 0.0f;
    int highPass = 0;
    float freq = 100.0f, piOverFs = 7.1237928e-05f;

    void coefficients (float t) noexcept
    {
        if (highPass)
        {
            const auto num = t - 1.0f, den = t + 1.0f;
            b0 = 1.0f / den;
            b1 = -b0;
            a1 = -num / den;
        }
        else
        {
            const auto den = t + 1.0f;
            b0 = t / den;
            b1 = b0;
            a1 = -(t - 1.0f) / den;
        }
    }
    void setRate (double fs) noexcept
    {
        piOverFs = (float) (1.0 / fs) * 3.1415927410125732f;
        coefficients ((float) std::tan ((double) (piOverFs * freq)));
    }
    void setFreq (float f) noexcept
    {
        freq = f;
        coefficients ((float) std::tan ((double) (freq * piOverFs)));
    }
};

// -------------------------------------------------------------------------------------------- diode ladder (§2.5)
struct Ladder
{
    float fb = 0.0f, hp1 = 0.0f; // feedback one-pole high-pass: output, previous input
    float b0 = 0.0f, b1 = 0.0f, a1 = 0.0f;
    double g = 0.0, y1 = 0.0, y2 = 0.0, y3 = 0.0, y4 = 0.0, k = 0.0, outGain = 0.0, maxCut = 0.0, wcScale = 0.0;

    void setRate (double fs) noexcept
    {
        maxCut = fs * 0.4;
        wcScale = 1.0 / (fs * 1.4142135623730951);
        const auto piOverFs = (float) (1.0 / fs) * 3.1415927410125732f;
        const auto t = (float) std::tan ((double) (piOverFs * 160.0f));
        const auto num = t - 1.0f, den = t + 1.0f;
        b0 = 1.0f / den;
        b1 = -b0;
        a1 = -num / den;
    }
    void set (float cutoff, float res, float gainOverride) noexcept
    {
        auto f = cutoff;
        if (11.0f > f)
            f = 11.0f;
        else if ((double) f > maxCut)
            f = (float) maxCut;
        const auto wc = (double) f * wcScale;
        g = (wc * 6.192218900001 + 0.0004552234600001) / (wc * wc * 4.415634500001 + (wc * 12.35835400001 + 1.000001));
        auto p = (((wc + 7198.699700001) * wc - 5837.791700001) * wc - 476.4730800001) * wc;
        p = ((p + 614.9561100001) * wc + 213.8712600001) * wc + 16.99879200001;
        const auto r = (double) res;
        if (0.0f > gainOverride)
            outGain = (((p * 0.058823529411764705 - 1.0) * r + 1.0) * (r + 1.0)) * 2.0;
        else
            outGain = (double) gainOverride;
        k = p * r;
    }
    float tick (float x) noexcept
    {
        const auto t = (float) (y4 * k);
        const auto u = b1 * hp1, v = a1 * fb;
        hp1 = t;
        auto w = t * b0;
        w = w + u;
        w = w + v;
        fb = w;
        auto a = (double) (x - w);
        a = ((a - y1) + y2) * g;
        y1 = a + a + y1;
        y2 = ((y1 - (y2 + y2)) + y3) * g + y2;
        y3 = ((y2 - (y3 + y3)) + y4) * g + y3;
        y4 = (y3 - (y4 + y4)) * g + y4;
        return (float) (y4 * outGain);
    }
};

// ------------------------------------------------------------------------------------------ half-band decimator (§2.5)
struct HalfBand
{
    struct Allpass { double a, x1 = 0, x2 = 0, y1 = 0, y2 = 0; };
    std::array<Allpass, 6> pathA, pathB;
    double prevB = 0.0;

    HalfBand()
    {
        constexpr double a[6] = { 0.036681502163648017, 0.2746317593794541, 0.5610989697879195, 0.769741833862266, 0.8922608180038789, 0.962094548378084 };
        constexpr double b[6] = { 0.13654762463195771, 0.42313861743656667, 0.6775400499741616, 0.839889624849638, 0.9315419599631839, 0.9878163707328971 };
        for (int i = 0; i < 6; ++i)
            pathA[(std::size_t) i].a = a[i], pathB[(std::size_t) i].a = b[i];
    }
    static double run (std::array<Allpass, 6>& path, double x) noexcept
    {
        for (auto& s : path)
        {
            const auto x1 = s.x1;
            s.x1 = x;
            x = (x - s.y2) * s.a + s.x2;
            s.x2 = x1;
            s.y2 = s.y1;
            s.y1 = x;
        }
        return x;
    }
    // one input sample; the output is meaningful on every second call
    double push (double x) noexcept
    {
        const auto out = (run (pathA, x) + prevB) * 0.5;
        prevB = run (pathB, x);
        return out;
    }
};

// ------------------------------------------------------------------------------------- Rat-style distortion (§2.7)
// five one-poles (7.2 Hz and 60 Hz high-passes, the "HP" high-pass, the drive low-pass, the "Tone" low-pass), the
// drive gain, a slew limiter with ±4.5 clamp and tanh, the output gain; runs at the oversampled rate
struct Distortion
{
    std::array<OnePole, 5> f;
    float drive = 0.0f, hpKnob = 0.0f, slew = 0.0f, slewMax = 0.0f, gain = 1.0f, level = 1.0f, out = 1.0f;

    Distortion()
    {
        f[0].highPass = 1, f[0].freq = 7.2f;
        f[1].highPass = 1, f[1].freq = 60.0f;
        f[2].highPass = 1, f[2].freq = 10.0f;
        f[3].highPass = 0, f[3].freq = 16000.0f;
        f[4].highPass = 0, f[4].freq = 19475.0f;
    }
    void setRate (double fs) noexcept
    {
        for (auto& x : f)
            x.setRate (fs);
        slewMax = (float) (300000.0 / fs);
    }
    float outputGain() const noexcept
    {
        auto e = 1.0f - expF (drive * -11.834190368652344f);
        e = e * 0.9819049835205078f;
        auto g = 1.0041040182113647f - e;
        g = g * level;
        return g * (hpKnob + 1.0f);
    }
    void setHp (float v) noexcept
    {
        hpKnob = v;
        auto fr = v * 3000.0f;
        fr = fr * v;
        f[2].highPass = 1;
        f[2].freq = fr + 10.0f;
        f[2].coefficients ((float) std::tan ((double) (f[2].freq * f[2].piOverFs)));
        out = outputGain();
    }
    void setDrive (float d) noexcept
    {
        static constexpr float taper[7] = { 16000.0f, 5000.0f, 2500.0f, 1500.0f, 900.0f, 600.0f, 8.871999740600586f };
        drive = d;
        auto x = d * 4.999000072479248f;
        const auto i = (int) x;
        x = x - (float) i;
        auto fr = (1.0f - x) * taper[i];
        fr = fr + x * taper[i + 1];
        f[3].highPass = 0;
        f[3].freq = fr;
        f[3].coefficients ((float) std::tan ((double) (fr * f[3].piOverFs)));
        auto g = d * d;
        g = g * d;
        gain = g * 2305.0f + 1.0f;
        out = outputGain();
    }
    void setTone (float v) noexcept
    {
        f[4].highPass = 0;
        auto fr = v * 19000.0f;
        fr = fr * v;
        f[4].freq = fr + 475.0f;
        f[4].coefficients ((float) std::tan ((double) (f[4].freq * f[4].piOverFs)));
    }
    void setVolume (float v) noexcept
    {
        auto e = v - 0.5f;
        e = e + e;
        level = (float) powD (4.0, (double) e);
        out = outputGain();
    }
    static float stage (OnePole& p, float x) noexcept
    {
        auto y = p.b1 * p.x1;
        y = y + x * p.b0;
        y = y + p.a1 * p.y;
        p.x1 = x;
        p.y = y;
        return y;
    }
    float tick (float x) noexcept
    {
        x = x * gain;
        auto y = stage (f[0], x);
        y = stage (f[1], y);
        y = stage (f[2], y);
        y = stage (f[3], y);
        auto d = y - slew;
        if (-slewMax > d)
            d = -slewMax;
        else
            d = std::min (slewMax, d);
        auto s = slew + d;
        if (-4.5f > s)
            s = -4.5f;
        else
            s = std::min (4.5f, s);
        slew = s;
        const auto t = (float) std::tanh ((double) s);
        auto& p = f[4];
        auto o = t * p.b0;
        o = o + p.b1 * p.x1;
        o = o + p.a1 * p.y;
        p.x1 = t;
        p.y = o;
        return o * out;
    }
};

// ------------------------------------------------------------------------------------------------ stereo delay (§2.8)
// one line (2^18 points) read at a left and a right tap, each a crossfade of two tap times (time changes fade over
// 0.3 s); the feedback runs through a 10 Hz high-pass, the send is added, then a 2×-iterated state-variable low-pass
// ("Tone", 270…22 000 Hz), a soft clipper with level follower and a 65-point line before the write; outputs through 20 Hz
// high-passes and the stereo matrix
struct Delay
{
    double fs = 44100.0;
    std::vector<float> line = std::vector<float> (0x40000, 0.0f), ap = std::vector<float> (0x8000, 0.0f);
    std::uint32_t w = 0, apIdx = 0;
    float send = 0.5f, feedback = 0.99f, tone = 0.0f, stereoA = 1.0f, stereoB = 0.0f, dry = 1.0f, wet = 1.0f;
    float satIn = 0.1f, satOut = 10.029618263244629f, env = 0.0f, envUp = 0.0f, envDown = 0.0f;
    std::array<float, 2> time { 0.1f, 0.1f }, right { 0.5f, 0.5f }, tapL { 0, 0 }, tapR { 0, 0 }, tapX { 0, 0 };
    OnePole fbHp, outL, outR;
    // state-variable filter
    float svSat = 0.0f, mixLp = 0.0f, mixHp = 1.0f, mixBp = 0.0f, svF = 0.0f, svQ = 0.0f, svCut = -1.0f, svRes = -1.0f;
    float svHpOut = 0.0f, svLp = 0.0f, svHp = 0.0f, svBp = 0.0f, fsF = 44100.0f;
    int svCounter = 0;

    Delay()
    {
        envUp = 1.0f - (float) powD (0.1, 0.0022675736961451248);
        envDown = 1.0f - (float) powD (0.1, 0.00022675736961451248);
        outL.highPass = outR.highPass = 1;
        outL.freq = outR.freq = 20.0f;
        setRate (44100.0);
    }
    void updateTaps (int i) noexcept
    {
        auto t = (float) fs * time[(std::size_t) i];
        t = t + 1.0f;
        tapL[(std::size_t) i] = t;
        const auto r = right[(std::size_t) i] * 0.5f;
        const auto a = 1.0f - r;
        t = t - 0.9998999834060669f;
        auto x = a * t;
        tapR[(std::size_t) i] = x + 1.0f;
        x = t * r;
        tapX[(std::size_t) i] = x + 1.0f;
    }
    void setRate (double rate) noexcept
    {
        fs = rate;
        fsF = (float) rate;
        svCut = svRes = -1.0f; // (the engine re-derives the filter from the next update)
        fbHp.highPass = 0;
        fbHp.freq = 100.0f;
        fbHp.setRate (44100.0);
        fbHp.highPass = 1;
        fbHp.setFreq (10.0f);
        outL.setRate (rate);
        outR.setRate (rate);
        updateTaps (0);
        updateTaps (1);
        clear();
    }
    void clear() noexcept
    {
        std::fill (line.begin(), line.end(), 0.0f);
        std::fill (ap.begin(), ap.end(), 0.0f);
        svHpOut = svLp = svHp = svBp = 0.0f;
        fbHp.y = fbHp.x1 = 0.0f;
    }
    void setSvf (float f, float q) noexcept
    {
        if (f == svCut && q == svRes)
            return;
        if (f != svCut)
        {
            auto x = f / (fsF + fsF);
            x = std::min (0.25f, x);
            svF = sinF (x * 3.1415927410125732f);
            svF = svF + svF;
            svCut = f;
        }
        const auto p = powF (q, 0.25f);
        svRes = q;
        auto damp = 1.0f - p;
        damp = damp + damp;
        auto lim = 2.0f / svF;
        lim = lim - svF * 0.5f;
        lim = std::min (2.0f, lim);
        svQ = std::min (damp, lim);
    }
    void modulation() noexcept
    {
        if (svCounter == 0)
        {
            // filter mode 0 (set by the plugin): low-pass on the 270…22 000 Hz table
            static constexpr float cut[6] = { 270.0f, 480.0f, 1300.0f, 7000.0f, 22000.0f, 0.0f };
            auto t = tone;
            t = 0.0f <= t ? std::min (1.0f, t) : 0.0f;
            auto x = t * 3.999000072479248f;
            const auto i = (int) x;
            const auto fr = x - (float) i;
            auto a = fr * cut[i + 1];
            auto b = (1.0f - fr) * cut[i];
            mixLp = 1.0f, mixHp = 0.0f, mixBp = 0.0f;
            setSvf (b + a, 0.0f * 0.25f);
        }
        svCounter = (svCounter + 1) & 3;
    }
    float svf (float x) noexcept
    {
        const auto bp = svBp, q = svQ, F = svF;
        auto lp1 = F * bp;
        lp1 = lp1 + svLp;
        auto hp1 = x - q * bp;
        hp1 = hp1 - lp1;
        auto t3 = svSat * bp;
        t3 = t3 * bp;
        t3 = t3 * bp;
        auto acc = mixLp * lp1;
        acc = acc + hp1 * mixHp;
        auto bp1 = hp1 * F;
        bp1 = bp1 + bp;
        bp1 = bp1 - t3;
        acc = acc + bp1 * mixBp;
        auto lp2 = bp1 * F;
        lp2 = lp2 + lp1;
        acc = acc * 0.5f;
        const auto hx = x - bp1 * q;
        svLp = lp2;
        svHpOut = hx;
        const auto hp2 = hx - lp2;
        svHp = hp2;
        auto t = bp1 * svSat;
        t = t * bp1;
        t = t * bp1;
        auto bp2 = hp2 * F;
        bp2 = bp2 + bp1;
        bp2 = bp2 - t;
        svBp = bp2;
        auto o = lp2 * mixLp;
        o = o + hp2 * mixHp;
        o = o + bp2 * mixBp;
        o = o * 0.5f;
        return o + acc;
    }
    float saturate (float x) noexcept
    {
        if (satIn != 1.0f)
        {
            const auto v = satIn * x;
            float r;
            if (-3.0f > v)
                r = -1.0f;
            else if (v > 3.0f)
                r = 1.0f;
            else
            {
                auto n = v * v;
                n = n + 27.0f;
                n = n * v;
                auto d = v * 9.0f;
                d = d * v;
                d = d + 27.0f;
                r = n / d;
            }
            x = r * satOut;
        }
        const auto a = std::fabs (x);
        const auto diff = a - env;
        env = env + diff * (a >= env ? envUp : envDown);
        if (env > 1.0f)
            x = x / env;
        return x;
    }
    static float pole (OnePole& p, float x) noexcept
    {
        auto y = p.b1 * p.x1;
        y = y + x * p.b0;
        y = y + p.a1 * p.y;
        p.x1 = x;
        p.y = y;
        return y;
    }
    void tick (float& L, float& R, float xa, float xb) noexcept
    {
        const auto fbScale = feedback / std::max (1.0f, satOut * satIn);
        auto mono = L + R;
        mono = mono * 0.5f;
        mono = mono * send;
        modulation();
        const std::uint32_t mask = 0x3FFFF;
        auto readFrac = [&] (float pos) {
            const auto i = (std::uint32_t) (std::int64_t) pos;
            const auto f = pos - (float) (std::int64_t) i;
            auto a = (1.0f - f) * line[(w - i) & mask];
            a = a + f * line[(w - i - 1) & mask];
            return a;
        };
        auto yl = readFrac (tapL[0]) * xa;
        yl = yl + xb * line[(w - (std::uint32_t) (std::int64_t) tapL[1]) & mask];
        auto yr = readFrac (tapR[0]) * xa;
        yr = yr + xb * line[(w - (std::uint32_t) (std::int64_t) tapR[1]) & mask];
        const auto x = yl * fbScale;
        auto h = x * fbHp.b0;
        h = h + fbHp.b1 * fbHp.x1;
        h = h + fbHp.a1 * fbHp.y;
        fbHp.x1 = x;
        fbHp.y = h;
        const auto in = h + mono;
        const auto s = saturate (svf (in));
        const auto old = ap[(apIdx - 65u) & 0x7FFFu];
        const auto c = 0.0f * 0.49900001287460327f;
        ap[apIdx] = s - old * c;
        apIdx = (apIdx + 1) & 0x7FFF;
        auto v = (1.0f - c * c) * old;
        line[w] = v + c * s;
        w = (w + 1) & mask;
        const auto ol = pole (outL, yl), orr = pole (outR, yr);
        auto l = orr * stereoB;
        l = l + ol * stereoA;
        l = l * wet;
        L = l + L * dry;
        auto r = orr * stereoA;
        r = r + ol * stereoB;
        r = r * wet;
        R = r + R * dry;
    }
};

// ------------------------------------------------------------------------------------------------ plate reverb (§2.9)
// FL's plate (Dattorro-style figure-eight tank with the shared plate's line proportions and output taps) with an
// early-reflection line (only its first tap pair is weighted), predelay with feedback, high/low cut, two-band decay and
// damping, room scaling, wet smoothing and stereo width; float throughout, in the engine's order
struct Reverb
{
    static constexpr int lineN[12] { 142, 107, 379, 277, 672, 4453, 1800, 3720, 908, 4217, 2656, 3163 };
    static constexpr int tapN[14] { 266, 2974, 1913, 2496, 1690, 187, 1566, 353, 3627, 1728, 3173, 1811, 335, 621 };
    static constexpr int erLN[8] { 1102, 2904, 3784, 4873, 5878, 7470, 9062, 9263 };
    static constexpr int erRN[8] { 837, 1423, 2865, 3938, 5278, 5978, 7532, 9263 };

    double fs = 44100.0;
    std::vector<float> mem;
    std::array<std::size_t, 14> base {}; // 12 lines, predelay, early reflections
    std::array<int, 12> L {}, P {};
    int preLen = 1, preP = 0, erSize = 1, erP = 0, storedSize = 0;
    std::array<int, 14> taps {};
    std::array<int, 8> erL {}, erR {};
    std::array<float, 8> erGL {}, erGR {};
    // integer controls
    int inLevel = 0, lowCut = 75, highCut = 40, predelayMs = 0, size = 50, diffusion = 100, bassMul = 10, xover = 500, decay = 15,
        damping = 40, dry = 0, er = 0, wet = 0, width = 100;
    bool muteIn = false, muteDry = false, muteEr = false, muteWet = false, dirty = true;
    float preFeedback = 0.0f;
    // coefficients and state
    float gIn = 1.0f, cLc = 0.0f, cHc = 1.0f, cD1 = 0.0f, cD2 = 0.0f, cT1 = 0.0f, cT2 = 0.0f, g = 0.0f, cShelf = 0.0f, cXover = 0.0f, cDamp = 1.0f;
    float gDry = 0.0f, gEr = 0.0f, gWet = 0.0f, wetNow = -1000.0f;
    float sHc = 0.0f, sLcY = 0.0f, sLcX = 0.0f, sAx = 0.0f, sAy = 0.0f, sBx = 0.0f, sBy = 0.0f, sDampA = 0.0f, sDampB = 0.0f;

    Reverb()
    {
        // the constructor's control values (the plugin overwrites all but input level, bass multiplier and crossover)
        setParam (0, bitsF (0x3f5b83f4));
        setParam (1, bitsF (0x3c99e459));
        setParam (2, bitsF (0x3e25ed09));
        setParam (3, 0.0f);
        setParam (5, bitsF (0x3efd6a05));
        setParam (6, 1.0f);
        setParam (7, bitsF (0x3e924925));
        setParam (8, bitsF (0x3e76474b));
        setParam (9, bitsF (0x3d901495));
        setParam (10, bitsF (0x3e25ed09));
        setParam (11, bitsF (0x3f5b83f4));
        setParam (12, bitsF (0x3f463b97));
        setParam (13, bitsF (0x3f128bdc));
        setParam (14, 0.5f);
        setParam (15, 0.0f);
        setParam (16, 0.0f);
        setParam (17, 0.0f);
        setParam (18, 0.0f);
        allocate (44100.0);
    }

    static int law (float v, float a, float b) noexcept
    {
        auto x = v * a;
        x = x + b;
        x = x + 0.5f;
        return (int) std::floor ((double) x);
    }
    void setParam (int id, float v) noexcept
    {
        dirty = true;
        switch (id)
        {
            case 0: { auto x = v * 421.0f; x = x - 361.0f; x = x + 0.5f; inLevel = (int) std::floor ((double) x); break; }
            case 1: lowCut = law (v, 2981.0f, 19.0f); break;
            case 2: highCut = law (v, 216.0f, 5.0f); break;
            case 3: predelayMs = law (v, 250.0f, 0.0f); break;
            case 4: preFeedback = v; break;
            case 5: size = law (v, 99.0f, 1.0f); break;
            case 6: diffusion = law (v, 100.0f, 0.0f); break;
            case 7: bassMul = law (v, 28.0f, 2.0f); break;
            case 8: xover = law (v, 1975.0f, 25.0f); break;
            case 9: decay = law (v, 199.0f, 1.0f); break;
            case 10: damping = law (v, 216.0f, 5.0f); break;
            case 11: { auto x = v * 421.0f; x = x - 361.0f; x = x + 0.5f; dry = (int) std::floor ((double) x); break; }
            case 12: { auto x = v * 421.0f; x = x - 361.0f; x = x + 0.5f; er = (int) std::floor ((double) x); break; }
            case 13: { auto x = v * 421.0f; x = x - 361.0f; x = x + 0.5f; wet = (int) std::floor ((double) x); break; }
            case 14: width = law (v, 200.0f, 0.0f); break;
            case 15: muteIn = (int) std::floor ((double) (v + 0.0f + 0.5f)) != 0; break;
            case 16: muteDry = (int) std::floor ((double) (v + 0.0f + 0.5f)) != 0; break;
            case 17: muteEr = (int) std::floor ((double) (v + 0.0f + 0.5f)) != 0; break;
            case 18: muteWet = (int) std::floor ((double) (v + 0.0f + 0.5f)) != 0; break;
            default: break;
        }
    }

    // (re)allocation at a rate: every line at room 100, zeroed, positions 0
    void allocate (double rate)
    {
        fs = rate;
        const auto f = (int) rate;
        std::size_t total = 0;
        for (int k = 0; k < 12; ++k)
        {
            base[(std::size_t) k] = total;
            L[(std::size_t) k] = (f * lineN[k]) / 20000;
            total += (std::size_t) L[(std::size_t) k];
            P[(std::size_t) k] = 0;
        }
        base[12] = total;
        preLen = (f * 250) / 1000;
        total += (std::size_t) preLen;
        base[13] = total;
        auto u = (unsigned) ((double) f * 13950.0 / 44100.0);
        int bits = 0;
        if ((int) u > 0)
            while (u != 0)
                ++bits, u >>= 1;
        erSize = 1 << (bits + 1);
        total += (std::size_t) erSize;
        mem.assign (total, 0.0f);
        preP = erP = 0;
    }
    // FL's reset: every line filled with a tiny constant, filters cleared, the wet level jumps on the next block
    void reset() noexcept
    {
        std::fill (mem.begin(), mem.end(), bitsF (0x0da24260));
        sHc = sLcY = sLcX = 0.0f;
        sAx = sAy = sBx = sBy = 0.0f;
        sDampA = sDampB = 0.0f;
        wetNow = -1000.0f;
    }

    void coefficients() noexcept
    {
        const auto fsF = (float) fs;
        auto level = [] (int v) { return v < -360 ? 0.0f : powF (10.0f, (float) v / 200.0f); };
        gIn = level (inLevel);
        if (muteIn)
            gIn = 0.0f;
        if ((float) lowCut > 19.0f)
        {
            auto x = fsF * 0.5f;
            x = std::min (x, (float) lowCut);
            x = x * 3.1415927410125732f;
            x = x / fsF;
            const auto t = (float) std::tan ((double) x);
            cLc = (t - 1.0f) / (t + 1.0f);
        }
        else
            cLc = 0.0f;
        auto cutLaw = [&] (int v) {
            if (221.0f > (float) v)
            {
                auto x = (float) v * 100.0f;
                x = x + x;
                x = x * 3.1415927410125732f;
                x = x / fsF;
                const auto c = (float) std::cos ((double) x);
                const auto a = 2.0f - c;
                auto r = a * a;
                r = r - 1.0f;
                const auto sq = std::sqrt (r);
                return 1.0f - (a - sq);
            }
            return 1.0f;
        };
        cHc = cutLaw (highCut);
        const auto fi = (int) fsF;
        int len = (fi * predelayMs) / 1000;
        if (len < 1)
            len = 1;
        preLen = len;
        preP = preP % len;
        const auto R = ((size + 15) * fi) / 115;
        for (int k = 0; k < 12; ++k)
        {
            auto n = (R * lineN[k]) / 20000;
            if (n < 1)
                n = 1;
            L[(std::size_t) k] = n;
            P[(std::size_t) k] = P[(std::size_t) k] % n;
        }
        for (int k = 0; k < 14; ++k)
            taps[(std::size_t) k] = (R * tapN[k]) / 20000;
        const auto E = (float) (((size + 3) * fi) / 103);
        for (int k = 0; k < 8; ++k)
        {
            auto a = (float) erLN[k];
            a = a * 1.5f;
            a = a * E;
            a = a / 44100.0f;
            auto b = (float) erRN[k];
            b = b * 1.5f;
            b = b * E;
            b = b / 44100.0f;
            erL[(std::size_t) k] = (int) a;
            erR[(std::size_t) k] = (int) b;
            float x = 0.0f;
            if (k == 0)
                x = erL[0] < erR[0] ? (float) erL[0] : (float) erR[0];
            auto gl = x * 0.5f;
            erGL[(std::size_t) k] = gl / (float) erL[(std::size_t) k];
            auto gr = x * 0.36000001430511475f;
            erGR[(std::size_t) k] = gr / (float) erR[(std::size_t) k];
        }
        if (size != storedSize)
        {
            storedSize = size;
            for (int k = 0; k < 12; ++k)
                std::fill_n (mem.begin() + (std::ptrdiff_t) base[(std::size_t) k], L[(std::size_t) k], 0.0f);
            std::fill_n (mem.begin() + (std::ptrdiff_t) base[13], erSize, 0.0f);
        }
        const auto diff = (float) diffusion / 100.0f;
        const auto logv = lnF (0.0010000000474974513f);
        auto half = [&] (int a, int b) {
            auto x = (float) (L[(std::size_t) a] + L[(std::size_t) b]);
            x = x * 0.5f;
            return x / fsF;
        };
        if (diff == 0.0f)
            cD1 = cD2 = cT1 = cT2 = 0.0f;
        else
        {
            auto x = half (0, 1) * logv;
            cD1 = expF (x / (diff * 0.07000000029802322f));
            x = logv * half (2, 3);
            cD2 = expF (x / (diff * 0.10999999940395355f));
            x = logv * half (4, 8);
            cT1 = expF (x / (diff * 0.3499999940395355f));
            x = logv * half (6, 10);
            cT2 = expF (x / (diff * 0.5099999904632568f));
        }
        const auto sum = (float) (L[9] + L[7] + L[5] + L[11]);
        const auto d10 = (float) decay / 10.0f;
        auto t = sum / fsF;
        t = t * logv;
        g = powF (expF (t / d10), 0.25f);
        auto b = (float) bassMul * d10;
        b = b / 10.0f;
        const auto ratio = powF (expF (t / b), 0.25f) / g;
        const auto m = std::min (1.0f, ratio);
        auto sh = ratio - 1.0f;
        cShelf = sh * 0.5f;
        auto xo = (float) xover * 3.1415927410125732f;
        xo = xo / fsF;
        const auto tx = (float) std::tan ((double) xo);
        cXover = (tx - m) / (m + tx);
        cDamp = cutLaw (damping);
        gDry = level (dry);
        if (muteDry)
            gDry = 0.0f;
        gEr = level (er) * 0.5f;
        if (muteEr)
            gEr = 0.0f;
        auto w = level (wet) * 0.6000000238418579f;
        gWet = w * 0.5f;
        if (muteWet)
            gWet = 0.0f;
        dirty = false;
    }

    float* line (int k) noexcept { return mem.data() + base[(std::size_t) k]; }
    float allpass (int k, float x, float cRead, float cWrite) noexcept
    {
        auto* b = line (k);
        auto& p = P[(std::size_t) k];
        auto v = b[p] - cRead * x;
        b[p] = cWrite * v + x;
        if (--p < 0)
            p = L[(std::size_t) k] - 1;
        return v;
    }
    float tap (int k, int at) noexcept
    {
        const auto n = L[(std::size_t) k];
        return line (k)[at < n ? at : at - n];
    }

    void process (float* left, float* right, int n) noexcept
    {
        if (dirty)
            coefficients();
        const auto t1 = 1.0f - g;
        auto wA = t1 * 0.6000000238418579f;
        wA = wA + g;
        auto wB = t1 * 0.25f;
        wB = wB + g;
        auto wC = t1 * 0.699999988079071f;
        wC = wC + g;
        auto wD = t1 * 0.20000000298023224f;
        wD = wD + g;
        auto wE = t1 * 0.5f;
        wE = wE + g;
        const auto wdt = (float) width * 0.009999999776482582f;
        float erLs = 0.0f, erRs = 0.0f;
        auto* erb = line (13);
        const auto mask = erSize - 1;
        for (int i = 0; i < n; ++i)
        {
            const auto a = gIn * left[i];
            const auto b = gIn * right[i];
            auto m = b + a;
            m = m * 0.5f;
            m = m + 1.0000000031710769e-30f;
            m = (m - sHc) * cHc + sHc;
            sHc = m;
            auto lc = (m - sLcY) * cLc + sLcX;
            sLcX = m;
            sLcY = lc;
            if (cLc != 0.0f)
            {
                m = (m - lc) * 0.5f;
                m = m + 1.0000000031710769e-30f;
            }
            if (gEr != 0.0f)
            {
                auto rd = [&] (int t) { return erb[(t + erP) & mask]; };
                auto x = rd (erL[0]) * erGL[0];
                x = x + rd (erL[1]) * erGL[1];
                x = x - rd (erL[2]) * erGL[2];
                erLs = rd (erL[3]) * erGL[3] + x;
                erLs = erLs + rd (erL[4]) * erGL[4];
                erLs = erLs + rd (erL[5]) * erGL[5];
                erLs = erLs + rd (erL[6]) * erGL[6];
                erRs = rd (erR[0]) * erGR[0];
                erRs = erRs - rd (erR[1]) * erGR[1];
                erRs = erRs + rd (erR[2]) * erGR[2];
                erRs = erRs + rd (erR[3]) * erGR[3];
                erRs = erRs - rd (erR[4]) * erGR[4];
                erRs = erRs + rd (erR[5]) * erGR[5];
                erRs = erRs + rd (erR[6]) * erGR[6];
                erRs = erRs - rd (erR[7]) * erGR[7];
            }
            const auto v1 = allpass (0, m, cD1, cD1);
            const auto v2 = allpass (1, v1, cD2, cD1);
            erb[erP] = v1;
            if (--erP < 0)
                erP = erSize - 1;
            const auto v3 = allpass (2, v2, cD1, cD1);
            const auto v4 = allpass (3, v3, cD2, cD1);
            auto* pre = line (12);
            auto x = pre[preP];
            pre[preP] = x * preFeedback + v4;
            if (--preP < 0)
                preP = preLen - 1;
            x = x + 9.999999682655225e-21f;
            // tank half A
            auto s = line (11)[P[11]] * g;
            auto sh = (s - sAy) * cXover + sAx;
            sAx = s;
            sAy = sh;
            auto u = (sh + s) * cShelf + s;
            u = u + x;
            u = (u - sDampA) * cDamp + sDampA;
            sDampA = u;
            const auto a1 = allpass (4, u, cT1, cT1);
            auto& p5 = P[5];
            auto y = line (5)[p5];
            line (5)[p5] = a1;
            if (--p5 < 0)
                p5 = L[5] - 1;
            y = y * g;
            const auto a2 = allpass (6, y, cT2, cT2);
            auto& p7 = P[7];
            line (7)[p7] = a2;
            if (--p7 < 0)
                p7 = L[7] - 1;
            // tank half B
            s = line (7)[p7] * g;
            sh = (s - sBy) * cXover + sBx;
            sBy = sh;
            sBx = s;
            u = (sh + s) * cShelf + s;
            u = u + x;
            u = (u - sDampB) * cDamp + sDampB;
            sDampB = u;
            const auto b1 = allpass (8, u, cT1, cT1);
            auto& p9 = P[9];
            y = line (9)[p9];
            line (9)[p9] = b1;
            if (--p9 < 0)
                p9 = L[9] - 1;
            y = y * g;
            const auto b2 = allpass (10, y, cT2, cT2);
            auto& p11 = P[11];
            line (11)[p11] = b2;
            if (--p11 < 0)
                p11 = L[11] - 1;
            // output taps
            auto oL = tap (9, taps[0] + P[9]);
            oL = oL - wB * tap (9, taps[1] + P[9]);
            oL = oL - wC * tap (10, taps[2] + P[10]);
            oL = oL - wD * tap (11, taps[3] + P[11]);
            oL = oL + wA * tap (5, taps[4] + P[5]);
            oL = oL + tap (6, taps[5] + P[6]);
            oL = oL + wE * tap (7, taps[6] + P[7]);
            auto oR = wB * tap (5, taps[8] + P[5]);
            oR = oR + tap (5, taps[7] + P[5]);
            oR = oR - wC * tap (6, taps[9] + P[6]);
            oR = oR - wD * tap (7, taps[10] + P[7]);
            oR = oR - wA * tap (9, taps[11] + P[9]);
            oR = oR + tap (10, taps[12] + P[10]);
            oR = oR + wE * tap (11, taps[13] + P[11]);
            float w;
            if (-99.0f > wetNow)
            {
                wetNow = gWet;
                w = (gWet - wetNow) * 0.0010000000474974513f + wetNow;
            }
            else
                w = (gWet - wetNow) * 0.0010000000474974513f + wetNow;
            wetNow = w;
            auto wl = w * oL;
            wl = wl + gEr * erLs;
            auto wr = w * oR;
            wr = wr + gEr * erRs;
            const auto sum = wr + wl;
            auto d = wl - wr;
            d = d * wdt;
            const auto r2 = sum - d;
            auto l2 = d + sum;
            l2 = l2 + a * gDry;
            auto rr = b * gDry;
            rr = rr + r2;
            left[i] = l2;
            right[i] = rr;
        }
    }
};

// ------------------------------------------------------------------------------------------------ engine "V1" (§2.5)
class Engine
{
public:
    virtual ~Engine() = default;
    virtual void setParam (int i, float v) noexcept = 0;
    virtual float getParam (int i) const noexcept = 0;
    virtual void setRate (double fs) noexcept = 0;
    virtual void noteOn (int key, bool slide, bool accent) noexcept = 0;
    virtual void noteOff() noexcept = 0;
    virtual float tick() noexcept = 0;
    virtual bool silent() const noexcept = 0;
    float transpose = 0.0f, pitchExpr = 0.0f, cutExpr = 0.0f, resExpr = 0.0f;
    double bend = 0.0;
};

class Engine1 final : public Engine
{
public:
    explicit Engine1 (std::shared_ptr<const DllData> d) : dll (std::move (d))
    {
        for (int i = 0; i < 128; ++i)
            noteTable[(std::size_t) i] = (double) i + 0.0;
        selectRecorded (0x1B4);
        setRate (44100.0);
    }

    // §2.5: one stored raw value per index; laws for the used indices
    void setParam (int i, float v) noexcept override
    {
        if (i >= 0 && i < 256)
            raw[(std::size_t) i] = v;
        switch (i)
        {
            case 2: cutoff = powF (v, 1.5f) * 0.5799999833106995f + 0.10000000149011612f; return;
            case 5:
            {
                const auto sq = powF (v, 2.0f);
                envMod = sq * 0.5400000214576721f + 0.1599999964237213f;
                envModInv = 1.0f - sq * 0.3100000023841858f;
                return;
            }
            case 6: p[6] = v; updateDecay (minDecay); return;
            case 7: accent = v * v; return;
            case 8: volume = v * 0.25f * 3.0f; return;
            case 9:
                p[9] = v;
                hp.highPass = 1;
                hp.freq = v * 390.0f + 10.0f;
                hpFreqParam = hp.freq;
                hp.coefficients ((float) std::tan ((double) (hp.freq * hp.piOverFs)));
                return;
            case 10:
                minDecay = (double) (v * 0.1850000023841858f + 0.1850000023841858f);
                updateDecay (minDecay);
                return;
            case 11: pw = v * 0.44999998807907104f + 0.5f; return;
            case 12:
                p[12] = v;
                lfoInc = (float) ((double) (v * 9.899999618530273f + 0.10000000149011612f) * invFs);
                return;
            case 13: lfoAmount = v * 0.23999999463558197f; return;
            case 0x1B: distOn = v >= 0.5f; return;
            case 0x1C: p[0x1C] = v; dist.setHp (v); return;
            case 0x1D: dist.setDrive (v * v); return;
            case 0x1E: dist.setTone (v); return;
            case 0x1F: dist.setVolume (v); return;
            case 0x28:
            {
                const auto s = (double) (v * v);
                vcaRelease = s * 0.045 + 0.005;
                vcaAttack = s * 0.01 + 0.005;
                vcaAttackCoef = 1.0 - powD (0.1, 1.0 / (vcaAttack * fsOs));
                vcaReleaseCoef = 1.0 - powD (0.1, 1.0 / (vcaRelease * fsOs));
                return;
            }
            case 0x29: pulseMode = (int) ((double) (2 - 1) * (double) v + 0.5); return;
            default:
                if (i >= 0 && i < 256)
                    p[(std::size_t) i] = v;
                return;
        }
    }
    float getParam (int i) const noexcept override { return i >= 0 && i < 256 ? raw[(std::size_t) i] : 0.0f; }

    void setRate (double fs) noexcept override
    {
        fsBase = fs;
        invFs = 1.0 / fs;
        fsOs = (double) oversampling * fs;
        invFsOs = 1.0 / fsOs;
        slideCoef[0] = 1.0 - powD (0.1, 1.0 / (fs * glideTime[0]));
        slideCoef[1] = 1.0 - powD (0.1, 1.0 / (fs * glideTime[1]));
        vcaAttackCoef = 1.0 - powD (0.1, 1.0 / (fsOs * vcaAttack));
        vcaReleaseCoef = 1.0 - powD (0.1, 1.0 / (fsOs * vcaRelease));
        hp.piOverFs = (float) (1.0 / fsOs) * 3.1415927410125732f;
        hp.freq = hpFreqParam;
        hp.highPass = 1;
        hp.coefficients ((float) std::tan ((double) (hp.freq * hp.piOverFs)));
        ladder.setRate (fsOs);
        dist.setRate (fsOs);
        // saw-table selection (§2.4)
        const auto& st = sawTables();
        oscInvFs = 1.0 / fsOs;
        const auto x = fsOs * 0.9 / ((double) st.length[0] / 16.0) / 8.17579891564;
        oscIndexOffset = -std::log (x) / std::log (2.0) / 12.0;
        recInvFs = 1.0 / fsOs;
        accentCoef[0] = 1.0 - powD (0.1, 1.0 / (fs * 0.05));
        accentCoef[1] = 1.0 - powD (0.1, 1.0 / (fs * 0.2));
        sustainCoef = powD (0.1, 1.0 / (fsOs * 1.0));
    }

    void noteOn (int key, bool slide, bool acc) noexcept override
    {
        gate = true;
        key = std::clamp (key, 0, 127);
        accentOn = acc;
        if (! slide)
        {
            env = 1.0;
            sustain = 1.0;
        }
        slideIndex = slide ? 1 : 0;
        target = noteTable[(std::size_t) key];
    }
    void noteOff() noexcept override { gate = false; }

    float tick() noexcept override
    {
        const auto s = sinF (lfoPhase * 6.2831854820251465f);
        lfoPhase = lfoPhase + lfoInc;
        const auto t0 = (double) p[0] * 24.0;
        lfoPhase = lfoPhase - (float) (int) lfoPhase;
        pitch = (target - pitch) * slideCoef[(std::size_t) slideIndex] + pitch;
        auto x = pitch + bend;
        x = x - 64.0;
        x = x + 24.0;
        x = x + 4.0;
        x = x + t0;
        x = x + (double) pitchExpr;
        x = x + (double) transpose;
        double xn;
        if (0.0 > x)
            xn = 0.0;
        else
            xn = std::min (144.0, x);
        xn = xn / 144.0;
        const auto ec = powD (0.1, 1.0 / (fsBase * (accentOn ? minDecay : decay)));
        env = ec * env;
        sustain = sustainCoef * sustain;
        const auto r = 1.0f - (1.0f - p[4]) * 0.5f;
        const auto rInv = 1.0f - r;
        double acc = 0.0;
        if (accentOn)
            acc = (double) accent * env * (double) r;
        auto pwNow = s * lfoAmount + pw;
        const auto accF = (float) acc;
        const auto accD = (double) accF;
        auto boost = accF * rInv * 6.0f;
        accentSmooth = (accD - accentSmooth) * accentCoef[accD > accentSmooth ? 0 : 1] + accentSmooth;
        auto boostF = (float) ((double) boost + accentSmooth);
        if (0.0010000000474974513f > pwNow)
            pwNow = bitsF (0x3a83126f);
        else if (pwNow > 0.9990000128746033f)
            pwNow = bitsF (0x3f7fbe77);
        pulseWidth = pwNow;
        if (pulseMode > 0)
        {
            recFreq = (double) powF (2.0f, (float) (xn * 12.0)) * 8.17579891564;
            recInc = recFreq * recInvFs;
        }
        oscSelect (xn);
        // cutoff (§2.5)
        const auto c1 = envModInv * cutoff;
        boostF = boostF * 0.23000000417232513f;
        auto c = (double) c1 + (double) envMod * env + (double) boostF;
        auto cf = (float) c - 0.019999999552965164f;
        auto cd = (double) cf + (double) (p[3] - 0.5f) * xn;
        cf = (float) cd + cutExpr;
        const auto hz = powD (2.0, (double) cf * 128.0 * 0.08333333333333333) * 8.1757989156;
        auto res = resExpr + p[4];
        float rr;
        if (0.0f > res)
            rr = 0.0f;
        else
            rr = std::min (1.0f, res);
        ladder.set ((float) hz, rr, -1.0f);

        std::array<float, 4> buf {};
        for (int n = 0; n < oversampling; ++n)
        {
            float o;
            const auto len = oscLen;
            auto ph = oscPhase;
            const auto pos = len * ph;
            if (pulseMode > 0)
            {
                const auto nextPh = ph + oscInc;
                const auto i = (int) pos;
                const auto f1 = (float) (pos - (double) i);
                const auto wrap = (int) nextPh;
                oscPhase = nextPh - (double) wrap;
                oscCycles += wrap;
                auto saw = (1.0f - f1) * oscTab[(std::size_t) i + 1];
                saw = saw + f1 * oscTab[(std::size_t) i + 2];
                const auto rp = recPhase * recLen;
                recPhase = recPhase + recInc;
                const auto j = (int) rp;
                const auto g = (float) (rp - (double) j);
                auto rec = g * recTab[(std::size_t) j + 1];
                rec = rec + (1.0f - g) * recTab[(std::size_t) j];
                const auto k = (int) recPhase;
                recPhase = recPhase - (double) k;
                if (k != 0)
                    selectRecorded ((int) (192000.0 / recFreq));
                o = (1.0f - p[1]) * rec;
                o = o + p[1] * saw;
            }
            else
            {
                auto pos2 = (double) pulseWidth + ph;
                const auto nextPh = ph + oscInc;
                const auto i = (int) pos;
                const auto j = (int) pos2;
                pos2 = (pos2 - (double) j) * len;
                const auto f1 = (float) (pos - (double) i);
                const auto k = (int) pos2;
                const auto f2 = (float) (pos2 - (double) k);
                auto saw1 = (1.0f - f1) * oscTab[(std::size_t) i + 1];
                saw1 = saw1 + f1 * oscTab[(std::size_t) i + 2];
                auto saw2 = (1.0f - f2) * oscTab[(std::size_t) k + 1];
                saw2 = saw2 + f2 * oscTab[(std::size_t) k + 2];
                const auto wrap = (int) nextPh;
                oscCycles += wrap;
                oscPhase = nextPh - (double) wrap;
                o = (saw1 - saw2) * (1.0f - p[1]);
                o = o + saw1 * p[1];
            }
            auto y = ladder.tick (o);
            // VCA (§2.5)
            double v;
            if (gate)
            {
                float tg = 0.0f;
                if (accentOn)
                {
                    const auto a = (float) ((accentPeak - accentBase) * (double) accent + accentBase);
                    tg = (float) ((double) a * env + accentBase);
                }
                if (sustain > (double) tg)
                    tg = (float) sustain;
                v = ((double) tg - vca) * vcaAttackCoef;
            }
            else
                v = (0.0 - vca) * vcaReleaseCoef;
            vca = v + vca;
            const auto z = (float) ((double) y * vca);
            auto h = z * hp.b0;
            h = h + hp.x1 * hp.b1;
            h = h + hp.a1 * hp.y;
            hp.x1 = z;
            hp.y = h;
            auto out = h * volume;
            if (distOn)
                out = distortion (out);
            buf[(std::size_t) n] = out;
        }
        if (oversampling == 1)
            return buf[0];
        if (oversampling == 2)
        {
            dec1.push ((double) buf[0]);
            return (float) dec1.push ((double) buf[1]);
        }
        if (oversampling == 4)
        {
            dec1.push ((double) buf[0]);
            const auto a = (float) dec1.push ((double) buf[1]);
            dec1.push ((double) buf[2]);
            const auto b = (float) dec1.push ((double) buf[3]);
            dec2.push ((double) a);
            return (float) dec2.push ((double) b);
        }
        return 0.0f;
    }

    bool silent() const noexcept override { return ! gate && std::fabs (vca) < 1e-9; }

private:
    void updateDecay (double minD) noexcept
    {
        decay = (double) powF (p[6], 2.0f) * (decayMax - minD) + minD;
    }

    void oscSelect (double xn) noexcept
    {
        const auto& st = sawTables();
        const auto x0 = (float) xn;
        const auto xd = (double) x0;
        const auto a = (float) (xd * 12.0);
        const auto pw2 = powF (2.0f, a);
        const auto t = (float) ((double) pw2 * 8.17579891564);
        const auto inc = (float) ((double) t * oscInvFs);
        oscInc = (double) inc;
        auto idx = (int) ((xd + oscIndexOffset) * (double) 48);
        idx = idx < 0 ? 0 : std::min (idx, 47);
        oscLen = (double) st.length[(std::size_t) idx];
        oscTab = st.data[(std::size_t) idx].data();
    }

    void selectRecorded (int thr) noexcept
    {
        if (dll == nullptr || dll->recorded.empty())
            return;
        int found = 0;
        for (int i = (int) dll->recorded.size() - 1; i >= 0; --i)
            if (thr < dll->recorded[(std::size_t) i].length)
            {
                found = i;
                break;
            }
        recLen = (double) dll->recorded[(std::size_t) found].length;
        recTab = dll->recorded[(std::size_t) found].data.data();
    }

    float distortion (float x) noexcept { return dist.tick (x); }

    std::shared_ptr<const DllData> dll;
    std::array<float, 256> raw {}, p {};
    std::array<double, 128> noteTable {};
    int oversampling = 4;
    double fsBase = 44100.0, invFs = 1.0 / 44100.0, fsOs = 176400.0, invFsOs = 1.0 / 176400.0;
    // envelope, accent
    double minDecay = 0.185, decayMax = 2.16, decay = 0.185, env = 0.0, sustain = 0.0, sustainCoef = 0.0;
    double accentSmooth = 0.0;
    std::array<double, 2> accentCoef {};
    float accent = 0.0f, envMod = 0.0f, envModInv = 0.0f, cutoff = 0.0f, volume = 0.0f, pw = 0.5f, lfoAmount = 0.0f;
    float lfoPhase = 0.0f, lfoInc = 0.0f;
    bool gate = false, accentOn = false, distOn = false;
    // pitch
    std::array<double, 2> glideTime { 0.0001, 0.05 }, slideCoef {};
    int slideIndex = 0;
    double pitch = 0.0, target = 0.0;
    // oscillators
    int pulseMode = 0, oscCycles = 0;
    float pulseWidth = 0.5f;
    double oscPhase = 0.0, oscInc = 0.0, oscLen = 0.0, oscInvFs = 0.0, oscIndexOffset = 0.0;
    const float* oscTab = nullptr;
    double recPhase = 0.0, recInc = 0.0, recLen = 0.0, recFreq = 440.0, recInvFs = 0.0;
    const float* recTab = nullptr;
    // VCA, filters
    double vca = 0.0, vcaAttack = 0.005, vcaRelease = 0.005, vcaAttackCoef = 1.0, vcaReleaseCoef = 1.0;
    double accentBase = 0.7400000095367432, accentPeak = 2.5999999046325684;
    Ladder ladder;
    OnePole hp;
    float hpFreqParam = 200.0f;
    HalfBand dec1, dec2;
    Distortion dist;
};

// ------------------------------------------------------------------------------------------------ engine "V2" (§2.6)
// the second engine: the same oscillators (its own recorded "303 Pulse" set), ladder, high-pass, distortion and
// decimators, with a modelled accent circuit (two RC paths) driving the cutoff, three envelopes (VCA, accent, accent
// release) and table-based cutoff / resonance / envelope-modulation laws ("TB303 range"; "extended" scales them
// linearly)
class Engine2 final : public Engine
{
public:
    Engine2 (std::shared_ptr<const DllData> d, bool extended) : dll (std::move (d)), ext (extended)
    {
        for (int i = 0; i < 128; ++i)
            noteTable[(std::size_t) i] = (double) i + 0.0;
        selectRecorded (100);
        env2.releaseTime = 0.2, env2.releaseTarget = 0.15, env2.decayTime = 0.007;
        env3.releaseTime = 0.185, env3.releaseTarget = 0.1;
        setRate (44100.0);
    }

    void setParam (int i, float v) noexcept override
    {
        if (i >= 0 && i < 256)
            raw[(std::size_t) i] = v;
        switch (i)
        {
            case 2:
            {
                if (ext)
                    cutoff = v * 0.699999988079071f;
                else if (1.0f <= v)
                    cutoff = 0.6299999952316284f;
                else
                    cutoff = interp4 (cutTable, v);
                return;
            }
            case 4:
            {
                static constexpr float t[6] = { 0.0f, 0.10300000011920929f, 0.38999998569488525f, 0.5910000205039978f, 1.0019999742507935f, 0.0f };
                res = 1.0f <= v ? 1.0019999742507935f : interp4 (t, v);
                return;
            }
            case 5:
            {
                static constexpr float t[6] = { 0.091796875f, 0.12800000607967377f, 0.169097900390625f, 0.2709999978542328f, 0.550000011920929f, 0.0f };
                if (ext)
                {
                    auto x = v * 0.699999988079071f;
                    envMod = x + 0.091796875f;
                }
                else
                {
                    const auto r = raw[5];
                    envMod = 1.0f <= r ? 0.550000011920929f : interp4 (t, r);
                }
                return;
            }
            case 6:
                p[6] = v;
                decayCoef = powD (0.1, 1.0 / (decayTime (p[6]) * fsBase));
                return;
            case 8: volume = v * 0.25f * 3.0f; return;
            case 9:
                p[9] = v;
                hp.highPass = 1;
                hp.freq = v * 390.0f + 10.0f;
                hpFreqParam = hp.freq;
                hp.coefficients ((float) std::tan ((double) (hp.freq * hp.piOverFs)));
                return;
            case 10:
            {
                auto x = v - 0.5f;
                x = x * 0.10000000149011612f;
                minDecay = (double) (x + 0.1850000023841858f);
                const auto q = p[6];
                auto f = (float) (minDecay / (double) 0.1850000023841858f);
                f = f * (1.0f - q);
                f = f + q;
                float t;
                if (1.0f <= q)
                    t = 2.2249999046325684f;
                else
                {
                    auto y = q * 10.0f;
                    const auto k = (int) y;
                    y = y - (float) k;
                    auto a = (1.0f - y) * decayTable[k];
                    t = a + y * decayTable[k + 1];
                }
                t = t * f;
                decayCoef = powD (0.1, 1.0 / ((double) t * fsBase));
                accentDecayCoef = powD (0.1, 1.0 / (decayTime (0.0f) * fsBase));
                return;
            }
            case 11: pw = v * 0.44999998807907104f + 0.5f; return;
            case 12:
                p[12] = v;
                lfoInc = (float) ((double) (v * 9.899999618530273f + 0.10000000149011612f) * invFs);
                return;
            case 13: lfoAmount = v * 0.23999999463558197f; return;
            case 0x1B: distOn = v >= 0.5f; return;
            case 0x1C: p[0x1C] = v; dist.setHp (v); return;
            case 0x1D: dist.setDrive (v * v); return;
            case 0x1E: dist.setTone (v); return;
            case 0x1F: dist.setVolume (v); return;
            case 0x28: vcaSmoothCoef = 1.0 - powD (0.09999999999999998, 1.0 / ((smoothLaw() * 0.05 + 0.0001) * fsBase)); return;
            case 0x29: pulse = 0.5f <= v; return;
            default:
                if (i >= 0 && i < 256)
                    p[(std::size_t) i] = v;
                return;
        }
    }
    float getParam (int i) const noexcept override { return i >= 0 && i < 256 ? raw[(std::size_t) i] : 0.0f; }

    void setRate (double fs) noexcept override
    {
        fsBase = fs;
        fsOs = (double) oversampling * fs;
        invFs = 1.0 / fs;
        invFsOs = 1.0 / fsOs;
        slideCoef[0] = 1.0 - powD (0.09999999999999998, 1.0 / (fs * glideTime[0]));
        slideCoef[1] = 1.0 - powD (0.09999999999999998, 1.0 / (fs * glideTime[1]));
        for (auto* e : { &env1, &env2, &env3 })
        {
            e->fs = fs;
            e->attackInc = 1.0 / (fs * e->attackTime);
            e->decayInc = 1.0 / (fs * e->decayTime);
            e->releaseCoef = powD (e->releaseTarget, 1.0 / (fs * e->releaseTime));
        }
        vcaSmoothCoef = 1.0 - powD (0.09999999999999998, 1.0 / ((smoothLaw() * 0.05 + 0.0001) * fs));
        hp.piOverFs = (float) (1.0 / fsOs) * 3.1415927410125732f;
        hp.freq = hpFreqParam;
        hp.highPass = 1;
        hp.coefficients ((float) std::tan ((double) (hp.freq * hp.piOverFs)));
        ladder.setRate (fsOs);
        const auto& st = sawTables();
        oscInvFs = 1.0 / fsOs;
        const auto x = fsOs * 0.9 / ((double) st.length[0] / 16.0) / 8.17579891564;
        oscIndexOffset = -std::log (x) / std::log (2.0) / 12.0;
        recInvFs = 1.0 / fsOs;
        circuit.fs = fs;
        circuit.dirty = true;
        decayCoef = accentDecayCoef = powD (0.1, 1.0 / (decayTime (0.0f) * fs));
        vcaUp = 1.0 - powD (0.09999999999999998, 1.0 / (fs * 0.0003));
        vcaDown = 1.0 - powD (0.09999999999999998, 1.0 / (fs * 0.01));
        dist.setRate (fsOs);
    }

    void noteOn (int key, bool slide, bool acc) noexcept override
    {
        accentOn = acc;
        gate = true;
        key = std::clamp (key, 0, 127);
        if (! slide)
        {
            env1.restart();
            env = 1.0;
            if (accentOn)
            {
                env2.restart();
                env3.restart();
            }
        }
        slideIndex = slide ? 1 : 0;
        target = noteTable[(std::size_t) key];
    }
    void noteOff() noexcept override
    {
        gate = false;
        env1.stage = 3;
        env2.stage = 3;
    }

    float tick() noexcept override
    {
        const auto s = sinF (lfoPhase * 6.2831854820251465f);
        lfoPhase = lfoPhase + lfoInc;
        const auto t0 = (double) p[0] * 24.0;
        lfoPhase = lfoPhase - (float) (int) lfoPhase;
        pitch = (target - pitch) * slideCoef[(std::size_t) slideIndex] + pitch;
        auto x = pitch + bend;
        x = x - 64.0;
        x = x + 24.0;
        x = x + 4.0;
        x = x + t0;
        x = x + (double) pitchExpr;
        x = x + (double) transpose;
        double xn;
        if (0.0 > x)
            xn = 0.0;
        else
            xn = std::min (144.0, x);
        xn = xn / 144.0;
        double accIn = 0.0;
        if (accentOn)
        {
            env = accentDecayCoef * env;
            envCopy = env;
            accIn = env * (double) raw[7];
        }
        else
        {
            env = decayCoef * env;
            envCopy = env;
        }
        const auto drive = (float) ((double) (float) accIn * 10.0);
        const auto c = circuit.tick ((double) raw[4], (double) raw[7], drive);
        auto pwNow = s * lfoAmount + pw;
        auto boost = (float) ((((double) c - 3.25) * 0.1) * 1.33);
        if (0.0010000000474974513f > pwNow)
            pwNow = bitsF (0x3a83126f);
        else if (pwNow > 0.9990000128746033f)
            pwNow = bitsF (0x3f7fbe77);
        pulseWidth = pwNow;
        if (pulse)
        {
            recFreq = (double) powF (2.0f, (float) (xn * 12.0)) * 8.17579891564;
            recInc = recFreq * recInvFs;
        }
        oscSelect (xn);
        const auto em = (float) ((double) envMod * (envCopy - 0.32499998807907104));
        boost = boost * 0.75f;
        auto cf = em + boost;
        cf = cf + cutoff;
        cf = cf + 0.10000000149011612f;
        auto cd = (double) cf + (double) (p[3] - 0.5f) * xn;
        cf = (float) cd + cutExpr;
        const auto hz = powD (2.0, (double) cf * 128.0 * 0.08333333333333333) * 8.1757989156;
        ladder.set ((float) hz, res, -1.0f);
        const auto e1 = env1.tick();
        auto e2 = env2.tick();
        e2 = e2 * 3.0f;
        e2 = e2 * raw[7];
        auto e3 = env3.tick();
        e3 = e3 * 1.75f;
        e3 = e3 * raw[7];
        auto g = std::min (1.0f, e3);
        if (gate)
            g = 0.0f;
        const auto m = std::max (e1, e2);
        const auto y = std::max (g, m);
        const auto yd = (double) y;
        const auto d = yd - vca1;
        vca1 = d * (yd > vca1 ? vcaUp : vcaDown) + vca1;
        vca2 = (vca1 - vca2) * vcaSmoothCoef + vca2;

        std::array<float, 4> buf {};
        for (int n = 0; n < oversampling; ++n)
        {
            float o;
            const auto len = oscLen;
            const auto ph = oscPhase;
            const auto pos = len * ph;
            if (pulse)
            {
                const auto nextPh = ph + oscInc;
                const auto i = (int) pos;
                const auto f1 = (float) (pos - (double) i);
                const auto wrap = (int) nextPh;
                oscPhase = nextPh - (double) wrap;
                oscCycles += wrap;
                auto saw = (1.0f - f1) * oscTab[(std::size_t) i + 1];
                saw = saw + f1 * oscTab[(std::size_t) i + 2];
                const auto rp = recPhase * recLen;
                recPhase = recPhase + recInc;
                const auto j = (int) rp;
                const auto gg = (float) (rp - (double) j);
                auto rec = gg * recTab[(std::size_t) j + 1];
                rec = rec + (1.0f - gg) * recTab[(std::size_t) j];
                const auto k = (int) recPhase;
                recPhase = recPhase - (double) k;
                if (k != 0)
                    selectRecorded ((int) (44100.0 / recFreq));
                o = (1.0f - p[1]) * rec;
                o = o + p[1] * saw;
            }
            else
            {
                auto pos2 = (double) pulseWidth + ph;
                const auto nextPh = ph + oscInc;
                const auto i = (int) pos;
                const auto j = (int) pos2;
                pos2 = (pos2 - (double) j) * len;
                const auto f1 = (float) (pos - (double) i);
                const auto k = (int) pos2;
                const auto f2 = (float) (pos2 - (double) k);
                auto saw1 = (1.0f - f1) * oscTab[(std::size_t) i + 1];
                saw1 = saw1 + f1 * oscTab[(std::size_t) i + 2];
                auto saw2 = (1.0f - f2) * oscTab[(std::size_t) k + 1];
                saw2 = saw2 + f2 * oscTab[(std::size_t) k + 2];
                const auto wrap = (int) nextPh;
                oscCycles += wrap;
                oscPhase = nextPh - (double) wrap;
                o = (saw1 - saw2) * (1.0f - p[1]);
                o = o + saw1 * p[1];
            }
            const auto f = ladder.tick (-o);
            const auto z = (float) ((double) f * vca2);
            auto h = z * hp.b0;
            h = h + hp.x1 * hp.b1;
            h = h + hp.y * hp.a1;
            hp.x1 = z;
            hp.y = h;
            auto out = h * volume;
            if (distOn)
                out = dist.tick (out);
            buf[(std::size_t) n] = out;
        }
        if (oversampling == 1)
            return buf[0];
        if (oversampling == 2)
        {
            dec1.push ((double) buf[0]);
            return (float) dec1.push ((double) buf[1]);
        }
        if (oversampling == 4)
        {
            dec1.push ((double) buf[0]);
            const auto a = (float) dec1.push ((double) buf[1]);
            dec1.push ((double) buf[2]);
            const auto b = (float) dec1.push ((double) buf[3]);
            dec2.push ((double) a);
            return (float) dec2.push ((double) b);
        }
        return 0.0f;
    }

    bool silent() const noexcept override { return ! gate && std::fabs (vca2) < 1e-9 && std::fabs (vca1) < 1e-9; }

private:
    static constexpr float decayTable[12] = { 0.1850000023841858f, 0.20399999618530273f, 0.2329999953508377f, 0.3540000021457672f, 0.45500001311302185f, 0.5379999876022339f,
                                              0.652999997138977f, 0.9409999847412109f, 1.4299999475479126f, 2.121999979019165f, 2.2249999046325684f, 0.0f };
    static constexpr float cutTable[6] = { 0.3330000042915344f, 0.3709999918937683f, 0.44699999690055847f, 0.5329999923706055f, 0.6299999952316284f, 0.0f };

    static float interp4 (const float* t, float v) noexcept
    {
        auto x = v * 4.0f;
        const auto i = (int) x;
        x = x - (float) i;
        auto a = (1.0f - x) * t[i];
        return a + x * t[i + 1];
    }
    double smoothLaw() const noexcept
    {
        const auto v = raw[0x28];
        auto c = v * v;
        c = c * v;
        return (double) c;
    }
    // decay time law (seconds): the table at 10·x scaled by the minimum-decay ratio
    double decayTime (float q) const noexcept
    {
        auto f = (float) (minDecay / (double) 0.1850000023841858f);
        f = f * (1.0f - q);
        f = f + q;
        if (1.0f <= q)
            return (double) (2.2249999046325684f * f);
        auto y = q * 10.0f;
        const auto k = (int) y;
        y = y - (float) k;
        auto a = (1.0f - y) * decayTable[k];
        a = a + y * decayTable[k + 1];
        return (double) (a * f);
    }

    struct Env
    {
        double attackTime = 0.00077, level = 0.5, attackInc = 0.0, phase = 0.0, decayTime = 0.01, decayPhase = 0.0, decayInc = 0.0;
        double releaseTime = 3.2, releaseTarget = 0.1, out = 0.0, releaseCoef = 0.0, fs = 44100.0;
        int stage = 2;
        void restart() noexcept { phase = 0.0, decayPhase = 0.0, stage = 0; }
        float tick() noexcept
        {
            if (stage == 0)
            {
                auto x = attackInc + phase;
                phase = x;
                if (x > 1.0)
                {
                    stage = 1;
                    phase = 1.0;
                    x = 1.0;
                }
                out = x * x * x * level;
                return (float) out;
            }
            if (stage == 1)
            {
                auto y = decayInc + decayPhase;
                decayPhase = y;
                if (y > 1.0)
                {
                    stage = 2;
                    decayPhase = 1.0;
                    y = 1.0;
                }
                const auto f = (float) (1.0 - y);
                auto c = f * f;
                c = c * f;
                out = (double) (1.0f - c) * (1.0 - level) + level;
                return (float) out;
            }
            if (stage == 2)
            {
                out = releaseCoef * out;
                return (float) out;
            }
            return 0.0f;
        }
    };

    // the accent circuit: two RC paths whose time constants follow resonance and accent (§2.6)
    struct Circuit
    {
        double ratio = 0.0, gUp = 0.0, gOut = 0.0, gDown = 0.0, cachedRes = 0.0, cachedAcc = 0.0, cA = 0.0, cB = 0.0, fs = 44100.0;
        double out = 0.0, s1 = 3.25, s2 = 3.25;
        bool dirty = true;
        float tick (double r, double a, float drive) noexcept
        {
            if (dirty || r != cachedRes || a != cachedAcc)
            {
                dirty = false;
                cachedRes = r;
                cachedAcc = a;
                const auto x1 = a * 50000.0;
                const auto ra = (1.0 - a) * 50000.0;
                const auto rr = (1.0 - r) * 50000.0;
                const auto r5 = r * 50000.0;
                auto par = ra * x1;
                par = par / (ra + x1);
                par = par + 47000.0;
                par = par + r5;
                auto t = 1.0 / par + 1e-05;
                auto rtot = 1.0 / t + rr;
                const auto rb = rr + 100000.0;
                cA = 1.0 - powD (0.37, 1.0 / (rtot * 1e-06 * fs));
                cB = 1.0 - powD (0.37, 1.0 / (rb * 1e-06 * fs));
                par = par + 100000.0;
                gUp = 100000.0 / par;
                gOut = rr / rtot;
                gDown = 100000.0 / rb;
                ratio = gDown;
            }
            auto in = (double) drive * a;
            in = in - 0.43;
            const auto thr = (s2 - s1) * ratio + s1;
            double o;
            if (in > thr)
            {
                auto t = (in - s1) * gUp + s1;
                s2 = (t - s2) * cA + s2;
                o = (t - s2) * gOut + s2;
            }
            else
            {
                s2 = (s1 - s2) * cB + s2;
                o = (s2 - s1) * gDown + s1;
            }
            out = o;
            return (float) o;
        }
    };

    void oscSelect (double xn) noexcept
    {
        const auto& st = sawTables();
        const auto x0 = (float) xn;
        const auto xd = (double) x0;
        const auto a = (float) (xd * 12.0);
        const auto pw2 = powF (2.0f, a);
        const auto t = (float) ((double) pw2 * 8.17579891564);
        const auto inc = (float) ((double) t * oscInvFs);
        oscInc = (double) inc;
        auto idx = (int) ((xd + oscIndexOffset) * (double) 48);
        idx = idx < 0 ? 0 : std::min (idx, 47);
        oscLen = (double) st.length[(std::size_t) idx];
        oscTab = st.data[(std::size_t) idx].data();
    }

    void selectRecorded (int thr) noexcept
    {
        if (dll == nullptr || dll->recorded2.empty())
            return;
        int found = 0;
        for (int i = (int) dll->recorded2.size() - 1; i >= 0; --i)
            if (thr < dll->recorded2[(std::size_t) i].length)
            {
                found = i;
                break;
            }
        recLen = (double) dll->recorded2[(std::size_t) found].length;
        recTab = dll->recorded2[(std::size_t) found].data.data();
    }

    std::shared_ptr<const DllData> dll;
    bool ext = true;
    std::array<float, 256> raw {}, p {};
    std::array<double, 128> noteTable {};
    int oversampling = 4;
    double fsBase = 44100.0, invFs = 1.0 / 44100.0, fsOs = 176400.0, invFsOs = 1.0 / 176400.0;
    double minDecay = 0.185, decayCoef = 0.0, accentDecayCoef = 0.0, env = 0.0, envCopy = 0.0;
    float cutoff = 0.0f, res = 0.0f, envMod = 0.0f, volume = 0.0f, pw = 0.5f, lfoAmount = 0.0f, lfoPhase = 0.0f, lfoInc = 0.0f;
    bool gate = false, accentOn = false, distOn = false, pulse = false;
    std::array<double, 2> glideTime { 0.0001, 0.05 }, slideCoef {};
    int slideIndex = 0, oscCycles = 0;
    double pitch = 0.0, target = 0.0;
    float pulseWidth = 0.5f;
    double oscPhase = 0.0, oscInc = 0.0, oscLen = 0.0, oscInvFs = 0.0, oscIndexOffset = 0.0;
    const float* oscTab = nullptr;
    double recPhase = 0.0, recInc = 0.0, recLen = 0.0, recFreq = 440.0, recInvFs = 0.0;
    const float* recTab = nullptr;
    double vca1 = 0.0, vca2 = 0.0, vcaUp = 0.0, vcaDown = 0.0, vcaSmoothCoef = 0.0;
    Env env1, env2, env3;
    Circuit circuit;
    Ladder ladder;
    OnePole hp;
    float hpFreqParam = 200.0f;
    HalfBand dec1, dec2;
    Distortion dist;
};

// ------------------------------------------------------------------------------------------------------ state (§2.2)
struct Step
{
    bool slide = false, gate = false, accent = false;
    int note = 60;
    double length = 0.5;
    float velocity = 0.5f;
};

struct Sequence
{
    std::array<Step, 32> steps {};
    int length = 16;
    double bpm = 125.0, shuffle = 0.0, gateLength = 0.66;
    bool internalBpm = false;
};

struct Program
{
    int patch = 0, sequence = 0, transpose = 0;
};

struct State
{
    bool usable = false;
    int version = 0, program = 0, sequencerMode = 0, engine = 0, oversampling = 4;
    float seqTranspose = 0.0f;
    std::array<Program, 128> programs {};
    std::vector<std::array<float, 100>> patches; // the working copies (the second of each stored pair)
    std::vector<Sequence> sequences;
};

struct Reader
{
    const std::vector<std::uint8_t>& b;
    std::size_t at;
    bool ok = true;
    template <typename T> T get()
    {
        T v {};
        if (at + sizeof (T) > b.size())
        {
            ok = false;
            return v;
        }
        std::memcpy (&v, b.data() + at, sizeof (T));
        at += sizeof (T);
        return v;
    }
    void skipString()
    {
        const auto n = get<std::int32_t>();
        if (n < 0 || at + (std::size_t) n > b.size())
            ok = false;
        else
            at += (std::size_t) n;
    }
};

State parseState (const std::vector<std::uint8_t>& s)
{
    State st;
    Reader r { s, 0 };
    const auto wrapperVersion = r.get<std::uint32_t>(); // the presets carry 1, FL 2026 saves 2; the wrapper refuses > 2
    if (wrapperVersion < 1 || wrapperVersion > 2)
        return st;
    std::size_t engineAt = 0, engineEnd = 0;
    while (r.ok && r.at + 12 <= s.size())
    {
        const auto id = r.get<std::uint32_t>();
        const auto n = r.get<std::uint64_t>();
        if (n > s.size() - r.at)
            return st;
        if (id == 2)
            engineAt = r.at, engineEnd = r.at + (std::size_t) n;
        r.at += (std::size_t) n;
    }
    if (engineAt == 0)
        return st;
    Reader e { s, engineAt };
    e.get<std::uint32_t>();
    const auto n = e.get<std::uint64_t>();
    if (! e.ok || e.at + n != engineEnd)
        return st;
    st.version = e.get<std::int16_t>();
    if (st.version < 1 || st.version > 5)
        return st;
    st.program = e.get<std::int32_t>();
    e.get<std::int32_t>();
    e.get<std::uint8_t>();
    for (auto& p : st.programs)
    {
        e.get<std::int16_t>();
        e.skipString();
        p.sequence = e.get<std::int32_t>();
        p.patch = e.get<std::int32_t>();
        p.transpose = e.get<std::int32_t>();
    }
    st.patches.resize (128);
    for (int k = 0; k < 128; ++k)
        for (int c = 0; c < 2; ++c)
        {
            e.skipString();
            std::array<float, 100> v {};
            for (auto& x : v)
                x = e.get<float>();
            if (c == 1)
                st.patches[(std::size_t) k] = v;
        }
    st.sequences.resize (128);
    for (auto& q : st.sequences)
    {
        const auto ver = e.get<std::int16_t>();
        e.skipString();
        e.get<std::int16_t>();
        q.length = e.get<std::int32_t>();
        for (auto& stp : q.steps)
        {
            const auto sv = e.get<std::int16_t>();
            stp.slide = e.get<std::int8_t>() > 0;
            stp.note = e.get<std::int32_t>();
            stp.gate = e.get<std::int8_t>() > 0;
            stp.length = e.get<double>();
            stp.velocity = e.get<float>();
            if (sv == 1)
                stp.accent = e.get<std::int8_t>() > 0;
        }
        q.bpm = e.get<double>();
        q.internalBpm = e.get<std::int8_t>() > 0;
        q.shuffle = e.get<double>();
        q.gateLength = 0.66;
        if (ver > 0)
            q.gateLength = e.get<double>();
    }
    st.sequencerMode = e.get<std::int32_t>() != 0 ? 4 : 0;
    if (st.version >= 2)
    {
        st.seqTranspose = (float) e.get<std::int32_t>();
        e.get<std::int32_t>();
        e.get<std::int32_t>();
        e.get<std::int32_t>();
    }
    if (st.version >= 3)
        st.oversampling = e.get<std::int32_t>();
    st.engine = 0;
    if (st.version >= 5)
        st.engine = e.get<std::int32_t>();
    st.usable = e.ok && e.at == engineEnd && st.program >= 0 && st.program < 128;
    for (const auto& p : st.programs)
        if (p.patch < 0 || p.patch >= 128 || p.sequence < 0 || p.sequence >= 128)
            st.usable = false;
    return st;
}

// ------------------------------------------------------------------------------------------- the plugin (§2.10)
class Plugin
{
public:
    Plugin (const State& s, std::shared_ptr<const DllData> d, double fs) : st (s), dll (std::move (d))
    {
        if (st.engine == 0)
            engine = std::make_unique<Engine1> (dll);
        else
            engine = std::make_unique<Engine2> (dll, st.engine == 1);
        reverb.reset();
        program = st.program;
        seqMode = st.sequencerMode;
        seqTranspose = st.seqTranspose;
        applyPatch();
        setRate (fs);
    }

    void setRate (double fs) noexcept
    {
        engine->setRate (fs);
        delay.setRate (fs);
        reverb.allocate (fs);
        xfLength = (int) (fs * 0.3);
        xfStep = 1.0f / (float) xfLength;
        rate = fs;
        invRate = 1.0 / fs;
    }
    void setTempo (double bpm) noexcept { hostTempo = bpm; }

    // MyPlugin note-on (§2.10)
    void noteOn (int channel, int key, int velocity) noexcept
    {
        if (channel == 12)
        {
            seqTranspose = (float) (key - 60);
            return;
        }
        lastKey = key;
        if (seqMode == 0)
        {
            releaseList();
            held.clear();
            arpNoteOn (key, (float) velocity / 127.0f);
            playing = false;
            return;
        }
        const auto oldPatch = st.programs[(std::size_t) program].patch;
        program = std::clamp (key, 0, 127);
        if (st.programs[(std::size_t) program].patch != oldPatch)
            reset();
        if (seqMode != 0)
            arpNoteOn (0, 1.0f);
        applyPatch();
        playing = true;
    }

    void noteOff (int channel, int key) noexcept
    {
        if (channel == 12)
            return;
        if (seqMode != 0)
        {
            if (key != lastKey)
                return;
            key = 0;
        }
        arpNoteOff (key);
        playing = false;
    }

    void setPanExpression (float pan) noexcept
    {
        auto p = (pan + 1.0f) * 0.5f;
        p = 0.0f <= p ? std::min (1.0f, p) : 0.0f;
        gainR = std::sqrt (p);
        gainL = std::sqrt (1.0f - p);
    }
    void setPitchExpression (float semis) noexcept { engine->pitchExpr = semis * 0.009999999776482582f; }
    void setCutExpression (float x) noexcept { engine->cutExpr = x; }
    void setResExpression (float y) noexcept { engine->resExpr = y; }

    void render (float* left, float* right, int n) noexcept
    {
        RenderMode mode;
        for (int i = 0; i < n; ++i)
        {
            const auto& q = st.sequences[(std::size_t) st.programs[(std::size_t) program].sequence];
            const auto tempo = q.internalBpm ? q.bpm : hostTempo;
            if (playing)
            {
                engine->transpose = seqTranspose;
                clockSequencer (tempo);
            }
            else
                engine->transpose = 0.0f;
            const auto s = engine->tick();
            auto l = s * gainL, r = s * gainR;
            if (delayOn)
            {
                if (i == 0)
                    delayTimes (tempo);
                delay.tick (l, r, xfA, xfB);
                if (xfading)
                {
                    const auto f = (float) xfCount * xfStep;
                    if (xfIndex == 0)
                        xfA = 1.0f - f, xfB = f;
                    else
                        xfA = f, xfB = 1.0f - f;
                    if (++xfCount == xfLength)
                    {
                        xfading = false;
                        xfA = 0.5f > xfA ? 0.0f : 1.0f;
                        xfB = 0.5f > xfB ? 0.0f : 1.0f;
                        xfIndex = xfTarget;
                    }
                }
            }
            left[i] = l * midiVolume;
            right[i] = r * midiVolume;
        }
        if (0.5f <= params[0x0E])
            reverb.process (left, right, n);
    }

    // delay times at the start of each block: first use sets both taps; later changes crossfade over 0.3 s (§2.8)
    void delayTimes (double tempo) noexcept
    {
        float t;
        if (delaySync)
        {
            const auto beat = tempo / 60.0;
            t = (float) ((double) delaySyncTime * (1.0 / beat));
        }
        else
            t = delayFreeTime;
        if (delayTime == 0.0f)
        {
            delayTime = t;
            delay.time[0] = t;
            delay.updateTaps (0);
            delay.time[1] = delayTime;
            delay.updateTaps (1);
            delay.right[0] = delayRightNow;
            delay.updateTaps (0);
            delay.right[1] = delayRightNow;
            delay.updateTaps (1);
        }
        if (! xfading && (delayTime != t || delayRightNow != delayRight))
        {
            xfading = true;
            xfCount = 0;
            xfTarget = xfIndex ^ 1;
            delayTime = t;
            delayRightNow = delayRight;
            delay.time[(std::size_t) xfTarget] = t;
            delay.right[(std::size_t) xfTarget] = delayRight;
            delay.updateTaps (xfTarget);
        }
    }

    bool silent() const noexcept { return engine->silent(); }

private:
    void applyPatch() noexcept
    {
        const auto& patch = st.patches[(std::size_t) st.programs[(std::size_t) program].patch];
        for (int i = 0; i < 0x2A; ++i)
            setParam (i, (double) patch[(std::size_t) i]);
    }

    // MyPlugin parameter setter (§2.10)
    void setParam (int i, double value) noexcept
    {
        const auto d = 0.0 <= value ? std::min (1.0, value) : 0.0;
        const auto v = (float) d;
        engine->setParam (i, v);
        params[(std::size_t) i] = v;
        switch (i)
        {
            case 0x0F: reverb.setParam (1, v); break;
            case 0x10: reverb.setParam (2, v); break;
            case 0x11: reverb.setParam (3, v); break;
            case 0x12: reverb.setParam (4, v); break;
            case 0x13: reverb.setParam (5, v); break;
            case 0x14: reverb.setParam (6, v); break;
            case 0x15: reverb.setParam (9, (float) (d * d)); break;
            case 0x16: reverb.setParam (10, v); break;
            case 0x17: reverb.setParam (14, v); break;
            case 0x18: reverb.setParam (11, v); break;
            case 0x19: reverb.setParam (12, v); break;
            case 0x1A: reverb.setParam (13, v); break;
            case 0x20: delayOn = 0.5 <= d; break;
            case 0x21: delaySync = 0.5 <= d; break;
            case 0x22: delay.send = v; break;
            case 0x23:
            {
                static constexpr double sync[18] = { 0.015625, 0.03125, 0.041666666666666664, 0.046875, 0.0625, 0.08333333333333333, 0.09375, 0.3333333333333333, 0.125,
                                                     0.16666666666666666, 0.1875, 0.25, 0.3333333333333333, 0.375, 0.5, 0.6666666666666666, 0.75, 1.0 };
                const auto x = (double) params[0x23];
                const auto k = (int) ((double) (0x12 - 1) * x + 0.5);
                delaySyncTime = (float) (sync[k] * 4.0);
                delayFreeTime = (float) (x * 0.49 + 0.01);
                break;
            }
            case 0x24: delayRight = v; break;
            case 0x25: delay.feedback = v; break;
            case 0x26: delay.tone = v; break;
            case 0x27:
            {
                auto a = v * 0.5f;
                a = a + 0.5f;
                delay.stereoA = a;
                delay.stereoB = 1.0f - a;
                break;
            }
            default: break;
        }
    }

    void reset() noexcept
    {
        releaseList();
        held.clear();
        engine->noteOff();
        delay.clear();
        reverb.reset();
        reverb.dirty = true;
        playing = false;
    }

    // ---- the arpeggiator / sequencer object (§2.10)
    struct Held { int note; float velocity; };

    void arpNoteOn (int note, float vel) noexcept
    {
        if (seqMode == 0)
        {
            targetNoteOn (note, vel, false);
            return;
        }
        for (const auto& h : held)
            if (h.note == note)
                return;
        if (held.size() >= 20)
            return;
        if (held.empty())
        {
            octave = 0;
            heldIndex = 0;
            position = 0.0;
            stepIndex = -1;
        }
        held.push_back ({ note, vel });
    }

    void arpNoteOff (int note) noexcept
    {
        if (seqMode == 0)
        {
            targetNoteOff();
            return;
        }
        for (std::size_t i = 0; i < held.size(); ++i)
            if (held[i].note == note)
            {
                held.erase (held.begin() + (std::ptrdiff_t) i);
                break;
            }
        if (held.empty())
            releaseList();
    }

    void releaseList() noexcept
    {
        for (std::size_t i = 0; i < sounding.size(); ++i)
            targetNoteOff();
        sounding.clear();
        gateLeft = 0.0;
    }

    void targetNoteOn (int note, float vel, bool accentFlag) noexcept
    {
        const auto acc = accentFlag || 0.8661417365074158f <= vel;
        engine->noteOn (note + st.programs[(std::size_t) program].transpose, targetCount > 0, acc);
        ++targetCount;
    }
    void targetNoteOff() noexcept
    {
        targetCount = std::max (0, targetCount - 1);
        if (targetCount == 0)
            engine->noteOff();
    }

    void stepEvent (const Step& s, const Sequence& q) noexcept
    {
        if (! s.gate)
            return;
        if (gateLeft < 10000.0)
            releaseList();
        if (held.empty() || seqMode != 4)
            return;
        if (heldIndex >= (int) held.size())
            heldIndex = 0;
        const auto& h = held[(std::size_t) heldIndex];
        const auto note = std::clamp (s.note + octave * 12 + h.note, 0, 127);
        auto vel = (1.0f - velocityMix) * s.velocity;
        vel = vel + velocityMix * h.velocity;
        targetNoteOn (note, vel, s.accent);
        releaseList();
        sounding.push_back (note);
        gateLeft = s.slide ? 20000.0 : q.gateLength;
        ++heldIndex;
    }

    void clockSequencer (double tempo) noexcept
    {
        const auto& q = st.sequences[(std::size_t) st.programs[(std::size_t) program].sequence];
        auto inc = tempo * 0.06666666666666667;
        inc = inc * invRate;
        if (seqMode == 0)
            return;
        inc = inc * rateScale;
        const auto shuffle = -q.shuffle;
        const auto sw = shuffle * 0.5 + 1.0;
        auto pos = position;
        auto x = pos - (double) ((int) pos & ~1);
        auto base = pos - x;
        double idx;
        if (1.0 < x)
        {
            const auto a = (2.0 - sw) * (x - 1.0);
            idx = a + (base + sw);
        }
        else
            idx = x * sw + base;
        const auto step = (int) idx;
        if (stepIndex != step)
        {
            stepIndex = step;
            if (step >= 0 && step < 32)
                stepEvent (q.steps[(std::size_t) step], q);
        }
        if (! sounding.empty())
        {
            gateLeft = gateLeft - inc;
            if (0.0 > gateLeft)
            {
                for (std::size_t i = 0; i < sounding.size(); ++i)
                    targetNoteOff();
                sounding.clear();
                gateLeft = 0.0;
            }
        }
        pos = inc + position;
        position = pos;
        if ((double) q.length <= pos)
        {
            position = pos - (double) (int) pos;
            stepIndex = -1;
        }
    }

    State st;
    std::shared_ptr<const DllData> dll;
    std::unique_ptr<Engine> engine;
    std::array<float, 0x2A> params {};
    Delay delay;
    Reverb reverb;
    bool delayOn = false, delaySync = false, xfading = false;
    float delaySyncTime = 0.0f, delayFreeTime = 0.0f, delayRight = 0.0f, delayRightNow = 0.0f, delayTime = 0.0f;
    float xfA = 1.0f, xfB = 0.0f, xfStep = 0.0f;
    int xfLength = 13230, xfCount = 0, xfIndex = 0, xfTarget = 0;
    double rate = 44100.0, invRate = 1.0 / 44100.0, hostTempo = 125.0, rateScale = 1.0;
    int program = 0, seqMode = 0, lastKey = -1, targetCount = 0;
    float seqTranspose = 0.0f, gainL = 1.0f, gainR = 1.0f, midiVolume = 1.0f, velocityMix = 0.0f;
    bool playing = false;
    std::vector<Held> held;
    std::vector<int> sounding;
    int octave = 0, heldIndex = 0, stepIndex = -1;
    double position = 0.0, gateLeft = 0.0;
};

// MIDI velocity as FL's wrapper derives it from Init.Vol (§2.10)
int velocityFromInitVol (double initVol) noexcept
{
    auto v = lnF ((float) initVol * 10.0f + 1.0f) / lnF (21.0f) * 127.0f;
    v = 0.0f > v ? 0.0f : std::min (127.0f, v);
    return (int) v;
}

// ------------------------------------------------------------------------------------------------ LYNX instrument
class TransistorBassInstrument final : public Instrument
{
public:
    ~TransistorBassInstrument() override
    {
        delete incoming.exchange (nullptr);
        delete retired.exchange (nullptr);
    }

    void prepare (double sr) noexcept override
    {
        delete retired.exchange (nullptr);
        take();
        if (sr == rate.load() && plugin != nullptr)
            return;
        rate.store (sr);
        delete incoming.exchange (build().release());
        take();
    }

    void configure (const ChannelDef& channel, double bpm) noexcept override
    {
        delete retired.exchange (nullptr);
        tempo.store (bpm > 0.0 ? bpm : 140.0);
        channelCents.store (channel.sampler.pitch * 100.0f);
        if (channel.pluginState == state && channel.samplePath == dllPath)
            return;
        state = channel.pluginState;
        dllPath = channel.samplePath;
        parsed = parseState (state);
        data = loadDll (dllPath);
        delete incoming.exchange (build().release());
    }

    void setNextNoteExpression (float pan, float cents, bool) noexcept override
    {
        nextPan = pan;
        nextCents = cents;
    }

    void noteOn (int key, float velocity) noexcept override
    {
        take();
        if (plugin == nullptr)
            return;
        const auto k = std::clamp (key, 0, 127);
        const auto vel = std::clamp ((double) velocity, 0.0, 1.0);
        const auto initVol = (std::pow (11.0, 1.28 * vel) - 1.0) / 10.0;
        const auto v7 = velocityFromInitVol (initVol);
        if (sounding[(std::size_t) k])
            plugin->noteOff (0, k);
        sounding[(std::size_t) k] = true;
        notePan[(std::size_t) k] = nextPan;
        noteCents[(std::size_t) k] = (float) channelCents.load() + nextCents;
        slide = {};
        slideKey = k;
        plugin->noteOn (0, k, v7);
        sent[(std::size_t) k] = false;
        nextPan = 0.0f, nextCents = 0.0f;
        quiet = 0;
    }

    void noteOff (int key) noexcept override
    {
        take();
        const auto k = std::clamp (key, 0, 127);
        if (plugin == nullptr || ! sounding[(std::size_t) k])
            return;
        sounding[(std::size_t) k] = false;
        plugin->noteOff (0, k);
    }

    // a piano-roll slide note (issue #14): FL's host glides the sounding note's Final.Pitch (the bass is monophonic:
    // one pitch expression), separate from Transistor Bass's own sequencer slides
    void slideTo (int key, float cents, float samples) noexcept override
    {
        take();
        if (plugin == nullptr || slideKey < 0 || ! sounding[(std::size_t) slideKey])
            return;
        const auto to = (float) (100.0 * (std::clamp (key, 0, 127) - 60)) + channelCents.load() + cents;
        slide.start (to - ((float) (100 * (slideKey - 60)) + noteCents[(std::size_t) slideKey]), samples);
    }

    void allNotesOff() noexcept override
    {
        for (int k = 0; k < 128; ++k)
            noteOff (k);
    }

    // idle once no key is held and the output (synth, delay and reverb tails) stayed below −140 dB for half a second
    bool idle() const noexcept override { return plugin == nullptr || quiet >= (int) (rate.load() * 0.5); }

    void render (float* left, float* right, int n, float gl, float gr) noexcept override
    {
        take();
        if (plugin == nullptr || n <= 0)
            return;
        const auto vol = std::sqrt (((double) gl * gl + (double) gr * gr) * 0.5);
        auto pan = vol > 0.0 ? std::atan2 ((double) gr, (double) gl) * (4.0 / 3.141592653589793) - 1.0 : 0.0;
        if (std::fabs (pan) < 1e-9)
            pan = 0.0;
        for (int k = 0; k < 128; ++k)
            if (sounding[(std::size_t) k] && ! sent[(std::size_t) k])
            {
                plugin->setCutExpression (0.0f);
                plugin->setResExpression (0.0f);
                plugin->setPitchExpression (noteCents[(std::size_t) k]);
                plugin->setPanExpression ((float) std::clamp ((double) notePan[(std::size_t) k] + pan, -1.0, 1.0));
                sent[(std::size_t) k] = true;
            }
        plugin->setTempo (tempo.load());
        float peak = 0.0f;
        for (int done = 0; done < n;)
        {
            auto m = std::min (n - done, (int) scratchL.size());
            if (slideKey >= 0 && (slide.moving() || slide.cents != 0.0f)) // (issue #14) the slide, 64 samples at a time
            {
                if (slide.moving())
                    m = std::min (m, 64);
                plugin->setPitchExpression (noteCents[(std::size_t) slideKey] + slide.cents);
                slide.advance ((float) m);
            }
            plugin->render (scratchL.data(), scratchR.data(), m);
            const auto g = (float) vol;
            for (int i = 0; i < m; ++i)
            {
                left[done + i] += scratchL[(std::size_t) i] * g;
                right[done + i] += scratchR[(std::size_t) i] * g;
                peak = std::max ({ peak, std::fabs (scratchL[(std::size_t) i]), std::fabs (scratchR[(std::size_t) i]) });
            }
            done += m;
        }
        bool held = false;
        for (const auto k : sounding)
            held = held || k;
        quiet = ! held && peak < 1.0e-7f ? std::min (quiet + n, 1 << 30) : 0;
    }

private:
    std::unique_ptr<Plugin> build() const
    {
        if (! parsed.usable || data == nullptr)
            return nullptr;
        return std::make_unique<Plugin> (parsed, data, rate.load());
    }

    void take() noexcept
    {
        if (retired.load() != nullptr)
            return;
        if (auto* s = incoming.exchange (nullptr))
        {
            retired.store (plugin.release());
            plugin.reset (s);
            sounding.fill (false);
            quiet = 0;
        }
    }

    std::unique_ptr<Plugin> plugin;
    std::atomic<Plugin*> incoming { nullptr }, retired { nullptr };
    State parsed;
    std::shared_ptr<const DllData> data;
    std::atomic<double> rate { 44100.0 }, tempo { 140.0 };
    std::atomic<float> channelCents { 0.0f };
    std::vector<std::uint8_t> state;
    std::string dllPath;
    int quiet = 0;
    std::array<bool, 128> sounding {}, sent {};
    std::array<float, 128> notePan {}, noteCents {};
    HostSlide slide;   // a piano-roll slide note (issue #14)
    int slideKey = -1; // the last note started
    float nextPan = 0.0f, nextCents = 0.0f;
    std::array<float, 512> scratchL {}, scratchR {};
};
} // namespace

bool transistorBassStateUsable (const std::vector<std::uint8_t>& state) { return parseState (state).usable; }

bool transistorBassDllUsable (const std::string& path) { return loadDll (path) != nullptr; }

std::unique_ptr<Instrument> makeTransistorBassInstrument() { return std::make_unique<TransistorBassInstrument>(); }

namespace tbass
{
std::vector<float> renderReference (const std::vector<std::uint8_t>& state, const std::string& dll, const std::vector<RefNote>& notes,
                                    double sampleRate, int length, const RefOptions& options)
{
    const auto st = parseState (state);
    auto data = loadDll (dll);
    if (! st.usable || data == nullptr || length <= 0)
        return {};
    auto plugin = std::make_unique<Plugin> (st, data, sampleRate);
    plugin->setTempo (options.tempo > 0.0 ? options.tempo : 125.0);
    std::vector<float> out (2 * (std::size_t) length);
    std::vector<float> l ((std::size_t) std::max (1, options.block)), r ((std::size_t) std::max (1, options.block));
    struct Pending { int key; float pan, cents, fcut, fres; };
    std::vector<Pending> pending;
    std::array<bool, 128> sounding {};
    for (int n0 = 0; n0 < length;)
    {
        auto len = std::min (options.block, length - n0);
        for (const auto& nt : notes)
        {
            if (nt.start > n0 && nt.start < n0 + len)
                len = nt.start - n0;
            if (nt.release > n0 && nt.release < n0 + len)
                len = nt.release - n0;
        }
        for (const auto& nt : notes)
            if (nt.release == n0 && sounding[(std::size_t) (nt.key & 127)])
            {
                sounding[(std::size_t) (nt.key & 127)] = false;
                plugin->noteOff (nt.channel, nt.key);
            }
        for (const auto& nt : notes)
            if (nt.start == n0)
            {
                const auto v = velocityFromInitVol (nt.initVol);
                if (sounding[(std::size_t) (nt.key & 127)])
                    plugin->noteOff (nt.channel, nt.key);
                sounding[(std::size_t) (nt.key & 127)] = true;
                plugin->noteOn (nt.channel, nt.key, v);
                pending.push_back ({ nt.key, (float) nt.finalPan, (float) nt.pitchCents, (float) nt.fcut, (float) nt.fres });
            }
        for (const auto& p : pending)
        {
            plugin->setCutExpression (p.fcut);
            plugin->setResExpression (p.fres);
            plugin->setPitchExpression (p.cents);
            plugin->setPanExpression (p.pan);
        }
        pending.clear();
        plugin->render (l.data(), r.data(), len);
        for (int i = 0; i < len; ++i)
        {
            out[2 * (std::size_t) (n0 + i)] = l[(std::size_t) i];
            out[2 * (std::size_t) (n0 + i) + 1] = r[(std::size_t) i];
        }
        n0 += len;
    }
    return out;
}
} // namespace tbass
} // namespace lynx
