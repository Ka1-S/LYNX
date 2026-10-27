#include "core/SearchIndex.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace lynx::search
{
std::string Index::join (const std::string& folder, const std::string& name, char separator)
{
    if (! folder.empty() && (folder.back() == '/' || folder.back() == '\\'))
        return folder + name;
    return folder + separator + name;
}

bool Index::isUnder (const std::string& path, const std::string& root)
{
    if (root.empty() || path.size() < root.size() || path.compare (0, root.size(), root) != 0)
        return false;
    if (path.size() == root.size())
        return true;
    const auto last = root.back();
    const auto next = path[root.size()];
    return last == '/' || last == '\\' || next == '/' || next == '\\';
}

void Index::drop (const std::string& folder, char separator)
{
    const auto found = folders.find (folder);
    if (found == folders.end())
        return;
    const auto subfolders = found->second.subfolders;
    folders.erase (found);
    ++gen;
    for (const auto& s : subfolders)
        drop (join (folder, s, separator), separator);
}

int Index::refresh (const std::string& root, const FileSystem& fs, const std::function<bool()>& shouldStop, std::mutex* guard)
{
    if (! fs.folderTime || ! fs.list || root.empty())
        return 0;

    auto hold = [guard] { return guard != nullptr ? std::unique_lock<std::mutex> (*guard) : std::unique_lock<std::mutex>(); };
    int listed = 0;
    std::vector<std::string> pending { root };

    while (! pending.empty())
    {
        if (shouldStop && shouldStop())
            break;

        const auto folder = pending.back();
        pending.pop_back();
        if (folder.size() > 1024) // a runaway path (a link loop the file system did not report): not followed
            continue;
        const auto time = fs.folderTime (folder); // (the disk: without the lock)

        std::vector<std::string> subs;
        bool dirty = false;
        {
            const auto lock = hold();
            if (time < 0)
            {
                drop (folder, fs.separator);
                continue;
            }
            const auto known = folders.find (folder);
            dirty = known == folders.end() || known->second.time != time; // new or "dirty": list it again
            if (! dirty)
                subs = known->second.subfolders;
        }

        if (dirty)
        {
            auto listing = fs.list (folder);
            ++listed;
            const auto lock = hold();
            if (! listing.exists)
            {
                drop (folder, fs.separator);
                continue;
            }
            if (const auto known = folders.find (folder); known != folders.end())
                for (const auto& old : known->second.subfolders)
                    if (std::find (listing.subfolders.begin(), listing.subfolders.end(), old) == listing.subfolders.end())
                        drop (join (folder, old, fs.separator), fs.separator);
            auto& entry = folders[folder];
            ++gen;
            entry.time = time;
            entry.files = std::move (listing.files);
            entry.subfolders = std::move (listing.subfolders);
            subs = entry.subfolders;
        }

        // depth-first, in name order (pushed reversed)
        for (auto s = subs.rbegin(); s != subs.rend(); ++s)
            pending.push_back (join (folder, *s, fs.separator));
    }
    return listed;
}

std::vector<std::string> Index::find (const std::string& query, const std::vector<std::string>& roots, std::size_t max, bool& truncated) const
{
    truncated = false;
    std::vector<std::string> found;
    const auto parsed = parse (query);

    for (const auto& [path, folder] : folders)
    {
        if (std::none_of (roots.begin(), roots.end(), [&path] (const std::string& r) { return isUnder (path, r); }))
            continue;
        const auto separator = path.find ('\\') != std::string::npos ? '\\' : '/';
        for (const auto& name : folder.files)
        {
            if (! matches (parsed, name))
                continue;
            if (found.size() >= max)
            {
                truncated = true;
                return found;
            }
            found.push_back (join (path, name, separator));
        }
    }
    return found;
}

FolderMatches Index::folderMatches (const std::function<bool (const std::string&)>& accept) const
{
    FolderMatches result;
    result.generation = gen;
    for (const auto& [path, folder] : folders)
    {
        if (result.some.count (path) > 0 || std::none_of (folder.files.begin(), folder.files.end(), accept))
            continue;
        // this folder and every indexed folder above it hold something
        auto at = path;
        while (result.some.insert (at).second)
        {
            const auto cut = at.find_last_of ("/\\");
            if (cut == std::string::npos || cut == 0)
                break;
            auto parent = at.substr (0, cut);
            if (folders.count (parent) == 0)
            {
                parent += at[cut]; // a drive's root keeps its separator ("C:\\")
                if (folders.count (parent) == 0)
                    break;
            }
            at = std::move (parent);
        }
    }
    for (const auto& [path, folder] : folders)
        if (result.some.count (path) == 0 && isComplete (path))
            result.none.insert (path);
    return result;
}

bool Index::isComplete (const std::string& folder) const
{
    return std::any_of (complete.begin(), complete.end(), [this, &folder] (const std::string& r) { return isUnder (folder, r) && folders.count (r) > 0; });
}

std::size_t Index::fileCount() const noexcept
{
    std::size_t n = 0;
    for (const auto& f : folders)
        n += f.second.files.size();
    return n;
}

std::string Index::toText() const
{
    std::string out = "LYNX browser index 1\n";
    for (const auto& r : complete)
        out += "R\t" + r + "\n";
    for (const auto& [path, folder] : folders)
    {
        out += "D\t" + std::to_string (folder.time) + "\t" + path + "\n";
        for (const auto& f : folder.files)
            out += "F\t" + f + "\n";
        for (const auto& s : folder.subfolders)
            out += "S\t" + s + "\n";
    }
    return out;
}

bool Index::fromText (const std::string& text)
{
    clear();
    ++gen;
    std::istringstream in (text);
    std::string line;
    if (! std::getline (in, line) || line != "LYNX browser index 1")
        return false;
    Folder* current = nullptr;
    while (std::getline (in, line))
    {
        if (! line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() < 2 || line[1] != '\t')
            continue;
        const auto rest = line.substr (2);
        if (line[0] == 'D')
        {
            const auto tab = rest.find ('\t');
            if (tab == std::string::npos)
            {
                current = nullptr;
                continue;
            }
            auto& f = folders[rest.substr (tab + 1)];
            f.time = std::strtoll (rest.substr (0, tab).c_str(), nullptr, 10);
            current = &f;
        }
        else if (line[0] == 'R')
            complete.insert (rest);
        else if (current != nullptr && line[0] == 'F')
            current->files.push_back (rest);
        else if (current != nullptr && line[0] == 'S')
            current->subfolders.push_back (rest);
    }
    return true;
}
} // namespace lynx::search
