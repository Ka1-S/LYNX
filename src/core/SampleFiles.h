#pragma once

#include <array>
#include <cctype>
#include <string_view>

namespace lynx
{
// The audio files LYNX loads as samples, by extension: wav, aif(f), flac, ogg, mp3 and FL's WavPack (.wv, FL's own
// sample packs). Every drop target (Channel Rack, Playlist, Newtone...) and the Browser ask this through
// SampleLibrary::isSampleFile, so they all take what the Browser lists (round 165q: the Channel Rack ignored the .wv
// pack samples dropped from the Browser).
inline constexpr std::array<std::string_view, 7> sampleFileExtensions { "wav", "aif", "aiff", "flac", "ogg", "mp3", "wv" };

// `extension` with or without its leading dot, in any case ("WAV", ".wv").
inline bool isSampleExtension (std::string_view extension)
{
    if (! extension.empty() && extension.front() == '.')
        extension.remove_prefix (1);

    for (const auto known : sampleFileExtensions)
    {
        if (known.size() != extension.size())
            continue;
        bool same = true;
        for (std::size_t i = 0; i < known.size() && same; ++i)
            same = std::tolower ((unsigned char) extension[i]) == known[i];
        if (same)
            return true;
    }
    return false;
}
} // namespace lynx
