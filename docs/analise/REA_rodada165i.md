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
- 16:15 PAUSED by the coordinator (machine load 33). State at the pause:
  - NOT yet compiled/tested: the checkpoint (task graph) and the new PluginHost change below. First thing on resume:
    build-local with FL Data (`tmp/perf165/bl.sh`, via buildslot, load gate < 14), then the Windows build.
  - **New fix (uncommitted before, now in this commit, unverified): plugin scan at start-up repeated every launch.**
    [EXEC] every LYNX start on this PC spawned `--scan-one VST3 …` children (learneq, AutoTune, Metamorph,
    smartchain…), tree RAM 55 → 162–904 MB, idle CPU polluted. [CODE] 22 VST3 bundles (Antares, sonible) are listed by
    the module inside the bundle (`…\Contents\x86_64-win\x.vst3`), so `isListingUpToDate(bundle folder)` never
    matched and the background scan described them again on every start; and `scanFiles` saved only at the end, so a
    LYNX closed mid-scan lost everything. Fix: `listedInside()` (bundle up to date when the modules listed inside it
    are), `PluginHost::sameModule()` for forgetFile, save every 5 s during a scan.
  - Benchmark harness: `bench.ps1` now also reports `idle_main_cpu_pct` (main process only, without scan children);
    `benche.ps1` + `tmp/perf165/eb.sh "<ENV=V;…>" <args>` run with environment variables (A/B `LYNX_TASKGRAPH=0`).
  - Baseline data so far (tmp/perf165/res-startup.jsonl, load 8–12): empty start LYNX base window 3.2–4.7 s,
    50–55 MB, idle 14–24 % (tree, incl. the scan child); FL 1.6–4.1 s, 310 MB, idle 22–32 %. Opens (res-open.jsonl):
    Thief LYNX 8.2 s / 577 MB vs FL 15.6 s / 818 MB, idle 11.6 % vs 60.5 %.
  - REA so far [DECOMP, objdump of FLEngine_x64.dll, /mnt/d/lynx-work/perf165/fleng.dis]: FL sets MXCSR 0x9f80 (FTZ),
    plus DAZ (0x40) when the CPU reports it (init at 0x63ab03–0x63ab3e, feature word bits 0x80 / 0x20000), and
    re-asserts x87 CW (`fnclex; fldcw; emms`, 0x63a660) + MXCSR (0x63a6d0) at 71 call sites — e.g. right before the
    global render lock (0xc4eb44) — i.e. it re-establishes the FPU state after foreign code. LYNX: FTZ+DAZ per callback
    and per worker, not re-asserted after a hosted plugin's process call → candidate (restore MXCSR after
    PluginHost.cpp:169 `processBlock` only if changed). FLEngine's DSP is mostly scalar SSE (mulss 5380, mulsd 4976,
    mulps 243, almost no AVX). Previous REA (ANALISE_FL_STUDIO §1.3/§2.2): FL runs generators in one task group, then
    the mixer layers with a barrier each; LYNX's task graph replaces the barriers with readiness counters.
  - Next after verification: Windows A/B (rtstress callback load, export time, null tests `LYNX_TASKGRAPH=0/1`),
    startup trace with the new marks, re-run the base/branch/FL series at low load.

## 1. Method

(to be filled)

## 2. Results — LYNX vs FL Studio 2026

(to be filled)

## 3. How FL does it (REA)

(to be filled)

## 4. Changes in LYNX, measured

(to be filled)
