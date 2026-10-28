// Round 165g flgran165: Fruity Granulizer's slices + transient hold, tempo sync, key modes 2/3 and FL's render quality
// (core/Granulizer, REA_rodada165g-flgran165)

#include <numeric>

#include "core/FlAutoSlicer.h"

namespace flgran165
{
// our own deterministic drum-like signal (tmp/flgran165/py/goldsig.py builds the same int16 data for the DLL):
// six square-wave hits halving every 1200 frames; right = left·3 >> 2
lynx::SampleData drums (int ch, double rate)
{
    constexpr int n = 48000;
    const int hits[] = { 0, 7000, 15500, 26000, 33000, 41000 };
    std::vector<std::int32_t> x ((std::size_t) n, 0);
    for (int j = 0; j < 6; ++j)
    {
        const int per = 14 + 6 * (j % 4), amp = 20000 - 3000 * (j % 3);
        for (int i = hits[j]; i < n; ++i)
        {
            const int k = i - hits[j];
            const int a = amp >> (k / 1200);
            if (a == 0)
                break;
            const int v = (k / per) % 2 == 0 ? a : -a;
            x[(std::size_t) i] = std::clamp (x[(std::size_t) i] + v, -32768, 32767);
        }
    }
    lynx::SampleData d;
    d.sampleRate = rate;
    for (int i = 0; i < n; ++i)
    {
        d.left.push_back ((float) (std::int16_t) x[(std::size_t) i] / 32768.0f);
        if (ch > 1)
            d.right.push_back ((float) (std::int16_t) ((x[(std::size_t) i] * 3) >> 2) / 32768.0f);
    }
    return d;
}

// a version-7 state without the channel snapshot (as tmp/r165g/py/mkgr.py writes them for FL)
std::vector<std::uint8_t> state (const std::array<int, 12>& P, int sliceMode, int keyMode, int sync)
{
    std::vector<std::uint8_t> b (64, 0);
    const std::int32_t head[1] = { 7 };
    std::memcpy (b.data(), head, 4);
    std::memcpy (b.data() + 4, P.data(), 48);
    std::memcpy (b.data() + 52, &sliceMode, 4);
    std::memcpy (b.data() + 56, &keyMode, 4);
    std::memcpy (b.data() + 60, &sync, 4);
    return lynx::granulizerNormalisedState (b, {});
}

double energy (const std::vector<float>& y, int c)
{
    double e = 0.0;
    for (std::size_t i = (std::size_t) c; i < y.size(); i += 2)
        e += (double) y[i] * y[i];
    return e;
}

// the instrument on a channel, one note, `n` frames (left only)
std::vector<float> play (const lynx::ChannelDef& ch, std::shared_ptr<const lynx::SampleData> smp, int n, double rate = 44100.0)
{
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::granulizer, smp);
    inst->prepare (rate);
    inst->configure (ch, 140.0);
    std::vector<float> l ((std::size_t) n, 0.0f), r ((std::size_t) n, 0.0f);
    inst->noteOn (62, 0.8f);
    inst->render (l.data(), r.data(), n, 1.0f, 1.0f);
    return l;
}
} // namespace flgran165

TEST (granulizer165_modes)
{
    using namespace flgran165;
    const std::array<int, 12> P { 204, 50, 611, 300, 0, 180, 0, 0, 0, 0, 0, 228 };
    auto s = state (P, 1, 2, 1);
    CHECK (lynx::granulizerMode (s, 0) == 1 && lynx::granulizerMode (s, 1) == 2 && lynx::granulizerMode (s, 2) == 1
           && ! lynx::granulizerMode (s, 3));
    CHECK (lynx::granulizerSetMode (s, 2, 0) && lynx::granulizerMode (s, 2) == 0);
    CHECK (lynx::granulizerSetMode (s, 0, 9) && lynx::granulizerMode (s, 0) == 2); // clamped to "detect"
    // FL's editor: "Transient" keys turn slice detection on when the transients are off (0x758050, on every load)
    CHECK (lynx::granulizerSetMode (s, 0, 0) && lynx::granulizerSetMode (s, 1, 3) && lynx::granulizerMode (s, 0) == 2);
    CHECK (lynx::granulizerMode (state (P, 0, 3, 0), 0) == 2 && lynx::granulizerMode (state (P, 1, 3, 0), 0) == 1);
    // the sync byte 2 ("auto", never saved by FL in practice) is kept as off
    CHECK (lynx::granulizerMode (state (P, 0, 0, 2), 2) == 0);
}

TEST (granulizer165_goldens)
{
    // LYNX's engine against our goldens, which the installation's DLL renders within −134 / −94 / −136 dB before the
    // first release (tmp/flgran165/py/gold165.py; the −94 dB case: the sample reader's edge path at a 48 kHz sample)
    using namespace flgran165;
    using N = lynx::granulizer::RefNote;
    struct Golden { std::vector<std::uint8_t> state; int ch, fs, rate; lynx::granulizer::RefOptions opt; std::vector<N> notes;
                    double el, er; std::tuple<int, float, float> at[6]; };
    auto check = [] (const Golden& g) {
        const auto smp = drums (g.ch, g.rate);
        auto opt = g.opt;
        if (lynx::granulizerMode (g.state, 0) == 2)
            opt.slices = lynx::flslice::autoSlice (smp.leftData(), g.ch > 1 ? smp.rightData() : nullptr, (std::int64_t) smp.frames(), smp.sampleRate);
        const auto y = lynx::granulizer::renderReference (g.state, smp, g.notes, (double) g.fs, 32768, opt);
        CHECK (y.size() == 2 * 32768);
        if (y.size() != 2 * 32768)
            return;
        const auto el = energy (y, 0), er = energy (y, 1);
        CHECK (std::abs (el - g.el) <= 1e-6 * g.el && std::abs (er - g.er) <= 1e-6 * g.er);
        for (const auto& [i, l, r] : g.at)
            CHECK (goldenSampleClose (y[2 * (std::size_t) i], l, granulizerTolerance())
                   && goldenSampleClose (y[2 * (std::size_t) i + 1], r, granulizerTolerance()));
    };
    {
        // auto slices (slices 0, 7000, 15480, 26000, 32976, 41000) with the transient hold
        CHECK ((lynx::flslice::autoSlice (drums (2, 44100).leftData(), drums (2, 44100).rightData(), 48000, 44100.0)
                == std::vector<std::int64_t> { 0, 7000, 15480, 26000, 32976, 41000 }));
        lynx::granulizer::RefOptions o;
        o.points = 12, o.tempo = 120.0;
        check ({ state ({ 204, 50, 611, 300, 0, 180, 0, 0, 0, 0, 0, 228 }, 2, 0, 0), 2, 44100, 44100, o,
                 { { 0, 60, 0.8, 0.0, 0.0, -1 }, { 9000, 62, 0.6, -0.3, 0.0, 20000 } }, 646.7863275739593, 309.46088519508237,
                 { { 100, -0.3466797173023224f, -0.2600097954273224f }, { 2500, 0.086669921875f, 0.06500244140625f },
                   { 6100, -0.08848780393600464f, -0.06636585295200348f }, { 11111, -0.14579427242279053f, -0.06419368088245392f },
                   { 17000, -0.16434688866138458f, -0.13543438911437988f }, { 30000, -0.03256341814994812f, -0.024431241676211357f } } });
    }
    {
        // regions (the WAV's markers) with "Transient" keys: C#5 and D#5 start at slices 1 and 3; mono 48 kHz sample
        lynx::granulizer::RefOptions o;
        o.points = 12, o.tempo = 120.0, o.slices = { 0, 7000, 26000, 41000 };
        check ({ state ({ 150, 120, 400, 500, 0, 180, 0, 20, 1, 0, 0, 300 }, 1, 3, 0), 1, 44100, 48000, o,
                 { { 0, 61, 1.0, 0.2, 0.0, -1 }, { 6000, 63, 0.5, 0.0, 0.0, -1 } }, 1561.4073825720589, 2339.501975751048,
                 { { 100, -0.03832821547985077f, -0.03161964565515518f }, { 2500, 0.09727142751216888f, 0.08024617284536362f },
                   { 6100, -0.12772196531295776f, 0.1849401742219925f }, { 11111, 0.43030425906181335f, 0.41122832894325256f },
                   { 17000, 0.2787952423095703f, 0.22190749645233154f }, { 30000, -0.01753564178943634f, 0.03832763060927391f } } });
    }
    {
        // tempo locked at 4 beats, 140 BPM, "Step" keys, auto slices, FL's render quality (16 points), 44.1 kHz sample at 48 kHz
        lynx::granulizer::RefOptions o;
        o.points = 16, o.tempo = 140.0, o.beats = 4.0f;
        check ({ state ({ 180, 60, 500, 611, 0, 180, 0, 30, 0, 0, 0, 100 }, 2, 2, 1), 2, 48000, 44100, o,
                 { { 0, 60, 0.9, 0.0, 0.0, -1 }, { 5000, 64, 0.7, 0.0, 13.0, 25000 } }, 1200.1440687254085, 436.4748503902965,
                 { { 100, 0.49821606278419495f, 0.16563299298286438f }, { 2500, 0.11618572473526001f, -0.06435169279575348f },
                   { 6100, -0.059035543352365494f, -0.05729549378156662f }, { 11111, -0.17978179454803467f, -0.16116628050804138f },
                   { 17000, 0.1330646574497223f, 0.05158308893442154f }, { 30000, -0.12011360377073288f, -0.1214638277888298f } } });
    }
}

TEST (granulizer165_instrument)
{
    // the channel's wiring: which beat count FL hands the plugin, where the slices come from, the render quality
    using namespace flgran165;
    auto make = [] (double acid, std::vector<std::int64_t> markers = {}) {
        auto d = std::make_shared<lynx::SampleData> (drums (2, 44100.0));
        d->acidBeats = (float) acid;
        d->markerStarts = std::move (markers);
        return std::shared_ptr<const lynx::SampleData> (d);
    };
    lynx::ChannelDef ch;
    ch.instrument = lynx::InstrumentKind::granulizer;
    ch.pluginState = state ({ 204, 50, 611, 300, 0, 180, 0, 0, 0, 0, 0, 228 }, 0, 2, 1);
    constexpr int n = 30000;
    // the WAV's ACID beats win over the channel's TIME (FL render g12): ACID 4 + TIME 8 = TIME 4 alone ≠ TIME 8 alone
    ch.sampler.stretchBeats = 8.0f;
    const auto acid4 = play (ch, make (4.0), n), time8 = play (ch, make (0.0), n);
    ch.sampler.stretchBeats = 4.0f;
    const auto time4 = play (ch, make (0.0), n);
    CHECK (acid4 == time4 && acid4 != time8);
    // tempo lock off: the beats only move the Step keys' start (no sync); still differs from no beats at all
    ch.sampler.stretchBeats = 0.0f;
    const auto none = play (ch, make (0.0), n);
    CHECK (none != time4);
    // slices: "Use regions" with the auto slicer's positions as WAV markers = "Detect"; both differ from no slices
    const std::vector<std::int64_t> autoSl { 0, 7000, 15480, 26000, 32976, 41000 };
    ch.pluginState = state ({ 204, 50, 611, 300, 0, 180, 0, 0, 0, 0, 0, 228 }, 2, 0, 0);
    const auto detect = play (ch, make (0.0), n);
    ch.pluginState = state ({ 204, 50, 611, 300, 0, 180, 0, 0, 0, 0, 0, 228 }, 1, 0, 0);
    const auto regions = play (ch, make (0.0, autoSl), n), noMarkers = play (ch, make (0.0), n);
    ch.pluginState = state ({ 204, 50, 611, 300, 0, 180, 0, 0, 0, 0, 0, 228 }, 0, 0, 0);
    const auto plain = play (ch, make (0.0), n);
    CHECK (detect == regions && detect != plain && noMarkers == plain);
    // FL renders with 16 interpolation points per side, plays live with 12: a sample off the host rate sounds slightly
    // different while exporting
    auto offRate = std::make_shared<lynx::SampleData> (drums (2, 48000.0));
    const auto live = play (ch, offRate, n);
    lynx::setOfflineRendering (true);
    const auto exported = play (ch, offRate, n);
    lynx::setOfflineRendering (false);
    CHECK (live != exported && std::inner_product (live.begin(), live.end(), live.begin(), 0.0) > 1.0);
}
