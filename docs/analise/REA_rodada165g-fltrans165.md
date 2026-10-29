# REA round 165g, sub-round fltrans165 — Transmitter

Evidence markers: [DECOMP addr] Ghidra on the installation's `Plugins\Fruity\Effects\Transmitter\Transmitter_x64.dll`
(the Delphi front end) and `Shared\ildsp_x64.dll` (the engine; SHA-256 `b96354e1…1760`, the same file rounds 54/60/154
analysed), clean-room: behaviour only; [EXEC] a small Windows harness that loads `ildsp_x64.dll` and drives its
`Transmitter_CreateInstance` engine directly (goldens), [FL render] FL64.exe /R, [TEST], [SHOT], [HYP].

## Where I stopped
- 2026-10-29 07:15: front end and engine mapped (§1–§3); EXEC harness runs FL's engine on Windows. Next: the Faust
  modules' DSP (detector, enveloper, tilt, saturation, bounce, diffuser, delay), LYNX's port, compare with the harness.

Method note: the decompile is read for behaviour only (structure, constants, parameter mappings, routing). LYNX's
engine is an own implementation of that behaviour; it is checked numerically against FL's DLL run as a black box by
the EXEC harness (no decompiled code is translated into LYNX). Where an own implementation cannot reach FL's exact
float output, the residual is measured and documented instead.

## 1. Front end (Transmitter_x64.dll) [DECOMP 0x7145c0, 0x715d30, 0x7161d0, 0x7157e0]
- 42 controls. State 0xD5: i32 version 3, 42 i32 raw control values (0…10000 for 0–32, raw ints for 33–41), then
  transient-send track, sustain-send track, sidechain track, an editor value (49…90), 4 editor bytes.
- Control → engine value (x = raw / 10000; curve(lo, hi, c) = lo + (hi − lo)·x^(1/c)):
  0 sensitivity x · 1–2 hold / release 1…250 ms · 3–4 transient / sustain level −120…+12 dB, c = 4 · 5–6 tilt
  −6…+6 dB · per section (transient 7…, sustain 10…): diffusion mix x, g 0…0.99, delay 10^(2x−1) ms; saturation
  mix x, drive 0…18 dB, warmth x; bounce mix x, amplitude / delay curve −1…+1; delay mix x, time 0.001…2 s (c = 0.5),
  synced time 16·x sixteenths, feedback x · 33 lookahead on/off · 34 stereo link (stored; the engine ignores it)
  · 35 mode Smooth / Balanced / Dense · 36–37 transient / sustain on · 38–39 section effect Saturation / Bounce /
  Diffusion / Blur / Delay / Off · 40–41 tempo sync of the two delays.
- Render: the sidechain (or the input) goes to the engine as the detection signal; the engine's transient and sustain
  outputs can be sent to two mixer tracks; the main output is the engine's mix.

## 2. Engine (ildsp_x64.dll `TransmitterImpl`) [DECOMP 0x1801a5a10, 0x1801a6db0, 0x1801a7990][EXEC]
- Detection on (L + R)/2 of the detection signal: an 8-band analysis with per-band energy envelopes; its output is
  an envelope e ∈ [0, 1]. Each channel is delayed by the lookahead (2 ms when on, glided at 10 Hz) and split into
  transient = x·e and sustain = x·(1 − e) (their sum is the delayed input).
- Each part: tilt filter (one-pole split at 1 kHz, high part ×g, low part ÷g, g from ±dB), then ONE section effect
  chosen by the selector (Blur: nothing in the engine), output = transient·level·on + sustain·level·on, the gains
  glided per sample. Latency = 88 samples at 44.1 kHz with lookahead on.
