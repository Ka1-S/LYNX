# REA round 165g — sub-round flbeta165h: the Harmor presets that still differ from FL

Goal: make LYNX's Harmor match FL's `Harmor_x64.dll` on the factory presets that still differ after flbeta165s
(REA_rodada165g-flbeta165s.md §5), then drop "(beta)" only if every preset nulls (< −60 dB) or matches its spectrum
(STFT magnitude < −40 dB), except presets the DLL itself makes non-deterministic / explode.
Data: `/mnt/d/lynx-work/flbeta165h` (`bat.sh` → `py/full.py`: all 325 installation presets, DLL vs LYNX, 3 notes,
rack off, the DLL's Mersenne-Twister state handed to LYNX; columns idx, null dB, STFT dB, name).

## Where I stopped
- 2026-10-28 13:55 — worktree + battery set up; baseline `base0.tsv` on origin/main ea719caf: 262 null < −60 dB,
  44 more with STFT < −40 dB, 19 audibly different. Next: part parameter 2 (note start delay).

## Battery history
| step | null < −60 | STFT < −40 only | audible (STFT ≥ −40) |
|---|---|---|---|
| base0 (origin/main ea719caf) | 262 | 44 | 19 |
