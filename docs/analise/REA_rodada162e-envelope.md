# Round 162e — Envelope Controller: LFO, maps, ADSR knobs, Mod X / Y (issue #7, item 3)

Clean-room behaviour study of FL Studio's own `Fruity Envelope Controller_x64.dll` (sha256 e60f7462…c763, the same
build as round 15) with headless Ghidra (project ~/ghproj/r162ec), plus the 22 factory presets in
`Data/Patches/Plugin presets/Generators/Fruity Envelope Controller`. No code or graphics copied; LYNX keeps its own
formulas and UI. HYP = inferred, not confirmed.

## State (event 0xD5)
- u32 version (0x00060003 → 6), 90 × i32 parameters (8 outputs × 11 + Mod X, Mod Y; value / 65536), 4 ints
  (output shown, part shown, global flags: bit0 Freeze, bit1 Slide, bit2 continuous output, combo).
- Per output: i32 flags (bit0 bipolar), version, f32 base / envelope amount / LFO amount, envelope (TCustomMEnvelope)
  + 4 i32 ADSR knobs, LFO envelope + 4 i32 (speed, tension, skew, pulse width) + i32 phase (LFO version ≥ 3),
  5 maps (key, velocity, Mod X, Mod Y, random; version < 6: 2), i32 low key, high key.
- Envelope header: flags (bit0 tempo, bit1 global), enabled, decay point, loop start, sustain point.

## Runtime
- `FUN_008af660` (note on): key = (cents + 6000) / 100; velocity input = ln(10·vol + 1) / ln(21.55) · 127/128
  (≈ 0.9918 × LYNX velocity); per output in key range: monophonic (last note wins), envelope / LFO restart
  (`FUN_007dfdf0`), voice factor = velocity map (`FUN_008b1410`) × random map (`FUN_007df500`) unless neutral.
- `FUN_008afec0` (tick): v = base; envelope on → v += env · amount (env centred on the mode's neutral,
  `FUN_007e0100` / `FUN_007d1600`); LFO → bipolar v += lfo · amount, unipolar v *= 1 + 2 · lfo · amount;
  key map (x = key / 120), Mod X map (note Mod X − 0.5 + knob), Mod Y map combined: product (unipolar),
  sum of (y − 0.5) (bipolar); × voice factor; clip 0…1; sent only when it changes. Without a voice: continuous
  output → base with the Mod X / Y maps at the knobs, else the last value is kept. Voice ends when envelope and
  LFO have finished (`FUN_008b11c0` parts list). Mode switch `FUN_008b0f90` moves neutral maps to the new neutral.
- Neutral map: flat 0.5 (bipolar) / 1.0 (unipolar) — LYNX stores it as an empty map. `FUN_007df730` = flat check.
- LFO (class "MLFO", VMT 0x7d1098): rate `FUN_007de390`: cycles per second (or beat) = (10001^(speed/65536) − 1) / 625;
  shape `FUN_007de1d0` / `FUN_006223f0` / `FUN_00622620`: triangle with skew (moves the bottom), pulse width (flat
  top), tension T = ±(1e18^((21^(|t|/128) − 1) · 0.05) − 1) bending each half. Start phase `FUN_007de6f0` /
  `FUN_00622910`: the rising zero crossing (+ phase offset), or the song position when GLOBAL. The LFO envelope is an
  amplitude curve (`FUN_007daf00`).
- ADSR knobs `FUN_008ae740` (params 11k + 3…6 envelope, 7…10 LFO), knob / 128 time scale (`FUN_007ddb90`),
  section weights `FUN_007d7720`; SUS as a level shift is HYP.

## LYNX
- `EnvelopeControllerData` (FlSynths.h) + LYEC v2 state (v1 and FL bytes still load), `controllerValue` with all
  parts, `ControllerInstrument` (continuous output, per-note Mod X / Y from the rack, global LFO from the song).
- Editor (ControllerEditors.cpp): X/Y pad, outputs 1–8, Base / Env / LFO knobs, Bipolar, Continuous output, part
  dropdown (Envelope, LFO, Keyboard / Velocity / Mod X / Mod Y mapping, Random), keyboard strip, ATT/DEC/SUS/REL,
  LFO Speed / Tension / Skew / PW with a wave preview; also usable for Patcher nodes (locate overload).
- Tests: tests/EnvController162Tests.inc.

## Not done
- Mod X / Y are saved and editable but not yet automatable / linkable as parameters.
- LFO flag bits 2 (alternate unipolar shaping) and 3; FL's per-tick update (LYNX per block); FL point tension
  formula approximated by LYNX's curve; FL applies flat non-neutral X/Y maps twice in voice mode (not copied).

## Where I stopped
Implementation, tests and screenshots done on feature/patcher162e (pushed to wip/patcher162e); landing is the
parent round's job.
