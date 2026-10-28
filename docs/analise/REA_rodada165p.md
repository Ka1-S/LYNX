# Round 165p "issue12" — GitHub issue #12 (BURNER1PA, Windows 10, portable 0.1.18)

## Where I stopped
- 2026-10-28 12:45 (resumed after the 2nd BSOD): batch 1 = feature/issue12 (../lynx-wt-issue12), batch 2 =
  feature/issue12b (../lynx-wt-issue12b, has issue12 merged). Neither landed yet.
- Batch 1 Windows build (build-win-issue12, c0333184) checked on Windows: F10/F11 with the Playlist focused, Metronome
  on 'C' (Playlist, Channel Rack, Piano Roll), Options › Theme. Sample-cycling stress running (fixed build, 5 min).
- Added core/SortOnce.h + test issue12_sort_reads_each_changing_key_once (the crash's root cause in a JUCE-free test).
- Next: build-local (tmp/coord/bl-issue12.log) → land batch 1 → build/verify/land batch 2 → reply draft + notes.
- Windows helpers: C:\Users\kauan\lynx-test\issue12\ (launch.ps1 -keep 1 keeps a seeded profile, drive.ps1 = audit165's
  real-input driver, close.ps1, winshot.ps1, fg.ps1, hwnd.ps1). Test samples: D:\lynx-work\issue12\samples (1500 WAVs).

- 12:45-12:51 incident: I launched LYNX while audit165 held the wingui lock (my lock check was shadowed by a shell
  function named ps); my clicks hit their LYNX: one opened its Recording dialog (cancelled again), the Cancel click may
  have selected their Mixer insert 10. Closed my instance at once; lock helper fixed.
- 13:05: 0.1.18 repro build queued: ../lynx-wt-issue12-v018 (v0.1.18-alpha + LYNX_PROFILE_DIR in Paths.cpp only, never
  released), OUT=build-win-v018, to check "Theme opens MIDI" on the user's version without touching the real profile.

## Items
| item | reproduced? | root cause | fix | test | Windows check |
|---|---|---|---|---|---|
| Bug: Metronome on 'C' does nothing | yes (code + user's steps): the focused Playlist / Piano Roll took 'C' for its Knife before the key map | panels handled their built-in letters before user keys | user keys win over the panels' keys; the tools are key-map actions (7b1042de) | issue12_user_key_wins_over_a_panel_default, issue12_every_action_takes_a_user_key | Options › Manage shortcuts › Metronome → C ("your key wins there now"); C toggles the metronome with the Playlist, Channel Rack and Piano Roll focused (shots c-tb.png, c-tb2.png) |
| F-keys / numpad typed as letters (r165q's find, #11 "F10/F11 work") | yes on main (F4 = 'S' Slip, F1 = 'P') | JUCE's Windows key codes for F-keys/numpad are VK\|0x10000; the editors' towupper() turned them into letters | keys::letterOfKeyCode (7b1042de) | issue12_function_keys_are_not_letters | F10 → Settings on MIDI, F11 → Project info, Playlist focused (b-f10.png, b-f11.png) |
| Bug (round 165q's find, likely part of the 'C' report): shortcuts dead while a tool window has the focus (Undo history, Settings, Tempo tapper, Quick-add, Audio editor, plugin / channel windows) | reported by audit165 with real input (Ctrl+H then Space / F12) | ToolWindow only knew Esc; no window forwarded unused keys to the key map | keys a secondary window leaves unused run the global actions: a KeyListener installed by keepAboveMainWindow → MainComponent::keyFromSecondaryWindow → KeyMap::findFromSecondaryWindow (global actions only; a focused TextEditor keeps its typing keys; detached panels keep their own full forwarding) (90ec18f4) | issue12_secondary_windows_run_the_global_shortcuts | pending |
| Bug: Options › Theme opens MIDI | yes on 0.1.18/0.1.19 by code: openSettings clamped the page with jlimit (0, 5) and Theme = 6 → MIDI | page clamp | already fixed on main by r165a's 8a723472 (merged 94e25da3), not by this round | — | main ea719caf: 17 tries with real clicks open Theme, also with Settings already on MIDI (pm13-tabs.png). 2 early tries opened nothing: driver/focus artefact, not reproducible in 15 later tries (see below) |
| Bug: song-mode automation recording cuts itself | by test | the take's clip ended at the song end / loop wrap | linear playback while recording a take + the take's clips grow to the playhead (7b1042de) | issue12_song_mode_automation_take_grows_past_the_song_end | pending |
| Bug: pitch bend from a controller not recorded | by code | the wheel only played, never reached the take recorder | wheel moves queued with their tick, written to the channel's pitch bend target; plays on LYNX instruments (7b1042de) | issue12_recorded_pitch_bend_plays_on_a_lynx_instrument | pending (no controller; MIDI injection) |
| CRASH: fast cycling through samples | user's crash report d3b378e5 (0.1.18, his PC): access violation in std::__introsort ← juce::File::getLastAccessTime ← WaveformView pruneDiskCache. GUI stress on main (20 keys/s, 3 min, 620-file seeded cache) did not crash: the race needs two prunes at once on a disk with access-time updates | comparator reading access times that change during the sort (UB) + concurrent prunes deleting files | times read once (FileSort.h / core/SortOnce.h), one prune at a time, prune every 16 writes (c475852e + this round) | issue12_sort_reads_each_changing_key_once | fixed build: stress running |
| CRASH: ASIO control panel (user's report d1f26cca, 0.1.18, LYNX ASIO) | report | closed the device, then called the deleted device's showControlPanel | JUCE's order: panel first, reopen only when it changed (c475852e) | — | pending |

### Options › Theme on main (point (a) of the coordinator)
Real input (mouse_event with real coordinates, keybd_event with scan codes), fresh LYNX_PROFILE_DIR + LYNX_TEST_MODE=1,
main ea719caf = C:\Users\kauan\lynx-test\issue12\main\LYNX.exe:
- 15 of 17 tries opened Settings on the Theme tab: right after launch, after other menus, with Windows Terminal or the
  taskbar holding the focus before the click, and with Settings already open on MIDI (menu or F10) — it switches to Theme.
- The 2 misses were both the very first menu use of a fresh launch whose window had not taken the foreground
  (Windows Terminal kept it); the same sequence then worked in 6 more fresh launches. No code path drops the page; I
  count them as input artefacts, same family as audit165's first driver.
- 0.1.18 / 0.1.19 (the user's version): `showSettings (engine, (SettingsPage) juce::jlimit (0, 5, page), …)` with
  SettingsPage::theme == 6 → MIDI, exactly the report. Fixed on main by 8a723472 (round 165a fl165). The 0.1.18
  exe was not run: it has no LYNX_PROFILE_DIR / test mode and would use the user's real profile.

### Features of batch 1, checked on Windows (build c0333184, real input, fresh profile)
| feature | Windows check |
|---|---|
| Click a mixer track's VU meter selects it | click on Insert 6's meter: selected, Effects panel "Insert 6", hint "[LMB] selects the track and clears the clip light" (f2.png) |
| Double-click the header of the mixer insert's right-click menu renames | opens "Rename track"; typed "drums" → strip renamed (f6c.png). (The name field doesn't take the focus by itself: audit165's F2-dialog focus fix covers that dialog.) |
| Same for a pattern's right-click menu | double-click on the "PATTERN / Pattern 1" header opens "Rename pattern" (f8.png) |
| Channel Rack pattern length (bars) box | hint "Pattern length in bars: drag or wheel, double-click automatic"; wheel 1 → 4 bars, the grid grows (f11.png) |
| Channel Rack follow playhead | on, 4-bar pattern playing: the step view pages to bar 2 (fpc.png) |
