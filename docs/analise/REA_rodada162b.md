# Round 162b — transport and editing items of issue #9

Branch `feature/transport162`. Issue #9 (BURNER1PA, Windows 10, LYNX 0.1.17): the transport / editing requests.
Evidence markers: [EXEC] run here, [CODE] read in our source, [FL] FL Studio behaviour as a user sees it (manual /
the app), interpreted clean-room.

## What was already there
- [CODE] The Playlist always paged along with the playhead in Song mode (`PlaylistComponent::timerCallback`), with no
  way to turn it off.
- [CODE] The Piano Roll drew a playhead, but only in Pattern mode (`PianoRollComponent::timerCallback`), and never
  scrolled with it.
- [CODE] Stop already returns to the start (or, with "Remember seek time", to the position clicked on the Playlist
  ruler); its double click was "all notes off".
- [CODE] Quick quantize (Ctrl+Q) and quantize start times (Shift+Q) existed in the Piano Roll, hard-wired in
  `handleKey`, so they could not be reassigned in the shortcut editor. The Playlist had no quantize.
- [CODE] The Channel Rack title showed "Channel Rack - <pattern>" (Shift+wheel on it stepped the pattern) but had no
  selector to click.
- [CODE] The Mixer meters had no dB numbers.

## What changed
1. **Follow (auto scrolling)** [FL: View › "Auto scrolling" / the editors' scroll-follow]. `EditorGlobals` keeps
   `followPlaylist` / `followPianoRoll` (default on, saved in ui-options.txt as `followPlaylist`, `followPianoRoll`).
   A Follow button (new `Icon::follow`) in the Playlist and Piano Roll toolbars, items in View and in each window's
   menu, key-map actions `playlist.follow` / `pianoRoll.follow` (no default key, assignable). The page jump is
   `transportedit::followViewStart` (core, tested).
2. **Piano Roll playhead in Song mode**: `transportedit::patternPlayhead` gives the position inside the edited pattern
   in Pattern mode (loop) and in Song mode while the song position is inside a clip of that pattern (clip offset
   counted, the uppermost track wins, the song loops). Shift+0 "Centre on the playhead" uses it too.
3. **Double Stop → start**: [FL] Stop returns to the start; pressing Stop again while stopped goes to the very
   start. LYNX now does the same: Stop while nothing plays (the second press of a double click) goes to bar 1 / the
   pattern start even with "Remember seek time" on; a double click also sends all notes off. "Back to the start" in
   Stop's right-click menu. Logic in `transportedit::stopTarget` (tested).
4. **Channel Rack pattern selector**: in the rack's title bar, `< Pattern name ▾ >` with the pattern's colour: the
   name opens the list of patterns (+ New pattern, Rename and color), the arrows go to the previous / next pattern,
   the wheel steps. The title is now just "Channel Rack".
5. **Quantize 50% and Quantize starts as shortcuts**: key-map actions `pianoRoll.quickQuantize` (Ctrl+Q),
   `pianoRoll.quantizeStarts` (Shift+Q), `pianoRoll.quantizeHalf` (unbound), `playlist.quantizeStarts` (Ctrl+Q, FL's
   Playlist quick quantize) and `playlist.quantizeHalf` (unbound); all in Options › Shortcuts. 50% moves each start
   halfway to the nearest grid line (again: halfway again), lengths kept; selection, or everything without one.
   Math: `transportedit::quantizeTick` (tested).
6. **Master dB scale**: +3, 0, -3, -6, -12, -18, -24, -36, -48 beside the master meter (scale -60..+6 dB like the
   meter), with small ticks; `meters::scaleMarks` keeps only values whose labels don't overlap at the meter's
   height, so short mixers show 0 / -12 / -24 and tall ones all. Inserts: their 60 px strips have no room for
   numbers without squeezing the fader, so only the master got them.

Tests: tests/Transport162Tests.inc (quantize strength, notes and clips, Song-mode playhead, follow, stop, scale).

## Where I stopped
See the final section (updated when landing).
