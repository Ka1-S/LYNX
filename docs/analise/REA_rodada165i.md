# REA round 165i — FL Studio's performance technology, and LYNX light and fast (perf165)

Brief (2026-10-27): benchmark LYNX against FL Studio 2026 on the same PC and projects, study with REA/Ghidra how FL
gets its performance (multi-core mixer scheduling, smart disable, voices, denormals, SIMD, sample streaming, UI
redraw, lazy loading), and land the real wins — with null tests (no change in the sound).

Evidence markers: **[EXEC]** measured here, **[DECOMP]** Ghidra pseudocode of FL's binaries, **[CODE]** our source,
**[model]** our reading. Raw data stays in `tmp/perf165` and `C:\Users\kauan\lynx-test\perf165` (never in the repo).

## Where I stopped

- 2026-10-27 15:26: the PC blue-screened mid-round (driver bugcheck, not LYNX). The work on disk was saved as the
  checkpoint commit `0bb53ce5` (unverified). Rebuilt from the diff:
  - **Task-graph mixer scheduling** (`RackState::buildTaskGraph`, `RackRenderer::runTaskGraph`): channels and inserts
    as one dependency graph run through the existing `WorkerPool`, an insert starts as soon as its own channels and
    sources are done (instead of the channel pass → layer barriers). Bit-exact by construction (each insert sums its
    channels in channel order, then live input, then its effects — the layered order). Falls back to the layers when a
    channel/insert is pinned to the audio thread, a routing cycle exists, or an instrument has aux outputs.
    `LYNX_TASKGRAPH=0` switches it off for A/B runs. Test `rea165i_task_graph_matches_layers_with_sidechain_and_cycle`.
  - Start-up trace: the audio engine's own steps in `LYNX_TRACE_STARTUP` (`lynx::startupMark`), splash steps.
  - UI perf report (`--uiperf`): process CPU via `processCpuSeconds()` instead of `std::clock()` (wall time on Windows).
  - Benchmark harness `tmp/perf165/run.sh` + `bench.ps1`: baseline (main 5ed718d1) start-up (3 runs, LYNX only) and
    project opens (LYNX only, partial) were measured at load 24–29 — to be re-run at low load with FL.
- Now (resume): verify the checkpoint (build-local + Windows), re-run the benchmark base vs branch vs FL.

## 1. Method

(to be filled)

## 2. Results — LYNX vs FL Studio 2026

(to be filled)

## 3. How FL does it (REA)

(to be filled)

## 4. Changes in LYNX, measured

(to be filled)
