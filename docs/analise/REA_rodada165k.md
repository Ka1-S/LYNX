# REA round 165k (flexact165) — every FL native plugin to Exact

Date: 2026-10-27. Clean-room: FL Studio's own plug-in DLLs are studied with Ghidra 12.1.4 (headless) and run
unmodified in our own harness (round 154's `fx.c`); this document describes their behaviour in our own words. No
Image-Line code, preset or sample is in the repository.

Markers as in round 154: **[DECOMP]** Ghidra, **[EXEC]** the DLL in our harness, **[DATA]** project / preset files,
**[FL]** FL's own render, **[HYP]** not verified.

## Scope

Effects still "Approximate" in docs/PLUGINS.md: Fruity 7 Band EQ, Fruity Bass Boost, Fruity Delay (v1), Fruity Phaser
(already exact since round 105 — the PLUGINS.md row was stale), Fruity Center, Fruity Phase Inverter, Transporter,
Emphasis. Then the "Close" generators (Sytrus, Harmless, Ogun, GMS, Morphine, Sakura / Drumpad).

## Where I stopped

- 16:40 start: worktree `lynx-wt-flexact165`, tools in `tmp/flexact165/` (bin = DLL copies, py, cpp, gh), data in
  `/mnt/d/lynx-work/flexact165`. States of the seven plug-ins extracted (`states/<tag>/`, `states/index.txt`).
- 17:00 engines written and bit-exact against the DLLs (`py/lgbatch.py <kind> seed n`, `cpp/lgcmp.cpp`): Phase
  Inverter, Center, Bass Boost, 7 Band EQ (`src/core/FlLegacyFx.{h,cpp}`). NOT yet wired into LYNX (EffectKinds,
  import, mixer list, tests). Next: wire them, then Fruity Delay (v1), Transporter, Emphasis.
