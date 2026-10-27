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
