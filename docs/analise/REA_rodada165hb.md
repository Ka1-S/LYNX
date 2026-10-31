# REA round 165hb (parity165b) — LYNX Tune ← Auto-Tune Pro toward 10/10

Sub-round of r165h "parity165". Pair: **LYNX Tune ← Auto-Tune Pro (Auto mode)**, before: config 8 / design 7.
Black-box only: Auto-Tune Pro 11.6 (`Auto-Tune Pro.vst3`) and AutoTune 2026 (`AutoTune.vst3`) hosted in
tools/fxmeasure (public VST3 parameters, value texts, audio in / out, editor screenshots). Data:
`D:\lynx-work\parity165b\at` (in/ test signals, out/<campaign>/ renders, *.png editor shots); harness
`tmp/parity165b` (py/atrun.py, camp*.py, anaprobe.py, anath.py; fxlynx.cpp).

## Where I stopped (PAUSED by the parent at ~16:20 — load limit; nothing running)
- Code (uncommitted → committed as WIP, does NOT compile-check yet, not built): `LynxTune.h` appends six params after
  `referenceToneLevel`: throatLength, vibratoAmplitude, vibratoFormant, vibratoVariation, transposeInScale,
  scaleTranspose (+ helper declarations); `LynxTune.cpp` has throatPercent / throatValue / throatSemitones (§2 law).
  STILL TO DO in code: info() entries ("Throat length" 0.5, "Vibrato amplitude" 0, "Vibrato formant" 0, "Vibrato
  variation" 0, "Transpose in scale" 0, "Scale transpose" 0.5), paramText, the other helpers
  (vibratoAmplitudeOf, scaleTransposeSteps, transposeInScaleNote), engine use (throat only when Formant preserve is
  on; created-vibrato amplitude / formant / variation; transpose in scale), PluginSubstitutes: X71 → throatLength
  (drop round 138's half law; also the EFX+ pThroatLength path), X47/X48/X49 → vibrato amp/formant/variation, X179/X181
  → transpose in scale; update tests/Vmatch127Tests.inc throat checks; new tests/Parity165bTests.inc; editor layout
  (THROAT knob in OUTPUT where SHIFT is, SHIFT moved to Advanced › Voice shifter; Advanced › Create vibrato gets
  AMPLITUDE / FORMANT / VARIATION; an IN SCALE toggle + scale-steps stepper by Transpose); pt-BR strings.
- Measurement NOT yet run: `tmp/parity165b/py/camp3.py` (fixed to use `set` with normalised values: Vibrato Shape
  sine = 0.3333, square = 0.6667; Transpose In Scale = 1 — `settext` with "Sine Wave" / "In Scale" did not parse).
  Run it via buildslot.sh with the load gate (< 14) when resumed, then write an analysis (YIN pitch from
  tmp/vox138/py/sig.py, RMS envelope for amplitude, harmonic-envelope scale at pitch peaks vs troughs for formant,
  peak spacing / depth spread for variation, median pitch per note for transpose in scale).
- Defaults of the reference (for the record): Vibrato Amplitude 10 (0 … 30), Formant 70 (0 … 100), Variation 20 (0 …
  100), Throat Length 100 % (50 … 180), Scale Transpose 0 (−7 … +7, 15 steps), Transpose In Scale "Ignoring Scale".

## 2. Throat length measured [EXEC]
Single-resonance probe (+24 dB, bw 60 Hz, at 400 … 4000 Hz) under a 147 Hz harmonic source, Retune 0
(`out/probe`, anaprobe.py, latency 2670): resonance shift in semitones by throat length —

| L % | 700 | 1000 | 1500 | 2000 | 3000 | −12·log2(L/100) |
|---|---|---|---|---|---|---|
| 50 | +13.6 | +13.2 | +12.6 | +13.0 | +11.0 | +12.0 |
| 60 | +10.8 | +9.7 | +9.1 | +9.7 | +9.3 | +8.8 |
| 72 | +7.7 | +6.6 | +5.6 | +6.2 | +6.0 | +5.7 |
| 85 | +1.7 | +3.5 | +2.9 | +3.6 | +3.1 | +2.8 |
| 118 | −0.4 | −2.1 | −2.6 | −2.3 | −2.4 | −2.9 |
| 135 | −2.6 | −5.3 | −5.4 | −4.9 | −4.5 | −5.2 |
| 180 | −8.2 | −9.4 | −9.8 | −9.3 | −9.4 | −10.2 |

The 400 Hz probe never moves (+1 st at every L: the envelope at the 2nd/3rd harmonic is not shifted). Law adopted:
−12·log2(L/100) × 1.08 below 100 %, × 0.93 above. Formant Correction off: throat 50 % changes nothing (vowel renders
`out/throat` fcoff vs fcoff_th50: identical). Round 138's half-size law came from the synthetic vowel (its envelope is
too smooth for the scale fit: the same vowels give only +2.5 st at 50 % here) — superseded.
Transpose +5 with Formant Correction on: resonances 1000 … 4000 Hz stay within ±1 st; off: they move +4.7 … +5.6 st.

## Notes for the parent
- The repeated 0x7E bugchecks: Antares / Soundtoys trials are iLok (PACE) protected, which installs a kernel driver;
  worth checking the dumps' faulting module before more trial-hosting rounds [model, not verified].
