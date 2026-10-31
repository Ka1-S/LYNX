// FL Studio's BeepMap (REA_rodada65 §2). See BeepMap.h.

#include "core/BeepMap.h"
#include "core/HostSlide.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>

#include "core/FlVoiceProcessor.h"

// The DLL's oscillators run on the x87 with 64-bit precision (long double here) and its control code in float/double;
// this file keeps those types and the operation order, so contraction stays off.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")
#endif

namespace lynx
{
namespace
{
using flvp::roundEven;
using ld = long double;

constexpr int settingCount = 15;
constexpr int snapshotSize = flvp::snapshotSize;
constexpr int maxVoices = 64;
constexpr int maxRows = 1024;                       // (FL has no limit but the Max height knob)
const double ln2Cents = (std::log (2.0) / 12.0) / 100.0;
constexpr double c5Phase = 2247346493527.166;       // 523.2511306…·2^32 (DAT_0072ea30)
constexpr double c5Hz = 523.251130601197;           // DAT_0072fb10

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
struct X87Extended
{
    unsigned short saved = 0;
    X87Extended() noexcept
    {
        asm volatile ("fnstcw %0" : "=m"(saved));
        unsigned short cw = (unsigned short) (saved | 0x300);
        asm volatile ("fldcw %0" : : "m"(cw));
    }
    ~X87Extended() { asm volatile ("fldcw %0" : : "m"(saved)); }
};
#else
struct X87Extended
{
};
#endif

std::int32_t i32At (const std::uint8_t* d) noexcept
{
    std::int32_t v;
    std::memcpy (&v, d, 4);
    return v;
}

// ------------------------------------------------------------------------------------------------------- state

// A decoded bitmap: rows top to bottom, B, G, R per pixel
struct Image
{
    int width = 0, height = 0;
    std::vector<std::uint8_t> bgr; // 3·width·height
    const std::uint8_t* pixel (int x, int y) const noexcept { return bgr.data() + 3 * ((std::size_t) y * (std::size_t) width + (std::size_t) x); }
};

// uncompressed 24/32-bit BMP (BITMAPINFOHEADER or later); bottom-up or top-down
bool decodeBmp (const std::uint8_t* b, std::size_t n, Image& out)
{
    if (n < 54 || b[0] != 'B' || b[1] != 'M')
        return false;
    const auto offset = (std::size_t) (std::uint32_t) i32At (b + 10);
    const auto hdr = (std::uint32_t) i32At (b + 14);
    if (hdr < 40 || 14 + (std::size_t) hdr > n)
        return false;
    const auto w = i32At (b + 18), hRaw = i32At (b + 22);
    const auto bpp = (int) (b[28] | b[29] << 8);
    const auto compression = (std::uint32_t) i32At (b + 30);
    if (w <= 0 || w > 1 << 16 || hRaw == 0 || std::abs ((long long) hRaw) > 1 << 16 || (bpp != 24 && bpp != 32)
        || ! (compression == 0 || (compression == 3 && bpp == 32)))
        return false;
    const auto h = std::abs (hRaw);
    const auto stride = (((std::size_t) w * (std::size_t) bpp + 31) / 32) * 4;
    if (offset > n || stride * (std::size_t) h > n - offset)
        return false;
    out.width = w, out.height = h;
    out.bgr.assign (3 * (std::size_t) w * (std::size_t) h, 0);
    for (int y = 0; y < h; ++y)
    {
        const auto src = hRaw > 0 ? h - 1 - y : y; // bottom-up files store the last row first
        const auto* row = b + offset + stride * (std::size_t) src;
        for (int x = 0; x < w; ++x)
            std::memcpy (out.bgr.data() + 3 * ((std::size_t) y * (std::size_t) w + (std::size_t) x), row + (std::size_t) x * (std::size_t) (bpp / 8), 3);
    }
    return true;
}

struct Params
{
    std::array<std::int32_t, settingCount> P {};
    std::array<std::uint8_t, snapshotSize> snap = flvp::defaultSnapshot();
    std::shared_ptr<const Image> image;
};

// FUN_0072ebc0 (load): version 0 = 15 int32 settings; version 1 = the same + the 364-byte channel snapshot (taken only
// when complete); then the bitmap as a stream (int32 size + a BMP file)
bool parse (const std::vector<std::uint8_t>& d, Params& out, std::size_t* settingsAt = nullptr)
{
    if (d.size() < 4)
        return false;
    const auto version = i32At (d.data());
    if (version < 0 || version > 1)
        return false;
    Params p;
    std::size_t at = 4;
    const std::size_t body = version == 0 ? 0x3c : 0x1a8;
    if (d.size() < at + body + 4)
        return false;
    for (int k = 0; k < settingCount; ++k)
        p.P[(std::size_t) k] = i32At (d.data() + at + 4 * (std::size_t) k);
    if (settingsAt != nullptr)
        *settingsAt = at;
    if (version == 1)
        std::memcpy (p.snap.data(), d.data() + at + 0x3c, snapshotSize);
    at += body;
    const auto len = i32At (d.data() + at);
    at += 4;
    if (len <= 0 || (std::size_t) len > d.size() - at)
        return false;
    auto img = std::make_shared<Image>();
    if (! decodeBmp (d.data() + at, (std::size_t) len, *img) || img->height <= 2)
        return false;
    // FUN_007300d0: taller than "Max height": FL scales the image down (its image scaler, not decoded) — LYNX keeps
    // every row by nearest neighbour
    const auto maxH = p.P[14];
    if (img->height > maxH && maxH > 2)
    {
        auto small = std::make_shared<Image>();
        small->width = img->width, small->height = maxH;
        small->bgr.resize (3 * (std::size_t) img->width * (std::size_t) maxH);
        for (int y = 0; y < maxH; ++y)
        {
            const auto sy = (int) ((long long) y * img->height / maxH);
            std::memcpy (small->bgr.data() + 3 * (std::size_t) y * (std::size_t) img->width, img->pixel (0, sy), 3 * (std::size_t) img->width);
        }
        img = small;
    }
    if (img->height > maxRows)
        return false;
    p.image = img;
    out = std::move (p);
    return true;
}

// ------------------------------------------------------------------------------------------------------- engine

struct Row // the plugin's 0x30-byte oscillator record
{
    std::uint32_t phase = 0, offsetR = 0, inc = 0;
    std::int32_t freq = 0;
    float ampL = 0, ampR = 0, dL = 0, dR = 0, tL = 0, tR = 0;
    std::int32_t w = 256;
    bool active = false;
};

struct Voice
{
    std::int32_t cents = 0x7fffffff, column = -1, counter = 0, boundary = 0;
    std::uint32_t grainPhase = 0, grainInc = 0;
    std::array<Row, maxRows> rows {};
    bool ended = false;
    float fpitch = 0, fpitchBase = 0, fvol = 1, fpan = 0;
    HostSlide slide; // a slide note moving Final.Pitch (issue #14)
    int key = 60;
    std::uint64_t serial = 0;
    flvp::Proc proc;
};

class Engine
{
public:
    void init (const Params& p, double sampleRate, int ppq, double tempo)
    {
        P = p;
        fs = (int) std::lround (sampleRate);
        rate = (float) (c5Phase / (double) fs);
        S.retime (P.snap.data(), ppq, tempo);
        if (handsRate)
            S.setRate ((double) fs);
        update();
    }

    void timing (int ppq, double tempo) noexcept { S.retime (P.snap.data(), ppq, tempo); }

    // ProcessParam (FUN_0072efc0): "Length" → samples per column (FUN_005b6ef0 = MulDiv), the image's level scale
    void update() noexcept
    {
        const auto x = (float) ((double) P.P[1] / 4096.0);
        const auto e = std::exp ((double) x * std::log (33.0));
        const auto steps = roundEven ((double) ((float) (e - 1.0) * 256.0f));
        auto spc = (std::int64_t) ((steps + 64) * (std::int64_t) fs / 44100);
        spc = std::max<std::int64_t> (spc, 1);
        samplesPerColumn = (std::int32_t) spc;
        invColumn = (float) (1.0 / (double) samplesPerColumn);
        height = P.image != nullptr ? P.image->height : 0;
        width = P.image != nullptr ? P.image->width : 0;
        levelScale = (float) (1.0 / (std::sqrt ((double) height) * 255.0));
        for (int i = 0; i < count; ++i)
            voices[(std::size_t) order[(std::size_t) i]].cents = 0x7fffffff; // (parameter 0: every voice recomputes)
    }

    // Delphi's Random(range)
    std::uint32_t random (std::uint32_t range) noexcept
    {
        seed = seed * 0x8088405u + 1u;
        return (std::uint32_t) (((std::uint64_t) seed * range) >> 32);
    }

    // FUN_00730880: past the max polyphony, the first unreleased voice (else the last one) starts its end fade
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

    // FUN_0072f5b0 (TriggerVoice): random start phases (and right-channel offsets with "Widen")
    int trigger (int key, float finalPitch, float finalVol, float finalPan) noexcept
    {
        steal();
        if (count >= maxVoices)
            removeAt (0);
        int slot = 0;
        while (used[(std::size_t) slot])
            ++slot;
        used[(std::size_t) slot] = true;
        auto& v = voices[(std::size_t) slot];
        v.cents = 0x7fffffff, v.column = -1, v.counter = 0, v.boundary = 0, v.grainPhase = 0, v.grainInc = 0;
        v.ended = false;
        for (int r = 0; r < height; ++r)
        {
            auto& o = v.rows[(std::size_t) r];
            o = Row {};
            o.phase = random (0x1000000) << 8;
            o.offsetR = P.P[6] == 0 ? 0u : random (0x1000000) << 8;
        }
        v.key = key;
        v.serial = ++serials;
        v.fpitch = v.fpitchBase = finalPitch;
        v.slide = {};
        v.fvol = finalVol, v.fpan = finalPan;
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

    // FUN_0072f7e0: the rows' phase increments for the voice's pitch (cents from C5), by "Scale"
    void increments (Voice& v) noexcept
    {
        const auto H1 = height - 1;
        const auto mode = P.P[2];
        if (mode == 0)
        {
            for (int r = 0; r < height; ++r)
            {
                auto& o = v.rows[(std::size_t) r];
                auto c = (std::int64_t) (v.cents - ((r * 2 - H1) * P.P[0] * o.w) / (H1 * 0x200));
                c = std::clamp<std::int64_t> (c, -4800, 4800);
                o.inc = (std::uint32_t) roundEven ((double) rate * std::exp ((double) (std::int32_t) c * ln2Cents));
            }
            return;
        }
        if (mode != 1 && mode != 2)
            return;
        const auto quarter = fs / 4;
        const auto e = std::exp ((double) (mode == 1 ? v.cents : v.cents - 2400) * ln2Cents);
        auto base = roundEven (e * c5Hz);
        if ((std::int64_t) quarter < base)
            base = quarter;
        const auto hz = (std::int32_t) base;
        auto limit = quarter;
        if (hz != 0)
            limit = (quarter / hz) * hz;
        const auto span = (P.P[0] * 3) / 2;
        for (int r = 0; r < height; ++r)
        {
            auto& o = v.rows[(std::size_t) r];
            std::int32_t f;
            if (mode == 1)
                f = hz - ((r * 2 - H1) * span * o.w) / (H1 * 0x200);
            else
                f = (height - r) * hz * (9 - o.w / 32);
            o.freq = f;
            auto clamped = (std::int64_t) f;
            if (clamped < 0x29)
                clamped = 0x28;
            if ((std::int64_t) limit < clamped)
                clamped = limit;
            o.inc = (std::uint32_t) (((std::uint64_t) (std::uint32_t) clamped << 32) / (std::uint32_t) fs);
        }
    }

    // FUN_0072fb20: a new column — the pixels' levels as ramp targets, increments, the roll-off of "harmonic" rows
    void column (Voice& v) noexcept
    {
        std::int32_t rollOff = 0x7fffffff;
        if (P.P[2] == 2)
        {
            const auto x = (float) ((double) P.P[0] * 0.00010416666666666667);
            const auto e = std::exp ((double) x * std::log (11.0));
            rollOff = (std::int32_t) roundEven ((double) (float) (e - 1.0) * 1102.5);
        }
        auto rowsLit = height;
        if (width <= v.column)
        {
            if (P.P[5] < 1)
                rowsLit = -1;
            else
                v.column = 0;
            if (width == 0)
                rowsLit = -1;
        }
        for (int r = 0; r < height; ++r)
        {
            auto& o = v.rows[(std::size_t) r];
            auto flush = [] (float& a) {
                std::int32_t b;
                std::memcpy (&b, &a, 4);
                if ((b & 0x7fffffff) < 0x33800000)
                    a = 0.0f;
            };
            flush (o.ampL);
            flush (o.ampR);
            o.w = 256;
            o.tL = 0.0f, o.tR = 0.0f;
            if (r < rowsLit)
            {
                const auto* px = P.image->pixel (v.column, r);
                o.tL = (float) px[2] * levelScale;
                o.tR = (float) px[1] * levelScale;
                if (P.P[3] > 0)
                    o.w = 256 - (std::int32_t) px[0];
            }
        }
        increments (v);
        for (int r = 0; r < height; ++r)
        {
            auto& o = v.rows[(std::size_t) r];
            if (rollOff < o.freq)
            {
                auto g = (float) (1.0 - (double) (o.freq - rollOff) * 0.0020833333333333333);
                if (g < 0.0f)
                    g = 0.0f;
                o.tL = o.tL * g, o.tR = o.tR * g;
            }
            o.dL = (o.tL - o.ampL) * invColumn;
            o.dR = (o.tR - o.ampR) * invColumn;
            o.active = ! (flvp::bitsOf (o.ampL) == 0 && flvp::bitsOf (o.ampR) == 0 && flvp::bitsOf (o.tL) == 0 && flvp::bitsOf (o.tR) == 0);
        }
        v.grainInc = (std::uint32_t) (0x100000000ull / (std::uint32_t) samplesPerColumn);
    }

    // FUN_0072fe80 (the processor's per-voice callback): the oscillator bank into x (interleaved stereo); returns true
    // once the image has played (no loop)
    bool voiceRender (Voice& v, float* x, int n) noexcept
    {
        X87Extended guard;
        const auto c = (std::int32_t) roundEven ((double) v.fpitch);
        if (c != v.cents)
        {
            v.cents = c;
            increments (v);
        }
        const auto& sine = flvp::sineTable();
        const auto end = v.counter + n;
        auto* y = x;
        while (v.counter < end)
        {
            if (v.counter == v.boundary)
            {
                v.boundary += samplesPerColumn;
                ++v.column;
                column (v);
            }
            const auto from = v.counter;
            const auto to = std::min (v.boundary, end);
            v.counter = to;
            const auto m = to - from;
            for (int r = 0; r < height; ++r)
            {
                auto& o = v.rows[(std::size_t) r];
                if (r > 0 && ! o.active)
                {
                    o.phase += o.inc * (std::uint32_t) m;
                    continue;
                }
                // FUN_007309a0 (row 0: stores) / FUN_00730930 (adds): levels stepped in extended precision
                ld aL = o.ampL, aR = o.ampR;
                const ld sL = o.dL, sR = o.dR;
                auto ph = o.phase;
                for (int i = 0; i < m; ++i)
                {
                    const ld l = (ld) sine[ph >> 18] * aL;
                    const ld rr = (ld) sine[(std::uint32_t) (ph + o.offsetR) >> 18] * aR;
                    if (r == 0)
                        y[2 * i] = (float) l, y[2 * i + 1] = (float) rr;
                    else
                        y[2 * i] = (float) (l + (ld) y[2 * i]), y[2 * i + 1] = (float) (rr + (ld) y[2 * i + 1]);
                    aL = aL + sL;
                    aR = aR + sR;
                    ph += o.inc;
                }
                o.phase = ph;
                o.ampL = (float) aL, o.ampR = (float) aR;
            }
            if (P.P[4] > 0) // "Grainy" (FUN_00730a00)
            {
                auto ph = v.grainPhase;
                for (int i = 0; i < m; ++i)
                {
                    const auto t = sine[ph >> 18];
                    y[2 * i] = y[2 * i] * t;
                    y[2 * i + 1] = y[2 * i + 1] * t;
                    ph += v.grainInc;
                }
                v.grainPhase = ph;
            }
            y += 2 * m;
        }
        return width < v.column;
    }

    // Gen_Render (FUN_00730740): voices through the processor, mixed into l/r (overwrites)
    void render (float* l, float* r, int n) noexcept
    {
        std::fill (l, l + n, 0.0f);
        std::fill (r, r + n, 0.0f);
        if (count > 0)
            listed = true;
        const auto tick = tickPending;
        tickPending = false;
        if ((int) bufA.size() < 2 * n || height <= 2)
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
            const auto imageDone = voiceRender (v, bufA.data(), n);
            if (flvp::mixVoice (S, pr, bufA.data(), bufB.data(), n, v.fvol, v.fpan, l, r) || imageDone)
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
    }

    Params P;
    flvp::Settings S;
    bool handsRate = true; // Dispatcher 4 hands the rate to the processor library (as 3x Osc)
    int fs = 44100, maxPoly = 0, height = 0, width = 0;
    std::int32_t samplesPerColumn = 64;
    float rate = 0, invColumn = 0, levelScale = 0;
    std::uint32_t seed = 0;
    bool listed = false, tickPending = false;
    std::array<Voice, maxVoices> voices {};
    std::array<bool, maxVoices> used {};
    std::array<int, maxVoices> order {};
    int count = 0;
    std::uint64_t serials = 0;
    std::vector<float> bufA, bufB;
};

// ------------------------------------------------------------------------------------------------------- LYNX front-end
//
// As 3x Osc (REA_rodada47 §2.1): Final.Vol = channel volume × Init.Vol, Final.Pan = note pan + channel pan, Final.Pitch =
// 100·(key − 60) + channel pitch + note cents; FL's sequencer ticks step the processor's articulators.

struct Setup
{
    Params P;
    int ppq = 96;
    double tempo = 140.0;
};

class BeepMapInstrument final : public Instrument
{
public:
    BeepMapInstrument()
    {
        engine = std::make_unique<Engine>();
        engine->prepareBlock (blockSize);
        engine->init (Params {}, 44100.0, 96, 140.0);
    }

    ~BeepMapInstrument() override
    {
        delete incoming.exchange (nullptr);
        delete retired.exchange (nullptr);
    }

    void prepare (double sr) noexcept override
    {
        rate = sr;
        take();
        engine->init (current.P, sr, current.ppq, current.tempo);
        engine->reset();
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
        if (channel.pluginState == state && ppq == lastPpq && tempo == lastTempo)
            return;
        Params p;
        if (! parse (channel.pluginState, p))
            return;
        auto s = std::make_unique<Setup>();
        s->P = std::move (p);
        s->ppq = ppq, s->tempo = tempo;
        state = channel.pluginState, lastPpq = ppq, lastTempo = tempo;
        delete incoming.exchange (s.release());
    }

    void slideTo (int key, float cents, float samples) noexcept override
    {
        const auto to = (float) (100.0 * (std::clamp (key, 0, 127) - 60) + (double) pitchCents.load() + (double) cents);
        for (int i = 0; i < engine->count; ++i)
        {
            auto& v = engine->voices[(std::size_t) engine->order[(std::size_t) i]];
            if (! v.proc.released)
                v.slide.start (to - (v.fpitchBase - v.slide.cents), samples);
        }
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
        const auto slot = engine->trigger (k, finalPitch, (float) (channelVol * (double) initVol),
                                           (float) std::clamp ((double) nextPan + channelPan, -1.0, 1.0));
        volFactor[(std::size_t) slot] = initVol;
        notePan[(std::size_t) slot] = nextPan;
        nextPan = 0.0f;
        nextCents = 0.0f;
    }

    void noteOff (int key) noexcept override
    {
        for (int i = 0; i < engine->count; ++i)
        {
            const auto slot = engine->order[(std::size_t) i];
            const auto& v = engine->voices[(std::size_t) slot];
            if (v.key == key && ! v.proc.released)
                engine->release (slot);
        }
    }

    void allNotesOff() noexcept override
    {
        for (int i = 0; i < engine->count; ++i)
        {
            const auto slot = engine->order[(std::size_t) i];
            if (! engine->voices[(std::size_t) slot].proc.released)
                engine->release (slot);
        }
    }

    bool idle() const noexcept override { return engine->count == 0; }

    void setSongPosition (double ticks) noexcept override
    {
        const auto playing = lastTicks >= 0.0 && ticks != lastTicks;
        lastTicks = ticks;
        if (playing)
            tickPhase = ticks / (double) ppq * (double) current.ppq;
    }

    void render (float* left, float* right, int n, float gl, float gr) noexcept override
    {
        take();
        engine->maxPoly = maxPoly.load();
        const auto vol = std::sqrt (((double) gl * gl + (double) gr * gr) * 0.5);
        auto pan = vol > 0.0 ? std::atan2 ((double) gr, (double) gl) * (4.0 / 3.141592653589793) - 1.0 : 0.0;
        if (std::fabs (pan) < 1e-9)
            pan = 0.0;
        if (vol != channelVol || pan != channelPan)
        {
            channelVol = vol, channelPan = pan;
            for (int i = 0; i < engine->count; ++i)
            {
                const auto slot = engine->order[(std::size_t) i];
                auto& v = engine->voices[(std::size_t) slot];
                v.fvol = (float) (vol * (double) volFactor[(std::size_t) slot]);
                v.fpan = (float) std::clamp ((double) notePan[(std::size_t) slot] + pan, -1.0, 1.0);
            }
        }
        const auto perSample = current.tempo * (double) current.ppq / (60.0 * rate);
        for (int done = 0; done < n;)
        {
            if (std::fabs (tickPhase - std::round (tickPhase)) < 1e-9)
            {
                tickPhase = std::round (tickPhase);
                for (int i = 0; i < engine->count; ++i) // the host restores Final.Pitch before the tick
                {
                    auto& v = engine->voices[(std::size_t) engine->order[(std::size_t) i]];
                    v.fpitch = v.fpitchBase;
                }
                engine->newTick();
            }
            const auto next = std::floor (tickPhase) + 1.0;
            const auto toNext = std::max (1, (int) std::ceil ((next - tickPhase) / perSample - 1e-9));
            const auto len = std::min ({ blockSize, n - done, toNext });
            engine->render (bufL.data(), bufR.data(), len);
            for (int i = 0; i < engine->count; ++i) // a slide note moves the voices' Final.Pitch (issue #14)
            {
                auto& v = engine->voices[(std::size_t) engine->order[(std::size_t) i]];
                if (v.slide.moving())
                {
                    const auto was = v.slide.cents;
                    const auto d = v.slide.advance ((float) len) - was;
                    v.fpitchBase += d;
                    v.fpitch += d;
                }
            }
            for (int j = 0; j < len; ++j)
            {
                left[done + j] += bufL[(std::size_t) j];
                right[done + j] += bufR[(std::size_t) j];
            }
            tickPhase = len == toNext ? next : tickPhase + (double) len * perSample;
            done += len;
        }
    }

private:
    void take() noexcept
    {
        if (retired.load() != nullptr)
            return;
        if (auto* s = incoming.exchange (nullptr))
        {
            const bool newImage = s->P.image != current.P.image;
            current = *s;
            if (newImage)
                engine->reset(); // (FL: loading a bitmap kills every voice, FUN_0072f780)
            engine->P = current.P;
            engine->timing (current.ppq, current.tempo);
            engine->update();
            retired.store (s);
        }
    }

    static constexpr int blockSize = 512;
    std::unique_ptr<Engine> engine;
    Setup current;
    std::atomic<Setup*> incoming { nullptr }, retired { nullptr };
    std::atomic<float> pitchCents { 0.0f };
    std::atomic<int> maxPoly { 0 };
    std::vector<std::uint8_t> state;
    int lastPpq = -1;
    double lastTempo = -1.0;
    std::array<float, blockSize> bufL {}, bufR {};
    std::array<float, maxVoices> volFactor {}, notePan {};
    double rate = 44100.0, channelVol = 1.0, channelPan = 0.0, tickPhase = 0.0, lastTicks = -1.0;
    float nextPan = 0.0f, nextCents = 0.0f;
    static constexpr int ppq = 96;
};
} // namespace

bool beepMapStateUsable (const std::vector<std::uint8_t>& state)
{
    Params p;
    return parse (state, p);
}

std::optional<std::vector<int>> beepMapParams (const std::vector<std::uint8_t>& state)
{
    Params p;
    if (! parse (state, p))
        return std::nullopt;
    return std::vector<int> (p.P.begin(), p.P.end());
}

int beepMapParamOffset (const std::vector<std::uint8_t>& state, int index)
{
    Params p;
    std::size_t at = 0;
    if (index < 0 || index >= settingCount || ! parse (state, p, &at))
        return -1;
    return (int) (at + 4 * (std::size_t) index);
}

std::unique_ptr<Instrument> makeBeepMapInstrument() { return std::make_unique<BeepMapInstrument>(); }

namespace beepmap
{
std::vector<float> renderReference (const std::vector<std::uint8_t>& state, const std::vector<RefNote>& notes, double sampleRate,
                                    int length, int block, std::uint32_t seed, int maxPoly)
{
    Params P;
    if (! parse (state, P) || length <= 0)
        return {};
    auto engine = std::make_unique<Engine>();
    engine->init (P, sampleRate, 96, 140.0);
    engine->maxPoly = maxPoly;
    engine->seed = seed;
    block = std::max (1, block);
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
    std::size_t next = 0;
    for (int n0 = 0; n0 < length;)
    {
        auto ln = std::min (block, length - n0);
        for (const auto& e : order)
            if (n0 < e.first && e.first < n0 + ln)
                ln = e.first - n0;
        while (next < order.size() && order[next].first < n0)
            ++next;
        while (next < order.size() && order[next].first == n0)
        {
            const auto i = (std::size_t) (order[next].second / 2);
            const auto& nt = notes[i];
            if (order[next].second % 2 == 0)
            {
                slot[i] = engine->trigger (nt.key, (float) (100.0 * nt.key - 6000.0 + nt.pitchOffset), (float) nt.finalVol, (float) nt.finalPan);
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
} // namespace beepmap
} // namespace lynx
