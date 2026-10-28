# Round 165d — VFX Script runs Python (pocketpy + LYNX's own `flvfx`)

## Where I stopped

- 2026-10-27 15:26: the PC blue-screened and killed the first agent of this round. Its work was saved as the
  checkpoint commit 7af588e3 (unverified): pocketpy in `third_party/pocketpy`, `src/core/Python.{h,cpp}` (the
  embedded interpreter: one Python thread, per-instance VMs, sandbox, time limit), `src/core/VfxScript.{h,cpp}`
  (the `flvfx` module, the processor, describe/values), `tests/VfxScript165Tests.inc`, build-local.sh. Not yet
  done at that point: the editor UI, CMake (Windows / JUCE build), THIRD_PARTY.md, FL renders for comparison
  (the FL comparison projects were generated in D:\lynx-work\vfxscript165\cases but never rendered).
- 16:00 resume: the checkpoint builds; 8 vfx_script tests: 50 checks, 3 failures (ticks timing test; `re.match`
  fails because pocketpy treats `match` as a keyword after "." → patch the pocketpy parser; full run with FL Data
  not done yet).
- Done since the resume (uncommitted until the 16:3x WIP commit): CMake target `pocketpy` + Python.cpp/VfxScript.cpp in
  lynx_core; THIRD_PARTY.md row; Python thread wake-up = OS semaphore (std::counting_semaphore polls on Windows),
  Python thread HIGHEST priority on Windows; `vfx.context.voices` is a snapshot (FL's tutorials release voices while
  walking the list); new VFX Script editor in src/ui/VfxEditors.cpp (FL's tabs User interface / Script / Log / API
  reference, controls from createDialog, Python syntax colours, Apply Ctrl+Enter, Revert, Restart, error line jump,
  output-controller bars, live values; unique state tag per editor) — NOT compiled yet (needs the JUCE build).
- WARNING: one test run was OOM-killed (43 GB RSS) — cause unknown (all 8 tests pass alone and together under
  `ulimit -v 4000000`). dev.sh now runs the tests under ulimit -v + timeout. Find the cause before any unlimited run.
- FL renders of the comparison cases (D:\lynx-work\vfxscript165\cases\*.flp, tmp py/cases.py) not done yet:
  flrender.sh (now kills only PIDs it started) waited for the FL lock and was stopped at the coordinator's PAUSE.
  Compare tool: tmp/vfxscript165/py/notes.py (note segments of a render).
- PAUSED 16:3x by the coordinator (load 33). Next: read the LOAD LIMIT section, fix the 3 failures + `match`, JUCE
  build of the editor, FL renders vs LYNX, screenshots, notes, land.
- 17:xx (after a context compaction): fixed since the pause — the 43 GB run (a bare `except:` caught the
  TimeoutError forever; interrupts now skip except handlers), `match` soft keyword, counted pocketpy heap (scripts
  stopped past 768 MB), CPython's missing str methods, `globals().get(k, default)`, realtime flag from the engine.
  Test build with FL Data: 334 checks, 0 failures; 89/89 FL factory scripts run. FL renders of the 12 cases done
  (D:\lynx-work\vfxscript165\fl). Next: JUCE Linux build of the editor (queued in buildslot), LYNX renders of the
  cases (tmp/vfxscript165/lxrender.sh) + compare.py, --vfxshot screenshots, build-local + Windows, notes, land.
- 21:xx: Linux JUCE build of the editor OK (one compile fix); LYNX renders of the 12 cases compared with FL (§3):
  two fixes (stopped onTick before the first block; a cut-group cut keeps a Patcher's VFX voices), combo boxes of
  the editor had a zero width (seen in the --vfxshot screenshots, fixed). Merged origin/main. Running: build-local
  with FL Data + Windows build, then land.

## 1. What FL's VFX Script is

- A note processor (Patcher node / Channel Rack channel, 16 voice outputs) that runs a Python 3.12 script
  (`import flvfx as vfx`). The script's controls (`createDialog()` → `vfx.ScriptDialog`) become the plugin's
  automatable inputs; `vfx.addOutputController()` adds control outputs.
- FL's own API reference (the plugin's "API reference" tab, text inside `VFX Script_x64.dll`) [MANUAL]:
  `context` (isPlaying, ticks, PPQ, tempo, voices, form), `addOutputController(name, defaultvalue)`,
  `setOutputController(name, value)`, `class Voice([incomingVoice])` with pan, velocity, note, finePitch, color,
  length, releaseVelocity, fcut, fres, output and trigger() / release() / copyFrom(otherVoice); callbacks
  onTriggerVoice(incomingVoice), onReleaseVoice(incomingVoice), onTick(), createDialog(); "see the Piano Roll
  scripting reference for ScriptDialog". The editor's tabs: User interface, Script, Log ("Use print-function in
  script to output text to this log"), API reference [EXEC FL, DLL form resources].
- FL's tutorial scripts: an incoming voice cannot be triggered (a copy is), and they release voices while walking
  `vfx.context.voices` — LYNX's `voices` is a snapshot list so that works.

## 2. LYNX's design

- **pocketpy 2.2** (MIT, one C file, ~1.3 MB of source, ~0.5 MB in the binary) instead of CPython (~30 MB): a Python 3
  subset; LYNX adds what FL's factory scripts need (`re` on std::regex, 3-argument pow, sys.float_info, random.gauss /
  seed of any value, generator expressions, backslash line continuation, `match` as a soft keyword).
- `src/core/Python.{h,cpp}`: one Python thread runs every script (pocketpy keeps global state); up to 15 VMs, one per
  script instance; sandbox (no `os`, no file imports); a time limit (supervisor thread → interrupt flag checked per
  opcode; a stopped script cannot catch it in its own `except:`); a counted heap (stopped past 768 MB).
  The audio thread wakes the Python thread with an OS semaphore (std::counting_semaphore polls with Sleep() under
  Zig's libc++ for Windows).
- `src/core/VfxScript.{h,cpp}`: LYNX's own `flvfx` (written from the API reference above), the processor and the
  values / [Parameters] encodings. Real time: each audio block hands its notes + ticks to the Python thread
  (lock-free ring) and waits at most 0.3 × the block (0.2–2 ms); an answer that comes later plays at the next block's
  start, and while it is late the audio thread does not wait at all. Exports wait for every answer (exact).
- Editor (`src/ui/VfxEditors.cpp`, ScriptBody): FL's four tabs. User interface: the script's controls in FL's two
  columns (knobs, checkboxes, combos, text), group headers, output-controller bars, live values (automation,
  setNormalizedValue) and the error that stops the script. Script: code editor with Python colours and line
  numbers, Apply (Ctrl+Enter), Revert, Restart, "Line N: error" button that jumps to the line. Log: print() and
  errors (the same error repeated is counted, not flooded). API reference: LYNX's own text.

## 3. Verification

- Core tests (build-local with FL Data): every vfx_script test passes; 89/89 of FL's factory VFX Script presets
  compile and run 2 s with a held chord without an error (slowest: Sustain Pedal Weighted / Constant Speed Slowdown,
  2 s of audio in 0.25–0.5 s). Endless loop: stopped, audio block wait p95 1.9 ms, worst 5.9 ms. Runaway memory:
  stopped at 768 MB with "MemoryError" in the editor.
- FL vs LYNX renders [EXEC: FL64.exe /R of 12 projects made from FL's "Patcher (generator)" render test, a VFX
  Script node with a factory script before Sytrus; LYNX --export of the same .flp]. Note detection per 11.6 ms frame
  (tmp/vfxscript165/py/compare.py) and energy onsets (env.py), within FL's render length:

| case | script | same notes per frame | onsets (time) |
|---|---|---|---|
| tut1 | Tutorial 1 (pass-through) | 100 % | identical |
| tut2 | Tutorial 2 (+7, pan, velocity) | 98.8 % | identical |
| tut3 | Tutorial 3 (onTick pulse every 48 ticks) | 91.3 % (release tails) | identical |
| arp | Arpeggiator (Up, 2/beat) | 94.8 % | identical |
| arpsync | Arpeggiator Sync, Speed 3 | 90.6 % | identical (after fix 2) |
| harm | Harmonize | 100 % | identical notes |
| snap | Snap To Scale (Enable, Down) | 100 % | identical |
| contrary | Contrary Motion | 94.6 % | identical |
| formula | Note Formula "note + 12" | 100 % (after fix 1) | identical |
| repeat | Note Repeat | 97.3 % | identical |
| micro | Microtune | 100 % | identical |
| diag | own script: velocity, PPQ, tempo, onTick count | 84.3 % | identical; onTick count = FL's (after fix 1) |

- Fix 1 [EXEC]: FL calls onTick while the song is stopped too, so before a render's first note a script has ticked
  (FL's Note Formula compiles its formula in onTick: FL's first note was +12, LYNX's was not; the diag script's tick
  counter at tick 288 was one lower in LYNX). LYNX now runs one stopped onTick before the first block after a
  compile; the diag counter and the first note now match FL.
- Fix 2 [EXEC]: the render test's Patcher channel has "cut itself" (cut group 1, cut by 1). LYNX turned each new
  note's cut into an all-notes-off of the Patcher, which cleared the VFX Script's held input voices, so the synced
  arpeggio never heard the release of the notes that started earlier and played on to the end of the render. FL
  keeps them (its arpeggio stops at the notes' end, 2.17 s). A cut-group cut now stops the Patcher's instruments but
  leaves its VFX nodes' voices (`Instrument::cut()`); stop / panic still clear them.
- Left open: Contrary Motion: FL has a ~40 ms one-semitone-lower blip at each release (both pitches sound), LYNX
  not; the tails then match. FL's onTick runs a few extra times before playback (diag: 290–299 by tick 288); LYNX's
  one warm-up tick matches the measured note. FL renders stop at the song end, LYNX's export renders tails (tut3 /
  arpsync keep generating notes while the transport runs on in the tail).
- Screenshots: `LYNX --vfxshot prefix` (with LYNX_FL_INSTALL pointing at FL) writes the VFX Script editor with FL's
  Snap To Scale (User interface tab), its Script / Log / API reference tabs and a script with a syntax error
  (Script tab: "Line 8: SyntaxError: expected an expression, got def").
