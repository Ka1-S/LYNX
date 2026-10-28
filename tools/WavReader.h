#pragma once

// Minimal WAV reader (PCM 8/16/24/32 and float 32, WavPack, FL's Ogg Vorbis: Ogg-in-WAV and .ogg, and MP3, decoded as
// the app decodes them, core/OggVorbis.h and core/Mp3Decoder.h) for the tools and tests. The app uses JUCE's readers for the rest (FLAC…).

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "core/AudioEdit.h"
#include "core/Instruments.h"
#include "core/Mp3Decoder.h"
#include "core/OggVorbis.h"
#include "core/WavPack.h"

namespace lynx::tools
{
inline std::shared_ptr<const SampleData> readWav (const std::string& path)
{
    std::ifstream in (path, std::ios::binary);

    if (! in)
        return nullptr;

    std::vector<std::uint8_t> d ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());

    // WavPack (.wv from FL's packs)
    if (d.size() >= 4 && std::memcmp (d.data(), "wvpk", 4) == 0)
    {
        DecodedAudio audio;
        if (! decodeWavPack (d.data(), d.size(), audio))
            return nullptr;
        auto out = std::make_shared<SampleData>();
        out->sampleRate = audio.sampleRate;
        out->left = std::move (audio.channels[0]);
        if (audio.channels.size() > 1)
            out->right = std::move (audio.channels[1]);
        return out;
    }

    // FL's Ogg Vorbis samples (WAV with an Ogg "data" chunk, or plain .ogg), like the app's SampleLibrary
    if (oggvorbis::isOggWav (d.data(), d.size()) || oggvorbis::isOggStream (d.data(), d.size()))
    {
        oggvorbis::Decoded audio;
        if (! oggvorbis::decode (d.data(), d.size(), audio) || audio.sampleRate <= 0 || audio.data.empty() || audio.data[0].empty())
            return nullptr;
        auto out = std::make_shared<SampleData>();
        out->sampleRate = audio.sampleRate;
        out->left = std::move (audio.data[0]);
        if (audio.data.size() > 1)
            out->right = std::move (audio.data[1]);
        return out;
    }

    // MP3 (round 157b: the app's reader, with FL's timing)
    if (mp3::looksLikeMp3 (d.data(), std::min<std::size_t> (d.size(), 1 << 16)))
    {
        mp3::Decoded audio;
        if (! mp3::decode (d.data(), d.size(), audio))
            return nullptr;
        auto out = std::make_shared<SampleData>();
        out->sampleRate = audio.info.sampleRate;
        out->left = std::move (audio.left);
        out->right = std::move (audio.right);
        return out;
    }

    if (d.size() < 12 || std::memcmp (d.data(), "RIFF", 4) != 0 || std::memcmp (d.data() + 8, "WAVE", 4) != 0)
        return nullptr;

    auto u16 = [&] (std::size_t o) { return (std::uint16_t) (d[o] | (d[o + 1] << 8)); };
    auto u32 = [&] (std::size_t o) { return (std::uint32_t) (u16 (o) | ((std::uint32_t) u16 (o + 2) << 16)); };

    std::uint16_t format = 0, channels = 0, bits = 0;
    std::uint32_t rate = 0;
    const std::uint8_t* data = nullptr;
    std::size_t dataSize = 0;
    std::int64_t loopStart = -1, loopEnd = -1;

    for (std::size_t pos = 12; pos + 8 <= d.size();)
    {
        const auto size = u32 (pos + 4);
        const auto body = pos + 8;

        if (std::memcmp (&d[pos], "fmt ", 4) == 0 && body + 16 <= d.size())
        {
            format = u16 (body);
            channels = u16 (body + 2);
            rate = u32 (body + 4);
            bits = u16 (body + 14);

            if (format == 0xFFFE && size >= 26) // WAVE_FORMAT_EXTENSIBLE: subformat in the first 2 bytes of the GUID
                format = u16 (body + 24);
        }
        else if (std::memcmp (&d[pos], "smpl", 4) == 0 && size >= 60 && body + 60 <= d.size() && u32 (body + 28) > 0)
        {
            loopStart = u32 (body + 44); // first loop: start and end frames (end inclusive)
            loopEnd = (std::int64_t) u32 (body + 48) + 1;
        }
        else if (std::memcmp (&d[pos], "data", 4) == 0)
        {
            data = &d[body];
            dataSize = std::min<std::size_t> (size, d.size() - body);
        }

        pos = body + size + (size & 1);
    }

    if (data == nullptr || channels == 0 || rate == 0 || bits == 0 || (format != 1 && format != 3))
        return nullptr;

    const auto bytesPerSample = bits / 8u;
    const auto frames = dataSize / (bytesPerSample * channels);
    auto sample = std::make_shared<SampleData>();
    sample->sampleRate = rate;
    if (loopStart >= 0 && loopEnd > loopStart && loopEnd <= (std::int64_t) frames)
        sample->loopStart = loopStart, sample->loopEnd = loopEnd;
    sample->left.resize (frames);

    if (channels > 1)
        sample->right.resize (frames);

    auto read = [&] (const std::uint8_t* p) -> float {
        if (format == 3 && bits == 32)
        {
            float f;
            std::memcpy (&f, p, 4);
            return f;
        }

        switch (bits)
        {
            case 8: return (p[0] - 128) / 128.0f;
            case 16: return (std::int16_t) (p[0] | (p[1] << 8)) / 32768.0f;
            case 24: return (std::int32_t) ((p[0] << 8) | (p[1] << 16) | ((std::uint32_t) p[2] << 24)) / 2147483648.0f;
            case 32: return (std::int32_t) (p[0] | (p[1] << 8) | (p[2] << 16) | ((std::uint32_t) p[3] << 24)) / 2147483648.0f;
            default: return 0.0f;
        }
    };

    for (std::size_t i = 0; i < frames; ++i)
    {
        const auto* frame = data + i * bytesPerSample * channels;
        sample->left[i] = read (frame);

        if (channels > 1)
            sample->right[i] = read (frame + bytesPerSample);
    }

    // cue points / regions and ACID beats, as the app's SampleLibrary keeps them (Fruity Granulizer's slices and sync)
    const auto extras = audioedit::decodeWavExtras (d.data(), d.size(), (std::int64_t) frames);
    sample->markerStarts = extras.markerStarts;
    sample->acidBeats = extras.acidBeats;
    return sample;
}
} // namespace lynx::tools
