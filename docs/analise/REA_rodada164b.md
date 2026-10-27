# Round 164b — FL's VFX note effects (vfx164)

Goal: FL Studio's **VFX** plugins are note-level processors. They sit in a Patcher between "From FL Studio" and the
generators, or on a Channel Rack channel, and they change, split, create or animate notes. This round lists the family
and builds a LYNX equivalent of each one. Each equivalent gets:
- its own editor;
- FL's presets;
- .flp import;
- entries in the plugin picker, in the Patcher's "Add plugin" columns and in the Browser;
- tests, screenshots and pt-BR strings.

Evidence markers:
- [DECOMP fn]: Ghidra decompile of FL's plugin DLL (clean-room: behaviour only, our own code);
- [PRESET]: FL's factory presets / plugin database states, parsed;
- [EXEC FL]: seen in a running FL Studio 2026;
- [TEST]: LYNX unit test;
- [RUN]: measured with LYNX;
- [HYP]: inferred, not proven.

The full specs written for this round are in the job folder (`tmp/vfx164/spec-*.md`, not in the repo). The sections
below record what LYNX does and where it differs.

## 1. The family

FL Studio 2026 installs seven VFX plugins: `Plugins/Fruity/Effects/VFX *`, and `Data/Patches/Plugin database/Effects/Patcher/VFX *.fst`.
VFX Pads is not installed.

| FL plugin | What it does | LYNX | FL presets |
|---|---|---|---|
| VFX Key Mapper | each input pitch class → a set of output notes (transpose, scales, chords, harmony); multimode all / gain-compensated / random / cycle up / cycle down; base key, offset, pitch slide | ✅ processor + editor | ✅ 33 factory .fst read and written back byte for byte [TEST] |
| VFX Color Mapper | note colour → voice output 1…16 and a new colour | ✅ | ✅ (Default) |
| VFX Level Scaler | per property (velocity, release, fine pitch, pan, Mod X, Mod Y) a transfer curve: center, multiply, offset, tension, humanize (subtract / spread / add) | ✅ (applied at note-on; release at note-off) | ✅ FL's curves to 1e-6 [TEST]; LYNX's own presets (humanize, softer, compress…) |
| VFX Keyboard Splitter | 16 zones, each a keyboard map and a velocity map (FL's TMMap curves), transpose, random, absolute; one output per zone | ✅ (except Manual mapping) | ✅ factory presets read; the zones are written back equal [TEST] |
| VFX Envelope | 16 articulators (note colour + key range) × 5 targets (velocity, pitch, pan, Mod X, Mod Y), each with an envelope and an LFO, as in the Envelope Controller | ✅ (loop points ignored, see §4) | ✅ factory presets round-trip [TEST] |
| VFX Sequencer | FL's port of BlueARP: a bank of 128 step programs (scale step, octave, step type, key select, velocity, gate lanes × 64 steps) that turn held notes into arpeggios / sequences | ✅ engine + editor (chains not played, see §4) | ✅ FL's factory bank (plugin database state) read and written back byte for byte; "Factory bank" in the preset menu [TEST] |
| VFX Script | Python 3.12 note scripts (`import flvfx`) | ⚠️ the script and its control values are kept and editable; **LYNX does not run Python**: notes pass through | ✅ 89 factory scripts read and written back byte for byte [TEST] |

## 2. Where they live in LYNX

- **Engine**: `src/core/Vfx.h` / `Vfx.cpp` (states, the processors of six kinds), `VfxSequencer.cpp` (the
  Sequencer), and `VfxDetail.h` (byte reader and the lock-free settings hand-over).
  - The processors never allocate or lock on the audio thread.
  - New settings are handed over through an atomic pending/retired pair, the same pattern as the Patcher graph.
  - The Envelope's runtime is about 1.7 MB, so it is built on the heap on the UI thread.
- **State**: `ChannelDef::pluginState` = "LVFX" header (kind, the channel's output MIDI port), then FL's own 0xD5 bytes.
  An .flp / .fst state therefore loads unchanged, and LYNX saves it unchanged.
- **Patcher**: VFX nodes take the notes of "From LYNX" or of another VFX node, and their outputs cable to generators
  or to other VFX nodes. Output pins:
  - Color Mapper and Keyboard Splitter have 16 outputs. Four are shown, as in FL, and the zone names become the pin names.
  - The other kinds have one output.

  Voice properties that change over time (the Envelope, the Key Mapper's pitch slide) reach the instruments through
  `Instrument::voiceProperty`. The Synth and the Sampler implement it.
- **Channel Rack**: "VFX (note effects)" submenu. A VFX channel sends output k to MIDI port `port + k`, and its
  editor has an "Output port" control and "Send to a plugin...".
- **.flp import**: VFX channels and Patcher VFX nodes, with their states, ports and cables.
- **Lists**:
  - the plugin picker (vendor LYNX, ids `lynx:vfx:<kind>`);
  - the Patcher's Add plugin › Patcher column;
  - the Browser's Generators › "VFX (note effects)" folder.
- **CLI**: `LYNX --vfxshot <prefix>` saves an editor per kind, plus a Patcher with Color Mapper → 3 synths and its menus.

## 3. Behaviour notes (what FL does, rebuilt)

- **Velocity ↔ volume**: FL's law vol = (11^(1.28·v) − 1) / 10, so velocity 100/128 is volume 1.
  - Keyboard Splitter map gain: g(m) = (11^m − 1) / 10 [DECOMP].
  - The Sequencer's "MIDI velocity" is volume × 100 [DECOMP].
- **Key Mapper**:
  - row = (key − base + 12) mod 12;
  - out = key − row + value + offset;
  - the cycle modes walk the row's stored order;
  - pitch slide: the new note starts at the played pitch and glides at 0.01^(beats·192/slide) per block [DECOMP, TEST].
- **Level Scaler**:
  - T(x) = clamp(sign(d)·shape(|d|·S)·mul + off) around the center;
  - tension uses (8193^t − 1);
  - the identity skip happens when Multiply = 8192, Offset = Tension = Humanize = 0;
  - humanize draws one random value per note and tab [DECOMP, TEST].
- **Keyboard Splitter**:
  - every active zone whose gain > 0 plays a copy on its output;
  - random zones: one is picked, weighted by g(map);
  - absolute: the velocity map gives the volume [DECOMP, TEST].
- **Envelope**: the Envelope Controller's articulator code (FlSynths.h `controllerValue` …) drives each target.
  - Pitch: ±48 semitones at 100 %, Init.Pitch clamped to 0…12000 cents.
  - Velocity: multiplicative, 0…4.
  - Pan / Mod: additive [DECOMP, TEST].
- **Sequencer** (BlueARP):
  - step = floor(position / step length) mod steps, swing on step pairs;
  - key list: held keys in the chosen order (pitch, played, velocity, chord normalized / as played, with the chord
    table of 38 chords), missing keys N1…N5 filled (cyclic, first, last, fixed);
  - pitch: scale forcing (snap down, then move by scale steps), transpose, output wrap;
  - per step: Off / Rest / Note / Tie / Chord / Random, octave bits, velocity mode, random velocity / gate / start;
  - a rest lengthens the previous note by a step, and a tie stretches it to 1.25 steps (legato);
  - restart on beat / key / first key / play;
  - stopped transport: its own clock, kept for one second after the last key [DECOMP].
  - Notes start at their exact sample. FL starts them at the processing block, so LYNX is tighter.

## 4. Differences and limits (honest list)

- **VFX Script does not run**. Python is not embedded, so the script's notes pass through unchanged and the editor
  says so. The script text and control values are kept, editable and saved, and FL still runs them on that project.
  The next steps are written in "Where I stopped".
- **Sequencer chains** (the 16 chains of 8 programs) are kept in the state but not played. No FL preset uses them; only
  the plugin database bank defines 8. Also not reproduced:
  - incoming program changes;
  - sustain pedal / CC filters (LYNX's VFX events carry notes only);
  - Smart bend's "cut all but the first note" on change.
- **Envelope**: the loop points of its envelopes are ignored, so FL's "Pumping" preset does not loop. A free (non-tempo)
  envelope's time is in seconds.
- **Level Scaler** is applied at note-on; FL re-evaluates it every tick, so automating it moves held notes.
- **Keyboard Splitter**:
  - "Manual mapping" (press keys to map zones) is not implemented.
  - When no random zone wins, FL plays zone 1. LYNX plays no random zone.
- **Voice properties over time** (Envelope, pitch slide) reach instruments inside a Patcher only. On a Channel Rack VFX
  channel the notes are routed by MIDI port and the property changes are dropped.
- VFX outputs to the Patcher's "To LYNX" MIDI ports are not supported.
- The Level Scaler's "Event persistence" lollipop display is not drawn.

## 5. Verification

- `tests/Vfx164Tests.inc` [TEST]: 19 tests. Among them:
  - FL's factory presets for Key Mapper / Splitter / Envelope / Script / the Sequencer bank, round-tripped;
  - the Level Scaler's test vectors;
  - the Sequencer's FL vectors T1–T12: up-arp of program 3, missing keys, thru, scale forcing, swing, output wrap,
    rest / tie, chord normalized, stopped clock;
  - Patcher and channel routing.
- **CPU** [RUN]: the Sequencer runs 1 s of audio (4 keys, chords on every step, 140 bpm) in 0.74 ms, 0.07 % of one core.
  The other processors only work at note events. The Envelope works per block for the voices it matches.
- Screenshots: `--vfxshot` saves an editor per kind, plus the Sequencer with FL's factory bank and the Script with
  FL's "Snap To Scale". They were compared with FL 2026 running (`/mnt/d/lynx-work/vfx164/shots/`, sequencer/ and
  script/) [EXEC FL]. The layout follows FL's, drawn with our own controls:
  - program selector, Latch, pages, Page lock, shift;
  - the big grid of the selected lane over six overview lanes;
  - MODE / INPUT / ARPEGGIATOR / OUTPUT panel.
- Used in the Windows app like a user [RUN]:
  - Channel Rack › + › VFX (note effects) › VFX Sequencer adds the channel and opens its editor;
  - clicking a Note select cell sets the step (N2 → R);
  - picking the Velocity lane shows its bars;
  - a click on a bar sets the velocity (96 → 42), and the overview row follows.

## Where I stopped

Landed: the seven kinds, each with:
- state;
- processor (the Script passes notes through);
- editor;
- FL presets;
- .flp import;
- picker / Patcher / Browser entries;
- tests;
- pt-BR strings.

Left for later, in order of value:
1. **VFX Script running Python.** This needs an embedded CPython 3.12 (about 30 MB, its own licence and packaging on the
   three platforms) and a clean-room `flvfx` module: voices, `onTriggerVoice` / `onReleaseVoice` / `onTick`, and
   `createDialog` surface controls. The state and the editor are ready for it. `ScriptSettings::parameters()` already reads
   the control values. Running a script must stay off the audio thread (a worker with a lock-free event queue), and a
   slow script must not glitch audio.
2. **Sequencer chains and program changes**: the 16 × 8 chain tables are parsed (`SequencerSettings::chains`); an
   incoming program change would select the program or chain step.
3. **Envelope loop points** (FL's "Pumping" preset).
4. **Level Scaler re-evaluated per tick** (automation on held notes) and the "Event persistence" lollipop display.
5. **Keyboard Splitter Manual mapping**, and FL's zone-1 fallback for random zones.
6. **Voice property changes** on Channel Rack VFX channels (now Patcher only) and VFX → "To LYNX" MIDI ports.
