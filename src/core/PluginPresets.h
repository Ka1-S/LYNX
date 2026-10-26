#pragma once

// Presets of hosted (third-party) plugins (round 162d, issue #8): FL's wrapper has a presets menu with the plugin's own
// programs, ◀ ▶ and Save / Load preset. LYNX keeps:
//   - its own preset file for any plugin format (VST2, VST3, CLAP, LV2, sandboxed or not): the plugin's whole saved
//     state with the plugin's name and id, as a small text file (`toText` / `fromText`, extension .lynxpreset like the
//     presets of LYNX's own engines, with another first line), in <settings>/Presets/Plugins/<plugin name>/;
//   - VST2's standard .fxp (one program) / .fxb (bank) files, read and written here (`parseFx` / `writeFx*`, the
//     publicly documented big-endian layout: "CcnK", then FxCk / FPCh / FxBk / FBCh);
//   - VST3's .vstpreset through the VST3 host itself (ui/PluginWindow.cpp).
// Pure C++ (no JUCE) so the formats are tested in the core tests.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lynx::pluginpresets
{
struct Preset
{
    std::string name;          // the preset's name
    std::string pluginName;    // "Surge XT"
    std::string pluginId;      // PluginRef::id of the plugin it was saved from
    std::string format;        // "VST3", "CLAP"… (informative)
    std::vector<std::uint8_t> state; // what the plugin returned when it was saved (getStateInformation)
};

constexpr const char* extension = ".lynxpreset";
constexpr const char* header = "LYNX plugin preset 1";

std::string toText (const Preset& preset);
// nullopt when the text is not a plugin preset (e.g. a preset of one of LYNX's own engines) or has no state.
std::optional<Preset> fromText (const std::string& text);

// The folder name for a plugin's presets (a plugin name made safe: no path separators or reserved characters,
// trimmed, at most 64 characters; "Plugin" when nothing is left).
std::string folderNameFor (const std::string& pluginName);

// --- VST2 .fxp / .fxb -----------------------------------------------------------------------------------------------
struct FxProgram
{
    std::string name;          // up to 27 characters
    std::vector<float> params; // normalised 0..1
};

struct FxFile
{
    enum class Kind
    {
        paramProgram, // FxCk: one program as parameter values
        chunkProgram, // FPCh: one program as the plugin's opaque chunk
        paramBank,    // FxBk: every program as parameter values
        chunkBank     // FBCh: the whole bank as the plugin's opaque chunk
    };
    Kind kind = Kind::paramProgram;
    std::int32_t pluginId = 0;      // the plugin's 4-character VST2 unique id
    std::int32_t pluginVersion = 0;
    std::int32_t currentProgram = 0; // banks (version 2): the program that was selected
    std::int32_t bankPrograms = 0;   // chunk banks: how many programs the plugin said the bank holds
    std::string programName;        // chunk programs
    std::vector<FxProgram> programs; // param programs (1) and param banks (all)
    std::vector<std::uint8_t> chunk; // chunk programs and banks

    bool isBank() const noexcept { return kind == Kind::paramBank || kind == Kind::chunkBank; }
    bool isChunk() const noexcept { return kind == Kind::chunkProgram || kind == Kind::chunkBank; }
};

std::optional<FxFile> parseFx (const std::vector<std::uint8_t>& bytes);
std::vector<std::uint8_t> writeFx (const FxFile& file);

// A 4-character id ('Syn1') as an int and back (VST2 unique ids are often printable).
std::int32_t fourCC (const char* text) noexcept;
} // namespace lynx::pluginpresets
