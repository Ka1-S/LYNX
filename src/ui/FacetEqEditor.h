#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/AudioEngine.h"

namespace lynx
{
// Facet EQ's own editor (REA round 103): a large response graph over a live spectrum analyser (the slot's input and
// output), one node per band — drag = frequency and gain, mouse wheel = Q, double-click on the graph = new band,
// double-click on a node = remove it, right-click = shape / slope / dynamics / solo menu, the small diamond under a
// dynamic band's node = its range —, a strip with the selected band's controls (shape, frequency, gain, Q, slope,
// dynamic range and threshold, solo) and the global ones (phase mode with its latency, analyser, attack / release,
// detector and sidechain, output, gain scale).
namespace FacetEqWindows
{
void open (AudioEngine& engine, int track, int slot);
void closeAll();
} // namespace FacetEqWindows

// The response curve alone (the mixer panel's thumbnail)
void drawFacetEqCurve (juce::Graphics& g, juce::Rectangle<float> area, const EffectParams& params, double sampleRate, bool enabled);

// The editor alone (for --plugui / --snapshot); `demo` fills the analyser with a synthetic spectrum.
std::unique_ptr<juce::Component> makeFacetEqEditor (AudioEngine& engine, int track, int slot, bool demo = false);
// Round 166f (--eq166fshot): one analyser tick without the editor's timer (claims the slot's analyser tap)
void tickFacetEqEditor (juce::Component& editor);
} // namespace lynx
