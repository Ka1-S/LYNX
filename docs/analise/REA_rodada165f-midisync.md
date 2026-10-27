# REA round 165f sub-round rea165-midisync: MIDI clock / MTC master sync and external clock sync

Gap #2 of docs/analise/REA_rodada165f.md. Clean-room: behaviour and names only.

## Evidence so far
- [DFM] TMIDIForm (Options › MIDI): per output `MasterSyncBox` "Send master sync", `SyncTypeSelect` = MIDI clock
  ("Standard MIDI clock and Song Position Pointer") / MTC 24 / 25 / 30 drop / 30 non-drop / 29.97 drop / 29.97
  non-drop, `MasterSyncOffset` −1000…1000 ms, `SendAllNotesOffBox` "Send all notes off to this device on init and
  after render". `MIDISyncPanel` "External sync": `ExtClockSyncSelect` (device list, filled at run time),
  `MidiSyncOffset` −1000…1000 ms, `ExtSyncStatusLabel` "Status:". ("Slave tempo" is the FL-as-plugin page, not MIDI.)
- [DFM/strings] handlers (Delphi method table): MasterSyncBoxClick 0x1367850, SyncTypeSelectChange 0x1368120,
  MasterSyncOffsetChange 0x13678e0, MidiSyncOffsetChange 0x1366fb0, ExtClockSyncSelectChange 0x13610e0,
  SendAllNotesOffBoxClick 0x135cd70.
- [strings] class `TMIDIClockReceiver`, "Create MIDI clock receiver", "Start the MIDI sync thread", `SyncThread`,
  "Clock is too fast !", "FL Studio Midi Sync warning", "Your audio device is set to FL Studio ASIO. This can cause
  inaccurate MIDI sync timing.", registry names `ExternalMidiClockDeviceID`, `MasterClockSyncOffset`,
  `MidiSyncTimingOffset`, toolbar `MIDISyncLED`, action "Enable MIDI master sync" (OptionsEnableMIDIMasterSyncAction).

## LYNX today [CODE]
- No juce::MidiOutput anywhere; MIDI inputs opened in AudioEngine::enableAllMidiInputs via the deviceManager;
  handleIncomingMidiMessage ignores system real-time. Prefs: prefs::MidiPrefs (src/core/Preferences.h, text format).

## Plan
core/MidiSync.{h,cpp} (pure, unit-tested): clock generator (24 PPQN, sample offsets in the block, Start/Continue/
Stop/SPP), MTC quarter-frame / full-frame encoder (all 6 fps incl. drop-frame), clock receiver (smoothed tempo,
transport commands). Engine: audio thread pushes timestamped events into a lock-free queue; a sender thread opens the
MidiOutput and sends at the target time (sample clock → ms through a DLL); offsets applied there. Settings › MIDI UI.

## Where I stopped
- 16:20 — PAUSED by the coordinator (load). Done: briefs read, DFM + strings evidence above, handler addresses,
  own Ghidra script dir tmp/rea165-midisync (g.sh + Dec.java on /mnt/d/lynx-work/rea165-midisync). No code yet.
  Next: decompile the handlers + TMIDIClockReceiver (depth 1–2), then implement core/MidiSync.
