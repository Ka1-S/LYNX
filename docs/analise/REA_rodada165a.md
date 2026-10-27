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
