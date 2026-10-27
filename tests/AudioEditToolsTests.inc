// Audio editor tools (round 165l, core/AudioEditTools.h): Edison's tool forms in LYNX. Included at the end of
// tests.cpp (after AudioEditTests.inc, whose helpers sineDoc / zeroCrossFreq it uses).

#include "core/AudioEditTools.h"

namespace
{
double rmsOf (const std::vector<float>& v, std::int64_t a, std::int64_t b)
{
    double e = 0.0;
    for (auto i = a; i < b; ++i)
        e += (double) v[(std::size_t) i] * v[(std::size_t) i];
    return b > a ? std::sqrt (e / (double) (b - a)) : 0.0;
}

// the level of a sine at `hz` in `x` (a single-bin DFT over [a, b))
double toneLevel (const std::vector<float>& x, std::int64_t a, std::int64_t b, double hz, double sr)
{
    double re = 0.0, im = 0.0;
    for (auto i = a; i < b; ++i)
    {
        const auto ph = 2.0 * 3.141592653589793 * hz * (double) i / sr;
        re += x[(std::size_t) i] * std::cos (ph);
        im += x[(std::size_t) i] * std::sin (ph);
    }
    return 2.0 * std::hypot (re, im) / (double) (b - a);
}

ae::Document noiseDoc (double seconds, float level, double rate = 44100.0, std::uint32_t seed = 3)
{
    ae::Document d;
    d.audio = ae::silence (2, rate, (std::int64_t) (seconds * rate));
    ae::generateNoise (d, {}, level, seed);
    return d;
}
} // namespace

TEST (audioedittools_envelope_eval_and_point_tools_round165l)
{
    ae::Envelope e;
    e.points = { { 0.0, 0.0 }, { 0.5, 1.0 }, { 1.0, 0.0 } };
    CHECK (std::abs (e.at (0.25) - 0.5) < 1e-9);
    CHECK (std::abs (e.at (0.75) - 0.5) < 1e-9);
    e.points[1].tension = 0.5f; // slow start on the rising segment
    CHECK (e.at (0.25) < 0.5);

    ae::Envelope f = ae::Envelope::flat (0.5);
    ae::scaleLevels (f, 0.0, 2.0, 0.0, 0.0); // multiply x2 around 0
    CHECK (std::abs (f.points[0].y - 1.0) < 1e-9);
    ae::scaleLevels (f, 0.0, 1.0, -0.25, 0.0);
    CHECK (std::abs (f.points[0].y - 0.75) < 1e-9);
    ae::flipLevels (f);
    CHECK (std::abs (f.points[0].y - 0.25) < 1e-9);

    ae::Envelope g;
    g.points = { { 0.0, 0.2 }, { 0.5, 0.4 }, { 1.0, 0.6 } };
    ae::normalizeLevels (g);
    CHECK (std::abs (g.points[0].y) < 1e-9 && std::abs (g.points[2].y - 1.0) < 1e-9);

    // decimation removes the collinear middle point but keeps the corner
    ae::Envelope h;
    h.points = { { 0.0, 0.0 }, { 0.25, 0.25 }, { 0.5, 0.5 }, { 0.75, 0.0 }, { 1.0, 0.0 } };
    ae::decimatePoints (h, 0.5);
    CHECK (h.points.size() == 4u);
    CHECK (std::abs (h.points[1].x - 0.5) < 1e-9);
    ae::decimatePoints (h, 0.0); // threshold 0 keeps everything
    CHECK (h.points.size() == 4u);
}

TEST (audioedittools_smooth_filter_sequence_round165l)
{
    // a step 0 -> 1 at x = 0.5: smooth up with a long attack rises gradually
    ae::Envelope step;
    step.points = { { 0.0, 0.0 }, { 0.5, 0.0 }, { 0.5, 1.0 }, { 1.0, 1.0 } };
    auto s = step;
    ae::smoothUp (s, 0.5, 0.5, 0.5, 0.5, 0.0, 0.2);
    CHECK (s.at (0.52) < 0.5);
    CHECK (s.at (0.99) > 0.9);
    CHECK (s.at (0.4) < 0.01);

    auto f = step;
    ae::filterEnvelope (f, {}, 0.3, 0.0, 0.1);
    CHECK (f.at (0.45) > 0.02 && f.at (0.55) < 0.98); // blurred around the step
    CHECK (f.at (0.05) < 0.01 && f.at (0.95) > 0.99);

    ae::SequenceSettings q;
    q.levels = { 1.0f, 0.5f };
    q.breaks = { false, true };
    q.stepsPerUnit = 2;
    const auto seq = ae::createSequence (q, 2.0); // 4 steps of 1/4
    CHECK (seq.at (0.125 * 0.3) > 0.5); // inside step 0's gate
    CHECK (seq.at (0.25 + 0.05) < 1e-6); // step 1 is a break
    CHECK (seq.at (0.5 + 0.05) > 0.5);  // step 2 = level 1 again
}

TEST (audioedittools_amp_mixdown_and_levels_round165l)
{
    auto d = sineDoc (440.0, 0.2, 44100.0, 0.5f);
    const auto n = d.audio.frames();
    ae::AmpSettings a;
    a.pan = 1.0; // hard right: left silent, right unity
    ae::ampTool (d, {}, a);
    CHECK (rmsOf (d.audio.channels[0], 0, n) < 1e-6);
    CHECK (std::abs (rmsOf (d.audio.channels[1], 0, n) - 0.5 / std::sqrt (2.0)) < 1e-3);

    auto e = sineDoc (440.0, 0.2, 44100.0, 0.5f);
    ae::AmpSettings b;
    b.pan = 0.5;
    b.circularLaw = true;
    ae::ampTool (e, {}, b);
    const auto l = rmsOf (e.audio.channels[0], 0, n), r = rmsOf (e.audio.channels[1], 0, n);
    CHECK (r > l && r <= 0.5 / std::sqrt (2.0) + 1e-6);

    // stereo separation -1 = mono
    auto m = rampDoc (100);
    ae::AmpSettings c;
    c.stereoSeparation = -1.0;
    ae::ampTool (m, {}, c);
    CHECK (std::abs (m.audio.channels[0][50]) < 1e-6 && std::abs (m.audio.channels[1][50]) < 1e-6);

    // volume envelope: 0.5 = unity, the end at 0 fades out
    auto v = sineDoc (100.0, 0.5, 44100.0, 0.5f);
    ae::Envelope vol;
    vol.points = { { 0.0, 0.5 }, { 0.5, 0.5 }, { 1.0, 0.0 } };
    ae::mixdownEnvelopes (v, vol, {}, {});
    const auto vn = v.audio.frames();
    CHECK (std::abs (rmsOf (v.audio.channels[0], 0, vn / 4) - 0.5 / std::sqrt (2.0)) < 0.01);
    CHECK (rmsOf (v.audio.channels[0], vn - vn / 50, vn) < 0.01);

    // lossy normalize: louder than plain normalize, never over full scale
    auto ln = noiseDoc (0.5, 0.25f);
    ln.audio.channels[0][100] = 0.9f; // one stray peak
    auto plain = ln;
    ae::normalize (plain, {}, 0.0f);
    ae::lossyNormalize (ln, {});
    CHECK (rmsOf (ln.audio.channels[0], 0, ln.audio.frames()) > 1.5 * rmsOf (plain.audio.channels[0], 0, plain.audio.frames()));
    CHECK (ae::peak (ln.audio, {}) <= 1.0f);

    auto mono = sineDoc (440.0, 0.05);
    CHECK (ae::monoIfMono (mono) && mono.audio.numChannels() == 1);
    auto st = rampDoc (10);
    CHECK (! ae::monoIfMono (st));
}

TEST (audioedittools_noise_gating_and_regions_round165l)
{
    // tone 0.2 s, silence(ish) 0.3 s, tone 0.2 s
    const double sr = 44100.0;
    auto d = sineDoc (300.0, 0.7, sr, 0.5f);
    auto hiss = noiseDoc (0.7, 0.001f, sr);
    for (int c = 0; c < 2; ++c)
        for (auto i = (std::int64_t) (0.2 * sr); i < (std::int64_t) (0.5 * sr); ++i)
            d.audio.channels[(std::size_t) c][(std::size_t) i] = hiss.audio.channels[(std::size_t) c][(std::size_t) i];
    const auto thr = ae::acquireNoiseThreshold (d.audio, { (std::int64_t) (0.25 * sr), (std::int64_t) (0.45 * sr) });
    CHECK (thr > 0.0005f && thr < 0.0015f);
    const auto parts = ae::quietParts (d.audio, {}, thr * 1.1f);
    CHECK (parts.size() == 1u);

    auto g = d;
    CHECK (ae::gateNoise (g, {}, thr * 1.1f) == 1);
    CHECK (rmsOf (g.audio.channels[0], (std::int64_t) (0.3 * sr), (std::int64_t) (0.4 * sr)) == 0.0);

    auto t = d;
    const auto n = ae::trimNoiseAndSlice (t, {}, thr * 1.1f);
    CHECK (n == 2);
    CHECK (std::abs (t.audio.seconds() - 0.41) < 0.03);

    // regions: normalize each
    auto r = sineDoc (300.0, 1.0, sr, 0.1f);
    ae::addRegion (r, { 0, 10000 }, "a");
    ae::addRegion (r, { 20000, 30000 }, "b");
    CHECK (ae::normalizeRegions (r) == 2);
    CHECK (std::abs (ae::peak (r.audio, { 0, 10000 }) - 1.0f) < 1e-3);
    CHECK (ae::peak (r.audio, { 12000, 18000 }) < 0.11f);
    CHECK (ae::perfectRegions (r) == 2);
    CHECK (std::abs (r.audio.channels[0][(std::size_t) r.markers[0].start]) < 0.05f);
}

TEST (audioedittools_eq_and_unharmonics_round165l)
{
    const double sr = 44100.0;
    auto d = sineDoc (200.0, 1.0, sr, 0.25f);
    auto hi = sineDoc (8000.0, 1.0, sr, 0.25f);
    for (int c = 0; c < 2; ++c)
        for (std::size_t i = 0; i < d.audio.channels[0].size(); ++i)
            d.audio.channels[(std::size_t) c][i] += hi.audio.channels[(std::size_t) c][i];
    ae::EqSettings eq;
    eq.freqScale = 1.0;
    const auto split = ae::curvePosition (1000.0, 1.0, sr);
    eq.gain.points = { { 0.0, 0.5 }, { split, 0.5 }, { split + 0.01, 0.0 }, { 1.0, 0.0 } }; // low-pass at ~1 kHz
    ae::equalize (d, {}, eq);
    const auto n = d.audio.frames();
    const auto a = n / 4, b = 3 * n / 4;
    CHECK (std::abs (toneLevel (d.audio.channels[0], a, b, 200.0, sr) - 0.25) < 0.01);
    CHECK (toneLevel (d.audio.channels[0], a, b, 8000.0, sr) < 0.0025);

    // a flat curve at 0.5 is transparent
    auto t = sineDoc (1234.0, 0.5, sr, 0.3f);
    const auto before = t.audio.channels[0];
    ae::EqSettings flat;
    flat.gain = ae::Envelope::flat (0.5);
    ae::equalize (t, {}, flat);
    double err = 0.0;
    for (std::size_t i = 0; i < before.size(); ++i)
        err = std::max (err, (double) std::abs (before[i] - t.audio.channels[0][i]));
    CHECK (err < 1e-4);

    CHECK (std::abs (ae::curveFrequency (ae::curvePosition (440.0, 0.6, sr), 0.6, sr) - 440.0) < 0.5);
    const auto u = ae::unharmonicsCurve (100.0, 1, 1.0, sr);
    CHECK (u.at (ae::curvePosition (300.0, 1.0, sr)) > 0.45);
    CHECK (u.at (ae::curvePosition (350.0, 1.0, sr)) < 0.05);
}

TEST (audioedittools_reverb_blur_round165l)
{
    const double sr = 44100.0;
    ae::Document d;
    d.audio = ae::silence (2, sr, (std::int64_t) sr);
    d.audio.channels[0][1000] = d.audio.channels[1][1000] = 1.0f; // an impulse
    ae::ReverbSettings rv;
    rv.length = 0.5;
    rv.dry = 1.0;
    rv.wet = 0.5;
    rv.addTail = true;
    const auto before = d.audio.frames();
    ae::convolutionReverb (d, { 0, before }, rv);
    CHECK (d.audio.frames() == before + (std::int64_t) (0.5 * sr));
    CHECK (std::abs (d.audio.channels[0][1000] - 1.0f) < 0.1f); // the dry hit
    CHECK (rmsOf (d.audio.channels[0], 2000, 10000) > 1e-4);     // the tail rings
    CHECK (rmsOf (d.audio.channels[0], 1000 + (std::int64_t) (0.6 * sr), before) < 1e-6);

    // blur spreads a burst in time
    ae::Document b;
    b.audio = ae::silence (1, sr, (std::int64_t) sr);
    for (int i = 0; i < 2000; ++i)
        b.audio.channels[0][(std::size_t) (10000 + i)] = (float) std::sin (i * 0.1) * 0.5f;
    ae::BlurSettings bs;
    bs.amount = 0.1;
    ae::blur (b, {}, bs);
    CHECK (rmsOf (b.audio.channels[0], 14000, 18000) > 1e-3);
}

TEST (audioedittools_denoise_round165l)
{
    const double sr = 44100.0;
    auto noise = noiseDoc (2.0, 0.02f, sr, 9);
    auto tone = sineDoc (440.0, 2.0, sr, 0.3f);
    auto d = tone;
    for (int c = 0; c < 2; ++c)
        for (std::size_t i = 0; i < d.audio.channels[0].size(); ++i)
            d.audio.channels[(std::size_t) c][i] += noise.audio.channels[(std::size_t) c][i];
    // the first 0.5 s: noise only (the profile)
    for (auto& ch : d.audio.channels)
        for (std::int64_t i = 0; i < (std::int64_t) (0.5 * sr); ++i)
            ch[(std::size_t) i] = noise.audio.channels[0][(std::size_t) i];
    const auto profile = ae::acquireNoiseProfile (d.audio, { 0, (std::int64_t) (0.5 * sr) });
    CHECK (profile.valid());
    ae::DenoiseSettings s;
    s.amount = 1.0;
    ae::denoise (d, {}, profile, s);
    const auto a = (std::int64_t) (1.0 * sr), b = (std::int64_t) (1.8 * sr);
    CHECK (std::abs (toneLevel (d.audio.channels[0], a, b, 440.0, sr) - 0.3) < 0.03); // the tone stays
    CHECK (rmsOf (d.audio.channels[0], 2000, (std::int64_t) (0.45 * sr)) < 0.02 * 0.2); // the noise-only part goes
    // the residual (signal - tone) is much smaller than the noise was
    double resid = 0.0;
    for (auto i = a; i < b; ++i)
    {
        const auto e = d.audio.channels[0][(std::size_t) i] - tone.audio.channels[0][(std::size_t) i];
        resid += e * e;
    }
    CHECK (std::sqrt (resid / (double) (b - a)) < 0.02 * 0.5);

    // declipper: a clipped sine gets its peaks back
    auto c = sineDoc (100.0, 0.1, sr, 1.0f);
    const auto orig = c.audio.channels[0];
    for (auto& ch : c.audio.channels)
        for (auto& v : ch)
            v = std::clamp (v, -0.7f, 0.7f);
    CHECK (ae::declip (c.audio, {}, 0.99) > 0);
    CHECK (ae::peak (c.audio, {}) > 0.85f);

    // declicker: a single spike in a sine is removed
    auto k = sineDoc (200.0, 0.2, sr, 0.3f);
    k.audio.channels[0][4000] += 0.6f;
    CHECK (ae::declick (k.audio, {}, 0.5) >= 1);
    CHECK (std::abs (k.audio.channels[0][4000] - 0.3f * (float) std::sin (2.0 * 3.141592653589793 * 200.0 * 4000 / sr)) < 0.05f);
}

TEST (audioedittools_vocal_denoise_match_spectrum_round165l)
{
    const double sr = 44100.0;
    auto noise = noiseDoc (2.0, 0.03f, sr, 5);
    auto tone = sineDoc (300.0, 2.0, sr, 0.3f);
    auto d = tone;
    for (int c = 0; c < 2; ++c)
        for (std::size_t i = 0; i < d.audio.channels[0].size(); ++i)
            d.audio.channels[(std::size_t) c][i] += noise.audio.channels[(std::size_t) c][i];
    auto v = d;
    ae::vocalDenoise (v, {}, 1.0);
    const auto a = (std::int64_t) (0.8 * sr), b = (std::int64_t) (1.8 * sr);
    double before = 0.0, after = 0.0;
    for (auto i = a; i < b; ++i)
    {
        const auto e0 = d.audio.channels[0][(std::size_t) i] - tone.audio.channels[0][(std::size_t) i];
        const auto e1 = v.audio.channels[0][(std::size_t) i] - tone.audio.channels[0][(std::size_t) i];
        before += e0 * e0;
        after += e1 * e1;
    }
    CHECK (after < before * 0.35);
    auto n = d;
    ae::vocalDenoise (n, {}, -1.0); // the noise only: the tone mostly goes
    CHECK (toneLevel (n.audio.channels[0], a, b, 300.0, sr) < 0.1);

    // match spectrum: white noise matched to a low-passed noise gets darker
    auto ref = noiseDoc (1.0, 0.2f, sr, 11);
    ae::EqSettings lp;
    const auto split = ae::curvePosition (1000.0, 1.0, sr);
    lp.gain.points = { { 0.0, 0.5 }, { split, 0.5 }, { split + 0.05, 0.1 }, { 1.0, 0.1 } };
    ae::equalize (ref, {}, lp);
    auto w = noiseDoc (1.0, 0.2f, sr, 13);
    ae::matchSpectrum (w, {}, ref.audio, 1.0);
    // compare highs vs lows by a crude high-pass (first difference) energy ratio
    auto hpRatio = [] (const std::vector<float>& x) {
        double e = 0.0, h = 0.0;
        for (std::size_t i = 1; i < x.size(); ++i)
        {
            e += (double) x[i] * x[i];
            h += (double) (x[i] - x[i - 1]) * (x[i] - x[i - 1]);
        }
        return h / std::max (1e-12, e);
    };
    const auto white = noiseDoc (1.0, 0.2f, sr, 13);
    CHECK (hpRatio (w.audio.channels[0]) < 0.5 * hpRatio (white.audio.channels[0]));
}

TEST (audioedittools_claw_drumloop_loop_tuner_round165l)
{
    const double sr = 44100.0;
    // claw: 120 bpm, period 1 beat (0.5 s), trash 4/4: a quarter of every beat goes
    auto d = sineDoc (220.0, 2.0, sr, 0.3f);
    ae::ClawSettings c;
    c.snapToMarkers = false;
    ae::clawMachine (d, {}, c, 120.0);
    CHECK (std::abs (d.audio.seconds() - 1.5) < 0.01);
    auto full = sineDoc (220.0, 2.0, sr, 0.3f);
    c.compensation = 1.0;
    c.resample = true;
    ae::clawMachine (full, {}, c, 120.0);
    CHECK (std::abs (full.audio.seconds() - 2.0) < 0.01);

    // drumloop stretch: 4 hits at 0, 0.25, 0.5, 0.75 s → ratio 2 → hits at 0, 0.5, 1.0, 1.5 s, attacks intact
    ae::Document loop;
    loop.audio = ae::silence (1, sr, (std::int64_t) sr);
    for (int h = 0; h < 4; ++h)
        for (int i = 0; i < 2000; ++i)
            loop.audio.channels[0][(std::size_t) (h * sr / 4 + i)] = (float) (std::sin (i * 0.3) * std::exp (-i / 400.0));
    for (int h = 1; h < 4; ++h)
        ae::addMarker (loop, (std::int64_t) (h * sr / 4));
    ae::DrumStretchSettings ds;
    ds.ratio = 2.0;
    ae::drumloopStretch (loop, {}, ds);
    CHECK (std::abs (loop.audio.seconds() - 2.0) < 0.001);
    for (int h = 0; h < 4; ++h)
    {
        const auto at = (std::int64_t) (h * sr / 2);
        CHECK (std::abs (loop.audio.channels[0][(std::size_t) (at + 5)] - (float) (std::sin (5 * 0.3) * std::exp (-5 / 400.0))) < 0.05f);
    }
    CHECK (loop.markers.size() == 3u && std::abs (loop.markers[0].start - (std::int64_t) (sr / 2)) <= 1);

    // loop tuner: a loop over a sine whose end does not match its start gets a smooth seam
    auto t = sineDoc (100.0, 1.0, sr, 0.5f);
    ae::setLoop (t, { 20000, 30123 });
    ae::LoopTuneSettings lt;
    lt.blurOrder = 1;
    ae::tuneLoop (t, {}, lt);
    CHECK (t.hasLoop());
    const auto& x = t.audio.channels[0];
    const auto jump = std::abs (x[(std::size_t) t.loopEnd - 1] - x[(std::size_t) t.loopStart - 1]);
    CHECK (jump < 0.02f); // the end now leads where the audio before the start led
}

#ifndef LYNX_MINI_HARNESS // (needs core/EventEdit)
#include "core/EventEdit.h"

TEST (eventedit_to_automation_points_round165l)
{
    lynx::PatternEvents lane;
    // a ramp of events (0, 0.1, ..., 0.9) every 24 ticks, then a hold
    for (int i = 0; i < 10; ++i)
        lane.events.push_back ({ i * 24, i * 0.1f });
    const auto exact = lynx::eventedit::toAutomationPoints (lane, 1.0f);
    CHECK (exact.size() == 19u); // a step (two points) per change, plus the first
    CHECK (exact.front().tick == 0 && exact.back().tick == 9 * 24);
    const auto simple = lynx::eventedit::toAutomationPoints (lane, 0.4f);
    CHECK (simple.size() < 6u);
    CHECK (std::abs (simple.back().value - 0.9f) < 1e-6f);
}
#endif
