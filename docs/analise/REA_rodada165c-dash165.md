# Round 165c sub-round dash165 — Dashboard polish, Dashboard MIDI, X-Y-Z curves, Voltage Controller links

Evidence markers: **[DECOMP addr]** headless Ghidra on FL Studio 2026's own DLLs, **[DATA]** FL's own files,
**[FL render]** FL's command-line render of a test project, **[EXEC FL]** FL running, **[TEST]** LYNX unit tests,
**[SHOT]** screenshot looked at, **[RUN]** LYNX run, **[HYP]** not checked against FL. Clean-room: no FL code, art or
presets are copied. Work data: `/mnt/d/lynx-work/dash165` (Ghidra copies `ghidra/dash`, `ghidra-xyz`, decompiles
`d_*.c`, `x*.c`).

## Where I stopped

- 2026-10-27 15:26 — the PC blue-screened; the agent died. Checkpoint commit 8a6f6b93 (22 files, unverified) holds:
  `core/FlCurve.{h,cpp}` (FL's point law, 13 modes), X-Y-Z curve modes (params 14…16) + import of the output curve's
  mode / tension, Dashboard limit 32 → 1024 outputs (Control Surface stays at maxEffectParams), Dashboard MIDI
  (per-control CC / RPN / NRPN / pressure / note, port / channel / program / bank, sent by the rack to the plugins
  on the port), Voltage Controller offsets / scales as automation / link targets, label fitting in SurfacePanel.
- 2026-10-27 16:00 — resumed (new agent). Next: build + fix, unit tests, shots, FL render checks, land.

## 1. Dashboard: labels, 36+ controls, its own outputs as targets

- **Labels** (`ui/Controllers164.cpp` `SurfacePanel::drawLabel / labelRoom`): FL draws a control's caption in full,
  running past a narrow skin. LYNX now gives a caption the free room beside its control (up to the next control on the
  same rows, at most 3 × the control's width), shrinks the font down to 8.5 px, then narrows it (drawFittedText,
  0.6 horizontal scale) — no "Stere..." any more while the text can be read. Buttons fit inside themselves. [SHOT §5]
- **Limit**: `surface::maxOutputs` 32 → **1024** for a Dashboard (FL's Dashboard keeps up to 1024 records,
  REA_rodada164a §5) — FL's Peavey preset now imports all **36** controls [DATA, TEST `dashboard165_many_controls`];
  a Control Surface (a mixer effect, one parameter per output) is capped at `maxEffectParams` (160)
  (`surface::maxSurfaceOutputs`; `effectInfo (controlSurface)` lists exactly 160 parameters).
- A Channel Rack Dashboard's outputs are now automation / link targets (`channelParam` k = output k:
  `Song::targetValue / setTargetValue`, `RackState::buildAutomationTables`, `describeAutomationTarget`); an FL
  automation / link on Dashboard parameter p ≥ 1 maps to LYNX output `firstOutput (p − 1)` at import (parameter 0 is
  FL's patch selector) [DECOMP 0x7b67c0: parameter k = record k − 1].

## 2. Dashboard MIDI (FL's XP-30 preset)

FL [DECOMP Dashboard_x64.dll]:
- `0x7b79b0` sends one record: nothing when its number < 0 (bit 31 = deactivated); the value is clamped to the
  record's min…max; **type 0** = CC `B0|ch, number, value` (max ≥ 128: high 7 bits then low 7 bits on the *same*
  CC); **types 1 / 2** = RPN / NRPN: the select pair from the table at `0x81fefe` (101/100, 99/98) with number
  >> 7 and & 127, then data entry CC 6 (and CC 38 for the low half when max ≥ 128); **type 3** = channel pressure
  `D0|ch, value`; **type 5** = note: `90|ch, number, value`, value 0 → `80|ch, number, 0`; type 4 = controller output
  (no MIDI).
- `0x7b8100` / `0x7b82e0`: the patch selector (parameter 0, program + 1, 0 = none) sends bank select CC 0 / CC 32
  (header words 2 / 3, −1 = none) then the program change. `0x7b7d40`: the port is header word 5 (`port << 24 |
  channel`), the channel header word 0. `0x7b5f70`: everything is sent when the plugin loads and when a value
  changes.
- [DATA] XP30.fst: header (channel 0, bank MSB 84, LSB 4, port 0), parameter 0 = 29 (program 29), six type-0
  records CUTOFF 74, RES 71, ATT 73, DEC 72, REV 91, CHOR 93, range 0…127, all active. Peavey / MIDI Out / Sliders /
  PhatBoy: type-4 controller outputs only.

LYNX:
- `SurfaceControl::midi / midiNumber / midiLow / midiHigh` (saved in .lynx as 4 extra fields of a surface control);
  `surface::DashboardMidi` (port, channel, program, bank MSB/LSB) read / written in FL's state layout
  (`dashboardMidi`, `writeDashboardMidi`; a new Dashboard gets a short FL-shaped header); FL's records import with
  their MIDI type, number and range.
- `DashboardInstrument` keeps per output the wanted message (packed 64-bit key, atomics: build thread → audio
  thread, no locks, no allocation) and `takeMidiMessages` sends what changed (bank + program first, then each
  control; at most 192 per block, the rest next block). Automation / links (`setParameter`) move the output and its
  MIDI. The rack (`RackRenderer::processChunk`) delivers them to every plugin whose MIDI input port is the
  Dashboard's port (`midiTargets`, the same routes as MIDI Out); a Dashboard's notes go to that port too.
- UI: Dashboard header **MIDI** button (port, channel, program, bank); in Edit mode a control's right-click menu
  **Send MIDI ›** Controller output only / CC / RPN / NRPN / Channel pressure / Note (number, lowest, highest; > 127
  = 14-bit); the edit-mode badge shows `1 CC74`.
- [TEST] `dashboard165_midi_messages`, `dashboard165_midi_settings_state` (XP-30's header and CCs),
  `dashboard165_instrument_sends_its_midi`, `dashboard165_many_controls` (300 CC controls over several blocks).

## 3. Voltage Controller: its own knobs as targets

FL can link / automate the Voltage Controller's 16 parameters (0…7 offsets, 8…15 scales, 0…65536) [DATA fl_dash.flp,
REA_rodada164f-flimport §1.2: a Dashboard output linked to offset L / R]. LYNX: `voltage::paramCount / paramName /
paramValue / setParamValue`; the editor's offset / scale knobs carry `channelParam` targets (right-click: link /
automate); `VoltageInstrument::setParameter` overrides the value on the audio thread until the song's own value
changes (a knob moved by hand wins again); `.flp` links to parameters ≥ 16 are dropped.
[TEST] `dashboard165_and_voltage_targets`, `voltage165_linked_offset_moves_the_output` (FL's render values −0.5 / +0.75).

## 4. FL's curve point law → X-Y-Z Controller curves (`core/FlCurve.{h,cpp}`)

[DECOMP Fruity X-Y-Z Controller_x64.dll] `0x782750` prepares a point (mode byte at +0xC, f32 tension at +8),
`0x784140` the step count of modes 3…6, `0x7858b0` evaluates a segment; the Ghidra copy is
`/mnt/d/lynx-work/dash165/ghidra-xyz`, decompiles `x3.c`, `x4.c`. The law (tension t in −1…1, x 0…1):
- modes 0 / 1 single / double curve: k = 1000001^|t| − 1; t > 0: 1 − (e^((1−x) ln(k+1)) − 1)/k, t < 0: (e^(x ln(k+1)) − 1)/k
  (double = two mirrored halves); 2 hold (0); 3 stairs, 4 smooth stairs, 5 pulse, 6 wave with
  n = round (0.001 (1000001^(1 − 0.6|t|) − 1)) (+1 for stairs at t ≥ 0); 7 / 8 single / double curve 2
  (x − (u⁶ − u) t); 9 half sine; 10 smooth (needs the neighbours — LYNX draws it straight and leaves it out of the
  menu); 11 / 12 single / double curve 3 (hyperbolic, a = max (0.9|t|, 2^−16)); zero tension is straight in modes 0, 1,
  7, 8, 9, 11, 12 (mask 0x1B83).
- It is the same law as FL's WaveShaper map that LYNX already null-tests against FL (core/FruityShapers,
  REA_rodada51 §2.4): [TEST] `flcurve165_matches_the_waveshaper_law` compares the two implementations on every
  mode × 9 tensions (worst difference printed, ≤ 2 / 32767).
- X-Y-Z: new parameters 14…16 = each axis' curve mode (value = mode / 12); the X-Y-Z's Tension is now FL's point
  tension (param 0.5 = 0, shown ±100 %); the output is `min + (max − min) · shape (mode, tension, position)`. The
  X-Y Controller keeps its own law (REA_rodada17 §4). Import: the output curve's second point's mode and tension
  [DATA: FL presets bend them by 0.65, ±0.28, −0.725]; the input curves are identity in every FL file seen [DATA].
- UI: X-Y-Z editor, a **Curve** box next to the X / Y / Z tabs; the curve view samples per pixel (stairs stay sharp).
  [TEST] `flcurve165_ends_straight_and_counts`, `xyz165_curve_modes`, `xyz165_fl_state_curve_tension_and_mode`.

## 5. Voltage Controller velocity curve and links vs FL's renders

- **Velocity** [FL render vs RUN]: FL's own render of `/mnt/d/lynx-work/patcher164v/fl_volt10.flp` (main L = velocity,
  main R = pitch; REA_rodada164f-flimport §2) against LYNX's render of the same .flp through the whole import + rack
  path (`build-local/renderwav`, which normalises the peak 1.0 to −1 dBFS, × 0.891 — undone below):

  | velocity (FL) | 1 | 16 | 32 | 64 | 100 | 128 |
  |---|---|---|---|---|---|---|
  | FL main L | 0.00781 | 0.12500 | 0.25000 | 0.50000 | 0.78122 | 0.99997 |
  | LYNX main L (÷ 0.891) | 0.00781 | 0.12498 | 0.24997 | 0.49997 | 0.78119 | 0.99995 |

  and main R (1 V/oct pitch) −0.2 … +0.3 the same. The curve is FL's: **linear, velocity / 128** — nothing to change.
- **Links** [FL render vs RUN]: `fl_dash.flp` (a Dashboard's outputs 3 / 7 linked to the Voltage Controller's offsets
  L / R) renders L = −0.500, R = +0.750 in FL. LYNX rendered **silence**: the live links were imported right (debug
  print: output 3 → offset L, output 7 → offset R) but `RackRenderer::updateAutomation` applied plugin / instrument
  parameters in **pattern mode** only for pattern event lanes — every controller link to a parameter (Dashboard →
  Voltage, Peak / Envelope / X-Y → a plugin's parameter) did nothing while a pattern played. Fixed: in pattern mode
  the live links' bindings are applied too (`applyPluginParams (…, andLive)`); song mode was already right.
  [TEST] `dashboard165_links_and_midi_through_the_rack_in_pattern_mode` (both modes, plus the Dashboard's CC reaching a
  plugin on its port).
