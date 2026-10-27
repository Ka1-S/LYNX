#pragma once

// LYNX Dance on screen (round 165e): the dancers of the project's dance channels drawn over the workspace (drag one to
// move it, right-click for its moves), and the channel's editor. The character is LYNX's own, drawn with vector shapes
// (core/FruityDance.h has the moves and FL's key map).

#include <functional>
#include <map>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/FruityDance.h"

namespace lynx
{
class AudioEngine;
class Instrument;

namespace dancer
{
// The character in `pose`, standing at `feet` (between its feet), `height` pixels tall at rest
void draw (juce::Graphics& g, juce::Point<float> feet, float height, const dance::Pose& pose, bool mirrored, juce::Colour outfit);
// The box the character can reach (arms up, jumps), for repaints and the mouse
juce::Rectangle<float> reach (juce::Point<float> feet, float height);
} // namespace dancer

// Over the workspace: one dancer per visible dance channel. Only the dancers take the mouse.
class DanceStage final : public juce::Component,
                         private juce::Timer
{
public:
    explicit DanceStage (AudioEngine& engine);
    ~DanceStage() override;

    void paint (juce::Graphics& g) override;
    bool hitTest (int x, int y) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;

    int dancerCount() const noexcept { return (int) dancers.size(); }

private:
    struct Dancer
    {
        int channel = -1;
        std::vector<std::uint8_t> savedState;
        dance::Settings saved, now; // as saved; as the notes left it
        dance::Pose from, shown;
        double blendStart = -1.0, cycles = 0.0;
        int lastMove = -1;
        std::uint32_t seen = 0;
        std::shared_ptr<Instrument> instrument;
        juce::Rectangle<float> bounds; // painted last
        bool drawn = false;
    };

    void timerCallback() override;
    void scan();
    juce::Rectangle<float> boxOf (const Dancer& d) const;
    Dancer* dancerAt (juce::Point<float> p);
    void showMenu (Dancer& d);
    void save (int channel, const dance::Settings& s);

    AudioEngine& engine;
    std::map<int, Dancer> dancers;
    double lastMs = 0.0, freeBeats = 0.0;
    bool wasPlaying = false;
    int scanCountdown = 0;
    int dragging = -1;
    juce::Point<int> dragOffset;
};

// The dance channel's settings: a live preview, the move, mirrored / shown, size, speed, the outfit colour, the keys
std::unique_ptr<juce::Component> makeDanceEditor (AudioEngine& engine, int channel);
} // namespace lynx
