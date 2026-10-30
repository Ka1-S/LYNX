# winget manifest draft (later)

Draft manifests for submitting LYNX SHARE's **installer** (`LYNX-<version>-setup-x64.exe`, NSIS, per user) to
[microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs). Not submitted yet: winget's validation scans the
installer and an unsigned one is more likely to be held for manual review, so this waits for code signing
(docs/CODE_SIGNING.md) or for the Store listing. Once LYNX SHARE is on the Microsoft Store, `winget install LYNX` already
finds it through winget's `msstore` source without any of this.

To submit a version: fill `<VERSION>` (e.g. `0.1.20-alpha`) and `InstallerSha256` (from the release's
`SHA256SUMS.txt`), check with `winget validate --manifest <folder>` and `winget install --manifest <folder>` in a
Windows Sandbox, then open a pull request to winget-pkgs under `manifests/k/Ka1-S/LYNX/<VERSION>/`, or let
`wingetcreate update Ka1-S.LYNX --version <VERSION> --urls <installer url> --submit` do it.
