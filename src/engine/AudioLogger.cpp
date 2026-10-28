#include "platform/Paths.h"
#include "engine/AudioLogger.h"

namespace lynx
{
AudioLogger::AudioLogger (AudioCapture& t) : juce::Thread ("LYNX audio logger"), tap (t) {}

AudioLogger::~AudioLogger()
{
    stop();
}

juce::File AudioLogger::defaultFolder()
{
    return lynx::platform::documentsFolder().getChildFile ("LYNX").getChildFile ("Audio logger");
}

bool AudioLogger::start (const juce::File& folder, double sampleRate)
{
    stop();
    if (sampleRate <= 0.0 || ! folder.createDirectory())
        return false;

    const auto name = "LYNX log " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S") + ".wav";
    auto target = folder.getChildFile (name);
    if (target.exists())
        target = target.getNonexistentSibling();

    auto stream = target.createOutputStream();
    if (stream == nullptr)
        return false;

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> out (stream.release());
    auto* w = wav.createWriterFor (out.get(), sampleRate, 2, 24, {}, 0);
    if (w == nullptr)
        return false;
    out.release(); // owned by the writer now

    {
        const juce::ScopedLock lock (writing);
        writer.reset (w);
        file = target;
        rate = sampleRate;
        framesWritten.store (0);
    }
    tap.setSource (0); // the Master
    startThread (juce::Thread::Priority::low);
    return true;
}

void AudioLogger::stop()
{
    if (writer == nullptr)
        return;
    signalThreadShouldExit();
    notify();
    stopThread (2000);
    tap.setSource (AudioCapture::off);
    drain();
    const juce::ScopedLock lock (writing);
    writer.reset(); // flushes and closes the file (the header gets its final length)
}

void AudioLogger::drain()
{
    const juce::ScopedLock lock (writing);
    if (writer == nullptr)
        return;
    left.clear();
    right.clear();
    if (tap.pull (left, right) <= 0)
        return;
    const float* channels[] = { left.data(), right.data() };
    writer->writeFromFloatArrays (channels, 2, (int) left.size());
    framesWritten.fetch_add ((std::int64_t) left.size());
}

void AudioLogger::run()
{
    // the ring holds ~5 s: emptying it ten times a second leaves plenty of room
    while (! threadShouldExit())
    {
        drain();
        wait (100);
    }
}
} // namespace lynx
