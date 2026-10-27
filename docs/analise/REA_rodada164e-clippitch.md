# Round 164e (misc164p): FL audio clip pitch import, checked against FL

2026-10-27. Round 163c (REA_rodada163c-pitch.md) stopped applying an FL audio clip channel's header pitch and root note
on an unchecked assumption, and FL's time-stretch PITCH knob was never read. This round measured what FL really does.
Evidence markers: [FL] rendered by FL Studio 2026 (`FL64.exe /R /Ewav`) and measured; [CODE] LYNX source; [TEST]
tests/ClipPitch164Tests.inc; [WIN] LYNX Windows build (`LYNX.exe --render`).

## 1. Method

- Test projects: a small FL 25.2 project (one Sampler, two audio clip channels, no effects) used as a template; a script
  (tmp/misc164p/mk.py, mk2.py) rewrites the second audio clip channel: sample = a 2 s 440 Hz sine
  (C:\Users\kauan\lynx-test\misc164p\sine440.wav), header pitch (0xDB +8), root note (0x87), fine tune (0x8E), the
  0xD7 time-stretch fields (TIME +0x60, PITCH +0x64, MUL +0x68, MODE +0x6C), one Playlist audio clip of 6 beats at
  86 BPM; mk2.py makes the same channel a Sampler channel played by a pattern note instead.
- FL renders each one from the command line (FL lock held, minutes); pitch = interpolated FFT peak of the steady part,
  length = while above 30% of the peak (tmp/misc164p/meas.py). Projects and renders: C:\Users\kauan\lynx-test\misc164p
  (copy on D:\lynx-work\misc164p).
- 0xD7 layout (matches the public PyFLP description): +0x60 TIME, +0x64 PITCH (cents, signed), +0x68 MUL
  (2^(v/10000)), +0x6C MODE (0 Resample, -1 Stretch, 1, 2, 5 … élastique / auto modes).

## 2. What FL does [FL]

| case (audio clip unless noted) | FL pitch (cents) | FL length (s) |
|---|---|---|
| base | 0.0 | 2.000 |
| header pitch +400 / -300 | +400.0 / -300.0 | 1.587 / 2.378 |
| root note 48 / 67 | +1200.0 / -700.0 | 1.000 / 2.996 |
| fine tune +50 | +50.0 | 1.943 |
| stretch PITCH +700, Resample | 0.0 (ignored) | 2.000 |
| stretch PITCH -1200, Resample | 0.0 (ignored) | 2.000 |
| stretch PITCH +700, modes -1 / 1 / 2 / 5 | +698.4 / +699.7 / +694.6 / +699.7 | 2.000 |
| header +400, Stretch mode | +398.1 | 2.000 |
| header +400 + PITCH +700, Resample / Stretch | +400.0 / +1099.6 | 1.587 / 2.004 |
| root 67, Stretch / fine +50, Stretch | -701.3 / +48.1 | 2.001 / 1.999 |
| TIME 1 beat, Resample / Stretch + PITCH +700 | +1823 (2 s squeezed) / +696.7 | 0.698 / 0.692 |
| MUL x2 without TIME, Resample / Stretch | -1200.0 / -0.5 | 4.000 / 4.004 |
| Sampler channel, key C5: PITCH +700 Resample / Stretch | 0.0 / +698.4 | 2.000 / 2.000 |
| Sampler channel: header +400 Stretch; key G5 Resample / Stretch | +398.1; +700.0 / +698.4 | 2.000; 1.335 / 2.000 |
| Sampler channel: MUL x2, Resample / Stretch | -1200.0 / -0.5 | 4.000 / 4.004 |

So an audio clip plays like a C5 note of its channel: header pitch, root note and fine tune all transpose it. Every
mode but Resample keeps the length (also without a TIME) and adds the PITCH knob; Resample ignores the PITCH knob.
MUL without a TIME stretches the sample by 2^(MUL/10000). Sampler channels follow the same rules.

## 3. Changes

- `src/flp/Flp.*`: reads the PITCH knob (0xD7 +0x64, `stretchPitch`).
- `src/core/FlpImport.cpp`: the round-163 line that zeroed an audio clip channel's pitch is gone (header pitch + root
  note reach it again); new `applyFlStretchPanel` for Sampler and audio clip channels: a non-Resample mode turns on
  "keep length" (with its stretch method) and adds the PITCH knob; MUL without TIME becomes a stretch in beats at the
  project tempo (needs the sample's length: new `FlpImportOptions::sampleSeconds`, filled by AudioEngine from the
  decoded sample, which the rack reuses).
- [TEST] `c164p_fl_clip_and_sampler_pitch_import` (every row of §2 as an import case), `c164p_stretch_panel_rules`.

## Where I stopped

- (updated at landing)
