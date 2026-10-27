# Round 164a (REA + features) — Layer, X-Y / X-Y-Z Controller, Control Surface, Dashboard, Voltage Controller

FL's Layer generator and its controller plugins, reproduced clean-room: behaviour and file formats from FL's manual,
FL's own projects and a static decompilation of FL Studio 2026 (`FLEngine_x64.dll`, image base 0x400000, and the
plugin DLLs Fruity X-Y Controller, Fruity XYZ, Fruity Voltage Controller, Dashboard). Nothing of FL's code, graphics or
presets is copied; LYNX draws its own editors.

Evidence markers: **[DECOMP addr]** headless Ghidra (one process at a time, project and dumps in
`/mnt/d/lynx-work/layer164/`), **[DATA]** FL's own `.flp` files and presets (`Data/Demo projects`, `Data/Templates`,
`Data/Patches/Plugin presets`), **[MAN]** FL's manual, **[TEST]** LYNX's unit tests, **[HYP]** inference not yet checked
against FL running.

---

## 1. Layer channel

### 1.1 FL

- **Children** [DECOMP 0xF46490]: each event `0x5E` (u16 channel index) appends one child, in file order. *Set
  children* adds the selected channels in ascending index order [DECOMP 0xFF8E30].
- **Flags** `0x90` [DECOMP 0xF46490, 0xD29240]: bit 0 Random, **bit 1 Crossfade**, **bit 2 Sequential** (PyFLP's
  "bit 3 = crossfade" is wrong). The UI keeps Random and Sequential apart; Random is tested first. Toggling Sequential
  resets its step counter. In FL's demos every layer has flags 0 except RawFL channel 42 (Random) [DATA].
- **Fade** [DECOMP 0xFEA3D0]: channel parameter 10 (`Layer_FadeWheel`, 0…256), saved in `0xD7` at +0x4C. With
  Crossfade on, child i of N gets `max (0, 1 − |f·(N − 1) − i|)` (a linear triangle; one child: f).
- **Which children play** [DECOMP 0xD29240]: Random = one child, uniform (MT19937), repeats allowed; Sequential =
  `list[ctr]`, then `ctr = (ctr + 1) % N`, from 0 in list order; neither = all. Disabled children don't play.
- **The Layer's own VOL / PAN / PITCH** [DECOMP 0xD265E0]: the child voice's volume × the Layer's channel gain (FL's
  channel curve `(11^v − 1)/10`, ~−5 dB at the default 78 %), pan + Layer pan, pitch + Layer pitch + fine −
  (root − 60)·100 cents (RawFL's layer at root 56 plays its children 4 semitones up).
- **Key region** (every channel) [DECOMP 0xD29370]: `0xD7` +0x44 low, +0x48 high, inclusive; default 0 / 256 = every
  key. A note outside is dropped before the channel plays it; a Layer filters with its own region, then each child
  with its own. *Split children* [DECOMP 0xFF8F20]: child k (list order) gets root = region = layer root + k (≤ 119).

### 1.2 LYNX

- `InstrumentKind::layer`, `ChannelDef::layer` (children, random, sequential, crossfade, fade) and every channel's
  `keyLow / keyHigh` (`core/Song.h`); logic in `core/Layer.{h,cpp}`.
- **Engine** (`core/RackRenderer.cpp` `expandLayers`): a Layer's note-ons / offs / slides / chokes are copied to its
  children **inside the chunk's event list** (sample-accurate, live notes too), with pan, pitch (`layer::pitchCents`)
  and gain (`levelGain × crossfadeGain`); Random / Sequential remember the child per key so the note-off reaches it.
  The key region drops notes in `renderChannel` and on the live path. No allocation on the audio thread
  (`layerPick` sized in `prepare`). The child's gain goes to `setNextNoteGain`, else (plugins, engines without voice
  gain) the velocity carries it — as the echo's gains do.
- **Editor** (`ui/LayerEditor.cpp`, Channel settings of a Layer): Set / Show children, Layering ▾ (Split, Reset,
  Group children…, Delete children), Random / Sequential / Crossfade, Fade knob (automatable), the children list
  (colour, name, key region, crossfade level; right-click: remove, whole keyboard) and a keyboard map C1–C10 with each
  child's region and the root (right-click sets it). Every channel's Miscellaneous root keyboard got a **key-region
  ruler** (drag to set, double-click = every key).
- **Channel edits**: removing / reordering channels keeps the children (`Song::removeChannel`, `permuteChannels`);
  a used Layer marks its children used.
- **Project file**: `"layer"` and `"keyRegion"`; **.flp import**: children (0x5E, mapped through the import's channel
  map, FPC splits too), flags, Fade and every channel's key region.

## 2. X-Y Controller

### 2.1 FL [DECOMP 0x6C6D90, 0x6C6470, 0x6C5D20; REA_rodada15 §11.1, REA_rodada17 §4]

- 10 parameters (X level, min, max, tension; Y the same; speed 1024…2048; acceleration 256…1024), 2 outputs (X, Y).
- **Motion, per tick (96 PPQ)**: pull `A = (1e8^(P8/2304) − 1)·1e-8 + 1e-4` (1 at 2048), inertia
  `D = 1 − (1001^(P9/1024) − 1)·0.001` (0 at 1024); `d = D·(1 − (10^(10 (1 − dist/0.3)) − 1)·1e-10)` near the goal,
  `step = min (A (1 − D), dist)`, `vel = vel·d + u·step`, `pos += vel` (one 2-D point).
- **Output**: FL's tension curve, then `min + y (max − min)`.

### 2.2 LYNX (`core/XyController.{h,cpp}`, `EffectKind::xyController`)

The exact law above, per tick of the song's tempo. A mixer controller (no change to the sound; never smart-disabled
on a silent insert — the Formula Controller had that bug too and is fixed with it); outputs 0 X, 1 Y; meters 4…6 =
the target's position for the editor. **Import**: FL's state → parameters (the state is kept); FL links from it become
**live** links (they were constants applied once).

## 3. X-Y-Z Controller

### 3.1 FL [DECOMP 0x83B520, 0x83C0A0, 0x83C400, 0x83CBF0, 0x604A90; DATA presets]

11 parameters (X / Y / Z levels 0…65536; per-axis smoothing speed and acceleration; the global smoothing time P9;
joystick sensitivity), 3 outputs. State version 3 (560 bytes): parameters, flags, a combo value, then per axis the
joystick assignment, flags and two 80-byte curves (input, output: points of f64 x, f64 y, f32 tension, type).
**Smoothing**: `T = (17^(P9/4096) − 1)·24` ticks, `acc = P6/1024`, `n = round (acc + 0.5)` one-pole stages in series
(the first n − 1 of `T/√acc`, the last of that × (1 − (n − acc))), coefficient `0.01^(1/Ts)` per tick; snaps under
1.526e-6.

### 3.2 LYNX (`EffectKind::xyzController`)

LYNX keeps the X-Y Controller's controls per axis (level, min, max, tension) and one Speed / Accel pair: Speed s →
`P9 = 4096 (1 − s)` (0.5 = FL's default 2048, 1 = instant, 0 = FL's "Slow" preset), Accel a → `acc = 1 + 4a`
(0.5 = FL's default 3). The smoothing is FL's law above. The Z axis moves with the mouse wheel over the pad (as FL)
or its strip. **Import**: levels, P9 / P6 as Speed / Accel, each axis' output curve's ends as Min / Max (its tension
and the input curve are not mapped yet).

## 4. Control Surface

### 4.1 FL [DATA: every Control Surface in FL's demos and templates]

State: i32 version 1, then chunks `u32 id · u64 size`: 0 = group, 0x7D0 header, 0x834 control type (first i32), 0x837
name (UTF-16), 0x838 rect (4 × f32 x, y, w, h), **0x836 one per published value** (f32 value, f32 default, i32 output
index — not contiguous: read it), 0x839…0x83B text properties whose first line is the class
(`control TVectorWheel` …), 0x835 end. Types seen: 0 knob (TVectorWheel, 39), 2 slider (TVectorSlider, 4), 3 button
(TQuickBtn, 90), 5 bevel (TVectorBevel, no value), 6 label (TQuickLabel, no value).

### 4.2 LYNX (`core/ControlSurface.{h,cpp}`, `EffectKind::controlSurface`, `ui/Controllers164.cpp`)

On the Patcher's Surface model (`SurfaceControl`, round 151; new `size`): knobs, sliders, buttons (held while
pressed), check boxes and XY pads, up to 32 outputs (an XY pad gives two). Output k = parameter k (automatable); the
panel has an Edit mode (drag to move on a 10 px grid; right-click: link / automate, rename, size, delete) and "+".
**Import**: the controls with a value become LYNX controls in FL's output order, with FL's names, values and places;
bevels and labels are dropped (LYNX's panel has no decorations yet); FL links → live links with FL's output number
mapped to the panel's (`surface::flOutputToLynx`).

## 5. Dashboard

FL's Dashboard [DECOMP 0x7B50A0, 0x7B5F70; DATA presets] is the older panel generator (257 parameters, 1024
outputs; a record list and a skinned panel; no FL demo uses one). LYNX's `InstrumentKind::dashboard` is the same panel
as a **Channel Rack channel** (its controls in `channel.patcher.surface`, the values as the channel's control outputs
— linkable like an Envelope Controller's) and as a Patcher generator node. FL Dashboard **state import is not done**
(the record ↔ control mapping is not resolved).

## 5b. Voltage Controller

### 5b.1 FL [DECOMP 0x702BD0 … 0x706180; TEST FL render]
A generator that turns its notes into control voltages for modular hardware through a DC-coupled audio interface.
Eight outputs: the channel's own stereo pair (main L / R) and three more pairs sent to chosen mixer inserts. Each
output has a source (0 pitch, 1 gate, 2 trigger, 3 velocity, 4 volume, 5 pan, 6 Mod X, 7 Mod Y, 8 clock, 9 none),
an offset (0…65536, 32768 = 0), a scale (0…65536, 65536 = × 1, 0 = × −1) and a polarity; the interface range
(±10 V / ±5 V / 0…10 V, full scale 1.0 = 10 V) and pitch standard (1 V/oct, 1.2 V/oct) are global, with the clock's
PPQ and the trigger/clock pulse length (samples). `out = range (scale · (source + offset))`. State versions 1…3
(103 / 115 / 123 bytes): 16 × i32 params, 8 source bytes, enable, range, standard, 8 polarities, 3 tracks, clock PPQ,
pulse length, f64 pitch factor (v3 adds 8 per-output voice bytes). A new one starts **off** (silence).

FL render of a test project (`/mnt/d/lynx-work/layer164/fl/volt.flp` → `volt.wav`, FL64 `/R`): notes C0, C1 … C10
then C5 — main L steps −0.5 … +0.5 by exactly 0.1 per octave (1 V/oct at 10 V full scale, 0 V at C5) and **holds**
after the note ends (monophonic, last note); main R = gate × 0.5 (scale 49152) [TEST FL render]. The channel's
volume (78 %) does not scale the voltages.

### 5b.2 LYNX (`core/VoltageController.{h,cpp}`, `InstrumentKind::voltage`, `ui/VoltageEditor.cpp`)
FL's state bytes are the channel's settings (`pluginState`, written as v3); .flp channels with FL's plugin import as
this kind. The instrument copies the settings with a seqlock (UI thread `configure` → audio thread, no lock, no
allocation), tracks held notes for the last-note pitch/gate, makes sample-accurate trigger and clock pulses (clock only
while the song position moves), writes the main pair into the channel and the other pairs into the instrument's extra
mixer outputs (`auxOutputData`, the same path as a generator Patcher's "To LYNX" outputs). Off = idle (the mixer skips
it). Editor: on/off, range, pitch standard, clock PPQ, pulse length, and per output source / offset (in volts) /
scale / polarity, with the insert of each extra pair. [HYP] FL's 1.2 V/oct formula decompiles to 10 × the standard's
value; LYNX uses the standard (0.1 V per semitone). Velocity, pan and Mod X/Y use LYNX's 0…1 / −1…1 values.

## 6. Where they appear

Channel Rack ▸ add (Layer, Dashboard), the quick-add palette and the Browser's plugin database (Layer, Dashboard and
the three mixer controllers through `addableEffects`), the mixer's effect menu (controllers group), Patcher ▸ Add
plugin ▸ Generators › Controller (Dashboard) / Effects › Controller (X-Y, X-Y-Z, Control Surface) with their own
editors on the nodes (`openNodeEditor`) and their control output pins (`patcher::nodePorts`), and "Link to internal
controller" (`controllerOutputs`: X / Y / Z, every panel output by its control's name).

## 7. Tests [TEST]

`tests/Layer164Tests.inc`: every child plays; Random spread and offs; Sequential order; crossfade law; key region,
split and reset; a sampler's pitch on split; children follow channel edits; FL flag bits and Layer pitch; X-Y shape
and output; X-Y / X-Y-Z follow the goal (smooth, monotonic, arrive; faster speed arrives sooner); FL X-Y state and the
old constant output agree; Control Surface outputs / params / Patcher pins / Dashboard outputs; FL's Trap template:
its Control Surface imports with live links; Voltage Controller: FL's state, FL's render values (pitch steps, held
pitch, gate × 0.5), trigger / clock pulses, ranges, extra outputs, off = idle.

## Where I stopped

- **Voltage Controller**: per-output voice (FL v3 "mod source") is kept but LYNX follows the last note; the 1.2 V/oct
  value and the velocity curve still want a side-by-side FL render.
- **Dashboard .flp import**: the panel block parses (§5) but which control drives which record is not resolved.
- **Control Surface decorations** (bevels, labels) and its other control types (check box, digits, keyboard, XY id
  numbers) were not seen in FL's files; the X-Y-Z's curve tensions / input curve are not mapped.
- **Patcher leftovers from REA_rodada162e** (instrument editors of built-in engines inside a Patcher, MIDI output
  pins on plugin nodes, automating the EC's Mod X/Y, sample-accurate MIDI into mixer-slot Patchers): not started in
  this round.
- To check with FL running: the Layer's level at the default 78 % (decompile: × the channel curve), nested layers.
