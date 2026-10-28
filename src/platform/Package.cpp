#include "platform/Package.h"

#include "core/StorePackage.h"
#include "platform/Paths.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <shellapi.h>
#endif

namespace lynx::platform
{
namespace
{
struct Identity
{
    juce::String family, fullName;
    bool simulated = false;
};

const Identity& identity()
{
    static const Identity id = [] {
        Identity result;
       #if JUCE_WINDOWS
        // looked up at run time: kernel32 has them since Windows 8, and MinGW's appmodel.h depends on _WIN32_WINNT
        using GetName = LONG (WINAPI*) (UINT32*, PWSTR);
        if (auto* kernel = GetModuleHandleW (L"kernel32.dll"))
        {
            const auto query = [kernel] (const char* function) -> juce::String {
                auto* get = reinterpret_cast<GetName> (reinterpret_cast<void*> (GetProcAddress (kernel, function)));
                if (get == nullptr)
                    return {};
                UINT32 length = 0;
                if (get (&length, nullptr) != ERROR_INSUFFICIENT_BUFFER || length == 0 || length > 1024)
                    return {}; // APPMODEL_ERROR_NO_PACKAGE: a normal, unpackaged process
                std::wstring name (length, L'\0');
                if (get (&length, name.data()) != ERROR_SUCCESS)
                    return {};
                return juce::String (name.c_str());
            };
            result.family = query ("GetCurrentPackageFamilyName");
            result.fullName = query ("GetCurrentPackageFullName");
        }
       #endif
        if (result.family.isEmpty())
        {
            const auto simulated = juce::SystemStats::getEnvironmentVariable ("LYNX_SIMULATE_PACKAGE_FAMILY", {}).trim();
            if (simulated.isNotEmpty() && ! simulated.containsAnyOf ("\\/:*?\"<>|"))
            {
                result.family = simulated;
                result.simulated = true;
            }
        }
        return result;
    }();
    return id;
}

// Copies the installer build's preferences once (core/StorePackage.h lists what comes along).
void migrateFromUnpackaged (const juce::File& target)
{
    const auto marker = target.getChildFile (store::migrationMarker);
    if (marker.exists())
        return;
    // a packaged process reading %APPDATA%\LYNX sees the real folder merged with its redirected one; the redirected
    // one is `target` itself, so whatever is found there that `target` lacks is the real, unpackaged folder's
    const auto old = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("LYNX");
    juce::StringArray copied;
    if (old.isDirectory() && old != target)
    {
        for (const auto& entry : juce::RangedDirectoryIterator (old, false, "*", juce::File::findFilesAndDirectories))
        {
            const auto& file = entry.getFile();
            const auto name = file.getFileName();
            if (! store::migratesFromUnpackaged (name.toStdString(), entry.isDirectory()))
                continue;
            const auto destination = target.getChildFile (name);
            if (destination.exists())
                continue; // never overwrite what the packaged LYNX already has
            if (entry.isDirectory() ? file.copyDirectoryTo (destination) : file.copyFileTo (destination))
                copied.add (name);
        }
    }
    marker.replaceWithText ("Copied from " + old.getFullPathName() + " on first start of the packaged LYNX:\n"
                            + copied.joinIntoString ("\n") + "\n");
}
} // namespace

bool runningPackaged() { return identity().family.isNotEmpty(); }
juce::String packageFamilyName() { return identity().family; }
juce::String packageFullName() { return identity().fullName; }

juce::File packagedSettingsFolder()
{
    static const juce::File folder = [] {
        if (! runningPackaged())
            return juce::File();
       #if JUCE_WINDOWS
        const auto local = juce::File::getSpecialLocation (juce::File::windowsLocalAppData).getFullPathName();
       #else
        const auto local = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getFullPathName();
       #endif
        const auto path = store::packagedSettingsFolder (local.toStdString(), packageFamilyName().toStdString());
        if (path.empty())
            return juce::File();
        juce::File f (juce::String::fromUTF8 (path.c_str()));
        if (! f.createDirectory())
            return juce::File();
        migrateFromUnpackaged (f);
        return f;
    }();
    return folder;
}

bool relaunchApp()
{
   #if JUCE_WINDOWS
    if (runningPackaged() && ! identity().simulated)
    {
        const auto target = juce::String (store::appsFolderTarget (packageFamilyName().toStdString()));
        const auto result = (INT_PTR) ShellExecuteW (nullptr, L"open", target.toWideCharPointer(), nullptr, nullptr, SW_SHOWNORMAL);
        if (result > 32)
            return true;
    }
   #endif
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile).startAsProcess();
}

juce::String packageReport()
{
    juce::StringArray lines;
    bool ok = true;
    const auto& id = identity();
    lines.add ("packaged=" + juce::String (runningPackaged() ? (id.simulated ? "simulated" : "yes") : "no"));
    lines.add ("family=" + id.family);
    lines.add ("fullName=" + id.fullName);
    lines.add ("exe=" + juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName());
    const auto settings = settingsFolder();
    lines.add ("settings=" + settings.getFullPathName());
    lines.add ("cache=" + cacheFolder ("wavecache").getFullPathName());
    lines.add ("temp=" + juce::File::getSpecialLocation (juce::File::tempDirectory).getFullPathName());
    lines.add ("pluginFolders=" + standardPluginFolders().toString());

    if (runningPackaged())
    {
        // the settings folder must be the package's real folder, so Explorer and other programs find its files
        // (LYNX_PROFILE_DIR, a test profile, wins over the package folder on purpose)
        const auto expected = packagedSettingsFolder();
        const bool profile = profileOverride() != juce::File();
        const bool inPackageData = expected != juce::File() && settings == expected;
        lines.add ("settingsIsPackageFolder=" + juce::String (inPackageData ? "yes" : profile ? "no (LYNX_PROFILE_DIR)" : "NO"));
        ok = ok && (inPackageData || profile);
        lines.add ("migration=" + settings.getChildFile (store::migrationMarker).loadFileAsString().replace ("\n", " | ").trim());
    }

    // a file written to the settings folder can be read back (the CI step checks it from outside the package too)
    const auto probe = settings.getChildFile ("package-probe.txt");
    const auto stamp = juce::String (juce::Time::currentTimeMillis());
    const bool wrote = probe.replaceWithText (stamp) && probe.loadFileAsString() == stamp;
    lines.add ("probe=" + probe.getFullPathName() + (wrote ? " written" : " FAILED"));
    ok = ok && wrote;
    if (! runningPackaged())
        probe.deleteFile(); // (packaged, it stays: the release workflow looks for it from outside the package)
    lines.add (juce::String ("result=") + (ok ? "OK" : "FAILED"));
    return lines.joinIntoString ("\n") + "\n";
}
} // namespace lynx::platform
