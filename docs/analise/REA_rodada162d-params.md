# Round 162d (params) — right-click parameter menu on every knob + "last tweaked" read-out (issue #8)

Issue #8 comment (alltern, Linux): "make it so you can right click knobs and parameters to open up a context menu
about them so you can automate them easier. Also on the toolbar add a way to see the last tweaked knob or parameter
and open the context menu from there in case the plugin has issues."

Evidence markers: **[CODE]** read in LYNX's sources, **[EXEC]** seen in the running app (Linux build of this branch on
Xvfb, screenshots in the session scratchpad, not in the repo), **[TEST]** core unit test.

## 1. Audit — what LYNX already had **[CODE]**

LYNX's parameter menu (ui/ValueControl `showValueMenu` / `addValueItems` + ui/AutomationUi `addAutomationItem`,
rounds 143–149) already has FL's order: Reset, *Automation* (Edit events in the pattern, Edit events in piano roll,
Init song with this position, Delete initial value, Create automation clip, Edit automation clip), *Remote control*
(Link to MIDI controller, Override global link, Link to internal controller, Unlink), *Value* (Copy, Paste, Type in).

Every control that has an engine automation target already opens it on right-click:

| Where | Controls | Target |
|---|---|---|
| Channel rack | volume, pan knobs | channelVolume / channelPan |
| Channel settings frame | VOL, PAN | channelVolume / channelPan |
| Mixer strips | fader, pan | mixerVolume / mixerPan |
| Mixer effects panel | every slot's mix knob, the selected effect's knobs | effectMix / effectParam |
| Toolbar | master volume, tempo | mixerVolume 0 / tempo |
| LYNX effect editors | every knob, slider, switch of the generic native editor (PluginEditors.cpp, all FL-style and LYNX engines), Pro editors (ProEditorBase + ProEditors.cpp), Facet EQ, Formula / Peak controller knobs | effectParam |
| LYNX Soundfont, Slayer | knobs | channelParam |
| Hosted plugins (VST2/VST3/CLAP/LV2) | their own GUI cannot get LYNX's menu: Tools › Last tweaked / Add › Automation for last tweaked parameter, and the channel / mixer slot menu’s "Automate plugin parameter" list (every parameter with its full menu) | channelParam / effectParam |

Plain `juce::Slider`s get the slider contract (Reset, Copy/Paste, Type in value) but no automation items; none of
them is an automatable target today (mixer track EQ, sampler/synth pages of LYNX's own instruments, Patcher node
rows, metronome volume).

## 2. What this round adds

1. **Last-tweaked read-out in the toolbar** **[EXEC]**: at the right end of the hint bar, once any automatable
   control was moved: an automation glyph, the parameter's name ("Kick - Volume") and its value in its own unit
   ("100%", "-3.2 dB", "L50", "128.00 BPM", a LYNX effect's own unit, a plugin parameter's own text + label). The
   value pill flashes in the accent colour for a moment when it changes. Click = the parameter's full menu (the same
   menu as Tools › Last tweaked; its header now also shows the value). It only appears when the hint keeps 240 px.
   Colours read from theme tokens at paint time (works with r162d's runtime themes).
2. **Plugin knobs without gestures count as tweaks** **[TEST]**: many plugins move parameters without begin/end
   gestures, so their knobs never became "last tweaked". The plugin window now collects parameter changes made on
   the UI thread and, 10× a second, reports the single parameter that moved while the window is active or under the
   mouse (core/LastTweaked.h `ChangeFilter`: automation and controller links run on the audio thread and are
   ignored; two or more parameters in one tick = a preset load / randomise, ignored; parameters inside a gesture are
   left to the gesture). Tests: `look162_plugin_change_filter`, `look162_lasttweak_value_text`.
3. pt-BR strings for the new texts (resources/i18n/pt-BR.txt, I18nData.h regenerated).

## 3. Not done (and why)

- **Automation of controls without an engine target kind**: mixer sends, mixer track EQ, stereo separation, master
  pitch, the sampler / LYNX synth pages (3x Osc, Sytrus… knobs), Patcher node rows. They keep Reset / Copy / Paste /
  Type value; making them automatable needs new AutomationTargetKinds in core (Song::targetValue/setTargetValue,
  RackRenderer, FL import) — a separate round.
- **Sandboxed plugins** ("run plugins in a separate process"): their window lives in the child process, so the
  gesture-less detection can't tell whether the user is over it; their gestures still report as before.
- Not verified live with a gesture-less third-party plugin (none installed in the Linux test set); the filter logic is
  unit-tested and the gestured path is unchanged.

## Where I stopped

All of the above committed on feature/look162m (wip pushed to origin/wip/look162m). Core tests + Linux GUI build
done; Windows build run. Not landed (the parent round look162 merges and lands).
