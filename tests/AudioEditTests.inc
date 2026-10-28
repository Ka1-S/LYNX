// Audio editor core (round 86, core/AudioEdit.h): every edit operation, markers, history, spectrogram, WAV.
// Included at the end of tests.cpp.

#include "core/AudioEdit.h"

namespace
{
namespace ae = lynx::audioedit;

// stereo ramp: left[i] = i, right[i] = -i (exact values make positions easy to check)
ae::Document rampDoc (std::int64_t frames, double rate = 1000.0)
{
    ae::Document d;
    d.audio.sampleRate = rate;
    d.audio.channels.assign (2, std::vector<float> ((std::size_t) frames));
    for (std::int64_t i = 0; i < frames; ++i)
    {
        d.audio.channels[0][(std::size_t) i] = (float) i;
        d.audio.channels[1][(std::size_t) i] = -(float) i;
    }
    return d;
}

ae::Document sineDoc (double freq, double seconds, double rate = 44100.0, float amplitude = 0.5f, int channels = 2)
{
    ae::Document d;
    d.audio.sampleRate = rate;
    const auto n = (std::size_t) std::llround (seconds * rate);
    d.audio.channels.assign ((std::size_t) channels, std::vector<float> (n));
    for (std::size_t i = 0; i < n; ++i)
        for (auto& ch : d.audio.channels)
            ch[i] = amplitude * (float) std::sin (2.0 * 3.141592653589793 * freq * (double) i / rate);
    return d;
}

// dominant frequency by zero crossings of channel 0 over [from, to)
double zeroCrossFreq (const ae::Audio& a, std::int64_t from, std::int64_t to)
{
    int crossings = 0;
    const auto& x = a.channels[0];
    for (auto i = from + 1; i < to; ++i)
        if ((x[(std::size_t) i - 1] < 0.0f) != (x[(std::size_t) i] < 0.0f))
            ++crossings;
    return crossings * 0.5 / ((double) (to - from) / a.sampleRate);
}
} // namespace

TEST (audioedit_copy_cut_erase_insert)
{
    auto d = rampDoc (100);
    const auto c = ae::copy (d.audio, { 10, 20 });
    CHECK (c.frames() == 10 && c.numChannels() == 2 && c.channels[0][0] == 10.0f && c.channels[1][9] == -19.0f);

    auto cutDoc = d;
    const auto clip = ae::cut (cutDoc, { 10, 20 });
    CHECK (clip.frames() == 10 && clip.channels[0][3] == 13.0f);
    CHECK (cutDoc.audio.frames() == 90 && cutDoc.audio.channels[0][10] == 20.0f && cutDoc.audio.channels[1][9] == -9.0f);

    auto erased = d;
    ae::erase (erased, { 0, 50 });
    CHECK (erased.audio.frames() == 50 && erased.audio.channels[0][0] == 50.0f);
    ae::erase (erased, { 40, 40 }); // empty: nothing
    CHECK (erased.audio.frames() == 50);
    ae::erase (erased, { 45, 999 }); // clamped
    CHECK (erased.audio.frames() == 45);

    // paste insert puts the clip back where it was
    ae::insert (cutDoc, 10, clip);
    CHECK (cutDoc.audio.frames() == 100);
    bool same = true;
    for (int i = 0; i < 100; ++i)
        same = same && cutDoc.audio.channels[0][(std::size_t) i] == (float) i && cutDoc.audio.channels[1][(std::size_t) i] == -(float) i;
    CHECK (same);

    // into an empty document: the clip becomes the audio
    ae::Document empty;
    ae::insert (empty, 0, clip);
    CHECK (empty.audio.frames() == 10 && empty.audio.sampleRate == 1000.0);
}

TEST (audioedit_paste_replace_mix_conform)
{
    auto d = rampDoc (100);
    ae::Audio mono;
    mono.sampleRate = 1000.0;
    mono.channels = { std::vector<float> (5, 1.0f) };

    auto r = d;
    ae::pasteReplace (r, { 10, 30 }, mono); // 20 frames become 5, mono duplicated to both sides
    CHECK (r.audio.frames() == 85 && r.audio.channels[0][10] == 1.0f && r.audio.channels[1][14] == 1.0f && r.audio.channels[0][15] == 30.0f);

    auto m = d;
    ae::pasteMix (m, 98, mono); // summed, extends past the end
    CHECK (m.audio.frames() == 103 && m.audio.channels[0][98] == 99.0f && m.audio.channels[1][99] == -98.0f && m.audio.channels[0][102] == 1.0f);

    // stereo → mono averages; another rate is resampled to the document's
    ae::Audio stereo;
    stereo.sampleRate = 2000.0;
    stereo.channels = { std::vector<float> (200, 0.8f), std::vector<float> (200, 0.2f) };
    const auto c = ae::conform (stereo, 1, 1000.0);
    CHECK (c.numChannels() == 1 && c.frames() == 100 && c.sampleRate == 1000.0);
    CHECK (std::abs (c.channels[0][50] - 0.5f) < 1.0e-4f && std::abs (c.channels[0][0] - 0.5f) < 1.0e-3f);
}

TEST (audioedit_trim_clear_silence_after_loop)
{
    auto d = rampDoc (100);
    ae::trim (d, { 20, 30 });
    CHECK (d.audio.frames() == 10 && d.audio.channels[0][0] == 20.0f && d.audio.channels[0][9] == 29.0f);

    auto c = rampDoc (100);
    ae::clear (c, { 10, 20 });
    CHECK (c.audio.frames() == 100 && c.audio.channels[0][10] == 0.0f && c.audio.channels[1][19] == 0.0f && c.audio.channels[0][20] == 20.0f);

    auto s = rampDoc (100);
    ae::insertSilence (s, 50, 25);
    CHECK (s.audio.frames() == 125 && s.audio.channels[0][50] == 0.0f && s.audio.channels[0][74] == 0.0f && s.audio.channels[0][75] == 50.0f);

    auto l = rampDoc (100);
    ae::setLoop (l, { 10, 60 });
    ae::deleteAfterLoop (l);
    CHECK (l.audio.frames() == 60 && l.loopStart == 10 && l.loopEnd == 60);
}

TEST (audioedit_fades_and_curves)
{
    for (auto c : { ae::Curve::linear, ae::Curve::smooth, ae::Curve::exponential, ae::Curve::logarithmic })
    {
        CHECK (ae::curveGain (c, 0.0f) == 0.0f);
        CHECK (std::abs (ae::curveGain (c, 1.0f) - 1.0f) < 1.0e-6f);
        // monotonic
        float prev = -1.0f;
        bool rising = true;
        for (int i = 0; i <= 20; ++i)
        {
            const auto g = ae::curveGain (c, (float) i / 20.0f);
            rising = rising && g >= prev;
            prev = g;
        }
        CHECK (rising);
    }
    CHECK (ae::curveGain (ae::Curve::exponential, 0.5f) < 0.5f && ae::curveGain (ae::Curve::logarithmic, 0.5f) > 0.5f);
    CHECK (std::abs (ae::curveGain (ae::Curve::smooth, 0.5f) - 0.5f) < 1.0e-6f);

    ae::Document d;
    d.audio.sampleRate = 1000.0;
    d.audio.channels = { std::vector<float> (100, 1.0f) };
    auto in = d;
    ae::fadeIn (in, { 0, 11 }, ae::Curve::linear);
    CHECK (in.audio.channels[0][0] == 0.0f && std::abs (in.audio.channels[0][5] - 0.5f) < 1.0e-6f && in.audio.channels[0][10] == 1.0f && in.audio.channels[0][50] == 1.0f);
    auto out = d;
    ae::fadeOut (out, { 89, 100 }, ae::Curve::smooth);
    CHECK (out.audio.channels[0][88] == 1.0f && out.audio.channels[0][89] == 1.0f && out.audio.channels[0][99] == 0.0f);

    // declick: a short (4 ms) fade at the edge of the range only
    ae::Document k;
    k.audio.sampleRate = 10000.0;
    k.audio.channels = { std::vector<float> (1000, 1.0f) };
    ae::declickIn (k, {});
    ae::declickOut (k, {});
    CHECK (k.audio.channels[0][0] == 0.0f && k.audio.channels[0][40] == 1.0f && k.audio.channels[0][999] == 0.0f && k.audio.channels[0][959] == 1.0f);
}

TEST (audioedit_gain_normalize_polarity_dc)
{
    auto d = sineDoc (100.0, 0.1, 8000.0, 0.25f);
    const auto g = ae::normalize (d, {}, 0.0f);
    CHECK (std::abs (g - 4.0f) < 0.01f);
    CHECK (std::abs (ae::peak (d.audio, { 0, d.audio.frames() }) - 1.0f) < 1.0e-5f);
    ae::normalize (d, {}, -6.0f);
    CHECK (std::abs (ae::peak (d.audio, { 0, d.audio.frames() }) - 0.501187f) < 1.0e-4f);

    // silence is left alone
    auto s = ae::Document {};
    s.audio = ae::silence (2, 44100.0, 100);
    CHECK (ae::normalize (s, {}) == 1.0f);

    // gain + pan: pan right turns the left side down, the right side stays
    auto p = rampDoc (10);
    ae::amp (p, {}, 0.5f, 0.5f);
    CHECK (std::abs (p.audio.channels[0][4] - 4.0f * 0.25f) < 1.0e-6f && std::abs (p.audio.channels[1][4] + 4.0f * 0.5f) < 1.0e-6f);

    auto inv = rampDoc (10);
    ae::invert (inv, { 2, 4 });
    CHECK (inv.audio.channels[0][2] == -2.0f && inv.audio.channels[1][3] == 3.0f && inv.audio.channels[0][4] == 4.0f);

    // DC removal: a sine on a 0.3 offset ends up centred
    auto dc = sineDoc (50.0, 0.2, 1000.0, 0.5f, 1);
    for (auto& v : dc.audio.channels[0])
        v += 0.3f;
    ae::removeDc (dc, {});
    double mean = 0.0;
    for (auto v : dc.audio.channels[0])
        mean += v;
    CHECK (std::abs (mean / (double) dc.audio.frames()) < 1.0e-5);
    CHECK (ae::rms (dc.audio, { 0, dc.audio.frames() }) > 0.34f && ae::rms (dc.audio, { 0, dc.audio.frames() }) < 0.36f);
}

TEST (audioedit_reverse_channels)
{
    auto d = rampDoc (10);
    ae::addMarker (d, 2, "a");
    ae::addRegion (d, { 1, 3 }, "r");
    ae::reverse (d, { 0, 10 });
    CHECK (d.audio.channels[0][0] == 9.0f && d.audio.channels[1][9] == 0.0f);
    // markers inside are mirrored (region [1,3) → [7,9), marker 2 → 8)
    CHECK (d.markers.size() == 2 && d.markers[0].start == 7 && d.markers[0].end == 9 && d.markers[1].start == 8 && d.markers[1].name == "a");

    auto s = rampDoc (4);
    ae::swapChannels (s);
    CHECK (s.audio.channels[0][3] == -3.0f && s.audio.channels[1][3] == 3.0f);
    auto m = rampDoc (4);
    ae::toMono (m, ae::MonoSource::merged);
    CHECK (m.audio.numChannels() == 1 && m.audio.channels[0][3] == 0.0f);
    auto r = rampDoc (4);
    ae::toMono (r, ae::MonoSource::right);
    CHECK (r.audio.channels[0][2] == -2.0f);
    ae::toStereo (r);
    CHECK (r.audio.numChannels() == 2 && r.audio.channels[1][2] == -2.0f);
}

TEST (audioedit_resample_speed_stretch_pitch)
{
    // resampling keeps the pitch and the duration
    auto d = sineDoc (440.0, 0.5, 44100.0);
    ae::resample (d, 22050.0);
    CHECK (d.audio.sampleRate == 22050.0 && d.audio.frames() == 11025);
    CHECK (std::abs (zeroCrossFreq (d.audio, 1000, 10000) - 440.0) < 3.0);
    CHECK (std::abs (ae::peak (d.audio, { 1000, 10000 }) - 0.5f) < 0.01f);

    // a tone above the new Nyquist is filtered out instead of aliasing
    auto hi = sineDoc (15000.0, 0.2, 44100.0);
    ae::resample (hi, 22050.0);
    CHECK (ae::rms (hi.audio, { 500, hi.audio.frames() - 500 }) < 0.01f);

    // DC stays DC (also at the edges)
    std::vector<float> dc (100, 0.7f);
    const auto up = ae::resampleChannel (dc, 333);
    bool flat = true;
    for (auto v : up)
        flat = flat && std::abs (v - 0.7f) < 1.0e-4f;
    CHECK (up.size() == 333 && flat);

    // speed change: half as long, one octave up
    auto s = sineDoc (440.0, 0.5, 44100.0);
    ae::changeSpeed (s, {}, 0.5);
    CHECK (s.audio.frames() == 11025);
    CHECK (std::abs (zeroCrossFreq (s.audio, 500, 10500) - 880.0) < 6.0);

    // time stretch: twice as long, same pitch; markers after the range shift, inside scale
    auto t = sineDoc (440.0, 0.5, 44100.0);
    ae::addMarker (t, 11025, "mid");
    ae::addMarker (t, 22000, "end");
    ae::timeStretch (t, { 0, 11025 }, 2.0);
    CHECK (t.audio.frames() == 22050 + 11025);
    CHECK (std::abs (zeroCrossFreq (t.audio, 2000, 20000) - 440.0) < 6.0);
    CHECK (t.markers.size() == 2 && t.markers[0].start == 22050 && t.markers[1].start == 22000 + 11025);

    // pitch shift: same length, +12 semitones = double frequency
    auto p = sineDoc (440.0, 0.5, 44100.0);
    ae::pitchShift (p, {}, 12.0);
    CHECK (p.audio.frames() == 22050);
    CHECK (std::abs (zeroCrossFreq (p.audio, 3000, 19000) - 880.0) < 12.0);
}

TEST (audioedit_markers_follow_edits)
{
    auto d = rampDoc (100);
    ae::addMarker (d, 10, "a");
    ae::addMarker (d, 50, "b");
    ae::addRegion (d, { 60, 80 }, "r");
    ae::setLoop (d, { 40, 90 });
    CHECK (d.markers.size() == 3 && d.markers[2].isRegion());

    auto e = d;
    ae::erase (e, { 45, 70 }); // marker 50 dropped, region clipped to [45, 55), loop [40, 65)
    CHECK (e.markers.size() == 2 && e.markers[1].start == 45 && e.markers[1].end == 55 && e.loopStart == 40 && e.loopEnd == 65);

    auto i = d;
    ae::insertSilence (i, 20, 5);
    CHECK (i.markers[0].start == 10 && i.markers[1].start == 55 && i.markers[2].start == 65 && i.loopEnd == 95);

    auto t = d;
    ae::trim (t, { 50, 100 }); // marker at 50 stays at the new start
    CHECK (t.markers.size() == 2 && t.markers[0].start == 0 && t.markers[1].start == 10 && t.loopStart == 0 && t.loopEnd == 40);

    // erasing the loop entirely removes it
    auto l = d;
    ae::erase (l, { 30, 95 });
    CHECK (! l.hasLoop());

    // removing: markers in a range, regions overlapping it
    auto r = d;
    CHECK (ae::removeMarkers (r, { 0, 61 }) == 3);
    auto r2 = d;
    CHECK (ae::removeMarkers (r2, { 50, 50 }) == 1 && r2.markers.size() == 2);

    // resampling scales them
    auto s = rampDoc (100, 1000.0);
    ae::addMarker (s, 50);
    ae::setLoop (s, { 10, 20 });
    ae::resample (s, 2000.0);
    CHECK (s.markers[0].start == 100 && s.loopStart == 20 && s.loopEnd == 40);

    ae::setLoop (s, {});
    CHECK (! s.hasLoop());
}

TEST (audioedit_zero_crossing_and_onsets)
{
    auto d = sineDoc (100.0, 0.1, 1000.0, 0.5f, 1); // crossings every 5 frames
    const auto z = ae::nearestZeroCrossing (d.audio, 12, 10);
    CHECK (z == 10 || z == 11);
    CHECK (ae::nearestZeroCrossing (d.audio, 12, 0) == 12);

    // clicks at known places on low noise
    ae::Document c;
    c.audio = ae::silence (1, 44100.0, 44100);
    std::uint32_t seed = 1;
    for (auto& v : c.audio.channels[0])
    {
        seed = seed * 1664525u + 1013904223u;
        v = ((float) (seed >> 8) / 16777216.0f - 0.5f) * 0.001f;
    }
    for (std::int64_t at : { 4410, 15000, 30000 })
        for (int i = 0; i < 2000; ++i)
            c.audio.channels[0][(std::size_t) (at + i)] += 0.8f * std::exp (-(float) i / 300.0f) * (float) std::sin (0.3 * i);
    const auto onsets = ae::detectOnsets (c.audio, 0.5f);
    CHECK (onsets.size() == 3);
    if (onsets.size() == 3)
        CHECK (std::abs (onsets[0] - 4410) < 300 && std::abs (onsets[1] - 15000) < 300 && std::abs (onsets[2] - 30000) < 300);
}

TEST (audioedit_history_undo_redo)
{
    ae::History h (std::size_t (1) << 30);
    auto d = rampDoc (100);
    h.reset (d, "Open");
    CHECK (! h.canUndo() && ! h.canRedo() && h.size() == 1);

    ae::erase (d, { 0, 10 });
    h.commit (d, "Delete");
    ae::reverse (d, {});
    h.commit (d, "Reverse");
    CHECK (h.size() == 3 && h.getPosition() == 2 && h.label (1) == "Delete");

    CHECK (h.undo().audio.frames() == 90 && h.current().audio.channels[0][0] == 10.0f);
    CHECK (h.undo().audio.frames() == 100);
    CHECK (! h.canUndo() && h.canRedo());
    CHECK (h.redo().audio.frames() == 90);
    CHECK (h.jumpTo (2).audio.channels[0][0] == 99.0f);

    // a new edit after undo drops the redo steps
    h.jumpTo (0);
    auto e = h.current();
    ae::clear (e, {});
    h.commit (e, "Clear");
    CHECK (h.size() == 2 && ! h.canRedo() && h.label (1) == "Clear");

    // memory bound: oldest states go, the current and one before it stay
    ae::History small (1000);
    auto s = rampDoc (100); // 800 bytes per state
    small.reset (s, "Open");
    small.commit (s, "a");
    small.commit (s, "b");
    CHECK (small.size() == 2 && small.label (0) == "a" && small.canUndo());
}

TEST (audioedit_spectrogram)
{
    const auto d = sineDoc (1000.0, 0.5, 44100.0, 1.0f);
    const auto s = ae::spectrogram (d.audio, { 0, d.audio.frames() }, 8, 2048);
    CHECK (s.columns == 8 && s.bins == 1024);
    // the strongest bin is at 1000 Hz and reads about 0 dB
    int best = 0;
    for (int b = 1; b < s.bins; ++b)
        if (s.at (4, b) > s.at (4, best))
            best = b;
    const auto hz = best * 44100.0 / 2048.0;
    CHECK (std::abs (hz - 1000.0) < 25.0);
    CHECK (s.at (4, best) > -3.0f && s.at (4, best) < 0.5f);
    CHECK (s.at (4, 600) < -60.0f);
    CHECK (ae::spectrogram (d.audio, { 0, 100 }, 4, 1000).columns == 0); // not a power of two
}

TEST (audioedit_wav_roundtrip_with_markers)
{
    auto d = sineDoc (440.0, 0.05, 48000.0, 0.5f);
    ae::addMarker (d, 100, "kick");
    ae::addRegion (d, { 500, 900 }, "snare");
    ae::addMarker (d, 1200); // unnamed
    ae::setLoop (d, { 200, 2000 });

    for (auto format : { ae::WavFormat::int16, ae::WavFormat::int24, ae::WavFormat::float32 })
    {
        const auto bytes = ae::encodeWav (d, format);
        ae::Document back;
        CHECK (ae::decodeWav (bytes.data(), bytes.size(), back));
        CHECK (back.audio.sampleRate == 48000.0 && back.audio.numChannels() == 2 && back.audio.frames() == d.audio.frames());
        const auto tolerance = format == ae::WavFormat::int16 ? 1.0e-4f : format == ae::WavFormat::int24 ? 1.0e-6f : 0.0f;
        float err = 0.0f;
        for (std::size_t i = 0; i < (std::size_t) d.audio.frames(); ++i)
            err = std::max (err, std::abs (back.audio.channels[1][i] - d.audio.channels[1][i]));
        CHECK (err <= tolerance);
        CHECK (back.markers.size() == 3);
        if (back.markers.size() == 3)
        {
            CHECK (back.markers[0].start == 100 && back.markers[0].name == "kick" && ! back.markers[0].isRegion());
            CHECK (back.markers[1].start == 500 && back.markers[1].end == 900 && back.markers[1].name == "snare");
            CHECK (back.markers[2].start == 1200 && back.markers[2].name.empty());
        }
        CHECK (back.loopStart == 200 && back.loopEnd == 2000);
        // RIFF size is consistent
        CHECK ((std::size_t) (bytes[4] | bytes[5] << 8 | bytes[6] << 16 | bytes[7] << 24) == bytes.size() - 8);
    }

    // markers only (the audio comes from another decoder)
    const auto bytes = ae::encodeWav (d, ae::WavFormat::int16);
    ae::Document only;
    only.audio = ae::silence (2, 48000.0, d.audio.frames());
    CHECK (ae::decodeWavMarkers (bytes.data(), bytes.size(), only) && only.markers.size() == 3 && only.loopEnd == 2000);

    // mono, no markers, and not-a-WAV
    ae::Document mono;
    mono.audio = ae::silence (1, 22050.0, 10);
    const auto m = ae::encodeWav (mono);
    ae::Document mb;
    CHECK (ae::decodeWav (m.data(), m.size(), mb) && mb.audio.numChannels() == 1 && mb.markers.empty() && ! mb.hasLoop());
    const std::uint8_t junk[16] = { 'R', 'I', 'F', 'X' };
    CHECK (! ae::decodeWav (junk, sizeof (junk), mb));
}

// What FL hands a plugin about a WAV besides its audio (round 165g): cue / region starts, ACID beats
TEST (audioedit_wav_extras_markers_and_acid)
{
    auto d = sineDoc (440.0, 0.05, 44100.0, 0.5f);
    ae::addRegion (d, { 1500, 1900 }, "b");
    ae::addMarker (d, 300, "a");
    ae::addMarker (d, 2000);
    auto bytes = ae::encodeWav (d, ae::WavFormat::int16);
    // an ACID chunk: flags, root note, ?, ?, beats = 8, meter 4/4, tempo 120
    const std::uint8_t acid[8 + 24] = { 'a', 'c', 'i', 'd', 24, 0, 0, 0, 2, 0, 0, 0, 60, 0, 0, 0x80, 0, 0, 0, 0,
                                        8, 0, 0, 0, 4, 0, 4, 0, 0, 0, 0xf0, 0x42 };
    bytes.insert (bytes.end(), acid, acid + sizeof (acid));
    const auto riff = (std::uint32_t) bytes.size() - 8;
    for (int k = 0; k < 4; ++k)
        bytes[4 + (std::size_t) k] = (std::uint8_t) (riff >> (8 * k));
    const auto x = ae::decodeWavExtras (bytes.data(), bytes.size(), d.audio.frames());
    CHECK (x.markerStarts == (std::vector<std::int64_t> { 300, 1500, 2000 }));
    CHECK (x.acidBeats == 8.0f);
    // positions past the end are clamped; no chunks / not a WAV: nothing
    const auto clamped = ae::decodeWavExtras (bytes.data(), bytes.size(), 1000);
    CHECK (clamped.markerStarts == (std::vector<std::int64_t> { 300, 999, 999 }));
    ae::Document plain = sineDoc (440.0, 0.01);
    const auto p = ae::encodeWav (plain);
    const auto none = ae::decodeWavExtras (p.data(), p.size(), plain.audio.frames());
    CHECK (none.markerStarts.empty() && none.acidBeats == 0.0f);
    const std::uint8_t junk[16] = { 'R', 'I', 'F', 'X' };
    CHECK (ae::decodeWavExtras (junk, sizeof (junk), 10).markerStarts.empty());
}

// FL's auto slicing (core/FlAutoSlicer, round 165g): slice starts of the installation's Fruity Granulizer DLL on this
// sample (5 square-wave hits; stereo 44.1 kHz and mono 48 kHz) [EXEC FL, tmp/r165g/py/gold.py]
#include "core/FlAutoSlicer.h"
TEST (fl_auto_slicer_matches_fl)
{
    const int n = 60000;
    std::vector<float> x ((std::size_t) n, 0.0f);
    std::vector<int> acc ((std::size_t) n, 0);
    for (const int h : { 0, 9000, 21000, 30500, 47000 })
        for (int i = h; i < n; ++i)
        {
            const int k = i - h, a = 20000 >> (k / 1500);
            if (a == 0)
                break;
            acc[(std::size_t) i] = std::clamp (acc[(std::size_t) i] + ((k / 20) % 2 == 0 ? a : -a), -32768, 32767);
        }
    for (int i = 0; i < n; ++i)
        x[(std::size_t) i] = (float) acc[(std::size_t) i] / 32768.0f;
    const std::vector<std::int64_t> fl { 0, 9000, 21000, 30480, 46980 };
    std::vector<float> r = x;
    CHECK (lynx::flslice::autoSlice (x.data(), r.data(), n, 44100.0) == fl);
    CHECK (lynx::flslice::autoSlice (x.data(), nullptr, n, 48000.0) == fl);
    CHECK (lynx::flslice::autoSlice (nullptr, nullptr, 0, 44100.0).empty());
}
