#include "ui/HintBar.h"

#include <cmath>

#include "core/HintMarkup.h"
#include "ui/I18n.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

namespace lynx::ui
{
namespace
{
struct Posted
{
    Hint hint;
    double until = 0.0;
};

Posted& posted()
{
    static Posted p;
    return p;
}

// one drawable piece of a hint line
struct Item
{
    enum class Kind { word, mouse, key } kind = Kind::word;
    juce::String text;  // word, key name, or mouse token
    float gap = 0.0f;   // space before it
    float width = 0.0f;
    bool title = false; // the hint's subject ("Volume" in "Volume: drag...")
};

std::vector<Item> makeItems (const juce::String& text, const juce::Font& font, const juce::Font& keyFont, float glyph)
{
    std::vector<Item> items;
    const auto space = juce::GlyphArrangement::getStringWidth (font, " ");

    // the subject: the words before the first ": " or " - " (when it comes early, before any glyph)
    auto titleEnd = -1;
    {
        const auto colon = text.indexOf (": "), dash = text.indexOf (" - "), bracket = text.indexOf ("[");
        const auto first = colon < 0 ? dash : dash < 0 ? colon : juce::jmin (colon, dash);
        if (first > 0 && first < 48 && (bracket < 0 || first < bracket))
            titleEnd = first + (first == colon ? 1 : 0);
    }

    int consumed = 0; // characters of `text` already turned into items (for the title)
    float pendingGap = 0.0f;

    for (const auto& segment : hintmarkup::parse (text.toStdString()))
    {
        const auto value = juce::String::fromUTF8 (segment.value.c_str());

        if (segment.kind != hintmarkup::Segment::Kind::text)
        {
            Item item;
            item.kind = segment.kind == hintmarkup::Segment::Kind::mouse ? Item::Kind::mouse : Item::Kind::key;
            item.text = value;
            item.gap = pendingGap;
            item.width = item.kind == Item::Kind::mouse
                             ? glyph + (value == "2xLMB" ? juce::GlyphArrangement::getStringWidth (keyFont, "x2") + 1.0f : 0.0f)
                             : juce::GlyphArrangement::getStringWidth (keyFont, value) + 8.0f;
            items.push_back (item);
            pendingGap = 0.0f;
            consumed += value.length() + 2;
            continue;
        }

        // words, keeping how much space separates them (two spaces = a wider break between gestures)
        int i = 0;
        while (i < value.length())
        {
            int spaces = 0;
            while (i < value.length() && value[i] == ' ')
            {
                ++spaces;
                ++i;
            }

            pendingGap += spaces == 0 ? 0.0f : spaces == 1 ? space : space * 2.5f;
            const auto start = i;
            while (i < value.length() && value[i] != ' ')
                ++i;

            if (i > start)
            {
                Item item;
                item.text = value.substring (start, i);
                item.gap = pendingGap;
                item.width = juce::GlyphArrangement::getStringWidth (font, item.text);
                item.title = titleEnd > 0 && consumed + start < titleEnd;
                items.push_back (item);
                pendingGap = 0.0f;
            }
        }

        consumed += value.length();
    }

    return items;
}

// Splits the items into at most `maxLines` lines of `width`; false if they do not fit.
bool layOut (const std::vector<Item>& items, float width, int maxLines, std::vector<std::vector<Item>>& lines)
{
    lines.assign (1, {});
    float x = 0.0f;

    // a line only breaks after a word: key and mouse glyphs stay with what follows them ("[Ctrl][T] typing")
    for (std::size_t i = 0; i < items.size();)
    {
        auto end = i + 1;
        while (end < items.size() && items[end - 1].kind != Item::Kind::word)
            ++end;

        float chunkWidth = 0.0f;
        for (auto j = i; j < end; ++j)
            chunkWidth += (j == i ? 0.0f : items[j].gap) + items[j].width;

        const auto needed = (lines.back().empty() ? 0.0f : items[i].gap) + chunkWidth;

        if (! lines.back().empty() && x + needed > width)
        {
            if ((int) lines.size() == maxLines)
                return false;

            lines.emplace_back();
            x = 0.0f;
        }

        for (auto j = i; j < end; ++j)
        {
            auto item = items[j];
            if (lines.back().empty())
                item.gap = 0.0f;
            x += item.gap + item.width;
            lines.back().push_back (item);
        }

        i = end;
    }

    return x <= width;
}

void drawLine (juce::Graphics& g, const std::vector<Item>& line, juce::Rectangle<float> area, const juce::Font& font,
               const juce::Font& keyFont, float glyph)
{
    using namespace theme;
    auto x = area.getX();

    for (const auto& item : line)
    {
        x += item.gap;

        if (x >= area.getRight())
            break;

        const auto w = juce::jmin (item.width, area.getRight() - x);
        const auto box = juce::Rectangle<float> (x, area.getY(), w, area.getHeight());

        switch (item.kind)
        {
            case Item::Kind::word:
                g.setFont (font);
                g.setColour (item.title ? colour::textPrimary : colour::textSecondary);
                g.drawText (item.text, box, juce::Justification::centredLeft, true);
                break;

            case Item::Kind::mouse:
            {
                const auto icon = item.text == "RMB" ? Icon::rightClick : item.text == "MMB" ? Icon::mouseMiddle
                                                                          : item.text == "Wheel" ? Icon::mouseWheel
                                                                                                 : Icon::mouseLeft;
                icons::draw (g, icon, box.withWidth (glyph).withSizeKeepingCentre (glyph, glyph), colour::textPrimary);

                if (item.text == "2xLMB")
                {
                    g.setFont (keyFont);
                    g.setColour (colour::textPrimary);
                    g.drawText ("x2", box.withTrimmedLeft (glyph), juce::Justification::centredLeft, false);
                }
                break;
            }

            case Item::Kind::key:
            {
                const auto cap = box.withSizeKeepingCentre (box.getWidth(), juce::jmin (box.getHeight(), keyFont.getHeight() + 5.0f));
                g.setColour (colour::bg4);
                g.fillRoundedRectangle (cap, radius::sm);
                g.setColour (colour::borderStrong);
                g.drawRoundedRectangle (cap.reduced (0.5f), radius::sm, 1.0f);
                g.setColour (colour::textPrimary);
                g.setFont (keyFont);
                g.drawText (item.text, cap, juce::Justification::centred, false);
                break;
            }
        }

        x += item.width;
    }
}
} // namespace

void postHint (const Hint& hint, int holdMs)
{
    posted().hint = hint;
    posted().until = juce::Time::getMillisecondCounterHiRes() + holdMs;
}

void clearPostedHint()
{
    posted() = {};
}

std::optional<Hint> postedHint()
{
    if (posted().until > 0.0 && juce::Time::getMillisecondCounterHiRes() < posted().until && ! posted().hint.isEmpty())
        return posted().hint;

    return std::nullopt;
}

juce::String valueGestures()
{
    return tr ("[LMB] drag  [Ctrl] fine  [Wheel] step  [Alt][LMB] reset  [2xLMB] type  [RMB] menu");
}

juce::String plainHint (const juce::String& markup)
{
    const std::vector<std::string> words { tr ("click").toStdString(), tr ("double-click").toStdString(), tr ("right-click").toStdString(),
                                           tr ("middle-click").toStdString(), tr ("wheel").toStdString() };
    return juce::String::fromUTF8 (hintmarkup::plain (markup.toStdString(), words).c_str());
}

juce::String withMarkup (const juce::String& text)
{
    // the old gesture phrases still used by some panels (kept translated, so they match in both languages)
    static const char* const valuePhrases[] = { TR_NOOP ("drag (Ctrl: fine), wheel, Alt+click resets") };

    for (const auto* phrase : valuePhrases)
        if (const auto translated = tr (phrase); text.contains (translated))
            return text.replace (translated, valueGestures());

    return text;
}

void HintBar::setHint (const Hint& hint, bool isLive)
{
    if (hint.text != current.text || hint.value != current.value || hint.automatable != current.automatable || hint.hasMenu != current.hasMenu
        || isLive != live)
    {
        current = hint;
        live = isLive;
        repaint();
    }
}

void HintBar::paint (juce::Graphics& g)
{
    using namespace theme;
    auto r = getLocalBounds().toFloat().reduced (6.0f, 1.0f);

    // capability markers (dim when not applicable, so the text does not jump)
    if (current.automatable || current.hasMenu)
    {
        icons::draw (g, Icon::automate, r.removeFromLeft (16.0f).withSizeKeepingCentre (14.0f, 14.0f),
                     current.automatable ? colour::automation : colour::textDisabled.withAlpha (0.35f));
        r.removeFromLeft (2.0f);
        icons::draw (g, Icon::rightClick, r.removeFromLeft (16.0f).withSizeKeepingCentre (14.0f, 14.0f),
                     current.hasMenu ? colour::textSecondary : colour::textDisabled.withAlpha (0.35f));
        r.removeFromLeft (8.0f);
    }

    // the value, in a pill on the right (accent while it changes under the mouse)
    if (current.value.isNotEmpty())
    {
        const juce::Font valueFont (font (type::small, true));
        const auto w = juce::jmin (r.getWidth() / 3.0f, juce::GlyphArrangement::getStringWidth (valueFont, current.value) + 18.0f);
        const auto valueArea = r.removeFromRight (juce::jmax (44.0f, w)).reduced (0.0f, 3.0f);
        g.setColour (live ? colour::accentMuted : colour::bg3);
        g.fillRoundedRectangle (valueArea, radius::sm);
        if (live)
        {
            g.setColour (colour::accent);
            g.drawRoundedRectangle (valueArea.reduced (0.5f), radius::sm, 1.0f);
        }
        g.setColour (colour::textPrimary);
        g.setFont (valueFont);
        g.drawFittedText (current.value, valueArea.toNearestInt(), juce::Justification::centred, 1, 0.8f);
        r.removeFromRight (8.0f);
    }

    // the text: one line if it fits, else two smaller lines, else two lines cut at the end
    const auto text = withMarkup (current.text);
    std::vector<std::vector<Item>> lines;

    const juce::Font big (font (type::small)), bigKey (font (type::caption - 1.0f, true));
    if (auto items = makeItems (text, big, bigKey, 15.0f); layOut (items, r.getWidth(), 1, lines))
    {
        drawLine (g, lines.front(), r.withSizeKeepingCentre (r.getWidth(), 17.0f), big, bigKey, 15.0f);
        return;
    }

    const juce::Font smallFont (font (type::caption)), smallKey (font (type::caption - 2.0f, true));
    const auto items = makeItems (text, smallFont, smallKey, 12.0f);
    const auto fits = layOut (items, r.getWidth(), 2, lines);
    const auto lineHeight = juce::jmin (13.0f, r.getHeight() / 2.0f);
    auto area = r.withSizeKeepingCentre (r.getWidth(), lineHeight * 2.0f);

    for (std::size_t i = 0; i < lines.size() && i < 2; ++i)
        drawLine (g, lines[i], area.removeFromTop (lineHeight), smallFont, smallKey, 12.0f);

    if (! fits) // the rest did not fit: a fade at the end of the second line says so
    {
        const auto fade = r.withLeft (r.getRight() - 24.0f).withTrimmedTop (r.getHeight() / 2.0f);
        g.setGradientFill (juce::ColourGradient (colour::bg1.withAlpha (0.0f), fade.getX(), 0.0f, colour::bg1, fade.getRight(), 0.0f, false));
        g.fillRect (fade);
    }
}
// --- quick tips ----------------------------------------------------------------------------------

namespace
{
const juce::Identifier quickTipProperty ("lynxQuickTip");

juce::Font tipNameFont() { return juce::Font (theme::font (theme::type::caption)); }
juce::Font tipKeyFont() { return juce::Font (theme::font (theme::type::caption - 1.0f)); }
constexpr float tipPadX = 7.0f, tipGap = 8.0f;
constexpr int tipHeight = 20;
} // namespace

void setQuickTip (juce::Component& c, const juce::String& name, const juce::String& shortcut)
{
    c.getProperties().set (quickTipProperty, shortcut.trim().isEmpty() ? name : name + "\t" + shortcut.trim());
}

juce::String quickTipOf (const juce::Component& c)
{
    return c.getProperties()[quickTipProperty].toString();
}

juce::Rectangle<int> quickTipSize (const juce::String& tip)
{
    const auto name = tip.upToFirstOccurrenceOf ("\t", false, false), key = tip.fromFirstOccurrenceOf ("\t", false, false);
    auto w = juce::GlyphArrangement::getStringWidth (tipNameFont(), name) + 2.0f * tipPadX;
    if (key.isNotEmpty())
        w += tipGap + juce::GlyphArrangement::getStringWidth (tipKeyFont(), key);
    return { 0, 0, (int) std::ceil (w) + 2, tipHeight };
}

void paintQuickTip (juce::Graphics& g, juce::Rectangle<float> bubble, const juce::String& tip)
{
    using namespace theme;
    const auto name = tip.upToFirstOccurrenceOf ("\t", false, false), key = tip.fromFirstOccurrenceOf ("\t", false, false);
    g.setColour (colour::bg0.withAlpha (0.96f));
    g.fillRoundedRectangle (bubble, radius::sm);
    g.setColour (colour::borderStrong.withAlpha (0.9f));
    g.drawRoundedRectangle (bubble.reduced (0.5f), radius::sm, 1.0f);

    auto text = bubble.reduced (tipPadX, 0.0f);
    g.setColour (colour::textPrimary.withAlpha (0.92f));
    g.setFont (tipNameFont());
    const auto nameWidth = juce::GlyphArrangement::getStringWidth (tipNameFont(), name);
    g.drawText (name, text.removeFromLeft (nameWidth + 1.0f), juce::Justification::centredLeft, false);

    if (key.isNotEmpty())
    {
        text.removeFromLeft (tipGap);
        g.setColour (colour::textSecondary.withAlpha (0.85f));
        g.setFont (tipKeyFont());
        g.drawText (key, text, juce::Justification::centredLeft, false);
    }
}

juce::Rectangle<int> quickTipBounds (const juce::String& tip, juce::Rectangle<int> target, juce::Rectangle<int> area)
{
    auto r = quickTipSize (tip).withCentre ({ target.getCentreX(), 0 }).withY (target.getBottom() + 5);
    if (r.getBottom() > area.getBottom())
        r.setY (target.getY() - 5 - r.getHeight());
    return r.constrainedWithin (area);
}

struct QuickTipWindow::Look final : juce::LookAndFeel_V4
{
    juce::Rectangle<int> target; // screen bounds of the control the tip is for

    juce::Rectangle<int> getTooltipBounds (const juce::String& tip, juce::Point<int> pos, juce::Rectangle<int> parentArea) override
    {
        return quickTipBounds (tip, target.isEmpty() ? juce::Rectangle<int> (pos.x, pos.y, 1, 16) : target, parentArea);
    }

    void drawTooltip (juce::Graphics& g, const juce::String& tip, int width, int height) override
    {
        paintQuickTip (g, juce::Rectangle<int> (width, height).toFloat(), tip);
    }
};

QuickTipWindow::QuickTipWindow() : juce::TooltipWindow (nullptr, 400), look (std::make_unique<Look>())
{
    setOpaque (false);
    setLookAndFeel (look.get());
}

QuickTipWindow::~QuickTipWindow()
{
    hideTip();
    setLookAndFeel (nullptr);
}

juce::String QuickTipWindow::getTipFor (juce::Component& c)
{
    if (juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown() || c.isCurrentlyBlockedByAnotherModalComponent())
        return {};

    // the control itself or the component it is drawn in (a button's inner parts)
    auto* owner = &c;
    for (int depth = 0; owner != nullptr && depth < 3 && quickTipOf (*owner).isEmpty(); ++depth)
        owner = owner->getParentComponent();

    if (owner == nullptr || quickTipOf (*owner).isEmpty() || ! owner->isEnabled())
        return {};

    look->target = owner->getScreenBounds();
    return quickTipOf (*owner);
}

// --- last tweaked read-out (round 162d) -------------------------------------------------------------------------

TweakReadout::TweakReadout()
{
    setRepaintsOnMouseActivity (true);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip (tr ("Last tweaked parameter: click for its menu (automation clip, link to controller, events, value)"));
    setHint (*this, tr ("Last tweaked parameter: [LMB] its menu (create automation clip, link to controller, edit events, init song with this position, value)"),
             true, true);
}

void TweakReadout::setContent (const juce::String& newName, const juce::String& newValue)
{
    if (newName == name && newValue == value)
        return;
    if (newName == name && newValue != value)
    {
        changedMs = juce::Time::getMillisecondCounterHiRes();
        startTimer (950);
    }
    name = newName;
    value = newValue;
    repaint();
}

void TweakReadout::paint (juce::Graphics& g)
{
    using namespace theme;
    auto r = getLocalBounds().toFloat().reduced (0.0f, 1.0f);
    const auto hover = isMouseOver();
    fillRoundRect (g, r, radius::md, hover ? colour::bg4 : colour::bg1);
    if (hover)
    {
        g.setColour (colour::borderStrong);
        g.drawRoundedRectangle (r.reduced (0.5f), radius::md, 1.0f);
    }
    r = r.reduced (6.0f, 0.0f);
    icons::draw (g, Icon::automate, r.removeFromLeft (16.0f).withSizeKeepingCentre (14.0f, 14.0f),
                 name.isNotEmpty() ? colour::automation : colour::textDisabled);
    r.removeFromLeft (5.0f);

    if (name.isEmpty())
    {
        g.setFont (font (type::small));
        g.setColour (colour::textDisabled);
        g.drawFittedText (tr ("Last tweaked: none"), r.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
        return;
    }

    const auto fresh = juce::Time::getMillisecondCounterHiRes() - changedMs < 900.0;
    if (value.isNotEmpty())
    {
        const juce::Font valueFont (font (type::small, true));
        const auto w = juce::jmin (r.getWidth() * 0.45f, juce::GlyphArrangement::getStringWidth (valueFont, value) + 14.0f);
        const auto valueArea = r.removeFromRight (juce::jmax (36.0f, w)).reduced (0.0f, 3.0f);
        g.setColour (fresh ? colour::accentMuted : colour::bg3);
        g.fillRoundedRectangle (valueArea, radius::sm);
        g.setColour (colour::textPrimary);
        g.setFont (valueFont);
        g.drawFittedText (value, valueArea.toNearestInt(), juce::Justification::centred, 1, 0.75f);
        r.removeFromRight (6.0f);
    }
    g.setFont (font (type::small));
    g.setColour (hover ? colour::textPrimary : colour::textSecondary);
    g.drawFittedText (name, r.toNearestInt(), juce::Justification::centredLeft, 1, 0.75f);
}

void TweakReadout::mouseDown (const juce::MouseEvent&)
{
    if (onClick)
        onClick();
}

} // namespace lynx::ui
