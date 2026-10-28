#include "ui/Icons.h"

#include <cmath>
#include <map>

#include "ui/Theme.h"

namespace lynx
{
namespace icons
{
namespace
{
constexpr float pi = juce::MathConstants<float>::pi;

void line (juce::Path& p, float x1, float y1, float x2, float y2)
{
    p.startNewSubPath (x1, y1);
    p.lineTo (x2, y2);
}

void roundRect (juce::Path& p, float x, float y, float w, float h, float r)
{
    p.addRoundedRectangle (x, y, w, h, r);
}

void polyline (juce::Path& p, std::initializer_list<juce::Point<float>> points, bool close = false)
{
    bool first = true;
    for (auto pt : points)
    {
        if (first)
            p.startNewSubPath (pt);
        else
            p.lineTo (pt);
        first = false;
    }
    if (close)
        p.closeSubPath();
}

Shape make (Icon icon)
{
    Shape s;
    auto& st = s.stroke;
    auto& fi = s.fill;

    // a page with a folded corner (file icons)
    auto page = [&st] {
        juce::Path p;
        polyline (p, { { 6.0f, 3.5f }, { 13.5f, 3.5f }, { 18.0f, 8.0f }, { 18.0f, 20.5f }, { 6.0f, 20.5f } }, true);
        st.addPath (p.createPathWithRoundedCorners (1.5f));
        polyline (st, { { 13.5f, 3.5f }, { 13.5f, 8.0f }, { 18.0f, 8.0f } });
    };

    // a speaker cone (mute and playback tools)
    auto speaker = [&st] {
        juce::Path p;
        polyline (p, { { 3.5f, 9.5f }, { 7.0f, 9.5f }, { 11.5f, 5.5f }, { 11.5f, 18.5f }, { 7.0f, 14.5f }, { 3.5f, 14.5f } }, true);
        st.addPath (p.createPathWithRoundedCorners (1.2f));
    };

    // a window outline with a title band (view icons)
    auto window = [&st] {
        roundRect (st, 3.5f, 4.5f, 17.0f, 15.0f, 2.0f);
    };

    switch (icon)
    {
        case Icon::none:
            break;

        // --- transport (solid) ---
        case Icon::play:
        {
            juce::Path tri;
            tri.addTriangle (7.0f, 4.5f, 7.0f, 19.5f, 19.5f, 12.0f);
            fi = tri.createPathWithRoundedCorners (2.0f);
            break;
        }
        case Icon::pause:
            fi.addRoundedRectangle (6.0f, 5.0f, 4.5f, 14.0f, 1.25f);
            fi.addRoundedRectangle (13.5f, 5.0f, 4.5f, 14.0f, 1.25f);
            break;
        case Icon::stop:
            fi.addRoundedRectangle (5.5f, 5.5f, 13.0f, 13.0f, 2.25f);
            break;
        case Icon::record:
            fi.addEllipse (5.0f, 5.0f, 14.0f, 14.0f);
            break;

        // --- small glyphs ---
        case Icon::chevronDown:
            polyline (st, { { 7.0f, 9.5f }, { 12.0f, 14.5f }, { 17.0f, 9.5f } });
            break;
        case Icon::more:
            for (float x : { 5.5f, 12.0f, 18.5f })
                fi.addEllipse (x - 1.75f, 10.25f, 3.5f, 3.5f);
            break;
        case Icon::plus:
            line (st, 12.0f, 5.0f, 12.0f, 19.0f);
            line (st, 5.0f, 12.0f, 19.0f, 12.0f);
            break;
        case Icon::check:
            polyline (st, { { 5.0f, 12.5f }, { 9.5f, 17.0f }, { 19.0f, 7.0f } });
            break;
        case Icon::close:
            line (st, 6.5f, 6.5f, 17.5f, 17.5f);
            line (st, 17.5f, 6.5f, 6.5f, 17.5f);
            break;
        case Icon::minimise:
            line (st, 6.0f, 12.0f, 18.0f, 12.0f);
            break;
        case Icon::maximise:
            roundRect (st, 5.5f, 5.5f, 13.0f, 13.0f, 2.0f);
            line (st, 5.5f, 9.0f, 18.5f, 9.0f);
            break;
        case Icon::restore: // two stacked windows
            roundRect (st, 4.5f, 8.5f, 11.0f, 11.0f, 1.75f);
            polyline (st, { { 8.5f, 4.5f }, { 17.5f, 4.5f }, { 19.5f, 6.5f }, { 19.5f, 15.5f } });
            break;
        case Icon::panelMenu: // three bars of decreasing length: "this window's options"
            line (st, 5.0f, 7.0f, 19.0f, 7.0f);
            line (st, 5.0f, 12.0f, 16.0f, 12.0f);
            line (st, 5.0f, 17.0f, 13.0f, 17.0f);
            break;

        // --- modes ---
        case Icon::pattern: // a 2x2 block of steps, one lit
            roundRect (st, 4.0f, 4.0f, 6.5f, 6.5f, 1.5f);
            roundRect (st, 4.0f, 13.5f, 6.5f, 6.5f, 1.5f);
            roundRect (st, 13.5f, 13.5f, 6.5f, 6.5f, 1.5f);
            fi.addRoundedRectangle (12.75f, 3.25f, 8.0f, 8.0f, 2.0f);
            break;
        case Icon::song: // clips staggered along three lanes
            fi.addRoundedRectangle (3.5f, 4.5f, 9.0f, 4.0f, 1.25f);
            fi.addRoundedRectangle (9.0f, 10.0f, 11.5f, 4.0f, 1.25f);
            fi.addRoundedRectangle (5.5f, 15.5f, 8.0f, 4.0f, 1.25f);
            break;
        case Icon::metronome:
        {
            juce::Path body;
            polyline (body, { { 9.0f, 3.5f }, { 15.0f, 3.5f }, { 19.0f, 20.5f }, { 5.0f, 20.5f } }, true);
            st.addPath (body.createPathWithRoundedCorners (2.0f));
            line (st, 12.0f, 16.5f, 17.5f, 6.0f);
            line (st, 7.0f, 16.5f, 17.0f, 16.5f);
            fi.addEllipse (14.0f, 8.0f, 3.5f, 3.5f);
            break;
        }

        // --- file ---
        case Icon::newFile:
            page();
            line (st, 12.0f, 11.0f, 12.0f, 17.0f);
            line (st, 9.0f, 14.0f, 15.0f, 14.0f);
            break;
        case Icon::file:
            page();
            break;
        case Icon::folder:
        {
            juce::Path f;
            polyline (f, { { 3.5f, 5.5f }, { 9.0f, 5.5f }, { 11.0f, 7.5f }, { 20.5f, 7.5f }, { 20.5f, 18.5f }, { 3.5f, 18.5f } }, true);
            st.addPath (f.createPathWithRoundedCorners (1.75f));
            break;
        }
        case Icon::open: // a folder with its front flap tipped open
        {
            juce::Path back;
            polyline (back, { { 4.0f, 18.5f }, { 3.5f, 5.5f }, { 9.0f, 5.5f }, { 11.0f, 7.5f }, { 18.0f, 7.5f }, { 18.0f, 10.5f } });
            st.addPath (back.createPathWithRoundedCorners (1.5f));
            juce::Path flap;
            polyline (flap, { { 4.0f, 18.5f }, { 7.0f, 10.5f }, { 21.0f, 10.5f }, { 18.0f, 18.5f } }, true);
            st.addPath (flap.createPathWithRoundedCorners (1.5f));
            break;
        }
        case Icon::save: // a disk
        {
            juce::Path body;
            polyline (body, { { 4.0f, 4.0f }, { 16.5f, 4.0f }, { 20.0f, 7.5f }, { 20.0f, 20.0f }, { 4.0f, 20.0f } }, true);
            st.addPath (body.createPathWithRoundedCorners (2.0f));
            polyline (st, { { 8.0f, 4.0f }, { 8.0f, 8.5f }, { 15.0f, 8.5f }, { 15.0f, 4.0f } });
            roundRect (st, 7.5f, 13.0f, 9.0f, 7.0f, 1.0f);
            break;
        }
        case Icon::exportAudio: // a waveform leaving a tray
            polyline (st, { { 3.5f, 14.0f }, { 3.5f, 19.5f }, { 20.5f, 19.5f }, { 20.5f, 14.0f } });
            line (st, 12.0f, 15.5f, 12.0f, 4.0f);
            polyline (st, { { 8.0f, 8.0f }, { 12.0f, 4.0f }, { 16.0f, 8.0f } });
            break;
        case Icon::settings: // three sliders
            line (st, 4.0f, 7.0f, 20.0f, 7.0f);
            line (st, 4.0f, 12.0f, 20.0f, 12.0f);
            line (st, 4.0f, 17.0f, 20.0f, 17.0f);
            fi.addEllipse (6.5f, 4.5f, 5.0f, 5.0f);
            fi.addEllipse (13.0f, 9.5f, 5.0f, 5.0f);
            fi.addEllipse (8.5f, 14.5f, 5.0f, 5.0f);
            break;
        case Icon::help:
            st.addEllipse (3.5f, 3.5f, 17.0f, 17.0f);
            st.startNewSubPath (9.5f, 10.0f);
            st.addCentredArc (12.0f, 10.0f, 2.5f, 2.5f, 0.0f, -pi * 0.5f, pi * 0.7f, false);
            st.lineTo (12.0f, 13.5f);
            fi.addEllipse (10.9f, 15.6f, 2.2f, 2.2f);
            break;
        case Icon::undo:
        case Icon::redo:
        {
            juce::Path p;
            p.startNewSubPath (19.0f, 18.5f);
            p.cubicTo (19.0f, 12.0f, 15.0f, 9.0f, 5.5f, 9.0f);
            polyline (p, { { 9.5f, 5.0f }, { 5.5f, 9.0f }, { 9.5f, 13.0f } });
            if (icon == Icon::redo)
                p.applyTransform (juce::AffineTransform::scale (-1.0f, 1.0f, 12.0f, 12.0f));
            st.addPath (p);
            break;
        }

        // --- views (the window buttons) ---
        case Icon::playlist: // clips on lanes inside a window
            window();
            fi.addRoundedRectangle (6.0f, 7.5f, 6.5f, 2.5f, 0.75f);
            fi.addRoundedRectangle (10.5f, 11.0f, 7.5f, 2.5f, 0.75f);
            fi.addRoundedRectangle (6.0f, 14.5f, 4.5f, 2.5f, 0.75f);
            fi.addRoundedRectangle (12.5f, 14.5f, 5.5f, 2.5f, 0.75f);
            break;
        case Icon::channelRack: // three channels: a name pad and a row of steps each
            for (int r = 0; r < 3; ++r)
            {
                const auto y = 4.5f + (float) r * 5.75f;
                roundRect (st, 3.5f, y, 6.5f, 3.5f, 1.0f);
                for (int c = 0; c < 3; ++c)
                {
                    const auto x = 12.5f + (float) c * 3.0f;
                    if ((r + c) % 2 == 0)
                        fi.addRoundedRectangle (x - 0.25f, y - 0.25f, 2.5f, 4.0f, 0.6f);
                    else
                        fi.addRoundedRectangle (x + 0.5f, y + 1.25f, 1.0f, 1.0f, 0.3f);
                }
            }
            break;
        case Icon::pianoRoll: // a keyboard edge and notes
            window();
            line (st, 8.0f, 4.5f, 8.0f, 19.5f);
            line (st, 3.5f, 9.5f, 6.0f, 9.5f);
            line (st, 3.5f, 14.5f, 6.0f, 14.5f);
            fi.addRoundedRectangle (10.0f, 7.0f, 4.5f, 2.5f, 0.75f);
            fi.addRoundedRectangle (13.0f, 10.75f, 5.5f, 2.5f, 0.75f);
            fi.addRoundedRectangle (10.5f, 14.5f, 4.0f, 2.5f, 0.75f);
            break;
        case Icon::mixer: // three faders
            for (int i = 0; i < 3; ++i)
                line (st, 6.0f + (float) i * 6.0f, 4.0f, 6.0f + (float) i * 6.0f, 20.0f);
            fi.addRoundedRectangle (3.5f, 12.0f, 5.0f, 3.5f, 1.0f);
            fi.addRoundedRectangle (9.5f, 6.5f, 5.0f, 3.5f, 1.0f);
            fi.addRoundedRectangle (15.5f, 14.5f, 5.0f, 3.5f, 1.0f);
            break;
        case Icon::browser: // a tree: a folder and two items under it
            roundRect (st, 3.5f, 4.0f, 6.0f, 5.0f, 1.25f);
            polyline (st, { { 6.5f, 9.0f }, { 6.5f, 17.5f }, { 10.0f, 17.5f } });
            line (st, 6.5f, 12.5f, 10.0f, 12.5f);
            line (st, 12.5f, 6.5f, 20.5f, 6.5f);
            line (st, 13.0f, 12.5f, 20.5f, 12.5f);
            line (st, 13.0f, 17.5f, 18.0f, 17.5f);
            break;
        case Icon::pluginPicker: // a plug
        {
            line (st, 9.0f, 3.5f, 9.0f, 7.5f);
            line (st, 15.0f, 3.5f, 15.0f, 7.5f);
            juce::Path body;
            polyline (body, { { 5.5f, 7.5f }, { 18.5f, 7.5f }, { 18.5f, 11.0f }, { 14.5f, 16.0f }, { 9.5f, 16.0f }, { 5.5f, 11.0f } }, true);
            st.addPath (body.createPathWithRoundedCorners (1.75f));
            line (st, 12.0f, 16.0f, 12.0f, 20.5f);
            break;
        }

        // --- content ---
        case Icon::audioFile: // a short waveform
        {
            const float heights[] = { 5.0f, 11.0f, 16.0f, 8.0f, 12.0f, 4.0f };
            for (int i = 0; i < 6; ++i)
            {
                const auto x = 4.5f + (float) i * 3.0f;
                line (st, x, 12.0f - heights[i] / 2.0f, x, 12.0f + heights[i] / 2.0f);
            }
            break;
        }
        case Icon::plugin: // a module with a jack
            roundRect (st, 4.0f, 6.5f, 16.0f, 12.0f, 2.0f);
            line (st, 8.5f, 3.5f, 8.5f, 6.5f);
            line (st, 15.5f, 3.5f, 15.5f, 6.5f);
            st.addEllipse (9.5f, 10.0f, 5.0f, 5.0f);
            break;
        case Icon::preset: // a knob
            st.addCentredArc (12.0f, 12.0f, 8.0f, 8.0f, 0.0f, -pi * 0.75f, pi * 0.75f, true);
            line (st, 12.0f, 12.0f, 15.5f, 8.5f);
            break;
        case Icon::project: // a diamond with a core
        {
            juce::Path d;
            polyline (d, { { 12.0f, 3.0f }, { 20.0f, 12.0f }, { 12.0f, 21.0f }, { 4.0f, 12.0f } }, true);
            st.addPath (d.createPathWithRoundedCorners (2.0f));
            fi.addEllipse (9.75f, 9.75f, 4.5f, 4.5f);
            break;
        }

        // --- edit tools ---
        case Icon::draw: // pencil
        {
            juce::Path p;
            polyline (p, { { 15.5f, 4.0f }, { 20.0f, 8.5f }, { 9.0f, 19.5f }, { 4.0f, 20.0f }, { 4.5f, 15.0f } }, true);
            st.addPath (p.createPathWithRoundedCorners (1.25f));
            line (st, 13.0f, 6.5f, 17.5f, 11.0f);
            break;
        }
        case Icon::brush: // a round brush: handle, ferrule and a filled tip
        {
            line (st, 20.0f, 4.0f, 13.5f, 10.5f);
            juce::Path ferrule;
            polyline (ferrule, { { 11.5f, 9.5f }, { 14.5f, 12.5f }, { 12.5f, 14.5f }, { 9.5f, 11.5f } }, true);
            st.addPath (ferrule.createPathWithRoundedCorners (0.75f));
            juce::Path tip;
            tip.startNewSubPath (9.0f, 12.5f);
            tip.quadraticTo (4.0f, 13.0f, 3.5f, 20.5f);
            tip.quadraticTo (11.0f, 20.0f, 11.5f, 15.0f);
            tip.closeSubPath();
            fi.addPath (tip);
            break;
        }
        case Icon::erase: // an eraser on a line
        {
            juce::Path p;
            polyline (p, { { 13.0f, 4.0f }, { 20.0f, 11.0f }, { 12.0f, 19.0f }, { 7.5f, 19.0f }, { 4.0f, 15.5f } }, true);
            st.addPath (p.createPathWithRoundedCorners (1.25f));
            line (st, 8.5f, 8.5f, 15.5f, 15.5f);
            line (st, 12.0f, 19.0f, 20.5f, 19.0f);
            break;
        }
        case Icon::mute: // a speaker with a cross
            speaker();
            line (st, 15.0f, 9.0f, 20.5f, 15.0f);
            line (st, 20.5f, 9.0f, 15.0f, 15.0f);
            break;
        case Icon::listen: // a speaker with two waves (playback tool)
            speaker();
            st.addCentredArc (12.0f, 12.0f, 3.5f, 3.5f, 0.0f, pi * 0.25f, pi * 0.75f, true);
            st.addCentredArc (12.0f, 12.0f, 7.5f, 7.5f, 0.0f, pi * 0.22f, pi * 0.78f, true);
            break;
        case Icon::knife: // a blade with a handle, cutting down to the left
        {
            juce::Path blade;
            polyline (blade, { { 4.0f, 20.0f }, { 13.5f, 8.0f }, { 17.0f, 11.5f } }, true);
            st.addPath (blade.createPathWithRoundedCorners (1.25f));
            line (st, 15.5f, 9.5f, 20.0f, 5.0f);
            break;
        }
        case Icon::select: // the four corners of a selection box
        {
            const float a = 4.0f, b = 20.0f, l = 4.5f;
            polyline (st, { { a, a + l }, { a, a }, { a + l, a } });
            polyline (st, { { b - l, a }, { b, a }, { b, a + l } });
            polyline (st, { { b, b - l }, { b, b }, { b - l, b } });
            polyline (st, { { a + l, b }, { a, b }, { a, b - l } });
            fi.addRoundedRectangle (9.5f, 9.5f, 5.0f, 5.0f, 1.0f);
            break;
        }
        case Icon::slice:
        case Icon::scissors:
            st.addEllipse (3.5f, 14.0f, 6.0f, 6.0f);
            st.addEllipse (14.5f, 14.0f, 6.0f, 6.0f);
            line (st, 8.5f, 15.0f, 17.5f, 3.5f);
            line (st, 15.5f, 15.0f, 6.5f, 3.5f);
            break;
        case Icon::zoom: // a magnifier with a plus
            st.addEllipse (3.5f, 3.5f, 13.0f, 13.0f);
            line (st, 14.5f, 14.5f, 20.5f, 20.5f);
            line (st, 10.0f, 7.0f, 10.0f, 13.0f);
            line (st, 7.0f, 10.0f, 13.0f, 10.0f);
            break;
        case Icon::slip: // a fixed frame with its content sliding inside
            roundRect (st, 3.5f, 5.5f, 17.0f, 13.0f, 2.0f);
            polyline (st, { { 9.0f, 9.5f }, { 6.5f, 12.0f }, { 9.0f, 14.5f } });
            polyline (st, { { 15.0f, 9.5f }, { 17.5f, 12.0f }, { 15.0f, 14.5f } });
            line (st, 7.0f, 12.0f, 17.0f, 12.0f);
            break;
        case Icon::ghost: // two overlapping notes, the back one hollow
            roundRect (st, 3.5f, 5.5f, 12.0f, 5.5f, 1.5f);
            fi.addRoundedRectangle (8.5f, 13.0f, 12.0f, 5.5f, 1.5f);
            break;
        case Icon::link:
            roundRect (st, 3.0f, 8.5f, 11.0f, 7.0f, 3.5f);
            roundRect (st, 10.0f, 8.5f, 11.0f, 7.0f, 3.5f);
            break;
        case Icon::magnet: // snap
        {
            juce::Path m;
            m.startNewSubPath (5.5f, 4.0f);
            m.lineTo (5.5f, 12.0f);
            m.addCentredArc (12.0f, 12.0f, 6.5f, 6.5f, 0.0f, pi * 1.5f, pi * 0.5f, false);
            m.lineTo (18.5f, 4.0f);
            st.addPath (m);
            line (st, 3.5f, 8.0f, 7.5f, 8.0f);
            line (st, 16.5f, 8.0f, 20.5f, 8.0f);
            break;
        }

        // --- workspace and control contract ---
        case Icon::detach: // a window with an arrow leaving its corner
            polyline (st, { { 10.5f, 4.5f }, { 6.0f, 4.5f }, { 4.5f, 6.0f }, { 4.5f, 18.0f }, { 6.0f, 19.5f }, { 18.0f, 19.5f }, { 19.5f, 18.0f }, { 19.5f, 13.5f } });
            line (st, 12.0f, 12.0f, 19.5f, 4.5f);
            polyline (st, { { 14.0f, 4.5f }, { 19.5f, 4.5f }, { 19.5f, 10.0f } });
            break;
        case Icon::dock: // an arrow going into a window's corner
            polyline (st, { { 10.5f, 4.5f }, { 6.0f, 4.5f }, { 4.5f, 6.0f }, { 4.5f, 18.0f }, { 6.0f, 19.5f }, { 18.0f, 19.5f }, { 19.5f, 18.0f }, { 19.5f, 13.5f } });
            line (st, 19.5f, 4.5f, 12.0f, 12.0f);
            polyline (st, { { 12.0f, 6.5f }, { 12.0f, 12.0f }, { 17.5f, 12.0f } });
            break;
        case Icon::keyboard: // a small piano: outline, white-key gaps, two black keys
            roundRect (st, 3.0f, 6.0f, 18.0f, 12.0f, 2.0f);
            line (st, 9.0f, 13.0f, 9.0f, 18.0f);
            line (st, 15.0f, 13.0f, 15.0f, 18.0f);
            fi.addRoundedRectangle (7.25f, 6.0f, 3.5f, 7.0f, 0.75f);
            fi.addRoundedRectangle (13.25f, 6.0f, 3.5f, 7.0f, 0.75f);
            break;
        case Icon::automate: // a curve through two points
        {
            juce::Path c;
            c.startNewSubPath (4.0f, 17.0f);
            c.cubicTo (9.0f, 17.0f, 10.0f, 7.0f, 15.0f, 7.0f);
            c.lineTo (20.5f, 7.0f);
            st.addPath (c);
            fi.addEllipse (2.0f, 15.0f, 4.0f, 4.0f);
            fi.addEllipse (13.0f, 5.0f, 4.0f, 4.0f);
            break;
        }
        case Icon::rightClick: // a mouse with the right button lit
        {
            roundRect (st, 6.5f, 3.5f, 11.0f, 17.0f, 5.5f);
            line (st, 6.5f, 10.5f, 17.5f, 10.5f);
            line (st, 12.0f, 3.5f, 12.0f, 10.5f);
            juce::Path b;
            b.startNewSubPath (12.0f, 3.5f);
            b.lineTo (12.0f, 10.5f);
            b.lineTo (17.5f, 10.5f);
            b.lineTo (17.5f, 9.0f);
            b.quadraticTo (17.5f, 3.5f, 12.0f, 3.5f);
            b.closeSubPath();
            fi.addPath (b);
            break;
        }
        case Icon::tap: // a dot with two rings
            fi.addEllipse (9.5f, 9.5f, 5.0f, 5.0f);
            st.addCentredArc (12.0f, 12.0f, 5.5f, 5.5f, 0.0f, -pi * 0.35f, pi * 0.35f, true);
            st.addCentredArc (12.0f, 12.0f, 8.5f, 8.5f, 0.0f, -pi * 0.3f, pi * 0.3f, true);
            break;
        case Icon::mic: // recording options: a microphone
            roundRect (st, 9.0f, 3.0f, 6.0f, 11.0f, 3.0f);
            st.startNewSubPath (18.5f, 11.0f);
            st.addCentredArc (12.0f, 11.0f, 6.5f, 6.0f, 0.0f, pi * 0.5f, pi * 1.5f, false);
            line (st, 12.0f, 17.0f, 12.0f, 20.5f);
            line (st, 8.5f, 20.5f, 15.5f, 20.5f);
            break;
        case Icon::countIn: // a stopwatch: dial, crown and a hand, with the first quarter filled
        {
            st.addEllipse (4.5f, 6.0f, 15.0f, 15.0f);
            line (st, 10.0f, 3.0f, 14.0f, 3.0f);
            line (st, 12.0f, 3.0f, 12.0f, 6.0f);
            line (st, 12.0f, 13.5f, 12.0f, 9.5f);
            juce::Path quarter;
            quarter.startNewSubPath (12.0f, 13.5f);
            quarter.addCentredArc (12.0f, 13.5f, 5.0f, 5.0f, 0.0f, 0.0f, pi * 0.5f, false);
            quarter.closeSubPath();
            fi.addPath (quarter);
            break;
        }
        case Icon::waitInput: // an hourglass: two triangles between two bars, sand in the bottom one
            line (st, 6.0f, 3.5f, 18.0f, 3.5f);
            line (st, 6.0f, 20.5f, 18.0f, 20.5f);
            polyline (st, { { 7.5f, 3.5f }, { 7.5f, 6.5f }, { 12.0f, 12.0f }, { 7.5f, 17.5f }, { 7.5f, 20.5f } });
            polyline (st, { { 16.5f, 3.5f }, { 16.5f, 6.5f }, { 12.0f, 12.0f }, { 16.5f, 17.5f }, { 16.5f, 20.5f } });
            polyline (fi, { { 12.0f, 16.0f }, { 15.0f, 19.5f }, { 9.0f, 19.5f } }, true);
            break;
        case Icon::mouseLeft: // a mouse with the left button lit (the mirror of rightClick)
        {
            roundRect (st, 6.5f, 3.5f, 11.0f, 17.0f, 5.5f);
            line (st, 6.5f, 10.5f, 17.5f, 10.5f);
            line (st, 12.0f, 3.5f, 12.0f, 10.5f);
            juce::Path b;
            b.startNewSubPath (12.0f, 3.5f);
            b.lineTo (12.0f, 10.5f);
            b.lineTo (6.5f, 10.5f);
            b.lineTo (6.5f, 9.0f);
            b.quadraticTo (6.5f, 3.5f, 12.0f, 3.5f);
            b.closeSubPath();
            fi.addPath (b);
            break;
        }
        case Icon::mouseMiddle: // a mouse with its wheel button lit
            roundRect (st, 6.5f, 3.5f, 11.0f, 17.0f, 5.5f);
            line (st, 6.5f, 11.0f, 17.5f, 11.0f);
            fi.addRoundedRectangle (10.25f, 4.5f, 3.5f, 6.0f, 1.75f);
            break;
        case Icon::mouseWheel: // a narrower mouse with its wheel, and up/down chevrons beside it
            roundRect (st, 3.5f, 4.5f, 10.0f, 15.0f, 5.0f);
            line (st, 3.5f, 11.0f, 13.5f, 11.0f);
            fi.addRoundedRectangle (7.25f, 5.5f, 3.5f, 5.0f, 1.75f);
            polyline (st, { { 16.5f, 8.0f }, { 18.5f, 5.5f }, { 20.5f, 8.0f } });
            polyline (st, { { 16.5f, 16.0f }, { 18.5f, 18.5f }, { 20.5f, 16.0f } });
            break;
        case Icon::drumBrush: // a brush laying a row of short hits on the grid
        {
            line (st, 20.5f, 3.5f, 15.0f, 9.0f);
            juce::Path tip;
            tip.startNewSubPath (14.0f, 8.0f);
            tip.lineTo (16.0f, 10.0f);
            tip.quadraticTo (14.5f, 14.0f, 10.0f, 13.5f);
            tip.quadraticTo (10.5f, 9.5f, 14.0f, 8.0f);
            tip.closeSubPath();
            fi.addPath (tip);
            fi.addRoundedRectangle (3.0f, 17.0f, 4.0f, 3.5f, 1.0f);
            fi.addRoundedRectangle (9.0f, 17.0f, 4.0f, 3.5f, 1.0f);
            fi.addRoundedRectangle (15.0f, 17.0f, 4.0f, 3.5f, 1.0f);
            break;
        }
        case Icon::graph: // bars of different heights standing on a base line (a step graph)
            line (st, 3.5f, 20.5f, 20.5f, 20.5f);
            fi.addRoundedRectangle (4.0f, 11.0f, 3.0f, 8.0f, 1.0f);
            fi.addRoundedRectangle (8.5f, 6.0f, 3.0f, 13.0f, 1.0f);
            fi.addRoundedRectangle (13.0f, 13.5f, 3.0f, 5.5f, 1.0f);
            fi.addRoundedRectangle (17.5f, 9.0f, 3.0f, 10.0f, 1.0f);
            break;
        case Icon::follow: // a playhead line with an arrow carrying it to the right
        {
            line (st, 7.5f, 4.5f, 7.5f, 19.5f);
            juce::Path head;
            head.addTriangle (4.5f, 3.5f, 10.5f, 3.5f, 7.5f, 7.0f);
            fi.addPath (head);
            line (st, 11.5f, 12.0f, 19.5f, 12.0f);
            line (st, 16.5f, 9.0f, 19.5f, 12.0f);
            line (st, 16.5f, 15.0f, 19.5f, 12.0f);
            break;
        }
        case Icon::loop: // two arcs chasing each other around (a loop), arrow heads at their ends
        {
            juce::Path a;
            a.addCentredArc (12.0f, 12.0f, 7.5f, 7.5f, 0.0f, 0.35f, 2.85f, true);
            st.addPath (a);
            juce::Path b;
            b.addCentredArc (12.0f, 12.0f, 7.5f, 7.5f, 0.0f, 3.5f, 6.0f, true);
            st.addPath (b);
            juce::Path h1, h2;
            h1.addTriangle (11.2f, 20.1f, 14.9f, 21.6f, 13.4f, 16.8f); // ahead of the first arc's end (bottom), clockwise
            h2.addTriangle (12.8f, 3.9f, 9.2f, 2.4f, 10.6f, 7.2f);     // ahead of the second arc's end (top)
            fi.addPath (h1);
            fi.addPath (h2);
            break;
        }
        case Icon::tuningFork: // two prongs on a stem, with a weighted foot
        {
            juce::Path f;
            f.startNewSubPath (8.5f, 3.5f);
            f.lineTo (8.5f, 10.0f);
            f.quadraticTo (8.5f, 14.0f, 12.0f, 14.0f);
            f.quadraticTo (15.5f, 14.0f, 15.5f, 10.0f);
            f.lineTo (15.5f, 3.5f);
            st.addPath (f);
            line (st, 12.0f, 14.0f, 12.0f, 18.0f);
            fi.addEllipse (10.0f, 17.0f, 4.0f, 4.0f);
            break;
        }
    }

    return s;
}
} // namespace

const Shape& get (Icon icon)
{
    static std::map<Icon, Shape> cache;
    auto it = cache.find (icon);
    if (it == cache.end())
        it = cache.emplace (icon, make (icon)).first;
    return it->second;
}

float strokeWidth (float size)
{
    // a fine line that thickens only a little with the size: 1.25 px at 14 px, 1.4 px at 20 px, 1.6 px from
    // 30 px (logical pixels; HiDPI screens draw it with proportionally more device pixels)
    return juce::jlimit (1.2f, 1.6f, 1.0f + size * 0.019f);
}

void draw (juce::Graphics& g, Icon icon, juce::Rectangle<float> area, juce::Colour colour)
{
    if (icon == Icon::none || area.isEmpty())
        return;

    // a whole-pixel box on whole-pixel coordinates: the grid's half units then land on the same
    // sub-pixel phase on every button, so strokes look equally sharp everywhere
    const auto size = std::floor (juce::jmin (area.getWidth(), area.getHeight()));
    const auto box = juce::Rectangle<float> (std::round (area.getCentreX() - size * 0.5f), std::round (area.getCentreY() - size * 0.5f), size, size);
    const auto scale = size / 24.0f;
    const auto transform = juce::AffineTransform::scale (scale).translated (box.getX(), box.getY());
    const auto& shape = get (icon);

    g.setColour (colour);

    if (! shape.fill.isEmpty())
        g.fillPath (shape.fill, transform);

    if (! shape.stroke.isEmpty())
        g.strokePath (shape.stroke, juce::PathStrokeType (strokeWidth (size), juce::PathStrokeType::curved, juce::PathStrokeType::rounded), transform);
}
} // namespace icons

IconButton::IconButton (const juce::String& name, Icon i, const juce::String& t)
    : juce::Button (name), icon (i), text (t), onColour (theme::colour::accent), iconOnColour (theme::colour::onAccent)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void IconButton::setIconSizing (float fraction, float maxPx)
{
    if (fraction != iconFraction || maxPx != iconMax)
    {
        iconFraction = fraction;
        iconMax = maxPx;
        repaint();
    }
}

void IconButton::setIcon (Icon newIcon)
{
    if (icon != newIcon)
    {
        icon = newIcon;
        repaint();
    }
}

void IconButton::setText (const juce::String& newText)
{
    if (text != newText)
    {
        text = newText;
        repaint();
    }
}

void IconButton::setPrimary (bool b) { primary = b; repaint(); }
void IconButton::setOnColour (juce::Colour c) { onColour = c; repaint(); }
void IconButton::setIconColourWhenOn (juce::Colour c) { iconOnColour = c; repaint(); }
void IconButton::setFlat (bool b) { flat = b; repaint(); }
void IconButton::setIconColour (juce::Colour c) { iconColour = c; repaint(); }

void IconButton::setCorners (bool left, bool right)
{
    roundLeft = left;
    roundRight = right;
    repaint();
}

void IconButton::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() && onRightClick != nullptr)
        return; // handled on mouse up, without pressing the button

    juce::Button::mouseDown (e);
}

void IconButton::mouseUp (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() && onRightClick != nullptr)
    {
        if (getLocalBounds().contains (e.getPosition()))
            onRightClick();
        return;
    }

    juce::Button::mouseUp (e);
}

void IconButton::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (onDoubleClick != nullptr && ! e.mods.isPopupMenu())
        onDoubleClick();
}

int IconButton::getIdealWidth (int height) const
{
    const auto iconSize = icon == Icon::none ? 0 : juce::roundToInt (height * 0.55f);
    const auto textWidth = text.isEmpty() ? 0
                                          : juce::roundToInt (juce::GlyphArrangement::getStringWidth (juce::Font (theme::font (theme::type::body)), text));
    const auto gap = iconSize > 0 && textWidth > 0 ? 6 : 0;
    return juce::jmax (height, iconSize + gap + textWidth + (textWidth > 0 ? 20 : 0));
}

void IconButton::buttonStateChanged()
{
    juce::Button::buttonStateChanged();

    const auto target = isOver() || isDown() ? 1.0f : 0.0f;

    if (target != hover && ! isTimerRunning())
        startTimerHz (60);
}

void IconButton::timerCallback()
{
    const auto target = isOver() || isDown() ? 1.0f : 0.0f;
    const auto step = target > hover ? 0.34f : 0.18f; // in quickly, out a little slower
    hover = target > hover ? juce::jmin (target, hover + step) : juce::jmax (target, hover - step);

    if (hover == target)
        stopTimer();

    repaint();
}

void IconButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    using namespace theme;
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    const auto on = getToggleState();
    const auto enabled = isEnabled();
    // the animated hover level (snaps to the state when nothing animates, e.g. in snapshots)
    const auto h = isTimerRunning() ? hover : (highlighted ? 1.0f : 0.0f);

    juce::Colour bg;
    if (on || primary)
        bg = down ? colour::accentPressed : onColour.interpolatedWith (onColour.brighter (0.12f), h);
    else if (down)
        bg = flat ? colour::textPrimary.withAlpha (0.13f) : colour::bg6;
    else // flat buttons get a light wash under the mouse instead of a solid surface
        bg = flat ? colour::textPrimary.withAlpha (0.075f * h) : colour::bg4.interpolatedWith (colour::bg5, h);

    if (! enabled)
        bg = bg.withMultipliedAlpha (0.5f);

    if (! bg.isTransparent())
    {
        juce::Path p;
        p.addRoundedRectangle (bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(), radius::md, radius::md,
                               roundLeft, roundRight, roundLeft, roundRight);
        g.setColour (bg);
        g.fillPath (p);
    }

    auto fg = (on || primary) ? iconOnColour
                              : (! iconColour.isTransparent() ? iconColour.interpolatedWith (iconColour.brighter (0.2f), h)
                                                              : colour::textSecondary.brighter (0.25f).interpolatedWith (colour::textPrimary, h));
    if (! enabled)
        fg = colour::textDisabled;

    auto area = getLocalBounds().toFloat();
    const auto iconSize = juce::jmin (area.getHeight() * iconFraction, iconMax);

    if (text.isEmpty())
    {
        icons::draw (g, icon, area.withSizeKeepingCentre (iconSize, iconSize), fg);
        return;
    }

    const auto font = juce::Font (theme::font (type::body));
    const auto textWidth = juce::GlyphArrangement::getStringWidth (font, text);
    const auto gap = icon == Icon::none ? 0.0f : 6.0f;
    const auto contentWidth = juce::jmin (area.getWidth() - 12.0f, (icon == Icon::none ? 0.0f : iconSize) + gap + textWidth);
    auto content = area.withSizeKeepingCentre (contentWidth, area.getHeight());

    if (icon != Icon::none)
        icons::draw (g, icon, content.removeFromLeft (iconSize).withSizeKeepingCentre (iconSize, iconSize), fg);

    content.removeFromLeft (gap);
    g.setColour (fg);
    g.setFont (font);
    g.drawFittedText (text, content.toNearestInt(), juce::Justification::centredLeft, 1, 0.85f);
}
} // namespace lynx
