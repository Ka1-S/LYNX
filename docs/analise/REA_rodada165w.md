# Round 165w "parity165w": Glow ← CLA-2A release, Gemini ← Doubler shelves (sub-round of 165h)

2026-10-28. Black-box only: the official Waves 17.1 trials (trial ends ~10-31) hosted in our own `tools/fxmeasure`
(public VST3 parameters, rendered WAVs), no decompiling. Data: `D:\lynx-work\parity165w\{glow,dbl}`; analysis and
fitting scripts: job tmp `parity165w/` (`glow/gen.py`, `glow/gr.py`, `glow/glowfit.cpp`, `dbl_resp.py`, `dblfit.py`).

## Where I stopped

- (kept current below, in §5)

## 1. Method

- Trial host: `fxmeasure.exe` in `D:\lynx-work\parity165w\{glow,dbl}`, with `fxmeasure-cache.xml` in the cwd and
  "DeEsser Mono" loaded first (the shell quirk), every run under the machine-wide trial lock.
- Gain trajectories: 1 kHz tones, 10 ms windows of out / in (dB), each file's own pre-burst probe as the 0 dB point.
- Shelves: 31 stepped tones (20 Hz … 20 kHz, 0.25 s each, −20 dBFS), one voice alone (0 dB, centre, 0 ms, 0 cents,
  depth 0), direct off; each response relative to both shelves off.

## 2. Glow Leveler ← CLA-2A: the release

(see below)

## 3. Gemini Doubler ← Doubler2 / Doubler4: the shelves

Parameters (Doubler4 Stereo, 17.1): `EQLowGain` / `EQHighGain` are linear in dB over the normalised value:
**dB = −88 + 100 · v** (0 → −88, 0.88 → 0, 0.921 → +4.1, 1 → +12; read with `texts`, 200 steps). Frequencies 16 …
21357 Hz (log), as before.

Measured: high shelf at 2448 Hz and low shelf at 281 Hz for −88, −80, −70, −60, −50, −40, −30, −24, −18, −12, −6, 0,
+6, +12 dB, plus −88 / −40 / −24 dB at 101 and 1189 Hz (both shelves): 38 responses.

**Law found [EXEC]:** each shelf is **two identical first-order sections**, each with half the gain in dB, whose
fixed corner is at **f0 · 1.557** (high shelf) or **f0 / 1.557** (low shelf); the other corner moves with the gain
(high: zero at fp / √g, low: zero at fp · √g). So the curve is a plain shelf near 0 dB and turns into a **12 dB / octave
cut at the fixed corner** below about −24 dB (the manual's "the floor drops to make a cut filter"; −80 and −88 are the
same filter in the audible band). Fit with one constant for all 38 responses (bilinear, the fixed corner pre-warped):
rms 0.012 dB (high) / 0.034 dB (low), worst point 0.25 dB (low −40 dB at 101 Hz, 20 Hz).

Round 124's LYNX shelf (one first-order section, corners f0 and f0 · max (g, 1/g), gain −30 … +12) against the same
data, deeper settings clamped to −30: rms 3.61 dB mean (max 10.5 dB mean, 56 dB worst: −88 dB high shelf at 101 Hz);
even inside its range it was 1.0 … 1.5 dB rms off at ±12 / −24 (the slope is twice as steep and the corner differs).
After: **mean rms 0.020 dB, mean max 0.08 dB**.

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

## 4. Tests

tests/Parity165wTests.inc: Gemini's measured shelves against the trial's points (−88 high / low, −24 at 1189 Hz, +12,
−12; engine within 0.3 dB, editor curve within 0.4), the old law kept and shown on its own scale, older projects /
presets → Old, deep cuts translated; Glow (see §2).
