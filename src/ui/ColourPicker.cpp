#include "ui/ColourPicker.h"

#include "core/RackOps.h"
#include "core/ThemePalette.h"
#include "ui/I18n.h"
#include "ui/Theme.h"

namespace lynx::ui
{
// --- the hue/saturation wheel ----------------------------------------------------------------------------------------
class ColourPicker::Wheel final : public juce::Component
{
public:
    explicit Wheel (ColourPicker& p) : picker (p) { setRepaintsOnMouseActivity (false); }

    void paint (juce::Graphics& g) override
    {
        const auto size = juce::jmin (getWidth(), getHeight());
        if (size <= 4)
            return;
        if (image.getWidth() != size || imageBrightness != picker.brightness)
            render (size);
        const auto origin = juce::Point<int> ((getWidth() - size) / 2, (getHeight() - size) / 2);
        g.drawImageAt (image, origin.x, origin.y);

        // the marker
        const auto radius = (float) size * 0.5f - 1.0f;
        const auto angle = picker.hue * juce::MathConstants<float>::twoPi;
        const auto centre = origin.toFloat() + juce::Point<float> ((float) size * 0.5f, (float) size * 0.5f);
        const auto at = centre + juce::Point<float> (std::cos (angle), -std::sin (angle)) * (picker.saturation * radius);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre (at), 3.0f);
        g.setColour (juce::Colours::white);
        g.drawEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre (at), 1.5f);
    }

    void mouseDown (const juce::MouseEvent& e) override { pick (e.position); }
    void mouseDrag (const juce::MouseEvent& e) override { pick (e.position); }

private:
    void pick (juce::Point<float> p)
    {
        const auto size = (float) juce::jmin (getWidth(), getHeight());
        const auto centre = getLocalBounds().toFloat().getCentre();
        const auto d = p - centre;
        const auto radius = size * 0.5f - 1.0f;
        auto angle = std::atan2 (-d.y, d.x);
        if (angle < 0.0f)
            angle += juce::MathConstants<float>::twoPi;
        picker.hue = angle / juce::MathConstants<float>::twoPi;
        picker.saturation = juce::jlimit (0.0f, 1.0f, d.getDistanceFromOrigin() / radius);
        picker.update (true, this);
    }

    void render (int size)
    {
        imageBrightness = picker.brightness;
        image = juce::Image (juce::Image::ARGB, size, size, true);
        juce::Image::BitmapData data (image, juce::Image::BitmapData::writeOnly);
        const auto radius = (float) size * 0.5f - 1.0f;
        const auto c = (float) size * 0.5f;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                const auto dx = (float) x + 0.5f - c, dy = (float) y + 0.5f - c;
                const auto dist = std::sqrt (dx * dx + dy * dy);
                if (dist > radius + 1.0f)
                    continue;
                auto angle = std::atan2 (-dy, dx);
                if (angle < 0.0f)
                    angle += juce::MathConstants<float>::twoPi;
                const auto alpha = juce::jlimit (0.0f, 1.0f, radius + 1.0f - dist); // a soft edge
                data.setPixelColour (x, y, juce::Colour::fromHSV (angle / juce::MathConstants<float>::twoPi, juce::jmin (1.0f, dist / radius),
                                                                   imageBrightness, alpha));
            }
    }

    ColourPicker& picker;
    juce::Image image;
    float imageBrightness = -1.0f;
};

// --- the brightness strip ---------------------------------------------------------------------------------------------
class ColourPicker::Strip final : public juce::Component
{
public:
    explicit Strip (ColourPicker& p) : picker (p) {}

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f, 2.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour::fromHSV (picker.hue, picker.saturation, 1.0f, 1.0f), r.getTopLeft(),
                                                 juce::Colours::black, r.getBottomLeft(), false));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (theme::colour::borderStrong);
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
        const auto y = r.getY() + (1.0f - picker.brightness) * r.getHeight();
        g.setColour (juce::Colours::white);
        g.fillRect (juce::Rectangle<float> (r.getX() - 1.0f, y - 1.5f, r.getWidth() + 2.0f, 3.0f));
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawRect (juce::Rectangle<float> (r.getX() - 1.0f, y - 2.5f, r.getWidth() + 2.0f, 5.0f), 1.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override { pick (e.position.y); }
    void mouseDrag (const juce::MouseEvent& e) override { pick (e.position.y); }

private:
    void pick (float y)
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f, 2.0f);
        picker.brightness = juce::jlimit (0.0f, 1.0f, 1.0f - (y - r.getY()) / juce::jmax (1.0f, r.getHeight()));
        picker.update (true, this);
    }

    ColourPicker& picker;
};

// --- the picker -------------------------------------------------------------------------------------------------------
ColourPicker::ColourPicker (juce::Colour initial, bool isCompact) : original (initial), compact (isCompact)
{
    wheel = std::make_unique<Wheel> (*this);
    strip = std::make_unique<Strip> (*this);
    addAndMakeVisible (*wheel);
    addAndMakeVisible (*strip);

    auto setUp = [this] (juce::Slider& s, juce::Label& l, const juce::String& name, double max, const juce::String& suffix) {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 20);
        s.setRange (0.0, max, 1.0);
        s.setTextValueSuffix (suffix);
        s.setName (name);
        s.onValueChange = [this, &s] {
            if (updating)
                return;
            if (&s == &hueSlider)
                hue = (float) (s.getValue() / 360.0);
            else if (&s == &saturationSlider)
                saturation = (float) (s.getValue() / 100.0);
            else
                brightness = (float) (s.getValue() / 100.0);
            update (true, &s);
        };
        l.setText (name, juce::dontSendNotification);
        l.setFont (theme::font (theme::type::small));
        l.setColour (juce::Label::textColourId, theme::colour::textSecondary);
        addAndMakeVisible (s);
        addAndMakeVisible (l);
    };
    setUp (hueSlider, hueLabel, tr ("Hue"), 359.0, juce::String::fromUTF8 ("\xc2\xb0"));
    setUp (saturationSlider, saturationLabel, tr ("Saturation"), 100.0, "%");
    setUp (brightnessSlider, brightnessLabel, tr ("Brightness"), 100.0, "%");

    hexLabel.setText (tr ("Hex"), juce::dontSendNotification);
    hexLabel.setFont (theme::font (theme::type::small));
    hexLabel.setColour (juce::Label::textColourId, theme::colour::textSecondary);
    addAndMakeVisible (hexLabel);
    hexField.setFont (theme::monoFont (theme::type::body));
    hexField.setInputRestrictions (9, "#0123456789abcdefABCDEF");
    hexField.setIndents (6, 3);
    hexField.setTooltip (tr ("A color as #RRGGBB (Enter applies)"));
    auto applyHex = [this] {
        std::uint32_t argb = 0;
        if (themes::parseHex (hexField.getText().toStdString(), argb))
            setColour (juce::Colour (argb), true);
        else
            update (false, nullptr); // back to the current value
    };
    hexField.onReturnKey = applyHex;
    hexField.onFocusLost = applyHex;
    addAndMakeVisible (hexField);

    setColour (initial, false);
    setSize (preferredWidth, preferredHeight());
}

ColourPicker::~ColourPicker() = default;

juce::Colour ColourPicker::getColour() const
{
    return juce::Colour::fromHSV (hue, saturation, brightness, 1.0f);
}

void ColourPicker::setColour (juce::Colour c, bool notify)
{
    float h = 0.0f, s = 0.0f, v = 0.0f;
    c.getHSB (h, s, v);
    if (s > 0.0f) // a grey keeps the hue the user had
        hue = h;
    saturation = s;
    brightness = v;
    update (notify, nullptr);
}

void ColourPicker::setOriginal (juce::Colour c)
{
    original = c;
    repaint (previewArea);
}

void ColourPicker::update (bool notify, juce::Component* source)
{
    {
        const juce::ScopedValueSetter<bool> guard (updating, true);
        if (source != &hueSlider)
            hueSlider.setValue (std::round (hue * 360.0f) >= 360.0f ? 0.0 : std::round (hue * 360.0f), juce::dontSendNotification);
        if (source != &saturationSlider)
            saturationSlider.setValue (std::round (saturation * 100.0f), juce::dontSendNotification);
        if (source != &brightnessSlider)
            brightnessSlider.setValue (std::round (brightness * 100.0f), juce::dontSendNotification);
        if (! hexField.hasKeyboardFocus (true) || source == nullptr)
            hexField.setText (themes::toHex (getColour().getARGB()), false);
    }
    wheel->repaint();
    strip->repaint();
    repaint (previewArea);
    if (notify && onChange)
        onChange (getColour());
}

void ColourPicker::resized()
{
    auto area = getLocalBounds();
    const auto wheelSize = juce::jmin (area.getHeight(), compact ? 140 : 168);
    wheel->setBounds (area.removeFromLeft (wheelSize).withSizeKeepingCentre (wheelSize, wheelSize));
    area.removeFromLeft (8);
    strip->setBounds (area.removeFromLeft (16).withSizeKeepingCentre (16, wheelSize));
    area.removeFromLeft (12);

    const auto rowH = compact ? 24 : 28;
    previewArea = area.removeFromTop (rowH).reduced (0, 2);
    area.removeFromTop (compact ? 4 : 8);
    for (auto [s, l] : { std::pair { &hueSlider, &hueLabel }, { &saturationSlider, &saturationLabel }, { &brightnessSlider, &brightnessLabel } })
    {
        auto row = area.removeFromTop (rowH);
        l->setBounds (row.removeFromLeft (70));
        s->setBounds (row);
    }
    area.removeFromTop (4);
    auto row = area.removeFromTop (rowH);
    hexLabel.setBounds (row.removeFromLeft (70));
    hexField.setBounds (row.removeFromLeft (100).reduced (0, 1));
}

void ColourPicker::paint (juce::Graphics& g)
{
    if (previewArea.isEmpty())
        return;
    // before | after
    const auto r = previewArea.toFloat();
    const auto half = r.withWidth (r.getWidth() * 0.5f);
    g.setColour (original);
    g.fillRoundedRectangle (half, 4.0f);
    g.setColour (getColour());
    g.fillRoundedRectangle (half.withX (half.getRight()), 4.0f);
    g.setFont (theme::font (theme::type::small));
    g.setColour (juce::Colour (themes::readableTextOn (original.getARGB())));
    g.drawText (tr ("Before"), half.toNearestInt(), juce::Justification::centred);
    g.setColour (juce::Colour (themes::readableTextOn (getColour().getARGB())));
    g.drawText (trc ("@colour after change: New"), half.withX (half.getRight()).toNearestInt(), juce::Justification::centred);
    g.setColour (theme::colour::borderStrong);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
}

// --- the pop-up -------------------------------------------------------------------------------------------------------
namespace
{
class PickerPopup final : public juce::Component
{
public:
    PickerPopup (const juce::String& titleText, juce::Colour initial, std::function<void (juce::Colour)> preview, std::function<void (juce::Colour)> done,
                 std::function<void()> cancelled)
        : picker (initial), onPreview (std::move (preview)), onDone (std::move (done)), onCancel (std::move (cancelled))
    {
        title.setText (titleText, juce::dontSendNotification);
        title.setFont (theme::font (theme::type::body, true));
        addAndMakeVisible (title);
        addAndMakeVisible (picker);
        picker.onChange = [this] (juce::Colour c) {
            if (onPreview)
                onPreview (c);
        };
        ok.onClick = [this] { finish (true); };
        cancel.onClick = [this] { finish (false); };
        ok.setColour (juce::TextButton::buttonColourId, theme::colour::accent);
        ok.setColour (juce::TextButton::textColourOffId, theme::colour::onAccent);
        addAndMakeVisible (ok);
        addAndMakeVisible (cancel);
        setWantsKeyboardFocus (true);
        setSize (ColourPicker::preferredWidth + 24, picker.preferredHeight() + 90);
    }

    ~PickerPopup() override
    {
        if (! finished && onDone) // closed by clicking outside: keep the colour
            onDone (picker.getColour());
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            finish (false);
            return true;
        }
        if (key == juce::KeyPress::returnKey)
        {
            finish (true);
            return true;
        }
        return false;
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 10);
        title.setBounds (area.removeFromTop (22));
        area.removeFromTop (6);
        picker.setBounds (area.removeFromTop (picker.preferredHeight()));
        area.removeFromTop (10);
        auto buttons = area.removeFromTop (28);
        cancel.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        ok.setBounds (buttons.removeFromRight (90));
    }

private:
    void finish (bool keep)
    {
        if (finished)
            return;
        finished = true;
        if (keep && onDone)
            onDone (picker.getColour());
        if (! keep && onCancel)
            onCancel();
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
            box->dismiss();
    }

    juce::Label title;
    ColourPicker picker;
    juce::TextButton ok { tr ("OK") }, cancel { tr ("Cancel") };
    std::function<void (juce::Colour)> onPreview, onDone;
    std::function<void()> onCancel;
    bool finished = false;
};
} // namespace

void showColourPickerPopup (juce::Rectangle<int> screenArea, const juce::String& title, juce::Colour initial, std::function<void (juce::Colour)> onPreview,
                            std::function<void (juce::Colour)> onDone, std::function<void()> onCancel)
{
    auto content = std::make_unique<PickerPopup> (title, initial, std::move (onPreview), std::move (onDone), std::move (onCancel));
    juce::CallOutBox::launchAsynchronously (std::move (content), screenArea, nullptr);
}

juce::PopupMenu colourMenu (std::uint32_t current, std::function<void (std::uint32_t)> set, juce::Rectangle<int> screenArea)
{
    juce::PopupMenu colours;
    colours.addItem (tr ("Default"), true, current == 0, [set] { set (0); });
    colours.addSeparator();
    for (auto c : lynxPalette())
    {
        juce::Image swatch (juce::Image::ARGB, 16, 16, true);
        juce::Graphics (swatch).fillAll (juce::Colour (c));
        // (an Item made with its text gets the ID JUCE's own addItem uses; a default-made one has ID 0 and JUCE never
        // triggers it: the bug behind "Change color does nothing", issue #9)
        juce::PopupMenu::Item item (juce::String (themes::toHex (c)));
        item.image = std::make_unique<juce::DrawableImage> (swatch);
        item.isTicked = current == c;
        item.action = [set, c] { set (c); };
        colours.addItem (std::move (item));
    }
    colours.addSeparator();
    const auto custom = current != 0 && std::find (lynxPalette().begin(), lynxPalette().end(), current) == lynxPalette().end();
    colours.addItem (tr ("Custom color..."), true, custom, [set, current, screenArea] {
        showColourPickerPopup (screenArea, tr ("Custom color"), current != 0 ? juce::Colour (current) : juce::Colour (lynxPalette()[0]), {},
                               [set] (juce::Colour c) { set (c.getARGB()); });
    });
    return colours;
}
} // namespace lynx::ui
