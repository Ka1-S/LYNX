# Round 162e (REA + features) — issue #7: Patcher plugin list, MIDI ports on the I/O nodes, Envelope Controller sections

Issue #7 (Ivan-Spoda, with FL vs LYNX screenshot pairs) asks for three things: (1) every plugin LYNX has in the
Patcher's plugin list (MIDI Out and Envelope Controller could not be added), (2) MIDI port filtering on the Patcher's
I/O nodes, (3) the Envelope Controller's missing sections (Keyboard / Velocity / Mod X / Mod Y mapping, Random).

Evidence markers: **[DECOMP]** FL's `Patcher_x64.dll` (the same binary as round 141, own headless Ghidra project
`/mnt/d/lynx-work/patcher162/ghidra`), **[SHOT]** the reporter's FL screenshots (`/mnt/d/lynx-work/patcher162/shots`),
**[TEST]** LYNX's tests, **[HYP]** inference. Clean-room: behaviour and formats only, no FL code copied.

---

## 1. FL's Patcher: Add plugin and the I/O nodes' pins

- **Add plugin** [SHOT s1]: a right-click on the map opens *Show plugin picker / Add plugin › …*; *Add plugin* is one
  long menu in columns: **Generators** (Open plugin picker, Browse plugin database, More…, then Controller, Drum,
  Instrument, MIDI, Misc, Patcher, Sampler, Synth classic, Synth special, Visual) and **Effects** (Open plugin picker,
  Browse plugin database, More…, then Controller, Delay reverb, Distortion, Dynamics, Filter, Flanger, Misc, Patcher,
  Visual). Every installed plugin and FL's own appear, controllers included (Envelope/Keyboard Controller, MIDI Out,
  Formula/Peak/X-Y Controller…).
- **The I/O node menu** [DECOMP `FUN_0083ce50`, `FUN_00839ab0`; SHOT s3/s5]: a node's menu has *Disconnect all*
  and, per kind of pin, a submenu (*Events*, *Audio*, and *Parameters* on plugin nodes) built by `FUN_00839ab0`:
  **Activate all, Deactivate all, Deactivate unused**, *Add one* (kinds that grow, e.g. audio), *Last tweaked*
  (parameters), a separator, then every possible pin as a **check item**.
- **Which pins exist** [DECOMP `FUN_0094fb20`]: for "From FL Studio" / "To FL Studio" FL asks the host for its audio
  inputs / outputs (names from host calls 0x33 / 0x34; bus 0 = "Audio input" / "Audio output") and then lists
  **256 MIDI pins, "MIDI port 0" … "MIDI port 255"** (or the port's own name when one is set). The "From" node also has
  the **Notes** pin (the channel's notes).
- **Saved pins** [DECOMP `FUN_00952560`]: each active pin is a `1400` record (28 bytes: `u64 id`, `i32 kind` (0 audio,
  1 MIDI), `u32 flags` (bit 0 = on the "From" node, bit 2 = …), `u8`, `i32 index` = bus or **MIDI port number**) with
  its name in `1402`. A MIDI pin with no name is "MIDI Port %d"; the "Notes" pin of the From node is built in and not
  re-created from the file. So a pin that is activated but has no cable is still in the file — the selection survives.

## 2. LYNX

### 2.1 Add plugin (`ui/PatcherEditor.cpp` showAddMenu)
Right-click › *Open plugin picker...* and *Add plugin ›* with FL's two sides in columns: Generators (Open plugin
picker, More… = the installed VST/VST3/CLAP instruments, then Controller: Envelope Controller, Keyboard Controller;
Drum: LYNX's drums + FL's drum engines; MIDI: MIDI Out; Misc: BooBass, FL Keys, LYNX Soundfont; Patcher: Patcher;
Sampler: Sampler (choose file)…, Granulizer; Synth classic / special: every FL engine LYNX reproduces + LYNX Synth)
and Effects (Open plugin picker, More…, then every effect of the mixer's list in FL's categories, the Formula and Peak
Controllers under Controller, a nested Patcher under Patcher). Not in LYNX (so not listed): FL's Layer, Dashboard,
Fruity Voltage Controller, VFX plugins, Visual plugins, X-Y / Control Surface as new nodes; "Browse plugin database"
(LYNX's Browser has the Plugin database, but the Patcher window can't reach it yet).

### 2.2 Controllers and MIDI Out inside a Patcher (`core/Patcher.cpp`)
- Envelope Controller node: notes in, 8 control outputs; Keyboard Controller node: 1 control output; MIDI Out node:
  notes in, a **MIDI out** pin, no audio. Their editors open from the node (double-click / Open editor), editing the
  node's own settings (`makeMidiOutEditor (engine, locate)`, `makeEnvelopeControllerEditor (engine, locate)`).
- MIDI between nodes: a MIDI Out node passes what reaches its notes input to whatever its MIDI out pin is cabled to
  (instruments, "To LYNX" MIDI ports), on its MIDI channel.

### 2.3 MIDI ports on the I/O nodes
- `PatcherDoc::fromMidiPorts / toMidiPorts / fromNotesHidden`: the activated pins (a pin with a cable is always
  shown). Saved in `.lynx` (`fromMidiPorts`, `toMidiPorts`, `fromNotesHidden`).
- Pin numbering: "From LYNX" Notes = port 0 of the wire, MIDI port N = `patchMidiPortPin (N)` = N + 1; "To LYNX"
  MIDI port N = N.
- Editor: right-click "From LYNX" / "To LYNX": *Disconnect all*, *Events ›* (Activate all, Deactivate all, Deactivate
  unused, Notes, MIDI port 0…255 as check items in columns; deactivating a pin removes its cables) and *Audio ›*
  ("To LYNX": Add one, Deactivate unused and the outputs; a mixer slot's "From LYNX": its audio input and side-chain).
- Runtime (`RackState::buildMidiRoutes`): a source that sends on port N (a MIDI Out channel; a generator Patcher's
  "To LYNX" MIDI port N) now reaches, besides the plugin channels whose MIDI input port is N, **every Patcher whose
  "From LYNX" MIDI port N pin is cabled** — generator Patchers (at the note's sample) and mixer-slot Patchers (at the
  chunk's start). A Patcher that passes port N on to its own "To LYNX" MIDI port M (directly or through a MIDI Out
  node) makes the source reach the targets of M too (up to 4 levels). Each route keeps its port and MIDI channel
  (`midiRoutesOut / midiRoutesIn / effectMidiRoutes`).
- Import (`FlpImport.cpp`): every MIDI pin of the I/O nodes becomes an activated port (`flPatcherMidiPinPort`:
  "Notes" = −1, "MIDI port N", or the pin's port index), MIDI cables from "From" pins keep their port, and MIDI
  cables out of a MIDI Out node are kept.

## 3. Envelope Controller sections

Done in the 162e fork (commit `16af6c5b`); the full REA write-up is `docs/analise/REA_rodada162e-envelope.md`.

- **FL** [DECOMP `Fruity Envelope Controller`]: note on `FUN_008af660` (key = (cents+6000)/100, one voice per output,
  last note wins, voice factor = velocity map × random map); tick `FUN_008afec0` (base + envelope × amount; LFO adds in
  bipolar mode, multiplies by 1+2·lfo·amount in unipolar; keyboard / X / Y maps combined, clipped 0..1; with no voice
  and *Continuous output*, the output is base with the X/Y maps); LFO class "MLFO": rate (10001^(speed/65536)−1)/625
  cycles per second or per beat (`FUN_007de390`), shape `FUN_007de1d0`/`FUN_006223f0`/`FUN_00622620`, start phase
  `FUN_007de6f0`; ADSR knobs `FUN_008ae740` (SUS as a level shift is [HYP]).
- **LYNX**: `ControllerArticulator` gained the LFO (tempo, global, speed, tension, skew, pulse width, phase, amplitude
  curve), the 5 maps (keyboard, velocity, Mod X, Mod Y, random), ADSR knobs and the decay point;
  `EnvelopeControllerData` gained global Mod X/Y and continuous output; the state is version 2 (v1 and FL states still
  load, all 22 factory presets import). The editor has FL's dropdown (Envelope / LFO / Keyboard / Velocity / Mod X /
  Mod Y mapping / Random), Base/Env/LFO knobs, Bipolar, Continuous output, X/Y pad, key-range strip, ATT/DEC/SUS/REL
  + Time in beats, and the LFO page with a wave preview. The same editor opens for Envelope Controller nodes inside a
  Patcher. Tests: `tests/EnvController162Tests.inc`.
- Not done: automating Mod X/Y from the rack, LFO flag bits 2–3, per-tick (LYNX: per-block) update.

## Where I stopped

Batch 1 (Add plugin menu, MIDI port pins + routing + import, MIDI Out / controller nodes, `--patchershot` menu
shots, tests `tests/Patcher162Tests.inc`) landed. Batch 2 = the Envelope Controller sections (§3) and the Envelope
Controller node editor inside a Patcher. Open: "Browse plugin database" from the Patcher, FL's Layer / Dashboard /
VFX / Visual / X-Y nodes, instrument editors of built-in engines inside a Patcher, MIDI output pins on plugin nodes,
automating the EC's Mod X/Y, sample-accurate MIDI into mixer-slot Patchers.
