#pragma once

// LYNX's key map: every keyboard shortcut lives in one remappable table (docs/WORKFLOW_FL.md,
// P1 item 14). Platform-neutral (no JUCE): the UI converts its key events into a Chord, asks
// the map which action it triggers in the focused panel, and runs it.
//
// Typing piano: while it is on, the unmodified keys of the piano layout play notes and win over any
// shortcut bound to them; other plain printable keys only work for actions marked `whilePiano`.
// Modified chords (Ctrl/Alt), F-keys and the non-printable keys (Space, Enter, arrows, Delete...)
// always work.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#ifndef TR_NOOP
 #define TR_NOOP(text) text // marks a literal for translation (tools/i18n_keys.py scans core too)
#endif

namespace lynx::keys
{
// Key codes: printable keys use their upper-case ASCII code ('A', '1', '+'); the others these values.
enum Key : int
{
    space = ' ',
    f1 = 0x10001, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12,
    up = 0x10020, down, left, right, pageUp, pageDown, home, end,
    enter = 0x10030, escape, tab, backspace, del, insert,
    numpadPlus = 0x10040, numpadMinus,
    numpad0 = 0x10050, numpad1, numpad2, numpad3, numpad4, numpad5, numpad6, numpad7, numpad8, numpad9
};

enum Modifier : std::uint8_t
{
    none = 0,
    ctrl = 1,
    shift = 2,
    alt = 4
};

struct Chord
{
    int key = 0;
    std::uint8_t mods = none;

    bool operator== (const Chord&) const = default;
    bool isPrintable() const noexcept; // a plain character key (letter, digit, punctuation), Space excluded
    bool isValid() const noexcept { return key != 0; }
};

// "Ctrl+Shift+F4", "Alt+Up", "Numpad +", "Space"... (English key names, used in the JSON file and menus)
std::string toString (const Chord& chord);
std::optional<Chord> parseChord (const std::string& text);

// Where a binding applies. Global bindings work everywhere; the others only while that panel has focus
// (and win over a global binding of the same chord).
enum class Context : std::uint8_t { global, channelRack, pianoRoll, playlist, mixer, browser };

struct Action
{
    const char* id;          // stable identifier ("pattern.next"), the key of the JSON file
    const char* description; // English, translated by the UI
    Context context;
    std::vector<Chord> defaults;
    bool whilePiano = false; // a plain printable key that keeps working while the typing piano is on
};

// The built-in actions with LYNX's default chords.
const std::vector<Action>& actions();
const Action* findAction (const std::string& id);

// PC keyboard as a piano: MIDI note of a key (upper-case code), or -1. Two rows from C5 (Z S X D C V G
// B H N J M) and from C6 (Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P), with C5 = MIDI 60.
int typingPianoNote (int key) noexcept;

// The upper-case printable character of a key code (JUCE's getKeyCode()), or 0 for any other key. On Windows JUCE's
// F-keys, numpad keys and arrows are VK | 0x10000: a plain towupper() of them (16-bit) gave F4 -> 'S', F1 -> 'P',
// Numpad1 -> 'A', so the editors took them for tool letters (issue #12, found by round 165q).
constexpr int letterOfKeyCode (int code) noexcept
{
    return code > ' ' && code < 0x7f ? (code >= 'a' && code <= 'z' ? code - 'a' + 'A' : code) : 0;
}

class KeyMap
{
public:
    KeyMap(); // the defaults

    // Action triggered by `chord` with `focus` focused; "" for none. `typingPiano`: the piano is on.
    std::string find (const Chord& chord, Context focus, bool typingPiano) const;

    // Like find(), but only an action the user gave `chord` himself (not one of its default keys). The main window
    // runs it before the panels' own keys, so a key you chose always does what you chose (issue #12: Metronome on
    // 'C' did nothing because the focused Playlist took 'C' for its Knife).
    std::string findUserBound (const Chord& chord, Context focus, bool typingPiano) const;
    // The actions of the other places (global vs a panel) bound to `chord` too: where both apply, the user's own key
    // wins over a default one, else the panel's (the Shortcut manager tells which).
    std::vector<std::string> shadowedBy (const Chord& chord, const std::string& actionId) const;

    // True when the chord plays a note (piano on, unmodified, mapped key).
    static bool playsNote (const Chord& chord, bool typingPiano) noexcept;

    const std::vector<Chord>& chordsFor (const std::string& actionId) const;
    void setChords (const std::string& actionId, std::vector<Chord> chords); // [] = unbound
    void resetToDefaults();

    // Overrides from a JSON object {"action.id": "Ctrl+L", "other.id": ["F4", "Shift+F4"], "x": ""}.
    // Unknown ids and unreadable chords are reported in `errors` and skipped. Returns false if the
    // text is not a JSON object.
    bool loadJson (const std::string& json, std::vector<std::string>* errors = nullptr);
    std::string toJson() const; // every action with its current chords

    // Shortcut manager: the actions other than `except` that `chord` triggers in `context` (same context only:
    // a panel's binding wins over a global one there, so those don't clash).
    std::vector<std::string> actionsWith (const Chord& chord, Context context, const std::string& except = {}) const;
    bool isDefault (const std::string& actionId) const;
    // Only the actions whose chords differ from the defaults, as loadJson reads them (what keys.json keeps).
    std::string overridesJson() const;

    // Pairs of actions bound to the same chord in overlapping contexts (for the help sheet / tests).
    std::vector<std::pair<std::string, std::string>> conflicts() const;

private:
    std::vector<std::vector<Chord>> chords; // parallel to actions()
};
} // namespace lynx::keys
