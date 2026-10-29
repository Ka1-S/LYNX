# Round 165s "issue11b": issue #11, comment 4 (Jocho-Smith, Fedora 44 GNOME, 0.1.18 tar.gz)

Three new points: (1) Shift+Space while playing, (2) plugin editors don't move with their automation, (3) drag & drop
of audio files into the Playlist (and the Channel Rack) from the file manager doesn't work.

## Where I stopped

- 03:10 item 1 done in code. Item 3: four JUCE fixes in cmake/LynxPatchJuce.cmake (CMake builds patch the fetched
  JUCE; scripts/build-windows-zig.sh compiles a patched copy of juce_gui_basics). Item 2: feature/issue12c (not landed
  yet) merged into this branch; core/ShownEffectSlot.h used by the native effect editors (Parametric EQ 2 and every
  FL-style layout) and Facet EQ. Next: Linux build + tests, GUI checks (Linux, Windows), land.

## 2. Effect editors following their automation

Issue #12's follow-up (round 165p, issue12c) made knobs, faders and LYNX's pro editors show the automated value while
the song plays (core/AutomationDisplay, UI-side evaluation). The native effect windows (Parametric EQ 2's band handles
and curve, Fruity Filter, Fruity Delay, Reeverb and every other layout drawn by NativeEffectEditor) and Facet EQ read
their slot straight from the Song, so they stayed still. Now `current()` returns the slot through
core/ShownEffectSlot.h: a cached copy with the automated `effectParam`s overlaid (the same values the renderer uses),
or the Song's slot when nothing is automated, the transport is stopped or a mouse button is down (the hand wins).
Every edit still goes through setParams on the Song. The editor's timer repaints when the shown values change.
Third-party plugins get automation through their own parameters, so their own editors already move.

## 3. Drag & drop from the file manager (Linux)

Evidence [CODE]:
- mutter 50 `src/wayland/meta-xwayland-dnd.c` (`xdnd_send_enter`) carries a Wayland drag into an X11 (XWayland)
  window with `XDND_VERSION 5` in XdndEnter, ignoring the version the target announced in XdndAware.
- JUCE 8.0.4 `juce_DragAndDrop_linux.cpp` `handleDragAndDropEnter` drops every XdndEnter above its own version 3
  (`dndCurrentVersion > DndVersion` → `dragAndDropSourceWindow = 0`), so no XdndStatus is sent and the drop is refused.
  Still the same on JUCE's develop branch (2026-10).
- GTK 4.22 `gdk/gdkcontentserializer.c` registers `text/plain;charset=utf-8` before `text/uri-list` for file lists,
  so Nautilus 50 offers the plain-text type first; JUCE picks the first offered type it knows and would deliver the
  paths as a *text* drag (no LYNX drop target takes text).
- LYNX's own Browser drags never go through X11 (JUCE DragAndDropContainer), which is why those worked.
- A stale `finishAfterDropDataReceived` after a refused drag made the next drag "finish" at XdndEnter (reproduced
  with an XDND emulator, tmp/issue11b/xdndsrc.c: a v5 drag, then a v3 drop → nothing).
- All platforms: `ComponentPeer::handleDragDrop` keeps `lastDragAndDropCompUnderMouse` when no component took the drop,
  so the next drag starting over the same component skips the target search (reproduced: an unsupported file dropped
  on the Playlist, then every following Playlist drop failed).
- Real GNOME 50 path (mutter 50.1 headless + Xwayland + a GTK 4 Wayland drag source, private prefix): mutter 50.1
  never sends any Xdnd message to an X11 window (gdb on XSendEvent; a minimal logging X target gets nothing), while
  Wayland→Wayland drags work. mutter 51.1's NEWS: "Fix drag&drop between Wayland and Xwayland clients (!5362)". So on
  GNOME 50 the compositor drops the drag before LYNX; with the fixes above LYNX takes it once the bridge delivers it
  (mutter 51.1+, KWin). Workaround: add the folder to LYNX's Browser and drag from there.

Fix: cmake/LynxPatchJuce.cmake, four idempotent source fixes (XDND 3..5, text/uri-list first, reset on enter, forget
the component under the mouse after a drop). Verified with the emulator on Xvfb: GNOME-style v5 drag with GTK 4's type
order and a percent-encoded path with a space lands as a clip on the Playlist and as a sample in the Channel Rack.

## 1. Shift+Space (FL 2026, checked in FL)

FL 2026 under the FL lock, demo "Asher Postman - Future House", keys sent with SendKeys, position read from FL's time
display every few hundred ms (shots in C:\Users\kauan\lynx-test\issue11b\s2..s5) [SHOT]:

| mode / window | start | action | FL result |
|---|---|---|---|
| Song, Playlist | 1:01 (bar 1) | Space, then Shift+Space while playing | stops, position 1:01 |
| Song, Playlist | 5:01 (ruler clicked at bar 5) | Space → 6:10, Shift+Space | stops at **1:01** (not 5:01) |
| Song, stopped at 1:01 | | Shift+Space | plays from 1:01 |
| | | then Space | stops, back to 1:01 (where that play started) |
| Pattern, Piano Roll | 1:09 (Piano Roll ruler clicked) | Space → 2:02, Shift+Space | stops at **1:01** |
| Pattern, stopped | | Shift+Space | plays from 1:01 |

So FL's Shift+Space is "play / stop from the start of the song or pattern": it ignores the position set on a ruler
and where the playback started (Space goes back there). Not in FL's online shortcut list. LYNX had nothing on
Shift+Space. Now: action `transport.playFromStart` (Options › Manage shortcuts), default Shift+Space, global (works with
the Playlist, Piano Roll, Channel Rack, Mixer, Browser or a plugin window focused); `MainComponent::playFromStart`.
