# Round 165z-b — audit of issues #11, #12, #13: "did we do what they asked, exactly like FL?"

Brief: round165z-audit-brief.md. Part b = issues #11 (Jocho-Smith, Linux), #12 (BURNER1PA, Windows 10), #13
(caiorobertcr7). Excluded (proved by issue11c, follow-up running elsewhere): #11 Shift+Space and Playlist arm.

Method
- FL Studio 2026 on this PC, driven with real mouse / keyboard input (SetCursorPos + mouse_event / keybd_event), default
  template unless noted; closed by its own PID. Screenshots in `C:\Users\kauan\lynx-test\audit165z-b\` (named below).
- LYNX SHARE main 997adb94 (`LYNX.exe` in the same folder), fresh `LYNX_PROFILE_DIR` + `LYNX_TEST_MODE=1`, the same
  real-input driver; real Windows Explorer drags for the file drops. Core tests: `scripts/build-local.sh` with FL's Data.
- Desktop screenshot after every session: clean (end1, dend, kend, flend, fgend, fhend).

## Issue #11 (Jocho-Smith)

| # | Reporter's words | FL 2026 (evidence) | LYNX SHARE main (evidence) | Verdict |
|---|---|---|---|---|
| 1 | "if one presses the keys more to the left it would play it with lower velocity and if one plays it on the far right it would be of max velocity" | Clicked one Piano Roll key at 5 points left→right: the meter rises monotonically (fv1133…fv1196). Clicking the keyboard while recording records nothing. | `gesture::keyClickVelocity` linear left→right (EditorGestures.h); tests in EditorGestureTests | MATCH |
| 2 | "when adding a node to the pattern sequencer (at least while it is playing) it would not make any sounds" | Step click stopped and playing: no sound (fs1–fs3) | Step click is silent (165a test + r165q Windows run) | MATCH |
| 3 | "drag and drop samples into pattern sequencer, seems to not be possible yet" | FL accepts file drops on a channel (replace) and on the rack (new channel) | Real Explorer drag on Windows: onto a channel → replaced Snare with s0002; onto free space → new channel s0003 (r1, r2) | MATCH |
| 4 | "shortcuts like p, m, ... for pattern, mixer for quick navigation seems to be missing" | FL ships no single-letter P/M window keys (165a: FL's default key table, concrete per key); F5–F9 etc. are FL's window keys | F-keys as FL; P/M assignable in Manage shortcuts (issue12_every_action_takes_a_user_key) | MATCH |
| 5 | "fruity waveshaper looks correct but I cannot change the shape with my mouse" | FL: points drag / add / delete with the mouse | Linux-only mouse bug fixed in 165b (LinuxMouse165Tests, Xvfb runs in REA_rodada165b.md) | MATCH (Linux by test) |
| 6 | "sequence buttons that are transparent and ignored unless one clicks it and adds a note" | Rack widened: steps 17–64 drawn dim; clicking step 19 set it and the pattern became 32 steps (2 bars) (fg5, fg6) | Ghost steps past the end; a click sets it and lengthens by whole bars (`lengthWithStep`, 165a tests; r165q f7/f8 16→32) | MATCH |
| 7 | "after right click on a effect, pressing 'Delete' on the keyboard would delete the plugin" | FL 2026's slot menu has "Delete…"; the Delete key with that menu open does nothing ("D" = Move down); Del over a slot asks to delete the mixer track (fxa–fxd) | Slot menu "Delete  Del" + Del removes the plugin (per the request) | DIFFERENT from FL 2026 on purpose (reporter's request; FL has only the menu entry). Kept; reply says so |
| 8 | "rename plugins … append a 1_ or 2_ … click on a slot … press '1'" | FL's slot menu is type-ahead: typing a unique first letter (W) loads Wave Candy at once (fx3, fx4); "1_EQ" works the same way | Rename in the Plugin database; type-ahead in the slot menu, renamed plugins listed first under a header | MATCH (LYNX lists renamed ones first: presentation only) |
| 9 | "double right click and hold on the second would not remove it but mute it" | Measured 5 ways (fm3–fme): the first right-click DELETES the clip under the mouse and FL does not bring it back; holding the second click is the Mute tool: the first clip crossed decides mute/unmute for every clip crossed (already-muted stay muted); started on empty space, the first clip crossed decides | Main undid the deletion and toggled that clip (PlaylistComponent) | DIFFERENT → **fixed** (batch b1): the deleted clip stays deleted, sweep as FL. Reporter's memory ("would not remove it") differs from FL 2026; reply explains |
| 10 | "when I click a sound on the browser to preview it, I cannot stop it … pressing the main stop button" | Transport Stop stops the Browser preview (fb5–fb7) | Same (165a; r165q Windows) | MATCH |
| 11 | "vertical zoom is missing for the playlist" | Alt+wheel over the Playlist: wheel up = taller tracks (fz1, fz2) | Alt+wheel → `setTrackZoom (trackZoom * wheelZoomFactor)`, deltaY > 0 → ×1.2 (taller) | MATCH |
| 12 | "automation editing also seems to not work for me" (Linux) | FL: automation points drag | Linux mouse fix 165b (same cause as #5), tests | MATCH (Linux by test) |
| 13 | "I can hear that the automation is actually happening, but I cant see it in the plugin" | FL's Parametric EQ 2 etc. move with automation (165s, per-effect evidence) | Effect editors follow automation (165s, tests) | MATCH |
| 14 | "drag and drop audio into playlist also does not work" | FL: a file dropped on the Playlist becomes an audio clip | Real Explorer drag on Windows → audio clip (e2); Linux XDND fixes in 165s (mutter 50 limit documented there) | MATCH (Windows real drag; Linux see 165s) |

## Issue #12 (BURNER1PA)

| Item | Reporter's words (short) | FL 2026 (evidence) | LYNX SHARE main (evidence) | Verdict |
|---|---|---|---|---|
| Tool shortcuts | custom shortcuts for the tools | FL's tools have keys | every tool is a key-map action (playlist.tool.*, pianoRoll.tool.*), test issue12_every_action_takes_a_user_key | MATCH |
| VU click selects | "Clicking on a mixer track's VU meter to select that channel" | request | click on Insert 5's meter selects it (v1) | MATCH (request) |
| Sampler knobs automatable | "Make ADSR, Cutoff, Resonance, and Start + End knobs automatable via automation clip" | FL: right-click a knob → one parameter menu (Create automation clip …) | right-click Attack → "Create automation clip" opened "Riser Cymbal - Attack" (k6, k7). **Found: two menus opened at once** (the knob's automation menu + the app-wide slider value menu; also visible in r165p's c12.png) | DIFFERENT → **fixed** (b1): one menu in FL's order (name, Reset, Automation, Remote control, Value) |
| Header double-click rename | pattern menu and mixer insert menu | request | insert menu header → Rename track, focused (v2, v3); pattern menu → rename "verse" (v4, v5) | MATCH (request) |
| Audio Editor follow / drop / replace | follow button; drop replaces the loaded sample | request | Follow button; real Explorer drop loads (e6), a second drop replaces (e7) | MATCH |
| Audio Editor region → new Sampler | (FL Edison "Send to channel") | Edison sends a selection to a channel | "Send to a new Sampler channel" in the selection menu (code; b1 Windows check below) | see b1 check |
| Channel Rack pattern length + follow | length buttons, follow playhead | request | "A 1"→"L 4" by wheel, follow button (v6, v7) | MATCH |
| Piano Roll bold "All" | bulk edit of the pattern | request | bold All shows every channel's notes (v8, v9) | MATCH |
| Mixer send knobs automatable | "ADD Automation ability for the routing volume level knobs" | FL: route knob has the parameter menu | right-click the route knob → one menu; "Insert 1 → Master - Level to Master" clip created (k13, k14) | MATCH |
| Explorer drop into a Browser folder | copy files into a Browser folder | request | real Explorer drag: copied, "Copied 1 file to bfolder" (b2) | MATCH |
| Sampler waveform drop from Browser | replace the sample | FL: drop on the waveform replaces | Browser "Riser Engine" dropped on Riser Cymbal's waveform → replaced, title follows (k9) | MATCH |
| Backgrounds folder + presets keep image | | request | image in Backgrounds listed and applied (k17, k18); "Save preset" wrote `"image": "mountain.png"`; Default then "My theme" restored the image (k21–k23) | MATCH |
| Startup sound | | request | Theme › Startup sound: Choose / Play / Default (k19) | MATCH |
| Bug: Metronome on C | "does nothing" | — | Manage shortcuts Metronome→C warns "C was also Tool: Knife … your key wins"; C toggles with Playlist, Rack, Browser, track header, Piano Roll, Mixer focused (s1_08, m0–m6) | MATCH (fixed) |
| Bug: Options › Theme opens MIDI | | — | all 7 Options settings entries open their own page (s1_06, s1_opt_*) | MATCH (fixed) |
| Bug: song-mode automation recording | "should sustain and continue to smoothly elongate clip length until Recording has stopped … without cutting itself" | Default template, Song mode, Notes+automation, empty Playlist, knob moved from ~bar 2.3 to ~3.4, stopped at ~7.1 (fh2–fh5): FL writes the moves as **events into Pattern 1** (no automation clip, the "Clips" filter changes nothing: fa18–fa20), shows nothing while recording, and at Stop places a Pattern 1 clip from **bar 2** (the bar of the first move) to bar 4 (the pattern's whole bars). With a Pattern 1 clip already there it writes into that pattern (fa10–fa13). | One automation clip "Insert 1 - X": starts at the bar of the first move, grows live with the playhead past the song end, ends at the end of the bar where recording stopped; playhead back to 1:1:1 (a5, a6). No cutting. | Reporter's bug: fixed. **DIFFERENT from FL's storage** (FL default = pattern events + pattern clip ending at the last event's bar; LYNX = automation clip growing to the Stop, which is what the reporter asked for). Not changed for 0.1.20: switching song-mode recording to pattern events is a model change against the reporter's explicit request → next version, user's decision |
| Bug: pitch bend not recorded | "does not actually print/record to Pattern" | FL records the wheel as the channel's pitch events | Code path: MIDI thread → bendRecordQueue → takeRecordedBends → touches → commit; test covers playback of a recorded bend lane only | UNVERIFIED end to end: no MIDI input with a wheel and no MIDI loopback on this PC; AudioEngine is not built in core tests. Next version: a test-mode MIDI injection to drive it |
| Crash: fast cycling samples | "about 2 1/2 samples/per second causes LYNX crash" | — | user's report d3b378e5 (WaveformCache prune sort on changing access times); main with a 651-file cache: ~2.5/s, ~1,950 keys at ~20/s, 120 rapid clicks while playing: no crash, cache pruned (s1_03, s1_04) | MATCH (fixed) |
| Crash from the same PC | (crash report d1f26cca, 0.1.18) | — | Driver settings… panel, see #13 | MATCH (fixed) |

## Issue #13 (caiorobertcr7)

| Item | Reporter's words | FL 2026 | LYNX SHARE main | Verdict |
|---|---|---|---|---|
| Crash | crash on Settings › Audio "Driver settings…" (ASIO4ALL) | FL opens the driver's panel and keeps running | ASIO › FL Studio ASIO, Driver settings… opened 9 times, buffer changed 512→256→1024… in the panel each time: no crash; LYNX's buffer box follows the panel (256 after the last) (d5–d8) | MATCH (fixed) |
| 2–8 s freezes | "UI not responding" | FL also blocks while an ASIO driver opens | session hang files: one 2.07 s stall = `ASIOAudioIODevice::open` on the driver switch (JUCE waits for the first callback); none from the 9 panel cycles | MATCH (rest = driver open, as FL) |

## Batch b1 (feature/audit165z-b1)
1. Playlist double right-click (held) = FL 2026: no undo of the first click's deletion; `gesture::MuteSweep` (first clip
   decides) + `gesture::isDoubleClick` in core/EditorGestures.h; test `audit165zb_double_right_click_mute_sweep_follows_the_first_clip`.
2. Sampler knobs: one parameter menu (`ui::showSliderMenu`, `ui::setOwnMenu`; the slider contract skips sliders that
   show their own menu).

## Counts
#11 (14 items): 12 MATCH, 1 DIFFERENT fixed (#9), 1 DIFFERENT kept on purpose (#7 Delete key, reporter's request).
#12 (19 items): 15 MATCH, 1 DIFFERENT fixed (Sampler knob menu), 1 DIFFERENT kept (song-mode storage model), 1 UNVERIFIED
(pitch bend recording), 1 pending b1 Windows check (region → new Sampler).
#13 (2 items): 2 MATCH.

## Where I stopped
(updated at the end of the round)
