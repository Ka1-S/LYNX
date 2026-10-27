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
