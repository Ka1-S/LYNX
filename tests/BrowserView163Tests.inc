// Round 163 (user feedback): what a Browser shows — hidden roots, the content filter and the saved view of the main
// Browser and of the separate Sample browser window.

#include "core/BrowserView.h"

TEST (browser163_content_filter)
{
    using namespace lynx::browserview;
    CHECK (listed ("Kick.WAV", ContentFilter::all));
    CHECK (listed ("pad.sf2", ContentFilter::all));
    CHECK (! listed ("readme.txt", ContentFilter::all));
    CHECK (! listed ("noextension", ContentFilter::all));
    CHECK (listed ("loop.flac", ContentFilter::audio));
    CHECK (! listed ("pad.sf2", ContentFilter::audio));
    CHECK (listed ("pad.SF3", ContentFilter::samples));
    CHECK (listed ("hat.mp3", ContentFilter::samples));
    CHECK (! listed ("beat.flp", ContentFilter::samples));
    CHECK (listed ("lead.fst", ContentFilter::presets));
    CHECK (! listed ("lead.wav", ContentFilter::presets));
    CHECK (listed ("song.lynx", ContentFilter::projects));
    CHECK (listed ("song.mid", ContentFilter::projects));
    CHECK (! listed ("kick.wav", ContentFilter::projects));
    for (int i = 0; i < contentFilterCount; ++i)
        CHECK (contentFilterFromId (contentFilterId ((ContentFilter) i)) == (ContentFilter) i);
    CHECK (contentFilterFromId ("bogus") == ContentFilter::all);
}

TEST (browser163_view_round_trip)
{
    using namespace lynx::browserview;
    View v;
    CHECK (v.shows ("C:\\Users\\me\\Desktop"));
    v.setShown ("C:\\Users\\me\\Desktop", false);
    v.setShown (projectRootId, false);
    v.filter = ContentFilter::samples;
    v.open = true;
    v.x = 10; v.y = -20; v.width = 340; v.height = 600;
    CHECK (! v.shows ("C:\\Users\\me\\Desktop") && ! v.shows (projectRootId) && v.shows (pluginsRootId));
    const auto back = View::fromText (v.toText());
    CHECK (back == v);
    v.setShown (projectRootId, true);
    CHECK (v.shows (projectRootId));
    // old / empty files: everything shown, all files
    CHECK (View::fromText ("") == View {});
    CHECK (View::fromText ("garbage\nfilter=presets\r\nbounds=1,2,x\n").filter == ContentFilter::presets);
    CHECK (sampleBrowserShowsByDefault (RootKind::flSamples) && sampleBrowserShowsByDefault (RootKind::user));
    CHECK (! sampleBrowserShowsByDefault (RootKind::documents) && ! sampleBrowserShowsByDefault (RootKind::project)
           && ! sampleBrowserShowsByDefault (RootKind::flDemos) && ! sampleBrowserShowsByDefault (RootKind::pluginDatabase));
}
