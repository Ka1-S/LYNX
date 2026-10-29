# LYNX Polish (pl) glossary

Reference for `resources/i18n/pl.txt` (language header "Polski"). English is the source language;
the pt-BR table was only used as a hint for meaning.

## Register and style

- **Form of address: avoid direct address; where unavoidable, informal "ty" (lower case).** Polish
  producer communities, tutorials and most Polish-localized apps speak informally; formal "Pan/Pani"
  feels bureaucratic in a DAW. Most LYNX strings are commands or descriptions anyway:
  - buttons and menu commands use the **imperative** as in Polish Windows/macOS ("Zapisz", "Otwórz",
    "Anuluj", "Zmień nazwę", "Usuń kanał");
  - descriptions/tooltips use the **3rd person** ("Odtwarza bieżący pattern w pętli") or impersonal forms.
- **Concise labels.** Buttons, menu entries and column headers stay short; prefer the shorter of two natural
  options; no extra explanations. ALL-CAPS knob labels (FREQ, GAIN, ATTACK) stay in caps (translated only
  when the English is a plain word with a short Polish equivalent).
- **Capitalisation:** Polish sentence case (only the first word and proper names capitalised) instead of
  English Title Case.
- **Plurals:** Polish has three plural forms (1 takt / 2–4 takty / 5+ taktów) that a single "{0} bars"
  string cannot express. Count strings are rephrased with a colon ("Takty: {0}", "Znalezione nowe pluginy: {0}")
  or with an abbreviation, so they are correct for every number.
- **Quotes:** Polish typographic quotes „…” (no escaping needed); nested: «…».
- **Ellipsis:** keep "..." exactly as in English.
- **Numbers and units:** keep the spacing of the English source ("{0}%", "10 ms"); decimals written literally
  use a comma (0,5). Placeholders are formatted by the code and left alone.
- **Shortcuts and keys:** Polish keyboards print the English key names, so **Ctrl, Shift, Alt, Enter, Esc,
  Tab, Backspace, Delete, F1–F12** stay. In prose: Space → "Spacja", Arrows → "Strzałki". Key caps in brackets
  stay ASCII and English: `[Ctrl]`, `[Space]`, `[Up]`, `[Down]`, `[Left]`, `[Right]`, `[Esc]`, `[Enter]`.
  "Ctrl+drag" → "Ctrl+przeciągnij", "Alt+click" → "Alt+klik", "right-click" → "prawy klik" (short labels) /
  "kliknij prawym" (sentences), "double-click" → "dwuklik", "wheel" → "kółko".
- **Hint-bar markup tokens** `[LMB]`, `[2xLMB]`, `[RMB]`, `[MMB]`, `[Wheel]` stay exactly in English.
  Non-markup brackets such as "[double-click]" are translated as text ("[dwuklik]").
- **Lists separated by "|"** keep exactly the same number of items; preset/algorithm names stay in English,
  functional modes are translated.

## Stays in English (unchanged)

Channel Rack, Piano Roll, Playlist, Mixer, Browser (LYNX/FL proper names; Polish FL users use them in
English; inflection avoided where possible: "w Playliście" is acceptable, "w oknie Playlist" preferred in
formal sentences). Plugin, engine and product names (Sytrus, Harmor, Fruity …, LYNX Tune, Facet EQ, VFX,
FL Studio, VST/VST3/CLAP, ASIO, WASAPI, ALSA, JACK), file extensions, units (Hz, kHz, dB, ms, %, BPM, st, ct),
note names (C4, F#), MIDI terms (MIDI, CC, NRPN, MPE), Master, and the producer jargon marked below.

## Terms

| English | Polski | note |
|---|---|---|
| Channel Rack | Channel Rack | proper name |
| Piano Roll | Piano Roll | proper name |
| Playlist | Playlist | proper name (fem. "playlista" when inflected) |
| Mixer | Mixer | proper name |
| Browser | Browser | proper name |
| pattern | pattern | pl. patterny, gen. patternu (FL jargon) |
| Pattern mode / Song mode | tryb Pattern / tryb Song | buttons "Pattern" / "Song" stay |
| song | utwór | "Song" kept only as the mode button |
| clip | klip | audio clip = klip audio, automation clip = klip automatyki |
| track (Playlist) | ścieżka | |
| track (Mixer insert) | ścieżka miksera / insert | "insert" kept in FL contexts |
| channel | kanał | |
| bar | takt | |
| beat | miara | music-theory term; "beat" only in jargon sentences |
| step | krok | step sequencer = sekwencer krokowy |
| tick | tick | |
| note | nuta | |
| chord | akord | |
| scale | skala | |
| root (note) | pryma / tonika | "tonika" for scale root |
| key (musical) | tonacja | |
| key (keyboard) | klawisz | |
| octave | oktawa | |
| semitone | półton | |
| velocity | velocity | standard jargon |
| release velocity | release velocity | |
| pan / panning | panorama | knob label "Pan" may stay |
| volume | głośność | |
| gain | gain / wzmocnienie | knob label "Gain" |
| mute | wycisz (cmd) / wyciszenie | |
| solo | solo | |
| insert | insert | |
| send | send | |
| slot | slot | |
| preset | preset | pl. presety |
| sample | sample | pl. sample, gen. sampla |
| plugin | plugin | pl. pluginy |
| effect | efekt | |
| instrument | instrument | |
| synth | syntezator / synth | "Synth" as channel type label |
| sampler | sampler | |
| slice / slicer | plaster / slicer | "cięcie" for the action |
| automation | automatyka | |
| quantize | kwantyzuj / kwantyzacja | |
| snap | przyciąganie | "No snap" = Bez przyciągania |
| grid | siatka | |
| loop | pętla | |
| render | renderuj / renderowanie | |
| export / import | eksportuj / importuj | |
| stem | stem | pl. stemy |
| tempo | tempo | |
| swing | swing | |
| time signature | metrum | |
| pitch | wysokość (dźwięku) / pitch | knob label "Pitch" may stay |
| fine pitch | dostrojenie | |
| tune / tuning | strojenie | |
| envelope | obwiednia | |
| LFO | LFO | |
| oscillator | oscylator | |
| filter | filtr | |
| cutoff | cutoff / odcięcie | "Cutoff" as label |
| resonance | rezonans | |
| attack / hold / decay / sustain / release | Attack / Hold / Decay / Sustain / Release | standard jargon |
| wet / dry | wet / dry | jargon (sygnał przetworzony / czysty in prose) |
| mix | miks | |
| fade-in / fade-out | fade-in / fade-out | |
| crossfade | crossfade | |
| latency | latencja | |
| buffer | bufor | |
| sample rate | częstotliwość próbkowania | short field label: "Próbkowanie" |
| audio device | urządzenie audio | |
| input / output | wejście / wyjście | |
| MIDI controller | kontroler MIDI | |
| arpeggiator | arpeggiator | |
| portamento / glide / slide | portamento / glide / slide | |
| marker | znacznik | |
| arrangement | aranżacja | |
| freeze | zamroź / zamrożenie | |
| bypass | bypass / omiń | "Bypass" as label |
| sidechain | sidechain | |
| reverb / delay / chorus / compressor | pogłos / delay / chorus / kompresor | |
| limiter | limiter | |
| meter | miernik | |
| undo / redo | Cofnij / Ponów | |
| new / open / save / save as | Nowy / Otwórz / Zapisz / Zapisz jako | |
| settings / options | ustawienia / opcje | |
| OK / Cancel / Apply / Revert | OK / Anuluj / Zastosuj / Przywróć | |
| delete / remove | usuń | |
| rename | zmień nazwę | |
| clone / duplicate | klonuj / duplikuj | |
| select / select all | zaznacz / zaznacz wszystko | |
| cut / copy / paste | wytnij / kopiuj / wklej | |
| zoom | zoom / powiększ | |
| tool: Pencil / Brush / Eraser / Knife / Lasso / Picker / Slip | Ołówek / Pędzel / Gumka / Nóż / Lasso / Pipeta / Slip | |
| ghost notes | nuty-duchy | |
| template | szablon | |
| project | projekt | |
| autosave | autozapis | |
| scan (plugins) | skanuj | |
| drag / drop | przeciągnij / upuść | |
| click / double-click / right-click | klik / dwuklik / prawy klik | |
| wheel | kółko | |
| default | domyślny | |
| reset | resetuj | |
| bounce | bounce / zgraj | |
| take | take | recording take |
| browser favourites | ulubione | |
| arm / disarm (recording) | uzbrój / rozbrój | "Record arm" = uzbrojenie nagrywania |
| record safe | ochrona nagrywania | |
| crossover | zwrotnica | "X-over" knob labels stay |
| playhead | głowica | |
| ruler | linijka | |
| hint bar | pasek podpowiedzi | |
| toolbar | pasek narzędzi | |
| layout (workspace) | układ | |
| detach / dock | odłącz / zadokuj | |
| threshold / ratio / knee | próg / ratio / knee | ALL-CAPS labels (THRESHOLD, ATTACK...) stay English |
| double (vocal doubler voice) | dubla | |
| root note (sampler) | pryma | scale key = tonika skali |
| Layer children | dzieci | |
| plugin wrapper / bridged | wrapper pluginu / przez bridge | |
| audition / preview | odsłuch / podgląd | |
| starred | ulubione | |
| style/algorithm names (Clean, Punch, Vari-mu, Concert Hall...) | unchanged | preset-like names stay English |
