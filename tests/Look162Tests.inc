// Round 162d (issue #8): the toolbar's last-tweaked read-out value text and the filter that reports a third-party
// plugin's own parameter moves as "last tweaked" (core/LastTweaked.h)

#include "core/LastTweaked.h"

TEST (look162_lasttweak_value_text)
{
    using namespace lynx;
    Song song;
    song.mixer.resize (2);
    song.bpm = 140.0;
    CHECK (lasttweak::valueText (song, { AutomationTargetKind::channelVolume, 0 }, 0.78f) == "78%");
    CHECK (lasttweak::valueText (song, { AutomationTargetKind::channelPan, 0 }, 0.5f) == "C");
    CHECK (lasttweak::valueText (song, { AutomationTargetKind::mixerPan, 1 }, 0.25f) == "L50");
    CHECK (lasttweak::valueText (song, { AutomationTargetKind::mixerPan, 1 }, 1.0f) == "R100");
    CHECK (lasttweak::valueText (song, { AutomationTargetKind::mixerVolume, 1 }, 0.0f) == "-inf dB");
    CHECK (lasttweak::valueText (song, { AutomationTargetKind::mixerVolume, 1 }, 0.8f) == "0.0 dB"); // the fader's 0 dB point
    CHECK (lasttweak::valueText (song, { AutomationTargetKind::tempo, 0 }, valueFromTempo (128.0)) == "128.00 BPM");
    CHECK (lasttweak::valueText (song, { AutomationTargetKind::channelParam, 0, 0, 3 }, 0.3f).empty()); // the plugin formats it
    CHECK (lasttweak::valueText (song, { AutomationTargetKind::effectParam, 1, 5, 0 }, 0.3f).empty());  // no such slot
    song.mixer[1].effects.push_back (makeEffectSlot (EffectKind::flLowLifter));
    CHECK (lasttweak::valueText (song, { AutomationTargetKind::effectParam, 1, 0, 10 }, 0.775f) == formatEffectParam (EffectKind::flLowLifter, 10, 0.775f, 140.0, song.mixer[1].effects[0].params));
    CHECK (lasttweak::valueText (song, { AutomationTargetKind::effectMix, 1, 0 }, 0.5f) == "50%");
}

TEST (look162_plugin_change_filter)
{
    lynx::lasttweak::ChangeFilter f;
    CHECK (f.tick (true) == -1);  // nothing moved
    f.changed (7);
    f.changed (7);
    CHECK (f.tick (true) == 7);   // one knob dragged
    CHECK (f.tick (true) == -1);  // reported once
    f.changed (7);
    CHECK (f.tick (false) == -1); // window not active / not under the mouse: automation or a link moved it
    f.changed (1);
    f.changed (2);
    f.changed (3);
    CHECK (f.tick (true) == -1);  // a preset load
    f.gesture (4, true);
    f.changed (4);
    CHECK (f.tick (true) == -1);  // inside a gesture: the gesture already reported it
    f.gesture (4, false);
    f.changed (4);
    CHECK (f.tick (true) == 4);
    for (int p = 0; p < 100; ++p)
        f.changed (p);
    f.changed (9);
    CHECK (f.tick (true) == -1);  // a burst
    f.changed (9);
    CHECK (f.tick (true) == 9);
}
