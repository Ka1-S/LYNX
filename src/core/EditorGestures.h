#pragma once

// The mouse grammar of the Piano Roll and the Playlist, without JUCE (unit-tested in
// tests/EditorGestureTests.inc). The editors ask these functions what a press would do at the
// point under the mouse, so the cursor, the hover highlight and the hint bar always tell the
// same story as the click itself. Behaviour interpreted from the FL Studio manual's Piano Roll
// and Playlist pages (docs/WORKFLOW_FL.md R1-R25, P1-P17), clean-room.

#include <algorithm>
#include <cmath>

namespace lynx
{
// The editors' tools (the strip in their toolbar).
enum class EditTool
{
    pencil, // add, move and resize (the default gestures)
    brush,  // drag lays a run of notes / clips (Shift: one note at a time in the Piano Roll)
    drumPaint, // Piano Roll only: drag lays one short note per grid cell, following the mouse across keys
    eraser, // click or drag deletes
    mute,   // click or drag mutes / unmutes
    knife,  // drag a line to cut through notes / clips; a click cuts at the cursor
    lasso,  // click selects, drag draws a selection box (Shift adds)
    zoom,   // drag a range to zoom into it; a click zooms back out
    listen, // click or drag to hear notes (Piano Roll) or play from there (Playlist)
    slip    // Playlist only: drag inside a clip to move its content under it
};

namespace gesture
{
// --- hit testing -------------------------------------------------------------------------------

// Where on an item (note or clip) spanning [x0, x1) pixels the mouse is.
enum class Zone
{
    outside,
    body,
    leftEdge,
    rightEdge
};

struct EdgeRules
{
    double grab = 6.0;             // widest edge zone, in pixels
    double minimumHit = 4.0;       // very short items are still this wide to the mouse
    double minimumForLeft = 16.0;  // narrower items only offer the right edge (the body must stay grabbable)
    bool allowLeft = true;         // the left edge resizes / trims
};

// The edge zone shrinks with the item (a quarter of its width, at most `grab`), so a short note is
// still mostly "body" and can be moved; the right edge always wins over the left one.
inline Zone zoneAt (double x, double x0, double x1, const EdgeRules& rules = {})
{
    const auto right = std::max (x1, x0 + rules.minimumHit);

    if (x < x0 || x >= right)
        return Zone::outside;

    const auto width = right - x0;
    const auto edge = std::clamp (width / 4.0, 2.0, rules.grab);

    if (x >= right - edge)
        return Zone::rightEdge;
    if (rules.allowLeft && width >= rules.minimumForLeft && x < x0 + edge)
        return Zone::leftEdge;
    return Zone::body;
}

// --- what a left press does ----------------------------------------------------------------------

struct Mods
{
    bool shift = false, ctrl = false, alt = false;
};

// What is under the mouse in an editor's grid.
enum class Target
{
    empty,       // background
    item,        // body of a note / clip
    leftEdge,    // left edge of a note / clip
    rightEdge,   // right edge of a note / clip
    fadeHandle,  // Playlist: fade handle of an audio clip
    autoPoint,   // Playlist: a point of an inline automation curve
    autoCurve,   // Playlist: on an automation curve (a click adds a point)
    autoTension, // Playlist: the tension handle between two automation points (drag up / down bends)
    clipMenu     // Playlist: the menu button in a clip's title strip
};

inline Target targetFor (Zone zone)
{
    switch (zone)
    {
        case Zone::body: return Target::item;
        case Zone::leftEdge: return Target::leftEdge;
        case Zone::rightEdge: return Target::rightEdge;
        case Zone::outside: break;
    }
    return Target::empty;
}

enum class Action
{
    none,
    create,       // draws a new note / places a clip and drags it
    paint,        // brush: lays a run while dragging
    move,         // drags the item (and the selection it belongs to)
    copyMove,     // Shift: drags a copy
    resizeRight,  // drags the end
    resizeLeft,   // drags the start (Playlist: trims, keeping the content in place)
    slipContent,  // moves a clip's content under it
    toggleSelect, // Ctrl+click: adds / removes from the selection
    boxSelect,    // draws a selection box
    erase,
    toggleMute,
    slice,
    zoomRange,
    listen,
    fade,         // drags an audio fade
    movePoint,    // drags an automation point
    addPoint,     // adds an automation point on the curve
    bendSegment,  // drags an automation segment's tension handle
    openMenu      // opens the clip menu
};

// The action of a left press with `tool` on `target`. `playlist` selects the Playlist's extras
// (slip, fades, inline automation, clip menu). Mirrors the editors' mouseDown.
inline Action leftAction (EditTool tool, Target target, Mods mods, bool playlist)
{
    const auto onItem = target != Target::empty;

    if (target == Target::clipMenu)
        return Action::openMenu;

    // every tool: Ctrl+click toggles an item in the selection; Ctrl+drag on empty space selects
    if (mods.ctrl)
        return onItem ? Action::toggleSelect : Action::boxSelect;

    const auto painter = tool == EditTool::brush || tool == EditTool::drumPaint;

    if (tool == EditTool::lasso || (! onItem && mods.shift && ! painter))
        return Action::boxSelect;

    switch (tool)
    {
        case EditTool::eraser: return Action::erase;
        case EditTool::mute: return Action::toggleMute;
        case EditTool::knife: return Action::slice;
        case EditTool::zoom: return Action::zoomRange;
        case EditTool::listen: return Action::listen;
        case EditTool::slip: return playlist && onItem ? Action::slipContent : Action::none;
        case EditTool::pencil:
        case EditTool::brush:
        case EditTool::drumPaint:
        case EditTool::lasso: break;
    }

    if (playlist)
    {
        if (target == Target::autoPoint && ! mods.shift)
            return Action::movePoint;
        if (target == Target::autoTension && ! mods.shift)
            return Action::bendSegment;
        if (target == Target::autoCurve && ! mods.shift)
            return Action::addPoint;
        if (target == Target::fadeHandle)
            return Action::fade;
        if (mods.alt && mods.shift && target == Target::item)
            return Action::slipContent;
    }

    switch (target)
    {
        case Target::item:
        case Target::autoPoint:
        case Target::autoCurve:
        case Target::autoTension:
        case Target::fadeHandle: return mods.shift ? Action::copyMove : Action::move;
        case Target::rightEdge: return Action::resizeRight;
        case Target::leftEdge: return Action::resizeLeft;
        case Target::empty: return painter ? Action::paint : Action::create;
        case Target::clipMenu: break;
    }

    return Action::none;
}

// --- cursors ---------------------------------------------------------------------------------------

enum class Cursor
{
    normal,
    draw,      // pencil
    paint,     // brush
    move,      // four-way: moving an item
    copy,      // moving a copy
    resizeH,   // left-right: an edge
    erase,
    mute,
    slice,
    select,    // crosshair: box selection
    zoom,
    listen,
    slip,
    point,     // pointing hand: a handle or a button
    pan        // grabbing hand: middle-drag
};

inline Cursor cursorFor (Action action, EditTool tool)
{
    switch (action)
    {
        case Action::create: return Cursor::draw;
        case Action::paint: return Cursor::paint;
        case Action::move: return Cursor::move;
        case Action::copyMove: return Cursor::copy;
        case Action::resizeRight:
        case Action::resizeLeft:
        case Action::fade: return Cursor::resizeH;
        case Action::slipContent: return Cursor::slip;
        case Action::toggleSelect:
        case Action::boxSelect: return Cursor::select;
        case Action::erase: return Cursor::erase;
        case Action::toggleMute: return Cursor::mute;
        case Action::slice: return Cursor::slice;
        case Action::zoomRange: return Cursor::zoom;
        case Action::listen: return Cursor::listen;
        case Action::movePoint:
        case Action::addPoint:
        case Action::bendSegment:
        case Action::openMenu: return Cursor::point;
        case Action::none: break;
    }

    return tool == EditTool::slip ? Cursor::slip : Cursor::normal;
}

// --- view maths ------------------------------------------------------------------------------------

// New scroll position (in ticks) after changing the zoom from `oldPixelsPerTick` to `newPixelsPerTick`,
// keeping the tick under the mouse (`anchorPixels` from the left of the grid) where it is.
inline double zoomAnchored (double scrollTicks, double oldPixelsPerTick, double newPixelsPerTick, double anchorPixels)
{
    const auto anchorTick = scrollTicks + anchorPixels / oldPixelsPerTick;
    return std::max (0.0, anchorTick - anchorPixels / newPixelsPerTick);
}

// Zoom factor of one wheel event. Notched wheels step by 1.2 per notch whatever the driver's delta
// size; smooth (trackpad / high-resolution) wheels zoom in proportion to the delta.
inline double wheelZoomFactor (double deltaY, bool smooth)
{
    if (deltaY == 0.0)
        return 1.0;
    if (! smooth)
        return deltaY > 0.0 ? 1.2 : 1.0 / 1.2;
    return std::pow (1.2, std::clamp (deltaY, -1.0, 1.0) * 7.0);
}

// Scroll distance in pixels of one wheel event (notched: ~3 rows per notch; smooth: proportional).
inline double wheelScrollPixels (double delta, bool smooth, double pixelsPerNotch)
{
    if (delta == 0.0)
        return 0.0;
    if (! smooth)
        return (delta > 0.0 ? -1.0 : 1.0) * pixelsPerNotch;
    return -delta * pixelsPerNotch * 7.0;
}

// A value that glides towards its target (smooth wheel scrolling). `step` advances it by one frame.
struct Glide
{
    double current = 0.0, target = 0.0;

    void jumpTo (double v) noexcept { current = target = v; }
    bool isMoving() const noexcept { return current != target; }

    // 60 Hz frames: covers ~45% of the remaining distance per frame, snaps the last half pixel
    double step (double fraction = 0.45, double settle = 0.5) noexcept
    {
        current += (target - current) * fraction;
        if (std::abs (target - current) < settle)
            current = target;
        return current;
    }
};

// Value on the straight line from (x0, v0) to (x1, v1) at x (the event lane's right-drag line), 0..1.
inline double lineValue (double x0, double v0, double x1, double v1, double x)
{
    if (x1 == x0)
        return std::clamp (v1, 0.0, 1.0);
    const auto t = (x - x0) / (x1 - x0);
    return std::clamp (v0 + (v1 - v0) * t, 0.0, 1.0);
}

// FL's Piano roll keyboard: where a key is clicked sets the note's velocity, soft at the
// key's base and full at its tip (`along` pixels from the base of a key `length` pixels long), 1/127 at least.
inline float keyClickVelocity (double along, double length)
{
    if (length <= 0.0)
        return 0.8f;
    return (float) std::clamp ((along + 1.0) / length, 1.0 / 127.0, 1.0);
}

// Alt+wheel over a note: the property moves by 4/127 per notch (Ctrl+Alt: 1/127), clamped to 0..1.
inline double wheelStepValue (double value, double deltaY, bool fine)
{
    if (deltaY == 0.0)
        return value;
    const auto step = (fine ? 1.0 : 4.0) / 127.0;
    return std::clamp (value + (deltaY > 0.0 ? step : -step), 0.0, 1.0);
}

// Font height for the keyboard and note labels: follows the row height, readable at every zoom.
inline float labelHeight (int rowHeight)
{
    return std::clamp ((float) rowHeight * 0.72f, 9.0f, 13.0f);
}

// Whether a note label fits: the row must be tall enough to read and the note wide enough for "C#5".
inline bool noteLabelFits (double widthPixels, int rowHeight)
{
    return rowHeight >= 9 && widthPixels >= 3.0 * labelHeight (rowHeight);
}

// --- overview strip (round 90) ------------------------------------------------------------------
// The strip above the Piano Roll's and the Playlist's ruler shows the whole content in miniature with
// the visible window outlined (FL Studio's preview scroll bar, interpreted). Positions are in content
// units (ticks); `width` is the strip's width in pixels.
enum class OverviewGrab { none, move, left, right };

struct OverviewWindow
{
    double start = 0.0, length = 1.0;
};

// What a press at `x` grabs: an edge of the window (zooms), the window (scrolls) or nothing (jumps).
inline OverviewGrab overviewGrabAt (double x, double width, double total, OverviewWindow w, double edgePixels = 5.0)
{
    if (total <= 0.0 || width <= 0.0)
        return OverviewGrab::none;

    const auto x0 = w.start / total * width, x1 = (w.start + w.length) / total * width;
    // a narrow window keeps a middle zone to move it
    const auto edge = std::min (edgePixels, std::max (1.0, (x1 - x0) / 3.0));

    if (std::abs (x - x0) <= edge)
        return OverviewGrab::left;
    if (std::abs (x - x1) <= edge)
        return OverviewGrab::right;
    if (x > x0 && x < x1)
        return OverviewGrab::move;
    return OverviewGrab::none;
}

// The window after dragging `grab` by `dx` pixels from `from`; `minLength` keeps the zoom in range.
// A press outside the window (none) centres it on the press first, then drags like `move`.
inline OverviewWindow overviewDrag (OverviewGrab grab, OverviewWindow from, double dx, double width, double total,
                                    double minLength)
{
    if (width <= 0.0 || total <= 0.0)
        return from;

    const auto d = dx / width * total;
    auto w = from;

    switch (grab)
    {
        case OverviewGrab::none:
        case OverviewGrab::move:
            w.start = std::clamp (from.start + d, 0.0, std::max (0.0, total - from.length));
            break;
        case OverviewGrab::left:
        {
            const auto end = from.start + from.length;
            w.start = std::clamp (from.start + d, 0.0, std::max (0.0, end - minLength));
            w.length = end - w.start;
            break;
        }
        case OverviewGrab::right:
            w.length = std::clamp (from.length + d, minLength, std::max (minLength, total - from.start));
            break;
    }

    return w;
}

// The window centred on content position `at` (a click outside the window jumps there).
inline OverviewWindow overviewCentredOn (double at, OverviewWindow w, double total)
{
    w.start = std::clamp (at - w.length / 2.0, 0.0, std::max (0.0, total - w.length));
    return w;
}
} // namespace gesture
} // namespace lynx
