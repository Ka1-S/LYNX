#pragma once

// In-app update check (round 166b), the logic without JUCE (tested in isolation; the app side is ui/Updater.h):
// - the GitHub releases list of Ka1-S/LYNX-SHARE (JSON) parsed into releases, drafts skipped, prereleases included;
// - semantic-version comparison of a release tag ("v0.1.21-alpha") with the running version (CMake's "0.1.20");
// - the build flavour LYNX runs as (installer, portable zip, Store, macOS, AppImage, tar.gz), the update method and
//   the release asset that flavour uses;
// - the release's SHA256SUMS.txt line for a file;
// - the release notes (Markdown) as readable lines;
// - the settings file (automatic check on/off, the skipped version, the time of the last check).

#include <cstdint>
#include <string>
#include <vector>

namespace lynx::update
{
// --- versions ---------------------------------------------------------------------------------------------------
struct Version
{
    int major = 0, minor = 0, patch = 0;
    std::vector<std::string> pre; // "alpha", "1" for "-alpha.1"; empty for a release
    bool valid = false;
    std::string text() const;     // "0.1.21-alpha"
};

// "v0.1.21-alpha", "0.1.21", "0.1" (patch 0), "V1.2.3-rc.1+build.5" (build metadata ignored); invalid otherwise.
Version parseVersion (const std::string& text);

// <0, 0, >0 like strcmp, by semver 2.0 precedence: numbers first, then a prerelease sorts before the release of the
// same numbers ("0.1.21-alpha" < "0.1.21"), prerelease identifiers compared one by one (numeric ones as numbers,
// below alphanumeric ones; a shorter list sorts first when equal so far).
int compare (const Version& a, const Version& b);

// --- releases ---------------------------------------------------------------------------------------------------
struct Asset
{
    std::string name, url; // url = browser_download_url
    std::int64_t size = 0;
};

struct Release
{
    std::string tag, name, body, page; // page = html_url
    bool draft = false, prerelease = false;
    std::vector<Asset> assets;
    Version version; // from the tag
};

// The GitHub API's answer for /repos/<o>/<r>/releases (an array) or /releases/<id> (one object). Releases whose tag
// is not a version are dropped. `error` (optional) gets the reason when the text is not that JSON.
std::vector<Release> parseReleases (const std::string& json, std::string* error = nullptr);

// The newest non-draft release newer than `running`, or -1.
int newestIndex (const std::vector<Release>& releases, const Version& running);

// --- build flavours ---------------------------------------------------------------------------------------------
enum class Flavour
{
    windowsInstaller, // %LOCALAPPDATA%\Programs\LYNX with the NSIS installer's Uninstall.exe beside LYNX.exe
    windowsPortable,  // the zip (or the bare LYNX.exe)
    store,            // the Microsoft Store (MSIX) package: the Store updates it; LYNX never checks
    macos,
    linuxAppImage,
    linuxTarball,
    unknown
};

struct Facts
{
    std::string os;              // "windows", "macos", "linux"
    bool packaged = false;       // Windows: a package identity (MSIX)
    bool uninstallerBesideExe = false;
    std::string appImagePath;    // Linux: $APPIMAGE when it names an existing file
    std::string forced;          // LYNX_UPDATE_FLAVOUR (tests): a flavourId() name wins over the facts
};

Flavour detectFlavour (const Facts& facts);
const char* flavourId (Flavour); // "installer", "portable", "store", "macos", "appimage", "tarball", "unknown"
Flavour flavourFromId (const std::string& id); // unknown for anything else

// Whether that build looks for updates at all (the Store build never does).
bool checksForUpdates (Flavour);

enum class Method
{
    runInstaller,       // download setup.exe, verify, save the project, run it silently over the install, relaunch
    downloadAndReveal,  // download the zip / dmg to Downloads, verify, show it in the file manager
    openPage,           // open the release page in the browser
    appImageUpdate,     // appimageupdatetool (zsync) in place, else download the new AppImage beside the old one
    none
};
Method methodFor (Flavour);

// The asset a flavour uses (index into `assets`), or -1: the installer "*-setup-x64.exe", the portable
// "*-windows-x64.zip", macOS "*.dmg", the AppImage "*-x86_64.AppImage", the tarball "*-linux-x86_64.tar.gz".
int assetFor (Flavour, const std::vector<Asset>& assets);
int checksumsAsset (const std::vector<Asset>& assets); // "SHA256SUMS.txt", or -1

// --- checksums --------------------------------------------------------------------------------------------------
// The lower-case SHA-256 hex of `fileName` in a `sha256sum` listing ("<hex>  <name>" or "<hex> *<name>"), else "".
std::string checksumFor (const std::string& sumsText, const std::string& fileName);
// SHA-256 over a file read in chunks (a 60 MB AppImage is not held in memory twice); same result as sha256Hex.
class Sha256Stream
{
public:
    Sha256Stream();
    void add (const void* data, std::size_t size);
    std::string finish(); // lower-case hex; the object is spent afterwards
private:
    void block (const std::uint8_t* p);
    std::uint32_t h[8];
    std::uint8_t pending[64];
    std::size_t pendingSize = 0;
    std::uint64_t total = 0;
};

// `actual` (any case) equals `expected` and both are 64 hex digits.
bool checksumMatches (const std::string& expected, const std::string& actual);

// --- release notes ----------------------------------------------------------------------------------------------
struct NoteLine
{
    enum class Kind { heading, bullet, text, blank } kind = Kind::text;
    int level = 0;    // heading: 1..6; bullet: the nesting depth (0 = top)
    std::string text; // without the Markdown marks: links keep their text, images, HTML tags and ** ` go
};
// Markdown as readable lines (headings, bullets, paragraphs; at most one blank line in a row; code fences kept as
// plain text).
std::vector<NoteLine> releaseNotes (const std::string& markdown);
std::string releaseNotesText (const std::vector<NoteLine>&); // the same as plain text ("• " bullets)

// --- settings (updates.txt in the settings folder) --------------------------------------------------------------
struct Settings
{
    bool automatic = true;      // Settings › General › "Check for updates automatically"
    std::string skipped;        // the version the user skipped (✕ on the bar): that one and older stay quiet
    std::int64_t lastCheckMs = 0; // when the release list was last fetched (the cache's age)
};
Settings parseSettings (const std::string& text);
std::string formatSettings (const Settings&);

// Whether the bar shows `release`: a manual check always, an automatic one unless that version (or a newer one)
// was skipped.
bool shouldShow (const Release& release, const Settings& settings, bool manual);

// An automatic check reuses the cached release list for this long (GitHub allows 60 unauthenticated calls an hour
// per address; LYNX started many times a day asks once every few hours).
constexpr std::int64_t cacheLifetimeMs = 3LL * 60 * 60 * 1000;
bool cacheFresh (std::int64_t lastCheckMs, std::int64_t nowMs);

// The GitHub API address of the release list (prereleases included; drafts are only listed for the repository's
// own token, and dropped anyway).
constexpr const char* releasesUrl = "https://api.github.com/repos/Ka1-S/LYNX-SHARE/releases?per_page=10";
constexpr const char* releasesPage = "https://github.com/Ka1-S/LYNX-SHARE/releases";
} // namespace lynx::update
