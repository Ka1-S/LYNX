# Round 164e (misc164) — Browser folders window, content filter hides empty folders, Mixer knobs vs EQ, fixed-size windows taller than the screen, Linux check

2026-10-27. What 0.1.18 left over (tmp/round164-brief.md "r164e misc164"). Two parts went to sub-rounds with their own docs:
mod wheel / aftertouch on the native instruments → REA_rodada164e-modwheel.md (misc164m); FL audio clip pitch import →
REA_rodada164e-clippitch.md (misc164p). Evidence markers: [CODE] LYNX source; [TEST] tests/Misc164Tests.inc
(build-local); [LIN] Linux build in WSL (WSLg), screenshots; [WIN] Windows build.

## 1. Browser: "Show folders..." window
- Before [CODE]: *Show folders* was a checklist sub-menu (one click per folder, the menu closes each time); a folder
  the user added could not be taken out again (only hidden).
- Now: the options button beside the search box (and the folder menu's *Browser* sub-menu) › *Show folders...* opens
  a window per Browser (main / Sample browser, key `browser-folders` / `sample-browser-folders`): every entry with a
  tick (applies at once; the whole row is clickable), its path under the name, *Current project* and *Plugin
  database* first; the user's own folders say "(your folder)" and have an × (*Remove this folder from the Browsers*,
  the folder stays on disk); *Add folder...*, *Show all*, *Close*. Under a content filter the window says that folders
  with nothing of that kind are left out and marks each root that has nothing to list. A user folder can also be
  removed from its right-click menu. The window follows changes made elsewhere (right-click › Hide, filter).
- Snapshot `-browser-folders.png` [LIN].

## 2. The content filter leaves out folders with nothing to list
- Before [CODE]: with *Show: Samples and SoundFonts* (or Audio / Presets / Projects) every folder was still listed,
  many of them empty once opened.
- Now: a folder (and a top-level folder) whose tree holds no file of that kind is left out. The answer comes from the
  Browser's search index (round 162: it already lists every folder's usable files), never from the disk while
  drawing: `search::Index::folderMatches` marks every indexed folder that holds a matching file at any depth (and its
  parents), and the folders of completely walked trees that hold none. `search::Service::folderMatches` caches it per
  filter until the index changes (`Index::generation`), and only `try_lock`s the index: while the background worker
  holds it, the last answer is used — no UI stall. A folder the index doesn't know yet (never indexed, or its tree not
  walked to the end) stays listed. When the index changes (start-up pass, refresh, a search's re-check), the open tree
  drops the folders found empty, keeping what is open and selected (`FileTree::pruneFolders`).
- Cost: one pass over the index per filter and index change (string extension checks; tens of thousands of files are
  a few ms), on the message thread only when the filter or index changed.
- [TEST] `misc164_browser_filter_hides_empty_folders` (unknown before the tree is complete; samples keep the parents of
  a deep SoundFont; presets / projects; a file arriving changes the generation and the answer; Windows drive roots).
- Snapshots `-browser164-all.png`, `-browser164-samples.png`, `-browser164-projects.png` (a small library made by the
  snapshot: Drums/Kicks, Keys, Presets/Synth, Projects/MIDI, Notes, Empty/Nothing).

## 3. Mixer: the selected effect's knobs no longer cut by the EQ
- Before [CODE]: `EffectsPanel::neededHeight` counted the slots and the effect's title row only, so at medium heights
  (e.g. 680 px, UI zoom 150 %) the EQ stayed open and the clip region cut the knobs' names and values.
- Now the panel's needed height includes the selected effect's controls (knob grid at its shortest rows, the volume
  gate's steps, the vocoder's band levels, LYNX Tune's keys, the parametric / Facet EQ curve, the plugin button,
  the parallel container's branches): when they and the open EQ don't both fit, the EQ folds; when even that
  doesn't fit, the panel scrolls (ui163's rule). Selecting another effect refits it (EffectsPanel timer, only when
  the slot or its kind changed).
- [LIN] `-mixer163-450/560/680/820.png`: 680 now folds the EQ with Ceiling / Gain / Release names and values whole;
  820 keeps the EQ open below them; 450 scrolls.

## 4. Fixed-size windows taller than the screen
- Before [CODE]: ui163's `fitWindowOnScreen` shrinks resizable windows; a fixed-size one (every tool window: Settings,
  New project, Clip properties, Plugin manager, Shortcuts...; Export, Customize toolbar, crash report dialogs) only
  moved so its title bar is on the screen — its bottom (OK / Accept buttons) stayed below the screen.
- Now `fitContentToScreen` (ui/WindowOwner) puts such a content into a scrolling viewport the size of the usable
  screen minus the window frame (the window's own frame once on screen, else another LYNX window's, else 32/8 px),
  and follows the content's later size changes; content that fits is untouched (no wrapper). Used by
  `openToolWindow` and the three fixed-size `DialogWindow`s. Size rule `windowfit::scrollFit` (core/WindowFit.h).
- [TEST] `misc164_fixed_window_scrolls_when_taller_than_the_screen`. `--fittest` got a step for it (a 520 x
  screen+300 px tool window must fit whole, its content in a viewport); `LYNX_FITTEST_MS` keeps each window longer
  for screenshots.

## 5. Linux check of ui163's window fitting [LIN]
- WSLg (Weston + XWayland), 1920x1080, `LYNX --fittest` at 100 / 150 / 200 %, screenshots taken from Windows
  (D:\lynx-work\misc164\lin-fit-*.png): the tall tool window's title bar and close button are on the screen, the
  fixed-size tool window fits with a scroll bar, the resizable window shrinks after the zoom change.
- Finding: Weston reports its invisible drop shadow in the frame size (`_NET_FRAME_EXTENTS`: 59 px top, 38 px on
  each side at 100 %), so the old `--fittest` check said FAIL while the windows were visibly fine. The check now
  subtracts such a shadow on Linux (a frame side ≥ 16 px) and allows 4 px. The fit itself treats the shadow as frame,
  which only makes windows a little smaller than necessary there.

## Where I stopped
- (updated at landing)
