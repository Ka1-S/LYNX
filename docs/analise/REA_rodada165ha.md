# REA round 165ha (parity165a): Ember Saturator ← Decapitator (Soundtoys)

2026-10-27. Sub-round of r165h parity165. Black-box only: the official Decapitator trial (5.5.5, until ~11-22) hosted in
our own host (tools/fxmeasure: public VST3 parameters and value texts, audio in / out), her saved states (read back
through the parameter API in round 136), measurements. Nothing was decompiled; no graphics, preset names or text copied.
Raw data: /mnt/d/lynx-work/parity165a (out/meas1_0 … meas5 trial renders, lab/ fits and logs), harness
~/.claude/jobs/f8789b49/tmp/parity165a (py/ meas*.py, evp.py, fitp.py; lab/decalab).

## Where I stopped
- Resumed after the 2026-10-27 BSOD from checkpoint 68b0ff3a (the `--plugui` shot of Ember's Classic voicing, the
  fxmeasure `editor <out.png>` fix). meas2.pkl was zeroed by the crash; its WAVs were intact and used instead.
- Done (branch feature/parity165a, pushed to origin/wip/parity165a): §1 Punish's measured law per style, §2 style A's
  Punish curve, §3 Auto link, §4 the one-screen Classic panel with the VU, tests/Parity165aTests.inc, PARITY rows.
- In progress: build-local with FL Data, Windows build, `--plugui` re-shot, landing.
- Not done (ranked): Punish curves of styles E / N / T / P (only their gain law is measured; their own curve is used);
  style A at drive ≥ 7 without Punish (round 138 §5's open item: a level-dependent even-harmonic term); Saturn 2's
  design items (drawn modulation cables, the 16-step LFO editor).

## 1. Punish's law [EXEC trial]
- The trial applies the FIRST setting after any change of Style or Punish wrongly: the same commands sent twice in a row
  give a different small-signal gain the first time (meas3: −4.1 … −9.6 dB off; meas5: also after a Style change), and
  re-sending them reads correctly and repeatably. Round 136 measured Punish at drive 0 as the first job after the toggle,
  so its +9.75 dB constant came from such a reading. Every job here is preceded by a dummy job after each toggle.
- Steady state (1 kHz at −70 dBFS, meas4): Punish adds a + k × drive dB before the curve, per style:
  A 13.83 + 0.406 d, E 14.01 + 0.479 d, N 14.14 + 0.103 d, T 14.02 + 0.512 d, P 13.92 + 0.741 d (max error 0.02 – 0.14 dB
  over drive 0 … 10). Without Punish the slopes are A 3.30, E 3.32, N 3.10, T 2.40, P 3.05 dB per unit (as round 136).
- LYNX (`emberclassic::StyleModel::punishLaw*`, `Settings::measuredPunish`): the measured law; a new appended Ember
  parameter `Classic law` (1 for new instances; projects saved before round 165 load with 0 = round 136's +9.75 dB, and
  an "Old | Measured" switch shows in the Classic panel only for them).

## 2. Style A's curve with Punish on [EXEC + model]
With the gain law alone the Punish-on renders still null only −10 dB: Punish also drives a different curve (H2 at
−20 dBFS, drive 1: trial −19.4 dBc vs −26 dBc without Punish at the same small-signal gain). Fitted in the lab (decalab,
Nelder–Mead 2 × 150, 3 threads) on drives 1 / 3 / 6 / 10 (100 Hz and 1 kHz stairs −80 … +6 dBFS, bursts, two takes):
the law, output, both knees, static and dynamic bias, the bias follower times. Check on every drive 1 … 10 incl. the
held-out drives 2 / 4 / 5 / 7 / 8, the unseen 5 kHz stairs and the third take (py/evp.py):

| Punish on, style A | mean null (63 renders) | drive 1 | drive 5 | drive 10 |
|---|---|---|---|---|
| round 136 (+9.75 dB, A's curve) | −9.47 dB | −10.1 | −9.6 | −7.9 |
| measured law, A's curve | −10.08 dB | −11.8 | −10.1 | −8.2 |
| **measured law + Punish curve** | **−17.27 dB** | **−23.1** | **−17.3** | **−11.7** |

E / N / T / P keep their own curve under Punish (only A was rendered with Punish at every drive; her states use A).

## 3. Auto [EXEC her states]
The trial's Auto changes nothing in the audio path (round 135); in its editor it turns Output down as Drive goes up.
Her 63 saved states (all Auto on) hold 11 distinct (drive, output) pairs from 1.1 / −1.7 to 5.9 / −9.4 dB, every one on
Output = −1.6 dB × Drive within the 0.1 dB read-out. LYNX: `Classic auto` (appended, default on) — an editor edit of
Drive moves Output by −1.6 dB per unit (relative, so a hand-set Output keeps its offset; clamped to −24 … 0 dB);
host automation of Drive does not move it, as in the trial (`mixsuite::ember::linkedEdit`, hooked like Echoform's
filter link).

## 4. The Classic panel [SHOT]
Trial: /mnt/d/lynx-work/vox138/trials/Decapitator.png. LYNX before: the round-136 strip (style radio left, a
Classic section, no meter, no Auto) inside a 520 × 1110 window (Ember's multiband page). Now (520 × 440, the window only
opens for the Classic voicing): Punish top left, a needle VU top centre (output level, click for input), Output with
Auto top right, the big Drive left, Low cut (+ Thump) / Tone / High cut (+ Steep) / Mix, the five styles one click each
along the bottom, "Classic" (back to Ember's engine) bottom right — LYNX's widgets and colours.
Shot: C:\Users\kauan\lynx-test\parity165a\shots\Ember Saturator-classic.png.

## Scores
| pair | config | design |
|---|---|---|
| Ember Saturator ← Decapitator | 8 → 9 | 7 → 9 |
