#include "core/SearchService.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>

namespace lynx::search
{
Service::Service (FileSystem fileSystem, Callbacks cb)
    : fs (std::move (fileSystem)), callbacks (std::move (cb))
{
    worker = std::thread ([this] { run(); });
}

Service::~Service()
{
    {
        const std::lock_guard<std::mutex> guard (lock);
        stopping = true;
        pendingSearch.reset();
        passes.clear();
    }
    wake.notify_all();
    if (worker.joinable())
        worker.join();
}

std::int64_t Service::now() const
{
    if (callbacks.now)
        return callbacks.now();
    using namespace std::chrono;
    return duration_cast<milliseconds> (system_clock::now().time_since_epoch()).count();
}

std::uint64_t Service::search (std::string query, std::vector<std::string> roots, std::size_t max, std::function<void (const SearchResult&)> done)
{
    std::uint64_t id = 0;
    {
        const std::lock_guard<std::mutex> guard (lock);
        id = ++latestSearch;
        SearchJob job;
        job.id = id;
        job.query = std::move (query);
        job.roots = std::move (roots);
        job.max = max;
        job.done = std::move (done);
        pendingSearch = std::move (job);
    }
    wake.notify_all();
    return id;
}

void Service::cancelSearch()
{
    const std::lock_guard<std::mutex> guard (lock);
    ++latestSearch;
    pendingSearch.reset();
}

void Service::refresh (std::vector<std::string> roots, bool rebuild)
{
    {
        const std::lock_guard<std::mutex> guard (lock);
        if (rebuild)
            passes.clear();
        else
            for (const auto& p : passes)
                if (p.roots == roots && ! p.rebuild)
                    return; // already waiting
        Pass pass;
        pass.roots = std::move (roots);
        pass.rebuild = rebuild;
        passes.push_back (std::move (pass));
    }
    wake.notify_all();
}

IndexStatus Service::status() const
{
    const std::lock_guard<std::mutex> guard (lock);
    return state;
}

bool Service::waitIdle (int timeoutMs)
{
    std::unique_lock<std::mutex> guard (lock);
    return idleWake.wait_for (guard, std::chrono::milliseconds (timeoutMs), [this] { return ! working && ! pendingSearch && passes.empty(); });
}

bool Service::superseded (std::uint64_t id) const
{
    const std::lock_guard<std::mutex> guard (lock);
    return stopping || latestSearch != id;
}

void Service::setStatus (const std::function<void (IndexStatus&)>& change, bool notify)
{
    std::size_t folders = 0, files = 0;
    {
        const std::lock_guard<std::mutex> guard (indexLock);
        folders = index.folderCount();
        files = index.fileCount();
    }
    {
        const std::lock_guard<std::mutex> guard (lock);
        state.folders = folders;
        state.files = files;
        change (state);
    }
    if (notify && callbacks.statusChanged)
        callbacks.statusChanged();
}

void Service::run()
{
    loadIndex();

    for (;;)
    {
        std::optional<SearchJob> job;
        std::optional<Pass> pass;
        {
            std::unique_lock<std::mutex> guard (lock);
            working = false;
            idleWake.notify_all();
            wake.wait (guard, [this] { return stopping || pendingSearch.has_value() || ! passes.empty(); });
            if (stopping)
                break;
            working = true;
            if (pendingSearch)
            {
                job = std::move (pendingSearch);
                pendingSearch.reset();
            }
            else
            {
                pass = std::move (passes.front());
                passes.pop_front();
            }
        }

        try
        {
            if (job)
                runSearch (*job);
            else if (pass && ! runPass (*pass))
            {
                // interrupted by a search: carries on after it (the work done so far is kept)
                const std::lock_guard<std::mutex> guard (lock);
                if (! stopping)
                    passes.push_front (std::move (*pass));
            }
        }
        catch (const std::exception& e) // never takes the app down: the search falls back to what is indexed
        {
            std::fprintf (stderr, "browser search: %s\n", e.what());
            setStatus ([] (IndexStatus& s) { s.busy = false; s.current.clear(); });
        }
    }

    const std::lock_guard<std::mutex> guard (lock);
    working = false;
    idleWake.notify_all();
}

void Service::loadIndex()
{
    std::string text;
    try
    {
        if (callbacks.load)
            text = callbacks.load();
        if (! text.empty())
        {
            const std::lock_guard<std::mutex> guard (indexLock);
            if (! index.fromText (text))
                index.clear();
        }
    }
    catch (const std::exception& e)
    {
        std::fprintf (stderr, "browser index: %s\n", e.what());
        const std::lock_guard<std::mutex> guard (indexLock);
        index.clear();
    }
    setStatus ([] (IndexStatus& s) { s.loaded = true; });
}

std::shared_ptr<const FolderMatches> Service::folderMatches (int key, const std::function<bool (const std::string&)>& accept)
{
    auto& cached = matchCache[key];
    std::unique_lock<std::mutex> guard (indexLock, std::try_to_lock);
    if (! guard.owns_lock())
        return cached;
    if (cached == nullptr || cached->generation != index.generation())
        cached = std::make_shared<const FolderMatches> (index.folderMatches (accept));
    return cached;
}

void Service::saveIndex()
{
    if (! callbacks.save)
        return;
    std::string text;
    {
        const std::lock_guard<std::mutex> guard (indexLock);
        text = index.toText();
    }
    callbacks.save (text); // (the disk: without the lock)
}

SearchResult Service::find (const SearchJob& job, bool partial)
{
    SearchResult result;
    result.id = job.id;
    result.partial = partial;
    const std::lock_guard<std::mutex> guard (indexLock);
    result.paths = index.find (job.query, job.roots, job.max, result.truncated);
    return result;
}

namespace
{
// Counts the folders a walk lists (for the status while it runs).
FileSystem counting (const FileSystem& fs, std::atomic<std::size_t>& listed)
{
    auto counted = fs;
    counted.list = [list = fs.list, &listed] (const std::string& folder) {
        auto l = list (folder);
        ++listed;
        return l;
    };
    return counted;
}
} // namespace

void Service::markChecked (const std::string& root)
{
    const auto t = now();
    const std::lock_guard<std::mutex> guard (lock);
    auto found = std::find_if (checked.begin(), checked.end(), [&root] (const auto& c) { return c.first == root; });
    if (found == checked.end())
        checked.emplace_back (root, t);
    else
        found->second = t;
}

void Service::runSearch (SearchJob& job)
{
    // folders never indexed are indexed first, with partial results along the way
    std::vector<std::string> missing;
    {
        const std::lock_guard<std::mutex> guard (indexLock);
        for (const auto& r : job.roots)
            if (! index.isComplete (r))
                missing.push_back (r);
    }

    if (! missing.empty())
    {
        std::atomic<std::size_t> listed { 0 };
        const auto disk = counting (fs, listed);
        auto lastShown = now(), lastSaved = now();
        std::size_t savedAt = 0;
        bool shown = false;
        auto stop = [&] {
            if (superseded (job.id))
                return true;
            const auto t = now();
            // what is already indexed is shown at once (round 164), then partial results every 500 ms
            if (! shown || t - lastShown >= 500)
            {
                const bool first = ! shown;
                shown = true;
                lastShown = t;
                setStatus ([&] (IndexStatus& s) { s.listed = listed; });
                const auto partial = find (job, true);
                if (first && partial.paths.empty())
                    return false;
                const std::lock_guard<std::mutex> guard (lock);
                if (! stopping && latestSearch == job.id && job.done)
                    job.done (partial);
            }
            if (t - lastSaved >= 60000 && listed > savedAt) // a long first walk is kept even if LYNX closes meanwhile
            {
                lastSaved = t;
                savedAt = listed;
                saveIndex();
            }
            return false;
        };

        for (const auto& root : missing)
        {
            setStatus ([&] (IndexStatus& s) {
                s.busy = true;
                s.current = root;
                s.listed = listed;
            });
            index.refresh (root, disk, stop, &indexLock);
            if (superseded (job.id))
                break;
            {
                const std::lock_guard<std::mutex> guard (indexLock);
                index.markComplete (root);
            }
            markChecked (root);
        }
        if (listed > savedAt)
            saveIndex();
        setStatus ([&] (IndexStatus& s) {
            s.busy = false;
            s.current.clear();
            s.listed = listed;
        });
    }

    if (superseded (job.id))
        return;

    const auto result = find (job, false);
    {
        const std::lock_guard<std::mutex> guard (lock);
        if (stopping || latestSearch != job.id)
            return;
        if (job.done)
            job.done (result);

        // then the folders searched are checked again in the background (FL's "dirty folders"), unless that was just done
        std::vector<std::string> stale;
        const auto t = now();
        for (const auto& r : job.roots)
        {
            const auto found = std::find_if (checked.begin(), checked.end(), [&r] (const auto& c) { return c.first == r; });
            if (found == checked.end() || t - found->second >= recheckAfterMs)
                stale.push_back (r);
        }
        if (! stale.empty()
            && std::none_of (passes.begin(), passes.end(), [&stale] (const Pass& p) { return p.roots == stale; }))
        {
            Pass pass;
            pass.roots = std::move (stale);
            passes.push_back (std::move (pass));
        }
    }
}

bool Service::runPass (Pass& pass)
{
    if (pass.rebuild)
    {
        // the index is not forgotten first (searches meanwhile still read it, round 164): every folder is listed again
        // and the ones no walk reached are dropped at the end
        {
            const std::lock_guard<std::mutex> guard (indexLock);
            pass.rebuildMark = index.beginRebuild();
        }
        {
            const std::lock_guard<std::mutex> guard (lock);
            checked.clear();
        }
        pass.rebuild = false; // (an interrupted rebuild resumes without clearing again)
        setStatus ([] (IndexStatus& s) { s.rebuilding = true; });
    }

    std::atomic<std::size_t> listed { 0 };
    const auto disk = counting (fs, listed);
    auto lastShown = now(), lastSaved = now();
    std::size_t savedAt = 0;
    auto interrupted = [this] {
        const std::lock_guard<std::mutex> guard (lock);
        return stopping || pendingSearch.has_value();
    };
    auto stop = [&] {
        if (interrupted())
            return true;
        const auto t = now();
        if (t - lastShown >= 500)
        {
            lastShown = t;
            setStatus ([&] (IndexStatus& s) { s.listed = listed; });
        }
        if (t - lastSaved >= 60000 && listed > savedAt)
        {
            lastSaved = t;
            savedAt = listed;
            saveIndex();
        }
        return false;
    };

    while (! pass.roots.empty())
    {
        const auto root = pass.roots.front();
        setStatus ([&] (IndexStatus& s) {
            s.busy = true;
            s.current = root;
            s.listed = listed;
        });
        index.refresh (root, disk, stop, &indexLock, pass.rebuildMark);
        if (interrupted())
        {
            if (listed > savedAt)
                saveIndex();
            setStatus ([] (IndexStatus& s) {
                s.busy = false;
                s.current.clear();
            });
            pass.listedBefore += listed; // (the Browser hears of it when the pass ends)
            return false;
        }
        {
            const std::lock_guard<std::mutex> guard (indexLock);
            index.markComplete (root);
        }
        markChecked (root);
        pass.roots.erase (pass.roots.begin());
    }

    if (pass.rebuildMark != 0)
    {
        const std::lock_guard<std::mutex> guard (indexLock);
        index.sweep (pass.rebuildMark);
    }
    if (listed > savedAt || pass.rebuildMark != 0)
        saveIndex();
    const auto changed = listed + pass.listedBefore > 0;
    setStatus ([&] (IndexStatus& s) {
        s.busy = false;
        s.rebuilding = false;
        s.current.clear();
        s.listed = listed;
        s.checkedAt = now();
    });
    if (changed && callbacks.indexChanged)
        callbacks.indexChanged();
    return true;
}
} // namespace lynx::search
