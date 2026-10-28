# Round 142 — issue #5: FL-only engines usable in native LYNX projects

Issue #5 (alltern): "Allow the plugins that only exist from flps to be used outside of FLP's or in native projects.
Like allow the use of Newtone outside of FLP projects." LYNX reproduces many FL engines (rounds 28–73), but until
now most of them only appeared when an `.flp` was imported: they were not in the mixer's Add menu, the quick-add
palette, the Patcher or the Channel Rack's "+" menu, and a fresh instance had no usable state.

Evidence markers: **[TEST]** LYNX's tests (`tests/Natives142Tests.inc`, build-local with the FL Data dir),
**[EXEC]** LYNX.exe `--selftest` / `--snapshot` on Windows with FL Studio 2026 installed, **[DATA]** the files of the
user's FL installation (read at run time, never copied into LYNX), **[HYP]** inference.

---

## 1. How a fresh FL engine gets its state (`core/NativeEngines.h`)

Most of these engines run on the plugin's own saved state (`ChannelDef::pluginState`, `EffectSlot::state`) and many
read data files of the FL installation (DLL tables, wave folders, impulses). A new instance therefore starts from the
plugin's **Default preset of the user's own FL installation**
(`<Data>\Patches\Plugin presets\<Generators|Effects>\<plugin>\Default.fst`), imported exactly like an `.flp` channel
or mixer slot (`importFlp` / `effectFromFl` with the installation's data reader). Nothing of FL is stored in LYNX —
the same contract as an imported project.

- Some engines don't read every version of their plugin's state: Harmor's and Ogun's `Default.fst` are such
  states (88 of 325 Harmor presets and 95 of 229 Ogun presets fall back to the conversion) **[DATA][TEST]**. The
  candidates are therefore `Default.*` first, then the folder's presets in order; the first one the exact engine
  reads is used (at most 60 tries).
- Presets that are the plugin's raw file (GMS `.gmsynth`, Morphine `.mrp`, Toxic Biohazard `.tbio`) are wrapped as
  FL saves them: `"SSLF"` + size + the file **[DATA]** (the same wrapper the browser's GMS preset conversion used).
- BooBass has no preset folder: a new one is its three knobs at their default 0.5 (REA_rodada29 §4) over the
  multisample of the user's `BooBass_Eng_x64.dll`.
- FL Keys: FL 2026 ships no `Piano.wti`, which every FL Keys preset names. The folder's `default.wti` says
  `[default] file=mda piano.wti`; LYNX now loads that wavetable when the named one is missing (also fixes `.flp`
  imports of FL Keys on FL 2026) **[DATA][TEST]**.
- Engines that play from an empty state need no FL: 3x Osc, Harmless, SimSynth, Fruity DX10, Plucked!, and the
  effects Pitch Shifter, Vintage Phaser, Fruity Squeeze, the rhythmic envelope, Newtone and Newtime. With FL
  installed they still start from FL's Default preset.
- Without FL (or without the needed data files) an engine is shown **disabled with "(requires FL Studio)"** in the
  menus and is left out of the quick-add palette and the Browser's Plugin database.
- Partly reproduced engines carry **"(beta)"**: Harmor, Kepler, Fruity Granulizer, Newtone, Newtime.

| Engine | New instance on this PC | Sound of a note / signal | Notes |
|---|---|---|---|
| 3x Osc, Sytrus, Harmless, PoiZone, SimSynth, Fruity DX10, Plucked!, Fruit Kick, Drumpad | Default.fst | ✅ | |
| Harmor (beta), Ogun | Harmor: Default.fst (old states upgraded, REA_rodada165g-flbeta165s); Ogun: first preset the engine reads | ✅ | |
| GMS, Morphine | first `.gmsynth` / `.mrp` | ✅ | no Default preset in FL |
| Toxic Biohazard | `Default.tbio` + the DLL's spectra | ✅ | |
| Kepler (beta), Transistor Bass, Sawer, MiniSynth, FL Slayer, BassDrum, BooBass | Default + the FL data file | ✅ | |
| Sakura, BeepMap, Drumaxx | Default.fst | ✅ | (Drumaxx: pads on the drum keys) |
| FL Keys | Default.fst + `default.wti` fallback | ✅ (zones from the installation) | |
| Fruity Granulizer (beta) | Default.fst | silent until a sample is given | drop an audio file on its row; it keeps its engine |
| Maximus, Vocodex, EQUO, Distructor, Love Philter, Gross Beat, Fruity Convolver | Default.fst (+ impulses / install data) | ✅ | |
| Pitch Shifter, Vintage Phaser, Fruity Squeeze, rhythmic envelope | Default.fst / LYNX defaults | ✅ | |
| Newtone (beta), Newtime (beta) | LYNX's own project (§3) | ✅ | |

All 26 instruments and 13 effects are made, survive `.lynx` save / load and the instruments play through the rack
(`native_engines=OK (39 made)` in the selftest) **[EXEC][TEST]**.

Not offered: Fruity DrumSynth Live (FL's Default/Extra presets are version-1 24-patch kits, which the engine does
not read), Wasp / Wasp XT and Fruity Dance (FL 2026's placeholders make no sound), Autogun and DirectWave (no
preset to start from; DirectWave as a plain sampler is LYNX's Sampler). They still import from `.flp`.

## 2. Where they appear

- **Mixer** slot menu (Add / Replace with), **quick-add palette** (effects and instruments), **Patcher** editor
  (Add effect; Add instrument › FL engines), **Channel Rack** "+" menu › FL engines (the channel's editor opens),
  and a new **Browser › Plugin database** (FL's, interpreted): Generators and Effects, each with LYNX, FL engines and
  Installed (the scanned VST3 / CLAP plugins); double-click adds it (a generator = new channel, an effect = into the
  selected insert); the folders remember being open.
- The native-editor fuzz of the selftest now opens the FL effects with their fresh state, as the menus make them.

## 3. Newtone and Newtime in a native project (`core/NewtoneFx.h`, `ui/NewtoneEditor.cpp`)

FL's Newtone / Newtime are editors around zplane's engines; LYNX only played back an imported Newtone's edits
(REA_rodada73/95). For native slots LYNX now has its own project format and editors:

- **Newtone**: `analyseNotes` (YIN pitch track, frames below −34 dB of the loudest ignored, a new note when the
  smoothed pitch moves > 0.7 semitone for 3 frames, notes ≥ 60 ms, median pitch) → notes; the edits are rendered by
  LYNX's Newtone engine (TD-PSOLA, round 95). State `"LXNT"` v1: frames, global formant, the notes' fields.
  Editor: notes on a pitch grid with the sung pitch inside them; drag = pitch (snapped to semitones; Alt free),
  Ctrl+drag = time, double-click = snap, right-click = snap / reset / mute / volume; Up/Down move the selected one,
  Delete mutes it; "Snap all to notes", "Reset edits", "Analyse", Preview, "Save as WAV...", "Play with the song"
  (the plugin's slave mode). An imported FL Newtone opens with its notes; editing saves it in LYNX's format.
- **Newtime**: warp markers (source → output sample); between markers the source is time-stretched with pitch kept
  (`timeStretch`, WSOLA); after the last marker its slope goes on; the first marker added also pins the end, so a
  marker drag stretches one side and squeezes the other. State `"LXNW"` v1. Editor: the warped waveform with the
  song's beats; double-click adds a marker, drag moves it, right-click removes it; "Length..." = 50–200 % or 1–16
  bars at the song's tempo.
- A synthetic sung line (A3 B3 C4 · E4) analyses to 4 notes within 0.1 semitone; moving B3 +2 renders C#4
  (60.96); unedited notes return the source unchanged; markers map both ways exactly **[TEST]**.
- Snapshots: `--snapshot` writes `-newtone.png`, `-newtime.png`, `-browser-plugins.png` **[EXEC]**.

## 4. Tests

`tests/Natives142Tests.inc`: lists and no-FL behaviour; FL Keys `default.wti`; every engine from the real FL
installation (made, right kind, renders > −60 dBFS RMS, effects finite and not silent, state-based ones carry FL's
state); Newtone / Newtime native projects. Selftest: `native_engines`.

## Where I stopped

- Newtone: no formant / vibrato / drift controls in the editor yet (the engine and state support them), no note
  split / merge, no multi-select. Newtime: no transient detection for automatic markers.
- No drag-and-drop from the Plugin database onto the Channel Rack / Mixer (double-click and the menu add).
- Engines without a preset to start from (DrumSynth Live v1 kits, Autogun, DirectWave) are not offered.
- On Linux / macOS (no FL installation) only the engines with LYNX defaults are offered.
