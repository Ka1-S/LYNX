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

// Comment 4, point 2: an effect editor (Parametric EQ 2's band handles, Fruity Filter's knobs...) draws its slot with
// the automated parameters at their automation's value while the song plays; the Song keeps its own values.
TEST (issue11b_effect_editor_shows_its_automation)
{
    using namespace lynx;
    const AutomationTarget gain { AutomationTargetKind::effectParam, 1, 0, 2 };
    auto song = issue12c::songWithClip (gain);
    EffectSlot fx;
    fx.kind = EffectKind::parametricEq;
    fx.params[2] = 0.5f;
    fx.params[3] = 0.3f;
    song.mixer[1].effects.push_back (fx);
    const auto* stored = &song.mixer[1].effects[0];

    AutomationDisplay shown;
    ShownEffectSlot view;

    // stopped: the Song's slot itself
    shown.update (song, 1, 4.0 * ppq, false);
    CHECK (view.get (shown, stored, 1, 0, false) == stored);

    // playing: a copy with the automated parameter at the playhead's value, the others as stored
    shown.update (song, 1, 4.0 * ppq, true);
    CHECK (view.changed (shown, 1, 0));
    CHECK (! view.changed (shown, 1, 0)); // nothing new: no repaint
    const auto* a = view.get (shown, stored, 1, 0, false);
    CHECK (a != stored && std::abs (a->params[2] - 0.25f) < 0.01f && a->params[3] == 0.3f && a->kind == fx.kind);
    CHECK (stored->params[2] == 0.5f); // the Song is untouched
    CHECK (view.get (shown, stored, 1, 0, false) == a);
    // another slot / track: not automated
    CHECK (! ShownEffectSlot::automates (shown, 1, 1) && ! ShownEffectSlot::automates (shown, 0, 0));
    ShownEffectSlot other;
    CHECK (other.get (shown, stored, 2, 0, false) == stored);

    // moving on: the shown value follows; a hand on the control shows the Song's value
    shown.update (song, 1, 12.0 * ppq, true);
    CHECK (view.changed (shown, 1, 0));
    CHECK (std::abs (view.get (shown, stored, 1, 0, false)->params[2] - 0.75f) < 0.01f);
    CHECK (view.get (shown, stored, 1, 0, true) == stored);

    // an edit of another parameter shows at once, still with the automated one overlaid
    song.mixer[1].effects[0].params[3] = 0.9f;
    a = view.get (shown, stored, 1, 0, false);
    CHECK (a->params[3] == 0.9f && std::abs (a->params[2] - 0.75f) < 0.01f);

    // stopping: back to the Song's values (and a repaint)
    shown.update (song, 1, 12.0 * ppq, false);
    CHECK (view.changed (shown, 1, 0));
    CHECK (view.get (shown, stored, 1, 0, false) == stored);
}
