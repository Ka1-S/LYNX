// Round 165 issue11c (issue #11, n3m351S + Jocho-Smith): the record button of an audio track's header and FL 2026's
// measured Shift+Space

// FL 2026 [SHOT, issue11c]: only an audio track (Track mode › Audio track › Insert N) has the header buttons; they act on
// its linked mixer track. Unassigned and instrument tracks have none.
TEST (issue11c_audio_track_arms_its_mixer_track)
{
    using namespace lynx;
    auto song = Song::makeDefault();
    song.playlistTracks.resize (4);
    CHECK (trackmodes::armableInsert (song, 0) < 0);      // unassigned
    CHECK (trackmodes::armableInsert (song, -1) < 0 && trackmodes::armableInsert (song, 99) < 0);

    CHECK (trackmodes::makeAudioTrack (song, 2));
    const auto insert = song.playlistTracks[2].mixerTrack;
    CHECK (insert > 0 && trackmodes::armableInsert (song, 2) == insert);

    CHECK (trackmodes::makeInstrumentTrack (song, 1, 0)); // an instrument track: no record button
    CHECK (trackmodes::armableInsert (song, 1) < 0);

    song.playlistTracks[3].mode = PlaylistTrackMode::audio; // linked to a mixer track that doesn't exist: none
    song.playlistTracks[3].mixerTrack = (int) song.mixer.size();
    CHECK (trackmodes::armableInsert (song, 3) < 0);
    song.playlistTracks[3].mixerTrack = 0; // the Master can't be armed
    CHECK (trackmodes::armableInsert (song, 3) < 0);

    trackmodes::unassign (song, 2);
    CHECK (trackmodes::armableInsert (song, 2) < 0);
}

// FL 2026, measured with real keys in FL (Song and Pattern mode, Playlist / Piano Roll / Channel Rack focus, recording):
// Shift+Space while playing stops at the start of the time selection (Song mode) or else at 1:01 — not where playback
// started (bar 5 → 1:01) and not where it was pressed; while stopped it plays from the current position like Space.
TEST (issue11c_shift_space_stop_target_matches_fl)
{
    using namespace lynx;
    const auto bar = (std::int64_t) (4 * ppq);
    CHECK (transportedit::shiftSpaceStopTarget (true, false, 0) == 0);                  // Song, no selection: 1:01
    CHECK (transportedit::shiftSpaceStopTarget (true, true, 2 * bar) == 2 * bar);      // Song, selection bars 3-7: 3:01
    CHECK (transportedit::shiftSpaceStopTarget (false, true, 2 * bar) == 0);           // Pattern mode: pattern start
    CHECK (transportedit::shiftSpaceStopTarget (false, false, 0) == 0);
    CHECK (transportedit::shiftSpaceStopTarget (true, true, -5) == 0);

    using namespace lynx::keys;
    KeyMap map;
    CHECK (map.find ({ space, shift }, Context::playlist, false) == "transport.playFromStart");
    const auto* action = findAction ("transport.playFromStart");
    CHECK (action != nullptr && std::string (action->description) == "Play, or stop at the start of the song or pattern");
}
