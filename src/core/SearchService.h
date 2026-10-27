#pragma once

// The Browser's search, run by one background worker that owns the search index (round 162, issue #9).
//
// LYNX 0.1.17 started a new thread for every search and, when the user typed the next letter, waited 3 s for the old
// one and then killed it (juce::Thread::stopThread's time-out ends in TerminateThread on Windows). A search thread
// that was walking a big folder tree (a slow or sleeping drive, a folder with thousands of files) was killed in the
// middle of a heap allocation or while it changed the index, which froze or crashed LYNX a little later. Here nothing
// is ever killed: one worker serves the newest search first, every walk stops between two folders when a newer search
// arrives or the service closes, and the index is only locked while it is read or changed, never while the disk is.
//
// A search reads the index (instant once it exists). A folder that was never indexed is indexed first, with partial
// results along the way. After a search, the folders searched are checked again in the background (only each folder's
// modification time is read; changed folders are listed again), and onIndexChanged tells the Browser when that found
// something, so it can search again. refresh() asks for such a pass by hand ("Refresh search index"), rebuild = from
// scratch. No JUCE: the disk and the index file go through callbacks.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/SearchIndex.h"

namespace lynx::search
{
struct IndexStatus
{
    std::size_t folders = 0, files = 0; // in the index
    bool loaded = false;                // the index file was read (or there was none)
    bool busy = false;                  // a pass (or the indexing a search needs) is running
    bool rebuilding = false;
    std::string current;                // the folder tree being walked
    std::size_t listed = 0;             // folders listed (again) by the running pass
    std::int64_t checkedAt = 0;         // when the last full pass ended (ms since the epoch; 0: not in this session)
};

struct SearchResult
{
    std::uint64_t id = 0;
    std::vector<std::string> paths;
    bool truncated = false; // more matches than `max`
    bool partial = false;   // a folder searched was still being indexed: more results follow
};

class Service
{
public:
    struct Callbacks
    {
        std::function<std::string()> load;                     // the index file's text ("" when there is none)
        std::function<void (const std::string&)> save;         // write the index file
        std::function<void()> statusChanged;                   // (from the worker thread)
        std::function<void()> indexChanged;                    // a background pass listed folders again (worker thread)
        std::function<std::int64_t()> now;                     // ms clock (tests); default: the system clock
    };

    Service (FileSystem fs, Callbacks callbacks);
    ~Service(); // stops the worker between two folders and joins it (never kills it)

    Service (const Service&) = delete;
    Service& operator= (const Service&) = delete;

    // Searches `roots` for `query`; replaces a search still waiting or running. `done` is called on the worker thread,
    // possibly several times (partial results while a folder is indexed, then the final ones), never after a newer
    // search() or cancelSearch(). Returns the search's id (SearchResult::id).
    std::uint64_t search (std::string query, std::vector<std::string> roots, std::size_t max, std::function<void (const SearchResult&)> done);
    void cancelSearch();

    // A background pass over these folders: only changed folders are listed again; rebuild = forget the index first.
    void refresh (std::vector<std::string> roots, bool rebuild = false);

    IndexStatus status() const;

    // Which folders hold a file `accept` takes (round 164: the Browser's content filter), worked out from the index
    // and kept per `key` until the index changes. Never waits for the worker: while it holds the index, the last
    // answer for `key` (or nullptr) is returned. Call from one thread (the message thread).
    std::shared_ptr<const FolderMatches> folderMatches (int key, const std::function<bool (const std::string&)>& accept);

    // Waits until the worker has nothing to do (tests, --snapshot); false on time-out.
    bool waitIdle (int timeoutMs);

    // Folders searched less than this long ago are not checked again after a search.
    static constexpr std::int64_t recheckAfterMs = 30000;

private:
    struct SearchJob
    {
        std::uint64_t id = 0;
        std::string query;
        std::vector<std::string> roots;
        std::size_t max = 0;
        std::function<void (const SearchResult&)> done;
    };
    struct Pass
    {
        std::vector<std::string> roots;
        bool rebuild = false;
        std::size_t listedBefore = 0; // folders listed again before the pass was interrupted
    };

    void run();
    void loadIndex();
    void runSearch (SearchJob& job);
    bool runPass (Pass& pass); // false: interrupted (a search came in, or closing)
    void saveIndex();
    void setStatus (const std::function<void (IndexStatus&)>& change, bool notify = true);
    SearchResult find (const SearchJob& job, bool partial);
    bool superseded (std::uint64_t id) const;
    void markChecked (const std::string& root);
    std::int64_t now() const;

    FileSystem fs;
    Callbacks callbacks;

    mutable std::mutex indexLock; // guards `index`
    Index index;

    std::map<int, std::shared_ptr<const FolderMatches>> matchCache; // (the caller's thread)

    mutable std::mutex lock; // guards everything below
    std::condition_variable wake;
    bool stopping = false, working = true; // working: the worker has a job (or is reading the index file)
    std::optional<SearchJob> pendingSearch;
    std::uint64_t latestSearch = 0;
    std::deque<Pass> passes;
    std::vector<std::pair<std::string, std::int64_t>> checked; // root -> when it was last checked
    IndexStatus state;
    std::condition_variable idleWake;

    std::thread worker; // (last: started when everything else exists)
};
} // namespace lynx::search
