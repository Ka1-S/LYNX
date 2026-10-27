#include "core/BrowserView.h"


#include <algorithm>
#include <cctype>
#include <sstream>

namespace lynx::browserview
{
namespace
{
std::string extensionOf (const std::string& fileName)
{
    const auto dot = fileName.find_last_of ('.');
    if (dot == std::string::npos || dot + 1 >= fileName.size())
        return {};
    auto ext = fileName.substr (dot + 1);
    std::transform (ext.begin(), ext.end(), ext.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return ext;
}

bool among (const std::string& ext, std::initializer_list<const char*> list)
{
    return std::any_of (list.begin(), list.end(), [&ext] (const char* e) { return ext == e; });
}

bool isAudioExt (const std::string& ext) { return among (ext, { "wav", "flac", "ogg", "mp3", "aif", "aiff", "wv" }); }
bool isFontExt (const std::string& ext) { return among (ext, { "sf2", "sf3" }); }
bool isPresetExt (const std::string& ext) { return among (ext, { "fst" }); }
bool isProjectExt (const std::string& ext) { return among (ext, { "lynx", "flp", "mid", "midi" }); }
} // namespace

const char* contentFilterId (ContentFilter filter)
{
    switch (filter)
    {
        case ContentFilter::audio: return "audio";
        case ContentFilter::samples: return "samples";
        case ContentFilter::presets: return "presets";
        case ContentFilter::projects: return "projects";
        default: return "all";
    }
}

const char* contentFilterLabel (ContentFilter filter)
{
    switch (filter)
    {
        case ContentFilter::audio: return "Audio files only";
        case ContentFilter::samples: return "Samples and SoundFonts";
        case ContentFilter::presets: return "Presets";
        case ContentFilter::projects: return "Projects and MIDI";
        default: return "All files";
    }
}

ContentFilter contentFilterFromId (const std::string& id)
{
    for (int i = 0; i < contentFilterCount; ++i)
        if (id == contentFilterId ((ContentFilter) i))
            return (ContentFilter) i;
    return ContentFilter::all;
}

bool listed (const std::string& fileName, ContentFilter filter)
{
    const auto ext = extensionOf (fileName);
    const auto audio = isAudioExt (ext), font = isFontExt (ext), preset = isPresetExt (ext), project = isProjectExt (ext);
    switch (filter)
    {
        case ContentFilter::audio: return audio;
        case ContentFilter::samples: return audio || font;
        case ContentFilter::presets: return preset || font;
        case ContentFilter::projects: return project;
        default: return audio || font || preset || project;
    }
}

bool sampleBrowserShowsByDefault (RootKind kind)
{
    return kind == RootKind::flSamples || kind == RootKind::music || kind == RootKind::downloads || kind == RootKind::user;
}

void View::setShown (const std::string& id, bool shown)
{
    if (shown)
        hidden.erase (id);
    else
        hidden.insert (id);
}

std::string View::toText() const
{
    std::ostringstream out;
    out << "filter=" << contentFilterId (filter) << "\n";
    out << "open=" << (open ? 1 : 0) << "\n";
    out << "bounds=" << x << "," << y << "," << width << "," << height << "\n";
    for (const auto& id : hidden)
        out << "hidden=" << id << "\n";
    return out.str();
}

View View::fromText (const std::string& text)
{
    View v;
    std::istringstream in (text);
    std::string line;
    while (std::getline (in, line))
    {
        if (! line.empty() && line.back() == '\r')
            line.pop_back();
        const auto eq = line.find ('=');
        if (eq == std::string::npos)
            continue;
        auto trim = [] (std::string s) {
            const auto a = s.find_first_not_of (" \t");
            const auto b = s.find_last_not_of (" \t");
            return a == std::string::npos ? std::string() : s.substr (a, b - a + 1);
        };
        const auto key = trim (line.substr (0, eq));
        const auto value = trim (line.substr (eq + 1));
        if (key == "filter")
            v.filter = contentFilterFromId (value);
        else if (key == "open")
            v.open = value == "1";
        else if (key == "hidden" && ! value.empty())
            v.hidden.insert (value);
        else if (key == "bounds")
        {
            int n[4] {};
            char sep = 0;
            std::istringstream b (value);
            if ((b >> n[0] >> sep >> n[1] >> sep >> n[2] >> sep >> n[3]) && n[2] >= 0 && n[3] >= 0)
            {
                v.x = n[0];
                v.y = n[1];
                v.width = n[2];
                v.height = n[3];
            }
        }
    }
    return v;
}
} // namespace lynx::browserview
