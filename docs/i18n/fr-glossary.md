# LYNX French (fr) glossary

Reference for `resources/i18n/fr.txt` (language header "Français", countries fr be ch ca lu). English is the
source language; the pt-BR table was only used as a hint for meaning.

## Register and style

- **Neutral French** usable in France, Belgium, Switzerland and Québec: no regionalisms, no anglicisms that
  Québec rejects when a common French word exists (fichier, dossier, courriel not needed), but producer jargon
  that every French-speaking producer uses stays (pattern, preset, sample, plugin, swing, send...).
- **Form of address: "vous"**, as in every French DAW (Ableton Live, Cubase, Logic, Bitwig French UIs).
  Most strings avoid direct address: commands use the **infinitive** ("Enregistrer le projet",
  "Renommer le canal"), descriptions use the third person ("Joue le pattern actuel en boucle").
- **Capitalisation:** French sentence case (only the first word and proper names capitalised) instead of
  English Title Case. ALL-CAPS knob labels stay in caps.
- **Typography:** a plain space (U+0020) before `:` `;` `?` `!` (France/Belgium/Switzerland rule; Québec only
  spaces before `:` but readers accept it). No narrow/non-breaking spaces: UI fonts and JUCE word-wrap handle
  U+0020 reliably, U+202F not. Quotes are « … » with plain spaces inside, so no `\"` escaping is needed.
- **Ellipsis:** "..." exactly as in English.
- **Numbers and units:** keep the English spacing around `%` and units next to placeholders ("{0}%", "{0} ms"),
  because values are formatted by the code and many labels are narrow. Decimal numbers written literally in a
  sentence use a comma (0,5).
- **Keys:** Ctrl, Alt, Tab, F1–F12 unchanged (printed so on French AZERTY keyboards). Shift → **Maj** (what
  French Windows and French DAW manuals print), Space → **Espace**, Del → **Suppr**, Backspace → **Retour
  arrière**, and in running text Esc → **Échap**, Enter → **Entrée** (as printed on AZERTY keys); arrows →
  **flèches**, Up/Down/Left/Right → Haut/Bas/Gauche/Droite. Bracketed key caps must stay ASCII, so they are
  [Ctrl] [Alt] [Maj] [Espace] [Haut] [Bas] [Gauche] [Droite], while [Esc] [Enter] [Backspace] [Tab] stay in
  English (the French names have accents or spaces). Non-markup brackets are text: "[double-click]" →
  "[double-clic]".
  "Ctrl+drag" → "Ctrl+glisser", "Alt-click" → "Alt+clic", "right-click" → "clic droit".
- **Mouse tokens** `[LMB]` `[2xLMB]` `[RMB]` `[MMB]` `[Wheel]` stay exactly in English (drawn as glyphs).
- **"|"-lists** keep the same number of items; algorithm/preset names stay in English, functional modes are
  translated.

## Stays in English (unchanged)

Channel Rack, Piano Roll, Playlist, Mixer, Browser (LYNX/FL proper names; French FL users say them in English,
used with the article: "le Channel Rack", "le Piano Roll", "la Playlist", "le Mixer", "le Browser").
Plugin, engine and product names (Sytrus, Harmor, Fruity …, LYNX Tune, Facet EQ, VFX, FL Studio, VST/VST3/CLAP,
ASIO, WASAPI, ALSA, JACK), file extensions, units (Hz, kHz, dB, ms, %, BPM, st, ct), note names (C4, F#), MIDI
terms (MIDI, CC, NRPN, MPE), and the producer jargon marked below.

## Terms

| English | Français | note |
|---|---|---|
| Channel Rack / Piano Roll / Playlist / Mixer / Browser | same | proper names (m., m., f., m., m.) |
| pattern | pattern | le pattern, pl. patterns |
| Pattern mode / Song mode | mode Pattern / mode Song | the "Pattern" / "Song" buttons stay |
| song | morceau | "Song" stays only as the mode name |
| clip | clip | audio clip = clip audio, automation clip = clip d'automation |
| track (Playlist and mixer) | piste | |
| channel | canal | pl. canaux |
| insert (mixer) | insert | l'insert (m.) du Mixer; mixer track = piste du Mixer |
| strip (mixer) | tranche | |
| send | send | le send; verb "send to" = envoyer vers |
| slot | slot | effect slot = slot d'effet |
| master | Master | |
| bar | mesure | "{0} bars" = "{0} mesures" |
| beat | temps | "bar : beat : step" = "mesure : temps : step" |
| step | step | le step; step sequencer = séquenceur pas à pas |
| tick | tick | |
| note | note | |
| velocity | vélocité | release velocity = vélocité de relâchement |
| pan / panning | pan / panoramique | knob label "Pan" stays |
| volume | volume | |
| level | niveau | |
| mute / unmute | rendre muet / réactiver | label "Mute" = "Muet" |
| solo | solo | "solo the channel" = mettre le canal en solo |
| preset | preset | le preset |
| sample | sample | le sample; sample rate = fréquence d'échantillonnage; "{0} samples" (latency) = "{0} échantillons" |
| plugin | plugin | le plugin |
| effect | effet | |
| instrument | instrument | |
| synth | synthé | |
| automation | automation | as in Ableton/FL French usage |
| quantize | quantifier / quantification | |
| snap | magnétisme | "No snap" = Sans magnétisme; "snap to grid" = aligner sur la grille |
| grid | grille | |
| loop | boucle | "in a loop" = en boucle |
| render | rendre / rendu | "Render to audio" = Rendre en audio |
| export | exporter | |
| stem | stem | pl. stems |
| tempo | tempo | |
| swing / groove | swing / groove | |
| humanize | humaniser | |
| pitch | hauteur | pitch bend = pitch bend; fine pitch = hauteur fine |
| semitone / cent | demi-ton / cent | st / ct unchanged |
| octave | octave | |
| envelope | enveloppe | "env." abbreviation = env. |
| LFO | LFO | |
| filter | filtre | |
| cutoff | coupure | "Cutoff LFO" = LFO de coupure |
| resonance | résonance | |
| attack / decay / sustain / release / hold | Attack / Decay / Sustain / Release / Hold | as on French synth UIs |
| delay (effect) | delay | "delay" of an LFO/envelope onset = délai |
| wet / dry | wet / dry | mix = mix |
| gain | gain | make-up gain = gain de compensation |
| threshold / ratio / knee | seuil / ratio / knee | |
| compressor / limiter / gate / expander | compresseur / limiteur / gate / expandeur | |
| sidechain | sidechain | |
| reverb | réverb | plugin names stay |
| feedback | réinjection | knob label may stay "Feedback" |
| distortion / saturation / drive | distorsion / saturation / drive | |
| EQ / band | EQ / bande | |
| low-pass / high-pass / band-pass | passe-bas / passe-haut / passe-bande | low cut / high cut = coupe-bas / coupe-haut |
| low / mid / high (bands) | graves / médiums / aigus | |
| shelf / peak / notch | shelf / crête / notch | low shelf / high shelf stay |
| oscillator | oscillateur | |
| waveform | forme d'onde | sine/triangle/saw/square/pulse = sinus/triangle/dent de scie/carré/impulsion |
| voice | voix | unison = unisson, polyphony = polyphonie |
| detune | désaccord | fine/coarse = fin/grossier |
| glide / portamento / legato | glide / portamento / legato | |
| arpeggiator | arpégiateur | up/down = montant/descendant |
| chord | accord | |
| strum | strum | |
| scale | gamme | |
| key (musical) | tonalité | |
| key (keyboard) | touche | PC keyboard = clavier du PC |
| root note | fondamentale | |
| triplet / dotted | triolet / pointé | |
| time signature | signature rythmique | New project label "Time signature" = Signature (narrow) |
| metronome / count-in | métronome / décompte | |
| latency | latence | delay compensation = compensation de latence |
| buffer | tampon | buffer size = taille du tampon |
| audio device | périphérique audio | sound card = carte son; driver = pilote |
| input / output | entrée / sortie | |
| MIDI controller | contrôleur MIDI | |
| knob / fader / slider | bouton / fader / curseur | |
| wheel | molette | |
| click / double-click / right-click | clic / double-clic / clic droit | verb cliquer |
| drag | glisser | "Alt+drag" = Alt+glisser |
| ruler | règle | |
| playhead | tête de lecture | |
| marker | marqueur | |
| record / recording | enregistrer / enregistrement | same verb as "save", context disambiguates (as in every French DAW) |
| arm (record) | armer | record safe = protection d'enregistrement |
| overdub / take | overdub / prise | |
| slice / slicer | découper, tranche / slicer | |
| stretch | étirer / étirement | time-stretch = time-stretch |
| fade in / fade out / crossfade | fondu entrant / fondu sortant / fondu enchaîné | "Fade-in"/"Fade-out" knob labels may stay |
| reverse | inverser / inversé | |
| normalize | normaliser | |
| freeze | figer | as Ableton French |
| consolidate / bounce | consolider / bouncer | |
| ghost notes | notes fantômes | |
| undo / redo | Annuler / Rétablir | undo history = historique des annulations |
| new / open / save / save as | Nouveau / Ouvrir / Enregistrer / Enregistrer sous | |
| open recent | Ouvrir un fichier récent | menu: "Récents" where narrow |
| template | modèle | |
| autosave | sauvegarde automatique | |
| settings / options / preferences | réglages / options / préférences | "Audio settings" = Réglages audio |
| OK / cancel / apply / close | OK / Annuler / Appliquer / Fermer | |
| accept / discard | Accepter / Ignorer | discard changes = ne pas enregistrer |
| delete / remove | Supprimer / Retirer | |
| rename | Renommer | |
| clone / duplicate | Cloner / Dupliquer | |
| cut / copy / paste | Couper / Copier / Coller | |
| select / select all / deselect | Sélectionner / Tout sélectionner / Désélectionner | |
| split | Scinder | |
| make unique | Rendre unique | |
| zoom | zoom / zoomer | |
| reset | Réinitialiser | |
| bypass | bypass / contourner | |
| enable / disable / on / off | activer / désactiver / activé / désactivé | label "Off" = Désactivé (or "Off" in narrow mode lists); short toggle label "On" = Actif |
| show / hide | afficher / masquer | |
| scan for plugins | Rechercher des plugins | |
| folder / file | dossier / fichier | |
| File / Edit / View / Options / Help | Fichier / Éditer / Vue / Options / Aide | short menu titles: the menu bar width decides how many toolbar icons fit at 1280 px |
| Exit | Quitter | |
| language / shortcuts | langue / raccourcis | |
| tooltip / hint bar | info-bulle / barre d'aide | |
| default | par défaut | |
| color | couleur | |
| length | durée | |
| position / time | position | |
| arrangement | arrangement | |
| routing | routage | |
| group | groupe | |
| kick / snare / hat / clap | Kick / Snare / Hat / Clap | drum names stay |
| meter | vumètre | |
| peak (meter) | crête | |
| spectrum analyzer | analyseur de spectre | |
| frequency | fréquence | |
| transient | transitoire | |
| warning / error | avertissement / erreur | |
| toolbar | barre d'outils | |
| panel / window | panneau / fenêtre | |
| tools: picker, pencil, brush, eraser, knife, lasso, slip | pipette, crayon, pinceau, gomme, couteau, lasso, glissement | |
| preview / audition | aperçu / pré-écoute | |
| built-in | intégré | |
| setup wizard | assistant de configuration | |
| backup | sauvegarde | |
| tail | queue | reverb tail = queue de réverb |
| chop | hacher | "Chop to grid" = Hacher sur la grille |
| amount | intensité | |
| speed / rate | vitesse | |
| range | étendue | |
| shape / period | forme / période | |
| node / handle | nœud / poignée | |
| cut group | groupe de coupure | |
| favorites / starred | favoris | |
| toolbar label "SNAP" | GRILLE | short caps label; elsewhere snap = magnétisme |
| tone (knob) | ton | "tonalité" is reserved for the musical key |
| layout (workspace) | disposition | |
| take | prise | comp track = piste de comp |
| layer (visualizer) | calque | the "Layer" channel type keeps its name |
| child (Layer) | enfant | |
| reverb/delay/chorus algorithm and preset names in "\|" lists | unchanged | e.g. "Concert Hall\|Plate\|Room...", tape-echo names, metal plate names; functional modes are translated |
| wrapper / bridge (plugin) | wrapper / bridge | "Make bridged" = Exécuter en bridge |
| hint "drag" in tooltips | glisser | imperatives use "glissez", "cliquez" |
