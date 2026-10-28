# Round 165j (tune165): vocal pitch correction at maximum quality — LYNX Tune vs Auto-Tune, Waves Tune, FL Pitcher / Newtone

2026-10-27. Goal (user): "maximum quality, but with good performance". This round owns the LYNX Tune pairs
(Auto-Tune Pro, Waves Tune Real-Time). Evidence markers: [CODE] LYNX source; [TEST] unit test; [RUN] measured with
LYNX builds; [REF] official third-party trial hosted black-box (nothing decompiled); [DECOMP] FL Studio DLL read with
REA/Ghidra (clean-room); [SRC] open-source tuner source read (licence noted); [model] LYNX design choice.

## Where I stopped

- Done: harness, references scored (Auto-Tune Pro ×3, Waves Tune RT, FL Pitcher DLL), LYNX baseline, Pitcher null vs
  FL's DLL (§3), experiments E1–E7 (§4.2); adopted the normalised overlap-add + 3 kHz shelf (§4.3, committed; the
  experiment switches are gone). Unit tests in `tests/Tune165Tests.inc`.
- Done too: final renders with the landed code (bit-identical to E6+E7), CPU (+1 %), the A/B listening folder
  (Desktop `LYNX-AB-afinacao`, 8 of her takes + 2 synthetic, outside git), PARITY rows.
- First landing failed on two older tests (§4.3); fixed (normalisation follows the correction activity), re-measured, re-landing.

## 1. Method: the three-way comparison harness

Off-line renders of the same takes through every corrector, then one scorer (`tmp/tune165/py/measure.py`, our own
code). Data outside git: `D:\lynx-work\tune165` (`syn/` takes, `ref/<system>/` trial renders, `lynx/<build>/`).

- **Synthetic takes with a known f0** (`py/synth.py`, 10 takes, 44.1 kHz): a source-filter voice (band-limited
  harmonics at the f0 trajectory, a fixed formant envelope per vowel, 1/f jitter, shimmer, formant-shaped aspiration)
  with s01 held notes 20–45 c off, s02 a legato C-major run with 150 c scoops in 60 ms, s03 vibrato (5.5 Hz ±50 c,
  6.2 Hz ±35 c delayed), s04 a G3→G4 glide, s05 legato octave jumps, s06 syllables (s / t / h / breath / si / ta
  onsets), s07 low male G2–C3, s08 soprano C5–A5, s09 breathy (aspiration ~10 dB under the harmonics), s10 290 Hz notes
  on a narrow 870 Hz formant (the round-157 harmonic trap). The target of every scored frame is known.
- **Her takes** (the singer's FL session of 2026-10-24 and four older projects; never in git) with the round-157
  Auto-Tune Pro renders of her own states (`D:\lynx-work\tune157\ref`).
- Metrics per take: latency (envelope cross-correlation), accuracy = median / p90 |cents| of the output f0 from the
  target over the scored frames, octave errors (% frames > 300 c off), retune time (ms from the note start until the
  output stays within 10 c for 40 ms), vibrato kept (output / input pitch spread on vibrato notes), bursts / min not in
  the dry take (round 157's spectral-flux / HNR dip counter), formant envelope distance (LPC-40 envelopes of output vs
  dry, 200–5000 Hz, dB RMS after removing the level difference), CPU per 128-sample block (LYNX).
- Systems: Auto-Tune Pro 11.6 trial [REF] (retune 20 ms = its default, retune 0, Classic mode), Waves Tune Real-Time
  trial [REF], LYNX Tune before / after [RUN], FL Pitcher (FL's own DLL in the round-30 harness, and LYNX's rebuild).
  Input Type / Range per take (low male, soprano), chromatic scale, humanize / flex-tune 0, formant correction on.

## 2. What the references do (techniques)

### 2.1 FL Studio Pitcher [DECOMP, rounds 14/15/19/30]
Already decompiled and rebuilt bit-exactly in rounds 14–30 (`REA_rodada30.md` §2, `src/core/PitcherDsp.h`): a
high-pass + 16 one-pole low-passes, an **integer-lag AMDF** over non-overlapping frames of N = 4·⌊fs/f_min/4⌋
samples, the first local minimum under 0.3 (no sub-sample refinement: ~3 c steps at 800 Hz, one frame of delay), the
nearest allowed note by the midpoint rule, a one-pole correction toward it (reset to 0 when unvoiced), then either a
4-head Hermite delay line with period splices (128-sample cross-fade, 9 samples latency; formants move) or TD-PSOLA
with two-period Hann grains (Formant on, 257 samples latency). Lessons for LYNX Tune: nothing to adopt — LYNX's YIN
with a full-rate sub-sample refinement and its peak-aligned PSOLA are finer on every axis; Pitcher's one useful trait
is its very short plain-path latency (LYNX's Low latency is 6.4 ms, round 157c).

### 2.2 FL Studio Newtone [DECOMP + EXEC, rounds 73 / 95]
Newtone's engine is zplane's (`Newtone_Engine_x64.dll`; the track class is `CzplJPositionablePSOLASource`): an
off-line analysis into notes and pitch marks, then PSOLA resynthesis with per-note pitch offset, drift (pitch
deviation scaled around the note's mean), vibrato and formant warp (REA_rodada95 §2). It is a licensed third-party
engine inside FL, so this round keeps it black-box (no new decompilation); its design (whole-note decisions from an
off-line analysis, drift scaled around the note centre) is the "graph mode" kind of correction, not the real-time one.

### 2.3 Open-source tuners [SRC] (read for technique only; no code taken)
- **x42 fat1.lv2 / Zita-AT1** (GPL-2.0-or-later, F. Adriaensen / R. Gareus): FFT autocorrelation of a Hann-windowed
  frame, normalised by the window's own autocorrelation, with a spectral low-pass weighting `1/(1 + (k f)^2)` before the
  inverse FFT (high harmonics count less: fewer octave errors on bright vowels); the period is the highest weighted
  peak above 0.8 after the first descent. Shifting = resampling with whole-period jumps and a 128-sample cross-fade
  (formants move with the pitch). Note decision with a bias toward the last note; correction filter on the error.
- **TalentedHack / Autotalent** (GPL-3.0 / GPL-2.0, J. Salwen / T. Baran): autocorrelation pitch detection via FFT,
  a PSOLA-like shifter with LPC formant correction (the input is whitened by its LPC filter, shifted, re-coloured), a
  pitch smoother and an LFO. LYNX's TD-PSOLA keeps the formants without the LPC round trip.
- Published detectors considered: pYIN (Mauch & Dixon 2014: YIN with a threshold distribution + HMM voicing), YAAPT,
  SWIPE (sawtooth-waveform-inspired, spectral). In the real-time path only causal ideas apply (a voicing hysteresis,
  the harmonic guard, spectral weighting); a CREPE-like network was not tried (CPU).

## 3. FL Pitcher: LYNX's rebuild against FL's own DLL [EXEC]

The round-30 harness (`tmp/r30/run/pit.c`, our own code: loads `Pitcher_x64.dll` unmodified, drives its own
setParameter / render) and LYNX's `pitcher::Pitcher` (`tools: tmp/tune165/tools/pitcherrender.cpp`) on the ten
synthetic takes, Formant on, Correction Speed 1 (instant) and 0.612 (~20 ms), Range 110 Hz (55 Hz for the low male,
220 Hz for the soprano): **14 of 20 renders bit-identical**; the other six null at −43 … −51 dB (soprano ×2,
breathy ×2, the formant trap at 0.612) — the CRT `log2f` rounding already noted in round 30 (one splice instant moves).
Nothing to fix.

## 4. LYNX Tune: what was measured, tried and kept [RUN]

### 4.1 Where LYNX Tune stood (0.1.19)

Synthetic takes (medians over the ten takes; Auto-Tune Pro retune 20 ms = its default):

| system | latency ms | accuracy med / p90 c | retune ms | vibrato kept | bursts / min | formant dB |
|---|---|---|---|---|---|---|
| Auto-Tune Pro (retune 20) [REF] | 61.6 | 2.8 / 10.1 | 42 | 1.01 | 30.0 | 0.18 |
| Auto-Tune Pro Classic [REF] | 2.1 | 2.6 / 8.1 | 35 | — | 29.0 | 0.32 |
| Waves Tune RT (transition 20 ms) [REF] | 5.4 | 2.3 / 12.2 | 29 | 1.02 | 18.5 | 0.26 |
| LYNX Tune 0.1.19 [RUN] | 25.4 | 1.7 / 4.4 | 38 | 0.97 | 15.7 | 0.28 |

Her 24 takes, each with the Auto-Tune Pro state she used translated into LYNX Tune (as the FL import does): Auto-Tune
median 5.79 c / p90 26.2 c, bursts 31.2 / min, formant distance 0.49 dB, HF balance −0.07 dB; LYNX 0.1.19 4.39 c /
20.9 c, 95.8 % of voiced frames on Auto-Tune's note, bursts 40.9 / min, formant 0.80 dB, **HF balance −0.52 dB**.
LYNX is already tighter on pitch; it lost on timbre (darker highs, bigger envelope change) and on bursts.

The HF loss: on corrections of ~35 c and more the corrected voice lost 3–10 dB at 2.5–6 kHz on the synthetic held
notes (s01) and ~0.5 dB on average on her takes; Auto-Tune and Waves keep the band. Two causes found: (a) the grains'
Hann windows do not sum to 1 while the output marks are re-spaced (the overlap-add sum dips between marks: a comb
that removes energy where harmonics are dense, i.e. the highs), (b) the grain's centre is not on the glottal pulse
(the HF energy is concentrated there; a grain centred off the pulse smears it).

Burst locations on her takes (0.1.19, the 21.4 / min LYNX has and the dry take does not): note changes 26 %, note
onsets 19 %, offsets 17 %, voicing dropouts 16 %, held notes 11 %; pitch slopes at note changes are no steeper than
Auto-Tune's.

### 4.2 Experiments (each one off-line, minitune = LYNX Tune alone, block 128)

| exp | change | synthetic | her takes (bursts / formant / HF) | verdict |
|---|---|---|---|---|
| E1 | voicing hysteresis (2 frames to drop voicing) | bursts 10.8, formant 0.30 | 40.4 / 0.87 / −0.60 | dropped: darker, no burst gain |
| E2 | PSOLA dropout bridge (hold the last period over short unvoiced gaps) | 14.4, 0.39 | 44.2 / — / — | dropped |
| E3 | first mark of a voiced run on the HF-energy peak | HF −0.20, 17.4 | 40.6 / 0.88 / — | dropped: fixes synthetic, not her takes |
| E5 | slow phase lock of the marks onto the HF peak | — | 55.8 / — / — | dropped: many more bursts |
| E6 | normalised overlap-add (divide by the window sum, clamped 0.5…2) | 17.6 (±2 events), — | 39.4 / 0.76 / −0.52 (5 takes better, 2 worse) | **kept** |
| E7 | high shelf 3 kHz +1.5 dB on the corrected path (was 5 kHz +1 dB) | — | 42.3 / — / −0.13 | kept with E6 |
| E6+E7 | both | see §4.3 | **40.1 / 0.77 / −0.13** | **adopted** |
| E6+E7b | shelf at 4 kHz | — | — / — / −0.28 | dropped |

### 4.3 What changed (code)

- `vocal::Psola::setNormalise` (`src/core/VocalPitch.h/.cpp`): the overlap-add also accumulates the window weights in a
  ring and divides the output by their sum (clamped 0.5…2) — on a steady unshifted note the windows already sum to 1,
  so little changes there; when the marks are re-spaced the dips between marks are gone. It takes an amount (0…1):
  LYNX Tune fades it in with the high shelf's correction activity (voiced and corrected by 5 c or more), so a voice
  already on its note and unvoiced sounds keep the plain overlap-add — fully normalised, the round-95 consonant
  transparency (−60 dB) and the round-148 held-note-in-noise wobble (110 Hz: 1.49 c > 1.2 c) tests failed at the
  first landing; faded, −97 dB and 0.69 c, and her-take scores unchanged (§4.4). VoiceShift (the other Psola user)
  is unchanged.
- LYNX Tune's high shelf on the corrected path: 5 kHz +1 dB → 3 kHz +1.5 dB (it compensates the residual softening
  of the grain at the pulse, measured HF balance −0.52 → −0.13 dB vs the dry take; Auto-Tune −0.07 dB).
- Tests: `tests/Tune165Tests.inc`.

### 4.4 Before → after (the landed code: E6+E7, the normalisation faded in with the shelf's correction activity)

| measure | Auto-Tune Pro | Waves Tune RT | LYNX 0.1.19 | LYNX 0.1.20 |
|---|---|---|---|---|
| her takes: pitch med / p90 c | 5.79 / 26.2 | — | 4.39 / 20.9 | 4.42 / 21.0 |
| her takes: same note as Auto-Tune | 100 % | — | 95.8 % | 95.7 % |
| her takes: HF balance vs dry (dB) | −0.07 | — | −0.52 | **−0.13** |
| her takes: formant distance (dB) | 0.49 | — | 0.80 | 0.78 |
| her takes: bursts / min | 31.2 | — | 40.9 | 40.1 |
| her takes, Classic / Low latency: bursts, HF | 27.9, −0.02 | — | 48.8, −0.51 | 45.4, −0.13 |
| her takes, Classic / Low latency: pitch med / p90 c | 7.64 / 28.2 | — | 4.12 / 20.5 | 4.16 / 20.4 |
| synthetic: latency ms | 61.6 (Classic 2.1) | 5.4 | 25.4 (Low 6.4) | 25.4 (Low 6.4) |
| synthetic: accuracy med / p90 c | 2.8 / 10.1 | 2.3 / 12.2 | 1.7 / 4.4 | 1.7 / 4.4 |
| synthetic: retune ms | 42 | 29 | 38 | 38 |
| synthetic: vibrato kept | 1.01 | 1.02 | 0.97 | 0.97 |
| synthetic: bursts / min | 30.0 | 18.5 | 15.7 | 17.6 (±2 events) |
| synthetic: formant distance (dB) | 0.18 | 0.26 | 0.28 | 0.36 |
| synthetic: HF balance vs dry (dB) | — | — | −0.40 | −0.04 |
| CPU per instance, 44.1 kHz block 128 (median, interleaved runs) | — | — | 64.7 µs | 65.1 µs (+1 %) |

The synthetic formant distance went up 0.08 dB (the 3 kHz shelf on held notes whose highs were not lost: the takes
with the large corrections improved, s01 2.02 → 1.89 dB, the near-unshifted ones moved 0.05–0.12 dB). On her takes
both timbre measures improved, so the shelf stays; it is a candidate for a per-shift amount next round.

### 4.5 What is left

- Bursts: 40 vs Auto-Tune's 31 / min on her takes, mostly at note changes and onsets (§4.1). None of E1 / E2 / E5
  helped; the next idea is a grain-length change at note boundaries (one-period grains for the first marks of a
  note), measured the same way.
- Large corrections on the synthetic held notes still lose highs (the grain centre vs the glottal pulse: E3 fixed it
  on synthetic takes only).
- Newtone's engine stays black-box (zplane's licensed code).
