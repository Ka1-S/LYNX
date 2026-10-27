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

- **PC:** AMD Ryzen 5 3600 (6 cores / 12 threads), Windows 11, the same machine as REA rounds 116 / 151. FL Studio
  2026 (`FL64.exe` build of 2026-10-08). LYNX **base** = main `5ed718d1` (0.1.19 + site), **branch** = this round.
- **Harness** `C:\Users\kauan\lynx-test\perf165\bench.ps1` (round 116's, extended): starts the program at
  AboveNormal priority, waits for its main window (≥ 900×500, answering `WM_NULL`, titled with the project), then
  for it to go quiet; idle CPU over 10 s for the **main process** (`idle_main_cpu_pct`, new) and for the whole tree
  (LYNX's plugin-scan children, FL's helpers), private working set, threads. Playback: `--rtstress` (LYNX: its own
  callback statistics) / a posted Space (FL), per-thread CPU over 20 s, the shared "FL Studio ASIO" driver with its
  block size set in the registry. `benche.ps1` = the same with environment variables (A/B switches).
  Driver scripts: `tmp/perf165/run.sh <startup|open|render|play>` (LYNX and FL interleaved, under the Windows GUI
  lock and FL's lock, every process closed by PID), `null.sh` (Linux null tests with `renderwav`).
- **Load:** other rounds were building during the whole session (load average 2–33 on 12 cores). Every run logs the
  load; timing series were started only below load 8 (the gate waits), CPU *time* is reported rather than wall time
  where possible, and A/B pairs run back to back on the same binary (`LYNX_TASKGRAPH=0/1`).
- **Null tests:** `renderwav` of the FL demo patterns sequential / layers / task graph → identical files (md5);
  `LYNX.exe --export` base vs branch on Windows.

## 2. Results — LYNX vs FL Studio 2026

(to be filled)

## 3. How FL does it (REA)

Sources: `objdump` of `FLEngine_x64.dll` (FL Studio 2026, 22 MB, Delphi; full listing kept on
`D:\lynx-work\perf165\fleng.dis`), its UTF-16/ASCII strings, and the earlier decompilations
(docs/ANALISE_FL_STUDIO.md §1–2, REA_rodada2 §2.4, REA_rodada139 B6). Clean-room: behaviour only.

| Topic | FL Studio 2026 | LYNX (before this round) | Evidence |
|---|---|---|---|
| Multi-core mixer | generators: one task group (shared atomic counter, "self-scheduling", `QuickThreads`/`TSTBTaskGroup`), summed into tracks under a per-track `CRITICAL_SECTION`; then the mixer **by layers, a barrier after each** (`FUN_00cf42f0`), tracks grouped into "units" | channel pass (parallelFor), then the layers (parallelFor per layer) — FL's model | [DECOMP] ANALISE §1.3/§2.2 |
| Smart disable | per plugin ("Allow smart disable"), −96 dB, 4 s hold, input **and** output tested; Options › CPU › Smart disable (global) | per slot since r139; silent channels skipped | [DECOMP] REA_rodada2 §2.4 |
| Denormals | MXCSR set at engine init to **0x9f80 (FTZ)** and `|= 0x40` **(DAZ)** when the CPU feature word says so (`0x63ab03`–`0x63ab3e`: bits 0x80 / 0x20000), else left alone; **re-asserted at 71 call sites** together with the x87 control word (`0x63a660`: `fnclex; fldcw; emms`, then `0x63a6d0`: `ldmxcsr`), e.g. right before taking the global render lock (`0xc4eb44`) and around plugin entry points | FTZ+DAZ for the audio callback and each worker (ScopedFlushDenormals); **not** re-asserted after a hosted plugin's process call | [DECOMP] |
| SIMD | FLEngine's own code is scalar SSE: `mulss` 5 380, `mulsd` 4 976, `addsd` 4 056 vs packed `mulps` 243 / `addps` 184, practically no AVX (`vmovups` 12). The heavy DSP lives in the plugin DLLs | core/Simd.h (SSE / NEON via sse2neon) in the hot loops | [DECOMP] instruction census |
| Sample loading | "Use threads to load samples while loading project" (`TThreadedSampleLoader`, `DataBrowser.SampleLoader`); "Keep on disk" / "Stream sample from hard disk when possible" (`TWAVStreamingThread`); ffmpeg decoder on a threaded queue (`TThreadedQueue<SmpLoader_Ffmpeg.TAudioBlock>`) | parallel decode at open (≤ 6 threads), samples > 30 s memory-mapped from a PCM disk cache, prefetch thread | [STR] |
| Plugin database | no scan at start: the plugin database is files (`Data\Patches\Plugin database`, `.nfo` / `.fst`), "Rescan plugins (fast)", a separate scanner; bridged plugins load through a queue (`TBridgeLoaderQueueItem`) | `plugins.xml` + optional background scan at start of new/changed files (child process per file) | [STR] |
| Waveforms | `il.WaveformCacheProcessor` with job keys (asynchronous, cached waveform rendering) | WaveformCache on a juce::ThreadPool + `%APPDATA%\LYNX\wavecache` | [STR] |
| Voices | typed voice pools `TDynamicVoiceArray<TMixVoiceGenPlug / AudioClip / Hybrid / 404>`; per-channel polyphony limit, "Disable maximum polyphony" | fixed voice arrays per instrument | [STR] |
| Tick splitting | the driver block is cut at every tick boundary (variable sub-blocks), "Align tick lengths" makes the ticks a whole number of samples | chunks cut only at events (sample-accurate), so far fewer, longer chunks | [DECOMP] ANALISE §1.1, [MANUAL] |

What LYNX takes from it this round: re-asserting the floating-point mode after foreign code (§4.2), FL's
"next step" for the mixer — readiness instead of layer barriers (§4.1, the improvement ANALISE §2 already proposed),
and FL's "no start-up rescans" behaviour for the plugin list (§4.3).

## 4. Changes in LYNX, measured

(to be filled)
