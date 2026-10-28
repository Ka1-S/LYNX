# Round 165h "parity165": third-party "inspired by" engines toward 10/10 (parent)

2026-10-27. Black-box only (official trials in our own host `tools/fxmeasure`, public VST3 parameters, manuals,
measurements). Sub-rounds: `REA_rodada165ha.md` (Ember ← Decapitator), `REA_rodada165hb.md` (LYNX Tune ← Auto-Tune
Pro). The parent takes Ridge Strip ← SSL E-Channel (Waves trial ends ~10-31).

## Where I stopped

- 16:05 — resumed after the 15:26 BSOD. Checkpoint 699386ae (Ridge: "EQ to SC" = the reference's Dyn S.C., model 0
  + Ridge's own engine; LED ladders; per-curve gain read-out; translated from RealWorld 26) is UNVERIFIED: build and
  measure next. fxmeasure with the `editor <png>` fix rebuilt: /mnt/d/lynx-work/parity165/tools/fxmeasure.exe.
- Sub-rounds relaunched: 165a (Ember ← Decapitator, worktree lynx-wt-parity165a), 165b (LYNX Tune ← Auto-Tune Pro,
  lynx-wt-parity165b). 16:13 load limit: one sub at a time → 165b paused; 16:20 LYNX Tune moved to round r165j
  (165b's work: wip/parity165b 1d756d02, for r165j).
- 19:52 Ridge verified on the trial (§1); commit 8802dc96. Next: build-local with FL Data + Windows build, --plugui
  shot, land; then the Waves trio (Glow / Hush DeEsser / Gemini) before the trial ends.
- 01:40 (10-28) Ridge landed 120f74f1; Hush DeEsser (10/8) and Gemini defaults queued to land; 165a Ember 9/9 landing.
- Queue after them: Glow ← CLA-2A release tail; Gemini shelf cut range; Hush ← Pro-DS;
  Valhalla Halo / Horizon / Orbit.

## Trial status (2026-10-27 15:30 + 16:02)

All installed trials play in fxmeasure (a 12 s sine): Waves 17.1 shell (SSLChannel, CLA-2A, DeEsser, Doubler4, Waves
Tune RT; trial ~10-31), FabFilter (Pro-DS, Pro-Q 4, Pro-C 3, Pro-G, Pro-L 2, Saturn 2; ~11-16), Soundtoys
(Decapitator, PanMan, LittlePlate, EchoBoy; ~11-22), Antares AutoTune (2026) and Auto-Tune Pro, Slate Fresh Air, SSL
Vocalstrip 2 / 4K E, Valhalla demos. No new trials since 10-23.

## Current scores (config / design, latest round per pair, before round 165)

See docs/PARITY.md "Current scores".

## 1. Ridge Strip ← SSL E-Channel (Waves 17.1 trial, black-box)

Harness: D:\lynx-work\parity165\ridge (cmp.txt, grid.txt, ugrid.txt: the trial with `set` / `settext`, its state saved
with `savecomponent`, the same state translated by LYNX's substitute in `fxlynx`, null per take); scripts
tmp/parity165h/{cmp.sh,null.py,ufit.py}.

- [EXEC] **EQ TO DYN S.C.** (RealWorld 26): the filters (unless Split) and the EQ leave the audio and feed only the
  detectors. LYNX (checkpoint 699386ae, now verified): de-essing setting (HF bell +15 at 6 kHz, 5 : 1, −10) on a vocal
  take and a two-tone: null −25.2 / −27.0 dB; the trial's output level within 0.4 dB.
- [EXEC] **∞ : 1**: the state stores ∞ as 100; the translator took it as 20 : 1 (`> 1000`). Fixed (≥ 99 = ∞).
- [EXEC] **The ∞ column of the round-123 law was wrong**: on 1 kHz stairs (−60 … 0 dBFS, 2 dB steps, fast attack) the
  trial is nearly flat above the threshold (0.05 dB/dB) while LYNX's fit gave ~0.4 dB/dB (2 … 5 dB rms off). Refit of the
  ∞ column (offset, knee, slope, make-up) per threshold +10 … −20: 0.15 … 0.50 dB rms (the 8 : 1 column checked with
  the same method: 0.04 … 0.14 dB rms after a refit, 0.23 … 0.66 as shipped — left as it is).
  Two-tone nulls, ratio × threshold × fast grid (24 settings): 2 : 1 and 4 : 1 unchanged (−31 … −62 dB); 10 : 1
  −17 … −26 → −23 … −51 dB; ∞ −0.6 … −9.9 → −20.9 … −38.9 dB. Programme (vocal take, ∞ + HF shelf + HPF, EQ in the audio
  or in the side chain): −0.6 … +5.6 → −13 … −29 dB.
- Design: the two GR LED ladders (3 / 6 / 10 / 14 / 20 dB) beside the routing (Dyn in, Analog, order, EQ to side
  chain) as on the reference; the band gain read-out shows only the curve in use; Split only in the E voicing.
- Scores: config 8 → 9, design 7 → 8. Left: the E voicing's threshold direction (clockwise = higher; changing it needs
  a state migration), output fader range, gate-open LED, Expand default.

## 2. Hush De-esser (classic voicing) ← Waves DeEsser (17.1 trial, black-box)

Harness: D:\lynx-work\parity165\dees (s.txt: 7 kHz stairs −70 … 0 dBFS in 2 dB steps of 0.3 s at thresholds −10 … −60 ×
HighPass / BandPass × Split / Wideband; f.txt / g.txt: 4.5 … 16 kHz stairs at dial 5506 and 8000 Hz; c.txt: a vocal take
at six settings), states saved with `savecomponent` and translated by LYNX's substitute.

- [EXEC] **Latency**: the trial reports 0; the classic voicing reported 96 samples (a plain delay). Now 0.
- [EXEC] **BandPass states**: 17.1 stores the side chain as HighPass 1 / BandPass 0 (RealWorld 13); the translator took
  ≥ 1.5 as BandPass, so every BandPass state played as HighPass. Fixed (0 or 2 = BandPass).
- [EXEC] **BandPass shape**: relative to its own HighPass the trial's BandPass needs 0.3 / 0.7 / 1.8 / 4.4 / 11.9 dB more
  input at 4.5 / 5.5 / 7 / 9 / 12 kHz (dial 5506) and 0.9 / 1.5 / 3.1 / 6.5 / 10.9 dB at 9 / 10 / 12 / 14 / 16 kHz (dial
  8000): the high-pass side chain followed by a 4th-order low-pass (two sections, Q 0.68) at 1.8435 × (f / 5506)^−0.139
  × the dial (0.06 dB rms at 5.5 kHz). Round 123's band-pass (Q 1.3, +3.9 dB) was up to 8 dB off.
- [EXEC] **Floor**: in Wideband the trial never reduces more than 25.6 dB (thresholds −40 … −60); LYNX went to 49 dB.
- Result: the 24 stair settings 0.10 … 0.25 dB rms (max 0.65); 4.5 … 14 kHz 0.1 … 0.35 dB rms (0.9 at 12 kHz, dial
  8 kHz, HighPass); vocal take −16 … −37 dB null (−94 dB where nothing is reduced).
- Scores: config 9 → 10, design 8 → 8.

## 3. Gemini Doubler ← Waves Doubler2 / Doubler4 (17.1 trial, black-box)

- A fresh Doubler instance on the trial (public VST3 params): Align 0, low shelf on 0 dB at 101 Hz, high shelf on
  +4.1 dB at 1306 Hz. LYNX's new instances now start the same (src/core/VoiceTools.cpp `geminiInfo` defaults); test
  `gemini_doubler_reference_defaults_round165`. VoiceToolsTests119's law test now switches the shelves off first.
- PARITY rows: Align ✅, feedback HP range ✅, voice pans ✅, master range ✅; shelves ⚠️ (the trial's gain range
  −88 … +12 dB — the deep-cut end — is not covered). Score unchanged: 9 / 8.

## 4. Ember Saturator ← Decapitator (sub-round 165a)

Punish law per style, style A's Punish curve, Auto link, one-screen Classic panel with VU: 8 → 9 / 7 → 9. Details in
REA_rodada165ha.md. Left: Punish curves of styles E / N / T / P, style A drive ≥ 7.
