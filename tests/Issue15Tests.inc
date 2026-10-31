// Round 166a, issue #15 (esiowry): "Audio clips routed to a mixer insert become reassigned to a different insert when
// moved to other tracks in the playlist". FL 2026 [DECOMP FLEngine FUN_00e66ba0 / FUN_00e66580, REA_rodada166a §1]:
// only the clips an edit placed follow an audio track; a channel also used on another audio track asks first.

namespace issue15
{
// tracks: 0 = audio track → insert 14, 1 = audio track → insert 10, 2 = unassigned; channel "take" on insert 14
struct Setup
{
    lynx::Song song = lynx::Song::makeDefault();
    int take = -1;
    Setup()
    {
        using namespace lynx;
        while (song.mixer.size() < 16)
            song.mixer.emplace_back();
        song.playlistTracks.resize (3);
        song.playlistTracks[0].mode = PlaylistTrackMode::audio;
        song.playlistTracks[0].mixerTrack = 14;
        song.playlistTracks[1].mode = PlaylistTrackMode::audio;
        song.playlistTracks[1].mixerTrack = 10;
        ChannelDef c;
        c.name = "Take";
        c.instrument = InstrumentKind::audioClip;
        c.mixerTrack = 14;
        song.channels.push_back (c);
        take = (int) song.channels.size() - 1;
    }
    int clip (int track, std::int64_t start, int channel = -1)
    {
        lynx::Clip c;
        c.kind = lynx::ClipKind::audio;
        c.channel = channel < 0 ? take : channel;
        c.track = track;
        c.start = start;
        c.length = 384;
        song.playlist.push_back (c);
        return (int) song.playlist.size() - 1;
    }
};
} // namespace issue15

TEST (issue15_only_placed_audio_clips_follow_an_audio_track)
{
    using namespace lynx;
    issue15::Setup s;
    const auto a = s.clip (0, 0);
    const auto b = s.clip (0, 384);
    // an edit that placed nothing (or other clips) never reroutes
    CHECK (trackmodes::followAudioTracks (s.song, {}) == 0);

    // b moved onto the audio track of insert 10: FL routes its channel there (every clip of the channel follows)
    s.song.playlist[(std::size_t) b].track = 1;
    CHECK (trackmodes::audioTrackConflicts (s.song, { b }).size() == 1); // (a is on the audio track of insert 14)
    s.song.playlist[(std::size_t) a].track = 2;                          // a on an unassigned track: no question
    CHECK (trackmodes::audioTrackConflicts (s.song, { b }).empty());
    CHECK (trackmodes::followAudioTracks (s.song, { b }) == 1 && s.song.channels[(std::size_t) s.take].mixerTrack == 10);

    // routed back to 14 by hand: it stays there while other clips are moved, until b itself is placed again
    s.song.channels[(std::size_t) s.take].mixerTrack = 14;
    const auto other = s.clip (1, 2000, 0); // (a generator channel's clip: ignored)
    s.song.playlist[(std::size_t) other].kind = ClipKind::pattern;
    CHECK (trackmodes::followAudioTracks (s.song, { a, other }) == 0 && s.song.channels[(std::size_t) s.take].mixerTrack == 14);
    CHECK (trackmodes::followAudioTracks (s.song, { b }) == 1 && s.song.channels[(std::size_t) s.take].mixerTrack == 10);

    // onto an unassigned track: the route stays
    s.song.channels[(std::size_t) s.take].mixerTrack = 14;
    s.song.playlist[(std::size_t) b].track = 2;
    CHECK (trackmodes::followAudioTracks (s.song, { b }) == 0 && s.song.channels[(std::size_t) s.take].mixerTrack == 14);
}

TEST (issue15_shared_channel_asks_to_make_the_clip_unique)
{
    using namespace lynx;
    issue15::Setup s;
    const auto a = s.clip (0, 0);   // on the audio track of insert 14
    const auto b = s.clip (1, 384); // moved onto the audio track of insert 10
    const auto c = s.clip (1, 768); // moved with it
    const auto channels = s.song.channels.size();
    CHECK (trackmodes::audioTrackConflicts (s.song, { b, c }) == std::vector<int> { s.take });

    // No: nothing changes
    CHECK (trackmodes::followAudioTracks (s.song, { b, c }, false) == 0);
    CHECK (s.song.channels.size() == channels && s.song.channels[(std::size_t) s.take].mixerTrack == 14);

    // Yes: one copy for the placed clips, routed to 10; a keeps the channel on 14
    CHECK (trackmodes::followAudioTracks (s.song, { b, c }, true) == 2);
    CHECK (s.song.channels.size() == channels + 1 && s.song.channels.back().mixerTrack == 10);
    CHECK (s.song.playlist[(std::size_t) b].channel == (int) channels && s.song.playlist[(std::size_t) c].channel == (int) channels);
    CHECK (s.song.playlist[(std::size_t) a].channel == s.take && s.song.channels[(std::size_t) s.take].mixerTrack == 14);
    CHECK (trackmodes::audioTrackConflicts (s.song, { b, c }).empty());

    // a clip of the channel in another arrangement counts too (FL looks at every playlist)
    issue15::Setup t;
    t.clip (0, 0);
    Arrangement other;
    other.clips = t.song.playlist;
    t.song.arrangements = { Arrangement {}, other };
    t.song.currentArrangement = 0;
    t.song.playlist.clear();
    const auto d = t.clip (1, 0);
    CHECK (trackmodes::audioTrackConflicts (t.song, { d }).size() == 1);
}
