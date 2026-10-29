# REA round 165g — sub-round flnewt165: Newtone and Newtime out of "(beta)"?

Sub-round of r165g flplugins165. Goal: drop "(beta)" from Newtone, then Newtime, only where LYNX is verified to
match FL. Clean-room as every REA round (behaviour in our own words, no Image-Line / zplane code or data in the
repository). Newtone's engine is zplane's licensed code (`Newtone_Engine_x64.dll`), so it stays **black-box**: no
decompilation, only renders of FL's unmodified `Newtone_x64.dll` in our EXEC harness (round 73 / 95, `fx.c`) — the
same plug-in FL runs on a mixer insert — compared with LYNX's renders of the same states.

Evidence markers: [EXEC FL] (the installation's DLL run unmodified in our harness), [MANUAL] FL's online manual,
[TEST], [SHOT], [HYP]. Data: `/mnt/e/lynx-work/flnewt165` (`nt/` states, FL renders `fl_*.f32`, LYNX renders
`lx_*.f32`; `tools/` our scripts: `ntr.cpp` LYNX render, `cmp.py` per-note comparison, `lvl.py`, `harm.py`,
`fine.py`, `env.py`, `trc*.py`, `run2b.py` + `flrun.sh` FL renders under the FL lock; `results/`).

## Where I stopped
- 2026-10-29 12:40 — Newtone out of beta (engine §1, editor §2, tests §3, shots §4); Newtime stays beta (§5, the
  next version's work). Landing.

## 1. Newtone's engine against FL [EXEC FL]

Method: `cmp.py` renders each state with LYNX's `newtonefx::renderEdits` (`tools/ntr`) and compares it with FL's
render of the same state, per note (FL's note spans): median and 90th percentile of the frame-by-frame pitch
difference (our YIN, 1024-sample frames, octave jumps of either tracker excluded and counted), RMS level
difference, pitch spread. A sample-level null is meaningless here (two PSOLA engines place their grains
differently), except where both copy the source (unedited notes: −141 dB).

States: round 95's 60 (`tmp/r95/nt`, three synthetic vowels: v1 steady / vibrato / steady notes, v2 legato with
80 ms glides, v3 instant steps) plus 75 new ones (`results/spec_*.txt`): pitch offsets −12…+12 st on a steady and
a vibrato note, formant factors 0.5…2, pitch × formant, transitions with long / zero smooth times on a step, tremolo,
ramps, stereo. Harness note: the first render after a new source file sometimes ignores the edits (the engine's
analysis cache, `ud\Newtone`); `run2b.py` renders every state twice and keeps the second.

### 1.1 Grain size and gain (the level errors)
LYNX used two-period grains of the *source* period with a gain f/r (r pitch ratio, f formant factor). FL's levels
stay within −2.2…+0.6 dB of the source whatever the shift (sweep in `results/spec_sweep.txt`), LYNX's went from
+2.3 dB (−12 st) to −8.5 dB (+12 st, formant 0.5). Harmonic-by-harmonic spectra of the steady note (`harm.py`) give
FL's grain: **no gain correction; the Hann window spans two periods of the output pitch, at most three source
periods, divided by the formant factor when it is above 1** (half-length = P·min(1/r, 1.5)/max(1, f)). The octave
down is the telling case: FL's sub-harmonics sit 14–20 dB under their neighbours, which a 2P window (−6 dB) or a 4P
window (no shift at all) cannot give; the 3P cap reproduces −20 / −19 / −14 dB exactly. Mean harmonic error over 12
shifts / formants: 2.96 dB (old) → 1.44 dB.

| sweep (steady A3) | FL level | LYNX before | LYNX now |
|---|---|---|---|
| −12 st | −2.18 dB | +2.27 | −2.19 |
| −7 st | −2.11 | +1.02 | −2.42 |
| +4 st | −0.13 | −2.27 | −0.26 |
| +12 st | +0.46 | −8.45 | +0.17 |
| formant 0.5 | +0.45 | −8.44 | +0.19 |
| formant 2.0 | −3.61 | +2.31 | −3.71 |

Also: the synthesis step is now the distance to the next analysis mark (marks lock to the waveform's peaks), not the
smeared pitch-frame period.

### 1.2 Tremolo (`fTremAmp`) [EXEC FL, `spec_trem.txt`]
Gain × (1 + T·e(u)·sin(2π·t/V)), t from the note's start, V = `fVibFreq` (a period in seconds), in step with the
vibrato; e(u) = the vibrato amplitude envelope (`fVibAmpStart` → `fVibAmpEnd` across the note) divided by its peak,
so the depth does not depend on the vibrato's size (0.5, 1.0 and 0.001 semitone give the same tremolo). Both
amplitudes 0 → FL's engine outputs NaN; LYNX: no tremolo. LYNX now matches window by window (T 0.25 / 0.5 / 1,
2 Hz and 10 Hz, a 0 → 1 envelope).

### 1.3 Pitch ramps (`fRampIn` / `fRampOut`) [EXEC FL, `spec_ramp*.txt`]
`fRampIn` semitones at the note's start, linear to 0 over `fPitchSmoothInTime`; `fRampOut` reached at the note's
end, linear from 0 over the last `fPitchSmoothOutTime`; independent of the volume fades. Implemented; frame by frame
within 0.05 st of FL. LYNX's own format ("LXNT") is version 2 with the ramps (version 1 still loads).

### 1.4 Checked, already matching
Stereo sources: each channel through the same grains (FL and LYNX keep a 3-sample inter-channel lag and the 0.5
ratio; note level L −0.18 / R −0.08 dB vs FL). Transitions between notes on a step (`q_*`): the round-95 rule (the
4.35 % step over the last half of the earlier note's smooth-out time, then a linear ramp over the later note's
smooth-in time) is confirmed for every combination of 0 / 3000 / 8000-sample times and either note edited.

### 1.5 Result (135 states, `results/cmp_after2.txt`; before: `cmp_before2.txt`)
| | before | now |
|---|---|---|
| per-note level within 0.5 dB of FL | 62 / 135 (worst 8.8 dB) | 135 / 135 (worst 0.39 dB) |
| median pitch within 10 c | 135 | 135 (worst 8.7 c: FL's own +1191 c for +12 st) |
| 90th-percentile frame pitch within 10 c | 126 | 133 |
| unedited notes | bit-identical | bit-identical |

### 1.6 Not reproduced (documented differences)
- Legato transitions (a sung glide across the note boundary) with long smooth times: FL starts the ramp earlier and
  follows the source's glide; LYNX applies the step rule → up to 40 c (90th percentile) inside the glide of `l_up2s8`
  (smooth times 8000), 8 c with FL's default times.
- After an edit FL's later unedited notes come out shifted by a whole number of samples (round 95 §2.5); LYNX keeps
  them in place (sub-millisecond, inaudible).
- FL's pitch lands up to ±9 c off the exact interval on large shifts (+12 st → +1191 c); LYNX is exact.
- At a 48 kHz host rate our harness's FL engine plays a 44.1 kHz source 1.47 st higher with its timing kept (likely
  a harness artefact: FL itself not checked); LYNX resamples (pitch kept).

## 2. Newtone's editor [MANUAL, SHOT]

FL's Newtone window (online manual, "Newtone"): a note view of the analysed sample, the global **Center / Variation /
Transition** knobs, **Cut mode** (click slices, Ctrl+click glues), **Advanced Edit** per note (pitch variation,
ramp-in / ramp-out, formant, pitch and volume; a vibrato view with frequency, start / end intensity and its volume
envelope), Alt+drag for unsnapped pitch, right-click snaps a note to the semitone, **Snap to scale** + **Scale**,
multi-selection, undo, **Send to piano roll** / **Create Score**, **Save as MIDI**. LYNX's editor
(`src/ui/NewtoneEditor.cpp`, our own layout in LYNX's dark style) now has each of these, on core functions the tests
cover (`NewtoneFx.h`):

| FL | LYNX |
|---|---|
| Cut mode, click slices | **Cut** toggle (C): click slices the note there (`splitNote`; each half gets its own analysed median pitch) |
| Ctrl+click in Cut mode glues | the same, or "Glue to the note before" in the right-click menu (`glueNote`: the earlier note's edits kept) |
| Center / Variation / Transition | global bars: apply to the selected notes, all when none (`applyCentre` 0…100 % toward the semitone, `applyVariation` 0…100 % of the drift and vibrato kept, `applyTransition` × the analysed smooth times) |
| Snap to scale + Scale | key + scale combos (Chromatic, Major, Minor, harmonic / melodic minor, the modes, pentatonics, blues; `nearestInScale`, ties go down) and **Snap all** |
| Advanced Edit per note | note panel bars: Pitch (st), Formant (st), Volume (dB), Variation, Ramp in / out, Vibrato rate / start / end, Tremolo (all the FL state fields of §1) |
| Alt+drag, right-click snap | Alt+drag = free pitch; right-click menu "Snap to the nearest note" (plus mute, reset, slice, glue, volume) |
| multi-select, Select all, Deselect | Shift+click, rubber band on empty space, Ctrl+A, Esc |
| Undo | Ctrl+Z / Ctrl+Y (the engine's own edit history) |
| Send to piano roll, Save as MIDI | **Score** menu: send the notes to a channel's pattern, or save them as a MIDI file (`score`: one note per enabled note, edited pitch rounded) |

LYNX extra: a keyboard ruler (a click sets the selected notes' pitch). Missing (next version): Sync to the project
tempo, Snap to grid, the clipboard actions, Send to Playlist / Export regions / drag-out, the select-before / after
commands, auto-scroll, the vibrato *volume* envelope view (LYNX has the tremolo amount only).

## 3. Tests [TEST]
`tests/FlNewt165Tests.inc`: `flnewt165_newtone_levels_follow_fl` (FL's sweep: +12 / +7 st and formant 0.5
within 1.5 dB of the source, −12 st −0.5…−4 dB, formant 2 −2…−5.5 dB; the pitch within 0.1 st of the target), `_tremolo_and_ramps` (tremolo +3.52 / −6.02 dB at the
cycle's peak / trough against the same note without tremolo, for a 0.5 and a 0.001-semitone vibrato; ramp-in +1 st at
the start, gone after the smooth-in time, ramp-out reached at the end), `_fl_fields_and_native_v2` (FL's ramp and
original smooth-time fields parsed; LYNX format v2 round trip; v1 still loads), `_editor_tools` (split / glue,
Center / Variation / Transition, scale snapping with ties down, the score). `Natives142Tests`: the menu name is now
"Newtone"; Newtime keeps "(beta)".

## 4. Shots [SHOT]
`--snapshot` on Windows (`shots/s-newtone.png`, `s-newtime.png`): toolbar (Load sample, Analyse, Cut, Snap all,
Reset, Preview, Save as WAV, Score), the Center / Variation / Transition bars with the key and scale boxes, the note
view with its keyboard ruler, the note panel. Two faults seen and fixed: the ruler's lowest key was painted over the
status line (the ruler and the note view now clip to their areas), and Newtime's "(beta)" note was replaced by the
file's details once a sample was loaded (it now leads the status line).

## 5. Decision
**Newtone leaves beta** (`NativeEngines.cpp`): its engine follows FL's own on 135 states (every note's level within
0.4 dB, median pitch within 9 c, tremolo and ramps window by window, unedited notes bit-identical) and the editor
has FL's tools (§2). The gaps of §1.6 and §2 are listed in `docs/PLUGINS.md` (status stays **Close**).

**Newtime stays beta**: FL's warp state (`CzplJTimelineWarpSource`, its marker XML) is not parsed into LYNX's warp
map, and zplane's élastique time stretch is not reproduced (LYNX's own WSOLA plays LYNX-made warps). Next version:
(1) parse FL's Newtime markers and slice states into `WarpMap`; (2) EXEC FL's `Newtime_x64.dll` in the harness on
stretch ratios 0.5…2 with transients / sustained material and fit LYNX's stretch to its levels and timing, as §1 did
for Newtone; (3) FL's Newtime editor features (auto-markers on transients, tempo sync, the stretch modes).
