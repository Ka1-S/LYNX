# REA round 165g — sub-round flbeta165h: the Harmor presets that still differ from FL

Goal: make LYNX's Harmor match FL's `Harmor_x64.dll` on the factory presets that still differ after flbeta165s
(REA_rodada165g-flbeta165s.md §5), then drop "(beta)" only if every preset nulls (< −60 dB) or matches its spectrum
(STFT magnitude < −40 dB), except presets the DLL itself makes non-deterministic / explode.
Data: `/mnt/d/lynx-work/flbeta165h` (`bat.sh` → `py/full.py`: all 325 installation presets, DLL vs LYNX, 3 notes,
rack off, the DLL's Mersenne-Twister state handed to LYNX; columns idx, null dB, STFT dB, name).

## Where I stopped
- 2026-10-28 15:40 — landed: note start delay (§1), attack fade (§2), IPP Exp/Ln_32f_A11 at precision 0 (§3) with
  tests (`tests/HarmorBeta165hTests.inc`). Battery `s2.tsv`: 298 null < −60, 19 STFT-only, 8 audible. Harmor stays
  "(beta)": what is left is in §4.

## Battery history
| step | null < −60 | STFT < −40 only | audible (STFT ≥ −40) |
|---|---|---|---|
| base0 (origin/main ea719caf) | 262 | 44 | 19 |
| s1 (§1–§3) | 297 | 21 | 7 |
| s2 (§1–§3 + harmonic-randomness Exp A11) | 298 | 19 | 8 |

Runs vary a little: presets that draw from the random sources the battery cannot hand over (IPP's own uniform seeds
in some paths) land anywhere between −50 and −80 dB from run to run (Blue screen −49…−78, The ball −62…−82, Shorty
−43…−82); a preset is only counted as fixed/broken after a rerun with `py/bat_sel.py`.

Largest base0 → s2 gains (null / STFT dB): Rotochip 1.6/−9.8 → −91.1/−96.4; Harmony 0.7/−13.2 → −83.7/−90.1;
Subspace −8.7/−23.0 → −80.4/−83.7; Metal thingy −14.3/−23.5 → −85.8/−89.9; Ice bells −13.0/−36.0 → −84.4/−89.6;
Magic swamp −18.1/−35.1 → −80.8/−83.4; Morning −27.8/−31.6 → −87.6/−91.6; Oldschool −19.4/−27.8 → −78.6/−83.0;
Church bell −26.1/−46.2 → −83.1/−85.8; Pixel to freq −34.5/−44.6 → −89.5/−94.9; Steam starship −29.6/−42.3 →
−81.2/−84.3; Lead Hardstyle −33.8/−38.1 → −84.8/−88.8; Lead Hoover −34.5/−39.3 → −84.2/−88.7; Labyrinth −18.9/−39.9
→ −68.0/−79.3.

## §1 Note start delay (part parameter 2)
[DECOMP FUN_00879540 parameter 2 → part +0x1a0; FUN_0088ab10 at 0x88b150]
- Part +0x1a0 = seconds × (float)(int) sample rate, in float. Seconds: p ≥ 0 → p·0.3125/(float) tempo (tempo-synced:
  96 at 120 BPM = 0.25 s); p < 0 → (2^(−p/48) − 1)/255.
- At the record's first control block (after articulators 31…42 have stepped) d = articulator 42 × part +0x1a0. Voice
  +0x3214 is articulator 42's output (not 8, as the old note said). Any non-zero d (even one that rounds to 0 samples)
  sets the wait to nearbyint(d) and restarts the block afterwards: the record advances only its phase accumulators
  while it waits, then runs its first control block again (the prelude runs twice, like FL).
- Harmony 0.7 → −83.7 dB, Rotochip 1.6 → −91.1 dB. Test: `harmor165h_note_start_delay`.

## §2 Attack fade (part misc +4)
[DECOMP FUN_008781f0 → FUN_00863450 / FUN_00862d70 type 3 segment 0; FUN_008635c0; FUN_00784840; TriggerVoice 0x884e62]
- v = part misc +4 (0 = off). N = max(1, nearbyint((v·0.001f)²·25 ms·fs)); table T[i] = (cos((N − i)·float(π/(N + 1)))
  + 1)/2 in float, multiplied into the record's mono output from its first sample.
- With the fade on, TriggerVoice clears bank +0x4d: the first render sets the partial amplitudes straight to their
  targets instead of gliding up from 0.
- Morning −27.8 → −87.6 dB. Test: `harmor165h_attack_fade`.

## §3 Precision "Average": IPP's 11-bit functions
[DECOMP FUN_00878f80 (global misc +28 → plugin +0x1140) and the pointer table at 0x8790ec…0x8791b3]
- Plugin +0x1150 Exp, +0x1158 Ln, +0x1160 Tanh, +0x1168 Sqrt, +0x1170 Cbrt, +0x1178 Div point at ipps*_32f_A11 at
  precision 0 and at the A24 versions above. Every table also gets the precision (FUN_00874840): 0 → nearest entry
  (`cvtps2dq`, round-half-even, after `maxps 0` / `minps N − 1`), ≥ 1 → linear interpolation. (The table keeps
  precision 2 when the CPU-features word at 0x92c480 +0x12 lacks bit 0x100.)
- Black-box (our probe `ipp/ipp.exe` calling FL's own `dsp_ippv2_x64.dll`, unmodified, over every float bit pattern of
  the ranges Harmor uses), on the AVX2 path (this machine: Ryzen 5 3600):
  - Exp_32f_A11: n = rint(x·0x1.715476p+0), r = fma(−n, 0x1.62e430p−1, x),
    q = fma(fma(fma(0x1.52c758p−3, r, 0x1.01e5acp−1), r, 0x1.0007bcp+0), r, 1), result q·2ⁿ. Bit-exact on (−87.3, 88.7).
  - Ln_32f_A11: e = (bits − 0x3f2aaaab) >> 23, m = x·2⁻ᵉ in [2/3, 4/3), f = m − 1,
    q = Horner(0x1.c25266p−3, −0x1.11d1fcp−2, 0x1.54e2c0p−2, −0.5) in f, result fma(e, ln 2, fma(q·f, f, f)). Bit-exact
    on normal positive floats.
  - fma is a float fused multiply-add (LYNX: the product in double, one rounding).
- Sites using the Exp pointer, all switched to the model at precision 0: unison table (0x88c9b3 area), keyboard-glide
  gains, image frequencies (two sites), filter resonance, phaser 209, pluck rates, harmonic randomness (TriggerVoice
  0x884dfa: ippsRandUniform → × table → Exp). Ln: the per-partial log2 frequency (FUN_00890210).
- Not modelled (documented): Div_32f_A11 is a·rcpps(b) — the hardware reciprocal estimate, whose low bits differ
  between CPU vendors — used only by the image formant shift; Expm1_32f_A11 (image gain plane, FUN_00798130, up to
  ~280 ulp from the exact value); Tanh/Sqrt/Cbrt only in the rack distortion.
- IPP's SSE/AVX code paths (CPUs without AVX2) round differently: the same DLL forced onto them with
  `ippSetCpuFeatures` (`py/cpu.py`) differs from itself by −20…−30 dB on Subspace. FL's own output for these presets
  depends on the CPU, so LYNX follows the AVX2 path (every CPU from 2013 on).
- Fixed by this: Hoover, Hardstyle, Blue screen, Labyrinth, Subspace, Oldschool, Ice bells, Magic swamp, Metal
  thingy. Tests: `harmor165h_ipp_a11_models_match_the_dll`, `harmor165h_precision_average_changes_the_sound_slightly`.

## §4 What is left (Harmor stays beta)
Audible (STFT ≥ −40 dB) after s2, with what is known:
- **X Olbaid FX White Noise Caida / Plato Cerrado / FX Subida** (−32 / −38 / −37 dB). Very quiet presets (rms −57
  dBFS): the error is −94 dBFS absolute. All three match at precision 1 (−99…−103 dB); at precision 0 FL's own output
  differs from its precision-1 output by the same −37 dB, and LYNX's precision-0 output equals FL's precision 1. The
  DLL at precision 0 outputs exact zeros while the narrow filter-2 band (code 1 or 5, width ≥ 820) is swept towards
  the partials by its envelope (LF/stop-band partials −9…−18 dB lower than at precision 1); with the envelope amount
  at 0, or with the cutoff fixed, the precision-0 static reads match (−85…−89 dB). Verified equal: the table read rule
  (asm 0x874860), the tables (FUN_00874ed0 / zero cut FUN_00629eb0 / trim FUN_00874e40), the Ln of the frequencies,
  every Exp site, the CPU path (the DLL forced to SSE/AVX gives −99 dB against itself). Next: dump FL's per-partial
  filter gain buffer (voice +0x30 → +0x20) during the sweep from our harness to see which partials it zeroes.
- **Shield hit** (−24 dB; old v11 state, unison type 3, harmonizer, legato): single notes match; a second note while
  the first one's release still sounds diverges, only with the harmonizer on. FUN_00885cb0 (snapshot of the first
  passing iteration only) and FUN_008837a0 (pairwise add) are reproduced; the legato search FUN_00890990 skips
  released voices (+0x214 ≥ 6) like LYNX. Open.
- **Pomona** (−2 dB): several causes; the best single-parameter reset (part 1 misc +0x34 = 4) only reaches −8 dB.
- **Dub steppy 1** (−13 dB): diverges from the first sample; best single reset (global 0 = 256) −16 dB.
- **Sunroom** (−25 dB; old v12 state): best single reset (part 1 parameter 11 = 1000) −30 dB; no single cause.
- **Old vox** (−40 dB STFT, borderline since base0).

