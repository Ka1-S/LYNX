# Round 164f — Patcher leftovers: engine editors in a Patcher, MIDI out of plugin nodes, Envelope Controller Mod X / Y, sample-accurate MIDI into mixer-slot Patchers

The four Patcher items left open by rounds 162e and 164a (`REA_rodada162e.md`, `REA_rodada164a.md` "Where I stopped").
FL's import side of this round (Dashboard, X-Y-Z curves, Voltage Controller checks) is in `REA_rodada164f-flimport.md`.

Evidence markers: **[MAN]** FL's online manual (Patcher page), **[DECOMP]** earlier static decompilation (REA_rodada15),
**[TEST]** LYNX unit tests (`tests/Patcher164fTests.inc`), **[SHOT]** screenshots of LYNX running, **[HYP]** not checked
against FL running.

## 1. Instrument editors of built-in engines inside a Patcher

**FL** [MAN]: "plugins now open in 'detached' mode outside the Patcher UI when double-clicked" — a node opens the same
plugin window it has on a channel (the wrapper with the plugin's own interface), not the channel's settings.

**LYNX before**: a generator node of LYNX's synth / sampler or of an FL engine (3x Osc, Sytrus, Harmor, FL Keys…) answered a
double-click with "This node has no editor of its own here."

**LYNX now** (`ui/ChannelSettings.cpp`): the channel pages (synth page, sampler page, the FL engines' page and the
synth / sampler "Envelope / instrument" tab) edit a `ChannelRef` — a Channel Rack index or a Patcher node found through a
locator — instead of `song.channels[index]`. `makeNodeChannelFrame` builds the same tabbed window for a node, without the
channel-only parts (the Pan / Vol / Pitch / Track header and the Misc tab: a node has no mixer track, arpeggiator or cut
groups). The Patcher's double-click / "Open editor" (`PatcherEditor::openNodeEditor`) opens it.

The engine keeps playing while its knobs move: a Patcher node used to be rebuilt whenever its `modelParams`, `pluginState`
or `controllerState` changed (`sameInstrumentSetup`), so every knob step restarted the instrument. It now follows the rack's
own rule for a channel (`RackState::build`): only what `configure()` cannot take (another sample, zones, a plugin, a MIDI
Out's routes, a Keyboard ↔ Envelope Controller switch, a Dashboard's control kinds) builds a new instance [TEST
`patcher164f_engine_edits_keep_the_running_node`].

## 2. MIDI output pins on plugin nodes

**FL** [MAN]: a node's inputs / outputs are activated from its menu; event links are green; plugins that send MIDI play the
plugins cabled to their output.

**LYNX** (`core/Patcher.cpp`): a hosted plugin generator node has a "MIDI out" pin (`nodePorts().midiOut`). The editor shows
it when the plugin reports MIDI output (`producesMidi()`: arpeggiators, MIDI effects) or a cable already uses it. What the
plugin sends in a piece (`Instrument::takeMidiOutput`, the wrapper's "Output port" path of round 149) plays the nodes
cabled to the pin at the next piece's start — instruments, MIDI Out nodes (which set the MIDI channel) and VFX nodes
(round 164b) alike, sample-split like the VFX outputs. No allocation on the audio thread (fixed arrays, reserved lists)
[TEST `patcher164f_plugin_node_midi_output_plays_the_nodes_cabled_to_it`]. VFX nodes already had theirs (164b).

## 3. Automating the Envelope Controller's Mod X / Y

FL's Envelope Controller parameters 88 / 89 are the global Mod X / Mod Y (value / 65536) [DECOMP REA_rodada15
`FUN_008ae740`]. LYNX now treats them as automation / link targets of the controller channel
(`AutomationTargetKind::channelParam`, param 88 / 89):

- `ControllerInstrument::setParameter` holds the automated value over the saved one until the knob is moved in the editor;
- the rack binds such automation clips and links like a plugin parameter (`RackRenderer` bind);
- `Song::targetValue` / `setTargetValue` read and write the value inside the controller's state (automation clip creation,
  "last tweaked", recording);
- the editor's Mod X / Mod Y knobs carry the target (right-click: Create automation clip, Link to controller…); the
  automation's name is "<channel> - Mod X";
- in a Patcher, a control cable dropped on an Envelope Controller node offers Mod X / Mod Y.

FL projects that automate those parameters should import as these targets, since the import keeps FL's channel parameter
numbers [HYP: no FL project at hand automates them] [TEST `patcher164f_envelope_controller_mod_xy_are_automation_targets`].

## 4. Sample-accurate MIDI into mixer-slot Patchers

Round 162 delivered the notes a MIDI Out / VFX / plugin channel sends to a mixer-slot Patcher's "MIDI port N" pin at the
start of the chunk. `Graph::queueNoteOnPort / queueNoteOffPort` now queue them with their sample offset; the graph renders
its nodes in parts between those samples (the same split the VFX outputs use), so the instrument inside starts where the
note is — the same sample as a Channel Rack instrument [TEST `patcher164f_mixer_slot_patcher_notes_land_at_their_sample`,
`patcher164f_graph_queued_notes_play_at_their_offset_across_pieces`]. A note also wakes a smart-disabled Patcher slot (the
instruments inside a slot on a silent insert used to stay asleep after 4 s of silence); a slot that is off gets nothing.

## Where I stopped

(kept current below)
