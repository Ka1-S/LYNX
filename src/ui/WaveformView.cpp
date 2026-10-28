#include "ui/WaveformView.h"
#include "platform/Paths.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

#include "core/Sha256.h"
#include "engine/FileSort.h"
#include "core/Mp3Decoder.h"
#include "engine/Mp3Format.h"
#include "engine/WavPackFormat.h"
#include "ui/I18n.h"
#include "ui/Theme.h"

namespace lynx::ui
{
namespace
{
constexpr double longestDecodedSeconds = 20.0 * 60.0; // files decoded here (Browser): longer ones are skipped
constexpr int diskCacheMaxFiles = 600;

// path + size + modification time: a changed file gets a new entry
juce::String diskKey (const juce::File& file)
{
    const auto text = (file.getFullPathName() + "|" + juce::String (file.getSize()) + "|" + juce::String (file.getLastModificationTime().toMilliseconds())
                       + "|" + juce::String ((int) wave::formatVersion)
                       // MP3s: waveforms drawn from JUCE's decoder (before round 157b) are drawn again
                       + (file.hasFileExtension ("mp3;mp2;mpga") ? juce::String ("|") + mp3::decoderId : juce::String()))
                          .toStdString();
    return juce::String (sha256Hex (reinterpret_cast<const std::uint8_t*> (text.data()), text.size())).substring (0, 40);
}

void pruneDiskCache (const juce::File& folder)
{
    // one prune at a time (the pool's threads finish waveforms together), and not after every single new file
    static std::mutex pruning;
    static std::atomic<int> writes { 0 };
    if (writes.fetch_add (1) % 16 != 0)
        return;
    const std::lock_guard<std::mutex> guard (pruning);
    auto files = folder.findChildFiles (juce::File::findFiles, false, "*.lwv");
    if (files.size() <= diskCacheMaxFiles)
        return;
    filesort::byTime (files, filesort::By::accessTime); // (times read once: issue #12's crash)
    for (int i = 0; i < files.size() - diskCacheMaxFiles * 3 / 4; ++i)
        files.getReference (i).deleteFile();
}
} // namespace

// --- cache ---------------------------------------------------------------------------------------

class WaveformCache::Job final : public juce::ThreadPoolJob
{
public:
    Job (WaveformCache& c, juce::String p, std::shared_ptr<const SampleData> d)
        : juce::ThreadPoolJob ("waveform"),
          cache (c),
          path (std::move (p)),
          data (std::move (d))
    {
    }

    JobStatus runJob() override
    {
        cache.finished (path, build());
        return jobHasFinished;
    }

private:
    std::shared_ptr<const wave::Summary> build()
    {
        const juce::File file (path);
        const auto onDisk = file.existsAsFile();
        const auto folder = diskFolder();
        const auto cacheFile = onDisk ? folder.getChildFile (diskKey (file) + ".lwv") : juce::File();

        // 1. the disk cache
        if (onDisk && cacheFile.existsAsFile())
        {
            juce::MemoryBlock block;
            wave::Summary s;
            if (cacheFile.loadFileAsData (block) && wave::deserialise (static_cast<const std::uint8_t*> (block.getData()), block.getSize(), s))
            {
                cacheFile.setLastAccessTime (juce::Time::getCurrentTime());
                return std::make_shared<const wave::Summary> (std::move (s));
            }
            cacheFile.deleteFile();
        }

        // 2. the audio: from the engine, or decoded here
        if (data == nullptr && onDisk)
            data = decode (file);
        if (data == nullptr || data->frames() == 0 || cache.cancel.load())
            return nullptr;

        auto s = wave::analyse (data->leftData(), data->isStereo() ? data->rightData() : nullptr, data->frames(), data->sampleRate, &cache.cancel);
        data.reset();
        if (s.levels.empty())
            return nullptr;

        // 3. kept for the next session
        if (onDisk && folder.createDirectory())
        {
            const auto bytes = wave::serialise (s);
            const auto temp = cacheFile.withFileExtension ("tmp");
            if (temp.replaceWithData (bytes.data(), bytes.size()))
                temp.moveFileTo (cacheFile);
            pruneDiskCache (folder);
        }

        return std::make_shared<const wave::Summary> (std::move (s));
    }

    std::shared_ptr<const SampleData> decode (const juce::File& file)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (cache.formats.createReaderFor (file));
        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0
            || (double) reader->lengthInSamples / reader->sampleRate > longestDecodedSeconds)
            return nullptr;

        const auto frames = (int) reader->lengthInSamples;
        const auto stereo = reader->numChannels > 1;
        juce::AudioBuffer<float> buffer (stereo ? 2 : 1, frames);
        if (! reader->read (&buffer, 0, frames, 0, true, stereo))
            return nullptr;

        auto out = std::make_shared<SampleData>();
        out->sampleRate = reader->sampleRate;
        out->left.assign (buffer.getReadPointer (0), buffer.getReadPointer (0) + frames);
        if (stereo)
            out->right.assign (buffer.getReadPointer (1), buffer.getReadPointer (1) + frames);
        return out;
    }

    WaveformCache& cache;
    juce::String path;
    std::shared_ptr<const SampleData> data;
};

WaveformCache::WaveformCache()
{
    registerAudioFormats (formats); // LYNX's MP3 reader first (round 157b)
    formats.registerFormat (new WavPackAudioFormat(), false);
}

WaveformCache::~WaveformCache()
{
    cancel = true;
    pool.removeAllJobs (true, 10000);
    clearSingletonInstance();
}

juce::File WaveformCache::diskFolder()
{
    return lynx::platform::cacheFolder ("wavecache");
}

std::shared_ptr<const wave::Summary> WaveformCache::find (const juce::String& path, const Source& source)
{
    if (path.isEmpty())
        return nullptr;

    {
        std::lock_guard<std::mutex> guard (lock);
        if (auto it = ready.find (path); it != ready.end())
            return it->second;
        if (state.count (path) != 0)
            return nullptr;
        state[path] = true;
    }

    // UI thread: the engine's copy (usually already loaded, so no decoding here)
    auto data = source ? source() : nullptr;
    pool.addJob (new Job (*this, path, std::move (data)), true);
    return nullptr;
}

bool WaveformCache::isPending (const juce::String& path) const
{
    std::lock_guard<std::mutex> guard (lock);
    const auto it = state.find (path);
    return it != state.end() && it->second;
}

bool WaveformCache::hasFailed (const juce::String& path) const
{
    std::lock_guard<std::mutex> guard (lock);
    const auto it = state.find (path);
    return it != state.end() && ! it->second;
}

void WaveformCache::finished (const juce::String& path, std::shared_ptr<const wave::Summary> summary)
{
    {
        std::lock_guard<std::mutex> guard (lock);
        if (summary != nullptr)
        {
            ready[path] = std::move (summary);
            state.erase (path);
        }
        else
            state[path] = false;
    }
    if (! cancel.load())
        sendChangeMessage(); // any thread; delivered on the message thread
}

bool WaveformCache::waitUntilIdle (int timeoutMs)
{
    const auto until = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (pool.getNumJobs() > 0)
    {
        if (juce::Time::getMillisecondCounter() > until)
            return false;
        juce::Thread::sleep (5);
    }
    return true;
}

// --- drawing -------------------------------------------------------------------------------------

WaveStyle currentWaveStyle (juce::Colour classic, float gain)
{
    WaveStyle s;
    s.classic = classic;
    s.spectral = WaveformCache::spectral;
    s.stereo = WaveformCache::stereo;
    s.gain = gain;
    return s;
}

float normalisingGain (const wave::Summary& summary)
{
    return summary.peak > 0.0f ? std::min (1.0f / summary.peak, 15.85f) : 1.0f;
}

namespace
{
// The clean style: per lane, the peak envelope mirrored around the centre and filled as one anti-aliased
// path in a single colour; no RMS layer, no outline, so it reads calmly at any zoom. The envelope is computed
// per physical pixel and lightly smoothed (a 1-2-1 blend that never goes below the true peak), which removes
// one-pixel spikes without hiding transients.
void appendCleanWaveform (juce::Path& wave, float scale, juce::Rectangle<float> body, juce::Rectangle<float> visible, const wave::Summary& summary,
                          double startFrame, double framesPerPixel, const WaveStyle& style)
{
    const auto columns = (int) std::ceil (visible.getWidth() * scale) + 1;
    const auto framesPerColumn = framesPerPixel / scale;
    const auto firstFrame = startFrame + (double) (visible.getX() - body.getX()) * framesPerPixel;
    const auto lanes = style.stereo && summary.channels > 1 ? 2 : 1;
    const auto laneHeight = body.getHeight() / (float) lanes;

    std::vector<float> peak ((std::size_t) columns), smooth ((std::size_t) columns);
    std::vector<char> valid ((std::size_t) columns);

    for (int lane = 0; lane < lanes; ++lane)
    {
        for (int x = 0; x < columns; ++x)
        {
            auto from = firstFrame + (double) x * framesPerColumn;
            auto to = from + framesPerColumn;
            if (to < from)
                std::swap (from, to);
            // zoomed in closer than the summary's finest columns: look at least one column wide, centred, so the
            // envelope stays a smooth outline instead of beading along single cycles
            if (const auto minWidth = (double) summary.bucket * 1.5; to - from < minWidth)
            {
                const auto centre = (from + to) * 0.5;
                const auto end = (double) summary.frames;
                const auto inside = centre >= 0.0 && centre < end;
                from = std::max (0.0, centre - minWidth * 0.5);
                to = std::min (end, centre + minWidth * 0.5);
                if (! inside)
                    from = to = -1.0;
            }
            const auto p = from < 0.0 ? wave::Point {} : summary.query (from, to, lanes == 2 ? lane : -1);
            valid[(std::size_t) x] = p.valid ? 1 : 0;
            peak[(std::size_t) x] = p.valid ? std::min (1.0f, std::max (std::abs (p.hi), std::abs (p.lo)) * style.gain) : 0.0f;
        }

        for (int x = 0; x < columns; ++x)
        {
            const auto l = peak[(std::size_t) std::max (0, x - 1)], c = peak[(std::size_t) x], r = peak[(std::size_t) std::min (columns - 1, x + 1)];
            smooth[(std::size_t) x] = std::max (c, (l + 2.0f * c + r) * 0.25f);
        }

        const auto centre = body.getY() + laneHeight * ((float) lane + 0.5f);
        const auto half = laneHeight * 0.5f - 1.0f;
        const auto minHalf = 0.5f; // silence still shows as a hairline

        // one closed path per run of valid columns: the top edge left to right, the bottom edge back
        int x = 0;
        while (x < columns)
        {
            while (x < columns && ! valid[(std::size_t) x])
                ++x;
            if (x >= columns)
                break;
            const auto runStart = x;
            while (x < columns && valid[(std::size_t) x])
                ++x;
            const auto runEnd = x; // exclusive

            auto px = [&] (int i) { return visible.getX() + (float) i / scale; };
            wave.startNewSubPath (px (runStart), centre - std::max (minHalf, smooth[(std::size_t) runStart] * half));
            for (int i = runStart + 1; i < runEnd; ++i)
                wave.lineTo (px (i), centre - std::max (minHalf, smooth[(std::size_t) i] * half));
            wave.lineTo (px (runEnd), centre - std::max (minHalf, smooth[(std::size_t) (runEnd - 1)] * half));
            wave.lineTo (px (runEnd), centre + std::max (minHalf, smooth[(std::size_t) (runEnd - 1)] * half));
            for (int i = runEnd - 1; i >= runStart; --i)
                wave.lineTo (px (i), centre + std::max (minHalf, smooth[(std::size_t) i] * half));
            wave.closeSubPath();
        }
    }
}

void paintCleanWaveform (juce::Graphics& g, juce::Rectangle<float> body, juce::Rectangle<float> visible, const wave::Summary& summary,
                         double startFrame, double framesPerPixel, const WaveStyle& style)
{
    const auto scale = juce::jlimit (1.0f, 4.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    juce::Path wave;
    appendCleanWaveform (wave, scale, body, visible, summary, startFrame, framesPerPixel, style);
    g.setColour (style.classic);
    g.fillPath (wave);
}
} // namespace

bool appendWaveformPath (juce::Path& out, float scale, juce::Rectangle<float> body, juce::Rectangle<float> clip, const wave::Summary& summary,
                         double startFrame, double framesPerPixel, const WaveStyle& style)
{
    if (style.spectral)
        return false;
    const auto visible = body.getIntersection (clip);
    if (! visible.isEmpty() && ! summary.levels.empty() && framesPerPixel != 0.0 && body.getHeight() >= 2.0f)
        appendCleanWaveform (out, juce::jlimit (1.0f, 4.0f, scale), body, visible, summary, startFrame, framesPerPixel, style);
    return true;
}

void paintWaveform (juce::Graphics& g, juce::Rectangle<float> body, const wave::Summary& summary, double startFrame, double framesPerPixel,
                    const WaveStyle& style)
{
    const auto visible = body.getIntersection (g.getClipBounds().toFloat());
    if (visible.isEmpty() || summary.levels.empty() || framesPerPixel == 0.0 || body.getHeight() < 2.0f)
        return;

    if (! style.spectral)
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (body.getSmallestIntegerContainer());
        paintCleanWaveform (g, body, visible, summary, startFrame, framesPerPixel, style);
        return;
    }

    // one pixel buffer at physical resolution (sharp on high-DPI screens), reused between calls
    const auto scale = juce::jlimit (1.0f, 4.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    const auto width = (int) std::ceil (visible.getWidth() * scale);
    const auto height = (int) std::ceil (body.getHeight() * scale);
    auto& scratch = WaveformCache::getInstance()->scratchImage();
    if (! scratch.isValid() || scratch.getWidth() < width || scratch.getHeight() < height)
        scratch = juce::Image (juce::Image::ARGB, std::max (width, scratch.isValid() ? scratch.getWidth() : 0),
                               std::max (height, scratch.isValid() ? scratch.getHeight() : 0), false, juce::SoftwareImageType());

    const auto lanes = style.stereo && summary.channels > 1 ? 2 : 1;
    const auto laneHeight = (float) height / (float) lanes;
    const auto framesPerColumn = framesPerPixel / scale;
    const auto firstFrame = startFrame + (double) (visible.getX() - body.getX()) * framesPerPixel;
    const auto classic = style.classic;
    bool drewAny = false;

    {
        juce::Image::BitmapData pixels (scratch, 0, 0, width, height, juce::Image::BitmapData::writeOnly);

        for (int x = 0; x < width; ++x)
        {
            auto from = firstFrame + (double) x * framesPerColumn;
            auto to = from + framesPerColumn;
            if (to < from)
                std::swap (from, to);

            for (int lane = 0; lane < lanes; ++lane)
            {
                const auto top = (int) std::round ((float) lane * laneHeight);
                const auto bottom = (int) std::round ((float) (lane + 1) * laneHeight);
                const auto p = summary.query (from, to, lanes == 2 ? lane : -1);

                if (! p.valid)
                {
                    for (int y = top; y < bottom; ++y)
                        reinterpret_cast<juce::PixelARGB*> (pixels.getPixelPointer (x, y))->setARGB (0, 0, 0, 0);
                    continue;
                }

                drewAny = true;
                float r, gr, b;
                if (style.spectral)
                {
                    const auto c = wave::spectralColour (p);
                    r = c.r;
                    gr = c.g;
                    b = c.b;
                }
                else
                {
                    r = classic.getFloatRed();
                    gr = classic.getFloatGreen();
                    b = classic.getFloatBlue();
                }

                // louder = a little brighter
                const auto level = std::min (1.0f, p.rms * style.gain * 2.5f);
                const auto shade = 0.72f + 0.28f * level;

                const auto half = (float) (bottom - top) * 0.5f - 0.5f;
                const auto centre = (float) top + (float) (bottom - top) * 0.5f;
                const auto hi = std::clamp (p.hi * style.gain, -1.0f, 1.0f), lo = std::clamp (p.lo * style.gain, -1.0f, 1.0f);
                auto peakTop = centre - hi * half, peakBottom = centre - lo * half;
                if (peakBottom - peakTop < 1.0f) // never thinner than one pixel (the centre line)
                {
                    const auto mid = (peakTop + peakBottom) * 0.5f;
                    peakTop = mid - 0.5f;
                    peakBottom = mid + 0.5f;
                }
                const auto rms = std::min (p.rms * style.gain, 1.0f) * half;
                const auto rmsTop = std::max (peakTop, centre - rms), rmsBottom = std::min (peakBottom, centre + rms);

                auto cover = [] (float y, float a, float b2) { return std::clamp (std::min (y + 1.0f, b2) - std::max (y, a), 0.0f, 1.0f); };

                for (int y = top; y < bottom; ++y)
                {
                    const auto fy = (float) y;
                    const auto hull = cover (fy, peakTop, peakBottom);
                    auto* px = reinterpret_cast<juce::PixelARGB*> (pixels.getPixelPointer (x, y));
                    if (hull <= 0.0f)
                    {
                        px->setARGB (0, 0, 0, 0);
                        continue;
                    }

                    const auto body2 = rmsBottom > rmsTop ? cover (fy, rmsTop, rmsBottom) : 0.0f;
                    const auto edge = std::min (1.0f, cover (fy, peakTop, peakTop + 1.0f) + cover (fy, peakBottom - 1.0f, peakBottom));
                    // RMS body opaque; the peak hull translucent with a brighter one-pixel outline
                    const auto hullAlpha = (hull - body2) * (0.42f + 0.4f * edge);
                    const auto alpha = std::min (1.0f, body2 + hullAlpha);
                    const auto lift = 1.0f + 0.25f * edge * (1.0f - body2); // the outline is lighter
                    const auto k = alpha * shade;
                    px->setARGB ((juce::uint8) std::lround (alpha * 255.0f),
                                 (juce::uint8) std::lround (std::min (alpha, r * k * lift) * 255.0f),
                                 (juce::uint8) std::lround (std::min (alpha, gr * k * lift) * 255.0f),
                                 (juce::uint8) std::lround (std::min (alpha, b * k * lift) * 255.0f));
                }
            }
        }
    }

    if (! drewAny)
        return;

    juce::Graphics::ScopedSaveState save (g);
    g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
    g.drawImage (scratch, visible.getX(), body.getY(), visible.getWidth(), body.getHeight(), 0, 0, width, height);
}

// --- display component ---------------------------------------------------------------------------

WaveformDisplay::WaveformDisplay()
{
    setOpaque (true);
    WaveformCache::getInstance()->addChangeListener (this);
}

WaveformDisplay::~WaveformDisplay()
{
    if (auto* cache = WaveformCache::getInstanceWithoutCreating())
        cache->removeChangeListener (this);
}

void WaveformDisplay::setFile (const juce::String& newPath, WaveformCache::Source newSource)
{
    if (newPath == path)
        return;
    path = newPath;
    source = std::move (newSource);
    repaint();
}

void WaveformDisplay::setRegion (float start, float end)
{
    if (start == regionStart && end == regionEnd)
        return;
    regionStart = start;
    regionEnd = end;
    repaint();
}

void WaveformDisplay::setReversed (bool r)
{
    if (r != reversed)
    {
        reversed = r;
        repaint();
    }
}

void WaveformDisplay::setCaption (const juce::String& text)
{
    caption = text;
    repaint();
}

void WaveformDisplay::setEmptyText (const juce::String& text)
{
    emptyText = text;
    repaint();
}

void WaveformDisplay::paint (juce::Graphics& g)
{
    using namespace theme;
    const auto bounds = getLocalBounds().toFloat();
    g.fillAll (colour::bg0);
    g.setColour (colour::border);
    g.drawRect (getLocalBounds());

    const auto inner = bounds.reduced (1.0f);
    g.setColour (colour::gridBeat);
    g.fillRect (inner.getX(), inner.getCentreY() - 0.5f, inner.getWidth(), 1.0f);

    auto& cache = *WaveformCache::getInstance();
    const auto summary = cache.find (path, source);
    g.setFont (font (type::small));

    if (summary == nullptr)
    {
        g.setColour (colour::textDisabled);
        const auto text = path.isEmpty() ? emptyText
                          : cache.hasFailed (path) ? tr ("Could not read this audio file")
                                                   : tr ("Analysing...");
        g.drawText (text, getLocalBounds(), juce::Justification::centred);
        return;
    }

    const auto frames = (double) summary->frames;
    const auto perPixel = frames / (double) inner.getWidth();
    paintWaveform (g, inner, *summary, reversed ? frames : 0.0, reversed ? -perPixel : perPixel,
                   currentWaveStyle (colour::audioClip.brighter (0.3f), normalisingGain (*summary)));

    // the part outside the region is dimmed; its edges are marked in the accent colour
    if (regionStart > 0.0f || regionEnd < 1.0f)
    {
        const auto x0 = inner.getX() + inner.getWidth() * juce::jlimit (0.0f, 1.0f, reversed ? 1.0f - regionEnd : regionStart);
        const auto x1 = inner.getX() + inner.getWidth() * juce::jlimit (0.0f, 1.0f, reversed ? 1.0f - regionStart : regionEnd);
        g.setColour (colour::bg0.withAlpha (0.7f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (inner.getX(), inner.getY(), x0, inner.getBottom()));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (x1, inner.getY(), inner.getRight(), inner.getBottom()));
        g.setColour (colour::accent);
        g.fillRect (x0 - 0.5f, inner.getY(), 1.5f, inner.getHeight());
        g.fillRect (x1 - 1.0f, inner.getY(), 1.5f, inner.getHeight());
    }

    // caption: the file name and its length
    auto text = caption;
    if (text.isEmpty())
        text = juce::File (path).getFileName() + "  " + juce::String (frames / summary->sampleRate, 2) + " s"
               + (summary->channels > 1 ? "  " + tr ("stereo") : "  " + tr ("mono"));
    const auto band = getLocalBounds().reduced (6, 3).removeFromTop (16);
    g.setColour (colour::bg0.withAlpha (0.6f));
    g.fillRect (band.withWidth (juce::jmin (band.getWidth(), juce::roundToInt (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text)) + 8)));
    g.setColour (colour::textSecondary);
    g.drawText (text, band.withTrimmedLeft (4), juce::Justification::centredLeft, true);
}
} // namespace lynx::ui
