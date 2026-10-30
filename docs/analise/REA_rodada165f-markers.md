# REA round 165f, sub-round rea165-markers: FL Studio 2026's song marker actions

2026-10-27. Goal: LYNX's Playlist markers behave like FL's marker actions (Start, Loop, Pattern length, Marker loop,
Marker skip, Marker pause, Bar override, Time signature, Start / Stop recording, Key/Scale), Quantize song jumps,
the Time markers menu items and MIDI "Song marker jump", for LYNX projects and imported .flp.

Evidence tags: **[DFM]** FL's form/menu resources, **[DECOMP]** Ghidra on `FLEngine_x64.dll` (own copy
`D:\lynx-work\rea165-markers`), **[DATA]** FL's demo projects, **[FL render]**, **[MANUAL]**, **[CODE]** LYNX.
Clean-room: behaviour and names only.

## 1. Findings so far

### 1.1 Marker type numbers (the .flp `0x94` event, `type << 24 | position`) [DECOMP + DFM]
Table of 13 internal names at `0x015d5068` (and short names at `0x015d50d0`):
0 normal, 1 marker loop, 2 marker skip, 3 marker pause, 4 loop, 5 start, 6 pattern length, 7 unused (menu "Bar
override" = `EEMarkerMode7Menu`), 8 time signature, 9 start recording, 10 stop recording, 11 loop length,
12 Key/Scale. The marker menu components are named `EEMarkerMode<type>Menu` (None = 0, Start = 5, Loop = 4,
Pattern length = 6, Marker loop = 1, Marker skip = 2, Marker pause = 3, Bar override = 7, Time signature = 8,
Start / Stop recording = 9 / 10); Key/Scale is `EEmarkerMode11Menu` — to be checked whether the stored type is 11
or 12 (REA_rodada6 table says 12).

### 1.2 Use in FL's demo projects [DATA] (`tmp/rea165m/py/scan.py`)
- Performance-mode demos: plain markers over the clip-launch area, then a **Start** (5) marker after it → the song
  plays from the Start marker.
- **Loop** (4): Bombs Away (36864), DEDZII (0), Electroconductor (15744), LollieVox (23040).
- **Marker loop** (1): Exoplanet arrangement 2, at the very end (101376).
- **Marker pause** (3): Astes - Bien Duro, at 35904 (song end area).
- Pattern-level markers (before any arrangement): Edlan & Ella Noël pattern with type 11 (384) and 6 (1536) =
  loop length / pattern length in the Piano Roll.

### 1.3 FL's command-line render ignores every marker action [FL render]
Test projects (`tmp/rea165m/py/mkm.py`: FL's Empty template + a sampler playing a sine, one note per beat whose key
encodes the beat number, an 8-bar clip; markers of each type; renders `FL64.exe /R /Ewav`, analysed by `seq.py`):
base, marker loop, marker skip (with / without a following marker, mid-bar), marker pause, start, loop, pattern length,
bar override — **all 11 renders are identical**: 16.0 s, beats 0…31 in order. FL's export plays the song linearly
from bar 1 to the song's end. (The markers are loaded: the same files opened in FL show them with their action icons,
see 1.4.) → LYNX's export must also ignore the marker jumps.

### 1.4 Live playback in FL (GUI sessions, positions read from FL's song-position display) [SHOT]
Shots in `D:\lynx-work\rea165-markers\shots`.
- `fl_skip_pause_play.png` (Skip at bar 2, plain "Here" at bar 4, Pause at bar 6): 1:12 → (bar 2 reached) **4:08**
  → 5:08 → stopped at **6:01:00** (play button off). **Marker skip jumps to the next marker; Marker pause stops
  playback exactly on the marker.**
- `fl_pause_resume.png`: Play again from 6:01 goes on (6:08, 7:08, 8:16) — the pause does not fire again when playback
  starts on it.
- `fl_marker_menu.png`: FL's Playlist marker menu: Add marker..., Add time signature..., Place loop, the header
  "Skip - marker skip at 2:01:00", Delete, Rename..., Move content left / right, **Action**: None, Start, Loop,
  Marker loop, Marker skip, Marker pause, Time signature, Start recording, Stop recording (9 actions: Pattern length,
  Bar override and Key/Scale are only offered for the Piano Roll's pattern markers).
- `fl_combo2_*.png` (Start at 1:09, plain "From" at bar 2, Marker loop at bar 3, no marker after it): Play from the
  beginning starts at the Start marker and Stop returns there; reaching the Marker loop marker does nothing; at the
  song's end playback goes back to the Marker loop marker (absolute 3072 → 768).
- `fl_marker_loop_section.png` (Marker loop at bar 3, plain "End" at bar 5): 1…4:15 → **3:03**, then 3–4 forever.
  **Marker loop repeats the section from the marker to the next marker (or the song's end).**
- `fl_start_loop_play.png` + `fl_start_ruler.png` (Start at bar 2, Loop at bar 5): in Song mode FL numbers the ruler
  and the song position **from the Start marker** (the clip starts before "bar 1"); Play starts there; at the song's
  end playback goes back to the **Loop** marker (display 7:14 → 4:02 = absolute bar 9 → bar 5), every time.
- `fl_time_markers_menu.png`: Playlist menu › Time markers: Add one... (Alt+T), Add auto (Ctrl+T), Add two...,
  Add one every ▸, Add jump to next bar, Add time signature change... (Shift+Alt+T), Add key marker ▸, Place loop
  (Shift+T), Delete, Change to marker loop. `fl_quantize_song_jumps.png`: Quantize song jumps ▸ Off (default), Beats,
  Bars, 2 bars, 4 bars, 8 bars.
- `fl_quantized_marker_click.png` (Quantize song jumps = Bars): a click on the "Lp" marker's tag at display 1:09
  while playing → the song goes on to **2:01, then jumps to the marker (4:01)**. `fl_quantized_ruler_click.png`: a
  click on the ruler (display 6:05) waits for the next bar line too (4:16 → 6:05): every song-position change
  while playing is quantized, and the target is kept as clicked.
- `fl_add_jump_next_bar*.png`: "Add jump to next bar" with the song position at 6:05 adds a **Marker skip at the
  position and a plain marker one bar later** (7:05; its menu: "at 8:05:00", action None) — that bar is skipped.

### 1.5 Semantics implemented (LYNX, `core/MarkerPlan.*`)
| FL action (type) | LYNX playback | evidence |
|---|---|---|
| None (0) | nothing | — |
| Marker loop (1) | at the next marker (or the song's end) → back to this marker, forever | [SHOT] |
| Marker skip (2) | at this marker → the next marker (or the song's end, where the song starts over) | [SHOT] |
| Marker pause (3) | playback stops on the marker; Play goes on from it (not fired again) | [SHOT] |
| Loop (4) | the song's end goes back to this marker (no loop region on) | [SHOT] |
| Start (5) | Play from the song's beginning (position 0) starts here; without a Loop marker the song's end comes back here | [SHOT] / [HYP] for the wrap |
| Pattern length (6), Bar override (7), Loop length (11), Key/Scale (12) | kept (import / save), shown with their name; no playback effect — FL offers them only for the Piano Roll's pattern markers (`fl_marker_menu.png`) | [SHOT] |
| Time signature (8) | bars (existing) | — |
| Start / Stop recording (9 / 10) | punch in / out (existing, round 139) | — |
| Export | linear from bar 1 to the end: the actions above are dropped from the render song | [FL render] |

Real time: the plan is a fixed table (64 jumps) published by the UI under a seqlock (`Transport::publishMarkers`); the
audio thread copies it when the version changes, splits the block at the jump's sample (`markerStep`), then moves the
position (the renderer sees a position jump: held notes are released and audio clips resume mid-way, as for the loop
wrap). No locks or allocations on the audio thread.

## 2. What LYNX does now [CODE]
- `core/MarkerPlan.*`: the marker table for the audio thread (`makeMarkerPlan`, `markerStep`), Quantize song jumps
  (`markers::quantizedJumpTick`), the menu commands (`setAction`, `addEvery`, `addJumpToNextBar`,
  `changeToMarkerLoop`).
- `engine/AudioEngine`: follows the table while playing in Song mode (skip, pause, marker loop, Loop / Start wrap);
  Play from 0 starts at the Start marker; `jumpTo` / `jumpToMarker` / `jumpToNextMarker` wait for the quantize grid
  while playing; MIDI "Song marker jump" (Settings › MIDI: off / channel 1-16, note 0 = first marker [HYP]).
- Playlist ruler: action markers drawn with their symbol and name; the marker's menu has FL's action list (None,
  Start, Loop, Marker loop, Marker skip, Marker pause, Time signature, Start / Stop recording) and "Place loop";
  the ruler menu has "Time markers" (Add one every 1/2/4/8 bars, Add jump to next bar, Change to marker loop) and
  "Quantize song jumps"; clicking a marker's tag jumps to it. Alt+Numpad * / Alt+Numpad / = next / previous marker.
- Export drops the actions (linear, like FL's render). `.flp` import keeps the type; `.lynx` save/load already
  round-trips `action`; edits are undoable ("Markers").
- Tests: `tests/Markers165Tests.inc` (incl. a block-by-block simulation checked against FL's observed beat orders).

## 3. Not done / open
- FL renumbers the ruler and the time display from the Start marker in Song mode (`fl_start_ruler.png`): not done.
- Pattern length / Bar override / Loop length / Key/Scale are Piano Roll pattern markers in FL: kept, no effect.
- Time markers › Add auto, Add two..., Add key marker, Move content left / right: not done.
- [HYP]: the MIDI note → marker mapping, "Change to marker loop" details, the Start-marker wrap without a Loop marker.
- The audio-clip stream prefetcher may underrun once right after a marker jump far away (as after a ruler click).

## 4. Checks [CODE]
- Tests: build-local with FL's Data: 1760508 checks, 0 failures (`tests/Markers165Tests.inc`; the transport
  simulation reproduces the beat orders read off FL's screen in §1.4).
- Render vs FL: LYNX `--export` of the imported `m_mskip`, `m_mloop`, `m_mpause`, `m_start`, `m_combo` → beats 0-31 in
  order at the same onsets (0.50 s apart) as FL's `/R /Ewav` renders of the same files (§1.3): export is linear in both.
- Real time: `markerStep` with a full 64-jump table costs 41.5 ns per 512-sample block (g++ -O2, this machine),
  0.0004 % of the 10.7 ms block at 48 kHz.
- LYNX marker-menu screenshot next to FL's: not taken — the short GUI session was blocked by a crash-report dialog
  and a third-party plug-in's activation window from the background plug-in scan; the menu's entries are checked by
  the tests instead.

## Where I stopped
- 2026-10-28 20:45 (after the BSOD and the disk-full freeze): the work is complete. Merged origin/main cce0794b (mixer
  landed; conflicts in build-local.sh, PlaylistComponent.cpp and I18nData.h resolved, pt-BR deduplicated), build-local
  with FL Data is running, then land.sh. Earlier landing attempts failed only on random clang crashes (unstable RAM),
  not on the code. What is left is in §3.
