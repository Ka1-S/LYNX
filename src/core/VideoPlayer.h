#pragma once

// LYNX Video Player (round 165g, docs/analise/REA_rodada165g.md §7): FL's Fruity Video Player — a generator channel that
// shows a video in sync with the song (and plays its sound). The video is decoded by the user's own ffmpeg (ui/VideoPlayer
// View); this part holds FL's saved state, the clock that turns the song position into the video's time, and the
// instrument that plays the video's sound track. No JUCE here (tests/VideoPlayer165Tests.inc).
//
// FL's behaviour, read in its plugin (Fruity Video Player_x64.dll, SaveRestoreState 0x72d9b0, the options menu and its
// handlers, the time display 0x72bb50) [DECOMP]:
// - state version 3: int version; int flags (bit 0 Loop, bit 1 Keep aspect ratio, bits 2-3 the player's own transport
//   (0 stopped, 1 playing, 2 playing the start…end range, 3 paused), bit 4 Enable audio (bits 4|5 in older states),
//   bit 6 Ignore tempo changes, bit 7 Slave playback to host, bit 8 Dock control panel); int time display; int count (3)
//   and the three parameters (Volume 0…127, default 100; Mute; Play); the video's path (a byte length — 0xFF then an
//   int when ≥ 255 — and UTF-16 characters); two ints (the video window's size); with a path, the start and end times
//   (int64, 100 ns units).
// - FL's default state (Default.fst): flags 0x92 (Keep aspect ratio, Enable audio, Slave playback to host), time display
//   0, Volume 100.
// - "Delay playback until that time" (form +0x770) is not part of FL's state: LYNX appends it after FL's fields
//   ("LYNXvp1" + int64), where FL's reader stops.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lynx
{
class Instrument;
}

namespace lynx::video
{
constexpr std::int64_t unitsPerSecond = 10000000; // FL's times (100 ns)

// FL's time display modes (the menu's tags)
enum TimeDisplay : int
{
    hms = 0, smpte23976 = 1, smpte24 = 2, smpte25 = 3, smpte2997drop = 4, smpte30 = 5, smpteAuto = 6, smpte2997 = 7,
    smpte48 = 8, smpte50 = 9, smpte5994drop = 10, smpte5994 = 11, smpte60 = 12, smpte72 = 13, smpte120 = 14
};
constexpr int timeDisplayCount = 15;
const char* timeDisplayName (int mode) noexcept; // English (the UI translates "Hours:Minutes:Seconds:Ms")
// frames per second of a SMPTE mode (0 for H:M:S:Ms; the video's own rate for Auto) and whether it drops frames
double timeDisplayFps (int mode, double videoFps) noexcept;
bool timeDisplayDrops (int mode) noexcept;
// FL's "0:00:00:000" (H:MM:SS:mmm) or "H:MM:SS:FF" (SMPTE; ";" before the frames when dropping)
std::string formatTime (double seconds, int mode, double videoFps);

struct Settings
{
    std::string path;            // the video (UTF-8)
    bool loop = false;
    bool keepAspect = true;
    int transport = 0;           // FL's own player when not slaved: 0 stopped, 1 playing, 2 playing start…end, 3 paused
    bool enableAudio = true;
    bool ignoreTempo = false;
    bool slave = true;           // Slave playback to host: the song position drives the video
    bool dock = false;           // Dock control panel
    int timeDisplay = hms;
    int volume = 100;            // 0…127
    bool mute = false;
    bool play = false;           // the Play parameter (when not slaved)
    int width = 375, height = 0; // FL's video window size (kept for FL)
    std::int64_t start = 0, end = 0; // 100 ns; end 0 = the video's end
    std::int64_t delay = 0;      // LYNX: the song time where the video starts (100 ns)

    bool operator== (const Settings&) const = default;
};

std::vector<std::uint8_t> write (const Settings& s); // FL's version-3 state + LYNX's tail
std::optional<Settings> read (const std::vector<std::uint8_t>& state); // nullopt: not a Video Player state
Settings readOrDefault (const std::vector<std::uint8_t>& state);

// What the video looks like (ffmpeg's report)
struct Info
{
    double duration = 0.0; // seconds
    double fps = 0.0;
    int width = 0, height = 0;
    bool hasVideo = false, hasAudio = false;
};
// Parses ffmpeg's "-i" report (stderr): Duration, the first video stream's size and rate, an audio stream
Info parseFfmpegReport (const std::string& report);

// The video's time at a song position, FL's clock:
// - slaved, following the tempo: the song's beats at the song's tempo (a tempo change changes the video's speed);
// - slaved, Ignore tempo changes: real time since the play position (seconds), re-anchored at jumps;
// then the delay (the video starts when the song reaches it), the start time, and the end / loop.
struct Clock
{
    // videoSeconds for a song time (s) given as seconds since the song's start; < 0 before the delay
    static double videoTime (double songSeconds, const Settings& s, double duration) noexcept;
};

// The decoded sound track (interleaved stereo int16 at `rate`), shared between the UI and the audio thread
struct AudioTrack
{
    double rate = 48000.0;
    std::vector<std::int16_t> samples; // L R L R …
    std::int64_t frames() const noexcept { return (std::int64_t) samples.size() / 2; }
};

// The UI gives the instrument its sound track by path (ui/VideoPlayerView decodes it with ffmpeg; `wait` while
// exporting). nullptr: not decoded (yet), no audio, or no ffmpeg.
using AudioProvider = std::function<std::shared_ptr<const AudioTrack> (const std::string& path, bool wait)>;
void setAudioProvider (AudioProvider provider);

// The instrument: no notes; plays the sound track at the video's time while the song plays (or while its own
// transport plays when not slaved). Its clock is published for the video window.
class Player;
std::unique_ptr<Instrument> makeInstrument();
Player* playerOf (Instrument* instrument) noexcept;

class Player
{
public:
    virtual ~Player() = default;
    // the video's time the audio thread is at (seconds; < 0 before the delay) and whether it moves
    virtual double currentTime() const noexcept = 0;
    virtual bool running() const noexcept = 0;
    // the player's own transport (not slaved): play from `seconds`, pause, stop (UI thread)
    virtual void ownPlay (double seconds) noexcept = 0;
    virtual void ownPause() noexcept = 0;
    virtual void ownStop() noexcept = 0;
};
} // namespace lynx::video
