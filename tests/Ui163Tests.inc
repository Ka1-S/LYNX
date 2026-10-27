// Round 163 (user feedback, UI zoom 150%): windows kept reachable on their screen.
#include "core/WindowFit.h"

TEST (ui163_window_fit)
{
    using lynx::windowfit::Box;
    using lynx::windowfit::place;
    const Box screen { 0, 0, 1280, 700 }; // a 1920x1050 work area at 150%
    // inside: unchanged
    CHECK ((place ({ 100, 100, 600, 400 }, screen) == Box { 100, 100, 600, 400 }));
    // taller than the screen, centred (title bar above the top): the title bar comes back on the screen
    CHECK ((place ({ 100, -150, 600, 1000 }, screen) == Box { 100, 0, 600, 1000 }));
    // off the right edge: moved back left
    CHECK ((place ({ 1000, 50, 600, 300 }, screen) == Box { 680, 50, 600, 300 }));
    // wider than the screen: the right end (close button) stays on it
    CHECK ((place ({ 0, 0, 1600, 300 }, screen) == Box { -320, 0, 1600, 300 }));
    // below the bottom: moved up; above the top: down
    CHECK ((place ({ 10, 600, 300, 200 }, screen) == Box { 10, 500, 300, 200 }));
    CHECK ((place ({ 10, -40, 300, 200 }, Box { 0, 30, 1280, 670 }) == Box { 10, 30, 300, 200 }));
    // a second monitor at negative x
    CHECK ((place ({ -2000, 10, 400, 300 }, Box { -1920, 0, 1920, 1040 }) == Box { -1920, 10, 400, 300 }));
}
