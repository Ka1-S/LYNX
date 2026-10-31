#pragma once

// In-app update check + Update button (round 166b), the app side of core/UpdateCheck.h:
// - a background check at start-up (Settings › General › "Check for updates automatically", on by default; never
//   blocks the start, short timeouts, silent when offline) and Help › "Check for updates..." (says when LYNX SHARE is
//   up to date). One version query to the GitHub API, nothing about the user (PRIVACY.md). The list is cached for a
//   few hours. The Microsoft Store build never checks (the Store updates it), and neither do LYNX_TEST_MODE runs,
//   unless LYNX_UPDATE_URL points them at a test fixture; command-line modes (--selftest, --scantest...) never start one;
// - a slim bar under the toolbar: "LYNX SHARE x.y.z is available · What's new · Update · ✕" (✕ skips that version);
// - Update, by build: the Windows installer build downloads the new setup.exe, checks it against the release's
//   SHA256SUMS.txt, asks to save the project (as on quit), closes, installs it silently over itself (a copy of
//   LYNX.exe runs `--apply-update` and waits for this process to end) and starts again; the portable zip and macOS
//   download the new zip / dmg to Downloads (checked) and show it; the AppImage updates in place with
//   appimageupdatetool (zsync) when it is installed, else downloads the new AppImage beside the old one; the tar.gz
//   opens the release page.
// Test hooks: LYNX_UPDATE_URL (the release list; also enables the check under LYNX_TEST_MODE),
// LYNX_UPDATE_FLAVOUR (installer, portable, macos, appimage, tarball, store), LYNX_UPDATE_VERSION (the running version).

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/UpdateCheck.h"

namespace lynx::updater
{
update::Flavour flavour();
bool offered(); // this build looks for updates at all (false: the Store build) — the Settings toggle shows

bool automatic();
void setAutomatic (bool on);

// What the main window lends the updater: "save the project, then go on" (the quit prompt) and the project to reopen.
struct Host
{
    std::function<void (std::function<void()> then)> confirmDiscard;
    std::function<juce::File()> currentProject;
};
void setHost (Host host);

void startAutomaticCheck(); // start-up (normal UI runs only)
void checkNow();            // Help › Check for updates...
void shutdown();            // stops a check or download in flight (app shutdown)

// After an update by the installer: the helper's result (a notice when it failed) and the temporary files go.
void afterStart();

// `LYNX.exe --apply-update <setup.exe> <install folder> <pid> [project]` (Windows; a copy of LYNX.exe in the temp
// folder): waits for process <pid> to end, runs the installer silently into the folder, starts LYNX again. True when
// the arguments asked for it (the caller quits).
bool runApplyHelper (const juce::StringArray& args);

// The bar under the toolbar. Hidden while there is nothing to say; `onVisibilityChanged` lets the window lay out.
class UpdateBar final : public juce::Component,
                        private juce::ChangeListener
{
public:
    UpdateBar();
    ~UpdateBar() override;
    std::function<void()> onVisibilityChanged;
    static constexpr int height = 30;
    void paint (juce::Graphics&) override;
    void resized() override;
    void showForSnapshot (int state); // --snapshot: 0 available, 1 downloading, 2 failed, 3 ready

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void refresh();
    juce::Label message;
    juce::TextButton notesButton, updateButton, otherButton;
    juce::DrawableButton closeButton { "skip", juce::DrawableButton::ImageFitted };
    juce::String text;
    float progress = -1.0f;
};

// The release notes window (also for --snapshot).
void showReleaseNotes();
std::unique_ptr<juce::Component> makeReleaseNotesForSnapshot();
} // namespace lynx::updater
