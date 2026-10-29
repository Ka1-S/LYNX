# Lists the translatable strings of the UI (string literals passed to tr() / TR_NOOP()) and
# checks them against a translation table.
# Usage: python3 tools/i18n_keys.py [resources/i18n/pt-BR.txt] [--missing] [--unused]
#   --missing  print the keys with no translation, as lines ready to paste into the table
#   --unused   print table entries no source uses (JUCE's own strings are expected here)
import pathlib, re, sys

root = pathlib.Path(__file__).resolve().parent.parent

def unescape(s):
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            out.append({"n": "\n", "t": "\t", '"': '"', "\\": "\\", "'": "'"}.get(n, n))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)

def escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t")

def source_keys():
    keys = {}
    call = re.compile(r'(?<![A-Za-z0-9_])(tr|trc|TR_NOOP)\s*\(\s*')
    literal = re.compile(r'"((?:[^"\\\n]|\\.)*)"\s*')
    for path in sorted(list((root / "src").rglob("*.cpp")) + list((root / "src").rglob("*.h"))):
        text = path.read_text(encoding="utf-8", errors="replace")
        text = "\n".join("" if l.strip().startswith("//") else l for l in text.split("\n"))
        for m in call.finditer(text):
            pos, parts = m.end(), []
            while True:
                lm = literal.match(text, pos)
                if not lm:
                    break
                parts.append(lm.group(1))
                pos = lm.end()
            if parts:
                keys.setdefault(unescape("".join(parts)), str(path.relative_to(root)))
    return keys

def table(path):
    entries = {}
    pair = re.compile(r'^"((?:[^"\\]|\\.)*)"\s*=\s*"((?:[^"\\]|\\.)*)"\s*$')
    for line in pathlib.Path(path).read_text(encoding="utf-8").split("\n"):
        m = pair.match(line.strip())
        if m:
            entries[unescape(m.group(1))] = unescape(m.group(2))
    return entries

if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    path = args[0] if args else root / "resources/i18n/pt-BR.txt"
    keys = source_keys()
    entries = table(path) if pathlib.Path(path).exists() else {}
    missing = [k for k in keys if not entries.get(k)]
    if "--missing" in sys.argv:
        last = None
        for k in missing:
            if keys[k] != last:
                print(f"\n# {keys[k]}")
                last = keys[k]
            print(f'"{escape(k)}" = ""')
    elif "--unused" in sys.argv:
        for k in entries:
            if k not in keys:
                print(f'"{escape(k)}"')
    else:
        print(f"{len(keys)} keys in the sources, {len(entries)} entries in {path}, {len(missing)} missing")
