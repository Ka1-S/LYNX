# Round 165p "issue12" — GitHub issue #12 (BURNER1PA, Windows 10, portable 0.1.18)

## Where I stopped
- 2026-10-28: worktree ../lynx-wt-issue12 on feature/issue12 from main ea719caf.
- Code done (uncommitted→wip): bug 1 (tools + editor letters in the key map, user keys win, F-key/numpad letter fix
  from r165q), bug 3 (song-mode automation take: linear playback + clip grows), bug 4 (pitch wheel recorded as the
  channel's MIDI pitch bend target; plays on LYNX instruments). Tests in tests/Issue12Tests.inc (key map part passes).
- Baseline for repro: C:\Users\kauan\lynx-test\issue12\main\LYNX.exe (= land-vfxscript165 = main ea719caf).
  Helpers: C:\Users\kauan\lynx-test\issue12\*.ps1, scratchpad w.sh / lockgui.sh.
- Next: reproduce bugs 1, 2, 5 (crash) on Windows; build-local; Windows build of the branch; verify; land batch 1.

## Items
| item | reproduced? | root cause | fix commit | test | Windows check shot |
|---|---|---|---|---|---|
