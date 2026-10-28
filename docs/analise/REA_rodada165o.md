# Round 165o — i18n165: LYNX in 15 languages

Goal: the interface in es, fr, de, it, ru, pl, tr, ja, ko, zh-Hans, zh-Hant, id, nl (on top of en + pt-BR):
natural musician-facing translations, one glossary per language (docs/i18n/<lang>-glossary.md), full key
coverage test, CJK/Cyrillic fonts on all 3 OS, no cut text, OS-language default, Settings › Language.

## Where I stopped
- 2026-10-28 15:40 — VERIFIED, landing. Linux textfit after the fit fixes: en 0, de/ru/pl 0 (pt-BR only the
  pre-existing main items: export summary line, sample-pitch hint, "Ligado" 52 px, "1 compasso"); en/pt-BR shots
  pixel-identical to before except the fixed rows (Event editor tool row: baseline was 40 px short, English moves
  ≤ 4 px; pt-BR Export "Salvar também" and Settings MIDI offset caption now on their own line instead of cut).
  Windows: ja and zh-Hans CRASHED at start-up (dangling std::initializer_list in theme::uiFontForLanguage — Linux
  survived by luck); fixed with a std::vector; then ja/ko/zh-Hans/zh-Hant/ru/en shots clean on Windows (Yu Gothic UI,
  Malgun Gothic, YaHei UI, JhengHei UI, Segoe UI Cyrillic), desktop clean. build-local 1 781 888 checks, 0 failures.
  Branch squashed to one commit (+ fixes); unsquashed history in origin/backup/i18n165-unsquashed.
- 2026-10-28 13:00 (resumed after the 2nd BSOD) — all 13 language branches are merged into feature/i18n165 (checked).
  Store: listings say the interface has 15 languages (the EN/PT-only note is gone), MSIX manifest + makepri /dq declare
  the 15 languages, docs/STORE.md + store/CHECKLIST.pt-BR.md updated (commit fd3e8bc8). notes-0.1.20.md bullets added.
  Linux shots en/pt-BR/de/ru/ja/zh-Hans: CJK + Cyrillic render fine; textfit still listed cut text in de/ru/pl:
  Audio editor quick buttons (now two rows when one row can't hold them, English unchanged), Export "Also save"
  (own line when too long), Settings rows (caption too wide for the 190 px column -> own line above the control),
  Event editor bottom tools (LFO boxes shrink too). Rebuilding Linux (lin7.log) to reshoot; Windows build win5.log;
  build-local bl3.log. Then: Windows shots ja/zh/ru, squash, land, remove the 13 worktrees/branches.

## Earlier
- 2026-10-28 09:30 — all 13 tables complete and fit-reviewed (de ru fr es nl pl tr it id ja ko zh-Hans zh-Hant;
  ko needed nothing), lint clean, 0 pending, stale key dropped. Generated data split per language
  (src/ui/i18n/<id>.h) so a table fix rewrites ~500 KB, not 6.7 MB — squash the branch before landing (20
  regenerations ≈ 88 MB of history otherwise).
- Code since the first shots: tight menu padding, Channel misc captions squeeze, trc() context keys
  ("@plugin status: OK", "@key tracking: Key", "@colour after change: New"), Root note Reset sized to text, hint bar
  wraps Chinese/Japanese between characters (+ full-width colon title), Missing files "Used by" translated, Linux CJK
  font detection accepts any Noto CJK/Source Han face (false warning for ko/zh), setup wizard shows the language in
  use, Sampler Keep length column wider.
- Running: Linux GUI build lin3.log, Windows win4.log. Next: reshoot Linux all 14 (tmp/i18n165/linshot.sh) and look;
  Windows shots (tmp/i18n165/winshot.sh <langs>, fresh LYNX_PROFILE_DIR + LYNX_TEST_MODE=1) for Yu Gothic/YaHei/
  Malgun/JhengHei + Cyrillic; build-local tests; squash + land; notes-0.1.20.md; tell store165.

## Plan
1. Infrastructure: languages() with 15 entries (native names), tables zlib-compressed in I18nData.h,
   OS-language default (first run), "New translation — suggestions welcome" note, tests for every table.
2. Translations: pt-BR.txt keys (5 053 entries) exported in chunks; one agent per language writes chunks +
   glossary; tools validate placeholders/escapes/newlines; merge into resources/i18n/<lang>.txt.
3. Fonts: JUCE 8 font fallback check on Windows/Linux/macOS; OS fonts by locale if needed.
4. Fit: off-screen shots per language, look at them, fix cut text (shrink-to-fit / tooltips).
5. Land in batches (es/fr/de/it first), notes-0.1.20.md, docs/i18n/README.md.
