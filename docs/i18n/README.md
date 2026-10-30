# LYNX SHARE translations

LYNX SHARE's interface is written in English; every other language is a table in `resources/i18n/<id>.txt`
(JUCE `LocalisedStrings` format) embedded in the executable. Settings › General › Language lists them, each in
its own name; on the very first start LYNX SHARE uses the operating system's language when it has a table for it.

| id | Language | Glossary |
|---|---|---|
| pt-BR | Português (Brasil) | header of `pt-BR.txt` |
| es | Español | [es-glossary.md](es-glossary.md) |
| fr | Français | [fr-glossary.md](fr-glossary.md) |
| de | Deutsch | [de-glossary.md](de-glossary.md) |
| it | Italiano | [it-glossary.md](it-glossary.md) |
| nl | Nederlands | [nl-glossary.md](nl-glossary.md) |
| pl | Polski | [pl-glossary.md](pl-glossary.md) |
| ru | Русский | [ru-glossary.md](ru-glossary.md) |
| tr | Türkçe | [tr-glossary.md](tr-glossary.md) |
| id | Bahasa Indonesia | [id-glossary.md](id-glossary.md) |
| ja | 日本語 | [ja-glossary.md](ja-glossary.md) |
| ko | 한국어 | [ko-glossary.md](ko-glossary.md) |
| zh-Hans | 简体中文 | [zh-Hans-glossary.md](zh-Hans-glossary.md) |
| zh-Hant | 繁體中文 | [zh-Hant-glossary.md](zh-Hant-glossary.md) |

The translations other than pt-BR are new in 0.1.20. If a term sounds wrong to producers in your language, please
[open an issue](https://github.com/Ka1-S/LYNX-SHARE/issues/new?title=Translation%20(xx)%3A%20) (Settings › General ›
Language has a link too) or send a pull request.

## The file format

```
language: Deutsch
countries: de at ch li lu

"Open project" = "Projekt öffnen"
"Swing: {0}%" = "Swing: {0} %"
```

- One line per string: `"English text" = "translation"`. Escapes: `\n` (line break), `\t`, `\"`, `\\`.
- Keep `{0}`, `{1}`… (they may move inside the sentence), every `%`, line breaks, and leading/trailing spaces.
- Keep plugin and product names (Sytrus, Fruity …, VST3, ASIO), units (Hz, dB, ms, BPM) and note names (C4).
- Use the glossary of your language (`docs/i18n/<id>-glossary.md`) so a term is the same everywhere; add new terms
  to it.

## Fixing or completing a translation

```
python3 tools/i18n_tool.py status              # coverage of every language
python3 tools/i18n_tool.py lint de             # placeholders, line breaks, % signs, missing keys
python3 tools/i18n_tool.py export de /tmp/de   # the keys de still lacks, in chunks (with the pt-BR text as a hint)
#   ... translate /tmp/de/src-NN.txt into /tmp/de/out-NN.txt (same "English" = "translation" lines) ...
python3 tools/i18n_tool.py check de /tmp/de    # validates the chunks
python3 tools/i18n_tool.py merge de /tmp/de    # merges them into resources/i18n/de.txt
python3 tools/make_i18n.py                     # embeds the tables (src/ui/i18n/<id>.h)
```

Try it: `LYNX --lang de` starts in that language for one run (Settings keeps the saved one), and
`LYNX --lang de --snapshot shots/de` writes off-screen pictures of the main windows to check that nothing is cut.

## Adding a UI string (developers)

Wrap the English text in `tr ("...")` (or `TR_NOOP` for tables). The tests require a pt-BR translation of every
`tr()` string and every pt-BR key in all the other tables. When you can't translate a new string into all of
them, add its pt-BR line and run `python3 tools/i18n_tool.py pending`: it lists the keys the other tables still
lack in `resources/i18n/pending.keys`, which the tests accept (the text shows in English until it is translated).

## Adding a language

1. `resources/i18n/<id>.txt` with the header (`language:` its own name, `countries:` ISO codes), every key of
   `pt-BR.txt` translated, and `docs/i18n/<id>-glossary.md`.
2. Its id and own name in `languages()` (src/ui/I18n.cpp) and in `LANGUAGES` (tools/i18n_tool.py).
3. Japanese/Korean/Chinese-like scripts: the OS UI fonts to prefer in `LookAndFeel::applyLanguageFont`
   (src/ui/Theme.cpp).
4. `python3 tools/make_i18n.py`, build, run the tests and the `--snapshot` pictures in the new language.
