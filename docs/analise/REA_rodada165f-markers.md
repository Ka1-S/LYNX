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

## Where I stopped
- 16:15 — PAUSED by the coordinator (machine load). Done: briefs read; LYNX code located (Song.h `Marker`,
  FlpImport.cpp ~3300, ProjectFile.cpp ~1680/1957, Recorder.cpp punchRanges, AudioEngine signature table ~2490);
  own Ghidra scripts in `tmp/rea165m/gh` (g.sh, Str.java, Ptrs.java, Dec.java); marker type table decoded (§1.1);
  demo projects scanned (§1.2). No code changes yet.
- Next: decompile the transport's marker handling (find reads of the marker type field, e.g. from the
  `EEMarkerMode*Menu` handlers / the 0x94 loader), FL renders of small test .flp (base project with pitched notes
  per beat + markers of each type) via `FL64.exe /R /Ewav` under the FL lock, then implement in AudioEngine.
