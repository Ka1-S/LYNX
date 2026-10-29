# LYNX German (de) glossary

Reference for `resources/i18n/de.txt` (language header "Deutsch"). English is the source language;
the pt-BR table was only used as a hint for meaning.

## Register and style

- **Form of address: formal "Sie", but avoid direct address wherever possible.** German music software
  (Cubase, Ableton Live, Bitwig, Native Instruments) uses "Sie" in its German UI and manuals. Most LYNX
  strings are commands or descriptions, so they are written impersonally: commands use the infinitive
  ("Projekt speichern", "Kanal umbenennen"), descriptions use the third person ("Spielt das aktuelle
  Pattern in einer Schleife"). "Sie" appears only where a sentence really talks to the user.
- **Concise labels.** Buttons, menu entries and column headers stay short; prefer the shorter of two
  natural options ("Löschen" not "Entfernen und löschen"), hyphenated compounds for English terms
  ("Pattern-Modus", "Mixer-Insert", "Plugin-Fenster"), no extra explanations.
- **Capitalisation:** normal German rules (nouns capitalised, everything else lower case) instead of
  English Title Case. ALL-CAPS knob labels (FREQ, GAIN, ATTACK) stay in caps.
- **Quotes:** German typographic quotes „…“ (no escaping needed). Single ‚…‘ for nested quotes.
- **Ellipsis:** keep "..." exactly as in English.
- **Numbers and units:** a space before units as in the English (10 ms, 440 Hz, -6 dB, 50 %→ keep the
  English spacing of the source, since "%" often follows a placeholder: "{0}%"). Decimal numbers written
  literally in a sentence use a comma (0,5); placeholders are formatted by the code and left alone.
- **Shortcuts:** key names stay as printed on German keyboards where they differ: Ctrl → **Strg**,
  Shift stays **Shift** (German keyboards print ⇧/"Umschalt", but DAW docs and FL users say Shift),
  Alt stays **Alt**, Space → **Leertaste**, Arrows → **Pfeiltasten**, Enter/Esc/Del → **Enter/Esc/Entf**,
  F1–F12 unchanged. "Ctrl+drag" → "Strg+Ziehen", "Alt-click" → "Alt+Klick", "right-click" → "Rechtsklick".
  (Strg is what every German keyboard and every German Windows app prints; keeping "Ctrl" would look foreign.)
- **Lists separated by "|"** (preset names, modes) keep exactly the same number of items. Algorithm/
  preset names (reverb types "Concert Hall", "Plate", metal names...) stay in English like plugin names;
  functional modes ("Up|Up + down", "ms|Note|Dotted|Triplet") are translated.

- **Hint-bar markup tokens** (src/core/HintMarkup.h): the mouse tokens `[LMB]`, `[2xLMB]`, `[RMB]`, `[MMB]`
  and `[Wheel]` are drawn as glyphs and must stay exactly in English. Other bracketed words become key caps
  only when they are 1–10 ASCII letters/digits/`+`/`-`: translated key caps are ASCII-only (`[Strg]`,
  `[Leertaste]`, `[Entf]`, `[Hoch]`, `[Runter]`, `[Links]`, `[Rechts]`); `[Shift]`, `[Alt]`, `[Esc]`, `[Enter]`,
  `[Tab]`, `[Backspace]` stay ("Rücktaste" has a non-ASCII letter). Non-markup brackets such as
  "[double-click]" are translated as text ("[Doppelklick]").

## Stays in English (unchanged)

Channel Rack, Piano Roll, Playlist, Mixer, Browser (LYNX/FL proper names, as in pt-BR; German FL users use
them in English). Plugin, engine and product names (Sytrus, Harmor, Fruity …, LYNX Tune, Facet EQ, VFX,
Soundgoodizer, FL Studio, VST/VST3/CLAP, ASIO, WASAPI, ALSA, JACK), file extensions, units (Hz, kHz, dB, ms, %,
BPM, st, ct), note names (C4, F#), MIDI terms (MIDI, CC, NRPN, MPE), and the producer jargon below.

## Terms

| English | Deutsch | note |
|---|---|---|
| Channel Rack | Channel Rack | proper name, neuter ("das Channel Rack") |
| Piano Roll | Piano Roll | proper name, feminine ("die Piano Roll") |
| Playlist | Playlist | proper name, feminine |
| Mixer | Mixer | proper name, masculine |
| Browser | Browser | proper name, masculine |
| pattern | Pattern | das Pattern, pl. die Patterns |
| Pattern mode / Song mode | Pattern-Modus / Song-Modus | "Pattern" / "Song" buttons stay as is |
| song | Song | |
| clip | Clip | der Clip; audio clip = Audioclip, pattern clip = Pattern-Clip, automation clip = Automationsclip |
| track (Playlist) | Spur | |
| channel | Kanal | pl. Kanäle |
| insert (mixer) | Insert | das Insert; "mixer insert" and "mixer track" = Mixer-Insert (not "Spur", which is the Playlist track) |
| send | Send | der Send |
| slot | Slot | effect slot = Effekt-Slot |
| strip (mixer) | Kanalzug | |
| master | Master | |
| bar | Takt | "{0} bars" = "{0} Takte" |
| beat | Schlag | Position "bar : beat : step" = "Takt : Schlag : Step" |
| step | Step | step sequencer = Step-Sequencer |
| note | Note | |
| velocity | Velocity | as in Cubase/Ableton German UI |
| release velocity | Release-Velocity | |
| pan | Pan | panning = Panorama |
| volume | Lautstärke | |
| level | Pegel | |
| mute / unmute | stummschalten / Stummschaltung aufheben | label "Mute" = "Stumm" |
| solo | Solo | verb: solo schalten |
| preset | Preset | das Preset |
| sample | Sample | das Sample; sample rate = Samplerate; "{0} samples" (latency) = "{0} Samples" |
| plugin | Plugin | das Plugin |
| effect | Effekt | |
| instrument | Instrument | |
| synth | Synth | der Synth |
| automation | Automation | |
| quantize | quantisieren / Quantisierung | |
| snap | Einrasten / am Raster ausrichten | "snap to grid" = "am Raster einrasten"; menu label "Snap" = "Raster" |
| grid | Raster | |
| loop | Loop | der Loop; "in a loop" = "in Schleife" |
| render | rendern | |
| export | exportieren | |
| stem | Stem | pl. Stems |
| tempo | Tempo | |
| swing | Swing | |
| groove | Groove | |
| humanize | humanisieren | |
| pitch | Tonhöhe | as a knob label "Pitch" may stay "Pitch" when it is an ALL-CAPS/abbreviated label |
| pitch bend | Pitchbend | |
| semitone / cent | Halbton / Cent | st / ct unchanged |
| octave | Oktave | |
| envelope | Hüllkurve | |
| LFO | LFO | |
| filter | Filter | der Filter |
| cutoff | Cutoff | |
| resonance | Resonanz | |
| attack / decay / sustain / release | Attack / Decay / Sustain / Release | |
| hold | Hold | |
| wet / dry | Wet / Dry | |
| mix (wet/dry) | Mix | |
| gain | Gain | |
| threshold / ratio / knee | Threshold / Ratio / Knee | as in Cubase German |
| make-up gain | Make-up-Gain | |
| compressor / limiter / gate / expander | Kompressor / Limiter / Gate / Expander | |
| sidechain | Sidechain | |
| reverb | Hall | plugin "Reverb" names stay |
| delay / echo | Delay / Echo | |
| feedback | Feedback | |
| distortion | Verzerrung | |
| drive | Drive | |
| saturation | Sättigung | |
| equalizer / EQ | Equalizer / EQ | band = Band (das Band, pl. Bänder) |
| high-pass / low-pass / band-pass | Hochpass / Tiefpass / Bandpass | |
| highs / lows / mids | Höhen / Tiefen / Mitten | |
| sibilance / plosives | Zischlaute / Plosivlaute | de-esser = De-Esser |
| oscillator | Oszillator | |
| waveform | Wellenform | |
| voice (synth) | Stimme | polyphony = Polyphonie, unison = Unisono |
| detune | Verstimmung | fine = Fein, coarse = Grob |
| glide / portamento | Glide / Portamento | |
| legato | Legato | |
| arpeggiator / arpeggio | Arpeggiator / Arpeggio | |
| chord | Akkord | |
| strum | Strum | |
| scale | Skala | as in Ableton Live German |
| key (musical) | Tonart | |
| key (keyboard) | Taste | PC keyboard = PC-Tastatur |
| root note | Grundton | |
| latency | Latenz | delay compensation = Latenzausgleich |
| buffer | Puffer | buffer size = Puffergröße |
| audio device | Audiogerät | sound card = Soundkarte; driver = Treiber |
| input / output | Eingang / Ausgang | |
| MIDI controller | MIDI-Controller | |
| CC | CC | |
| knob | Regler | |
| fader | Fader | |
| slider | Schieberegler | |
| wheel (mouse) | Mausrad | "wheel" alone = Mausrad |
| click / double-click / right-click | Klick / Doppelklick / Rechtsklick | verbs: klicken, doppelklicken |
| drag | ziehen | as a key combo: "Alt+Ziehen" |
| ruler | Lineal | time ruler |
| playhead | Abspielmarke | |
| marker | Marker | |
| time signature | Taktart | |
| count-in | Einzähler | |
| metronome | Metronom | |
| record / recording | aufnehmen / Aufnahme | |
| arm / disarm / record safe | aufnahmebereit schalten (machen) / Aufnahmebereitschaft aufheben / Aufnahmeschutz | not "scharf schalten" |
| overdub / replace | Overdub / Ersetzen | |
| take | Take | |
| slice / slicer | Slice / Slicer | |
| stretch / time-stretch | Stretching / Time-Stretching | "stretch" verb = dehnen |
| fade in / fade out / crossfade | Einblenden / Ausblenden / Crossfade | noun Fade-in / Fade-out |
| reverse | umkehren / rückwärts | |
| normalize | normalisieren | |
| freeze | einfrieren | |
| consolidate / bounce | zusammenfügen / bouncen | |
| ghost notes | Ghost-Notes | |
| undo / redo | Rückgängig / Wiederholen | undo history = Rückgängig-Verlauf |
| new / open / save / save as | Neu / Öffnen / Speichern / Speichern unter | |
| open recent | Zuletzt geöffnet | |
| template | Vorlage | |
| autosave | automatische Sicherung | autosave folder = Ordner der automatischen Sicherung |
| settings / options / preferences | Einstellungen / Optionen / Einstellungen | |
| OK / cancel / apply / close | OK / Abbrechen / Anwenden / Schließen | |
| accept | Übernehmen | |
| discard | Verwerfen | |
| delete / remove | Löschen / Entfernen | |
| rename | Umbenennen | |
| clone / duplicate | Klonen / Duplizieren | |
| cut / copy / paste | Ausschneiden / Kopieren / Einfügen | |
| select / select all / deselect | Auswählen / Alles auswählen / Auswahl aufheben | |
| selection | Auswahl | |
| split | Teilen | |
| make unique | Eigenständig machen | |
| zoom | Zoom / zoomen | |
| reset | Zurücksetzen | the bare "Reset" button/menu label stays "Reset" (fits small buttons); phrases use "zurücksetzen" |
| bypass | Bypass / umgehen | |
| enable / disable | aktivieren / deaktivieren | on/off = an/aus |
| show / hide | Einblenden / Ausblenden (panels), Anzeigen | |
| scan for plugins | Nach Plugins suchen | |
| folder / file | Ordner / Datei | file manager = Dateimanager |
| File / Edit / View / Options / Help (menus) | Datei / Bearbeiten / Ansicht / Optionen / Hilfe | the "Tools" menu stays "Tools" (the menu bar must stay narrow for the toolbar) |
| Exit | Beenden | |
| language | Sprache | |
| shortcuts | Tastenkürzel | |
| tooltip / hint bar | Tooltip / Hinweisleiste | |
| default | Standard | |
| color | Farbe | |
| length | Länge | |
| position | Position | |
| arrangement | Arrangement | |
| routing | Routing | |
| group | Gruppe | |
| kick / snare / hat / clap | Kick / Snare / Hi-Hat / Clap | drum names stay |
| meter | Pegelanzeige | |
| peak | Peak | |
| spectrum analyzer | Spektrumanalysator | |
| frequency | Frequenz | |
| transient | Transiente | |
| warning / error | Warnung / Fehler | |
| toolbar | Werkzeugleiste | |
| panel / window | Panel / Fenster | |
| picker (tool) | Auswahl | |
| quick-add palette | Schnellauswahl | |
| preview / audition | Vorhören | "Preview" (visual) = Vorschau |
| built-in | integriert | |
| setup wizard | Einrichtungsassistent | |
| backup | Sicherung | |
| tail (reverb / export) | Hallfahne / Ausklang | |
| chop | zerhacken | |
| chord progression | Akkordfolge | |
| speed / rate / amount (knobs) | Rate / Rate / Stärke | "Speed" as a sentence noun = Geschwindigkeit |
| cut group | Cut-Gruppe | |
| node / handle | Knoten / Griff | |
| control (UI element) | Bedienelement | |
| slope | Flanke | filter/shelf slope; "Shelf slope" = Shelf-Steilheit |
| take / comp track | Take / Comp-Spur | |
| starred | Favoriten | |
