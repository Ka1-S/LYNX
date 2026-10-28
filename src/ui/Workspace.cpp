#include "ui/Workspace.h"
#include "ui/ThemeSettings.h"

#include "engine/PluginHost.h"
#include "ui/EditorTools.h"
#include "ui/HintBar.h"
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/ValueControl.h"
#include "ui/WindowOwner.h"

namespace lynx
{
using workspace::Panel;

namespace
{
constexpr int edge = 4; // frame border that resizes the panel

juce::Rectangle<int> toJuce (const workspace::Rect& r)
{
    return { r.x, r.y, r.w, r.h };
}

workspace::Rect toRect (juce::Rectangle<int> r)
{
    return { r.getX(), r.getY(), r.getWidth(), r.getHeight() };
}

juce::File stateFile()
{
    return PluginHost::settingsFolder().getChildFile ("workspace.txt");
}
} // namespace

// --- a panel: title strip + content ------------------------------------------------------------

class PanelFrame final : public juce::Component,
                         public ui::HintSource
{
public:
    PanelFrame (Workspace& w, Panel p) : owner (w), panel (p)
    {
        for (auto* b : { &menuButton, &dockButton, &maximiseButton, &closeButton })
        {
            b->setFlat (true);
            b->setIconSizing (0.62f, 14.0f); // fine glyphs, like the toolbar
            b->setWantsKeyboardFocus (false);
            b->setMouseClickGrabsKeyboardFocus (false);
            addAndMakeVisible (b);
        }

        addAndMakeVisible (titleLabel);
        menuButton.onClick = [this] { showMenu(); };
        menuButton.setTooltip (tr ("Window options: detach, maximize, close and this panel's own settings"));
        dockButton.onClick = [this] { owner.setDetached (panel, false); };
        dockButton.setTooltip (tr ("Dock the panel back into the main window"));
        maximiseButton.onClick = [this] { owner.toggleMaximised (panel); };
        closeButton.onClick = [this] { owner.hide (panel); };
        closeButton.setTooltip (tr ("Close the panel (its F-key shows it again)"));
        ui::setQuickTip (menuButton, tr ("Window options"));
        ui::setQuickTip (dockButton, tr ("Dock"));
        ui::setQuickTip (closeButton, tr ("Close"));
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
        // paints every pixel: the panels it covers (a maximised Mixer over the Playlist, a floating Piano roll)
        // are not painted under it (round 151: the hidden Playlist cost 13-19 ms a frame behind the Mixer)
        setOpaque (true);
        refreshButtons();
    }

    ~PanelFrame() override
    {
        if (content != nullptr)
            content->removeMouseListener (this);
    }

    void setContent (juce::Component& c, const juce::String& t, Icon i)
    {
        content = &c;
        sharer = dynamic_cast<SharesTitleBar*> (&c);
        title = t;
        icon = i;
        addAndMakeVisible (c);
        c.addMouseListener (this, true); // a click anywhere in the panel activates it
        raiseTitleBar();
        resized();
    }

    void setTitle (const juce::String& t)
    {
        if (title != t)
        {
            title = t;
            resized();
            titleLabel.repaint();
        }
    }

    void setTitleTooltip (const juce::String& t) { titleTooltip = t; }

    void setAccessory (juce::Component* c, int width)
    {
        if (accessory != nullptr)
            removeChildComponent (accessory);

        accessory = c;
        accessoryWidth = width;

        if (accessory != nullptr)
            addAndMakeVisible (accessory);

        raiseTitleBar();
        resized();
    }

    void setFocused (bool f)
    {
        if (focused != f)
        {
            focused = f;
            repaint();
            titleLabel.repaint();
        }
    }

    void refreshButtons()
    {
        const auto detached = owner.isDetached (panel);
        dockButton.setVisible (detached);
        maximiseButton.setVisible (! detached);
        maximiseButton.setIcon (owner.isMaximised (panel) ? Icon::restore : Icon::maximise);
        maximiseButton.setTooltip (owner.isMaximised (panel) ? tr ("Restore the panel size (double-click the title)")
                                                              : tr ("Maximize the panel (double-click the title)"));
        ui::setQuickTip (maximiseButton, owner.isMaximised (panel) ? tr ("Restore") : tr ("Maximize"));
        resized();
    }

    void showMenu()
    {
        juce::PopupMenu menu;
        menu.addSectionHeader (title);

        if (owner.onPanelMenu)
        {
            juce::PopupMenu own;
            owner.onPanelMenu (panel, own);

            if (own.getNumItems() > 0)
            {
                for (juce::PopupMenu::MenuItemIterator it (own); it.next();)
                    menu.addItem (it.getItem());
                menu.addSeparator();
            }
        }

        const auto detached = owner.isDetached (panel);
        menu.addItem (tr ("Detached (own window)"), true, detached, [this, detached] { owner.setDetached (panel, ! detached); });
        menu.addItem (tr ("Maximized"), ! detached, owner.isMaximised (panel), [this] { owner.toggleMaximised (panel); });
        menu.addItem (tr ("Close"), [this] { owner.hide (panel); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuButton));
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        g.fillAll (colour::bg2);
        auto strip = getLocalBounds().removeFromTop (Workspace::titleHeight);
        g.setColour (colour::bg3);
        g.fillRect (strip);
        g.setColour (colour::border);
        g.fillRect (strip.removeFromBottom (1));
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        using namespace theme;

        if (focused) // the focused panel gets the accent along its title
        {
            g.setColour (colour::accent);
            g.fillRect (0, 0, getWidth(), 2);
        }

        if (! owner.isDetached (panel))
        {
            g.setColour (focused ? colour::borderStrong.brighter (0.15f) : colour::borderStrong);
            g.drawRect (getLocalBounds(), 1);
        }
    }

    // the width the menu button and the title take at the left of the title bar
    int titleWidth() const
    {
        const auto text = juce::roundToInt (juce::GlyphArrangement::getStringWidth (juce::Font (theme::font (theme::type::body, true)), title));
        return 4 + menuSize + 6 + text + 12 + (accessory != nullptr ? accessoryWidth + 8 : 0);
    }

    void resized() override
    {
        auto strip = getLocalBounds().removeFromTop (Workspace::titleHeight).reduced (4, 3);
        const auto buttonsRight = strip.getRight();
        closeButton.setBounds (strip.removeFromRight (strip.getHeight()));
        strip.removeFromRight (2);

        if (maximiseButton.isVisible())
        {
            maximiseButton.setBounds (strip.removeFromRight (strip.getHeight()));
            strip.removeFromRight (2);
        }

        if (dockButton.isVisible())
            dockButton.setBounds (strip.removeFromRight (strip.getHeight()));

        const auto rightInset = getWidth() - (strip.getRight() - 4);
        juce::ignoreUnused (buttonsRight);

        menuButton.setBounds (4, 3, menuSize, Workspace::titleHeight - 6);
        const auto labelWidth = titleWidth() - (accessory != nullptr ? accessoryWidth + 8 : 0);
        titleLabel.setBounds (0, 0, labelWidth, Workspace::titleHeight - 1);

        if (accessory != nullptr)
            accessory->setBounds (labelWidth, 1, accessoryWidth, Workspace::titleHeight - 2);

        if (content != nullptr)
        {
            const auto inset = owner.isDetached (panel) || owner.isMaximised (panel) ? 0 : edge;

            if (sharer != nullptr)
            {
                // the editor's toolbar runs along the title row, between the title and the buttons
                content->setBounds (getLocalBounds().withTrimmedTop (1).reduced (inset, 0).withTrimmedBottom (inset));
                sharer->setTitleBarInsets (juce::jmax (0, titleWidth() - inset), juce::jmax (0, rightInset - inset));
            }
            else
                content->setBounds (getLocalBounds().withTrimmedTop (Workspace::titleHeight).reduced (inset, 0).withTrimmedBottom (inset));
        }
    }

    // --- mouse: activation, title drag, edge resize -----------------------------------------------

    int edgesAt (juce::Point<int> p) const
    {
        if (owner.isDetached (panel) || owner.isMaximised (panel))
            return 0;

        int edges = 0;
        if (p.x < edge)
            edges |= workspace::left;
        if (p.x >= getWidth() - edge)
            edges |= workspace::right;
        if (p.y >= getHeight() - edge)
            edges |= workspace::bottom;
        if (p.y < 3)
            edges |= workspace::top;
        return edges;
    }

    // True for a press on the title bar: the frame's own strip, or (for an editor sharing it) the bare
    // background of the editor's toolbar row.
    bool isOnTitleBar (const juce::MouseEvent& e)
    {
        if (e.eventComponent == this || e.eventComponent == &titleLabel)
            return e.getEventRelativeTo (this).y < Workspace::titleHeight;

        if (sharer == nullptr || content == nullptr || e.eventComponent == nullptr)
            return false;

        // the editor itself (not one of its controls)
        const auto bare = e.eventComponent == content || dynamic_cast<SharesTitleBar*> (e.eventComponent) != nullptr;
        return bare && e.getEventRelativeTo (this).y < Workspace::titleHeight;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        if (e.eventComponent != this)
            return;

        const auto edges = edgesAt (e.getPosition());
        using C = juce::MouseCursor;
        const auto horizontal = (edges & (workspace::left | workspace::right)) != 0;
        const auto vertical = (edges & (workspace::top | workspace::bottom)) != 0;

        if (horizontal && vertical)
            setMouseCursor ((edges & workspace::left) != 0 ? ((edges & workspace::top) ? C::TopLeftCornerResizeCursor : C::BottomLeftCornerResizeCursor)
                                                           : ((edges & workspace::top) ? C::TopRightCornerResizeCursor : C::BottomRightCornerResizeCursor));
        else if (horizontal)
            setMouseCursor (C::LeftRightResizeCursor);
        else if (vertical)
            setMouseCursor (C::UpDownResizeCursor);
        else
            setMouseCursor (C::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        owner.activate (panel);
        dragging = false;
        const auto onTitle = isOnTitleBar (e);

        if (e.eventComponent != this && ! onTitle)
            return;

        if (e.mods.isPopupMenu() && onTitle)
        {
            if (owner.onTitleRightClick)
                owner.onTitleRightClick (panel, e);
            return;
        }

        dragEdges = e.eventComponent == this ? edgesAt (e.getPosition()) : 0;
        dragging = dragEdges != 0 || (onTitle && ! owner.isDetached (panel) && ! owner.isMaximised (panel));
        startBounds = getBounds();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging || getParentComponent() == nullptr)
            return;

        const auto delta = e.getEventRelativeTo (getParentComponent()).getOffsetFromDragStart();
        auto r = startBounds;

        if (dragEdges == 0)
            r = r.translated (delta.x, delta.y);
        else
        {
            if (dragEdges & workspace::left)
                r.setLeft (juce::jmin (r.getRight() - workspace::minWidth, r.getX() + delta.x));
            if (dragEdges & workspace::right)
                r.setRight (juce::jmax (r.getX() + workspace::minWidth, r.getRight() + delta.x));
            if (dragEdges & workspace::top)
                r.setTop (juce::jmin (r.getBottom() - workspace::minHeight, r.getY() + delta.y));
            if (dragEdges & workspace::bottom)
                r.setBottom (juce::jmax (r.getY() + workspace::minHeight, r.getBottom() + delta.y));
        }

        // keep the title strip reachable
        const auto parent = getParentComponent()->getLocalBounds();
        r.setY (juce::jlimit (0, juce::jmax (0, parent.getHeight() - Workspace::titleHeight), r.getY()));

        if (! e.mods.isAltDown()) // Alt: no snapping
        {
            std::vector<workspace::Rect> others;
            for (const auto& o : owner.otherPanelBounds (panel))
                others.push_back (toRect (o));
            r = toJuce (workspace::snap (toRect (r), others, parent.getWidth(), parent.getHeight(), 8, dragEdges));
        }

        setBounds (r);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging)
            owner.frameMoved (panel, getBounds());

        dragging = false;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (isOnTitleBar (e) && ! owner.isDetached (panel))
            owner.toggleMaximised (panel);
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (isOnTitleBar (e) && owner.onTitleWheel)
            owner.onTitleWheel (panel, e, wheel);
    }

    ui::Hint hintAt (juce::Point<int> p) override
    {
        ui::Hint h;

        if (edgesAt (p) != 0)
            h.text = tr ("Drag the edge to resize the panel");
        else if (p.y < Workspace::titleHeight)
            h.text = title + ": " + (titleTooltip.isNotEmpty() ? titleTooltip + "   " : juce::String())
                     + (owner.isDetached (panel) ? tr ("Docks back with the button on the right")
                                                 : tr ("Drag to move (Alt: no snapping), double-click to maximize"));
        return h;
    }

    juce::String getTitle() const { return title; }
    juce::String getTitleTooltip() const { return titleTooltip; }
    bool isFocusedPanel() const { return focused; }

private:
    // the title text (a child, so it stays visible over an editor sharing the title row)
    struct TitleLabel final : juce::Component, ui::HintSource
    {
        explicit TitleLabel (PanelFrame& f) : frame (f) { setInterceptsMouseClicks (false, false); }

        void paint (juce::Graphics& g) override
        {
            using namespace theme;
            g.setColour (colour::bg3);
            g.fillRect (getLocalBounds().withTrimmedTop (2));
            g.setColour (frame.isFocusedPanel() ? colour::textPrimary : colour::textSecondary);
            g.setFont (font (type::body, true));
            g.drawFittedText (frame.getTitle(), getLocalBounds().withTrimmedLeft (4 + menuSize + 6).withTrimmedRight (8),
                              juce::Justification::centredLeft, 1, 0.9f);
        }

        ui::Hint hintAt (juce::Point<int> p) override { return frame.hintAt (p); }

        PanelFrame& frame;
    };

    void raiseTitleBar()
    {
        titleLabel.toFront (false);

        for (auto* c : { (juce::Component*) accessory, (juce::Component*) &menuButton, (juce::Component*) &dockButton,
                         (juce::Component*) &maximiseButton, (juce::Component*) &closeButton })
            if (c != nullptr)
                c->toFront (false);
    }

    static constexpr int menuSize = 22;

    Workspace& owner;
    Panel panel;
    juce::Component* content = nullptr;
    SharesTitleBar* sharer = nullptr;
    juce::Component* accessory = nullptr;
    int accessoryWidth = 0;
    juce::String title, titleTooltip;
    Icon icon = Icon::none;
    bool focused = false, dragging = false;
    int dragEdges = 0;
    juce::Rectangle<int> startBounds;
    TitleLabel titleLabel { *this };
    IconButton menuButton { "menu", Icon::panelMenu }, dockButton { "dock", Icon::dock }, maximiseButton { "maximise", Icon::maximise },
        closeButton { "close", Icon::close };
};

// --- a detached panel's native window -----------------------------------------------------------

// Detached windows snap to the screen edges, the main window and the other detached panels while they are
// moved or resized (Alt held: no snapping). The native title bar's moves come through the constrainer.
class SnapConstrainer final : public juce::ComponentBoundsConstrainer
{
public:
    std::function<std::vector<juce::Rectangle<int>>()> others;

    void checkBounds (juce::Rectangle<int>& bounds, const juce::Rectangle<int>& previous, const juce::Rectangle<int>& limits,
                      bool stretchingTop, bool stretchingLeft, bool stretchingBottom, bool stretchingRight) override
    {
        juce::ComponentBoundsConstrainer::checkBounds (bounds, previous, limits, stretchingTop, stretchingLeft, stretchingBottom, stretchingRight);

        if (juce::ModifierKeys::getCurrentModifiersRealtime().isAltDown() || previous.isEmpty())
            return;

        const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (bounds);

        if (display == nullptr)
            return;

        const auto area = display->userArea;
        auto local = [&area] (juce::Rectangle<int> r) { return workspace::Rect { r.getX() - area.getX(), r.getY() - area.getY(), r.getWidth(), r.getHeight() }; };
        std::vector<workspace::Rect> rects;

        if (others)
            for (const auto& o : others())
                rects.push_back (local (o));

        const auto edges = (stretchingLeft ? workspace::left : 0) | (stretchingTop ? workspace::top : 0) | (stretchingRight ? workspace::right : 0)
                           | (stretchingBottom ? workspace::bottom : 0);

        if (edges == 0 && bounds.getWidth() != previous.getWidth()) // a resize the window manager reports without edges
            return;

        const auto r = workspace::snap (local (bounds), rects, area.getWidth(), area.getHeight(), 10, edges);
        bounds = { r.x + area.getX(), r.y + area.getY(), r.w, r.h };
    }
};

class DetachedPanelWindow final : public juce::DocumentWindow
{
public:
    DetachedPanelWindow (Workspace& w, Panel p, PanelFrame& frame, juce::Rectangle<int> screenBounds)
        : juce::DocumentWindow ("LYNX - " + frame.getTitle(), theme::colour::bg2, DocumentWindow::closeButton | DocumentWindow::maximiseButton),
          owner (w),
          panel (p)
    {
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        snapping.setSizeLimits (workspace::minWidth, workspace::minHeight + Workspace::titleHeight, 10000, 10000);
        snapping.others = [this] { return owner.otherWindowScreenBounds (panel); };
        setConstrainer (&snapping);
        setContentNonOwned (&frame, false);

        if (screenBounds.isEmpty() || juce::Desktop::getInstance().getDisplays().getDisplayForRect (screenBounds) == nullptr)
            centreWithSize (juce::jmax (500, frame.getWidth()), juce::jmax (300, frame.getHeight()));
        else
            setBounds (screenBounds);

        getProperties().set ("lynxOwnKeys", true); // (its keyPressed hands every key to the main window itself)
        setVisible (true);
        keepAboveMainWindow (*this);
    }

    void closeButtonPressed() override { owner.hide (panel); }

    bool keyPressed (const juce::KeyPress& key) override
    {
        return owner.onKeyInDetachedWindow != nullptr && owner.onKeyInDetachedWindow (key);
    }

    void moved() override
    {
        juce::DocumentWindow::moved();
        owner.detachedWindowMoved (panel, getBounds());
    }

    void resized() override
    {
        juce::DocumentWindow::resized();
        owner.detachedWindowMoved (panel, getBounds());
    }

    void activeWindowStatusChanged() override
    {
        if (isActiveWindow())
            owner.activate (panel);
    }

private:
    Workspace& owner;
    Panel panel;
    SnapConstrainer snapping;
};

// --- the desktop -------------------------------------------------------------------------------

Workspace::Workspace()
{
    for (int i = 0; i < workspace::panelCount; ++i)
    {
        frames[(std::size_t) i] = std::make_unique<PanelFrame> (*this, (Panel) i);
        addChildComponent (*frames[(std::size_t) i]);
    }

    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
}

Workspace::~Workspace()
{
    saveState();

    for (auto& w : windows)
        if (w != nullptr)
            w->clearContentComponent();

    for (auto& w : windows)
        w.reset();
}

void Workspace::setContent (Panel panel, juce::Component& content, const juce::String& title, Icon icon)
{
    frames[(std::size_t) panel]->setContent (content, title, icon);
}

void Workspace::setTitle (Panel panel, const juce::String& title)
{
    frames[(std::size_t) panel]->setTitle (title);

    if (auto& w = windows[(std::size_t) panel])
        w->setName ("LYNX - " + title);
}

void Workspace::setTitleTooltip (Panel panel, const juce::String& tooltip)
{
    frames[(std::size_t) panel]->setTitleTooltip (tooltip);
}

void Workspace::setTitleAccessory (Panel panel, juce::Component* accessory, int width)
{
    frames[(std::size_t) panel]->setAccessory (accessory, width);
}

bool Workspace::isPanelVisible (Panel panel) const { return layout[panel].visible; }
bool Workspace::isMaximised (Panel panel) const { return layout[panel].maximised; }
bool Workspace::isDetached (Panel panel) const { return layout[panel].detached; }

bool Workspace::toggle (Panel panel)
{
    const auto visible = layout.toggle (panel);
    applyLayout();
    return visible;
}

void Workspace::show (Panel panel)
{
    layout.show (panel);
    applyLayout();
}

void Workspace::hide (Panel panel)
{
    layout.hide (panel);
    applyLayout();
}

void Workspace::toggleMaximised (Panel panel)
{
    layout.toggleMaximised (panel);
    applyLayout();
}

void Workspace::setDetached (Panel panel, bool detached)
{
    pristine = false;
    layout[panel].detached = detached;

    if (detached)
    {
        layout[panel].maximised = false;

        if (layout[panel].windowBounds.w <= 0 && isShowing()) // first time: where the panel is, on screen
        {
            const auto screen = localAreaToGlobal (frames[(std::size_t) panel]->getBounds());
            layout[panel].windowBounds = toRect (screen);
        }
    }

    layout.show (panel);
    applyLayout();
}

void Workspace::activate (Panel panel)
{
    if (layout.focused == panel && (layout[panel].detached || layout.isFrontmost (panel)))
        return;

    layout.bringToFront (panel);

    if (! layout[panel].detached && ! isBackgroundPanel (panel))
        frames[(std::size_t) panel]->toFront (false);

    setFocusedPanel (panel);
}

void Workspace::fitPanelContent (Panel panel, int contentWidth, int contentHeight)
{
    auto& state = layout[panel];

    if (! layoutReady || state.maximised || state.detached)
        return;

    auto r = state.bounds;
    if (contentWidth > 0)
        r.w = contentWidth + 2 * edge;
    if (contentHeight > 0)
        r.h = contentHeight + titleHeight + edge;

    // grows to the left (or up) rather than past the desktop's edge
    r.x = juce::jlimit (0, juce::jmax (0, getWidth() - r.w), r.x);
    r.y = juce::jlimit (0, juce::jmax (0, getHeight() - r.h), r.y);

    r.w = juce::jlimit (workspace::minWidth, juce::jmax (workspace::minWidth, getWidth() - r.x), r.w);
    r.h = juce::jlimit (workspace::minHeight, juce::jmax (workspace::minHeight, getHeight() - r.y), r.h);
    state.bounds = r;
    applyLayout();
}

void Workspace::setFocusedPanel (Panel panel)
{
    const auto changed = layout.focused != panel;
    layout.focused = panel;

    for (int i = 0; i < workspace::panelCount; ++i)
        frames[(std::size_t) i]->setFocused ((Panel) i == panel);

    if (changed && onFocusChanged)
        onFocusChanged (panel);
}

void Workspace::frameMoved (Panel panel, juce::Rectangle<int> bounds)
{
    if (! (layout[panel].bounds == toRect (bounds)))
        pristine = false; // moved or resized by hand

    layout[panel].bounds = toRect (bounds);
}

std::vector<juce::Rectangle<int>> Workspace::otherWindowScreenBounds (Panel panel) const
{
    std::vector<juce::Rectangle<int>> out;

    if (auto* top = getTopLevelComponent(); top != nullptr && top->isShowing())
        out.push_back (top->getScreenBounds());

    for (int i = 0; i < workspace::panelCount; ++i)
        if ((Panel) i != panel && windows[(std::size_t) i] != nullptr && windows[(std::size_t) i]->isVisible())
            out.push_back (windows[(std::size_t) i]->getBounds());

    return out;
}

void Workspace::detachedWindowMoved (Panel panel, juce::Rectangle<int> screenBounds)
{
    if (layout[panel].detached)
        layout[panel].windowBounds = toRect (screenBounds);
}

std::vector<juce::Rectangle<int>> Workspace::otherPanelBounds (Panel panel) const
{
    std::vector<juce::Rectangle<int>> out;

    for (int i = 0; i < workspace::panelCount; ++i)
        if ((Panel) i != panel && layout[(Panel) i].visible && ! layout[(Panel) i].detached && ! layout[(Panel) i].maximised)
            out.push_back (frames[(std::size_t) i]->getBounds());

    return out;
}

bool Workspace::isDetachedWindowActive() const
{
    for (const auto& w : windows)
        if (w != nullptr && w->isVisible() && w->isActiveWindow())
            return true;

    return false;
}

void Workspace::applyLayout()
{
    if (! layoutReady)
        return;

    for (int i = 0; i < workspace::panelCount; ++i)
    {
        const auto panel = (Panel) i;
        const auto& state = layout[panel];
        auto& frame = *frames[(std::size_t) i];
        auto& window = windows[(std::size_t) i];

        if (state.detached)
        {
            if (window == nullptr)
            {
                removeChildComponent (&frame);
                frame.setVisible (true);
                frame.setSize (state.bounds.w, state.bounds.h);
                window = std::make_unique<DetachedPanelWindow> (*this, panel, frame, toJuce (state.windowBounds));
            }

            if (window->isVisible() != state.visible)
                window->setVisible (state.visible);

            if (state.visible && layout.focused == panel)
                window->toFront (false);
        }
        else
        {
            if (window != nullptr)
            {
                window->clearContentComponent();
                window.reset();
                addChildComponent (frame);
            }

            frame.setBounds (state.maximised ? getLocalBounds() : toJuce (state.bounds));
            frame.setVisible (state.visible);
        }

        frame.refreshButtons();
    }

    // stacking order (back to front): the background panels first, then the floating ones
    for (auto background : { true, false })
        for (auto panel : layout.order)
            if (! layout[panel].detached && isBackgroundPanel (panel) == background)
                frames[(std::size_t) panel]->toFront (false);

    setFocusedPanel (layout.focused);
    repaint();
}

void Workspace::resized()
{
    if (getWidth() <= 0 || getHeight() <= 0)
        return;

    if (! layoutReady)
    {
        layout = workspace::Layout::makeDefault (getWidth(), getHeight());

        bool restored = false;

        if (pendingState.isNotEmpty())
            if (auto saved = workspace::Layout::parse (pendingState.toStdString(), getWidth(), getHeight()))
            {
                layout = *saved;
                restored = true;
            }

        layoutReady = true;
        pristine = ! restored;
        applyLayout();

        if (! restored && onDefaultLayout)
            onDefaultLayout();
        return;
    }

    if (pristine)
    {
        layout.relayoutDefault (getWidth(), getHeight());
        applyLayout();

        if (onDefaultLayout)
            onDefaultLayout();
        return;
    }

    layout.resize (getWidth(), getHeight());
    applyLayout();
}

void Workspace::paint (juce::Graphics& g)
{
    using namespace theme;
    themeui::paintWorkspaceBackdrop (g, getLocalBounds()); // the theme's colour and image (Options › Theme, round 162d)

    // an empty desktop says how to bring the panels back
    bool any = false;
    for (int i = 0; i < workspace::panelCount; ++i)
        any = any || (layout[(Panel) i].visible && ! layout[(Panel) i].detached);

    if (! any)
    {
        g.setColour (colour::textDisabled);
        g.setFont (font (type::body));
        g.drawFittedText (tr ("F5 Playlist   F6 Channel Rack   F7 Piano Roll   F9 Mixer   Alt+F8 Browser"), getLocalBounds().reduced (20),
                          juce::Justification::centred, 2);
    }
}

void Workspace::resetLayout()
{
    layout = workspace::Layout::makeDefault (juce::jmax (1, getWidth()), juce::jmax (1, getHeight()));
    layoutReady = true;
    pristine = true;
    applyLayout();

    if (onDefaultLayout)
        onDefaultLayout();
}

int Workspace::bringBackOffscreen()
{
    const auto moved = layout.bringBackOffscreen();
    applyLayout();
    return moved;
}

juce::String Workspace::getLayoutText() const
{
    return juce::String (layout.serialise());
}

bool Workspace::applyLayoutText (const juce::String& text)
{
    if (! layoutReady)
    {
        if (! workspace::Layout::parse (text.toStdString(), 1000, 1000))
            return false;

        pendingState = text;
        return true;
    }

    const auto parsed = workspace::Layout::parse (text.toStdString(), getWidth(), getHeight());

    if (! parsed)
        return false;

    layout = *parsed;
    pristine = false;
    applyLayout();
    return true;
}

juce::File Workspace::layoutsFolder()
{
    return PluginHost::settingsFolder().getChildFile ("layouts");
}

bool Workspace::saveLayoutAs (const juce::String& name)
{
    const auto folder = layoutsFolder();
    folder.createDirectory();
    return folder.getChildFile (juce::File::createLegalFileName (name) + ".txt").replaceWithText (getLayoutText());
}

bool Workspace::loadLayout (const juce::File& file)
{
    return applyLayoutText (file.loadFileAsString());
}

void Workspace::setPersistent (bool shouldPersist)
{
    persistent = shouldPersist;

    if (persistent && ! layoutReady)
        pendingState = stateFile().loadFileAsString();
}

void Workspace::saveState()
{
    if (persistent && layoutReady)
        stateFile().replaceWithText (getLayoutText());
}
} // namespace lynx
