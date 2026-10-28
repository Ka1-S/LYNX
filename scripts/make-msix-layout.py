#!/usr/bin/env python3
"""Builds the MSIX package layout (the folder `makeappx pack /d` packs) for the Microsoft Store build (round 165n).

Usage:
  scripts/make-msix-layout.py --version 0.1.20[-alpha] --bin <folder with LYNX.exe> --out <layout folder>
                              [--identity-name N] [--publisher P] [--publisher-display D] [--display-name L]
                              [--test-identity]
                              [--extra file ...]

The identity defaults come from the environment (MSIX_IDENTITY_NAME, MSIX_PUBLISHER, MSIX_PUBLISHER_DISPLAY: the
repository variables, copied from Partner Center › Product identity) and fall back to the same values; MSIX_DISPLAY_NAME
is the app name reserved in Partner Center (default "LYNX SHARE"; Start menu and the app itself say LYNX). --test-identity
appends Windows' "unsigned package" OID to the publisher, so `Add-AppxPackage -AllowUnsigned` installs the package
for a test (it can then never clash with the Store-signed one). See docs/STORE.md.
"""
import argparse
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# LYNX's identity in Partner Center (Product identity; family name KA1WARE.LYNXSHARE_sj8hjwabzpdr8, Store ID
# 9NQRFBFDCX6N): the defaults, so a local package matches the release workflow's (whose repository variables hold
# the same values)
DEFAULT_NAME = "KA1WARE.LYNXSHARE"
DEFAULT_PUBLISHER = "CN=BACBB786-A595-4AC3-9807-A70EAA268E4C"
DEFAULT_DISPLAY = "KA1WARE"
UNSIGNED_OID = "OID.2.25.311729368913984317654407730594956997722=1"
APP_FILES = ("LYNX.exe", "lynx-asio.dll")  # (lynx-asio32.dll is for registering with 32-bit hosts: not in the package)


def msix_version(version):
    """'0.1.20' / '0.1.20-alpha' -> '0.1.20.0' (the Store wants the fourth part 0; each part 0..65535)."""
    m = re.match(r"^(\d+)\.(\d+)\.(\d+)(?:[-+].*)?$", version.strip())
    if not m:
        raise ValueError(f"not a version: {version!r}")
    parts = [int(p) for p in m.groups()]
    if any(p > 65535 for p in parts):
        raise ValueError(f"version part above 65535: {version!r}")
    return ".".join(str(p) for p in parts) + ".0"


def xml_escape(text):
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--bin", required=True, help="folder with LYNX.exe (and lynx-asio.dll)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--identity-name", default=os.environ.get("MSIX_IDENTITY_NAME") or DEFAULT_NAME)
    ap.add_argument("--publisher", default=os.environ.get("MSIX_PUBLISHER") or DEFAULT_PUBLISHER)
    ap.add_argument("--publisher-display", default=os.environ.get("MSIX_PUBLISHER_DISPLAY") or DEFAULT_DISPLAY)
    ap.add_argument("--display-name", default=os.environ.get("MSIX_DISPLAY_NAME") or "LYNX SHARE")
    ap.add_argument("--test-identity", action="store_true")
    ap.add_argument("--loose", action="store_true",
                    help="also write unqualified logo names, for Add-AppxPackage -Register without makepri's resources.pri")
    ap.add_argument("--extra", nargs="*", default=[], help="more files for the package root (Demo.lynx, LICENSE...)")
    a = ap.parse_args(argv)

    if not re.match(r"^[A-Za-z0-9.\-]{3,50}$", a.identity_name):
        raise SystemExit(f"bad identity name {a.identity_name!r} (3-50 of A-Z a-z 0-9 . -)")
    if not a.publisher.startswith("CN="):
        raise SystemExit(f"publisher must be a distinguished name starting with CN=: {a.publisher!r}")
    publisher = a.publisher + (", " + UNSIGNED_OID if a.test_identity and UNSIGNED_OID not in a.publisher else "")

    if os.path.exists(a.out):
        shutil.rmtree(a.out)
    os.makedirs(a.out)
    for name in APP_FILES:
        src = os.path.join(a.bin, name)
        if os.path.exists(src):
            shutil.copy2(src, a.out)
        elif name == "LYNX.exe":
            raise SystemExit(f"missing {src}")
    for extra in a.extra:
        shutil.copy2(extra, a.out)
    assets = os.path.join(a.out, "Assets")
    shutil.copytree(os.path.join(ROOT, "resources", "msix", "Assets"), assets)
    if a.loose:
        for base in ("Square44x44Logo", "Square150x150Logo", "Wide310x150Logo", "SmallTile", "LargeTile", "StoreLogo", "ProjectFile"):
            shutil.copy2(os.path.join(assets, base + ".scale-100.png"), os.path.join(assets, base + ".png"))

    with open(os.path.join(ROOT, "resources", "msix", "AppxManifest.xml.in"), encoding="utf-8") as f:
        manifest = f.read()
    fields = {
        "@IDENTITY_NAME@": a.identity_name,
        "@PUBLISHER@": publisher,
        "@PUBLISHER_DISPLAY@": a.publisher_display,
        "@VERSION@": msix_version(a.version),
        "@DISPLAY_NAME@": a.display_name,
    }
    for key, value in fields.items():
        manifest = manifest.replace(key, xml_escape(value))
    if "@" in re.sub(r"<!--.*?-->", "", manifest, flags=re.S):
        raise SystemExit("unfilled field left in the manifest")
    with open(os.path.join(a.out, "AppxManifest.xml"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(manifest)
    print(f"layout {a.out}: {a.identity_name} {fields['@VERSION@']} publisher={publisher!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
