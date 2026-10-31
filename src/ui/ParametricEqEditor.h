#pragma once

#include <functional>

#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/AudioEngine.h"

namespace lynx
{
// 7-band parametric EQ (like FL's Parametric EQ 2). Its window is the native effect editor in FL's arrangement
// (ui/PluginEditors.h, core/PluginLayout.cpp): the response curve with one handle per band (drag = frequency and gain,
// wheel = Q / bandwidth, right-click = type and slope, double-click = 0 dB), the output fader and one column per band
// (type, gain fader, FREQ and BW knobs). These are the EQ's drawing helpers and the window entry points.
namespace ParametricEqWindows
{
void open (AudioEngine& engine, int track, int slot);
void closeAll(); // closes every native effect window
} // namespace ParametricEqWindows

// The editor alone (for --snapshot).
std::unique_ptr<juce::Component> makeParametricEqEditor (AudioEngine& engine, int track, int slot);

namespace peqview
{
class Analyser;
}

// Draws the EQ curve (also used in the mixer panel thumbnail); the graph spans ±rangeDb. `analyser` (round 166f): the
// live spectrum and heatmap behind the curve (the editor only).
void drawParametricEqCurve (juce::Graphics& g, juce::Rectangle<float> area, const EffectParams& params, double sampleRate,
                            bool enabled, int highlightBand = -1, double rangeDb = 24.0, bool labels = true,
                            const peqview::Analyser* analyser = nullptr);

namespace peqview
{
juce::Colour bandColour (int band);
float xForHz (juce::Rectangle<float> area, double hz);
double hzForX (juce::Rectangle<float> area, float x);
float yForDb (juce::Rectangle<float> area, double db, double rangeDb);
double dbForY (juce::Rectangle<float> area, float y, double rangeDb);
juce::String bandTypeName (int type);
bool hasGain (const EffectParams& params, int band);
// the handle of a band in the graph
juce::Point<float> handlePosition (juce::Rectangle<float> area, const EffectParams& params, int band, double rangeDb);
int bandAt (juce::Rectangle<float> area, const EffectParams& params, juce::Point<float> p, double rangeDb);
// right-click menu of a band: type, then FL's orders or the cut slope; `set` applies (param, value) changes
void showBandMenu (juce::Component& target, const EffectParams& params, int band, std::function<void (std::vector<std::pair<int, float>>)> set);
// one line about the band (type, frequency, gain, width / order)
juce::String bandText (const EffectParams& params, int band);

// Round 166f (issue #16): FL's Parametric EQ 2 display — the spectrum of the EQ's output (FL's "histogram", on by
// default) and the heatmap (Options › Heatmap: every frequency lit by its long-term level, cold → hot, so a boosted
// region glows hotter and a cut one colder; off by default, Full / Top / Bottom). The settings are LYNX-wide
// (settings folder, peq2-display.txt).
struct DisplaySettings
{
    bool spectrum = true, heatmap = false;
    int position = 0; // core/SpectrumHeat.h Position
};
DisplaySettings& displaySettings();
void saveDisplaySettings();
// the graph's right-click menu entries for them (`changed` repaints)
void addDisplayItems (juce::PopupMenu& menu, std::function<void()> changed);

class Analyser
{
public:
    Analyser();
    // reads the slot's analyser tap (its output); `claim`: take the tap (the editor in use). True when it changed.
    bool update (AudioEngine& engine, int track, int slot, bool claim, juce::Rectangle<float> graph);
    void paintHeat (juce::Graphics& g, juce::Rectangle<float> graph) const;
    void paintSpectrum (juce::Graphics& g, juce::Rectangle<float> graph) const;

private:
    static constexpr int fftOrder = 12, fftSize = 1 << fftOrder;
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> data, window, line;
    std::vector<double> heat; // dB per column (the long-term average)
    std::uint32_t lastWritten = 0;
    bool any = false;
};
} // namespace peqview
} // namespace lynx
