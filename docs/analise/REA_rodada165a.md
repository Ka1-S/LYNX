# Round 165a (fl165) — issue #11: FL muscle-memory items

Issue #11 (Jocho-Smith, Fedora 44, 0.1.18 tar.gz) lists FL Studio habits LYNX did not follow. Items 11/12 (Fruity
WaveShaper and automation editing ignoring the mouse on Linux) belong to round 165b.

Evidence markers: **[MAN]** FL's manual (the HTML help that ships with FL 2026), **[FL]** FL 2026 running on this PC,
**[DECOMP]** Ghidra on `FLEngine_x64.dll`, **[TEST]** LYNX unit tests, **[SHOT]** LYNX screenshots.

## Where I stopped

- 2026-10-27 15:26 the PC blue-screened mid-round; the previous agent's work was saved as checkpoint commit 3719884d
  (unverified, it was compiling the Linux build). Resumed 16:00: rebuilt the state below from the diff.
- State per item at resume (checkpoint):
  1. Piano Roll keyboard velocity by click position — done in code (`gesture::keyClickVelocity`), Piano Roll only.
  2. Silent step entry — done in code (applyPaint no longer previews).
  3. Drag & drop samples into the Channel Rack — already existed (`ChannelRackComponent::filesDropped/itemDropped`);
     needs a Linux check (Browser and the file manager) and the empty panel area.
  4. Single-key window shortcuts — not started.
  5. Ghost steps after the pattern's end — done in code.
  6. Mixer slot: Delete in the slot menu / on the selected slot — done in code.
  7. Renamed plugins + type the first character in slot menus — done in code (`ui/QuickPick.*`, PluginHost names).
  8. Playlist double right-click and hold = mute — done in code.
  9. Transport Stop stops the Browser preview — done in code.
  10. Playlist vertical zoom (Alt+wheel, handle above the scroll bar) — done in code.
- Nothing compiled, tested or screenshotted yet; pt-BR strings for most new texts missing.

## FL's behaviour, item by item

FL 2026's help is online only (the install ships no HTML manual), so the evidence is FL's own binary
(`FLEngine_x64.dll`: the binary DFM of `TShortcutsModule`, its action list with the default `ShortCut` of each action,
parsed with a small DFM reader; strings) plus FL's documented gestures already collected in docs/WORKFLOW_FL.md.

| # | FL | Evidence |
|---|---|---|
| 1 | The Piano Roll keyboard plays a clicked key softer near its base (left) and at full velocity at its tip (right). Channel settings' keyboard (Misc tab) only picks the root note; LYNX's Touch keyboard already maps the touch position to velocity | issue text, [MAN] |
| 2 | A step click in the Channel Rack is silent (stopped or playing); FL has no option to audition steps (its rack options are "Preview up to current step" and graph/preview toggles) | [DECOMP strings], issue |
| 3 | Samples dropped anywhere in the Channel Rack become a new Sampler channel; dropped on a channel, they replace its sample | [MAN] |
| 4 | Defaults in `TShortcutsModule` [DECOMP]: F5 Playlist, F6 Channel rack, F7 Piano roll, F8 Plugin picker, Alt+F8 Browser, Shift+F8 Plugin database, Ctrl+F8 Project picker, F9 Mixer, F10 MIDI settings, F11 Project info, F12 Close all windows, Alt+F12 Close all plugin windows, Ctrl+F12 Close all unfocused windows, Shift+F12 Align channel editors, Ctrl+F5 Tempo tapper, Alt+F7 Touch controller, L pattern/song, R record. **No single-letter window keys (P, M) by default**: FL 2026 has a Shortcut editor ("Set custom shortcuts for various actions", `il.CustomShortcuts`) where users bind them | [DECOMP DFM @ file offset 0x14EEF9C] |
| 5 | Steps after the pattern's end are drawn faint; a click there sets a step and lengthens the pattern | issue, [MAN] |
| 6 | Right-click an effect slot, then Delete removes the plugin (menu accelerator); Del also on the selected slot | issue |
| 7 | Renaming plugins in the Plugin database ("1_EQ") puts them first in the slot menus; the slot menu's type-ahead picks the one item starting with the typed character | issue, Windows menu behaviour |
| 8 | Double right-click and hold = the Mute tool while held (P2 in WORKFLOW_FL) | [MAN] |
| 9 | The transport Stop ends a Browser preview | issue |
| 10 | Vertical zoom of the Playlist: wheel with Alt (Ctrl+Alt in the editors), and the zoom grips of FL's scroll bars | [MAN], [HYP] for the exact modifier |
