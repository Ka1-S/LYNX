# Round 165q — audit165: independent verification of the #4, #9, #11 (and #8) fixes on Windows

Adversarial check, not the author's: every claim we posted in the replies to BURNER1PA (#4, #9), the #11 draft
(tmp/coord/replies-0.1.20.md) and the #8 reply is redone on the current origin/main Windows build, the way the user
works (real windows, mouse and keyboard), with a fresh `LYNX_PROFILE_DIR` + `LYNX_TEST_MODE=1`, under
`~/.cache/lynx-wingui.lock`. Verdicts: PASS / FAIL / PARTIAL, each with the screenshot that was looked at
(shots in /mnt/d/lynx-work/audit165/, not in git).

## Where I stopped
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
| 9 | Options › Theme | FAIL (r165p's #12 item) | i5: the menu entry opens nothing on main ea719caf — reported to the coordinator |
| 11 | Ghost steps after the pattern's end; clicking one lengthens the pattern | PASS | f7/f8 (16 → 32 steps) |
| 11 | Setting a step is silent | PASS | f8/f9: master meter dark right after the click (f10: it does light up while playing) |
| 11 | Drop a Browser sample into the Channel Rack | PASS | f15: new "DL Breaker" sampler channel (drop from the OS file manager not automated) |
| 11 | Double right-click-and-hold mutes a clip | PASS | j2/j3: clip veiled + strike line (the muted look) |
| 11 | Playlist vertical zoom with Alt+wheel | PASS | j0/j1 |
| 11 | FL window keys (Ctrl+F5 …) | PARTIAL | Ctrl+F5 works; F-keys are swallowed while the Playlist / Piano Roll has the focus (see Findings F1) |

## Findings (not in the posted claims, found on the way)
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
- F4 (cosmetic, fixed here): Quick-add palette — long names ran under the right-hand source label; the source is now
  laid out first and the name is cut with "...".
