// Round 164 (misc164): the Browser's content filter leaves out folders with nothing to list (from the search index).

#include "core/BrowserView.h"
#include "core/SearchIndex.h"

TEST (misc164_browser_filter_hides_empty_folders)
{
    using lynx::search::FolderMatches;
    struct F { std::int64_t time; std::vector<std::string> files, subs; };
    std::map<std::string, F> disk { { "/lib", { 1, { "song.flp" }, { "Drums", "Presets", "Empty", "Deep" } } },
                                    { "/lib/Drums", { 1, { "kick.wav" }, {} } },
                                    { "/lib/Presets", { 1, { "lead.fst" }, {} } },
                                    { "/lib/Empty", { 1, {}, {} } },
                                    { "/lib/Deep", { 1, {}, { "A" } } },
                                    { "/lib/Deep/A", { 1, { "pad.sf2" }, {} } } };
    lynx::search::FileSystem fs;
    fs.folderTime = [&] (const std::string& f) { auto i = disk.find (f); return i == disk.end() ? (std::int64_t) -1 : i->second.time; };
    fs.list = [&] (const std::string& f) {
        lynx::search::FolderListing l;
        if (auto i = disk.find (f); i != disk.end())
            l.exists = true, l.time = i->second.time, l.files = i->second.files, l.subfolders = i->second.subs;
        return l;
    };
    auto accept = [] (lynx::browserview::ContentFilter filter) {
        return [filter] (const std::string& name) { return lynx::browserview::listed (name, filter); };
    };

    lynx::search::Index index;
    index.refresh ("/lib", fs);
    // not walked to the end yet: nothing is known to be empty (the Browser keeps listing those folders)
    auto m = index.folderMatches (accept (lynx::browserview::ContentFilter::audio));
    CHECK (m.of ("/lib/Drums") == FolderMatches::Contents::some && m.of ("/lib") == FolderMatches::Contents::some);
    CHECK (m.of ("/lib/Presets") == FolderMatches::Contents::unknown && m.of ("/elsewhere") == FolderMatches::Contents::unknown);

    const auto before = index.generation();
    index.markComplete ("/lib");
    CHECK (index.generation() != before);
    m = index.folderMatches (accept (lynx::browserview::ContentFilter::audio));
    CHECK (m.generation == index.generation());
    CHECK (m.of ("/lib/Presets") == FolderMatches::Contents::none && m.of ("/lib/Empty") == FolderMatches::Contents::none);
    CHECK (m.of ("/lib/Deep") == FolderMatches::Contents::none && m.of ("/lib/Deep/A") == FolderMatches::Contents::none);

    // a SoundFont deep down keeps every folder above it under "Samples and SoundFonts"
    m = index.folderMatches (accept (lynx::browserview::ContentFilter::samples));
    CHECK (m.of ("/lib/Deep") == FolderMatches::Contents::some && m.of ("/lib/Deep/A") == FolderMatches::Contents::some);
    CHECK (m.of ("/lib/Presets") == FolderMatches::Contents::none);
    m = index.folderMatches (accept (lynx::browserview::ContentFilter::presets));
    CHECK (m.of ("/lib/Presets") == FolderMatches::Contents::some && m.of ("/lib/Drums") == FolderMatches::Contents::none);
    m = index.folderMatches (accept (lynx::browserview::ContentFilter::projects));
    CHECK (m.of ("/lib") == FolderMatches::Contents::some && m.of ("/lib/Drums") == FolderMatches::Contents::none);

    // a file arrives in Empty: the index changes, and so does the answer
    disk["/lib/Empty"] = { 2, { "hat.wav" }, {} };
    const auto gen = index.generation();
    CHECK (index.refresh ("/lib", fs) == 1 && index.generation() != gen);
    m = index.folderMatches (accept (lynx::browserview::ContentFilter::audio));
    CHECK (m.of ("/lib/Empty") == FolderMatches::Contents::some);
    // nothing changed on disk: same generation (the Browser's cache is kept)
    const auto still = index.generation();
    CHECK (index.refresh ("/lib", fs) == 0 && index.generation() == still);

    // Windows paths: a drive root keeps its separator
    lynx::search::Index win;
    std::map<std::string, F> wdisk { { "D:\\", { 1, {}, { "Samples" } } }, { "D:\\Samples", { 1, { "a.wav" }, {} } } };
    fs.separator = '\\';
    fs.folderTime = [&] (const std::string& f) { auto i = wdisk.find (f); return i == wdisk.end() ? (std::int64_t) -1 : i->second.time; };
    fs.list = [&] (const std::string& f) {
        lynx::search::FolderListing l;
        if (auto i = wdisk.find (f); i != wdisk.end())
            l.exists = true, l.time = i->second.time, l.files = i->second.files, l.subfolders = i->second.subs;
        return l;
    };
    win.refresh ("D:\\", fs);
    win.markComplete ("D:\\");
    m = win.folderMatches (accept (lynx::browserview::ContentFilter::audio));
    CHECK (m.of ("D:\\Samples") == FolderMatches::Contents::some && m.of ("D:\\") == FolderMatches::Contents::some);
}

#include "core/WindowFit.h"

TEST (misc164_fixed_window_scrolls_when_taller_than_the_screen)
{
    using lynx::windowfit::scrollFit;
    // fits: kept as it is
    auto f = scrollFit (700, 540, 1280, 690, 16, 40, 12);
    CHECK (! f.scrolls() && f.w == 700 && f.h == 540);
    // 150% zoom of a 1080p screen (1280 x ~690 logical): a 760 px tall settings page scrolls, the bar beside it
    f = scrollFit (700, 760, 1280, 690, 16, 40, 12);
    CHECK (f.scrollsV && ! f.scrollsH && f.h == 650 && f.w == 712);
    // too wide as well: both ways, never larger than the screen
    f = scrollFit (1400, 900, 1280, 690, 16, 40, 12);
    CHECK (f.scrollsV && f.scrollsH && f.w == 1264 && f.h == 650);
    // only too wide: the horizontal bar may push it over the height
    f = scrollFit (1400, 645, 1280, 690, 16, 40, 12);
    CHECK (f.scrollsH && f.scrollsV && f.h == 650);
    f = scrollFit (1400, 300, 1280, 690, 16, 40, 12);
    CHECK (f.scrollsH && ! f.scrollsV && f.h == 312 && f.w == 1264);
}
