# Round 165q — audit165: independent verification of the #4, #9, #11 (and #8) fixes on Windows

Adversarial check, not the author's: every claim we posted in the replies to BURNER1PA (#4, #9), the #11 draft
(tmp/coord/replies-0.1.20.md) and the #8 reply is redone on the current origin/main Windows build, the way the user
works (real windows, mouse and keyboard), with a fresh `LYNX_PROFILE_DIR` + `LYNX_TEST_MODE=1`, under
`~/.cache/lynx-wingui.lock`. Verdicts: PASS / FAIL / PARTIAL, each with the screenshot that was looked at
(shots in /mnt/d/lynx-work/audit165/, not in git).

## Where I stopped
- 2026-10-28 08:50: session 1-2 done on main's own Windows build (ea719caf, build-win/LYNX.exe). Results below.
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
| 11 | FL window keys (Ctrl+F5 …) | PARTIAL | Ctrl+F5 works; F-keys are swallowed while the Playlist / Piano Roll has the focus (see Findings F1) |

## Findings (not in the posted claims, found on the way)
- F1 (handed to r165p, shortcuts): with the Playlist focused, F4 changes the Playlist tool instead of making the next
  empty pattern. PlaylistComponent::handleKey / PianoRollComponent::handleKey turn the key code into a letter with
  towupper, which truncates JUCE's Windows codes (`VK_F4 | 0x10000`) to 's' → the S tool. F1-F11 → P-Z, Numpad1-9 → A-I.
- F2: dialogs with a text field (Rename pattern, F4's "and name it", rename insert, …) open with the field not
  focused: typing does nothing until the field is clicked (Enter presses OK). JUCE's AlertWindow never focuses its
  text editor. (to fix here)
- F3: Ctrl+A does not select all in the Quick-add palette's search box. (to fix here)
- F4 (cosmetic): Quick-add palette — long names run under the right-hand source label.
