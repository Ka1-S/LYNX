# Round 165c — vfxfix165: the VFX leftovers of 0.1.19

Scope (from REA_rodada164b.md §4 / "Where I stopped"): Sequencer chains + program change + Smart bend cut; Envelope
loop points ("Pumping"); Level Scaler on held notes + "Event persistence" display; Keyboard Splitter manual mapping +
zone-1 fallback; voice-property changes outside a Patcher; VFX output to "To LYNX" ports; plus renamed plugins
(Plugin database) showing their new name in the Quick-add palette and on mixer slot labels.

Evidence markers: [DECOMP addr], [DATA], [FL render], [EXEC FL], [TEST], [SHOT], [RUN], [HYP].

## Where I stopped

- 2026-10-28 resume after the BSOD: checkpoint 25c28cdf (Vfx.cpp/h: program Event type, Monitor ring, Level Scaler
  setParameter + held-voice re-evaluation, Envelope loop time) merged with origin/main (c18471e2). Not built yet.
- Prior REA data on /mnt/d/lynx-work/vfxfix165: dec/ (Ghidra decompiles of the Sequencer), fl/ (FL renders of the
  Envelope default vs "Pumping" preset, env_*.wav), tools/ (mkvfx.py builds FL Patcher test projects with a VFX node).
- 07:00: checkpoint builds (Linux, build-linux/), tests/Vfx165cTests.inc added: Level Scaler held voices, Envelope
  loop (custom + FL's Pumping.fst), Keyboard Splitter zone-1 fallback + Manual mapping core — all green.
  Renamed plugins: Quick-add palette + mixer slot label use the Plugin database names (not built yet in the app).
  Next: Sequencer chains / program change / Smart bend cut (dec/ seq1 a190 seq4 decompiles), editors (Level Scaler
  Event persistence, Splitter Manual mapping button), voice props outside a Patcher, VFX → "To LYNX" ports.
- 2026-10-28 later: Sequencer chains + program change + Smart bend cut, voice props outside a Patcher, VFX → "To LYNX"
  ports, editors. Details:
  - Sequencer [DECOMP FUN_18000cde0 chain position, FUN_180009620 MIDI input, FUN_180010e00 program select]: the
    chain is a list of program slots, each `steps × syncBeats` long; Cur_Chain switches are quantized (Chain quantize)
    and Chain restart restarts the chain at the switch. Program change mode 1 selects the sequence (clamped to
    NumPrograms+7 like FL), mode 2 passes the program change on. Smart bend change regenerates and cuts every sounding
    note but the first. Chain end actions (chnp params 21–24) [HYP]: only "loop" — not implemented.
  - VfxInstrument: setParameter forwards to the processor (automation), MIDI 0xC0 → programChange; live MIDI program
    changes reach VFX channels (AudioEngine → Scheduler controlProgram) [TEST].
  - Voice properties outside a Patcher: a VFX channel targeting a LYNX channel sends its voice props as
    NoteEvent::voiceProp, delivered via Instrument::voicePropertyPort [TEST: enveloped 82.68 vs passed 100.6].
  - Patcher VFX node → "To LYNX" port: routeVfxOutput queues host events, the rack drains them each chunk
    (portOnly routes keep the port silent for ordinary notes) [TEST]. One chunk of delay.
  - Editors: Level Scaler "Persistence" + fading lollipop markers (FL fade formula, 30 Hz), Keyboard Splitter
    "Manual mapping" toggle + hint, Sequencer Chain + Program change combos [SHOT build-linux vfxshot].
  - 2026-10-28 13:00 (after the 2nd BSOD): the Plugin picker (Patcher / plugin window "Add plugin", core/PluginCatalog
    Entry::renamed) and the mixer's plugin window title also show the Plugin database name of a renamed plugin (sorted
    and found by it, still found by the original name; category from the original) [TEST r165c_plugin_catalog_renamed_plugins].
    Merged origin/main (165d VFX Script: one Processor::setParameter for both rounds). build-local with FL Data
    1 781 851 checks, 0 failures [RUN]; Linux build --vfxshot under Xvfb: quickadd-renamed / mixer-renamed show "1_EQ",
    Sequencer Chain + Program change combos, Level Scaler Persistence, Splitter Manual mapping [SHOT].
  - Left: no automation target UI for VFX params; hosted plugins get no per-voice props; the program picked live by
    program change / chain is not written back to the saved state; zone-16 auto-end of Manual mapping is [HYP].
