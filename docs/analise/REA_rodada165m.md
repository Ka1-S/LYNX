# REA round 165m — Performance mode + Patcherize (perfmode165)

Branch feature/perfmode165 (wip: origin/wip/perfmode165). Goal: FL's Performance mode (clip launching, track
Performance settings, trigger / position sync, controller launching, recording a performance, .flp import) and
Patcherize / Patcherize all (null before/after). Clean-room: FL behaviour from REA/Ghidra on FLEngine_x64.dll, FL's
menus/DFM, the demo projects and FL running; no copied code or graphics.

## Where I stopped
- 2026-10-27 21:10: core + UI built (Linux GUI, core tests 0 failures for perf165), merged origin/main, landing batch 1:
  Performance mode (zone before the Start marker, launching by click / column / MIDI channel, every track setting,
  recording the passes as clips, .flp import of the settings and of the project flag), Patcherize / Patcherize all.
- Left: FL's "Prepare for performance mode" macro; the Performance settings apply to the clicked track (FL: every
  selected track); FL's modifier keys 125–127 on the MIDI channel (meaning not decoded); a controller-script layer
  (Launchpad / APC grids) beyond the generic note mapping.

## 1. Evidence

### 1.1 Menus [DFM] (TFruityLoopsMainForm)
- Playlist › Options › **Performance mode** (`EELiveModeMenu`, ShortCut 16464 = Ctrl+P).
- Track header › **Performance settings** (`EEPLTrackLiveMenu`), radio groups with Tag = stored value:
  - Press: Retrigger 0, Hold & stop 1, Hold & motion 2, Latch 3.
  - Motion: Stay 0, One shot 1, March & wrap 2, March & stay 3, March & stop 4, Random 5, Exclusive random 6.
  - Trigger sync: Off 0, 1/4 beat 1, 1/2 beat 2, 1 beat 3, 2 beats 4, 4 beats 5, Auto 6; checkboxes Queue, Tolerant.
  - Position sync: Off 0, 1/4 beat 1, 1/2 beat 2, 1 beat 3, 2 beats 4, 4 beats 5, Auto 6.
- View: Performance clip progress, Performance track progress; Zoom on performance clips; Tools › Macros ›
  Prepare for performance mode.
- Channel menu › Patcherize ("Convert the channel to Patcher with the current plugin loaded"); mixer slot menu ›
  Patcherize / Patcherize all.

### 1.2 .flp [DATA] (Playlist track record 0xEE, demo projects in Data/Demo projects/Performance mode)
| byte | field |
|---|---|
| 22 | u32 motion (0…6 as above) |
| 26 | u32 press (0…3) |
| 30 | u32 trigger sync (0…6) |
| 34 | u32 queue (bool) |
| 38 | u32 tolerant (bool) |
| 42 | u32 position sync (0…6) |
FL's default track: motion Stay, press Retrigger, trigger sync 4 beats, tolerant on, position sync Off.
- The performance zone: every demo project has a "Start" marker (marker type 5) after the clips meant for launching;
  plain markers mark the columns. Charles Deluxe: Start at FL tick 1920 (ppq 96) → LYNX 19200 (ppq 960).

### 1.3 Strings [STRINGS] (FLEngine_x64.dll)
- "Can't record in the playlist while performance mode is active." / "in a live performance situation (no undo)."
- "Select what to merge|The source arrangement has both performance and song data".
- "MIDI channel used for playlist live mode" (Settings › MIDI input, `PLLiveChanSelect`, 0 = off, 1…16).
- "Patcherizing will lose automation links" / "When patcherizing a plugin, all automation links to that plugin will
  be lost." — LYNX keeps them: the moved plugin's targets become the Patcher's published parameters.
- Undo names: "set performance settings", "patcherize channel", "patcherize master mixer track", "patcherize current
  mixer insert", "patcherize mixer insert ", "patcherize effect slot ".
- Templates Data/System/Patcherize (Effect).fst / (Generator).fst: an empty Patcher with the Audio input / Notes /
  Audio output nodes — the plugin is cabled between them.

### 1.4 Decompiled [DECOMP] (Ghidra, project /mnt/d/lynx-work/rea165)
- `EELiveModeMenuClick` 0x122d810: toggles the global performance flag, refreshes the record-filter radio items and,
  when turning on, walks the playlist tracks and stops what the track was doing (FUN_012c1910), then repaints.
- `EEPLLiveLoop1MenuClick` 0x1212fe0 → FUN_012ead80: one handler for every Performance settings item; the item's Tag
  and group index (5 press, 10 motion, 15 trigger sync, 20 position sync) select the field; it is applied to every
  selected track under one undo step "set performance settings" (LYNX: the clicked track, undoable).
- Scheduler FUN_00d0bde0 (per block): pass 1 — clips whose stop time has come end (FUN_00d274e0 writes the finished
  pass to the playlist when the record filter's "Clips" bit 8 is on; FUN_00d273f0 swaps it out of the active list);
  pass 2 — pending launches whose start time has come begin, the motion picks the next clip (FUN_0100bbd0), the
  clip's (track << 16 | index) resolves through FUN_0100bb70, and with position sync the start offset is the song
  position modulo the clip length (FUN_010649c0 / FUN_010646d0, at least 1 tick long).
- `PatcherizeMenuClick` 0x11f9050 → FUN_011f8ee0 (the "lose automation links" confirmation) → FUN_011f9650 (moves
  the slot / all slots). `ChanPatcherizeMenuClick` 0x11f59b0 → same confirmation → FUN_01189ba0 (loads the generator
  template with the channel's plugin).

- MIDI "playlist live mode" channel (Settings › MIDI, `PLLiveChanSelect` field 0xa70 → global 0x15c26ac, read in the
  MIDI input handler at 0x1181be1): when the message's channel matches, Performance mode is on, the transport plays
  in song mode: notes 120 / 121 = track offset −/+ (1, or 10 with the big-step toggle 124, clamped 0…500), 122 / 123
  = column offset −/+ (1 or 12); 125–127 held set modifier bits 0x80 / 0x180 / 0x8 on the launches; any other note →
  launch (FUN_01221c00) of track note / 12 + offset, column note % 12 + offset, velocity 0 = release (flag 0x20).
- .flp: event 0x1F (byte) = Performance mode on — 1 in all six performance demos, 0 in all 20 demo songs.

### 1.5 FL running [FL] (2026-10-27, Charles Deluxe demo, flref in tmp/perfmode165)
- The project opens in Performance mode with the view on the zone (markers named Pad #1…#5); the zone's idle clips are
  dimmed, the launched ones bright with a ▸ mark at the left edge (× on the others); while playing the Playlist title
  reads "Performance" and the launched clip's played part is lighter (its progress).

## 2. LYNX (what landed)
- core/Performance.{h,cpp}: the Player (audio thread, lock-free commands from the UI and from the MIDI thread), every
  press / motion / trigger sync / Queue / Tolerant / position sync; Tolerant = a launch up to 1/8 of the sync step late
  starts at once as if on the grid point (LYNX's window; FL's exact one not decoded); stops are quantized to the
  trigger sync grid; launches while stopped are dropped (FL launches only while playing).
- Scheduler: the zone's clips don't play with the song; the song (and its loop) starts at the Start marker.
- Recording (record filter "Clips", FL's bit 8): each pass becomes a clip copy on its track at the song position.
- Playlist: click = launch (held for the hold modes), right-click = stop the track, ruler menu: Start marker, launch a
  column, stop all; View: clip / track progress; Zoom on performance clips; Ctrl+P.
- core/Patcherize.{h,cpp}: channel → generator Patcher (Notes → plugin → To LYNX), insert slot / all slots → effect
  Patcher in series (side-chain through the Patcher's side-chain pin); null tests (channel max diff 3e-8, slots 0);
  automation and links follow to the published parameters (FL loses them).
