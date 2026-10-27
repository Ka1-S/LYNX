# REA round 165m — Performance mode + Patcherize (perfmode165)

Branch feature/perfmode165 (wip: origin/wip/perfmode165). Goal: FL's Performance mode (clip launching, track
Performance settings, trigger / position sync, controller launching, recording a performance, .flp import) and
Patcherize / Patcherize all (null before/after). Clean-room: FL behaviour from REA/Ghidra on FLEngine_x64.dll, FL's
menus/DFM, the demo projects and FL running; no copied code or graphics.

## Where I stopped
- 2026-10-27 16:50: worktree created from main 5ed718d1; studying FL (DFM menus, demo .flp 0xEE records, REA next).

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
