#pragma once

// Crash / freeze reports (round 159). After LYNX closed unexpectedly (a crash, a freeze the user ended, a fail-fast),
// the next start can send one report to lynx-share.com — only with the user's consent (Settings › General: Ask /
// Always send / Never) — or open GitHub's bug form prefilled. This part is the platform-independent logic: consent,
// anonymising, the report JSON (schema 1, website/api/crash.php checks it) and the GitHub URL. Collecting the system
// facts, the dialog and the upload are in ui/CrashReporter.cpp.
//
// What leaves the computer is exactly buildJson()'s text (the dialog's "Show what will be sent" shows it): no audio,
// no projects, no machine name, no address; user names in paths become <user>, file names of projects / audio become
// <file>.<extension> unless the user ticks "include file names".

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lynx::crashreport
{
// ---- consent -----------------------------------------------------------------------------------------------------
enum class Consent
{
    ask,    // default: the start-up dialog asks every time
    always, // send without asking
    never   // never send, never ask (the plain "closed unexpectedly" notice only)
};
Consent parseConsent (std::string_view text); // "ask" / "always" / "never"; anything else = ask
const char* consentName (Consent);

enum class Action
{
    askUser,
    sendSilently,
    dontSend
};
// What the start after an unclean end does. A report is never sent without consent: only `always` sends unasked.
Action actionFor (Consent);

// ---- anonymising -------------------------------------------------------------------------------------------------
struct Anonymiser
{
    std::vector<std::string> userNames;    // the logon name, the full name (each word of 3+ letters too)
    std::vector<std::string> homeDirs;     // the home folder(s), any slash style
    std::vector<std::string> machineNames; // the computer's name
    bool keepFileNames = false;            // the user ticked "include file names"

    // Every path's user folder -> <user> (C:\Users\<user>, /home/<user>, /Users/<user>, also other accounts'),
    // the known names -> <user> / <machine>, UNC servers -> \\<host>, OneDrive for Business' organisation -> <org>;
    // without keepFileNames, project / audio / archive file names -> <file>.<ext> and the folders of their paths ->
    // <dir> (common folders such as Desktop, Documents, Music, OneDrive, Área de Trabalho stay).
    std::string apply (std::string_view text) const;
};
// project / audio / MIDI / archive extensions whose file names are hidden ("flp", "wav", ...; lower case, no dot)
bool isContentExtension (std::string_view extension);

// ---- the report --------------------------------------------------------------------------------------------------
struct Gpu
{
    std::string name, driver;
};
struct Screen
{
    int width = 0, height = 0;
    double scale = 1.0;
};
struct PluginInfo
{
    std::string name, vendor, format, version;
};

struct ReportInput
{
    std::string id;           // newId()-style UUID v4
    std::string kind;         // "crash", "hang", "unclean" (ended without a crash file) or "manual"
    bool test = false;        // a test report (the server keeps it apart)
    std::string created;      // ISO 8601 time
    std::string version, build, commit, language;
    std::string os, osVersion, cpu;
    int cpuCores = 0, memoryMb = 0;
    std::vector<Gpu> gpus;
    std::vector<Screen> screens;
    std::string audioDriver, audioDevice;
    double sampleRate = 0.0;
    int bufferSize = 0;
    std::string crashFile, crashText; // file name and contents of autosave\crash *.txt
    std::string hangFile, hangText;   // ... of autosave\hang *.txt
    std::vector<std::string> steps;   // the session's last steps, oldest first
    std::vector<PluginInfo> plugins;  // third-party plugins loaded in that session
    std::vector<std::pair<std::string, bool>> settings; // gpuRendering, pluginSandbox, ...
    std::string userText, email;      // the manual report's description / optional contact
};

constexpr std::size_t maxSteps = 60;
constexpr std::size_t defaultMaxBytes = 512 * 1024; // the JSON (the server takes up to 4 MB raw / 1 MB gzip)

// The report JSON (UTF-8, invalid sequences repaired), every text through `anonymiser`, at most `maxBytes`: when
// larger, the crash / hang texts are shortened first (head and tail kept), then the steps, plugins and user text;
// what was cut is listed in "notes".
std::string buildJson (const ReportInput& input, const Anonymiser& anonymiser, std::size_t maxBytes = defaultMaxBytes);

std::string jsonEscape (std::string_view utf8);     // the quoted JSON string
std::string repairUtf8 (std::string_view text);     // invalid bytes -> U+FFFD
std::string truncateUtf8 (std::string_view text, std::size_t maxBytes); // never cuts a character
bool validId (std::string_view id);                 // lower-case UUID v4 (what the server accepts)
std::string formatUuid (const std::uint8_t (&bytes)[16]); // the v4 UUID text of 16 random bytes (version bits set)

// ---- GitHub ------------------------------------------------------------------------------------------------------
// The fields of .github/ISSUE_TEMPLATE/bug_report.yml (ids version, os, os-details, audio, kind, steps, expected,
// plugins, logs). `os` and `kind` must be one of the form's dropdown options to be preselected.
struct IssueFields
{
    std::string title, version, os, osDetails, audio, kind, steps, expected, plugins, logs;
};
constexpr const char* issueFormBase = "https://github.com/Ka1-S/LYNX-SHARE/issues/new";
constexpr std::size_t defaultMaxUrl = 8000;

// The new-issue URL with the form prefilled; at most `maxLength` bytes: the longest fields (logs, steps, plugins)
// are shortened with "…" (never inside a %-escape or a character).
std::string githubIssueUrl (const IssueFields& fields, std::size_t maxLength = defaultMaxUrl);
std::string percentEncode (std::string_view utf8); // RFC 3986 unreserved characters stay
// The form's dropdown option for an OS name ("Windows 11", "macOS (Apple silicon)", "Linux (AppImage)"...; "" when unknown)
std::string githubOsOption (std::string_view osName, std::string_view arch = {}, bool appImage = false);
// "crash" -> "LYNX closed (crash)", "hang" -> "LYNX froze (not responding)", else "Other"
std::string githubKindOption (std::string_view kind);
} // namespace lynx::crashreport
