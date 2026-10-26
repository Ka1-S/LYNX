// Round 162 (issue #9): the Browser's search worker. 0.1.17 killed the search thread (juce::Thread::stopThread time-out
// = TerminateThread on Windows) when it was still walking a big folder tree as the user typed on; these tests type
// fast against a slow synthetic disk and check that searches are served newest-first, never killed, cancellable,
// that the index persists, and that a changed folder is picked up after a search.

#include "core/SearchService.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>

namespace b162
{
// A disk of `folders` folders under /lib (a tree 3 levels deep), 20 files each; listing a folder takes `listUs`.
struct SlowDisk
{
    struct F { std::int64_t time = 1; std::vector<std::string> files, subs; };
    std::mutex m;
    std::map<std::string, F> disk;
    std::atomic<int> lists { 0 }, times { 0 };
    int listUs = 0;

    explicit SlowDisk (int width, int us) : listUs (us)
    {
        auto& root = disk["/lib"];
        for (int a = 0; a < width; ++a)
        {
            const auto pa = "/lib/a" + std::to_string (a);
            root.subs.push_back ("a" + std::to_string (a));
            for (int b = 0; b < width; ++b)
            {
                const auto pb = pa + "/b" + std::to_string (b);
                disk[pa].subs.push_back ("b" + std::to_string (b));
                auto& leaf = disk[pb];
                for (int i = 0; i < 20; ++i)
                    leaf.files.push_back ((i % 5 == 0 ? "kick " : "snare ") + std::to_string (a) + "-" + std::to_string (b) + "-" + std::to_string (i) + ".wav");
            }
        }
    }

    lynx::search::FileSystem fs()
    {
        lynx::search::FileSystem f;
        f.folderTime = [this] (const std::string& p) {
            ++times;
            const std::lock_guard<std::mutex> g (m);
            auto i = disk.find (p);
            return i == disk.end() ? (std::int64_t) -1 : i->second.time;
        };
        f.list = [this] (const std::string& p) {
            ++lists;
            if (listUs > 0)
                std::this_thread::sleep_for (std::chrono::microseconds (listUs));
            lynx::search::FolderListing l;
            const std::lock_guard<std::mutex> g (m);
            if (auto i = disk.find (p); i != disk.end())
                l.exists = true, l.time = i->second.time, l.files = i->second.files, l.subfolders = i->second.subs;
            return l;
        };
        return f;
    }
};

struct Inbox
{
    std::mutex m;
    std::vector<lynx::search::SearchResult> results;
    std::function<void (const lynx::search::SearchResult&)> sink()
    {
        return [this] (const lynx::search::SearchResult& r) {
            const std::lock_guard<std::mutex> g (m);
            results.push_back (r);
        };
    }
    std::vector<lynx::search::SearchResult> take()
    {
        const std::lock_guard<std::mutex> g (m);
        return std::exchange (results, {});
    }
};

double msSince (std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t).count();
}
} // namespace b162

TEST (browser162_typing_fast_during_a_slow_first_index)
{
    // 31 x 31 = 993 folders, 1 ms per listing: the first index takes about a second
    b162::SlowDisk disk (31, 1000);
    std::string saved;
    std::mutex savedLock;
    lynx::search::Service::Callbacks cb;
    cb.save = [&] (const std::string& t) {
        const std::lock_guard<std::mutex> g (savedLock);
        saved = t;
    };
    lynx::search::Service service (disk.fs(), cb);
    b162::Inbox inbox;

    // the user types "k", "ki", "kic", "kick 3" while the folders are being indexed for the first time
    std::uint64_t last = 0;
    std::vector<std::uint64_t> ids;
    for (const char* q : { "k", "ki", "kic", "kick", "kick 3" })
    {
        const auto t = std::chrono::steady_clock::now();
        last = service.search (q, { "/lib" }, 100000, inbox.sink());
        ids.push_back (last);
        CHECK (b162::msSince (t) < 50); // asking never waits for the walk
        std::this_thread::sleep_for (std::chrono::milliseconds (60));
    }
    CHECK (service.waitIdle (20000));
    const auto results = inbox.take();
    // the final answer is the last query's, complete: kick files of folders a3, a30, b3, b30 (every 5th file is a kick)
    CHECK (! results.empty() && results.back().id == last && ! results.back().partial);
    std::size_t expected = 0;
    for (int a = 0; a < 31; ++a)
        for (int b = 0; b < 31; ++b)
            for (int i = 0; i < 20; i += 5)
                if ((std::to_string (a) + "-" + std::to_string (b) + "-" + std::to_string (i)).find ('3') != std::string::npos)
                    ++expected;
    CHECK (! results.empty() && results.back().paths.size() == expected);
    // superseded searches never answered after the next one started: every result after the first of `last` is `last`'s
    bool seenLast = false, order = true;
    for (const auto& r : results)
    {
        seenLast = seenLast || r.id == last;
        order = order && (! seenLast || r.id == last);
    }
    CHECK (order);
    // every folder listed once, even though the walk was interrupted four times
    CHECK (disk.lists.load() == 993);
    const auto st = service.status();
    CHECK (st.loaded && ! st.busy && st.folders == 993 && st.files == 961 * 20);
    {
        const std::lock_guard<std::mutex> g (savedLock);
        CHECK (saved.rfind ("LYNX browser index 1\n", 0) == 0);
    }
}

TEST (browser162_closing_mid_walk_is_quick)
{
    // 60 x 60 = 3661 folders at 2 ms each (~7 s): closing LYNX (or the Browser) must not wait for it, nor kill it
    b162::SlowDisk disk (60, 2000);
    b162::Inbox inbox;
    auto service = std::make_unique<lynx::search::Service> (disk.fs(), lynx::search::Service::Callbacks {});
    service->search ("kick", { "/lib" }, 500, inbox.sink());
    std::this_thread::sleep_for (std::chrono::milliseconds (300));
    CHECK (service->status().busy);
    const auto t = std::chrono::steady_clock::now();
    service.reset();
    const auto closeMs = b162::msSince (t);
    CHECK (closeMs < 500);
    CHECK (disk.lists.load() < 3661);
}

TEST (browser162_index_persists_and_follows_changes)
{
    b162::SlowDisk disk (6, 0);
    std::string saved;
    std::atomic<int> changed { 0 };
    {
        lynx::search::Service::Callbacks cb;
        cb.save = [&] (const std::string& t) { saved = t; };
        lynx::search::Service service (disk.fs(), cb);
        service.refresh ({ "/lib" });
        CHECK (service.waitIdle (10000));
        CHECK (service.status().folders == 43 && service.status().checkedAt > 0);
    }
    CHECK (! saved.empty());
    const auto listedFirst = disk.lists.load();
    CHECK (listedFirst == 43);

    // the next session reads the index file: searching lists nothing, and the result is there at once
    std::int64_t clock = 1000000;
    lynx::search::Service::Callbacks cb;
    cb.load = [&] { return saved; };
    cb.save = [&] (const std::string& t) { saved = t; };
    cb.indexChanged = [&] { ++changed; };
    cb.now = [&] { return clock; };
    lynx::search::Service service (disk.fs(), cb);
    b162::Inbox inbox;
    service.search ("kick?2-*", { "/lib" }, 500, inbox.sink());
    CHECK (service.waitIdle (10000));
    auto r = inbox.take();
    CHECK (r.size() == 1 && r[0].paths.size() == 6 * 4 && ! r[0].partial);
    CHECK (disk.lists.load() == listedFirst); // the check after the search found nothing changed
    CHECK (changed.load() == 0);

    // a file is added to one folder (its time changes); the next search (30 s later) answers from the index at once,
    // then the background check lists that folder again and tells the Browser, whose new search finds the file
    {
        const std::lock_guard<std::mutex> g (disk.m);
        disk.disk["/lib/a2/b1"].files.push_back ("kick 2-new.wav");
        disk.disk["/lib/a2/b1"].time = 2;
    }
    clock += lynx::search::Service::recheckAfterMs;
    service.search ("kick?2-*", { "/lib" }, 500, inbox.sink());
    CHECK (service.waitIdle (10000));
    r = inbox.take();
    CHECK (r.size() == 1 && r[0].paths.size() == 24);
    CHECK (changed.load() == 1 && disk.lists.load() == listedFirst + 1);
    service.search ("kick?2-*", { "/lib" }, 500, inbox.sink());
    CHECK (service.waitIdle (10000));
    r = inbox.take();
    CHECK (r.size() == 1 && r[0].paths.size() == 25);

    // "Rebuild search index": everything listed again
    service.refresh ({ "/lib" }, true);
    CHECK (service.waitIdle (10000));
    CHECK (disk.lists.load() == listedFirst + 1 + 43 && service.status().files == 36 * 20 + 1);

    // cancelled: no answer
    service.search ("kick", { "/lib" }, 500, inbox.sink());
    service.cancelSearch();
    CHECK (service.waitIdle (10000));
    CHECK (inbox.take().empty());
}

TEST (browser162_search_is_served_before_a_background_pass)
{
    // a big background pass (a manual "Rebuild") of a slow tree, and a search of a small, indexed folder meanwhile
    b162::SlowDisk big (50, 1000);
    lynx::search::FileSystem fs = big.fs();
    lynx::search::Service service (fs, {});
    b162::Inbox inbox;
    service.search ("kick", { "/lib/a1" }, 500, inbox.sink()); // indexes /lib/a1 (51 folders)
    CHECK (service.waitIdle (10000));
    inbox.take();
    service.refresh ({ "/lib" }, true);
    std::this_thread::sleep_for (std::chrono::milliseconds (200));
    CHECK (service.status().busy);
    const auto t = std::chrono::steady_clock::now();
    service.search ("kick 1-", { "/lib/a1" }, 500, inbox.sink());
    while (inbox.take().empty() && b162::msSince (t) < 5000)
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    const auto answeredMs = b162::msSince (t);
    std::printf ("  [b162] search answered in %.1f ms during a rebuild\n", answeredMs);
    CHECK (answeredMs < 500);
    CHECK (service.waitIdle (30000));
    CHECK (service.status().folders == 2551 && ! service.status().busy);
}
