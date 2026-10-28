# REA round 165g — flgran165: Fruity Granulizer's sound engine (slices, transient hold, sync, key modes 2/3)

Sub-round of r165g flplugins165, continuing docs/analise/REA_rodada165g-flbeta165p.md (FL's auto slicer, WAV
markers / ACID beats) and docs/analise/REA_rodada67.md (the engine). Goal: finish the engine and drop "(beta)" only if
LYNX matches FL.

Evidence markers: [DECOMP addr], [DATA], [FL render], [EXEC FL] (the installation's DLL, MD5 083484d2…, run unmodified
in our harness tmp/r165g/gx.exe), [TEST], [SHOT], [RUN], [HYP]. Scripts: tmp/flgran165/py (cmp165.py C++ vs DLL,
flq.py DLL vs FL renders, lynxfl.py LYNX's import + render vs FL renders, mk2.py the new FL projects); Ghidra project
~/ghproj/r67_gr (decompiles in /mnt/d/lynx-work/flgran165/dec).

## Where I stopped

- 2026-10-28 05:10 — started. Decompiled Voice_Render 0x753ea0, update 0x752290, sync 0x7524e0/0x752490/0x752520,
  Dispatcher 0x13 (0x7517f8), TriggerVoice 0x753580, slice set-up 0x7548c0, sample load 0x754b30.
- 05:40 — engine done in `core/Granulizer.cpp` (slices + transient hold, tempo sync, key modes 2/3, FL's render
  quality); C++ vs DLL 127 random cases, 0 bad. FL renders g11…g16 settle the three open questions (§3).
- 06:05 — LYNX's whole chain (FL import → rack → render, `renderwav`) matches all 17 FL renders at the 16-bit floor
  (§5) → "(beta)" dropped. Editor: Key to / Transients / Tempo locked / Beats (pt-BR). Tests `granulizer165_*`.
  Commit ea68d1e2 on wip/flgran165. Next: core tests green, Linux GUI shot of the editor, land.

## 1. The grain clock with slices [DECOMP 0x753ea0, 0x752290]

Per grain, in the non-random branch (randomness off, or its draw failed, or freeze on), when the slicer object holds
slices and the wave spacing W > 0:

- `t` = where the voice will be after the attack at the grain's speed: `pos + A` when `Final.Pitch + keyOffset = 0`,
  else `float(pos + A·exp(cents·ln2/1200))` (ln2/12/100 is a constant built at load, 0x60f93d).
- The first slice start `s_i ≥ round(t − 0.5)` (32-bit compare). `d = float(s_i) − t`.
  - Same slice as the voice's previous grain: `n = max(1, round(d/W))`, `d = ((d − n·W)·k)/n`, hold = `max(0, H + d)`.
  - A new slice: the voice remembers it, `d ·= k`, hold = TH (the transient hold, `ms(P11)` in samples) — or, when
    `d < A + TH`, `max(0, d − A)`.
  - No slice ahead: hold = H.
  - `k = (A + H) / max(W, 1)` (P+0x18c), so distances in the sample become grain time.
- `f = (A + hold)·(1/(A + H))`: the next grain starts `round(G·f)` later and the position moves `W·f` (wobble as before).
- The grain itself gets `round(hold)` as its hold.

## 2. Tempo sync and the key modes [DECOMP 0x7517f8, 0x752520, 0x7524e0, 0x753580, 0x758050]

- Dispatcher 0x13 stores the host's beat count (float, P+0x19c); mode 2 ("auto", the constructor's value) becomes
  `beats > 0`. The beat length `round(beats·60·hostRate/tempo)` (all float, P+0x198) is rebuilt at 0x13 and at every
  tempo event (ProcessEvent 0).
- Sync is on when the state's byte is 1 and beats > 0: W = `G·(end + 1)/beatLength` (one pass over the sample per beat
  count, independent of the sample's rate) and loop forced on.
- TriggerVoice, key mode 2: with beats > 0 the step is `end·(k/4)/beats` (a 16th of the sample's beat length), else
  `(k/4)·60·sampleRate/tempo`; key mode 3: the start of slice `k mod n` (0 without slices).
- The editor's key-mode handler (0x758050, run on every state load) switches the slice mode from 0 to 2 when the key
  mode is 3 [EXEC FL: such a state plays the auto slices]; LYNX applies this when it reads a state.

## 3. What FL sends [FL render + EXEC FL]

New FL projects (tmp/flgran165/py/mk2.py, renders in C:\Users\kauan\lynx-test\flbeta165p\gr), compared with the DLL
fed each candidate (flq.py; residual up to the note-off, −76…−78 dB = 16-bit quantisation):

| render | setup | result |
|---|---|---|
| g11_mul | TIME 4 beats, MUL ×2 | beats = 4 (−76.6 dB; 8 / 2: +2.9 dB) → **TIME/192 without MUL** |
| g16_mulneg | TIME 4 beats, MUL −5000, auto slices | beats = 4 (−76.8 dB) |
| g12_acidtime | ACID 4 beats + TIME 8 beats | beats = 4 (−76.6 dB; 8: +2.9 dB) → **the ACID beats win** |
| g14_48sync | 48 kHz sample, TIME 4 beats | SmpRateConv = 48000/44100, beat length at the host rate; only with **16 interpolation points per side**: −76.6 dB (the plugin's default 12: −40.6 dB) |
| g13_48slice2 | 48 kHz sample, auto slices | slices on the sample at its own rate, quality 16: every block at the quantisation floor (−55 dB only in the quiet blocks) |
| g15_48cuekey3 | 48 kHz, cue points, key mode 3, sync 6 beats, 130 BPM | −77.3 dB |

So FL renders with ProcessMode quality 16 ("32-point sinc", FL's render default — as LYNX's sampler keeps
`flrs::renderQuality`); LYNX uses 16 while exporting and 12 (FL's 24-point playback default) live [HYP for live:
FL's playback value is not measured; it mirrors the sampler's].

## 4. C++ vs DLL [EXEC FL]

`cmp165.py` (random parameters, slice modes 0–2 with regions or auto slices, key modes 0–3, sync on/off with 0…8
beats, tempos 90…174, mono/stereo, sample rate ≠ host rate, 44.1/48/96 kHz, blocks 64…1000, 1–2 notes and
releases): seeds 1–5, 102 cases — before the fix of §2's key-mode rule one bad case; after it **0 bad**, typically
−130…−140 dB, worst −105.9 dB (positions on the reader's edge path, as round 67's isolated near-integer samples).

## 5. LYNX vs FL, end to end [FL render + RUN]

`lynxfl.py`: `build-local/renderwav` imports each FL project (the importer, the rack, the instrument, the voice
processor and the mixer, as LYNX exports: 16-point reader) and the raw float output is compared with FL's 16-bit render
up to the pattern end (renderwav then restarts the pattern; FL's render stops ~600 samples after the note-off):

| render | what | residual (gain 1.0000, lag 0) |
|---|---|---|
| g0_plain / g1_sync0 / g4_v5_4b | plain; sync byte but no beats; v5 state with TIME 4 (sync off) | −78.4 dB |
| g2_sync4b / g3_sync8b / g5_acid | sync with TIME 4 (140 BPM), TIME 8 (100 BPM), ACID 4 | −76.6 / −76.2 / −76.6 dB |
| g6_slice2 / g7_slice1cue / g8_slice1reg | auto slices, cue points, regions + transient hold | −76.3 / −76.7 / −76.8 dB |
| g9_key3 / g10_key2sync | Transient keys (auto slices), Step keys with sync; three notes, releases included | −78.1 / −76.5 dB |
| g11_mul / g12_acidtime / g16_mulneg | TIME + MUL, ACID + TIME, auto slices + sync + MUL | −76.6 / −76.6 / −76.8 dB |
| g13_48slice2 / g14_48sync / g15_48cuekey3 | 48 kHz sample: auto slices, sync, cue points + Transient keys + sync at 130 BPM | −75.9 / −76.6 / −77.6 dB |

The 16-bit quantisation floor of these renders is about −76…−78 dB; the worst 8192-sample blocks (−55 dB) are the
quiet ones, where the quantisation noise dominates. Live playback (12 points) differs from FL's render only for samples
off the host rate (g13–g15), by the interpolation's own error (about −40 dB on this material).

## 6. LYNX side

- `core/Granulizer`: `Engine::sliceHold` (§1), `syncOn` / `update` (§2), key modes 2/3 in `trigger`, reader quality
  `points` (16 while `offlineRendering()`, else 12). The instrument builds the slice list once per slice mode in
  `configure` (UI thread: the WAV's `markerStarts`, or `flslice::autoSlice` — about 1.4 s for a 3-minute stereo sample,
  30 ms for a 4-second loop; FL also slices synchronously behind a "Slicing..." hint) and the audio thread only reads it.
  Beat count = `SampleData::acidBeats`, else `ChannelDef::sampler.stretchBeats` (the importer keeps TIME/192 for this
  engine, without MUL). A state with Transient keys and no slice mode reads as auto slicing.
- Editor (Channel settings › plugin page): *Key to* (Pitch / Percent / Step / Transient), *Transients* (Off / Use
  regions / Detect), *Tempo locked*, *Beats* (the channel's TIME; 0 = the sample's own ACID beats).
- `tools/granulizerrender` takes `tempo=`, `beats=`, `slices=auto|file`, `points=`; `tools/WavReader.h` reads cue
  points / regions / ACID beats like the app.
- Tests: `granulizer165_modes`, `granulizer165_goldens` (goldens from LYNX checked against the DLL: −134 / −94 / −136 dB),
  `granulizer165_instrument` (ACID beats over TIME, regions = detect with the same positions, render vs live quality).
