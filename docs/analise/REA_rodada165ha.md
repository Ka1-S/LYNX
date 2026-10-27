# REA round 165ha (parity165a): Ember Saturator ← Decapitator (Soundtoys)

2026-10-27. Sub-round of r165h parity165. Black-box only: the official Decapitator trial hosted in our own host
(tools/fxmeasure: public VST3 parameters, audio in / out, editor screenshot), the manual and measurements. Nothing was
decompiled; no graphics, preset names or text copied. Raw data: /mnt/d/lynx-work/parity165a (lab/, meas1_0.pkl, meas2.pkl),
harness ~/.claude/jobs/f8789b49/tmp/parity165a (py/, lab/decalab).

## Where I stopped
- Resumed after the 2026-10-27 BSOD. On disk: checkpoint 68b0ff3a (a `--plugui` shot of Ember's Classic voicing in
  src/Main.cpp, the fxmeasure `editor <out.png>` fix); trial renders meas1_0 (style A, Punish on at drives 0…10 + off at
  5 / 7 / 9: deep stairs 100 Hz / 1 kHz / 5 kHz, bursts, three takes) and meas2 (Punish's small-signal law at fine drives).
- meas2.pkl was zeroed by the crash; its WAVs (out/meas2) are intact and were used instead.
- Found [EXEC meas3/meas5]: the trial applies the FIRST setting after any Style / Punish change wrongly (a host-order
  quirk; re-sending the same values reads correctly). Round 136's Punish constant (+9.75 dB) came from such a reading.
  Steady state: Punish adds 13.8 – 14.1 dB + k × drive (k: A 0.41, E 0.48, N 0.10, T 0.51, P 0.74; meas4).
- Coded (uncommitted until the build): EmberClassic punishLaw per style + Settings::measuredPunish; Ember params
  `classicAuto`, `classicLaw` appended (count 160 = maxEffectParams); `ember::linkedEdit` (Auto: Output −1.6 dB / Drive
  unit, from her 63 states) hooked in the layout editor's setValue; the Classic layout re-arranged as the reference's
  one-screen panel (VU top centre, Punish top left, Output + Auto top right, big Drive left, filter row, A–P row);
  tests/Parity165aTests.inc.
- Next: lab eval old vs new Punish law on meas1 renders (py/evp.py), build-local + Windows build, --plugui shot.
