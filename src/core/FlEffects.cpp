#include "core/FlEffects.h"
#include "core/ControlSurface.h"
#include "core/XyController.h"
#include "core/FormulaController.h"
#include "core/Reeverb2.h"
#include "core/LuxeVerb.h"
#include "core/ConvolverFx.h"
#include "core/VintagePhaser.h"
#include "core/Vocodex.h"
#include "core/FruityVocoder.h"
#include "core/MaximusFx.h"
#include "core/EquoFx.h"
#include "core/HardcoreFx.h"
#include "core/DistructorFx.h"
#include "core/MultibandDelayFx.h"
#include "core/PitchShifterFl.h"
#include "core/ParametricEqFl.h"
#include "core/FruityDynamics.h"
#include "core/FruityShapers.h"
#include "core/FruityBasicFx.h"
#include "core/FruityFilters.h"
#include "core/FruityDelay.h"
#include "core/StereoFx.h"
#include "core/FrequencySplitter.h"
#include "core/ChorusFx.h"
#include "core/GrossBeat.h"
#include "core/LovePhilter.h"
#include "core/FruityDelayBank.h"
#include "core/FruityMultiband.h"
#include "core/FlangusFastDist.h"
#include "core/FruityPhaser.h"
#include "core/EmphasizerFx.h"
#include "core/FlLegacyFx.h"
#include "core/FreqShifterFl.h"
#include "core/LowLifterFl.h"
#include "core/SytrusEngine.h"
#include "core/FlSynths.h"

#include <algorithm>
#include <functional>
#include <cctype>
#include <cmath>
#include <cstring>

#include "core/Effects.h"
#include "core/FlpImport.h"
#include "core/NewtoneFx.h"
#include "core/VisualFx.h"
#include "flp/Flp.h"

namespace lynx
{
namespace
{
std::int32_t i32At (const std::vector<std::uint8_t>& d, std::size_t offset)
{
    std::int32_t v = 0;
    if (offset + 4 <= d.size())
        std::memcpy (&v, d.data() + offset, 4);
    return v;
}

// Lk(x) = (k^x − 1)/(k − 1): the gain curves of the Fruity plugins
double curve (double k, double x)
{
    return (std::pow (k, std::max (0.0, x)) - 1.0) / (k - 1.0);
}

double toDb (double gain)
{
    return 20.0 * std::log10 (std::max (gain, 1.0e-6));
}

// inverse of logRange (lo·(hi/lo)^v)
float fromLog (double value, double lo, double hi)
{
    return (float) std::clamp (std::log (std::max (value, lo) / lo) / std::log (hi / lo), 0.0, 1.0);
}

float clamp01 (double v)
{
    return (float) std::clamp (v, 0.0, 1.0);
}

// Q of a peak/band-pass from the width in octaves (cookbook)
double qFromOctaves (double octaves)
{
    return 1.0 / (2.0 * std::sinh (std::log (2.0) / 2.0 * std::max (0.01, octaves)));
}

// --- Fruity Parametric EQ 2 (REA_rodada7 §1) ------------------------------------------------

bool parametricEq2 (const std::vector<std::uint8_t>& d, EffectSlot& slot)
{
    // FL's own engine (REA_rodada50): the slot keeps FL's values and runs the exact Parametric EQ 2
    if (const auto exact = peqfl::parsePeq2 (d))
    {
        peqfl::toParams (*exact, slot.params);
        return true;
    }

    // older approximation (REA_rodada7 §1), for states too short to be FL's
    if (d.size() < 4 + 36 * 4)
        return false;

    const auto version = i32At (d, 0);
    const auto mask = version >= 4 && d.size() >= 0x142 ? (std::uint32_t) i32At (d, 0x13E) : 0x7Fu;
    auto param = [&] (int i) { return i32At (d, 4 + 4 * (std::size_t) i); };

    for (int b = 0; b < peq::bands; ++b)
    {
        const auto gainDb = std::clamp (param (b) / 100.0, -18.0, 18.0);
        const auto hz = 20.0 * std::pow (1000.0, std::clamp (param (7 + b), 0, 65536) / 65536.0);
        const auto w = std::clamp (param (14 + b), 0, 65536) / 65536.0;
        const auto type = param (21 + b);
        const auto order = std::clamp (param (28 + b), -3, 3);
        const auto on = (mask & (1u << b)) != 0;

        peq::BandType t = peq::BandType::off;
        double q = 1.0;

        switch (on ? type : 0)
        {
            case 1: t = peq::BandType::highCut; q = 0.1 + 9.9 * std::pow (1.0 - w, 3.0); break;
            case 2: t = peq::BandType::bandPass; q = qFromOctaves ((0.1 + 4.9 * w) * 0.708 * std::sqrt (std::abs (order) + 1.0)); break;
            case 3: t = peq::BandType::lowCut; q = 0.1 + 9.9 * std::pow (1.0 - w, 3.0); break;
            case 4: t = peq::BandType::notch; q = qFromOctaves ((0.1 + 4.9 * w) / (0.708 * std::sqrt (std::abs (order) + 1.0))); break;
            case 5: t = peq::BandType::lowShelf; q = 1.0 / 3.0 + 1.7193 * (1.0 - w); break; // Q = shelf slope S
            case 6: t = peq::BandType::peak; q = qFromOctaves (0.1 + 4.9 * w); break;
            case 7: t = peq::BandType::highShelf; q = 1.0 / 3.0 + 1.7193 * (1.0 - w); break;
            default: break;
        }

        slot.params[(std::size_t) peq::param (b, 0)] = peq::typeValue (t);
        slot.params[(std::size_t) peq::param (b, 1)] = peq::hzValue (hz);
        slot.params[(std::size_t) peq::param (b, 2)] = peq::gainValue (gainDb);
        slot.params[(std::size_t) peq::param (b, 3)] = peq::qValue (q);
        // order: |order| + 1 biquads (the "Steep 8" cuts = 4)
        slot.params[(std::size_t) (peq::slopeParam + b)] = peq::slopeValue (std::abs (order) + 1);
    }

    slot.params[peq::outputParam] = peq::gainValue (std::clamp (param (35) / 100.0, -18.0, 18.0));
    return true;
}

// --- Fruity Limiter (§2): limiter, or its compressor when only the compressor is active -------

bool limiter (const std::vector<std::uint8_t>& d, EffectSlot& slot, bool forceCompressor = false)
{
    if (d.size() < 4 + 18 * 4)
        return false;

    auto param = [&] (int i) { return i32At (d, 4 + 4 * (std::size_t) i); };
    auto timeMs = [] (int v) { return 2.0 * (std::pow (501.0, std::clamp (v, 0, 10000) / 10000.0) - 1.0); };
    const auto gainDb = toDb (curve (7.0, param (0) / 1000.0));
    const auto ceilingDb = toDb (curve (3.0, param (2) / 1000.0));
    const auto compThresholdDb = toDb (curve (7.0, param (8) / 1000.0));
    const auto ratioRaw = param (9);
    const auto ratio = ratioRaw > 0 ? 1.0 + 0.38 * (std::pow (51.0, ratioRaw / 1000.0) - 1.0) : 1.0;

    // a ceiling above 0 dB practically disables the limiter; then what you hear is the compressor
    if (forceCompressor || (ceilingDb > -0.1 && ratio > 1.05 && compThresholdDb < -0.5))
    {
        slot = makeEffectSlot (EffectKind::compressor);
        slot.params[0] = clamp01 ((compThresholdDb + 60.0) / 60.0);
        slot.params[1] = clamp01 ((std::min (ratio, 20.0) - 1.0) / 19.0);
        slot.params[2] = fromLog (std::max (0.1, timeMs (param (11))), 0.1, 100.0);
        slot.params[3] = fromLog (2.0 * timeMs (param (12)), 10.0, 1000.0);
        slot.params[4] = clamp01 (std::max (0.0, gainDb) / 24.0);
        return true;
    }

    slot = makeEffectSlot (EffectKind::limiter);
    slot.params[0] = clamp01 ((std::min (ceilingDb, 0.0) + 12.0) / 12.0);
    slot.params[1] = clamp01 (std::max (0.0, gainDb) / 24.0);
    slot.params[2] = fromLog (2.0 * timeMs (param (5)), 10.0, 1000.0);
    return true;
}

// --- Fruity Reeverb 2 (§3) --------------------------------------------------------------------

bool reeverb2Approx (const std::vector<std::uint8_t>& d, EffectSlot& slot, double bpm)
{
    const auto version = i32At (d, 0);
    const auto count = version == 10001 ? 15 : version == 10000 ? 13 : 0;

    if (count == 0 || d.size() < 4 + (std::size_t) count * 4)
        return false;

    auto param = [&] (int i) { return i32At (d, 4 + 4 * (std::size_t) i); };
    const auto decay = std::clamp (param (5) / 10.0, 0.1, 20.0);
    const auto dampingKhz = param (6) >= 221 ? 22.0 : std::clamp (param (6) / 10.0, 0.5, 22.0);
    const auto separation = std::clamp (param (9) / 64.0, -1.0, 1.0);
    const auto dry = curve (11.0, param (10) / 128.0);
    const auto wet = curve (11.0, param (12) / 256.0);

    // LYNX's Freeverb: comb feedback for the requested decay (~30 ms combs)
    const auto feedback = std::pow (10.0, -3.0 * 0.03 / decay);
    slot.params[0] = clamp01 ((feedback - 0.7) / 0.28);
    slot.params[1] = clamp01 (1.0 - std::log (dampingKhz / 0.5) / std::log (22.0 / 0.5)); // more damping with a lower cutoff
    slot.params[2] = clamp01 (1.0 + std::min (0.0, separation));                           // negative separation narrows
    slot.params[3] = clamp01 (wet / 0.25);
    slot.params[4] = clamp01 (dry);

    // low cut (< 20 Hz = off), high cut (>= 22.1 kHz = off) and predelay
    const auto lowCutHz = param (0);
    slot.params[5] = lowCutHz < 20 ? 0.0f : std::max (0.002f, fromLog (lowCutHz, 20.0, 3000.0));
    slot.params[6] = param (1) >= 221 ? 1.0f : std::min (0.998f, fromLog (param (1) * 100.0, 500.0, 22000.0));
    const auto tempoPredelay = d.size() > 4 + (std::size_t) count * 4 && d[4 + (std::size_t) count * 4] != 0;
    const auto predelayMs = tempoPredelay ? param (2) / 48.0 * 60000.0 / std::max (1.0, bpm) : param (2) * 1000.0 / 384.0;
    slot.params[7] = clamp01 (predelayMs / 1000.0);
    return true;
}

// --- Fruity Delay 2 and Delay 3 (§4) ---------------------------------------------------------------

float delayTimeValue (double beats)
{
    // the closest built-in delay division (0.25 … 4 beats, 8 divisions)
    constexpr double divisions[] = { 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0 };
    int best = 0;

    for (int i = 1; i < 8; ++i)
        if (std::abs (std::log (divisions[i] / beats)) < std::abs (std::log (divisions[best] / beats)))
            best = i;

    return ((float) best + 0.5f) / 8.0f;
}

bool delay2 (const std::vector<std::uint8_t>& d, EffectSlot& slot)
{
    if (d.size() < 32)
        return false;

    slot.params[0] = delayTimeValue (std::clamp (i32At (d, 0x10), 1, 768) / 48.0);
    slot.params[1] = clamp01 (i32At (d, 0x0C) / 127.0 * 0.9);
    slot.params[2] = clamp01 (curve (11.0, i32At (d, 0x04) / 128.0) * 0.5);
    slot.params[3] = i32At (d, 0x18) == 2 ? 1.0f : 0.0f;
    slot.params[4] = clamp01 (1.0 - i32At (d, 0x1C) / 128.0);
    return true;
}

bool delay3 (const std::vector<std::uint8_t>& d, EffectSlot& slot)
{
    if (d.size() < 4 + 26 * 4)
        return false;

    auto param = [&] (int i) { return i32At (d, 4 + 4 * (std::size_t) i); };
    const auto synced = param (2) != 0;
    // synced: ticks of 1/96 beat; unsynced the unit is unknown: use 1/4
    slot.params[0] = delayTimeValue (synced ? std::clamp (param (4), 1, 768) / 96.0 : 1.0);
    slot.params[1] = clamp01 (param (14) / 6000.0 * 0.9);
    slot.params[2] = clamp01 (param (23) / 6000.0 * 0.5);
    slot.params[3] = param (1) == 2 ? 1.0f : 0.0f;
    slot.params[4] = clamp01 (1.0 - param (16) / 6000.0);
    return true;
}

// --- Fruity Compressor (§5) ----------------------------------------------------------------------

bool compressor (const std::vector<std::uint8_t>& d, EffectSlot& slot)
{
    if (d.size() < 4 + 6 * 4)
        return false;

    auto param = [&] (int i) { return i32At (d, 4 + 4 * (std::size_t) i); };
    slot.params[0] = clamp01 ((std::clamp (param (0) / 10.0, -60.0, 0.0) + 60.0) / 60.0);
    slot.params[1] = clamp01 ((std::clamp (param (1) / 10.0, 1.0, 20.0) - 1.0) / 19.0);
    slot.params[4] = clamp01 (std::clamp (param (2) / 10.0, 0.0, 24.0) / 24.0);
    slot.params[2] = fromLog (std::max (0.1, param (3) / 10.0), 0.1, 100.0);
    slot.params[3] = fromLog (std::max (10.0, (double) param (4)), 10.0, 1000.0);
    return true;
}
// --- REA_rodada8: Maximus, Soundgoodizer, Fast Dist, Fruity Filter, Flangus, Soft Clipper -------

double timeMs (int v) // T(x) = 2·(501^x − 1) ms
{
    return 2.0 * (std::pow (501.0, std::clamp (v, 0, 10000) / 10000.0) - 1.0);
}

float compressorAttackValue (double ms) { return fromLog (std::max (0.1, ms), 0.1, 100.0); }
float compressorReleaseValue (double ms) { return fromLog (std::max (10.0, ms), 10.0, 1000.0); }
float limiterReleaseValue (double ms) { return fromLog (std::max (10.0, ms), 10.0, 1000.0); }

float utilityGainValue (double gain);

bool maximus (const std::vector<std::uint8_t>& d, EffectSlot& slot)
{
    if (d.size() < 4 + 59 * 4)
        return false;

    const auto header = (std::uint32_t) i32At (d, 0);
    const auto version = header > 0xFFFFu ? (int) (header >> 16) : (int) header;
    auto param = [&] (int i) { return i32At (d, 4 + 4 * (std::size_t) i); };

    // the master section (band 3) represents the effect; the bands are multiband (LYNX has none)
    constexpr int master = 7 + 13 * 3;
    const auto mode = param (master);
    const auto preDb = toDb (curve (7.0, param (master + 1) / 1000.0));
    const auto postDb = toDb (curve (7.0, param (master + 2) / 1000.0));

    if (mode == 3) // master off: pass-through (a transparent utility keeps the slot position)
    {
        slot = makeEffectSlot (EffectKind::utility);
        return true;
    }

    if (mode != 0) // no compression: gains only
    {
        slot = makeEffectSlot (EffectKind::utility);
        slot.params[0] = utilityGainValue (std::pow (10.0, (preDb + postDb) / 20.0));
        return true;
    }

    // master compression curve: 7th envelope (low, 2nd, mid, 2nd, high, 2nd, master, 2nd)
    std::vector<std::pair<double, double>> points;
    std::size_t pos = version >= 15 ? 0x21F : version >= 14 ? 0x21B : 0x1FE;

    for (int env = 0; env < 8 && pos + 12 <= d.size(); ++env)
    {
        const auto count = (std::size_t) std::max (0, i32At (d, pos + 8));

        if (count > 512 || pos + 12 + 24 * count + 20 > d.size())
            break;

        if (env == 6)
        {
            double x = 0.0;

            for (std::size_t k = 0; k < count; ++k)
            {
                double dx = 0.0, y = 0.0;
                std::memcpy (&dx, d.data() + pos + 12 + 24 * k, 8);
                std::memcpy (&y, d.data() + pos + 12 + 24 * k + 8, 8);
                x += dx;
                if (std::isfinite (x) && std::isfinite (y))
                    points.push_back ({ std::clamp (x, 0.0, 1.0), std::clamp (y, 0.0, 1.0) });
            }
        }

        pos += 12 + 24 * count + 20;
    }

    if (points.size() < 2)
        points = { { 0.0, 0.0 }, { 0.5, 0.5 }, { 1.0, 0.5 } }; // the default curve: limiter at −0.22 dB

    auto curveAt = [&] (double c) {
        if (c <= points.front().first)
            return points.front().second;
        for (std::size_t k = 1; k < points.size(); ++k)
            if (c <= points[k].first)
            {
                const auto span = points[k].first - points[k - 1].first;
                const auto t = span > 0.0 ? (c - points[k - 1].first) / span : 1.0;
                return points[k - 1].second + t * (points[k].second - points[k - 1].second);
            }
        return points.back().second;
    };
    auto levelDb = [] (double c, double scale) { return toDb ((std::pow (3.0, 2.0 * c) - 1.0) / 2.0 * scale); };

    // ceiling, knee and ratio of the curve (input level c: 0.5 = 0 dBFS, 1 = +12 dB)
    double ceilingDb = -120.0, kneeDb = 0.0, kneeOut = 0.0;
    bool knee = false;

    for (int k = 1; k <= 400; ++k)
    {
        const auto c = k / 400.0;
        const auto in = levelDb (c, 1.0), out = levelDb (curveAt (c), 0.975);
        ceilingDb = std::max (ceilingDb, out);

        if (! knee && out < in - 0.5)
        {
            knee = true;
            kneeDb = in;
            kneeOut = out;
        }
    }

    const auto topIn = levelDb (1.0, 1.0), topOut = levelDb (curveAt (1.0), 0.975);
    const auto ratio = knee && topOut - kneeOut > 0.05 ? (topIn - kneeDb) / (topOut - kneeOut) : 100.0;

    if (knee && kneeDb < -0.5 && ratio < 8.0) // real compression (knee below 0 dB)
    {
        slot = makeEffectSlot (EffectKind::compressor);
        slot.params[0] = clamp01 ((kneeDb + 60.0) / 60.0);
        slot.params[1] = clamp01 ((std::clamp (ratio, 1.0, 20.0) - 1.0) / 19.0);
        slot.params[2] = compressorAttackValue (timeMs (param (master + 6)));
        slot.params[3] = compressorReleaseValue (2.0 * timeMs (param (master + 9)));
        slot.params[4] = clamp01 (std::max (0.0, preDb + postDb) / 24.0);
        return true;
    }

    slot = makeEffectSlot (EffectKind::limiter);
    slot.params[0] = clamp01 ((std::min (ceilingDb + postDb, 0.0) + 12.0) / 12.0);
    slot.params[1] = clamp01 (std::max (0.0, preDb) / 24.0);
    slot.params[2] = limiterReleaseValue (2.0 * timeMs (param (master + 9)));
    return true;
}

bool soundgoodizer (const std::vector<std::uint8_t>& d, EffectSlot& slot)
{
    if (d.size() < 12)
        return false;

    // preset A–D (full Maximus copies); compressor approximation measured at amount 0.6
    struct Preset { double threshold, ratio, makeup; };
    constexpr Preset presets[] = { { -6.0, 2.5, 5.3 }, { -10.5, 2.2, 6.6 }, { -10.8, 2.5, 7.9 }, { -14.0, 1.9, 8.5 } };
    const auto& p = presets[std::clamp (i32At (d, 4), 0, 3)];
    const auto amount = std::clamp (i32At (d, 8), 0, 1000) / 1000.0;
    const auto scale = amount / 0.6;

    slot = makeEffectSlot (EffectKind::compressor);
    slot.params[0] = clamp01 ((p.threshold + 60.0) / 60.0);
    slot.params[1] = clamp01 ((1.0 + (p.ratio - 1.0) * scale - 1.0) / 19.0);
    slot.params[2] = compressorAttackValue (2.0);
    slot.params[3] = compressorReleaseValue (85.0);
    slot.params[4] = clamp01 (p.makeup * scale / 24.0);
    return true;
}

bool fastDist (const std::vector<std::uint8_t>& d, EffectSlot& slot)
{
    if (d.size() < 20)
        return false;

    const auto pre = curve (11.0, std::clamp (i32At (d, 0), 64, 192) / 128.0);
    const auto t = std::clamp (i32At (d, 4), 1, 10);
    const auto typeB = i32At (d, 8) != 0;
    const auto mix = curve (11.0, std::clamp (i32At (d, 12), 0, 128) / 128.0);
    const auto post = curve (11.0, std::clamp (i32At (d, 16), 0, 128) / 128.0);

    // equivalent tanh drive for FL's curve (fit from REA_rodada8 §3.2)
    constexpr double driveA[] = { 0, 0, 0, 0, 2.2, 4.5, 6.8, 9.2, 12.8, 17.8, 22.8 };
    const auto curveDb = typeB ? std::max (0.0, 0.8 + (t - 6) * 0.85) * (t >= 6 ? 1.0 : 0.0) : driveA[t];
    const auto wet = mix * post;
    const auto dry = (post - wet) * pre * post; // post is applied squared to the dry signal (as in FL)
    const auto level = typeB ? 1.0 : std::pow (10.0, -0.9 * (t - 1) / 20.0);
    const auto total = wet * level + dry;

    slot.params[0] = clamp01 ((toDb (pre) + curveDb) / 36.0);
    slot.params[1] = 1.0f; // Fast Dist does not filter
    slot.params[2] = total > 1.0e-6 ? clamp01 (wet * level / total) : 1.0f;
    slot.params[3] = clamp01 (0.5 + toDb (std::max (total, 1.0e-6)) / 24.0);
    return true;
}

bool fruityFilter (const std::vector<std::uint8_t>& d, EffectSlot& slot)
{
    if (d.size() < 4 + 6 * 4)
        return false;

    auto param = [&] (int i) { return i32At (d, 4 + 4 * (std::size_t) i); };
    const auto lp = curve (11.0, std::clamp (param (2), 0, 1024) / 1024.0);
    const auto bp = curve (11.0, std::clamp (param (3), 0, 1024) / 1024.0);
    const auto hp = curve (11.0, std::clamp (param (4), 0, 1024) / 1024.0);

    // built-in filter type: 0 low-pass, 1 high-pass, 2 band-pass (floor(3v))
    auto type = 0.0f;
    if (hp > lp && hp >= bp && ! (lp >= 0.5 && hp >= 0.5))
        type = 0.4f;
    else if (bp > lp && bp > hp)
        type = 0.7f;

    // Chamberlin SVF without rate correction: "open" still cuts at ~7.35 kHz (at 44.1 kHz)
    const auto f = std::min (1.0, (std::pow (26.0, std::clamp (param (0), 0, 1024) / 1024.0) - 1.0) * 0.04);
    const auto hz = std::max (20.0, 44100.0 / 3.141592653589793 * std::asin (f / 2.0));
    auto q = 1.0 / (1.0 - 0.8 * std::clamp (param (1), 0, 1024) / 1024.0);
    if (param (5) != 0)
        q *= 1.3; // x2: second stage in series

    slot.params[0] = type;
    slot.params[1] = fromLog (hz, 20.0, 20000.0);
    slot.params[2] = fromLog (q, 0.5, 12.0);
    slot.params[3] = 1.0f;
    return true;
}

bool flangus (const std::vector<std::uint8_t>& d, EffectSlot& slot)
{
    if (d.size() < 32)
        return false;

    auto param = [&] (int i) { return i32At (d, 4 * (std::size_t) i); };
    const auto depthMs = 24.8 * curve (11.0, std::clamp (param (1), 0, 1024) / 1024.0);
    const auto voiceDepth = depthMs * 44.1 + 10.0; // samples at 44.1 kHz
    const auto period = 75000.0 * (std::pow (101.0, 1.0 - std::clamp (param (2), 0, 1024) / 1024.0) - 1.0) + 50000.0;
    const auto hz = 250.0 * 44100.0 / (period * voiceDepth);
    const auto dryGain = curve (11.0, std::abs (std::clamp (param (6), -128, 128)) / 128.0);
    const auto wetGain = curve (11.0, std::abs (std::clamp (param (7), -128, 128)) / 128.0);
    const auto r = dryGain > 1.0e-6 ? wetGain / dryGain : 1000.0;

    slot.params[0] = fromLog (std::max (0.05, hz), 0.05, 5.0);
    slot.params[1] = clamp01 (2.0 * depthMs / 8.0);
    slot.params[2] = clamp01 (2.0 * r / (1.0 + r));
    slot.params[3] = 0.0f; // Flangus has no feedback
    return true;
}

bool softClipper (const std::vector<std::uint8_t>& d, EffectSlot& slot)
{
    if (d.size() < 8)
        return false;

    const auto t = std::clamp (i32At (d, 0), 1, 127);
    const auto postDb = toDb (curve (11.0, std::clamp (i32At (d, 4), 0, 160) / 128.0));
    const auto drop = t >= 100 ? 1.5 : t >= 64 ? 2.0 - (t - 64) / 36.0 * 0.5 : 3.8 - (t - 1) / 63.0 * 1.8; // level drop of the curve

    slot.params[0] = 0.0f;
    slot.params[1] = 1.0f;
    slot.params[2] = 1.0f;
    slot.params[3] = clamp01 (0.5 + (postDb - drop) / 24.0);
    return true;
}

// --- Balance, PanOMatic, Stereo Shaper (§6) -> Utility ------------------------------------------

float utilityGainValue (double gain) { return clamp01 (toDb (gain) / 48.0 + 0.5); }
float utilityMatrixValue (double m) { return clamp01 ((m + 2.0) / 4.0); }

bool balance (const std::vector<std::uint8_t>& d, EffectSlot& slot)
{
    if (d.size() < 8)
        return false;

    slot.params[1] = clamp01 (std::clamp (i32At (d, 0), -128, 128) / 256.0 + 0.5);
    slot.params[0] = utilityGainValue (curve (11.0, i32At (d, 4) / 256.0)); // 256 = 0 dB
    return true;
}

bool panOMatic (const std::vector<std::uint8_t>& d, EffectSlot& slot)
{
    if (d.size() < 8)
        return false;

    slot.params[1] = clamp01 (std::clamp (i32At (d, 0), 0, 256) / 256.0); // 128 = center
    slot.params[0] = utilityGainValue (curve (11.0, i32At (d, 4) / 256.0));
    return true; // PanOMatic's LFO (enabled in few demos) is left out
}

bool stereoShaper (const std::vector<std::uint8_t>& d, EffectSlot& slot)
{
    if (d.size() < 4 + 4 * 4)
        return false;

    auto param = [&] (int i) { return i32At (d, 4 + 4 * (std::size_t) i) / 12800.0; }; // 12800 = 1.0
    slot.params[3] = utilityMatrixValue (param (0)); // R -> L
    slot.params[2] = utilityMatrixValue (param (1)); // L
    slot.params[5] = utilityMatrixValue (param (2)); // R
    slot.params[4] = utilityMatrixValue (param (3)); // L -> R
    return true;
}
// --- REA_rodada14: Gross Beat (volume), Frequency Splitter, Effector ----------------------------

// Gross Beat: the selected volume slot (4-beat envelope) becomes the gate's 32 steps; the
// "time travel" has no equivalent. Without a readable state, the gate stays transparent.
bool grossBeat (const std::vector<std::uint8_t>& d, EffectSlot& slot)
{
    for (int i = 0; i < 32; ++i)
        slot.params[(std::size_t) i] = 1.0f;

    if (d.size() < 64)
        return false;
    auto version = (std::uint32_t) i32At (d, 0);
    if (version > 0xffff)
        version >>= 16;
    if (version < 3 || version > 9)
        return false;

    const auto paramCount = 9;
    auto param = [&] (int i) { return i32At (d, 4 + 4 * (std::size_t) i); };
    std::size_t pos = 4 + 4 * paramCount + (version >= 5 ? 9 : 8);
    const auto volumeSlot = 36 + std::clamp (param (1), 0, 35);

    // the 72 slots (36 time + 36 volume): name, 15 B of sync, envelope
    std::vector<std::pair<double, double>> chosen; // (accumulated x in beats, y)
    std::vector<int> curves;
    for (int s = 0; s < 72; ++s)
    {
        if (pos >= d.size())
            return false;
        pos += 1 + d[pos] + 15;
        if (pos + 12 > d.size())
            return false;
        const auto n = i32At (d, pos + 8);
        if (n < 0 || n > 5000 || pos + 12 + (std::size_t) n * 24 + 20 > d.size())
            return false;
        pos += 12;
        double x = 0.0;
        for (int k = 0; k < n; ++k, pos += 24)
            if (s == volumeSlot)
            {
                double dx = 0.0, y = 0.0;
                std::memcpy (&dx, d.data() + pos, 8);
                std::memcpy (&y, d.data() + pos + 8, 8);
                x += std::isfinite (dx) ? std::max (0.0, dx) : 0.0;
                chosen.emplace_back (x, std::isfinite (y) ? std::clamp (y, 0.0, 1.0) : 1.0);
                curves.push_back (d[pos + 20]);
            }
        pos += 20;
    }

    if (chosen.empty())
        return true;

    // value at the middle of each step; "hold" (2) holds the previous point, the rest is linear
    for (int i = 0; i < 32; ++i)
    {
        const auto t = (i + 0.5) / 32.0 * 4.0;
        auto value = chosen.back().second;
        if (t <= chosen.front().first)
            value = chosen.front().second;
        else
            for (std::size_t k = 1; k < chosen.size(); ++k)
                if (t < chosen[k].first)
                {
                    const auto [x0, y0] = chosen[k - 1];
                    const auto [x1, y1] = chosen[k];
                    value = curves[k] == 2 || x1 <= x0 ? y0 : y0 + (y1 - y0) * (t - x0) / (x1 - x0);
                    break;
                }
        slot.params[(std::size_t) i] = (float) value;
    }

    slot.params[32] = clamp01 (param (3) / 256.0);
    slot.params[33] = clamp01 (param (4) / 10000.0);
    slot.params[34] = clamp01 (param (5) / 10000.0);
    return true;
}

// Frequency Splitter: the main output with per-band level becomes shelves in the parametric EQ
bool frequencySplitter (const std::vector<std::uint8_t>& d, EffectSlot& slot)
{
    if (d.size() < 15 * 4)
        return false;
    auto word = [&] (int i) { return i32At (d, 4 * (std::size_t) i); };
    auto hz = [] (int v) { return 30.0 * std::pow (16000.0 / 30.0, std::clamp (v, 0, 65536) / 65536.0); };
    auto db = [] (int v) { return std::clamp (v / 100.0, -18.0, 18.0); };
    const auto threeBands = word (6) != 0;
    const auto low = db (word (12)), mid = db (word (13)), high = db (word (14));

    for (int b = 0; b < peq::bands; ++b)
        slot.params[(std::size_t) peq::param (b, 0)] = peq::typeValue (peq::BandType::off);

    auto shelf = [&] (int band, peq::BandType type, double f, double gain) {
        slot.params[(std::size_t) peq::param (band, 0)] = peq::typeValue (type);
        slot.params[(std::size_t) peq::param (band, 1)] = peq::hzValue (f);
        slot.params[(std::size_t) peq::param (band, 2)] = peq::gainValue (gain);
        slot.params[(std::size_t) peq::param (band, 3)] = peq::qValue (1.0);
    };

    if (threeBands)
    {
        shelf (0, peq::BandType::lowShelf, hz (word (1)), low - mid);
        shelf (6, peq::BandType::highShelf, hz (word (2)), high - mid);
        slot.params[peq::outputParam] = peq::gainValue (mid);
    }
    else
    {
        shelf (0, peq::BandType::lowShelf, hz (word (1)), low - mid);
        slot.params[peq::outputParam] = peq::gainValue (mid);
    }
    return true;
}

// Peak Controller (§7.3): base, amount, tension and decay speed
bool peakController (const std::vector<std::uint8_t>& d, EffectSlot& slot)
{
    if (d.size() < 40)
        return false;
    slot.params[0] = clamp01 (i32At (d, 0) / 65536.0);
    slot.params[1] = clamp01 ((std::clamp (i32At (d, 4), -256, 256) / 256.0 + 1.0) / 2.0);
    slot.params[2] = clamp01 ((std::clamp (i32At (d, 8), -128, 128) / 128.0 + 1.0) / 2.0);
    slot.params[3] = clamp01 ((std::clamp (i32At (d, 12), 5, 128) - 5) / 123.0);
    slot.params[4] = clamp01 (i32At (d, 16) / 65536.0); // LFO base
    return true;
}

// --- REA_rodada15: Pitch Shifter, Frequency Shifter, Emphasis, Low Lifter, Send, Transporter -----

bool pitchShifter (const std::vector<std::uint8_t>& d, EffectSlot& slot)
{
    if (d.size() < 4 + 13 * 4)
        return false;
    auto param = [&] (int i) { return i32At (d, 4 + 4 * (std::size_t) i); };
    const auto cents = std::clamp (param (0), -1200, 1200);
    const auto semis = (int) std::lround (cents / 100.0);
    slot.params[0] = clamp01 (0.5 + semis / 48.0);
    slot.params[1] = clamp01 (0.5 + (cents - semis * 100) / 200.0);
    slot.params[2] = clamp01 (std::clamp (param (3), 0, 2400) / 2400.0);
    const auto granular = d.size() >= 65 && i32At (d, 61) == 1;
    const auto grainMs = granular ? 20.0 + 130.0 * (std::clamp (param (4), 2000, 15000) - 2000) / 13000.0 : 60.0;
    slot.params[3] = clamp01 ((grainMs - 20.0) / 100.0);
    return true;
}

bool frequencyShifter (const std::vector<std::uint8_t>& d, EffectSlot& slot)
{
    if (d.size() < 4 + 10 * 4)
        return false;
    auto param = [&] (int i) { return i32At (d, 4 + 4 * (std::size_t) i); };
    const auto s = std::clamp (param (2), -70000, 70000) / 70000.0;
    const auto k = param (8) == 0 ? 17.6096 : 10.9658; // range 20 kHz / 200 Hz ("Tempo" is treated as 200 Hz)
    const auto hz = s == 0.0 ? 0.0 : (s < 0.0 ? -1.0 : 1.0) * 0.1 * std::pow (2.0, k * std::abs (s));
    // our scale: 2000·x·|x| Hz
    const auto x = std::clamp ((hz < 0.0 ? -1.0 : 1.0) * std::sqrt (std::min (1.0, std::abs (hz) / 2000.0)), -1.0, 1.0);
    slot.params[0] = clamp01 (0.5 + x / 2.0);
    slot.params[1] = clamp01 (std::clamp (param (0), 0, 10000) / 10000.0);
    slot.params[2] = 0.0f;
    return true;
}

bool emphasis (const std::vector<std::uint8_t>& d, EffectSlot& slot)
{
    if (d.size() < 4 + 13 * 4)
        return false;
    auto x = [&] (int i) { return std::clamp (i32At (d, 4 + 4 * (std::size_t) i), 0, 10000) / 10000.0; };
    const auto ceilingDb = toDb (curve (11.0, x (9)));
    slot.params[0] = clamp01 ((std::min (ceilingDb, 0.0) + 12.0) / 12.0);
    slot.params[1] = clamp01 (x (6)); // input gain 24x dB = our 0..24 dB scale
    slot.enabled = i32At (d, 4) == 0;
    return true;
}

bool lowLifter (const std::vector<std::uint8_t>& d, EffectSlot& slot)
{
    if (d.size() < 4 + 15 * 4)
        return false;
    auto x = [&] (int i) { return std::clamp (i32At (d, 4 + 4 * (std::size_t) i), 0, 10000) / 10000.0; };
    auto hz = [] (double v) { return 20.0 + 100.0 * v * v; };
    const auto mix = x (1);
    const auto lowDb = std::clamp ((x (2) - 0.5) * 24.0, -18.0, 12.0);
    const auto highDb = std::clamp (100.0 * std::log10 (std::max (1e-3, x (9))), -18.0, 0.0);
    // the harmonics generator (Chebyshev up to 3rd order, REA_rodada17 §13) has no equivalent here; the EQ
    // reproduces only the low cut and the band gains
    const auto boost = 0.0;

    for (int b = 0; b < peq::bands; ++b)
        slot.params[(std::size_t) peq::param (b, 0)] = peq::typeValue (peq::BandType::off);
    if (hz (x (11)) > 21.0)
    {
        slot.params[(std::size_t) peq::param (0, 0)] = peq::typeValue (peq::BandType::lowCut);
        slot.params[(std::size_t) peq::param (0, 1)] = peq::hzValue (hz (x (11)));
        slot.params[(std::size_t) peq::param (0, 3)] = peq::qValue (0.707);
    }
    slot.params[(std::size_t) peq::param (1, 0)] = peq::typeValue (peq::BandType::lowShelf);
    slot.params[(std::size_t) peq::param (1, 1)] = peq::hzValue (hz (x (10)));
    slot.params[(std::size_t) peq::param (1, 2)] = peq::gainValue (std::clamp ((lowDb - highDb) * mix + boost, -18.0, 18.0));
    slot.params[(std::size_t) peq::param (1, 3)] = peq::qValue (1.0);
    slot.params[peq::outputParam] = peq::gainValue (highDb * mix);
    slot.enabled = i32At (d, 4) != 0;
    return true;
}

bool fruitySend (const std::vector<std::uint8_t>& d, EffectSlot& slot)
{
    if (d.size() < 12)
        return false;
    slot.params[0] = utilityGainValue (std::clamp (i32At (d, 8), 0, 256) / 256.0); // what continues on the track
    slot.params[1] = 0.5f;
    return true;
}

// Fruity Squeeze (REA_rodada29 §2): the exact effect; state = u32 version + 12 raw i32 controls
bool squeeze (const std::vector<std::uint8_t>& d, EffectSlot& slot)
{
    if (d.size() < 4 + 12 * 4)
        return false;
    slot = makeEffectSlot (EffectKind::squeeze);
    for (int i = 0; i < 12; ++i)
        slot.params[(std::size_t) i] = (float) clamp01 (std::clamp (i32At (d, 4 + 4 * (std::size_t) i), 0, (int) squeezeMax[(std::size_t) i])
                                                        / (double) squeezeMax[(std::size_t) i]);
    return true;
}

bool transporter (const std::vector<std::uint8_t>& d, EffectSlot& slot)
{
    if (d.size() < 8)
        return false;
    slot.params[0] = utilityGainValue (curve (11.0, std::clamp (i32At (d, 4), 0, 16000) / 12800.0)); // dry level
    return true;
}

// Effector: LYNX's Effector effect (core/Effector.h) receives FL's 12 raw parameters
std::optional<EffectSlot> effector (const std::vector<std::uint8_t>& d, double)
{
    // "SSLF" + size + "10XF" + 64 reserved floats + the 12 parameters
    if (d.size() < 12 + 76 * 4 || std::memcmp (d.data(), "SSLF", 4) != 0)
        return std::nullopt;
    float p[12];
    std::memcpy (p, d.data() + 12 + 64 * 4, sizeof (p));
    auto slot = makeEffectSlot (EffectKind::effector);
    for (int i = 0; i < 12; ++i)
        slot.params[(std::size_t) i] = std::isfinite (p[i]) ? std::clamp (p[i], 0.0f, 1.0f) : 0.0f;
    slot.enabled = slot.params[1] == 0.0f; // bypass = any value ≠ 0
    slot.params[1] = 0.0f;
    slot.mix = 1.0f; // each algorithm's mix is done inside the effect (Wet Mix = parameter 2)
    return slot;
}

// Fruity Vocoder (REA_rodada20 §4.1.1, REA_rodada70): the exact engine (core/FruityVocoder.h). The slot keeps FL's
// saved state (its per-band levels) and the controls as parameters (Effects.cpp, namespace vocoder).
std::optional<EffectSlot> fruityVocoder (const std::vector<std::uint8_t>& d, std::vector<std::string>* notes)
{
    (void) notes;
    const auto st = fvocoder::parse (d);
    if (! st)
        return std::nullopt;
    auto slot = makeEffectSlot (EffectKind::vocoder);
    int index = 0;
    while ((4 << index) < st->bands && index < 5)
        ++index;
    slot.params[0] = (float) index / 5.0f;
    slot.params[1] = (float) (st->order - 1) / 2.0f;
    slot.params[2] = (float) st->listen / 2.0f;
    slot.params[3] = st->swap ? 1.0f : 0.0f;
    for (int c = 0; c < fvocoder::controls; ++c)
    {
        const auto lo = fvocoder::controlMin (c), hi = fvocoder::controlMax (c);
        const auto v = std::clamp (st->c[(std::size_t) c], lo, hi);
        slot.params[(std::size_t) (4 + c)] = (float) (v - lo) / (float) (hi - lo);
    }
    slot.state = d;
    return slot;
}
} // namespace

std::vector<EffectSlot> soundgoodizerApproximation (int preset, int amount)
{
    // a Soundgoodizer state (version, preset, amount) through the same conversion as the plugin's own slots, then
    // the master limiter of its Maximus presets (a–d.dat: −0.2 dB ceiling, 86 ms release; REA_rodada8 §2)
    std::vector<std::uint8_t> state (12, 0);
    const std::int32_t words[3] { 1, preset, amount };
    std::memcpy (state.data(), words, sizeof (words));
    std::vector<EffectSlot> chain (1);
    soundgoodizer (state, chain[0]);
    auto limiterSlot = makeEffectSlot (EffectKind::limiter);
    limiterSlot.params[0] = clamp01 ((-0.2 + 12.0) / 12.0);
    limiterSlot.params[1] = 0.0f;
    limiterSlot.params[2] = limiterReleaseValue (86.0);
    chain.push_back (limiterSlot);
    return chain;
}

std::optional<VocodexConversion> vocodexEffect (const std::vector<std::uint8_t>& d, const FlDataReader& data)
{
    auto out = vocodexEffect (d);
    if (! out || ! data)
        return out;
    const auto state = vocodex::parseState (d);
    vocodex::InstallData files;
    const std::string folder = "Plugins\\Fruity\\Effects\\Vocodex\\";
    if (state->params[vocodex::sg] > 0)
    {
        files.sgDat = data (folder + "Data\\SG.dat");
        if (hlfx::makeMaximusPreset (files.sgDat) == nullptr)
            files.sgDat.clear();
    }
    if (state->internalCarrier() && ! state->tone.empty())
    {
        // FUN_008506a0: Synthesizer\Data\Presets\<tone>.FST (the files ship as .fst)
        auto fst = data (folder + "Synthesizer\\Data\\Presets\\" + state->tone + ".fst");
        if (fst.empty())
            fst = data (folder + "Synthesizer\\Data\\Presets\\" + state->tone + ".FST");
        if (auto st = hlfx::maximusStateFromFst (fst); st && sytrus::stateUsable (*st))
            files.carrierState = std::move (*st);
    }
    out->slot.state = vocodex::appendInstallData (d, files);
    out->notes.clear();
    if (state->internalCarrier() && vocodex::makeInternalCarrier (*state, files.carrierState) == nullptr)
        out->notes.push_back ("Vocodex: internal carrier synth not available (modulator only)");
    if (state->params[vocodex::sg] > 0 && files.sgDat.empty())
        out->notes.push_back ("Vocodex: Soundgoodizer stage approximated (SG.dat not found)");
    return out;
}

std::optional<VocodexConversion> vocodexEffect (const std::vector<std::uint8_t>& d)
{
    // the exact engine keeps FL's raw state (REA_rodada35); ModIn/CarIn index the track's inputs list
    const auto state = vocodex::parseState (d);
    if (! state)
        return std::nullopt;
    VocodexConversion out;
    out.slot = makeEffectSlot (EffectKind::vocodex);
    out.slot.state = d;
    out.modIn = state->lrEncoding ? 0 : state->modIn;
    out.carIn = state->lrEncoding ? 0 : state->carIn;
    if (state->internalCarrier() && vocodex::makeInternalCarrier (*state) == nullptr)
        out.notes.push_back ("Vocodex: internal carrier synth not available (modulator only)");
    if (state->params[vocodex::sg] > 0)
        out.notes.push_back ("Vocodex: Soundgoodizer stage approximated (SG.dat not found)");
    return out;
}

std::optional<EffectSlot> fruityLimiterCompressor (const std::vector<std::uint8_t>& state)
{
    EffectSlot slot;
    if (! limiter (state, slot, true))
        return std::nullopt;
    return slot;
}

std::optional<EffectSlot> maximusExact (const std::vector<std::uint8_t>& state)
{
    maximusfx::Blob blob;
    blob.state = state;
    if (maximusfx::presetOf (blob) == nullptr)
        return std::nullopt;
    auto slot = makeEffectSlot (EffectKind::maximus);
    slot.state = maximusfx::pack (blob);
    return slot;
}

EffectSlot soundgoodizerApproxSlot (int preset, int amount)
{
    preset = std::clamp (preset, 0, 3);
    amount = std::clamp (amount, 0, 1000);
    auto slot = makeEffectSlot (EffectKind::parallel);
    slot.branches.push_back ({ soundgoodizerApproximation (preset, amount), 1.0f });
    slot.state.resize (12);
    const std::int32_t words[3] { 1, preset, amount };
    std::memcpy (slot.state.data(), words, sizeof (words));
    return slot;
}

bool isSoundgoodizerApprox (const EffectSlot& slot)
{
    return slot.kind == EffectKind::parallel && slot.state.size() == 12 && slot.branches.size() == 1
           && maximusfx::parseSoundgoodizer (slot.state).has_value();
}

std::optional<EffectSlot> soundgoodizerExact (const std::vector<std::uint8_t>& state, const std::vector<std::uint8_t>& presetFile)
{
    // FUN_00836ac0 (Soundgoodizer_x64.dll): the preset file is loaded through the plugin's own Maximus copy, then
    // parameter 5 (band mix) is set to Amount (REA_rodada40 §3.1)
    const auto sg = maximusfx::parseSoundgoodizer (state);
    if (! sg)
        return std::nullopt;
    maximusfx::Blob blob;
    blob.build = hlfx::MaximusBuild::soundgoodizer;
    blob.mix = sg->amount;
    blob.state = presetFile;
    if (maximusfx::presetOf (blob) == nullptr)
        return std::nullopt;
    auto slot = makeEffectSlot (EffectKind::maximus);
    slot.state = maximusfx::pack (blob);
    return slot;
}

std::string soundgoodizerPresetPath (const std::vector<std::uint8_t>& state)
{
    const auto sg = maximusfx::parseSoundgoodizer (state);
    if (! sg)
        return {};
    return std::string ("Plugins\\Fruity\\Effects\\Soundgoodizer\\Data\\Presets\\") + maximusfx::soundgoodizerPresetFile (sg->preset);
}

std::optional<EffectSlot> effectFromFl (const std::string& pluginName, const std::vector<std::uint8_t>& state, double bpm,
                                        const FlDataReader& data)
{
    if (pluginName == "Vocodex" && data)
        if (auto v = vocodexEffect (state, data))
            return v->slot;
    if (pluginName == "Distructor" && data) // the amp/cabinet impulses of the installation travel with the slot (REA_rodada71)
        if (const auto s = distructor::parse (state))
        {
            std::map<int, std::vector<std::uint8_t>> files;
            for (const auto i : distructor::impulsesUsed (*s))
                if (auto bytes = data (distructor::impulsePath (i)); ! bytes.empty())
                    files[i] = std::move (bytes);
            auto slot = makeEffectSlot (EffectKind::distructor);
            slot.state = distructor::packBlob (state, files);
            distructor::toParams (*s, slot.params.data());
            return slot;
        }
    if (pluginName == "Fruity Convolver" && data) // impulse response file of the installation (REA_rodada60)
        if (auto exact = convolverFromFl (state, data))
            return exact;
    if (pluginName == "Hardcore" && data) // the exact engine with the installation's cabinet impulses (REA_rodada71)
        if (auto exact = hardcore::fromFl (state, data))
            return exact;
    if (pluginName == "Soundgoodizer" && data)
        if (const auto path = soundgoodizerPresetPath (state); ! path.empty())
            if (auto exact = soundgoodizerExact (state, data (path)))
                return exact;
    return effectFromFl (pluginName, state, bpm);
}

std::optional<EffectSlot> fruityDynamicsExact (const std::string& pluginName, const std::vector<std::uint8_t>& state)
{
    // FL's own Fruity Limiter / Fruity Compressor engines from the saved controls (REA_rodada48); the approximations
    // of REA_rodada7 remain for states that cannot be read
    std::string lower = pluginName;
    std::transform (lower.begin(), lower.end(), lower.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    if (lower == "fruity limiter")
        if (const auto s = fldyn::parseLimiter (state))
        {
            auto slot = makeEffectSlot (EffectKind::fruityLimiter);
            fldyn::limiterToParams (*s, slot.params.data());
            return slot;
        }
    if (lower == "fruity compressor")
        if (const auto s = fldyn::parseCompressor (state))
        {
            auto slot = makeEffectSlot (EffectKind::fruityCompressor);
            fldyn::compressorToParams (*s, slot.params.data());
            return slot;
        }
    return std::nullopt;
}

std::optional<EffectSlot> fruityShaperExact (const std::string& pluginName, const std::vector<std::uint8_t>& state)
{
    // FL's own Fruity WaveShaper / Fruity Soft Clipper engines from the saved state (REA_rodada51); the distortion
    // approximations of REA_rodada8 remain for states that cannot be read
    if (pluginName == "Fruity WaveShaper")
        if (const auto s = flshape::parseWaveShaper (state))
        {
            auto slot = makeEffectSlot (EffectKind::fruityWaveShaper);
            flshape::waveShaperToParams (*s, slot.params.data());
            slot.state = state; // the curve
            return slot;
        }
    if (pluginName == "Fruity Soft Clipper")
        if (const auto s = flshape::parseSoftClipper (state))
        {
            auto slot = makeEffectSlot (EffectKind::fruitySoftClipper);
            flshape::softClipperToParams (*s, slot.params.data());
            return slot;
        }
    return std::nullopt;
}

std::optional<EffectSlot> fruityFilterExact (const std::string& pluginName, const std::vector<std::uint8_t>& state)
{
    // FL's own Fruity Fast LP / Free Filter / Filter engines from the saved state (REA_rodada63); the generic filter
    // approximations remain for states that cannot be read
    if (pluginName == "Fruity Fast LP")
        if (const auto s = flfilter::parseFastLP (state))
        {
            auto slot = makeEffectSlot (EffectKind::fruityFastLP);
            flfilter::fastLPToParams (*s, slot.params.data());
            return slot;
        }
    if (pluginName == "Fruity Free Filter")
        if (const auto s = flfilter::parseFreeFilter (state))
        {
            auto slot = makeEffectSlot (EffectKind::fruityFreeFilter);
            flfilter::freeFilterToParams (*s, slot.params.data());
            return slot;
        }
    if (pluginName == "Fruity Filter")
        if (const auto s = flfilter::parseFilter (state))
        {
            auto slot = makeEffectSlot (EffectKind::fruityFilter);
            flfilter::filterToParams (*s, slot.params.data());
            return slot;
        }
    return std::nullopt;
}

std::optional<EffectSlot> fruityBasicExact (const std::string& pluginName, const std::vector<std::uint8_t>& state)
{
    // FL's own Fruity PanOMatic / Balance / Blood Overdrive engines from the saved state (REA_rodada58); the utility
    // and distortion approximations remain for states that cannot be read
    if (pluginName == "Fruity PanOMatic")
        if (const auto s = flbasic::parsePanOMatic (state))
        {
            auto slot = makeEffectSlot (EffectKind::fruityPanOMatic);
            flbasic::panOMaticToParams (*s, slot.params.data());
            return slot;
        }
    if (pluginName == "Fruity Balance")
        if (const auto s = flbasic::parseBalance (state))
        {
            auto slot = makeEffectSlot (EffectKind::fruityBalance);
            flbasic::balanceToParams (*s, slot.params.data());
            return slot;
        }
    if (pluginName == "Fruity Send") // the exact send to another insert (REA_rodada105_send)
        if (const auto s = flbasic::parseSend (state))
        {
            auto slot = makeEffectSlot (EffectKind::fruitySend);
            flbasic::sendToParams (*s, slot.params.data());
            return slot;
        }
    if (pluginName == "Fruity Blood Overdrive")
        if (const auto s = flbasic::parseBloodOverdrive (state))
        {
            auto slot = makeEffectSlot (EffectKind::fruityBloodOverdrive);
            flbasic::bloodOverdriveToParams (*s, slot.params.data());
            return slot;
        }
    return std::nullopt;
}

std::optional<EffectSlot> fruityDelayExact (const std::string& pluginName, const std::vector<std::uint8_t>& state)
{
    // FL's own Fruity Delay 2 / Delay 3 engines from the saved controls (REA_rodada52); the approximations of
    // REA_rodada7 remain for states that cannot be read (case-insensitive: FL 3 presets say "Fruity delay 2")
    std::string lower = pluginName;
    std::transform (lower.begin(), lower.end(), lower.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    if (lower == "fruity delay 2")
        if (const auto s = fldelay::parseDelay2 (state))
        {
            auto slot = makeEffectSlot (EffectKind::fruityDelay2);
            fldelay::delay2ToParams (*s, slot.params.data());
            return slot;
        }
    if (lower == "fruity delay 3")
        if (const auto s = fldelay::parseDelay3 (state))
        {
            auto slot = makeEffectSlot (EffectKind::fruityDelay3);
            fldelay::delay3ToParams (*s, slot.params.data());
            return slot;
        }
    return std::nullopt;
}

std::optional<EffectSlot> stereoFxExact (const std::string& pluginName, const std::vector<std::uint8_t>& state)
{
    // FL's own engines from the saved state (REA_rodada54); the earlier approximations remain for unreadable states
    if (pluginName == "Fruity Stereo Shaper")
        if (const auto s = stereofx::parseStereoShaper (state))
        {
            auto slot = makeEffectSlot (EffectKind::stereoShaper);
            stereofx::stereoShaperToParams (*s, slot.params.data());
            return slot;
        }
    if (pluginName == "Fruity Stereo Enhancer")
        if (const auto s = stereofx::parseStereoEnhancer (state))
        {
            auto slot = makeEffectSlot (EffectKind::stereoEnhancer);
            stereofx::stereoEnhancerToParams (*s, slot.params.data());
            return slot;
        }
    if (pluginName == "Transient Processor")
        if (const auto s = stereofx::parseTransientProcessor (state))
        {
            auto slot = makeEffectSlot (EffectKind::transientProcessor);
            stereofx::transientProcessorToParams (*s, slot.params.data());
            return slot;
        }
    if (pluginName == "Spreader")
        if (const auto s = stereofx::parseSpreader (state))
        {
            auto slot = makeEffectSlot (EffectKind::spreader);
            stereofx::spreaderToParams (*s, slot.params.data());
            return slot;
        }
    return std::nullopt;
}

std::optional<EffectSlot> fruityDelayBankExact (const std::vector<std::uint8_t>& state)
{
    // FL's own Delay Bank engine from the saved state (REA_rodada56); the generic delay remains for unreadable states
    const auto s = fldbank::parse (state);
    if (! s)
        return std::nullopt;
    auto slot = makeEffectSlot (EffectKind::fruityDelayBank);
    slot.state = fldbank::serialise (*s);
    for (int i = 0; i < fldbank::globalControls; ++i)
        slot.params[(std::size_t) i] = (float) std::clamp (s->p[(std::size_t) i], 0, 128) / 128.0f;
    return slot;
}

std::optional<EffectSlot> fruityMultibandExact (const std::vector<std::uint8_t>& state)
{
    // FL's own Multiband Compressor engine (REA_rodada56); the single-band approximation remains for unreadable states.
    // The slot keeps FL's state too, so that the exact integer controls survive the project file's rounding.
    const auto s = flmb::parse (state);
    if (! s)
        return std::nullopt;
    auto slot = makeEffectSlot (EffectKind::fruityMultiband);
    flmb::toParams (*s, slot.params.data());
    slot.state = state;
    return slot;
}

std::optional<EffectSlot> effectFromFl (const std::string& pluginName, const std::vector<std::uint8_t>& state, double bpm)
{
    // Newtone / Newtime: FL's state kept as it is; the source sample is resolved by the project import (REA_rodada73)
    // Formula Controller (REA_rodada133 §3): the knobs as parameters, the formula in the slot's state (FL's own bytes)
    if (pluginName == "Fruity Formula Controller")
        if (const auto parsed = formula::parseState (state))
        {
            auto slot = makeEffectSlot (EffectKind::formulaController);
            slot.params[0] = parsed->a;
            slot.params[1] = parsed->b;
            slot.params[2] = parsed->c;
            slot.state = state;
            return slot;
        }
    // X-Y / X-Y-Z Controller, Control Surface (REA_rodada164a): live controllers with FL's settings
    if (auto xyc = xy::fromFl (pluginName, state))
        return xyc;
    if (pluginName == "Control Surface")
        if (auto controls = surface::parseFlControlSurface (state))
        {
            auto slot = makeEffectSlot (EffectKind::controlSurface);
            slot.patcher.surface = std::move (*controls);
            surface::syncParams (slot);
            slot.state = state;
            return slot;
        }
    if (pluginName == "Newtone" || pluginName == "Newtime")
    {
        auto slot = makeEffectSlot (pluginName == "Newtone" ? EffectKind::newtone : EffectKind::newtime);
        if (const auto parsed = newtonefx::parse (state))
        {
            newtonefx::toParams (*parsed, slot.params);
            slot.state = state;
        }
        return slot;
    }
    if (pluginName == "Emphasizer") // REA_rodada154: the exact engine
        if (const auto s = emphasizer::parse (state))
        {
            auto slot = makeEffectSlot (EffectKind::emphasizer);
            emphasizer::toParams (*s, slot.params.data());
            return slot;
        }
    if (pluginName == "Wave Candy") // REA_rodada164c: LYNX's analyser in its role, in the display mode FL's state shows
    {
        auto slot = makeEffectSlot (EffectKind::waveCandy);
        visualfx::wavecandy::fromFl (state, slot);
        return slot;
    }
    if (pluginName == "ZGameEditor Visualizer") // REA_rodada164c: its layers mapped to LYNX Visualizer layers
    {
        auto slot = makeEffectSlot (EffectKind::visualizer);
        if (const auto scene = visualfx::scene::fromZge (state))
            slot.state = visualfx::scene::toState (*scene);
        return slot;
    }
    {
        // REA_rodada165k: the legacy Fruity Phase Inverter, Center, Bass Boost, 7 Band EQ and Delay (v1) on FL's own engines (the
        // generic approximations remain for states that cannot be read); FL 3 presets spell some names in lower case
        std::string lname = pluginName;
        std::transform (lname.begin(), lname.end(), lname.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
        auto exact = [] (EffectKind kind, const auto& s) {
            auto slot = makeEffectSlot (kind);
            fllegacy::toParams (s, slot.params.data());
            return slot;
        };
        if (lname == "fruity phase inverter")
            if (const auto s = fllegacy::parsePhaseInverter (state))
                return exact (EffectKind::flPhaseInverter, *s);
        if (lname == "fruity center")
            if (const auto s = fllegacy::parseCenter (state))
                return exact (EffectKind::flCenter, *s);
        if (lname == "fruity bass boost")
            if (const auto s = fllegacy::parseBassBoost (state))
                return exact (EffectKind::flBassBoost, *s);
        if (lname == "fruity 7 band eq")
            if (const auto s = fllegacy::parseEq7 (state))
                return exact (EffectKind::fl7BandEq, *s);
        if (lname == "fruity delay")
            if (const auto s = fllegacy::parseDelay1 (state))
                return exact (EffectKind::flDelay1, *s);
    }
    if (pluginName == "Low Lifter") // REA_rodada154 §5: FL's own engine
        if (const auto s = lowlifter::parse (state))
        {
            auto slot = makeEffectSlot (EffectKind::flLowLifter);
            lowlifter::toParams (*s, slot.params.data());
            return slot;
        }
    if (pluginName == "Frequency Shifter") // REA_rodada154 §4: FL's own engine
        if (const auto s = fsfl::parse (state))
        {
            auto slot = makeEffectSlot (EffectKind::flFrequencyShifter);
            fsfl::toParams (*s, slot.params.data());
            return slot;
        }
    if (auto exact = fruityDelayExact (pluginName, state))
        return exact;
    if (auto exact = fruityDynamicsExact (pluginName, state))
        return exact;
    if (auto exact = fruityBasicExact (pluginName, state))
        return exact;
    if (auto exact = fruityFilterExact (pluginName, state))
        return exact;
    if (auto exact = fruityShaperExact (pluginName, state))
        return exact;
    if (auto exact = stereoFxExact (pluginName, state))
        return exact;
    if (auto exact = chorusFromFl (pluginName, state)) // REA_rodada59; the generic chorus remains for unreadable states
        return exact;
    if (pluginName == "Fruity Delay Bank")
        if (auto exact = fruityDelayBankExact (state))
            return exact;
    if (pluginName == "Fruity Multiband Compressor")
        if (auto exact = fruityMultibandExact (state))
            return exact;
    // FL's own Fast Dist / Flangus engines from the saved controls (REA_rodada55); the approximations of REA_rodada8
    // remain for states that cannot be read
    if (pluginName == "Fruity Fast Dist")
        if (const auto s = fastdistfx::parse (state))
        {
            auto slot = makeEffectSlot (EffectKind::fruityFastDist);
            fastdistfx::toParams (*s, slot.params.data());
            return slot;
        }
    if (pluginName == "Fruity Flangus")
        if (const auto s = flangusfx::parse (state))
        {
            auto slot = makeEffectSlot (EffectKind::fruityFlangus);
            flangusfx::toParams (*s, slot.params.data());
            return slot;
        }
    if (pluginName == "Fruity Flanger")
        if (const auto s = flangerfx::parse (state))
        {
            auto slot = makeEffectSlot (EffectKind::fruityFlanger);
            flangerfx::toParams (*s, slot.params.data());
            return slot;
        }
    if (pluginName == "Fruity Phaser") // the exact engine (REA_rodada105_phaser); LYNX's generic chorus otherwise
        if (const auto s = phaserfx::parse (state))
        {
            auto slot = makeEffectSlot (EffectKind::fruityPhaser);
            phaserfx::toParams (*s, slot.params.data());
            return slot;
        }
    if (pluginName == "Maximus") // FL's own engine from the saved state (REA_rodada36/38); the approximation otherwise
        if (auto exact = maximusExact (state))
            return exact;
    if (pluginName == "Gross Beat") // FL's own engine from the saved slots (REA_rodada53); the volume gate otherwise
        if (auto exact = grossbeat::slotFromFl (state))
            return exact;
    if (pluginName == "Fruity Love Philter") // FL's own engine (REA_rodada66); lovePhilterEffects' conversion otherwise
        if (auto exact = philter::slotFromFl (state))
            return exact;
    if (pluginName == "Pitch Shifter") // the exact engines (REA_rodada61); LYNX's generic pitch shifter otherwise
        if (psfl::State ps; psfl::parsePitchShifter (state.data(), state.size(), ps))
        {
            auto slot = makeEffectSlot (EffectKind::flPitchShifter);
            psfl::toParams (ps, slot.params);
            return slot;
        }
    if (pluginName == "Hardcore") // the exact engine without the cabinet impulses (REA_rodada71)
        if (auto exact = hardcore::fromFl (state, {}))
            return exact;
    if (pluginName == "Distructor") // the exact modules (REA_rodada71); a generic distortion otherwise
        if (const auto s = distructor::parse (state))
        {
            auto slot = makeEffectSlot (EffectKind::distructor);
            slot.state = distructor::packBlob (state, {});
            distructor::toParams (*s, slot.params.data());
            return slot;
        }
    if (pluginName == "EQUO") // the exact engine (REA_rodada61); a flat parametric EQ otherwise
        if (const auto s = equofx::parse (state))
        {
            auto slot = makeEffectSlot (EffectKind::equo);
            slot.state = state;
            equofx::toParams (*s, slot.params);
            return slot;
        }
    if (pluginName == "Multiband Delay") // the exact engine (REA_rodada71); LYNX's generic delay otherwise
        if (const auto s = mbdelay::parse (state))
        {
            auto slot = makeEffectSlot (EffectKind::multibandDelay);
            slot.state = state;
            mbdelay::toParams (*s, slot.params);
            return slot;
        }
    if (pluginName == "Frequency Splitter") // FL's engine (REA_rodada62): its summed (main) output; shelves otherwise
        if (const auto s = fsplit::parseState (state))
        {
            auto slot = makeEffectSlot (EffectKind::frequencySplitter);
            fsplit::toParams (*s, slot.params.data());
            return slot;
        }
    if (pluginName == "Effector")
        return effector (state, bpm);
    if (pluginName == "Fruity Vocoder")
        return fruityVocoder (state, nullptr);
    if (pluginName == "Vocodex")
        if (auto v = vocodexEffect (state))
            return v->slot;
    if (pluginName == "Vintage Phaser") // the exact effect (REA_rodada29 §3)
        return vintagePhaserFromFl (state);
    if (pluginName == "Fruity Reeverb 2") // the exact effect (REA_rodada49); the Freeverb mapping below otherwise
        if (auto exact = reeverb2FromFl (state))
            return exact;
    if (pluginName == "LuxeVerb") // the exact effect (REA_rodada60); the generic reverb mapping below otherwise
        if (auto exact = luxeVerbFromFl (state))
            return exact;
    if (pluginName == "Fruity Convolver") // the exact effect with an embedded impulse response (REA_rodada60)
        if (auto exact = convolverFromFl (state))
            return exact;
    if (pluginName == "Fruity Reeverb") // the first version, exact (REA_rodada49 §6)
        if (auto exact = reeverb1FromFl (state))
            return exact;
    if (pluginName == "Pitcher")
    {
        // "SSLF" + u32 + "1RTP" + 33 f32 (raw parameters 0…32) + 12 B note mask + f32 (REA_rodada19 §6)
        if (state.size() < 12 + 33 * 4 + 12 || std::memcmp (state.data(), "SSLF", 4) != 0 || std::memcmp (state.data() + 8, "1RTP", 4) != 0)
            return std::nullopt;
        float p[33];
        std::memcpy (p, state.data() + 12, sizeof (p));
        for (auto& v : p)
            v = std::isfinite (v) ? v : 0.0f;
        auto slot = makeEffectSlot (EffectKind::pitchCorrect);
        slot.params[0] = std::clamp (p[0], 0.0f, 1.0f);
        slot.params[1] = std::clamp (p[3], 0.0f, 1.0f);
        slot.params[2] = p[4] != 0.0f ? 1.0f : 0.0f;
        slot.params[3] = std::clamp (p[23], 0.0f, 1.0f);
        slot.params[4] = (float) std::clamp ((int) p[32], 0, 5) / 5.0f;
        const auto* mask = state.data() + 12 + 33 * 4;
        for (int k = 0; k < 12; ++k)
        {
            slot.params[(std::size_t) (5 + k)] = mask[k] != 0 ? 1.0f : 0.0f;
            slot.params[(std::size_t) (17 + k)] = p[5 + k] != 0.0f ? 1.0f : 0.0f;
        }
        // Formant/Gender, Pitch Reference, harmony and MIDI (REA_rodada30 §2; MIDI notes do not reach LYNX effects)
        auto flag = [&p] (int i) { return p[i] != 0.0f ? 1.0f : 0.0f; };
        slot.params[29] = flag (22);
        slot.params[30] = std::clamp (p[2], 0.0f, 1.0f);
        slot.params[31] = std::clamp (p[24], 0.0f, 1.0f);
        slot.params[32] = std::clamp (p[1], 0.0f, 1.0f);
        slot.params[33] = flag (27);
        slot.params[34] = flag (25);
        slot.params[35] = flag (26);
        slot.params[36] = flag (30);
        slot.params[37] = flag (31);
        for (int j = 0; j < 4; ++j)
            slot.params[(std::size_t) (38 + j)] = std::clamp (p[17 + j], 0.0f, 1.0f);
        return slot;
    }

    const auto kind = effectForFlPlugin (pluginName);

    if (! kind)
        return std::nullopt;

    auto slot = makeEffectSlot (*kind);

    // case-insensitive compare (FL 3 presets: "Fruity delay 2")
    std::string lower = pluginName;
    std::transform (lower.begin(), lower.end(), lower.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    auto is = [&] (const char* canonical) {
        std::string other = canonical;
        std::transform (other.begin(), other.end(), other.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
        return lower == other;
    };

    if (*kind == EffectKind::filter)
        slot.params[1] = 1.0f; // open: without FL's settings, better not to muffle

    if (is ("Fruity Soft Clipper"))
        softClipper (state, slot);

    if (is ("Fruity Parametric EQ 2"))
        parametricEq2 (state, slot);
    else if (is ("Fruity Parametric EQ"))
    {
        if (const auto exact = peqfl::parsePeq1 (state)) // REA_rodada50 §3
            peqfl::toParams (*exact, slot.params);
    }
    else if (is ("Fruity Limiter"))
        limiter (state, slot);
    else if (is ("Fruity Reeverb 2"))
        reeverb2Approx (state, slot, bpm);
    else if (is ("Fruity Delay 2"))
        delay2 (state, slot);
    else if (is ("Fruity Delay 3"))
        delay3 (state, slot);
    else if (is ("Fruity Compressor"))
        compressor (state, slot);
    else if (is ("Fruity Balance"))
        balance (state, slot);
    else if (is ("Fruity PanOMatic"))
        panOMatic (state, slot);
    else if (is ("Fruity Stereo Shaper"))
        stereoShaper (state, slot);
    else if (is ("Maximus"))
        maximus (state, slot);
    else if (is ("Soundgoodizer"))
    {
        // the compressor approximation followed by its presets' master limiter (REA_rodada8 §2; FL's own SG DLL checked in REA_rodada35),
        // as one serial branch
        if (const auto sg = maximusfx::parseSoundgoodizer (state))
            slot = soundgoodizerApproxSlot (sg->preset, sg->amount);
    }
    else if (is ("Fruity Fast Dist"))
        fastDist (state, slot);
    else if (is ("Fruity Filter"))
        fruityFilter (state, slot);
    else if (is ("Fruity Flangus"))
        flangus (state, slot);
    else if (is ("Gross Beat"))
        grossBeat (state, slot);
    else if (is ("Fruity Peak Controller"))
        peakController (state, slot);
    else if (is ("Pitch Shifter"))
        pitchShifter (state, slot);
    else if (is ("Frequency Shifter"))
        frequencyShifter (state, slot);
    else if (is ("Emphasis"))
        emphasis (state, slot);
    else if (is ("Low Lifter"))
        lowLifter (state, slot);
    else if (is ("Fruity Send"))
        fruitySend (state, slot);
    else if (is ("Transporter"))
        transporter (state, slot);
    else if (is ("Fruity Squeeze"))
        squeeze (state, slot);
    else if (is ("Frequency Splitter"))
        frequencySplitter (state, slot);
    else if (is ("Fruity Phase Inverter"))
    {
        slot.params[2] = utilityMatrixValue (-1.0); // inverts both sides
        slot.params[5] = utilityMatrixValue (-1.0);
    }

    return slot;
}
std::optional<EffectSlot> effectFromFlPreset (const std::vector<std::uint8_t>& bytes, double bpm)
{
    try
    {
        // the effect preset has the same format as the channel one: plugin name (0xC9) and state (0xD5)
        const auto project = flp::parseProject (bytes, true);

        if (project.channels.empty() || project.channels.front().pluginState.empty())
            return std::nullopt;

        return effectFromFl (project.channels.front().pluginName, project.channels.front().pluginState, bpm);
    }
    catch (const flp::Error&)
    {
        return std::nullopt;
    }
}

std::vector<std::string> flPluginsForEffect (EffectKind kind)
{
    switch (kind)
    {
        case EffectKind::parametricEq: return { "Fruity Parametric EQ 2" };
        case EffectKind::fruityLimiter: return { "Fruity Limiter" };
        case EffectKind::fruityCompressor: return { "Fruity Compressor" };
        case EffectKind::fruityWaveShaper: return { "Fruity WaveShaper" };
        case EffectKind::fruitySoftClipper: return { "Fruity Soft Clipper" };
        case EffectKind::fruityChorus: return { "Fruity Chorus" };
        case EffectKind::hyperChorus: return { "Hyper Chorus" };
        case EffectKind::vintageChorus: return { "Vintage Chorus" };
        case EffectKind::fruityPanOMatic: return { "Fruity PanOMatic" };
        case EffectKind::fruityBalance: return { "Fruity Balance" };
        case EffectKind::fruitySend: return { "Fruity Send" };
        case EffectKind::fruityBloodOverdrive: return { "Fruity Blood Overdrive" };
        case EffectKind::fruityFastDist: return { "Fruity Fast Dist" };
        case EffectKind::fruityFlangus: return { "Fruity Flangus" };
        case EffectKind::fruityFlanger: return { "Fruity Flanger" };
        case EffectKind::fruityPhaser: return { "Fruity Phaser" };
        case EffectKind::fruityFastLP: return { "Fruity Fast LP" };
        case EffectKind::fruityFreeFilter: return { "Fruity Free Filter" };
        case EffectKind::fruityFilter: return { "Fruity Filter" };
        case EffectKind::newtone: return { "Newtone" };
        case EffectKind::newtime: return { "Newtime" };
        case EffectKind::facetEq:
        case EffectKind::crestCompressor:
        case EffectKind::prismMultiband:
        case EffectKind::sentryGate:
        case EffectKind::quellDenoiser:
        case EffectKind::geminiDoubler:
        case EffectKind::apexLimiter: return {};
        case EffectKind::formulaController: return { "Fruity Formula Controller" }; // LYNX's own effects: no FL counterpart
        case EffectKind::voiceShift: return {}; // LYNX's own effect: no FL counterpart
        case EffectKind::hardwareInsert: return {}; // LYNX's own effect (round 164d): FL has none
        case EffectKind::emphasizer: return { "Emphasizer" };
        case EffectKind::flFrequencyShifter: return { "Frequency Shifter" };
        case EffectKind::flLowLifter: return { "Low Lifter" };
        case EffectKind::flPhaseInverter: return { "Fruity Phase Inverter" };
        case EffectKind::flCenter: return { "Fruity Center" };
        case EffectKind::flBassBoost: return { "Fruity Bass Boost" };
        case EffectKind::fl7BandEq: return { "Fruity 7 Band EQ" };
        case EffectKind::flDelay1: return { "Fruity Delay" };
        case EffectKind::waveCandy: return { "Wave Candy" };
        case EffectKind::visualizer: return { "ZGameEditor Visualizer" };
        case EffectKind::xyController: return { "Fruity X-Y Controller" };
        case EffectKind::xyzController: return {}; // (FL's X-Y-Z Controller: name from REA_rodada164a)
        case EffectKind::controlSurface: return { "Control Surface" };
        case EffectKind::limiter:
        case EffectKind::compressor: return { "Fruity Limiter", "Fruity Compressor" };
        case EffectKind::reverb:
        case EffectKind::reeverb2: return { "Fruity Reeverb 2" };
        case EffectKind::luxeVerb: return { "LuxeVerb" };
        case EffectKind::maximus: return { "Maximus" }; // (a Soundgoodizer slot has its own presets A–D)
        case EffectKind::convolver: return { "Fruity Convolver" };
        case EffectKind::delay: return { "Fruity Delay 3", "Fruity Delay 2" };
        case EffectKind::fruityDelay2: return { "Fruity Delay 2" };
        case EffectKind::fruityDelay3: return { "Fruity Delay 3" };
        case EffectKind::utility:
        case EffectKind::stereoShaper: return { "Fruity Stereo Shaper" };
        case EffectKind::stereoEnhancer: return { "Fruity Stereo Enhancer" };
        case EffectKind::transientProcessor: return { "Transient Processor" };
        case EffectKind::spreader: return { "Spreader" };
        case EffectKind::fruityDelayBank: return { "Fruity Delay Bank" };
        case EffectKind::fruityMultiband: return { "Fruity Multiband Compressor" };
        case EffectKind::equo: return { "EQUO" };
        case EffectKind::hardcore: return { "Hardcore" };
        case EffectKind::distructor: return { "Distructor" };
        case EffectKind::flPitchShifter: return { "Pitch Shifter" };
        case EffectKind::multibandDelay: return { "Multiband Delay" };
        case EffectKind::vocoder: return { "Fruity Vocoder" };
        case EffectKind::vocodex: return { "Vocodex" };
        case EffectKind::effector: return { "Effector" };
        case EffectKind::frequencySplitter: return { "Frequency Splitter" };
        default: return {};
    }
}
std::optional<FlParamMap> effectParamFromFl (const std::string& pluginName, int k)
{
    auto is = [&] (const char* n) { return pluginName == n; };

    // the exact Fruity Limiter / Compressor take FL's controls as they are (value over the control's range)
    if (is ("Fruity Limiter") && k >= 0 && k < fldyn::limiterControls)
        return FlParamMap { k, [] (float v) { return std::clamp (v, 0.0f, 1.0f); } };
    if (is ("Fruity Compressor") && k >= 0 && k < fldyn::compressorControls)
        return FlParamMap { k, [] (float v) { return std::clamp (v, 0.0f, 1.0f); } };
    // the exact WaveShaper (pre, mix, post) and Soft Clipper (threshold, post) take FL's controls over their ranges
    if ((is ("Fruity WaveShaper") && k >= 0 && k < 3) || (is ("Fruity Soft Clipper") && k >= 0 && k < 2))
        return FlParamMap { k, [] (float v) { return std::clamp (v, 0.0f, 1.0f); } };
    // the exact PanOMatic, Balance and Blood Overdrive likewise (REA_rodada58)
    if ((is ("Fruity PanOMatic") && k >= 0 && k < flbasic::panOMaticParams)
        || (is ("Fruity Balance") && k >= 0 && k < flbasic::balanceParams)
        || (is ("Fruity Blood Overdrive") && k >= 0 && k < flbasic::bloodOverdriveParams)
        || (is ("Fruity Send") && k >= 0 && k < 3)) // (pan, volume, dry; the insert is not a parameter in FL)
        return FlParamMap { k, [] (float v) { return std::clamp (v, 0.0f, 1.0f); } };
    // Hardcore's 50 parameters: FL sends 0…1 for all of them (the box types and the cabinet as raw numbers, REA_rodada71)
    if (is ("Hardcore") && k >= 0 && k < hardcore::numParams)
        return FlParamMap { k, [k] (float v) { return hardcore::toSlot (k, std::clamp (v, 0.0f, 1.0f)); } };
    // Distructor: module m's controls 36…38 (FL parameter 49m + 36…38) → slot parameters 3m…3m+2
    if (is ("Distructor") && k >= 0 && k < 4 * distructor::moduleParams && k % distructor::moduleParams >= 36
        && k % distructor::moduleParams <= 38)
        return FlParamMap { 3 * (k / distructor::moduleParams) + k % distructor::moduleParams - 36,
                            [] (float v) { return std::clamp (v, 0.0f, 1.0f); } };
    // EQUO's five knobs (shift, mix, morph, smoothness, volume) over their ranges; band automation is not mapped
    // Multiband Delay's twelve global controls (FL parameters 384…395) over their ranges; bank cells are not mapped
    if (is ("Multiband Delay") && k >= 384 && k < 396)
        return FlParamMap { k - 384, [] (float v) { return std::clamp (v, 0.0f, 1.0f); } };
    if (is ("EQUO") && k >= 0 && k < 5)
        return FlParamMap { k, [] (float v) { return std::clamp (v, 0.0f, 1.0f); } };
    // the exact Fast LP, Free Filter and Fruity Filter likewise (REA_rodada63; the Free Filter's CENTER and the
    // Filter's CENTER are buttons, not parameters)
    if ((is ("Fruity Fast LP") && k >= 0 && k < flfilter::fastLPParams)
        || (is ("Fruity Free Filter") && k >= 0 && k < 4)
        || (is ("Fruity Filter") && k >= 0 && k < 6))
        return FlParamMap { k, [] (float v) { return std::clamp (v, 0.0f, 1.0f); } };
    // the exact choruses take FL's controls over their ranges
    if (chorusControls (pluginName) > k && k >= 0)
        return FlParamMap { k, [] (float v) { return std::clamp (v, 0.0f, 1.0f); } };
    // the exact Fruity Delay 2 / 3 likewise (every control)
    if (is ("Fruity Delay 2") && k >= 0 && k < fldelay::delay2Controls)
        return FlParamMap { k, [] (float v) { return std::clamp (v, 0.0f, 1.0f); } };
    if (is ("Fruity Delay 3") && k >= 0 && k < fldelay::delay3Controls)
        return FlParamMap { k, [] (float v) { return std::clamp (v, 0.0f, 1.0f); } };
    // the exact Stereo Shaper takes FL's controls over their ranges; FL's parameter 7 (effects order) is our 6
    if (is ("Fruity Stereo Shaper") && ((k >= 0 && k < 6) || k == 7))
        return FlParamMap { k == 7 ? 6 : k, [] (float v) { return std::clamp (v, 0.0f, 1.0f); } };
    // the exact Stereo Enhancer and Transient Processor take every FL control over its range
    if ((is ("Fruity Stereo Enhancer") && k >= 0 && k < 6) || (is ("Transient Processor") && k >= 0 && k < 9)
        || (is ("Spreader") && k >= 0 && k < 5))
        return FlParamMap { k, [] (float v) { return std::clamp (v, 0.0f, 1.0f); } };
    // the exact Delay Bank: its four global controls (0…128) are the slot's parameters
    if (is ("Fruity Delay Bank") && k >= 0 && k < fldbank::globalControls)
        return FlParamMap { k, [] (float v) { return std::clamp (v, 0.0f, 1.0f); } };
    if (is ("Fruity Multiband Compressor") && k >= 0 && k < flmb::controls) // FL's controls as they are (value over range)
    // the exact Fast Dist / Flangus / Flanger take FL's controls over their ranges
    if ((is ("Fruity Fast Dist") && k >= 0 && k < fastdistfx::params) || (is ("Fruity Flangus") && k >= 0 && k < flangusfx::params)
        || (is ("Fruity Flanger") && k >= 0 && k < flangerfx::params))
        return FlParamMap { k, [] (float v) { return std::clamp (v, 0.0f, 1.0f); } };
    // the exact Fruity Phaser likewise (its nine controls; FL normalises them over their ranges)
    if (is ("Fruity Phaser") && k >= 0 && k < phaserfx::params)
        return FlParamMap { k, [] (float v) { return std::clamp (v, 0.0f, 1.0f); } };

    if (is ("Fruity Parametric EQ 2"))
    {
        if (k >= 0 && k < 7) // band gain: ±18 dB
            return FlParamMap { peq::param (k, 2), [] (float v) { return peq::gainValue (36.0 * v - 18.0); } };
        if (k >= 7 && k < 14) // frequency: 20 Hz · 1000^v
            return FlParamMap { peq::param (k - 7, 1), [] (float v) { return peq::hzValue (20.0 * std::pow (1000.0, (double) v)); } };
        if (k >= 14 && k < 21) // bandwidth: the imported slot keeps FL's own width (core/ParametricEqFl.h)
            return FlParamMap { peq::param (k - 14, 3), [] (float v) { return std::clamp (v, 0.0f, 1.0f); } };
        if (k == 35)
            return FlParamMap { peq::outputParam, [] (float v) { return peq::gainValue (36.0 * v - 18.0); } };
        return std::nullopt;
    }

    if (is ("Fruity Filter"))
    {
        if (k == 0) // cutoff (Chamberlin SVF, as in the state import)
            return FlParamMap { 1, [] (float v) {
                const auto f = std::min (1.0, (std::pow (26.0, (double) v) - 1.0) * 0.04);
                return fromLog (std::max (20.0, 44100.0 / 3.141592653589793 * std::asin (f / 2.0)), 20.0, 20000.0);
            } };
        if (k == 1)
            return FlParamMap { 2, [] (float v) { return fromLog (1.0 / (1.0 - 0.8 * (double) v), 0.5, 12.0); } };
        return std::nullopt;
    }

    if (is ("Fruity Balance") || is ("Fruity PanOMatic"))
    {
        if (k == 0) // pan: the full range of both matches ours
            return FlParamMap { 1, [] (float v) { return v; } };
        if (k == 1) // volume: 0..320 (256 = 0 dB) [HIP for the maximum]
            return FlParamMap { 0, [] (float v) { return utilityGainValue (curve (11.0, 1.25 * (double) v)); } };
        return std::nullopt;
    }

    if (is ("Fruity Fast LP") && (k == 0 || k == 1)) // cutoff / resonance: the built-in filter has the defaults, follow the knob
        return FlParamMap { k + 1, [] (float v) { return v; } };

    if (is ("Fruity Reeverb 2"))
    {
        if (k == 5) // decay 0.1..20 s -> comb feedback
            return FlParamMap { 0, [] (float v) {
                const auto decay = 0.1 + 19.9 * (double) v;
                return clamp01 ((std::pow (10.0, -3.0 * 0.03 / decay) - 0.7) / 0.28);
            } };
        if (k == 10)
            return FlParamMap { 4, [] (float v) { return clamp01 (curve (11.0, (double) v)); } };
        if (k == 12)
            return FlParamMap { 3, [] (float v) { return clamp01 (curve (11.0, (double) v) / 0.25); } };
        return std::nullopt;
    }

    if (is ("Pitch Shifter") && k >= 0 && k < 13) // the exact effect takes FL's 13 knobs over their ranges (REA_rodada61)
        return FlParamMap { k, [] (float v) { return std::clamp (v, 0.0f, 1.0f); } };
    if (is ("Pitch Shifter"))
    {
        if (k == 0) // pitch: −12..+12 st (in cents) -> our ±24 st
            return FlParamMap { 0, [] (float v) { return (float) clamp01 (0.5 + std::round (24.0 * v - 12.0) / 48.0); } };
        if (k == 3)
            return FlParamMap { 2, [] (float v) { return v; } };
        return std::nullopt;
    }

    if (is ("Fruity Delay 2") && k == 1) // wet
        return FlParamMap { 2, [] (float v) { return clamp01 (curve (11.0, (double) v) * 0.5); } };

    if (is ("Fruity Delay 3"))
    {
        if (k == 14)
            return FlParamMap { 1, [] (float v) { return clamp01 (v * 0.9); } };
        if (k == 23)
            return FlParamMap { 2, [] (float v) { return clamp01 (v * 0.5); } };
        if (k == 16)
            return FlParamMap { 4, [] (float v) { return clamp01 (1.0 - v); } };
    }

    return std::nullopt;
}
std::optional<float> controllerConstantOutput (const std::string& pluginName, const std::vector<std::uint8_t>& d, int output)
{
    if (pluginName == "Fruity X-Y Controller")
    {
        if (d.size() < 44 || output < 0 || output > 1)
            return std::nullopt;
        auto param = [&] (int i) { return std::clamp (i32At (d, 4 + 4 * (std::size_t) i), -65536, 65536) / 65536.0; };
        const auto axis = output * 4;
        const auto level = std::clamp (param (axis), 0.0, 1.0);
        // tension (REA_rodada17 §4): k = 0.1·(1001^(|t|/128) − 1); t > 0 concave, t < 0 convex
        const auto t = std::clamp (i32At (d, 4 + 4 * (std::size_t) (axis + 3)), -128, 128);
        auto shaped = level;
        if (t != 0)
        {
            const auto k = 0.1 * (std::pow (1001.0, std::abs (t) / 128.0) - 1.0);
            shaped = t > 0 ? 1.0 - (std::pow (k + 1.0, 1.0 - level) - 1.0) / k : (std::pow (k + 1.0, level) - 1.0) / k;
        }
        return (float) std::clamp (param (axis + 1) + (param (axis + 2) - param (axis + 1)) * shaped, 0.0, 1.0);
    }

    if (pluginName == "Control Surface")
    {
        if (d.size() < 16)
            return std::nullopt;
        std::optional<float> found;
        // chunks u32 id + u64 size (id 0 = group); 0x836 = a control's value and the published output
        std::function<void (std::size_t, std::size_t)> walk = [&] (std::size_t pos, std::size_t end) {
            while (pos + 12 <= end && ! found)
            {
                std::uint32_t id = 0;
                std::uint64_t size = 0;
                std::memcpy (&id, d.data() + pos, 4);
                std::memcpy (&size, d.data() + pos + 4, 8);
                if (size > end - pos - 12)
                    return;
                const auto body = pos + 12;
                if (id == 0)
                    walk (body, body + (std::size_t) size);
                else if (id == 0x836 && size >= 12 && i32At (d, body + 8) == output)
                {
                    float v = 0.0f;
                    std::memcpy (&v, d.data() + body, 4);
                    if (std::isfinite (v))
                        found = std::clamp (v, 0.0f, 1.0f);
                }
                pos = body + (std::size_t) size;
            }
        };
        walk (4, d.size());
        return found;
    }

    return std::nullopt;
}
namespace
{
// FL knob gain curve: (11^x − 1)/10, signed
double scurve11 (double x) { return (x < 0.0 ? -1.0 : 1.0) * (std::pow (11.0, std::abs (x)) - 1.0) / 10.0; }

struct PhilterBand
{
    peq::BandType type = peq::BandType::off;
    double hz = 1000.0, q = 0.707, gainDb = 0.0;
    int stages = 1;
    bool ok = false;
    std::vector<double> stageHz; // alternate modes: cutoff of each stage
};

// type, cutoff and Q of a unit with cutoff c and resonance r (0..1), at 44.1 kHz (k = 1)
PhilterBand philterBand (const LovePhilterUnit& u, double c, double r)
{
    PhilterBand b;
    c = std::clamp (c, 0.0, 1.0);
    r = std::clamp (r, 0.0, 1.0);
    const auto mode = std::clamp (u.mode, 0, 5);
    if (mode == 0)
        return b; // the unit does not filter
    b.stages = mode == 4 ? 2 : mode == 5 ? 3 : mode;
    auto lpQ = [] (double rr) { return 0.1 + 0.02495 * (std::pow (2001.0, rr) - 1.0); };
    const auto lowGain = scurve11 (std::clamp (u.params[3], 0, 128) / 128.0);
    const auto bandGain = scurve11 (std::clamp (u.params[4], 0, 128) / 128.0);
    const auto highGain = scurve11 (std::clamp (u.params[5], 0, 128) / 128.0);
    double x = 0.0;
    std::function<double (double)> hzOf;

    switch (u.type)
    {
        case 0: // Chamberlin SVF: fc = (x + 0.005)·44100/π; output = L·LP + B·BP + H·HP
        {
            x = (std::pow (51.0, c) - 1.0) * 0.0159;
            hzOf = [] (double v) { return (v + 0.005) * 44100.0 / 3.141592653589793; };
            b.hz = hzOf (x);
            auto d = r <= 0.0 ? 1.0 : r >= 1.0 ? 0.2 : 1.0 - 0.9 * r;
            if (u.alt)
                d *= d;
            b.q = 1.0 / std::max (0.05, d);
            const auto l = std::abs (lowGain) > 1e-6, bp = std::abs (bandGain) > 1e-6, h = std::abs (highGain) > 1e-6;
            // only the pure outputs have an equivalent: LP, BP, HP or L+H (band-reject) with the same gain
            if (l && ! bp && ! h)
                b.type = peq::BandType::highCut, b.gainDb = 20.0 * std::log10 (std::abs (lowGain));
            else if (h && ! l && ! bp)
                b.type = peq::BandType::lowCut, b.gainDb = 20.0 * std::log10 (std::abs (highGain));
            else if (bp && ! l && ! h)
                b.type = peq::BandType::bandPass, b.gainDb = 20.0 * std::log10 (std::abs (bandGain));
            else if (l && h && ! bp && std::abs (lowGain - highGain) < 1e-6)
                b.type = peq::BandType::notch, b.gainDb = 20.0 * std::log10 (std::abs (lowGain));
            else
                return b;
            b.ok = true;
            break;
        }
        case 1: case 2: // Lime: ω = (x + 0.007)·π/2 -> fc = (x + 0.007)·44100/4
        {
            x = std::min (1.6, (std::pow (76.0, c) - 1.0) * 0.01324);
            hzOf = [] (double v) { return (v + 0.007) * 44100.0 / 4.0; };
            b.hz = hzOf (x);
            b.q = std::min (50.0, std::pow (1001.0, r));
            b.type = u.type == 1 ? peq::BandType::highCut : peq::BandType::lowCut; // the L slot is "drive" here: left out
            b.ok = true;
            break;
        }
        case 3: // Vanilla LP: fc = (x + 0.003)·44100
        {
            x = (std::pow (1501.0, c) - 1.0) * 0.000664667;
            hzOf = [] (double v) { return (v + 0.003) * 44100.0; };
            b.hz = hzOf (x);
            b.q = lpQ (r);
            b.type = peq::BandType::highCut;
            b.ok = true;
            break;
        }
        case 6: case 8: case 9: case 10: // Vanilla 4…10 family: fc = (x + 0.0001)·44100
        {
            x = std::min (1.0001, (std::pow (1501.0, c) - 1.0) * 0.00039993);
            if (u.type == 6)
                x = std::min (x, 0.6001);
            hzOf = [] (double v) { return (v + 0.0001) * 44100.0; };
            b.hz = hzOf (x);
            if (u.type == 6)
            {
                b.type = peq::BandType::lowCut;
                b.q = lpQ (r);
            }
            else if (u.type == 9)
            {
                b.type = peq::BandType::peak;
                b.q = 0.1 + 0.04995 * (std::pow (2001.0, 1.0 - r) - 1.0);
                b.gainDb = 20.0 * std::log10 (std::max (1e-6, std::abs (lowGain))); // the peak gain is the low/gain slot
            }
            else
            {
                b.type = u.type == 8 ? peq::BandType::lowShelf : peq::BandType::highShelf;
                b.q = 0.1 + 0.45 * (std::pow (3.0, 1.0 - r) - 1.0); // shelf slope S
                b.gainDb = 20.0 * std::log10 (std::max (1e-6, std::abs (lowGain)));
            }
            b.ok = true;
            break;
        }
        case 4: case 5: // Vanilla band-pass / band-reject: width in octaves (REA_rodada19 §7.1)
        {
            x = std::min (1.0001, (std::pow (1501.0, c) - 1.0) * 0.00039993);
            hzOf = [] (double v) { return (v + 0.0001) * 44100.0; };
            b.hz = hzOf (x);
            const auto n = std::max (1, mode);
            auto bw = 0.01 + 0.1199 * (std::pow (101.0, u.alt ? r * (c + 1.0) * 0.5 : r) - 1.0);
            bw = u.type == 4 ? bw * n : bw / n; // per stage
            const auto w = std::clamp (2.0 * 3.141592653589793 * b.hz / 44100.0, 1e-4, 2.8495);
            b.q = std::clamp (1.0 / (2.0 * std::sinh (std::log (2.0) / 2.0 * bw * w / std::sin (w))), 0.1, 18.0);
            b.type = u.type == 4 ? peq::BandType::bandPass : peq::BandType::notch;
            b.ok = true;
            if (b.stages > 1) // the width was already divided per stage: don't redo the Q root below
            {
                b.hz = std::clamp (b.hz, 20.0, 20000.0);
                return b;
            }
            break;
        }
        case 7: // all-pass: only changes phase; alone in the unit, the magnitude is 1 (REA_rodada19 §7.2)
            b.type = peq::BandType::off;
            b.ok = true;
            return b;
        case 11: // Mango: 4 poles with feedback; the cutoff in Hz is inferred from the pole (REA_rodada19 §7.3)
        {
            x = std::min (3.0, (std::pow (201.0, c) - 1.0) * 0.01495);
            hzOf = [] (double v) {
                const auto w = std::min ((v + 0.01) * 0.5, 0.65);
                return 22050.0 * w * (1.873 * w * w * w + 0.4955 * w * w - 0.649 * w + 0.9988);
            };
            b.hz = hzOf (x);
            b.q = lpQ (r);
            b.type = peq::BandType::highCut;
            b.stages = std::min (4, 2 * b.stages); // 4 poles (24 dB/oct) per stage
            b.ok = true;
            break;
        }
        default:
            return b;
    }
    if (b.stages > 1)
        b.q = std::pow (b.q, 1.0 / b.stages);
    // alternate (modes 4/5): each stage's frequency variable is x, x^E, x^(E²), E = 0.5 / 0.75
    if ((mode == 4 || mode == 5) && hzOf)
    {
        const auto e = mode == 4 ? 0.5 : 0.75;
        auto stageX = x;
        for (int i = 0; i < b.stages; ++i)
        {
            b.stageHz.push_back (std::clamp (hzOf (stageX), 20.0, 20000.0));
            stageX = std::pow (std::max (0.0, stageX), e);
        }
    }
    b.hz = std::clamp (b.hz, 20.0, 20000.0);
    return b;
}

EffectSlot philterSlot (const PhilterBand& band, double unitGain)
{
    auto slot = makeEffectSlot (EffectKind::parametricEq);
    for (int i = 0; i < peq::bands; ++i)
        slot.params[(std::size_t) peq::param (i, 0)] = peq::typeValue (peq::BandType::off);
    double outputDb = 20.0 * std::log10 (std::max (1e-6, unitGain));
    if (band.ok)
    {
        const auto cut = band.type == peq::BandType::highCut || band.type == peq::BandType::lowCut;
        const auto alternate = ! band.stageHz.empty();
        // cuts: stages = EQ slope; the other types (and the alternate ones) repeat the band (cascade)
        const auto copies = cut && ! alternate ? 1 : std::min (band.stages, peq::bands);
        for (int i = 0; i < copies; ++i)
        {
            slot.params[(std::size_t) peq::param (i, 0)] = peq::typeValue (band.type);
            slot.params[(std::size_t) peq::param (i, 1)] = peq::hzValue (alternate ? band.stageHz[(std::size_t) i] : band.hz);
            slot.params[(std::size_t) peq::param (i, 2)] = peq::gainValue (std::clamp (band.gainDb, -18.0, 18.0));
            slot.params[(std::size_t) peq::param (i, 3)] = peq::qValue (band.q);
            slot.params[(std::size_t) (peq::slopeParam + i)] = peq::slopeValue (cut && ! alternate ? std::clamp (band.stages, 1, 4) : 1);
        }
        if (cut) // for cuts the L/H slot gain multiplies the output
            outputDb += band.gainDb;
    }
    slot.params[peq::outputParam] = peq::gainValue (std::clamp (outputDb, -18.0, 18.0));
    return slot;
}
} // namespace

std::optional<LovePhilterConversion> lovePhilterEffects (const std::vector<std::uint8_t>& stateBytes)
{
    const auto state = parseLovePhilter (stateBytes);
    if (! state)
        return std::nullopt;

    const auto master = scurve11 (std::clamp (state->globals[0], 0, 160) / 128.0);
    std::vector<int> active;
    for (int u = 0; u < 8; ++u)
        if (state->units[(std::size_t) u].enabled && (state->soloUnit < 0 || state->soloUnit == u))
            active.push_back (u);

    LovePhilterConversion out;
    // volume articulator (multiplicative, REA_rodada17 §15.3 + rodada18 §4): base × mapX(x) × mapY(y);
    // disabled envelope/LFO are neutral (enabled ones are left out)
    const auto modX = std::clamp (state->modX, 0, 1024) / 1024.0, modY = std::clamp (state->modY, 0, 1024) / 1024.0;
    auto volumeFactor = [] (const LovePhilterUnit& unit, double x, double y) {
        const auto& a = unit.articulators[1];
        const auto base = std::isfinite (a.base) ? std::clamp ((double) a.base, 0.0, 4.0) : 1.0;
        return std::max (0.0, base * lovePhilterMapAt (a.mapX, x) * lovePhilterMapAt (a.mapY, y));
    };
    auto mapsNeutral = [] (const LovePhilterMap& m) {
        return std::all_of (m.points.begin(), m.points.end(), [] (const auto& pt) { return std::abs (pt.second - 1.0) < 1e-6; });
    };
    for (const auto u : active)
        if (u + 1 < 8 && state->units[(std::size_t) u + 1].enabled && state->units[(std::size_t) u].params[11] != 0)
        {
            out.notes.push_back ("Love Philter: chained units (NEXT) play in parallel");
            break;
        }

    auto unitEffects = [&] (int u) {
        const auto& unit = state->units[(std::size_t) u];
        const auto band = philterBand (unit, std::clamp (unit.params[1], 0, 256) / 256.0, std::clamp (unit.params[2], 0, 256) / 256.0);
        if (unit.mode != 0 && ! band.ok)
            out.notes.push_back ("Love Philter: filter type " + std::to_string (unit.type) + " has no equivalent");
        const auto gain = scurve11 (std::clamp (unit.params[9], 0, 160) / 128.0) * master * std::abs (scurve11 (std::clamp (unit.params[10], -160, 160) / 128.0));
        std::vector<EffectSlot> chain { philterSlot (band, gain) };
        const auto& vol = unit.articulators[1];
        if (! mapsNeutral (vol.mapX) || ! mapsNeutral (vol.mapY) || std::abs (vol.base - 1.0f) > 1e-6f)
        {
            auto utility = makeEffectSlot (EffectKind::utility); // volume controlled by Mod X/Y
            utility.params[0] = utilityGainValue (volumeFactor (unit, modX, modY));
            chain.push_back (utility);
        }
        // pattern envelope on volume (mode 0: gain × y·d_E), in beats and looped (REA_rodada20 §1.2–1.4)
        if (const auto& env = vol.pattern; env.on)
        {
            double period = 0.0;
            for (std::size_t k = 1; k < env.points.size(); ++k)
                period += env.points[k].dt;
            // points (x in beats, level, tension 0..1); curve type 5 ("pulse", REA_rodada27 §3.2) becomes steps: a square
            // wave of n + 1 cycles between the two levels, n = round(0.001·(1000001^(1 − 0.6|t|) − 1)), flips at x = j/P
            // (P = 2(n + 1)), starting on the previous level for t ≥ 0 and on the point's level for t < 0
            struct GatePoint { double x, y, tension; };
            std::vector<GatePoint> gatePoints;
            const auto depth = std::clamp ((double) vol.envDepth, 0.0, 1.0);
            double x = 0.0;
            for (std::size_t k = 0; k < env.points.size(); ++k)
            {
                const auto& pt = env.points[k];
                const auto level = pt.y * depth;
                if (k > 0 && pt.curve == 5 && ! gatePoints.empty())
                {
                    const auto t = (double) pt.tension;
                    const auto steps = 2 * ((int) std::round (0.001 * (std::pow (1000001.0, 1.0 - 0.6 * std::abs (t)) - 1.0)) + 1);
                    if (steps <= 22)
                    {
                        const auto from = gatePoints.back().y, eps = 1e-6 * pt.dt;
                        auto valueAt = [&] (int j) { return (j % 2 == 0) == (t >= 0.0) ? from : level; }; // interval j of P
                        if (valueAt (0) != from)
                            gatePoints.push_back ({ x + eps, valueAt (0), 0.5 });
                        for (int j = 1; j < steps; ++j)
                        {
                            const auto at = x + pt.dt * j / steps;
                            gatePoints.push_back ({ at - eps, valueAt (j - 1), 0.5 });
                            gatePoints.push_back ({ at, valueAt (j), 0.5 });
                        }
                        if (valueAt (steps - 1) != level)
                            gatePoints.push_back ({ x + pt.dt - eps, valueAt (steps - 1), 0.5 });
                    }
                    else
                        out.notes.push_back ("Love Philter: pulse curve with too many cycles treated as simple");
                }
                else if (k > 0 && pt.curve != 0)
                    out.notes.push_back ("Love Philter: envelope curve of type " + std::to_string (pt.curve) + " treated as simple");
                x += k > 0 ? pt.dt : 0.0;
                gatePoints.push_back ({ x, level, (pt.tension + 1.0) / 2.0 });
            }
            if (! env.tempo || period <= 1e-6 || gatePoints.size() > 12 || gatePoints.empty())
                out.notes.push_back ("Love Philter: volume envelope outside the supported format");
            else
            {
                auto gate = makeEffectSlot (EffectKind::rhythmGain);
                gate.params[0] = (float) std::min (1.0, period / 64.0);
                gate.params[1] = (float) gatePoints.size() / 12.0f;
                for (std::size_t k = 0; k < gatePoints.size(); ++k)
                {
                    gate.params[2 + 3 * k] = (float) clamp01 (gatePoints[k].x / period);
                    gate.params[3 + 3 * k] = (float) clamp01 (gatePoints[k].y);
                    gate.params[4 + 3 * k] = (float) clamp01 (gatePoints[k].tension);
                }
                chain.push_back (gate);
            }
        }

        const auto pan = std::clamp (unit.params[8], -128, 128) / 128.0;
        if (const auto& lfo = unit.articulators[0].lfo; lfo.on && ! lfo.frozen && lfo.tempo)
        {
            // pan = PAN/128 + 2·A·d_L·W(φ), period 625/(10001^(S/65536) − 1) beats (REA_rodada20 §1.5)
            auto autoPan = makeEffectSlot (EffectKind::autoPan);
            const auto period = 625.0 / (std::pow (10001.0, std::clamp (lfo.speed, 1, 65536) / 65536.0) - 1.0);
            const auto depth = std::clamp (2.0 * lfo.amplitude * unit.articulators[0].lfoDepth, -1.0, 1.0);
            autoPan.params[0] = (float) std::clamp (period / 64.0, 1e-4, 1.0);
            autoPan.params[1] = (float) clamp01 (depth * 0.5 + 0.5);
            autoPan.params[2] = (float) (lfo.phase / 4294967296.0);
            autoPan.params[3] = (float) clamp01 (pan * 0.5 + 0.5);
            autoPan.params[5] = 1.0f; // FL's pan law (REA_rodada27 §3.1)
            autoPan.params[6] = (float) clamp01 ((std::clamp (lfo.tension, -128, 128) + 128) / 256.0); // §3.3
            chain.push_back (autoPan);
            if (lfo.skew != 0 || lfo.width != 0)
                out.notes.push_back ("Love Philter: pan LFO shape approximated by a triangle");
        }
        else if (std::abs (pan) > 1e-3)
        {
            // FL's pan law (REA_rodada27 §3.1): √2·cos/sin((1 + pan)π/4) with the far side folded into the near one
            float ll, rl, lr, rr;
            flPanMatrix (pan, ll, rl, lr, rr);
            auto utility = makeEffectSlot (EffectKind::utility);
            utility.params[2] = utilityMatrixValue (ll);
            utility.params[3] = utilityMatrixValue (rl);
            utility.params[4] = utilityMatrixValue (lr);
            utility.params[5] = utilityMatrixValue (rr);
            chain.push_back (utility);
        }
        // cutoff and resonance envelope/LFO: live modulation of the EQ, added by modulateUnit (below)
        return chain;
    };

    // cutoff/resonance envelope and LFO (additive, REA_rodada20 §1.4/§1.5): c = CUT/256 + (y − 0.5)·d_E (for the
    // cutoff d_E = ENV/128·depth) or + A·d_L·W, and r = RES/256 + the same terms; a "control-only" slot generates
    // the value and a live automation drives the EQ's frequency or Q
    // the chain's EQ (slot 0) gets one control-only slot per enabled envelope/LFO, appended to `chain`;
    // `addLink (source, param, curve)` records the live link from that slot to the EQ
    auto modulateUnit = [&] (const LovePhilterUnit& unit, std::vector<EffectSlot>& chain,
                             const std::function<void (int, int, std::vector<float>)>& recordLink) {
        const auto cut0 = std::clamp (unit.params[1], 0, 256) / 256.0, res0 = std::clamp (unit.params[2], 0, 256) / 256.0;
        auto modulate = [&] (const auto& art, bool cutoff, double envDepth) {
            if (! (art.pattern.on || art.lfo.on) || chain.empty() || chain[0].kind != EffectKind::parametricEq)
                return;
            auto addLink = [&] (EffectSlot control, const std::function<double (double)>& valueOf) {
                std::vector<float> curve;
                for (int i = 0; i <= 32; ++i)
                {
                    const auto v = std::clamp (valueOf (i / 32.0), 0.0, 1.0);
                    const auto band = cutoff ? philterBand (unit, v, res0) : philterBand (unit, cut0, v);
                    curve.push_back (cutoff ? peq::hzValue (band.hz) : peq::qValue (band.q));
                }
                recordLink ((int) chain.size(), peq::param (0, cutoff ? 1 : 3), std::move (curve));
                chain.push_back (std::move (control));
            };
            const auto start = cutoff ? cut0 : res0;
            const auto& env = art.pattern;
            double period = 0.0;
            for (std::size_t k = 1; k < env.points.size(); ++k)
                period += env.points[k].dt;
            if (env.on && env.tempo && period > 1e-6 && env.points.size() <= 12)
            {
                auto control = makeEffectSlot (EffectKind::rhythmGain);
                control.params[0] = (float) std::min (1.0, period / 64.0);
                control.params[1] = (float) env.points.size() / 12.0f;
                double x = 0.0;
                for (std::size_t k = 0; k < env.points.size(); ++k)
                {
                    if (k > 0)
                        x += env.points[k].dt;
                    control.params[2 + 3 * k] = (float) clamp01 (x / period);
                    control.params[3 + 3 * k] = (float) clamp01 (env.points[k].y);
                    control.params[4 + 3 * k] = (float) clamp01 ((env.points[k].tension + 1.0) / 2.0);
                }
                control.params[38] = 1.0f;
                addLink (std::move (control), [=] (double y) { return start + (y - 0.5) * envDepth; });
            }
            else if (env.on)
                out.notes.push_back (std::string ("Love Philter: ") + (cutoff ? "cutoff" : "resonance") + " envelope outside the supported format");
            if (const auto& lfo = art.lfo; lfo.on && lfo.tempo && ! lfo.frozen)
            {
                auto control = makeEffectSlot (EffectKind::autoPan);
                const auto period = 625.0 / (std::pow (10001.0, std::clamp (lfo.speed, 1, 65536) / 65536.0) - 1.0);
                control.params[0] = (float) std::clamp (period / 64.0, 1e-4, 1.0);
                control.params[2] = (float) (lfo.phase / 4294967296.0);
                control.params[4] = 1.0f;
                const auto amount = lfo.amplitude * art.lfoDepth;
                addLink (std::move (control), [=] (double v) { return start + amount * (2.0 * v - 1.0); });
            }
        };
        modulate (unit.articulators[2], true, std::clamp (unit.params[0], -128, 128) / 128.0 * unit.articulators[2].envDepth);
        modulate (unit.articulators[3], false, unit.articulators[3].envDepth);
    };

    if (active.empty())
    {
        out.slots.push_back (philterSlot ({}, 0.0)); // no unit enabled: the Love Philter is silent
        return out;
    }

    if (active.size() == 1)
    {
        const auto u = active.front();
        out.slots = unitEffects (u);
        const auto& unit = state->units[(std::size_t) u];

        modulateUnit (unit, out.slots, [&] (int source, int param, std::vector<float> curve) {
            out.links.push_back ({ source, 0, param, std::move (curve) });
        });
        const auto base = 8 + 44 * u;
        const auto masterGain = master;
        // Mod X (k = 6) / Mod Y (k = 7) drive the volume utility, if any
        const auto modSlot = out.slots.size() >= 2 && out.slots[1].kind == EffectKind::utility && out.slots[1].params[1] == 0.5f ? 1 : -1;
        out.paramMap = [unit, base, masterGain, modSlot, modX, modY, volumeFactor] (int k) -> std::optional<std::tuple<int, int, std::function<float (float)>>> {
            if ((k == 6 || k == 7) && modSlot >= 0)
                return std::make_tuple (modSlot, 0, std::function<float (float)> ([unit, k, modX, modY, volumeFactor] (float v) {
                                            return utilityGainValue (k == 6 ? volumeFactor (unit, v, modY) : volumeFactor (unit, modX, v));
                                        }));
            if (k == base + 1) // CUT: c = value
                return std::make_tuple (0, peq::param (0, 1), std::function<float (float)> ([unit] (float v) {
                                            const auto b = philterBand (unit, v, std::clamp (unit.params[2], 0, 256) / 256.0);
                                            return peq::hzValue (b.hz);
                                        }));
            if (k == base + 2) // RES
                return std::make_tuple (0, peq::param (0, 3), std::function<float (float)> ([unit] (float v) {
                                            const auto b = philterBand (unit, std::clamp (unit.params[1], 0, 256) / 256.0, v);
                                            return peq::qValue (b.q);
                                        }));
            if (k == base + 9) // VOL: 0..160
                return std::make_tuple (0, peq::outputParam, std::function<float (float)> ([masterGain] (float v) {
                                            const auto g = scurve11 (160.0 * v / 128.0) * masterGain;
                                            return peq::gainValue (std::clamp (20.0 * std::log10 (std::max (1e-6, g)), -18.0, 18.0));
                                        }));
            return std::nullopt;
        };
        return out;
    }

    auto container = makeEffectSlot (EffectKind::parallel);
    for (const auto u : active)
    {
        auto chain = unitEffects (u);
        const auto branch = (int) container.branches.size();
        modulateUnit (state->units[(std::size_t) u], chain, [&] (int source, int param, std::vector<float> curve) {
            container.links.push_back ({ branch, source, 0, param, std::move (curve) });
        });
        container.branches.push_back ({ std::move (chain), 1.0f });
    }
    out.slots.push_back (std::move (container));
    return out;
}
PatcherBranchHead patcherBranchHead (const std::string& plugin, const std::vector<std::uint8_t>& d, int bus)
{
    PatcherBranchHead head;
    std::string name = plugin;
    std::transform (name.begin(), name.end(), name.begin(), [] (unsigned char c) { return (char) std::tolower (c); });

    if (name == "frequency splitter")
    {
        if (const auto s = fsplit::parseState (d))
        {
            // the exact engine (REA_rodada62): bus 0 = the summed output, bus k = the bands whose send destination is k
            if (! fsplit::busUsed (*s, bus))
                return head;
            auto exact = makeEffectSlot (EffectKind::frequencySplitter);
            fsplit::toParams (*s, exact.params.data(), bus);
            head.valid = true;
            head.slot = exact;
            return head;
        }
        if (d.size() < 18 * 4)
            return head;
        auto word = [&] (int i) { return i32At (d, 4 * (std::size_t) i); };
        if (bus == 0)
        {
            auto slot = makeEffectSlot (EffectKind::parametricEq);
            if (frequencySplitter (d, slot))
            {
                head.valid = true;
                head.slot = slot;
            }
            return head;
        }
        int band = -1;
        for (int i = 0; i < 3; ++i)
            if (word (15 + i) == bus)
                band = i;
        if (band < 0)
            return head;
        head.valid = true;
        const auto threeBands = word (6) != 0;
        if (! threeBands && band == 1)
        {
            head.gain = 0.0f; // with 2 bands the mid bus is muted
            return head;
        }
        // the band becomes a Crossover with the exact structure of each slope (REA_rodada17 §1.2); only the
        // 18 dB/oct one (FL's all-pass structure) is approximated by a 3rd-order Butterworth
        auto slot = makeEffectSlot (EffectKind::crossover);
        auto value = [] (int v) { return (float) (std::clamp (v, 0, 65536) / 65536.0); };
        auto combo = [] (int c) { return (float) std::clamp (c, 0, 7) / 7.0f; };
        bool approximate = false;
        auto highPass = [&] (int freqWord, int comboWord) {
            slot.params[0] = 1.0f;
            slot.params[1] = value (word (freqWord));
            slot.params[2] = combo (word (comboWord));
            approximate = approximate || word (comboWord) == 2;
        };
        auto lowPass = [&] (int freqWord, int comboWord) {
            slot.params[3] = 1.0f;
            slot.params[4] = value (word (freqWord));
            slot.params[5] = combo (word (comboWord));
            approximate = approximate || word (comboWord) == 2;
        };
        if (band == 0)
            lowPass (1, 3);
        else if (band == 1)
        {
            highPass (1, 3);
            lowPass (2, 4);
        }
        else if (threeBands)
            highPass (2, 4);
        else
            highPass (1, 3);
        head.slot = slot;
        head.gain = (float) std::pow (10.0, std::clamp (word (9 + band), -6000, 1800) / 100.0 / 20.0); // send level (hundredths of a dB)
        if (approximate)
            head.note = "Frequency Splitter: 18 dB/oct slope approximated";
        return head;
    }

    if (name == "fruity stereo shaper")
    {
        if (d.size() < 4 + 8 * 4)
            return head;
        if (const auto s = stereofx::parseStereoShaper (d))
        {
            // the exact engine (REA_rodada54): bus 0 = its output, the parameter-6 bus = input − output
            if (bus != 0 && bus != s->send)
                return head;
            auto exact = makeEffectSlot (EffectKind::stereoShaper);
            stereofx::stereoShaperToParams (*s, exact.params.data());
            exact.params[7] = bus != 0 ? 1.0f : 0.0f;
            head.valid = true;
            head.slot = exact;
            return head;
        }
        auto param = [&] (int i) { return i32At (d, 4 + 4 * (std::size_t) i); };
        auto slot = makeEffectSlot (EffectKind::utility);
        if (bus == 0)
        {
            stereoShaper (d, slot);
            head.valid = true;
            head.slot = slot;
            return head;
        }
        if (bus != param (6) || param (4) != 0 || param (5) != 0)
            return head; // with delay/phase the difference is not a matrix: skip
        // input − matrix output M = (I − M)
        const auto rl = param (0) / 12800.0, l = param (1) / 12800.0, r = param (2) / 12800.0, lr = param (3) / 12800.0;
        slot.params[2] = utilityMatrixValue (1.0 - l);
        slot.params[3] = utilityMatrixValue (-rl);
        slot.params[5] = utilityMatrixValue (1.0 - r);
        slot.params[4] = utilityMatrixValue (-lr);
        head.valid = true;
        head.slot = slot;
        return head;
    }

    return head;
}
} // namespace lynx
