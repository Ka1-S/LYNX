# Round 165n (store165) — LYNX on the Microsoft Store (MSIX)

User (2026-10-27): "monta aí o Microsoft, é de graça". Goal: Windows users install LYNX from the Microsoft Store
(free individual developer account; the Store signs the MSIX after certification), so no SmartScreen / antivirus
warnings. The user creates the Partner Center account and submits; this round prepares everything else.

Evidence markers: **[DOC]** Microsoft Learn (MSIX docs), **[CODE]** LYNX source, **[TEST]** test run (local or CI),
**[REG]** registry/OS state read on this PC.

## Where I stopped

- 19:05 started: briefs read, worktree ../lynx-wt-store165 on feature/store165 (from main 7367aca4).
- Developer Mode is OFF on this PC [REG: HKLM\...\AppModelUnlock has no AllowDevelopmentWithoutDevLicense]; an
  unsigned MSIX needs an elevated PowerShell [DOC unsigned-package] → no local install possible without UAC / a
  setting change. Plan: real install test on the GitHub Windows runner (admin, -AllowUnsigned with the test OID
  publisher), a local `--packageinfo` check with a simulated package identity, and ask the user (via the coordinator)
  about Developer Mode.
