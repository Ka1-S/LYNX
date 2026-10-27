// Editor mouse grammar (core/EditorGestures.h): hit zones, what a press does, cursors and view maths.
// Included from tests.cpp.

#include "core/EditorGestures.h"

TEST (gesture_zones_follow_item_width)
{
    using namespace lynx::gesture;
    // a wide note: 6 px edge zones, the body in between
    CHECK (zoneAt (99.0, 100.0, 200.0) == Zone::outside);
    CHECK (zoneAt (100.0, 100.0, 200.0) == Zone::leftEdge);
    CHECK (zoneAt (105.0, 100.0, 200.0) == Zone::leftEdge);
    CHECK (zoneAt (106.0, 100.0, 200.0) == Zone::body);
    CHECK (zoneAt (194.5, 100.0, 200.0) == Zone::rightEdge);
    CHECK (zoneAt (200.0, 100.0, 200.0) == Zone::outside);
    // left edge turned off: only the right edge resizes
    EdgeRules noLeft;
    noLeft.allowLeft = false;
    CHECK (zoneAt (101.0, 100.0, 200.0, noLeft) == Zone::body);
    // a 12 px note: edges shrink to a quarter (3 px) and the left one is not offered (< 16 px)
    CHECK (zoneAt (100.0, 100.0, 112.0) == Zone::body);
    CHECK (zoneAt (108.0, 100.0, 112.0) == Zone::body);
    CHECK (zoneAt (109.5, 100.0, 112.0) == Zone::rightEdge);
    // a tiny note is still 4 px wide to the mouse, mostly grabbable
    CHECK (zoneAt (101.0, 100.0, 100.5) == Zone::body);
    CHECK (zoneAt (103.5, 100.0, 100.5) == Zone::rightEdge);
    CHECK (zoneAt (104.0, 100.0, 100.5) == Zone::outside);
}

TEST (gesture_left_press_actions)
{
    using namespace lynx::gesture;
    using lynx::EditTool;
    const Mods none, shift { true, false, false }, ctrl { false, true, false }, alt { false, false, true }, altShift { true, false, true };

    // Pencil: draws on empty space, moves bodies, resizes edges, Shift copies, Alt is only "no snap"
    CHECK (leftAction (EditTool::pencil, Target::empty, none, false) == Action::create);
    CHECK (leftAction (EditTool::pencil, Target::empty, alt, false) == Action::create);
    CHECK (leftAction (EditTool::pencil, Target::item, none, false) == Action::move);
    CHECK (leftAction (EditTool::pencil, Target::item, shift, false) == Action::copyMove);
    CHECK (leftAction (EditTool::pencil, Target::rightEdge, shift, false) == Action::resizeRight);
    CHECK (leftAction (EditTool::pencil, Target::leftEdge, none, false) == Action::resizeLeft);
    // every tool: Ctrl selects
    for (auto tool : { EditTool::pencil, EditTool::brush, EditTool::eraser, EditTool::knife, EditTool::zoom })
    {
        CHECK (leftAction (tool, Target::item, ctrl, false) == Action::toggleSelect);
        CHECK (leftAction (tool, Target::empty, ctrl, true) == Action::boxSelect);
    }
    // Shift on empty space selects, except with the Brush (monophonic paint)
    CHECK (leftAction (EditTool::pencil, Target::empty, shift, false) == Action::boxSelect);
    CHECK (leftAction (EditTool::brush, Target::empty, shift, false) == Action::paint);
    // the other tools
    CHECK (leftAction (EditTool::eraser, Target::item, none, false) == Action::erase);
    CHECK (leftAction (EditTool::mute, Target::empty, none, false) == Action::toggleMute);
    CHECK (leftAction (EditTool::knife, Target::item, none, true) == Action::slice);
    CHECK (leftAction (EditTool::lasso, Target::item, none, false) == Action::boxSelect);
    CHECK (leftAction (EditTool::listen, Target::empty, none, true) == Action::listen);
    CHECK (leftAction (EditTool::slip, Target::item, none, true) == Action::slipContent);
    CHECK (leftAction (EditTool::slip, Target::empty, none, true) == Action::none);
    // Playlist extras
    CHECK (leftAction (EditTool::pencil, Target::item, altShift, true) == Action::slipContent);
    CHECK (leftAction (EditTool::pencil, Target::item, altShift, false) == Action::copyMove);
    CHECK (leftAction (EditTool::pencil, Target::fadeHandle, none, true) == Action::fade);
    CHECK (leftAction (EditTool::pencil, Target::autoPoint, none, true) == Action::movePoint);
    CHECK (leftAction (EditTool::brush, Target::autoCurve, none, true) == Action::addPoint);
    CHECK (leftAction (EditTool::pencil, Target::autoCurve, shift, true) == Action::copyMove);
    // round 165: FL's automation gestures in the Playlist (Alt + drag = no snap, the tension handle bends)
    CHECK (leftAction (EditTool::pencil, Target::autoPoint, Mods { false, false, true }, true) == Action::movePoint);
    CHECK (leftAction (EditTool::pencil, Target::autoTension, none, true) == Action::bendSegment);
    CHECK (leftAction (EditTool::eraser, Target::clipMenu, none, true) == Action::openMenu);
}

TEST (gesture_cursors)
{
    using namespace lynx::gesture;
    using lynx::EditTool;
    CHECK (cursorFor (Action::create, EditTool::pencil) == Cursor::draw);
    CHECK (cursorFor (Action::paint, EditTool::brush) == Cursor::paint);
    CHECK (cursorFor (Action::move, EditTool::pencil) == Cursor::move);
    CHECK (cursorFor (Action::copyMove, EditTool::pencil) == Cursor::copy);
    CHECK (cursorFor (Action::resizeLeft, EditTool::pencil) == Cursor::resizeH);
    CHECK (cursorFor (Action::resizeRight, EditTool::pencil) == Cursor::resizeH);
    CHECK (cursorFor (Action::erase, EditTool::eraser) == Cursor::erase);
    CHECK (cursorFor (Action::slice, EditTool::knife) == Cursor::slice);
    CHECK (cursorFor (Action::boxSelect, EditTool::lasso) == Cursor::select);
    CHECK (cursorFor (Action::none, EditTool::slip) == Cursor::slip);
    CHECK (cursorFor (Action::none, EditTool::pencil) == Cursor::normal);
    // the same press always shows the cursor of what it will do
    CHECK (cursorFor (leftAction (EditTool::pencil, targetFor (zoneAt (195.0, 100.0, 200.0)), {}, false), EditTool::pencil) == Cursor::resizeH);
    CHECK (cursorFor (leftAction (EditTool::pencil, targetFor (zoneAt (150.0, 100.0, 200.0)), {}, false), EditTool::pencil) == Cursor::move);
    CHECK (cursorFor (leftAction (EditTool::pencil, targetFor (zoneAt (250.0, 100.0, 200.0)), {}, false), EditTool::pencil) == Cursor::draw);
}

TEST (gesture_view_maths)
{
    using namespace lynx::gesture;
    // zooming keeps the tick under the mouse in place
    const auto scroll = 960.0, ppt = 0.25, anchor = 300.0;
    const auto tickUnderMouse = scroll + anchor / ppt;
    for (auto factor : { 1.2, 1.0 / 1.2, 3.0 })
    {
        const auto next = zoomAnchored (scroll, ppt, ppt * factor, anchor);
        CHECK (std::abs ((next + anchor / (ppt * factor)) - tickUnderMouse) < 1e-9);
    }
    CHECK (zoomAnchored (0.0, 1.0, 0.5, 100.0) == 0.0); // never scrolls before the start

    CHECK (wheelZoomFactor (0.14, false) == 1.2);
    CHECK (std::abs (wheelZoomFactor (-0.5, false) - 1.0 / 1.2) < 1e-12);
    CHECK (wheelZoomFactor (0.0, true) == 1.0);
    CHECK (wheelZoomFactor (0.02, true) > 1.0 && wheelZoomFactor (0.02, true) < 1.2); // a small trackpad move zooms a little
    CHECK (wheelScrollPixels (0.14, false, 42.0) == -42.0);
    CHECK (wheelScrollPixels (-0.6, false, 42.0) == 42.0);
    CHECK (wheelScrollPixels (0.01, true, 42.0) < 0.0 && wheelScrollPixels (0.01, true, 42.0) > -42.0);

    // the glide reaches its target in a few frames and stops exactly there
    Glide glide;
    glide.jumpTo (0.0);
    glide.target = 300.0;
    int frames = 0;
    while (glide.isMoving() && frames < 100)
    {
        const auto before = glide.current;
        glide.step();
        CHECK (glide.current > before && glide.current <= 300.0);
        ++frames;
    }
    CHECK (glide.current == 300.0 && frames > 3 && frames < 20);
}

TEST (gesture_lane_values)
{
    using namespace lynx::gesture;
    CHECK (lineValue (0.0, 0.2, 100.0, 0.8, 0.0) == 0.2);
    CHECK (std::abs (lineValue (0.0, 0.2, 100.0, 0.8, 50.0) - 0.5) < 1e-12);
    CHECK (lineValue (100.0, 0.8, 0.0, 0.2, 100.0) == 0.8); // drawn right to left
    CHECK (lineValue (0.0, 0.0, 10.0, 1.0, 50.0) == 1.0);   // clamped
    CHECK (lineValue (5.0, 0.3, 5.0, 0.6, 5.0) == 0.6);     // no width: the mouse's value
    CHECK (std::abs (wheelStepValue (0.5, 0.14, false) - (0.5 + 4.0 / 127.0)) < 1e-12);
    CHECK (std::abs (wheelStepValue (0.5, -1.0, true) - (0.5 - 1.0 / 127.0)) < 1e-12);
    CHECK (wheelStepValue (0.99, 1.0, false) == 1.0 && wheelStepValue (0.01, -1.0, false) == 0.0);
    // labels grow with the rows and only show where they fit
    CHECK (labelHeight (6) == 9.0f && labelHeight (14) > 9.0f && labelHeight (40) == 13.0f);
    CHECK (! noteLabelFits (200.0, 6));
    CHECK (noteLabelFits (40.0, 14) && ! noteLabelFits (20.0, 14));
}
