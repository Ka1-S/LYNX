#pragma once

// Round 165n (store165): LYNX running from an MSIX package (the Microsoft Store build, docs/STORE.md). The same
// LYNX.exe ships in the installer, the zip and the Store package; it branches on "running packaged" only where the
// package changes what is right:
// - settings folder: the real path of the package's redirected %APPDATA% (core/StorePackage.h explains why), with a
//   one-time copy of the installer build's preferences;
// - no per-user file-type registration (the package manifest declares .lynx, .flp, .mid and audio files);
// - no relaunch through the 8.3 short path (WindowsApps paths are plain ASCII);
// - a restart goes through the package (shell:AppsFolder) so it keeps the package identity;
// - LYNX ASIO is not offered for registration with other programs (the package folder changes with every update).
// Updates come from the Store; LYNX has no self-updater on Windows.

#include <juce_core/juce_core.h>

namespace lynx::platform
{
// True when this process runs with a package identity (GetCurrentPackageFamilyName succeeds). For tests on a PC
// without the package, LYNX_SIMULATE_PACKAGE_FAMILY=<family name> makes LYNX behave as if it were packaged.
bool runningPackaged();

// The package family name ("LYNX.LYNX_8wekyb3d8bbwe"); empty when not packaged.
juce::String packageFamilyName();

// The full package name (with version and architecture); empty when not packaged or simulated.
juce::String packageFullName();

// The settings folder of a packaged LYNX (created, migrated on first use); an empty File when not packaged.
juce::File packagedSettingsFolder();

// Starts LYNX again (after a language change): through the package when packaged, else this exe. False when it
// could not be started.
bool relaunchApp();

// A text report for `LYNX.exe --packageinfo <out.txt>`: identity, folders, and a write/read check of the settings
// folder on the real disk (what Explorer sees). "result=OK" on the last line when everything checks out.
juce::String packageReport();
} // namespace lynx::platform
