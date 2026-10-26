# Round 162c (sampler162): issue #9 — routing past insert 16, Sampler tuning / stretch, pitch wheel, export, editor volume

2026-10-26. Issue #9 (BURNER1PA, Windows 10, LYNX 0.1.17): the sampler / channel / MIDI / export items. Evidence
markers: [CODE] read in LYNX's source; [TEST] proven by tests/Sampler162Tests.inc (build-local); [DOC] FL Studio's
online manual; [WIN] checked in the Windows build.

## 1. Summary

| # | Report | Cause | Fix | Status |
|---|---|---|---|---|
| 1 | "Unable to assign a channel past insert 16" | [CODE] the route box, the channel window's Track knob and the setters were limited to `song.mixer.size() - 1`; a new project has Master + 16 inserts (maxMixerTracks = 128 was never reachable from there); the renderer clamps a route past the last insert to the last one | `mixerops::routeChannel` / `ensureInsert`: any insert 0…127 (FL 2026: 125 + Master), the missing inserts are added; `Song::claimFreeInsert` gives new channels insert 17, 18… instead of Master once 1…16 are in use; a project whose channel routes past its saved mixer gets the inserts on load | done [TEST] |
| 2 | "Sampler fine tune does not change the pitch" | [CODE] (a) keys played live (MIDI / PC keyboard) never got Misc › Fine tune nor the main pitch (only the song's notes did); (b) with Stretch (Resample) the pitch knob, the fine tune and the key were all ignored (`semitones = 0` when stretched) | live notes get `setNextNoteExpression (fineTune + main pitch)`; (b) see 3 | done [TEST] |
| 3 | "Stretch ignores the key: every note the same pitch" | [CODE] Resample stretch: rate fixed at the stretch speed; "Stretch keeping pitch": pre-stretched sample then resampled per key (pitch and length both changed) | [DOC] FL: Resample = "pitch and length linked … each note at a different speed"; Stretch modes = "pitch changes keep the original duration". Resample: the key/pitch/fine transpose the stretched speed (like tape). Keeping pitch: a transposed voice reads the stretched sample `factor` times faster (exact transposition, FL's sinc reader) and a real-time Signalsmith Stretch (`VoicePitchShifter`) stretches it back to its length — the key transposes, the length stays; portamento / slides / pitch envelope / wheel move the factor | done [TEST] |
| 4 | "MIDI pitch bend / mod wheel don't work by default" | [CODE] `handleIncomingMidiMessage` only played notes; CCs went to MIDI-learn links only; the pitch wheel was dropped | the pitch wheel, CCs (except 120…127 and CCs that have a MIDI link) and channel pressure go to the channels the notes go to (selected / device target): hosted plugins (VST/VST3/CLAP, SoundFont player, MIDI Out ports) get the MIDI message; LYNX's Sampler and Synth bend by the channel's pitch range (FL's Misc › Pitch range, default ±2) | done [TEST] |
| 5 | "Export fails when replacing a file of the same name" | [CODE] the exporter deleted the old file and opened a new stream on the same name; on Windows a file another program (player, preview) holds open cannot be deleted, and JUCE's FileOutputStream then opens the existing file positioned at its end (the new audio appended after the old) | write to a temporary file next to it and replace the target only when complete; a target held open is reported before the render ("… is in use by another program …"); a cancelled export leaves the old file intact. The save dialog already asks to replace (warnAboutOverwriting) | done [WIN] |
| 6 | "Audio Editor: add a volume knob" | — | a playback volume knob next to Loop (0…125 %, double-click 100 %, remembered in the settings, live while playing); "Amp..." (Tools / quick tools) already changes the audio's level | done |

## 2. Notes

- Native instruments other than the Sampler and the built-in Synth ignore the pitch wheel for now (they have
  `Instrument::pitchBend`, default no-op); hosted plugins get every message. The mod wheel reaches plugins and the
  SoundFont player; LYNX's own engines have no CC 1 mapping (FL's natives neither, unless linked).
- A key played at the root of a "keeping pitch" sampler plays the stretched sample untouched (bit-identical to before);
  only transposed voices go through the shifter.

## 3. Stretch keeping pitch: measurements [MEAS]

Scratch benchmarks (g++ -O2, machine loaded by other builds, so times are upper bounds):
- Signalsmith's own transposition (`setTransposeFactor`) on pure sines, 60 ms blocks: up to 33 cents off at 110 Hz
  (8 cents at 440 Hz); 120 ms blocks: 12 cents. Not usable for a sampler's keys.
- Resample by the factor + Signalsmith time-stretch back to length (transpose 1): ≤ 0.05 cent from 55 Hz to 1 kHz,
  −12…+12 semitones, with 80–100 ms blocks (60 ms failed at 55 Hz). Chosen: 100 ms, overlap 4 (drums: 80 ms).
- Priming a note (outputSeek, no latency): ~1 ms unloaded (11 ms with the offline 120/20 ms preset). A render primes
  at most 2 notes (exports: all); the 3rd+ note of a chord starts one block later. 8 shifters per channel (~4 MB);
  beyond 8 transposed notes at once the extra ones play resampled.

## Where I stopped

- Items 1–6 done; tests in tests/Sampler162Tests.inc (7 tests, 40 checks). build-local with FL Data: 1752886 checks,
  0 failures. Windows build (build-win-sampler162): selftest OK, export over the same file twice OK, a target held open
  reported (rc 1) with the old file kept, `--snapshot` shows the Audio Editor volume knob.
- Not done: FL black-box comparison of the stretch modes (relied on FL's manual [DOC]); native engines other than the
  Sampler / Synth ignore the pitch wheel; pitch bend is not recorded into patterns.
- Landed to main (round 162c).
