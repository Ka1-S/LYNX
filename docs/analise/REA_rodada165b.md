# Round 165b (linuxmouse165) — "WaveShaper and automation editing ignore the mouse on Linux" (issue #11)

Issue #11 (Jocho-Smith, Fedora 44, 0.1.18 tar.gz): "fruity waveshaper looks correct but I cannot change the shape with my
mouse" and "automation editing also seems to not work for me (… piano roll and everything else works though, so it's
not the mouse output itself)".

Evidence markers: **[CODE]** LYNX source on main before this round, **[DOC]** FL's manual as summarised in
`docs/WORKFLOW_FL.md` (A2/A3), **[STR]** strings of FL Studio 2026's `FLEngine_x64.dll`, **[SHOT]** screenshot taken
in this round (Xvfb / WSLg), **[TEST]** unit test.

## Where I stopped

- 2026-10-27 15:26 BSOD killed the first agent; its work was saved as the checkpoint commit d33eab8e (unverified).
- 16:05 resumed (r165b): diff read and summarised below.
- 17:10 Linux build OK; Xvfb + xdotool sweep done (§3); Alt+click delete on automation points replaced by FL's Alt = no
  snap; Playlist tension drag scaled to the clip height. Next: core tests with FL Data, land.

## 1. Root cause (not a Linux mouse bug)

1. **Fruity WaveShaper**: LYNX drew the saved shaping curve but had **no point editor at all** — the display item was
   paint-only (`PluginEditors.cpp`: `// the saved shaping curve (FL's point editor is not reproduced)`) [CODE]. Nothing
   reacted to the mouse on any OS; on Windows nobody had noticed because FL projects bring their curve along.
2. **Automation in the Playlist**: LYNX's in-clip editing needed the Pencil/Brush tool and a left click *exactly on the
   curve* (±5 px) to add a point; a right-click on a point deleted it and a right-click anywhere else in the clip
   **deleted the whole clip** [CODE]. FL's muscle memory is the other way round: **right-click (and drag) adds a point**,
   right-click on a point opens its menu, left-drag moves, a hollow **tension handle** between two points bends the
   segment [DOC A2/A3]. An FL user right-clicking to add points therefore lost the clip or saw nothing happen — which
   reads as "the mouse doesn't work".
3. Same FL gesture gap in LYNX's other point editors (controller envelopes, VFX curve editors): double-click only to
   add a point, no right-click-add [CODE].

Linux-specific suspects checked (see §3): unbounded mouse movement / pointer warping (XWayland), modifier detection,
double-click timing, OpenGL child components.

## 2. What the checkpoint (first agent) built

- `core/FruityShapers`: `waveShaperForEditing`, `writeWaveShaper` (FL's state layout 4, header/editor flag kept),
  point add/move/remove, `waveShaperLevelAt`, FL's 13 segment modes in FL's menu order; `WaveShaperEffect::acceptState`
  swaps new tables in on the audio thread without allocation (`StateHandoff`).
- `PluginEditors.cpp`: the WaveShaper graph is an editor (right-click add + drag, left-drag move with Shift/Ctrl locks,
  tension handles, point menu with shape / copy / paste / type in value / delete).
- `PlaylistComponent.cpp` + `EditorGestures.h`: FL's in-clip automation editing (right-click add + drag, point menu,
  tension handles, autoTension drag).
- `AutomationUi.cpp`: tension handles, right-click add + drag, shared point-value helpers.
- `ControllerEditors.cpp`, `VfxEditors.cpp`: right-click adds a point.
- Tests `tests/LinuxMouse165Tests.inc`; pt-BR strings.

## 3. Verification (this round)

Harness: Xvfb 1600×1000 (no window manager, user namespace + overlay for xkbcomp), LYNX built for Linux from this
branch, fresh profile, real X events through XTEST (`xdotool`), screenshots `xwd` → ffmpeg. Note for later rounds: a
hand-written XWD→PNG converter misread the row stride and produced a skewed image, so clicks seemed to "miss"
everything — use ffmpeg to convert XWD. Under WSLg (Weston + XWayland) XTEST clicks are not delivered to X clients at all
(motion only, checked with a bare Xlib window), so WSLg can't be driven by xdotool; Xvfb is the reproducible path.

| # | Editor / gesture (real X11 mouse events) | Result |
|---|---|---|
| 1 | 0.1.19 tar.gz and AppImage, fresh profile | start, welcome wizard, Skip works [SHOT] |
| 2 | Playlist automation clip: right-press in the clip + drag | point added and follows the mouse [SHOT] |
| 3 | Playlist: hover / drag a tension handle | hint "Tension: …", segment bends (after the fix the handle follows the mouse on a short clip) [SHOT] |
| 4 | Playlist: right-click a point | FL's point menu (value, Delete, Copy/Paste, Type in, Straight, Edit points…) [SHOT] |
| 5 | Automation editor window (separate X window): right-press + drag, left-click add + drag, tension handle drag | all react [SHOT] |
| 6 | Fruity WaveShaper window: right-press + drag adds a point (value tip "In 28 % Out 89 %") | OK [SHOT] |
| 7 | WaveShaper: tension handle drag, left-drag a point, right-click point → mode "Stairs", Alt+click deletes | OK [SHOT] |
| 8 | WaveShaper PRE knob drag | OK (+3.2 dB tip) [SHOT] |
| 9 | Parametric EQ window: drag band 3, wheel = Q | OK (band follows, Q 2.71) [SHOT] |
| 10 | Unit tests: state round trip (FL layout 4, header kept), point edits, running effect takes the new curve without rebuild (bit-exact vs a rebuilt effect) | [TEST] |

LYNX has no `enableUnboundedMouseMovement` and no pointer warping on the editing paths (the only `setMousePosition` is
Shift+wheel track reordering in the Playlist; on Wayland the pointer just doesn't follow — harmless). No OpenGL child
components in these editors. Conclusion: nothing Linux-specific blocks the mouse; the two reports were the missing
WaveShaper point editor and the FL gestures missing in automation editing, both fixed here.

## 4. FL behaviour followed

- Automation (Playlist and editor) [DOC A2/A3]: right-press adds a point and drags it while held; Shift+right-click adds
  on the curve keeping the level; left-drag moves (Shift keeps time, Ctrl keeps value, **Alt = no snap**); right-click a
  point = its menu; hollow tension handle per segment, drag up/down (Ctrl fine), right-click = straight.
  The checkpoint had Alt+click delete a point; FL documents Alt as "no snap" while dragging, so that was dropped.
- WaveShaper: same point-editor gestures; segment shapes = FL's 13 modes in FL's menu order; Alt+click delete is kept
  there (no snap concept on the map, no conflict).

## 5. Left for later

- The WaveShaper window at FL's size truncates a few labels ("Fr…", "Wav…", "S…") — pre-existing layout, not touched.
- Fedora GNOME (mutter / XWayland) itself was not available here; Xvfb (X11) covers the X path LYNX uses on Wayland too
  (JUCE 8 talks X11 through XWayland).
