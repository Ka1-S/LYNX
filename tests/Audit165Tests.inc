// Round 165q (audit165): independent check of the #4 / #9 / #11 / #8 fixes (docs/analise/REA_rodada165q.md)

TEST (audit165_drop_targets_take_every_sample_the_browser_lists)
{
    // FL's own pack samples are WavPack (.wv): the Browser listed and previewed them, but the Channel Rack ignored
    // them when dropped (it only knew wav/aif/flac/ogg/mp3). Every drop target now asks SampleLibrary::isSampleFile.
    using lynx::SampleLibrary;
    for (const auto* name : { "Attack Kick 01.wv", "a.wav", "B.WAV", "c.aif", "d.aiff", "e.flac", "f.ogg", "g.mp3" })
        CHECK (SampleLibrary::isSampleFile (juce::File::getCurrentWorkingDirectory().getChildFile (name)));
    for (const auto* name : { "a.mid", "b.sf2", "c.flp", "d.txt", "e" })
        CHECK (! SampleLibrary::isSampleFile (juce::File::getCurrentWorkingDirectory().getChildFile (name)));

    // and the library has a reader for each of those extensions
    SampleLibrary library;
    const auto wildcard = library.wildcard().toLowerCase();
    for (const auto& ext : juce::StringArray::fromTokens (SampleLibrary::sampleExtensions, ";", ""))
        CHECK (wildcard.contains ("*." + ext));
}
