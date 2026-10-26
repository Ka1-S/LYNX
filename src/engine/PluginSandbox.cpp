#include "engine/PluginSandbox.h"

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

#include <juce_gui_extra/juce_gui_extra.h>

#include "core/Denormals.h"
#include "engine/PluginHost.h"
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/WindowOwner.h"

#if JUCE_WINDOWS
 #include <windows.h>
#elif JUCE_LINUX || JUCE_BSD
 #include <cerrno>
 #include <ctime>
 #include <fcntl.h>
 #include <semaphore.h>
 #include <sys/mman.h>
 #include <unistd.h>
 #if JUCE_LINUX
  #include <poll.h>
  #include <sys/eventfd.h>
  #include <sys/syscall.h>
  #include <thread>
  #include <dlfcn.h>
 #endif
 #define LYNX_POSIX_SANDBOX 1
#endif

namespace lynx
{
bool pluginSandboxEnabled()
{
    return PluginHost::settingsFolder().getChildFile ("sandbox.txt").loadFileAsString().trim() == "1";
}

void setPluginSandboxEnabled (bool enabled)
{
    PluginHost::settingsFolder().getChildFile ("sandbox.txt").replaceWithText (enabled ? "1" : "0");
}

namespace
{
juce::File separateListFile() { return PluginHost::settingsFolder().getChildFile ("separate-process.txt"); }
juce::StringArray separateList()
{
    auto lines = juce::StringArray::fromLines (separateListFile().loadFileAsString());
    lines.trim();
    lines.removeEmptyStrings();
    return lines;
}
} // namespace

bool pluginAlwaysSeparate (const juce::String& name)
{
    return name.isNotEmpty() && separateList().contains (name.trim(), true);
}

void setPluginAlwaysSeparate (const juce::String& name, bool on)
{
    if (name.trim().isEmpty())
        return;
    auto list = separateList();
    list.removeString (name.trim(), true);
    if (on)
        list.add (name.trim());
    separateListFile().replaceWithText (list.joinIntoString ("\n") + (list.isEmpty() ? "" : "\n"));
}

namespace
{
std::atomic<bool> showAlerts { true };

int processTag()
{
    static const int tag = juce::Random::getSystemRandom().nextInt (1000000);
    return tag;
}

// diagnostics: LYNX_SANDBOX_LOG=file logs the steps (one file per process)
void trace (const juce::String& what)
{
    if (const auto path = juce::SystemStats::getEnvironmentVariable ("LYNX_SANDBOX_LOG", {}); path.isNotEmpty())
        juce::File (path + "." + juce::String (processTag())).appendText (juce::String (juce::Time::getMillisecondCounter()) + " " + what + "\n");
}
}

void setPluginSandboxAlerts (bool show)
{
    showAlerts = show;
}

#if JUCE_WINDOWS || LYNX_POSIX_SANDBOX
namespace
{
// Block shared between LYNX and the child. The parent writes the input and signals `request`; the child
// processes in place and signals `response`.
struct SharedBlock
{
    static constexpr std::uint32_t magicValue = 0x4C594E58; // "LYNX"
    static constexpr int maxChannels = 8, maxFrames = PluginHost::maxBlockSize, maxMidi = 1024, maxParams = 512;

    struct Midi
    {
        std::int32_t offset;
        std::uint8_t size, bytes[3];
    };
    struct Param
    {
        std::int32_t index;
        float value;
    };

    std::uint32_t magic;
    std::int32_t frames, channels, midiCount, paramCount, paramOutCount, latency;
    Midi midi[maxMidi];
    Param params[maxParams], paramsOut[maxParams];
    float audio[maxChannels][maxFrames];
};

// The shared block plus the two "doorbells" (request: parent -> child, response: child -> parent).
// Windows: a named file mapping and two auto-reset events. Linux/BSD (round 161): a POSIX shared-memory object
// (/dev/shm) holding the block and two process-shared semaphores.
class SandboxChannel
{
public:
    SandboxChannel() = default;
    SandboxChannel (const SandboxChannel&) = delete;
    SandboxChannel& operator= (const SandboxChannel&) = delete;
    ~SandboxChannel() { close(); }

    // parent: creates the objects under `base`
    bool create (const juce::String& base)
    {
        name = base;
       #if JUCE_WINDOWS
        mapping = CreateFileMappingW (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD) sizeof (SharedBlock),
                                      wide (objectName ("-mem")).c_str());
        if (mapping != nullptr)
            shared = static_cast<SharedBlock*> (MapViewOfFile (mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof (SharedBlock)));
        request = CreateEventW (nullptr, FALSE, FALSE, wide (objectName ("-req")).c_str());
        response = CreateEventW (nullptr, FALSE, FALSE, wide (objectName ("-resp")).c_str());
        if (shared == nullptr || request == nullptr || response == nullptr)
            return false;
        std::memset (shared, 0, sizeof (SharedBlock));
       #else
        const auto path = shmPath();
        ::shm_unlink (path.toRawUTF8()); // a leftover of a crashed LYNX with the same pid
        const int fd = ::shm_open (path.toRawUTF8(), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0)
            return false;
        owner = true;
        const bool sized = ::ftruncate (fd, (off_t) sizeof (Posix)) == 0;
        void* p = sized ? ::mmap (nullptr, sizeof (Posix), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
        ::close (fd);
        if (p == MAP_FAILED)
            return false;
        posix = static_cast<Posix*> (p);
        std::memset (&posix->block, 0, sizeof (SharedBlock));
        if (::sem_init (&posix->request, 1, 0) != 0 || ::sem_init (&posix->response, 1, 0) != 0)
            return false;
        semaphores = true;
        shared = &posix->block;
       #endif
        shared->magic = SharedBlock::magicValue;
        return true;
    }

    // child: opens the parent's objects
    bool open (const juce::String& base)
    {
        name = base;
       #if JUCE_WINDOWS
        mapping = OpenFileMappingW (FILE_MAP_ALL_ACCESS, FALSE, wide (objectName ("-mem")).c_str());
        if (mapping != nullptr)
            shared = static_cast<SharedBlock*> (MapViewOfFile (mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof (SharedBlock)));
        request = OpenEventW (SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, wide (objectName ("-req")).c_str());
        response = OpenEventW (SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, wide (objectName ("-resp")).c_str());
        if (request == nullptr || response == nullptr)
            return false;
       #else
        const int fd = ::shm_open (shmPath().toRawUTF8(), O_RDWR, 0600);
        if (fd < 0)
            return false;
        void* p = ::mmap (nullptr, sizeof (Posix), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close (fd);
        if (p == MAP_FAILED)
            return false;
        posix = static_cast<Posix*> (p);
        shared = &posix->block;
       #endif
        return shared != nullptr && shared->magic == SharedBlock::magicValue;
    }

    // parent, once the child has mapped the block: the name is no longer needed (nothing left in /dev/shm if LYNX dies)
    void unlinkName()
    {
       #if LYNX_POSIX_SANDBOX
        if (owner && ! unlinked)
            unlinked = ::shm_unlink (shmPath().toRawUTF8()) == 0 || errno == ENOENT;
       #endif
    }

    SharedBlock* block() const noexcept { return shared; }

    void signalRequest() noexcept { post (true); }
    void signalResponse() noexcept { post (false); }
    bool waitRequest (int timeoutMs) noexcept { return wait (true, timeoutMs); }
    bool waitResponse (int timeoutMs) noexcept { return wait (false, timeoutMs); }

private:
   #if JUCE_WINDOWS
    juce::String objectName (const char* suffix) const { return "Local\\" + name + suffix; }
    static std::wstring wide (const juce::String& s) { return std::wstring (s.toWideCharPointer()); }

    void post (bool req) noexcept { SetEvent (req ? request : response); }
    bool wait (bool req, int timeoutMs) noexcept { return WaitForSingleObject (req ? request : response, (DWORD) timeoutMs) == WAIT_OBJECT_0; }

    void close()
    {
        if (shared != nullptr)
            UnmapViewOfFile (shared);
        for (auto* h : { mapping, request, response })
            if (h != nullptr)
                CloseHandle (h);
        shared = nullptr;
        mapping = request = response = nullptr;
    }

    HANDLE mapping = nullptr, request = nullptr, response = nullptr;
   #else
    struct Posix
    {
        sem_t request, response;
        SharedBlock block;
    };

    juce::String shmPath() const { return "/" + name; }

    void post (bool req) noexcept { ::sem_post (req ? &posix->request : &posix->response); }

    bool wait (bool req, int timeoutMs) noexcept
    {
        auto* sem = req ? &posix->request : &posix->response;
        timespec until {};
        ::clock_gettime (CLOCK_MONOTONIC, &until);
        until.tv_sec += timeoutMs / 1000;
        until.tv_nsec += (long) (timeoutMs % 1000) * 1000000L;
        if (until.tv_nsec >= 1000000000L)
        {
            ++until.tv_sec;
            until.tv_nsec -= 1000000000L;
        }
        for (;;)
        {
            if (::sem_clockwait (sem, CLOCK_MONOTONIC, &until) == 0)
                return true;
            if (errno != EINTR)
                return false;
        }
    }

    void close()
    {
        if (posix != nullptr)
        {
            if (semaphores)
            {
                ::sem_destroy (&posix->request);
                ::sem_destroy (&posix->response);
            }
            ::munmap (posix, sizeof (Posix));
        }
        unlinkName();
        posix = nullptr;
        shared = nullptr;
    }

    Posix* posix = nullptr;
    bool owner = false, unlinked = false, semaphores = false;
   #endif
    juce::String name;
    SharedBlock* shared = nullptr;
};

juce::MemoryBlock encode (const juce::ValueTree& message)
{
    juce::MemoryOutputStream out;
    message.writeToStream (out);
    return out.getMemoryBlock();
}

// Parameter-change queue with multiple producers (UI and audio thread) and one consumer.
class ParamQueue
{
public:
    void push (int index, float value) noexcept
    {
        const juce::SpinLock::ScopedLockType lock (spin);
        if (count < (int) items.size())
            items[(std::size_t) count++] = { index, value };
    }

    int drain (SharedBlock::Param* out, int max) noexcept
    {
        const juce::SpinLock::ScopedLockType lock (spin);
        const auto n = std::min (count, max);
        std::memcpy (out, items.data(), sizeof (SharedBlock::Param) * (std::size_t) n);
        std::memmove (items.data(), items.data() + n, sizeof (SharedBlock::Param) * (std::size_t) (count - n));
        count -= n;
        return n;
    }

private:
    juce::SpinLock spin;
    std::array<SharedBlock::Param, 4096> items {};
    int count = 0;
};

// ---------------------------------------------------------------------------
// LYNX side: connection to the child, shared memory and events.

class SandboxSession final : private juce::ChildProcessCoordinator
{
public:
    SandboxSession()
    {
        static std::atomic<int> counter { 0 };
       #if JUCE_WINDOWS
        const auto pid = (int) GetCurrentProcessId();
       #else
        const auto pid = (int) ::getpid();
       #endif
        baseName = "LYNX-sandbox-" + juce::String (pid) + "-" + juce::String (++counter);
        created = channel.create (baseName);
    }

    ~SandboxSession() override
    {
        closing = true;
        stopWatching();
        killWorkerProcess();
    }

    // Linux (round 161): the moment the child process ends (a crash, abort, kill), the plugin is marked dead; before,
    // a crash was noticed only by the audio block's 0.5 s timeout or the connection's own ping
    void watchChild (int pid)
    {
       #if JUCE_LINUX && defined(SYS_pidfd_open)
        if (pid <= 0 || watcher.joinable())
            return;
        const int fd = (int) ::syscall (SYS_pidfd_open, (pid_t) pid, 0);
        if (fd < 0)
            return;
        stopFd = ::eventfd (0, EFD_CLOEXEC);
        if (stopFd < 0)
        {
            ::close (fd);
            return;
        }
        watcher = std::thread ([this, fd] {
            pollfd fds[2] = { { fd, POLLIN, 0 }, { stopFd, POLLIN, 0 } };
            while (::poll (fds, 2, -1) < 0 && errno == EINTR)
            {
            }
            if ((fds[0].revents & POLLIN) != 0)
            {
                trace ("parent: child process ended");
                markDead();
            }
            ::close (fd);
        });
       #else
        juce::ignoreUnused (pid);
       #endif
    }

    bool start()
    {
        if (! created)
            return false;
        const auto launched = launchWorkerProcess (juce::File::getSpecialLocation (juce::File::currentExecutableFile), pluginSandboxId, 10000);
        trace ("parent: child launched " + juce::String ((int) launched));
        return launched;
    }

    const juce::String& name() const noexcept { return baseName; }
    SharedBlock* block() noexcept { return channel.block(); }
    void childAttached() { channel.unlinkName(); }
    bool isDead() const noexcept { return dead.load(); }

    void markDead()
    {
        if (dead.exchange (true))
            return;
        replyReady.signal();
        channel.signalResponse(); // an audio thread waiting for a block that will never come returns now (round 161)
        if (closing || ! showAlerts)
            return;
        juce::MessageManager::callAsync ([title = pluginName] {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, lynx::tr ("Plugin"),
                                                    lynx::tr ("{0} stopped responding (separate process). "
                                                              "It has been muted; save the project and reopen it to reload.", { title }));
        });
    }

    void setPluginName (const juce::String& n) { pluginName = n; }

    // request with reply (UI thread)
    juce::ValueTree ask (const juce::ValueTree& message, int timeoutMs)
    {
        const std::scoped_lock lock (askMutex);
        if (dead)
            return {};
        {
            const std::scoped_lock l (replyMutex);
            reply = {};
        }
        replyReady.reset();
        if (! sendMessageToWorker (encode (message)))
            return {};
        if (! replyReady.wait (timeoutMs))
        {
            markDead();
            return {};
        }
        const std::scoped_lock l (replyMutex);
        return reply;
    }

    void tell (const juce::ValueTree& message)
    {
        if (! dead)
            sendMessageToWorker (encode (message));
    }

    // one audio block (audio thread): false = the child didn't respond
    bool runBlock (int timeoutMs) noexcept
    {
        if (dead)
            return false;
        channel.signalRequest();
        if (! channel.waitResponse (timeoutMs))
        {
            markDead();
            return false;
        }
        return ! dead; // woken by markDead (the child crashed or the connection was lost)
    }

private:
    void handleMessageFromWorker (const juce::MemoryBlock& data) override
    {
        {
            const std::scoped_lock l (replyMutex);
            reply = juce::ValueTree::readFromData (data.getData(), data.getSize());
        }
        trace ("parent: reply received " + reply.getType().toString());
        replyReady.signal();
    }

    void handleConnectionLost() override
    {
        trace ("parent: connection lost");
        markDead();
    }

    void stopWatching()
    {
       #if JUCE_LINUX
        if (watcher.joinable())
        {
            const std::uint64_t one = 1;
            juce::ignoreUnused (::write (stopFd, &one, sizeof (one)));
            watcher.join();
        }
        if (stopFd >= 0)
            ::close (stopFd);
        stopFd = -1;
       #endif
    }

    juce::String baseName, pluginName;
    SandboxChannel channel;
    bool created = false;
   #if JUCE_LINUX
    std::thread watcher;
    int stopFd = -1;
   #endif
    std::atomic<bool> dead { false }, closing { false };
    std::mutex askMutex, replyMutex;
    juce::ValueTree reply;
    juce::WaitableEvent replyReady;
};

class RemotePlugin;

class RemoteParameter final : public juce::AudioPluginInstance::HostedParameter
{
public:
    RemoteParameter (RemotePlugin& o, int i, const juce::ValueTree& info)
        : owner (o), index (i),
          id (info["id"].toString()), name (info["name"].toString()), label (info["label"].toString()),
          defaultValue ((float) (double) info["default"]), steps ((int) info["steps"]), discrete ((bool) info["discrete"]),
          value ((float) (double) info["value"])
    {
    }

    float getValue() const override { return value.load(); }
    void setValue (float v) override;
    float getDefaultValue() const override { return defaultValue; }
    juce::String getName (int maximumLength) const override { return name.substring (0, maximumLength); }
    juce::String getLabel() const override { return label; }
    float getValueForText (const juce::String& text) const override { return juce::jlimit (0.0f, 1.0f, text.getFloatValue()); }
    juce::String getText (float v, int) const override { return juce::String (v, 3); }
    int getNumSteps() const override { return steps > 0 ? steps : AudioProcessorParameter::getNumSteps(); }
    bool isDiscrete() const override { return discrete; }
    juce::String getParameterID() const override { return id; }

    void setFromWorker (float v)
    {
        value = v;
        sendValueChangedMessageToListeners (v);
    }

private:
    RemotePlugin& owner;
    const int index;
    const juce::String id, name, label;
    const float defaultValue;
    const int steps;
    const bool discrete;
    std::atomic<float> value;
};

// LYNX's stand-in for a sandboxed plugin's editor (inside LYNX's plugin window). The plugin's own window lives in the
// child process: it is shown, owned by LYNX's plugin window, while this stand-in is on screen, and hidden when the stand-in
// is not (the Wrapper settings tab, the window closed or the plugin removed; issues #8 / #9).
class RemoteEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    RemoteEditor (juce::AudioProcessor& p, std::function<void (juce::int64)> show, std::function<void()> hide)
        : AudioProcessorEditor (p), showWindow (std::move (show)), hideWindow (std::move (hide))
    {
        text.setText (tr ("This plugin runs in a separate process: its interface opens in its own window."),
                      juce::dontSendNotification);
        text.setJustificationType (juce::Justification::centred);
        button.setButtonText (tr ("Show the plugin window"));
        button.onClick = [this] { showWindow (ownerHandle()); };
        addAndMakeVisible (text);
        addAndMakeVisible (button);
        setSize (380, 96);
        startTimerHz (4); // (a window shown, minimised or restored tells its children nothing: polled)
    }

    ~RemoteEditor() override
    {
        stopTimer();
        if (shown)
            hideWindow();
    }

    void visibilityChanged() override { follow(); }
    void timerCallback() override { follow(); }
    void parentHierarchyChanged() override { follow(); }

    void paint (juce::Graphics& g) override { g.fillAll (theme::colour::bg1); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        button.setBounds (r.removeFromBottom (28).withSizeKeepingCentre (220, 28));
        text.setBounds (r);
    }

private:
    // LYNX's main window (HWND, X11 Window id): the child's window is kept above it, not above every program. (Not the
    // plugin window holding this stand-in: Windows destroys an owned window with its owner, even in another process.)
    juce::int64 ownerHandle() { return (juce::int64) (juce::pointer_sized_int) mainWindowHandle(); }

    void follow()
    {
        const auto showing = isShowing() && getPeer() != nullptr;
        if (showing == shown)
            return;
        shown = showing;
        if (shown)
            showWindow (ownerHandle());
        else
            hideWindow();
    }

    std::function<void (juce::int64)> showWindow;
    std::function<void()> hideWindow;
    bool shown = false;
    juce::Label text;
    juce::TextButton button;
};

class RemotePlugin final : public juce::AudioPluginInstance
{
public:
    RemotePlugin (std::unique_ptr<SandboxSession> s, const juce::PluginDescription& d, const juce::ValueTree& info)
        : AudioPluginInstance (busesFor ((int) info["ins"], (int) info["outs"])),
          session (std::move (s)), description (d),
          midiIn ((bool) info["midiIn"]), midiOut ((bool) info["midiOut"]), editor ((bool) info["editor"]),
          tail ((double) info["tail"]), lastState (info["state"].toString())
    {
        session->setPluginName (description.name);
        setLatencySamples ((int) info["latency"]);
        const auto params = info.getChildWithName ("params");
        for (int i = 0; i < params.getNumChildren(); ++i)
        {
            auto p = std::make_unique<RemoteParameter> (*this, i, params.getChild (i));
            remoteParams.push_back (p.get());
            addHostedParameter (std::move (p));
        }
    }

    ~RemotePlugin() override { session.reset(); }

    bool dead() const noexcept { return session->isDead(); }
    void hangForTest() { session->tell (juce::ValueTree ("hang")); }
    void crashForTest() { session->tell (juce::ValueTree ("crash")); }

    void queueParameter (int index, float value) noexcept { pending.push (index, value); }

    void fillInPluginDescription (juce::PluginDescription& d) const override { d = description; }
    const juce::String getName() const override { return description.name; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    double getTailLengthSeconds() const override { return tail; }
    bool acceptsMidi() const override { return midiIn; }
    bool producesMidi() const override { return midiOut; }
    bool hasEditor() const override { return editor; }
    juce::AudioProcessorEditor* createEditor() override
    {
        return new RemoteEditor (
            *this, [this] (juce::int64 owner) { session->tell (juce::ValueTree ("showEditor").setProperty ("owner", owner, nullptr)); },
            [this] { session->tell (juce::ValueTree ("hideEditor")); });
    }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        if (const auto r = session->ask (juce::ValueTree ("getState"), 10000); r.isValid())
            lastState = r["state"].toString();
        dest.fromBase64Encoding (lastState);
    }

    void setStateInformation (const void* data, int size) override
    {
        const juce::MemoryBlock block (data, (std::size_t) juce::jmax (0, size));
        lastState = block.toBase64Encoding();
        session->ask (juce::ValueTree ("setState").setProperty ("state", lastState, nullptr), 10000);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        auto* shared = session->block();
        const auto frames = std::min (buffer.getNumSamples(), SharedBlock::maxFrames);
        const auto channels = std::min (buffer.getNumChannels(), SharedBlock::maxChannels);
        if (session->isDead() || shared == nullptr || frames <= 0)
        {
            buffer.clear();
            return;
        }

        shared->frames = frames;
        shared->channels = channels;
        for (int c = 0; c < channels; ++c)
            std::memcpy (shared->audio[c], buffer.getReadPointer (c), sizeof (float) * (std::size_t) frames);

        int count = 0;
        for (const auto m : midi)
            if (m.numBytes >= 1 && m.numBytes <= 3 && count < SharedBlock::maxMidi)
            {
                auto& e = shared->midi[count++];
                e.offset = juce::jlimit (0, frames - 1, m.samplePosition);
                e.size = (std::uint8_t) m.numBytes;
                std::memcpy (e.bytes, m.data, (std::size_t) m.numBytes);
            }
        shared->midiCount = count;
        shared->paramCount = pending.drain (shared->params, SharedBlock::maxParams);
        shared->paramOutCount = 0;

        // timeout: 0.5 s (a healthy plugin responds in well under one block)
        if (! session->runBlock (500))
        {
            buffer.clear();
            return;
        }

        for (int c = 0; c < channels; ++c)
            std::memcpy (buffer.getWritePointer (c), shared->audio[c], sizeof (float) * (std::size_t) frames);
        for (int c = channels; c < buffer.getNumChannels(); ++c)
            buffer.clear (c, 0, frames);

        for (int i = 0; i < std::min (shared->paramOutCount, SharedBlock::maxParams); ++i)
            if (const auto k = shared->paramsOut[i].index; k >= 0 && k < (int) remoteParams.size())
                remoteParams[(std::size_t) k]->setFromWorker (shared->paramsOut[i].value);

        if (shared->latency != getLatencySamples())
            setLatencySamples (shared->latency);
    }

private:
    static BusesProperties busesFor (int ins, int outs)
    {
        auto set = [] (int n) { return n <= 8 ? juce::AudioChannelSet::canonicalChannelSet (n) : juce::AudioChannelSet::discreteChannels (n); };
        BusesProperties buses;
        if (ins > 0)
            buses = buses.withInput ("Input", set (ins), true);
        if (outs > 0)
            buses = buses.withOutput ("Output", set (outs), true);
        return buses;
    }

    std::unique_ptr<SandboxSession> session;
    juce::PluginDescription description;
    bool midiIn, midiOut, editor;
    double tail;
    juce::String lastState;
    std::vector<RemoteParameter*> remoteParams;
    ParamQueue pending;
};

void RemoteParameter::setValue (float v)
{
    value = v;
    owner.queueParameter (index, v);
}

// ---------------------------------------------------------------------------
// Child side.

class SandboxWindow final : public juce::DocumentWindow
{
public:
    explicit SandboxWindow (const juce::String& title)
        : DocumentWindow (title, theme::colour::bg1, DocumentWindow::closeButton | DocumentWindow::minimiseButton)
    {
        setUsingNativeTitleBar (true);
        setIcon (logoImage());
    }

    // Keeps the window above LYNX's main window (`owner`: its HWND / X11 Window id, from the other process) instead of
    // above every program: it was always on top, covering LYNX's Wrapper settings and other apps (issue #8). False when
    // the platform can't (then it stays always on top, as before). Windows: not done — an owner in another process
    // attaches the two processes' input queues, so a plugin that hangs its window would freeze LYNX's too; there the
    // window stays on top but is hidden whenever LYNX's stand-in is not on screen (Wrapper settings, minimised, closed).
    bool setOwner (juce::int64 owner)
    {
        if (owner == 0 || getWindowHandle() == nullptr)
            return false;
       #if JUCE_LINUX
        // WM_TRANSIENT_FOR across processes (X11 / XWayland), through our own connection to the display
        using OpenDisplay = void* (*) (const char*);
        using SetTransient = int (*) (void*, unsigned long, unsigned long);
        using Flush = int (*) (void*);
        static void* lib = dlopen ("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        if (lib == nullptr)
            return false;
        static auto* openDisplay = (OpenDisplay) dlsym (lib, "XOpenDisplay");
        static auto* setTransient = (SetTransient) dlsym (lib, "XSetTransientForHint");
        static auto* flush = (Flush) dlsym (lib, "XFlush");
        static void* display = openDisplay != nullptr ? openDisplay (nullptr) : nullptr;
        if (display == nullptr || setTransient == nullptr || flush == nullptr)
            return false;
        setTransient (display, (unsigned long) (juce::pointer_sized_int) getWindowHandle(), (unsigned long) owner);
        flush (display);
        return true;
       #else
        return false;
       #endif
    }

    void closeButtonPressed() override { setVisible (false); }
};

class SandboxWorker final : public juce::ChildProcessWorker,
                            private juce::Thread,
                            private juce::AudioProcessorParameter::Listener
{
public:
    SandboxWorker() : Thread ("LYNX sandbox audio") {}

    ~SandboxWorker() override
    {
        stopThread (2000);
        window.reset();
        if (plugin != nullptr)
            for (auto* p : plugin->getParameters())
                p->removeListener (this);
        plugin.reset();
    }

    void handleMessageFromCoordinator (const juce::MemoryBlock& data) override
    {
        auto message = juce::ValueTree::readFromData (data.getData(), data.getSize());
        trace ("child: message " + message.getType().toString());
        juce::MessageManager::callAsync ([this, message] { handle (message); });
    }

    void handleConnectionLost() override
    {
        trace ("child: connection lost");
        juce::MessageManager::callAsync ([] { juce::JUCEApplicationBase::quit(); });
    }

private:
    void send (const juce::ValueTree& message) { sendMessageToCoordinator (encode (message)); }

    void handle (const juce::ValueTree& message)
    {
        if (message.hasType ("open"))
            send (open (message));
        else if (message.hasType ("getState"))
        {
            juce::ValueTree reply ("state");
            if (plugin != nullptr)
                reply.setProperty ("state", juce::String (capturePluginState (*plugin)), nullptr);
            send (reply);
        }
        else if (message.hasType ("setState"))
        {
            if (plugin != nullptr)
            {
                juce::MemoryBlock state;
                if (state.fromBase64Encoding (message["state"].toString()))
                    plugin->setStateInformation (state.getData(), (int) state.getSize());
            }
            send (juce::ValueTree ("ok"));
        }
        else if (message.hasType ("hang"))
            hang = true;
        else if (message.hasType ("crash")) // only in the crash-isolation test
            std::abort();
        else if (message.hasType ("showEditor") && plugin != nullptr && plugin->hasEditor())
        {
            if (window == nullptr)
            {
                window = std::make_unique<SandboxWindow> (plugin->getName());
                if (auto* ed = plugin->createEditorIfNeeded())
                    window->setContentOwned (ed, true);
                window->centreWithSize (window->getWidth(), window->getHeight());
            }
            if (! window->isOnDesktop())
                window->addToDesktop(); // (the native window exists before it is shown: its owner is set first)
            window->setAlwaysOnTop (! window->setOwner ((juce::int64) message["owner"]));
            window->setVisible (true);
            window->toFront (true);
        }
        else if (message.hasType ("hideEditor") && window != nullptr)
            window->setVisible (false);
    }

    juce::ValueTree open (const juce::ValueTree& message)
    {
        juce::ValueTree reply ("opened");
        auto fail = [&] (const juce::String& why) {
            reply.setProperty ("error", why, nullptr);
            return reply;
        };

        const auto base = message["shm"].toString();
        if (! channel.open (base))
            return fail ("shared memory unavailable");
        shared = channel.block();

        juce::PluginDescription description;
        const auto xml = juce::parseXML (message["description"].toString());
        if (xml == nullptr || ! description.loadFromXml (*xml))
            return fail ("invalid plugin description");

        const auto sampleRate = (double) message["sampleRate"];
        blockSize = juce::jlimit (16, SharedBlock::maxFrames, (int) message["blockSize"]);
        PluginHost host (false);
        if (description.pluginFormatName == "LV2")
            host.findLv2Plugins(); // (an LV2 URI from a folder outside JUCE's two default ones; round 161)
        juce::String error;
        plugin = createRightInstance (host.getFormats(), description, sampleRate, blockSize, error);
        if (plugin == nullptr)
            return fail (error.isEmpty() ? juce::String ("the plugin failed to open") : error);

        preparePlugin (*plugin, sampleRate, blockSize);
        if (const auto state = message["state"].toString(); state.isNotEmpty())
        {
            juce::MemoryBlock block;
            if (block.fromBase64Encoding (state))
                plugin->setStateInformation (block.getData(), (int) block.getSize());
        }

        reply.setProperty ("ins", plugin->getTotalNumInputChannels(), nullptr);
        reply.setProperty ("outs", plugin->getTotalNumOutputChannels(), nullptr);
        reply.setProperty ("midiIn", plugin->acceptsMidi(), nullptr);
        reply.setProperty ("midiOut", plugin->producesMidi(), nullptr);
        reply.setProperty ("editor", plugin->hasEditor(), nullptr);
        reply.setProperty ("tail", plugin->getTailLengthSeconds(), nullptr);
        reply.setProperty ("latency", plugin->getLatencySamples(), nullptr);
       #if LYNX_POSIX_SANDBOX
        reply.setProperty ("pid", (int) ::getpid(), nullptr);
       #endif
        reply.setProperty ("state", juce::String (capturePluginState (*plugin)), nullptr);

        juce::ValueTree params ("params");
        const auto& list = plugin->getParameters();
        for (int i = 0; i < list.size(); ++i)
        {
            auto* p = list[i];
            juce::ValueTree info ("p");
            info.setProperty ("name", p->getName (128), nullptr);
            info.setProperty ("label", p->getLabel(), nullptr);
            info.setProperty ("default", p->getDefaultValue(), nullptr);
            info.setProperty ("value", p->getValue(), nullptr);
            info.setProperty ("steps", p->getNumSteps(), nullptr);
            info.setProperty ("discrete", p->isDiscrete(), nullptr);
            if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (p))
                info.setProperty ("id", hosted->getParameterID(), nullptr);
            else
                info.setProperty ("id", juce::String (i), nullptr);
            params.appendChild (info, nullptr);
            p->addListener (this);
        }
        reply.appendChild (params, nullptr);

        channels = juce::jmin (SharedBlock::maxChannels, juce::jmax (2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels()));
        midiBuffer.ensureSize (8192);
        startThread (juce::Thread::Priority::highest);
        reply.setProperty ("ok", true, nullptr);
        return reply;
    }

    void run() override
    {
        // the sandbox's audio thread gets the same FTZ/DAZ as LYNX's own audio threads (round 160: it had none)
        const lynx::ScopedFlushDenormals noDenormals;
        while (! threadShouldExit())
        {
            if (! channel.waitRequest (100))
                continue;
            process();
            channel.signalResponse();
        }
    }

    void process() noexcept
    {
        if (hang) // only in the timeout test
            Thread::sleep (3000);
        const auto frames = juce::jlimit (0, blockSize, (int) shared->frames);
        const auto& list = plugin->getParameters();
        for (int i = 0; i < std::min ((int) shared->paramCount, SharedBlock::maxParams); ++i)
            if (const auto k = shared->params[i].index; k >= 0 && k < list.size())
                list[k]->setValue (shared->params[i].value);

        midiBuffer.clear();
        for (int i = 0; i < std::min ((int) shared->midiCount, SharedBlock::maxMidi); ++i)
        {
            const auto& e = shared->midi[i];
            if (e.size >= 1 && e.size <= 3)
                midiBuffer.addEvent (e.bytes, e.size, juce::jlimit (0, juce::jmax (0, frames - 1), (int) e.offset));
        }

        float* pointers[SharedBlock::maxChannels];
        for (int c = 0; c < channels; ++c)
        {
            pointers[c] = shared->audio[c];
            if (c >= shared->channels)
                std::memset (shared->audio[c], 0, sizeof (float) * (std::size_t) frames);
        }
        if (frames > 0)
        {
            juce::AudioBuffer<float> buffer (pointers, channels, frames);
            plugin->processBlock (buffer, midiBuffer);
        }

        shared->paramOutCount = changed.drain (shared->paramsOut, SharedBlock::maxParams);
        shared->latency = plugin->getLatencySamples();
    }

    void parameterValueChanged (int index, float value) override { changed.push (index, value); }
    void parameterGestureChanged (int, bool) override {}

    std::unique_ptr<juce::AudioPluginInstance> plugin;
    std::unique_ptr<SandboxWindow> window;
    SandboxChannel channel;
    SharedBlock* shared = nullptr;
    int blockSize = 512, channels = 2;
    std::atomic<bool> hang { false };
    juce::MidiBuffer midiBuffer;
    ParamQueue changed;
};

std::unique_ptr<SandboxWorker>& worker()
{
    static std::unique_ptr<SandboxWorker> w;
    return w;
}
} // namespace

std::unique_ptr<juce::AudioPluginInstance> createSandboxedPlugin (const juce::PluginDescription& description, double sampleRate,
                                                                  int blockSize, const std::string& stateBase64, juce::String& error)
{
    trace ("parent: session");
    auto session = std::make_unique<SandboxSession>();
    trace ("parent: starting child");
    if (! session->start())
    {
        error = juce::String::fromUTF8 ("could not start the plugin process");
        return nullptr;
    }

    juce::ValueTree open ("open");
    open.setProperty ("shm", session->name(), nullptr);
    open.setProperty ("description", description.createXml()->toString(), nullptr);
    open.setProperty ("sampleRate", sampleRate, nullptr);
    open.setProperty ("blockSize", blockSize, nullptr);
    open.setProperty ("state", juce::String::fromUTF8 (stateBase64.c_str()), nullptr);

    trace ("parent: open");
    const auto info = session->ask (open, 30000);
    session->childAttached();
    if (info.isValid())
        session->watchChild ((int) info["pid"]);
    trace ("parent: reply " + juce::String ((int) info.isValid()));
    if (! info.isValid() || ! (bool) info["ok"])
    {
        error = info.isValid() ? info["error"].toString() : juce::String::fromUTF8 ("the plugin process did not respond");
        return nullptr;
    }
    auto remote = std::make_unique<RemotePlugin> (std::move (session), description, info);
    trace ("parent: proxy ready");
    return remote;
}

bool isSandboxedPluginDead (const juce::AudioPluginInstance& plugin)
{
    const auto* remote = dynamic_cast<const RemotePlugin*> (&plugin);
    return remote != nullptr && remote->dead();
}

void hangSandboxedPluginForTest (juce::AudioPluginInstance& plugin)
{
    if (auto* remote = dynamic_cast<RemotePlugin*> (&plugin))
        remote->hangForTest();
}

void crashSandboxedPluginForTest (juce::AudioPluginInstance& plugin)
{
    if (auto* remote = dynamic_cast<RemotePlugin*> (&plugin))
        remote->crashForTest();
}

bool runPluginSandboxWorker (const juce::String& commandLine)
{
    if (! commandLine.contains (pluginSandboxId))
        return false;
    trace ("child: " + commandLine);
    worker() = std::make_unique<SandboxWorker>();
    if (! worker()->initialiseFromCommandLine (commandLine, pluginSandboxId, 10000))
    {
        worker().reset();
        return false;
    }
    return true;
}

void shutdownPluginSandboxWorker()
{
    worker().reset();
}

#else // macOS: not yet

std::unique_ptr<juce::AudioPluginInstance> createSandboxedPlugin (const juce::PluginDescription&, double, int, const std::string&, juce::String& error)
{
    error = juce::String::fromUTF8 ("out-of-process plugins: Windows and Linux only");
    return nullptr;
}

bool isSandboxedPluginDead (const juce::AudioPluginInstance&) { return false; }
void hangSandboxedPluginForTest (juce::AudioPluginInstance&) {}
void crashSandboxedPluginForTest (juce::AudioPluginInstance&) {}
bool runPluginSandboxWorker (const juce::String&) { return false; }
void shutdownPluginSandboxWorker() {}

#endif
} // namespace lynx
