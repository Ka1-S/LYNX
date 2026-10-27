# Round 163c (workflow163c): sample pitching — "pitching didn't work for me and getting to the sample pitching"

2026-10-26. User feedback (FL user, Windows): pitching samples did not work and was hard to find. Evidence markers:
[CODE] read in LYNX's source; [TEST] tests/Workflow163cTests.inc (build-local); [WIN] rendered with the Windows build
(`LYNX.exe --render`) and measured (tools/pitchcheck/pitchcheck.py); [ASSUMPTION] not checked in FL.
Builds on round 162c (docs/analise/REA_rodada162c.md: fine tune for live keys, stretch obeys the keys, pitch wheel).

## 1. What was broken

| Path a FL user takes | Before | Cause | Now |
|---|---|---|---|
| Sample dragged into the Playlist (an audio clip) → Clip properties › Pitch, or the channel window's PITCH knob, or Misc › Fine tune | nothing happened [WIN: +7 st rendered at 440.0 Hz = 0 cents] | [CODE] `SamplerInstrument` in clip mode used `semitones = 0` — the clip's channel pitch was stored and shown but never played | the clip's channel pitch (+ fine tune) transposes it: resample (speed and length follow, the Playlist shows the file at its new speed) or, with "Keep length", the stretch engine |
| Clip properties › "Keep the pitch" | greyed out unless the clip was stretched to the tempo | [CODE] `clipprops::apply` dropped keepPitch without a stretch | always available; renamed "Keep length (stretch engine)" |
| Sample channel with "Stretch keeping pitch" and no Stretch beats, playing keys | keys resampled (length changed) | [CODE] the voice pitch shifter (round 162) only ran with a stretch | "Keep length" alone makes the keys / pitch / fine tune keep the length |
| Finding it | pitch only in Channel settings (Sampler page / header knob ±2 st / Misc) or Clip properties | — | right-click › "Pitch... (semitones, cents, keep length)" on a sample channel (Channel Rack) and on an audio clip (Playlist clip menu); hint bar mentions it; tooltips explain root note / chromatic keys |

The Piano Roll already played a sample channel chromatically around its root note (key − root + pitch) [CODE][TEST
s162]; that was not broken — the dead control was the audio clip's.

## 2. Changes

- `src/core/Instruments.cpp`: clip voices get `pitch + fine tune` (not the main pitch: clips never followed it);
  `keyShift = keepPitch` for samplers and clips (pitch shifter per voice, length kept); a resampled clip started in its
  middle reads at the new speed. Pitch 0 = untouched (bit-identical with or without keep length [TEST]).
- `src/core/RackRenderer.cpp`: the shifters are allocated for audio clip channels too; clips don't take the main pitch.
- `src/core/ClipOps.h`: `clipSpeed`, `framesPerTick/fileTicks (ChannelDef)` (pitch included), `rescaleClipsOf`,
  `setChannelPitch` (the Pitch... command: semitones + cents, keep length; a resampled audio file's clips are rescaled
  so they keep the same audio, like "Fit to tempo").
- `src/ui/Dialogs.*`: the Pitch dialog (semitones, cents, Keep length, Reset). Menu items in ChannelRackComponent and
  PlaylistComponent; Clip properties pitch now 0.01 st steps; Channel settings texts.
- `src/core/FlpImport.cpp`: an FL audio clip channel's header pitch / root note no longer reach `sampler.pitch` (LYNX
  played them untransposed; keeps imported projects sounding as before) [ASSUMPTION: FL's own clip transposition is
  its time-stretch pitch, not read yet].

## 3. Measurements [WIN]

Windows build (build-win-workflow163c), `LYNX.exe --render <case>.lynx out.wav 4`, 44.1 kHz; a 1 s 440 Hz sine and
FL's "Piano Rhodes (3).wav" (0.715 s, Ogg-in-WAV). Pitch = harmonic-sum FFT peak on the steady middle, relative to the
untransposed render; length = until −60 dB. Data: D:\lynx-work\workflow163c\pc.

| sample | case | expected (cents) | measured | error | length (s) | expected (s) |
|---|---|---|---|---|---|---|
| sine440 | clip +7 st | +700 | +700.0 | 0.0 | 0.667 | 0.667 |
| sine440 | clip −12 st | −1200 | −1200.0 | 0.0 | 2.000 | 2.000 |
| sine440 | clip +50 cents (pitch knob) | +50 | +50.0 | 0.0 | 0.972 | 0.972 |
| sine440 | clip +50 cents (Misc fine tune) | +50 | +50.0 | 0.0 | 0.972 | 0.972 |
| sine440 | clip +7 st keep length | +700 | +700.0 | 0.0 | 1.000 | 1.000 |
| sine440 | clip −12 st keep length | −1200 | −1200.0 | 0.0 | 1.000 | 1.000 |
| sine440 | clip +50 cents keep length | +50 | +50.0 | 0.0 | 1.000 | 1.000 |
| sine440 | sampler key G5 / C4 | +700 / −1200 | +700.0 / −1200.0 | 0.0 | 0.667 / 2.000 | same |
| sine440 | sampler key G5 / C4 keep length | +700 / −1200 | +700.0 / −1200.0 | 0.0 | 1.000 / 1.000 | 1.000 |
| Rhodes | clip +7 / −12 / +50 c | +700 / −1200 / +50 | +700.0 / −1200.0 / +50.0 | 0.0 | 0.477 / 1.430 / 0.695 | same |
| Rhodes | clip +7 / −12 / +50 c keep length | +700 / −1200 / +50 | +700.8 / −1200.7 / +50.0 | ≤ 0.8 | 0.715 | 0.715 |
| Rhodes | sampler G5 / C4 / +50 c keep length | +700 / −1200 / +50 | +700.8 / −1200.7 / +50.0 | ≤ 0.8 | 0.715 | 0.715 |

Before (the 0.1.17 build, same projects): clip +7 st → 440.0 Hz (0 cents), 1.000 s; clip +7 st keep length → 0 cents.

## Where I stopped

- Done: items above; tests/Workflow163cTests.inc (6 tests); Windows renders measured (table).
- Not done: FL's audio clip "time-stretch pitch" from .flp (0xD7 record) is not read; FL black-box check of whether FL's
  main pitch moves audio clips (LYNX: it doesn't) — not measured.
