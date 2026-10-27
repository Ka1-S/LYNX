#pragma once

// LYNX Dance (round 165e, docs/analise/REA_rodada165e.md): FL's Fruity Dance role — an animated dancer over the
// workspace whose moves the channel's notes pick — with LYNX's own procedural character (no FL artwork). No sound.
//
// FL's behaviour, read in its plugin (REA round 68 §8 and round 165e): a note picks a move (the keys from B4 up: 0 =
// Freestyle, then the moves in order), the keys from C4 down play the same moves mirrored, F4 hides the dancer and F#4
// shows it; the note itself makes no sound and ends at once — the move stays until the next note. The animation follows
// the song position, with a speed setting. The saved state (int version; seven ints: move, mirrored, shown, a scale,
// speed, x, y (the dancer's bottom centre in the main window); one int and four flags of display options; version 2 adds
// a picture path) is read when an FL project is imported.
// No JUCE here (tests/Dance165Tests.inc).

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lynx
{
class Instrument;
}

namespace lynx::dance
{
enum Move : int { freestyle, waiting, stepping, jumping, zombie, waving, hula, windmill, flail, spin, held };
constexpr int moveCount = 11;
const char* moveName (int move) noexcept; // English (the UI translates)

struct Settings
{
    int move = stepping;
    bool mirrored = false;
    bool visible = true;
    float scale = 1.0f;      // size (FL's scale value / 64)
    float speed = 1.0f;      // moves per beat relative to the default
    int x = 412, y = 500;    // the dancer's bottom centre in the main window, in pixels (FL's defaults)
    std::uint32_t colour = 0xffff3048; // the outfit (ARGB)

    bool operator== (const Settings&) const = default;
};

// LYNX's own state ("LYNX dance 1", key=value lines)
std::vector<std::uint8_t> toState (const Settings& s);
std::optional<Settings> fromState (const std::vector<std::uint8_t>& state);
// FL's Fruity Dance state (40 bytes or more); nullopt = not one
std::optional<Settings> fromFl (const std::vector<std::uint8_t>& state);
// either (LYNX's text first, then FL's bytes); the defaults when neither
Settings read (const std::vector<std::uint8_t>& state);

// What a note does (FL's key map)
struct KeyAction
{
    enum class Kind { none, move, hide, show };
    Kind kind = Kind::none;
    int move = 0;
    bool mirrored = false;
};
KeyAction actionForKey (int key) noexcept;
constexpr int firstMoveKey = 59;   // B4: Freestyle; C5 … A5: the moves
constexpr int firstMirrorKey = 48; // C4: Freestyle mirrored; down to D#3
constexpr int hideKey = 53, showKey = 54;

// The body at a moment: angles in radians (0 = hanging down for limbs, upright for the torso), lengths in body units
// (the figure is about 1.0 tall), the origin between the feet.
struct Pose
{
    float x = 0.0f, y = 0.0f;  // the whole body's offset (y up)
    float lean = 0.0f;         // torso tilt (positive = to the right)
    float head = 0.0f;         // head tilt
    float turn = 1.0f;         // −1 … 1: the body's facing (a spin squeezes it through 0)
    float squash = 1.0f;       // vertical squash on landings
    float hipShift = 0.0f;     // hips sideways
    std::array<float, 2> shoulder {}, elbow {}, hip {}, knee {}; // [0] left, [1] right (as seen)
    float tail = 0.0f;

    static Pose mix (const Pose& a, const Pose& b, float t) noexcept;
};
// `cycles`: the move's own clock (one cycle = one beat at speed 1, the song's beats while playing); `seed` varies
// Freestyle's choices. Deterministic.
Pose pose (int move, double cycles, std::uint32_t seed = 0) noexcept;
// Freestyle: the move of cycle `n` (never Waiting or Held)
int freestyleMove (std::int64_t cycleIndex, std::uint32_t seed) noexcept;

// The silent instrument of a dance channel: its notes go to the UI through a small lock-free queue (the audio thread
// only stores the key and counts).
class Signals
{
public:
    void push (int key) noexcept;
    // UI: the keys since `seen` (oldest first, at most 16), `seen` moved on
    int pull (std::uint32_t& seen, int* keys, int max) const noexcept;

private:
    std::array<std::atomic<int>, 16> ring {};
    std::atomic<std::uint32_t> count { 0 };
};
std::unique_ptr<Instrument> makeInstrument();
Signals* signalsOf (Instrument* instrument) noexcept; // nullptr: not a dance channel
} // namespace lynx::dance
