# Round 164c (visual164) — Visual plugins: Wave Candy, Visualizer, video export

Brief: a Wave Candy-like analyzer as a mixer effect; scope Fruity Dance / ZGameEditor Visualizer and ship at least a
LYNX "Visualizer" effect with audio-reactive scenes, a full-screen window and a video export of the song; a Visual
category in the plugin picker and the Patcher's Add-plugin menu. Priority from the user: performance and quality
first — a visualizer must not raise the audio CPU.

## 1. What FL has (clean-room study)
- [FL] **Wave Candy** (`Plugins/Fruity/Effects/Wave Candy`, Visual category in FL's plugin database): one display
  with four pages — Oscilloscope, Spectrum (curve, or a scrolling spectrogram), Meter (Peak, PPM, RMS, VU, Leq(A),
  ITU-R BS.1770) and Vectorscope — and an Auto mode that picks the page from the window's shape (tall → meter,
  wide → scope). Input gain, fade-out, update rate, window, stereo separation, pitch read-out, colours (hue,
  lightness, saturation, opacity). FL's own picture of it: `Data/Patches/Plugin database/Effects/Visual/Wave Candy.png`.
- [FL] Wave Candy's saved state (checked on every Function/Theme preset of the installation): `[title pstring]
  [font pstring][i32 negative font height][4 bytes][i32 mode: 0 oscilloscope, 1 spectrum, 2 peak, 3 vector]`. LYNX
  reads the mode (Spectrum presets whose name says "spectrogram" open as the spectrogram); the colours / fonts are
  FL's theme and are not taken.
- [FL] **ZGameEditor Visualizer**: its state is INI text — `[General] LayerCount/LayerOrder`, one `[AppSetN]` per
  layer with `App=Category\Effect`, Enabled, ImageIndex and up to ~30 parameters, `[Video export] VideoW/VideoH/
  VideoRenderFps`, `[UserContent] Text="…","…" / Images=…`. 230 presets in the installation use 119 distinct effects
  in these categories (count of uses): HUD 1835, Postprocess 765, Image effects 636, Background 412, Misc 351,
  Text 238, Peak Effects 157, Canvas effects 99, Feedback 70, Particles 55, Terrain 53, Blend 28, Object Arrays 26,
  Scenes 7, Physics 2. Most are GLSL shader programs (ZGameEditor scripts) — a GPU scene engine.
- [FL] Side by side (FL 2025 on this machine, Wave Candy added to the master of the "Future House" demo while
  playing): FL shows the picture in a separate floating, rounded display over the workspace and the settings in the
  plugin window (Input gain / fade-out / freeze, Appearance, Colors, then the page's own row: Oscilloscope Update /
  Window / Stereo separation; Spectrum Update / Max res / Scale / dB range, natural weighting; Meter Scale / Hold
  time / Mode / Decay speed; Vectorscope Update / Thickness / Mode "Tube"). Its spectrum page defaults to a hot
  (red-yellow) spectrogram; its vectorscope draws mono as a vertical line, like LYNX's. LYNX keeps the display and
  the knobs in one resizable window (own layout) and the same per-page knob sets. FL's Visual category also holds
  Big Clock, dB Meter, NoteBook(s), Spectroman, Razer Chroma and Tuner (LYNX has its own clock / meter / tuner
  elsewhere; not part of this round).
- [FL] **Fruity Dance** is a generator (an animated dancer driven by the notes, character art + moves). LYNX already
  imports it as a silent visual channel (InstrumentKind::fruitydance). Not reproduced (see §5).

## 2. LYNX: the effects (core/VisualFx.h, .cpp)
- Two new EffectKinds appended: `waveCandy`, `visualizer` (ids "waveCandy" / "visualizer" in .lynx files). Both pass
  the audio through untouched.
- The audio thread only copies each block into a lock-free stereo ring of 32 768 frames per slot (`visualfx::Tap`)
  and publishes the first 16 parameters (so automation shows); no allocation, no lock, no analysis. The editors read
  the ring on the UI thread at 30–40 frames a second and only while visible. `acceptState` returns true, so editing a
  scene never rebuilds the effect. The renderer's "smart disable" (skip silent slots) leaves visual slots alone so
  the picture keeps time.
- Measured audio-thread cost (60 s of stereo in 256-frame blocks, `--visualtest`): see §6.
- **Wave Candy** (18 parameters, all automatable): Mode (Auto, Oscilloscope, Spectrum, Spectrogram, Peak meter,
  Vectorscope), Gain ±24 dB, Hold; scope window 2–500 ms, separation, trigger sync; spectrum speed, range 30–120 dB,
  scale (linear … logarithmic), +3 dB/oct weighting, resolution 1 024–16 384 points; meter mode (Peak, PPM, RMS, VU,
  LUFS momentary — K-weighted), peak hold, decay, scale; vectorscope persistence and thickness; colour (hue).
- **Visualizer**: a scene of layers stored in the slot's state as text ("LYNX visualizer 1", key=value lines).
  Layer types: Background (gradient or picture), Spectrum bars, Circle spectrum, Waveform, Particles, Beat rings,
  Image, Text, Beat flash. Each layer: two colours, opacity, position, size, React (how much the sound moves it),
  the band it reacts to (Level, Bass, Mids, Highs, Beat), speed, amount, count, mirror, glow. Six global knobs
  (automatable): Sensitivity, Beat threshold, Hue shift, Brightness, Zoom, Speed. Five built-in scenes (Neon bars,
  Pulse circle, Starfield, Title card, Scope). The frame analysis (64 log bands, bass/mid/high, beat detection on
  the bass energy) and the particles are deterministic, so the video export and the live preview behave alike.
- Import: FL projects map Wave Candy (mode) and ZGameEditor Visualizer (each `[AppSetN]` to the closest LYNX layer:
  Background\SolidColor → solid background, other backgrounds → gradient, Peak Effects / HUD Graph Linear → bars,
  Polar / FreqRing / HUD Graph Polar / HUD Meter Radial → circle, WaveSimple / StereoWaveForm / JoyDividers →
  waveform, Particles / ReactiveMob / fLuids → particles, Text / HUD Text → text with the project's own lines,
  Image effects / HUD Image → image when the project names a user picture, Bloom / AudioShake / ParameterShake → beat
  flash; the export size and frame rate). The import report lists the Visualizer as "approximated". FL ZGE `.fst`
  presets can also be imported from the Visualizer's Scenes menu.

## 3. LYNX: the editors (ui/VisualEditors.cpp)
- Wave Candy: the display over mode tabs and the knobs of the shown mode (a narrow window gets a mode menu button
  instead of the tabs). Hover reads frequency, note and level (spectrum / spectrogram) or time and level (scope);
  middle-click or the Hold button freezes the display; right-click picks the mode. Oscilloscope with a rising-edge
  trigger and min/max columns when there are more samples than pixels; spectrum with a fall-off; scrolling
  spectrogram (an image moved one step per frame); two-bar meter with peak hold; vectorscope with a fading
  persistence image and a correlation bar.
- Visualizer: live preview at the scene's aspect (double-click: full screen), the layers (add / remove / copy /
  up / down, the dot shows / hides), the selected layer's settings (only the rows its type uses), colour pickers,
  picture chooser, scene menu (built-in scenes, import an FL ZGE preset), Full screen (borderless on the editor's
  display, Esc / click closes), Export video.
- Both open from the mixer slot, from the plugin picker / Browser plugin database ("Visual" category) and as nodes
  in a Patcher (their own window on the node).

## 4. Video export
- Export video… (Visualizer): size (720p … 4K, square, vertical), frame rate (24–60), quality (x264 CRF 23/18/14).
  The song is rendered exactly as the Export window renders it (own engine, offline, PDC trim) to a temporary
  48 kHz WAV while the Visualizer's input is captured; then each frame is analysed from the captured audio (the
  knobs as automated at that moment) and drawn off-screen. With ffmpeg the frames are piped raw (BGRA) to
  `ffmpeg … -c:v libx264 -pix_fmt yuv420p -c:a aac -b:a 320k -movflags +faststart` → one MP4. Without ffmpeg the
  frames are written as PNG with audio.wav and a text file holding the ffmpeg command.
- ffmpeg is searched on the PATH, in the usual Windows install folders (winget links, Chocolatey, Scoop,
  C:\ffmpeg\bin, Program Files) and next to LYNX; "Locate ffmpeg…" remembers a chosen one.

## 5. Not done / left
- ZGameEditor's shader scene engine: Postprocess (bloom, blur, colour correction, ASCII, dot matrix, point clouds,
  transitions…), Feedback, Canvas effects, Terrain, Scenes (Mandelbulb, Cloud Ten…), Object Arrays, Physics, Blend,
  the HUD system's prefab layouts, Misc (Automator, ParamLink, MatCap…), image warps (sphere, slices, tile sprite),
  per-layer blend modes and the 30 per-effect parameters. LYNX draws with JUCE's 2D renderer (no GPU shaders) and maps
  each ZGE layer to the nearest of its 9 layer types; layers with no counterpart are dropped (listed in the import
  report as approximated). A GPU path (OpenGL) would be the next step.
- Fruity Dance (dancer generator): needs character art and move sets; not started.
- Wave Candy's FL colour scheme / custom font and the Leq(A) mode (LYNX meters LUFS momentary instead).

## 6. Measurements
Linux (WSLg) build, `--visualtest` and `--editorcpu`-style runs:
- Audio thread: the tap adds 0.004–0.02 % of real time per slot (60 s of stereo in 256-frame blocks; within the
  noise of the reference loop), also with the export capture armed. No allocation or lock on the audio thread.
- Editors (UI thread, animating with the test signal): Wave Candy 7–15 % of one core depending on the page
  (spectrum the most), the Visualizer preview 9–18 % (Pulse circle the most). In silence they stop repainting (scope
  after 3 frames, spectrum / vectorscope after 90, spectrogram after 80, meter once below its floor; the Visualizer
  preview drops to a third of its rate): 2.2 % (Wave Candy), 4 % (Visualizer) — the process baseline is ~2 %.
- Drawing one 1280×720 Visualizer frame off-screen: 3.4–12.9 ms depending on the scene (cached background and glow
  images, small particles as rectangles; measured while a Windows build ran) — a 3-minute 30 fps video is drawn in
  about real time or faster.
- Windows (LYNX.exe, zig build, `--visualtest` with the FL demo "Sacco - FlashyWaves"): audio path +0.0006–0.0024 %;
  editors 10–22 % animating (vectorscope the most), 2.2–2.8 % idle; scene frame 1.9–9.4 ms; plain export first sound
  at 0.002 s; image-sequence export OK (no ffmpeg installed on this Windows).
- Video: MP4 1280×720 30 fps h264 + AAC with ffmpeg 7.0.2; PNG + audio.wav fallback without ffmpeg; the captured
  input matches the rendered WAV sample for sample (correlation of the first block 1.000).

## Fix found on the way: Export lost the start of the song
`AudioEngine::renderOffline` prepared the JUCE AudioProcessorGraph on the export's worker thread; JUCE then rebuilds
the graph's render sequence asynchronously on the message thread, so the first 0.05–2 s of the song were rendered
silent while the transport already moved on (the Export window's file started late into the song). The graph is now
prepared on the message thread (handshake with a 10 s timeout and a fallback); `--visualtest` checks that the first
sound of a plain export comes before 0.1 s.

## Where I stopped
Everything in §2–§4 landed with unit tests (tests/Visual164Tests.inc), `--visualtest` and `--opentest` passing.
Left for a next round: §5 (ZGE shader engine / GPU path, Fruity Dance, Wave Candy's FL colour themes and Leq(A)).
