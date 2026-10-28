# Round 165p "issue12" — GitHub issue #12 (BURNER1PA, Windows 10, portable 0.1.18)

## Where I stopped
- 2026-10-28 09:30: batch 1 = feature/issue12 (worktree ../lynx-wt-issue12): bugs 1/3/4, crash (wavecache sort, FileSort.h),
  ASIO panel use-after-free, tool keys, pattern length box, follow toggles, Audio Editor drop + send to Sampler, VU click,
  menu-header rename. Commits 7b1042de, c475852e, 98900da0. Builds running (scratchpad bl2.log, winb3.log).
- Batch 2 = feature/issue12b (worktree ../lynx-wt-issue12b, from c475852e): Sampler knobs automatable (samplerparam),
  mixer send level automatable (AutomationTargetKind::mixerSend), Piano Roll bold "All" channel, Browser OS-file drop
  into a folder, Sampler waveform drop replaces sample, Theme Backgrounds folder + startup sound. Uncommitted, unbuilt.
- Ctrl+H in Playlist: audit artefact (scan code 0); keys.ps1 now sends real scan codes.
- Next: Windows check of batch 1 with build-win-issue12 exe; land batch 1; build/verify/land batch 2.

## Items
| item | reproduced? | root cause | fix commit | test | Windows check shot |
|---|---|---|---|---|---|
