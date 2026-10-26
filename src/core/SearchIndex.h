#pragma once

// The Browser's search index (REA round 134; FL's browser cache "BrowserCache.TDirtyFolder" / "Indexing",
// interpreted): the file names of every folder under the Browser's roots are kept in memory (and in a file between
// sessions), so a search reads the index instead of walking the disk. Before a search the index is refreshed cheaply:
// only each known folder's modification time is read, and only the folders whose time changed (files added, removed
// or renamed in them) are listed again - FL's "dirty folders". No JUCE: the file system goes through callbacks.

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "core/SearchQuery.h"

namespace lynx::search
{
// One folder's direct contents, as the file system reported them.
struct FolderListing
{
    bool exists = false;
    std::int64_t time = 0;               // modification time (any unit; only compared for equality)
    std::vector<std::string> files;      // names (no path) of the files the Browser lists
    std::vector<std::string> subfolders; // names of the subfolders
};

struct FileSystem
{
    std::function<std::int64_t (const std::string& folder)> folderTime; // −1 when the folder no longer exists
    std::function<FolderListing (const std::string& folder)> list;
    char separator = '/';
};

class Index
{
public:
    // Brings the folders under `root` up to date (adds root when new). Returns how many folders were listed again;
    // stops early (leaving the rest for the next refresh) when shouldStop returns true (checked between folders).
    // With `guard`, the index is only locked while it is read or changed, never while the disk is read, so searches
    // of what is already indexed are not held up by a long walk (round 162, issue #9).
    int refresh (const std::string& root, const FileSystem& fs, const std::function<bool()>& shouldStop = {}, std::mutex* guard = nullptr);

    // Files whose name matches `query` under the given roots (full paths), in folder order, at most `max`.
    std::vector<std::string> find (const std::string& query, const std::vector<std::string>& roots, std::size_t max, bool& truncated) const;

    std::size_t folderCount() const noexcept { return folders.size(); }
    std::size_t fileCount() const noexcept;
    bool contains (const std::string& folder) const { return folders.count (folder) > 0; }
    void clear()
    {
        folders.clear();
        complete.clear();
    }

    // A folder tree walked to the end (round 162): searching it needs no walk first. A folder inside one counts too.
    void markComplete (const std::string& root) { complete.insert (root); }
    bool isComplete (const std::string& folder) const;

    static bool isUnder (const std::string& path, const std::string& root);

    // Text form for the index file: one "D\t<time>\t<folder>" line per folder followed by "F\t<name>" / "S\t<name>";
    // "R\t<folder>" lines name the complete trees.
    std::string toText() const;
    bool fromText (const std::string& text);

private:
    struct Folder
    {
        std::int64_t time = 0;
        std::vector<std::string> files, subfolders;
    };
    std::map<std::string, Folder> folders; // full path -> contents
    std::set<std::string> complete;
    static std::string join (const std::string& folder, const std::string& name, char separator);
    void drop (const std::string& folder, char separator);
};
} // namespace lynx::search
