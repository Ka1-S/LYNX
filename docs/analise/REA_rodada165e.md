# Round 165e (zge165) — a ZGameEditor-style GPU path for the Visualizer + LYNX Dance (Fruity Dance)

Brief (tmp/round165-brief.md "r165e"): an OpenGL path for the Visualizer (own component, falls back to the 2D
renderer), GLSL shader layers in the spirit of ZGE's categories (Postprocess, Feedback, Canvas, Terrain/3D, blend
modes, HUD layers), audio-reactive uniforms, presets, full screen, 1080p video export, ZGE preset mapping on import;
the GPU path must not raise audio CPU and must never stress the GPU/driver. A Fruity Dance equivalent with original
art (procedural dancer, moves picked by notes), FL's Fruity Dance channels imported.

## Where I stopped

- 2026-10-27 15:26: the PC blue-screened (kernel driver fault) while this round was running; the work on disk was
  saved as the checkpoint commit "WIP zge165" (unverified). Resumed 16:00 from that commit.
- State at resume (rebuilt from the diff): VisualFx (shader layer types, effects, blend modes, GPU presets, ZGE
  mapping) done; VisualGl.{h,cpp} + VisualGlShaders.inc (renderer, live view, exporter) written but never compiled;
  VisualEditors wired (LivePicture, GPU button, effect/blend boxes, export through the GPU, --visualtest);
  core/FruityDance.{h,cpp} done; ui/DanceView.h declared but **DanceView.cpp not written**; no tests yet.
- 16:20 PAUSED by the coordinator (machine load 33). Done since resume (all UNCOMPILED so far — no build ran yet,
  the Linux build waited for a slot and was stopped):
  - ui/DanceView.cpp written (vector lynx dancer, DanceStage over the workspace in MainComponent, DanceEditor as the
    channel's page via makeInstrumentPage / makeChannelSettingsPanel), "Fruity Dance" in the Channel Rack Add menu,
    `--danceshot prefix` CLI (moves sheet, editor, demo .lynx), CMake + build-windows-zig.sh sources.
  - VisualGl: live picture capped at 1280 x 720 inside; LYNX_VISUALIZER_GPU=0 forces 2D; --visualtest env
    LYNX_VISUALTEST_ONLY / _SECONDS / _SMALL / _NO_VIDEO / _VIDEO_SECONDS for short GPU checks.
  - ZGE import: a scene with only feedback/post layers keeps them over the default scene.
  - tests/Visual165Tests.inc (shader layers, ZGE GPU mapping, dance state/keys/poses/signals); Visual164 test updated.
  - FL key map verified in the decompile (tmp/zge165/d2.c FUN_0071a410: keys 59.. = moves, 48.. down mirrored,
    53 = 0x10000 hide, 54 = 0x10001 show).
- Next on resume (respect LOAD LIMIT): Linux build (tmp/zge165/blinux.sh, PATH needs $DR for pkg-config) → fix compile
  errors → build-local tests → Xvfb+llvmpipe shader check (LIBGL_ALWAYS_SOFTWARE=1) → --danceshot shots → pt-BR strings
  (tools/i18n_keys.py --missing + dynamic names: moves, effects, blends, types) → Windows build + short GPU check →
  notes-0.1.20.md → land.

## GPU safety (after the BSOD)

- No continuous repainting: frames only when the message thread hands one over (30/s editor, 60/s full screen,
  a third in silence), vsync on; live picture at most 1920 x 1080; fixed small loop counts in the shaders.
- Session note "visualizer GPU: start/stop": a crash with the GPU view drawing turns the GPU path off at the next
  start (Main.cpp) with a message.
- Checks: shaders compiled and rendered on Linux (Xvfb + Mesa llvmpipe, software); real Windows GPU at most a few
  seconds per check, tiny windows, under ~/.cache/lynx-wingui.lock.
