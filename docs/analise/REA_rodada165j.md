# Round 165j (tune165): vocal pitch correction at maximum quality — LYNX Tune vs Auto-Tune, Waves Tune, FL Pitcher / Newtone

2026-10-27. Goal (user): "maximum quality, but with good performance". This round owns the LYNX Tune pairs
(Auto-Tune Pro, Waves Tune Real-Time). Evidence markers: [CODE] LYNX source; [TEST] unit test; [RUN] measured with
LYNX builds; [REF] official third-party trial hosted black-box (nothing decompiled); [DECOMP] FL Studio DLL read with
REA/Ghidra (clean-room); [SRC] open-source tuner source read (licence noted); [model] LYNX design choice.

## Where I stopped

- Started: worktree ../lynx-wt-tune165 (feature/tune165), round doc. Next: baseline measurements.

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
