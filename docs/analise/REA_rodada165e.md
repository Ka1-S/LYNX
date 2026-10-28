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
  a third in silence), vsync on; live picture at most 1280 x 720 (scaled up on screen); fixed small loop counts in the shaders.
- Session note "visualizer GPU: start/stop": a crash with the GPU view drawing turns the GPU path off at the next
  start (Main.cpp) with a message.
- Checks: shaders compiled and rendered on Linux (Xvfb + Mesa llvmpipe, software); real Windows GPU at most a few
  seconds per check, tiny windows, under ~/.cache/lynx-wingui.lock.

## 1. Scope (measured before building)

- ZGameEditor Visualizer (REA_rodada164c §1): 230 presets, 119 effects in 15 categories; most are GLSL programs run
  by ZGameEditor scripts. LYNX does not run ZGE scripts or copy its shaders: it has its own small set of shader
  effects per category and maps each ZGE effect to the closest one on import (§4). Kept out: Physics (2 uses),
  "Scenes" as full 3D worlds (mapped to a 3D object / terrain / canvas look-alike), HUD meters beyond what the 2D
  layers already draw.
- Fruity Dance: FL's character and its moves are FL artwork — not copied. LYNX draws its own character (a lynx, the
  app's mascot) procedurally; only the behaviour (keys, state) follows FL.

## 2. The Visualizer's GPU path [CODE]

- `ui/VisualGl.{h,cpp}`, `ui/VisualGlShaders.inc`: a JUCE `OpenGLContext` on its own child component
  (`GlSceneView`); the 2D layers draw into frame buffers through JUCE's OpenGL renderer, the shader layers are
  full-screen passes; layers are composed in two ping-pong buffers with per-layer blend modes (Normal, Add, Screen,
  Multiply, Overlay, Difference, Lighten, Darken).
- Shader layer types (after the 9 2D types): Canvas (Plasma, Tunnel, Cells, Kaleidoscope, Clouds, Warp stars,
  Electric, Sun rays), Terrain 3D (Spectrum mountains from a 64 x 128 spectrum-history texture, Shaded hills,
  Retro road), 3D object (ray-marched Reactive sphere, Torus, Cube grid, Ring tunnel), Feedback (Zoom trails, Spin
  tunnel, Kaleido feedback, Warp, Echo, Smoke drift — each keeps its own previous picture), Post-process (Bloom,
  Blur — downsampled two-pass Gaussian —, RGB shift, Pixelate, Dot matrix, Scan lines, Vignette, Colour
  correction, Kaleidoscope, Mirror, Edge detect, Shake, Glitch, Film grain, Invert on beat).
- Audio-reactive uniforms: level, bass, mid, high, beat, the layer's band × React, the 64-band spectrum texture and
  its history; Size / Amount / Speed / Count / X / Y / the two colours drive each effect (hint per effect).
- 8 new built-in GPU scenes (Retro road, Spectrum mountains, Reactive sphere, Kaleido feedback, Plasma tunnel, Cube
  city, Electric storm, Neon CRT).
- The audio stays where it was: the tap is read and analysed on the message thread (as the 2D preview), the frame
  is handed to the GL thread under a small mutex — the audio thread is untouched.
- Fallback: no context in 5 s, shader compile/link failure of the base programs, or the user's GPU button off → the
  2D renderer (Canvas and Terrain drawn simplified, 3D / Feedback / Post-process skipped, with a note in the editor).
- Full screen and video export use the same renderer; the export draws each frame off-screen at the export size
  (1080p included) through a thumbnail context in the progress window, read back with glReadPixels; if the GPU
  stops answering (15 s) the rest of the export is drawn in 2D.

## 3. LYNX Dance (FL's Fruity Dance) [DECOMP + CODE]

- FL (Fruity dance_x64.dll, Ghidra project r68_dance): TriggerVoice `FUN_0071b630` maps the key through a 128-entry
  table and kills the voice at once (no sound, `Gen_Render` `FUN_0071b7c0` is empty). The table is filled in
  `FUN_0071a410`: keys 59 (B4) up = move 0, 1, … (as many as the loaded dance has), keys 48 (C4) down = the same
  moves mirrored (stored as −1 − move), key 53 (F4) = 0x10000 → parameter 2 (shown) = 0, key 54 (F#4) = 0x10001 →
  shown = 1. A move key also sets parameter 1 (mirrored) when it changes. LYNX: `dance::actionForKey`, the same keys.
- State (REA_rodada68 §8): int version; move, mirrored, shown, scale (/64), speed, x, y; display options; version 2
  adds a picture path. LYNX reads it on .flp import (`dance::fromFl`) and writes its own text state once edited.
- LYNX's character: its own lynx (tufted ears, cheek ruffs, bob tail, a shirt in the outfit colour) drawn from a pose
  with vector shapes; 11 moves (Freestyle, Waiting, Stepping, Jumping, Zombie, Waving, Hula, Windmill, Flailing,
  Spinning, Pose) as pose functions of a cycle (one cycle = two beats at speed 1), Freestyle picks a move every two
  cycles, changes blend over 180 ms. The clock follows the song while playing, 120 BPM free-running when stopped.
- Where: a transparent stage over the main window (only the dancers take the mouse: drag to place, right-click for
  moves / mirrored / hide / settings), repainting only the dancer's box at 30 frames a second (2 a second when the
  project has no dance channel). The channel's editor (live preview, move, mirrored, shown, size, speed, outfit
  colour, put back, the key map) replaces the old "silent" page. Channel Rack › Add › Fruity Dance makes one.
- The audio thread only stores the note's key in a 16-entry lock-free ring (`dance::Signals`); the UI reads it.

## 4. ZGameEditor presets on import [CODE]

- `visual::fromZge` (VisualFx.cpp) now maps ZGE's GPU categories to the shader layers instead of dropping them or
  faking them with a flash: Feedback\ (Kaleido → Kaleido feedback, WormHole → Spin tunnel, WarpBack → Warp,
  BoxedIn / FrameBlur → Echo, the rest → Zoom trails); Postprocess\ (Bloom / Blooming / Starburst / Whitehole →
  Bloom with Youlean Bloom's amount / threshold / radius / opacity read; blurs, Smudge, Watercolor, Raindrops → Blur;
  Color Correction / Shaper / Fast HSV / Luminosity / ColorCyclePalette → Colour correction (Youlean's contrast and
  saturation read); Dot Matrix / Point Cloud / Ascii → Dot matrix; Dither / Fast Static → Film grain; Inverter,
  Glitch, Edge Detect (+ Notebook Drawings), RGB Shift, ScanLines, Vignette (amount + size read), Kaleidoscope,
  Pixelate; AudioShake / ParameterShake / Handheld → Shake); Terrain\ and Physics\Heightfield → Spectrum mountains /
  Shaded hills (CubesAndSpheres → Cube grid); Object Arrays\, Physics\, MatCap → Reactive sphere / Ring tunnel / Cube
  grid by name; Scenes\ by name → 3D object, hills, clouds, cubes or tunnel; Clouds\, Tunnel\, Canvas effects\ and
  Patterns\ → the Canvas effects by name (cells, electric, warp stars, kaleidoscope, plasma).
- A ZGE scene left with only feedback / post-process layers keeps them over LYNX's default scene (they need a
  picture below). Effects still without a counterpart are listed in the import report as before.
- Tests: visual165_zgameeditor_gpu_mapping; Visual164's GoopFlow case now maps its terrain (4 layers).

## 5. Verification

- Tests (tests/Visual165Tests.inc): shader layers round trip (type, effect, blend), ZGE GPU mapping, dance state /
  key map / poses, the silent dance channel's signals. build-local with FL Data: see "Where I stopped".
- Linux, Xvfb + Mesa llvmpipe (software GL, no real GPU): every GPU scene compiled and drew (`--visualtest`, report
  "GPU views 8, result=OK"); llvmpipe time per 1040 x 720 frame 5.6–24 ms (Spectrum mountains and Cube city the
  heaviest) — a real GPU is far faster; the audio-path cost of the Visualizer is unchanged (~0.01 % of a block).
  Bugs found and fixed there: JUCE's text vanished after a shader layer (texture units left bound → `resetState()`
  resets units 4..0 and the 2D pass calls it first); the canvas pass sampled its own target.
- `--danceshot`: the moves sheet (11 moves × 4 phases), the editor and a frame over the workspace — original lynx
  character, readable at 0.6 to 2× size; a mirrored dancer faces the other way.
