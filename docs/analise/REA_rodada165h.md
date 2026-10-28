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
- Queue after them: Waves trio (Glow ← CLA-2A, Hush ← DeEsser, Gemini ← Doubler) before the trial ends; Hush ← Pro-DS;
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
