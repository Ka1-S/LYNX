#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

namespace lynx::ui
{
// LYNX's colour picker (round 162d, issues #8 / #9): a hue/saturation wheel, a brightness strip, H / S / V sliders, a
// hex field and a before/after preview. Used by Options › Theme (accent, background, workspace) and by the
// "Rename and color" dialogs of mixer tracks, channels, patterns and playlist tracks.
class ColourPicker final : public juce::Component
{
public:
    explicit ColourPicker (juce::Colour initial = juce::Colours::red, bool compact = false);
    ~ColourPicker() override;

    juce::Colour getColour() const;
    // `notify` false: no onChange (e.g. the owner setting the colour from a swatch)
    void setColour (juce::Colour c, bool notify = false);
    // the colour the preview shows as "before" (default: the initial one)
    void setOriginal (juce::Colour c);

    std::function<void (juce::Colour)> onChange; // every change while dragging

    void resized() override;
    void paint (juce::Graphics&) override;

    static constexpr int preferredWidth = 360;
    int preferredHeight() const noexcept { return compact ? 150 : 176; }

private:
    class Wheel;
    class Strip;
    void update (bool notify, juce::Component* source);

    float hue = 0.0f, saturation = 1.0f, brightness = 1.0f;
    juce::Colour original;
    bool compact = false, updating = false;
    std::unique_ptr<Wheel> wheel;
    std::unique_ptr<Strip> strip;
    juce::Slider hueSlider, saturationSlider, brightnessSlider;
    juce::Label hueLabel, saturationLabel, brightnessLabel, hexLabel;
    juce::TextEditor hexField;
    juce::Rectangle<int> previewArea;
};

// A colour picker in a pop-up pointing at `screenArea` (e.g. the mouse position) (OK or clicking outside keeps the
// colour, Cancel / Escape restores the original).
// `onPreview` gets every change while the pop-up is open (live preview; may be empty); `onDone` gets the final colour
// on OK, `onCancel` runs when the change is dropped.
void showColourPickerPopup (juce::Rectangle<int> screenArea, const juce::String& title, juce::Colour initial, std::function<void (juce::Colour)> onPreview,
                            std::function<void (juce::Colour)> onDone, std::function<void()> onCancel = {});

// The "Change color" submenu of mixer tracks, channels, patterns, playlist tracks and clips: Default, LYNX's palette and
// "Custom color..." (the picker in a pop-up at `screenArea`). `set` gets the chosen colour (0 = default).
juce::PopupMenu colourMenu (std::uint32_t current, std::function<void (std::uint32_t)> set, juce::Rectangle<int> screenArea);
} // namespace lynx::ui
