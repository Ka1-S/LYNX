# Round 162d (look162p): plugin windows and plugin presets — issues #8 and #9

2026-10-26. Part of round 162d (look162: theming, plugin windows, parameter menus). This fork covers:
- #9 "When removing a VST effect or instrument, the removed VST's GUI stays on screen";
- #8 (Linux) "putting a plugin on wrapper settings, the original plugin window still appears over everything, making
  the wrapper settings inaccessible";
- #8 "Add plugin presets" (FL: the wrapper's presets menu, .fst).

Evidence markers: [CODE] read in the source (LYNX, JUCE 8.0.4); [TEST] core test; [RUN] run here (WSL2, WSLg / Xvfb,
Linux build, DPF / LSP / Dragonfly test plugins from ~/opt/r161b).

## 1. Causes

| Bug | Cause [CODE] |
|---|---|
| Removed plugin's window stays (#9) | `PluginWindows::open` keeps the adapter (`PluginInstrument` / `PluginEffect`) alive through a `shared_ptr` ("keep the plugin alive while its window is open", so the editor never outlives the plugin). Nothing closed the window when the song stopped using the plugin. LYNX's own effect windows (layout editors, Pro editors, Facet EQ, Newtone, Formula controller) are keyed by (insert, slot) and also stayed open, showing nothing. |
| Wrapper settings covered (#8, Linux) | (a) JUCE's `XEmbedComponent` (VST3 and LV2 editors on Linux; LYNX's `NativeEmbed` for CLAP and VST2) maps / unmaps the plugin's X window only when the peer changes — never on `setVisible(false)`. Hiding the editor component for the Wrapper settings tab left the plugin's native window on top of the page. (b) With "Run plugins in a separate process" (on Linux since 0.1.17) the plugin's own window lives in the child process and was `setAlwaysOnTop (true)`: over every window of every program, the wrapper settings included. |

## 2. What was built

| Part | Where | Notes |
|---|---|---|
| Orphaned plugin windows close | `ui/PluginWindow.cpp` `OrphanReaper` | 4 Hz while a plugin window is open: a window whose adapter is held by nothing but the window (`use_count() == 1`: removed, replaced, channel deleted, project closed) closes. The engine drops a removed adapter when the audio thread has let go of its last snapshot (retired snapshots are collected at 30 Hz), so the window goes within ~0.3 s. `PluginWindows::closeOrphans()` does it at once. |
| Native effect windows close | `ui/SlotWatch.h` `SlotGoneWatcher`, used by `NativeEffectWindow`, `ProEditorWindow`, `FacetEqWindow`, `NewtoneWindow`, `FormulaWindow` | closes the window when its slot is removed or holds another kind of effect. |
| Wrapper settings reachable | `ui/PluginWindow.cpp` `WrappedEditor::show` | the editor is also moved outside the window (−12000, −12000; X11 positions are 16-bit even when scaled) and every XEmbedComponent inside it is told to follow (JUCE only moves its X window when the XEmbedComponent itself moves, not its parent) while the settings page shows, which clips a native child window away on every platform; `resized()` puts it back. |
| Separate-process plugin window | `engine/PluginSandbox.cpp` `RemoteEditor`, `SandboxWindow` | the stand-in inside LYNX's plugin window tells the child to show the plugin's window while the stand-in is on screen and to hide it when it is not (Wrapper settings tab, LYNX window minimised or closed, plugin removed): new `hideEditor` message, polled at 4 Hz plus `visibilityChanged`. Linux: the child's window is made transient for LYNX's main window (`WM_TRANSIENT_FOR` through its own libX11 connection, dlopen) instead of always-on-top, so it stays above LYNX but not above other programs. Windows: still always on top — a cross-process owner would attach both processes' input queues, and a plugin hanging its window would freeze LYNX (the point of the separate process); the hide/show covers the settings page. |
| Plugin presets | `core/PluginPresets.{h,cpp}` (formats), `ui/PluginPresetMenu.{h,cpp}` (menu, files), wrapper header | ◀ ▶ and the preset name in the plugin window's header (next to Plugin / Wrapper settings). Menu: the plugin's own programs (factory presets; a sub-menu when > 32), "Your presets" (the plugin's folder), Save preset..., Load preset file..., Export preset file..., Delete preset, Show the presets folder. ◀ ▶ step through programs then user presets. A change marks the project modified. |
| Formats | same | `.lynxpreset` (LYNX's own: the plugin's whole state + its name and id, text with base64; any format — VST2, VST3, CLAP, LV2, separate process). VST2 `.fxp` / `.fxb` read and written by LYNX (big-endian `CcnK` layout: FxCk / FPCh / FxBk / FBCh) through new `vst2::info / getChunk / setChunk` (`engine/Vst2Format.h`); parameter programs also load into any plugin with the same parameter count. VST3 `.vstpreset` through JUCE's VST3 host (`ExtensionsVisitor::VST3Client::getPreset / setPreset`). |
| Folder | `<settings>/Presets/Plugins/<plugin name>/` | `.fxp`, `.fxb` and `.vstpreset` files copied there are listed too. |
| Browser | `ui/BrowserComponent.cpp` | new place "Plugin presets" (that folder); preset icon + hint; double-click: into an open plugin window's plugin (the last used first) or the selected channel's plugin when it is the preset's plugin; a `.lynxpreset` of an instrument nobody has open becomes a new channel with the plugin in that state; otherwise a message says to open the plugin first. |
| Test mode | `Main.cpp --plugwintest plugin report.txt ["name"] [seconds]` | hosts the plugin in an offline engine (channel or insert 1), opens its window with the wrapper header, checks the preset round trips (.lynxpreset / .fxp / .fxb / .vstpreset), keeps the window for xdotool / screenshots, removes the plugin and reports whether the window closed. |
| Tests | `tests/PluginPresets162Tests.inc` [TEST] | `.lynxpreset` text round trip (and refusal of a LYNX engine preset), folder names, `.fxp` / `.fxb` byte layout and round trips of all four kinds, damaged files refused. |

LYNX's own engines already had presets (round 129: the preset bar of every LYNX plugin header, `core/EffectPresets`).

## 3. Not done / limits

- Presets of a plugin running in a separate process: `.lynxpreset` only (its programs, `.fxp` chunks and `.vstpreset`
  need the plugin in LYNX's process; the menu says so).
- CLAP's preset-discovery and LV2 preset bundles are not browsed (LV2 presets appear as JUCE programs where the plugin
  lists them; `.lynxpreset` works for both).
- FL's `.fst` presets of third-party plugins (FL wrapper state) are not applied to plugins (the Browser keeps reading
  FL synth `.fst` presets as before).
- Linux: LYNX's own secondary windows still have no WM_TRANSIENT_FOR to the main window (only the separate-process
  plugin window got it); left out as riskier than this round's scope.

## 4. Verification

| Check | Result |
|---|---|
| [TEST] `plugin_preset_text_round_trip_round162`, `plugin_preset_folder_names_round162`, `vst2_fxp_fxb_round_trip_round162` | pass (build-local with FL Data) |
| [RUN] Linux (Xvfb), `--plugwintest` Dragonfly Room Reverb (VST3) | window with header + presets; Wrapper settings click: the page is fully visible (before the fix the plugin's X window covered its top 340 px); Plugin tab: the editor is back; `.lynxpreset` round trip restores Dry Level 0.8 after setting 0.1; `.vstpreset` save + load OK; plugin removed → window closed (`window_open_after_remove=0`). Screenshots D:\lynx-work\look162p\linux-*.png |
| [RUN] Linux, MVerb (VST3, 5 programs) | presets menu lists Halves / Dark / Cupboard / Stadium / Subtle + "Your presets"; ▶ steps programs then user presets and wraps |
| [RUN] Linux, MVerb in a separate process (sandbox.txt = 1) | the child's plugin window: `IsViewable` → Wrapper settings tab: `IsUnMapped` → Plugin tab: `IsViewable` → plugin removed: window gone; `WM_TRANSIENT_FOR` = LYNX's main window, no always-on-top state |
| [RUN] Windows, ValhallaFreqEcho VST2 (`.dll`) | `.lynxpreset`, `.fxp`, `.fxb` save + load OK, value restored; window closed after removal |
| [RUN] Windows, ValhallaFreqEcho VST3 | `.lynxpreset`, `.vstpreset` OK; Wrapper settings page shows cleanly (HWND child hidden); window closed after removal. Screenshot D:\lynx-work\look162p\windows-wrapper-settings.png |
| LYNX's own effect windows closing with their slot | [CODE] only (`SlotGoneWatcher`), not run |

## Where I stopped

All three items done and verified on Linux and Windows (see §4); branch feature/look162p, to be merged into
feature/look162 by the round's main agent. Open: the limits in §3.
