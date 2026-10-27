# Round 164e (misc164m): the mod wheel and aftertouch on native instruments

2026-10-27. Brief: "LYNX's Sampler, Synth and the FL-engine instruments respond to CC1 (and aftertouch) where FL's do
(sampler162 left it plugin-only) — map like FL does per instrument". Evidence markers: [DECOMP] disassembly /
decompilation of FL's own DLLs (objdump, Ghidra 12 headless, our scripts in tmp/misc164m/gh); [EXEC] FL's DLLs loaded
unmodified by our own harness (tmp/misc164m/run/mi.c, from round 41's ms.c: CreatePlugInstance with a fake host that
logs every host Dispatcher call, `midi=<n>,<msg>` calls the plugin's MIDIIn); [STR] strings; [DOC] web; [TEST] tests in
tests/Misc164mTests.inc.

## 1. How FL gets a controller's mod wheel to a plugin

- FL's plugin SDK: a plugin receives raw MIDI (CCs, channel pressure) only through `TFruityPlug::MIDIIn` (vtable slot
  16), and only after asking for it with the host dispatcher `FHD_WantMIDIInput` (id 5, "switch in Value"). Notes come
  through TriggerVoice, the channel pitch / pitch wheel through the voice's Final.Pitch.
- [EXEC] At creation, FL's **Fruity Wrapper** (VST/VST3/CLAP host) calls host dispatcher 5; the Patcher implements
  MIDIIn. No native generator calls 5 (logged for MiniSynth, GMS, Morphine, Toxic Biohazard, Sawer, Sakura, PoiZone,
  DirectWave, FL Slayer, 3x Osc, Wasp).
- [DECOMP] MIDIIn (slot 16) of every Delphi generator shell (VMT found from CreatePlugInstance, tmp script
  `midiin.py`): **abstract** (Delphi's abstract-error stub) for 3x Osc, Autogun, BassDrum, BeepMap, Drumaxx, Drumpad,
  FL Keys, FLEX, FPC, Fruit Kick, Fruity DX10, DrumSynth Live, Granulizer, Slicer, Slicer 2, Slicex, Harmless, Harmor,
  Kepler, Kepler Exo, MIDI Out, Ogun, Plucked!, PoiZone, Sakura, SoundFont Player, Sytrus, Transistor Bass, Wave
  Traveller (calling it would raise run-time error 210, so FL never does). Implemented: Fruity Wrapper, Patcher,
  ReWired, Wasp / Wasp XT, Sawer (it hands the message to its engine).
- [DECOMP] The C++ "framework" generators (MiniSynth, GMS, Morphine, Toxic Biohazard, and the engines behind Sawer /
  PoiZone / Sakura) have a MIDIIn and a CC table (value / 127 at `+0x1268`, pressure at `+0x166c`, the per-plugin CC
  handlers: MiniSynth CC 1 → mod wheel `+0x39d8`, GMS CC 1 → `+0x3a14`, Toxic/Sawer/PoiZone sustain / reset /
  MIDI-learn), but the request for MIDI input (`Dispatcher(5, 0, 1)`) and the forwarding to the engine are gated by a
  module flag (`+0x52b`) that is 0 in FL's generator builds (`+0x52a` = 0 = generator: plugin flags 0x200011).
- [EXEC] Even when MIDIIn is called directly with CC 1 = 127 and channel pressure 64, the engine's CC table entry and
  pressure stay 0 (MiniSynth, GMS, Morphine, Toxic Biohazard: engine at shell `+0x228`, proved by the note's velocity
  100/127 = 0.7874 landing in its velocity table) — the generator path only forwards notes to the shell's keyboard.
- [STR] FL's remote-control targets include "Omni - Mod wheel (Wrapper)" (FLEngine_x64.dll): the focused **wrapper**'s
  mod wheel. [DOC] Novation's FLkey notes and the Image-Line forum ("every single native synth ignores the mod wheel")
  agree: CC 1 has no default function in FL; it is linked by hand (right-click a control › Link to controller).

**Conclusion.** In FL 2026 no native generator — Sampler / audio clips (channel), 3x Osc, Sytrus, Harmor, Harmless,
GMS, Morphine, MiniSynth, PoiZone, Toxic Biohazard, Sakura, Sawer, DirectWave, FL Slayer, DX10, Kepler, Ogun, Transistor
Bass, the drum generators… — responds to the mod wheel or to aftertouch by itself. Their engines' "mod wheel" /
"aftertouch" sources (Sakura's matrix, GMS's matrix rows, PoiZone's MW destination, Toxic's and Sawer's MIDI matrix,
DirectWave's "Mod Wheel" source, DX10's mod wheel) are fed in FL only by their own on-screen controls / automation
(PoiZone's wheel = parameter 157, Slayer's = parameter 28) or stay 0. LYNX already behaves the same: its native engines
ignore CC 1 / pressure (`Instrument::controlChange` default), hosted plugins get the raw message (round 162c), and
"Link to MIDI controller" (any knob's right-click) is FL's way to put the mod wheel on a native parameter.

## 2. What changed

- **Patcher** (`src/core/Patcher.*`): FL's Patcher implements MIDIIn and passes MIDI to the plugins inside; LYNX's
  Patcher passed the live notes but dropped the mod wheel, the other CCs, channel pressure and the pitch wheel. Now
  `PatcherInstrument::midiMessage / pitchBend / controlChange` reach the instruments cabled to "From LYNX" notes (a
  VST inside a Patcher gets its mod wheel and aftertouch; LYNX's Sampler / Synth inside bend by the channel's range).
  [TEST `misc164m_patcher_passes_live_controllers_to_its_instruments`]
- Native engines unchanged on purpose. [TEST `misc164m_native_engines_keep_ignoring_the_mod_wheel_like_fl`: a Sakura
  patch with mod wheel → level keeps the wheel at 0 whatever CC 1 does.]

## 3. Not done / notes

- LYNX's SoundFont player moves its "Mod wheel" knob from a live CC 1 (round 158 choice). FL's SoundFont Player has
  no MIDI input (MIDIIn abstract), so this is a LYNX extra, left as it is.
- The sustain pedal (CC 64) is not handled for LYNX's native instruments; FL's channel handles it on the host side
  (not checked in this round).
- Not checked with FL running and a real controller: this PC has no MIDI loopback (Windows MIDI Services runs but no
  loopback endpoint exists; installing one needs the user). The evidence is the code path above and the harness runs.

## Where I stopped

- Done: research (above), Patcher forwarding + tests (commit 8e2c1df0). [VERIFIED] build-local with FL Data: 1757356 checks, 0 failures (both misc164m tests ran); Windows build (build-win-misc164m) OK.
- Next round, if wanted: sustain pedal (CC64) on native instruments needs its own FL check first; a live FL MIDI test needs a loopback MIDI device on the machine.
