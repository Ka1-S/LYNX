# Round 164f (FL side) — Dashboard import, Voltage Controller pitch / velocity, X-Y-Z curves

Clean-room study of FL Studio 2026's own `Dashboard_x64.dll` (headless Ghidra, project copy in
`/mnt/d/lynx-work/patcher164v/ghidra/dash`), FL's Dashboard presets (`Data/Patches/Plugin presets/Generators/Dashboard`)
and FL command-line renders (`FL64.exe /R /Ewav`, under the FL lock) of test projects written for this round
(`/mnt/d/lynx-work/patcher164v/fl_*.flp`, renders in `flout/`). No FL code, graphics or presets are copied.

Evidence markers: **[DECOMP addr]** Ghidra, **[DATA]** FL's own files, **[FL render]** FL's render of a test project,
**[TEST]** LYNX unit tests, **[HYP]** not checked against FL.

## 1. Dashboard

### 1.1 State (0xD5)

- i32 version 2, 6 × i32, i32 n + n × i32 **parameters** (raw values), i32 m + m × **81-byte records**, then the
  skinned panel (REA_rodada164a §5) [DATA, DECOMP 0x7b50a0].
- Record [DECOMP 0x7b50a0, 0x7b6eb0, 0x7b8160]: `string[47]` name (+0x00), `string[15]` short name (+0x30),
  i32 **number** (+0x40: low 30 bits = the CC / output number, bit 31 = deactivated, −1 = none), i32 **type** (+0x44:
  4 = controller output with range 0…65536; 0 = CC 0…127; 3 = program; 5 = 0…127; others 14-bit), i32 min (+0x48),
  i32 max (+0x4C), u8.
- Parameter 0 is the patch selector; **parameter k = record k − 1** (FL's parameter names come from the records,
  `FUN_007b67c0` with FPN_Param) [DECOMP 0x7b67c0].
- Panel controls: u8 type, string skin, i32 length + data (string name, string caption, i32 x, y, w, h, …), 16 bytes.
  Type names [DATA strings]: 0 Digit, 1 Slider, 2 Wheel, 3 Panel, 4 Switch, 5 Label, 6 Image, 7 Selector, 8 Patch
  selector, 9 Page selector.

### 1.2 Which control is which record

- The panel keeps two lists: every control (+0x510) and the **value controls** (+0x508): a new control joins the
  value list when its type's bit is set in the mask 0x9F = types 0, 1, 2, 3, 4, 7 [DECOMP 0x7ab6f0, data 0x7aba20];
  labels, images and the selectors don't (class 0x79b458, mask 0x360 at 0x7aba39).
- **Record k = the k-th value control** (in panel order): the record loop reads `valueList[k]` [DECOMP 0x7b8160,
  0x7a81e0]; every FL preset confirms it by name (XP-30: CUTOFF, RES, ATT, DEC, REV, CHOR are the sliders, the patch
  selector and the three labels sit between them; MIDI Out: Wheel1…6, Slider1, Wheel7…12) [DATA].
- **Controller outputs**: FL names output N after the type-4 record whose number is N (FPN_OutCtrl) [DECOMP 0x7b67c0],
  and sends a type-4 record's value on its number. Checked with FL's render of `fl_dash.flp` (a Sliders Dashboard whose
  records 3 and 7 swap numbers, values 0.25 / 0.875, linked output 7 → Voltage Controller offset L and output 3 →
  offset R; the Voltage Controller's sources off): FL renders L = −0.500, R = +0.750 — output 7 carries record 3's value
  [FL render]. Value = (parameter − min) / (max − min).

### 1.3 LYNX

- `surface::parseFlDashboard` (core/ControlSurface.cpp): the value controls become LYNX's Dashboard controls (Slider →
  slider, Switch → check box, Wheel / Digit / Panel / Selector → knob) with the record's name and value; FL's places
  scaled so each LYNX control fits in its FL box scaled the same way (≤ × 3; no overlaps when FL had none), on LYNX's
  10 px grid; at most 32 outputs (Peavey's 36 controls keep 32).
- `.flp` import: a "Dashboard" channel becomes `InstrumentKind::dashboard` (FL's state kept in `pluginState`); its
  controller links become live links with FL's output number mapped to the panel's (`flDashboardOutputToLynx`); the
  import report counts it as converted.
- Not reproduced: records of other types send MIDI (CC, program…) to FL's MIDI out port — LYNX's Dashboard has no MIDI
  output; the patch selector, labels and images are dropped (LYNX's panel has no decorations).
- Tests [TEST]: `dashboard164f_fl_presets` (Sliders, XP-30, MIDI Out, Peavey from the installation),
  `dashboard164f_import_with_link` (the swapped-number case of the FL render, a live link to a channel volume).

## 2. Voltage Controller: 1.2 V/oct and velocity

FL renders of `fl_volt10.flp` / `fl_volt12.flp` / `fl_volt12b.flp` (main L = velocity, main R = pitch, offsets 0,
scales × 1, ±10 V; notes C3…C7 / C5, C#5, D5, B4, A#4, D#5, A4) [FL render]:

| | FL |
|---|---|
| velocity 1, 16, 32, 64, 100, 128 | 0.0078, 0.125, 0.25, 0.5, 0.781, 1.0 — **linear, velocity / 128** (LYNX already) |
| 1 V/oct, C3…C7 | −0.2, −0.1, 0, +0.1, +0.2, +0.3 (LYNX already) |
| "1.2 V/oct", C5, C#5, D5, B4, A#4, D#5, A4 | 0, +0.1, +0.2, −0.1, −0.2, +0.3, −0.3; an octave away clips at ±1 |

FL's "1.2 V/oct" gives **0.1 of full scale (1 V) per semitone** — 10 × what the name says, exactly as its code
computes it (REA_rodada164a §6). LYNX now follows FL (`voltage::pitchValue`); before it gave 0.01 per semitone.
Test: `voltage164f_fl_render_pitch_and_velocity`.

## 3. X-Y-Z Controller curves

Every X-Y-Z Controller state in FL's files has an **identity input curve** on all three axes; the output curves have
two points whose ends LYNX already maps to Min / Max, and some presets bend them (point tension 0.65, ±0.28, −0.725)
[DATA]. Mapping that tension needs FL's envelope-point tension law (a different law from the X-Y Controller's tension
LYNX implements), not decompiled yet — left as it was.

## Where I stopped

- Done: Dashboard state → LYNX panel + live links (mapping proven by an FL render), Voltage Controller "1.2 V/oct" made
  FL's, velocity confirmed linear; tests above.
- Left: Dashboard MIDI-sending records (CC / program to a MIDI port), the X-Y-Z output-curve tension (needs FL's
  envelope tension formula), LYNX links to the Voltage Controller's own parameters (FL can link its offsets; LYNX's
  `channelParam` binding skips the voltage kind).
- Screenshots not taken: the coordinator paused all Windows desktop use (the user was on the PC). `LYNX.exe
  --controllershot <prefix>` (build-win-patcher164v) now also writes `-dashboard-fl-<preset>.png` for each FL Dashboard
  preset of this PC's FL install — run it (and screenshot FL's Dashboard with the same preset) once the desktop is free.
