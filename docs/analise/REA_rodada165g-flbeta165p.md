# REA round 165g — flbeta165p: Fruity Granulizer, Newtime, Newtone out of "(beta)"?

Sub-round of r165g flplugins165. Goal: render the same FL projects in FL and LYNX, fix the differences, and drop
"(beta)" only for an engine that really matches FL.

Evidence markers: [DECOMP addr], [DATA], [FL render], [EXEC FL] (the installation's DLL run unmodified in our harness),
[TEST], [SHOT], [RUN], [HYP].

## Where I stopped

- 2026-10-27 23:40 — landing the verified part; the round ends here (parent: keep scope small, no new long REA).
- Landed: `core/FlAutoSlicer` (FL's auto slicing, [TEST] `fl_auto_slicer_matches_fl` = the DLL's slice starts
  [EXEC FL, tmp/r165g/py/gold.py]), `core/AudioEdit` `decodeWavExtras` ([TEST] `audioedit_wav_extras_markers_and_acid`),
  `SampleData::markerStarts` / `acidBeats` filled by `engine/SampleLibrary` for in-memory WAVs. Core tests: 0 failures.
- NOT done: the engine side below (steps 1–6) — Fruity Granulizer, Newtime and Newtone keep "(beta)". Nothing in the
  engine reads the slicer / markers / beats yet.
- Verified this session [EXEC FL vs FL render], scripts in the scratchpad (copies: tmp/r165g/py, cmpg.py, sl.py, blk.py):
  - g0/g1/g4: DLL with no beats == FL render (−78.6 dB = 16-bit quantisation). g4 (state v5, no sync byte) plays
    WITHOUT sync although FL sends beats → states without the sync byte: sync off (FL sends Dispatcher 0x13 with 0
    before the state, switching mode 2 → 0).
  - g2/g3 (channel TIME stretch 4 / 8 beats) and g5 (ACID chunk 4 beats, no stretch): FL sends Dispatcher 0x13 =
    those beats; DLL with beats=4/8 == FL to quantisation up to the note end (blockwise −60…−80 dB; the −25 dB of the
    whole-file figure is only the harness holding the note past FL's note-off).
  - g6 (auto slice), g7 (cue points), g8 (regions): DLL with slices from autoSlice / the cue or region starts == FL
    (same pattern). Region end does not matter (the slice start = min(start, end, len−1), sorted).
- Next steps (decoded, to implement in `core/Granulizer.cpp`):
  1. Engine fields: slices (starts), th = samplesMs(msLaw(p[11])) (P+0x174), invAH = 1/AH (0x17c),
     k18c = AH / max(wsp, 1) (0x18c), beats (0x19c), beatLen = rnd((beats·60f·(float)fs)/tempoF) (0x198);
     sync on iff state sync byte == 1 and beats > 0 → wsp = (gsp·(float)(le+1))/(float)beatLen, loop on.
  2. Voice.slice = −1 at trigger. Voice_Render (0x753ea0), non-random branch with slices and wsp > 0:
     t = Final.Pitch+pofs; t==0 ? pos+A : (float)(pos + A·exp(t·ln2/1200)); i7 = rnd(t − 0.5); first slice s_i ≥ i7:
     d = s_i − t; same slice as before → n = max(1, rnd(d/wsp)), d = ((d − n·wsp)·k18c)/n, hold = max(0, H + d);
     new slice → v.slice = i, d·= k18c, hold = th, if d < A+th: hold = max(0, d − A). f14 = (A+hold)·invAH
     (float, not exactly 1). Grain with rnd(hold); next += rnd(gsp·f14); pos += wsp·f14. Random branch: f14 = 1.
  3. TriggerVoice: key mode 2 with beats > 0 (sync on or not): d = le·(k/4)/beats; key mode 3: slices[k % n] (0 if none).
  4. Instrument: slices computed in configure() (off the audio thread) — mode 1: sample markerStarts; mode 2:
     flslice::autoSlice(sample); beats = channel.sampler.stretchBeats (TIME/192 [·MUL? HYP]) else sample acidBeats.
  5. C++ vs DLL batch (r67/py/cppcmp.py + slices/sync/rates), then LYNX vs FL renders g0…g10; new FL renders still
     wanted: stretch+MUL, ACID+TIME both set, 48 kHz sample (auto slice; sync — is 0x198 at host fs?).
  6. tests (tests/*Tests.inc), drop "(beta)" if all match; then Newtime, Newtone.

## 1. Auto slicing (framework slicer used by the Granulizer's slice mode 2)

See `src/core/FlAutoSlicer.h` for the outline. [DECOMP 0x7400a0, 0x740e80, 0x740210, 0x740630, 0x6057c0, 0x60b200]
[EXEC FL: `py/bat2.py` — slice starts of the DLL (`#sl` dump of the slicer object at plugin+0x150) identical to
`lynx::flslice::autoSlice` on 60 random multi-hit samples, mono/stereo, five sample rates].
