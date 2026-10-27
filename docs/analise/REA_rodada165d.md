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
