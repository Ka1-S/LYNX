# LYNX Russian (ru) glossary

Reference for `resources/i18n/ru.txt` (language header "Русский"). English is the source language;
the pt-BR table was only used as a hint for meaning.

## Register and style

- **Form of address: polite "вы" (lower case), but avoid direct address wherever possible.** Russian
  software (Windows, Cubase, Ableton Live, FL Studio community translations) uses "вы". Commands and menu
  items use the infinitive ("Сохранить проект", "Переименовать канал"); descriptions use the third person
  ("Воспроизводит текущий паттерн по кругу"). Imperatives (when unavoidable) are polite plural
  ("Нажмите", "Перетащите").
- **Concise labels.** Russian runs ~20–30 % longer than English: on buttons, column headers and knob
  labels prefer the shorter natural word ("Удалить", not "Удалить навсегда"); common abbreviations are
  fine where the English is abbreviated (Громк., Част., Рез.). ALL-CAPS knob labels stay in caps
  (FREQ/GAIN often stay in English when they are hardware-style knob labels).
- **Capitalisation:** Russian sentence case (only the first word and proper names capitalised) instead
  of English Title Case.
- **Quotes:** «ёлочки» «…» (no escaping needed); nested „…“.
- **Ellipsis:** keep "..." exactly as in the English.
- **Ё:** written as "е" throughout (common Russian UI practice), e.g. "еще", "все".
- **Numbers and units:** keep the spacing of the English source ("{0}%", "10 ms" → "10 ms"); units stay in
  Latin script as in Russian DAW UIs (Hz, kHz, dB, ms, BPM, st, ct). Literal decimal numbers in sentences
  use a comma (0,5); placeholders are formatted by the code and left alone.
- **Shortcuts:** Russian keyboards print the same Latin key caps, so key names stay in English: Ctrl,
  Shift, Alt, Enter, Esc, Del, Tab, F1–F12. "Space" in running text → **Пробел**; inside markup brackets
  key caps stay ASCII: `[Space]`, `[Ctrl]`, `[Del]`, `[Up]`, `[Down]`, `[Left]`, `[Right]`.
  "Ctrl+drag" → "Ctrl+перетаскивание", "Alt-click" → "Alt+клик", "right-click" → "правый клик" (ПКМ in
  very short hints), "double-click" → "двойной клик".
- **Hint-bar markup tokens** `[LMB]`, `[2xLMB]`, `[RMB]`, `[MMB]`, `[Wheel]` stay exactly in English.
- **Lists separated by "|"** keep exactly the same number of items; algorithm/preset names stay English,
  functional modes are translated.

## Stays in English (unchanged)

Channel Rack, Piano Roll, Playlist, Mixer, Browser (LYNX/FL proper names; Russian FL users say them in
English or as "плейлист/микшер/пианоролл" in speech, but the UI keeps the English names, as pt-BR does).
Plugin, engine and product names (Sytrus, Harmor, Fruity …, LYNX Tune, Facet EQ, VFX, FL Studio,
VST/VST3/CLAP, ASIO, WASAPI, ALSA, JACK), file extensions, units, note names (C4, F#), MIDI terms (MIDI,
CC, NRPN, MPE).

Also kept in English: reverb/delay/compressor **algorithm and model names** in "|" lists and amp/tape
model names (Concert Hall, Plate, Vari-mu, Combo 59, Worn reel…), and the **ALL-CAPS faceplate labels of
plugin panels** (LYNX Tune: RETUNE SPEED, FLEX-TUNE, FORMANT SHIFT; Facet EQ dynamic band: FREQ, GAIN,
RANGE, THRESHOLD; Soundgoodizer-style INPUT GAIN, EMPHASIS…) — they mirror hardware/plugin conventions
Russian producers know in English and must fit fixed knob widths. ALL-CAPS labels of LYNX's own UI
(toolbar, Browser tabs, visualizer sections) are translated: ПРИВЯЗКА, ПАТТЕРН, МАСТЕР, ВЫСОТА, ЦП, ОЗУ,
ПОЛИ, ВСЕ, ПРОЕКТ, ИЗБРАННОЕ, СЛОИ, ТАКТ : ДОЛЯ : ШАГ.

## Pattern / Song

Transport buttons "Pattern" / "Song" = **Паттерн** / **Песня**; "Pattern mode" / "Song mode" = **Режим
паттерна** / **Режим песни** (how Russian FL users describe them).

## Counting

Russian has three plural forms, the tables have one string per key, so counts with a placeholder are
written as "Noun: {0}" ("{0} bars" = "Тактов: {0}", "{0} notes" = "Нот: {0}", "{0} file(s)" = "Файлов: {0}")
or with an invariant genitive plural after numbers ("{0} сэмплов", "{0} центов"). "1 …" keys use the
singular ("1 такт", "Найден 1 новый плагин").

## Terms

| English | Русский | note |
|---|---|---|
| Channel Rack | Channel Rack | proper name, masc. |
| Piano Roll | Piano Roll | proper name, masc. |
| Playlist | Playlist | proper name, masc. |
| Mixer | Mixer | proper name, masc. (generic "mixer" in prose = микшер) |
| Browser | Browser | proper name, masc. |
| pattern | паттерн | pl. паттерны |
| Pattern mode / Song mode | режим паттерна / режим песни | transport buttons Паттерн / Песня |
| song | песня | |
| clip | клип | audio clip = аудиоклип, pattern clip = клип паттерна, automation clip = клип автоматизации |
| track (Playlist) | дорожка | |
| channel | канал | |
| insert (mixer) | инсерт | "Insert {0}" = Инсерт {0}; "mixer track" = канал микшера; verb "Insert" = Вставить |
| send | посыл | "Send to" = Посыл на; audio editor "Send" (to Playlist/Sampler) = Отправить |
| slot | слот | effect slot = слот эффекта |
| strip (mixer) | линейка канала | |
| master | мастер | "Master" label = Мастер |
| bar | такт | "{0} bars" = "Тактов: {0}" (avoids plural forms) |
| beat | доля | position "bar : beat : step" = "такт : доля : шаг" |
| step | шаг | step sequencer = степ-секвенсор |
| note | нота | |
| velocity | velocity | kept as the FL/DAW term (Russian FL users say "велосити"); "how hard" in prose = сила нажатия |
| release velocity | release velocity | |
| pan | панорама | label "Pan" = Пан. |
| volume | громкость | label "Vol" = Громк. |
| level | уровень | |
| mute / unmute | заглушить / включить звук | label "Mute" = Заглушить; "Muted" = Заглушен |
| solo | соло | |
| preset | пресет | |
| sample | сэмпл | sample rate = частота дискретизации; latency "{0} samples" = "{0} сэмплов" |
| plugin | плагин | |
| effect | эффект | |
| instrument | инструмент | |
| synth | синт / синтезатор | |
| automation | автоматизация | |
| quantize | квантовать / квантизация | |
| snap | привязка | "snap to grid" = привязка к сетке; toolbar caption "SNAP" = СЕТКА |
| grid | сетка | |
| loop | луп / цикл | sample loop = луп; "in a loop" = по кругу; loop playback = зацикливание |
| render | рендер / рендерить | |
| export | экспорт / экспортировать | |
| stem | стем | pl. стемы |
| tempo | темп | |
| swing | свинг | |
| groove | грув | |
| humanize | хьюманайз / «очеловечить» | prose: очеловечивание |
| pitch | высота тона | knob "Pitch" = Pitch |
| pitch bend | питч-бенд | |
| semitone / cent | полутон / цент | |
| octave | октава | |
| envelope | огибающая | |
| LFO | LFO | |
| filter | фильтр | |
| cutoff | срез (cutoff) | label = Срез |
| resonance | резонанс | |
| attack / decay / sustain / release | атака / спад / сустейн / релиз | knob labels keep Attack/Decay/... when ALL CAPS |
| hold | удержание (hold) | |
| wet / dry | wet / dry | "Mix" = Микс |
| gain | гейн / усиление | label Gain = Gain |
| threshold / ratio / knee | порог / ratio / knee | |
| compressor / limiter / gate / expander | компрессор / лимитер / гейт / экспандер | |
| sidechain | сайдчейн | |
| reverb | реверб | |
| delay / echo | дилей / эхо | |
| feedback | обратная связь (feedback) | label = Feedback |
| distortion | дисторшн / искажение | |
| saturation | сатурация | |
| equalizer / EQ | эквалайзер / EQ | band = полоса |
| high-pass / low-pass / band-pass | ФВЧ / ФНЧ / полосовой | prose: high-pass фильтр is also common; we use ФВЧ/ФНЧ |
| oscillator | осциллятор | |
| waveform | форма волны | |
| voice (synth) | голос | polyphony = полифония, unison = унисон |
| detune | расстройка (detune) | fine = точно, coarse = грубо |
| glide / portamento | глайд / портаменто | |
| arpeggiator | арпеджиатор | |
| chord | аккорд | |
| strum | страм | |
| scale | лад / гамма | we use **гамма** (scale highlighting) |
| key (musical) | тональность | |
| key (keyboard) | клавиша | PC keyboard = клавиатура ПК |
| root note | тоника | |
| latency | задержка (латентность) | delay compensation = компенсация задержки |
| buffer | буфер | buffer size = размер буфера |
| audio device | аудиоустройство | sound card = звуковая карта; driver = драйвер |
| input / output | вход / выход | |
| knob | ручка | |
| fader | фейдер | |
| slider | ползунок | |
| wheel (mouse) | колесо мыши | |
| click / double-click / right-click | клик / двойной клик / правый клик | |
| drag | перетащить / перетаскивание | |
| ruler | линейка | |
| playhead | курсор воспроизведения | |
| marker | маркер | |
| time signature | размер | |
| count-in | отсчет | |
| metronome | метроном | |
| record / recording | записать / запись | |
| arm / disarm / record safe | подготовить к записи / снять готовность / защита от записи | armed = готов к записи |
| take / comp track | дубль / комп-дорожка | |
| round trip (latency) | круговая задержка | |
| sibilance / plosives | сибилянты / взрывные | de-esser = деэссер, de-ploser = деплозер |
| lookahead | упреждение | |
| gain reduction | подавление усиления | |
| tap (delay) | отвод | tap tempo = тап-темп |
| child (Layer) | дочерний (канал) | |
| hint tokens | [LMB] [RMB] [2xLMB] [MMB] [Wheel] | never translated |
| slice / slicer | слайс / слайсер | |
| time-stretch | тайм-стретч | |
| fade in / fade out / crossfade | фейд-ин / фейд-аут / кроссфейд | |
| reverse | реверс | |
| normalize | нормализовать | |
| freeze | заморозить | |
| consolidate / bounce | свести (дорожку) / рендер в аудио | "Consolidate track" = Свести дорожку |
| ghost notes | ghost-ноты | |
| undo / redo | отменить / повторить | undo history = история отмен |
| new / open / save / save as | создать / открыть / сохранить / сохранить как | menu "New" = Новый |
| open recent | недавние | |
| template | шаблон | |
| autosave | автосохранение | |
| settings / options | настройки / параметры | menu "Options" = Опции (short, fits the toolbar) |
| OK / cancel / apply / close | OK / Отмена / Применить / Закрыть | |
| discard | не сохранять / отбросить | "Discard" in save dialog = Не сохранять |
| delete / remove | удалить / убрать | |
| rename | переименовать | |
| clone / duplicate | клонировать / дублировать | |
| cut / copy / paste | вырезать / копировать / вставить | |
| select / select all / deselect | выбрать / выделить все / снять выделение | |
| split | разрезать | |
| make unique | сделать уникальным | |
| zoom | масштаб | |
| reset | сбросить | |
| bypass | байпас / обход | |
| enable / disable | включить / выключить | On/Off = Вкл/Выкл |
| show / hide | показать / скрыть | |
| folder / file | папка / файл | |
| File / Edit / View / Options / Tools / Help | Файл / Правка / Вид / Опции / Сервис / Справка | short titles keep the toolbar from dropping icons |
| Exit | Выход | |
| language | язык | |
| shortcuts | горячие клавиши | |
| tooltip / hint bar | подсказка / строка подсказок | |
| default | по умолчанию | |
| color | цвет | |
| length | длина | |
| arrangement | аранжировка | |
| routing | маршрутизация | |
