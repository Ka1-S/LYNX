# REA round 166c (tune166c) — LYNX Tune: the controls paused in round 165hb, finished

LYNX Tune is LYNX SHARE's own pitch corrector (no FL Studio equivalent: LYNX-own item). Round 165hb (branch
`wip/parity165b`, last commit 1d756d02) measured Auto-Tune Pro's Throat Length black-box and declared six new
parameters, then was paused before any of them did anything. This round brings that branch onto current main and
finishes it: DSP, editor, Auto-Tune Pro / EFX+ state translation, translations, tests.

Black-box only (rules): the reference plugins were hosted in tools/fxmeasure (public parameters, audio in / out); no
third-party binary was opened or decompiled.

## Where I stopped
Done: everything below (code, tests, docs). Landing via land.sh; the remote `wip/parity165b` branch is deleted after.

## 1. Per item

| item | reference evidence | LYNX before | LYNX now |
|---|---|---|---|
| Throat length | [EXEC] round 165hb §2: Auto-Tune Pro 11.6, single +24 dB resonance at 700 … 3000 Hz under a 147 Hz voice: formants move −12·log2(L/100) st (×1.08 below 100 %, ×0.93 above), nothing with Formant Correction off | parameter declared, no DSP; the Auto-Tune translator approximated X71 with Formant shift and round 138's half-size law | own **Throat length** parameter (50 … 180 %, knob halves 50–100 / 100–180 %), applied on the PSOLA formant ratio with the measured law, only while Formant preserve is on; X71 and EFX+ `pThroatLength` translate onto it; THROAT knob beside SHIFT in OUTPUT |
| Created vibrato: Amplitude | defaults / ranges from the trial's own parameter texts (0 … 30, default 10; spec §3). Response **not measured**: see §2 | declared only | level swing synchronous with the created vibrato, ±A % (30 = +2.3 / −3.1 dB) [model]; X47 translated; AMPLITUDE knob on Advanced › Create vibrato |
| Created vibrato: Formant | 0 … 100, default 70 (trial texts); manual: how far the formants follow the vibrato | declared only | with Formant preserve on, the formant ratio follows the created vibrato's pitch swing by that share [model]; X48 translated; FORMANT knob |
| Created vibrato: Variation | 0 … 100, default 20 (trial texts); manual: random variation of the vibrato | declared only | a new random aim each vibrato cycle for rate (±25 %) and depth (±50 %) at 100 %, glided over about half a cycle; deterministic, real-time safe [model]; X49 translated; VARIATION knob |
| Transpose in scale / Scale transpose | Ignoring / In Scale, −7 … +7 steps (trial texts; manual "conform to scale") | declared only | In scale: the target note moves by that many steps of the scale in effect (a note outside the scale from the nearest scale note below, keeping its distance); the Transpose knob is ignored while it is on; X179 / X181 translated (not in Classic mode). The Transpose stepper becomes a "Scale steps" stepper while IN SCALE is lit |

LYNX defaults stay neutral (Throat 100 %, Amplitude / Formant / Variation 0, In scale off, 0 steps) so every existing
project sounds exactly as before; Auto-Tune Pro states always carry X47 … X49, so imported ones get the reference's own
values.

## 2. Measurement status
- Throat: measured in round 165hb (table in REA_rodada165hb §2), law in `throatSemitones`; the test checks the law
  against the measured means within 1 st.
- Vibrato Amplitude / Formant / Variation and Transpose in scale: campaign `camp3` (tmp/tune166c/py/camp3.py, data
  `E:\lynx-work\tune166c\at`) was re-run on 2026-10-31: **fxmeasure can no longer load Auto-Tune Pro.vst3** ("Unable
  to load VST-3 plug-in file") nor AutoTune.vst3 ("no plugin") — the trials have expired. Nothing was bypassed. These
  four controls are therefore built from the reference's documented meaning and parameter ranges and are marked
  [model] in the code; a future round with a licensed copy can fit them (the campaign script and the analysis plan in
  REA_rodada165hb "Where I stopped" are ready).

## 3. Editor (data-driven layout, PluginLayout.cpp `lynxTune()`)
- OUTPUT: SHIFT (22 px) and THROAT (22 px) side by side under Formant; Transpose / Scale steps stepper with the IN
  SCALE switch under it.
- Advanced › Create vibrato: Shape menu, Ignore vibrato under it (the "Pitch tracking" caption and its two hint lines
  were dropped to make room), then RATE, DEPTH, AMPLITUDE, FORMANT, VARIATION, ONSET DELAY, ONSET RATE.
- New captions in all 15 languages (THROAT, Scale steps, In scale, AMPLITUDE, VARIATION).

## 4. Tests (tests/Tune166cTests.inc)
value laws and defaults; throat moves the formant centre (72 %: up > 12 %, 150 %: down > 10 %, no effect with formant
off, pitch unchanged); transpose in scale (A3/E4 + 2 steps = C4/G4, −3 = E3/B3, knob ignored, off = knob back);
created vibrato level swing (1.6 … 2.2 ratio at 30), formant following, variation bounded and deterministic, no
allocation on the audio thread with every new control on; Auto-Tune Pro X47/48/49/71/179/181 translation (+ Classic).
Vmatch127 throat checks updated to the new parameter.
