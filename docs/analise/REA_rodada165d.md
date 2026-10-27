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
