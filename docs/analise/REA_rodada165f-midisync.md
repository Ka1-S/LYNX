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

- [DECOMP] `MasterSyncBoxClick` (0x1367850): when master sync is turned on, FL first checks the audio driver: with
  "FL Studio ASIO" it warns "Your audio device is set to FL Studio ASIO. This can cause inaccurate MIDI sync timing. Use a
  dedicated ASIO driver for best results." (Cancel = the box goes off again); then stores the device's master-sync and
  notes-off flags and reopens the MIDI outputs ("Opening MIDI output devices", 0x117ca10): an output is opened when it
  has a port number or master sync. `SyncTypeSelectChange` (0x1368120), `MasterSyncOffsetChange` (0x13678e0) and
  `MidiSyncOffsetChange` (0x1366fb0) each store **one global** value (sync type and master offset are not per device in
  FL; LYNX keeps them per output — a superset). `SendAllNotesOffBoxClick` (0x135cd70) stores the flag of the selected
  output. `ExtClockSyncSelectChange` (0x13610e0): item 0 = off (device −1, status 0); a device that is not enabled
  asks "The device |%s| isn't enabled" [Enable device] [Disable sync]; status = 1 when the input is open, 3 when it is
  not (the Status label); it also runs the FL Studio ASIO warning.

- [DECOMP] **Master sync sender** = FL's "MIDI sync thread" (loop 0xbb0b00, `Sleep (1)` per pass, i.e. about 1 ms
  timing resolution): MIDI clock only **while playing** (no clock when stopped). Start (0xbb0a40): if the song is not at
  its start, Stop (0xFC) + Song Position Pointer (0xF2, position / ticks per 16th, rounded) first; then it waits for
  playback and sends **Start (0xFA)** — FL never sends Continue. Each clock (0xF8, 0xbb12f0) is scheduled from the
  previous one at one clock period, corrected against the song position (+ the master offset converted to ticks) so
  the clocks stay phase-locked to the song; a positive master offset delays the first clock by that many ms. Stop
  (0xbb1360): 0xFC. MTC (sync type > 0): quarter frames 0xF1 from the song time in seconds (+ offset), pieces 0…7
  cycling, the timecode snapshot taken at piece 0 (frames = fraction of the second × fps — no drop-frame label
  conversion), hours byte = hours mod 24 + the rate bits.
- [DECOMP] **Clock receiver** (TMIDIClockReceiver message handler 0x1312760): 0xFA and 0xFB both reset and start
  (status "Status: running [Beat n]"; FL plays from its **own** position — SPP is ignored); 0xFC stops ("Status:
  ready"). Each 0xF8 while running: a least-squares line through the **last 16 clocks** (time in samples → beats),
  tempo = its slope × 60, **clamped to 10…522 BPM and rounded to 0.1 BPM**; a second regression of FL's position
  against the clock-derived position gives a playback speed ratio clamped to 0.25…4 (phase lock); the external offset
  (ms × tempo / 60 000 beats) shifts the target. Every beat the status label's beat number moves. Status texts [DECOMP
  0x136a650]: "Status: off" / "Status: ready" / "Status: running [Beat n]" / "Status: error"; the toolbar shows a MIDI
  sync LED while running.
- LYNX vs FL: LYNX sends SPP + **Continue** when starting elsewhere (the MIDI spec; devices that treat Start as "from
  the top" then follow correctly), and **follows SPP** in (FL ignores it); per-output sync type / offset (FL: one
  global); MTC with real drop-frame labels; sample-accurate time stamps instead of a 1 ms polling thread; tempo fit over
  96 clocks with change detection + hysteresis (FL: 16 clocks, no hysteresis) — steadier with jittery USB interfaces;
  phase correction ±5 % (FL: up to ×0.25…×4). Same tempo range (10…522) and 0.1 BPM rounding as FL.

## LYNX today [CODE]
- No juce::MidiOutput anywhere; MIDI inputs opened in AudioEngine::enableAllMidiInputs via the deviceManager;
  handleIncomingMidiMessage ignores system real-time. Prefs: prefs::MidiPrefs (src/core/Preferences.h, text format).

## What LYNX does now (this round) [CODE]
- `src/core/MidiSync.{h,cpp}` (pure, unit-tested, no allocation):
  - `Generator` (one per output): MIDI clock = 24 pulses per quarter (every 40 ticks of LYNX's 960 PPQ), each at the
    first sample whose position reaches it (sample-accurate inside the block, also across tempo changes); Start
    (0xFA) when playback starts at 0, else Song Position Pointer (0xF2, rounded **up** to the next 16th) + Continue
    (0xFB) so the first pulse after Continue falls exactly on the SPP; Stop (0xFC) when the transport stops; a jump
    while playing (loop wrap, seek) = Stop + Start/SPP+Continue. MTC: quarter frames 0xF1 4 per frame, pieces 0…7
    over 2 frames, piece 0 on an even frame, the 8 pieces carry the frame of piece 0; a full-frame SysEx
    `F0 7F 7F 01 01 hh mm ss ff F7` on every start / locate; rate bits 0 = 24, 1 = 25, 2 = 30 drop (used for 29.97
    drop and 30 drop), 3 = 30 (also 29.97 non-drop); 29.97 runs at 30000/1001 real frames per second with drop-frame
    labels (frames 0 and 1 skipped each minute except every 10th). Song time for MTC: position / tempo at the locate,
    then the sample count.
  - `Receiver`: 0xF8 times → least-squares fit over the last 96 pulses (4 beats), tempo-change detection (mean of the
    last 6 intervals > 4 % off on two pulses running → the fit restarts from those 7 pulses), hysteresis 0.1 BPM on
    the published tempo; Start / Continue start on the **next pulse** at 0 / the last SPP, Stop, SPP; the master's
    position = last pulse's position + time since it at the measured tempo (extrapolated ≤ 0.1 s); "Clock is too
    fast!" above 999 BPM.
- `src/engine/MidiSyncIo.{h,cpp}`: audio thread runs the generators per sub-block (the block split at loop ends) and
  pushes time-stamped messages into one lock-free SPSC queue per output (no device call, no lock, no allocation);
  the "LYNX MIDI sync" thread (highest priority, `timeBeginPeriod(1)` on Windows) sends each message when due. Due
  time = smoothed block clock (sample count pulled 2 % per block towards the callback time; resync after 50 ms) +
  sample offset + one buffer + the device output latency + the output's offset (ms). External clock: the input named
  in the settings feeds the receiver on the MIDI thread; at each block start its commands move the transport, the
  transport plays at the clock's tempo (`Transport::externalBpm`, also the renderer's tempo, over the song's tempo
  automation), phase-locked: the error between the master's position (at now + latency − offset) and LYNX's corrects
  the tempo by up to ±5 % within ~0.5 s; more than half a beat off = relocate.
- "Send all notes off": CC 64 = 0 + CC 123 on the 16 channels of the outputs that have it, at start-up and after
  every export (`setPluginsRenderMode (false)`).
- Settings › MIDI: "Output devices" (per device: Send master sync, type, Offset (ms), Send all notes off) and
  "External sync" (External clock sync = Off / an input, External sync offset (milliseconds), Status: "Receiving
  clock 120.0 BPM, playing" / "No clock"). Saved in the MIDI settings file (`output=`, `clockin=` lines). pt-BR.
- CLI: `LYNX --midisync-test report.txt ["output"] ["input"]` (loopback in memory, sender timing, a real loopback
  when both devices are given); `--devices` now lists `midi-in=` / `midi-out=`.

## Plan
core/MidiSync.{h,cpp} (pure, unit-tested): clock generator (24 PPQN, sample offsets in the block, Start/Continue/
Stop/SPP), MTC quarter-frame / full-frame encoder (all 6 fps incl. drop-frame), clock receiver (smoothed tempo,
transport commands). Engine: audio thread pushes timestamped events into a lock-free queue; a sender thread opens the
MidiOutput and sends at the target time (sample clock → ms through a DLL); offsets applied there. Settings › MIDI UI.

## Where I stopped
- 16:20 — PAUSED by the coordinator (load). Done: briefs read, DFM + strings evidence above, handler addresses,
  own Ghidra script dir tmp/rea165-midisync (g.sh + Dec.java on /mnt/d/lynx-work/rea165-midisync). No code yet.
  Next: decompile the handlers + TMIDIClockReceiver (depth 1–2), then implement core/MidiSync.
