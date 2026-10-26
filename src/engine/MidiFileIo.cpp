#include "engine/MidiFileIo.h"

#include <limits>
#include <map>

#include "core/ExportOptions.h"
#include "core/MidiControls.h"
#include "engine/Text.h"

namespace lynx::MidiFileIo
{
namespace
{
struct Parsed
{
    std::vector<std::vector<Note>> tracks; // notes per track (channel = track index)
    std::vector<std::string> names;
    std::vector<std::vector<int>> midiChannels; // per track, per note: its MIDI channel 0..15
    double bpm = 120.0;
    int numerator = 0, denominator = 4; // the first time signature (0 = none)
};

std::optional<Parsed> parse (const juce::File& file, juce::String& error)
{
    juce::FileInputStream in (file);
    juce::MidiFile midi;

    if (! in.openedOk() || ! midi.readFrom (in))
    {
        error = utf8 ("not a valid MIDI file");
        return std::nullopt;
    }

    const auto format = midi.getTimeFormat();

    if (format <= 0)
    {
        // SMPTE: convert to seconds, then to ticks at the default tempo
        midi.convertTimestampTicksToSeconds();
    }

    Parsed out;
    const auto filePpq = format > 0 ? (double) format : 0.0;
    auto toTicks = [&] (double t) {
        return filePpq > 0.0 ? (std::int64_t) std::llround (t * ppq / filePpq) : (std::int64_t) std::llround (t * out.bpm / 60.0 * ppq);
    };

    for (int t = 0; t < midi.getNumTracks(); ++t)
    {
        auto track = *midi.getTrack (t);
        track.updateMatchedPairs();
        std::vector<Note> notes;
        std::vector<int> channels;
        std::string name;

        for (int i = 0; i < track.getNumEvents(); ++i)
        {
            const auto* e = track.getEventPointer (i);
            const auto& m = e->message;

            if (m.isTempoMetaEvent() && out.bpm == 120.0 && m.getTempoSecondsPerQuarterNote() > 0.0)
                out.bpm = juce::jlimit (20.0, 999.0, 60.0 / m.getTempoSecondsPerQuarterNote());
            else if (m.isTimeSignatureMetaEvent() && out.numerator == 0)
            {
                int num = 0, den = 0;
                m.getTimeSignatureInfo (num, den);
                if (num > 0 && den > 0)
                {
                    out.numerator = num;
                    out.denominator = den;
                }
            }
            else if (m.isTrackNameEvent() && name.empty())
                name = m.getTextFromTextMetaEvent().toStdString();
            else if (m.isNoteOn())
            {
                Note n;
                n.start = toTicks (m.getTimeStamp());
                const auto end = e->noteOffObject != nullptr ? toTicks (e->noteOffObject->message.getTimeStamp()) : n.start + ticksPerStep;
                n.length = std::max<std::int64_t> (1, end - n.start);
                n.key = m.getNoteNumber();
                n.velocity = m.getFloatVelocity();
                n.colour = juce::jlimit (0, noteColourCount - 1, m.getChannel() - 1); // FL: MIDI channel = note colour
                notes.push_back (n);
                channels.push_back (juce::jlimit (0, 15, m.getChannel() - 1));
            }
        }

        if (! notes.empty())
        {
            out.tracks.push_back (std::move (notes));
            out.names.push_back (name);
            out.midiChannels.push_back (std::move (channels));
        }
    }

    if (out.tracks.empty())
    {
        error = utf8 ("the file has no notes");
        return std::nullopt;
    }

    return out;
}
} // namespace

std::optional<Song> loadSong (const juce::File& file, juce::String& error)
{
    auto parsed = parse (file, error);

    if (! parsed)
        return std::nullopt;

    Song song;
    song.bpm = parsed->bpm;
    song.mixer = Song::defaultMixer();
    auto& pattern = song.patterns.emplace_back();
    pattern.name = file.getFileNameWithoutExtension().toStdString();

    for (std::size_t t = 0; t < parsed->tracks.size(); ++t)
    {
        ChannelDef c;
        c.name = parsed->names[t].empty() ? "MIDI " + std::to_string (t + 1) : parsed->names[t];
        c.instrument = InstrumentKind::synth;
        c.mixerTrack = song.claimFreeInsert();
        c.volume = 0.6f;
        song.channels.push_back (c);

        for (auto n : parsed->tracks[t])
        {
            n.channel = (int) t;
            pattern.notes.push_back (n);
        }
    }

    pattern.sortNotes();
    return song;
}

std::optional<midiimport::ParsedMidi> parseFile (const juce::File& file, juce::String& error)
{
    juce::FileInputStream in (file);
    juce::MidiFile midi;
    if (! in.openedOk() || ! midi.readFrom (in))
    {
        error = utf8 ("not a valid MIDI file");
        return std::nullopt;
    }
    const auto format = midi.getTimeFormat();
    if (format <= 0)
        midi.convertTimestampTicksToSeconds();
    midiimport::ParsedMidi out;
    const auto filePpq = format > 0 ? (double) format : 0.0;
    auto toTicks = [&] (double t) {
        return filePpq > 0.0 ? (std::int64_t) std::llround (t * ppq / filePpq) : (std::int64_t) std::llround (t * out.bpm / 60.0 * ppq);
    };
    for (int t = 0; t < midi.getNumTracks(); ++t)
    {
        const auto& track = *midi.getTrack (t);
        std::string name;
        for (int i = 0; i < track.getNumEvents(); ++i)
        {
            const auto& m = track.getEventPointer (i)->message;
            if (m.isTempoMetaEvent() && out.bpm == 120.0 && m.getTempoSecondsPerQuarterNote() > 0.0)
                out.bpm = juce::jlimit (20.0, 999.0, 60.0 / m.getTempoSecondsPerQuarterNote());
            else if (m.isTimeSignatureMetaEvent() && out.numerator == 0)
            {
                int num = 0, den = 0;
                m.getTimeSignatureInfo (num, den);
                if (num > 0 && den > 0)
                {
                    out.numerator = num;
                    out.denominator = den;
                }
            }
            else if (m.isTrackNameEvent() && name.empty())
                name = m.getTextFromTextMetaEvent().toStdString();
            else if (m.isNoteOnOrOff() || m.isController())
            {
                const auto* raw = m.getRawData();
                // a running-status note off may come as a note on with velocity 0: kept as it is (the import decides)
                out.events.push_back ({ t, toTicks (m.getTimeStamp()), raw[0], raw[1], m.getRawDataSize() > 2 ? raw[2] : (std::uint8_t) 0 });
            }
        }
        out.trackNames.push_back (name);
    }
    return out;
}

bool importInto (Song& song, const juce::File& file, const ImportOptions& o, juce::String& error)
{
    auto parsed = parseFile (file, error);
    if (! parsed)
        return false;
    std::string err;
    if (! midiimport::importInto (song, *parsed, o, file.getFileNameWithoutExtension().toStdString(), err))
    {
        error = utf8 (err.c_str());
        return false;
    }
    return true;
}

std::optional<std::vector<Note>> loadNotes (const juce::File& file, int channel, juce::String& error)
{
    auto parsed = parse (file, error);

    if (! parsed)
        return std::nullopt;

    std::vector<Note> notes;

    for (const auto& track : parsed->tracks)
        for (auto n : track)
        {
            n.channel = channel;
            notes.push_back (n);
        }

    std::stable_sort (notes.begin(), notes.end(), [] (const Note& a, const Note& b) { return a.start < b.start; });
    return notes;
}

bool savePattern (const Song& song, int patternIndex, const juce::File& file, juce::String& error)
{
    if (patternIndex < 0 || patternIndex >= (int) song.patterns.size())
    {
        error = "pattern does not exist";
        return false;
    }

    const auto& pattern = song.patterns[(std::size_t) patternIndex];
    std::map<int, juce::MidiMessageSequence> byChannel;

    for (const auto& n : pattern.notes)
    {
        auto& seq = byChannel[n.channel];
        const auto velocity = (juce::uint8) juce::jlimit (1, 127, juce::roundToInt (n.velocity * 127.0f));
        const auto midiChannel = juce::jlimit (1, 16, n.colour + 1); // FL: the note colour is the MIDI channel
        seq.addEvent (juce::MidiMessage::noteOn (midiChannel, n.key, velocity), (double) n.start);
        seq.addEvent (juce::MidiMessage::noteOff (midiChannel, n.key), (double) (n.start + std::max<std::int64_t> (1, n.length)));
    }

    juce::MidiFile midi;
    midi.setTicksPerQuarterNote (ppq);

    juce::MidiMessageSequence tempo;
    tempo.addEvent (juce::MidiMessage::tempoMetaEvent (juce::roundToInt (60'000'000.0 / song.bpm)), 0.0);
    tempo.addEvent (juce::MidiMessage::timeSignatureMetaEvent (song.beatsPerBar, 4), 0.0);
    midi.addTrack (tempo);

    for (auto& [channel, seq] : byChannel)
    {
        const auto name = channel >= 0 && channel < (int) song.channels.size() ? song.channels[(std::size_t) channel].name : std::string ("Channel");
        seq.addEvent (juce::MidiMessage::textMetaEvent (3, juce::String::fromUTF8 (name.c_str())), 0.0);
        seq.sort();
        seq.updateMatchedPairs();
        midi.addTrack (seq);
    }

    file.deleteFile();
    juce::FileOutputStream out (file);

    if (out.failedToOpen() || ! midi.writeTo (out))
    {
        error = utf8 ("could not write ") + file.getFullPathName();
        return false;
    }

    return true;
}

bool saveSong (const Song& song, const juce::File& file, juce::String& error)
{
    // the arrangement flattened into one pattern, written like a pattern (one track per channel)
    Song flat;
    flat.bpm = song.bpm;
    flat.beatsPerBar = song.beatsPerBar;
    flat.channels = song.channels;
    flat.patterns.resize (1);
    flat.patterns[0].notes = exportopts::midiExportNotes (song);
    return savePattern (flat, 0, file, error);
}
} // namespace lynx::MidiFileIo
