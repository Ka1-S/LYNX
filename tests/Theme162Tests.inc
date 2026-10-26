// --- round 162d (look162): user themes, the palette derived from an accent and a background (core/ThemePalette.h) ---

#include "core/ThemePalette.h"

TEST (theme162_default_palette_is_the_built_in_look)
{
    using namespace lynx::themes;
    const auto p = derivePalette (defaultAccent, defaultBackground);
    CHECK (p == defaultPalette());
    CHECK (p.accent == 0xffe5383b && p.accentHover == 0xfff2545a && p.accentPressed == 0xffc12a30 && p.accentMuted == 0xff5c1f24);
    CHECK (p.bg0 == 0xff0e0f12 && p.bg1 == 0xff15161a && p.bg6 == 0xff434852 && p.textPrimary == 0xffeceef2 && p.onAccent == 0xffffffff);
    CHECK (Spec {}.isDefaultLook());
    CHECK (workspaceColour (Spec {}) == 0xff0e0f12);
    CHECK (builtInThemes().size() >= 5 && builtInThemes()[0].accent == defaultAccent && builtInThemes()[0].background == defaultBackground);
}

TEST (theme162_derived_palettes_keep_relations_and_contrast)
{
    using namespace lynx::themes;
    // a navy background keeps the elevation steps of the built-in look
    const auto navy = derivePalette (0xff3d8bfd, 0xff12161d);
    CHECK (navy.bg1 == 0xff12161d);
    CHECK (navy.bg0 == 0xff0b0f15);                 // bg1 - (7, 7, 8)
    CHECK (navy.bg2 == 0xff181d25);                 // bg1 + (6, 7, 8)
    CHECK (navy.accent == 0xff3d8bfd && navy.selectionFill == 0x2e3d8bfd);
    CHECK (relativeLuminance (navy.accentHover) > relativeLuminance (navy.accent));
    CHECK (relativeLuminance (navy.accentPressed) < relativeLuminance (navy.accent));
    // every built-in theme: readable text on panels and on the accent
    for (const auto& t : builtInThemes())
    {
        const auto p = derivePalette (t.accent, t.background);
        CHECK (contrastRatio (p.textPrimary, p.bg2) >= 7.0);
        CHECK (contrastRatio (p.onAccent, p.accent) >= 3.0);
    }
    // a light accent gets dark text; a light background gets dark text and darker raised surfaces
    CHECK (derivePalette (0xfff5e663, defaultBackground).onAccent == 0xff111214);
    const auto light = derivePalette (defaultAccent, 0xffe8e8ec);
    CHECK (contrastRatio (light.textPrimary, light.bg2) >= 7.0);
    CHECK (relativeLuminance (light.bg3) < relativeLuminance (light.bg1));
    // mid grey: the text still reads
    const auto grey = derivePalette (defaultAccent, 0xff6a6a6a);
    CHECK (contrastRatio (grey.textPrimary, grey.bg2) >= 4.5);
}

TEST (theme162_hex_and_fit_ids)
{
    using namespace lynx::themes;
    std::uint32_t c = 0;
    CHECK (parseHex ("#E5383B", c) && c == 0xffe5383b);
    CHECK (parseHex ("e5383b", c) && c == 0xffe5383b);
    CHECK (parseHex (" 0x12161D ", c) && c == 0xff12161d);
    CHECK (parseHex ("#fa0", c) && c == 0xffffaa00);
    CHECK (parseHex ("80112233", c) && c == 0xff112233);
    CHECK (! parseHex ("#12345", c) && ! parseHex ("zzzzzz", c) && ! parseHex ("", c));
    CHECK (toHex (0xffe5383b) == "#E5383B");
    CHECK (mix (0xff000000, 0xffffffff, 0.5) == 0xff808080);
    for (int i = 0; i < imageFitCount; ++i)
        CHECK (imageFitFromId (imageFitId ((ImageFit) i)) == (ImageFit) i);
    CHECK (imageFitFromId ("center") == ImageFit::centre && imageFitFromId ("bogus") == ImageFit::fill);
    CHECK (readableTextOn (0xff000000) == 0xffffffff && readableTextOn (0xffffffff) == 0xff111214);
}
