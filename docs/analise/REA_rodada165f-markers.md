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

## Where I stopped
- 16:15 — PAUSED by the coordinator (machine load). Done: briefs read; LYNX code located (Song.h `Marker`,
  FlpImport.cpp ~3300, ProjectFile.cpp ~1680/1957, Recorder.cpp punchRanges, AudioEngine signature table ~2490);
  own Ghidra scripts in `tmp/rea165m/gh` (g.sh, Str.java, Ptrs.java, Dec.java); marker type table decoded (§1.1);
  demo projects scanned (§1.2). No code changes yet.
- Next: decompile the transport's marker handling (find reads of the marker type field, e.g. from the
  `EEMarkerMode*Menu` handlers / the 0x94 loader), FL renders of small test .flp (base project with pitched notes
  per beat + markers of each type) via `FL64.exe /R /Ewav` under the FL lock, then implement in AudioEngine.
