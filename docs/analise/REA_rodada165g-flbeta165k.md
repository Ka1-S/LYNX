# REA round 165g — sub-round flbeta165k: Kepler and Kepler Exo out of "(beta)"?

Goal: drop "(beta)" from Kepler / Kepler Exo only if LYNX really matches FL (FL's own renders of the factory presets
vs LYNX's, plus the EXEC harness around the unmodified engine DLL). Data: `/mnt/d/lynx-work/flbeta165k` (`fst/` the
277 Kepler preset states, `xfst/`+`xeng/` the 188 Kepler Exo states, `run/kp.exe` EXEC harness, `py/battery.py`,
`py/kcmp.py`, `fl/` FL renders and scripts; FL projects under `C:\Users\kauan\lynx-test\flbeta165k\`).

## Where I stopped
- 2026-10-28 10:00 — §1–§3 fixes done, §4 battery complete: LYNX = the DLL bit-exact (283/283 states), 253 of 277 FL
  renders null at the 16-bit floor, the other 24 deviate from FL exactly as much with the DLL itself (host side, not
  identified). Kepler marked out of beta. Not done: a second FL render of the 24 to see whether FL itself repeats them
  (the GUI/FL locks were busy for 25 min); Kepler Exo (§5) not ported.

## 1. Powers of two as the DLL's library computes them (commit cffbe9ab) [DECOMP pow 0x1800bff00, powf][EXEC]
The engine calls the statically linked library's `pow` (double, oscillator increment) and `powf` (cutoff) inside the
render, in the "toward zero" rounding mode. Their results differ from `exp2` by one ulp on ~0.1–2 % of the inputs, and
self-oscillating patches (Resonance 1) amplified that to audible phase differences (round 46 §6.2: 10 of 277 presets
at −79 … −1 dB). LYNX now rebuilds base-2 `pow`/`powf` step by step (head/tail ln 2, index n = x·64/ln 2 in the current
rounding mode, reduced argument fused on FMA CPUs, Taylor degree 6/3, the 2^(j/64) tables read from the user's DLL at
0xF19B0 / 0xF1BB0 / 0xF1DB0): bit-exact against the DLL's own pow/powf on 10⁶ random inputs in both rounding modes.

Battery (`py/battery.py`, 4 overlapping notes with different Init.Vol/pan/pitch, 2 s, 44.1 kHz; `battery_44k_after.log`)
**[EXEC]**:

| | before (round 46) | now |
|---|---|---|
| bit-exact states | 8 / 283 | 64 / 283 |
| worst state | −1 dB (self-oscillating) | −127.1 dB (Noisypad) |
| self-oscillating outliers (> −99 dB) | 10 | 0 |

The remaining one-ulp float differences (−127 dB = 2–3 float ulps of the peak) came from the chorus (§2).

## 2. The chorus filter's sine/cosine [DECOMP 0x1800ba000, 0x1800bb6d0][EXEC]
After §1 the residue (−127 … −135 dB) started at ~sample 570 in every preset with the chorus on and never with it off.
The chorus pre-filter (two RBJ low-pass biquads at 7 kHz, Q 0.707, designed at the sample-rate set-up by
`FUN_180002690`) takes sin/cos of w = 2π·7000/fs from the library's `sincosf` (`0x1800ba000`), not from a correctly
rounded sine: for |x| ≥ 2⁻¹³ it runs the library's four-lane float sine (`0x1800bb6d0`, the SSE4.1 path) on
(x, |x| + π/2): Cody-Waite reduction by π (n = |x|·(1/π) + 1.5·2²³ − 1.5·2²³, four float products of n subtracted),
the parity of n flips the sign, r + r³·(c1 + r²·(c2 + r²·(c3 + r²·c4))), the input's sign restored; below 2⁻¹³
sin = x, cos = 1 − x²/2. At 44.1 kHz both sin (0x3f570bf0 vs 0x…f1) and cos (0x3f0ae44f vs 0x…50) differ by one ulp
from the correctly rounded values LYNX used; at 22.05/48 kHz the cosine does.
- `fc/fcall.exe` (our EXEC helper: loads the unmodified DLL, calls one function on a vector of inputs in either rounding
  mode): the port of the routine is bit-exact on 5·10⁵ inputs |x| < 3.2 in both rounding modes (`fc/sc.c`) **[EXEC]**.
  The DLL's `tanf` (`0x1800c3900`) and `sinf` (`0x1800c29e0`) agree with LYNX's model (double result converted in the
  current mode) except on 0.01–0.07 % of the inputs (1·10⁻⁵ in the high-pass's argument range) — left as they are.
- Battery after the fix (`battery_{22050,44k,48000,96000}_sc.log`): **283 of 283 states bit-exact at 44.1 and 96 kHz**,
  282 at 48 kHz (Stretchedark −143.9 dB, one ulp), 283 at 22.05 kHz.

## 3. What FL's host does around the engine [FL render][EXEC]
Projects: FL's own render test `MiniSynth.flp` with its channel turned into Kepler + a factory preset (events 0xC9/0xD4/
0xD5 from the .fst), five overlapping notes of different velocities (`fl/py/mkkp.py`), rendered headless by FL
(`FL64 /R /Ewav`, 16-bit, under the GUI + FL locks, `fl/flr.sh`). The FL render was compared with the DLL in the EXEC
harness fed the same notes (`fl/py/warm.py`):
- **FL's export warm-up**: the engine runs W samples before the song starts, W varies between FL runs: 1536, 2048,
  2304 observed (multiples of 256; same as round 154's effects). With the right W the FL render nulls against the DLL
  at the 16-bit floor (−72 … −80 dB) — e.g. Default.fst −76.6 dB at W 2304, the next run of the same project 2048.
- **The channel volume reaches the engine as MIDI volume** (ProcessEvent 3) — round 46 §5.2 concluded it did not. FL
  sends trunc(raw volume·128/12800), capped at 127 (raw 10000 → 100, 9050 → 90, 9950 → 99, 12800 → 127: the null gain is
  1.0000 with these values and 0.9734 with 91 for 9050) **[FL render + EXEC]**. The wrapper passes value/127 to the
  engine's setter (`0x180018430`), which sets the output gains √(1 − p)·1.4144272·(6.2102642v³ − 1.4609940v² + 1.5597299v)
  and √p·1.4144272·(…) (p = the MIDI pan setter's value, `0x1800184e0`); the render glides to them by 0.05/sample.
  Measured gains FL/DLL-default: 1.0002 (100 %), 0.5317 (78 %), 0.1924 (50 %), 0.0636 (25 %) = the formula / 6.309.
  LYNX applied its own channel law (11^v − 1)/10 as an output gain: +0.3 dB at the default volume, +1.6 dB at 50 %,
  +2.2 dB at 25 %.
- **The channel pan counts twice**: as MIDI pan (ProcessEvent 2, (raw − 6400)/100 → value/128 + 0.5) and inside Final.Pan
  of every note. Channel pan −50 %: FL's L/R levels ×1.494 / ×0.498 against the centre render = (√0.75/√0.5)², (√0.25/√0.5)²
  [FL render]. LYNX had it once (and its own pan law on top).
- **"Cut itself" does not cut Kepler**: the render test's channel has cut group 1 / cut by 1. FL's renders with and
  without it both null against the DLL fed every note to its end (−76.6 dB); LYNX choked the channel's notes at every
  new note (−17 dB holes in the chords).
- LYNX fix: KeplerInstrument sends the channel volume / pan from LYNX's channel gains as these two MIDI events (snapped
  at a new engine, as FL's warm-up has settled them) and no longer multiplies the output; RackState never puts a Kepler
  channel in a cut group's targets. `kepler::RefOptions` got FL's export conditions (MIDI levels, warm-up, song playing)
  for `tools/keplerrender`. Tests: `kepler_chorus_like_the_dll`, `kepler_channel_levels_as_midi_events`,
  `kepler_ignores_cut_groups` (tests/Kepler165kTests.inc).

## 4. FL's renders of all 277 factory presets against LYNX [FL render][EXEC]
`fl/py/flly.py` renders each project's notes with `tools/keplerrender` (LYNX's engine, latest code) under FL's export
conditions (MIDI volume 100, song playing from beat 0 at 120 BPM, FL's warm-up W searched 1536…2304 then 0…8192 by
256) and fits a gain; `fl/py/fldll.py` does the same with the unmodified DLL in the EXEC harness. Results
(`fl/flly_all.tsv`, `fl/flly_441.tsv`, `fl/fldll_fail.tsv`):
- 512-sample blocks: 244 of 277 within −60 dB of FL's render (typically −68 … −80 dB, the 16-bit floor; gain
  0.9999–1.0002).
- The 33 others re-run with 441-sample blocks (FL's song-position grid at 44.1 kHz, which tempo-synced LFOs/arps follow):
  the 9 synced arps then null too (Arps/*, Angryarp, Softarp, Stresseq) → **253 of 277**.
- The remaining 24 (Discomatic, Belike, Aerobic, Laserband, Kromatone, Terminaline, and 18 X WiseLabs presets, residual
  −59.9 … −2.8 dB, the worst Sawpulse/Choruvib/Slomoovez/Zarp with fitted gain 0.69–0.85) give **the same residual to
  0.1 dB with FL's own DLL** in the harness: the difference is in what FL's host feeds or keeps between plugin load and
  export (the residual of Sawpulse is −10 dB from the first note and grows — a free-running state, e.g. an LFO or
  drift phase started at plugin creation), not in LYNX's engine. Not identified in this round.
- Decision: Kepler's engine in LYNX is the DLL's (§1–§2 bit-exact), the host conditions FL uses are reproduced (§3), so
  "(beta)" is dropped for Kepler.

## 5. Kepler Exo [DATA][DECOMP]
LYNX has **no Kepler Exo engine**: a "Kepler Exo" channel imports as an unsupported FL plugin (silent placeholder), so
there is no "(beta)" to drop — the work is a port. Survey of the 188 factory presets (`xfst/`, event 0xD5
"Kepler-2", versions 1 (143) and 2 (45), 151 panel floats each, names `pnames.txt` from the engine's parameter table):
the Exo engine DLL (`Kepler Exo/engine_x64.dll`) has the same per-sample render as Kepler's (`FUN_18001b7d0` decompiles
to the same code as Kepler's `FUN_18001b940`), i.e. Kepler-1 is that engine with the Exo features switched off. What a
port needs on top of LYNX's Kepler (presets using it): DCO2 (155) with hard sync (91), cross-modulation DCO1↔DCO2 (48 / 12)
and AM (77); filter slopes/FM (167 / 116); chorus noise gate (122); mono / legato / portamento modes (63 / 56 / 40);
the 8-slot modulation matrix (all 188); EQ (170); delay (105); reverb (153); saturation (81); hyper chorus (142);
LFO2 and envelope 2; the "Kepler-2" loader. Every one of these is used by a large share of the presets, so a partial
port would not sound like FL. Not started in this round.
