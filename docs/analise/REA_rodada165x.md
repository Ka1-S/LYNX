# Round 165x "parity165x": Glow ← CLA-2A attack and Auto Makeup, Hush ← Pro-DS ranges and link (sub-round of 165h)

2026-10-28. Black-box only: the official trials (Waves 17.1 CLA-2A, trial ends ~10-31; FabFilter Pro-DS, ends ~11-16)
hosted in our own `tools/fxmeasure` (public VST3 parameters, saved states, rendered WAVs), no decompiling. Data:
`D:\lynx-work\parity165x\{glow,pds}`; scripts: job tmp `parity165x/` (`att.py`, `glow/gen.py`, `glow/attfit.cpp`,
`glow/cmpatt.sh`, `glow/amg.py`, `pds/gen.py`, `pds/lk.py`, `pds/cmplink.sh`).

## Where I stopped

2026-10-28 12:25 (resumed after the 2nd BSOD, 09:14): all four items are coded (commits 4b1b4948 + checkpoint
f5fd3e39, the attack's slow-cell cap / low-pass refit). Now: rebuild fxlynx + tests, re-run `glow/cmpatt.sh` and the
tests, check the first-10-ms overshoot, then merge origin/main (23 behind), build-local with FL Data, Windows build,
--plugui shots of Glow / Hush / Ember, land.

## 1. Glow Leveler ← CLA-2A: the attack

Signals (`glow/gen.py`): 1 s of silence, then a 1 kHz burst at 0 / −6 / −12 / −24 dBFS (and a −12 → 0 dBFS step),
Compress and Limit at peak reduction 46 / 62.6 / 80, fresh instances (Gain 40, Hi Freq 50). Analysis (`att.py`): the gain
in 1 ms windows from the onset.

The trial lets the transient through. Its gain falls ≈ 2 dB per ms whatever the drive:

| Case | 0 ms | 1 ms | 2 ms | 4 ms | 10 ms | 49 ms |
|---|---|---|---|---|---|---|
| Limit 80, 0 dBFS | −0.90 | −2.93 | −4.77 | −7.86 | −13.06 | −21.66 |
| Compress 62.6, −12 dBFS | −0.79 | −2.43 | −3.87 | −6.16 | −9.49 | −11.31 |

Round 123's attack (a log-domain approach, tau / (1 + C)^beta) clamped Limit within the first millisecond (−13.5 … −19.6 dB).

Model (`attfit`, Nelder–Mead over 1 ms windows of the first 100 ms of 30 onsets, plus round 152a's Limit stairs held in
the cost): the fast cell approaches its drive in the linear domain (tau / (1 + C)^beta), its rise is limited to
rate · (1 + C)^rateBeta per ms, and its drive is capped (ctMax). The slow cell takes its own cap (ctMaxSlow) through a
low-pass (slowLpMs). Without that, the fast cell's cap also starved the slow cell and broke round 152a's Limit stairs
(Limit 62.6 at −6 / 0 dBFS: −11.9 / −16.5 dB against the trial's −13.7 / −19.6).

After the 2nd BSOD the fit was re-checked against every earlier measurement (`glow/evtest.py`: the test points of round
165w's recoveries, round 152a's Limit stairs and the onsets above, on `attfit` dumps; attfit reproduces LYNX's numbers
to 0.01 dB). One slow-cell law could not hold both: the smoothed drive that builds Limit's deep stairs (−19.6 dB at
0 dBFS, PR 62.6) also deepened Compress 46's recovery after a 2 s burst by 0.7 dB. The trial does both, so the smoothing
applies in Limit only; the slow drive is capped at 250 in both modes.

| First 50 ms, 1 ms windows (rms / worst onset dB) | Round 123's attack | Measured (165x) |
|---|---|---|
| All 30 onsets + stairs + recoveries (mean rms) | 2.36 | 0.91 |
| Limit 80, 0 dBFS | 6.76 / 14.8 | 1.94 / 2.4 |
| Limit 62.6, 0 dBFS | 5.84 / 12.7 | 1.40 / 2.1 |
| Compress 80, 0 dBFS | 3.60 / 7.9 | 1.27 / 1.3 |
| Compress 46, 0 dBFS | 0.25 / 0.5 | 0.49 / 1.1 |

Compress 46 (the factory setting) was already right with round 123's attack and is now within 1.1 dB; every Limit
and every loud Compress onset is much closer. The trial's first millisecond still reduces ≈ 0.8 dB more than LYNX's
(−0.9 against −0.0 in Limit 80): no rate law fitted both that and the 2 dB per ms that follows.

## 2. Glow Leveler ← CLA-2A: Auto Makeup's reference

Fresh-instance runs f1 … f11 (`glow/amg.py`), a −40 … 0 dBFS 1 kHz stair, saved states read back:

- Auto Makeup is a pure output gain: level-independent within 0.03 dB, the same in Compress and Limit:
  f(PR) − f(PR_ref) on the measured table (`amGain`).
- PR_ref is the dial's position when Auto Makeup was switched on. Switching it on at P gives 0 dB, and so does setting
  both before processing. On at 46 then moved to 80: +11.37 dB; on at 30 then to 80: +12.39 dB; 46 → 80 → 62.6 → 80: 11.37.
- Switching Auto Makeup off keeps the make-up: the trial turns its own Gain (the saved RealWorld index 38 goes
  40 → 53.79 while the host's Gain parameter still reads 40).
- After a state load, peak-reduction moves do not re-arm the reference.
- Round 152's translator therefore counted it twice (Gain 53.79 plus LYNX's own +11.37).

LYNX: appended **Makeup ref** (Glow index 11, default 46 — the factory value, so older projects are unchanged). Switching
Auto Makeup on sets the reference to the dial, switching it off folds the make-up into Gain, as the trial does. The
CLA-2A translator sets the reference to the state's peak reduction (the state's Gain already holds any folded make-up).

## 3. Hush De-esser ← FabFilter Pro-DS: ranges and Stereo Link

Saved states swept through the public parameters (`pds/lk.py`, il / ol / hp / sl / lp tables):

- Input / Output Level: dB = 72 v − 36 for v ≥ 0.2, 40 · log10 (v) + 6.3588 below (0 = −∞).
- High-pass: 2000 · 10^v Hz (2 … 20 kHz).
- Stereo Link: 0 … 1 with 0.5 = 100 %; above that "100 %, n % Mid-only" (or Side-only with the mode Side, state index 5).
- Link k < 100 %: each channel detects on (1 − k/2) · own + (k/2) · other; 100 % detects on the mid.
- Mid-only m: out = (1 − m) · g · x + m · MidOnly (g), g from the mid detection.
- Side-only m: L detects on (1 − m) M − m S, R on (1 − m) M + m S; out_L = L + (g_L − 1) ((1 − m) L + m S),
  out_R = R + (g_R − 1) ((1 − m) R − m S) — verified on the link matrices at 125 / 150 / 200 %.

LYNX: appended **Range law** (Old ranges | Extended; new instances Extended, older projects Old), **Link only** and
**Link only side** (Mid only | Side only). Extended: levels −∞ … +36 dB, high-pass to 20 kHz, the link's
above-100 % half; the classic voicing keeps the old law. The Pro-DS translator reads all of them from the state.

Link matrices (`pds/lk.py`: a tone in one channel, the gain on both, mid and side modes at 0 / 50 / 100 / 125 / 150 /
200 %): LYNX within 0.014 of the trial. The editor's STEREO LINK knob now runs 0 … 200 % as the trial's does: one knob
over Stereo link (first half) and Link only (second half), its value "100 %, 40 % mid only"; the Mid | Side switch
under it picks the upper half's signal (the Old ranges keep round 128's MID button).

## 4. maxEffectParams 160 → 192

Hush now has 25 parameters and the largest engines were near the old 160 cap. Raised to 192 (Song.h); the selftest
old-project cuts add 160, the Control Surface follows the limit. Test: an effect with 192 parameters round-trips through
the preset text format.

## 5. Ember Classic footer hint

The Classic panel's footer now shows its own hint (the five Classic styles), with pt-BR text.

## 5b. Glow's VU display is saved

[EXEC] `glow/m4.txt`: the trial's VU DISPLAY knob is stored in its state (RealWorld 17: 0 In, 1 GR, 2 Out). LYNX kept
the selector in the editor only; it is now an appended parameter **VU display** (0 gain reduction — the default, as
before —, 0.5 input, 1 output), saved with the project and translated from CLA-2A states.

## 6. Tests

`tests/Parity165xTests.inc`: Glow's measured onsets (Compress / Limit), Auto Makeup's reference and fold, Hush's
extended ranges and upgrade, the Pro-DS stereo link (mid- and side-only blends), the translations, 192 parameters,
the Ember footer. Older tests adjusted for the appended Hush parameters.
