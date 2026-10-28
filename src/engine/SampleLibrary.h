#pragma once

#include <map>
#include <memory>
#include <mutex>

#include <juce_audio_formats/juce_audio_formats.h>

#include "core/Instruments.h"

namespace lynx
{
// Decodes audio files into SampleData, cached by path (one cache for the whole process).
// Any thread except the audio thread.
class SampleLibrary
{
public:
    SampleLibrary();

    std::shared_ptr<const SampleData> load (const juce::String& path);
    // The sample when something already holds it loaded (nullptr otherwise): never reads the file (round 149: the
    // stream prefetcher looked its clips up with load(), which decoded again every audio clip whose channel plays a
    // time-stretched copy, seconds on every edit of a song with many of them)
    std::shared_ptr<const SampleData> findLoaded (const juce::String& path);
    // The file changed on disk (the audio editor saved over it): the next load() reads it again.
    void forget (const juce::String& path);

    // The sample time-stretched without changing pitch (core/TimeStretch), cached by (path, ratio, method).
    std::shared_ptr<const SampleData> loadStretched (const juce::String& path, double ratio, StretchMethod method = StretchMethod::general);

    // Above this, audio goes to a memory-mapped disk cache instead of staying entirely
    // in RAM ("disk streaming"). The cache is reused across sessions.
    static constexpr double streamAboveSeconds = 30.0;
    static juce::File cacheFolder();

    // Filter for FileChooser ("*.wav;*.flac;...")
    juce::String wildcard() const;

    // The audio files LYNX loads as samples (core/SampleFiles.h: wav, aif(f), flac, ogg, mp3 and FL's WavPack .wv).
    // Every drop target (Channel Rack, Playlist, Audio editor...) asks this, so they all take what the Browser lists.
    static bool isSampleFile (const juce::File& file);

private:
    std::unique_ptr<juce::AudioFormatReader> createReader (const juce::File& file);
    std::shared_ptr<const SampleData> loadStreamed (const juce::File& file, juce::AudioFormatReader& reader);
    std::shared_ptr<const SampleData> decode (const juce::String& path);

    juce::AudioFormatManager formats;
};
// Brings in from disk the range [from, to) (seconds) of a mapped sample. Any thread except the audio thread.
void prefetchSample (const SampleData& data, double fromSeconds, double toSeconds);
} // namespace lynx
