# REA round 165g, sub-round flscratch165 — Fruity Scratcher

Evidence markers: [DECOMP addr] Ghidra on the installation's `Plugins\Fruity\Effects\Fruity Scratcher\Fruity
Scratcher_x64.dll` (clean-room: behaviour only), [DFM] the editor form `TSYNTHEDITORFORM` in the DLL's resources,
[FL render] FL64.exe /R of a project, [TEST], [SHOT], [HYP]. Earlier analysis: REA_rodada20 §2 (engine), rounds 71 / 77
(why it stayed unported: FL's Ogg decoder — solved in round 77 — and the pattern events' scale — solved here).

## Where I stopped
- 2026-10-28 21:10: core (`src/core/Scratcher.{h,cpp}`), effect wiring, import (state, sample path, pattern-event
  ranges), editor (`ui/FlExtraEditors.cpp`), tests (`tests/FlScratch165Tests.inc`) written; first core build running;
  FL renders of 7 crafted projects running (E:\lynx-work\flscratch165\r). Next: LYNX renders of the same projects,
  compare, Windows build + editor shot, land.

## 1. What it is
An **effect** (FL lists it under Effects): a turntable over a sample, **added to the slot's input**. One turn of the
platter = the whole sample, which loops. Modes: motor (Hold off: the platter turns by itself) and hold (Hold on: the
platter follows Wave position). DVS (timecode vinyl via engine_x64.dll) is not reproduced.

## 2. Controls [DFM] and parameters [DECOMP 0x729fe0 ProcessParam, 0x72a630 apply, 0x729f10 reset]
| # | FL control (DFM) | raw range (default) | internal value |
|---|---|---|---|
| 0 | WavePosWheel (endless) | 0…30000 (690) | turns 0…1 |
| 1 | MuteCheckBox | 0/1 | host value v → internal \|v − 1\|; internal ≠ 0 = sound |
| 2 | TurntableSpeedWheel | 0…100 (50) | v |
| 3 | TurntableAccWheel | 0…100 (25) | v |
| 4 | PanWheel | −128…128 (0) | 2v − 1 |
| 5 | VolumeWheel | 0…255 (199; reset 200/255) | linear gain |
| 6 | TurntableHoldCheckBox | 0/1 | bool |
| 7 | TurntableSenWheel | 0…100 (50) | editor drag speed only (0.0025 · v turn per pixel) |
| 8 | TransportSelector | off / on / inverted → −1 / 0 / +1 (default 0: stopped) | integer |
| 9–12 | Smth1…4Wheel | 0…1000 (500, 908, 999, 400) | v |
| 13 | RotationsDummyWheel (hidden) | 0…18 (9) | integer; whole turns (−9…+9) |

The value FL hands a plugin through its controls is the control's integer: `ProcessParam` turns it into
`(raw − min) · (1 / (max − min))`; the apply stores `min + Round(v · (max − min))` back into the control table
(Delphi Round = CVTSD2SI, ties to even [DECOMP 0x409fe0]). A state load re-applies every control twice from that table,
so the loaded values are **quantized to the control steps** (and Mute's inversion cancels out) [DECOMP 0x728d80].
LYNX quantizes the same way on import and in its engine.

## 3. State (0xD5) [DECOMP 0x728d80]
u32 version (FL 2026 writes 0xF462A, the render test / presets hold 0xF4628), 14 f32 internal values (§2), i32 n + n
bytes of the sample's path (ANSI, with FL's `%FLStudioFactoryData%` macros), then version > 0xF4628: the editor's size
byte; ≥ 0xF462A: the timecode index (i32, −1 = off) and the RIAA byte. Older (integer) versions: in no file seen,
refused by LYNX.

## 4. Engine [DECOMP 0x72add0 render, 0x72be20 sample load]
- Sample: requested from the host as 16-bit stereo 44.1 kHz, `/32768`; one extra frame for the interpolation, which FL
  fills with **frame 0's right channel on both sides** [DECOMP 0x72c214]. 1/(n−1), n−1 and 2/(n−1) are kept. A newly
  loaded sample takes over at the end of the next block, and the volume target is 0 for the block after it.
- Reading: position φ in turns, wrapped into [0, 1] (motor: φ itself; hold: a copy), `x = φ · n`, frame index
  `Trunc(x − 0.5) + (Frac(x − 0.5) > 0.5)` [DECOMP 0x72ab70], linear interpolation; each side through the DC blocker
  `y = (s − s₋₁) + 0.99 · y₋₁` (in double) [DECOMP 0x72c660 / 0x72c6d0]; values below 1e−14 flushed [DECOMP 0x72c740].
- Gain `g ← 0.99 g + 0.01 T` and pan `p ← 0.99 p + 0.01 P` per sample; T = Volume (0 when muted), P = Pan, **both
  taken at the end of each block** (the next block glides to them). Linear balance: p ≤ 0 → R · (1 + p); p > 0 →
  L · (1 − p). Output = input + sample.
- Motor (Hold off): `v ← a (v − s) + s`, `s = (2/(n−1)) · Speed · Transport`, `a = 0.99999 − 0.0005 · Acc`; the sample is
  read at φ, then `φ += v`; Wave position follows φ (FL's editor shows it turning).
- Hold (Hold on): `e = (W + Rot − 9) − φ`, `v ← c₂ v + (1 − c₂)/(n₁ + 250) · e`, `φ += S₄ v + e (1 − c₃)(1 − S₄)` with
  `n₁ = round(2500 S₁)`, `c₂ = 0.99 + 0.0099 S₂`, `c₃ = 0.99 + 0.00999 S₃`. W = **the previous block's** Wave position.
  The `(1 − c₃)(1 − S₄)` term sets a ~2 s time constant at the defaults: a still platter goes silent (DC blocker).
- Editor [DECOMP 0x732630 / 0x7329d0 / 0x732770]: pressing the platter turns Hold on, releasing turns it off; dragging
  moves Wave position by 0.0025 · Sensitivity of a turn per pixel, whole turns counted in the hidden Rotations wheel
  (0…18). The waveform strip scratches the same way, horizontally.

## 5. Pattern events
FL's render test automates Wave position with 259 `0xDF` records (old mixer class 0x2…, track 1, slot 0, param 0x8000),
values 0…131. [FL render vs model] those are the control's raw integers (/30000): a Python model of §4 fed /30000
follows FL's level envelope window by window (0.1 s windows, e.g. 0.088 / 0.085, 0.120 / 0.127, 0.133 / 0.133), /3000
and /1000 do not.

## 6. LYNX
`EffectKind::scratcher` (project id `fruityScratcher`), core/Scratcher.h (engine, state, the host's 16-bit copy), the
sample in `EffectSlot::samplePath` (loaded off the audio thread by the rack builder, like Newtone's), import of FL's
state + path (%FLStudioFactoryData% resolved, missing files reported, DVS noted), pattern-event / automation ranges,
editor (platter, waveform, transport, knobs; drag = scratch with Hold, file drop / Load sample).
