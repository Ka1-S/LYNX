# Round 163a (ui163) — UI zoom 150%, compact menus, Alt = no snap

User feedback (2026-10-26, Windows, UI zoom 150%): windows "lost the ability to exit out", the Mixer's EQ sits on top
of the effect slots when the window is short, menus waste vertical space, Alt-drag in the Playlist should be free
(no snap).

## 1. UI zoom 150% (and 125/175/200%)
- [CODE] LYNX zooms with `juce::Desktop::setGlobalScaleFactor` (MainComponent::setToolbarLayout). JUCE keeps each
  window's logical bounds, so at a higher zoom the windows grow in physical pixels: a 1600x900 main window becomes
  2400x1350 at 150% on a 1920x1080 screen — its right end, with the close button, is off the screen. Tool / plugin /
  editor windows centred on the main window (`setCentrePosition`, `centreWithSize`) that are taller than the
  screen get a negative top: the native title bar is above the screen (only Esc closes them).
- Fix: `fitWindowOnScreen` (ui/WindowOwner.cpp) shrinks a resizable window to its screen's work area (frame
  included, respecting the resize limits) and moves it so the title bar and close button are on the screen
  (rule in core/WindowFit.h, test `ui163_window_fit`). `keepAboveMainWindow` (called by every secondary window
  when shown) applies it once; the main window applies it when revealed; a UI-scale change applies it to every
  LYNX window 200 ms later.
- Mixer: the effects panel (EffectsPanel) put its slots at fixed positions from the top and the EQ + output at
  fixed positions from the bottom, so a short mixer (zoom 150% = 2/3 of the logical height) drew the EQ over the
  slots. Now the EQ has a title bar (chevron + "EQ"); when the slots and the EQ don't both fit, the EQ folds to that
  bar by itself (a folded EQ that still changes the sound shows "active"); clicking the bar folds / unfolds it by
  hand; when even the folded layout doesn't fit, the panel sits in a viewport and scrolls. Snapshot
  `-mixer163-<h>.png` at 450/560/680/820 px.

## 2. Compact menus
- [CODE] `LookAndFeel::getIdealPopupMenuItemSize` returned 26 px rows; JUCE asks the section-header height with
  the same call (standard height −1) and adds half → 39 px headers; separators 9 px. The screenshot showed that.
- Now (default "Compact"): 21 px rows, 19 px headers, 5 px separators, tighter widths (Windows 10/11 native menus
  and FL are ~20–22 px at 100%). Options › General › "Menu density": Compact / Comfortable (the old sizes), saved
  as `menuDensity` in the UI options. Snapshot `-menu163-compact.png` / `-menu163-comfortable.png`.
- Settings pages: rows 28→26 px, gaps 8→5 px, section titles 28→24 px.

## 3. Alt = no snap while dragging
- [CODE] Already: Playlist clip move / resize / trim / slip / fades / paint / knife / time selection / time-move,
  Piano Roll note move / resize / paint / knife / stretch / drum brush / ruler select, automation points.
- Missing (fixed): scrubbing the song position on the Playlist ruler (and the Listen tool) always snapped; the
  Piano Roll ruler had no click-to-position at all (FL: clicking the Piano Roll timeline moves the position).
  Now both rulers follow the mouse, Alt = off the grid; Piano Roll in Song mode lands in the clip of the pattern
  that plays (or its first clip) — `transportedit::patternSeekTarget`, test `ui163_piano_roll_ruler_seek`.
- Hints: rulers and the seek / time-selection drags mention "[Alt] no snap".

## Verification
- build-local with FL Data: 1753133 checks, 0 failures (tests `ui163_window_fit`, `ui163_piano_roll_ruler_seek`).
- Windows `--snapshot` at 1280x700 (= a 1920x1050 work area at 150%): `-menu163-compact/comfortable.png`,
  `-mixer163-450/560/680/820.png` (450: panel scrolls; 560: EQ folded, output row kept; 680+: EQ open),
  `-settings-general.png`. Copies in D:\lynx-work\ui163\snap.
- Windows `LYNX.exe --fittest report.txt <percent>` on the real desktop (1920x1080): at 125/150/175/200% a centred
  tool window taller than the screen lands with its title bar at the top of the work area (y=0, close button in),
  and a 90%-of-screen resizable window shrinks to the work area after the zoom goes up. All OK.
- Not done: a Linux GUI pass (the fit rule is platform-neutral; on Linux a missing frame size counts as none).

## Where I stopped
- Batch 1 (all three items) handed to land.sh; notes in tmp/coord/notes-0.1.18.md (New: Compact menus, Alt on the
  rulers; Fixes: windows reachable at 150%+, Mixer EQ folds).
