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
- Next: build (Linux) → shader check under Xvfb/llvmpipe → DanceView.cpp + wiring → tests → shots → Windows build.

## GPU safety (after the BSOD)

- No continuous repainting: frames only when the message thread hands one over (30/s editor, 60/s full screen,
  a third in silence), vsync on; live picture at most 1920 x 1080; fixed small loop counts in the shaders.
- Session note "visualizer GPU: start/stop": a crash with the GPU view drawing turns the GPU path off at the next
  start (Main.cpp) with a message.
- Checks: shaders compiled and rendered on Linux (Xvfb + Mesa llvmpipe, software); real Windows GPU at most a few
  seconds per check, tiny windows, under ~/.cache/lynx-wingui.lock.
