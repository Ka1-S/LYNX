#include "core/Granulizer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>

#include "core/FlAutoSlicer.h"
#include "core/FlVoiceProcessor.h"

// Port of our round-67 reference model (r67/py/granmodel.py, REA_rodada67). The model emulates the DLL's float32
// arithmetic with numpy float32 scalars, doubles where the DLL computes in double and 80-bit long doubles where it uses
// the x87 (precision 64 bits); this file keeps those types and the operation order, so FP contraction stays off.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")
#endif

namespace lynx
{
namespace
{
constexpr int paramCount = 12;
constexpr int snapshotSize = flvp::snapshotSize;
constexpr int stateVersion = 7;
constexpr int stateSize = 4 + 48 + 12 + snapshotSize; // version 7: 428 bytes
constexpr int maxVoices = 64;   // voice records per instance (FL: unlimited; the oldest is cut beyond this)
constexpr int maxGrains = 48;   // grains per voice (the knobs' ranges allow about 20 at once)
constexpr int defaultPoints = 12; // the library's default quality (0x79cda0 = 4 → 0x79c294[3]): "24-point sinc"
// what FL's host sets with ProcessMode (Dispatcher 1, bits 8…23 → FUN_0060b020) when it renders: 16 points per side
// ("32-point sinc") [EXEC FL: the DLL matches FL's renders of a 48 kHz sample only with 16, −76.6 dB; 12: −40.6 dB]
constexpr int flPoints = 16;
constexpr int maxPoints = 0x200;

using flvp::bitsOf;
using ld = long double;

std::int32_t i32At (const std::uint8_t* d) noexcept
{
    std::int32_t v;
    std::memcpy (&v, d, 4);
    return v;
}

void putI32 (std::uint8_t* d, std::int32_t v) noexcept { std::memcpy (d, &v, 4); }

// cvtsd2si (Delphi Round): half to even
inline std::int64_t rnd (double x) noexcept { return (std::int64_t) std::nearbyint (x); }

// the editor knobs (EXEC: TVectorWheel min/max at +0x3a8/+0x3ac)
constexpr std::array<std::pair<int, int>, paramCount> ranges { {
    { 50, 500 }, { 0, 500 }, { 125, 1000 }, { -1000, 1000 }, { 0, 384 }, { 4, 200 },
    { 0, 256 }, { 0, 64 }, { 0, 1 }, { 0, 1 }, { 0, 4096 }, { 0, 500 } } };
// the plugin's defaults (the knobs' default values; parameter 11 is 0xf8 when a short state leaves it out)
constexpr std::array<int, paramCount> defaults { 204, 50, 611, 611, 0, 180, 0, 0, 0, 0, 0, 228 };

struct Params
{
    std::array<int, paramCount> P = defaults;
    int sliceMode = 0, keyMode = 0, sync = 2;
};

// SaveRestoreState (FUN_007519f0): int version, 12 ints (a short stream keeps what it does not reach: parameter 11 =
// 0xf8), version ≥ 4 the slice mode, ≥ 5 the key mode, ≥ 7 the sync byte (an int) and the 364-byte snapshot
bool parse (const std::vector<std::uint8_t>& d, Params& out, std::vector<std::uint8_t>* snapshot)
{
    if (d.size() < 8)
        return false;
    const auto ver = i32At (d.data());
    if (ver < 1 || ver > 7)
        return false;
    Params p;
    p.P[11] = 0xf8;
    const auto body = std::min<std::size_t> (48, d.size() - 4);
    for (std::size_t k = 0; k < body / 4; ++k)
        p.P[k] = i32At (d.data() + 4 + 4 * k);
    std::size_t o = 52;
    if (ver >= 4)
    {
        p.sliceMode = d.size() >= o + 4 ? i32At (d.data() + o) : 0;
        o += 4;
    }
    if (ver >= 5)
    {
        p.keyMode = d.size() >= o + 4 ? i32At (d.data() + o) : 0;
        o += 4;
    }
    if (ver > 6 && d.size() >= o + 4)
    {
        p.sync = i32At (d.data() + o) & 0xff;
        o += 4;
        if (snapshot != nullptr && d.size() >= o + snapshotSize)
            snapshot->assign (d.begin() + (std::ptrdiff_t) o, d.begin() + (std::ptrdiff_t) (o + snapshotSize));
    }
    // the editor's key-mode combo (0x758050, run on every state load): "slices" keys without a slice mode turn on auto
    // slicing [DECOMP + EXEC: such a state plays the auto slices]
    if (p.keyMode == 3 && p.sliceMode == 0)
        p.sliceMode = 2;
    out = p;
    return true;
}

bool parseNormalised (const std::vector<std::uint8_t>& d, Params& out)
{
    return d.size() == (std::size_t) stateSize && i32At (d.data()) == stateVersion && parse (d, out, nullptr);
}

// FUN_00752170: milliseconds of a time knob
int msLaw (int v) noexcept
{
    const auto x = (double) (float) ((double) v * 0.002) * std::log (51.0);
    return (int) rnd ((double) ((float) (std::exp (x) - 1.0) * 10.0f));
}

// FUN_007521f0: spacing knobs, 256 = 100 % (signed)
int pctLaw (int v) noexcept
{
    const auto a = std::abs (v);
    const auto x = (double) (float) ((double) a * 0.001) * std::log (11.0);
    const auto r = (int) rnd ((double) (float) (std::exp (x) - 1.0) * 76.8);
    return v < 0 ? -r : r;
}

// ------------------------------------------------------------------------------------------------------- the sample

struct Sample
{
    const float* L = nullptr;
    const float* R = nullptr;
    int n = 0;
    double rate = 44100.0;
    int ls = 0, le = -1; // loop points (+0x108/+0x110): the whole sample
};

// ------------------------------------------------------------------------------------------------------- reader

const ld piX87 = 3.14159265358979323846264338327950288L; // fldpi

// The library's sample reader runs with x87 control word 0x73F (FUN_005a2b90 … FUN_005a2b80): 64-bit precision,
// rounding toward −∞ for every x87 operation and store (SSE code keeps rounding to nearest). The guard sets the same
// rounding for LYNX's long double arithmetic (x87 on x86 GCC/Clang); the x87 parts live in noinline functions so the
// compiler cannot move them out of the guarded region.
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
struct X87Down
{
    unsigned short saved = 0;
    X87Down() noexcept
    {
        asm volatile ("fnstcw %0" : "=m"(saved));
        unsigned short cw = (unsigned short) ((saved & ~0xc00) | 0x400 | 0x300);
        asm volatile ("fldcw %0" : : "m"(cw) : "memory");
    }
    ~X87Down() { asm volatile ("fldcw %0" : : "m"(saved) : "memory"); }
};
#define LYNX_NOINLINE __attribute__ ((noinline))
// an x87 store to float (fstp DWORD) under the current control word: the compiler would turn the conversion of a value
// that came from a double into an SSE conversion (rounding to nearest)
inline float x87Float (ld v) noexcept
{
    float r;
    asm volatile ("fstps %0" : "=m"(r) : "t"(v) : "st");
    return r;
}
#else
inline float x87Float (ld v) noexcept { return (float) v; }
struct X87Down
{
};
#define LYNX_NOINLINE
#endif

// the kernel's x87 set-up, part 1 (0x5f8c40): the window and sinc arguments
LYNX_NOINLINE void sincArgs (double fr, double wa, double cb, double* d) noexcept
{
    X87Down guard;
    const auto frl = (ld) fr;
    const auto d0e = frl * (ld) (-wa) + (ld) 1.5707963267948966;
    d[0] = (double) d0e;
    d[1] = (double) (d0e + (ld) (-wa));
    const auto d2e = frl * (ld) cb;
    d[2] = (double) d2e;
    d[3] = (double) (d2e + (ld) cb);
}

// part 2 (0x5f8ca0): the recurrences' first values and the four denominators
LYNX_NOINLINE void sincStart (double fr, const double* sn, double wc, double cc, float* cur, float* prev, float* x) noexcept
{
    X87Down guard;
    const auto w0 = (ld) sn[0], w1 = (ld) sn[1], n0 = (ld) sn[2], n1 = (ld) sn[3];
    const auto C38 = (ld) wc, C30 = (ld) cc;
    const auto t = w0 * C38 - w1;
    const auto t2 = n0 * C30 - n1;
    cur[0] = x87Float (t2 * C30 - n0), cur[1] = x87Float (n1), cur[2] = x87Float (t * C38 - w0), cur[3] = x87Float (w1);
    prev[0] = x87Float (t2), prev[1] = x87Float (n0), prev[2] = x87Float (t), prev[3] = x87Float (w0);
    const auto e = (ld) fr * piX87;
    const auto em = e - piX87;
    x[0] = x87Float (em), x[1] = x87Float (e), x[2] = x87Float (em - piX87), x[3] = x87Float (e + piX87);
}

// The windowed sinc of the library (0x5f8f40 → 0x5f8c40, ippsSin_64f_A50, 0x5f8ca0; float data: 0x5f9880), one frame:
// taps L/R hold frames i − pts + 1 … i + pts; our restatement, with rcpps replaced by the exact reciprocal and IPP's
// sine by the C library's (REA_rodada67 §3.3)
void sincFrame (const float* tl, const float* tr, int pts, double fr, double wa, double wc, double cb, double cc, float& outL, float& outR) noexcept
{
    if (! (fr >= 1e-25))
        fr = 1e-25; // ippsThreshold_LT_64f_I
    double d[4], sn[4];
    sincArgs (fr, wa, cb, d);
    for (int k = 0; k < 4; ++k)
        sn[k] = std::sin (d[k]);
    float cur[4], prev[4], x[4];
    sincStart (fr, sn, wc, cc, cur, prev, x);
    const float mul[4] = { (float) cc, (float) cc, (float) wc, (float) wc };
    const float inc[4] = { -6.28318548f, 6.28318548f, -6.28318548f, 6.28318548f };
    float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const int i0 = pts - 1;
    for (int it = 0; it < pts / 2; ++it)
    {
        float r2[4];
        for (int k = 0; k < 4; ++k)
        {
            const auto r = 1.0f / x[k];
            auto x1 = x[k] * r;
            x1 = x1 * r;
            r2[k] = (r + r) - x1;
        }
        float o6[4];
        for (int k = 0; k < 4; ++k)
        {
            o6[k] = prev[k];
            prev[k] = cur[k];
            cur[k] = cur[k] * mul[k] - o6[k];
        }
        float c01[2];
        for (int k = 0; k < 2; ++k)
        {
            const auto w = o6[2 + k];
            const auto wt = ((0.64f * w) * w + w) + 0.36f;
            c01[k] = (o6[k] * r2[k]) * wt;
        }
        for (int k = 0; k < 4; ++k)
            x[k] = x[k] + inc[k];
        const auto f0 = i0 + 1 + 2 * it, b1 = i0 - 2 * it, f1 = i0 + 2 + 2 * it, b0 = i0 - 1 - 2 * it;
        acc[0] = acc[0] + c01[0] * tl[f0];
        acc[1] = acc[1] + c01[0] * tr[f0];
        acc[2] = acc[2] + c01[1] * tl[b1];
        acc[3] = acc[3] + c01[1] * tr[b1];
        float o6b[4];
        for (int k = 0; k < 4; ++k)
        {
            o6b[k] = prev[k];
            prev[k] = cur[k];
            cur[k] = cur[k] * mul[k] - o6b[k];
        }
        float c23[2];
        for (int k = 0; k < 2; ++k)
        {
            const auto w = o6b[2 + k];
            const auto wt = ((0.64f * w) * w + w) + 0.36f;
            c23[k] = (o6b[k] * r2[2 + k]) * wt;
        }
        acc[0] = acc[0] + c23[0] * tl[f1];
        acc[1] = acc[1] + c23[0] * tr[f1];
        acc[2] = acc[2] + c23[1] * tl[b0];
        acc[3] = acc[3] + c23[1] * tr[b0];
    }
    outL = (acc[2] + acc[0]) * 0.5f;
    outR = (acc[3] + acc[1]) * 0.5f;
}

// One grain's sample reader (the library's FUN_005fc5c0 class): positions in double, the copy path at unity speed,
// otherwise the sinc above; edges and loops through a window of repeated / wrapped frames (FUN_005fd480)
struct Reader
{
    const Sample* s = nullptr;
    bool loop = false;
    std::int64_t a = 0, b = 0, la = 0, lb = 0, c0 = 0, c8 = 0, d0 = 0, d8 = 0;
    int pts = defaultPoints, quality = defaultPoints, loops = 0;
    double pos = 0.0, speed = 1.0, wa = 0.0, wc = 0.0, cb = 0.0, cc = 0.0;
    bool copy = false;

    void init (const Sample& smp, bool looped, int points) noexcept
    {
        quality = std::clamp (points, 1, maxPoints);
        s = &smp;
        loop = looped;
        const auto L = (std::int64_t) smp.n - 1;
        a = 0, b = L + 1;
        if (loop && smp.ls >= 0)
        {
            la = std::min (std::max<std::int64_t> (smp.ls, a), b - 1);
            lb = std::min (std::max<std::int64_t> (smp.le, a), b - 1) + 1;
        }
        else
            la = a, lb = b;
        pts = quality, loops = 0;
        setPoints (quality);
        pos = (double) a, speed = 1.0;
        cb = 3.141592653589793, cc = -2.0;
        bounds();
    }

    void setPoints (int p) noexcept
    {
        pts = p;
        wa = 3.141592653589793 / (double) p;
        wc = std::cos (wa) * 2.0;
    }

    // FUN_005fc750
    void bounds() noexcept
    {
        c0 = (loops < 1 ? a : la) - 1 + pts;
        c8 = lb - pts;
        d0 = c0 + 3, d8 = c8 - 3;
    }

    // FUN_005fcfa0 → FUN_005fce50: the speed, the copy flag, the cutoff above unity and the points it needs
    void setSpeed (double cents, double ratio) noexcept
    {
        if (cents == 0.0 && ratio == 1.0)
            speed = 1.0;
        else
            speed = std::exp (cents * (std::log (2.0) / 12.0 / 100.0)) * ratio;
        copy = speed == 1.0 && pos - std::floor (pos) == 0.0;
        const auto sp = std::fabs (speed);
        if (sp <= 1.0)
            cb = 3.141592653589793, cc = -2.0;
        else
            cb = 3.141592653589793 / sp, cc = std::cos (cb) * 2.0;
        auto p = (int) ((rnd (sp) + 3) & ~(std::int64_t) 3);
        p = std::min (std::max (p, quality), maxPoints);
        if (p != pts)
        {
            setPoints (p);
            bounds();
        }
    }

    float frameL (std::int64_t i) const noexcept { return s->L[std::clamp (i, a, b - 1)]; }
    float frameR (std::int64_t i) const noexcept { return s->R[std::clamp (i, a, b - 1)]; }

    // frames i − pts + 1 … i + pts into tl/tr (the edge window of FUN_005fd480; +0x76 = 0: edge frames repeated)
    void taps (std::int64_t i, float* tl, float* tr, bool edge) const noexcept
    {
        if (! edge)
        {
            for (int k = 0; k < 2 * pts; ++k)
            {
                tl[k] = s->L[i - pts + 1 + k];
                tr[k] = s->R[i - pts + 1 + k];
            }
            return;
        }
        const auto span = lb - la;
        auto cnt = loops;
        auto lim = cnt >= 1 ? la : (std::int64_t) -0x7fffffff;
        auto j = i;
        for (int k = 0; k < pts; ++k)
        {
            while (j < lim)
            {
                j += span;
                if (--cnt < 1)
                    lim = -0x7fffffff;
            }
            tl[pts - 1 - k] = frameL (j);
            tr[pts - 1 - k] = frameR (j);
            --j;
        }
        j = i;
        const auto hi = loop ? lb : (std::int64_t) 0x7fffffff;
        for (int k = 0; k < pts; ++k)
        {
            ++j;
            while (hi <= j)
                j -= span;
            tl[pts + k] = frameL (j);
            tr[pts + k] = frameR (j);
        }
    }

    void interp (std::int64_t i, double fr, bool edge, float* tl, float* tr, float& oL, float& oR) const noexcept
    {
        taps (i, tl, tr, edge);
        sincFrame (tl, tr, pts, fr, wa, wc, cb, cc, oL, oR);
    }

    // FUN_005fd890 (…, 1): n frames into out (interleaved stereo); returns how many it produced
    int render (float* out, int n, float* tl, float* tr) noexcept
    {
        if (b <= a)
            return 0;
        int done = 0;
        while (done < n)
        {
            const auto m = n - done;
            const auto ip = rnd (pos);
            if (ip < d0 || (double) d8 <= pos + speed * (double) m)
            {
                int k = 0;
                while (k < m)
                {
                    auto i = (std::int64_t) std::trunc (pos);
                    if (lb <= i)
                    {
                        if (! loop)
                            return done + k;
                        do
                        {
                            pos = (pos - (double) lb) + (double) la;
                            i = (std::int64_t) std::trunc (pos);
                            if (++loops == 1)
                                bounds();
                        } while (lb <= i);
                    }
                    const auto fr = pos - (double) i;
                    pos = pos + speed;
                    const auto edge = i < c0 || c8 <= i;
                    interp (i, fr, edge, tl, tr, out[2 * (done + k)], out[2 * (done + k) + 1]);
                    ++k;
                    if (edge && k < m)
                        break; // the edge window is flushed: the path is chosen again (FUN_005fd890)
                }
                done += k;
            }
            else
            {
                if (copy)
                    for (int k = 0; k < m; ++k)
                    {
                        out[2 * (done + k)] = s->L[ip + k];
                        out[2 * (done + k) + 1] = s->R[ip + k];
                    }
                else
                {
                    // ippsVectorSlope_64f (the installation's dsp_ippv2_x64.dll, EXEC): eight lanes from pos + speed·l
                    // stepping by speed·8, the last m mod 8 positions from lane 0's next value adding speed one by one
                    double lanes[8];
                    for (int l = 0; l < 8; ++l)
                        lanes[l] = pos + speed * (double) l;
                    const auto full = (m / 8) * 8;
                    auto at = [&] (int k, double p) {
                        const auto i = (std::int64_t) p;
                        interp (i, p - (double) i, false, tl, tr, out[2 * (done + k)], out[2 * (done + k) + 1]);
                    };
                    for (int b0 = 0; b0 < full; b0 += 8)
                        for (int l = 0; l < 8; ++l)
                        {
                            at (b0 + l, lanes[l]);
                            lanes[l] = lanes[l] + speed * 8.0;
                        }
                    auto x = lanes[0];
                    for (int k = full; k < m; ++k)
                    {
                        at (k, x);
                        x = x + speed;
                    }
                }
                pos = pos + speed * (double) m;
                done += m;
            }
        }
        return done;
    }
};

// ------------------------------------------------------------------------------------------------------- voices

struct Grain
{
    Reader r;
    float g = 0, step = 0, L = 0, R = 0;
    int cnt = 0, hold = 0, end = 0, ofs = 0;
};

struct Voice
{
    std::array<Grain, maxGrains> grains;
    int nGrains = 0, count = 0, next = 0, elapsed = 0, key = 60, slice = -1;
    float pos = 0, phase = 0, pofs = 0, ipitch = 0;
    bool done = false, looped = false, ended = false;
    float fpitch = 0, fpitchBase = 0, fvol = 1, fpan = 0;
    std::uint64_t serial = 0;
    flvp::Proc proc;
};

// 0x60b3f0: dst += x·g (interleaved)
inline void mixConst (const float* x, float* dst, int n, float gL, float gR) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        dst[2 * i] = x[2 * i] * gL + dst[2 * i];
        dst[2 * i + 1] = x[2 * i + 1] * gR + dst[2 * i + 1];
    }
}

// 0x60b5c0: the SSE ramp — lanes [L0, R0, L1, R1] start at g and g + s, +2s per pair of frames, single frames +s
inline void mixRampI (const float* x, float* dst, int n, float gL, float gR, float sL, float sR) noexcept
{
    float a0 = gL + 0.0f, a1 = gR + 0.0f, a2 = gL + sL, a3 = gR + sR;
    const float s2L = sL + sL, s2R = sR + sR;
    const int full = (n >> 3) << 3;
    for (int i = 0; i < full; i += 2)
    {
        dst[2 * i] = x[2 * i] * a0 + dst[2 * i];
        dst[2 * i + 1] = x[2 * i + 1] * a1 + dst[2 * i + 1];
        dst[2 * i + 2] = x[2 * i + 2] * a2 + dst[2 * i + 2];
        dst[2 * i + 3] = x[2 * i + 3] * a3 + dst[2 * i + 3];
        a0 = a0 + s2L, a1 = a1 + s2R, a2 = a2 + s2L, a3 = a3 + s2R;
    }
    for (int i = full; i < n; ++i)
    {
        dst[2 * i] = x[2 * i] * a0 + dst[2 * i];
        dst[2 * i + 1] = x[2 * i + 1] * a1 + dst[2 * i + 1];
        a0 = a0 + sL, a1 = a1 + sR;
    }
}

class Engine
{
public:
    void init (const Params& params, double sampleRate, const std::uint8_t* snapshot, int ppq, double tempo)
    {
        P = params;
        fs = (int) std::lround (sampleRate);
        std::memcpy (snap.data(), snapshot, snap.size());
        timing (ppq, tempo);
        S.setRate ((double) fs); // Dispatcher 4 → FUN_0060adc0: the library's rate ratio and ramp limit
        update();
    }

    void timing (int ppq, double tempo) noexcept { S.retime (snap.data(), ppq, tempo); }

    // FUN_0060dbf0: ms → samples (double), stored as float
    float samplesMs (int ms) const noexcept { return (float) ((double) ms * (double) fs * (double) 0.001f); }

    // tempo sync (FUN_007524e0 → FUN_00752490): the state's sync byte is 1 and the host sent a beat count (Dispatcher
    // 0x13: the channel's stretch time, else the sample's ACID beats, REA_rodada165g-flgran165 §2)
    bool syncOn() const noexcept { return P.sync == 1 && beats > 0.0f && beatLen > 0; }

    // FUN_00752290 (+ the laws of parameters 4, 5, 10); the beat length of FUN_00752520 first
    void update() noexcept
    {
        const auto& p = P.P;
        const auto tempoF = (float) tempo;
        beatLen = tempoF > 0.0f ? (int) rnd ((double) (((beats * 60.0f) * (float) fs) / tempoF)) : 0;
        A = samplesMs (msLaw (p[0]));
        H = samplesMs (msLaw (p[1]));
        th = samplesMs (msLaw (p[11]));
        AH = A + H;
        auto gs = (AH * (float) pctLaw (p[2])) * 0.00390625f;
        if (5.0f > gs)
            gs = 5.0f;
        gsp = gs;
        loopFlag = p[8] > 0;
        ratio = smp.rate * (1.0 / (double) fs);
        if (! syncOn())
            wsp = (float) ((double) (AH * (float) pctLaw (p[3])) * ratio * (double) 0.00390625f);
        else
        {
            // one pass over the sample per beat count: the wave spacing follows the grain spacing, the voice loops
            wsp = (gsp * (float) ((std::int64_t) smp.le + 1)) / (float) beatLen;
            loopFlag = true;
        }
        invAH = 1.0f / AH;
        k18c = AH / (wsp <= 1.0f ? 1.0f : wsp);
        depth = (float) p[4] * 0.00390625f;
        rate5 = (float) (3.141592653589793 / (double) (0xcc - p[5]));
        start = smp.n > 0 ? (int) (((std::int64_t) p[10] * (std::int64_t) smp.le) / 0x1000) : 0;
        loopLen = smp.le - smp.ls + 1;
    }

    // Delphi's Random (the DLL's RandSeed)
    std::uint32_t random (std::uint32_t range) noexcept
    {
        seed = seed * 0x8088405u + 1u;
        return (std::uint32_t) (((std::uint64_t) seed * range) >> 32);
    }

    // FUN_007534e0: past the max polyphony, the first unreleased voice (else the last one) starts its end fade
    void steal() noexcept
    {
        if (maxPoly <= 0 || maxPoly > count)
            return;
        auto* v = &voices[(std::size_t) order[0]];
        for (int i = 0; i < count; ++i)
        {
            v = &voices[(std::size_t) order[(std::size_t) i]];
            if (! v->proc.released)
                break;
        }
        v->proc.kill();
    }

    // FUN_00753580 (TriggerVoice); −1 without a sample (FL: no voice, the host kills the note)
    int trigger (int key, float initPitch, float finalPitch, float finalVol, float finalPan) noexcept
    {
        if (smp.n < 1)
            return -1;
        steal();
        if (count >= maxVoices)
            removeAt (0);
        int slot = 0;
        while (used[(std::size_t) slot])
            ++slot;
        used[(std::size_t) slot] = true;
        auto& v = voices[(std::size_t) slot];
        v.nGrains = 0, v.count = 0, v.next = 0, v.elapsed = 0, v.key = key, v.slice = -1;
        v.pos = (float) start, v.phase = 0.0f, v.pofs = 0.0f, v.ipitch = initPitch;
        v.done = v.looped = v.ended = false;
        v.fpitch = v.fpitchBase = finalPitch, v.fvol = finalVol, v.fpan = finalPan;
        v.serial = ++serials;
        if (P.keyMode >= 1) // key mapping: C5 = the start, each key a step (mode 1: 1/24 of the sample, 2: a 16th, 3: a slice)
        {
            auto k = (int) rnd ((double) (v.ipitch * 0.01f)) - 60;
            v.pofs = (float) (k * -100);
            k = std::max (k, 0);
            auto d = 0.0;
            if (P.keyMode == 1)
                d = (double) (smp.le * k) / 24.0;
            else if (P.keyMode == 2)
            {
                if (beats <= 0.0f)
                    d = (((double) k / 4.0) * 60.0 * smp.rate) / (double) (float) tempo; // (P+0x1a0: the tempo as a float)
                else
                    d = ((double) smp.le * ((double) k / 4.0)) / (double) beats; // the sample spans `beats` beats
            }
            else if (P.keyMode == 3 && nSl > 0)
                d = (double) sl[(std::size_t) (k % nSl)];
            const auto p = rnd ((double) v.pos + d);
            v.pos = (float) (p % ((std::int64_t) smp.le + 1));
        }
        v.proc.init (S);
        order[(std::size_t) count++] = slot;
        return slot;
    }

    void release (int slot) noexcept { voices[(std::size_t) slot].proc.release (S); }

    void newTick() noexcept
    {
        tickPending = true;
        if (listed)
            S.advanceGlobal();
    }

    // FUN_00753c70
    void startGrain (Voice& v, int ipos, int ofs, int a, int h) noexcept
    {
        if (! (ipos <= smp.le) || v.nGrains >= maxGrains)
            return;
        auto& g = v.grains[(std::size_t) v.nGrains++];
        ++v.count;
        g.r.init (smp, loopFlag && smp.ls >= 0, points);
        const auto p = (double) ipos;
        auto d = (double) g.r.a;
        if ((double) g.r.a <= p)
        {
            d = (double) g.r.b;
            if (p <= (double) g.r.b)
                d = p;
        }
        g.r.pos = d;
        g.r.setSpeed ((double) (v.fpitch + v.pofs), ratio);
        g.cnt = -a, g.hold = h, g.end = h + a, g.ofs = ofs;
        g.step = (float) (1.0 / (double) a);
        g.g = g.step;
        auto pan = (float) P.P[7] * 0.015625f;
        pan = std::min (std::max (pan, -1.0f), 1.0f);
        float l, r;
        flvp::panLaw (pan, 1.42f, l, r);
        if ((v.count & 1) == 0)
            g.L = l, g.R = r;
        else
            g.L = r, g.R = l;
    }

    // Voice_Render's slice branch (0x75400e…0x754214): the grains line up with the slice starts. Where the voice will be
    // after the attack (at the grain's speed), the next slice start ahead: a new slice holds its grain for the
    // transient hold (cut so the grain ends at the slice), later grains of the same slice share out the distance left
    // over their hold; `f14` scales the grain and wave spacing by the grain's length over A + H.
    void sliceHold (Voice& v, float& hold, float& f14) noexcept
    {
        const auto cents = v.fpitch + v.pofs;
        auto t = cents == 0.0f ? v.pos + A
                               : (float) ((double) v.pos + (double) A * std::exp ((double) cents * (std::log (2.0) / 12.0 / 100.0)));
        const auto at = (std::int64_t) (std::int32_t) rnd ((double) t - 0.5);
        for (int i = 0; i < nSl; ++i)
        {
            if (sl[(std::size_t) i] < at)
                continue;
            auto d = (float) sl[(std::size_t) i] - t;
            if (i == v.slice)
            {
                auto n = rnd ((double) (d / wsp));
                const auto ni = n < 1 ? 1 : (int) n;
                d = ((d - (float) ni * wsp) * k18c) / (float) ni;
                hold = 0.0f < H + d ? H + d : 0.0f;
            }
            else
            {
                v.slice = i;
                d = d * k18c;
                hold = th;
                if (d < A + th)
                    hold = 0.0f < d - A ? d - A : 0.0f;
            }
            break;
        }
        f14 = (A + hold) * invAH;
    }

    // FUN_00753ea0 (Voice_Render): grains into x (interleaved, cleared); true when the voice is finished
    bool voiceRender (Voice& v, float* x, int n) noexcept
    {
        std::fill (x, x + 2 * n, 0.0f);
        bool ret = false;
        const auto& p = P.P;
        if (! v.done)
        {
            const auto old = v.elapsed;
            v.elapsed += n;
            while (v.next < v.elapsed)
            {
                auto f14 = 1.0f, hold = H;
                auto ip = (int) rnd ((double) v.pos);
                if (! (p[6] < 1 || p[9] != 0 || (std::uint32_t) p[6] <= 0xffu - random (0x100)))
                {
                    ip += (int) random ((std::uint32_t) loopLen);
                    while (smp.le < ip)
                        ip -= loopLen;
                }
                else if (nSl > 0 && wsp > 0.0f)
                    sliceHold (v, hold, f14);
                startGrain (v, ip, v.next - old, (int) rnd ((double) A), (int) rnd ((double) hold));
                if (v.next == 0 && v.pos == 0.0f && v.nGrains > 0)
                {
                    auto& g = v.grains[0];
                    g.hold -= g.cnt, g.end -= g.cnt, g.cnt = 0, g.step = 0.0f, g.g = 1.0f;
                }
                v.next += (int) rnd ((double) (gsp * f14));
                if (p[9] == 0)
                {
                    if (p[4] > 0)
                    {
                        const auto d = rnd (std::fabs (std::sin ((double) v.phase)) * (double) wsp * (double) depth);
                        v.pos = v.pos - (float) d;
                    }
                    v.phase = v.phase + rate5;
                    v.pos = v.pos + wsp * f14;
                    const auto endF = (float) smp.le;
                    if (endF < v.pos)
                    {
                        if (! loopFlag)
                        {
                            v.done = true;
                            if (v.nGrains == 0)
                                ret = true;
                        }
                        else
                            v.looped = true;
                        do
                            v.pos = v.pos - (float) loopLen;
                        while (endF < v.pos);
                    }
                    if (! v.looped)
                        while (v.pos <= 0.0f && v.pos != 0.0f)
                            v.pos = (v.pos + endF) + 1.0f;
                    else
                    {
                        const auto lsF = (float) smp.ls;
                        while (v.pos <= lsF && lsF != v.pos)
                            v.pos = v.pos + (float) loopLen;
                    }
                }
            }
        }
        int gi = 0;
        while (gi < v.nGrains)
        {
            auto& g = v.grains[(std::size_t) gi];
            int m = n, o = 0;
            if (g.ofs > 0)
                o = g.ofs, m = n - g.ofs, g.ofs = 0;
            auto cnt = g.r.render (grainBuf.data(), m, tapL.data(), tapR.data());
            const auto fin = ! g.r.loop && g.r.pos >= (double) g.r.lb;
            int src = 0;
            bool removed = false;
            while (cnt > 0)
            {
                const auto c = g.cnt;
                int len;
                if (c < 0)
                    len = std::min (-c, cnt);
                else if (c < g.hold)
                    len = std::min (g.hold - c, cnt);
                else
                    len = std::min (g.end - c, cnt);
                const auto* xs = grainBuf.data() + 2 * src;
                auto* dst = x + 2 * o;
                if (bitsOf (g.step) == 0)
                    mixConst (xs, dst, len, g.g * g.L, g.g * g.R);
                else
                {
                    mixRampI (xs, dst, len, g.g * g.L, g.g * g.R, g.step * g.L, g.step * g.R);
                    g.g = g.g + (float) len * g.step;
                }
                g.cnt += len;
                cnt -= len;
                if (g.end <= g.cnt || (fin && cnt < 1))
                {
                    for (int j = gi; j + 1 < v.nGrains; ++j)
                        v.grains[(std::size_t) j] = v.grains[(std::size_t) (j + 1)];
                    --v.nGrains;
                    removed = true;
                    if (v.done && v.nGrains == 0)
                        ret = true;
                    break;
                }
                if (g.cnt == g.hold)
                    g.step = (float) (-1.0 / (double) (g.end - g.hold));
                else if (g.cnt == 0)
                    g.step = 0.0f;
                src += len;
                o += len;
            }
            if (! removed)
                ++gi;
        }
        return ret;
    }

    // Gen_Render: voices through the processor, mixed into l/r (overwrites)
    void render (float* l, float* r, int n) noexcept
    {
        flvp::X87Extended guard;
        std::fill (l, l + n, 0.0f);
        std::fill (r, r + n, 0.0f);
        if (count > 0)
            listed = true;
        const auto tick = tickPending;
        tickPending = false;
        if ((int) bufA.size() < 2 * n)
            return;
        for (int i = 0; i < count; ++i)
        {
            auto& v = voices[(std::size_t) order[(std::size_t) i]];
            auto& pr = v.proc;
            if (tick || pr.first > 0)
            {
                pr.control (S);
                v.fpitch = v.fpitchBase + pr.pitch;
            }
            const auto fin = voiceRender (v, bufA.data(), n);
            if (flvp::mixVoice (S, pr, bufA.data(), bufB.data(), n, v.fvol, v.fpan, l, r) || fin)
                v.ended = true;
        }
        for (int i = count - 1; i >= 0; --i)
            if (voices[(std::size_t) order[(std::size_t) i]].ended)
                removeAt (i);
    }

    void removeAt (int i) noexcept
    {
        if (i < 0 || i >= count)
            return;
        used[(std::size_t) order[(std::size_t) i]] = false;
        for (int j = i; j + 1 < count; ++j)
            order[(std::size_t) j] = order[(std::size_t) (j + 1)];
        --count;
    }

    void reset() noexcept
    {
        count = 0;
        used.fill (false);
        listed = false, tickPending = false;
        for (auto& rec : S.recs)
            rec.globalPhase = 0;
    }

    void prepareBlock (int maxBlock)
    {
        const auto m = (std::size_t) std::max (maxBlock, 1);
        bufA.assign (2 * m, 0.0f);
        bufB.assign (2 * m, 0.0f);
        grainBuf.assign (2 * m, 0.0f);
        tapL.assign (2 * maxPoints, 0.0f);
        tapR.assign (2 * maxPoints, 0.0f);
    }

    Params P;
    Sample smp;
    flvp::Settings S;
    std::array<std::uint8_t, snapshotSize> snap = flvp::defaultSnapshot();
    int fs = 44100, maxPoly = 0, start = 0, loopLen = 1;
    float A = 0, H = 0, AH = 0, gsp = 5, wsp = 0, depth = 0, rate5 = 0, th = 0, invAH = 1, k18c = 1;
    float beats = 0;  // the host's beat count (P+0x19c)
    int beatLen = 0;  // samples of `beats` beats at the tempo (P+0x198)
    const std::int64_t* sl = nullptr; // slice starts (the slicer object's list, ascending), owned by the caller
    int nSl = 0;
    int points = flPoints; // the sample reader's interpolation points per side
    double ratio = 1.0, tempo = 140.0;
    bool loopFlag = false, listed = false, tickPending = false;
    std::uint32_t seed = 0;
    std::array<Voice, maxVoices> voices {};
    std::array<bool, maxVoices> used {};
    std::array<int, maxVoices> order {};
    int count = 0;
    std::uint64_t serials = 0;
    std::vector<float> bufA, bufB, grainBuf, tapL, tapR;
};

Sample sampleOf (const SampleData* d) noexcept
{
    Sample s;
    if (d == nullptr || d->frames() == 0)
        return s;
    s.L = d->leftData();
    s.R = d->rightData();
    s.n = (int) std::min<std::size_t> (d->frames(), 0x7fffffff);
    s.rate = (double) std::lround (d->sampleRate); // round(SmpRateConv·host rate): the sample's rate in Hz
    s.ls = 0, s.le = s.n - 1;
    return s;
}
} // namespace

bool granulizerStateUsable (const std::vector<std::uint8_t>& state)
{
    Params p;
    return parse (state, p, nullptr);
}

std::vector<std::uint8_t> granulizerNormalisedState (const std::vector<std::uint8_t>& state, const std::vector<std::uint8_t>& snapshot)
{
    Params p;
    std::vector<std::uint8_t> own;
    if (! parse (state, p, &own))
        return {};
    std::vector<std::uint8_t> out ((std::size_t) stateSize, 0);
    putI32 (out.data(), stateVersion);
    for (int k = 0; k < paramCount; ++k)
        putI32 (out.data() + 4 + 4 * k, p.P[(std::size_t) k]);
    putI32 (out.data() + 52, p.sliceMode);
    putI32 (out.data() + 56, p.keyMode);
    putI32 (out.data() + 60, p.sync == 1 ? 1 : 0); // what the plugin saves: whether sync is on
    const auto defaults = flvp::defaultSnapshot(); // one array: begin() and end() of two temporaries would not be a range
    const auto snap = snapshot.size() == (std::size_t) snapshotSize ? snapshot
                    : own.size() == (std::size_t) snapshotSize    ? own
                                                                  : std::vector<std::uint8_t> (defaults.begin(), defaults.end());
    std::memcpy (out.data() + 64, snap.data(), (std::size_t) snapshotSize);
    return out;
}

std::optional<int> granulizerMode (const std::vector<std::uint8_t>& state, int which)
{
    Params p;
    if (! parseNormalised (state, p))
        return std::nullopt;
    switch (which)
    {
        case 0: return p.sliceMode;
        case 1: return p.keyMode;
        case 2: return p.sync == 1 ? 1 : 0;
        default: return std::nullopt;
    }
}

bool granulizerSetMode (std::vector<std::uint8_t>& state, int which, int value)
{
    Params p;
    if (which < 0 || which > 2 || ! parseNormalised (state, p))
        return false;
    const int offset[] = { 52, 56, 60 }, high[] = { 2, 3, 1 };
    putI32 (state.data() + offset[which], std::clamp (value, 0, high[which]));
    return true;
}

int granulizerParamCount() noexcept { return paramCount; }

std::pair<int, int> granulizerParamRange (int index) noexcept
{
    return index >= 0 && index < paramCount ? ranges[(std::size_t) index] : std::pair<int, int> { 0, 0 };
}

std::optional<int> granulizerParam (const std::vector<std::uint8_t>& state, int index)
{
    Params p;
    if (index < 0 || index >= paramCount || ! parseNormalised (state, p))
        return std::nullopt;
    return p.P[(std::size_t) index];
}

bool granulizerSetParam (std::vector<std::uint8_t>& state, int index, int value)
{
    Params p;
    if (index < 0 || index >= paramCount || ! parseNormalised (state, p))
        return false;
    const auto r = ranges[(std::size_t) index];
    putI32 (state.data() + 4 + 4 * index, std::clamp (value, r.first, r.second));
    return true;
}

namespace
{
// ------------------------------------------------------------------------------------------------------- instrument

struct Setup
{
    Params P;
    std::array<std::uint8_t, snapshotSize> snap = flvp::defaultSnapshot();
    int ppq = 96;
    double tempo = 140.0;
    float beats = 0.0f;
};

// What FL hands the plugin as the sample's beat count (Dispatcher 0x13): the WAV's ACID beats, else the channel's
// time-stretch TIME in beats — without the MUL factor (the importer keeps TIME/192 for this engine) [FL render g11, g12,
// g16: REA_rodada165g-flgran165 §3]
float hostBeats (const ChannelDef& channel, const SampleData* sample) noexcept
{
    if (sample != nullptr && sample->acidBeats > 0.0f)
        return sample->acidBeats;
    return std::max (channel.sampler.stretchBeats, 0.0f);
}

class GranulizerInstrument final : public Instrument
{
public:
    explicit GranulizerInstrument (std::shared_ptr<const SampleData> s) : sample (std::move (s))
    {
        engine.prepareBlock (blockSize);
        engine.smp = sampleOf (sample.get());
        engine.init (Params {}, 44100.0, flvp::defaultSnapshot().data(), 96, 140.0);
    }

    ~GranulizerInstrument() override
    {
        delete incoming.exchange (nullptr);
        delete retired.exchange (nullptr);
    }

    void prepare (double sr) noexcept override
    {
        rate = sr;
        engine.points = offlineRendering() ? flPoints : defaultPoints; // FL: 32-point sinc when it renders, 24 live
        take();
        engine.tempo = current.tempo;
        engine.init (current.P, sr, current.snap.data(), current.ppq, current.tempo);
        engine.reset();
        tickPhase = 0.0;
        lastTicks = -1.0;
    }

    void configure (const ChannelDef& channel, double bpm) noexcept override
    {
        delete retired.exchange (nullptr);
        pitchCents.store (channel.sampler.pitch * 100.0f);
        maxPoly.store (channel.synth.mono ? 1 : 0);
        const auto ppq = channel.modelParams.empty() ? 96 : std::clamp ((int) channel.modelParams[0], 1, 1 << 16);
        const auto tempo = bpm > 0.0 ? bpm : 140.0;
        const auto beats = hostBeats (channel, sample.get());
        if (channel.pluginState == state && ppq == lastPpq && tempo == lastTempo && beats == lastBeats)
            return;
        Params p;
        if (! parseNormalised (channel.pluginState, p))
            return;
        prepareSlices (p.sliceMode);
        auto s = std::make_unique<Setup>();
        s->P = p;
        std::memcpy (s->snap.data(), channel.pluginState.data() + 64, snapshotSize);
        s->ppq = ppq, s->tempo = tempo, s->beats = beats;
        state = channel.pluginState, lastPpq = ppq, lastTempo = tempo, lastBeats = beats;
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
        const auto initVol = (float) ((std::pow (11.0, 1.28 * vel) - 1.0) / 10.0);
        const auto k = std::clamp (key, 0, 127);
        const auto finalPitch = (float) (100.0 * (k - 60) + (double) pitchCents.load() + (double) nextCents);
        const auto slot = engine.trigger (k, (float) (100.0 * k), finalPitch, (float) (channelVol * (double) initVol),
                                          (float) std::clamp ((double) nextPan + channelPan, -1.0, 1.0));
        if (slot >= 0)
        {
            volFactor[(std::size_t) slot] = initVol;
            notePan[(std::size_t) slot] = nextPan;
        }
        nextPan = 0.0f;
        nextCents = 0.0f;
    }

    void noteOff (int key) noexcept override
    {
        for (int i = 0; i < engine.count; ++i)
        {
            const auto slot = engine.order[(std::size_t) i];
            const auto& v = engine.voices[(std::size_t) slot];
            if (v.key == key && ! v.proc.released)
                engine.release (slot);
        }
    }

    void allNotesOff() noexcept override
    {
        for (int i = 0; i < engine.count; ++i)
        {
            const auto slot = engine.order[(std::size_t) i];
            if (! engine.voices[(std::size_t) slot].proc.released)
                engine.release (slot);
        }
    }

    bool idle() const noexcept override { return engine.count == 0; }

    void setSongPosition (double ticks) noexcept override
    {
        const auto playing = lastTicks >= 0.0 && ticks != lastTicks;
        lastTicks = ticks;
        if (playing)
            tickPhase = ticks / (double) ppq * (double) current.ppq;
    }

    void render (float* left, float* right, int n) noexcept
    {
        renderImpl (left, right, n);
    }

    void render (float* left, float* right, int n, float gl, float gr) noexcept override
    {
        take();
        engine.maxPoly = maxPoly.load();
        // FL's channel volume/pan from LYNX's gains (as 3x Osc)
        const auto vol = std::sqrt (((double) gl * gl + (double) gr * gr) * 0.5);
        auto pan = vol > 0.0 ? std::atan2 ((double) gr, (double) gl) * (4.0 / 3.141592653589793) - 1.0 : 0.0;
        if (std::fabs (pan) < 1e-9)
            pan = 0.0;
        if (vol != channelVol || pan != channelPan)
        {
            channelVol = vol, channelPan = pan;
            for (int i = 0; i < engine.count; ++i)
            {
                const auto slot = engine.order[(std::size_t) i];
                auto& v = engine.voices[(std::size_t) slot];
                v.fvol = (float) (vol * (double) volFactor[(std::size_t) slot]);
                v.fpan = (float) std::clamp ((double) notePan[(std::size_t) slot] + pan, -1.0, 1.0);
            }
        }
        renderImpl (left, right, n);
    }

private:
    // The slice list of a slice mode (FUN_007548c0), built once per instrument before a Setup that uses it is published
    // (the audio thread only reads it): 1 = the WAV's cue points / regions, 2 = FL's auto slicing
    void prepareSlices (int mode)
    {
        if (sample == nullptr || sample->frames() == 0)
            return;
        if (mode == 1 && ! slicesReady[0])
        {
            slices[0] = sample->markerStarts;
            slicesReady[0] = true;
        }
        else if (mode == 2 && ! slicesReady[1])
        {
            const auto* l = sample->leftData();
            const auto* r = sample->rightData();
            slices[1] = flslice::autoSlice (l, r == l ? nullptr : r, (std::int64_t) sample->frames(), sample->sampleRate);
            slicesReady[1] = true;
        }
    }

    void useSlices (int mode) noexcept
    {
        const auto* list = mode == 1 ? &slices[0] : mode == 2 ? &slices[1] : nullptr;
        engine.sl = list != nullptr && ! list->empty() ? list->data() : nullptr;
        engine.nSl = engine.sl != nullptr ? (int) std::min<std::size_t> (list->size(), 0x7fffffff) : 0;
    }

    void renderImpl (float* left, float* right, int n) noexcept
    {
        const auto perSample = current.tempo * (double) current.ppq / (60.0 * rate);
        for (int done = 0; done < n;)
        {
            if (std::fabs (tickPhase - std::round (tickPhase)) < 1e-9)
            {
                tickPhase = std::round (tickPhase);
                engine.newTick();
            }
            const auto next = std::floor (tickPhase) + 1.0;
            const auto toNext = std::max (1, (int) std::ceil ((next - tickPhase) / perSample - 1e-9));
            const auto len = std::min ({ blockSize, n - done, toNext });
            engine.render (bufL.data(), bufR.data(), len);
            for (int j = 0; j < len; ++j)
            {
                left[done + j] += bufL[(std::size_t) j];
                right[done + j] += bufR[(std::size_t) j];
            }
            tickPhase = len == toNext ? next : tickPhase + (double) len * perSample;
            done += len;
        }
    }

    void take() noexcept
    {
        if (retired.load() != nullptr)
            return;
        if (auto* s = incoming.exchange (nullptr))
        {
            current = *s;
            engine.P = current.P;
            engine.tempo = current.tempo;
            engine.beats = current.beats;
            useSlices (current.P.sliceMode);
            std::memcpy (engine.snap.data(), current.snap.data(), snapshotSize);
            engine.timing (current.ppq, current.tempo);
            engine.update();
            retired.store (s);
        }
    }

    static constexpr int blockSize = 512;
    static constexpr double ppq = 96.0; // LYNX's song ticks per beat (setSongPosition)
    std::shared_ptr<const SampleData> sample;
    Engine engine;
    Setup current;
    std::atomic<Setup*> incoming { nullptr }, retired { nullptr };
    std::atomic<float> pitchCents { 0.0f };
    std::atomic<int> maxPoly { 0 };
    std::vector<std::uint8_t> state;
    int lastPpq = -1;
    double lastTempo = -1.0;
    float lastBeats = -1.0f;
    std::array<std::vector<std::int64_t>, 2> slices; // written once each (configure), then read by the audio thread
    std::array<bool, 2> slicesReady { false, false };
    std::array<float, blockSize> bufL {}, bufR {};
    std::array<float, maxVoices> volFactor {}, notePan {};
    double rate = 44100.0, channelVol = 1.0, channelPan = 0.0, tickPhase = 0.0, lastTicks = -1.0;
    float nextPan = 0.0f, nextCents = 0.0f;
};
} // namespace

std::unique_ptr<Instrument> makeGranulizerInstrument (std::shared_ptr<const SampleData> sample)
{
    return std::make_unique<GranulizerInstrument> (std::move (sample));
}

namespace granulizer
{
std::vector<float> renderReference (const std::vector<std::uint8_t>& state, const SampleData& sample, const std::vector<RefNote>& notes,
                                    double sampleRate, int length, const RefOptions& opt)
{
    Params P;
    if (! parseNormalised (state, P) || length <= 0)
        return {};
    auto engine = std::make_unique<Engine>();
    engine->smp = sampleOf (&sample);
    engine->tempo = opt.tempo;
    engine->beats = opt.beats;
    engine->points = opt.points;
    if (P.sliceMode > 0 && ! opt.slices.empty())
    {
        engine->sl = opt.slices.data();
        engine->nSl = (int) opt.slices.size();
    }
    engine->init (P, sampleRate, state.data() + 64, opt.ppq, opt.tempo);
    engine->maxPoly = opt.maxPoly;
    engine->seed = opt.seed;
    const auto block = std::max (1, opt.block);
    engine->prepareBlock (block);
    std::vector<int> slot (notes.size(), -1);
    std::vector<std::uint64_t> serial (notes.size(), 0);
    std::vector<float> out (2 * (std::size_t) length, 0.0f), l ((std::size_t) block), r ((std::size_t) block);
    std::vector<std::pair<int, int>> order;
    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        order.push_back ({ notes[i].start, 2 * (int) i });
        if (notes[i].release >= 0)
            order.push_back ({ notes[i].release, 2 * (int) i + 1 });
    }
    std::stable_sort (order.begin(), order.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
    int n0 = 0;
    std::size_t next = 0;
    while (n0 < length)
    {
        auto ln = std::min (block, length - n0);
        for (const auto& e : order)
            if (n0 < e.first && e.first < n0 + ln)
                ln = e.first - n0;
        if (opt.tick > 0)
        {
            const auto nt = (n0 / opt.tick + 1) * opt.tick;
            if (nt < n0 + ln)
                ln = nt - n0;
            if (n0 % opt.tick == 0)
            {
                for (int i = 0; i < engine->count; ++i)
                {
                    auto& v = engine->voices[(std::size_t) engine->order[(std::size_t) i]];
                    v.fpitch = v.fpitchBase;
                }
                engine->newTick();
            }
        }
        while (next < order.size() && order[next].first < n0)
            ++next;
        while (next < order.size() && order[next].first == n0)
        {
            const auto i = (std::size_t) (order[next].second / 2);
            const auto& nt = notes[i];
            if (order[next].second % 2 == 0)
            {
                slot[i] = engine->trigger (nt.key, (float) (100.0 * nt.key), (float) (100.0 * nt.key - 6000.0 + nt.pitchOffset),
                                           (float) nt.finalVol, (float) nt.finalPan);
                if (slot[i] >= 0)
                    serial[i] = engine->voices[(std::size_t) slot[i]].serial;
            }
            else if (slot[i] >= 0 && engine->used[(std::size_t) slot[i]] && engine->voices[(std::size_t) slot[i]].serial == serial[i])
                engine->release (slot[i]);
            ++next;
        }
        engine->render (l.data(), r.data(), ln);
        for (int j = 0; j < ln; ++j)
        {
            out[2 * (std::size_t) (n0 + j)] = l[(std::size_t) j];
            out[2 * (std::size_t) (n0 + j) + 1] = r[(std::size_t) j];
        }
        n0 += ln;
    }
    return out;
}
} // namespace granulizer
} // namespace lynx
