#include "engine/SampleLibrary.h"
#include "platform/Paths.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <set>

#include "core/OggVorbis.h"
#include "core/Mp3Decoder.h"
#include "core/TimeStretch.h"
#include "core/LoadTrace.h"
#include "engine/Mp3Format.h"
#include "engine/WavPackFormat.h"

namespace lynx
{
namespace
{
// Decoded samples are shared by every SampleLibrary of the process (the export's engine finds what the open project
// already decoded) and may be loaded from several threads at once (round 112: an open decodes its samples in parallel,
// off the message thread). A path being decoded is waited for, not decoded twice (its stream cache file is written once).
struct SharedCache
{
    std::mutex lock;
    std::condition_variable done;
    std::map<juce::String, std::weak_ptr<const SampleData>> entries;
    std::set<juce::String> loading;
    // stretched copies (pitch-preserving tempo follow): the latest length per file, kept (shared like the decoded ones)
    std::mutex stretchLock;
    std::map<juce::String, std::shared_ptr<const SampleData>> stretched;
};

SharedCache& sharedCache()
{
    static SharedCache c;
    return c;
}
} // namespace

namespace
{
// FL stores many factory samples as WAV whose "data" is an Ogg Vorbis stream
// (format tag 0x674F/0x6750/0x6751, "Og"). JUCE's WAV reader doesn't understand this.
std::unique_ptr<juce::MemoryBlock> extractOggFromWav (const juce::File& file)
{
    juce::MemoryBlock bytes;

    if (! file.loadFileAsData (bytes) || bytes.getSize() < 12)
        return nullptr;

    const auto* d = static_cast<const juce::uint8*> (bytes.getData());

    if (std::memcmp (d, "RIFF", 4) != 0 || std::memcmp (d + 8, "WAVE", 4) != 0)
        return nullptr;

    bool isOgg = false;

    for (size_t pos = 12; pos + 8 <= bytes.getSize();)
    {
        const auto size = (size_t) juce::ByteOrder::littleEndianInt (d + pos + 4);
        const auto body = pos + 8;

        if (std::memcmp (d + pos, "fmt ", 4) == 0 && body + 2 <= bytes.getSize())
        {
            const auto tag = juce::ByteOrder::littleEndianShort (d + body);
            isOgg = tag == 0x674F || tag == 0x6750 || tag == 0x6751;
        }
        else if (std::memcmp (d + pos, "data", 4) == 0 && isOgg)
        {
            const auto available = juce::jmin (size, bytes.getSize() - body);
            return std::make_unique<juce::MemoryBlock> (d + body, available);
        }

        pos = body + size + (size & 1);
    }

    return nullptr;
}
// Serves samples decoded up front (FL-exact Ogg Vorbis, core/OggVorbis.h) through JUCE's reader interface.
class DecodedReader final : public juce::AudioFormatReader
{
public:
    explicit DecodedReader (oggvorbis::Decoded d) : juce::AudioFormatReader (nullptr, "Ogg Vorbis (FL)"), decoded (std::move (d))
    {
        sampleRate = decoded.sampleRate;
        bitsPerSample = 32;
        usesFloatingPointData = true;
        numChannels = (unsigned int) decoded.channels;
        lengthInSamples = decoded.data.empty() ? 0 : (juce::int64) decoded.data[0].size();
    }

    bool readSamples (int* const* destSamples, int numDestChannels, int startOffsetInDestBuffer,
                      juce::int64 startSampleInFile, int numSamples) override
    {
        for (int c = 0; c < numDestChannels; ++c)
        {
            if (destSamples[c] == nullptr)
                continue;

            auto* dest = reinterpret_cast<float*> (destSamples[c]) + startOffsetInDestBuffer;

            if (c >= decoded.channels)
            {
                std::fill (dest, dest + numSamples, 0.0f);
                continue;
            }

            const auto& src = decoded.data[(size_t) c];

            for (int i = 0; i < numSamples; ++i)
            {
                const auto pos = startSampleInFile + i;
                dest[i] = pos >= 0 && pos < (juce::int64) src.size() ? src[(size_t) pos] : 0.0f;
            }
        }

        return true;
    }

private:
    oggvorbis::Decoded decoded;
};

// FL's Ogg-in-WAV samples and .ogg files, decoded the way FL decodes them (REA_rodada77); null when the file is
// something else or does not decode.
std::unique_ptr<juce::AudioFormatReader> createFlOggReader (const juce::File& file)
{
    juce::MemoryBlock head;

    if (auto in = file.createInputStream())
        in->readIntoMemoryBlock (head, 4096);

    const auto* h = static_cast<const std::uint8_t*> (head.getData());

    if (! oggvorbis::isOggWav (h, head.getSize()) && ! oggvorbis::isOggStream (h, head.getSize()))
        return nullptr;

    juce::MemoryBlock bytes;

    if (! file.loadFileAsData (bytes))
        return nullptr;

    oggvorbis::Decoded decoded;

    if (! oggvorbis::decode (static_cast<const std::uint8_t*> (bytes.getData()), bytes.getSize(), decoded)
        || decoded.sampleRate <= 0 || decoded.data.empty() || decoded.data[0].empty())
        return nullptr;

    return std::make_unique<DecodedReader> (std::move (decoded));
}
} // namespace

SampleLibrary::SampleLibrary()
{
    registerAudioFormats (formats); // LYNX's MP3 reader first (round 157b), then JUCE's
    formats.registerFormat (new WavPackAudioFormat(), false); // .wv files from FL packs
}

juce::String SampleLibrary::wildcard() const
{
    return formats.getWildcardForAllFormats();
}

std::unique_ptr<juce::AudioFormatReader> SampleLibrary::createReader (const juce::File& file)
{
    // before JUCE's own Ogg reader (vorbisfile), which drops blocks in many of FL's files (REA_rodada77 §3)
    if (auto reader = createFlOggReader (file))
        return reader;

    if (auto reader = std::unique_ptr<juce::AudioFormatReader> (formats.createReaderFor (file)))
        return reader;

    // Misleading extension (e.g. an Ogg saved as .wav): try every format by content.
    if (auto* reader = formats.createReaderFor (file.createInputStream()))
        return std::unique_ptr<juce::AudioFormatReader> (reader);

    if (auto ogg = extractOggFromWav (file))
    {
        juce::OggVorbisAudioFormat vorbis;

        // The reader takes ownership of the stream; on failure, JUCE itself deletes the stream.
        if (auto* reader = vorbis.createReaderFor (new juce::MemoryInputStream (std::move (*ogg)), true))
            return std::unique_ptr<juce::AudioFormatReader> (reader);
    }

    return nullptr;
}

namespace
{
// Time-stretched copies on disk (round 165i): opening a project stretched every "keep pitch" sample again (FL's Thief:
// 23 vocal chops, 2.0 s of the 4.4 s open). The copy is kept in the cache folder, keyed by the source file (path, size,
// date), the ratio, the method and the stretcher's own fingerprint — a short test signal stretched by this build — so
// a changed algorithm never reuses an old copy. The floats are stored as computed: what plays is bit for bit the same.
constexpr char stretchMagic[8] = { 'L', 'Y', 'N', 'X', 'S', 'T', 'R', '1' };
constexpr int stretchHeader = 32;
constexpr juce::int64 stretchCacheLimit = 1024LL * 1024 * 1024; // pruned (oldest first) to 3/4 of it when exceeded

std::uint64_t fnv (const void* data, std::size_t bytes, std::uint64_t h = 1469598103934665603ULL) noexcept
{
    const auto* b = static_cast<const unsigned char*> (data);
    for (std::size_t i = 0; i < bytes; ++i)
        h = (h ^ b[i]) * 1099511628211ULL;
    return h;
}

std::uint64_t stretchFingerprint (StretchMethod method)
{
    static std::array<std::once_flag, 3> once;
    static std::array<std::uint64_t, 3> prints {};
    const auto m = (std::size_t) juce::jlimit (0, 2, (int) method);
    std::call_once (once[m], [m] {
        SampleData test;
        test.sampleRate = 44100.0;
        std::uint32_t noise = 12345;
        for (int i = 0; i < 6000; ++i)
        {
            noise = noise * 1664525u + 1013904223u;
            const auto n = (float) (noise >> 8) / 16777216.0f - 0.5f;
            const auto t = (double) i / test.sampleRate;
            test.left.push_back ((float) (0.5 * std::sin (2.0 * 3.14159265358979 * 220.0 * t)) + 0.1f * n + (i % 1500 == 0 ? 0.8f : 0.0f));
            test.right.push_back ((float) (0.4 * std::sin (2.0 * 3.14159265358979 * 331.0 * t)) - 0.1f * n);
        }
        const auto out = timeStretch (test, 2.6, (StretchMethod) m); // (beyond 2x: the seeded randomised steps count too)
        auto h = fnv (out->left.data(), out->left.size() * sizeof (float));
        prints[m] = fnv (out->right.data(), out->right.size() * sizeof (float), h);
    });
    return prints[m];
}

juce::File stretchCacheFile (const juce::String& path, double ratio, StretchMethod method)
{
    const auto source = platform::localFile (path);
    if (! source.existsAsFile())
        return {};
    const auto key = source.getFullPathName() + "|" + juce::String (source.getSize()) + "|" + juce::String (source.getLastModificationTime().toMilliseconds())
                   + "|" + juce::String (ratio, 10) + "|" + juce::String ((int) method) + "|" + juce::String::toHexString ((juce::int64) stretchFingerprint (method));
    return SampleLibrary::cacheFolder().getChildFile (juce::String::toHexString (key.hashCode64()) + ".str");
}

std::shared_ptr<SampleData> readStretched (const juce::File& file)
{
    juce::MemoryBlock block;
    if (file == juce::File() || ! file.existsAsFile() || ! file.loadFileAsData (block) || block.getSize() < (std::size_t) stretchHeader)
        return nullptr;
    const auto* d = static_cast<const char*> (block.getData());
    if (std::memcmp (d, stretchMagic, 8) != 0)
        return nullptr;
    double rate = 0.0;
    juce::int64 frames = 0;
    std::int32_t channels = 0;
    std::memcpy (&rate, d + 8, 8);
    std::memcpy (&frames, d + 16, 8);
    std::memcpy (&channels, d + 24, 4);
    if (frames <= 0 || (channels != 1 && channels != 2) || ! (rate > 0.0)
        || (juce::int64) block.getSize() != stretchHeader + frames * channels * (juce::int64) sizeof (float))
        return nullptr;
    auto out = std::make_shared<SampleData>();
    out->sampleRate = rate;
    const auto* samples = reinterpret_cast<const float*> (d + stretchHeader);
    out->left.assign (samples, samples + frames);
    if (channels == 2)
        out->right.assign (samples + frames, samples + 2 * frames);
    file.setLastModificationTime (juce::Time::getCurrentTime()); // (recently used: pruned last)
    return out;
}

void pruneStretchCache()
{
    // writes cut short by quitting (an hour old: not one another LYNX is writing now)
    for (const auto& t : SampleLibrary::cacheFolder().findChildFiles (juce::File::findFiles, false, "*.str.*.tmp"))
        if (t.getLastModificationTime() < juce::Time::getCurrentTime() - juce::RelativeTime::hours (1))
            t.deleteFile();
    auto files = SampleLibrary::cacheFolder().findChildFiles (juce::File::findFiles, false, "*.str");
    juce::int64 total = 0;
    for (const auto& f : files)
        total += f.getSize();
    if (total <= stretchCacheLimit)
        return;
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) { return a.getLastModificationTime() < b.getLastModificationTime(); });
    for (const auto& f : files)
    {
        if (total <= stretchCacheLimit * 3 / 4)
            break;
        total -= f.getSize();
        f.deleteFile();
    }
}

void writeStretched (const juce::File& file, const SampleData& data)
{
    if (file == juce::File() || data.isMapped() || data.left.empty() || (! data.right.empty() && data.right.size() != data.left.size()))
        return;
    static std::once_flag pruned;
    std::call_once (pruned, pruneStretchCache);
    const auto temp = file.getSiblingFile (file.getFileName() + "." + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()) + ".tmp");
    auto ok = false;
    {
        juce::FileOutputStream out (temp);
        if (out.failedToOpen())
            return;
        const auto frames = (juce::int64) data.left.size();
        const std::int32_t channels = data.right.empty() ? 1 : 2;
        char header[stretchHeader] {};
        std::memcpy (header, stretchMagic, 8);
        std::memcpy (header + 8, &data.sampleRate, 8);
        std::memcpy (header + 16, &frames, 8);
        std::memcpy (header + 24, &channels, 4);
        ok = out.write (header, stretchHeader) && out.write (data.left.data(), data.left.size() * sizeof (float));
        if (ok && channels == 2)
            ok = out.write (data.right.data(), data.right.size() * sizeof (float));
        out.flush();
        ok = ok && out.getStatus().wasOk();
    }
    if (! ok)
    {
        temp.deleteFile();
        return;
    }
    if (! temp.moveFileTo (file)) // (another LYNX wrote it meanwhile: keep that one)
        temp.deleteFile();
}
// The disk copy is written in the background, off the project open (round 165i: on Windows the 23 stretched copies of
// Thief, 229 MB, took 14.7 s of the threads' time to write, the open waited 7 s more than without the cache). One
// low-priority thread, never destroyed: a write cut short by quitting leaves a .tmp, which the next prune removes.
void writeStretchedLater (const juce::File& file, std::shared_ptr<const SampleData> data)
{
    static auto* writer = new juce::ThreadPool (juce::ThreadPoolOptions{}
                                                    .withThreadName ("LYNX stretch cache")
                                                    .withNumberOfThreads (1)
                                                    .withDesiredThreadPriority (juce::Thread::Priority::background));
    writer->addJob ([file, data = std::move (data)] { writeStretched (file, *data); });
}
} // namespace

std::shared_ptr<const SampleData> SampleLibrary::loadStretched (const juce::String& path, double ratio, StretchMethod method)
{
    // 10 significant digits: ratios within 0.1 % of 1 are resampled to the exact length, so 4 decimals were too coarse
    const auto key = path + "|" + juce::String (ratio, 10) + "|" + juce::String ((int) method);
    auto& shared = sharedCache();

    {
        const std::scoped_lock l (shared.stretchLock);
        if (auto it = shared.stretched.find (key); it != shared.stretched.end())
            return it->second;
    }

    static const bool useDisk = [] { const auto* e = std::getenv ("LYNX_STRETCH_DISK"); return e == nullptr || e[0] != '0'; }();
    const auto onDisk = useDisk ? stretchCacheFile (path, ratio, method) : juce::File();
    std::shared_ptr<const SampleData> stretched;
    {
        const loadtrace::Scope timing ("stretch_disk_read");
        stretched = readStretched (onDisk);
    }
    if (stretched == nullptr)
    {
        auto original = load (path);

        if (original == nullptr)
            return nullptr;

        std::shared_ptr<SampleData> made;
        {
            const loadtrace::Scope timing ("stretch_compute");
            made = timeStretch (*original, ratio, method);
        }
        stretched = std::move (made);
        if (stretched != nullptr && onDisk != juce::File())
            writeStretchedLater (onDisk, stretched);
    }

    // the other lengths of this file go when nothing uses them any more (changing the BPM replaces the stretched one).
    // Round 165i: every other length was dropped, also the ones the song being opened still needed — two channels
    // stretching one sample to different lengths (Thief's vocal chops) were stretched again on the message thread
    // while building the rack (rack_stretch 754 ms), each evicting the other
    const std::scoped_lock l (shared.stretchLock);
    for (auto it = shared.stretched.begin(); it != shared.stretched.end();)
        it = it->first.startsWith (path + "|") && it->second.use_count() == 1 ? shared.stretched.erase (it) : std::next (it);

    shared.stretched[key] = stretched;
    return stretched;
}

juce::File SampleLibrary::cacheFolder()
{
    // (under a test profile, LYNX_PROFILE_DIR: inside it)
    const auto profile = platform::profileOverride();
    auto folder = (profile != juce::File() ? profile.getChildFile ("cache") : juce::File::getSpecialLocation (juce::File::tempDirectory)).getChildFile ("LYNX-cache");
    folder.createDirectory();
    return folder;
}

namespace
{
// One cache file per channel: 32-byte header + contiguous float32.
constexpr char cacheMagic[8] = { 'L', 'Y', 'N', 'X', 'P', 'C', 'M', '1' };
constexpr int cacheHeader = 32;

struct Mapped
{
    std::unique_ptr<juce::MemoryMappedFile> left, right;
};

bool writeHeader (juce::OutputStream& out, double sampleRate, juce::int64 frames)
{
    out.write (cacheMagic, 8);
    out.writeDouble (sampleRate);
    out.writeInt64 (frames);
    out.writeInt64 (0);
    return out.getPosition() == cacheHeader;
}

std::unique_ptr<juce::MemoryMappedFile> mapChannel (const juce::File& file, double sampleRate, juce::int64 frames)
{
    if (file.getSize() != cacheHeader + frames * (juce::int64) sizeof (float))
        return nullptr;

    auto mapped = std::make_unique<juce::MemoryMappedFile> (file, juce::MemoryMappedFile::readOnly);
    const auto* d = static_cast<const char*> (mapped->getData());

    if (d == nullptr || (juce::int64) mapped->getSize() != file.getSize() || std::memcmp (d, cacheMagic, 8) != 0)
        return nullptr;

    double rate = 0.0;
    juce::int64 count = 0;
    std::memcpy (&rate, d + 8, 8);
    std::memcpy (&count, d + 16, 8);
    return rate == sampleRate && count == frames ? std::move (mapped) : nullptr;
}

// Reads one sample per page so the OS brings the range in from disk now (off the audio thread).
float touch (const float* data, std::size_t from, std::size_t to)
{
    float sum = 0.0f;
    for (auto i = from; i < to; i += 1024)
        sum += data[i];
    return sum;
}
} // namespace

std::shared_ptr<const SampleData> SampleLibrary::loadStreamed (const juce::File& file, juce::AudioFormatReader& reader)
{
    const auto frames = reader.lengthInSamples;
    const auto channels = juce::jmin (2, (int) reader.numChannels);
    const auto key = juce::String::toHexString ((file.getFullPathName() + "|" + juce::String (file.getSize()) + "|"
                                                 + juce::String (file.getLastModificationTime().toMilliseconds())
                                                 // the decoder too: a file read by another one is decoded again
                                                 + (reader.getFormatName() == Mp3AudioFormat::formatName ? juce::String ("|") + mp3::decoderId : juce::String()))
                                                    .hashCode64());
    const auto leftFile = cacheFolder().getChildFile (key + ".L.pcm");
    const auto rightFile = cacheFolder().getChildFile (key + ".R.pcm");

    auto mapped = std::make_shared<Mapped>();
    mapped->left = mapChannel (leftFile, reader.sampleRate, frames);
    mapped->right = channels > 1 ? mapChannel (rightFile, reader.sampleRate, frames) : nullptr;

    if (mapped->left == nullptr || (channels > 1 && mapped->right == nullptr))
    {
        // decode once, in chunks, straight into the cache files
        leftFile.deleteFile();
        rightFile.deleteFile();
        {
            juce::FileOutputStream outL (leftFile);
            std::unique_ptr<juce::FileOutputStream> outR;

            if (channels > 1)
                outR = std::make_unique<juce::FileOutputStream> (rightFile);

            if (outL.failedToOpen() || (outR != nullptr && outR->failedToOpen()) || ! writeHeader (outL, reader.sampleRate, frames)
                || (outR != nullptr && ! writeHeader (*outR, reader.sampleRate, frames)))
                return nullptr;

            constexpr int block = 1 << 16;
            juce::AudioBuffer<float> buffer (channels, block);

            for (juce::int64 done = 0; done < frames; done += block)
            {
                const auto n = (int) juce::jmin<juce::int64> (block, frames - done);
                reader.read (&buffer, 0, n, done, true, channels > 1);
                outL.write (buffer.getReadPointer (0), (size_t) n * sizeof (float));

                if (outR != nullptr)
                    outR->write (buffer.getReadPointer (1), (size_t) n * sizeof (float));
            }

            outL.flush();
            if (outR != nullptr)
                outR->flush();
        }

        mapped->left = mapChannel (leftFile, reader.sampleRate, frames);
        mapped->right = channels > 1 ? mapChannel (rightFile, reader.sampleRate, frames) : nullptr;

        if (mapped->left == nullptr || (channels > 1 && mapped->right == nullptr))
            return nullptr;
    }

    auto data = std::make_shared<SampleData>();
    data->sampleRate = reader.sampleRate;
    data->mappedFrames = (std::size_t) frames;
    data->mappedLeft = reinterpret_cast<const float*> (static_cast<const char*> (mapped->left->getData()) + cacheHeader);
    data->mappedRight = mapped->right != nullptr
                            ? reinterpret_cast<const float*> (static_cast<const char*> (mapped->right->getData()) + cacheHeader)
                            : nullptr;
    data->mapping = mapped;

    // the first seconds already in memory: the clip start doesn't wait for the disk
    const auto head = juce::jmin ((std::size_t) frames, (std::size_t) (reader.sampleRate * 3.0));
    volatile float sink = touch (data->leftData(), 0, head) + touch (data->rightData(), 0, head);
    juce::ignoreUnused (sink);
    return data;
}

void prefetchSample (const SampleData& data, double fromSeconds, double toSeconds)
{
    if (! data.isMapped())
        return;

    const auto from = (std::size_t) juce::jlimit (0.0, (double) data.frames(), fromSeconds * data.sampleRate);
    const auto to = (std::size_t) juce::jlimit (0.0, (double) data.frames(), toSeconds * data.sampleRate);
    volatile float sink = touch (data.leftData(), from, to) + (data.isStereo() ? touch (data.rightData(), from, to) : 0.0f);
    juce::ignoreUnused (sink);
}

void SampleLibrary::forget (const juce::String& path)
{
    auto& shared = sharedCache();
    {
        const std::scoped_lock l (shared.lock);
        shared.entries.erase (path);
    }
    const std::scoped_lock l (shared.stretchLock);
    for (auto it = shared.stretched.begin(); it != shared.stretched.end();)
        it = it->first.startsWith (path + "|") ? shared.stretched.erase (it) : std::next (it);
}

std::shared_ptr<const SampleData> SampleLibrary::load (const juce::String& path)
{
    auto& shared = sharedCache();
    {
        std::unique_lock l (shared.lock);
        for (;;)
        {
            if (const auto it = shared.entries.find (path); it != shared.entries.end())
                if (auto cached = it->second.lock())
                    return cached;
            if (shared.loading.count (path) == 0)
                break;
            shared.done.wait (l);
        }
        shared.loading.insert (path);
    }
    auto data = decode (path);
    {
        const std::scoped_lock l (shared.lock);
        shared.loading.erase (path);
        if (data != nullptr)
            shared.entries[path] = data;
    }
    shared.done.notify_all();
    return data;
}

std::shared_ptr<const SampleData> SampleLibrary::findLoaded (const juce::String& path)
{
    auto& shared = sharedCache();
    const std::scoped_lock l (shared.lock);
    if (const auto it = shared.entries.find (path); it != shared.entries.end())
        return it->second.lock();
    return nullptr;
}

std::shared_ptr<const SampleData> SampleLibrary::decode (const juce::String& path)
{
    const auto file = platform::localFile (path); // FL's Windows spelling on macOS/Linux (platform/Paths.h)

    if (! file.existsAsFile())
        return nullptr;

    auto reader = createReader (file);

    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels == 0)
        return nullptr;

    if ((double) reader->lengthInSamples > reader->sampleRate * streamAboveSeconds)
        if (auto streamed = loadStreamed (file, *reader))
            return streamed;
    // In memory: 10-minute safety limit (long ones go through the disk cache).
    const auto frames = (int) juce::jmin<juce::int64> (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 600.0));
    const auto channels = juce::jmin (2, (int) reader->numChannels);

    juce::AudioBuffer<float> buffer (channels, frames);
    reader->read (&buffer, 0, frames, 0, true, channels > 1);

    auto data = std::make_shared<SampleData>();
    data->sampleRate = reader->sampleRate;
    data->left.assign (buffer.getReadPointer (0), buffer.getReadPointer (0) + frames);
    // the file's own loop (WAV smpl, first loop; JUCE gives the end frame inclusive)
    {
        const auto& meta = reader->metadataValues;
        if (meta.getValue ("NumSampleLoops", "0").getIntValue() > 0)
        {
            const auto start = meta.getValue ("Loop0Start", "-1").getLargeIntValue();
            const auto end = meta.getValue ("Loop0End", "-1").getLargeIntValue() + 1;
            if (start >= 0 && end > start && end <= frames)
                data->loopStart = start, data->loopEnd = end;
        }
    }

    if (channels > 1)
        data->right.assign (buffer.getReadPointer (1), buffer.getReadPointer (1) + frames);

    return data;
}
} // namespace lynx
