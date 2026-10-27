# Round 165c sub-round dash165 — Dashboard polish, Dashboard MIDI, X-Y-Z curves, Voltage Controller links

Evidence markers: **[DECOMP addr]** headless Ghidra on FL Studio 2026's own DLLs, **[DATA]** FL's own files,
**[FL render]** FL's command-line render of a test project, **[EXEC FL]** FL running, **[TEST]** LYNX unit tests,
**[SHOT]** screenshot looked at, **[RUN]** LYNX run, **[HYP]** not checked against FL. Clean-room: no FL code, art or
presets are copied. Work data: `/mnt/d/lynx-work/dash165` (Ghidra copies `ghidra/dash`, `ghidra-xyz`, decompiles
`d_*.c`, `x*.c`).

## Where I stopped

- 2026-10-27 15:26 — the PC blue-screened; the agent died. Checkpoint commit 8a6f6b93 (22 files, unverified) holds:
  `core/FlCurve.{h,cpp}` (FL's point law, 13 modes), X-Y-Z curve modes (params 14…16) + import of the output curve's
  mode / tension, Dashboard limit 32 → 1024 outputs (Control Surface stays at maxEffectParams), Dashboard MIDI
  (per-control CC / RPN / NRPN / pressure / note, port / channel / program / bank, sent by the rack to the plugins
  on the port), Voltage Controller offsets / scales as automation / link targets, label fitting in SurfacePanel.
- 2026-10-27 16:00 — resumed (new agent). Next: build + fix, unit tests, shots, FL render checks, land.
