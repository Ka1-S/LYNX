#pragma once

#include <cstdint>
#include <string>
#include <vector>

// User themes (round 162d, issues #8 / #9): the accent colour, the background and the workspace backdrop are the
// user's choice. This is the pure part (no JUCE): it derives every colour token of ui/Theme.h from the two colours the
// user picks, so the whole UI keeps its relations (elevation steps, hover/pressed shades, tinted selections) and the
// text stays readable. With LYNX's default accent and background it gives exactly the built-in palette.
namespace lynx::themes
{
constexpr std::uint32_t defaultAccent = 0xffe5383b;     // LYNX red
constexpr std::uint32_t defaultBackground = 0xff15161a; // the app background (token bg1)

// Every themeable token of ui/Theme.h (ARGB).
struct Palette
{
    std::uint32_t bg0, bg1, bg2, bg3, bg4, bg5, bg6;
    std::uint32_t border, borderStrong;
    std::uint32_t textPrimary, textSecondary, textDisabled;
    std::uint32_t accent, accentHover, accentPressed, accentMuted, onAccent, selectionFill;
    std::uint32_t knobArc, knobTrack, stepOff, stepOffAlt, stepNote;
    std::uint32_t gridBar, gridBeat, gridStep, keyBlack, rowWhite, rowBlack;
    std::uint32_t noteSoft, noteHard, noteMuted, ledOff;

    bool operator== (const Palette&) const = default;
};

// The palette for an accent and a background (alpha is ignored: both are opaque).
Palette derivePalette (std::uint32_t accent, std::uint32_t background);
// LYNX's built-in palette (= derivePalette (defaultAccent, defaultBackground)).
const Palette& defaultPalette();

// How the workspace image is laid out.
enum class ImageFit
{
    fill,    // covers the whole area, cropped (keeps the proportions)
    fit,     // whole image visible, centred (keeps the proportions)
    stretch, // exactly the area (distorts)
    tile,    // repeated at its own size from the top left
    centre   // its own size, centred
};
constexpr int imageFitCount = 5;
const char* imageFitId (ImageFit fit); // "fill", "fit", "stretch", "tile", "centre"
ImageFit imageFitFromId (const std::string& id);

// A theme: what a .lynxtheme file holds and what Options › Theme edits.
struct Spec
{
    std::string name = "LYNX";
    std::uint32_t accent = defaultAccent;
    std::uint32_t background = defaultBackground;
    std::uint32_t workspace = 0;     // the workspace backdrop's colour; 0 = automatic (the theme's deepest surface)
    std::string image;               // workspace image file (empty = none)
    ImageFit fit = ImageFit::fill;
    float imageOpacity = 1.0f;       // 0..1
    float dim = 0.0f;                // 0..1: darkens the image towards the backdrop colour

    bool isDefaultLook() const noexcept { return accent == defaultAccent && background == defaultBackground && workspace == 0 && image.empty(); }
    bool operator== (const Spec&) const = default;
};

// The built-in themes ("LYNX" first = the default look).
const std::vector<Spec>& builtInThemes();

// The workspace backdrop colour of a theme (its own, or the palette's bg0).
std::uint32_t workspaceColour (const Spec& spec);

// --- colour maths ------------------------------------------------------------------------------------------------
double relativeLuminance (std::uint32_t argb);              // WCAG 2 (0..1)
double contrastRatio (std::uint32_t a, std::uint32_t b);    // WCAG 2 (1..21)
std::uint32_t readableTextOn (std::uint32_t background);    // white or near-black, whichever reads better
std::uint32_t mix (std::uint32_t a, std::uint32_t b, double amount); // a → b by amount (0..1), opaque
std::string toHex (std::uint32_t argb);                     // "#RRGGBB"
bool parseHex (const std::string& text, std::uint32_t& argb); // "#RGB", "RRGGBB", "#RRGGBB", "0xRRGGBB" (opaque)
} // namespace lynx::themes
