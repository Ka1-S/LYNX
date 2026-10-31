// --- REA round 146a: FL's mixer groups, separators and docks; Playlist track modes; Consolidate track -------------

#include "core/MixerOps.h"
#include "core/PlaylistOps.h"

TEST (rea146a_mixer_separators_groups_and_docks)
{
    using namespace lynx;
    auto song = Song::makeDefault();
    song.mixer.resize (9);

    // Create group on 3..5: a separator on 3 and on 6 (the track after the run); uncoloured tracks take the current's
    song.mixer[4].colour = 0xFF112233u;
    mixerops::createGroup (song, { 3, 4, 5 }, 4);
    CHECK (song.mixer[3].separator && song.mixer[6].separator && ! song.mixer[4].separator && ! song.mixer[5].separator);
    CHECK (song.mixer[3].colour == 0xFF112233u && song.mixer[5].colour == 0xFF112233u && song.mixer[2].colour == 0);
    CHECK ((mixerops::groupOf (song, 4) == std::vector<int> { 3, 4, 5 }));
    CHECK ((mixerops::groupOf (song, 7) == std::vector<int> { 6, 7, 8 }));          // from the separator on 6 to the end
    CHECK ((mixerops::groupOf (song, 1) == std::vector<int> { 1, 2 }) && mixerops::groupOf (song, 0).empty());

    // Auto color group: the group takes the clicked track's colour
    song.mixer[5].colour = 0xFF445566u;
    CHECK (mixerops::autoColorGroup (song, 5) == 2 && song.mixer[3].colour == 0xFF445566u && song.mixer[6].colour == 0);

    // Separator toggles on the selected tracks (never on the Master)
    mixerops::setSeparator (song, { 0, 2 }, true);
    CHECK (song.mixer[2].separator && ! song.mixer[0].separator);
    CHECK ((mixerops::groupOf (song, 1) == std::vector<int> { 1 }));

    // Dock to: the display order is left block, middle, right; a group stays inside its block
    mixerops::dockTracks (song, { 8 }, MixerDock::left);
    mixerops::dockTracks (song, { 1 }, MixerDock::right);
    const auto order = mixerops::displayOrder (song);
    CHECK (order.front() == 8 && order[1] == 0 && order.back() == 1 && order.size() == song.mixer.size());
    CHECK ((mixerops::groupOf (song, 7) == std::vector<int> { 6, 7 }));

    // moving inserts keeps a track's separator and dock with it; a playlist track's link follows
    song.playlistTracks.resize (2);
    song.playlistTracks[1].mode = PlaylistTrackMode::audio;
    song.playlistTracks[1].mixerTrack = 3;
    mixerops::swapTracks (song, 3, 2);
    CHECK (song.mixer[2].separator && song.playlistTracks[1].mixerTrack == 2);
    mixerops::deleteTrack (song, 2);
    CHECK (song.playlistTracks[1].mode == PlaylistTrackMode::unassigned && song.playlistTracks[1].mixerTrack == -1);
}

TEST (rea146a_flp_insert_flags_dock_and_separator)
{
    // FL's insert writer (FUN_00f3d620): flags = (dock & 3) << 6 | 0x400 separator | 0x08 enabled ...
    FlpBuilder b;
    b.ascii (0xC7, "25.1.0.4000");
    auto insert = [&b] (std::uint32_t flags) {
        std::vector<std::uint8_t> params (12, 0);
        std::memcpy (&params[4], &flags, 4);
        b.data (0xEC, params);
    };
    insert (0x0C);         // Master: dock 0 = left
    insert (0x4C);         // 1: middle
    insert (0x44C);        // 2: middle, separator
    insert (0x8C);         // 3: right
    insert (0x0C);         // 4: left
    for (int i = 0; i < 14; ++i)
        insert (0x4C);
    insert (0x0C);         // the last one is FL's "Current" track: stays in the middle
    const auto song = lynx::importFlp (flp::parseProject (b.bytes));
    CHECK (song.mixer.size() >= 5);
    if (song.mixer.size() >= 5)
    {
        CHECK (song.mixer[0].dock == lynx::MixerDock::left && song.mixer[1].dock == lynx::MixerDock::middle);
        CHECK (song.mixer[2].separator && ! song.mixer[1].separator && song.mixer[3].dock == lynx::MixerDock::right);
        CHECK (song.mixer[4].dock == lynx::MixerDock::left);
    }
    if (song.mixer.size() > 19)
        CHECK (song.mixer[19].dock == lynx::MixerDock::middle);
}

TEST (rea146a_playlist_track_modes)
{
    using namespace lynx;
    auto song = Song::makeDefault(); // channels 0..4 on inserts 1..5
    song.channels.push_back ({});
    song.channels.back().name = "Vox";
    song.channels.back().instrument = InstrumentKind::audioClip;
    const auto vox = (int) song.channels.size() - 1;
    song.channels[(std::size_t) vox].mixerTrack = 0;

    // an audio track takes the first free mixer track (nothing routed, no effects) and names it
    song.playlistTracks.resize (3);
    song.playlistTracks[2].name = "Vocals";
    CHECK (trackmodes::makeAudioTrack (song, 2));
    const auto mixer = song.playlistTracks[2].mixerTrack;
    CHECK (mixer == 6 && song.playlistTracks[2].mode == PlaylistTrackMode::audio && song.mixer[(std::size_t) mixer].name == "Vocals");
    CHECK (trackmodes::describe (song, 2) == "(Audio track, linked to mixer track 6)" && trackmodes::describe (song, 0).empty());

    // audio clips placed on it play through its mixer track (issue #15: only the placed ones); a channel also used on
    // an unassigned track simply follows (FL), one used on another audio track needs "Make clip unique?"
    Clip a;
    a.kind = ClipKind::audio;
    a.channel = vox;
    a.track = 2;
    a.length = 384;
    song.playlist = { a };
    CHECK (trackmodes::followAudioTracks (song, {}) == 0 && song.channels[(std::size_t) vox].mixerTrack == 0);
    CHECK (trackmodes::followAudioTracks (song, { 0 }) == 1 && song.channels[(std::size_t) vox].mixerTrack == mixer);
    Clip b = a;
    b.track = 0;
    b.start = 1000;
    song.playlist.push_back (b);
    song.channels[(std::size_t) vox].mixerTrack = 0; // routed by hand: stays until the clip is placed again
    CHECK (trackmodes::followAudioTracks (song, { 1 }) == 0 && song.channels[(std::size_t) vox].mixerTrack == 0);
    const auto channels = song.channels.size();
    CHECK (trackmodes::audioTrackConflicts (song, { 0 }).empty());
    CHECK (trackmodes::followAudioTracks (song, { 0 }) == 1 && song.channels.size() == channels && song.channels[(std::size_t) vox].mixerTrack == mixer);
    CHECK (trackmodes::followAudioTracks (song, { 0 }) == 0);

    // an instrument track: a generator (not an audio clip channel) and its mixer track
    CHECK (! trackmodes::makeInstrumentTrack (song, 1, vox));
    CHECK (trackmodes::makeInstrumentTrack (song, 1, 4));
    CHECK (song.playlistTracks[1].mode == PlaylistTrackMode::instrument && song.playlistTracks[1].channel == 4 && song.playlistTracks[1].mixerTrack == 5);
    CHECK (song.playlistTracks[1].name == "Synth" && trackmodes::describe (song, 1) == "(Instrument track)");
    // the same channel on another track moves there
    CHECK (trackmodes::makeInstrumentTrack (song, 0, 4) && song.playlistTracks[1].mode == PlaylistTrackMode::unassigned);
    // removing the channel unassigns its instrument track; a later channel's index follows
    CHECK (trackmodes::makeInstrumentTrack (song, 1, 3));
    song.removeChannel (4);
    CHECK (song.playlistTracks[0].mode == PlaylistTrackMode::unassigned && song.playlistTracks[1].channel == 3);
    trackmodes::unassign (song, 1);
    CHECK (song.playlistTracks[1].mode == PlaylistTrackMode::unassigned && song.playlistTracks[1].mixerTrack == -1);
}

TEST (rea146a_flp_playlist_track_modes)
{
    // record 0xEE (70 bytes): u32 mode at 49 (1 audio, 3 instrument), i32 mixer track at 53, i32 FL channel at 57
    FlpBuilder b;
    b.ascii (0xC7, "25.1.0.4000");
    b.u8 (0x40); b.u16 (0);
    b.u8 (0x15); b.u8 (0);
    b.u8 (0x40); b.u16 (7);
    b.u8 (0x15); b.u8 (0);
    for (int i = 0; i < 6; ++i)
    {
        std::vector<std::uint8_t> params (12, 0);
        params[4] = 0x4C;
        b.data (0xEC, params);
    }
    b.u8 (0x63); b.u16 (0);
    auto track = [&b] (std::uint32_t index, std::uint32_t mode, std::int32_t mixer, std::int32_t channel) {
        std::vector<std::uint8_t> r (70, 0);
        std::memcpy (&r[0], &index, 4);
        const std::uint32_t grey = 0x565148;
        std::memcpy (&r[4], &grey, 4);
        r[12] = 1;
        const float size = 1.0f;
        std::memcpy (&r[13], &size, 4);
        std::memcpy (&r[49], &mode, 4);
        std::memcpy (&r[53], &mixer, 4);
        std::memcpy (&r[57], &channel, 4);
        r[61] = 1;
        b.data (0xEE, r);
    };
    track (1, 1, 3, -1);
    track (2, 3, -1, 7);
    track (3, 0, -1, -1);
    const auto song = lynx::importFlp (flp::parseProject (b.bytes));
    CHECK (song.playlistTracks.size() >= 2);
    if (song.playlistTracks.size() >= 2)
    {
        CHECK (song.playlistTracks[0].mode == lynx::PlaylistTrackMode::audio && song.playlistTracks[0].mixerTrack == 3);
        CHECK (song.playlistTracks[1].mode == lynx::PlaylistTrackMode::instrument && song.playlistTracks[1].channel == 1);
    }
}

TEST (rea146a_consolidate_track)
{
    using namespace lynx;
    auto song = Song::makeDefault();
    song.playlist.clear();
    Clip c;
    c.pattern = 0;
    c.track = 1;
    c.start = 2 * ppq * 4;
    c.length = ppq * 4;
    song.playlist.push_back (c);
    c.start = 4 * ppq * 4;
    song.playlist.push_back (c);
    c.track = 0;
    c.start = 0;
    song.playlist.push_back (c);
    c.track = 1;
    c.muted = true;
    c.start = 6 * ppq * 4;
    song.playlist.push_back (c); // muted: not rendered and kept

    Song render;
    std::int64_t start = 0, length = 0;
    CHECK (! trackmodes::makeConsolidateSong (song, 5, false, render, start, length));
    CHECK (trackmodes::makeConsolidateSong (song, 1, false, render, start, length));
    CHECK (start == 2 * ppq * 4 && length == 3 * ppq * 4 && render.playlist.size() == 2 && render.playlist[0].start == 0);
    CHECK (render.mixer[0].effects.empty() && render.mode == PlayMode::song);
    CHECK (trackmodes::makeConsolidateSong (song, 1, true, render, start, length) && start == 0 && length == 5 * ppq * 4);

    const auto channel = trackmodes::applyConsolidate (song, 1, "/tmp/x.wav", 2 * ppq * 4, 3 * ppq * 4);
    CHECK (channel == (int) song.channels.size() - 1 && song.channels.back().instrument == InstrumentKind::audioClip);
    CHECK (song.channels.back().name == "Track 2 (consolidated)" && song.channels.back().mixerTrack == 0);
    int onTrack = 0;
    for (const auto& clip : song.playlist)
        if (clip.track == 1)
            ++onTrack;
    CHECK (onTrack == 2 && song.playlist.size() == 3); // the new clip and the muted one; track 0 keeps its clip
}

#include "core/FlWrapper.h"

TEST (rea146a_wrapper_settings_page_options)
{
    // FL's wrapper settings (round 149: in its form's order): Processing options, then Compatibility; the buffer
    // sub-options need fixed buffers
    const auto& o = lynx::wrapperOptions();
    CHECK (o.size() == 13); // round 151 added the MIDI ones and two Compatibility ones (indices 4-9)
    if (o.size() == 13)
    {
        CHECK (std::string (o[0].label) == "Notify about rendering mode" && o[0].member == &lynx::PluginRef::notifyRender && ! o[0].recreates);
        CHECK (std::string (o[10].label) == "Use fixed size buffers" && o[10].member == &lynx::PluginRef::fixedBuffers && o[10].recreates);
        CHECK (o[11].needsFixed && o[12].needsFixed && ! o[10].needsFixed && ! o[1].needsFixed);
        CHECK (! o[1].recreates && ! o[3].recreates && o[2].recreates && std::string (o[2].section) == "Processing"
               && std::string (o[10].section) == "Compatibility" && std::string (o[4].section) == "MIDI");
        lynx::PluginRef ref;
        CHECK (ref.*(o[0].member) && ref.*(o[1].member) && ref.*(o[3].member) && ! (ref.*(o[10].member))); // FL's defaults
    }
}
