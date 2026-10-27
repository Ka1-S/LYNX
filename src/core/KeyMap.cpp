#include "core/KeyMap.h"

#include <algorithm>
#include <cctype>

namespace lynx::keys
{
namespace
{
struct KeyName
{
    int key;
    const char* name;
};

const KeyName keyNames[] = {
    { space, "Space" },        { enter, "Enter" },     { escape, "Esc" },      { tab, "Tab" },           { backspace, "Backspace" },
    { del, "Delete" },         { insert, "Insert" },   { up, "Up" },           { down, "Down" },         { left, "Left" },
    { right, "Right" },        { pageUp, "PageUp" },   { pageDown, "PageDown" }, { home, "Home" },       { end, "End" },
    { numpadPlus, "Numpad +" }, { numpadMinus, "Numpad -" },
    { numpad0, "Numpad 0" }, { numpad1, "Numpad 1" }, { numpad2, "Numpad 2" }, { numpad3, "Numpad 3" }, { numpad4, "Numpad 4" },
    { numpad5, "Numpad 5" }, { numpad6, "Numpad 6" }, { numpad7, "Numpad 7" }, { numpad8, "Numpad 8" }, { numpad9, "Numpad 9" },
};

std::string lower (std::string s)
{
    for (auto& c : s)
        c = (char) std::tolower ((unsigned char) c);
    return s;
}

std::string trim (const std::string& s)
{
    const auto a = s.find_first_not_of (" \t\r\n");
    const auto b = s.find_last_not_of (" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr (a, b - a + 1);
}

Chord chord (int key, std::uint8_t mods = none)
{
    return { key, mods };
}

std::vector<Action> makeActions()
{
    std::vector<Action> list {
        // transport and modes
        { "transport.playStop", TR_NOOP ("Play / stop (back to where playback started)"), Context::global, { chord (space) } },
        { "transport.pause", TR_NOOP ("Play / pause"), Context::global, { chord (space, ctrl) } },
        { "transport.toggleMode", TR_NOOP ("Pattern / song mode"), Context::global, { chord ('L') }, true },
        { "view.typingPiano", TR_NOOP ("Typing keyboard as a piano on/off"), Context::global, { chord ('T', ctrl) } },
        { "view.toolbar", TR_NOOP ("Show or hide the toolbar panels"), Context::global, { chord (f11, ctrl) } },
        { "view.audioEditor", TR_NOOP ("Open the audio editor"), Context::global, {} },
        { "view.tempoTapper", TR_NOOP ("Tempo tapper (tap the tempo)"), Context::global, { chord (f5, ctrl) } },
        { "view.sampleBrowser", TR_NOOP ("Sample browser (separate window)"), Context::global, {} },

        // workspace panels
        { "panel.playlist", TR_NOOP ("Show, focus or hide the Playlist"), Context::global, { chord (f5) } },
        { "panel.channelRack", TR_NOOP ("Show, focus or hide the Channel Rack"), Context::global, { chord (f6) } },
        { "panel.pianoRoll", TR_NOOP ("Show, focus or hide the Piano Roll"), Context::global, { chord (f7) } },
        { "panel.browser", TR_NOOP ("Show, focus or hide the Browser"), Context::global, { chord (f8, alt) } },
        { "panel.quickAdd", TR_NOOP ("Quick-add palette: instruments and effects"), Context::global, { chord (f8) } },
        { "panel.mixer", TR_NOOP ("Show, focus or hide the Mixer"), Context::global, { chord (f9) } },
        // FL 2026's other window keys (its shortcut list, REA round 165a)
        { "view.pluginDatabase", TR_NOOP ("Plugin database (in the Browser)"), Context::global, { chord (f8, shift) } },
        { "view.midiSettings", TR_NOOP ("MIDI settings"), Context::global, { chord (f10) } },
        { "view.projectInfo", TR_NOOP ("Project info"), Context::global, { chord (f11) } },
        { "view.closeWindows", TR_NOOP ("Close all windows (plugins, channel settings, tools)"), Context::global, { chord (f12) } },
        { "view.closePluginWindows", TR_NOOP ("Close all plugin windows"), Context::global, { chord (f12, alt) } },
        { "panel.maximisePlaylist", TR_NOOP ("Maximize or restore the Playlist"), Context::global, { chord (enter) } },

        // patterns
        { "pattern.next", TR_NOOP ("Next pattern"), Context::global, { chord (numpadPlus) } },
        { "pattern.previous", TR_NOOP ("Previous pattern"), Context::global, { chord (numpadMinus) } },
        { "pattern.nextEmpty", TR_NOOP ("Next empty pattern (and name it)"), Context::global, { chord (f4) } },
        { "pattern.rename", TR_NOOP ("Rename and color the pattern"), Context::global, { chord (f2) } },
        { "pattern.insert", TR_NOOP ("Insert an empty pattern before the current one"), Context::global, { chord (insert, ctrl | shift) } },
        { "pattern.clone", TR_NOOP ("Clone the current pattern"), Context::global, { chord ('C', alt) } },
        { "pattern.moveUp", TR_NOOP ("Move the current pattern up"), Context::global, { chord (up, ctrl | shift) } },
        { "pattern.moveDown", TR_NOOP ("Move the current pattern down"), Context::global, { chord (down, ctrl | shift) } },

        // recording
        { "transport.record", TR_NOOP ("Recording on/off"), Context::global, { chord ('R') } },
        { "transport.metronome", TR_NOOP ("Metronome on/off"), Context::global, { chord ('M', ctrl) } },
        { "record.countIn", TR_NOOP ("Count-in before recording on/off"), Context::global, { chord ('P', ctrl) } },
        { "record.waitForInput", TR_NOOP ("Recording waits for the first note on/off"), Context::global, { chord ('I', ctrl) } },
        { "record.quickAudio", TR_NOOP ("Record audio now: arm the selected insert and record (press again to stop)"), Context::global, { chord ('R', shift) } },

        // files and history
        { "file.new", TR_NOOP ("New project"), Context::global, { chord ('N', ctrl) } },
        { "file.open", TR_NOOP ("Open project"), Context::global, { chord ('O', ctrl) } },
        { "file.save", TR_NOOP ("Save project"), Context::global, { chord ('S', ctrl) } },
        { "file.saveAs", TR_NOOP ("Save project as"), Context::global, { chord ('S', ctrl | shift) } },
        { "file.saveNewVersion", TR_NOOP ("Save new version (Song_2, Song_3...)"), Context::global, { chord ('S', ctrl | alt) } },
        { "file.export", TR_NOOP ("Export audio"), Context::global, { chord ('R', ctrl) } },
        { "edit.undo", TR_NOOP ("Undo"), Context::global, { chord ('Z', ctrl) } },
        { "edit.redo", TR_NOOP ("Redo"), Context::global, { chord ('Y', ctrl), chord ('Z', ctrl | shift) } },
        { "edit.history", TR_NOOP ("Undo history"), Context::global, { chord ('H', ctrl) } },
        { "help.shortcuts", TR_NOOP ("Shortcut sheet"), Context::global, { chord (f1) } },
        { "browser.search", TR_NOOP ("Search the Browser"), Context::global, { chord ('F', ctrl) } },

        // Channel Rack (focused)
        { "rack.selectPrevious", TR_NOOP ("Select the channel above"), Context::channelRack, { chord (up) } },
        { "rack.selectNext", TR_NOOP ("Select the channel below"), Context::channelRack, { chord (down) } },
        { "rack.extendUp", TR_NOOP ("Extend the selection up"), Context::channelRack, { chord (up, shift) } },
        { "rack.extendDown", TR_NOOP ("Extend the selection down"), Context::channelRack, { chord (down, shift) } },
        { "rack.moveUp", TR_NOOP ("Move the selected channels up"), Context::channelRack, { chord (up, alt) } },
        { "rack.moveDown", TR_NOOP ("Move the selected channels down"), Context::channelRack, { chord (down, alt) } },
        { "rack.selectAll", TR_NOOP ("Select all channels"), Context::channelRack, { chord ('A', ctrl) } },
        { "rack.clone", TR_NOOP ("Clone the selected channels"), Context::channelRack, { chord ('C', alt) } },
        { "rack.deleteSelected", TR_NOOP ("Delete the selected channels"), Context::channelRack, { chord (del, alt) } },
        { "rack.sendToInserts", TR_NOOP ("Send the selected channels to free mixer inserts"), Context::channelRack, { chord ('L', ctrl) } },
        { "rack.graphEditor", TR_NOOP ("Step graph lanes on/off"), Context::channelRack, { chord ('K', ctrl) } },
        { "rack.pianoRollView", TR_NOOP ("Piano-roll view of all channels on/off"), Context::channelRack, {} },
        { "rack.groupSelected", TR_NOOP ("Put the selected channels in a new filter group"), Context::channelRack, { chord ('G', alt) } },
        { "rack.rotateLeft", TR_NOOP ("Rotate the selected channels' steps left"), Context::channelRack, { chord (left, ctrl | shift) } },
        { "rack.rotateRight", TR_NOOP ("Rotate the selected channels' steps right"), Context::channelRack, { chord (right, ctrl | shift) } },

        // Piano Roll (focused): tool dialogs and zoom
        { "pianoRoll.arpeggiate", TR_NOOP ("Arpeggiate..."), Context::pianoRoll, { chord ('A', alt) } },
        { "pianoRoll.strum", TR_NOOP ("Strum..."), Context::pianoRoll, { chord ('S', alt) } },
        { "pianoRoll.flip", TR_NOOP ("Flip..."), Context::pianoRoll, { chord ('Y', alt) } },
        { "pianoRoll.randomise", TR_NOOP ("Randomise..."), Context::pianoRoll, { chord ('R', alt) } },
        { "pianoRoll.scaleLevels", TR_NOOP ("Scale levels..."), Context::pianoRoll, { chord ('X', alt) } },
        { "pianoRoll.limit", TR_NOOP ("Limit to a range..."), Context::pianoRoll, { chord ('K', alt) } },
        // the other tools with FL's keys (REA round 143: the piano roll Tools menu)
        { "pianoRoll.quantize", TR_NOOP ("Quantize..."), Context::pianoRoll, { chord ('Q', alt) } },
        { "pianoRoll.chop", TR_NOOP ("Chop..."), Context::pianoRoll, { chord ('U', alt) } },
        { "pianoRoll.flam", TR_NOOP ("Flam..."), Context::pianoRoll, { chord ('F', alt) } },
        { "pianoRoll.claw", TR_NOOP ("Claw machine..."), Context::pianoRoll, { chord ('W', alt) } },
        { "pianoRoll.lfo", TR_NOOP ("LFO..."), Context::pianoRoll, { chord ('O', alt) } },
        { "pianoRoll.articulate", TR_NOOP ("Articulate..."), Context::pianoRoll, { chord ('L', alt) } },
        { "pianoRoll.riffMachine", TR_NOOP ("Riff machine..."), Context::pianoRoll, { chord ('E', alt) } },
        { "pianoRoll.chordProgression", TR_NOOP ("Generate chord progression..."), Context::pianoRoll, { chord ('P', alt) } },
        // note colours and the Select menu (REA round 153: FL's keys from its piano roll manual)
        { "pianoRoll.colourSelected", TR_NOOP ("Colour the selected notes with the note colour"), Context::pianoRoll, { chord ('C', alt) } },
        { "pianoRoll.selectByColour", TR_NOOP ("Select the notes of the same colour"), Context::pianoRoll, { chord ('C', shift) } },
        { "pianoRoll.invertSelection", TR_NOOP ("Invert the note selection"), Context::pianoRoll, { chord ('I', shift) } },
        { "pianoRoll.groupNotes", TR_NOOP ("Group the selected notes"), Context::pianoRoll, { chord ('G', shift) } },
        { "pianoRoll.ungroupNotes", TR_NOOP ("Ungroup the selected notes"), Context::pianoRoll, { chord ('G', alt) } },
        { "pianoRoll.zoomIn", TR_NOOP ("Zoom in"), Context::pianoRoll, { chord (pageUp) } },
        { "pianoRoll.zoomOut", TR_NOOP ("Zoom out"), Context::pianoRoll, { chord (pageDown) } },
        { "pianoRoll.zoom1", TR_NOOP ("Zoom: one bar"), Context::pianoRoll, { chord ('1', shift) }, true },
        { "pianoRoll.zoom2", TR_NOOP ("Zoom: four bars"), Context::pianoRoll, { chord ('2', shift) }, true },
        { "pianoRoll.zoom3", TR_NOOP ("Zoom: sixteen bars"), Context::pianoRoll, { chord ('3', shift) }, true },
        { "pianoRoll.zoomAll", TR_NOOP ("Zoom to show everything"), Context::pianoRoll, { chord ('4', shift) }, true },
        { "pianoRoll.zoomSelection", TR_NOOP ("Zoom to the selection"), Context::pianoRoll, { chord ('5', shift) }, true },
        { "pianoRoll.centrePlayhead", TR_NOOP ("Centre the view on the playhead"), Context::pianoRoll, { chord ('0', shift) }, true },
        // quick quantize (round 162, issue #9): FL's Ctrl+Q / Shift+Q, plus a 50% one to give a key of your own
        { "pianoRoll.quickQuantize", TR_NOOP ("Quick quantize (starts and lengths)"), Context::pianoRoll, { chord ('Q', ctrl) } },
        { "pianoRoll.quantizeStarts", TR_NOOP ("Quantize starts"), Context::pianoRoll, { chord ('Q', shift) }, true },
        { "pianoRoll.quantizeHalf", TR_NOOP ("Quantize starts 50% (halfway to the grid)"), Context::pianoRoll, {} },
        { "pianoRoll.follow", TR_NOOP ("Follow the playhead (auto scrolling) on/off"), Context::pianoRoll, {} },

        // Playlist (focused)
        { "playlist.zoomIn", TR_NOOP ("Zoom in"), Context::playlist, { chord (pageUp) } },
        { "playlist.zoomOut", TR_NOOP ("Zoom out"), Context::playlist, { chord (pageDown) } },
        { "playlist.zoom1", TR_NOOP ("Zoom: one bar"), Context::playlist, { chord ('1', shift) }, true },
        { "playlist.zoom2", TR_NOOP ("Zoom: four bars"), Context::playlist, { chord ('2', shift) }, true },
        { "playlist.zoom3", TR_NOOP ("Zoom: sixteen bars"), Context::playlist, { chord ('3', shift) }, true },
        { "playlist.zoomAll", TR_NOOP ("Zoom to show everything"), Context::playlist, { chord ('4', shift) }, true },
        { "playlist.zoomSelection", TR_NOOP ("Zoom to the selection"), Context::playlist, { chord ('5', shift) }, true },
        { "playlist.centrePlayhead", TR_NOOP ("Centre the view on the playhead"), Context::playlist, { chord ('0', shift) }, true },
        { "playlist.rippleDelete", TR_NOOP ("Delete the selected clips and close the gaps"), Context::playlist, { chord (del, shift) } },
        { "playlist.ripple", TR_NOOP ("Ripple editing on/off"), Context::playlist, { chord ('S', shift) } },
        { "playlist.quantizeStarts", TR_NOOP ("Quantize the clips' starts"), Context::playlist, { chord ('Q', ctrl) } },
        { "playlist.quantizeHalf", TR_NOOP ("Quantize the clips' starts 50% (halfway to the grid)"), Context::playlist, {} },
        { "playlist.follow", TR_NOOP ("Follow the playhead (auto scrolling) on/off"), Context::playlist, {} },

        // Browser (focused)
        { "browser.previous", TR_NOOP ("Previous file (preview)"), Context::browser, { chord (up) } },
        { "browser.next", TR_NOOP ("Next file (preview)"), Context::browser, { chord (down) } },
        { "browser.swapPrevious", TR_NOOP ("Previous file into the selected channel"), Context::browser, { chord (up, shift) } },
        { "browser.swapNext", TR_NOOP ("Next file into the selected channel"), Context::browser, { chord (down, shift) } },
        { "browser.load", TR_NOOP ("Load the file into the selected channel"), Context::browser, { chord (enter) } },
        { "browser.closeFolder", TR_NOOP ("Close the folder (or go to the parent folder)"), Context::browser, { chord (left) } },
        { "browser.openFolder", TR_NOOP ("Open the folder"), Context::browser, { chord (right) } },
        { "browser.rename", TR_NOOP ("Rename the selected plugin (Plugin database)"), Context::browser, { chord (f2) } },

        // Mixer (focused)
        { "mixer.moveLeft", TR_NOOP ("Move the selected inserts left"), Context::mixer, { chord (left, alt) } },
        { "mixer.moveRight", TR_NOOP ("Move the selected inserts right"), Context::mixer, { chord (right, alt) } },
        { "mixer.linkChannels", TR_NOOP ("Link the selected channels to the selected insert"), Context::mixer, { chord ('L', ctrl) } },
        { "mixer.linkChannelsFrom", TR_NOOP ("Link the selected channels to inserts starting from the selected one"), Context::mixer, { chord ('L', ctrl | shift) } },
        { "mixer.selectLinkedChannels", TR_NOOP ("Select the channels linked to the selected insert"), Context::mixer, { chord ('L', alt) } },
        { "mixer.deleteEffect", TR_NOOP ("Delete the selected effect slot's plugin"), Context::mixer, { chord (del) } },
    };

    // numeric keypad 1-9: select patterns 1-9
    static const char* patternIds[] = { "pattern.select1", "pattern.select2", "pattern.select3", "pattern.select4", "pattern.select5",
                                        "pattern.select6", "pattern.select7", "pattern.select8", "pattern.select9" };

    for (int i = 0; i < 9; ++i)
        list.push_back ({ patternIds[i], TR_NOOP ("Select pattern {0}"), Context::global, { chord (numpad1 + i) } });

    // number keys: mute (plain) / solo (Ctrl) channels 1-10 while the rack has focus
    static const char* muteIds[] = { "rack.mute1", "rack.mute2", "rack.mute3", "rack.mute4", "rack.mute5",
                                     "rack.mute6", "rack.mute7", "rack.mute8", "rack.mute9", "rack.mute10" };
    static const char* soloIds[] = { "rack.solo1", "rack.solo2", "rack.solo3", "rack.solo4", "rack.solo5",
                                     "rack.solo6", "rack.solo7", "rack.solo8", "rack.solo9", "rack.solo10" };

    for (int i = 0; i < 10; ++i)
    {
        const auto digit = i == 9 ? '0' : (char) ('1' + i);
        list.push_back ({ muteIds[i], TR_NOOP ("Mute or unmute channel {0}"), Context::channelRack, { chord (digit) } });
        list.push_back ({ soloIds[i], TR_NOOP ("Solo channel {0}"), Context::channelRack, { chord (digit, ctrl) } });
    }

    return list;
}

bool contextsOverlap (Context a, Context b)
{
    return a == b;
}

// --- tiny JSON reader for {"id": "chord" | ["chord", ...], ...} ---------------------------------
struct Json
{
    const std::string& s;
    std::size_t i = 0;

    void skip()
    {
        while (i < s.size() && std::isspace ((unsigned char) s[i]))
            ++i;
    }

    bool eat (char c)
    {
        skip();
        if (i < s.size() && s[i] == c)
        {
            ++i;
            return true;
        }
        return false;
    }

    std::optional<std::string> string()
    {
        skip();
        if (i >= s.size() || s[i] != '"')
            return std::nullopt;

        std::string out;
        for (++i; i < s.size(); ++i)
        {
            const auto c = s[i];
            if (c == '"')
            {
                ++i;
                return out;
            }
            if (c == '\\' && i + 1 < s.size())
            {
                const auto n = s[++i];
                out += n == 'n' ? '\n' : n == 't' ? '\t' : n;
            }
            else
                out += c;
        }
        return std::nullopt;
    }
};
} // namespace

bool Chord::isPrintable() const noexcept
{
    return key > ' ' && key < 0x7f;
}

std::string toString (const Chord& c)
{
    std::string out;
    if (c.mods & ctrl)
        out += "Ctrl+";
    if (c.mods & shift)
        out += "Shift+";
    if (c.mods & alt)
        out += "Alt+";

    if (c.key >= f1 && c.key <= f12)
        return out + "F" + std::to_string (c.key - f1 + 1);

    for (const auto& k : keyNames)
        if (k.key == c.key)
            return out + k.name;

    if (c.isPrintable())
        return out + std::string (1, (char) c.key);

    return out + "?";
}

std::optional<Chord> parseChord (const std::string& text)
{
    auto rest = trim (text);
    Chord c;

    // modifiers first ("+" alone, or "Numpad +", is a key, so only split on "+" followed by more text)
    for (;;)
    {
        const auto l = lower (rest);
        auto take = [&] (const char* prefix, std::uint8_t mod) {
            const std::string p (prefix);
            if (l.size() > p.size() && l.compare (0, p.size(), p) == 0)
            {
                c.mods |= mod;
                rest = trim (rest.substr (p.size()));
                return true;
            }
            return false;
        };

        if (! (take ("ctrl+", ctrl) || take ("shift+", shift) || take ("alt+", alt)))
            break;
    }

    const auto name = lower (rest);

    if (name.empty())
        return std::nullopt;

    if (name.size() >= 2 && name[0] == 'f' && std::all_of (name.begin() + 1, name.end(), [] (char ch) { return std::isdigit ((unsigned char) ch); }))
    {
        const auto n = std::stoi (name.substr (1));
        if (n < 1 || n > 12)
            return std::nullopt;
        c.key = f1 + n - 1;
        return c;
    }

    for (const auto& k : keyNames)
        if (lower (k.name) == name)
        {
            c.key = k.key;
            return c;
        }

    if (name == "del")
        c.key = del;
    else if (name == "ins")
        c.key = insert;
    else if (name == "escape")
        c.key = escape;
    else if (name == "return")
        c.key = enter;
    else if (rest.size() == 1 && rest[0] > ' ' && rest[0] < 0x7f)
        c.key = std::toupper ((unsigned char) rest[0]);
    else
        return std::nullopt;

    return c;
}

const std::vector<Action>& actions()
{
    static const auto list = makeActions();
    return list;
}

const Action* findAction (const std::string& id)
{
    for (const auto& a : actions())
        if (id == a.id)
            return &a;

    return nullptr;
}

int typingPianoNote (int key) noexcept
{
    static const char lowerRow[] = "ZSXDCVGBHNJM";
    static const char upperRow[] = "Q2W3ER5T6Y7UI9O0P";
    const auto c = key >= 'a' && key <= 'z' ? key - 'a' + 'A' : key;

    for (int i = 0; lowerRow[i] != 0; ++i)
        if (lowerRow[i] == c)
            return 60 + i;

    for (int i = 0; upperRow[i] != 0; ++i)
        if (upperRow[i] == c)
            return 72 + i;

    return -1;
}

KeyMap::KeyMap()
{
    resetToDefaults();
}

void KeyMap::resetToDefaults()
{
    chords.clear();

    for (const auto& a : actions())
        chords.push_back (a.defaults);
}

bool KeyMap::playsNote (const Chord& c, bool typingPiano) noexcept
{
    return typingPiano && (c.mods & (ctrl | alt | shift)) == 0 && typingPianoNote (c.key) >= 0;
}

std::string KeyMap::find (const Chord& c, Context focus, bool typingPiano) const
{
    if (! c.isValid() || playsNote (c, typingPiano))
        return {};

    const auto plainPrintable = c.isPrintable() && (c.mods & (ctrl | alt)) == 0;
    const auto& list = actions();
    const Action* globalMatch = nullptr;

    for (std::size_t i = 0; i < list.size(); ++i)
    {
        const auto& a = list[i];

        if (a.context != Context::global && a.context != focus)
            continue;

        if (std::find (chords[i].begin(), chords[i].end(), c) == chords[i].end())
            continue;

        if (typingPiano && plainPrintable && ! a.whilePiano)
            continue;

        if (a.context == focus && focus != Context::global)
            return a.id; // the focused panel wins

        if (globalMatch == nullptr)
            globalMatch = &a;
    }

    return globalMatch != nullptr ? globalMatch->id : std::string();
}

const std::vector<Chord>& KeyMap::chordsFor (const std::string& actionId) const
{
    static const std::vector<Chord> empty;
    const auto& list = actions();

    for (std::size_t i = 0; i < list.size(); ++i)
        if (actionId == list[i].id)
            return chords[i];

    return empty;
}

void KeyMap::setChords (const std::string& actionId, std::vector<Chord> newChords)
{
    const auto& list = actions();

    for (std::size_t i = 0; i < list.size(); ++i)
        if (actionId == list[i].id)
            chords[i] = std::move (newChords);
}

bool KeyMap::loadJson (const std::string& json, std::vector<std::string>* errors)
{
    Json p { json };
    auto fail = [&] (const std::string& message) {
        if (errors != nullptr)
            errors->push_back (message);
    };

    if (! p.eat ('{'))
        return false;

    if (p.eat ('}'))
        return true;

    for (;;)
    {
        const auto id = p.string();

        if (! id || ! p.eat (':'))
            return false;

        std::vector<std::string> values;

        if (p.eat ('['))
        {
            if (! p.eat (']'))
                for (;;)
                {
                    const auto v = p.string();
                    if (! v)
                        return false;
                    values.push_back (*v);
                    if (p.eat (']'))
                        break;
                    if (! p.eat (','))
                        return false;
                }
        }
        else if (const auto v = p.string())
        {
            if (! trim (*v).empty())
                values.push_back (*v);
        }
        else
            return false;

        if (findAction (*id) == nullptr)
            fail ("unknown action: " + *id);
        else
        {
            std::vector<Chord> parsed;

            for (const auto& v : values)
                if (const auto c = parseChord (v))
                    parsed.push_back (*c);
                else
                    fail ("unreadable key for " + *id + ": " + v);

            setChords (*id, parsed);
        }

        if (p.eat ('}'))
            return true;

        if (! p.eat (','))
            return false;
    }
}

std::string KeyMap::toJson() const
{
    std::string out = "{\n";
    const auto& list = actions();

    for (std::size_t i = 0; i < list.size(); ++i)
    {
        out += "  \"" + std::string (list[i].id) + "\": [";

        for (std::size_t k = 0; k < chords[i].size(); ++k)
            out += (k > 0 ? ", \"" : "\"") + toString (chords[i][k]) + "\"";

        out += i + 1 < list.size() ? "],\n" : "]\n";
    }

    return out + "}\n";
}

std::vector<std::pair<std::string, std::string>> KeyMap::conflicts() const
{
    std::vector<std::pair<std::string, std::string>> out;
    const auto& list = actions();

    for (std::size_t a = 0; a < list.size(); ++a)
        for (std::size_t b = a + 1; b < list.size(); ++b)
        {
            if (! contextsOverlap (list[a].context, list[b].context))
                continue;

            for (const auto& c : chords[a])
                if (std::find (chords[b].begin(), chords[b].end(), c) != chords[b].end())
                {
                    out.push_back ({ list[a].id, list[b].id });
                    break;
                }
        }

    return out;
}
std::vector<std::string> KeyMap::actionsWith (const Chord& chord, Context context, const std::string& except) const
{
    std::vector<std::string> out;
    const auto& list = actions();

    for (std::size_t i = 0; i < list.size(); ++i)
        if (list[i].context == context && except != list[i].id
            && std::find (chords[i].begin(), chords[i].end(), chord) != chords[i].end())
            out.push_back (list[i].id);

    return out;
}

bool KeyMap::isDefault (const std::string& actionId) const
{
    const auto& list = actions();

    for (std::size_t i = 0; i < list.size(); ++i)
        if (actionId == list[i].id)
            return chords[i] == list[i].defaults;

    return true;
}

std::string KeyMap::overridesJson() const
{
    std::string out = "{\n";
    const auto& list = actions();
    bool first = true;

    for (std::size_t i = 0; i < list.size(); ++i)
    {
        if (chords[i] == list[i].defaults)
            continue;

        out += first ? "" : ",\n";
        first = false;
        out += "  \"" + std::string (list[i].id) + "\": [";

        for (std::size_t k = 0; k < chords[i].size(); ++k)
            out += (k > 0 ? ", \"" : "\"") + toString (chords[i][k]) + "\"";

        out += "]";
    }

    return out + (first ? "}\n" : "\n}\n");
}

} // namespace lynx::keys
