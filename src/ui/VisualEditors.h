#pragma once

// Editors of LYNX's visual plugins (round 164c, docs/analise/REA_rodada164c.md):
//  - Wave Candy: one resizable display in the mode the tabs pick (Auto, Oscilloscope, Spectrum, Spectrogram, Peak meter,
//    Vectorscope) over the controls of that mode; hover the display to read frequency / note / level, middle-click or
//    the Hold button freezes it, right-click picks the mode.
//  - Visualizer: a live preview of the scene, its layers (add, remove, reorder, show / hide) and the selected layer's
//    settings, the six global knobs (automatable), built-in scenes, Full screen and Export video (the song rendered
//    offline frame by frame, muxed with ffmpeg when it is installed, else PNG images + WAV).
// The displays read the slot's tap (core/VisualFx.h) on the UI thread at 30–40 frames a second and only while visible;
// the audio thread does nothing but copy the signal.

#include <functional>
#include <memory>

#include "core/VisualFx.h"
#include "engine/AudioEngine.h"

namespace lynx
{
// What an editor shows: a mixer slot, or a node inside a Patcher
struct VisualTarget
{
    AudioEngine* engine = nullptr;
    std::function<EffectSlot* (Song&)> locate;           // the slot in a song (null = gone)
    std::function<std::shared_ptr<Effect>()> running;    // the running effect (may be null)
    int track = -1, slot = -1;                           // a mixer slot: automation targets for the knobs
    // the Patcher graph the node lives in: an export renders the song and finds the node's effect again
    std::function<std::shared_ptr<Effect> (AudioEngine&)> runningIn;

    const EffectSlot* current() const;
    void edit (const std::function<void (EffectSlot&)>& change, bool undoable = true) const;

    static VisualTarget mixerSlot (AudioEngine& engine, int track, int slot);
};

namespace VisualWindows
{
bool handles (EffectKind kind); // waveCandy, visualizer
void open (AudioEngine& engine, int track, int slot);
// a node in a Patcher (its own window, keyed by `key`)
void openNode (const VisualTarget& target, const juce::String& title, const juce::String& key);
void closeAll();
} // namespace VisualWindows

// The editors alone (snapshots, --editorcpu): `demo` feeds them a built-in test signal instead of the slot's audio.
std::unique_ptr<juce::Component> makeVisualEditor (AudioEngine& engine, int track, int slot, bool demo = false);

// One frame of a Visualizer scene drawn into an image (the video export and --visualtest).
class SceneRenderer
{
public:
    SceneRenderer();
    ~SceneRenderer();
    // moves the scene's animated layers (particles, rings) on to `frame` — once per frame
    void advance (const visualfx::scene::Scene& scene, const visualfx::Frame& frame, float aspect);
    void draw (juce::Graphics& g, juce::Rectangle<float> area, const visualfx::scene::Scene& scene, const visualfx::Frame& frame);
    void reset();

private:
    struct State;
    std::unique_ptr<State> state;
};

// ffmpeg for the video export: on the PATH, in the usual install folders, or the one the user located (remembered)
juce::File findFfmpeg();
void setFfmpegLocation (const juce::File& exe);

// LYNX.exe --visualtest <folder>: editor pictures of every Wave Candy mode and Visualizer preset fed a test signal, a
// CPU check of the audio path and a short video export of `project` (default: an FL ZGameEditor demo project), with
// ffmpeg if present and as images + WAV. Writes report.txt.
void runVisualTest (const juce::File& folder, const juce::File& project, std::function<void (bool)> done);
} // namespace lynx
