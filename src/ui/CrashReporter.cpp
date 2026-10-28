#include "engine/FileSort.h"
#include "ui/CrashReporter.h"

#include <atomic>
#include <cmath>
#include <mutex>

#include "platform/CrashGuard.h"
#include "platform/Paths.h"
#include "engine/PluginHost.h"
#include "engine/PluginSandbox.h"
#include "ui/HintBar.h"
#include "ui/I18n.h"
#include "ui/SettingsPages.h"
#include "ui/Theme.h"
#include "ui/WindowOwner.h"

#if __has_include("lynx_build.h")
 #include "lynx_build.h" // LYNX_GIT_COMMIT (written by the build)
#endif
#ifndef LYNX_GIT_COMMIT
 #define LYNX_GIT_COMMIT ""
#endif

namespace lynx::crashreporter
{
namespace cr = crashreport;

namespace
{
juce::File reportsFolder() { return platform::settingsFile ("crash-reports"); }
juce::File pendingFolder() { return reportsFolder().getChildFile ("pending"); }
juce::File consentFile() { return platform::settingsFile ("crash-reports.txt"); }

std::string utf8 (const juce::String& s) { return s.toStdString(); }
juce::String fromUtf8 (const std::string& s) { return juce::String::fromUTF8 (s.data(), (int) s.size()); }
} // namespace

cr::Consent consent()
{
    return cr::parseConsent (utf8 (consentFile().loadFileAsString()));
}

void setConsent (cr::Consent c)
{
    consentFile().replaceWithText (cr::consentName (c));
}

// ================================================================================================================
// the session file
// ================================================================================================================
juce::String Session::kind() const
{
    if (crashFile.existsAsFile())
        return "crash";
    if (hangFile != juce::File())
        return "hang";
    return "unclean";
}

Session parseSession (const juce::String& text)
{
    Session s;
    const auto autosave = platform::settingsFile ("autosave");
    for (const auto& line : juce::StringArray::fromLines (text))
    {
        if (line.startsWith ("step="))
            s.steps.add (line.fromFirstOccurrenceOf ("=", false, false));
        else if (line.startsWith ("plugin="))
            s.plugins.add (line.fromFirstOccurrenceOf ("=", false, false));
        else if (line.startsWith ("info."))
            s.info.set (line.fromFirstOccurrenceOf (".", false, false).upToFirstOccurrenceOf ("=", false, false),
                        line.fromFirstOccurrenceOf ("=", false, false));
        else if (line.startsWith ("version="))
            s.version = line.fromFirstOccurrenceOf ("=", false, false);
    }
    // the crash / hang file the steps name ("crashed: backtrace in autosave\crash ….txt", "UI not responding: stack in
    // autosave\hang ….txt"); the newest counts
    for (const auto& step : s.steps)
    {
        const auto body = step.fromFirstOccurrenceOf (" ", false, false);
        for (const auto* marker : { "backtrace in autosave\\", "stack in autosave\\" })
            if (body.contains (marker))
            {
                const auto file = autosave.getChildFile (body.fromFirstOccurrenceOf (marker, false, false).trim());
                (juce::String (marker).startsWith ("backtrace") ? s.crashFile : s.hangFile) = file;
            }
        if (body.startsWith ("UI responding again"))
            s.hangFile = juce::File(); // the freeze ended by itself: not what closed LYNX
    }
    return s;
}

// ================================================================================================================
// this computer's facts
// ================================================================================================================
namespace
{
juce::String osVersion()
{
   #if JUCE_WINDOWS
    const juce::String key = "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\";
    const auto display = juce::WindowsRegistry::getValue (key + "DisplayVersion");
    const auto build = juce::WindowsRegistry::getValue (key + "CurrentBuild");
    const auto ubr = juce::WindowsRegistry::getValue (key + "UBR");
    return (display.isNotEmpty() ? display + " " : juce::String()) + "(build " + build + (ubr.isNotEmpty() ? "." + ubr : juce::String()) + ")";
   #elif JUCE_LINUX || JUCE_BSD
    juce::String pretty;
    for (const auto& line : juce::StringArray::fromLines (juce::File ("/etc/os-release").loadFileAsString()))
        if (line.startsWith ("PRETTY_NAME="))
            pretty = line.fromFirstOccurrenceOf ("=", false, false).unquoted();
    const auto kernel = juce::File ("/proc/sys/kernel/osrelease").loadFileAsString().trim();
    const auto session = juce::SystemStats::getEnvironmentVariable ("XDG_SESSION_TYPE", {});
    return pretty + (kernel.isNotEmpty() ? ", kernel " + kernel : juce::String()) + (session.isNotEmpty() ? ", " + session : juce::String())
         + (juce::SystemStats::getEnvironmentVariable ("APPIMAGE", {}).isNotEmpty() ? ", AppImage" : juce::String());
   #else
    return {};
   #endif
}

std::vector<cr::Gpu> gpus()
{
    std::vector<cr::Gpu> list;
   #if JUCE_WINDOWS
    // the display adapters' class key: name and driver version of each (no extra API, readable by every user)
    const juce::String base = "HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\";
    for (int i = 0; i < 16; ++i)
    {
        const auto key = base + juce::String (i).paddedLeft ('0', 4) + "\\";
        const auto name = juce::WindowsRegistry::getValue (key + "DriverDesc");
        if (name.isEmpty())
            continue;
        list.push_back ({ utf8 (name), utf8 (juce::WindowsRegistry::getValue (key + "DriverVersion")) });
    }
   #elif JUCE_LINUX || JUCE_BSD
    for (const auto& card : juce::File ("/sys/class/drm").findChildFiles (juce::File::findDirectories, false, "card?"))
    {
        const auto device = card.getChildFile ("device");
        const auto ids = device.getChildFile ("vendor").loadFileAsString().trim() + ":" + device.getChildFile ("device").loadFileAsString().trim();
        const auto driver = device.getChildFile ("driver").getLinkedTarget().getFileName();
        list.push_back ({ utf8 ("PCI " + ids), utf8 (driver) });
    }
    if (const auto nv = juce::File ("/proc/driver/nvidia/version").loadFileAsString(); nv.isNotEmpty())
        list.push_back ({ "NVIDIA", utf8 (juce::StringArray::fromLines (nv)[0].trim()) });
   #endif
    return list;
}

juce::String describeDevice (juce::AudioDeviceManager& devices)
{
    if (auto* device = devices.getCurrentAudioDevice())
        return device->getTypeName() + " | " + device->getName() + " | " + juce::String ((int) device->getCurrentSampleRate()) + " | "
             + juce::String (device->getCurrentBufferSizeSamples());
    return "none";
}

juce::String nowIso() { return juce::Time::getCurrentTime().toISO8601 (true); }

juce::String newId()
{
    juce::Random random; // (seeded from the system's entropy)
    std::uint8_t bytes[16];
    for (auto& b : bytes)
        b = (std::uint8_t) random.nextInt (256);
    const juce::Uuid uuid;
    for (int i = 0; i < 16; ++i)
        bytes[i] ^= uuid.getRawData()[i]; // two sources: nothing predictable even if one is weak
    return fromUtf8 (cr::formatUuid (bytes));
}
} // namespace

cr::Anonymiser makeAnonymiser (bool keepFileNames)
{
    cr::Anonymiser a;
    for (const auto& n : { juce::SystemStats::getLogonName(), juce::SystemStats::getFullUserName(),
                           juce::SystemStats::getEnvironmentVariable ("USERNAME", {}), juce::SystemStats::getEnvironmentVariable ("USER", {}) })
        if (n.trim().isNotEmpty())
            a.userNames.push_back (utf8 (n.trim()));
    for (const auto& h : { juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName(),
                           juce::SystemStats::getEnvironmentVariable ("USERPROFILE", {}), juce::SystemStats::getEnvironmentVariable ("HOME", {}) })
        if (h.isNotEmpty())
            a.homeDirs.push_back (utf8 (h));
    for (const auto& m : { juce::SystemStats::getComputerName(), juce::SystemStats::getEnvironmentVariable ("COMPUTERNAME", {}) })
        if (m.trim().isNotEmpty())
            a.machineNames.push_back (utf8 (m.trim()));
    a.keepFileNames = keepFileNames;
    return a;
}

cr::ReportInput makeInput (const Session& session, const juce::String& kind, juce::AudioDeviceManager* devices)
{
    cr::ReportInput in;
    in.id = utf8 (newId());
    in.kind = utf8 (kind);
    in.created = utf8 (nowIso());
    in.version = utf8 (session.version.isNotEmpty() && kind != "manual" ? session.version : juce::String (JUCE_APPLICATION_VERSION_STRING));
   #if JUCE_DEBUG
    in.build = "debug";
   #else
    in.build = "release";
   #endif
   #if JUCE_WINDOWS
    in.build += " windows-x64";
   #elif JUCE_MAC && JUCE_ARM
    in.build += " macos-arm64";
   #elif JUCE_MAC
    in.build += " macos-x64";
   #elif JUCE_ARM
    in.build += " linux-arm64";
   #else
    in.build += " linux-x64";
   #endif
    in.commit = LYNX_GIT_COMMIT;
    in.language = utf8 (i18n::current());
    in.os = utf8 (juce::SystemStats::getOperatingSystemName());
    in.osVersion = utf8 (osVersion());
    in.cpu = utf8 (juce::SystemStats::getCpuModel().trim());
    in.cpuCores = juce::SystemStats::getNumCpus();
    in.memoryMb = juce::SystemStats::getMemorySizeInMegabytes();
    in.gpus = gpus();
    if (juce::MessageManager::existsAndIsCurrentThread())
        for (const auto& d : juce::Desktop::getInstance().getDisplays().displays)
            in.screens.push_back ({ d.totalArea.getWidth(), d.totalArea.getHeight(), d.scale });

    // the audio device of that session (as its file recorded it), else the one open now
    auto audio = session.info["audio"];
    if (audio.isEmpty() && devices != nullptr)
        audio = describeDevice (*devices);
    if (const auto parts = juce::StringArray::fromTokens (audio, "|", ""); parts.size() >= 4)
    {
        in.audioDriver = utf8 (parts[0].trim());
        in.audioDevice = utf8 (parts[1].trim());
        in.sampleRate = parts[2].trim().getDoubleValue();
        in.bufferSize = parts[3].trim().getIntValue();
    }

    if (session.crashFile.existsAsFile())
    {
        in.crashFile = utf8 (session.crashFile.getFileName());
        in.crashText = utf8 (session.crashFile.loadFileAsString());
    }
    if (session.hangFile.existsAsFile())
    {
        in.hangFile = utf8 (session.hangFile.getFileName());
        in.hangText = utf8 (session.hangFile.loadFileAsString());
    }
    for (const auto& step : session.steps)
        in.steps.push_back (utf8 (step));
    for (const auto& p : session.plugins)
    {
        const auto f = juce::StringArray::fromTokens (p, "|", "");
        in.plugins.push_back ({ utf8 (f[0]), utf8 (f[1]), utf8 (f[2] + (f.size() > 4 && f[4].isNotEmpty() ? " (" + f[4] + ")" : juce::String())), utf8 (f[3]) });
    }
    in.settings = { { "gpuRendering", gpuRenderingEnabled() }, { "pluginSandbox", pluginSandboxEnabled() }, { "pluginSubstitutes", pluginSubstitutesEnabled() } };
    return in;
}

juce::String buildJson (const cr::ReportInput& input, bool keepFileNames)
{
    return fromUtf8 (cr::buildJson (input, makeAnonymiser (keepFileNames)));
}

juce::String githubUrl (const cr::ReportInput& in)
{
    const auto an = makeAnonymiser (false);
    const auto a = [&] (const std::string& s) { return an.apply (s); };
    cr::IssueFields f;
    f.version = in.version;
    f.os = cr::githubOsOption (in.os, in.build.find ("arm64") != std::string::npos ? "arm64" : "x64",
                               juce::SystemStats::getEnvironmentVariable ("APPIMAGE", {}).isNotEmpty());
    std::string details = in.os + " " + in.osVersion + ", " + in.cpu;
    for (const auto& g : in.gpus)
        details += ", " + g.name + (g.driver.empty() ? "" : " (" + g.driver + ")");
    for (const auto& s : in.screens)
        details += ", " + std::to_string ((int) std::lround (s.scale * 100.0)) + " %";
    f.osDetails = a (details);
    if (! in.audioDriver.empty())
        f.audio = a (in.audioDriver + " / " + in.audioDevice + ", " + std::to_string ((int) in.sampleRate) + " Hz, " + std::to_string (in.bufferSize) + " samples");
    f.kind = cr::githubKindOption (in.kind);
    const auto last = in.steps.empty() ? std::string() : a (in.steps.back().substr (std::min<std::size_t> (in.steps.back().find (' ') + 1, in.steps.back().size())));
    f.title = (in.kind == "hang" ? "Freeze: " : in.kind == "manual" ? "Problem: " : "Crash: ") + last;
    if (! in.steps.empty())
    {
        std::string steps = "LYNX's last steps before it closed:\n";
        for (auto i = in.steps.size() > 15 ? in.steps.size() - 15 : 0; i < in.steps.size(); ++i)
            steps += a (in.steps[i]) + "\n";
        f.steps = steps;
    }
    for (const auto& p : in.plugins)
        f.plugins += a (p.name + " (" + p.format + ", " + p.vendor + (p.version.empty() ? "" : ", " + p.version) + ")") + "\n";
    std::string logs;
    for (const auto& [file, text] : { std::pair (in.crashFile, in.crashText), std::pair (in.hangFile, in.hangText) })
        if (! text.empty())
        {
            logs += "Please attach " + a (file) + " (LYNX: \"Open the folder with the report\"). Its first lines:\n```\n";
            std::string head;
            int lines = 0;
            for (std::size_t at = 0; at < text.size() && lines < 40; ++lines)
            {
                auto end = text.find ('\n', at);
                if (end == std::string::npos)
                    end = text.size();
                head += text.substr (at, end - at) + "\n";
                at = end + 1;
            }
            logs += a (head) + "```\n";
        }
    f.logs = logs;
    return fromUtf8 (cr::githubIssueUrl (f));
}

// ================================================================================================================
// sending
// ================================================================================================================
juce::String endpoint()
{
    const auto custom = juce::SystemStats::getEnvironmentVariable ("LYNX_CRASH_ENDPOINT", {});
    return custom.isNotEmpty() ? custom : juce::String ("https://lynx-share.com/api/crash.php");
}

namespace
{
juce::MemoryBlock gzip (const juce::String& json)
{
    juce::MemoryOutputStream out;
    {
        juce::GZIPCompressorOutputStream z (out, 9, juce::GZIPCompressorOutputStream::windowBitsGZIP);
        z.write (json.toRawUTF8(), json.getNumBytesAsUTF8());
        z.flush();
    }
    return out.getMemoryBlock();
}

enum class Outcome
{
    sent,
    retry,  // offline, timeout, server busy: try again later
    reject  // the server refused it (4xx): never again
};

Outcome post (const juce::MemoryBlock& body, juce::String& message)
{
   #if JUCE_LINUX || JUCE_BSD
    // (LYNX is built without libcurl, JUCE_USE_CURL=0: the curl program sends it)
    const auto file = juce::File::createTempFile (".json.gz");
    file.replaceWithData (body.getData(), body.getSize());
    juce::ChildProcess curl;
    juce::StringArray args { "curl", "-sS", "-m", "20", "-X", "POST", "-H", "Content-Type: application/json", "-H", "Content-Encoding: gzip",
                             "--data-binary", "@" + file.getFullPathName(), "-w", "\n%{http_code}", endpoint() };
    if (! curl.start (args, juce::ChildProcess::wantStdOut))
    {
        file.deleteFile();
        message = "curl not found";
        return Outcome::retry;
    }
    const auto output = curl.readAllProcessOutput();
    curl.waitForProcessToFinish (25000);
    file.deleteFile();
    const auto status = output.fromLastOccurrenceOf ("\n", false, false).trim().getIntValue();
    const auto text = output.upToLastOccurrenceOf ("\n", false, false);
   #else
    int status = 0;
    juce::StringPairArray headers;
    auto stream = juce::URL (endpoint())
                      .withPOSTData (body)
                      .createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                                              .withExtraHeaders ("Content-Type: application/json\r\nContent-Encoding: gzip")
                                              .withConnectionTimeoutMs (20000)
                                              .withNumRedirectsToFollow (2)
                                              .withStatusCode (&status));
    const auto text = stream != nullptr ? stream->readEntireStreamAsString() : juce::String();
   #endif
    const auto reply = juce::JSON::parse (text);
    if (status == 200 && (bool) reply.getProperty ("ok", false))
    {
        message = reply.getProperty ("id", {}).toString();
        return Outcome::sent;
    }
    message = status == 0 ? juce::String ("no connection") : "HTTP " + juce::String (status) + " " + reply.getProperty ("error", {}).toString();
    return status == 0 || status == 408 || status == 429 || status >= 500 ? Outcome::retry : Outcome::reject;
}

void logSent (const juce::String& id, const juce::String& what)
{
    reportsFolder().createDirectory();
    reportsFolder().getChildFile ("sent.log").appendText (nowIso() + "  " + id + "  " + what + "\n");
}

// One background thread sends the pending folder's reports, oldest first; never on the message thread.
class Uploader final : private juce::Thread
{
public:
    Uploader() : juce::Thread ("LYNX crash report upload") {}
    ~Uploader() override { stopThread (1000); }

    void wake (std::function<void (bool, const juce::String&)> done, const juce::String& id)
    {
        {
            const std::lock_guard<std::mutex> lock (mutex);
            if (done)
                callbacks.emplace_back (id, std::move (done));
        }
        if (! isThreadRunning())
            startThread (juce::Thread::Priority::low);
        notify();
    }

    bool busy() const { return isThreadRunning() && working.load(); }
    void finish() { signalThreadShouldExit(); notify(); }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            working = true;
            auto files = pendingFolder().findChildFiles (juce::File::findFiles, false, "*.json.gz");
            filesort::byTime (files, filesort::By::modificationTime);
            for (const auto& file : files)
            {
                if (threadShouldExit())
                    break;
                const auto id = file.getFileName().upToFirstOccurrenceOf (".", false, false);
                if (handled.contains (id))
                    continue; // tried in this run already: the next start retries it
                handled.add (id);
                juce::MemoryBlock body;
                file.loadFileAsData (body);
                juce::String message;
                const auto outcome = post (body, message);
                const auto tries = file.withFileExtension ("").withFileExtension ("tries");
                if (outcome == Outcome::sent)
                {
                    logSent (id, "sent");
                    file.deleteFile();
                    tries.deleteFile();
                }
                else if (outcome == Outcome::reject || tries.loadFileAsString().getIntValue() >= 1)
                {
                    logSent (id, "dropped (" + message + ")");
                    file.deleteFile();
                    tries.deleteFile();
                }
                else
                    tries.replaceWithText ("1"); // once more at the next start
                callBack (id, outcome == Outcome::sent, outcome == Outcome::sent ? id : message);
            }
            working = false;
            wait (-1);
        }
    }

    void callBack (const juce::String& id, bool ok, const juce::String& message)
    {
        std::function<void (bool, const juce::String&)> fn;
        {
            const std::lock_guard<std::mutex> lock (mutex);
            for (auto it = callbacks.begin(); it != callbacks.end(); ++it)
                if (it->first == id)
                {
                    fn = std::move (it->second);
                    callbacks.erase (it);
                    break;
                }
        }
        if (fn)
            juce::MessageManager::callAsync ([fn, ok, message] { fn (ok, message); });
    }

    std::mutex mutex;
    std::vector<std::pair<juce::String, std::function<void (bool, const juce::String&)>>> callbacks;
    juce::StringArray handled;
    std::atomic<bool> working { false };
};

Uploader& uploader()
{
    static Uploader instance;
    return instance;
}
} // namespace

void send (const juce::String& json, const juce::String& id, std::function<void (bool, const juce::String&)> done)
{
    pendingFolder().createDirectory();
    const auto body = gzip (json);
    pendingFolder().getChildFile (id + ".json.gz").replaceWithData (body.getData(), body.getSize());
    uploader().wake (std::move (done), id);
}

void retryPending()
{
    if (pendingFolder().findChildFiles (juce::File::findFiles, false, "*.json.gz").isEmpty())
        return;
    if (consent() == cr::Consent::never)
        return; // (the user changed their mind: nothing leaves)
    uploader().wake ({}, {});
}

void shutdown()
{
    for (int i = 0; i < 30 && uploader().busy(); ++i) // up to 3 s for an upload in flight
        juce::Thread::sleep (100);
    uploader().finish();
}

juce::String sendNow (const juce::String& json)
{
    juce::String message;
    const auto outcome = post (gzip (json), message);
    if (outcome == Outcome::sent)
    {
        logSent (message, "sent (test)");
        return {};
    }
    return message.isNotEmpty() ? message : juce::String ("failed");
}

void watchAudioDevice (juce::AudioDeviceManager& devices)
{
    struct Watch final : juce::ChangeListener
    {
        void changeListenerCallback (juce::ChangeBroadcaster* source) override
        {
            if (auto* d = dynamic_cast<juce::AudioDeviceManager*> (source))
                crashguard::setSessionInfo ("audio", utf8 (describeDevice (*d)));
        }
    };
    static Watch watch;
    devices.addChangeListener (&watch);
    watch.changeListenerCallback (&devices);
}

// ================================================================================================================
// the dialog
// ================================================================================================================
namespace
{
class ReportPanel final : public juce::Component
{
public:
    ReportPanel (Session s, juce::String notice, juce::String plugin, std::function<void()> separateFn, juce::AudioDeviceManager* d, bool isManual)
        : session (std::move (s)), separatePlugin (std::move (plugin)), onSeparate (std::move (separateFn)), devices (d), manual (isManual)
    {
        input = makeInput (session, manual ? juce::String ("manual") : session.kind(), devices);

        settingsui::styleLabel (message, manual ? tr ("Describe what happened. LYNX adds its version, your system (operating system, CPU, graphics "
                                                     "card, audio device) and, if you want, the newest crash / freeze files and this session's last "
                                                     "steps. Nothing is sent before you click Send.")
                                                : tr ("LYNX closed unexpectedly last time. Send the report to help fix it?") + "\n\n" + notice,
                                false);
        message.setJustificationType (juce::Justification::topLeft);
        message.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (message);

        auto toggle = [this] (juce::ToggleButton& t, const juce::String& text, bool on) {
            t.setButtonText (text);
            t.setToggleState (on, juce::dontSendNotification);
            t.setColour (juce::ToggleButton::textColourId, theme::colour::textPrimary);
            t.setColour (juce::ToggleButton::tickColourId, theme::colour::accent);
            addAndMakeVisible (t);
        };
        if (separatePlugin.isNotEmpty())
            toggle (separate, tr ("Run {0} in a separate process from now on (if it crashes again, only the plugin stops)", { separatePlugin }), true);
        toggle (fileNames, tr ("Include file names (projects, samples); otherwise only their extension is sent"), false);
        fileNames.onClick = [this] { refreshPreview(); };
        if (manual)
        {
            toggle (attach, tr ("Attach the newest crash / freeze files and this session's steps"), true);
            attach.onClick = [this] { refreshInput(); };
            settingsui::styleLabel (descriptionLabel, tr ("What happened?"));
            settingsui::styleLabel (emailLabel, tr ("E-mail (optional, only if you want an answer)"));
            for (auto* e : { &description, &email })
            {
                settingsui::styleEditor (*e);
                addAndMakeVisible (*e);
            }
            description.setMultiLine (true, true);
            description.setReturnKeyStartsNewLine (true);
            description.onTextChange = [this] { refreshInput(); };
            email.onTextChange = [this] { refreshInput(); };
            addAndMakeVisible (descriptionLabel);
            addAndMakeVisible (emailLabel);
            refreshInput();
        }
        else
            toggle (always, tr ("Always send without asking"), false);

        sendButton.setButtonText (tr ("Send"));
        sendButton.setColour (juce::TextButton::buttonColourId, theme::colour::accent);
        sendButton.setColour (juce::TextButton::textColourOffId, theme::colour::onAccent);
        showButton.setButtonText (tr ("Show what will be sent"));
        cancelButton.setButtonText (manual ? tr ("Cancel") : tr ("Don't send"));
        githubButton.setButtonText (tr ("Report on GitHub instead"));
        folderButton.setButtonText (tr ("Open the folder with the report"));
        for (auto* b : { &sendButton, &showButton, &cancelButton, &githubButton, &folderButton })
            addAndMakeVisible (*b);

        sendButton.onClick = [this] { finish (true); };
        cancelButton.onClick = [this] { finish (false); };
        showButton.onClick = [this] {
            previewShown = ! previewShown;
            showButton.setButtonText (previewShown ? tr ("Hide the report") : tr ("Show what will be sent"));
            refreshPreview();
            resizeToFit();
        };
        githubButton.onClick = [this] {
            juce::Process::openDocument (githubUrl (input), {});
            reveal();
            applyLocalChoices();
            if (! manual)
                close();
        };
        folderButton.onClick = [this] { reveal(); };

        preview.setMultiLine (true, false);
        preview.setReadOnly (true);
        preview.setScrollbarsShown (true);
        preview.setFont (juce::Font (theme::monoFont (theme::type::small)));
        preview.setColour (juce::TextEditor::backgroundColourId, theme::colour::bg0);
        addChildComponent (preview);
        setSize (640, 10);
        resizeToFit();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        message.setBounds (r.removeFromTop (messageHeight()));
        r.removeFromTop (8);
        if (manual)
        {
            descriptionLabel.setBounds (r.removeFromTop (20));
            description.setBounds (r.removeFromTop (90));
            r.removeFromTop (6);
            emailLabel.setBounds (r.removeFromTop (20));
            email.setBounds (r.removeFromTop (26));
            r.removeFromTop (8);
            attach.setBounds (r.removeFromTop (24));
        }
        if (separatePlugin.isNotEmpty())
            separate.setBounds (r.removeFromTop (24));
        fileNames.setBounds (r.removeFromTop (24));
        if (! manual)
            always.setBounds (r.removeFromTop (24));
        r.removeFromTop (10);
        auto buttons = r.removeFromTop (30);
        sendButton.setBounds (buttons.removeFromLeft (110));
        buttons.removeFromLeft (8);
        showButton.setBounds (buttons.removeFromLeft (200));
        buttons.removeFromLeft (8);
        cancelButton.setBounds (buttons.removeFromLeft (110));
        r.removeFromTop (8);
        auto links = r.removeFromTop (26);
        githubButton.setBounds (links.removeFromLeft (220));
        links.removeFromLeft (8);
        folderButton.setBounds (links.removeFromLeft (240));
        if (previewShown)
        {
            r.removeFromTop (10);
            preview.setBounds (r);
        }
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::colour::bg2); }

private:
    int messageHeight() const
    {
        juce::AttributedString text;
        text.append (message.getText(), message.getFont(), theme::colour::textPrimary);
        juce::TextLayout layout;
        layout.createLayout (text, (float) getWidth() - 32.0f);
        return (int) std::ceil (layout.getHeight()) + 6;
    }

    void resizeToFit()
    {
        const int rows = (manual ? 20 + 90 + 6 + 20 + 26 + 8 + 24 : 0) + (separatePlugin.isNotEmpty() ? 24 : 0) + 24 + (manual ? 0 : 24);
        const int height = 16 + messageHeight() + 8 + rows + 10 + 30 + 8 + 26 + (previewShown ? 10 + 320 : 0) + 16;
        setSize (getWidth(), height);
        preview.setVisible (previewShown);
        if (auto* w = findParentComponentOfClass<juce::DialogWindow>())
            w->setContentComponentSize (getWidth(), height);
    }

    void refreshInput()
    {
        Session s;
        if (attach.getToggleState())
        {
            // this session's steps and plugins, and the newest crash / freeze files of the last 7 days
            const auto own = platform::settingsFile ("sessions").getChildFile ("session-" + juce::String (crashguard::processId()) + ".txt");
            s = parseSession (own.loadFileAsString());
            const auto week = juce::Time::getCurrentTime() - juce::RelativeTime::days (7);
            for (const auto* pattern : { "crash *.txt", "hang *.txt" })
            {
                auto files = platform::settingsFile ("autosave").findChildFiles (juce::File::findFiles, false, pattern);
                filesort::byTime (files, filesort::By::modificationTime, true);
                if (! files.isEmpty() && files[0].getLastModificationTime() > week)
                    (juce::String (pattern).startsWith ("crash") ? s.crashFile : s.hangFile) = files[0];
            }
        }
        const auto id = input.id, created = input.created;
        input = makeInput (s, "manual", devices);
        input.id = id;
        input.created = created;
        input.userText = utf8 (description.getText());
        input.email = utf8 (email.getText().trim());
        session = s;
        refreshPreview();
    }

    void refreshPreview()
    {
        json = buildJson (input, fileNames.getToggleState());
        if (previewShown)
            preview.setText (json, false);
    }

    void reveal()
    {
        const auto file = session.crashFile.existsAsFile() ? session.crashFile : session.hangFile.existsAsFile() ? session.hangFile : juce::File();
        if (file.existsAsFile())
            file.revealToUser();
        else
            platform::settingsFile ("autosave").startAsProcess();
    }

    // what the dialog decides besides sending (the separate-process offer, "always send")
    void applyLocalChoices()
    {
        if (separatePlugin.isNotEmpty() && separate.getToggleState() && onSeparate)
            onSeparate();
        separatePlugin.clear();
    }

    void finish (bool sendIt)
    {
        applyLocalChoices();
        if (sendIt)
        {
            if (always.getToggleState())
                setConsent (cr::Consent::always);
            if (json.isEmpty())
                refreshPreview();
            const auto id = fromUtf8 (input.id);
            send (json, id, [] (bool ok, const juce::String& what) {
                ui::postHint ({ ok ? tr ("Crash report sent, thank you (id {0})", { what })
                                   : tr ("The report could not be sent ({0}); LYNX tries again at the next start", { what }),
                                {}, false, false },
                              8000);
            });
        }
        close();
    }

    void close()
    {
        if (auto* w = findParentComponentOfClass<juce::DialogWindow>())
            w->exitModalState (0);
    }

    Session session;
    juce::String separatePlugin;
    std::function<void()> onSeparate;
    juce::AudioDeviceManager* devices;
    bool manual, previewShown = false;
    cr::ReportInput input;
    juce::String json;
    juce::Label message, descriptionLabel, emailLabel;
    juce::TextEditor description, email, preview;
    juce::ToggleButton separate, fileNames, always, attach;
    juce::TextButton sendButton, showButton, cancelButton, githubButton, folderButton;
};

void launch (std::unique_ptr<ReportPanel> panel, const juce::String& title)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (fitContentToScreen (std::move (panel)).release());
    options.dialogTitle = title;
    options.dialogBackgroundColour = theme::colour::bg2;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.escapeKeyTriggersCloseButton = true;
    if (auto* dialog = options.launchAsync())
        keepAboveMainWindow (*dialog);
}
} // namespace

void showStartupDialog (const Session& session, const juce::String& notice, const juce::String& separatePlugin, std::function<void()> onSeparate,
                        juce::AudioDeviceManager* devices)
{
    launch (std::make_unique<ReportPanel> (session, notice, separatePlugin, std::move (onSeparate), devices, false), tr ("LYNX closed unexpectedly"));
}

void showManualReport (juce::AudioDeviceManager* devices)
{
    launch (std::make_unique<ReportPanel> (Session {}, juce::String(), juce::String(), nullptr, devices, true), tr ("Report a problem"));
}

std::unique_ptr<juce::Component> makeDialogForSnapshot (bool manual)
{
    Session s;
    s.version = JUCE_APPLICATION_VERSION_STRING;
    s.steps = { "00:05:37 loaded plugin Example", "00:07:59 opening the editor of Example", "00:08:00 crashed: backtrace in autosave\\crash example.txt" };
    if (manual)
        return std::make_unique<ReportPanel> (s, juce::String(), juce::String(), nullptr, nullptr, true);
    return std::make_unique<ReportPanel> (s, tr ("The last session ended without closing normally. Last step: {0}. Backup copies of your project are in "
                                                 "LYNX's autosave folder; details in crash-history.log.",
                                                 { "opening the editor of Example" }),
                                          "Example", nullptr, nullptr, false);
}
} // namespace lynx::crashreporter
