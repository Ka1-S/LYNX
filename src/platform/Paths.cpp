#include "platform/Paths.h"

#include "core/FlpImport.h"
#include "core/TestProfile.h"

#include <iterator>
#include <string>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace lynx::platform
{
namespace
{
juce::File home() { return juce::File::getSpecialLocation (juce::File::userHomeDirectory); }

#if JUCE_LINUX || JUCE_BSD
// An XDG base directory: the variable when it holds an absolute path, else the default under the home folder.
juce::File xdgFolder (const char* variable, const char* fallback)
{
    const auto value = juce::SystemStats::getEnvironmentVariable (variable, {});
    if (value.isNotEmpty() && juce::File::isAbsolutePath (value))
        return juce::File (value);
    return home().getChildFile (fallback);
}
#endif
} // namespace

namespace
{
// (JUCE's reader: the wide environment on Windows, so a non-ASCII folder survives; "" when unset)
std::string envValue (const char* name)
{
    return juce::SystemStats::getEnvironmentVariable (name, {}).toStdString();
}
} // namespace

juce::File profileOverride()
{
    static const juce::File folder = [] {
        const auto dir = testprofile::profileDirFrom (envValue ("LYNX_PROFILE_DIR").c_str());
        if (! dir)
            return juce::File();
        juce::File f (juce::String::fromUTF8 (dir->c_str()));
        f.createDirectory();
        return f;
    }();
    return folder;
}

bool testMode()
{
    static const bool on = testprofile::testModeFrom (envValue ("LYNX_TEST_MODE").c_str());
    return on;
}

void testModeLog (const juce::String& what)
{
    settingsFile ("test-mode.log").appendText (juce::Time::getCurrentTime().toISO8601 (true) + " | skipped: " + what + "\n");
}

juce::File documentsFolder()
{
    if (const auto profile = profileOverride(); profile != juce::File())
        return profile.getChildFile ("Documents");
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
}

juce::File settingsFolder()
{
    if (const auto profile = profileOverride(); profile != juce::File())
    {
        auto folder = profile.getChildFile ("LYNX");
        folder.createDirectory();
        return folder;
    }
   #if JUCE_WINDOWS
    auto folder = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("LYNX");
   #elif JUCE_MAC
    auto folder = home().getChildFile ("Library/Application Support/LYNX");
   #else
    auto folder = xdgFolder ("XDG_CONFIG_HOME", ".config").getChildFile ("LYNX");
   #endif
    folder.createDirectory();
    return folder;
}

juce::File settingsFile (const juce::String& relativePath)
{
    return settingsFolder().getChildFile (relativePath);
}

juce::File cacheFolder (const juce::String& name)
{
    if (const auto profile = profileOverride(); profile != juce::File())
        return profile.getChildFile ("cache").getChildFile (name);
   #if JUCE_WINDOWS
    return settingsFolder().getChildFile (name);
   #elif JUCE_MAC
    return home().getChildFile ("Library/Caches/LYNX").getChildFile (name);
   #else
    return xdgFolder ("XDG_CACHE_HOME", ".cache").getChildFile ("LYNX").getChildFile (name);
   #endif
}

juce::FileSearchPath standardPluginFolders()
{
    juce::FileSearchPath paths;
   #if JUCE_WINDOWS
    // the locations Steinberg / the CLAP spec / common installers use
    const auto programs = juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory);
    paths.add (programs.getChildFile ("Common Files/VST3"));
    for (const auto* sub : { "VSTPlugins", "Steinberg/VSTPlugins", "Common Files/VST2", "Common Files/Steinberg/VST2" })
        paths.add (programs.getChildFile (sub));
    paths.add (programs.getChildFile ("Common Files/CLAP"));
    paths.add (juce::File::getSpecialLocation (juce::File::windowsLocalAppData).getChildFile ("Programs/Common/CLAP"));
   #elif JUCE_MAC
    // user folders first, then the system-wide ones
    for (const auto& root : { home().getChildFile ("Library/Audio/Plug-Ins"), juce::File ("/Library/Audio/Plug-Ins") })
        for (const auto* sub : { "VST3", "Components", "CLAP", "VST" })
            paths.add (root.getChildFile (sub));
   #else
    // VST3 and CLAP specs' Linux folders, LV2's usual ones and the de-facto VST2 ones; the lib64 variants are where
    // Fedora, openSUSE and other 64-bit-libdir distributions install them
    for (const auto* format : { "vst3", "clap", "lv2", "vst" })
    {
        paths.add (home().getChildFile (juce::String (".") + format));
        for (const auto* root : { "/usr/lib", "/usr/local/lib" })
            paths.add (juce::File (root).getChildFile (format));
        // only where they exist and are not a link to /usr/lib (Arch), which would list every plugin twice
        for (const auto* root : { "/usr/lib64", "/usr/local/lib64" })
            if (const juce::File lib (root); lib.isDirectory() && ! lib.isSymbolicLink() && lib.getChildFile (format).isDirectory()
                && ! lib.getChildFile (format).isSymbolicLink())
                paths.add (lib.getChildFile (format));
    }
   #endif
    paths.add (settingsFolder().getChildFile ("Plugins"));
    return paths;
}

juce::String pluginFormatNames()
{
   #if JUCE_WINDOWS
    return "VST, VST3, CLAP";
   #elif JUCE_MAC
    return "VST3, AU, CLAP";
   #else
    return "VST, VST3, CLAP, LV2";
   #endif
}

juce::File localFile (const juce::String& path)
{
   #if JUCE_WINDOWS
    return juce::File (path);
   #else
    if (! path.containsChar ('\\') && juce::File::isAbsolutePath (path) && juce::File (path).exists())
        return juce::File (path);
    const auto posix = path.replaceCharacter ('\\', '/');
    if (! juce::File::isAbsolutePath (posix))
        return {};
    return juce::File (juce::String::fromUTF8 (existingPathIgnoringCase (posix.toStdString()).c_str()));
   #endif
}

juce::File localFile (const std::string& utf8Path)
{
    return localFile (juce::String::fromUTF8 (utf8Path.c_str()));
}

bool relaunchFromAsciiPath (int& exitCode)
{
   #if JUCE_WINDOWS
    wchar_t exe[MAX_PATH * 4] {};
    const auto length = GetModuleFileNameW (nullptr, exe, (DWORD) std::size (exe));
    if (length == 0 || length >= std::size (exe))
        return false;

    const auto isAscii = [] (const wchar_t* text) { for (; *text != 0; ++text) if (*text > 127) return false; return true; };
    if (isAscii (exe) || GetEnvironmentVariableW (L"LYNX_SHORT_PATH_RELAUNCH", nullptr, 0) > 0)
        return false;

    wchar_t shortExe[MAX_PATH * 4] {};
    const auto shortLength = GetShortPathNameW (exe, shortExe, (DWORD) std::size (shortExe));
    if (shortLength == 0 || shortLength >= std::size (shortExe) || ! isAscii (shortExe))
        return false; // 8.3 names disabled on this volume: nothing to do

    // the same arguments: the original command line without its first token (the program, quoted or not)
    const wchar_t* rest = GetCommandLineW();
    if (*rest == L'"')
    {
        for (++rest; *rest != 0 && *rest != L'"'; ++rest) {}
        if (*rest == L'"')
            ++rest;
    }
    else
        for (; *rest != 0 && *rest != L' ' && *rest != L'\t'; ++rest) {}

    std::wstring commandLine = L"\"" + std::wstring (shortExe) + L"\"" + rest;
    SetEnvironmentVariableW (L"LYNX_SHORT_PATH_RELAUNCH", L"1"); // inherited: the child never relaunches again

    STARTUPINFOW startup {};
    startup.cb = sizeof (startup);
    GetStartupInfoW (&startup);
    PROCESS_INFORMATION process {};
    if (! CreateProcessW (shortExe, commandLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process))
        return false;

    AllowSetForegroundWindow (process.dwProcessId); // its window may come to the front
    CloseHandle (process.hThread);
    WaitForSingleObject (process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess (process.hProcess, &code);
    CloseHandle (process.hProcess);
    exitCode = (int) code;
    return true;
   #else
    juce::ignoreUnused (exitCode);
    return false;
   #endif
}
} // namespace lynx::platform
