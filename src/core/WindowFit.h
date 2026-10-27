#pragma once

// Keeping a window reachable on its screen (round 163: UI zoom 150%+ made windows larger than the screen, with the
// title bar or the close button out of reach). Pure logic, no JUCE (unit-tested in tests/tests.cpp).

#include <algorithm>

namespace lynx::windowfit
{
struct Box
{
    int x = 0, y = 0, w = 0, h = 0;
    bool operator== (const Box&) const = default;
};

// Where a window whose outer rectangle (frame included) is `outer` goes inside the usable screen `area`: moved as
// little as possible so it fits; when it is wider than the screen its right end (the close button) stays on the
// screen; its title bar is never above the screen. The size is kept.
inline Box place (Box outer, Box area)
{
    auto x = outer.x, y = outer.y;
    if (x + outer.w > area.x + area.w)
        x = area.x + area.w - outer.w;
    if (x < area.x && outer.w <= area.w)
        x = area.x;
    if (y + outer.h > area.y + area.h)
        y = area.y + area.h - outer.h;
    y = std::max (y, area.y);
    return { x, y, outer.w, outer.h };
}
// Round 164: the size of a fixed-size window's content when it must fit the usable screen (`areaW` x `areaH`,
// minus the window frame): a content that is too tall (or too wide) scrolls in a viewport of this size, the scroll
// bar taking `scrollBar` px beside it. A content that fits keeps its size (`scrolls` false).
struct ScrollFit
{
    int w = 0, h = 0;
    bool scrollsV = false, scrollsH = false;
    bool scrolls() const { return scrollsV || scrollsH; }
};

inline ScrollFit scrollFit (int contentW, int contentH, int areaW, int areaH, int frameW, int frameH, int scrollBar)
{
    const auto maxW = std::max (100, areaW - frameW), maxH = std::max (100, areaH - frameH);
    ScrollFit fit;
    fit.scrollsV = contentH > maxH;
    // a vertical scroll bar needs room beside the content; without the room, the content scrolls sideways too
    const auto wantW = contentW + (fit.scrollsV ? scrollBar : 0);
    fit.scrollsH = wantW > maxW;
    fit.w = std::min (wantW, maxW);
    fit.h = std::min (contentH + (fit.scrollsH ? scrollBar : 0), maxH);
    if (fit.scrollsH && ! fit.scrollsV && contentH + scrollBar > maxH)
        fit.scrollsV = true;
    return fit;
}
} // namespace lynx::windowfit
