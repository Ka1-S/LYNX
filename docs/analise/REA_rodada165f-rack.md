# REA round 165f — sub-round rea165-rack: Channel Rack technologies (Burn MIDI, channel loops, Receive notes from, auto-sampler)

2026-10-28. Lead doc: docs/analise/REA_rodada165f.md (§1.5, §2 gap #9). Clean-room: behaviour and names from FL's forms
([DFM], tmp/rea165/dfm/TStepSeqForm.dfm.txt, TFruityLoopsMainForm.dfm.txt), headless Ghidra on our own copy of the
analysed FLEngine project ([DECOMP], /mnt/d/lynx-work/rea165-rack), FL's demo projects ([DATA]) and the online manual
([MANUAL]). No Image-Line code, art or data copied.

## 1. What FL does (REA)

### 1.1 Channel loops ("Loop step channels / Loop all channels / Advanced looping")
- [DECOMP] `SSLoopDisabledMenuClick` (0x1046330) sets the pattern's loop type (pattern+0x50: 1 = step channels,
  2 = all channels, 3 = advanced) through FUN_01038550; `LoopCtrlBtnClick` (0x1045910) toggles the pattern's loop
  switch (pattern+0x54). Looping is **per pattern**, not per project.
- Each pattern keeps an array of 0x14-byte entries {channel, setting, length in ticks, iteration position, last note end}.
  The *setting* is the loop point: 0 = don't loop, −1 = after the last step ("Step"), −2 = next beat, −3 = next bar,
  N > 0 = a fixed number of steps (from "Set to next beat/bar", or by dragging the loop marker).
- FUN_012cfd20 computes the loop length: lastEnd = max over unmuted notes of start + max(length − 1, 0);
  −1 → (lastEnd / step + 1) · step, −2 → next beat, −3 → next bar of the pattern's time signature, N → N steps;
  clamped to the pattern length; a zero length with a non-zero setting means the whole pattern.
- FUN_01045450: when looping is switched on, channels with no entry get −3 (Bar): mode 1 only step-sequenced channels,
  mode 2 every channel. Mode 3 shows the per-channel loop-point column for editing.
- FUN_01064140 (playback): notes at or after the loop length don't play; the loop's notes repeat at +k·L up to the
  pattern length → polyrhythms (a 3-step hat against a 16-step kick).
- `CtrlAutoNextStepMenuClick` (0x1045d70), the loop-point menu: Don't loop / Step / Beat / Bar / Set to next beat /
  Set to next bar (the last two freeze the current length as a fixed step count).
- [DATA] .flp pattern events: 0x1A (u8) loop on, 0xA2 (u32) loop type, then pairs 0xA0 (u32 FL channel) + 0xA1 (i32
  setting) after the notes (0xE0). Demo "Edlan & Ella Noël - Song For You.flp" pattern 10: loop on, type 1, all −3;
  "Astes - Bien Duro" stores entries with looping off; "Sid De La Cruz - Secret Agent" stores 0 settings.

### 1.2 Burn to pattern
- `BurnAllLoopsToPatternMenuClick` (0x1046000) / `CtrlBurnToPatternMenuClick` (0x10462e0) → FUN_01063c80: writes the
  repeats into real notes over the pattern length, then FUN_012cf150 sets the channel's setting to 0 (Don't loop).

### 1.3 Burn MIDI to › Current / New pattern
- `BurnMIDItoPatternMenuClick` (0x124c920) → FUN_012fe2d0, `BurnSelMenuClick` (0x124c950). FL renders the channel's
  played notes (arpeggiator, echo, swing, time shift/gate, loops) through a temporary MIDI Out channel into
  TempRender.MID and imports it back; "Nothing to burn|The selected channel doesn't contain any notes to burn." when
  empty; then a "Reset after burning to MIDI|Select what you want to reset:" dialog with Arpeggiator / Echo feedback /
  Swing mix (remembered).

### 1.4 Receive notes from
- [MANUAL] locks a MIDI input device (optionally one MIDI channel) to the channel, whatever channel is selected; a lock
  icon on the channel button. `LineUnlockControllerMenuClick` (0x123ce50) unlocks.

### 1.5 Create DirectWave instrument (TDWPRenderForm)
- Options: Low note 36, High 83, Keys per zone 4, Root offset 2, Velocity layers 1, Max velocity 127, Link velocity to
  gain, Cycle layers 1–8; Stop on Silence / Max length, Max length + Release after (bars/beats/seconds); resampling,
  dithering, HQ plugins, disable max polyphony, 16-bit int / 32-bit float, Stereo / Mono merged / left / right,
  Declick in/out, Normalize, Trim PDC silence; Enable insert effects / master effects, Open in DirectWave;
  Start / Abort / progress, Background rendering. `CreateDWPMenuClick` (0x124c7c0).

## 2. What LYNX does now (our implementation)
- core/ChannelLoops: the loop-point rules of §1.1 (`loopLength`, `effectiveSetting`, `expandPattern/expandSong`,
  burn to pattern, loop types, remap on channel delete/move). Expansion runs when the engine snapshot is built
  (RackRenderer::RackState::build), so playback, export, stems and Song mode all hear the repeats with zero
  audio-thread cost.
- core/ChannelBurn: Burn MIDI offline — the scheduler's own timing (`playedTiming`: swing, shift, gate), the real
  Arpeggiator and NoteEcho run tick by tick, echo gain → velocity, pitch offsets → key + fine pitch; to the current
  pattern (replaces the channel's notes, loop → Don't loop) or a new pattern "<pattern> - <channel>" after it; the
  reset dialog (Arpeggiator / Echo feedback / Swing mix / Time).
- .flp import of 0x1A/0xA2/0xA0/0xA1, .lynx save/load (pattern loopOn/loopMode/channelLoops; channel receiveFrom),
  undo ("Channel looping: …").
- Channel Rack: loop button in the rack title (click on/off, right-click the loop types + Burn), the loop-point column
  (Advanced looping; drag, wheel, double-click = Bar, right-click menu), faint repeats + accent marker in the steps,
  channel menu "Burn MIDI to", "Loop point", "Receive notes from" (device × Any / 1–16, Unlock) with a padlock on the
  name. AudioEngine routes a locked device / MIDI channel to its channel.

- core/AutoSampler + ui/AutoSamplerDialog ("Create sampled instrument...", FL's Create DirectWave instrument): the plan
  (zones every *keys per zone* from low to high note, root = zone low + root offset, velocity layers up to max
  velocity), a render song with only the channel at 0 dB (volume 1.0) and centred, automation / controllers removed,
  one note per take held *Release after* seconds in slots of release + max length; rendered by the export engine
  (ExportJob, pattern range, 32-bit float, cut tail, PDC trimmed, Master effects only when asked, insert effects only
  when asked) on a ThreadWithProgressWindow (progress + Cancel; the job is built and torn down on the UI thread); the
  takes are cut (stop on silence: last sample above −90 dBFS + 50 ms, or release + max length), declicked (2 ms in /
  10 ms out), mono merged/left/right, normalized with one gain for all takes (layers keep their levels), written as
  16-bit int or 32-bit float WAVs in Music/LYNX/Samples/<channel> (sampled)/, and a Sampler channel with one zone per
  take (key + velocity ranges, root) is inserted after the source, with its volume, pan, colour, group and insert
  (Master when the insert effects were recorded), release 0.25 s.

## 3. Not done / differences
- Auto-sampler: FL's *Cycle layers* (round robin), *Link velocity to gain*, resampling / dithering / HQ choices,
  bars/beats units for the times and *Background rendering* are not offered (LYNX's Sampler zones have no round robin;
  the sampler's own velocity response applies on top of the velocity layers). The result is a LYNX Sampler channel,
  not DirectWave.
- Burn MIDI: done offline from our own scheduler + arpeggiator + echo code instead of FL's MIDI Out round trip; plugin
  MIDI output (VFX / note generators inside plugins) is not baked.
- Receive notes from: the device list is the enabled MIDI inputs; FL's "orange LED" is our padlock on the name.
- No FL audio null-compare of a looped pattern this round (no small FL project with loops to render); evidence is the
  REA rules + the demo .flp loop events import test (rack165_fl_demo_song_loops).

## 4. Verification
- Tests (tests/Rack165Tests.inc, 11): loop-length rules, loop types, polyrhythm expansion, burn to pattern, loop points
  following channel delete / move, .flp loop events, FL demo "Song For You" pattern loops, Burn MIDI (echo + swing +
  time, arpeggio to a new pattern), auto-sampler plan / render song / cuts / channel.
- Windows GUI (scratch profile, test mode): looping project with Kick Bar / Hat Step (3 steps) / Snare 5 steps draws
  the faint repeats and the loop-point column; the channel menu shows Receive notes from / Burn MIDI to / Loop point /
  Create sampled instrument...; sampling the Kick (defaults: 12 takes of 6 s) finished in < 1 s and added
  "Kick (sampled)" with 12 zones (16-bit stereo WAVs, peak −0.2 dBFS); the channel loops stayed on their channels.
- Perf: the loop expansion runs when the engine snapshot is built (never on the audio thread). One-pass expansion:
  100 patterns × 64 looping channels (410k played notes) = 35 ms per snapshot (was 226 ms before the one-pass
  rewrite); a typical looping 4-bar pattern with 16 channels is well under 0.1 ms. Non-looping patterns cost nothing.

## Where I stopped
- 2026-10-28 03:40: everything implemented and verified (tests, Windows GUI); landing one batch (build-local with FL
  Data + Windows build, then land.sh). Left: see §3.
