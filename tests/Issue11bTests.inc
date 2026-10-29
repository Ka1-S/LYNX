// Round 165s (issue #11, comment 4): Shift+Space, automation shown in the effect editors, Linux file drops

TEST (issue11b_shift_space_plays_from_the_start_everywhere)
{
    using namespace lynx::keys;
    KeyMap map;
    CHECK (map.conflicts().empty());
    // FL 2026 (checked in FL): Space = play / stop back to where playback started, Ctrl+Space = pause,
    // Shift+Space = play / stop from the start of the song or pattern — in the Playlist, Piano Roll, Channel Rack...
    CHECK (map.find ({ space }, Context::global, false) == "transport.playStop");
    CHECK (map.find ({ space, ctrl }, Context::global, false) == "transport.pause");
    for (const auto focus : { Context::global, Context::playlist, Context::pianoRoll, Context::channelRack, Context::mixer, Context::browser })
        CHECK (map.find ({ space, shift }, focus, false) == "transport.playFromStart");
    // also with the typing piano on (Space is not a piano key) and from a plugin / channel window
    CHECK (map.find ({ space, shift }, Context::pianoRoll, true) == "transport.playFromStart");
    CHECK (map.findFromSecondaryWindow ({ space, shift }, false, false) == "transport.playFromStart");
}
