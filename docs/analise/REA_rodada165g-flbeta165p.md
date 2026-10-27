# REA round 165g — flbeta165p: Fruity Granulizer, Newtime, Newtone out of "(beta)"?

Sub-round of r165g flplugins165. Goal: render the same FL projects in FL and LYNX, fix the differences, and drop
"(beta)" only for an engine that really matches FL.

Evidence markers: [DECOMP addr], [DATA], [FL render], [EXEC FL] (the installation's DLL run unmodified in our harness),
[TEST], [SHOT], [RUN], [HYP].

## Where I stopped

- 2026-10-27 16:05 — resumed after the BSOD. State rebuilt from the checkpoint commit 1a512119 (`src/core/FlAutoSlicer.*`)
  and the scratch data `tmp/r165g` (harness `gx.c`/`gx.exe` = round 67's Granulizer harness + `slices`/`dispf`
  options, Python model `py/slmodel.py`, C++ probe `cpp/as.cpp`, batch logs `py/bat1_*.log` (Python model vs DLL, 40/40
  identical) and `py/bat2_*.log` (C++ `autoSlice` vs DLL, 60/60 identical at 44.1/48/22.05/96/32 kHz)).
- FL projects g0…g10 (slice modes, key modes, sync, ACID beats) rendered by FL 2026 headless under
  `C:\Users\kauan\lynx-test\flbeta165p\gr` (made by `tmp/r165g/py/mkgr.py`).
- Next: wire the auto slicer + region slices + transient hold + key mode 3 + sync into `core/Granulizer`, compare with
  the DLL harness and the FL renders.

## 1. Auto slicing (framework slicer used by the Granulizer's slice mode 2)

See `src/core/FlAutoSlicer.h` for the outline. [DECOMP 0x7400a0, 0x740e80, 0x740210, 0x740630, 0x6057c0, 0x60b200]
[EXEC FL: `py/bat2.py` — slice starts of the DLL (`#sl` dump of the slicer object at plugin+0x150) identical to
`lynx::flslice::autoSlice` on 60 random multi-hit samples, mono/stereo, five sample rates].
