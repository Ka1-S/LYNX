# Round 166e — issue #16 (esiowry): Playlist / Mixer / Piano Roll items → v0.1.21

FL Studio 2026 driven as a black box (real mouse / keyboard input through SetCursorPos + mouse_event / keybd_event,
full-screen shots, FL's own hint bar read back), FL 2026.1, template Minimal › Basic and Packs › Loops › "DL Breaker".
Shots: C:\Users\kauan\lynx-test\pl166e\*.png (not in the repo). FLEngine_x64.dll strings for the option names
(clean-room: names and observed behaviour only, no code).

## Where I stopped

- Done. FL measured (all 7 items); implemented with tests (tests/Round166eTests.inc); build-local with FL Data:
  1,887,404 checks, 0 failures; Windows build checked by hand in test mode (E:\lynx-work\pl166e\*.png): crossfade
  S-curves on and off, gain handle hint / drag (100 px = +5.0 dB) / menu, ruler double-click drag (selection, clips
  selected, position at its start), Settings › Project › Playlist, record menu, Mixer › Disk recording › Options,
  Piano roll Alt+wheel (nearest note, selection from anywhere) and the note template.
- Not done: FL .flp import of FL's crossfade flag (imported FL projects keep it off); FL's render of armed tracks also
  drops the rendered audio on the Playlist (not part of this issue).

## 1. Playlist: automatic crossfades toggle

| | |
|---|---|
| Reporter | "when edges of two audio clips intersect one another ... they do not have the ability create crossfades between them. FL Studio has a toggle for this under Options > Project Settings." |
| FL evidence | Options › Project general settings › Playlist has **"Automatic crossfades"** (this project) and **"Enable automatic crossfades in new projects"** (global, on by default; DLL strings `EEEnableAutomaticCrossfadesAction`, `AutomaticCrossfadesInNewProjectBox`). The Playlist menu › Edit also has "Automatic crossfades (current project)". [SHOT] Two audio clips overlapping on a track: the crossfade covers the **whole overlap**, drawn as two crossing S-curves; switching it off removes them at once, switching it on applies to overlaps already there; moving a clip updates it. A clip lying entirely inside another gets none. FL's What's new: older projects load with it off; it is saved with the project. |
| LYNX before | No crossfades: overlapping clips simply summed (a level bump over the overlap). |
| LYNX now | `Song::autoCrossfades` (saved as `autoCrossfades`, missing = off as in FL). `clipops::updateCrossfades` derives `Clip::crossfadeIn / crossfadeOut` (the overlap) after every edit and on load; the scheduler sends them as the clip's fades with an S shape (`NoteEvent::fadeShape`), and the sampler voice applies the raised-cosine curve (`clipops::crossfadeCurve`, the two sides add up to 1: the level stays flat through the overlap — tested by rendering). The Playlist draws the two crossing S-curves. Toggles: Settings › Project › Playlist (both FL options), Playlist menu and ruler menu "Automatic crossfades (current project)". New projects take the global choice (`NewProjectDefaults::autoCrossfades`, key `newAutoCrossfades`). |

## 2. Playlist: audio clip volume from the bottom edge

| | |
|---|---|
| Reporter | "click along the bottom edge of an audio clip and drag up and down to change the volume level of the audio clip ... separate and independently from their source volume in the channel rack." |
| FL evidence | [SHOT] Hovering an audio clip shows a small **semicircle at the bottom centre**; its hint: "Gain: x dB". A vertical drag changes the clip's gain **0.05 dB per pixel, linear in dB**, from **−96.1 dB (shown "-INF")** to **+36 dB**; the waveform scales with it. With several clips selected, the drag moves them all **relatively** (10 → 5 dB and 5 → 0 dB together). Right-click on the handle: Reset gain, Normalize, Copy value, Paste value, Type in value... |
| LYNX before | Clip gain only from the clip menu's "Clip volume" presets (+6 … −12 dB) and Normalize. |
| LYNX now | The same handle (drawn on the hovered audio clip), hint "Gain: x dB" / "-INF dB", drag at 0.05 dB/px over −96.1 … +36 dB, every selected audio clip relative to its own value, one undo step per drag; right-click menu Reset gain / Normalize / Copy value / Paste value / Type in value... The waveform already followed the clip gain. |

## 3. Playlist: double-click-and-drag on the ruler = time selection

| | |
|---|---|
| Reporter | "you can double click and hold, then drag in either direction to create a time selection as well." |
| FL evidence | [SHOT] Double-click held on the ruler and dragged (either direction) draws a time selection; the clips in it are selected and the song position goes to the selection's start. A plain double click (released in place) clears the selection. |
| LYNX before | Only Ctrl+drag; a double click cleared the selection. |
| LYNX now | The second click of a double click starts the same time-selection drag as Ctrl+drag (selects the clips in it); released after dragging, the song position goes to the selection start; released in place it still clears. Ruler hint updated. |

## 4. Mixer: disarm the inserts after recording

| | |
|---|---|
| Reporter | "Toggle to automatically disarm inserts after recording has completed ... In FL, this option appears under the Mixer's Window Options" |
| FL evidence | [SHOT] FL has two related options, neither of which disarms the inserts after a live take: (a) the record button menu's **"Disarm on stop"** (`RecDisableOnStopMenu`) switches the **main record switch** off when playback stops (checked on and off); (b) Mixer menu › Disk recording › Options **"Auto-unarm"** (default off) disarms the armed tracks after **"Render armed tracks to wave file(s)"** — after a live take with it on, the inserts stayed armed. |
| LYNX before | The record menu had "Disarm the inserts when recording stops" (wrongly commented as FL's "Disarm on stop"); nothing in the mixer. |
| LYNX now | FL's two options as FL does them: record menu "Disarm on stop (recording switches off when playback stops)" (`RecordOptions::recordOffOnStop`, `RecordOffOnStop` helper) and Mixer › Disk recording › Options "Auto-unarm (rendering the armed tracks disarms them)" (`unarmAfterRender`: the render's completion disarms the tracks it rendered). What the reporter asked — disarming the inserts after a take — is **LYNX's own** option "Disarm the inserts when recording stops", now also in Mixer › Disk recording › Options (same setting as the record menu's). All saved in ui-options. |

## 5. Piano roll: Alt+wheel on the selection without hovering

| | |
|---|---|
| Reporter | "simply having them selected and scrolling is enough to adjust the velocities regardless of the position of the mouse." |
| FL evidence | [SHOT] With a selection, Alt+wheel changes **every selected note wherever the mouse is** (even over an unselected note). With nothing selected, it changes the **note nearest to the mouse**. Step ≈ 3.1 % (≈ 4/128) per notch. |
| LYNX before | Only over a note: the hovered note (or the selection if the hovered note was in it). |
| LYNX now | Selection: all selected notes wherever the mouse is in the grid / lanes; no selection: the hovered note, else the nearest one (`noteedit::nearestNote`, pixel distance to the note's rectangle; in the lanes by time). |

## 6. Piano roll: clicking a note remembers its properties

| | |
|---|---|
| Reporter | "clicking on a note remembers its velocity and assigns that velocity to a new note when clicking to create it." |
| FL evidence | [SHOT] After clicking a note, new notes take its **velocity and pan** (both checked); Alt+wheel on the nearest note (no selection) updates the template too. |
| LYNX before | Only the length was remembered. |
| LYNX now | `noteedit::NoteTemplate` (velocity, pan, release, fine pitch, mod X / Y) taken from the clicked note and from a single-note Alt+wheel edit; new notes from a click, the brush and the drum paint tool get it. (Release, fine pitch and mod X / Y travel with velocity and pan; only velocity and pan were measured.) |

## 7. Piano roll: hear the note while wheel-adjusting

| | |
|---|---|
| Reporter | "It would be useful to hear the note play at its new velocity when scrolling" |
| FL evidence | [SHOT] FL plays the note on Alt+wheel only in the no-selection (nearest note) case; with a selection nothing sounds. |
| LYNX now | The note sounds briefly at its new velocity on every Alt+wheel step; with a selection, the selected note nearest to the mouse (that part is **LYNX's own**, as asked). |
