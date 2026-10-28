# Round 165q — audit165: independent verification of the #4, #9, #11 (and #8) fixes on Windows

Adversarial check, not the author's: every claim we posted in the replies to BURNER1PA (#4, #9), the #11 draft
(tmp/coord/replies-0.1.20.md) and the #8 reply is redone on the current origin/main Windows build, the way the user
works (real windows, mouse and keyboard), with a fresh `LYNX_PROFILE_DIR` + `LYNX_TEST_MODE=1`, under
`~/.cache/lynx-wingui.lock`. Verdicts: PASS / FAIL / PARTIAL, each with the screenshot that was looked at
(shots in /mnt/d/lynx-work/audit165/, not in git).

## Where I stopped
- 2026-10-28 13:05: session 5 done on the branch build: F2/F4/F6 verified with real input, --windowtest OK
  (16 checks incl. the name dialog). Theme FAIL withdrawn; F9 handed to r165p. Landing now; next session 6 = plugin
  window (#8 presets menu, Space in a plugin window, many plugins abuse).
- 2026-10-28 12:25: resumed after the 2nd BSOD (09:14). Removed the 0-byte / last-minute objects in
  build-win-audit165/app, Windows build of the branch re-queued (winbuild.sh, JOBS=3). Next: session 5 = verify
  F2/F4/F6 + Ctrl+A/H/Z with real scan codes + Theme steps for r165p; then land; then the remaining #9/#11/#8 claims.
- 2026-10-28 09:30: session 4 done (Sampler, export, Piano Roll, abuse). F4 + F6 committed (d3eae9c9). Next: verify
  F2/F4/F6 on the branch's Windows build + --windowtest, then land.
- 2026-10-28 09:10: sessions 1-3 done; F2 fix committed (05ab9e97), F4 cosmetic fix in progress; Windows build of
  the branch running. Earlier: 08:50 session 1-2 done on main's own Windows build (ea719caf, build-win/LYNX.exe). Results below.
  Next: Channel Rack / Sampler / Mixer / Piano Roll items, export overwrite, #11 items, abuse cases; then fix the FAILs
  (dialog text-field focus, Quick-add Ctrl+A) in feature/audit165. F-key/numpad shortcut bug handed to r165p (main).
- Tools: GUI driver C:\Users\kauan\lynx-test\audit165\drive.ps1 (SendInput-style mouse/keys, window z-order list,
  screenshots), bash helpers tmp/audit165/g.sh. Test profiles under C:\Users\kauan\lynx-test\audit165\prof\.

## Method
Windows 11 desktop 1920x1080, LYNX.exe portable, `LYNX_PROFILE_DIR=<fresh dir>` + `LYNX_TEST_MODE=1`, window placed at
0,0 1920x1040. Clicks and keys are real input events (mouse_event / keybd_event), so they go through the same Windows
focus and z-order rules the user hits. `wins` lists the process's top-level windows in z-order with the foreground
window, so "stays above" and "keeps the focus" are checked from Windows' point of view, not only from a screenshot.
The profile used for plugin tests got a copy of the user's plugins.xml (the list only; nothing rescanned).

## Results

| # | Claim (as posted) | Verdict | Evidence |
|---|---|---|---|
| 4 | Audio editor opens above the main window and stays there after clicking the main window | PASS | a3/a4: owner = main HWND, above it in z-order, foreground after opening |
| 4 | View › Audio editor | PASS | a2/a3 |
| 4 | Quick-add palette keeps its search box focused (typing goes in, also after clicking away and back, after 3 s) | PASS | a6/a7 |
| 4 | Plugin windows keep the focus: drop-down menus open and stay | PASS | b5: Pro-Q 4 (VST3 trial) "MIDI Learn" menu still open 3 s later |
| 4 | Song mode loops to the start at the song's end | PASS | loop1-12: 1:1 → 2:4 → 1:1 (clip ends in bar 2) |
| 4 | 2-bar empty pattern records and loops as 2 bars | PASS | pl1-14: 1:1 → 2:4 → 1:1 while recording in Pattern mode |
| 4 | Metronome volume (right-click the metronome) | PASS | c1 |
| 4 | Tempo tapper (View, Ctrl+F5; any key taps; Apply tempo) | PASS | c3/c4: 8 key taps → 115.57 BPM applied to the song |
| 4 | Browser preview volume knob | PASS | d5 (knob next to the search box) |
| 9 | Browser search over every folder no longer crashes 20-40 s later | PASS | d1/d2: "kick" over ALL, results in 1.5 s, alive after 40 s; + 15 s of fast edits, alive |
| 9 | Leaving the search box gives Space back to the transport | PASS | d4: click on the Playlist, Space plays |
| 9 | Browser Stop button; arrow keys walk and preview samples | PASS | d6/d7 (function only — the fast-cycling crash is r165p's #12 item) |
| 9 | Removing a plugin closes its window | PASS | b7: slot menu › Delete, Pro-Q 4 window gone within 1.5 s |
| 9 | Master meter dB scale | PASS | b2 |
| 11 | Right-click a mixer slot, then Delete removes the plugin | PASS | b6/b7 |
| 11 | Transport Stop also stops a Browser preview | PASS | d8/d9 |
| 4 | Channel Rack piano-roll view button | PASS | f2 |
| 4 | Gain knob in a sample channel's settings | PASS | f16 |
| 9 | Channel Rack pattern selector | PASS | f3-f6: list, New pattern, switch |
| 9 | Drag plugins from the Plugin database onto the Channel Rack | PASS | f21: LYNX › Synth dropped → new channel |
| 9 | Inserts past 16 | PASS | f17: channel TRACK knob → 57 |
| 9 | Follow on/off (Playlist) | PASS | h2: on, the view follows to bars 4-6; h6: off, the view stays on bars 1-3 at 4:4:1 |
| 9 | Double Stop goes back to the start | PASS | g0-g2 (single Stop already returns to the start / remembered position, per the posted tooltip) |
| 9 | Mixer › Change color works | PASS | i3 (Insert 1 purple); the colour wheel is in Rename and color (c11) |
| 9 | Options › Theme | PASS (first FAIL withdrawn) | s5_13/s5_15/s5_18: opens Settings › Theme cold, over Settings › MIDI, with the Mixer open and right after Mixer › Change color; i5 could not be reproduced with real input (same code) |
| 11 | Ghost steps after the pattern's end; clicking one lengthens the pattern | PASS | f7/f8 (16 → 32 steps) |
| 11 | Setting a step is silent | PASS | f8/f9: master meter dark right after the click (f10: it does light up while playing) |
| 11 | Drop a Browser sample into the Channel Rack | PASS | f15: new "DL Breaker" sampler channel (drop from the OS file manager not automated) |
| 11 | Double right-click-and-hold mutes a clip | PASS | j2/j3: clip veiled + strike line (the muted look) |
| 11 | Playlist vertical zoom with Alt+wheel | PASS | j0/j1 |
| 9 | Sampler fine tune reaches the audio | PASS | +53 cents on a 440 Hz sine → exported WAV peak 453.67 Hz (expected 453.63) |
| 9 | Export over an existing file | PASS | Windows "replace?" prompt, file rewritten, "Export complete" |
| 9 | Quantize 50% / Quantize starts in Piano Roll | PASS | menu entries with their shortcuts |
| 11 | Piano Roll keyboard: velocity from the click position | PASS | left edge soft, right edge loud (master meter) |
| 11 | FL window keys (Shift+F8, F10, F11, F12, Alt+F12, Ctrl+F5) | PARTIAL | with the main window focused: F10 → Settings › MIDI, F11 → Project info, Shift+F8 → Plugin database with its search focused (s5_19-s5_21), Ctrl+F5; but F-keys are swallowed while the Playlist / Piano Roll has the focus (F1) and every global key is dead while a LYNX tool window has the focus (F9) — both handed to r165p |
| 11 | Plugin database › Rename "1_EQ"; empty mixer slot, type 1 | PASS | s5_23-s5_28: rename dialog opens focused (F2 fix), "1_eq" listed first, empty Slot 1 menu + key 1 → EQ added |
| 11 | Playlist vertical zoom grip above the scroll bar | PASS | s5_29: dragging the grip changes the track height |
| 9 | Enter / Esc leave the Browser search box, Space plays | PASS | s5_30/s5_31: Esc (clears) and Enter (keeps the text) both give Space back to the transport |
| 9 | Drag plugins from the Plugin database onto the Mixer | PASS | s5_34: Delay dropped on Insert 3 → slot 1 |
| 9 | Piano Roll playhead in Song mode | PASS | s5_35: playhead shown inside the pattern clip's range, hidden outside |
| 9 | Audio Editor volume knob | PASS | s5_37 (knob next to Loop) |
| 4 | Tempo display right-click › Tempo tapper; Sync | PASS | s5_40/s5_41 |
| 8 | Last tweaked read-out on the hint bar, click → menu | PASS | s5_49/s5_50: "Insert 2 - Pan R8", menu with automation / remote control / value (note: Stereo separation is not an automation target, so it never shows there) |
| 8 | Plugin window Presets menu with ◀ ▶, save / load / export | PASS | s6_05-s6_08: Pro-Q 4 (VST3 trial): menu, "Save preset..." name dialog opens focused, "mine" listed under Your presets |
| 4 | Undo history Ctrl+H; Quick-add Ctrl+A / Ctrl+Z (real scan codes) | PASS | s5_10/s5_11/s5_51 — F3 stays withdrawn |

## Findings (not in the posted claims, found on the way)
- F10 (fixed here): closing an active third-party plugin window (its X; also F12 / Alt+F12 / removing the plugin)
  handed the Windows activation to ANOTHER program (here a terminal on the second screen) instead of LYNX, so the
  next Space or shortcut went to that program. Reproduced 2/2 with Pro-Q 4 (s6, `wins` shows fg = the terminal's
  HWND after the close); LYNX's own windows (Channel settings, Audio editor, tool windows) were fine. Fix:
  ~PluginWindow activates the main window first when it is the active window (activateMainWindow, WindowOwner).
- F9 addendum: Space in a focused plugin window (Pro-Q 4) does not reach the transport either (s6_09) — sent to r165p.
- F9 (handed to r165p, it is #12 bug 1's area): every global shortcut (Space, F-keys, Ctrl+…) is dead while a LYNX
  tool window has the keyboard focus (Undo history, Settings, Tempo tapper, Quick-add, Audio editor): ToolWindow::
  keyPressed only handles Esc and nothing forwards unhandled keys to MainComponent. Since #4 keeps the focus in those
  windows, the user hits it more often. Steps: Ctrl+H, then Space → no playback; F12 → nothing closes (s5_52).
- F8 (note): after recording in the Audio editor, Windows keeps showing LYNX's microphone-in-use icon (the input
  stays open); memory does not grow, nothing is recorded.
- Abuse (session 5, all survived): Audio editor opened, recording started and the window closed mid-recording 9x;
  60 key taps at 20 ms in the Tempo tapper (→ 522 BPM, FL's maximum) then playing at 522 BPM.
- Cleanup: Vfx.o / VfxSequencer.o were committed at the repo root (9329c953); removed, *.o ignored.
- F1 (handed to r165p, shortcuts): with the Playlist focused, F4 changes the Playlist tool instead of making the next
  empty pattern. PlaylistComponent::handleKey / PianoRollComponent::handleKey turn the key code into a letter with
  towupper, which truncates JUCE's Windows codes (`VK_F4 | 0x10000`) to 's' → the S tool. F1-F11 → P-Z, Numpad1-9 → A-I.
- F2 (fixed here, 05ab9e97): dialogs with a text field (Rename pattern, "Next empty pattern (and name it)", rename
  insert, preset names…) open with the field not focused: typing does nothing until the field is clicked, Enter
  presses OK. Cause: JUCE's AlertWindow::addButton gives every button explicit focus order 1, so when the dialog takes
  the focus, the traverser hands it to the first button. Fix: one Desktop focus listener (WindowOwner.cpp,
  installed by MainComponent) moves a dialog's first focus to its first editable text field; --windowtest checks it.
- F3: Ctrl+A did not select all in the Quick-add palette's search box — WITHDRAWN as a probable artefact of the first
  driver version: it sent key events with scan code 0, and JUCE rebuilds Ctrl+letter (WM_CHAR) from the scan code.
  The driver now sends real scan codes; Ctrl+letter results from sessions 1-3 (Ctrl+A, Ctrl+H, Ctrl+Z) are re-checked.
- F5 (minor, by design): clicking a built-in drum channel's name (Kick, Clap…) only selects it, while the hint bar says
  "[LMB] open/close the editor". They have no editor window (ChannelRackComponent::toggleEditor).
- F6 (fixed here): Esc did not close the Quick-add palette while its search box had the focus (the TextEditor eats
  Esc), though the palette's own hint says "Esc closes". search.onEscapeKey now closes the tool window.
- F7 (note, not investigated): double-clicking a channel name opened Channel settings, while the hint promises the
  Piano Roll.
- Abuse (session 4, all survived): Pattern/Song toggled 40x while playing; Ctrl+Z/Ctrl+Y spam while playing; 6 Pro-Q 4
  instances on the Master with windows opened while playing, 3 deleted (windows closed), undo/redo spam, Alt+F12.
- Not testable here: MIDI input light / pitch bend (no MIDI device or loopMIDI on this machine); fine tune / stretch
  are also covered by tests/Sampler162Tests.inc.
- F4 (cosmetic, fixed here): Quick-add palette — long names ran under the right-hand source label; the source is now
  laid out first and the name is cut with "...".
