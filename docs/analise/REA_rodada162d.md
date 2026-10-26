# Round 162d (look162): themes, colour picker, plugin windows, plugin presets, parameter menus

Issues: #8 (alltern, Linux: theming, plugin presets, wrapper settings hidden by the plugin window, right-click
parameter menus + last tweaked readout) and #9 (BURNER1PA, Windows 10: accent colour wheel, workspace background
colour/image, mixer "Change color" does nothing, mixer colour wheel, removed VST GUI stays on screen).

Work split: this worktree (feature/look162) = themes + colour picker + mixer colour (items 1–2); feature/look162p =
plugin windows + presets (items 3–5, REA_rodada162d-plugins.md); feature/look162m = parameter menus + last tweaked
(item 6, REA_rodada162d-params.md).

## 1. Themes (Options › Theme)
- The colour tokens of src/ui/Theme.h were `inline const`; they are now plain variables set from a palette
  (core/ThemePalette.*: pure C++, unit-tested). `derivePalette (accent, background)` keeps the built-in relations:
  surfaces/hairlines/grids = the built-in step from bg1 applied to the new background (mirrored for a light
  background), the accent family (hover/pressed/muted/selection/step note/note colours) from the accent, and the text
  colours chosen for contrast (WCAG ratio; dark text on a light accent). With LYNX's red + background it returns the
  exact built-in palette (test theme162_default_palette_is_the_built_in_look) — the default look is unchanged.
- Live apply (ui/ThemeSettings.cpp): tokens → LookAndFeel::refreshColours() → colour properties that components set
  from the old tokens (`jcclr_*` component properties equal to an old token) are replaced by the new ones →
  window chrome (Windows title bar) → sendLookAndFeelChange + repaint of every desktop window.
- Theme file: <settings>/theme.lynxtheme (JSON: name, accent, background, workspace|auto, image, fit, imageOpacity,
  dim); presets: 6 built-in (LYNX default, Ocean, Emerald, Amber, Violet, Graphite) + the user's in <settings>/Themes;
  Import/Export .lynxtheme.
- Workspace backdrop (the area behind the panels, Workspace::paint): own colour or automatic, plus an image with
  Fill/Fit/Stretch/Tile/Center, opacity and dim; the scaled image is cached per area size (no rescale per repaint).
- Not themed on purpose: semantic colours (record, clip/meter red, LEDs, automation teal, warning) and the plugin
  editors' own family accents (skin126 per-plugin styles).

## 2. Colour picker + mixer "Change color"
- ui/ColourPicker.*: hue/saturation wheel + brightness strip + H/S/V sliders + hex + before/after preview; used by
  the Theme page, embedded in every "Rename and color" dialog (mixer tracks, channels, patterns, playlist tracks) and
  as "Custom color..." in every "Change color" submenu (shared builder ui::colourMenu).

- Root cause of "Change color does nothing" (#9): the swatch entries were juce::PopupMenu::Item built with the
  default constructor, whose itemID is 0; PopupMenu only triggers items with itemID != 0, so clicking a swatch just
  closed the menu ("Custom color..." used addItem and worked). Same bug in the channel rack, playlist track/clip and
  pattern menus. ui::colourMenu now builds items with Item(String) (itemID -1). Verified on Linux (Xvfb): Insert 9 turns
  green after picking the swatch.
- Also: LookAndFeel::preparePopupMenuWindow moves a submenu window that would open on top of a visible (multi-column)
  parent menu to its side, so the submenu no longer hides the parent's items.

## 3-6. Plugin windows, presets, parameter menu / Last tweaked
- See REA_rodada162d-plugins.md (removed plugin closes its window, Linux wrapper settings not covered, sandbox
  window hides with the tab, plugin presets) and REA_rodada162d-params.md (Last tweaked readout in the hint bar,
  gesture-less plugin parameters count as last tweaked).

## Where I stopped
- All six items implemented; forks look162p / look162m merged into feature/look162 with origin/main.
- Not done: parameter menu on controls without automation targets (sends, track EQ, stereo separation, master pitch,
  LYNX instrument pages, Patcher rows); live test with a gesture-less third-party plugin.
