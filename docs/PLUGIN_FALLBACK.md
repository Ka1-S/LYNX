# Third-party plugins in projects: loading, and LYNX SHARE substitutes

What LYNX SHARE does with the third-party plugins (VST2, VST3, CLAP) a project uses, whether the project is an FL Studio
`.flp` or a LYNX SHARE `.lynx`.

## 1. The plugin is installed: it loads with its saved settings

LYNX SHARE looks for the same plugin among the ones it has scanned, in this order (`PluginHost::findForeign`):

1. the same identifier: VST3 class ID, VST2 unique ID, CLAP id (an entry of the plugin list whose file is gone — an
   uninstalled older version next to the current one — is skipped, round 113);
2. the file FL Studio's plugin database lists for it on this PC, or the very file the project saved;
3. the same name (plugin or file name) in the same role, preferring the format the project used — so a project saved
   with a VST2 opens with the VST3 of the same product, and vice versa.

The saved state is carried over: VST3 (FL's component/controller records → JUCE's format), VST2 (FL's own state
layout, REA round 89), CLAP (FL's `u32 0` + the plugin's state). Between VST2 and VST3 the states differ, so a
cross-format match plays with the plugin's default settings (the Missing files window says so).

**Waves**: a Waves project plugin is a WaveShell file holding hundreds of plugins. LYNX SHARE finds the sub-plugin by its
VST3 class ID; failing that, by the `<PluginName>` its saved state carries ("RComp" → "RComp Stereo"), never by the
shell's file name (nor by the file FL's plugin database lists for it: that file holds the whole bundle).

A plugin file the scan could not describe is listed in `%APPDATA%\LYNX\scan-failures.log` with the reason (timeout
before / while describing, exit code). Since round 113 the scan child ends right after writing its answer (a Waves
shell's shutdown could hang and get the whole bundle marked as hung), a complete answer is kept even when the child had
to be stopped, and files marked as failed by earlier versions get one more try.

## 2. The plugin is NOT installed: a LYNX SHARE substitute plays in its place

With **Settings › General › Plugins › "Substitute missing plugins with LYNX SHARE equivalents"** on (the default), an
effect slot whose plugin is missing gets the LYNX SHARE effect that plays the same role — a reverb for a reverb, a delay
for a delay — instead of being dropped:

- the slot shows the original plugin's name with a **LYNX SHARE** badge; selecting it shows "LYNX SHARE substitute for X";
- the **Missing files** window lists the plugin as "LYNX SHARE substitute: <effect> (the original plugin is used again once
  it is installed)";
- when the saved state can be read, its settings are translated (e.g. ValhallaVintageVerb's Mix becomes the slot's
  dry/wet); otherwise the substitute starts from a sensible default preset;
- automation of the original plugin's parameters is kept but does not drive the substitute (its parameters mean
  something else); the slot's on/off, dry/wet and side-chain routing are kept.

**The original is never lost.** The slot keeps the original reference byte for byte (`EffectSlot::substituteFor`):
an `.flp` plugin keeps its whole FL wrapper record (format, IDs, name, path, state, MIDI ports), a LYNX SHARE plugin keeps
its identifier and state. Saving as `.lynx` writes it next to the substitute; opening that project on a machine that
has the plugin puts the original back with its saved settings. The `.flp` file itself is never written.

**A/B**: right-click a substitute slot › **Bypass all LYNX SHARE substitutes of this project** switches every substitute of
the project off at once (the project's own effects and the hosted plugins keep playing); the slots show "(bypassed)".
The switch is saved with the project (`substitutesBypassed` in `.lynx`).

**The project's key** (round 113): a pitch corrector whose own key can't be read (Waves Tune, an unreadable state)
corrects to the key and scale the project's other pitch correctors were saved with; when nothing in the project names
a key, the substitute corrects gently (chromatic, 50 ms, humanize 50 %) rather than forcing a key.

Instruments are not substituted yet (a missing instrument plugin still becomes a built-in synth chosen by the channel
name, as before).

## 3. New projects: the LYNX SHARE equivalents are always available

The Quick add palette lists every equivalent as "<LYNX SHARE effect> (LYNX SHARE equivalent of <product>)", so typing the
product's name finds it; the mixer's add menu has a **LYNX SHARE equivalents** sub-menu. Both add the built-in effect with
the same default preset a substitute gets — no third-party plugin involved.

## The substitutes table

The mapping is data: a text table built into `src/core/PluginSubstitutes.cpp` (`builtInTable`), extendable at run
time with `registerPluginSubstitutes (text)` (later registrations of the same key win). Format:

```
# comment
[valhalla-vintageverb]                  # stable key
product = ValhallaVintageVerb           # shown to the user
vendor = Valhalla DSP
role = effect                           # effect (default) | instrument (reserved: not applied yet)
lynx = Reeverb 2                        # the LYNX effect, by its name in the effect list
vst3 = 7654535665657633616c68616c6c6176 # VST3 class IDs (32 hex digits, as FL stores them), ';'-separated
vst2 = StEB; 0x5374506e                 # VST2 unique IDs: four characters or a number
clap = com.vendor.plugin                # CLAP ids
names = ValhallaVintageVerb; Vintage Verb   # plugin / file names (case, spaces, punctuation, x64, Stereo/Mono ignored)
waves = RComp; CLA-2A                   # WaveShell sub-plugins, by the <PluginName> in their state
preset = Dry=0; Wet=0.8                 # LYNX parameter (by name) = normalised value 0..1
mix = 0.25                              # the substitute's slot dry/wet
vst2params = 12                         # VST2 parameters per program (to read "all programs" states)
map = xml:Mix -> mix                    # translation: source -> target [| inLo inHi outLo outHi]
map = xml:ReverbMode -> Mode | steps 0:0 0.0828:0.0476   # enumerations: breakpoints (round 100)
map = xml:decay -> Decay | curve 0:0 0.019:0.5 1:1      # other laws: piecewise-linear points
translate = pro-c3                      # a built-in state translator: pro-c3, pro-mb, pro-g, saturn2 (round 108),
                                        # auto-tune-pro, fresh-air (round 113)
```

Map sources: `vst2:<n>` (float parameter n of the current program in FL's VST2 state), `xml:<attribute>` (an
attribute of the first element of a JUCE XML state, found after `VC2!` or `<?xml`), `waves:<n>` (value n of the
active setup's "RealWorld" list in a Waves preset chunk). Targets: `mix` (lower case: the slot's dry/wet) or a parameter name of
the LYNX SHARE effect; the optional range maps the source linearly (clamped) onto the normalised parameter. Instead of the
range, `| curve x:y x:y …` maps through piecewise-linear points (clamped at the ends) and `| steps x:y …` gives the y
of the last point whose x ≤ the value (enumerations); x must ascend.
`translate` names a built-in translator (src/core/FabFilterStates.cpp) for states whose layout was learned from our own
black-box observation (round 108: the FabFilter component states, "FFBS" / "FabF" + one float per VST3 parameter);
it runs after the maps and only when the state's header and parameter count match the observed layout.

Matching order: VST2 ID, VST3 class ID, CLAP id, Waves sub-plugin name, then names (a WaveShell's own name never
matches by name).

### Rules for adding entries

- Parameter maps only from what the saved data itself shows (attribute names, value lists) plus meanings from the
  vendor's public documentation or our own black-box measurements (render through a legally licensed copy with test
  signals). Never from decompiling or disassembling third-party binaries. Each map says where its meaning comes from.
- Unknown meaning → no map (the default preset). Mark unconfirmed readings as such in the table comment.
- New LYNX SHARE modules that play a third-party role (pitch correction, de-esser, noise suppressor, doubler, exciter…)
  add their entry to `builtInTable` (or call `registerPluginSubstitutes` at start-up) when they land.

### Seed (round 98)

| Third-party product | LYNX SHARE substitute | Identified by | Translated |
|---|---|---|---|
| ValhallaVintageVerb | Epoch Verb (round 100) | VST3 class ID [DATA], name | every parameter (measured laws, mode / colour breakpoints) |
| ValhallaRoom | Atrium (round 100) | name | every parameter (decay through a curve onto Atrium's law) |
| ValhallaPlate | Foil Plate (round 100) | name | every parameter |
| Valhalla Supermassive | Nebula (round 100) | name | every parameter except Clear (a button) |
| ValhallaShimmer | Halo (round 100) | name | every parameter (bare `parameterN` attributes; mode lists partly not confirmed) |
| ValhallaFutureVerb | Horizon (round 100) | name | every parameter |
| Soundtoys Little Plate | Foil Plate (round 129; 2 s, 100 % wet, slot dry/wet 25 %; was Reeverb 2) | name | — |
| Waves RVerb / H-Reverb / TrueVerb | Reeverb 2 (100 % wet, slot dry/wet 25 %) | Waves name | — |
| FabFilter Pro-R | LuxeVerb (default) | name | — |
| Waves H-Delay | Echoform (round 119; was Fruity Delay 3) | VST3 class ID [DATA], Waves name | sync (host / BPM / ms), note, time, feedback, ping-pong (pairs) / stereo, mix, output, filters; laws measured on the official trial [EXEC] (round 119 §2) |
| Waves NS1 | Quell Denoiser (round 119) | Waves name | the one knob (RealWorld 229), Quell fitted to the official trial [EXEC] (round 119 §3) |
| Waves Doubler (2 / 4 voices) | Gemini Doubler (round 119) | Waves name | every control of voices 1–2, direct, align, shelves, feedback cut, range [EXEC] (round 119 §4) |
| Soundtoys EchoBoy / PrimalTap | Echoform (round 129; tape / lo-fi echo, 1/4 note, 25 % feedback, 25 % mix; was Fruity Delay 3) | VST2 ID [DATA] (EchoBoy), names | — |
| Waves SuperTap, FabFilter Timeless | Fruity Delay 3 | names | — |
| ValhallaDelay | Echoform (round 101) | VST3 class ID [derived], name | every setting but Era / taps / repeat-swell: mix, style, sync, times, feedback, filters, modulation, shift, pitch, ducking; the 17 modes onto Echoform's 12 characters |
| ValhallaFreqEcho | Spiral Echo (round 101) | VST3 class ID [derived], name | all 8 settings |
| ValhallaSpaceModulator | Orbit (round 101) | VST3 class ID [derived], name | all 6 settings |
| ValhallaUberMod | Lattice Mod (round 101) | VST3 class ID [derived], name | all but DelaySync and InputPan (meaning not confirmed) |
| Waves L1/L2/L3/L4 | Limiter | Waves name | — |
| FabFilter Pro-L | Apex Limiter (round 99) | names | — |
| Waves CLA-76 | Crest Compressor (Bus style fitted to the official trial; round 127) | Waves name | Input, Output, Attack, Release, Ratio (59), Mix (103 → slot dry / wet), fitted at her state [EXEC] (round 127 §3) |
| Waves RComp, CLA-3A, C1, SSLComp, API-2500 | Compressor (2:1 above −15 dBFS, auto gain; round 113) | VST3 class ID [DATA], Waves names | RComp: threshold, ratio, attack, release (ARC), gain, laws measured on the official trial [EXEC] (round 113 §3c) |
| FabFilter Pro-C (2 / 3) | Crest Compressor (round 108) | names | Pro-C 3: style, threshold, ratio, knee, range, attack, release, hold, look-ahead, character, side-chain EQ (3 bands), levels, mix, oversampling [EXEC layout] |
| FabFilter Pro-MB | Prism Multiband (round 108) | names | enabled bands laid out on Prism's four, threshold, ratio, range, mode, knee, attack / release, level, solo, look-ahead, mix [EXEC layout] |
| FabFilter Pro-G | Sentry Gate (round 108) | names | every control [EXEC layout] |
| Waves CLA-2A | Glow Leveler (peak reduction 30 %, gain +2 dB; round 113) | VST3 class ID [DATA], Waves name | gain, peak reduction, comp / limit, laws measured on the official trial [EXEC] (round 113 §3c) |
| Waves SSL E-Channel, SSL Native Channel Strip / 4K E / 4K B / Vocalstrip | Ridge Strip (neutral; round 96) | VST3 class ID [DATA], Waves name, names | SSL E-Channel: filters, 4-band EQ, compressor, output, band laws measured on the official trial [EXEC] (round 113 §3c) |
| Waves Q10, REQ, SSLEQ | Parametric EQ (flat) | Waves names | — |
| FabFilter Pro-Q | Facet EQ (flat, round 99) | names | — |
| Waves DeEsser | Hush De-esser (5.5 kHz, −20 dBFS, 4 dB; round 113) | Waves name | frequency, threshold, split / wideband, side-chain filter, monitor, laws measured on the official trial [EXEC] (round 113 §3c) |
| FabFilter Pro-DS, Harrison De-Esser | Hush De-esser (default; round 96) | names | — |
| Slate Digital Fresh Air | Aether (neutral without a state; round 96) | name | Mid / High Air → Aether's air shelf gain and frequency, laws measured on the official trial (within 0.5 dB RMS for her settings) [EXEC] (round 113) |
| Soundtoys Decapitator, FabFilter Saturn | Ember Saturator (drive +1.4 dB, auto gain; round 113) | VST2 ID `StDc`, names | Saturn 2: multiband mode — bands, crossovers, style family, drive, mix, level (round 108) [EXEC layout]; Decapitator: her states (round 135, read black-box in the official trial: payload hash → voicing, drive, punish, tone, wide cuts, thump / steep, mix, trim) |
| Soundtoys PanMan | Auto-pan, panner sources (round 135: a 3.56 Hz soft LFO, 150° wide, analog — her usual setting) | VST2 ID [DATA], name | her states (round 135, read black-box in the official trial: source, rate, steps, width, smoothing, gains, analog) |

| Antares Auto-Tune (Pro / Artist / Access / EFX+ / Hybrid) | LYNX Tune | VST3 class ID [DATA] (Auto-Tune Pro), names | Auto-Tune Pro: key and scale (`<Custom ScaleGivenKey / ScaleGivenScale>` [DATA]; scale list 0 major, 1 minor, 2 chromatic [model, round 113 §2]) retune, humanize, flex-tune (round 120), detune, transpose, mix, input type, formant / throat, Classic, tracking (round 127); Auto-Tune EFX+: key, scale and note switches, retune, humanize, pitch shift, formant / throat, input type, mix (round 127); otherwise the project's key or a gentle chromatic correction |
| Waves Tune Real-Time | LYNX Tune | Waves name, names | — (the project's key, else gentle chromatic) |

Round 135: **Soundtoys Little AlterBoy → Voice Shift** (VST2 ID `StLa`, names; her states translated — read black-box in
the official trial; others: Quantize, formant +1.2 st, drive 4.6).

Not mapped yet (no LYNX SHARE module plays their role): Filterverse, Kickstart,
Sausage Fattener, Effectrix, Analog Lab (instrument). The mixing suite of round 96 (Ridge Strip, Glow Leveler, Hush
De-esser, Aether, Ember Saturator) took over the channel strip, CLA-2A, de-esser, air and saturator roles.

[DATA] = read from FL wrapper records of real projects (identifiers and state layouts only; no plugin code was read).
[derived] = Steinberg's VST2-compatible VST3 UID ("VST" + the VST2 ID read from the plugin's own state header + the first nine letters of
its lower-case name, in FL's byte order); the rule reproduces ValhallaVintageVerb's [DATA] ID exactly.
Waves states hold readable "RealWorld" parameter lists; for RComp, DeEsser, CLA-2A and SSL E-Channel which value is
which control was measured on the official trials (black box, round 113 §3c) and they are translated; the other
Waves plugins' lists are not. Unreadable settings fall back to
gentle presets (round 113: chosen by rendering her vocal takes, docs/analise/REA_rodada113.md §3). Soundtoys states are encoded (not translated).
