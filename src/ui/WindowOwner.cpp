#include "ui/WindowOwner.h"

#include "ui/LogoData.h"
#include "ui/Theme.h"
#include "platform/Paths.h"

#include <cstdint>
#include <map>
#include <memory>
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

void keepAboveMainWindow (juce::Component& window)
{
    applyRenderingChoice (window);
    auto& all = keepers();
    auto& keeper = all[&window];
    if (keeper == nullptr)
        keeper = std::make_unique<OwnershipKeeper> (window);
    applyOwnership (window);
}
} // namespace lynx
