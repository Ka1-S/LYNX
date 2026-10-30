#!/usr/bin/env python3
"""Fills Partner Center's exported Store-listing CSV with LYNX's listings in every language (round 165n).

Usage: scripts/store-listing-csv.py <exported.csv> <output folder>

1. Partner Center › LYNX SHARE › app overview › Store listings › **Export listing** → a .csv (UTF-8) whose Field / ID /
   Type columns must stay as they are (the IDs are Partner Center's).
2. This script writes <output folder>/listings.csv (the same rows, a column per language from store/listings/*.md,
   the shared fields from store/listings/common.md in the "default" column) and copies the screenshots and logos into
   <output folder>/, with the paths Partner Center expects ("<folder name>/screenshots/01-workspace.png").
3. Partner Center › **Import listings** › **Import folder** → choose <output folder>.

Field names in the export are matched loosely (case and punctuation ignored, a few known aliases); everything that
could not be placed is listed at the end, so a renamed field shows up instead of being silently dropped.
"""
import csv
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LISTINGS = os.path.join(ROOT, "store", "listings")
# extra listing languages that reuse another file's text
REUSE = {"es-mx": "es-es"}
LIMITS = {"Description": 10000, "ReleaseNotes": 1500, "ShortDescription": 1000, "Title": 256,
          "Feature": 200, "SearchTerm": 30, "Caption": 200}


def norm(name):
    return re.sub(r"[^a-z0-9]", "", name.lower())


def is_cjk(ch):
    return ord(ch) >= 0x2E80


def unwrap(lines):
    """Hard-wrapped Markdown paragraphs → Store text: a paragraph is one line, bullets ("•", "- ") start new lines,
    blank lines stay paragraph breaks. CJK lines are joined without a space."""
    out = []
    for raw in lines:
        line = raw.rstrip()
        if not line:
            if out and out[-1] != "":
                out.append("")
            continue
        starts_item = line.lstrip().startswith(("•", "- ", "* ")) or re.match(r"^\s*\d+\.\s", line)
        if out and out[-1] != "" and not starts_item:
            prev = out[-1]
            joiner = "" if (prev and is_cjk(prev[-1])) or is_cjk(line.lstrip()[0]) else " "
            out[-1] = prev + joiner + line.strip()
        else:
            out.append(line.strip())
    while out and out[-1] == "":
        out.pop()
    return "\n".join(out)


def sections(path):
    """'## Heading' → list of body lines (HTML comments removed)."""
    with open(path, encoding="utf-8") as f:
        text = re.sub(r"<!--.*?-->", "", f.read(), flags=re.S)
    result, current = {}, None
    for line in text.splitlines():
        m = re.match(r"^##\s+(.+?)\s*$", line)
        if m:
            current = m.group(1)
            result[current] = []
        elif current is not None:
            result[current].append(line)
    return result


def items(lines):
    """Numbered or bulleted list items, one per line."""
    out = []
    for line in lines:
        m = re.match(r"^\s*(?:\d+\.|[-*•])\s+(.*\S)\s*$", line)
        if m:
            out.append(m.group(1))
    return out


def language_fields(path):
    """{logical field: text} for one language file."""
    s = sections(path)
    fields = {}
    simple = {"Title": "Title", "Short description": "ShortDescription", "Description": "Description",
              "What's new": "ReleaseNotes"}
    for heading, field in simple.items():
        if heading in s:
            fields[field] = unwrap(s[heading])
    for i, text in enumerate(items(s.get("Features", [])), 1):
        fields[f"Feature{i}"] = text
    for i, text in enumerate(items(s.get("Search terms", [])), 1):
        fields[f"SearchTerm{i}"] = text
    for i, text in enumerate(items(s.get("Screenshot captions", [])), 1):
        # "01-workspace.png — caption"
        fields[f"Caption{i}"] = re.split(r"\s+[—–-]\s+", text, maxsplit=1)[-1]
    return fields


def candidates(field):
    m = re.match(r"^([A-Za-z]+?)(\d+)$", field)
    if field.startswith("Caption") and m:
        n = m.group(2)
        return [f"desktopscreenshotcaption{n}", f"desktopscreenshot{n}caption", f"screenshotcaption{n}",
                f"caption{n}"]
    if field.startswith("Screenshot") and m:
        return [f"desktopscreenshot{m.group(2)}", f"screenshot{m.group(2)}"]
    aliases = {"ReleaseNotes": ["releasenotes", "whatsnew"], "PrivacyPolicy": ["privacypolicy", "privacypolicyurl"],
               "WebsiteURL": ["websiteurl", "website"], "SupportContactInfo": ["supportcontactinfo", "supportcontact"],
               "DevStudio": ["devstudio", "developedby"],
               "CopyrightTrademarkInfo": ["copyrighttrademarkinfo", "copyrighttrademarkinformation"]}
    return aliases.get(field, [norm(field)])


def check_limit(lang, field, text, problems):
    key = re.sub(r"\d+$", "", field)
    limit = LIMITS.get(key)
    if limit and len(text) > limit:
        problems.append(f"{lang}: {field} has {len(text)} characters (limit {limit})")


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    exported, out = argv
    with open(exported, encoding="utf-8-sig", newline="") as f:
        rows = list(csv.reader(f))
    # Partner Center has exported the third header both as "Type" and as "Type (Type)"
    if not rows or [c.strip().lower().split(" (")[0] for c in rows[0][:4]] != ["field", "id", "type", "default"]:
        raise SystemExit("not a Partner Center listing export (first columns must be Field, ID, Type, default)")
    header = rows[0]
    index = {}
    for r, row in enumerate(rows[1:], 1):
        if row:
            index.setdefault(norm(row[0]), r)

    folder = os.path.basename(os.path.normpath(out))
    os.makedirs(os.path.join(out, "screenshots"), exist_ok=True)
    problems, unplaced = [], []

    def put(field, column, text):
        for c in candidates(field):
            if c in index:
                row = rows[index[c]]
                while len(row) <= column:
                    row.append("")
                row[column] = text
                return True
        unplaced.append(field)
        return False

    # default column: shared fields, screenshots and logos
    common = sections(os.path.join(LISTINGS, "common.md"))
    for heading, lines in common.items():
        if heading == "Screenshots":
            for i, name in enumerate([l.strip() for l in lines if l.strip()], 1):
                shutil.copy2(os.path.join(ROOT, "store", "screenshots", name), os.path.join(out, "screenshots", name))
                put(f"Screenshot{i}", 3, f"{folder}/screenshots/{name}")
        elif heading.startswith("StoreLogo"):
            name = unwrap(lines)
            shutil.copy2(os.path.join(ROOT, "store", name), os.path.join(out, name))
            put(heading, 3, f"{folder}/{name}")
        else:
            put(heading, 3, unwrap(lines))

    # one column per language file
    langs = sorted(f[:-3] for f in os.listdir(LISTINGS) if f.endswith(".md") and f != "common.md")
    # an extra language goes in only when the export has its column: importing a column the submission lacks fails
    langs += sorted(extra for extra, source in REUSE.items() if source in langs and extra not in langs and extra in [h.strip().lower() for h in header])
    for lang in langs:
        if lang in [h.strip().lower() for h in header]:
            column = [h.strip().lower() for h in header].index(lang)
        else:
            header.append(lang)
            column = len(header) - 1
        for field, text in language_fields(os.path.join(LISTINGS, REUSE.get(lang, lang) + ".md")).items():
            check_limit(lang, field, text, problems)
            put(field, column, text)

    width = len(header)
    for row in rows:
        row.extend([""] * (width - len(row)))
    with open(os.path.join(out, "listings.csv"), "w", encoding="utf-8", newline="") as f:
        csv.writer(f).writerows(rows)

    print(f"{out}/listings.csv: {len(langs)} languages ({', '.join(langs)})")
    for p in problems:
        print("TOO LONG:", p)
    for field in sorted(set(unplaced)):
        print("NOT IN THE EXPORT (fill by hand or rename the section):", field)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
