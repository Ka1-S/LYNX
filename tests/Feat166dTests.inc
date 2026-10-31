// Round 166d (issue #14 feature requests): 500 mixer inserts (FL 2026's dynamic mixer), "Fold to the scale" in the
// Piano Roll (LYNX's own) and FL's ghost channels (docs/analise/REA_rodada166d.md).

#include "core/FruityBasicFx.h"
#include "core/MixerOps.h"
#include "core/NoteTools.h"

namespace feat166d
{
// one sampler channel on insert 1 playing a held DC, `inserts` inserts; returns the audio thread's time per block (µs)
double idleInsertCost (int inserts, int blocks, std::vector<float>* firstBlock = nullptr)
{
    lynx::Song s;
    s.mixer = lynx::Song::defaultMixer (inserts);
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::sampler;
    c.samplePath = "dc";
    c.volume = 1.0f;
    c.mixerTrack = 1;
    s.channels.push_back (c);
    s.patterns.emplace_back();
    auto provider = [] (const std::string&) {
        auto d = std::make_shared<lynx::SampleData>();
        d->sampleRate = 48000.0;
        d->left.assign (48000 * 20, 0.25f);
        return std::shared_ptr<const lynx::SampleData> (d);
    };
    auto st = lynx::RackState::build (s, nullptr, 48000.0, provider);
    lynx::RackRenderer r;
    r.prepare (48000.0, 256);
    lynx::LiveEventQueue live;
    lynx::NoteEvent on;
    on.channel = 0;
    on.key = 60;
    on.velocity = 1.0f;
    live.push (on);
    std::vector<float> l (256), rr (256);
    for (int b = 0; b < 20; ++b) // warm up
    {
        r.process (st.get(), false, 0.0, 256, l.data(), rr.data(), &live);
        if (b == 0 && firstBlock != nullptr)
            *firstBlock = l;
    }
    double best = 1e30;
    for (int round = 0; round < 5; ++round) // the best of five: the machine is busy with other builds
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b)
            r.process (st.get(), false, 0.0, 256, l.data(), rr.data(), nullptr);
        const auto us = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count() / blocks;
        best = std::min (best, us);
    }
    return best;
}
} // namespace feat166d

TEST (s166d_the_mixer_takes_500_inserts_like_fl_2026)
{
    using namespace lynx;
    CHECK (maxMixerTracks == 501); // Master + 500 inserts (FL 2026: "You can add a maximum of 375 tracks" with 125 used)
    Song s;
    s.mixer = Song::defaultMixer (125);
    CHECK (mixerops::roomForInserts (s) == 375);
    CHECK (mixerops::insertTracks (s, 126, 1000) == 375); // clamped to the room left
    CHECK ((int) s.mixer.size() == 501);
    CHECK (s.mixer.back().name == "Insert 500");
    CHECK (mixerops::roomForInserts (s) == 0);
    CHECK (! mixerops::insertTrack (s, 3));
    CHECK (mixerops::ensureInsert (s, 900) == 500);
    // a channel routed to insert 480 (Channel settings' target spinner goes to 500)
    Song t;
    t.mixer = Song::defaultMixer (16);
    CHECK (mixerops::ensureInsert (t, 480) == 480);
    CHECK ((int) t.mixer.size() == 481);
}

TEST (s166d_insert_multiple_shifts_every_reference_at_once)
{
    using namespace lynx;
    Song s;
    s.mixer = Song::defaultMixer (6);
    s.mixer[4].name = "Bass bus";
    s.mixer[2].sends.push_back ({ 5, 0.5f });
    auto slot = makeEffectSlot (EffectKind::compressor);
    slot.sidechain = 4;
    s.mixer[1].effects.push_back (slot);
    ChannelDef c;
    c.mixerTrack = 3;
    s.channels.push_back (c);
    PlaylistTrack pt;
    pt.mixerTrack = 5;
    s.playlistTracks.push_back (pt);
    Automation a;
    a.target = { AutomationTargetKind::mixerVolume, 4 };
    s.automations.push_back (a);

    auto reference = s; // the same edit, one insert at a time
    for (int i = 0; i < 3; ++i)
        CHECK (mixerops::insertTrack (reference, 3 + i));

    CHECK (mixerops::insertTracks (s, 3, 3) == 3);
    CHECK ((int) s.mixer.size() == 10);
    CHECK (s.channels[0].mixerTrack == 6);
    CHECK (s.mixer[7].name == "Bass bus");
    CHECK (s.mixer[2].sends.size() == 1 && s.mixer[2].sends[0].target == 8);
    CHECK (s.mixer[1].effects[0].sidechain == 7);
    CHECK (s.playlistTracks[0].mixerTrack == 8);
    CHECK (s.automations[0].target.index == 7);
    CHECK (s.mixer[3].name == "Insert 3" && s.mixer[5].name == "Insert 5" && s.mixer[6].name == "Insert 6");
    CHECK (s.mixer[8].name == "Insert 8" && s.mixer[9].name == "Insert 9");
    CHECK (s.mixer.size() == reference.mixer.size());
    for (std::size_t t = 0; t < s.mixer.size() && t < reference.mixer.size(); ++t)
        CHECK (s.mixer[t].name == reference.mixer[t].name && s.mixer[t].sends == reference.mixer[t].sends
               && s.mixer[t].effects.size() == reference.mixer[t].effects.size());
    CHECK (s.mixer[1].effects[0].sidechain == reference.mixer[1].effects[0].sidechain);
    CHECK (s.channels == reference.channels);
    CHECK (s.playlistTracks[0].mixerTrack == reference.playlistTracks[0].mixerTrack);
    CHECK (s.automations[0].target.index == reference.automations[0].target.index);

    // 375 at the front of a full 125-insert mixer: one pass (the old swaps took ~100 000 song walks)
    Song big;
    big.mixer = Song::defaultMixer (125);
    for (int i = 0; i < 64; ++i)
    {
        ChannelDef ch;
        ch.mixerTrack = 1 + i;
        big.channels.push_back (ch);
    }
    const auto t0 = std::chrono::steady_clock::now();
    CHECK (mixerops::insertTracks (big, 1, 375) == 375);
    const auto ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
    std::printf ("  [166d] 375 inserts at the front of 125: %.2f ms\n", ms);
    CHECK (ms < 250.0);
    CHECK (big.channels[0].mixerTrack == 376 && big.channels[63].mixerTrack == 439);
    CHECK (big.mixer[376].name == "Insert 376" && big.mixer[1].name == "Insert 1");
}

TEST (s166d_fruity_send_targets_any_of_the_500_inserts_and_old_files_keep_theirs)
{
    using namespace lynx;
    auto slot = makeEffectSlot (EffectKind::fruitySend);
    flbasic::sendToParams ({ 0, 256, 256, 437 }, slot.params.data());
    CHECK (fruitySendTarget (slot) == 437);
    CHECK (flbasic::sendFromParams (slot.params.data()).track == 437);
    // the value survives the project file's 4 decimals
    const auto saved = (float) (std::lround (slot.params[3] * 10000.0f) / 10000.0);
    CHECK (flbasic::sendTrack (saved) == 437);
    // a project saved before round 166d normalised "Send to" over 0..127
    for (int track : { 0, 1, 2, 17, 64, 126, 127 })
    {
        const auto old = (float) (std::lround ((double) track / 127.0 * 10000.0) / 10000.0);
        CHECK (flbasic::sendTrack (flbasic::rescaleSendTrackValue (old, flbasic::legacySendSpan)) == track);
    }
    CHECK (flbasic::rescaleSendTrackValue (0.25f, maxMixerTracks - 1) == 0.25f); // today's files: unchanged
}

TEST (s166d_idle_inserts_cost_almost_nothing)
{
    // 500 inserts that nothing feeds next to 16: the audio thread's extra time per 256-frame block
    std::vector<float> a, b;
    const auto few = feat166d::idleInsertCost (16, 400, &a);
    const auto many = feat166d::idleInsertCost (500, 400, &b);
    std::printf ("  [166d] per 256-frame block: 16 inserts %.1f us, 500 inserts %.1f us (+%.2f us per idle insert)\n", few, many,
                 (many - few) / 484.0);
    CHECK (a == b); // the same sound
    CHECK (many - few < 400.0); // (5.3 ms per block at 48 kHz)
}

TEST (s166d_fold_to_scale_rows)
{
    using namespace lynx::notetools;
    int major = -1;
    for (int i = 0; i < (int) scales().size(); ++i)
        if (std::string (scales()[(std::size_t) i].name) == "Major")
            major = i;
    CHECK (major > 0);
    const auto all = keyRows (0, major, false);
    CHECK (all.count == 128 && all.keyOfRow[0] == 127 && all.rowOfKey[60] == 67);
    const auto none = keyRows (0, 0, true); // no scale chosen: nothing folds
    CHECK (none.count == 128);

    const auto c = keyRows (0, major, true); // C major: 7 of every 12 keys
    CHECK (c.count == 75); // 0..127: 10 full octaves (70) + 120..127 has C D E F G (5)
    for (int r = 1; r < c.count; ++r)
        CHECK (c.keyOfRow[(std::size_t) r] < c.keyOfRow[(std::size_t) r - 1]);
    for (int k = 0; k < 128; ++k)
        CHECK (c.shown (k) == inScale (k, 0, major));
    CHECK (c.keyOfRow[(std::size_t) c.rowOfKey[64]] == 64);
    // steps go through the scale: E +1 = F, F +1 = G, B +1 = C, C −1 = B
    CHECK (c.step (64, 1) == 65 && c.step (65, 1) == 67 && c.step (71, 1) == 72 && c.step (72, -1) == 71);
    CHECK (c.step (60, 7) == 72 && c.step (60, -7) == 48);
    // from a folded key (C#): one step up is D, one step down is C, no step is the key above
    CHECK (c.step (61, 1) == 62 && c.step (61, -1) == 60 && c.step (61, 0) == 62);
    CHECK (c.step (127, 3) == 127 && c.step (0, -3) == 0); // clamped to the rows
    CHECK (c.step (127, -1) == 125); // G (127) is in C major; the next shown key down is F (125)

    const auto a = keyRows (9, major, true); // A major: G# is in, G is not
    CHECK (a.shown (68) && ! a.shown (67) && a.shown (69));
}
