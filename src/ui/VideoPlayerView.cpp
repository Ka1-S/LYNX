#include "ui/VideoPlayerView.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <cstring>
#include <thread>

#include "core/VideoPlayer.h"
#include "engine/AudioEngine.h"
#include "platform/Paths.h"
#include "ui/ControllerKnob.h"
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/ValueControl.h"
#include "ui/VisualEditors.h"

namespace lynx
{
namespace
{
juce::File resolveVideo (const std::string& path)
{
    if (path.empty())
        return {};
    return platform::localFile (path);
}

// ---------------------------------------------------------------------------------------------------------------
// ffmpeg's report (stderr of "ffmpeg -i"), per path
struct Probe
{
    juce::String report;
    video::Info info;
};

std::mutex probeLock;
std::map<juce::String, Probe> probes;

Probe probe (const juce::String& path)
{
    {
        std::lock_guard<std::mutex> lock (probeLock);
        if (const auto it = probes.find (path); it != probes.end())
            return it->second;
    }
    Probe p;
    const auto ffmpeg = findFfmpeg();
    if (ffmpeg.existsAsFile() && juce::File (path).existsAsFile())
    {
        juce::ChildProcess process;
        if (process.start (juce::StringArray { ffmpeg.getFullPathName(), "-hide_banner", "-nostdin", "-i", path }, juce::ChildProcess::wantStdErr))
        {
            p.report = process.readAllProcessOutput();
            process.waitForProcessToFinish (2000);
        }
        p.info = video::parseFfmpegReport (p.report.toStdString());
        std::lock_guard<std::mutex> lock (probeLock);
        probes[path] = p; // (not cached without ffmpeg / the file: they may appear later)
    }
    return p;
}

// ---------------------------------------------------------------------------------------------------------------
// The sound tracks (48 kHz stereo int16), decoded once per video in the background
constexpr double trackRate = 48000.0;

std::shared_ptr<const video::AudioTrack> decodeTrack (const juce::File& file)
{
    const auto ffmpeg = findFfmpeg();
    if (! ffmpeg.existsAsFile() || ! file.existsAsFile())
        return nullptr;
    const auto info = probe (file.getFullPathName()).info;
    if (! info.hasAudio)
        return nullptr;
    juce::ChildProcess process;
    if (! process.start (juce::StringArray { ffmpeg.getFullPathName(), "-nostdin", "-v", "quiet", "-i", file.getFullPathName(), "-vn", "-sn",
                                             "-ac", "2", "-ar", juce::String ((int) trackRate), "-f", "s16le", "-" },
                         juce::ChildProcess::wantStdOut))
        return nullptr;
    auto track = std::make_shared<video::AudioTrack>();
    track->rate = trackRate;
    if (info.duration > 0.0)
        track->samples.reserve ((std::size_t) (info.duration * trackRate * 2.0) + 4096);
    std::vector<char> bytes;
    char buffer[1 << 16];
    std::size_t odd = 0; // a byte of a sample split between two reads
    char carry = 0;
    for (;;)
    {
        const auto got = process.readProcessOutput (buffer, (int) sizeof (buffer));
        if (got <= 0)
            break;
        int i = 0;
        if (odd != 0)
        {
            const auto lo = (unsigned char) carry, hi = (unsigned char) buffer[0];
            track->samples.push_back ((std::int16_t) (lo | (hi << 8)));
            odd = 0;
            i = 1;
        }
        for (; i + 1 < got; i += 2)
            track->samples.push_back ((std::int16_t) ((unsigned char) buffer[i] | ((unsigned char) buffer[i + 1] << 8)));
        if (i < got)
        {
            carry = buffer[i];
            odd = 1;
        }
    }
    process.waitForProcessToFinish (2000);
    if (track->samples.size() % 2 != 0)
        track->samples.pop_back();
    if (track->samples.empty())
        return nullptr;
    return track;
}

struct TrackEntry
{
    std::shared_ptr<const video::AudioTrack> track;
    bool pending = false, done = false;
};

std::mutex trackLock;
std::map<std::string, TrackEntry> tracks;
AudioEngine* installedEngine = nullptr;
juce::ThreadPool& trackPool()
{
    static juce::ThreadPool pool (1);
    return pool;
}

std::shared_ptr<const video::AudioTrack> provideTrack (const std::string& path, bool wait)
{
    {
        std::lock_guard<std::mutex> lock (trackLock);
        auto& e = tracks[path];
        if (e.done)
            return e.track;
        if (! wait)
        {
            if (! e.pending)
            {
                e.pending = true;
                trackPool().addJob ([path] {
                    auto track = decodeTrack (resolveVideo (path));
                    {
                        std::lock_guard<std::mutex> l (trackLock);
                        auto& entry = tracks[path];
                        entry.track = track;
                        entry.done = true;
                        entry.pending = false;
                    }
                    // the channel's instrument takes it at the next snapshot
                    juce::MessageManager::callAsync ([] {
                        if (installedEngine != nullptr)
                            installedEngine->editSong ([] (Song&) {}, false);
                    });
                });
            }
            return nullptr;
        }
    }
    // exporting: decode now (the export must have the sound)
    auto track = decodeTrack (resolveVideo (path));
    std::lock_guard<std::mutex> lock (trackLock);
    auto& e = tracks[path];
    if (! e.done)
    {
        e.track = track;
        e.done = true;
    }
    return e.track;
}

// ---------------------------------------------------------------------------------------------------------------
// The frames: ffmpeg decodes from the wanted time at the shown size (BGRA), one process per stretch of playback;
// a still (stopped transport) is one frame.
class FrameDecoder final : public juce::Thread
{
public:
    FrameDecoder() : juce::Thread ("LYNX video frames") {}
    ~FrameDecoder() override
    {
        signalThreadShouldExit();
        notify();
        {
            std::lock_guard<std::mutex> lock (procLock);
            if (process != nullptr)
                process->kill(); // (our own child process)
        }
        stopThread (4000);
    }

    void setSource (const juce::String& p, const video::Info& i)
    {
        std::lock_guard<std::mutex> lock (want);
        if (p == path)
            return;
        path = p;
        info = i;
        restart = true;
        notify();
    }

    void setSize (int w, int h)
    {
        std::lock_guard<std::mutex> lock (want);
        w = juce::jlimit (16, 1920, w & ~1);
        h = juce::jlimit (16, 1080, h & ~1);
        if (w == width && h == height)
            return;
        width = w;
        height = h;
        restart = true;
        notify();
    }

    // the time to show (seconds) and whether it moves
    void request (double t, bool moving)
    {
        std::lock_guard<std::mutex> lock (want);
        if (std::abs (t - wantTime) < 1e-6 && moving == wantMoving)
            return;
        wantTime = t;
        wantMoving = moving;
        notify();
    }

    // the latest frame (UI thread)
    juce::Image frame (double& time)
    {
        std::lock_guard<std::mutex> lock (out);
        time = shownTime;
        return shown;
    }

    int frameCount() const noexcept { return frames.load(); }

    void run() override
    {
        double streamStart = -1.0, nextTime = 0.0, eofTime = -1.0, fps = 30.0;
        int w = 0, h = 0;
        bool streaming = false;
        juce::String current;
        std::vector<std::uint8_t> bytes;
        while (! threadShouldExit())
        {
            double t;
            bool moving, fresh;
            juce::String p;
            {
                std::lock_guard<std::mutex> lock (want);
                t = juce::jmax (0.0, wantTime);
                moving = wantMoving;
                fresh = restart;
                restart = false;
                p = path;
                if (fresh)
                {
                    w = width;
                    h = height;
                    fps = info.fps > 1.0 && info.fps < 240.0 ? info.fps : 30.0;
                }
            }
            if (p.isEmpty() || w <= 0 || h <= 0)
            {
                stopProcess();
                streaming = false;
                wait (100);
                continue;
            }
            const auto frameTime = 1.0 / fps;
            if (fresh || p != current)
            {
                eofTime = -1.0;
                lastShown = -1e9;
            }
            bytes.resize ((std::size_t) w * (std::size_t) h * 4);
            if (! moving)
            {
                // a still: one frame at t (once)
                if (std::abs (t - lastShown) < frameTime * 0.5)
                {
                    if (streaming)
                    {
                        stopProcess();
                        streaming = false;
                    }
                    wait (50);
                    continue;
                }
                startProcess (p, t, w, h, fps, false);
                current = p;
                streaming = false;
                if (readFrame (bytes))
                    publish (bytes, w, h, t);
                lastShown = t; // (past the end: the last frame stays)
                stopProcess();
                continue;
            }
            // playing: one ffmpeg per stretch; started again when the time jumps
            if (! streaming && eofTime >= 0.0 && p == current && t >= eofTime - frameTime)
            {
                wait (50); // past the video's end
                continue;
            }
            if (! streaming || p != current || t < streamStart - 1e-3 || t > nextTime + 1.0)
            {
                startProcess (p, t, w, h, fps, true);
                current = p;
                streamStart = nextTime = t;
                eofTime = -1.0;
                streaming = process != nullptr;
                if (! streaming)
                {
                    wait (200);
                    continue;
                }
            }
            if (nextTime > t + 0.04)
            {
                wait (juce::jlimit (2, 30, (int) ((nextTime - t - 0.04) * 1000.0)));
                continue;
            }
            if (! readFrame (bytes))
            {
                stopProcess();
                streaming = false;
                eofTime = nextTime;
                continue;
            }
            const auto ft = nextTime;
            nextTime += frameTime;
            if (nextTime <= t)
                continue; // late: on to the frame of now
            publish (bytes, w, h, ft);
            lastShown = ft;
        }
        stopProcess();
    }

private:
    void startProcess (const juce::String& p, double t, int w, int h, double fps, bool moving)
    {
        stopProcess();
        const auto ffmpeg = findFfmpeg();
        if (! ffmpeg.existsAsFile())
            return;
        juce::StringArray args { ffmpeg.getFullPathName(), "-nostdin", "-v", "quiet", "-ss", juce::String (t, 3), "-i", p, "-an", "-sn" };
        if (! moving)
            args.addArray ({ "-frames:v", "1" });
        args.addArray ({ "-vf", "fps=" + juce::String (fps, 6) + ",scale=" + juce::String (w) + ":" + juce::String (h) + ":flags=bilinear",
                         "-pix_fmt", "bgra", "-f", "rawvideo", "-" });
        auto proc = std::make_unique<juce::ChildProcess>();
        if (! proc->start (args, juce::ChildProcess::wantStdOut))
            return;
        std::lock_guard<std::mutex> lock (procLock);
        process = std::move (proc);
    }

    void stopProcess()
    {
        std::unique_ptr<juce::ChildProcess> old;
        {
            std::lock_guard<std::mutex> lock (procLock);
            old = std::move (process);
        }
        if (old != nullptr && old->isRunning())
            old->kill();
    }

    bool readFrame (std::vector<std::uint8_t>& bytes)
    {
        std::size_t got = 0;
        while (got < bytes.size() && ! threadShouldExit())
        {
            juce::ChildProcess* p;
            {
                std::lock_guard<std::mutex> lock (procLock);
                p = process.get();
            }
            if (p == nullptr)
                return false;
            const auto n = p->readProcessOutput (bytes.data() + got, (int) juce::jmin<std::size_t> (bytes.size() - got, 1 << 20));
            if (n <= 0)
                return false;
            got += (std::size_t) n;
        }
        return got == bytes.size();
    }

    void publish (const std::vector<std::uint8_t>& bytes, int w, int h, double t)
    {
        if (back.isNull() || back.getWidth() != w || back.getHeight() != h)
            back = juce::Image (juce::Image::ARGB, w, h, false);
        {
            juce::Image::BitmapData data (back, juce::Image::BitmapData::writeOnly);
            for (int y = 0; y < h; ++y)
                std::memcpy (data.getLinePointer (y), bytes.data() + (std::size_t) y * (std::size_t) w * 4, (std::size_t) w * 4);
        }
        {
            std::lock_guard<std::mutex> lock (out);
            std::swap (shown, back);
            shownTime = t;
        }
        if (back.isValid() && (back.getWidth() != w || back.getHeight() != h))
            back = {};
        ++frames;
    }

    std::mutex want, out, procLock;
    juce::String path;
    video::Info info;
    int width = 0, height = 0;
    double wantTime = 0.0;
    bool wantMoving = false, restart = false;
    std::unique_ptr<juce::ChildProcess> process;
    juce::Image shown, back;
    double shownTime = -1.0, lastShown = -1e9;
    std::atomic<int> frames { 0 };
};

// ---------------------------------------------------------------------------------------------------------------
// Where the video is drawn (the editor's area or the detached window)
class VideoSurface final : public juce::Component
{
public:
    std::function<void (juce::Rectangle<int>)> onSize;
    std::function<void()> onDoubleClick;
    juce::String message; // drawn when there is no frame
    bool keepAspect = true;
    juce::Image image;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black);
        if (image.isValid())
        {
            const auto area = getLocalBounds().toFloat();
            if (keepAspect)
                g.drawImageWithin (image, 0, 0, getWidth(), getHeight(), juce::RectanglePlacement::centred, false);
            else
                g.drawImage (image, area);
            return;
        }
        g.setColour (theme::colour::textSecondary);
        g.setFont (theme::font (theme::type::body));
        g.drawFittedText (message, getLocalBounds().reduced (16), juce::Justification::centred, 4);
    }

    void resized() override
    {
        if (onSize)
            onSize (getLocalBounds());
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (onDoubleClick)
            onDoubleClick();
    }
};

// A small icon button (drawn paths, LYNX's style)
class IconButton final : public juce::Button
{
public:
    enum class Icon { load, options, play, pause, stop, loop, sound, mute, detach, start, end };

    IconButton (Icon i) : juce::Button ({}), icon (i) {}
    Icon icon;

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const auto on = getToggleState();
        g.setColour (down ? theme::colour::bg6 : over ? theme::colour::bg5 : theme::colour::bg4);
        g.fillRoundedRectangle (r, 4.0f);
        if (on)
        {
            g.setColour (theme::colour::accent);
            g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.2f);
        }
        g.setColour (on ? theme::colour::accent : theme::colour::textPrimary);
        const auto c = r.getCentre();
        const auto s = juce::jmin (r.getWidth(), r.getHeight()) * 0.30f;
        juce::Path p;
        switch (icon)
        {
            case Icon::play:
                p.addTriangle (c.x - s * 0.8f, c.y - s, c.x - s * 0.8f, c.y + s, c.x + s, c.y);
                g.fillPath (p);
                break;
            case Icon::pause:
                g.fillRect (juce::Rectangle<float> (c.x - s * 0.8f, c.y - s, s * 0.55f, s * 2.0f));
                g.fillRect (juce::Rectangle<float> (c.x + s * 0.25f, c.y - s, s * 0.55f, s * 2.0f));
                break;
            case Icon::stop:
                g.fillRect (juce::Rectangle<float> (c.x - s * 0.8f, c.y - s * 0.8f, s * 1.6f, s * 1.6f));
                break;
            case Icon::loop:
                p.addCentredArc (c.x, c.y, s, s * 0.8f, 0.0f, 0.6f, 5.6f, true);
                g.strokePath (p, juce::PathStrokeType (1.6f));
                {
                    juce::Path a;
                    const auto tip = juce::Point<float> (c.x + s * std::sin (0.6f), c.y - s * 0.8f * std::cos (0.6f));
                    a.addTriangle (tip.x - s * 0.45f, tip.y - s * 0.35f, tip.x + s * 0.2f, tip.y + s * 0.45f, tip.x + s * 0.4f, tip.y - s * 0.4f);
                    g.fillPath (a);
                }
                break;
            case Icon::sound:
            case Icon::mute:
                p.startNewSubPath (c.x - s, c.y - s * 0.35f);
                p.lineTo (c.x - s * 0.45f, c.y - s * 0.35f);
                p.lineTo (c.x + s * 0.15f, c.y - s);
                p.lineTo (c.x + s * 0.15f, c.y + s);
                p.lineTo (c.x - s * 0.45f, c.y + s * 0.35f);
                p.lineTo (c.x - s, c.y + s * 0.35f);
                p.closeSubPath();
                g.fillPath (p);
                if (icon == Icon::mute)
                {
                    g.drawLine (c.x + s * 0.4f, c.y - s * 0.45f, c.x + s * 1.1f, c.y + s * 0.45f, 1.6f);
                    g.drawLine (c.x + s * 0.4f, c.y + s * 0.45f, c.x + s * 1.1f, c.y - s * 0.45f, 1.6f);
                }
                else
                {
                    juce::Path w;
                    w.addCentredArc (c.x + s * 0.2f, c.y, s * 0.75f, s * 0.75f, 0.0f, 0.6f, 2.55f, true);
                    g.strokePath (w, juce::PathStrokeType (1.4f));
                }
                break;
            case Icon::load:
                p.startNewSubPath (c.x - s * 1.1f, c.y + s * 0.8f);
                p.lineTo (c.x - s * 1.1f, c.y - s * 0.8f);
                p.lineTo (c.x - s * 0.3f, c.y - s * 0.8f);
                p.lineTo (c.x, c.y - s * 0.45f);
                p.lineTo (c.x + s * 1.1f, c.y - s * 0.45f);
                p.lineTo (c.x + s * 1.1f, c.y + s * 0.8f);
                p.closeSubPath();
                g.strokePath (p, juce::PathStrokeType (1.5f));
                break;
            case Icon::options:
                p.addTriangle (c.x - s * 0.8f, c.y - s * 0.35f, c.x + s * 0.8f, c.y - s * 0.35f, c.x, c.y + s * 0.5f);
                g.fillPath (p);
                break;
            case Icon::detach:
                g.drawRect (juce::Rectangle<float> (c.x - s, c.y - s * 0.6f, s * 1.5f, s * 1.4f), 1.4f);
                g.drawLine (c.x - s * 0.1f, c.y + s * 0.1f, c.x + s * 1.0f, c.y - s * 1.0f, 1.4f);
                g.drawLine (c.x + s * 0.35f, c.y - s * 1.0f, c.x + s * 1.0f, c.y - s * 1.0f, 1.4f);
                g.drawLine (c.x + s * 1.0f, c.y - s * 1.0f, c.x + s * 1.0f, c.y - s * 0.35f, 1.4f);
                break;
            case Icon::start:
            case Icon::end:
            {
                const auto dir = icon == Icon::start ? 1.0f : -1.0f;
                const auto x = c.x - dir * s * 0.8f;
                g.drawLine (x, c.y - s, x, c.y + s, 1.6f);
                p.addTriangle (x + dir * s * 0.3f, c.y, x + dir * s * 1.5f, c.y - s * 0.75f, x + dir * s * 1.5f, c.y + s * 0.75f);
                g.fillPath (p);
                break;
            }
        }
    }
};

// The video position: the whole video, the start…end range, the time now; drag to move
class TimeRail final : public juce::Component
{
public:
    std::function<void (double)> onSeek; // seconds
    double duration = 0.0, start = 0.0, end = 0.0, now = 0.0;

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced (6.0f, 0.0f);
        const auto rail = r.withSizeKeepingCentre (r.getWidth(), 4.0f);
        g.setColour (theme::colour::bg4);
        g.fillRoundedRectangle (rail, 2.0f);
        if (duration <= 0.0)
            return;
        const auto x = [&] (double t) { return rail.getX() + (float) (juce::jlimit (0.0, 1.0, t / duration)) * rail.getWidth(); };
        const auto e = end > 0.0 ? end : duration;
        g.setColour (theme::colour::accentMuted);
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (x (start), rail.getY(), x (e), rail.getBottom()));
        g.setColour (theme::colour::accent);
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (x (start), rail.getY(), x (juce::jlimit (start, e, now)), rail.getBottom()));
        g.setColour (theme::colour::textSecondary);
        g.fillRect (juce::Rectangle<float> (x (start) - 1.0f, r.getCentreY() - 7.0f, 2.0f, 14.0f));
        g.fillRect (juce::Rectangle<float> (x (e) - 1.0f, r.getCentreY() - 7.0f, 2.0f, 14.0f));
        g.setColour (theme::colour::textPrimary);
        g.fillEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ x (juce::jmax (0.0, now)), r.getCentreY() }));
    }

    void mouseDown (const juce::MouseEvent& e) override { seekAt (e.x); }
    void mouseDrag (const juce::MouseEvent& e) override { seekAt (e.x); }

private:
    void seekAt (int px)
    {
        if (duration <= 0.0 || ! onSeek)
            return;
        const auto r = getLocalBounds().toFloat().reduced (6.0f, 0.0f);
        onSeek (juce::jlimit (0.0, duration, (double) (px - r.getX()) / juce::jmax (1.0f, r.getWidth()) * duration));
    }
};

class ClickLabel final : public juce::Label
{
public:
    std::function<void()> onLeft, onRight;
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            if (onRight)
                onRight();
        }
        else if (onLeft)
            onLeft();
    }
};

std::optional<double> parseTime (const juce::String& typed)
{
    // H:M:S:ms, M:S, S (seconds, decimals allowed)
    auto parts = juce::StringArray::fromTokens (typed.trim(), ":", "");
    parts.removeEmptyStrings();
    if (parts.isEmpty() || parts.size() > 4)
        return std::nullopt;
    for (const auto& p : parts)
        if (! p.containsOnly ("0123456789.-"))
            return std::nullopt;
    double t = 0.0;
    if (parts.size() == 4)
        t = parts[0].getDoubleValue() * 3600.0 + parts[1].getDoubleValue() * 60.0 + parts[2].getDoubleValue() + parts[3].getDoubleValue() / 1000.0;
    else if (parts.size() == 3)
        t = parts[0].getDoubleValue() * 3600.0 + parts[1].getDoubleValue() * 60.0 + parts[2].getDoubleValue();
    else if (parts.size() == 2)
        t = parts[0].getDoubleValue() * 60.0 + parts[1].getDoubleValue();
    else
        t = parts[0].getDoubleValue();
    return t;
}

// ---------------------------------------------------------------------------------------------------------------
class VideoPlayerEditor final : public juce::Component,
                                public juce::FileDragAndDropTarget,
                                private juce::Timer,
                                private juce::ChangeListener
{
public:
    VideoPlayerEditor (AudioEngine& e, std::function<ChannelDef* (Song&)> l, int c) : engine (e), locate (std::move (l)), channel (c)
    {
        setWantsKeyboardFocus (true);
        addAndMakeVisible (surface);
        surface.onSize = [this] (juce::Rectangle<int> r) {
            if (window == nullptr)
                sizeDecoder (r);
        };
        surface.onDoubleClick = [this] { detach(); };
        surface.addMouseListener (this, false);

        auto icon = [this] (IconButton& b, const juce::String& hint, std::function<void()> click) {
            ui::setHint (b, hint);
            b.onClick = std::move (click);
            addAndMakeVisible (b);
        };
        icon (loadButton, tr ("Load video file..."), [this] { chooseFile(); });
        icon (optionsButton, tr ("Plugin options"), [this] { showOptions(); });
        icon (setStartButton, tr ("Set current position as start time"), [this] { setRange (true, nowTime); });
        icon (setEndButton, tr ("Set current position as end time"), [this] { setRange (false, nowTime); });
        icon (loopButton, tr ("Loop"), [this] { edit ([] (video::Settings& s) { s.loop = ! s.loop; }); });
        icon (playButton, tr ("Play / pause"), [this] { playPause(); });
        icon (stopButton, tr ("Stop"), [this] { stopPlayback(); });
        icon (soundButton, tr ("Mute audio"), [this] { edit ([] (video::Settings& s) { s.mute = ! s.mute; }); });
        icon (detachButton, tr ("Show the video in its own window (double-click the video)"), [this] { detach(); });

        slaveButton.setButtonText (tr ("Host"));
        slaveButton.setClickingTogglesState (false);
        slaveButton.setColour (juce::TextButton::buttonOnColourId, theme::colour::accent);
        slaveButton.onClick = [this] { edit ([] (video::Settings& s) { s.slave = ! s.slave; }); };
        ui::setHint (slaveButton, tr ("Slave playback to host: the song position drives the video (off: the player's own Play / Stop)"));
        addAndMakeVisible (slaveButton);

        ui::ValueSpec vol;
        vol.name = tr ("Volume");
        vol.minimum = 0.0;
        vol.maximum = 127.0;
        vol.interval = 1.0;
        vol.defaultValue = 100.0;
        vol.toText = [] (double v) { return juce::String (juce::roundToInt (v / 100.0 * 100.0)) + "%"; };
        volume = std::make_unique<ctlui::Knob> (&engine, vol, [this] { return (double) settings.volume; },
                                                [this] (double v) { edit ([v] (video::Settings& s) { s.volume = juce::jlimit (0, 127, juce::roundToInt (v)); }); });
        volume->shortName = tr ("Vol");
        addAndMakeVisible (*volume);

        for (auto* lab : { &startLabel, &endLabel, &nowLabel })
        {
            lab->setFont (theme::font (theme::type::body));
            lab->setColour (juce::Label::textColourId, theme::colour::textPrimary);
            lab->setJustificationType (juce::Justification::centred);
            addAndMakeVisible (*lab);
        }
        nowLabel.setColour (juce::Label::textColourId, theme::colour::accent);
        ui::setHint (startLabel, tr ("Start time (click to go to, right click to set)"));
        ui::setHint (endLabel, tr ("End time (click to go to, right click to set)"));
        ui::setHint (nowLabel, tr ("Current time (right click to set)"));
        startLabel.onLeft = [this] { seekVideo (seconds (settings.start)); };
        startLabel.onRight = [this] { typeTime (true); };
        endLabel.onLeft = [this] { seekVideo (endTime()); };
        endLabel.onRight = [this] { typeTime (false); };
        nowLabel.onRight = [this] { typeNow(); };

        rail.onSeek = [this] (double t) { seekVideo (t); };
        ui::setHint (rail, tr ("Video position (use left/right keys for previous/next frame)"));
        addAndMakeVisible (rail);

        locateButton.setButtonText (tr ("Locate ffmpeg..."));
        locateButton.onClick = [this] { chooseFfmpeg(); };
        addChildComponent (locateButton);

        decoder.startThread (juce::Thread::Priority::low);
        engine.addChangeListener (this);
        reload();
        setSize (600, 420);
        startTimerHz (30);
    }

    ~VideoPlayerEditor() override
    {
        engine.removeChangeListener (this);
        window = nullptr;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg2);
        g.setColour (theme::colour::bg3);
        g.fillRect (panelArea);
        g.setColour (theme::colour::border);
        g.drawHorizontalLine (panelArea.getY(), 0.0f, (float) getWidth());
        if (! info.hasVideo && settings.path.size() > 0)
        {
            g.setColour (theme::colour::textSecondary);
            g.setFont (theme::font (theme::type::small));
        }
        g.setColour (theme::colour::textSecondary);
        g.setFont (theme::font (theme::type::small));
        g.drawText (statusLine, panelArea.withTrimmedTop (panelArea.getHeight() - 18).reduced (8, 0), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        const auto panelHeight = 98;
        panelArea = r.removeFromBottom (panelHeight);
        surface.setBounds (r);
        locateButton.setBounds (r.withSizeKeepingCentre (160, 28).translated (0, 40));
        auto p = panelArea.reduced (8, 6);
        auto row1 = p.removeFromTop (22);
        startLabel.setBounds (row1.removeFromLeft (100));
        endLabel.setBounds (row1.removeFromRight (100));
        rail.setBounds (row1.reduced (4, 0));
        p.removeFromTop (6);
        auto row2 = p.removeFromTop (34);
        const auto b = 30;
        loadButton.setBounds (row2.removeFromLeft (b).withSizeKeepingCentre (b, 28));
        row2.removeFromLeft (4);
        optionsButton.setBounds (row2.removeFromLeft (b).withSizeKeepingCentre (b, 28));
        row2.removeFromLeft (4);
        detachButton.setBounds (row2.removeFromLeft (b).withSizeKeepingCentre (b, 28));
        row2.removeFromLeft (10);
        setStartButton.setBounds (row2.removeFromLeft (b).withSizeKeepingCentre (b, 28));
        row2.removeFromLeft (4);
        setEndButton.setBounds (row2.removeFromLeft (b).withSizeKeepingCentre (b, 28));
        volume->setBounds (row2.removeFromRight (44));
        row2.removeFromRight (4);
        soundButton.setBounds (row2.removeFromRight (b).withSizeKeepingCentre (b, 28));
        row2.removeFromRight (10);
        slaveButton.setBounds (row2.removeFromRight (52).withSizeKeepingCentre (52, 26));
        row2.removeFromRight (6);
        stopButton.setBounds (row2.removeFromRight (b).withSizeKeepingCentre (b, 28));
        row2.removeFromRight (4);
        playButton.setBounds (row2.removeFromRight (b).withSizeKeepingCentre (b, 28));
        row2.removeFromRight (4);
        loopButton.setBounds (row2.removeFromRight (b).withSizeKeepingCentre (b, 28));
        nowLabel.setBounds (row2.reduced (6, 2));
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        const auto frame = 1.0 / (info.fps > 1.0 ? info.fps : 30.0);
        if (key.isKeyCode (juce::KeyPress::leftKey))
        {
            seekVideo (juce::jmax (0.0, nowTime - frame));
            return true;
        }
        if (key.isKeyCode (juce::KeyPress::rightKey))
        {
            seekVideo (nowTime + frame);
            return true;
        }
        return false;
    }

    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        return files.size() == 1 && juce::File (files[0]).hasFileExtension (videoExtensions);
    }
    void filesDropped (const juce::StringArray& files, int, int) override { load (juce::File (files[0])); }

private:
    static constexpr const char* videoExtensions = "mp4;m4v;mov;avi;mkv;webm;wmv;flv;mpg;mpeg;ts;gif;ogv;3gp";

    static double seconds (std::int64_t units) noexcept { return (double) units / (double) video::unitsPerSecond; }
    static std::int64_t units (double s) noexcept { return (std::int64_t) std::llround (juce::jmax (0.0, s) * (double) video::unitsPerSecond); }
    double endTime() const { return settings.end > 0 ? seconds (settings.end) : info.duration; }

    const ChannelDef* def() const { return locate ? locate (const_cast<Song&> (engine.getSong())) : nullptr; }

    void edit (std::function<void (video::Settings&)> change, bool undoable = true)
    {
        auto s = settings;
        change (s);
        if (s == settings)
            return;
        settings = s;
        const auto state = video::write (s);
        engine.editSong ([l = locate, state] (Song& song) {
            if (auto* d = l ? l (song) : nullptr)
                d->pluginState = state;
        }, undoable);
        refreshControls();
    }

    void reload()
    {
        const auto* d = def();
        if (d == nullptr)
            return;
        settings = video::readOrDefault (d->pluginState);
        const auto file = resolveVideo (settings.path);
        const auto path = file.getFullPathName();
        if (path != loadedPath)
        {
            loadedPath = path;
            info = path.isNotEmpty() ? probe (path).info : video::Info {};
            decoder.setSource (info.hasVideo ? path : juce::String(), info);
        }
        refreshControls();
    }

    void refreshControls()
    {
        loopButton.setToggleState (settings.loop, juce::dontSendNotification);
        slaveButton.setToggleState (settings.slave, juce::dontSendNotification);
        soundButton.icon = settings.mute ? IconButton::Icon::mute : IconButton::Icon::sound;
        soundButton.setToggleState (settings.mute, juce::dontSendNotification);
        surface.keepAspect = settings.keepAspect;
        const auto noFfmpeg = ! findFfmpeg().existsAsFile();
        locateButton.setVisible (noFfmpeg);
        if (noFfmpeg)
            surface.message = tr ("ffmpeg was not found. LYNX plays videos with ffmpeg: install it or locate it.");
        else if (settings.path.empty())
            surface.message = tr ("No video") + "\n" + tr ("Load a video file (or drop one here)");
        else if (! resolveVideo (settings.path).existsAsFile())
            surface.message = tr ("The video was not found:") + "\n" + juce::String::fromUTF8 (settings.path.c_str());
        else if (! info.hasVideo)
            surface.message = tr ("ffmpeg could not read a video in this file");
        else
            surface.message = {};
        juce::String s;
        if (info.hasVideo)
            s << info.width << "x" << info.height << ", " << juce::String (info.fps, 3).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") << " fps, "
              << juce::String (video::formatTime (info.duration, video::hms, info.fps)) << (info.hasAudio ? "" : ", " + tr ("no sound"));
        if (settings.delay > 0)
            s << "  |  " << tr ("Delay playback by {0}", { juce::String (video::formatTime (seconds (settings.delay), settings.timeDisplay, info.fps)) });
        statusLine = s;
        rail.duration = info.duration;
        rail.start = seconds (settings.start);
        rail.end = settings.end > 0 ? seconds (settings.end) : 0.0;
        startLabel.setText (video::formatTime (seconds (settings.start), settings.timeDisplay, info.fps), juce::dontSendNotification);
        endLabel.setText (video::formatTime (endTime(), settings.timeDisplay, info.fps), juce::dontSendNotification);
        for (auto* b : { &setStartButton, &setEndButton, &loopButton, &playButton, &stopButton })
            b->setEnabled (info.hasVideo);
        repaint();
    }

    video::Player* player() const
    {
        if (channel < 0)
            return nullptr;
        heldInstrument = engine.getChannelInstrument (channel);
        return video::playerOf (heldInstrument.get());
    }

    double songSeconds() const
    {
        const auto ticks = engine.getTransport().positionTicks.load();
        const auto& song = engine.getSong();
        if (settings.ignoreTempo)
            return song.secondsForTicks ((std::int64_t) ticks);
        return ticks / (double) ppq * 60.0 / juce::jmax (1.0, song.bpm);
    }

    void timerCallback() override
    {
        const auto& t = engine.getTransport();
        bool moving = false;
        if (settings.slave)
        {
            nowTime = video::Clock::videoTime (songSeconds(), settings, info.duration);
            moving = t.playing.load();
        }
        else if (auto* p = player())
        {
            nowTime = p->currentTime();
            moving = p->running();
        }
        const auto shownTime = juce::jmax (0.0, nowTime < 0.0 ? seconds (settings.start) : nowTime);
        if (info.hasVideo)
            decoder.request (shownTime, moving && nowTime >= 0.0 && (settings.loop || nowTime < endTime() - 1e-3));
        double ft = 0.0;
        auto img = decoder.frame (ft);
        auto* target = window != nullptr ? detachedSurface : &surface;
        if (target != nullptr && (img.getPixelData() != target->image.getPixelData() || frames != decoder.frameCount()))
        {
            frames = decoder.frameCount();
            target->image = img;
            target->keepAspect = settings.keepAspect;
            target->repaint();
        }
        if (window != nullptr && surface.image.isValid())
        {
            surface.image = {};
            surface.message = tr ("The video is in its own window");
            surface.repaint();
        }
        playButton.icon = moving ? IconButton::Icon::pause : IconButton::Icon::play;
        playButton.repaint();
        nowLabel.setText (video::formatTime (nowTime, settings.timeDisplay, info.fps), juce::dontSendNotification);
        if (std::abs (rail.now - nowTime) > 1e-4)
        {
            rail.now = nowTime;
            rail.repaint();
        }
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        const auto* d = def();
        if (d == nullptr)
            return;
        if (d->pluginState != lastState)
        {
            lastState = d->pluginState;
            reload();
        }
    }

    void sizeDecoder (juce::Rectangle<int> r)
    {
        if (! info.hasVideo || r.isEmpty())
            return;
        const auto scale = juce::jmin (1.0, juce::jmin ((double) r.getWidth() / info.width, (double) r.getHeight() / info.height));
        const auto dpi = juce::jmax (1.0f, juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() != nullptr
                                               ? (float) juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->scale
                                               : 1.0f);
        const auto s = juce::jmin (1.0, scale * dpi);
        decoder.setSize (juce::roundToInt (info.width * s), juce::roundToInt (info.height * s));
    }

    // the song position (slaved) or the player's clock that shows video time `t`
    void seekVideo (double t)
    {
        t = juce::jlimit (0.0, juce::jmax (0.0, info.duration), t);
        if (settings.slave)
        {
            const auto song = t - seconds (settings.start) + seconds (settings.delay);
            const auto ticks = juce::jmax (0.0, song) * juce::jmax (1.0, engine.getSong().bpm) / 60.0 * (double) ppq;
            engine.seek (ticks);
        }
        else if (auto* p = player())
        {
            const auto wasRunning = p->running();
            p->ownPlay (t);
            if (! wasRunning)
                p->ownPause();
        }
        nowTime = t;
    }

    void playPause()
    {
        if (settings.slave)
        {
            if (engine.getTransport().playing.load())
                engine.stop();
            else
                engine.play();
            return;
        }
        if (auto* p = player())
        {
            if (p->running())
                p->ownPause();
            else
                p->ownPlay (nowTime >= endTime() - 1e-3 ? seconds (settings.start) : juce::jmax (seconds (settings.start), nowTime));
        }
    }

    void stopPlayback()
    {
        if (settings.slave)
            engine.stop();
        else if (auto* p = player())
            p->ownStop();
    }

    void setRange (bool isStart, double t)
    {
        edit ([this, isStart, t] (video::Settings& s) {
            if (isStart)
            {
                s.start = units (t);
                if (s.end > 0 && s.end < s.start)
                    s.end = s.start;
            }
            else
            {
                s.end = units (juce::jmin (t, info.duration));
                if (s.end < s.start)
                    s.start = s.end;
            }
        });
    }

    void typeTime (bool isStart)
    {
        auto* w = new juce::AlertWindow (isStart ? tr ("Start time (H:M:S:MS)") : tr ("End time (H:M:S:MS)"), {}, juce::MessageBoxIconType::NoIcon, this);
        w->addTextEditor ("t", video::formatTime (isStart ? seconds (settings.start) : endTime(), video::hms, info.fps));
        w->addButton (tr ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<VideoPlayerEditor> self (this);
        w->enterModalState (true, juce::ModalCallbackFunction::create ([self, w, isStart] (int r) {
            if (r == 1 && self != nullptr)
                if (const auto t = parseTime (w->getTextEditorContents ("t")))
                    self->setRange (isStart, *t);
        }), true);
    }

    void typeNow()
    {
        auto* w = new juce::AlertWindow (tr ("Current time (H:M:S:MS)"), {}, juce::MessageBoxIconType::NoIcon, this);
        w->addTextEditor ("t", video::formatTime (juce::jmax (0.0, nowTime), video::hms, info.fps));
        w->addButton (tr ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<VideoPlayerEditor> self (this);
        w->enterModalState (true, juce::ModalCallbackFunction::create ([self, w] (int r) {
            if (r == 1 && self != nullptr)
                if (const auto t = parseTime (w->getTextEditorContents ("t")))
                    self->seekVideo (*t);
        }), true);
    }

    void typeDelay()
    {
        auto* w = new juce::AlertWindow (tr ("Delay playback (H:M:S:MS)"), tr ("The video starts when the song reaches this time."),
                                         juce::MessageBoxIconType::NoIcon, this);
        w->addTextEditor ("t", video::formatTime (seconds (settings.delay), video::hms, info.fps));
        w->addButton (tr ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<VideoPlayerEditor> self (this);
        w->enterModalState (true, juce::ModalCallbackFunction::create ([self, w] (int r) {
            if (r == 1 && self != nullptr)
                if (const auto t = parseTime (w->getTextEditorContents ("t")))
                    self->edit ([d = units (*t)] (video::Settings& s) { s.delay = d; });
        }), true);
    }

    void showOptions()
    {
        juce::PopupMenu m;
        m.addItem (tr ("Enable audio"), true, settings.enableAudio, [this] { edit ([] (video::Settings& s) { s.enableAudio = ! s.enableAudio; }); });
        m.addItem (tr ("Dock control panel"), true, settings.dock, [this] { edit ([] (video::Settings& s) { s.dock = ! s.dock; }); });
        m.addItem (tr ("Ignore tempo changes"), true, settings.ignoreTempo, [this] { edit ([] (video::Settings& s) { s.ignoreTempo = ! s.ignoreTempo; }); });
        m.addItem (tr ("Keep aspect ratio"), true, settings.keepAspect, [this] { edit ([] (video::Settings& s) { s.keepAspect = ! s.keepAspect; }); });
        juce::PopupMenu display;
        // FL's menu order (the modes' numbers are FL's)
        for (const auto mode : { video::hms, video::smpte23976, video::smpte24, video::smpte25, video::smpte2997drop, video::smpte2997, video::smpte30,
                                 video::smpte48, video::smpte50, video::smpte5994drop, video::smpte5994, video::smpte60, video::smpte72, video::smpte120,
                                 video::smpteAuto })
            display.addItem (mode == video::hms ? tr ("Hours:Minutes:Seconds:Ms") : juce::String (video::timeDisplayName (mode)), true, settings.timeDisplay == mode,
                             [this, mode] { edit ([mode] (video::Settings& s) { s.timeDisplay = mode; }); });
        m.addSubMenu (tr ("Time display"), display);
        m.addSeparator();
        m.addItem (tr ("Send audio to playlist"), info.hasAudio, false, [this] { sendAudioToPlaylist(); });
        juce::PopupMenu delay;
        delay.addSectionHeader (tr ("Delay playback by {0}", { juce::String (video::formatTime (seconds (settings.delay), settings.timeDisplay, info.fps)) }));
        delay.addItem (tr ("Set..."), [this] { typeDelay(); });
        delay.addItem (tr ("Delay playback until the song position"), [this] {
            const auto ticks = engine.getTransport().positionTicks.load();
            edit ([d = units (ticks / (double) ppq * 60.0 / juce::jmax (1.0, engine.getSong().bpm))] (video::Settings& s) { s.delay = d; });
        });
        delay.addItem (tr ("No delay"), settings.delay != 0, false, [this] { edit ([] (video::Settings& s) { s.delay = 0; }); });
        m.addSubMenu (tr ("Delay playback"), delay);
        m.addSeparator();
        m.addItem (tr ("Locate ffmpeg..."), [this] { chooseFfmpeg(); });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&optionsButton));
    }

    void chooseFile()
    {
        chooser = std::make_unique<juce::FileChooser> (tr ("Load video file..."), resolveVideo (settings.path).getParentDirectory(),
                                                       juce::String ("*.") + juce::String (videoExtensions).replace (";", ";*."));
        juce::Component::SafePointer<VideoPlayerEditor> self (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [self] (const juce::FileChooser& fc) {
            if (self != nullptr && fc.getResult().existsAsFile())
                self->load (fc.getResult());
        });
    }

    void load (const juce::File& file)
    {
        const auto pr = probe (file.getFullPathName());
        edit ([&] (video::Settings& s) {
            s.path = file.getFullPathName().toStdString();
            s.start = 0;
            s.end = 0;
            if (pr.info.hasVideo)
            {
                s.width = pr.info.width;
                s.height = pr.info.height;
            }
        });
        reload();
    }

    void chooseFfmpeg()
    {
        chooser = std::make_unique<juce::FileChooser> (tr ("Locate ffmpeg"), juce::File(),
#if JUCE_WINDOWS
                                                       "ffmpeg.exe");
#else
                                                       "*");
#endif
        juce::Component::SafePointer<VideoPlayerEditor> self (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [self] (const juce::FileChooser& fc) {
            if (self == nullptr || ! fc.getResult().existsAsFile())
                return;
            setFfmpegLocation (fc.getResult());
            {
                std::lock_guard<std::mutex> lock (probeLock);
                probes.clear();
            }
            {
                std::lock_guard<std::mutex> lock (trackLock);
                tracks.clear();
            }
            self->loadedPath = {};
            self->reload();
            self->engine.editSong ([] (Song&) {}, false); // (the instrument asks for its sound track again)
        });
    }

    // FL's "Send audio to playlist": the sound track as a WAV in an audio clip where the video starts
    void sendAudioToPlaylist()
    {
        const auto file = resolveVideo (settings.path);
        const auto ffmpeg = findFfmpeg();
        if (! file.existsAsFile() || ! ffmpeg.existsAsFile())
            return;
        auto folder = platform::cacheFolder ("video-audio");
        folder.createDirectory();
        const auto key = juce::String::toHexString (file.getFullPathName().hashCode64()) + "-" + juce::String::toHexString (file.getLastModificationTime().toMilliseconds());
        const auto wav = folder.getChildFile (file.getFileNameWithoutExtension() + " (" + tr ("audio") + ") " + key.substring (0, 8) + ".wav");
        const auto delay = seconds (settings.delay);
        juce::Component::SafePointer<VideoPlayerEditor> self (this);
        statusLine = tr ("Extracting the sound...");
        repaint();
        std::thread ([self, file, ffmpeg, wav, delay] {
            bool ok = wav.existsAsFile();
            if (! ok)
            {
                juce::ChildProcess p;
                if (p.start (juce::StringArray { ffmpeg.getFullPathName(), "-nostdin", "-v", "quiet", "-y", "-i", file.getFullPathName(), "-vn", "-sn",
                                                 "-ac", "2", "-c:a", "pcm_s16le", wav.getFullPathName() },
                             0))
                    p.waitForProcessToFinish (10 * 60 * 1000);
                ok = wav.existsAsFile() && wav.getSize() > 44;
            }
            juce::MessageManager::callAsync ([self, wav, ok, delay] {
                if (self == nullptr)
                    return;
                if (ok)
                    self->addClip (wav, delay);
                self->refreshControls();
            });
        }).detach();
    }

    void addClip (const juce::File& wav, double delaySeconds)
    {
        const auto data = engine.getSampleLibrary().load (wav.getFullPathName());
        if (data == nullptr)
            return;
        const auto bpm = juce::jmax (1.0, engine.getSong().bpm);
        const auto length = juce::jmax<std::int64_t> (ppq / 4, (std::int64_t) ((double) data->frames() / data->sampleRate * bpm / 60.0 * ppq));
        const auto start = (std::int64_t) std::llround (delaySeconds * bpm / 60.0 * ppq);
        const auto path = wav.getFullPathName().toStdString();
        const auto name = wav.getFileNameWithoutExtension().toStdString();
        engine.editSong ([=] (Song& s) {
            int track = 0;
            for (;; ++track)
                if (std::none_of (s.playlist.begin(), s.playlist.end(), [&] (const Clip& c) { return c.track == track && c.start < start + length && c.end() > start; }))
                    break;
            ChannelDef c;
            c.name = name;
            c.instrument = InstrumentKind::audioClip;
            c.samplePath = path;
            c.mixerTrack = s.claimFreeInsert();
            s.channels.push_back (c);
            Clip clip;
            clip.kind = ClipKind::audio;
            clip.channel = (int) s.channels.size() - 1;
            clip.track = track;
            clip.start = start;
            clip.length = length;
            s.playlist.push_back (clip);
            s.mode = PlayMode::song;
        });
    }

    // the video in its own resizable window (double-click: full screen)
    class DetachedWindow final : public juce::DocumentWindow
    {
    public:
        DetachedWindow (VideoPlayerEditor& o) : juce::DocumentWindow ("Video", theme::colour::bg1, juce::DocumentWindow::allButtons), owner (o)
        {
            setUsingNativeTitleBar (true);
            setResizable (true, true);
            auto* s = new VideoSurface();
            s->onSize = [this] (juce::Rectangle<int> r) { owner.sizeDecoder (r); };
            s->onDoubleClick = [this] { setFullScreen (! isFullScreen()); };
            surface = s;
            setContentOwned (s, false);
            const auto w = owner.info.width > 0 ? owner.info.width : 640, h = owner.info.height > 0 ? owner.info.height : 360;
            const auto scale = juce::jmin (1.0, 960.0 / w, 540.0 / h);
            centreWithSize (juce::roundToInt (w * scale), juce::roundToInt (h * scale));
            setVisible (true);
        }
        void closeButtonPressed() override
        {
            juce::MessageManager::callAsync ([o = juce::Component::SafePointer<VideoPlayerEditor> (&owner)] {
                if (o != nullptr)
                    o->attach();
            });
        }
        VideoSurface* surface = nullptr;

    private:
        VideoPlayerEditor& owner;
    };

    void detach()
    {
        if (window != nullptr)
        {
            window->toFront (true);
            return;
        }
        if (! info.hasVideo)
            return;
        window = std::make_unique<DetachedWindow> (*this);
        window->setName (juce::File (juce::String::fromUTF8 (settings.path.c_str())).getFileName());
        detachedSurface = window->surface;
        detachButton.setToggleState (true, juce::dontSendNotification);
        frames = -1;
    }

    void attach()
    {
        window = nullptr;
        detachedSurface = nullptr;
        detachButton.setToggleState (false, juce::dontSendNotification);
        surface.message = {};
        sizeDecoder (surface.getLocalBounds());
        refreshControls();
        frames = -1;
    }

    AudioEngine& engine;
    std::function<ChannelDef* (Song&)> locate;
    int channel = -1;
    video::Settings settings;
    video::Info info;
    juce::String loadedPath, statusLine;
    std::vector<std::uint8_t> lastState;
    mutable std::shared_ptr<Instrument> heldInstrument;
    double nowTime = 0.0;
    int frames = -1;

    FrameDecoder decoder;
    VideoSurface surface;
    VideoSurface* detachedSurface = nullptr;
    std::unique_ptr<DetachedWindow> window;
    juce::Rectangle<int> panelArea;
    IconButton loadButton { IconButton::Icon::load }, optionsButton { IconButton::Icon::options }, setStartButton { IconButton::Icon::start },
        setEndButton { IconButton::Icon::end }, loopButton { IconButton::Icon::loop }, playButton { IconButton::Icon::play },
        stopButton { IconButton::Icon::stop }, soundButton { IconButton::Icon::sound }, detachButton { IconButton::Icon::detach };
    juce::TextButton slaveButton, locateButton;
    std::unique_ptr<ctlui::Knob> volume;
    ClickLabel startLabel, endLabel, nowLabel;
    TimeRail rail;
    std::unique_ptr<juce::FileChooser> chooser;
};
} // namespace

namespace videoview
{
void install (AudioEngine& engine)
{
    installedEngine = &engine;
    video::setAudioProvider ([] (const std::string& path, bool wait) { return provideTrack (path, wait); });
}

juce::String report (const juce::String& path)
{
    return probe (path).report;
}
} // namespace videoview

std::unique_ptr<juce::Component> makeVideoPlayerEditor (AudioEngine& engine, std::function<ChannelDef* (Song&)> locate, int channel)
{
    return std::make_unique<VideoPlayerEditor> (engine, std::move (locate), channel);
}

std::unique_ptr<juce::Component> makeVideoPlayerEditor (AudioEngine& engine, int channel)
{
    return makeVideoPlayerEditor (engine, [channel] (Song& s) -> ChannelDef* {
        return channel >= 0 && channel < (int) s.channels.size() ? &s.channels[(std::size_t) channel] : nullptr;
    }, channel);
}
} // namespace lynx
