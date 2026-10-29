# Round 165s "issue11b": issue #11, comment 4 (Jocho-Smith, Fedora 44 GNOME, 0.1.18 tar.gz)

Three new points: (1) Shift+Space while playing, (2) plugin editors don't move with their automation, (3) drag & drop
of audio files into the Playlist (and the Channel Rack) from the file manager doesn't work.

## Where I stopped

- 02:30 item 1 done in code (FL checked, `transport.playFromStart` = Shift+Space, test, 14 languages). Item 3: the
  JUCE fix (cmake/LynxPatchJuce.cmake) is in; the bug is reproduced on the real GNOME 50 path (mutter 50 headless +
  Xwayland + a GTK 4 Wayland drag source, tmp/issue11b/gnome-*.sh). A SECOND bug: even a plain XDND v3 drop is ignored
  by the Playlist (the Channel Rack takes it); debugging with temporary `DBG11b` stderr lines in PlaylistComponent.cpp
  (remove before landing). Item 2 waits for issue12c (its landing failed 01:32, zig cache error).

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
