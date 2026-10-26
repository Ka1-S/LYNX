#include "core/ThemePalette.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace lynx::themes
{
namespace
{
int red (std::uint32_t c) { return (int) ((c >> 16) & 0xff); }
int green (std::uint32_t c) { return (int) ((c >> 8) & 0xff); }
int blue (std::uint32_t c) { return (int) (c & 0xff); }

std::uint32_t rgb (int r, int g, int b)
{
    auto clamp = [] (int v) { return (std::uint32_t) std::clamp (v, 0, 255); };
    return 0xff000000u | (clamp (r) << 16) | (clamp (g) << 8) | clamp (b);
}

// The built-in palette (the values ui/Theme.h shipped with before themes existed).
constexpr Palette builtIn {
    0xff0e0f12, 0xff15161a, 0xff1b1d22, 0xff23262c, 0xff2c2f37, 0xff363a43, 0xff434852, // bg0..bg6
    0xff2a2d34, 0xff3b3f48,                                                             // border, borderStrong
    0xffeceef2, 0xffa1a6b0, 0xff60656f,                                                 // text
    0xffe5383b, 0xfff2545a, 0xffc12a30, 0xff5c1f24, 0xffffffff, 0x2ee5383b,             // accent family
    0xffc5c9d1, 0xff444955, 0xff2a2d34, 0xff33363f, 0xffe8a3a6,                         // knobs, steps
    0xff3a3e47, 0xff262930, 0xff1d1f24, 0xff17181c, 0xff1c1e23, 0xff16171b,             // grids, keys, rows
    0xffb3545a, 0xffff6b6e, 0xff4a4e57, 0xff3a3e47                                      // notes, LED
};

// A surface token for another background: the built-in step from bg1 to that token, applied to the new background
// (mirrored for a light background, whose raised surfaces get darker instead of lighter).
std::uint32_t surface (std::uint32_t token, std::uint32_t background, bool light)
{
    const auto base = builtIn.bg1;
    const auto sign = light ? -1 : 1;
    return rgb (red (background) + sign * (red (token) - red (base)), green (background) + sign * (green (token) - green (base)),
                blue (background) + sign * (blue (token) - blue (base)));
}
} // namespace

std::uint32_t mix (std::uint32_t a, std::uint32_t b, double amount)
{
    const auto t = std::clamp (amount, 0.0, 1.0);
    auto channel = [t] (int x, int y) { return (int) std::lround (x + (y - x) * t); };
    return rgb (channel (red (a), red (b)), channel (green (a), green (b)), channel (blue (a), blue (b)));
}

double relativeLuminance (std::uint32_t argb)
{
    auto linear = [] (int v) {
        const auto c = v / 255.0;
        return c <= 0.03928 ? c / 12.92 : std::pow ((c + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear (red (argb)) + 0.7152 * linear (green (argb)) + 0.0722 * linear (blue (argb));
}

double contrastRatio (std::uint32_t a, std::uint32_t b)
{
    const auto la = relativeLuminance (a), lb = relativeLuminance (b);
    return (std::max (la, lb) + 0.05) / (std::min (la, lb) + 0.05);
}

std::uint32_t readableTextOn (std::uint32_t background)
{
    constexpr std::uint32_t white = 0xffffffff, dark = 0xff111214;
    // white wins ties: LYNX's accent keeps its white text unless the colour is clearly light
    return contrastRatio (white, background) * 1.7 >= contrastRatio (dark, background) ? white : dark;
}

Palette derivePalette (std::uint32_t accentIn, std::uint32_t backgroundIn)
{
    const auto accent = accentIn | 0xff000000u;
    const auto background = backgroundIn | 0xff000000u;
    if (accent == builtIn.accent && background == builtIn.bg1)
        return builtIn;

    Palette p = builtIn;
    const auto light = relativeLuminance (background) > 0.4;

    // surfaces, hairlines, grids: the built-in steps from the background
    for (auto* token : { &p.bg0, &p.bg1, &p.bg2, &p.bg3, &p.bg4, &p.bg5, &p.bg6, &p.border, &p.borderStrong, &p.knobTrack, &p.stepOff,
                         &p.stepOffAlt, &p.gridBar, &p.gridBeat, &p.gridStep, &p.keyBlack, &p.rowWhite, &p.rowBlack, &p.noteMuted, &p.ledOff })
        *token = surface (*token, background, light);

    if (light)
    {
        p.textPrimary = 0xff16181c;
        p.textSecondary = 0xff4c515b;
        p.textDisabled = 0xff8d929b;
        p.knobArc = 0xff4c515b;
    }
    // a mid-grey background where the dark theme's text would not read: the readable one
    if (contrastRatio (p.textPrimary, p.bg2) < 4.5)
        p.textPrimary = readableTextOn (p.bg2);

    // the accent family
    if (accent == builtIn.accent)
    {
        p.accentMuted = mix (background, accent, 0.34);
    }
    else
    {
        p.accent = accent;
        p.accentHover = mix (accent, 0xffffffff, 0.14);
        p.accentPressed = mix (accent, 0xff000000, 0.18);
        p.accentMuted = mix (background, accent, 0.34);
        p.onAccent = readableTextOn (accent);
        p.selectionFill = (accent & 0x00ffffffu) | 0x2e000000u;
        p.stepNote = mix (accent, light ? 0xff000000 : 0xffffffff, 0.55);
        p.noteHard = mix (accent, 0xffffffff, 0.26);
        p.noteSoft = mix (accent, 0xff808080, 0.42);
    }
    return p;
}

const Palette& defaultPalette()
{
    return builtIn;
}

const char* imageFitId (ImageFit fit)
{
    switch (fit)
    {
        case ImageFit::fill: return "fill";
        case ImageFit::fit: return "fit";
        case ImageFit::stretch: return "stretch";
        case ImageFit::tile: return "tile";
        case ImageFit::centre: return "centre";
    }
    return "fill";
}

ImageFit imageFitFromId (const std::string& id)
{
    for (int i = 0; i < imageFitCount; ++i)
        if (id == imageFitId ((ImageFit) i))
            return (ImageFit) i;
    if (id == "center")
        return ImageFit::centre;
    return ImageFit::fill;
}

const std::vector<Spec>& builtInThemes()
{
    static const std::vector<Spec> all = [] {
        auto make = [] (const char* name, std::uint32_t accent, std::uint32_t background) {
            Spec s;
            s.name = name;
            s.accent = accent;
            s.background = background;
            return s;
        };
        return std::vector<Spec> {
            make ("LYNX", defaultAccent, defaultBackground), make ("Ocean", 0xff3d8bfd, 0xff12161d), make ("Emerald", 0xff2fbf71, 0xff131715),
            make ("Amber", 0xfff0a020, 0xff17150f),          make ("Violet", 0xff9d6bff, 0xff16141c), make ("Graphite", 0xffd5d8de, 0xff141414),
        };
    }();
    return all;
}

std::uint32_t workspaceColour (const Spec& spec)
{
    return spec.workspace != 0 ? (spec.workspace | 0xff000000u) : derivePalette (spec.accent, spec.background).bg0;
}

std::string toHex (std::uint32_t argb)
{
    static const char* digits = "0123456789ABCDEF";
    std::string s = "#";
    for (int shift = 20; shift >= 0; shift -= 4)
        s += digits[(argb >> shift) & 0xf];
    return s;
}

bool parseHex (const std::string& textIn, std::uint32_t& argb)
{
    std::string text;
    for (auto c : textIn)
        if (! std::isspace ((unsigned char) c))
            text += c;
    if (text.rfind ("0x", 0) == 0 || text.rfind ("0X", 0) == 0)
        text = text.substr (2);
    else if (! text.empty() && text[0] == '#')
        text = text.substr (1);
    if (text.size() == 3)
        text = std::string { text[0], text[0], text[1], text[1], text[2], text[2] };
    if (text.size() == 8) // AARRGGBB: the alpha is dropped (themes are opaque)
        text = text.substr (2);
    if (text.size() != 6)
        return false;
    std::uint32_t value = 0;
    for (auto c : text)
    {
        value <<= 4;
        if (c >= '0' && c <= '9')
            value |= (std::uint32_t) (c - '0');
        else if (c >= 'a' && c <= 'f')
            value |= (std::uint32_t) (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            value |= (std::uint32_t) (c - 'A' + 10);
        else
            return false;
    }
    argb = 0xff000000u | value;
    return true;
}
} // namespace lynx::themes
