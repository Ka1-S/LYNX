# Round 165z-a — audit of issues #2–#9: "did we do what they asked, exactly like FL Studio 2026?"

Part a of round 165z (the other part audits #11–#13). For every item of issues #2, #3, #4, #5, #6, #7, #8 and #9
(Ka1-S/LYNX-SHARE, issue text + every comment): the reporter's words, what FL Studio 2026 really does (driven in FL
with real input, screenshots, FL's form/menu resources), what LYNX SHARE main does now (tests / Windows run), and a
verdict MATCH / DIFFERENT / MISSING / UNVERIFIED. DIFFERENT / MISSING items that can be made solid are fixed.

## Where I stopped

- 2026-10-29 16:45: started. Worktree ../lynx-wt-audit165z-a (feature/audit165z-a1 from origin/main 997adb94).
  Tools: C:\Users\kauan\lynx-test\audit165z-a (drive.ps1, shots fl/ ly/).
- 2026-10-29 17:40: FL measured (shots fl/*.png), fixes A–E committed (8cd0271d, 22be5564), local + Windows builds
  running; next: Windows LYNX session (ly/*.png), then land.
- 2026-10-29 17:57: Windows session done (§ Windows run below, all looked at; desktop clean, lock released). Small
  follow-up: the preview strip updates its button only on state changes. Next: tests, land.

## Evidence

- **[FL]** FL Studio 2026 (25.2.99 build 5487, the user's install), fresh template, real mouse/keyboard input
  (SetCursorPos + mouse_event / keybd_event), screenshots in `C:\Users\kauan\lynx-test\audit165z-a\fl\` (named per
  row). FL renders: `fl_*.wav` there, analysed with a zero-crossing/segment pitch script.
- **[FLDOC]** earlier rounds with concrete per-case FL evidence (form resources / strings / FL runs), quoted, not
  re-measured: REA_rodada131/132 (#4), 133 (#3), 141 (#3 Patcher), 142 (#5), 158 (#6), 162a–e (#7/#8/#9).
- **[T]** LYNX core tests (tests/*.inc), run on this branch (build-local, 0 failures).
- **[W]** LYNX SHARE Windows build of this branch, fresh `LYNX_PROFILE_DIR` + `LYNX_TEST_MODE=1`, real input,
  shots in `...\audit165z-a\ly\`. **[W165q]** the adversarial Windows audit of main the day before
  (REA_rodada165q, every row with its own looked-at screenshot).
- **[L]** LYNX Linux build (build-linux, 2026-10-22; the plugin-scan code is unchanged since) driven with
  `--scantest` on the very packages the reporter listed (Debian/Ubuntu `dpf-plugins-vst`, `dpf-plugins-lv2`).

## The table

### #2 — Linux: "No plugins appear in the plugin manager and cannot seem to figure out how to get the LYNX native plugins" (alltern + log)

FL Studio has no Linux version, so the FL column is the closest FL behaviour; the verdict is on the request.

| item | reporter's words | FL 2026 | LYNX SHARE now | verdict |
|---|---|---|---|---|
| 2.1 VST2 `.so` | "No plugins appear in the plugin manager" (log: `/usr/lib/vst/Nekobi-vst.so` …) | n/a (Windows/macOS only); FL lists every installed VST/VST3 in its Plugin manager | [L] `--scantest` of the reporter's `dpf-plugins-vst` (`/usr/lib/vst/*.so`): **14 found** (Kars, Nekobi, MVerb, Ma*, Soul Force, glBars …) | MATCH |
| 2.2 LV2 + the lilv error flood | "`lilv_world_load_bundle(): error: Error reading file:///usr/lib/vst/Nekobi-vst.so/manifest.ttl`" for every plugin | n/a | [L] `dpf-plugins-lv2`: **15 found**; VST2 + LV2 folders scanned together: 29 found, **0 lines** on stderr (LV2 only gets folders that hold `.lv2` bundles, `lv2Folders`) | MATCH |
| 2.3 lib64 duplicates | "`Duplicate plugin <…> found in file:///usr/lib64/lv2/… and file:///usr/lib/lv2/… (ignored)`" | n/a | `/usr/lib64/{vst3,clap,lv2,vst}` are scan folders only when they are real folders, not links to /usr/lib (Paths.cpp). A distro shipping **real copies** in both still makes lilv print its own "Duplicate plugin … (ignored)" warning (harmless; the plugin is listed once) | UNVERIFIED (no lib64 distro here) |
| 2.4 LYNX's own plugins | "cannot seem to figure out how to get the LYNX native plugins" | FL's Plugin manager lists third-party plugins; the natives are in the Browser's Plugin database, Channel rack "+" / Add menu and mixer slots | not in the Plugin manager (third-party only, its empty state says where they are); Channel Rack **+**, mixer slot menu, Quick-add **F8**, Browser › Plugin database | MATCH |

Note found on the way (tool only, not user-facing): `--scantest <lv2 folder>` finds 0 LV2 plugins unless that
folder is also in the user's scan folders (`plugin-folders.txt`), because the scan child resolves LV2 URIs from the
configured folders. The Plugin manager writes that file before it scans, so users are not affected.

### #3 — "More automations and Build-In plugins" (Ivan-Spoda)

| item | reporter's words | FL 2026 | LYNX SHARE now | verdict |
|---|---|---|---|---|
| 3.1 | "Midi CC (as plugin parameter) … plugins have only automated parameters, defined by dev" | [FLDOC 133] the VST wrapper adds "MIDI CC" entries (CC 0–127, aftertouch, pitch bend) to the automatable list | every hosted plugin: *Automate MIDI controller (CC, aftertouch, pitch bend)*; clips/links send real MIDI [T] `midi_cc_automation_reaches_the_plugin_instrument`, `flp_wrapper_midi_cc_automation_keeps_its_index` | MATCH |
| 3.2 | "Midi Out (for Channel Rack only. With connecting to a plugin via Midi In port into plugin properties)" | [FLDOC 133 §4] MIDI Out generator: port, channel, program, knob pages; plugins listen on *MIDI input port* | Channel Rack + › MIDI Out; plugin *Processing › MIDI input port* [T] `midi_out_routes_notes_and_controls_to_the_plugin_on_its_port` | MATCH |
| 3.3 | "Envelope Controller (for Channel Rack only)" | [FLDOC 162e-envelope] 8 articulators, ADSR/LFO, key/vel/X/Y maps | [T] `envelope_controller_*` (8 tests incl. FL presets import) | MATCH |
| 3.4 | "Formula Controller (for Mixer only)" | [FLDOC 133] a, b, c knobs + FL's formula language | [T] `formula_controller_language_follows_fl`, `…_state_import_and_output`, `flp_demos_formula_controllers_import_and_compile` | MATCH |
| 3.5 | "Patcher (With Audio, Midi I\O)" (+ owner: "MIDI between nodes isn't there yet") | [FLDOC 141] audio, event and MIDI pins between nodes, From/To FL nodes | audio, notes, control and MIDI ports between nodes [T] `patcher_midi_out_node_routes_notes_to_ports_with_its_channel`, `patcher_from_lynx_midi_port_pins_take_only_their_port`, `patcher_io_midi_pins_show_and_deactivate`, `dashboard165_links_and_midi_through_the_rack_in_pattern_mode` | MATCH |

### #4 — "Various Critical Bugs & Features" (BURNER1PA)

| item | reporter's words | FL 2026 | LYNX SHARE now | verdict |
|---|---|---|---|---|
| 4.1 | "'Audio Editor' window doesn't display over-top of the main DAW window" | tool windows float over the main window | [W165q] a3/a4: owned by the main window, above it, stays after clicking the main window; | MATCH |
| 4.2 | "'Plugin picker' window … doesn't stay selected" (search bar) | the plugin picker keeps the keyboard | [W165q] a6/a7 search box keeps focus (also after clicking away and back) | MATCH |
| 4.3 | "The same issue occurs on VST plugin windows … impossible to open a drop-down menu" | plugin drop-downs work | [W165q] b5: Pro-Q 4 "MIDI Learn" menu stays open | MATCH |
| 4.4 | "MIDI input from connected controllers do not register … ADD a simple indicator light" | FL reopens enabled inputs; the hint bar has a MIDI activity icon | inputs re-opened every 2 s; toolbar MIDI light, click = reconnect [W165q] s9_03/04 (lists the open inputs) | UNVERIFIED (blink needs a MIDI device; none here) |
| 4.5 | "set an empty pattern length … to 2 bars … it loops back after 1 bar" | records/loops the pattern's length | [T] `bugs131_pattern_length_is_respected_on_the_first_recording_in_song_mode`; [W165q] pl1-14 | MATCH |
| 4.6 | "In 'Song' mode the transport … doesn't always loop back to start after the last clip/pattern" | [FL] u0–u7: clip 2:05–3:05 → the song loops at **3:05**, the exact end of the last clip (not the end of bar 3); v0–v3: playing from past the end jumps to the start at once | before: looped at the end of the last clip **rounded up to a bar** (3:05 → 4:01, Song::songLength). **Fixed**: loops at the exact clip end [T] `audit165za_song_mode_loops_at_the_exact_end_of_the_last_clip`; [W] ly/loop*.png | DIFFERENT → fixed |
| 4.7 | "Include 'Audio Editor' in the "View" menu" | [FL] s01: FL's View menu has **no** Audio editor (FL opens Edison from a mixer track: "Open audio editor Ctrl+E", m1) | View › Audio editor (by request) [W165q] a2 | DIFFERENT (extra, by request; kept) |
| 4.8 | "Add a basic volume control in 'Browser' window for samples that are previewed" | [FL] s16/s17: the volume is an orange **slider on the preview strip** at the bottom of the Browser, beside a Play/Stop toggle, both shown on hover / while playing | before: a small knob at the end of the search row. **Fixed**: slider + Play/Stop toggle on the preview strip, shown on hover or while a sample plays; [W] ly/pv*.png | DIFFERENT → fixed |
| 4.9 | "Channel Rack: Add a 'Piano Roll' button right next to "Graph editor" button and "Swing" knob" | [FL] s08/s09: overview button right of the graph-editor button; every row becomes a mini piano roll; click → Piano roll of that channel | same [T] `feat132_rack_piano_roll_view`; [W165q] f2 | MATCH |
| 4.10 | "Add a metronome volume control - somewhere handy." | [FL] s05: metronome right-click = only *Metronome sound: Hi-hat / Tick / Beep / Cowbell / Pulse*; the level is the fader of the **metronome target mixer track** (Settings › MIDI, `MetronomeTrackSelect`) | right-click on the metronome = volume knob + presets + count-in; Options › Metronome volume | DIFFERENT → next version (needs metronome routing to a mixer track + FL's 5 sounds; LYNX's click is synthesised and not routed through the mixer) |
| 4.11 | "ADD Tap Tempo function." | [FL] s01: View › Tempo tapper Ctrl+F5; s06: tempo right-click › Tempo tapper | View › Tempo tapper Ctrl+F5, tempo right-click [T] `feat132_tap_tempo_states`, `feat132_tapper_sync_and_nudge`; [W165q] c3/c4, s5_40/41 | MATCH |
| 4.12 | "'Channel settings' (for a loaded sample): Add gain knob" | [FLDOC 132] FL's Sampler has **no** gain knob (precomputed effects Normalize etc.; level = channel volume) | Gain −24…+24 dB in Channel settings [W165q] f16 | DIFFERENT (extra, by request; kept) |

### #5 — "Allow the plugins that only exist from flps to be used … in native projects" (alltern)

| item | reporter's words | FL 2026 | LYNX SHARE now | verdict |
|---|---|---|---|---|
| 5.1 | "allow the use of newtone to be used outside of FLP projects" | every FL plugin is in the Add menus | FL engines in Mixer › Add, Channel Rack + › FL engines, Quick-add, Patcher; a new one starts from the user's FL **Default preset** [FLDOC 142] [T] `Natives142Tests.inc`; Newtone/Newtime have editors (round 165 newt) | MATCH (needs FL installed for the engines that read FL data — by design, LYNX ships no FL data) |

### #6 — "native soundfont player close to the FL soundfont player or like SFLT with a search function" (alltern)

| item | reporter's words | FL 2026 | LYNX SHARE now | verdict |
|---|---|---|---|---|
| 6.1 | "native soundfont player close to the FL soundfont player" | [FLDOC 158] Fruity Soundfont Player: envelope/LFO/cutoff overrides, reverb/chorus, program mode | LYNX Soundfont (SF2/SF3) with those sections; FL projects import with font/preset/automation [T] `soundfont158_*` | MATCH |
| 6.2 | "or like SFLT with a search function" | FL has no preset search (SFLT does) | search across every preset of all fonts; click hears, double-click loads, drag onto the rack [T] `soundfont158_preset_search_index` | MATCH (beyond FL, as asked) |

### #7 — Patcher list, MIDI port filtering, Envelope Controller sections (Ivan-Spoda)

| item | reporter's words | FL 2026 | LYNX SHARE now | verdict |
|---|---|---|---|---|
| 7.1 | "Include all plugins into patcher plugin list." (Midi Out, Envelope Controller) | Patcher's Add plugin lists every generator/effect | Patcher › Add plugin: LYNX instruments, Envelope/Keyboard Controller, Dashboard, Voltage, MIDI Out, Layer, LYNX Soundfont, Video Player, VFX nodes, FL engines, all effects, hosted plugins (PatcherEditor `addPluginMenu`) | MATCH |
| 7.2 | "Add a Midi port filtering into Patcher for I\O nodes." | [FLDOC 141] From/To FL MIDI pins per port | [T] `patcher_from_lynx_midi_port_pins_take_only_their_port`, `patcher_fl_midi_port_pins_import` | MATCH |
| 7.3 | "Add a EC missing sections (Key-mapping, Vel-Mapping, Mod X/Mod Y mapping, Random)" | [FLDOC 162e-envelope] the four maps + Random | [T] `envelope_controller_maps_and_modes`, `…_instrument_plays_maps_and_continuous_output` | MATCH |

### #8 — "Quality of life features and bugs" (alltern, Linux)

| item | reporter's words | FL 2026 | LYNX SHARE now | verdict |
|---|---|---|---|---|
| 8.1 | "Add theming support." (FL Studio Themes) | [FL] th0: Options › Theme settings — presets, Hue/Saturation/Brightness/Contrast/Text, role colours, Light mode, save preset | Options › Theme: presets, accent + background colours, workspace colour/image, save/import/export `.lynxtheme` [W165q] s5_13… | DIFFERENT (LYNX themes accent/background, not FL's HSB knobs + role colours; content-wise covers the request) |
| 8.2 | "Add plugin presets" (FL studio Fsts) | plugin window Presets menu, ◀ ▶ | plugin window Presets menu with ◀ ▶, save/load, `.fxp/.fxb/.vstpreset` [T] `plugin_preset_text_round_trip_round162`, `vst2_fxp_fxb_round_trip_round162`; [W165q] s6_05-08 | MATCH |
| 8.3 | "when putting a plugin on wrapper settings the original plugin window still appears over everything" (Linux) | FL's wrapper settings replace the editor view in the same window | wrapper settings page on top of the plugin window (round 162d-plugins, Linux run then) | UNVERIFIED now (Linux GUI not re-run in this round) |
| 8.4 | "right click knobs and parameters to open up a context menu about them" | right-click any control: automation, link to controller, … | LYNX knobs' right-click menu (automation clip, link to controller, type value …) [W165q] s5_49/50 | MATCH |
| 8.5 | "on the toolbar add like a way to see the last tweaked knob or parameter and open the context menu from there" | [FL] lt0: Tools › Last tweaked parameters › Last tweaked ›, Next to last tweaked › (and the hint bar shows the value) | hint-bar read-out of the last tweaked control (click → its menu) + Tools › Last tweaked / Next to last tweaked [W165q] s5_49/50 | MATCH |

### #9 — "More Critical Features & Bugs - v0.1.17" (BURNER1PA)

| item | reporter's words | FL 2026 | LYNX SHARE now | verdict |
|---|---|---|---|---|
| 9.1 | "A localized search index that you can manually refresh/update" | [FL] s10: Browser menu: Enable indexing, Re-index changed folders on startup, Reread structure Ctrl+R (no separate "refresh index") | index (persistent, follows changes) + Browser › Refresh search index [T] `browser162_*`; [W165q] s9_01/02 | MATCH (+ manual refresh, as asked) |
| 9.2 | "Add a Stop (or Play/Pause) button next preview volume knob" | [FL] s16/s17: Play/Stop toggle on the preview strip next to the volume slider | before: Stop button in the search row. **Fixed** with 4.8: Play/Stop toggle + slider on the strip [W] ly/pv*.png | DIFFERENT → fixed |
| 9.3 | "Use Up and Down arrow keys to browse and play through samples" | [FL] s18: Down selects the next sample and plays it | same [W165q] d6/d7 | MATCH |
| 9.4 | "Add a "Follow" button/setting that turns ON or Off mandatory following of the playhead during 'Song' mode" | [FL] s02, w1–w5, x1–x6: one global Options › **Auto scrolling** switch; it drives both Playlist and Piano roll; off = neither view moves | per-window Follow buttons (Playlist, Piano roll, Channel rack, Audio editor) + View items; no global switch. **Fixed**: Options › Auto scrolling turns every Follow on/off at once (per-window buttons kept) [W] ly/as*.png | DIFFERENT → fixed |
| 9.5 | "Ability to change current selected Pattern within Channel Rack (perhaps a drop-down menu…)" | FL's rack has no pattern selector (pattern picked in the toolbar) | pattern selector in the rack's title [W165q] f3-f6 | DIFFERENT (extra, by request; kept) |
| 9.6 | "'Audio Editor': Add a volume knob." | Edison has its own volume | volume knob beside Loop [W165q] s5_37 | MATCH |
| 9.7 | "Stretch function does not respect note changes … Allow Stretch function to work chromatically" | [FL] st0–st10, renders of a 0.5 s 440 Hz + 0.5 s 660 Hz file at 130 BPM: *Resample* 100 % C6 → 880 Hz, 0.55 s (tape); *Resample* MUL 200 % C5 → 220/330 Hz 2.05 s, C6 → 440/660 Hz 1.05 s; *Stretch* (pitch kept) MUL 200 % C6 → 880/1320 Hz, 2.05 s | [T] `s162_resample_stretch_follows_the_keys`, `s162_stretch_keeping_pitch_transposes_and_keeps_the_length` (same rules) | MATCH |
| 9.8 | "ADD: a '50% Quantize' to grid function + Assignable to a custom keyboard shortcut." | FL's Quantize tool has strength; no fixed 50 % item | Quantize 50 % in Piano roll/Playlist, assignable [T] `transport162_quantize_tick_strength`; [W165q] | MATCH (as asked) |
| 9.9 | "ADD: 'Quantize Starts' as a custom shortcut option." | FL: Quantize start times Shift+Q (Piano roll) | `pianoRoll.quantizeStarts` Shift+Q, `playlist.quantizeStarts`, both assignable | MATCH |
| 9.10 | "'Playlist': Double clicking Stop returns playhead to start." | [FL] t0–t3: ruler at bar 5, play, Stop → **5:01**, Stop again → **1:01** — with this FL's *Remember seek time* on (registry `FruityLoopsMainForm\RemSeekTime = 1`; FL's string "Remember seek time" is the Stop menu option) | [W] st3–st6: option off (LYNX's default): Stop → 1:1:1; st9/st10 with the Stop button's right-click › *Remember seek time* on: Stop → **3:1:1**, Stop again → **1:1:1** [T] `transport162_follow_and_stop` | MATCH (the double Stop asked for; the first Stop follows the same option as FL. FL's factory default for it was not measurable on this install) |
| 9.11 | "Allow drag and drop of VSTs from 'Browser' to 'Channel Rack'." | [FL] z3: a generator dropped **onto a channel replaces it** ("808 Snare" → "3x Osc", Insert 4 kept, its window opens); z4: dropped below the channels → a new channel | before: always a new channel. **Fixed**: onto a channel = replace (place, notes, insert, volume, pan, mute, colour, group, swing kept; window opens), below = new channel [T] `audit165za_generator_dropped_on_a_channel_replaces_it`; [W] ly/dr*.png | DIFFERENT → fixed |
| 9.12 | "'Piano Roll': Enable playhead in 'Piano Roll' + "Follow" On/Off button." | [FL] y0–y2: Song mode: the Piano roll's playhead shows only while the song is inside that pattern's clip; follow = the global Auto scrolling (9.4) | same [T] `transport162_piano_roll_playhead`; [W165q] s5_35 | MATCH |
| 9.13 | "'Mixer': Add decibel values printed aside the MASTER fader VU meter … (ex. +3, 0, -3, -6, -12, etc.)" | [FL] m0: the mixer's "C" scale strip, unsigned labels every 3 dB: 3, 0, 3, 6, 9 … 36 | signed labels beside the master meter, as many as fit: +3, 0, −3, −6, −9, −12, −18, −24, −36, −48 [T] `transport162_meter_scale_marks` | DIFFERENT (format; kept the reporter's signed values — FL's unsigned 3-dB ladder reads ambiguously; noted for the user) |
| 9.14 | "Ability the change LYNX's Red accent color to any other color via a color wheel/sliders." | [FL] th0: Theme settings Hue knob + presets | Options › Theme accent colour wheel | MATCH |
| 9.15 | "'Mixer': Ability to change mixer track color via color wheel/slider." | [FL] m1/m2: mixer track menu › Change color… = Color selector (hue wheel, brightness, presets, HSL/RGB/hex) | Rename and color dialog with colour wheel + Change color menu [W165q] i3, c11 | MATCH |
| 9.16 | "change LYNX's Black/default workspace background to either another solid color or an image file … with simple image screen-fitting options" | [FL] th1: **View › Background ›** Set image wallpaper…, Set dynamic wallpaper / HTML document…, Set background color… | Options › Theme: workspace colour, image, fit (fill/fit/stretch/tile/centre), opacity, dim; **added** FL's View › Background › Set image wallpaper… / Remove image wallpaper / Set background color… [W] ly/bg*.png | MATCH (+ FL's menu path added) |
| 9.17 | "LYNX crashes after attempting to search/load ALL folders for a search term" | no crash | [T] `browser162_typing_fast_during_a_slow_first_index`, `…_closing_mid_walk_is_quick`; [W165q] d1/d2 alive 40 s after | MATCH |
| 9.18 | "the Browser's search bar remains active when switching windows … space bar … continues Browser search typing" | Space = transport outside text fields | clicking elsewhere / Enter / Esc leave the box [W165q] d4, s5_30/31 | MATCH |
| 9.19 | "MIDI pitch bend/modulation wheels to not work by default" | pitch bend reaches every generator; FL's native synths ignore the mod wheel unless linked | [T] `s162_pitch_wheel_bends_native_instruments_by_the_pitch_range`, `s162_pitch_and_mod_wheel_reach_a_hosted_plugin`, `misc164m_native_engines_keep_ignoring_the_mod_wheel_like_fl` | MATCH (hardware not re-run) |
| 9.20 | "'Mixer': Mixer track > "Change color" function (specifically) does not work." | [FL] m1/m2 | [W165q] i3 | MATCH |
| 9.21 | "When removing a VST Effect OR Instrument, the removed VST's GUI stays on-screen" | the window closes | [W165q] b7 | MATCH |
| 9.22 | "Export: When trying to overwrite a file with same name during an export, the export fails" | replaces after a prompt | [W165q] replace prompt, file rewritten | MATCH |
| 9.23 | "'Sampler': Changing cents/fine tuning does not affect sample pitch at all." | [FL] Misc tab Fine tune (st*) | [T] `s162_sampler_fine_tune_changes_live_notes`; [W165q] +53 cents → 453.67 Hz | MATCH |
| 9.24 | "Unable to assign a channel past track/insert number 16. Increase this to at least a 64 track max." | [FL] ch1–ch4: the channel's insert selector clamps to the existing inserts (16 in the template; typing 40 → 16); FL has 125 inserts you add in the mixer; right-click: "Assign to free mixer track" | any insert up to 127; a higher number adds the inserts [T] `s162_a_channel_routes_to_any_insert`, `s162_the_rack_mixes_a_channel_routed_past_insert_16_into_that_insert`; [W165q] f17 | DIFFERENT (more permissive than FL's clamp; kept — it is what was asked) |

## Windows run (this branch, fresh profile, test mode, real input) — shots in `...\audit165z-a\ly\`

| check | result |
|---|---|
| Options › Auto scrolling (as0/as1) | listed and ticked by default; clicking it unticks it, and View's "Playlist follows…" / "Piano Roll follows…" are both unticked afterwards |
| View › Background (bg0–bg3) | submenu with Set image wallpaper… / Remove image wallpaper (greyed while there is none) / Set background color…; a PNG picked in the file dialog is copied to `<profile>\Backgrounds\wall.png`, saved in theme.lynxtheme (`"image": "wall.png"`) and shown behind the panels (bg3); Remove image wallpaper takes it away (the theme file goes back to the default) |
| Song loop at the exact clip end (loop0, 60-shot position sequence) | a 1-bar clip placed at ≈4:3:3: the darker "after the song" area starts at the clip's right edge, not at the bar line; playing from 1:1 the position runs … 5:3:1, 5:3:3, 5:3:4 → **1:1:2** (before the fix it went on to 5:4:4 → 1:1) |
| Generator dropped on a channel (dr0–dr6) | LYNX SHARE › Synth onto "Clap" (channel 2): it becomes "Synth" on row 2, its steps and insert 2 stay, still 5 channels; Kick dropped below the channels: new channel 6 "Kick"; FL engines › 3x Osc onto channel 6: the row turns 3x Osc and its window "3x Osc (Insert 6)" opens — as FL's z3 |
| Browser preview strip (pv2–pv7, fc1) | while a loop plays: Stop button (accent) + volume slider at the strip's bottom left, the search row has neither any more; clicking Stop stops it and the button turns into Play (hover hint "Stop the sample preview"); with the mouse away and nothing playing both hide. 5 of 6 clicks stopped at once; the first click of session 1 replayed the sample instead (not reproduced in 5 later tries nor on a fresh start); the button's state is now only touched when it changes |
| Stop / double Stop (st3–st10) | see 9.10 |

## Fixes in this round

A. **Song mode loops at the exact end of the last clip** (4.6). `Song::songLength()` returned the last clip's end
   rounded up to a bar; FL loops at the clip's end itself (u0–u7). Now the exact end (an empty Playlist still loops
   the current pattern's length, at least a bar). Export already rendered to the exact end; the Playlist's song-end
   marker, "zoom to song", recording wrap and the host position follow the same value.
B. **A generator dropped from the Plugin database onto a channel replaces it** (9.11, FL z3/z4) —
   `replaceGenerator` (RackOps): the channel keeps its row, notes, mixer insert, volume, pan, mute, colour, group and
   swing; its instrument and name are the dropped generator's; the old editor closes and the new one opens. Below the
   channels it is still a new channel.
C. **Options › Auto scrolling** (9.4, FL s02): one switch for every editor's Follow; ticked while the Playlist and
   the Piano roll both follow. The per-window Follow buttons stay.
D. **Browser preview controls on the preview strip** (4.8 / 9.2, FL s16/s17): Play/Stop toggle (accent while
   playing) + horizontal volume slider at the strip's bottom left, shown while the mouse is over the strip or a sample
   plays; removed from the search row.
E. **View › Background ›** Set image wallpaper… / Remove image wallpaper / Set background color… (9.16, FL th1) —
   the image goes through the same copy-into-Backgrounds path as Options › Theme (`themeui::chooseWorkspaceImage`).

New strings ("Auto scrolling", "Play the sample preview", "Set image wallpaper...", "Remove image wallpaper",
"Set background color...") in all 15 languages.

## Left for the next version

- 4.10 metronome: route the click to a selectable mixer track (FL's "Metronome target mixer track") and offer FL's
  five sounds (Hi-hat, Tick, Beep, Cowbell, Pulse) in the right-click menu. LYNX's synthesised click is mixed after
  the mixer today, so this is an engine change, not a menu.
- 8.1 FL-style theme knobs (Hue/Saturation/Brightness/Contrast/Text + role colours, light mode) on top of LYNX's
  accent/background themes.
- 9.13 an FL-like unsigned 3-dB ladder, if users prefer it to the signed labels.
