#include "ui/Updater.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

#include "platform/Package.h"
#include "platform/Paths.h"
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/WindowOwner.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#else
 #include <unistd.h>
#endif

namespace lynx::updater
{
namespace
{
// the failure text of a download stopped by Cancel (never shown)
const juce::String cancelledMarker ("\x01" "cancelled");

juce::String env (const char* name) { return juce::SystemStats::getEnvironmentVariable (name, {}).trim(); }
juce::String fromUtf8 (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }
std::string utf8 (const juce::String& s) { return s.toStdString(); }

// --- where things go (a test profile keeps them all inside it) --------------------------------------------------
juce::File updateFolder()
{
    const auto profile = platform::profileOverride();
    const auto base = profile != juce::File() ? profile.getChildFile ("Temp") : juce::File::getSpecialLocation (juce::File::tempDirectory);
    return base.getChildFile ("LYNX-update");
}

juce::File downloadsFolder()
{
    const auto profile = platform::profileOverride();
    if (profile != juce::File())
        return profile.getChildFile ("Downloads");
   #if JUCE_WINDOWS
    // the Downloads known folder (it can be moved to another drive)
    const auto known = juce::WindowsRegistry::getValue (
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders\\{374DE290-123F-4565-9164-39C4925E467B}");
    if (known.isNotEmpty())
    {
        wchar_t expanded[MAX_PATH * 2] {};
        if (ExpandEnvironmentStringsW (known.toWideCharPointer(), expanded, (DWORD) std::size (expanded)) > 0)
            if (const juce::File f (juce::String (expanded)); juce::File::isAbsolutePath (f.getFullPathName()) && f.isDirectory())
                return f;
    }
   #endif
    const auto xdg = env ("XDG_DOWNLOAD_DIR");
    if (juce::File::isAbsolutePath (xdg) && juce::File (xdg).isDirectory())
        return juce::File (xdg);
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Downloads");
}

juce::File settingsPath() { return platform::settingsFile ("updates.txt"); }
juce::File cachePath() { return platform::settingsFile ("update-cache.json"); }

update::Settings loadSettings() { return update::parseSettings (utf8 (settingsPath().loadFileAsString())); }
void saveSettings (const update::Settings& s) { settingsPath().replaceWithText (fromUtf8 (update::formatSettings (s))); }

update::Version runningVersion()
{
    const auto forced = env ("LYNX_UPDATE_VERSION");
    return update::parseVersion (utf8 (forced.isNotEmpty() ? forced : juce::String (JUCE_APPLICATION_VERSION_STRING)));
}

juce::String overrideUrl() { return env ("LYNX_UPDATE_URL"); }

juce::String userAgent() { return "LYNX-SHARE/" + juce::String (JUCE_APPLICATION_VERSION_STRING) + " (update check)"; }

#if JUCE_LINUX || JUCE_BSD
juce::File findInPath (const juce::String& program)
{
    for (const auto& dir : juce::StringArray::fromTokens (env ("PATH"), ":", {}))
        if (const auto f = juce::File (dir).getChildFile (program); dir.isNotEmpty() && juce::File::isAbsolutePath (dir) && f.existsAsFile())
            return f;
    return {};
}
#endif

juce::File appImageFile()
{
    const auto path = env ("APPIMAGE");
    return juce::File::isAbsolutePath (path) && juce::File (path).existsAsFile() ? juce::File (path) : juce::File();
}

void log (const juce::String& line)
{
    updateFolder().createDirectory();
    updateFolder().getChildFile ("update.log").appendText (juce::Time::getCurrentTime().toISO8601 (true) + "  " + line + "\n");
}

// ================================================================================================================
// the controller: state, the worker thread, the network
// ================================================================================================================
enum class Phase
{
    idle,
    checking,
    available,   // the bar offers the update
    downloading, // (progress, Cancel)
    working,     // appimageupdatetool runs
    ready,       // downloaded and checked: "Install and restart" / "Restart now"
    saved,       // zip / dmg saved to Downloads
    failed,
    updated      // the first start after an update by the installer
};

class Controller final : public juce::ChangeBroadcaster,
                         private juce::Thread
{
public:
    Controller() : juce::Thread ("LYNX update check") {}
    ~Controller() override { stop(); }

    // --- state (message thread) ---
    Phase phase = Phase::idle;
    bool manual = false, hidden = false;
    update::Release release;
    juce::String error;
    std::atomic<float> progress { -1.0f };
    juce::File result;     // the downloaded file / the AppImage to start
    Host host;

    void stop()
    {
        cancelled = true;
        {
            const std::lock_guard<std::mutex> lock (active);
            if (stream != nullptr)
                stream->cancel();
            if (process != nullptr)
                process->kill();
        }
        stopThread (4000);
    }

    bool busy() const { return isThreadRunning(); }

    void setPhase (Phase p)
    {
        phase = p;
        hidden = false;
        sendChangeMessage();
    }

    // runs `job` on the worker thread; false when one is already running
    bool run (std::function<void()> job)
    {
        // (a job posts its result just before it ends: give it that moment)
        if (isThreadRunning() && ! waitForThreadToExit (2000))
            return false;
        cancelled = false;
        pending = std::move (job);
        startThread (juce::Thread::Priority::low);
        return true;
    }

    // `fn` on the message thread, unless the controller is gone by then
    void post (std::function<void()> fn)
    {
        juce::MessageManager::callAsync ([token = std::weak_ptr<int> (alive), fn = std::move (fn)] {
            if (! token.expired())
                fn();
        });
    }

    bool isCancelled() const { return cancelled.load() || threadShouldExit(); }

    // GET `url` into `dest`. `expected` (bytes, 0 = unknown) for the progress; `quick` = a short overall timeout (the
    // release list). False with `error` on failure or Cancel.
    bool download (const juce::String& url, const juce::File& dest, std::int64_t expected, bool quick, juce::String& failure)
    {
        dest.getParentDirectory().createDirectory();
        dest.deleteFile();
       #if JUCE_LINUX || JUCE_BSD
        // (LYNX is built without libcurl, JUCE_USE_CURL=0: the curl program fetches it, as the crash report upload)
        juce::StringArray args { "curl", "-sS", "-L", "--connect-timeout", "8", "-A", userAgent(), "-o", dest.getFullPathName(), "-w", "%{http_code}" };
        if (quick)
            args.addArray ({ "-m", "20", "-H", "Accept: application/vnd.github+json" });
        else
            args.addArray ({ "--speed-limit", "1", "--speed-time", "30" });
        args.add (url);
        auto child = std::make_unique<juce::ChildProcess>();
        if (! child->start (args, juce::ChildProcess::wantStdOut))
        {
            failure = tr ("the curl program is missing");
            return false;
        }
        {
            const std::lock_guard<std::mutex> lock (active);
            process = child.get();
        }
        while (child->isRunning())
        {
            if (isCancelled())
                child->kill();
            if (expected > 0)
                setProgress ((float) juce::jlimit (0.0, 1.0, (double) dest.getSize() / (double) expected));
            juce::Thread::sleep (150);
        }
        const auto code = child->readAllProcessOutput().trim().getIntValue();
        {
            const std::lock_guard<std::mutex> lock (active);
            process = nullptr;
        }
        if (isCancelled())
        {
            dest.deleteFile();
            failure = cancelledMarker;
            return false;
        }
        if (code != 200)
        {
            dest.deleteFile();
            failure = code == 0 ? tr ("no connection") : "HTTP " + juce::String (code);
            return false;
        }
        return dest.existsAsFile();
       #else
        juce::WebInputStream web (juce::URL (url), false);
        web.withExtraHeaders ("User-Agent: " + userAgent() + "\r\nAccept: " + (quick ? "application/vnd.github+json" : "application/octet-stream"))
            .withConnectionTimeout (quick ? 10000 : 20000)
            .withNumRedirectsToFollow (5);
        {
            const std::lock_guard<std::mutex> lock (active);
            stream = &web;
        }
        struct Clear
        {
            Controller& c;
            ~Clear()
            {
                const std::lock_guard<std::mutex> lock (c.active);
                c.stream = nullptr;
            }
        } clear { *this };
        if (isCancelled() || ! web.connect (nullptr))
        {
            failure = isCancelled() ? cancelledMarker : tr ("no connection");
            return false;
        }
        const auto status = web.getStatusCode();
        if (status != 200)
        {
            failure = "HTTP " + juce::String (status);
            return false;
        }
        const auto total = web.getTotalLength() > 0 ? web.getTotalLength() : expected;
        bool ok = true;
        {
            juce::FileOutputStream out (dest);
            if (! out.openedOk())
            {
                failure = tr ("can't write {0}", { dest.getFullPathName() });
                return false;
            }
            juce::HeapBlock<char> buffer (65536);
            std::int64_t got = 0;
            const auto started = juce::Time::getMillisecondCounter();
            while (! web.isExhausted())
            {
                if (isCancelled())
                {
                    failure = cancelledMarker;
                    ok = false;
                    break;
                }
                const auto n = web.read (buffer, 65536);
                if (n < 0 || (n == 0 && ! web.isExhausted() && web.isError()))
                {
                    failure = tr ("the connection was lost");
                    ok = false;
                    break;
                }
                if (n == 0)
                {
                    if (web.isExhausted())
                        break;
                    if (quick && juce::Time::getMillisecondCounter() - started > 20000)
                    {
                        failure = tr ("no connection");
                        ok = false;
                        break;
                    }
                    juce::Thread::sleep (20);
                    continue;
                }
                if (! out.write (buffer, (size_t) n))
                {
                    failure = tr ("can't write {0}", { dest.getFullPathName() });
                    ok = false;
                    break;
                }
                got += n;
                if (total > 0)
                    setProgress ((float) juce::jlimit (0.0, 1.0, (double) got / (double) total));
                if (quick && juce::Time::getMillisecondCounter() - started > 20000)
                {
                    failure = tr ("no connection");
                    ok = false;
                    break;
                }
            }
            out.flush();
            ok = ok && ! out.getStatus().failed();
        }
        if (! ok)
            dest.deleteFile();
        return ok;
       #endif
    }

    bool fetchText (const juce::String& url, juce::String& text, juce::String& failure)
    {
        const auto file = updateFolder().getChildFile ("fetch-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt()) + ".tmp");
        const bool ok = download (url, file, 0, true, failure);
        if (ok)
            text = file.loadFileAsString();
        file.deleteFile();
        return ok;
    }

    // sends a change message at most ~8 times a second while downloading
    void setProgress (float p)
    {
        progress = p;
        const auto now = juce::Time::getMillisecondCounter();
        if (now - lastProgressPost < 120)
            return;
        lastProgressPost = now;
        post ([this] { sendChangeMessage(); });
    }

    juce::ChildProcess* process = nullptr;
    std::mutex active;

private:
    void run() override
    {
        if (pending)
            pending();
        pending = nullptr;
    }

    std::function<void()> pending;
    std::atomic<bool> cancelled { false };
    juce::WebInputStream* stream = nullptr;
    juce::uint32 lastProgressPost = 0;
    std::shared_ptr<int> alive = std::make_shared<int> (0);
};

std::unique_ptr<Controller>& instancePtr()
{
    static std::unique_ptr<Controller> p;
    return p;
}

Controller& controller()
{
    auto& p = instancePtr();
    if (p == nullptr)
        p = std::make_unique<Controller>();
    return *p;
}

void message (juce::MessageBoxIconType icon, const juce::String& title, const juce::String& text)
{
    if (platform::testMode())
        platform::testModeLog ("update: " + title + ": " + text);
    juce::AlertWindow::showMessageBoxAsync (icon, title, text);
}

// --- the check ----------------------------------------------------------------------------------------------------
void check (bool manual)
{
    auto& c = controller();
    if (c.busy())
        return;
    if (! manual && (c.phase == Phase::available || c.phase == Phase::ready))
        return;
    const auto url = overrideUrl();
    c.manual = manual;
    if (manual)
        c.setPhase (Phase::checking);
    c.run ([&c, manual, url] {
        juce::String text, failure;
        auto settings = loadSettings();
        const auto now = juce::Time::currentTimeMillis();
        const bool fixture = url.isNotEmpty();
        bool fetched = false;
        if (! manual && ! fixture && update::cacheFresh (settings.lastCheckMs, now) && cachePath().existsAsFile())
            text = cachePath().loadFileAsString();
        else if (c.fetchText (fixture ? url : juce::String (update::releasesUrl), text, failure))
            fetched = true;

        std::string parseError;
        auto releases = failure.isEmpty() ? update::parseReleases (utf8 (text), &parseError) : std::vector<update::Release>();
        if (failure.isEmpty() && releases.empty() && ! parseError.empty())
            failure = fromUtf8 (parseError);
        if (fetched && failure.isEmpty() && ! fixture)
        {
            cachePath().replaceWithText (text);
            settings.lastCheckMs = now;
            saveSettings (settings);
        }
        const auto running = runningVersion();
        const auto best = update::newestIndex (releases, running);
        update::Release found;
        if (best >= 0)
            found = releases[(std::size_t) best];
        c.post ([&c, manual, failure, best, found, settings, running] {
            if (c.phase == Phase::checking)
                c.phase = Phase::idle;
            if (failure.isNotEmpty())
            {
                if (manual)
                    message (juce::MessageBoxIconType::WarningIcon, tr ("Check for updates"), tr ("Could not check for updates ({0}). Try again later.", { failure }));
                c.sendChangeMessage();
                return;
            }
            if (best < 0)
            {
                if (manual)
                    message (juce::MessageBoxIconType::InfoIcon, tr ("Check for updates"),
                             tr ("You're up to date: LYNX SHARE {0} is the newest version.", { fromUtf8 (running.text()) }));
                c.sendChangeMessage();
                return;
            }
            if (! update::shouldShow (found, settings, manual))
            {
                c.sendChangeMessage();
                return;
            }
            c.release = found;
            c.error.clear();
            c.setPhase (Phase::available);
        });
    });
}

juce::String versionText (const update::Release& r) { return fromUtf8 (r.version.text()); }

juce::String releasePage (const update::Release& r) { return r.page.empty() ? juce::String (update::releasesPage) : fromUtf8 (r.page); }

void fail (const juce::String& why)
{
    auto& c = controller();
    log ("failed: " + why);
    c.error = why;
    c.setPhase (Phase::failed);
}

// downloads the flavour's asset to `dest` and checks it against SHA256SUMS.txt; on the worker thread
bool fetchVerified (Controller& c, const update::Asset& asset, const update::Asset* sums, const juce::File& dest, juce::String& failure)
{
    if (sums == nullptr)
    {
        failure = tr ("the release has no checksums (SHA256SUMS.txt)");
        return false;
    }
    juce::String sumsText;
    if (! c.fetchText (fromUtf8 (sums->url), sumsText, failure))
        return false;
    const auto expected = update::checksumFor (utf8 (sumsText), asset.name);
    if (expected.empty())
    {
        failure = tr ("SHA256SUMS.txt has no line for {0}", { fromUtf8 (asset.name) });
        return false;
    }
    const auto part = dest.getSiblingFile (dest.getFileName() + ".part");
    c.progress = 0.0f;
    if (! c.download (fromUtf8 (asset.url), part, asset.size, false, failure))
        return false;
    update::Sha256Stream hash;
    {
        juce::FileInputStream in (part);
        if (! in.openedOk())
        {
            failure = tr ("can't read {0}", { part.getFullPathName() });
            return false;
        }
        juce::HeapBlock<char> buffer (1 << 16);
        for (int n; (n = in.read (buffer, 1 << 16)) > 0;)
            hash.add (buffer, (std::size_t) n);
    }
    const auto actual = hash.finish();
    if (! update::checksumMatches (expected, actual))
    {
        part.deleteFile();
        failure = tr ("the download is damaged (its checksum does not match)");
        log ("checksum mismatch for " + fromUtf8 (asset.name) + ": expected " + fromUtf8 (expected) + ", got " + fromUtf8 (actual));
        return false;
    }
    dest.deleteFile();
    if (! part.moveFileTo (dest))
    {
        failure = tr ("can't write {0}", { dest.getFullPathName() });
        return false;
    }
    log ("downloaded and checked " + dest.getFullPathName() + " (sha256 " + fromUtf8 (actual) + ")");
    return true;
}

// starts a downloaded file's job: `onDone(file)` on the message thread when it was saved and checked
void downloadThen (const juce::File& dest, std::function<void (const juce::File&)> onDone)
{
    auto& c = controller();
    const auto& assets = c.release.assets;
    const auto index = update::assetFor (flavour(), assets);
    if (index < 0)
    {
        juce::URL (releasePage (c.release)).launchInDefaultBrowser(); // (no file for this build in the release)
        return;
    }
    const auto asset = assets[(std::size_t) index];
    const auto sumsIndex = update::checksumsAsset (assets);
    const std::optional<update::Asset> sums = sumsIndex >= 0 ? std::optional<update::Asset> (assets[(std::size_t) sumsIndex]) : std::nullopt;
    const auto target = dest.isDirectory() ? dest.getChildFile (fromUtf8 (asset.name)) : dest;
    c.progress = 0.0f;
    log ("downloading " + fromUtf8 (asset.url) + " to " + target.getFullPathName());
    const bool started = c.run ([&c, asset, sums, target, onDone] {
        juce::String failure;
        const bool ok = fetchVerified (c, asset, sums ? &*sums : nullptr, target, failure);
        c.post ([ok, failure, target, onDone] {
            if (ok)
                onDone (target);
            else if (failure != cancelledMarker)
                fail (failure);
            else
                controller().setPhase (Phase::available);
        });
    });
    if (started) // (the result is posted to this thread: it comes after this)
        c.setPhase (Phase::downloading);
}

// --- applying -----------------------------------------------------------------------------------------------------
#if JUCE_WINDOWS
std::uint32_t currentPid() { return (std::uint32_t) GetCurrentProcessId(); }
#else
std::uint32_t currentPid() { return (std::uint32_t) getpid(); }
#endif

// Windows installer build: a copy of LYNX.exe waits for this process to end, installs and restarts LYNX SHARE
void installAndRestart (const juce::File& setup)
{
    auto& c = controller();
    if (! c.host.confirmDiscard)
        return;
    c.host.confirmDiscard ([setup] {
        auto& c = controller();
        const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        const auto helper = updateFolder().getChildFile ("LYNX-updater.exe");
        helper.deleteFile();
        if (! exe.copyFileTo (helper))
        {
            fail (tr ("can't write {0}", { helper.getFullPathName() }));
            return;
        }
        updateFolder().getChildFile ("result.txt").deleteFile();
        juce::StringArray args { helper.getFullPathName(), "--apply-update", setup.getFullPathName(), exe.getParentDirectory().getFullPathName(),
                                 juce::String ((juce::int64) currentPid()) };
        if (const auto project = c.host.currentProject ? c.host.currentProject() : juce::File(); project.existsAsFile())
            args.add (project.getFullPathName());
        log ("starting " + args.joinIntoString (" "));
        juce::ChildProcess child;
        if (! child.start (args, 0))
        {
            fail (tr ("can't start {0}", { helper.getFullPathName() }));
            return;
        }
        juce::JUCEApplication::quit();
    });
}

void restartInto (const juce::File& program)
{
    auto& c = controller();
    if (! c.host.confirmDiscard)
        return;
    c.host.confirmDiscard ([program] {
        auto& c = controller();
        juce::StringArray args { program.getFullPathName() };
        if (const auto project = c.host.currentProject ? c.host.currentProject() : juce::File(); project.existsAsFile())
            args.add (project.getFullPathName());
        juce::ChildProcess child;
        if (! child.start (args, 0))
        {
            fail (tr ("can't start {0}", { program.getFullPathName() }));
            return;
        }
        juce::JUCEApplication::quit();
    });
}

void appImageUpdate()
{
    auto& c = controller();
    const auto current = appImageFile();
   #if JUCE_LINUX || JUCE_BSD
    // AppImageUpdate's command-line tool: the zsync update information embedded by scripts/package-linux.sh
    // (gh-releases-zsync|Ka1-S|LYNX-SHARE|latest-all|...) fetches only the changed blocks, then -O replaces the file
    if (const auto tool = findInPath ("appimageupdatetool"); current != juce::File() && tool != juce::File() && overrideUrl().isEmpty())
    {
        c.progress = -1.0f;
        c.setPhase (Phase::working);
        log ("running " + tool.getFullPathName() + " -O " + current.getFullPathName());
        c.run ([&c, tool, current] {
            auto child = std::make_unique<juce::ChildProcess>();
            bool ok = false;
            juce::String output;
            if (child->start (juce::StringArray { tool.getFullPathName(), "-O", current.getFullPathName() }))
            {
                {
                    const std::lock_guard<std::mutex> lock (c.active);
                    c.process = child.get();
                }
                output = child->readAllProcessOutput();
                child->waitForProcessToFinish (600000);
                ok = ! c.isCancelled() && child->getExitCode() == 0;
                const std::lock_guard<std::mutex> lock (c.active);
                c.process = nullptr;
            }
            const auto cancelled = c.isCancelled();
            c.post ([ok, cancelled, output, current] {
                log ("appimageupdatetool: " + output.trim().getLastCharacters (2000));
                if (ok)
                {
                    controller().result = current;
                    controller().setPhase (Phase::ready);
                }
                else if (cancelled)
                    controller().setPhase (Phase::available);
                else
                    fail (tr ("appimageupdatetool could not update the AppImage"));
            });
        });
        return;
    }
   #endif
    // no AppImageUpdate: the new AppImage beside the old one (Downloads when that folder can't be written)
    auto folder = current != juce::File() ? current.getParentDirectory() : downloadsFolder();
    if (! folder.hasWriteAccess())
        folder = downloadsFolder();
    folder.createDirectory();
    downloadThen (folder, [] (const juce::File& file) {
        file.setExecutePermission (true);
        controller().result = file;
        controller().setPhase (Phase::ready);
    });
}

void startUpdate()
{
    auto& c = controller();
    switch (update::methodFor (flavour()))
    {
        case update::Method::runInstaller:
            updateFolder().createDirectory();
            downloadThen (updateFolder(), [] (const juce::File& setup) {
                controller().result = setup;
                controller().setPhase (Phase::ready);
                installAndRestart (setup); // (the save prompt; Cancel leaves "Install and restart" on the bar)
            });
            break;
        case update::Method::downloadAndReveal:
            downloadsFolder().createDirectory();
            downloadThen (downloadsFolder(), [] (const juce::File& file) {
                controller().result = file;
                controller().setPhase (Phase::saved);
                if (! platform::testMode())
                    file.revealToUser();
                else
                    platform::testModeLog ("update: saved " + file.getFullPathName());
            });
            break;
        case update::Method::appImageUpdate: appImageUpdate(); break;
        case update::Method::openPage:
            juce::URL (releasePage (c.release)).launchInDefaultBrowser();
            break;
        case update::Method::none: break;
    }
}

void readyAction()
{
    auto& c = controller();
    if (update::methodFor (flavour()) == update::Method::runInstaller)
        installAndRestart (c.result);
    else
        restartInto (c.result);
}
} // namespace

// ================================================================================================================
// public
// ================================================================================================================
update::Flavour flavour()
{
    static const update::Flavour f = [] {
        update::Facts facts;
       #if JUCE_WINDOWS
        facts.os = "windows";
       #elif JUCE_MAC
        facts.os = "macos";
       #elif JUCE_LINUX || JUCE_BSD
        facts.os = "linux";
       #endif
        facts.packaged = platform::runningPackaged();
        const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        facts.uninstallerBesideExe = exe.getSiblingFile ("Uninstall.exe").existsAsFile();
        facts.appImagePath = utf8 (appImageFile().getFullPathName());
        facts.forced = utf8 (env ("LYNX_UPDATE_FLAVOUR"));
        return update::detectFlavour (facts);
    }();
    return f;
}

bool offered() { return update::checksForUpdates (flavour()); }

bool automatic() { return loadSettings().automatic; }

void setAutomatic (bool on)
{
    auto s = loadSettings();
    if (s.automatic == on)
        return;
    s.automatic = on;
    saveSettings (s);
}

void setHost (Host host) { controller().host = std::move (host); }

void startAutomaticCheck()
{
    if (! offered() || ! automatic())
        return;
    // LYNX_TEST_MODE: no network, unless a test fixture is named
    if (platform::testMode() && overrideUrl().isEmpty())
        return;
    check (false);
}

void checkNow()
{
    if (! offered())
        return;
    if (platform::testMode() && overrideUrl().isEmpty())
    {
        message (juce::MessageBoxIconType::InfoIcon, tr ("Check for updates"), "LYNX_TEST_MODE: no network (set LYNX_UPDATE_URL)");
        return;
    }
    check (true);
}

void shutdown()
{
    if (auto& p = instancePtr(); p != nullptr)
    {
        p->stop();
        p.reset();
    }
}

void afterStart()
{
    const auto folder = updateFolder();
    const auto resultFile = folder.getChildFile ("result.txt");
    if (resultFile.existsAsFile())
    {
        const auto values = juce::StringArray::fromLines (resultFile.loadFileAsString());
        juce::String result, detail, version;
        for (const auto& line : values)
        {
            if (line.startsWith ("result=")) result = line.fromFirstOccurrenceOf ("=", false, false).trim();
            if (line.startsWith ("detail=")) detail = line.fromFirstOccurrenceOf ("=", false, false).trim();
            if (line.startsWith ("version=")) version = line.fromFirstOccurrenceOf ("=", false, false).trim();
        }
        resultFile.deleteFile();
        if (result == "ok")
        {
            auto& c = controller();
            // the release notes come from the cached list when there is one
            const auto list = update::parseReleases (utf8 (cachePath().loadFileAsString()));
            c.release = {};
            for (const auto& r : list)
                if (update::compare (r.version, runningVersion()) == 0 || r.version.text() == utf8 (version))
                    c.release = r;
            if (c.release.tag.empty())
                c.release.version = runningVersion();
            c.setPhase (Phase::updated);
        }
        else
            message (juce::MessageBoxIconType::WarningIcon, tr ("Update"),
                     tr ("The update could not be installed ({0}). This version of LYNX SHARE is still installed; details in {1}.",
                         { detail, folder.getChildFile ("update.log").getFullPathName() }));
    }
    // the installer and the helper copy of LYNX.exe go (the helper may still be ending: once more a little later)
    auto clean = [folder] {
        for (const auto& f : folder.findChildFiles (juce::File::findFiles, false, "*.exe;*.part;*.tmp"))
            f.deleteFile();
    };
    clean();
    juce::Timer::callAfterDelay (15000, clean);
}

bool runApplyHelper (const juce::StringArray& args)
{
    const auto at = args.indexOf ("--apply-update");
    if (at < 0)
        return false;
   #if JUCE_WINDOWS
    const juce::File setup (args[at + 1].unquoted()), folder (args[at + 2].unquoted());
    const auto pid = (DWORD) args[at + 3].getLargeIntValue();
    const auto project = args[at + 4].unquoted();
    const auto resultFile = setup.getSiblingFile ("result.txt");
    auto finish = [&] (bool ok, const juce::String& detail) {
        log (juce::String ("helper: ") + (ok ? "installed" : "FAILED") + (detail.isNotEmpty() ? " (" + detail + ")" : juce::String()));
        resultFile.replaceWithText ("result=" + juce::String (ok ? "ok" : "failed") + "\ndetail=" + detail + "\n");
        // LYNX SHARE starts again (the old one when the installation failed: it shows why)
        const auto exe = folder.getChildFile ("LYNX.exe");
        if (exe.existsAsFile())
            exe.startAsProcess (juce::File::isAbsolutePath (project) ? project.quoted() : juce::String());
    };
    log ("helper: waiting for process " + juce::String ((int) pid) + ", then " + setup.getFullPathName() + " into " + folder.getFullPathName());
    if (! setup.existsAsFile() || ! folder.isDirectory())
    {
        finish (false, "missing " + (! setup.existsAsFile() ? setup.getFullPathName() : folder.getFullPathName()));
        return true;
    }
    if (pid != 0)
        if (auto process = OpenProcess (SYNCHRONIZE, FALSE, pid))
        {
            const auto waited = WaitForSingleObject (process, 120000);
            CloseHandle (process);
            if (waited == WAIT_TIMEOUT)
            {
                finish (false, "LYNX SHARE did not close");
                return true;
            }
        }
    // plugin sandbox / scan child processes of LYNX.exe end a moment later: wait until the exe can be replaced
    const auto exe = folder.getChildFile ("LYNX.exe");
    for (int i = 0; i < 150 && exe.existsAsFile(); ++i)
    {
        auto h = CreateFileW (exe.getFullPathName().toWideCharPointer(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            CloseHandle (h);
            break;
        }
        Sleep (200);
    }
    // NSIS: /S silent, /D=<folder> last and unquoted (it may contain spaces)
    auto command = "\"" + setup.getFullPathName() + "\" /S /D=" + folder.getFullPathName();
    STARTUPINFOW si {};
    si.cb = sizeof (si);
    PROCESS_INFORMATION pi {};
    std::wstring line (command.toWideCharPointer());
    if (! CreateProcessW (nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        finish (false, "can't start the installer (error " + juce::String ((int) GetLastError()) + ")");
        return true;
    }
    const auto waited = WaitForSingleObject (pi.hProcess, 600000);
    DWORD code = 1;
    GetExitCodeProcess (pi.hProcess, &code);
    CloseHandle (pi.hThread);
    CloseHandle (pi.hProcess);
    if (waited == WAIT_TIMEOUT)
        finish (false, "the installer did not finish");
    else
        finish (code == 0, code == 0 ? juce::String() : "installer exit code " + juce::String ((int) code));
   #else
    juce::ignoreUnused (at);
   #endif
    return true;
}

// ================================================================================================================
// the bar
// ================================================================================================================
UpdateBar::UpdateBar()
{
    message.setFont (theme::font (theme::type::body));
    message.setColour (juce::Label::textColourId, theme::colour::textPrimary);
    message.setMinimumHorizontalScale (0.8f);
    addAndMakeVisible (message);
    for (auto* b : { &notesButton, &updateButton, &otherButton })
        addChildComponent (*b);
    updateButton.setColour (juce::TextButton::buttonColourId, theme::colour::accent);
    updateButton.setColour (juce::TextButton::textColourOffId, theme::colour::onAccent);

    juce::Path cross;
    cross.startNewSubPath (0, 0);
    cross.lineTo (10, 10);
    cross.startNewSubPath (10, 0);
    cross.lineTo (0, 10);
    juce::DrawablePath normal, over;
    normal.setPath (cross);
    normal.setStrokeType (juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    normal.setFill (juce::FillType());
    normal.setStrokeFill (theme::colour::textSecondary);
    over.setPath (cross);
    over.setStrokeType (juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    over.setFill (juce::FillType());
    over.setStrokeFill (theme::colour::textPrimary);
    closeButton.setImages (&normal, &over, &over);
    closeButton.setEdgeIndent (8);
    addAndMakeVisible (closeButton);

    notesButton.onClick = [] { showReleaseNotes(); };
    updateButton.onClick = [] {
        auto& c = controller();
        if (c.phase == Phase::available || c.phase == Phase::failed)
            startUpdate();
        else if (c.phase == Phase::ready)
            readyAction();
    };
    otherButton.onClick = [] {
        auto& c = controller();
        if (c.phase == Phase::downloading || c.phase == Phase::working)
            c.stop();
        else if (c.phase == Phase::saved)
            c.result.revealToUser();
        else if (c.phase == Phase::failed)
            juce::URL (releasePage (c.release)).launchInDefaultBrowser();
    };
    closeButton.onClick = [] {
        auto& c = controller();
        if (c.phase == Phase::available) // ✕ = skip this version
        {
            auto s = loadSettings();
            s.skipped = c.release.version.text();
            saveSettings (s);
            log ("skipped " + versionText (c.release));
        }
        c.hidden = true;
        c.sendChangeMessage();
    };
    controller().addChangeListener (this);
    setVisible (false);
    refresh();
}

UpdateBar::~UpdateBar()
{
    if (auto& p = instancePtr(); p != nullptr)
        p->removeChangeListener (this);
}

void UpdateBar::changeListenerCallback (juce::ChangeBroadcaster*) { refresh(); }

void UpdateBar::refresh()
{
    auto& c = controller();
    const auto v = versionText (c.release);
    bool show = ! c.hidden;
    notesButton.setVisible (false);
    updateButton.setVisible (false);
    otherButton.setVisible (false);
    closeButton.setVisible (true);
    closeButton.setTooltip (tr ("Hide"));
    progress = -1.0f;
    const bool installer = update::methodFor (flavour()) == update::Method::runInstaller;
    switch (c.phase)
    {
        case Phase::available:
            text = tr ("LYNX SHARE {0} is available", { v });
            notesButton.setButtonText (tr ("What's new"));
            notesButton.setVisible (true);
            updateButton.setButtonText (update::methodFor (flavour()) == update::Method::openPage ? tr ("Download...") : tr ("Update"));
            updateButton.setVisible (true);
            closeButton.setTooltip (tr ("Skip this version"));
            break;
        case Phase::downloading:
            progress = c.progress.load();
            text = progress >= 0.0f ? tr ("Downloading LYNX SHARE {0}... {1}%", { v, juce::String (juce::roundToInt (progress * 100.0f)) })
                                    : tr ("Downloading LYNX SHARE {0}...", { v });
            otherButton.setButtonText (tr ("Cancel"));
            otherButton.setVisible (true);
            closeButton.setVisible (false);
            break;
        case Phase::working:
            text = tr ("Updating the AppImage to LYNX SHARE {0}...", { v });
            otherButton.setButtonText (tr ("Cancel"));
            otherButton.setVisible (true);
            closeButton.setVisible (false);
            break;
        case Phase::ready:
            text = installer ? tr ("LYNX SHARE {0} is downloaded and ready to install", { v }) : tr ("LYNX SHARE {0} is ready", { v });
            notesButton.setButtonText (tr ("What's new"));
            notesButton.setVisible (true);
            updateButton.setButtonText (installer ? tr ("Install and restart") : tr ("Restart now"));
            updateButton.setVisible (true);
            break;
        case Phase::saved:
            text = tr ("LYNX SHARE {0} was saved to {1}", { v, c.result.getParentDirectory().getFullPathName() });
            otherButton.setButtonText (tr ("Show in folder"));
            otherButton.setVisible (true);
            break;
        case Phase::failed:
            text = tr ("The update failed: {0}", { c.error });
            updateButton.setButtonText (tr ("Try again"));
            updateButton.setVisible (true);
            otherButton.setButtonText (tr ("Open the release page"));
            otherButton.setVisible (true);
            break;
        case Phase::updated:
            text = tr ("LYNX SHARE was updated to {0}", { v });
            notesButton.setButtonText (tr ("What's new"));
            notesButton.setVisible (! c.release.body.empty());
            break;
        case Phase::idle:
        case Phase::checking: show = false; break;
    }
    message.setText (text, juce::dontSendNotification);
    resized();
    repaint();
    if (show != isVisible())
    {
        setVisible (show);
        if (onVisibilityChanged)
            onVisibilityChanged();
    }
}

void UpdateBar::showForSnapshot (int state)
{
    auto& c = controller();
    c.release = {};
    c.release.version = update::parseVersion ("0.1.22-alpha");
    c.release.body = "## New\n- The Update button";
    c.result = downloadsFolder().getChildFile ("LYNX-0.1.22-alpha-windows-x64.zip");
    c.error = tr ("the download is damaged (its checksum does not match)");
    c.progress = 0.42f;
    c.phase = state == 1 ? Phase::downloading : state == 2 ? Phase::failed : state == 3 ? Phase::ready : Phase::available;
    c.hidden = false;
    refresh();
}

void UpdateBar::paint (juce::Graphics& g)
{
    g.fillAll (theme::colour::bg3);
    g.setColour (theme::colour::accent);
    g.fillRect (0, 0, 3, getHeight());
    g.setColour (theme::colour::border);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);
    if (progress >= 0.0f)
    {
        g.setColour (theme::colour::accent);
        g.fillRect (0.0f, (float) getHeight() - 2.0f, (float) getWidth() * progress, 2.0f);
    }
}

void UpdateBar::resized()
{
    auto r = getLocalBounds().reduced (0, 4).withTrimmedLeft (12).withTrimmedRight (4);
    if (closeButton.isVisible())
        closeButton.setBounds (r.removeFromRight (r.getHeight() + 4));
    auto place = [&r] (juce::TextButton& b) {
        if (! b.isVisible())
            return;
        const auto w = juce::jmax (70, b.getBestWidthForHeight (r.getHeight()) + 8);
        b.setBounds (r.removeFromRight (w));
        r.removeFromRight (6);
    };
    place (updateButton);
    place (otherButton);
    place (notesButton);
    message.setBounds (r);
}

// ================================================================================================================
// the release notes window
// ================================================================================================================
namespace
{
class NotesPanel final : public juce::Component
{
public:
    explicit NotesPanel (const update::Release& r)
    {
        title.setText (r.name.empty() ? "LYNX SHARE " + versionText (r) : fromUtf8 (r.name), juce::dontSendNotification);
        title.setFont (theme::font (theme::type::title + 3.0f, true));
        title.setColour (juce::Label::textColourId, theme::colour::textPrimary);
        addAndMakeVisible (title);

        notes.setMultiLine (true, true);
        notes.setReadOnly (true);
        notes.setScrollbarsShown (true);
        notes.setCaretVisible (false);
        notes.setColour (juce::TextEditor::backgroundColourId, theme::colour::bg1);
        notes.setColour (juce::TextEditor::outlineColourId, theme::colour::border);
        notes.setColour (juce::TextEditor::focusedOutlineColourId, theme::colour::border);
        notes.setBorder (juce::BorderSize<int> (10, 12, 10, 12));
        const auto lines = update::releaseNotes (r.body);
        if (lines.empty())
        {
            notes.setColour (juce::TextEditor::textColourId, theme::colour::textSecondary);
            notes.setFont (theme::font (theme::type::body));
            notes.insertTextAtCaret (tr ("This release has no notes."));
        }
        for (const auto& l : lines)
        {
            using K = update::NoteLine::Kind;
            const auto text = fromUtf8 (l.text);
            switch (l.kind)
            {
                case K::heading:
                    notes.setColour (juce::TextEditor::textColourId, l.level <= 2 ? theme::colour::accent : theme::colour::textPrimary);
                    notes.setFont (theme::font (l.level <= 1 ? theme::type::title + 3.0f : l.level == 2 ? theme::type::title + 1.0f : theme::type::body + 1.0f, true));
                    notes.insertTextAtCaret (text + "\n");
                    break;
                case K::bullet:
                    notes.setColour (juce::TextEditor::textColourId, theme::colour::textPrimary);
                    notes.setFont (theme::font (theme::type::body));
                    notes.insertTextAtCaret (juce::String::repeatedString ("    ", l.level) + juce::String::fromUTF8 ("\xE2\x80\xA2  ") + text + "\n");
                    break;
                case K::text:
                    notes.setColour (juce::TextEditor::textColourId, theme::colour::textSecondary);
                    notes.setFont (theme::font (theme::type::body));
                    notes.insertTextAtCaret (text + "\n");
                    break;
                case K::blank:
                    notes.setFont (theme::font (theme::type::small));
                    notes.insertTextAtCaret ("\n");
                    break;
            }
        }
        notes.moveCaretToTop (false);
        addAndMakeVisible (notes);

        page = releasePage (r);
        githubButton.setButtonText (tr ("Open on GitHub"));
        githubButton.onClick = [this] { juce::URL (page).launchInDefaultBrowser(); };
        closeButton.setButtonText (tr ("Close"));
        closeButton.onClick = [this] {
            if (auto* w = findParentComponentOfClass<juce::DialogWindow>())
                w->exitModalState (0); // (launchAsync: deleted when dismissed)
        };
        const auto canUpdate = controller().phase == Phase::available && update::methodFor (flavour()) != update::Method::none;
        updateButton.setButtonText (update::methodFor (flavour()) == update::Method::openPage ? tr ("Download...") : tr ("Update"));
        updateButton.setColour (juce::TextButton::buttonColourId, theme::colour::accent);
        updateButton.setColour (juce::TextButton::textColourOffId, theme::colour::onAccent);
        updateButton.onClick = [this] {
            startUpdate();
            closeButton.triggerClick();
        };
        updateButton.setVisible (canUpdate);
        for (auto* b : { &githubButton, &closeButton })
            addAndMakeVisible (*b);
        addChildComponent (updateButton);
        setSize (680, 540);
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::colour::bg2); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        title.setBounds (r.removeFromTop (30));
        r.removeFromTop (8);
        auto buttons = r.removeFromBottom (30);
        r.removeFromBottom (12);
        notes.setBounds (r);
        closeButton.setBounds (buttons.removeFromRight (100));
        buttons.removeFromRight (8);
        if (updateButton.isVisible())
        {
            updateButton.setBounds (buttons.removeFromRight (120));
            buttons.removeFromRight (8);
        }
        githubButton.setBounds (buttons.removeFromLeft (150));
    }

private:
    juce::Label title;
    juce::TextEditor notes;
    juce::TextButton githubButton, closeButton, updateButton;
    juce::String page;
};
} // namespace

void showReleaseNotes()
{
    const auto& release = controller().release;
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (fitContentToScreen (std::make_unique<NotesPanel> (release)).release());
    options.dialogTitle = tr ("What's new in LYNX SHARE {0}", { versionText (release) });
    options.dialogBackgroundColour = theme::colour::bg2;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.escapeKeyTriggersCloseButton = true;
    if (auto* dialog = options.launchAsync())
    {
        dialog->setResizeLimits (420, 320, 1600, 1400);
        keepAboveMainWindow (*dialog);
    }
}

std::unique_ptr<juce::Component> makeReleaseNotesForSnapshot()
{
    update::Release r;
    r.name = "LYNX SHARE 0.1.22-alpha";
    r.version = update::parseVersion ("0.1.22-alpha");
    r.body = "## New\n- **In-app updates**: a bar shows when a new version is out; *Update* installs it.\n"
             "  - Windows installer: downloads, checks and installs it, then restarts.\n- Help › Check for updates...\n\n"
             "## Fixes\n- The [Mixer](https://lynx-share.com) keeps its layout.\n\nThanks to everyone who reported a bug!";
    controller().release = r;
    controller().phase = Phase::available;
    return std::make_unique<NotesPanel> (r);
}
} // namespace lynx::updater
