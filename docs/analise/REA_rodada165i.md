# REA round 165i — FL Studio's performance technology, and LYNX light and fast (perf165)

Brief (2026-10-27): benchmark LYNX against FL Studio 2026 on the same PC and projects, study with REA/Ghidra how FL
gets its performance (multi-core mixer scheduling, smart disable, voices, denormals, SIMD, sample streaming, UI
redraw, lazy loading), and land the real wins — with null tests (no change in the sound).

Evidence markers: **[EXEC]** measured here, **[DECOMP]** Ghidra pseudocode of FL's binaries, **[CODE]** our source,
**[model]** our reading. Raw data stays in `tmp/perf165` and `C:\Users\kauan\lynx-test\perf165` (never in the repo).

## Where I stopped

- 2026-10-27 23:10 (resumed after the BSOD): landed so far — the test profile / test mode / `--opentime` (main
  `2f520ca6`). On feature/perf165, still to land: the task graph, the plugin rescan fix, KeepFloatMode, the stretch disk
  cache, and (new, `bae869a6`) the keep-pitch shifters made in parallel (`prepareKeyShifts`, `rack_keyshift` in
  load.log, `LYNX_PARALLEL_KEYSHIFT=0` for A/B; test `perf165_parallel_key_shifters_render_the_same`). Running:
  `tmp/perf165/ot.sh main br` (C:\…\perf165\{main,br}; fresh profile from `seed/`, LYNX_TEST_MODE=1, `--opentime`,
  cold and warm open, load gate < 8) → `res-ot.jsonl`; build-local of bae869a6. Next: FL runs (`run.sh open`, FL lock),
  fill §2, land everything else via land.sh.
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

Same PC (12 cores, Windows 11), FL Studio 2026 and LYNX side by side, load average < 8 for every timed run.
LYNX: `--opentime` under a fresh test profile (`LYNX_PROFILE_DIR`, seeded with the plugin list and audio device,
`LYNX_TEST_MODE=1`), clean exit; "cold" = first open in that profile, "warm" = the same profile again (the disk
caches filled). FL: `bench.ps1` (window title with the project name, then quiet), closed by PID. Medians.
"main" = `2f520ca6` (this round's test profile only), "branch" = feature/perf165. Memory: LYNX reports the whole
working set, FL the private working set (so the comparison is, if anything, unfair to LYNX).

### 2.1 Project open (seconds until the project is loaded; working set)

| Project | FL 2026 | LYNX main cold / warm | LYNX branch cold / warm | RAM FL / main / branch (warm) |
|---|---|---|---|---|
| Empty start (window) | 1.6–1.7 | 2.5 / 2.8 | 2.5 / 2.5 | 310 / 99 / 99 MB |
| Thief | 7.2–7.4 | 7.2 / 6.2 | 5.7 / **3.8** | 819 / 837 / **629** MB |
| SHINE | 7.0–10.5 | 3.7 / 3.2 | 3.4 / 3.1 | 810 / 198 / 197 MB |
| Never Out | 9.6–9.8 | 4.6 / 4.4 | 4.4 / **3.6** | 930 / 461 / 462 MB |
| Exoplanet | 7.6–8.5 | 3.5 / 3.4 | 3.5 / 3.4 | 905 / 334 / 333 MB |

CPU time spent until loaded (Thief): main 10.2 s cold / 9.2 s warm → branch 9.2 / **4.0** s. load.log (Thief, warm):
total 3 727 → 1 237 ms; rack_build 1 532 → 453 ms (rack_instruments 768 → 208 ms, rack_keyshift 139 ms in parallel,
rack_stretch 654 → 0 ms); stretch_wall 1 624 → 168 ms (the copies read from the disk cache in 505 ms of thread time).

Idle CPU after the open varies run to run with the third-party plugins in the project (LYNX 5–60 %, FL 29–111 %).

### 2.2 Rendering (null tests)

- Linux `renderwav`, sequential / layers / task graph: byte-identical (Thief `67a4184bf5ec…`, SHINE `e6be7b9a893b…`,
  also after the seed change). CPU at load ~2: Thief layers 4.90 → graph 4.20 s, SHINE 12.31 → 11.05 s (wall
  6.36 → 5.55 s).
- Windows `--export` (test profile): see §2.3.


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

### 4.1 Task-graph mixer scheduling (`src/core/RackRenderer.{h,cpp}`)

Channels and mixer inserts form one dependency graph (`RackState::buildTaskGraph`) run on the `WorkerPool` with
readiness counters (`RackRenderer::runTaskGraph`): an insert starts as soon as its own channels and sources are done,
with no barrier per layer. Each insert sums its channels in channel order, then live input, then runs its effects, the
same order as the layers, so the result is bit-exact. Fallback to the layers when something is pinned to the audio
thread, the routing has a cycle, or an instrument has aux outputs. `LYNX_TASKGRAPH=0` switches it off.

- [EXEC] null test (Linux `renderwav`): sequential, layers and graph renders are byte-identical (SHINE md5
  `e6be7b9a893b…`, Thief `67a4184bf5ec…`).
- [EXEC] CPU (Linux, machine under load): Thief layers 5.45 s → graph 4.31 s CPU; block load mean 10 → 7 %,
  p50 9 → 5 %, p99 32 → 26 %. SHINE mean 15 → 13 %, p50 12 → 10 %.
- Test `rea165i_task_graph_matches_layers_with_sidechain_and_cycle`.

### 4.2 Floating-point mode kept after plugin code (`src/core/Denormals.h`, `PluginHost.cpp`)

`KeepFloatMode` restores MXCSR (and the x87 control word on x86-64 GCC/Clang) after each hosted plugin block call,
only when the plugin changed it — FL's re-assert pattern (§3). A plugin that clears FTZ/DAZ can no longer make the
rest of the mix fall into denormals. Test `rea165i_keep_float_mode_restores_ftz_after_foreign_code`.

### 4.3 No plugin rescans at every start (`src/engine/PluginHost.{h,cpp}`)

`listedInside()` treats a VST3 bundle as up to date when the modules listed inside it are current (22 bundles on this
PC are listed by their inner module path and were rescanned on every launch, spawning `--scan-one` children of up to
900 MB). `PluginHost::sameModule()` for `forgetFile`; the scan list is saved every 5 s during a scan, so a LYNX
closed mid-scan keeps what it already found.

### 4.4 Time-stretch disk cache (`src/engine/SampleLibrary.cpp`)

Stretched samples are kept as `.str` files in the cache folder, keyed by path | size | mtime | ratio | method |
`stretchFingerprint` (a test signal stretched once per method, so a changed algorithm never reuses old files); LRU
prune to 3/4 when over 1 GB. The in-memory stretch cache now evicts only lengths nothing uses, and the project
preload keeps the stretched results. [EXEC] Thief's `load.log` before: stretch_wall 1 981 ms (×23) and
rack_stretch 754 ms (×29, misses from the old eviction) on every open.

### 4.5 Test profile, test mode and `--opentime` (`src/core/TestProfile.h`, `src/platform/Paths.cpp`, `Main.cpp`)

`LYNX_PROFILE_DIR` moves settings, caches and Documents/LYNX into a throw-away folder; `LYNX_TEST_MODE=1` skips (and
logs to `test-mode.log`) the unclean-session notice, crash-report retry, setup wizard, Missing files, project info and
the start-up plugin scan. `--opentime <project|-> <report> [idleS]` opens through the normal start-up, waits for the
title, settles, measures idle CPU and RAM, writes the report and quits cleanly. Benchmarks no longer touch the real
`%APPDATA%\LYNX` or leave dialogs on the desktop. Test `rea165i_test_profile_and_test_mode_parsing`.

### 4.7 Faster rebuilds of keep-pitch samplers (`RackRenderer.cpp`), fixed stretch seed (`TimeStretch.cpp`)

- The 8 pitch shifters of every keep-pitch sampler are made after the channel loop, all channels at once on up to 6
  threads that take the building thread's float mode (`FloatMode` in Denormals.h). Thief: rack_instruments 768 →
  208 ms + rack_keyshift 139 ms. `LYNX_PARALLEL_KEYSHIFT=0` keeps the old order; test
  `perf165_parallel_key_shifters_render_the_same`.
- Found by that test: the offline stretch seeded Signalsmith from `std::random_device`, and beyond 2× it randomises
  each bin's time step, so the same sample stretched twice (two opens, two exports) came out different. Now a fixed
  seed; test `perf165_long_stretches_repeat_exactly`. The stretch fingerprint now stretches 2.6× so the seed is part
  of it.
- The disk copy of a stretched sample is written by one background thread after the open, not during it (it cost
  14.7 s of thread time for Thief's 229 MB on Windows, cold open 7 s slower than without the cache). load.log has
  stretch_disk_read and stretch_compute; `LYNX_STRETCH_DISK=0` turns the disk copy off.

### 4.6 Tracing

`lynx::startupMark` steps inside the audio engine and the splash for `LYNX_TRACE_STARTUP`; `--uiperf` CPU from
`processCpuSeconds()`.

