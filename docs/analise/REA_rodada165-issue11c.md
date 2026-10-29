# Round 165 "issue11c": issue #11 — right-click to arm a Playlist track (n3m351S) and Shift+Space re-measured

n3m351S (2026-10-29): "right-clicking on the track button in the Playlist window to automatically arm it for recording".
Coordinator: also re-measure FL's Shift+Space (Jocho-Smith: "would not just stop … and play it back to where the cursor
was last, but where shift+space was pressed") in every setup, and fix round issue11b's implementation if it differs.

## Where I stopped

- Done: FL observed, implemented, lynx_tests 0 failures (1887113 checks, with FL Data), Windows build checked by hand. Landing.

## 1. FL 2026: arming from a Playlist track [SHOT] [STRINGS]

FL 2026.1.7, fresh template, real mouse input (SetCursorPos + mouse_event), shots in
C:\Users\kauan\lynx-test\issue11c\f*.png.

| step | FL result |
|---|---|
| right-click the name of an unassigned track ("Track 1") | the track menu (Rename…, Track mode, Performance settings, Size…, no arm item) |
| Track mode › Audio track › "6. Insert 6" | the track is renamed "Insert 6", an "A" icon appears left of the name, hint "Insert 6 (Audio track, linked to mixer track 6)" |
| right-click the name / the "A" icon of that audio track | the same track menu; nothing is armed |
| track at 100 % height | only the name and the mute LED |
| Size › 150 % or 200 % | a row of three buttons appears: hints "Audio input source: (none)", "Recording: Post EQ, Monitoring: When armed", "Arm/disarm recording" |
| left-click "Arm/disarm recording" (Insert 6 has no input) | armed at once, no prompt: the button turns red, the "A" icon turns red, the mixer strip of Insert 6 shows its arm |
| right-click "Arm/disarm recording" (armed or not) | nothing changes, no menu |
| left-click it again | disarmed |
| instrument track (Track mode › Instrument track › Sampler) at 200 % | no buttons |

FLEngine_x64.dll strings: the Playlist track form has `PLTrackInputSelectMenu` (input source) and `PLTrackMonitorMenu`
(Record: External input only / External and mixer input / Post effects / Post EQ / Post level and panning / Post track;
Monitor external input: Off / When armed / Auto / On), and the track menu has a hidden "Track options" group (Select
audio input, Arm recording, Disarm recording) that never showed in the menus above. FL's WhatsNew: "Show indication of
'armed' status of the linked Mixer track on an audio track header in the Playlist", "Rail color for sliders on Mixer
tracks is not updated when (dis)armed from the Playlist track".

So FL has no "right-click arms" gesture: right-click on a track header is always the track menu (on the LED: solo).
Arming from the Playlist = the record button of an *audio track* (linked to a mixer track).

## 2. LYNX before

Track mode › Audio track existed (round 146a, links a free mixer insert); the header had only the name, the group arrow
and the mute LED. Arming was only in the Mixer (`mixerinput::toggleArm`) and Shift+R-style "arm selected insert".

## 3. LYNX now

- An audio track's header shows a record button left of the LED (and of a group arrow): [LMB] arms / disarms its linked
  mixer track (`mixerinput::toggleArm`, the same as the Mixer's arm switch), red while armed — also when armed from the
  Mixer. [RMB] on it does nothing (FL). On rows of 52 px or more (FL: 150 %) an "Audio input source" button sits next
  to it (`mixerinput::showMenu`: inputs + monitoring + record safe). Unassigned / instrument tracks: unchanged.
- Right-click on the name keeps opening the track menu (FL).
- Difference kept on purpose: when the insert has no input, LYNX's arm (Mixer and Playlist alike) opens the input menu
  first and arms once an input is chosen; FL 2026 arms without an input.
- Code: `trackmodes::armableInsert` (core/PlaylistOps), `PlaylistComponent::trackArm / trackInput`.

## 4. Shift+Space in FL 2026 [SHOT]

Same session, keys by SendKeys to FL's window, position read from FL's time display (shots in issue11c\A, B, C, R, T, P,
Q). 130 BPM, clips over bars 1-8.

| # | mode / focus | setup | action | FL |
|---|---|---|---|---|
| A1 | Song / Playlist | stopped 1:01 | Space, 3 s, Shift+Space | stops, 1:01 |
| A2 | Song / Playlist | stopped 1:01 | Shift+Space | plays from 1:01; Shift+Space again: stops at 1:01 |
| B1 | Song / Playlist | ruler clicked at bar 5 | Space (plays from 5), Shift+Space | stops at **1:01** |
| B2 | Song / Playlist | ruler clicked at 5, stopped | Shift+Space | plays **from 5** (6:02 after 2 s); Shift+Space: stops at 1:01 |
| C1 | Song / Playlist | ruler at 5, stopped | Shift+Space, then Space | plays from 5; Space stops back at **5** (as after Space) |
| C2 | Song / Playlist | playing from 5, ruler clicked at 9 while playing | Shift+Space | stops at 1:01 |
| C3 | Song / Playlist | playing from 1:01, ruler clicked at 9 | Space | stops back at 9 (Space goes back to the last start/click) |
| R1 | Song, recording (Notes and automation) | ruler at 5 | Space (records from 5), Shift+Space | stops at 1:01 |
| R2 | Song, recording | ruler at 5, stopped | Shift+Space | records from 5; Shift+Space: stops at 1:01 |
| T1 | Song / Playlist | time selection bars 3-7 (Ctrl+drag on the ruler) | Space, Shift+Space | stops at **3:01** (selection start) |
| T2 | Song / Playlist | selection 3-7, stopped at 3:01 | Shift+Space | plays from 3:01 (loops in the selection) |
| T3 | Song / Playlist | selection 3-7, ruler clicked at 6 | Shift+Space | plays from 6 (loops to 3); Shift+Space: stops at 3:01 |
| P1 | Pattern / Channel Rack | stopped 1:01 | Space, Shift+Space | stops at 1:01; stopped Shift+Space plays, again: 1:01 |
| Q1 | Pattern / Piano Roll | Piano Roll ruler clicked at 1:09 | Shift+Space | plays from 1:09; Shift+Space: stops at 1:01 |
| Q2 | Pattern / Piano Roll | ruler at 1:09 | Space, Space | plays from 1:09, stops back at 1:09 |

Settings: no FL option changes this (FLEngine strings searched for rewind / return / "from start"; only the scripting
API's `rewind`). So FL's Shift+Space is: **playing/recording → stop at the start of the time selection (Song mode) or
of the song / pattern; stopped → play from the current position, exactly like Space.** It never stops "where
Shift+Space was pressed".

Round issue11b had the playing half right (except the time selection) and the stopped half wrong (it rewound to 1:01
and played from there). Fixed: `MainComponent::playFromStart` (stopped → `playStop()`; playing → stop at
`transportedit::shiftSpaceStopTarget`). Shortcut label: "Play, or stop at the start of the song or pattern".

## Tests

`issue11c_audio_track_arms_its_mixer_track`, `issue11c_shift_space_stop_target_matches_fl` (tests/Issue11cTests.inc).

## Windows check (fresh LYNX_PROFILE_DIR, LYNX_TEST_MODE=1, real mouse; shots C:\Users\kauan\lynx-test\issue11c\l*.png, L, w*, v*, z*)

- Track 1 › Track mode › Audio track: a record button appears left of the LED, hint "Audio track: [LMB] arm / disarm
  recording (its mixer track)". Click: the insert had no input, so the input menu opened; after picking "Input
  channel 1 - Input channel 2" the button is red and the Mixer shows insert 6 ("Track 1") armed. Right-click on the
  button: nothing (stays armed, no menu). Disarming in the Mixer greys the Playlist button; a click arms again at once.
  Taller row (divider dragged to 68 px): the input button appears next to it (lit: the insert has an input).
- Shift+Space with real key events (keybd_event): marker at 5, Space plays from 5, Shift+Space stops at 1:1:1;
  stopped Shift+Space plays from 1; marker at 5 + Shift+Space plays from 5 and Space then goes back to 5; with a time
  selection 3-7, Shift+Space while playing stops at 3:1:1 — the same as FL in every case.
- Note: WScript SendKeys "+ " reaches LYNX as plain Space (JUCE doesn't see the synthetic Shift), while FL gets
  Shift+Space; keybd_event is needed for LYNX tests.
