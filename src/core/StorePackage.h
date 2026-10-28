#pragma once

// Round 165n (store165): LYNX packaged as MSIX for the Microsoft Store. The pure rules behind src/platform/Package.cpp
// (no JUCE, no Windows headers, so tests can check them on every OS).
//
// Inside an MSIX package Windows redirects *new* files under %APPDATA% / %LOCALAPPDATA% to a private per-user,
// per-package folder (%LOCALAPPDATA%\Packages\<family name>\LocalCache\Roaming|Local) and merges it back only for the
// package's own processes. Explorer, curl and other programs see the real disk, so a crash report LYNX "wrote to
// %APPDATA%\LYNX\crash-reports" would not be there when the user opens that folder. A packaged LYNX therefore uses the
// redirected folder's real path as its settings folder: the same place Windows would have put the files anyway (so
// stray writes to %APPDATA%\LYNX still land next to them), but a path every program can open.

#include <string>
#include <string_view>

namespace lynx::store
{
// The application id in AppxManifest.xml (<Application Id="LYNX">): "shell:AppsFolder\<family>!LYNX" starts it.
inline constexpr const char* applicationId = "LYNX";

// The settings folder of a packaged LYNX: <LocalAppData>\Packages\<family>\LocalCache\Roaming\LYNX — where Windows
// redirects %APPDATA%\LYNX for this package. Empty when either part is missing.
inline std::string packagedSettingsFolder (std::string_view localAppData, std::string_view familyName)
{
    if (localAppData.empty() || familyName.empty())
        return {};
    std::string path (localAppData);
    if (path.back() != '\\' && path.back() != '/')
        path += '\\';
    path += "Packages\\";
    path += familyName;
    path += "\\LocalCache\\Roaming\\LYNX";
    return path;
}

// The shell target that starts the packaged app (a relaunch after a language change, for instance): a packaged exe
// started by its file path would run without the package's identity.
inline std::string appsFolderTarget (std::string_view familyName)
{
    return "shell:AppsFolder\\" + std::string (familyName) + "!" + applicationId;
}

// First start of the Store build on a PC that already ran the installer / zip build: which top-level entries of the
// old %APPDATA%\LYNX are copied into the packaged settings folder. Preferences, the plugin list, favourites, presets,
// themes, layouts and the user's own plugin folder come along; caches (rebuilt on demand), autosaves, crash reports,
// session markers and logs of the other build stay where they are (a copied "session running" marker would even
// report a crash that never happened).
inline bool migratesFromUnpackaged (std::string_view name, bool isDirectory)
{
    if (name.empty() || name == "." || name == "..")
        return false;
    if (isDirectory)
        return name != "wavecache" && name != "autosave" && name != "flp-embedded" && name != "crash-reports" && name != "sessions";
    const auto endsWith = [name] (std::string_view suffix) {
        return name.size() >= suffix.size() && name.substr (name.size() - suffix.size()) == suffix;
    };
    if (endsWith (".log") || endsWith (".tmp"))
        return false;
    return name != "session-running.txt" && name != "migrated-from-appdata.txt";
}

// The marker written once the copy is done (it is never copied itself, see above).
inline constexpr const char* migrationMarker = "migrated-from-appdata.txt";
} // namespace lynx::store
