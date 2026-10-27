# REA round 165hb (parity165b) — LYNX Tune ← Auto-Tune Pro toward 10/10

Sub-round of r165h "parity165". Pair: **LYNX Tune ← Auto-Tune Pro (Auto mode)**, before: config 8 / design 7.
Black-box only: Auto-Tune Pro 11.6 (`Auto-Tune Pro.vst3`) and AutoTune 2026 (`AutoTune.vst3`) hosted in
tools/fxmeasure (public VST3 parameters, value texts, audio in / out, editor screenshots). Data:
`D:\lynx-work\parity165b\at` (in/ test signals, out/<campaign>/ renders, *.png editor shots); harness
`tmp/parity165b` (py/atrun.py, camp*.py, anaprobe.py, anath.py; fxlynx.cpp).

## Where I stopped
- Resumed after the 2026-10-27 BSOD (the first agent died before any commit). Re-read the data on D:: editor shots
  (old_main, new_main; old_adv.png is a copy of old_main — the Advanced click did not take), throat campaigns
  `out/throat` (vowels) and `out/probe` (single-resonance probe).
- Running now: `out/vib` (created vibrato amplitude / formant / variation; Transpose In Scale + Scale Transpose).
- Next: implement Throat length (%), created-vibrato amplitude / formant / variation, transpose in scale; tests;
  editor; build-local + Windows build; --plugui shots; land.
