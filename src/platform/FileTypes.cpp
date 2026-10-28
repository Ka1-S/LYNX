#include "platform/FileTypes.h"
#include "platform/Package.h"

#include <juce_core/juce_core.h>

#if JUCE_WINDOWS
 #include <windows.h>
 #include <shlobj.h>
#endif

namespace lynx
{
namespace
{
#if JUCE_LINUX || JUCE_BSD
constexpr const char* entryName = "com.lynx_share.LYNX.desktop"; // = the AppStream id inside the AppImage
constexpr const char* marker = "X-LYNX-AppImage=true";

juce::File dataHome()
{
    const auto xdg = juce::SystemStats::getEnvironmentVariable ("XDG_DATA_HOME", {});
    return juce::File::isAbsolutePath (xdg) ? juce::File (xdg) : juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".local/share");
}

juce::File appImageFile()
{
    const auto path = juce::SystemStats::getEnvironmentVariable ("APPIMAGE", {});
    return juce::File::isAbsolutePath (path) && juce::File (path).existsAsFile() ? juce::File (path) : juce::File();
}

// the mounted AppImage: $APPDIR, else two folders above usr/bin/LYNX
juce::File appDir()
{
    const auto env = juce::SystemStats::getEnvironmentVariable ("APPDIR", {});
    if (juce::File::isAbsolutePath (env) && juce::File (env).isDirectory())
        return juce::File (env);
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory().getParentDirectory().getParentDirectory();
}

juce::File entryFile() { return dataHome().getChildFile ("applications").getChildFile (entryName); }
juce::File mimeFile() { return dataHome().getChildFile ("mime/packages/lynx-mime.xml"); }
juce::File iconFile() { return dataHome().getChildFile ("icons/hicolor/256x256/apps/lynx.png"); }

// the Exec= value for one path: quoted with ", \\, ` and $ escaped, then backslashes doubled again (the desktop file's
// own string escaping) and % doubled (field codes)
juce::String quoted (const juce::String& path)
{
    const auto inner = path.replace ("\\", "\\\\").replace ("\"", "\\\"").replace ("`", "\\`").replace ("$", "\\$");
    return ("\"" + inner + "\"").replace ("\\", "\\\\").replace ("%", "%%");
}

void runQuietly (const juce::StringArray& command)
{
    juce::ChildProcess process;
    if (process.start (command, 0))
        process.waitForProcessToFinish (5000);
}

void refreshDesktopDatabases()
{
    // (both tools are optional: the desktop picks the files up by itself at its next scan)
    runQuietly ({ "update-mime-database", dataHome().getChildFile ("mime").getFullPathName() });
    runQuietly ({ "update-desktop-database", dataHome().getChildFile ("applications").getFullPathName() });
}

bool writeEntry (const juce::File& appImage)
{
    auto text = appDir().getChildFile (entryName).loadFileAsString();
    if (text.isEmpty())
        text = "[Desktop Entry]\nType=Application\nName=LYNX\nIcon=lynx\nTerminal=false\nCategories=AudioVideo;Audio;Midi;Music;Sequencer;\n"
               "MimeType=application/x-lynx-project;\n";
    juce::StringArray lines;
    lines.addLines (text);
    lines.removeEmptyStrings();
    for (int i = lines.size(); --i >= 0;)
        if (lines[i].startsWith ("Exec=") || lines[i].startsWith ("TryExec=") || lines[i].startsWith ("X-LYNX-"))
            lines.remove (i);
    lines.insert (1, "Exec=" + quoted (appImage.getFullPathName()) + " %F");
    lines.insert (2, "TryExec=" + appImage.getFullPathName());
    lines.add (marker);

    const auto ok = entryFile().getParentDirectory().createDirectory() && entryFile().replaceWithText (lines.joinIntoString ("\n") + "\n");
    if (const auto icon = appDir().getChildFile ("lynx.png"); icon.existsAsFile() && iconFile().getParentDirectory().createDirectory())
        icon.copyFileTo (iconFile());
    if (const auto mime = appDir().getChildFile ("usr/share/mime/packages/lynx-mime.xml"); mime.existsAsFile() && mimeFile().getParentDirectory().createDirectory())
        mime.copyFileTo (mimeFile());
    refreshDesktopDatabases();
    return ok;
}
#endif
} // namespace

bool runningAsAppImage()
{
   #if JUCE_LINUX || JUCE_BSD
    return appImageFile() != juce::File();
   #else
    return false;
   #endif
}

bool appImageMenuEntryInstalled()
{
   #if JUCE_LINUX || JUCE_BSD
    return entryFile().loadFileAsString().contains (marker);
   #else
    return false;
   #endif
}

bool setAppImageMenuEntry (bool on)
{
   #if JUCE_LINUX || JUCE_BSD
    if (on)
        return runningAsAppImage() && writeEntry (appImageFile());
    if (! appImageMenuEntryInstalled())
        return true;
    entryFile().deleteFile();
    mimeFile().deleteFile();
    // the icon stays when the .tar.gz's install.sh put LYNX in the menu too (it uses the same icon file)
    if (! dataHome().getChildFile ("applications/lynx.desktop").existsAsFile())
        iconFile().deleteFile();
    refreshDesktopDatabases();
    return true;
   #else
    juce::ignoreUnused (on);
    return false;
   #endif
}

void registerFileTypes()
{
   #if JUCE_LINUX || JUCE_BSD
    // the AppImage moved or was replaced (AppImageUpdate keeps the old file name only with -O): follow it
    if (const auto appImage = appImageFile(); appImage != juce::File() && appImageMenuEntryInstalled()
        && ! entryFile().loadFileAsString().contains ("TryExec=" + appImage.getFullPathName() + "\n"))
        writeEntry (appImage);
   #endif

   #if JUCE_WINDOWS
    // the Microsoft Store build: its manifest declares the file types (and HKCU writes would only reach the package's
    // private copy of the registry)
    if (platform::runningPackaged())
        return;
    using juce::WindowsRegistry;
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();
    const juce::String classes = "HKEY_CURRENT_USER\\Software\\Classes\\";
    const juce::String progId = "LYNX.Project";
    const auto command = "\"" + exe + "\" \"%1\"";
    const auto icon = "\"" + exe + "\",-2"; // resource id 2 in resources/LYNX.rc: the project-file icon

    if (WindowsRegistry::getValue (classes + ".lynx\\") == progId
        && WindowsRegistry::getValue (classes + progId + "\\shell\\open\\command\\") == command
        && WindowsRegistry::getValue (classes + progId + "\\DefaultIcon\\") == icon)
        return;

    WindowsRegistry::setValue (classes + ".lynx\\", progId);
    WindowsRegistry::setValue (classes + ".lynx\\Content Type", juce::String ("application/x-lynx-project"));
    WindowsRegistry::setValue (classes + progId + "\\", juce::String ("LYNX Project"));
    WindowsRegistry::setValue (classes + progId + "\\DefaultIcon\\", icon);
    WindowsRegistry::setValue (classes + progId + "\\shell\\open\\command\\", command);
    // Explorer caches icons per type: tell it the association changed
    SHChangeNotify (SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
   #endif
}
} // namespace lynx
