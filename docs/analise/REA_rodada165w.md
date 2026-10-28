# Round 165w "parity165w": Glow ← CLA-2A release, Gemini ← Doubler shelves (sub-round of 165h)

2026-10-28. Black-box only: the official Waves 17.1 trials (trial ends ~10-31) hosted in our own `tools/fxmeasure`
(public VST3 parameters, rendered WAVs), no decompiling. Data: `D:\lynx-work\parity165w\{glow,dbl}`; analysis and
fitting scripts: job tmp `parity165w/` (`glow/gen.py`, `glow/gr.py`, `glow/glowfit.cpp`, `dbl_resp.py`, `dblfit.py`).

## Where I stopped

- 2026-10-28 05:05: §2 (Glow release), §3 (Gemini shelves) and §5 (Hush ← Pro-DS pans, state levels) measured,
  fitted, built and verified with `fxlynx` against the trial renders; tests 0 failures; PARITY rows / scores and the
  0.1.20 notes updated; landing queued (`coord/land-parity165w.out`).
- Trials: Waves 17.1 CLA-2A and Doubler4 played on 10-28 ~03:40; FabFilter Pro-DS on 10-28 ~04:40.
- Left: Glow's attack overshoot (first 10 ms of a burst) and Auto Makeup's reference point; Gemini's design items
  (numeric boxes, source drop-downs); Hush's −∞ … +36 dB levels, 20 kHz high-pass, Stereo Link's mid-only half.

## 1. Method

- Trial host: `fxmeasure.exe` in `D:\lynx-work\parity165w\{glow,dbl}`, with `fxmeasure-cache.xml` in the cwd and
  "DeEsser Mono" loaded first (the shell quirk), every run under the machine-wide trial lock.
- Gain trajectories: 1 kHz tones, 10 ms windows of out / in (dB), each file's own pre-burst probe as the 0 dB point.
- Shelves: 31 stepped tones (20 Hz … 20 kHz, 0.25 s each, −20 dBFS), one voice alone (0 dB, centre, 0 ms, 0 cents,
  depth 0), direct off; each response relative to both shelves off.

## 2. Glow Leveler ← CLA-2A: the release

Signals (`glow/gen.py`): 1 s of a −40 dBFS 1 kHz probe, a burst, then the probe for 25 s (30 s after 10 s bursts).
Fresh instances (Gain 40, Hi Freq 50), `reset` + 3 s of silence before each file:

| Mode / peak reduction | Bursts |
|---|---|
| Compress 46 (default) | 0 dBFS for 0.1 / 0.5 / 2 / 10 s; −10 and −5 dBFS for 2 s |
| Compress 80 | −12 dBFS for 0.1 / 0.5 / 2 / 10 s; −24 and 0 dBFS for 2 s |
| Compress 62.6, Limit 62.6 | −6 dBFS for 0.5 / 2 / 10 s |

**The trial [EXEC]** (gain against the probe's own level, after the burst ends; Compress 46, 0 dBFS for 2 s, ≈ 10 dB
of reduction at the end): −7.56 dB at 50 ms, −6.20 at 0.1 s, −4.38 at 0.2, −2.18 at 0.5, −1.29 at 1, −0.84 at 2, −0.31
at 5, −0.06 at 10, 0.00 at 20 s. Two things stand out:
- **No long memory.** After 0.5, 2 or 10 s of the same reduction the recovery is the same within 0.02 dB (the reduction
  itself deepens by ≈ 0.2 dB over the first second, then holds). Only a 0.1 s burst recovers a little faster (it left
  less reduction).
- **The tail ends.** Back within 0.1 dB after ≈ 10 s and 0.01 dB after 15–20 s at every setting (Limit too:
  −13.3 → −3.03 at 0.5 s → −0.41 at 5 s → −0.08 at 10 s).

Round 123's model (fitted on her takes) had a slow conductance with a 27 s attack and an `−a Cⁿ` release with n ≈ 3.9:
it charged more the longer the reduction lasted and then held **≈ 0.8 dB for many seconds** (−0.84 dB 10 s after a 10 s
burst at 46, −0.99 at 62.6), the "≈ 1 dB slower" round 152 saw on stairs.

**Fit** (`glow/glowfit.cpp`: the leveler's loop as in `opto::Leveler::process`, every release constant free, plus a
linear term on each conductance: dC/dt = −a Cⁿ − b C; Nelder-Mead over all 18 trajectories, 10 ms windows, the first
3 s of each recovery weighted ×10): fast cell a 3.771, n 1.845, b 4.513; slow cell share 0.626, attack 4.62 s, a 8.631,
n 3.366, b 0.338. The static curve, attack and Limit (round 152) are untouched.

| Trajectory rms (dB) | round 123 | round 165w |
|---|---|---|
| all 18 (burst + recovery) | 0.61 | 0.24 |
| recovery only, per file | 0.08 … 0.84 | 0.03 … 0.15 |
| Compress 46, 0 dBFS 2 s, gain at 0.5 / 2 / 10 s | −2.55 / −1.21 / −0.84 | −2.52 / −1.02 / −0.07 (trial −2.18 / −0.84 / −0.06) |
| held out: round 152's 0.5 s stairs, falling half, Compress 46 / 62.6 / 80 | 0.52 / 0.76 / 0.65 | 0.35 / 0.39 / 0.45 |
| same, Limit 46 / 62.6 / 80 | 0.38 / 2.01 / 1.97 | 0.23 / 1.96 / 1.95 |

(Limit 62.6 / 80 on the stairs is dominated by round 152's over-compression model, not the release.) What is left is
mostly the first window of each burst (the attack overshoot, up to 2.7 dB in Limit, unchanged) and a 0.3 dB deeper
static reduction at 46 / 0 dBFS.

LYNX: `vmatch::opto` (core/VoiceMatch.{h,cpp}): `Settings::measuredRelease`, the two release laws (`release123` = the
old constants exactly, `release165`). Glow (core/MixSuite.{h,cpp}): appended **Opto release** (index 10; Measured for
new instances and translated CLA-2A states, Old = round 123 for projects / user presets saved before:
`upgradeLoadedEffectParams`); editor: "Release" Old | Measured tabs top right (under the VU selector) with the classic
voicing.

## 3. Gemini Doubler ← Doubler2 / Doubler4: the shelves

Parameters (Doubler4 Stereo, 17.1): `EQLowGain` / `EQHighGain` are linear in dB over the normalised value:
**dB = −88 + 100 · v** (0 → −88, 0.88 → 0, 0.921 → +4.1, 1 → +12; read with `texts`, 200 steps). Frequencies 16 …
21357 Hz (log), as before.

Measured: high shelf at 2448 Hz and low shelf at 281 Hz for −88, −80, −70, −60, −50, −40, −30, −24, −18, −12, −6, 0,
+6, +12 dB, plus −88 / −40 / −24 dB at 101 and 1189 Hz (both shelves): 38 responses.

**Law found [EXEC]:** each shelf is **two identical first-order sections**, each with half the gain in dB, whose
fixed corner is at **f0 · 1.557** (high shelf) or **f0 / 1.5605** (low shelf; fitted separately); the other corner moves with the gain
(high: zero at fp / √g, low: zero at fp · √g). So the curve is a plain shelf near 0 dB and turns into a **12 dB / octave
cut at the fixed corner** below about −24 dB (the manual's "the floor drops to make a cut filter"; −80 and −88 are the
same filter in the audible band). Fit with one constant for all 38 responses (bilinear, the fixed corner pre-warped):
rms 0.012 dB (high) / 0.034 dB (low), worst point 0.25 dB (low −40 dB at 101 Hz, 20 Hz).

Round 124's LYNX shelf (one first-order section, corners f0 and f0 · max (g, 1/g), gain −30 … +12) against the same
data, deeper settings clamped to −30: rms 3.61 dB mean (max 10.5 dB mean, 56 dB worst: −88 dB high shelf at 101 Hz);
even inside its range it was 1.0 … 1.5 dB rms off at ±12 / −24 (the slope is twice as steep and the corner differs).
After: **mean rms 0.020 dB, mean max 0.08 dB** (the law). The LYNX engine itself, rendered with `fxlynx` on the same
stepped tones (one voice, the same settings): high shelf 0.002 … 0.003 dB rms (max 0.011), low shelf 0.017 … 0.096 dB
rms (max 0.52 dB, the −40 dB cut at 101 Hz near 30 Hz).

LYNX (`voicetools::gemini`, core/VoiceTools.{h,cpp}):
- appended parameter **EQ law** (index 54): Measured (new instances, translated states) | Old (round 124: a project or
  user preset saved before keeps its gains and shapes — `upgradeLoadedEffectParams`; user presets now go through the
  same upgrade by the number of parameters they carry);
- `shelfDb (v, measured)`: −88 … +12 dB (measured) or −30 … +12 (old); `shelfValue (dB, measured)`;
- the measured shelves are one biquad each (the square of the first-order section); `shelfResponseDb` is the analog
  prototype for the editor;
- defaults stay the trial's (round 165h): low 0 dB at 101 Hz (0.88), high +4.1 dB at 1306 Hz (0.921);
- editor: the EQ graph's height is the law's own range (−88 … +12 measured), the curve is the law's shape, the L / H
  markers sit on their gain values; **Old | Measured** tabs at the top right of the Direct row;
- translation (`translateWavesDoubler`): the shelves' gains one to one over −88 … +12, EQ law Measured.

## 5. Hush De-esser ← FabFilter Pro-DS (follow-up, branch parity165w2)

Parameter list of the trial (`pds/p.tsv`): Mode, Threshold, Range, Band Processing, Stereo Link (0 … 100 %, then
"100 %, n % Mid-only" up to 200 %), Stereo Link Mode, Lookahead 0 … 15 ms + Enabled, Audition Triggering, Side Chain
Input, High-Pass / Low-Pass 2 … 20 kHz, Audition Side Chain, MIDI, Oversampling Off / 2x / 4x, **Input / Output Level
−∞ … +36 dB** (0.5 = 0 dB, 72 dB per unit above 0.25 = −18 dB, a steeper curve below), **Input / Output Pan −1 … +1**,
Bypass. Fresh instance: Single Vocal, −36 dB, 6 dB, Wide Band, 100 %, look-ahead 10 ms on, 7.5 … 14 kHz; latency 720.

- **Pan law [EXEC]** (L = 1 kHz, R = 1.5 kHz, threshold 0 dB, range 0; every quarter step): a balance — the far channel
  × (1 − |pan|), the near one untouched, nothing crosses over; Input Pan and Output Pan alike.
- **State units [EXEC]** (a component saved after setting Input Level 0.6 = +7.2 dB, Input Pan −0.5, Output Pan +0.75,
  2x): the 20-value list stores the pans as −1 … +1, the oversampling as an index, and **the levels as 2 v − 1**
  (+7.2 dB → 0.2). LYNX's translator read the output level as dB (+7.2 dB came back as +0.2 dB); fixed (36 dB per unit,
  Hush's ±12 dB clamp), and the input level, pans and oversampling are now translated.

LYNX: Hush appended **Input pan / Output pan** (indices 20 / 21, centre by default, so older projects are unchanged):
input after the input level (the detector hears it), output after everything; "In pan" / "Out pan" dials in the panel.
Left: the levels' −∞ … +36 dB range, the 20 kHz high-pass, Stereo Link's mid-only half.

## 4. Tests

tests/Parity165wTests.inc: Gemini's measured shelves against the trial's points (−88 high / low, −24 at 1189 Hz, +12,
−12; engine within 0.3 dB, the editor's curve within 0.1 dB of the engine), the old law kept and shown on its own scale, older projects /
presets → Old, deep cuts translated; Glow's measured release against the trial (Compress 46 after 2 s and 10 s, Limit 62.6;
within 0.45 / 0.6 / 0.7 dB), round 123's tail kept for older projects, the upgrade and the CLA-2A translation; Hush's pans (balance in / out) and the
Pro-DS state's levels, pans and oversampling.
