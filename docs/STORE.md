# LYNX on the Microsoft Store (MSIX)

Windows users who install LYNX from the Microsoft Store get no SmartScreen or antivirus warning: the Store signs the
package with Microsoft's certificate after it passes certification, and keeps LYNX up to date. An individual developer
account in Partner Center is free. The maintainer creates the account and does the submission clicks; everything else
is in the repository:

| What | Where |
|---|---|
| Package manifest (template) | [`resources/msix/AppxManifest.xml.in`](../resources/msix/AppxManifest.xml.in) |
| Logos and tiles (all scales, generated from `resources/lynx_badge.png`) | `resources/msix/Assets/`, made by [`scripts/make-msix-assets.py`](../scripts/make-msix-assets.py) |
| Package layout (exe + assets + filled manifest) | [`scripts/make-msix-layout.py`](../scripts/make-msix-layout.py) |
| Build, install test, release asset | job `package` in [`.github/workflows/release.yml`](../.github/workflows/release.yml) → `LYNX-<version>-store.msix` |
| Runtime differences when packaged | [`src/platform/Package.h`](../src/platform/Package.h), [`src/core/StorePackage.h`](../src/core/StorePackage.h) |
| Store listing texts, screenshots, logos | [`store/`](../store) (`listing.en.md`, `listing.pt-BR.md`, `screenshots/`) |
| Step-by-step for the maintainer (pt-BR) | [`store/CHECKLIST.pt-BR.md`](../store/CHECKLIST.pt-BR.md) |
| winget manifest draft (installer exe) | [`store/winget/`](../store/winget) |

## The package

- **Full-trust desktop app** (`Windows.FullTrustApplication`, capability `runFullTrust`): the same `LYNX.exe` as the
  installer and the zip, plus `lynx-asio.dll`, `Demo.lynx`, `LICENSE`, `THIRD_PARTY.md`, `PRIVACY.md`.
- **Windows 10 1809 (build 17763) or newer, x64.**
- **File types:** `.lynx` (LYNX project, with the project-file icon), `.flp` and `.mid`/`.midi` (offered in *Open
  with*; Windows never changes an existing default program for an MSIX app). Single audio files are not associated:
  LYNX opens projects and MIDI files from Explorer, not a lone `.wav`.
- **Microphone** device capability: Windows asks once before LYNX records from a microphone / audio interface.
- **Execution alias** `lynx.exe` (`%LOCALAPPDATA%\Microsoft\WindowsApps\lynx.exe`): command-line options such as
  `lynx --selftest r.txt` run inside the package.
- **Version:** `project(LYNX VERSION x.y.z)` → `x.y.z.0` (the Store requires the fourth part to be 0). Every
  submission needs a higher version than the last one.
- **Identity:** Partner Center › the app › *Product management › Product identity* shows `Package/Identity/Name`,
  `Package/Identity/Publisher` and `Package/Properties/PublisherDisplayName`. Copy them into the repository variables
  (GitHub › Settings › Secrets and variables › Actions › *Variables*):

  | Variable | Value from Partner Center | Placeholder until then |
  |---|---|---|
  | `MSIX_IDENTITY_NAME` | Package/Identity/Name, e.g. `12345Ka1-S.LYNX` | `LYNX.DAW` |
  | `MSIX_PUBLISHER` | Package/Identity/Publisher, e.g. `CN=1A2B3C4D-…` | `CN=LYNX` |
  | `MSIX_PUBLISHER_DISPLAY` | Package/Properties/PublisherDisplayName | `LYNX` |
  | `MSIX_DISPLAY_NAME` | the reserved app name (only if it is not exactly `LYNX`) | `LYNX` |

  A package built with the placeholders is valid but Partner Center rejects it ("identity does not match"): set the
  variables first, then run the workflow (tag push, or *Actions › Release › Run workflow*).

## What the workflow does

In the `package` job (Windows), after the self-test, the editor windows test and the installer:

1. `make-msix-layout.py` builds the layout; `makepri` indexes the scale / target-size assets into `resources.pri`
   (languages `en-US`, `pt-BR`); `makeappx pack` writes **`LYNX-<label>-store.msix`** (unsigned — the Store signs it).
   It goes into the release assets and `SHA256SUMS.txt` like every other file.
2. **Install test:** the same layout with a *test identity* (the publisher gets Windows' "unsigned package" OID
   `2.25.311729368913984317654407730594956997722`, so it installs without a certificate and can never clash with the
   Store's) is installed with `Add-AppxPackage -AllowUnsigned`, then LYNX runs **inside the package** through its
   execution alias: `--packageinfo` must report the package identity and a settings folder on the real disk (checked
   from outside the package, like Explorer would), and `--selftest` must pass. The package is removed afterwards.

## LYNX inside a package

Windows virtualizes a packaged app's writes: new files under `%APPDATA%` / `%LOCALAPPDATA%` and every `HKCU` write go to
a private per-package copy that only the package's own processes see ([Microsoft: how packaged desktop apps
run](https://learn.microsoft.com/windows/msix/desktop/desktop-to-uwp-behind-the-scenes)). LYNX detects the package
(`GetCurrentPackageFamilyName`) and changes only what that breaks:

| Area | Store build | Why |
|---|---|---|
| Settings folder (preferences, autosave, crash reports, plugin list, wave cache, presets) | `%LOCALAPPDATA%\Packages\<family>\LocalCache\Roaming\LYNX` | The real path of the redirected `%APPDATA%\LYNX`: *Open the folder with the report*, *Open autosave folder* etc. open a folder Explorer can see; anything still written to `%APPDATA%\LYNX` lands there too. Removed with the app. |
| First start | copies preferences, plugin list, favourites, presets, themes, layouts and the `Plugins` folder from an existing `%APPDATA%\LYNX` (installer build) | The user keeps their setup. Caches, autosaves, crash reports, logs and session markers are not copied (`migrated-from-appdata.txt` lists what was). |
| `.lynx` registration in `HKCU\Software\Classes` | skipped | The manifest declares the file types; HKCU writes would only reach the package's private registry. |
| Relaunch through the 8.3 short path (folders with accents) | skipped | `WindowsApps` paths are ASCII; a packaged exe started by its path would lose the package identity. |
| Restart after a language change | `shell:AppsFolder\<family>!LYNX` | Starts the app through the package. |
| LYNX ASIO for other programs | not offered (the hint says to use the installer build) | The package folder changes with every update, so a registered driver path would break. LYNX itself uses LYNX ASIO as usual. |
| Updates | the Store | LYNX has no self-updater on Windows; nothing to disable. |

Unchanged: plugin scanning (VST3 / CLAP / VST2 in Common Files, user folders; the `--scan-one` child process and the
plugin sandbox run inside the package too), WASAPI / ASIO / DirectSound, FL Studio detection (reads only), drag & drop,
opening files from Explorer, the opt-in crash report. A plugin that writes its own licence or presets under
`%APPDATA%` while running inside LYNX may have them stored in the package's private copy (Windows' rule for packaged
hosts); files that already exist there are read and updated in place.

For a test on a PC without the package, `LYNX_SIMULATE_PACKAGE_FAMILY=<family name>` makes LYNX behave as if packaged
(`LYNX.exe --packageinfo out.txt` writes what it sees).

### Testing an installed package locally

Installing an unsigned package needs an elevated PowerShell, and registering a loose folder needs Developer Mode
(*Settings › System › For developers*). With Developer Mode on:

```powershell
python scripts\make-msix-layout.py --version 0.1.20 --bin build-win --out msix-test --test-identity --loose --extra LICENSE
Add-AppxPackage -Register msix-test\AppxManifest.xml   # (loose files: no makepri/makeappx needed)
lynx --packageinfo $PWD\packageinfo.txt; Get-Content packageinfo.txt
Get-AppxPackage LYNX.DAW | Remove-AppxPackage
```

## Store policy check

- **Open source (AGPL-3.0)** is allowed; the listing links the source and licence (*Additional license terms*).
- **Third-party plugin hosting** is allowed: LYNX loads VST3 / CLAP / VST2 the user installed themself; it downloads
  no code (Store policy 10.2.2: no dynamic code from the network).
- **No surprises:** no telemetry, ads, accounts or purchases; the only network use is the crash report, **opt-in**
  (asked after a crash; *Settings › General › Crash reports*). Say so in the privacy questions.
- **`runFullTrust`** is a restricted capability; the submission asks why. Answer: *"LYNX is a desktop digital audio
  workstation (Win32/JUCE). It needs full trust to host the user's VST3/CLAP audio plugins, open ASIO/WASAPI audio
  drivers with low latency and read and write the user's project and sample files anywhere on disk."*
- **Names of other products** (FL Studio, plugin vendors) appear only as compatibility facts with the trademark notice;
  they are **not** used as search keywords (Store policy 10.1).

## Updating the Store version

1. Release as usual (`docs/RELEASE.md`); the workflow attaches `LYNX-<version>-store.msix`.
2. Partner Center › LYNX › *Start update* → *Packages*: upload the new `.msix` (and remove the old one) → *What's new*
   from the release notes → *Submit to the Store*. Certification usually takes 1–3 working days.
