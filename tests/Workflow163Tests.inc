// Round 163 (user feedback): a clean / passive mixer (no built-in EQ processing), the inserts and Master chain a new
// project starts with, and hiding the unused inserts.

#include "core/MixerOps.h"
#include "core/Preferences.h"
#include "core/Templates.h"

namespace w163
{
// Renders 4096 samples of a tonal + noisy sample through insert 1 of `song` (channel 0 must exist).
std::vector<float> render (const lynx::Song& song)
{
    auto provider = [] (const std::string&) {
        auto d = std::make_shared<lynx::SampleData>();
        d->left.resize (48000);
        std::uint32_t seed = 1;
        for (std::size_t i = 0; i < d->left.size(); ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            d->left[i] = 0.3f * (float) std::sin ((double) i * 0.05) + 0.2f * ((float) (seed >> 8) / 16777216.0f - 0.5f);
        }
        return std::shared_ptr<const lynx::SampleData> (d);
    };
    auto state = lynx::RackState::build (song, nullptr, 48000.0, provider);
    lynx::RackRenderer r;
    r.prepare (48000.0, 256);
    lynx::LiveEventQueue live;
    lynx::NoteEvent on;
    on.channel = 0;
    on.velocity = 1.0f;
    live.push (on);
    std::vector<float> out;
    std::vector<float> l (512), rr (512);
    for (int b = 0; b < 8; ++b)
    {
        r.process (state.get(), false, 0.0, 512, l.data(), rr.data(), b == 0 ? &live : nullptr);
        out.insert (out.end(), l.begin(), l.end());
        out.insert (out.end(), rr.begin(), rr.end());
    }
    return out;
}

lynx::Song oneSampler()
{
    lynx::Song s;
    s.mixer = lynx::Song::defaultMixer (2);
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::sampler;
    c.samplePath = "tone";
    c.volume = 1.0f;
    c.mixerTrack = 1;
    s.channels.push_back (c);
    s.patterns.emplace_back();
    return s;
}
} // namespace w163

TEST (workflow163_clean_mixer_bypasses_eq_bit_exact)
{
    auto plain = w163::oneSampler();
    const auto reference = w163::render (plain);

    auto eq = plain;
    eq.mixer[1].eq.gainDb = { 9.0f, -6.0f, 12.0f };
    eq.mixer[0].eq.gainDb = { 0.0f, 4.0f, 0.0f };
    const auto withEq = w163::render (eq);
    CHECK (withEq != reference); // the EQ does change the sound

    auto clean = eq;
    clean.cleanMixer = true;
    const auto bypassed = w163::render (clean);
    CHECK (bypassed.size() == reference.size());
    CHECK (bypassed == reference); // bit-identical to a mixer without EQ: nothing computed
    CHECK (clean.mixer[1].eq.active()); // the settings are kept for when it is switched off
}

TEST (workflow163_new_project_defaults)
{
    using namespace lynx;
    // as before: 16 inserts and LYNX's limiter on the Master
    auto song = Song::makeDefault();
    applyNewProjectDefaults (song, {});
    CHECK (song.mixer.size() == 17 && song.mixer[0].effects.size() == 1 && song.mixer[0].effects[0].kind == EffectKind::limiter);
    CHECK (! song.cleanMixer);

    // no inserts, nothing on the Master, clean: the template's channels go to the Master
    NewProjectDefaults bare;
    bare.inserts = 0;
    bare.master = MasterChain::empty;
    bare.cleanMixer = true;
    song = Song::makeDefault();
    applyNewProjectDefaults (song, bare);
    CHECK (song.mixer.size() == 1 && song.mixer[0].effects.empty() && song.cleanMixer);
    for (const auto& c : song.channels)
        CHECK (c.mixerTrack == 0);
    // a new channel still gets an insert of its own (added on demand)
    CHECK (song.claimFreeInsert() == 1 && song.mixer.size() == 2);

    // a clipper only, 4 inserts: the 5th channel (Synth, insert 5) goes to the Master, the others keep theirs
    NewProjectDefaults clip;
    clip.inserts = 4;
    clip.master = MasterChain::clipper;
    song = Song::makeDefault();
    applyNewProjectDefaults (song, clip);
    CHECK (song.mixer.size() == 5 && song.mixer[0].effects.size() == 1 && song.mixer[0].effects[0].kind == EffectKind::fruitySoftClipper);
    CHECK (song.channels[0].mixerTrack == 1 && song.channels[3].mixerTrack == 4 && song.channels[4].mixerTrack == 0);

    // many inserts
    NewProjectDefaults many;
    many.inserts = 125;
    song = makeTemplate (2);
    applyNewProjectDefaults (song, many);
    CHECK (song.mixer.size() == 126 && song.mixer[125].name == "Insert 125");

    // ui-options.txt round trip, clamped values
    NewProjectDefaults back;
    for (const auto& [k, v] : prefs::parseKeyValues (clip.toText()))
        back.readKey (k, v);
    CHECK (back == clip);
    back.readKey ("newInserts", "999");
    back.readKey ("playlistTracks", "0");
    back.readKey ("newMasterChain", "nonsense");
    CHECK (back.inserts == maxMixerTracks - 1 && back.playlistTracks == 1 && back.master == MasterChain::lynxDefault);
    back.readKey ("newInserts", "abc");
    CHECK (back.inserts == NewProjectDefaults::defaultInserts);
}

TEST (workflow163_unused_inserts)
{
    using namespace lynx;
    auto song = Song::makeDefault(); // channels on inserts 1..5
    CHECK (song.insertInUse (0) && song.insertInUse (1) && song.insertInUse (5) && ! song.insertInUse (6));
    song.mixer[8].volume = 0.5f; // a fader moved by hand
    song.mixer[9].colour = 0xFF112233u;
    song.mixer[10].effects.push_back (song.mixer[0].effects[0]);
    song.mixer[12].effects.push_back (song.mixer[0].effects[0]);
    song.mixer[12].effects[0].sidechain = 13; // a side-chain source is in use
    CHECK (song.insertInUse (8) && song.insertInUse (9) && song.insertInUse (10) && song.insertInUse (13) && ! song.insertInUse (11));
    Automation a;
    a.target = { AutomationTargetKind::mixerPan, 14, 0, 0 };
    song.automations.push_back (a);
    CHECK (song.insertInUse (14) && ! song.insertInUse (15));
    CHECK (song.nextFreeInsert() == 6);

    // removing the unused ones at the end: 15, 16 go; 14 (automated) stays
    CHECK (mixerops::removeUnusedTrailingInserts (song) == 2 && song.mixer.size() == 15);
    CHECK (mixerops::removeUnusedTrailingInserts (song) == 0);
    auto empty = Song::makeDefault();
    empty.channels.clear();
    CHECK (mixerops::removeUnusedTrailingInserts (empty, 3) == 13 && empty.mixer.size() == 4);
}
