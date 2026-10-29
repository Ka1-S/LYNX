# LYNX Turkish (tr) glossary

Reference for `resources/i18n/tr.txt` (language header "Türkçe"). English is the source language;
the pt-BR table was only used as a hint for meaning.

## Register and style

- **Form of address: formal "siz", but avoid direct address where possible.** Turkish Windows, macOS and
  most localized software use the polite plural ("Kaydetmek istiyor musunuz?", "Bir dosya seçin").
  Menu entries and buttons use the short imperative as Turkish Windows does ("Kaydet", "Aç", "Sil",
  "Yeniden adlandır"). Descriptions/tooltips use the third person ("Geçerli pattern'i döngüde çalar").
  Instructions inside sentences use the formal imperative ("Bir knob'u sürükleyin").
- **Concise labels.** Buttons, menu entries and column headers stay short; prefer the shorter of two
  natural options; no extra explanations.
- **Capitalisation:** Turkish sentence case (only the first word and proper names capitalised) instead of
  English Title Case. Remember dotted/dotless I: "İptal", "İz", "ı/İ, i/I". ALL-CAPS knob labels stay caps.
- **Suffixes on English/foreign words** are attached with an apostrophe following the pronunciation:
  pattern'i/pattern'e/pattern'de, Playlist'e/Playlist'te, Mixer'a/Mixer'da, Channel Rack'e/Rack'te,
  Piano Roll'a/Roll'da, Browser'a/Browser'da, plugin'i/plugin'e/plugin'de, preset'i/preset'e,
  sample'ı/sample'a/sample'da, LYNX'i/LYNX'e/LYNX'te, MIDI'ye/MIDI'de, FL Studio'ya/Studio'da.
- **Placeholders:** never glue a case suffix to {0}/{1} (vowel harmony depends on the inserted word).
  Restructure instead: "Açılamadı: {0}", "{0} adlı dosya…" , "Kanal: {0}". Plain plurals after numbers are
  not used in Turkish ("{0} ölçü", "{0} nota", never "{0} ölçüler").
- **Quotes:** “…” (no escaping needed). **Ellipsis:** keep "..." as in English.
- **Numbers and units:** keep the spacing of the source ("10 ms", "{0}%"). Turkish puts % before the
  number in prose (%50), but placeholders keep the English order "{0}%" because the code formats it.
- **Shortcuts:** key names stay as printed on Turkish keyboards, which are the English names: Ctrl, Shift,
  Alt, Esc, Enter, Tab, Del, F1–F12. In running text Space → "Boşluk", arrows → "Ok tuşları".
  "Ctrl+drag" → "Ctrl+sürükle", "Alt-click" → "Alt+tık", "right-click" → "sağ tık", double-click → "çift tık".
- **Hint-bar markup tokens** (src/core/HintMarkup.h): `[LMB]`, `[2xLMB]`, `[RMB]`, `[MMB]`, `[Wheel]` stay
  exactly in English. Key caps in brackets stay ASCII/English: `[Ctrl]`, `[Shift]`, `[Alt]`, `[Space]`,
  `[Esc]`, `[Enter]`, `[Up]`, `[Down]`, `[Left]`, `[Right]`, `[Backspace]`, `[F1]`… Non-markup brackets such as
  "[double-click]" are translated as text ("[çift tık]").
- **Lists separated by "|"** keep the same number of items; algorithm/preset names stay English, functional
  modes are translated.

## Stays in English (unchanged)

Channel Rack, Piano Roll, Playlist, Mixer, Browser (LYNX/FL proper names; Turkish FL users use them in
English, FL Studio has no Turkish UI). Plugin, engine and product names (Sytrus, Harmor, Fruity …, LYNX Tune,
Facet EQ, VFX, FL Studio, VST/VST3/CLAP, ASIO, WASAPI, ALSA, JACK), file extensions, units (Hz, kHz, dB, ms,
%, BPM, st, ct), note names (C4, F#), MIDI terms (MIDI, CC, NRPN, MPE), and the producer jargon below
(pattern, sample, plugin, preset, step, swing, send, insert, slot, velocity, pan, gain, cutoff, ADSR…).

## Terms

| English | Türkçe | note |
|---|---|---|
| Channel Rack | Channel Rack | proper name; Channel Rack'e, Channel Rack'te |
| Piano Roll | Piano Roll | proper name; Piano Roll'a, Piano Roll'da |
| Playlist | Playlist | proper name; Playlist'e, Playlist'te |
| Mixer | Mixer | proper name; Mixer'a, Mixer'da |
| Browser | Browser | proper name; Browser'a, Browser'da |
| pattern | pattern | Pattern mode / Song mode = Pattern modu / Song modu; "Pattern"/"Song" buttons unchanged |
| song | şarkı | "Song" mode button stays "Song" |
| clip | klip | klibi, klibe, klipte; audio clip = ses klibi; pattern clip = pattern klibi; automation clip = otomasyon klibi |
| track (Playlist) | iz | "Track {0}" = "İz {0}" |
| track / insert (Mixer) | mixer kanalı / insert | generic "track" in Mixer context = kanal |
| channel (Channel Rack) | kanal | |
| strip (mixer) | kanal şeridi | |
| insert | insert | |
| send | send | verb: göndermek |
| slot | slot | effect slot = efekt slot'u |
| master | Master | |
| bar | ölçü | "{0} bars" = "{0} ölçü" |
| beat | vuruş | "bar : beat : step" = "ölçü : vuruş : step" |
| step | step | step sequencer = step sequencer |
| tick | tick | |
| note | nota | |
| velocity | velocity | release velocity = release velocity |
| pan | pan | |
| volume | ses | as label "Ses"; in sentences "ses düzeyi" |
| level | seviye | |
| mute / unmute | sustur / sesi aç | state label "Mute" = "Sessiz" |
| solo | solo | verb: solo yap |
| preset | preset | preset'i |
| sample | sample | sample'ı; sample rate = örnekleme hızı; "{0} samples" (latency) = "{0} sample" |
| plugin | plugin | plugin'i |
| effect | efekt | |
| instrument | enstrüman | |
| synth | synth | |
| automation | otomasyon | automate = otomasyon yapmak / otomatikleştir |
| quantize | kuantize | quantize = kuantize et |
| snap | snap | "No snap" = "Snap yok" |
| grid | grid | grid'e oturt |
| loop | döngü (looping) / loop (audio loop) | "in a loop" = "döngüde" |
| render | render | render al / render et |
| export / import | dışa aktar / içe aktar | |
| stem | stem | |
| tempo | tempo | |
| swing | swing | |
| groove | groove | |
| humanize | humanize / insansılaştır | prefer "humanize" |
| pitch | pitch | pitch bend = pitch bend |
| semitone / cent | yarım ton / cent | |
| octave | oktav | |
| envelope | envelope | "Cutoff env." = "Cutoff env." |
| LFO | LFO | |
| filter | filtre | |
| cutoff / resonance | cutoff / rezonans | |
| attack / hold / decay / sustain / release | Attack / Hold / Decay / Sustain / Release | |
| wet / dry / mix | wet / dry / mix | |
| gain | gain | |
| threshold / ratio / knee | threshold / ratio / knee | |
| compressor / limiter / gate | kompresör / limiter / gate | |
| sidechain | sidechain | |
| reverb / delay / echo | reverb / delay / eko | |
| feedback | feedback | |
| distortion / drive / saturation | distorsiyon / drive / satürasyon | |
| EQ / band | EQ / bant | |
| low-pass / high-pass / band-pass | low-pass / high-pass / band-pass | low cut / high cut = low cut / high cut |
| lows / mids / highs | bas / orta / tiz | |
| oscillator | osilatör | |
| waveform | dalga formu | sine/triangle/saw/square = sinüs/üçgen/testere/kare |
| voice (synth) | ses (voice) | polyphony = polifoni, unison = unison |
| detune | detune | fine / coarse = ince / kaba |
| glide / portamento / legato | glide / portamento / legato | |
| arpeggiator | arpeggiator | |
| chord | akor | root note = kök nota |
| strum | strum | |
| scale | gam | |
| key (musical) | ton | |
| key (keyboard) | tuş | PC keyboard = PC klavyesi |
| latency | gecikme | delay compensation = gecikme telafisi |
| buffer | arabellek | buffer size = arabellek boyutu |
| audio device | ses aygıtı | sound card = ses kartı; driver = sürücü |
| input / output | giriş / çıkış | |
| MIDI controller | MIDI kontrolcü | |
| knob / fader / slider | knob / fader / kaydırıcı | |
| wheel (mouse) | tekerlek | Ctrl+wheel = Ctrl+tekerlek |
| click / double-click / right-click | tık / çift tık / sağ tık | verbs: tıkla(yın) |
| drag | sürükle | |
| ruler | cetvel | |
| playhead | çalma imleci | |
| marker | işaretçi | |
| time signature | ölçü işareti | |
| count-in | geri sayım | |
| metronome | metronom | |
| record / recording | kaydet / kayıt | record (audio) = kayıt al |
| arm | kayda hazırla | |
| take | take | |
| slice / slicer | dilim / slicer | "Slice sample" = "Sample'ı dilimle" |
| stretch | stretch / esnet | time-stretch = time-stretch |
| fade in / fade out / crossfade | fade-in / fade-out / crossfade | |
| reverse | ters çevir / ters | |
| normalize | normalize et | |
| freeze | dondur | |
| consolidate / bounce | birleştir / bounce | |
| ghost notes | hayalet notalar | |
| undo / redo | Geri al / Yinele | undo history = geri alma geçmişi |
| new / open / save / save as | Yeni / Aç / Kaydet / Farklı kaydet | |
| open recent | Son kullanılanlar | |
| template | şablon | |
| autosave | otomatik kayıt | |
| settings / options | Ayarlar / Seçenekler | |
| OK / cancel / apply / close | Tamam / İptal / Uygula / Kapat | |
| discard | Kaydetme / Yoksay | dialog button "Discard" = "Kaydetme" |
| delete / remove | Sil / Kaldır | |
| rename | Yeniden adlandır | |
| clone / duplicate | Klonla / Çoğalt | |
| cut / copy / paste | Kes / Kopyala / Yapıştır | |
| select / select all / deselect | Seç / Tümünü seç / Seçimi kaldır | |
| split | Böl | |
| make unique | Benzersiz yap | |
| zoom | yakınlaştır / uzaklaştır / zoom | |
| reset | Sıfırla | |
| bypass | bypass | |
| enable / disable / on / off | etkinleştir / devre dışı bırak / açık / kapalı | |
| show / hide | göster / gizle | |
| scan for plugins | Plugin tara | |
| folder / file | klasör / dosya | |
| File / Edit / View / Options / Help | Dosya / Düzen / Görünüm / Seçenekler / Yardım | |
| Exit | Çıkış | |
| language | dil | |
| shortcuts | kısayollar | |
| tooltip / hint bar | ipucu / ipucu çubuğu | |
| default | varsayılan | |
| color | renk | |
| length | uzunluk | |
| position | konum | |
| arrangement | aranjman | |
| routing | yönlendirme | route = yönlendir |
| current | geçerli | |
| selected | seçili | |
