// Audit 165: a sample dropped on the Fruity Granulizer's window loads it into the Granulizer (before, the drop fell
// through to the Playlist and made an audio clip). The window's drop target (ui/ChannelSettings ModelPanel) asks
// projectcontents::pluginWindowTakesSample and SampleLibrary::isSampleFile, then sets the channel's sample path.

#include "core/ProjectContents.h"
#include "core/RackRenderer.h"
#include "core/SampleFiles.h"

TEST (grandrop165_granulizer_window_takes_a_dropped_sample)
{
    using namespace lynx;
    CHECK (projectcontents::pluginWindowTakesSample (InstrumentKind::granulizer));
    CHECK (! projectcontents::pluginWindowTakesSample (InstrumentKind::sampler)); // (its waveform has its own target)
    CHECK (! projectcontents::pluginWindowTakesSample (InstrumentKind::harmor));
    // the Browser's sample files, FL's .wv packs included
    for (const auto* e : { "wav", ".WAV", "aif", "aiff", "flac", "ogg", "mp3", "wv" })
        CHECK (isSampleExtension (e));
    CHECK (! isSampleExtension ("mid") && ! isSampleExtension ("flp") && ! isSampleExtension ("mp4"));

    // the drop's edit (the channel's sample path) makes the channel play the new sample at once, settings unchanged
    auto a = std::make_shared<SampleData> (flgran165::drums (2, 44100.0));
    auto b = std::make_shared<SampleData> (*a);
    for (auto& x : b->left)
        x *= 0.5f;
    for (auto& x : b->right)
        x *= 0.5f;
    const RackState::SampleProvider samples = [a, b] (const std::string& path) -> std::shared_ptr<const SampleData> {
        return path == "a.wav" ? a : path == "b.wav" ? b : nullptr;
    };
    Song song;
    song.mixer = Song::defaultMixer (2);
    song.bpm = 140.0; // (flgran165::play's tempo)
    ChannelDef ch;
    ch.name = "Fruity Granulizer";
    ch.instrument = InstrumentKind::granulizer;
    ch.pluginState = flgran165::state ({ 204, 50, 611, 300, 0, 180, 0, 0, 0, 0, 0, 228 }, 0, 2, 1);
    ch.samplePath = "a.wav";
    song.channels.push_back (ch);
    auto before = RackState::build (song, nullptr, 44100.0, samples);
    song.channels[0].samplePath = "b.wav"; // what the drop does
    auto after = RackState::build (song, before.get(), 44100.0, samples);
    CHECK (before->instruments.size() == 1 && after->instruments.size() == 1);
    if (after->instruments.size() != 1)
        return;
    CHECK (after->instruments[0] != before->instruments[0]);
    constexpr int n = 8000;
    std::vector<float> l (n, 0.0f), r (n, 0.0f);
    after->instruments[0]->noteOn (62, 0.8f);
    after->instruments[0]->render (l.data(), r.data(), n, 1.0f, 1.0f);
    CHECK (l == flgran165::play (song.channels[0], b, n));
    CHECK (l != flgran165::play (song.channels[0], a, n));
    CHECK (song.channels[0].name == "Fruity Granulizer");
}
