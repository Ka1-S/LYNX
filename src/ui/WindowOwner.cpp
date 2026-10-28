#include "ui/WindowOwner.h"

#include "core/WindowFit.h"

#include "ui/LogoData.h"
#include "ui/Theme.h"
#include "platform/Paths.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ctime>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace lynx
{
namespace
{
juce::Component::SafePointer<juce::Component>& mainWindow()
{
    static juce::Component::SafePointer<juce::Component> window;
    return window;
}
} // namespace

void* mainWindowHandle()
{
    auto* main = mainWindow().getComponent();
    return main != nullptr && main->getPeer() != nullptr ? main->getWindowHandle() : nullptr;
}

void setMainWindowForOwnership (juce::Component* window)
{
    mainWindow() = window;
}

void activateMainWindow()
{
    auto* main = mainWindow().getComponent();
    if (main == nullptr || ! main->isShowing())
        return;
    auto* top = main->getTopLevelComponent();
    if (auto* peer = top->getPeer(); peer != nullptr && ! peer->isMinimised())
        top->toFront (true);
}

double processCpuSeconds()
{
   #if JUCE_WINDOWS
    FILETIME created {}, exited {}, kernel {}, user {};
    if (GetProcessTimes (GetCurrentProcess(), &created, &exited, &kernel, &user))
        return (double) ((((std::uint64_t) kernel.dwHighDateTime << 32) | kernel.dwLowDateTime) + (((std::uint64_t) user.dwHighDateTime << 32) | user.dwLowDateTime)) * 1e-7;
    return 0.0;
   #else
    return (double) std::clock() / CLOCKS_PER_SEC;
   #endif
}

juce::TextEditor* firstTextField (juce::Component& dialog)
{
    juce::TextEditor* best = nullptr;
    juce::Point<int> bestAt;
    std::function<void (juce::Component&)> walk = [&] (juce::Component& c) {
        for (auto* child : c.getChildren())
        {
            if (! child->isVisible() || ! child->isEnabled())
                continue;
            if (auto* editor = dynamic_cast<juce::TextEditor*> (child); editor != nullptr && ! editor->isReadOnly() && editor->getWantsKeyboardFocus())
            {
                const auto at = dialog.getLocalPoint (editor, juce::Point<int>());
                if (best == nullptr || at.y < bestAt.y || (at.y == bestAt.y && at.x < bestAt.x))
                {
                    best = editor;
                    bestAt = at;
                }
                continue;
            }
            walk (*child);
        }
    };
    walk (dialog);
    return best;
}

namespace
{
struct DialogTextFocus final : juce::FocusChangeListener
{
    void globalFocusChanged (juce::Component* focused) override
    {
        if (focused == nullptr)
            return;
        auto* dialog = dynamic_cast<juce::AlertWindow*> (focused->getTopLevelComponent());
        if (dialog == nullptr || ! dialog->isShowing())
            return;
        auto& props = dialog->getProperties();
        static const juce::Identifier done ("lynxTextFocused");
        if (props.contains (done)) // only the dialog's first focus: Tab and clicks move it freely afterwards
            return;
        props.set (done, true);
        if (dynamic_cast<juce::TextEditor*> (focused) != nullptr)
            return;
        if (auto* field = firstTextField (*dialog))
            field->grabKeyboardFocus(); // (AlertWindow's fields select all their text when focused)
    }
};

std::unique_ptr<DialogTextFocus>& dialogTextFocus()
{
    static std::unique_ptr<DialogTextFocus> listener;
    return listener;
}
} // namespace

void installDialogTextFocus()
{
    if (dialogTextFocus() != nullptr)
        return;
    dialogTextFocus() = std::make_unique<DialogTextFocus>();
    juce::Desktop::getInstance().addFocusChangeListener (dialogTextFocus().get());
}

void removeDialogTextFocus()
{
    if (dialogTextFocus() == nullptr)
        return;
    juce::Desktop::getInstance().removeFocusChangeListener (dialogTextFocus().get());
    dialogTextFocus() = nullptr;
}

juce::Image logoImage()
{
    return juce::ImageCache::getFromMemory (logo::png256, (int) sizeof (logo::png256));
}

void applyWindowChrome (juce::Component& window)
{
   #if JUCE_WINDOWS
    // dark native title bar (Windows 10 1809+), tinted with the LYNX background on Windows 11
    auto* hwnd = (HWND) window.getWindowHandle();
    if (hwnd == nullptr)
        return;

    using SetAttribute = HRESULT (WINAPI*) (HWND, DWORD, LPCVOID, DWORD);
    static const auto setAttribute = [] {
        auto* dwm = LoadLibraryW (L"dwmapi.dll");
        return dwm != nullptr ? (SetAttribute) (void*) GetProcAddress (dwm, "DwmSetWindowAttribute") : nullptr;
    }();

    if (setAttribute == nullptr)
        return;

    const BOOL dark = TRUE;
    if (FAILED (setAttribute (hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof (dark))))
        setAttribute (hwnd, 19 /* same attribute before Windows 10 20H1 */, &dark, sizeof (dark));

    auto colorRef = [] (juce::Colour c) { return (DWORD) (c.getRed() | (c.getGreen() << 8) | (c.getBlue() << 16)); };
    const auto caption = colorRef (theme::colour::bg1);
    const auto text = colorRef (theme::colour::textPrimary);
    const auto border = colorRef (theme::colour::border);
    setAttribute (hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof (caption));
    setAttribute (hwnd, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof (text));
    setAttribute (hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof (border));
   #else
    juce::ignoreUnused (window);
   #endif
}

namespace
{
void applyOwnership (juce::Component& window)
{
    applyWindowChrome (window);

    if (auto* top = dynamic_cast<juce::TopLevelWindow*> (&window))
        if (auto* peer = top->getPeer())
            peer->setIcon (logoImage());
   #if JUCE_WINDOWS
    auto* owner = mainWindow().getComponent();
    if (owner == nullptr || owner == &window || window.getWindowHandle() == nullptr || owner->getWindowHandle() == nullptr)
        return;
    // "owned" window: Windows keeps it above its owner and minimizes it along with it
    SetWindowLongPtrW ((HWND) window.getWindowHandle(), GWLP_HWNDPARENT, (LONG_PTR) owner->getWindowHandle());
   #endif
}

// JUCE recreates a window's native peer when its style changes (setResizable, setUsingNativeTitleBar...
// after it is on screen). The new native window has no owner, so it fell behind the main window
// (issue #4: the Audio editor opened behind LYNX). This re-applies the ownership to every new peer.
class OwnershipKeeper final : public juce::ComponentListener
{
public:
    explicit OwnershipKeeper (juce::Component& w) : window (&w) { w.addComponentListener (this); }
    ~OwnershipKeeper() override
    {
        if (window != nullptr)
            window->removeComponentListener (this);
    }

    void componentParentHierarchyChanged (juce::Component& c) override
    {
        if (c.getPeer() != nullptr) // (also called while the old peer is still there: harmless)
            applyOwnership (c);
    }

    void componentBeingDeleted (juce::Component& c) override;

    juce::Component* window;
};

std::map<juce::Component*, std::unique_ptr<OwnershipKeeper>>& keepers()
{
    static std::map<juce::Component*, std::unique_ptr<OwnershipKeeper>> all;
    return all;
}

void OwnershipKeeper::componentBeingDeleted (juce::Component& c)
{
    c.removeComponentListener (this);
    window = nullptr;
    keepers().erase (&c); // deletes this
}
} // namespace

bool isOwnedByMainWindow (juce::Component& window)
{
   #if JUCE_WINDOWS
    auto* owner = mainWindow().getComponent();
    return owner != nullptr && window.getWindowHandle() != nullptr && owner->getWindowHandle() != nullptr
           && GetWindow ((HWND) window.getWindowHandle(), GW_OWNER) == (HWND) owner->getWindowHandle();
   #else
    return keepers().count (&window) > 0;
   #endif
}

bool isAboveMainWindow (juce::Component& window)
{
   #if JUCE_WINDOWS
    auto* owner = mainWindow().getComponent();
    if (owner == nullptr || window.getWindowHandle() == nullptr || owner->getWindowHandle() == nullptr)
        return false;
    for (auto h = GetWindow ((HWND) window.getWindowHandle(), GW_HWNDNEXT); h != nullptr; h = GetWindow (h, GW_HWNDNEXT))
        if (h == (HWND) owner->getWindowHandle())
            return true;
    return false;
   #else
    return isOwnedByMainWindow (window);
   #endif
}

void forceForegroundForTest (juce::Component& window)
{
   #if JUCE_WINDOWS
    // a process started in the background may not take the foreground: join the input of the foreground window's
    // thread for a moment (what UI automation tools do)
    auto* hwnd = (HWND) window.getWindowHandle();
    auto* foreground = GetForegroundWindow();
    if (hwnd == nullptr || foreground == hwnd)
        return;
    const auto theirs = GetWindowThreadProcessId (foreground, nullptr), ours = GetCurrentThreadId();
    AttachThreadInput (theirs, ours, TRUE);
    SetForegroundWindow (hwnd);
    SetFocus (hwnd);
    AttachThreadInput (theirs, ours, FALSE);
   #else
    window.toFront (true);
   #endif
}

namespace
{
int gpuSetting = -1; // −1 not read yet

void applyToPeer (juce::ComponentPeer& peer, bool gpu)
{
   #if JUCE_WINDOWS
    const auto engines = peer.getAvailableRenderingEngines();
    if (engines.size() < 2)
        return;
    const auto wanted = gpu ? engines.size() - 1 : 0; // JUCE 8: 0 = GDI ("Software Renderer"), last = Direct2D
    if (peer.getCurrentRenderingEngine() != wanted)
        peer.setCurrentRenderingEngine (wanted);
   #else
    (void) peer;
    (void) gpu;
   #endif
}

struct RenderingWatch final : juce::Timer
{
    void timerCallback() override
    {
        if (gpuRenderingEnabled())
            return; // (JUCE's default: nothing to change)
        for (int i = juce::ComponentPeer::getNumPeers(); --i >= 0;)
            if (auto* peer = juce::ComponentPeer::getPeer (i))
                applyToPeer (*peer, false);
    }
};
} // namespace

bool gpuRenderingEnabled()
{
    if (gpuSetting < 0)
        gpuSetting = platform::settingsFile ("gpu-rendering.txt").loadFileAsString().trim() == "0" ? 0 : 1;
    return gpuSetting != 0;
}

void setGpuRenderingEnabled (bool on)
{
    gpuSetting = on ? 1 : 0;
    platform::settingsFile ("gpu-rendering.txt").replaceWithText (on ? "1" : "0");
    for (int i = juce::ComponentPeer::getNumPeers(); --i >= 0;)
        if (auto* peer = juce::ComponentPeer::getPeer (i))
        {
            applyToPeer (*peer, on && ! peer->getComponent().getProperties()["lynxGdi"]); // (plugin windows stay on GDI)
            peer->getComponent().repaint();
        }
}

void startRenderingWatch()
{
   #if JUCE_WINDOWS
    static RenderingWatch watch;
    watch.startTimer (500);
   #endif
}

void applyRenderingChoice (juce::Component& window)
{
    if (! gpuRenderingEnabled())
        if (auto* peer = window.getPeer())
            applyToPeer (*peer, false);
}

namespace
{
struct SecondaryKeyForwarder final : juce::KeyListener
{
    std::function<bool (const juce::KeyPress&)> handler;

    bool keyPressed (const juce::KeyPress& key, juce::Component* window) override
    {
        if (handler == nullptr || key == juce::KeyPress::escapeKey)
            return false;
        if (window != nullptr && (bool) window->getProperties().getWithDefault ("lynxOwnKeys", false))
            return false;
        return handler (key);
    }
};

SecondaryKeyForwarder& keyForwarder()
{
    static SecondaryKeyForwarder forwarder;
    return forwarder;
}
} // namespace

#if JUCE_WINDOWS
// A hosted plugin's own view (VST3 / VST2 / CLAP) gets the keys itself: with the focus in its native child window the
// keys never reach JUCE, and with the focus on JUCE's wrapper the wrapper swallows them all. FL still runs its
// shortcuts there (Space plays), so a thread keyboard hook hands such a key to the main window's global actions when
// it is one, before the plugin sees it (round 165q / issue #12: Space did nothing over Pro-Q 4). Plain letters and
// digits stay the plugin's (its own keys and text fields), and so does everything typed into a native Edit control.
namespace
{
int juceKeyCodeOf (WPARAM vk)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return (int) vk;
    if (vk >= VK_F1 && vk <= VK_F12)
        return juce::KeyPress::F1Key + (int) (vk - VK_F1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return juce::KeyPress::numberPad0 + (int) (vk - VK_NUMPAD0);
    switch (vk)
    {
        case VK_SPACE: return juce::KeyPress::spaceKey;
        case VK_RETURN: return juce::KeyPress::returnKey;
        case VK_TAB: return juce::KeyPress::tabKey;
        case VK_DELETE: return juce::KeyPress::deleteKey;
        case VK_BACK: return juce::KeyPress::backspaceKey;
        case VK_INSERT: return juce::KeyPress::insertKey;
        case VK_UP: return juce::KeyPress::upKey;
        case VK_DOWN: return juce::KeyPress::downKey;
        case VK_LEFT: return juce::KeyPress::leftKey;
        case VK_RIGHT: return juce::KeyPress::rightKey;
        case VK_HOME: return juce::KeyPress::homeKey;
        case VK_END: return juce::KeyPress::endKey;
        case VK_PRIOR: return juce::KeyPress::pageUpKey;
        case VK_NEXT: return juce::KeyPress::pageDownKey;
        case VK_ADD: return juce::KeyPress::numberPadAdd;
        case VK_SUBTRACT: return juce::KeyPress::numberPadSubtract;
        default: return 0;
    }
}

// The focus is in a hosted plugin's view inside one of LYNX's secondary windows (not the main window)
bool focusIsInPluginView()
{
    const auto focus = GetFocus();
    if (focus == nullptr)
        return false;
    const auto root = GetAncestor (focus, GA_ROOT);
    auto* main = mainWindow().getComponent();
    if (root == nullptr || (main != nullptr && main->getWindowHandle() == (void*) root))
        return false;
    juce::Component* window = nullptr;
    for (int i = 0; i < juce::ComponentPeer::getNumPeers(); ++i)
        if (auto* peer = juce::ComponentPeer::getPeer (i); peer != nullptr && peer->getNativeHandle() == (void*) root)
            window = &peer->getComponent();
    if (window == nullptr)
        return false; // (not a LYNX window)
    if (focus != root)
    {
        // a native child: the plugin's view, unless it is a native text field
        wchar_t cls[64] = {};
        GetClassNameW (focus, cls, 63);
        const juce::String name (cls);
        return ! name.containsIgnoreCase ("edit");
    }
    // JUCE's own focus: a hosted plugin's editor wrapper (it swallows every key)
    auto* focused = juce::Component::getCurrentlyFocusedComponent();
    auto* editor = focused != nullptr ? (dynamic_cast<juce::AudioProcessorEditor*> (focused) != nullptr ? dynamic_cast<juce::AudioProcessorEditor*> (focused)
                                                                                                      : focused->findParentComponentOfClass<juce::AudioProcessorEditor>())
                                      : nullptr;
    return editor != nullptr && dynamic_cast<juce::AudioPluginInstance*> (editor->getAudioProcessor()) != nullptr;
}

HHOOK pluginKeyHook = nullptr;

LRESULT CALLBACK pluginKeyProc (int code, WPARAM vk, LPARAM info)
{
    const auto down = (info & (1u << 31)) == 0, repeat = (info & (1 << 30)) != 0;
    if (code == HC_ACTION && down && ! repeat && keyForwarder().handler != nullptr && vk != VK_ESCAPE)
        if (const auto keyCode = juceKeyCodeOf (vk); keyCode != 0)
        {
            int mods = 0;
            if (GetKeyState (VK_CONTROL) & 0x8000) mods |= juce::ModifierKeys::ctrlModifier;
            if (GetKeyState (VK_SHIFT) & 0x8000) mods |= juce::ModifierKeys::shiftModifier;
            if (GetKeyState (VK_MENU) & 0x8000) mods |= juce::ModifierKeys::altModifier;
            const auto plainCharacter = ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
                                        && (mods & (juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::altModifier)) == 0;
            if (! plainCharacter && focusIsInPluginView() && keyForwarder().handler (juce::KeyPress (keyCode, juce::ModifierKeys (mods), 0)))
                return 1; // ran a LYNX action: the plugin doesn't get the key too
        }
    return CallNextHookEx (pluginKeyHook, code, vk, info);
}
} // namespace
#endif

void setSecondaryWindowKeyHandler (std::function<bool (const juce::KeyPress&)> handler)
{
    keyForwarder().handler = std::move (handler);
   #if JUCE_WINDOWS
    if (keyForwarder().handler != nullptr && pluginKeyHook == nullptr)
        pluginKeyHook = SetWindowsHookExW (WH_KEYBOARD, pluginKeyProc, nullptr, GetCurrentThreadId());
    else if (keyForwarder().handler == nullptr && pluginKeyHook != nullptr)
    {
        UnhookWindowsHookEx (pluginKeyHook);
        pluginKeyHook = nullptr;
    }
   #endif
}

void keepAboveMainWindow (juce::Component& window)
{
    window.addKeyListener (&keyForwarder()); // (JUCE adds it once)
    applyRenderingChoice (window);
    auto& all = keepers();
    auto& keeper = all[&window];
    if (keeper == nullptr)
    {
        keeper = std::make_unique<OwnershipKeeper> (window);
        fitWindowOnScreen (window);
    }
    applyOwnership (window);
}

void fitWindowOnScreen (juce::Component& window)
{
    if (! window.isOnDesktop() || ! window.isVisible())
        return;
    auto* resizable = dynamic_cast<juce::ResizableWindow*> (&window);
    if (resizable != nullptr && (resizable->isFullScreen() || resizable->isMinimised()))
        return;

    juce::BorderSize<int> frame;
    if (auto* peer = window.getPeer())
        if (const auto size = peer->getFrameSizeIfPresent())
            frame = *size;

    const auto bounds = window.getBounds();
    const auto& displays = juce::Desktop::getInstance().getDisplays();
    const auto* display = displays.getDisplayForRect (frame.addedTo (bounds));
    if (display == nullptr)
        display = displays.getPrimaryDisplay();
    if (display == nullptr)
        return;
    const auto area = display->userArea;

    auto content = bounds;
    if (resizable != nullptr && resizable->isResizable())
    {
        auto w = juce::jmin (content.getWidth(), area.getWidth() - frame.getLeftAndRight());
        auto h = juce::jmin (content.getHeight(), area.getHeight() - frame.getTopAndBottom());
        if (auto* limits = resizable->getConstrainer())
        {
            w = juce::jmax (w, limits->getMinimumWidth());
            h = juce::jmax (h, limits->getMinimumHeight());
        }
        content.setSize (juce::jmax (1, w), juce::jmax (1, h));
    }
    const auto outer = frame.addedTo (content);
    const auto placed = windowfit::place ({ outer.getX(), outer.getY(), outer.getWidth(), outer.getHeight() },
                                          { area.getX(), area.getY(), area.getWidth(), area.getHeight() });
    content.setPosition (placed.x + frame.getLeft(), placed.y + frame.getTop());
    if (content != bounds)
        window.setBounds (content);
}

void fitAllWindowsOnScreen()
{
    auto& desktop = juce::Desktop::getInstance();
    for (int i = desktop.getNumComponents(); --i >= 0;)
        if (auto* window = dynamic_cast<juce::ResizableWindow*> (desktop.getComponent (i)))
            fitWindowOnScreen (*window);
}
namespace
{
// the usable screen of the main window (or the primary one), in logical pixels
juce::Rectangle<int> usableScreen()
{
    const auto& displays = juce::Desktop::getInstance().getDisplays();
    const juce::Displays::Display* display = nullptr;
    if (auto* main = juce::TopLevelWindow::getActiveTopLevelWindow())
        display = displays.getDisplayForRect (main->getScreenBounds());
    if (display == nullptr)
        display = displays.getPrimaryDisplay();
    return display != nullptr ? display->userArea : juce::Rectangle<int> (0, 0, 1280, 720);
}

// a window's frame (title bar and borders, logical px): this window's own once it is on the screen, else another
// LYNX window's (the same window manager draws them), else a typical native title bar
juce::BorderSize<int> windowFrame (juce::Component* window)
{
    auto frameOf = [] (juce::Component* c) -> std::optional<juce::BorderSize<int>> {
        if (c != nullptr)
            if (auto* peer = c->getPeer())
                if (const auto f = peer->getFrameSizeIfPresent(); f && f->getTop() > 0)
                    return *f;
        return std::nullopt;
    };
    if (const auto own = frameOf (window))
        return *own;
    for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i)
        if (const auto other = frameOf (juce::TopLevelWindow::getTopLevelWindow (i)))
            return *other;
    return { 32, 8, 8, 8 };
}

class ScrollingContent final : public juce::Component,
                               private juce::ComponentListener
{
public:
    explicit ScrollingContent (std::unique_ptr<juce::Component> c) : content (c.release())
    {
        setName (content->getName());
        viewport.setViewedComponent (content, true); // (the viewport deletes it)
        viewport.setScrollBarThickness (12);
        addAndMakeVisible (viewport);
        content->addComponentListener (this);
        fit();
    }

    ~ScrollingContent() override
    {
        if (content != nullptr)
            content->removeComponentListener (this);
    }

    void resized() override { viewport.setBounds (getLocalBounds()); }

    // on the screen: fitted again with the window's real frame, and the window placed so it is all on the screen
    void parentHierarchyChanged() override
    {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<ScrollingContent> (this)] {
            if (safe != nullptr && safe->getPeer() != nullptr)
                safe->fit();
        });
    }

    void fit()
    {
        if (content == nullptr)
            return;
        const auto area = usableScreen();
        auto* window = getTopLevelComponent() != this ? getTopLevelComponent() : nullptr;
        const auto frame = windowFrame (window);
        const auto f = windowfit::scrollFit (content->getWidth(), content->getHeight(), area.getWidth(), area.getHeight(), frame.getLeftAndRight(),
                                             frame.getTopAndBottom(), viewport.getScrollBarThickness());
        viewport.setScrollBarsShown (f.scrollsV, f.scrollsH);
        setSize (f.w, f.h);
        if (window != nullptr && window->isOnDesktop())
            fitWindowOnScreen (*window);
    }

private:
    void componentMovedOrResized (juce::Component&, bool, bool wasResized) override
    {
        if (wasResized)
            fit(); // (the window follows its content's size)
    }

    juce::Component::SafePointer<juce::Component> content;
    juce::Viewport viewport;
};
} // namespace

std::unique_ptr<juce::Component> fitContentToScreen (std::unique_ptr<juce::Component> content)
{
    if (content == nullptr)
        return content;
    const auto area = usableScreen();
    const auto frame = windowFrame (nullptr);
    const auto f = windowfit::scrollFit (content->getWidth(), content->getHeight(), area.getWidth(), area.getHeight(), frame.getLeftAndRight(),
                                         frame.getTopAndBottom(), 12);
    if (! f.scrolls())
        return content;
    return std::make_unique<ScrollingContent> (std::move (content));
}

void matchWindowOrderToScreen()
{
    auto* owner = mainWindow().getComponent();
    if (owner == nullptr)
        return;

    // the windows above the main window, from the lowest up (raising them in that order keeps their order)
    std::vector<juce::Component*> above;
    auto& desktop = juce::Desktop::getInstance();
    for (int i = 0; i < desktop.getNumComponents(); ++i)
        if (auto* c = desktop.getComponent (i); c != owner && c->isVisible() && ! c->isAlwaysOnTop() && isAboveMainWindow (*c))
            above.push_back (c);

   #if JUCE_WINDOWS
    std::map<HWND, int> depth; // 0 = the topmost window on the screen
    int d = 0;
    for (auto h = GetTopWindow (nullptr); h != nullptr; h = GetWindow (h, GW_HWNDNEXT))
        depth[h] = d++;
    const auto depthOf = [&depth] (juce::Component* c) {
        const auto it = depth.find ((HWND) c->getWindowHandle());
        return it == depth.end() ? std::numeric_limits<int>::max() : it->second;
    };
    std::stable_sort (above.begin(), above.end(), [&] (juce::Component* a, juce::Component* b) { return depthOf (a) > depthOf (b); });
   #endif

    for (auto* c : above)
        c->toFront (false);
}
} // namespace lynx
