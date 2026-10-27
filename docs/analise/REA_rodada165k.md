# REA round 165k (flexact165) — every FL native plugin to Exact

Date: 2026-10-27. Clean-room: FL Studio's own plug-in DLLs are studied with Ghidra 12.1.4 (headless) and run
unmodified in our own harness (round 154's `fx.c`); this document describes their behaviour in our own words. No
Image-Line code, preset or sample is in the repository.

Markers as in round 154: **[DECOMP]** Ghidra, **[EXEC]** the DLL in our harness, **[DATA]** project / preset files,
**[FL]** FL's own render, **[HYP]** not verified.

## Scope

Effects still "Approximate" in docs/PLUGINS.md: Fruity 7 Band EQ, Fruity Bass Boost, Fruity Delay (v1), Fruity Phaser
(already exact since round 105 — the PLUGINS.md row was stale), Fruity Center, Fruity Phase Inverter, Transporter,
Emphasis. Then the "Close" generators (Sytrus, Harmless, Ogun, GMS, Morphine, Sakura / Drumpad).

## Method

`tmp/flexact165/`: `bin/` copies of the installation DLLs, `out/*.s` objdump listings of the parameter setters and
render routines (the plug-ins are small Delphi units; objdump was enough, Ghidra was not needed), `states/` the
plug-ins' saved states from the installation's projects and presets. `py/lgbatch.py <kind> <seed> <n>` renders each
installation state, a state-less instance and `n` random states (random rate 22.05 / 44.1 / 48 / 96 kHz, host blocks
of 512, 333, 64 or 1 frames, up to three control changes at random samples) through the DLL in our harness (`early`:
rate before the state, as FL) and through LYNX's engine (`cpp/lgcmp.cpp`) and compares the float bits;
`py/lggold.py` makes the unit-test goldens.

All five are Delphi `TFruityPlug` effects: the state (record 0xD5) is `i32` version 1 followed by one `i32` per
control (FL's raw control value); ProcessParam gets `v = (raw − min) / (max − min)` as a float. The kernels run on the
x87 in 80-bit precision with float or double states; LYNX does the same (`long double`, 64-bit precision, round to
nearest, `fp-contract=off`).

## 1. Fruity Phase Inverter **[DECOMP + EXEC]**

One control 0…1024 (default 1024). `mode = round(float(3v − 0.5))` (ties to even): 0 none, 1 left, 2 right, so raw
0…341 none, 342…682 left, 683…1024 right. The selected side gets its sign bit flipped. FL's default therefore inverts
the right side only; LYNX's old import (a utility stage inverting both sides) was wrong for every state.

## 2. Fruity Center **[DECOMP + EXEC]**

One control: on (raw ≠ 0, default 1). A DC-removing two-pole high-pass per side: `c = float(4.567e-6 · 44100 / fs)`,
`q = 0.96`; per sample `v += (x − l)·c; l += v; out = x − l; v *= q` in 80-bit. The four states are kept in 80-bit within
a call and stored as doubles at its end, zeroed when |s| ≤ 2⁻²⁴. A sample-rate notification clears them.

## 3. Fruity Bass Boost **[DECOMP + EXEC]**

Two controls 0…1024: Frequency (512) and Amount (768). `hz = round(80v + 20)` (whole Hz 20…100); `w = 2π·hz / fs`;
`cs = float(cos w)` (the Delphi run-time's cosine); `c = 2 − 2cs`; with `m = cs − 1`:
`r = float((√2·√(−m³) + m/2) / (m/2))` (`m³` by Delphi's integer power in single precision); gain
`g = float(e^{v·ln 11} − 1) / 2`. Per sample and side: `v += (x − l)·c; l += v; out = l·g + x; v *= r` — a resonant
two-pole low-pass added to the dry signal — with the same 80-bit / guarded-double state handling as the Center.

## 4. Fruity 7 Band EQ **[DECOMP + EXEC]**

Seven controls −1800…1800 (0 dB at 0): `dB = 36v − 18`. Bands: low shelf 63 Hz (shelf slope 0.5), peaks at 250, 500,
1500 Hz (bandwidth 2 octaves) and 3000, 5000 Hz (1 octave), high shelf 8 kHz (0.5). The coefficients are always
designed for **44.1 kHz**, whatever the host rate (the plug-in never reads the rate for them [EXEC: identical at every
rate]): `A = 10^(dB/40)` (Delphi `Power` in single precision), `w = float(2π·f) / 44100`, sine and cosine from the
Delphi run-time; peaks `α = sin w · sinh(ln 2 / 2 · bw · w / sin w)`, shelves `α = √((A² + 1)/S − (A − 1)²)`; the RBJ
cookbook coefficients computed in double and stored as floats. Each band is a biquad run per sample in 80-bit with
float history, the history flushed (|h| < 2⁻²⁴ → 0) at the end of each call. A band set to 0 dB keeps running for 48
more render calls (its tail), then its history is cleared and it is skipped; any other gain keeps it running.

## 5. Fruity Delay (the first one) **[DECOMP + EXEC]**

Six controls: Input 0…1024 (338), Feedback 0…1024 (512), Cutoff 0…1024 (1024), Tempo 0…1024 (0), Time 1…16 (3), Mode
0…1024 (0). ProcessParam:

| Control | Value |
|---|---|
| Input | the input level `v` (a glide target) |
| Feedback | `float(0.99·v)` (a glide target) |
| Cutoff | one-pole coefficient `k = float(0.05 · float(e^{v·ln 21} − 1))` (1 at v = 1, 0 at v = 0) |
| Tempo | 0 = the host's tempo; otherwise `round(220v + 60)` BPM (60…280). A sixteenth is `round(float(15·fs) / tempo)` samples |
| Time | `round(15v + 1)` sixteenths |
| Mode | `round(3v − 0.5)`: normal, inverted stereo, ping-pong |

Tempo, Time (and the host's tempo notification while Tempo is 0) set the delay to sixteenth × steps samples (at least
1). The delay line is a ring of `delay + host block size` stereo frames; when its size changes it is reallocated and
cleared, the read position at its start, the write position one delay later, and the feedback glide jumps to its target.

Glides: at the start of every render call a control glides from its current value to its target linearly over the call,
at most `float(44.1 / fs)` per sample (0.001 at 44.1 kHz); if the limit applies and the end point lands within 2⁻²⁴ of 0
it ends at 0. A fresh plug-in starts at its defaults (Input 338/1024), so a loaded state's input level glides in.

Render, per call of n frames:

1. The input, times the gliding Input level, is **added** into the ring at the write position (the routine steps the
   gain two frames at a time: even frames from `g`, odd from `g + d`, each advancing by `2d`; the last `n mod 8` frames
   by `d` — float arithmetic). With a constant level of 0 nothing is added.
2. Per frame: the delayed frame is read at the read position and cleared; the output is the dry input plus the delayed
   signal (inverted stereo and ping-pong swap the sides at the output); each side's line gets
   `z = z + ((d·g + w) − z)·k` (80-bit: `d` the delayed sample of the same side — of the other side in ping-pong — `g`
   the gliding feedback, `w` the input already added there), stored as a float (0 when |z| ≤ 2⁻²⁴). The two `z` stay in
   80-bit during the call and are stored as doubles. Both positions advance and wrap.

The library also has SSE versions of the normal and ping-pong kernels (double low-pass), selected by a CPU-feature flag;
in the installation they are not used (the x87 kernels are what the DLL computes [EXEC]).

## 6. Fruity Phaser

Exact since round 105 (`EffectKind::fruityPhaser`); docs/PLUGINS.md still listed it as Approximate.

## 7. What LYNX does

`src/core/FlLegacyFx.{h,cpp}` (`lynx::fllegacy`): the five engines and their effects (`EffectKind::flPhaseInverter`,
`flCenter`, `flBassBoost`, `fl7BandEq`, `flDelay1`), the slot parameters being FL's controls `(raw − min) / range`, so
FL automation and the LYNX editor drive the same values; host blocks are cut into 512-frame engine calls. `effectFromFl`
builds them from FL's state (names compared case-insensitively: FL 3 presets spell "Fruity 7 band EQ"); unreadable
states keep the old approximations and the import report says so. Delay (v1) takes the song tempo while its Tempo
control is 0. All five are in the Mixer's effect list.

## 8. Verification **[EXEC]**

| Plug-in | Installation states | Random states (rates, blocks, control changes) | Result |
|---|---|---|---|
| Phase Inverter | 0 (+ state-less) | 40 | all bit-identical |
| Center | 1 (+ state-less) | 40 | all bit-identical |
| Bass Boost | 3 (+ state-less) | 40 | all bit-identical |
| 7 Band EQ | 13 (+ state-less) | 40 | all bit-identical |
| Delay (v1) | 7 (+ state-less) | 40 (and 6 more with seed 1) | all bit-identical, the three modes, host and fixed tempo, 1-frame calls included |

Before this round these five were approximations on other LYNX engines (no sample null to speak of: the Phase Inverter
inverted the wrong side, the Center passed the signal through, the EQs and the delay were LYNX's own designs).

`tests/tests.cpp` `fl_legacy_fx_match_fl_engines`: DLL goldens (two rates each; FNV-1a of the float bits on x86, ten
samples within the x87 tolerance elsewhere), the value texts and the Phase Inverter's side boundaries.

## Where I stopped

- 16:40 start: worktree `lynx-wt-flexact165`, tools in `tmp/flexact165/` (bin = DLL copies, py, cpp, gh), data in
  `/mnt/d/lynx-work/flexact165`. States of the seven plug-ins extracted (`states/<tag>/`, `states/index.txt`).
- 17:00 engines written and bit-exact against the DLLs: Phase Inverter, Center, Bass Boost, 7 Band EQ.
- 17:40 Fruity Delay (v1) exact; all five wired (import, mixer, project ids, tests, PLUGINS.md, pt-BR, notes).
  Next: build-local + Windows build, land; then Transporter (§ to come), Emphasis, the Close generators.
