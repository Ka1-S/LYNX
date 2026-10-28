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
