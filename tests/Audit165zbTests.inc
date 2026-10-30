// Round 165z-b (audit of issues #11, #12, #13 against FL Studio 2026 driven with real input)

// FL 2026 [SHOT fm3-fme, round 165z-b]: a double right-click held on the Playlist is the Mute tool. The first click
// deleted the clip under it and FL does not restore it; the sweep then gives every clip it crosses one state, decided
// by the first clip crossed (unmuted -> mute all, already-muted ones stay muted; muted -> unmute all).
TEST (audit165zb_double_right_click_mute_sweep_follows_the_first_clip)
{
    using namespace lynx;
    CHECK (! gesture::isDoubleClick (0, 100, 0.0));          // no earlier click
    CHECK (gesture::isDoubleClick (1000, 1300, 2.0));
    CHECK (! gesture::isDoubleClick (1000, 1600, 2.0));      // too slow
    CHECK (! gesture::isDoubleClick (1000, 1300, 9.0));      // moved away

    gesture::MuteSweep fromUnmuted;
    CHECK (fromUnmuted.touch (false) == true);  // first clip unmuted: the sweep mutes
    CHECK (fromUnmuted.touch (true) == true);   // an already-muted clip stays muted
    CHECK (fromUnmuted.touch (false) == true);

    gesture::MuteSweep fromMuted;
    CHECK (fromMuted.touch (true) == false);    // first clip muted: the sweep unmutes
    CHECK (fromMuted.touch (false) == false);   // an unmuted clip stays unmuted
    CHECK (fromMuted.touch (true) == false);

    // the clip the first right-click deleted stays deleted: the double click starts the sweep on what is left under
    // the mouse (nothing, here), so the first clip crossed afterwards decides
    gesture::MuteSweep fromEmpty;
    CHECK (! fromEmpty.decided);
    CHECK (fromEmpty.touch (true) == false && fromEmpty.decided);
}
