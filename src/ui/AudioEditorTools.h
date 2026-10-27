#pragma once

// The audio editor's tool windows (round 165l): LYNX's versions of FL Edison's tool forms (Amp, Equalize, Match
// spectrum, Blur, Convolution reverb, Clean up, Vocal denoise, Claw machine, Drum(loop) stretch, Loop tuner and the
// point-envelope tools Scale levels / Decimate / Filter / Smooth up / Create sequence). Each window lays its controls
// out in sections like FL's form (docs/analise/REA_rodada165l.md), in LYNX's look; Reset / Preview / Accept /
// Cancel; the audio tools run on a worker thread with a progress bar and Cancel, Accept makes one undo step. The DSP
// is core/AudioEditTools.

#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/AudioEditTools.h"

namespace lynx::audiotools
{
// What a tool window needs from the editor that opened it.
class Host
{
public:
    virtual ~Host() = default;
    virtual const audioedit::Document& toolDocument() const = 0;
    virtual audioedit::Range toolSelection() const = 0;
    // Accept: the edited document (one undo step labelled `label`) and the selection to show afterwards.
    virtual void toolApply (const juce::String& label, audioedit::Document doc, audioedit::Range selection) = 0;
    virtual void toolPreview (const audioedit::Audio& audio) = 0;
    virtual void toolStopPreview() = 0;
    virtual double toolBpm() const = 0;
    virtual audioedit::NoiseProfile& toolNoiseProfile() = 0;
    virtual const audioedit::Audio& toolClipboard() const = 0;
    // the envelope tools: the envelope being edited over the audio (nullptr when none is shown)
    virtual audioedit::Envelope* toolEnvelope() = 0;
    virtual void toolEnvelopeChanged (bool commit, const juce::String& label) = 0;
    virtual juce::Component& toolComponent() = 0;
};

enum class Tool
{
    amp,
    equalize,
    matchSpectrum,
    blur,
    reverb,
    denoise,
    vocalDenoise,
    claw,
    drumStretch,
    loopTuner,
    scaleLevels,
    decimate,
    envFilter,
    smoothUp,
    createSequence
};

const char* toolTitle (Tool tool); // English, for tr()

// The window's content (also for --snapshot).
std::unique_ptr<juce::Component> makeToolPanel (Host& host, Tool tool);
// Opens the tool in its own window (one audio-editor tool window at a time).
void showTool (Host& host, Tool tool);

// A point-envelope editor (the curves of the tool forms), with FL's point editing: right-click (or click) on an empty
// spot adds a point and drags it, drag a point moves it (Shift: only its level), right-click or Alt+click on a point
// deletes it, the hollow handle between two points bends the segment (right-click it: straight again), double-click
// a point straightens the segment that ends at it.
class CurveEditor : public juce::Component
{
public:
    enum class Axis
    {
        time,
        frequency
    };

    CurveEditor();

    audioedit::Envelope envelope;
    Axis axis = Axis::time;
    double freqScale = 1.0, sampleRate = 44100.0;
    bool centreLine = false;           // a reference line at y = 0.5 (0 dB of the EQ curve, unity)
    std::vector<float> background;     // a level per column 0..1 drawn under the curve (spectrum), optional
    juce::String caption;              // drawn in a corner
    std::function<juce::String (double x, double y)> describe; // the readout while dragging
    std::function<void()> onChange;    // while editing
    std::function<void()> onEditEnd;   // mouse up after a change

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;

    // the drawing area inside the component (the rest holds the axis labels)
    juce::Rectangle<float> plotArea() const;
    juce::Point<float> toScreen (double x, double y) const;
    std::pair<double, double> fromScreen (juce::Point<float> p) const;

private:
    int hitPoint (juce::Point<float> p) const;
    int hitHandle (juce::Point<float> p) const; // the segment whose tension handle is under p (its end point), or -1
    int dragging = -1, bending = -1;
    float bendStartY = 0.0f, bendStartTension = 0.0f;
    bool changed = false;
    juce::Point<float> hover;
    juce::String readout;
};
} // namespace lynx::audiotools
