# Round 166d (feat166d): issue #14 feature requests — fold to scale, ghost channels, 500 mixer inserts

Issue #14 (kubapl2004gmail, Linux), "Feature Requests". The three bugs of the same issue are round 166a's.

## Where I stopped

- All three items implemented and merged with round 166a (snap to scale moves every note, lit keys): when the view is
  folded, dragging / Up-Down move by shown rows; otherwise 166a's snap-to-scale rules apply. build-local with FL Data
  after the merge: 1 887 689 checks, 0 failures. Piano Roll, Mixer, Main and ProjectFile also syntax-checked against
  the Linux JUCE build. Next: landing (land.sh), then the Windows GUI check (shots) on the landed build.

## FL evidence (FL Studio 2026.1, this PC)

Session 2026-10-31 00:41–00:44, FL 2026 opened with the demo "Asher Postman - Future House.flp" (copied to
C:\Users\kauan\lynx-test\feat166d\demo.flp), real mouse input (SetCursorPos + mouse_event), FL + wingui locks, FL closed
by its PID afterwards. Shots in C:\Users\kauan\lynx-test\feat166d\ (a*.png, b*.png).

| # | step | FL result |
|---|---|---|
| a6 | Piano Roll (F7), right-click the ghost notes button at the right end of its title bar (hint "Show ghost notes") | a menu: header "Ghost channels", "All", "None", a line, then every channel of the project in Channel Rack order (automation clips included), each with a check mark; the edited channel (ASHER POSTMAN) in bold; two columns when long |
| a7/a8 | click "Basic Sidechain", right-click the button again | the menu closed at the click; reopened, "Basic Sidechain" has no check mark, the others keep theirs |
| [DFM] | TEventEditForm (tmp/rea165/dfm): `GhostNotesBtn` (Hint "Show ghost notes", Options2 tbStayDown, PopupMenu = `GhostMenu`), `GhostMenu` (OnPopup = GhostMenuPopup, one static item "-Ghost channels" = the header, the rest filled at popup) | the button toggles the ghosts; its right-click menu is built from the channels each time |
| [TXT] | FL's WhatsNew.htm: "7571 Added a ghost note channels panel in the piano roll to select what channels to show ghost notes for", "19904 Channel names starting with hyphens cannot be selected in the ghost notes menu", "20457 The ghost notes menu doesn't work correctly after cloning an instrument" | per-channel ghosts are an FL feature |
| [DFM] | Piano Roll View menu: "Scale highlighting" (Root note C…B, Scale list, Default / Black notes / Automatic, "Snap incoming MIDI"), "Snap PR notes to scale", Keyboard options (Classic / Modern / Flat / Dark / List, Key labels…) | no fold / hide of the out-of-scale keys anywhere in FL's Piano Roll menus: FL only highlights the scale |
| b1 | Mixer (F9), right-click its "+" ("Add a new track after the selected track") | menu: Add one / Add multiple... / Add from state |
| b2–b4 | Add multiple..., type 999, Enter (the project has 125 inserts) | error box "You can add a maximum of 375 tracks." — 125 + 375 = 500 inserts |
| [TXT] | WhatsNew: "20040 Dynamic mixer: the Add Tracks button in the mixer title bar is not disabled when there are 500 tracks", "19861 Mouse stops responding after adding the 500th mixer track", "20000 Export project bones always exports 500 mixer tracks" | FL 2026's dynamic mixer: up to 500 inserts (+ Master, + Current) |

## 1. "An option to hide notes completely (like in ableton) that are outside the scale selected."

| | |
|---|---|
| FL | no such option (DFM menus above): Scale highlighting and Snap to scale only. So this is LYNX's own (as asked, like Ableton Live's Scale fold). |
| LYNX before | the scale box highlighted the in-scale rows; "Snap notes to the scale" |
| LYNX now | Piano Roll › Note tools menu (the "..." button) › **"Fold to the scale (hide the other keys and their notes)"** (enabled when a scale is chosen). Only the in-scale keys are rows (the keyboard turns into a named list, the octave line sits under each root); notes on the other keys are not drawn and are left alone by every edit (selection, Ctrl+A, the tools, the velocity lane only see shown notes). Moving notes by mouse or Up/Down steps through the shown keys (scale degrees; Shift+Up/Down still moves an octave). Changing the scale refolds; the key at the top of the view stays put. Core: `notetools::keyRows` / `KeyRows::step`. |

## 2. "An option to individually select which ghost channels are visible rather than all or nothing."

| | |
|---|---|
| FL | the ghost notes button's right-click menu (a6–a8 above) |
| LYNX before | the ghost button / Alt+V: all other channels or none |
| LYNX now | as FL: right-click the ghost notes button → "Ghost channels" header, All, None, then every channel with a check mark (the edited one in bold); a click toggles that channel (also Note tools menu › Ghost channels). Picking channels turns the ghosts on. Stored per channel (`ChannelDef::ghostHidden`, saved in .lynx as "ghostHidden" when set); hidden ghosts are not drawn, not hit (double right-click, editable ghosts) and not snap targets. |

## 3. "Atleast 500 allowed mixer tracks for feature parity with fl studio or just get rid of the limit in general."

| | |
|---|---|
| FL | 500 inserts (b4 + WhatsNew) |
| LYNX before | `maxMixerTracks = 128` (127 inserts) |
| LYNX now | `maxMixerTracks = 501`: Master + 500 inserts, FL's limit. Every user of the constant was checked (list below). "Insert multiple..." / the "+" menu's new "Add one / Add multiple..." (FL's) refuse more than the room left with FL's message ("You can add a maximum of N inserts."); inserting shifts every reference in one pass (it used to swap the new insert into place one neighbour at a time — ~100 000 song walks for 375 inserts). Fruity Send's "Send to" parameter is normalised over the inserts: files now save `"sendSpan": 500` and older files (0…127) are rescaled on load, so they keep their target. |

Checked users of `maxMixerTracks`: RackRenderer (mix / scratch buffers sized at prepare; per-block loops run over the song's
inserts, not the maximum; idle inserts measured below), ProcessTimes / UnitTimes / Meters / LiveInput / PluginMonitor
arrays (heap or a few KB), graph capacity, MixerOps (ensureInsert, linkChannels, insert, delete), Song (defaultMixer,
claimFreeInsert, nextFreeInsert), Templates (inserts in new projects up to 500), FlpImport (FL projects with more than
127 inserts now import every insert), ProjectFile (load / jlimit), ChannelSettings / Channel Rack target spinner (500),
SettingsPages metronome track list, Patcher, AudioEngine input peaks, MixerComponent ("+", insert, menus).

## Measurements

build-local tests (`LYNX_TEST=s166d`), best of 5 × 400 blocks of 256 frames at 48 kHz, one sampler channel playing:

| | 16 inserts | 500 inserts |
|---|---|---|
| audio thread per block | 7.5–7.7 µs | 158–185 µs (+0.31–0.37 µs per idle insert, ~3 % of the 5.3 ms block) |
| sound | — | identical samples |

Idle insert = no effects, built-in EQ flat (or clean mixer) and a silent buffer: it skips effects, EQ, stereo, gain ramp,
meters and stays inactive, so the Master doesn't sum it. What is left per idle insert is the buffer clear and the
silence check (2 × 256 floats each).

`mixerops::insertTracks`: 375 inserts at the front of a 125-insert mixer with 64 routed channels: 0.02–0.04 ms (one
remap; the old per-insert swap chain walked the song ~100 000 times).
