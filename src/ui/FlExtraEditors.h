#pragma once

// Editors of the FL plugins added in round 165g (docs/analise/REA_rodada165g.md, core/FlExtraFx.h):
//  - Fruity Big Clock: the song position in big digits; right-click (or a click on the caption) picks the display.
//  - Tuner: Basic (note, octave, frequency, offset bar) or Dial (a clock face in cents) over Reference, Reactivity, Bass
//    and the Visual switch. It reads the slot's tap (core/VisualFx.h) on the UI thread, only while visible.
//  - Fruity HTML NoteBook: the page's address (a local .html file or a web address), Browse, Open in browser, and the
//    local page shown as formatted text (links open in the browser; local pages in place). LYNX never fetches web pages.
//  - Fruity Scratcher: the platter (drag it to scratch: Hold is on while the button is down), the waveform strip with the
//    platter's position, Load sample / file drop, the transport (Backwards / Stopped / Forwards) and FL's knobs.
// Fruity Mute 2 uses the plugin layout editor (core/PluginLayout.cpp).

#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/Song.h"
#include "ui/VisualEditors.h"

namespace lynx
{
class AudioEngine;

namespace FlExtraWindows
{
bool handles (EffectKind kind); // bigClock, tuner, htmlNotebook, scratcher
void open (AudioEngine& engine, int track, int slot);
void openNode (const VisualTarget& target, const juce::String& title, const juce::String& key); // a node in a Patcher
void closeAll();
} // namespace FlExtraWindows

// The editor alone (snapshots): `demo` feeds the Tuner a built-in test tone and the clock a moving position
std::unique_ptr<juce::Component> makeFlExtraEditor (const VisualTarget& target, EffectKind kind, bool demo = false);

// LYNX.exe --flextrashot <folder>: PNGs of every editor of round 165g in its states (Big Clock's three displays, the Tuner's
// Basic / Dial on test tones, the HTML NoteBook on FL's DefaultHtml.html or a built-in page, Mute 2's layout, the
// Scratcher in motor and hold views, the Transmitter with its section effects).
// Writes report.txt; true when every picture was made.
bool runFlExtraShots (AudioEngine& engine, const juce::File& folder);
} // namespace lynx
