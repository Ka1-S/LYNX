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
