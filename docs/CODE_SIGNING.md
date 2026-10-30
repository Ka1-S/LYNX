# Code signing (SignPath Foundation)

Windows SmartScreen warns about `LYNX.exe` because it is not signed. The
[SignPath Foundation](https://signpath.org) signs open-source software for free, with a certificate issued to
"SignPath Foundation", as long as the project follows their [terms](https://signpath.org/terms). This page lists what
the repository already does for that and what the maintainer has to do by hand.

## What the repository already provides

| Requirement | Where |
|---|---|
| OSI-approved licence for LYNX and every component, no proprietary code | [`LICENSE`](../LICENSE) (AGPL-3.0), [`THIRD_PARTY.md`](../THIRD_PARTY.md) |
| Binaries built automatically from the public source | [`.github/workflows/release.yml`](../.github/workflows/release.yml): tag push `v*` or manual run |
| Product name and version in the exe | `VERSIONINFO` in [`resources/LYNX.rc`](../resources/LYNX.rc); version from `project(LYNX VERSION ...)` in `CMakeLists.txt` |
| Code signing policy on the README and website | [README](../README.md#code-signing-policy), lynx-share.com (section *Code signing & privacy*) |
| Privacy statement | [`PRIVACY.md`](../PRIVACY.md) |
| SignPath step in CI, off until approved | job `sign` in `release.yml`, switched on by the repository variable `SIGNPATH_ENABLED` |

The release workflow: `build` (Ubuntu) cross-compiles `LYNX.exe` with Zig 0.14.1 and JUCE 8.0.4 (both downloaded and
checked by SHA-256) → `sign` (SignPath, when enabled) → `package` (Windows) checks the version resource and the
signature, runs `LYNX.exe --selftest`, writes `Demo.lynx`, builds `LYNX-<version>-windows-x64.zip` and
`SHA256SUMS.txt`, and on a tag push attaches them to the GitHub release (creating a draft release if none exists).

## Step by step for the maintainer

### 1. Turn on two-factor authentication

SignPath requires MFA for everyone with commit or approval rights.

1. GitHub → *Settings* → *Password and authentication* → *Enable two-factor authentication* (authenticator app or
   security key). Save the recovery codes.
2. Optional but recommended: *Settings* → *Branches* on the repo, protect `main` (no force-push, no deletion).

### 2. Apply

1. Open <https://signpath.org/apply> (linked from <https://signpath.org>) and fill in the form. Suggested answers:
   - **Project name:** LYNX
   - **Repository:** https://github.com/Ka1-S/LYNX-SHARE
   - **Homepage / download page:** https://lynx-share.com and https://github.com/Ka1-S/LYNX-SHARE/releases
   - **Licence:** GNU AGPL-3.0 (OSI approved); components listed in `THIRD_PARTY.md`
   - **Description:** a free, open-source digital audio workstation for Windows (C++/JUCE) with a pattern-based
     workflow; it opens `.flp` project files.
   - **Build system:** GitHub Actions, workflow `.github/workflows/release.yml`, GitHub-hosted runners
   - **Artifacts to sign:** `LYNX.exe` (64-bit PE); later also the installer, if one is added
   - **Code signing policy URL:** https://github.com/Ka1-S/LYNX-SHARE#code-signing-policy
   - **Privacy policy URL:** https://github.com/Ka1-S/LYNX-SHARE/blob/main/PRIVACY.md
   - **Team / roles:** Ka1-S is author, reviewer and approver
   - **Maturity:** public releases since 0.1.0 alpha (October 2026)
2. Wait for their e-mail. They may ask questions or request changes; the README and website already say
   "pending approval".

### 3. After approval: set up SignPath

SignPath creates an organization for LYNX and invites you to <https://app.signpath.io>.

1. Log in, enable MFA on the SignPath account too.
2. **Trusted build system:** link GitHub.com (*Trusted Build Systems* → GitHub.com) and install the SignPath GitHub
   app on `Ka1-S/LYNX-SHARE` when asked. This lets SignPath verify that the artifact came from this repository's workflow.
3. **Project:** create (or check) the project with slug `LYNX`, repository `https://github.com/Ka1-S/LYNX-SHARE`.
4. **Artifact configuration** (the GitHub artifact `LYNX-unsigned` is a zip containing `LYNX.exe`):

   ```xml
   <?xml version="1.0" encoding="utf-8"?>
   <artifact-configuration xmlns="http://signpath.io/artifact-configuration/v1">
     <zip-file>
       <pe-file path="LYNX.exe">
         <authenticode-sign/>
       </pe-file>
     </zip-file>
   </artifact-configuration>
   ```
   SignPath can also check the exe's product name/version here; see their artifact configuration docs.
5. **Signing policy:** slug `release-signing`, certificate from SignPath Foundation, approver `Ka1-S`, and limit it
   to the `main` branch / `v*` tags if offered.
6. **CI user:** *Users* → add a CI user with the *Submitter* role on the project, create an API token.

### 4. Connect the GitHub repository

GitHub → repo *Settings* → *Secrets and variables* → *Actions*:

| Kind | Name | Value |
|---|---|---|
| Secret | `SIGNPATH_API_TOKEN` | the CI user's API token |
| Variable | `SIGNPATH_ORGANIZATION_ID` | organization id (SignPath → *Settings*, a GUID) |
| Variable | `SIGNPATH_PROJECT_SLUG` | `LYNX` (only needed if different) |
| Variable | `SIGNPATH_POLICY_SLUG` | `release-signing` (only needed if different) |
| Variable | `SIGNPATH_ENABLED` | `true` |

Then update the wording: in `README.md` (section *Code signing policy*) and `website/index.html` (`#signing`,
English and `PT` strings) remove "pending approval", and drop the SmartScreen note from the *Download* section.

### 5. Every release

1. Bump `project(LYNX VERSION x.y.z)` in `CMakeLists.txt` (the only version source), commit, then
   `git tag vX.Y.Z-alpha && git push origin vX.Y.Z-alpha`.
2. The `Release` workflow starts; the `sign` job submits a signing request and waits.
3. Open the request in <https://app.signpath.io> (you also get an e-mail), check that it comes from the expected
   commit and workflow run, and click **Approve** within 3 hours (otherwise the job times out; re-run it from the Actions tab).
4. The `package` job self-tests the signed exe and attaches `LYNX.exe`, the zip and `SHA256SUMS.txt` to the release
   (a draft is created if the release does not exist yet). Edit the notes and publish.

Only binaries from this workflow may be signed; never upload a locally built exe for signing.

## Things to keep in mind

- SignPath signs only what the project builds itself. Do not bundle other people's binaries (plugins, DLLs) in a
  signed release.
- The FL Studio behaviour reproduced in LYNX is documented clean-room work (`docs/analise/`), with no Image-Line code
  or content. If SignPath asks about it, point them to that folder and to the *Credits and notice* section of the
  README.
- If an installer is added (`scripts/make-installer.sh`), add it to the `build`/`package` jobs and to the artifact
  configuration so it is signed too.
