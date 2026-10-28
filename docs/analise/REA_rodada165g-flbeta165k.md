# REA round 165g — sub-round flbeta165k: Kepler and Kepler Exo out of "(beta)"?

Goal: drop "(beta)" from Kepler / Kepler Exo only if LYNX really matches FL (FL's own renders of the factory presets
vs LYNX's, plus the EXEC harness around the unmodified engine DLL). Data: `/mnt/d/lynx-work/flbeta165k` (`fst/` the
277 Kepler preset states, `xfst/`+`xeng/` the 188 Kepler Exo states, `run/kp.exe` EXEC harness, `py/battery.py`,
`py/kcmp.py`, `fl/` FL renders and scripts; FL projects under `C:\Users\kauan\lynx-test\flbeta165k\`).

## Where I stopped
- 2026-10-28 05:10 — worktree recreated from origin/wip/flbeta165k (cffbe9ab) + origin/main. DLL battery re-run with
  cffbe9ab: all 283 states within −127 dB of the DLL (§1). FL renders of all 277 Kepler + 188 Exo presets running
  (`fl/batch.sh`); LYNX side via `renderwav` on the same .flp files (§2). Next: compare, then decide.

## 1. Powers of two as the DLL's library computes them (commit cffbe9ab) [DECOMP pow 0x1800bff00, powf][EXEC]
The engine calls the statically linked library's `pow` (double, oscillator increment) and `powf` (cutoff) inside the
render, in the "toward zero" rounding mode. Their results differ from `exp2` by one ulp on ~0.1–2 % of the inputs, and
self-oscillating patches (Resonance 1) amplified that to audible phase differences (round 46 §6.2: 10 of 277 presets
at −79 … −1 dB). LYNX now rebuilds base-2 `pow`/`powf` step by step (head/tail ln 2, index n = x·64/ln 2 in the current
rounding mode, reduced argument fused on FMA CPUs, Taylor degree 6/3, the 2^(j/64) tables read from the user's DLL at
0xF19B0 / 0xF1BB0 / 0xF1DB0): bit-exact against the DLL's own pow/powf on 10⁶ random inputs in both rounding modes.

Battery (`py/battery.py`, 4 overlapping notes with different Init.Vol/pan/pitch, 2 s, 44.1 kHz; `battery_44k_after.log`)
**[EXEC]**:

| | before (round 46) | now |
|---|---|---|
| bit-exact states | 8 / 283 | 64 / 283 |
| worst state | −1 dB (self-oscillating) | −127.1 dB (Noisypad) |
| self-oscillating outliers (> −99 dB) | 10 | 0 |

The remaining one-ulp float differences start after the first block (e.g. sample 570 of Bassliner) — other library
calls with a base other than 2 (envelope/glide coefficients `pow(0.1, …)`, `powf(0.1f, …)`), computed in LYNX in
extended precision rounded to double [HYP]. −127 dB is 2–3 float ulps of the peak.
