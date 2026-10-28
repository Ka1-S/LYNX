#include "platform/Paths.h"
#include "engine/ProjectPackage.h"

#include "core/MissingFiles.h"
#include "core/ProjectSafety.h"
#include "engine/ProjectFile.h"

namespace lynx::ProjectPackage
{
namespace
{
juce::String str (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }

juce::File fileFor (const std::string& path)
{
    const auto p = str (path);
    return juce::File::isAbsolutePath (p) ? juce::File (p) : juce::File();
}
} // namespace

bool exportZip (const Song& song, const juce::String& projectName, const juce::File& zip, Result& result, juce::String& error)
{
    result = {};
    const auto plan = safety::packagePlan (song, "samples");

    Song packed = song;
    std::map<std::string, std::string> relink;
    juce::ZipFile::Builder builder;

    for (const auto& [original, inside] : plan)
    {
        const auto source = fileFor (original);
        if (! source.existsAsFile())
        {
            result.missing.add (str (original));
            continue; // the project keeps the old path (the Missing files dialog can find it later)
        }
        builder.addFile (source, 0, str (inside)); // samples are already compressed audio or barely compress
        relink[original] = inside;
        ++result.files;
    }
    applyRelocations (packed, relink);

    const auto name = juce::File::createLegalFileName (projectName.isNotEmpty() ? projectName : juce::String ("untitled"));
    const auto json = ProjectFile::toJson (packed);
    builder.addEntry (new juce::MemoryInputStream (json.toRawUTF8(), json.getNumBytesAsUTF8(), true), 9, name + ProjectFile::extension,
                      juce::Time::getCurrentTime());

    juce::TemporaryFile temp (zip);
    {
        juce::FileOutputStream out (temp.getFile());
        if (! out.openedOk() || ! builder.writeToStream (out, nullptr))
        {
            error = "could not write " + zip.getFullPathName();
            return false;
        }
    }
    if (! temp.overwriteTargetFileWithTemporary())
    {
        error = "could not write " + zip.getFullPathName();
        return false;
    }
    return true;
}

bool collectSamples (const Song& song, const juce::File& folder, std::map<std::string, std::string>& relink, Result& result,
                     juce::String& error)
{
    result = {};
    relink.clear();
    if (! folder.createDirectory())
    {
        error = "could not create " + folder.getFullPathName();
        return false;
    }

    for (const auto& original : safety::referencedSamples (song))
    {
        const auto source = fileFor (original);
        if (! source.existsAsFile())
        {
            result.missing.add (str (original));
            continue;
        }
        if (source.getParentDirectory() == folder) // already there
        {
            relink[original] = source.getFullPathName().toStdString();
            continue;
        }
        const auto stem = safety::uniqueName (source.getFileNameWithoutExtension().toStdString(), [&] (const std::string& s) {
            return folder.getChildFile (str (s) + source.getFileExtension()).exists();
        });
        const auto target = folder.getChildFile (str (stem) + source.getFileExtension());
        if (! source.copyFileTo (target))
        {
            error = "could not copy " + source.getFullPathName();
            return false;
        }
        relink[original] = target.getFullPathName().toStdString();
        ++result.files;
    }
    return true;
}

juce::File extractZip (const juce::File& zipFile, const juce::File& destinationRoot, juce::String& error)
{
    juce::ZipFile zip (zipFile);
    if (zip.getNumEntries() == 0)
    {
        error = "not a project package (empty or not a zip file)";
        return {};
    }

    bool hasProject = false;
    for (int i = 0; i < zip.getNumEntries(); ++i)
        if (const auto* e = zip.getEntry (i); e != nullptr && (e->filename.endsWithIgnoreCase (ProjectFile::extension) || e->filename.endsWithIgnoreCase (".flp")))
            hasProject = true;
    if (! hasProject)
    {
        error = "the package has no project file (.lynx or .flp)";
        return {};
    }

    destinationRoot.createDirectory();
    const auto folder = destinationRoot.getNonexistentChildFile (juce::File::createLegalFileName (zipFile.getFileNameWithoutExtension()), {}, false);
    if (! folder.createDirectory())
    {
        error = "could not create " + folder.getFullPathName();
        return {};
    }
    if (const auto r = zip.uncompressTo (folder, false); r.failed()) // (JUCE refuses entries outside the folder)
    {
        error = r.getErrorMessage();
        return {};
    }

    for (const auto* pattern : { "*.lynx", "*.flp" })
    {
        auto found = folder.findChildFiles (juce::File::findFiles, true, pattern);
        found.sort();
        if (! found.isEmpty())
            return found.getFirst();
    }
    error = "the package has no project file (.lynx or .flp)";
    return {};
}

juce::File defaultExtractRoot()
{
    return lynx::platform::documentsFolder().getChildFile ("LYNX/Projects/Unpacked");
}
} // namespace lynx::ProjectPackage
