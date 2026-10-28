# REA round 165c — patch165: Hardware Insert in a Patcher, real-time hardware export, re-ping prompt, Patcher node editors, Layer node

## Where I stopped

**2026-10-28 03:20: all five items done and verified** (core tests 0 failures, Linux + Windows builds, Xvfb
`--patchershot` and PipeWire-loopback `--hwinserttest --patcher --bounce` all OK; see Measurements). Squashed into one
commit and landing via land.sh. History of the session below.

(2026-10-27 ~16:10, resumed after the 15:26 BSOD; rebuilt from `git diff origin/main...HEAD`.)

Already written before the crash (commits c2db4cd8 + checkpoint 902d2c0e, **unverified, not yet compiled**):
- Hardware Insert inside a Patcher: `hwinsert::forEachHardwareInsert` (nested Patchers, generator Patchers),
  `patcher::usesHardwareIo`, Graph/PatcherEffect/PatcherInstrument `setHardwareIo` with the device offset per piece,
  RackRenderer pins such channels/tracks to the audio thread and hands them the buses; round trip = structure (graph
  rebuilt on a new ping). Add plugin menu lists the Hardware Insert; its node opens the mixer editor
  (`makeNodeEffectEditor`).
- AudioEngine: `getHardwareInserts / locateHardwareInsert / runningHardwareInsert / staleHardwareInserts /
  pingHardwareInserts`, ping results applied by the engine (with the device setup key kept in the slot state
  `setup=<key>`), re-ping notice → MainComponent prompt "Ping now / Later"; editor shows "Measured for another setup".
- Export › "Render through the hardware in real time": `ExportJob (AudioEngine&, …)`, `runRealtime` (Master tap
  `bounceTap`, progress, cancel, dropout notice), ExportDialog toggle (visible only with an active hardware insert).
- Keyboard Controller editor (`writeKeyboardController`, KeyValues drawing), SoundFont editor on a Patcher node
  (`SfTarget`), Envelope Controller node editor with live outputs; `--patchershot` shoots every node editor
  (`PatcherWindows::shootNodeEditors`); `--hwinserttest --patcher --bounce file.wav`.
- tests/Patch165Tests.inc (7 tests), pt-BR strings.

Since the resume (16:10-16:40, PAUSED by the coordinator for the load limit):
- **Layer as a Patcher node**: Add plugin › Misc › Layer; ports = Notes in + a **Children** MIDI pin; the generator
  nodes that pin reaches (through MIDI Out nodes, Layers left out, node order) are its children
  (`patcher::layerChildren`). Graph: `MidiTarget::layer`, `Graph::LayerNode` (defs for `layer::pick`, picked child per
  key, random/turn), `forLayeredTargets` used by deliver / routeNote / routeVfxOutput / CC+bend / slide; level
  (`layer::levelGain` → setNextNoteGain or velocity), pan, pitch (`pitchCents`), key regions, Add-to-key key shift.
  Layer settings + children key regions are structure (graph rebuilt); a note off without a remembered pick goes to
  every child (no stuck notes after a rebuild). `layer::splitChildren/resetChildren` generalized to any children.
- LayerEditor refactored (`LayerSource`): node version without Set/Show/Group/Delete, with Level/Pan/Pitch knobs and
  a hint; opens from the node.
- Granulizer node page: "Sample..." button (no channel to drop a sample on). SoundFont node menu "Load into this node".
- `--patchershot` channel demo gains a Layer node (crossfade 0.35) on the SoundFont + Granulizer nodes.
- tests/Patch165Tests.inc: 4 Layer-node tests added (every child/level/bend, pan/pitch/regions,
  crossfade/sequential/random, children/structure/split).
- **Nothing compiled yet** (the first Linux build showed only the pre-existing gcc `unix` macro error in
  tests/Rea146bTests.inc — build target `LYNX` only; my -j6 build was stopped for the load limit).

Next on resume (LOAD LIMIT: buildslot + ninja -j3, wait while loadavg ≥ 14): core tests
(`scripts/build-local.sh` FL Data), Linux `ninja -j3 LYNX`, fix errors; pt-BR strings for the new texts ("Children",
"Layer - Level/Pan/Pitch", "Cable its Children pin…", "No children yet: cable…", "Sample...", "Choose the sample the
Granulizer plays", "Load into this node"); `--patchershot` + `--hwinserttest --patcher --bounce` under Xvfb, look at
shots; Windows build; land.

## FL facts (Layer inside a Patcher)

- [DATA] FL 2026's plugin database files Layer under **Generators › Misc** (`Data/Patches/Plugin database/Generators/
  Misc/Layer.fst`, next to Audio Clip, Automation Clip, SoundFont Player, Fruity Voltage Controller). FL's Patcher
  › Add plugin lists the plugin database's categories, so FL offers Layer there under Misc — LYNX lists it in the same
  place.
- [HYP] How FL's Layer picks children inside a Patcher was not checked with FL running (Windows desktop rule; no cheap
  check). LYNX's clean-room choice: the generators cabled to the Layer node's **Children** pin are its children, in
  node order, so the Patcher map shows the layering.

## Measurements and checks (2026-10-28)

[RUN] `--hwinserttest` on a private PipeWire daemon (`pw-loopback` "Loopback" sink → "Loopback-return" source, 48 kHz,
LYNX under Xvfb; scripts in the session scratchpad), click track on Insert 1 through the insert, dry twin on Insert 2:

| Insert | Ping (engine-applied) | PDC | Pairs | Align | Null | Real-time bounce vs offline | Re-ping 256→512 |
|---|---|---|---|---|---|---|---|
| Mixer slot | 256, setup key kept | 256 | 12/12 | 0 smp | −147.2 dB | — | notice 1 → Ping now → 512, PDC 512, stale 0 |
| Inside a Patcher on Insert 1 | 256, setup key kept | 256 | 13/13 | 0 smp | −147.2 dB | 10.2 s for 10.0 s of music; 480000 = 480000 frames; −300 dB residual; wet vs dry −147.2 dB | notice 1 → 512, stale 0 |

- The ping's result is now applied by the engine (`applyHardwareInsertPings`, 6×/s) with the device setup key
  (`setup=<driver|out|in|rate|buffer>` in the slot state); the new round trip rebuilds the insert (its latency), so the
  test reads the result from the slot (the rebuilt instance's live ping meter is back at 0).
- Re-ping: changing the buffer size makes `staleHardwareInserts()` non-empty and posts the notice once; the app shows
  "Ping now / Later"; "Ping now" measures again for the new setup.
- [TEST] the two hardware-in-Patcher unit tests used a simulated cable shorter than the callback (the loop model needs
  round trip ≥ block); cases fixed (4800/4800/1600, 6000/4800/4800 → graph pieces of 2048; generator case round trip 512).
- [SHOT] `--patchershot` under Xvfb (`/mnt/d/lynx-work/patch165/shots/`): channel Patcher map with the Layer node and
  its Children cables, the Layer node editor (crossfade meters 65/35 %), Keyboard Controller editor (ramp, live output
  line), SoundFont node editor, Dashboard node, Hardware Insert node editor inside the insert Patcher, Add plugin menu
  (Misc › Layer, Hardware Insert listed). Fixed after looking: demo nodes overlapped the Envelope Controller; the
  Granulizer node's Sample button was not laid out (resized before it existed).
