// Round 165g flnewt165: Newtone's engine against FL's (grain size and gain, tremolo, pitch ramps), the editor's tools
// (cut / glue, Center / Variation / Transition, snap to scale, score) and LYNX's format version 2.
// docs/analise/REA_rodada165g-flnewt165.md

namespace flnewt165
{
// a sung vowel: harmonics of f0 (1/k) through a resonance at 700 Hz, `seconds` long, 44.1 kHz
lynx::SampleData vowel (double f0, double seconds)
{
    lynx::SampleData s;
    s.sampleRate = 44100.0;
    const auto n = (int) (seconds * 44100.0);
    s.left.assign ((std::size_t) n, 0.0f);
    for (int k = 1; k * f0 < 8000.0; ++k)
    {
        const auto f = k * f0;
        const auto res = 1.0 / std::sqrt (1.0 + std::pow ((f - 700.0) / 120.0, 2.0)) + 0.15 / std::sqrt (1.0 + std::pow ((f - 1220.0) / 150.0, 2.0));
        const auto a = (float) (0.25 * res / k);
        for (int i = 0; i < n; ++i)
            s.left[(std::size_t) i] += a * (float) std::sin (2.0 * M_PI * f * i / 44100.0);
    }
    return s;
}

lynx::newtonefx::Track oneNote (std::int64_t frames, double pitch)
{
    lynx::newtonefx::Track t;
    t.frames = frames;
    lynx::newtonefx::Note p;
    p.origStart = p.curStart = 0;
    p.origEnd = p.curEnd = frames;
    p.origPitch = pitch;
    p.smoothIn = p.smoothOut = p.origSmoothIn = p.origSmoothOut = 3000.0;
    t.pitches.push_back (p);
    t.notes = 1;
    return t;
}

double rmsDb (const float* x, int n)
{
    double e = 0.0;
    for (int i = 0; i < n; ++i)
        e += (double) x[i] * x[i];
    return 10.0 * std::log10 (std::max (1e-30, e / std::max (1, n)));
}

double noteAt (const lynx::SampleData& s, int from, int length)
{
    for (const auto& f : lynx::vocal::analysePitch (s.leftData() + from, length, s.sampleRate, 512))
        if (f.hz > 0.0)
            return lynx::vocal::hzToNote (f.hz);
    return 0.0;
}
} // namespace flnewt165

TEST (flnewt165_newtone_levels_follow_fl)
{
    using namespace lynx;
    // FL keeps a shifted note's level within ~2 dB of the source (no gain correction, grains of two output periods;
    // REA_rodada165g-flnewt165 §1.1): +12 st +0.5 dB, −12 st −2.2 dB, formant ×2 −3.6 dB on a steady vowel. LYNX's old
    // engine gave −8.5, +2.3 and +2.3 dB there.
    const auto s = flnewt165::vowel (223.0, 1.0);
    const auto n = (int) s.frames();
    const auto src = flnewt165::rmsDb (s.leftData() + 6000, n - 12000);
    struct Case
    {
        double shift, formant, lo, hi;
    } cases[] = { { 12.0, 1.0, -1.5, 1.5 }, { -12.0, 1.0, -4.0, -0.5 }, { 7.0, 1.0, -1.5, 1.5 }, { 0.0, 2.0, -5.5, -2.0 }, { 0.0, 0.5, -1.5, 1.5 } };
    for (const auto& c : cases)
    {
        auto t = flnewt165::oneNote (n, 57.12);
        t.pitches[0].shift = c.shift;
        t.pitches[0].formant = c.formant;
        const auto out = newtonefx::renderEdits (s, t);
        CHECK (out != nullptr && (int) out->frames() == n);
        if (out == nullptr)
            continue;
        const auto d = flnewt165::rmsDb (out->leftData() + 6000, n - 12000) - src;
        CHECK (d > c.lo && d < c.hi);
        // the pitch lands where asked (formant changes keep it)
        CHECK (std::abs (flnewt165::noteAt (*out, 20000, 8192) - (flnewt165::noteAt (s, 20000, 8192) + c.shift)) < 0.1);
    }
}

TEST (flnewt165_newtone_tremolo_and_ramps)
{
    using namespace lynx;
    const auto s = flnewt165::vowel (220.0, 1.0);
    const auto n = (int) s.frames();
    // tremolo: gain 1 + T·sin(2π t / V) in step with the vibrato, whatever the vibrato's depth (FL [EXEC])
    for (const auto depth : { 0.5, 0.001 })
    {
        auto t = flnewt165::oneNote (n, 57.0);
        auto& p = t.pitches[0];
        p.vibPeriod = 0.5; // (seconds)
        p.vibAmpStart = p.vibAmpEnd = depth;
        const auto plain = newtonefx::renderEdits (s, t); // the same note without tremolo: the reference level
        p.tremAmp = 0.5;
        const auto out = newtonefx::renderEdits (s, t);
        CHECK (out != nullptr && plain != nullptr);
        if (out == nullptr || plain == nullptr)
            continue;
        const auto at = [&] (double phase) { // level re no tremolo over 20 ms around that point of the first cycle
            const auto c = (int) (phase * 0.5 * 44100.0);
            return flnewt165::rmsDb (out->leftData() + c - 441, 882) - flnewt165::rmsDb (plain->leftData() + c - 441, 882);
        };
        CHECK (std::abs (at (0.25) - 20.0 * std::log10 (1.5)) < 0.1);
        CHECK (std::abs (at (0.75) - 20.0 * std::log10 (0.5)) < 0.15);
    }
    // ramps: +1 st at the start, gone after the smooth-in time; the end ramp reached at the end
    {
        auto t = flnewt165::oneNote (n, 57.0);
        t.pitches[0].rampIn = 1.0;
        t.pitches[0].rampOut = -1.0;
        t.pitches[0].smoothIn = t.pitches[0].smoothOut = 12000.0;
        CHECK (t.pitches[0].edited (1.0));
        const auto out = newtonefx::renderEdits (s, t);
        CHECK (out != nullptr);
        if (out != nullptr)
        {
            const auto base = flnewt165::noteAt (s, 20000, 4096);
            CHECK (std::abs (flnewt165::noteAt (*out, 1000, 2048) - (base + 0.9)) < 0.2);
            CHECK (std::abs (flnewt165::noteAt (*out, 20000, 4096) - base) < 0.05);
            CHECK (std::abs (flnewt165::noteAt (*out, n - 3048, 2048) - (base - 0.85)) < 0.3);
        }
    }
}

TEST (flnewt165_newtone_fl_fields_and_native_v2)
{
    using namespace lynx;
    using namespace newtonetest;
    // FL's XML: the ramps and the analysed smooth times are read (and a ramp alone is an edit)
    const auto xml = "<NewToneProject NumOfTracks=\"1\"><Tracks><Trk_0 TrackNum=\"0\" Type=\"class CzplJPositionablePSOLASource\" FilePath=\"a.wav\">"
                     "<AudioObject OriginalFileLength=\"1000\"><Pitch_0 fOrigPitch=\"60.0\" fCurPitch=\"60.0\" iOrigStartSamplePos=\"0\" iOrigEndSamplePos=\"1000\" "
                     "fPitchSmoothInTime=\"300.0\" fOrigPitchSmoothInTime=\"150.0\" fPitchSmoothOutTime=\"40.0\" fRampIn=\"0.5\" fRampOut=\"-0.25\"/>"
                     "</AudioObject></Trk_0></Tracks></NewToneProject>";
    const auto parsed = newtonefx::parse (state (0, 5, xml, ""));
    CHECK (parsed && parsed->tracks.size() == 1 && parsed->tracks[0].edited);
    if (parsed && ! parsed->tracks.empty() && ! parsed->tracks[0].pitches.empty())
    {
        const auto& p = parsed->tracks[0].pitches[0];
        CHECK (p.rampIn == 0.5 && p.rampOut == -0.25 && p.origSmoothIn == 150.0 && p.smoothIn == 300.0 && p.origSmoothOut == 40.0);
    }

    // version 2 round-trips the knobs, ramps and analysed times
    auto t = flnewt165::oneNote (44100, 57.0);
    t.centre = 0.5;
    t.variation = 0.25;
    t.transition = 1.5;
    t.pitches[0].rampIn = 2.0;
    t.pitches[0].origSmoothIn = 1234.0;
    const auto bytes = newtonefx::writeNative (t);
    const auto back = newtonefx::parseNative (bytes);
    CHECK (back && back->centre == 0.5 && back->variation == 0.25 && back->transition == 1.5 && back->pitches.size() == 1);
    if (back && ! back->pitches.empty())
        CHECK (back->pitches[0].rampIn == 2.0 && back->pitches[0].origSmoothIn == 1234.0);

    // version 1 (rounds 142–165) still loads: no knobs, no ramps, the analysed times = the current ones
    std::vector<std::uint8_t> v1 { 'L', 'X', 'N', 'T' };
    auto put = [&v1] (auto v) {
        const auto* b = reinterpret_cast<const std::uint8_t*> (&v);
        v1.insert (v1.end(), b, b + sizeof (v));
    };
    put ((std::uint32_t) 1);
    put ((std::int64_t) 1000);
    put (1.0);
    put ((std::uint32_t) 1);
    for (const auto v : { (std::int64_t) 0, (std::int64_t) 1000, (std::int64_t) 0, (std::int64_t) 1000 })
        put (v);
    for (const auto v : { 60.0, 2.0, 1.0, 1.0, 1.0, 1.0, 700.0, 500.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0 })
        put (v);
    v1.push_back (1);
    const auto old = newtonefx::parseNative (v1);
    CHECK (old && old->pitches.size() == 1 && old->edited && old->centre == 0.0 && old->transition == 1.0);
    if (old && ! old->pitches.empty())
        CHECK (old->pitches[0].shift == 2.0 && old->pitches[0].origSmoothIn == 700.0 && old->pitches[0].origSmoothOut == 500.0 && old->pitches[0].rampIn == 0.0);
}

TEST (flnewt165_newtone_editor_tools)
{
    using namespace lynx;
    using newtonefx::Note;
    newtonefx::Track t;
    t.frames = 30000;
    for (int k = 0; k < 3; ++k)
    {
        Note p;
        p.origStart = p.curStart = k * 10000;
        p.origEnd = p.curEnd = (k + 1) * 10000;
        p.origPitch = 60.3 + k; // sung 30 cents sharp
        p.smoothIn = p.smoothOut = p.origSmoothIn = p.origSmoothOut = 600.0;
        t.pitches.push_back (p);
    }
    t.pitches[1].shift = 2.0; // moved up two semitones by hand

    // Center: on the semitone the note is on, by the amount; 0 = as sung (keeping the hand-moved semitone)
    newtonefx::applyCentre (t, 1.0);
    CHECK (std::abs (t.pitches[0].origPitch + t.pitches[0].shift - 60.0) < 1e-9);
    CHECK (std::abs (t.pitches[1].origPitch + t.pitches[1].shift - 63.0) < 1e-9);
    newtonefx::applyCentre (t, 0.5, { 0 });
    CHECK (std::abs (t.pitches[0].origPitch + t.pitches[0].shift - 60.15) < 1e-9 && t.centre == 1.0); // (one note: the knob value stays)
    newtonefx::applyCentre (t, 0.0);
    CHECK (std::abs (t.pitches[0].shift) < 1e-9 && std::abs (t.pitches[1].shift - 2.0) < 1e-9 && t.centre == 0.0);

    // Variation = the drift, Transition = × the analysed smooth times
    newtonefx::applyVariation (t, 0.25);
    CHECK (t.pitches[2].drift == 0.25 && t.variation == 0.25 && t.edited);
    newtonefx::applyTransition (t, 0.5, { 1 });
    CHECK (t.pitches[1].smoothIn == 300.0 && t.pitches[0].smoothIn == 600.0);

    // snap to scale (C major): C#+0.3 → D (ties go down), and only scale notes are targets
    CHECK (newtonefx::nearestInScale (61.3, 0, 0b101010110101) == 62.0);
    CHECK (newtonefx::nearestInScale (61.0, 0, 0b101010110101) == 60.0);
    CHECK (newtonefx::nearestInScale (66.4, 7, 0b101010110101) == 66.0); // (G major has F#)
    CHECK (newtonefx::nearestInScale (61.6, 0, 0xFFF) == 62.0);
    newtonefx::snapToScale (t, 0, 0b101010110101);
    CHECK (std::abs (t.pitches[1].origPitch + t.pitches[1].shift - 64.0) < 1e-9); // (63.3 → E)

    // cut: two notes, every edit kept, the pitch of each half from the source; glue joins them back
    t.pitches[2].volume = 0.5;
    t.pitches[2].rampIn = 1.0;
    t.pitches[2].rampOut = -1.0;
    const auto pitchOf = [] (const void*, std::int64_t a, std::int64_t) { return a < 25000 ? 62.2 : 62.4; };
    CHECK (! newtonefx::splitNote (t, 2, 20000, pitchOf)); // (not strictly inside)
    CHECK (newtonefx::splitNote (t, 2, 25000, pitchOf) && t.pitches.size() == 4 && t.notes == 4);
    CHECK (t.pitches[2].curEnd == 25000 && t.pitches[3].curStart == 25000 && t.pitches[2].origEnd == 25000 && t.pitches[3].origStart == 25000);
    CHECK (t.pitches[2].volume == 0.5 && t.pitches[3].volume == 0.5 && t.pitches[2].rampIn == 1.0 && t.pitches[2].rampOut == 0.0
           && t.pitches[3].rampIn == 0.0 && t.pitches[3].rampOut == -1.0);
    CHECK (t.pitches[2].origPitch == 62.2 && t.pitches[3].origPitch == 62.4);
    CHECK (newtonefx::glueNote (t, 3) && t.pitches.size() == 3);
    CHECK (t.pitches[2].curEnd == 30000 && t.pitches[2].rampOut == -1.0 && std::abs (t.pitches[2].origPitch - 62.3) < 1e-9);
    t.pitches[2].curStart += 100; // (no longer adjacent: not glued)
    CHECK (! newtonefx::glueNote (t, 2) && ! newtonefx::glueNote (t, 0));

    // score: the sounding semitones, disabled notes left out
    t.pitches[0].enabled = false;
    const auto notes = newtonefx::score (t, 10000.0);
    CHECK (notes.size() == 2);
    if (notes.size() == 2)
        CHECK (notes[0].key == 64 && notes[0].start == 1.0 && notes[0].length == 1.0 && notes[1].key == 62);
}
