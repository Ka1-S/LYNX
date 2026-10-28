#include "ui/ValueControl.h"

#include <cmath>

#include "core/ValueMath.h"
#include "ui/AutomationUi.h"
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/WindowOwner.h"

namespace lynx::ui
{
namespace
{
constexpr const char* hintProperty = "lynxHint";
constexpr const char* hintMenuProperty = "lynxHintMenu";
constexpr const char* hintAutoProperty = "lynxHintAuto";
constexpr const char* ownContractProperty = "lynxOwnContract";
constexpr const char* configuredProperty = "lynxContract";
constexpr const char* sensitivityProperty = "lynxDragPixels";

juce::String number (double v, int decimals)
{
    auto s = juce::String (v, decimals);
    if (s.containsChar ('.'))
        s = s.trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
    return s == "-0" ? juce::String ("0") : s;
}

std::optional<double> leadingNumber (const juce::String& typed)
{
    const auto t = typed.trim().replaceCharacter (',', '.');
    const auto digits = t.initialSectionContainingOnly ("+-0123456789.");

    if (digits.isEmpty() || digits.containsOnly ("+-."))
        return std::nullopt;

    return digits.getDoubleValue();
}
} // namespace

// --- specs -------------------------------------------------------------------------------------

juce::String ValueSpec::text (double value) const
{
    if (toText)
        return toText (value);

    return number (value, interval >= 1.0 ? 0 : 3);
}

std::optional<double> ValueSpec::parse (const juce::String& typed) const
{
    auto value = fromText ? fromText (typed) : leadingNumber (typed);

    if (! value || ! std::isfinite (*value))
        return std::nullopt;

    auto v = juce::jlimit (minimum, maximum, *value);
    return controls::snapToInterval (v, minimum, interval);
}

juce::String utilityVolumeText (double volume)
{
    const auto v = controls::clampUtilityVolume (volume);
    const auto gain = controls::utilityGain (v);
    const auto db = gain > 0.0f ? juce::String (20.0 * std::log10 ((double) gain), 1) + " dB" : juce::String ("-inf dB");
    return juce::String (juce::roundToInt (v * 100.0)) + "% (" + (gain > 1.0f ? "+" : "") + db + ")";
}

ValueSpec percentSpec (const juce::String& name, double defaultValue)
{
    ValueSpec s;
    s.name = name;
    s.defaultValue = defaultValue;
    s.toText = [] (double v) { return number (v * 100.0, 0) + "%"; };
    s.fromText = [] (const juce::String& t) -> std::optional<double> {
        if (const auto v = leadingNumber (t))
            return *v / 100.0;
        return std::nullopt;
    };
    return s;
}

ValueSpec panSpec (const juce::String& name)
{
    ValueSpec s;
    s.name = name;
    s.minimum = -1.0;
    s.maximum = 1.0;
    s.defaultValue = 0.0;
    s.toText = [] (double v) {
        const auto percent = juce::roundToInt (std::abs (v) * 100.0);
        return percent == 0 ? tr ("Centered") : (v < 0 ? tr ("{0}% left", { juce::String (percent) }) : tr ("{0}% right", { juce::String (percent) }));
    };
    // typed: -50 / 50, or "50 L" / "50 R"
    s.fromText = [] (const juce::String& t) -> std::optional<double> {
        const auto lower = t.toLowerCase();
        auto v = leadingNumber (lower);

        if (! v)
        {
            if (lower.trim().startsWith ("c"))
                return 0.0;
            v = leadingNumber (lower.retainCharacters ("0123456789.,"));
        }

        if (! v)
            return std::nullopt;

        auto pan = *v / 100.0;
        const auto trimmed = lower.trim();
        if (trimmed.contains ("left") || trimmed.contains ("esq") || trimmed.startsWithChar ('l') || trimmed.endsWithChar ('l')) // "50 L", "50% left", "esquerda"
            pan = -std::abs (pan);
        return pan;
    };
    return s;
}

ValueSpec faderSpec (const juce::String& name, double defaultValue)
{
    ValueSpec s;
    s.name = name;
    s.defaultValue = defaultValue;
    s.pixelsForRange = 200.0;
    s.toText = [] (double v) {
        const auto gain = faderGain ((float) v);
        return gain <= 1.0e-5f ? juce::String ("-inf dB") : juce::String (juce::Decibels::gainToDecibels (gain), 1) + " dB";
    };
    s.fromText = [] (const juce::String& t) -> std::optional<double> {
        if (t.trim().startsWithIgnoreCase ("-inf"))
            return 0.0;

        const auto db = leadingNumber (t);
        if (! db)
            return std::nullopt;

        // invert the fader curve (monotonic) by bisection
        const auto target = juce::Decibels::decibelsToGain (*db, -200.0);
        double lo = 0.0, hi = 1.0;
        for (int i = 0; i < 40; ++i)
        {
            const auto mid = (lo + hi) / 2.0;
            (faderGain ((float) mid) < target ? lo : hi) = mid;
        }
        return (lo + hi) / 2.0;
    };
    return s;
}

std::function<std::optional<double> (const juce::String&)> inverseOf (std::function<juce::String (double)> toText, double minimum,
                                                                      double maximum)
{
    return [toText, minimum, maximum] (const juce::String& typed) -> std::optional<double> {
        const auto wanted = leadingNumber (typed);

        if (! wanted)
            return std::nullopt;

        std::optional<double> best;
        double bestDistance = 0.0;
        constexpr int steps = 2000;

        for (int i = 0; i <= steps; ++i)
        {
            const auto v = minimum + (maximum - minimum) * i / steps;
            if (const auto shown = leadingNumber (toText (v)))
                if (const auto d = std::abs (*shown - *wanted); ! best || d < bestDistance)
                {
                    best = v;
                    bestDistance = d;
                }
        }

        return best;
    };
}

// --- gestures ----------------------------------------------------------------------------------

void ValueDrag::begin (const juce::MouseEvent& e, double value)
{
    startValue = value;
    anchorY = e.position.y;
    fine = e.mods.isCtrlDown();
    active = true;
}

std::function<void (const AutomationTarget&)> onTweak;

void reportTweak (const ValueSpec& spec)
{
    if (spec.target && onTweak)
        onTweak (*spec.target);
}

double ValueDrag::update (const juce::MouseEvent& e, const ValueSpec& spec)
{
    reportTweak (spec);
    const auto nowFine = e.mods.isCtrlDown();

    if (nowFine != fine) // switching mid-drag: continue from where the value is now
    {
        startValue = controls::dragValue (startValue, anchorY - e.position.y, spec.minimum, spec.maximum, spec.pixelsForRange, fine);
        anchorY = e.position.y;
        fine = nowFine;
    }

    const auto v = controls::dragValue (startValue, anchorY - e.position.y, spec.minimum, spec.maximum, spec.pixelsForRange, fine);
    return controls::snapToInterval (v, spec.minimum, spec.interval);
}

bool isResetClick (const juce::MouseEvent& e) noexcept
{
    return e.mods.isMiddleButtonDown() || (e.mods.isLeftButtonDown() && e.mods.isAltDown() && ! e.mods.isCtrlDown());
}

std::optional<double> wheelValue (double value, const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel, const ValueSpec& spec)
{
    const auto delta = std::abs (wheel.deltaY) >= std::abs (wheel.deltaX) ? wheel.deltaY : -wheel.deltaX;

    if (delta == 0.0f)
        return std::nullopt;

    const auto notches = (wheel.isReversed ? -1.0 : 1.0) * (delta > 0.0f ? 1.0 : -1.0) * juce::jmax (1.0, std::abs (delta) / 0.12);
    reportTweak (spec);
    return controls::wheelValue (value, notches, spec.minimum, spec.maximum, e.mods.isCtrlDown(), spec.interval);
}

// --- menu --------------------------------------------------------------------------------------

void addValueItems (juce::PopupMenu& menu, juce::Component& target, AudioEngine* engine, const ValueSpec& spec, double current,
                    std::function<void (double)> set)
{
    // in FL's order (REA round 149 [EXEC]: FL's parameter menu): Reset, Automation, Remote control, Value
    juce::Component::SafePointer<juce::Component> safe (&target);
    menu.addItem (tr ("Reset ({0})", { spec.text (spec.defaultValue) }), [spec, set] { set (spec.defaultValue); });

    if (engine != nullptr && spec.target)
        addAutomationItem (menu, *engine, *spec.target, true);
    else
        menu.addSeparator();

    // FL's Copy value / Paste value: the position within the control's range, so it pastes into another kind of control
    static double copied = -1.0;
    const auto range = spec.maximum - spec.minimum;
    menu.addSectionHeader (tr ("Value"));
    menu.addItem (tr ("Copy value"), range > 0.0, false, [spec, current, range] { copied = (current - spec.minimum) / range; });
    menu.addItem (tr ("Paste value"), copied >= 0.0 && range > 0.0, false, [spec, set, range] {
        set (juce::jlimit (spec.minimum, spec.maximum, spec.minimum + copied * range));
    });
    menu.addItem (tr ("Type in value..."), [safe, spec, current, set] {
        if (safe != nullptr)
            typeValue (*safe, spec, current, set);
    });
}

void showValueMenu (juce::Component& target, AudioEngine* engine, const ValueSpec& spec, double current, std::function<void (double)> set)
{
    juce::PopupMenu menu;

    if (spec.name.isNotEmpty())
        menu.addSectionHeader (spec.name + ": " + spec.text (current));

    addValueItems (menu, target, engine, spec, current, std::move (set));
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target).withMousePosition());
}

void typeValue (juce::Component& target, const ValueSpec& spec, double current, std::function<void (double)> set)
{
    juce::ignoreUnused (target);
    auto* window = new juce::AlertWindow (tr ("Type value"), spec.name, juce::MessageBoxIconType::NoIcon);
    window->addTextEditor ("value", spec.text (current));
    window->addButton (tr ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));

    if (auto* editor = window->getTextEditor ("value"))
        editor->selectAll();

    window->enterModalState (true, juce::ModalCallbackFunction::create ([window, spec, set] (int result) {
                                 if (result != 1)
                                     return;

                                 if (const auto v = spec.parse (window->getTextEditorContents ("value")))
                                     set (*v);
                             }),
                             true);
    keepAboveMainWindow (*window);
}

// --- value read-out ------------------------------------------------------------------------------

namespace
{
class ValueTip final : public juce::Component,
                       private juce::Timer
{
public:
    ValueTip()
    {
        setInterceptsMouseClicks (false, false);
        setAlwaysOnTop (true);
        setOpaque (false);
    }

    void show (juce::Component& over, juce::Rectangle<int> control, const juce::String& newText, bool transient)
    {
        auto* top = over.getTopLevelComponent();

        if (top == nullptr || ! over.isShowing())
            return;

        if (getParentComponent() != top)
            top->addChildComponent (this);

        text = newText;
        const auto f = juce::Font (theme::font (theme::type::small, true));
        const auto width = juce::GlyphArrangement::getStringWidthInt (f, text) + 22;
        const auto r = top->getLocalArea (&over, control);
        auto b = juce::Rectangle<int> (width, 23).withCentre ({ r.getCentreX(), r.getY() - 14 });

        if (b.getY() < 2) // no room above: below the control
            b.setY (r.getBottom() + 3);

        setBounds (b.constrainedWithin (top->getLocalBounds().reduced (2)));
        toFront (false);
        setVisible (true);
        repaint();

        if (transient)
            startTimer (900);
        else
            stopTimer();
    }

    void hide()
    {
        stopTimer();
        setVisible (false);
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        // a small glassy pill with an accent edge (round 122: the value pop-up while dragging)
        const auto r = getLocalBounds().toFloat().reduced (1.0f, 1.5f);
        const auto corner = r.getHeight() * 0.5f;
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillRoundedRectangle (r.translated (0.0f, 1.5f), corner);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff30333c), 0.0f, r.getY(), juce::Colour (0xff1e2026), 0.0f, r.getBottom(), false));
        g.fillRoundedRectangle (r, corner);
        g.setColour (colour::accent.withAlpha (0.75f));
        g.drawRoundedRectangle (r.reduced (0.5f), corner, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.10f));
        g.drawHorizontalLine ((int) r.getY() + 1, r.getX() + corner, r.getRight() - corner);
        g.setColour (colour::textPrimary);
        g.setFont (font (type::small, true));
        g.drawText (text, getLocalBounds(), juce::Justification::centred, false);
    }

private:
    void timerCallback() override { hide(); }

    juce::String text;
};

// Owns the tip; deleted by JUCE before the message loop goes away.
struct ValueTipHolder final : juce::DeletedAtShutdown
{
    ValueTip tip;
    static ValueTipHolder*& instance()
    {
        static ValueTipHolder* holder = nullptr;
        return holder;
    }
    ~ValueTipHolder() override { instance() = nullptr; }
};

ValueTip& valueTip()
{
    auto*& holder = ValueTipHolder::instance();
    if (holder == nullptr)
        holder = new ValueTipHolder();
    return holder->tip;
}
} // namespace

void showValueTip (juce::Component& over, juce::Rectangle<int> control, const juce::String& text)
{
    valueTip().show (over, control, text, false);
}

void flashValueTip (juce::Component& over, juce::Rectangle<int> control, const juce::String& text)
{
    valueTip().show (over, control, text, true);
}

void hideValueTip()
{
    if (auto* holder = ValueTipHolder::instance())
        holder->tip.hide();
}

// --- hints -------------------------------------------------------------------------------------

void drawFittingText (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, juce::Justification just, juce::Font font,
                      float minHeight, float room, float minScale)
{
    room = juce::jmax (area.getWidth(), room);
    auto width = juce::GlyphArrangement::getStringWidth (font, text);
    for (auto h = font.getHeight(); width > room && h > minHeight;)
    {
        h = juce::jmax (minHeight, h - 0.5f);
        font = font.withHeight (h);
        width = juce::GlyphArrangement::getStringWidth (font, text);
    }
    g.setFont (font);
    auto box = area;
    if (width > area.getWidth() && room > area.getWidth())
    {
        const auto w = juce::jmin (room, width + 2.0f);
        box = just.testFlags (juce::Justification::left)    ? area.withWidth (w)
              : just.testFlags (juce::Justification::right) ? area.withLeft (area.getRight() - w)
                                                            : area.withSizeKeepingCentre (w, area.getHeight());
    }
    g.drawFittedText (text, box.toNearestInt(), just, 1, minScale);
}

void setHint (juce::Component& c, const juce::String& text, bool hasMenu, bool automatable)
{
    c.getProperties().set (hintProperty, text);
    c.getProperties().set (hintMenuProperty, hasMenu);
    c.getProperties().set (hintAutoProperty, automatable);
}

Hint valueHint (const ValueSpec& spec, double value, const juce::String& gesture)
{
    Hint h;
    h.text = spec.name + (gesture.isNotEmpty() ? " - " + gesture : juce::String());
    h.value = spec.text (value);
    h.automatable = spec.target.has_value();
    h.hasMenu = true;
    return h;
}

namespace
{
juce::String sliderName (juce::Slider& s)
{
    if (s.getTooltip().isNotEmpty())
        return s.getTooltip().upToFirstOccurrenceOf ("\n", false, false);

    // a label attached to the slider, or the closest label above it
    if (auto* parent = s.getParentComponent())
    {
        juce::Label* best = nullptr;
        int bestDistance = 60;

        for (auto* child : parent->getChildren())
            if (auto* label = dynamic_cast<juce::Label*> (child))
            {
                if (label->getAttachedComponent() == &s)
                    return label->getText();

                const auto d = std::abs (label->getBounds().getCentreX() - s.getBounds().getCentreX())
                               + std::abs (label->getBottom() - s.getY());
                if (label->isVisible() && label->getBottom() <= s.getY() + 4 && d < bestDistance)
                {
                    best = label;
                    bestDistance = d;
                }
            }

        if (best != nullptr)
            return best->getText();
    }

    return s.getName().isNotEmpty() ? s.getName() : tr ("Knob");
}

ValueSpec sliderSpec (juce::Slider& s)
{
    ValueSpec spec;
    spec.name = sliderName (s);
    spec.minimum = s.getMinimum();
    spec.maximum = s.getMaximum();
    spec.interval = s.getInterval();
    spec.defaultValue = s.isDoubleClickReturnEnabled() ? s.getDoubleClickReturnValue() : s.getMinimum();
    juce::Component::SafePointer<juce::Slider> safe (&s);
    spec.toText = [safe] (double v) { return safe != nullptr ? safe->getTextFromValue (v) : juce::String (v); };
    spec.fromText = [safe] (const juce::String& t) -> std::optional<double> {
        if (safe == nullptr || ! leadingNumber (t))
            return std::nullopt;
        return safe->getValueFromText (t);
    };
    return spec;
}

juce::Slider* sliderOf (juce::Component* c)
{
    if (auto* s = dynamic_cast<juce::Slider*> (c))
        return s;

    return c != nullptr ? c->findParentComponentOfClass<juce::Slider>() : nullptr;
}

bool takesContract (juce::Slider* s)
{
    return s != nullptr && ! s->getProperties().contains (ownContractProperty) && s->getSliderStyle() != juce::Slider::IncDecButtons
           && s->getSliderStyle() != juce::Slider::TwoValueHorizontal && s->getSliderStyle() != juce::Slider::TwoValueVertical
           && s->getSliderStyle() != juce::Slider::ThreeValueHorizontal && s->getSliderStyle() != juce::Slider::ThreeValueVertical;
}

void configure (juce::Slider& s)
{
    if (s.getProperties().contains (configuredProperty))
        return;

    s.getProperties().set (configuredProperty, true);
    s.getProperties().set (sensitivityProperty, s.getMouseDragSensitivity());
    s.setVelocityModeParameters (1.0, 1, 0.0, false); // Ctrl means "fine", not "velocity mode"
    s.setScrollWheelEnabled (false);                  // the wheel is handled below (small steps, Ctrl finer)
    s.setPopupMenuEnabled (false);
}

// sliders that show no value of their own get the floating read-out
bool wantsValueTip (juce::Slider& s)
{
    return s.getTextBoxPosition() == juce::Slider::NoTextBox;
}

void setFine (juce::Slider& s, bool fine)
{
    const auto base = (int) s.getProperties().getWithDefault (sensitivityProperty, 250);
    const auto wanted = fine ? (int) (base / controls::fineFactor) : base;

    if (s.getMouseDragSensitivity() != wanted)
        s.setMouseDragSensitivity (wanted);
}

struct SliderContract final : juce::MouseListener
{
    void mouseMove (const juce::MouseEvent& e) override
    {
        if (auto* s = sliderOf (e.eventComponent); takesContract (s))
            configure (*s);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto* s = sliderOf (e.eventComponent);

        if (! takesContract (s) || ! s->isEnabled())
            return;

        configure (*s);
        setFine (*s, e.mods.isCtrlDown());
        juce::Component::SafePointer<juce::Slider> safe (s);
        auto set = [safe] (double v) {
            if (safe != nullptr)
                safe->setValue (v, juce::sendNotificationSync);
        };

        if (e.mods.isPopupMenu())
            showValueMenu (*s, nullptr, sliderSpec (*s), s->getValue(), set);
        else if (e.mods.isMiddleButtonDown())
            set (sliderSpec (*s).defaultValue);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (auto* s = sliderOf (e.eventComponent); takesContract (s))
        {
            setFine (*s, e.mods.isCtrlDown());
            if (wantsValueTip (*s) && ! e.mods.isPopupMenu())
                showValueTip (*s, s->getLocalBounds(), s->getTextFromValue (s->getValue()));
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (auto* s = sliderOf (e.eventComponent); takesContract (s))
        {
            setFine (*s, false);
            if (wantsValueTip (*s))
                hideValueTip();
        }
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        auto* s = sliderOf (e.eventComponent);

        if (! takesContract (s) || ! s->isEnabled() || ! s->getProperties().contains (configuredProperty))
            return;

        if (const auto v = wheelValue (s->getValue(), e, wheel, sliderSpec (*s)))
        {
            s->setValue (*v, juce::sendNotificationSync);
            if (wantsValueTip (*s))
                flashValueTip (*s, s->getLocalBounds(), s->getTextFromValue (s->getValue()));
        }
    }
};

std::unique_ptr<SliderContract>& contract()
{
    static std::unique_ptr<SliderContract> instance;
    return instance;
}
} // namespace

Hint hintFor (juce::Component* component, juce::Point<int> position)
{
    for (auto* c = component; c != nullptr; c = c->getParentComponent())
    {
        if (auto* source = dynamic_cast<HintSource*> (c))
        {
            auto h = source->hintAt (position);
            if (! h.isEmpty())
                return h;
        }

        if (auto* s = dynamic_cast<juce::Slider*> (c))
        {
            Hint h;
            h.text = c->getProperties().contains (hintProperty) ? c->getProperties()[hintProperty].toString() : sliderName (*s);
            h.value = s->getTextFromValue (s->getValue());
            h.hasMenu = true;
            h.automatable = (bool) c->getProperties().getWithDefault (hintAutoProperty, false);
            return h;
        }

        if (c->getProperties().contains (hintProperty))
        {
            Hint h;
            h.text = c->getProperties()[hintProperty].toString();
            h.hasMenu = c->getProperties()[hintMenuProperty];
            h.automatable = c->getProperties()[hintAutoProperty];
            return h;
        }

        if (auto* client = dynamic_cast<juce::TooltipClient*> (c))
            if (const auto tip = client->getTooltip(); tip.isNotEmpty())
            {
                Hint h;
                h.text = tip.replace ("\n", "   ");
                return h;
            }

        if (auto* parent = c->getParentComponent())
            position = parent->getLocalPoint (c, position);
    }

    return {};
}

void installSliderContract()
{
    if (contract() == nullptr)
    {
        contract() = std::make_unique<SliderContract>();
        juce::Desktop::getInstance().addGlobalMouseListener (contract().get());
    }
}

void uninstallSliderContract()
{
    if (contract() != nullptr)
    {
        juce::Desktop::getInstance().removeGlobalMouseListener (contract().get());
        contract().reset();
    }
}
namespace
{
class ItemHeader final : public juce::PopupMenu::CustomComponent
{
public:
    ItemHeader (juce::String k, juce::String n, juce::Colour c) : juce::PopupMenu::CustomComponent (false), kind (std::move (k)), name (std::move (n)), colour (c) {}

    void getIdealSize (int& width, int& height) override
    {
        const juce::Font big (theme::font (theme::type::body, true)), small (theme::font (theme::type::caption, true));
        width = juce::jmax (160, juce::jmax (juce::GlyphArrangement::getStringWidthInt (big, name), juce::GlyphArrangement::getStringWidthInt (small, kind)) + 32);
        height = kind.isNotEmpty() ? 40 : 28;
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        auto r = getLocalBounds().toFloat().reduced (4.0f, 3.0f);
        const auto base = colour.isTransparent() ? colour::bg5 : tame (colour);
        g.setColour (base.withAlpha (0.9f));
        g.fillRoundedRectangle (r, radius::md);
        g.setColour (base.brighter (0.25f));
        g.fillRoundedRectangle (r.removeFromLeft (4.0f), 2.0f);
        const auto text = base.getPerceivedBrightness() > 0.6f ? colour::bg0 : colour::textPrimary;
        auto area = getLocalBounds().reduced (14, 4);
        if (kind.isNotEmpty())
        {
            g.setFont (font (type::caption - 1.0f, true));
            g.setColour (text.withAlpha (0.7f));
            g.drawText (caps (kind), area.removeFromTop (13), juce::Justification::bottomLeft, true);
        }
        g.setFont (font (type::body, true));
        g.setColour (text);
        g.drawText (name, area, juce::Justification::centredLeft, true);
    }

private:
    juce::String kind, name;
    juce::Colour colour;
};
} // namespace

void addItemHeader (juce::PopupMenu& menu, const juce::String& kind, const juce::String& name, juce::Colour colour)
{
    juce::PopupMenu::Item item (name);
    item.customComponent = new ItemHeader (kind, name, colour);
    item.isEnabled = false;
    menu.addItem (std::move (item));
}
} // namespace lynx::ui
