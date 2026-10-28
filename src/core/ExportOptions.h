#pragma once

// The Export window's choices (FL's "WAV rendering" window, interpreted; docs/analise/REA_rodada84.md) and the parts of
// exporting that are plain logic: what song copy is rendered (song / pattern / selection, repeated N times, with a tail),
// what happens to the tail (cut / leave / wrap into the start), normalising, mono, which inserts or channels become
// stems, and how the options are remembered. No JUCE here: the engine side is engine/Exporter.*.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/Song.h"

namespace lynx::exportopts
{
enum class Format : int
{
    wav,
    mp3,
    ogg,
    flac
};

enum class TailMode : int
{
    cut,   // stop exactly at the end (loops)
    leave, // keep the tail after the end until it is silent (songs)
    wrap   // render the tail and mix it into the start, keeping the exact length (seamless loops)
};

enum class Range : int
{
    song,      // the Playlist arrangement
    pattern,   // the current pattern
    selection  // the Playlist's time selection (its loop region)
};

enum class Stems : int
{
    none,
    mixerTracks, // one file per insert that gets sound (FL's "Split mixer tracks")
    channels     // one file per Channel Rack channel that plays something (each heard through its mixer path)
};

enum class DitherMode : int
{
    none,
    tpdf,   // ±1 LSB triangular noise
    shaped  // the same with 2nd-order noise shaping (pushes the noise to the highs)
};

enum class Channels : int
{
    stereo,
    monoMerged,
    monoLeft,
    monoRight
};

struct Options
{
    Format format = Format::wav;
    int wavBits = 24;          // 16, 24 (integer) or 32 (float)
    int flacBits = 24;         // 16 or 24
    int flacLevel = 5;         // compression 0 (fastest) .. 8 (smallest)
    int mp3Kbps = 320;         // constant bitrate
    int oggKbps = 256;         // nominal bitrate
    double sampleRate = 44100.0;
    DitherMode dither = DitherMode::shaped;
    TailMode tail = TailMode::leave;
    double maxTailSeconds = 12.0; // leave / wrap: the tail stops earlier when the sound ends
    Range range = Range::song;
    int loops = 1;                 // how many times the range plays (1..16)
    Stems stems = Stems::none;
    bool stemsIncludeMaster = false; // with stems: also the full mix
    bool normalize = false;          // peak to 0 dBFS
    bool masterEffects = true;       // the Master insert's effects
    bool trimPdc = true;             // drop the silence the delay compensation adds at the start
    // FL's "HQ for all plugins" (REA round 134): engines with a separate render quality use it and HQ switches are
    // forced on; off = the export uses the same (draft) quality as playback, so it matches what was heard
    bool hqAllPlugins = true;
    // "Plugin warm-up" (round 155): hosted plugins get silent, discarded audio before the render until they have
    // settled (fresh instances: bridged plugins / background loading apply their state late), so the file does not
    // start with half-loaded effects
    bool pluginWarmUp = true;
    bool showWhenDone = false;       // FL's "Show file(s) when complete": the file manager opens on the result
    // FL's "Disable maximum polyphony" (on by default; REA round 139, FLEngine FUN_00d1a…: every channel's Max
    // polyphony is set to 0 for the render): the channels' polyphony limits are lifted in the exported song
    bool disableMaxPolyphony = true;
    // more formats written from the same render (FL's WAV / MP3 / OGG / FLAC boxes can be ticked together): a mask of
    // `1 << Format`, besides `format` (whose settings window shows); 0 = only `format`
    std::uint32_t moreFormats = 0;
    Channels channels = Channels::stereo;

    bool operator== (const Options&) const = default;
};

// Choices offered by the window.
const std::vector<int>& mp3Bitrates();  // kbps
const std::vector<int>& oggBitrates();  // kbps (JUCE's Vorbis encoder's settings)
const std::vector<double>& sampleRates();

// ".wav", ".mp3", ".ogg", ".flac"
const char* extension (Format format);

// Every format the export writes, `format` first, then the others of `moreFormats` in WAV, MP3, OGG, FLAC order.
std::vector<Format> formatsOf (const Options& o);

// FL's "LastExportedFormats" (REA round 139, FLEngine FUN_011dd840 / the render window's close): a mask of the ticked
// format boxes, bit 3 WAV, 4 MP3, 5 OGG, 6 FLAC, 7 MIDI, 9 M4A; 8 (WAV) when nothing valid was saved (< 8 or > 0x3FF).
// The File › Export entries open the window with one format (WAV 3, MP3 4, OGG 5, FLAC 6, MIDI 7, M4A 9) or, with
// "Last exported format(s)", the boxes of that mask.
constexpr std::uint32_t flFormatBit (Format f) noexcept
{
    return f == Format::wav ? 0x8u : f == Format::mp3 ? 0x10u : f == Format::ogg ? 0x20u : 0x40u;
}
std::uint32_t flLastExportedMask (const Options& o) noexcept; // the mask of the formats `o` writes
Options withFlLastExported (Options o, std::uint32_t mask);    // `o` with the formats of an FL mask (invalid → WAV)
Options withSingleFormat (Options o, Format f);                // what an FL File › Export › "<format> file..." entry opens

// Bits per sample written to the file (32 = float WAV; MP3/OGG encode floats and report 16).
int fileBits (const Options& o) noexcept;

// Whether dither is applied: integer output of 16 or 24 bits with dither on (MP3/OGG/float never).
bool usesDither (const Options& o) noexcept;

// MP3 is limited to MPEG-1 rates: higher project rates are encoded at 48 kHz (44.1 kHz family → 44.1 kHz).
double mp3SampleRate (double rate) noexcept;

// What the render window shows before the export (FL: "Length: 144 bars  Total time: 3'57''  Disk space: 38.76 MB"):
// the music's length in bars and the file size the options give for `seconds` (one file; the tail not counted;
// FLAC and the lossy formats are estimates).
double lengthInBars (double seconds, double bpm, int beatsPerBar) noexcept;
std::int64_t estimatedBytes (const Options& o, double seconds) noexcept;

// The notes FL's File › Export › "MIDI file..." writes (REA round 139): in song mode, each pattern clip's part of its
// pattern placed at the clip (notes from `offset` to `offset + length`, cut at the clip's end; muted clips / notes /
// playlist tracks left out), sorted; in pattern mode or with no pattern clip, the current pattern's notes.
std::vector<Note> midiExportNotes (const Song& song);

// Remembered between sessions as "key=value" lines; unknown or bad values fall back to the defaults (clamped).
std::string toText (const Options& o);
Options fromText (const std::string& text);
Options clamped (Options o);

// --- the song that is rendered ------------------------------------------------------------------------------------

// Copy of `song` ready to render (always song mode, no loop region), with the range repeated `loops` times and, when
// `tailTicks` > 0, an empty tail after it. `musicTicks` receives the length of the music (all the loops, no tail);
// `loopTicks` one pass. Selection = song.loopStart..loopEnd (falls back to the whole song when there is none).
// Clips crossing the selection's edges are cut there (audio and pattern offsets follow).
Song makeRenderSong (const Song& song, const Options& o, std::int64_t tailTicks, std::int64_t& musicTicks, std::int64_t& loopTicks);

// The selection exists (loop region set and inside the song).
bool hasSelection (const Song& song) noexcept;

// Inserts that get sound: a channel goes to them or another insert sends to them (Master excluded).
std::vector<int> stemTracks (const Song& song);
// FL's Mixer › Disk recording › "Render armed / selected tracks to wave file(s)" (REA round 165f-mixer): the inserts
// of `only` (each once, in mixer order, the Master excluded) whether or not they get sound; empty = stemTracks (song).
std::vector<int> stemTracks (const Song& song, const std::vector<int>& only);

// Channels that play something in the range: notes in a pattern placed in the Playlist (or the current pattern in
// pattern range), or an audio clip.
std::vector<int> stemChannels (const Song& song, Range range);

// Copy of the song where only `channel` plays (every other channel muted); the mixer stays as it is.
Song makeChannelStemSong (const Song& song, int channel);

// "<base> - <number> <name><ext>" with characters that are illegal in file names replaced.
std::string stemFileName (const std::string& base, int number, const std::string& name, const char* ext);

// --- post-processing of the rendered audio --------------------------------------------------------------------------

// Tail handling on the whole render (`frames` per channel): cut drops what follows `loopFrames`; wrap adds it back
// from the start (wrapping more than once if the tail is longer than the loop) and then cuts; leave does nothing.
void applyTail (std::vector<float>& left, std::vector<float>& right, std::int64_t loopFrames, TailMode mode);

// Gain that brings `peak` to full scale (1 when silent).
float normalizeGain (float peak) noexcept;

// Mono choices: the result goes to `left` (right is left untouched for stereo).
void toChannels (float* left, const float* right, int n, Channels mode) noexcept;
inline int channelCount (Channels mode) noexcept { return mode == Channels::stereo ? 2 : 1; }

// "m:ss" (or "h:mm:ss") for the total time line.
std::string formatDuration (double seconds);
} // namespace lynx::exportopts
