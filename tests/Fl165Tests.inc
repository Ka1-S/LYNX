// Round 165a (fl165): issue #11, FL muscle memory (docs/analise/REA_rodada165a.md)

TEST (fl165_key_click_velocity_follows_the_position_on_the_key)
{
    // FL's preview keyboard: soft at the key's base, full velocity at its tip
    using lynx::gesture::keyClickVelocity;
    CHECK (keyClickVelocity (0.0, 60.0) < 0.05f);
    CHECK (keyClickVelocity (0.0, 60.0) >= 1.0f / 127.0f);
    CHECK (std::abs (keyClickVelocity (29.5, 60.0) - 0.5f) < 0.01f);
    CHECK (keyClickVelocity (59.0, 60.0) == 1.0f);
    CHECK (keyClickVelocity (200.0, 60.0) == 1.0f);   // past the tip: clamped
    CHECK (keyClickVelocity (-5.0, 60.0) == 1.0f / 127.0f);
    CHECK (keyClickVelocity (10.0, 0.0) == 0.8f);     // no key: the old fixed velocity
    CHECK (keyClickVelocity (10.0, 60.0) < keyClickVelocity (40.0, 60.0));
}

TEST (fl165_ghost_step_makes_the_pattern_longer)
{
    using lynx::lengthWithStep;
    const std::int64_t step = 24, bar = 384; // 16 steps a bar
    CHECK (lengthWithStep (bar, 3, step, bar) == bar);          // inside: unchanged
    CHECK (lengthWithStep (bar, 15, step, bar) == bar);         // the last step: unchanged
    CHECK (lengthWithStep (bar, 16, step, bar) == 2 * bar);     // the first ghost step: a second bar
    CHECK (lengthWithStep (bar, 40, step, bar) == 3 * bar);     // further: whole bars up to it
    CHECK (lengthWithStep (0, 40, step, bar) == 0);             // automatic length stays automatic
    CHECK (lengthWithStep (bar / 2, 9, step, bar) == bar);      // a half-bar pattern grows to the bar
}

TEST (fl165_plugin_names_round_trip)
{
    std::map<std::string, std::string> names { { "lynx:fx:Parametric EQ", "1_EQ" }, { "VST3-Limiter-abc", "2_Limiter" } };
    const auto text = lynx::prefs::formatPluginNames (names);
    CHECK (lynx::prefs::parsePluginNames (text) == names);
    // a broken line, spaces and an empty name are skipped
    const auto parsed = lynx::prefs::parsePluginNames ("no tab here\n  key \t  3_Verb \nempty\t \n");
    CHECK (parsed.size() == 1 && parsed.at ("key") == "3_Verb");
    // JUCE writes the file with CRLF line ends on every system
    CHECK (lynx::prefs::parsePluginNames ("lynx:fx:EQ\t1_EQ\r\nb\t2_B\r\n").at ("lynx:fx:EQ") == "1_EQ");
    // names with tabs or new lines are never written (they would break the file)
    CHECK (lynx::prefs::formatPluginNames ({ { "k", "a\tb" } }).empty());
}

TEST (fl165_type_ahead_picks_the_one_renamed_plugin)
{
    using lynx::prefs::uniqueTypeAhead;
    const std::vector<std::string> labels { "1_EQ", "2_Limiter", "3_Verb", "Reverb", "reese" };
    CHECK (uniqueTypeAhead (labels, '1') == 0);
    CHECK (uniqueTypeAhead (labels, '3') == 2);
    CHECK (uniqueTypeAhead (labels, '4') == -1); // none
    CHECK (uniqueTypeAhead (labels, 'r') == -1); // several (case-insensitive): ambiguous
    CHECK (uniqueTypeAhead (labels, ' ') == -1);
    CHECK (uniqueTypeAhead ({ "Limiter" }, 'L') == 0);
}

TEST (fl165_fl_window_keys)
{
    using namespace lynx::keys;
    KeyMap map;
    CHECK (map.conflicts().empty());
    // FL 2026's defaults (its ShortcutsModule action list, REA round 165a)
    CHECK (map.find ({ f5 }, Context::global, false) == "panel.playlist");
    CHECK (map.find ({ f6 }, Context::global, false) == "panel.channelRack");
    CHECK (map.find ({ f7 }, Context::global, false) == "panel.pianoRoll");
    CHECK (map.find ({ f9 }, Context::global, false) == "panel.mixer");
    CHECK (map.find ({ f8, shift }, Context::global, false) == "view.pluginDatabase");
    CHECK (map.find ({ f10 }, Context::global, false) == "view.midiSettings");
    CHECK (map.find ({ f11 }, Context::global, false) == "view.projectInfo");
    CHECK (map.find ({ f12 }, Context::global, false) == "view.closeWindows");
    CHECK (map.find ({ f12, alt }, Context::global, false) == "view.closePluginWindows");
    CHECK (map.find ({ f5, ctrl }, Context::global, false) == "view.tempoTapper");
    CHECK (map.find ({ del }, Context::mixer, false) == "mixer.deleteEffect");
    CHECK (map.find ({ f2 }, Context::browser, false) == "browser.rename");
    // a single letter the user assigns (FL users bind M to the Mixer): works, but not while the typing piano is on
    map.setChords ("panel.mixer", { { 'M' } });
    CHECK (map.find ({ 'M' }, Context::global, false) == "panel.mixer");
    CHECK (map.find ({ 'M' }, Context::global, true).empty());
}
