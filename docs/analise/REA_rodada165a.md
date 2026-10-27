# Round 165a (fl165) — issue #11: FL muscle-memory items

Issue #11 (Jocho-Smith, Fedora 44, 0.1.18 tar.gz) lists FL Studio habits LYNX did not follow. Items 11/12 (Fruity
WaveShaper and automation editing ignoring the mouse on Linux) belong to round 165b.

Evidence markers: **[MAN]** FL's manual (the HTML help that ships with FL 2026), **[FL]** FL 2026 running on this PC,
**[DECOMP]** Ghidra on `FLEngine_x64.dll`, **[TEST]** LYNX unit tests, **[SHOT]** LYNX screenshots.

## Where I stopped

- 2026-10-27 15:26 BSOD; the previous agent's work is checkpoint 3719884d. Resumed 16:00.
- 17:05: checkpoint compiles on Linux; verified in the Linux app under Xvfb (shots in the session scratchpad, see
  "Verification"): items 1, 3 (Browser drag and external XDND drop), 5, 6, 7, 8, 10. Added since: FL's F-key
  window actions (item 4), visible ghost steps, drops on the rack panel's free space, Ctrl+Alt+wheel, the Theme page
  clamp fix, unit tests `tests/Fl165Tests.inc`, pt-BR strings.
- 18:12: drop on the rack's free space failed in the app: juce::Viewport calls `setInterceptsMouseClicks (false, true)`
  on itself, so the drag target search skipped it. `ChannelRackViewport` now intercepts clicks. Next: re-test that
  drop, finish build-local + Windows build, merge main, land.
- 18:31: free-space drop verified (`u10-freedrop.png`), Theme page verified. Waiting for build-local + Windows build,
  then land.

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

## What LYNX does now

| # | LYNX | Where |
|---|---|---|
| 1 | Piano Roll keys: velocity from the click's x on the key (`gesture::keyClickVelocity`), hint shows "vel N" | `EditorGestures.h`, `PianoRollComponent` |
| 2 | Step clicks never preview (Ctrl+click the channel button auditions) | `ChannelRackComponent::applyPaint` |
| 3 | `ChannelRackViewport` forwards file and Browser drops on the panel's free space; it intercepts clicks because juce::Viewport makes itself click-transparent | `ChannelRackComponent.h` |
| 4 | New KeyMap actions with FL's defaults: Shift+F8, F10, F11, F12, Alt+F12, Ctrl+F5, F2 (Browser rename), Del (Mixer slot); View/Options menu entries. P/M stay unassigned (FL has none) and can be set in Options › Manage shortcuts | `KeyMap`, `MainComponent::runAction` |
| 5 | Ghost steps outlined past the pattern end; a click sets the step and lengthens the pattern by whole bars (`lengthWithStep`) | `RackOps`, `ChannelRackComponent` |
| 6 | Slot menu "Delete\tDel"; Del while the menu is open | `QuickPick`, `EffectsPanel::deleteSlot` |
| 7 | Plugin display names (`plugin-names.txt`), Browser Rename.../F2/Original name; renamed plugins first in slot/add menus; one-key type-ahead | `PluginHost`, `prefs`, `QuickPick` |
| 8 | Double right-click and hold = mute while held | `PlaylistComponent` |
| 9 | Transport Stop also stops the Browser preview | `MainComponent::stopAndRewind` |
| 10 | Playlist `trackZoom`: Alt+wheel, Ctrl+Alt+wheel, grip above the vertical scroll bar (double-click = 100%) | `PlaylistComponent` |
| fix | Options › Theme opened the MIDI page (page index clamped to 0..5) | `MainComponent::openSettings` |

## Verification

Linux build under Xvfb (shots in the session scratchpad `shots/`):
- 1 `s29-vel.png`: clicks at a key's left/middle/right gave vel 10/62/121.
- 3 `s24-dropped.png` (Browser drag → new channel / onto a channel), `s25-xdnd.png` (external XDND drop), `u10-freedrop.png`
  (file dropped on the empty space under the channels of an enlarged rack panel → new Sampler channel; before the
  viewport fix the same drop was ignored, `t04all.png`).
- 4 `u11-options.png` (F10/F11 in the Options menu), `u12-theme.png` (Options › Theme opens the Theme page). The F-keys
  themselves don't reach JUCE through xdotool on Xvfb; covered by unit tests (defaults and no conflicts).
- 5 `t02c.png` (outlined ghost steps), `s04-ghostclick.png` (click on a ghost step → pattern of 2 bars).
- 6 `s18-slotmenu.png`, `s20all.png` (Delete removes the plugin).
- 7 `s11c.png`, `s13c.png` (EQ renamed 1_EQ, "Renamed plugins" header), `s19-picked.png` (typing 1 added it).
- 8 `s08.png` (two clips muted by double right-click and hold).
- 10 `s06-vzoom.png` (173 %, 81 %, grip double-click back to 100 %).
- 2 and 9: code-verified (no audio device under Xvfb).
- Unit tests `tests/Fl165Tests.inc`: velocity mapping, `lengthWithStep`, plugin-name file (CRLF too), type-ahead, FL
  window keys and conflict check.

## Left for later

- The Quick-add palette (F8) and the slot's label still show the plugin's original name.
- FL's exact modifier for vertical Playlist zoom is [HYP] (Alt+wheel follows FL's documented editors).
