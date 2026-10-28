# REA round 165g — sub-round flbeta165h: the Harmor presets that still differ from FL

Goal: make LYNX's Harmor match FL's `Harmor_x64.dll` on the factory presets that still differ after flbeta165s
(REA_rodada165g-flbeta165s.md §5), then drop "(beta)" only if every preset nulls (< −60 dB) or matches its spectrum
(STFT magnitude < −40 dB), except presets the DLL itself makes non-deterministic / explode.
Data: `/mnt/d/lynx-work/flbeta165h` (`bat.sh` → `py/full.py`: all 325 installation presets, DLL vs LYNX, 3 notes,
rack off, the DLL's Mersenne-Twister state handed to LYNX; columns idx, null dB, STFT dB, name).

## Where I stopped
- 2026-10-28 15:10 — fixed: note start delay (§1), attack fade (§2), IPP Exp/Ln_32f_A11 at precision 0 (§3).
  Battery `s1.tsv`: 297 null < −60, 21 STFT-only, 7 audible: Pomona, Dub steppy 1, Shield hit (overlapping release +
  harmonizer, §4 in progress), Sunroom, Olbaid FX White Noise Caida / Plato Cerrado / FX Subida (precision-dependent,
  not Exp/Ln). Next: those 7; then tests, build-local, land.

## Battery history
| step | null < −60 | STFT < −40 only | audible (STFT ≥ −40) |
|---|---|---|---|
| base0 (origin/main ea719caf) | 262 | 44 | 19 |
| s1 (§1–§3) | 297 | 21 | 7 |
