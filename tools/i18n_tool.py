# Translation workflow helper for resources/i18n/<lang>.txt (JUCE LocalisedStrings tables).
# English is the source language; resources/i18n/pt-BR.txt is the reference table: every key it has must
# exist in every other language file.
#
# Usage (from the repository root):
#   python3 tools/i18n_tool.py status                      coverage of every language
#   python3 tools/i18n_tool.py export <lang> <dir> [N]     writes the keys <lang> still lacks to <dir>/src-NN.txt
#                                                          (N keys per chunk, default 400), with the pt-BR text as
#                                                          a hint; translate into <dir>/out-NN.txt
#   python3 tools/i18n_tool.py check <lang> <dir>          validates <dir>/out-*.txt (placeholders, escapes...)
#   python3 tools/i18n_tool.py merge <lang> <dir>          merges <dir>/out-*.txt into resources/i18n/<lang>.txt
#   python3 tools/i18n_tool.py lint <lang>                 validates resources/i18n/<lang>.txt itself
#   python3 tools/i18n_tool.py pending                     rewrites resources/i18n/pending.keys with the pt-BR keys
#                                                          some other table still lacks (the tests accept those)
#
# Output chunk format: one line per string, exactly like the tables:  "English text" = "Translation"
# (escapes \n \t \" \\; keep {0} {1}... placeholders, % signs, and leading/trailing spaces and punctuation).
import pathlib, re, sys

root = pathlib.Path(__file__).resolve().parent.parent
i18n = root / "resources/i18n"
sys.path.insert(0, str(root / "tools"))
from i18n_keys import unescape, escape, table  # noqa: E402

# id, file header "language:" name, JUCE "countries:" codes
LANGUAGES = {
    "pt-BR": ("Português (Brasil)", "br"),
    "es": ("Español", "es mx ar co cl pe ve"),
    "fr": ("Français", "fr be ch ca lu"),
    "de": ("Deutsch", "de at ch li lu"),
    "it": ("Italiano", "it ch sm"),
    "ru": ("Русский", "ru by kz"),
    "pl": ("Polski", "pl"),
    "tr": ("Türkçe", "tr"),
    "ja": ("日本語", "jp"),
    "ko": ("한국어", "kr"),
    "zh-Hans": ("简体中文", "cn sg"),
    "zh-Hant": ("繁體中文", "tw hk mo"),
    "id": ("Bahasa Indonesia", "id"),
    "nl": ("Nederlands", "nl be"),
}

PAIR = re.compile(r'^"((?:[^"\\]|\\.)*)"\s*=\s*"((?:[^"\\]|\\.)*)"\s*$')


def reference():
    return table(i18n / "pt-BR.txt")


def placeholders(s):
    return sorted(re.findall(r"\{\d\}", s))


def problems(english, text):
    """Reasons a translation is not acceptable (empty list = fine)."""
    out = []
    if not text.strip():
        out.append("empty")
    if placeholders(english) != placeholders(text):
        out.append("placeholders differ")
    if english.count("\n") != text.count("\n"):
        out.append("line breaks differ")
    if english.count("\t") != text.count("\t"):
        out.append("tabs differ")
    if english.count("%") != text.count("%"):
        out.append("% signs differ")
    mice = lambda t: sorted(re.findall(r"\[(?:LMB|2xLMB|RMB|MMB|Wheel)\]", t))
    if mice(english) != mice(text):
        out.append("[LMB]/[RMB]/[Wheel] mouse tokens differ (keep them in English)")
    if english[:1] == " " and text[:1] != " ":
        out.append("leading space lost")
    if english[-1:] == " " and text[-1:] != " ":
        out.append("trailing space lost")
    if english.endswith("...") and not (text.endswith("...") or text.endswith("…")):
        out.append("trailing ... lost")
    return out


def read_chunks(folder):
    entries, bad = {}, []
    for path in sorted(pathlib.Path(folder).glob("out-*.txt")):
        for n, line in enumerate(path.read_text(encoding="utf-8").split("\n"), 1):
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            m = PAIR.match(s)
            if not m:
                bad.append(f"{path.name}:{n}: not a \"key\" = \"value\" line: {s[:80]}")
                continue
            entries[unescape(m.group(1))] = unescape(m.group(2))
    return entries, bad


def cmd_status():
    ref = reference()
    for lang in LANGUAGES:
        path = i18n / f"{lang}.txt"
        t = table(path) if path.exists() else {}
        have = sum(1 for k in ref if t.get(k))
        print(f"{lang:8} {have:5}/{len(ref)}")


def cmd_export(lang, folder, size=400):
    ref = reference()
    path = i18n / f"{lang}.txt"
    t = table(path) if path.exists() else {}
    todo = [k for k in ref if not t.get(k)]
    folder = pathlib.Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    for i in range(0, len(todo), size):
        lines = [f"# chunk {i // size + 1:02}: translate each \"English\" into {LANGUAGES[lang][0]} ({lang});",
                 "# the '# pt-BR:' line is the Brazilian Portuguese translation, only as a hint for the meaning.", ""]
        for k in todo[i:i + size]:
            lines.append(f"# pt-BR: {escape(ref[k])}")
            lines.append(f'"{escape(k)}" = ""')
        (folder / f"src-{i // size + 1:02}.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{len(todo)} keys -> {(len(todo) + size - 1) // size} chunk(s) in {folder}")


def cmd_check(lang, folder):
    ref = reference()
    entries, bad = read_chunks(folder)
    for b in bad:
        print(b)
    count = 0
    for k, v in entries.items():
        if k not in ref:
            print(f"unknown key (typo in the English?): \"{escape(k)[:90]}\"")
            count += 1
            continue
        for p in problems(k, v):
            print(f"{p}: \"{escape(k)[:90]}\"")
            count += 1
    path = i18n / f"{lang}.txt"
    t = table(path) if path.exists() else {}
    missing = [k for k in ref if not entries.get(k) and not t.get(k)]
    print(f"{lang}: {len(entries)} translated in chunks, {len(missing)} still missing, {count + len(bad)} problem(s)")
    return count + len(bad)


def cmd_merge(lang, folder):
    ref = reference()
    path = i18n / f"{lang}.txt"
    current = table(path) if path.exists() else {}
    entries, _ = read_chunks(folder) if folder else ({}, [])
    merged = {k: v for k, v in current.items()}
    merged.update({k: v for k, v in entries.items() if k in ref and not problems(k, v)})
    name, countries = LANGUAGES[lang]
    out = [f"language: {name}", f"countries: {countries}", "",
           f"# LYNX interface in {name} (JUCE LocalisedStrings format).",
           "# One line per string: \"English text\" = \"Translation\"  (escapes: \\n \\t \\\" \\\\)",
           "# Every key of pt-BR.txt must be here; terms: docs/i18n/" + lang + "-glossary.md; how to help: docs/i18n/README.md",
           "# After editing, run: python3 tools/make_i18n.py  (embeds the tables in src/ui/I18nData.h)"]
    # same order and sections as pt-BR.txt
    for line in (i18n / "pt-BR.txt").read_text(encoding="utf-8").split("\n"):
        s = line.strip()
        if s.startswith("# ---"):
            out += ["", s]
            continue
        m = PAIR.match(s)
        if m:
            k = unescape(m.group(1))
            if merged.get(k):
                out.append(f'"{escape(k)}" = "{escape(merged[k])}"')
    # entries this language has beyond pt-BR (kept, at the end)
    extra = [k for k in merged if k not in ref and merged[k]]
    if extra:
        out += ["", "# --- Not in pt-BR.txt (yet)"] + [f'"{escape(k)}" = "{escape(merged[k])}"' for k in extra]
    path.write_text("\n".join(out) + "\n", encoding="utf-8")
    have = sum(1 for k in ref if merged.get(k))
    print(f"{path.name}: {have}/{len(ref)} keys")


def cmd_lint(lang):
    ref = reference()
    t = table(i18n / f"{lang}.txt")
    n = 0
    for k, v in t.items():
        for p in problems(k, v):
            print(f"{p}: \"{escape(k)[:90]}\"")
            n += 1
    missing = [k for k in ref if not t.get(k)]
    print(f"{lang}: {len(t)} entries, {len(missing)} missing, {n} problem(s)")
    return n


def cmd_pending():
    ref = reference()
    path = i18n / "pending.keys"
    head = [l for l in path.read_text(encoding="utf-8").split("\n") if l.startswith("#")] if path.exists() else []
    lacking = []
    for lang in LANGUAGES:
        p = i18n / f"{lang}.txt"
        if lang == "pt-BR" or not p.exists():
            continue
        t = table(p)
        lacking += [k for k in ref if not t.get(k) and k not in lacking]
    path.write_text("\n".join(head + [f'"{escape(k)}"' for k in lacking]) + "\n", encoding="utf-8")
    print(f"{len(lacking)} pending key(s) in {path.name}")


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a:
        print(__doc__ or open(__file__).read().split("import")[0])
    elif a[0] == "status":
        cmd_status()
    elif a[0] == "export":
        cmd_export(a[1], a[2], int(a[3]) if len(a) > 3 else 400)
    elif a[0] == "check":
        sys.exit(1 if cmd_check(a[1], a[2]) else 0)
    elif a[0] == "merge":
        cmd_merge(a[1], a[2] if len(a) > 2 else None)
    elif a[0] == "pending":
        cmd_pending()
    elif a[0] == "lint":
        sys.exit(1 if cmd_lint(a[1]) else 0)
