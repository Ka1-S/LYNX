// FL Slayer's engine (REA_rodada68). Clean-room: our own code from the documented behaviour; field names are ours.
#include "core/Slayer.h"
#include "core/HostSlide.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>

#include "core/FlVoiceProcessor.h" // flvp::panLaw (FL's ComputeLRVol law)
#include "core/Sha256.h"

// The engine switches the x87 rounding mode (§2.1): the compiler must not assume round-to-nearest.
#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#elif defined(__GNUC__)
#pragma GCC optimize ("rounding-math")
#endif

namespace lynx
{
namespace
{
using ld = long double;

// ------------------------------------------------------------------------------------------------ x87 (§2.1)
// The engine runs on the x87 with 64-bit precision; processReplacing switches to truncating rounding for its whole
// duration. long double is the x87 format on the targets LYNX builds for; the guards set the control word.
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
struct X87Mode
{
    unsigned short saved = 0;
    explicit X87Mode (bool chop) noexcept
    {
        asm volatile ("fnstcw %0" : "=m"(saved));
        unsigned short cw = (unsigned short) ((saved & ~0x0F00) | 0x0300 | (chop ? 0x0C00 : 0));
        asm volatile ("fldcw %0" : : "m"(cw) : "memory");
    }
    ~X87Mode() { asm volatile ("fldcw %0" : : "m"(saved) : "memory"); }
};
inline ld fsin (ld x) noexcept
{
    asm ("fsin" : "+t"(x));
    return x;
}
inline ld fcos (ld x) noexcept
{
    asm ("fcos" : "+t"(x));
    return x;
}
// the compiler's inline exp: x·log2(e) split by frndint (current rounding mode), f2xm1, fscale
inline ld fexp (ld x) noexcept
{
    ld r;
    asm ("fldl2e\n\t"
         "fmulp\n\t"
         "fld %%st(0)\n\t"
         "frndint\n\t"
         "fxch %%st(1)\n\t"
         "fsub %%st(1), %%st\n\t"
         "f2xm1\n\t"
         "fld1\n\t"
         "faddp\n\t"
         "fscale\n\t"
         "fstp %%st(1)"
         : "=t"(r)
         : "0"(x));
    return r;
}
// fistp with the current rounding mode
inline int fistp (ld x) noexcept
{
    int r;
    asm ("fistpl %0" : "=m"(r) : "t"(x) : "st");
    return r;
}
#else
struct X87Mode
{
    explicit X87Mode (bool) noexcept {}
};
inline ld fsin (ld x) noexcept { return std::sin (x); }
inline ld fcos (ld x) noexcept { return std::cos (x); }
inline ld fexp (ld x) noexcept { return std::exp (x); }
inline int fistp (ld x) noexcept { return (int) std::nearbyint (x); }
#endif

#if (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__) && ! defined(__clang__)
// GCC (not Clang) narrows a float-to-long-double product stored back to float into an SSE operation, which rounds by
// MXCSR (to nearest) instead of the x87 control word; the stores go through the FPU explicitly (REA_rodada79 §5)
inline float F (ld x) noexcept
{
    float r;
    asm ("fstps %0" : "=m"(r) : "t"(x) : "st");
    return r;
}
inline double D (ld x) noexcept
{
    double r;
    asm ("fstpl %0" : "=m"(r) : "t"(x) : "st");
    return r;
}
#else
inline float F (ld x) noexcept { return (float) x; }  // fstp dword (current rounding mode)
inline double D (ld x) noexcept { return (double) x; } // fstp qword
#endif
inline int ftol (ld x) noexcept { return (int) x; }    // the C runtime's __ftol (truncation)
inline float bitsF (std::uint32_t u) noexcept
{
    float f;
    std::memcpy (&f, &u, 4);
    return f;
}

// constants of the engine (float unless noted)

// ------------------------------------------------------------------------------------------------ installation data
constexpr const char* engineSha256 = "84b64c43cc6528401e1c4e84074785e100e8453c4db4c8322a5d5f608de724a9";

struct DllData
{
    std::array<std::vector<std::int16_t>, 29> picks; // recorded excitations by table index (count, then samples)
    std::array<std::int8_t, 72> chords {};           // 12 roots × 6 strings, −1 ends a chord (offsets from note 48)
    std::array<float, 7> glideA {}, glideB {};        // glide tables of the two solo modes
};

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
    std::uint32_t imageBase = 0;
    rd (pe + 24 + 28, imageBase);
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
    auto offset = [&] (std::uint32_t va) -> std::size_t {
        const auto rva = va - imageBase;
        for (const auto& s : secs)
            if (rva >= s.va && rva < s.va + std::min (s.vs, s.rs))
                return (std::size_t) (s.ro + rva - s.va);
        return b.size();
    };
    auto t = std::make_shared<DllData>();
    // pick tables (§3.4): table index → address in the engine's data; each a 16-bit count and that many samples
    static constexpr std::pair<int, std::uint32_t> tables[] = { { 1, 0x100179b8 }, { 7, 0x10018b48 }, { 14, 0x100199c8 },
        { 15, 0x1001a854 }, { 16, 0x1001b718 }, { 17, 0x1001c5e0 }, { 18, 0x1001d1b8 }, { 19, 0x1001da80 }, { 20, 0x1001e5e0 },
        { 21, 0x1001f000 }, { 22, 0x1001fc10 }, { 24, 0x10020324 }, { 26, 0x10021278 }, { 27, 0x10021f74 }, { 28, 0x10022c1c } };
    for (const auto& [index, va] : tables)
    {
        const auto at = offset (va);
        std::int16_t count = 0;
        rd (at, count);
        if (count <= 2 || at + 2 + 2 * (std::size_t) count > b.size())
            return nullptr;
        auto& v = t->picks[(std::size_t) index];
        v.resize ((std::size_t) count + 1);
        std::memcpy (v.data(), b.data() + at, 2 * v.size());
    }
    const auto ch = offset (0x10015318);
    if (ch + 72 > b.size())
        return nullptr;
    std::memcpy (t->chords.data(), b.data() + ch, 72);
    for (int i = 0; i < 7; ++i)
    {
        rd (offset (0x100152e0) + 4 * (std::size_t) i, t->glideA[(std::size_t) i]);
        rd (offset (0x100152fc) + 4 * (std::size_t) i, t->glideB[(std::size_t) i]);
    }
    cache[path] = t;
    return t;
}

// ------------------------------------------------------------------------------------------------ the engine
constexpr int ringMask = 0x7fff;

struct Str // one string (§3)
{
    std::uint8_t pick = 0;          // recorded excitation table
    float pickRate = 0, pickGain = 0;
    float note = 0, vel = 0, velHeld = 0;
    std::int8_t detune = 0;
    std::uint8_t held = 0;          // key still down
    float env = 0, envPhase = 0, envStart = 0; // exciter envelope: output, phase, level at release
    float gate = 0;                 // 1 while sounding, 0 released
    std::uint8_t envState = 0;      // 4 attack, 3 decay, 2 rest, 1 release, 0 off
    float level = 0, envCopy = 0;   // exciter level (ramped), envelope value seen by the strings
    int idxA = 0, idxC = 0;
    std::vector<float> ringA, ringB, ringC; // the two delay lines and the pickup line (0x8000 each)
    float lpA = 0, slide = 0, lpC = 0;
    float releaseTimer = 0, pickPos = 0;
    int age = 0;
    std::array<float, 4> hist {};   // last four internal samples (newest first)
    float phase = 0;                // resampler phase
    int delay = 0;                  // chunks before the string starts (strum)
    Str() : ringA ((std::size_t) ringMask + 1), ringB ((std::size_t) ringMask + 1), ringC ((std::size_t) ringMask + 1) {}
    Str (std::vector<float> a, std::vector<float> b, std::vector<float> c) : ringA (std::move (a)), ringB (std::move (b)), ringC (std::move (c))
    {
        std::fill (ringA.begin(), ringA.end(), 0.0f);
        std::fill (ringB.begin(), ringB.end(), 0.0f);
        std::fill (ringC.begin(), ringC.end(), 0.0f);
    }
};

struct Event
{
    int delta = 0, kind = 1, note = 0, detune = 0, cc = 0, value = 0;
    float fvalue = 0;
};

struct Cabinet // §5.3: high-pass one-pole, then a low shelf and a high shelf (left channel only is used)
{
    std::array<float, 0xd4 / 4> c {}; // working fields at their original byte offsets
    float& C (int off) noexcept { return c[(std::size_t) off / 4]; }
    bool on = true;
};

struct Engine
{
    explicit Engine (std::shared_ptr<const DllData> d);

    // VST side
    void setParameter (int i, float v) noexcept;
    void midi (int status, int d1, int d2, int detune = 0) noexcept;
    void setSampleRate (float sr) noexcept;
    void resume() noexcept;
    void process (float* outL, float* outR, int n) noexcept;
    double tempo = 120.0;

    // internals
    void reset() noexcept;
    void cabinetRate (float sr) noexcept;
    void cabinetSet (int i, float v) noexcept;
    void cabinetCoefs() noexcept;
    void cabinetSection (int s, float freq, float gain, float q) noexcept;
    void cabinetProcess (float* x, int n) noexcept;
    void handle (const Event& e) noexcept;
    void noteOn (int key, float vel, int detune) noexcept;
    void noteOff (int key, float vel) noexcept;
    void trigger (int key, float vel, int detune, int delay) noexcept;
    void release (int key, bool latched) noexcept;
    void releaseAny (int key) noexcept;
    Str& choose (int key) noexcept;
    int pushHeld (int key) noexcept;
    int popHeld (int key) noexcept;
    void envelopes() noexcept;
    void strings (float* out, int n) noexcept;
    void effects (float* l, float* r, int n) noexcept;
    void biquad (int type, float gain, float freq, float sr, float bw, int idx) noexcept;
    ld eqSection (float x, int idx) noexcept;
    ld ampFilter (int base, float x, float a, float b, float clampA) noexcept;
    ld ampClean (float x, float drive, float vol, float sr) noexcept;
    ld ampDrive (float x, float drive, float vol, float sr) noexcept;
    ld ampFuzz (float x, float drive, float vol, float sr) noexcept;
    ld phaser (float x, float lfo) noexcept;

    // the engine's working fields at their original byte offsets below 0x868 (named accessors where it helps)
    std::array<float, 0x868 / 4> m {};
    float& M (int off) noexcept { return m[(std::size_t) off / 4]; }

    std::shared_ptr<const DllData> data;
    std::array<float, 28> prm {};
    float& P (int i) noexcept { return prm[(std::size_t) i]; }
    int flags = 0, prevFlags = 1; // bit 0 sustain (CC 64), bit 1 latch (parameter 26)
    float rate = 44100.0f, rateRatio = 1.0f, tempoF = 120.0f;
    float volume = 1.0f, expression = 1.0f; // CC 7, CC 11
    int idle = 5, lastMode = -1;
    std::int16_t lastKey = 0;
    std::uint16_t active = 1;
    std::uint8_t pickActive = 0;
    std::array<float, 255> detuneTab {}; // 1.00057779^k, k = -127…127 (index k + 127)
    std::array<float, 4> fxRate { 0.25f, 0.5f, 1.0f, 2.0f };
    std::vector<float> fbRing;                          // 0x8000 (feedback path, written by the amp)
    std::vector<float> bufA, bufB, bufC, bufD, bufE;    // 0x40000 each (effects; bufE: amp delay)
    int cnt8c = 0, cnt90 = 0, cnt94 = 0, ampIdx = 0, fbWrite = 0, fbRead = 0;
    float lfoE = 0, envA = 0, envB = 0, envC = 0, envD = 0, tapA = 0, tapB = 0, tapC = 0, tapD = 0;
    std::array<Str, 6> str;
    Cabinet cab;
    // event queue (1024 entries) and the chunk counter
    std::array<Event, 1024> queue {};
    int qHead = 0, qTail = 0, qCount = 0;
    float chunkCounter = 0.0f;
    // held keys of the solo modes (0x80 keys, minimum, maximum, count)
    std::array<std::int8_t, 0x84> heldKeys {};
    int heldCount = 0;
    // the front-end's view (not engine state): the parameter integers and the pitch bend it last sent
    std::array<int, 29> sentParams {};
    int sentBend = 0x2000;
    int strumCount = 0, strumChunks = 0, glideTarget = 0;
    float glidePos = 0.0f, glideStep = 0.0f;
    std::uint32_t randState = 1; // the C runtime's per-thread rand() seed
    int rnd() noexcept
    {
        randState = randState * 0x343fdu + 0x269ec3u;
        return (int) ((randState >> 16) & 0x7fff);
    }
    std::vector<float> bufL, bufR;
    // debugging aid for the verification harness: a field by its offset in the original object (§7)
    double peek (int off) noexcept;
};

// ------------------------------------------------------------------------------------------------ cabinet (§5.3)
// shelving section s (even: low shelf, odd: high shelf) with the shelf slope S
void Engine::cabinetSection (int s, float freq, float gain, float slope) noexcept
{
    auto& C = cab;
    if (slope < bitsF (0x3c23d70a)) // 0.01
        slope = bitsF (0x3c23d70a);
    if ((ld) freq + (ld) freq > (ld) C.C (0x1c))
        freq = F ((ld) C.C (0x1c) * (ld) 0.49000000953674316f);
    const ld A = (ld) std::pow (10.0, D ((ld) gain * (ld) 0.02500000037252903f)); // the C runtime's pow in double
    const ld w = (ld) freq * (ld) 6.2831854820251465f / (ld) C.C (0x1c);
    const double sn = D (fsin (w));
    const ld cs = fcos (w);
    const ld am1e = A - (ld) 1.0;
    const double am1 = D (am1e);
    const ld beta = std::sqrt ((A * A + (ld) 1.0) / (ld) slope - am1e * am1e);
    ld b0, b1, b2, a0, a1, a2;
    if ((s & 1) == 0)
    {
        const double ap1 = D (A + (ld) 1.0);
        const ld am1csE = (ld) am1 * cs;
        const double am1cs = D (am1csE);
        const double t34 = D ((ld) ap1 - am1csE);
        const ld bsE = beta * (ld) sn;
        const double bs = D (bsE);
        b0 = (ld) D ((bsE + (ld) t34) * A);
        const double ap1cs = D ((ld) ap1 * cs);
        b1 = (ld) D (((ld) am1 - (ld) ap1cs) * A * (ld) 2.0);
        b2 = ((ld) t34 - (ld) bs) * A;
        a0 = (ld) bs + (ld) am1cs + (ld) ap1;
        a1 = ((ld) ap1cs + (ld) am1) * (ld) -2.0;
        a2 = (ld) D ((ld) am1cs + (ld) ap1 - (ld) bs);
    }
    else
    {
        const double am1cs = D ((ld) am1 * cs);
        const double bs = D (beta * (ld) sn);
        const ld ap1e = A + (ld) 1.0;
        const double ap1 = D (ap1e);
        b0 = (ld) D (((ld) bs + (ld) am1cs + ap1e) * A);
        const double ap1cs = D (ap1e * cs);
        b1 = (ld) D (((ld) ap1cs + (ld) am1) * A * (ld) -2.0);
        b2 = ((ld) am1cs + (ld) ap1 - (ld) bs) * A;
        const ld t34e = (ld) ap1 - (ld) am1cs;
        a0 = t34e + (ld) bs;
        a1 = ((ld) am1 - (ld) ap1cs) * (ld) 2.0;
        a2 = (ld) D ((ld) D (t34e) - (ld) bs);
    }
    const int o = 0x44 + s * 0x24;
    C.C (o) = F (b0 / a0);
    C.C (o + 4) = F (b1 / a0);
    C.C (o + 8) = F (b2 / a0);
    C.C (o + 12) = F (a1 / a0);
    C.C (o + 16) = F (a2 / a0);
}

void Engine::cabinetCoefs() noexcept
{
    auto& C = cab;
    for (int s = 0; s < 4; s += 2)
    {
        const auto g = F ((ld) C.C (0x2c) * (ld) 24.0f);
        const auto f = F ((ld) C.C (0x28) * (ld) 400.0f);
        cabinetSection (s, f, g, C.C (0x30));
        const auto g2 = F ((ld) C.C (0x38) * (ld) 24.0f);
        const auto f2 = F ((ld) C.C (0x34) * (ld) 5000.0f + (ld) 7500.0f);
        cabinetSection (s + 1, f2, g2, 1.0f);
    }
}

void Engine::cabinetRate (float sr) noexcept
{
    if (cab.C (0x1c) == sr)
        return;
    cab.C (0x1c) = sr;
    cabinetCoefs();
}

void Engine::cabinetSet (int i, float v) noexcept
{
    if (i < 0 || i > 6)
        return;
    cab.C (0x20 + 4 * i) = v;
    if (i > 1)
        cabinetCoefs();
}

void Engine::cabinetProcess (float* x, int n) noexcept
{
    auto& C = cab;
    if (! C.on)
        return;
    const float k = F ((ld) 176.40000915527344f / (ld) C.C (0x1c));
    const ld tiny = (ld) bitsF (0x219392ef);
    for (int i = 0; i < n; ++i)
    {
        const ld in = (ld) x[i] + tiny;
        const ld t = (in - (ld) C.C (0x3c)) * (ld) k + (ld) C.C (0x3c);
        C.C (0x3c) = F (t);
        const ld u = (in - t) + tiny;
        ld y = (ld) C.C (0x58) * (ld) C.C (0x48) + u * (ld) C.C (0x44);
        y = y + (ld) C.C (0x4c) * (ld) C.C (0x5c);
        y = y - (ld) C.C (0x60) * (ld) C.C (0x50);
        y = y - (ld) C.C (0x64) * (ld) C.C (0x54);
        C.C (0x5c) = C.C (0x58);
        C.C (0x64) = C.C (0x60);
        const float y1 = F (y);
        C.C (0x58) = F (u);
        C.C (0x60) = y1;
        ld z = (ld) C.C (0x80) * (ld) C.C (0x70) + (ld) C.C (0x6c) * (ld) C.C (0x7c);
        z = z + (ld) y1 * (ld) C.C (0x68);
        z = z - (ld) C.C (0x74) * (ld) C.C (0x84);
        z = z - (ld) C.C (0x78) * (ld) C.C (0x88);
        C.C (0x80) = C.C (0x7c);
        C.C (0x88) = C.C (0x84);
        const float y2 = F (z);
        C.C (0x7c) = y1;
        C.C (0x84) = y2;
        x[i] = y2;
    }
}

// ------------------------------------------------------------------------------------------------ amp section helpers (§5)
// the 3-band EQ's biquads (index 0…2): RBJ peaking (type 4) and the shelves (5 low, 6 high) as the engine computes them
void Engine::biquad (int type, float gain, float freq, float sr, float bw, int idx) noexcept
{
    const double A = std::pow (10.0, D ((ld) gain * (ld) 0.02500000037252903f));
    const ld w = (ld) freq * (ld) 6.2831854820251465f / (ld) sr;
    const double wD = D (w);
    const double sn = D (fsin (w));
    const double cs = D (fcos (w));
    ld b0, b1, b2, a0, a1, a2;
    if (type == 4)
    {
        const double alpha = D ((ld) std::sinh (D ((ld) bw * (ld) 0.3465735912322998f * (ld) wD / (ld) sn)) * (ld) sn);
        const ld aA = (ld) alpha * (ld) A;
        b0 = (ld) D (aA + (ld) 1.0);
        const double m2cs = D ((ld) cs * (ld) -2.0);
        b1 = (ld) m2cs;
        b2 = (ld) D ((ld) 1.0 - aA);
        const double adA = D ((ld) alpha / (ld) A);
        a0 = (ld) adA + (ld) 1.0;
        a1 = (ld) m2cs;
        a2 = (ld) D ((ld) 1.0 - (ld) adA);
    }
    else if (type == 5)
    {
        const double sq = D (std::sqrt ((ld) A + (ld) A));
        const double ap1 = D ((ld) A + (ld) 1.0);
        const ld am1e = (ld) A - (ld) 1.0;
        const double am1 = D (am1e);
        const ld am1csE = am1e * (ld) cs;
        const double am1cs = D (am1csE);
        const ld d = (ld) ap1 - am1csE;
        const ld sqsnE = (ld) sq * (ld) sn;
        const double sqsn = D (sqsnE);
        b0 = (ld) D ((sqsnE + d) * (ld) A);
        const double ap1cs = D ((ld) ap1 * (ld) cs);
        b1 = (ld) D (((ld) am1 - (ld) ap1cs) * (ld) A * (ld) 2.0);
        b2 = (ld) D ((d - sqsnE) * (ld) A);
        a0 = (ld) sqsn + (ld) am1cs + (ld) ap1;
        a1 = ((ld) ap1cs + (ld) am1) * (ld) -2.0;
        a2 = (ld) D ((ld) am1cs + (ld) ap1 - (ld) sqsn);
    }
    else // 6
    {
        const double sq = D (std::sqrt ((ld) A + (ld) A));
        const ld am1e = (ld) A - (ld) 1.0;
        const double am1 = D (am1e);
        const ld am1cs = am1e * (ld) cs;
        const double sqsn = D ((ld) sq * (ld) sn);
        const ld ap1 = (ld) A + (ld) 1.0;
        b0 = (ld) D (((ld) sqsn + am1cs + ap1) * (ld) A);
        const ld ap1csE = ap1 * (ld) cs;
        const double ap1cs = D (ap1csE);
        b1 = (ld) D ((ap1csE + (ld) am1) * (ld) A * (ld) -2.0);
        b2 = (ld) D ((am1cs + ap1 - (ld) sqsn) * (ld) A);
        const ld d = ap1 - am1cs;
        const double dD = D (d);
        a0 = d + (ld) sqsn;
        a1 = ((ld) am1 - (ld) ap1cs) * (ld) 2.0;
        a2 = (ld) D ((ld) dD - (ld) sqsn);
    }
    const int i = idx;
    M (0xb0 + 4 * i) = F (b0 / a0);
    M (0xd8 + 4 * i) = F (b1 / a0);
    M (0x100 + 4 * i) = F (b2 / a0);
    M (0x128 + 4 * i) = F (a1 / a0);
    M (0x150 + 4 * i) = F (a2 / a0);
}

ld Engine::eqSection (float x, int i) noexcept
{
    const auto x1 = M (0x178 + 4 * i);
    ld y = (ld) M (0xd8 + 4 * i) * (ld) x1 + (ld) x * (ld) M (0xb0 + 4 * i);
    y = y + (ld) M (0x100 + 4 * i) * (ld) M (0x1a0 + 4 * i);
    y = y - (ld) M (0x128 + 4 * i) * (ld) M (0x1c8 + 4 * i);
    y = y - (ld) M (0x150 + 4 * i) * (ld) M (0x1f0 + 4 * i);
    M (0x1a0 + 4 * i) = x1;
    M (0x178 + 4 * i) = x;
    M (0x1f0 + 4 * i) = M (0x1c8 + 4 * i);
    M (0x1c8 + 4 * i) = F (y);
    return y;
}

// the three EQ sections in a row (each call takes its input as a float)
#define SLAYER_EQ3(v) eqSection (F (eqSection (F (eqSection ((v), 0)), 1)), 2)

// one of the amp's 4-pole filters with cubic soft clipping (base 0x4dc, 0x4fc, 0x51c; clamp of `a`: none, 0.9, 1.0)
ld Engine::ampFilter (int base, float x, float a, float b, float clampA) noexcept
{
    ld c;
    float dF;
    if (clampA <= 0.0f)
    {
        c = (ld) a * (ld) 3.5999999046325684f - (ld) a * (ld) a * (ld) 1.600000023841858f - (ld) 1.0f;
        dF = F ((c + (ld) 1.0f) * (ld) 0.5f);
    }
    else
    {
        ld ac = (ld) a;
        if (ac > (ld) clampA)
            ac = (ld) clampA;
        c = ac * (ld) 3.5999999046325684f - ac * ac * (ld) 1.600000023841858f - (ld) 1.0f;
        c = (ld) F (c);
        dF = F ((c + (ld) 1.0f) * (ld) 0.5f);
    }
    const float cF = F (c);
    ld e = (ld) b / ((ld) a * (ld) 20000.0f + (ld) 5050.0f) * (ld) 12000.0f * (ld) M (base + 0xc);
    e = e + e;
    const ld v = (ld) x - e;
    ld s = (v + (ld) M (base + 0x10)) * (ld) dF - (ld) cF * (ld) M (base);
    M (base) = F (s);
    s = s + (ld) M (base + 0x14);
    M (base + 0x14) = M (base);
    s = s * (ld) dF - (ld) cF * (ld) M (base + 4);
    M (base + 4) = F (s);
    s = s + (ld) M (base + 0x18);
    M (base + 0x18) = M (base + 4);
    s = s * (ld) dF - (ld) cF * (ld) M (base + 8);
    M (base + 8) = F (s);
    s = s + (ld) M (base + 0x1c);
    M (base + 0x1c) = M (base + 8);
    s = s * (ld) dF - (ld) cF * (ld) M (base + 0xc);
    M (base + 0xc) = F (s - s * s * s * (ld) 0.016669999808073044f);
    M (base + 0x10) = F (v);
    return ((ld) 40000.0f - (ld) a) * (ld) M (base + 0xc) * (ld) 9.999999747378752e-05f;
}

// ------------------------------------------------------------------------------------------------ set-up (§2)
Engine::Engine (std::shared_ptr<const DllData> d)
    : data (std::move (d)), fbRing ((std::size_t) ringMask + 1), bufA (0x40000), bufB (0x40000), bufC (0x40000), bufD (0x40000), bufE (0x40000),
      bufL (8192), bufR (8192)
{
    X87Mode fpu (false);
    // the plugin's constructor: program 0's values (the front-end overwrites every parameter right after loading)
    // are only stored; then the cabinet (§5.3), the sample rate 44100 and the reset
    cab.C (0x30) = 0.5f;
    cab.C (0x34) = 0.5f;
    cab.C (0x24) = 1.0f;
    cab.C (0x28) = 0.325f;
    cab.C (0x2c) = 0.625f;
    cab.C (0x38) = 0.875f;
    cabinetRate (44100.0f);
    cabinetSet (3, bitsF (0x3ed55555));
    setSampleRate (44100.0f);
    reset();
    for (auto& st : str)
    {
        auto a = std::move (st.ringA), b = std::move (st.ringB), c = std::move (st.ringC);
        st = Str (std::move (a), std::move (b), std::move (c));
    }
    qHead = qTail = qCount = 0;
    volume = expression = 1.0f;
    M (0x4c8) = 1.0f;
    M (0x4c0) = 2.0f;
    chunkCounter = 0.0f;
    strumCount = strumChunks = 0;
    heldKeys.fill (0);
    heldCount = 0;
}

void Engine::setSampleRate (float sr) noexcept
{
    X87Mode fpu (false);
    cabinetRate (sr);
    rate = sr;
    rateRatio = F ((ld) 44100.0f / (ld) sr);
}

// the plugin's reset (§2.2): lookup tables, every filter and line cleared
void Engine::reset() noexcept
{
    // (the strings are cleared right after the reset; the sine table is shared, see sineTable())
    for (int k = 0; k < 21; ++k)
    {
        static const std::uint32_t pat[6] = { 0x3f800000, 0x3f953f7d, 0x3faa9fbe, 0x3fc00000, 0x3fd53f7d, 0x3feaa5f8 };
        for (int j = 0; j < 6; ++j)
            M (0x5dc + 4 * (6 * k + j)) = bitsF (pat[j]);
    }
    M (0x7d4) = 1.0f;
    M (0x7d8) = bitsF (0x3f953f7d);
    {
        ld f = (ld) 1.0;
        for (int i = 0; i < 128; ++i)
        {
            detuneTab[(std::size_t) (127 + i)] = F (f);
            detuneTab[(std::size_t) (127 - i)] = F ((ld) 1.0 / f);
            f = f * (ld) 1.00057779;
        }
    }
    for (int off : { 0x7fc, 0x7f4, 0x7f8, 0x808, 0x800, 0x804, 0x814, 0x80c, 0x810, 0x820, 0x818, 0x81c })
        M (off) = 0.0f;
    ampIdx = 0;
    for (int off = 0x4dc; off <= 0x538; off += 4)
        M (off) = 0.0f;
    M (0x23c) = M (0x238) = 0.0f;
    std::fill (fbRing.begin(), fbRing.end(), 0.0f);
    for (int off = 0x53c; off < 0x5cc; off += 4)
        M (off) = 0.0f;
    for (int off = 0x258; off < 0x2b0; off += 4)
        M (off) = 0.0f;
    cnt8c = cnt90 = cnt94 = 0;
    lastMode = -1;
    M (0x4d0) = M (0x4c8) = M (0x4cc) = M (0x4c0) = 0.0f;
    active = 1;
    idle = 5;
    M (0x240) = M (0x5cc) = M (0x228) = M (0x864) = 0.0f;
    for (int off = 0xb0; off < 0x218; off += 4)
        M (off) = 0.0f;
    M (0x4d8) = M (0x218) = M (0x21c) = M (0x22c) = 0.0f;
    lfoE = tapA = tapB = tapC = tapD = 0.0f;
    M (0x5d0) = 0.0f;
    fbWrite = 1 - ftol ((ld) rate * (ld) -0.004999999888241291f);
    fbRead = 0;
    std::fill (bufA.begin(), bufA.end(), 0.0f);
    std::fill (bufB.begin(), bufB.end(), 0.0f);
    std::fill (bufC.begin(), bufC.end(), 0.0f);
    std::fill (bufD.begin(), bufD.end(), 0.0f);
    std::fill (bufE.begin(), bufE.end(), 0.0f);
}

void Engine::setParameter (int i, float v) noexcept
{
    if (i < 0 || i >= 28)
        return;
    prm[(std::size_t) i] = v;
    if (i == 26)
    {
        flags &= ~2;
        if (ftol ((ld) v) != 0)
            flags |= 2;
    }
}

// effMainsChanged(1): every gated string released, held keys and the first string's lines cleared, idle state (§2.3)
void Engine::resume() noexcept
{
    X87Mode fpu (false);
    for (auto& s : str)
        if (s.gate != 0.0f)
            release (ftol ((ld) s.note), false);
    strumCount = 0;
    heldKeys.fill (0);
    heldCount = 0;
    std::fill (str[0].ringA.begin(), str[0].ringA.end(), 0.0f);
    std::fill (str[0].ringB.begin(), str[0].ringB.end(), 0.0f);
    strumChunks = 0;
    idle = 5;
}

// MIDI from the host (§2.4), queued with deltaFrames 0 and applied at the next chunk boundary
void Engine::midi (int status, int d1, int d2, int detune) noexcept
{
    X87Mode fpu (false);
    auto push = [&] () -> Event& {
        auto& e = queue[(std::size_t) qHead];
        e = Event {};
        e.delta = 0;
        e.kind = 1;
        qHead = (qHead + 1) & 0x3ff;
        return e;
    };
    auto commit = [&] { if (qCount < 0x400) ++qCount; };
    auto type = status & 0xf0;
    d1 &= 0x7f;
    d2 &= 0x7f;
    if (type == 0x80 || type == 0x90)
    {
        if (d2 == 0)
            type = 0x80;
        auto& e = push();
        e.note = d1;
        e.value = d2;
        e.fvalue = F ((ld) d2 * (ld) 0.007874015718698502f);
        if (type == 0x90)
        {
            e.kind = 2;
            e.detune = (std::int8_t) detune;
        }
        else
            e.kind = 4;
        commit();
    }
    else if (type == 0xb0)
    {
        auto& e = push();
        e.cc = d1;
        e.value = d2;
        e.fvalue = F ((ld) d2 * (ld) 0.007874015718698502f);
        commit();
        if (d1 == 0x78 || (d1 >= 0x7b && d1 <= 0x7f))
        {
            for (auto& s : str)
                if (s.gate != 0.0f)
                    release (ftol ((ld) s.note), false);
            strumCount = 0;
            heldKeys.fill (0);
            heldCount = 0;
        }
        else if (d1 == 0x79)
        {
            volume = expression = 1.0f;
            M (0x4c0) = 2.0f;
            M (0x4c8) = 1.0f;
        }
    }
    else if (type == 0xc0)
    {
        // program change: the factory programs are not part of LYNX (the front-end never sends it)
    }
    else if (type == 0xe0)
    {
        auto& e = push();
        e.cc = 0x85;
        const int v = (d2 << 7) | d1;
        e.value = v;
        e.fvalue = F ((ld) v * (ld) (v > 0x2000 ? 6.103888154029846e-05f : 6.103515625e-05f));
        commit();
    }
}

// ------------------------------------------------------------------------------------------------ processReplacing (§2.5)
void Engine::process (float* outL, float* outR, int n) noexcept
{
    if (n <= 0)
        return;
    {
        X87Mode fpu (false);
        tempoF = F ((ld) tempo);
    }
    X87Mode fpu (true);
    if ((int) bufL.size() < n)
    {
        bufL.resize ((std::size_t) n);
        bufR.resize ((std::size_t) n);
    }
    std::fill (bufL.begin(), bufL.begin() + n, 0.0f);
    std::fill (bufR.begin(), bufR.begin() + n, 0.0f);
    int pos = 0, remaining = n;
    while (remaining != 0)
    {
        const int k = fistp ((ld) chunkCounter);
        if (k > remaining)
        {
            chunkCounter = F ((ld) chunkCounter - (ld) remaining);
            strings (bufL.data() + pos, remaining);
            effects (bufL.data() + pos, bufR.data() + pos, remaining);
            break;
        }
        remaining -= k;
        if (k != 0)
        {
            strings (bufL.data() + pos, k);
            effects (bufL.data() + pos, bufR.data() + pos, k);
            pos += k;
        }
        const ld chunk = (ld) rate * (ld) 0.004999999888241291f;
        chunkCounter = F (((ld) chunkCounter - (ld) k) + chunk);
        const int limit = fistp ((ld) F (chunk)) + pos;
        while (qCount != 0 && queue[(std::size_t) qTail].delta < limit)
        {
            handle (queue[(std::size_t) qTail]);
            qTail = (qTail + 1) & 0x3ff;
            --qCount;
        }
        envelopes();
    }
    for (int i = 0; i < n; ++i)
    {
        outL[i] = F ((ld) bufL[(std::size_t) i] + (ld) outL[i]);
        outR[i] = F ((ld) bufR[(std::size_t) i] + (ld) outR[i]);
    }
}

// pitch-bend / mod-wheel semitone ratios (§3.5)
float semitoneRatio (int idx) noexcept
{
    static const int limits[] = { 1, 5, 0xc, 0x10, 0x15, 0x1b, 0x20, 0x25, 0x2b, 0x2f, 0x35, 0x39, 0x42, 0x45, 0x4b, 0x50, 0x55,
                                  0x5b, 0x60, 0x65, 0x6a, 0x70, 0x75, 0x7a };
    static const float ratio[] = { 0.5f, 0.529699981212616f, 0.5609999895095825f, 0.5942999720573425f, 0.6297000050544739f,
                                   0.6671000123023987f, 0.7067999839782715f, 0.7487999796867371f, 0.79339998960495f, 0.8406000137329102f,
                                   0.890500009059906f, 0.9434999823570251f, 1.0f, 1.059499979019165f, 1.122499942779541f,
                                   1.1892000436782837f, 1.2598999738693237f, 1.3348000049591064f, 1.414199948310852f, 1.4982999563217163f,
                                   1.587399959564209f, 1.6818000078201294f, 1.7818000316619873f, 1.8876999616622925f, 2.0f };
    for (int i = 0; i < 24; ++i)
        if (idx < limits[i])
            return ratio[i];
    return ratio[24];
}

void Engine::handle (const Event& e) noexcept
{
    if ((e.kind & 8) != 0)
        return; // program change (never sent by the front-end)
    if ((e.kind & 2) != 0)
    {
        noteOn (e.note, e.fvalue, (std::int8_t) e.detune);
        return;
    }
    if ((e.kind & 4) != 0)
    {
        noteOff (e.note, e.fvalue);
        return;
    }
    const auto gliss = ftol (F ((ld) P (27) * (ld) 1.9900000095367432f));
    switch (e.cc)
    {
        case 1:
            if (gliss != 0)
            {
                const ld t = ((ld) e.fvalue + (ld) 1.0f) * (ld) 0.5f * (ld) 128.0f;
                M (0x4c0) = F (fexp (t * (ld) 0.010830424726009369f));
                M (0x4c8) = semitoneRatio (ftol (F (t)) - 2);
            }
            else
                M (0x4d0) = e.fvalue;
            break;
        case 7: volume = e.fvalue; break;
        case 0xb: expression = e.fvalue; break;
        case 0x40:
            flags &= ~1;
            if (e.value != 0)
                flags |= 1;
            break;
        case 0x85:
            if (gliss != 0)
                M (0x4d0) = F ((ld) e.fvalue + (ld) e.fvalue - (ld) 1.0f);
            else
            {
                M (0x4c0) = F (fexp ((ld) e.fvalue * (ld) 128.0f * (ld) 0.010830424726009369f));
                M (0x4c8) = semitoneRatio (ftol (F ((ld) e.fvalue * (ld) 128.0f)) - 2);
            }
            break;
        default: break;
    }
}

// ------------------------------------------------------------------------------------------------ notes (§3.1)
void Engine::releaseAny (int key) noexcept
{
    // the first gated string playing `key` (compared as a byte) is released, regardless of sustain / latch
    if (key < 0 || key > 127)
        return;
    for (auto& s : str)
        if (s.gate != 0.0f && (std::int8_t) ftol ((ld) s.note) == (std::int8_t) key)
        {
            s.held = 0;
            s.envState = 1;
            s.gate = 0.0f;
            s.envStart = s.env;
            s.envPhase = 0.0f;
            return;
        }
}

void Engine::release (int key, bool latchCheck) noexcept
{
    if (key < 0 || key > 127)
        return;
    for (auto& s : str)
        if (s.gate != 0.0f && (std::int8_t) ftol ((ld) s.note) == (std::int8_t) key)
        {
            s.held = 0;
            if (flags != 0 && latchCheck)
                return;
            s.gate = 0.0f;
            s.envState = 1;
            s.envStart = s.env;
            s.envPhase = 0.0f;
            return;
        }
}

Str& Engine::choose (int key) noexcept
{
    const auto k = (float) (std::int8_t) key;
    for (auto& s : str)
        if (k == s.note)
            return s;
    const float thr = F ((ld) rate * (ld) 0.30000001192092896f);
    for (auto& s : str)
        if (s.releaseTimer >= thr)
            return s;
    for (auto& s : str)
        if (s.gate < 0.5f)
            return s;
    int best = 0, bestAge = 0;
    for (int i = 0; i < 6; ++i)
        if (str[(std::size_t) i].age > bestAge)
            bestAge = str[(std::size_t) i].age, best = i;
    return str[(std::size_t) best];
}

void Engine::trigger (int key, float vel, int detune, int delay) noexcept
{
    if (key < 0 || key > 127)
        return;
    auto& s = choose (key);
    s.gate = 1.0f;
    s.held = 1;
    s.note = F ((ld) (std::int8_t) key);
    s.envState = 4;
    s.env = 0.0f;
    s.envPhase = 0.0f;
    s.pickPos = 0.0f;
    s.slide = 0.0f;
    s.lpA = 0.0f;
    M (0x248) = 0.0f;
    M (0x244) = 0.5f;
    lastKey = (std::int16_t) ftol ((ld) s.note);
    s.detune = (std::int8_t) detune;
    s.vel = vel;
    s.releaseTimer = -1.0f;
    s.age = 0;
    s.phase = 0.0f;
    s.hist[0] = 0.0f;
    s.level = 0.0f;
    idle = 0;
    s.delay = delay < 0 ? 0 : delay;
}

int Engine::pushHeld (int key) noexcept
{
    const int old = heldCount;
    std::memmove (heldKeys.data() + 1, heldKeys.data(), (std::size_t) old);
    heldKeys[0] = (std::int8_t) key;
    heldKeys[0x81] = heldKeys[0x82] = (std::int8_t) key;
    heldCount = old + 1;
    for (int i = 1; i < heldCount; ++i)
    {
        if (heldKeys[(std::size_t) i] < heldKeys[0x81])
            heldKeys[0x81] = heldKeys[(std::size_t) i];
        if (heldKeys[(std::size_t) i] > heldKeys[0x82])
            heldKeys[0x82] = heldKeys[(std::size_t) i];
    }
    return old;
}

int Engine::popHeld (int key) noexcept
{
    if (heldCount == 0)
        return 0;
    int i = 0;
    while (i < heldCount && heldKeys[(std::size_t) i] != (std::int8_t) key)
        ++i;
    if (i < heldCount)
    {
        std::memmove (heldKeys.data() + i, heldKeys.data() + i + 1, (std::size_t) (heldCount - i));
        --heldCount;
    }
    heldKeys[0x81] = heldKeys[0x82] = heldKeys[0];
    for (int j = 1; j < heldCount; ++j)
    {
        if (heldKeys[(std::size_t) j] < heldKeys[0x81])
            heldKeys[0x81] = heldKeys[(std::size_t) j];
        if (heldKeys[(std::size_t) j] > heldKeys[0x82])
            heldKeys[0x82] = heldKeys[(std::size_t) j];
    }
    return heldCount;
}

void Engine::noteOn (int key, float vel, int detune) noexcept
{
    const auto& d = *data;
    switch (ftol (F ((ld) P (24) * (ld) 5.989999771118164f)))
    {
        case 0: trigger (key, vel, detune, 0); break;
        case 1: // chords
        {
            const int root = key % 12;
            const int strum = ftol (F ((ld) P (25) * (ld) 10.989999771118164f));
            for (auto& s : str)
                releaseAny (ftol ((ld) s.note));
            int delay = 0;
            for (int i = 0; i < 6; ++i)
            {
                const int o = d.chords[(std::size_t) (root * 6 + i)];
                if (o < 0)
                    break;
                trigger (o + 0x30, vel, 0, delay);
                delay += strum;
            }
            break;
        }
        case 2: // power chords
        {
            const int strum = ftol (F ((ld) P (25) * (ld) 10.989999771118164f));
            for (auto& s : str)
                releaseAny (ftol ((ld) s.note));
            if (key < 0x3c)
            {
                trigger (key + 12, vel, detune, 0);
                trigger (key + 17, vel, detune + 5, strum);
                trigger (key + 24, vel, detune + 2, strum * 2);
            }
            else
            {
                trigger (key - 12, vel, detune, 0);
                trigger (key - 5, vel, detune - 4, strum);
                trigger (key, vel, detune - 2, strum * 2);
            }
            break;
        }
        case 3: // strum
        {
            if (strumCount == 0)
                strumChunks = 0;
            const int strum = ftol (F ((ld) P (25) * (ld) 10.989999771118164f));
            trigger (key, vel, detune, strumCount * strum - strumChunks);
            ++strumCount;
            break;
        }
        case 4:
        case 5: // solo modes: glide from the sounding pitch
        {
            const bool fixed = ftol (F ((ld) P (24) * (ld) 5.989999771118164f)) == 4;
            glideTarget = key;
            if (pushHeld (key) == 0)
            {
                trigger (key, vel, detune, 0);
                glidePos = F ((ld) key);
                break;
            }
            M (0x4d4) = F ((ld) vel * (ld) 0.30000001192092896f);
            const int t = ftol (F ((ld) P (25) * (ld) 6.989999771118164f));
            if (fixed)
                glideStep = F (std::fabs (((ld) glideTarget - (ld) glidePos) / ((ld) d.glideA[(std::size_t) t] * (ld) 200.0f / (ld) tempoF)));
            else
                glideStep = F ((ld) d.glideB[(std::size_t) t] * (ld) 200.0f / (ld) tempoF / std::fabs ((ld) glideTarget - (ld) glidePos));
            break;
        }
        default: break;
    }
}

void Engine::noteOff (int key, float vel) noexcept
{
    const auto& d = *data;
    switch (ftol (F ((ld) P (24) * (ld) 5.989999771118164f)))
    {
        case 0: release (key, true); break;
        case 1:
        {
            const int root = key % 12;
            for (int i = 0; i < 6; ++i)
            {
                const int o = d.chords[(std::size_t) (root * 6 + i)];
                if (o < 0)
                    break;
                release (o + 0x30, true);
            }
            break;
        }
        case 2:
            if (key < 0x3c)
                for (int o : { 12, 17, 24, 29, 36 })
                    release (key + o, true);
            else
            {
                release (key - 12, true);
                release (key - 5, true);
                release (key, true);
                release (key + 7, true);
                release (key + 12, true);
            }
            break;
        case 3:
            release (key, true);
            --strumCount;
            break;
        case 4:
        case 5:
        {
            const bool fixed = ftol (F ((ld) P (24) * (ld) 5.989999771118164f)) == 4;
            if (popHeld (key) == 0)
            {
                for (auto& s : str)
                    release (ftol ((ld) s.note), false);
                break;
            }
            if (fixed)
                M (0x4d4) = F ((ld) vel * (ld) 0.30000001192092896f);
            glideTarget = heldKeys[0];
            const int t = ftol (F ((ld) P (25) * (ld) 6.989999771118164f));
            if (fixed)
                glideStep = F (std::fabs (((ld) glideTarget - (ld) glidePos) / ((ld) d.glideA[(std::size_t) t] * (ld) 200.0f / (ld) tempoF)));
            else
                glideStep = F ((ld) d.glideB[(std::size_t) t] * (ld) 200.0f / (ld) tempoF / std::fabs ((ld) glideTarget - (ld) glidePos));
            break;
        }
        default: break;
    }
}

// the exciter envelopes, once per chunk (§3.3)
void Engine::envelopes() noexcept
{
    ld x = ((ld) 1.0f - ((ld) P (3) * (ld) 0.5f + (ld) 0.25f)) * (ld) 0.8922699689865112f + (ld) 0.10773000121116638f;
    const ld a = (ld) 20.0f / (x * x * x * (ld) 800.0f);
    const ld b = (ld) 20.0f / (a * a * a * (ld) 800.0f);
    for (auto& s : str)
    {
        if (s.envState == 0)
            continue;
        if (s.delay > 0)
        {
            --s.delay;
            continue;
        }
        switch (s.envState)
        {
            case 1:
            {
                const float p = F (b + (ld) s.envPhase);
                s.envPhase = p;
                if (p < 1.0f)
                    s.env = F (((ld) 1.0f - (ld) p) * (ld) s.envStart);
                else
                {
                    s.envState = 0;
                    s.env = 0.0f;
                }
                break;
            }
            case 2: s.env = 0.0f; break;
            case 3:
            {
                const float p = F (a + (ld) s.envPhase);
                s.envPhase = p;
                if (p < 1.0f)
                    s.env = F ((ld) 1.0f - (ld) p);
                else
                {
                    s.envState = 2;
                    s.envPhase = 0.0f;
                    s.env = 0.0f;
                }
                break;
            }
            case 4:
            {
                const float p = F ((ld) s.envPhase + (ld) 19.99539566040039f);
                s.envPhase = p;
                if (p < 1.0f)
                    s.env = p;
                else
                {
                    s.envState = 3;
                    s.envPhase = 0.0f;
                    s.env = 1.0f;
                }
                break;
            }
            default: break;
        }
        s.envCopy = s.env;
    }
}

// ------------------------------------------------------------------------------------------------ the strings (§3.2)
void Engine::strings (float* out, int n) noexcept
{
    const auto& d = *data;
    active = 0;
    M (0x85c) = bitsF (0x37be37c6);
    M (0x858) = F ((ld) 1.0f / (ld) rate);
    const int mode = ftol (F ((ld) P (24) * (ld) 5.989999771118164f));
    if (lastMode != mode)
    {
        lastMode = mode;
        for (auto& s : str)
            releaseAny (ftol ((ld) s.note));
        strumCount = 0;
        heldKeys.fill (0);
        heldCount = 0;
    }
    if (flags != prevFlags)
    {
        prevFlags = flags;
        if (flags == 0)
            for (auto& s : str)
                if (s.held == 0)
                    releaseAny (ftol ((ld) s.note));
    }
    // strings released long ago (or everything idle) are cut; the engine passes the string's index as the key
    for (int k = 0; k < 6; ++k)
    {
        auto& s = str[(std::size_t) k];
        if (s.gate != 0.0f && ((ld) rate * (ld) 0.20000000298023224f <= (ld) s.releaseTimer || idle >= 4))
        {
            releaseAny (k);
            if (flags != 0)
                s.releaseTimer = 1.0e9f;
        }
    }
    const float step = F ((ld) 1.0f / ((ld) rate * (ld) 0.004999999888241291f));
    M (0x864) = 0.0f;
    const int fbStart = fbRead;
    ++strumChunks;
    if (idle >= 4)
    {
        fbRead = fbStart + n;
        return;
    }
    for (int si = 0; si < 6; ++si)
    {
        auto& s = str[(std::size_t) si];
        ++s.age;
        if (s.delay != 0 || ! ((ld) rate * (ld) 0.20000000298023224f > (ld) s.releaseTimer))
            continue;
        active = 1;
        if (s.gate > 0.5f)
            M (0x864) = F ((ld) s.vel * (ld) 1.5f);
        if (M (0x864) > 1.0f)
            M (0x864) = 1.0f;
        float* o = out;
        float target = s.envCopy;
        if (ftol (F ((ld) P (0) * (ld) 6.989999771118164f)) != 0)
            target = 1.0f;
        const float keyScale = F (((ld) s.note - (ld) 64.0f) * (ld) 0.03125f + (ld) 1.0f);
        float lvl = s.level;
        float damp;
        {
            const ld v = (ld) s.vel;
            ld a = (v - (ld) 1.0f) * (ld) 3.0f;
            ld b = (ld) 1.0f - v;
            b = b + b;
            b = b + (ld) 1.0f;
            a = a / b + (ld) 1.0f;
            const ld c = ((ld) 1.0f - (ld) P (7)) * ((ld) 1.0f - (ld) P (9));
            const ld x = (c + a * (ld) P (9)) * (ld) keyScale;
            damp = F (x);
            if (x < (ld) 0.029999999329447746f)
                damp = bitsF (0x3cf5c28f);
            else if (damp > 0.9990000128746033f)
                damp = bitsF (0x3f7fbe77);
        }
        float keyLp;
        {
            const ld t = ((ld) 128.0f - (ld) s.note) * (ld) 0.0078125f;
            keyLp = F (t * t * (ld) 16.0f + (ld) 1.0f);
        }
        if (M (0x4c8) != M (0x4cc))
        {
            M (0x4cc) = M (0x4c8);
            M (0x4d4) = F (((ld) 1.0f - (ld) P (5)) * (ld) 0.05000000074505806f + (ld) P (5) * (ld) s.vel * (ld) 0.07999999821186066f);
        }
        if (mode < 4)
            glidePos = s.note;
        else
        {
            const float tgt = F ((ld) glideTarget);
            const ld cur = (ld) glidePos;
            if (cur > (ld) tgt)
            {
                const ld c2 = cur - (ld) glideStep;
                glidePos = c2 < (ld) tgt ? tgt : F (c2);
            }
            else if (cur < (ld) tgt)
            {
                const ld c2 = cur + (ld) glideStep;
                glidePos = c2 > (ld) tgt ? tgt : F (c2);
            }
            else
                glidePos = F (cur);
        }
        float lenA, lenC;
        {
            ld y = fexp ((ld) glidePos * (ld) 0.0577622652053833f) * (ld) 8.175749778747559f;
            y = (ld) 44100.0f / y;
            y = y / (ld) M (0x4c8);
            y = y / ((ld) M (0x4d0) * (ld) 0.12240000069141388f + (ld) 1.0f);
            y = y + y;
            y = y * (ld) detuneTab[(std::size_t) (127 + s.detune)];
            lenA = F (y * (ld) 0.9992006421089172f);
            lenC = F (y * (ld) 1.000800013542175f);
        }
        if (s.gate > 0.5f)
            s.velHeld = s.vel;
        float exc;
        {
            const ld p8 = (ld) P (8);
            ld t = (ld) s.velHeld * (ld) s.velHeld;
            t = t * p8;
            t = t * p8;
            t = ((ld) 1.0f - p8) + t;
            exc = F (t * (ld) 0.9700000286102295f * (ld) keyScale);
            if (exc < 0.029999999329447746f)
                exc = bitsF (0x3cf5c28f);
            else if (exc > 1.0f)
                exc = 1.0f;
        }
        const float excRest = F ((ld) 1.0f - (ld) exc);
        fbRead = fbStart;
        const float fbAmount = F ((ld) P (15) * (ld) P (15) * (ld) 0.0010000000474974513f);
        const float loss = F (((ld) 1.0f - (ld) P (7)) * (ld) 0.009999999776482582f + (ld) 0.9900000095367432f);
        float tone;
        {
            ld e = (ld) P (3) * (ld) 0.5f + (ld) 0.25f;
            if (e < (ld) 0.5f)
                e = e * (ld) 1.5f + (ld) 0.25f;
            else
                e = (e - (ld) 0.5f) * (ld) 6.0f + (ld) 1.0f;
            tone = F (e);
        }
        const float velMix = F ((ld) P (8) * (ld) s.vel + (ld) 1.0f - (ld) P (8));
        float limit;
        {
            const ld f = (ld) 1.0f - (ld) P (4) * (ld) 0.949999988079071f;
            limit = F (f * f);
        }
        const float slapGain = F ((ld) 1.0f / (((ld) 1.0f - (ld) P (4) * (ld) 0.9800000190734863f) + (ld) 1.000000045813705e-18f));
        float res;
        {
            const ld sn = fsin (((ld) P (6) * (ld) 6000.0f + (ld) 1000.0f) * (ld) 7.123786781448871e-05f);
            res = F (sn + sn);
            if (sn + sn > (ld) 0.9900000095367432f)
                res = bitsF (0x3f7d70a4);
        }
        int pick1 = ftol (F (((ld) P (2) + (ld) 0.05000000074505806f) * (ld) M (0x5dc + 4 * fistp ((ld) s.note)) * (ld) 100.0f));
        int pick2 = 0;
        float g1 = 0.0f, g2 = 0.0f;
        switch (ftol (F ((ld) P (1) * (ld) 2.990000009536743f)))
        {
            case 0: g1 = g2 = 0.0f; break;
            case 1:
                pick2 = pick1 * 2;
                g2 = -1.0f;
                g1 = 1.0f;
                break;
            case 2:
                g2 = -1.0f;
                g1 = 1.0f;
                pick1 = ftol (F ((ld) pick1 * (ld) lenC * (ld) 0.0020000000949949026f));
                pick2 = pick1 * 2;
                break;
            default: break;
        }
        const int type = ftol (F ((ld) P (0) * (ld) 6.989999771118164f));
        const auto key = (std::int8_t) ftol ((ld) s.note);
        switch (type)
        {
            case 1:
                s.pickRate = 1.0f, s.pickGain = 0.125f;
                s.pick = key < 0x32 ? 0xe : key < 0x3c ? 0xf : key < 0x46 ? 0x10 : (key >= 0x50 ? 0x12 : 0x11);
                break;
            case 2:
                s.pickRate = 1.0f;
                if (key < 0x32)
                    s.pick = 0x13, s.pickGain = bitsF (0x3daa9931);
                else if (key < 0x3c)
                    s.pick = 0x14, s.pickGain = bitsF (0x3daa9931);
                else if (key < 0x46)
                    s.pick = 0x15, s.pickGain = 0.125f;
                else
                    s.pick = 0x16, s.pickGain = 0.125f;
                break;
            case 3:
                s.pick = 1, s.pickGain = bitsF (0x3d2a64c3);
                s.pickRate = bitsF (key < 0x30 ? 0x3f47ae14 : key < 0x38 ? 0x3f79999a : key < 0x3e ? 0x3fa66666 : key < 0x46 ? 0x3fd851ec : key < 0x4e ? 0x400d70a4 : 0x40370a3d);
                break;
            case 4:
            {
                const int v = ftol (F ((ld) s.velHeld * (ld) 2.990000009536743f));
                if (v == 0)
                {
                    s.pick = 0x1b, s.pickGain = bitsF (0x3d577319);
                    s.pickRate = bitsF (key < 0x30 ? 0x3f666666 : key < 0x3a ? 0x3f900000 : key < 0x44 ? 0x3fc00000 : 0x3ff9999a);
                }
                else if (v == 1)
                {
                    s.pick = 0x1a, s.pickGain = 0.0625f;
                    s.pickRate = bitsF (key < 0x30 ? 0x3f666666 : key < 0x3a ? 0x3f900000 : key < 0x44 ? 0x3fc00000 : 0x3ff9999a);
                }
                else if (v == 2)
                {
                    s.pick = 0x1c, s.pickGain = 0.03125f;
                    s.pickRate = bitsF (key < 0x30 ? 0x3fe66666 : key < 0x3a ? 0x40100000 : key < 0x44 ? 0x40400000 : 0x4079999a);
                }
                break;
            }
            case 5:
                s.pick = 7, s.pickGain = 0.125f;
                s.pickRate = bitsF (key < 0x30 ? 0x3f666666 : key < 0x3a ? 0x3f900000 : key < 0x44 ? 0x3fc00000 : 0x3ff9999a);
                break;
            case 6:
                s.pick = 0x18, s.pickGain = bitsF (0x3d2a64c3);
                s.pickRate = bitsF (key < 0x30 ? 0x3f666666 : key < 0x3a ? 0x3f900000 : key < 0x44 ? 0x3fc00000 : 0x3ff9999a);
                break;
            default: break;
        }
        const float excGain = F ((ld) velMix * (ld) tone * (ld) keyLp);
        float pickPos = s.pickPos;
        pickActive = 1;
        const float* fb = fbRing.data();
        for (int i = 0; i < n; ++i)
        {
            if (s.phase >= 1.0f)
            {
                const float lpMix = F ((ld) 1.0f - (ld) damp);
                const float negLimit = -limit;
                ld outE = 0;
                ld ph;
                do
                {
                    float x = bitsF (0x219392ef); // 1e-18
                    if (s.envState != 0)
                    {
                        float l2 = lvl;
                        if ((ld) lvl > (ld) target)
                        {
                            const ld t = (ld) lvl - (ld) step;
                            l2 = F (t);
                            if (t < (ld) target)
                                l2 = target;
                        }
                        else if ((ld) lvl < (ld) target)
                        {
                            const ld t = (ld) lvl + (ld) step;
                            l2 = F (t);
                            if (t > (ld) target)
                                l2 = target;
                        }
                        lvl = l2;
                        if (type != 0)
                        {
                            const auto& tab = d.picks[(std::size_t) s.pick];
                            const float g = F ((ld) l2 * (ld) s.pickGain * (ld) excGain * (ld) 3.0517578125e-05f);
                            const float adv = F ((ld) tone * (ld) s.pickRate);
                            if (! tab.empty() && (ld) tab[0] - (ld) 1.0f > (ld) pickPos)
                            {
                                const int k = fistp ((ld) pickPos);
                                const ld fr = (ld) pickPos - (ld) k;
                                ld v = (ld) tab[(std::size_t) k + 1] * ((ld) 1.0f - fr);
                                v = v + (ld) tab[(std::size_t) k + 2] * fr;
                                x = F (v * (ld) g);
                                pickPos = F ((ld) adv + (ld) pickPos);
                            }
                            else
                                pickActive = 0;
                        }
                        else
                        {
                            const int r = rnd();
                            ld v = (ld) r * (ld) 3.0510000215144828e-05 - (ld) 0.5;
                            x = F (v * (ld) l2 * (ld) excGain * (ld) 0.125f);
                        }
                    }
                    if ((ld) M (0x4d4) > (ld) 0.004999999888241291f)
                    {
                        const float q = F (((ld) M (0x4d8) - (ld) 0.5f) * (ld) M (0x4d4) * (ld) velMix);
                        const float ph2 = F ((ld) M (0x4d8) + (ld) 0.009999999776482582f);
                        M (0x4d8) = ph2;
                        M (0x4d8) = F ((ld) M (0x4d8) - (ld) fistp ((ld) ph2));
                        M (0x4d4) = F ((ld) M (0x4d4) * (ld) 0.9997000098228455f);
                        x = F (((ld) q + (ld) x) * (ld) exc + (ld) excRest * (ld) s.slide);
                        s.slide = x;
                    }
                    const float fret = F ((ld) x * (ld) P (5));
                    ++s.idxC;
                    const int kc = fistp ((ld) lenC);
                    const ld frC = (ld) lenC - (ld) kc;
                    ld vc = ((ld) 1.0f - frC) * (ld) s.ringC[(std::size_t) ((s.idxC - kc) & ringMask)];
                    vc = vc + frC * (ld) s.ringC[(std::size_t) ((s.idxC - kc - 1) & ringMask)];
                    ++s.idxA;
                    const float yC = F (vc * (ld) loss + (ld) x);
                    const int ka = fistp ((ld) lenA);
                    const ld frA = (ld) lenA - (ld) ka;
                    const float fbv = F ((ld) fb[(std::size_t) (fbRead & ringMask)] * (ld) fbAmount);
                    ld va = ((ld) 1.0f - frA) * (ld) s.ringA[(std::size_t) ((s.idxA - ka) & ringMask)];
                    va = va + frA * (ld) s.ringA[(std::size_t) ((s.idxA - ka - 1) & ringMask)];
                    float yA = F (va * (ld) loss + (ld) x + (ld) fbv);
                    ld yb = (ld) fbv + (ld) yC;
                    if (yA < negLimit)
                        yA = negLimit;
                    else if (yA > limit)
                        yA = limit;
                    if (yb < (ld) negLimit)
                        yb = (ld) negLimit;
                    else if (yb > (ld) limit)
                        yb = (ld) limit;
                    const ld wA = (ld) lpMix * (ld) s.lpA + (ld) yA * (ld) damp;
                    s.lpA = F (wA);
                    s.ringA[(std::size_t) (s.idxA & ringMask)] = F (wA);
                    const ld wC = (ld) lpMix * (ld) s.lpC + yb * (ld) damp;
                    s.lpC = F (wC);
                    s.ringC[(std::size_t) (s.idxC & ringMask)] = F (wC);
                    ld o2 = (yb * (ld) 0.4000000059604645f + (ld) yA) * (ld) 0.8500000238418579f + (ld) fret;
                    if (s.gate < 0.5f)
                    {
                        s.releaseTimer = F ((ld) tone + (ld) s.releaseTimer);
                        ld l = (ld) lvl;
                        if (l > 0)
                        {
                            l = (ld) lvl - (ld) step;
                            if (l < 0)
                                l = 0;
                        }
                        else if (l < 0)
                        {
                            l = (ld) lvl + (ld) step;
                            if (l > 0)
                                l = 0;
                        }
                        lvl = F (l);
                        o2 = o2 * l;
                    }
                    if (o2 < (ld) negLimit)
                        o2 = (ld) negLimit;
                    else if (o2 > (ld) limit)
                        o2 = (ld) limit;
                    s.ringB[(std::size_t) (s.idxA & ringMask)] = F (o2);
                    const ld b1 = (ld) s.ringB[(std::size_t) ((s.idxA - pick1) & ringMask)];
                    const ld b2 = (ld) s.ringB[(std::size_t) ((s.idxA - pick2) & ringMask)];
                    const ld sv = (ld) res * (ld) M (0x56c + 4 * si) + (ld) M (0x53c + 4 * si);
                    M (0x53c + 4 * si) = F (sv);
                    const ld sum = b2 * (ld) g1 + b1 * (ld) g2;
                    ld hp = ((o2 + sum) + (ld) 1.000000045813705e-18f) * (ld) 0.5f - sv;
                    hp = hp - (ld) M (0x56c + 4 * si) * (ld) 0.5f;
                    M (0x59c + 4 * si) = F (hp);
                    M (0x56c + 4 * si) = F (hp * (ld) res + (ld) M (0x56c + 4 * si));
                    outE = (ld) slapGain * (ld) M (0x53c + 4 * si);
                    ph = (ld) s.phase - (ld) 1.0f;
                    s.phase = F (ph);
                } while (ph >= (ld) 1.0f);
                s.hist[3] = s.hist[2];
                s.hist[2] = s.hist[1];
                s.hist[1] = s.hist[0];
                s.hist[0] = F (outE);
            }
            ++fbRead;
            const ld phE = (ld) 44100.0f / (ld) rate + (ld) s.phase;
            s.phase = F (phE);
            const ld h3 = s.hist[3], h2 = s.hist[2], h1 = s.hist[1], h0 = s.hist[0];
            ld A = (h1 + h1 + h3) - (h2 * (ld) 5.0f + h0) * (ld) 0.5f;
            const ld B = A + (((h2 - h1) * (ld) 3.0f - h3 + h0) * (ld) 0.5f) * phE;
            const ld Cc = B * phE + (h1 - h3) * (ld) 0.5f;
            o[i] = F (Cc * phE + (ld) o[i] + h2);
        }
        if (pickActive == 0)
            s.envState = 0;
        s.level = lvl;
        s.pickPos = pickPos;
    }
    fbRead = fbStart + n;
}

// ------------------------------------------------------------------------------------------------ the amp helpers (§5.1)
const float* sineTable() noexcept
{
    // 65536 points of sin(k/65536 · 6.28318024) (the engine's float 2π), computed like the plugin's reset
    static const std::vector<float> t = [] {
        X87Mode fpu (false);
        std::vector<float> v (65536);
        for (int k = 0; k < 65536; ++k)
            v[(std::size_t) k] = F (fsin ((ld) k * (ld) 1.52587890625e-05f * (ld) 6.283180236816406f));
        return v;
    }();
    return t.data();
}

// clamp to ±1, scale by 0.785·65536/(2π) and look the sine table up (truncation, 16-bit wrap)
inline ld shaper (ld v) noexcept
{
    if (v > (ld) 1.0f)
        v = (ld) 1.0f;
    else if (v < (ld) -1.0f)
        v = (ld) -1.0f;
    const float i = F (v * (ld) 0.7850000262260437f * (ld) 10430.0546875f);
    return (ld) sineTable()[fistp ((ld) i) & 0xffff];
}

inline ld eqGain (Engine& e) noexcept
{
    return (((ld) 3.5f - (ld) e.P (16)) - (ld) e.P (17)) - (ld) e.P (18);
}

ld Engine::ampClean (float x, float drive, float vol, float sr) noexcept
{
    const float k = F (((ld) drive * (ld) drive * (ld) drive * (ld) 0.8999999761581421f + (ld) 0.05000000074505806f) * (ld) 44100.0f / (ld) sr);
    const float g = F ((ld) vol * (ld) vol * (ld) 32.0f + (ld) 1.0f);
    const float om = F ((ld) 1.0f - (ld) k);
    const ld p15 = (ld) P (15);
    ld v = (p15 * (ld) M (0x7fc) * p15 * (ld) 0.10000000149011612f + (ld) x) * (ld) k + (ld) om * (ld) M (0x218);
    M (0x218) = F (v);
    ld t = shaper (v * (ld) g);
    v = t * (ld) k + (ld) om * (ld) M (0x21c);
    M (0x21c) = F (v);
    t = shaper (v * (ld) g);
    v = t * (ld) k + (ld) om * (ld) M (0x22c);
    M (0x22c) = F (v);
    t = shaper (v * (ld) g);
    const ld r = SLAYER_EQ3 (F (t + (ld) 1.000000045813705e-18f));
    return eqGain (*this) * r * (ld) 0.5f * (ld) 0.5f;
}

ld Engine::ampDrive (float x, float drive, float vol, float sr) noexcept
{
    const float k = F (((ld) drive * (ld) drive * (ld) drive * (ld) 0.8999999761581421f + (ld) 0.05000000074505806f) * (ld) 44100.0f / (ld) sr);
    const float g = F ((ld) vol * (ld) vol * (ld) 32.0f + (ld) 1.0f);
    const ld p15 = (ld) P (15);
    const ld r = SLAYER_EQ3 (F (p15 * (ld) M (0x7fc) * p15 * (ld) 0.10000000149011612f + (ld) x + (ld) 1.000000045813705e-18f));
    const ld omE = (ld) 1.0f - (ld) k;
    const float om = F (omE);
    ld v = omE * (ld) M (0x218) + (ld) k * (eqGain (*this) * r * (ld) 0.5f);
    M (0x218) = F (v);
    ld t = shaper (v * (ld) g);
    v = t * (ld) k + (ld) om * (ld) M (0x21c);
    M (0x21c) = F (v);
    t = shaper (v * (ld) g);
    v = t * (ld) k + (ld) om * (ld) M (0x22c);
    M (0x22c) = F (v);
    return shaper (v * (ld) g) * (ld) 0.5f;
}

ld Engine::ampFuzz (float x, float drive, float vol, float sr) noexcept
{
    const ld s = fsin (((ld) drive * (ld) drive * (ld) 2000.0f + (ld) 300.0f) * ((ld) 3.141590118408203f / (ld) sr));
    const float f = F (s + s);
    const float g = F ((ld) vol * (ld) vol * (ld) 32.0f + (ld) 1.0f);
    const ld p15 = (ld) P (15);
    ld a = (ld) f * (ld) M (0x814) + (ld) M (0x80c);
    M (0x80c) = F (a);
    ld hp = (((p15 * (ld) M (0x7fc) * p15 * (ld) 0.10000000149011612f + (ld) x) + (ld) 0.0010000000474974513f) - a) - (ld) M (0x814);
    M (0x810) = F (hp);
    ld lp = hp * (ld) f + (ld) M (0x814);
    M (0x814) = F (lp);
    ld t = shaper (lp * (ld) g);
    a = (ld) f * (ld) M (0x820) + (ld) M (0x818);
    M (0x818) = F (a);
    hp = ((t + (ld) 0.0010000000474974513f) - a) - (ld) M (0x820);
    M (0x81c) = F (hp);
    lp = hp * (ld) f + (ld) M (0x820);
    M (0x820) = F (lp);
    t = shaper (lp * (ld) g);
    a = (ld) f * (ld) M (0x82c) + (ld) M (0x824);
    M (0x824) = F (a);
    hp = ((t + (ld) 0.0010000000474974513f) - a) - (ld) M (0x82c);
    M (0x828) = F (hp);
    lp = hp * (ld) f + (ld) M (0x82c);
    M (0x82c) = F (lp);
    t = shaper (lp * (ld) g);
    const ld r = SLAYER_EQ3 (F (t + (ld) 1.000000045813705e-18f));
    return eqGain (*this) * r * (ld) 0.5f * (ld) 0.699999988079071f;
}

// two 3-stage all-pass chains in quadrature mixed with a sine/cosine of the LFO (frequency shifter, §4.1)
ld Engine::phaser (float x, float lfo) noexcept
{
    const float m27c = M (0x27c);
    ld a1 = ((ld) M (0x270) - (ld) x) * (ld) 0.2328290045261383f + (ld) M (0x268);
    M (0x258) = F (a1);
    ld a2 = ((ld) M (0x278) - a1) * (ld) 0.8435729742050171f + (ld) M (0x270);
    M (0x25c) = F (a2);
    ld a3 = ((ld) M (0x280) - a2) * (ld) 0.9803509712219238f + (ld) M (0x278);
    M (0x280) = m27c;
    M (0x278) = M (0x274);
    M (0x270) = M (0x26c);
    M (0x268) = M (0x264);
    M (0x260) = F (a3);
    const float a3F = F (a3);
    const float oldB3 = M (0x2a8);
    M (0x27c) = M (0x260);
    M (0x274) = M (0x25c);
    M (0x26c) = M (0x258);
    M (0x264) = x;
    ld b1 = ((ld) M (0x29c) - (ld) x) * (ld) 0.6127790212631226f + (ld) M (0x294);
    M (0x284) = F (b1);
    ld b2 = ((ld) M (0x2a4) - b1) * (ld) 0.9426360130310059f + (ld) M (0x29c);
    M (0x288) = F (b2);
    ld b3 = ((ld) M (0x2ac) - b2) * (ld) 0.9953020215034485f + (ld) M (0x2a4);
    M (0x2ac) = oldB3;
    M (0x2a4) = M (0x2a0);
    M (0x29c) = M (0x298);
    M (0x294) = M (0x290);
    M (0x28c) = F (b3);
    M (0x2a8) = M (0x28c);
    M (0x2a0) = M (0x288);
    M (0x298) = M (0x284);
    M (0x290) = x;
    const ld t = (ld) lfo * (ld) 6.283180236816406f;
    const float tF = F (t);
    const float i1 = F (t * (ld) 10430.0546875f);
    const float s = sineTable()[fistp ((ld) i1) & 0xffff];
    const float i2 = F (((ld) tF + (ld) 1.5707950592041016f) * (ld) 10430.0546875f);
    const float c = sineTable()[fistp ((ld) i2) & 0xffff];
    return (ld) c * (ld) oldB3 + (ld) s * (ld) a3F;
}

// ------------------------------------------------------------------------------------------------ effects, amp, cabinet (§4, §5)
// triangle helper of the LFOs: t = 2·|2·(p − trunc p − 0.5)| − 1 computed the engine's way (in steps, x87)
inline ld tri (float p) noexcept
{
    ld t = ((ld) p - (ld) fistp ((ld) p)) - (ld) 0.5f;
    t = t + t;
    if (t < 0)
        t = -t;
    t = t - (ld) 0.5f;
    return t + t;
}

void Engine::effects (float* l, float* r, int n) noexcept
{
    const float vol = F ((ld) volume * (ld) expression * (ld) 1.2999999523162842f);
    biquad (5, F ((ld) P (16) * (ld) 20.0f - (ld) 10.0f), 500.0f, rate, 0.0f, 0);
    biquad (4, F ((ld) P (17) * (ld) 20.0f - (ld) 10.0f), 1000.0f, rate, 1.5f, 1);
    biquad (6, F ((ld) P (18) * (ld) 20.0f - (ld) 10.0f), 2500.0f, rate, 0.0f, 2);
    const int fx = fistp ((ld) F ((ld) P (20) * (ld) 15.989999771118164f));
    if (flags != 0)
    {
        ld acc = 0;
        for (int i = 0; i < n; ++i)
            acc = acc - std::fabs ((ld) l[i]);
        if (std::fabs (acc) < (ld) 9.999999974752427e-07)
            ++idle;
    }
    const ld tiny = (ld) 1.000000045813705e-18f;
    const ld p21 = (ld) P (21), p22 = (ld) P (22);
    bool skipAmp = false;
    if (fx >= 2 && fx <= 8 && active != 1)
        skipAmp = true;
    else
        switch (fx)
        {
            case 1: // phaser (frequency shifter) before the amp
                for (int i = 0; i < n; ++i)
                {
                    const float x = F ((ld) l[i] + tiny);
                    float ph = F (p21 * (ld) M (0x858) * (ld) 8.819999694824219f + (ld) M (0x240));
                    M (0x240) = ph;
                    ph = F ((ld) M (0x240) - (ld) fistp ((ld) ph));
                    M (0x240) = ph;
                    const ld y = phaser (F ((ld) M (0x5d0) * p22 * (ld) 0.800000011920929f + (ld) x), ph);
                    M (0x5d0) = F (y);
                    l[i] = F (y + (ld) x);
                }
                break;
            case 2: // tremolo
                for (int i = 0; i < n; ++i)
                {
                    const float x = F ((ld) l[i] + tiny);
                    const int k = fistp ((ld) F (p21 * (ld) 3.990000009536743f));
                    float ph = F ((ld) tempoF / ((ld) rate * (ld) 30.0f) / (ld) fxRate[(std::size_t) k] + (ld) M (0x248));
                    M (0x248) = ph;
                    ph = F ((ld) M (0x248) - (ld) fistp ((ld) ph));
                    M (0x248) = ph;
                    const ld t = tri (ph);
                    l[i] = F ((t * p22 + (ld) 1.0f) * (ld) x);
                }
                break;
            case 3: // wah-wah (envelope)
                for (int i = 0; i < n; ++i)
                {
                    const float x = l[i];
                    const ld ax = std::fabs ((ld) x);
                    const ld c = p21 * (ld) 0.004900000058114529f + (ld) 0.9950000047683716f;
                    const ld e = c * (ld) M (0x5cc) + ax * ((ld) 1.0f - c);
                    M (0x5cc) = F (e);
                    const float a = F ((e * p22 * (ld) 3.0f + (ld) 0.014999999664723873f) * (ld) M (0x858) * (ld) 44100.0f);
                    const ld y = ampFilter (0x4fc, x, a, 0.75f, 0.8999999761581421f);
                    l[i] = F ((y * (ld) 1.5f + (ld) x) * (ld) 0.5f);
                }
                break;
            case 4: // wah-wah (LFO)
                for (int i = 0; i < n; ++i)
                {
                    const float x = l[i];
                    const ld ax = std::fabs ((ld) x);
                    M (0x5cc) = F ((ld) M (0x5cc) * (ld) 0.996999979019165f + ax * (ld) 0.003000020980834961f);
                    float ph = F ((p21 + (ld) 0.10000000149011612f) * (ld) M (0x858) * (ld) 8.819999694824219f + (ld) M (0x244));
                    M (0x244) = ph;
                    ph = F ((ld) M (0x244) - (ld) fistp ((ld) ph));
                    M (0x244) = ph;
                    const ld t = tri (ph);
                    const float a = F (((t + (ld) 1.0f) * (ld) M (0x5cc) * (ld) M (0x5cc) * p22 * (ld) 4.0f + (ld) 0.014999999664723873f)
                                       * (ld) M (0x858) * (ld) 44100.0f);
                    const ld y = ampFilter (0x4fc, x, a, 0.75f, 0.8999999761581421f);
                    l[i] = F ((y * (ld) 1.5f + (ld) x) * (ld) 0.5f);
                }
                break;
            case 5: // ring modulator at the played pitch
                for (int i = 0; i < n; ++i)
                {
                    const float x = F ((ld) l[i] + tiny);
                    const int mult = fistp ((ld) F (p21 * (ld) 15.0f + (ld) 1.0f));
                    const ld f = fexp ((ld) glidePos * (ld) 0.0577622652053833f) * (ld) 8.175749778747559f / (ld) rate;
                    float ph = F ((ld) mult * f * (ld) 0.0625f + (ld) M (0x250));
                    M (0x250) = ph;
                    const ld t = (ld) M (0x250) - (ld) fistp ((ld) ph);
                    M (0x250) = F (t);
                    const ld s = fsin (t * (ld) 6.283180236816406f) * (ld) x;
                    l[i] = F (s + s);
                }
                break;
            case 6: // fuzz box: exponential shaping, then quantisation
                for (int i = 0; i < n; ++i)
                {
                    ld x = (ld) l[i] + tiny;
                    const float c = F ((p21 + (ld) 0.5f) * (ld) 4.0f);
                    ld v;
                    bool neg = x < 0;
                    if (! neg)
                    {
                        v = (fexp ((ld) c * x) - (ld) 1.0f) / (ld) c;
                        if (v > (ld) 0.5f)
                            v = (ld) 0.5f;
                        else if (v < 0)
                        {
                            x = v; // (unreachable for c > 0: the engine re-enters the negative branch with this value)
                            neg = true;
                        }
                    }
                    if (neg)
                    {
                        v = ((ld) 1.0f - fexp (-((ld) c * x))) / (ld) c;
                        if (v < (ld) -0.5f)
                            v = (ld) -0.5f;
                    }
                    const ld q = p22 * (ld) 0.10000000149011612f + (ld) 9.999999747378752e-05f;
                    const int k = fistp ((ld) F (v / q));
                    l[i] = F (q * (ld) k);
                }
                break;
            case 7: // harmonizer: octave from zero crossings
                for (int i = 0; i < n; ++i)
                {
                    const ld x = (ld) l[i] + tiny;
                    const ld ax = std::fabs (x);
                    envA = F ((ld) envA * (ld) 0.996999979019165f + ax * (ld) 0.029999999329447746f);
                    const ld eB = (ld) envB * (ld) 0.9900000095367432f + ax * (ld) 0.009999999776482582f;
                    envB = F (eB);
                    const float diff = F ((ax - eB) * (ld) envA);
                    const float xF = F (x);
                    const ld eC = x * (ld) 0.004999999888241291f + (ld) envC * (ld) 0.9950000047683716f;
                    envC = F (eC);
                    const ld y = ampFilter (0x51c, F ((ld) xF - eC), bitsF (0x3ba3d70a), 0.0f, 1.0f);
                    if (M (0x220) > 0.0f && y < 0)
                        M (0x224) = M (0x224) < 0.0f ? 1.0f : -1.0f;
                    M (0x220) = F (y);
                    const ld eD = (ld) envD * (ld) 0.9919999837875366f + (ld) M (0x224) * (ld) 0.00800000037997961f;
                    envD = F (eD);
                    l[i] = F ((eD * (ld) envA * p22 + (ld) xF) - (ld) diff * p21);
                }
                break;
            case 8: // talk box: three formant band-passes swept by a triangle LFO
                for (int i = 0; i < n; ++i)
                {
                    const ld T = (tri (lfoE) + (ld) 1.0f) * (ld) 0.5f;
                    const ld q = p22 * (ld) 1.5f + (ld) 0.5f;
                    const float xin = F ((ld) l[i] + tiny + (ld) 0.0010000000474974513f);
                    const ld inv = (ld) M (0x858);
                    ld s1 = fsin (((ld) 2250.0f - T * (ld) 1400.0f) * q * inv * (ld) 3.1399197578430176f);
                    const float f1 = F (s1 + s1);
                    ld s2 = fsin (((ld) 2900.0f - T * (ld) 700.0f) * q * inv * (ld) 3.1399197578430176f);
                    const float f2 = F (s2 + s2);
                    const ld u = T * (ld) 0.9700000286102295f - (ld) 0.5f;
                    const ld w = (ld) 270.0f - (u * u - (ld) 0.25f) * (ld) 1560.0f;
                    ld s3 = fsin (q * w * inv * (ld) 3.1399197578430176f);
                    const ld f3 = s3 + s3;
                    ld lo = (ld) f1 * (ld) M (0x840) + (ld) M (0x834);
                    M (0x834) = F (lo);
                    const ld xsE = (ld) xin * (ld) 0.050000011920928955f;
                    const float xs = F (xsE);
                    ld hp = (xsE - lo) - (ld) M (0x840) * (ld) 0.050000011920928955f;
                    M (0x84c) = F (hp);
                    const float bp1 = F (hp * (ld) f1 + (ld) M (0x840));
                    M (0x840) = bp1;
                    lo = (ld) f2 * (ld) M (0x844) + (ld) M (0x838);
                    ++cnt8c;
                    M (0x838) = F (lo);
                    hp = ((ld) xs - lo) - (ld) M (0x844) * (ld) 0.050000011920928955f;
                    M (0x850) = F (hp);
                    const float bp2 = F (hp * (ld) f2 + (ld) M (0x844));
                    M (0x844) = bp2;
                    lo = f3 * (ld) M (0x848) + (ld) M (0x83c);
                    M (0x83c) = F (lo);
                    hp = ((ld) xs - lo) - (ld) M (0x848) * (ld) 0.050000011920928955f;
                    M (0x854) = F (hp);
                    const ld bp3 = f3 * hp + (ld) M (0x848);
                    M (0x848) = F (bp3);
                    const ld v = T - (ld) 0.5f;
                    ld o = ((ld) 0.4000000059604645f - (v * v - (ld) 0.25f) * (ld) 1.600000023841858f) * bp3;
                    o = o + ((ld) 1.0f - T * T * (ld) 0.30000001192092896f) * (ld) bp1;
                    o = o + (ld) bp2 * (ld) 0.8999999761581421f;
                    const float out = F (o * (ld) 8.0f);
                    float ph = F (p21 * (ld) M (0x858) * (ld) 2.2049999237060547f + (ld) lfoE);
                    lfoE = ph;
                    lfoE = F ((ld) lfoE - (ld) fistp ((ld) ph));
                    l[i] = out;
                }
                break;
            default: break;
        }
    if (! skipAmp && active == 1)
    {
        const int ampType = (std::int8_t) ftol ((ld) P (12) * (ld) 3.990000009536743f);
        const float drv = F ((ld) P (13) * (ld) 0.5f + (ld) 0.25f);
        const int cabType = (std::int8_t) ftol ((ld) P (19) * (ld) 3.990000009536743f);
        int taps = 0;
        float fb = 0, mix = 0, gain = 0;
        ld cut = 0;
        switch (cabType)
        {
            case 0: cut = (ld) 0.20000000298023224f, taps = 0x14, fb = 0.0f, mix = 0.0f, gain = bitsF (0x3f333333); break;
            case 1: cut = (ld) 0.25f, taps = 0x40, fb = 0.5f, mix = 1.0f, gain = bitsF (0x3f99999a); break;
            case 2: cut = (ld) 0.17000000178813934f, taps = 0x20, fb = bitsF (0x3f19999a), mix = 1.0f, gain = bitsF (0x3fb33333); break;
            case 3: cut = (ld) 0.33000001311302185f, taps = 0x30, fb = 0.5f, mix = 1.0f, gain = 1.0f; break;
            default: cut = (ld) n; break;
        }
        if (ampType >= 0 && ampType <= 3 && n > 0)
        {
            const float cutF = F (cut * (ld) 44100.0f / (ld) rate);
            const int dl = ftol ((ld) taps * (ld) rate * (ld) 2.2675736545352265e-05f);
            const float om = F ((ld) 1.0f - (ld) mix);
            for (int i = 0; i < n; ++i)
            {
                const float x = l[i];
                ld a;
                if (ampType == 0)
                {
                    const ld dc = (ld) x * (ld) 0.004999999888241291f + (ld) M (0x23c) * (ld) 0.9950000047683716f;
                    M (0x23c) = F (dc);
                    const ld r0 = SLAYER_EQ3 (F (((ld) x - dc) + tiny));
                    a = eqGain (*this) * r0 * (ld) 0.5f * (ld) drv * (ld) 3.0f;
                }
                else if (ampType == 3)
                    a = ampFuzz (x, P (14), drv, rate) * (ld) 0.5f;
                else
                {
                    const ld dc = (ld) x * (ld) 0.004999999888241291f + (ld) M (0x23c) * (ld) 0.9950000047683716f;
                    M (0x23c) = F (dc);
                    const float xx = F ((ld) x - dc);
                    a = (ampType == 1 ? ampClean (xx, P (14), drv, rate) : ampDrive (xx, P (14), drv, rate)) * (ld) 0.5f;
                }
                const float aF = F (a);
                const ld f = ampFilter (0x4dc, aF, cutF, fb, 0.0f) * (ld) 0.25f;
                bufE[(std::size_t) (ampIdx & 0x1ff)] = F (f);
                const ld s = f + (ld) bufE[(std::size_t) ((ampIdx - dl) & 0x1ff)];
                ++ampIdx;
                const ld w0 = fsin ((ld) M (0x858) * (ld) 12566.3603515625f);
                const ld w = w0 + w0;
                if (ampType == 0)
                {
                    const ld ra = w * (ld) M (0x7fc) + (ld) M (0x7f4);
                    M (0x7f4) = F (ra);
                    const ld rb = -ra - (ld) M (0x7fc) * (ld) 0.699999988079071f;
                    M (0x7f8) = F (rb);
                    const ld rc = (w * rb + (ld) M (0x7fc)) * (ld) M (0x864);
                    M (0x7fc) = F (rc);
                    fbRing[(std::size_t) (fbWrite & ringMask)] = F (rc * (ld) 8.0f);
                    ++fbWrite;
                    l[i] = F (((ld) aF * (ld) om + s * (ld) mix) * (ld) gain * (ld) vol);
                }
                else
                {
                    const ld y = ((ld) aF * (ld) om + s * (ld) mix) * (ld) gain;
                    const ld ra = w * (ld) M (0x7fc) + (ld) M (0x7f4);
                    M (0x7f4) = F (ra);
                    const ld rb = (y * (ld) 0.699999988079071f - ra) - (ld) M (0x7fc) * (ld) 0.699999988079071f;
                    M (0x7f8) = F (rb);
                    const ld rc = (w * rb + (ld) M (0x7fc)) * (ld) M (0x864);
                    M (0x7fc) = F (rc);
                    fbRing[(std::size_t) (fbWrite & ringMask)] = F (rc * (ld) 8.0f);
                    ++fbWrite;
                    l[i] = F (y * (ld) vol);
                }
            }
        }
        cabinetProcess (l, n);
    }
    std::memcpy (r, l, sizeof (float) * (std::size_t) n);
    switch (fx)
    {
        case 9: // chorus
        {
            const float base = F ((ld) fistp ((ld) F ((ld) rate * (ld) 0.00907029490917921f)));
            const float depth = F (p22 * (ld) rate * (ld) 0.00290249427780509f);
            const float inc = F (p21 * (ld) M (0x858) * (ld) 4.409999847412109f);
            for (int i = 0; i < n; ++i)
            {
                const float x = l[i];
                bufA[(std::size_t) (cnt8c & 0xfff)] = x;
                const float t = F (tri (lfoE));
                float ph = F ((ld) inc + (ld) lfoE);
                lfoE = ph;
                lfoE = F ((ld) lfoE - (ld) fistp ((ld) ph));
                const float p1 = F (((ld) t + (ld) 1.0f) * (ld) depth + (ld) base);
                const int k1 = fistp ((ld) p1);
                const float fr1 = F ((ld) p1 - (ld) k1);
                const float p2 = F (((ld) 1.0f - (ld) t) * (ld) depth + (ld) base);
                const int k2 = fistp ((ld) p2);
                const ld fr2 = (ld) p2 - (ld) k2;
                const float A = F (((ld) 1.0f - (ld) fr1) * (ld) bufA[(std::size_t) ((cnt8c - k1) & 0xfff)]
                                   + (ld) fr1 * (ld) bufA[(std::size_t) ((cnt8c - k1 - 1) & 0xfff)]);
                const float xh = F ((ld) x * (ld) 0.5f);
                const ld B = ((ld) 1.0f - fr2) * (ld) bufA[(std::size_t) ((cnt8c - k2) & 0xfff)] + fr2 * (ld) bufA[(std::size_t) ((cnt8c - k2 - 1) & 0xfff)];
                l[i] = F ((B * (ld) 0.5f + (ld) xh) * (ld) 1.600000023841858f);
                r[i] = F (((ld) A * (ld) 0.5f + (ld) xh) * (ld) 1.600000023841858f);
                ++cnt8c;
            }
            break;
        }
        case 10: // flanger (stereo, with feedback)
        {
            const float base = F ((ld) fistp ((ld) F ((ld) rate * (ld) 0.00580498855561018f)));
            const float depth = F (p22 * (ld) rate * (ld) 0.00290249427780509f);
            const float inc = F (p21 * (ld) 1.7639999389648438f / (ld) rate);
            for (int i = 0; i < n; ++i)
            {
                const float x = l[i];
                bufA[(std::size_t) (cnt8c & 0xfff)] = F ((ld) tapA * (ld) 0.800000011920929f + (ld) x);
                bufB[(std::size_t) (cnt8c & 0xfff)] = F ((ld) tapB * (ld) 0.800000011920929f + (ld) x);
                const float t = F (tri (lfoE));
                float ph = F ((ld) inc + (ld) lfoE);
                lfoE = ph;
                lfoE = F ((ld) lfoE - (ld) fistp ((ld) ph));
                const float p1 = F (((ld) t + (ld) 1.0f) * (ld) depth + (ld) base);
                const float p2 = F (((ld) 1.0f - (ld) t) * (ld) depth + (ld) base);
                const int k1 = fistp ((ld) p1);
                const float fr1 = F ((ld) p1 - (ld) k1);
                const int k2 = fistp ((ld) p2);
                const ld fr2 = (ld) p2 - (ld) k2;
                const float A = F (((ld) 1.0f - (ld) fr1) * (ld) bufA[(std::size_t) ((cnt8c - k1) & 0xfff)]
                                   + (ld) fr1 * (ld) bufA[(std::size_t) ((cnt8c - k1 - 1) & 0xfff)]);
                const float B = F (((ld) 1.0f - fr2) * (ld) bufB[(std::size_t) ((cnt8c - k2) & 0xfff)]
                                   + fr2 * (ld) bufB[(std::size_t) ((cnt8c - k2 - 1) & 0xfff)]);
                const ld xh = (ld) x * (ld) 0.5f;
                l[i] = F (((ld) A * (ld) 0.5f + xh) * (ld) 1.600000023841858f);
                r[i] = F (((ld) B * (ld) 0.5f + xh) * (ld) 1.600000023841858f);
                ++cnt8c;
                const float ta = F ((ld) tapC * (ld) 0.699999988079071f + (ld) A * (ld) 0.30000001192092896f);
                tapA = ta;
                const float tb = F ((ld) B * (ld) 0.30000001192092896f + (ld) tapD * (ld) 0.699999988079071f);
                tapB = tb;
                tapC = ta;
                tapD = tb;
            }
            break;
        }
        case 11: // stereo phaser (frequency shifter)
            for (int i = 0; i < n; ++i)
            {
                const float x = F ((ld) l[i] + tiny);
                float ph = F (p21 * (ld) M (0x858) * (ld) 8.819999694824219f + (ld) M (0x240));
                M (0x240) = ph;
                ph = F ((ld) M (0x240) - (ld) fistp ((ld) ph));
                M (0x240) = ph;
                const ld y = phaser (F ((ld) M (0x5d0) * p22 * (ld) 0.800000011920929f + (ld) x), ph) + tiny;
                M (0x5d0) = F (y);
                l[i] = F ((ld) x - y);
                r[i] = F (y + (ld) x);
            }
            break;
        case 12: // leslie
        {
            if (active != 1)
                break;
            for (int i = 0; i < n; ++i)
            {
                const float D = F (((ld) 1.0f - p21) * ((ld) rate * (ld) 0.0009070294909179211f));
                bufA[(std::size_t) (cnt8c & 0x1ff)] = l[i];
                const float q = F ((ld) lfoE + (ld) 0.25f);
                const float d1 = F ((tri (q) + (ld) 1.0f) * (ld) D);
                const float d2 = F (((ld) 1.0f - tri (q)) * (ld) D);
                const int k1 = fistp ((ld) d1);
                const float fr1 = F ((ld) d1 - (ld) fistp ((ld) d1));
                const int k2 = fistp ((ld) d2);
                const float fr2 = F ((ld) d2 - (ld) fistp ((ld) d2));
                const int i1a = fistp ((ld) F ((ld) cnt8c + (ld) k1 + (ld) 3.0f));
                const int i1b = fistp ((ld) F ((ld) (k1 + 1) + (ld) cnt8c + (ld) 3.0f));
                const float A = F (((ld) 1.0f - (ld) fr1) * (ld) bufA[(std::size_t) (i1a & 0x1ff)] + (ld) fr1 * (ld) bufA[(std::size_t) (i1b & 0x1ff)]
                                   + (ld) 0.0010000000474974513f);
                const int i2a = fistp ((ld) F ((ld) cnt8c + (ld) k2 + (ld) 3.0f));
                const int i2b = fistp ((ld) F ((ld) (k2 + 1) + (ld) cnt8c + (ld) 3.0f));
                const float B = F (((ld) 1.0f - (ld) fr2) * (ld) bufA[(std::size_t) (i2a & 0x1ff)] + (ld) fr2 * (ld) bufA[(std::size_t) (i2b & 0x1ff)]
                                   + (ld) 0.0010000000474974513f);
                float ph = F ((p21 + (ld) 0.20000000298023224f) * (ld) M (0x858) * (ld) 8.819999694824219f + (ld) lfoE);
                lfoE = ph;
                ph = F ((ld) lfoE - (ld) fistp ((ld) ph));
                lfoE = ph;
                ++cnt8c;
                const ld T = tri (ph);
                const ld a = (T + (ld) 1.0f) * (ld) 0.5f;
                const float b = F (((ld) 1.0f - T) * (ld) 0.5f);
                const ld inv = (ld) M (0x858);
                ld s1 = fsin ((a * a * (ld) 0.8999999761581421f + (ld) 0.10000000149011612f) * inv * (ld) 44100.0f * (ld) 0.800000011920929f);
                const ld f1 = s1 + s1;
                ld s2 = fsin (((ld) b * (ld) b * (ld) 0.8999999761581421f + (ld) 0.10000000149011612f) * inv * (ld) 44100.0f * (ld) 0.800000011920929f);
                const float f2 = F (s2 + s2);
                ld lo = f1 * (ld) M (0x840) + (ld) M (0x834);
                M (0x834) = F (lo);
                ld hp = ((ld) B * (ld) 0.699999988079071f - lo) - (ld) M (0x840) * (ld) 0.699999988079071f;
                M (0x84c) = F (hp);
                M (0x840) = F (f1 * hp + (ld) M (0x840));
                lo = (ld) f2 * (ld) M (0x844) + (ld) M (0x838);
                M (0x838) = F (lo);
                hp = ((ld) A * (ld) 0.699999988079071f - lo) - (ld) M (0x844) * (ld) 0.699999988079071f;
                M (0x850) = F (hp);
                M (0x844) = F ((ld) f2 * hp + (ld) M (0x844));
                const ld g = T * p22;
                const float og = F ((ld) 1.0f - g);
                l[i] = F ((g + (ld) 1.0f) * (ld) M (0x834) * (ld) 1.7999999523162842f);
                r[i] = F ((ld) og * (ld) M (0x838) * (ld) 1.7999999523162842f);
            }
            break;
        }
        case 13: // tremolo (after the amp)
        {
            if (active != 1)
                break;
            for (int i = 0; i < n; ++i)
            {
                const int k = fistp ((ld) F (p21 * (ld) 3.990000009536743f));
                const float x = l[i];
                float ph = F ((ld) tempoF / ((ld) rate * (ld) 30.0f) / (ld) fxRate[(std::size_t) k] + (ld) M (0x248));
                M (0x248) = ph;
                ph = F ((ld) M (0x248) - (ld) fistp ((ld) ph));
                M (0x248) = ph;
                l[i] = F ((tri (ph) * p22 + (ld) 1.0f) * (ld) x);
            }
            std::memcpy (r, l, sizeof (float) * (std::size_t) n);
            break;
        }
        case 14: // dub delay
        {
            const int D = fistp ((ld) F (p21 * (ld) rate * (ld) 0.09287981688976288f)) + 1;
            for (int i = 0; i < n; ++i)
            {
                cnt8c = (cnt8c + 1) & 0x7fff;
                const float x = l[i];
                const float y = bufA[(std::size_t) ((cnt8c - D) & 0x7fff)];
                bufA[(std::size_t) cnt8c] = x;
                l[i] = y;
                r[i] = x;
            }
            break;
        }
        case 15: // multitap
        {
            const int k = fistp ((ld) F (p21 * (ld) 3.990000009536743f));
            int L1 = fistp ((ld) F ((ld) rate * (ld) 60.0f / (ld) tempoF * (ld) fxRate[(std::size_t) k]));
            int L2 = fistp ((ld) F ((ld) rate * (ld) 0.001451247138902545f));
            L1 = L1 < 1 ? 1 : std::min (L1, 0x40000);
            L2 = L2 < 1 ? 1 : std::min (L2, 0x40000);
            const int half = L1 / 2 + 1;
            const ld fbk = p22;
            for (int i = 0; i < n; ++i)
            {
                cnt94 = (cnt94 + 1) % L2;
                const ld x = (ld) l[i] + tiny;
                const float d0 = bufD[(std::size_t) cnt94];
                bufD[(std::size_t) cnt94] = F (x);
                cnt90 = (cnt90 + 1) % half;
                const ld t = (ld) tapA * (ld) 0.800000011920929f + (ld) bufC[(std::size_t) cnt90] * (ld) 0.20000000298023224f;
                const float tA = F (t);
                tapA = tA;
                bufC[(std::size_t) cnt90] = F (x);
                cnt8c = (cnt8c + 1) % L1;
                if (cnt8c < 0)
                    cnt8c += L1;
                const ld y = ((ld) tapC * (ld) 0.800000011920929f + (ld) bufA[(std::size_t) cnt8c] * (ld) 0.20000000298023224f) * fbk + x;
                bufA[(std::size_t) cnt8c] = F (y);
                tapC = F (y);
                const ld z = ((ld) bufB[(std::size_t) cnt8c] * (ld) 0.20000000298023224f + (ld) tapD * (ld) 0.800000011920929f) * fbk + (ld) tA;
                const float zF = F (z);
                bufB[(std::size_t) cnt8c] = zF;
                tapD = zF;
                l[i] = F (y * (ld) 0.699999988079071f);
                r[i] = F (((ld) d0 + (ld) zF) * (ld) 0.699999988079071f);
            }
            break;
        }
        default: break;
    }
}

double Engine::peek (int off) noexcept
{
    if (off == 0x24c)
        return idle;
    if (off == 0x2b0)
        return lastMode;
    if (off >= 0 && off < 0x868)
        return M (off);
    if (off >= 0x520ce8 && off < 0x520ce8 + 6 * 0x80198)
    {
        auto& s = str[(std::size_t) ((off - 0x520ce8) / 0x80198)];
        const int o = (off - 0x520ce8) % 0x80198;
        switch (o)
        {
            case 0xc: return s.note;
            case 0x10: return s.vel;
            case 0x14: return s.velHeld;
            case 0x1c: return s.env;
            case 0x20: return s.envPhase;
            case 0x28: return s.gate;
            case 0x2c: return s.envState;
            case 0x30: return s.level;
            case 0x34: return s.envCopy;
            case 0xc4: return s.idxA;
            case 0x80154: return s.lpA;
            case 0x80158: return s.slide;
            case 0x80164: return s.lpC;
            case 0x80174: return s.releaseTimer;
            case 0x80178: return s.pickPos;
            case 0x80180: return s.hist[0];
            case 0x80184: return s.hist[1];
            case 0x80190: return s.phase;
            case 0x80194: return s.delay;
            default:
                if (o >= 0xc8 && o < 0x200c8)
                    return s.ringA[(std::size_t) (o - 0xc8) / 4];
                if (o >= 0x200c8 && o < 0x400c8)
                    return s.ringB[(std::size_t) (o - 0x200c8) / 4];
                if (o >= 0x4014c && o < 0x6014c)
                    return s.ringC[(std::size_t) (o - 0x4014c) / 4];
                return 0;
        }
    }
    if (off >= 0x8216f4 && off < 0x8216f4 + 28 * 4)
        return prm[(std::size_t) (off - 0x8216f4) / 4];
    switch (off)
    {
        case 0x24c: return idle;
        case 0x520cd4: return rate;
        case 0x520cdc: return tempoF;
        case 0x520c98: return lfoE;
        case 0x520cc0: return fbWrite;
        case 0x520cc4: return fbRead;
        case 0x520cac: return ampIdx;
        case 0x821764: return chunkCounter;
        case 0x829810: return glidePos;
        case 0x8297fc: return flags;
        default: break;
    }
    if (off >= 0x868 && off < 0x868 + 0x20000)
        return fbRing[(std::size_t) (off - 0x868) / 4];
    if (off >= 0x420868 && off < 0x420868 + 0x800)
        return bufE[(std::size_t) (off - 0x420868) / 4];
    return 0;
}

// ------------------------------------------------------------------------------------------------ the front-end (§6)
constexpr int paramMax[29] = { 6, 2, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 3, 65535, 65535,
                               65535, 65535, 65535, 65535, 3, 15, 65535, 65535, 65535, 5, 65535, 1, 1, 127 };

// the state: int version (0: 28 parameters follow, 1: 29), then the parameter integers
bool parseState (const std::vector<std::uint8_t>& st, std::array<int, 29>& out)
{
    if (st.size() < 4)
        return false;
    std::int32_t version = 0;
    std::memcpy (&version, st.data(), 4);
    const std::size_t count = version == 0 ? 28 : version == 1 ? 29 : 0;
    if (count == 0 || st.size() < 4 + 4 * count)
        return false;
    out.fill (0);
    for (std::size_t i = 0; i < count; ++i)
        std::memcpy (&out[i], st.data() + 4 + 4 * i, 4);
    return true;
}

// MIDI velocity from Init.Vol as the front-end computes it (TriggerVoice): Round(ln(10·v + 1)/ln(21.551182)·127), ≤ 127
int velocityFromInitVol (double initVol) noexcept
{
    const auto r = (float) (std::log ((double) ((float) initVol * 10.0f + 1.0f)) / std::log ((double) (20.55118179321289f + 1.0f)));
    const auto v = std::nearbyint ((double) (r * 127.0f));
    return (int) std::min (127.0, v);
}

// key from Final.Pitch (cents relative to C5 = key 60 with 6000 added by the front-end): Round(pitch·0.01) clamped 0…127
int keyFromPitch (float finalPitchPlus6000) noexcept
{
    auto f = finalPitchPlus6000 * 0.009999999776482582f;
    if (f <= 0.0f)
        f = 0.0f;
    const auto k = (long long) std::nearbyint ((double) f);
    return (int) std::clamp (k, 0ll, 127ll);
}

// ProcessEvent 4 (MIDI pitch, cents) → the pitch bend the front-end sends: MulDiv(cents, 8192, 1200) + 8192 (64-bit
// multiply, truncating divide), 0 below 1, at most 0x3fff (REA_rodada79 §2.2)
int bendFromCents (int cents) noexcept
{
    auto v = (long long) cents * 8192 / 1200 + 0x2000;
    if (v < 1)
        v = 0;
    return (int) std::min (v, 0x3fffll);
}

// FL's channel pan as ProcessEvent 2 sends it (the event's flags): (pan − 6400)/100 truncated, clamped to −64…64; LYNX
// keeps the channel pan as (pan − 6400)/6400
int midiPanFrom (double pan) noexcept
{
    const auto p = (long long) std::llround (pan * 6400.0);
    return (int) std::clamp (p / 100, -64ll, 64ll);
}

// the front-end's output gains: host ComputeLRVol (slot 3: integer pan / 64, FL's cos/sin law) of the MIDI pan and of
// twice the volume ProcessEvent 3 carries (the channel's gain as a float)
void hostGainsFor (int pan, float channelGain, float& l, float& r) noexcept
{
    flvp::panLaw ((float) pan * 0.015625f, channelGain * 2.0f, l, r);
}

// ------------------------------------------------------------------------------------------------ LYNX instrument
// FL Slayer as a LYNX instrument: the engine driven like the front-end drives it (§6 of round 68, round 79). FL sends
// this plugin the channel's levels as MIDI events (its info flags carry 0x4000): pan and volume become the front-end's
// output gains (FL does not scale the plugin's output itself), the channel pitch a pitch bend; the root note stays in
// the key. Parameter automation and state edits reach the running engine as setParameter calls, like FL's
// ProcessParam; only a new DLL or sample rate rebuilds it.
class SlayerInstrument final : public Instrument
{
public:
    ~SlayerInstrument() override
    {
        delete incoming.exchange (nullptr);
        delete retired.exchange (nullptr);
    }

    void prepare (double sr) noexcept override
    {
        delete retired.exchange (nullptr);
        take();
        if (sr == rate.load() && engine != nullptr)
            return;
        rate.store (sr);
        delete incoming.exchange (build().release());
        take();
    }

    void configure (const ChannelDef& channel, double bpm) noexcept override
    {
        delete retired.exchange (nullptr);
        tempo.store (bpm > 0.0 ? bpm : 140.0);
        // channel pitch (knob + fine) → ProcessEvent 4; the root-note shift (kept in modelParams[0] by the importer)
        // stays in Final.Pitch, i.e. in the key
        const auto root = channel.modelParams.empty() || ! std::isfinite (channel.modelParams[0]) ? 0.0f : channel.modelParams[0];
        keyCents.store ((float) std::lround (root * 100.0f));
        bendCents.store ((int) std::lround ((channel.sampler.pitch - root) * 100.0f));
        if (channel.pluginState == state && channel.samplePath == dllPath)
            return;
        std::array<int, 29> next {};
        const auto nextUsable = parseState (channel.pluginState, next);
        const auto sameDll = channel.samplePath == dllPath && data != nullptr;
        state = channel.pluginState;
        if (sameDll && usable && nextUsable)
        {
            // an edit of the plugin's settings: FL's ProcessParam → setParameter on the running engine (no restart)
            params = next;
            for (std::size_t i = 0; i < 29; ++i)
                uiParams[i].store (next[i]);
            uiGeneration.fetch_add (1);
            return;
        }
        params = next;
        usable = nextUsable;
        dllPath = channel.samplePath;
        data = loadDll (dllPath);
        for (std::size_t i = 0; i < 29; ++i)
            uiParams[i].store (params[i]);
        delete incoming.exchange (build().release());
    }

    void setNextNoteExpression (float, float cents, bool) noexcept override { nextCents = cents; }

    // a slide note (issue #14): FL hands this MIDI-driven plugin its pitch as a pitch bend (ProcessEvent 4), so the
    // glide from the last note to the slide's pitch goes out the same way, on top of the channel pitch
    void slideTo (int key, float cents, float samples) noexcept override
    {
        if (lastKey < 0 || std::none_of (sounding.begin(), sounding.end(), [] (int k) { return k >= 0; }))
            return;
        slide.start (100.0f * (float) (key - lastKey) + cents - lastCents, samples);
    }

    // automation of a plugin parameter (0…1): ProcessParam with REC_FromMIDI → min + Round(value·(max − min))
    void setParameter (int index, float value) noexcept override
    {
        take();
        if (engine == nullptr || index < 0 || index >= 29 || ! std::isfinite (value))
            return;
        const auto v = (int) std::nearbyint ((double) std::clamp (value, 0.0f, 1.0f) * (double) paramMax[index]);
        send (index, v);
    }

    void noteOn (int key, float velocity) noexcept override
    {
        take();
        if (engine == nullptr)
            return;
        const auto k = std::clamp (key, 0, 127);
        // FL: Init.Vol = (11^(1.28·velocity) − 1)/10 (the channel's default velocity curve), Final.Pitch in cents
        const auto initVol = (std::pow (11.0, 1.28 * std::clamp ((double) velocity, 0.0, 1.0)) - 1.0) / 10.0;
        const int v = velocityFromInitVol (initVol);
        const int midiKey = keyFromPitch ((float) ((k - 60) * 100) + nextCents + keyCents.load() + 6000.0f);
        lastKey = k;
        lastCents = nextCents;
        slide = {};
        nextCents = 0.0f;
        if (sounding[(std::size_t) k] >= 0)
            engine->midi (0x80, sounding[(std::size_t) k], soundingVel[(std::size_t) k]);
        sounding[(std::size_t) k] = midiKey;
        soundingVel[(std::size_t) k] = v;
        engine->midi (0x90, midiKey, v);
    }

    void noteOff (int key) noexcept override
    {
        take();
        const auto k = std::clamp (key, 0, 127);
        if (engine == nullptr || sounding[(std::size_t) k] < 0)
            return;
        engine->midi (0x80, sounding[(std::size_t) k], soundingVel[(std::size_t) k]);
        sounding[(std::size_t) k] = -1;
    }

    void allNotesOff() noexcept override
    {
        for (int k = 0; k < 128; ++k)
            noteOff (k);
    }

    void render (float* left, float* right, int n, float gl, float gr) noexcept override
    {
        take();
        if (engine == nullptr || n <= 0)
            return;
        engine->tempo = tempo.load();
        // settings edited while playing (only the integers that changed, so automation is not overwritten)
        const auto generation = uiGeneration.load();
        if (generation != appliedGeneration)
        {
            appliedGeneration = generation;
            for (std::size_t i = 0; i < 29; ++i)
            {
                const auto v = uiParams[i].load();
                if (v != uiApplied[i])
                {
                    uiApplied[i] = v;
                    send ((int) i, v);
                }
            }
        }
        // channel pitch: ProcessEvent 4 → a pitch bend, sent when it changes
        auto sendBend = [this] {
            const auto bend = bendFromCents (bendCents.load() + (int) std::lround (slide.cents));
            if (bend != engine->sentBend)
            {
                engine->sentBend = bend;
                engine->midi (0xe0, bend & 0x7f, bend >> 7);
            }
        };
        sendBend();
        // channel volume and pan (LYNX's gains: g·√2·cos/sin((1 + pan)·π/4)) → ProcessEvent 3 / 2 → the front-end's gains
        const auto vol = std::sqrt (((double) gl * gl + (double) gr * gr) * 0.5);
        const auto pan = vol > 0.0 ? std::atan2 ((double) gr, (double) gl) * (4.0 / 3.141592653589793) - 1.0 : 0.0;
        float hostL = 0.0f, hostR = 0.0f;
        hostGainsFor (midiPanFrom (pan), (float) vol, hostL, hostR);
        for (int done = 0; done < n;)
        {
            auto m = std::min (n - done, (int) scratchL.size());
            if (slide.moving()) // a slide's bend moves in steps of 64 samples
            {
                m = std::min (m, 64);
                slide.advance ((float) m);
                sendBend();
            }
            std::fill (scratchL.begin(), scratchL.begin() + m, 0.0f);
            std::fill (scratchR.begin(), scratchR.begin() + m, 0.0f);
            engine->process (scratchL.data(), scratchR.data(), m);
            for (int i = 0; i < m; ++i)
            {
                left[done + i] += scratchL[(std::size_t) i] * hostL;
                right[done + i] += scratchR[(std::size_t) i] * hostR;
            }
            done += m;
        }
    }

private:
    // ProcessParam with the update flag: setParameter(i, v/max) for the 28 engine parameters, MIDI CC 1 for the 29th
    void send (int i, int v) noexcept
    {
        if (engine->sentParams[(std::size_t) i] == v)
            return;
        engine->sentParams[(std::size_t) i] = v;
        if (i < 28)
            engine->setParameter (i, (float) v / (float) paramMax[i]);
        else
            engine->midi (0xb0, 1, v);
    }

    std::unique_ptr<Engine> build() const
    {
        if (! usable || data == nullptr)
            return nullptr;
        auto e = std::make_unique<Engine> (data);
        for (int i = 0; i < 28; ++i)
            e->setParameter (i, (float) params[(std::size_t) i] / (float) paramMax[i]);
        e->setSampleRate ((float) rate.load());
        e->resume();
        e->midi (0xe0, 0, 64);              // the front-end's centred pitch bend after loading
        e->midi (0xb0, 1, params[28]);      // parameter 28 (mod wheel)
        e->sentParams = params;
        e->sentBend = 0x2000;
        e->tempo = tempo.load();
        float l = 0.0f, r = 0.0f;
        e->process (&l, &r, 1);              // one sample in: the plugin has been running before the first note (§6.3)
        return e;
    }

    void take() noexcept
    {
        if (retired.load() != nullptr)
            return;
        if (auto* s = incoming.exchange (nullptr))
        {
            retired.store (engine.release());
            engine.reset (s);
            sounding.fill (-1);
            uiApplied = engine->sentParams;
            appliedGeneration = uiGeneration.load();
        }
    }

    std::unique_ptr<Engine> engine;
    std::atomic<Engine*> incoming { nullptr }, retired { nullptr };
    std::array<int, 29> params {};
    bool usable = false;
    std::shared_ptr<const DllData> data;
    std::atomic<double> rate { 44100.0 }, tempo { 140.0 };
    std::atomic<float> keyCents { 0.0f };
    std::atomic<int> bendCents { 0 };
    int lastKey = -1;      // the last note played (a slide glides from it)
    float lastCents = 0.0f; // its fine pitch + main pitch
    HostSlide slide;       // the slide's bend (cents) on top of the channel pitch
    std::array<std::atomic<int>, 29> uiParams {};
    std::atomic<int> uiGeneration { 0 };
    int appliedGeneration = 0;
    std::array<int, 29> uiApplied {};
    std::vector<std::uint8_t> state;
    std::string dllPath;
    std::array<int, 128> sounding = [] { std::array<int, 128> a {}; a.fill (-1); return a; }();
    std::array<int, 128> soundingVel {};
    float nextCents = 0.0f;
    std::array<float, 512> scratchL {}, scratchR {};
};
} // namespace

// ------------------------------------------------------------------------------------------------ public API
bool slayerStateUsable (const std::vector<std::uint8_t>& state)
{
    std::array<int, 29> p {};
    return parseState (state, p);
}

std::vector<int> slayerStateParams (const std::vector<std::uint8_t>& state)
{
    std::array<int, 29> p {};
    if (! parseState (state, p))
        return {};
    return std::vector<int> (p.begin(), p.end());
}

int slayerParamMax (int index) noexcept { return index >= 0 && index < 29 ? paramMax[index] : 1; }

bool slayerDllUsable (const std::string& path) { return loadDll (path) != nullptr; }

std::unique_ptr<Instrument> makeSlayerInstrument() { return std::make_unique<SlayerInstrument>(); }

namespace slayer
{
int midiVelocity (double initVol) noexcept { return velocityFromInitVol (initVol); }

int pitchBend (int cents) noexcept { return bendFromCents (cents); }

int midiPan (double pan) noexcept { return midiPanFrom (pan); }

void hostGains (int pan, float channelGain, float& left, float& right) noexcept { hostGainsFor (pan, channelGain, left, right); }

std::vector<float> renderReference (const std::vector<int>& params, const std::string& dll, const std::vector<RefNote>& notes,
                                    double sampleRate, int length, const RefOptions& options)
{
    auto data = loadDll (dll);
    if (data == nullptr || params.size() < 28 || length <= 0)
        return {};
    auto e = std::make_unique<Engine> (data);
    e->tempo = options.tempo;
    for (int i = 0; i < 28; ++i)
        e->setParameter (i, (float) params[(std::size_t) i] / (float) paramMax[i]);
    e->setSampleRate ((float) sampleRate);
    e->resume();
    struct Ev { int at, status, d1, d2; };
    std::vector<Ev> ev;
    ev.push_back ({ 0, 0xe0, 0, 64 });
    ev.push_back ({ 0, 0xb0, 1, params.size() > 28 ? params[28] : 0 });
    for (const auto& n : notes)
    {
        ev.push_back ({ n.start, 0x90, n.key, n.velocity });
        if (n.release >= 0 && n.release < length)
            ev.push_back ({ n.release, 0x80, n.key, n.velocity });
    }
    for (const auto& m : options.midi)
        ev.push_back ({ m[0], m[1], m[2], m[3] });
    // parameter changes (status −1: setParameter, or CC 1 for index 28) sorted before the MIDI of the same sample
    for (const auto& p : options.params)
        if (p[1] >= 0 && p[1] < 29)
            ev.push_back ({ p[0], -1, p[1], p[2] });
    std::stable_sort (ev.begin(), ev.end(), [] (const Ev& a, const Ev& b) {
        return a.at != b.at ? a.at < b.at : (a.status == -1 && b.status != -1);
    });
    std::vector<float> out ((std::size_t) length * 2, 0.0f);
    std::vector<float> l ((std::size_t) options.block), r ((std::size_t) options.block);
    std::size_t next = 0;
    for (int pos = 0; pos < length;)
    {
        int len = std::min (options.block, length - pos);
        while (next < ev.size() && ev[next].at <= pos)
        {
            if (ev[next].status == -1 && ev[next].d1 < 28)
                e->setParameter (ev[next].d1, (float) ev[next].d2 / (float) paramMax[ev[next].d1]);
            else if (ev[next].status == -1)
                e->midi (0xb0, 1, ev[next].d2);
            else
                e->midi (ev[next].status, ev[next].d1, ev[next].d2);
            ++next;
        }
        if (next < ev.size() && ev[next].at < pos + len)
            len = ev[next].at - pos;
        std::fill (l.begin(), l.begin() + len, 0.0f);
        std::fill (r.begin(), r.begin() + len, 0.0f);
        e->process (l.data(), r.data(), len);
        if (! options.peek.empty())
        {
            std::printf ("#blk %d", pos);
            for (auto o : options.peek)
                std::printf (" %.9g", e->peek (o));
            std::printf ("\n");
        }
        for (int i = 0; i < len; ++i)
        {
            out[2 * (std::size_t) (pos + i)] = l[(std::size_t) i];
            out[2 * (std::size_t) (pos + i) + 1] = r[(std::size_t) i];
        }
        pos += len;
    }
    return out;
}
} // namespace slayer
} // namespace lynx
