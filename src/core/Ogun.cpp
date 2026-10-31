// FL Studio's Ogun: the inverse-FFT timbre engine of docs/analise/REA_rodada37.md, ported 1:1 from our own round-37
// reference model (r37/py/ogmodel.py, ogcurve.py, ogpatch.py, ogfx_chorus.py, ogfx_delay.py, ogfx_reverb.py,
// ogos.py, ogos_fast.py). Clean-room: written from the behavioural description; no Image-Line code. Every float32
// rounding of the model is kept (the model was checked against the DLL), so most expressions below spell out their
// float/double steps. Section references (§) are to REA_rodada37.

#include "core/Ogun.h"
#include "core/HostSlide.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>

#include "core/HarmlessFx.h"
#include "core/Inflate.h"
#include "core/PlateReverb.h"

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace lynx
{
namespace
{
constexpr double kPi = 3.141592653589793;
constexpr double LN2 = 0.6931471805599453;
constexpr double tiny24 = 5.960464477539063e-08; // 2^-24
inline double f32 (double x) noexcept { return (double) (float) x; }
inline long long rnd (double x) noexcept { return (long long) std::nearbyint (x); } // Delphi/Python round: half to even
inline int sgn (double x) noexcept { return x > 0.0 ? 1 : (x < 0.0 ? -1 : 0); }

// ---------------------------------------------------------------------------------------------------------------
// numpy's pairwise summation (float accumulators for float32 data), used where the model sums with np.sum
template <typename T, typename S>
T pairwiseSum (const S* a, std::size_t n) noexcept
{
    if (n < 8)
    {
        T res = (T) 0;
        for (std::size_t i = 0; i < n; ++i)
            res += (T) a[i];
        return res;
    }
    if (n <= 128)
    {
        T r[8];
        for (int j = 0; j < 8; ++j)
            r[j] = (T) a[j];
        std::size_t i = 8;
        for (; i < n - (n % 8); i += 8)
            for (int j = 0; j < 8; ++j)
                r[j] += (T) a[i + (std::size_t) j];
        T res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
        for (; i < n; ++i)
            res += (T) a[i];
        return res;
    }
    auto n2 = n / 2;
    n2 -= n2 % 8;
    return pairwiseSum<T> (a, n2) + pairwiseSum<T> (a + n2, n - n2);
}

// ---------------------------------------------------------------------------------------------------------------
// state (§1.3; our ogpatch.py)

struct Point
{
    double dx = 0.0, y = 0.0, T = 0.0;
    int mode = 0;
};

struct CurveData
{
    int type = 0, ver = 0;
    std::vector<Point> pts;
    int flags = 0, enabled = 0, decay = -1, loop = -1, sus = -1;
    int cfg = 0;
    double centre = 0.0;
};

constexpr int cfgEnv = 0x2d, cfgLfo = 0x27, cfgMap = 0x20;
constexpr int artMode[13] { 0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 0, 1, 1 };
constexpr double artCentre[13] { 0.0, 0.5, 0.5, 0.5, 0.0, 0.0, 0.0, 0.0, 0.0, 0.5, 0.0, 0.5, 0.5 };

struct ArtData
{
    double base = 0.0, envamt = 0.0, lfoamt = 0.0;
    CurveData env, lfo, key, vel, X, Y;
    std::array<int, 4> adsr {}, lfoKnobs {};
    int lfoExtra = 0;
    int mode = 0;
};

struct StateData
{
    int version = 0;
    std::vector<std::uint8_t> body;
    std::array<std::int32_t, 64> P {};
    std::array<std::uint8_t, 3> b358 {};
    std::array<int, 3> eqtype {};
    std::array<ArtData, 13> arts;
    std::array<CurveData, 4> maps; // 13 forced, 14 impulse, 15 decay, 16 release
    bool autogun = false;          // Autogun's variant of the state (REA_rodada57 §1)
    std::uint8_t byte (int plugOffset) const noexcept { return body[(std::size_t) (plugOffset - 0x12c)]; }
};

struct Reader
{
    const std::uint8_t* b;
    std::size_t n, i = 0;
    bool ok = true;

    bool need (std::size_t k) noexcept
    {
        if (! ok || i + k > n)
            ok = false;
        return ok;
    }
    std::int32_t i32() noexcept
    {
        if (! need (4))
            return 0;
        std::int32_t v;
        std::memcpy (&v, b + i, 4);
        i += 4;
        return v;
    }
    float f32v() noexcept
    {
        if (! need (4))
            return 0.0f;
        float v;
        std::memcpy (&v, b + i, 4);
        i += 4;
        return v;
    }
    double f64v() noexcept
    {
        if (! need (8))
            return 0.0;
        double v;
        std::memcpy (&v, b + i, 8);
        i += 8;
        return v;
    }
    void skip (std::size_t k) noexcept
    {
        if (need (k))
            i += k;
    }
    std::uint8_t u8() noexcept
    {
        if (! need (1))
            return 0;
        return b[i++];
    }
};

bool readCurve (Reader& r, CurveData& c, int cfg, double centre)
{
    c.cfg = cfg;
    c.centre = centre;
    c.type = r.i32();
    c.ver = r.i32();
    const auto n = r.i32();
    if (! r.ok || n < 0 || n > 100000)
        return false;
    c.pts.resize ((std::size_t) n);
    for (auto& p : c.pts)
    {
        if (c.ver >= 2)
        {
            p.dx = r.f64v();
            p.y = (double) (float) r.f64v();
        }
        else
        {
            p.dx = (double) r.f32v();
            p.y = (double) r.f32v();
        }
        p.T = (double) r.f32v();
        if (c.ver >= 1)
        {
            p.mode = r.u8();
            r.skip (3); // arpeggio byte (cleared) + 2
        }
        else
            p.mode = 0;
    }
    c.flags = r.i32();
    c.enabled = r.i32();
    c.decay = r.i32();
    c.loop = r.i32();
    c.sus = r.i32();
    if (! (cfg & 4))
        c.decay = c.loop = c.sus = -1;
    if (! (cfg & 8))
        c.decay = -1;
    return r.ok;
}

std::optional<StateData> parseState (const std::vector<std::uint8_t>& blob, bool autogun = false)
{
    if (blob.size() < 8)
        return std::nullopt;
    std::uint32_t vw = 0;
    std::memcpy (&vw, blob.data(), 4);
    StateData S;
    S.autogun = autogun;
    S.version = (int) (vw > 0xffff ? vw >> 16 : vw);
    if (S.version < 14)
        return std::nullopt;
    auto body = inflateZlib (blob.data() + 4, blob.size() - 4, false, 16u << 20);
    if (! body || body->size() < 0x100 + 3 + 12)
        return std::nullopt;
    S.body = std::move (*body);
    std::memcpy (S.P.data(), S.body.data(), 256);
    Reader r { S.body.data(), S.body.size(), 0x100 };
    for (auto& b : S.b358)
        b = r.u8();
    for (auto& t : S.eqtype)
        t = r.i32();
    if (S.version < 18)
    {
        const auto n = r.i32();
        if (n < 0)
            return std::nullopt;
        r.skip ((std::size_t) n);
    }
    else
    {
        // comment: UTF-16 characters, count as one byte or 0xff + i32 (REA_rodada57 §1.2)
        std::size_t v = r.u8();
        if (v == 0xff)
        {
            const auto n = r.i32();
            if (n < 0)
                return std::nullopt;
            v = (std::size_t) n;
        }
        r.skip (2 * v);
    }
    if (autogun && S.version >= 19)
        r.skip (4); // Autogun: the random-pad seed (user interface only)
    for (int k = 0; k < 13; ++k)
    {
        auto& a = S.arts[(std::size_t) k];
        const auto c = artCentre[k];
        r.i32(); // version
        a.base = r.f32v();
        a.envamt = r.f32v();
        a.lfoamt = r.f32v();
        if (! readCurve (r, a.env, cfgEnv, c))
            return std::nullopt;
        for (auto& v : a.adsr)
            v = r.i32();
        if (! readCurve (r, a.lfo, cfgLfo, 0.5))
            return std::nullopt;
        for (auto& v : a.lfoKnobs)
            v = r.i32();
        a.lfoExtra = a.lfo.ver >= 3 ? r.i32() : 0;
        if (! readCurve (r, a.key, cfgMap, c) || ! readCurve (r, a.vel, cfgMap, c) || ! readCurve (r, a.X, cfgMap, c)
            || ! readCurve (r, a.Y, cfgMap, c))
            return std::nullopt;
        a.mode = artMode[k];
    }
    for (auto& m : S.maps)
        if (! readCurve (r, m, cfgMap, 0.0))
            return std::nullopt;
    if (! r.ok)
        return std::nullopt;
    return S;
}

// ---------------------------------------------------------------------------------------------------------------
// point curves (§1.4; our ogcurve.py)

struct Curve
{
    double centre = 0.0;
    int flags = 0, enabled = 0, decay = -1, loop = -1, sus = -1, cfg = 0, rel = -1;
    std::vector<double> dx, y, T, w;
    std::vector<int> mode, sign;
    std::vector<std::array<double, 3>> k;
    double lo = 0.0, hi = 1.0;

    Curve() = default;
    Curve (const CurveData& cd, double length = -1.0) { build (cd, length); }

    void build (const CurveData& cd, double length)
    {
        centre = cd.centre;
        flags = cd.flags, enabled = cd.enabled;
        decay = cd.decay, loop = cd.loop, sus = cd.sus;
        cfg = cd.cfg;
        auto pts = cd.pts;
        if (length >= 0.0)
        {
            if (pts.empty())
                pts.push_back (Point {});
            else if (pts[0].dx > 0.0)
            {
                Point p0;
                p0.y = pts[0].y;
                pts.insert (pts.begin(), p0);
                for (auto* a : { &decay, &loop, &sus })
                    if (*a >= 0)
                        ++*a;
            }
            double tot = 0.0;
            for (const auto& p : pts)
                tot += p.dx;
            if (tot > 0.0)
            {
                for (auto& p : pts)
                    p.dx = p.dx * (length / tot);
            }
            else
                pts.back().dx = pts.size() > 1 ? length : 0.0;
        }
        if (pts.empty())
            pts.push_back (Point {}); // (an empty envelope; never in FL's data)
        const auto n = pts.size();
        dx.resize (n), y.resize (n), T.resize (n), mode.resize (n);
        for (std::size_t i = 0; i < n; ++i)
        {
            dx[i] = pts[i].dx;
            y[i] = f32 (pts[i].y);
            T[i] = f32 (pts[i].T);
            mode[i] = pts[i].mode;
        }
        coeffs();
    }

    void coeffs()
    {
        const int n = (int) dx.size();
        for (auto& d : dx)
            if (d < 0.0)
                d = 0.0;
        sign.assign ((std::size_t) n, 0);
        k.assign ((std::size_t) n, { 0.0, 0.0, 0.0 });
        const auto LN1M = std::log (1000001.0);
        for (int i = 1; i < n; ++i)
        {
            const auto Ti = T[(std::size_t) i];
            const auto m = mode[(std::size_t) i];
            int s = Ti == 0.0 ? 0 : (Ti < 0.0 ? -1 : 1);
            long long N = 0;
            if (m >= 3 && m <= 6)
            {
                const auto v = f32 (1.0 - std::fabs (Ti) * 0.6);
                N = rnd (f32 (std::exp (v * LN1M) - 1.0) * 0.001);
                if (m == 3 && Ti >= 0.0)
                    N += 1;
            }
            double k14 = 0.0, k18 = 0.0, k1c = 0.0;
            if (m == 6)
            {
                k14 = f32 ((double) N + 0.5);
                k18 = f32 (2.0 * kPi);
                if (N == 0 && s > 0)
                    s = 2;
            }
            else if (m < 2)
            {
                auto q = f32 (std::exp (f32 (std::fabs (Ti)) * LN1M) - 1.0);
                q = Ti >= 0.0 ? q : -q;
                k14 = q;
                if (q != 0.0)
                {
                    k18 = f32 (1.0 / std::fabs (q));
                    k1c = f32 (std::log (std::fabs (q) + 1.0));
                }
            }
            else if (m == 3)
            {
                const double sg = Ti < 0.0 ? -1.0 : 1.0;
                k1c = f32 (sg * 0.5);
                k14 = f32 ((double) (N + 1));
                k18 = f32 (1.0 / f32 (k14 - sg));
            }
            else if (m == 4)
            {
                k14 = f32 ((double) (N + 1));
                k18 = f32 (1.0 / k14);
            }
            else if (m == 5)
            {
                const double sg = Ti < 0.0 ? -1.0 : 1.0;
                k1c = f32 (sg * 0.5);
                k14 = f32 ((double) ((N + 1) * 2));
                k18 = f32 (2.0 * kPi);
            }
            else if (m == 7 || m == 8)
                k14 = Ti;
            else if (m == 9)
                k14 = f32 (std::fabs (Ti));
            else if (m == 10)
            {
                const auto X6 = std::max (dx[(std::size_t) i], tiny24);
                const auto yi = y[(std::size_t) i];
                const int j = i - 1;
                const auto y9 = y[(std::size_t) j];
                auto X8 = y9;
                double X7 = 1.0;
                if (mode[(std::size_t) j] == 10)
                {
                    X7 = dx[(std::size_t) j] / X6;
                    if (j - 1 >= 0)
                        X8 = y[(std::size_t) (j - 1)];
                }
                const int kk = i + 1;
                double in1, d19;
                if (kk > n - 1 || mode[(std::size_t) kk] != 10)
                    in1 = yi, d19 = 1.0;
                else
                    d19 = dx[(std::size_t) kk] / X6, in1 = y[(std::size_t) kk];
                const auto d = yi - y9;
                if (std::fabs (d) >= tiny24)
                {
                    X8 = (X8 - y9) / d;
                    const auto d24 = std::max (X7, tiny24);
                    d19 = std::max (d19, tiny24);
                    const auto d26 = ((in1 - y9) / d - 1.0) / d19;
                    const auto d21 = (-X8) / d24;
                    double a = 0.0;
                    if (f32 (f32 (d26) * 1.0) > 0.0)
                    {
                        const auto f25 = f32 (f32 (d19) * 2.0 + 1.0), f18 = f32 (f32 (d19) + 2.0);
                        a = f32 (f32 (f32 (f32 (d26) * 1.0) * f32 (f25 + f18)) / f32 (f32 (f25 * f32 (d26)) + f32 (f18 * 1.0)));
                    }
                    double b = 0.0;
                    if (f32 (1.0 * f32 (d21)) > 0.0)
                    {
                        const auto f18 = f32 (2.0 + f32 (d24)), f25 = f32 (1.0 + f32 (d24) * 2.0);
                        b = f32 (f32 (f32 (1.0 * f32 (d21)) * f32 (f18 + f25)) / f32 (f32 (f18 * 1.0) + f32 (f25 * f32 (d21))));
                    }
                    k14 = a, k18 = b, k1c = f32 (1.0 - std::fabs (Ti));
                }
                else
                    s = 2;
            }
            else if (m == 11 || m == 12)
            {
                const auto d = std::max (std::fabs (Ti) * 0.9, 1.52587890625e-05);
                const auto u = 1.0 - d;
                k14 = f32 (1.0 - u * u * u);
                k18 = f32 ((1.0 - k14) / k14);
                k1c = k18;
                if (s > 0)
                {
                    k18 = -k18;
                    k1c = f32 (k18 - 1.0);
                }
                if (m == 12)
                {
                    k18 = f32 (k18 * 0.5);
                    k1c = f32 (k1c * 0.5);
                }
            }
            if (m >= 0 && m < 16 && ((0x1b83 >> m) & 1) && Ti == 0.0)
                s = 2;
            sign[(std::size_t) i] = s;
            k[(std::size_t) i] = { k14, k18, k1c };
        }
        // per-point weights of the sustain-level offset
        const int i15 = std::max (decay, 0);
        const int l80 = sus >= 0 ? sus : n - 1;
        const int l7c = loop >= 0 ? loop : l80;
        double L98 = 0.0, L90 = 0.0, L88 = 0.0;
        for (int i = 0; i < n; ++i)
        {
            if (i <= i15)
                L98 += dx[(std::size_t) i];
            else if (i <= l7c)
                L90 += dx[(std::size_t) i];
            if (i > l80)
                L88 += dx[(std::size_t) i];
        }
        w.assign ((std::size_t) n, 0.0);
        double d = L98;
        for (int i = 0; i < n; ++i)
        {
            const auto di = dx[(std::size_t) i];
            if (i > i15)
            {
                w[(std::size_t) i] = 1.0;
                if (i > l80)
                {
                    d -= di;
                    w[(std::size_t) i] = ! (cfg & 0x80) ? f32 (d / std::max (L88, 1.52587890625e-05)) : 1.0;
                }
                else
                {
                    if (i <= l7c)
                        w[(std::size_t) i] = f32 ((d + di) / std::max (L90, 1.52587890625e-05));
                    d += di;
                    if (i == l80)
                        d = L88;
                }
            }
            else if (i == i15)
                d = 0.0;
            else
            {
                d -= di;
                w[(std::size_t) i] = f32 (d / std::max (L98, 1.52587890625e-05));
            }
        }
        rel = sus >= 0 ? sus : (loop >= 0 ? n - 1 : -1);
    }

    int size() const noexcept { return (int) dx.size(); }

    double val (int i, double mod = 0.0, bool clamp = true) const noexcept
    {
        if (i < 0 || i >= size())
            return 0.0;
        auto v = y[(std::size_t) i];
        if (mod != 0.0)
        {
            v = v + f32 (w[(std::size_t) i] * mod);
            if (clamp)
                v = std::min (std::max (v, lo), hi);
        }
        return v - centre;
    }

    static double half01 (double x, double k14, double k18, double k1c) noexcept
    {
        if (k14 == 0.0)
            return x;
        if (k14 > 0.0)
            return 1.0 - (std::exp ((1.0 - x) * k1c) - 1.0) * k18;
        return (std::exp (x * k1c) - 1.0) * k18;
    }

    double shape (int i, double t) const noexcept
    {
        const auto m = mode[(std::size_t) i];
        const auto s = sign[(std::size_t) i];
        const auto [k14, k18, k1c] = k[(std::size_t) i];
        if (s == 2)
            return t;
        switch (m)
        {
            case 7:
            {
                const auto u = s > 0 ? 1.0 - t : t;
                const auto u3 = u * u * u;
                return t - (u3 * u3 - u) * k14;
            }
            case 0: return half01 (t, k14, k18, k1c);
            case 1:
                if (t < 0.5)
                    return half01 (t * 2.0, k14, k18, k1c) * 0.5;
                return 1.0 - half01 ((1.0 - t) * 2.0, k14, k18, k1c) * 0.5;
            case 2: return 0.0;
            case 3: return (double) ((float) rnd (t * k14 - k1c) * (float) k18);
            case 5: return (double) (rnd (t * k14 - k1c) & 1);
            case 6:
            {
                if (s < 0)
                    return (std::sin ((t * k14 + 0.75) * k18) + 1.0) * 0.5;
                const auto v = t * k14 - 0.5;
                const auto r = (double) rnd (v - 0.5);
                return std::fabs ((v - r) * 2.0 - 1.0);
            }
            case 4:
            {
                const auto v = t * k14 - 0.5;
                const auto r = (double) rnd (v - 0.5);
                auto d = (v - r) * 2.0 - 1.0;
                if (s < 0)
                {
                    const auto a = 1.0 - std::fabs (d);
                    d = (1.0 - a * a * a) * (d >= 0.0 ? 1.0 : -1.0);
                }
                else
                    d = d * d * d;
                return (r + d * 0.5 + 1.0) * k18;
            }
            case 8:
            {
                if (t < 0.5)
                {
                    const auto x = t * 2.0, u = s > 0 ? 1.0 - x : x, u3 = u * u * u;
                    return (x - (u3 * u3 - u) * k14) * 0.5;
                }
                const auto x = (1.0 - t) * 2.0, u = s > 0 ? 1.0 - x : x, u3 = u * u * u;
                return 1.0 - (x - (u3 * u3 - u) * k14) * 0.5;
            }
            case 9:
            {
                const auto g = s < 1 ? (1.0 - std::cos (t * 1.5707963705062866)) : std::sin (t * 1.5707963705062866);
                return t - (t - g) * k14;
            }
            case 10:
            {
                const auto t2 = t * t, t1 = t - 1.0;
                return t + (((t2 * 3.0 - t2 * 2.0 * t) + t2 * t1 * k14 + t * t1 * t1 * k18) - t) * k1c;
            }
            case 11:
                if (s < 1)
                    return k18 / (1.0 - k14 * t) - k1c;
                return k18 / (1.0 - k14 * (1.0 - t)) - k1c;
            case 12:
            {
                if (t < 0.5)
                {
                    if (s < 1)
                        return k18 / (1.0 - k14 * t * 2.0) - k1c;
                    return k18 / (1.0 - k14 * (1.0 - t * 2.0)) - k1c;
                }
                const auto d = (1.0 - t) * 2.0;
                if (s < 1)
                    return (1.0 - k18 / (1.0 - k14 * d)) + k1c;
                return (1.0 - k18 / (1.0 - k14 * (1.0 - d))) + k1c;
            }
            default: return 0.0;
        }
    }

    // sequential stepping state used by the free evaluation, table rendering and per-partial sampling
    struct Seq
    {
        int idx = 0;
        double r = 0.0, t = 0.0, ys = 0.0, dl = 0.0;
    };

    Seq seqStart (double rate) const noexcept
    {
        Seq q;
        if (dx[0] == 0.0)
            q.r = 1.0, q.t = 1.0;
        else
            q.r = rate / dx[0], q.t = 0.0;
        q.ys = val (0);
        return q;
    }

    void seqStep (Seq& q, double x) const noexcept
    {
        const int n = size();
        q.t = q.t + x * q.r;
        while (q.t >= 1.0)
        {
            const auto old = q.idx;
            q.t = q.r == 0.0 ? x : (q.t - 1.0) / q.r;
            q.ys = val (q.idx);
            if (q.idx < n - 1)
                ++q.idx;
            q.dl = val (q.idx) - q.ys;
            if (old == q.idx)
                q.t = 0.0, q.r = 0.0;
            else
            {
                const auto d = dx[(std::size_t) q.idx];
                if (d <= 0.0)
                    q.r = 1.0, q.t = q.t + 1.0;
                else
                    q.r = 1.0 / d, q.t = q.t * q.r;
            }
        }
    }

    double valueAt (double x, double rate = 1.0) const noexcept
    {
        auto q = seqStart (rate);
        seqStep (q, x);
        if (q.idx < 0 || q.idx >= size())
            return 0.0;
        return q.ys + shape (q.idx, q.t) * q.dl;
    }

    std::vector<double> render (int N, double step) const
    {
        std::vector<double> out ((std::size_t) N);
        auto q = seqStart (1.0);
        out[0] = q.ys;
        for (int j = 1; j < N; ++j)
        {
            seqStep (q, step);
            out[(std::size_t) j] = q.ys + shape (q.idx, q.t) * q.dl;
        }
        return out;
    }

    // per-partial maps: stepping to increasing positions (ogmodel._seq_eval)
    void seqEval (const double* xs, int n, double* out) const noexcept
    {
        auto q = seqStart (1.0);
        double prev = 0.0;
        for (int j = 0; j < n; ++j)
        {
            const auto stp = xs[j] - prev;
            prev = xs[j];
            seqStep (q, stp);
            out[j] = q.ys + (q.dl != 0.0 ? shape (q.idx, q.t) * q.dl : 0.0);
        }
    }

    bool isConstant() const noexcept
    {
        for (const auto v : y)
            if (v != y[0])
                return false;
        return true;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// envelope / LFO time engine (§1.5)

struct EnvRun
{
    const Curve* c = nullptr;
    double rate0 = 0.0;
    double kn[4] {};
    int stage = 9, sub = 9, idx = 0;
    double ys = 0.0, dl = 0.0, out = 0.0, t = 0.0, r = 0.0, mod = 0.0, relv = 0.0;
    bool loopzero = false;

    void init (const Curve& curve, double rateTime, double rateTempo, const std::array<int, 4>& knobs) noexcept
    {
        c = &curve;
        rate0 = (curve.flags & 1) ? rateTempo : rateTime;
        for (int i = 0; i < 4; ++i)
            kn[i] = f32 (knobs[(std::size_t) i] / 128.0);
        if (! curve.enabled)
        {
            stage = 9, sub = 9, ys = dl = out = 0.0, idx = 0, t = r = mod = relv = 0.0;
            loopzero = false;
            return;
        }
        idx = 0, mod = 0.0;
        stage = curve.decay < 0 ? 3 : 1;
        if (curve.dx[0] == 0.0)
            r = 1.0, t = 1.0;
        else
            r = rate0 / curve.dx[0], t = 0.0;
        sub = stage;
        dl = 0.0;
        relv = curve.sus >= 0 ? kn[2] : 0.0;
        loopzero = false;
        if (curve.rel >= 0 && curve.loop >= 0)
        {
            double a = 0.0, b = 0.0;
            for (int i = 0; i <= curve.rel; ++i)
                a += curve.dx[(std::size_t) i];
            for (int i = 0; i <= curve.loop; ++i)
                b += curve.dx[(std::size_t) i];
            loopzero = ! (a - b > 0.0);
        }
        ys = curve.val (0, f32 (mod));
        out = ys;
    }

    void setMod (double v) noexcept
    {
        mod = stage < 2 ? f32 (v) : relv;
        out = c->val (0, f32 (mod));
    }

    double step (double x) noexcept
    {
        const auto& C = *c;
        const int n = C.size();
        if (stage == 9 && ! C.enabled)
            return out;
        if (! (0 <= idx && idx <= n - 1))
            return out;
        t += x * r;
        while (t >= 1.0)
        {
            t = r == 0.0 ? x : (t - 1.0) / r;
            ys = C.val (idx, f32 (mod));
            bool end = false;
            if (idx < C.rel || C.rel < 0 || stage > 5)
            {
                if (C.decay >= 0 && C.decay <= idx && sub < 3)
                {
                    stage = std::max (stage, 3);
                    sub = 3;
                    mod = relv;
                }
                if (idx < n - 1)
                    ++idx;
                else
                {
                    stage = 9;
                    end = true;
                }
            }
            else if (C.loop < 0)
                end = true;
            else
            {
                sub = 4;
                idx = C.loop + 1;
                end = loopzero;
            }
            dl = C.val (idx, f32 (mod)) - ys;
            if (! end)
            {
                const int i2 = (stage >= 6 && C.sus >= 0) ? 1 : 0;
                const auto sc = kn[(stage > 2 ? 1 : 0) + 2 * i2];
                const auto d = C.dx[(std::size_t) idx] * sc;
                if (d <= 0.0)
                    r = 1.0, t += 1.0;
                else
                {
                    r = rate0 / d;
                    t *= r;
                }
            }
            else
                t = 0.0, r = 0.0;
        }
        if (dl == 0.0)
            out = ys;
        else
            out = ys + C.shape (idx, t) * dl;
        return out;
    }

    void release() noexcept
    {
        const auto& C = *c;
        if (! C.enabled || stage >= 6)
            return;
        if (C.sus >= 0)
        {
            idx = C.sus;
            const auto v = C.val (idx, f32 (mod), false);
            mod = mod + (out - v);
            t = 1.0;
            sub = 6;
        }
        stage = 6;
        if (r == 0.0)
            t = 1.0;
    }
};

struct LfoRun : EnvRun
{
    double wT = 0.0, w0 = 0.0, w1 = 0.0, w3 = 0.0, w4 = 0.0;
    std::uint32_t w5 = 0, w6 = 0, w7 = 0, ph = 0;
    double inc = 0.0, lout = 0.0;
    bool flag = false;

    void initLfo (const Curve& curve, double rateTime, double rateTempo, const std::array<int, 4>& knobs, int extra) noexcept
    {
        EnvRun::init (curve, rateTime, rateTempo, { 128, 128, 0, 128 });
        kn[0] = 1.0, kn[1] = 1.0, kn[2] = 0.0, kn[3] = 1.0;
        relv = 0.0;
        const auto speed = knobs[0], tens = knobs[1], skew = knobs[2], pw = knobs[3];
        // (global LFOs, header flag 2, run as per-voice ones: not modelled, REA_rodada37 open questions)
        const auto av = std::abs (tens);
        auto T = f32 (f32 (std::exp (f32 (av * f32 (1.0 / 128.0)) * std::log (21.0)) - 1.0) * 0.05);
        T = f32 (std::exp (T * std::log (9.999999843067494e+17)) - 1.0);
        T = T * (tens == 0 ? 0.0 : (tens > 0 ? 1.0 : -1.0));
        wT = f32 (T);
        if (wT != 0.0)
        {
            w3 = f32 (1.0 / std::fabs (wT));
            w4 = f32 (std::log (std::fabs (wT) + 1.0));
        }
        const auto s = f32 (skew * 0.00390625), p = f32 (pw * f32 (1.0 / 128.0));
        flag = (curve.flags & 4) != 0;
        if (p > 0.0)
        {
            const float a = (float) (1.0 - p);
            const float b = a * 4294967296.0f;
            const float cc = b - 1.0f;
            w7 = (std::uint32_t) (std::uint64_t) rnd ((double) cc);
        }
        else
            w7 = 0xffffffffu;
        auto d = (f32 ((1.0 - p) / 2.0) - s * (1.0 - std::fabs (p))) * 4294967296.0;
        if (d > (double) w7 - 1.0)
            d = (double) w7 - 1.0;
        else if (d < 1.0)
            d = 1.0;
        w5 = (std::uint32_t) (std::uint64_t) rnd (d);
        if (p < 0.0)
        {
            const auto v = (std::uint32_t) (std::int64_t) rnd (d + f32 (p * 4294967296.0));
            w6 = v != 0 ? v : 1u;
        }
        else
            w6 = w5;
        w0 = f32 (2.0 / (double) w6);
        const auto span = (std::uint32_t) (w7 - w5);
        w1 = span != 0 ? f32 (2.0 / (double) span) : 0.0;
        auto in = (std::exp (f32 (speed * 1.52587890625e-05) * std::log (10001.0)) - 1.0) * 6871947.6736 * rate0;
        in = std::min (in, 805306368.0);
        inc = (double) rnd (in);
        if (curve.flags & 8)
            inc = 0.0;
        ph = (std::uint32_t) ((std::int64_t) w5 + (std::int64_t) ((w7 - w5) >> 1) + 1 + (std::int64_t) extra);
        lout = 0.0;
    }

    double shapeT (double a) const noexcept
    {
        if (wT == 0.0)
            return a;
        if (wT > 0.0)
            return 1.0 - (std::exp ((1.0 - a) * w4) - 1.0) * w3;
        return (std::exp (a * w4) - 1.0) * w3;
    }

    double wave (std::uint32_t p) const noexcept
    {
        double v;
        if (p > w7)
            return 1.0;
        if (p < w6)
            v = f32 (1.0 - (double) (float) p * w0);
        else if (p < w5)
            return -1.0;
        else
            v = f32 ((double) (float) (std::uint32_t) (p - w5) * w1 - 1.0);
        if (! flag)
        {
            const auto r = shapeT (std::fabs (v));
            return f32 (std::copysign (f32 (r), v));
        }
        const auto u = f32 ((v + 1.0) * 0.5);
        return f32 (shapeT (u) * 2.0 - 1.0);
    }

    double lstep (double x) noexcept
    {
        auto xp = x;
        if (idx == 0 && stage < 9 && t < 1.0)
        {
            const auto d = r != 0.0 ? x + (t - 1.0) / r : x;
            xp = d > 0.0 ? d : 0.0;
        }
        step (x);
        if (idx != 0 || stage > 8 || t >= 1.0)
        {
            ph = (std::uint32_t) ((std::uint64_t) ph + (std::uint64_t) (std::int64_t) rnd (xp * inc));
            lout = out * wave (ph);
        }
        return lout;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// IPP-style uniform generator and the seed mix (round 24 §1.3/§1.4)

void rand32f (float* out, int n, float lo, float hi, std::uint32_t seed) noexcept
{
    constexpr std::uint32_t A = 69069u, C = 0x3c6ef373u;
    const auto sc = (double) (hi - lo) * 2.3283064365386963e-10;
    const auto of = (double) (lo + hi) * 0.5;
    std::uint32_t e0 = (seed * A + C) * A + C, e1 = seed * A + C, e2 = seed;
    std::uint32_t L = 0x436cbae9u;
    std::int32_t borrow = -1;
    for (int k = 0; k < n; ++k)
    {
        auto v = (std::int32_t) (e1 - e0 + (std::uint32_t) borrow);
        borrow = v < 0 ? -1 : 0;
        if (v < 0)
            v = (std::int32_t) ((std::uint32_t) v - 18u);
        e0 = e1, e1 = e2, e2 = (std::uint32_t) v;
        L = L * A + C;
        out[k] = (float) ((double) (std::int32_t) ((std::uint32_t) v + L) * sc + of);
    }
}

long long mix32 (long long a0, std::uint32_t k1) noexcept
{
    const auto a = (std::uint32_t) a0;
    std::uint32_t u6 = ((0u - a - k1)) ^ (a << 8);
    std::uint32_t u7 = (0x1eefu - a - u6) ^ (u6 >> 13);
    std::uint32_t u5 = (a - u6 - u7) ^ (u7 >> 12);
    u6 = (u6 - u7 - u5) ^ (u5 << 16);
    u7 = (u7 - u5 - u6) ^ (u6 >> 5);
    u5 = (u5 - u6 - u7) ^ (u7 >> 3);
    u6 = (u6 - u7 - u5) ^ (u5 << 10);
    u6 = (u7 - u5 - u6) ^ (u6 >> 15);
    return std::llabs ((long long) (std::int32_t) u6);
}

// ---------------------------------------------------------------------------------------------------------------
// inverse real FFT (unscaled, double): x[n] = Σ_k X_k e^{2πikn/N} for a Hermitian spectrum with bins 1…M

// sin and cos of |x| ≲ 4 in double (quadrant reduction + Taylor polynomials on |r| ≤ π/4; within a few ulps of libm)
inline void sinCos (double x, double& s, double& c) noexcept
{
    const auto q = std::nearbyint (x * 0.6366197723675814);
    const auto r = (x - q * 1.5707963267341256) - q * 6.077100506506192e-11;
    const auto r2 = r * r;
    const auto sr = r + r * r2 * (-1.0 / 6 + r2 * (1.0 / 120 + r2 * (-1.0 / 5040 + r2 * (1.0 / 362880 + r2 * (-1.0 / 39916800
                    + r2 * (1.0 / 6227020800.0 + r2 * (-1.0 / 1307674368000.0)))))));
    const auto cr = 1.0 + r2 * (-0.5 + r2 * (1.0 / 24 + r2 * (-1.0 / 720 + r2 * (1.0 / 40320 + r2 * (-1.0 / 3628800
                    + r2 * (1.0 / 479001600.0 + r2 * (-1.0 / 87178291200.0 + r2 * (1.0 / 20922789888000.0))))))));
    switch (((int) q) & 3)
    {
        case 0: s = sr, c = cr; break;
        case 1: s = cr, c = -sr; break;
        case 2: s = -sr, c = -cr; break;
        default: s = -cr, c = sr; break;
    }
}

#if defined(__x86_64__) || defined(_M_X64)
#define OGUN_X86 1
#define OGUN_INLINE inline __attribute__ ((always_inline))
bool ogunCpuHasAvx() noexcept
{
    unsigned a = 1, b = 0, c = 0, d = 0;
    __asm__ __volatile__ ("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d) : "c"(0));
    if ((c & (1u << 27)) == 0 || (c & (1u << 28)) == 0) // OSXSAVE, AVX
        return false;
    unsigned lo = 0, hi = 0;
    __asm__ __volatile__ ("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (lo & 6u) == 6u;
}
#else
#define OGUN_INLINE inline
#endif

// The 32-point windowed-sinc reader of quality 4 (ogos_fast.sinc_read_fast) for one chunk, both tables at once:
// tap ix + k (k = −31…32) weighted by ½·sin(b(f − k))/(π(f − k))·(0.36 + w + 0.64w²), w = cos(a(f − k)); the sines of
// the shifted arguments come from sin/cos(bf), sin/cos(af) and the per-k tables (angle addition).
struct SincTables
{
    double b = -1.0;
    double cb[64], sb[64], cA[64], sA[64], kk[64];
};

OGUN_INLINE void sincChunkBody (const SincTables& T, const float* tn, const float* to, double pos0, double step, int m, int N,
                                float* vn, float* vo) noexcept
{
    constexpr double a = kPi / 32.0;
    alignas (32) double wgt[64];
    for (int i = 0; i < m; ++i)
    {
        const auto pos = pos0 + step * i;
        const auto ip = (long long) pos;
        const auto f = std::max (pos - (double) ip, 1e-25);
        const auto ix = (std::size_t) ((ip & (N - 1)) + 32);
        double sbf, cbf, saf, caf;
        sinCos (T.b * f, sbf, cbf);
        sinCos (a * f, saf, caf);
        for (int t = 0; t < 64; ++t)
        {
            const auto d = f - T.kk[t];
            const auto sn = sbf * T.cb[t] - cbf * T.sb[t];
            const auto w = caf * T.cA[t] + saf * T.sA[t];
            wgt[t] = (sn / (kPi * d)) * (0.36 + w + 0.64 * w * w);
        }
        const float* pn = tn + ix - 31;
        const float* po = to + ix - 31;
        double an[4] {}, ao[4] {};
        for (int t = 0; t < 64; t += 4)
            for (int l = 0; l < 4; ++l)
            {
                an[l] += (double) pn[t + l] * wgt[t + l];
                ao[l] += (double) po[t + l] * wgt[t + l];
            }
        vn[i] = (float) (0.5 * ((an[0] + an[1]) + (an[2] + an[3])));
        vo[i] = (float) (0.5 * ((ao[0] + ao[1]) + (ao[2] + ao[3])));
    }
}

void sincChunkPlain (const SincTables& T, const float* tn, const float* to, double pos0, double step, int m, int N, float* vn, float* vo) noexcept
{
    sincChunkBody (T, tn, to, pos0, step, m, N, vn, vo);
}

#ifdef OGUN_X86
__attribute__ ((target ("avx"))) void sincChunkAvx (const SincTables& T, const float* tn, const float* to, double pos0, double step, int m,
                                                   int N, float* vn, float* vo) noexcept
{
    sincChunkBody (T, tn, to, pos0, step, m, N, vn, vo);
}
#endif

void sincChunk (const SincTables& T, const float* tn, const float* to, double pos0, double step, int m, int N, float* vn, float* vo) noexcept
{
#ifdef OGUN_X86
    static const bool avx = ogunCpuHasAvx();
    if (avx)
    {
        sincChunkAvx (T, tn, to, pos0, step, m, N, vn, vo);
        return;
    }
#endif
    sincChunkPlain (T, tn, to, pos0, step, m, N, vn, vo);
}

// radix-2 passes (decimation in time, sign +) on split re/im arrays, lengths 8…Nh: block by block up to `block`
// points (cache-resident), then the remaining lengths over the whole array; twiddles per length contiguous
OGUN_INLINE void fftPassesBody (double* zr, double* zi, const double* twr, const double* twi, int Nh, int block) noexcept
{
    auto pass = [&] (int from, int to, int L) {
        const int h = L / 2;
        const double* wr = twr + (h - 4);
        const double* wi = twi + (h - 4);
        for (int i = from; i < to; i += L)
        {
            double* ar = zr + i;
            double* ai = zi + i;
            double* br = ar + h;
            double* bi = ai + h;
            for (int j = 0; j < h; ++j)
            {
                const auto c = wr[j], s = wi[j];
                const auto tr = br[j] * c - bi[j] * s, ti = br[j] * s + bi[j] * c;
                const auto ur = ar[j], ui = ai[j];
                ar[j] = ur + tr, ai[j] = ui + ti;
                br[j] = ur - tr, bi[j] = ui - ti;
            }
        }
    };
    const int bl = std::min (block, Nh);
    for (int start = 0; start < Nh; start += bl)
        for (int L = 8; L <= bl; L <<= 1)
            pass (start, start + bl, L);
    for (int L = bl * 2; L <= Nh; L <<= 1)
        pass (0, Nh, L);
}

void fftPassesPlain (double* zr, double* zi, const double* twr, const double* twi, int Nh, int block) noexcept
{
    fftPassesBody (zr, zi, twr, twi, Nh, block);
}

#ifdef OGUN_X86
__attribute__ ((target ("avx"))) void fftPassesAvx (double* zr, double* zi, const double* twr, const double* twi, int Nh, int block) noexcept
{
    fftPassesBody (zr, zi, twr, twi, Nh, block);
}
#endif

void fftPasses (double* zr, double* zi, const double* twr, const double* twi, int Nh, int block) noexcept
{
#ifdef OGUN_X86
    static const bool avx = ogunCpuHasAvx();
    if (avx)
    {
        fftPassesAvx (zr, zi, twr, twi, Nh, block);
        return;
    }
#endif
    fftPassesPlain (zr, zi, twr, twi, Nh, block);
}

struct Fft
{
    int N = 0, Nh = 0;
    std::vector<int> rev;
    std::vector<double> twr, twi; // per stage (length L ≥ 8): e^{2πij/L}, j < L/2, at index L/2 − 4
    std::vector<double> hr, hi;   // e^{2πik/N}, k < Nh
    static constexpr int block = 4096;

    void init (int n)
    {
        N = n, Nh = n / 2;
        int logNh = 0;
        while ((1 << logNh) < Nh)
            ++logNh;
        rev.assign ((std::size_t) Nh, 0);
        for (int i = 0; i < Nh; ++i)
        {
            int r = 0;
            for (int b = 0; b < logNh; ++b)
                if (i & (1 << b))
                    r |= 1 << (logNh - 1 - b);
            rev[(std::size_t) i] = r;
        }
        twr.assign ((std::size_t) std::max (0, Nh - 4), 0.0);
        twi.assign (twr.size(), 0.0);
        for (int L = 8; L <= Nh; L <<= 1)
            for (int j = 0; j < L / 2; ++j)
            {
                const auto a = 2.0 * kPi * j / L;
                twr[(std::size_t) (L / 2 - 4 + j)] = std::cos (a);
                twi[(std::size_t) (L / 2 - 4 + j)] = std::sin (a);
            }
        hr.resize ((std::size_t) Nh), hi.resize ((std::size_t) Nh);
        for (int k = 0; k < Nh; ++k)
        {
            const auto a = 2.0 * kPi * k / N;
            hr[(std::size_t) k] = std::cos (a), hi[(std::size_t) k] = std::sin (a);
        }
    }

    // e: spectrum bins 1…M (re/im float32 values); out: N float samples; z: scratch (2·Nh doubles)
    void inverse (const float* eRe, const float* eIm, int M, float* out, double* z) const noexcept
    {
        double* zr = z;
        double* zi = z + Nh;
        // Z[k] = A + i·B, A = X[k] + conj(X[Nh−k]), B = (X[k] − conj(X[Nh−k]))·e^{2πik/N}, written bit-reversed
        std::fill (z, z + 2 * (std::size_t) Nh, 0.0);
        auto put = [&] (int k) {
            double ar = 0, ai = 0, br = 0, bi = 0;
            if (k >= 1 && k <= M)
                ar = eRe[k - 1], ai = eIm[k - 1];
            const int kb = Nh - k;
            if (kb >= 1 && kb <= M)
                br = eRe[kb - 1], bi = eIm[kb - 1];
            const auto Ar = ar + br, Ai = ai - bi;
            const auto Dr = ar - br, Di = ai + bi;
            const auto Br = Dr * hr[(std::size_t) k] - Di * hi[(std::size_t) k];
            const auto Bi = Dr * hi[(std::size_t) k] + Di * hr[(std::size_t) k];
            const auto j = (std::size_t) rev[(std::size_t) k];
            zr[j] = Ar - Bi;
            zi[j] = Ai + Br;
        };
        const int lo = std::min (M, Nh - 1);
        for (int k = 0; k <= lo; ++k)
            put (k);
        for (int k = std::max (lo + 1, Nh - M); k < Nh; ++k)
            put (k);
        // lengths 2 and 4 together
        if (Nh >= 4)
            for (int i = 0; i < Nh; i += 4)
            {
                double* r = zr + i;
                double* m = zi + i;
                const auto b0r = r[0] + r[1], b0i = m[0] + m[1], b1r = r[0] - r[1], b1i = m[0] - m[1];
                const auto b2r = r[2] + r[3], b2i = m[2] + m[3], b3r = r[2] - r[3], b3i = m[2] - m[3];
                r[0] = b0r + b2r, m[0] = b0i + b2i, r[2] = b0r - b2r, m[2] = b0i - b2i;
                r[1] = b1r - b3i, m[1] = b1i + b3r, r[3] = b1r + b3i, m[3] = b1i - b3r; // ± i·b3
            }
        else if (Nh == 2)
        {
            const auto r = zr[0], i = zi[0];
            zr[0] = r + zr[1], zi[0] = i + zi[1], zr[1] = r - zr[1], zi[1] = i - zi[1];
        }
        if (Nh >= 8)
            fftPasses (zr, zi, twr.data(), twi.data(), Nh, block);
        for (int m = 0; m < Nh; ++m)
        {
            out[2 * m] = (float) zr[m];
            out[2 * m + 1] = (float) zi[m];
        }
    }
};

// ---------------------------------------------------------------------------------------------------------------
// biquad designer (normalised by a0) and the library's stereo biquad

struct Coef
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

Coef design (int dtype, double A, double p3, double sn, double cs) noexcept
{
    Coef c;
    if (dtype == 6)
    {
        const auto d2 = (A - 1.0) * cs, q = p3 * sn;
        const auto n = 1.0 / (A + 1.0 + d2 + q);
        c.a1 = n * ((A - 1.0) + (A + 1.0) * cs) * -2.0;
        c.a2 = n * ((A + 1.0 + d2) - q);
        c.b1 = n * A * 2.0 * ((A - 1.0) - (A + 1.0) * cs);
        c.b0 = n * A * (((A + 1.0) - d2) + q);
        c.b2 = n * A * (((A + 1.0) - d2) - q);
    }
    else if (dtype == 7)
    {
        const auto n = 1.0 / (p3 / A + 1.0);
        c.a1 = n * cs * -2.0;
        c.a2 = n * (1.0 - p3 / A);
        c.b1 = c.a1;
        c.b0 = n * (p3 * A + 1.0);
        c.b2 = n * (1.0 - p3 * A);
    }
    else if (dtype == 8)
    {
        const auto d2 = (A - 1.0) * cs, q = p3 * sn;
        const auto n = 1.0 / (((A + 1.0) - d2) + q);
        c.a1 = n * ((A - 1.0) - (A + 1.0) * cs) * 2.0;
        c.a2 = n * (((A + 1.0) - d2) - q);
        c.b1 = n * A * -2.0 * ((A - 1.0) + (A + 1.0) * cs);
        c.b0 = n * A * (A + 1.0 + d2 + q);
        c.b2 = n * A * ((A + 1.0 + d2) - q);
    }
    return c;
}

// direct form 1 in double per sample, float32 output (in place); afterwards the 8 state values are rounded to float32
// and values below 2^-24 zeroed. st: x1[2], x2[2], y1[2], y2[2]
void biquadRun (const Coef& k, double* st, float* L, float* R, int n) noexcept
{
    for (int ch = 0; ch < 2; ++ch)
    {
        float* x = ch == 0 ? L : R;
        auto x1 = st[ch], x2 = st[2 + ch], y1 = st[4 + ch], y2 = st[6 + ch];
        for (int i = 0; i < n; ++i)
        {
            const auto xv = (double) x[i];
            const auto yv = xv * k.b0 + (((x1 * k.b1 - y2 * k.a2) + x2 * k.b2) - y1 * k.a1);
            x[i] = (float) yv;
            x2 = x1, x1 = xv, y2 = y1, y1 = yv;
        }
        st[ch] = x1, st[2 + ch] = x2, st[4 + ch] = y1, st[6 + ch] = y2;
    }
    for (int i = 0; i < 8; ++i)
    {
        const auto v = f32 (st[i]);
        st[i] = std::fabs (v) < tiny24 ? 0.0 : v;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// the patch: everything that depends on the state and the rate (immutable, shared by an engine's voices)

constexpr double uniD[9] { 0.877, 1.069, 0.929, 1.181, 1.031, 1.129, 0.887, 1.117, 0.977 };

// chorus jitter table (DATA 0x8e2b7c, round 37 §1.16)
constexpr std::uint32_t chorusJitterBits[20] {
    0x3f778d50, 0x3f84fdf4, 0x3f82b021, 0x3dcf7660, 0x3f7a1cac, 0x3f843958, 0x3f7f3b64, 0x3f789375, 0x3f87ced9, 0x3f726e98,
    0x3f83f7cf, 0x3dcea4a9, 0x3f881062, 0x3f73f7cf, 0x3f7db22d, 0x3f8645a2, 0x3f7ba5e3, 0x3f86872b, 0x3f826e98, 0x3f88d4fe };

struct ChorusTap
{
    int ch = 0;
    float gL = 0, gR = 0;
    double centre = 0, s8 = 0, s9 = 0, s10 = 0;
    std::uint32_t inc = 0, ph0 = 0;
};

struct Art
{
    double base = 0, envamt = 0, lfoamt = 0;
    int mode = 0;
    Curve env, lfo, key, vel;
    std::array<int, 4> adsr {}, lfoKnobs {};
    int lfoExtra = 0;
    std::vector<float> xtab, ytab; // 513 points
};

struct Patch
{
    StateData S;
    std::array<std::int32_t, 64> P {};
    std::array<std::uint8_t, 0x28> fl {}; // bytes +0x200…+0x227
    int quality = 0, interp = 1, qt = 0, osf = 1, rich = 0, N = 0, M = 0, hop = 0, B = 0, Lmap = 0;
    double hopMs = 50, fsHost = 44100, fs = 44100, r588 = 1, xysmooth = 0, tempo = 140, envRate = 0, envRateTempo = 0;
    bool oversampleFilter = true;
    double rateLimit = 0, declickLen = 0, master = 1;
    int masterCents = 0;
    double pre = 0, dec = -1, rel = 0, full = 0, morph = 0, fcut = 0, fres = 0;
    long long seed1 = -1, seed2 = -1;
    int ftype = 0, forder = 0, fflat = 0;
    bool alt = false, norm = false, isqrt = false, logx = false;
    std::array<Art, 13> arts;
    bool hasForced = false;
    std::vector<float> mForced, mImp, mDec, mRel; // forced stored as 1 − F (length M); others ≥ M
    int first = 0, last = 0;
    double gnorm = 1.0;
    std::vector<float> phRe, phIm;
    Fft fft;
    double speedK = 0; // f32(33488.07421875·2^q)
    // EQ
    struct Band
    {
        bool on = false;
        Coef c;
    };
    std::array<Band, 3> eq;
    // chorus
    bool chorusOn = false;
    float chorusDry = 1;
    std::vector<ChorusTap> taps;
    int chorusD = 0, chorusRing = 0;
    // delay
    bool delayOn = false;
    int delaySize = 0, delayLen = 1, delayOffL = 0, delayOffR = 0, delayMode = 0;
    double delayFb = 0, delayWet = 0;
    // reverb
    bool reverbOn = false;
    PlateReverb reverbProto; // coefficients and line lengths (lines empty; the engine allocates its own)
    int reverbPre = 1;
    float reverbWet = 0;
    int tailLen = 0;
    int maxBlock = 512;
    // Autogun's Magic Dust: FL's Soundgoodizer engine (Maximus, Shared build) loaded with Autogun\Data\SG.dat, band mix =
    // P63, in place on the host-rate output when P63 > 0 (REA_rodada57 §1.4); null for Ogun or without SG.dat
    std::shared_ptr<const hlfx::MaximusPreset> sg;
    int sgMix = 0;
    std::vector<std::string> fallbacks;

    std::uint8_t flag (int off) const noexcept { return fl[(std::size_t) (off - 0x200)]; }
};

std::shared_ptr<Patch> buildPatch (const StateData& S, double fsHost, int quality, double tempo, int maxBlock)
{
    auto pp = std::make_shared<Patch>();
    auto& p = *pp;
    p.S = S;
    p.P = S.P;
    const auto& P = p.P;
    for (int o = 0x200; o < 0x228; ++o)
        p.fl[(std::size_t) (o - 0x200)] = S.byte (o);
    p.fsHost = fsHost;
    p.maxBlock = maxBlock;
    const int q = std::clamp (quality >= 0 ? quality : P[54], 0, 4);
    p.quality = q;
    static constexpr int tab[5][5] { { 1, 0, 50, 96, 0 }, { 2, 0, 50, 96, 0 }, { 2, 1, 50, 48, 1 }, { 2, 2, 25, 24, 1 }, { 6, 2, 25, 8, 1 } };
    p.interp = tab[q][0];
    p.qt = tab[q][1];
    p.hopMs = tab[q][2];
    const int ctl = tab[q][3];
    p.osf = tab[q][4] ? 2 : 1;
    const auto fs = fsHost * p.osf;
    p.fs = fs;
    p.rich = std::clamp ((int) P[52], 2, 16);
    if (p.rich != P[52])
        p.fallbacks.push_back ("richness out of range");
    if (p.rich < 10 && fs < 88200.0 && p.qt < 1)
        p.qt = 1;
    p.N = 1 << (p.rich + p.qt);
    p.M = (1 << (p.rich - 1)) - 1;
    p.hop = (int) rnd (p.hopMs * 0.001 * fs);
    const auto r588 = ctl * fs / 44100.0;
    p.B = std::max (4, (int) rnd (r588 / 4.0) * 4);
    p.r588 = (p.B * (double) ctl) / (r588 * 96.0);
    p.xysmooth = f32 (std::exp (std::log (0.01) * p.r588 / 96.0));
    p.tempo = tempo;
    const auto d3 = (p.r588 * 96.0) / 44100.0;
    p.envRate = d3 * 2.0;
    p.envRateTempo = d3 * tempo / 60.0;
    p.oversampleFilter = fs < 88200.0;
    p.rateLimit = f32 (0.001 * 44100.0 / fs);
    const auto r20 = 44100.0 / fs;
    p.declickLen = f32 (96.0 / (r20 * r20));
    const auto x = f32 (P[0] * 0.0078125);
    p.master = x == 1.0 ? 1.0 : f32 ((std::exp (x * f32 (std::log (3.0))) - 1.0) * 0.5);
    if (S.autogun)
        p.master = f32 (p.master * 0.25); // Autogun: the same law × 0.25 (REA_rodada57 §1.3)
    p.masterCents = P[1] * 100 + P[2];
    p.pre = f32 (P[5] * f32 (0.001));
    p.dec = P[6] == 10000 ? -1.0 : f32 (P[6] * 0.0001);
    p.rel = f32 (P[7] * 0.0001);
    p.full = f32 (P[8] * f32 (0.001));
    p.morph = f32 (P[9] * f32 (0.001));
    p.seed1 = P[10] - 1 < 0 ? -1 : mix32 ((long long) P[10] - 0x20cf, 0x1cfc);
    p.seed2 = P[11] - 1 < 0 ? -1 : mix32 ((long long) P[11] - 0x20cf, 0x1cfc);
    p.fcut = f32 (P[17] * 0.00390625);
    p.fres = f32 (P[18] * 0.00390625);
    p.ftype = P[49], p.forder = P[50], p.fflat = P[51];
    if (p.forder > 0 && (p.ftype < 0 || p.ftype > 3))
    {
        p.fallbacks.push_back ("unknown filter type (filter off)");
        p.forder = 0;
    }
    p.forder = std::min (p.forder, 8);
    p.alt = p.flag (0x224) != 0;
    p.norm = p.flag (0x225) != 0;
    p.isqrt = p.flag (0x226) != 0;
    p.logx = p.flag (0x213) != 0;
    // articulators
    for (int k = 0; k < 13; ++k)
    {
        const auto& a = S.arts[(std::size_t) k];
        auto& A = p.arts[(std::size_t) k];
        A.base = a.base, A.envamt = a.envamt, A.lfoamt = a.lfoamt, A.mode = a.mode;
        A.env.build (a.env, -1.0);
        A.lfo.build (a.lfo, -1.0);
        A.key.build (a.key, 1.0);
        A.vel.build (a.vel, 1.0);
        A.adsr = a.adsr, A.lfoKnobs = a.lfoKnobs, A.lfoExtra = a.lfoExtra;
        for (int t = 0; t < 2; ++t)
        {
            const Curve c (t == 0 ? a.X : a.Y, 1.0);
            const auto v = c.render (513, f32 (1.0 / 512.0));
            auto& dst = t == 0 ? A.xtab : A.ytab;
            dst.resize (513);
            for (int i = 0; i < 513; ++i)
                dst[(std::size_t) i] = (float) v[(std::size_t) i];
        }
        if (k < 5 && a.lfo.enabled && (a.lfo.flags & 2))
            p.fallbacks.push_back ("global LFO (runs per voice)");
    }
    // per-partial maps (FUN_0086e6c0, FUN_00842960)
    const int M = p.M;
    const int L = std::min (M + M / 2, 0x8000);
    p.Lmap = L;
    std::vector<double> xs ((std::size_t) L);
    for (int i = 0; i < L; ++i)
        xs[(std::size_t) i] = (p.logx ? std::log2 (i + 1.0) / 15.0 : i / 32767.0) + 5.960464477539063e-08;
    std::vector<double> tmp ((std::size_t) L);
    const float fill[4] { 0.0f, 0.0f, 1.0f, 1.0f };
    const double ln3 = std::log (3.0);
    for (int k = 13; k <= 16; ++k)
    {
        const Curve c (S.maps[(std::size_t) (k - 13)], 1.0);
        auto& dst = k == 13 ? p.mForced : k == 14 ? p.mImp : k == 15 ? p.mDec : p.mRel;
        if (k == 13 && c.isConstant() && c.y[0] == 0.0)
        {
            p.hasForced = false;
            continue;
        }
        if (k == 13)
            p.hasForced = true;
        const int n = k == 13 ? M : L;
        c.seqEval (xs.data(), n, tmp.data());
        std::vector<float> v ((std::size_t) n);
        for (int i = 0; i < n; ++i)
            v[(std::size_t) i] = std::clamp ((float) tmp[(std::size_t) i], 0.0f, 1.0f);
        if (k != 13)
            for (auto& e : v)
                e = e * 2.0f == 1.0f ? 1.0f : (float) ((std::exp ((double) (e * 2.0f * (float) ln3)) - 1.0) * 0.5);
        if (p.isqrt && k <= 14 + (p.alt ? 1 : 0))
            for (int i = 0; i < n; ++i)
                v[(std::size_t) i] = v[(std::size_t) i] * (float) (1.0 / std::sqrt ((double) (i + 1)));
        const auto pat = P[58 + (k - 13)];
        if (pat > 0)
            for (int i = 0; i < n; ++i)
                if (pat < 5 ? (i % (pat + 1) == pat) : (pat - 2 != 0 && i % (pat - 2) > 0))
                    v[(std::size_t) i] = fill[k - 13];
        int ln = n;
        while (ln > M && v[(std::size_t) (ln - 1)] == v[(std::size_t) (ln - 2)])
            --ln;
        v.resize ((std::size_t) ln);
        if (k == 13)
            for (auto& e : v)
                e = 1.0f - e;
        dst = std::move (v);
    }
    // update span
    {
        const auto& Ei = p.mImp;
        const auto& Ed = p.mDec;
        int fi = -1, la = -1;
        for (int i = 0; i < M; ++i)
        {
            const bool nz = p.alt ? std::fabs ((double) Ei[(std::size_t) i] - (double) Ed[(std::size_t) i]) > tiny24
                                  : (double) Ei[(std::size_t) i] > tiny24;
            if (nz)
            {
                if (fi < 0)
                    fi = i;
                la = i;
            }
        }
        p.first = fi >= 0 ? fi : M;
        p.last = la >= 0 ? la + 1 : M;
        p.gnorm = 1.0;
        if (p.norm)
        {
            auto rmsOf = [&] (const std::vector<float>& v) {
                std::vector<float> sq (v.size());
                for (std::size_t i = 0; i < v.size(); ++i)
                    sq[i] = v[i] * v[i];
                const auto s = sq.empty() ? 0.0f : sq[0] + pairwiseSum<float> (sq.data() + 1, sq.size() - 1);
                return std::sqrt ((double) s / M);
            };
            auto r = rmsOf (Ei);
            if (p.alt)
                r = std::max (r, rmsOf (Ed));
            if (r > tiny24)
                p.gnorm = f32 (0.5 / r);
        }
    }
    // phases (seed 3)
    {
        std::vector<float> th ((std::size_t) M);
        rand32f (th.data(), M, 0.0f, 6.2831855f, 3);
        const auto s = 1.0 / std::sqrt ((double) M);
        p.phRe.resize ((std::size_t) M), p.phIm.resize ((std::size_t) M);
        for (int i = 0; i < M; ++i)
        {
            auto re = (float) (s * std::cos ((double) th[(std::size_t) i]));
            auto im = (float) (s * std::sin ((double) th[(std::size_t) i]));
            p.phRe[(std::size_t) i] = std::fabs (re) < (float) tiny24 ? 0.0f : re;
            p.phIm[(std::size_t) i] = std::fabs (im) < (float) tiny24 ? 0.0f : im;
        }
    }
    p.fft.init (p.N);
    p.speedK = f32 (f32 (33488.07421875) * (double) (1 << p.qt));
    // EQ (FUN_0086a670, FUN_00625290)
    for (int k = 0; k < 3; ++k)
    {
        const auto level = f32 (P[40 + k] * f32 (0.01));
        auto fq = f32 (f32 (std::exp (f32 (P[43 + k] * 1.52587890625e-05) * std::log (101.0)) - 1.0) * 159.9 + 10.0);
        const auto width = f32 (P[46 + k] * 1.52587890625e-05);
        const int dtype = S.eqtype[(std::size_t) k] + 6;
        const auto nyq = f32 (fs * 0.4999);
        if (fq > nyq)
            fq = nyq;
        const auto w = fq * (2.0 * kPi / fs);
        const auto sn = std::sin (w), cs = std::cos (w);
        double A, p3;
        if (dtype == 7)
        {
            A = std::exp (level * 0.025 * std::log (10.0));
            const auto f1 = f32 (std::exp (width * std::log (1001.0)) - 1.0);
            p3 = sn / ((f1 * 0.0999 + 0.1) * 2.0);
        }
        else
        {
            A = std::exp (level * 0.025 * std::log (10.0));
            p3 = std::sqrt ((A * A + 1.0) / (width * 1.9 + 0.1) - (A - 1.0) * (A - 1.0));
        }
        auto& b = p.eq[(std::size_t) k];
        b.on = level != 0.0 && dtype >= 6 && dtype <= 8;
        if (level != 0.0 && ! b.on)
            p.fallbacks.push_back ("unknown EQ band type (band off)");
        if (b.on)
            b.c = design (dtype, A, p3, sn, cs);
    }
    p.tailLen = (int) ((long long) fs * 100 / 44100);
    // chorus (FUN_0086a2d0, FUN_00840690; our ogfx_chorus.py)
    if (P[19] >= 1)
    {
        p.chorusOn = true;
        const int order = std::max ((int) P[19], 1);
        const auto depth = f32 (P[20] * 0.0009765625), speed = f32 (P[21] * 0.0009765625);
        const auto delay = (double) ((float) P[22] * 0.0078125f);
        const auto spread = (double) ((float) P[23] * 0.0078125f);
        const auto cross = (double) ((float) P[24] * 0.0078125f);
        const auto m = (double) ((float) std::abs ((int) P[25]) * 0.0078125f);
        const double LN11 = std::log (11.0), LN101 = std::log (101.0), LN1001 = std::log (1001.0), LN5 = std::log (5.0);
        p.chorusDry = (float) (1.0f - (float) (std::exp (m * LN5) - 1.0) * 0.25f);
        const int sg = P[25] == 0 ? 0 : (P[25] > 0 ? 1 : -1);
        const auto wet = (float) ((float) sg * (1.0f - (float) (std::exp ((double) (float) (1.0f - (float) m) * LN5) - 1.0) * 0.25f));
        auto warp = [&] (double v, double scale) { return (float) ((float) (std::exp ((double) (float) v * LN11) - 1.0) * (float) ((float) scale / 10.0f)); };
        const auto A = warp (depth, 1092.5);
        const auto r = f32 (std::exp ((double) (float) (1.0f - (float) speed) * LN101) - 1.0);
        const auto K = (2147483648.0 / (r * 75000.0 + 50000.0)) * 500.0;
        const auto Bd = warp (delay, 4410.0);
        const auto sp = (float) ((float) (std::exp (spread * LN1001) - 1.0) / 1000.0f);
        const auto fsr = (float) (44100.0 / (double) (int) fs);
        auto xmix = [&] (double a, double b) { return a * (double) sp + b * (double) (1.0f - sp); };
        const int cnt = 2 * order;
        float maxd = 0.0f, gsum = 0.0f;
        for (int i = 0; i < cnt; ++i)
        {
            float t;
            int ch;
            std::uint32_t ph0;
            if (i < order)
                t = (float) i, ch = 0, ph0 = 0;
            else
                t = (float) (cnt - i - 1), ch = 1, ph0 = 0x80000000u;
            const float w = 1.0f - t / (float) order;
            float jt;
            std::memcpy (&jt, &chorusJitterBits[i % 20], 4);
            const auto jj = f32 (xmix ((double) (jt * jt), 1.0));
            float gL = 1.0f, gR = (float) cross * w;
            if (ch)
                std::swap (gL, gR);
            gsum = (float) ((double) gsum + std::fabs ((double) gL) + std::fabs ((double) gR));
            auto base = A * w + 10.0f;
            const auto k0 = K / (double) (A + 10.0f);
            const auto k1 = K * jj / (double) base;
            const auto kk = xmix (k1, k0);
            const auto dmod = (float) xmix ((double) ((Bd * w) * (float) jj), (double) Bd);
            const auto off = (long long) rnd ((double) ((w * sp) * 2147483648.0f / 256.0f));
            ph0 = (std::uint32_t) (ph0 + (std::uint32_t) ((std::uint64_t) off << 8));
            const auto inc = (std::uint32_t) (std::int64_t) rnd (kk * (double) fsr);
            base = base / fsr;
            auto qd = dmod / fsr;
            if (! (qd > 8.0f))
                qd = 8.0f;
            const float centre = base + qd;
            const float v = centre + base;
            if (! (maxd > v))
                maxd = v;
            const auto sq = std::sqrt ((double) (base * 4.0f));
            ChorusTap tp;
            tp.ch = ch, tp.gL = gL, tp.gR = gR, tp.centre = (double) centre, tp.inc = inc, tp.ph0 = ph0;
            tp.s8 = f32 (sq / 2147483648.0), tp.s9 = f32 (sq), tp.s10 = f32 (sq / 2.0);
            p.taps.push_back (tp);
        }
        auto g = (float) (1.0f - (float) (std::exp ((double) (float) (1.0f - 1.0f / gsum) * LN1001) - 1.0) / 1000.0f);
        g = g * wet;
        for (auto& tp : p.taps)
            tp.gL = tp.gL * g, tp.gR = tp.gR * g;
        p.chorusD = (int) rnd ((double) maxd) + 8;
        p.chorusRing = maxBlock * p.osf + p.chorusD;
        p.tailLen += p.chorusRing;
    }
    // delay (FUN_00869dd0; our ogfx_delay.py)
    if (S.b358[1])
    {
        p.delayOn = true;
        const int ifs = (int) fs;
        const auto a = f32 (std::abs ((int) P[28]) * 0.001953125);
        const auto factor = f32 (f32 (std::exp (a * std::log (1001.0)) - 1.0) * 50.0);
        const int off = (int) rnd ((double) ((float) factor * (float) ifs / 100000.0f));
        double d;
        if (S.b358[0] != 0)
            d = ((double) P[27] * ifs) * 0.3125 / f32 (tempo);
        else
            d = ((double) P[27] * ifs) / 384.0;
        p.delayLen = std::max (1, (int) rnd (d));
        p.delaySize = maxBlock * p.osf + off + p.delayLen;
        p.delayOffL = P[28] > 0 ? 0 : off;
        p.delayOffR = P[28] > 0 ? off : 0;
        p.delayFb = f32 (P[26] * 0.0078125);
        const auto v = f32 (P[29] * 0.0078125);
        const auto av = f32 (std::fabs (v));
        double g;
        if (av == 1.0)
            g = 1.0;
        else
            g = f32 ((std::exp ((double) ((float) av * 2.39789534f)) - 1.0) * f32 (0.1));
        p.delayWet = f32 (g * (v == 0.0 ? 0.0 : (v < 0.0 ? -1.0 : 1.0)));
        p.delayMode = P[30];
    }
    // reverb (FUN_0086a0a0, FUN_0083f840; DSP in core/PlateReverb.h)
    if (P[53] & 0xff)
    {
        p.reverbOn = true;
        const int ifs = (int) fs;
        double pd;
        if (p.flag (0x201))
            pd = ((double) P[33] * ifs) * 0.3125 / f32 (tempo);
        else
            pd = ((double) P[33] * ifs) / 384.0;
        p.reverbPre = std::max (1, (int) rnd (pd));
        const auto decay = f32 (((double) P[36] * ifs) / 10.0);
        const auto lowcut = P[31] == 0x13 ? 0.0 : (double) P[31];
        const auto highcut = P[32] == 0xdd ? 0.0 : (double) P[32];
        const auto damp = P[37] == 0xdd ? 0.0 : (double) P[37];
        const int room = P[34], diff = P[35], colour = P[38];
        p.reverbWet = (float) P[39] * 0.00390625f;
        auto& R = p.reverbProto;
        const auto inv = f32 (1.0 / ifs);
        auto onepole = [&] (double f) {
            if (f == 0.0)
                return 1.0f;
            const auto h = f32 (ifs * 0.5);
            if (! (h > f))
                f = h;
            const auto w = f * 628.3185307179587 * inv;
            const auto b = f32 (2.0 - std::cos (w));
            const auto bb = f32 (b * b);
            return (float) (1.0 - (b - std::sqrt (bb - 1.0)));
        };
        if (lowcut == 0.0)
            R.cLc = 0.0f;
        else
        {
            auto f = lowcut;
            const auto h = f32 (ifs * 0.5);
            if (! (h > f))
                f = h;
            const auto t = f32 (std::tan (f * kPi * inv));
            R.cLc = (float) ((t - 1.0) / (t + 1.0));
        }
        R.cHc = onepole (highcut);
        auto muldiv = [] (long long a, long long b, long long c) {
            const auto pr = a * b;
            const auto qq = std::llabs (pr) / std::llabs (c);
            return (pr >= 0) == (c > 0) ? qq : -qq;
        };
        const auto Rr = muldiv (ifs, room + 15, 0x73);
        for (int i = 0; i < 12; ++i)
            R.L[(std::size_t) i] = (int) std::max (1LL, muldiv (PlateReverb::lines[i], Rr, 20000));
        for (int i = 0; i < 7; ++i)
            R.tl[(std::size_t) i] = (int) muldiv (PlateReverb::tapsL[i], Rr, 20000), R.tr[(std::size_t) i] = (int) muldiv (PlateReverb::tapsR[i], Rr, 20000);
        const auto fd = f32 (diff * 0.01), hh = f32 (inv * 0.5);
        const double LN_1E3 = std::log (0.001);
        if (fd == 0.0)
            R.cIn = R.c1 = R.c2 = 0.0f;
        else
        {
            auto gg = [&] (int a, int b, double kf) {
                const auto xx = (double) ((float) (a + b) * (float) hh);
                return (float) std::exp (LN_1E3 * xx / (fd * kf));
            };
            R.cIn = gg (R.L[0], R.L[1], 0.07);
            R.c1 = gg (R.L[4], R.L[8], 0.35);
            R.c2 = gg (R.L[6], R.L[10], 0.51);
        }
        const auto Ssum = (double) (float) (R.L[5] + R.L[7] + R.L[9] + R.L[11]);
        R.g = (float) std::exp (0.25 * (LN_1E3 * Ssum / decay));
        R.cDamp = onepole (damp);
        const auto kc = colour >= 0 && colour <= 4 ? PlateReverb::colourFactor[colour] : 1.0;
        const auto d2 = f32 (std::exp (0.25 * (LN_1E3 * Ssum / f32 (decay * (double) (float) kc))));
        auto rr = f32 (d2 / (double) R.g);
        R.c48 = (float) ((rr - 1.0) * 0.5);
        if (rr > 1.0)
            rr = 1.0;
        const auto t = (float) std::tan (500.0 * kPi * inv);
        R.c44 = (float) ((float) (t - (float) rr) / (float) (t + (float) rr));
    }
    return pp;
}

// ---------------------------------------------------------------------------------------------------------------
// voice

struct Sub
{
    double pos = 0, ratio = 1, L = 0, R = 0, det = 0;
    float g[4] {}, d[4] {};
};

// cumulative float32 gain ramp for one chunk (values g, g+d, g+2d...)
inline void accRamp (float& g, float d, int m, float* out) noexcept
{
    if (d == 0.0f)
    {
        for (int i = 0; i < m; ++i)
            out[i] = g;
        return;
    }
    auto x = g;
    for (int i = 0; i < m; ++i)
    {
        out[i] = x;
        x = x + d;
    }
    g = x;
}

inline double hermite6 (double ym2, double ym1, double y0, double y1, double y2, double y3, double x) noexcept
{
    const auto c0 = y0;
    const auto c1 = 1.0 / 12 * (ym2 - y2) + 2.0 / 3 * (y1 - ym1);
    const auto c2 = 13.0 / 12 * ym1 - 25.0 / 12 * y0 + 3.0 / 2 * y1 - 11.0 / 24 * y2 + 1.0 / 12 * y3 - 1.0 / 8 * ym2;
    const auto c3 = 5.0 / 12 * y0 - 7.0 / 12 * y1 + 7.0 / 24 * y2 - 1.0 / 24 * (ym2 + ym1 + y3);
    const auto c4 = 1.0 / 8 * ym2 - 7.0 / 12 * ym1 + 13.0 / 12 * y0 - y1 + 11.0 / 24 * y2 - 1.0 / 12 * y3;
    const auto c5 = 1.0 / 24 * (y3 - ym2) + 5.0 / 24 * (ym1 - y2) + 5.0 / 12 * (y1 - y0);
    return ((((c5 * x + c4) * x + c3) * x + c2) * x + c1) * x + c0;
}

class Engine;

struct Voice
{
    // buffers sized for the patch (allocated once per pool slot)
    std::vector<float> tA, tB;            // two tables (N + 64 guard samples)
    float* tNew = nullptr;
    float* tOld = nullptr;
    std::vector<float> eRe, eIm, tgRe, tgIm, fac, Bw;
    std::vector<float> xL, xR, fL, fR;   // chunk buffers (control block), filter at 2×
    std::vector<SincTables> sinc;        // quality 4: per sub-voice reader tables

    // identity / host
    bool used = false, dead = false;
    long long id = 0;
    int lynxKey = -1;
    bool lynxReleased = false;
    double notePan = 0;
    double fvol = 1, fpan = 0, fcutH = 0, fresH = 0, FP = 0, relvel = 0, note = 0, vel = 0;
    HostSlide slide; // a slide note moving Final.Pitch (issue #14)
    int keyi = 0, state = 1, cnt = 0;
    bool firstChunk = true;
    double xy[2] {};
    int ixy[2] {};
    double stat[13] {}, out[13] {};
    EnvRun envs[5];
    LfoRun lfos[5];
    bool lfoOn[5] {}, en[5] {};
    double cents0 = 0, cents = 0, upitch = 0, S = 0;
    int envmin = 9;
    float gt[4] {}, gc[4] {}, gs[4] {}, gd[4] {};
    std::vector<std::array<double, 8>> fst;
    int declick = -1;
    float dcG = 0, dcD = 0, dcL = 0, dcR = 0;
    bool timRelOnEnv = false, timReleased = false;

    // timbre (§1.7)
    double tpre = 0, decSamples = 0, p = 0, tmorph = 0, speed = 0, c = 1;
    long long seed1 = -1, seed2 = -1;
    bool alt = false, started = false;
    int shift = 0, rot = 0, nsub = 1;
    std::array<Sub, 9> subs;
    long long cntTotal = 0, frameEnd = 0, inFrame = 0;
    long long silence = 0;
    double thr = 0;
    int tstate = 0, band = 0, tfirst = 0, tlast = 0;
};

class Engine
{
public:
    static constexpr int maxHostBlock = 512;
    static constexpr int maxVoices = 64;

    explicit Engine (std::shared_ptr<const Patch> patch)
        : pt (std::move (patch))
    {
        const auto& p = *pt;
        nb = maxHostBlock * p.osf;
        bufL.assign ((std::size_t) nb, 0.0f), bufR.assign ((std::size_t) nb, 0.0f);
        tmpL.assign ((std::size_t) nb, 0.0f), tmpR.assign ((std::size_t) nb, 0.0f);
        mono.assign ((std::size_t) nb, 0.0f);
        zr.assign (2 * (std::size_t) p.fft.Nh, 0.0);
        sU.assign ((std::size_t) p.M, 0.0f), sV.assign ((std::size_t) p.M, 0.0f), sD.assign ((std::size_t) p.M, 0.0f);
        rampBuf.assign (8 * (std::size_t) p.B, 0.0f);
        if (p.interp == 6)
            sincN.assign ((std::size_t) p.B, 0.0f), sincO.assign ((std::size_t) p.B, 0.0f);
        for (auto& e : eqSt)
            e.fill (0.0);
        if (p.chorusOn)
        {
            chorusPh.resize (p.taps.size());
            for (std::size_t i = 0; i < p.taps.size(); ++i)
                chorusPh[i] = p.taps[i].ph0;
            int sz = 1;
            while (sz < nb + p.chorusD + 16)
                sz <<= 1;
            ringL.assign ((std::size_t) sz, 0.0f), ringR.assign ((std::size_t) sz, 0.0f);
            ringMask = sz - 1;
        }
        if (p.delayOn)
        {
            dline.assign (2 * (std::size_t) p.delaySize, 0.0f);
            dr = 0, dw = p.delayLen;
            fbCur = p.delayFb;
        }
        if (p.reverbOn)
        {
            reverb = p.reverbProto;
            reverb.allocate (p.reverbPre);
        }
        decSt.fill (0.0);
        if (p.sg != nullptr && p.sgMix > 0)
        {
            sg = std::make_unique<hlfx::MaximusProcessor>();
            sg->prepare (p.sg, p.fsHost, p.sgMix);
        }
        quietNeeded = (long long) (2.0 * p.fsHost) + (p.chorusOn ? p.chorusRing : 0) + (p.delayOn ? 4LL * p.delaySize : 0)
                      + (p.reverbOn ? (long long) p.reverbPre + reverb.L[5] + reverb.L[9] : 0);
        quietFor = quietNeeded;
        pool.reserve (maxVoices);
        order.reserve (maxVoices);
        grow (std::min (4, maxVoices));
    }

    const Patch& patch() const noexcept { return *pt; }

    // host values
    int counter = 0;
    int maxpoly = 0;
    bool hostKills = true;
    double hostEv2 = 0;

    // (key, velocity, Final.Vol, Final.Pan, Final.Pitch offset cents, FCut, FRes, release velocity) → voice id
    long long noteOn (int key, double vel, double fvol, double fpan, double pofs, double fcut = 0, double fres = 0,
                      double relvel = 0, int j0shift = 0, int lynxKey = -1, double notePan = 0) noexcept
    {
        const auto& p = *pt;
        if (0 < maxpoly && maxpoly <= nlisted)
        {
            Voice* lastAct = nullptr;
            int act = 0;
            for (auto* u : order)
                if (u->state < 9)
                    ++act, lastAct = u;
            if (act >= maxpoly && lastAct != nullptr)
                kill (*lastAct);
        }
        if (p.flag (0x209) && nlisted > 0)
        {
            const float fp = (float) (100.0 * key - 6000.0 + pofs);
            const auto kk = (int) rnd (std::max ((double) (fp * 0.01f + 60.0f), 0.0));
            for (auto* u : order)
                if (u->keyi == kk && u->state < 9)
                    kill (*u);
        }
        Voice* v = freeVoice();
        if (v == nullptr)
            return -1;
        startVoice (*v, key, vel, fvol, fpan, pofs, fcut, fres, relvel);
        v->id = ++serial;
        v->lynxKey = lynxKey;
        v->lynxReleased = false;
        v->notePan = notePan;
        v->used = true;
        order.push_back (v);
        ++nlisted;
        if (j0shift != 0)
            j0Map.push_back ({ v, j0shift });
        return v->id;
    }

    void release (long long id) noexcept
    {
        for (auto* v : order)
            if (v->id == id)
                voiceRelease (*v);
    }

    // LYNX: releases the oldest held note of a key
    void releaseKey (int key) noexcept
    {
        for (auto* v : order)
            if (v->lynxKey == key && ! v->lynxReleased)
            {
                v->lynxReleased = true;
                voiceRelease (*v);
                return;
            }
    }

    void killAll() noexcept
    {
        for (auto* v : order)
            kill (*v);
    }

    void setLevels (double fvol, double fpan) noexcept
    {
        for (auto* v : order)
            v->fvol = fvol, v->fpan = v->notePan + fpan;
    }

    int voices() const noexcept { return (int) order.size(); }
    bool quiet() const noexcept { return order.empty() && quietFor >= quietNeeded; }

    // one host block (n ≤ maxHostBlock): adds to out
    // a slide note (issue #14): the held voices glide to `to` (Final.Pitch, cents from C5) over `samples`
    void slideTo (double to, float samples) noexcept
    {
        for (auto* v : order)
            if (! v->lynxReleased && ! v->dead)
                v->slide.start ((float) (to - (v->FP - (double) v->slide.cents)), samples);
    }

    void render (float* outL, float* outR, int n) noexcept
    {
        for (auto* v : order) // a slide note moves Final.Pitch (issue #14)
            if (v->slide.moving())
            {
                const auto was = v->slide.cents;
                v->FP += (double) (v->slide.advance ((float) n) - was);
            }
        const auto& p = *pt;
        const int m = n * p.osf;
        std::fill (bufL.begin(), bufL.begin() + m, 0.0f);
        std::fill (bufR.begin(), bufR.begin() + m, 0.0f);
        if (nlisted < 1)
        {
            if (tail > 0)
                tail -= m;
        }
        else
            tail = p.tailLen;
        for (std::size_t i = 0; i < order.size();)
        {
            auto* v = order[i];
            renderVoice (*v, bufL.data(), bufR.data(), m);
            if (v->dead)
            {
                order.erase (order.begin() + (std::ptrdiff_t) i);
                v->used = false;
                for (auto& j : j0Map)
                    if (j.first == v)
                        j.first = nullptr;
                if (hostKills)
                    --nlisted;
            }
            else
                ++i;
        }
        for (int k = 0; k < 3; ++k)
            if (p.eq[(std::size_t) k].on)
                biquadRun (p.eq[(std::size_t) k].c, eqSt[(std::size_t) k].data(), bufL.data(), bufR.data(), m);
        if (p.chorusOn)
            chorus (m, tail > 0);
        if (p.delayOn)
            delay (m, tail > 0);
        if (p.reverbOn)
        {
            for (int i = 0; i < m; ++i)
                mono[(std::size_t) i] = bufL[(std::size_t) i] + bufR[(std::size_t) i];
            reverb.process (mono.data(), tmpL.data(), tmpR.data(), m);
            for (int i = 0; i < m; ++i)
            {
                bufL[(std::size_t) i] = bufL[(std::size_t) i] + tmpL[(std::size_t) i] * p.reverbWet;
                bufR[(std::size_t) i] = bufR[(std::size_t) i] + tmpR[(std::size_t) i] * p.reverbWet;
            }
        }
        if (p.osf > 1)
            decimate (m);
        if (sg != nullptr)
            sg->process (bufL.data(), bufR.data(), n);
        float peak = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            outL[i] += bufL[(std::size_t) i];
            outR[i] += bufR[(std::size_t) i];
            peak = std::max (peak, std::max (std::fabs (bufL[(std::size_t) i]), std::fabs (bufR[(std::size_t) i])));
        }
        if (! order.empty() || peak > 1.0e-7f)
            quietFor = 0;
        else
            quietFor += n;
    }

    // nothing sounds: only the chorus phases (and the effect tail counter) advance, as in Gen_Render without voices
    void skip (int n) noexcept
    {
        const auto& p = *pt;
        const int m = n * p.osf;
        if (tail > 0)
            tail -= m;
        if (p.chorusOn)
            for (std::size_t i = 0; i < chorusPh.size(); ++i)
                chorusPh[i] = chorusPh[i] + p.taps[i].inc * (std::uint32_t) m;
        if (p.delayOn)
        {
            dr = (int) ((dr + (long long) m) % p.delaySize);
            dw = (int) ((dw + (long long) m) % p.delaySize);
        }
        if (sg != nullptr)
        {
            // the Soundgoodizer stage keeps running on silence (its envelopes relax as in FL's Gen_Render)
            std::fill (tmpL.begin(), tmpL.begin() + n, 0.0f);
            std::fill (tmpR.begin(), tmpR.begin() + n, 0.0f);
            sg->process (tmpL.data(), tmpR.data(), n);
        }
    }

private:
    std::shared_ptr<const Patch> pt;
    std::vector<std::unique_ptr<Voice>> pool;
    std::vector<Voice*> order; // listed voices in note-on order
    std::vector<std::pair<Voice*, int>> j0Map;
    long long serial = 0;
    int nlisted = 0, tail = 0, nb = 0;
    std::vector<float> bufL, bufR, tmpL, tmpR, mono;
    std::vector<double> zr;
    std::vector<float> sU, sV, sD, rampBuf;
    std::array<std::array<double, 8>, 3> eqSt {};
    // chorus
    std::vector<std::uint32_t> chorusPh;
    std::vector<float> ringL, ringR;
    int ringMask = 0, ringPos = 0;
    // delay
    std::vector<float> dline;
    int dr = 0, dw = 0;
    double fbCur = 0;
    // reverb
    PlateReverb reverb;
    // decimator: per channel 12 doubles
    std::array<double, 24> decSt {};
    std::unique_ptr<hlfx::MaximusProcessor> sg; // Autogun's Magic Dust stage
    long long quietFor = 0, quietNeeded = 0;

    void grow (int count)
    {
        const auto& p = *pt;
        for (int i = 0; i < count && (int) pool.size() < maxVoices; ++i)
        {
            auto v = std::make_unique<Voice>();
            v->tA.assign ((std::size_t) p.N + 64, 0.0f);
            v->tB.assign ((std::size_t) p.N + 64, 0.0f);
            v->eRe.assign ((std::size_t) p.M, 0.0f), v->eIm.assign ((std::size_t) p.M, 0.0f);
            if (p.alt)
                v->tgRe.assign ((std::size_t) p.M, 0.0f), v->tgIm.assign ((std::size_t) p.M, 0.0f);
            v->fac.assign ((std::size_t) p.M, 0.0f), v->Bw.assign ((std::size_t) p.M, 0.0f);
            v->xL.assign ((std::size_t) p.B, 0.0f), v->xR.assign ((std::size_t) p.B, 0.0f);
            v->fL.assign (2 * (std::size_t) p.B, 0.0f), v->fR.assign (2 * (std::size_t) p.B, 0.0f);
            v->fst.assign ((std::size_t) std::max (0, p.forder), std::array<double, 8> {});
            if (p.interp == 6)
                v->sinc.assign (9, SincTables {});
            pool.push_back (std::move (v));
        }
    }

    Voice* freeVoice() noexcept
    {
        for (auto& v : pool)
            if (! v->used)
                return v.get();
        if ((int) pool.size() < maxVoices)
        {
            grow (std::min (8, maxVoices - (int) pool.size())); // (allocates: only when the polyphony reaches a new high)
            for (auto& v : pool)
                if (! v->used)
                    return v.get();
        }
        // pool full: the oldest voice is cut at once
        if (order.empty())
            return nullptr;
        auto* v = order.front();
        order.erase (order.begin());
        if (hostKills)
            --nlisted;
        v->used = false;
        return v;
    }

    // ---- articulators ----
    double artStatic (int k, double key, double vel) const noexcept
    {
        const auto& a = pt->arts[(std::size_t) k];
        const auto kv = a.key.valueAt (key * (1.0 / 120.0));
        const auto vv = a.vel.valueAt (vel);
        double s;
        if (a.mode == 0)
            s = kv * vv;
        else if (a.mode == 1)
            s = kv + vv;
        else
            s = kv * vv * 2.0;
        return f32 (s);
    }

    double xyApply (int k, double v, int ix, int iy) const noexcept
    {
        const auto& a = pt->arts[(std::size_t) k];
        for (int t = 0; t < 2; ++t)
        {
            const auto tv = (double) (t == 0 ? a.xtab : a.ytab)[(std::size_t) (t == 0 ? ix : iy)];
            if (a.mode == 0)
                v = v * tv;
            else if (a.mode == 1)
                v = v + tv;
            else
                v = v * tv * 2.0;
        }
        return v;
    }

    static int ixOf (double v, double h) noexcept { return (int) std::clamp (rnd (f32 (v + h * 256.0)), 0LL, 512LL); }

    // ---- voice start (TriggerVoice; ogmodel.Voice.__init__) ----
    void startVoice (Voice& v, int key, double vel, double fvol, double fpan, double pofs, double fcut, double fres, double relvel) noexcept
    {
        const auto& p = *pt;
        const auto& P = p.P;
        v.dead = false;
        v.fvol = fvol, v.fpan = fpan, v.fcutH = fcut, v.fresH = fres;
        v.FP = f32 (100.0 * key - 6000.0 + pofs);
        v.slide = {};
        v.relvel = relvel;
        const auto note = std::max (f32 (v.FP * f32 (0.01) + 60.0), 0.0);
        v.note = note;
        v.keyi = (int) rnd (note);
        v.vel = vel;
        v.state = 1;
        v.cnt = -p.B;
        v.firstChunk = true;
        v.xy[0] = (double) P[3], v.xy[1] = (double) P[4];
        v.ixy[0] = ixOf (v.xy[0], fcut), v.ixy[1] = ixOf (v.xy[1], fres);
        for (int k = 0; k < 13; ++k)
            v.stat[k] = 0.0, v.out[k] = 0.0;
        const bool porta = ((p.flag (0x210) != 0) != (hostEv2 == 14.0)) && hasLastPitch;
        for (int k = 0; k < 13; ++k)
        {
            if (k == 8)
                continue;
            const auto& a = p.arts[(std::size_t) k];
            auto s = artStatic (k, note, vel);
            if (k < 5)
            {
                if (k == 3)
                {
                    s = s * 4.0;
                    if (p.flag (0x20b) == 0)
                        s = s - v.FP * (1.0 / 1200.0);
                    v.cents0 = f32 ((v.FP + (double) p.masterCents) + s * 1200.0);
                }
                v.stat[k] = s;
                auto& env = v.envs[k];
                env.init (a.env, p.envRate, p.envRateTempo, a.adsr);
                v.lfoOn[k] = a.lfo.enabled != 0;
                if (v.lfoOn[k])
                    v.lfos[k].initLfo (a.lfo, p.envRate, p.envRateTempo, a.lfoKnobs, a.lfoExtra);
                v.en[k] = a.env.enabled != 0 || v.lfoOn[k];
                auto val = a.base;
                if (a.env.enabled && porta && k == 3)
                {
                    auto d = f32 (lastPitch - v.cents0);
                    const int m12 = p.flag (0x212);
                    if (m12)
                    {
                        if (m12 < 2)
                        {
                            const auto sg = (double) sgn (d);
                            while (std::fabs (d) > 1250.0)
                                d = f32 (d - sg * 1200.0);
                        }
                        else
                        {
                            const auto lim = (double) ((6 - m12) * 100);
                            const auto sg = (double) sgn (d);
                            d = f32 (sg * std::min (std::fabs (d), lim));
                        }
                    }
                    if (d != 0.0)
                    {
                        env.setMod (f32 (d * (1.0 / 1200.0)));
                        if (p.flag (0x211))
                            env.kn[0] = f32 (env.kn[0] * (std::fabs (d) * (1.0 / 1200.0)));
                        env.relv = env.c->sus >= 0 ? env.kn[2] : 0.0;
                        env.setMod (env.mod);
                    }
                }
                else if (a.env.enabled)
                {
                    const int vo = p.flag (0x20d);
                    if (vo)
                    {
                        if (vo & 1)
                        {
                            env.kn[0] = f32 (env.kn[0] * f32 (2.0 - vel * 2.0));
                            env.relv = env.c->sus >= 0 ? env.kn[2] : 0.0;
                            env.setMod (env.mod);
                        }
                        if ((vo & 2) && env.c->decay >= 0)
                        {
                            double cum = 0.0;
                            for (int i = 0; i <= env.c->decay; ++i)
                                cum += env.c->dx[(std::size_t) i];
                            env.step (cum * env.kn[0] * vel / env.rate0);
                        }
                    }
                    val = val + env.out * a.envamt;
                }
                if (v.lfoOn[k])
                {
                    if (a.mode == 1)
                        val = val + v.lfos[k].lout * a.lfoamt;
                    else
                        val = val * (v.lfos[k].lout * a.lfoamt * 2.0 + 1.0);
                }
                if (! v.en[k])
                    v.out[k] = s;
                else if (a.mode == 0)
                    v.out[k] = val * s;
                else if (a.mode == 1)
                    v.out[k] = val + s;
                else
                    v.out[k] = val * s * 2.0;
            }
            else
                v.stat[k] = xyApply (k, s, v.ixy[0], v.ixy[1]);
        }
        // timbre start parameters
        const auto* st = v.stat;
        double dec = -1.0;
        if (p.dec >= 0.0)
            dec = f32 (f32 (f32 (f32 (std::exp (p.dec * std::log (21.0)) - 1.0) * 997.5 * f32 (st[7])) + 50.0) * 0.001 * p.fs);
        const auto full = std::min (std::max (f32 (p.full + st[9] * 2.0), -1.0), 1.0);
        auto s1 = p.seed1;
        if (p.flag (0x20c))
            s1 = mix32 ((long long) P[10] + rnd (note) - 0x1f88, 0x1e42);
        long long s2;
        if (p.seed2 < 0)
            s2 = mix32 ((long long) counter - 0x200e, 0x1dbc), ++counter;
        else
            s2 = p.seed2;
        const auto morph = f32 (p.morph * st[10]);
        const auto ratio = std::exp (v.cents0 * (1.0 / 1200.0) * LN2);
        const auto eqs = std::min (std::max (f32 (st[12]), -0.5), 0.5);
        const int n = std::clamp ((int) P[12], 1, 9);
        // Timbre.__init__
        v.tpre = p.pre * st[6];
        v.decSamples = dec;
        const auto mag = f32 ((std::exp (std::fabs (full) * std::log (10000001.0)) - 1.0) * 0.01);
        v.p = f32 (mag * -(double) sgn (full));
        v.seed1 = s1, v.seed2 = s2, v.tmorph = morph;
        v.alt = p.alt;
        v.shift = (int) rnd (eqs * p.M);
        {
            const auto r = rnd (f32 (st[11]) * p.M + (double) ((long long) p.M << 8));
            v.rot = (int) (((r % p.M) + p.M) % p.M);
        }
        v.speed = p.speedK / p.fs * ratio;
        v.nsub = n;
        for (auto& sb : v.subs)
            sb = Sub {};
        v.started = false;
        v.cntTotal = 0, v.frameEnd = p.hop, v.inFrame = 0;
        v.silence = rnd (p.fs * 0.1);
        v.thr = f32 (std::exp (std::log (10.0) / 20.0 * -80.0));
        v.tstate = 0;
        v.c = 1.0;
        v.cents = v.cents0;
        // unison
        v.upitch = f32 (P[15] * v.out[4]);
        const auto S = f32 (f32 (std::exp (f32 (v.upitch * 0.00390625) * std::log (11.0)) - 1.0) * 5.0);
        const auto d23 = 1.0 - f32 (P[16] * 0.0078125) * st[5];
        const auto sp = std::exp (f32 (d23) * std::log (101.0));
        const auto base = (double) (1.0f - (float) ((float) (sp - 1.0) * 0.01f));
        const auto spread = f32 (std::pow (base, (double) p.rich));
        const auto vv = f32 (P[14] * 0.0078125);
        const auto g3 = n < 3 ? 1.0 : f32 (1.0 / std::sqrt ((double) n));
        for (int k = 0; k < n; ++k)
        {
            auto& sb = v.subs[(std::size_t) k];
            double L, R, ph, fr;
            if (n == 1)
            {
                sb.det = 0.0;
                L = R = f32 (0.3);
                ph = 0.0, fr = 1.0;
            }
            else
            {
                const float fk = (float) k, fn = (float) n;
                ph = (double) (((fk / fn) - (((float) (n - 1) / fn) * 0.5f)) * (float) spread);
                const auto u = f32 (f32 ((double) k / (n - 1)) * 2.0 - 1.0);
                auto au = std::fabs (u);
                const auto gain = n == 2 ? 0.3 : f32 (f32 (g3 * vv + g3 * 2.0 * (1.0 - au) * (1.0 - vv)) * 0.3);
                const auto pan = f32 (u * 0.0078125 * P[13]);
                const auto ang = (1.0 + pan) * 0.7853981633974483;
                L = f32 (std::cos (ang) * gain);
                R = f32 (std::sin (ang) * gain);
                if (k & 1)
                    au = -au;
                if ((n & 1) && n < 2 * k)
                    au = -au;
                sb.det = f32 (au * uniD[k]);
                fr = f32 (std::exp (f32 (sb.det * f32 (S * f32 (LN2 / 1200.0)))));
            }
            sb.ratio = fr, sb.pos = ph, sb.L = L, sb.R = R;
        }
        v.S = S;
        for (int i = 0; i < 4; ++i)
            v.gt[i] = v.gc[i] = v.gs[i] = v.gd[i] = 0.0f;
        for (auto& f : v.fst)
            f.fill (0.0);
        v.declick = -1;
        if (p.flag (0x20e))
        {
            v.declick = (int) rnd (p.declickLen);
            v.dcG = 0.0f;
            v.dcD = (float) (1.0 / std::max (1, v.declick));
            v.dcL = v.dcR = 0.0f;
        }
        v.timRelOnEnv = false;
        if (p.flag (0x20f))
        {
            const auto& e = p.arts[0].env;
            v.timRelOnEnv = e.enabled && e.decay >= 0;
        }
        v.timReleased = false;
        lastPitch = v.cents0;
        hasLastPitch = true;
        v.tNew = v.tA.data(), v.tOld = v.tB.data();
    }

    void kill (Voice& v) noexcept
    {
        if (v.state < 9)
            v.state = 9;
        for (int k = 0; k < 5; ++k)
        {
            v.envs[k].stage = 9, v.envs[k].sub = 9;
            if (v.lfoOn[k])
                v.lfos[k].stage = 9, v.lfos[k].sub = 9;
        }
    }

    void voiceRelease (Voice& v) noexcept
    {
        if (v.state < 5)
            v.state = 5;
        if (! v.timRelOnEnv)
            timRelease (v, v.relvel);
        for (int k = 0; k < 5; ++k)
        {
            v.envs[k].release();
            if (v.lfoOn[k])
                v.lfos[k].release();
        }
    }

    void timRelease (Voice& v, double rv) noexcept
    {
        if (v.timReleased)
            return;
        v.timReleased = true;
        const auto& p = *pt;
        rv = std::min (std::max (rv, 0.0), 1.0);
        auto a8 = artStatic (8, v.note, rv);
        a8 = xyApply (8, a8, v.ixy[0], v.ixy[1]);
        const auto t = f32 (f32 (f32 (std::exp (p.rel * std::log (21.0)) - 1.0) * 997.5 * a8 + 50.0) * 0.001 * p.fs);
        timbreRelease (v, t);
    }

    // ---- per control block (ogmodel.Voice.control) ----
    static void volpan (double pan, double vol, float* g) noexcept
    {
        if (pan == 0.0)
        {
            g[0] = g[1] = (float) vol, g[2] = g[3] = 0.0f;
            return;
        }
        if (vol == 0.0)
        {
            g[0] = g[1] = g[2] = g[3] = 0.0f;
            return;
        }
        const auto ang = (1.0 + pan) * 0.7853981633974483;
        const auto vs = f32 (vol * f32 (std::sqrt (2.0)));
        const float L = (float) (std::cos (ang) * vs), R = (float) (std::sin (ang) * vs);
        const float fv = (float) vol;
        float f3, f2;
        if (pan <= 0.0)
            f3 = 1.0f - R / fv, f2 = 0.0f;
        else
            f3 = 0.0f, f2 = 1.0f - L / fv;
        g[2] = L * f3 * 0.5f;
        g[0] = L * (1.0f - f3 * 0.5f);
        g[3] = R * f2 * 0.5f;
        g[1] = R * (1.0f - f2 * 0.5f);
    }

    void ramp (float cur, float tgt, int n, float& start, float& d, float& newc) const noexcept
    {
        start = cur;
        auto dd = tgt - start;
        if (dd == 0.0f)
        {
            d = 0.0f, newc = cur;
            return;
        }
        newc = tgt;
        dd = dd / (float) n;
        const auto lim = (float) pt->rateLimit;
        if (std::fabs (dd) > lim)
        {
            dd = std::copysign (lim, dd);
            newc = start + dd * (float) n;
            if (std::fabs ((double) newc) < tiny24)
            {
                newc = 0.0f;
                dd = -start / (float) n;
            }
        }
        d = dd;
    }

    void control (Voice& v) noexcept
    {
        const auto& p = *pt;
        double x;
        if (v.cnt < 0)
            v.cnt = -v.cnt, x = 0.0;
        else
            v.cnt = p.B, x = 1.0;
        v.envmin = 9;
        for (int i = 0; i < 2; ++i)
        {
            const auto Pv = (double) p.P[(std::size_t) (3 + i)];
            if (p.flag (0x208) == 0)
                v.xy[i] = Pv;
            else
                v.xy[i] = f32 (Pv + f32 (v.xy[i] - Pv) * p.xysmooth);
        }
        v.ixy[0] = ixOf (v.xy[0], v.fcutH), v.ixy[1] = ixOf (v.xy[1], v.fresH);
        for (int k = 0; k < 5; ++k)
        {
            const auto& a = p.arts[(std::size_t) k];
            auto val = xyApply (k, v.stat[k], v.ixy[0], v.ixy[1]);
            if (v.en[k])
            {
                auto& env = v.envs[k];
                if (a.env.enabled)
                {
                    const auto eo = env.step (x);
                    if (a.mode == 1)
                        val = val + eo * a.envamt;
                    else
                        val = val * eo * a.envamt;
                }
                int lst = 9;
                if (v.lfoOn[k])
                {
                    const auto lo = v.lfos[k].lstep (x);
                    if (a.mode == 1)
                        val = val + lo * a.lfoamt;
                    else
                        val = val * (lo * a.lfoamt * 2.0 + 1.0);
                    lst = v.lfos[k].stage;
                }
                v.envmin = std::min ({ v.envmin, env.stage, lst });
            }
            v.out[k] = val;
        }
        const auto vb = f32 (v.fvol * p.master);
        const auto pan = std::min (std::max (v.fpan, -1.0), 1.0);
        if (v.state < 9)
            volpan (pan, f32 (vb * v.out[0]), v.gt);
        else
            v.gt[0] = v.gt[1] = v.gt[2] = v.gt[3] = 0.0f;
        if (v.firstChunk)
            for (int i = 0; i < 4; ++i)
                v.gc[i] = v.gt[i];
        for (int i = 0; i < 4; ++i)
        {
            float s, d, c;
            ramp (v.gc[i], v.gt[i], v.cnt, s, d, c);
            v.gs[i] = s, v.gd[i] = d, v.gc[i] = c;
        }
        if (v.timRelOnEnv && v.envs[0].stage > 2)
            timRelease (v, 0.5);
    }

    // ---- filter (§1.10) ----
    Coef filterCoeffs (const Voice& v) const noexcept
    {
        const auto& p = *pt;
        const auto u = p.fcut + v.out[1];
        auto r = p.fres + v.out[2];
        if (p.fflat)
            r = r * (u + 1.0) * 0.5;
        const int typ = p.ftype, order = p.forder;
        auto s = f32 (44100.0 / p.fs);
        if (p.oversampleFilter)
            s = f32 (s * 0.5);
        const auto ws = s * (2.0 * kPi);
        auto pw = [] (double base, double xx) { return f32 (std::exp (f32 (xx) * std::log (base))); };
        double sn, cs, alpha;
        if (typ == 0 || typ == 3)
        {
            double c = 0.0;
            if (u > 0.0)
            {
                if (typ == 0)
                    c = f32 (pw (1501.0, u) - 1.0) * 0.0006646666442975402;
                else
                    c = std::min (f32 (pw (1501.0, u) - 1.0) * 0.00039993334212340415, 0.6001);
            }
            double Q;
            if (r <= 0.0)
                Q = 0.1;
            else if (r >= 1.0)
                Q = 50.0;
            else
                Q = f32 (pw (2001.0, r) - 1.0) * 0.024950001388788223 + 0.1;
            if (order > 1)
                Q = f32 (std::exp (f32 (1.0 / order) * std::log (f32 (Q))));
            const auto kq = 0.5 / Q;
            auto w = (c + (typ == 0 ? 0.003 : 0.0001)) * ws;
            w = std::min (std::max (w, 0.0), 2.849517146113191);
            sn = std::sin (w), cs = std::cos (w);
            alpha = sn * kq;
        }
        else
        {
            double c = 0.0;
            if (u > 0.0)
                c = std::min (f32 (pw (1501.0, u) - 1.0) * 0.00039993334212340415, 1.0001);
            double bw;
            if (r <= 0.0)
                bw = 0.01;
            else if (r >= 1.0)
                bw = 12.0;
            else
                bw = f32 (pw (101.0, r) - 1.0) * 0.11989999562501907 + 0.01;
            auto f38 = (double) order;
            if (typ == 2)
                f38 = f32 (1.0 / f38);
            f38 = f32 (f38 * (LN2 / 2.0));
            auto w = (c + 0.0001) * ws;
            w = std::min (std::max (w, 0.0), 2.849517146113191);
            sn = std::sin (w), cs = std::cos (w);
            alpha = sn * std::sinh ((bw * f38 * w) / sn);
        }
        const auto n = 1.0 / (alpha + 1.0);
        Coef k;
        k.a1 = n * cs * -2.0;
        k.a2 = n * (1.0 - alpha);
        if (typ == 0)
            k.b1 = n * (1.0 - cs), k.b0 = k.b1 * 0.5, k.b2 = k.b0;
        else if (typ == 1)
            k.b1 = 0.0, k.b0 = n * alpha, k.b2 = n * -alpha;
        else if (typ == 2)
            k.b1 = k.a1, k.b0 = n, k.b2 = n;
        else
            k.b1 = n * -(cs + 1.0), k.b0 = -k.b1 * 0.5, k.b2 = k.b0;
        return k;
    }

    void filter (Voice& v, float* L, float* R, int m) noexcept
    {
        const auto& p = *pt;
        const auto k = filterCoeffs (v);
        float* xl = L;
        float* xr = R;
        int len = m;
        if (p.oversampleFilter)
        {
            for (int i = 0; i < m; ++i)
            {
                v.fL[(std::size_t) (2 * i)] = v.fL[(std::size_t) (2 * i + 1)] = L[i];
                v.fR[(std::size_t) (2 * i)] = v.fR[(std::size_t) (2 * i + 1)] = R[i];
            }
            xl = v.fL.data(), xr = v.fR.data(), len = 2 * m;
        }
        for (auto& st : v.fst)
            biquadRun (k, st.data(), xl, xr, len);
        if (p.oversampleFilter)
            for (int i = 0; i < m; ++i)
            {
                L[i] = xl[2 * i] * 0.5f + xl[2 * i + 1] * 0.5f;
                R[i] = xr[2 * i] * 0.5f + xr[2 * i + 1] * 0.5f;
            }
    }

    // ---- voice render (ogmodel.Voice.render) ----
    void renderVoice (Voice& v, float* bl, float* br, int n) noexcept
    {
        const auto& p = *pt;
        int o = 0;
        float* gsL[4] { rampBuf.data(), rampBuf.data() + p.B, rampBuf.data() + 2 * p.B, rampBuf.data() + 3 * p.B };
        while (o < n)
        {
            if (v.cnt < 1)
                control (v);
            const auto c = f32 ((v.FP + (double) p.masterCents) + v.out[3] * 1200.0);
            if (c != v.cents)
            {
                v.cents = c;
                v.speed = p.speedK / p.fs * std::exp (c * (1.0 / 1200.0) * LN2);
            }
            const auto up = f32 (p.P[15] * v.out[4]);
            if (up != v.upitch)
            {
                v.upitch = up;
                const auto S = f32 (f32 (std::exp (f32 (up * 0.00390625) * std::log (11.0)) - 1.0) * 5.0);
                for (int k = 0; k < v.nsub; ++k)
                {
                    auto& sb = v.subs[(std::size_t) k];
                    sb.ratio = f32 (std::exp (f32 (sb.det * f32 (S * f32 (LN2 / 1200.0)))));
                }
            }
            const int m = std::min (v.cnt, n - o);
            float* xL = v.xL.data();
            float* xR = v.xR.data();
            timbreRender (v, xL, xR, m);
            if (v.silence < 1 && v.state < 9)
                v.state = 9;
            if (v.declick > 0)
            {
                v.declick -= m;
                const int k = v.declick >= 0 ? m : m + v.declick;
                auto g = v.dcG, L = v.dcL, R = v.dcR;
                for (int i = 0; i < k; ++i)
                {
                    L = L + g * (xL[i] - L);
                    xL[i] = L;
                    R = R + g * (xR[i] - R);
                    xR[i] = R;
                    g = g + v.dcD;
                }
                v.dcL = L, v.dcR = R;
                v.dcG = v.dcG + v.dcD * (float) k;
            }
            if (p.forder > 0)
                filter (v, xL, xR, m);
            for (int i = 0; i < 4; ++i)
            {
                auto g = v.gs[i];
                accRamp (g, v.gd[i], m, gsL[i]);
            }
            for (int i = 0; i < m; ++i)
            {
                const float Lv = xL[i] * gsL[0][i] + xR[i] * gsL[2][i];
                const float Rv = xR[i] * gsL[1][i] + xL[i] * gsL[3][i];
                bl[o + i] = bl[o + i] + Lv;
                br[o + i] = br[o + i] + Rv;
            }
            for (int i = 0; i < 4; ++i)
                v.gs[i] = v.gs[i] + v.gd[i] * (float) m;
            v.firstChunk = false;
            v.cnt -= m;
            o += m;
        }
        if (v.state >= 9 || v.envmin >= 9)
            if (v.state < 10 && v.gs[0] == 0.0f && v.gs[1] == 0.0f && v.gs[2] == 0.0f && v.gs[3] == 0.0f && v.gd[0] == 0.0f
                && v.gd[1] == 0.0f && v.gd[2] == 0.0f && v.gd[3] == 0.0f)
                v.state = 10;
        if (v.state >= 10)
            v.dead = true;
    }

    // ---- timbre (ogmodel.Timbre) ----
    void shaped (const Voice& v, long long seed, float* out) noexcept
    {
        const auto& p = *pt;
        const int M = p.M;
        rand32f (sU.data(), M, 0.0f, 1.0f, (std::uint32_t) seed);
        const float* u = sU.data();
        if (v.rot > 0)
        {
            for (int i = 0; i < M; ++i)
                sV[(std::size_t) i] = u[(i - v.rot + M) % M];
            u = sV.data();
        }
        const auto pp = v.p;
        if (pp == 0.0)
        {
            std::memmove (out, u, sizeof (float) * (std::size_t) M);
            return;
        }
        if (pp <= 0.0)
        {
            const float q = (float) (1.0 - pp);
            const float lq = (float) std::log ((double) q);
            for (int i = 0; i < M; ++i)
            {
                const float w = (1.0f - u[i]) * lq;
                out[i] = q - (float) std::exp ((double) w);
            }
            return;
        }
        const float lp = (float) std::log ((double) (float) (pp + 1.0));
        for (int i = 0; i < M; ++i)
        {
            const float w = u[i] * lp;
            out[i] = (float) std::exp ((double) w) + -1.0f;
        }
    }

    void setup (Voice& v, int mode, double argPre, double argTime) noexcept
    {
        const auto& p = *pt;
        const int M = p.M, hop = p.hop;
        const auto T0 = (p.fs * 0.025) / hop;
        const auto Tm = argTime / hop - T0;
        const auto T = Tm > 0.0 ? Tm : 0.0;
        v.tstate = argTime < 0.0 ? 1 : 0;
        if (v.alt && v.tstate == 0)
            v.c = T > 0.0 ? 1.0 - std::exp (std::log (0.01) / T) : 1.0;
        const float ln001 = (float) std::log (0.01);
        if (mode == 0)
        {
            const auto Pp = std::max (argPre, 0.001);
            const auto invP = 1.0 / Pp;
            const auto lp = v.p == 0.0 ? 1.0 : f32 (1.0 / std::fabs (v.p));
            const auto m = v.tmorph;
            float* B = v.Bw.data();
            bool haveB = false;
            if (m != 0.0)
            {
                shaped (v, v.seed2, B);
                const float sc = (float) (m * lp);
                for (int i = 0; i < M; ++i)
                    B[i] = B[i] * sc;
                haveB = true;
            }
            if (m < 1.0)
            {
                if (v.seed1 < 0)
                {
                    const float a = (float) (1.0 - m);
                    if (! haveB)
                        std::fill (B, B + M, a);
                    else
                        for (int i = 0; i < M; ++i)
                            B[i] = B[i] + a;
                }
                else
                {
                    shaped (v, v.seed1, sD.data());
                    const float sc = (float) ((1.0 - m) * lp);
                    for (int i = 0; i < M; ++i)
                    {
                        const float t = sD[(std::size_t) i] * sc;
                        B[i] = haveB ? B[i] + t : t;
                    }
                }
                haveB = true;
            }
            if (! (v.tmorph == 0.0 && v.seed1 < 0))
                for (int i = 0; i < M; ++i)
                    B[i] = std::max (B[i], 0.0f);
            if (p.hasForced)
                for (int i = 0; i < M; ++i)
                {
                    const auto Fm = p.mForced[(std::size_t) i];
                    B[i] = (B[i] * Fm + 1.0f) - Fm;
                }
            // D (in sD), hold factors
            const float fInvP = (float) invP;
            float* D = sD.data();
            for (int i = 0; i < M; ++i)
            {
                const float x4 = B[i] * fInvP + 0.001f;
                D[i] = (float) std::exp ((double) (ln001 / x4));
            }
            if (! v.alt && v.tstate == 0)
            {
                const float fT = (float) T, fT0 = (float) T0;
                const int L = (int) p.mDec.size();
                for (int i = 0; i < M; ++i)
                {
                    const float Ed = p.mDec[(std::size_t) std::clamp (i - v.shift, 0, L - 1)];
                    const float xx = (B[i] * Ed) * fT + fT0;
                    v.fac[(std::size_t) i] = (float) std::exp ((double) (ln001 / xx));
                }
            }
            else
                std::fill (v.fac.begin(), v.fac.end(), 1.0f);
            {
                // rms over M in double (np.sum of float32 squares with a float64 accumulator)
                for (int i = 0; i < M; ++i)
                    sU[(std::size_t) i] = D[i] * D[i];
                const auto sum = sU.empty() ? 0.0 : (double) sU[0] + pairwiseSum<double> (sU.data() + 1, (std::size_t) M - 1);
                const auto rms = f32 (std::sqrt (sum / M));
                if (rms > 0.0)
                {
                    const float g = (float) (p.gnorm / rms);
                    for (int i = 0; i < M; ++i)
                        D[i] = D[i] * g;
                }
            }
            auto band = (long long) ((M * (double) (1 << p.qt) * 0.907029478) / v.speed);
            band = std::min (std::max (band, 0LL), (long long) M);
            v.band = (int) band;
            int first, last;
            if (v.shift == 0)
                first = std::clamp (p.first, 0, M), last = std::min (v.band, p.last);
            else
                first = 0, last = v.band;
            v.tfirst = first, v.tlast = last;
            const int f0 = v.alt ? 0 : first, ec = v.alt ? v.band : last;
            std::fill (v.eRe.begin(), v.eRe.end(), 0.0f);
            std::fill (v.eIm.begin(), v.eIm.end(), 0.0f);
            const int Li = (int) p.mImp.size();
            for (int i = f0; i < std::max (f0, ec); ++i)
            {
                const float amp = D[i] * p.mImp[(std::size_t) std::clamp (i - v.shift, 0, Li - 1)];
                v.eRe[(std::size_t) i] = p.phRe[(std::size_t) i] * amp;
                v.eIm[(std::size_t) i] = p.phIm[(std::size_t) i] * amp;
            }
            if (v.alt)
            {
                std::fill (v.tgRe.begin(), v.tgRe.end(), 0.0f);
                std::fill (v.tgIm.begin(), v.tgIm.end(), 0.0f);
                const int Ld = (int) p.mDec.size();
                for (int i = first; i < std::max (first, last); ++i)
                {
                    const float yv = D[i] * p.mDec[(std::size_t) std::clamp (i - v.shift, 0, Ld - 1)];
                    v.tgRe[(std::size_t) i] = p.phRe[(std::size_t) i] * yv;
                    v.tgIm[(std::size_t) i] = p.phIm[(std::size_t) i] * yv;
                }
            }
            for (int i = first; i < last; ++i)
            {
                if (std::fabs (v.eRe[(std::size_t) i]) < (float) tiny24)
                    v.eRe[(std::size_t) i] = 0.0f;
                if (std::fabs (v.eIm[(std::size_t) i]) < (float) tiny24)
                    v.eIm[(std::size_t) i] = 0.0f;
            }
            for (auto& f : v.fac)
                if (std::fabs (f) < (float) tiny24)
                    f = 0.0f;
        }
        else
        {
            const float fT = (float) T, fT0 = (float) T0;
            const int L = (int) p.mRel.size();
            for (int i = 0; i < M; ++i)
            {
                const float Er = p.mRel[(std::size_t) std::clamp (i - v.shift, 0, L - 1)];
                const float xx = (v.Bw[(std::size_t) i] * Er) * fT + fT0;
                auto f = (float) std::exp ((double) (ln001 / xx));
                v.fac[(std::size_t) i] = std::fabs (f) < (float) tiny24 ? 0.0f : f;
            }
            if (v.alt)
            {
                v.alt = false;
                v.tfirst = 0, v.tlast = v.band;
            }
        }
    }

    void advance (Voice& v) noexcept
    {
        if (v.tstate != 0)
            return;
        const int a = v.tfirst, b = v.tlast;
        if (b <= a)
            return;
        if (! v.alt)
            for (int i = a; i < b; ++i)
            {
                v.eRe[(std::size_t) i] = v.eRe[(std::size_t) i] * v.fac[(std::size_t) i];
                v.eIm[(std::size_t) i] = v.eIm[(std::size_t) i] * v.fac[(std::size_t) i];
            }
        else
        {
            const float c = (float) v.c;
            for (int i = a; i < b; ++i)
            {
                const float dr = (v.tgRe[(std::size_t) i] - v.eRe[(std::size_t) i]) * c;
                const float di = (v.tgIm[(std::size_t) i] - v.eIm[(std::size_t) i]) * c;
                v.eRe[(std::size_t) i] = v.eRe[(std::size_t) i] + dr;
                v.eIm[(std::size_t) i] = v.eIm[(std::size_t) i] + di;
            }
        }
    }

    void ifftInto (Voice& v, float* dst) noexcept
    {
        const auto& p = *pt;
        p.fft.inverse (v.eRe.data(), v.eIm.data(), p.M, dst, zr.data());
        std::memcpy (dst + p.N, dst, 64 * sizeof (float));
    }

    // FUN_00843300: new table from the current spectrum, then advance the spectrum
    void frame (Voice& v) noexcept
    {
        if (v.tstate < 0)
            return;
        std::swap (v.tNew, v.tOld);
        ifftInto (v, v.tNew);
        advance (v);
    }

    void timbreStart (Voice& v) noexcept
    {
        const auto& p = *pt;
        v.started = true;
        setup (v, 0, v.tpre, v.decSamples);
        frame (v);
        frame (v);
        // j0 = first index of the smallest squared sample (float32)
        int j0 = 0;
        float best = v.tNew[0] * v.tNew[0];
        for (int i = 1; i < p.N; ++i)
        {
            const float sq = v.tNew[i] * v.tNew[i];
            if (sq < best)
                best = sq, j0 = i;
        }
        for (auto& j : j0Map)
            if (j.first == &v)
            {
                j0 = (int) (((long long) j0 + j.second) % p.N + p.N) % p.N;
                j.first = nullptr;
            }
        for (int k = 0; k < v.nsub; ++k)
            v.subs[(std::size_t) k].pos = v.subs[(std::size_t) k].pos * p.N + (double) (j0 + (2 * p.N - 32));
        resetGains (v);
        if (v.tstate > 0)
            v.tstate = -1;
    }

    void resetGains (Voice& v) const noexcept
    {
        const auto h = (float) (1.0 / pt->hop);
        for (int k = 0; k < v.nsub; ++k)
        {
            auto& s = v.subs[(std::size_t) k];
            s.g[0] = 0.0f, s.g[1] = 0.0f, s.g[2] = (float) s.L, s.g[3] = (float) s.R;
            s.d[0] = (float) s.L * h, s.d[1] = (float) s.R * h;
            s.d[2] = -s.d[0], s.d[3] = -s.d[1];
        }
    }

    void timbreRelease (Voice& v, double relSamples) noexcept
    {
        const auto& p = *pt;
        if (! v.started)
            timbreStart (v);
        setup (v, 1, 0.0, relSamples);
        v.tstate = 0;
        if (v.cntTotal != 0)
        {
            const float fr = (float) v.inFrame / (float) p.hop;
            const float fo = 1.0f - fr;
            for (int i = 0; i < p.N; ++i)
                v.tNew[i] = v.tNew[i] * fr + v.tOld[i] * fo;
            std::memcpy (v.tNew + p.N, v.tNew, 64 * sizeof (float));
            frame (v);
            v.frameEnd = v.cntTotal + p.hop;
            v.inFrame = 0;
            resetGains (v);
        }
    }

    void timbreRender (Voice& v, float* outL, float* outR, int n) noexcept
    {
        const auto& p = *pt;
        const int N = p.N;
        int o = 0;
        float* gv[4] { rampBuf.data() + 4 * p.B, rampBuf.data() + 5 * p.B, rampBuf.data() + 6 * p.B, rampBuf.data() + 7 * p.B };
        while (o < n)
        {
            const int m = (int) std::min ((long long) (n - o), v.frameEnd - v.cntTotal);
            v.inFrame += m;
            if (! v.started)
                timbreStart (v);
            const float* tn = v.tNew;
            const float* to = v.tOld;
            for (int j = 0; j < v.nsub; ++j)
            {
                auto& s = v.subs[(std::size_t) j];
                const auto step = v.speed * s.ratio;
                const auto pos0 = s.pos;
                s.pos = s.pos + step * m;
                for (int i = 0; i < 4; ++i)
                    accRamp (s.g[i], s.d[i], m, gv[i]);
                float* dL = outL + o;
                float* dR = outR + o;
                if (p.interp == 1)
                {
                    for (int i = 0; i < m; ++i)
                    {
                        const auto pos = pos0 + step * i;
                        const auto ip = (long long) pos;
                        const float fr = (float) (pos - (double) ip);
                        const auto ix = (std::size_t) ((ip & (N - 1)) + 32);
                        const float a0 = tn[ix], a1 = tn[ix + 1], b0 = to[ix], b1 = to[ix + 1];
                        const float vn = (a1 - a0) * fr + a0, vo = (b1 - b0) * fr + b0;
                        const float L = vn * gv[0][i] + vo * gv[2][i];
                        const float R = vn * gv[1][i] + vo * gv[3][i];
                        if (j == 0)
                            dL[i] = L, dR[i] = R;
                        else
                            dL[i] = dL[i] + L, dR[i] = dR[i] + R;
                    }
                }
                else if (p.interp == 6)
                {
                    sincRead (v.sinc[(std::size_t) j], tn, to, pos0, step, m, N, gv, dL, dR, j == 0);
                }
                else
                {
                    for (int i = 0; i < m; ++i)
                    {
                        const auto pos = pos0 + step * i;
                        const auto ip = (long long) pos;
                        const auto xf = (double) (float) (pos - (double) ip);
                        const auto ix = (std::size_t) ((ip & (N - 1)) + 32);
                        const float vn = (float) hermite6 (tn[ix - 2], tn[ix - 1], tn[ix], tn[ix + 1], tn[ix + 2], tn[ix + 3], xf);
                        const float vo = (float) hermite6 (to[ix - 2], to[ix - 1], to[ix], to[ix + 1], to[ix + 2], to[ix + 3], xf);
                        const float L = vn * gv[0][i] + vo * gv[2][i];
                        const float R = vn * gv[1][i] + vo * gv[3][i];
                        if (j == 0)
                            dL[i] = L, dR[i] = R;
                        else
                            dL[i] = dL[i] + L, dR[i] = dR[i] + R;
                    }
                }
            }
            o += m;
            v.cntTotal += m;
            if (v.cntTotal >= v.frameEnd)
            {
                frame (v);
                v.frameEnd += p.hop;
                v.inFrame = 0;
                resetGains (v);
            }
        }
        bool silent = true;
        for (int i = 0; i < n && silent; ++i)
            silent = std::fabs ((double) outL[i]) <= v.thr && std::fabs ((double) outR[i]) <= v.thr;
        if (silent)
            v.silence -= n;
        else
            v.silence = rnd (p.fs * 0.1);
    }

    std::vector<float> sincN, sincO;

    void sincRead (SincTables& sk, const float* tn, const float* to, double pos0, double step, int m, int N, float* const* gv,
                   float* dL, float* dR, bool firstSub) noexcept
    {
        constexpr int points = 32;
        const auto s = std::fabs (step);
        const auto b = kPi * (s > 1.0 ? 1.0 / s : 1.0);
        if (b != sk.b)
        {
            // cos/sin(b·k) by rotation from k = 0 (error ≲ 10⁻¹⁵, far below the float output)
            double c1 = 1.0, s1 = 0.0;
            if (b != kPi)
                sinCos (b, s1, c1);
            double cp = 1.0, sp = 0.0, cm = 1.0, sm = 0.0;
            sk.cb[31] = 1.0, sk.sb[31] = 0.0;
            for (int k = 1; k <= points; ++k)
            {
                const auto ncp = cp * c1 - sp * s1, nsp = sp * c1 + cp * s1;
                cp = ncp, sp = nsp;
                sk.cb[31 + k] = cp, sk.sb[31 + k] = sp;
                if (k < points)
                {
                    const auto ncm = cm * c1 + sm * s1, nsm = sm * c1 - cm * s1;
                    cm = ncm, sm = nsm;
                    sk.cb[31 - k] = cm, sk.sb[31 - k] = sm;
                }
            }
            for (int k = -points + 1; k <= points; ++k)
            {
                const int t = k + points - 1;
                if (sk.b < 0.0)
                {
                    sk.kk[t] = (double) k;
                    sk.cA[t] = std::cos (kPi / points * k), sk.sA[t] = std::sin (kPi / points * k);
                }
                if (b == kPi)
                    sk.cb[t] = (k & 1) ? -1.0 : 1.0, sk.sb[t] = 0.0;
            }
            sk.b = b;
        }
        float* vn = sincN.data();
        float* vo = sincO.data();
        sincChunk (sk, tn, to, pos0, step, m, N, vn, vo);
        for (int i = 0; i < m; ++i)
        {
            const float L = vn[i] * gv[0][i] + vo[i] * gv[2][i];
            const float R = vn[i] * gv[1][i] + vo[i] * gv[3][i];
            if (firstSub)
                dL[i] = L, dR[i] = R;
            else
                dL[i] = dL[i] + L, dR[i] = dR[i] + R;
        }
    }

    // ---- chorus (§1.16; our ogfx_chorus.py) ----
    static float trunc32 (double x) noexcept
    {
        auto f = (float) x;
        if (std::fabs ((double) f) > std::fabs (x))
            f = std::nextafter (f, 0.0f);
        return f;
    }

    void chorus (int n, bool active) noexcept
    {
        const auto& p = *pt;
        if (! active)
        {
            for (std::size_t i = 0; i < chorusPh.size(); ++i)
                chorusPh[i] = chorusPh[i] + p.taps[i].inc * (std::uint32_t) n;
            return;
        }
        const int base = ringPos;
        for (int i = 0; i < n; ++i)
        {
            ringL[(std::size_t) ((base + i) & ringMask)] = bufL[(std::size_t) i];
            ringR[(std::size_t) ((base + i) & ringMask)] = bufR[(std::size_t) i];
        }
        ringPos = (ringPos + n) & ringMask;
        for (int i = 0; i < n; ++i)
        {
            float aL = 0.0f, aR = 0.0f;
            for (std::size_t t = 0; t < p.taps.size(); ++t)
            {
                const auto& tp = p.taps[t];
                const auto ph = (std::uint32_t) (chorusPh[t] + tp.inc * (std::uint32_t) i);
                const auto phs = (double) (std::int32_t) ph;
                const auto x = std::fabs (phs * tp.s8) - tp.s10;
                const auto d = (tp.s9 - std::fabs (x)) * x + tp.centre;
                const auto k = (long long) d;
                const auto fr = d - (double) k;
                const auto& src = tp.ch == 0 ? ringL : ringR;
                const auto x0 = (double) src[(std::size_t) ((base + i - k) & ringMask)];
                const auto x1 = (double) src[(std::size_t) ((base + i - k - 1) & ringMask)];
                const auto val = x0 + (x1 - x0) * fr;
                if (t == 0)
                    aL = trunc32 (val * (double) tp.gL), aR = trunc32 (val * (double) tp.gR);
                else
                    aL = trunc32 (val * (double) tp.gL + (double) aL), aR = trunc32 (val * (double) tp.gR + (double) aR);
            }
            tmpL[(std::size_t) i] = aL, tmpR[(std::size_t) i] = aR;
        }
        for (std::size_t t = 0; t < chorusPh.size(); ++t)
            chorusPh[t] = chorusPh[t] + p.taps[t].inc * (std::uint32_t) n;
        for (int i = 0; i < n; ++i)
        {
            auto l = bufL[(std::size_t) i], r = bufR[(std::size_t) i];
            if (p.chorusDry != 1.0f)
                l = l * p.chorusDry, r = r * p.chorusDry;
            bufL[(std::size_t) i] = l + tmpL[(std::size_t) i];
            bufR[(std::size_t) i] = r + tmpR[(std::size_t) i];
        }
    }

    // ---- delay (§1.17; our ogfx_delay.py) ----
    void delay (int n, bool active) noexcept
    {
        const auto& p = *pt;
        const int S = p.delaySize;
        auto line = [&] (int i, int ch) -> float& { return dline[2 * (std::size_t) i + (std::size_t) ch]; };
        if (active)
            for (int i = 0; i < n; ++i)
            {
                auto& a = line ((dw + p.delayOffL + i) % S, 0);
                a = a + bufL[(std::size_t) i];
                auto& b = line ((dw + p.delayOffR + i) % S, 1);
                b = b + bufR[(std::size_t) i];
            }
        // feedback ramp (FUN_0062b400)
        const auto g0 = fbCur;
        double inc = 0.0;
        {
            const auto d = f32 (p.delayFb - g0);
            if (d != 0.0)
            {
                fbCur = p.delayFb;
                inc = f32 (d / (double) (float) n);
                const auto lim = p.rateLimit;
                if (std::fabs (inc) > lim)
                {
                    inc = std::copysign (lim, inc);
                    const auto cur = f32 (g0 + f32 (inc * (double) (float) n));
                    fbCur = cur;
                    if (std::fabs (cur) < tiny24)
                    {
                        fbCur = 0.0;
                        inc = f32 (-g0 / (double) (float) n);
                    }
                }
            }
        }
        for (int i = 0; i < n; ++i)
        {
            const auto g = g0 + inc * i;
            auto& rl = line (dr, 0);
            auto& rr = line (dr, 1);
            const auto L = (double) rl, R = (double) rr;
            rl = 0.0f, rr = 0.0f;
            double oL, oR, fLv, fRv;
            if (p.delayMode == 0)
                oL = L, oR = R, fLv = L, fRv = R;
            else if (p.delayMode == 1)
                oL = R, oR = L, fLv = L, fRv = R;
            else
                oL = R, oR = L, fLv = R, fRv = L;
            tmpL[(std::size_t) i] = (float) oL, tmpR[(std::size_t) i] = (float) oR;
            auto& wl = line (dw, 0);
            auto& wr = line (dw, 1);
            auto vl = (float) (fLv * g + (double) wl);
            auto vr = (float) (fRv * g + (double) wr);
            wl = std::fabs ((double) vl) < tiny24 ? 0.0f : vl;
            wr = std::fabs ((double) vr) < tiny24 ? 0.0f : vr;
            dr = dr + 1 == S ? 0 : dr + 1;
            dw = dw + 1 == S ? 0 : dw + 1;
        }
        const auto wet = (float) p.delayWet;
        for (int i = 0; i < n; ++i)
        {
            if (wet == 1.0f)
                bufL[(std::size_t) i] = bufL[(std::size_t) i] + tmpL[(std::size_t) i], bufR[(std::size_t) i] = bufR[(std::size_t) i] + tmpR[(std::size_t) i];
            else
                bufL[(std::size_t) i] = bufL[(std::size_t) i] + tmpL[(std::size_t) i] * wet,
                bufR[(std::size_t) i] = bufR[(std::size_t) i] + tmpR[(std::size_t) i] * wet;
        }
    }

    // ---- 2:1 decimator of qualities 2–4 (§1.19; our ogos.Decimator, order 1) ----
    static bool quietWord (std::uint32_t w) noexcept { return (std::uint32_t) (w << 1) <= (std::uint32_t) (0x358637bdu * 2u); }

    void decimate (int m) noexcept
    {
        bool quiet = true;
        for (int i = 0; i < m && quiet; ++i)
        {
            std::uint32_t a, b;
            std::memcpy (&a, &bufL[(std::size_t) i], 4);
            std::memcpy (&b, &bufR[(std::size_t) i], 4);
            quiet = quietWord (a) && quietWord (b);
        }
        // state slots 0…5 (allpass outputs) of both channels, tested as 32-bit words
        for (int s = 0; s < 6 && quiet; ++s)
            for (int ch = 0; ch < 2 && quiet; ++ch)
            {
                std::uint32_t w[2];
                std::memcpy (w, &decSt[(std::size_t) (s * 2 + ch)], 8);
                quiet = quietWord (w[0]) && quietWord (w[1]);
            }
        const int h = m / 2;
        if (quiet)
            return; // the first half of the block is passed on as it is
        static constexpr double A0 = 0.1473771136010466, A1 = 0.48246854276970014, A2 = 0.8830050257693731;
        static constexpr double B0 = 0.039151597734460045, B1 = 0.3026468483284934, B2 = 0.6746159185469639;
        for (int ch = 0; ch < 2; ++ch)
        {
            float* x = ch == 0 ? bufL.data() : bufR.data();
            double S[12];
            for (int k = 0; k < 12; ++k)
                S[k] = decSt[(std::size_t) (k * 2 + ch)];
            for (int i = 0; i < h; ++i)
            {
                const auto e = (double) x[2 * i], od = (double) x[2 * i + 1];
                auto t = S[6] + (e - S[0]) * A0;
                S[6] = e, S[0] = t;
                t = S[9] + (od - S[3]) * B0;
                S[9] = od, S[3] = t;
                t = S[7] + (S[0] - S[1]) * A1;
                S[7] = S[0], S[1] = t;
                t = S[10] + (S[3] - S[4]) * B1;
                S[10] = S[3], S[4] = t;
                t = S[8] + (S[1] - S[2]) * A2;
                S[8] = S[1], S[2] = t;
                t = S[11] + (S[4] - S[5]) * B2;
                S[11] = S[4], S[5] = t;
                x[i] = (float) ((S[5] + S[2]) * 0.5);
            }
            for (int k = 0; k < 6; ++k)
                if (std::fabs (S[k]) < tiny24)
                    S[k] = 0.0;
            for (int k = 0; k < 12; ++k)
                decSt[(std::size_t) (k * 2 + ch)] = S[k];
        }
    }

    double lastPitch = 0.0;
    bool hasLastPitch = false;
};

// ---------------------------------------------------------------------------------------------------------------
// LYNX front-end
//
// Levels (§1.12/§1.13, REA_rodada28 §4.1): FL hands Ogun Final.Vol = the channel volume and Final.Pan = note pan +
// channel pan, and Ogun applies Final.Vol × master × volume articulator through its own pan matrix (no host
// ComputeLRVol). LYNX's channel gains are gl = √2·V·cos((1 + p)π/4), gr = √2·V·sin(…), so V = √((gl² + gr²)/2) and p
// are recovered exactly and Ogun's law is applied once. Final.Pitch = 100·(key − 60) + channel pitch + note cents;
// velocity = LYNX velocity (FL velocity / 128); release velocity 0.5; Mod X/Y (Final.FCut/FRes) 0.
// Seed 2 "Random": the per-note counter starts at 0 for every instance (FL: from the RTL random seed).
// Autogun (REA_rodada57): the same engine on Autogun's state variant, master × 0.25, then the Magic Dust stage, whose
// preset SG.dat is read from the user's installation (ChannelDef::samplePath); without it the stage is skipped.

std::vector<std::uint8_t> readInstallFile (std::string path)
{
    if (path.empty())
        return {};
    std::ifstream in (path, std::ios::binary);
    if (! in)
    {
        std::replace (path.begin(), path.end(), '\\', '/');
        in.open (path, std::ios::binary);
    }
    if (! in)
        return {};
    return std::vector<std::uint8_t> ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
}

std::shared_ptr<const hlfx::MaximusPreset> sgPresetFrom (const std::vector<std::uint8_t>& sgState)
{
    if (sgState.empty())
        return nullptr;
    try
    {
        return hlfx::makeMaximusPreset (sgState, hlfx::MaximusBuild::shared);
    }
    catch (...)
    {
        return nullptr;
    }
}

class OgunInstrument final : public Instrument
{
public:
    explicit OgunInstrument (bool isAutogun = false)
        : autogun (isAutogun)
    {
    }

    ~OgunInstrument() override
    {
        delete incoming.exchange (nullptr);
        delete retired.exchange (nullptr);
    }

    void prepare (double sr) noexcept override
    {
        take();
        rate.store (sr);
        const bool render = offlineRendering();
        {
            const std::lock_guard<std::mutex> lock (buildMutex);
            if (! loadedState.empty() && (sr != loadedRate || render != loadedMode))
                rebuild (sr, render);
        }
        take();
    }

    void configure (const ChannelDef& channel, double bpm) noexcept override
    {
        delete retired.exchange (nullptr);
        const auto b = bpm > 0.0 ? bpm : 120.0;
        channelCents.store (channel.sampler.pitch * 100.0f);
        mono.store (channel.synth.mono);
        const bool render = offlineRendering();
        const std::lock_guard<std::mutex> lock (buildMutex);
        const bool sgChanged = autogun && channel.samplePath != loadedSgPath;
        if (channel.pluginState == loadedState && loadedRate == rate.load() && loadedMode == render && b == loadedBpm && ! sgChanged)
            return;
        if (sgChanged)
        {
            loadedSgPath = channel.samplePath;
            sgPreset = sgPresetFrom (readInstallFile (loadedSgPath));
        }
        if (channel.pluginState != loadedState)
        {
            loadedState = channel.pluginState;
            try
            {
                parsed = parseState (loadedState, autogun);
            }
            catch (...)
            {
                parsed.reset();
            }
        }
        loadedBpm = b;
        rebuild (rate.load(), render);
    }

    void setNextNoteExpression (float pan, float cents, bool) noexcept override
    {
        nextPan = pan;
        nextCents = cents;
    }

    void noteOn (int key, float velocity) noexcept override
    {
        take();
        if (engine == nullptr)
            return;
        engine->maxpoly = mono.load() ? 1 : 0;
        const auto pofs = (double) channelCents.load() + (double) nextCents;
        engine->noteOn (key, std::clamp ((double) velocity, 0.0, 1.0), channelVol, (double) nextPan + channelPan, pofs, 0.0, 0.0,
                        0.5, 0, key, (double) nextPan);
        nextPan = 0.0f, nextCents = 0.0f;
    }

    void slideTo (int key, float cents, float samples) noexcept override
    {
        take();
        if (engine != nullptr)
            engine->slideTo (100.0 * std::clamp (key, 0, 127) - 6000.0 + (double) channelCents.load() + (double) cents, samples);
    }

    void noteOff (int key) noexcept override
    {
        take();
        if (engine != nullptr)
            engine->releaseKey (key);
    }

    void allNotesOff() noexcept override
    {
        take();
        if (engine != nullptr)
            engine->killAll();
    }

    bool idle() const noexcept override { return engine == nullptr || engine->quiet(); }

    void render (float* left, float* right, int n, float gl, float gr) noexcept override
    {
        take();
        if (engine == nullptr)
            return;
        const auto vol = std::sqrt (((double) gl * gl + (double) gr * gr) * 0.5);
        auto pan = vol > 0.0 ? std::atan2 ((double) gr, (double) gl) * (4.0 / kPi) - 1.0 : 0.0;
        if (std::fabs (pan) < 1e-9)
            pan = 0.0; // (equal gains: exactly centre, as FL's Final.Pan)
        if (vol != channelVol || pan != channelPan)
        {
            channelVol = vol, channelPan = pan;
            engine->setLevels (vol, pan);
        }
        for (int done = 0; done < n; done += Engine::maxHostBlock)
        {
            const auto m = std::min (Engine::maxHostBlock, n - done);
            if (engine->quiet())
                engine->skip (m); // nothing sounds: only the plugin-wide phases run
            else
                engine->render (left + done, right + done, m);
        }
    }

private:
    void rebuild (double sr, bool render) noexcept
    {
        loadedRate = sr;
        loadedMode = render;
        if (! parsed)
        {
            delete incoming.exchange (nullptr);
            clearEngine.store (true);
            return;
        }
        try
        {
            auto patch = buildPatch (*parsed, sr, render ? 4 : -1, loadedBpm, Engine::maxHostBlock);
            if (autogun)
                patch->sg = sgPreset, patch->sgMix = parsed->P[63];
            auto e = std::make_unique<Engine> (std::move (patch));
            delete incoming.exchange (e.release());
        }
        catch (...)
        {
        }
    }

    void take() noexcept
    {
        if (clearEngine.exchange (false))
        {
            delete retired.exchange (engine.release());
        }
        if (auto* e = incoming.exchange (nullptr))
        {
            delete retired.exchange (engine.release());
            engine.reset (e);
            engine->setLevels (channelVol, channelPan);
        }
    }

    std::unique_ptr<Engine> engine;
    std::atomic<Engine*> incoming { nullptr }, retired { nullptr };
    std::atomic<bool> clearEngine { false };
    std::mutex buildMutex; // configure (UI thread) and prepare build engines; the audio thread only takes them
    std::atomic<double> rate { 44100.0 };
    std::atomic<float> channelCents { 0.0f };
    std::atomic<bool> mono { false };
    const bool autogun;
    std::vector<std::uint8_t> loadedState;
    std::string loadedSgPath;
    std::shared_ptr<const hlfx::MaximusPreset> sgPreset;
    std::optional<StateData> parsed;
    double loadedRate = 0.0, loadedBpm = 120.0;
    bool loadedMode = false;
    double channelVol = 1.0, channelPan = 0.0;
    float nextPan = 0.0f, nextCents = 0.0f;
};

} // namespace

bool ogunStateUsable (const std::vector<std::uint8_t>& state)
{
    try
    {
        return parseState (state).has_value();
    }
    catch (...)
    {
        return false;
    }
}

std::optional<int> ogunParam (const std::vector<std::uint8_t>& state, int index)
{
    if (index < 0 || index >= 64 || state.size() < 8)
        return std::nullopt;
    const auto body = inflateZlib (state.data() + 4, state.size() - 4, false, 16u << 20);
    if (! body || body->size() < 256)
        return std::nullopt;
    std::int32_t v;
    std::memcpy (&v, body->data() + 4 * index, 4);
    return v;
}

bool ogunSetParam (std::vector<std::uint8_t>& state, int index, int value)
{
    if (index < 0 || index >= 64 || state.size() < 8)
        return false;
    auto body = inflateZlib (state.data() + 4, state.size() - 4, false, 16u << 20);
    if (! body || body->size() < 256)
        return false;
    const std::int32_t v = value;
    std::memcpy (body->data() + 4 * index, &v, 4);
    const auto z = zlibStored (*body);
    state.resize (4);
    state.insert (state.end(), z.begin(), z.end());
    return true;
}

bool autogunStateUsable (const std::vector<std::uint8_t>& state)
{
    try
    {
        return parseState (state, true).has_value();
    }
    catch (...)
    {
        return false;
    }
}

std::vector<std::string> ogunFallbacks (const std::vector<std::uint8_t>& state, bool autogun)
{
    const auto S = parseState (state, autogun);
    if (! S)
        return { "state not usable" };
    auto f = buildPatch (*S, 44100.0, -1, 140.0, 512)->fallbacks;
    if (S->P[11] < 1 && S->P[9] != 0)
        f.push_back ("Seed 2 random (fixed per-instance counter)");
    return f;
}

std::unique_ptr<Instrument> makeOgunInstrument() { return std::make_unique<OgunInstrument>(); }
std::unique_ptr<Instrument> makeAutogunInstrument() { return std::make_unique<OgunInstrument> (true); }

namespace ogun
{
std::vector<float> renderReference (const std::vector<std::uint8_t>& state, const std::vector<RefNote>& notes, double sampleRate,
                                    int length, const RefOptions& opt)
{
    const auto S = parseState (state, opt.autogun);
    if (! S || length <= 0)
        return {};
    const auto patch = buildPatch (*S, sampleRate, opt.quality, opt.tempo, 512);
    if (opt.autogun)
        patch->sg = sgPresetFrom (opt.sgState), patch->sgMix = S->P[63];
    auto engine = std::make_unique<Engine> (patch);
    engine->counter = opt.counter;
    engine->maxpoly = opt.maxPolyphony;
    engine->hostKills = opt.hostKills;
    std::vector<int> evs;
    for (const auto& nt : notes)
    {
        evs.push_back (nt.start);
        if (nt.release >= 0)
            evs.push_back (nt.release);
    }
    std::vector<long long> handle (notes.size(), -1);
    std::vector<float> out (2 * (std::size_t) length, 0.0f), l ((std::size_t) Engine::maxHostBlock), r (l.size());
    const int block = std::max (1, std::min (opt.block, Engine::maxHostBlock));
    int n0 = 0;
    while (n0 < length)
    {
        auto ln = std::min (block, length - n0);
        for (auto e : evs)
            if (n0 < e && e < n0 + ln)
                ln = e - n0;
        // events at this sample in input order (ogmodel sorts by sample, stable)
        struct Ev
        {
            int idx;
            bool on;
        };
        std::vector<std::pair<int, Ev>> list;
        for (std::size_t i = 0; i < notes.size(); ++i)
        {
            if (notes[i].start == n0)
                list.push_back ({ (int) list.size(), { (int) i, true } });
            if (notes[i].release == n0)
                list.push_back ({ (int) list.size(), { (int) i, false } });
        }
        for (const auto& [o, e] : list)
        {
            const auto& nt = notes[(std::size_t) e.idx];
            if (e.on)
            {
                const int sh = (std::size_t) e.idx < opt.j0Shift.size() ? opt.j0Shift[(std::size_t) e.idx] : 0;
                handle[(std::size_t) e.idx] = engine->noteOn (nt.key, nt.velocity, nt.finalVol, nt.finalPan, nt.pitchOffset, nt.fcut,
                                                              nt.fres, nt.releaseVelocity, sh);
            }
            else if (handle[(std::size_t) e.idx] >= 0)
                engine->release (handle[(std::size_t) e.idx]);
        }
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
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
} // namespace ogun
} // namespace lynx
