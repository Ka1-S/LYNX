#include "ui/TouchKeyboard.h"
#include "core/NoteNames.h"

#include <map>

#include "core/HeldNotes.h"
#include "core/TouchKeyboard.h"
#include "engine/AudioEngine.h"
#include "ui/I18n.h"
#include "ui/SettingsPages.h"
#include "ui/Theme.h"
#include "ui/ToolWindow.h"

namespace lynx
{
namespace
{
juce::String keyName (int key)
{
    return juce::String (notenames::keyName (key)); // Settings › General › Note names (round 165f)
}

class Keys final : public juce::Component
{
public:
    std::function<int()> channel;              // the channel to play (-1: none)
    std::function<float (float)> fixedVelocity; // returns < 0 when the position decides
    bool labels = true;
    touchkb::Layout layout;
    std::map<int, float> shown;               // key → velocity, for drawing (also the snapshot's pressed keys)

    explicit Keys (AudioEngine& e) : engine (e), notes ([this] (int ch, int key, float velocity, bool on) {
                                                    if (live)
                                                        engine.sendLiveNote (ch, key, velocity, on, true);
                                                    if (on)
                                                        shown[key] = velocity;
                                                    else
                                                        shown.erase (key);
                                                    repaint();
                                                })
    {
        setRepaintsOnMouseActivity (false);
    }

    ~Keys() override { notes.releaseAll(); }

    void setLive (bool on) { live = on; }
    void releaseAll() { notes.releaseAll(); }
    bool holding() const { return ! notes.empty(); }

    void resized() override
    {
        layout.width = (float) getWidth();
        layout.height = (float) getHeight();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg0);
        const auto top = layout.lowest, last = layout.highest();
        auto draw = [&] (bool black) {
            for (int key = top; key <= last + 1 && key <= 127; ++key)
            {
                if (touchkb::isBlack (key) != black || (key > last && ! black))
                    continue;
                const auto r = layout.keyRect (key);
                if (r.x + r.w > layout.width + 0.5f)
                    continue;
                auto box = juce::Rectangle<float> (r.x, r.y, r.w, r.h);
                const auto held = shown.find (key);
                if (! black)
                {
                    box = box.reduced (1.0f, 0.0f).withTrimmedBottom (1.0f);
                    g.setColour (held != shown.end() ? theme::colour::keyWhite.interpolatedWith (theme::colour::accent, 0.35f + 0.5f * held->second)
                                                     : theme::colour::keyWhite);
                    g.fillRoundedRectangle (box, 3.0f);
                    if (labels && (key % 12 == 0 || held != shown.end()))
                    {
                        g.setColour (held != shown.end() ? theme::colour::onAccent : juce::Colour (0xff6b707a));
                        g.setFont (theme::font (theme::type::caption, key % 12 == 0));
                        g.drawText (keyName (key), box.withTrimmedBottom (6.0f).toNearestInt(), juce::Justification::centredBottom);
                    }
                }
                else
                {
                    g.setColour (held != shown.end() ? theme::colour::accent.withMultipliedBrightness (0.6f + 0.4f * held->second) : theme::colour::keyBlack);
                    g.fillRoundedRectangle (box.withTrimmedTop (-3.0f), 3.0f);
                    g.setColour (juce::Colours::white.withAlpha (0.07f));
                    g.fillRect (box.reduced (3.0f, 0.0f).withTrimmedTop (box.getHeight() - 6.0f));
                }
            }
        };
        draw (false);
        draw (true);
        // the window's top edge over the keys
        g.setColour (theme::colour::accent.withAlpha (0.8f));
        g.fillRect (0, 0, getWidth(), 2);
    }

    void mouseDown (const juce::MouseEvent& e) override { touch (e); }
    void mouseDrag (const juce::MouseEvent& e) override { touch (e); }
    void mouseUp (const juce::MouseEvent& e) override { notes.releaseTag (e.source.getIndex()); }
    void mouseExit (const juce::MouseEvent& e) override
    {
        if (! e.mods.isAnyMouseButtonDown())
            notes.releaseTag (e.source.getIndex());
    }
    void focusLost (FocusChangeType) override { notes.releaseAll(); }
    void visibilityChanged() override
    {
        if (! isShowing())
            notes.releaseAll();
    }
    void parentHierarchyChanged() override
    {
        if (! isShowing())
            notes.releaseAll();
    }

private:
    void touch (const juce::MouseEvent& e)
    {
        const auto tag = e.source.getIndex();
        const auto ch = channel ? channel() : -1;
        const auto pos = e.position;
        const auto key = getLocalBounds().toFloat().contains (pos) ? layout.noteAt (pos.x, pos.y) : -1;
        const auto currentKey = keyOf (tag);

        if (key < 0 || ch < 0)
        {
            notes.releaseTag (tag); // dragged off the keys
            return;
        }
        if (key == currentKey)
            return;

        notes.releaseTag (tag); // glide: the previous key stops before the next starts
        const auto fixed = fixedVelocity ? fixedVelocity (0.0f) : -1.0f;
        notes.press (ch, key, fixed >= 0.0f ? fixed : layout.velocityAt (key, pos.y), tag);
        tagKeys[tag] = key;
    }

    int keyOf (int tag) const
    {
        const auto it = tagKeys.find (tag);
        return it != tagKeys.end() && notes.hasTag (tag) ? it->second : -1;
    }

    AudioEngine& engine;
    HeldNotes notes;
    std::map<int, int> tagKeys;
    bool live = true;
};

class TouchKeyboardPanel final : public juce::Component,
                                 private juce::Timer
{
public:
    TouchKeyboardPanel (AudioEngine& e, bool liveMode) : engine (e), keys (e)
    {
        keys.setLive (liveMode);
        keys.channel = [this] { return engine.getSelectedChannel(); };
        keys.fixedVelocity = [this] (float) { return velocityMode.getSelectedItemIndex() == 0 ? -1.0f : (float) fixedVelocity.getValue() / 100.0f; };
        addAndMakeVisible (keys);

        octaveDown.setTooltip (tr ("One octave down"));
        octaveUp.setTooltip (tr ("One octave up"));
        octaveDown.onClick = [this] { shift (-1); };
        octaveUp.onClick = [this] { shift (1); };
        addAndMakeVisible (octaveDown);
        addAndMakeVisible (octaveUp);
        range.setFont (theme::monoFont (theme::type::small));
        range.setJustificationType (juce::Justification::centred);
        range.setColour (juce::Label::textColourId, theme::colour::textPrimary);
        addAndMakeVisible (range);

        sizeBox.addItemList ({ tr ("Small keys"), tr ("Medium keys"), tr ("Large keys") }, 1);
        sizeBox.setSelectedItemIndex (1, juce::dontSendNotification);
        sizeBox.onChange = [this] { layoutKeys(); };
        addAndMakeVisible (sizeBox);
        velocityMode.addItemList ({ tr ("Velocity: where you touch"), tr ("Velocity: fixed") }, 1);
        velocityMode.setSelectedItemIndex (0, juce::dontSendNotification);
        velocityMode.setTooltip (tr ("Where you touch: near the top of a key plays soft, near the bottom loud"));
        velocityMode.onChange = [this] { fixedVelocity.setEnabled (velocityMode.getSelectedItemIndex() == 1); };
        addAndMakeVisible (velocityMode);
        fixedVelocity.setSliderStyle (juce::Slider::LinearHorizontal);
        fixedVelocity.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 20);
        fixedVelocity.setRange (1.0, 100.0, 1.0);
        fixedVelocity.setValue (80.0, juce::dontSendNotification);
        fixedVelocity.setEnabled (false);
        addAndMakeVisible (fixedVelocity);
        labels.setButtonText (tr ("Note names"));
        labels.setToggleState (true, juce::dontSendNotification);
        labels.onClick = [this] {
            keys.labels = labels.getToggleState();
            keys.repaint();
        };
        addAndMakeVisible (labels);
        target.setFont (theme::font (theme::type::body));
        target.setColour (juce::Label::textColourId, theme::colour::textSecondary);
        target.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (target);

        if (liveMode)
            startTimerHz (10);
        timerCallback();
        setSize (820, 210);
        layoutKeys();
    }

    ~TouchKeyboardPanel() override { keys.releaseAll(); }

    void showPressedForSnapshot()
    {
        keys.shown = { { 60, 0.95f }, { 64, 0.7f }, { 67, 0.45f }, { 70, 0.8f } };
        keys.repaint();
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::colour::bg3); }

    void resized() override
    {
        auto area = getLocalBounds();
        auto bar = area.removeFromTop (38).reduced (10, 6);
        octaveDown.setBounds (bar.removeFromLeft (30));
        range.setBounds (bar.removeFromLeft (92));
        octaveUp.setBounds (bar.removeFromLeft (30));
        bar.removeFromLeft (12);
        sizeBox.setBounds (bar.removeFromLeft (124));
        bar.removeFromLeft (8);
        velocityMode.setBounds (bar.removeFromLeft (196));
        bar.removeFromLeft (6);
        fixedVelocity.setBounds (bar.removeFromLeft (120));
        bar.removeFromLeft (8);
        labels.setBounds (bar.removeFromLeft (110));
        target.setBounds (bar);
        keys.setBounds (area);
    }

private:
    void layoutKeys()
    {
        static const int whites[] = { 35, 28, 21 };
        keys.layout.whiteKeys = whites[juce::jlimit (0, 2, sizeBox.getSelectedItemIndex())];
        keys.layout.lowest = touchkb::shiftOctave (keys.layout.lowest, 0, keys.layout.whiteKeys);
        keys.releaseAll();
        updateRange();
        keys.repaint();
    }

    void shift (int octaves)
    {
        keys.releaseAll(); // the held keys would otherwise change under the finger
        keys.layout.lowest = touchkb::shiftOctave (keys.layout.lowest, octaves, keys.layout.whiteKeys);
        updateRange();
        keys.repaint();
    }

    void updateRange()
    {
        range.setText (keyName (keys.layout.lowest) + " - " + keyName (keys.layout.highest()), juce::dontSendNotification);
        octaveDown.setEnabled (keys.layout.lowest >= 12);
        touchkb::Layout probe = keys.layout;
        probe.lowest += 12;
        octaveUp.setEnabled (probe.highest() <= 127);
    }

    void timerCallback() override
    {
        const auto channel = engine.getSelectedChannel();
        if (channel != lastChannel)
        {
            keys.releaseAll(); // the notes go to the channel that started them; stop them before switching
            lastChannel = channel;
        }
        const auto& song = engine.getSong();
        target.setText (channel >= 0 && channel < (int) song.channels.size()
                            ? tr ("Plays: {0}", { juce::String::fromUTF8 (song.channels[(std::size_t) channel].name.c_str()) })
                            : tr ("Select a channel in the Channel Rack"),
                        juce::dontSendNotification);
    }

    AudioEngine& engine;
    Keys keys;
    int lastChannel = -2;
    juce::TextButton octaveDown { "-" }, octaveUp { "+" };
    juce::Label range, target;
    juce::ComboBox sizeBox, velocityMode;
    juce::Slider fixedVelocity;
    juce::ToggleButton labels;
};
} // namespace

bool isTouchKeyboardOpen()
{
    return findToolWindow ("touch-keyboard") != nullptr;
}

void toggleTouchKeyboard (AudioEngine& engine)
{
    if (auto* w = findToolWindow ("touch-keyboard"))
    {
        if (auto* content = w->getContentComponent())
            closeToolWindowOf (*content);
        return;
    }
    if (auto* window = openToolWindow (std::make_unique<TouchKeyboardPanel> (engine, true), tr ("Touch keyboard"), "touch-keyboard"))
        if (auto* main = juce::TopLevelWindow::getTopLevelWindow (0))
        {
            // along the bottom of the main window, where an on-screen keyboard is expected
            const auto area = main->getScreenBounds();
            window->setTopLeftPosition (area.getCentreX() - window->getWidth() / 2, area.getBottom() - window->getHeight() - 40);
        }
}

std::unique_ptr<juce::Component> makeTouchKeyboardPanel (AudioEngine& engine)
{
    auto panel = std::make_unique<TouchKeyboardPanel> (engine, false);
    panel->showPressedForSnapshot();
    return panel;
}
} // namespace lynx
