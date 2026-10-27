# REA round 165g — sub-round flbeta165s: Harmor out of "(beta)"?

Goal: drop Harmor's "(beta)" only if LYNX's engine really matches FL (null / spectra / listening on FL's presets and
demo projects). Data: `/mnt/d/lynx-work/flbeta165s` (survey tsv, `st/` old states, `up/` DLL re-saves, `run/hm.exe`
EXEC harness around the unmodified `Harmor_x64.dll`, `py/` batteries, `tools/harmorrender`).

## Where I stopped
- 2026-10-27 16:05 — resumed after the BSOD. Checkpoint commit 798e9ad2 (old-state upgrade in `Harmor.cpp`, the
  importer storing upgraded bytes, velocity-following portamento) kept; not yet re-verified. `py/bisect.py` and the
  `run/o` listing were zeroed by the crash (rewritten as needed).
- 16:30 — harmorrender rebuilt from the checkpoint (identical binary to the pre-crash one: the checkpoint compiles).
  Full battery of all 325 installation presets (`bat_all0.log`, `bat_all0_spec.tsv`): 248 null < −60 dB, 291 STFT
  magnitude < −40 dB; 34 audibly different (§3). Ghidra project of Harmor_x64.dll being imported (own project
  `/mnt/d/lynx-work/flbeta165s/ghidra`, under the ghidra lock + build slot) for the missing filter shapes.
- Pre-crash battery `bat_old1.log` (88 old presets, LYNX(upgraded) vs DLL(old), rack off): 70 within −70 dB, outliers
  listed in §3. Next: rebuild harmorrender, re-run, triage outliers, run the current-format presets + demos.

## 1. Old states (version 11…15) [DECOMP FUN_0086ed30][EXEC]
The 88 presets saved by Harmor's first releases (Default.fst among them) were refused by LYNX ("state not usable").
FL's loader brings them into the current layout: a part stores 385 integers (the articulator-33 smoothing, index 376,
is missing — a 0 is inserted); version 11 stores the 43 smoothing times (343…385) on an older scale, converted as
`round(log2(1 + (41^(v/1000) − 1)·37.5·0.0511)·1000/9)`; articulator 33 is absent and gets the default articulator
(off, amounts 1, envelope 0.5, LFO 0.75, flat maps). The global text becomes an empty string + option byte 1.
`harmorUpgradeState()` writes exactly what the DLL re-saves (`up/up*.bin` = the DLL's own re-save through the EXEC
harness, `py/hup.py` = our upgrade): all 88 identical except shaping-curve point positions the DLL renormalises at
load (sub-ulp of the curve length). Rendering the DLL with the old bytes vs its own re-save vs our upgrade:
−141…−629 dB on the deterministic ones (`py/dll_up.py`) **[EXEC]**. The importer stores the upgraded bytes so the
settings panel's parameter offsets apply. A new Harmor instance now starts from FL's Default.fst.

## 2. Portamento following the velocity [DECOMP FUN_00883ae0]
Global 6 on: the note's glide factor is `(1 − 0.85·velocity)²` (float) — a harder note glides faster.

## 3. Battery: all 325 installation presets, DLL vs LYNX [EXEC]
`py/bat_all.py` (3 notes: 60/1.0, 67/0.6, 55/0.95 over 1 s at 44.1 kHz, effects rack off, the DLL's Mersenne-Twister
state handed to LYNX), `py/spec.py` (STFT magnitude error, 4096/2048 Hann). Results `bat_all0.log`, `bat_all0_spec.tsv`:

| | presets |
|---|---|
| null < −60 dB (sample-exact within float noise) | 248 / 325 (median −74.8 dB) |
| null ≥ −60 dB but STFT magnitude < −40 dB (same sound, phase drift) | 43 |
| audibly different (STFT ≥ −40 dB) | 34 — 18 use a filter shape LYNX lacks (8, 10, 11, 12, 13, 15, 16) |

Audibly different without a fallback: Dub steppy 1, Ice bells, Labyrinth, Shield hit, Magic swamp, Rotochip,
Morning, Sunroom, Metal thingy, Pomona, Image synthesis/Interpolation, Resynthesis/Harmony, Olbaid Plato Cerrado,
FX Subida, FX White Noise Caida, Lead Hoover. (Olbaid Bajo Acustico: the DLL itself blows up to 3.7·10¹⁹.)

- **Labyrinth / Interpolation**: their images are small (16 × 12 → an 829-byte object; Interpolation 317 bytes) and
  LYNX only built images from a part tail > 1024 bytes, so it played them without the image. Fixed: any image object
  is read (FL's DLL plays it: the DLL's partials are inharmonic, LYNX's were harmonic) **[EXEC]**.
- **Pomona**: no single parameter explains it (`sweep_pomona.log`: every parameter/global that differs from
  Default.fst reset one at a time; best −8 dB) — several interacting causes, not found yet.
- **Frequency phaser (type 209) at a high mix** (Ice bells, Magic swamp, Metal thingy, Church bell, Steam starship;
  `py/` probes): the error starts at 10⁻⁴ relative and grows — a frequency drift. With the precision patched to 1
  ("High") the same presets go from −13…−18 dB to −47…−56 dB **[EXEC]**: at precision 0 FL computes the phaser's
  `2^x` frequency factor with IPP's 11-bit `Exp_32f_A11` (REA_rodada75 §5), which LYNX does not copy. The spectra
  match (−35…−45 dB STFT); the residue is phase drift.
- **Part parameter 2 = note start delay** [DECOMP FUN_00879540 case 2, FUN_0088ab10 0x88b150]: `p < 0`:
  `(2^(−p/48) − 1)/255` s, `p ≥ 0`: `p·0.3125/tempo`; times the sample rate and the voice's articulator-8 output
  (voice +0x3214), the voice's output starts that many samples later (its control-block grid shifts with it).
  LYNX ignores it. Used by Rotochip (part B, −100) and Resynthesis/Harmony (−135) only: −47 dB on Rotochip with the
  delay zeroed in both. Not implemented this round (LYNX's records share one control-block grid).

## 4. Filter shapes 8, 10…13, 15, 16 [DECOMP FUN_00874ed0 + DLL constants]
Used by 24 presets (18 of the audible outliers); LYNX switched that filter off. Tables (s = 1/(N−1), cos in double,
`dB(x) = exp(float(x·−72)·float(ln10/20))`, `p16(x) = (x⁴)⁴`):
- 8 (N 682): `−cos((1 − i·s/2)·π)`; 10 (N 1365): u = the same expression + 1 … `dB(u)·(1 − p16(u))`;
- 11 (N 1024): `(1 − (cos(3π·(1 − i·s)) + 1)/2)·(1 − (cos(π·(1 − i·s)) + 1)/2)`;
- 12 (N 4096, origin ½): first half `(1 − (cos(2π(1 − i·s)) + 1)/4)²`, second half `dB(x)·(1 − p16(x))`, x = 2·(i − N/2)·s;
- 13 (N 1024): `exp(float((1 − (cos(π(1 − i·s)) + 1)/2)·18 − 18)·ln10/20)`;
- 15 (N 4096, origin ½, ends 1/1): `a²·b² + b/8`, a = (cos(12π(1 − i·s)) + 1)/2, b = 1 − (cos(2π(1 − i·s)) + 1)/2;
- 16 (N 2048, filled with 1.0, origin ½): the first and last N/3 entries `(1 − (cos(2π(1 − i·0.5/(N/3))) + 1)/2)²`.
