// Round 165q (audit165): independent check of the #4 / #9 / #11 / #8 fixes (docs/analise/REA_rodada165q.md)

TEST (audit165_drop_targets_take_every_sample_the_browser_lists)
{
    // FL's own pack samples are WavPack (.wv): the Browser listed and previewed them, but the Channel Rack ignored
    // them when dropped (it only knew wav/aif/flac/ogg/mp3). Every drop target now asks one list (core/SampleFiles.h).
    using lynx::isSampleExtension;
    for (const auto* ext : { "wv", ".wv", "WV", "wav", ".WAV", "aif", "aiff", "flac", "ogg", "mp3" })
        CHECK (isSampleExtension (ext));
    for (const auto* ext : { "", ".", "mid", ".sf2", "flp", "wave", "w", "txt", "mp" })
        CHECK (! isSampleExtension (ext));
    CHECK (lynx::sampleFileExtensions.size() == 7);
}
