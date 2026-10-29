# REA round 165g, sub-round fltrans165 — Transmitter

Evidence markers: [DECOMP addr] Ghidra on the installation's `Plugins\Fruity\Effects\Transmitter\Transmitter_x64.dll`
(the Delphi front end) and `Shared\ildsp_x64.dll` (the engine; SHA-256 `b96354e1…1760`, the same file rounds 54/60/154
analysed), clean-room: behaviour only; [EXEC] a small Windows harness that loads `ildsp_x64.dll` and drives its
`Transmitter_CreateInstance` engine directly (goldens), [FL render] FL64.exe /R, [TEST], [SHOT], [HYP].

## Where I stopped
- 2026-10-29 08:05: every section ported (§3) and checked against FL's engine (§4); wired into LYNX (effect kind
  `transmitter`, FL import with the sidechain, FL-arranged layout editor, i18n in the 14 languages, tests/FlTrans165Tests.inc).
  Windows build OK; editor screenshots (--flextrashot transmitter-default / -bounce-delay / -blur-off) checked, layout
  fine; fltrans165 tests 196 checks / 0 failures. Next: full suite result, landing on main for 0.1.20.

Method note: the decompile is read for behaviour only (structure, constants, parameter mappings, routing). LYNX's
engine is an own implementation of that behaviour; it is checked numerically against FL's DLL run as a black box by
the EXEC harness (no decompiled code is translated into LYNX). Where an own implementation cannot reach FL's exact
float output, the residual is measured and documented instead.

## 1. Front end (Transmitter_x64.dll) [DECOMP 0x7145c0, 0x715d30, 0x7161d0, 0x7157e0]
- 42 controls. State 0xD5: i32 version 3, 42 i32 raw control values (0…10000 for 0–32, raw ints for 33–41), then
  transient-send track, sustain-send track, sidechain track, an editor value (49…90), 4 editor bytes.
- Control → engine value (x = raw / 10000; curve(lo, hi, c) = lo + (hi − lo)·x^(1/c)):
  0 sensitivity x · 1–2 hold / release 1…250 ms · 3–4 transient / sustain level −120…+12 dB, c = 4 · 5–6 tilt
  −6…+6 dB · per section (transient 7…, sustain 10…): diffusion mix x, depth g 0…0.99, delay 10^(2x−1) (FL shows it in s: 0.10…10.00 s); saturation
  mix x, drive 0…18 dB, warmth x; bounce mix x, amplitude / delay curve −1…+1; delay mix x, time 0.001…2 s (c = 0.5),
  synced time 16·x sixteenths, feedback x · 33 lookahead on/off · 34 stereo link (stored; the engine ignores it)
  · 35 mode Smooth / Balanced / Dense · 36–37 transient / sustain on · 38–39 section effect Saturation / Bounce /
  Diffusion / Blur / Delay / Off · 40–41 tempo sync of the two delays.
- Render: the sidechain (or the input) goes to the engine as the detection signal; the engine's transient and sustain
  parts (its two extra stereo outputs, seen in the EXEC harness) can be sent to two mixer tracks; the main output is the engine's mix.

## 2. Engine (ildsp_x64.dll `TransmitterImpl`) [DECOMP 0x1801a5a10, 0x1801a6db0, 0x1801a7990][EXEC]
- Detection on (L + R)/2 of the detection signal: an 8-band analysis with per-band energy envelopes; its output is
  an envelope e ∈ [0, 1]. Each channel is delayed by the lookahead (2 ms when on, glided at 10 Hz) and split into
  transient = x·e and sustain = x·(1 − e) (their sum is the delayed input).
- Each part: tilt filter (one-pole split at 1 kHz, high part ×g, low part ÷g, g from ±dB), then ONE section effect
  chosen by the selector (Blur: nothing in the engine), output = transient·level·on + sustain·level·on, the gains
  glided per sample. Latency = 88 samples at 44.1 kHz with lookahead on.

## 3. The section effects (ildsp Faust modules) [DECOMP 0x180197710, 0x18019e940, …][EXEC]
- Saturation (WarmSatFaust): drive 10^(dB/20) into a smoothed tanh(x)/x gain (two one-pole stages at the warmth's
  frequency 2^(10·(1 − √warmth))·20 Hz), the dry path through the matching allpass, a soft knee above (1 − warmth)·0.9;
  mix, warmth and drive glided (c = 44.1 / fs).
- Delay (DelayFaust): feedback delay, a 10 Hz DC blocker and a ~10 kHz one-pole lowpass in the loop, the time glided
  (e^(−4/fs)), linear interpolation; synced time = 15000 / BPM · value ms.
- Diffusion (DiffuserFaust): 16 normalized-lattice allpass stages per channel ([v; y] = [[cos, −g], [g, cos]]·[x; d],
  cos = √(1 − g²), g glided). Stage k's base delay = Delay · fs · {0.2702, 0.1977, 0.1446, 0.1058, 0.0774, 0.0566,
  0.0414, 0.0303, 0.0222, 0.0162, 0.0119, 0.00869, 0.00635, 0.00465, 0.00340, 0.00249} (ratio ≈ 0.7316), plus a random
  offset (one LCG ×1103515245 + 12345 per sample shared by every stage and both channels, amplitude max(0, (2 − D)·0.025)·D
  of the base, clamped ±4800 samples). A stage takes a new delay only when its counter restarts: at a zero crossing of
  its output once at least the base delay has passed. Two taps crossfade (a triangle 0↔1 at 1 / ((clamp(1 − 5g, 0, 1)·112
  + 16)) per sample); the tap not heard is retargeted at the ends. Mix = dry·(1 − mix) + chain·mix. Initial delays are 0
  (the chain is a short allpass until the first restarts). Depends on flush-to-zero: denormals change the zero crossings,
  so LYNX's engine sets FTZ / DAZ while it runs, as FL's front end does.
- Bounce (BouncingTapFaust): 32 taps per channel (left: t = 1/64, 3/64 … 63/64; right: 2/64 … 62/64 and 1) at
  2 s · (1 − e^(k·t)) / (1 − e^k), k = −10 · Delay Curve (|k| ≥ 1e−6); tap gain (1 − |a|) + |a| · c(t) for a ≤ 0, with
  c(1 − t) for a > 0, c(t) = (e^(−10t) − e^(−10)) / (1 − e^(−10)); a = 0.95 · Amplitude Curve (the ×0.95 is in
  TransmitterImpl's setParam), glided like the mix. Taps crossfade over 50 ms (20 / fs per sample). Output = Σ taps · mix +
  dry · (1 − mix) (no feedback; the taps add up, so a flat curve is loud).
- Blur and Off: the engine passes the part unchanged (Blur has no controls in FL's editor either).

## 4. Verification [EXEC][TEST]
LYNX's engine (cpp harness) against FL's engine run by the EXEC harness (`trh.exe` loading Shared\ildsp_x64.dll), 3 s drum
input (in1) or the test input (in2, tests/FlTrans165Tests.inc), blocks of 512, 44.1 kHz, FTZ on both sides:

| config | what | null | exact samples |
|---|---|---|---|
| env, env2, env3, envnl | detector modes / sensitivity / hold / release / no lookahead (transient only) | ≈ −580 dB | 100 % |
| lvl | levels and tilts | −585 dB | 100 % |
| dly, dly2 | Delay (free, tempo-synced, feedback) | −581 / −583 dB | 100 % |
| blur | Blur | −582 dB | 100 % |
| dif, dif2, dif4–dif7 | Diffusion (Delay 0.12…9.5, depth 0.1…0.99, mix) | −582 … −584 dB | 100 % |
| sat, sat2, dif3 | Saturation | −140 / −141 / −143 dB | 42–68 % (tanh) |
| bnc, bnc2, bnc3 | Bounce (curves ±1, mixes) | −137 / −141 / −140 dB | 44–69 % (the tap sum's order) |
| tdef | FL's Default preset (Saturation / Diffusion) on in2 | −127 dB | |
| tdly | Delay / Delay, tempo-synced | exact | 100 % |
| tmisc | no lookahead, Dense, sensitivity 0.9 | −81 dB (max 3.9e−4) | 93.5 % |

tmisc: only Dense with a high sensitivity differs, in a short stretch (a 1-ulp logf difference moves the detector; FL's
CRT logf is not reproduced bit for bit). The tests hold the FL goldens of tdef / Bounce / tdly / tmisc (RMS and samples).

## 5. Done / what's left
- Done: the whole engine (detector, split, lookahead, tilt, levels, Saturation, Bounce, Diffusion, Blur, Delay), FL's
  state import (42 controls), automation ranges, the sidechain input feeding the detection (FL's sidechain index → LYNX's
  slot sidechain, like the Limiter's), the editor (FL's arrangement: detection, transient / sustain columns with the
  section knobs swapping in place), the parameter texts.
- Left: the transient / sustain sends to other mixer tracks (FL's plugin outputs carrying the split parts; the import
  reports them as "not routed"; the main output is exact); FL's synced-time text (FL shows steps:ticks); the envelope display of FL's editor;
  bit-exact tanh / logf (residuals above).
