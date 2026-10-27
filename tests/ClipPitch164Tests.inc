// --- Round 164e (misc164p): FL's audio clip / sample channel pitch as FL plays it (REA_rodada164e-clippitch) ---------

namespace c164p
{
struct FlCase
{
    const char* name;
    int type;          // 4 audio clip, 0 sampler
    int headerCents;   // 0xDB pitch
    int root;          // 0x87, -1 = none
    int stretchCents;  // 0xD7 +0x64
    int mode;          // 0xD7 +0x6C
    int mul;           // 0xD7 +0x68
    float time;        // 0xD7 +0x60 (1/192 beat)
    float semitones;   // LYNX's channel pitch after the import
    bool keepLength;
    float beats;       // stretchBeats (0 = none)
};

lynx::Song importCase (const FlCase& k, double sampleSeconds)
{
    FlpBuilder b (96);
    b.ascii (0xC7, "25.2.5.5319");
    b.u8 (0xA9); b.u32 (7); // float stretch time
    b.u8 (0x9C); b.u32 (86000);
    b.u8 (0x40); b.u16 (0);
    b.u8 (0x15); b.u8 ((std::uint8_t) k.type);
    if (k.root >= 0)
    {
        b.u8 (0x87); b.u32 ((std::uint32_t) k.root);
    }
    b.utf16 (0xC4, u"C:\\samples\\sine440.wav");
    std::vector<std::uint8_t> levels (24, 0);
    const std::int32_t lv[] = { 6400, 10000, k.headerCents };
    std::memcpy (levels.data(), lv, sizeof (lv));
    b.data (0xDB, levels);
    std::vector<std::uint8_t> params (168, 0);
    std::memcpy (&params[0x60], &k.time, 4);
    std::memcpy (&params[0x64], &k.stretchCents, 4);
    std::memcpy (&params[0x68], &k.mul, 4);
    std::memcpy (&params[0x6C], &k.mode, 4);
    const double end = 1.0;
    std::memcpy (&params[0x88], &end, 8);
    b.data (0xD7, params);

    lynx::FlpImportOptions o;
    o.fileExists = [] (const std::string&) { return true; };
    o.sampleSeconds = [sampleSeconds] (const std::string&) { return sampleSeconds; };
    return lynx::importFlp (flp::parseProject (b.bytes), o);
}
} // namespace c164p

TEST (c164p_fl_clip_and_sampler_pitch_import)
{
    // every row was rendered by FL 2026 (FL64.exe /R) from a 2 s 440 Hz sine at 86 BPM and measured (pitch, length)
    using c164p::FlCase;
    const FlCase cases[] = {
        { "clip base", 4, 0, -1, 0, 0, 0, 0.0f, 0.0f, false, 0.0f },                    // 440 Hz, 2.000 s
        { "clip header +400", 4, 400, -1, 0, 0, 0, 0.0f, 4.0f, false, 0.0f },          // +400.0 c, 1.587 s
        { "clip header -300", 4, -300, -1, 0, 0, 0, 0.0f, -3.0f, false, 0.0f },        // -300.0 c, 2.378 s
        { "clip root 48", 4, 0, 48, 0, 0, 0, 0.0f, 12.0f, false, 0.0f },               // +1200 c, 1.000 s
        { "clip root 67", 4, 0, 67, 0, 0, 0, 0.0f, -7.0f, false, 0.0f },               // -700 c, 2.996 s
        { "clip stretch pitch, resample", 4, 0, -1, 700, 0, 0, 0.0f, 0.0f, false, 0.0f }, // ignored: 0 c, 2.000 s
        { "clip stretch pitch, stretch", 4, 0, -1, 700, -1, 0, 0.0f, 7.0f, true, 0.0f },  // +698 c, 2.000 s
        { "clip stretch pitch, e3", 4, 0, -1, 700, 1, 0, 0.0f, 7.0f, true, 0.0f },        // +700 c, 2.000 s
        { "clip stretch pitch, mode 2", 4, 0, -1, 700, 2, 0, 0.0f, 7.0f, true, 0.0f },    // +695 c, 1.999 s
        { "clip stretch pitch, auto", 4, 0, -1, 700, 5, 0, 0.0f, 7.0f, true, 0.0f },      // +700 c, 2.000 s
        { "clip header +400, stretch", 4, 400, -1, 0, -1, 0, 0.0f, 4.0f, true, 0.0f },    // +398 c, 2.000 s
        { "clip header + stretch pitch", 4, 400, -1, 700, -1, 0, 0.0f, 11.0f, true, 0.0f }, // +1100 c, 2.004 s
        { "clip header + stretch pitch, resample", 4, 400, -1, 700, 0, 0, 0.0f, 4.0f, false, 0.0f }, // +400 c, 1.587 s
        { "clip root 67, stretch", 4, 0, 67, 0, -1, 0, 0.0f, -7.0f, true, 0.0f },         // -701 c, 2.001 s
        { "clip time 1 beat, resample", 4, 0, -1, 0, 0, 0, 192.0f, 0.0f, false, 1.0f },   // +1823 c, 0.698 s
        { "clip time 1 beat, stretch +700", 4, 0, -1, 700, -1, 0, 192.0f, 7.0f, true, 1.0f }, // +697 c, 0.692 s
        { "clip mul x2, resample", 4, 0, -1, 0, 0, 10000, 0.0f, 0.0f, false, 2.0f * 2.0f * 86.0f / 60.0f },   // -1200 c, 4.000 s
        { "clip mul x2, stretch", 4, 0, -1, 0, -1, 10000, 0.0f, 0.0f, true, 2.0f * 2.0f * 86.0f / 60.0f },    // -0.5 c, 4.004 s
        { "sampler stretch pitch, resample", 0, 0, -1, 700, 0, 0, 0.0f, 0.0f, false, 0.0f },  // 0 c, 2.000 s
        { "sampler stretch pitch, stretch", 0, 0, -1, 700, -1, 0, 0.0f, 7.0f, true, 0.0f },   // +698 c, 2.000 s
        { "sampler header +400, stretch", 0, 400, -1, 0, -1, 0, 0.0f, 4.0f, true, 0.0f },     // +398 c, 2.000 s
        { "sampler mul x2, resample", 0, 0, -1, 0, 0, 10000, 0.0f, 0.0f, false, 2.0f * 2.0f * 86.0f / 60.0f }, // -1200 c, 4.000 s
    };

    for (const auto& k : cases)
    {
        const auto song = c164p::importCase (k, 2.0);
        const bool ok = song.channels.size() == 1;
        CHECK (ok);
        if (! ok)
            continue;
        const auto& s = song.channels[0].sampler;
        const bool match = std::abs (s.pitch - k.semitones) < 1e-4f && s.keepPitch == k.keepLength && std::abs (s.stretchBeats - k.beats) < 1e-3f;
        if (! match)
            std::fprintf (stderr, "  %s: pitch %.3f keep %d beats %.4f\n", k.name, (double) s.pitch, (int) s.keepPitch, (double) s.stretchBeats);
        CHECK (match);
        CHECK (song.channels[0].instrument == (k.type == 4 ? lynx::InstrumentKind::audioClip : lynx::InstrumentKind::sampler));
    }

    // the stretch pitch is read from the file
    const c164p::FlCase raw { "raw", 4, 0, -1, -1200, 2, 0, 0.0f, 0.0f, false, 0.0f };
    const auto song = c164p::importCase (raw, 2.0);
    CHECK (song.channels.size() == 1 && std::abs (song.channels[0].sampler.pitch + 12.0f) < 1e-4f && song.channels[0].sampler.keepPitch);
}

TEST (c164p_stretch_panel_rules)
{
    using namespace lynx;
    SamplerSettings s;
    applyFlStretchPanel (s, 0, 700, 0, 0.0f, 2.0, 120.0); // Resample: nothing changes
    CHECK (s.pitch == 0.0f && ! s.keepPitch && s.stretchBeats == 0.0f);
    applyFlStretchPanel (s, 0, 0, -10000, 0.0f, 2.0, 120.0); // MUL x0.5: 1 s = 2 beats at 120 BPM
    CHECK (std::abs (s.stretchBeats - 2.0f) < 1e-5f && ! s.keepPitch);
    SamplerSettings t;
    t.pitch = 20.0f;
    applyFlStretchPanel (t, 5, 1200, 0, 0.0f, 0.0, 120.0); // clamped to LYNX's ±24 semitones
    CHECK (t.pitch == 24.0f && t.keepPitch);
    SamplerSettings u;
    applyFlStretchPanel (u, -1, 0, 5000, 384.0f, 2.0, 120.0); // with a TIME the MUL belongs to it (set by the caller)
    CHECK (u.stretchBeats == 0.0f && u.keepPitch);
    SamplerSettings v;
    applyFlStretchPanel (v, -1, 0, 5000, 0.0f, 0.0, 120.0); // unknown sample length: no stretch
    CHECK (v.stretchBeats == 0.0f);
}
