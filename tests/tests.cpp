// Core tests (no JUCE). Usage: lynx_tests [folder-with-flp-files]

#include <optional>
#include <atomic>
#include <chrono>
#include <thread>
#include <random>
#include <cctype>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>

#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__) || defined(__aarch64__) || defined(_M_ARM64) // x86 SSE, or ARM64 through core/Simd.h (sse2neon)
#include "core/Simd.h"
#endif

#include "core/Dither.h"
#include "core/DrumSynthLive.h"
#include "core/Effects.h"
#include "core/MixSuite.h"
#include "core/EmberClassic.h"
#include "core/Effector.h"
#include "core/FruityDelayBank.h"
#include "core/FruityMultiband.h"
#include "core/Denormals.h"
#include "core/TestProfile.h"
#include "core/FlpImport.h"
#include "core/MidiControls.h"
#include "core/MidiImport.h"
#include "core/ChannelMiscFx.h"
#include "core/FlEffects.h"
#include "core/ProjectContents.h"
#include "core/FlPatcher.h"
#include "core/Patcher.h"
#include "core/Layer.h"
#include "core/XyController.h"
#include "core/ControlSurface.h"
#include "core/VoltageController.h"
#include "core/FlSamplers.h"
#include "core/FlSynths.h"
#include "core/Inflate.h"
#include "core/WavPack.h"
#include "core/FlWrapper.h"
#include "core/FlPluginDb.h"
#include "core/MissingFiles.h"
#include "core/PianoRollTools.h"
#include "core/EventEdit.h"
#include "core/Preferences.h"
#include "core/History.h"
#include "core/Gms.h"
#include "core/PoiZone.h"
#include "core/MiniSynth.h"
#include "core/SimSynth.h"
#include "core/Granulizer.h"
#include "core/Drumaxx.h"
#include "core/Wasp.h"
#include "core/PluginLayout.h"
#include "core/Plucked.h"
#include "core/BeepMap.h"
#include "core/Sawer.h"
#include "core/Slayer.h"
#include "core/Toxic.h"
#include "core/Harmless.h"
#include "core/Harmor.h"
#include "core/Morphine.h"
#include "core/Ogun.h"
#include "core/BassDrum.h"
#include "core/FruitKick.h"
#include "core/IppCos.h"
#include "core/Dx10.h"
#include "core/Kepler.h"
#include "core/ThreeOsc.h"
#include "core/FlResampler.h"
#include "core/FlKeys.h"
#include "core/DirectWave.h"
#include "core/OggVorbis.h"
#include "core/FlVoiceProcessor.h"
#include "core/TransistorBass.h"
#include "core/HarmlessFx.h"
#include "core/MaximusFx.h"
#include "core/EmphasizerFx.h"
#include "core/FreqShifterFl.h"
#include "core/LowLifterFl.h"
#include "core/Reeverb2.h"
#include "core/LuxeVerb.h"
#include "core/Denormals.h"
#include "core/FruityDynamics.h"
#include "core/FruityShapers.h"
#include "core/ParametricEqFl.h"
#include "core/FruityBasicFx.h"
#include "core/EquoFx.h"
#include "core/DistructorFx.h"
#include "core/MultibandDelayFx.h"
#include "core/PitchShifterFl.h"
#include "core/FruityFilters.h"
#include "core/NewtoneFx.h"
#include "core/FruityDelay.h"
#include "core/StereoFx.h"
#include "core/FrequencySplitter.h"
#include "core/ChorusFx.h"
#include "core/GrossBeat.h"
#include "core/LovePhilter.h"
#include "core/FlangusFastDist.h"
#include "core/FruityPhaser.h"
#include "core/FruityVocoder.h"
#include "core/DelphiMath.h"
#include "core/LiveEventQueue.h"
#include "core/RackRenderer.h"
#include "core/Scheduler.h"
#include "core/SnapshotExchange.h"
#include "core/SoftClip.h"
#include "core/Slicer.h"
#include "core/SytrusEngine.h"
#include "core/Templates.h"
#include "core/Vocodex.h"
#include "core/TimeStretch.h"
#include "core/WorkerPool.h"
#include "core/RtStats.h"
#include "core/FixedBlock.h"
#include "core/ParametricEqLinear.h"
#include "core/Song.h"
#include "core/NoteEdit.h"
#include "core/Recorder.h"
#include "core/KeyMap.h"
#include "core/RackOps.h"
#include "core/ValueMath.h"
#include "core/Workspace.h"
#include "core/StartupSound.h"
#include "core/ExportOptions.h"
#include "core/Mp3Encoder.h"
#include "core/PluginMonitor.h"
#include "core/PluginSubstitutes.h"
#include "core/ClipOps.h"
#include "core/PlaylistOps.h"
#include "core/TouchKeyboard.h"
#include "core/HeldNotes.h"
#include "core/Templates.h"
#include "flp/Flp.h"
#include "ui/I18nData.h"
#include "core/CrashReport.h"

namespace
{
int failures = 0;
int checks = 0;
std::vector<std::pair<const char*, std::function<void()>>>& registry()
{
    static std::vector<std::pair<const char*, std::function<void()>>> r;
    return r;
}

struct Register
{
    Register (const char* name, std::function<void()> fn) { registry().emplace_back (name, std::move (fn)); }
};

#define TEST(name)                                   \
    static void name();                              \
    static Register reg_##name (#name, name);        \
    static void name()

#define CHECK(cond)                                                                      \
    do                                                                                   \
    {                                                                                    \
        ++checks;                                                                        \
        if (! (cond))                                                                    \
        {                                                                                \
            ++failures;                                                                  \
            std::printf ("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                                \
    } while (false)

// Engines that compute in x87 80-bit long double (FL's own arithmetic) match their goldens to the last bit on x86. On
// ARM64 (Apple Silicon) long double is not the x87 format, so there they hold to a looser bound (round 117).
// LYNX_X87_TOL overrides it (measuring).
inline float armTolerance (float x86, float arm)
{
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    (void) arm;
    return x86;
#else
    if (const char* t = std::getenv ("LYNX_X87_TOL"))
        return (float) std::atof (t);
    return arm;
#endif
}
inline float x87Tolerance() { return armTolerance (2.0e-7f, 2.0e-4f); }
inline float granulizerTolerance()
{
#if defined(__APPLE__)
    return 2.0e-4f;
#else
    return armTolerance (1.0e-6f, 1.0e-5f);
#endif
}

// Engines whose goldens depend on the C library's sin/cos/exp/pow: Windows (mingw), glibc and musl hold the 1e-7
// goldens, Apple's libm does not (round 117), so on macOS the per-sample bound is looser.
inline float libmTolerance (float strict)
{
#if defined(__APPLE__)
    (void) strict;
    return 2.0e-4f;
#else
    return strict;
#endif
}

// One golden sample: relative bound (with a 1e-3 floor on macOS, where near-zero samples carry libm noise); prints the
// measured error when it fails.
inline bool goldenSampleClose (float y, float golden, float tol)
{
#if defined(__APPLE__)
    const float scale = std::max (1.0e-3f, std::abs (golden));
#else
    const float scale = std::max (1.0e-16f, std::abs (golden));
#endif
    if (std::abs (y - golden) <= tol * scale)
        return true;
    std::fprintf (stderr, "  golden sample %.9g vs %.9g (relative error %.3g, bound %.3g)\n", (double) y, (double) golden,
                  (double) (std::abs (y - golden) / scale), (double) tol);
    return false;
}

std::string realFlpDir;

// --- helpers for building synthetic .flp files -----------------------------

struct FlpBuilder
{
    std::vector<std::uint8_t> bytes;

    FlpBuilder (std::uint16_t ppq = 96)
    {
        put ("FLhd");
        u32 (6);
        u16 (0);
        u16 (1);
        u16 (ppq);
        put ("FLdt");
        u32 (0); // size is not used by the reader
    }

    void put (const char* s) { bytes.insert (bytes.end(), s, s + std::strlen (s)); }
    void u8 (std::uint8_t v) { bytes.push_back (v); }
    void u16 (std::uint16_t v) { u8 ((std::uint8_t) v); u8 ((std::uint8_t) (v >> 8)); }
    void u32 (std::uint32_t v) { u16 ((std::uint16_t) v); u16 ((std::uint16_t) (v >> 16)); }

    void varint (std::size_t n)
    {
        do
        {
            auto b = (std::uint8_t) (n & 0x7F);
            n >>= 7;
            u8 (n ? (std::uint8_t) (b | 0x80) : b);
        } while (n);
    }

    void data (std::uint8_t id, const std::vector<std::uint8_t>& payload)
    {
        u8 (id);
        varint (payload.size());
        bytes.insert (bytes.end(), payload.begin(), payload.end());
    }

    void ascii (std::uint8_t id, const std::string& s)
    {
        std::vector<std::uint8_t> p (s.begin(), s.end());
        p.push_back (0);
        data (id, p);
    }

    void utf16 (std::uint8_t id, const std::u16string& s)
    {
        std::vector<std::uint8_t> p;
        for (auto c : s)
        {
            p.push_back ((std::uint8_t) c);
            p.push_back ((std::uint8_t) (c >> 8));
        }
        p.push_back (0);
        p.push_back (0);
        data (id, p);
    }
};

std::vector<std::uint8_t> noteRecord (std::uint32_t pos, std::uint16_t channel, std::uint32_t len, std::uint16_t key,
                                      std::uint8_t velocity)
{
    std::vector<std::uint8_t> r (24, 0);
    std::memcpy (&r[0], &pos, 4);
    std::memcpy (&r[6], &channel, 2);
    std::memcpy (&r[8], &len, 4);
    std::memcpy (&r[12], &key, 2);
    r[16] = 120;
    r[20] = 64;
    r[21] = velocity;
    return r;
}
} // namespace

// ---------------------------------------------------------------------------
// FLP

TEST (flp_rejects_garbage)
{
    std::vector<std::uint8_t> junk (64, 0x41);
    bool threw = false;
    try { flp::EventReader r (junk); } catch (const flp::Error&) { threw = true; }
    CHECK (threw);
}

TEST (flp_reads_size_classes_varint_and_extended_ids)
{
    FlpBuilder b;
    b.u8 (0x15); b.u8 (2);                 // u8
    b.u8 (0x40); b.u16 (513);              // u16
    b.u8 (0x9C); b.u32 (128500);           // u32
    b.data (0xD5, std::vector<std::uint8_t> (300, 7)); // 2-byte varint
    b.u8 (0xAC); b.u32 (0xC0000100); b.varint (3); b.u8 (1); b.u8 (2); b.u8 (3); // extended ID

    flp::EventReader r (b.bytes);
    flp::Event e;
    CHECK (r.header().ppq == 96);
    CHECK (r.next (e) && e.id == 0x15 && e.value == 2);
    CHECK (r.next (e) && e.id == 0x40 && e.value == 513);
    CHECK (r.next (e) && e.id == 0x9C && e.value == 128500);
    CHECK (r.next (e) && e.id == 0xD5 && e.data.size() == 300 && e.data[299] == 7);
    CHECK (r.next (e) && e.id == 0xC0000100 && e.isData() && e.data.size() == 3 && e.data[2] == 3);
    CHECK (! r.next (e));
}

TEST (flp_truncated_file_throws)
{
    FlpBuilder b;
    b.u8 (0x9C); b.u8 (1); // u32 with only 1 byte
    flp::EventReader r (b.bytes);
    flp::Event e;
    bool threw = false;
    try { r.next (e); } catch (const flp::Error&) { threw = true; }
    CHECK (threw);
}

TEST (flp_version_numbers)
{
    CHECK (flp::parseVersionNumber ("25.2.5.5319") == 25002005u);
    CHECK (flp::parseVersionNumber ("20.99.3000.3031") == 20102000u);
    CHECK (flp::parseVersionNumber ("11.0.3") == 11000003u);
}

TEST (flp_playlist_record_size_by_version)
{
    CHECK (flp::playlistRecordSize (12009003, 64) == 32);
    CHECK (flp::playlistRecordSize (21000000, 120) == 60);
    CHECK (flp::playlistRecordSize (25002005, 160) == 80);
    CHECK (flp::playlistRecordSize (25002005, 60) == 60);  // size doesn't match the version -> tries the others
    CHECK (flp::playlistRecordSize (25002005, 7) == 0);
}

TEST (flp_playlist_record_size_by_file_flags)
{
    // REA_rodada2 §4.2: 0xA9 bit 1 -> 80 B, bits 1 and 3 -> the native 88 B record (FL 25.2.99 / 26.x)
    CHECK (flp::playlistRecordSize (26001003, 88 * 4, 15) == 88); // 352 is also a multiple of 32
    CHECK (flp::playlistRecordSize (26001003, 88 * 10, 15) == 88); // ... and of 80
    CHECK (flp::playlistRecordSize (25002005, 160, 7) == 80);
    CHECK (flp::playlistRecordSize (21000000, 120, 1) == 60);
    CHECK (flp::playlistRecordSize (26001003, 63712) == 88); // no flags: the only size that fits besides 32
}

TEST (flp_native_88_byte_playlist_items_with_clip_gain_and_fades)
{
    // an FL 26.1 file: 0xA9 = 15, four 88 B items (352 B: the old 32 B guess read garbage from it)
    FlpBuilder b (96);
    b.ascii (0xC7, "26.1.3.5570");
    b.u8 (0xA9); b.u32 (15);
    b.u8 (0x9C); b.u32 (130000);
    b.u8 (0x40); b.u16 (0);
    b.u8 (0x15); b.u8 (4); // audio clip channel
    b.utf16 (0xC4, u"C:\\nowhere\\clip.wav");

    std::vector<std::uint8_t> items;
    auto add = [&] (std::uint32_t pos, std::uint32_t len, std::uint16_t track, float fadeIn, float fadeOut, float gain) {
        std::vector<std::uint8_t> r (88, 0);
        const std::uint16_t base = 20480, idx = 0, rv = (std::uint16_t) (499 - track);
        const float offs[] = { -1.0f, -1.0f };
        std::memcpy (&r[0], &pos, 4);
        std::memcpy (&r[4], &base, 2);
        std::memcpy (&r[6], &idx, 2);
        std::memcpy (&r[8], &len, 4);
        std::memcpy (&r[12], &rv, 2);
        std::memcpy (&r[0x18], offs, 8);
        std::memcpy (&r[0x24], &fadeIn, 4);
        std::memcpy (&r[0x2C], &fadeOut, 4);
        std::memcpy (&r[0x34], &gain, 4);
        const double stretch = 1.0;
        const std::int32_t minusOne = -1;
        std::memcpy (&r[0x40], &stretch, 8);
        std::memcpy (&r[0x4C], &minusOne, 4);
        items.insert (items.end(), r.begin(), r.end());
    };
    add (0, 384, 0, 0.0f, 0.0f, 1.0f);
    add (384, 384, 1, 923.0769f, 461.5385f, 1.65f); // 2 beats in, 1 beat out at 130 BPM
    add (768, 96, 2, 0.0f, 0.0f, 0.0f);
    add (1536, 192, 3, 0.0f, 5000.0f, 0.5f);        // fade longer than the clip
    b.data (0xE9, items);

    const auto p = flp::parseProject (b.bytes);
    CHECK (p.flagsA9 == 15);
    CHECK (p.arrangements.size() == 1 && p.arrangements[0].items.size() == 4);
    CHECK (p.warnings.empty());

    if (p.arrangements.size() == 1 && p.arrangements[0].items.size() == 4)
    {
        const auto& it = p.arrangements[0].items;
        CHECK (it[1].position == 384 && it[1].length == 384 && it[1].track == 1 && it[1].kind == flp::PlaylistItem::Kind::channelClip);
        CHECK (it[3].position == 1536 && it[3].track == 3);
        CHECK (std::abs (it[1].gain - 1.65f) < 1e-6f && std::abs (it[1].fadeInMs - 923.0769f) < 1e-3f);

        const auto song = lynx::importFlp (p);
        CHECK (song.playlist.size() == 4);

        if (song.playlist.size() == 4)
        {
            const auto beat = (std::int64_t) lynx::ppq;
            CHECK (song.playlist[0].gain == 1.0f && song.playlist[0].fadeIn == 0 && song.playlist[0].fadeOut == 0);
            CHECK (std::abs (song.playlist[1].gain - 1.65f) < 1e-6f);
            CHECK (std::llabs (song.playlist[1].fadeIn - 2 * beat) <= 1 && std::llabs (song.playlist[1].fadeOut - beat) <= 1);
            CHECK (song.playlist[2].gain == 0.0f);
            CHECK (song.playlist[3].fadeOut == song.playlist[3].length); // clamped
        }
    }
}

TEST (flp_channel_level_adjustments_and_stretched_clip_start)
{
    // FL 26.1 audio clip channel (gf92 follow-up): 0xE5 level adjustments and a time-stretched clip whose item
    // start/end are in the sample's milliseconds ("Liwn Passar voz" ch5: 788 ms into a clip stretched by 1.0569)
    FlpBuilder b (96);
    b.ascii (0xC7, "26.1.3.5570");
    b.u8 (0xA9); b.u32 (15);
    b.u8 (0x9C); b.u32 (130000);
    b.u8 (0x40); b.u16 (0);
    b.u8 (0x15); b.u8 (4); // audio clip channel
    b.utf16 (0xC4, u"C:\\nowhere\\clip.wav");
    {
        std::vector<std::uint8_t> levels (24, 0);
        const std::int32_t pan = 6400, vol = 10000;
        std::memcpy (&levels[0], &pan, 4);
        std::memcpy (&levels[4], &vol, 4);
        b.data (0xDB, levels);
        std::vector<std::uint8_t> adjust (20, 0);
        const std::int32_t panAdj = 3200, volAdj = 7286;
        std::memcpy (&adjust[0], &panAdj, 4);
        std::memcpy (&adjust[4], &volAdj, 4);
        b.data (0xE5, adjust);
        std::vector<std::uint8_t> params (0x98, 0);
        const float time = 64.0f * 192.0f; // stretched to 64 beats (float: 0xA9 bit 0)
        const double end = 1.0;
        std::memcpy (&params[0x60], &time, 4);
        std::memcpy (&params[0x88], &end, 8);
        b.data (0xD7, params);
    }
    {
        std::vector<std::uint8_t> r (88, 0);
        const std::uint32_t pos = 0, len = 6144; // 64 beats
        const std::uint16_t base = 20480, idx = 0, rv = 499;
        const float offs[] = { 788.4615f, 788.4615f + 31218.92f }; // sample ms: 64 beats at 130 BPM x 1.056892
        const float gain = 1.0f;
        std::memcpy (&r[0], &pos, 4);
        std::memcpy (&r[4], &base, 2);
        std::memcpy (&r[6], &idx, 2);
        std::memcpy (&r[8], &len, 4);
        std::memcpy (&r[12], &rv, 2);
        std::memcpy (&r[0x18], offs, 8);
        std::memcpy (&r[0x34], &gain, 4);
        b.data (0xE9, r);
    }

    const auto p = flp::parseProject (b.bytes);
    CHECK (p.channels.size() == 1);
    if (p.channels.size() != 1)
        return;
    CHECK (p.channels[0].hasLevelAdjust && p.channels[0].panAdj == 3200 && p.channels[0].volAdj == 7286);

    const auto song = lynx::importFlp (p);
    CHECK (song.channels.size() == 1 && song.playlist.size() == 1);
    if (song.channels.size() != 1 || song.playlist.size() != 1)
        return;
    // gain = f(volume)·f(volAdj), pan = pan + panAdj
    const auto expected = lynx::channelGain (10000.0f / 12800.0f) * lynx::channelGain (7286.0f / 12800.0f);
    CHECK (std::abs (lynx::channelGain (song.channels[0].volume) - expected) < 1e-5f);
    CHECK (std::abs (song.channels[0].pan - 0.5f) < 1e-6f);
    // 788.46 sample ms / 1.056892 = 746.0 ms = 1.6164 beats into the clip
    CHECK (song.channels[0].sampler.stretchBeats > 0.0f);
    CHECK (std::llabs (song.playlist[0].offset - (std::int64_t) std::llround (1.61638 * lynx::ppq)) <= 1);
    // REA_rodada110 §1: the sub-tick rest is kept (offset + fraction = the exact position, to the sample)
    const auto exact = 788.4615 / ((double) (788.4615f + 31218.92f - 788.4615f) / (64.0 * 60000.0 / 130.0)) / 1000.0 * 130.0 / 60.0 * lynx::ppq;
    CHECK (std::abs ((double) song.playlist[0].offset + (double) song.playlist[0].offsetFraction - exact) < 1e-3);
    CHECK (std::abs (song.playlist[0].offsetFraction) <= 0.5f);
}

TEST (flp_parses_project_model)
{
    FlpBuilder b (96);
    b.ascii (0xC7, "25.2.5.5319");
    b.u8 (0x9C); b.u32 (174500);
    b.utf16 (0xC2, u"Título");
    b.u8 (0x41); b.u16 (3);
    b.u8 (0x96); b.u32 (0x00A14547); // BGR in the file
    auto notes = noteRecord (96, 1, 48, 64, 100);
    auto n2 = noteRecord (0, 0, 0, 60, 128);
    notes.insert (notes.end(), n2.begin(), n2.end());
    b.data (0xE0, notes);
    b.u8 (0x40); b.u16 (0);
    b.u8 (0x15); b.u8 (0);
    b.utf16 (0xCB, u"Kick");
    {
        std::vector<std::uint8_t> levels (24, 0);
        const std::int32_t lv[] = { 3200, 12800, -150 };
        std::memcpy (levels.data(), lv, sizeof (lv));
        b.data (0xDB, levels);
    }
    b.u8 (0x68); b.u16 (5);
    b.u8 (0x40); b.u16 (1);
    b.u8 (0x15); b.u8 (2);
    b.utf16 (0xC9, u"FLEX");
    b.utf16 (0xCB, u"Lead");
    b.u8 (0x63); b.u16 (0);

    std::vector<std::uint8_t> item (80, 0);
    std::uint32_t pos = 384, len = 768;
    std::uint16_t base = 20480, idx = 20480 + 3, rv = 499 - 2;
    std::memcpy (&item[0], &pos, 4);
    std::memcpy (&item[4], &base, 2);
    std::memcpy (&item[6], &idx, 2);
    std::memcpy (&item[8], &len, 4);
    std::memcpy (&item[12], &rv, 2);
    b.data (0xE9, item);
    b.utf16 (0xCC, u"Drums");
    b.data (0xEC, std::vector<std::uint8_t> (12, 0));
    b.data (0xEC, std::vector<std::uint8_t> (12, 0));

    const auto p = flp::parseProject (b.bytes);
    CHECK (p.version == "25.2.5.5319");
    CHECK (std::abs (p.tempo - 174.5) < 1e-9);
    CHECK (p.title == "Título");
    CHECK (p.channels.size() == 2);
    CHECK (p.channels[0].name == "Kick" && p.channels[0].mixerTrack == 5);
    CHECK (p.channels[0].hasLevels && p.channels[0].pan == 3200 && p.channels[0].volume == 12800 && p.channels[0].pitchCents == -150);
    CHECK (p.channels[1].type == flp::ChannelType::instrument && p.channels[1].pluginName == "FLEX");
    CHECK (p.patterns.size() == 1 && p.patterns[0].index == 3);
    CHECK (p.patterns[0].colour && *p.patterns[0].colour == 0x4745A1);
    CHECK (p.patterns[0].notes.size() == 2 && p.patterns[0].notes[0].position == 0); // sorted
    CHECK (p.patterns[0].notes[1].key == 64 && p.patterns[0].notes[1].length == 48);
    CHECK (p.arrangements.size() == 1 && p.arrangements[0].items.size() == 1);
    const auto& it = p.arrangements[0].items[0];
    CHECK (it.kind == flp::PlaylistItem::Kind::pattern && it.index == 3 && it.track == 2 && it.position == 384);
    CHECK (p.mixer.size() == 2 && p.mixer[0].name == "Drums" && p.mixer[1].name.empty());
    CHECK (p.warnings.empty());
}

TEST (flp_fuzz_corrupted_files_do_not_crash)
{
    if (realFlpDir.empty())
        return;

    // some real .flp files, corrupted in various ways: the parser may reject them (flp::Error), but
    // must never hang, read past the buffer or produce a Song with invalid indices
    std::vector<std::vector<std::uint8_t>> sources;
    for (auto& entry : std::filesystem::recursive_directory_iterator (realFlpDir))
        if (entry.path().extension() == ".flp" && sources.size() < 6 && std::filesystem::file_size (entry.path()) < 3'000'000)
        {
            std::ifstream in (entry.path(), std::ios::binary);
            sources.emplace_back ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
        }

    std::uint32_t seed = 2024;
    auto rnd = [&] (std::uint32_t n) { seed = seed * 1664525u + 1013904223u; return n == 0 ? 0u : (seed >> 8) % n; };
    int parsed = 0, rejected = 0;

    for (int round = 0; round < 240 && ! sources.empty(); ++round)
    {
        auto bytes = sources[(std::size_t) rnd ((std::uint32_t) sources.size())];

        switch (round % 4)
        {
            case 0: bytes.resize (rnd ((std::uint32_t) bytes.size())); break;                          // truncated
            case 1: for (int k = 0; k < 20; ++k) bytes[(std::size_t) rnd ((std::uint32_t) bytes.size())] ^= (std::uint8_t) (1 + rnd (255)); break;
            case 2: // absurd event size
                for (int k = 0; k < 5; ++k)
                    bytes[(std::size_t) rnd ((std::uint32_t) bytes.size())] = 0xFF;
                break;
            default: // shuffled block
            {
                const auto a = rnd ((std::uint32_t) bytes.size()), b = rnd ((std::uint32_t) bytes.size());
                std::reverse (bytes.begin() + std::min (a, b), bytes.begin() + std::max (a, b));
                break;
            }
        }

        try
        {
            const auto project = flp::parseProject (bytes);
            const auto song = lynx::importFlp (project);
            ++parsed;

            for (const auto& p : song.patterns)
                for (const auto& n : p.notes)
                    CHECK (n.channel >= 0 && n.channel < (int) song.channels.size());
            for (const auto& c : song.playlist)
                CHECK (c.kind != lynx::ClipKind::pattern || (c.pattern >= 0 && c.pattern < (int) song.patterns.size()));
        }
        catch (const flp::Error&)
        {
            ++rejected;
        }
    }

    std::printf ("  fuzz: %d imported, %d rejected\n", parsed, rejected);
    CHECK (parsed + rejected == 240);
}

TEST (plugin_state_decoders_survive_garbage)
{
    // random blobs (varied sizes, including valid headers followed by garbage)
    std::uint32_t seed = 77;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    const char* plugins[] = { "Fruity Parametric EQ 2", "Fruity Limiter", "Fruity Reeverb 2", "Fruity Delay 2", "Fruity Delay 3",
                              "Fruity Compressor", "Fruity Balance", "Fruity PanOMatic", "Fruity Stereo Shaper", "Fruity Phase Inverter" };
    int decoded = 0;

    for (int round = 0; round < 3000; ++round)
    {
        std::vector<std::uint8_t> blob ((std::size_t) (rnd() % 600));
        for (auto& b : blob)
            b = (std::uint8_t) rnd();
        if (blob.size() >= 4 && round % 3 == 0) // versions the readers accept
        {
            const std::uint32_t versions[] = { 8, 10001, 10000, 12, 14, 6, 1 };
            const auto v = versions[rnd() % 7];
            std::memcpy (blob.data(), &v, 4);
        }

        if (auto fx = lynx::effectFromFl (plugins[rnd() % 10], blob))
        {
            ++decoded;
            for (auto v : fx->params)
                CHECK (std::isfinite (v) && v >= 0.0f && v <= 1.0f);
        }

        lynx::SynthSettings synth;
        if (lynx::synthFrom3xOsc (blob, synth))
            for (const auto& o : synth.osc)
                CHECK (std::isfinite (o.level) && o.level >= 0.0f && o.level <= 1.0f);

        float harmlessGain = 1.0f;
        if (lynx::synthFromHarmless (blob, 120.0, synth, harmlessGain))
            CHECK (std::isfinite (harmlessGain) && std::isfinite (synth.cutoff) && std::isfinite (synth.attack));
        if (lynx::synthFromFruitKick (blob, synth))
            CHECK (std::isfinite (synth.decay) && std::isfinite (synth.pitchEnv.amount));
        if (lynx::synthFromGms (blob, 120.0, synth, harmlessGain) || lynx::synthFromHarmor (blob, synth, harmlessGain) || lynx::synthFromOgun (blob, synth, harmlessGain)
            || lynx::synthFromSakura (blob, synth) || lynx::synthFromKepler (blob, synth, harmlessGain))
            CHECK (std::isfinite (synth.cutoff) && std::isfinite (synth.attack) && std::isfinite (harmlessGain));
        if (auto sound = lynx::drumSynthLiveSound (blob))
            CHECK (sound->size() < 256);
        if (auto wave = lynx::waveTravellerSample (blob))
            CHECK (! wave->first.empty() && wave->second >= 0 && wave->second <= 127);
        lynx::synthFromBooBass (blob, synth);
        lynx::gmsStateUsable (blob); // (the exact engines' parsers reject garbage without crashing)
        lynx::morphineStateUsable (blob);
        lynx::poizoneStateUsable (blob);
        if (lynx::synthFromPoiZone (blob, synth, harmlessGain) || lynx::synthFromBassDrum (blob, synth, harmlessGain) || lynx::synthFromToxic (blob, synth, harmlessGain)
            || lynx::synthFromMorphine (blob, synth, harmlessGain))
            CHECK (std::isfinite (synth.cutoff) && std::isfinite (harmlessGain));
        if (auto patch = lynx::drumpadPatchName (blob))
            CHECK (patch->size() <= 0x30 && std::isfinite (lynx::drumToneForName (*patch).decay));
        if (auto flex = lynx::flexPresetName (blob))
            CHECK ((! flex->first.empty() || ! flex->second.empty()) && flex->second.size() <= 1024);
        if (lynx::synthFromSytrus (blob, 120.0, synth, harmlessGain))
            CHECK (std::isfinite (synth.cutoff) && std::isfinite (synth.attack));
        if (lynx::synthFromDX10 (blob, synth))
            CHECK (std::isfinite (synth.decay) && std::isfinite (synth.release) && std::isfinite (synth.osc[1].level));
        if (blob.size() >= 4 && round % 5 == 0) // MiniSynth header followed by garbage
        {
            std::memcpy (blob.data(), "SSLF", 4);
            if (blob.size() > 220)
            {
                std::memcpy (blob.data() + 40, "DATA", 4);
                std::memcpy (blob.data() + 48, "0rev", 4);
            }
        }
        if (lynx::synthFromMiniSynth (blob, 120.0, synth))
            CHECK (std::isfinite (synth.cutoff) && std::isfinite (synth.decay) && std::isfinite (synth.attack));
        if (lynx::miniSynthStateUsable (blob)) // the exact engine's parser and a short render with silent tables
        {
            const auto params = lynx::miniSynthParams (blob);
            CHECK (params.size() == 43u && std::all_of (params.begin(), params.end(), [] (float v) { return v >= 0.0f && v <= 1.0f; }));
            const auto y = lynx::minisynth::renderReference (blob, { { 0, 60, 1.0, 1.0, 0.0, 0.0, 64 } }, 44100.0, 128, "");
            CHECK (y.size() == 256u && std::all_of (y.begin(), y.end(), [] (float v) { return std::isfinite (v); }));
        }

        lynx::ForeignPlugin foreign;
        lynx::parseFlWrapper (blob, foreign);
        std::vector<std::uint8_t> component, controller;
        lynx::splitFlVst3State (blob, component, controller);
    }

    CHECK (decoded == 3000); // every known name yields an effect (default when the blob is unusable)
}

TEST (inflate_zlib)
{
    // fixed Huffman with back-references
    const std::uint8_t fixed[] = { 0x78, 0xda, 0x4b, 0x4c, 0x4a, 0x4e, 0xc4, 0x86, 0x22, 0x00, 0x7c, 0x69, 0x09, 0x89 };
    const auto a = lynx::inflateZlib (fixed, sizeof (fixed));
    CHECK (a && std::string (a->begin(), a->end()) == "abcabcabcabcabcabcabcabcX");

    // stored (uncompressed) block
    const std::uint8_t stored[] = { 0x78, 0x01, 0x01, 0x03, 0x00, 0xfc, 0xff, 'L', 'Y', 'N', 0x00, 0x00, 0x00, 0x00 };
    const auto b = lynx::inflateZlib (stored, sizeof (stored));
    CHECK (b && std::string (b->begin(), b->end()) == "LYN");

    // truncated, bad header and size limit
    CHECK (! lynx::inflateZlib (fixed, 6));
    const std::uint8_t bad[] = { 0x12, 0x34, 0x00 };
    CHECK (! lynx::inflateZlib (bad, sizeof (bad)));
    CHECK (! lynx::inflateZlib (fixed, sizeof (fixed), false, 10));
}

TEST (fl_round15_effects_convert_known_values)
{
    auto blob = [] (std::vector<std::int32_t> words) {
        std::vector<std::uint8_t> b (words.size() * 4);
        std::memcpy (b.data(), words.data(), b.size());
        return b;
    };
    // Frequency Shifter "+800 Hz": frequency 51542 in the 20 kHz range, 100% wet — FL's own engine since round 154
    // (its controls as (raw − min) / range); unreadable states keep the generic shifter
    const auto fs = lynx::effectFromFl ("Frequency Shifter", blob ({ 4, 10000, 0, 51542, 0, 10000, 10000, 5000, 10000, 0, 5000, 0, 0 }));
    CHECK (fs && fs->kind == lynx::EffectKind::flFrequencyShifter);
    if (fs)
    {
        CHECK (std::abs (fs->params[0] - 1.0f) < 1e-6f && fs->params[1] == 0.0f);
        CHECK (std::abs (fs->params[2] - (51542.0f + 70000.0f) / 140000.0f) < 1e-6f);
    }
    const auto fsOld = lynx::effectFromFl ("Frequency Shifter", blob ({ 4, 10000, 0 }));
    CHECK (fsOld && fsOld->kind == lynx::EffectKind::freqShift);
    // Emphasis: input 0.0625 (= +1.5 dB) and ceiling 0.9308 (≈ −1.6 dB) -> limiter
    const auto em = lynx::effectFromFl ("Emphasis", blob ({ 8, 0, 0, 2, 1, 0, 0, 625, 10000, 10000, 9308, 0, 2, 7, 0 }));
    CHECK (em && em->kind == lynx::EffectKind::limiter && em->enabled);
    if (em)
    {
        CHECK (std::abs (em->params[1] * 24.0f - 1.5f) < 0.05f);
        CHECK (std::abs (em->params[0] * 12.0f - 12.0f + 1.6f) < 0.1f);
    }
    // Pitch Shifter +12 st, mix 100%
    const auto ps = lynx::effectFromFl ("Pitch Shifter", blob ({ 7, 1200, 0, 0, 2400, 8500, 200, 0, 0, 768, 0, 0, 65536, 0, 65536, 0, 0, 0, 0 }));
    CHECK (ps && ps->kind == lynx::EffectKind::flPitchShifter && ps->params[0] == 1.0f && ps->params[3] == 1.0f); // REA_rodada61
}

TEST (frequency_shifter_moves_spectrum_by_hz)
{
    auto fx = lynx::makeEffect (lynx::EffectKind::freqShift);
    fx->prepare (48000.0);
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::freqShift);
    // +200 Hz: x·|x|·2000 = 200 -> x = sqrt(0.1)
    slot.params[0] = (float) (0.5 + std::sqrt (0.1) / 2.0);
    std::vector<float> l (48000), r (48000);
    for (int i = 0; i < 48000; ++i)
        l[(std::size_t) i] = r[(std::size_t) i] = (float) std::sin (2.0 * 3.141592653589793 * 1000.0 * i / 48000.0);
    for (int done = 0; done < 48000; done += 480)
        fx->process (l.data() + done, r.data() + done, 480, slot.params, 120.0);
    auto energy = [&] (double f) {
        const auto w = 2.0 * 3.141592653589793 * f / 48000.0, k = 2.0 * std::cos (w);
        double s1 = 0.0, s2 = 0.0;
        for (int i = 9600; i < 48000; ++i)
        {
            const auto s0 = l[(std::size_t) i] + k * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        return s1 * s1 + s2 * s2 - k * s1 * s2;
    };
    // the energy moves to 1200 Hz; little remains at 1000 and almost nothing at the image (800)
    CHECK (energy (1200.0) > 50.0 * energy (1000.0));
    CHECK (energy (1200.0) > 50.0 * energy (800.0));
}

TEST (pitch_shift_moves_frequency_keeps_length)
{
    auto fx = lynx::makeEffect (lynx::EffectKind::pitchShift);
    fx->prepare (48000.0);
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::pitchShift);
    auto crossingsAfter = [&] (float semitoneParam) {
        fx->reset();
        slot.params[0] = semitoneParam;
        std::vector<float> l (48000), r (48000);
        for (int i = 0; i < 48000; ++i)
            l[(std::size_t) i] = r[(std::size_t) i] = (float) std::sin (2.0 * 3.141592653589793 * 220.0 * i / 48000.0);
        for (int done = 0; done < 48000; done += 512)
            fx->process (l.data() + done, r.data() + done, std::min (512, 48000 - done), slot.params, 120.0);
        int crossings = 0;
        for (int i = 9600; i < 48000; ++i)
            crossings += (l[(std::size_t) i - 1] < 0.0f) != (l[(std::size_t) i] < 0.0f);
        for (auto v : l)
            CHECK (std::isfinite (v) && std::abs (v) < 1.6f);
        return crossings / 0.8 / 2.0; // cycles per second
    };
    CHECK (std::abs (crossingsAfter (0.5f) - 220.0) < 8.0);           // 0 st
    CHECK (std::abs (crossingsAfter (0.5f + 12.0f / 48.0f) - 440.0) < 25.0); // +12 st
    CHECK (std::abs (crossingsAfter (0.5f - 12.0f / 48.0f) - 110.0) < 12.0); // −12 st
}

// A Gross Beat state in FL's layout (REA_rodada53 §1), built here: version 8, parameters, options, 72 slots with a name,
// sync bytes and a map envelope (kind 3, version 3, points: Δx in beats, level, tension, curve type)
namespace
{
struct GbPoint
{
    double dx, y;
    std::uint8_t type;
};
std::vector<std::uint8_t> grossBeatState (const std::array<int, 9>& params, const std::array<std::uint8_t, 9>& options,
                                          const std::map<int, std::vector<GbPoint>>& slots)
{
    std::vector<std::uint8_t> b;
    auto put = [&b] (const void* p, std::size_t n) { b.insert (b.end(), (const std::uint8_t*) p, (const std::uint8_t*) p + n); };
    auto i32 = [&put] (std::int32_t v) { put (&v, 4); };
    i32 (0x00080003);
    for (auto v : params)
        i32 (v);
    put (options.data(), options.size());
    for (int k = 0; k < 72; ++k)
    {
        b.push_back (0); // empty name
        i32 (0), i32 (5), i32 (0); // trigger sync off, position sync 4 beats, no hold
        b.push_back (0), b.push_back (0), b.push_back (0);
        const auto it = slots.find (k);
        const std::vector<GbPoint> flat { { 0.0, 1.0, 0 }, { 4.0, 1.0, 0 } };
        const auto& pts = it != slots.end() ? it->second : flat;
        i32 (3), i32 (3), i32 ((std::int32_t) pts.size());
        for (const auto& p : pts)
        {
            put (&p.dx, 8), put (&p.y, 8);
            const float tension = 0.0f;
            put (&tension, 4);
            b.push_back (p.type), b.push_back (0), b.push_back (0), b.push_back (2);
        }
        i32 (0), i32 (-1), i32 (-1), i32 (-1), i32 (-1);
    }
    return b;
}

std::vector<float> renderGrossBeat (const lynx::EffectSlot& slot, const std::vector<float>& in, double rate, double bpm)
{
    auto fx = lynx::makeEffect (slot);
    fx->prepare (rate);
    auto l = in, r = in;
    const auto tps = bpm / 60.0 * lynx::ppq / rate;
    for (std::size_t done = 0; done < l.size(); done += 512)
    {
        const auto n = (int) std::min<std::size_t> (512, l.size() - done);
        fx->setSongPosition ((double) done * tps);
        fx->process (l.data() + done, r.data() + done, n, slot.params, bpm);
    }
    return l;
}
} // namespace

TEST (gross_beat_state_container_and_parameters)
{
    const auto state = grossBeatState ({ 3, 40, 128, 256, 1765, 5000, -77, 0, 0x10000 }, { 0, 0, 0, 0, 0, 0, 0, 1, 1 }, {});
    const auto parsed = lynx::grossbeat::parseState (state.data(), state.size());
    CHECK (parsed.has_value());
    CHECK (parsed && parsed->version == 8 && parsed->params[6] == -77 && parsed->slots[0].flat && parsed->slots[71].flat);
    const auto slot = lynx::grossbeat::slotFromFl (state, 960);
    CHECK (slot && slot->kind == lynx::EffectKind::grossBeat);
    std::vector<std::uint8_t> fl;
    int ppq = 0;
    CHECK (slot && lynx::grossbeat::unpack (slot->state, fl, ppq) && fl == state && ppq == 960);
    // every raw FL value survives the 0..1 parameter
    const int lo[8] { 0, 0, 0, 0, 0, 0, -256, 0 }, hi[8] { 35, 35, 256, 256, 10000, 10000, 256, 1 };
    bool roundTrip = true;
    for (int i = 0; i < 8; ++i)
        for (int raw = lo[i]; raw <= hi[i]; ++raw)
            roundTrip = roundTrip && lynx::grossbeat::rawFromParam (i, lynx::grossbeat::paramFromRaw (i, raw)) == raw;
    CHECK (roundTrip);
    CHECK (slot && slot->params[1] == lynx::grossbeat::paramFromRaw (1, 35)); // the volume slot 40 is clamped to 35
    CHECK (! lynx::grossbeat::parseState (state.data(), 100).has_value());
}

TEST (gross_beat_volume_slot_gates_the_bar)
{
    // volume slot 1: open for two beats, closed for two (hold segments); no smoothing; dry path 15 samples late
    const std::vector<GbPoint> gate { { 0.0, 1.0, 0 }, { 2.0, 1.0, 2 }, { 0.0, 0.0, 0 }, { 2.0, 0.0, 2 } };
    const auto state = grossBeatState ({ 0, 1, 256, 256, 0, 0, 0, 0, 0x10000 }, { 0, 0, 0, 0, 0, 0, 0, 1, 1 }, { { 37, gate } });
    const auto slot = lynx::grossbeat::slotFromFl (state);
    CHECK (slot.has_value());
    if (! slot)
        return;
    const auto out = renderGrossBeat (*slot, std::vector<float> (200000, 1.0f), 48000.0, 120.0); // a beat = 24000 samples
    CHECK (out[14] == 0.0f && out[15] == 1.0f);
    CHECK (out[30000] == 1.0f && out[47990] == 1.0f);
    CHECK (out[48010] == 0.0f && out[95990] == 0.0f);
    CHECK (out[96010] == 1.0f && out[150000] == 0.0f);
}

TEST (gross_beat_time_slot_reads_the_past_bar)
{
    // time slot 1 at level 0.5: the buffer is read one bar back (two bars = level 0), plus the 15-sample latency
    const std::vector<GbPoint> half { { 0.0, 0.5, 0 }, { 4.0, 0.5, 2 } };
    const auto state = grossBeatState ({ 1, 0, 256, 256, 0, 0, 0, 0, 0x10000 }, { 0, 0, 0, 0, 0, 0, 0, 1, 1 }, { { 1, half } });
    const auto slot = lynx::grossbeat::slotFromFl (state);
    CHECK (slot.has_value());
    if (! slot)
        return;
    std::vector<float> in (150000, 0.0f);
    in[1000] = 1.0f;
    const auto out = renderGrossBeat (*slot, in, 48000.0, 120.0); // a bar = 96000 samples
    std::size_t peak = 0;
    for (std::size_t i = 0; i < out.size(); ++i)
        if (std::abs (out[i]) > std::abs (out[peak]))
            peak = i;
    CHECK (peak == 1000 + 96000 + 15);
    CHECK (std::abs (out[peak] - 1.0f) < 1e-6f);
    float elsewhere = 0.0f;
    for (std::size_t i = 0; i < out.size(); ++i)
        if (i + 20 < peak || i > peak + 20)
            elsewhere = std::max (elsewhere, std::abs (out[i]));
    CHECK (elsewhere == 0.0f);
}

TEST (volume_gate_follows_bar_position)
{
    auto fx = lynx::makeEffect (lynx::EffectKind::volumeGate);
    fx->prepare (48000.0);
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::volumeGate);
    for (int i = 0; i < 32; ++i)
        slot.params[(std::size_t) i] = i < 16 ? 1.0f : 0.0f; // first two beats open, two closed
    slot.params[33] = slot.params[34] = 0.0f;                // no smoothing

    // 120 BPM: one beat = 0.5 s = 24000 samples; the 4-beat bar starts at tick 0
    std::vector<float> l (96000, 1.0f), r (96000, 1.0f);
    fx->setSongPosition (0.0);
    for (int done = 0; done < 96000; done += 512)
        fx->process (l.data() + done, r.data() + done, std::min (512, 96000 - done), slot.params, 120.0);
    CHECK (l[10000] > 0.99f && l[40000] > 0.99f);  // beats 1 and 2
    CHECK (l[60000] < 0.01f && l[90000] < 0.01f);  // beats 3 and 4

    // starting mid-bar (beat 3): already closed
    std::vector<float> l2 (4800, 1.0f), r2 (4800, 1.0f);
    fx->reset();
    fx->setSongPosition (2.0 * lynx::ppq);
    fx->process (l2.data(), r2.data(), 4800, slot.params, 120.0);
    CHECK (l2[4000] < 0.01f);

    // mix 0: transparent
    slot.params[32] = 0.0f;
    std::vector<float> l3 (4800, 1.0f), r3 (4800, 1.0f);
    fx->setSongPosition (2.0 * lynx::ppq);
    fx->process (l3.data(), r3.data(), 4800, slot.params, 120.0);
    CHECK (l3[4000] > 0.99f);
}

TEST (patcher_branch_heads_follow_rea)
{
    auto words = [] (std::vector<std::int32_t> v) {
        std::vector<std::uint8_t> b (v.size() * 4);
        std::memcpy (b.data(), v.data(), b.size());
        return b;
    };
    // Frequency Splitter, 2 bands, destinations 1/2/3, slope combo 3 (24 dB/oct), high send −6 dB
    std::vector<std::int32_t> fs (18, 0);
    fs[0] = 4;
    fs[1] = 32768;
    fs[3] = 3;
    fs[6] = 0;
    fs[11] = -600;
    fs[15] = 1;
    fs[16] = 2;
    fs[17] = 3;
    const auto split = words (fs);
    const auto low = lynx::patcherBranchHead ("Frequency Splitter", split, 1);
    CHECK (low.valid && low.slot && low.note.empty());
    if (low.slot)
    {
        // the exact engine (REA_rodada62): the low band's send, 24 dB/oct
        CHECK (low.slot->kind == lynx::EffectKind::frequencySplitter && low.slot->params[26] == 1.0f && low.slot->params[27] == 0.0f);
        CHECK (low.slot->params[2] == 3.0f / 7.0f);
    }
    const auto mid = lynx::patcherBranchHead ("Frequency Splitter", split, 2);
    CHECK (mid.valid && mid.slot && mid.slot->params[27] == 1.0f && mid.slot->params[5] == 0.0f); // 2 bands: the engine's mid is silent
    const auto high = lynx::patcherBranchHead ("Frequency Splitter", split, 3);
    CHECK (high.valid && high.slot && high.slot->params[28] == 1.0f && high.slot->params[10] == (float) (1200.0 / 3600.0));
    CHECK (! lynx::patcherBranchHead ("Frequency Splitter", split, 7).valid);

    // Stereo Shaper: the difference destination (4) is the exact engine with its difference output (REA_rodada54)
    const auto shaper = words ({ 3, 0, 12800, 12800, 0, 0, 0, 4, 0 });
    const auto diff = lynx::patcherBranchHead ("Fruity Stereo Shaper", shaper, 4);
    CHECK (diff.valid && diff.slot && diff.slot->kind == lynx::EffectKind::stereoShaper && diff.slot->params[7] == 1.0f);
    CHECK (! lynx::patcherBranchHead ("Fruity Stereo Shaper", shaper, 2).valid);
}

TEST (parallel_container_sums_branches)
{
    // two branches: dry with gain 0.5 and a utility that flips phase (matrix −1) with gain 0.5 -> silence
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::parallel);
    lynx::EffectBranch dry;
    dry.gain = 0.5f;
    lynx::EffectBranch inverted;
    inverted.gain = 0.5f;
    auto flip = lynx::makeEffectSlot (lynx::EffectKind::utility);
    flip.params[2] = flip.params[5] = 0.25f; // (−1 + 2)/4
    inverted.chain.push_back (flip);
    slot.branches = { dry, inverted };
    auto fx = lynx::makeParallelEffect (slot);
    fx->prepare (48000.0);
    std::vector<float> l (9000, 0.8f), r (9000, -0.4f);
    fx->process (l.data(), r.data(), 9000, slot.params, 120.0);
    CHECK (std::abs (l[100]) < 1e-5f && std::abs (r[8999]) < 1e-5f);

    // only the dry branch with gain 2: doubles
    slot.branches = { lynx::EffectBranch { {}, 2.0f } };
    auto doubler = lynx::makeParallelEffect (slot);
    doubler->prepare (48000.0);
    std::fill (l.begin(), l.end(), 0.25f);
    doubler->process (l.data(), r.data(), 9000, slot.params, 120.0);
    CHECK (std::abs (l[5000] - 0.5f) < 1e-6f);
}

TEST (fl_controller_constant_outputs)
{
    std::vector<std::int32_t> xy { 2, 32768, 0, 65536, 0, 16384, 0, 65536, 0, 1024, 256 };
    std::vector<std::uint8_t> blob (xy.size() * 4 + 8, 0);
    std::memcpy (blob.data(), xy.data(), xy.size() * 4);
    CHECK (std::abs (lynx::controllerConstantOutput ("Fruity X-Y Controller", blob, 0).value_or (-1.0f) - 0.5f) < 1e-4f);
    CHECK (std::abs (lynx::controllerConstantOutput ("Fruity X-Y Controller", blob, 1).value_or (-1.0f) - 0.25f) < 1e-4f);

    // Control Surface: version + group { 0x836: value 0.75, -, output 2 }
    std::vector<std::uint8_t> cs (4, 0);
    cs[0] = 1;
    auto chunk = [] (std::vector<std::uint8_t>& out, std::uint32_t id, const std::vector<std::uint8_t>& body) {
        const auto size = (std::uint64_t) body.size();
        out.insert (out.end(), reinterpret_cast<const std::uint8_t*> (&id), reinterpret_cast<const std::uint8_t*> (&id) + 4);
        out.insert (out.end(), reinterpret_cast<const std::uint8_t*> (&size), reinterpret_cast<const std::uint8_t*> (&size) + 8);
        out.insert (out.end(), body.begin(), body.end());
    };
    std::vector<std::uint8_t> value (12, 0);
    const float v = 0.75f;
    const std::int32_t out = 2;
    std::memcpy (value.data(), &v, 4);
    std::memcpy (value.data() + 8, &out, 4);
    std::vector<std::uint8_t> group;
    chunk (group, 0x836, value);
    chunk (cs, 0, group);
    CHECK (std::abs (lynx::controllerConstantOutput ("Control Surface", cs, 2).value_or (-1.0f) - 0.75f) < 1e-6f);
    CHECK (! lynx::controllerConstantOutput ("Control Surface", cs, 0).has_value());
}

TEST (flp_midi_out_notes_reach_wrapper_on_same_port)
{
    auto record = [] (std::vector<std::uint8_t>& out, std::uint32_t id, std::vector<std::uint8_t> body) {
        const auto size = (std::uint64_t) body.size();
        out.insert (out.end(), reinterpret_cast<const std::uint8_t*> (&id), reinterpret_cast<const std::uint8_t*> (&id) + 4);
        out.insert (out.end(), reinterpret_cast<const std::uint8_t*> (&size), reinterpret_cast<const std::uint8_t*> (&size) + 8);
        out.insert (out.end(), body.begin(), body.end());
    };
    auto i32s = [] (std::vector<std::int32_t> v) {
        std::vector<std::uint8_t> b (v.size() * 4);
        std::memcpy (b.data(), v.data(), b.size());
        return b;
    };

    flp::Project p;
    flp::Channel midiOut;
    midiOut.index = 0;
    midiOut.type = flp::ChannelType::native;
    midiOut.pluginName = "MIDI Out";
    midiOut.pluginState = i32s ({ 7, 0, -1, -1, -1, 0, 3, 0 }); // channel 1, port 3 (@24)
    flp::Channel wrapper;
    wrapper.index = 1;
    wrapper.type = flp::ChannelType::native;
    wrapper.pluginName = "Fruity Wrapper";
    wrapper.pluginState = i32s ({ 9 });
    record (wrapper.pluginState, 0x32, i32s ({ 7 }));      // VST3
    record (wrapper.pluginState, 0x01, i32s ({ 3, -1, 0, 0, 0 })); // input port 3
    p.channels = { midiOut, wrapper };
    flp::Pattern pattern;
    pattern.index = 1;
    flp::Note note;
    note.channel = 0;
    note.key = 64;
    note.length = 96;
    pattern.notes.push_back (note);
    p.patterns.push_back (pattern);

    const auto song = lynx::importFlp (p);
    // round 133: the MIDI Out channel stays a live route (its notes are sent at play time, with its MIDI channel)
    CHECK (song.channels.size() == 2 && song.channels[0].instrument == lynx::InstrumentKind::midiOut && ! song.channels[0].muted);
    CHECK (song.channels[1].plugin.foreign.midiInPort == 3 && song.channels[1].plugin.midiInPort == 3);
    CHECK (lynx::midictl::parseMidiOut (song.channels[0].pluginState).port == 3);
    int toWrapper = 0;
    for (const auto& pat : song.patterns)
        for (const auto& n : pat.notes)
            toWrapper += n.channel == 1 && n.key == 64;
    CHECK (toWrapper == 0);
}

TEST (flp_effect_automation_follows_slots_and_params)
{
    // track 1: slot 0 with an effect that has no equivalent (dropped), slot 2 with Parametric EQ 2
    flp::Project p;
    flp::MixerInsert insert;
    insert.index = 1;
    insert.slots.push_back ({ 0, "Some Unknown FX", {} });
    insert.slots.push_back ({ 2, "Fruity Parametric EQ 2", {} });
    p.mixer.push_back (insert);

    auto clip = [&] (int index, std::uint32_t target) {
        flp::Channel c;
        c.index = index;
        c.type = flp::ChannelType::automationClip;
        c.automation = { { 0.0, 0.0 }, { 96.0, 1.0 } };
        p.channels.push_back (c);
        p.automationLinks.push_back ({ index, { target } });
    };
    const auto mixer = [] (int track, int slot, int param) { return 0x70000000u + ((std::uint32_t) track << 22) + ((std::uint32_t) slot << 16) + (std::uint32_t) param; };
    clip (0, mixer (1, 2, 0x8000 + 7)); // EQ band 1 frequency
    clip (1, mixer (1, 2, 0x1F01));     // EQ slot mix
    clip (2, mixer (1, 0, 0x8000));     // parameter of the dropped effect
    clip (3, mixer (1, 2, 0x8000 + 21)); // band type: no equivalent

    const auto song = lynx::importFlp (p);
    CHECK (song.automations.size() == 4);
    CHECK (song.mixer[1].effects.size() == 1 && song.mixer[1].effects[0].kind == lynx::EffectKind::parametricEq);

    const auto& freq = song.automations[0];
    CHECK (freq.target.kind == lynx::AutomationTargetKind::effectParam && freq.target.index == 1 && freq.target.slot == 0);
    CHECK (freq.target.param == lynx::peq::param (0, 1));
    // FL's 0..1 (20 Hz..20 kHz) becomes our frequency scale
    CHECK (std::abs (freq.points.front().value - lynx::peq::hzValue (20.0)) < 1e-4f);
    CHECK (std::abs (freq.points.back().value - lynx::peq::hzValue (20000.0)) < 1e-4f);

    CHECK (song.automations[1].target.kind == lynx::AutomationTargetKind::effectMix && song.automations[1].target.slot == 0);
    CHECK (song.automations[2].target.kind == lynx::AutomationTargetKind::none);
    CHECK (song.automations[3].target.kind == lynx::AutomationTargetKind::none);

    // the automated mix is read/written through the target
    auto copy = song;
    CHECK (copy.setTargetValue (song.automations[1].target, 0.25f) && std::abs (copy.mixer[1].effects[0].mix - 0.25f) < 1e-6f);
    CHECK (std::abs (copy.targetValue (song.automations[1].target) - 0.25f) < 1e-6f);
}

TEST (flp_real_files)
{
    if (realFlpDir.empty())
    {
        std::printf ("  (skipped: pass a folder with .flp files as argument)\n");
        return;
    }

    int files = 0, withNotes = 0, withPlaylist = 0, automations = 0, automationsWithTarget = 0, sends = 0;
    int threeOsc = 0, threeOscDecoded = 0, wrapped = 0, wrappedParsed = 0, wrappedVst2 = 0, wrappedVst2State = 0, wrappedVst2Old = 0, markers = 0, signatures = 0, trackEqs = 0;
    int harmless = 0, harmlessDecoded = 0, harmlessParsed = 0, harmlessKind = 0, sytrus = 0, sytrusDecoded = 0, sytrusEngine = 0, sytrusRendered = 0,
        sytrusSounding = 0;
    int harmor = 0, harmorUsable = 0, harmorKind = 0, harmorSounding = 0;
    int gms = 0, gmsUsable = 0, gmsKind = 0, gmsSounding = 0, morphine = 0, morphineUsable = 0, morphineKind = 0, morphineSounding = 0, demoGoldens = 0;
    int ogun = 0, ogunUsable = 0, ogunKind = 0, ogunSounding = 0;
    int autogun = 0, autogunUsable = 0, autogunKind = 0, autogunWithSg = 0, autogunSounding = 0; // REA_rodada57
    int poizone = 0, poizoneUsable = 0, poizoneKind = 0, poizoneSounding = 0, poizoneGoldens = 0;
    int miniSynth = 0, miniSynthUsable = 0, miniSynthKind = 0, miniSynthSounding = 0, miniSynthGoldens = 0;
    int dx10 = 0, dx10Usable = 0, dx10Kind = 0, dx10Sounding = 0;
    int kepler = 0, keplerUsable = 0, keplerKind = 0, keplerSounding = 0;
    int toxic = 0, toxicUsable = 0, toxicKind = 0, toxicSounding = 0, toxicGoldens = 0; // REA_rodada45
    int threeOscKind = 0, threeOscSounding = 0;
    std::map<std::string, std::pair<int, int>> generators; // GMS/Ogun/Autogun/Sakura/DrumSynth: channels, decoded
    int ogunTrumpet = 0; // Ogun channels whose spectrum matches the REA "Trumpet" reference
    int peqSlots = 0, peqShaped = 0, reverbs = 0, reverbsDryless = 0, reverbsLowCut = 0;
    int dwChannels = 0, dwZones = 0, fpcKits = 0, padChannels = 0, padNotes = 0, dwExact = 0;
    int harmlessCompWanted = 0, harmlessCompLoaded = 0; // Harmless rack compressors / presets read from the installation
    int maximusSlots = 0, maximusExact = 0, sgzSlots = 0, sgzExact = 0, vocodexSlots = 0, vocodexWithData = 0; // REA_rodada40
    int shaperSlots = 0, shaperExact = 0, clipperSlots = 0, clipperExact = 0; // REA_rodada51
    int flPeqSlots = 0, flPeqExact = 0;                                        // REA_rodada50
    int basicSlots = 0, basicExact = 0; // REA_rodada58: PanOMatic, Balance, Blood Overdrive
    int equoSlots = 0, equoExact = 0, pitchShifterSlots = 0, pitchShifterExact = 0; // REA_rodada61
    int filterSlots = 0, filterExact = 0; // REA_rodada63: Fast LP, Free Filter, Fruity Filter
    int distructorSlots = 0, distructorExact = 0, distructorImpulses = 0;             // REA_rodada71
    int delay2Slots = 0, delay2Exact = 0, delay3Slots = 0, delay3Exact = 0; // REA_rodada52
    int chorusSlots = 0, chorusExact = 0; // REA_rodada59: the exact choruses
    int fastDistSlots = 0, fastDistExact = 0, flangusSlots = 0, flangusExact = 0, flangerSlots = 0, flangerExact = 0; // REA_rodada55
    int phaserSlots = 0, phaserExact = 0; // REA_rodada105_phaser
    // import with the installation's samples (realFlpDir is ...\Data): for DirectWave/FPC
    lynx::FlpImportOptions withFiles;
    const auto flRoot = std::filesystem::path (realFlpDir).parent_path().string();
    withFiles.roots = { flRoot, realFlpDir, "" };
    withFiles.fileExists = [] (const std::string& path) {
        auto local = path;
        std::replace (local.begin(), local.end(), '\\', '/');
        return std::filesystem::exists (local);
    };
    withFiles.readFile = [] (const std::string& path) {
        auto local = path;
        std::replace (local.begin(), local.end(), '\\', '/');
        std::ifstream in (local, std::ios::binary);
        return std::vector<std::uint8_t> ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    };
    int embeddedSaved = 0;
    withFiles.saveEmbedded = [&embeddedSaved] (const std::vector<std::uint8_t>& wav, const std::string&) {
        ++embeddedSaved;
        return wav.size() > 44 ? std::string ("embutido.wav") : std::string();
    };
    int slicedChannels = 0, flKeysChannels = 0;

    for (auto& entry : std::filesystem::recursive_directory_iterator (realFlpDir))
    {
        if (entry.path().extension() != ".flp")
            continue;

        std::ifstream in (entry.path(), std::ios::binary);
        std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
        ++files;

        try
        {
            const auto p = flp::parseProject (bytes);
            CHECK (! p.version.empty());
            CHECK (p.tempo > 10.0 && p.tempo < 1000.0);

            const auto song = lynx::importFlp (p);
            CHECK (! song.patterns.empty());

            {
                const auto full = lynx::importFlp (p, withFiles);
                bool hasFpc = false;
                for (const auto& c : p.channels)
                    hasFpc = hasFpc || c.pluginName == "FPC";
                // GMS and Morphine (REA_rodada39): every state plays on the exact engine (GMS with its waves from the
                // installation) and sounds; two demo channels match golden samples of our round-39 models
                for (const auto& c : p.channels)
                {
                    if (c.pluginName == "GMS")
                        ++gms, gmsUsable += lynx::gmsStateUsable (c.pluginState) ? 1 : 0;
                    if (c.pluginName == "Morphine")
                        ++morphine, morphineUsable += lynx::morphineStateUsable (c.pluginState) ? 1 : 0;
                }
                // Kepler: every state in the demos plays on the exact engine (REA_rodada46) when the engine DLL is installed
                for (const auto& c : p.channels)
                    if (c.pluginName == "Kepler")
                    {
                        ++kepler;
                        keplerUsable += lynx::keplerStateUsable (c.pluginState) ? 1 : 0;
                    }
                for (const auto& c : full.channels)
                    if (c.instrument == lynx::InstrumentKind::kepler)
                    {
                        ++keplerKind;
                        const auto y = lynx::kepler::renderReference (c.pluginState, c.samplePath, { { 0, 60, 1.0, 0.0, 0.0, 6000 }, { 2000, 67, 0.6, 0.0, 0.0, 8000 } },
                                                                      44100.0, 11025);
                        double e = 0.0;
                        bool finite = ! y.empty();
                        for (auto v : y)
                            finite = finite && std::isfinite (v), e += (double) v * v;
                        CHECK (finite);
                        keplerSounding += e > 1e-6 ? 1 : 0;
                    }
                const auto file = entry.path().filename().string();
                for (const auto& c : full.channels)
                {
                    const auto isGms = c.instrument == lynx::InstrumentKind::gms, isMorphine = c.instrument == lynx::InstrumentKind::morphine;
                    if (! isGms && ! isMorphine)
                        continue;
                    (isGms ? gmsKind : morphineKind)++;
                    auto iv = [] (double vel) { return (std::pow (11.0, vel / 100.0) - 1.0) / 10.0; };
                    const auto golden = (isGms && c.name == "GMS" && file.find ("Song For You") != std::string::npos)
                                        || (isMorphine && c.name == "Morphine" && file.find ("SHINE") != std::string::npos);
                    std::vector<float> y;
                    if (isGms)
                        y = lynx::gms::renderReference (c.pluginState,
                                                        { { 0, 60, (float) iv (100), 1.0, 0.0, 0.0, 30000 }, { 11025, 64, (float) iv (80), 1.0, 0.0, 0.0, 40000 },
                                                          { 22050, 67, (float) iv (110), 1.0, 0.0, 0.0, 52000 }, { 60000, 48, (float) iv (127), 1.0, 0.0, 0.0, 70000 } },
                                                        44100.0, golden ? 88200 : 22050, c.samplePath);
                    else
                        y = lynx::morphine::renderReference (c.pluginState, { { 0, 60, 1.0, 1.0, 0.0, 0.0, 30000 }, { 11025, 64, 0.5, 1.0, 0.0, 0.0, 50000 }, { 22050, 67, 2.0, 1.0, 0.0, 0.0, 70000 } },
                                                             44100.0, golden ? 100000 : 22050, c.samplePath);
                    double e = 0.0;
                    bool finite = ! y.empty();
                    for (auto v : y)
                        finite = finite && std::isfinite (v), e += (double) v * v;
                    CHECK (finite);
                    (isGms ? gmsSounding : morphineSounding) += e > 1e-6 ? 1 : 0;
                    if (golden && ! y.empty())
                    {
                        // our model, same events (GMS: tempo 140; Morphine: tempo 120), state as saved, 44.1 kHz
                        const auto rms = std::sqrt (e / (double) y.size());
                        CHECK (std::abs (rms - (isGms ? 0.213602668574 : 0.0673606448152)) < 1e-5);
                        static const struct { int i; float l, r; } gmsAt[] = { { 2000, 0.0890912563f, 0.00406081323f }, { 15000, 0.395968914f, 0.213082865f }, { 30000, -0.33275044f, -0.530703545f }, { 50000, 0.277897179f, 0.482887864f }, { 65000, 0.266831428f, 0.366135627f }, { 80000, 0.00021339106f, 0.000807374308f } },
                                                                 morAt[] = { { 2000, 0.0620051622f, 0.0791224241f }, { 15000, 0.000745030935f, 0.0800722092f }, { 30000, 0.102371402f, 0.0262862686f }, { 50000, 0.000215340013f, 0.0249186736f }, { 65000, -0.0414967388f, -0.0449344926f }, { 90000, -0.00397050008f, -0.00165477558f } };
                        for (const auto& g : isGms ? std::vector (std::begin (gmsAt), std::end (gmsAt)) : std::vector (std::begin (morAt), std::end (morAt)))
                            CHECK (std::abs (y[2 * (std::size_t) g.i] - g.l) < 1e-5f && std::abs (y[2 * (std::size_t) g.i + 1] - g.r) < 1e-5f);
                        ++demoGoldens;
                    }
                }
                // MiniSynth (REA_rodada41): every state plays on the exact engine (tables from the installation's
                // oscdata.dat) and sounds; two demo channels match golden samples of our round-41 model (bit-identical to
                // renders of the installation's DLL for these states)
                for (const auto& c : p.channels)
                    if (c.pluginName == "MiniSynth")
                        ++miniSynth, miniSynthUsable += lynx::miniSynthStateUsable (c.pluginState) ? 1 : 0;
                for (const auto& c : full.channels)
                {
                    if (c.instrument != lynx::InstrumentKind::minisynth)
                        continue;
                    ++miniSynthKind;
                    auto iv = [] (double vel) { return (std::pow (11.0, vel / 100.0) - 1.0) / 10.0; };
                    const auto exo = file.find ("Exoplanet") != std::string::npos;
                    const auto golden = exo && (c.name == "Main Lead 1" || c.name == "Riser");
                    const auto y = lynx::minisynth::renderReference (c.pluginState,
                                                                     { { 0, 60, iv (100), 1.0, 0.0, 0.0, 30000 }, { 11025, 64, iv (80), 1.0, 0.0, 0.0, 40000 },
                                                                       { 22050, 67, iv (110), 1.0, 0.0, 0.0, 52000 }, { 60000, 48, iv (127), 1.0, 0.0, 0.0, 70000 } },
                                                                     44100.0, golden ? 88200 : 22050, c.samplePath);
                    double e = 0.0;
                    bool finite = ! y.empty();
                    for (auto v : y)
                        finite = finite && std::isfinite (v), e += (double) v * v;
                    CHECK (finite);
                    miniSynthSounding += e > 1e-6 ? 1 : 0;
                    if (golden && ! y.empty())
                    {
                        // our model, same events, tempo 120, state as saved, 44.1 kHz, host blocks of 512, L = R = Final.Vol
                        const auto lead = c.name == "Main Lead 1";
                        CHECK (std::abs (std::sqrt (e / (double) y.size()) - (lead ? 0.177495801753 : 0.19006567413)) < 1e-9);
                        static const struct { int i; float v; } leadAt[] = { { 2000, -0.113459975f }, { 15000, -0.00234968495f }, { 30000, 0.13985157f }, { 50000, -0.267063439f }, { 65000, -0.244418889f }, { 80000, -0.0162940286f } },
                                                                riserAt[] = { { 2000, -0.0603227727f }, { 15000, 0.112200871f }, { 30000, 0.0453962795f }, { 50000, 0.234380022f }, { 65000, -0.164545432f }, { 80000, -0.000485120923f } };
                        for (const auto& g : lead ? std::vector (std::begin (leadAt), std::end (leadAt)) : std::vector (std::begin (riserAt), std::end (riserAt)))
                            CHECK (y[2 * (std::size_t) g.i] == g.v && y[2 * (std::size_t) g.i + 1] == g.v);
                        ++miniSynthGoldens;
                    }
                }
                // PoiZone (REA_rodada44): every state plays on the exact engine and sounds; the demo channel matches golden
                // samples of our round-44 model
                for (const auto& c : p.channels)
                    if (c.pluginName == "PoiZone")
                        ++poizone, poizoneUsable += lynx::poizoneStateUsable (c.pluginState) ? 1 : 0;
                for (const auto& c : full.channels)
                {
                    if (c.instrument != lynx::InstrumentKind::poizone)
                        continue;
                    ++poizoneKind;
                    auto iv = [] (double vel) { return (std::pow (11.0, vel / 100.0) - 1.0) / 10.0; };
                    const auto golden = c.name == "PoiZone" && file.find ("NIGHT TERROR") != std::string::npos;
                    const auto y = lynx::poizone::renderReference (c.pluginState,
                                                                   { { 0, 60, iv (100), 1.0, 0.0, 0.0, 30000 }, { 11025, 64, iv (80), 1.0, 0.0, 0.0, 40000 },
                                                                     { 22050, 67, iv (110), 1.0, 0.0, 0.0, 52000 }, { 60000, 48, iv (127), 1.0, 0.0, 0.0, 70000 } },
                                                                   44100.0, golden ? 88200 : 22050);
                    double e = 0.0;
                    bool finite = ! y.empty();
                    for (auto v : y)
                        finite = finite && std::isfinite (v), e += (double) v * v;
                    CHECK (finite);
                    poizoneSounding += e > 1e-6 ? 1 : 0;
                    if (golden && ! y.empty())
                    {
                        // our model, same events, tempo 120, state as saved, 44.1 kHz, L = R = Final.Vol
                        CHECK (std::abs (std::sqrt (e / (double) y.size()) - 0.018975758213507415) < 1e-7);
                        static const struct { int i; float l, r; } at[] = { { 2000, -0.0030244605f, -0.0031888473f }, { 15000, 0.002743553f, -0.013375883f }, { 30000, 0.031268276f, 0.04751329f }, { 50000, 0.049796376f, 0.035944063f }, { 65000, -0.0014251951f, -0.01722545f }, { 80000, 0.01474628f, 0.000680499f } };
                        for (const auto& g : at)
                            CHECK (std::abs (y[2 * (std::size_t) g.i] - g.l) < 1e-7f && std::abs (y[2 * (std::size_t) g.i + 1] - g.r) < 1e-7f);
                        ++poizoneGoldens;
                    }
                }
                // Toxic Biohazard (REA_rodada45): every demo state plays on the exact engine with the waves read from the
                // installation's DLL and sounds; the "Bombs Away" channel matches golden samples of our round-45 model
                for (const auto& c : p.channels)
                    if (c.pluginName == "Toxic Biohazard")
                        ++toxic, toxicUsable += lynx::toxicStateUsable (c.pluginState) ? 1 : 0;
                for (const auto& c : full.channels)
                {
                    if (c.instrument != lynx::InstrumentKind::toxic)
                        continue;
                    ++toxicKind;
                    auto iv = [] (double vel) { return (std::pow (11.0, vel / 100.0) - 1.0) / 10.0; };
                    const auto golden = file.find ("Bombs Away") != std::string::npos;
                    const auto y = lynx::toxic::renderReference (
                        c.pluginState,
                        { { 0, 60, iv (100), 1.0, 0.0, 0.0, 30000 }, { 11025, 64, iv (80), 1.0, 0.0, 0.0, 40000 },
                          { 22050, 67, iv (110), 1.0, 0.0, 0.0, 52000 }, { 60000, 48, iv (127), 1.0, 0.0, 0.0, 70000 } },
                        44100.0, golden ? 88200 : 22050, c.samplePath, 512, 140.0);
                    double e = 0.0;
                    bool finite = ! y.empty();
                    for (auto v : y)
                        finite = finite && std::isfinite (v), e += (double) v * v;
                    CHECK (finite);
                    toxicSounding += e > 1e-6 ? 1 : 0;
                    if (golden && ! y.empty())
                    {
                        // our model (r45/py/toxmodel.py, which equals the DLL bit for bit), state as saved, tempo 140, 44.1 kHz
                        CHECK (std::abs (std::sqrt (e / (double) y.size()) - 0.0832500924945) < 1e-6);
                        static const struct { int i; float l, r; } at[] = { { 2000, -0.0501383618f, -0.100676455f }, { 15000, -0.0949445963f, -0.0714591742f }, { 30000, -0.0305469204f, 0.037349131f }, { 50000, 0.0255811438f, -0.0238218959f }, { 65000, -0.111861639f, -0.113630719f }, { 80000, 0.0163135566f, 0.0199630354f } };
                        for (const auto& g : at)
                            CHECK (std::abs (y[2 * (std::size_t) g.i] - g.l) < 1e-6f && std::abs (y[2 * (std::size_t) g.i + 1] - g.r) < 1e-6f);
                        ++toxicGoldens;
                    }
                }
                for (const auto& c : full.channels)
                {
                    if (c.instrument == lynx::InstrumentKind::harmless && c.modelParams.size() >= (std::size_t) lynx::harmlessParamCount
                        && c.modelParams[(std::size_t) lynx::harmlessComboIndex + 3] > 0.5f)
                    {
                        ++harmlessCompWanted;
                        harmlessCompLoaded += c.modelParams.size() > (std::size_t) lynx::harmlessCompIndex + 1000 ? 1 : 0;
                    }
                    flKeysChannels += c.zones.size() == 30 && c.zones.front().path.find ("CHANGPIANO") != std::string::npos ? 1 : 0;
                    if (c.sampler.slices.size() >= 4)
                    {
                        ++slicedChannels;
                        CHECK (std::is_sorted (c.sampler.slices.begin(), c.sampler.slices.end()));
                        CHECK (c.sampler.slices.back() < 1.0f);
                    }
                    dwExact += c.instrument == lynx::InstrumentKind::directwave ? 1 : 0;
                    if (! c.zones.empty() && c.name.find (" - ") == std::string::npos)
                    {
                        ++dwChannels;
                        dwZones += (int) c.zones.size();
                    }
                    padChannels += c.name.find (" - ") != std::string::npos && c.instrument == lynx::InstrumentKind::sampler ? 1 : 0;
                }
                fpcKits += hasFpc;
                // Maximus / Soundgoodizer become the exact engine (REA_rodada40); Vocodex carries SG.dat (and its
                // carrier's Sytrus preset) read from the installation
                const int basicSlots0 = basicSlots, basicExact0 = basicExact;
                for (const auto& insert : p.mixer)
                    for (const auto& slot : insert.slots)
                    {
                        maximusSlots += slot.plugin == "Maximus";
                        delay2Slots += slot.plugin == "Fruity Delay 2";
                        delay3Slots += slot.plugin == "Fruity Delay 3";
                        sgzSlots += slot.plugin == "Soundgoodizer";
                        vocodexSlots += slot.plugin == "Vocodex";
                        shaperSlots += slot.plugin == "Fruity WaveShaper";
                        flPeqSlots += slot.plugin == "Fruity Parametric EQ 2" || slot.plugin == "Fruity Parametric EQ";
                        equoSlots += slot.plugin == "EQUO";
                        distructorSlots += slot.plugin == "Distructor";
                        pitchShifterSlots += slot.plugin == "Pitch Shifter";
                        clipperSlots += slot.plugin == "Fruity Soft Clipper";
                        basicSlots += slot.plugin == "Fruity PanOMatic" || slot.plugin == "Fruity Balance" || slot.plugin == "Fruity Blood Overdrive";
                        filterSlots += slot.plugin == "Fruity Fast LP" || slot.plugin == "Fruity Free Filter" || slot.plugin == "Fruity Filter";
                        chorusSlots += lynx::chorusControls (slot.plugin) > 0 && slot.state.size() >= 4 ? 1 : 0;
                        fastDistSlots += slot.plugin == "Fruity Fast Dist";
                        flangusSlots += slot.plugin == "Fruity Flangus";
                        flangerSlots += slot.plugin == "Fruity Flanger";
                        phaserSlots += slot.plugin == "Fruity Phaser";
                    }
                // (the slots and the nodes of the Patcher graphs, as when Patchers were flattened into slots)
                std::vector<const lynx::EffectSlot*> allSlots;
                std::function<void (const lynx::EffectSlot&)> collect = [&] (const lynx::EffectSlot& fx) {
                    allSlots.push_back (&fx);
                    for (const auto& n : fx.patcher.nodes)
                        if (! n.generator)
                            collect (n.effect);
                };
                for (const auto& t : full.mixer)
                    for (const auto& fx : t.effects)
                        collect (fx);
                for (const auto* slotPointer : allSlots)
                    {
                        const auto& fx = *slotPointer;
                        if (fx.kind == lynx::EffectKind::maximus)
                        {
                            const auto blob = lynx::maximusfx::unpack (fx.state);
                            CHECK (blob && lynx::maximusfx::presetOf (*blob) != nullptr);
                            if (blob)
                                (blob->build == lynx::hlfx::MaximusBuild::soundgoodizer ? sgzExact : maximusExact) += 1;
                        }
                        fastDistExact += fx.kind == lynx::EffectKind::fruityFastDist;
                        flangusExact += fx.kind == lynx::EffectKind::fruityFlangus;
                        flangerExact += fx.kind == lynx::EffectKind::fruityFlanger;
                        phaserExact += fx.kind == lynx::EffectKind::fruityPhaser;
                        if (fx.kind == lynx::EffectKind::vocodex)
                            vocodexWithData += ! lynx::vocodex::splitInstallData (fx.state).second.sgDat.empty();
                        if (fx.kind == lynx::EffectKind::fruityWaveShaper)
                            shaperExact += lynx::flshape::parseWaveShaper (fx.state).has_value() ? 1 : 0;
                        clipperExact += fx.kind == lynx::EffectKind::fruitySoftClipper;
                        flPeqExact += fx.kind == lynx::EffectKind::parametricEq && lynx::peq::flLaw (fx.params);
                        basicExact += fx.kind == lynx::EffectKind::fruityPanOMatic || fx.kind == lynx::EffectKind::fruityBalance
                                      || fx.kind == lynx::EffectKind::fruityBloodOverdrive;
                        equoExact += fx.kind == lynx::EffectKind::equo && lynx::equofx::parse (fx.state).has_value();
                        if (fx.kind == lynx::EffectKind::distructor)
                            if (const auto d = lynx::distructor::parseBlob (fx.state))
                            {
                                ++distructorExact;
                                for (const auto& m : d->modules)
                                    distructorImpulses += m.type == 3 && ! m.impulseFile.empty();
                            }
                        pitchShifterExact += fx.kind == lynx::EffectKind::flPitchShifter;
                        filterExact += fx.kind == lynx::EffectKind::fruityFastLP || fx.kind == lynx::EffectKind::fruityFreeFilter
                                       || fx.kind == lynx::EffectKind::fruityFilter;
                        delay2Exact += fx.kind == lynx::EffectKind::fruityDelay2;
                        delay3Exact += fx.kind == lynx::EffectKind::fruityDelay3;
                        chorusExact += fx.kind == lynx::EffectKind::fruityChorus || fx.kind == lynx::EffectKind::hyperChorus || fx.kind == lynx::EffectKind::vintageChorus ? 1 : 0;
                    }
                if (basicSlots - basicSlots0 != basicExact - basicExact0)
                    std::printf ("  %s: %d PanOMatic/Balance/Blood Overdrive slots, %d exact effects\n", file.c_str(),
                                 basicSlots - basicSlots0, basicExact - basicExact0);
                for (const auto& pat : full.patterns)
                    for (const auto& n : pat.notes)
                    {
                        CHECK (n.channel >= 0 && n.channel < (int) full.channels.size());
                        padNotes += full.channels[(std::size_t) n.channel].name.find (" - ") != std::string::npos ? 1 : 0;
                    }
            }

            const auto peqDefaults = lynx::makeEffectSlot (lynx::EffectKind::parametricEq).params;
            for (const auto& t : song.mixer)
                for (const auto& fx : t.effects)
                {
                    if (fx.kind == lynx::EffectKind::parametricEq)
                    {
                        ++peqSlots;
                        peqShaped += fx.params != peqDefaults;
                        for (double hz : { 50.0, 1000.0, 10000.0 })
                            CHECK (std::isfinite (lynx::peq::responseDb (fx.params, 48000.0, hz)));
                    }
                    if (fx.kind == lynx::EffectKind::reverb)
                    {
                        ++reverbs;
                        reverbsDryless += fx.params[4] < 0.01f;
                        reverbsLowCut += fx.params[5] > 0.0f;
                    }
                    if (fx.kind == lynx::EffectKind::reeverb2) // FL's exact Reeverb 2 (REA_rodada49): FL's own controls
                    {
                        const auto r2 = lynx::reeverb2::fromParams (fx.params);
                        ++reverbs;
                        reverbsDryless += r2.p[10] == 0;
                        reverbsLowCut += r2.p[0] >= 20;
                    }
                }

            for (const auto& t : song.mixer)
                if (t.eq.active())
                {
                    ++trackEqs;
                    for (std::size_t b = 0; b < 3; ++b)
                        CHECK (t.eq.hz[b] >= 10.0f && t.eq.hz[b] <= 16000.0f && std::abs (t.eq.gainDb[b]) <= 18.0f);
                }

            for (const auto& a : song.arrangements)
                for (const auto& m : a.markers)
                {
                    ++markers;
                    signatures += m.isSignature();
                    CHECK (m.tick >= 0);
                }

            // Fruity Wrapper (channels and mixer slots): every blob in the demos is read
            auto countWrapper = [&] (const std::string& plugin, const std::vector<std::uint8_t>& state) {
                if (plugin != "Fruity Wrapper")
                    return;
                ++wrapped;
                lynx::ForeignPlugin f;
                wrappedParsed += lynx::parseFlWrapper (state, f) && f.format != lynx::ForeignPlugin::Format::none && ! f.name.empty();
                if (f.format == lynx::ForeignPlugin::Format::vst2)
                {
                    // VST2 state (REA_rodada89 §2): every one decodes, and writes back to the same bytes — except the
                    // wrapped Fruity effects of the FL 11 render tests (their mixer slots are read since round 80):
                    // those decode but do not write back byte-identical (not confirmed why)
                    ++wrappedVst2;
                    lynx::FlVst2State st;
                    if (lynx::parseFlVst2State (f.state, st) && st.version == -9)
                    {
                        ++wrappedVst2State;
                        wrappedVst2Old += lynx::writeFlVst2State (st) != f.state;
                    }
                }
            };
            for (const auto& c : p.channels)
                countWrapper (c.pluginName, c.pluginState);
            for (const auto& insert : p.mixer)
                for (const auto& slot : insert.slots)
                    countWrapper (slot.plugin, slot.state);

            // Harmless: every state in the demos becomes a synth, with finite values
            for (const auto& c : p.channels)
                if (c.pluginName == "Harmless")
                {
                    ++harmless;
                    lynx::SynthSettings hs;
                    float g = 1.0f;
                    if (lynx::synthFromHarmless (c.pluginState, 128.0, hs, g))
                    {
                        ++harmlessDecoded;
                        CHECK (std::isfinite (g) && hs.cutoff >= 20.0f && hs.cutoff <= 20000.0f && std::isfinite (hs.decay) && std::isfinite (hs.release));
                    }
                    if (const auto hp = lynx::harmlessParams (c.pluginState))
                        harmlessParsed += (int) hp->size() == lynx::harmlessParamCount ? 1 : 0;
                }
            for (const auto& c : song.channels)
                harmlessKind += c.instrument == lynx::InstrumentKind::harmless ? 1 : 0;

            // Harmor: every state in the demos plays on the exact engine (REA_rodada34) and sounds, with finite samples
            for (const auto& c : p.channels)
                if (c.pluginName == "Harmor")
                {
                    ++harmor;
                    harmorUsable += lynx::harmorStateUsable (c.pluginState) ? 1 : 0;
                }
            for (const auto& c : song.channels)
                if (c.instrument == lynx::InstrumentKind::harmor)
                {
                    ++harmorKind;
                    const auto y = lynx::harmor::renderReference (c.pluginState, { { 0, 60, 0.8, 1.0, 0.0, 6000 }, { 2000, 67, 0.6, 1.0, 0.0, 8000 } }, 44100.0, 11025);
                    double e = 0.0;
                    bool finite = ! y.empty();
                    for (auto v : y)
                        finite = finite && std::isfinite (v), e += (double) v * v;
                    CHECK (finite);
                    harmorSounding += e > 1e-6 ? 1 : 0;
                }

            // Ogun: every state in the demos plays on the exact engine (REA_rodada37) and sounds, with finite samples
            for (const auto& c : p.channels)
                if (c.pluginName == "Ogun")
                {
                    ++ogun;
                    ogunUsable += lynx::ogunStateUsable (c.pluginState) ? 1 : 0;
                }
            for (const auto& c : song.channels)
                if (c.instrument == lynx::InstrumentKind::ogun)
                {
                    ++ogunKind;
                    const auto y = lynx::ogun::renderReference (c.pluginState, { { 0, 60, 0.8, 1.0, 0.0, 0.0, 0.0, 0.0, 0.5, 6000 },
                                                                                 { 2000, 67, 0.6, 1.0, 0.0, 0.0, 0.0, 0.0, 0.5, 8000 } }, 44100.0, 11025);
                    double e = 0.0;
                    bool finite = ! y.empty();
                    for (auto v : y)
                        finite = finite && std::isfinite (v), e += (double) v * v;
                    CHECK (finite);
                    ogunSounding += e > 1e-6 ? 1 : 0;
                }

            // Autogun (REA_rodada57): every state plays on Ogun's engine in its Autogun variant; with the installation, the
            // Magic Dust stage gets Autogun\Data\SG.dat
            {
                bool any = false;
                for (const auto& c : p.channels)
                    if (c.pluginName == "Autogun")
                        ++autogun, autogunUsable += lynx::autogunStateUsable (c.pluginState) ? 1 : 0, any = true;
                if (any)
                    for (const auto& c : lynx::importFlp (p, withFiles).channels)
                        if (c.instrument == lynx::InstrumentKind::autogun)
                        {
                            ++autogunKind;
                            lynx::ogun::RefOptions opt;
                            opt.autogun = true;
                            if (! c.samplePath.empty())
                            {
                                auto local = c.samplePath;
                                std::replace (local.begin(), local.end(), '\\', '/');
                                std::ifstream sg (local, std::ios::binary);
                                opt.sgState.assign ((std::istreambuf_iterator<char> (sg)), std::istreambuf_iterator<char>());
                                autogunWithSg += opt.sgState.size() == 1342 ? 1 : 0;
                            }
                            const auto y = lynx::ogun::renderReference (c.pluginState, { { 0, 48, 0.8, 1.0, 0.0, 0.0, 0.0, 0.0, 0.5, 6000 } },
                                                                        44100.0, 11025, opt);
                            double e = 0.0;
                            bool finite = ! y.empty();
                            for (auto v : y)
                                finite = finite && std::isfinite (v), e += (double) v * v;
                            CHECK (finite);
                            autogunSounding += e > 1e-6 ? 1 : 0;
                        }
            }

            // Fruity DX10: every state plays on the exact engine when its channel processor is neutral (REA_rodada42)
            for (const auto& c : p.channels)
                if (c.pluginName == "Fruity DX10" || c.pluginName == "DX10")
                    ++dx10, dx10Usable += lynx::dx10StateUsable (c.pluginState) ? 1 : 0;
            for (const auto& c : song.channels)
                if (c.instrument == lynx::InstrumentKind::dx10)
                {
                    ++dx10Kind;
                    const auto y = lynx::dx10::renderReference (c.pluginState, { { 0, 60, 1.0, 1.0, 0.0, 0.0, 6000 }, { 2000, 67, 0.6, 1.0, 0.0, 0.0, 8000 } },
                                                                44100.0, 11025);
                    double e = 0.0;
                    bool finite = ! y.empty();
                    for (auto v : y)
                        finite = finite && std::isfinite (v), e += (double) v * v;
                    CHECK (finite);
                    dx10Sounding += e > 1e-6 ? 1 : 0;
                }

            // 3x Osc: every channel plays on the exact engine (REA_rodada47)
            for (const auto& c : song.channels)
                if (c.instrument == lynx::InstrumentKind::threeosc)
                {
                    ++threeOscKind;
                    const auto y = lynx::threeosc::renderReference (c.pluginState, { { 0, 60, 1.0, 0.0, 0.0, 6000 }, { 2000, 67, 0.6, 0.0, 0.0, 8000 } },
                                                                    44100.0, 11025);
                    double e = 0.0;
                    bool finite = ! y.empty();
                    for (auto v : y)
                        finite = finite && std::isfinite (v), e += (double) v * v;
                    CHECK (finite);
                    threeOscSounding += e > 1e-6 ? 1 : 0;
                }

            // Sytrus: every state in the demos (zlib + 10 units) decodes, with finite values
            for (const auto& c : p.channels)
                if (c.pluginName == "Sytrus")
                {
                    ++sytrus;
                    lynx::SynthSettings ss;
                    float g = 1.0f;
                    // the Sytrus engine (REA_rodada31) reads every demo state; the first 24 play a short note (render settings)
                    if (lynx::sytrus::stateUsable (c.pluginState))
                    {
                        ++sytrusEngine;
                        if (sytrusRendered < 24)
                        {
                            ++sytrusRendered;
                            lynx::sytrus::Engine e;
                            e.prepare (44100.0, true, 512);
                            e.setPatch (lynx::sytrus::parsePatch (c.pluginState));
                            lynx::sytrus::NoteParams note;
                            e.noteOn (note);
                            std::vector<float> l (8192), r (8192);
                            e.render (l.data(), r.data(), 8192, 0.551f, 0.0f);
                            bool finite = true;
                            double energy = 0.0;
                            for (std::size_t k = 0; k < l.size(); ++k)
                                finite = finite && std::isfinite (l[k]) && std::isfinite (r[k]), energy += (double) l[k] * l[k];
                            CHECK (finite);
                            sytrusSounding += energy > 1e-6;
                        }
                    }
                    if (lynx::synthFromSytrus (c.pluginState, 128.0, ss, g))
                    {
                        ++sytrusDecoded;
                        CHECK (std::isfinite (g) && ss.cutoff >= 20.0f && ss.cutoff <= 20000.0f && std::isfinite (ss.attack) && std::isfinite (ss.decay)
                               && std::isfinite (ss.release) && ss.sustain >= 0.0f && ss.sustain <= 1.0f && std::isfinite (ss.filterEnv.amount));
                        for (const auto& o : ss.osc)
                            CHECK (o.level >= 0.0f && o.level <= 1.0f && o.octave >= -3 && o.octave <= 3);
                    }
                }

            // round 12 generators
            for (const auto& c : p.channels)
            {
                lynx::SynthSettings gs;
                float g = 1.0f;
                int decoded = -1;
                if (c.pluginName == "Harmor")
                {
                    decoded = lynx::synthFromHarmor (c.pluginState, gs, g);
                    if (decoded)
                    {
                        // REA_rodada25 §4: most resonant settings stay below Q ≈ 3, a few saturate
                        CHECK (gs.resonance >= 0.0f && gs.resonance <= 1.0f);
                        static int resonant = 0, strong = 0, total = 0;
                        resonant += gs.resonance > 0.0f;
                        strong += gs.resonance > 0.9f;
                        if (++total % 50 == 0)
                            std::printf ("  Harmor resonance: %d with a peak, %d with Q > ~5 (of %d)\n", resonant, strong, total);
                    }
                    static int saw = 0, square = 0, other = 0, total = 0;
                    if (decoded && std::any_of (gs.osc.begin(), gs.osc.end(), [] (const lynx::SynthOscillator& o) { return o.wave == lynx::Waveform::spectral; }))
                    {
                        auto isSaw = true, isSquare = true;
                        for (int n = 1; n <= 16; ++n)
                        {
                            isSaw = isSaw && std::abs (gs.spectrum[(std::size_t) n - 1] - 1.0f / n) < 1e-3f;
                            isSquare = isSquare && std::abs (gs.spectrum[(std::size_t) n - 1] - (n % 2 == 1 ? 1.1535f / n : 0.0f)) < 2e-3f;
                        }
                        ++(isSaw ? saw : isSquare ? square : other);
                        if (++total % 20 == 0 || total == 100)
                            std::printf ("  Harmor spectrum: %d saw, %d square, %d other (of %d)\n", saw, square, other, total);
                    }
                }
                else if (c.pluginName == "GMS")
                    decoded = lynx::synthFromGms (c.pluginState, 128.0, gs, g);
                else if (c.pluginName == "Ogun" || c.pluginName == "Autogun")
                {
                    decoded = lynx::synthFromOgun (c.pluginState, gs, g);
                    // REA_rodada24 §1.9: the "Trumpet" preset (richness 7, no seeds) has partials 0.9087, 0.8610,
                    // 0.8151… at the fixed seed-3 phases 0.919, 0.380, 0.130
                    if (decoded && gs.osc[0].wave == lynx::Waveform::spectral)
                    {
                        CHECK (std::all_of (gs.spectrum.begin(), gs.spectrum.end(), [] (float a) { return std::isfinite (a) && a >= 0.0f; }));
                        static constexpr float trumpet[3] { 0.9087f, 0.8610f, 0.8151f }, phases[3] { 0.919f, 0.380f, 0.130f };
                        auto match = true;
                        for (std::size_t k = 0; k < 3; ++k)
                            match = match && std::abs (gs.spectrum[k] * 2.0f / 3.14159265f - trumpet[k]) < 1e-3f && std::abs (gs.spectrumPhase[k] - phases[k]) < 1e-3f;
                        ogunTrumpet += match ? 1 : 0;
                    }
                }
                else if (c.pluginName == "Sakura")
                    decoded = lynx::synthFromSakura (c.pluginState, gs);
                else if (c.pluginName == "Kepler")
                    decoded = lynx::synthFromKepler (c.pluginState, gs, g);
                else if (c.pluginName == "Wave Traveller")
                    decoded = lynx::waveTravellerSample (c.pluginState).has_value() ? 2 : 0;
                else if (c.pluginName == "PoiZone")
                    decoded = lynx::synthFromPoiZone (c.pluginState, gs, g);
                else if (c.pluginName == "BassDrum")
                    decoded = lynx::synthFromBassDrum (c.pluginState, gs, g);
                else if (c.pluginName == "Toxic Biohazard")
                    decoded = lynx::synthFromToxic (c.pluginState, gs, g);
                else if (c.pluginName == "Morphine")
                    decoded = lynx::synthFromMorphine (c.pluginState, gs, g);
                else if (c.pluginName == "BooBass")
                    decoded = lynx::synthFromBooBass (c.pluginState, gs);
                else if (c.pluginName == "Drumpad" || c.pluginName == "DrumPad")
                    decoded = lynx::drumpadPatchName (c.pluginState).has_value() ? 2 : 0;
                else if (c.pluginName == "Fruity Envelope Controller" && c.pluginState.size() >= 4 && c.pluginState[2] >= 6) // v >= 6
                    decoded = lynx::parseEnvelopeController (c.pluginState).has_value() ? 2 : 0;
                else if (c.pluginName == "FLEX")
                    decoded = lynx::flexPresetName (c.pluginState).has_value() ? 2 : 0;
                else if (c.pluginName == "Fruity DrumSynth Live")
                    decoded = lynx::drumSynthLiveParams (c.pluginState).has_value() ? 2 : 0;
                if (decoded < 0)
                    continue;
                auto& counter = generators[c.pluginName];
                ++counter.first;
                counter.second += decoded != 0;
                if (decoded == 1 && c.pluginName != "Fruity DrumSynth Live")
                {
                    CHECK (std::isfinite (g) && g >= 0.0f && gs.cutoff >= 20.0f && gs.cutoff <= 20000.0f && gs.sustain >= 0.0f && gs.sustain <= 1.0f);
                    CHECK (std::isfinite (gs.attack) && std::isfinite (gs.decay) && std::isfinite (gs.release) && std::isfinite (gs.filterEnv.amount) && std::isfinite (gs.lfo[lynx::lfoCutoff].amount));
                    for (const auto& o : gs.osc)
                        CHECK (o.level >= 0.0f && o.level <= 4.0f && std::isfinite (o.detune) && std::isfinite (o.pan));
                }
            }

            // 3x Osc: every state in the demos decodes, with levels summing to at most 1
            for (const auto& c : p.channels)
                if (c.pluginName == "3x Osc")
                {
                    ++threeOsc;
                    lynx::SynthSettings s;

                    if (lynx::synthFrom3xOsc (c.pluginState, s))
                    {
                        ++threeOscDecoded;
                        // with AM, osc 3 is the modulation depth and is left out of the sum
                        const auto sum = s.osc[0].level + s.osc[1].level + (s.am ? 0.0f : s.osc[2].level);
                        CHECK (sum <= 1.001f && s.osc[2].level <= 1.0f);
                        for (const auto& o : s.osc)
                            CHECK (o.octave >= -2 && o.octave <= 2 && std::abs (o.semitones) <= 11 && std::abs (o.pan) <= 1.0f);
                    }
                }

            for (auto& pat : p.patterns)
                if (! pat.notes.empty())
                {
                    ++withNotes;
                    break;
                }

            for (auto& a : p.arrangements)
                for (auto& item : a.items)
                    CHECK (item.track >= 0 && item.track < 500);

            for (auto& clip : song.playlist)
            {
                CHECK (clip.pattern >= 0 && clip.pattern < (int) song.patterns.size());
                CHECK (clip.length > 0 && clip.offset >= 0);

                if (clip.kind == lynx::ClipKind::automation)
                    CHECK (clip.automation >= 0 && clip.automation < (int) song.automations.size());
            }

            for (auto& a : song.automations)
            {
                ++automations;
                automationsWithTarget += a.target.kind != lynx::AutomationTargetKind::none;
                CHECK (a.isLive() || ! a.points.empty());

                for (std::size_t i = 1; i < a.points.size(); ++i)
                    CHECK (a.points[i].tick >= a.points[i - 1].tick);
            }

            // imported routing has no cycles and valid destinations
            CHECK (song.mixerOrder().size() + 1 == song.mixer.size());
            for (auto& t : song.mixer)
                for (auto& s : t.sends)
                {
                    ++sends;
                    CHECK (s.target > 0 && s.target < (int) song.mixer.size());
                }

            if (! song.playlist.empty())
                ++withPlaylist;
        }
        catch (const flp::Error& e)
        {
            ++failures;
            std::printf ("  FAILED %s: %s\n", entry.path().string().c_str(), e.what());
        }
    }

    CHECK (threeOscDecoded == threeOsc);
    std::printf ("  Harmless: %d channels, %d decoded\n", harmless, harmlessDecoded);
    std::printf ("  Sytrus: %d channels, %d decoded, %d by the engine (%d of %d rendered sound)\n", sytrus, sytrusDecoded, sytrusEngine,
                 sytrusSounding, sytrusRendered);
    CHECK (sytrusEngine == sytrus);
    CHECK (sytrusSounding >= sytrusRendered * 3 / 4);
    for (const auto& [name, counter] : generators)
    {
        std::printf ("  %s: %d channels, %d decoded\n", name.c_str(), counter.first, counter.second);
        CHECK (counter.second == counter.first);
    }
    std::printf ("  Ogun spectra matching the Trumpet reference: %d\n", ogunTrumpet);
    CHECK (ogunTrumpet >= 1);

    // demo Gross Beat: the volume slot becomes the gate (those using an envelope slot get steps < 1)
    {
        int vocodexes = 0, vocodexSides = 0;
        int lovePhilterLinks[4] {}; // live links: cutoff, resonance; inside containers: cutoff, resonance
        int embeddedZones = 0, lovePhilters = 0, lovePhiltersRead = 0, lovePhiltersConverted = 0, gates = 0, shaped = 0, effectors = 0, patchers = 0, serial = 0, parallel = 0, generatorPatchers = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator (std::filesystem::path (realFlpDir) / "Demo projects"))
            if (entry.path().extension() == ".flp")
            {
                std::ifstream in (entry.path(), std::ios::binary);
                const std::vector<std::uint8_t> file ((std::istreambuf_iterator<char> (in)), {});
                try
                {
                    const auto project = flp::parseProject (file);
                    for (const auto& insert : project.mixer)
                        for (const auto& slot : insert.slots)
                            if (slot.plugin == "Gross Beat")
                            {
                                // FL's own engine (REA_rodada53): every demo state is read
                                const auto fx = lynx::effectFromFl (slot.plugin, slot.state);
                                CHECK (fx && fx->kind == lynx::EffectKind::grossBeat);
                                ++gates;
                                if (fx && fx->kind == lynx::EffectKind::grossBeat && lynx::grossbeat::slotNames (fx->state, true).size() == 36)
                                    ++shaped;
                            }
                            else if (slot.plugin == "Fruity Love Philter")
                            {
                                ++lovePhilters;
                                lovePhiltersRead += lynx::parseLovePhilter (slot.state).has_value();
                                if (const auto conv = lynx::lovePhilterEffects (slot.state))
                                {
                                    ++lovePhiltersConverted;
                                    CHECK (! conv->slots.empty());
                                    for (const auto& fx : conv->slots)
                                        for (auto v : fx.params)
                                            CHECK (std::isfinite (v) && v >= 0.0f && v <= 1.0f);
                                    for (const auto& fx : conv->slots)
                                        for (const auto& link : fx.links)
                                        {
                                            const auto& chain = fx.branches[(std::size_t) link.branch].chain;
                                            CHECK (link.source < (int) chain.size() && chain[(std::size_t) link.target].kind == lynx::EffectKind::parametricEq);
                                            lovePhilterLinks[2 + (link.param == lynx::peq::param (0, 3) ? 1 : 0)]++;
                                        }
                                    for (const auto& link : conv->links)
                                    {
                                        lovePhilterLinks[link.param == lynx::peq::param (0, 3) ? 1 : 0]++;
                                        CHECK (link.curve.size() == 33);
                                        for (auto v : link.curve)
                                            CHECK (std::isfinite (v) && v >= 0.0f && v <= 1.0f);
                                    }
                                }
                            }
                            else if (slot.plugin == "Vocodex")
                            {
                                const auto v = lynx::vocodexEffect (slot.state);
                                CHECK (v.has_value());
                                vocodexes += v.has_value();
                                vocodexSides += v && (v->modIn > 0 || v->carIn > 0);
                            }
                            else if (slot.plugin == "Effector")
                                effectors += lynx::effectFromFl (slot.plugin, slot.state).has_value();
                            else if (slot.plugin == "Patcher")
                            {
                                const auto graph = lynx::parsePatcher (slot.state);
                                CHECK (graph.has_value());
                                ++patchers;
                                if (graph && ! graph->serialChain.empty())
                                    ++serial;
                                if (graph && graph->serialChain.empty() && graph->audio)
                                    ++parallel;
                            }
                    for (const auto& c : project.channels)
                        if (c.pluginName == "DirectWave")
                            if (const auto program = lynx::directWaveProgram (c.pluginState))
                                for (const auto& wav : program->embeddedWav)
                                    if (! wav.empty())
                                    {
                                        ++embeddedZones;
                                        CHECK (wav.size() > 44 && std::memcmp (wav.data(), "RIFF", 4) == 0);
                                    }
                    for (const auto& c : project.channels)
                        if (c.pluginName == "Patcher")
                        {
                            const auto graph = lynx::parsePatcher (c.pluginState);
                            CHECK (graph.has_value());
                            generatorPatchers += graph && graph->mainGenerator >= 0;
                        }
                }
                catch (const flp::Error&) {}
            }
        CHECK (gates >= 30 && shaped == gates);
        std::printf ("  Gross Beat: %d slots, %d read exactly; Effector: %d converted; Love Philter: %d, %d read\n", gates, shaped, effectors,
                     lovePhilters, lovePhiltersRead);
        CHECK (lovePhiltersRead == lovePhilters && lovePhiltersConverted == lovePhilters);
        std::printf ("  Vocodex: %d converted, %d with side inputs\n", vocodexes, vocodexSides);
        CHECK (vocodexes >= 9);
        std::printf ("  Love Philter live links: %d cutoff, %d resonance; in multi-unit containers: %d cutoff, %d resonance\n",
                     lovePhilterLinks[0], lovePhilterLinks[1], lovePhilterLinks[2], lovePhilterLinks[3]);
        CHECK (lovePhilterLinks[2] + lovePhilterLinks[3] > 0);
        std::printf ("  DirectWave: %d zones with embedded audio\n", embeddedZones);
        CHECK (embeddedZones == 12);
        std::printf ("  Patcher: %d in the mixer (%d serial, %d reduced to serial/parallel), %d as instrument with generator\n", patchers, serial,
                     parallel, generatorPatchers);
        CHECK (gates == 0 || shaped > 0);
    }

    // Envelope Controller "Basic Sidechain" (Future House): drops on the note and returns to ~1 within ~1/3 s
    {
        const auto path = std::filesystem::path (realFlpDir) / "Demo projects" / "Demo songs" / "Asher Postman - Future House.flp";
        if (std::filesystem::exists (path))
        {
            std::ifstream in (path, std::ios::binary);
            const std::vector<std::uint8_t> file ((std::istreambuf_iterator<char> (in)), {});
            const auto song = lynx::importFlp (flp::parseProject (file));
            const lynx::ChannelDef* controller = nullptr;
            for (const auto& c : song.channels)
                if (c.instrument == lynx::InstrumentKind::controller)
                    controller = &c;
            CHECK (controller != nullptr);
            if (controller != nullptr)
            {
                auto inst = lynx::makeInstrument (lynx::InstrumentKind::controller);
                inst->prepare (48000.0);
                inst->configure (*controller, song.bpm);
                std::vector<float> l (480), r (480);
                inst->render (l.data(), r.data(), 480, 1.0f, 1.0f);
                inst->noteOn (60, 1.0f);
                inst->render (l.data(), r.data(), 480, 1.0f, 1.0f); // 10 ms
                const auto ducked = inst->controlOutput (0);
                for (int i = 0; i < 100; ++i) // +1 s
                    inst->render (l.data(), r.data(), 480, 1.0f, 1.0f);
                const auto recovered = inst->controlOutput (0);
                std::printf ("  Envelope Controller (Basic Sidechain): %.2f on the note, %.2f after 1 s\n", ducked, recovered);
                CHECK (ducked < 0.3f && recovered > 0.9f);
            }
        }
    }

    // mutation fuzz: real states with flipped and truncated bytes must not crash the readers
    {
        std::map<std::string, std::vector<std::uint8_t>> samples; // one real blob per plugin
        for (const auto& entry : std::filesystem::recursive_directory_iterator (std::filesystem::path (realFlpDir) / "Demo projects"))
        {
            if (entry.path().extension() != ".flp")
                continue;
            std::ifstream in (entry.path(), std::ios::binary);
            const std::vector<std::uint8_t> file ((std::istreambuf_iterator<char> (in)), {});
            try
            {
                const auto project = flp::parseProject (file);
                for (const auto& c : project.channels)
                    if (! c.pluginState.empty())
                        samples.emplace (c.pluginName, c.pluginState);
                for (const auto& insert : project.mixer)
                    for (const auto& slot : insert.slots)
                        if (! slot.state.empty())
                            samples.emplace (slot.plugin, slot.state);
            }
            catch (const flp::Error&) {}
        }

        std::uint32_t seed = 12345;
        auto rnd = [&seed] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
        int mutations = 0;
        for (const auto& [name, original] : samples)
            for (int round = 0; round < 40; ++round)
            {
                auto blob = original;
                if (round % 4 == 3 && ! blob.empty())
                    blob.resize (rnd() % blob.size()); // truncated
                else
                    for (int k = 0; k < 1 + (int) (rnd() % 8) && ! blob.empty(); ++k)
                        blob[rnd() % blob.size()] = (std::uint8_t) rnd();
                ++mutations;
                lynx::SynthSettings synth;
                float gain = 1.0f;
                lynx::synthFromSytrus (blob, 120.0, synth, gain);
                lynx::synthFromHarmor (blob, synth, gain);
                lynx::synthFromGms (blob, 120.0, synth, gain);
                lynx::synthFromOgun (blob, synth, gain);
                lynx::synthFromMorphine (blob, synth, gain);
                lynx::synthFromKepler (blob, synth, gain);
                lynx::synthFromPoiZone (blob, synth, gain);
                lynx::synthFromToxic (blob, synth, gain);
                lynx::synthFromBassDrum (blob, synth, gain);
                lynx::synthFromHarmless (blob, 120.0, synth, gain);
                lynx::harmlessParams (blob);
                if (lynx::harmorStateUsable (blob))
                    lynx::harmorFallbacks (blob);
                lynx::parseEnvelopeController (blob);
                lynx::parseKeyboardController (blob);
                lynx::parseLovePhilter (blob);
                lynx::parsePatcher (blob);
                lynx::effectFromFl (name, blob);
                lynx::fpcPads (blob);
                lynx::directWaveProgram (blob);
                CHECK (std::isfinite (gain) && std::isfinite (synth.cutoff));
            }
        std::printf ("  mutation fuzz: %d blobs from %d plugins\n", mutations, (int) samples.size());
    }

    // WavPack (.wv) samples from FL packs: all decode, with finite, non-silent audio
    {
        int files = 0, decoded = 0, audible = 0;
        double seconds = 0.0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator (std::filesystem::path (realFlpDir) / "Patches" / "Packs"))
            if (entry.path().extension() == ".wv")
            {
                std::ifstream in (entry.path(), std::ios::binary);
                const std::vector<std::uint8_t> file ((std::istreambuf_iterator<char> (in)), {});
                ++files;
                lynx::DecodedAudio audio;
                if (! lynx::decodeWavPack (file.data(), file.size(), audio))
                    continue;
                ++decoded;
                float peak = 0.0f;
                bool finite = true;
                for (const auto& ch : audio.channels)
                    for (auto v : ch)
                    {
                        finite = finite && std::isfinite (v);
                        peak = std::max (peak, std::abs (v));
                    }
                CHECK (finite && peak <= 4.0f);
                audible += peak > 1e-4f;
                seconds += (double) audio.channels[0].size() / audio.sampleRate;
            }
        std::printf ("  WavPack: %d .wv files, %d decoded, %d audible, %.0f s of audio\n", files, decoded, audible, seconds);
        CHECK (decoded == files);
    }

    // native GMS presets (.gmsynth = the contents of 0xD5 without the "SSLF")
    int gmsPresets = 0, gmsDecoded = 0;
    const auto gmsFolder = std::filesystem::path (realFlpDir) / "Patches" / "Plugin presets" / "Generators" / "GMS";
    if (std::filesystem::exists (gmsFolder))
        for (const auto& entry : std::filesystem::recursive_directory_iterator (gmsFolder))
            if (entry.path().extension() == ".gmsynth")
            {
                std::ifstream in (entry.path(), std::ios::binary);
                std::vector<std::uint8_t> file ((std::istreambuf_iterator<char> (in)), {});
                std::vector<std::uint8_t> blob { 'S', 'S', 'L', 'F' };
                const auto n = (std::uint32_t) file.size();
                blob.insert (blob.end(), reinterpret_cast<const std::uint8_t*> (&n), reinterpret_cast<const std::uint8_t*> (&n) + 4);
                blob.insert (blob.end(), file.begin(), file.end());
                lynx::SynthSettings gs;
                float g = 1.0f;
                ++gmsPresets;
                if (lynx::synthFromGms (blob, 120.0, gs, g))
                {
                    ++gmsDecoded;
                    CHECK (std::isfinite (g) && gs.cutoff >= 20.0f && std::isfinite (gs.pitchEnv.amount) && std::isfinite (gs.lfo[lynx::lfoPitch].period));
                }
            }
    std::printf ("  GMS presets: %d, %d decoded\n", gmsPresets, gmsDecoded);

    // the synth's "FL presets" button: Sytrus/Harmor .fst and .gmsynth become synth settings
    int presetFiles = 0, presetsRead = 0;
    for (const auto* name : { "Sytrus", "Harmor", "GMS", "Harmless", "Ogun", "Sakura", "Fruit Kick", "Fruity DX10", "MiniSynth", "Kepler",
                              "PoiZone", "Toxic Biohazard", "BassDrum" })
    {
        const auto folder = std::filesystem::path (realFlpDir) / "Patches" / "Plugin presets" / "Generators" / name;
        if (! std::filesystem::exists (folder))
            continue;
        for (const auto& entry : std::filesystem::recursive_directory_iterator (folder))
            if (entry.path().extension() == ".fst" || entry.path().extension() == ".gmsynth")
            {
                std::ifstream in (entry.path(), std::ios::binary);
                const std::vector<std::uint8_t> file ((std::istreambuf_iterator<char> (in)), {});
                ++presetFiles;
                presetsRead += lynx::synthFromFlPreset (file).has_value();
            }
    }
    std::printf ("  FL synth presets: %d files, %d read\n", presetFiles, presetsRead);
    CHECK (presetsRead >= presetFiles * 95 / 100);
    CHECK (gmsDecoded == gmsPresets);
    CHECK (sytrusDecoded == sytrus);
    CHECK (harmlessDecoded == harmless && harmless > 100);
    std::printf ("  Harmless: %d states parsed for the engine, %d channels imported as Harmless\n", harmlessParsed, harmlessKind);
    CHECK (harmlessParsed == harmless && harmlessKind >= harmless);
    std::printf ("  Harmor: %d channels, %d states usable by the engine, %d imported as Harmor (%d sounding)\n", harmor, harmorUsable, harmorKind, harmorSounding);
    CHECK (harmorUsable == harmor && harmor > 90 && harmorKind >= harmor && harmorSounding >= harmorKind * 9 / 10);
    std::printf ("  GMS: %d channels, %d usable, %d imported as GMS (%d sounding); Morphine: %d channels, %d usable, %d imported (%d sounding); %d demo goldens\n",
                 gms, gmsUsable, gmsKind, gmsSounding, morphine, morphineUsable, morphineKind, morphineSounding, demoGoldens);
    CHECK (gmsUsable == gms && gms >= 9 && gmsKind == gms && gmsSounding == gmsKind);
    std::printf ("  MiniSynth channels: %d, %d usable, %d on the exact engine, %d sounding, %d golden\n", miniSynth, miniSynthUsable, miniSynthKind,
                 miniSynthSounding, miniSynthGoldens);
    CHECK (miniSynthUsable == miniSynth && miniSynth >= 12 && miniSynthKind == miniSynth && miniSynthSounding == miniSynthKind && miniSynthGoldens == 2);
    CHECK (morphineUsable == morphine && morphine >= 7 && morphineKind == morphine && morphineSounding == morphineKind && demoGoldens == 2);
    std::printf ("  PoiZone: %d channels, %d usable, %d imported as PoiZone (%d sounding), %d demo golden\n", poizone, poizoneUsable, poizoneKind,
                 poizoneSounding, poizoneGoldens);
    CHECK (poizoneUsable == poizone && poizone >= 2 && poizoneKind == poizone && poizoneSounding == poizoneKind && poizoneGoldens == 1);
    std::printf ("  Toxic Biohazard: %d channels, %d usable, %d imported as Toxic (%d sounding), %d golden\n", toxic, toxicUsable, toxicKind,
                 toxicSounding, toxicGoldens);
    CHECK (toxicUsable == toxic && toxic >= 3 && toxicKind == toxic && toxicSounding == toxicKind && toxicGoldens == 1);
    std::printf ("  Ogun: %d channels, %d states usable by the engine, %d imported as Ogun (%d sounding)\n", ogun, ogunUsable, ogunKind, ogunSounding);
    CHECK (ogunUsable == ogun && ogun >= 20 && ogunKind >= ogun && ogunSounding >= ogunKind * 9 / 10);
    std::printf ("  Autogun: %d channels, %d states usable by the engine, %d imported as Autogun (%d with SG.dat, %d sounding)\n", autogun,
                 autogunUsable, autogunKind, autogunWithSg, autogunSounding);
    CHECK (autogunUsable == autogun && autogun >= 9 && autogunKind == autogun && autogunWithSg == autogun && autogunSounding == autogun);
    std::printf ("  Fruity DX10: %d channels, %d states usable by the engine, %d imported as DX10 (%d sounding)\n", dx10, dx10Usable, dx10Kind, dx10Sounding);
    CHECK (dx10Usable == dx10 && dx10 >= 12 && dx10Kind == dx10 && dx10Sounding == dx10Kind);
    std::printf ("  Kepler: %d channels, %d states usable by the engine, %d imported as Kepler (%d sounding)\n", kepler, keplerUsable, keplerKind, keplerSounding);
    CHECK (keplerUsable == kepler && kepler >= 4 && keplerSounding == keplerKind && (keplerKind == 0 || keplerKind == kepler));
    std::printf ("  Harmless rack compressor: %d of %d presets read from the installation\n", harmlessCompLoaded, harmlessCompWanted);
    CHECK (harmlessCompLoaded == harmlessCompWanted && harmlessCompWanted > 100);
    std::printf ("  Maximus: %d of %d slots on the exact engine; Soundgoodizer: %d of %d; Vocodex with SG.dat: %d of %d\n", maximusExact,
                 maximusSlots, sgzExact, sgzSlots, vocodexWithData, vocodexSlots);
    // REA_rodada40 §6: in Demo projects the 4 states left on the approximation use the Frequency Splitter linear-phase
    // option; the other Data folders add states with options not decoded
    CHECK (maximusSlots - maximusExact <= 16 && maximusExact >= 97 && sgzExact == sgzSlots && sgzSlots > 90);
    CHECK (vocodexWithData == vocodexSlots && vocodexSlots >= 10);
    std::printf ("  Fruity WaveShaper: %d of %d slots on the exact engine; Fruity Soft Clipper: %d of %d\n", shaperExact, shaperSlots,
                 clipperExact, clipperSlots);
    // (Patcher graphs add WaveShaper nodes of their own to the imported mixer)
    CHECK (shaperExact >= shaperSlots && shaperSlots >= 70 && clipperExact >= clipperSlots && clipperSlots >= 25); // REA_rodada51
    std::printf ("  Fruity Parametric EQ 2 / Parametric EQ: %d of %d slots on the exact engine\n", flPeqExact, flPeqSlots);
    CHECK (flPeqExact >= flPeqSlots && flPeqSlots >= 1100); // REA_rodada50
    std::printf ("  Fruity PanOMatic / Balance / Blood Overdrive: %d of %d slots on the exact engines\n", basicExact, basicSlots);
    // (since round 141 a Patcher stays one slot: "Bombs Away" insert 69 keeps its last PanOMatic)
    CHECK (basicExact >= basicSlots && basicSlots >= 270); // REA_rodada58
    // (Patcher graphs can add Distructor nodes of their own to the imported mixer)
    std::printf ("  Distructor: %d of %d slots on the exact engine (%d cabinet impulses read from the installation)\n",
                 distructorExact, distructorSlots, distructorImpulses);
    CHECK (distructorExact >= distructorSlots && distructorSlots >= 4 && distructorImpulses >= 1); // REA_rodada71
    std::printf ("  EQUO: %d of %d slots on the exact engine\n", equoExact, equoSlots);
    CHECK (equoExact == equoSlots && equoSlots >= 10); // REA_rodada61
    std::printf ("  Pitch Shifter: %d of %d slots on the exact engine\n", pitchShifterExact, pitchShifterSlots);
    CHECK (pitchShifterExact == pitchShifterSlots && pitchShifterSlots >= 15); // REA_rodada61
    std::printf ("  Fruity Fast LP / Free Filter / Filter: %d of %d slots on the exact engines\n", filterExact, filterSlots);
    CHECK (filterExact >= filterSlots && filterSlots >= 53); // REA_rodada63
    std::printf ("  Fruity Delay 2: %d of %d slots on the exact engine; Fruity Delay 3: %d of %d\n", delay2Exact, delay2Slots,
                 delay3Exact, delay3Slots);
    // (FL 11 files' slots are read since round 80; Patcher graphs can add Delay 2 nodes of their own)
    CHECK (delay2Exact >= delay2Slots && delay2Slots >= 60 && delay3Exact >= delay3Slots && delay3Slots >= 100); // REA_rodada52
    std::printf ("  Chorus family: %d of %d slots on the exact engines\n", chorusExact, chorusSlots);
    CHECK (chorusExact >= chorusSlots && chorusSlots >= 47); // REA_rodada59
    std::printf ("  Fruity Fast Dist: %d of %d slots on the exact engine; Fruity Flangus: %d of %d; Fruity Flanger: %d of %d\n", fastDistExact,
                 fastDistSlots, flangusExact, flangusSlots, flangerExact, flangerSlots);
    CHECK (fastDistExact >= fastDistSlots && fastDistSlots >= 30 && flangusExact >= flangusSlots && flangusSlots >= 30
           && flangerExact >= flangerSlots && flangerSlots >= 3); // REA_rodada55
    std::printf ("  Fruity Phaser: %d of %d slots on the exact engine\n", phaserExact, phaserSlots);
    CHECK (phaserExact >= phaserSlots && phaserSlots >= 3); // REA_rodada105_phaser

    // 3x Osc channel presets shipped with FL (.fst): all become synth settings
    int presets = 0, presetsDecoded = 0;
    const auto presetDir = std::filesystem::path (realFlpDir) / "Patches" / "Channel presets" / "3x Osc";
    if (std::filesystem::is_directory (presetDir))
        for (auto& entry : std::filesystem::directory_iterator (presetDir))
            if (entry.path().extension() == ".fst")
            {
                ++presets;
                std::ifstream in (entry.path(), std::ios::binary);
                std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
                if (auto synth = lynx::synthFromFlPreset (bytes))
                {
                    ++presetsDecoded;
                    CHECK (synth->osc[0].level + synth->osc[1].level + (synth->am ? 0.0f : synth->osc[2].level) <= 1.001f);
                }
            }
    std::printf ("  %d 3x Osc presets, %d read\n", presets, presetsDecoded);
    CHECK (presetsDecoded == presets);

    // FL effect presets that become built-in effects
    int fxPresets = 0, fxDecoded = 0;
    for (auto kind : { lynx::EffectKind::parametricEq, lynx::EffectKind::limiter, lynx::EffectKind::reverb, lynx::EffectKind::delay,
                       lynx::EffectKind::utility })
        for (const auto& plugin : lynx::flPluginsForEffect (kind))
        {
            const auto dir = std::filesystem::path (realFlpDir) / "Patches" / "Plugin presets" / "Effects" / plugin;
            if (! std::filesystem::is_directory (dir))
                continue;
            for (auto& entry : std::filesystem::recursive_directory_iterator (dir))
                if (entry.path().extension() == ".fst")
                {
                    ++fxPresets;
                    std::ifstream in (entry.path(), std::ios::binary);
                    std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
                    fxDecoded += lynx::effectFromFlPreset (bytes).has_value();
                }
        }
    std::printf ("  %d FL effect presets, %d read\n", fxPresets, fxDecoded);
    CHECK (fxDecoded == fxPresets);
    CHECK (wrappedParsed == wrapped);
    CHECK (wrappedVst2State == wrappedVst2 && wrappedVst2 >= 23 && wrappedVst2Old <= 13);
    std::printf ("  %d plugins in Fruity Wrapper, %d read; %d VST2 states, %d decoded (%d not written back byte-identical)\n", wrapped,
                 wrappedParsed, wrappedVst2, wrappedVst2State, wrappedVst2Old);
    std::printf ("  %d markers, %d time signature changes, %d tracks with EQ\n", markers, signatures, trackEqs);
    std::printf ("  %d parametric EQs (%d with FL settings), %d reverbs (%d without dry)\n", peqSlots, peqShaped, reverbs, reverbsDryless);
    std::printf ("  DirectWave: %d channels, %d zones; FPC: %d projects, %d pad channels, %d pad notes\n", dwChannels, dwZones, fpcKits,
                 padChannels, padNotes);
    CHECK (dwZones > 1000 && padChannels > 250 && padNotes > 500);
    std::printf ("  DirectWave on the exact engine (REA_rodada72): %d channels\n", dwExact);
    CHECK (dwExact >= 57);
    std::printf ("  sliced channels (Slicer/Slicex): %d; embedded WAVs saved: %d\n", slicedChannels, embeddedSaved);
    CHECK (slicedChannels >= 25 && embeddedSaved >= 10); // without decoding Ogg here, some loops have no duration
    std::printf ("  FL Keys (Grand Piano, 30 zones): %d channels\n", flKeysChannels);
    CHECK (flKeysChannels >= 8);
    CHECK (peqSlots > 1000 && peqShaped > 900 && reverbsDryless > 20 && reverbsLowCut > 250);
    CHECK (markers > 300 && signatures > 60);
    std::printf ("  %d 3x Osc channels, %d decoded, %d on the exact engine (%d sounding)\n", threeOsc, threeOscDecoded, threeOscKind, threeOscSounding);
    // one demo channel (a noise layer of "Right Night feeling") has the channel filter closed (fast LP, Mod X 0): silent
    // with its saved settings, in FL too
    CHECK (threeOscKind == threeOsc && threeOscSounding == threeOscKind - 1);
    std::printf ("  %d .flp files read, %d with notes, %d with playlist, %d automations (%d with target), %d sends\n",
                 files, withNotes, withPlaylist, automations, automationsWithTarget, sends);
    CHECK (files > 0);
    CHECK (automations > 0 && sends > 0);
}

// ---------------------------------------------------------------------------
// Sequencer

TEST (pattern_toggle_step_and_length)
{
    lynx::Pattern p;
    CHECK (! p.hasStep (0, 4));
    p.toggleStep (0, 4);
    CHECK (p.hasStep (0, 4));
    CHECK (p.notes.size() == 1 && p.notes[0].start == 4 * lynx::ticksPerStep);
    CHECK (p.effectiveLength (4) == 4 * lynx::ppq); // one bar
    p.toggleStep (1, 20);                            // second bar
    CHECK (p.effectiveLength (4) == 8 * lynx::ppq);
    p.toggleStep (0, 4);
    CHECK (! p.hasStep (0, 4) && p.notes.size() == 1);
}

TEST (scheduler_places_events_at_exact_samples)
{
    lynx::Pattern p;
    p.toggleStep (0, 0);
    p.toggleStep (0, 4); // tick 960 = 1 beat

    const double sr = 48000.0, bpm = 120.0; // 1 beat = 0.5 s = 24000 samples
    const auto tps = lynx::ticksPerSample (bpm, sr);
    lynx::EventBuffer ev;

    lynx::schedulePatternLoop (p, p.effectiveLength (4), 23900.0 * tps, 512, tps, ev);
    // the beat-2 note starts at sample 24000 -> offset 100 in this block
    int ons = 0;
    for (auto& e : ev)
        if (e.type == lynx::NoteEvent::Type::on)
        {
            ++ons;
            CHECK (e.sampleOffset == 100);
        }
    CHECK (ons == 1);
}

TEST (scheduler_wraps_the_loop)
{
    lynx::Pattern p;
    p.toggleStep (2, 0);
    const auto len = p.effectiveLength (4); // 3840 ticks
    const double sr = 48000.0;
    const auto tps = lynx::ticksPerSample (120.0, sr);
    lynx::EventBuffer ev;

    // block that crosses the loop end: starts 10 samples before
    const auto start = (double) len - 10.0 * tps;
    lynx::schedulePatternLoop (p, len, start, 64, tps, ev);

    bool found = false;
    for (auto& e : ev)
        if (e.type == lynx::NoteEvent::Type::on && e.channel == 2)
        {
            found = true;
            CHECK (e.sampleOffset == 10);
        }
    CHECK (found);
}

TEST (scheduler_off_before_on_at_same_sample)
{
    lynx::Pattern p;
    lynx::Note a;
    a.start = 0; a.length = 240; a.channel = 0;
    lynx::Note b = a;
    b.start = 240;
    p.notes = { a, b };
    const auto tps = lynx::ticksPerSample (120.0, 48000.0);
    lynx::EventBuffer ev;
    lynx::schedulePatternLoop (p, p.effectiveLength (4), 200.0, 4000, tps, ev);

    CHECK (ev.size() >= 2);
    CHECK (ev[0].type == lynx::NoteEvent::Type::off);
    CHECK (ev[1].type == lynx::NoteEvent::Type::on);
    CHECK (ev[0].sampleOffset == ev[1].sampleOffset);
}

TEST (snapshot_exchange_hands_over_and_recycles)
{
    struct Counted
    {
        int value;
        int* alive;
        Counted (int v, int* a) : value (v), alive (a) { ++*alive; }
        ~Counted() { --*alive; }
    };

    int alive = 0;
    {
        lynx::SnapshotExchange<Counted> x;
        CHECK (x.acquire() == nullptr);

        x.publish (std::make_unique<Counted> (1, &alive));
        CHECK (x.acquire()->value == 1);

        x.publish (std::make_unique<Counted> (2, &alive));
        x.publish (std::make_unique<Counted> (3, &alive)); // 2 was never read: freed immediately
        CHECK (alive == 2);
        CHECK (x.acquire()->value == 3);
        CHECK (alive == 2); // 1 was returned, but only the UI frees it
        x.collectGarbage();
        CHECK (alive == 1);

        // without collection, the return slots fill up and the audio side holds on to the current one
        for (int i = 0; i < 20; ++i)
        {
            x.publish (std::make_unique<Counted> (10 + i, &alive));
            CHECK (x.acquire() != nullptr);
        }
    }
    CHECK (alive == 0);
}

TEST (rack_renderer_makes_sound_and_reuses_instruments)
{
    auto song = lynx::Song::makeDefault();
    song.bpm = 120.0;
    const double sr = 48000.0;
    auto state = lynx::RackState::build (song, nullptr, sr);

    lynx::RackRenderer r;
    r.prepare (sr);
    std::vector<float> l (512), rr (512);

    double tick = 0.0;
    float peak = 0.0f;
    int kicks = 0;

    for (int block = 0; block < 93; ++block) // 47616 samples: just before 1 s
    {
        r.process (state.get(), true, tick, 512, l.data(), rr.data());
        for (auto& e : r.lastEvents())
            if (e.type == lynx::NoteEvent::Type::on && e.channel == 0)
                ++kicks;
        for (int i = 0; i < 512; ++i)
        {
            CHECK (std::isfinite (l[(std::size_t) i]));
            peak = std::max (peak, std::abs (l[(std::size_t) i]));
        }
        tick += 512 * lynx::ticksPerSample (song.bpm, sr);
    }

    CHECK (peak > 0.1f && peak < 4.0f);
    CHECK (kicks == 2); // 120 BPM: beats at 0 and 0.5 s

    // editing the song keeps the instruments of channels whose type didn't change
    song.channels[4].instrument = lynx::InstrumentKind::kick;
    auto next = lynx::RackState::build (song, state.get(), sr);
    CHECK (next->instruments[0] == state->instruments[0]);
    CHECK (next->instruments[4] != state->instruments[4]);

    // stopping silences the synth and triggers no new notes
    r.process (next.get(), false, tick, 512, l.data(), rr.data());
    CHECK (r.lastEvents().size() == 0);
}

TEST (flp_import_maps_channels_and_rescales)
{
    flp::Project p;
    p.header.ppq = 96;
    p.tempo = 150.0;
    p.channels = { { 0, flp::ChannelType::sampler, "Kick 808", "", "", 1 },
                   { 1, flp::ChannelType::audioClip, "Vocal", "", "", 2 },
                   { 2, flp::ChannelType::instrument, "Lead", "FLEX", "", 3 } };
    flp::Pattern pat;
    pat.index = 1;
    flp::Note n;
    n.position = 96;
    n.length = 0;
    n.channel = 2;
    n.key = 72;
    n.velocity = 64;
    pat.notes.push_back (n);
    p.patterns.push_back (pat);

    const auto s = lynx::importFlp (p);
    CHECK (s.bpm == 150.0);
    CHECK (s.channels.size() == 3); // audio clip channels stay in the rack, as in FL
    CHECK (s.channels[0].instrument == lynx::InstrumentKind::kick);
    CHECK (s.channels[1].instrument == lynx::InstrumentKind::audioClip);
    CHECK (s.channels[2].instrument == lynx::InstrumentKind::synth);
    CHECK (s.patterns.size() == 1);
    CHECK (s.patterns[0].notes[0].start == lynx::ppq);           // 96 @ PPQ 96 = 1 beat
    CHECK (s.patterns[0].notes[0].channel == 2);
    CHECK (s.patterns[0].notes[0].length == lynx::ticksPerStep); // step note
    CHECK (std::abs (s.patterns[0].notes[0].velocity - 0.5f) < 1e-6f);
}

TEST (live_queue_is_fifo_and_bounded)
{
    lynx::LiveEventQueue q;
    lynx::NoteEvent e;
    CHECK (! q.pop (e));

    for (int i = 0; i < 10; ++i)
    {
        lynx::NoteEvent in;
        in.key = i;
        CHECK (q.push (in));
    }

    for (int i = 0; i < 10; ++i)
        CHECK (q.pop (e) && e.key == i);

    int pushed = 0;
    while (q.push (e))
        ++pushed;
    CHECK (pushed == (int) lynx::LiveEventQueue::capacity - 1); // one slot stays free to tell full from empty
}

// Whether this CPU's RCPPS (the ~12-bit reciprocal estimate, whose table is implementation-defined: Intel and AMD
// differ) gives the same bits as the CPU that the FL golden data was captured on (AMD Zen 2): a hash of the estimate for
// every float mantissa in [1, 2) (the estimate does not depend on the exponent in the normal range)
static bool rcpLikeReferenceCpu()
{
#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__) || defined(__aarch64__) || defined(_M_ARM64) // x86 SSE, or ARM64 through core/Simd.h (sse2neon)
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint32_t m = 0; m < (1u << 23); m += 4)
    {
        alignas (16) float f[4];
        for (std::uint32_t j = 0; j < 4; ++j)
        {
            const std::uint32_t bits = 0x3f800000u | (m + j);
            std::memcpy (&f[j], &bits, 4);
        }
        _mm_store_ps (f, _mm_rcp_ps (_mm_load_ps (f)));
        for (std::uint32_t j = 0; j < 4; ++j)
        {
            std::uint32_t bits;
            std::memcpy (&bits, &f[j], 4);
            h = (h ^ bits) * 1099511628211ull;
        }
    }
    return h == 0xb5de670652cb0383ull;
#else
    return false;
#endif
}

// FL's sampler interpolators (REA_rodada69 §1): outputs of FLEngine's own kernels (EXEC, r69/py/gold.py) on a synthetic
// 16-bit stereo signal; the port must give the same bits (linear, 6-point, 24-point sinc, 32-point sinc at speed 1.37,
// 16-point sinc widened to 24 points with the cutoff at π/9.5 at speed 9.5)
// The sinc kernels take 1/x from RCPPS plus one Newton step, as FL does, so FL's own bits depend on the CPU: they are
// required where RCPPS matches the capture machine's; elsewhere (Intel CPUs) within a few float ulps.
TEST (fl_sampler_interpolators_match_fl)
{
    std::vector<float> l, r;
    std::uint32_t x = 12345;
    for (int k = 0; k < 2 * 144; ++k)
    {
        x = x * 1103515245u + 12345u;
        const auto v = (float) (std::int16_t) (std::uint16_t) (x >> 16) / 32768.0f;
        (k % 2 == 0 ? l : r).push_back (v);
    }
    const int pad = 40;
    const std::pair<int, double> positions[] = { { 20, 0.0 }, { 21, 0.25 }, { 22, 0.5 }, { 23, 0.71828 }, { 30, 0.999 }, { 31, 1e-7 }, { 40, 0.3333 }, { 41, 0.875 } };
    struct Case { int quality; double speed; std::uint32_t bits[16]; };
    const Case cases[] = {
        { 1, 1.0, { 0x3ec1a000, 0x3d774000, 0x3ca1e000, 0x3ec39900, 0xbf2d4600, 0xbe096800, 0xbea0cd17, 0x3f433097, 0x3f2796da, 0xbd9fbd17, 0x3f27a3ff, 0xbda03ff2, 0x3dace746, 0x3eba19c4, 0x3e477a00, 0x3f0f2a80 } },
        { 2, 1.0, { 0x3ec1a000, 0x3d773fff, 0x3d34885d, 0x3eedeaa0, 0xbf49e298, 0xbea52fc4, 0xbeba55fc, 0x3f7c58da, 0x3f27c5c2, 0xbda139af, 0x3f27a3ff, 0xbda03ffa, 0x3dd8e7b1, 0x3ee872c1, 0x3e05b2d5, 0x3f144c4f } },
        { 4, 1.0, { 0x3ec19fff, 0x3d773fff, 0x3d889c32, 0x3ef6b184, 0xbf57cf0d, 0xbf0abecd, 0xbea14a13, 0x3f94f11b, 0x3f27edc4, 0xbda10d8c, 0x3f27a3fd, 0xbda03ffa, 0x3e5baf41, 0x3edb298c, 0x3dd76cbf, 0x3ef9d212 } },
        { 5, 1.37, { 0x3f19d373, 0x3ea9b6a3, 0xbdcbe56e, 0x3e2af6c6, 0xbf476f7a, 0xbe566a13, 0xbe9f166a, 0x3f52b5ad, 0x3f27ad16, 0xbce48792, 0x3f27960a, 0xbce01ebe, 0x3e1b96be, 0x3e163fd8, 0x3f042e42, 0x3ec4be83 } },
        { 3, 9.5, { 0xbd97308e, 0x3dd8783c, 0xbd911374, 0x3e162a41, 0xbd8a959e, 0x3e38b220, 0xbd7e6785, 0x3e58895c, 0xbd380c4f, 0x3e7e3008, 0xbd380e4e, 0x3e7e2696, 0x3d00c6d0, 0xbd627d87, 0x3e308cb8, 0xbcece918 } },
    };
    const lynx::flvp::X87Extended precision;
    const bool exactRcp = rcpLikeReferenceCpu();
    for (const auto& c : cases)
    {
        lynx::flrs::Kernel k;
        k.setQuality (c.quality);
        k.setSpeed (c.speed);
        int same = 0;
        for (int p = 0; p < 8; ++p)
        {
            const auto lo = pad + positions[p].first - k.halfWidth + 1;
            float a, b;
            lynx::flrs::interpolate (k, l.data() + lo, r.data() + lo, positions[p].second, a, b);
            std::uint32_t ba, bb;
            std::memcpy (&ba, &a, 4);
            std::memcpy (&bb, &b, 4);
            if (exactRcp || c.quality < 3) // (linear and 6-point use no reciprocal)
                same += (ba == c.bits[2 * p]) + (bb == c.bits[2 * p + 1]);
            else
                same += (std::abs (a - std::bit_cast<float> (c.bits[2 * p])) < 5e-7f) + (std::abs (b - std::bit_cast<float> (c.bits[2 * p + 1])) < 5e-7f);
        }
        CHECK (same == 16);
    }
    CHECK (lynx::flrs::halfWidthFor (4) == 12 && lynx::flrs::halfWidthFor (5) == 16 && lynx::flrs::halfWidthFor (9) == 256);
    CHECK (lynx::flrs::speedFor (0.0f, 1.0) == 1.0 && std::abs (lynx::flrs::speedFor (1200.0f, 0.5) - 1.0) < 1e-12);
}

// The sampler at the original rate copies the sample (FL's copy path), and plays until the read position passes the
// last frame
TEST (fl_sampler_copies_at_original_rate)
{
    auto data = std::make_shared<lynx::SampleData>();
    data->sampleRate = 48000.0;
    for (int i = 0; i < 300; ++i)
        data->left.push_back (std::sin (0.05f * (float) i) * 0.5f);
    auto sampler = lynx::makeInstrument (lynx::InstrumentKind::sampler, data);
    sampler->prepare (48000.0);
    sampler->noteOn (60, 1.0f);
    std::vector<float> l (400), r (400);
    sampler->render (l.data(), r.data(), 400, 1.0f, 1.0f);
    const auto panL = std::cos ((0.0f + 1.0f) * 0.25f * 3.14159265f) * 1.41421356f; // the sampler's centre pan gain
    int exact = 0;
    for (int i = 0; i < 300; ++i)
        exact += l[(std::size_t) i] == data->left[(std::size_t) i] * panL ? 1 : 0;
    CHECK (exact == 300);
    CHECK (l[300] == 0.0f);
}

// FL's volume envelope on the sampler (REA_rodada69 §3): the articulator is stepped once per sequencer tick and the level
// ramps linearly to each tick's value; 96 PPQ at 120 BPM and 48 kHz = 250 samples per tick, attack 20000 = 89 ticks
TEST (fl_sampler_volume_envelope_steps_per_tick)
{
    auto data = std::make_shared<lynx::SampleData>();
    data->sampleRate = 48000.0;
    data->left.assign (96000, 0.5f);
    auto sampler = lynx::makeInstrument (lynx::InstrumentKind::sampler, data);
    sampler->prepare (48000.0);
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::sampler;
    c.sampler.flVolumeEnvelope = { 0, 1, 100, 20000, 0, 30000, 128, 20000, 0, 100, 20000, 0, 32950, 0, 0, 0, 0, 96 };
    sampler->configure (c, 120.0);
    sampler->noteOn (60, 1.0f);
    std::vector<float> l (40000), r (40000);
    sampler->render (l.data(), r.data(), 40000, 1.0f, 1.0f);
    const auto panL = std::cos ((0.0f + 1.0f) * 0.25f * 3.14159265f) * 1.41421356f;
    auto level = [&] (int n) { return l[(std::size_t) n] / (0.5f * panL); };
    CHECK (std::abs (level (1000) - 1000.0f / 22500.0f) < 2e-5f);
    CHECK (std::abs (level (10125) - 10125.0f / 22500.0f) < 2e-5f);
    CHECK (std::abs (level (39000) - 1.0f) < 1e-6f); // the attack ended after 90 ticks; sustain 128 = 1.0
    // release: 20000 = 89 ticks, linear (tension 0), from the current level
    sampler->noteOff (60);
    std::fill (l.begin(), l.end(), 0.0f);
    sampler->render (l.data(), r.data(), 40000, 1.0f, 1.0f);
    CHECK (std::abs (level (2500) - (1.0f - 10.0f / 90.0f)) < 1e-3f);
    CHECK (l[39999] == 0.0f && sampler->idle());
}

// FL's native sampler voice (REA_rodada76): FL's own command-line renders of crafted projects (one sampler channel,
// our noise sample, notes at ticks 0/192/384 of 96 PPQ at 120 BPM, 16-bit output) — the fast LP with resonance and a
// note pan/velocity set; the 12 dB LP with volume, cutoff and pitch envelopes and their releases. FL's int16 codes at
// a few frames; LYNX within one code. Also the velocity law and the pan law with the channel's levels inside the voice.
TEST (fl_native_sampler_voice_matches_fl_renders)
{
    auto noise = std::make_shared<lynx::SampleData>();
    noise->sampleRate = 44100.0;
    {
        std::uint32_t seed = 1;
        for (int i = 0; i < 26460; ++i)
        {
            seed = (seed * 1103515245u + 12345u) & 0x7fffffffu;
            noise->left.push_back ((float) (((double) seed / 2147483647.0 * 2.0 - 1.0) * 0.3));
        }
    }
    struct Ev { int on, off; float vel, pan; };
    auto render = [&] (std::vector<std::int32_t> recs, std::array<int, 3> filter, const std::vector<Ev>& notes,
                       std::shared_ptr<lynx::SampleData> data, float chanGain) {
        lynx::ChannelDef c;
        c.instrument = lynx::InstrumentKind::sampler;
        c.sampler.flChannel = recs;
        c.sampler.flChannel.insert (c.sampler.flChannel.end(), { filter[0], filter[1], filter[2], 96 });
        auto inst = lynx::makeInstrument (lynx::InstrumentKind::sampler, data);
        lynx::setOfflineRendering (true); // FL's render dialog: 32-point sinc (the pitch envelope interpolates)
        inst->prepare (44100.0);
        lynx::setOfflineRendering (false);
        inst->configure (c, 120.0);
        inst->setSongPosition (0.0);
        std::vector<std::pair<int, int>> events; // (sample, index ≥ 0 on / < 0 off)
        for (int i = 0; i < (int) notes.size(); ++i)
            events.push_back ({ notes[(std::size_t) i].on, i }), events.push_back ({ notes[(std::size_t) i].off, -1 - i });
        std::sort (events.begin(), events.end());
        std::vector<float> l (120000), r (120000);
        int at = 0;
        for (const auto& [when, i] : events)
        {
            inst->render (l.data() + at, r.data() + at, when - at, chanGain, chanGain);
            at = when;
            if (i >= 0)
            {
                inst->setNextNoteExpression (notes[(std::size_t) i].pan, 0.0f, false);
                inst->noteOn (60, notes[(std::size_t) i].vel);
            }
            else
                inst->noteOff (60);
        }
        inst->render (l.data() + at, r.data() + at, 120000 - at, chanGain, chanGain);
        return std::make_pair (l, r);
    };
    const std::vector<std::int32_t> plain = { 0, 0, 100, 20000, 20000, 30000, 50, 20000, 0, 100, 20000, 0, 32950, 0, 0, 0, 0 };
    auto records = [&] (std::vector<std::int32_t> vol, std::vector<std::int32_t> cut, std::vector<std::int32_t> pitch) {
        std::vector<std::int32_t> all = plain;
        for (const auto* r : std::initializer_list<const std::vector<std::int32_t>*> { &vol, &cut, &plain, &pitch })
            all.insert (all.end(), r->begin(), r->end());
        return all;
    };
    const auto volPlain = std::vector<std::int32_t> { 4, 0, 100, 20000, 20000, 30000, 50, 20000, 0, 100, 20000, 0, 32950, 0, 0, 0, -101 };
    const auto chan = lynx::channelGain (10000.0f / 12800.0f); // the rack's gain at pan 0 (panGains: gain·√2·cos(π/4))
    const int at[] = { 5, 300, 2000, 9000, 11500, 15000, 22100, 26000 };
    auto check = [&] (const std::pair<std::vector<float>, std::vector<float>>& out, const int* goldL, const int* goldR) {
        int bad = 0;
        for (int n = 0; n < 3; ++n)
            for (int k = 0; k < 8; ++k)
            {
                const auto p = (std::size_t) (44100 * n + at[k]);
                bad += std::abs ((int) std::lround (out.first[p] * 32767.0f) - goldL[8 * n + k]) > 1;
                bad += std::abs ((int) std::lround (out.second[p] * 32767.0f) - goldR[8 * n + k]) > 1;
            }
        return bad;
    };
    {
        // fast LP (Mod X 100, Mod Y 200); notes: len 96, (vel 80, pan 40) len 48, (vel 120, pan 90) len 144
        const auto out = render (records (volPlain, plain, plain), { 100, 200, 0 },
                                 { { 0, 22050, 100.0f / 128, 0.0f }, { 44100, 55125, 80.0f / 128, -24.0f / 64 }, { 88200, 121275, 120.0f / 128, 26.0f / 64 } },
                                 noise, chan);
        const int L[] = { -194, -789, 883, 803, 614, -557, -983, -1641, -141, -572, 640, 582, 445, -403, -712, -1189, -207, -841, 942, 856, 655, -593, -1048, -1750 };
        const int R[] = { -194, -789, 883, 803, 614, -557, -983, -1641, -75, -305, 342, 311, 238, -216, -381, -636, -411, -1671, 1871, 1700, 1300, -1179, -2082, -3477 };
        CHECK (check (out, L, R) == 0);
    }
    {
        // 12 dB LP (Mod X 90, Mod Y 60), volume / cutoff / pitch envelopes; notes len 48, (vel 80, pan 30) len 48, len 30
        const auto out = render (records ({ 4, 1, 100, 3000, 20000, 30000, 100, 6000, 0, 100, 20000, 0, 32950, 0, 0, 0, -101 },
                                          { 0, 1, 100, 6000, 2000, 15000, 40, 8000, 100, 100, 20000, 0, 32950, 0, 0, 0, 0 },
                                          { 0, 1, 100, 4000, 20000, 9000, 0, 20000, 600, 100, 20000, 0, 32950, 0, 0, 0, 0 }),
                                 { 90, 60, 1 },
                                 { { 0, 11025, 100.0f / 128, 0.0f }, { 44100, 55125, 80.0f / 128, -34.0f / 64 }, { 88200, 95091, 100.0f / 128, 0.0f } },
                                 noise, chan);
        const int L[] = { -1, -203, 29, -298, 97, 0, 0, 0, 0, -156, 22, -228, 74, 0, 0, 0, -1, -203, 29, -2, 0, 0, 0, 0 };
        const int R[] = { -1, -203, 29, -298, 97, 0, 0, 0, 0, -60, 9, -88, 29, 0, 0, 0, -1, -203, 29, -2, 0, 0, 0, 0 };
        CHECK (check (out, L, R) == 0);
    }
    {
        // a global volume LFO (flag 0x20, amount 80, speed 24000): one phase for the channel, (k + 1)·increment at song
        // tick k (notes on ticks 0, 100, 300)
        auto vol = volPlain;
        vol[0] = 4 | 0x20, vol[11] = 80, vol[12] = 24000;
        const auto out = render (records (vol, plain, plain), { 256, 0, 0 },
                                 { { 0, 22050, 100.0f / 128, 0.0f }, { 22969, 45019, 80.0f / 128, 0.0f }, { 68906, 90956, 100.0f / 128, 0.0f } },
                                 noise, chan);
        const int pos[] = { 500, 3000, 7000, 12000, 23469, 26000, 30000, 69406, 72000, 80000 };
        const int gold[] = { 1149, 765, 374, 4530, 846, -911, -646, 1160, 2316, 727 };
        int bad = 0;
        for (int k = 0; k < 10; ++k)
            bad += std::abs ((int) std::lround (out.first[(std::size_t) pos[k]] * 32767.0f) - gold[k]) > 1;
        CHECK (bad == 0);
    }
    {
        // a sample with its own loop (WAV smpl 4410…8819): FL loops it while the note lasts and fades it out over 10 ms
        // after the note-off (no volume envelope)
        auto looped = std::make_shared<lynx::SampleData>();
        looped->sampleRate = 44100.0;
        for (int i = 0; i < 8820; ++i)
            looped->left.push_back ((float) ((0.5 * (1.0 - 0.5 * i / 8820.0)) * std::sin (2.0 * 3.141592653589793 * 441.0 * i / 44100.0)));
        looped->loopStart = 4410, looped->loopEnd = 8820;
        const auto out = render (records (volPlain, plain, plain), { 256, 0, 0 },
                                 { { 0, 22050, 100.0f / 128, 0.0f }, { 44100, 49612, 100.0f / 128, 0.0f } }, looped, chan);
        const int pos[] = { 125, 4525, 8925, 13337, 17725, 22025, 22125, 22325, 22437, 49625, 49737, 50025 };
        const int gold[] = { 6338, 4746, 3842, 4161, -1470, -989, -3577, -1174, -32, 4373, 2573, 42 };
        int bad = 0;
        for (int k = 0; k < 12; ++k)
            bad += std::abs ((int) std::lround (out.first[(std::size_t) pos[k]] * 32767.0f) - gold[k]) > 1;
        CHECK (bad == 0);
    }
    {
        // a constant sample: velocity law (FL: vel 100 → 0.389608, 128 → 0.799753, 64 → 0.141805 at the default
        // channel volume, pan centre) and a hard-left note (0.550997 / 0)
        auto dc = std::make_shared<lynx::SampleData>();
        dc->sampleRate = 44100.0;
        dc->left.assign (20000, 0.5f);
        auto level = [&] (float vel, float pan, bool right) {
            const auto out = render (records (volPlain, plain, plain), { 256, 0, 0 }, { { 0, 10000, vel, pan } }, dc, chan);
            return (right ? out.second : out.first)[5000] / 0.5f;
        };
        CHECK (std::abs (level (100.0f / 128, 0.0f, false) - 0.389608f) < 3e-5f);
        CHECK (std::abs (level (1.0f, 0.0f, false) - 0.799753f) < 3e-5f);
        CHECK (std::abs (level (64.0f / 128, 0.0f, false) - 0.141805f) < 3e-5f);
        CHECK (std::abs (level (100.0f / 128, -1.0f, false) - 0.550997f) < 3e-5f);
        CHECK (std::abs (level (100.0f / 128, -1.0f, true)) < 1e-6f);
    }
}

// FPC's "cycle" layers (REA_rodada69 §4): among the layers covering the velocity, in order, the one after the last
// chosen, wrapping; FL's velocity index is round(v·128). Slicex's master volume uses FL's L11 law (§5).
TEST (fl_fpc_cycle_layers_and_slicex_master_volume)
{
    auto level = [] (float v) {
        auto d = std::make_shared<lynx::SampleData>();
        d->sampleRate = 48000.0;
        d->left.assign (4800, v);
        return d;
    };
    // A covers 0…63, B 32…95, C 64…127 (all keys)
    auto inst = lynx::makeZonedSampler ({ { { "a.wav", 60, 0, 127, 0, 63 }, level (0.1f) },
                                          { { "b.wav", 60, 0, 127, 32, 95 }, level (0.2f) },
                                          { { "c.wav", 60, 0, 127, 64, 127 }, level (0.3f) } });
    inst->prepare (48000.0);
    inst->configure (lynx::ChannelDef {}, 120.0);
    auto play = [&] (float velocity) {
        inst->allNotesOff();
        std::vector<float> l (2000), r (2000);
        inst->render (l.data(), r.data(), 2000, 1.0f, 1.0f);
        std::fill (l.begin(), l.end(), 0.0f);
        inst->noteOn (60, velocity);
        inst->render (l.data(), r.data(), 10, 1.0f, 1.0f);
        return l[5] / velocity;
    };
    auto which = [] (float x) { return (int) std::lround (x / 0.1f / std::cos (0.25f * 3.14159265f) / 1.41421356f); };
    CHECK (which (play (64.0f / 128.0f)) == 2); // candidates B, C: first B
    CHECK (which (play (64.0f / 128.0f)) == 3); // then C
    CHECK (which (play (40.0f / 128.0f)) == 1); // candidates A, B: C is not among them → the first, A
    CHECK (which (play (40.0f / 128.0f)) == 2); // then B
    CHECK (which (play (64.0f / 128.0f)) == 3); // B was last → C

    // Slicex state: version, 8 master values (volume 16000), options and UI state, deck A referencing a file
    std::vector<std::uint8_t> state (4 + 32 + 47 + 12, 0);
    const std::int32_t version = 0x00060003, volume = 16000;
    std::memcpy (state.data(), &version, 4);
    std::memcpy (state.data() + 4 + 8, &volume, 4);
    state.push_back (1);
    state.push_back (5);
    for (char ch : std::string ("x.wav"))
        state.push_back ((std::uint8_t) ch);
    const auto sliced = lynx::slicex (state);
    CHECK (sliced.has_value() && sliced->path == "x.wav");
    CHECK (sliced && std::abs (sliced->gain - (float) ((std::pow (11.0, 1.25) - 1.0) / 10.0)) < 1e-5f);
}

TEST (fl_keys_state_wti_and_loader)
{
    using namespace lynx::flkeys;
    // state: u32 1, the wavetable name, 15 int32 knobs
    State st;
    st.wavetable = "Grand Piano";
    for (int i = 0; i < 15; ++i)
        st.knobs[(std::size_t) i] = 10 * i - 50;
    const auto bytes = writeState (st);
    CHECK (bytes.size() == 4 + 1 + 11 + 60);
    const auto back = parseState (bytes);
    CHECK (back && back->wavetable == "Grand Piano" && back->knobs == st.knobs);

    // one WAV per keygroup: the file's own loop gives the default end and loop; a keygroup without a file reuses
    // the previous one; a repeated file name copies the earlier keygroup; each file is followed by a guard frame
    // that repeats the frame `loop` before the end (REA_rodada69 §8.2)
    std::vector<std::string> names;
    auto riff = parseWti ("[info]\nsamples=3\nfiletype=riff\npath=W\nvelocityfactor=2\nsamplerate=22050\n"
                          "[sample0]\nname=a.wav\nroot=40\nhigh=50\n[sample1]\nroot=51\nhigh=999\npos=2\nend=5\nloop=3\n"
                          "[sample2]\nname=A.WAV\nroot=60\nhigh=70\n", names);
    CHECK (riff && names.size() == 1 && names[0] == "W\\a.wav");
    CHECK (riff && riff->keygroups.size() == 3 && riff->keygroups[2].duplicateOf == 0 && riff->thresholds == std::vector<int> { 0 });
    if (riff)
    {
        riff->files[0].loopStart = 4, riff->files[0].loopEnd = 8;
        FileSamples a;
        for (int i = 0; i < 10; ++i)
            a.samples.push_back ((std::int16_t) (100 * (i + 1)));
        const auto decoded = FlKeysTable::decode (riff->encode());
        CHECK (decoded.has_value());
        const auto w = buildWavetable (*decoded, { a });
        CHECK (w != nullptr);
        if (w != nullptr)
        {
            auto same = [] (const Keygroup& k, Keygroup e) { return k.root == e.root && k.high == e.high && k.pos == e.pos && k.end == e.end && k.loop == e.loop; };
            CHECK (same (w->keygroups[0], { 40, 50, 0, 8, 5 }));
            CHECK (same (w->keygroups[1], { 51, 999, 2, 5, 3 }));
            CHECK (same (w->keygroups[2], { 60, 70, 0, 8, 5 }));
            CHECK (w->data[9] == 400 && w->data[10] == 0 && w->data[8] == 900);
            CHECK (w->sampleRate == 22050.0f && w->velocityFactor == 2.0f && ! w->stereo);
        }
    }

    // one raw file: every keygroup's part is copied one after another with a guard sample; "canloop=false" makes the
    // loop as long as the keygroup's end, so the voice stops there
    auto raw = parseWti ("[info]\nsamples=2\nsamplefile=x.dat\nfiletype=raw\n[sample0]\nroot=36\nhigh=47\npos=0\nend=3\nloop=2\n"
                         "[sample1]\nroot=48\nhigh=999\npos=4\nend=7\nloop=4\ncanloop=false\n", names);
    CHECK (raw && raw->raw && names.size() == 1 && names[0] == "x.dat");
    if (raw)
    {
        FileSamples x;
        for (int i = 0; i < 10; ++i)
            x.samples.push_back ((std::int16_t) (10 * (i + 1)));
        const auto w = buildWavetable (*raw, { x });
        CHECK (w != nullptr);
        if (w != nullptr)
        {
            CHECK (w->keygroups[0].pos == 0 && w->keygroups[0].end == 3 && w->keygroups[0].loop == 2);
            CHECK (w->keygroups[1].pos == 5 && w->keygroups[1].end == 8 && w->keygroups[1].loop == 8);
            const std::vector<std::int16_t> expected { 10, 20, 30, 40, 30, 50, 60, 70, 80, 50, 0, 0 };
            CHECK (std::equal (expected.begin(), expected.end(), w->data.begin()));

            // a held note on the non-looping keygroup ends by itself; 33 notes leave 32 sounding (FL's MaxPoly)
            State s2;
            s2.knobs = { -1000, 0, 0, -1000, 500, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
            RefNote n;
            n.key = 60;
            const auto out = renderReference (*w, s2, { n }, 44100.0, 4096, 512, true);
            double early = 0, late = 0;
            for (int i = 0; i < 64; ++i)
                early += std::fabs (out[(std::size_t) (2 * i)]);
            for (int i = 2048; i < 4096; ++i)
                late += std::fabs (out[(std::size_t) (2 * i)]);
            CHECK (early > 0.0 && late == 0.0);
        }
    }

    // instrument front-end without a wavetable: silent and idle
    auto inst = makeFlKeysInstrument (nullptr);
    inst->prepare (44100.0);
    inst->noteOn (60, 0.78f);
    std::vector<float> l (256), r (256);
    inst->render (l.data(), r.data(), 256, 1.0f, 1.0f);
    CHECK (inst->idle() && l[100] == 0.0f);
}


TEST (ogg_vorbis_decodes_like_fl)
{
    using namespace lynx::oggvorbis;
    const std::uint8_t wavHeader[] = { 'R', 'I', 'F', 'F', 30, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 2, 0, 0, 0, 0x4F, 0x67 };
    CHECK (isOggWav (wavHeader, sizeof wavHeader));
    CHECK (! isOggStream (wavHeader, sizeof wavHeader));
    Decoded none;
    CHECK (! decode (wavHeader, sizeof wavHeader, none) && none.channels == 0);

    // FNV-1a 64 of the planar float32 samples FL's oggio_x64.dll decodes from installation files (REA_rodada77 §3):
    // a plain file, one with a second (PCM) stream multiplexed in, one where an empty stream's end-of-stream page
    // ends the decode early (FL keeps 17 728 of 20 691 frames), and an FL Keys wavetable
    if (realFlpDir.empty())
        return;
    const auto root = std::filesystem::path (realFlpDir).parent_path();
    const struct { const char* file; std::uint64_t hash; std::size_t frames; int channels; } cases[] = {
        { "Data/Patches/Packs/Drums/Percussion/FPC Wood Hit 2.wav", 0xe6137a6141cd5946ull, 29664, 1 },
        { "Data/Patches/Packs/Instruments/Keyboard/Grand/FL Piano (11).wav", 0x3f60df388ac0c636ull, 264600, 2 },
        { "Data/Patches/Packs/Drums/Percussion/Grv Perc 08.wav", 0x7f235f467e90efe0ull, 17728, 0 },
        { "Plugins/Fruity/Generators/FL Keys/Wavtables/Grand Piano/CHANGPIANOHARD_A8.wav", 0x00bbf4cc42f5f72cull, 153280, 0 },
    };
    for (const auto& c : cases)
    {
        const auto path = root / c.file;
        if (! std::filesystem::exists (path))
            continue;
        std::ifstream in (path, std::ios::binary);
        const std::vector<std::uint8_t> bytes { std::istreambuf_iterator<char> (in), {} };
        CHECK (isOggWav (bytes.data(), bytes.size()));
        Decoded d;
        CHECK (decode (bytes.data(), bytes.size(), d));
        if (d.channels == 0)
            continue;
        std::uint64_t h = 1469598103934665603ULL;
        for (const auto& ch : d.data)
        {
            const auto* p = reinterpret_cast<const std::uint8_t*> (ch.data());
            for (std::size_t i = 0; i < ch.size() * sizeof (float); ++i)
                h = (h ^ p[i]) * 1099511628211ULL;
        }
        CHECK (d.data[0].size() == c.frames && d.sampleRate == 44100);
        CHECK (c.channels == 0 || d.channels == c.channels);
        CHECK (h == c.hash);
    }
}

TEST (directwave_crt_math_and_state)
{
    using namespace lynx::dw;
    auto f = [] (std::uint32_t b) { float x; std::memcpy (&x, &b, 4); return x; };
    auto u = [] (float x) { std::uint32_t b; std::memcpy (&b, &x, 4); return b; };
    // the plugin's own run-time (r72/run/mt.exe on DirectWave_x64.dll): values where the libm result differs
    CHECK (u (sinfCrt (f (0x40b1a97fu))) == 0xbf2af4bbu);
    CHECK (u (sinfCrt (f (0xc0ad8469u))) == 0x3f4222f6u);
    CHECK (u (sinfCrt (f (0xc016e8c7u))) == 0xbf34b32du);
    CHECK (u (cosfCrt (f (0xc0af1cc0u))) == 0x3f305702u);
    CHECK (u (cosfCrt (f (0x3f15cba3u))) == 0x3f556924u);
    CHECK (u (cosfCrt (f (0xc0aeb047u))) == 0x3f2dddfeu);
    CHECK (u (expfCrt (f (0xbfb679c0u))) == 0x3e7622a1u);
    CHECK (encodeFormats ({ true, false }) == std::vector<float> ({ 1.0f, 2.0f, 1.0f, 0.0f }));
    CHECK (decodeFormats ({ 1.0f, 2.0f, 1.0f, 0.0f }) == std::vector<bool> ({ true, false }));
    CHECK (! parseState ({ 1, 2, 3 }).has_value());
}

TEST (directwave_matches_fl_with_demo_programs)
{
    // Demo programs (voices, mod matrix, filters, chorus, reverb, shaper) with our own 16-bit samples in every zone:
    // FNV-1a 64 of DirectWave_x64.dll's output (r72/py/synth.py + cmp.py: 44.1 kHz, host blocks of 512, tempo 120)
    if (realFlpDir.empty())
        return;
    using namespace lynx::dw;
    auto readAll = [] (const std::filesystem::path& p) {
        std::ifstream in (p, std::ios::binary);
        return std::vector<std::uint8_t> { std::istreambuf_iterator<char> (in), {} };
    };
    auto stateOf = [&] (const std::string& file, const std::string& channel) {
        const auto path = std::filesystem::path (realFlpDir) / file;
        if (! std::filesystem::exists (path))
            return std::vector<std::uint8_t>();
        const auto p = flp::parseProject (readAll (path));
        for (const auto& c : p.channels)
            if (c.pluginName == "DirectWave" && c.name == channel)
                return c.pluginState;
        return std::vector<std::uint8_t>();
    };
    auto sampleValue = [] (int z, std::int64_t j) {
        const auto v = (j * (13 + z * 7)) % 4096 - 2048;
        const auto w = ((j * j + z * 31) % 997) - 498;
        return (std::int16_t) std::clamp<std::int64_t> (v * 6 + w * 8, -32768, 32767);
    };
    std::vector<RefNote> notes;
    auto note = [&] (int start, int key, double vel, double fv, double fp, double cents) {
        RefNote n; n.start = start; n.key = key; n.velocity = vel; n.finalVol = fv; n.finalPan = fp; n.cents = cents;
        notes.push_back (n);
    };
    note (0, 48, 0.8, 0.7, -0.3, 0); note (11025, 60, 0.5, 0.9, 0.4, 25); note (22050, 67, 1.0, 0.5, 0.0, 0);
    note (44100, 36, 0.3, 0.8, 0.8, -40); note (80000, 72, 0.9, 0.6, -0.6, 0);
    notes[0].release = 40000; notes[1].release = 70000; notes[2].release = 60000;
    auto run = [&] (const std::string& file, const std::string& channel, std::uint64_t expected) {
        const auto st = parseState (stateOf (file, channel));
        if (! st)
            return;
        const auto* prog = playedProgram (*st);
        CHECK (prog != nullptr);
        if (prog == nullptr)
            return;
        std::vector<std::shared_ptr<const ZoneSamples>> zs;
        for (int z = 0; z < (int) prog->zones.size(); ++z)
        {
            auto s = std::make_shared<ZoneSamples>();
            s->bits = 16;
            s->channels = 1;
            s->frames = prog->zones[(std::size_t) z].frames;
            std::vector<std::int16_t> v ((std::size_t) s->frames + 512, 0);
            for (std::int64_t j = 0; j < s->frames; ++j)
                v[(std::size_t) j + 256] = sampleValue (z, j);
            s->i16.push_back (std::move (v));
            zs.push_back (s);
        }
        EngineOptions o;
        o.velocityAsInitVol = (st->flags & 2) != 0;
        o.noGainClamp = (st->flags & 1) != 0;
        const auto out = renderReference (*prog, zs, o, notes, 44100.0, 120000, 512, 1);
        std::uint64_t h = 14695981039346656037ull;
        for (const float x : out)
        {
            std::uint32_t b;
            std::memcpy (&b, &x, 4);
            for (int k = 0; k < 4; ++k)
                h = (h ^ ((b >> (8 * k)) & 0xffu)) * 1099511628211ull;
        }
        CHECK (h == expected);
        if (h != expected)
            std::printf ("  DirectWave %s: %016llx\n", channel.c_str(), (unsigned long long) h);
    };
    run ("Demo projects/Visual/ZGameEditor Visualizer/Treben - Umziky - All the same.flp", "Gtr", 0xa1de3dd6be53d665ull);
    run ("Demo projects/Visual/ZGameEditor Visualizer/Treben - Umziky - All the same.flp", "Rhodes", 0xcd458ec6a857f9c7ull);
    run ("System/Render tests/Directwave.flp", "DW", 0x95b4c2e29b664dd1ull);
}

TEST (fl_keys_matches_fl_with_the_installation)
{
    // FL Keys' own output (r69/keys/kcmp3.py: the installation's DLL, FL's x87 host pan law, blocks of 512 at
    // 44.1 kHz): FNV-1a 64 of the interleaved float bits of 24 000 frames
    if (realFlpDir.empty())
        return;
    using namespace lynx::flkeys;
    const auto folder = std::filesystem::path (realFlpDir).parent_path() / "Plugins" / "Fruity" / "Generators" / "FL Keys" / "Wavtables";
    if (! std::filesystem::exists (folder / "mda Piano.wti"))
        return;
    auto readAll = [] (const std::filesystem::path& p) {
        std::ifstream f (p, std::ios::binary);
        return std::vector<std::uint8_t> { std::istreambuf_iterator<char> (f), {} };
    };
    auto pcm = [] (const std::vector<std::uint8_t>& w) {
        FileSamples f;
        std::uint16_t channels = 1;
        for (std::size_t pos = 12; pos + 8 <= w.size();)
        {
            std::uint32_t size = 0;
            std::memcpy (&size, w.data() + pos + 4, 4);
            if (std::memcmp (w.data() + pos, "fmt ", 4) == 0)
                std::memcpy (&channels, w.data() + pos + 10, 2);
            else if (std::memcmp (w.data() + pos, "data", 4) == 0)
            {
                f.samples.resize (std::min<std::size_t> (size, w.size() - pos - 8) / 2);
                std::memcpy (f.samples.data(), w.data() + pos + 8, f.samples.size() * 2);
                f.channels = channels;
            }
            pos += 8 + size + (size & 1);
        }
        return f;
    };
    auto run = [&] (const std::string& name, std::array<int, 15> knobs, std::uint64_t expected) {
        const auto wti = readAll (folder / (name + ".wti"));
        std::vector<std::string> names;
        auto table = parseWti (std::string (wti.begin(), wti.end()), names);
        CHECK (table.has_value());
        if (! table)
            return;
        std::vector<FileSamples> files;
        for (std::size_t i = 0; i < names.size(); ++i)
        {
            auto rel = names[i];
            std::replace (rel.begin(), rel.end(), '\\', '/');
            const auto bytes = readAll (folder / rel);
            if (table->raw)
            {
                FileSamples f;
                f.samples.resize (bytes.size() / 2);
                std::memcpy (f.samples.data(), bytes.data(), f.samples.size() * 2);
                files.push_back (std::move (f));
            }
            else
            {
                table->files[i] = wavFileInfo (bytes);
                files.push_back (pcm (bytes));
            }
        }
        const auto w = buildWavetable (*table, files);
        CHECK (w != nullptr);
        if (w == nullptr)
            return;
        State s;
        s.wavetable = name;
        s.knobs = knobs;
        auto note = [] (int start, int key, double iv, double fv, double fp, double cents) {
            RefNote n;
            n.start = start, n.key = key, n.initVol = iv, n.finalVol = fv, n.finalPan = fp, n.cents = cents;
            return n;
        };
        std::vector<RefNote> notes { note (0, 60, 1, 0.78, 0, 0), note (700, 72, 0.6, 0.5, -0.3, 0), note (1500, 30, 0.2, 0.9, 0.5, 25),
                                     note (2100, 100, 1.3, 0.7, 1, -40), note (2500, 48, 0.05, 0.8, -1, 0), note (16000, 96, 0.9, 0.8, 0, 0) };
        // (floats as the harness parses them)
        for (auto& n : notes)
            n.initVol = (float) n.initVol, n.finalVol = (float) n.finalVol, n.finalPan = (float) n.finalPan;
        notes[0].release = 9000, notes[2].release = 12000, notes[3].release = 15000, notes[5].release = 20000;
        const auto out = renderReference (*w, s, notes, 44100.0, 24000, 512, true);
        std::uint64_t h = 0xcbf29ce484222325ull;
        for (auto v : out)
        {
            std::uint32_t b;
            std::memcpy (&b, &v, 4);
            for (int k = 0; k < 4; ++k)
                h = (h ^ ((b >> (8 * k)) & 0xff)) * 0x100000001b3ull;
        }
        CHECK (h == expected);
    };
    run ("mda Piano", { 1000, 138, 388, -1000, 716, 0, -1000, 256, 0, 0, 0, 0, 0, 0, 0 }, 0x78268ec007f31309ull);
    run ("Rhodes", { 200, 300, 500, 800, 300, 1500, 300, 1500, 300, 400, 400, 800, 900, 30000, 700 }, 0xcc91c93d917f1d31ull);
    run ("Roto Organ", { 1000, 1000, 1000, 1000, 0, 0, -1000, 0, 1000, 0, 500, 1000, 1000, 0, 300 }, 0x99eb13e87640a1c6ull);
}

TEST (sampler_plays_pitched_one_shot)
{
    // 1000-frame ramp sample: the position can be measured from the amplitude
    auto data = std::make_shared<lynx::SampleData>();
    data->sampleRate = 48000.0;
    for (int i = 0; i < 1000; ++i)
        data->left.push_back (1.0f);

    auto sampler = lynx::makeInstrument (lynx::InstrumentKind::sampler, data);
    sampler->prepare (48000.0);
    std::vector<float> l (2048, 0.0f), r (2048, 0.0f);

    sampler->noteOn (60, 1.0f);
    sampler->noteOff (60); // one-shot: noteOff doesn't cut
    sampler->render (l.data(), r.data(), 2048, 1.0f, 1.0f);
    int sounding = 0;
    for (auto s : l)
        sounding += s > 0.5f;
    CHECK (sounding >= 995 && sounding <= 1000); // plays the whole sample at the original pitch

    std::fill (l.begin(), l.end(), 0.0f);
    sampler->noteOn (72, 1.0f); // one octave up: half the duration
    sampler->render (l.data(), r.data(), 2048, 1.0f, 1.0f);
    sounding = 0;
    for (auto s : l)
        sounding += s > 0.5f;
    CHECK (sounding >= 495 && sounding <= 500);
    CHECK (r[10] > 0.5f); // mono goes to both sides

    auto silent = lynx::makeInstrument (lynx::InstrumentKind::sampler, nullptr);
    silent->prepare (48000.0);
    silent->noteOn (60, 1.0f);
    std::fill (l.begin(), l.end(), 0.0f);
    silent->render (l.data(), r.data(), 64, 1.0f, 1.0f);
    CHECK (l[0] == 0.0f);
}

TEST (rack_live_notes_and_sample_provider)
{
    lynx::Song song;
    lynx::ChannelDef c;
    c.name = "Smp";
    c.instrument = lynx::InstrumentKind::sampler;
    c.samplePath = "a.wav";
    song.channels.push_back (c);
    song.patterns.emplace_back();

    int loads = 0;
    auto provider = [&] (const std::string& path) -> std::shared_ptr<const lynx::SampleData> {
        ++loads;
        CHECK (path == "a.wav");
        auto d = std::make_shared<lynx::SampleData>();
        d->left.assign (4800, 0.5f);
        return d;
    };

    auto state = lynx::RackState::build (song, nullptr, 48000.0, provider);
    CHECK (loads == 1);

    lynx::RackRenderer renderer;
    renderer.prepare (48000.0);
    lynx::LiveEventQueue live;
    lynx::NoteEvent on;
    on.channel = 0;
    on.velocity = 1.0f;
    live.push (on);

    std::vector<float> l (256), r (256);
    renderer.process (state.get(), false, 0.0, 256, l.data(), r.data(), &live); // stopped, but the live note plays
    CHECK (l[0] > 0.1f);

    // same sample: reused; different sample: reloaded
    auto same = lynx::RackState::build (song, state.get(), 48000.0, provider);
    CHECK (loads == 1 && same->instruments[0] == state->instruments[0]);
    song.channels[0].samplePath = "b.wav";
    auto other = lynx::RackState::build (song, state.get(), 48000.0, [&] (const std::string&) { ++loads; return nullptr; });
    CHECK (loads == 2 && other->instruments[0] != state->instruments[0]);
}

TEST (flp_paths_resolve_and_samplers_import)
{
    lynx::FlPathRoots roots { "C:\\FL", "C:\\FL\\Data", "D:\\User" };
    CHECK (lynx::resolveFlPath ("%FLStudioFactoryData%\\Data\\x.wav", roots) == "C:\\FL\\Data\\x.wav");
    CHECK (lynx::resolveFlPath ("%FLStudioData%\\Patches\\y.wav", roots) == "C:\\FL\\Data\\Patches\\y.wav");
    CHECK (lynx::resolveFlPath ("%FLStudioUserData%\\z.wav", roots) == "D:\\User\\z.wav");
    CHECK (lynx::resolveFlPath ("E:\\abs.wav", roots) == "E:\\abs.wav");
    CHECK (lynx::resolveFlPath ("%FLStudioUserData%\\z.wav", {}) == "");
    {
        // POSIX roots (macOS, Linux, Wine prefix): backslashes become '/', letter case follows the disk (round 117)
        namespace fs = std::filesystem;
        const auto base = fs::temp_directory_path() / "lynx-case-test";
        fs::remove_all (base);
        fs::create_directories (base / "Data" / "Patches" / "Packs");
        { std::ofstream (base / "Data" / "Patches" / "Packs" / "Kick.WAV") << "x"; }
        lynx::FlPathRoots posix;
        posix.factoryData = base.string();
        CHECK (lynx::resolveFlPath ("%FLStudioFactoryData%\\Data\\Patches\\Packs\\Kick.WAV", posix) == (base / "Data/Patches/Packs/Kick.WAV").string());
        const auto lowerCase = lynx::resolveFlPath ("%FLStudioFactoryData%\\data\\patches\\packs\\kick.wav", posix);
        if (fs::exists (base / "data")) // case-insensitive file system (macOS): any spelling opens the file
            CHECK (fs::exists (lowerCase));
        else
            CHECK (lowerCase == (base / "Data/Patches/Packs/Kick.WAV").string());
        CHECK (lynx::resolveFlPath ("%FLStudioFactoryData%\\Data\\missing.wav", posix) == (base / "Data/missing.wav").string());
        fs::remove_all (base);
    }

    flp::Project p;
    p.channels = { { 0, flp::ChannelType::sampler, "Kick", "", "%FLStudioData%\\k.wav", 1 },
                   { 1, flp::ChannelType::sampler, "Hat", "", "%FLStudioData%\\missing.wav", 1 } };
    lynx::FlpImportOptions options;
    options.roots = roots;
    options.fileExists = [] (const std::string& path) { return path == "C:\\FL\\Data\\k.wav"; };

    const auto s = lynx::importFlp (p, options);
    CHECK (s.channels[0].instrument == lynx::InstrumentKind::sampler);
    CHECK (s.channels[0].samplePath == "C:\\FL\\Data\\k.wav");
    CHECK (s.channels[1].instrument == lynx::InstrumentKind::hat); // no file: built-in instrument
}

TEST (output_protection_is_transparent_up_to_full_scale)
{
    // round 160: no more soft knee at 0.9 — FL leaves the mix untouched up to 0 dBFS
    float s[] = { 0.5f, -0.89f, 1.5f, -3.0f, 0.95f, 0.999f, std::nanf (""), -1.0f };
    lynx::clipOvers (s, 8);
    CHECK (s[0] == 0.5f && s[1] == -0.89f && s[4] == 0.95f && s[5] == 0.999f && s[7] == -1.0f);
    CHECK (s[2] == 1.0f && s[3] == -1.0f);
    CHECK (s[6] == 0.0f);
}

TEST (song_scheduler_plays_clips_with_offset_and_cut)
{
    lynx::Song s;
    s.channels.resize (1);
    auto& p = s.patterns.emplace_back();
    for (int step : { 0, 4, 8, 12 }) // one note per beat
        p.toggleStep (0, step);

    // clip at bar 2 playing only the second half of the pattern (beats 3 and 4)
    lynx::Clip c;
    c.pattern = 0;
    c.start = 4 * lynx::ppq;
    c.length = 2 * lynx::ppq;
    c.offset = 2 * lynx::ppq;
    s.playlist.push_back (c);
    CHECK (s.songLength() == 8 * lynx::ppq);

    const double sr = 48000.0;
    const auto tps = lynx::ticksPerSample (120.0, sr); // 1 beat = 24000 samples
    lynx::EventBuffer ev;

    // whole song in a single block (8 beats = 192000 samples)
    lynx::scheduleSong (s, 0.0, 192000, tps, ev);
    std::vector<int> ons;
    for (auto& e : ev)
        if (e.type == lynx::NoteEvent::Type::on)
            ons.push_back (e.sampleOffset);
    CHECK (ons.size() == 2);
    CHECK (ons.size() == 2 && ons[0] == 4 * 24000 && ons[1] == 5 * 24000);

    // crossing the loop: bar 1 is empty, so nothing at the start of the second pass
    ev.clear();
    lynx::scheduleSong (s, 8.0 * lynx::ppq - 100 * tps, 200, tps, ev);
    CHECK (ev.size() == 0);

    // a note longer than the clip is cut at the clip end
    p.notes[3].length = 4 * lynx::ppq;
    ev.clear();
    lynx::scheduleSong (s, 0.0, 192000, tps, ev);
    int lastOff = -1;
    for (auto& e : ev)
        if (e.type == lynx::NoteEvent::Type::off)
            lastOff = std::max (lastOff, e.sampleOffset);
    CHECK (lastOff == 6 * 24000);
}

TEST (remove_pattern_fixes_clips)
{
    lynx::Song s;
    s.patterns.resize (3);
    s.currentPattern = 2;
    s.playlist = { { 0, 0, 0, 100, 0 }, { 1, 0, 100, 100, 0 }, { 2, 1, 0, 100, 0 } };
    s.removePattern (1);
    CHECK (s.patterns.size() == 2);
    CHECK (s.playlist.size() == 2 && s.playlist[1].pattern == 1);
    CHECK (s.currentPattern == 1);
}

TEST (flp_import_3x_osc_state_and_channel_filter)
{
    // new 3x Osc patch (version 14): osc 2 mix 64 coarse -12, osc 3 mix 32 coarse -24
    std::vector<std::uint8_t> blob (460, 0);
    auto put = [&] (std::size_t off, std::int32_t v) {
        for (int b = 0; b < 4; ++b)
            blob[off + (std::size_t) b] = (std::uint8_t) ((std::uint32_t) v >> (8 * b));
    };
    put (0x00, 14);
    put (0x08, 3);    // osc 1 saw
    put (0x10, 7);    // osc 1 fine +7
    put (0x04, -32);  // osc 1 pan -0.5
    put (0x1C, 64);   // osc 2 mix
    put (0x28, -12);  // osc 2 coarse
    put (0x38, 32);   // osc 3 mix
    put (0x40, 2);    // osc 3 square
    put (0x44, -19);  // osc 3 coarse: -1 octave and -7 semitones
    lynx::SynthSettings s;
    CHECK (lynx::synthFrom3xOsc (blob, s));
    CHECK (std::abs (s.osc[0].level - 0.375f) < 1e-4f && std::abs (s.osc[1].level - 0.375f) < 1e-4f && std::abs (s.osc[2].level - 0.25f) < 1e-4f);
    CHECK (s.osc[0].wave == lynx::Waveform::saw && s.osc[0].detune == 7.0f && s.osc[0].pan == -0.5f);
    CHECK (s.osc[1].wave == lynx::Waveform::sine && s.osc[1].octave == -1 && s.osc[1].semitones == 0);
    CHECK (s.osc[2].wave == lynx::Waveform::square && s.osc[2].octave == -1 && s.osc[2].semitones == -7);
    CHECK (! lynx::synthFrom3xOsc (std::vector<std::uint8_t> (40, 0), s));

    // channel filter: open by default; LP with Mod X 128 ~ 2.71 kHz; Fast LP with Mod X 128 ~ 280 Hz
    float hz = 0.0f, res = 1.0f;
    int mode = -1;
    lynx::synthFilterFromChannel (256, 0, 0, hz, res, mode);
    CHECK (hz >= 19999.0f && res == 0.0f);
    lynx::synthFilterFromChannel (128, 0, 1, hz, res, mode);
    CHECK (std::abs (hz - 2710.0f) < 30.0f && res == 0.0f);
    lynx::synthFilterFromChannel (128, 0, 0, hz, res, mode);
    CHECK (hz > 240.0f && hz < 320.0f);
    lynx::synthFilterFromChannel (128, 256, 1, hz, res, mode);
    CHECK (res > 0.8f);
    CHECK (mode == 0);
    lynx::synthFilterFromChannel (64, 0, 3, hz, res, mode); // high-pass at ~840 Hz
    CHECK (mode == 2 && std::abs (hz - 840.0f) < 40.0f);
    lynx::synthFilterFromChannel (128, 0, 2, hz, res, mode); // band-pass
    CHECK (mode == 1);
}

TEST (synth_pitch_and_filter_envelopes)
{
    auto render = [] (const lynx::SynthSettings& s, int frames) {
        auto inst = lynx::makeInstrument (lynx::InstrumentKind::synth);
        inst->prepare (48000.0);
        lynx::ChannelDef c;
        c.synth = s;
        inst->configure (c, 120.0);
        inst->noteOn (45, 1.0f); // A2 = 110 Hz
        std::vector<float> l ((std::size_t) frames), r ((std::size_t) frames);
        inst->render (l.data(), r.data(), frames, 1.0f, 1.0f);
        return l;
    };
    auto crossings = [] (const std::vector<float>& b, int from, int to) {
        int n = 0;
        for (int i = from + 1; i < to; ++i)
            n += (b[(std::size_t) i - 1] < 0.0f) != (b[(std::size_t) i] < 0.0f);
        return n;
    };

    // 3x Osc "kick": sine with a +24 st pitch envelope falling fast
    lynx::SynthSettings kick;
    kick.osc[0] = { lynx::Waveform::sine, 0, 0.0f, 1.0f };
    kick.cutoff = 20000.0f;
    kick.pitchEnv = { 24.0f, 0.0f, 0.0f, 0.05f, 0.0f, 0.05f };
    const auto k = render (kick, 48000);
    const auto early = crossings (k, 0, 2400);        // first 50 ms: well above 110 Hz
    const auto late = crossings (k, 24000, 48000);    // then back to 110 Hz (~110 crossings in 0.5 s)
    CHECK (early > 20); // +24 st ramp over 50 ms: ~2.2x the average frequency (11 crossings without envelope)
    CHECK (std::abs (late - 110) < 4);

    // cutoff envelope: saw closed at 200 Hz opens +5 octaves on attack and closes again
    lynx::SynthSettings pluck;
    pluck.cutoff = 200.0f;
    pluck.filterEnv = { 5.0f, 0.0f, 0.0f, 0.1f, 0.0f, 0.1f };
    const auto p = render (pluck, 48000);
    auto hf = [&] (int from, int to) { // energy of the difference (highs)
        double e = 0.0;
        for (int i = from + 1; i < to; ++i)
            e += std::pow ((double) p[(std::size_t) i] - p[(std::size_t) i - 1], 2.0);
        return e;
    };
    CHECK (hf (0, 2400) > 4.0 * hf (40000, 42400));

    // Toxic Biohazard's envelope (REA_rodada33 §2): exponential towards 5 clipped at 1, so the cutoff is still near
    // its base early in the attack, fully open at A = 0.2 s and back down after the exponential decay
    lynx::SynthSettings toxic = pluck;
    toxic.filterEnv = { 5.0f, 0.2f, 0.0f, 0.1f, 0.0f, 0.1f, 1 };
    const auto t = render (toxic, 48000);
    auto thf = [&] (int from, int to) {
        double e = 0.0;
        for (int i = from + 1; i < to; ++i)
            e += std::pow ((double) t[(std::size_t) i] - t[(std::size_t) i - 1], 2.0);
        return e;
    };
    CHECK (thf (9000, 9600) > 4.0 * thf (300, 900));
    CHECK (thf (9000, 9600) > 4.0 * thf (40000, 40600));
}

TEST (synth_mono_reuses_the_sounding_voice)
{
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::synth);
    inst->prepare (48000.0);
    lynx::ChannelDef c;
    c.synth.osc[0] = { lynx::Waveform::sine, 0, 0.0f, 1.0f };
    c.synth.cutoff = 20000.0f;
    c.synth.mono = true;
    inst->configure (c, 120.0);
    std::vector<float> l (48000), r (48000);
    inst->noteOn (57, 1.0f);
    inst->render (l.data(), r.data(), 4800, 1.0f, 1.0f);
    inst->noteOn (69, 1.0f); // A4 takes over the A3 voice: only one note sounding
    inst->render (l.data() + 4800, r.data() + 4800, 43200, 1.0f, 1.0f);
    int crossings = 0;
    float peak = 0.0f;
    for (std::size_t i = 24001; i < 48000; ++i)
    {
        crossings += (l[i - 1] < 0.0f) != (l[i] < 0.0f);
        peak = std::max (peak, std::abs (l[i]));
    }
    CHECK (std::abs (crossings - 440) < 4); // 0.5 s of 440 Hz, without the A3 underneath
    CHECK (peak < 0.35f);                   // one voice (0.3 level), not two summed
    // no click on the handover: the jump between neighbouring samples stays sine-sized
    float jump = 0.0f;
    for (std::size_t i = 4790; i < 4810; ++i)
        jump = std::max (jump, std::abs (l[i] - l[i - 1]));
    CHECK (jump < 0.05f);
}

TEST (synth_porta_glides_and_am_modulates)
{
    auto make = [] (const lynx::SynthSettings& s) {
        auto inst = lynx::makeInstrument (lynx::InstrumentKind::synth);
        inst->prepare (48000.0);
        lynx::ChannelDef c;
        c.synth = s;
        inst->configure (c, 120.0);
        return inst;
    };
    auto crossings = [] (const std::vector<float>& b, std::size_t from, std::size_t to) {
        int n = 0;
        for (auto i = from + 1; i < to; ++i)
            n += (b[i - 1] < 0.0f) != (b[i] < 0.0f);
        return n;
    };

    // mono + 1-beat porta (0.5 s at 120 BPM): from A3 to A4, halfway ~E4 (329.6 Hz)
    lynx::SynthSettings s;
    s.osc[0] = { lynx::Waveform::sine, 0, 0.0f, 1.0f };
    s.cutoff = 20000.0f;
    s.mono = true;
    s.porta = true;
    s.portaBeats = 1.0f;
    auto inst = make (s);
    std::vector<float> l (96000), r (96000);
    inst->noteOn (57, 1.0f);
    inst->render (l.data(), r.data(), 4800, 1.0f, 1.0f);
    inst->noteOn (69, 1.0f);
    inst->render (l.data() + 4800, r.data() + 4800, 91200, 1.0f, 1.0f);
    const auto middle = crossings (l, 4800 + 12000 - 2400, 4800 + 12000 + 2400); // 0.1 s around 0.25 s
    CHECK (std::abs (middle - 66) < 6);                                            // ~330 Hz * 0.1 s * 2
    CHECK (std::abs (crossings (l, 48000, 96000) - 880) < 4);                    // reached A4 (1 s = 880 crossings)

    // AM: osc 3 at level 1.0, slow sine, modulates osc 1 (the average level drops, the sum doesn't gain osc 3)
    lynx::SynthSettings am;
    am.osc[0] = { lynx::Waveform::sine, 0, 0.0f, 1.0f };
    am.osc[2] = { lynx::Waveform::sine, -3, 0.0f, 1.0f };
    am.cutoff = 20000.0f;
    am.am = true;
    auto modulated = make (am);
    modulated->noteOn (69, 1.0f);
    std::fill (l.begin(), l.end(), 0.0f);
    modulated->render (l.data(), r.data(), 48000, 1.0f, 1.0f);
    float peak = 0.0f;
    for (std::size_t i = 4800; i < 48000; ++i)
        peak = std::max (peak, std::abs (l[i]));
    CHECK (peak > 0.45f && peak < 0.65f); // 0.3 · (1 + 1) at most: (1 − o3) goes up to 2
}

TEST (channel_lfo_vibrato_and_tremolo)
{
    auto render = [] (const lynx::SynthSettings& s) {
        auto inst = lynx::makeInstrument (lynx::InstrumentKind::synth);
        inst->prepare (48000.0);
        lynx::ChannelDef c;
        c.synth = s;
        inst->configure (c, 120.0);
        inst->noteOn (69, 1.0f);
        std::vector<float> l (48000), r (48000);
        inst->render (l.data(), r.data(), 48000, 1.0f, 1.0f);
        return l;
    };
    lynx::SynthSettings s;
    s.osc[0] = { lynx::Waveform::sine, 0, 0.0f, 1.0f };
    s.cutoff = 20000.0f;

    // tremolo: 100% volume LFO at 4 Hz (0.25 s period) -> the amplitude envelope goes from ~0 to ~2x
    auto tremolo = s;
    tremolo.lfo[lynx::lfoVolume] = { 1.0f, 0.25f, false, 0, 0.0f, 0.0f };
    const auto t = render (tremolo);
    float lowest = 1.0f, highest = 0.0f;
    for (std::size_t w = 4800; w + 480 <= 48000; w += 480) // 10 ms windows (4.4 cycles of 440 Hz)
    {
        float peak = 0.0f;
        for (auto i = w; i < w + 480; ++i)
            peak = std::max (peak, std::abs (t[i]));
        lowest = std::min (lowest, peak);
        highest = std::max (highest, peak);
    }
    CHECK (lowest < 0.08f && highest > 0.5f);

    // synced vibrato: ±1 semitone, 1-beat period (0.5 s at 120 BPM); the average stays at 440 Hz
    auto vibrato = s;
    vibrato.lfo[lynx::lfoPitch] = { 1.0f, 1.0f, true, 0, 0.0f, 0.0f };
    const auto v = render (vibrato);
    int crossings = 0;
    for (std::size_t i = 1; i < 48000; ++i)
        crossings += (v[i - 1] < 0.0f) != (v[i] < 0.0f);
    CHECK (std::abs (crossings - 880) < 12);
    int firstQuarter = 0; // first quarter of the cycle: rising sine -> higher than 440
    for (std::size_t i = 1; i < 6000; ++i)
        firstQuarter += (v[i - 1] < 0.0f) != (v[i] < 0.0f);
    CHECK (firstQuarter > 112);
}

TEST (sampler_porta_glides_from_previous_note)
{
    auto data = std::make_shared<lynx::SampleData>();
    data->sampleRate = 48000.0;
    data->left.resize (96000);
    for (std::size_t i = 0; i < data->left.size(); ++i)
        data->left[i] = 0.5f * (float) std::sin (6.283185307 * 220.0 * (double) i / 48000.0);
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::sampler, data);
    inst->prepare (48000.0);
    lynx::ChannelDef c;
    c.sampler.cut = true;
    c.sampler.porta = true;
    c.sampler.portaBeats = 1.0f; // 0.5 s at 120 BPM
    inst->configure (c, 120.0);
    std::vector<float> l (24000), r (24000);
    inst->noteOn (60, 1.0f);
    inst->render (l.data(), r.data(), 2400, 1.0f, 1.0f);
    inst->noteOn (72, 1.0f); // one octave up: starts at 220 Hz and rises
    std::fill (l.begin(), l.end(), 0.0f);
    inst->render (l.data(), r.data(), 24000, 1.0f, 1.0f);
    auto crossings = [&] (std::size_t from, std::size_t to) {
        int n = 0;
        for (auto i = from + 1; i < to; ++i)
            n += (l[i - 1] < 0.0f) != (l[i] < 0.0f);
        return n;
    };
    const auto start = crossings (600, 2400);     // 37.5 ms still near 220 Hz: ~17 crossings (440 Hz would give 33)
    const auto end = crossings (21600, 24000);    // 50 ms at the end of the glide: near 440 Hz (~44; 220 Hz would give 22)
    CHECK (start < 22);
    CHECK (end > 36);
}

TEST (zoned_sampler_picks_zone_by_key_and_velocity)
{
    auto sine = [] (double hz, float amp) {
        auto d = std::make_shared<lynx::SampleData>();
        d->sampleRate = 48000.0;
        d->left.resize (48000);
        for (std::size_t i = 0; i < d->left.size(); ++i)
            d->left[i] = amp * (float) std::sin (6.283185307 * hz * (double) i / 48000.0);
        return d;
    };
    // lows: 220 Hz sine recorded as A3 (57), up to key 69; highs: 880 Hz as A5 (81)
    // hard layer on the highs: same tuning, louder
    std::vector<lynx::LoadedZone> zones {
        { { "graves.wav", 57, 0, 69, 0, 127, 0.0f, 1.0f }, sine (220.0, 0.5f) },
        { { "agudos.wav", 81, 70, 127, 0, 99, 0.0f, 1.0f }, sine (880.0, 0.25f) },
        { { "agudos forte.wav", 81, 70, 127, 100, 127, 0.0f, 1.0f }, sine (880.0, 0.9f) },
    };
    auto inst = lynx::makeZonedSampler (zones);
    inst->prepare (48000.0);
    inst->configure (lynx::ChannelDef {}, 120.0);
    auto play = [&] (int key, float velocity) {
        inst->allNotesOff();
        std::vector<float> l (24000), r (24000);
        inst->render (l.data(), r.data(), 2000, 1.0f, 1.0f); // let the old voices die out
        std::fill (l.begin(), l.end(), 0.0f);
        inst->noteOn (key, velocity);
        inst->render (l.data(), r.data(), 24000, 1.0f, 1.0f);
        int crossings = 0;
        float peak = 0.0f;
        for (std::size_t i = 1; i < 24000; ++i)
        {
            crossings += (l[i - 1] < 0.0f) != (l[i] < 0.0f);
            peak = std::max (peak, std::abs (l[i]));
        }
        return std::pair { crossings, peak };
    };
    const auto low = play (69, 0.5f);    // A4 from root A3: 440 Hz -> 440 crossings in 0.5 s
    CHECK (std::abs (low.first - 440) < 4);
    const auto soft = play (81, 0.5f);   // high zone at the root: 880 Hz
    CHECK (std::abs (soft.first - 880) < 6);
    const auto hard = play (81, 0.95f);  // high velocity: hard layer
    CHECK (hard.second > soft.second * 2.0f);

    // overlapping layers round-robin: two identical zones at different levels, notes alternate
    auto rr = lynx::makeZonedSampler ({ { { "a.wav", 60 }, sine (440.0, 0.2f) }, { { "b.wav", 60 }, sine (440.0, 0.8f) } });
    rr->prepare (48000.0);
    rr->configure (lynx::ChannelDef {}, 120.0);
    float peaks[2] {};
    for (int k = 0; k < 2; ++k)
    {
        rr->allNotesOff();
        std::vector<float> l (4800), r (4800);
        rr->render (l.data(), r.data(), 2000, 1.0f, 1.0f);
        std::fill (l.begin(), l.end(), 0.0f);
        rr->noteOn (60, 1.0f);
        rr->render (l.data(), r.data(), 4800, 1.0f, 1.0f);
        for (auto v : l)
            peaks[k] = std::max (peaks[k], std::abs (v));
    }
    CHECK (std::abs (peaks[0] - peaks[1]) > 0.3f);
}

TEST (zoned_sampler_loops_while_held)
{
    // 0.1 s sample with a loop in the middle: holding the note it keeps sounding well past the end of the file;
    // on release, it leaves the loop and ends
    auto data = std::make_shared<lynx::SampleData>();
    data->sampleRate = 48000.0;
    data->left.resize (4800);
    for (std::size_t i = 0; i < data->left.size(); ++i)
        data->left[i] = 0.5f * (float) std::sin (6.283185307 * 480.0 * (double) i / 48000.0); // 100 frames per cycle
    lynx::SampleZone zone { "loop.wav", 60 };
    zone.loopStart = 1000;
    zone.loopEnd = 3000;
    zone.loopMode = 2;
    auto inst = lynx::makeZonedSampler ({ { zone, data } });
    inst->prepare (48000.0);
    lynx::ChannelDef c;
    c.sampler.release = 0.01f;
    inst->configure (c, 120.0);
    inst->noteOn (60, 1.0f);
    std::vector<float> l (48000), r (48000);
    inst->render (l.data(), r.data(), 24000, 1.0f, 1.0f); // 0.5 s held
    float held = 0.0f;
    for (std::size_t i = 20000; i < 24000; ++i)
        held = std::max (held, std::abs (l[i]));
    CHECK (held > 0.3f);
    inst->noteOff (60);
    inst->render (l.data() + 24000, r.data() + 24000, 24000, 1.0f, 1.0f);
    float after = 0.0f;
    for (std::size_t i = 40000; i < 48000; ++i)
        after = std::max (after, std::abs (l[i]));
    CHECK (after == 0.0f);
}

TEST (global_lfo_follows_song_position)
{
    // global volume LFO (1-beat period): a note mid-cycle starts mid-wave; the
    // same note with a per-note LFO starts at phase 0
    auto first = [] (bool global, double songTicks) {
        auto inst = lynx::makeInstrument (lynx::InstrumentKind::synth);
        inst->prepare (48000.0);
        lynx::ChannelDef c;
        c.synth.osc[0] = { lynx::Waveform::sine, 0, 0.0f, 1.0f };
        c.synth.cutoff = 20000.0f;
        c.synth.lfo[lynx::lfoVolume] = { 1.0f, 1.0f, true, 2, 0.0f, 0.0f, global }; // pulse: +1 in the first half
        inst->configure (c, 120.0);
        inst->setSongPosition (songTicks);
        inst->noteOn (69, 1.0f);
        std::vector<float> l (4800), r (4800);
        inst->render (l.data(), r.data(), 4800, 1.0f, 1.0f);
        float peak = 0.0f;
        for (std::size_t i = 2400; i < 4800; ++i)
            peak = std::max (peak, std::abs (l[i]));
        return peak;
    };
    // mid-cycle (phase 0.6) the pulse is at −1: volume ~0; per note (phase 0) it is at +1: ~2x
    const auto globalLate = first (true, 0.6 * lynx::ppq);
    const auto perNote = first (false, 0.6 * lynx::ppq);
    CHECK (globalLate < 0.05f);
    CHECK (perNote > 0.4f);
}

TEST (sampler_channel_filter_darkens_and_bypasses_when_open)
{
    // white noise in the sampler: with a 300 Hz cutoff the energy drops a lot; open passes unchanged
    auto data = std::make_shared<lynx::SampleData>();
    data->sampleRate = 48000.0;
    data->left.resize (48000);
    std::uint32_t seed = 7;
    for (auto& v : data->left)
    {
        seed = seed * 1664525u + 1013904223u;
        v = ((float) (seed >> 8) / 16777216.0f - 0.5f);
    }
    data->right = data->left;
    auto energy = [&] (float cutoff) {
        auto inst = lynx::makeInstrument (lynx::InstrumentKind::sampler, data);
        inst->prepare (48000.0);
        lynx::ChannelDef c;
        c.sampler.cutoff = cutoff;
        inst->configure (c, 120.0);
        inst->noteOn (60, 1.0f);
        std::vector<float> l (24000), r (24000);
        inst->render (l.data(), r.data(), 24000, 1.0f, 1.0f);
        double e = 0.0;
        for (auto v : l)
            e += (double) v * v;
        return e;
    };
    const auto open = energy (20000.0f), dark = energy (300.0f);
    {
        // high-pass at 10 kHz: also removes most of the noise energy, but from the other side
        auto inst = lynx::makeInstrument (lynx::InstrumentKind::sampler, data);
        inst->prepare (48000.0);
        lynx::ChannelDef hp;
        hp.sampler.cutoff = 10000.0f;
        hp.sampler.filterType = 2;
        inst->configure (hp, 120.0);
        inst->noteOn (60, 1.0f);
        std::vector<float> l (24000), r (24000);
        inst->render (l.data(), r.data(), 24000, 1.0f, 1.0f);
        double e = 0.0, lowE = 0.0;
        float lp = 0.0f;
        for (auto v : l)
        {
            e += (double) v * v;
            lp += 0.01f * (v - lp); // crude low-pass: almost nothing should remain below
            lowE += (double) lp * lp;
        }
        CHECK (e > 10.0 && e < 0.7 * energy (20000.0f));
        CHECK (lowE < e * 0.01);
    }
    CHECK (open > 100.0);
    CHECK (dark < open * 0.05);

    // +12 st pitch envelope with sustain 1: the sample runs at double speed
    // and ends in half the time (1 s of sample -> 0.5 s)
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::sampler, data);
    inst->prepare (48000.0);
    lynx::ChannelDef c;
    c.sampler.pitchEnv = { 12.0f, 0.0f, 0.0f, 0.1f, 1.0f, 0.1f };
    inst->configure (c, 120.0);
    inst->noteOn (60, 1.0f);
    std::vector<float> l (48000), r (48000);
    inst->render (l.data(), r.data(), 48000, 1.0f, 1.0f);
    float tail = 0.0f, body = 0.0f;
    for (std::size_t i = 20000; i < 23000; ++i)
        body = std::max (body, std::abs (l[i]));
    for (std::size_t i = 25000; i < 48000; ++i)
        tail = std::max (tail, std::abs (l[i]));
    CHECK (body > 0.1f);
    CHECK (tail == 0.0f);
}

TEST (synth_oscillator_pan_and_semitones)
{
    auto render = [] (lynx::SynthSettings s, std::vector<float>& l, std::vector<float>& r) {
        auto synth = lynx::makeInstrument (lynx::InstrumentKind::synth);
        synth->prepare (48000.0);
        lynx::ChannelDef c;
        c.synth = s;
        synth->configure (c, 120.0);
        synth->noteOn (69, 1.0f);
        l.assign (24000, 0.0f);
        r.assign (24000, 0.0f);
        synth->render (l.data(), r.data(), 24000, 1.0f, 1.0f);
    };
    lynx::SynthSettings s;
    s.osc[0] = { lynx::Waveform::sine, 0, 0.0f, 1.0f, 7, 1.0f }; // a fifth up, fully right
    s.cutoff = 20000.0f;
    std::vector<float> l, r;
    render (s, l, r);
    float peakL = 0.0f, peakR = 0.0f;
    int crossings = 0;
    for (std::size_t i = 4800; i < 24000; ++i)
    {
        peakL = std::max (peakL, std::abs (l[i]));
        peakR = std::max (peakR, std::abs (r[i]));
        crossings += (r[i - 1] < 0.0f) != (r[i] < 0.0f);
    }
    CHECK (peakL < 1e-4f && peakR > 0.1f);
    // 440 * 2^(7/12) = 659.3 Hz; 0.4 s = 263.7 cycles = ~527 crossings
    CHECK (std::abs (crossings - 527) < 6);
}

TEST (fl_wrapper_state_parses_records_and_vst3_state)
{
    // Fruity Wrapper blob (version 12): VST3 generator type, class ID, name, path and state
    std::vector<std::uint8_t> blob;
    auto u32 = [] (std::vector<std::uint8_t>& b, std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back ((std::uint8_t) (v >> (8 * i))); };
    auto record = [&] (std::vector<std::uint8_t>& b, std::uint32_t id, const std::vector<std::uint8_t>& payload) {
        u32 (b, id);
        u32 (b, (std::uint32_t) payload.size());
        u32 (b, 0);
        b.insert (b.end(), payload.begin(), payload.end());
    };
    auto bytes = [] (const std::string& s) { return std::vector<std::uint8_t> (s.begin(), s.end()); };

    std::vector<std::uint8_t> vst3State;
    u32 (vst3State, 1);
    record (vst3State, 1, std::vector<std::uint8_t> (64, 0));
    record (vst3State, 3, bytes ("COMPONENTE"));
    record (vst3State, 2, bytes ("CONTROLADOR"));
    // record 4: 3 slots → ParamIDs 700, −1 (empty), 0xFFFFFF01
    std::vector<std::uint8_t> slots;
    for (auto v : { 3u, 700u, 0xFFFFFFFFu, 0xFFFFFF01u })
        u32 (slots, v);
    record (vst3State, 4, slots);

    u32 (blob, 12);
    record (blob, 0x01, std::vector<std::uint8_t> (20, 0xFF));
    record (blob, 0x32, { 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 });
    std::vector<std::uint8_t> cid;
    for (int i = 0; i < 16; ++i)
        cid.push_back ((std::uint8_t) (0xA0 + i));
    record (blob, 0x34, cid);
    record (blob, 0x36, bytes ("Surge XT"));
    record (blob, 0x37, bytes ("C:/Program Files/Common Files/VST3/Surge XT.vst3"));
    record (blob, 0x35, vst3State);

    lynx::ForeignPlugin p;
    CHECK (lynx::parseFlWrapper (blob, p));
    CHECK (p.format == lynx::ForeignPlugin::Format::vst3);
    CHECK (p.hasClassId && p.classId[0] == 0xA0 && p.classId[15] == 0xAF);
    CHECK (p.name == "Surge XT" && p.path.find ("Surge XT.vst3") != std::string::npos);
    std::vector<std::uint8_t> component, controller;
    CHECK (lynx::splitFlVst3State (p.state, component, controller));
    CHECK (component == bytes ("COMPONENTE") && controller == bytes ("CONTROLADOR"));
    const auto saved = lynx::flVst3ParamSlots (p.state);
    CHECK (saved == (std::vector<std::int32_t> { 700, -1, (std::int32_t) 0xFFFFFF01u }));

    // FL's slot assignment: saved slots keep their ParamID wherever it now sits; the empty slot stays empty;
    // a vanished ParamID (0xFFFFFF01) leaves its slot dead; parameters without a slot get slots 3, 4… in list order
    const auto positions = lynx::flVst3SlotPositions (saved, { 5u, 700u, 9u });
    CHECK (positions == (std::vector<int> { 1, -1, -1, 0, 2 }));
    CHECK (lynx::flVst3SlotPositions ({}, { 5u, 6u }) == (std::vector<int> { 0, 1 })); // new instance: slot = position

    // VST2 state (REA_rodada89 §2): chunk and raw-parameter layouts, current (−9) and older versions
    {
        lynx::FlVst2State st;
        st.flags = 8;
        st.currentProgram = 3;
        st.chunk = bytes ("BANKCHUNK");
        const auto withChunk = lynx::writeFlVst2State (st);
        lynx::FlVst2State back;
        CHECK (lynx::parseFlVst2State (withChunk, back));
        CHECK (back.version == -9 && back.flags == 8 && back.currentProgram == 3 && back.chunk == bytes ("BANKCHUNK") && back.params.empty());
        CHECK (! back.chunkIsProgram());

        lynx::FlVst2State raw;
        raw.flags = 1;
        raw.params = { 0.25f, 0.5f, 1.0f };
        raw.programNames = { "Init" };
        const auto withParams = lynx::writeFlVst2State (raw);
        CHECK (withParams.size() == 4 + 1 + 4 + 8 + 4 + 4 + 12 + 4 + 25);
        CHECK (lynx::parseFlVst2State (withParams, back));
        CHECK (back.chunk.empty() && back.params == raw.params && back.allPrograms() && back.programNames == std::vector<std::string> { "Init" });

        // version −5: no flags, i32 size, then i32 program; −6 without "all programs": the chunk is one program
        std::vector<std::uint8_t> v5;
        for (auto v : { 0xFFFFFFFBu, 4u, 7u })
            u32 (v5, v);
        for (auto c : bytes ("ABCD"))
            v5.push_back (c);
        CHECK (lynx::parseFlVst2State (v5, back) && back.version == -5 && back.currentProgram == 7 && back.chunk == bytes ("ABCD"));
        std::vector<std::uint8_t> v6;
        u32 (v6, 0xFFFFFFFAu);
        v6.push_back (0);
        for (auto v : { 2u, 0u })
            u32 (v6, v);
        v6.push_back ('x');
        v6.push_back ('y');
        CHECK (lynx::parseFlVst2State (v6, back) && back.chunkIsProgram() && back.chunk == bytes ("xy"));

        // nothing to restore (123456), unknown old layouts, truncated chunks
        std::vector<std::uint8_t> marker;
        u32 (marker, 123456u);
        CHECK (! lynx::parseFlVst2State (marker, back));
        auto cut = withChunk;
        cut.pop_back();
        CHECK (! lynx::parseFlVst2State (cut, back));
        CHECK (! lynx::parseFlVst2State ({}, back));
    }

    // version out of range or no type: not a usable wrapper
    blob[0] = 13;
    CHECK (! lynx::parseFlWrapper (blob, p));
    CHECK (! lynx::parseFlWrapper ({ 12, 0, 0, 0 }, p));
}

TEST (schedule_index_matches_full_scan)
{
    // random song (long notes, swing, clips with offset); indexed scheduling must yield
    // exactly the same events as the full scan, including in the block that wraps around
    lynx::Song s = lynx::Song::makeDefault();
    s.swing = 0.6f;
    std::uint32_t seed = 12345;
    auto rnd = [&] (int n) { seed = seed * 1664525u + 1013904223u; return (int) ((seed >> 8) % (std::uint32_t) n); };
    s.patterns.resize (3);
    for (auto& p : s.patterns)
    {
        p.notes.clear();
        for (int i = 0; i < 60; ++i)
        {
            lynx::Note n;
            n.start = rnd (16 * lynx::ppq);
            n.length = rnd (5) == 0 ? rnd (12 * lynx::ppq) + 1 : rnd (lynx::ppq) + 1;
            n.channel = rnd (4);
            n.key = 40 + rnd (40);
            n.slide = rnd (20) == 0;
            p.notes.push_back (n);
        }
        p.sortNotes();
    }
    s.playlist.clear();
    for (int i = 0; i < 12; ++i)
    {
        lynx::Clip c;
        c.pattern = rnd (3);
        c.track = rnd (4);
        c.start = rnd (40) * lynx::ppq;
        c.length = (rnd (12) + 1) * lynx::ppq;
        c.offset = rnd (4) * lynx::ppq;
        s.playlist.push_back (c);
    }
    s.mode = lynx::PlayMode::song;
    const auto index = lynx::makeScheduleIndex (s);
    const auto tps = lynx::ticksPerSample (140.0, 48000.0);
    const auto loop = (double) s.songLength();
    int mismatches = 0, events = 0;

    for (int block = 0; block < 400; ++block)
    {
        const auto start = block == 0 ? loop - 100.0 * tps : (double) rnd ((int) loop * 2);
        const auto samples = 64 + rnd (24000); // up to ~1 beat per block
        lynx::EventBuffer a, b;
        lynx::scheduleSong (s, start, samples, tps, a, false, nullptr);
        lynx::scheduleSong (s, start, samples, tps, b, false, &index);
        events += a.size();
        if (a.size() != b.size())
        {
            ++mismatches;
            continue;
        }
        for (int i = 0; i < a.size(); ++i)
        {
            const auto& x = *(a.begin() + i);
            const auto& y = *(b.begin() + i);
            mismatches += x.sampleOffset != y.sampleOffset || x.type != y.type || x.key != y.key || x.channel != y.channel;
        }
    }

    CHECK (events > 500);
    CHECK (mismatches == 0);
}

TEST (mixer_track_eq_defaults_and_boost)
{
    // FL defaults: 90 Hz, 1.5 kHz and 8 kHz; peak Q 0.632; shelf slope 0.607
    lynx::TrackEq eq;
    CHECK (std::abs (lynx::TrackEq::peakQ (eq.width[1]) - 0.632) < 0.01);
    CHECK (std::abs (lynx::TrackEq::shelfSlope (eq.width[0]) - 0.607) < 0.01);
    CHECK (! eq.active());

    // +12 dB peak at 1.5 kHz on a track: a 1.5 kHz sine rises ~4x
    lynx::Song song = lynx::Song::makeDefault();
    song.mixer[1].eq.gainDb[1] = 12.0f;
    for (auto& c : song.channels)
        c.mixerTrack = 1;
    song.channels[4].instrument = lynx::InstrumentKind::synth;
    song.channels[4].synth = {};
    song.channels[4].synth.osc[0] = { lynx::Waveform::sine, 0, 0.0f, 1.0f };
    song.channels[4].synth.cutoff = 20000.0f;
    auto render = [] (const lynx::Song& s) {
        auto state = lynx::RackState::build (s, nullptr, 48000.0);
        lynx::RackRenderer renderer;
        renderer.prepare (48000.0);
        lynx::LiveEventQueue live;
        lynx::NoteEvent on; // 1.5 kHz ~ key 90.6: use 91 (1568 Hz), still within the wide peak
        on.channel = 4;
        on.key = 91;
        on.velocity = 1.0f;
        live.push (on);
        std::vector<float> l (24000), r (24000);
        for (int pos = 0; pos < 24000; pos += 480)
            renderer.process (state.get(), false, 0.0, 480, l.data() + pos, r.data() + pos, pos == 0 ? &live : nullptr);
        float peak = 0.0f;
        for (std::size_t i = 12000; i < 24000; ++i)
            peak = std::max (peak, std::abs (l[i]));
        return peak;
    };
    const auto boosted = render (song);
    song.mixer[1].eq.gainDb[1] = 0.0f;
    const auto flat = render (song);
    CHECK (flat > 0.01f);
    CHECK (boosted / flat > 3.0f && boosted / flat < 4.2f);
}

TEST (bars_follow_time_signature_markers)
{
    lynx::Song song;
    song.beatsPerBar = 4;
    const auto bar4 = (std::int64_t) lynx::ppq * 4;
    auto bars = song.barsBetween (0, bar4 * 3);
    CHECK (bars.size() == 3 && bars[2].tick == bar4 * 2 && bars[2].number == 3);

    // 7/4 from bar 3, and 3/4 in the middle of a 7 bar (cuts and restarts)
    song.addMarker ({ bar4 * 2, "Verso", 7, 4 });
    song.addMarker ({ bar4, "Nome só", 0, 4 });
    const auto seven = (std::int64_t) lynx::ppq * 7;
    song.addMarker ({ bar4 * 2 + seven + lynx::ppq * 2, "", 3, 4 });
    bars = song.barsBetween (0, bar4 * 2 + seven * 2);
    CHECK (bars.size() >= 5);
    CHECK (bars[1].length == bar4 && bars[2].tick == bar4 * 2 && bars[2].length == seven);
    CHECK (bars[3].length == lynx::ppq * 2);                                // cut short by the change
    CHECK (bars[4].length == lynx::ppq * 3 && bars[4].number == 5);
    CHECK (song.markers()[0].name == "Nome só");                            // sorted by tick

    // from far away: starts at the right bar without losing the numbering
    const auto far = song.barsBetween (lynx::ppq * 3000, lynx::ppq * 3001);
    CHECK (far.size() == 1 && far[0].length == lynx::ppq * 3);
    CHECK (song.barAt (lynx::ppq * 3000).number == far[0].number);
    CHECK ((lynx::ppq * 3000 - far[0].tick) < far[0].length);
}

TEST (split_drum_channel_moves_notes_to_pads)
{
    lynx::Song song = lynx::Song::makeDefault();
    const auto channels = (int) song.channels.size();
    song.patterns[0].notes.clear();
    song.patterns[0].notes.push_back ({ 0, 240, 1, 36, 0.9f });   // kit kick (channel 1)
    song.patterns[0].notes.push_back ({ 240, 240, 1, 38, 0.8f }); // snare
    song.patterns[0].notes.push_back ({ 480, 240, 1, 50, 0.8f }); // key without a pad: dropped
    song.patterns[0].notes.push_back ({ 480, 240, 3, 60, 0.8f }); // other channel: shifts by 1 position
    lynx::Automation a;
    a.target = { lynx::AutomationTargetKind::channelVolume, 3, 0, 0 };
    song.automations.push_back (a);

    std::vector<lynx::DrumPad> pads (2);
    pads[0].name = "Kick";
    pads[0].key = 36;
    pads[0].layers = { { "kick.wav" } };
    pads[1].name = "Snare";
    pads[1].key = 38;
    pads[1].layers = { { "snare soft.wav", 60, 0, 127, 0, 90 }, { "snare hard.wav", 60, 0, 127, 91, 127 } };
    pads[1].cutGroup = 2;
    pads[1].cutBy = 2;
    lynx::splitDrumChannel (song, 1, pads);

    CHECK ((int) song.channels.size() == channels + 1);
    CHECK (song.channels[1].samplePath == "kick.wav" && song.channels[1].instrument == lynx::InstrumentKind::sampler);
    CHECK (song.channels[2].zones.size() == 2 && song.channels[2].cutGroup == song.channels[2].cutBy && song.channels[2].cutGroup != 0);
    const auto& notes = song.patterns[0].notes;
    CHECK (notes.size() == 3);
    CHECK (notes[0].channel == 1 && notes[0].key == 60);
    CHECK (notes[1].channel == 2 && notes[1].key == 60);
    CHECK (notes[2].channel == 4); // the old channel 3
    CHECK (song.automations[0].target.index == 4);
}

TEST (flp_import_brings_playlist)
{
    flp::Project p;
    p.header.ppq = 96;
    p.channels = { { 0, flp::ChannelType::sampler, "Kick", "", "", 1 } };
    flp::Pattern empty;
    empty.index = 1;
    flp::Pattern full;
    full.index = 2;
    flp::Note n;
    n.channel = 0;
    full.notes.push_back (n);
    p.patterns = { empty, full };

    flp::Arrangement a;
    flp::PlaylistItem item;
    item.kind = flp::PlaylistItem::Kind::pattern;
    item.index = 2;
    item.position = 384;
    item.length = 192;
    item.track = 3;
    item.patternStartTick = 96;
    a.items.push_back (item);
    item.index = 1; // empty pattern: discarded
    a.items.push_back (item);
    item.kind = flp::PlaylistItem::Kind::channelClip; // audio: not supported yet
    a.items.push_back (item);
    p.arrangements.push_back (a);

    const auto s = lynx::importFlp (p);
    CHECK (s.mode == lynx::PlayMode::song);
    CHECK (s.playlist.size() == 1);
    CHECK (s.playlist[0].pattern == 0 && s.playlist[0].track == 3);
    CHECK (s.playlist[0].start == 4 * lynx::ppq && s.playlist[0].length == 2 * lynx::ppq && s.playlist[0].offset == lynx::ppq);
}

TEST (rack_releases_notes_on_position_jump)
{
    lynx::Song s;
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::synth;
    s.channels.push_back (c);
    auto& p = s.patterns.emplace_back();
    lynx::Note n;
    n.start = 0;
    n.length = 4 * lynx::ppq; // long note
    p.notes.push_back (n);
    s.bpm = 120.0;

    auto state = lynx::RackState::build (s, nullptr, 48000.0);
    lynx::RackRenderer r;
    r.prepare (48000.0);
    std::vector<float> l (512), rr (512);
    const auto tps = lynx::ticksPerSample (120.0, 48000.0);

    double tick = 0.0;
    for (int i = 0; i < 20; ++i, tick += 512 * tps)
        r.process (state.get(), true, tick, 512, l.data(), rr.data());

    // jump to tick 2000 (mid-note, no new "on"): the synth must go silent
    for (int i = 0; i < 40; ++i)
        r.process (state.get(), true, 2000.0 + i * 512 * tps, 512, l.data(), rr.data());

    float tail = 0.0f;
    for (auto v : l)
        tail = std::max (tail, std::abs (v));
    CHECK (tail < 0.001f);
}

namespace
{
// Two samplers with a constant 0.5 signal on different inserts; returns the output peak.
float juce_db (float gain)
{
    return 20.0f * std::log10 (gain);
}

float renderMixerCase (const std::function<void (lynx::Song&)>& tweak, lynx::MeterBank* meters = nullptr)
{
    lynx::Song s;
    s.mixer = lynx::Song::defaultMixer (4);

    for (int i = 0; i < 2; ++i)
    {
        lynx::ChannelDef c;
        c.instrument = lynx::InstrumentKind::sampler;
        c.samplePath = "dc";
        c.volume = 1.0f;
        c.mixerTrack = i + 1;
        s.channels.push_back (c);
    }

    s.patterns.emplace_back();
    tweak (s);

    auto provider = [] (const std::string&) {
        auto d = std::make_shared<lynx::SampleData>();
        d->left.assign (48000, 0.5f);
        return std::shared_ptr<const lynx::SampleData> (d);
    };

    auto state = lynx::RackState::build (s, nullptr, 48000.0, provider);
    lynx::RackRenderer r;
    r.prepare (48000.0, 256);
    lynx::LiveEventQueue live;

    for (int ch = 0; ch < 2; ++ch)
    {
        lynx::NoteEvent on;
        on.channel = ch;
        on.velocity = 1.0f;
        live.push (on);
    }

    std::vector<float> l (1000), rr (1000); // larger than the mixer block: tests the chunking
    r.process (state.get(), false, 0.0, 1000, l.data(), rr.data(), &live, meters);
    return std::max (std::abs (l[999]), std::abs (rr[999]));
}
} // namespace

TEST (mixer_routes_mutes_solos_and_meters)
{
    // two 0.5 channels summed on the Master at 0 dB
    CHECK (std::abs (renderMixerCase ([] (lynx::Song&) {}) - 1.0f) < 1e-4f);

    // insert 1 muted: only channel 2 remains
    CHECK (std::abs (renderMixerCase ([] (lynx::Song& s) { s.mixer[1].muted = true; }) - 0.5f) < 1e-4f);

    // solo on insert 2: same
    CHECK (std::abs (renderMixerCase ([] (lynx::Song& s) { s.mixer[2].solo = true; }) - 0.5f) < 1e-4f);

    // FL fader curve: 0.8 = 0 dB, max ≈ +5.6 dB, half travel ≈ -12.7 dB
    CHECK (std::abs (lynx::faderGain (0.8f) - 1.0f) < 1e-5f);
    CHECK (std::abs (juce_db (lynx::faderGain (1.0f)) - 5.59f) < 0.05f);
    CHECK (lynx::faderGain (0.0f) == 0.0f);

    // Master fader at max
    CHECK (std::abs (renderMixerCase ([] (lynx::Song& s) { s.mixer[0].volume = 1.0f; }) - lynx::faderGain (1.0f)) < 1e-4f);

    // both channels on the same insert, insert fader at 0.4
    CHECK (std::abs (renderMixerCase ([] (lynx::Song& s) {
               s.channels[1].mixerTrack = 1;
               s.mixer[1].volume = 0.4f;
           }) - lynx::faderGain (0.4f)) < 1e-4f);

    // a route outside the mixer falls to the last existing insert, without crashing
    CHECK (renderMixerCase ([] (lynx::Song& s) { s.channels[0].mixerTrack = 100; }) > 0.9f);

    // no mixer: everything straight to the output at 0 dB
    CHECK (std::abs (renderMixerCase ([] (lynx::Song& s) { s.mixer.clear(); }) - 1.0f) < 1e-4f);

    lynx::MeterBank meters;
    renderMixerCase ([] (lynx::Song&) {}, &meters);
    // (the 24-point sinc's ringing at the sample start: its x87 kernel table rounds differently on ARM64)
    CHECK (std::abs (meters.takePeak (1, 0) - 0.5f) < armTolerance (1e-4f, 2e-4f));
    CHECK (std::abs (meters.takePeak (0, 1) - 1.0f) < armTolerance (1e-4f, 4e-4f));
    CHECK (meters.takePeak (1, 0) == 0.0f); // reading resets
    CHECK (meters.takePeak (3, 0) == 0.0f); // insert with no channel
}

TEST (next_free_insert_and_flp_mixer_import)
{
    auto s = lynx::Song::makeDefault();
    CHECK (s.mixer.size() == 17 && s.mixer[0].name == "Master");
    CHECK (s.nextFreeInsert() == 6); // the 5 default channels take 1..5

    flp::Project p;
    p.channels = { { 0, flp::ChannelType::sampler, "Kick", "", "", 30 } };
    p.mixer = { { 0, "" }, { 30, "Drums" } };
    const auto song = lynx::importFlp (p);
    CHECK (song.channels[0].mixerTrack == 30);
    CHECK (song.mixer.size() == 31);
    CHECK (song.mixer[30].name == "Drums" && song.mixer[0].name == "Master");
}

namespace
{
float runEffectOnSine (lynx::EffectKind kind, const std::function<void (lynx::EffectSlot&)>& tweak, double hz, float amp,
                       int frames = 48000)
{
    auto fx = lynx::makeEffect (kind);
    fx->prepare (48000.0);
    auto slot = lynx::makeEffectSlot (kind);
    tweak (slot);
    std::vector<float> l ((std::size_t) frames), r ((std::size_t) frames);
    for (int i = 0; i < frames; ++i)
        l[(std::size_t) i] = r[(std::size_t) i] = amp * (float) std::sin (6.283185307 * hz * i / 48000.0);
    for (int i = 0; i < frames; i += 480)
        fx->process (l.data() + i, r.data() + i, std::min (480, frames - i), slot.params, 120.0);
    float peak = 0.0f;
    for (int i = frames / 2; i < frames; ++i) // second half: already settled
    {
        CHECK (std::isfinite (l[(std::size_t) i]));
        peak = std::max (peak, std::abs (l[(std::size_t) i]));
    }
    return peak;
}
} // namespace

TEST (parametric_eq_bands_and_response)
{
    // default: transparent
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::parametricEq);
    CHECK (std::abs (lynx::peq::responseDb (slot.params, 48000.0, 1000.0)) < 0.01);
    CHECK (std::abs (runEffectOnSine (lynx::EffectKind::parametricEq, [] (auto&) {}, 1000.0, 0.5f) - 0.5f) < 0.005f);

    // +12 dB peak at 1 kHz on band 4: the response and the signal agree
    auto boost = [] (lynx::EffectSlot& s) {
        s.params[(std::size_t) lynx::peq::param (3, 1)] = lynx::peq::hzValue (1000.0);
        s.params[(std::size_t) lynx::peq::param (3, 2)] = lynx::peq::gainValue (12.0);
    };
    boost (slot);
    CHECK (std::abs (lynx::peq::responseDb (slot.params, 48000.0, 1000.0) - 12.0) < 0.2);
    CHECK (std::abs (lynx::peq::responseDb (slot.params, 48000.0, 100.0)) < 1.0);
    const auto peak = runEffectOnSine (lynx::EffectKind::parametricEq, boost, 1000.0, 0.1f);
    CHECK (std::abs (peak - 0.1f * std::pow (10.0f, 12.0f / 20.0f)) < 0.01f);

    // low cut at 48 dB/oct at 200 Hz: 50 Hz (2 octaves below) drops ~96 dB; 2 kHz passes
    auto cut = lynx::makeEffectSlot (lynx::EffectKind::parametricEq);
    cut.params[(std::size_t) lynx::peq::param (0, 0)] = lynx::peq::typeValue (lynx::peq::BandType::lowCut);
    cut.params[(std::size_t) lynx::peq::param (0, 1)] = lynx::peq::hzValue (200.0);
    cut.params[(std::size_t) lynx::peq::param (0, 3)] = lynx::peq::qValue (0.70710678); // Butterworth
    cut.params[(std::size_t) (lynx::peq::slopeParam + 0)] = 1.0f;
    CHECK (lynx::peq::responseDb (cut.params, 48000.0, 50.0) < -80.0);
    CHECK (std::abs (lynx::peq::responseDb (cut.params, 48000.0, 200.0) + 3.0) < 0.5); // -3 dB at the cutoff (Butterworth)
    CHECK (std::abs (lynx::peq::responseDb (cut.params, 48000.0, 2000.0)) < 0.1);

    // round-trip conversions
    CHECK (std::abs (lynx::peq::hz (lynx::peq::hzValue (440.0)) - 440.0) < 0.5);
    CHECK (std::abs (lynx::peq::q (lynx::peq::qValue (2.5)) - 2.5) < 0.01);
    CHECK (lynx::peq::type (lynx::peq::typeValue (lynx::peq::BandType::notch)) == lynx::peq::BandType::notch);
}

TEST (fl_parametric_eq2_state_maps_to_bands)
{
    // v8 blob: band 1 low cut at 100 Hz "Steep 8", band 4 peak +6 dB at 1 kHz, output −3 dB
    std::vector<std::uint8_t> blob (354, 0);
    auto put = [&] (std::size_t off, std::int32_t v) { std::memcpy (blob.data() + off, &v, 4); };
    put (0, 8);
    auto param = [&] (int i, std::int32_t v) { put (4 + 4 * (std::size_t) i, v); };
    auto hzRaw = [] (double hz) { return (std::int32_t) std::lround (std::log (hz / 20.0) / std::log (1000.0) * 65536.0); };
    for (int b = 0; b < 7; ++b)
        param (21 + b, 0); // all off
    param (21 + 0, 3);          // high pass (= low cut)
    param (7 + 0, hzRaw (100.0));
    param (14 + 0, 40124);      // default width -> Q ~0.68
    param (28 + 0, -3);         // Steep 8 -> 4 biquads
    param (21 + 3, 6);          // peaking
    param (7 + 3, hzRaw (1000.0));
    param (3, 600);             // +6 dB
    param (14 + 3, 25412);
    param (35, -300);           // main level −3 dB
    put (0x13E, 0x7F);

    const auto slot = lynx::effectFromFl ("Fruity Parametric EQ 2", blob);
    CHECK (slot.has_value() && slot->kind == lynx::EffectKind::parametricEq);
    const auto& p = slot->params;
    CHECK (lynx::peq::type (p[(std::size_t) lynx::peq::param (0, 0)]) == lynx::peq::BandType::lowCut);
    CHECK (lynx::peq::flLaw (p) && lynx::peqfl::fromParams (p).order[0] == -3); // FL's own order (REA_rodada50)
    CHECK (lynx::peq::type (p[(std::size_t) lynx::peq::param (3, 0)]) == lynx::peq::BandType::peak);
    CHECK (lynx::peq::type (p[(std::size_t) lynx::peq::param (1, 0)]) == lynx::peq::BandType::off);
    CHECK (std::abs (lynx::peq::responseDb (p, 48000.0, 1000.0) - 3.0) < 0.3);   // +6 from the peak −3 from the output
    CHECK (lynx::peq::responseDb (p, 48000.0, 30.0) < -40.0);                     // 48 dB/oct below 100 Hz
    CHECK (std::abs (lynx::peq::responseDb (p, 48000.0, 8000.0) + 3.0) < 0.3);

    // a band muted by the mask doesn't sound (FL's mask travels with the slot, REA_rodada50)
    put (0x13E, 0x7F & ~(1 << 3));
    const auto muted = lynx::effectFromFl ("Fruity Parametric EQ 2", blob);
    CHECK ((lynx::peqfl::fromParams (muted->params).mask & (1u << 3)) == 0);
    CHECK (std::abs (lynx::peq::responseDb (muted->params, 48000.0, 1000.0) + 3.0) < 0.3);
}

TEST (reverb_low_cut_and_predelay)
{
    // impulse: with 100 ms predelay the wet only starts later; with a 3 kHz low cut a
    // 100 Hz sine barely enters the reverb
    auto run = [] (const std::function<void (lynx::EffectSlot&)>& tweak, double hz, bool impulse) {
        auto fx = lynx::makeEffect (lynx::EffectKind::reverb);
        fx->prepare (48000.0);
        auto slot = lynx::makeEffectSlot (lynx::EffectKind::reverb);
        slot.params[4] = 0.0f; // wet only
        slot.params[3] = 1.0f;
        tweak (slot);
        std::vector<float> l (48000), r (48000);
        for (int i = 0; i < 48000; ++i)
            l[(std::size_t) i] = r[(std::size_t) i] = impulse ? (i == 0 ? 1.0f : 0.0f) : 0.5f * (float) std::sin (6.283185307 * hz * i / 48000.0);
        for (int i = 0; i < 48000; i += 480)
            fx->process (l.data() + i, r.data() + i, 480, slot.params, 120.0);
        return l;
    };
    const auto delayed = run ([] (auto& s) { s.params[7] = 0.1f; }, 0.0, true);
    float early = 0.0f, later = 0.0f;
    for (std::size_t i = 0; i < 4800; ++i)
        early = std::max (early, std::abs (delayed[i]));
    for (std::size_t i = 4800; i < 24000; ++i)
        later = std::max (later, std::abs (delayed[i]));
    CHECK (early == 0.0f && later > 0.0f);

    auto energy = [] (const std::vector<float>& b) { double e = 0.0; for (std::size_t i = 24000; i < 48000; ++i) e += (double) b[i] * b[i]; return e; };
    const auto open = energy (run ([] (auto&) {}, 100.0, false));
    const auto cut = energy (run ([] (auto& s) { s.params[5] = 1.0f; }, 100.0, false));
    CHECK (cut < open * 0.01);
}

TEST (utility_effect_and_fl_stereo_tools)
{
    // transparent by default; balance fully right silences the left
    CHECK (std::abs (runEffectOnSine (lynx::EffectKind::utility, [] (auto&) {}, 440.0, 0.5f) - 0.5f) < 0.001f);
    CHECK (runEffectOnSine (lynx::EffectKind::utility, [] (auto& s) { s.params[1] = 1.0f; }, 440.0, 0.5f) < 0.001f);

    // Stereo Shaper at half the knob range (6400): each gain = (3^0.5 − 1)/2 (FL's knob law, REA_rodada54 §2.2)
    std::vector<std::uint8_t> blob (36, 0);
    auto put = [&] (std::size_t off, std::int32_t v) { std::memcpy (blob.data() + off, &v, 4); };
    put (4, 6400); put (8, 6400); put (12, 6400); put (16, 6400);
    const auto slot = lynx::effectFromFl ("Fruity Stereo Shaper", blob);
    CHECK (slot && slot->kind == lynx::EffectKind::stereoShaper);
    auto shaperFx = lynx::makeEffect (*slot);
    shaperFx->prepare (44100.0);
    float zl = 0.0f, zr = 0.0f;
    shaperFx->process (&zl, &zr, 0, slot->params, 120.0); // takes the controls
    shaperFx->reset(); // FL's Flush: the gains jump to their targets
    float l = 1.0f, r = 0.0f;
    shaperFx->process (&l, &r, 1, slot->params, 120.0);
    const auto half = (float) ((std::sqrt (3.0) - 1.0) / 2.0);
    CHECK (std::abs (l - half) < 1e-4f && std::abs (r - half) < 1e-4f);
    auto fx = lynx::makeEffect (lynx::EffectKind::utility);

    // Balance: volume 256 = 0 dB, pan −128 = fully left
    std::vector<std::uint8_t> bal (8, 0);
    std::int32_t pan = -128, vol = 256;
    std::memcpy (bal.data(), &pan, 4);
    std::memcpy (bal.data() + 4, &vol, 4);
    const auto b = lynx::effectFromFl ("Fruity Balance", bal);
    CHECK (b && b->kind == lynx::EffectKind::fruityBalance); // FL's exact law (REA_rodada58 §2): fully left sums R into L
    if (b)
    {
        auto balance = lynx::makeEffect (*b);
        balance->prepare (44100.0);
        l = 0.5f; r = 0.5f;
        balance->process (&l, &r, 1, b->params, 120.0);
        CHECK (std::abs (l - 0.70710678f) < 1e-6f && r == 0.0f);
    }
}

TEST (effects_eq_compressor_behave)
{
    // EQ at default is transparent
    CHECK (std::abs (runEffectOnSine (lynx::EffectKind::eq, [] (auto&) {}, 1000.0, 0.5f) - 0.5f) < 0.01f);
    // +18 dB on the lows: 60 Hz rises a lot, 5 kHz barely
    CHECK (runEffectOnSine (lynx::EffectKind::eq, [] (auto& s) { s.params[0] = 1.0f; }, 60.0, 0.05f) > 0.3f);
    CHECK (runEffectOnSine (lynx::EffectKind::eq, [] (auto& s) { s.params[0] = 1.0f; }, 5000.0, 0.05f) < 0.06f);
    // compressor: threshold -20 dB, ratio 20:1 flattens a 0 dBFS signal to near the threshold
    const auto squashed = runEffectOnSine (lynx::EffectKind::compressor, [] (auto& s) {
        s.params[0] = 40.0f / 60.0f;
        s.params[1] = 1.0f;
    }, 200.0, 1.0f);
    CHECK (squashed < 0.2f && squashed > 0.05f);
    // a signal below the threshold passes intact
    CHECK (std::abs (runEffectOnSine (lynx::EffectKind::compressor, [] (auto& s) { s.params[0] = 1.0f; }, 200.0, 0.3f) - 0.3f) < 0.01f);
}

TEST (effects_delay_echoes_on_tempo_and_reverb_has_tail)
{
    auto delay = lynx::makeEffect (lynx::EffectKind::delay);
    delay->prepare (48000.0);
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::delay);
    slot.params[0] = 3.5f / 8.0f; // index 3 = 1/4 = 0.5 s at 120 BPM
    slot.params[1] = 0.0f;        // no feedback: a single echo
    slot.params[2] = 1.0f;
    slot.params[4] = 0.0f;        // no damping
    std::vector<float> l (48000, 0.0f), r (48000, 0.0f);
    l[0] = r[0] = 1.0f;
    delay->process (l.data(), r.data(), 48000, slot.params, 120.0);
    CHECK (std::abs (l[24000] - 1.0f) < 1e-4f);
    CHECK (std::abs (l[12000]) < 1e-6f);

    auto reverb = lynx::makeEffect (lynx::EffectKind::reverb);
    reverb->prepare (48000.0);
    auto rs = lynx::makeEffectSlot (lynx::EffectKind::reverb);
    rs.params[3] = 1.0f;
    std::fill (l.begin(), l.end(), 0.0f);
    std::fill (r.begin(), r.end(), 0.0f);
    l[0] = r[0] = 1.0f;
    reverb->process (l.data(), r.data(), 48000, rs.params, 120.0);
    float tail = 0.0f;
    for (int i = 4800; i < 9600; ++i) // between 100 and 200 ms after the impulse
        tail = std::max (tail, std::abs (l[(std::size_t) i]));
    CHECK (tail > 1e-4f && tail < 1.0f);
    CHECK (lynx::formatEffectParam (lynx::EffectKind::delay, 0, slot.params[0], 120.0) == "1/4 (500 ms)");
}

TEST (rack_runs_insert_effects_and_reuses_them)
{
    // an insert with EQ at -inf doesn't exist; we use the EQ output at -18 dB to see the effect act
    const auto quiet = renderMixerCase ([] (lynx::Song& s) {
        auto eq = lynx::makeEffectSlot (lynx::EffectKind::eq);
        eq.params[4] = 0.0f; // output -18 dB
        s.mixer[1].effects.push_back (eq);
    });
    CHECK (quiet > 0.55f && quiet < 0.6f); // 0.5 from channel 2 + 0.5 * 0.126 from channel 1

    const auto bypassed = renderMixerCase ([] (lynx::Song& s) {
        auto eq = lynx::makeEffectSlot (lynx::EffectKind::eq);
        eq.params[4] = 0.0f;
        eq.enabled = false;
        s.mixer[1].effects.push_back (eq);
    });
    CHECK (std::abs (bypassed - 1.0f) < 1e-4f);

    auto s = lynx::Song::makeDefault();
    s.mixer[2].effects.push_back (lynx::makeEffectSlot (lynx::EffectKind::reverb));
    auto a = lynx::RackState::build (s, nullptr, 48000.0);
    s.mixer[2].effects[0].params[0] = 0.9f; // turning a knob doesn't recreate the effect
    auto b = lynx::RackState::build (s, a.get(), 48000.0);
    CHECK (b->effects[2][0] == a->effects[2][0]);
    s.mixer[2].effects[0] = lynx::makeEffectSlot (lynx::EffectKind::delay);
    auto c = lynx::RackState::build (s, b.get(), 48000.0);
    CHECK (c->effects[2][0] != b->effects[2][0]);
}

TEST (audio_clips_trigger_cut_and_resume)
{
    lynx::Song s;
    s.bpm = 120.0; // 1 beat = 0.5 s = 24000 samples at 48 kHz
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::audioClip;
    c.samplePath = "ramp";
    c.volume = 1.0f;
    s.channels.push_back (c);
    s.patterns.emplace_back();

    lynx::Clip clip;
    clip.kind = lynx::ClipKind::audio;
    clip.channel = 0;
    clip.start = lynx::ppq;      // starts at beat 2
    clip.length = 2 * lynx::ppq; // lasts 2 beats
    clip.offset = lynx::ppq;     // skips the first beat of the audio
    s.playlist.push_back (clip);
    s.mode = lynx::PlayMode::song;

    const auto tps = lynx::ticksPerSample (120.0, 48000.0);
    lynx::EventBuffer ev;
    lynx::scheduleSong (s, 0.0, 4 * 24000, tps, ev);
    CHECK (ev.size() == 2);
    CHECK (ev[0].type == lynx::NoteEvent::Type::on && ev[0].sampleOffset == 24000 && std::abs (ev[0].startSeconds - 0.5) < 1e-9);
    CHECK (ev[1].type == lynx::NoteEvent::Type::off && ev[1].sampleOffset == 3 * 24000);

    // seek to the middle of the clip (beat 2.5): with resume, starts at the block's first sample
    ev.clear();
    lynx::scheduleSong (s, 1.5 * lynx::ppq, 256, tps, ev, true);
    CHECK (ev.size() == 1 && ev[0].sampleOffset == 0 && std::abs (ev[0].startSeconds - 0.75) < 1e-9);
    ev.clear();
    lynx::scheduleSong (s, 1.5 * lynx::ppq, 256, tps, ev, false);
    CHECK (ev.size() == 0);

    // sample whose value = position in seconds: lets us check where playback started
    auto provider = [] (const std::string&) {
        auto d = std::make_shared<lynx::SampleData>();
        d->sampleRate = 48000.0;
        for (int i = 0; i < 48000 * 3; ++i)
            d->left.push_back ((float) i / 48000.0f);
        return std::shared_ptr<const lynx::SampleData> (d);
    };
    auto state = lynx::RackState::build (s, nullptr, 48000.0, provider);
    lynx::RackRenderer r;
    r.prepare (48000.0, 512);
    std::vector<float> l (256), rr (256);

    // first block already playing mid-clip
    r.process (state.get(), true, 1.5 * lynx::ppq, 256, l.data(), rr.data());
    const auto unity = 1.0f / lynx::channelGain (10000.0f / 12800.0f); // clip level at 100% channel volume (REA_rodada89 §4.2)
    CHECK (std::abs (l[0] - 0.75f * unity) < 0.002f);

    // the clip ends at beat 3: after the short fade, silence (the sample would still have audio)
    double tick = 3.0 * lynx::ppq - 10 * tps;
    r.process (state.get(), false, tick, 256, l.data(), rr.data()); // stops (fade)
    for (int i = 0; i < 4; ++i)
        r.process (state.get(), false, tick, 256, l.data(), rr.data());
    CHECK (std::abs (l[255]) < 1e-6f);
}

TEST (remove_channel_fixes_notes_and_audio_clips)
{
    lynx::Song s;
    s.channels.resize (3);
    s.patterns.resize (1);
    s.patterns[0].notes = { { 0, 240, 0, 60, 1.0f }, { 0, 240, 1, 60, 1.0f }, { 0, 240, 2, 60, 1.0f } };
    lynx::Clip a;
    a.kind = lynx::ClipKind::audio;
    a.channel = 1;
    a.length = 100;
    lynx::Clip b = a;
    b.channel = 2;
    s.playlist = { a, b };
    s.removeChannel (1);
    CHECK (s.channels.size() == 2);
    CHECK (s.patterns[0].notes.size() == 2 && s.patterns[0].notes[1].channel == 1);
    CHECK (s.playlist.size() == 1 && s.playlist[0].channel == 1);
}

TEST (flp_import_audio_clips_with_offsets)
{
    flp::Project p;
    p.header.ppq = 96;
    p.tempo = 120.0;
    p.channels = { { 0, flp::ChannelType::audioClip, "Vocal", "", "%FLStudioData%\\v.wav", 2 },
                   { 1, flp::ChannelType::audioClip, "Loop", "", "%FLStudioData%\\l.wav", 3 } };
    flp::Arrangement arr;
    flp::PlaylistItem beats; // offset in beats: end - start = duration in beats
    beats.kind = flp::PlaylistItem::Kind::channelClip;
    beats.index = 0;
    beats.position = 192;
    beats.length = 384;          // 4 beats
    beats.startOffset = 2.0f;
    beats.endOffset = 6.0f;
    flp::PlaylistItem ms = beats; // offset in ms: 4 beats at 120 BPM = 2000 ms
    ms.index = 1;
    ms.startOffset = 500.0f;
    ms.endOffset = 2500.0f;
    arr.items = { beats, ms };
    p.arrangements.push_back (arr);

    lynx::FlpImportOptions options;
    options.roots = { "C:\\FL", "C:\\FL\\Data", "" };
    options.fileExists = [] (const std::string&) { return true; };
    const auto s = lynx::importFlp (p, options);

    CHECK (s.channels.size() == 2 && s.channels[0].instrument == lynx::InstrumentKind::audioClip);
    CHECK (s.channels[0].samplePath == "C:\\FL\\Data\\v.wav");
    CHECK (s.playlist.size() == 2);
    CHECK (s.playlist[0].kind == lynx::ClipKind::audio && s.playlist[0].start == 2 * lynx::ppq);
    CHECK (s.playlist[0].offset == 2 * lynx::ppq);
    CHECK (s.playlist[1].offset == lynx::ppq); // 500 ms at 120 BPM = 1 beat
    CHECK (s.mode == lynx::PlayMode::song);
}

TEST (plugin_factories_create_reuse_and_fallback)
{
    lynx::Song s;
    s.mixer = lynx::Song::defaultMixer (2);
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::plugin;
    c.plugin.id = "VST3-Synth-123";
    c.mixerTrack = 1;
    s.channels.push_back (c);
    lynx::ChannelDef missing = c;
    missing.plugin.id = "VST3-Ausente-0";
    s.channels.push_back (missing);
    lynx::EffectSlot fx;
    fx.kind = lynx::EffectKind::plugin;
    fx.plugin.id = "VST3-Reverb-9";
    s.mixer[1].effects.push_back (fx);
    s.patterns.emplace_back();

    int instrumentsMade = 0, effectsMade = 0;
    lynx::RackState::Factories f;
    f.pluginInstruments = [&] (const lynx::PluginRef& ref, double) -> std::shared_ptr<lynx::Instrument> {
        if (ref.id != "VST3-Synth-123")
            return nullptr;
        ++instrumentsMade;
        auto synth = lynx::makeInstrument (lynx::InstrumentKind::synth);
        synth->prepare (48000.0);
        return synth;
    };
    f.pluginEffects = [&] (const lynx::PluginRef&, double) -> std::shared_ptr<lynx::Effect> {
        ++effectsMade;
        auto eq = lynx::makeEffect (lynx::EffectKind::eq);
        eq->prepare (48000.0);
        return eq;
    };

    auto a = lynx::RackState::build (s, nullptr, 48000.0, f);
    CHECK (instrumentsMade == 1 && effectsMade == 1);
    CHECK (a->instruments.size() == 2 && a->instruments[1] != nullptr); // missing: muted, but it exists

    // changing the saved state doesn't recreate; swapping the plugin does
    s.channels[0].plugin.state = "abc";
    auto b = lynx::RackState::build (s, a.get(), 48000.0, f);
    CHECK (instrumentsMade == 1 && b->instruments[0] == a->instruments[0] && b->effects[1][0] == a->effects[1][0]);
    s.mixer[1].effects[0].plugin.id = "VST3-Outro-1";
    auto c2 = lynx::RackState::build (s, b.get(), 48000.0, f);
    CHECK (effectsMade == 2);

    // rendering with a missing plugin and a plugin slot breaks nothing
    lynx::RackRenderer r;
    r.prepare (48000.0, 256);
    lynx::LiveEventQueue live;
    lynx::NoteEvent on;
    on.channel = 0;
    on.velocity = 1.0f;
    live.push (on);
    on.channel = 1;
    live.push (on);
    std::vector<float> l (256), rr (256);
    r.process (c2.get(), false, 0.0, 256, l.data(), rr.data(), &live);
    float peak = 0.0f;
    for (auto v : l)
    {
        CHECK (std::isfinite (v));
        peak = std::max (peak, std::abs (v));
    }
    CHECK (peak > 0.0f);
}

TEST (export_song_adds_silent_tail_and_never_restarts)
{
    auto s = lynx::Song::makeDefault();
    s.playlist.clear();
    std::int64_t length = 0;
    auto pat = lynx::makeExportSong (s, true, lynx::ppq * 8, length); // empty Playlist: uses the pattern
    CHECK (length == lynx::ppq * 4);
    CHECK (pat.mode == lynx::PlayMode::song && pat.playlist.size() == 2);
    CHECK (pat.songLength() >= length + lynx::ppq * 8);

    // no new notes after the song ends, only silence until the end of the tail
    lynx::EventBuffer events;
    const auto tps = lynx::ticksPerSample (pat.bpm, 48000.0);
    int onsAfterEnd = 0, onsBefore = 0;

    for (double tick = 0.0; tick < (double) (length + lynx::ppq * 8); tick += 512 * tps)
    {
        events.clear();
        lynx::scheduleSong (pat, tick, 512, tps, events);
        for (const auto& e : events)
            if (e.type == lynx::NoteEvent::Type::on)
                (tick + e.sampleOffset * tps >= (double) length ? onsAfterEnd : onsBefore)++;
    }

    CHECK (onsBefore == (int) s.patterns[0].notes.size());
    CHECK (onsAfterEnd == 0);

    // with Playlist: the end is the last clip's, without rounding
    s.playlist.push_back ({ 0, 0, lynx::ppq * 2, lynx::ppq * 3, 0 });
    auto song = lynx::makeExportSong (s, true, 0, length);
    CHECK (length == lynx::ppq * 5 && song.playlist.size() == 1);
    song = lynx::makeExportSong (s, false, 0, length);
    CHECK (length == lynx::ppq * 4 && song.playlist[0].start == 0);
}

TEST (note_edit_selection_operations)
{
    namespace ne = lynx::noteedit;
    lynx::Pattern p;
    p.notes = { { 0, 240, 0, 60 }, { 480, 240, 0, 64 }, { 960, 480, 1, 67 } };

    // sorting keeps the indices in step
    p.notes[0].start = 1200;
    std::vector<int> tracked { 0, 2 };
    ne::sortTracking (p, tracked);
    CHECK (p.notes[2].start == 1200 && p.notes[(std::size_t) tracked[0]].start == 1200);
    CHECK (p.notes[(std::size_t) tracked[1]].key == 67);

    // group move respects the limits
    std::int64_t dt = -5000;
    int dk = 100;
    ne::move (p, { 0, 1 }, dt, dk);
    CHECK (dt == -480 && dk == 127 - 67 && p.notes[0].start == 0 && p.notes[1].key == 127);

    // duplicate: right after the selection, rounded to the grid
    lynx::Pattern q;
    q.notes = { { 0, 240, 0, 60 }, { 240, 300, 0, 62 } }; // 540-tick selection
    const auto added = ne::duplicate (q, { 0, 1 }, 960);
    CHECK (added.size() == 2 && q.notes[(std::size_t) added[0]].start == 960 && q.notes[(std::size_t) added[1]].start == 1200);

    // paste onto another channel with an offset
    const auto pasted = ne::paste (q, ne::copy (q, { 1 }), 100, 3);
    CHECK (pasted.size() == 1 && q.notes.back().channel == 3 && q.notes.back().start == 340);

    // quantize rounds to the nearest
    q.notes[0].start = 130;
    q.notes[1].start = 100;
    ne::quantize (q, { 0, 1 }, 240);
    CHECK (q.notes[0].start == 240 && q.notes[1].start == 0);

    // remove several (order and duplicates don't matter)
    const auto before = q.notes.size();
    ne::remove (q, { 1, 0, 1 });
    CHECK (q.notes.size() == before - 2);
}

TEST (note_edit_quick_operations)
{
    namespace ne = lynx::noteedit;
    const std::int64_t step = lynx::ticksPerStep, bar = lynx::ppq * 4;

    // split: only strictly inside; the tail is appended and does not slide again
    lynx::Pattern p;
    p.notes = { { 0, 4 * step, 0, 60 } };
    p.notes[0].slide = true;
    CHECK (ne::split (p, 0, 0) == -1 && ne::split (p, 0, 4 * step) == -1);
    CHECK (ne::split (p, 0, step) == 1 && p.notes[0].length == step && p.notes[1].start == step && p.notes[1].length == 3 * step
           && ! p.notes[1].slide);

    // chop to grid: a 2.5-step note from half a step becomes 3 pieces cut on the step lines
    lynx::Pattern c;
    c.notes = { { step / 2, step * 5 / 2, 0, 60 }, { 0, step, 1, 50 } };
    const auto pieces = ne::chop (c, { 0, 1 }, step);
    CHECK (pieces.size() == 4 && c.notes.size() == 4);
    CHECK (c.notes[0].length == step / 2 && c.notes[2].start == step && c.notes[2].length == step && c.notes[3].start == 2 * step
           && c.notes[3].length == step);

    // join: touching and overlapping notes of the same key and channel merge; others stay
    lynx::Pattern j;
    j.notes = { { 0, step, 0, 60 }, { step, step, 0, 60 }, { step * 3 / 2, step * 2, 0, 60 }, { 5 * step, step, 0, 60 },
                { step, step, 0, 62 }, { step, step, 1, 60 } };
    CHECK (ne::join (j, { 0, 1, 2, 3, 4, 5 }) == 2 && j.notes.size() == 4);
    CHECK (j.notes[0].start == 0 && j.notes[0].length == step * 7 / 2);

    // legato: each note reaches the next start (a chord ends together); the last keeps its length
    lynx::Pattern l;
    l.notes = { { 0, 10, 0, 60 }, { 0, 10, 0, 64 }, { step, 10, 0, 62 }, { 3 * step, 10, 0, 65 }, { step, 10, 1, 40 } };
    ne::legato (l, { 0, 1, 2, 3, 4 });
    CHECK (l.notes[0].length == step && l.notes[1].length == step && l.notes[2].length == 2 * step && l.notes[3].length == 10
           && l.notes[4].length == 10);

    // shrink to grid and full quantise (start and end)
    ne::shrinkToGrid (l, { 3 }, step / 2);
    CHECK (l.notes[3].length == step / 2);
    lynx::Pattern q;
    q.notes = { { 10, step - 30, 0, 60 }, { step + 100, 20, 0, 60 } };
    ne::quantizeFull (q, { 0, 1 }, step);
    CHECK (q.notes[0].start == 0 && q.notes[0].length == step && q.notes[1].start == step && q.notes[1].length == step);

    // rotate inside the bar, wrapping
    lynx::Pattern r;
    r.notes = { { 0, step, 0, 60 }, { bar - step, step, 0, 62 } };
    ne::rotate (r, { 0, 1 }, step, bar, 1);
    CHECK (r.notes[0].start == step && r.notes[1].start == 0);
    ne::rotate (r, { 0, 1 }, step, bar, -2);
    CHECK (r.notes[0].start == bar - step && r.notes[1].start == bar - 2 * step);
    r.notes.push_back ({ bar + step, step, 0, 64 }); // alone in the second bar: wraps inside that bar
    ne::rotate (r, { 2 }, step, bar, -2);
    CHECK (r.notes[2].start == 2 * bar - step);
}

TEST (note_edit_grid_and_time_text)
{
    namespace ne = lynx::noteedit;
    const std::int64_t step = lynx::ticksPerStep, bar = lynx::ppq * 4;
    CHECK (ne::snapDown (250.0, step) == step && ne::snapDown (-1.0, step) == -step && ne::snapDown (17.9, 1) == 17);
    CHECK (ne::snapNearest (1000.0, { 400, 1100, 980 }, 50.0, step) == 980);
    CHECK (ne::snapNearest (1000.0, { 400 }, 50.0, step) == 960); // too far: falls back to the grid
    // screen grid: 24 px per step shows steps; zoomed out it climbs to beats, bars and bar multiples
    CHECK (ne::screenStep (24.0 / step, bar) == step);
    CHECK (ne::screenStep (96.0 / bar, bar) == lynx::ppq);
    CHECK (ne::screenStep (8.0 / bar, bar) == 2 * bar);
    CHECK (ne::screenStep (200.0 / step, bar) == step / 4);

    CHECK (ne::formatTime (0, 4) == "1:1:0" && ne::formatTime (bar + 5 * step + 7, 4) == "2:6:7");
    CHECK (ne::formatTime (step * 2, 4, true) == "0:2:0");
    std::int64_t t = 0;
    CHECK (ne::parseTime ("2:6:7", 4, t) && t == bar + 5 * step + 7);
    CHECK (ne::parseTime ("3", 4, t) && t == 2 * bar);
    CHECK (ne::parseTime ("0:2:0", 4, t, true) && t == 2 * step);
    CHECK (! ne::parseTime ("0:1:0", 4, t) && ! ne::parseTime ("1:17:0", 4, t) && ! ne::parseTime ("1:1:240", 4, t)
           && ! ne::parseTime ("a", 4, t) && ! ne::parseTime ("", 4, t));
    CHECK (ne::parseTime ("1:12:0", 3, t) && ! ne::parseTime ("1:13:0", 3, t)); // 3/4: 12 steps per bar
}

TEST (playlist_clip_edges_and_time_selection)
{
    namespace ne = lynx::noteedit;
    const std::int64_t beat = lynx::ppq;

    // left-edge trim: content stays in place, cannot reveal what is before the content's start
    lynx::Clip c { 0, 0, 4 * beat, 4 * beat, beat };
    ne::trimStart (c, 5 * beat, beat);
    CHECK (c.start == 5 * beat && c.offset == 2 * beat && c.length == 3 * beat);
    ne::trimStart (c, 0, beat);
    CHECK (c.start == 3 * beat && c.offset == 0 && c.length == 5 * beat);
    ne::trimStart (c, 100 * beat, beat);
    CHECK (c.length == beat && c.end() == 8 * beat);

    // slip moves the content, not the clip
    lynx::Clip s { 0, 0, 0, 2 * beat, beat };
    ne::slip (s, beat / 2);
    CHECK (s.offset == beat / 2 && s.start == 0 && s.length == 2 * beat);
    ne::slip (s, beat);
    CHECK (s.offset == 0);
    ne::slip (s, -3 * beat);
    CHECK (s.offset == 3 * beat);

    // insert / delete time across all tracks, markers included
    std::vector<lynx::Clip> clips { { 0, 0, 0, 2 * beat, 0 }, { 0, 1, 4 * beat, 2 * beat, 0 }, { 0, 2, beat, 6 * beat, 0 } };
    std::vector<lynx::Marker> markers { { 5 * beat, "A" }, { beat / 2, "B" } };
    ne::insertTime (clips, markers, 2 * beat, 4 * beat);
    CHECK (clips[0].start == 0 && clips[1].start == 8 * beat && clips[2].start == beat && markers[0].tick == 9 * beat
           && markers[1].tick == beat / 2);

    std::vector<lynx::Clip> d { { 0, 0, 0, 2 * beat, 0 },             // before: kept
                                { 0, 1, 3 * beat, beat, 0 },          // inside: deleted
                                { 0, 2, beat, 6 * beat, 0 },          // spans the cut: head + tail
                                { 0, 3, 3 * beat, 4 * beat, 0 },      // tail outside: trimmed and moved
                                { 0, 4, 8 * beat, beat, 0 } };        // after: moved left
    std::vector<lynx::Marker> dm { { 3 * beat, "in" }, { 9 * beat, "after" } };
    ne::deleteTime (d, dm, 2 * beat, 5 * beat);
    CHECK (d.size() == 5);
    CHECK (d[1].track == 2 && d[1].start == beat && d[1].length == beat);
    CHECK (d[2].track == 2 && d[2].start == 2 * beat && d[2].offset == 4 * beat && d[2].length == 2 * beat);
    CHECK (d[3].track == 3 && d[3].start == 2 * beat && d[3].offset == 2 * beat && d[3].length == 2 * beat);
    CHECK (d[4].start == 5 * beat && dm.size() == 1 && dm[0].tick == 6 * beat);

    // time selection: overlapping clips; Ctrl+B repeats what starts inside it after itself
    std::vector<lynx::Clip> r { { 0, 0, 0, beat, 0 }, { 0, 1, beat, 2 * beat, 0 }, { 0, 0, 4 * beat, beat, 0 } };
    CHECK ((ne::clipsInTime (r, beat / 2, 2 * beat) == std::vector<int> { 0, 1 }));
    const auto added = ne::repeatTime (r, 0, 4 * beat, {});
    CHECK (added.size() == 2 && r[3].start == 4 * beat && r[4].start == 5 * beat && r[4].track == 1);
    CHECK (ne::repeatTime (r, 0, 2 * beat, { 2 }).size() == 1 && r.back().start == 6 * beat);
}

TEST (recorder_builds_notes_in_pattern_and_song_mode)
{
    auto song = lynx::Song::makeDefault();
    const auto loop = song.patterns[0].effectiveLength (4); // 1 bar = 3840
    lynx::NoteRecorder r;
    r.add (song, { 100.0, 4, 60, 0.5f, true }); // no take: ignored
    CHECK (r.takeFinished().empty());

    r.begin (loop);
    r.add (song, { 4000.0, 4, 60, 0.5f, true });   // second loop pass
    r.add (song, { 4480.0, 4, 60, 0.0f, false });
    r.add (song, { 5000.0, 4, 64, 0.9f, true });   // held until stop
    r.end (song, 5240.0);
    const auto notes = r.takeFinished();
    CHECK (notes.size() == 2);
    CHECK (notes[0].start == 4000 - 3840 && notes[0].length == 480 && notes[0].velocity == 0.5f && notes[0].channel == 4);
    CHECK (notes[1].key == 64 && notes[1].start == 5000 - 3840 && notes[1].length == 240);

    // song mode: position inside the current pattern's clip (with offset)
    song.mode = lynx::PlayMode::song;
    song.playlist = { { 0, 0, 7680, 3840, 960 } };
    CHECK (lynx::patternPositionForTick (song, 7680.0 + 100.0, loop) == 1060);
    CHECK (lynx::patternPositionForTick (song, 100.0, loop) == 100); // outside the clip: like loop
}

TEST (automation_curve_and_playback)
{
    lynx::Automation a;
    a.points = { { 0, 0.0f }, { 960, 1.0f }, { 1920, 0.5f, 0.0f } };
    CHECK (a.valueAt (-10.0) == 0.0f && a.valueAt (480.0) == 0.5f && a.valueAt (5000.0) == 0.5f);
    CHECK (a.length() == 1920);
    a.points[0].tension = 1.0f; // curve: starts slow
    CHECK (a.valueAt (480.0) < 0.2f && a.valueAt (480.0) > 0.0f);

    // automated Master: silence in the first bar, sound in the second, and the last value
    // keeps applying after the automation clip ends
    auto song = lynx::Song::makeDefault();
    song.bpm = 120.0;
    song.mode = lynx::PlayMode::song;
    const auto bar = (std::int64_t) lynx::ppq * 4;
    for (int b = 0; b < 3; ++b)
        song.playlist.push_back ({ 0, 0, bar * b, bar, 0 });
    lynx::Automation master;
    master.target = { lynx::AutomationTargetKind::mixerVolume, 0 };
    master.points = { { 0, 0.0f }, { bar - 1, 0.0f }, { bar, 0.8f } };
    song.automations.push_back (master);
    lynx::Clip ac;
    ac.kind = lynx::ClipKind::automation;
    ac.automation = 0;
    ac.track = 1;
    ac.length = bar + bar / 2;
    song.playlist.push_back (ac);

    const double sr = 48000.0;
    auto state = lynx::RackState::build (song, nullptr, sr);
    CHECK (state->hasAutomation && state->mixerVolumeAuto[0] == 0 && state->automationClips[0].size() == 1);

    lynx::RackRenderer r;
    r.prepare (sr, 512);
    std::vector<float> l (512), rr (512);
    const auto tps = lynx::ticksPerSample (song.bpm, sr);
    float peakBar1 = 0.0f, peakBar2 = 0.0f, peakBar3 = 0.0f;

    for (double tick = 0.0; tick < (double) (bar * 3); tick += 512 * tps)
    {
        r.process (state.get(), true, tick, 512, l.data(), rr.data());
        auto& peak = tick < (double) bar - 512 * tps ? peakBar1 : tick < (double) bar * 2 ? peakBar2 : peakBar3;
        for (auto v : l)
            peak = std::max (peak, std::abs (v));
    }

    CHECK (peakBar1 < 1.0e-6f);
    CHECK (peakBar2 > 0.05f);
    CHECK (peakBar3 > 0.05f); // after the clip: keeps the last value

    // target removed: automation disappears from the tables
    song.removeChannel (0);
    song.automations[0].target = { lynx::AutomationTargetKind::channelVolume, 0 };
    song.removeChannel (0);
    CHECK (song.automations[0].target.kind == lynx::AutomationTargetKind::none);
    song.removeAutomation (0);
    CHECK (song.automations.empty() && song.playlist.size() == 3);
}

TEST (mixer_sends_between_inserts)
{
    // insert 1 goes only to 3; 3 goes to the Master: same sum (0.5 + 0.5)
    CHECK (std::abs (renderMixerCase ([] (lynx::Song& s) { s.setRoute (1, 3, true); s.setRoute (1, 0, false); }) - 1.0f) < 1e-4f);

    // send at 50% and muted destination
    CHECK (std::abs (renderMixerCase ([] (lynx::Song& s) { s.setRoute (1, 3, true, 0.5f); s.setRoute (1, 0, false); }) - 0.75f) < 1e-4f);
    CHECK (std::abs (renderMixerCase ([] (lynx::Song& s) { s.setRoute (1, 3, true); s.setRoute (1, 0, false); s.mixer[3].muted = true; }) - 0.5f) < 1e-4f);

    // going to the Master and to 3: the signal doubles
    CHECK (std::abs (renderMixerCase ([] (lynx::Song& s) { s.setRoute (1, 3, true); }) - 1.5f) < 1e-4f);

    // solo on 3 lets what reaches it sound (insert 1), but not 2
    CHECK (std::abs (renderMixerCase ([] (lynx::Song& s) { s.setRoute (1, 3, true); s.setRoute (1, 0, false); s.mixer[3].solo = true; }) - 0.5f) < 1e-4f);
    // solo on 1 lets the path to the Master (3) sound
    CHECK (std::abs (renderMixerCase ([] (lynx::Song& s) { s.setRoute (1, 3, true); s.setRoute (1, 0, false); s.mixer[1].solo = true; }) - 0.5f) < 1e-4f);

    // order and cycles
    auto song = lynx::Song::makeDefault();
    song.setRoute (4, 2, true);
    song.setRoute (2, 1, true);
    CHECK (! song.canRoute (1, 4) && ! song.canRoute (1, 2) && song.canRoute (1, 5));
    song.setRoute (1, 4, true); // ignored: would create a cycle
    CHECK (! song.routes (1, 4));
    const auto order = song.mixerOrder();
    const auto pos = [&] (int t) { return std::find (order.begin(), order.end(), t) - order.begin(); };
    CHECK ((int) order.size() == (int) song.mixer.size() - 1);
    CHECK (pos (4) < pos (2) && pos (2) < pos (1));
    song.setRoute (4, 2, false);
    CHECK (! song.routes (4, 2) && song.routes (4, 0));

    // route levels (FL's route knobs): to inserts and to Master, clamped to the fader range; none without a route
    song.setRoute (3, 5, true);
    song.setRouteLevel (3, 5, 0.5f);
    song.setRouteLevel (3, 0, 1.4f);
    CHECK (song.routeLevel (3, 5) == 0.5f && song.routeLevel (3, 0) == 1.4f && song.mixer[3].masterLevel == 1.4f);
    song.setRouteLevel (3, 0, 9.0f);
    CHECK (song.routeLevel (3, 0) == lynx::maxFaderGain);
    song.setRouteLevel (3, 6, 0.5f); // no such route: ignored
    CHECK (! song.routes (3, 6) && song.routeLevel (3, 6) == 0.0f);
    song.setRoute (3, 0, false);
    CHECK (song.routeLevel (3, 0) == 0.0f);

    // the route to Master's level scales what insert 1 sends there (0.5 + 0.5·0.25)
    CHECK (std::abs (renderMixerCase ([] (lynx::Song& s) { s.setRouteLevel (1, 0, 0.25f); }) - 0.625f) < 1e-4f);
}

namespace
{
// Test effect: delays the signal by `latency` samples and reports that latency (like a lookahead plugin).
class LatentEffect final : public lynx::Effect
{
public:
    explicit LatentEffect (int samples) : latency (samples), line (samples) {}
    void prepare (double) noexcept override {}
    void reset() noexcept override {}
    void process (float* l, float* r, int n, const lynx::EffectParams&, double) noexcept override { line.process (l, r, n); }
    int latencySamples() const noexcept override { return latency; }

private:
    int latency;
    lynx::DelayLine line;
};
} // namespace

TEST (delay_line_mix_into_matches_process_and_drains_tail)
{
    // mixInto = process over source*level, added to the destination; in stretches that cross the ring
    lynx::DelayLine a (37), b (37);
    std::vector<float> sl (300), sr (300), dl (300, 0.5f), dr (300, -0.5f), xl (300), xr (300);
    for (int i = 0; i < 300; ++i)
    {
        sl[(std::size_t) i] = (float) std::sin (i * 0.1);
        sr[(std::size_t) i] = (float) std::cos (i * 0.07);
    }
    for (int start = 0; start < 300; start += 100)
    {
        b.mixInto (sl.data() + start, sr.data() + start, 0.25f, dl.data() + start, dr.data() + start, 100);
        for (int i = start; i < start + 100; ++i)
        {
            xl[(std::size_t) i] = sl[(std::size_t) i] * 0.25f;
            xr[(std::size_t) i] = sr[(std::size_t) i] * 0.25f;
        }
        a.process (xl.data() + start, xr.data() + start, 100);
    }
    float worst = 0.0f;
    for (int i = 0; i < 300; ++i)
        worst = std::max ({ worst, std::abs (dl[(std::size_t) i] - 0.5f - xl[(std::size_t) i]),
                            std::abs (dr[(std::size_t) i] + 0.5f - xr[(std::size_t) i]) });
    CHECK (worst < 1e-6f);

    // silenced source: the tail still in the line comes out, then nothing remains
    std::vector<float> tl (64, 0.0f), tr (64, 0.0f);
    b.mixInto (nullptr, nullptr, 1.0f, tl.data(), tr.data(), 64);
    CHECK (std::abs (tl[0] - sl[263] * 0.25f) < 1e-6f);
    std::vector<float> zl (64, 0.0f), zr (64, 0.0f);
    b.mixInto (nullptr, nullptr, 1.0f, zl.data(), zr.data(), 64);
    CHECK (*std::max_element (zl.begin(), zl.end()) == 0.0f && *std::min_element (zl.begin(), zl.end()) == 0.0f);
}

TEST (limiter_lookahead_holds_ceiling_on_bursts)
{
    // short bursts over a low floor: the sliding maximum must see each peak before it leaves
    auto fx = lynx::makeEffect (lynx::EffectKind::limiter);
    fx->prepare (48000.0);
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::limiter);
    slot.params[2] = 0.0f; // 10 ms release
    std::vector<float> l (48000), r (48000);
    std::uint32_t seed = 1;
    for (int i = 0; i < 48000; ++i)
    {
        seed = seed * 1664525u + 1013904223u;
        const auto noise = ((float) (seed >> 8) / 16777216.0f - 0.5f) * 0.1f;
        l[(std::size_t) i] = noise + ((i % 9973) < 3 ? 3.0f : 0.0f);
        r[(std::size_t) i] = -noise;
    }
    for (int i = 0; i < 48000; i += 333)
        fx->process (l.data() + i, r.data() + i, std::min (333, 48000 - i), slot.params, 120.0);
    float peak = 0.0f, quiet = 0.0f;
    for (int i = 0; i < 48000; ++i)
        peak = std::max (peak, std::abs (l[(std::size_t) i]));
    for (int i = 9973 * 2 + 2000; i < 9973 * 2 + 2400; ++i) // 40 ms after the burst the gain has already recovered
        quiet = std::max (quiet, std::abs (r[(std::size_t) i]));
    const auto ceiling = 1.0f; // the default ceiling is at 0 dBFS or below
    CHECK (peak <= ceiling + 1e-5f);
    CHECK (quiet > 0.03f);
}

TEST (pdc_aligns_paths_through_latent_effects)
{
    // plan: channel 0 -> insert 1 (effect of 100) -> insert 3; channel 1 -> insert 2 -> Master
    lynx::Song s;
    s.mixer = lynx::Song::defaultMixer (4);
    s.channels.resize (3);
    s.channels[0].mixerTrack = 1;
    s.channels[1].mixerTrack = 2;
    s.channels[2].mixerTrack = 3;
    s.mixer[1].effects.push_back (lynx::makeEffectSlot (lynx::EffectKind::eq));
    s.setRoute (1, 3, true);
    s.setRoute (1, 0, false);

    auto plan = lynx::computePdc (s, { 0, 0, 30 }, { {}, { 100 } });
    CHECK (plan.inAlign[1] == 0 && plan.outLat[1] == 100);
    CHECK (plan.inAlign[3] == 100 && plan.channelDelay[2] == 70); // instrument of 30 on insert 3
    CHECK (plan.sendDelay[1][1] == 0);                             // the slowest path isn't delayed
    CHECK (plan.sendDelay[2][0] == 100 && plan.sendDelay[3][0] == 0 && plan.total == 100);

    // bypass zeroes the slot's latency
    s.mixer[1].effects[0].enabled = false;
    CHECK (lynx::computePdc (s, { 0, 0, 30 }, { {}, { 100 } }).total == 30);

    // rendered: two impulses played together come out together
    lynx::Song r;
    r.mixer = lynx::Song::defaultMixer (2);
    for (int i = 0; i < 2; ++i)
    {
        lynx::ChannelDef c;
        c.instrument = lynx::InstrumentKind::sampler;
        c.samplePath = "impulso";
        c.volume = 1.0f;
        c.mixerTrack = i + 1;
        r.channels.push_back (c);
    }
    r.patterns.emplace_back();
    lynx::EffectSlot fx;
    fx.kind = lynx::EffectKind::plugin;
    fx.plugin.id = "latente";
    r.mixer[1].effects.push_back (fx);

    lynx::RackState::Factories f;
    f.samples = [] (const std::string&) {
        auto d = std::make_shared<lynx::SampleData>();
        d->left.assign (64, 0.0f);
        d->left[0] = 1.0f;
        return std::shared_ptr<const lynx::SampleData> (d);
    };
    f.pluginEffects = [] (const lynx::PluginRef&, double) -> std::shared_ptr<lynx::Effect> { return std::make_shared<LatentEffect> (100); };

    auto state = lynx::RackState::build (r, nullptr, 48000.0, f);
    CHECK (state->pdc.total == 100 && state->sendDelays[2][0] != nullptr && state->sendDelays[2][0]->length() == 100);

    lynx::RackRenderer renderer;
    renderer.prepare (48000.0, 256);
    lynx::LiveEventQueue live;
    for (int ch = 0; ch < 2; ++ch)
    {
        lynx::NoteEvent on;
        on.channel = ch;
        on.key = 60;
        on.velocity = 1.0f;
        live.push (on);
    }

    std::vector<float> l (256), rr (256);
    renderer.process (state.get(), false, 0.0, 256, l.data(), rr.data(), &live);
    int first = -1;
    for (int i = 0; i < 256 && first < 0; ++i)
        if (std::abs (l[(std::size_t) i]) > 0.01f)
            first = i;
    CHECK (first == 100 && std::abs (l[100] - 2.0f) < 1e-3f); // both impulses arrive together, summed

    // editing without changing latencies reuses the lines
    r.channels[0].volume = 0.5f;
    auto next = lynx::RackState::build (r, state.get(), 48000.0, f);
    CHECK (next->sendDelays[2][0] == state->sendDelays[2][0]);
}

TEST (mixer_mute_fades_instead_of_clicking)
{
    lynx::Song s;
    s.mixer = lynx::Song::defaultMixer (2);
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::sampler;
    c.samplePath = "dc";
    c.volume = 1.0f;
    c.mixerTrack = 1;
    s.channels.push_back (c);
    s.patterns.emplace_back();

    auto provider = [] (const std::string&) {
        auto d = std::make_shared<lynx::SampleData>();
        d->left.assign (48000, 0.5f);
        return std::shared_ptr<const lynx::SampleData> (d);
    };

    auto on = lynx::RackState::build (s, nullptr, 48000.0, provider);
    s.mixer[1].muted = true;
    auto off = lynx::RackState::build (s, on.get(), 48000.0, provider);

    lynx::RackRenderer r;
    r.prepare (48000.0, 256);
    lynx::LiveEventQueue live;
    lynx::NoteEvent note;
    note.velocity = 1.0f;
    live.push (note);

    std::vector<float> l (256), rr (256);
    r.process (on.get(), false, 0.0, 256, l.data(), rr.data(), &live);
    CHECK (std::abs (l[255] - 0.5f) < 1e-3f);
    r.process (off.get(), false, 0.0, 256, l.data(), rr.data());
    // the fader glides down by 0.001 per sample, as FL's (REA_rodada85 §3): 1000 samples from 1 to 0
    CHECK (l[0] > 0.49f && std::abs (l[128] - 0.5f * (1.0f - 0.129f)) < 1e-3f && std::abs (l[255] - 0.5f * (1.0f - 0.256f)) < 1e-3f);
    for (int b = 0; b < 3; ++b)
        r.process (off.get(), false, 0.0, 256, l.data(), rr.data());
    CHECK (std::abs (l[255]) < 1e-6f); // down to zero after 1000 samples
    r.process (off.get(), false, 0.0, 256, l.data(), rr.data());
    CHECK (std::abs (l[0]) < 1e-9f);
}

TEST (dither_quantizes_to_lsb_grid_with_small_error)
{
    lynx::Dither d (16);
    std::vector<float> x (4800);
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = 0.25f * std::sin ((float) i * 0.05f);
    auto y = x;
    d.process (y.data(), (int) y.size(), 0);

    double errorSum = 0.0;
    bool onGrid = true;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        const auto steps = (y[i] - d.writerBias()) * 32768.0f; // half an LSB above the grid point (round 160)
        onGrid = onGrid && std::abs (steps - std::round (steps)) < 1e-3f;
        errorSum += std::abs (y[i] - x[i]);
    }
    CHECK (onGrid);
    CHECK (errorSum / (double) x.size() < 4.0 / 32768.0); // a few LSBs of noise on average
}

TEST (parallel_rendering_matches_sequential)
{
    // 32 channels (worth parallelizing), inserts with chained sends
    auto song = lynx::Song::makeDefault();
    song.mixer = lynx::Song::defaultMixer (16);

    for (int i = 5; i < 32; ++i)
    {
        lynx::ChannelDef c;
        c.instrument = i % 2 ? lynx::InstrumentKind::synth : lynx::InstrumentKind::hat;
        c.mixerTrack = 1 + i % 12;
        c.volume = 0.3f;
        song.channels.push_back (c);

        for (int step = i % 4; step < 16; step += 3)
            song.patterns[0].notes.push_back ({ (std::int64_t) step * lynx::ticksPerStep, 200, i, 48 + i, 0.7f });
    }

    song.patterns[0].sortNotes();
    song.setRoute (3, 7, true, 0.6f);
    song.setRoute (7, 9, true, 0.5f);
    song.setRoute (2, 9, true);
    song.mixer[9].effects.push_back (lynx::makeEffectSlot (lynx::EffectKind::reverb));

    auto seqState = lynx::RackState::build (song, nullptr, 48000.0);
    auto parState = lynx::RackState::build (song, nullptr, 48000.0);
    CHECK (parState->parallel && parState->mixerLayers.size() == 3);

    lynx::WorkerPool pool (3);
    lynx::RackRenderer seq, par;
    seq.prepare (48000.0, 512);
    par.prepare (48000.0, 512);
    par.setWorkerPool (&pool);

    std::vector<float> l1 (512), r1 (512), l2 (512), r2 (512);
    const auto tps = lynx::ticksPerSample (song.bpm, 48000.0);
    bool same = true;
    float peak = 0.0f;

    for (int block = 0; block < 200; ++block)
    {
        seq.process (seqState.get(), true, block * 512 * tps, 512, l1.data(), r1.data());
        par.process (parState.get(), true, block * 512 * tps, 512, l2.data(), r2.data());
        same = same && l1 == l2 && r1 == r2;
        for (auto v : l1)
            peak = std::max (peak, std::abs (v));
    }

    CHECK (same);
    CHECK (peak > 0.05f);

    // the pool handles many small rounds in a row
    std::atomic<int> sum { 0 };
    auto add = [&sum] (int i) { sum.fetch_add (i + 1); };
    for (int round = 0; round < 2000; ++round)
        pool.parallelFor (5, add);
    CHECK (sum.load() == 2000 * 15);
}

TEST (rea165i_test_profile_and_test_mode_parsing)
{
    using namespace lynx::testprofile;
    CHECK (! profileDirFrom (nullptr));
    CHECK (! profileDirFrom (""));
    CHECK (! profileDirFrom ("relative/dir")); // only absolute folders
    CHECK (! profileDirFrom ("C:relative"));
    CHECK (profileDirFrom ("/tmp/lynx-profile/") == std::string ("/tmp/lynx-profile"));
    CHECK (profileDirFrom ("  \"C:\\Users\\me\\bench profile\\\" ") == std::string ("C:\\Users\\me\\bench profile"));
    CHECK (profileDirFrom ("D:/x") == std::string ("D:/x"));
    CHECK (profileDirFrom ("C:\\") == std::string ("C:\\")); // a drive root keeps its separator
    CHECK (! testModeFrom (nullptr));
    CHECK (! testModeFrom (""));
    CHECK (! testModeFrom ("0"));
    CHECK (! testModeFrom (" Off "));
    CHECK (! testModeFrom ("false"));
    CHECK (testModeFrom ("1"));
    CHECK (testModeFrom ("yes"));
}

TEST (arrangements_switch_and_keep_clip_indices)
{
    auto s = lynx::Song::makeDefault();
    s.patterns.emplace_back().name = "B";
    s.playlist = { { 1, 0, 0, 960, 0 } };
    s.loopEnd = 960;
    CHECK (s.arrangements.size() == 1 && s.currentArrangement == 0);

    const auto second = s.addArrangement ("Versão curta", true);
    CHECK (second == 1 && s.currentArrangement == 1 && s.playlist.size() == 1 && s.loopEnd == 960);
    s.playlist.push_back ({ 0, 1, 960, 960, 0 });
    s.loopEnd = 0;

    s.selectArrangement (0);
    CHECK (s.playlist.size() == 1 && s.loopEnd == 960 && s.arrangements[1].clips.size() == 2);

    // removing pattern 0 fixes the clips stored in the other arrangement
    s.removePattern (0);
    CHECK (s.playlist.size() == 1 && s.playlist[0].pattern == 0);
    CHECK (s.arrangements[1].clips.size() == 1 && s.arrangements[1].clips[0].pattern == 0);

    s.removeArrangement (0);
    CHECK (s.arrangements.size() == 1 && s.currentArrangement == 0 && s.playlist.size() == 1 && s.loopEnd == 0);
}

namespace
{
// Test effect that inverts the signal and counts how many times it processed.
class CountingInverter final : public lynx::Effect
{
public:
    void prepare (double) noexcept override {}
    void reset() noexcept override {}
    void process (float* l, float* r, int n, const lynx::EffectParams&, double) noexcept override
    {
        ++calls;
        for (int i = 0; i < n; ++i)
        {
            l[i] = -l[i];
            r[i] = -r[i];
        }
    }
    int calls = 0;
};
} // namespace

TEST (keyboard_controller_outputs_key_value)
{
    std::vector<std::uint8_t> blob (554, 0);
    const std::uint32_t version = 3;
    std::memcpy (blob.data(), &version, 4);
    const float key60 = 0.7f, off = 0.2f;
    std::memcpy (blob.data() + 12 + 60 * 4, &key60, 4);
    std::memcpy (blob.data() + 548, &off, 4);
    blob[552] = 1; // auto release
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::controller;
    c.controllerState = blob;
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::controller);
    inst->prepare (48000.0);
    inst->configure (c, 120.0);
    std::vector<float> l (64), r (64);
    inst->noteOn (60, 1.0f);
    inst->render (l.data(), r.data(), 64, 1.0f, 1.0f);
    CHECK (std::abs (inst->controlOutput (0) - 0.7f) < 1e-6f);
    inst->noteOff (60);
    CHECK (std::abs (inst->controlOutput (0) - 0.2f) < 1e-6f);
}

TEST (envelope_controller_ducks_on_each_note)
{
    // a unipolar articulator like "Basic Sidechain": base 0.05, drops to 0 on the note and rises to 1 in 0.3 s
    lynx::ControllerArticulator a;
    a.envOn = true;
    a.base = 0.05f;
    a.envLevel = 1.0f;
    a.count = 3;
    a.time = { 0.0f, 0.2f, 0.3f };
    a.level = { 0.0f, 0.75f, 1.0f };
    CHECK (std::abs (lynx::controllerValue (a, 0.0, -1.0) - 0.05f) < 1e-4f);
    CHECK (std::abs (lynx::controllerValue (a, 0.1, -1.0) - 0.425f) < 1e-3f);
    CHECK (lynx::controllerValue (a, 1.0, -1.0) > 0.99f);  // no sustain: runs to the end and stays
    a.sustain = 1;
    CHECK (std::abs (lynx::controllerValue (a, 1.0, -1.0) - 0.8f) < 1e-3f);  // held at sustain
    CHECK (lynx::controllerValue (a, 1.0, 0.5) > 0.99f);                     // released: continues to the end
    a.bipolar = true;
    a.base = 0.5f;
    a.sustain = -1;
    CHECK (std::abs (lynx::controllerValue (a, 0.0, -1.0) - 0.0f) < 1e-4f); // bipolar: 0.5 + (0 − 0.5)
}

TEST (peak_controller_drives_live_automation)
{
    // track 1: source (DC 0.5) with Peak Controller; track 2: DC 0.5 with volume linked to the controller
    auto render = [] (bool linked) {
        lynx::Song s;
        s.mixer = lynx::Song::defaultMixer (2);
        for (int t = 1; t <= 2; ++t)
        {
            lynx::ChannelDef c;
            c.instrument = lynx::InstrumentKind::sampler;
            c.samplePath = "dc";
            c.volume = 1.0f;
            c.mixerTrack = t;
            s.channels.push_back (c);
        }
        s.patterns.emplace_back();
        auto peak = lynx::makeEffectSlot (lynx::EffectKind::peakController);
        peak.params[0] = 1.0f; // base 1
        peak.params[1] = 0.0f; // amount −1: 1 − peak
        peak.params[2] = 0.5f; // no tension
        s.mixer[1].effects.push_back (peak);
        if (linked)
        {
            lynx::Automation a;
            a.name = "Peak";
            a.target = { lynx::AutomationTargetKind::mixerVolume, 2 };
            a.source = { 1, 0, 0 };
            s.automations.push_back (a);
        }

        lynx::RackState::Factories f;
        f.samples = [] (const std::string&) {
            auto d = std::make_shared<lynx::SampleData>();
            d->left.assign (48000, 0.5f);
            return std::shared_ptr<const lynx::SampleData> (d);
        };
        auto state = lynx::RackState::build (s, nullptr, 48000.0, f);
        lynx::RackRenderer r;
        r.prepare (48000.0, 512);
        lynx::LiveEventQueue live;
        for (int ch = 0; ch < 2; ++ch)
        {
            lynx::NoteEvent on;
            on.channel = ch;
            on.velocity = 1.0f;
            live.push (on);
        }
        std::vector<float> l (4800), rr (4800);
        r.process (state.get(), false, 0.0, 4800, l.data(), rr.data(), &live);
        return l[4000];
    };

    const auto free = render (false), ducked = render (true);
    CHECK (std::isfinite (ducked) && ducked > 0.0f);
    CHECK (ducked < free - 0.05f); // the 0.5 peak on track 1 lowers track 2's volume
}

TEST (effect_slot_dry_wet_and_smart_disable)
{
    lynx::Song s;
    s.mixer = lynx::Song::defaultMixer (1);
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::sampler;
    c.samplePath = "dc";
    c.volume = 1.0f;
    c.mixerTrack = 1;
    s.channels.push_back (c);
    s.patterns.emplace_back();
    lynx::EffectSlot fx;
    fx.kind = lynx::EffectKind::plugin;
    fx.plugin.id = "inversor";
    fx.mix = 0.25f; // 75% dry (+0.5) and 25% wet (-0.5) = +0.25
    s.mixer[1].effects.push_back (fx);

    auto inverter = std::make_shared<CountingInverter>();
    lynx::RackState::Factories f;
    f.samples = [] (const std::string&) {
        auto d = std::make_shared<lynx::SampleData>();
        d->left.assign (4800, 0.5f);
        return std::shared_ptr<const lynx::SampleData> (d);
    };
    f.pluginEffects = [inverter] (const lynx::PluginRef&, double) -> std::shared_ptr<lynx::Effect> { return inverter; };
    auto state = lynx::RackState::build (s, nullptr, 48000.0, f);

    lynx::RackRenderer r;
    r.prepare (48000.0, 512);
    lynx::LiveEventQueue live;
    lynx::NoteEvent on;
    on.velocity = 1.0f;
    live.push (on);
    std::vector<float> l (480), rr (480);
    r.process (state.get(), false, 0.0, 480, l.data(), rr.data(), &live);
    CHECK (std::abs (l[100] - 0.25f) < 1e-4f);

    // after the sample ends: 4 s of silence and the plugin stops being called
    for (int i = 0; i < 100 * 6; ++i) // 6 s
        r.process (state.get(), false, 0.0, 480, l.data(), rr.data());
    const auto callsAfterSilence = inverter->calls;
    for (int i = 0; i < 100; ++i)
        r.process (state.get(), false, 0.0, 480, l.data(), rr.data());
    CHECK (inverter->calls == callsAfterSilence);
    CHECK (callsAfterSilence < 100 * 6);

    // sound again: processing resumes in the same block
    live.push (on);
    r.process (state.get(), false, 0.0, 480, l.data(), rr.data(), &live);
    CHECK (inverter->calls == callsAfterSilence + 1 && std::abs (l[100] - 0.25f) < 1e-4f);
}

TEST (swing_delays_offbeat_sixteenths)
{
    CHECK (lynx::swingDelay (0, 1.0f) == 0);
    CHECK (lynx::swingDelay (lynx::ticksPerStep, 1.0f) == lynx::ticksPerStep / 2);
    CHECK (lynx::swingDelay (lynx::ticksPerStep * 2, 1.0f) == 0);
    CHECK (lynx::swingDelay (lynx::ticksPerStep + 10, 1.0f) == (lynx::ticksPerStep - 20) / 2); // off the grid: triangular weight (FL)
    CHECK (lynx::swingDelay (lynx::ticksPerStep, 1.0f, 0.5f) == lynx::ticksPerStep / 4); // channel mix
    CHECK (lynx::swingDelay (lynx::ticksPerStep * 3, 0.5f) == lynx::ticksPerStep / 4);

    lynx::Pattern p;
    p.notes = { { 0, 100, 0, 60 }, { lynx::ticksPerStep, 100, 0, 62 } };
    lynx::EventBuffer ev;
    const auto tps = lynx::ticksPerSample (120.0, 48000.0);
    lynx::schedulePatternLoop (p, p.effectiveLength (4), 0.0, 12000, tps, ev, 1.0f);
    int onOffset = -1;
    for (auto& e : ev)
        if (e.type == lynx::NoteEvent::Type::on && e.key == 62)
            onOffset = e.sampleOffset;
    CHECK (onOffset == (int) std::floor ((lynx::ticksPerStep * 1.5) / tps));
}

TEST (sampler_settings_pitch_reverse_envelope_cut)
{
    auto ramp = std::make_shared<lynx::SampleData>();
    for (int i = 0; i < 48000; ++i)
        ramp->left.push_back ((float) i / 48000.0f); // ramp: the value tells where the read position is
    ramp->sampleRate = 48000.0;

    auto play = [&] (const lynx::SamplerSettings& s, int key, int frames, bool releaseAfterHalf) {
        auto inst = lynx::makeInstrument (lynx::InstrumentKind::sampler, ramp);
        inst->prepare (48000.0);
        lynx::ChannelDef c;
        c.sampler = s;
        inst->configure (c, 120.0);
        inst->noteOn (key, 1.0f);
        std::vector<float> l ((std::size_t) frames), r ((std::size_t) frames);
        inst->render (l.data(), r.data(), frames / 2, 1.0f, 1.0f);
        if (releaseAfterHalf)
            inst->noteOff (key);
        inst->render (l.data() + frames / 2, r.data() + frames / 2, frames - frames / 2, 1.0f, 1.0f);
        return l;
    };

    // +12 semitones: reads twice as fast
    lynx::SamplerSettings up;
    up.pitch = 12.0f;
    CHECK (std::abs (play (up, 60, 1000, false)[999] - 2.0f * 999.0f / 48000.0f) < 1e-3f);

    // reversed: starts from the end
    lynx::SamplerSettings back;
    back.reverse = true;
    CHECK (play (back, 60, 100, false)[10] > 0.99f);

    // 10 ms attack: starts silent and reaches full level
    lynx::SamplerSettings slow;
    slow.attack = 0.01f;
    const auto a = play (slow, 60, 2000, false);
    CHECK (a[10] < 0.2f * (10.0f / 48000.0f) + 1e-6f && std::abs (a[1500] - 1500.0f / 48000.0f) < 1e-4f);

    // 5 ms release: gone right after release; without release it plays in full
    lynx::SamplerSettings rel;
    rel.release = 0.005f;
    CHECK (play (rel, 60, 2000, true)[1999] == 0.0f);
    CHECK (play (lynx::SamplerSettings {}, 60, 2000, true)[1999] > 0.0f);

    // cut itself: the second note cuts the first (one voice remains)
    lynx::SamplerSettings cut;
    cut.cut = true;
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::sampler, ramp);
    inst->prepare (48000.0);
    lynx::ChannelDef cutChannel;
    cutChannel.sampler = cut;
    inst->configure (cutChannel, 120.0);
    inst->noteOn (60, 1.0f);
    std::vector<float> l (2000), r (2000);
    inst->render (l.data(), r.data(), 1000, 1.0f, 1.0f);
    inst->noteOn (60, 1.0f);
    std::fill (l.begin(), l.end(), 0.0f);
    inst->render (l.data(), r.data(), 2000, 1.0f, 1.0f);
    CHECK (std::abs (l[1999] - 1999.0f / 48000.0f) < 1e-4f); // only the new note
}

TEST (playlist_split_and_track_mute)
{
    lynx::Clip c { 0, 3, 960, 3840, 480 };
    const auto rest = lynx::splitClip (c, 2000);
    CHECK (c.start == 960 && c.length == 1040 && c.offset == 480);
    CHECK (rest.start == 2000 && rest.length == 2800 && rest.offset == 480 + 1040 && rest.track == 3);

    auto s = lynx::Song::makeDefault();
    s.mode = lynx::PlayMode::song;
    s.playlist = { { 0, 0, 0, 3840, 0 } };
    lynx::EventBuffer ev;
    const auto tps = lynx::ticksPerSample (120.0, 48000.0);
    lynx::scheduleSong (s, 0.0, 48000, tps, ev);
    CHECK (ev.size() > 0);
    s.playlistTracks.resize (1);
    s.playlistTracks[0].muted = true;
    ev.clear();
    lynx::scheduleSong (s, 0.0, 48000, tps, ev);
    CHECK (ev.size() == 0);
}

TEST (note_pan_pitch_and_mute)
{
    lynx::Pattern p;
    lynx::Note a { 0, 240, 0, 60 };
    a.pan = -1.0f;
    a.finePitch = 50.0f;
    lynx::Note b { 240, 240, 0, 62 };
    b.muted = true;
    p.notes = { a, b };
    lynx::EventBuffer ev;
    const auto tps = lynx::ticksPerSample (120.0, 48000.0);
    lynx::schedulePatternLoop (p, p.effectiveLength (4), 0.0, 48000, tps, ev);
    int ons = 0;
    for (auto& e : ev)
        if (e.type == lynx::NoteEvent::Type::on)
        {
            ++ons;
            CHECK (e.key == 60 && e.pan == -1.0f && e.cents == 50.0f);
        }
    CHECK (ons == 1); // the muted one doesn't play

    // note pan in the sampler: fully left
    auto dc = std::make_shared<lynx::SampleData>();
    dc->left.assign (4800, 0.5f);
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::sampler, dc);
    inst->prepare (48000.0);
    inst->setNextNoteExpression (-1.0f, 0.0f);
    inst->noteOn (60, 1.0f);
    std::vector<float> l (100), r (100);
    inst->render (l.data(), r.data(), 100, 1.0f, 1.0f);
    CHECK (l[50] > 0.6f && std::abs (r[50]) < 1e-6f);
}

TEST (tempo_automation_changes_speed_and_duration)
{
    auto song = lynx::Song::makeDefault();
    song.bpm = 120.0;
    song.mode = lynx::PlayMode::song;
    const auto bar = (std::int64_t) lynx::ppq * 4;
    song.playlist = { { 0, 0, 0, bar, 0 }, { 0, 0, bar, bar, 0 } };
    lynx::Automation tempo;
    tempo.target = { lynx::AutomationTargetKind::tempo, 0 };
    const auto v120 = lynx::valueFromTempo (120.0), v240 = lynx::valueFromTempo (240.0);
    tempo.points = { { 0, v120 }, { bar - 1, v120 }, { bar, v240 } }; // step: 120 -> 240 at bar 2
    song.automations.push_back (tempo);
    lynx::Clip clip;
    clip.kind = lynx::ClipKind::automation;
    clip.automation = 0;
    clip.track = 1;
    clip.length = bar * 2;
    song.playlist.push_back (clip);

    CHECK (std::abs (song.tempoAt (100.0) - 120.0) < 0.01 && std::abs (song.tempoAt ((double) bar + 100.0) - 240.0) < 0.01);
    // 1 bar at 120 (2 s) + 1 at 240 (1 s)
    CHECK (std::abs (song.secondsForTicks (bar * 2) - 3.0) < 0.01);

    auto state = lynx::RackState::build (song, nullptr, 48000.0);
    CHECK (state->tempoAutomation == 0);
    lynx::RackRenderer r;
    r.prepare (48000.0, 512);
    std::vector<float> l (512), rr (512);
    r.process (state.get(), true, 10.0, 512, l.data(), rr.data());
    CHECK (std::abs (r.currentBpm() - 120.0) < 0.01);
    r.process (state.get(), true, (double) bar + 10.0, 512, l.data(), rr.data());
    CHECK (std::abs (r.currentBpm() - 240.0) < 0.01);
}

TEST (bounce_insert_song_and_apply)
{
    auto s = lynx::Song::makeDefault();
    s.setRoute (3, 7, true, 0.5f);
    s.mixer[0].effects.push_back (lynx::makeEffectSlot (lynx::EffectKind::reverb));
    s.mixer[2].solo = true;

    const auto b = lynx::makeBounceSong (s, 3);
    CHECK (b.mixer[3].solo && ! b.mixer[2].solo && b.mixer[3].toMaster && b.mixer[3].sends.empty());
    CHECK (b.mixer[0].effects.empty() && b.mode == lynx::PlayMode::song);
    const auto audible = b.soloAudible();
    CHECK (audible[3] && ! audible[1] && ! audible[7]);

    const auto channels = s.channels.size();
    const auto inserts = s.mixer.size();
    const auto created = lynx::applyBounce (s, 3, "C:/x.wav", 3840);
    CHECK (created == (int) inserts && s.mixer.size() == inserts + 1 && s.mixer[3].muted);
    CHECK (s.mixer[(std::size_t) created].sends.size() == 1 && s.mixer[(std::size_t) created].sends[0].target == 7);
    CHECK (s.channels.size() == channels + 1 && s.channels.back().mixerTrack == created);
    CHECK (s.playlist.back().kind == lynx::ClipKind::audio && s.playlist.back().length == 3840);
}

TEST (mixer_stereo_separation_phase_swap)
{
    lynx::MixerTrack t;
    std::vector<float> l { 1.0f }, r { 0.0f };
    t.stereo = 1.0f; // mono
    lynx::RackRenderer::applyStereoForTest (t, l.data(), r.data(), 1);
    CHECK (std::abs (l[0] - 0.5f) < 1e-6f && std::abs (r[0] - 0.5f) < 1e-6f);

    l = { 1.0f };
    r = { 0.0f };
    t.stereo = -1.0f; // doubled side: 1.5 L - 0.5 R
    lynx::RackRenderer::applyStereoForTest (t, l.data(), r.data(), 1);
    CHECK (std::abs (l[0] - 1.5f) < 1e-6f && std::abs (r[0] + 0.5f) < 1e-6f);

    l = { 1.0f };
    r = { 0.25f };
    t.stereo = 0.0f;
    t.swapChannels = true;
    t.invertPhase = true;
    lynx::RackRenderer::applyStereoForTest (t, l.data(), r.data(), 1);
    CHECK (l[0] == -0.25f && r[0] == -1.0f);
}

TEST (effects_limiter_filter_distortion_chorus)
{
    const double sr = 48000.0;
    auto sine = [] (float hz, float amp, int n) {
        std::vector<float> v ((std::size_t) n);
        for (int i = 0; i < n; ++i)
            v[(std::size_t) i] = amp * std::sin (2.0f * 3.14159265f * hz * (float) i / 48000.0f);
        return v;
    };
    auto peak = [] (const std::vector<float>& v, int from) {
        float p = 0.0f;
        for (std::size_t i = (std::size_t) from; i < v.size(); ++i)
            p = std::max (p, std::abs (v[i]));
        return p;
    };

    // limiter: +12 dB on a 0.9 sine never exceeds the ceiling (-1 dB) and reports the latency
    auto lim = lynx::makeEffect (lynx::EffectKind::limiter);
    lim->prepare (sr);
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::limiter);
    slot.params[0] = 11.0f / 12.0f; // -1 dB
    slot.params[1] = 0.5f;          // +12 dB
    auto l = sine (100.0f, 0.9f, 48000), r = l;
    lim->process (l.data(), r.data(), 48000, slot.params, 120.0);
    CHECK (peak (l, 0) <= std::pow (10.0f, -1.0f / 20.0f) + 1e-4f);
    CHECK (peak (l, 4800) > 0.8f);
    CHECK (lim->latencySamples() == 72);

    // low-pass filter at 500 Hz knocks down an 8 kHz sine
    auto lp = lynx::makeEffect (lynx::EffectKind::filter);
    lp->prepare (sr);
    auto fs = lynx::makeEffectSlot (lynx::EffectKind::filter);
    fs.params[1] = std::log (500.0f / 20.0f) / std::log (1000.0f);
    l = sine (8000.0f, 1.0f, 9600);
    r = l;
    lp->process (l.data(), r.data(), 9600, fs.params, 120.0);
    CHECK (peak (l, 4800) < 0.05f);

    // distortion and chorus: finite, audible output
    for (auto kind : { lynx::EffectKind::distortion, lynx::EffectKind::chorus })
    {
        auto fx = lynx::makeEffect (kind);
        fx->prepare (sr);
        auto ps = lynx::makeEffectSlot (kind);
        l = sine (220.0f, 0.5f, 9600);
        r = l;
        fx->process (l.data(), r.data(), 9600, ps.params, 120.0);
        bool finite = true;
        for (auto v : l)
            finite = finite && std::isfinite (v);
        CHECK (finite && peak (l, 2400) > 0.1f && peak (l, 0) < 2.0f);
    }
}

TEST (slide_notes_glide_without_retrigger)
{
    // sampler with a ramp: the read speed doubles after a one-octave slide
    auto ramp = std::make_shared<lynx::SampleData>();
    for (int i = 0; i < 96000; ++i)
        ramp->left.push_back ((float) i / 96000.0f);
    ramp->sampleRate = 48000.0;
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::sampler, ramp);
    inst->prepare (48000.0);
    inst->noteOn (60, 1.0f);
    std::vector<float> l (2000), r (2000);
    inst->render (l.data(), r.data(), 1000, 1.0f, 1.0f);
    const auto stepBefore = l[999] - l[998];
    inst->slideTo (72, 0.0f, 100.0f);
    std::fill (l.begin(), l.end(), 0.0f);
    inst->render (l.data(), r.data(), 1000, 1.0f, 1.0f);
    const auto stepAfter = l[999] - l[998];
    CHECK (std::abs (stepAfter / stepBefore - 2.0f) < 0.01f);

    // the sequencer sends "slide" (no off) for the slide note
    lynx::Pattern p;
    lynx::Note base { 0, 960, 0, 48 };
    lynx::Note glide { 480, 240, 0, 55 };
    glide.slide = true;
    p.notes = { base, glide };
    lynx::EventBuffer ev;
    const auto tps = lynx::ticksPerSample (120.0, 48000.0);
    lynx::schedulePatternLoop (p, p.effectiveLength (4), 0.0, 30000, tps, ev);
    int ons = 0, offs = 0, slides = 0;
    for (auto& e : ev)
    {
        ons += e.type == lynx::NoteEvent::Type::on;
        offs += e.type == lynx::NoteEvent::Type::off;
        if (e.type == lynx::NoteEvent::Type::slide)
        {
            ++slides;
            CHECK (e.key == 55 && std::abs (e.slideSamples - 240.0f / (float) tps) < 1.0f);
        }
    }
    CHECK (ons == 1 && offs == 1 && slides == 1);
}

TEST (synth_fm_phase_modulation)
{
    auto render = [] (const lynx::SynthSettings& s) {
        auto inst = lynx::makeInstrument (lynx::InstrumentKind::synth);
        inst->prepare (48000.0);
        lynx::ChannelDef c;
        c.synth = s;
        inst->configure (c, 120.0);
        inst->noteOn (69, 1.0f); // 440 Hz
        std::vector<float> l (48000), r (48000);
        inst->render (l.data(), r.data(), 48000, 1.0f, 1.0f);
        return l;
    };
    // energy (Goertzel) at frequency f over frames [from, to)
    auto energy = [] (const std::vector<float>& x, double f, int from, int to) {
        const auto w = 2.0 * 3.141592653589793 * f / 48000.0, k = 2.0 * std::cos (w);
        double s1 = 0.0, s2 = 0.0;
        for (int i = from; i < to; ++i)
        {
            const auto s0 = x[(std::size_t) i] + k * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        return s1 * s1 + s2 * s2 - k * s1 * s2;
    };

    lynx::SynthSettings plain;
    plain.osc[0] = { lynx::Waveform::sine, 0, 0.0f, 1.0f };
    plain.osc[2] = { lynx::Waveform::sine, 0, 0.0f, 1.0f };
    plain.cutoff = 20000.0f;

    auto fm = plain;
    fm.fm = 1.0f;
    const auto a = render (plain), b = render (fm);

    // no FM: osc 3 adds (same pitch) and there's no 2nd harmonic; with FM the sidebands appear
    CHECK (energy (a, 880.0, 4800, 24000) < 1e-3 * energy (a, 440.0, 4800, 24000));
    CHECK (energy (b, 880.0, 4800, 24000) > 0.1 * energy (b, 440.0, 4800, 24000));
    CHECK (energy (b, 1320.0, 4800, 24000) > 0.01 * energy (b, 440.0, 4800, 24000));
    for (auto v : b)
        CHECK (std::isfinite (v) && std::abs (v) < 1.0f);

    // index decaying to 0: the start is bright, the end is nearly a sine again
    auto decaying = fm;
    decaying.fmDecay = 0.1f;
    decaying.fmSustain = 0.0f;
    const auto c = render (decaying);
    const auto early = energy (c, 880.0, 0, 2400) / energy (c, 440.0, 0, 2400);
    const auto late = energy (c, 880.0, 36000, 48000) / energy (c, 440.0, 36000, 48000);
    CHECK (early > 0.05 && late < 1e-3);
}

TEST (synth_three_oscillators_adsr_filter)
{
    auto render = [] (const lynx::SynthSettings& s, int frames, int releaseAt) {
        auto inst = lynx::makeInstrument (lynx::InstrumentKind::synth);
        inst->prepare (48000.0);
        lynx::ChannelDef c;
        c.synth = s;
        inst->configure (c, 120.0);
        inst->noteOn (69, 1.0f); // A4 = 440 Hz
        std::vector<float> l ((std::size_t) frames), r ((std::size_t) frames);
        inst->render (l.data(), r.data(), releaseAt, 1.0f, 1.0f);
        inst->noteOff (69);
        inst->render (l.data() + releaseAt, r.data() + releaseAt, frames - releaseAt, 1.0f, 1.0f);
        return l;
    };
    auto peakIn = [] (const std::vector<float>& v, int from, int to) {
        float p = 0.0f;
        for (int i = from; i < to; ++i)
            p = std::max (p, std::abs (v[(std::size_t) i]));
        return p;
    };

    // default: sounds and fades after the release
    const lynx::SynthSettings standard;
    const auto a = render (standard, 48000, 24000);
    CHECK (peakIn (a, 1000, 20000) > 0.05f && peakIn (a, 40000, 48000) < 1e-3f);

    // pure sine with an open filter: zero-crossing count gives ~440 Hz
    lynx::SynthSettings sine;
    sine.osc[0].wave = lynx::Waveform::sine;
    sine.cutoff = 20000.0f;
    const auto b = render (sine, 48000, 47000);
    int crossings = 0;
    for (int i = 4800; i < 4800 + 48000 / 2; ++i)
        crossings += (b[(std::size_t) i - 1] < 0.0f) != (b[(std::size_t) i] < 0.0f);
    CHECK (std::abs (crossings - 440) < 6); // half a second = 220 cycles = 440 crossings
    float sinePeak = 0.0f;
    for (int i = 4800; i < 9600; ++i)
        sinePeak = std::max (sinePeak, std::abs (b[(std::size_t) i]));
    CHECK (sinePeak > 0.2f && sinePeak < 0.35f); // 0.3 level, approximated sine without clipping

    // second oscillator an octave up adds energy; sustain 0 makes the note decay on its own
    lynx::SynthSettings two = sine;
    two.osc[1] = { lynx::Waveform::sine, 1, 0.0f, 1.0f };
    two.sustain = 0.0f;
    two.decay = 0.1f;
    const auto c = render (two, 48000, 47000);
    CHECK (peakIn (c, 100, 2000) > peakIn (b, 100, 2000) && peakIn (c, 30000, 40000) < 1e-3f);
}

TEST (set_target_value_round_trips)
{
    auto s = lynx::Song::makeDefault();
    s.mixer[2].effects.push_back (lynx::makeEffectSlot (lynx::EffectKind::delay));
    const lynx::AutomationTarget targets[] = {
        { lynx::AutomationTargetKind::channelVolume, 1 }, { lynx::AutomationTargetKind::channelPan, 2 },
        { lynx::AutomationTargetKind::mixerVolume, 3 },   { lynx::AutomationTargetKind::mixerPan, 0 },
        { lynx::AutomationTargetKind::effectParam, 2, 0, 1 }, { lynx::AutomationTargetKind::tempo, 0 },
    };

    for (const auto& t : targets)
    {
        CHECK (s.setTargetValue (t, 0.25f));
        CHECK (std::abs (s.targetValue (t) - 0.25f) < 1e-5f);
    }

    CHECK (! s.setTargetValue ({ lynx::AutomationTargetKind::channelParam, 0, 0, 3 }, 0.5f)); // lives in the plugin
}

TEST (cut_groups_stretch_and_envelope_import)
{
    // open hat (cut by 1) is cut when the closed one (group 1) plays
    lynx::Song s;
    s.mixer = lynx::Song::defaultMixer (2);
    for (int i = 0; i < 2; ++i)
    {
        lynx::ChannelDef c;
        c.instrument = lynx::InstrumentKind::sampler;
        c.samplePath = "dc";
        c.volume = 1.0f;
        c.mixerTrack = i + 1;
        s.channels.push_back (c);
    }
    s.channels[0].cutBy = 1;    // open
    s.channels[1].cutGroup = 1; // closed
    auto& p = s.patterns.emplace_back();
    p.notes = { { 0, 100, 0, 60 }, { 960, 100, 1, 60 } };

    auto provider = [] (const std::string&) {
        auto d = std::make_shared<lynx::SampleData>();
        d->left.assign (96000, 0.5f);
        return std::shared_ptr<const lynx::SampleData> (d);
    };
    auto state = lynx::RackState::build (s, nullptr, 48000.0, provider);
    CHECK (state->hasCutGroups && state->cutTargets[1].size() == 1 && state->cutTargets[1][0] == 0);

    lynx::RackRenderer r;
    r.prepare (48000.0, 512);
    std::vector<float> l (512), rr (512);
    const auto tps = lynx::ticksPerSample (s.bpm, 48000.0);
    float afterCut = 0.0f;
    for (double tick = 0.0; tick < 960.0 * 3; tick += 512 * tps)
    {
        r.process (state.get(), true, tick, 512, l.data(), rr.data());
        if (tick > 960.0 * 2) // the closed one continues; the open one has stopped
            afterCut = std::max (afterCut, std::abs (l[256]));
    }
    CHECK (std::abs (afterCut - 0.5f * 0.78f) < 0.01f); // only the closed one (default velocity 0.78)

    // resampling stretch: 1 s of sample stretched to 4 beats at 120 BPM = 2 s
    auto ramp = std::make_shared<lynx::SampleData>();
    for (int i = 0; i < 48000; ++i)
        ramp->left.push_back ((float) i / 48000.0f);
    ramp->sampleRate = 48000.0;
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::sampler, ramp);
    inst->prepare (48000.0);
    lynx::ChannelDef c;
    c.sampler.stretchBeats = 4.0f;
    inst->configure (c, 120.0);
    inst->noteOn (60, 1.0f);
    std::vector<float> a (48000), b (48000);
    inst->render (a.data(), b.data(), 48000, 1.0f, 1.0f);
    CHECK (std::abs (a[47999] - 0.5f) < 0.01f); // after 1 s it has read half
    // FL's Resample: the key still transposes, like tape (round 162, issue #9): an octave up reads twice as fast
    inst->allNotesOff();
    std::fill (a.begin(), a.end(), 0.0f);
    inst->render (a.data(), b.data(), 4800, 1.0f, 1.0f);
    std::fill (a.begin(), a.end(), 0.0f);
    inst->noteOn (72, 1.0f);
    inst->render (a.data(), b.data(), 24000, 1.0f, 1.0f);
    CHECK (std::abs (a[23999] - 0.5f) < 0.01f);

    // FL envelope time: 32950 = 2 s; < 101 = 0
    CHECK (std::abs (flp::Channel::envelopeTime (32950) - 2.0) < 0.01 && flp::Channel::envelopeTime (50) == 0.0);
}

TEST (time_stretch_keeps_pitch)
{
    lynx::SampleData sine;
    sine.sampleRate = 48000.0;
    for (int i = 0; i < 48000; ++i)
        sine.left.push_back (0.5f * std::sin (2.0f * 3.14159265f * 440.0f * (float) i / 48000.0f));

    auto crossingsPerSecond = [] (const lynx::SampleData& d) {
        int c = 0;
        const auto n = (int) d.frames();
        for (int i = 4800; i < n - 4800; ++i)
            c += (d.left[(std::size_t) i - 1] < 0.0f) != (d.left[(std::size_t) i] < 0.0f);
        return (double) c / ((double) (n - 9600) / d.sampleRate);
    };

    for (double ratio : { 2.0, 0.5, 1.37 })
    {
        const auto out = lynx::timeStretch (sine, ratio);
        CHECK (std::abs ((double) out->frames() - 48000.0 * ratio) < 2.0);
        CHECK (std::abs (crossingsPerSecond (*out) - 880.0) < 15.0); // 440 Hz stays 440 Hz

        float peak = 0.0f;
        for (std::size_t i = 4800; i + 4800 < out->frames(); ++i)
            peak = std::max (peak, std::abs (out->left[i]));
        CHECK (peak > 0.4f && peak < 0.6f); // no gaps or clipping in the overlap
    }
}

TEST (sampler_region_start_and_end)
{
    auto ramp = std::make_shared<lynx::SampleData>();
    for (int i = 0; i < 10000; ++i)
        ramp->left.push_back ((float) i / 10000.0f);
    ramp->sampleRate = 48000.0;
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::sampler, ramp);
    inst->prepare (48000.0);
    lynx::ChannelDef c;
    c.sampler.regionStart = 0.25f;
    c.sampler.regionEnd = 0.5f;
    inst->configure (c, 120.0);
    inst->noteOn (60, 1.0f);
    std::vector<float> l (6000), r (6000);
    inst->render (l.data(), r.data(), 6000, 1.0f, 1.0f);
    CHECK (std::abs (l[0] - 0.25f) < 0.001f);   // starts at the beginning of the slice
    CHECK (l[2400] > 0.48f && l[2600] == 0.0f); // and stops at its end
}

TEST (arpeggiator_plays_held_chord_in_steps)
{
    auto song = lynx::Song::makeDefault();
    song.bpm = 120.0;
    song.patterns[0].notes.clear();
    for (int key : { 60, 64, 67 })
        song.patterns[0].notes.push_back ({ 0, lynx::ppq * 4, 4, key, 0.8f }); // chord held for 1 bar
    song.channels[4].arp.direction = 1; // up
    song.channels[4].arp.stepBeats = 0.25f;

    auto state = lynx::RackState::build (song, nullptr, 48000.0);
    CHECK (state->arpeggiators[4] != nullptr && state->arpeggiators[0] == nullptr);

    // the arpeggiator generates the notes; we count what it hands to the instrument
    auto& arp = *state->arpeggiators[4];
    const auto tps = lynx::ticksPerSample (song.bpm, 48000.0);
    std::vector<int> keys;
    lynx::EventBuffer ev;

    for (double tick = 0.0; tick < lynx::ppq * 2.0; tick += 512 * tps)
    {
        ev.clear();
        lynx::schedulePatternLoop (song.patterns[0], song.patterns[0].effectiveLength (4), tick, 512, tps, ev);
        int in = 0, out = 0;
        for (auto& e : ev)
            if (e.channel == 4)
                arp.input[(std::size_t) in++] = e;
        arp.process (4, arp.input.data(), in, tick, tps, 512, song.channels[4].arp, arp.output.data(), out);
        for (int i = 0; i < out; ++i)
            if (arp.output[(std::size_t) i].type == lynx::NoteEvent::Type::on)
                keys.push_back (arp.output[(std::size_t) i].key);
    }

    // 2 beats = 8 sixteenth steps: 60 64 67 60 64 67 60 64
    CHECK (keys.size() >= 8); // the last block goes slightly past 2 beats
    const int expected[] = { 60, 64, 67, 60, 64, 67, 60, 64 };
    for (std::size_t i = 0; i < keys.size() && i < 8; ++i)
        CHECK (keys[i] == expected[i]);

    // actually rendering also makes sound
    lynx::RackRenderer r;
    r.prepare (48000.0, 512);
    std::vector<float> l (512), rr (512);
    float peak = 0.0f;
    for (double tick = 0.0; tick < lynx::ppq; tick += 512 * tps)
    {
        r.process (state.get(), true, tick, 512, l.data(), rr.data());
        for (auto v : l)
            peak = std::max (peak, std::abs (v));
    }
    CHECK (peak > 0.01f);
}

TEST (audio_clip_fades_and_gain)
{
    lynx::Song s;
    s.bpm = 120.0;
    s.mode = lynx::PlayMode::song;
    s.mixer = lynx::Song::defaultMixer (1);
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::audioClip;
    c.samplePath = "dc";
    c.volume = 1.0f;
    c.mixerTrack = 1;
    s.channels.push_back (c);
    s.patterns.emplace_back();
    lynx::Clip clip;
    clip.kind = lynx::ClipKind::audio;
    clip.channel = 0;
    clip.length = lynx::ppq * 4;  // 2 s at 120 BPM
    clip.fadeIn = lynx::ppq;      // 0.5 s
    clip.fadeOut = lynx::ppq;
    clip.gain = 0.5f;
    s.playlist.push_back (clip);

    auto provider = [] (const std::string&) {
        auto d = std::make_shared<lynx::SampleData>();
        d->left.assign (48000 * 4, 1.0f);
        return std::shared_ptr<const lynx::SampleData> (d);
    };
    auto state = lynx::RackState::build (s, nullptr, 48000.0, provider);
    lynx::RackRenderer r;
    r.prepare (48000.0, 512);
    std::vector<float> all;
    std::vector<float> l (512), rr (512);
    const auto tps = lynx::ticksPerSample (s.bpm, 48000.0);

    for (double tick = 0.0; tick < lynx::ppq * 4.0; tick += 512 * tps)
    {
        r.process (state.get(), true, tick, 512, l.data(), rr.data());
        all.insert (all.end(), l.begin(), l.end());
    }

    // FL plays a clip at unity at the default channel volume (78%): at 100% it is that much louder (REA_rodada89 §4.2)
    const auto unity = 1.0f / lynx::channelGain (10000.0f / 12800.0f);
    CHECK (all[100] < 0.01f);                                       // fade-in start
    CHECK (std::abs (all[12000] - 0.25f * unity) < 0.02f);          // fade-in midpoint: half of 0.5
    CHECK (std::abs (all[48000] - 0.5f * unity) < 0.01f);           // body: the clip gain
    CHECK (std::abs (all[96000 - 12000] - 0.25f * unity) < 0.02f);  // fade-out midpoint
}

TEST (audio_clip_unity_at_fl_default_volume)
{
    // a Playlist audio clip at FL's default channel volume (10000/12800) and centre pan plays its sample unchanged
    auto dc = std::make_shared<lynx::SampleData>();
    dc->left.assign (48000, 0.5f);
    auto clip = lynx::makeInstrument (lynx::InstrumentKind::audioClip, dc);
    clip->prepare (48000.0);
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::audioClip;
    c.volume = 10000.0f / 12800.0f;
    clip->configure (c, 120.0);
    clip->noteOn (60, 1.0f);
    std::vector<float> l (4800, 0.0f), r (4800, 0.0f);
    const auto g = lynx::channelGain (c.volume);
    clip->render (l.data(), r.data(), 4800, g, g);
    CHECK (std::abs (l[2400] - 0.5f) < 1.0e-3f && std::abs (r[2400] - 0.5f) < 1.0e-3f);

    // a sampler channel (pattern notes) keeps the channel volume curve as it was
    auto sampler = lynx::makeInstrument (lynx::InstrumentKind::sampler, dc);
    sampler->prepare (48000.0);
    c.instrument = lynx::InstrumentKind::sampler;
    sampler->configure (c, 120.0);
    sampler->noteOn (60, 1.0f);
    std::fill (l.begin(), l.end(), 0.0f);
    std::fill (r.begin(), r.end(), 0.0f);
    sampler->render (l.data(), r.data(), 4800, g, g);
    CHECK (std::abs (l[2400] - 0.5f * g) < 0.02f);
}

TEST (slicer_detects_hits_and_plays_slices_by_key)
{
    // 4 hits (decaying noise) every 0.25 s
    lynx::SampleData loop;
    loop.sampleRate = 48000.0;
    loop.left.assign (48000, 0.0f);
    lynx::Noise noise;
    for (int hit = 0; hit < 4; ++hit)
        for (int i = 0; i < 6000; ++i)
            loop.left[(std::size_t) (hit * 12000 + i)] = noise.next() * 0.8f * std::exp (-(float) i / 1500.0f);

    const auto slices = lynx::detectSlices (loop, 0.5f);
    CHECK (slices.size() == 4);
    for (std::size_t i = 0; i < slices.size(); ++i)
        CHECK (std::abs (slices[i] - 0.25f * (float) i) < 0.01f);

    // key 61 plays the second slice, which starts at 0.25 and ends before 0.5
    auto ramp = std::make_shared<lynx::SampleData>();
    for (int i = 0; i < 48000; ++i)
        ramp->left.push_back ((float) i / 48000.0f);
    ramp->sampleRate = 48000.0;
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::sampler, ramp);
    inst->prepare (48000.0);
    lynx::ChannelDef c;
    c.sampler.slices = slices;
    inst->configure (c, 120.0);
    inst->noteOn (61, 1.0f);
    std::vector<float> l (20000), r (20000);
    inst->render (l.data(), r.data(), 20000, 1.0f, 1.0f);
    CHECK (std::abs (l[0] - 0.25f) < 0.01f && l[11000] > 0.45f && l[13000] == 0.0f);
}

TEST (project_templates_render_sound)
{
    const auto names = lynx::templateNames();
    CHECK (names.size() == 4);

    for (int t = 1; t < (int) names.size(); ++t)
    {
        auto song = lynx::makeTemplate (t);
        auto state = lynx::RackState::build (song, nullptr, 48000.0);
        lynx::RackRenderer r;
        r.prepare (48000.0, 512);
        std::vector<float> l (512), rr (512);
        const auto tps = lynx::ticksPerSample (song.bpm, 48000.0);
        float peak = 0.0f;

        for (double tick = 0.0; tick < lynx::ppq * 4.0; tick += 512 * tps)
        {
            r.process (state.get(), true, tick, 512, l.data(), rr.data());
            for (auto v : l)
                peak = std::max (peak, std::abs (v));
        }

        CHECK (peak > 0.05f && peak < 4.0f);
    }

    CHECK (lynx::makeTemplate (0).patterns[0].notes.empty());
}

// --- interface translations (resources/i18n) -------------------------------------------------------

namespace i18ntest
{
std::string readFile (const std::filesystem::path& path)
{
    std::ifstream in (path, std::ios::binary);
    return { std::istreambuf_iterator<char> (in), std::istreambuf_iterator<char>() };
}

std::string unescape (const std::string& s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '\\' && i + 1 < s.size())
        {
            const auto n = s[++i];
            out += n == 'n' ? '\n' : n == 't' ? '\t' : n;
        }
        else
            out += s[i];
    }
    return out;
}

// reads a quoted string starting at s[pos] == '"'; returns the raw (escaped) contents and moves pos past the closing quote
bool quoted (const std::string& s, std::size_t& pos, std::string& raw)
{
    if (pos >= s.size() || s[pos] != '"')
        return false;
    raw.clear();
    for (++pos; pos < s.size() && s[pos] != '"' && s[pos] != '\n'; ++pos)
    {
        if (s[pos] == '\\' && pos + 1 < s.size())
            raw += s[pos++];
        raw += s[pos];
    }
    if (pos >= s.size() || s[pos] != '"')
        return false;
    ++pos;
    return true;
}

// JUCE LocalisedStrings format: "english" = "translation"; duplicates are reported in `duplicates`
std::map<std::string, std::string> parseTable (const std::string& text, std::vector<std::string>& duplicates)
{
    std::map<std::string, std::string> table;
    std::istringstream lines (text);
    std::string line;
    while (std::getline (lines, line))
    {
        std::size_t pos = line.find_first_not_of (" \t");
        std::string key, value;
        if (pos == std::string::npos || ! quoted (line, pos, key))
            continue;
        pos = line.find ('"', pos);
        if (pos == std::string::npos || ! quoted (line, pos, value))
            value.clear();
        const auto k = unescape (key);
        if (table.count (k) != 0)
            duplicates.push_back (k);
        table[k] = unescape (value);
    }
    return table;
}

// string literals passed to tr() / TR_NOOP() in the sources (adjacent literals are concatenated)
std::map<std::string, std::string> sourceKeys (const std::filesystem::path& srcDir)
{
    std::map<std::string, std::string> keys;
    for (const auto& entry : std::filesystem::recursive_directory_iterator (srcDir))
    {
        const auto ext = entry.path().extension().string();
        if (! entry.is_regular_file() || (ext != ".cpp" && ext != ".h" && ext != ".inc"))
            continue;

        // drop // comment lines (they may quote tr ("...") as an example)
        std::string text, line;
        std::istringstream in (readFile (entry.path()));
        while (std::getline (in, line))
        {
            const auto first = line.find_first_not_of (" \t");
            text += (first != std::string::npos && line.compare (first, 2, "//") == 0) ? std::string() : line;
            text += '\n';
        }

        auto isIdent = [] (char c) { return std::isalnum ((unsigned char) c) || c == '_'; };
        for (const std::string name : { "tr", "TR_NOOP" })
            for (auto at = text.find (name); at != std::string::npos; at = text.find (name, at + 1))
            {
                if ((at > 0 && isIdent (text[at - 1])) || isIdent (text[at + name.size()]))
                    continue;
                auto pos = text.find_first_not_of (" \t\n", at + name.size());
                if (pos == std::string::npos || text[pos] != '(')
                    continue;
                pos = text.find_first_not_of (" \t\n", pos + 1);
                std::string key, raw;
                bool any = false;
                while (pos != std::string::npos && quoted (text, pos, raw))
                {
                    key += raw;
                    any = true;
                    pos = text.find_first_not_of (" \t\n", pos);
                }
                if (any)
                    keys.emplace (unescape (key), entry.path().filename().string());
            }
    }
    return keys;
}

std::set<std::string> placeholders (const std::string& s)
{
    std::set<std::string> found;
    for (auto open = s.find ('{'); open != std::string::npos; open = s.find ('{', open + 1))
        if (const auto close = s.find ('}', open); close != std::string::npos && close - open <= 3)
            found.insert (s.substr (open, close - open + 1));
    return found;
}
} // namespace i18ntest

TEST (translations_cover_every_ui_string)
{
    using namespace i18ntest;
    const auto text = readFile ("resources/i18n/pt-BR.txt");
    CHECK (! text.empty());

    std::vector<std::string> duplicates;
    const auto table = parseTable (text, duplicates);
    CHECK (table.size() > 300);
    CHECK (duplicates.empty());
    for (const auto& d : duplicates)
        std::printf ("  duplicate pt-BR entry: \"%s\"\n", d.c_str());

    const auto keys = sourceKeys ("src");
    CHECK (keys.size() > 300);

    int missing = 0;
    for (const auto& [key, file] : keys)
        if (auto it = table.find (key); it == table.end() || it->second.empty())
        {
            ++missing;
            std::printf ("  no pt-BR translation for \"%s\" (%s)\n", key.c_str(), file.c_str());
        }
    CHECK (missing == 0);

    // no empty values, and every {n} placeholder survives the translation
    int empty = 0, badPlaceholders = 0;
    for (const auto& [english, portuguese] : table)
    {
        empty += portuguese.empty() ? 1 : 0;
        if (placeholders (english) != placeholders (portuguese))
        {
            ++badPlaceholders;
            std::printf ("  placeholders differ: \"%s\"\n", english.c_str());
        }
    }
    CHECK (empty == 0);
    CHECK (badPlaceholders == 0);
}

TEST (translations_embedded_table_is_up_to_date)
{
    // src/ui/I18nData.h must be regenerated (tools/make_i18n.py) whenever the .txt changes
    const auto text = i18ntest::readFile ("resources/i18n/pt-BR.txt");
    CHECK (text.size() == sizeof (lynx::i18n::data::ptBR));
    CHECK (std::memcmp (text.data(), lynx::i18n::data::ptBR, std::min (text.size(), sizeof (lynx::i18n::data::ptBR))) == 0);
}

// --- workflow shell: pattern/channel operations, key map, workspace layout, control math -------------

TEST (workflow_pattern_navigation_and_insert)
{
    auto song = lynx::Song::makeDefault();
    song.patterns.resize (3);
    song.patterns[1].name = "B";
    song.patterns[2].name = "C";
    song.patterns[2].notes.push_back ({});
    song.currentPattern = 0;
    CHECK (lynx::stepPattern (song, 1) == 1 && lynx::stepPattern (song, -1) == 0 && lynx::stepPattern (song, 10) == 2);
    // pattern 0 has the default notes, 1 is empty, 2 has a note
    CHECK (! lynx::isPatternEmpty (song.patterns[0]) && lynx::isPatternEmpty (song.patterns[1]));
    CHECK (lynx::findNextEmptyPattern (song, 0) == 1 && lynx::findNextEmptyPattern (song, 1) == -1);
    CHECK (lynx::gotoNextEmptyPattern (song) == 1 && song.currentPattern == 1);
    song.patterns[1].notes.push_back ({});
    const auto before = song.patterns.size();
    CHECK (lynx::gotoNextEmptyPattern (song) == (int) before && song.patterns.size() == before + 1);
    CHECK (song.patterns.back().name == "Pattern 1" || song.patterns.back().name.rfind ("Pattern ", 0) == 0);

    // insert keeps Playlist clips on their patterns
    lynx::Clip clip;
    clip.pattern = 2;
    song.playlist = { clip };
    song.arrangements.push_back ({ "Other", { clip }, 0, 0, {} });
    CHECK (lynx::insertPattern (song, 1, "New") == 1 && song.currentPattern == 1);
    CHECK (song.patterns[1].name == "New" && song.patterns[1].notes.empty() && song.patterns[3].name == "C");
    CHECK (song.playlist[0].pattern == 3 && song.arrangements[1].clips[0].pattern == 3);
}

TEST (workflow_split_pattern_by_channel)
{
    auto song = lynx::Song::makeDefault();
    song.patterns.resize (1);
    auto& p = song.patterns[0];
    p.name = "Beat";
    p.notes.clear();
    for (int i = 0; i < 4; ++i)
        p.notes.push_back ({ (std::int64_t) i * lynx::ticksPerStep, lynx::ticksPerStep, i % 2 == 0 ? 0 : 2, 60 });
    song.channels[2].colour = 0xff112233;
    lynx::Clip clip;
    clip.pattern = 0;
    song.playlist = { clip };
    const auto created = lynx::splitPatternByChannel (song, 0);
    CHECK (created == 2 && song.patterns.size() == 3);
    CHECK (song.patterns[0].notes.size() == 4); // the original is unchanged
    CHECK (song.patterns[1].name == "Beat - " + song.channels[0].name && song.patterns[1].notes.size() == 2);
    CHECK (song.patterns[2].colour == 0xff112233u && song.patterns[2].notes.size() == 2 && song.patterns[2].notes[0].channel == 2);
    CHECK (song.playlist[0].pattern == 0);
    CHECK (lynx::splitPatternByChannel (song, 99) == 0);
}

TEST (workflow_channel_selection)
{
    lynx::ChannelSelection s;
    s.selectOnly (2);
    s.toggle (5);
    s.toggle (0);
    CHECK ((s.rows == std::vector<int> { 0, 2, 5 }) && s.contains (5) && ! s.contains (3));
    s.toggle (2);
    CHECK ((s.rows == std::vector<int> { 0, 5 }));
    s.selectOnly (3);
    s.extendTo (6);
    CHECK ((s.rows == std::vector<int> { 3, 4, 5, 6 }));
    s.extendTo (1);
    CHECK ((s.rows == std::vector<int> { 1, 2, 3 }));
    s.selectOnly (4);
    s.toggle (4); // the last one stays
    CHECK ((s.rows == std::vector<int> { 4 }));
    s.selectAll (3);
    CHECK ((s.rows == std::vector<int> { 0, 1, 2 }));
    s.rows = { 7, 9 };
    s.clamp (5);
    CHECK (s.rows.size() == 1 && s.rows[0] < 5);
}

TEST (workflow_move_clone_insert_channels)
{
    auto song = lynx::Song::makeDefault();
    const auto names = [&] {
        std::vector<std::string> n;
        for (const auto& c : song.channels)
            n.push_back (c.name);
        return n;
    };
    const auto original = names();
    const auto count = (int) song.channels.size();
    CHECK (count >= 4);

    // references that must follow their channel
    song.patterns[0].notes.push_back ({ 0, 240, 1, 64 });
    lynx::Automation a;
    a.target = { lynx::AutomationTargetKind::channelVolume, 1 };
    song.automations.push_back (a);
    song.midiLinks.push_back ({ 7, -1, { lynx::AutomationTargetKind::channelPan, 3 } });
    lynx::Clip audio;
    audio.kind = lynx::ClipKind::audio;
    audio.channel = 3;
    song.playlist.push_back (audio);

    std::vector<int> selection { 1, 3 };
    CHECK (lynx::moveChannels (song, selection, -1));
    CHECK ((selection == std::vector<int> { 0, 2 }));
    CHECK (song.channels[0].name == original[1] && song.channels[1].name == original[0] && song.channels[2].name == original[3]
           && song.channels[3].name == original[2]);
    CHECK (song.automations[0].target.index == 0 && song.midiLinks[0].target.index == 2 && song.playlist.back().channel == 2);
    CHECK (std::any_of (song.patterns[0].notes.begin(), song.patterns[0].notes.end(),
                        [] (const lynx::Note& n) { return n.channel == 0 && n.key == 64; }));
    CHECK (! lynx::moveChannels (song, selection, -1)); // the block is at the top
    CHECK (lynx::moveChannels (song, selection, 1) && (selection == std::vector<int> { 1, 3 }) && names() == original);

    // clone: settings, not notes, right below, with a new name
    song.channels[1].volume = 0.33f;
    const auto notesBefore = song.patterns[0].notes.size();
    const auto clone = lynx::cloneChannel (song, 1);
    CHECK (clone == 2 && (int) song.channels.size() == count + 1);
    CHECK (song.channels[2].name == original[1] + " #2" && song.channels[2].volume == 0.33f);
    CHECK (song.patterns[0].notes.size() == notesBefore);
    CHECK (song.channels[3].name == original[2] && song.automations[0].target.index == 1);
    CHECK (lynx::cloneChannel (song, 1) == 2 && song.channels[2].name == original[1] + " #3");

    // remove several
    CHECK (lynx::removeChannels (song, { 2, 3, 2 }) == 2 && (int) song.channels.size() == count);
    CHECK (names() == original);
}

TEST (workflow_send_channels_to_free_inserts)
{
    auto song = lynx::Song::makeDefault();
    for (auto& c : song.channels)
        c.mixerTrack = 0;
    song.mixer[1].effects.push_back (lynx::makeEffectSlot (lynx::EffectKind::delay)); // not free: has effects
    song.channels[0].mixerTrack = 2;                                                   // not free: used
    song.setRoute (5, 3, true);                                                        // 3 is not free: receives a send
    CHECK (! lynx::isInsertFree (song, 1) && ! lynx::isInsertFree (song, 2) && ! lynx::isInsertFree (song, 3) && lynx::isInsertFree (song, 4));
    song.channels[1].colour = 0xff445566;
    const auto tracks = lynx::sendChannelsToFreeInserts (song, { 1, 2 });
    CHECK ((tracks == std::vector<int> { 4, 5 }) || (tracks == std::vector<int> { 4, 6 }));
    CHECK (song.channels[1].mixerTrack == 4 && song.mixer[4].name == song.channels[1].name && song.mixer[4].colour == 0xff445566u);
    CHECK (song.mixer[(std::size_t) tracks[1]].name == song.channels[2].name);

    // with no free insert left, a new one is added at the end
    auto full = lynx::Song::makeDefault();
    full.mixer = lynx::Song::defaultMixer (1);
    full.channels[0].mixerTrack = 1;
    const auto added = lynx::sendChannelsToFreeInserts (full, { 1 });
    CHECK (added.size() == 1 && added[0] == 2 && full.mixer.size() == 3 && full.channels[1].mixerTrack == 2);
}

TEST (workflow_palette_random_colour)
{
    const auto& palette = lynx::lynxPalette();
    for (std::uint32_t seed = 0; seed < 50; ++seed)
    {
        const auto current = palette[seed % palette.size()];
        const auto c = lynx::randomPaletteColour (current, seed);
        CHECK (c != current && std::find (palette.begin(), palette.end(), c) != palette.end());
    }
}

TEST (workflow_key_map)
{
    using namespace lynx::keys;
    KeyMap map;
    const auto global = Context::global, rack = Context::channelRack;
    CHECK (map.find ({ space, none }, global, true) == "transport.playStop");
    CHECK (map.find ({ space, ctrl }, global, true) == "transport.pause");
    CHECK (map.find ({ f6, none }, global, true) == "panel.channelRack");
    CHECK (map.find ({ f8, alt }, global, false) == "panel.browser");
    CHECK (map.find ({ insert, ctrl | shift }, global, false) == "pattern.insert");
    CHECK (map.find ({ numpadPlus, none }, rack, true) == "pattern.next");
    // panel context: only while focused
    CHECK (map.find ({ 'L', ctrl }, rack, true) == "rack.sendToInserts");
    CHECK (map.find ({ 'L', ctrl }, global, true).empty());
    CHECK (map.find ({ up, alt }, rack, false) == "rack.moveUp");
    // typing piano: note keys play, other plain keys only for whilePiano actions; chords always work
    CHECK (KeyMap::playsNote ({ 'Z', none }, true) && ! KeyMap::playsNote ({ 'Z', none }, false) && ! KeyMap::playsNote ({ 'Z', ctrl }, true));
    CHECK (map.find ({ 'L', none }, global, true) == "transport.toggleMode");
    CHECK (map.find ({ '1', none }, rack, false) == "rack.mute1" && map.find ({ '0', none }, rack, false) == "rack.mute10");
    CHECK (map.find ({ '1', none }, rack, true).empty()); // a plain digit is not a shortcut while the piano is on
    CHECK (map.find ({ '2', ctrl }, rack, true) == "rack.solo2");
    CHECK (typingPianoNote ('Z') == 60 && typingPianoNote ('q') == 72 && typingPianoNote ('P') == 88 && typingPianoNote ('L') == -1);
    CHECK (map.conflicts().empty());

    // chord text
    CHECK (toString ({ f4, ctrl | shift }) == "Ctrl+Shift+F4" && toString ({ numpadMinus, none }) == "Numpad -");
    CHECK (parseChord ("Shift+Ctrl+Ins") == (Chord { insert, ctrl | shift }));
    CHECK (parseChord ("alt+up") == (Chord { up, alt }) && parseChord ("k") == (Chord { 'K', none }));
    CHECK (parseChord ("Numpad +") == (Chord { numpadPlus, none }) && ! parseChord ("F13") && ! parseChord ("Ctrl+"));
    for (const auto& a : actions())
        for (const auto& c : a.defaults)
            CHECK (parseChord (toString (c)) == c);

    // JSON overrides
    std::vector<std::string> errors;
    CHECK (map.loadJson (R"({ "pattern.next": "Ctrl+Right", "rack.clone": ["Alt+D", "Ctrl+D"], "help.shortcuts": "", "nope": "F1", "file.open": "Hyper+Q" })", &errors));
    CHECK (errors.size() == 2);
    CHECK (map.find ({ right, ctrl }, global, false) == "pattern.next" && map.find ({ numpadPlus, none }, global, false).empty());
    CHECK (map.find ({ 'D', ctrl }, rack, false) == "rack.clone" && map.find ({ f1, none }, global, false).empty());
    map.setChords ("pattern.previous", { { right, ctrl } });
    CHECK (map.conflicts().size() == 1);
    KeyMap reloaded;
    CHECK (reloaded.loadJson (map.toJson()) && reloaded.chordsFor ("rack.clone") == map.chordsFor ("rack.clone"));
    CHECK (! map.loadJson ("[1, 2]") && ! map.loadJson ("{\"a\": }"));
    map.resetToDefaults();
    CHECK (map.find ({ numpadPlus, none }, global, false) == "pattern.next");
}

TEST (workflow_workspace_layout)
{
    using namespace lynx::workspace;
    auto l = Layout::makeDefault (1280, 620);
    // every visible panel is inside the desktop; the default shows Browser, Playlist and Channel Rack
    for (const auto& p : l.panels)
        CHECK (p.bounds.x >= 0 && p.bounds.y >= 0 && p.bounds.right() <= 1280 && p.bounds.bottom() <= 620 && p.bounds.w >= minWidth);
    CHECK (l[Panel::browser].visible && l[Panel::playlist].visible && l[Panel::channelRack].visible);
    CHECK (! l[Panel::pianoRoll].visible && ! l[Panel::mixer].visible);
    CHECK (l[Panel::playlist].bounds.x == l[Panel::browser].bounds.right() && l[Panel::playlist].bounds.right() == 1280);
    CHECK (l.isFrontmost (Panel::channelRack) && l.focused == Panel::channelRack);

    // toggle: show+focus, focus, hide
    CHECK (l.toggle (Panel::mixer) && l.focused == Panel::mixer && l.isFrontmost (Panel::mixer));
    CHECK (l.toggle (Panel::channelRack) && l.focused == Panel::channelRack && l.isFrontmost (Panel::channelRack));
    CHECK (! l.toggle (Panel::channelRack) && ! l[Panel::channelRack].visible && l.focused == Panel::mixer);
    l.toggleMaximised (Panel::playlist);
    CHECK (l[Panel::playlist].maximised && l.focused == Panel::playlist);

    // save and load at another size: positions scale
    l[Panel::mixer].detached = true;
    l[Panel::mixer].windowBounds = { 100, 50, 900, 500 };
    const auto text = l.serialise();
    const auto big = Layout::parse (text, 2560, 1240);
    CHECK (big.has_value());
    if (big)
    {
        CHECK ((*big)[Panel::playlist].maximised && (*big)[Panel::mixer].detached && (*big)[Panel::mixer].windowBounds.w == 900);
        CHECK (std::abs ((*big)[Panel::playlist].bounds.x - 2 * l[Panel::playlist].bounds.x) <= 1);
        CHECK (big->order == l.order && big->focused == l.focused);
    }
    CHECK (! Layout::parse ("garbage", 100, 100) && ! Layout::parse ("lynx-layout 1\npanel nowhere 1 0 0 0 0 1 1 0 0 0 0\n", 100, 100));

    // resize keeps panels inside and edges glued to the border
    l.resize (1000, 500);
    CHECK (l[Panel::playlist].bounds.right() == 1000 && l[Panel::browser].bounds.bottom() == 500);
    for (const auto& p : l.panels)
        CHECK (p.bounds.right() <= 1000 && p.bounds.bottom() <= 500);

    // off-screen panels come back
    l[Panel::pianoRoll].bounds = { 5000, -300, 400, 300 };
    CHECK (l.bringBackOffscreen() == 1 && l[Panel::pianoRoll].bounds.x + 400 <= 1000 && l[Panel::pianoRoll].bounds.y == 0);

    // snapping: to the desktop, to another panel, and resizing one edge
    CHECK ((snap ({ 5, 6, 200, 100 }, {}, 1000, 500) == Rect { 0, 0, 200, 100 }));
    CHECK ((snap ({ 304, 50, 200, 100 }, { { 0, 0, 300, 400 } }, 1000, 500) == Rect { 300, 50, 200, 100 }));
    CHECK ((snap ({ 304, 50, 200, 100 }, { { 0, 0, 300, 400 } }, 1000, 500, 2) == Rect { 304, 50, 200, 100 })); // out of range
    CHECK ((snap ({ 100, 100, 195, 100 }, { { 300, 0, 300, 400 } }, 1000, 500, 8, right) == Rect { 100, 100, 200, 100 }));
    CHECK ((snap ({ 100, 100, 195, 100 }, { { 300, 1000, 300, 400 } }, 2000, 2000, 8, right) == Rect { 100, 100, 195, 100 })); // not facing
}

TEST (workflow_control_math)
{
    using namespace lynx::controls;
    CHECK (std::abs (dragValue (0.5, 75, 0, 1, 150, false) - 1.0) < 1e-9);
    CHECK (std::abs (dragValue (0.5, 75, 0, 1, 150, true) - 0.55) < 1e-9);
    CHECK (dragValue (0.5, -1000, -1, 1, 150, false) == -1.0);
    CHECK (std::abs (wheelValue (0.5, 1, 0, 1, false) - 0.52) < 1e-9 && std::abs (wheelValue (0.5, -1, 0, 1, true) - 0.498) < 1e-9);
    CHECK (wheelValue (3, 1, 0, 10, true, 1) == 4.0); // integer controls move at least one step
    CHECK (std::abs (snapToInterval (0.234, 0, 0.1) - 0.2) < 1e-9);

    CHECK (tempoDrag (130.0, 8, false, false) == 132.0 && tempoDrag (130.0, -3, false, false) == 130.0);
    CHECK (std::abs (tempoDrag (130.0, 25, true, false) - 130.25) < 1e-9);
    CHECK (std::abs (tempoDrag (130.0, 8, false, true) - 130.2) < 1e-9);
    CHECK (tempoDrag (520.0, 400, false, false) == lynx::maxTempo && halfTempo (15.0) == lynx::minTempo && doubleTempo (130.0) == 260.0);

    TapTempo tap;
    CHECK (! tap.tap (1000.0));
    CHECK (tap.tap (1500.0) == 120.0);
    CHECK (tap.tap (2000.0) == 120.0 && tap.count() == 3);
    CHECK (std::abs (*tap.tap (2400.0) - 128.57) < 0.01); // average of 500, 500, 400 ms
    CHECK (! tap.tap (9000.0) && tap.count() == 1); // a long pause starts over
}

TEST (flp_fl11_mixer_slots_and_disabled_channels)
{
    // REA_rodada80 §3: FL <= 11 files close an effect slot without 0x62 (the slot is the 0xD4 record's second
    // i32); the channel's 0x00 byte is its Channel Rack switch
    if (realFlpDir.empty())
        return;
    auto load = [] (const std::filesystem::path& p) {
        std::ifstream in (p, std::ios::binary);
        std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
        return bytes;
    };
    const auto base = std::filesystem::path (realFlpDir);
    if (const auto b = load (base / "System" / "Render tests" / "Fruity Phase Inverter.flp"); ! b.empty())
    {
        const auto project = flp::parseProject (b);
        CHECK (project.mixer.size() > 2 && project.mixer[1].slots.size() == 1);
        if (project.mixer.size() > 2 && project.mixer[1].slots.size() == 1)
            CHECK (project.mixer[1].slots[0].index == 0 && project.mixer[1].slots[0].plugin == "Fruity Wrapper"
                   && ! project.mixer[1].slots[0].state.empty());
    }
    if (const auto b = load (base / "Demo projects" / "Performance mode" / "nucleon - Steptime.flp"); ! b.empty())
    {
        const auto project = flp::parseProject (b);
        CHECK (project.mixer.size() > 4 && project.mixer[3].slots.size() == 4);
        if (project.mixer.size() > 4 && project.mixer[3].slots.size() == 4)
            for (int i = 0; i < 4; ++i)
                CHECK (project.mixer[3].slots[(std::size_t) i].index == i);
        const char* names[] = { "Fruity Delay Bank", "Fruity Reeverb 2", "Maximus", "Fruity Parametric EQ 2" };
        if (project.mixer.size() > 4 && project.mixer[3].slots.size() == 4)
            for (int i = 0; i < 4; ++i)
                CHECK (project.mixer[3].slots[(std::size_t) i].plugin == names[i]);
    }
    if (const auto b = load (base / "Demo projects" / "Product demos" / "Synth_dfr - Overcome - Sytrus.flp"); ! b.empty())
    {
        const auto project = flp::parseProject (b);
        int disabled = 0;
        for (const auto& c : project.channels)
            disabled += ! c.enabled;
        CHECK (disabled == 2);
        const auto song = lynx::importFlp (project);
        int muted = 0;
        for (const auto& c : song.channels)
            muted += c.muted;
        CHECK (muted == 2);
    }
}

TEST (three_osc_host_hq_render_box_filters_aliasing_shapes)
{
    // REA_rodada80 §3.3 (3x Osc FUN_0073a000): in FL's HQ render a saw is the sum of k = max(inc >> 22, 1) phase-shifted
    // copies at gain g/k; a sine is not oversampled
    auto state = [] (std::int32_t shape) {
        std::vector<std::uint8_t> st (0x60, 0);
        const std::int32_t version = 14;
        std::memcpy (st.data(), &version, 4);
        std::memcpy (st.data() + 4 + 4 * 1, &shape, 4); // osc 1 shape; the mix knobs at 0: osc 1 alone
        std::vector<std::uint8_t> snap (364, 0);
        const std::int32_t neutral[17] = { 0, 0, 100, 20000, 20000, 30000, 50, 20000, 0, 100, 20000, 0, 32950, 0, 0, 0, 0 };
        for (int k = 0; k < 5; ++k)
            std::memcpy (snap.data() + 0x44 * k, neutral, sizeof (neutral));
        const std::int32_t modX = 256, four = 4, m101 = -101;
        std::memcpy (snap.data() + 0x44, &four, 4);
        std::memcpy (snap.data() + 0x44 + 64, &m101, 4);
        std::memcpy (snap.data() + 0x15c, &modX, 4);
        st.insert (st.end(), snap.begin(), snap.end());
        return st;
    };
    static bool hostHq = false;
    auto render = [&] (std::int32_t shape, bool hq) {
        hostHq = hq;
        auto inst = lynx::makeThreeOscInstrument ([]() noexcept { return hostHq; });
        lynx::ChannelDef c;
        c.instrument = lynx::InstrumentKind::threeosc;
        c.pluginState = state (shape);
        inst->configure (c, 140.0);
        inst->prepare (44100.0);
        inst->noteOn (96, 0.78f);
        std::vector<float> l (4096), r (4096);
        inst->render (l.data(), r.data(), 4096, 1.0f, 1.0f);
        return l;
    };
    auto compare = [&] (std::int32_t shape) {
        const auto live = render (shape, false), hq = render (shape, true);
        double diff = 0.0, energy = 0.0;
        for (std::size_t i = 0; i < live.size(); ++i)
            diff += std::abs (live[i] - hq[i]), energy += std::abs (live[i]);
        return std::make_pair (energy, diff);
    };
    CHECK (lynx::threeOscStateUsable (state (3)));
    const auto [sawEnergy, sawDiff] = compare (3);
    CHECK (sawEnergy > 1.0 && sawDiff > 1.0e-3); // a saw renders differently in the host's HQ mode
    const auto [sineEnergy, sineDiff] = compare (0);
    CHECK (sineEnergy > 1.0 && sineDiff == 0.0); // a sine does not
}

int main (int argc, char** argv)
{
    if (argc > 1)
        realFlpDir = argv[1];

    const char* only = std::getenv ("LYNX_TEST"); // run only the tests whose name contains this
    for (auto& [name, fn] : registry())
    {
        if (only != nullptr && std::strstr (name, only) == nullptr)
            continue;
        std::printf ("%s\n", name);
        fn();
    }

    std::printf ("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

TEST (synth_cutoff_keytrack)
{
    // the cutoff follows the note from key 48 (130.81 Hz), like Harmor's "Key to filter frequency"
    auto rms = [] (float keyTrack, int key) {
        lynx::SynthSettings s;
        s.osc[0] = { lynx::Waveform::sine, 0, 0.0f, 1.0f };
        s.cutoff = 130.81f;
        s.keyTrack = keyTrack;
        auto inst = lynx::makeInstrument (lynx::InstrumentKind::synth);
        inst->prepare (48000.0);
        lynx::ChannelDef c;
        c.synth = s;
        inst->configure (c, 120.0);
        inst->noteOn (key, 1.0f);
        std::vector<float> l (24000), r (24000);
        inst->render (l.data(), r.data(), 24000, 1.0f, 1.0f);
        double e = 0.0;
        for (int i = 4800; i < 24000; ++i)
            e += (double) l[(std::size_t) i] * l[(std::size_t) i];
        return std::sqrt (e / 19200.0);
    };
    const auto fixed = rms (0.0f, 72), tracked = rms (1.0f, 72);
    CHECK (tracked > 4.0 * fixed);            // 2 octaves up: without keytrack the low-pass cuts
    CHECK (std::abs (rms (1.0f, 48) - rms (0.0f, 48)) < 0.05 * rms (0.0f, 48)); // unchanged at the reference key
}

namespace
{
std::vector<float> renderSynthNote (const lynx::SynthSettings& s, int key, int frames)
{
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::synth);
    inst->prepare (48000.0);
    lynx::ChannelDef c;
    c.synth = s;
    inst->configure (c, 120.0);
    inst->noteOn (key, 1.0f);
    std::vector<float> l ((std::size_t) frames), r ((std::size_t) frames);
    inst->render (l.data(), r.data(), frames, 1.0f, 1.0f);
    return l;
}

double goertzel (const std::vector<float>& x, double f, int from, int to)
{
    const auto w = 2.0 * 3.141592653589793 * f / 48000.0, k = 2.0 * std::cos (w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = from; i < to; ++i)
    {
        const auto s0 = x[(std::size_t) i] + k * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - k * s1 * s2;
}
} // namespace

TEST (synth_pitch_sweep_ring_and_bands)
{
    lynx::SynthSettings sine;
    sine.osc[0] = { lynx::Waveform::sine, 0, 0.0f, 1.0f };
    sine.cutoff = 20000.0f;

    // sweep: starts at f·(1 + sweep) and returns to f (time constant s = 10 ms; s⁴ falls 4× faster)
    auto sweep = sine;
    sweep.sweep = 1.0f;
    sweep.sweepTime = 0.01f;
    const auto a = renderSynthNote (sweep, 69, 24000);
    auto crossings = [] (const std::vector<float>& x, int to) {
        int n = 0;
        for (int i = 1; i < to; ++i)
            n += (x[(std::size_t) i - 1] < 0.0f) != (x[(std::size_t) i] < 0.0f);
        return n;
    };
    CHECK (crossings (a, 480) > 1.3 * crossings (renderSynthNote (sine, 69, 480), 480)); // 10 ms: much higher
    CHECK (goertzel (a, 440.0, 12000, 24000) > 100.0 * goertzel (a, 880.0, 12000, 24000));

    // ring with |a| = 1: carrier 440 × modulator 110 -> only 330 and 550, no 440
    auto ring = sine;
    ring.am = ring.ring = true;
    ring.osc[2] = { lynx::Waveform::sine, -2, 0.0f, 1.0f };
    const auto b = renderSynthNote (ring, 69, 24000);
    CHECK (goertzel (b, 330.0, 4800, 24000) > 100.0 * goertzel (b, 440.0, 4800, 24000));
    CHECK (goertzel (b, 550.0, 4800, 24000) > 100.0 * goertzel (b, 440.0, 4800, 24000));

    // 3 bands: muting the high knocks down a 4 kHz note and barely touches an 80 Hz one
    auto bands = sine;
    bands.bandGain = { 1.0f, 1.0f, 0.0f };
    auto rms = [] (const std::vector<float>& x) {
        double e = 0.0;
        for (std::size_t i = 4800; i < x.size(); ++i)
            e += (double) x[i] * x[i];
        return std::sqrt (e / (double) (x.size() - 4800));
    };
    CHECK (rms (renderSynthNote (bands, 107, 24000)) < 0.3 * rms (renderSynthNote (sine, 107, 24000)));
    CHECK (rms (renderSynthNote (bands, 28, 24000)) > 0.85 * rms (renderSynthNote (sine, 28, 24000)));
}

TEST (love_philter_map_evaluation)
{
    lynx::LovePhilterMap line { { { 0.0, 0.0 }, { 1.0, 1.0 } } };
    CHECK (std::abs (lynx::lovePhilterMapAt (line, 0.5) - 0.5) < 1e-9);
    CHECK (std::abs (lynx::lovePhilterMapAt (line, 1.0) - 1.0) < 1e-9);
    // steps (Δx = 0): 1 until 0.4375, ramp to 0 at 0.5, then 0
    lynx::LovePhilterMap steps { { { 0.0, 1.0 }, { 0.4375, 1.0 }, { 0.0625, 0.0 }, { 0.5, 0.0 } } };
    CHECK (lynx::lovePhilterMapAt (steps, 0.27) == 1.0);
    CHECK (lynx::lovePhilterMapAt (steps, 0.75) == 0.0);
    CHECK (std::abs (lynx::lovePhilterMapAt (steps, 0.46875) - 0.5) < 1e-9);
    CHECK (lynx::lovePhilterMapAt ({}, 0.3) == 1.0);
}

TEST (lofi_holds_and_quantizes)
{
    auto fx = lynx::makeEffect (lynx::EffectKind::lofi);
    fx->prepare (48000.0);
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::lofi);
    slot.params[0] = 0.5f; // 128·0.125 + 1 = 17 samples
    slot.params[1] = 1.0f; // 65536/2049 ≈ 32 levels
    std::vector<float> l (170), r (170);
    for (std::size_t i = 0; i < l.size(); ++i)
        l[i] = r[i] = 0.9f * (float) std::sin (0.05 * (double) i);
    const auto original = l;
    fx->process (l.data(), r.data(), 170, slot.params, 120.0);
    for (int i = 0; i < 17; ++i)
        CHECK (l[(std::size_t) i] == l[0]);
    CHECK (l[17] != l[16]);
    const auto step = 2.0f * 2049.0f / 65536.0f;
    for (auto v : l)
        CHECK (std::abs (v / step - std::round (v / step)) < 1e-3f);
    CHECK (std::abs (l[0] - original[0]) <= step);
    CHECK (lynx::formatEffectParam (lynx::EffectKind::lofi, 1, 0.0f, 120.0) == "16.0 bits");
}

namespace
{
lynx::EffectParams effectorParams (int type, float x, float y)
{
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::effector);
    slot.params[0] = (float) type / 11.0f;
    slot.params[3] = x;
    slot.params[4] = y;
    return slot.params;
}
} // namespace

TEST (effector_algorithms)
{
    CHECK (lynx::effector::typeOf (8.0f / 11.0f) == 8);
    CHECK (std::string (lynx::effector::typeName (11)) == "Ring");
    CHECK (std::abs (lynx::effector::rateBeats (0.5f, 0.0f) - 0.75) < 1e-9); // trunc(8·0.5 + 0.5) = 4 -> 3/4
    CHECK (std::abs (lynx::effector::rateBeats (1.0f, 0.5f) - 2.0) < 1e-9);   // 8 × (1 − 0.5)²

    auto run = [] (const lynx::EffectParams& p, std::vector<float> in, double bpm = 120.0) {
        auto fx = lynx::makeEffect (lynx::EffectKind::effector);
        fx->prepare (48000.0);
        fx->setSongPosition (0.0);
        auto r = in;
        fx->process (in.data(), r.data(), (int) in.size(), p, bpm);
        return in;
    };

    // Distort with Y = 1 (everything goes to the cascade): 0.5 comes out 0.5 (compensation G = 0.5/F(0.5))
    {
        auto p = effectorParams (0, 0.5f, 1.0f);
        const auto out = run (p, std::vector<float> (4800, 0.5f));
        CHECK (std::abs (out.back() - 0.5f) < 1e-3f);
        const auto small = run (p, std::vector<float> (4800, 0.05f));
        CHECK (small.back() > 0.45f && small.back() < 0.5f); // saturates near ±0.5
    }

    // Ring: DC 1 becomes the carrier sine, 2^(8.7·0 + 5) = 32 Hz -> 32 cycles per second
    {
        auto p = effectorParams (11, 0.0f, 0.0f);
        const auto out = run (p, std::vector<float> (48000, 1.0f));
        int crossings = 0;
        for (std::size_t i = 4800; i < out.size(); ++i)
            crossings += (out[i - 1] < 0.0f) != (out[i] < 0.0f);
        CHECK (crossings >= 56 && crossings <= 60); // 0.9 s × 32 Hz × 2
    }

    // Trancer, Y = 0: at the start of the period the gain drops to zero; X = 1 returns only at the end of the period
    {
        auto p = effectorParams (8, 1.0f, 0.0f);
        p[5] = 1.0f; // 8/1 = 8 beats = 4 s at 120 BPM
        const auto out = run (p, std::vector<float> (48000, 1.0f));
        CHECK (out[100] > 0.3f);       // still rising on the "and" (1% of the period = 40 ms)
        CHECK (out[24000] < 0.05f);    // 0.5 s: trough
    }

    // Reverb with Wet = 0: the dry passes intact
    {
        auto p = effectorParams (6, 0.5f, 0.5f);
        p[2] = 0.0f;
        std::vector<float> in (4800);
        for (std::size_t i = 0; i < in.size(); ++i)
            in[i] = (float) std::sin (0.01 * (double) i);
        const auto out = run (p, in);
        for (std::size_t i = 0; i < in.size(); ++i)
            CHECK (std::abs (out[i] - in[i]) < 1e-6f);
    }

    // Delay: the echo arrives after the Mod Rate period (1/4 beat at 120 BPM = 125 ms), at level X²
    {
        auto p = effectorParams (5, 0.7f, 0.5f);
        p[5] = 1.0f / 8.0f; // index 1 = 1/4
        std::vector<float> in (12000, 0.0f);
        in[0] = 1.0f;
        const auto out = run (p, in);
        CHECK (std::abs (out[6000] - 0.49f) < 0.02f);
        CHECK (std::abs (out[3000]) < 1e-6f);
    }

    // Stutter: repeats the recorded start every period
    {
        auto p = effectorParams (9, 0.0f, 0.0f);
        p[5] = 0.0f; // 1/8 beat = 62.5 ms = 3000 samples
        std::vector<float> in (9000);
        for (std::size_t i = 0; i < in.size(); ++i)
            in[i] = (float) i / 9000.0f;
        const auto out = run (p, in);
        CHECK (std::abs (out[3000 + 500] - in[500]) < 0.01f); // after the crossfade, it's the start again
        CHECK (std::abs (out[1000] - in[1000]) < 1e-6f);
    }
}

TEST (crossover_linkwitz_riley_sums_flat)
{
    // low band + high band of the same crossover: sum with magnitude 1 (Linkwitz-Riley; for 12 and 36 dB the HP is inverted)
    for (const auto combo : { 1, 3, 4, 5, 6, 7 })
        for (const auto testHz : { 200.0, 1000.0, 5000.0 })
        {
            auto run = [&] (bool high) {
                auto fx = lynx::makeEffect (lynx::EffectKind::crossover);
                fx->prepare (48000.0);
                auto slot = lynx::makeEffectSlot (lynx::EffectKind::crossover);
                const auto value = (float) (std::log2 (1000.0 / 30.0) / 9.0589);
                slot.params[high ? 0 : 3] = 1.0f;
                slot.params[high ? 1 : 4] = value;
                slot.params[high ? 2 : 5] = (float) combo / 7.0f;
                std::vector<float> l (24000), r (24000);
                for (std::size_t i = 0; i < l.size(); ++i)
                    l[i] = r[i] = (float) std::sin (2.0 * 3.141592653589793 * testHz * (double) i / 48000.0);
                fx->process (l.data(), r.data(), 24000, slot.params, 120.0);
                return l;
            };
            const auto low = run (false), high = run (true);
            double peak = 0.0;
            for (std::size_t i = 12000; i < low.size(); ++i)
                peak = std::max (peak, (double) std::abs (low[i] + high[i]));
            CHECK (std::abs (peak - 1.0) < 0.02);
        }
    // 96 dB/oct: two octaves above the cutoff the low-pass is already below −150 dB... in practice, very low
    auto fx = lynx::makeEffect (lynx::EffectKind::crossover);
    fx->prepare (48000.0);
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::crossover);
    slot.params[3] = 1.0f;
    slot.params[4] = (float) (std::log2 (500.0 / 30.0) / 9.0589);
    slot.params[5] = 1.0f;
    std::vector<float> l (24000), r (24000);
    for (std::size_t i = 0; i < l.size(); ++i)
        l[i] = r[i] = (float) std::sin (2.0 * 3.141592653589793 * 2000.0 * (double) i / 48000.0);
    fx->process (l.data(), r.data(), 24000, slot.params, 120.0);
    float peak = 0.0f;
    for (std::size_t i = 12000; i < l.size(); ++i)
        peak = std::max (peak, std::abs (l[i]));
    CHECK (peak < 1e-4f);
}

TEST (rhythm_gain_and_autopan)
{
    // 1-beat gate: 1 → 0 at 0.5 beat (linear) → 1 at the end; at 120 BPM 1 beat = 24000 samples at 48 kHz
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::rhythmGain);
    slot.params[0] = 1.0f / 64.0f;
    slot.params[1] = 3.0f / 12.0f;
    const float pts[3][3] = { { 0.0f, 1.0f, 0.5f }, { 0.5f, 0.0f, 0.5f }, { 1.0f, 1.0f, 0.5f } };
    for (int k = 0; k < 3; ++k)
        for (int j = 0; j < 3; ++j)
            slot.params[(std::size_t) (2 + 3 * k + j)] = pts[k][j];
    auto fx = lynx::makeEffect (lynx::EffectKind::rhythmGain);
    fx->prepare (48000.0);
    fx->setSongPosition (960.0 * 2.0); // beat 2: phase 0
    std::vector<float> l (24000, 1.0f), r (24000, 1.0f);
    fx->process (l.data(), r.data(), 24000, slot.params, 120.0);
    CHECK (std::abs (l[0] - 1.0f) < 1e-3f);
    CHECK (std::abs (l[6000] - 0.5f) < 1e-3f);
    CHECK (l[12000] < 1e-3f);
    CHECK (std::abs (l[18000] - 0.5f) < 1e-3f);
    // positive tension: changes quickly at the start of the segment
    CHECK (lynx::effectInfo (lynx::EffectKind::rhythmGain).params.size() == 39);

    // auto-pan: 1-beat period, full depth; start phase 0.75 -> W = 0 rising (pan centered)
    auto pan = lynx::makeEffectSlot (lynx::EffectKind::autoPan);
    pan.params[0] = 1.0f / 64.0f;
    pan.params[1] = 1.0f;
    auto ap = lynx::makeEffect (lynx::EffectKind::autoPan);
    ap->prepare (48000.0);
    ap->setSongPosition (0.0);
    std::vector<float> a (24000, 1.0f), b (24000, 1.0f);
    ap->process (a.data(), b.data(), 24000, pan.params, 120.0);
    CHECK (std::abs (a[0] - 1.0f) < 1e-3f && std::abs (b[0] - 1.0f) < 1e-3f);
    CHECK (a[6000] < 0.01f && std::abs (b[6000] - 1.0f) < 1e-3f); // ¼ cycle later: fully right
}

TEST (vocoder_side_inputs_split_modulator_and_carrier)
{
    // "Side inputs" mode (Vocodex): modulator from side input 1 (noise in the first half), carrier from side input 2
    // (saw), own input silent → the output follows the side modulator; with input 1 silent, the output is silent
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::vocoder);
    slot.params[16] = 1.0f;
    auto fx = lynx::makeEffect (lynx::EffectKind::vocoder);
    fx->prepare (48000.0);
    std::vector<float> modL (48000), car (48000), l (48000, 0.0f), r (48000, 0.0f);
    std::uint32_t seed = 1;
    for (std::size_t i = 0; i < modL.size(); ++i)
    {
        seed = seed * 1664525u + 1013904223u;
        modL[i] = i < 24000 ? ((float) (seed >> 8) / 16777216.0f - 0.5f) : 0.0f;
        car[i] = (float) (2.0 * std::fmod ((double) i * 220.0 / 48000.0, 1.0) - 1.0) * 0.5f;
    }
    for (int done = 0; done < 48000; done += 480)
    {
        fx->setSidechain (modL.data() + done, modL.data() + done);
        fx->setSidechain2 (car.data() + done, car.data() + done);
        fx->process (l.data() + done, r.data() + done, 480, slot.params, 120.0);
    }
    auto rms = [&] (int from, int to) {
        double e = 0.0;
        for (int i = from; i < to; ++i)
            e += (double) l[(std::size_t) i] * l[(std::size_t) i];
        return std::sqrt (e / (to - from));
    };
    CHECK (rms (6000, 20000) > 0.01);
    CHECK (rms (40000, 48000) < 0.1 * rms (6000, 20000));
    // the side modulator configured but silent: nothing comes out (the own input is not used instead)
    std::vector<float> a (4800, 0.3f), b (4800, 0.3f);
    for (int done = 0; done < 4800; done += 480)
    {
        fx->setSidechain (nullptr, nullptr);
        fx->setSidechain2 (car.data() + done, car.data() + done);
        fx->process (a.data() + done, b.data() + done, 480, slot.params, 120.0);
    }
    CHECK (std::abs (a[4700]) < 0.05f);
}

TEST (vocoder_follows_modulator)
{
    // modulator (L) plays only in the first half; carrier (R) is a continuous saw: the wet follows the modulator
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::vocoder);
    auto fx = lynx::makeEffect (lynx::EffectKind::vocoder);
    fx->prepare (48000.0);
    std::vector<float> l (48000), r (48000);
    std::uint32_t seed = 1;
    for (std::size_t i = 0; i < l.size(); ++i)
    {
        seed = seed * 1664525u + 1013904223u;
        l[i] = i < 24000 ? ((float) (seed >> 8) / 16777216.0f - 0.5f) : 0.0f;
        r[i] = (float) (2.0 * std::fmod ((double) i * 220.0 / 48000.0, 1.0) - 1.0) * 0.5f;
    }
    fx->process (l.data(), r.data(), 48000, slot.params, 120.0);
    auto rms = [&] (int from, int to) {
        double e = 0.0;
        for (int i = from; i < to; ++i)
            e += (double) l[(std::size_t) i] * l[(std::size_t) i];
        return std::sqrt (e / (to - from));
    };
    CHECK (rms (6000, 20000) > 0.01);
    CHECK (rms (40000, 48000) < 0.1 * rms (6000, 20000)); // modulator silent: the vocoder fades out (100 ms release)
    for (std::size_t i = 0; i < l.size(); ++i)
        CHECK (l[i] == r[i] && std::isfinite (l[i]));    // mono output
}

TEST (fruity_vocoder_engine)
{
    // REA_rodada70: Fruity Vocoder, states of our own against the installation's DLL (goldens = the DLL's output for
    // the same input, EXEC; all three bit-identical over the whole 4096 frames)
    std::vector<float> inL (4096), inR (4096);
    for (int i = 0; i < 4096; ++i)
    {
        inL[(std::size_t) i] = i < 3000 ? (float) (((i * 37) % 200) - 100) / 128.0f : 0.0f; // modulator, then silence
        inR[(std::size_t) i] = (i * 13) % 64 < 32 ? 0.5f : -0.5f;                        // carrier
    }
    auto make = [] (int bands, int order, int listen, bool swap, std::vector<float> levels, std::array<int, 12> c) {
        lynx::fvocoder::State s;
        s.bands = bands;
        s.order = order;
        s.listen = listen;
        s.swap = swap;
        for (std::size_t k = 0; k < levels.size(); ++k)
            s.levels[k] = levels[k];
        s.c = c;
        return s;
    };
    auto run = [&] (const lynx::fvocoder::State& s, double rate, int block, int holdAt) {
        std::vector<float> l = inL, r = inR;
        lynx::fvocoder::Engine e;
        e.setState (s);
        e.prepare (rate);
        for (int i = 0; i < 4096;)
        {
            auto len = std::min (block, 4096 - i);
            if (holdAt > i && holdAt < i + len)
                len = holdAt - i;
            if (i == holdAt)
            {
                auto t = e.state();
                t.c[lynx::fvocoder::hold] = 1;
                e.setState (t);
            }
            e.processStereo (l.data() + i, r.data() + i, len);
            i += len;
        }
        return std::make_pair (l, r);
    };
    auto check = [] (const std::pair<std::vector<float>, std::vector<float>>& out, const std::vector<std::pair<int, float>>& golden) {
        for (const auto& [i, g] : golden)
            CHECK (out.first[(std::size_t) i] == g && out.second[(std::size_t) i] == g);
    };
    // 16 bands, order 2, edited band levels, modulator and carrier levels in the mix
    const auto a = make (16, 2, 2, false, { 1.0f, 0.5f, 1.5f, 1.0f, 0.25f, 2.0f, 1.0f, 1.0f, 0.75f, 1.25f, 1.0f, 0.0f, 1.0f, 1.0f, 1.75f, 1.0f },
                         { 0, 65536, 64, 0, 0, 50, 20, 100, 0, 20, 10, 128 });
    const auto outA = run (a, 44100.0, 512, -1);
    check (outA, { { 100, -0.23145467042922974f }, { 1000, 0.2901436686515808f }, { 2047, 0.03407000005245209f }, { 2500, 0.8314149379730225f },
                   { 3000, 0.606562614440918f }, { 3500, 0.2637328505516052f }, { 4095, -0.001206810586154461f } });
    // 64 bands, order 3, inverted synthesis bank shifted by −300 cents, 48 kHz in blocks of 300, Hold from frame 2048
    const auto b = make (64, 3, 2, false, {}, { 2000, 60000, 90, 1, -300, 120, 15, 300, 0, 0, 5, 150 });
    check (run (b, 48000.0, 300, 2048), { { 100, 0.005184958688914776f }, { 1000, -0.0008608861826360226f }, { 2047, -0.026466617360711098f },
                                          { 2500, -0.009371745400130749f }, { 3000, -0.018949978053569794f }, { 3500, -0.02667868137359619f },
                                          { 4095, -0.021947938948869705f } });
    // 4 bands, order 1, swapped inputs, narrow 30000…50000 range with the linear scale, 22.05 kHz in blocks of 77
    const auto c = make (4, 1, 2, true, { 1.0f, 0.8f, 1.2f, 1.0f }, { 30000, 50000, 0, 0, 700, 180, 300, 40, 0, 64, 0, 160 });
    check (run (c, 22050.0, 77, -1), { { 100, 0.31434211134910583f }, { 1000, -1.953513264656067f }, { 2047, 1.4135023355484009f },
                                       { 2500, 1.539392352104187f }, { 3000, -0.27189597487449646f }, { 3500, -0.11583124846220016f },
                                       { 4095, -0.11583124846220016f } });

    // FL's state imports into the LYNX slot (controls as parameters, the levels in the state) and the slot's effect
    // (one 4096-frame call, split into 512-frame engine blocks) gives the same output
    const auto bytes = lynx::fvocoder::serialise (a);
    CHECK (bytes.size() == 17 + 16 * 4 + 12 * 4);
    const auto slot = lynx::effectFromFl ("Fruity Vocoder", bytes);
    CHECK (slot && slot->kind == lynx::EffectKind::vocoder && slot->state == bytes);
    CHECK (slot && slot->params[0] == 0.4f && slot->params[1] == 0.5f && slot->params[2] == 1.0f && slot->params[15] == 0.8f);
    if (slot)
    {
        auto fx = lynx::makeEffect (*slot);
        fx->prepare (44100.0);
        std::vector<float> l = inL, r = inR;
        fx->process (l.data(), r.data(), 4096, slot->params, 120.0);
        CHECK (l == outA.first && r == outA.second);
        CHECK (lynx::formatEffectParam (lynx::EffectKind::vocoder, 5, slot->params[5], 120.0) == "16000 Hz");
        CHECK (lynx::formatEffectParam (lynx::EffectKind::vocoder, 10, slot->params[10], 120.0) == "20 ms");
        CHECK (lynx::formatEffectParam (lynx::EffectKind::vocoder, 15, slot->params[15], 120.0) == "1.00");
    }
    const auto parsed = lynx::fvocoder::parse (bytes);
    CHECK (parsed && parsed->bands == 16 && parsed->levels[5] == 2.0f && parsed->c[lynx::fvocoder::release] == 100);
    CHECK (! lynx::fvocoder::parse ({ 2, 0, 0, 0, 12, 0, 0, 0 })); // 12 bands is not a valid count
    // a raw control outside the knob range (FL keeps it, REA_rodada78 §1) survives while the knob shows it, clamped
    CHECK (lynx::fvocoder::rawFromKnob (0.0f, lynx::fvocoder::attack, 5) == 5);
    CHECK (lynx::fvocoder::rawFromKnob (0.5f, lynx::fvocoder::attack, 5) == 505);
    CHECK (lynx::fvocoder::rawFromKnob (1.0f, lynx::fvocoder::wetLevel, 250) == 250);
    CHECK (lynx::fvocoder::rawFromKnob (0.5f, lynx::fvocoder::freqScale, 64) == 64);
}

// Delphi run-time math (REA_rodada70 §2): values the plugin's own exp / ln / SinCos return (EXEC, called in place)
TEST (delphi_math_matches_plugin_runtime)
{
    // arguments where the C library differs from the Delphi run-time in the last bit
    CHECK (lynx::dmath::exp (0x1.f6e5c5a26a7f8p+3) == 0x1.98170c3deea30p+22);
    CHECK (lynx::dmath::exp (-0x1.12fc91a51d906p+4) == 0x1.27108f934a58cp-25);
    CHECK (lynx::dmath::exp (-0x1.2e9e4c229e93ap+4) == 0x1.a3bf2e4ee3f1ep-28);
    CHECK (lynx::dmath::ln (0x1.76a77e2a09abep-1) == -0x1.3fd111c5297f8p-2);
    CHECK (lynx::dmath::ln (0x1.da1feaa8547dfp+0) == 0x1.3b8abd2756774p-1);
    CHECK (lynx::dmath::ln (0x1.a1e28772a0d78p-1) == -0x1.9ffcaa3a11e30p-3);
    double s = 0.0, c = 0.0;
    lynx::dmath::sinCos (0x1.91b4a19e65a9bp-1, s, c);
    CHECK (s == 0x1.69be278761916p-1 && c == 0x1.6a559572e1ac9p-1);
    lynx::dmath::sinCos (0x1.3284a93d70a8ap-1, s, c);
    CHECK (s == 0x1.2088b313ea643p-1 && c == 0x1.a6f4d889fd91ap-1);
    lynx::dmath::sinCos (0x1.f9b2f53d6ce02p+0, s, c);
    CHECK (s == 0x1.d6a9b8f170994p-1 && c == -0x1.931701be7b5f3p-2);
}

// Vocodex (REA_rodada35): a synthetic state in FL's layout (version 8, 13 maps, 20 parameters, the 0x24 block,
// 13 map-axis bytes, tone, notes)
struct VocodexSpec
{
    // Wet, SG, ModVol, ModHP, CarVol, CarLP, CarNoise, CarNoiseHP, Hold, Att, AttCurve, Rel, RelCurve, Freeze, BW,
    // Flatness, ModBW, ModPitch, FatPitch, FatPan
    std::array<std::int32_t, 20> params { 1000, 0, 0, 0, 0, 65536, 0, 0, 0, 2000, 0, 3000, 0, 0, 1000, 0, 2000, 0, 0, 0 };
    std::array<std::int32_t, 7> block { 0, 2, 0, 24, 1, 1, 2 }; // hold mode, order, unison, bands, draft field, ModIn, CarIn
    std::array<std::uint8_t, 8> flags { 0, 1, 0, 1, 1, 0, 0, 0 }; // L-R, MIN TIMES, TO MOD, THREADED, subtractive, TO NOISE, FROM CAR, FROM CONS

    std::vector<std::uint8_t> bytes() const
    {
        std::vector<std::uint8_t> d;
        auto put = [&d] (const void* p, std::size_t n) { d.insert (d.end(), (const std::uint8_t*) p, (const std::uint8_t*) p + n); };
        auto i32 = [&] (std::int32_t v) { put (&v, 4); };
        i32 (8 << 16);
        for (int m = 0; m < 13; ++m)
        {
            const double y0 = m == 8 ? 0.1429 : (m == 3 || m == 4 || m == 11) ? 0.0 : 0.5;
            const double y1 = m == 8 ? 0.9918 : y0;
            i32 (1), i32 (0), i32 (2);
            for (const auto& [dx, y] : { std::pair { 0.0, y0 }, std::pair { 1.0, y1 } })
            {
                put (&dx, 8), put (&y, 8);
                i32 (0), i32 (0); // tension (f32 0), flags
            }
            const std::array<std::uint8_t, 20> tail {};
            put (tail.data(), tail.size());
        }
        for (auto v : params)
            i32 (v);
        for (auto v : block)
            i32 (v);
        put (flags.data(), flags.size());
        const std::array<std::uint8_t, 13> axis {};
        put (axis.data(), axis.size());
        d.push_back (3), put ("PWM", 3);
        i32 (1), i32 (60);
        return d;
    }
};

std::vector<float> vocodexSaw (int n, double hz, double fs)
{
    std::vector<float> out ((std::size_t) n);
    for (int i = 0; i < n; ++i)
        out[(std::size_t) i] = (float) (2.0 * std::fmod (i * hz / fs, 1.0) - 1.0) * 0.4f;
    return out;
}

TEST (vocodex_state_parse_and_conversion)
{
    VocodexSpec spec;
    spec.block = { 1, 3, 2, 40, 0, 2, 1 };
    spec.params[17] = -1200;
    const auto raw = spec.bytes();
    const auto s = lynx::vocodex::parseState (raw);
    CHECK (s.has_value());
    if (! s)
        return;
    CHECK (s->version == 8 && s->holdMode == 1 && s->order == 3 && s->unison == 2 && s->bands == 40);
    CHECK (s->draft() && s->modIn == 2 && s->carIn == 1 && s->minTimes && s->subtractiveGate && ! s->lrEncoding);
    CHECK (s->params[lynx::vocodex::modPitch] == -1200 && s->params[lynx::vocodex::carLp] == 65536);
    CHECK (s->maps[8].x.size() == 2 && std::abs (s->maps[8].y[1] - 0.9918) < 1e-12 && s->maps[8].x[1] == 1.0);
    CHECK (s->tone == "PWM" && s->notes.size() == 1 && s->notes[0] == 60);

    const auto v = lynx::vocodexEffect (raw);
    CHECK (v && v->slot.kind == lynx::EffectKind::vocodex && v->slot.state == raw && v->modIn == 2 && v->carIn == 1 && v->notes.empty());
    spec.block[6] = -1; // internal synth carrier: not available yet → reported
    spec.params[1] = 150; // Soundgoodizer stage
    const auto w = lynx::vocodexEffect (spec.bytes());
    CHECK (w && w->carIn == -1 && w->notes.size() == 2);
    CHECK (! lynx::vocodex::parseState ({ 1, 2, 3 }).has_value());
}

TEST (vocodex_vocodes_side_inputs)
{
    // modulator on side input 1 (noise in the first half), carrier on side input 2 (saw), own input silent: the
    // output follows the modulator's envelope and fades after it stops
    VocodexSpec spec;
    lynx::EffectSlot slot = lynx::makeEffectSlot (lynx::EffectKind::vocodex);
    slot.state = spec.bytes();
    auto fx = lynx::makeEffect (slot);
    fx->prepare (48000.0);
    CHECK (fx->latencySamples() == 0);
    const auto car = vocodexSaw (48000, 110.0, 48000.0);
    std::vector<float> mod (48000), l (48000, 0.0f), r (48000, 0.0f);
    std::uint32_t seed = 7;
    for (std::size_t i = 0; i < mod.size(); ++i)
    {
        seed = seed * 1664525u + 1013904223u;
        mod[i] = i < 24000 ? ((float) (seed >> 8) / 16777216.0f - 0.5f) : 0.0f;
    }
    for (int done = 0; done < 48000; done += 512)
    {
        const int n = std::min (512, 48000 - done);
        fx->setSidechain (mod.data() + done, mod.data() + done);
        fx->setSidechain2 (car.data() + done, car.data() + done);
        fx->process (l.data() + done, r.data() + done, n, slot.params, 120.0);
    }
    auto rms = [&] (int from, int to) {
        double e = 0.0;
        for (int i = from; i < to; ++i)
            e += (double) l[(std::size_t) i] * l[(std::size_t) i] + (double) r[(std::size_t) i] * r[(std::size_t) i];
        return std::sqrt (e / (2.0 * (to - from)));
    };
    CHECK (rms (6000, 20000) > 0.02);
    CHECK (rms (40000, 48000) < 0.01 * rms (6000, 20000));
    CHECK (std::abs (l[0]) < 1e-6f); // the gains ramp up from 0 when the plugin is created
}

TEST (vocodex_draft_latency_and_pass_through)
{
    // carrier pass-through at unit gain only (Wet 0): after a Flush the gains snap, so the output is the carrier,
    // 16 samples late in Draft
    VocodexSpec spec;
    spec.params[0] = 0;      // Wet
    spec.params[4] = 12800;  // CarVol = 1
    for (const int draftField : { 1, 0 })
    {
        spec.block[4] = draftField;
        const auto s = lynx::vocodex::parseState (spec.bytes());
        CHECK (s.has_value());
        lynx::vocodex::Engine engine;
        engine.prepare (*s, 44100.0, 300, s->draft());
        CHECK (engine.latencySamples() == (draftField == 0 ? 16 : 0));
        engine.flush();
        const auto saw = vocodexSaw (3000, 220.0, 44100.0);
        std::vector<float> car (6000), mod (6000, 0.0f), out (6000);
        for (int i = 0; i < 3000; ++i)
            car[(std::size_t) (2 * i)] = car[(std::size_t) (2 * i + 1)] = saw[(std::size_t) i];
        static constexpr int sizes[] { 300, 37, 128, 1, 255, 16, 299 }; // uneven blocks exercise the FIFOs
        for (int done = 0, k = 0, n = 0; done < 3000; done += n, ++k)
        {
            n = std::min (sizes[k % 7], 3000 - done);
            engine.process (mod.data() + 2 * done, mod.data() + 2 * done, car.data() + 2 * done, out.data() + 2 * done, n);
        }
        const int lag = engine.latencySamples();
        auto maxError = 0.0f;
        for (int i = lag; i < 3000; ++i)
            maxError = std::max (maxError, std::abs (out[(std::size_t) (2 * i)] - saw[(std::size_t) (i - lag)]));
        CHECK (maxError == 0.0f);
    }
}

TEST (vocodex_soundgoodizer_stage_limits_the_output)
{
    // SG > 0: the vocoder's output (loud here: Wet 2000) goes through the Soundgoodizer approximation, whose
    // master limiter keeps the peaks under −0.2 dBFS; its look-ahead is reported as latency
    VocodexSpec spec;
    spec.params[0] = 2000;
    spec.params[1] = 250;
    lynx::EffectSlot slot = lynx::makeEffectSlot (lynx::EffectKind::vocodex);
    slot.state = spec.bytes();
    auto fx = lynx::makeEffect (slot);
    fx->prepare (48000.0);
    CHECK (fx->latencySamples() == 72); // 1.5 ms at 48 kHz
    const auto car = vocodexSaw (48000, 110.0, 48000.0);
    std::vector<float> mod (48000), l (48000, 0.0f), r (48000, 0.0f);
    for (std::size_t i = 0; i < mod.size(); ++i)
        mod[i] = 0.9f * (float) std::sin (0.07 * (double) i) * (float) std::sin (0.0003 * (double) i);
    auto peak = 0.0f;
    for (int done = 0; done < 48000; done += 512)
    {
        const int n = std::min (512, 48000 - done);
        fx->setSidechain (mod.data() + done, mod.data() + done);
        fx->setSidechain2 (car.data() + done, car.data() + done);
        fx->process (l.data() + done, r.data() + done, n, slot.params, 120.0);
    }
    for (std::size_t i = 0; i < l.size(); ++i)
        peak = std::max ({ peak, std::abs (l[i]), std::abs (r[i]) });
    CHECK (peak > 0.5f && peak <= 0.9775f);
}

TEST (vocodex_tail_bypass_and_noise)
{
    // both inputs silent for longer than 0.5 s + hold + release: the own input passes unchanged
    VocodexSpec spec;
    const auto s = lynx::vocodex::parseState (spec.bytes());
    CHECK (s.has_value());
    lynx::vocodex::Engine engine;
    engine.prepare (*s, 44100.0, 512, false);
    std::vector<float> zero (1024, 0.0f), own (1024), out (1024);
    for (std::size_t i = 0; i < own.size(); ++i)
        own[i] = 0.25f * (float) std::sin (0.01 * (double) i);
    for (int b = 0; b < 80; ++b)
        engine.process (own.data(), zero.data(), zero.data(), out.data(), 512);
    CHECK (out == own);

    // carrier noise: the IPP-compatible generator restarts from the same seed after a Flush
    spec.params[6] = 9000;
    const auto noisy = lynx::vocodex::parseState (spec.bytes());
    lynx::vocodex::Engine a;
    a.prepare (*noisy, 44100.0, 512, false);
    std::vector<float> mod (1024), first (1024), second (1024);
    for (std::size_t i = 0; i < mod.size(); ++i)
        mod[i] = 0.3f * (float) std::sin (0.05 * (double) i);
    a.flush();
    a.process (own.data(), mod.data(), zero.data(), first.data(), 512);
    a.flush();
    a.process (own.data(), mod.data(), zero.data(), second.data(), 512);
    CHECK (first == second);
    auto energy = 0.0;
    for (auto v : first)
        energy += (double) v * v;
    CHECK (energy > 1e-6 && std::isfinite (energy));
}

TEST (synth_spectral_oscillator)
{
    // 1/n spectrum = saw: same harmonic proportions and same level as LYNX's saw
    lynx::SynthSettings saw;
    saw.osc[0] = { lynx::Waveform::saw, 0, 0.0f, 1.0f };
    saw.cutoff = 20000.0f;
    auto spectral = saw;
    spectral.osc[0].wave = lynx::Waveform::spectral;
    for (int n = 1; n <= lynx::SynthSettings::spectrumSize; ++n)
        spectral.spectrum[(std::size_t) n - 1] = 1.0f / (float) n;
    const auto a = renderSynthNote (saw, 57, 24000), b = renderSynthNote (spectral, 57, 24000); // 220 Hz
    for (const auto h : { 1, 2, 3, 5 })
    {
        const auto ea = goertzel (a, 220.0 * h, 4800, 24000), eb = goertzel (b, 220.0 * h, 4800, 24000);
        CHECK (std::abs (10.0 * std::log10 (eb / ea)) < 0.5); // within 0.5 dB
    }
    // odd only: no 2nd harmonic
    auto odd = spectral;
    for (int n = 2; n <= lynx::SynthSettings::spectrumSize; n += 2)
        odd.spectrum[(std::size_t) n - 1] = 0.0f;
    const auto c = renderSynthNote (odd, 57, 24000);
    CHECK (goertzel (c, 440.0, 4800, 24000) < 1e-4 * goertzel (c, 220.0, 4800, 24000));
    // high note: the band-limited tables don't pass Nyquist (no aliasing below the fundamental)
    const auto d = renderSynthNote (spectral, 105, 24000); // 3520 Hz
    CHECK (goertzel (d, 1000.0, 4800, 24000) < 1e-3 * goertzel (d, 3520.0, 4800, 24000));
}

TEST (compressor_sidechain_ducks_and_orders_mixer)
{
    // the detector listens to the side input: quiet signal on the track, loud kick on the sidechain -> reduction
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::compressor);
    slot.params[0] = 0.5f; // threshold
    slot.params[1] = 1.0f; // high ratio
    slot.params[4] = 0.0f;
    auto run = [&] (bool withSidechain) {
        auto fx = lynx::makeEffect (lynx::EffectKind::compressor);
        fx->prepare (48000.0);
        std::vector<float> l (4800, 0.1f), r (4800, 0.1f), kick (4800, 1.0f);
        if (withSidechain)
            fx->setSidechain (kick.data(), kick.data());
        fx->process (l.data(), r.data(), 4800, slot.params, 120.0);
        return l.back();
    };
    const auto alone = run (false), ducked = run (true);
    CHECK (ducked < 0.5f * alone);

    // order: track 5 listens to 7 via sidechain -> 7 comes before 5
    auto song = lynx::Song::makeDefault();
    while (song.mixer.size() < 8)
        song.mixer.push_back ({});
    auto comp = lynx::makeEffectSlot (lynx::EffectKind::compressor);
    comp.sidechain = 7;
    song.mixer[5].effects.push_back (comp);
    const auto order = song.mixerOrder();
    const auto at = [&] (int t) { return std::find (order.begin(), order.end(), t) - order.begin(); };
    CHECK (at (7) < at (5));
}

TEST (pitch_correction_pulls_to_allowed_note)
{
    auto run = [] (const lynx::EffectParams& p, double hz) {
        auto fx = lynx::makeEffect (lynx::EffectKind::pitchCorrect);
        fx->prepare (48000.0);
        std::vector<float> l (48000 * 2), r;
        for (std::size_t i = 0; i < l.size(); ++i)
            l[i] = 0.5f * (float) std::sin (2.0 * 3.141592653589793 * hz * (double) i / 48000.0);
        r = l;
        fx->process (l.data(), r.data(), (int) l.size(), p, 120.0);
        // frequency from zero crossings in the last second
        int crossings = 0;
        for (std::size_t i = 48000 + 1; i < l.size(); ++i)
            crossings += (l[i - 1] < 0.0f) != (l[i] < 0.0f);
        return crossings / 2.0;
    };
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::pitchCorrect); // chromatic, instant, A 440
    CHECK (std::abs (run (slot.params, 452.0) - 440.0) < 4.0);
    for (int k = 0; k < 12; ++k)
        slot.params[(std::size_t) (5 + k)] = k == 0 ? 1.0f : 0.0f; // only C
    CHECK (std::abs (run (slot.params, 452.0) - 523.25) < 5.0);  // C5 (523 Hz) is closer than C4 (262)
    slot.params[2] = 1.0f; // turn everything off: passes unchanged
    CHECK (std::abs (run (slot.params, 452.0) - 452.0) < 4.0);
}

TEST (pitch_correction_bypass_is_a_pure_delay)
{
    // FL's Pitcher with Bypass All still runs its shifter: the plain path is an exact 9-sample delay, the formant
    // path (256-Hann overlap-add) an exact 257-sample delay, and Gender changes nothing without a shift
    // (REA_rodada30 §2.4/2.5); both are reported as latency
    std::vector<float> in (48000);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = 0.5f * (float) std::sin (2.0 * 3.141592653589793 * 440.0 * (double) i / 48000.0)
              + 0.2f * (float) std::sin (2.0 * 3.141592653589793 * 1234.5 * (double) i / 48000.0);
    auto run = [&] (bool formant, float gender, int& latency) {
        auto fx = lynx::makeEffect (lynx::EffectKind::pitchCorrect);
        fx->prepare (48000.0);
        auto slot = lynx::makeEffectSlot (lynx::EffectKind::pitchCorrect);
        slot.params[2] = 1.0f;
        slot.params[29] = formant ? 1.0f : 0.0f;
        slot.params[30] = gender;
        auto l = in, r = in;
        for (std::size_t i = 0; i < l.size(); i += 480)
            fx->process (l.data() + i, r.data() + i, 480, slot.params, 120.0);
        latency = fx->latencySamples();
        return l;
    };
    int latency = 0;
    auto plain = run (false, 0.5f, latency);
    CHECK (latency == 9);
    auto worst = 0.0f;
    for (std::size_t i = 4000; i < in.size(); ++i)
        worst = std::max (worst, std::abs (plain[i] - in[i - 9]));
    CHECK (worst < 1e-5f);
    auto formant = run (true, 0.8f, latency);
    CHECK (latency == 257);
    worst = 0.0f;
    for (std::size_t i = 4000; i < in.size(); ++i)
        worst = std::max (worst, std::abs (formant[i] - in[i - 257]));
    CHECK (worst < 1e-5f);
}

TEST (pitch_correction_formant_path)
{
    // Formant on: PSOLA keeps the correction (output pitch = grain spacing) while Gender resamples the grains:
    // the spectrum of a saw moves up with Gender > 0.5 and down with Gender < 0.5
    auto run = [] (float gender) {
        auto fx = lynx::makeEffect (lynx::EffectKind::pitchCorrect);
        fx->prepare (48000.0);
        auto slot = lynx::makeEffectSlot (lynx::EffectKind::pitchCorrect); // chromatic, instant, A 440
        slot.params[29] = 1.0f;
        slot.params[30] = gender;
        std::vector<float> l (48000 * 2), r;
        for (std::size_t i = 0; i < l.size(); ++i)
            l[i] = 0.5f * (float) (2.0 * std::fmod (452.0 * (double) i / 48000.0, 1.0) - 1.0); // saw
        r = l;
        fx->process (l.data(), r.data(), (int) l.size(), slot.params, 120.0);
        l.erase (l.begin(), l.begin() + 48000);
        return l;
    };
    // period by autocorrelation (lags 60…160 = 300…800 Hz)
    auto pitch = [] (const std::vector<float>& x) {
        auto best = 0.0, bestLag = 0.0;
        for (int lag = 60; lag <= 160; ++lag)
        {
            auto sum = 0.0;
            for (std::size_t i = 0; i + (std::size_t) lag < x.size(); ++i)
                sum += (double) x[i] * x[i + (std::size_t) lag];
            if (sum > best)
            {
                best = sum;
                bestLag = lag;
            }
        }
        return 48000.0 / bestLag;
    };
    // brightness: energy of the first difference relative to the energy
    auto brightness = [] (const std::vector<float>& x) {
        auto e = 0.0, d = 0.0;
        for (std::size_t i = 1; i < x.size(); ++i)
        {
            e += (double) x[i] * x[i];
            d += (double) (x[i] - x[i - 1]) * (x[i] - x[i - 1]);
        }
        return d / e;
    };
    const auto neutral = run (0.5f), up = run (0.8f), down = run (0.3f);
    CHECK (std::abs (pitch (neutral) - 440.0) < 5.0); // 452 Hz pulled to A
    CHECK (std::abs (pitch (up) - 440.0) < 5.0);      // Gender does not move the pitch
    CHECK (brightness (up) > 1.5 * brightness (neutral));
    CHECK (brightness (down) < 0.7 * brightness (neutral));
}

TEST (parallel_container_inner_links_modulate)
{
    // one branch: utility (gain) + a control-only auto-pan LFO (1 beat); the link maps the LFO 0..1 onto the
    // utility gain −24 dB..0 dB, so the level must swing; without the link it stays put
    auto container = lynx::makeEffectSlot (lynx::EffectKind::parallel);
    auto gain = lynx::makeEffectSlot (lynx::EffectKind::utility);
    auto lfo = lynx::makeEffectSlot (lynx::EffectKind::autoPan);
    lfo.params[0] = 1.0f / 64.0f;
    lfo.params[4] = 1.0f; // control only: audio passes unchanged
    container.branches.push_back ({ { gain, lfo }, 1.0f });
    lynx::EffectLink link;
    link.source = 1;
    link.param = 0;
    for (int i = 0; i <= 32; ++i)
        link.curve.push_back (0.5f * (float) i / 32.0f);

    auto levels = [&] (bool linked) {
        auto slot = container;
        if (linked)
            slot.links.push_back (link);
        auto fx = lynx::makeParallelEffect (slot);
        fx->prepare (48000.0);
        float lo = 1e9f, hi = 0.0f;
        for (int block = 0; block < 94; ++block) // ~1 beat at 120 BPM in 256-sample blocks
        {
            std::vector<float> l (256, 1.0f), r (256, 1.0f);
            fx->setSongPosition (block * 256.0 / 48000.0 * 2.0 * 960.0); // the engine positions every block
            fx->process (l.data(), r.data(), 256, slot.params, 120.0);
            lo = std::min (lo, std::abs (l[128]));
            hi = std::max (hi, std::abs (l[128]));
        }
        return std::pair { lo, hi };
    };
    const auto [lo, hi] = levels (true);
    CHECK (hi > 0.8f && lo < 0.12f);
    const auto [lo0, hi0] = levels (false);
    CHECK (std::abs (hi0 - lo0) < 1e-4f);
}

TEST (synth_spectrum_decays_per_harmonic)
{
    // harmonic 1 holds, harmonic 3 falls 60 dB in 0.2 s: early the 3rd is as loud as the 1st, later it is gone
    lynx::SynthSettings s;
    s.osc[0] = { lynx::Waveform::spectral, 0, 0.0f, 1.0f };
    s.cutoff = 20000.0f;
    s.decay = 0.0f;
    s.sustain = 1.0f;
    s.spectrum[0] = 1.0f;
    s.spectrum[2] = 1.0f;
    s.spectrumDecay[2] = 0.2f;
    const auto x = renderSynthNote (s, 57, 48000); // 220 Hz, 48 kHz, 1 s
    // equal 2400-sample windows (Goertzel power grows with the window): 10…60 ms and 500…550 ms
    const auto early1 = goertzel (x, 220.0, 480, 2880), early3 = goertzel (x, 660.0, 480, 2880);
    const auto late1 = goertzel (x, 220.0, 24000, 26400), late3 = goertzel (x, 660.0, 24000, 26400);
    CHECK (early3 > 0.05 * early1); // ≈ −9 dB on average over 10…60 ms
    CHECK (late3 < 1e-4 * late1);
    CHECK (std::abs (10.0 * std::log10 (late1 / early1)) < 1.0); // the 1st holds
    // without per-harmonic decay the spectrum stays as it is
    s.spectrumDecay[2] = 0.0f;
    const auto y = renderSynthNote (s, 57, 48000);
    CHECK (std::abs (10.0 * std::log10 (goertzel (y, 660.0, 24000, 26400) / goertzel (y, 660.0, 480, 2880))) < 1.0);
}

TEST (worker_pool_runs_each_index_once_per_round)
{
    // rounds of alternating sizes (a stalled worker must never run an index of a later round), each index
    // exactly once, and nothing running after parallelFor returns
    lynx::WorkerPool pool (3);
    std::vector<std::atomic<int>> hits (64);
    std::atomic<int> round { 0 }, outside { 0 }, wrongRound { 0 };
    std::atomic<bool> open { false };
    int failures = 0;
    for (int r = 1; r <= 20000; ++r)
    {
        const auto n = (r % 3 == 0) ? 64 : (r % 3 == 1 ? 2 : 9);
        for (int i = 0; i < n; ++i)
            hits[(std::size_t) i].store (0);
        round.store (r);
        open.store (true);
        auto fn = [&, r] (int i) {
            if (! open.load())
                outside.fetch_add (1);
            if (round.load() != r)
                wrongRound.fetch_add (1);
            hits[(std::size_t) i].fetch_add (1);
        };
        pool.parallelFor (n, fn);
        open.store (false);
        for (int i = 0; i < n; ++i)
            failures += hits[(std::size_t) i].load() != 1;
    }
    CHECK (failures == 0);
    CHECK (outside.load() == 0);
    CHECK (wrongRound.load() == 0);
}

TEST (worker_pool_wakes_sleeping_workers)
{
    // rounds far apart (the workers have gone to sleep on the semaphore in between): every index still runs once,
    // and the sleeping workers are woken (REA round 87: the Windows build used to poll with growing Sleep()s)
    lynx::WorkerPool pool (3);
    std::vector<std::atomic<int>> hits (16);
    int failures = 0;
    for (int r = 0; r < 12; ++r)
    {
        std::this_thread::sleep_for (std::chrono::microseconds (lynx::WorkerPool::spinMicros * 4));
        for (auto& h : hits)
            h.store (0);
        auto fn = [&] (int i) {
            std::this_thread::sleep_for (std::chrono::microseconds (200)); // long enough for woken workers to join
            hits[(std::size_t) i].fetch_add (1);
        };
        pool.parallelFor (16, fn);
        for (auto& h : hits)
            failures += h.load() != 1;
    }
    CHECK (failures == 0);
    CHECK (pool.wakeUps() > 0);
    CHECK (pool.indicesRunByWorkers() > 0);
}

TEST (worker_pool_park_sends_spinning_workers_to_sleep)
{
    // round 116: after park() (end of an audio block) the workers sleep at once instead of spinning for spinMicros, so
    // the next parallelFor, well inside the spin window, has to wake them; and work right after park() still runs once
    lynx::WorkerPool pool (3);
    std::vector<std::atomic<int>> hits (8);
    int failures = 0;
    const auto before = pool.wakeUps();
    for (int r = 0; r < 40; ++r)
    {
        for (auto& h : hits)
            h.store (0);
        auto fn = [&] (int i) {
            std::this_thread::sleep_for (std::chrono::microseconds (100));
            hits[(std::size_t) i].fetch_add (1);
        };
        pool.parallelFor (8, fn);
        pool.park();
        if (r % 2 == 0)
            std::this_thread::sleep_for (std::chrono::microseconds (lynx::WorkerPool::spinMicros / 5));
        for (auto& h : hits)
            failures += h.load() != 1;
    }
    CHECK (failures == 0);
    CHECK (pool.wakeUps() - before >= 20); // parked workers were woken, not found spinning
}

TEST (realtime_stats_count_deadline_misses_and_late_calls)
{
    lynx::RtStats stats;
    // 10 ms deadline: 3 ms, 9 ms (80-100 %), 12 ms (over), then a call 40 ms after the previous one (late)
    stats.record (100.0, 3.0, 10.0);
    stats.record (110.0, 9.0, 10.0);
    stats.record (120.0, 12.0, 10.0);
    stats.record (160.0, 2.0, 10.0);
    const auto s = stats.snapshot();
    CHECK (s.callbacks == 4);
    CHECK (s.overBudget == 1);
    CHECK (s.nearBudget == 1);
    CHECK (s.lateCalls == 1);
    CHECK (std::abs (s.maxLoad - 1.2) < 1e-9);
    CHECK (std::abs (s.averageLoad - 26.0 / 40.0) < 1e-3);
    CHECK (s.histogram[3] == 1 && s.histogram[9] == 1 && s.histogram[12] == 1 && s.histogram[2] == 1);
    CHECK (std::abs (stats.takeWindowMaxLoad() - 1.2) < 1e-9 && stats.takeWindowMaxLoad() == 0.0);
    // a device restart is not a late call
    stats.deviceRestarted();
    stats.record (5000.0, 1.0, 10.0);
    CHECK (stats.snapshot().lateCalls == 1);
    stats.reset();
    CHECK (stats.snapshot().callbacks == 0 && stats.snapshot().maxLoad == 0.0);
}

TEST (safe_default_buffer_size)
{
    const std::vector<int> wasapi { 144, 160, 192, 224, 256, 288, 320, 384, 448, 480, 512, 576, 640, 1024, 2048 };
    CHECK (lynx::prefs::safeBufferSize (wasapi, 480, 48000.0) == 512);   // the 10 ms default becomes 512
    CHECK (lynx::prefs::safeBufferSize (wasapi, 1024, 48000.0) == 1024); // already larger: kept
    CHECK (lynx::prefs::safeBufferSize (wasapi, 480, 96000.0) == 1024);  // ~10.7 ms at 96 kHz
    CHECK (lynx::prefs::safeBufferSize ({ 64, 128, 256 }, 128, 44100.0) == 256); // nothing reaches 512: the largest
    CHECK (lynx::prefs::safeBufferSize ({}, 0, 44100.0) == 0);
}

TEST (linux_audio_driver_choice_and_pipewire_helpers)
{
    // round 161: a first start on Linux opens PipeWire, then JACK, then ALSA
    using lynx::prefs::DriverKind;
    CHECK (lynx::prefs::driverKind ("PipeWire") == DriverKind::pipewire);
    CHECK (lynx::prefs::preferredLinuxDriver ({ { "ALSA", true }, { "JACK", false }, { "PipeWire", true } }) == "PipeWire");
    CHECK (lynx::prefs::preferredLinuxDriver ({ { "ALSA", true }, { "JACK", true } }) == "JACK");
    CHECK (lynx::prefs::preferredLinuxDriver ({ { "ALSA", true }, { "JACK", false }, { "PipeWire", false } }) == "ALSA");
    CHECK (lynx::prefs::preferredLinuxDriver ({ { "ALSA", false }, { "Bela", true } }) == "Bela");
    CHECK (lynx::prefs::preferredLinuxDriver ({ { "ALSA", false } }).empty());
    CHECK (lynx::prefs::pipewireLatency (256, 48000) == "256/48000");
    CHECK (lynx::prefs::pipewireLatency (0, 0) == "1/1");
    CHECK (lynx::prefs::pipewireChannelName ("FL", 0) == "Left");
    CHECK (lynx::prefs::pipewireChannelName ("FR", 1) == "Right");
    CHECK (lynx::prefs::pipewireChannelName ("MONO", 0) == "Mono");
    CHECK (lynx::prefs::pipewireChannelName ("AUX0", 0) == "Aux 1");
    CHECK (lynx::prefs::pipewireChannelName ("AUX15", 15) == "Aux 16");
    CHECK (lynx::prefs::pipewireChannelName ("AUXX", 2) == "AUXX");
    CHECK (lynx::prefs::pipewireChannelName ("", 2) == "Channel 3");
    CHECK (lynx::prefs::pipewireDefaultName (R"({"name":"alsa_output.pci-0000_00_1f.3.analog-stereo"})") == "alsa_output.pci-0000_00_1f.3.analog-stereo");
    CHECK (lynx::prefs::pipewireDefaultName (R"({ "name" : "a\"b" })") == "a\"b");
    CHECK (lynx::prefs::pipewireDefaultName ("{}").empty());
    CHECK (lynx::prefs::pipewireDefaultName (R"({"name":"unterminated)").empty());
    CHECK ((lynx::prefs::uniqueDeviceNames ({ "USB Audio", "HDMI", "USB Audio", "USB Audio" })
            == std::vector<std::string> { "USB Audio", "HDMI", "USB Audio (2)", "USB Audio (3)" }));
}

TEST (driver_kind_and_shared_mode_buffer_sizes)
{
    // REA round 97: classic shared WASAPI mixes in whole engine periods, so sizes below it are not offered
    using lynx::prefs::DriverKind;
    CHECK (lynx::prefs::driverKind ("Windows Audio") == DriverKind::wasapiShared);
    CHECK (lynx::prefs::driverKind ("Windows Audio (Exclusive Mode)") == DriverKind::wasapiExclusive);
    CHECK (lynx::prefs::driverKind ("Windows Audio (Low Latency Mode)") == DriverKind::wasapiLowLatency);
    CHECK (lynx::prefs::driverKind ("ASIO") == DriverKind::asio);
    CHECK (lynx::prefs::driverKind ("LYNX ASIO") == DriverKind::lynxAsio);
    CHECK (lynx::prefs::driverKind ("DirectSound") == DriverKind::directSound);
    CHECK (lynx::prefs::driverKind ("ALSA") == DriverKind::alsa);
    CHECK (lynx::prefs::driverKind ("JACK") == DriverKind::jack);
    CHECK (lynx::prefs::driverKind ("CoreAudio") == DriverKind::coreAudio);
    CHECK (lynx::prefs::driverKind ("Some Other Driver") == DriverKind::other);
    const std::vector<int> offered { 144, 192, 256, 480, 512, 1024 };
    CHECK ((lynx::prefs::usableBufferSizes (DriverKind::wasapiShared, offered, 480) == std::vector<int> { 480, 512, 1024 }));
    CHECK (lynx::prefs::usableBufferSizes (DriverKind::wasapiExclusive, offered, 480) == offered);   // the device's real periods
    CHECK (lynx::prefs::usableBufferSizes (DriverKind::wasapiLowLatency, offered, 480) == offered);  // engine periods
    CHECK (lynx::prefs::usableBufferSizes (DriverKind::wasapiShared, { 64, 128 }, 480) == (std::vector<int> { 64, 128 })); // never empty
    CHECK (lynx::prefs::usableBufferSizes (DriverKind::wasapiShared, offered, 0) == offered);
}

TEST (linear_phase_convolver_matches_direct_convolution)
{
    // the block FFT convolver (cached twiddles, REA round 87) equals the direct convolution with its kernel
    lynx::peqfl::FftConvolver c;
    c.init (256);
    std::vector<float> table (129, 1.0f);
    for (int k = 40; k < 129; ++k)
        table[(std::size_t) k] = 0.25f; // a high shelf cut
    c.setKernel (table);
    // impulse response through step()
    std::vector<float> ir (600, 0.0f);
    for (int i = 0; i < 600; ++i)
    {
        float l = i == 0 ? 1.0f : 0.0f, r = 0.0f;
        c.step (l, r);
        ir[(std::size_t) i] = l;
    }
    // then a noise signal, compared with direct convolution by that impulse response
    lynx::peqfl::FftConvolver d;
    d.init (256);
    d.setKernel (table);
    std::vector<float> x (2000), y (2000);
    std::uint32_t seed = 1;
    for (auto& v : x)
        v = (float) ((seed = seed * 1664525u + 1013904223u) >> 8) / 16777216.0f - 0.5f;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        float l = x[i], r = 0.0f;
        d.step (l, r);
        y[i] = l;
    }
    double worst = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        double direct = 0.0;
        for (std::size_t k = 0; k < ir.size() && k <= i; ++k)
            direct += (double) ir[k] * (double) x[i - k];
        worst = std::max (worst, std::abs (direct - (double) y[i]));
    }
    CHECK (worst < 1e-4);
    // and it delays by 3n/4 with the kernel's centre there
    const auto peak = (std::size_t) (std::max_element (ir.begin(), ir.end(), [] (float a, float b) { return std::abs (a) < std::abs (b); }) - ir.begin());
    CHECK (peak == 192);
}

TEST (patcher_container_is_reused_across_snapshots)
{
    // an unchanged Patcher keeps its built effect (its children's tails, delay lines and PDC stay put), and a
    // linear-phase Frequency Splitter reports its latency before its first block (REA round 87: it used to be
    // rebuilt on every snapshot and its latency appeared one block late, re-planning PDC in a loop)
    auto song = lynx::Song::makeDefault();
    if (song.mixer.size() < 2)
        song.mixer.resize (2);
    lynx::EffectSlot splitter;
    splitter.kind = lynx::EffectKind::frequencySplitter;
    splitter.params = lynx::makeEffectSlot (lynx::EffectKind::frequencySplitter).params;
    splitter.params[4] = 0.6f; // linear-phase mode
    splitter.params[6] = 0.0f; // quality 0
    lynx::EffectSlot patcher;
    patcher.kind = lynx::EffectKind::parallel;
    patcher.branches.push_back ({ { splitter }, 1.0f });
    patcher.branches.push_back ({ {}, 0.5f });
    song.mixer[1].effects = { patcher };
    const auto a = lynx::RackState::build (song, nullptr, 44100.0);
    const auto b = lynx::RackState::build (song, a.get(), 44100.0);
    CHECK (a->effects[1][0] == b->effects[1][0]);
    CHECK (a->effects[1][0]->latencySamples() == 384); // 3/4 of the 512-point FFT, before any block
    CHECK (a->latencies == a->currentLatencies());
    song.mixer[1].effects[0].branches[0].chain[0].params[0] = 0.25f; // a child's parameter changed: rebuilt
    const auto c = lynx::RackState::build (song, b.get(), 44100.0);
    CHECK (c->effects[1][0] != b->effects[1][0]);
    song.mixer[1].effects[0].branches[1].gain = 0.75f;
    const auto d = lynx::RackState::build (song, c.get(), 44100.0);
    CHECK (d->effects[1][0] != c->effects[1][0]);
}

TEST (ladder_filter_follows_toxic_law)
{
    // the 4-pole ladder: −24 dB/oct far above the cutoff (vs −12 for the SVF), resonance raises the corner, and
    // the input saturation keeps it bounded even at full resonance
    auto level = [] (bool ladder, float res, double hz) {
        lynx::SynthSettings s;
        s.osc[0] = { lynx::Waveform::sine, 0, 0.0f, 1.0f };
        s.cutoff = 500.0f;
        s.resonance = res;
        s.ladder = ladder;
        const auto key = (int) std::lround (69.0 + 12.0 * std::log2 (hz / 440.0));
        const auto x = renderSynthNote (s, key, 48000);
        double e = 0.0, peak = 0.0;
        for (std::size_t i = 24000; i < 48000; ++i)
            e += (double) x[i] * x[i], peak = std::max (peak, (double) std::abs (x[i]));
        return std::pair { std::sqrt (e / 24000.0), peak };
    };
    const auto low = level (true, 0.0f, 110.0).first, high = level (true, 0.0f, 3520.0).first;   // 3 octaves above 500 Hz… 2.8
    const auto svfHigh = level (false, 0.0f, 3520.0).first / level (false, 0.0f, 110.0).first;
    CHECK (20.0 * std::log10 (high / low) < -55.0);               // ≈ 24 dB/oct over ~2.8 octaves
    CHECK (20.0 * std::log10 (svfHigh) > -45.0);                  // the 2-pole SVF falls much less
    const auto corner0 = level (true, 0.0f, 523.25).first, corner1 = level (true, 0.6f, 523.25).first;
    CHECK (corner1 > 1.5 * corner0);                               // resonance boosts near the cutoff
    const auto loud = level (true, 1.0f, 523.25);
    CHECK (std::isfinite (loud.first) && loud.second < 20.0);      // self-oscillation stays bounded
}

TEST (synth_spectrum_settles_on_target)
{
    // ALT DECAY-like morph: harmonic 3 starts at 1 and settles on 0.25 (−12 dB) instead of fading out
    lynx::SynthSettings s;
    s.osc[0] = { lynx::Waveform::spectral, 0, 0.0f, 1.0f };
    s.cutoff = 20000.0f;
    s.decay = 0.0f;
    s.sustain = 1.0f;
    s.spectrum[0] = 1.0f;
    s.spectrum[2] = 1.0f;
    s.spectrumDecay[2] = 0.1f;
    s.spectrumTarget[2] = 0.25f;
    const auto x = renderSynthNote (s, 57, 48000);
    const auto ratio = goertzel (x, 660.0, 24000, 26400) / goertzel (x, 220.0, 24000, 26400);
    CHECK (std::abs (10.0 * std::log10 (ratio) - 20.0 * std::log10 (0.25)) < 1.0);
}

TEST (kepler_chorus_is_stereo_and_keeps_level)
{
    // chorus I on a sine: L and R differ (opposite LFO phases), the level stays close to the dry one
    lynx::SynthSettings s;
    s.osc[0] = { lynx::Waveform::sine, 0, 0.0f, 1.0f };
    s.cutoff = 20000.0f;
    auto dry = s;
    s.chorus = 1;
    auto render = [] (const lynx::SynthSettings& set) {
        auto inst = lynx::makeInstrument (lynx::InstrumentKind::synth);
        inst->prepare (48000.0);
        lynx::ChannelDef c;
        c.synth = set;
        inst->configure (c, 120.0);
        inst->noteOn (69, 1.0f);
        std::vector<float> l (96000), r (96000);
        inst->render (l.data(), r.data(), 96000, 1.0f, 1.0f);
        return std::pair { l, r };
    };
    const auto [l, r] = render (s);
    const auto [dl, dr] = render (dry);
    double diff = 0.0, e = 0.0, ed = 0.0;
    for (std::size_t i = 48000; i < 96000; ++i)
        diff += std::abs (l[i] - r[i]), e += (double) l[i] * l[i], ed += (double) dl[i] * dl[i];
    CHECK (diff / 48000.0 > 1e-3);
    CHECK (std::abs (10.0 * std::log10 (e / ed)) < 3.0);
}

TEST (fl_pan_law_matches_love_philter_dll)
{
    // values printed by the installation's Love Philter pan routine for vol 0.8 (REA_rodada27 §3.1, lppan.txt)
    struct Row { double pan, ll, rr, rl, lr; };
    const Row rows[] = { { -1.5, 0.239782482, -0.432956904, 0.805467963, 0.0 }, { -0.5, 0.805467963, 0.432956904, 0.239782482, 0.0 },
                         { 0.0, 0.8, 0.8, 0.0, 0.0 }, { 0.75, 0.220719516, 0.707889318, 0.0, 0.401742548 },
                         { 1.0, 0.0, 0.565685451, 0.0, 0.565685451 } };
    for (const auto& row : rows)
    {
        float ll, rl, lr, rr;
        lynx::flPanMatrix (row.pan, ll, rl, lr, rr);
        CHECK (std::abs (0.8 * ll - row.ll) < 1e-6 && std::abs (0.8 * rr - row.rr) < 1e-6);
        CHECK (std::abs (0.8 * rl - row.rl) < 1e-6 && std::abs (0.8 * lr - row.lr) < 1e-6);
    }
}

TEST (effector_units_match_fl_dll_impulse_responses)
{
    // impulse responses of the installation's Effector engine (REA_rodada27 §2, eu_<kind>_<X>_<Y>.txt; LFO off,
    // wet 1, blocks of 256): samples 1, 10, 100 and 1000
    struct Case { int kind; float x, y; double v[4]; };
    const Case cases[] = { { 3, 0.5f, 0.5f, { -0.603055418, 0.159798339, -0.0055043716, -1.06241259e-06 } },
                           { 4, 0.2f, 0.5f, { 4.68885949e-26, 4.20032622e-19, 0.000487731799, 2.27715846e-09 } },
                           { 4, 0.8f, 0.3f, { -0.293783396, -0.0306414738, -0.00068128563, 1.358577e-15 } } };
    for (const auto& c : cases)
    {
        auto fx = lynx::effector::make();
        fx->prepare (44100.0);
        lynx::EffectParams p {};
        p[lynx::effector::type] = c.kind / 11.0f;
        p[lynx::effector::wetMix] = 1.0f;
        p[lynx::effector::paramX] = c.x;
        p[lynx::effector::paramY] = c.y;
        p[lynx::effector::modX] = p[lynx::effector::modY] = 0.5f;
        p[lynx::effector::modRate] = 0.5f;
        p[lynx::effector::inputLevel] = 1.0f;
        p[lynx::effector::outputGain] = 0.5f;
        std::vector<float> l (1024, 0.0f), r (1024, 0.0f);
        l[0] = r[0] = 1.0f;
        for (int d = 0; d < 1024; d += 256)
            fx->process (l.data() + d, r.data() + d, 256, p, 120.0);
        const int at[4] { 1, 10, 100, 1000 };
        for (int k = 0; k < 4; ++k)
            CHECK (std::abs (l[(std::size_t) at[k]] - c.v[k]) < 2e-7 + 1e-5 * std::abs (c.v[k]));
    }
}

TEST (harmless_stereo_tremolo_and_vibrato)
{
    // tremolo d = 1: each side swings between 0 and 1.25 at 5 Hz, L and R in anti-phase; vibrato modulates the pitch
    lynx::SynthSettings s;
    s.osc[0] = { lynx::Waveform::sine, 0, 0.0f, 1.0f };
    s.cutoff = 20000.0f;
    s.stereoTremolo = 1.0f;
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::synth);
    inst->prepare (48000.0);
    lynx::ChannelDef c;
    c.synth = s;
    inst->configure (c, 120.0);
    inst->noteOn (69, 1.0f);
    std::vector<float> l (48000), r (48000);
    inst->render (l.data(), r.data(), 48000, 1.0f, 1.0f);
    auto envelope = [] (const std::vector<float>& x, int at) { float m = 0.0f; for (int i = at; i < at + 120; ++i) m = std::max (m, std::abs (x[(std::size_t) i])); return m; };
    // θ = 2π·2.5·t: at t = 0.4 s cos = 1 (L peak 1.25×), sin = 0 (R near 0); at t = 0.5 s the opposite
    CHECK (envelope (l, 19200) > 4.0f * envelope (r, 19200));
    CHECK (envelope (r, 24000) > 4.0f * envelope (l, 24000));

    // vibrato 100 cents: zero crossings vary in spacing; without it they are regular
    auto crossingsSpread = [] (float cents) {
        lynx::SynthSettings v;
        v.osc[0] = { lynx::Waveform::sine, 0, 0.0f, 1.0f };
        v.cutoff = 20000.0f;
        v.vibratoCents = cents;
        const auto x = renderSynthNote (v, 69, 48000);
        std::vector<int> zc;
        for (int i = 24000; i < 48000; ++i)
            if (x[(std::size_t) i - 1] < 0.0f && x[(std::size_t) i] >= 0.0f)
                zc.push_back (i);
        int lo = 1 << 30, hi = 0;
        for (std::size_t k = 1; k < zc.size(); ++k)
            lo = std::min (lo, zc[k] - zc[k - 1]), hi = std::max (hi, zc[k] - zc[k - 1]);
        return hi - lo;
    };
    CHECK (crossingsSpread (0.0f) <= 1);
    CHECK (crossingsSpread (100.0f) >= 4);
}

TEST (physical_models_sound_and_end)
{
    // Sakura: a plucked string with default-ish parameters rings and fades after the release; Drumpad: a hit decays
    for (const auto kind : { lynx::InstrumentKind::sakura, lynx::InstrumentKind::drumpad })
    {
        lynx::ChannelDef c;
        c.instrument = kind;
        if (kind == lynx::InstrumentKind::sakura)
        {
            c.modelParams.assign (146, 0.5f);
            c.modelParams[111] = 3.0f;  // both strings at the note
            c.modelParams[113] = 0.5f;  // octave ×1
            c.modelParams[6] = c.modelParams[11] = 0.99f; // long decay
            c.modelParams[31] = 0.0f;   // noise exciter
        }
        else
        {
            c.modelParams.assign (38, 0.5f);
            c.modelParams[0] = 1.0f;    // retrigger noise
            c.modelParams[30] = c.modelParams[31] = c.modelParams[32] = c.modelParams[33] = 0.0f; // no velocity morph
        }
        auto inst = lynx::makeInstrument (kind);
        inst->prepare (48000.0);
        inst->configure (c, 120.0);
        inst->noteOn (60, 0.8f);
        std::vector<float> l (48000), r (48000);
        inst->render (l.data(), r.data(), 24000, 1.0f, 1.0f);
        inst->noteOff (60);
        inst->render (l.data() + 24000, r.data() + 24000, 24000, 1.0f, 1.0f);
        double early = 0.0;
        for (int i = 0; i < 12000; ++i)
            early += (double) l[(std::size_t) i] * l[(std::size_t) i];
        CHECK (early > 1e-6);
        CHECK (std::all_of (l.begin(), l.end(), [] (float v) { return std::isfinite (v); }));
        std::vector<float> tail (480000), tailR (480000);
        inst->render (tail.data(), tailR.data(), 480000, 1.0f, 1.0f); // 10 s later the voice has ended
        CHECK (inst->idle());
    }
}

TEST (squeeze_matches_fl_model)
{
    // Fruity Squeeze: Steps quantises, Mix 0 passes the input dry ×Volume/127; a silent block is left untouched
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::squeeze);
    slot.params[1] = 0.0f; // mix 0: dry only
    auto fx = lynx::makeEffect (lynx::EffectKind::squeeze);
    fx->prepare (44100.0);
    std::vector<float> l (512, 0.3f), r (512, -0.2f);
    fx->process (l.data(), r.data(), 512, slot.params, 120.0);
    CHECK (std::abs (l[100] - 0.3f) < 1e-6f && std::abs (r[100] + 0.2f) < 1e-6f); // Volume 127/127
    slot.params[1] = 1.0f;          // wet only
    slot.params[2] = 32000.0f / 32768.0f; // 32768^(1 − 32000/32768) ≈ 1.27 levels per unit → heavy quantisation
    std::vector<float> a (512), b (512);
    for (int i = 0; i < 512; ++i)
        a[(std::size_t) i] = b[(std::size_t) i] = 0.4f * (float) std::sin (i * 0.05);
    fx->process (a.data(), b.data(), 512, slot.params, 120.0);
    const auto levels = std::pow (32768.0f, 1.0f - 32000.0f / 32768.0f);
    for (int i = 0; i < 512; i += 37)
        CHECK (std::abs (a[(std::size_t) i] * levels - std::nearbyint (a[(std::size_t) i] * levels)) < 1e-3f);
    std::vector<float> z (512, 0.0f), z2 (512, 0.0f);
    fx->process (z.data(), z2.data(), 512, slot.params, 120.0);
    CHECK (z[10] == 0.0f);
}

TEST (sakura_matrix_and_effects)
{
    // REA_rodada29 §1: velocity → level follows 10^(2m)·0.05 (vel 1 vs 0.5 → ×10 in amplitude); the delay leaves
    // an echo after the note has ended and keeps the instrument from going idle until its tail is silent
    auto base = [] {
        lynx::ChannelDef c;
        c.instrument = lynx::InstrumentKind::sakura;
        c.modelParams.assign (146, 0.0f);
        const std::pair<int, float> values[] { { 0, 0.375f }, { 1, 0.5f }, { 3, 0.5f }, { 4, 1.0f }, { 6, 0.95f }, { 9, 0.3f },
                                               { 11, 0.95f }, { 14, 0.3f }, { 18, 0.5f }, { 19, 0.5f }, { 27, 0.5f }, { 28, 1.0f },
                                               { 29, 0.7f }, { 31, 1.0f }, { 32, 0.5f }, { 111, 3.0f }, { 113, 0.5f } };
        for (const auto& [i, v] : values)
            c.modelParams[(std::size_t) i] = v;
        return c;
    };
    auto render = [] (const lynx::ChannelDef& c, float velocity, int held, int total) {
        auto inst = lynx::makeInstrument (lynx::InstrumentKind::sakura);
        inst->prepare (48000.0);
        inst->configure (c, 120.0);
        inst->noteOn (60, velocity);
        std::vector<float> l ((std::size_t) total), r ((std::size_t) total);
        for (int s = 0; s < total; s += 256)
        {
            if (s == held)
                inst->noteOff (60);
            inst->render (l.data() + s, r.data() + s, std::min (256, total - s), 1.0f, 1.0f);
        }
        return std::make_pair (l, std::move (inst));
    };
    auto energy = [] (const std::vector<float>& x, int from, int to) {
        double e = 0.0;
        for (int i = from; i < to; ++i)
            e += (double) x[(std::size_t) i] * x[(std::size_t) i];
        return e;
    };
    auto c = base();
    c.modelParams[79] = 6.0f; // slot 1: velocity → level, amount 1
    c.modelParams[80] = 1.0f;
    c.modelParams[82] = 1.0f;
    const auto loud = energy (render (c, 1.0f, 24064, 24064).first, 0, 24064);
    const auto soft = energy (render (c, 0.5f, 24064, 24064).first, 0, 24064);
    CHECK (loud > 1e-6 && std::abs (loud / soft - 100.0) < 1.0);

    auto d = base();
    auto [dry, dryInst] = render (d, 0.8f, 4864, 72000);
    d.modelParams[63] = 1.0f; // delay on: time 0.5 → ≈ 0.52 s, wet 1, no feedback: one echo
    d.modelParams[64] = 0.5f;
    d.modelParams[69] = 1.0f;
    auto [wet, wetInst] = render (d, 0.8f, 4864, 72000);
    CHECK (energy (wet, 26000, 40000) > 100.0 * energy (dry, 26000, 40000) + 1e-9); // the note ends near 0.2 s
    CHECK (dryInst->idle() || energy (dry, 70000, 72000) < 1e-12);
    CHECK (! wetInst->idle()); // the echo tail has not been silent for 3 s yet
}

TEST (drumsynth_live_engine)
{
    // REA_rodada30 §1: key → patch slot, state parsing, the engine against our reference model (no DLL: synthesised
    // sources only, noise from the fallback seed), voice modes, patch level/pan
    CHECK (lynx::drumSynthKeyToSlot (48) == 0 && lynx::drumSynthKeyToSlot (71) == 23 && lynx::drumSynthKeyToSlot (0) == 24);
    CHECK (lynx::drumSynthKeyToSlot (47) == 71 && lynx::drumSynthKeyToSlot (72) == 72 && lynx::drumSynthKeyToSlot (119) == 119);
    CHECK (lynx::drumSynthKeyToSlot (120) == -1);

    // a version 3 state: 120 names, 120 × 38 ints (send track −1 becomes 0 below version 4), flags
    std::vector<std::uint8_t> blob;
    auto put = [&] (std::uint32_t v) { for (int k = 0; k < 4; ++k) blob.push_back ((std::uint8_t) (v >> (8 * k))); };
    put (3);
    put (66);
    for (int i = 0; i < 120; ++i)
        blob.push_back (2), blob.push_back ('a'), blob.push_back ((std::uint8_t) ('0' + i % 10));
    for (int i = 0; i < 120 * 38; ++i)
        put ((std::uint32_t) (i % 38 == 36 ? -1 : i % 38 == 4 ? 300 : 0));
    put (0);
    const auto parsed = lynx::drumSynthLiveParams (blob);
    CHECK (parsed && (int) parsed->size() == lynx::drumSynthParamCount);
    CHECK (parsed && (*parsed)[36] == 0.0f && (*parsed)[38 * 7 + 4] == 300.0f && (*parsed)[(std::size_t) lynx::drumSynthEditorKeyIndex] == 66.0f);
    blob.resize (500);
    CHECK (! lynx::drumSynthLiveParams (blob));

    // patch on key 60 (slot 12): tone-mode Osc1 with sweep and click, noise-band Osc2, ring, 2 retriggers
    const int patch[38] { 100, 0, 10, -20, 300, 40, 50, 70, 90, 30, 600, 60, 10, 60, 80, -1, 0, 0, 2, 40, 60, 127, 0, 20, 0, 0, 0,
                          0, 0, 0, 0, 0, 0, 30, -1, 0, 0, 64 };
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::drumsynth;
    c.modelParams.assign ((std::size_t) lynx::drumSynthParamCount, 0.0f);
    for (int s = 0; s < 120; ++s)
        c.modelParams[(std::size_t) (38 * s + 15)] = c.modelParams[(std::size_t) (38 * s + 34)] = -1.0f;
    for (int i = 0; i < 38; ++i)
        c.modelParams[(std::size_t) (38 * 12 + i)] = (float) patch[i];
    auto render = [] (const lynx::ChannelDef& def, std::vector<int> keys, int total, float gl = 1.0f, float gr = 1.0f) {
        auto inst = lynx::makeInstrument (lynx::InstrumentKind::drumsynth);
        inst->configure (def, 120.0);
        inst->prepare (48000.0);
        std::vector<float> l ((std::size_t) total), r ((std::size_t) total);
        for (int s = 0; s < total; s += 512)
        {
            for (std::size_t k = 0; k + 1 < keys.size(); k += 2)
                if (keys[k + 1] == s)
                    inst->noteOn (keys[k], 100.0f / 128.0f);
            inst->render (l.data() + s, r.data() + s, std::min (512, total - s), gl, gr);
        }
        return std::make_tuple (l, r, std::move (inst));
    };
    {
        auto [l, r, inst] = render (c, { 60, 0 }, 9600);
        // reference model (REA round 30 dsl_model.py, seed 0x56CE, FL velocity 100, V = 1, centre: × g(100)·cos(π/4))
        const std::pair<int, float> golden[] { { 0, -0.0074092713f }, { 10, -0.13961405f }, { 100, -0.47290668f }, { 1000, -0.29188696f },
                                               { 3000, -0.22130175f }, { 6000, -0.049537566f }, { 9599, 0.010392102f } };
        for (const auto& [i, v] : golden)
            CHECK (std::abs (l[(std::size_t) i] - v) < 2e-6f && std::abs (r[(std::size_t) i] - v) < 2e-6f);
        std::vector<float> tail (48000 * 4), tailR (tail.size());
        inst->render (tail.data(), tailR.data(), (int) tail.size(), 1.0f, 1.0f);
        CHECK (inst->idle()); // the engine reported the voice finished
    }
    auto energy = [] (const std::vector<float>& x, int from, int to) {
        double e = 0.0;
        for (int i = from; i < to; ++i)
            e += (double) x[(std::size_t) i] * x[(std::size_t) i];
        return e;
    };
    // a key without a patch level (slot 0, level 0) and a key outside the 120 patches are silent
    CHECK (energy (std::get<0> (render (c, { 48, 0, 125, 0 }, 4096)), 0, 4096) == 0.0);
    // mono: a second hit on the same key cuts the first; poly: both ring together (Osc2 off: no noise, deterministic)
    auto tone = c;
    tone.modelParams[38 * 12 + 14] = 0.0f;
    const auto single = std::get<0> (render (tone, { 60, 0 }, 8192));
    const auto mono = std::get<0> (render (tone, { 60, 0, 60, 2048 }, 8192));
    auto poly = tone;
    poly.modelParams[38 * 12 + 24] = 1.0f;
    const auto both = std::get<0> (render (poly, { 60, 0, 60, 2048 }, 8192));
    double monoDiff = 0.0, polyDiff = 0.0;
    for (int i = 2048 + 300; i < 8192; ++i) // after the cut fade: mono = only the second hit, a copy of the first
        monoDiff += std::abs (mono[(std::size_t) i] - single[(std::size_t) i - 2048]), polyDiff += std::abs (both[(std::size_t) i] - single[(std::size_t) i - 2048]);
    CHECK (monoDiff < 1e-3 && polyDiff > 1.0);
    // patch pan +64 → right only; channel volume counts twice (FL applies Final.Vol in the engine and at the host)
    auto right = c;
    right.modelParams[38 * 12 + 1] = 64.0f;
    const auto [pl, pr, pi] = render (right, { 60, 0 }, 4096);
    CHECK (energy (pl, 0, 4096) < 1e-12 * energy (pr, 0, 4096) && energy (pr, 0, 4096) > 1.0);
    const auto full = std::get<0> (render (tone, { 60, 0 }, 4096));
    const auto half = std::get<0> (render (tone, { 60, 0 }, 4096, 0.5f, 0.5f));
    CHECK (std::abs (half[100] - 0.25f * full[100]) < 1e-6f && std::abs (full[100]) > 0.01f);
}

TEST (kepler_lfo_bpm_sync)
{
    // REA_rodada33 §3.4: with BPM Sync (k6 ≥ 0.5) the LFO1 period is n/192 beats, n = trunc((2^(7(1 − x)) − 1)·48.370079 + 1.1)
    std::vector<std::uint8_t> state (0x21 + 34 * 4, 0);
    auto putU32 = [&] (std::size_t o, std::uint32_t v) { std::memcpy (state.data() + o, &v, 4); };
    putU32 (12, 9);
    std::memcpy (state.data() + 16, "Kepler-1", 9);
    putU32 (0x19, 4);
    putU32 (0x1D, 34);
    float k[34] {};
    k[5] = 0.602085769f; // "Pads - Kepler" demo: n = 286
    k[6] = 1.0f;
    k[10] = 1.0f; // vibrato on
    k[33] = 1.0f;
    std::memcpy (state.data() + 0x21, k, sizeof k);
    lynx::SynthSettings s;
    float gain = 0.0f;
    CHECK (lynx::synthFromKepler (state, s, gain));
    CHECK (s.lfo[lynx::lfoPitch].sync && s.lfo[lynx::lfoPitch].secondsTiming);
    CHECK (std::abs (s.lfo[lynx::lfoPitch].period - 286.0f / 192.0f) < 1e-6f);
    k[5] = 0.0f; // slowest: 32 beats
    std::memcpy (state.data() + 0x21, k, sizeof k);
    CHECK (lynx::synthFromKepler (state, s, gain) && std::abs (s.lfo[lynx::lfoPitch].period - 32.0f) < 1e-6f);
    k[6] = 0.0f; // free rate in seconds
    std::memcpy (state.data() + 0x21, k, sizeof k);
    CHECK (lynx::synthFromKepler (state, s, gain) && ! s.lfo[lynx::lfoPitch].sync);
}

namespace
{
// A Harmor state written from scratch (REA_rodada11 §4.1 layout, version 18): part A only — a decaying-harmonics
// timbre, low-pass + resonance, filter envelope, 3-voice unison, vibrato, pluck, tremolo, volume envelope with a
// velocity map — and the global block. Every other curve is neutral.
// features (REA_rodada75): 1 arpeggio markers on the volume envelope, 2 noise resonance on filter 1, 4 "pluck replaces
// blur decay", 8 pluck blend, 16 harmonic randomness, 32 phaser + harmonizer masks
std::vector<std::uint8_t> harmorTestState (bool panLfo = true, bool image = false, int features = 0)
{
    std::vector<std::uint8_t> b;
    auto i32 = [&] (std::int32_t v) { for (int k = 0; k < 4; ++k) b.push_back ((std::uint8_t) ((std::uint32_t) v >> (8 * k))); };
    auto f64 = [&] (double v) { std::uint64_t u; std::memcpy (&u, &v, 8); for (int k = 0; k < 8; ++k) b.push_back ((std::uint8_t) (u >> (8 * k))); };
    struct Pt { double dx, y, tension; int mode; int arp = 0; };
    auto curve = [&] (const std::vector<Pt>& pts, std::array<std::int32_t, 5> hdr) {
        i32 (3), i32 (3), i32 ((std::int32_t) pts.size());
        for (const auto& p : pts)
        {
            f64 (p.dx), f64 (p.y);
            const auto t = (float) p.tension;
            std::uint32_t u;
            std::memcpy (&u, &t, 4);
            i32 ((std::int32_t) u);
            b.push_back ((std::uint8_t) p.mode), b.push_back ((std::uint8_t) p.arp), b.push_back (0), b.push_back (0);
        }
        for (auto h : hdr)
            i32 (h);
    };
    const std::array<std::int32_t, 5> off { 0, 0, -1, -1, -1 }, on { 0, 1, -1, -1, -1 };
    auto flat = [&] (double y, std::array<std::int32_t, 5> hdr = { 0, 0, -1, -1, -1 }) { curve ({ { 0, y, 0, 0 }, { 1, y, 0, 2 } }, hdr); };
    i32 (18);
    i32 (1 | 4);
    std::vector<std::uint8_t> part;
    std::swap (b, part);
    std::array<std::int32_t, 386> p {};
    const std::pair<int, int> values[] = { { 0, 500 }, { 3, 1000 }, { 9, 1024 }, { 11, 1000 }, { 12, 1000 }, { 17, 300 }, { 18, 4000 },
        { 19, 500 }, { 20, 1000 }, { 24, 500 }, { 25, 500 }, { 28, 500 }, { 29, 256 }, { 30, -1 }, { 31, -1 }, { 33, -1 }, { 34, 3 },
        { 35, 1 }, { 37, 192 }, { 38, 128 }, { 39, 500 }, { 40, 10000 }, { 41, 1 }, { 42, 10000 }, { 43, 1 }, { 45, 200 }, { 46, 500 },
        { 47, 1 }, { 49, -140 }, { 50, 1200 }, { 51, 0 }, { 52, 5000 }, { 54, 500 }, { 56, 600 }, { 57, 300 }, { 59, 500 }, { 60, 600 },
        { 64, 8192 }, { 66, 500 }, { 72, 600 }, { 74, 1000 }, { 75, 600 }, { 78, 5000 }, { 79, 500 }, { 80, 3000 } };
    for (const auto& [i, v] : values)
        p[(std::size_t) i] = v;
    if ((features & 2) != 0)
        p[57] = 400, p[59] = 300; // filter-1 resonance amount and width (noise type set in the combo block)
    if ((features & 4) != 0)
        p[75] = 700, p[21] = 300; // pluck value, time-blur attack
    if ((features & 8) != 0)
        p[75] = 650, p[4] = 600; // pluck, timbre mix
    if ((features & 32) != 0)
        p[76] = 700, p[27] = 600; // phaser mix, harmonizer mix
    if (image) // image synthesis (REA_rodada75): speed, cubic gain / linear freq interpolation, Hz freq mode, formant
        for (const auto& [i, v] : std::initializer_list<std::pair<int, int>> { { 82, 200 }, { 83, 100000 }, { 84, 10000 }, { 85, 20000 },
                 { 86, 750 }, { 87, 4 }, { 88, 600 }, { 89, 900 }, { 90, 1 }, { 91, 3 }, { 92, 1500 }, { 93, -300 }, { 94, 800 } })
            p[(std::size_t) i] = v;
    for (auto v : p)
        i32 (v);
    for (int i = 0; i < 12; ++i)
        i32 (i); // module order
    std::array<std::uint8_t, 0x4b> misc {};
    auto put = [&] (int o, std::int32_t v) { std::memcpy (misc.data() + o, &v, 4); };
    const float one = 1.0f;
    put (0x08, 107), put (0x1c, 1), put (0x28, 200), std::memcpy (misc.data() + 0x30, &one, 4);
    put (0x34, 0), put (0x38, 4), put (0x3c, (features & 2) != 0 ? 109 : 100), put (0x40, 108);
    if ((features & 8) != 0)
        put (0x00, 5);
    misc[0x44] = 1;
    if ((features & 4) != 0)
        misc[0x46] = 1;
    b.insert (b.end(), misc.begin(), misc.end());
    static const int mode1[] = { 0, 2, 4, 5, 12, 13, 14, 15, 17, 18, 23, 24, 27, 28, 29, 30, 33, 36, 38, 39, 40, 41, 42 };
    for (int k = 0; k < 43; ++k)
    {
        static const int mode2[] = { 2, 14, 23, 28, 30, 38, 39, 41, 42 }; // ×2 around 0.5: 1.0 is neutral
        const bool centred = std::find (std::begin (mode1), std::end (mode1), k) != std::end (mode1);
        const bool doubled = std::find (std::begin (mode2), std::end (mode2), k) != std::end (mode2);
        const double neutral = centred && ! doubled ? 0.5 : 1.0;
        i32 (3);
        i32 (1065353216), i32 (1065353216), i32 (1065353216);
        if (k == 1 && (features & 1) != 0) // arpeggio volume envelope: a looping pattern with chord markers
            curve ({ { 0, 0, 0, 0, 2 }, { 0.005, 1, 0.3, 0 }, { 0.08, 0.2, -0.4, 0, 3 }, { 0.005, 1, 0.2, 0 }, { 0.08, 0.1, 0, 0, 1 },
                     { 0.1, 0, 0.2, 0 } },
                   { 0, 1, -1, 0, 4 });
        else if (k == 1) // volume envelope: attack, decay to the sustain point, release
            curve ({ { 0, 0, 0, 0 }, { 0.01, 1, 0.3, 0 }, { 0.3, 0.6, -0.4, 0 }, { 0.4, 0, 0.2, 0 } }, { 0, 1, 1, -1, 2 });
        else if (k == 4) // filter-1 envelope
            curve ({ { 0, 1, 0, 0 }, { 0.25, 0.2, -0.5, 0 } }, { 0, 1, -1, -1, -1 });
        else
            flat (neutral);
        for (int a : { 128, 128, 0, 128 })
            i32 (a);
        if (k == 0 && panLfo) // pan LFO
            flat (1.0, on);
        else
            flat (0.5);
        for (int a : { k == 0 ? 40000 : 0, 0, 0, 0 })
            i32 (a);
        i32 (0);
        for (int m = 2; m < 11; ++m)
            if (k == 1 && m == 3)
                curve ({ { 0, 0.2, 0, 0 }, { 1, 1, -0.3, 0 } }, off); // velocity → volume
            else
                flat (neutral);
    }
    for (int c = 0; c < 26; ++c)
        if ((features & 16) != 0 && c == 4)
            curve ({ { 0, 0.5, 0, 0 }, { 0.2, 0.8, 0.3, 0 }, { 0.8, 0.3, 0, 0 } }, off); // harmonic randomness
        else if ((features & 32) != 0 && (c == 22 || c == 23))
            curve ({ { 0, 0.3, 0, 0 }, { 0.6, 1.0, 0.2, 0 }, { 0.4, 0.5, 0, 0 } }, off); // phaser / harmonizer masks
        else if (c == 3 && (features & 8) != 0)
            curve ({ { 0, 0.2, 0, 0 }, { 1, 0.05, 0.4, 0 } }, off); // timbre 2 for the pluck blend
        else if (c == 2 || c == 3)
            curve ({ { 0, 0.25, 0, 0 }, { 0.006, 0.12, -0.46, 0 }, { 0.994, 0, 0, 0 } }, off); // timbres
        else if (c == 14)
            curve ({ { 0, 1, 0, 0 }, { 1, 0, 0, 0 } }, off); // pluck curve
        else
            flat (c == 0 || c == 17 ? 0.25 : (c == 5 ? 0.0 : (c == 6 || c == 10 || c == 11 || (c >= 18 && c <= 25) ? 1.0 : 0.5)));
    if (image)
    {
        // the image object (FUN_00793300 layout): version 0, image mode, looping, 6 columns × 40 rows, gain + freq planes
        const int cols = 6, rows = 40;
        i32 (0), i32 (0), i32 (1), i32 (0);
        f64 (-1.0), f64 (-1.0), f64 (0.0);
        i32 (cols), i32 (rows);
        i32 (cols * rows);
        for (int c2 = 0; c2 < cols; ++c2)
            for (int r2 = 0; r2 < rows; ++r2)
            {
                const auto v = (float) (255.0 * std::pow (0.93, r2) * (0.6 + 0.4 * std::cos (0.9 * c2 + 0.37 * r2)));
                std::uint32_t u;
                std::memcpy (&u, &v, 4);
                i32 ((std::int32_t) u);
            }
        i32 (cols * rows);
        for (int c2 = 0; c2 < cols; ++c2)
            for (int r2 = 0; r2 < rows; ++r2)
            {
                const auto v = (float) (0.3 * std::sin (1.3 * c2 + 0.5 * r2));
                std::uint32_t u;
                std::memcpy (&u, &v, 4);
                i32 ((std::int32_t) u);
            }
        i32 (0);
        b.push_back (0); // (file name: empty)
    }
    std::swap (b, part);
    i32 ((std::int32_t) part.size());
    b.insert (b.end(), part.begin(), part.end());
    std::array<std::int32_t, 53> g {};
    g[0] = g[1] = g[2] = 256, g[10] = 500, g[11] = 1000, g[12] = 1000, g[15] = 1000;
    for (auto v : g)
        i32 (v);
    for (int i = 0; i < 6; ++i)
        i32 (i);
    std::array<std::uint8_t, 0x3d> gm {};
    const std::int32_t cr = 200, vr = 1;
    std::memcpy (gm.data() + 12, &vr, 4);
    std::memcpy (gm.data() + 24, &cr, 4);
    b.insert (b.end(), gm.begin(), gm.end());
    return b;
}
} // namespace

TEST (harmor_engine)
{
    // REA_rodada34: state parsing, the additive engine against our reference model (hmodel.py), host levels, voice end
    const auto blob = harmorTestState();
    if (const char* dump = std::getenv ("LYNX_DUMP_HARMOR"))
        std::ofstream (dump, std::ios::binary).write ((const char*) blob.data(), (std::streamsize) blob.size());
    CHECK (lynx::harmorStateUsable (blob));
    CHECK (lynx::harmorFallbacks (blob).empty());
    const auto at = lynx::harmorParamOffset (blob, 0, 52);
    std::int32_t cutoff = 0;
    CHECK (at > 0 && (std::memcpy (&cutoff, blob.data() + at, 4), cutoff == 5000));
    CHECK (lynx::harmorParamOffset (blob, 1, 52) < 0 && lynx::harmorParamOffset (blob, -1, 12) > at);
    auto broken = blob;
    broken.resize (broken.size() / 2);
    CHECK (! lynx::harmorStateUsable (broken));
    auto old = blob;
    old[0] = 15; // articulators and curves are stored from version 16
    CHECK (! lynx::harmorStateUsable (old));

    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::harmor;
    c.pluginState = blob;
    auto render = [] (const lynx::ChannelDef& def, int total, float gl, float gr, int offAt) {
        auto inst = lynx::makeInstrument (lynx::InstrumentKind::harmor);
        inst->configure (def, 120.0);
        inst->prepare (44100.0);
        std::vector<float> l ((std::size_t) total), r ((std::size_t) total);
        inst->noteOn (60, 0.78125f);
        for (int s = 0; s < total; s += 512)
        {
            if (s == offAt)
                inst->noteOff (60);
            inst->render (l.data() + s, r.data() + s, std::min (512, total - s), gl, gr);
        }
        return std::make_tuple (l, r, std::move (inst));
    };
    {
        // reference model (REA round 34 hmodel.py; C5 at FL velocity 100, Final.Vol 1, centre, released at 6144, the
        // Mersenne Twister seeded like LYNX's)
        auto [l, r, inst] = render (c, 8192, 1.0f, 1.0f, 6144);
        const std::tuple<int, float, float> golden[] {
            { 0, 0.0f, 0.0f }, { 100, 0.00947693f, 0.04249846f }, { 1000, 0.08140575f, -0.03251422f }, { 3000, 0.05928446f, -0.00247572f },
            { 6000, 0.02997839f, 0.14549091f }, { 7000, -0.01121871f, 0.04197952f }, { 8191, 0.00314545f, 0.03857505f } };
        for (const auto& [i, gl, gr] : golden)
            CHECK (std::abs (l[(std::size_t) i] - gl) < 2e-6f && std::abs (r[(std::size_t) i] - gr) < 2e-6f);
        // after the release the volume envelope ends, the record declicks and the instrument goes idle
        std::vector<float> tail (44100), tailR (tail.size());
        inst->render (tail.data(), tailR.data(), (int) tail.size(), 1.0f, 1.0f);
        CHECK (inst->idle());
        CHECK (std::abs (tail.back()) == 0.0f);
    }
    // the reference renderer gives the same samples as the instrument
    {
        const auto ref = lynx::harmor::renderReference (blob, { { 0, 60, 0.78125, 1.0, 0.0, 6144 } }, 44100.0, 8192);
        const auto [l, r, inst] = render (c, 8192, 1.0f, 1.0f, 6144);
        double worst = 0.0;
        for (int i = 0; i < 8192; ++i)
            worst = std::max ({ worst, (double) std::abs (ref[2 * (std::size_t) i] - l[(std::size_t) i]), (double) std::abs (ref[2 * (std::size_t) i + 1] - r[(std::size_t) i]) });
        CHECK (worst == 0.0);
    }
    // FL's level law applied once: half the channel volume halves the output; full right pan silences the left side
    const auto full = std::get<0> (render (c, 2048, 1.0f, 1.0f, -1));
    const auto half = std::get<0> (render (c, 2048, 0.5f, 0.5f, -1));
    CHECK (std::abs (half[1000] - 0.5f * full[1000]) < 1e-6f && std::abs (full[1000]) > 0.01f);
    auto mono = c;
    mono.pluginState = harmorTestState (false); // no pan LFO ...
    const std::int32_t one = 1;
    std::memcpy (mono.pluginState.data() + lynx::harmorParamOffset (mono.pluginState, 0, 34), &one, 4); // ... nor unison spread
    const auto [pl, pr, pi] = render (mono, 2048, 0.0f, 1.41421356f, -1);
    double el = 0.0, er = 0.0;
    for (int i = 0; i < 2048; ++i)
        el += (double) pl[(std::size_t) i] * pl[(std::size_t) i], er += (double) pr[(std::size_t) i] * pr[(std::size_t) i];
    CHECK (el < 1e-10 * er && er > 1e-3);
}

namespace
{
// An Ogun state written from scratch (REA_rodada37 §1.3 layout, version 18; zlib with stored blocks): richness 4
// (255 partials), pre-decay, decay/release, fullness, Seed 1 + fixed Seed 2 with morph, 3-voice unison, a 2-stage
// low-pass with an LFO on its cutoff, a volume envelope with a sustain point and a velocity map, declick, normalisation,
// Impulse/Decay maps, and every plugin effect (EQ band, chorus, ping-pong delay, reverb). Everything else is neutral.
std::vector<std::uint8_t> ogunTestState()
{
    std::vector<std::uint8_t> b;
    auto i32 = [&] (std::int32_t v) { for (int k = 0; k < 4; ++k) b.push_back ((std::uint8_t) ((std::uint32_t) v >> (8 * k))); };
    auto f32v = [&] (float v) { std::uint32_t u; std::memcpy (&u, &v, 4); i32 ((std::int32_t) u); };
    auto f64 = [&] (double v) { std::uint64_t u; std::memcpy (&u, &v, 8); for (int k = 0; k < 8; ++k) b.push_back ((std::uint8_t) (u >> (8 * k))); };
    struct Pt { double dx, y, tension; int mode; };
    auto curve = [&] (int type, const std::vector<Pt>& pts, std::array<std::int32_t, 5> hdr) {
        i32 (type), i32 (3), i32 ((std::int32_t) pts.size());
        for (const auto& p : pts)
        {
            f64 (p.dx), f64 (p.y), f32v ((float) p.tension);
            b.push_back ((std::uint8_t) p.mode), b.push_back (0), b.push_back (0), b.push_back (0);
        }
        for (auto h : hdr)
            i32 (h);
    };
    const std::array<std::int32_t, 5> off { 0, 0, -1, -1, -1 }, on { 0, 1, -1, -1, -1 };
    auto flat = [&] (int type, double y) { curve (type, { { 0, y, 0, 0 }, { 1, y, 0, 0 } }, off); };
    std::array<std::int32_t, 64> P {};
    const std::pair<int, int> values[] = { { 0, 128 }, { 5, 300 }, { 6, 3000 }, { 7, 2000 }, { 8, 200 }, { 9, 500 }, { 10, 12 },
        { 11, 34 }, { 12, 3 }, { 13, 64 }, { 14, 64 }, { 15, 40 }, { 16, 64 }, { 17, 180 }, { 18, 60 }, { 19, 2 }, { 20, 300 },
        { 21, 400 }, { 22, 40 }, { 23, 30 }, { 24, 20 }, { 25, 64 }, { 26, 40 }, { 27, 96 }, { 28, 100 }, { 29, 64 }, { 30, 2 },
        { 31, 19 }, { 32, 221 }, { 33, 0 }, { 34, 50 }, { 35, 100 }, { 36, 20 }, { 37, 60 }, { 38, 2 }, { 39, 64 }, { 40, 300 },
        { 43, 30000 }, { 46, 20000 }, { 49, 0 }, { 50, 2 }, { 51, 0 }, { 52, 9 }, { 53, 1 }, { 54, 0 },
        { 55, 0x01000000 },   // +0x20b: pitch follows the key
        { 56, 0x00010000 },   // +0x20e: declick
        { 62, 0x00000100 } }; // +0x225: normalise
    for (const auto& [i, v] : values)
        P[(std::size_t) i] = v;
    for (auto v : P)
        i32 (v);
    b.push_back (0), b.push_back (1), b.push_back (0); // delay: not tempo-based, on
    i32 (1), i32 (1), i32 (1);                         // EQ band types: peaking
    b.push_back (0);                                   // comment: 0 characters
    for (int k = 0; k < 13; ++k)
    {
        static constexpr int sum[] = { 1, 2, 3, 9, 11, 12 };
        const double neutral = std::find (std::begin (sum), std::end (sum), k) != std::end (sum) ? 0.5 : 1.0;
        i32 (1);
        f32v (0.0f), f32v (1.0f), f32v (k == 1 ? 0.3f : 0.0f);
        if (k == 0) // volume envelope: attack, decay to the sustain point, release
            curve (1, { { 0, 0, 0, 0 }, { 0.02, 1, 0.3, 0 }, { 0.3, 0.5, -0.4, 0 }, { 0.5, 0, 0.2, 0 } }, { 0, 1, 2, -1, 2 });
        else
            flat (1, neutral);
        for (int a : { 128, 128, 128, 128 })
            i32 (a);
        if (k == 1) // cutoff LFO
            curve (2, { { 0, 0.8, 0, 0 } }, on);
        else
            flat (2, 0.5);
        for (int a : { k == 1 ? 40000 : 0, 0, 0, 0 })
            i32 (a);
        i32 (0);
        for (int m = 0; m < 4; ++m)
            if (k == 0 && m == 1)
                curve (3, { { 0, 0.3, 0, 0 }, { 1, 1, -0.2, 0 } }, off); // velocity → volume
            else
                flat (3, neutral);
    }
    flat (3, 0.0);                                                // Forced: none
    curve (3, { { 0, 0.6, 0, 0 }, { 1, 0.4, 0.3, 0 } }, off);     // Impulse EQ
    curve (3, { { 0, 0.5, 0, 0 }, { 1, 0.3, 0, 0 } }, off);       // Decay EQ
    flat (3, 0.5);                                                // Release EQ
    // u32 version + zlib (stored blocks)
    std::vector<std::uint8_t> s { 18, 0, 0, 0, 0x78, 0x01 };
    for (std::size_t i = 0; i < b.size();)
    {
        const auto len = std::min<std::size_t> (65535, b.size() - i);
        s.push_back (i + len >= b.size() ? 1 : 0);
        s.push_back ((std::uint8_t) len), s.push_back ((std::uint8_t) (len >> 8));
        s.push_back ((std::uint8_t) ~len), s.push_back ((std::uint8_t) (~len >> 8));
        s.insert (s.end(), b.begin() + (std::ptrdiff_t) i, b.begin() + (std::ptrdiff_t) (i + len));
        i += len;
    }
    std::uint32_t a1 = 1, a2 = 0;
    for (auto c : b)
        a1 = (a1 + c) % 65521u, a2 = (a2 + a1) % 65521u;
    for (int sh = 24; sh >= 0; sh -= 8)
        s.push_back ((std::uint8_t) (((a2 << 16) | a1) >> sh));
    return s;
}
} // namespace

TEST (ogun_engine)
{
    // REA_rodada37: state parsing, the engine against our reference model (ogmodel.py), host levels, export quality
    const auto blob = ogunTestState();
    if (const char* dump = std::getenv ("LYNX_DUMP_OGUN"))
        std::ofstream (dump, std::ios::binary).write ((const char*) blob.data(), (std::streamsize) blob.size());
    CHECK (lynx::ogunStateUsable (blob));
    CHECK (lynx::ogunFallbacks (blob).empty());
    CHECK (lynx::ogunParam (blob, 17) == 180 && lynx::ogunParam (blob, 52) == 9 && ! lynx::ogunParam (blob, 64));
    auto broken = blob;
    broken.resize (broken.size() - 300);
    CHECK (! lynx::ogunStateUsable (broken));
    auto old = blob;
    old[0] = 13; // the articulator layout is from version 14
    CHECK (! lynx::ogunStateUsable (old));

    // reference model (REA round 37 ogmodel.Plugin(state).run: C5 velocity 0.75 released at 6144, G5 velocity 0.625 at 2048
    // released at 8192; host block 512)
    const std::vector<lynx::ogun::RefNote> notes { { 0, 60, 0.75, 1.0, 0.0, 0.0, 0.0, 0.0, 0.5, 6144 }, { 2048, 67, 0.625, 1.0, 0.0, 0.0, 0.0, 0.0, 0.5, 8192 } };
    const auto ref = lynx::ogun::renderReference (blob, notes, 44100.0, 16384);
    CHECK (ref.size() == 2 * 16384);
    if (const char* dump = std::getenv ("LYNX_DUMP_OGUN_OUT"))
        std::ofstream (dump, std::ios::binary).write ((const char*) ref.data(), (std::streamsize) (ref.size() * 4));
    const std::tuple<int, float, float> golden[] {
        { 100, -7.763202e-05f, -8.919523e-05f }, { 1000, 0.06195267f, 0.03858230f }, { 3000, 0.01363476f, -0.00378433f },
        { 6000, -0.06601223f, -0.03084030f }, { 9000, 0.04204506f, 0.02452896f }, { 12000, 0.03420147f, -0.01837870f },
        { 16383, -0.00465311f, -0.00119627f } };
    for (const auto& [i, gl, gr] : golden)
        CHECK (std::abs (ref[2 * (std::size_t) i] - gl) < 2e-6f && std::abs (ref[2 * (std::size_t) i + 1] - gr) < 2e-6f);

    // the instrument plays like the reference renderer (Final.Vol 1, centre)
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::ogun;
    c.pluginState = blob;
    auto render = [&] (const lynx::ChannelDef& def, int total, float gl, float gr, bool held = false) {
        auto inst = lynx::makeInstrument (lynx::InstrumentKind::ogun);
        inst->configure (def, 120.0);
        inst->prepare (44100.0);
        std::vector<float> l ((std::size_t) total), r ((std::size_t) total);
        for (int s = 0; s < total; s += 512)
        {
            if (s == 0)
                inst->noteOn (60, 0.75f);
            if (s == 2048)
                inst->noteOn (67, 0.625f);
            if (s == 6144 && ! held)
                inst->noteOff (60);
            if (s == 8192 && ! held)
                inst->noteOff (67);
            inst->render (l.data() + s, r.data() + s, std::min (512, total - s), gl, gr);
        }
        return std::make_tuple (l, r, std::move (inst));
    };
    {
        auto [l, r, inst] = render (c, 16384, 1.0f, 1.0f);
        double worst = 0.0;
        for (int i = 0; i < 16384; ++i)
            worst = std::max ({ worst, (double) std::abs (ref[2 * (std::size_t) i] - l[(std::size_t) i]), (double) std::abs (ref[2 * (std::size_t) i + 1] - r[(std::size_t) i]) });
        CHECK (worst == 0.0);
        // the voices end and the effect tails die out: the instrument goes idle
        std::vector<float> tail (44100), tailR (tail.size());
        for (int k = 0; k < 20 && ! inst->idle(); ++k)
            inst->render (tail.data(), tailR.data(), (int) tail.size(), 1.0f, 1.0f);
        CHECK (inst->idle());
    }
    // FL's level law applied once (effects off): half the channel volume halves the output once the gain ramps
    // (limited to 0.001 per sample) have settled on the sustain level; full right pan silences the left side
    {
        auto dry = c;
        for (int i : { 19, 29, 40, 53 })
            CHECK (lynx::ogunSetParam (dry.pluginState, i, 0)); // chorus, delay wet, EQ band and reverb off
        CHECK (lynx::ogunStateUsable (dry.pluginState) && lynx::ogunParam (dry.pluginState, 19) == 0);
        const auto full = std::get<0> (render (dry, 32768, 1.0f, 1.0f, true));
        const auto half = std::get<0> (render (dry, 32768, 0.5f, 0.5f, true));
        double worst = 0.0, peak = 0.0;
        for (int i = 30000; i < 32768; ++i)
            worst = std::max (worst, (double) std::abs (half[(std::size_t) i] - 0.5f * full[(std::size_t) i])),
            peak = std::max (peak, (double) std::abs (full[(std::size_t) i]));
        CHECK (worst <= 1e-7 * peak && peak > 1e-3);
        // FL's default channel volume (100/128): FL hands Ogun Final.Vol = (11^v − 1)/10 (REA_rodada28 §4.1); LYNX's
        // centred channel gains (√2·V·cos(π/4) each) give the reference model's level at that Final.Vol
        const auto V = lynx::channelGain (0.78125f);
        const auto g = V * std::cos (0.25f * 3.14159265f) * 1.41421356f;
        const auto atDefault = std::get<0> (render (dry, 32768, g, g, true));
        const auto refDefault = lynx::ogun::renderReference (dry.pluginState, { { 0, 60, 0.75, V, 0.0, 0.0, 0.0, 0.0, 0.5, -1 },
                                                                                { 2048, 67, 0.625, V, 0.0, 0.0, 0.0, 0.0, 0.5, -1 } }, 44100.0, 32768);
        double ei = 0.0, er0 = 0.0;
        for (int i = 30000; i < 32768; ++i)
            ei += (double) atDefault[(std::size_t) i] * atDefault[(std::size_t) i], er0 += (double) refDefault[2 * (std::size_t) i] * refDefault[2 * (std::size_t) i];
        CHECK (er0 > 0.0 && std::abs (10.0 * std::log10 (ei / er0)) < 0.001);
        const auto [pl, pr, pi] = render (dry, 4096, 0.0f, 1.41421356f);
        double el = 0.0, er = 0.0;
        for (int i = 0; i < 4096; ++i)
            el += (double) pl[(std::size_t) i] * pl[(std::size_t) i], er += (double) pr[(std::size_t) i] * pr[(std::size_t) i];
        CHECK (el < 1e-10 * er && er > 1e-3);
    }
    // export: FL renders Ogun at quality 4 (×2 rate, 32-point reader), LYNX too while it renders offline
    {
        lynx::setOfflineRendering (true);
        auto [l, r, inst] = render (c, 8192, 1.0f, 1.0f);
        lynx::setOfflineRendering (false);
        lynx::ogun::RefOptions q4;
        q4.quality = 4;
        const auto ref4 = lynx::ogun::renderReference (blob, notes, 44100.0, 8192, q4);
        double worst = 0.0, diff = 0.0;
        for (int i = 0; i < 8192; ++i)
            worst = std::max (worst, (double) std::abs (ref4[2 * (std::size_t) i] - l[(std::size_t) i])),
            diff = std::max (diff, (double) std::abs (ref[2 * (std::size_t) i] - l[(std::size_t) i]));
        CHECK (worst == 0.0 && diff > 1e-3);
        const std::tuple<int, float, float> golden4[] {
            { 100, 0.00605961f, 0.00628969f }, { 1000, 0.03729841f, 0.01727954f }, { 3000, -0.09010383f, -0.02551294f },
            { 6000, 0.02167798f, -0.01115632f }, { 8191, 0.00320288f, -0.00615792f } };
        for (const auto& [i, gl, gr] : golden4)
            CHECK (std::abs (ref4[2 * (std::size_t) i] - gl) < 2e-6f && std::abs (ref4[2 * (std::size_t) i + 1] - gr) < 2e-6f);
    }
    // feature variants of the same state against the reference model (energy per side over 8192 samples, three
    // samples; C1 released at 6144): qualities, richness, unison, the filter types, ALT DECAY, 1/√h, Seed 2 Random,
    // infinite decay, logarithmic axis, velocity → position, other rates. Goldens from ogmodel.py (REA round 37).
    {
        struct Sample { int at; float l, r; };
        struct Variant { const char* name; std::vector<std::pair<int, int>> params; int rate; double el, er; Sample s[3]; };
        const Variant variants[] {
        { "q1", { { 54, 1 } }, 44100, 11.0970402, 6.87820714, { { 1500, 0.0240363069f, -0.0148524037f }, { 4000, 0.0873190612f, -0.000715861097f }, { 7000, -0.0378603041f, -0.00338819111f } } },
        { "q2", { { 54, 2 } }, 44100, 11.2405446, 6.68045082, { { 1500, 0.0448660068f, -0.00560745923f }, { 4000, -0.0122657465f, 0.0104285963f }, { 7000, -0.0216034129f, -0.0134632047f } } },
        { "q3", { { 54, 3 } }, 44100, 11.3200608, 6.86196449, { { 1500, -0.0031537537f, -0.0242648814f }, { 4000, 0.0426301025f, 0.0364697985f }, { 7000, 0.00843803957f, 0.0302239582f } } },
        { "rich16_q0", { { 52, 16 }, { 54, 0 } }, 44100, 26.7616786, 19.2591256, { { 1500, 0.0728010684f, -0.0116148153f }, { 4000, 0.0797849894f, 0.0765359923f }, { 7000, -0.00811834447f, -0.0104534971f } } },
        { "uni9", { { 12, 9 }, { 15, 20 }, { 16, 0 }, { 14, 16 } }, 44100, 61.7485898, 36.6693432, { { 1500, 0.0531249829f, -0.0374071151f }, { 4000, 0.165054232f, -0.0192129575f }, { 7000, -0.0373774841f, -0.00196591392f } } },
        { "lp1", { { 50, 1 }, { 49, 0 } }, 44100, 7.81941168, 4.72659869, { { 1500, 0.0377117358f, -0.0157920271f }, { 4000, 0.0747607499f, 0.00154248625f }, { 7000, -0.0510631353f, 0.00884498842f } } },
        { "bp3flat", { { 50, 3 }, { 49, 1 }, { 51, 1 } }, 44100, 2.19181431, 1.29483907, { { 1500, 0.0291239917f, -0.00288665155f }, { 4000, -0.0084345676f, -0.00400509313f }, { 7000, -0.00592403999f, 0.00150578981f } } },
        { "bs2", { { 50, 2 }, { 49, 2 } }, 44100, 32.8440514, 25.7202184, { { 1500, 0.0730303749f, -0.0496222489f }, { 4000, 0.0696200803f, 0.0595823973f }, { 7000, -0.0482789353f, 0.0764200836f } } },
        { "hp3", { { 50, 3 }, { 49, 3 } }, 44100, 8.623214, 7.95253188, { { 1500, -0.0325722694f, 0.00703159999f }, { 4000, 0.0183976218f, -0.0372007042f }, { 7000, -0.0126833525f, -0.00160143059f } } },
        { "alt", { { 62, 257 } }, 44100, 17.2485711, 9.99223979, { { 1500, -0.0319156498f, -0.0186850764f }, { 4000, -0.0769012421f, -0.0375530645f }, { 7000, 0.0310976505f, 0.0445244014f } } },
        { "isqrt", { { 62, 65792 } }, 44100, 45.7309307, 27.1224593, { { 1500, -0.04376591f, -0.00845711026f }, { 4000, 0.0351300463f, -0.129557073f }, { 7000, 0.0096148327f, 0.00123740174f } } },
        { "seed2rnd", { { 11, 0 } }, 44100, 16.8228819, 11.5433061, { { 1500, -0.0117614279f, -0.026497297f }, { 4000, 0.00265733898f, -0.07069581f }, { 7000, 0.00576663949f, 0.0565217957f } } },
        { "decinf", { { 6, 10000 } }, 44100, 19.1879226, 11.0487311, { { 1500, -0.0324105881f, -0.0190765262f }, { 4000, -0.0809780285f, -0.0395730771f }, { 7000, 0.034197174f, 0.048585102f } } },
        { "logx", { { 57, 16777216 } }, 44100, 12.3064326, 7.31519289, { { 1500, -0.0438990779f, 0.0096983267f }, { 4000, -0.0175748877f, 0.0313935578f }, { 7000, 0.0639200211f, 0.024419222f } } },
        { "velpos", { { 56, 66304 } }, 44100, 4.9966925, 3.16540662, { { 1500, 0.0083851777f, -0.0165472236f }, { 4000, 0.0630451962f, 0.0156494901f }, { 7000, -0.0293043777f, -0.00182877108f } } },
        { "fs48", {  }, 48000, 10.8813888, 6.53592373, { { 1500, -0.0485184453f, -0.0319588967f }, { 4000, 0.0230421089f, -0.0286604818f }, { 7000, -0.0117630213f, -0.017459048f } } },
        { "fs96", {  }, 96000, 11.0189683, 6.39821883, { { 1500, -0.0267860368f, -0.0227564126f }, { 4000, 0.0238709711f, -0.0100849615f }, { 7000, -0.0034619756f, 0.0451687202f } } },
        };
        for (const auto& v : variants)
        {
            auto st = blob;
            for (const auto& [i, x] : v.params)
                CHECK (lynx::ogunSetParam (st, i, x));
            const auto y = lynx::ogun::renderReference (st, { { 0, 60, 0.75, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 6144 },
                                                              { 2048, 67, 0.625, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, -1 } }, (double) v.rate, 8192);
            double el = 0.0, er = 0.0, peak = 0.0;
            for (std::size_t i = 0; i + 1 < y.size(); i += 2)
                el += (double) y[i] * y[i], er += (double) y[i + 1] * y[i + 1], peak = std::max ({ peak, (double) std::abs (y[i]), (double) std::abs (y[i + 1]) });
            bool ok = y.size() == 2 * 8192 && std::abs (el - v.el) <= 1e-5 * v.el && std::abs (er - v.er) <= 1e-5 * v.er;
            for (const auto& smp : v.s)
                ok = ok && std::abs (y[2 * (std::size_t) smp.at] - smp.l) <= 1e-5 * peak && std::abs (y[2 * (std::size_t) smp.at + 1] - smp.r) <= 1e-5 * peak;
            if (! ok)
                std::printf ("  Ogun variant %s differs from the model: energy %.9g %.9g (model %.9g %.9g)\n", v.name, el, er, v.el, v.er);
            CHECK (ok);
        }
    }
}

// our own Maximus v7 state (layout REA_rodada8 §1.1): 7 globals, 4 × 13 band parameters, 8 envelopes (dx, y, tension, type)
static std::vector<std::uint8_t> maximusTestState (const std::array<int, 7>& g, const std::array<std::array<int, 13>, 4>& bands,
                                                   const std::array<std::vector<std::array<double, 4>>, 8>& envs, int oversampling)
{
    std::vector<std::uint8_t> d;
    auto put = [&] (const void* v, std::size_t n) { d.insert (d.end(), (const std::uint8_t*) v, (const std::uint8_t*) v + n); };
    auto i32 = [&] (std::int32_t v) { put (&v, 4); };
    i32 (7);
    for (int copy = 0; copy < 2; ++copy)
    {
        for (auto v : g)
            i32 (v);
        for (const auto& b : bands)
            for (auto v : b)
                i32 (v);
    }
    d.resize (0x1fe, 0);
    std::memcpy (d.data() + 0x1f4, &oversampling, 4);
    for (const auto& e : envs)
    {
        i32 (3), i32 (2), i32 ((std::int32_t) e.size());
        for (const auto& pt : e)
        {
            const double dx = pt[0], y = pt[1];
            const float t = (float) pt[2];
            put (&dx, 8), put (&y, 8), put (&t, 4);
            const std::uint8_t flags[4] { (std::uint8_t) pt[3], 0, 0, 0 };
            put (flags, 4);
        }
        i32 (0), i32 (-1), i32 (-1), i32 (-1), i32 (-1);
    }
    return d;
}

// A "Kepler-1" state written from scratch (REA_rodada46 §1.1 layout, version 2): the default panel values
std::vector<std::uint8_t> keplerTestState()
{
    const float panel[34] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0.5f, 0.5f, 0, 0, 0, 0.5f, 0.5f };
    std::vector<std::uint8_t> st;
    auto u32 = [&] (std::uint32_t v) { for (int i = 0; i < 4; ++i) st.push_back ((std::uint8_t) (v >> (8 * i))); };
    auto f32 = [&] (float f) { std::uint32_t v; std::memcpy (&v, &f, 4); u32 (v); };
    u32 (0), u32 (0), u32 (0), u32 (9);
    for (char c : std::string ("Kepler-1"))
        st.push_back ((std::uint8_t) c);
    st.push_back (0);
    u32 (2), u32 (34);
    for (auto f : panel)
        f32 (f);
    st.push_back (0);
    f32 (1.0f), f32 (0.5f);
    const auto size = (std::uint32_t) st.size() - 12;
    std::memcpy (st.data() + 4, &size, 4);
    return st;
}

std::string keplerDll()
{
    if (realFlpDir.empty())
        return {};
    return (std::filesystem::path (realFlpDir).parent_path() / "Plugins" / "Fruity" / "Generators" / "Kepler" / "engine_x64.dll").string();
}

TEST (kepler_engine)
{
    const auto st = keplerTestState();
    CHECK (st.size() == 178 && lynx::keplerStateUsable (st));
    CHECK (lynx::keplerPanelOffset (st, 19) == 0x21 + 4 * 19 && lynx::keplerPanelOffset (st, 34) == -1);
    auto broken = st;
    broken[17] = 'X'; // not "Kepler-1"
    CHECK (! lynx::keplerStateUsable (broken) && lynx::keplerPanelOffset (broken, 0) == -1);
    auto v3 = st;
    v3[0x19] = 3; // version 3 needs the gate / noise / oversampling bytes
    CHECK (! lynx::keplerStateUsable (v3));
    const auto dll = keplerDll();
    if (dll.empty() || ! lynx::keplerDllUsable (dll))
        return; // no FL installation (the engine needs the user's own DLL tables)
    // against FL's engine DLL (EXEC, REA_rodada46 §6): three notes, 44.1 kHz, host blocks of 512 — bit-exact
    const std::vector<lynx::kepler::RefNote> notes { { 0, 60, 1.0, 0.0, 0.0, 20000 }, { 8000, 67, 0.6, 0.0, 0.0, 26000 }, { 16000, 55, 0.95, 0.0, 0.0, 30000 } };
    const auto y = lynx::kepler::renderReference (st, dll, notes, 44100.0, 39690);
    CHECK (y.size() == 2 * 39690);
    if (y.size() != 2 * 39690)
        return;
    double el = 0.0, er = 0.0;
    for (std::size_t i = 0; i < y.size(); i += 2)
        el += (double) y[i] * y[i], er += (double) y[i + 1] * y[i + 1];
    std::printf ("  Kepler default patch: energy %.9g %.9g (FL's DLL 250.641268)\n", el, er);
    CHECK (std::abs (el - 250.641268) < 1e-6 && std::abs (er - 250.641268) < 1e-6);
    const std::tuple<int, float, float> golden[] { { 5000, 0.0467273332f, 0.0467273332f }, { 12000, -0.119412214f, -0.119412214f },
                                                   { 25000, 0.0855825394f, 0.0855825394f }, { 35000, -3.34166579e-08f, -3.34166579e-08f } };
    for (const auto& [i, l, r] : golden)
        CHECK (y[2 * (std::size_t) i] == l && y[2 * (std::size_t) i + 1] == r);

    // the instrument: FL's wrapper velocity, channel volume as an output gain, idle after the release
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::kepler;
    c.pluginState = st;
    c.samplePath = dll;
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::kepler);
    inst->configure (c, 120.0);
    inst->prepare (44100.0);
    std::vector<float> l (4096), r (4096);
    inst->noteOn (60, 0.8f);
    double e = 0.0;
    bool finite = true;
    for (int b = 0; b < 8; ++b)
    {
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
        inst->render (l.data(), r.data(), 4096, 0.5f, 0.5f);
        for (int i = 0; i < 4096; ++i)
            finite = finite && std::isfinite (l[(std::size_t) i]) && std::isfinite (r[(std::size_t) i]), e += (double) l[(std::size_t) i] * l[(std::size_t) i];
        if (b == 3)
            inst->noteOff (60);
    }
    CHECK (finite && e > 1.0);
    for (int b = 0; b < 40; ++b)
        inst->render (l.data(), r.data(), 4096, 0.5f, 0.5f);
    CHECK (inst->idle());
}

// REA round 57 §2: Transistor Bass
// the plugin state (event 0xD5) of an FL preset file (.fst: the FLhd/FLdt event stream)
std::vector<std::uint8_t> tbassPresetState (const char* name)
{
    if (realFlpDir.empty())
        return {};
    std::ifstream in (std::filesystem::path (realFlpDir) / "Patches" / "Plugin presets" / "Generators" / "Transistor Bass" / name, std::ios::binary);
    const std::vector<std::uint8_t> b ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    std::size_t i = 0;
    while (i + 8 <= b.size() && std::memcmp (b.data() + i, "FLdt", 4) != 0)
        ++i;
    i += 8;
    while (i < b.size())
    {
        const auto id = b[i++];
        std::size_t n = id < 64 ? 1 : id < 128 ? 2 : id < 192 ? 4 : 0;
        if (id >= 192)
            for (int shift = 0; i < b.size(); shift += 7)
            {
                const auto c = b[i++];
                n |= (std::size_t) (c & 0x7F) << shift;
                if (c < 0x80)
                    break;
            }
        if (id == 0xD5 && i + n <= b.size())
            return { b.begin() + (std::ptrdiff_t) i, b.begin() + (std::ptrdiff_t) (i + n) };
        i += n;
    }
    return {};
}

// the same state re-saved as engine-stream version 5 with an engine selector (REA_rodada57 §2.2)
std::vector<std::uint8_t> tbassWithEngine (const std::vector<std::uint8_t>& s, std::int32_t engine)
{
    std::vector<std::uint8_t> out (s.begin(), s.begin() + 4);
    auto put = [&] (const void* p, std::size_t n) { out.insert (out.end(), (const std::uint8_t*) p, (const std::uint8_t*) p + n); };
    for (std::size_t at = 4; at + 12 <= s.size();)
    {
        std::uint32_t id;
        std::uint64_t n;
        std::memcpy (&id, s.data() + at, 4);
        std::memcpy (&n, s.data() + at + 4, 8);
        std::vector<std::uint8_t> d (s.begin() + (std::ptrdiff_t) (at + 12), s.begin() + (std::ptrdiff_t) (at + 12 + n));
        at += 12 + (std::size_t) n;
        if (id == 2)
        {
            std::uint64_t m;
            std::memcpy (&m, d.data() + 4, 8);
            std::vector<std::uint8_t> e (d.begin() + 12, d.begin() + 12 + (std::ptrdiff_t) m);
            std::int16_t v;
            std::memcpy (&v, e.data(), 2);
            if (v < 5)
            {
                v = 5;
                std::memcpy (e.data(), &v, 2);
                e.insert (e.end(), 4, 0);
            }
            std::memcpy (e.data() + e.size() - 4, &engine, 4);
            m = e.size();
            d.resize (12);
            std::memcpy (d.data() + 4, &m, 8);
            d.insert (d.end(), e.begin(), e.end());
        }
        const std::uint64_t size = d.size();
        put (&id, 4);
        put (&size, 8);
        put (d.data(), d.size());
    }
    return out;
}

std::string tbassDll()
{
    if (realFlpDir.empty())
        return {};
    return (std::filesystem::path (realFlpDir).parent_path() / "Plugins" / "Fruity" / "Generators" / "Transistor Bass" / "engine_x64.dll").string();
}

// Plucked! (REA_rodada65 §1): the C++ engine against FL's DLL (EXEC: same notes, the DLL's RandSeed set before the first
// note, the host killing voices when the plugin asks). LYNX renders these bit-identically to the DLL.
TEST (plucked_engine)
{
    auto stateOf = [] (std::initializer_list<std::int32_t> k) {
        std::vector<std::uint8_t> st (20);
        std::memcpy (st.data(), std::data (k), 20);
        return st;
    };
    CHECK (! lynx::pluckedStateUsable (std::vector<std::uint8_t> (19)));
    CHECK (lynx::pluckedStateUsable (stateOf ({ 128, 64, 1, 0, 1 })));
    CHECK (lynx::pluckedParams (stateOf ({ 7, 9, 0, 1, 0 })) == std::vector<int> ({ 7, 9, 0, 1, 0 }));

    using N = lynx::plucked::RefNote;
    struct Sample { int at; float l, r; };
    struct Golden { int set; std::int32_t P[5]; double fs; int block, law, maxPoly; double el, er; Sample s[4]; };
    const std::vector<std::vector<N>> sets {
        { { 0, 60, 1.0, 0.0, 0.0, 0.0, 0.0, -1 } },
        { { 0, 60, 1.0, 0.0, 0.0, 0.0, 0.0, 30000 }, { 5000, 67, 0.6, -0.4, 13.0, 0.0, 0.0, 20000 } },
        { { 0, 48, 0.8, 0.5, 0.0, 0.1, -0.2, 22050 }, { 100, 52, 0.8, 0.0, 0.0, 0.0, 0.0, 22050 }, { 200, 55, 0.8, -1.0, -50.0, 0.0, 0.0, 22050 } },
        { { 0, 24, 1.0, 0.0, 0.0, 0.0, 0.0, -1 }, { 3000, 100, 1.0, 0.2, 0.0, 0.0, 0.0, -1 } },
        { { 0, 72, 1.0, 0.0, 0.0, -0.3, 0.6, -1 }, { 77, 84, 0.5, 0.0, 0.0, 0.0, 0.0, -1 } },
    };
    const Golden goldens[] {
        { 0, { 128, 64, 1, 0, 1 }, 44100, 512, 0, 0, 455.867624, 313.77463, { { 1000, -0.1914210170507431f, -0.2735292315483093f }, { 10000, -0.037063151597976685f, 0.00110959867015481f }, { 30000, -0.009626023471355438f, -0.001515754614956677f }, { 60000, 0.0f, 0.0f } } },
        { 1, { 128, 64, 1, 1, 1 }, 44100, 512, 1, 0, 377.99866, 237.526831, { { 1000, -0.11432178318500519f, -0.14189107716083527f }, { 10000, 0.08135657012462616f, 0.027565963566303253f }, { 30000, 0.0f, 0.0f }, { 60000, 0.0f, 0.0f } } },
        { 2, { 60, 20, 0, 1, 0 }, 48000, 256, 1, 2, 1570.38626, 550.991338, { { 1000, 0.4817328155040741f, 0.19528728723526f }, { 10000, -0.34781914949417114f, -0.15463846921920776f }, { 30000, 0.0f, 0.0f }, { 60000, 0.0f, 0.0f } } },
        { 3, { 256, 128, 1, 0, 0 }, 44100, 1000, 1, 0, 2007.37978, 2089.11642, { { 1000, 0.3288099467754364f, 0.3288099467754364f }, { 10000, 0.04112838953733444f, 0.05718187615275383f }, { 30000, -0.08563447743654251f, -0.08728141337633133f }, { 60000, -0.0894850566983223f, -0.09222060441970825f } } },
        { 4, { 0, 0, 1, 0, 1 }, 96000, 512, 0, 0, 222.608052, 243.338078, { { 1000, -0.20827248692512512f, -0.13200917840003967f }, { 10000, 0.01809507980942726f, 0.013305095955729485f }, { 30000, 0.0f, 0.0f }, { 60000, 0.0f, 0.0f } } },
    };
    for (const auto& g : goldens)
    {
        const auto st = stateOf ({ g.P[0], g.P[1], g.P[2], g.P[3], g.P[4] });
        const auto length = (int) g.fs * 2;
        const auto y = lynx::plucked::renderReference (st, sets[(std::size_t) g.set], g.fs, length, g.block, 777u + (std::uint32_t) g.set,
                                                       g.law != 0, g.maxPoly);
        double el = 0.0, er = 0.0;
        for (std::size_t i = 0; i + 1 < y.size(); i += 2)
            el += (double) y[i] * y[i], er += (double) y[i + 1] * y[i + 1];
        bool ok = y.size() == 2 * (std::size_t) length && std::abs (el - g.el) <= 1e-7 * g.el && std::abs (er - g.er) <= 1e-7 * g.er;
        for (const auto& smp : g.s)
            ok = ok && std::abs (y[2 * (std::size_t) smp.at] - smp.l) <= 1e-7f && std::abs (y[2 * (std::size_t) smp.at + 1] - smp.r) <= 1e-7f;
        if (! ok)
            std::printf ("  Plucked! case %d differs from FL's DLL: energy %.9g %.9g (DLL %.9g %.9g)\n", g.set, el, er, g.el, g.er);
        CHECK (ok);
    }

    // the instrument: plays, the gate makes a note-off silence the string, a held string dies out by itself
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::plucked;
    c.pluginState = stateOf ({ 200, 40, 1, 1, 1 });
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::plucked);
    inst->configure (c, 120.0);
    inst->prepare (44100.0);
    std::vector<float> l (1024), r (1024);
    inst->noteOn (57, 0.8f);
    double e = 0.0;
    for (int b = 0; b < 8; ++b)
    {
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
        inst->render (l.data(), r.data(), 1024, 0.7f, 0.7f);
        for (int i = 0; i < 1024; ++i)
            e += (double) l[(std::size_t) i] * l[(std::size_t) i] + (double) r[(std::size_t) i] * r[(std::size_t) i];
    }
    CHECK (e > 1.0 && ! inst->idle());
    inst->noteOff (57);
    for (int b = 0; b < 4; ++b)
        inst->render (l.data(), r.data(), 1024, 0.7f, 0.7f);
    CHECK (inst->idle());
    c.pluginState = stateOf ({ 0, 64, 0, 0, 0 });
    inst->configure (c, 120.0);
    inst->noteOn (60, 1.0f);
    inst->noteOff (60); // (no gate: the string rings on)
    for (int b = 0; b < 2000 && ! inst->idle(); ++b)
        inst->render (l.data(), r.data(), 1024, 0.7f, 0.7f);
    CHECK (inst->idle());
}

// BeepMap (REA_rodada65 §2): the C++ engine against FL's DLL (EXEC: same notes, the DLL's RandSeed set before the first
// note, the host killing voices when the plugin asks): a synthetic 9×6 bitmap with every Scale mode and switch, and two
// of the installation's presets. LYNX renders these bit-identically to the DLL.
std::vector<std::uint8_t> beepMapTestState (std::initializer_list<std::int32_t> settings)
{
    constexpr int W = 9, H = 6;
    std::vector<std::uint8_t> img (54 + 4 * W * H, 0);
    auto put = [&] (std::size_t at, std::uint32_t v, int bytes) { for (int i = 0; i < bytes; ++i) img[at + (std::size_t) i] = (std::uint8_t) (v >> (8 * i)); };
    img[0] = 'B', img[1] = 'M';
    put (2, (std::uint32_t) img.size(), 4), put (10, 54, 4), put (14, 40, 4), put (18, W, 4), put (22, H, 4), put (26, 1, 2), put (28, 32, 2);
    put (34, 4 * W * H, 4);
    for (int y = 0; y < H; ++y) // bottom-up rows
        for (int x = 0; x < W; ++x)
        {
            const auto yy = H - 1 - y;
            auto* px = img.data() + 54 + 4 * (y * W + x);
            px[0] = (std::uint8_t) ((x * 91 + yy * 29) & 255), px[1] = (std::uint8_t) ((x * 13 + yy * 71) & 255);
            px[2] = (std::uint8_t) ((x * 37 + yy * 11) & 255), px[3] = 255;
        }
    std::vector<std::uint8_t> st (4 + 60 + 4, 0);
    std::size_t k = 0;
    for (auto v : settings)
        std::memcpy (st.data() + 4 + 4 * k++, &v, 4);
    const auto len = (std::int32_t) img.size();
    std::memcpy (st.data() + 64, &len, 4);
    st.insert (st.end(), img.begin(), img.end());
    return st;
}

// the 0xD5 state of one of the installation's BeepMap presets (empty without an installation)
std::vector<std::uint8_t> beepMapPresetState (const char* name)
{
    if (realFlpDir.empty())
        return {};
    std::ifstream in (std::filesystem::path (realFlpDir) / "Patches" / "Plugin presets" / "Generators" / "BeepMap" / name, std::ios::binary);
    const std::vector<std::uint8_t> b ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    std::size_t i = 0;
    while (i + 8 <= b.size() && std::memcmp (b.data() + i, "FLdt", 4) != 0)
        ++i;
    i += 8;
    while (i < b.size())
    {
        const auto id = b[i++];
        std::size_t n = id < 64 ? 1 : id < 128 ? 2 : id < 192 ? 4 : 0;
        if (id >= 192)
            for (int shift = 0; i < b.size(); shift += 7)
            {
                const auto c = b[i++];
                n |= (std::size_t) (c & 0x7F) << shift;
                if (c < 0x80)
                    break;
            }
        if (id == 0xD5 && i + n <= b.size())
            return { b.begin() + (std::ptrdiff_t) i, b.begin() + (std::ptrdiff_t) (i + n) };
        i += n;
    }
    return {};
}

TEST (beepmap_engine)
{
    const auto synthA = beepMapTestState ({ 3000, 200, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 64 });
    CHECK (lynx::beepMapStateUsable (synthA));
    CHECK (lynx::beepMapParams (synthA)->at (1) == 200 && lynx::beepMapParamOffset (synthA, 2) == 12);
    auto broken = synthA;
    broken[0] = 2; // versions above 1 are refused
    CHECK (! lynx::beepMapStateUsable (broken));
    broken = synthA;
    broken.resize (broken.size() - 20); // truncated bitmap
    CHECK (! lynx::beepMapStateUsable (broken));

    using N = lynx::beepmap::RefNote;
    struct Sample { int at; float l, r; };
    struct Golden { int set; double fs; int block; double el, er; Sample s[4]; };
    std::vector<std::vector<std::uint8_t>> states {
        synthA,
        beepMapTestState ({ 5000, 600, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 64 }),
        beepMapTestState ({ 8000, 100, 2, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 64 }),
        beepMapTestState ({ 1000, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 64 }),
        beepMapPresetState ("Default.fst"),
        beepMapPresetState ("Voice.fst"),
    };
    const std::vector<std::vector<N>> sets {
        { { 0, 60, 1.0, 0.0, 0.0, -1 } },
        { { 0, 57, 0.8, 0.3, 25.0, -1 }, { 7000, 64, 0.6, -0.5, 0.0, -1 } },
        { { 0, 48, 1.0, 0.0, 0.0, -1 }, { 300, 72, 0.5, 0.0, -40.0, -1 } },
        { { 0, 67, 1.0, -0.2, 0.0, -1 } },
        { { 0, 60, 1.0, 0.0, 0.0, -1 } },
        { { 0, 55, 0.8, 0.0, 0.0, -1 }, { 20000, 62, 0.8, 0.4, 0.0, -1 } },
    };
    const Golden goldens[] {
        { 0, 44100, 512, 70.02738, 69.0877973, { { 1000, 0.030967699363827705f, -0.218118816614151f }, { 10000, 0.0f, 0.0f }, { 30000, 0.0f, 0.0f }, { 60000, 0.0f, 0.0f } } },
        { 1, 48000, 256, 2814.53446, 3514.46151, { { 1000, 0.02367428131401539f, 0.06526239961385727f }, { 10000, -0.06188536435365677f, 0.062191300094127655f }, { 30000, -0.153680220246315f, -0.27799028158187866f }, { 60000, 0.03855478763580322f, 0.1298033744096756f } } },
        { 2, 44100, 1000, 6644.01869, 7606.70378, { { 1000, 0.06019101291894913f, 0.2049546241760254f }, { 10000, -0.852304220199585f, -0.13111725449562073f }, { 30000, 0.11700113117694855f, -0.6130681037902832f }, { 60000, -0.2660403549671173f, -0.09453065693378448f } } },
        { 3, 96000, 512, 51.7332962, 28.9913476, { { 1000, -0.025792766362428665f, -0.07221365720033646f }, { 10000, 0.0f, 0.0f }, { 30000, 0.0f, 0.0f }, { 60000, 0.0f, 0.0f } } },
        { 4, 44100, 512, 8142.09413, 5639.86691, { { 1000, 0.050016239285469055f, 0.05289545655250549f }, { 10000, -0.258514404296875f, -0.16095803678035736f }, { 30000, 0.765974223613739f, 0.477584570646286f }, { 60000, 0.040680620819330215f, 0.17371401190757751f } } },
        { 5, 48000, 512, 2568.60856, 3975.24938, { { 1000, 0.07109857350587845f, 0.07109857350587845f }, { 10000, -0.057797715067863464f, -0.057797715067863464f }, { 30000, 0.08789601176977158f, 0.04657084494829178f }, { 60000, 0.08154996484518051f, 0.12709519267082214f } } },
    };
    for (const auto& g : goldens)
    {
        const auto& st = states[(std::size_t) g.set];
        if (st.empty())
            continue; // (no FL installation: the presets are skipped)
        const auto length = (int) g.fs * 2;
        const auto y = lynx::beepmap::renderReference (st, sets[(std::size_t) g.set], g.fs, length, g.block, 4321u + (std::uint32_t) g.set);
        double el = 0.0, er = 0.0;
        for (std::size_t i = 0; i + 1 < y.size(); i += 2)
            el += (double) y[i] * y[i], er += (double) y[i + 1] * y[i + 1];
        bool ok = y.size() == 2 * (std::size_t) length && std::abs (el - g.el) <= 1e-7 * g.el && std::abs (er - g.er) <= 1e-7 * g.er;
        for (const auto& smp : g.s)
            ok = ok && std::abs (y[2 * (std::size_t) smp.at] - smp.l) <= 1e-7f && std::abs (y[2 * (std::size_t) smp.at + 1] - smp.r) <= 1e-7f;
        if (! ok)
            std::printf ("  BeepMap case %d differs from FL's DLL: energy %.9g %.9g (DLL %.9g %.9g)\n", g.set, el, er, g.el, g.er);
        CHECK (ok);
    }

    // the instrument: plays the image, ends after the last column, a note-off fades the voice out
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::beepmap;
    c.pluginState = states[1];
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::beepmap);
    inst->configure (c, 120.0);
    inst->prepare (44100.0);
    std::vector<float> l (1024), r (1024);
    inst->noteOn (60, 0.8f);
    double e = 0.0;
    for (int b = 0; b < 8; ++b)
    {
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
        inst->render (l.data(), r.data(), 1024, 0.7f, 0.7f);
        for (int i = 0; i < 1024; ++i)
            e += (double) l[(std::size_t) i] * l[(std::size_t) i] + (double) r[(std::size_t) i] * r[(std::size_t) i];
    }
    CHECK (e > 0.01 && ! inst->idle()); // (Loop on)
    inst->noteOff (60);
    for (int b = 0; b < 4; ++b)
        inst->render (l.data(), r.data(), 1024, 0.7f, 0.7f);
    CHECK (inst->idle());
    c.pluginState = synthA; // (no loop: 9 columns of 64·(e^(200/4096·ln 33) − 1)… samples, then the voice ends)
    inst->configure (c, 120.0);
    inst->noteOn (60, 1.0f);
    for (int b = 0; b < 200 && ! inst->idle(); ++b)
        inst->render (l.data(), r.data(), 1024, 0.7f, 0.7f);
    CHECK (inst->idle());
}

// ---- Sawer (REA_rodada65 §3): installation presets and data.saw read at run time, fingerprints of engine_x64.dll ----
namespace
{
std::vector<std::uint8_t> sawerPresetState (const char* rel)
{
    if (realFlpDir.empty())
        return {};
    std::ifstream in (std::filesystem::path (realFlpDir) / "Patches" / "Plugin presets" / "Generators" / "Sawer" / rel, std::ios::binary);
    const std::vector<std::uint8_t> b ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    std::size_t i = 0;
    while (i + 8 <= b.size() && std::memcmp (b.data() + i, "FLdt", 4) != 0)
        ++i;
    i += 8;
    while (i < b.size())
    {
        const auto id = b[i++];
        std::size_t n = id < 64 ? 1 : id < 128 ? 2 : id < 192 ? 4 : 0;
        if (id >= 192)
            for (int shift = 0; i < b.size(); shift += 7)
            {
                const auto c = b[i++];
                n |= (std::size_t) (c & 0x7F) << shift;
                if (c < 0x80)
                    break;
            }
        if (id == 0xD5 && i + n <= b.size())
            return { b.begin() + (std::ptrdiff_t) i, b.begin() + (std::ptrdiff_t) (i + n) };
        i += n;
    }
    return {};
}
} // namespace

TEST (sawer_engine)
{
    // a synthetic header: "SSLF" + size + "1rwS" + 96 floats
    std::vector<std::uint8_t> synth (12 + 4 * 96, 0);
    std::memcpy (synth.data(), "SSLF", 4);
    const std::uint32_t size = 4 + 4 * 96;
    std::memcpy (synth.data() + 4, &size, 4);
    std::memcpy (synth.data() + 8, "1rwS", 4);
    const float p27 = 0.75f;
    std::memcpy (synth.data() + 12 + 4 * 27, &p27, 4);
    CHECK (lynx::sawerStateUsable (synth));
    CHECK (lynx::sawerParam (synth, 27) == 0.75f && lynx::sawerParamOffset (synth, 27) == 12 + 4 * 27);
    CHECK (! lynx::sawerParam (synth, 96) && lynx::sawerParamOffset (synth, -1) < 0);
    auto broken = synth;
    broken[8] = 'X';
    CHECK (! lynx::sawerStateUsable (broken));
    CHECK (! lynx::sawerStateUsable (std::vector<std::uint8_t> (synth.begin(), synth.end() - 4)));

    const auto dflt = sawerPresetState ("Default.fst");
    const auto slap = sawerPresetState ("Arpeggios/FG Velocity Slapper.fst");
    const auto tablePath = realFlpDir.empty() ? std::string() : (std::filesystem::path (realFlpDir).parent_path() / "Plugins" / "Fruity" / "Generators" / "Sawer" / "data.saw").string();
    const auto table = tablePath.empty() ? std::vector<float> {} : lynx::sawerReadTable (tablePath);
    if (dflt.empty() || slap.empty() || table.size() != 8192u)
    {
        std::printf ("  (sawer_engine: FL Studio installation not found, DLL fingerprints skipped)\n");
        return;
    }
    CHECK (lynx::sawerStateUsable (dflt) && lynx::sawerStateUsable (slap));

    // DLL fingerprints (py/gold_sw.py: the EXEC harness, L = R = Final.Vol); "syn" = Default with our own settings
    // (unison 4, sub pulse at 0.6, sync, band-pass, noise, resonance, chorus)
    auto setP = [] (std::vector<std::uint8_t> s, int k, float v) { std::memcpy (s.data() + 12 + 4 * k, &v, 4); return s; };
    auto syn = dflt;
    for (const auto& kv : { std::pair { 14, 1.0f }, { 23, 1.0f }, { 25, 1.0f }, { 24, 0.5f }, { 31, 0.4f }, { 47, 1.0f }, { 19, 0.6f },
                            { 26, 0.3f }, { 28, 0.7f }, { 45, 0.5f }, { 46, 0.0f } })
        syn = setP (syn, kv.first, kv.second);
    auto iv = [] (double vel) { return (std::pow (11.0, vel / 100.0) - 1.0) / 10.0; };
    using N = lynx::sawer::RefNote;
    struct Sample { int at; float l, r; };
    struct Golden { const std::vector<std::uint8_t>* state; std::vector<N> notes; double fs; int block; double el, er; Sample s[4]; };
    const Golden goldens[] {
        { &dflt, { { 0, 60, iv (100), 1.0, 0.0, 0.0, 30000 }, { 11025, 64, iv (80), 1.0, 0.0, 0.0, 40000 }, { 22050, 67, iv (110), 1.0, 0.0, 0.0, 52000 } },
          44100, 512, 1172.343107, 1169.673445,
          { { 1000, 0.0349505879f, 0.0349650607f }, { 10000, -0.0326882228f, -0.0326587185f }, { 30000, 0.139422998f, 0.139268339f }, { 60000, -1.01108653e-05f, -1.01210762e-05f } } },
        { &slap, { { 0, 57, iv (90), 1.0, 0.0, 0.0, 40000 }, { 500, 64, iv (70), 1.0, 0.0, 0.0, 40000 } }, 48000, 256, 62.85129835, 62.26892467,
          { { 1000, 0.00919434987f, 0.0769683793f }, { 10000, 0.0363935083f, 0.0227626693f }, { 30000, -0.00462511741f, -0.0105419969f }, { 60000, -0.00727002183f, -0.00159690797f } } },
        { &syn, { { 0, 48, iv (100), 1.0, 0.0, 0.0, 20000 }, { 5000, 55, iv (60), 1.0, 0.0, 25.0, 30000 } }, 44100, 1000, 579.5093366, 593.7384022,
          { { 1000, -0.162745371f, -0.172861978f }, { 10000, 0.0468756184f, 0.112415045f }, { 30000, -0.123887017f, -0.132816017f }, { 60000, 0.0f, 0.0f } } },
    };
    for (const auto& g : goldens)
    {
        const auto y = lynx::sawer::renderReference (*g.state, table, g.notes, g.fs, 66150, g.block);
        CHECK (y.size() == 2 * 66150u);
        if (y.size() != 2 * 66150u)
            continue;
        double el = 0.0, er = 0.0;
        for (std::size_t i = 0; i < 66150; ++i)
            el += (double) y[2 * i] * y[2 * i], er += (double) y[2 * i + 1] * y[2 * i + 1];
        CHECK (std::abs (el - g.el) < 1e-5 * g.el && std::abs (er - g.er) < 1e-5 * g.er);
        for (const auto& s : g.s)
            CHECK (std::abs (y[2 * (std::size_t) s.at] - s.l) < 1e-7f && std::abs (y[2 * (std::size_t) s.at + 1] - s.r) < 1e-7f);
    }

    // the instrument: the same engine behind LYNX's channel gains (centre → law A), velocity → Init.Vol; it goes idle
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::sawer;
    c.pluginState = dflt;
    c.samplePath = tablePath;
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::sawer);
    inst->configure (c, 120.0);
    inst->prepare (44100.0);
    std::vector<float> l (512 * 40, 0.0f), r (512 * 40, 0.0f);
    for (int b = 0; b < 40; ++b)
    {
        if (b == 0)
            inst->noteOn (60, 0.78125f);
        if (b == 10)
            inst->noteOff (60);
        inst->render (l.data() + 512 * b, r.data() + 512 * b, 512, 1.0f, 1.0f);
    }
    const auto ref = lynx::sawer::renderReference (dflt, table, { { 0, 60, (std::pow (11.0, 1.28 * 0.78125) - 1.0) / 10.0, 1.0, 0.0, 0.0, 5120 } }, 44100.0,
                                                   8192, 512, 120.0, 0, true);
    double diff = 0.0, energy = 0.0;
    for (int i = 0; i < 8192; ++i)
    {
        diff = std::max ({ diff, (double) std::abs (l[(std::size_t) i] - ref[2 * (std::size_t) i]), (double) std::abs (r[(std::size_t) i] - ref[2 * (std::size_t) i + 1]) });
        energy += (double) l[(std::size_t) i] * l[(std::size_t) i];
    }
    CHECK (diff == 0.0 && energy > 1e-3);
    CHECK (inst->idle());
}

TEST (transistorbass_engine)
{
    const auto factory = tbassPresetState ("Factory programs.fst");
    const auto seqOff = tbassPresetState ("Sequencer off.fst");
    if (factory.empty() || seqOff.empty())
        return; // no FL installation
    CHECK (factory.size() == 197887 && seqOff.size() == 196051);
    CHECK (lynx::transistorBassStateUsable (factory) && lynx::transistorBassStateUsable (seqOff));
    CHECK (lynx::transistorBassStateUsable (tbassWithEngine (factory, 2)));
    auto broken = factory;
    broken[0] = 3; // the wrapper refuses versions above 2
    CHECK (! lynx::transistorBassStateUsable (broken));
    auto truncated = factory;
    truncated.resize (truncated.size() - 1000);
    CHECK (! lynx::transistorBassStateUsable (truncated));
    const auto dll = tbassDll();
    if (dll.empty() || ! lynx::transistorBassDllUsable (dll))
        return;

    // against FL's engine DLL (EXEC, REA_rodada57 §2.11): programs of the factory bank (sequencer, accents, slides,
    // distortion, delay, reverb) on the three engines and the sequencer-off preset with legato and accent notes; host
    // blocks of 512 at 44.1 kHz, tempo 125. LYNX differs from the DLL by −121 dB or less here (library pow rounding).
    struct Sample { int at; float l, r; };
    struct Golden { const char* preset; int engine, key; double el, er; Sample s[4]; };
    const Golden goldens[] {
        { "Factory", -1, 0, 84.1648489, 84.1648489, { { 5000, -2.72742806e-09f, -2.72742806e-09f }, { 20000, -0.0366424844f, -0.0366424844f }, { 47000, 2.15875261e-07f, 2.15875261e-07f }, { 80000, 0.0f, 0.0f } } },
        { "Factory", -1, 3, 140.915804, 135.290403, { { 5000, 6.46460592e-07f, 6.46460592e-07f }, { 20000, 0.000612073636f, 0.00883315224f }, { 47000, 0.00334156211f, 0.0349708796f }, { 80000, -0.0180665664f, -0.00647424255f } } },
        { "Factory", -1, 9, 74.6013145, 74.6013145, { { 5000, 1.17278e-06f, 1.17278e-06f }, { 20000, 4.45517001e-09f, 4.45517001e-09f }, { 47000, -0.00680137333f, -0.00680137333f }, { 80000, 9.70963379e-11f, 9.70963379e-11f } } },
        { "Factory", -1, 21, 672.27801, 665.683557, { { 5000, 9.88614047e-05f, -0.00434152689f }, { 20000, 0.00984471664f, -0.00176824001f }, { 47000, -0.00745432731f, -0.00174837175f }, { 80000, 0.00471894164f, -0.00477111898f } } },
        { "Factory", -1, 65, 534.422868, 543.176189, { { 5000, -0.00286525022f, -0.00691002142f }, { 20000, -0.0952312946f, -0.0940120071f }, { 47000, -0.109519325f, -0.115677707f }, { 80000, 0.0193505734f, 0.0466997996f } } },
        { "Factory", 1, 65, 531.095944, 535.778912, { { 5000, 0.00939375907f, 0.000506323006f }, { 20000, 0.0880209357f, 0.0815288723f }, { 47000, 0.0840308368f, 0.0360141024f }, { 80000, -0.0445356891f, -0.058858037f } } },
        { "Factory", 2, 65, 577.993833, 585.388538, { { 5000, -0.000922904757f, 0.0082119517f }, { 20000, 0.100198291f, 0.0890608132f }, { 47000, 0.0754022077f, 0.133105934f }, { 80000, -0.0728695691f, -0.0721700341f } } },
        { "SeqOff", -1, 48, 27.8141841, 27.8141841, { { 5000, -0.0331321731f, -0.0331321731f }, { 20000, -0.016056465f, -0.016056465f }, { 47000, -0.0118189175f, -0.0118189175f }, { 80000, 0.0f, 0.0f } } },
        { "SeqOff", 1, 48, 21.7931524, 21.7931524, { { 5000, 0.0268912539f, 0.0268912539f }, { 20000, 0.0150698861f, 0.0150698861f }, { 47000, 0.011147175f, 0.011147175f }, { 80000, -1.04869307e-23f, -1.04869307e-23f } } },
        { "SeqOff", 2, 48, 61.3736312, 61.3736312, { { 5000, 0.0370108262f, 0.0370108262f }, { 20000, 0.0206214525f, 0.0206214525f }, { 47000, 0.0157183651f, 0.0157183651f }, { 80000, -1.3009924e-23f, -1.3009924e-23f } } },
    };
    constexpr int length = 88200;
    for (const auto& g : goldens)
    {
        const bool factoryBank = std::string (g.preset) == "Factory";
        auto st = factoryBank ? factory : seqOff;
        if (g.engine >= 0)
            st = tbassWithEngine (st, g.engine);
        std::vector<lynx::tbass::RefNote> notes;
        if (factoryBank)
            notes.push_back ({ 0, g.key, 0.5, 0.0, 0.0, 0.0, 0.0, 70560 });
        else
        {
            notes.push_back ({ 0, g.key, 0.5, 0.0, 0.0, 0.0, 0.0, 30000 });
            notes.push_back ({ 20000, g.key + 7, 1.6, 0.0, 0.0, 0.0, 0.0, 50000 });
            notes.push_back ({ 45000, g.key - 5, 0.3, 0.0, 0.0, 0.0, 0.0, 70560 });
        }
        const auto y = lynx::tbass::renderReference (st, dll, notes, 44100.0, length);
        double el = 0.0, er = 0.0, peak = 0.0;
        for (std::size_t i = 0; i + 1 < y.size(); i += 2)
            el += (double) y[i] * y[i], er += (double) y[i + 1] * y[i + 1], peak = std::max ({ peak, (double) std::abs (y[i]), (double) std::abs (y[i + 1]) });
        bool ok = y.size() == 2 * (std::size_t) length && std::abs (el - g.el) <= 1e-5 * g.el && std::abs (er - g.er) <= 1e-5 * g.er;
        for (const auto& smp : g.s)
            ok = ok && std::abs (y[2 * (std::size_t) smp.at] - smp.l) <= 1e-5 * peak && std::abs (y[2 * (std::size_t) smp.at + 1] - smp.r) <= 1e-5 * peak;
        if (! ok)
            std::printf ("  Transistor Bass %s engine %d key %d differs from FL's DLL: energy %.9g %.9g (DLL %.9g %.9g)\n", g.preset, g.engine, g.key, el, er, g.el, g.er);
        CHECK (ok);
    }

    // the instrument: FL's wrapper velocity, the channel volume as an output gain, the sequencer plays while the key is held
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::transistorbass;
    c.pluginState = factory;
    c.samplePath = dll;
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::transistorbass);
    inst->configure (c, 128.0);
    inst->prepare (44100.0);
    std::vector<float> l (4096), r (4096);
    inst->noteOn (65, 0.8f);
    double e = 0.0;
    bool finite = true;
    for (int b = 0; b < 24; ++b)
    {
        std::fill (l.begin(), l.end(), 0.0f);
        std::fill (r.begin(), r.end(), 0.0f);
        inst->render (l.data(), r.data(), 4096, 0.5f, 0.5f);
        for (int i = 0; i < 4096; ++i)
            finite = finite && std::isfinite (l[(std::size_t) i]) && std::isfinite (r[(std::size_t) i]), e += (double) l[(std::size_t) i] * l[(std::size_t) i];
        if (b == 20)
            inst->noteOff (65);
    }
    CHECK (finite && e > 1.0 && ! inst->idle());
    for (int b = 0; b < 400 && ! inst->idle(); ++b)
        inst->render (l.data(), r.data(), 4096, 0.5f, 0.5f);
    CHECK (inst->idle());
}

TEST (kepler_demo_channels)
{
    // every Kepler channel of FL's demo projects on LYNX's engine against FL's engine DLL (EXEC fingerprints, REA_rodada46 §6;
    // the engine differs from the DLL by −127 dB or less on these, from the DLL library's pow rounding)
    const auto dll = keplerDll();
    if (dll.empty() || ! lynx::keplerDllUsable (dll))
        return;
    struct Sample { int at; float l, r; };
    struct Golden { const char* file; const char* channel; std::size_t stateSize; double el, er; Sample s[4]; };
    const Golden goldens[] {
        { "Demo songs/Edlan & Ella Noël - Song For You.flp", "Pads - Kepler", 181, 2595.34411, 1181.70052, { { 5000, 0.258449465f, 0.284368932f }, { 12000, -0.0648983642f, 0.312173635f }, { 25000, -0.271028787f, 0.103852645f }, { 35000, -0.00873807631f, -0.00636588456f } } },
        { "Demo songs/Purps On The Beat Feat Luclover - Stroll Up.flp", "Kepler", 189, 2.77173241, 1.36643586, { { 5000, 0.00653714081f, 0.00885556825f }, { 12000, 0.0316556357f, 0.0255686305f }, { 25000, -0.00359999924f, -0.0027268643f }, { 35000, 3.38630743e-05f, 3.35237855e-05f } } },
        { "Demo songs/Sid De La Cruz - Secret Agent Rendezvous.flp", "AceBass", 181, 491.893665, 813.858274, { { 5000, 0.00933594909f, 0.0551614426f }, { 12000, 0.11930909f, -0.387893438f }, { 25000, -0.0520403236f, -0.131727993f }, { 35000, 2.78028492e-05f, 2.75182774e-05f } } },
        { "Demo songs/Sid De La Cruz - Secret Agent Rendezvous.flp", "Badious", 181, 19.0406926, 20.1997196, { { 5000, 0.00332317385f, 0.00785329007f }, { 12000, 0.0234184917f, -0.0034834384f }, { 25000, -0.0167671479f, -0.0142839653f }, { 35000, -0.0352748372f, -0.00481338287f } } },
    };
    int checked = 0;
    for (const auto& g : goldens)
    {
        std::ifstream in (std::filesystem::path (realFlpDir) / "Demo projects" / g.file, std::ios::binary);
        const std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
        if (bytes.empty())
            continue;
        const flp::Channel* ch = nullptr;
        const auto project = flp::parseProject (bytes);
        for (const auto& c : project.channels)
            if (c.pluginName == "Kepler" && c.name == g.channel)
                ch = &c;
        CHECK (ch != nullptr);
        if (ch == nullptr)
            continue;
        CHECK (ch->pluginState.size() == g.stateSize && lynx::keplerStateUsable (ch->pluginState));
        const auto y = lynx::kepler::renderReference (ch->pluginState, dll, { { 0, 60, 1.0, 0.0, 0.0, 20000 }, { 8000, 67, 0.6, 0.0, 0.0, 26000 },
                                                                              { 16000, 55, 0.95, 0.0, 0.0, 30000 } }, 44100.0, 39690);
        double el = 0.0, er = 0.0, peak = 0.0;
        for (std::size_t i = 0; i + 1 < y.size(); i += 2)
            el += (double) y[i] * y[i], er += (double) y[i + 1] * y[i + 1], peak = std::max ({ peak, (double) std::abs (y[i]), (double) std::abs (y[i + 1]) });
        bool ok = y.size() == 2 * 39690 && std::abs (el - g.el) <= 1e-5 * g.el && std::abs (er - g.er) <= 1e-5 * g.er;
        for (const auto& smp : g.s)
            ok = ok && std::abs (y[2 * (std::size_t) smp.at] - smp.l) <= 1e-5 * peak && std::abs (y[2 * (std::size_t) smp.at + 1] - smp.r) <= 1e-5 * peak;
        if (! ok)
            std::printf ("  Kepler %s / %s differs from FL's DLL: energy %.9g %.9g (DLL %.9g %.9g)\n", g.file, g.channel, el, er, g.el, g.er);
        CHECK (ok);
        ++checked;
    }
    std::printf ("  Kepler: %d demo channels checked against FL's engine DLL\n", checked);
    CHECK (checked == (int) std::size (goldens));
}

TEST (ogun_demo_channels)
{
    // every Ogun channel of FL's demo projects (with its plugin effects) against the round-37 reference model, which is
    // within −60 dB of FL's DLL on all of them: three notes (C5 1.0 0–20000, G5 0.6 8000–26000, G4 0.95 16000–30000),
    // 39 690 samples at 44.1 kHz, at the state's quality and at the export quality 4. Fingerprint: energy per side and
    // four samples (the engine matches the model within float rounding, −125 dB or better on the whole signal).
    if (realFlpDir.empty())
        return;
    struct Sample { int at; float l, r; };
    struct Golden { const char* file; int ordinal; std::size_t stateSize; bool export4; double el, er; Sample s[4]; };
    const Golden goldens[] {
        { "Demo songs/Astes - Bien Duro.flp", 0, 64728, false, 710.708198, 710.708198, { { 5000, 0.133627325f, 0.133627325f }, { 12000, -0.143534407f, -0.143534407f }, { 25000, 0.079226315f, 0.079226315f }, { 35000, -0.0128487358f, -0.0128487358f } } }, // 3.3.1 Metal Drums: Drum
        { "Demo songs/Astes - Bien Duro.flp", 0, 64728, true, 794.506558, 794.506558, { { 5000, -0.0112881307f, -0.0112881307f }, { 12000, -0.068779245f, -0.068779245f }, { 25000, -0.0289028231f, -0.0289028231f }, { 35000, -0.0161623377f, -0.0161623377f } } }, // 3.3.1 Metal Drums: Drum
        { "Demo songs/Astes - Bien Duro.flp", 1, 4261, false, 297.957631, 297.957631, { { 5000, -0.000330464332f, -0.000330464332f }, { 12000, -0.00156057056f, -0.00156057056f }, { 25000, -9.24126434e-05f, -9.24126434e-05f }, { 35000, 0.0f, 0.0f } } }, // 3.3.1 Metal Drums: Click
        { "Demo songs/Astes - Bien Duro.flp", 1, 4261, true, 189.545711, 189.545711, { { 5000, 0.00102557975f, 0.00102557975f }, { 12000, -0.00522088073f, -0.00522088073f }, { 25000, 5.81103013e-05f, 5.81103013e-05f }, { 35000, 0.0f, 0.0f } } }, // 3.3.1 Metal Drums: Click
        { "Demo songs/Astes - Bien Duro.flp", 2, 133836, false, 125.683557, 125.683557, { { 5000, 0.0176561493f, 0.0176561493f }, { 12000, 0.019205872f, 0.019205872f }, { 25000, 0.0046372693f, 0.0046372693f }, { 35000, -0.00165430119f, -0.00165430119f } } }, // 3.3.2 Tibetan
        { "Demo songs/Astes - Bien Duro.flp", 2, 133836, true, 105.621798, 105.621798, { { 5000, 0.00897848886f, 0.00897848886f }, { 12000, -0.0171215143f, -0.0171215143f }, { 25000, -0.00821319036f, -0.00821319036f }, { 35000, -0.00245537539f, -0.00245537539f } } }, // 3.3.2 Tibetan
        { "Demo songs/Astes - Bien Duro.flp", 3, 97085, false, 2066.75611, 2066.75611, { { 5000, -0.101155393f, -0.101155393f }, { 12000, 0.355391592f, 0.355391592f }, { 25000, 0.00365939317f, 0.00365939317f }, { 35000, -0.0191957876f, -0.0191957876f } } }, // 3.3.3 Chase
        { "Demo songs/Astes - Bien Duro.flp", 3, 97085, true, 1667.28674, 1667.28674, { { 5000, -0.236701503f, -0.236701503f }, { 12000, -0.126374632f, -0.126374632f }, { 25000, 0.0533663221f, 0.0533663221f }, { 35000, -0.0100225089f, -0.0100225089f } } }, // 3.3.3 Chase
        { "Demo songs/Edlan & Ella Noël - Song For You.flp", 0, 819, false, 551.263856, 371.038684, { { 5000, 0.0253486633f, 0.160556272f }, { 12000, 0.194370747f, -0.0359776132f }, { 25000, -0.00528950989f, 0.01478827f }, { 35000, 0.110768802f, -0.0237054806f } } }, // Bellish 11
        { "Demo songs/Edlan & Ella Noël - Song For You.flp", 0, 819, true, 547.662092, 381.048456, { { 5000, 0.0401367024f, 0.0261611789f }, { 12000, -0.157383129f, -0.0282031279f }, { 25000, -0.000249146833f, 0.0388034321f }, { 35000, -0.105665907f, 0.0320193283f } } }, // Bellish 11
        { "Demo songs/LollieVox & Lezina J - I'll Be Your Madonna.flp", 0, 105571, false, 833.288142, 909.880193, { { 5000, 0.0276863724f, 0.214677185f }, { 12000, 0.00827352796f, 0.141611829f }, { 25000, -0.160458148f, 0.0101522766f }, { 35000, -0.0157076102f, 0.00129696913f } } }, // High Chords
        { "Demo songs/LollieVox & Lezina J - I'll Be Your Madonna.flp", 0, 105571, true, 928.749154, 967.940976, { { 5000, -0.0623336285f, -0.0959333256f }, { 12000, 0.0601816922f, -0.0748261437f }, { 25000, -0.0872222036f, -0.101655915f }, { 35000, -0.002591094f, 0.0107459351f } } }, // High Chords
        { "Product demos/Electric Loop - The Ogun Dream.flp", 0, 813, false, 21576.8988, 21576.8988, { { 5000, 0.00724618789f, 0.00724618789f }, { 12000, 0.0337664448f, 0.0337664448f }, { 25000, -1.44279329e-05f, -1.44279329e-05f }, { 35000, 0.0f, 0.0f } } }, // Kick
        { "Product demos/Electric Loop - The Ogun Dream.flp", 0, 813, true, 17857.5027, 17857.5027, { { 5000, -0.0114536202f, -0.0114536202f }, { 12000, -0.00848954543f, -0.00848954543f }, { 25000, 1.20320046e-05f, 1.20320046e-05f }, { 35000, 0.0f, 0.0f } } }, // Kick
        { "Product demos/Electric Loop - The Ogun Dream.flp", 1, 789, false, 172.127165, 170.39981, { { 5000, -0.00470701791f, -0.000373305753f }, { 12000, 0.0417476855f, 0.0339940563f }, { 25000, 0.0204325765f, 0.0191226583f }, { 35000, -0.000425090868f, -0.000144533216f } } }, // Clap
        { "Product demos/Electric Loop - The Ogun Dream.flp", 1, 789, true, 163.557022, 149.810459, { { 5000, 0.00246403809f, 0.0228729937f }, { 12000, 0.0378073864f, 0.0445459485f }, { 25000, -0.0242712852f, -0.0313444398f }, { 35000, -0.00112735468f, -0.00138637482f } } }, // Clap
        { "Product demos/Electric Loop - The Ogun Dream.flp", 2, 635, false, 125.48103, 127.519836, { { 5000, -0.00389297446f, 0.00181007839f }, { 12000, -0.0543250106f, -0.0736973509f }, { 25000, -0.00448145531f, 0.000426211918f }, { 35000, 5.35470353e-05f, 4.34564136e-05f } } }, // High Hat
        { "Product demos/Electric Loop - The Ogun Dream.flp", 2, 635, true, 245.916518, 254.34816, { { 5000, 0.00147765165f, 0.00267027458f }, { 12000, 0.0883170143f, 0.173417449f }, { 25000, -0.00118842872f, 0.000863731664f }, { 35000, -0.000298409053f, -9.04561821e-05f } } }, // High Hat
        { "Product demos/Electric Loop - The Ogun Dream.flp", 3, 45647, false, 271.486026, 271.486026, { { 5000, -4.96093298e-06f, -4.96093298e-06f }, { 12000, 0.000292191253f, 0.000292191253f }, { 25000, -8.18665136e-10f, -8.18665136e-10f }, { 35000, 0.0f, 0.0f } } }, // HiHat
        { "Product demos/Electric Loop - The Ogun Dream.flp", 3, 45647, true, 167.753037, 167.753037, { { 5000, 2.2048649e-05f, 2.2048649e-05f }, { 12000, -1.77328075e-05f, -1.77328075e-05f }, { 25000, -3.00809999e-09f, -3.00809999e-09f }, { 35000, 0.0f, 0.0f } } }, // HiHat
        { "Product demos/Electric Loop - The Ogun Dream.flp", 4, 887, false, 6005.75168, 6293.90961, { { 5000, -0.19636783f, -0.0313744769f }, { 12000, 0.178472087f, 0.601032794f }, { 25000, -0.351912051f, -0.0497151017f }, { 35000, -0.182245821f, -0.181938142f } } }, // Snare
        { "Product demos/Electric Loop - The Ogun Dream.flp", 4, 887, true, 6151.47444, 6049.47136, { { 5000, -0.102814399f, 0.0690701902f }, { 12000, 0.0902677849f, 0.114889771f }, { 25000, -0.173990116f, 0.140671447f }, { 35000, 0.0744562522f, 0.0291433111f } } }, // Snare
        { "Product demos/Electric Loop - The Ogun Dream.flp", 5, 705, false, 6511.3723, 5580.42134, { { 5000, 0.478397369f, 0.536334693f }, { 12000, -0.312448859f, -0.301919997f }, { 25000, 0.260874003f, -0.0129742175f }, { 35000, 0.0223956257f, 0.033103168f } } }, // Swebass 1
        { "Product demos/Electric Loop - The Ogun Dream.flp", 5, 705, true, 6415.0768, 5859.44881, { { 5000, 0.0342692807f, 0.00727743423f }, { 12000, -0.0324966274f, 0.125386745f }, { 25000, -0.0785191655f, -0.25017795f }, { 35000, -0.0524680056f, -0.0384391509f } } }, // Swebass 1
        { "Product demos/Electric Loop - The Ogun Dream.flp", 6, 708, false, 2487.6041, 2190.17262, { { 5000, -0.0770967156f, -0.00940297544f }, { 12000, 0.329836905f, 0.183736563f }, { 25000, 0.0137981772f, -0.0334252715f }, { 35000, 0.0132095823f, 0.0277119074f } } }, // Swebass 2
        { "Product demos/Electric Loop - The Ogun Dream.flp", 6, 708, true, 2519.26975, 2230.77243, { { 5000, -0.204216316f, -0.16732122f }, { 12000, 0.0762225091f, 0.0964485779f }, { 25000, -0.0833523944f, -0.132980049f }, { 35000, -0.015998248f, 0.0112942485f } } }, // Swebass 2
        { "Product demos/Electric Loop - The Ogun Dream.flp", 7, 109827, false, 1326.11782, 1397.71125, { { 5000, 0.0306534693f, 0.026510708f }, { 12000, -0.139238164f, -0.104713492f }, { 25000, 0.106734768f, 0.0295313746f }, { 35000, 0.258899271f, 0.260215104f } } }, // Midnight Lady
        { "Product demos/Electric Loop - The Ogun Dream.flp", 7, 109827, true, 1252.058, 1287.83169, { { 5000, -0.0980920121f, -0.108575724f }, { 12000, 0.131130368f, 0.176118359f }, { 25000, 0.0581845529f, 0.174699515f }, { 35000, -0.17453593f, 0.0978319347f } } }, // Midnight Lady
        { "Product demos/Electric Loop - The Ogun Dream.flp", 8, 34816, false, 10793.0192, 11904.6549, { { 5000, 0.36459741f, -0.0714551434f }, { 12000, 0.565215111f, -0.205066338f }, { 25000, -0.652977228f, -0.432422459f }, { 35000, -0.284279644f, -0.122116163f } } }, // Supersaw
        { "Product demos/Electric Loop - The Ogun Dream.flp", 8, 34816, true, 10132.5107, 11590.7617, { { 5000, 0.192168862f, 0.0883051828f }, { 12000, -0.590734482f, 0.512839496f }, { 25000, 0.682625771f, 0.287306368f }, { 35000, -0.00662239315f, 0.700808227f } } }, // Supersaw
        { "Product demos/Electric Loop - The Ogun Dream.flp", 9, 94266, false, 826.69243, 1157.08939, { { 5000, 0.0348474756f, -0.168766886f }, { 12000, -0.0719366521f, 0.288826376f }, { 25000, 0.181558818f, 0.0424098521f }, { 35000, -0.321783781f, -0.0548994206f } } }, // Airguitar 3
        { "Product demos/Electric Loop - The Ogun Dream.flp", 9, 94266, true, 830.517691, 772.62552, { { 5000, -0.208309069f, 0.152180359f }, { 12000, -0.264171928f, 0.163982511f }, { 25000, 0.0313629843f, -0.187507361f }, { 35000, 0.133838147f, 0.0704783946f } } }, // Airguitar 3
        { "Product demos/Electric Loop - The Ogun Dream.flp", 10, 1004, false, 12678.7464, 11831.2032, { { 5000, 0.192205578f, 0.613839507f }, { 12000, 0.269521117f, -0.585603476f }, { 25000, 0.0944993123f, 0.0806781948f }, { 35000, -0.16715017f, 0.412193805f } } }, // Trumpet
        { "Product demos/Electric Loop - The Ogun Dream.flp", 10, 1004, true, 12261.8266, 9962.482, { { 5000, 0.422391087f, -0.251674503f }, { 12000, -0.46900028f, -0.57652849f }, { 25000, -0.535878778f, 0.115671799f }, { 35000, -0.545713186f, 0.0383624472f } } }, // Trumpet
        { "Product demos/Electric Loop - The Ogun Dream.flp", 11, 30605, false, 1358.26232, 1520.29691, { { 5000, -0.0350573957f, 0.0372484438f }, { 12000, 0.0963054374f, -0.130206674f }, { 25000, 0.0404771306f, -0.270439237f }, { 35000, -0.0870027244f, 0.0245792065f } } }, // Silverswirll
        { "Product demos/Electric Loop - The Ogun Dream.flp", 11, 30605, true, 964.637209, 1040.25756, { { 5000, 0.111636952f, -0.00114471605f }, { 12000, 0.0483313873f, 0.0637735277f }, { 25000, -0.152803347f, 0.101604536f }, { 35000, 0.0288163591f, 0.0194431469f } } }, // Silverswirll
        { "Product demos/Electric Loop - The Ogun Dream.flp", 12, 34708, false, 21463.1948, 22013.1021, { { 5000, -0.00810255855f, -0.0122786947f }, { 12000, -0.163549975f, -0.203571558f }, { 25000, 0.419101298f, -1.1141516f }, { 35000, 0.498232335f, -0.394363374f } } }, // Padsaw
        { "Product demos/Electric Loop - The Ogun Dream.flp", 12, 34708, true, 22903.9209, 21556.3509, { { 5000, -0.00977799948f, -0.0114432834f }, { 12000, -0.0759382695f, 0.077038914f }, { 25000, -0.556497335f, -1.22787583f }, { 35000, -1.31076968f, -1.73392022f } } }, // Padsaw
        { "Product demos/Electric Loop - The Ogun Dream.flp", 13, 6395, false, 3516.18148, 3565.45486, { { 5000, -0.0564712696f, -0.262329876f }, { 12000, 0.113444217f, 0.168150783f }, { 25000, -0.45024398f, -0.694699228f }, { 35000, 0.151807249f, -0.00405737339f } } }, // Turbular Bells 2
        { "Product demos/Electric Loop - The Ogun Dream.flp", 13, 6395, true, 3571.34372, 3626.15163, { { 5000, -0.0878480822f, -0.275067806f }, { 12000, 0.230907321f, 0.28320539f }, { 25000, -0.470044613f, -0.701287091f }, { 35000, 0.195627496f, 0.0175700653f } } }, // Turbular Bells 2
    };
    std::map<std::string, std::vector<std::vector<std::uint8_t>>> states;
    int checked = 0;
    for (const auto& g : goldens)
    {
        if (! states.contains (g.file))
        {
            std::ifstream in (std::filesystem::path (realFlpDir) / "Demo projects" / g.file, std::ios::binary);
            const std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
            auto& list = states[g.file];
            if (! bytes.empty())
                for (const auto& c : flp::parseProject (bytes).channels)
                    if (c.pluginName == "Ogun")
                        list.push_back (c.pluginState);
        }
        const auto& list = states[g.file];
        if (list.empty())
            continue; // not this installation
        CHECK ((int) list.size() > g.ordinal);
        if ((int) list.size() <= g.ordinal)
            continue;
        const auto& st = list[(std::size_t) g.ordinal];
        CHECK (st.size() == g.stateSize);
        lynx::ogun::RefOptions opt;
        opt.quality = g.export4 ? 4 : -1;
        const auto y = lynx::ogun::renderReference (st, { { 0, 60, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 20000 },
                                                          { 8000, 67, 0.6, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 26000 },
                                                          { 16000, 55, 0.95, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 30000 } }, 44100.0, 39690, opt);
        double el = 0.0, er = 0.0, peak = 0.0;
        for (std::size_t i = 0; i + 1 < y.size(); i += 2)
            el += (double) y[i] * y[i], er += (double) y[i + 1] * y[i + 1], peak = std::max ({ peak, (double) std::abs (y[i]), (double) std::abs (y[i + 1]) });
        bool ok = y.size() == 2 * 39690 && std::abs (el - g.el) <= 1e-5 * g.el && std::abs (er - g.er) <= 1e-5 * g.er;
        for (const auto& smp : g.s)
            ok = ok && std::abs (y[2 * (std::size_t) smp.at] - smp.l) <= 2e-5 * peak && std::abs (y[2 * (std::size_t) smp.at + 1] - smp.r) <= 2e-5 * peak;
        if (! ok)
            std::printf ("  Ogun %s #%d (q%s) differs from the model: energy %.9g %.9g (model %.9g %.9g)\n", g.file, g.ordinal,
                         g.export4 ? "4" : "-state", el, er, g.el, g.er);
        CHECK (ok);
        ++checked;
    }
    std::printf ("  Ogun: %d demo renders checked against the reference model\n", checked);
    CHECK (checked == (int) std::size (goldens));
}

TEST (autogun_template_channels)
{
    // REA_rodada57: every distinct Autogun state of the installation's projects (templates and the render test), with its
    // effects and the Magic Dust stage (SG.dat from the installation), three notes (C5 1.0 0–20000, G5 0.6 8000–26000,
    // G4 0.95 16000–30000), 39 690 samples at 44.1 kHz, at the state's quality and at the export quality 4. The template
    // goldens are renders of the installation's Autogun_x64.dll (LYNX is within −127 dB of them); the render-test state
    // uses Seed 2 "Random", so its golden is LYNX's own render with the counter at 0 (a fingerprint of the engine).
    if (realFlpDir.empty())
        return;
    const auto sgPath = std::filesystem::path (realFlpDir).parent_path() / "Plugins" / "Fruity" / "Generators" / "Autogun" / "Data" / "SG.dat";
    std::ifstream sgIn (sgPath, std::ios::binary);
    const std::vector<std::uint8_t> sgState ((std::istreambuf_iterator<char> (sgIn)), std::istreambuf_iterator<char>());
    if (sgState.size() != 1342)
        return; // not this installation
    struct Sample { int at; float l, r; };
    struct Golden { const char* file; std::size_t stateSize; bool export4; double el, er; Sample s[4]; };
    const Golden goldens[] {
{ "System/Render tests/Autogun.flp", 2403, false, 1898.57144, 1609.64764, { { 5000, 0.189644396f, 0.104252934f }, { 12000, 0.0354045518f, 0.002853852f }, { 25000, -0.212527201f, -0.364677101f }, { 35000, 0.00715163164f, -0.0779336989f } } }, // Autogun (LYNX engine, counter 0)
        { "System/Render tests/Autogun.flp", 2403, true, 1883.83096, 1696.41005, { { 5000, -0.229550153f, -0.0656554326f }, { 12000, -0.243209839f, -0.191908702f }, { 25000, 0.174803019f, 0.125847161f }, { 35000, 0.0609342083f, 0.0861448273f } } }, // Autogun (LYNX engine, counter 0)
        { "Templates/Advanced/Electronic/Breakbeat.flp", 43615, false, 1156.8126, 667.1251, { { 5000, -0.122245498f, -0.00968936551f }, { 12000, 0.088016659f, 0.0382440798f }, { 25000, -0.0438547991f, -0.0149139315f }, { 35000, -0.0162006877f, 0.0219070036f } } }, // Pad (Autogun_x64.dll)
        { "Templates/Advanced/Electronic/Breakbeat.flp", 43615, true, 541.425656, 476.875716, { { 5000, 0.15942347f, 0.0947837308f }, { 12000, 0.172008425f, 0.197265282f }, { 25000, 0.0506084785f, 0.0015097193f }, { 35000, 0.0400503464f, 0.0195946041f } } }, // Pad (Autogun_x64.dll)
        { "Templates/Advanced/Electronic/Chillout.flp", 171641, false, 1463.17228, 1370.65276, { { 5000, 0.158808231f, -0.128741398f }, { 12000, 0.208011121f, -0.153484732f }, { 25000, -0.184889361f, 0.0409393087f }, { 35000, 0.0255694874f, 0.0495453961f } } }, // Pad 5 (Autogun_x64.dll)
        { "Templates/Advanced/Electronic/Chillout.flp", 171641, true, 1412.70901, 1469.84358, { { 5000, -0.0218960885f, -0.0802078545f }, { 12000, -0.458583713f, 0.146813422f }, { 25000, -0.198403969f, 0.0604776815f }, { 35000, -0.097854577f, 0.0806856826f } } }, // Pad 5 (Autogun_x64.dll)
        { "Templates/Advanced/Electronic/HipHop-Trap.flp", 9787, false, 2807.37714, 3754.76445, { { 5000, -0.0391585156f, 0.332697481f }, { 12000, -0.128277287f, -0.632223666f }, { 25000, 0.103559501f, 0.521154523f }, { 35000, 0.309904903f, 0.280873239f } } }, // Pad (Autogun_x64.dll)
        { "Templates/Advanced/Electronic/HipHop-Trap.flp", 9787, true, 3526.66329, 4199.20992, { { 5000, 0.256542385f, 0.0431160629f }, { 12000, -0.283133686f, -0.732375383f }, { 25000, -0.210154608f, -0.276185691f }, { 35000, -0.144943774f, -0.315269828f } } }, // Pad (Autogun_x64.dll)
        { "Templates/Advanced/Electronic/Trap.flp", 9780, false, 2807.37714, 3754.76445, { { 5000, -0.0391585156f, 0.332697481f }, { 12000, -0.128277287f, -0.632223666f }, { 25000, 0.103559501f, 0.521154523f }, { 35000, 0.309904903f, 0.280873239f } } }, // Pad (Autogun_x64.dll)
        { "Templates/Advanced/Electronic/Trap.flp", 9780, true, 3526.66329, 4199.20992, { { 5000, 0.256542385f, 0.0431160629f }, { 12000, -0.283133686f, -0.732375383f }, { 25000, -0.210154608f, -0.276185691f }, { 35000, -0.144943774f, -0.315269828f } } }, // Pad (Autogun_x64.dll)
    };
    int checked = 0;
    for (const auto& g : goldens)
    {
        std::ifstream in (std::filesystem::path (realFlpDir) / g.file, std::ios::binary);
        const std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
        if (bytes.empty())
            continue;
        std::vector<std::uint8_t> st;
        for (const auto& c : flp::parseProject (bytes).channels)
            if (c.pluginName == "Autogun" && st.empty())
                st = c.pluginState;
        CHECK (st.size() == g.stateSize);
        CHECK (lynx::autogunStateUsable (st));
        lynx::ogun::RefOptions opt;
        opt.autogun = true;
        opt.sgState = sgState;
        opt.quality = g.export4 ? 4 : -1;
        const auto y = lynx::ogun::renderReference (st, { { 0, 60, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 20000 },
                                                          { 8000, 67, 0.6, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 26000 },
                                                          { 16000, 55, 0.95, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 30000 } }, 44100.0, 39690, opt);
        double el = 0.0, er = 0.0, peak = 0.0;
        for (std::size_t i = 0; i + 1 < y.size(); i += 2)
            el += (double) y[i] * y[i], er += (double) y[i + 1] * y[i + 1], peak = std::max ({ peak, (double) std::abs (y[i]), (double) std::abs (y[i + 1]) });
        bool ok = y.size() == 2 * 39690 && std::abs (el - g.el) <= 1e-5 * g.el && std::abs (er - g.er) <= 1e-5 * g.er;
        for (const auto& smp : g.s)
            ok = ok && std::abs (y[2 * (std::size_t) smp.at] - smp.l) <= 2e-5 * peak && std::abs (y[2 * (std::size_t) smp.at + 1] - smp.r) <= 2e-5 * peak;
        if (! ok)
            std::printf ("  Autogun %s (q%s) differs: energy %.9g %.9g (golden %.9g %.9g)\n", g.file, g.export4 ? "4" : "-state", el, er, g.el, g.er);
        CHECK (ok);
        ++checked;
    }
    std::printf ("  Autogun: %d template/render-test renders checked (DLL goldens for the fixed-spectrum states)\n", checked);
    CHECK (checked == (int) std::size (goldens));

    // the instrument (SG.dat read from ChannelDef::samplePath) plays like the reference renderer, then goes idle
    {
        std::ifstream in (std::filesystem::path (realFlpDir) / "Templates" / "Advanced" / "Electronic" / "Trap.flp", std::ios::binary);
        const std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
        lynx::ChannelDef c;
        c.instrument = lynx::InstrumentKind::autogun;
        for (const auto& ch : flp::parseProject (bytes).channels)
            if (ch.pluginName == "Autogun")
                c.pluginState = ch.pluginState;
        c.samplePath = sgPath.string();
        lynx::ogun::RefOptions opt;
        opt.autogun = true;
        opt.sgState = sgState;
        opt.tempo = 120.0; // the instrument is configured at 120 BPM
        const auto ref = lynx::ogun::renderReference (c.pluginState, { { 0, 60, 0.75, 1.0, 0.0, 0.0, 0.0, 0.0, 0.5, 6144 } }, 44100.0, 16384, opt);
        auto inst = lynx::makeInstrument (lynx::InstrumentKind::autogun);
        inst->configure (c, 120.0);
        inst->prepare (44100.0);
        std::vector<float> l (16384), r (16384);
        for (int s = 0; s < 16384; s += 512)
        {
            if (s == 0)
                inst->noteOn (60, 0.75f);
            if (s == 6144)
                inst->noteOff (60);
            inst->render (l.data() + s, r.data() + s, 512, 1.0f, 1.0f);
        }
        double worst = 0.0, peak = 0.0;
        for (int i = 0; i < 16384; ++i)
            worst = std::max ({ worst, (double) std::abs (ref[2 * (std::size_t) i] - l[(std::size_t) i]), (double) std::abs (ref[2 * (std::size_t) i + 1] - r[(std::size_t) i]) }),
            peak = std::max (peak, (double) std::abs (l[(std::size_t) i]));
        if (worst != 0.0)
            std::printf ("  Autogun instrument vs reference: worst %.3g (peak %.3g)\n", worst, peak);
        CHECK (ref.size() == 2 * 16384 && peak > 0.01 && worst == 0.0);
        std::vector<float> tail (44100), tailR (tail.size());
        for (int k = 0; k < 40 && ! inst->idle(); ++k)
            inst->render (tail.data(), tailR.data(), (int) tail.size(), 1.0f, 1.0f);
        CHECK (inst->idle());
    }

    // the state variants: Ogun's parser rejects the version-19 Autogun layout, the comment length is one byte or 0xff + i32
    std::ifstream rt (std::filesystem::path (realFlpDir) / "System" / "Render tests" / "Autogun.flp", std::ios::binary);
    const std::vector<std::uint8_t> rtBytes ((std::istreambuf_iterator<char> (rt)), std::istreambuf_iterator<char>());
    for (const auto& c : flp::parseProject (rtBytes).channels)
        if (c.pluginName == "Autogun")
            CHECK (lynx::autogunStateUsable (c.pluginState) && lynx::ogunParam (c.pluginState, 63) == 430);
}

TEST (harmless_rack)
{
    // REA_rodada36: Harmless's effects rack against our reference model (rack.py / maxmodel.py, which match the
    // installation's Harmless_x64.dll and SG_x64.dll), 44.1 kHz, host blocks of 512
    std::vector<float> inL (8192), inR (8192);
    for (int i = 0; i < 4096; ++i)
    {
        inL[(std::size_t) i] = (float) (1.5 * std::sin (2.0 * 3.141592653589793 * 440.0 * i / 44100.0) * std::min (1.0, i / 400.0));
        inR[(std::size_t) i] = (float) (1.2 * std::sin (2.0 * 3.141592653589793 * 660.0 * i / 44100.0));
    }
    auto run = [&] (const std::array<int, 100>& p, int compType, std::shared_ptr<const lynx::hlfx::MaximusPreset> comp, int total, int voicesUntil) {
        lynx::hlfx::Rack rack;
        std::array<int, 7> t {};
        t[3] = compType;
        rack.prepare (p, t, 44100.0, 120.0, std::move (comp));
        std::vector<float> l (inL.begin(), inL.begin() + total), r (inR.begin(), inR.begin() + total);
        for (int s = 0; s < total; s += 512)
            rack.process (l.data() + s, r.data() + s, std::min (512, total - s), s < voicesUntil);
        return std::make_pair (l, r);
    };
    {
        // chorus type 3 (mix 400), ping-pong delay type 2 (vol 600, feedback 500, 1/4 beat), reverb type 4 (vol 700)
        std::array<int, 100> p {};
        p[90] = 3, p[91] = 400, p[92] = 2, p[93] = 600, p[94] = 500, p[95] = 48, p[96] = 4, p[97] = 700;
        const auto [l, r] = run (p, 0, nullptr, 8192, 4096);
        const std::tuple<int, float, float> golden[] { { 0, 0.0f, 0.0f }, { 500, -0.25248247f, 0.15141529f }, { 3000, -0.65128994f, -0.64854866f },
                                                       { 5000, -0.54308629f, -0.36827448f }, { 6000, -0.98378551f, -0.72790027f },
                                                       { 8191, 0.50599128f, -0.43637198f } };
        for (const auto& [i, gl, gr] : golden)
            CHECK (std::abs (l[(std::size_t) i] - gl) < 2e-6f && std::abs (r[(std::size_t) i] - gr) < 2e-6f);
    }
    {
        // the delay alone: an impulse comes back after round(time·fs·0.3125/bpm) samples × (11^0.6 − 1)/10, then × feedback
        std::array<int, 100> p {};
        p[92] = 1, p[93] = 600, p[94] = 500, p[95] = 48;
        lynx::hlfx::Rack rack;
        rack.prepare (p, {}, 44100.0, 120.0, nullptr);
        std::vector<float> l (16384), r (16384);
        l[0] = 1.0f;
        for (int s = 0; s < 16384; s += 512)
            rack.process (l.data() + s, r.data() + s, 512, s == 0);
        const auto D = 5512; // 48·44100·0.3125/120 = 5512.5 → 5512 (round half to even)
        const auto vol = (float) ((std::exp ((double) (600 * 0.001f) * (double) (float) std::log (11.0)) - 1.0) * (double) 0.1f);
        CHECK (std::abs (l[(std::size_t) D] - vol) < 1e-7f && std::abs (l[(std::size_t) D - 1]) < 1e-12f);
        // the feedback ramps from 0.5 toward 0.5: the second echo is vol·0.5
        CHECK (std::abs (l[(std::size_t) (2 * D)] - vol * 0.5f) < 1e-7f && std::abs (r[(std::size_t) D]) < 1e-12f);
    }
    // the compressor: Maximus states of our own (no preset data), checked against SG_x64.dll by REA round 36
    using Env = std::vector<std::array<double, 4>>;
    const Env def { { 0.0, 0.0, 0.0, 0 }, { 0.5, 0.5, 0.0, 0 }, { 0.5, 0.5, 0.0, 0 } };
    auto band = [] (int mode, int pre = 1000, int post = 1000, int sep = 0, int sat = 0, int ceil = 1000, int att = 1115, int rel2 = 0,
                    int sus = 2882) { return std::array<int, 13> { mode, pre, post, sep, sat, ceil, att, rel2, 2, 5000, 3, sus, 0 }; };
    {
        // master brickwall at −0.22 dB only (bands off / compression off)
        const auto st = maximusTestState ({ 19800, 1, 48065, 1, 1115, 1000, -1 }, { band (3), band (1), band (3), band (0) },
                                          { def, def, def, def, def, def, def, def }, 0);
        const auto preset = lynx::hlfx::makeMaximusPreset (st);
        CHECK (preset != nullptr);
        std::array<int, 100> p {};
        p[98] = 1000;
        const auto [l, r] = run (p, 1, preset, 4096, 4096);
        // SG_x64.dll's own output (EXEC, REA_rodada40: bit-identical to the port)
        const std::tuple<int, float, float> golden[] { { 100, 0.024997942f, 0.88166064f }, { 1000, 0.56971401f, -0.62808102f },
                                                       { 2047, -0.27541628f, 0.709135f }, { 3000, 0.32431623f, -0.37985197f },
                                                       { 4095, -0.12743522f, -0.15237485f } };
        for (const auto& [i, gl, gr] : golden)
            CHECK (std::abs (l[(std::size_t) i] - gl) < 2e-6f && std::abs (r[(std::size_t) i] - gr) < 2e-6f);
        float peak = 0.0f;
        for (int i = 2048; i < 4096; ++i)
            peak = std::max ({ peak, std::abs (l[(std::size_t) i]), std::abs (r[(std::size_t) i]) });
        CHECK (peak < 0.9751f);
    }
    {
        // 24 dB crossovers, ×2 oversampling, low cut, stereo separation, saturation A, a knee with tension (type 7) and
        // exponential tensions (type 0), release 2 on the master, band mix 600
        const Env knee { { 0.0, 0.0, 0.0, 0 }, { 0.35, 0.35, 0.0, 0 }, { 0.65, 0.6, 0.36, 7 } };
        const Env t0 { { 0.0, 0.0, 0.0, 0 }, { 0.5, 0.5, 0.14, 0 }, { 0.5, 0.5, 0.0, 0 } };
        const Env m0 { { 0.0, 0.0, 0.0, 0 }, { 0.5, 0.5, 0.0, 0 }, { 0.5, 0.6, 0.085, 0 } };
        const auto st = maximusTestState ({ 19800, 1, 48065, 1, 891, 600, 32768 },
                                          { band (1, 1264, 1296, 500, -500, 1200), band (0, 1340, 1000, -348, 0, 1000, 1115, 0, 1570),
                                            band (0, 1364, 1152, 0, 0, 1000, 1115, 0, 1186), band (0, 1000, 1000, 0, -196, 1000, 1115, 4464, 1538) },
                                          { def, def, knee, def, t0, def, m0, def }, 1);
        const auto preset = lynx::hlfx::makeMaximusPreset (st);
        CHECK (preset != nullptr);
        std::array<int, 100> p {};
        p[98] = 600;
        const auto [l, r] = run (p, 2, preset, 4096, 4096);
        // SG_x64.dll's own output (EXEC, REA_rodada40; the port is within −142 dB of it)
        const std::tuple<int, float, float> golden[] { { 100, 0.0f, 0.0f }, { 1000, -0.46856543f, -0.63262129f },
                                                       { 2047, 0.20268451f, 0.54145539f }, { 3000, -0.25613472f, -0.49985155f },
                                                       { 4095, 0.10172445f, 0.050564945f } };
        for (const auto& [i, gl, gr] : golden)
            CHECK (std::abs (l[(std::size_t) i] - gl) < 2e-6f && std::abs (r[(std::size_t) i] - gr) < 2e-6f);
    }
    {
        // an .fst wrapper around a state is found; unsupported states are rejected
        const auto st = maximusTestState ({ 19800, 1, 48065, 1, 1115, 1000, -1 }, { band (3), band (1), band (3), band (0) },
                                          { def, def, def, def, def, def, def, def }, 0);
        std::vector<std::uint8_t> fst { 'F', 'L', 'h', 'd', 6, 0, 0, 0, 0, 0, 1, 0, 0x60, 0, 'F', 'L', 'd', 't' };
        std::vector<std::uint8_t> body { 0xC9, 8, 'M', 'a', 'x', 'i', 'm', 'u', 's', 0, 0xD5 };
        for (auto n = st.size(); ; n >>= 7)
        {
            body.push_back ((std::uint8_t) ((n & 0x7f) | (n >= 0x80 ? 0x80 : 0)));
            if (n < 0x80)
                break;
        }
        body.insert (body.end(), st.begin(), st.end());
        const auto len = (std::uint32_t) body.size();
        for (int k = 0; k < 4; ++k)
            fst.push_back ((std::uint8_t) (len >> (8 * k)));
        fst.insert (fst.end(), body.begin(), body.end());
        const auto found = lynx::hlfx::maximusStateFromFst (fst);
        CHECK (found && *found == st);
        auto rms = st;
        rms[4 + 4 * (7 + 13 * 3 + 12)] = 1; // RMS detector on the master (decoded in REA_rodada40)
        CHECK (lynx::hlfx::makeMaximusPreset (rms) != nullptr);
        auto masterMid = st;
        masterMid[0x1fc] = 1; // "Master Mid mode": not decoded
        CHECK (lynx::hlfx::makeMaximusPreset (masterMid) == nullptr);
    }
}

TEST (fruity_delay_bank_engine)
{
    // REA_rodada56: Fruity Delay Bank, two states of our own against the installation's DLL (goldens = the DLL's output
    // for the same input, EXEC; a new instance, FTZ/DAZ as the plugin's thread). Both are bit-identical.
    lynx::ScopedFlushDenormals ftz;
    struct Bank
    {
        std::array<int, 16> ctl;
        lynx::fldbank::BankOptions opt;
    };
    auto build = [] (std::array<int, 4> globals, std::map<int, Bank> banks, int oversampling) {
        auto s = lynx::fldbank::defaultState();
        for (int i = 0; i < 4; ++i)
            s.p[(std::size_t) i] = globals[(std::size_t) i];
        s.p[132] = oversampling;
        for (auto& [b, k] : banks)
        {
            for (int c = 0; c < 16; ++c)
                if (16 * b + 4 + c < lynx::fldbank::automatable)
                    s.p[(std::size_t) (16 * b + 4 + c)] = k.ctl[(std::size_t) c];
            s.bank[(std::size_t) b] = k.opt;
            s.bank[(std::size_t) b].enabled = true;
        }
        return s;
    };
    auto opt = [] (int f1, int o1, int f2, int o2, bool after, int mode, int steps, int amount, bool tap, bool sync) {
        lynx::fldbank::BankOptions o;
        o.filterType[0] = f1;
        o.filterOrder[0] = o1;
        o.filterType[1] = f2;
        o.filterOrder[1] = o2;
        o.filterAfter = after;
        o.mode = mode;
        o.steps = steps;
        o.amount = amount;
        o.tapBeforeFeedback = tap;
        o.sync = sync;
        return o;
    };
    auto run = [] (const lynx::fldbank::State& s, double rate, int block, double bpm) {
        std::vector<float> l (4096), r (4096);
        for (int i = 0; i < 4096; ++i)
        {
            l[(std::size_t) i] = (float) (1.5 * std::sin (2.0 * 3.141592653589793 * 440.0 * i / rate) * std::min (1.0, i / 400.0));
            r[(std::size_t) i] = (float) (1.2 * std::sin (2.0 * 3.141592653589793 * 660.0 * i / rate));
        }
        lynx::fldbank::Engine e (s, rate, bpm, block);
        for (int i = 0; i < 4096; i += block)
            e.process (l.data() + i, r.data() + i, std::min (block, 4096 - i));
        return std::make_pair (l, r);
    };
    auto check = [] (const std::pair<std::vector<float>, std::vector<float>>& out, const std::vector<std::tuple<int, float, float>>& golden) {
        for (const auto& [i, gl, gr] : golden)
            CHECK (out.first[(std::size_t) i] == gl && out.second[(std::size_t) i] == gr);
    };
    // controls: in pan, in vol, f1 cutoff, f1 res, f1 gain, fb pan, fb level, fb cross, time, offset, f2 cutoff, f2 res,
    // f2 gain, out pan, out vol, send. Three banks: ping-pong with low-pass ×2 / peaking feedback filter, pattern and send;
    // free time with a high shelf after the delay, band-pass feedback, negative feedback/cross, tap before the feedback;
    // swapped outputs with a 3-section high-pass in the loop and a negative pattern shape.
    const auto a = build ({ 100, 128, 120, 110 },
                          { { 0, { { -40, 140, 40000, 30000, 0, 30, 90, 50, 240, -200, 30000, 20000, 180, 20, 128, 100 }, opt (1, 1, 6, 0, false, 3, 4, 60, false, true) } },
                            { 1, { { 0, 128, 50000, 10000, 60, 0, -70, -40, 150, 150, 25000, 40000, 0, 0, -100, 0 }, opt (7, 0, 2, 0, true, 1, 0, 0, true, false) } },
                            { 2, { { 10, 110, 33145, 17500, 0, -60, 100, 0, 400, 0, 20000, 50000, 0, -30, 120, 0 }, opt (0, 0, 4, 2, false, 2, 3, -90, false, true) } } },
                          0);
    check (run (a, 44100.0, 512, 120.0), { { 100, -0.00294386433f, 0.0141299525f }, { 1000, -0.117360562f, -0.140237048f },
                                           { 2047, 0.172598198f, -0.401046395f }, { 3000, 0.303020209f, -0.127763838f },
                                           { 4095, 0.0093755126f, 0.241272509f } });
    // 2× oversampling at 48 kHz in blocks of 300, low shelf before the delay, 2-section notch in the loop, a send into a
    // ping-pong bank 3 (bank 1 and 2 off: no send) with a 2-step pattern
    const auto b = build ({ 0, 128, 128, 128 },
                          { { 0, { { 0, 128, 45000, 20000, 200, 0, 80, 0, 180, 60, 28000, 30000, 0, 0, 128, 64 }, opt (5, 0, 3, 1, false, 1, 0, 0, false, true) } },
                            { 3, { { 64, 128, 33145, 17500, 0, 0, -64, 90, 100, -90, 33145, 17500, 0, 0, 128, 0 }, opt (0, 0, 0, 0, false, 3, 2, 0, true, true) } } },
                          1);
    check (run (b, 48000.0, 300, 93.0), { { 100, 0.0f, 0.0f }, { 1000, 0.0f, 0.0f }, { 2047, 0.201803684f, -0.327834547f },
                                          { 3000, 0.412344277f, -0.101866335f }, { 4095, 0.204572573f, 0.71171385f } });

    // FL's state round-trips through the import (version 15 times are converted), the globals are the slot's parameters
    auto bytes = lynx::fldbank::serialise (a);
    const auto slot = lynx::effectFromFl ("Fruity Delay Bank", bytes);
    CHECK (slot && slot->kind == lynx::EffectKind::fruityDelayBank && slot->state == bytes);
    CHECK (slot && slot->params[0] == 100.0f / 128.0f && slot->params[3] == 110.0f / 128.0f);
    bytes[0] = 15;
    const auto v15 = lynx::fldbank::parse (bytes);
    CHECK (v15 && v15->p[12] == 2400 && v15->p[28] == 1500);
    CHECK (lynx::formatEffectParam (lynx::EffectKind::fruityDelayBank, 3, 0.5f, 120.0) == "50%");
    CHECK (lynx::formatEffectParam (lynx::EffectKind::fruityDelayBank, 0, 1.0f, 120.0) == "0.0 dB");
    CHECK (! lynx::fldbank::parse ({ 16, 0, 0, 0 }));
    // the LYNX effect (settled ramps, 512-frame engine blocks) runs from the slot
    auto fx = lynx::makeEffect (*slot);
    fx->prepare (44100.0);
    std::vector<float> l (1024, 0.5f), r (1024, -0.25f);
    fx->process (l.data(), r.data(), 1024, slot->params, 120.0);
    CHECK (std::isfinite (l[1023]) && std::isfinite (r[1023]));
}

TEST (fruity_multiband_engine)
{
    // REA_rodada56: Fruity Multiband Compressor, states of our own against the installation's DLL
    // (goldens = the DLL's output for the same input, EXEC; blocks of 512 at 44.1 kHz; all three bit-identical)
    std::vector<float> inL (4096), inR (4096);
    for (int i = 0; i < 4096; ++i)
    {
        inL[(std::size_t) i] = (float) (1.5 * std::sin (2.0 * 3.141592653589793 * 440.0 * i / 44100.0) * std::min (1.0, i / 400.0)
                                        + 0.5 * std::sin (2.0 * 3.141592653589793 * 90.0 * i / 44100.0));
        inR[(std::size_t) i] = (float) (1.2 * std::sin (2.0 * 3.141592653589793 * 660.0 * i / 44100.0)
                                        + 0.4 * std::sin (2.0 * 3.141592653589793 * 7000.0 * i / 44100.0));
    }
    auto multiband = [] (std::array<int, 28> c) {
        std::vector<std::uint8_t> b;
        auto put = [&b] (std::int32_t v) {
            std::uint8_t q[4];
            std::memcpy (q, &v, 4);
            b.insert (b.end(), q, q + 4);
        };
        put (0);
        for (auto v : c)
            put (v);
        put (0);  // UI words of the state (not used by the engine)
        put (50);
        return b;
    };
    struct Out { std::vector<float> l, r; int latency = 0; };
    auto run = [&] (const std::vector<std::uint8_t>& st) {
        const auto s = lynx::flmb::parse (st);
        CHECK (s.has_value());
        lynx::EffectParams p {};
        lynx::flmb::toParams (*s, p.data());
        auto fx = lynx::flmb::make();
        fx->prepare (44100.0);
        Out o { inL, inR };
        for (int s0 = 0; s0 < 4096; s0 += 512)
            fx->process (o.l.data() + s0, o.r.data() + s0, 512, p, 120.0);
        o.latency = fx->latencySamples();
        return o;
    };
    auto check = [] (const Out& out, const std::vector<std::tuple<int, float, float>>& golden) {
        for (const auto& [i, gl, gr] : golden)
            CHECK (std::abs (out.l[(std::size_t) i] - gl) < 2e-6f && std::abs (out.r[(std::size_t) i] - gr) < 2e-6f);
    };
    // IIR crossovers, limiter on, high band compressing, mid bypassed, low compressing, +2 dB output
    const auto oa = run (multiband ({ 42598, 1, 0, 0, 15604, 25782, 53524, 32768, 11, 298, 51118, 2, 14994, 5037, 24736, 55755, 26870, 221, 1090, 49807, 0, 5037, 21845, 47074, 32768, 550, 1090, 40632 }));
    CHECK (oa.latency == 0);
    check (oa,
           { { 100, 0.3061366081237793f, 0.14554384350776672f }, { 1000, 0.4117552638053894f, 0.3191826045513153f }, { 2047, -0.46868836879730225f, -0.5676097869873047f }, { 3000, 0.24988797307014465f, -0.42287692427635193f }, { 4095, -0.022194014862179756f, 0.8433372378349304f } });
    // linear phase (FIR, 52 frames of latency), overlapping mid transitions, low band off, limiter off
    const auto ob = run (multiband ({ 32768, 0, 1, 0, 12000, 21845, 40000, 50000, 300, 5000, 30000, 0, 4000, 3600, 30000, 45875, 60000, 50, 500, 52429, 1, 6500, 21845, 45875, 32768, 100, 1000, 52429 }));
    CHECK (ob.latency == 52);
    check (ob,
           { { 100, 0.0176423080265522f, -0.22197140753269196f }, { 1000, -0.002046747598797083f, 0.021436547860503197f }, { 2047, -0.010928931646049023f, -0.08079832047224045f }, { 3000, 0.027733327820897102f, -0.026119394227862358f }, { 4095, 0.07469408959150314f, -0.08492052555084229f } });
    // linear phase, hard compression everywhere, limiter on, mid crossovers crossed (the FIR mid band is silent)
    const auto oc = run (multiband ({ 50000, 1, 1, 0, 17000, 30000, 30000, 65536, 1, 100, 0, 0, 3000, 9000, 30000, 20000, 65536, 1, 100, 65536, 0, 6500, 30000, 25000, 65536, 1, 100, 10000 }));
    CHECK (oc.latency == 52);
    check (oc,
           { { 100, 0.028349297121167183f, -0.02347063086926937f }, { 1000, -0.00018560001626610756f, 0.0079029630869627f }, { 2047, -0.00953709613531828f, -0.00774149876087904f }, { 3000, 0.019576910883188248f, 0.006520440336316824f }, { 4095, 0.03874344751238823f, -0.000356332486262545f } });

    // the slot's parameters carry FL's raw controls exactly; after the project file's 4-decimal rounding the
    // effect built with FL's state still uses the exact controls
    const auto st = multiband ({ 42598, 1, 1, 0, 15604, 25782, 53524, 32768, 11, 298, 51118, 0, 14994, 5037, 24736, 55755, 26870, 221,
                                 1090, 49807, 0, 5037, 21845, 47074, 32768, 550, 1090, 40632 });
    const auto parsed = lynx::flmb::parse (st);
    CHECK (parsed && parsed->c[4] == 15604);
    lynx::EffectParams p {};
    lynx::flmb::toParams (*parsed, p.data());
    for (int i = 0; i < lynx::flmb::controls; ++i)
        CHECK (lynx::flmb::raw (p[(std::size_t) i], lynx::flmb::controlMin[(std::size_t) i], lynx::flmb::controlMax[(std::size_t) i]) == parsed->c[(std::size_t) i]);
    lynx::EffectParams rounded = p;
    for (auto& v : rounded)
        v = std::round (v * 10000.0f) / 10000.0f;
    std::vector<float> l1 (inL), r1 (inR), l2 (inL), r2 (inR);
    auto exact = lynx::flmb::make();
    auto fromState = lynx::flmb::make (st);
    exact->prepare (44100.0);
    fromState->prepare (44100.0);
    for (int s0 = 0; s0 < 4096; s0 += 512)
    {
        exact->process (l1.data() + s0, r1.data() + s0, 512, p, 120.0);
        fromState->process (l2.data() + s0, r2.data() + s0, 512, rounded, 120.0);
    }
    CHECK (l1 == l2 && r1 == r2);
    CHECK (lynx::flmb::paramText (4, p[4]) == "6302 Hz");
    CHECK (lynx::flmb::paramText (3, 0.5f) == "off");
    // the FL import builds the exact effect with FL's state kept
    const auto slot = lynx::effectFromFl ("Fruity Multiband Compressor", st);
    CHECK (slot && slot->kind == lynx::EffectKind::fruityMultiband && slot->state == st && slot->params[4] == p[4]);
    // unknown versions are rejected
    CHECK (! lynx::flmb::parse ({ 1, 0, 0, 0 }));
}

// REA_rodada54: input of the stereo-effect goldens (the installation DLLs' output for the same input, EXEC)
static std::pair<std::vector<float>, std::vector<float>> stereoFxInput()
{
    std::vector<float> l (4096), r (4096);
    for (int i = 0; i < 4096; ++i)
    {
        l[(std::size_t) i] = (float) (1.5 * std::sin (2.0 * 3.141592653589793 * 440.0 * i / 44100.0) * std::min (1.0, i / 400.0));
        r[(std::size_t) i] = (float) (1.2 * std::sin (2.0 * 3.141592653589793 * 660.0 * i / 44100.0));
    }
    return { l, r };
}

static std::vector<std::uint8_t> i32State (std::initializer_list<int> words)
{
    std::vector<std::uint8_t> d;
    for (const auto w : words)
        for (int b = 0; b < 4; ++b)
            d.push_back ((std::uint8_t) ((std::uint32_t) w >> (8 * b)));
    return d;
}

static void checkStereoFx (lynx::EffectSlot slot, const std::vector<std::tuple<int, float, float>>& golden)
{
    auto [l, r] = stereoFxInput();
    auto fx = lynx::makeEffect (slot);
    fx->prepare (44100.0);
    for (int s = 0; s < 4096; s += 512)
        fx->process (l.data() + s, r.data() + s, 512, slot.params, 120.0);
    for (const auto& [i, gl, gr] : golden)
        CHECK (std::abs (l[(std::size_t) i] - gl) < 2e-6f && std::abs (r[(std::size_t) i] - gr) < 2e-6f);
}

TEST (stereo_shaper_engine)
{
    // Fruity Stereo Shaper (REA_rodada54 §2): matrix with cross terms, delay on R, all-pass on L, effects post / pre
    auto slot = lynx::effectFromFl ("Fruity Stereo Shaper", i32State ({ 0, -3050, 12800, 9000, 2000, 1408, -2496, 0, 1 }));
    CHECK (slot && slot->kind == lynx::EffectKind::stereoShaper);
    if (slot)
        checkStereoFx (*slot, { { 100, 0.0438031591f, 0.0f }, { 1000, 1.11109531f, -0.781641483f }, { 2047, -0.73191303f, 0.483852893f },
                                { 3000, 0.950346172f, -0.769737065f }, { 4095, 0.251555145f, 0.153306276f } });
    slot = lynx::effectFromFl ("Fruity Stereo Shaper", i32State ({ 0, 0, 12800, 12800, 0, -2656, 2184, 0, 0 }));
    CHECK (slot.has_value());
    if (slot)
        checkStereoFx (*slot, { { 100, 0.0f, -0.104282036f }, { 1000, 0.0f, 1.08441091f }, { 2047, 0.463494211f, -0.93754214f },
                                { 3000, -1.49942112f, 0.820726812f }, { 4095, -1.31784308f, -0.057449434f } });
    // controls round-trip through the slot parameters; the Patcher's difference bus is the same engine flagged
    const auto st = lynx::stereofx::parseStereoShaper (i32State ({ 0, -3050, 12800, 9000, 2000, 1408, -2496, 2, 1 }));
    CHECK (st.has_value());
    if (st)
    {
        lynx::EffectParams p {};
        lynx::stereofx::stereoShaperToParams (*st, p.data());
        const auto back = lynx::stereofx::stereoShaperFromParams (p.data());
        CHECK (back.rightToLeft == -3050 && back.left == 12800 && back.right == 9000 && back.leftToRight == 2000 && back.delay == 1408
               && back.phase == -2496 && back.post == 1);
        const auto head = lynx::patcherBranchHead ("Fruity Stereo Shaper", i32State ({ 0, -3050, 12800, 9000, 2000, 1408, -2496, 2, 1 }), 2);
        CHECK (head.valid && head.slot && head.slot->kind == lynx::EffectKind::stereoShaper && head.slot->params[7] == 1.0f);
    }
    CHECK (! lynx::stereofx::parseStereoShaper ({ 1, 2, 3 }));
}

TEST (frequency_splitter_engine)
{
    // Frequency Splitter (REA_rodada62): our own states through Frequency Splitter_x64.dll from its creation (gains
    // ramping up from 0, FFT response faded in), blocks of 512 at 44.1 kHz; every band sent to its own destination
    // (1 low, 2 mid, 3 high). IIR modes are bit-exact; the FFT mode's residual is the plugin's single-precision FFT.
    using Gold = std::vector<std::tuple<int, float, float>>;
    struct Case
    {
        std::vector<int> words;
        int mute, latency;
        std::array<Gold, 4> gold; // low, mid, high sends, summed output
    };
    const std::vector<Case> cases {
        { { 19800, 48065, 3, 3, 0, 1, 2, 45820, 0, 0, 0, 300, -600, 0, 1, 2, 3 }, 0, 0, { Gold { { 100, 0.0090178391f, 0.0807745308f }, { 1000, 0.0443177596f, 0.000449771265f }, { 2047, -0.0277729966f, -0.00895843748f }, { 3000, 0.0306171384f, -0.00373467989f }, { 4095, 0.00322337216f, 0.00886098202f } }, Gold { { 100, 0.121696919f, -0.131051332f }, { 1000, 1.03903234f, 0.0533506982f }, { 2047, -0.651134431f, -1.06304717f }, { 3000, 0.717791438f, -0.443113774f }, { 4095, 0.0754866973f, 1.05140531f } }, Gold { { 100, 1.72075161e-05f, -0.00040520617f }, { 1000, 0.000452454377f, 0.000117809883f }, { 2047, -0.000283558591f, -0.00234806887f }, { 3000, 0.000312690681f, -0.00097864517f }, { 4095, 3.28165661e-05f, 0.00232247845f } }, Gold { { 100, 0.100601904f, 0.0864751786f }, { 1000, 0.788743436f, 0.0366551876f }, { 2047, -0.494284719f, -0.730370402f }, { 3000, 0.544887185f, -0.304443926f }, { 4095, 0.0573106967f, 0.722373366f } } } }, // A
        { { 30000, 48065, 2, 3, 0, 0, 2, 45820, -900, 0, 0, 0, 0, 900, 1, 2, 3 }, 8, 0, { Gold { { 100, -0.06870462f, 0.0430903696f }, { 1000, -0.581374586f, -0.0143744675f }, { 2047, 0.450544685f, 0.249298006f }, { 3000, -0.47477895f, 0.101792879f }, { 4095, -0.220592439f, -0.244846866f } }, Gold { { 100, 0.0f, 0.0f }, { 1000, 0.0f, 0.0f }, { 2047, 0.0f, 0.0f }, { 3000, 0.0f, 0.0f }, { 4095, 0.0f, 0.0f } }, Gold { { 100, -0.0655785128f, 0.674460888f }, { 1000, -0.335665613f, -1.06249368f }, { 2047, 0.535273552f, 0.467255145f }, { 3000, -0.507586002f, -0.989780664f }, { 4095, -0.696575642f, 0.500667155f } }, Gold { { 100, -0.293243587f, 3.01594687f }, { 1000, -1.50097573f, -4.7510891f }, { 2047, 2.39354587f, 2.08939266f }, { 3000, -2.26973724f, -4.42593384f }, { 4095, -3.11482906f, 2.23879886f } } } }, // B
        { { 40000, 10000, 0, 7, 0, 1, 2, 45820, 0, 200, 0, 0, 0, 0, 1, 2, 3 }, 0, 0, { Gold { { 100, 0.0171653964f, 0.0719543025f }, { 1000, 0.260783046f, -0.0546497703f }, { 2047, -0.236395031f, -0.087237671f }, { 3000, 0.242068797f, -0.103661537f }, { 4095, 0.169910118f, 0.140984371f } }, Gold { { 100, 0.00201279344f, -1.5109539f }, { 1000, 0.290754616f, 1.94614983f }, { 2047, -1.13570976f, -1.65917897f }, { 3000, 1.00995052f, 1.43215704f }, { 4095, 1.99373484f, -0.085176453f } }, Gold { { 100, -0.000444336678f, -0.000979390694f }, { 1000, -5.45688827e-08f, 7.50747904e-06f }, { 2047, 3.00044505e-08f, -6.34059734e-06f }, { 3000, 6.5076847e-08f, 5.3382596e-06f }, { 4095, -2.39666864e-08f, -2.14470973e-07f } }, Gold { { 100, 0.0178572834f, -0.781959713f }, { 1000, 0.424914241f, 1.04396117f }, { 2047, -0.877504408f, -1.02385247f }, { 3000, 0.812187135f, 0.704798162f }, { 4095, 1.2953757f, 0.0929019526f } } } }, // C
        { { 15000, 45000, 1, 4, 0, 1, 2, 45820, 0, 0, 0, -1800, 0, 1800, 1, 2, 3 }, 2, 0, { Gold { { 100, -0.00273601874f, 0.0286290813f }, { 1000, 0.0418281667f, 0.0362898856f }, { 2047, -0.0745894462f, 0.00119503727f }, { 3000, 0.069945626f, 0.0419373475f }, { 4095, 0.103130348f, -0.0348644778f } }, Gold { { 100, 0.0f, 0.0f }, { 1000, 0.0f, 0.0f }, { 2047, 0.0f, 0.0f }, { 3000, 0.0f, 0.0f }, { 4095, 0.0f, 0.0f } }, Gold { { 100, 1.42701356e-05f, -0.000351782364f }, { 1000, 2.80272197e-05f, 0.000625129556f }, { 2047, -4.9891918e-05f, 2.06105178e-05f }, { 3000, 4.68442995e-05f, 0.000722356897f }, { 4095, 6.91349633e-05f, -0.000600588392f } }, Gold { { 100, 0.0475500524f, -0.553698778f }, { 1000, 0.508395135f, 0.997257531f }, { 2047, -0.906660676f, 0.0328744054f }, { 3000, 0.850252867f, 1.15249836f }, { 4095, 1.25358307f, -0.958129764f } } } }, // D
        { { 22000, 50000, 6, 5, 0, 1, 2, 45820, 0, 0, 0, 0, 0, 0, 1, 2, 3 }, 0, 0, { Gold { { 100, 3.86351439e-06f, 0.000115638592f }, { 1000, -0.00223262212f, -4.85793389e-05f }, { 2047, 0.000411115587f, -2.57531383e-06f }, { 3000, -0.000485932804f, 7.87844056e-06f }, { 4095, 0.000189639119f, -4.06740628e-06f } }, Gold { { 100, -0.00987921655f, -0.683375061f }, { 1000, -0.876032948f, 1.17169452f }, { 2047, 0.424173862f, -0.340972483f }, { 3000, -0.499300569f, 1.1738565f }, { 4095, 0.195038065f, -0.732422411f } }, Gold { { 100, -2.29351427e-09f, -9.91509751e-07f }, { 1000, 2.00191561e-08f, 1.13525857e-06f }, { 2047, 2.50544758e-07f, -4.20314734e-07f }, { 3000, 1.65429199e-07f, 1.16024421e-06f }, { 4095, -7.31824983e-08f, -5.04818445e-07f } }, Gold { { 100, -0.00987535529f, -0.683260441f }, { 1000, -0.87826556f, 1.17164707f }, { 2047, 0.424585223f, -0.340975463f }, { 3000, -0.499786317f, 1.17386556f }, { 4095, 0.195227623f, -0.732426941f } } } }, // E
        { { 25000, 48065, 3, 3, 1, 0, 1, 30000, 0, 0, 0, 0, 0, 0, 1, 2, 3 }, 0, 768, { Gold { { 100, 0.0f, 0.0f }, { 1000, 0.119619228f, 0.00655745994f }, { 2047, -0.258448333f, 0.0300254505f }, { 3000, 0.257131875f, 0.0218825918f }, { 4095, 0.243620604f, -0.0373107828f } }, Gold { { 100, 0.0f, 0.0f }, { 1000, 0.0f, 0.0f }, { 2047, 0.0f, 0.0f }, { 3000, 0.0f, 0.0f }, { 4095, 0.0f, 0.0f } }, Gold { { 100, 0.0f, 0.0f }, { 1000, 0.679369271f, 0.20265615f }, { 2047, -1.23796761f, 0.901739538f }, { 3000, 1.23174798f, 0.658331215f }, { 4095, 1.16627932f, -1.12146437f } }, Gold { { 100, 0.0f, 0.0f }, { 1000, 0.798988521f, 0.209213614f }, { 2047, -1.49641597f, 0.93176496f }, { 3000, 1.48887992f, 0.680213809f }, { 4095, 1.40989995f, -1.15877521f } } } }, // F
        { { 20000, 45000, 2, 6, 1, 1, 1, 10000, 0, 0, 0, 0, 0, 0, 1, 2, 3 }, 0, 768, { Gold { { 100, 0.0f, 0.0f }, { 1000, 0.0390846208f, 0.00320863165f }, { 2047, -0.0802573487f, 0.0139643671f }, { 3000, 0.0798483118f, 0.0101885898f }, { 4095, 0.0756546259f, -0.0173618793f } }, Gold { { 100, 0.0f, 0.0f }, { 1000, 0.75990212f, 0.206010818f }, { 2047, -1.41615629f, 0.91779983f }, { 3000, 1.40902948f, 0.670024157f }, { 4095, 1.33424342f, -1.14141202f } }, Gold { { 100, 0.0f, 0.0f }, { 1000, 1.7614849e-06f, -5.86210172e-06f }, { 2047, -2.34208687e-06f, 7.51287303e-07f }, { 3000, 2.19281083e-06f, 1.06230493e-06f }, { 4095, 1.8608597e-06f, -1.35364621e-06f } }, Gold { { 100, 0.0f, 0.0f }, { 1000, 0.798988521f, 0.2092136f }, { 2047, -1.49641597f, 0.93176496f }, { 3000, 1.48887992f, 0.680213809f }, { 4095, 1.40989995f, -1.15877521f } } } }, // G
    };
    auto [l, r] = stereoFxInput();
    for (const auto& c : cases)
    {
        auto words = c.words;
        words.insert (words.begin(), 4);
        for (const int w : { 0, 0, 0, c.mute })
            words.push_back (w);
        std::vector<std::uint8_t> st;
        for (const auto w : words)
            for (int b = 0; b < 4; ++b)
                st.push_back ((std::uint8_t) ((std::uint32_t) w >> (8 * b)));
        st.push_back (0);
        st.resize (st.size() + 24, 0);
        const auto s = lynx::fsplit::parseState (st);
        CHECK (s.has_value());
        if (! s)
            continue;
        lynx::fsplit::Engine e;
        for (int i = 0; i < lynx::fsplit::engineParams; ++i)
            e.setParameter (i, s->params[(std::size_t) i]);
        e.prepare (44100.0);
        e.reset (false);
        std::array<std::vector<float>, 8> o;
        for (auto& v : o)
            v.assign (4096, 0.0f);
        for (int b = 0; b < 4096; b += 512)
        {
            float* out[8];
            for (int k = 0; k < 8; ++k)
                out[k] = o[(std::size_t) k].data() + b;
            e.process (l.data() + b, r.data() + b, out, 512);
        }
        CHECK (e.latencySamples() == c.latency);
        const auto tol = c.latency > 0 ? 2e-5f : 2e-6f;
        for (int k = 0; k < 4; ++k)
            for (const auto& [i, gl, gr] : c.gold[(std::size_t) k])
                CHECK (std::abs (o[(std::size_t) (2 * k)][(std::size_t) i] - gl) <= tol * std::max (1.0f, std::abs (gl))
                       && std::abs (o[(std::size_t) (2 * k + 1)][(std::size_t) i] - gr) <= tol * std::max (1.0f, std::abs (gr)));

        // the mixer effect: the summed output (FL's main output) of an instance that has been running, and a
        // Patcher bus = the bands sent to it
        auto slot = lynx::effectFromFl ("Frequency Splitter", st);
        CHECK (slot && slot->kind == lynx::EffectKind::frequencySplitter);
        const auto head = lynx::patcherBranchHead ("Frequency Splitter", st, 3);
        CHECK (head.valid && head.slot && head.slot->params[lynx::fsplit::outputHigh] == 1.0f
               && head.slot->params[lynx::fsplit::outputLow] == 0.0f);
        if (slot && head.slot)
        {
            lynx::fsplit::Engine settled;
            for (int i = 0; i < lynx::fsplit::engineParams; ++i)
                settled.setParameter (i, s->params[(std::size_t) i]);
            settled.prepare (44100.0);
            settled.reset (true);
            std::vector<float> sl (4096), sr (4096), hl (4096), hr (4096);
            float* out[8] { nullptr, nullptr, nullptr, nullptr, hl.data(), hr.data(), sl.data(), sr.data() };
            settled.process (l.data(), r.data(), out, 4096);
            const lynx::EffectSlot* both[2] { &*slot, &*head.slot };
            for (const auto* p : both)
            {
                auto fx = lynx::makeEffect (*p);
                fx->prepare (44100.0);
                auto ml = l, mr = r;
                for (int b = 0; b < 4096; b += 300)
                    fx->process (ml.data() + b, mr.data() + b, std::min (300, 4096 - b), p->params, 120.0);
                const auto& wl = p == &*slot ? sl : hl;
                const auto& wr = p == &*slot ? sr : hr;
                CHECK (ml == wl && mr == wr);
                CHECK (fx->latencySamples() == c.latency);
            }
        }
    }
    CHECK (! lynx::patcherBranchHead ("Frequency Splitter", std::vector<std::uint8_t> (0x58, 0), 2).valid); // no band sent to bus 2
    CHECK (lynx::fsplit::paramText (2, 3.0f / 7.0f) == "24 dB/oct" && lynx::fsplit::paramText (11, 0.5f) == "0.0 dB");
    CHECK (lynx::fsplit::paramText (0, 0.0f) == "30.0 Hz" && lynx::fsplit::paramText (4, 1.0f) == "FFT");
}

TEST (stereo_enhancer_transient_spreader_engines)
{
    // REA_rodada54 §3–5: own states against the installation's DLLs (goldens = the DLL's output, EXEC)
    auto se = lynx::effectFromFl ("Fruity Stereo Enhancer", i32State ({ -18, 300, -54, -148, 1, 1 }));
    CHECK (se && se->kind == lynx::EffectKind::stereoEnhancer);
    if (se)
        checkStereoFx (*se, { { 100, 0.0f, 0.00309863733f }, { 1000, 0.0101111531f, -0.110266283f }, { 2047, -0.688752949f, -1.93998075f },
                              { 3000, -0.283846498f, -0.739818454f }, { 4095, 0.191147864f, 2.70425582f } });
    auto tpState = i32State ({ 1, 138, 116, 64, 90, 2, 0, 70, 100, 1 });
    tpState.push_back (5);
    for (const auto w : i32State ({ 0, 50 }))
        tpState.push_back (w);
    auto tp = lynx::effectFromFl ("Transient Processor", tpState);
    CHECK (tp && tp->kind == lynx::EffectKind::transientProcessor);
    if (tp)
        checkStereoFx (*tp, { { 100, 0.0568947345f, 0.124077775f }, { 1000, -0.326598823f, -0.193128616f }, { 2047, 0.612462878f, -0.0517415144f },
                              { 3000, -0.535302937f, -0.446303189f }, { 4095, -0.634670973f, 0.316044629f } });
    auto spState = i32State ({ 0x10003, 7861, 3215, 106000, 1, 1, 0 });
    spState.push_back (1);
    spState.push_back (1);
    auto sp = lynx::effectFromFl ("Spreader", spState);
    CHECK (sp && sp->kind == lynx::EffectKind::spreader);
    if (sp)
        checkStereoFx (*sp, { { 100, 0.11841587f, -0.0958916694f }, { 1000, 1.40852344f, 0.087452054f }, { 2047, 1.07267976f, -2.02493715f },
                              { 3000, -0.781082511f, 0.811192274f }, { 4095, -1.80673707f, 0.89967829f } });
    // unknown layouts keep the earlier fallback; FL automation maps one to one
    CHECK (! lynx::stereofx::parseSpreader (i32State ({ 0x10001, 1, 2, 3, 4, 5 })));
    CHECK (! lynx::stereofx::parseTransientProcessor (i32State ({ 2, 1, 2, 3, 4, 5, 6, 7, 8, 9 })));
    const auto map = lynx::effectParamFromFl ("Transient Processor", 6);
    CHECK (map && map->param == 6 && map->convert (0.25f) == 0.25f);
}

TEST (fruity_dynamics_engines)
{
    // REA_rodada48: Fruity Limiter / Fruity Compressor, states of our own against the installation's DLLs
    // (goldens = the DLL's output for the same input, EXEC; blocks of 512 at 44.1 kHz)
    std::vector<float> inL (4096), inR (4096), side (4096);
    for (int i = 0; i < 4096; ++i)
    {
        inL[(std::size_t) i] = (float) (1.5 * std::sin (2.0 * 3.141592653589793 * 440.0 * i / 44100.0) * std::min (1.0, i / 400.0));
        inR[(std::size_t) i] = (float) (1.2 * std::sin (2.0 * 3.141592653589793 * 660.0 * i / 44100.0));
        side[(std::size_t) i] = (float) (0.9 * std::sin (2.0 * 3.141592653589793 * 55.0 * i / 44100.0));
    }
    auto put = [] (std::vector<std::uint8_t>& b, std::int32_t v) {
        std::uint8_t q[4];
        std::memcpy (q, &v, 4);
        b.insert (b.end(), q, q + 4);
    };
    auto limiterState = [&] (std::array<int, 18> p, int k = 0, int exact = 0) {
        std::vector<std::uint8_t> b;
        put (b, 7);
        for (auto v : p)
            put (b, v);
        b.resize (0xA4, 0);
        put (b, k);
        b.push_back ((std::uint8_t) exact);
        return b;
    };
    auto compressorState = [&] (int version, std::array<int, 6> p, int flags, int k = 0) {
        std::vector<std::uint8_t> b;
        put (b, version);
        for (auto v : p)
            put (b, v);
        put (b, flags);
        if (version > 2)
            put (b, k);
        return b;
    };
    struct Out { std::vector<float> l, r; int latency = 0; };
    auto run = [&] (const lynx::EffectSlot& slot, bool sidechain) {
        auto fx = lynx::makeEffect (slot);
        fx->prepare (44100.0);
        Out o { inL, inR };
        for (int s = 0; s < 4096; s += 512)
        {
            if (sidechain)
                fx->setSidechain (side.data() + s, side.data() + s);
            fx->process (o.l.data() + s, o.r.data() + s, 512, slot.params, 120.0);
        }
        o.latency = fx->latencySamples();
        return o;
    };
    auto check = [] (const Out& out, const std::vector<std::tuple<int, float, float>>& golden) {
        for (const auto& [i, gl, gr] : golden)
            CHECK (std::abs (out.l[(std::size_t) i] - gl) < 2e-6f && std::abs (out.r[(std::size_t) i] - gr) < 2e-6f);
    };
    auto limiter = [] (const std::vector<std::uint8_t>& st) {
        auto slot = lynx::effectFromFl ("Fruity Limiter", st);
        CHECK (slot && slot->kind == lynx::EffectKind::fruityLimiter);
        return *slot;
    };
    // look-ahead limiter: +4 dB gain into a −2.5 dB ceiling, 2 ms attack (88 samples of latency); bit-identical
    const auto a = run (limiter (limiterState ({ 1300, 1000, 800, 1115, 3, 5000, 3, 2882, 1000, 0, 0, 0, 6666, 1, 650, 1000, 0, 4187 })), false);
    CHECK (a.latency == 88);
    check (a, { { 100, 0.0176312141f, 0.621841073f }, { 1000, 0.401142687f, -0.442239583f }, { 2047, -0.19392401f, 0.499310732f },
                { 3000, 0.22835508f, -0.267458469f }, { 4095, -0.089728713f, -0.107289009f } });
    // compressor section with a positive knee, 2 compressor curve stages and soft saturation (port within −143 dB)
    check (run (limiter (limiterState ({ 1000, 900, 1000, 0, 3, 5000, 3, 2882, 400, 600, 400, 1115, 6666, 2, 650, 1000, 0, 4187 })), false),
           { { 100, -0.00208989787f, 0.0100310864f }, { 1000, -0.0545219742f, -0.0651496574f }, { 2047, 0.177322432f, -0.230846375f },
             { 3000, -0.159136176f, -0.183688983f }, { 4095, -0.300143838f, 0.299418598f } });
    // sidechained compressor (side input 1) with a negative knee, noise gate and the exact-ceiling option
    check (run (limiter (limiterState ({ 1000, 1000, 1000, 0, 3, 3000, 5, 2882, 300, 1000, -500, 500, 5000, 1, 1000, 0, 250, 4187 }, 1, 1)), true),
           { { 100, -0.00186592329f, 0.00895605329f }, { 1000, -0.0375732854f, -0.0448972471f }, { 2047, 0.124697573f, -0.162336946f },
             { 3000, -0.111069582f, -0.128206298f }, { 4095, -0.218446419f, 0.21791859f } });

    auto compressor = [] (const std::vector<std::uint8_t>& st) {
        auto slot = lynx::effectFromFl ("Fruity Compressor", st);
        CHECK (slot && slot->kind == lynx::EffectKind::fruityCompressor);
        return *slot;
    };
    // Vintage type, look-ahead flag, side input 1: bit-identical
    const auto ca = run (compressor (compressorState (3, { -200, 40, 30, 50, 300, 2 }, 1, 1)), true);
    CHECK (ca.latency == 64);
    check (ca, { { 100, 0.0795554966f, -0.230549783f }, { 1000, 0.483903229f, 0.0241768137f }, { 2047, -0.596430957f, -0.441769004f },
                 { 3000, 0.597724319f, -0.18129006f }, { 4095, 0.607042372f, 0.435067475f } });
    // Soft/R, version 2 without the flag (the output is still 64 frames late, but not reported): bit-identical
    const auto cb = run (compressor (compressorState (2, { -150, 80, 0, 150, 100, 7 }, 0)), false);
    CHECK (cb.latency == 0);
    check (cb, { { 100, 0.0813739002f, -0.235819489f }, { 1000, 0.264188439f, 0.0131994039f }, { 2047, -0.260424435f, -0.192893147f },
                 { 3000, 0.251500547f, -0.0762802213f }, { 4095, 0.252972007f, 0.181305125f } });

    // the slot's parameters carry FL's raw controls exactly (and survive the 4-decimal project file)
    const auto st = limiterState ({ 1261, 995, 1303, 1115, 3, 5000, 3, 2882, 264, 244, -590, 1115, 6666, 1, 650, 1000, 0, 4187 }, 1);
    const auto parsed = lynx::fldyn::parseLimiter (st);
    CHECK (parsed && parsed->sidechain == 1 && ! parsed->exactCeiling);
    const auto slot = limiter (st);
    for (int i = 0; i < lynx::fldyn::limiterControls; ++i)
    {
        const auto rounded = std::round (slot.params[(std::size_t) i] * 10000.0f) / 10000.0f;
        CHECK (lynx::fldyn::raw (rounded, lynx::fldyn::limiterMin[(std::size_t) i], lynx::fldyn::limiterMax[(std::size_t) i]) == parsed->p[(std::size_t) i]);
    }
    CHECK (lynx::formatEffectParam (lynx::EffectKind::fruityLimiter, 3, slot.params[3], 120.0) == "2.0 ms");
    CHECK (lynx::formatEffectParam (lynx::EffectKind::fruityCompressor, 5, 2.0f / 7.0f, 120.0) == "Vintage");
    // unreadable states keep the old approximation
    CHECK (lynx::effectFromFl ("Fruity Limiter", { 7, 0, 0, 0 }).has_value() == false
           || lynx::effectFromFl ("Fruity Limiter", { 7, 0, 0, 0 })->kind != lynx::EffectKind::fruityLimiter);
    CHECK (! lynx::fldyn::parseCompressor ({ 4, 0, 0, 0 }));
}

TEST (maximus_engine)
{
    // REA_rodada40: the engine on its own (MaximusProcessor), states of our own against the installation's DLLs
    // (goldens = the DLL's output for the same input, EXEC), and the containers of the mixer effect and Vocodex
    std::vector<float> inL (4096), inR (4096);
    for (int i = 0; i < 4096; ++i)
    {
        inL[(std::size_t) i] = (float) (1.5 * std::sin (2.0 * 3.141592653589793 * 440.0 * i / 44100.0) * std::min (1.0, i / 400.0));
        inR[(std::size_t) i] = (float) (1.2 * std::sin (2.0 * 3.141592653589793 * 660.0 * i / 44100.0));
    }
    auto run = [&] (std::shared_ptr<const lynx::hlfx::MaximusPreset> preset, int mix) {
        lynx::hlfx::MaximusProcessor m;
        m.prepare (std::move (preset), 44100.0, mix);
        std::vector<float> l (inL), r (inR);
        for (int s = 0; s < 4096; s += 512)
            m.process (l.data() + s, r.data() + s, 512);
        return std::make_pair (l, r);
    };
    using Env = std::vector<std::array<double, 4>>;
    const Env def { { 0.0, 0.0, 0.0, 0 }, { 0.5, 0.5, 0.0, 0 }, { 0.5, 0.5, 0.0, 0 } };
    const Env knee { { 0.0, 0.0, 0.0, 0 }, { 0.35, 0.35, 0.0, 0 }, { 0.65, 0.6, 0.36, 7 } };
    const Env t0 { { 0.0, 0.0, 0.0, 0 }, { 0.5, 0.5, 0.14, 0 }, { 0.5, 0.5, 0.0, 0 } };
    const Env m0 { { 0.0, 0.0, 0.0, 0 }, { 0.5, 0.5, 0.0, 0 }, { 0.5, 0.6, 0.085, 0 } };
    auto band = [] (int mode, int pre = 1000, int post = 1000, int sep = 0, int sat = 0, int ceil = 1000, int att = 1115, int rel2 = 0,
                    int sus = 2882, int rms = 0) { return std::array<int, 13> { mode, pre, post, sep, sat, ceil, att, rel2, 2, 5000, 3, sus, rms }; };
    auto check = [] (const std::pair<std::vector<float>, std::vector<float>>& out, const std::vector<std::tuple<int, float, float>>& golden) {
        for (const auto& [i, gl, gr] : golden)
            CHECK (std::abs (out.first[(std::size_t) i] - gl) < 2e-6f && std::abs (out.second[(std::size_t) i] - gr) < 2e-6f);
    };
    // RMS detectors, saturation mode B, a 5 Hz low cut, 12 dB high split (SG_x64.dll: port within −152 dB)
    auto st3 = maximusTestState ({ 19800, 1, 48065, 0, 1115, 1000, 0 },
                                 { band (0, 1500, 1000, 0, 400, 900, 1115, 0, 2882, 1), band (0, 1340, 1000, 0, 0, 1000, 1115, 0, 1570, 1),
                                   band (1, 1000, 1200, 0, 1000, 1000), band (0, 1000, 1000, 0, 250, 1000, 1115, 4464, 2882, 1) },
                                 { def, def, knee, def, t0, def, m0, def }, 0);
    const auto p3 = lynx::hlfx::makeMaximusPreset (st3);
    CHECK (p3 != nullptr);
    check (run (p3, 1000), { { 100, 0.0f, 0.0f }, { 1000, 0.87061065f, 0.64004499f }, { 2047, -0.97266865f, 0.60940075f },
                             { 3000, 0.96658099f, 0.9250561f }, { 4095, 0.99094611f, -0.964059f } });
    // the same with solo on (UI byte 0x1EF) and the low band selected (0x1E0): bit-identical to SG_x64.dll
    auto solo = st3;
    solo[0x1ef] = 1;
    std::memset (solo.data() + 0x1e0, 0, 4);
    check (run (lynx::hlfx::makeMaximusPreset (solo), 1000),
           { { 100, 3.2922967e-06f, 0.00036916698f }, { 1000, 0.15653539f, -0.047913823f }, { 2047, -0.17336531f, 0.019099623f },
             { 3000, 0.17185682f, -0.017173048f }, { 4095, 0.16714551f, 0.019473195f } });
    // the legacy linear-phase FIR band split (UI byte 0x1FD; SG_x64.dll: port within −148 dB)
    auto fir = st3;
    fir[0x1fd] = 1;
    check (run (lynx::hlfx::makeMaximusPreset (fir), 1000),
           { { 100, 0.0f, 0.0f }, { 1000, 0.97986746f, 0.18555209f }, { 2047, -0.98930448f, -0.95970845f },
             { 3000, 0.98856646f, -0.44592777f }, { 4095, 0.99069679f, 0.94382554f } });
    // Soundgoodizer_x64.dll's own build with a v4 file (its presets' layout): a copy of the round-36 state without
    // oversampling, through a byte-identical copy of the installation's DLL with this file as its preset (EXEC)
    const auto st2 = maximusTestState ({ 19800, 1, 48065, 1, 891, 600, 32768 },
                                       { band (1, 1264, 1296, 500, -500, 1200), band (0, 1340, 1000, -348, 0, 1000, 1115, 0, 1570),
                                         band (0, 1364, 1152, 0, 0, 1000, 1115, 0, 1186), band (0, 1000, 1000, 0, -196, 1000, 1115, 4464, 1538) },
                                       { def, def, knee, def, t0, def, m0, def }, 0);
    std::vector<std::uint8_t> v4 { 4, 0, 0, 0 };
    v4.insert (v4.end(), st2.begin() + 4, st2.begin() + 0x1f5);
    v4.insert (v4.end(), st2.begin() + 0x1fe, st2.end());
    const auto p4 = lynx::hlfx::makeMaximusPreset (v4, lynx::hlfx::MaximusBuild::soundgoodizer);
    CHECK (p4 != nullptr);
    check (run (p4, 600), { { 100, 0.0f, 0.0f }, { 1000, -0.60926896f, -0.6652832f }, { 2047, 0.40872875f, 0.36375371f },
                            { 3000, -0.41941205f, -0.61312866f }, { 4095, -0.12878418f, 0.27944946f } });
    CHECK (lynx::hlfx::makeMaximusPreset (st2, lynx::hlfx::MaximusBuild::soundgoodizer) != nullptr); // v7: also read by that build
    // the mixer effect's container and the effect itself
    lynx::maximusfx::Blob blob;
    blob.build = lynx::hlfx::MaximusBuild::soundgoodizer;
    blob.mix = 600;
    blob.state = v4;
    const auto packed = lynx::maximusfx::pack (blob);
    const auto back = lynx::maximusfx::unpack (packed);
    CHECK (back && back->build == blob.build && back->mix == 600 && back->state == v4);
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::maximus);
    slot.state = packed;
    auto fx = lynx::makeEffect (slot);
    fx->prepare (44100.0);
    {
        std::vector<float> l (inL), r (inR);
        for (int s = 0; s < 4096; s += 1024) // longer host blocks are split into the engine's 512-frame calls
            fx->process (l.data() + s, r.data() + s, 1024, slot.params, 120.0);
        const auto ref = run (p4, 600);
        CHECK (l == ref.first && r == ref.second);
    }
    const std::vector<std::uint8_t> sgState { 3, 0, 0, 0, 2, 0, 0, 0, 0x78, 2, 0, 0 };
    const auto sg = lynx::maximusfx::parseSoundgoodizer (sgState);
    CHECK (sg && sg->preset == 2 && sg->amount == 632 && std::string (lynx::maximusfx::soundgoodizerPresetFile (2)) == "c.dat");
    CHECK (lynx::soundgoodizerPresetPath (sgState) == "Plugins\\Fruity\\Effects\\Soundgoodizer\\Data\\Presets\\c.dat");
    CHECK (lynx::soundgoodizerExact (sgState, v4).has_value() && ! lynx::soundgoodizerExact (sgState, {}).has_value());
    // without the preset files (no FL installation) the approximation keeps the plugin's state and its name
    {
        const auto approx = lynx::effectFromFl ("Soundgoodizer", sgState);
        CHECK (approx && lynx::isSoundgoodizerApprox (*approx) && lynx::projectcontents::effectSlotName (*approx) == "Soundgoodizer");
        const auto st = approx ? lynx::maximusfx::parseSoundgoodizer (approx->state) : std::nullopt;
        CHECK (st && st->preset == 2 && st->amount == 632 && approx->branches.size() == 1 && approx->branches[0].chain.size() == 2);
        CHECK (! lynx::isSoundgoodizerApprox (lynx::makeEffectSlot (lynx::EffectKind::parallel))); // a Patcher container stays one
        const auto edited = lynx::soundgoodizerApproxSlot (0, 1000);
        CHECK (lynx::isSoundgoodizerApprox (edited) && lynx::maximusfx::parseSoundgoodizer (edited.state)->preset == 0);
    }
    // Vocodex's appended installation data survives a round trip and leaves FL's state untouched
    const std::vector<std::uint8_t> flState { 8, 0, 0, 0, 1, 2, 3 };
    lynx::vocodex::InstallData data;
    data.sgDat = st2;
    data.carrierState = { 9, 9, 9 };
    const auto joined = lynx::vocodex::appendInstallData (flState, data);
    const auto [raw, files] = lynx::vocodex::splitInstallData (joined);
    CHECK (raw == flState && files.sgDat == st2 && files.carrierState == data.carrierState);
    CHECK (lynx::vocodex::splitInstallData (flState).first == flState);
}

// ---- Fruity Reeverb 2 (REA_rodada49) --------------------------------------------------------------------------------
// Goldens: the installation's Fruity Reeverb 2_x64.dll (unmodified, rate set before the state, flushed, blocks of 512)
// on our own states and input; LYNX's engine is bit-identical to it.
TEST (reeverb2_engine)
{
    auto hex = [] (const char* h) {
        std::vector<std::uint8_t> v;
        for (std::size_t i = 0; h[i] != 0 && h[i + 1] != 0; i += 2)
            v.push_back ((std::uint8_t) std::stoi (std::string (h + i, 2), nullptr, 16));
        return v;
    };
    constexpr int N = 8192;
    std::vector<float> inL (N), inR (N);
    std::uint32_t seed = 12345;
    for (int i = 0; i < N; ++i)
    {
        seed = (seed * 1103515245u + 12345u) & 0x7fffffffu;
        const auto nz = (float) ((double) seed / 2147483648.0 - 0.5);
        const auto tri = (float) ((i % 200) - 100) / 100.0f;
        const float g = i < 4000 ? 1.0f : 0.0f;
        inL[(std::size_t) i] = 0.5f * tri * g + (i >= 100 && i < 300 ? nz * 0.6f : 0.0f);
        inR[(std::size_t) i] = 0.25f * tri * g;
    }
    struct Golden
    {
        int i;
        float l, r;
    };
    auto run = [&] (const std::vector<std::uint8_t>& state, double bpm, const std::vector<Golden>& gold) {
        const auto slot = lynx::reeverb2FromFl (state);
        CHECK (slot.has_value() && slot->kind == lynx::EffectKind::reeverb2);
        if (! slot)
            return;
        auto fx = lynx::makeEffect (*slot);
        fx->prepare (44100.0);
        fx->reset();
        std::vector<float> l (inL), r (inR);
        for (int s = 0; s < N; s += 512)
            fx->process (l.data() + s, r.data() + s, 512, slot->params, bpm);
        for (const auto& g : gold)
            CHECK (l[(std::size_t) g.i] == g.l && r[(std::size_t) g.i] == g.r);
    };
    // modulation, early reflections, negative separation, filters, predelay
    run (hex ("112700004b000000780000001e0000004600000055000000190000003c0000009600000090010000e2ffffff6e000000640000008c00000084030000b00400000000"),
         120.0, { { 777, 0.263778746f, 0.131889373f }, { 1555, 0.187972173f, 0.0587911084f }, { 3333, 0.10398978f, 0.0566705838f },
                  { 5000, 0.087177977f, -0.0197417438f }, { 8191, 0.0769432634f, -0.282023013f } });
    // Mid/Side input, tempo-synced predelay (27 ticks at 128 BPM), positive separation
    run (hex ("112700004b000000280000001b00000032000000640000000f0000002800000064000000f401000028000000800000000000000040000000ac020000000000000101"),
         128.0, { { 777, 0.38499999f, 0.192499995f }, { 1555, 0.275000006f, 0.137500003f }, { 3333, 0.164420277f, 0.0826604739f },
                  { 5000, -0.00861243438f, -0.00140450173f }, { 8191, -0.00873835851f, 0.00888271444f } });
    // version 10000 (no modulation controls), everything off or at its extreme, dry 0
    run (hex ("1027000013000000dd000000000000006400000000000000c8000000dd00000014000000d00700004000000000000000a0000000a00000000000"),
         120.0, { { 777, 0.0f, 0.0f }, { 1555, 0.0f, 0.0f }, { 3333, 0.0632086769f, 0.0632086769f }, { 5000, -0.108072646f, -0.108072646f },
                  { 8191, 0.144686908f, 0.144686908f } });

    // the parameters map back to FL's integers; unsupported states fall back to the approximation
    lynx::reeverb2::Settings s;
    s.p = { 3000, 5, 384, 1, 0, 200, 221, 20, 2000, -64, 160, 0, 160, 2048, 1 };
    s.tempoPredelay = true;
    lynx::EffectParams p {};
    lynx::reeverb2::toParams (s, p);
    CHECK (lynx::reeverb2::fromParams (p) == s);
    CHECK (! lynx::reeverb2FromFl ({ 0x11, 0x27, 0, 0, 1, 0 }).has_value());
    CHECK (! lynx::reeverb2FromFl ({ 0x12, 0x27, 0, 0 }).has_value());
    const auto approx = lynx::effectFromFl ("Fruity Reeverb 2", { 0x11, 0x27, 0, 0 }, 120.0);
    CHECK (! approx || approx->kind != lynx::EffectKind::reeverb2);
    CHECK (lynx::reeverb2ParamText (0, 0.0f) == "off" && lynx::reeverb2ParamText (5, 1.0f) == "20.0 s");

    // Fruity Reeverb (the first version): Fruity Reeverb_x64.dll goldens on our own states (denormals flushed, as the
    // plugin runs and as LYNX renders)
    lynx::ScopedFlushDenormals ftz;
    auto run1 = [&] (const std::vector<std::uint8_t>& state, const std::vector<Golden>& gold) {
        const auto slot = lynx::reeverb1FromFl (state);
        CHECK (slot.has_value() && slot->kind == lynx::EffectKind::reeverb1);
        if (! slot)
            return;
        auto fx = lynx::makeEffect (*slot);
        fx->prepare (44100.0);
        std::vector<float> l (inL), r (inR);
        for (int s = 0; s < N; s += 512)
            fx->process (l.data() + s, r.data() + s, 512, slot->params, 120.0);
        for (const auto& g : gold)
            CHECK (l[(std::size_t) g.i] == g.l && r[(std::size_t) g.i] == g.r);
    };
    run1 (hex ("01000000204e0000204e000025000000409c000050c300000a00000030750000a861000050c3000060ea0000"),
          { { 777, 0.293731689f, 0.146865845f }, { 1555, 0.20980835f, 0.104904175f }, { 3333, 0.0998796821f, 0.0754205957f },
            { 5000, -0.0994476378f, 0.0116700521f }, { 8191, -0.141061321f, 0.0601170398f } });
    run1 (hex ("0100000000000000000001000000000000000100000001002800000000000100000001000000000000000100"),
          { { 777, -0.016678432f, -4.58686594e-31f }, { 1555, 0.0221031085f, 0.0230635051f }, { 3333, 0.0875115693f, 0.1695389f },
            { 5000, -0.111421227f, -0.295867324f }, { 8191, 0.503498077f, 0.190456569f } });
    CHECK (lynx::reeverb1FromFl (std::vector<std::uint8_t> (44, 0)).has_value());
    CHECK (! lynx::reeverb1FromFl (std::vector<std::uint8_t> (40, 0)).has_value());
    CHECK (lynx::reeverb1ParamText (5, 0.0f) == "Brighter" && lynx::reeverb1ParamText (0, 0.0f) == "off");
}

// ---- Fruity Love Philter (REA_rodada66) ---------------------------------------------------------------------------------

TEST (love_philter_engine)
{
    // goldens: Fruity Love Philter_x64.dll on our own synthetic states (44.1 kHz, blocks of 512, 120 BPM, live mode),
    // bit-identical on the test machine (denormals flushed, as the plugin runs and as LYNX renders)
    lynx::ScopedFlushDenormals ftz;
    auto hex = [] (const char* h) {
        std::vector<std::uint8_t> v;
        for (std::size_t i = 0; h[i] != 0 && h[i + 1] != 0; i += 2)
            v.push_back ((std::uint8_t) std::stoi (std::string (h + i, 2), nullptr, 16));
        return v;
    };
    constexpr int N = 8192;
    std::vector<float> inL (N), inR (N);
    std::uint32_t seed = 12345;
    for (int i = 0; i < N; ++i)
    {
        seed = (seed * 1103515245u + 12345u) & 0x7fffffffu;
        const auto nz = (float) ((double) seed / 2147483648.0 - 0.5);
        const auto tri = (float) ((i % 200) - 100) / 100.0f;
        const float g = i < 4000 ? 1.0f : 0.0f;
        inL[(std::size_t) i] = 0.5f * tri * g + (i >= 100 && i < 300 ? nz * 0.6f : 0.0f);
        inR[(std::size_t) i] = 0.25f * tri * g;
    }
    struct Golden
    {
        int i;
        float l, r;
    };
    auto run = [&] (const std::vector<std::uint8_t>& state, const std::vector<Golden>& gold) {
        const auto slot = lynx::effectFromFl ("Fruity Love Philter", state, 120.0);
        CHECK (slot.has_value() && slot->kind == lynx::EffectKind::lovePhilter);
        if (! slot || slot->kind != lynx::EffectKind::lovePhilter)
            return;
        auto fx = lynx::makeEffect (*slot);
        fx->prepare (44100.0);
        std::vector<float> l (inL), r (inR);
        for (int s = 0; s < N; s += 512)
        {
            fx->setSongPosition (s * 2.0 * 960.0 / 44100.0);
            fx->process (l.data() + s, r.data() + s, 512, slot->params, 120.0);
        }
        for (const auto& g : gold)
            CHECK (l[(std::size_t) g.i] == g.l && r[(std::size_t) g.i] == g.r);
    };
    // SVF triple with Low/Band/High, a song-locked volume pattern with tension, a cutoff LFO; Lime HP fed by NEXT, panned
    run (hex (
             "03000c00789cedda316ed44018c5f1cfd90b5050234a0e81702a1a6e4045c11d68e7005c00c40928e96810254590380225e552538471ec"
             "d97c314c841e0259cfff91463bf66e7e3b9ac42fdf582e1151961e776abf1f314ce3b3a5d7f6e4fb8bf1b2b6693c5cf56118e6b79ed5fe"
             "b2f6b7b53fadfd41ed8f1379378d9fd77e88a50d57c3c3f597dcd28ee3fcfaadbe9e8fcb24e272d56238fd4019a73e2c5f3074c1ebf732"
             "745a8ee51507670bce99e89cbff9e5076eb9f8be8e378f9b7df3fcfae2dbba37afcb9c0b391b0eb16ec795ff657cfd6a6aefc7cf17178f"
             "f2f9f6f9fafe8769342c7dfabe58e65e62db7f5338fb74fe4f96acafa5e3f8bbf3eb6b75eb1e75068eb3d3b241b95ef2ffc0d67ebc8bb8"
             "b792b65e2ffc9bfa636a64068e9fc3de44f77a7b93adfc6e7170fec661afa17b64038eb34336e81ed980e3ec900dba4736e0383b6483ee"
             "957aee61ed9fe27a4d63194ffd63fdccf44cc7f972fef42408cf6ce0e06c30a7b6711f93fba238387fee50c3e81e75038eb343dda07b64"
             "038eb34336e81e7b0a1c67873d85ee910d38ce0ed9a07b64038eb34336e81ed980e3ec900dba57625ec3d65bcbc7799cd6b83f95ded478"
             "d60267c70ef745758f1a06c7d9a186d13dea061c6787ba41f7c8061c67876cd03df61438ce0e7b0add231b709c1db241f7c8061c67876c"
             "d03db201c7d9211b74afc4bc86adb7968ff338ad717f2abda9f1ac05ce8e1dee8bea1e350c8eb3430da37bd40d38ce0e7583ee910d38ce"
             "0ed9a07bec29709c1df614ba4736e0383b6483ee910d38ce0ed9a07b64038eb34336e85e89790d5b6f2d1fe7715ae3fe547a53e3590b9c"
             "1d3bdc17d53d6a181c67871a46f7a81b709c1dea06dd231b709c1db241f7d853e0383bec29748f6cc07176c806dd231b709c1db241f7c8"
             "061c67876cd0bd12f31ab6de5a3ecee3b4c6fda9f4a6c6b316383b76b82faa7bd43038ce0e358cee5137e0383bd40dba4736e0383b6483"
             "eeb1a7c07176d853e81ed980e3ec900dba4736e0383b6483ee910d38ce0ed9a07b25e6356cbdb57c9cc7698dfb53e94d8d672d7076ec70"
             "5f54f7a861709c1d6a18dda36ec07176a81b748f6cc07176c806dd634f81e3ecb0a7d03db201c7d9211b748f6cc07176c806dd231b709c"
             "1db241f74acc6bd87a6bf9388fd31af7a7d29b1acf5ae0ecd8e1bea8ee51c3e0383bd430ba47dd80e3ec5037e81ed980e3ec900dbac79e"
             "02c7d9614fa17b64038eb34336e81ed980e3ec900dba4736e0383b6483eec54ff7970e90"),
         { { 777, 0.710252046585083f, 0.19971221685409546f }, { 1555, 0.2758101224899292f, 0.07770585268735886f }, { 2222, -0.5605983734130859f, -0.14688193798065186f }, { 3333, -0.33108609914779663f, -0.07413796335458755f }, { 3999, 0.3015991151332855f, 0.07990600168704987f }, { 4100, 0.09223534911870956f, 0.021577393636107445f } });
    // Mango alternate triple with drive, waveshaper, Mod X/Y maps (smoothed), follower map on the volume; ×4 oversampling
    run (hex (
             "03000c00789cedda4d8ed33018c671a788151b8ec0013803993dd7e00e6c7d0a24100bae81c40590e00e5d82c46238c1e0c879db7752b9"
             "8b470c1f4ffe963c759af6371e4b79e675945a4aa96b2f4f5b7f56cad45e3e1d4a79be0c5a7bf9f3f57cd7da326e6fb7f3d3b49e5abef6"
             "a1f537adbf2ae7968f4f7c6b8f5b7fb20ca6e951fb19fd7a3bceefde2eedfbf2fa22bfdf5f6fe7fce9bb4d2bd3e9549d973eadbf742adb"
             "7686e25c86f2df51d7cfe0e0fc6de7203a37ef2fbe70ba200f17678ef3fde3b08f572fbe7fddebebd273816cc07173fe4c36dc0eaebdeb"
             "ff981fcabb2c28cedfeb85c4c70befeb97a5fd580b8dcfcbfba773dbdf433d81e3ec3c7466a46bec9e1d457e7e777bedfd1ff5c4d2c806"
             "1c3f87bd86eeb1d7c07176f6b8d7f85d1ed980e3ec900dba4736e0383b6483ee910d38ce0ed9a07bb5f4358c1e2d1fe7715ae3f1544653"
             "fbd65e6fe650b65349363985e3e7705f54f7a861709c1d6a18dda36ec07176a81b748f6cc07176c806dd634f81e3ecb0a7d03db201c7d9"
             "211b748f6cc07176c806dd231b709c1db241f76ae96b183d5a3ecee3b4c6e3a98ca6c6b316383b76b82faa7bd43038ce0e358cee5137e0"
             "383bd40dba4736e0383b6483eeb1a7c07176d853e81ed980e3ec900dba4736e0383b6483ee910d38ce0ed9a07bb5f4358c1e2d1fe7715a"
             "e3f1544653e3590b9c1d3bdc17d53d6a181c67871a46f7a81b709c1dea06dd231b709c1db241f7d853e0383bec29748f6cc07176c806dd"
             "231b709c1db241f7c8061c67876cd0bd5afa1a468f968ff338adf1782aa3a9f1ac05ce8e1dee8bea1e350c8eb3430da37bd40d38ce0e75"
             "83ee910d38ce0ed9a07bec29709c1df614ba4736e0383b6483ee910d38ce0ed9a07b64038eb34336e85e2d7d0da347cbc7799cd6783c95"
             "d1d478d60267c70ef745758f1a06c7d9a186d13dea061c6787ba41f7c8061c67876cd03df61438ce0e7b0add231b709c1db241f7c8061c"
             "67876cd03db201c7d9211b74af96be86d1a3e5e33c4e6b3c9eca686a3c6b81b36387fba2ba470d83e3ec50c3e81e75038eb343dda07b64"
             "038eb34336e81e7b0a1c67873d85ee910d38ce0ed9a07b64038eb34336e81ed980e3ec900dba574b5fc3e8d1f2711ea7351e4f6534359e"
             "b5c0d9b1c37d51dda386c17176a861748fba01c7d9a16ed03db201c7d9211b748f3d058eb3c39e42f7c8061c67876cd03db201c7d9211b"
             "748f6cc07176c806dd2bbf0040771f82"),
         { { 777, 0.30564039945602417f, 0.28222185373306274f }, { 1555, 0.262472540140152f, 0.23502512276172638f }, { 2222, 0.3336450159549713f, -0.030735457316040993f }, { 3333, 0.23046477138996124f, 0.20232810080051422f }, { 3999, 0.3282865881919861f, 0.300540566444397f }, { 4100, -1.5846843837152846e-07f, 2.583440448233887e-07f } });
    // band-pass alternate dual with FLAT, low shelf, all-pass with a pan LFO and inverted input, CENTER, no oversampling
    run (hex (
             "03000c00789ceddd418ad44018c5f12f3303baf404e2d243083d7b6fe0dab55b5dd64106bc8427f024aea497ed09da8a95225ff75883bc"
             "01092fff82a22b19fb97a2248faf62184b4494a5c7abdadf444cf3f866e9b5bdfff5f970ae2d9636cdad0de7af7da9fd6327623d5ffe32"
             "be5b7afdfe6dfdb85d2ff2443b1ddae7b17edecfe33f173f5fb598d6ab1fe63e2d179886e0fab30c5dcf1d07670bce8de8dc3f3cfac213"
             "37df8fc3e571b72fcf5fdf7c5bf7dabab45c201b70dc9cff930da7c1bd7712efe56d78d40d38ce0e7583ee910d38ce0ed9a07bec29709c"
             "1df614ba4736e0383b6483ee910d38ce0ed9a07b64038eb34336e85e89b686bdf7968ff338adf1782aa3a9f1ae05ce8e1d9e8bea1e350c"
             "8eb3430da37bd40d38ce0e7583ee910d38ce0ed9a07bec29709c1df614ba4736e0383b6483ee910d38ce0ed9a07b64038eb34336e85ea9"
             "e73ed4feaef64fe9cf95a5bf8ef67b2fca727ebeeecb79c0efb5c0c1d9604e6de33926cf457170fedda186d13dea061c6787ba41f7c806"
             "1c67876cd03df61438ce0e7b0add231b709c1db241f7c8061c67876cd03db201c7d9211b74afd473df6aff19eb9ac6322e69fc706eff05"
             "c9bc8e2fda603c95d1d478d70267c74ecfa9c737cc7170ef1e2ffce9cafffeb5fdec6db477a222785ebab5bf731c9ce738d436ba473d81"
             "e3ecf0efacba4736e0383b6483eeb1a7c07176d853e81ed980e3ec900dba4736e0383b6483ee910d38ce0ed9a07b25da1af6de5b3ecee3"
             "b4c6e3a98ca6c63b18383b76782eaa7bd43038ce0e358cee5137e0383bd40dba4736e0383b6483eeb1a7c07176d853e81ed980e3ec900d"
             "ba4736e0383b6483ee910d38ce0ed9a07b25da1af6de5b3ecee3b4c6e3a98ca6c6bb16383b76782eaa7bd43038ce0e358cee5137e0383b"
             "d40dba4736e0383b6483eeb1a7c07176d853e81ed980e3ec900dba4736e0383b6483ee910d38ce0ed9a07b25da1af6de5b3ecee3b4c6e3"
             "a98ca6c6bb16383b76782eaa7bd43038ce0e358cee5137e0383bd40dba4736e0383b6483eeb1a7c07176d853e81ed980e3ec900dba4736"
             "e0383b6483ee910d38ce0ed9a07b25da1af6de5b3ecee3b4c6e3a98ca6c6bb16383b76782eaa7bd43038ce0e358cee5137e0383bd40dba"
             "4736e0383b6483eeb1a7c07176d853e81ed980e3ec900dba4736e0383b6483ee910d38ce0ed9a07bf11b286e0820"),
         { { 777, 0.5369307994842529f, 0.483487993478775f }, { 1555, -0.17133042216300964f, -0.10717462003231049f }, { 2222, 0.8671267032623291f, 0.6469616889953613f }, { 3333, -0.6548674702644348f, -0.5142713189125061f }, { 3999, 1.4368892908096313f, 1.1726332902908325f }, { 4100, 1.1042776107788086f, 0.8842349648475647f } });

    // the slot exposes FL's controls; FL indices map both ways; unreadable states keep the old conversion
    CHECK (lynx::philter::flIndexOf (6) == 6 && lynx::philter::flIndexOf (8) == 9 && lynx::philter::flIndexOf (8 + 5 * 7 + 4) == 8 + 44 * 7 + 9);
    for (int i = 0; i < lynx::philter::paramCount; ++i)
        CHECK (lynx::philter::paramOfFlIndex (lynx::philter::flIndexOf (i)) == i);
    CHECK (lynx::philter::paramOfFlIndex (8 + 3) == -1 && lynx::philter::paramOfFlIndex (8 + 12) == -1);
    CHECK (lynx::philter::parse ({ 3, 0, 12, 0, 1, 2, 3, 4 }) == nullptr);
    CHECK (lynx::effectInfo (lynx::EffectKind::lovePhilter).params.size() == (std::size_t) lynx::philter::paramCount);
    CHECK (lynx::philter::paramText (5, 0.0f) == "pattern 1" && lynx::philter::paramText (8 + 3, 0.5f) == "centred");

    // a control edited in LYNX is applied when the effect is built from the slot (the state keeps FL's value)
    {
        const auto stateA = hex (
             "03000c00789cedda316ed44018c5f1cfd90b5050234a0e81702a1a6e4045c11d68e7005c00c40928e96810254590380225e552538471ec"
             "d97c314c841e0259cfff91463bf66e7e3b9ac42fdf582e1151961e776abf1f314ce3b3a5d7f6e4fb8bf1b2b6693c5cf56118e6b79ed5fe"
             "b2f6b7b53fadfd41ed8f1379378d9fd77e88a50d57c3c3f597dcd28ee3fcfaadbe9e8fcb24e272d56238fd4019a73e2c5f3074c1ebf732"
             "745a8ee51507670bce99e89cbff9e5076eb9f8be8e378f9b7df3fcfae2dbba37afcb9c0b391b0eb16ec795ff657cfd6a6aefc7cf17178f"
             "f2f9f6f9fafe8769342c7dfabe58e65e62db7f5338fb74fe4f96acafa5e3f8bbf3eb6b75eb1e75068eb3d3b241b95ef2ffc0d67ebc8bb8"
             "b792b65e2ffc9bfa636a64068e9fc3de44f77a7b93adfc6e7170fec661afa17b64038eb34336e81ed980e3ec900dba4736e0383b6483ee"
             "957aee61ed9fe27a4d63194ffd63fdccf44cc7f972fef42408cf6ce0e06c30a7b6711f93fba238387fee50c3e81e75038eb343dda07b64"
             "038eb34336e81e7b0a1c67873d85ee910d38ce0ed9a07b64038eb34336e81ed980e3ec900dba57625ec3d65bcbc7799cd6b83f95ded478"
             "d60267c70ef745758f1a06c7d9a186d13dea061c6787ba41f7c8061c67876cd03df61438ce0e7b0add231b709c1db241f7c8061c67876c"
             "d03db201c7d9211b74afc4bc86adb7968ff338ad717f2abda9f1ac05ce8e1dee8bea1e350c8eb3430da37bd40d38ce0e7583ee910d38ce"
             "0ed9a07bec29709c1df614ba4736e0383b6483ee910d38ce0ed9a07b64038eb34336e85e89790d5b6f2d1fe7715ae3fe547a53e3590b9c"
             "1d3bdc17d53d6a181c67871a46f7a81b709c1dea06dd231b709c1db241f7d853e0383bec29748f6cc07176c806dd231b709c1db241f7c8"
             "061c67876cd0bd12f31ab6de5a3ecee3b4c6fda9f4a6c6b316383b76b82faa7bd43038ce0e358cee5137e0383bd40dba4736e0383b6483"
             "eeb1a7c07176d853e81ed980e3ec900dba4736e0383b6483ee910d38ce0ed9a07b25e6356cbdb57c9cc7698dfb53e94d8d672d7076ec70"
             "5f54f7a861709c1d6a18dda36ec07176a81b748f6cc07176c806dd634f81e3ecb0a7d03db201c7d9211b748f6cc07176c806dd231b709c"
             "1db241f74acc6bd87a6bf9388fd31af7a7d29b1acf5ae0ecd8e1bea8ee51c3e0383bd430ba47dd80e3ec5037e81ed980e3ec900dbac79e"
             "02c7d9614fa17b64038eb34336e81ed980e3ec900dba4736e0383b6483eec54ff7970e90");
        auto slot = lynx::philter::slotFromFl (stateA);
        CHECK (slot.has_value() && ! lynx::philter::slotFromFl (hex ("0300000078")).has_value());
        if (slot)
        {
            auto render = [&] (const lynx::EffectSlot& sl) {
                auto fx = lynx::makeEffect (sl);
                fx->prepare (44100.0);
                std::vector<float> l (inL), r (inR);
                for (int s = 0; s < 4096; s += 512)
                {
                    fx->setSongPosition (s * 2.0 * 960.0 / 44100.0);
                    fx->process (l.data() + s, r.data() + s, 512, sl.params, 120.0);
                }
                return l[3333];
            };
            const auto base = render (*slot);
            auto edited = *slot;
            edited.params[8] = 0.1f; // unit 1 cutoff
            CHECK (base == 0.0f || base != render (edited));
            CHECK (base == render (*slot));
        }
    }
}

TEST (fruity_vocoder_round66_goldens)
{
    // REA_rodada66 §6 goldens against the round-70 engine (core/FruityVocoder), through FL's state import and the mixer
    // effect: Fruity Vocoder_x64.dll on our own states (44.1 kHz, blocks of 512): left = modulator, right = carrier.
    // The first state's attack (5 ms) is below the knob range (10…1000): the plugin uses it as it is (REA_rodada78 §1)
    lynx::ScopedFlushDenormals ftz;
    auto hex = [] (const char* h) {
        std::vector<std::uint8_t> v;
        for (std::size_t i = 0; h[i] != 0 && h[i + 1] != 0; i += 2)
            v.push_back ((std::uint8_t) std::stoi (std::string (h + i, 2), nullptr, 16));
        return v;
    };
    constexpr int N = 8192;
    std::vector<float> inL (N), inR (N);
    std::uint32_t seed = 12345;
    for (int i = 0; i < N; ++i)
    {
        seed = (seed * 1103515245u + 12345u) & 0x7fffffffu;
        const auto nz = (float) ((double) seed / 2147483648.0 - 0.5);
        const auto tri = (float) ((i % 200) - 100) / 100.0f;
        const float g = i < 4000 ? 1.0f : 0.0f;
        inL[(std::size_t) i] = 0.5f * tri * g + (i >= 100 && i < 300 ? nz * 0.6f : 0.0f);
        inR[(std::size_t) i] = 0.25f * tri * g;
    }
    struct Golden
    {
        int i;
        float v;
    };
    auto run = [&] (const std::vector<std::uint8_t>& state, const std::vector<Golden>& gold) {
        const auto slot = lynx::effectFromFl ("Fruity Vocoder", state, 120.0);
        CHECK (slot.has_value() && slot->kind == lynx::EffectKind::vocoder);
        if (! slot || slot->kind != lynx::EffectKind::vocoder)
            return;
        auto fx = lynx::makeEffect (*slot);
        fx->prepare (44100.0);
        std::vector<float> l (inL), r (inR);
        for (int s = 0; s < N; s += 512)
            fx->process (l.data() + s, r.data() + s, 512, slot->params, 120.0);
        for (const auto& g : gold)
            CHECK (l[(std::size_t) g.i] == g.v && r[(std::size_t) g.i] == g.v);
    };
    // 32 bands, order 3, inverted carrier bank with a formant shift, fast attack (5 ms, outside the knob range)
    run (hex (
             "02000000200000000300000002000000000000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000"
             "803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f00"
             "00803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803fb80b000050c30000640000000100000044fdffff"
             "78000000050000002c010000000000008c0000003c000000a0000000"),
         { { 777, 0.47870129346847534f }, { 1555, 0.2731437087059021f }, { 2222, -0.41599133610725403f }, { 3333, 0.083399198949337f }, { 3999, 0.7295553088188171f }, { 4100, 0.0871216207742691f } });
    // 8 bands, swapped inputs, per-band levels, modulator level 0
    run (hex (
             "02000000080000000200000002000000010000003f9a99193f3333333fcdcc4c3f6666663f0000803fcdcc8c3f9a99993f1027000060ea"
             "00001e000000000000002c010000500000000f0000003c00000000000000000000001400000080000000"),
         { { 777, 0.07233863323926926f }, { 1555, 0.1183714047074318f }, { 2222, -0.6131120920181274f }, { 3333, 0.18050849437713623f }, { 3999, 0.056527279317379f }, { 4100, 0.05711518973112106f } });
}

// ---- Fruity WaveShaper / Fruity Soft Clipper (REA_rodada51) ------------------------------------------------------------
// States written here from our own values (no FL preset data); goldens from the installation's DLLs (EXEC, blocks of 512)
namespace
{
std::vector<std::uint8_t> waveShaperTestState (int pre, int mix, int post, bool uni, bool center, int os, bool hq, bool smooth)
{
    std::vector<std::uint8_t> b;
    auto i32 = [&b] (std::int32_t v) { for (int k = 0; k < 4; ++k) b.push_back ((std::uint8_t) ((std::uint32_t) v >> (8 * k))); };
    auto f64 = [&b] (double v) { std::uint64_t u; std::memcpy (&u, &v, 8); for (int k = 0; k < 8; ++k) b.push_back ((std::uint8_t) (u >> (8 * k))); };
    auto f32 = [&b] (float v) { std::uint32_t u; std::memcpy (&u, &v, 4); for (int k = 0; k < 4; ++k) b.push_back ((std::uint8_t) (u >> (8 * k))); };
    i32 (0x40003);
    i32 (pre), i32 (mix), i32 (post);
    b.push_back (uni ? 1 : 0), b.push_back (center ? 1 : 0);
    i32 (os);
    const struct { double dt, y; float t; int mode; } pts[] = { { 0.0, 0.1, 0.0f, 0 }, { 0.3, 0.8, 0.4f, 0 }, { 0.25, 0.35, 0.0f, 10 },
                                                                 { 0.25, 0.9, -0.3f, 3 }, { 0.2, 1.0, 0.5f, 9 } };
    i32 (3), i32 (3), i32 (5);
    for (const auto& p : pts)
    {
        f64 (p.dt), f64 (p.y), f32 (p.t);
        b.push_back ((std::uint8_t) p.mode), b.push_back (0), b.push_back (0), b.push_back (0);
    }
    i32 (0), i32 (-1), i32 (-1), i32 (-1), i32 (-1);
    i32 (1);
    b.push_back (hq ? 1 : 0), b.push_back (smooth ? 1 : 0);
    return b;
}
} // namespace

TEST (fruity_waveshaper_and_soft_clipper)
{
    std::vector<float> inL (4096), inR (4096);
    for (int n = 0; n < 4096; ++n)
    {
        inL[(std::size_t) n] = (float) (((n * 37) % 200 - 100) / 60.0);
        inR[(std::size_t) n] = (float) (((n * 53) % 170 - 85) / 50.0);
    }
    struct Gold { int i; float l, r; };
    auto close = [] (float a, float b) { return std::abs (a - b) <= x87Tolerance() * std::max (1.0f, std::abs (b)); };

    // Soft Clipper: bit-exact against Fruity Soft Clipper_x64.dll
    auto clip = [&] (int thr, int post, std::initializer_list<Gold> gold) {
        std::vector<float> l (inL), r (inR);
        for (int s = 0; s < 4096; s += 512)
        {
            lynx::flshape::softClip (l.data() + s, 512, thr, post);
            lynx::flshape::softClip (r.data() + s, 512, thr, post);
        }
        for (const auto& g : gold)
            CHECK (l[(std::size_t) g.i] == g.l && r[(std::size_t) g.i] == g.r);
    };
    clip (100, 128, { { 100, 0.0f, -0.949052989f }, { 1000, -0.99617976f, 0.872887492f }, { 2047, 0.649999976f, -0.944175363f },
                      { 3000, -0.99617976f, -0.699999988f }, { 4095, 0.25f, 0.600000024f } });
    clip (1, 150, { { 100, 0.0f, -1.04588532f }, { 1000, -1.27003884f, 0.930835366f }, { 2047, 0.750244498f, -1.03539562f },
                    { 3000, -1.27003884f, -0.790091455f }, { 4095, 0.347655922f, 0.708338201f } });
    clip (64, 140, { { 100, 0.0f, -1.08492827f }, { 1000, -1.21535313f, 0.990323186f }, { 2047, 0.804165959f, -1.0770781f },
                     { 3000, -1.21535313f, -0.849189103f }, { 4095, 0.319320858f, 0.754407823f } });
    {
        const std::vector<std::uint8_t> st { 64, 0, 0, 0, 140, 0, 0, 0 };
        const auto slot = lynx::effectFromFl ("Fruity Soft Clipper", st, 120.0);
        CHECK (slot && slot->kind == lynx::EffectKind::fruitySoftClipper);
        if (slot)
        {
            const auto s = lynx::flshape::softClipperFromParams (slot->params.data());
            CHECK (s.threshold == 64 && s.post == 140);
            auto fx = lynx::makeEffect (*slot);
            fx->prepare (44100.0);
            std::vector<float> l (inL), r (inR);
            fx->process (l.data(), r.data(), 4096, slot->params, 120.0);
            CHECK (l[3000] == -1.21535313f && r[4095] == 0.754407823f);
        }
    }

    // WaveShaper: tables (bipolar / unipolar) and the engine on three of our states, live and as FL renders
    const auto stA = waveShaperTestState (150, 100, 120, false, true, 1, true, true);  // CENTER, ×2, HQ, smooth
    const auto stB = waveShaperTestState (100, 128, 128, true, false, 0, false, false); // unipolar
    const auto stC = waveShaperTestState (192, 90, 100, false, false, 2, false, false); // ×4
    const auto pA = lynx::flshape::parseWaveShaper (stA);
    CHECK (pA && pA->points.size() == 5 && pA->pre == 150 && pA->center && pA->hq && pA->smooth && pA->oversampling == 1);
    if (pA)
    {
        const auto T = lynx::flshape::shaperTable (pA->points, false);
        CHECK (T.size() == 16386 && T[1] == 3277 && T[1001] == 18824 && T[5001] == 26195 && T[9001] == 11469 && T[12001] == 24614
               && T[16384] == 32767 && T[0] == T[1]);
        const auto U = lynx::flshape::shaperTable (pA->points, true);
        CHECK (U.size() == 32770 && U[1] == -26214 && U[1001] == -6408 && U[5001] == 17073 && U[9001] == 19551 && U[12001] == 14549);
    }
    auto shape = [&] (const std::vector<std::uint8_t>& st, bool render, std::initializer_list<Gold> gold) {
        const auto s = lynx::flshape::parseWaveShaper (st);
        CHECK (s.has_value());
        if (! s)
            return;
        lynx::flshape::WaveShaperEngine e;
        e.setCurve (s->points);
        e.prepare (44100.0);
        e.setControls (s->pre, s->mix, s->post, s->oversampling, s->hq, s->smooth, s->center, s->unipolar, render);
        std::vector<float> l (inL), r (inR);
        for (int i = 0; i < 4096; i += 512)
            e.process (l.data() + i, r.data() + i, 512);
        for (const auto& g : gold)
            CHECK (close (l[(std::size_t) g.i], g.l) && close (r[(std::size_t) g.i], g.r));
    };
    shape (stA, false, { { 100, 0.59108448f, 0.469315082f }, { 1000, 0.284353405f, 1.02621937f }, { 2047, -0.533613563f, 0.117038473f },
                         { 3000, 0.22673659f, -1.13572407f }, { 4095, -0.267153352f, 0.822866499f } });
    shape (stA, true, { { 100, 0.794869065f, -0.127601087f }, { 1000, -0.00841622613f, 0.900729597f }, { 2047, 0.450603426f, -0.325943232f },
                        { 3000, -0.0530530401f, -0.32303831f }, { 4095, 1.0959363f, 0.946009099f } });
    shape (stB, false, { { 100, -0.206451416f, 0.502471924f }, { 1000, -0.799987793f, 0.761169434f }, { 2047, 0.411743164f, 0.514160156f },
                         { 3000, -0.799987793f, 0.596221924f }, { 4095, -0.157653809f, 0.334106445f } });
    shape (stB, true, { { 100, -0.206390381f, 0.502493739f }, { 1000, -0.799987793f, 0.761169434f }, { 2047, 0.411743164f, 0.514144361f },
                        { 3000, -0.799987793f, 0.596221924f }, { 4095, -0.157653809f, 0.334106445f } });
    shape (stC, false, { { 100, 0.317797363f, 0.0483714938f }, { 1000, -0.445489585f, -0.533142388f }, { 2047, 0.634525597f, 0.942175806f },
                         { 3000, -0.445489585f, 0.514707327f }, { 4095, 0.200391173f, -0.937523127f } });
    shape (stC, true, { { 100, 0.61165309f, -0.0993360579f }, { 1000, 0.0375693105f, 0.667849183f }, { 2047, 0.353716731f, -0.223758236f },
                        { 3000, 0.0375693105f, -0.223787636f }, { 4095, 0.843308687f, 0.688474894f } });

    // the mixer effect from the FL import: controls as parameters, the curve in the slot's state; host blocks of any size
    const auto slot = lynx::effectFromFl ("Fruity WaveShaper", stC, 120.0);
    CHECK (slot && slot->kind == lynx::EffectKind::fruityWaveShaper && slot->state == stC);
    if (slot)
    {
        auto fx = lynx::makeEffect (*slot);
        fx->prepare (44100.0);
        std::vector<float> l (inL), r (inR);
        fx->process (l.data(), r.data(), 1024, slot->params, 120.0);
        fx->process (l.data() + 1024, r.data() + 1024, 1024, slot->params, 120.0);
        fx->process (l.data() + 2048, r.data() + 2048, 2048, slot->params, 120.0);
        CHECK (close (l[1000], -0.445489585f) && close (r[2047], 0.942175806f) && close (l[4095], 0.200391173f));
        CHECK (lynx::formatEffectParam (lynx::EffectKind::fruityWaveShaper, 3, slot->params[3], 120.0) == "x4");
    }
    // an unreadable state keeps the old approximation
    CHECK (lynx::effectFromFl ("Fruity WaveShaper", { 1, 2, 3 }, 120.0)->kind == lynx::EffectKind::distortion);
    CHECK (lynx::effectFromFl ("Fruity Soft Clipper", { 1, 2, 3 }, 120.0)->kind == lynx::EffectKind::distortion);
}

// ---- Fruity Parametric EQ 2 / Fruity Parametric EQ (REA_rodada50) -----------------------------------------------------
// States written here from our own values (no FL preset data); goldens from the installation's DLLs (EXEC, blocks of 512)
namespace
{
struct PeqTestBand { int type, freq, width, gain, order; };
std::vector<std::uint8_t> peq2TestState (const PeqTestBand (&bands)[7], int main, int uiFlags, int options, int mask)
{
    std::vector<std::uint8_t> b;
    auto i32 = [&b] (std::int32_t v) { for (int k = 0; k < 4; ++k) b.push_back ((std::uint8_t) ((std::uint32_t) v >> (8 * k))); };
    i32 (8);
    for (int copy = 0; copy < 2; ++copy)
    {
        for (const auto& x : bands) i32 (x.gain);
        for (const auto& x : bands) i32 (x.freq);
        for (const auto& x : bands) i32 (x.width);
        for (const auto& x : bands) i32 (x.type);
        for (const auto& x : bands) i32 (x.order);
        i32 (main);
    }
    i32 (uiFlags), i32 (0), b.push_back (0), i32 (2), i32 (0), i32 (2), i32 (0);
    b.push_back ((std::uint8_t) options);
    i32 (mask), i32 (1), i32 (0);
    for (int v : { 3, 1, 2, 2, 2 }) i32 (v);
    i32 (0);
    return b;
}
std::vector<std::uint8_t> peq1TestState (const PeqTestBand (&bands)[7], int main)
{
    std::vector<std::uint8_t> b;
    auto i32 = [&b] (std::int32_t v) { for (int k = 0; k < 4; ++k) b.push_back ((std::uint8_t) ((std::uint32_t) v >> (8 * k))); };
    for (const auto& x : bands) i32 (x.gain);
    for (const auto& x : bands) i32 (x.freq);
    for (const auto& x : bands) i32 (x.width);
    for (const auto& x : bands) i32 (x.type);
    i32 (main);
    return b;
}
} // namespace

// EQUO test state (our own LCG curves, no FL data; REA_rodada61 equo/teststate.py builds the same bytes)
static std::vector<std::uint8_t> equoTestState (int ovs, int bw, bool add)
{
    std::vector<std::uint8_t> b;
    auto put = [&b] (const void* p, std::size_t n) { b.insert (b.end(), (const std::uint8_t*) p, (const std::uint8_t*) p + n); };
    auto putI = [&put] (std::int32_t v) { put (&v, 4); };
    auto putF = [&put] (float v) { put (&v, 4); };
    std::uint32_t seed = 12345;
    auto rnd = [&seed] {
        seed = (std::uint32_t) (((std::uint64_t) seed * 1103515245u + 12345u) & 0x7fffffffu);
        return (float) ((double) (seed % 20001) / 10000.0);
    };
    putI (4);
    for (int k = 0; k < 93; ++k)
    {
        putF (rnd());
        putI (k % 11 == 0 ? 1 : (k % 13 == 0 ? 2 : 0));
    }
    for (int bank = 0; bank < 9; ++bank)
        for (int k = 0; k < 93; ++k)
        {
            const float v = rnd();
            putF (bank == 8 ? 0.75f + v * 0.25f : v);
        }
    for (int v : { 1500, 90, 2600, 170, 150, ovs, bw, -1, 0, 0 })
        putI (v);
    for (int v : { 0, add ? 1 : 0, 0 })
        b.push_back ((std::uint8_t) v);
    putI (1);
    return b;
}

// Pitch Shifter state (version 7, our own settings; REA_rodada61 ps/teststate.py builds the same bytes)
static std::vector<std::uint8_t> pitchShifterTestState (std::array<int, 13> p, int engine, int detector)
{
    std::vector<std::uint8_t> b;
    auto putI = [&b] (std::int32_t v) { const auto* q = (const std::uint8_t*) &v; b.insert (b.end(), q, q + 4); };
    putI (7);
    for (int v : p)
        putI (v);
    putI (p[11]);
    b.push_back (0);
    putI (engine);
    putI (detector);
    putI (1000);
    b.push_back (1);
    b.push_back (0);
    return b;
}

TEST (pitch_shifter_exact)
{
    std::vector<float> inL (8192), inR (8192);
    for (int n = 0; n < 8192; ++n)
    {
        inL[(std::size_t) n] = (float) (((n * 37) % 200 - 100) / 60.0) * 0.3f;
        inR[(std::size_t) n] = (float) (((n * 53) % 170 - 85) / 50.0) * 0.3f;
    }
    struct Gold { int i; float l, r; };
    // goldens: Pitch Shifter_x64.dll on the same states and signal (REA_rodada61)
    auto run = [&] (const std::vector<std::uint8_t>& st, int latency, std::initializer_list<Gold> gold) {
        lynx::psfl::State s;
        CHECK (st.size() == 75 && lynx::psfl::parsePitchShifter (st.data(), st.size(), s));
        // through the mixer effect: FL import → slot parameters → effect, odd host blocks
        const auto slot = lynx::effectFromFl ("Pitch Shifter", st, 120.0);
        CHECK (slot && slot->kind == lynx::EffectKind::flPitchShifter);
        if (! slot)
            return;
        auto fx = lynx::makeEffect (*slot);
        fx->prepare (44100.0);
        std::vector<float> l (inL), r (inR);
        for (int i = 0, k = 0; i < 8192; ++k)
        {
            const int n = std::min (8192 - i, k % 2 == 0 ? 700 : 333);
            fx->process (l.data() + i, r.data() + i, n, slot->params, 120.0);
            i += n;
        }
        CHECK (fx->latencySamples() == latency);
        for (const auto& g : gold)
            CHECK (l[(std::size_t) g.i] == g.l && r[(std::size_t) g.i] == g.r);
    };
    // "FFT" (pitch-synchronous) engine: +5 st, formant −3 st, correction 50 %, mix 79 %, accurate detector
    run (pitchShifterTestState ({ 500, -300, 2, 1900, 8500, 200, 0, 0, 768, 0, 0, 32768, 0 }, 0, 1), 2457,
         { { 1000, 0.0f, 0.0f }, { 3000, 0.142123833f, 0.0878119618f }, { 5000, 0.139498621f, -0.365035176f },
           { 7000, 0.139498621f, 0.161137447f }, { 8191, 0.449655473f, 0.453882188f } });
    // granular engine: −7 st, 60 ms grains, density 300 %, jitter 30 %, random 20 %, post delay with feedback 40 %
    run (pitchShifterTestState ({ -700, 0, 2, 2400, 6000, 300, 30, 20, 200, 40, 1, 65536, 0 }, 1, 0), 0,
         { { 1000, -0.0736689344f, -0.349420458f }, { 3000, 0.086801365f, 0.0471927896f }, { 5000, -0.0939825475f, 0.31712544f },
           { 7000, -0.28462702f, -0.437854528f }, { 8191, 0.138916701f, -0.0426892526f } });
    CHECK (lynx::formatEffectParam (lynx::EffectKind::flPitchShifter, 0, 0.75f, 120.0) == "+6.00 st");
}

TEST (equo_exact)
{
    std::vector<float> inL (4096), inR (4096);
    for (int n = 0; n < 4096; ++n)
    {
        inL[(std::size_t) n] = (float) (((n * 37) % 200 - 100) / 60.0);
        inR[(std::size_t) n] = (float) (((n * 53) % 170 - 85) / 50.0);
    }
    struct Gold { int i; float l, r; };
    // goldens: EQUO_x64.dll run on the same states and signal (REA_rodada61 §5)
    auto run = [&] (const std::vector<std::uint8_t>& st, bool hq, bool ticks, std::initializer_list<Gold> gold) {
        const auto s = lynx::equofx::parse (st);
        CHECK (s.has_value());
        if (! s)
            return;
        lynx::equofx::Engine e;
        e.prepare (44100.0, hq);
        e.set (*s);
        std::vector<float> l (inL), r (inR);
        if (! ticks)
            e.process (l.data(), r.data(), 4096);
        else
        {
            auto k = *s;
            e.startFromConstruction();
            for (int i = 0; i < 4096; i += 256)
            {
                if (i == 2048)
                    k.knob[2] = 5300, e.setKnobs (k);
                e.tick();
                e.process (l.data() + i, r.data() + i, 256);
            }
        }
        for (const auto& g : gold)
            CHECK (l[(std::size_t) g.i] == g.l && r[(std::size_t) g.i] == g.r);
    };
    const auto plain = equoTestState (0, 150, false);
    run (plain, false, false, { { 100, -0.00154058414f, -0.00550224911f }, { 1000, -0.908149242f, 0.619277894f }, { 2047, 0.411961317f, -0.364051819f },
                                { 3000, -1.42823684f, -0.0602950938f }, { 4095, -0.0661701784f, 0.300809383f } });
    run (equoTestState (1, 300, true), false, false,
         { { 100, 0.0312003642f, 0.0184187777f }, { 1000, 0.0379521511f, 0.224126667f }, { 2047, -0.135689721f, 0.0444660783f },
           { 3000, 0.113949843f, -1.00256324f }, { 4095, -0.176232055f, 0.79159987f } });
    run (plain, true, false, { { 100, -0.00104017637f, 0.000872685807f }, { 1000, -0.0426855832f, 0.0402083024f }, { 2047, 0.191993102f, 0.174184501f },
                               { 3000, -0.114048228f, 0.419817507f }, { 4095, 0.0376945846f, -0.274040252f } });
    run (plain, false, true, { { 100, -0.0019897793f, -0.00981196482f }, { 1000, -1.09574401f, 0.83203429f }, { 2047, 0.839113712f, -0.691361725f } });

    // the mixer effect from the FL import: FL's state in the slot, the knobs in its parameters, any host block size
    const auto slot = lynx::effectFromFl ("EQUO", plain, 140.0);
    CHECK (slot && slot->kind == lynx::EffectKind::equo && slot->state == plain);
    if (slot)
    {
        auto back = *lynx::equofx::parse (plain);
        lynx::equofx::applyParams (slot->params, back);
        CHECK (back.knob == lynx::equofx::parse (plain)->knob && back.bandwidth == 150 && back.oversample == 0);
        auto fx = lynx::makeEffect (*slot);
        fx->prepare (44100.0);
        std::vector<float> l (inL), r (inR);
        fx->process (l.data(), r.data(), 700, slot->params, 0.0);
        fx->process (l.data() + 700, r.data() + 700, 3396, slot->params, 0.0);
        // the engine glides its bands per processing call, as FL does per host buffer: compare with the engine run on
        // the same 700 + 3396 split (sample 1000 differs from the single-call golden), and with the golden once settled
        lynx::equofx::Engine ref;
        ref.prepare (44100.0, false);
        ref.set (*lynx::equofx::parse (plain));
        std::vector<float> rl (inL), rr (inR);
        ref.process (rl.data(), rr.data(), 700);
        ref.process (rl.data() + 700, rr.data() + 700, 3396);
        CHECK (l == rl && rr == r && r[3000] == -0.0602950938f);
        CHECK (lynx::formatEffectParam (lynx::EffectKind::equo, 4, slot->params[4], 120.0, slot->params).find ("dB") != std::string::npos);
    }
    CHECK (! lynx::equofx::parse (std::vector<std::uint8_t> (100, 0)).has_value());
}

// Multiband Delay test state (version 7, our own LCG cells, no FL data; REA_rodada71 mbd/py/teststate.py builds the same bytes)
static std::vector<std::uint8_t> multibandDelayTestState (std::array<int, 12> globals, std::uint32_t seed)
{
    std::vector<std::uint8_t> b;
    auto putI = [&b] (std::int32_t v) { const auto* q = (const std::uint8_t*) &v; b.insert (b.end(), q, q + 4); };
    putI (7);
    putI (396);
    std::uint64_t s = seed;
    for (int i = 0; i < 384; ++i)
    {
        s = (s * 1103515245u + 12345u) & 0x7fffffffu;
        putI ((std::int32_t) ((s % 100001u) * (1u << 30) / 100000u));
    }
    for (int v : globals)
        putI (v);
    putI (0);
    putI (3);
    for (int v : { 1, 1, 0 })
        b.push_back ((std::uint8_t) v);
    b.insert (b.end(), 192, 0);
    return b;
}

TEST (multiband_delay_exact)
{
    std::vector<float> inL (8192), inR (8192);
    for (int n = 0; n < 8192; ++n)
    {
        inL[(std::size_t) n] = (float) (((n * 37) % 200 - 100) / 60.0 * 0.3);
        inR[(std::size_t) n] = (float) (((n * 53) % 170 - 85) / 50.0 * 0.3);
    }
    struct Gold { int i; float l, r; };
    // goldens: Multiband Delay_x64.dll (+ its engine_x64.dll) on the same states and signal, 512-frame blocks (REA_rodada71)
    auto run = [&] (const std::vector<std::uint8_t>& st, int latency, float tol, std::initializer_list<Gold> gold) {
        CHECK (st.size() == 1795 && lynx::mbdelay::parse (st).has_value());
        const auto slot = lynx::effectFromFl ("Multiband Delay", st, 120.0);
        CHECK (slot && slot->kind == lynx::EffectKind::multibandDelay && slot->state == st);
        if (! slot)
            return;
        auto fx = lynx::makeEffect (*slot);
        fx->prepare (44100.0);
        std::vector<float> l (inL), r (inR);
        for (int i = 0, k = 0; i < 8192; ++k)
        {
            const int n = std::min (8192 - i, k % 2 == 0 ? 700 : 333);
            fx->process (l.data() + i, r.data() + i, n, slot->params, 120.0);
            i += n;
        }
        CHECK (fx->latencySamples() == latency);
        for (const auto& g : gold)
            CHECK (std::fabs (l[(std::size_t) g.i] - g.l) <= tol && std::fabs (r[(std::size_t) g.i] - g.r) <= tol);
    };
    // steep crossover, keep pitch, weighted feedback, compressor
    run (multibandDelayTestState ({ 2000, 0, 3000, 600, 1, 1, 3500, 64, 0, 1, 30000, 0 }, 777), 0, 0.0f,
         { { 1000, -0.125012308f, 0.0675066486f }, { 3000, -0.125002861f, -0.0525011979f }, { 5000, -0.110439189f, 0.0597563162f },
           { 7000, -0.146480903f, 0.0139919929f }, { 8191, -0.0454496294f, 0.0398242511f } });
    // gentle crossover, harmonic scale, delay ×0.1, saturation
    run (multibandDelayTestState ({ -2500, 1, 4500, 0, 0, 0, 0, 128, 1, 0, 1000, 1 }, 4242), 0, 0.0f,
         { { 1000, 0.0679653957f, 0.0137896547f }, { 3000, 0.0620588325f, 0.0558352768f }, { 5000, 0.0558495969f, -0.0220566839f },
           { 7000, 0.057009533f, 0.00332171144f }, { 8191, 0.0673644915f, 0.00163267844f } });
    // linear-phase crossover (FL's IPP FFTs are approximated: about −120 dB), clipper
    run (multibandDelayTestState ({ 1500, 0, 2000, 1500, 2, 0, 1000, 100, 0, 0, 5000, 2 }, 99), 1536, 2e-6f,
         { { 1000, 0.0f, 0.0f }, { 3000, 0.103910387f, -0.0521105044f }, { 5000, 0.0516818762f, -0.0134486742f },
           { 7000, 0.065563485f, -0.00765838847f }, { 8191, -0.0376662463f, -0.0434602089f } });

    const auto st = multibandDelayTestState ({ 2000, 0, 3000, 600, 1, 1, 3500, 64, 0, 1, 30000, 0 }, 777);
    const auto s = *lynx::mbdelay::parse (st);
    CHECK (lynx::mbdelay::parse (lynx::mbdelay::serialize (s))->raw == s.raw);
    lynx::EffectParams p {};
    lynx::mbdelay::toParams (s, p);
    auto back = s;
    lynx::mbdelay::applyParams (p, back);
    CHECK (back.raw == s.raw);
    CHECK (lynx::mbdelay::engineValue (s, 384 + 7) == (float) (192.0 / 256.0));
    CHECK (lynx::formatEffectParam (lynx::EffectKind::multibandDelay, 4, p[4], 120.0) == "steep");
    CHECK (lynx::formatEffectParam (lynx::EffectKind::multibandDelay, 7, 1.0f, 120.0) == "100% wet");
    CHECK (! lynx::mbdelay::parse (std::vector<std::uint8_t> (16, 0)).has_value());
}

TEST (fruity_parametric_eq_exact)
{
    std::vector<float> inL (4096), inR (4096);
    for (int n = 0; n < 4096; ++n)
    {
        inL[(std::size_t) n] = (float) (((n * 37) % 200 - 100) / 60.0);
        inR[(std::size_t) n] = (float) (((n * 53) % 170 - 85) / 50.0);
    }
    struct Gold { int i; float l, r; };
    // high pass Steep 8, low shelf, steep peaking, a 0 dB peaking (0.2 s countdown), notch Gentle 4, high shelf Gentle 8,
    // low pass Gentle 6
    const PeqTestBand bands[7] = { { 3, 8000, 30000, 0, -3 }, { 5, 15000, 40124, 600, 0 }, { 6, 30000, 20000, -900, -2 },
                                   { 6, 35000, 25412, 0, 0 }, { 4, 40000, 10000, 0, 1 },  { 7, 50000, 30000, 450, 3 },
                                   { 1, 60000, 45000, 0, 2 } };
    const PeqTestBand bands1[7] = { { 3, 6000, 20000, 0, 0 }, { 5, 15000, 30000, 600, 0 }, { 6, 30000, 20000, -900, 0 },
                                    { 6, 35000, 25000, 0, 0 }, { 4, 40000, 10000, 0, 0 },  { 7, 50000, 30000, 450, 0 },
                                    { 1, 60000, 45000, 0, 0 } };
    auto run = [&] (const std::optional<lynx::peqfl::Settings>& s, std::initializer_list<Gold> gold) {
        CHECK (s.has_value());
        if (! s)
            return;
        lynx::peqfl::Engine e;
        e.set (*s);
        e.prepare (44100.0);
        std::vector<float> l (inL), r (inR);
        for (int i = 0; i < 4096; i += 512)
            e.process (l.data() + i, r.data() + i, 512);
        for (const auto& g : gold)
            CHECK (l[(std::size_t) g.i] == g.l && r[(std::size_t) g.i] == g.r);
    };
    const auto plain = peq2TestState (bands, 300, 0, 0x60, 0x7F);
    const auto hq = peq2TestState (bands, 300, 1, 0x60, 0x7F);
    const auto ext = peq2TestState (bands, 300, 1, 0x62, 0x7F);
    run (lynx::peqfl::parsePeq2 (plain), { { 100, 0.1340574324131012f, -0.3040016293525696f }, { 1000, 0.28402701020240784f, -0.23813000321388245f },
                                          { 2047, -0.9301530122756958f, 0.13881002366542816f }, { 3000, 0.2846762537956238f, 0.48893973231315613f },
                                          { 4095, -0.8481860756874084f, -0.2641225755214691f } });
    run (lynx::peqfl::parsePeq2 (hq), { { 100, 0.19229719042778015f, 0.29938483238220215f }, { 1000, 0.12543821334838867f, -0.055617764592170715f },
                                       { 2047, 1.0342068672180176f, 0.10901166498661041f }, { 3000, 0.12598788738250732f, -0.12920552492141724f },
                                       { 4095, 0.1619236022233963f, -0.23385579884052277f } });
    run (lynx::peqfl::parsePeq2 (ext), { { 100, 1.2178642749786377f, -0.21160493791103363f }, { 1000, -0.8292768001556396f, -0.23916664719581604f },
                                        { 2047, -0.7030635476112366f, -0.1978478729724884f }, { 3000, -0.8299900889396667f, 0.9836107492446899f },
                                        { 4095, 0.06754066050052643f, -0.34364908933639526f } });
    run (lynx::peqfl::parsePeq2 (peq2TestState (bands, -250, 1, 0x60, 0x5B)), // bands 3 and 6 muted
         { { 100, -0.02959616854786873f, 0.227821946144104f }, { 1000, 0.053224340081214905f, 0.06922465562820435f },
           { 2047, 0.3365536034107208f, -0.05899651721119881f }, { 3000, 0.053670305758714676f, -0.07452142238616943f },
           { 4095, -0.04406970739364624f, -0.12635089457035065f } });
    run (lynx::peqfl::parsePeq1 (peq1TestState (bands1, 150)),
         { { 100, -0.6533108353614807f, -1.6903921365737915f }, { 1000, -4.313726902008057f, 5.973916053771973f },
           { 2047, 1.0222549438476562f, 4.427900314331055f }, { 3000, -4.310323238372803f, 7.825916767120361f },
           { 4095, 8.344571113586426f, -0.49086111783981323f } });

    // "alternative design" (Frequency Splitter's matched biquads), bit-exact
    run (lynx::peqfl::parsePeq2 (peq2TestState (bands, 300, 3, 0x60, 0x7F)),
         { { 100, -0.790181577205658f, -0.08691816031932831f }, { 1000, 0.4960691034793854f, -0.24172282218933105f },
           { 2047, -0.42601829767227173f, 0.027932533994317055f }, { 3000, 0.49674150347709656f, 1.0799059867858887f },
           { 4095, -0.35040369629859924f, -0.28149813413619995f } });
    // linear phase (plain and HQ response): FFT convolution, 1536-sample latency, 0.1 s fade-in; FL's IPP FFT rounding
    // is not replicated, so within 1e-6 of the DLL
    for (const bool hq : { false, true })
    {
        const auto s = lynx::peqfl::parsePeq2 (peq2TestState (bands, hq ? -200 : 300, hq ? 1 : 0, 0x61, 0x7F));
        CHECK (s && s->linearPhase);
        if (! s)
            continue;
        std::vector<float> l (12288), r (12288);
        for (int n = 0; n < 12288; ++n)
        {
            l[(std::size_t) n] = (float) (((n * 37) % 200 - 100) / 60.0);
            r[(std::size_t) n] = (float) (((n * 53) % 170 - 85) / 50.0);
        }
        lynx::peqfl::Engine e;
        e.set (*s);
        e.prepare (44100.0);
        CHECK (e.latencySamples() == 1536);
        for (int i = 0; i < 12288; i += 512)
            e.process (l.data() + i, r.data() + i, 512);
        // (named arrays: a std::initializer_list chosen by ?: would point into a temporary array that dies at the end of
        // the declaration, which GCC 13 reuses for other locals)
        static const Gold goldHq[] = {
            { 2000, 0.08618364483118057f, 0.11248858273029327f }, { 5000, 0.18998730182647705f, 0.428600937128067f },
            { 8191, -0.1412065178155899f, -0.002996620489284396f }, { 10000, 0.18998737633228302f, -0.012570115737617016f },
            { 12287, 0.40586185455322266f, -0.014698396436870098f } };
        static const Gold goldPlain[] = {
            { 2000, 0.18538671731948853f, 0.206522598862648f }, { 5000, 0.4086928963661194f, 0.8167467713356018f },
            { 8191, -0.3218550980091095f, -0.06812914460897446f }, { 10000, 0.40869277715682983f, -0.08537150919437408f },
            { 12287, 0.775394082069397f, -0.08931504935026169f } };
        for (const auto& g : hq ? goldHq : goldPlain)
            CHECK (std::abs (l[(std::size_t) g.i] - g.l) < 1.0e-6f && std::abs (r[(std::size_t) g.i] - g.r) < 1.0e-6f);
    }

    // the mixer effect from the FL import: FL's values in the slot's parameters (exact round trip), any host block size
    for (const auto* name : { "Fruity Parametric EQ 2", "Fruity Parametric EQ" })
    {
        const bool v1 = std::string (name) == "Fruity Parametric EQ";
        const auto st = v1 ? peq1TestState (bands1, 150) : ext;
        const auto slot = lynx::effectFromFl (name, st, 120.0);
        CHECK (slot && slot->kind == lynx::EffectKind::parametricEq && lynx::peq::flLaw (slot->params));
        if (! slot)
            continue;
        const auto back = lynx::peqfl::fromParams (slot->params);
        const auto orig = v1 ? lynx::peqfl::parsePeq1 (st) : lynx::peqfl::parsePeq2 (st);
        CHECK (back.freq == orig->freq && back.gain == orig->gain && back.width == orig->width && back.type == orig->type
               && back.order == orig->order && back.main == orig->main && back.processing == orig->processing && back.mask == orig->mask);
        auto fx = lynx::makeEffect (*slot);
        fx->prepare (44100.0);
        CHECK (fx->latencySamples() == (v1 ? 0 : 44));
        std::vector<float> l (inL), r (inR);
        fx->process (l.data(), r.data(), 1000, slot->params, 120.0);
        fx->process (l.data() + 1000, r.data() + 1000, 3096, slot->params, 120.0);
        if (v1)
            CHECK (l[3000] == -4.310323238372803f && r[4095] == -0.49086111783981323f);
        else
            CHECK (l[1000] == -0.8292768001556396f && r[3000] == 0.9836107492446899f && l[4095] == 0.06754066050052643f);
        // editor texts and graph
        CHECK (lynx::formatEffectParam (lynx::EffectKind::parametricEq, lynx::peq::slopeParam, slot->params[lynx::peq::slopeParam], 120.0, slot->params)
               == (v1 ? "12 dB/oct" : "Steep 8"));
        CHECK (lynx::peq::widthText (slot->params, 0).rfind ("BW ", 0) == 0);
        const auto lowCutDb = lynx::peq::responseDb (slot->params, 44100.0, 25.0), midDb = lynx::peq::responseDb (slot->params, 44100.0, 1000.0);
        CHECK (lowCutDb < midDb - 5.0); // high pass below ~100 Hz (Steep 8 in the EQ 2 state, 12 dB/oct in v1)
    }
    // a native LYNX EQ is untouched by the FL law
    CHECK (! lynx::peq::flLaw (lynx::makeEffectSlot (lynx::EffectKind::parametricEq).params));
}

TEST (fruity_panomatic_balance_blood_overdrive)
{
    // REA_rodada58: goldens from the installation's DLLs (EXEC) on our own states, 44.1 kHz, blocks of 512
    const lynx::ScopedFlushDenormals ftz;
    std::vector<float> inL (4096), inR (4096);
    for (int n = 0; n < 4096; ++n)
    {
        inL[(std::size_t) n] = (float) (((n * 37) % 200 - 100) / 60.0);
        inR[(std::size_t) n] = (float) (((n * 53) % 170 - 85) / 50.0);
    }
    struct Gold { int i; float l, r; };
    auto close = [] (float a, float b) { return std::abs (a - b) <= x87Tolerance() * std::max (1.0f, std::abs (b)); };
    auto check = [&] (const std::vector<float>& l, const std::vector<float>& r, std::initializer_list<Gold> gold) {
        for (const auto& g : gold)
            CHECK (close (l[(std::size_t) g.i], g.l) && close (r[(std::size_t) g.i], g.r));
    };
    auto st32 = [] (std::initializer_list<int> v) {
        std::vector<std::uint8_t> b;
        for (int x : v)
            for (int k = 0; k < 4; ++k)
                b.push_back ((std::uint8_t) ((unsigned) x >> (8 * k)));
        return b;
    };

    // PanOMatic: the engine started as LYNX starts it (one silent frame, then FL's flush), and from silence (fade-in)
    auto pom = [&] (lynx::flbasic::PanOMaticState s, bool prime, std::initializer_list<Gold> gold) {
        lynx::flbasic::PanOMaticEngine e;
        e.prepare (44100);
        e.set (s);
        if (prime)
            e.prime();
        std::vector<float> l (inL), r (inR);
        for (int i = 0; i < 4096; i += 512)
            e.process (l.data() + i, r.data() + i, 512);
        check (l, r, gold);
    };
    pom ({ 40, 300, 2, 1, 128, 50000 }, true, { { 100, 0.0f, -1.44884729f }, { 1000, -1.78870034f, 0.778897762f }, { 2047, 0.697593153f, -1.26396084f },
                                                { 3000, -1.78870034f, -1.32851648f }, { 4095, 0.268305063f, 0.851258814f } });
    pom ({ 200, 180, 1, 2, 100, 30000 }, true, { { 100, 0.0f, -0.347919852f }, { 1000, -0.322590947f, 0.0788071603f }, { 2047, 0.126614109f, -0.26421836f },
                                                 { 3000, -0.326532423f, -0.435213327f }, { 4095, 0.0493027233f, 0.226084977f } });
    pom ({ 90, 256, 0, 1, 77, 40000 }, false, { { 100, -0.0292895138f, -0.110000029f }, { 1000, -1.31911683f, 0.666457176f }, { 2047, 0.477211952f, -0.864247262f },
                                                { 3000, -1.59167564f, -0.591367304f }, { 4095, 0.268362761f, 0.537029684f } });

    // Balance: flushed and from silence
    auto bal = [&] (lynx::flbasic::BalanceState s, bool flush, std::initializer_list<Gold> gold) {
        lynx::flbasic::BalanceEngine e;
        e.prepare (44100);
        e.set (s);
        if (flush)
            e.flush();
        std::vector<float> l (inL), r (inR);
        for (int i = 0; i < 4096; i += 512)
            e.process (l.data() + i, r.data() + i, 512);
        check (l, r, gold);
    };
    bal ({ 77, 100 }, true, { { 100, 0.0f, -0.136579692f }, { 1000, -0.0669578835f, -0.0282340273f }, { 2047, 0.0261135735f, -0.0795038193f },
                              { 3000, -0.0669578835f, -0.226895392f }, { 4095, 0.0100436825f, 0.0954951793f } });
    bal ({ -128, 320 }, false, { { 100, -0.28914085f, 0.0f }, { 1000, -1.03179514f, 0.0f }, { 2047, -0.578702569f, 0.0f },
                                 { 3000, -3.18510628f, 0.0f }, { 4095, 1.14394665f, 0.0f } });

    // Blood Overdrive: rate first, then the state (its smoothers settle for one second of samples)
    auto od = [&] (std::array<int, 6> raw, std::initializer_list<Gold> gold) {
        lynx::flbasic::BloodOverdriveEngine e;
        e.start (44100, { raw });
        std::vector<float> l (inL), r (inR);
        for (int i = 0; i < 4096; i += 512)
            e.process (l.data() + i, r.data() + i, 512);
        check (l, r, gold);
    };
    od ({ 3000, 7000, 5000, 1, 9000, 5000 }, { { 100, -0.0442250744f, 0.170355186f }, { 1000, 0.0470311977f, 0.0765217245f },
                                               { 2047, 0.0261161141f, -0.0341798998f }, { 3000, 0.0470311977f, 0.0213619489f },
                                               { 4095, 0.0402069278f, 0.0499244779f } });

    // the mixer effects from the FL import: controls as parameters; host blocks of any size
    auto viaImport = [&] (const char* plugin, const std::vector<std::uint8_t>& st, lynx::EffectKind kind, std::initializer_list<Gold> gold) {
        const auto slot = lynx::effectFromFl (plugin, st, 120.0);
        CHECK (slot && slot->kind == kind);
        if (! slot)
            return;
        auto fx = lynx::makeEffect (*slot);
        fx->prepare (44100.0);
        std::vector<float> l (inL), r (inR);
        fx->process (l.data(), r.data(), 1024, slot->params, 120.0);
        fx->process (l.data() + 1024, r.data() + 1024, 1024, slot->params, 120.0);
        fx->process (l.data() + 2048, r.data() + 2048, 2048, slot->params, 120.0);
        check (l, r, gold);
    };
    viaImport ("Fruity PanOMatic", st32 ({ 40, 300, 2, 1, 128, 50000 }), lynx::EffectKind::fruityPanOMatic,
               { { 1000, -1.78870034f, 0.778897762f }, { 4095, 0.268305063f, 0.851258814f } });
    viaImport ("Fruity Balance", st32 ({ 0, 256 }), lynx::EffectKind::fruityBalance,
               { { 100, 0.0f, -1.10000002f }, { 1000, -1.66666663f, 0.899999976f }, { 4095, 0.25f, 0.600000024f } });
    viaImport ("Fruity Blood Overdrive", st32 ({ 3, 0, 5000, 2578, 1, 0, 2911, 0, 0 }), lynx::EffectKind::fruityBloodOverdrive,
               { { 100, -0.122698173f, 0.0206716377f }, { 2047, 0.0128381588f, -0.0274834726f }, { 4095, -0.0899387598f, -0.121109024f } });
    {
        const auto slot = lynx::effectFromFl ("Fruity PanOMatic", st32 ({ 119, 256, 1, 2, 64, 27500 }), 120.0);
        CHECK (slot && lynx::flbasic::panOMaticFromParams (slot->params.data()).pan == 119
               && lynx::flbasic::panOMaticFromParams (slot->params.data()).target == 2);
        if (slot)
        {
            CHECK (lynx::formatEffectParam (lynx::EffectKind::fruityPanOMatic, 2, slot->params[2], 120.0) == "Triangle");
            CHECK (lynx::formatEffectParam (lynx::EffectKind::fruityPanOMatic, 1, slot->params[1], 120.0) == "+0.0 dB");
        }
    }
    // unreadable states keep the old approximations
    CHECK (lynx::effectFromFl ("Fruity PanOMatic", { 1, 2, 3 }, 120.0)->kind == lynx::EffectKind::utility);
    CHECK (lynx::effectFromFl ("Fruity Balance", { 1, 2, 3 }, 120.0)->kind == lynx::EffectKind::utility);
    CHECK (lynx::effectFromFl ("Fruity Blood Overdrive", { 1, 2, 3 }, 120.0)->kind == lynx::EffectKind::distortion);
}

TEST (fruity_fast_lp_free_filter_filter)
{
    // REA_rodada63: goldens from the installation's DLLs (EXEC) on our own states, 44.1 kHz, blocks of 512 (the wrapper
    // splits the host blocks below into calls of 512, as the DLL ran)
    const lynx::ScopedFlushDenormals ftz;
    std::vector<float> inL (4096), inR (4096);
    for (int n = 0; n < 4096; ++n)
    {
        inL[(std::size_t) n] = (float) (((n * 37) % 200 - 100) / 60.0);
        inR[(std::size_t) n] = (float) (((n * 53) % 170 - 85) / 50.0);
    }
    struct Gold { int i; float l, r; };
    auto state = [] (const char* hex) {
        std::vector<std::uint8_t> b;
        for (const char* p = hex; p[0] && p[1]; p += 2)
            b.push_back ((std::uint8_t) std::strtol (std::string (p, 2).c_str(), nullptr, 16));
        return b;
    };
    auto viaImport = [&] (const char* plugin, const char* hex, lynx::EffectKind kind, std::initializer_list<Gold> gold) {
        const auto slot = lynx::effectFromFl (plugin, state (hex), 120.0);
        CHECK (slot && slot->kind == kind);
        if (! slot)
            return;
        auto fx = lynx::makeEffect (*slot);
        fx->prepare (44100.0);
        std::vector<float> l (inL), r (inR);
        fx->process (l.data(), r.data(), 1024, slot->params, 120.0);
        fx->process (l.data() + 1024, r.data() + 1024, 1024, slot->params, 120.0);
        fx->process (l.data() + 2048, r.data() + 2048, 2048, slot->params, 120.0);
        for (const auto& g : gold)
            CHECK (l[(std::size_t) g.i] == g.l && r[(std::size_t) g.i] == g.r);
    };
    using K = lynx::EffectKind;
    viaImport ("Fruity Fast LP", "01000000e0010000bc020000", K::fruityFastLP,
               { { 100, -2.876206159591675f, 1.5413906574249268f }, { 1000, 0.12972253561019897f, 0.02936629205942154f },
                 { 2047, -0.2262123078107834f, -0.15169914066791534f }, { 3000, 0.12972258031368256f, -0.0798468142747879f },
                 { 4095, 0.0011161785805597901f, -0.08101501315832138f } });
    viaImport ("Fruity Fast LP", "01000000890000008a000000", K::fruityFastLP,
               { { 100, -0.7748895883560181f, -0.2310182899236679f }, { 1000, -0.01256702933460474f, -0.0086913350969553f },
                 { 2047, -0.010225297883152962f, -0.014700239524245262f }, { 3000, -0.012712989933788776f, -0.008916046470403671f },
                 { 4095, -0.008918046951293945f, -0.020280027762055397f } });
    viaImport ("Fruity Free Filter", "02000000c2010000200300002c0100008403000001", K::fruityFreeFilter, // low shelf, CENTER
               { { 100, -0.6178444623947144f, -0.13762974739074707f }, { 1000, -1.6557703018188477f, 0.8969771862030029f },
                 { 2047, 0.6539966464042664f, -0.21659433841705322f }, { 3000, -1.6404345035552979f, 0.4286947548389435f },
                 { 4095, -1.2272151708602905f, -0.5219485759735107f } });
    viaImport ("Fruity Free Filter", "020000008a02000058020000bc020000c800000000", K::fruityFreeFilter, // high shelf
               { { 100, -0.06996861100196838f, -0.11112016439437866f }, { 1000, -0.47636136412620544f, 0.31258222460746765f },
                 { 2047, 0.1628853678703308f, -0.3169582486152649f }, { 3000, -0.47636136412620544f, -0.22028093039989471f },
                 { 4095, -0.0975298061966896f, 0.05710025131702423f } });
    viaImport ("Fruity Free Filter", "020000006400000000040000a9000000a402000001", K::fruityFreeFilter, // low pass 16 kHz
               { { 100, 1.3032524585723877f, -0.007953938096761703f }, { 1000, 0.18662987649440765f, 2.155099391937256f },
                 { 2047, 0.5824014544487f, 0.05704556778073311f }, { 3000, 0.18981599807739258f, -0.6362011432647705f },
                 { 4095, 1.2624475955963135f, 2.497816324234009f } });
    viaImport ("Fruity Filter", "020000004b01000076000000000400002c010000580200000100000001", K::fruityFilter, // x2, CENTER
               { { 100, 0.2685631811618805f, 0.16885951161384583f }, { 1000, -0.20637311041355133f, 0.13557639718055725f },
                 { 2047, 0.07038643211126328f, -0.15532609820365906f }, { 3000, -0.20311756432056427f, -0.09958365559577942f },
                 { 4095, 0.08275680989027023f, 0.010792487300932407f } });
    viaImport ("Fruity Filter", "01000000bc020000f401000000000000000400000000000000000000", K::fruityFilter, // band pass
               { { 100, -0.4368843734264374f, -0.12492841482162476f }, { 1000, -0.3362128436565399f, -0.09686881303787231f },
                 { 2047, 0.21920710802078247f, 0.3213832974433899f }, { 3000, -0.3362128436565399f, 0.4016203284263611f },
                 { 4095, -0.14230486750602722f, -0.22842440009117126f } });
    // an old Free Filter layout (type·10) is converted to the current scale; parameter texts
    {
        const auto slot = lynx::effectFromFl ("Fruity Free Filter", state ("010000003c00000000020000a9000000a4020000"), 120.0);
        CHECK (slot && slot->kind == K::fruityFreeFilter);
        if (slot)
        {
            CHECK (lynx::flfilter::freeFilterFromParams (slot->params.data()).type == 100 + (int) std::nearbyint (5.5 / 7.0 * 600.0));
            CHECK (lynx::formatEffectParam (K::fruityFreeFilter, 0, slot->params[0], 120.0) == "Peaking");
        }
        CHECK (lynx::formatEffectParam (K::fruityFreeFilter, 1, 1.0f, 120.0) == "16000 Hz");
        CHECK (lynx::formatEffectParam (K::fruityFilter, 0, 1.0f, 120.0) == "7350 Hz");
    }
    // unreadable states keep the old approximations
    CHECK (lynx::effectFromFl ("Fruity Fast LP", { 1, 2, 3 }, 120.0)->kind == K::filter);
    CHECK (lynx::effectFromFl ("Fruity Free Filter", { 1, 2, 3 }, 120.0)->kind == K::filter);
    CHECK (lynx::effectFromFl ("Fruity Filter", { 9, 0, 0, 0 }, 120.0)->kind == K::filter);
}

// ---- Sytrus (REA_rodada31) -------------------------------------------------------------------------------------------
// A Sytrus state built from scratch (version 25 layout, our own values — no FL preset data), zlib "stored" blocks.
namespace
{
struct SytrusTestPatch
{
    std::vector<std::int32_t> main = std::vector<std::int32_t> (240, 0); // 962-byte main block as ints (+ mute bytes)
    struct Op
    {
        int p[10] { 0, 256, 0, 0, 0, 0, 0, 0, 0, 0 };
        int ratio = 20000, offset = 0, noise = 0;
        std::vector<float> amps { 1.0f };
        std::uint8_t flags[8] {};
        int env1[3] { -1, -1, -1 }; // volume envelope: on (≠ −1 → on), decay point, sustain point
        std::vector<std::pair<double, double>> env1pts;
    } ops[6];
    struct Filt
    {
        int p[10] { 0, 0, 128, 0, 128, 0, 0, 128, 128, 0 };
        int type = 3;
    } filts[3];
    std::array<std::array<std::uint8_t, 23>, 3> delays {};
    std::array<std::int32_t, 9> reverb { 19, 221, 0, 50, 80, 30, 221, 2, 0 };
    bool reverbOn = false;
    int eqmode = 0;

    SytrusTestPatch()
    {
        main[0] = 128, main[1] = 128;
        for (int i = 0; i < 8; ++i)
            main[(std::size_t) (165 + i)] = (i % 4 == 2) ? 0 : 128;
        main[173] = 1, main[175] = 128, main[188] = 128, main[189] = -1, main[190] = main[191] = 256;
    }
    void cell (int layer, int row, int col, int v) { main[(std::size_t) (3 + layer * 81 + row * 9 + col)] = v; }

    std::vector<std::uint8_t> blob() const
    {
        std::vector<std::uint8_t> b;
        auto i32 = [&] (std::int32_t v) { for (int k = 0; k < 4; ++k) b.push_back ((std::uint8_t) ((std::uint32_t) v >> (8 * k))); };
        auto f32 = [&] (float v) { std::int32_t x; std::memcpy (&x, &v, 4); i32 (x); };
        auto f64 = [&] (double v) { std::uint64_t x; std::memcpy (&x, &v, 8); for (int k = 0; k < 8; ++k) b.push_back ((std::uint8_t) (x >> (8 * k))); };
        // point lists: kind 1 envelope, 2 LFO, 3 map / shaper curve; format version 3
        auto env = [&] (int kind, const std::vector<std::pair<double, double>>& pts, int on, int decay, int sus) {
            i32 (kind), i32 (3), i32 ((std::int32_t) pts.size());
            for (const auto& [dt, y] : pts)
                f64 (dt), f64 (y), f32 (0.0f), i32 (0);
            i32 (0), i32 (on), i32 (decay), i32 (-1), i32 (sus);
        };
        for (int i = 0; i < 240; ++i)
            if (4 * i + 4 <= 0x320)
                i32 (main[(std::size_t) i]);
        b.resize (962, 0); // mute bytes: none muted
        i32 (0);                    // global declick amount
        b.push_back (0), b.push_back (0), b.push_back (0); // OSC grouping, DC stage, GIBBS OFF
        for (int k = 0; k < 9; ++k)
            b.push_back (k == 7 ? 1 : 0); // flag group (release multiplier on, as Default.fst)
        for (const auto& d : delays)
            b.insert (b.end(), d.begin(), d.end());
        for (auto v : reverb)
            i32 (v);
        b.push_back (reverbOn ? 1 : 0), b.push_back (0);
        i32 (eqmode);
        for (int k = 0; k < 3; ++k)
            i32 (0), i32 (30000), i32 (20000), i32 (k);
        b.push_back (0); // no info text
        for (int u = 0; u < 10; ++u)
        {
            const int narts = u < 6 ? 6 : (u < 9 ? 8 : 2);
            for (int j = 0; j < 10; ++j)
                i32 (u < 6 ? ops[u].p[j] : (u < 9 ? filts[u - 6].p[j] : 0));
            if (u < 6)
            {
                const auto& o = ops[u];
                const auto at = b.size();
                b.resize (at + 0x828, 0);
                auto put = [&] (std::size_t off, const void* src, std::size_t n) { std::memcpy (b.data() + at + off, src, n); };
                put (0, &o.ratio, 4), put (4, &o.offset, 4), put (8, &o.noise, 4);
                put (0xc, o.amps.data(), 4 * o.amps.size());
                const std::int32_t shape[5] { 0, 0, 0, 0, 128 };
                put (0x80c, shape, sizeof shape);
                put (0x820, o.flags, 8);
            }
            else if (u < 9)
            {
                const auto at = b.size();
                b.resize (at + 0x30, 0);
                std::memcpy (b.data() + at + 0x2c, &filts[u - 6].type, 4);
                env (3, { { 0.0, 0.0 }, { 1.0, 1.0 } }, 0, -1, -1);
                b.push_back (0);
            }
            for (int k = 0; k < narts; ++k)
            {
                const bool add = k == 0 || k == 3 || (u >= 6 && u <= 8 && (k == 2 || k == 3)) || (u < 6 && k == 4);
                const double neutral = add ? 0.5 : 1.0;
                i32 (3), i32 (0), f32 (1.0f), f32 (1.0f);
                if (u < 6 && k == 1 && ops[u].env1[0] != -1)
                    env (1, ops[u].env1pts, -1, ops[u].env1[1], ops[u].env1[2]);
                else
                    env (1, { { 0.0, 0.0 }, { 1.0, 1.0 } }, 0, -1, -1);
                i32 (128), i32 (128), i32 (0), i32 (128);
                env (2, { { 0.0, 1.0 }, { 1.0, 1.0 } }, 0, -1, -1);
                i32 (40000), i32 (0), i32 (0), i32 (0), i32 (0);
                for (int m = 0; m < 6; ++m)
                    env (3, { { 0.0, neutral }, { 1.0, neutral } }, 0, -1, -1);
            }
        }
        b.push_back (0), b.push_back (0);
        // zlib with stored blocks
        std::vector<std::uint8_t> z { 0x78, 0x01 };
        for (std::size_t at = 0; at < b.size(); at += 65535)
        {
            const auto n = std::min<std::size_t> (65535, b.size() - at);
            z.push_back (at + n >= b.size() ? 1 : 0);
            z.push_back ((std::uint8_t) n), z.push_back ((std::uint8_t) (n >> 8));
            z.push_back ((std::uint8_t) ~n), z.push_back ((std::uint8_t) (~n >> 8));
            z.insert (z.end(), b.begin() + (std::ptrdiff_t) at, b.begin() + (std::ptrdiff_t) (at + n));
        }
        std::uint32_t a = 1, c = 0;
        for (auto x : b)
            a = (a + x) % 65521, c = (c + a) % 65521;
        for (int k = 3; k >= 0; --k)
            z.push_back ((std::uint8_t) (((c << 16) | a) >> (8 * k)));
        std::vector<std::uint8_t> out { 25, 0, 0, 0 };
        out.insert (out.end(), z.begin(), z.end());
        return out;
    }
};

// renders like FL's Gen_Render calls: notes start/release at the block that contains them
std::pair<std::vector<float>, std::vector<float>> sytrusRender (const std::vector<std::uint8_t>& blob, bool renderMode, int n,
                                                                std::vector<std::array<int, 3>> notes, double fs = 44100.0)
{
    lynx::sytrus::Engine e;
    e.setTempo (120.0);
    e.prepare (fs, renderMode, 512);
    e.setPatch (lynx::sytrus::parsePatch (blob));
    std::vector<float> l ((std::size_t) n), r ((std::size_t) n);
    for (int n0 = 0; n0 < n; n0 += 512)
    {
        for (std::size_t k = 0; k < notes.size(); ++k)
        {
            if (notes[k][1] >= n0 && notes[k][1] < n0 + 512)
            {
                lynx::sytrus::NoteParams p;
                p.key = (float) notes[k][0];
                p.id = (std::int64_t) k + 1;
                e.noteOn (p);
            }
            if (notes[k][2] >= n0 && notes[k][2] < n0 + 512)
                e.release ((std::int64_t) k + 1);
        }
        e.render (l.data() + n0, r.data() + n0, std::min (512, n - n0), 1.0f, 0.0f);
    }
    return { l, r };
}
} // namespace

TEST (sytrus_engine_sine)
{
    // one sine operator, ratio 1, OUT 256, no envelopes: C5 = 523.2511 Hz at 0.7071 (centre of the pan law, §6.1)
    SytrusTestPatch t;
    t.cell (0, 0, 8, 256);
    const auto blob = t.blob();
    CHECK (lynx::sytrus::stateUsable (blob));
    CHECK (lynx::sytrus::parsePatch (blob) != nullptr);
    auto broken = blob;
    broken.resize (broken.size() / 2);
    CHECK (! lynx::sytrus::stateUsable (broken));
    const auto [l, r] = sytrusRender (blob, false, 8192, { { 60, 0, 4096 } });
    double worst = 0.0;
    for (int k = 0; k < 4000; ++k)
        worst = std::max (worst, std::abs ((double) l[(std::size_t) k] - 0.70710678 * std::sin (2.0 * 3.14159265358979 * 523.251130601197 * k / 44100.0)));
    CHECK (worst < 2e-5);
    CHECK (l == r);
    // envelope-less voice end (§4.9, §6.2): finished at a tick after the note-off, then the L/R gains (0.7071) ramp to 0
    // at 0.001 per sample: the last non-zero sample is 4895 for a release at 4096 — the DLL's own figure (EXEC, §6.2)
    int last = 0;
    for (int k = 0; k < 8192; ++k)
        if (l[(std::size_t) k] != 0.0f)
            last = k;
    CHECK (last == 4895);
    // key 69 = 880 Hz; 48 kHz
    const auto [l2, r2] = sytrusRender (blob, false, 4096, { { 69, 0, -1 } }, 48000.0);
    worst = 0.0;
    for (int k = 0; k < 4000; ++k)
        worst = std::max (worst, std::abs ((double) l2[(std::size_t) k] - 0.70710678 * std::sin (2.0 * 3.14159265358979 * 880.0 * k / 48000.0)));
    CHECK (worst < 5e-5);
    (void) r2;
}

TEST (sytrus_engine_reference)
{
    // FM 2→1, Vanilla LP filter, volume envelope, unison 3, delay and reverb, 2× oversampling with the 8-sample
    // envelope tick (render settings): against our Python reference model of REA_rodada31 (sytmodel.render)
    SytrusTestPatch t;
    t.main[195] = 0, t.main[196] = 1, t.main[193] = 0, t.main[194] = 1;
    t.main[173] = 3, t.main[174] = 64, t.main[176] = 100;
    t.ops[1].ratio = 40000;
    t.cell (0, 0, 1, 128); // op 2 → op 1 FM
    t.cell (0, 0, 8, 128);
    t.cell (0, 6, 0, 256); // op 1 → filter 1
    t.cell (0, 6, 8, 256);
    t.cell (0, 0, 7, 200); // FX send
    t.filts[0].p[0] = 1, t.filts[0].p[2] = 180, t.filts[0].p[3] = 60;
    t.ops[0].env1[0] = 0, t.ops[0].env1[1] = 1, t.ops[0].env1[2] = 2;
    t.ops[0].env1pts = { { 0.0, 0.0 }, { 0.05, 1.0 }, { 0.1, 0.6 }, { 0.2, 0.0 } };
    const std::int32_t d0[5] { 40, 100, 50, 100, 0 };
    std::memcpy (t.delays[0].data(), d0, 20);
    t.delays[0][22] = 1;
    t.reverb = { 19, 221, 10, 50, 80, 30, 100, 2, 100 };
    t.reverbOn = true;
    const auto blob = t.blob();
    const auto [l, r] = sytrusRender (blob, true, 12000, { { 60, 0, 6000 } });
    // reference values (model render_mode=True, 44.1 kHz, block 512, key 60, velocity 0.78125, release at 6000); the same
    // state rendered by FL's own Sytrus DLL in render mode differs from this by −136 dB (EXEC, REA round 31 harness)
    const std::array<std::pair<int, float>, 8> golden { { { 100, -0.00235529384f }, { 777, -0.175126165f }, { 2000, 0.30976975f }, { 4321, -0.0213534199f },
                                                          { 6100, 0.202372357f }, { 7000, 0.249530748f }, { 9000, 0.0307101421f }, { 11999, 0.176390275f } } };
    for (const auto& [i, v] : golden)
        CHECK (std::abs (l[(std::size_t) i] - v) < 1e-6f);
    const std::array<std::pair<int, float>, 3> goldenR { { { 777, -0.166890964f }, { 4321, -0.212358952f }, { 9000, -0.0251909085f } } };
    for (const auto& [i, v] : goldenR)
        CHECK (std::abs (r[(std::size_t) i] - v) < 1e-6f);
}

TEST (sytrus_instrument)
{
    // the LYNX instrument: state in ChannelDef::pluginState, channel volume/pan recovered from the rack gains
    SytrusTestPatch t;
    t.cell (0, 0, 8, 256);
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::sytrus;
    c.pluginState = t.blob();
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::sytrus);
    inst->prepare (44100.0);
    inst->configure (c, 120.0);
    CHECK (inst->idle());
    std::vector<float> l (4096), r (4096);
    inst->noteOn (60, 100.0f / 128.0f);
    // rack gains for volume V and pan p: √2·V·cos((1 + p)π/4), √2·V·sin(…); V = 0.5, p = 0 → Final.Vol 0.5
    inst->render (l.data(), r.data(), 4096, 0.5f, 0.5f);
    double peak = 0.0;
    for (int k = 1000; k < 4096; ++k)
        peak = std::max (peak, (double) std::abs (l[(std::size_t) k]));
    CHECK (std::abs (peak - 0.5 * 0.70710678) < 1e-3); // Final.Vol 0.5 × the plugin's centre pan law
    CHECK (! inst->idle());
    inst->noteOff (60);
    std::vector<float> tail (44100 * 3), tailR (tail.size());
    inst->render (tail.data(), tailR.data(), (int) tail.size(), 0.5f, 0.5f);
    CHECK (inst->idle());
    CHECK (tail.back() == 0.0f);
}

TEST (harmor_installation_presets)
{
    // REA_rodada165g-flbeta165s: every Harmor preset of the installation plays on the exact engine — the 88 old
    // (version 11…15) ones through the upgrade to the current layout, which renders exactly like the old bytes and
    // keeps the parameter offsets editable; small images (Labyrinth's 16 × 12) are played, not skipped
    if (realFlpDir.empty())
        return;
    const auto folder = std::filesystem::path (realFlpDir) / "Patches" / "Plugin presets" / "Generators" / "Harmor";
    if (! std::filesystem::exists (folder))
        return;
    int files = 0, usable = 0, old = 0, upgraded = 0, same = 0;
    const std::vector<lynx::harmor::RefNote> notes { { 0, 60, 0.8, 1.0, 0.0, 3000 } };
    for (const auto& entry : std::filesystem::recursive_directory_iterator (folder))
    {
        if (entry.path().extension() != ".fst")
            continue;
        std::ifstream in (entry.path(), std::ios::binary);
        const std::vector<std::uint8_t> file ((std::istreambuf_iterator<char> (in)), {});
        std::vector<std::uint8_t> st;
        try
        {
            for (const auto& c : flp::parseProject (file, true).channels)
                if (! c.pluginState.empty())
                    st = c.pluginState;
        }
        catch (const flp::Error&)
        {
        }
        ++files;
        usable += lynx::harmorStateUsable (st);
        std::uint32_t v = 0;
        if (st.size() >= 4)
            std::memcpy (&v, st.data(), 4);
        if ((v > 0xffff ? v >> 16 : v) >= 16)
            continue;
        ++old;
        const auto up = lynx::harmorUpgradeState (st);
        if (! up)
            continue;
        std::uint32_t uv = 0;
        std::memcpy (&uv, up->data(), 4);
        upgraded += uv == 0x130003u && lynx::harmorParamOffset (*up, 0, 34) > 0 && lynx::harmorStateUsable (*up);
        if (old <= 6) // (a few: rendering is the slow part)
            same += lynx::harmor::renderReference (st, notes, 44100.0, 4096) == lynx::harmor::renderReference (*up, notes, 44100.0, 4096);
        else
            ++same;
    }
    std::printf ("  Harmor installation presets: %d files, %d usable, %d old, %d upgraded\n", files, usable, old, upgraded);
    CHECK (files >= 300 && usable == files && old >= 80 && upgraded == old && same == old);

    std::ifstream in (folder / "Bell" / "Labyrinth.fst", std::ios::binary);
    const std::vector<std::uint8_t> file ((std::istreambuf_iterator<char> (in)), {});
    if (file.empty())
        return;
    auto st = flp::parseProject (file, true).channels.at (0).pluginState;
    const auto a = lynx::harmor::renderReference (st, notes, 44100.0, 4096);
    const std::int32_t zero = 0;
    for (int part = 0; part < 2; ++part)
        std::memcpy (st.data() + lynx::harmorParamOffset (st, part, 92), &zero, 4); // image frequency scale 0
    const auto b = lynx::harmor::renderReference (st, notes, 44100.0, 4096);
    CHECK (a.size() == b.size() && a != b);
}

TEST (harmor_image)
{
    // REA_rodada75: image synthesis — a looping 6-column image (gain + frequency planes) read at a speed, cubic gain and
    // linear frequency interpolation, Hz frequency mode, formant shift — against our reference model (r75 imgm.py)
    const auto blob = harmorTestState (false, true);
    if (const char* dump = std::getenv ("LYNX_DUMP_HARMOR_IMAGE"))
        std::ofstream (dump, std::ios::binary).write ((const char*) blob.data(), (std::streamsize) blob.size());
    CHECK (lynx::harmorStateUsable (blob));
    CHECK (lynx::harmorFallbacks (blob).empty()); // (the image is modelled)
    const auto ref = lynx::harmor::renderReference (blob, { { 0, 60, 0.78125, 1.0, 0.0, 6144 }, { 2000, 64, 0.6, 1.0, 0.0, 7000 } }, 44100.0, 8192);
    CHECK (ref.size() == 2 * 8192);
    if (std::getenv ("LYNX_PRINT_HARMOR_IMAGE") != nullptr)
        for (int i : { 100, 1000, 2500, 3000, 5000, 6500, 8191 })
            std::printf ("harmor image %d %.8g %.8g\n", i, (double) ref[2 * (std::size_t) i], (double) ref[2 * (std::size_t) i + 1]);
    const std::tuple<int, float, float> golden[] {
        { 100, 0.0022269291f, 0.018017211f }, { 1000, 0.013962966f, -0.028602127f }, { 2500, 0.016781429f, -0.028931717f },
        { 3000, -0.010532441f, -0.0061010128f }, { 5000, -0.017426063f, 0.016474312f }, { 6500, 0.1689423f, 0.078417062f },
        { 8191, -0.045403875f, -0.049045475f } };
    for (const auto& [i, gl, gr] : golden)
        CHECK (std::abs (ref[2 * (std::size_t) i] - gl) < 2e-6f && std::abs (ref[2 * (std::size_t) i + 1] - gr) < 2e-6f);
    // the image changes the sound: without it (gain mix 0, frequency scale 0) the same notes render differently
    auto plain = blob;
    const std::int32_t zero = 0;
    std::memcpy (plain.data() + lynx::harmorParamOffset (plain, 0, 89), &zero, 4);
    std::memcpy (plain.data() + lynx::harmorParamOffset (plain, 0, 92), &zero, 4);
    std::memcpy (plain.data() + lynx::harmorParamOffset (plain, 0, 94), &zero, 4);
    const auto ref0 = lynx::harmor::renderReference (plain, { { 0, 60, 0.78125, 1.0, 0.0, 6144 }, { 2000, 64, 0.6, 1.0, 0.0, 7000 } }, 44100.0, 8192);
    double diff = 0.0, energy = 0.0;
    for (std::size_t i = 0; i < ref.size(); ++i)
        diff += (double) (ref[i] - ref0[i]) * (ref[i] - ref0[i]), energy += (double) ref[i] * ref[i];
    CHECK (energy > 1.0 && diff > 0.01 * energy);
    // a resynthesis-mode image (a sample analysed at load time) is not modelled: reported, the part plays its timbres
    auto resyn = blob;
    {
        // the image object follows the 26 curves: find its header (version 0, mode 0, loop 1) from the end of part A
        const std::uint8_t hdr[] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xf0, 0xbf };
        const auto it = std::search (resyn.begin(), resyn.end(), std::begin (hdr), std::end (hdr));
        CHECK (it != resyn.end());
        if (it != resyn.end())
            *(it + 4) = 1;
    }
    const auto fb = lynx::harmorFallbacks (resyn);
    CHECK (std::any_of (fb.begin(), fb.end(), [] (const std::string& f) { return f.find ("resynthesis") != std::string::npos; }));
}

TEST (harmor_round75_features)
{
    // REA_rodada75: arpeggio markers, noise resonance, pluck blur mode, pluck blend, harmonic randomness, phaser and
    // harmonizer masks — LYNX's engine against our reference model (r75 hmodel.py, the Mersenne Twister seeded like LYNX's)
    const std::vector<lynx::harmor::RefNote> chord { { 0, 60, 0.8, 1.0, 0.0, 9000 }, { 1500, 64, 0.7, 1.0, 0.0, 9500 }, { 3000, 67, 0.9, 1.0, 0.0, 10000 } };
    struct Case { int features; std::array<std::tuple<int, float, float>, 4> golden; };
    const Case cases[] {
        { 1, { { { 2000, 0.00326227601f, -0.00143631078f }, { 4500, 0.104633873f, 0.0262026641f }, { 7000, -0.00242280135f, 0.00364600495f }, { 11000, -0.0029983636f, -0.00308720154f } } } },
        { 2, { { { 2000, 0.294110536f, -0.229805974f }, { 4500, -0.00492941859f, -0.00132748827f }, { 7000, 0.0132401136f, 0.00544014934f }, { 11000, -0.00105760543f, -0.00210882883f } } } },
        { 4, { { { 2000, 0.0720629904f, -0.0296807697f }, { 4500, -0.10764533f, 0.113311439f }, { 7000, -0.229827746f, -0.101087983f }, { 11000, -0.0519010639f, -0.0665841137f } } } },
        { 8, { { { 2000, 0.0720039901f, -0.0297230242f }, { 4500, -0.107470558f, 0.113653905f }, { 7000, -0.228478677f, -0.152556434f }, { 11000, -0.0412466244f, -0.0504480202f } } } },
        { 16, { { { 2000, 0.0634379551f, -0.0309438618f }, { 4500, -0.112099687f, 0.112784195f }, { 7000, -0.231666087f, -0.167064955f }, { 11000, -0.0411841346f, -0.0503804788f } } } },
        { 32, { { { 2000, 0.0862953135f, -0.0358822417f }, { 4500, 0.0694009664f, 0.141198453f }, { 7000, -0.158969622f, -0.103794064f }, { 11000, -0.0294273162f, -0.0417808629f } } } },
        { 63, { { { 2000, 0.0172010472f, -0.0106831535f }, { 4500, 0.556600683f, 0.0811084217f }, { 7000, -0.00255421684f, 0.020610704f }, { 11000, -0.00318711501f, 0.000247763831f } } } } };
    for (const auto& c : cases)
    {
        const auto blob = harmorTestState (false, false, c.features);
        if (const char* dump = std::getenv ("LYNX_DUMP_HARMOR_R75"))
            std::ofstream (std::string (dump) + std::to_string (c.features) + ".bin", std::ios::binary).write ((const char*) blob.data(), (std::streamsize) blob.size());
        CHECK (lynx::harmorStateUsable (blob));
        CHECK (lynx::harmorFallbacks (blob).empty());
        const auto ref = lynx::harmor::renderReference (blob, chord, 44100.0, 12000);
        if (std::getenv ("LYNX_PRINT_HARMOR_R75") != nullptr)
            for (int i : { 2000, 4500, 7000, 11000 })
                std::printf ("harmor r75 %d %d %.8g %.8g\n", c.features, i, (double) ref[2 * (std::size_t) i], (double) ref[2 * (std::size_t) i + 1]);
        for (const auto& [i, gl, gr] : c.golden)
            CHECK (std::abs (ref[2 * (std::size_t) i] - gl) < 2e-6f && std::abs (ref[2 * (std::size_t) i + 1] - gr) < 2e-6f);
    }
}

TEST (sytrus_main_settings)
{
    // the channel settings panel's controls (REA_rodada74 §3): FL's "Main settings" ints inside the zlib state
    SytrusTestPatch t;
    t.cell (0, 0, 8, 256);
    t.main[173] = 3;
    const auto blob = t.blob();
    const auto& S = lynx::sytrus::settings();
    auto find = [&] (const char* name) {
        for (int i = 0; i < (int) S.size(); ++i)
            if (std::string (S[(std::size_t) i].name) == name)
                return i;
        return -1;
    };
    const int master = find ("Master level"), unison = find ("Unison"), porta = find ("Porta"), live = find ("Live oversampling");
    CHECK (master >= 0 && unison >= 0 && porta >= 0 && live >= 0 && S.size() == 25u);
    CHECK (lynx::sytrus::settingValue (blob, master) == 128 && lynx::sytrus::settingValue (blob, unison) == 3);
    CHECK (lynx::sytrus::settingValue (blob, porta) == 0 && lynx::sytrus::settingValue (blob, find ("Mod X")) == 256);
    CHECK (! lynx::sytrus::settingValue (blob, -1) && ! lynx::sytrus::settingValue (blob, (int) S.size()));
    auto broken = blob;
    broken.resize (broken.size() / 2);
    CHECK (! lynx::sytrus::settingValue (broken, master) && ! lynx::sytrus::setSetting (broken, master, 64));

    // master level 64: gain (11^(64/128) − 1)/10 instead of 1 (REA_rodada11 §1.3); the state stays readable
    auto quiet = blob;
    CHECK (lynx::sytrus::setSetting (quiet, master, 64) && lynx::sytrus::stateUsable (quiet));
    CHECK (lynx::sytrus::settingValue (quiet, master) == 64 && lynx::sytrus::settingValue (quiet, unison) == 3);
    CHECK (quiet[0] == blob[0] && quiet[1] == blob[1]); // version word kept
    const auto [l1, r1] = sytrusRender (blob, false, 4096, { { 60, 0, -1 } });
    const auto [l2, r2] = sytrusRender (quiet, false, 4096, { { 60, 0, -1 } });
    const auto g = (std::sqrt (11.0) - 1.0) / 10.0;
    double worst = 0.0;
    for (int k = 0; k < 4096; ++k)
        worst = std::max (worst, std::abs ((double) l2[(std::size_t) k] - g * l1[(std::size_t) k]));
    CHECK (worst < 1e-5);
    (void) r1, (void) r2;

    // clamped to the control's range; PORTA is the flag byte the engine reads
    auto edited = blob;
    CHECK (lynx::sytrus::setSetting (edited, unison, 42) && lynx::sytrus::settingValue (edited, unison) == 9);
    CHECK (lynx::sytrus::setSetting (edited, porta, 1) && lynx::sytrus::settingValue (edited, porta) == 1);
    CHECK (lynx::sytrus::setSetting (edited, live, 2) && lynx::sytrus::settingValue (edited, live) == 2);
    CHECK (lynx::sytrus::parsePatch (edited) != nullptr);
}

TEST (harmor_effects_rack)
{
    // REA_rodada38: the rack runs in the state's unit order and the records' FX send joins it at unit 4 — with the FX
    // amount at 1000 every record output bypasses a reverb placed before unit 4 (EXEC: −600 dB vs the rack off)
    auto blob = harmorTestState (false);
    auto set = [&] (std::vector<std::uint8_t>& st, int part, int index, std::int32_t v) {
        std::memcpy (st.data() + lynx::harmorParamOffset (st, part, index), &v, 4);
    };
    set (blob, 0, 10, 1000); // all to the FX send
    auto render = [] (const std::vector<std::uint8_t>& st) {
        return lynx::harmor::renderReference (st, { { 0, 60, 0.8, 1.0, 0.0, 6000 } }, 44100.0, 12000);
    };
    const auto dry = render (blob);
    auto rev = blob;
    set (rev, -1, 22, 1); // reverb on: room 50, diffusion 100, decay 2^3·0.1 s, high cut / damping off, wet 0.8
    set (rev, -1, 24, 1050), set (rev, -1, 26, 50), set (rev, -1, 27, 100), set (rev, -1, 28, 300), set (rev, -1, 29, 1050);
    set (rev, -1, 30, 2), set (rev, -1, 31, 800);
    const auto bypassed = render (rev);
    double eb = 0.0, db = 0.0;
    for (std::size_t i = 0; i < dry.size(); ++i)
        eb += (double) (bypassed[i] - dry[i]) * (bypassed[i] - dry[i]), db += (double) dry[i] * dry[i];
    CHECK (eb < 1e-12 * db && db > 1e-3); // (float rounding of the rack's buffers only)
    auto after = rev;
    const std::int32_t order[6] { 0, 1, 2, 4, 3, 5 }; // the join before the reverb
    for (int k = 0; k < 6; ++k)
        set (after, -1, 53 + k, order[k]);
    const auto wet = render (after);
    double e = 0.0, d = 0.0;
    bool finite = true;
    for (std::size_t i = 0; i < wet.size(); ++i)
        e += (double) (wet[i] - dry[i]) * (wet[i] - dry[i]), d += (double) dry[i] * dry[i], finite = finite && std::isfinite (wet[i]);
    CHECK (finite && e > 1e-4 * d && d > 1e-3);
    // a distortion (type 6) and a delay on the dry path change the sound, stay finite, and the instrument ends quiet
    auto dist = harmorTestState (false);
    {
        const auto at = lynx::harmorParamOffset (dist, -1, 0) + 0xd4 + 0x18 + 48; // global misc +48: distortion type
        const std::int32_t six = 6;
        std::memcpy (dist.data() + at, &six, 4);
    }
    set (dist, -1, 17, 600), set (dist, -1, 19, 1000), set (dist, -1, 21, 1100);
    set (dist, -1, 39, 1), set (dist, -1, 40, 2), set (dist, -1, 41, 400), set (dist, -1, 42, 96), set (dist, -1, 44, 800);
    set (dist, -1, 47, 1050);
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::harmor;
    c.pluginState = dist;
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::harmor);
    inst->configure (c, 120.0);
    inst->prepare (44100.0);
    std::vector<float> l (44100 * 4), r (l.size());
    inst->noteOn (60, 0.8f);
    double energy = 0.0;
    for (int s = 0; s < (int) l.size(); s += 512)
    {
        if (s == 8192)
            inst->noteOff (60);
        inst->render (l.data() + s, r.data() + s, std::min (512, (int) l.size() - s), 1.0f, 1.0f);
    }
    for (std::size_t i = 0; i < l.size(); ++i)
        finite = finite && std::isfinite (l[i]) && std::isfinite (r[i]), energy += (double) l[i] * l[i];
    CHECK (finite && energy > 1.0);
    std::vector<float> tail (44100 * 6), tailR (tail.size());
    for (int s = 0; s < (int) tail.size(); s += 512)
        inst->render (tail.data() + s, tailR.data() + s, std::min (512, (int) tail.size() - s), 1.0f, 1.0f);
    CHECK (inst->idle());
}

TEST (harmless_engine)
{
    // REA_rodada32: state parsing, the additive engine against our reference model (hlmodel.py), host levels, voice end
    std::vector<std::uint8_t> blob;
    auto put = [&] (std::int32_t v) { for (int k = 0; k < 4; ++k) blob.push_back ((std::uint8_t) ((std::uint32_t) v >> (8 * k))); };
    std::array<std::int32_t, 100> p {}; // the reference's clean patch: Saw, open filter, no modulation
    for (int i = 11; i < 23; ++i)
        p[(std::size_t) i] = 1000;
    p[27] = 1000, p[28] = 250, p[31] = 1000, p[36] = 1000, p[37] = 1, p[47] = 1, p[49] = 500, p[51] = -300, p[54] = 8192;
    p[56] = 500, p[65] = 1, p[66] = 128, p[67] = 128, p[68] = 64, p[89] = 3000, p[91] = 500, p[95] = 144, p[97] = 500, p[98] = 500;
    // + resonance, pluck, 3-voice unison, tremolo, LFO on pitch
    p[54] = 3500, p[59] = 600, p[31] = 500, p[65] = 3, p[67] = 150, p[25] = 300, p[86] = 0, p[42] = 200, p[89] = 5000;
    put (12);
    for (auto v : p)
        put (v);
    for (int i = 0; i < 7; ++i)
        put (0);
    put (524); // Custom colour table: 524 × (amplitude, phase)
    for (int i = 0; i < 1048; ++i)
    {
        const auto f = i < 524 ? 1.0f / (float) (i + 1) : 0.25f;
        std::uint32_t bits = 0;
        std::memcpy (&bits, &f, 4);
        put ((std::int32_t) bits);
    }
    const auto parsed = lynx::harmlessParams (blob);
    CHECK (parsed && (int) parsed->size() == lynx::harmlessParamCount);
    CHECK (parsed && (*parsed)[54] == 3500.0f && (*parsed)[99] == 0.0f && (*parsed)[lynx::harmlessCustomFlagIndex] == 1.0f);
    CHECK (parsed && (*parsed)[lynx::harmlessCustomIndex + 1] == 0.5f && (*parsed)[lynx::harmlessCustomIndex + 524] == 0.25f);
    auto old = blob;
    old[0] = 3; // versions below 4 are not Harmless states we know
    CHECK (! lynx::harmlessParams (old));
    old = blob;
    old.resize (300);
    CHECK (! lynx::harmlessParams (old));

    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::harmless;
    c.modelParams = *parsed;
    c.modelParams[lynx::harmlessCustomFlagIndex] = 0.0f; // colour 0 (Saw): the custom table is not read
    auto render = [] (const lynx::ChannelDef& def, int total, float gl, float gr, int offAt) {
        auto inst = lynx::makeInstrument (lynx::InstrumentKind::harmless);
        inst->configure (def, 120.0);
        inst->prepare (44100.0);
        std::vector<float> l ((std::size_t) total), r ((std::size_t) total);
        inst->noteOn (60, 0.78125f);
        for (int s = 0; s < total; s += 512)
        {
            if (s == offAt)
                inst->noteOff (60);
            inst->render (l.data() + s, r.data() + s, std::min (512, total - s), gl, gr);
        }
        return std::make_tuple (l, r, std::move (inst));
    };
    {
        // reference model (REA round 32 hlmodel.py, C5 at FL velocity 100, channel volume 1, centre, released at 6144)
        auto [l, r, inst] = render (c, 8192, 1.0f, 1.0f, 6144);
        const std::tuple<int, float, float> golden[] {
            { 0, 0.0f, 0.0f }, { 10, -0.02220994f, -0.06232700f }, { 100, 0.40623373f, 0.17308202f }, { 1000, -0.04091963f, -0.26821208f },
            { 3000, 0.14519004f, 0.11027778f }, { 6000, 0.50411958f, 0.27901855f }, { 8191, 0.02117692f, -0.01466602f } };
        for (const auto& [i, gl, gr] : golden)
            CHECK (std::abs (l[(std::size_t) i] - gl) < 2e-6f && std::abs (r[(std::size_t) i] - gr) < 2e-6f);
        // after the release the record fades out (below 1e-4, then 30 ms) and the instrument goes idle
        std::vector<float> tail (44100), tailR (tail.size());
        inst->render (tail.data(), tailR.data(), (int) tail.size(), 1.0f, 1.0f);
        CHECK (inst->idle());
    }
    // FL's level law applied once: half the channel volume halves the output; full right pan silences the left side
    const auto full = std::get<0> (render (c, 2048, 1.0f, 1.0f, -1));
    const auto half = std::get<0> (render (c, 2048, 0.5f, 0.5f, -1));
    CHECK (std::abs (half[100] - 0.5f * full[100]) < 1e-6f && std::abs (full[100]) > 0.1f);
    auto mono = c;
    mono.modelParams[25] = 0.0f; // no tremolo, no unison pan spread
    mono.modelParams[65] = 1.0f;
    const auto [pl, pr, pi] = render (mono, 2048, 0.0f, 1.41421356f, -1);
    double el = 0.0, er = 0.0;
    for (int i = 0; i < 2048; ++i)
        el += (double) pl[(std::size_t) i] * pl[(std::size_t) i], er += (double) pr[(std::size_t) i] * pr[(std::size_t) i];
    CHECK (el < 1e-10 * er && er > 1.0);
}

TEST (startup_sound)
{
    // the splash's sound: the right length, finite, at its peak level, quiet before the whoosh swells,
    // a landing "pop" at impactTime and a chime ringing after it
    using namespace lynx::startup;
    const auto sound = makeSound (48000.0);
    CHECK (sound.left.size() == (std::size_t) (soundLength * 48000.0) && sound.right.size() == sound.left.size());
    float peak = 0.0f;
    bool finite = true;
    for (std::size_t i = 0; i < sound.left.size(); ++i)
    {
        finite = finite && std::isfinite (sound.left[i]) && std::isfinite (sound.right[i]);
        peak = std::max ({ peak, std::abs (sound.left[i]), std::abs (sound.right[i]) });
    }
    CHECK (finite && std::abs (peak - peakLevel) < 1.0e-4f);
    auto rms = [&sound] (double from, double to) {
        double e = 0.0;
        const auto a = (std::size_t) (from * 48000.0), b = (std::size_t) (to * 48000.0);
        for (auto i = a; i < b; ++i)
            e += (double) sound.left[i] * sound.left[i];
        return std::sqrt (e / (double) (b - a));
    };
    CHECK (rms (0.0, 0.05) < 0.2 * rms (impactTime - 0.05, impactTime));  // the whoosh swells
    CHECK (rms (impactTime, impactTime + 0.05) > rms (impactTime - 0.05, impactTime)); // the landing
    CHECK (rms (0.8, 1.0) > 0.005 && rms (soundLength - 0.02, soundLength) < rms (0.8, 1.0)); // chime rings, tail fades
    CHECK (makeSound (48000.0).left == sound.left); // the same sound on every start
}

namespace
{
// little-endian byte writer for the state builders below
struct Bytes
{
    std::vector<std::uint8_t> b;
    void raw (const char* s, std::size_t n) { b.insert (b.end(), s, s + n); }
    void i32 (std::int32_t v) { b.insert (b.end(), reinterpret_cast<const std::uint8_t*> (&v), reinterpret_cast<const std::uint8_t*> (&v) + 4); }
    void f32 (float v) { b.insert (b.end(), reinterpret_cast<const std::uint8_t*> (&v), reinterpret_cast<const std::uint8_t*> (&v) + 4); }
    void zeros (std::size_t n) { b.insert (b.end(), n, 0); }
    void bytes (const std::vector<std::uint8_t>& o) { b.insert (b.end(), o.begin(), o.end()); }
};

// A GMS state written from scratch (REA_rodada12 §1.1 layout, "Syn4"): oscillator waves that don't exist (silent
// tables, so the result does not depend on an FL installation), noise through the ladder with envelope and LFO,
// unison, slide, distortion, delay, reverb and the output EQ.
std::vector<std::uint8_t> gmsTestState()
{
    std::array<float, 212> P {};
    for (int i : { 1, 2, 44, 55, 56, 57, 58, 59, 60, 207, 208, 209, 210, 211 })
        P[(std::size_t) i] = 0.5f;
    P[0] = 0.75f, P[4] = 0.1f, P[5] = 0.4f, P[6] = 0.6f, P[7] = 0.3f, P[8] = 0.8f;
    P[10] = 0.2f, P[11] = 1.0f, P[12] = 0.45f, P[13] = 0.55f, P[16] = 0.6f, P[20] = 0.2f, P[22] = 0.5f;
    P[28] = 0.5f, P[34] = 0.4f, P[41] = 0.2f, P[42] = 0.6f, P[43] = 0.5f, P[45] = 0.8f, P[199] = 1.0f, P[198] = 0.8f, P[200] = 1.0f;
    P[147] = 1.0f, P[117] = 0.3f, P[127] = 0.6f;                  // distortion
    P[153] = 1.0f, P[123] = 0.5f, P[133] = 0.4f, P[143] = 0.4f;   // delay (1/2 beat)
    P[154] = 1.0f, P[124] = 0.6f, P[134] = 0.5f;                  // reverb
    P[206] = 1.0f, P[207] = 0.7f, P[211] = 0.3f;                  // output EQ
    Bytes preset;
    preset.raw ("4nyS", 4);
    for (const char* path : { "NoSuchDir\\NoWave A.wav", "NoSuchDir\\NoWave B.wav", "NoSuchDir\\NoWave C.wav", "", "", "", "" })
    {
        const auto n = std::strlen (path);
        preset.raw (path, n);
        preset.zeros (512 - n);
    }
    preset.i32 (0), preset.i32 (0), preset.i32 (0);
    for (auto v : P)
        preset.f32 (v);
    preset.raw ("LYNX test", 9);
    preset.zeros (512 - 9);
    Bytes s;
    s.raw ("SSLF", 4);
    s.i32 ((std::int32_t) preset.b.size());
    s.bytes (preset.b);
    return s.b;
}

// A Morphine state written from scratch (REA_rodada16 §3 / REA_rodada39 §2.2 layout): generators A and B with a few
// frames each (A loops, PWM on A), C and D silent, a morph line A→B, four envelopes (Eg1 → pitch and velocity →
// amplitude through the matrix), the master ADSR and every plugin effect on.
std::vector<std::uint8_t> morphineTestState()
{
    auto mbp = [] (int seed) {
        Bytes b;
        b.raw ("0PBM", 4);
        for (int a = 0; a < 3; ++a)
            for (int byte = 0; byte < 16; ++byte)
            {
                b.b.push_back (0xff);
                for (int bit = 0; bit < 8; ++bit)
                {
                    const auto k = byte * 8 + bit;
                    // (cbrtf is not correctly rounded before glibc 2.43 and differs between libms; the cube root in double
                    // rounded to float gives the same (correctly rounded) bits with old and new glibc)
                    const float v = a == 0 ? (float) std::cbrt ((double) (1.0f / (float) (k + 1 + seed * (k % 3)))) * (k < 40 ? 1.0f : 0.2f)
                                    : a == 1 ? 0.1f * (float) ((k * 7 + seed) % 11 - 5) / 5.0f
                                             : 0.02f * (float) ((k * 5 + seed) % 7 - 3);
                    b.f32 (v);
                }
            }
        return b.b;
    };
    auto msp = [&] (int frames, int seed, int loopMode) {
        Bytes b;
        b.raw ("0PSM", 4);
        b.i32 (frames);
        for (int f = 0; f < frames; ++f)
        {
            const auto m = mbp (seed + f);
            b.i32 (20 + 7 * f), b.f32 (1.0f - 0.2f * (float) f), b.i32 ((std::int32_t) m.size());
            b.bytes (m);
        }
        b.i32 (0x424);
        b.i32 (10), b.i32 (40), b.i32 (0), b.i32 (loopMode);
        b.f32 (0.45f), b.f32 (0.6f), b.f32 (0.5f), b.i32 (0);
        for (int k = 0; k < 128; ++k)
            b.f32 (0.3f);
        for (int k = 0; k < 128; ++k)
            b.f32 (1.0f);
        return b.b;
    };
    auto mvc = [&] (int frames, int seed, int pwmMode) {
        Bytes z;
        z.raw ("0HZM", 4);
        z.i32 (1);
        const auto s = msp (frames, seed, 1);
        z.i32 (128), z.f32 (frames > 1 ? 0.9f : 0.0f), z.i32 ((std::int32_t) s.size());
        z.bytes (s);
        Bytes b;
        b.raw ("0CVM", 4);
        b.i32 ((std::int32_t) z.b.size());
        b.bytes (z.b);
        b.i32 (28), b.f32 (0.4f), b.i32 (pwmMode), b.f32 (0.3f), b.i32 (0), b.f32 (0.5f), b.f32 (0.6f);
        return b.b;
    };
    Bytes p;
    p.raw ("0roM", 4);
    p.raw ("0RPM", 4);
    for (const auto& g : { mvc (3, 1, 2), mvc (2, 7, 0), mvc (1, 3, 0), mvc (1, 5, 0) })
    {
        p.i32 ((std::int32_t) g.size());
        p.bytes (g);
    }
    {
        Bytes m;
        m.raw ("0LMM", 4);
        m.i32 (2), m.i32 (12);
        m.f32 (0.0f), m.f32 (1.0f), m.i32 (60);
        m.f32 (1.0f), m.f32 (1.0f), m.i32 (60);
        m.i32 (0x54), m.i32 (0), m.i32 (0), m.i32 (0), m.i32 (0);
        for (float v : { 1.0f, 0.8f, 0.0f, 0.0f, 0.5f, 0.4f, 0.5f, 0.5f })
            m.f32 (v);
        for (int k = 0; k < 4; ++k)
            m.i32 (0);
        m.f32 (0.5f), m.f32 (0.5f), m.f32 (0.5f), m.i32 (0);
        p.i32 ((std::int32_t) m.b.size());
        p.bytes (m.b);
    }
    {
        Bytes d;
        d.raw ("0DMM", 4);
        d.i32 (4);
        for (int e = 0; e < 4; ++e)
        {
            Bytes g;
            g.raw ("0GEM", 4);
            g.i32 (3), g.i32 (8);
            g.f32 (0.0f), g.i32 (10), g.f32 (1.0f), g.i32 (40 + 10 * e), g.f32 (0.3f), g.i32 (50);
            g.i32 (0x2c), g.i32 (0), g.i32 (0), g.i32 (0), g.f32 (0.5f), g.i32 (0), g.f32 (0.45f), g.i32 (0), g.f32 (0.5f), g.f32 (0.6f),
                g.i32 (e == 0 ? 1 : 0);
            d.i32 ((std::int32_t) g.b.size());
            d.bytes (g.b);
        }
        d.i32 (0x94);
        const int src[12] = { 5, 2 }, dst[12] = { 3, 1 };
        const float amt[12] = { 0.56f, 0.7f };
        for (int k = 0; k < 12; ++k)
            d.i32 (src[k]);
        for (int k = 0; k < 12; ++k)
            d.i32 (dst[k]);
        for (int k = 0; k < 12; ++k)
            d.f32 (amt[k]);
        p.i32 ((std::int32_t) d.b.size());
        p.bytes (d.b);
    }
    {
        const float A[40] = { 0.5f, 0.0f, 0.0f, -1.0f, 0.2f, 0.5f, 0.55f, 0.1f, 0.17f, 0.2f,  // master … velocity curve, ADSR A
                              0.5f, 0.7f, 0.4f, 0.5f, 0.5f, 0.3f, 1.0f, 0.4f, 0.6f, 0.3f,     // D S R, chorus, delay …
                              0.4f, 0.3f, 1.0f, 0.5f, 0.5f, 0.4f, 1.0f, 0.6f, 0.4f, 0.5f,     // … delay, reverb, EQ …
                              0.5f, 0.5f, 0.55f, 0.5f, 0.45f, 1.0f, 0.0f, 0.5f, 1.0f, 0.0f }; // EQ on, noise (muted)
        Bytes g;
        g.i32 (0x5c4);
        for (auto v : A)
            g.f32 (v);
        g.zeros (0x5c4 - g.b.size());
        p.bytes (g.b);
    }
    Bytes s;
    s.raw ("SSLF", 4);
    s.i32 ((std::int32_t) p.b.size());
    s.bytes (p.b);
    return s.b;
}

void dumpIfAsked (const char* var, const std::vector<std::uint8_t>& blob)
{
    if (const char* path = std::getenv (var))
    {
        std::ofstream out (path, std::ios::binary);
        out.write (reinterpret_cast<const char*> (blob.data()), (std::streamsize) blob.size());
    }
}
} // namespace

TEST (dx10_engine)
{
    // REA_rodada42: a Fruity DX10 state written from scratch (version 1: 22 raw parameters) against our reference model
    // (dxmodel.py + dxproc.py, bit-identical to the installation's DLL), the channel processor, host levels and the
    // instrument front-end
    const std::int32_t P[22] = { 1000, 40000, 30000, 30000, 40000, 30000, 35000, 8000, 40000, 50000, 20000,
                                 25000, 15000, 30000, 20000, 10000, 20000, 30000, 5000, 30000, 20000, -1 };
    std::vector<std::uint8_t> blob (4 + 22 * 4, 0);
    blob[0] = 1;
    std::memcpy (blob.data() + 4, P, sizeof (P));
    CHECK (lynx::dx10StateUsable (blob) && lynx::dx10ProcessorNeutral (blob));
    CHECK (lynx::dx10Param (blob, 21) == -1 && lynx::dx10Param (blob, 3) == 30000 && ! lynx::dx10Param (blob, 22));
    CHECK (! lynx::dx10StateUsable (std::vector<std::uint8_t> (40, 0)));
    {
        auto bad = blob;
        bad[0] = 3; // versions ≥ 3 are ignored by the plugin
        CHECK (! lynx::dx10StateUsable (bad));
    }
    using N = lynx::dx10::RefNote;
    const std::vector<N> notes { { 0, 60, 1.0, 0.8, 0.3, 0.0, 6144 }, { 2048, 67, 0.4, 1.0, -0.5, 25.0, 8192 }, { 3000, 40, 2.06, 0.6, 1.0, 0.0, -1 } };
    struct Golden { std::vector<std::uint8_t> state; lynx::dx10::RefOptions opt; int rate; double el, er; std::tuple<int, float, float> at[6]; };
    auto check = [&] (const Golden& g) {
        const auto y = lynx::dx10::renderReference (g.state, notes, (double) g.rate, 16384, g.opt);
        CHECK (y.size() == 2 * 16384);
        if (y.size() != 2 * 16384)
            return y;
        double el = 0.0, er = 0.0;
        for (std::size_t i = 0; i + 1 < y.size(); i += 2)
            el += (double) y[i] * y[i], er += (double) y[i + 1] * y[i + 1];
        CHECK (std::abs (el - g.el) <= 1e-7 * g.el && std::abs (er - g.er) <= 1e-7 * g.er);
        for (const auto& [i, l, r] : g.at)
            CHECK (goldenSampleClose (y[2 * (std::size_t) i], l, libmTolerance (1e-7f))
                   && goldenSampleClose (y[2 * (std::size_t) i + 1], r, libmTolerance (1e-7f)));
        return y;
    };
    lynx::dx10::RefOptions plain, odd;
    odd.block = 333, odd.maxPoly = 1;
    check ({ blob, plain, 44100, 672.293115, 778.393071,
             { { 100, -0.0943799689f, -0.154014125f }, { 1000, -0.0814286768f, -0.132879525f }, { 2500, 0.499493301f, 0.312498778f },
               { 5000, -0.184045464f, 0.171641544f }, { 9000, -0.153369367f, -0.20805186f }, { 16383, -0.00441304967f, 0.200126961f } } });
    // 48 kHz, odd host blocks, max polyphony 1 (the last note steals); the hard-right note keeps FL's x87 left gain
    check ({ blob, odd, 48000, 122.232761, 493.320297,
             { { 100, 0.0236493405f, 0.0385922194f }, { 1000, 0.189806491f, 0.309736073f }, { 2500, -0.425576478f, -0.176279545f },
               { 5000, -3.54030076e-18f, -0.0578194074f }, { 9000, 1.37052479e-17f, 0.223831087f }, { 16383, 9.248742e-18f, 0.151048407f } } });

    // the channel processor (§3): our own settings snapshot — pan LFO (triangle, delayed, faded in), volume envelope
    // with tensions and LFO, cutoff envelope + square LFO, resonance LFO, pitch envelope, SVF LP ×2 (type 7)
    auto record = [] (std::vector<std::uint8_t>& s, int k, std::initializer_list<std::pair<int, int>> fields) {
        const std::int32_t neutral[17] = { 0, 0, 100, 20000, 20000, 30000, 50, 20000, 0, 100, 20000, 0, 32950, 0, 0, 0, 0 };
        std::memcpy (s.data() + 0x44 * k, neutral, sizeof (neutral));
        for (const auto& [i, v] : fields)
            std::memcpy (s.data() + 0x44 * k + 4 * i, &v, 4);
    };
    auto settings = [] (std::vector<std::uint8_t>& s, std::int32_t modX, std::int32_t modY, std::int32_t type) {
        std::memcpy (s.data() + 0x15c, &modX, 4);
        std::memcpy (s.data() + 0x160, &modY, 4);
        std::memcpy (s.data() + 0x168, &type, 4);
    };
    std::vector<std::uint8_t> snap (364, 0);
    record (snap, 0, { { 9, 5000 }, { 10, 9000 }, { 11, -40 }, { 12, 15000 }, { 13, 1 } });
    record (snap, 1, { { 0, 4 }, { 1, 1 }, { 3, 12000 }, { 4, 3000 }, { 5, 15000 }, { 6, 40 }, { 11, 30 }, { 12, 20000 }, { 14, 60 }, { 15, -90 }, { 16, -101 } });
    record (snap, 2, { { 1, 1 }, { 3, 6000 }, { 4, 100 }, { 5, 16000 }, { 6, 20 }, { 8, -60 }, { 11, 20 }, { 12, 18000 }, { 13, 2 } });
    record (snap, 3, { { 11, 40 }, { 12, 17000 } });
    record (snap, 4, { { 1, 1 }, { 3, 5000 }, { 4, 100 }, { 5, 14000 }, { 6, 30 }, { 8, -700 } });
    settings (snap, 140, 120, 7);
    const auto withSnap = lynx::dx10StateWithSnapshot (blob, snap);
    CHECK (withSnap.size() == 460 && withSnap[0] == 2 && ! lynx::dx10ProcessorNeutral (withSnap));
    CHECK (lynx::dx10StateWithSnapshot (withSnap, std::vector<std::uint8_t> (364, 0)) == withSnap); // a v2 state keeps its own
    lynx::dx10::RefOptions ticks;
    ticks.tick = 230, ticks.ppq = 96, ticks.tempo = 120.0;
    check ({ withSnap, ticks, 44100, 346.852062, 554.699375,
             { { 100, -0.00162654708f, -0.00265428401f }, { 1000, 0.0310285408f, 0.0506339073f }, { 2500, -0.118068494f, -0.0915558636f },
               { 5000, -0.297166437f, -0.256465197f }, { 9000, 0.199509233f, 0.166108996f }, { 16383, -0.00392446062f, 0.0410436988f } } });
    // a volume envelope decaying to zero: the 10 ms end fade, then every voice is gone (silence from 14950 on)
    std::vector<std::uint8_t> decay (364, 0);
    for (int k = 0; k < 5; ++k)
        record (decay, k, {});
    record (decay, 1, { { 0, 4 }, { 1, 1 }, { 3, 9000 }, { 4, 100 }, { 5, 12000 }, { 6, 0 }, { 15, 50 }, { 16, -101 } });
    settings (decay, 256, 0, 0);
    auto ticks48 = ticks;
    ticks48.block = 333;
    const auto y = check ({ lynx::dx10StateWithSnapshot (blob, decay), ticks48, 48000, 338.361707, 350.520208,
                            { { 100, 0.000514116196f, 0.000838961045f }, { 1000, 0.0412623473f, 0.0673340634f }, { 2500, -0.141532451f, -0.156581759f },
                              { 5000, 0.382550895f, 0.23462519f }, { 7000, -0.189829409f, 0.144132584f }, { 9000, -0.13638559f, 0.142912149f } } });
    bool silentTail = y.size() == 2 * 16384 && (y[2 * 14949] != 0.0f || y[2 * 14949 + 1] != 0.0f);
    for (std::size_t i = 2 * 14950; i < y.size(); ++i)
        silentTail = silentTail && y[i] == 0.0f;
    CHECK (silentTail);

    // the instrument: LYNX velocity v → Init.Vol = (11^(1.28·v) − 1)/10 and Final.Vol = channel volume × Init.Vol; channel
    // gains √2·V·(cos, sin)((1 + p)π/4) → V, p; notes on block boundaries play like the reference renderer (neutral
    // channel settings: the instrument's sequencer ticks change nothing; vibrato off, since the plugin-wide vibrato
    // oscillator is stepped by each voice in turn, so its interleaving follows the host block layout, as in FL)
    auto noVib = blob;
    CHECK (lynx::dx10SetParam (noVib, 10, 0));
    auto initVol = [] (double v) { return (double) (float) ((std::pow (11.0, 1.28 * v) - 1.0) / 10.0); }; // Init.Vol is a float
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::dx10;
    c.pluginState = noVib;
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::dx10);
    inst->configure (c, 120.0);
    inst->prepare (44100.0);
    const float V = 0.6f, pan = 0.25f;
    const auto gl = (float) (std::sqrt (2.0) * V * std::cos ((1.0 + pan) * 3.141592653589793 / 4.0));
    const auto gr = (float) (std::sqrt (2.0) * V * std::sin ((1.0 + pan) * 3.141592653589793 / 4.0));
    std::vector<float> l (16384), r (16384);
    for (int s = 0; s < 16384; s += 512)
    {
        if (s == 0)
            inst->noteOn (60, 0.78125f);
        if (s == 2048)
        {
            inst->setNextNoteExpression (-0.5f, 25.0f);
            inst->noteOn (67, 0.5f);
        }
        if (s == 6144)
            inst->noteOff (60);
        if (s == 8192)
            inst->noteOff (67);
        inst->render (l.data() + s, r.data() + s, 512, gl, gr);
    }
    // the channel level reaches the engine as FL's (recovered to float precision)
    const auto Vr = std::sqrt (((double) gl * gl + (double) gr * gr) * 0.5);
    const auto pr = std::atan2 ((double) gr, (double) gl) * (4.0 / 3.141592653589793) - 1.0;
    const auto ref = lynx::dx10::renderReference (noVib, { { 0, 60, initVol (0.78125), Vr * initVol (0.78125), pr, 0.0, 6144 },
                                                          { 2048, 67, initVol (0.5), Vr * initVol (0.5), std::clamp (-0.5 + pr, -1.0, 1.0), 25.0, 8192 } },
                                                  44100.0, 16384);
    double worst = 0.0, peak = 0.0;
    for (int i = 0; i < 16384; ++i)
        worst = std::max ({ worst, (double) std::abs (ref[2 * (std::size_t) i] - l[(std::size_t) i]), (double) std::abs (ref[2 * (std::size_t) i + 1] - r[(std::size_t) i]) }),
        peak = std::max (peak, (double) std::abs (l[(std::size_t) i]));
    CHECK (worst == 0.0 && peak > 0.05);
    // the released voices end (DX10's own release) and the instrument goes idle
    std::vector<float> tail (44100), tailR (tail.size());
    for (int k = 0; k < 20 && ! inst->idle(); ++k)
        inst->render (tail.data(), tailR.data(), (int) tail.size(), gl, gr);
    CHECK (inst->idle());

    // with the channel settings above the instrument follows FL's ticks: the cutoff envelope darkens the note
    {
        lynx::ChannelDef cs = c;
        cs.pluginState = withSnap;
        cs.modelParams = { 96.0f };
        auto si = lynx::makeInstrument (lynx::InstrumentKind::dx10);
        si->configure (cs, 120.0);
        si->prepare (44100.0);
        std::vector<float> sl (32768), sr (32768);
        si->noteOn (48, 0.78125f);
        for (int s = 0; s < 32768; s += 512)
            si->render (sl.data() + s, sr.data() + s, 512, 1.0f, 1.0f);
        double e1 = 0.0, e2 = 0.0;
        bool finite = true;
        for (int i = 0; i < 32768; ++i)
            finite = finite && std::isfinite (sl[(std::size_t) i]), (i < 4096 ? e1 : e2) += (double) sl[(std::size_t) i] * sl[(std::size_t) i];
        CHECK (finite && e1 > 1e-3 && e2 > 1e-3);
    }

    // parameter edits (the channel settings panel) write the raw integers, clamped to the knob ranges
    auto edited = blob;
    CHECK (lynx::dx10SetParam (edited, 21, 7) && lynx::dx10Param (edited, 21) == 3);
    CHECK (lynx::dx10SetParam (edited, 12, 70000) && lynx::dx10Param (edited, 12) == 65536);
}

TEST (threeosc_engine)
{
    // REA_rodada47: a 3x Osc state written from scratch (version 14) against our reference model (osc3model.py,
    // bit-identical to the installation's DLL), the plugin voice processor with its release path, the instrument
    std::int32_t P[21] = { -20, 3, 0, 7, 10, -15, 64, 30, 2, -12, 0, 0, 20, 40, 0, 4, 12, -30, -8, 0, 16 };
    auto make = [] (const std::int32_t* p, std::array<std::uint8_t, 3> inv, std::uint8_t am, const std::vector<std::uint8_t>& snap) {
        std::vector<std::uint8_t> st (0x60, 0);
        const std::int32_t version = 14;
        std::memcpy (st.data(), &version, 4);
        std::memcpy (st.data() + 4, p, 21 * 4);
        st[0x58] = inv[0], st[0x59] = inv[1], st[0x5a] = inv[2], st[0x5b] = am;
        st.insert (st.end(), snap.begin(), snap.end());
        return st;
    };
    auto record = [] (std::vector<std::uint8_t>& s, int k, std::initializer_list<std::pair<int, int>> fields) {
        const std::int32_t neutral[17] = { 0, 0, 100, 20000, 20000, 30000, 50, 20000, 0, 100, 20000, 0, 32950, 0, 0, 0, 0 };
        std::memcpy (s.data() + 0x44 * k, neutral, sizeof (neutral));
        for (const auto& [i, v] : fields)
            std::memcpy (s.data() + 0x44 * k + 4 * i, &v, 4);
    };
    auto settings = [] (std::vector<std::uint8_t>& s, std::int32_t modX, std::int32_t modY, std::int32_t type) {
        std::memcpy (s.data() + 0x15c, &modX, 4);
        std::memcpy (s.data() + 0x160, &modY, 4);
        std::memcpy (s.data() + 0x168, &type, 4);
    };
    std::vector<std::uint8_t> neutral (364, 0);
    for (int k = 0; k < 5; ++k)
        record (neutral, k, k == 1 ? std::initializer_list<std::pair<int, int>> { { 0, 4 }, { 16, -101 } } : std::initializer_list<std::pair<int, int>> {});
    settings (neutral, 256, 0, 0);
    const auto blob = make (P, { 0, 0, 0 }, 0, neutral);
    CHECK (lynx::threeOscStateUsable (blob) && lynx::threeOscNormalisedState (blob, {}) == blob);
    CHECK (lynx::threeOscParam (blob, 9) == -12 && lynx::threeOscParam (blob, 20) == 16 && lynx::threeOscParam (blob, 24) == 0 && ! lynx::threeOscParam (blob, 26));

    // older versions are normalised like the plugin reads them: version 12 (no snapshot → the plugin's defaults),
    // version 10 (20 parameters, no invert/AM bytes)
    {
        std::vector<std::uint8_t> v12 (blob.begin(), blob.begin() + 92);
        v12[0] = 12;
        v12[0x58] = 1;
        const auto n12 = lynx::threeOscNormalisedState (v12, {});
        CHECK (n12.size() == 460 && n12[0] == 14 && lynx::threeOscParam (n12, 21) == 1 && lynx::threeOscParam (n12, 20) == 16
               && std::equal (n12.begin() + 0x60, n12.end(), neutral.begin()));
        std::vector<std::uint8_t> v10 (blob.begin(), blob.begin() + 84);
        v10[0] = 10;
        const auto n10 = lynx::threeOscNormalisedState (v10, {});
        CHECK (lynx::threeOscParam (n10, 20) == 0 && lynx::threeOscParam (n10, 13) == 40 && lynx::threeOscParam (n10, 21) == 0);
    }

    using N = lynx::threeosc::RefNote;
    const std::vector<N> notes { { 0, 60, 0.8, 0.3, 0.0, 6144 }, { 2048, 67, 1.0, -0.5, 25.0, 8192 }, { 3000, 40, 0.6, 1.0, 0.0, -1 } };
    struct Golden { std::vector<std::uint8_t> state; lynx::threeosc::RefOptions opt; int rate; double el, er; std::tuple<int, float, float> at[6]; };
    auto check = [&] (const Golden& g) {
        const auto y = lynx::threeosc::renderReference (g.state, notes, (double) g.rate, 16384, g.opt);
        CHECK (y.size() == 2 * 16384);
        if (y.size() != 2 * 16384)
            return y;
        double el = 0.0, er = 0.0;
        for (std::size_t i = 0; i + 1 < y.size(); i += 2)
            el += (double) y[i] * y[i], er += (double) y[i + 1] * y[i + 1];
        CHECK (std::abs (el - g.el) <= 1e-7 * g.el && std::abs (er - g.er) <= 1e-7 * g.er);
        for (const auto& [i, l, r] : g.at)
            CHECK (goldenSampleClose (y[2 * (std::size_t) i], l, libmTolerance (1e-7f))
                   && goldenSampleClose (y[2 * (std::size_t) i + 1], r, libmTolerance (1e-7f)));
        return y;
    };
    // saw / square / triangle-ish mix with pans, stereo detune and phase, random start phases (seed 12345)
    lynx::threeosc::RefOptions plain;
    plain.seed = 12345;
    check ({ blob, plain, 44100, 681.6491972612481, 1509.7245664533937,
             { { 100, 0.0674247071146965f, -0.13440212607383728f }, { 1000, 0.12890009582042694f, 0.446039617061615f },
               { 2500, -0.007686644792556763f, 0.3574123680591583f }, { 5000, 0.08792756497859955f, 0.4948147237300873f },
               { 9000, 1.2136793475613885e-17f, 0.3116455078125f }, { 16383, 5.7305530145831346e-18f, 0.4134995639324188f } } });
    // AM (osc 3 noise modulating osc 1 + 2), inverted oscillators, 48 kHz, odd blocks, max polyphony 1
    const std::int32_t Pam[21] = { 0, 2, 0, 0, 0, 0, 30, 0, 3, 7, 0, 0, 0, 77, 0, 5, -12, 0, 0, 0, 0 };
    lynx::threeosc::RefOptions odd;
    odd.block = 333, odd.maxPoly = 1, odd.seed = 777;
    check ({ make (Pam, { 1, 0, 1 }, 1, neutral), odd, 48000, 453.27241060751771, 1996.4548848690094,
             { { 100, -0.2589033246040344f, -0.310184121131897f }, { 1000, 0.22021546959877014f, 0.3591070771217346f },
               { 2500, -0.5378497242927551f, -0.15602676570415497f }, { 5000, 1.2423200626807308e-17f, 0.3293072581291199f },
               { 9000, 2.5857780099332177e-17f, 0.42474493384361267f }, { 16383, 2.941168384618868e-17f, 0.3569665551185608f } } });
    // the channel processor with its release path: pan LFO, volume envelope released from its current value, cutoff
    // envelope + square LFO, resonance LFO, pitch envelope, SVF LP ×2 (type 7)
    std::vector<std::uint8_t> snap (364, 0);
    record (snap, 0, { { 9, 5000 }, { 10, 9000 }, { 11, -40 }, { 12, 15000 }, { 13, 1 } });
    record (snap, 1, { { 0, 4 }, { 1, 1 }, { 3, 12000 }, { 4, 3000 }, { 5, 15000 }, { 6, 40 }, { 7, 20000 }, { 11, 30 }, { 12, 20000 }, { 14, 60 }, { 15, -90 }, { 16, 50 } });
    record (snap, 2, { { 1, 1 }, { 3, 6000 }, { 4, 100 }, { 5, 16000 }, { 6, 20 }, { 7, 9000 }, { 8, -60 }, { 11, 20 }, { 12, 18000 }, { 13, 2 } });
    record (snap, 3, { { 11, 40 }, { 12, 17000 } });
    record (snap, 4, { { 1, 1 }, { 3, 5000 }, { 4, 100 }, { 5, 14000 }, { 6, 30 }, { 7, 12000 }, { 8, -700 } });
    settings (snap, 140, 120, 7);
    lynx::threeosc::RefOptions ticks;
    ticks.tick = 230, ticks.ppq = 96, ticks.tempo = 120.0, ticks.seed = 99;
    check ({ make (P, { 0, 0, 0 }, 0, snap), ticks, 44100, 977.9928196811321, 2072.965349921462,
             { { 100, -0.001351095037534833f, 0.0029758031014353037f }, { 1000, 0.002833257894963026f, 0.10575854033231735f },
               { 2500, 0.020360738039016724f, 0.3181821405887604f }, { 5000, 0.3340669274330139f, 0.4912331700325012f },
               { 9000, 0.26940545439720154f, 0.27708977460861206f }, { 16383, 0.16570518910884857f, -0.10794822871685028f } } });
    auto ticks48 = ticks;
    ticks48.block = 333, ticks48.tick = 250, ticks48.seed = 5;
    // (at 48 kHz the library's rate ratio 44100/fs scales the filter coefficients and the ramp limit)
    check ({ make (P, { 0, 1, 0 }, 0, snap), ticks48, 48000, 864.9859578029492, 1895.6281818565287,
             { { 100, 0.0031638722866773605f, -0.001987025374546647f }, { 1000, 0.049239177256822586f, 0.08794696629047394f },
               { 2500, 0.13807585835456848f, 0.3554498553276062f }, { 5000, -0.08793184161186218f, -0.1596519947052002f },
               { 9000, 0.08728396147489548f, 0.540290117263794f }, { 16383, 0.039887525141239166f, 0.21270982921123505f } } });

    // HQ oscillators (engine_x64.dll: band-limited tables built with an FFT; FL's single-precision FFT and LYNX's double
    // one agree to float rounding, so the golden values hold to 1e-5)
    {
        const std::int32_t Phq[21] = { 0, 3, 0, 0, 0, 0, 64, 20, 2, -12, 0, 0, 0, 40, -20, 4, 7, 0, 0, 0, 0 };
        auto hq = make (Phq, { 0, 0, 0 }, 0, neutral);
        hq[0x5c] = 1;
        CHECK (lynx::threeOscParam (hq, 25) == 1);
        lynx::threeosc::RefOptions o;
        o.seed = 3;
        const auto y = lynx::threeosc::renderReference (hq, notes, 44100.0, 16384, o);
        CHECK (y.size() == 2 * 16384);
        if (y.size() == 2 * 16384)
        {
            double el = 0.0, er = 0.0;
            for (std::size_t i = 0; i + 1 < y.size(); i += 2)
                el += (double) y[i] * y[i], er += (double) y[i + 1] * y[i + 1];
            CHECK (std::abs (el - 862.7912087773848) < 1e-5 * 862.8 && std::abs (er - 1232.8046929286115) < 1e-5 * 1232.8);
            const std::tuple<int, float, float> at[] = { { 100, -0.019788391888141632f, -0.19209949672222137f }, { 1000, 0.12019827216863632f, 0.03852222114801407f },
                                                          { 2500, 0.3239150643348694f, 0.07325324416160583f }, { 5000, -0.2809634208679199f, -0.5693862438201904f },
                                                          { 16383, -1.1937656360586644e-17f, -0.061853960156440735f } };
            for (const auto& [i, l, r] : at)
                CHECK (std::abs (y[2 * (std::size_t) i] - l) < 1e-5f && std::abs (y[2 * (std::size_t) i + 1] - r) < 1e-5f);
        }
    }

    // the instrument: Final.Vol = channel volume × Init.Vol, channel gains → FL's volume/pan; notes on block boundaries
    // play like the reference renderer (seed 0, neutral channel settings: ticks change nothing)
    auto initVol = [] (double v) { return (double) (float) ((std::pow (11.0, 1.28 * v) - 1.0) / 10.0); };
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::threeosc;
    c.pluginState = blob;
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::threeosc);
    inst->configure (c, 120.0);
    inst->prepare (44100.0);
    const float V = 0.6f, pan = 0.25f;
    const auto gl = (float) (std::sqrt (2.0) * V * std::cos ((1.0 + pan) * 3.141592653589793 / 4.0));
    const auto gr = (float) (std::sqrt (2.0) * V * std::sin ((1.0 + pan) * 3.141592653589793 / 4.0));
    std::vector<float> l (16384), r (16384);
    for (int s = 0; s < 16384; s += 512)
    {
        if (s == 0)
            inst->noteOn (60, 0.78125f);
        if (s == 2048)
        {
            inst->setNextNoteExpression (-0.5f, 25.0f);
            inst->noteOn (67, 0.5f);
        }
        if (s == 6144)
            inst->noteOff (60);
        if (s == 8192)
            inst->noteOff (67);
        inst->render (l.data() + s, r.data() + s, 512, gl, gr);
    }
    const auto Vr = std::sqrt (((double) gl * gl + (double) gr * gr) * 0.5);
    const auto pr = std::atan2 ((double) gr, (double) gl) * (4.0 / 3.141592653589793) - 1.0;
    const auto ref = lynx::threeosc::renderReference (blob, { { 0, 60, Vr * initVol (0.78125), pr, 0.0, 6144 },
                                                              { 2048, 67, Vr * initVol (0.5), std::clamp (-0.5 + pr, -1.0, 1.0), 25.0, 8192 } },
                                                      44100.0, 16384);
    double worst = 0.0, peak = 0.0;
    for (int i = 0; i < 16384; ++i)
        worst = std::max ({ worst, (double) std::abs (ref[2 * (std::size_t) i] - l[(std::size_t) i]), (double) std::abs (ref[2 * (std::size_t) i + 1] - r[(std::size_t) i]) }),
        peak = std::max (peak, (double) std::abs (l[(std::size_t) i]));
    CHECK (worst == 0.0 && peak > 0.05);
    // released notes end with the processor's 10 ms fade (volume envelope off) and the instrument goes idle
    CHECK (inst->idle());

    // parameter edits write the raw integers, clamped to the knob ranges
    auto edited = blob;
    CHECK (lynx::threeOscSetParam (edited, 9, -40) && lynx::threeOscParam (edited, 9) == -24);
    CHECK (lynx::threeOscSetParam (edited, 13, 500) && lynx::threeOscParam (edited, 13) == 128);
    CHECK (lynx::threeOscSetParam (edited, 24, 1) && edited[0x5b] == 1);
    CHECK (! lynx::threeOscStateUsable (std::vector<std::uint8_t> (2, 0)));
}

TEST (gms_engine)
{
    const auto blob = gmsTestState();
    dumpIfAsked ("LYNX_DUMP_GMS", blob);
    CHECK (lynx::gmsStateUsable (blob));
    CHECK (lynx::gmsWaveNames (blob) == std::vector<std::string> ({ "NoWave A", "NoWave B", "NoWave C" }));
    CHECK (lynx::gmsParamOffset (blob, 12) == 8 + 4 + 7 * 512 + 12 + 4 * 12 && lynx::gmsParamOffset (blob, 212) < 0);
    auto broken = blob;
    broken[8] = 'X';
    CHECK (! lynx::gmsStateUsable (broken));
    CHECK (! lynx::gmsStateUsable (std::vector<std::uint8_t> (blob.begin(), blob.begin() + 2000)));

    // golden samples of our round-39 model (gmsmodel.GmsPlugin, waves missing → silent tables, tempo 140, 44.1 kHz,
    // host blocks of 512): C5 vel 100 0–30000, E5 vel 80 11025–40000, G5 vel 110 22050–52000 (Init.Vol law A)
    auto iv = [] (double vel) { return (std::pow (11.0, vel / 100.0) - 1.0) / 10.0; };
    const std::vector<lynx::gms::RefNote> notes = { { 0, 60, (float) iv (100), 1.0, 0.0, 0.0, 30000 },
                                                    { 11025, 64, (float) iv (80), 1.0, 0.0, 0.0, 40000 },
                                                    { 22050, 67, (float) iv (110), 1.0, 0.0, 0.0, 52000 } };
    const auto y = lynx::gms::renderReference (blob, notes, 44100.0, 66150, "");
    CHECK (y.size() == 2 * 66150u);
    static const struct { int i; float l, r; } golden[] = { { 100, 0.0298744496f, 0.0198248848f }, { 5000, 0.0669416264f, 0.0666475743f }, { 15000, -0.005992427f, 0.013085857f }, { 25000, 0.0102018276f, 0.0130400788f }, { 35000, -0.0679694712f, -0.0050432696f }, { 45000, -0.0161557719f, 0.154340729f }, { 55000, -0.00719845621f, 0.0182181317f }, { 66000, 0.0389953516f, 0.0104107196f } };
    for (const auto& g : golden)
        CHECK (std::abs (y[2 * (std::size_t) g.i] - g.l) < 1e-6f && std::abs (y[2 * (std::size_t) g.i + 1] - g.r) < 1e-6f);
    double e = 0.0;
    for (auto v : y)
        e += (double) v * v;
    CHECK (std::abs (std::sqrt (e / (double) y.size()) - 0.0790270452795) < 1e-6);

    // the instrument: same engine behind LYNX's channel gains (V = 1, centre → law A), velocity → Init.Vol
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::gms;
    c.pluginState = blob;
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::gms);
    inst->configure (c, 140.0);
    inst->prepare (44100.0);
    std::vector<float> l (8192, 0.0f), r (8192, 0.0f);
    for (int b = 0; b < 16; ++b)
    {
        if (b == 0)
            inst->noteOn (60, 0.78125f);
        if (b == 10)
            inst->noteOff (60);
        inst->render (l.data() + 512 * b, r.data() + 512 * b, 512, 1.0f, 1.0f);
    }
    const auto ref = lynx::gms::renderReference (blob, { { 0, 60, (std::pow (11.0, 1.28 * 0.78125) - 1.0) / 10.0, 1.0, 0.0, 0.0, 5120 } }, 44100.0, 8192, "");
    double diff = 0.0, energy = 0.0;
    for (int i = 0; i < 8192; ++i)
    {
        diff = std::max ({ diff, (double) std::abs (l[(std::size_t) i] - ref[2 * (std::size_t) i]), (double) std::abs (r[(std::size_t) i] - ref[2 * (std::size_t) i + 1]) });
        energy += (double) l[(std::size_t) i] * l[(std::size_t) i];
    }
    CHECK (diff == 0.0 && energy > 1e-3);
}

// A Toxic Biohazard state written from scratch (REA_rodada45; r45/py/mktest.py builds the same bytes): our own drawn
// spectra on operators 1 and 3 (wave 0) and the sine on operator 2, so no wave of the installation is needed
std::vector<std::uint8_t> toxicTestState()
{
    auto s = lynx::toxicDefaultState();
    static const std::pair<int, double> set[] = {
        { 0, 0.62 }, { 1, 0.3 }, { 3, 0.05 }, { 4, 0.6 }, { 5, 0.7 }, { 6, 0.4 }, { 7, 0.3 }, { 8, 0.4 }, { 9, 0.8 }, { 10, 0.2 },
        { 13, 0.45 }, { 15, 0.45 }, { 16, 0.55 }, { 17, 0.5 }, { 19, 0.1 }, { 20, 0.5 }, { 21, 0.4 }, { 22, 0.4 }, { 23, 0.8 },
        { 24, 0.6 }, { 25, 1 }, { 26, 1 }, { 27, 1 }, { 32, 0.5606 }, { 33, 0.4394 }, { 38, 0.53 }, { 49, 0 }, { 55, 0.25 },
        { 62, 0.5 }, { 75, 0.08 }, { 81, 0.5 }, { 87, 0.3 }, { 104, 0.8 }, { 105, 0.6 }, { 110, 0.35 }, { 115, 0.22 },
        { 127, 0.18 }, { 147, 0.7 }, { 227, 1 }, { 229, 0.65 }, { 233, 0.4 }, { 236, 1 }, { 237, 1 }, { 252, 0.5 }, { 254, 0.3 },
        { 256, 0.4 }, { 267, 0.4 }, { 269, 0.5 }, { 277, 0.3 }, { 300, 1 }, { 301, 8 }, { 302, 2 }, { 303, 0 }, { 305, 3 },
        { 306, 0 }, { 307, 1 }, { 308, 0 }, { 314, 1 }, { 315, 0 }, { 316, 1 }, { 324, 106 }, { 332, 0.7 } };
    for (const auto& [index, value] : set)
    {
        const auto v = (float) value;
        std::memcpy (s.data() + lynx::toxicParamOffset (s, index), &v, 4);
    }
    auto spectrum = [&] (int slot, int h, float amp, float phase) {
        const auto at = 12 + 0x40 + 1024 * (std::size_t) slot + 4 * (std::size_t) h;
        std::memcpy (s.data() + at, &amp, 4);
        std::memcpy (s.data() + at + 512, &phase, 4);
    };
    for (int h = 0; h < 24; ++h)
        spectrum (0, h, (float) (1.0 / (h + 1)), 0.0f);
    for (int k = 0; k < 16; ++k)
        spectrum (2, 2 * k, (float) (1.0 / (2 * k + 1)), 0.25f);
    return s;
}

TEST (toxic_engine)
{
    const auto blob = toxicTestState();
    dumpIfAsked ("LYNX_DUMP_TOXIC", blob);
    CHECK (blob.size() == 10050u && lynx::toxicStateUsable (blob) && ! lynx::toxicNeedsDll (blob));
    CHECK (lynx::toxicParamOffset (blob, 15) == 12 + 0x40 + 8192 + 4 * 15 && lynx::toxicParamOffset (blob, 340) < 0);
    CHECK (! lynx::toxicDllUsable ("") && ! lynx::toxicDllUsable ("/nonexistent/Toxic Biohazard_x64.dll"));
    auto broken = blob;
    broken[8] = 'X';
    CHECK (! lynx::toxicStateUsable (broken));
    CHECK (! lynx::toxicStateUsable (std::vector<std::uint8_t> (blob.begin(), blob.begin() + 5000)));
    auto needs = blob;
    const auto w = 15.0f; // a built-in wave of the installation: without the DLL the engine refuses the state
    std::memcpy (needs.data() + lynx::toxicParamOffset (needs, 307), &w, 4);
    CHECK (lynx::toxicNeedsDll (needs) && lynx::toxic::renderReference (needs, { { 0, 60 } }, 44100.0, 64, "").empty());

    // golden samples of our round-45 model (toxmodel.run, which equals the DLL bit for bit on this state), effects on,
    // tempo 140, 44.1 kHz, host blocks of 512: C5 vel 100 0–30000, E5 vel 80 11025–40000, G5 vel 110 22050–52000
    auto iv = [] (double vel) { return (std::pow (11.0, vel / 100.0) - 1.0) / 10.0; };
    const std::vector<lynx::toxic::RefNote> notes = { { 0, 60, iv (100), 1.0, 0.0, 0.0, 30000 },
                                                      { 11025, 64, iv (80), 1.0, 0.0, 0.0, 40000 },
                                                      { 22050, 67, iv (110), 1.0, 0.0, 0.0, 52000 } };
    const auto y = lynx::toxic::renderReference (blob, notes, 44100.0, 66150, "", 512, 140.0);
    CHECK (y.size() == 2 * 66150u);
    static const struct { int i; float l, r; } golden[] = { { 100, 0.264782101f, 0.189374551f }, { 5000, -0.605827034f, 0.43244195f }, { 15000, -0.343959779f, -0.111227378f }, { 25000, 0.0127612427f, -0.354315311f }, { 35000, -0.353289753f, -0.307500422f }, { 45000, -0.260735184f, 0.202133358f }, { 55000, -0.0751226544f, -0.0776180848f }, { 66000, -0.0294751395f, 0.00692501338f } };
    for (const auto& g : golden)
        CHECK (std::abs (y[2 * (std::size_t) g.i] - g.l) < 1e-6f && std::abs (y[2 * (std::size_t) g.i + 1] - g.r) < 1e-6f);
    double e = 0.0;
    for (auto v : y)
        e += (double) v * v;
    CHECK (std::abs (std::sqrt (e / (double) y.size()) - 0.307227390984) < 1e-6);

    // the instrument: same engine behind LYNX's channel gains (V = 1, centre → law A), velocity → Init.Vol
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::toxic;
    c.pluginState = blob;
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::toxic);
    inst->configure (c, 140.0);
    inst->prepare (44100.0);
    std::vector<float> l (8192, 0.0f), r (8192, 0.0f);
    for (int b = 0; b < 16; ++b)
    {
        if (b == 0)
            inst->noteOn (60, 0.78125f);
        if (b == 10)
            inst->noteOff (60);
        inst->render (l.data() + 512 * b, r.data() + 512 * b, 512, 1.0f, 1.0f);
    }
    const auto ref = lynx::toxic::renderReference (blob, { { 0, 60, (std::pow (11.0, 1.28 * 0.78125) - 1.0) / 10.0, 1.0, 0.0, 0.0, 5120 } },
                                                   44100.0, 8192, "", 512, 140.0);
    double diff = 0.0, energy = 0.0;
    for (int i = 0; i < 8192; ++i)
    {
        diff = std::max ({ diff, (double) std::abs (l[(std::size_t) i] - ref[2 * (std::size_t) i]), (double) std::abs (r[(std::size_t) i] - ref[2 * (std::size_t) i + 1]) });
        energy += (double) l[(std::size_t) i] * l[(std::size_t) i];
    }
    CHECK (diff == 0.0 && energy > 1e-3);
}

TEST (morphine_engine)
{
    const auto blob = morphineTestState();
    dumpIfAsked ("LYNX_DUMP_MORPHINE", blob);
    CHECK (lynx::morphineStateUsable (blob));
    CHECK (lynx::morphineNoiseName (blob).empty());
    const auto at = lynx::morphineParamOffset (blob, 9);
    CHECK (at > 0);
    float a9 = 0.0f;
    std::memcpy (&a9, blob.data() + at, 4);
    CHECK (a9 == 0.2f && lynx::morphineParamOffset (blob, 40) < 0);
    auto broken = blob;
    broken[12] = 'X'; // the MPR0 tag
    CHECK (! lynx::morphineStateUsable (broken));
    CHECK (! lynx::morphineStateUsable (std::vector<std::uint8_t> (blob.begin(), blob.end() - 10)));

    // golden samples of our round-39 model (mormodel.Synth, tempo 120, 44.1 kHz, host blocks of 512): keys 60/64/67,
    // Init.Vol 1.0/0.5/2.0, note-offs at 30000/50000/70000
    const std::vector<lynx::morphine::RefNote> notes = { { 0, 60, 1.0, 1.0, 0.0, 0.0, 30000 },
                                                         { 11025, 64, 0.5, 1.0, 0.0, 0.0, 50000 },
                                                         { 22050, 67, 2.0, 1.0, 0.0, 0.0, 70000 } };
    const auto y = lynx::morphine::renderReference (blob, notes, 44100.0, 80000, "");
    CHECK (y.size() == 2 * 80000u);
    static const struct { int i; float l, r; } golden[] = { { 100, 0.00094500737f, 0.00085531926f }, { 5000, 0.0311509669f, -0.11552836f }, { 15000, 0.0843918771f, 0.0369227529f }, { 25000, 0.60864079f, 1.03062224f }, { 35000, -0.346013308f, 0.135865599f }, { 45000, -0.0373920798f, -0.13064307f }, { 60000, -0.138871655f, -0.163586512f }, { 75000, 0.0251323506f, -0.00735046482f } };
    for (const auto& g : golden)
        CHECK (std::abs (y[2 * (std::size_t) g.i] - g.l) < 1e-6f && std::abs (y[2 * (std::size_t) g.i + 1] - g.r) < 1e-6f);
    double e = 0.0;
    for (auto v : y)
        e += (double) v * v;
    CHECK (std::abs (std::sqrt (e / (double) y.size()) - 0.18467099041) < 1e-6);

    // the instrument: same engine behind LYNX's channel gains
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::morphine;
    c.pluginState = blob;
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::morphine);
    inst->configure (c, 120.0);
    inst->prepare (44100.0);
    std::vector<float> l (8192, 0.0f), r (8192, 0.0f);
    for (int b = 0; b < 16; ++b)
    {
        if (b == 0)
            inst->noteOn (60, 0.78125f);
        if (b == 10)
            inst->noteOff (60);
        inst->render (l.data() + 512 * b, r.data() + 512 * b, 512, 1.0f, 1.0f);
    }
    const auto ref = lynx::morphine::renderReference (blob, { { 0, 60, (std::pow (11.0, 1.28 * 0.78125) - 1.0) / 10.0, 1.0, 0.0, 0.0, 5120 } }, 44100.0, 8192, "");
    double diff = 0.0, energy = 0.0;
    for (int i = 0; i < 8192; ++i)
    {
        diff = std::max ({ diff, (double) std::abs (l[(std::size_t) i] - ref[2 * (std::size_t) i]), (double) std::abs (r[(std::size_t) i] - ref[2 * (std::size_t) i + 1]) });
        energy += (double) l[(std::size_t) i] * l[(std::size_t) i];
    }
    CHECK (diff == 0.0 && energy > 1e-3);
}
namespace
{
// A MiniSynth state written from scratch (REA_rodada41 §1.1 layout: "SSLF" + the .flms preset)
std::vector<std::uint8_t> miniSynthTestState (const std::array<float, 43>& P)
{
    const std::string name = "LYNX test";
    Bytes body;
    body.raw ("\x99" "FLS\r\n\x1a\n", 8);
    body.raw ("HEAD", 4);
    body.i32 (6 + 2 * (std::int32_t) name.size());
    const std::uint16_t head[3] = { 1, 1, (std::uint16_t) name.size() };
    body.raw (reinterpret_cast<const char*> (head), 6);
    for (char ch : name)
        body.raw (&ch, 1), body.zeros (1);
    body.raw ("DATA", 4);
    body.i32 (176);
    body.raw ("0rev", 4);
    for (auto v : P)
        body.f32 (v);
    Bytes s;
    s.raw ("SSLF", 4);
    s.i32 ((std::int32_t) body.b.size());
    s.bytes (body.b);
    return s.b;
}
std::array<float, 43> miniSynthDefaults()
{
    return { 0.375f, 0.5f, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0.5f, 0, 0, 0.25f, 0.5f, 0, 1, 1, 0.07f, 0, 0, 0, 0, 0.7f, 0.5f, 1,
             0.5f, 0, 0.25f, 0.5f, 0.5f, 0, 0.11222222f, 0.4f, 0.1f, 1, 0.5f };
}
// per-process suffix for scratch files: several test binaries (one per worktree) may run at the same time, and a
// fixed name let one process read a file another one was rewriting
std::string processTempTag()
{
    static const std::string tag = std::to_string (std::random_device {}()) + "_"
                                   + std::to_string (std::chrono::steady_clock::now().time_since_epoch().count());
    static const struct Cleanup
    {
        ~Cleanup()
        {
            std::error_code ec;
            std::filesystem::remove (std::filesystem::temp_directory_path (ec) / ("lynx_test_oscdata_" + tag + ".dat"), ec);
            std::filesystem::remove_all (std::filesystem::temp_directory_path (ec) / ("lynx_bassdrum_test_" + tag), ec);
        }
    } cleanup;
    (void) cleanup;
    return tag;
}
// a synthetic oscdata.dat (amplitudes 0.3/k, phases −π/2 + 0.001·(k − 1)): the test does not need an FL installation
std::string miniSynthTestOscdata()
{
    const auto path = (std::filesystem::temp_directory_path() / ("lynx_test_oscdata_" + processTempTag() + ".dat")).string();
    std::vector<float> d (2048);
    for (int h = 0; h < 1024; ++h)
        d[(std::size_t) h] = (float) (0.3 / (h + 1)), d[1024 + (std::size_t) h] = (float) (-1.5707963267948966 + 0.001 * h);
    std::ofstream out (path, std::ios::binary);
    out.write (reinterpret_cast<const char*> (d.data()), 8192);
    return path;
}
} // namespace

TEST (minisynth_engine)
{
    auto A = miniSynthDefaults(), B = miniSynthDefaults();
    // A: poly, SQR+SQR, noise, filter env/velocity/keytracking, LFO → cutoff, distortion + decimator, delay, chorus, phaser
    const std::pair<int, float> a[] = { { 5, 0.45f }, { 6, 0.6f }, { 7, 0.4f }, { 8, 0.45f }, { 9, 0.6f }, { 10, 0.5f }, { 11, 0.7f },
                                        { 12, 0.1f }, { 13, 0.4f }, { 14, 0.3f }, { 15, 0.5f }, { 16, 0.8f }, { 17, 0.6f }, { 18, 0.1f },
                                        { 19, 0.5f }, { 20, 0.85f }, { 21, 0.05f }, { 22, 0.5f }, { 23, 0.6f }, { 24, 0.4f }, { 25, 1 },
                                        { 26, 0.3f }, { 27, 0.1f }, { 28, 1 }, { 29, 0.4f }, { 30, 0.5f }, { 31, 0.6f }, { 32, 0.6f },
                                        { 33, 1 }, { 34, 0.4f }, { 35, 0.6f }, { 36, 0.5f }, { 37, 1 }, { 38, 0.3f }, { 39, 0.6f },
                                        { 40, 0.5f }, { 41, 0.7f }, { 42, 0.4f } };
    // B: lead (legato glide), BELL, LFO → pitch
    const std::pair<int, float> b[] = { { 0, 1 }, { 2, 0.4f }, { 5, 0.7f }, { 6, 0.3f }, { 7, 0.1f }, { 8, 0.6f }, { 9, 0.3f }, { 16, 0.7f },
                                        { 17, 0.1f }, { 18, 0.5f }, { 19, 0.6f }, { 20, 0.7f }, { 22, 0.6f }, { 23, 0.5f }, { 24, 0.3f } };
    for (const auto& [i, v] : a)
        A[(std::size_t) i] = v;
    for (const auto& [i, v] : b)
        B[(std::size_t) i] = v;
    const auto blobA = miniSynthTestState (A), blobB = miniSynthTestState (B);
    dumpIfAsked ("LYNX_DUMP_MINISYNTH", blobA);
    CHECK (lynx::miniSynthStateUsable (blobA) && lynx::miniSynthParams (blobA).size() == 43u && lynx::miniSynthParams (blobA)[26] == 0.3f);
    CHECK (lynx::miniSynthParamOffset (blobA, 9) == 8 + 8 + 8 + 6 + 18 + 12 + 4 * 9 && lynx::miniSynthParamOffset (blobA, 43) < 0);
    CHECK (! lynx::miniSynthStateUsable (std::vector<std::uint8_t> (blobA.begin(), blobA.begin() + 20)));
    auto shortData = blobA;
    shortData.resize (shortData.size() - 4 * 40); // only 3 parameters present: the rest keeps the defaults
    CHECK (lynx::miniSynthParams (shortData)[20] == 0.5f && lynx::miniSynthParams (shortData)[1] == 0.5f);

    // golden samples of our round-41 model (msmodel.Plugin on the same synthetic oscdata, 44.1 kHz, host blocks of 512,
    // L = R = Final.Vol): C5 vel 100 0–30000, E5 vel 80 11025–40000, G5 vel 110 22050–52000 (Init.Vol law A)
    const auto osc = miniSynthTestOscdata();
    auto iv = [] (double vel) { return (std::pow (11.0, vel / 100.0) - 1.0) / 10.0; };
    const std::vector<lynx::minisynth::RefNote> notes = { { 0, 60, iv (100), 1.0, 0.0, 0.0, 30000 },
                                                          { 11025, 64, iv (80), 1.0, 0.0, 0.0, 40000 },
                                                          { 22050, 67, iv (110), 1.0, 0.0, 0.0, 52000 } };
    struct Golden { int i; float l, r; };
    static const Golden goldenA[] = { { 100, 0.183149248f, -0.226551995f }, { 5000, -0.00842505973f, 0.254078597f }, { 15000, -0.180335894f, 0.0890596062f }, { 25000, 0.465151846f, 0.396314442f }, { 35000, 0.272939265f, 0.18288976f }, { 45000, -0.127651259f, -0.34875524f }, { 55000, -0.153043061f, -0.248885199f }, { 66000, -0.0197723396f, -0.0807986259f } };
    static const Golden goldenB[] = { { 100, 0.175394624f, 0.175394624f }, { 5000, 0.0503161065f, 0.0503161065f }, { 15000, 0.0470628254f, 0.0470628254f }, { 25000, 0.0486912318f, 0.0486912318f }, { 35000, 0.021853026f, 0.021853026f }, { 45000, 0.0362072662f, 0.0362072662f }, { 55000, -0.000109080342f, -0.000109080342f }, { 66000, 0.0f, 0.0f } };
    for (int which = 0; which < 2; ++which)
    {
        const auto y = lynx::minisynth::renderReference (which == 0 ? blobA : blobB, notes, 44100.0, 66150, osc);
        CHECK (y.size() == 2 * 66150u);
        if (y.size() != 2 * 66150u)
            continue;
        for (const auto& g : which == 0 ? std::vector (std::begin (goldenA), std::end (goldenA)) : std::vector (std::begin (goldenB), std::end (goldenB)))
            CHECK (y[2 * (std::size_t) g.i] == g.l && y[2 * (std::size_t) g.i + 1] == g.r);
        double e = 0.0;
        for (auto v : y)
            e += (double) v * v;
        CHECK (std::abs (std::sqrt (e / (double) y.size()) - (which == 0 ? 0.315552434392 : 0.03926116287)) < 1e-9);
    }
    // missing oscdata.dat: silent tables, still finite
    const auto silent = lynx::minisynth::renderReference (blobB, notes, 44100.0, 4096, "");
    CHECK (silent.size() == 2 * 4096u && std::all_of (silent.begin(), silent.end(), [] (float v) { return std::isfinite (v) && std::abs (v) < 1.0f; }));

    // the instrument: same engine behind LYNX's channel gains (V = 1, centre → law A), velocity → Init.Vol
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::minisynth;
    c.pluginState = blobA;
    c.samplePath = osc;
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::minisynth);
    inst->configure (c, 120.0);
    inst->prepare (44100.0);
    std::vector<float> l (8192, 0.0f), r (8192, 0.0f);
    for (int k = 0; k < 16; ++k)
    {
        if (k == 0)
            inst->noteOn (60, 0.78125f);
        if (k == 10)
            inst->noteOff (60);
        inst->render (l.data() + 512 * k, r.data() + 512 * k, 512, 1.0f, 1.0f);
    }
    const auto ref = lynx::minisynth::renderReference (blobA, { { 0, 60, (std::pow (11.0, 1.28 * 0.78125) - 1.0) / 10.0, 1.0, 0.0, 0.0, 5120 } },
                                                       44100.0, 8192, osc, 512, 120.0, 0, -1.0, true);
    double diff = 0.0, energy = 0.0;
    for (int i = 0; i < 8192; ++i)
    {
        diff = std::max ({ diff, (double) std::abs (l[(std::size_t) i] - ref[2 * (std::size_t) i]), (double) std::abs (r[(std::size_t) i] - ref[2 * (std::size_t) i + 1]) });
        energy += (double) l[(std::size_t) i] * l[(std::size_t) i];
    }
    CHECK (diff == 0.0 && energy > 1e-3 && ! inst->idle());
}
namespace
{
// A PoiZone state written from scratch (REA_rodada44 §1.3 layout, "Ver4" + 156 floats + name): pulse A synced to saw B,
// noise, envelope to the cutoff, triangle LFO to oscillator A's pitch, 3-voice unison, gate, chorus and tape delay.
std::vector<std::uint8_t> poizoneTestState()
{
    std::array<float, 156> P {};
    const std::pair<int, float> values[] = { { 0, 1.0f }, { 1, 0.7f }, { 2, 1.0f }, { 5, 0.6f }, { 6, 0.75f }, { 7, 0.2f }, { 8, 0.4f }, { 9, 0.1f },
                                             { 10, 0.8f }, { 11, 0.3f }, { 12, 0.5f }, { 13, 0.4f }, { 14, 0.5f }, { 15, 0.5f }, { 16, 0.5f }, { 17, 0.5f },
                                             { 18, 0.35f }, { 19, 0.6f }, { 20, 0.5f }, { 21, 0.3f }, { 22, 0.2f }, { 23, 0.6f }, { 24, 0.7f }, { 25, 0.5f },
                                             { 26, 0.7f }, { 28, 0.6f }, { 29, 0.3f }, { 30, 0.8f }, { 31, 1.0f }, { 32, 0.3f }, { 33, 0.5f }, { 34, 0.5f },
                                             { 35, 0.3f }, { 37, 1.0f }, { 38, 0.6f }, { 39, 1.0f }, { 40, 0.4f }, { 41, 0.5f }, { 42, 0.5f }, { 43, 0.5f },
                                             { 47, 0.5f }, { 48, 0.3f }, { 67, 0.2f }, { 68, 0.5f }, { 75, 2.0f }, { 76, 15.0f }, { 77, 1.0f }, { 78, 1.0f },
                                             { 79, 1.0f } };
    for (const auto& [i, v] : values)
        P[(std::size_t) i] = v;
    for (int i = 49; i < 65; ++i)
        P[(std::size_t) i] = (i - 49) % 3 != 2 ? 1.0f : 0.0f;
    for (int i = 81; i < 155; ++i)
        P[(std::size_t) i] = -1.0f;
    std::vector<std::uint8_t> blob { 'S', 'S', 'L', 'F', 0xb4, 0x02, 0x00, 0x00, '4', 'r', 'e', 'V' };
    blob.resize (12 + 4 * 156 + 64, 0);
    std::memcpy (blob.data() + 12, P.data(), 4 * 156);
    std::memcpy (blob.data() + 12 + 4 * 156, "LYNX test", 9);
    return blob;
}
} // namespace

TEST (poizone_engine)
{
    const auto blob = poizoneTestState();
    CHECK (lynx::poizoneStateUsable (blob));
    CHECK (lynx::poizoneParamOffset (blob, 18) == 12 + 4 * 18 && lynx::poizoneParamOffset (blob, 156) < 0);
    CHECK (lynx::poizoneParam (blob, 75) == 2.0f && ! lynx::poizoneParam (blob, -1));
    auto broken = blob;
    broken[8] = 'X'; // the "Ver4" tag
    CHECK (! lynx::poizoneStateUsable (broken));
    CHECK (! lynx::poizoneStateUsable (std::vector<std::uint8_t> (blob.begin(), blob.begin() + 10)));

    // golden samples of our round-44 model (pzmodel.Plugin, tempo 120, 44.1 kHz, host blocks of 512, L = R = Final.Vol):
    // C5 vel 100 0–30000, E5 vel 80 11025–40000, G5 vel 110 22050–52000 (Init.Vol = (11^(vel/100) − 1)/10)
    auto iv = [] (double vel) { return (std::pow (11.0, vel / 100.0) - 1.0) / 10.0; };
    const std::vector<lynx::poizone::RefNote> notes = { { 0, 60, iv (100), 1.0, 0.0, 0.0, 30000 },
                                                        { 11025, 64, iv (80), 1.0, 0.0, 0.0, 40000 },
                                                        { 22050, 67, iv (110), 1.0, 0.0, 0.0, 52000 } };
    const auto y = lynx::poizone::renderReference (blob, notes, 44100.0, 66150);
    CHECK (y.size() == 2 * 66150u);
    static const struct { int i; float l, r; } golden[] = { { 100, 0.0058150403f, 0.00038307323f }, { 5000, 0.01855834f, 0.09409425f }, { 15000, -0.01985861f, -0.013393694f }, { 25000, 0.121419534f, -0.12814318f }, { 35000, -0.09936376f, 0.034945987f }, { 45000, -0.09803468f, 0.05587045f }, { 55000, -0.07215294f, 0.02417629f }, { 66000, -0.007312033f, -0.047375992f } };
    for (const auto& g : golden)
        CHECK (std::abs (y[2 * (std::size_t) g.i] - g.l) < 1e-7f && std::abs (y[2 * (std::size_t) g.i + 1] - g.r) < 1e-7f);
    double e = 0.0;
    for (auto v : y)
        e += (double) v * v;
    CHECK (std::abs (std::sqrt (e / (double) y.size()) - 0.10990940878400073) < 1e-7);

    // the instrument: same engine behind LYNX's channel gains (V = 1, centre → law A), velocity → Init.Vol
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::poizone;
    c.pluginState = blob;
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::poizone);
    inst->configure (c, 120.0);
    inst->prepare (44100.0);
    std::vector<float> l (8192, 0.0f), r (8192, 0.0f);
    for (int b = 0; b < 16; ++b)
    {
        if (b == 0)
            inst->noteOn (60, 0.78125f);
        if (b == 10)
            inst->noteOff (60);
        inst->render (l.data() + 512 * b, r.data() + 512 * b, 512, 1.0f, 1.0f);
    }
    const auto ref = lynx::poizone::renderReference (blob, { { 0, 60, (std::pow (11.0, 1.28 * 0.78125) - 1.0) / 10.0, 1.0, 0.0, 0.0, 5120 } }, 44100.0, 8192,
                                                     512, 120.0, 0, true);
    double diff = 0.0, energy = 0.0;
    for (int i = 0; i < 8192; ++i)
    {
        diff = std::max ({ diff, (double) std::abs (l[(std::size_t) i] - ref[2 * (std::size_t) i]), (double) std::abs (r[(std::size_t) i] - ref[2 * (std::size_t) i + 1]) });
        energy += (double) l[(std::size_t) i] * l[(std::size_t) i];
    }
    CHECK (diff == 0.0 && energy > 1e-3);
}
// ---- Fruit Kick / BassDrum (REA_rodada43): synthetic states and data, golden values of our round-43 models ----
namespace
{
std::vector<std::uint8_t> fruitKickStateA()
{
    const std::int32_t w[8] = { 0, 0, -600, 64, 128, 32, 20, 0 };
    std::vector<std::uint8_t> b (32);
    std::memcpy (b.data(), w, 32);
    return b;
}

std::vector<std::uint8_t> fruitKickStateB()
{
    std::vector<std::uint8_t> b (400, 0);
    const std::int32_t head[7] = { 1, 624, -8, 42, 86, 64, 0 };
    std::memcpy (b.data(), head, sizeof (head));
    std::int32_t recs[5][17];
    for (auto& r : recs)
    {
        const std::int32_t d[17] = { 0, 0, 100, 20000, 20000, 30000, 50, 20000, 0, 100, 20000, 0, 32950, 0, 0, 0, 0 };
        std::memcpy (r, d, sizeof (d));
    }
    const std::int32_t pan[17] = { 0, 0, 100, 20000, 20000, 30000, 50, 20000, 0, 100, 3000, 64, 20000, 1, 0, 0, 0 };
    const std::int32_t vol[17] = { 4, -1, 100, 3000, 100, 25000, 64, 15000, 0, 100, 20000, 0, 32950, 0, 0, 40, -101 };
    std::memcpy (recs[0], pan, sizeof (pan));
    std::memcpy (recs[1], vol, sizeof (vol));
    for (int k = 0; k < 5; ++k)
        std::memcpy (b.data() + 32 + 68 * k, recs[k], 68);
    const std::int32_t cut = 256;
    std::memcpy (b.data() + 32 + 0x15c, &cut, 4);
    return b;
}

std::string bassDrumTestFolder()
{
    // our own oscillator table and a pseudo-random 16-bit noise sample, laid out like FL's BassDrum folder
    const auto dir = std::filesystem::temp_directory_path() / ("lynx_bassdrum_test_" + processTempTag());
    std::filesystem::create_directories (dir / "Noise Samples");
    auto wav = [] (const std::filesystem::path& path, const void* data, std::uint32_t bytes, std::uint16_t fmt, std::uint16_t bits) {
        std::ofstream o (path, std::ios::binary);
        auto u32 = [&] (std::uint32_t v) { o.write ((const char*) &v, 4); };
        auto u16 = [&] (std::uint16_t v) { o.write ((const char*) &v, 2); };
        o.write ("RIFF", 4), u32 (36 + bytes), o.write ("WAVEfmt ", 8), u32 (16), u16 (fmt), u16 (1), u32 (44100),
            u32 (44100u * bits / 8), u16 ((std::uint16_t) (bits / 8)), u16 (bits), o.write ("data", 4), u32 (bytes);
        o.write ((const char*) data, bytes);
    };
    std::vector<float> table (1024);
    for (int i = 0; i < 1024; ++i)
        table[(std::size_t) i] = (float) (0.9 * std::sin (2.0 * 3.141592653589793 * i / 1024.0) + 0.05 * std::sin (6.0 * 3.141592653589793 * i / 1024.0));
    wav (dir / "sounddata", table.data(), 4096, 3, 32);
    std::vector<std::int16_t> noise (3000);
    std::uint32_t s = 12345;
    for (auto& v : noise)
    {
        s = (s * 1103515245u + 12345u) & 0x7fffffffu;
        v = (std::int16_t) ((int) ((s >> 8) % 20001u) - 10000);
    }
    wav (dir / "Noise Samples" / "test noise.wav", noise.data(), 6000, 1, 16);
    return dir.string();
}

std::vector<std::uint8_t> bassDrumTestState()
{
    std::vector<std::uint8_t> b (0x50C + 92, 0);
    std::memcpy (b.data(), "SSLF", 4);
    const std::uint32_t size = 0x560;
    std::memcpy (b.data() + 4, &size, 4);
    std::memcpy (b.data() + 8, "2kiK", 4);
    std::memcpy (b.data() + 12, "Test kick", 9);
    const char path[] = "C:/Somewhere/Noise Samples/test noise.wav";
    std::memcpy (b.data() + 268, path, sizeof (path) - 1);
    const float P[23] = { 0.55f, 0.5f, 0.6f, 0.6f, 0.45f, 0.1f, 0.3f, 0.4f, 0.3f, 0.6f, 0.4f, 0.65f,
                          0.5f, 0.8f, 0.15f, 0.9f, 0.3f, 0.5f, 0.6f, 0.62f, 0.4f, 0.5f, 0.85f };
    std::memcpy (b.data() + 0x50C, P, sizeof (P));
    return b;
}

struct Golden
{
    int i;
    float l, r;
};

bool matchesGolden (const std::vector<float>& y, std::initializer_list<Golden> golden, double rms)
{
    bool ok = true;
    for (const auto& g : golden)
        ok = ok && std::abs (y[2 * (std::size_t) g.i] - g.l) < 1e-6f && std::abs (y[2 * (std::size_t) g.i + 1] - g.r) < 1e-6f;
    double e = 0.0;
    for (auto v : y)
        e += (double) v * v;
    return ok && std::abs (std::sqrt (e / (double) y.size()) - rms) < 1e-6;
}
} // namespace

TEST (fruitkick_engine)
{
    const auto A = fruitKickStateA(), B = fruitKickStateB();
    CHECK (lynx::fruitKickStateUsable (A) && lynx::fruitKickStateUsable (B));
    CHECK (! lynx::fruitKickStateUsable (std::vector<std::uint8_t> (A.begin(), A.begin() + 20)));
    CHECK (lynx::fruitKickParam (A, 1) == -600 && lynx::fruitKickParam (B, 0) == 624);
    CHECK (lynx::fruitKickFallbacks (A).empty() && lynx::fruitKickFallbacks (B).empty());

    // a version-0 state takes the channel's envelope records as its snapshot (and becomes version 1)
    std::vector<std::vector<std::uint8_t>> recs;
    for (int k = 0; k < 5; ++k)
        recs.emplace_back (B.begin() + 32 + 68 * k, B.begin() + 32 + 68 * (k + 1));
    const auto withEnv = lynx::fruitKickWithChannelEnvelope (A, recs, 256, 0, 0);
    CHECK (withEnv.size() == 400 && lynx::fruitKickParam (withEnv, 5) == 20 && std::memcmp (withEnv.data() + 32, B.data() + 32, 364) == 0);
    CHECK (lynx::fruitKickWithChannelEnvelope (B, recs, 256, 0, 0) == B);

    // golden samples of our round-43 model (fkmodel.run, 44.1 kHz, host blocks of 512): C5 at 0 held, C6 at 3000
    // (Final.Vol 0.5, pan 0.3) released at 9000; B with the plugin's tick every 230 samples from 115
    const std::vector<lynx::fruitkick::RefNote> notes = { { 0, 60, 1.0, 0.0, 0.0, -1 }, { 3000, 72, 0.5, 0.3, 0.0, 9000 } };
    const auto ya = lynx::fruitkick::renderReference (A, notes, 44100.0, 20000);
    CHECK (ya.size() == 40000u);
    // (A uses the distortion: values since REA_rodada55 with FL's x87 live host waveshaper, checked against FLEngine's own;
    // the release fade since REA_rodada74 with IPP's cosine, checked against the DLL)
    CHECK (matchesGolden (ya, { { 10, 0.803601742f, 0.803601742f }, { 700, 0.185467482f, 0.185467482f }, { 2900, -0.63492614f, -0.63492614f },
                                { 3100, 0.339092523f, 0.544403195f }, { 5000, -0.0974622667f, -0.150664076f }, { 9100, 0.124622017f, 0.0544089526f },
                                { 9400, -0.146487325f, -0.144867823f }, { 15000, 0.009439799f, 0.009439799f } },
                          0.284267665614));
    std::vector<int> ticks;
    for (int t = 115; t < 20000; t += 230)
        ticks.push_back (t);
    const auto yb = lynx::fruitkick::renderReference (B, notes, 44100.0, 20000, 512, ticks);
    CHECK (matchesGolden (yb, { { 10, -0.00760388654f, -0.00760388654f }, { 700, 0.386696398f, 0.442232877f }, { 2900, -0.123097703f, -0.302172035f },
                                { 3100, 0.0523624197f, 0.0877688825f }, { 5000, 0.045177035f, 0.100505695f }, { 9100, -0.00423920294f, 0.00810315367f },
                                { 9400, 0.00482427329f, -0.010199016f }, { 15000, -0.000171193431f, 1.01026744e-05f } },
                          0.117964927585));

    // the instrument: same engine behind LYNX's channel gains (V = 1, centre), velocity → Init.Vol (link velocity on)
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::fruitkick;
    c.pluginState = A;
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::fruitkick);
    inst->configure (c, 140.0);
    inst->prepare (44100.0);
    std::vector<float> l (8192, 0.0f), r (8192, 0.0f);
    for (int b = 0; b < 16; ++b)
    {
        if (b == 0)
            inst->noteOn (60, 0.78125f);
        if (b == 10)
            inst->noteOff (60);
        inst->render (l.data() + 512 * b, r.data() + 512 * b, 512, 1.0f, 1.0f);
    }
    const auto iv = (std::pow (11.0, 1.28 * 0.78125) - 1.0) / 10.0;
    const auto ref = lynx::fruitkick::renderReference (A, { { 0, 60, iv, 0.0, 0.0, 5120 } }, 44100.0, 8192);
    double diff = 0.0, energy = 0.0;
    for (int i = 0; i < 8192; ++i)
    {
        diff = std::max ({ diff, (double) std::abs (l[(std::size_t) i] - ref[2 * (std::size_t) i]), (double) std::abs (r[(std::size_t) i] - ref[2 * (std::size_t) i + 1]) });
        energy += (double) l[(std::size_t) i] * l[(std::size_t) i];
    }
    CHECK (diff < 1e-6 && energy > 1e-2);
}

TEST (ipp_cos_and_end_fade)
{
    // ippsCos_32f_A11 of the installation's dsp_ippv2_x64.dll (REA_rodada74 §1): outputs the DLL gave on its two paths
    const auto bits = [] (float v) { return ippcos::floatToBits (v); };
    const auto f = ippcos::bitsToFloat;
    CHECK (bits (ippcos::cosFma (0.1f)) == 0x3f7eaf8bu && bits (ippcos::cosFma (1.0f)) == 0x3f0a534cu);
    CHECK (bits (ippcos::cosFma (2.0f)) == 0xbed51118u && bits (ippcos::cosFma (-3.0f)) == 0xbf7d6646u);
    CHECK (bits (ippcos::cosFma (f (0x3e000007))) == 0x3f7df6f9u && bits (ippcos::cosNoFma (f (0x3e000007))) == 0x3f7df6f8u);
    CHECK (bits (ippcos::cosFma (f (0x3e00007c))) == 0x3f7df6f4u && bits (ippcos::cosNoFma (f (0x3e00007c))) == 0x3f7df6f5u);
    CHECK (ippcos::cosFma (1e-9f) == 1.0f && ippcos::cosNoFma (0.0f) == 1.0f);
    // the portable fused multiply-add rounds once: (1 + 2^-12)^2 = 1 + 2^-11 + 2^-24 is a float halfway case that a
    // tiny addend (lost in a plain double sum) must decide
    const float a = 1.0f + 0x1p-12f;
    CHECK (ippcos::fusedMulAdd (a, a, 0x1p-60f) == 1.0f + 0x1p-11f + 0x1p-23f);
    CHECK (ippcos::fusedMulAdd (a, a, -0x1p-60f) == 1.0f + 0x1p-11f && ippcos::fusedMulAdd (a, a, 0.0f) == 1.0f + 0x1p-11f);
    CHECK (ippcos::fusedMulAdd (3.0f, 5.0f, -1.0f) == 14.0f);
    // the end-fade table: within 1.5e-4 of the exact cosine, ends near zero
    std::vector<float> t (441);
    ippcos::endFadeTable (t.data(), 441);
    double worst = 0.0;
    for (int i = 0; i < 441; ++i)
        worst = std::max (worst, std::abs ((double) t[(std::size_t) i] - 0.5 * (1.0 + std::cos ((i + 1) * 3.141592653589793 / 442.0))));
    CHECK (worst < 1.5e-4 && t[0] > 0.9999f && t[440] < 1e-4f);
}

TEST (bassdrum_engine)
{
    // FL's CRT powf as the engine computes it: a case where its last bit differs from the correctly rounded power
    const float x = 0.87011606f;
    std::uint32_t bits = 0;
    const auto p = lynx::bassdrum::powfMsvc (x, 0.125f);
    std::memcpy (&bits, &p, 4);
    CHECK (bits == 1065063964u);
    CHECK (lynx::bassdrum::powfMsvc (2.0f, 1.0f) == 2.0f && lynx::bassdrum::powfMsvc (10.0f, 0.0f) == 1.0f);

    const auto st = bassDrumTestState();
    CHECK (lynx::bassDrumStateUsable (st) && lynx::bassDrumNoiseName (st) == "test noise.wav");
    CHECK (lynx::bassDrumParamOffset (st, 6) == 0x50C + 24 && lynx::bassDrumParamOffset (st, 23) < 0);
    auto broken = st;
    broken[8] = 'X';
    CHECK (! lynx::bassDrumStateUsable (broken));

    // golden samples of our round-43 model (bdmodel.run with its powf port, 44.1 kHz, host blocks of 512)
    const auto dir = bassDrumTestFolder();
    const std::vector<lynx::bassdrum::RefNote> notes = { { 0, 60, 1.0, 1.0, 0.0, 0.0, -1 }, { 6000, 64, 0.6, 0.8, 0.4, 25.0, 9000 },
                                                         { 6100, 55, 1.5, 1.0, -0.3, 0.0, -1 } };
    const auto y = lynx::bassdrum::renderReference (st, notes, 44100.0, 30000, dir);
    CHECK (y.size() == 60000u);
    CHECK (matchesGolden (y, { { 50, 0.0122119058f, 0.0122119058f }, { 400, -0.0886279643f, -0.0886279643f }, { 3000, -0.0428197235f, -0.0428197235f },
                               { 6050, 0.0199260078f, 0.0240648985f }, { 6300, -0.257363439f, -0.185516387f }, { 9000, 0.268954486f, 0.10506025f },
                               { 15000, -0.0102827176f, -0.0380715057f }, { 25000, 0.0f, 0.0f } },
                         0.230007588169));
    CHECK (lynx::bassdrum::renderReference (st, notes, 44100.0, 100, dir + "/nowhere").empty()); // no table: no engine

    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::bassdrum;
    c.pluginState = st;
    c.samplePath = dir;
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::bassdrum);
    inst->configure (c, 120.0);
    inst->prepare (44100.0);
    std::vector<float> l (8192, 0.0f), r (8192, 0.0f);
    for (int b = 0; b < 16; ++b)
    {
        if (b == 0)
            inst->noteOn (60, 0.78125f);
        inst->render (l.data() + 512 * b, r.data() + 512 * b, 512, 1.0f, 1.0f);
    }
    const auto ref = lynx::bassdrum::renderReference (st, { { 0, 60, (std::pow (11.0, 1.28 * 0.78125) - 1.0) / 10.0, 1.0, 0.0, 0.0, -1 } }, 44100.0, 8192, dir);
    double diff = 0.0, energy = 0.0;
    for (int i = 0; i < 8192; ++i)
    {
        diff = std::max ({ diff, (double) std::abs (l[(std::size_t) i] - ref[2 * (std::size_t) i]), (double) std::abs (r[(std::size_t) i] - ref[2 * (std::size_t) i + 1]) });
        energy += (double) l[(std::size_t) i] * l[(std::size_t) i];
    }
    CHECK (diff == 0.0 && energy > 1e-2);
}
// REA_rodada55: Fruity Fast Dist, Fruity Flangus and Fruity Flanger, goldens from the installation's DLLs (EXEC, our own states)
TEST (fruity_fast_dist_flangus_flanger)
{
    std::vector<float> inL (4096), inR (4096);
    for (int n = 0; n < 4096; ++n)
    {
        inL[(std::size_t) n] = (float) (((n * 37) % 200 - 100) / 60.0);
        inR[(std::size_t) n] = (float) (((n * 53) % 170 - 85) / 50.0);
    }
    struct Gold { int i; float l, r; };
    auto close = [] (float a, float b) { return std::abs (a - b) <= x87Tolerance() * std::max (1.0f, std::abs (b)); };

    // Fast Dist (+ FL's host waveshaper): live and render kernels
    auto dist = [&] (lynx::fastdistfx::State s, bool render, std::initializer_list<Gold> gold) {
        std::vector<float> l (inL), r (inR);
        lynx::fastdistfx::process (l.data(), 4096, s, render);
        lynx::fastdistfx::process (r.data(), 4096, s, render);
        for (const auto& g : gold)
            CHECK (close (l[(std::size_t) g.i], g.l) && close (r[(std::size_t) g.i], g.r));
    };
    dist ({ 128, 10, 0, 128, 128 }, false, { { 100, 0.0f, -0.39352417f }, { 1000, -0.39352417f, 0.389404297f }, { 2047, 0.376953125f, -0.39352417f },
                                            { 3000, -0.39352417f, -0.379760742f }, { 4095, 0.342590332f, 0.373931885f } });
    dist ({ 128, 10, 0, 128, 128 }, true, { { 3000, -0.39352417f, -0.379751563f }, { 4095, 0.342590332f, 0.373931885f } });
    dist ({ 192, 4, 1, 77, 97 }, false, { { 100, 0.0f, -0.868428111f }, { 1000, -1.23000622f, 0.740812242f }, { 2047, 0.58129251f, -0.855666518f },
                                         { 3000, -1.23000622f, -0.613196433f }, { 4095, 0.307311207f, 0.549388587f } });
    dist ({ 192, 4, 1, 77, 97 }, true, { { 2047, 0.58129245f, -0.855666518f }, { 4095, 0.307311267f, 0.549388528f } });
    dist ({ 90, 7, 0, 40, 120 }, false, { { 100, 0.0f, -0.346225947f }, { 1000, -0.511893928f, 0.287246376f }, { 2047, 0.212910682f, -0.340344131f },
                                         { 3000, -0.511893928f, -0.227848753f }, { 4095, 0.0910475403f, 0.197926477f } });
    dist ({ 90, 7, 0, 40, 120 }, true, { { 100, 0.0f, -0.346223623f }, { 1000, -0.511893749f, 0.287246734f }, { 2047, 0.212909341f, -0.340341568f },
                                        { 3000, -0.511893749f, -0.227846757f }, { 4095, 0.0910428166f, 0.197928891f } });
    {
        const auto* T = lynx::fastdistfx::curve (0, 10);
        CHECK (T[0] == 0 && T[8191] == T[8192] && lynx::fastdistfx::curve (1, 1)[8191] == 32767);
    }

    // Flangus: FL's call order (state, sample rate, block size of 512), no flush
    auto flange = [&] (lynx::flangusfx::State s, int fs, std::initializer_list<Gold> gold) {
        lynx::flangusfx::Engine e;
        e.prepare (fs);
        e.load (s);
        e.setRate (fs);
        e.setBlockSize (512);
        std::vector<float> io (8192);
        for (int n = 0; n < 4096; ++n)
        {
            io[2 * (std::size_t) n] = inL[(std::size_t) n];
            io[2 * (std::size_t) n + 1] = inR[(std::size_t) n];
        }
        for (int o = 0; o < 4096; o += 512)
            e.render (io.data() + 2 * o, io.data() + 2 * o, 512);
        for (const auto& g : gold)
            CHECK (close (io[2 * (std::size_t) g.i], g.l) && close (io[2 * (std::size_t) g.i + 1], g.r));
    };
    flange ({ 4, 512, 768, 8, 128, -32, 96, 96 }, 44100, { { 100, 0.0249633491f, -0.119510099f }, { 1000, -1.26988697f, 0.687134743f },
                                                           { 2047, 1.03176403f, -0.278752804f }, { 3000, -0.940228283f, -0.853936791f },
                                                           { 4095, 0.201686561f, 0.607052445f } });
    flange ({ 7, 300, 900, 40, 30, 100, -60, 128 }, 44100, { { 100, 0.0f, 0.0446268953f }, { 1000, 1.68992829f, 2.06982398f },
                                                             { 2047, 0.400214791f, 0.838502884f }, { 3000, 1.20018816f, 1.18875635f },
                                                             { 4095, -0.354708612f, 0.503573656f } });
    flange ({ 2, 1024, 200, 0, 0, -128, 128, -90 }, 48000, { { 100, 0.172592998f, -0.230506495f }, { 1000, -1.56807125f, 0.854493499f },
                                                             { 2047, 1.02325416f, -1.17055321f }, { 3000, -1.74136555f, -0.535462856f },
                                                             { 4095, 1.49345946f, -0.537866116f } });

    // Flanger: state, then the sample rate (FL's call order)
    auto flanger = [&] (lynx::flangerfx::State s, int fs, std::initializer_list<Gold> gold) {
        lynx::flangerfx::Engine e;
        e.load (s);
        e.prepare (fs);
        std::vector<float> io (8192);
        for (int n = 0; n < 4096; ++n)
        {
            io[2 * (std::size_t) n] = inL[(std::size_t) n];
            io[2 * (std::size_t) n + 1] = inR[(std::size_t) n];
        }
        for (int o = 0; o < 4096; o += 512)
            e.render (io.data() + 2 * o, io.data() + 2 * o, 512);
        for (const auto& g : gold)
            CHECK (close (io[2 * (std::size_t) g.i], g.l) && close (io[2 * (std::size_t) g.i + 1], g.r));
    };
    flanger ({ 0, 2000, 2000, 118, 0, 0, 23, 0, 1024, 3500, 3000, 2500 }, 44100,
             { { 100, -1.22955823f, -1.98144293f }, { 1000, -0.975861192f, 0.646231353f }, { 2047, 0.518505156f, -0.857361794f },
               { 3000, -0.546559215f, -0.0157452822f }, { 4095, 0.104127884f, 0.206255436f } });
    flanger ({ 300, 4000, 4500, 90, 1000, 300, 99, 1024, 1024, 2000, 4000, 1000 }, 48000,
             { { 100, 0.0f, -0.440000027f }, { 1000, 0.24619779f, 0.775473535f }, { 2047, 1.11609459f, -0.0639692694f },
               { 3000, -1.22629714f, -1.41118503f }, { 4095, -0.263562649f, 0.80683732f } });

    // import: FL's states become the exact effects with the controls as parameters; short states keep the approximations
    {
        const std::vector<std::uint8_t> fd { 192, 0, 0, 0, 4, 0, 0, 0, 1, 0, 0, 0, 77, 0, 0, 0, 97, 0, 0, 0 };
        const auto slot = lynx::effectFromFl ("Fruity Fast Dist", fd, 120.0);
        CHECK (slot && slot->kind == lynx::EffectKind::fruityFastDist);
        if (slot)
        {
            const auto s = lynx::fastdistfx::fromParams (slot->params.data());
            CHECK (s.pre == 192 && s.threshold == 4 && s.type == 1 && s.mix == 77 && s.post == 97);
            auto fx = lynx::makeEffect (*slot);
            fx->prepare (44100.0);
            std::vector<float> l (inL), r (inR);
            fx->process (l.data(), r.data(), 4096, slot->params, 120.0);
            CHECK (l[1000] == -1.23000622f && r[4095] == 0.549388587f);
        }
        std::vector<std::uint8_t> fg (32);
        const int v[8] { 7, 300, 900, 40, 30, 100, -60, 128 };
        std::memcpy (fg.data(), v, 32);
        const auto f2 = lynx::effectFromFl ("Fruity Flangus", fg, 120.0);
        CHECK (f2 && f2->kind == lynx::EffectKind::fruityFlangus);
        if (f2)
        {
            const auto s = lynx::flangusfx::fromParams (f2->params.data());
            for (int i = 0; i < 8; ++i)
                CHECK (s[(std::size_t) i] == v[i]);
            auto fx = lynx::makeEffect (*f2);
            fx->prepare (48000.0);
            std::vector<float> l (inL), r (inR);
            fx->process (l.data(), r.data(), 4096, f2->params, 120.0);
            CHECK (std::isfinite (l[4095]) && std::isfinite (r[4095]) && std::abs (l[2000]) > 0.0f);
        }
        std::vector<std::uint8_t> fl (52);
        const int w[13] { 1, 300, 4000, 4500, 90, 1000, 300, 99, 1024, 1024, 2000, 4000, 1000 };
        std::memcpy (fl.data(), w, 52);
        const auto f3 = lynx::effectFromFl ("Fruity Flanger", fl, 120.0);
        CHECK (f3 && f3->kind == lynx::EffectKind::fruityFlanger);
        if (f3)
        {
            const auto s = lynx::flangerfx::fromParams (f3->params.data());
            for (int i = 0; i < 12; ++i)
                CHECK (s[(std::size_t) i] == w[i + 1]);
            auto fx = lynx::makeEffect (*f3);
            fx->prepare (48000.0);
            std::vector<float> l (inL), r (inR);
            fx->process (l.data(), r.data(), 4096, f3->params, 120.0);
            CHECK (l[1000] == 0.24619779f && r[4095] == 0.80683732f);
        }
        const auto shortFd = lynx::effectFromFl ("Fruity Fast Dist", std::vector<std::uint8_t> (8, 0), 120.0);
        CHECK (! shortFd || shortFd->kind != lynx::EffectKind::fruityFastDist);
    }
}

// FL's Fruity Phaser (REA_rodada105_phaser): goldens from the installation's own DLL, rendered unmodified in 512-frame
// calls; LYNX's engine matches it to the last bit on the 27 presets (denormals aside), the tolerance only absorbs
// compilers (gcc / clang / zig) and libm.
TEST (fruity_phaser_engine)
{
    std::vector<float> in (8192);
    for (int n = 0; n < 4096; ++n)
    {
        in[2 * (std::size_t) n] = (float) (((n * 37) % 200 - 100) / 60.0);
        in[2 * (std::size_t) n + 1] = (float) (((n * 53) % 170 - 85) / 70.0);
    }
    struct Gold
    {
        int i;
        float l, r;
    };
    auto close = [] (float a, float b) { return std::abs (a - b) <= 2.0e-5f * std::max (1.0f, std::abs (b)); };
    auto phase = [&] (lynx::phaserfx::State s, int fs, std::initializer_list<Gold> gold) {
        lynx::phaserfx::Engine e;
        e.setRate (fs);
        e.load (s);
        e.reset();
        std::vector<float> io (in);
        for (int o = 0; o < 4096; o += 512)
            e.render (io.data() + 2 * o, io.data() + 2 * o, 512);
        for (const auto& g : gold)
            CHECK (close (io[2 * (std::size_t) g.i], g.l) && close (io[2 * (std::size_t) g.i + 1], g.r));
    };
    phase ({ 2500, 100, 800, 0, 512, 7, 400, 512, 4000 }, 44100,
           { { 100, 0.243106663f, -1.63760519f }, { 1000, -2.36990547f, 0.841273248f }, { 2047, 1.21970272f, -1.66379488f },
             { 3000, -2.80603957f, -1.03600585f }, { 4095, 1.80225599f, 0.845855355f } });
    phase ({ 417, 995, 1000, 0, 1024, 3, 705, 0, 2943 }, 48000,
           { { 100, -0.360522032f, -0.0510774851f }, { 1000, -0.788047731f, 0.324495822f }, { 2047, 0.304549545f, 0.0850031748f },
             { 3000, -0.79305625f, 0.114482567f }, { 4095, -0.67624253f, -0.0348635167f } });
    phase ({ 2448, 510, 417, 1024, 1024, 16, 508, 0, 2500 }, 44100,
           { { 100, 1.66273713f, -0.0830700323f }, { 1000, -1.47920549f, -0.0849421024f }, { 2047, -0.157817751f, -1.06916845f },
             { 3000, -0.703828216f, -0.0190049056f }, { 4095, 0.768234849f, -0.965515852f } });
    phase ({ 5000, 0, 1000, 700, 0, 22, 950, 300, 5000 }, 44100,
           { { 100, 0.0809119642f, -0.521223426f }, { 1000, 1.07321298f, 0.681761861f }, { 2047, 0.790922821f, -0.205452889f },
             { 3000, -1.91779351f, 0.599423349f }, { 4095, -1.18747222f, 1.03711367f } });

    // controls: FL's ranges, the stage count clamp, the texts
    CHECK (std::abs (lynx::phaserfx::sweepHz (0.5f, false) - 0.238405839f) < 1.0e-6f);
    CHECK (std::abs (lynx::phaserfx::sweepHz (1.0f, true) - 10.0f) < 1.0e-5f);
    CHECK (lynx::formatEffectParam (lynx::EffectKind::fruityPhaser, 5, 11.0f / 22.0f, 120.0) == "12");
    CHECK (lynx::formatEffectParam (lynx::EffectKind::fruityPhaser, 5, 0.0f, 120.0) == "3");
    CHECK (lynx::formatEffectParam (lynx::EffectKind::fruityPhaser, 7, 0.0f, 120.0) == "100% wet");
    CHECK (lynx::formatEffectParam (lynx::EffectKind::fruityPhaser, 8, 0.5f, 120.0) == "+0.0 dB");
    CHECK (lynx::formatEffectParam (lynx::EffectKind::fruityPhaser, 3, 0.0f, 120.0) == "small");
    {
        lynx::EffectParams ctx {};
        ctx[3] = 1.0f;
        CHECK (lynx::formatEffectParam (lynx::EffectKind::fruityPhaser, 0, 1.0f, 120.0, ctx) == "10.000 Hz");
    }
    CHECK (lynx::effectInfo (lynx::EffectKind::fruityPhaser).params.size() == 9);

    // import: FL's state (version 1 + nine controls) becomes the exact effect; its parameters round-trip
    const lynx::phaserfx::State ouch { 417, 995, 1000, 0, 1024, 3, 705, 0, 2943 };
    const auto bytes = lynx::phaserfx::serialize (ouch);
    const auto slot = lynx::effectFromFl ("Fruity Phaser", bytes, 120.0);
    CHECK (slot && slot->kind == lynx::EffectKind::fruityPhaser);
    if (slot)
    {
        CHECK (lynx::phaserfx::fromParams (slot->params.data()) == ouch);
        auto fx = lynx::makeEffect (*slot);
        fx->prepare (48000.0);
        std::vector<float> l (4096), r (4096);
        for (int n = 0; n < 4096; ++n)
        {
            l[(std::size_t) n] = in[2 * (std::size_t) n];
            r[(std::size_t) n] = in[2 * (std::size_t) n + 1];
        }
        fx->process (l.data(), r.data(), 4096, slot->params, 120.0);
        CHECK (close (l[1000], -0.788047731f) && close (r[4095], -0.0348635167f));
    }
    auto v2 = bytes;
    v2[0] = 2; // FL reads only versions below 2
    const auto later = lynx::effectFromFl ("Fruity Phaser", v2, 120.0);
    CHECK (! later || later->kind != lynx::EffectKind::fruityPhaser);
    CHECK (lynx::plugui::hasFlLayout (lynx::EffectKind::fruityPhaser));
}

// Workflow P2 (docs/WORKFLOW_FL.md)
#include "WorkflowP2Tests.inc"
// Editor mouse grammar (Piano Roll / Playlist)
#include "EditorGestureTests.inc"

TEST (chorus_family_engines)
{
    // REA_rodada59: goldens from the installation's DLLs (44.1 kHz, blocks of 512) on this signal
    std::vector<float> inL (4096), inR (4096);
    for (int n = 0; n < 4096; ++n)
    {
        inL[(std::size_t) n] = (float) (((n * 37) % 200 - 100) / 60.0) * 0.5f;
        inR[(std::size_t) n] = (float) (((n * 53) % 170 - 85) / 50.0) * 0.5f;
    }
    struct Gold { int i; float l, r; };
    auto close = [] (float a, float b) { return std::abs (a - b) <= x87Tolerance() * std::max (1.0f, std::abs (b)); };
    auto run = [&] (const char* plugin, const std::vector<std::uint8_t>& st, lynx::EffectKind kind, std::initializer_list<Gold> gold) {
        const auto slot = lynx::effectFromFl (plugin, st, 120.0);
        CHECK (slot && slot->kind == kind);
        if (! slot)
            return;
        auto fx = lynx::makeEffect (*slot);
        fx->prepare (44100.0);
        std::vector<float> l (inL), r (inR);
        fx->process (l.data(), r.data(), 1024, slot->params, 120.0);
        fx->process (l.data() + 1024, r.data() + 1024, 3072, slot->params, 120.0);
        for (const auto& g : gold)
            CHECK (close (l[(std::size_t) g.i], g.l) && close (r[(std::size_t) g.i], g.r));
    };
    auto ints = [] (std::initializer_list<int> v) {
        std::vector<std::uint8_t> b;
        for (int x : v)
            for (int k = 0; k < 4; ++k)
                b.push_back ((std::uint8_t) ((unsigned) x >> (8 * k)));
        return b;
    };

    // Fruity Chorus (start phases 0, which FL draws at random)
    run ("Fruity Chorus", ints ({ 1, 512, 2250, 341, 1500, 2500, 3500, 23, 0, 0, 0, 2500, 0 }), lynx::EffectKind::fruityChorus,
         { { 100, 0.0f, -0.3850000202655792f }, { 1000, -0.6181581020355225f, 0.2707735002040863f }, { 2047, -0.01424609124660492f, 0.24228274822235107f },
           { 3000, -1.1650776863098145f, -0.1809898018836975f }, { 4095, -0.012790083885192871f, 0.060571759939193726f } });
    run ("Fruity Chorus", ints ({ 1, 700, 4000, 900, 4000, 3000, 2000, 400, 700, 1000, 30, 4999, 0 }), lynx::EffectKind::fruityChorus,
         { { 100, 0.0f, -0.3850000202655792f }, { 1000, -0.5833333134651184f, 0.36345374584198f }, { 2047, -0.0336097776889801f, -0.3360772430896759f },
           { 3000, -0.40196430683135986f, -0.5073210597038269f }, { 4095, -0.1176539734005928f, 0.444706529378891f } });
    run ("Fruity Chorus", ints ({ 1, 36, 469, 1004, 2832, 293, 3500, 0, 428, 0, 0, 3887, 1024 }), lynx::EffectKind::fruityChorus,
         { { 100, 0.2975943684577942f, 0.3747217655181885f }, { 1000, -0.39463338255882263f, 0.18099871277809143f }, { 2047, 0.3894335627555847f, 0.35111793875694275f },
           { 3000, -0.1450725942850113f, 0.2847272455692291f }, { 4095, 0.33678582310676575f, -0.13365517556667328f } });
    {
        const auto slot = lynx::effectFromFl ("Fruity Chorus", ints ({ 1, 36, 469, 1004, 2832, 293, 3500, 0, 428, 0, 0, 3887, 1024 }), 120.0);
        CHECK (slot && lynx::fruitychorusfx::fromParams (slot->params.data())[2] == 1004);
        CHECK (lynx::formatEffectParam (lynx::EffectKind::fruityChorus, 11, 1.0f, 120.0) == "on");
        CHECK (lynx::effectParamFromFl ("Fruity Chorus", 11).has_value() && ! lynx::effectParamFromFl ("Fruity Chorus", 12).has_value());
    }
    // a state FL does not read (version 2) or too short keeps the generic chorus
    CHECK (! lynx::effectFromFl ("Fruity Chorus", ints ({ 2, 512, 2250, 341, 1500, 2500, 3500, 23, 0, 0, 0, 2500, 0 }), 120.0).has_value()
           || lynx::effectFromFl ("Fruity Chorus", ints ({ 2, 512, 2250, 341, 1500, 2500, 3500, 23, 0, 0, 0, 2500, 0 }), 120.0)->kind != lynx::EffectKind::fruityChorus);
    CHECK (! lynx::effectFromFl ("Fruity Chorus", { 1, 0, 0, 0 }, 120.0).has_value()
           || lynx::effectFromFl ("Fruity Chorus", { 1, 0, 0, 0 }, 120.0)->kind != lynx::EffectKind::fruityChorus);

    // Hyper Chorus engine in FL's load sequence (defaults, then the state: the parameters glide), instance k's phases
    auto hyper = [&] (std::initializer_list<int> raws, int instance, std::initializer_list<Gold> gold) {
        namespace hc = lynx::hyperchorusfx;
        hc::Engine e (instance);
        const std::vector<int> r (raws);
        auto val = [] (int i, int raw) { return (float) std::clamp ((double) (raw - hc::rawMin[(std::size_t) i]) * (1.0 / (double) (hc::rawMax[(std::size_t) i] - hc::rawMin[(std::size_t) i])), 0.0, 1.0); };
        for (int i = 0; i < hc::params; ++i)
            e.setParam (i, val (i, hc::defaults[(std::size_t) i]));
        for (int i = 0; i < hc::params; ++i)
            e.setParam (i, val (i, r[(std::size_t) i]));
        e.setRate (44100.0);
        std::vector<float> io (8192);
        for (int n = 0; n < 4096; ++n)
            io[2 * (std::size_t) n] = inL[(std::size_t) n], io[2 * (std::size_t) n + 1] = inR[(std::size_t) n];
        for (int s = 0; s < 4096; s += 512)
            e.render (io.data() + 2 * s, io.data() + 2 * s, 512);
        for (const auto& g : gold)
            CHECK (close (io[2 * (std::size_t) g.i], g.l) && close (io[2 * (std::size_t) g.i + 1], g.r));
    };
    hyper ({ 562, 919, 499, 505, 709, 716, 1, 19247, 10900, 191, 1000 }, 0,
           { { 100, 0.0f, -0.4115639328956604f }, { 1000, -0.658551037311554f, 0.28647756576538086f }, { 2047, 0.24743860960006714f, -0.5088979005813599f },
             { 3000, -0.5171564221382141f, -0.3630276620388031f }, { 4095, 0.08128821104764938f, 0.18811003863811493f } });
    hyper ({ 1000, 555, 0, 400, 300, 300, 0, 22000, 120, 368, 750 }, 2,
           { { 100, 0.0f, -0.398922324180603f }, { 1000, -1.1528236865997314f, 0.2839684784412384f }, { 2047, 0.5115746855735779f, -0.3330395221710205f },
             { 3000, -0.6094785928726196f, 0.17040175199508667f }, { 4095, -0.11666109412908554f, 0.21931695938110352f } });
    CHECK (lynx::hyperchorusfx::randState (1) == 1u * 214013u + 2531011u);
    {
        // the import: controls as parameters; a song's Hyper Chorus slots are numbered in mixer order
        const auto st = ints ({ 2, 1000, 555, 0, 400, 300, 300, 0, 22000, 120, 368, 750, 0 });
        const auto slot = lynx::effectFromFl ("Hyper Chorus", st, 120.0);
        CHECK (slot && slot->kind == lynx::EffectKind::hyperChorus && lynx::hyperchorusfx::fromParams (slot->params.data())[9] == 368);
        lynx::Song song;
        song.mixer.resize (3);
        song.mixer[1].effects = { *slot, *slot };
        song.mixer[2].effects = { *slot };
        lynx::numberChorusInstances (song);
        CHECK (lynx::hyperchorusfx::instanceOf (song.mixer[2].effects[0].state) == 2);
        auto fx = lynx::makeEffect (song.mixer[2].effects[0]);
        fx->prepare (48000.0);
        std::vector<float> l (inL), r (inR);
        fx->process (l.data(), r.data(), 4096, slot->params, 120.0);
        bool finite = true, wet = false;
        for (int n = 0; n < 4096; ++n)
        {
            finite = finite && std::isfinite (l[(std::size_t) n]) && std::isfinite (r[(std::size_t) n]);
            wet = wet || l[(std::size_t) n] != inL[(std::size_t) n];
        }
        CHECK (finite && wet);
        CHECK (lynx::formatEffectParam (lynx::EffectKind::hyperChorus, 8, 0.0f, 120.0) == "off");
        CHECK (! lynx::effectFromFl ("Hyper Chorus", ints ({ 3, 1000, 555, 0, 400, 300, 300, 0, 22000, 120, 368, 750, 0 }), 120.0).has_value()
               || lynx::effectFromFl ("Hyper Chorus", ints ({ 3, 1000, 555, 0, 400, 300, 300, 0, 22000, 120, 368, 750, 0 }), 120.0)->kind != lynx::EffectKind::hyperChorus);
    }

    // Vintage Chorus engine as FL drives it (created, sample rate, each control), fresh instance (its smoothed values
    // start from the engine's initial ones)
    auto vintage = [&] (std::initializer_list<int> raws, std::initializer_list<Gold> gold) {
        namespace vc = lynx::vintagechorusfx;
        vc::Engine e;
        e.setRate (44100.0);
        int i = 0;
        for (int raw : raws)
        {
            e.setParam (i, (float) std::clamp ((double) (raw - vc::rawMin[(std::size_t) i]) * (1.0 / (double) (vc::rawMax[(std::size_t) i] - vc::rawMin[(std::size_t) i])), 0.0, 1.0));
            ++i;
        }
        std::vector<float> io (8192);
        for (int n = 0; n < 4096; ++n)
            io[2 * (std::size_t) n] = inL[(std::size_t) n], io[2 * (std::size_t) n + 1] = inR[(std::size_t) n];
        for (int s = 0; s < 4096; s += 512)
            e.render (io.data() + 2 * s, io.data() + 2 * s, 512);
        for (const auto& g : gold)
            CHECK (close (io[2 * (std::size_t) g.i], g.l) && close (io[2 * (std::size_t) g.i + 1], g.r));
    };
    vintage ({ 2, 0, 211046, 1, 2411, 960, 1985, 5121, 0, 1, 4420, 6000, 273, 0, 0, 0, 0 },
             { { 100, 0.0f, -0.05606573447585106f }, { 1000, -0.3413679003715515f, 0.20469821989536285f }, { 2047, 0.1487489938735962f, -0.28199973702430725f },
               { 3000, -0.31699514389038086f, -0.43230289220809937f }, { 4095, 0.1255197525024414f, 0.0810704305768013f } });
    vintage ({ 2, 0, 80, 0, 0, 60, 3000, 3000, 0, 0, 0, 3000, 0, 3000, 0, 0, 0 }, // hiss
             { { 100, -1.2527654007499223e-06f, -0.053655341267585754f }, { 1000, -0.44306400418281555f, 0.25606146454811096f }, { 2047, 0.23425261676311493f, -0.21375307440757751f },
               { 3000, -0.5793180465698242f, -0.13371579349040985f }, { 4095, 0.05595953390002251f, 0.11362095922231674f } });
    vintage ({ 0, 1, 49152, 0, 3000, 60, 3000, 3000, 0, 0, 0, 3000, 0, 0, 0, 0, 0 }, // Chorus 1 + 2
             { { 100, 0.0f, -0.05365874245762825f }, { 1000, -0.5095958113670349f, 0.2809423506259918f }, { 2047, 0.17256303131580353f, -0.3563305139541626f },
               { 3000, -0.471851646900177f, -0.30123069882392883f }, { 4095, 0.0959278866648674f, 0.13436323404312134f } });
    {
        const auto st = ints ({ 1, 2, 0, 211046, 1, 2411, 960, 1985, 5121, 0, 1, 4420, 6000, 273, 0, 0, 0, 0 });
        const auto slot = lynx::effectFromFl ("Vintage Chorus", st, 120.0);
        CHECK (slot && slot->kind == lynx::EffectKind::vintageChorus && lynx::vintagechorusfx::fromParams (slot->params.data())[2] == 211046);
        auto fx = lynx::makeEffect (*slot);
        fx->prepare (44100.0);
        std::vector<float> l (inL), r (inR);
        fx->process (l.data(), r.data(), 4096, slot->params, 120.0);
        bool finite = true;
        for (int n = 0; n < 4096; ++n)
            finite = finite && std::isfinite (l[(std::size_t) n]) && std::isfinite (r[(std::size_t) n]);
        CHECK (finite && l[3000] != inL[3000]);
        CHECK (lynx::formatEffectParam (lynx::EffectKind::vintageChorus, 0, 0.0f, 120.0) == "Chorus 1");
        CHECK (! lynx::effectFromFl ("Vintage Chorus", ints ({ 2, 2, 0, 211046, 1, 2411, 960, 1985, 5121, 0, 1, 4420, 6000, 273, 0, 0, 0, 0 }), 120.0).has_value()
               || lynx::effectFromFl ("Vintage Chorus", ints ({ 2, 2, 0, 211046, 1, 2411, 960, 1985, 5121, 0, 1, 4420, 6000, 273, 0, 0, 0, 0 }), 120.0)->kind != lynx::EffectKind::vintageChorus);
    }
}
#include "UiChromeTests.inc"

TEST (fruity_delay_engines)
{
    // REA_rodada52: the engines on their own against the installation's DLLs (goldens = the DLL's output for the same
    // input with FL's set-up order, EXEC: bit-identical), then the mixer effects built from FL states
    const int n = 16384;
    std::vector<float> inL ((std::size_t) n, 0.0f), inR ((std::size_t) n, 0.0f);
    std::uint32_t seed = 1;
    for (int i = 0; i < 4096; ++i) // a noise burst, then the echoes
    {
        seed = seed * 1664525u + 1013904223u;
        inL[(std::size_t) i] = (float) ((int) (seed >> 16) - 32768) / 32768.0f * 0.5f;
        seed = seed * 1664525u + 1013904223u;
        inR[(std::size_t) i] = (float) ((int) (seed >> 16) - 32768) / 32768.0f * 0.5f;
    }
    using Golden = std::vector<std::tuple<int, float, float>>;
    auto check = [&] (const std::vector<float>& l, const std::vector<float>& r, const Golden& golden) {
        for (const auto& [i, gl, gr] : golden)
            CHECK (std::abs (l[(std::size_t) i] - gl) < 2e-6f && std::abs (r[(std::size_t) i] - gr) < 2e-6f);
    };
    auto run3 = [&] (const std::array<int, 26>& raw, double bpm) {
        lynx::fldelay::Delay3Engine e (44100.0);
        e.setTempo (bpm);
        for (int i = 0; i < 26; ++i)
            e.setParameter (i, lynx::fldelay::delay3EngineValue (i, raw[(std::size_t) i]));
        e.setSampleRate (44100.0); // FL's wrapper sets the rate up again after the state
        e.setSampleRate (44100.0);
        std::vector<float> l (inL), r (inR);
        for (int s = 0; s < n; s += 512)
            e.process (l.data() + s, r.data() + s, 512);
        return std::make_pair (l, r);
    };
    auto run2 = [&] (const std::array<int, 8>& raw, double bpm) {
        lynx::fldelay::Delay2State st;
        st.v = raw;
        lynx::fldelay::Delay2Engine e (st, 44100.0, bpm, 512);
        std::vector<float> l (inL), r (inR);
        for (int s = 0; s < n; s += 512)
            e.process (l.data() + s, r.data() + s, 512);
        return std::make_pair (l, r);
    };
    // Delay 3, true stereo, synced 1/8 beat, cutoff + resonance (Fruity Delay 3_x64.dll at 120 and 100 BPM)
    const std::array<int, 26> a { 6000, 1, 1, 0, 24, 4000, 500, 5000, 3000, 0, 0, 0, 0, 1, 4200, 0, 4500, 1500, 0, 0, 0, 60000, 6000, 6000, 0, 6000 };
    auto o = run3 (a, 120.0);
    check (o.first, o.second, { { 3000, 0.399949551f, 0.598533869f }, { 6000, 0.116703831f, -0.315510482f }, { 9000, -0.150520369f, -0.250620633f },
                                { 13000, 0.193608373f, 0.0598564595f }, { 16000, -0.0524701327f, 0.022601109f } });
    o = run3 (a, 100.0);
    check (o.first, o.second, { { 5000, 0.377422422f, 0.346570343f }, { 6000, -0.182619616f, 0.0488029197f }, { 11000, -0.00120426295f, 0.0253051519f },
                                { 16000, 0.0806550607f, -0.0991330743f } });
    // ping pong, keep pitch, modulation, cutoff LFO, diffusion, saturation, rate and bit reduction, colour
    o = run3 ({ 5000, 2, 1, 1, 36, 2100, 500, 6000, 4500, 3000, 2000, 3500, 4000, 1, 3800, 2, 3500, 2500, 2500, 1200, 3000, 40000, 4000, 5500, -1200, 4000 }, 120.0);
    check (o.first, o.second, { { 1500, 0.288540125f, -0.148862943f }, { 5000, 5.90502168e-06f, 0.111382999f }, { 8000, -0.0736631379f, -0.0305093247f },
                                { 11000, -0.00831047911f, 0.0102323527f }, { 16000, 0.0175791476f, 0.0273917634f } });
    // fake stereo, unsynced, high-pass, limit distortion, no dry
    o = run3 ({ 6000, 0, 0, 0, 200, 3700, 0, 3000, 1500, 0, 0, 1500, 2000, 0, 3000, 1, 3000, 0, 3000, 0, 0, 60000, 6000, 6000, 1500, 0 }, 120.0);
    check (o.first, o.second, { { 1500, 0.0825810209f, 0.0228505582f }, { 6000, -0.0427959524f, -0.0408911295f }, { 9000, 0.0114790015f, 0.019227922f },
                                { 16000, -0.000312746735f, 0.00101647642f } });
    // delay type "Off": filter, distortion and the bit/rate reduction on the dry path
    o = run3 ({ 6000, 3, 1, 0, 24, 3000, 500, 6000, 3000, 0, 0, 0, 0, 1, 3000, 2, 2500, 4000, 4000, 3000, 1000, 30000, 2000, 6000, 800, 3000 }, 120.0);
    check (o.first, o.second, { { 1500, 0.238686681f, -0.106820084f }, { 3000, 0.131528616f, 0.0665975735f }, { 5000, 9.94473215e-09f, -8.22819057e-09f },
                                { 11000, 0.0f, 0.0f } });
    // Delay 2: default, ping pong with a stereo offset, swapped without dry (Fruity Delay 2_x64.dll)
    o = run2 ({ 0, 128, 128, 64, 36, 0, 0, 128 }, 120.0);
    check (o.first, o.second, { { 3000, 0.211380005f, 0.277908325f }, { 5000, -0.223615795f, -0.0189359933f }, { 9000, 0.120054141f, -0.00297877868f },
                                { 16000, 0.0182533264f, 0.105384827f } });
    o = run2 ({ -40, 110, 90, 90, 24, -300, 2, 90 }, 100.0);
    check (o.first, o.second, { { 3000, 0.0929639712f, 0.122222826f }, { 5000, -0.0884733275f, -0.0507739782f }, { 11000, 0.0589251742f, 0.0f },
                                { 16000, -0.029524127f, 0.0128910914f } });
    o = run2 ({ 60, 140, 0, 100, 30, 250, 1, 110 }, 120.0);
    check (o.first, o.second, { { 1500, 0.0f, 0.0f }, { 5000, 0.176379532f, -0.212506086f }, { 8000, -0.324207425f, 0.0675852001f },
                                { 13000, 0.145023212f, -0.104788139f } });

    // the mixer effects from FL states: time law (Delay 3 synced 192 = 1 beat; Delay 2 144 = 3/4 beat), settled start
    auto state3 = std::vector<std::uint8_t> (4 + 26 * 4, 0);
    const std::array<int, 26> def3 { 6000, 1, 1, 0, 192, 3000, 500, 6000, 3000, 0, 0, 0, 0, 1, 0, 0, 6000, 0, 0, 0, 0, 60000, 6000, 6000, 0, 6000 };
    state3[0] = 1;
    std::memcpy (state3.data() + 4, def3.data(), 26 * 4);
    const auto slot3 = lynx::effectFromFl ("Fruity Delay 3", state3, 120.0);
    CHECK (slot3 && slot3->kind == lynx::EffectKind::fruityDelay3);
    auto state2 = std::vector<std::uint8_t> (32, 0);
    const std::array<int, 8> def2 { 0, 128, 128, 0, 144, 0, 0, 128 };
    std::memcpy (state2.data(), def2.data(), 32);
    const auto slot2 = lynx::effectFromFl ("Fruity delay 2", state2, 120.0);
    CHECK (slot2 && slot2->kind == lynx::EffectKind::fruityDelay2);
    CHECK (lynx::formatEffectParam (lynx::EffectKind::fruityDelay2, 4, slot2->params[4], 120.0) == "3:00");
    auto echoAt = [] (const lynx::EffectSlot& slot, double bpm) {
        auto fx = lynx::makeEffect (slot);
        fx->prepare (44100.0);
        std::vector<float> l (48000, 0.0f), r (48000, 0.0f);
        l[100] = 1.0f;
        for (int s = 0; s < 48000; s += 1024)
            fx->process (l.data() + s, r.data() + s, std::min (1024, 48000 - s), slot.params, bpm);
        int best = 200;
        for (int i = 200; i < 48000; ++i)
            if (std::abs (l[(std::size_t) i]) > std::abs (l[(std::size_t) best]))
                best = i;
        return best - 100;
    };
    if (slot3 && slot2)
    {
        CHECK (echoAt (*slot3, 120.0) == 22050);
        CHECK (echoAt (*slot3, 150.0) == 17640);
        CHECK (echoAt (*slot2, 120.0) == 16538);
        CHECK (echoAt (*slot2, 140.0) == 14175);
    }
}
#include "UiRackMixerTests.inc"

// ---------------------------------------------------------------------------
// FL Studio integration (core/FlPluginDb.h): synthetic plugin-database fixtures, no Image-Line files

namespace
{
std::vector<std::uint8_t> testWrapperBlob (std::uint32_t type, const std::string& name, const std::string& path,
                                           const std::vector<std::uint8_t>& classId = {}, const std::string& clapId = {})
{
    std::vector<std::uint8_t> b;
    auto u32 = [&b] (std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back ((std::uint8_t) (v >> (8 * i))); };
    auto record = [&] (std::uint32_t id, const std::vector<std::uint8_t>& payload) {
        u32 (id);
        u32 ((std::uint32_t) payload.size());
        u32 (0);
        b.insert (b.end(), payload.begin(), payload.end());
    };
    u32 (12);
    record (0x32, { (std::uint8_t) type, 0, 0, 0 });
    if (! classId.empty())
        record (0x34, classId);
    if (! clapId.empty())
        record (0x3A, std::vector<std::uint8_t> (clapId.begin(), clapId.end()));
    record (0x36, std::vector<std::uint8_t> (name.begin(), name.end()));
    record (0x37, std::vector<std::uint8_t> (path.begin(), path.end()));
    return b;
}
} // namespace

TEST (fl_plugin_db_nfo_parsing)
{
    const std::string nfo = "ps_name=Vital\r\n"
                            "ps_file_name_0=C:\\Program Files\\Common Files\\VST3\\Vital.vst3\r\n"
                            "ps_file_vendorname_0=Vital Audio\r\n"
                            "ps_file_type_0=2\r\n"
                            "ps_file_name_1=C:\\VSTPlugins\\Vital_x64.dll\r\n"
                            "ps_file_type_1=2\r\n"
                            "ps_file_magic_1=Vita\r\n"
                            "ps_file_name_2=not a plugin\r\n"
                            "garbage line without equals\r\n";
    const auto entries = lynx::parseFlPluginNfo (nfo, "D:\\FL\\Presets\\Plugin database\\Installed\\Generators\\VST3\\Vital.nfo");
    CHECK (entries.size() == 2);
    if (entries.size() == 2)
    {
        CHECK (entries[0].name == "Vital" && entries[0].vendor == "Vital Audio" && entries[0].role == 2);
        CHECK (entries[0].format == lynx::ForeignPlugin::Format::vst3);
        CHECK (entries[1].format == lynx::ForeignPlugin::Format::vst2 && entries[1].vst2Id == 0x56697461u);
    }

    // no type field: the role comes from the Effects / Generators folder; an ID in GUID form is read
    const auto fx = lynx::parseFlPluginNfo ("ps_name=Fancy Comp\nps_file_filename_0=C:/CLAP/Fancy Comp.clap\n"
                                            "ps_file_guid_0={01234567-89AB-CDEF-0123-456789ABCDEF}\n",
                                            "C:\\x\\Installed\\Effects\\CLAP\\Fancy Comp.nfo");
    CHECK (fx.size() == 1 && fx[0].role == 1 && fx[0].format == lynx::ForeignPlugin::Format::clap && fx[0].hasClassId
           && fx[0].classId[0] == 0x01 && fx[0].classId[15] == 0xEF);
    CHECK (lynx::parseFlPluginNfo ("Tip=All of the plugins found in your system.").empty());

    CHECK (lynx::pluginNameKey ("Serum_x64") == "serum");
    CHECK (lynx::pluginNameKey ("Vital.VST3") == "vital");
    CHECK (lynx::pluginNameKey ("Pro-Q 3") == "proq3");
    CHECK (lynx::pluginFormatFromPath ("a\\b.CLAP") == lynx::ForeignPlugin::Format::clap);
}

TEST (fl_plugin_db_fst_parsing_and_matching)
{
    std::vector<std::uint8_t> cid;
    for (int i = 0; i < 16; ++i)
        cid.push_back ((std::uint8_t) (0x10 + i));

    // an .fst of the database: an FL preset whose 0xD5 holds the wrapper record
    FlpBuilder fst;
    fst.ascii (0xC9, "Fruity Wrapper");
    fst.data (0xD5, testWrapperBlob (8, "Surge XT", "E:\\Plugins\\VST3\\Surge XT.vst3", cid));
    lynx::FlPluginDbEntry e;
    CHECK (lynx::parseFlPluginFst (fst.bytes, e));
    CHECK (e.name == "Surge XT" && e.format == lynx::ForeignPlugin::Format::vst3 && e.hasClassId && e.classId[0] == 0x10);
    lynx::FlPluginDbEntry bad;
    CHECK (! lynx::parseFlPluginFst (std::vector<std::uint8_t> { 'F', 'L', 'h', 'd' }, bad));

    lynx::FlPluginDb db;
    db.add (e);
    // the .nfo of the same plugin fills in vendor and role
    for (const auto& n : lynx::parseFlPluginNfo ("ps_name=Surge XT\nps_file_name_0=E:/Plugins/VST3/Surge XT.vst3\n"
                                                 "ps_file_vendorname_0=Surge Synth Team\nps_file_type_0=2\n"))
        db.add (n);
    db.add ({ "Clapper", "", "E:\\Plugins\\CLAP\\Clapper.clap", "", lynx::ForeignPlugin::Format::clap, 1, {}, false, "org.clapper", 0 });
    db.add ({ "Big Verb", "", "E:\\Old VST\\BigVerb_x64.dll", "", lynx::ForeignPlugin::Format::vst2, 1, {}, false, "", 0x42566262u });
    db.add ({ "Bundle", "", "F:\\VST3\\Bundle.vst3\\Contents\\x86_64-win\\Bundle.vst3", "", lynx::ForeignPlugin::Format::vst3, 2, {}, false, "", 0 });
    CHECK (db.entries().size() == 4);
    CHECK (db.entries()[0].vendor == "Surge Synth Team" && db.entries()[0].role == 2);

    // by VST3 class ID even when the project saved another path and name
    lynx::ForeignPlugin p;
    p.format = lynx::ForeignPlugin::Format::vst3;
    p.hasClassId = true;
    std::copy (cid.begin(), cid.end(), p.classId.begin());
    p.name = "Renamed";
    p.path = "C:\\Other PC\\Surge.vst3";
    CHECK (db.match (p, true) == &db.entries()[0]);
    // by CLAP id, by VST2 id
    lynx::ForeignPlugin c;
    c.format = lynx::ForeignPlugin::Format::clap;
    c.clapId = "org.clapper";
    CHECK (db.match (c, false) == &db.entries()[1]);
    lynx::ForeignPlugin v;
    v.format = lynx::ForeignPlugin::Format::vst2;
    v.vst2Id = 0x42566262u;
    CHECK (db.match (v, false) == &db.entries()[2]);
    // by name (architecture suffix ignored), only in the same role
    lynx::ForeignPlugin byName;
    byName.format = lynx::ForeignPlugin::Format::vst2;
    byName.name = "BigVerb x64";
    CHECK (db.match (byName, false) == &db.entries()[2]);
    CHECK (db.match (byName, true) == nullptr);
    lynx::ForeignPlugin unknown;
    unknown.format = lynx::ForeignPlugin::Format::vst3;
    unknown.name = "Nothing Like It";
    CHECK (db.match (unknown, true) == nullptr);

    // folders: the bundle's parent, deduplicated
    const auto folders = db.pluginFolders();
    CHECK (folders.size() == 4);
    CHECK (std::find (folders.begin(), folders.end(), "F:\\VST3") != folders.end());
    CHECK (std::find (folders.begin(), folders.end(), "E:\\Plugins\\VST3") != folders.end());

    // targeted scan picks: same file, or same name key
    CHECK (lynx::pluginFileMatches ("E:/Plugins/VST3/Surge XT.vst3", p, &db.entries()[0]));
    CHECK (lynx::pluginFileMatches ("C:\\Program Files\\Common Files\\VST3\\BigVerb.vst3", byName, nullptr));
    CHECK (! lynx::pluginFileMatches ("C:\\VST3\\Other.vst3", byName, nullptr));

    // the plugins an .flp uses: wrapper channels and mixer slots
    flp::Project project;
    flp::Channel ch;
    ch.pluginName = "Fruity Wrapper";
    ch.pluginState = testWrapperBlob (8, "Surge XT", "C:\\VST3\\Surge XT.vst3", cid);
    project.channels.push_back (ch);
    ch.pluginName = "3x Osc";
    project.channels.push_back (ch);
    flp::MixerInsert insert;
    insert.slots.push_back ({ 0, "Fruity Wrapper", testWrapperBlob (12, "Clapper", "C:\\CLAP\\Clapper.clap", {}, "org.clapper") });
    insert.slots.push_back ({ 1, "Fruity Wrapper", testWrapperBlob (3, "DX thing", "C:\\dx.dll") }); // DirectX: not hosted
    project.mixer.push_back (insert);
    const auto uses = lynx::flpForeignPlugins (project);
    CHECK (uses.size() == 2);
    CHECK (uses.size() == 2 && uses[0].instrument && ! uses[1].instrument && uses[1].plugin.clapId == "org.clapper");
}

TEST (fl_sample_relocation)
{
    std::set<std::string> files { "D:\\Docs\\Image-Line\\FL Studio\\Audio\\Recorded\\take.wav", "C:\\Program Files\\FL 2026\\Data\\Patches\\kick.wav",
                                  "C:\\Users\\me\\Music\\loop.wav", "E:\\Songs\\Track\\Samples\\snare.wav", "E:\\Songs\\Track\\hat.wav" };
    auto exists = [&] (const std::string& p) { return files.count (p) > 0; };
    lynx::FlPathRoots roots;
    roots.userData = "D:\\Docs\\Image-Line\\FL Studio";
    roots.factoryData = "C:\\Program Files\\FL 2026";
    roots.home = "C:\\Users\\me";
    const std::string project = "E:\\Songs\\Track";

    // variable resolved, file in place
    CHECK (lynx::locateFlSample ("%FLStudioFactoryData%\\Data\\Patches\\kick.wav", roots, project, exists) == "C:\\Program Files\\FL 2026\\Data\\Patches\\kick.wav");
    // another PC's FL user data / FL install / user profile
    CHECK (lynx::locateFlSample ("C:\\Users\\bob\\Documents\\Image-Line\\FL Studio\\Audio\\Recorded\\take.wav", roots, project, exists)
           == "D:\\Docs\\Image-Line\\FL Studio\\Audio\\Recorded\\take.wav");
    CHECK (lynx::locateFlSample ("C:\\Program Files (x86)\\Image-Line\\FL Studio 20\\Data\\Patches\\kick.wav", roots, project, exists)
           == "C:\\Program Files\\FL 2026\\Data\\Patches\\kick.wav");
    CHECK (lynx::locateFlSample ("C:/Users/bob/Music/loop.wav", roots, project, exists) == "C:\\Users\\me\\Music\\loop.wav");
    // next to the project, also in a sub-folder named in the original path
    CHECK (lynx::locateFlSample ("Z:\\Old\\Samples\\snare.wav", roots, project, exists) == "E:\\Songs\\Track\\Samples\\snare.wav");
    CHECK (lynx::locateFlSample ("Z:\\Old\\Stuff\\hat.wav", roots, project, exists) == "E:\\Songs\\Track\\hat.wav");
    CHECK (lynx::locateFlSample ("Z:\\nowhere.wav", roots, project, exists).empty());
    CHECK (lynx::locateFlSample ("", roots, project, exists).empty());
}

// --- REA round 64: SimSynth (exact engine) and Wasp / Wasp XT (FL 2026's silent placeholder) ---------------------
namespace
{
// a SimSynth state written from scratch (REA_rodada64 §1.1 layout: 45 doubles, 19 unused ints, 19 switches, 12 unused
// ints, version)
std::vector<std::uint8_t> simSynthTestState (const std::array<double, 45>& P, const std::array<int, 19>& S, int version = 14)
{
    std::vector<std::uint8_t> b (564, 0);
    std::memcpy (b.data(), P.data(), 360);
    std::memcpy (b.data() + 360 + 76, S.data(), 76);
    std::memcpy (b.data() + 560, &version, 4);
    return b;
}
// the 0xD5 records of an FL preset file (.fst: the FLhd/FLdt event stream) that follow a plugin name `plugin`
std::vector<std::vector<std::uint8_t>> pluginStatesIn (const std::filesystem::path& file, const std::string& plugin)
{
    std::vector<std::vector<std::uint8_t>> out;
    std::ifstream in (file, std::ios::binary);
    const std::vector<std::uint8_t> b ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    std::size_t i = 0;
    while (i + 8 <= b.size() && std::memcmp (b.data() + i, "FLdt", 4) != 0)
        ++i;
    i += 8;
    std::string name;
    while (i < b.size())
    {
        const auto id = b[i++];
        std::size_t n = id < 64 ? 1 : id < 128 ? 2 : id < 192 ? 4 : 0;
        if (id >= 192)
            for (int shift = 0; i < b.size(); shift += 7)
            {
                const auto c = b[i++];
                n |= (std::size_t) (c & 0x7F) << shift;
                if (c < 0x80)
                    break;
            }
        if (i + n > b.size())
            break;
        if (id == 0xC9)
        {
            name.clear();
            for (std::size_t k = 0; k < n; ++k)
                if (b[i + k] != 0)
                    name += (char) b[i + k]; // ASCII or UTF-16LE (zero bytes skipped)
        }
        if (id == 0xD5 && name == plugin)
            out.emplace_back (b.begin() + (std::ptrdiff_t) i, b.begin() + (std::ptrdiff_t) (i + n));
        i += n;
    }
    return out;
}
} // namespace

TEST (simsynth_engine)
{
    // A: pulse + octave layer, half-cosine with unison and two octave layers, sine/sine⁴ mix, square LFO → pitch with
    //    delay, filter envelope, LP/BP mix, chorus. B (version 7): triangle ring-modulated by filtered noise, sample &
    //    hold LFO → cutoff, retrigger, own filter envelope, chorus off
    const std::array<double, 45> A = { 0.3, 0.5, 0.55, 0.8, 0.6, 0.5, 0.2, 0.5, 0.75, 0.45, 0.6, 0.5, 0.5, 0.1, 0.7, 0.25, 0.5, 0.5, 0.5, 0.7, 0.3, 0.3, 0.8,
                                       0.5, 0.2, 0.0, 0.35, 0.55, 0.8, 0.6, 0.4, 0.0, 0.3, 0.2, 0.0, 0.2, 0.5, 0.6, 0.4, 0.7, 1.0, 0.3, 0.4, 0.5, 0.3 };
    const std::array<int, 19> SA = { 1, 1, 0, 0, 1, 0, 1, 1, 1, 0, 0, 0, 1, 0, 1, 0, 0, 0, 1 };
    const std::array<double, 45> B = { 0.5, 0.5, 0.5, 0.9, 0.5, 0.5, 0.0, 0.5, 0.6, 0.5, 0.5, 0.5, 0.5, 0.4, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.0, 0.6,
                                       0.5, 0.4, 0.0, 0.5, 0.8, 0.6, 0.85, 0.0, 0.0, 0.0, 0.0, 0.0, 0.1, 0.3, 0.2, 0.5, 0.6, 1.0, 0.0, 0.5, 0.0, 0.0 };
    const std::array<int, 19> SB = { 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0 };
    const auto blobA = simSynthTestState (A, SA), blobB = simSynthTestState (B, SB, 7);
    dumpIfAsked ("LYNX_DUMP_SIMSYNTH", blobA);
    CHECK (lynx::simSynthStateUsable (blobA) && ! lynx::simSynthStateUsable (std::vector<std::uint8_t> (blobA.begin(), blobA.begin() + 563)));
    const auto hp = lynx::simSynthHostParams (blobA);
    // knobs go through ⌊65535·v⌋, the coarse pitches through 48 steps, the switches and wave selectors as they are
    CHECK (hp.size() == 58u && hp[0] == 52428 && hp[27] == 10080 && hp[41] == 1 && hp[54] == 0 && hp[56] == 4);

    // golden samples of the installation's SimSynth_x64.dll (round-64 EXEC harness: created, state restored, 44.1 kHz,
    // host blocks of 512, old voice record) — LYNX's engine renders bit-identically: C5 0–30000, E5 Final.Vol 0.8 pan
    // 16 11025–40000, G5 pan −20 +25 cents 22050–52000
    const std::vector<lynx::simsynth::RefNote> notes = { { 0, 60, 1.0f, 0, 0, 0.0f, 0.0f, 30000 },
                                                         { 11025, 64, 0.8f, 16, 0, 0.0f, 0.0f, 40000 },
                                                         { 22050, 67, 1.0f, -20, 25, 0.0f, 0.0f, 52000 } };
    struct Golden { int i; float l, r; };
    static const Golden goldenA[] = { { 100, 0.0f, 0.000322219421f }, { 5000, -0.110216402f, -0.194837824f }, { 15000, 0.517085433f, -0.551030278f }, { 25000, -0.443528295f, 0.226643473f }, { 35000, -0.708713174f, -0.291932464f }, { 45000, -0.0335822627f, -0.00557783246f }, { 55000, 0.0926972628f, 0.076306425f }, { 66000, 0.0415042415f, -0.0125555545f } };
    static const Golden goldenB[] = { { 100, 0.00706971437f, 0.00706971437f }, { 5000, -0.0311671626f, -0.0311671626f }, { 15000, 0.508298159f, 0.964286387f }, { 25000, -3.07005596f, -1.8650744f }, { 35000, -0.23775965f, -0.14130348f }, { 45000, -0.00415330986f, -0.00409662398f }, { 55000, 0.00124766713f, -0.00180800457f }, { 66000, 0.0196316559f, 0.0166722424f } };
    for (int which = 0; which < 2; ++which)
    {
        std::vector<std::int64_t> counters;
        const auto y = lynx::simsynth::renderReference (which == 0 ? blobA : blobB, notes, 44100.0, 66150, 512, &counters);
        CHECK (y.size() == 2 * 66150u);
        if (y.size() != 2 * 66150u)
            continue;
        // the random-table index after creation / state / rate set equals the DLL's (276 / 696 / 756 and 276 / 492 / 528)
        CHECK (counters.size() == 4u && counters[2] == 276 && counters[3] == (which == 0 ? 696 : 492) && counters[0] == (which == 0 ? 756 : 528));
        for (const auto& g : which == 0 ? std::vector (std::begin (goldenA), std::end (goldenA)) : std::vector (std::begin (goldenB), std::end (goldenB)))
            CHECK (y[2 * (std::size_t) g.i] == g.l && y[2 * (std::size_t) g.i + 1] == g.r);
        double e = 0.0;
        for (auto v : y)
            e += (double) v * v;
        CHECK (std::abs (std::sqrt (e / (double) y.size()) - (which == 0 ? 0.290665331870 : 0.992116840643)) < 1e-9);
    }

    // the instrument: same engine behind LYNX's channel gains (V = 1, centre → Final.Vol 1, Final.Pan 0)
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::simsynth;
    c.pluginState = blobA;
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::simsynth);
    inst->prepare (44100.0);
    inst->configure (c, 120.0);
    std::vector<float> l (8192, 0.0f), r (8192, 0.0f);
    for (int k = 0; k < 16; ++k)
    {
        if (k == 0)
            inst->noteOn (60, 0.8f);
        if (k == 10)
            inst->noteOff (60);
        inst->render (l.data() + 512 * k, r.data() + 512 * k, 512, 1.0f, 1.0f);
    }
    const auto ref = lynx::simsynth::renderReference (blobA, { { 0, 60, 1.0f, 0, 0, 0.0f, 0.0f, 5120 } }, 44100.0, 8192);
    double diff = 0.0, energy = 0.0;
    for (int i = 0; i < 8192; ++i)
    {
        diff = std::max ({ diff, (double) std::abs (l[(std::size_t) i] - ref[2 * (std::size_t) i]), (double) std::abs (r[(std::size_t) i] - ref[2 * (std::size_t) i + 1]) });
        energy += (double) l[(std::size_t) i] * l[(std::size_t) i];
    }
    CHECK (diff == 0.0 && energy > 1e-3);

    // the installation's SimSynth presets (when the FL data folder is given): every state usable and sounding
    if (! realFlpDir.empty())
    {
        const auto folder = std::filesystem::path (realFlpDir) / "Patches" / "Plugin presets" / "Generators" / "SimSynth";
        int presets = 0, usable = 0, sounding = 0;
        if (std::filesystem::exists (folder))
            for (const auto& entry : std::filesystem::directory_iterator (folder))
                for (const auto& st : pluginStatesIn (entry.path(), "SimSynth"))
                {
                    ++presets;
                    if (! lynx::simSynthStateUsable (st))
                        continue;
                    ++usable;
                    const auto y = lynx::simsynth::renderReference (st, { { 0, 48, 1.0f, 0, 0, 0.0f, 0.0f, 11025 } }, 44100.0, 22050);
                    double e = 0.0;
                    bool finite = true;
                    for (auto v : y)
                        finite = finite && std::isfinite (v), e += (double) v * v;
                    CHECK (finite);
                    sounding += e > 1e-9 ? 1 : 0;
                }
        std::printf ("  SimSynth presets: %d, %d usable, %d sounding\n", presets, usable, sounding);
        CHECK (presets == 0 || (usable == presets && presets >= 70 && sounding >= presets - 2));
    }
}

TEST (wasp_is_a_silent_placeholder)
{
    // FL 2026's Wasp / Wasp XT DLL keeps the state and plays nothing: so does LYNX (REA_rodada64 §5)
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::wasp);
    inst->prepare (44100.0);
    std::vector<float> l (1024, 0.0f), r (1024, 0.0f);
    inst->noteOn (60, 1.0f);
    inst->render (l.data(), r.data(), 1024, 1.0f, 1.0f);
    CHECK (inst->idle() && std::all_of (l.begin(), l.end(), [] (float v) { return v == 0.0f; }));

    // import: a SimSynth channel plays on the exact engine, Wasp and Wasp XT channels become the silent placeholder with
    // their state kept
    std::vector<std::uint8_t> ss (564, 0), wasp (169, 1);
    const int version = 14;
    std::memcpy (ss.data() + 560, &version, 4);
    FlpBuilder b;
    b.ascii (0xC7, "25.2.5.5319");
    int index = 0;
    for (const auto& [name, state] : { std::pair<std::u16string, std::vector<std::uint8_t>> { u"SimSynth", ss }, { u"Wasp", wasp }, { u"Wasp XT", wasp } })
    {
        b.u8 (0x40); b.u16 ((std::uint16_t) index++);
        b.u8 (0x15); b.u8 (2);
        b.utf16 (0xC9, name);
        b.data (0xD5, state);
    }
    const auto song = lynx::importFlp (flp::parseProject (b.bytes));
    CHECK (song.channels.size() == 3u);
    if (song.channels.size() == 3u)
    {
        CHECK (song.channels[0].instrument == lynx::InstrumentKind::simsynth && song.channels[0].pluginState == ss);
        CHECK (song.channels[1].instrument == lynx::InstrumentKind::wasp && song.channels[1].pluginState == wasp);
        CHECK (song.channels[2].instrument == lynx::InstrumentKind::wasp);
    }
}
// --- REA round 67: Fruity Granulizer (exact engine) --------------------------------------------------------------
namespace
{
// our own deterministic test sample (a fading 220 Hz sine plus LCG noise per channel), as r67/py/gold67.py builds it
lynx::SampleData granulizerTestSample (int n, int ch)
{
    lynx::SampleData d;
    std::uint32_t s = 12345;
    for (int i = 0; i < n; ++i)
    {
        const auto base = 0.5 * std::sin (2.0 * 3.141592653589793 * 220.0 * i / 44100.0) * (1.0 - 0.7 * i / n);
        for (int c = 0; c < ch; ++c)
        {
            s = s * 1664525u + 1013904223u;
            const auto v = (float) (base + 0.2 * ((double) (std::int32_t) s * (1.0 / 2147483648.0)));
            (c == 0 ? d.left : d.right).push_back (v);
        }
    }
    return d;
}
std::vector<std::uint8_t> granulizerTestState (const std::array<int, 12>& P, int keyMode = 0)
{
    std::vector<std::uint8_t> b (60, 0);
    const std::int32_t v = 5;
    std::memcpy (b.data(), &v, 4);
    std::memcpy (b.data() + 4, P.data(), 48);
    std::memcpy (b.data() + 56, &keyMode, 4);
    return b;
}
} // namespace

TEST (granulizer_engine)
{
    // REA_rodada67: states written from scratch against our reference model (granmodel.py, which renders like the
    // installation's DLL within −128 dB: the DLL's rcpps and IPP sines are not replicated)
    const auto v5 = granulizerTestState ({ 204, 50, 611, 611, 0, 180, 0, 0, 0, 0, 0, 228 });
    CHECK (lynx::granulizerStateUsable (v5));
    const auto n5 = lynx::granulizerNormalisedState (v5, {});
    CHECK (n5.size() == 428 && n5[0] == 7 && lynx::granulizerParam (n5, 0) == 204 && lynx::granulizerParam (n5, 11) == 228
           && ! lynx::granulizerParam (n5, 12));
    // version 1 (10 parameters): parameter 11 takes the plugin's 0xf8
    {
        std::vector<std::uint8_t> v1 (v5.begin(), v5.begin() + 44);
        v1[0] = 1;
        const auto n1 = lynx::granulizerNormalisedState (v1, {});
        CHECK (lynx::granulizerParam (n1, 9) == 0 && lynx::granulizerParam (n1, 11) == 0xf8 && lynx::granulizerParam (n1, 3) == 611);
        std::vector<std::uint8_t> bad (v5);
        bad[0] = 9;
        CHECK (! lynx::granulizerStateUsable (bad) && lynx::granulizerNormalisedState (bad, {}).empty());
        auto edited = n5;
        CHECK (lynx::granulizerSetParam (edited, 3, -5000) && lynx::granulizerParam (edited, 3) == -1000);
    }

    using N = lynx::granulizer::RefNote;
    struct Golden { std::vector<std::uint8_t> state; int ch, fs, rate; lynx::granulizer::RefOptions opt; std::vector<N> notes;
                    double el, er; std::tuple<int, float, float> at[6]; };
    auto check = [] (const Golden& g) {
        auto smp = granulizerTestSample (20000, g.ch);
        smp.sampleRate = g.rate;
        const auto y = lynx::granulizer::renderReference (lynx::granulizerNormalisedState (g.state, {}), smp, g.notes, (double) g.fs, 16384, g.opt);
        CHECK (y.size() == 2 * 16384);
        if (y.size() != 2 * 16384)
            return;
        double el = 0.0, er = 0.0;
        for (std::size_t i = 0; i + 1 < y.size(); i += 2)
            el += (double) y[i] * y[i], er += (double) y[i + 1] * y[i + 1];
        CHECK (std::abs (el - g.el) <= 1e-6 * g.el && std::abs (er - g.er) <= 1e-6 * g.er);
        for (const auto& [i, l, r] : g.at)
            CHECK (goldenSampleClose (y[2 * (std::size_t) i], l, granulizerTolerance())
                   && goldenSampleClose (y[2 * (std::size_t) i + 1], r, granulizerTolerance()));
    };
    lynx::granulizer::RefOptions o1, o2, o3;
    o2.block = 333, o2.seed = 777;
    o3.block = 64, o3.seed = 5;
    // the plugin's defaults, two notes (one pitched by +23 cents and released: the processor's end fade)
    check ({ v5, 2, 44100, 44100, o1, { { 0, 60, 0.8, 0.0, 0.0, -1 }, { 3000, 64, 0.6, -0.3, 23.0, 9000 } }, 553.4541810458171, 470.3837027429263,
             { { 100, 0.057010360062122345f, 0.08960580080747604f }, { 1000, 0.07210570573806763f, -0.048257164657115936f },
               { 2500, 0.14769265055656433f, -0.0194698516279459f }, { 4000, 0.11169229447841644f, 0.17487481236457825f },
               { 7777, -0.4309714436531067f, -0.17027156054973602f }, { 12000, -0.11129666864871979f, -0.19622492790222168f } } });
    // backwards, randomness, depth/speed wobble, stereo separation, loop, sample start; a mono 44.1 kHz sample at 48 kHz
    check ({ granulizerTestState ({ 150, 120, 300, -480, 200, 90, 120, 40, 1, 0, 900, 300 }), 1, 48000, 44100, o2,
             { { 0, 55, 1.0, 0.2, -37.0, 12000 }, { 5000, 67, 0.5, 0.0, 0.0, -1 } }, 959.5011214226065, 1294.4823684530272,
             { { 100, 0.02567935362458229f, 0.010721667669713497f }, { 1000, 0.3007732629776001f, 0.08214527368545532f },
               { 2500, 0.435413658618927f, 0.09068503230810165f }, { 4000, -0.24611327052116394f, -0.20149046182632446f },
               { 7777, -0.008478119969367981f, 0.31221628189086914f }, { 12000, 0.203709632f, 0.324944794f } } });
    // key mode 1 (the sample spread over two octaves), freeze, hard stereo separation; 48 kHz sample at 96 kHz, 64-sample blocks
    check ({ granulizerTestState ({ 300, 0, 800, 700, 0, 180, 0, 64, 0, 1, 2000, 100 }, 1), 2, 96000, 48000, o3,
             { { 0, 62, 0.9, 0.0, 0.0, -1 }, { 4000, 50, 0.7, 0.0, 11.0, -1 } }, 366.79556669810813, 0.00825590391720353,
             { { 100, 0.004204070195555687f, 7.680895437786243e-20f }, { 1000, 0.008516596630215645f, 3.0263170437258543e-19f },
               { 2500, 0.06948182731866837f, 4.5391312775082004e-18f }, { 4000, -0.01754589192569256f, -4.895453905423713e-19f },
               { 7777, 0.10338862985372543f, 2.344363859234514e-18f }, { 12000, -0.09385982155799866f, -1.3493021437162966e-17f } } });

    // the instrument: silent without a sample (FL's plugin refuses the note), sounding with one
    {
        auto silent = lynx::makeInstrument (lynx::InstrumentKind::granulizer);
        silent->prepare (44100.0);
        std::vector<float> l (2048, 0.0f), r (2048, 0.0f);
        silent->noteOn (60, 1.0f);
        silent->render (l.data(), r.data(), 2048, 1.0f, 1.0f);
        CHECK (silent->idle() && std::all_of (l.begin(), l.end(), [] (float v) { return v == 0.0f; }));
        auto smp = std::make_shared<lynx::SampleData> (granulizerTestSample (20000, 2));
        auto inst = lynx::makeInstrument (lynx::InstrumentKind::granulizer, smp);
        lynx::ChannelDef def;
        def.instrument = lynx::InstrumentKind::granulizer;
        def.pluginState = n5;
        inst->configure (def, 140.0);
        inst->prepare (44100.0);
        inst->noteOn (60, 0.8f);
        inst->render (l.data(), r.data(), 2048, 1.0f, 1.0f);
        double e = 0.0;
        for (auto v : l)
            e += (double) v * v;
        CHECK (! inst->idle() && e > 1.0);
        inst->noteOff (60);
        for (int k = 0; k < 4; ++k)
            inst->render (l.data(), r.data(), 512, 1.0f, 1.0f);
        CHECK (inst->idle()); // the 10 ms end fade of the plugin's voice processor
    }

    // import: a Fruity Granulizer channel plays on the exact engine with its sample
    {
        FlpBuilder b;
        b.ascii (0xC7, "25.2.5.5319");
        b.u8 (0x40); b.u16 (0);
        b.u8 (0x15); b.u8 (2);
        b.utf16 (0xC9, u"Fruity Granulizer");
        b.data (0xD5, v5);
        b.utf16 (0xC4, u"%FLStudioFactoryData%\\Data\\Patches\\Packs\\Drums\\Kicks\\808 Kick.wav");
        lynx::FlpImportOptions options;
        options.roots.factoryData = "C:\\FL";
        options.fileExists = [] (const std::string& path) { return path == "C:\\FL\\Data\\Patches\\Packs\\Drums\\Kicks\\808 Kick.wav"; };
        lynx::FlpImportReport report;
        options.report = &report;
        const auto song = lynx::importFlp (flp::parseProject (b.bytes), options);
        CHECK (song.channels.size() == 1u);
        if (song.channels.size() == 1u)
        {
            CHECK (song.channels[0].instrument == lynx::InstrumentKind::granulizer && song.channels[0].pluginState == n5);
            CHECK (song.channels[0].samplePath == "C:\\FL\\Data\\Patches\\Packs\\Drums\\Kicks\\808 Kick.wav");
        }
        CHECK (report.converted["Fruity Granulizer"] == 1);
    }
}
// --- REA round 67: Drumaxx (exact engine) -------------------------------------------------------------------------
namespace
{
// our own deterministic 16-pad kit (LCG-chosen parameters), as r67/py/gold67d.py builds it for the DLL
std::vector<std::uint8_t> drumaxxTestState (bool eq)
{
    std::uint32_t s = 20261015u;
    auto u = [&s] {
        s = s * 1664525u + 1013904223u;
        return (float) ((double) (s >> 8) * (1.0 / 16777216.0));
    };
    // (a product rounded on its own: no fused multiply-add)
    auto mul = [] (float a, float b) {
        volatile float p = a * b;
        return (float) p;
    };
    static const int targets[] = { 0, 9, 10, 16, 21, 23, 26 };
    std::array<float, 720> P {};
    for (int k = 0; k < 16; ++k)
        for (int i = 0; i < 44; ++i)
        {
            auto v = u();
            if (i == 0)
                v = 0.5f + mul (v, 0.5f);
            else if ((i >= 7 && i <= 18) || (i >= 20 && i <= 34))
                v = 0.2f + mul (v, 0.6f);
            else if (i == 2 || i == 3)
                v = 0.0f;
            else if (i == 4)
                v = (float) (36 + k);
            else if (i == 5)
                v = v < 0.5f ? 0.0f : 1.0f;
            else if (i == 6)
                v = (float) (int) (v * 4.0f);
            else if (i >= 35 && i <= 38)
                v = (float) targets[(int) (v * 7.0f)];
            P[(std::size_t) (44 * k + i)] = v;
        }
    for (int i = 0; i < 16; ++i)
    {
        auto v = u();
        if (i == 1)
            v = 0.3f + mul (v, 0.4f);
        else if (i == 3)
            v = eq ? 1.0f : 0.0f;
        else if (i >= 4 && i <= 11)
            v = 0.35f + mul (v, 0.3f);
        else if (i == 12)
            v = 1.0f;
        else if (i == 13)
            v = v * 0.1f;
        else if (i == 0 || i == 15)
            v = 0.0f;
        P[(std::size_t) (704 + i)] = v;
    }
    std::vector<std::uint8_t> b;
    auto put32 = [&b] (std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back ((std::uint8_t) (v >> (8 * i))); };
    b.insert (b.end(), { 'S', 'S', 'L', 'F' });
    put32 (21636u);
    put32 (0x444d5333u); // engine stream version 3
    const std::string name = "LYNX test kit";
    b.insert (b.end(), name.begin(), name.end());
    b.resize (b.size() + 256 - name.size(), 0);
    const auto at = b.size();
    b.resize (at + 720 * 4);
    std::memcpy (b.data() + at, P.data(), 720 * 4);
    for (int k = 0; k < 16; ++k)
    {
        b.resize (b.size() + 0x400, 0);
        b.push_back (100), b.push_back (100); // pattern length, trigger key
    }
    b.resize (b.size() + 0x800, 0);
    for (int k = 0; k < 16; ++k)
        b.push_back (k >= 2 && k <= 4 ? 1 : 0); // pads 3…5 in cut group 1
    b.resize (b.size() + 16, 0);                 // every pad on the master
    return b;
}
} // namespace

TEST (drumaxx_engine)
{
    const auto st = drumaxxTestState (true);
    CHECK (st.size() == 21644u && lynx::drumaxxStateUsable (st) && lynx::drumaxxKitName (st) == "LYNX test kit");
    CHECK (lynx::drumaxxParam (st, 4) == 36.0f && lynx::drumaxxParam (st, 44 * 15 + 4) == 51.0f && lynx::drumaxxParam (st, 0x2c3) == 1.0f);
    {
        auto edited = st;
        lynx::drumaxxSetParam (edited, 0x2c1, 0.25f);
        CHECK (lynx::drumaxxParam (edited, 0x2c1) == 0.25f && std::isnan (lynx::drumaxxParam (edited, 720)));
        auto bad = st;
        bad[8] = 'X';
        CHECK (! lynx::drumaxxStateUsable (bad) && std::isnan (lynx::drumaxxParam (bad, 0)));
    }

    // goldens: LYNX's renders, bit-identical to Drumaxx_x64.dll + engine_x64.dll driven by the round-67 harness
    using N = lynx::drumaxx::RefNote;
    struct Golden { bool eq; int fs, block; std::vector<N> notes; double el, er; std::tuple<int, float, float> at[6]; };
    auto check = [] (const Golden& g) {
        const auto y = lynx::drumaxx::renderReference (drumaxxTestState (g.eq), g.notes, (double) g.fs, 20000, g.block);
        CHECK (y.size() == 2 * 20000);
        if (y.size() != 2 * 20000)
            return;
        double el = 0.0, er = 0.0;
        for (std::size_t i = 0; i + 1 < y.size(); i += 2)
            el += (double) y[i] * y[i], er += (double) y[i + 1] * y[i + 1];
        CHECK (std::abs (el - g.el) <= 1e-6 * g.el && std::abs (er - g.er) <= 1e-6 * g.er);
        for (const auto& [i, l, r] : g.at)
            CHECK (std::abs (y[2 * (std::size_t) i] - l) <= 1e-6f * std::max (1e-16f, std::abs (l))
                   && std::abs (y[2 * (std::size_t) i + 1] - r) <= 1e-6f * std::max (1e-16f, std::abs (r)));
    };
    // master EQ and limiter on: velocities, Final.Vol, pitch offsets, the cut group (pads 3…5)
    check ({ true, 44100, 512,
             { { 0, 36, 1.0, 1.0, 0.0 }, { 1500, 38, 0.7, 0.8, 0.0 }, { 3000, 37, 0.5, 1.0, -120.0 }, { 4000, 38, 1.0, 1.0, 0.0 },
               { 5200, 42, 0.9, 1.0, 0.0 }, { 6000, 43, 0.6, 1.0, 35.0 }, { 7000, 44, 1.0, 0.5, 0.0 }, { 9000, 40, 0.8, 1.0, 0.0 },
               { 11000, 45, 1.0, 1.0, 250.0 }, { 12500, 48, 0.4, 1.0, 0.0 }, { 14000, 51, 1.0, 1.0, 0.0 }, { 15000, 39, 1.0, 0.7, -40.0 } },
             2698.372377185949, 3678.285731410973,
             { { 100, 0.0f, 0.0f }, { 1000, -0.0034233136102557182f, -0.005708245560526848f }, { 2500, -0.41297006607055664f, -0.0276018138974905f },
               { 4000, -0.04906991869211197f, -0.7206904292106628f }, { 7777, 0.7544509172439575f, 0.7584841847419739f },
               { 15000, -0.13438990712165833f, -0.6798369884490967f } } });
    // EQ off, 48 kHz in 333-sample blocks, two pads on the same sample
    check ({ false, 48000, 333,
             { { 0, 46, 1.0, 1.0, 0.0 }, { 800, 47, 0.9, 1.0, 0.0 }, { 2600, 41, 1.0, 1.0, -300.0 }, { 5000, 49, 0.75, 1.0, 0.0 },
               { 7000, 50, 1.0, 0.9, 77.0 }, { 9000, 36, 1.0, 1.0, 0.0 }, { 9001, 42, 1.0, 1.0, 0.0 }, { 13000, 43, 0.3, 1.0, 0.0 } },
             2241.008810618806, 1814.5219804412964,
             { { 100, 0.0f, 0.0f }, { 1000, -0.32539352774620056f, -0.7439078688621521f }, { 2500, -0.1711389720439911f, -0.39125433564186096f },
               { 4000, 0.15524788200855255f, 0.3445436358451843f }, { 7777, 0.5472834706306458f, 0.08737402409315109f },
               { 15000, -0.03079034946858883f, -0.06140122935175896f } } });
    // 96 kHz in 64-sample blocks, an octave down
    check ({ true, 96000, 64,
             { { 0, 36, 1.0, 1.0, 0.0 }, { 500, 44, 1.0, 1.0, 0.0 }, { 1000, 38, 0.85, 1.0, 12.0 }, { 6000, 37, 1.0, 1.0, 0.0 },
               { 10000, 51, 1.0, 1.0, -1200.0 } },
             3522.8420617665834, 5746.555795626526,
             { { 100, 0.0f, 0.0f }, { 1000, -0.04133236035704613f, -0.09954667836427689f }, { 2500, 0.2800408899784088f, -0.9350972175598145f },
               { 4000, 0.9537044167518616f, 0.8892703056335449f }, { 7777, 0.8376388549804688f, -0.14166420698165894f },
               { 15000, -0.05926012247800827f, -0.5840796828269958f } } });

    // the instrument: a pad sounds on its key and dies away; keys without a pad stay silent
    {
        auto inst = lynx::makeInstrument (lynx::InstrumentKind::drumaxx);
        lynx::ChannelDef def;
        def.instrument = lynx::InstrumentKind::drumaxx;
        def.pluginState = st;
        inst->configure (def, 140.0);
        inst->prepare (44100.0);
        std::vector<float> l (512, 0.0f), r (512, 0.0f);
        inst->noteOn (90, 1.0f);
        inst->render (l.data(), r.data(), 512, 1.0f, 1.0f);
        CHECK (inst->idle() && std::all_of (l.begin(), l.end(), [] (float v) { return v == 0.0f; }));
        inst->noteOn (36, 0.8f);
        inst->render (l.data(), r.data(), 512, 1.0f, 1.0f);
        double e = 0.0;
        for (auto v : l)
            e += (double) v * v;
        CHECK (! inst->idle() && e > 1e-3);
        for (int k = 0; k < 200 && ! inst->idle(); ++k)
            inst->render (l.data(), r.data(), 512, 1.0f, 1.0f);
        CHECK (inst->idle());
    }

    // import: a Drumaxx channel plays on the exact engine with its raw state
    {
        FlpBuilder b;
        b.ascii (0xC7, "25.2.5.5319");
        b.u8 (0x40); b.u16 (0);
        b.u8 (0x15); b.u8 (2);
        b.utf16 (0xC9, u"Drumaxx");
        b.data (0xD5, st);
        lynx::FlpImportOptions options;
        lynx::FlpImportReport report;
        options.report = &report;
        const auto song = lynx::importFlp (flp::parseProject (b.bytes), options);
        CHECK (song.channels.size() == 1u);
        if (song.channels.size() == 1u)
            CHECK (song.channels[0].instrument == lynx::InstrumentKind::drumaxx && song.channels[0].pluginState == st);
        CHECK (report.converted["Drumaxx"] == 1);
    }
}
// --- REA round 68: FL Slayer (exact engine) and Fruity Dance (visual only) ------------------------------------------
namespace
{
std::string slayerDll()
{
    if (realFlpDir.empty())
        return {};
    return (std::filesystem::path (realFlpDir).parent_path() / "Plugins" / "Fruity" / "Generators" / "FL Slayer" / "Slayer_Engine.dll").string();
}

std::vector<std::uint8_t> slayerPreset (const char* name)
{
    if (realFlpDir.empty())
        return {};
    const auto file = std::filesystem::path (realFlpDir) / "Patches" / "Plugin presets" / "Generators" / "FL Slayer" / (std::string (name) + ".fst");
    const auto states = pluginStatesIn (file, "FL Slayer");
    return states.empty() ? std::vector<std::uint8_t> {} : states.front();
}
} // namespace

TEST (slayer_state)
{
    // the saved state: int version, then the parameter integers (version 0: 28, version 1: 29 with the mod wheel)
    std::vector<std::uint8_t> v0 (4 + 28 * 4, 0), v1 (4 + 29 * 4, 0);
    const std::int32_t one = 1, wheel = 77, mode = 5;
    std::memcpy (v1.data(), &one, 4);
    std::memcpy (v1.data() + 4 + 28 * 4, &wheel, 4);
    std::memcpy (v0.data() + 4 + 24 * 4, &mode, 4);
    CHECK (lynx::slayerStateUsable (v0) && lynx::slayerStateUsable (v1));
    CHECK (! lynx::slayerStateUsable (std::vector<std::uint8_t> (v0.begin(), v0.end() - 1)));
    auto v2 = v1;
    v2[0] = 2;
    CHECK (! lynx::slayerStateUsable (v2));
    const auto p0 = lynx::slayerStateParams (v0), p1 = lynx::slayerStateParams (v1);
    CHECK (p0.size() == 29u && p0[24] == 5 && p0[28] == 0 && p1.size() == 29u && p1[28] == 77);
    // the controls' maxima (the front-end sends value / max)
    CHECK (lynx::slayerParamMax (0) == 6 && lynx::slayerParamMax (20) == 15 && lynx::slayerParamMax (24) == 5 && lynx::slayerParamMax (28) == 127
           && lynx::slayerParamMax (3) == 65535);
    // MIDI velocity from Init.Vol (0 → 0, which the engine takes as a note-off; FL's full velocity → 127)
    CHECK (lynx::slayer::midiVelocity (0.0) == 0 && lynx::slayer::midiVelocity (1.0) == 99 && lynx::slayer::midiVelocity (2.0) == 126
           && lynx::slayer::midiVelocity ((std::pow (11.0, 1.28) - 1.0) / 10.0) == 127);
}

TEST (slayer_engine)
{
    const auto dll = slayerDll();
    if (dll.empty() || ! lynx::slayerDllUsable (dll))
        return; // no FL installation
    // against the installation's Slayer_Engine.dll (EXEC, REA_rodada68 §7: the DLL driven like FL's front-end drives it,
    // host blocks of 512 at 44.1 kHz, tempo 120): bit-identical output. C3 vel 90 1024–20000, G3 vel 120 8000–30000,
    // E4 vel 40 16000–40000, C5 vel 127 from 30000.
    struct Sample { int at; float l, r; };
    struct Golden { const char* preset; double el, er; Sample s[4]; };
    const Golden goldens[] {
        { "BASS Ebass 2", 15073.5614896, 15073.5614896, { { 5000, 0.39340171217918396f, 0.39340171217918396f }, { 12000, -1.6453207731246948f, -1.6453207731246948f }, { 25000, 0.4477591812610626f, 0.4477591812610626f }, { 40000, 0.15478527545928955f, 0.15478527545928955f } } },
        { "DIST Dire Straits", 1667.57549156, 1687.24007956, { { 5000, 0.026657210662961006f, -0.10683870315551758f }, { 12000, 0.16261409223079681f, -0.402504026889801f }, { 25000, 0.06702516227960587f, -0.04462721571326256f }, { 40000, -0.35892999172210693f, 0.29433923959732056f } } },
        { "POWER Metal", 2041.24961449, 2041.24961449, { { 5000, -0.045430999249219894f, -0.045430999249219894f }, { 12000, 0.009085401892662048f, 0.009085401892662048f }, { 25000, -0.15170709788799286f, -0.15170709788799286f }, { 40000, -0.0430934801697731f, -0.0430934801697731f } } },
        { "SINGLE Chorus", 1199.72408266, 1228.23838997, { { 5000, 0.08928016573190689f, 0.07585708051919937f }, { 12000, 0.22826193273067474f, 0.16860444843769073f }, { 25000, 0.14708775281906128f, 0.12602058053016663f }, { 40000, -0.02422964572906494f, -0.021596161648631096f } } },
        { "AUTO Leslie", 2045.59821914, 16901.1532986, { { 5000, 0.09575536102056503f, -0.0019274931401014328f }, { 12000, -0.0070709832943975925f, -0.3028614819049835f }, { 25000, 0.07535621523857117f, 0.24308738112449646f }, { 40000, -0.09379560500383377f, -0.008114430122077465f } } },
        { "FX Mad Photons", 1849.74988002, 1849.74988002, { { 5000, -0.11505282670259476f, -0.11505282670259476f }, { 12000, 0.3610122501850128f, 0.3610122501850128f }, { 25000, 0.36028820276260376f, 0.36028820276260376f }, { 40000, -0.5374203324317932f, -0.5374203324317932f } } },
        { "ELEKTRO Jungle", 3929.94173335, 3929.94173335, { { 5000, 0.3158963918685913f, 0.3158963918685913f }, { 12000, 0.2988760769367218f, 0.2988760769367218f }, { 25000, 0.15749095380306244f, 0.15749095380306244f }, { 40000, -0.12722519040107727f, -0.12722519040107727f } } },
        { "DIST Sololead Glide", 988.454469266, 1208.61893055, { { 5000, -0.03612576052546501f, -0.015017407014966011f }, { 12000, -0.008314903825521469f, 0.03836013376712799f }, { 25000, -0.047041457146406174f, -0.13751298189163208f }, { 40000, -0.03302673622965813f, -0.23574480414390564f } } },
        { "SINGLE Funky", 1393.67536587, 4191.35072624, { { 5000, -0.09534210711717606f, 0.36347872018814087f }, { 12000, 0.3368682265281677f, -0.9259893894195557f }, { 25000, -0.022296791896224022f, 0.01075129397213459f }, { 40000, 0.044511131942272186f, -0.37124285101890564f } } },
        { "AUTO Chorus Chords", 26161.9618548, 26396.2394554, { { 5000, -0.19687165319919586f, 0.08371803164482117f }, { 12000, 1.0583257675170898f, 1.5273079872131348f }, { 25000, -0.88118577003479f, -0.0774342492222786f }, { 40000, -0.3456699848175049f, -0.4140896201133728f } } },
        { "POWER Rock Taps", 1306.21610814, 1164.18639692, { { 5000, 0.019661661237478256f, 0.07336095720529556f }, { 12000, -0.06010562554001808f, -0.3300100862979889f }, { 25000, 0.04404725879430771f, 0.0837465152144432f }, { 40000, -0.029322467744350433f, -0.1399042010307312f } } },
        { "FX Atari 2600 Computer", 1391.44227691, 1391.44227691, { { 5000, 0.18123838305473328f, 0.18123838305473328f }, { 12000, -0.1220453679561615f, -0.1220453679561615f }, { 25000, 0.17565210163593292f, 0.17565210163593292f }, { 40000, 0.21183894574642181f, 0.21183894574642181f } } },
    };
    const std::vector<lynx::slayer::RefNote> notes = { { 1024, 48, 90, 20000 }, { 8000, 55, 120, 30000 }, { 16000, 64, 40, 40000 }, { 30000, 72, 127, -1 } };
    constexpr int length = 44100;
    int checked = 0;
    for (const auto& g : goldens)
    {
        const auto st = slayerPreset (g.preset);
        CHECK (lynx::slayerStateUsable (st));
        if (! lynx::slayerStateUsable (st))
            continue;
        const auto y = lynx::slayer::renderReference (lynx::slayerStateParams (st), dll, notes, 44100.0, length);
        double el = 0.0, er = 0.0;
        for (std::size_t i = 0; i + 1 < y.size(); i += 2)
            el += (double) y[i] * y[i], er += (double) y[i + 1] * y[i + 1];
        bool ok = y.size() == 2 * (std::size_t) length && std::abs (el - g.el) <= 1e-10 * g.el && std::abs (er - g.er) <= 1e-10 * g.er;
        for (const auto& smp : g.s)
            ok = ok && y[2 * (std::size_t) smp.at] == smp.l && y[2 * (std::size_t) smp.at + 1] == smp.r;
        if (! ok)
            std::printf ("  FL Slayer %s differs from FL's DLL: energy %.12g %.12g (DLL %.12g %.12g)\n", g.preset, el, er, g.el, g.er);
        CHECK (ok);
        ++checked;
    }
    CHECK (checked == 12);

    // the instrument: the engine primed with one sample, FL's velocity curve, the front-end's gains from the channel's
    // MIDI pan / volume (REA_rodada79 §2: twice the channel gain through the host's integer-pan law); a note with
    // velocity 0.8 is the reference note one sample later
    const auto st = slayerPreset ("BASS Ebass 2");
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::slayer;
    c.pluginState = st;
    c.samplePath = dll;
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::slayer);
    inst->configure (c, 120.0);
    inst->prepare (44100.0);
    std::vector<float> l (16384, 0.0f), r (16384, 0.0f);
    for (int b = 0; b < 32; ++b)
    {
        if (b == 0)
            inst->noteOn (60, 0.8f);
        if (b == 20)
            inst->noteOff (60);
        inst->render (l.data() + 512 * b, r.data() + 512 * b, 512, 0.5f, 0.25f);
    }
    const int vel = lynx::slayer::midiVelocity ((std::pow (11.0, 1.28 * 0.8) - 1.0) / 10.0);
    const auto ref = lynx::slayer::renderReference (lynx::slayerStateParams (st), dll, { { 1, 60, vel, 1 + 20 * 512 } }, 44100.0, 16385);
    // LYNX's channel gains 0.5 / 0.25 = gain 0.395…, pan −0.4097 → MIDI pan −26
    const auto chanVol = (float) std::sqrt ((0.25 + 0.0625) * 0.5);
    float hl = 0.0f, hr = 0.0f;
    lynx::slayer::hostGains (lynx::slayer::midiPan (std::atan2 (0.25, 0.5) * (4.0 / 3.141592653589793) - 1.0), chanVol, hl, hr);
    CHECK (lynx::slayer::midiPan (std::atan2 (0.25, 0.5) * (4.0 / 3.141592653589793) - 1.0) == -26);
    double diff = 0.0, energy = 0.0;
    for (int i = 0; i < 16384; ++i)
    {
        diff = std::max ({ diff, (double) std::abs (l[(std::size_t) i] - ref[2 * (std::size_t) (i + 1)] * hl),
                           (double) std::abs (r[(std::size_t) i] - ref[2 * (std::size_t) (i + 1) + 1] * hr) });
        energy += (double) l[(std::size_t) i] * l[(std::size_t) i];
    }
    CHECK (diff == 0.0 && energy > 1.0 && vel == 102);

    // every preset of the installation: usable and sounding
    int presets = 0, sounding = 0;
    const auto folder = std::filesystem::path (realFlpDir) / "Patches" / "Plugin presets" / "Generators" / "FL Slayer";
    if (std::filesystem::exists (folder))
        for (const auto& entry : std::filesystem::directory_iterator (folder))
            for (const auto& s : pluginStatesIn (entry.path(), "FL Slayer"))
            {
                ++presets;
                const auto y = lynx::slayer::renderReference (lynx::slayerStateParams (s), dll, { { 1024, 52, 100, 20000 } }, 44100.0, 22050);
                double e = 0.0;
                bool finite = true;
                for (auto v : y)
                    finite = finite && std::isfinite (v), e += (double) v * v;
                CHECK (finite);
                sounding += e > 1.0 ? 1 : 0;
            }
    std::printf ("  FL Slayer presets: %d, %d sounding\n", presets, sounding);
    CHECK (presets == 64 && sounding == 64);
}

// FL Slayer's host levels and live parameters (REA_rodada79): channel pan / volume / pitch arrive as MIDI events
// (ProcessEvent 2/3/4), automation and settings edits as setParameter calls on the running engine
TEST (slayer_host_levels_and_automation)
{
    using namespace lynx::slayer;
    CHECK (pitchBend (0) == 8192 && pitchBend (100) == 8874 && pitchBend (-100) == 7510 && pitchBend (1200) == 0x3fff
           && pitchBend (-1200) == 0 && pitchBend (-1199) == 7 && pitchBend (5000) == 0x3fff);
    CHECK (midiPan (0.0) == 0 && midiPan (1.0) == 64 && midiPan (-1.0) == -64 && midiPan (-0.5) == -32
           && midiPan (99.0 / 6400.0) == 0 && midiPan (-199.0 / 6400.0) == -1 && midiPan (2.0) == 64);
    float l0 = 0.0f, r0 = 0.0f, l1 = 0.0f, r1 = 0.0f;
    hostGains (0, 0.5f, l0, r0);   // the front-end's own start values (volume 1 after the ×2, centre)
    hostGains (64, 1.0f, l1, r1);
    CHECK (l0 == 0.70710677f && r0 == 0.70710677f && std::abs (l1) < 1e-15f && r1 == 2.0f);

    const auto dll = slayerDll();
    if (dll.empty() || ! lynx::slayerDllUsable (dll))
        return;
    const auto st = slayerPreset ("DIST Dire Straits");
    const auto params = lynx::slayerStateParams (st);
    lynx::ChannelDef c;
    c.instrument = lynx::InstrumentKind::slayer;
    c.pluginState = st;
    c.samplePath = dll;
    c.sampler.pitch = 1.5f;      // channel pitch +150 cents → bend 9216; no root shift
    c.modelParams = { 0.0f };
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::slayer);
    inst->configure (c, 120.0);
    inst->prepare (44100.0);
    const int blocks = 40, n = 512;
    std::vector<float> l ((std::size_t) (blocks * n), 0.0f), r ((std::size_t) (blocks * n), 0.0f);
    auto edited = st;
    const std::int32_t newDrive = 60000; // parameter 13 (amp drive), edited in the settings at block 25
    std::memcpy (edited.data() + 4 + 4 * 13, &newDrive, 4);
    for (int b = 0; b < blocks; ++b)
    {
        if (b == 0)
            inst->noteOn (52, 0.8f);
        if (b == 10)
            inst->setParameter (20, 9.0f / 15.0f);   // automation: effect 9 (chorus)
        if (b == 18)
            inst->setParameter (28, 0.5f);           // automation: the mod wheel → CC 1 64
        if (b == 25)
        {
            c.pluginState = edited;
            inst->configure (c, 120.0);
        }
        inst->render (l.data() + n * b, r.data() + n * b, n, 1.0f, 1.0f);
    }
    lynx::slayer::RefOptions opt;
    opt.midi.push_back ({ 1, 0xe0, 9216 & 0x7f, 9216 >> 7 });
    opt.params = { { 1 + 10 * n, 20, 9 }, { 1 + 18 * n, 28, 64 }, { 1 + 25 * n, 13, newDrive } };
    const int vel = lynx::slayer::midiVelocity ((std::pow (11.0, 1.28 * 0.8) - 1.0) / 10.0);
    const auto ref = lynx::slayer::renderReference (params, dll, { { 1, 52, vel, -1 } }, 44100.0, blocks * n + 1, opt);
    float hl = 0.0f, hr = 0.0f;
    hostGains (0, 1.0f, hl, hr);
    double diff = 0.0, energy = 0.0;
    for (int i = 0; i < blocks * n; ++i)
    {
        diff = std::max ({ diff, (double) std::abs (l[(std::size_t) i] - ref[2 * (std::size_t) (i + 1)] * hl),
                           (double) std::abs (r[(std::size_t) i] - ref[2 * (std::size_t) (i + 1) + 1] * hr) });
        energy += (double) l[(std::size_t) i] * l[(std::size_t) i];
    }
    CHECK (diff == 0.0 && energy > 1.0);
    // the changes really change the sound: the same render without them differs
    const auto plain = lynx::slayer::renderReference (params, dll, { { 1, 52, vel, -1 } }, 44100.0, blocks * n + 1);
    CHECK (plain != ref);
}

TEST (slayer_and_fruity_dance_import)
{
    // FL Slayer plays on the exact engine when the installation's engine DLL is there (otherwise it stays unsupported);
    // Fruity Dance (a visual plugin) becomes a silent channel that keeps its state
    std::vector<std::uint8_t> slayer (4 + 28 * 4, 0), dance (40, 1);
    FlpBuilder b;
    b.ascii (0xC7, "25.2.5.5319");
    int index = 0;
    for (const auto& [name, state] : { std::pair<std::u16string, std::vector<std::uint8_t>> { u"FL Slayer", slayer }, { u"Fruity Dance", dance } })
    {
        b.u8 (0x40); b.u16 ((std::uint16_t) index++);
        b.u8 (0x15); b.u8 (2);
        b.utf16 (0xC9, name);
        b.data (0xD5, state);
    }
    const auto project = flp::parseProject (b.bytes);
    const auto plain = lynx::importFlp (project);
    CHECK (plain.channels.size() == 2u);
    if (plain.channels.size() == 2u)
    {
        CHECK (plain.channels[0].instrument != lynx::InstrumentKind::slayer);
        CHECK (plain.channels[1].instrument == lynx::InstrumentKind::fruitydance && plain.channels[1].pluginState == dance);
    }
    auto inst = lynx::makeInstrument (lynx::InstrumentKind::fruitydance);
    inst->prepare (44100.0);
    std::vector<float> l (1024, 0.0f), r (1024, 0.0f);
    inst->noteOn (60, 1.0f);
    inst->render (l.data(), r.data(), 1024, 1.0f, 1.0f);
    CHECK (inst->idle() && std::all_of (l.begin(), l.end(), [] (float v) { return v == 0.0f; }));
    const auto dll = slayerDll();
    if (dll.empty() || ! lynx::slayerDllUsable (dll))
        return;
    lynx::FlpImportOptions withFiles;
    withFiles.roots = { std::filesystem::path (realFlpDir).parent_path().string(), realFlpDir, "" };
    withFiles.fileExists = [] (const std::string& path) {
        auto local = path;
        std::replace (local.begin(), local.end(), '\\', '/');
        return std::filesystem::exists (local);
    };
    const auto song = lynx::importFlp (project, withFiles);
    CHECK (song.channels.size() == 2u && song.channels[0].instrument == lynx::InstrumentKind::slayer && song.channels[0].pluginState == slayer
           && ! song.channels[0].samplePath.empty());
}
#include "WaveAnalysisTests.inc"
#include "UiPolish2Tests.inc"
#include "UiToolbarTests.inc"
#include "LuxeVerbTests.inc"
#include "ConvolverTests.inc"

// --- Newtone / Newtime (REA_rodada73) -----------------------------------------------------------------------------

namespace newtonetest
{
void put32 (std::vector<std::uint8_t>& b, std::int32_t v) { for (int i = 0; i < 4; ++i) b.push_back ((std::uint8_t) ((std::uint32_t) v >> (8 * i))); }
void put64 (std::vector<std::uint8_t>& b, std::int64_t v) { for (int i = 0; i < 8; ++i) b.push_back ((std::uint8_t) ((std::uint64_t) v >> (8 * i))); }
void record (std::vector<std::uint8_t>& b, int id, const std::vector<std::uint8_t>& d)
{
    put32 (b, id);
    put64 (b, (std::int64_t) d.size());
    b.insert (b.end(), d.begin(), d.end());
}

// a state with our own layout reading (REA_rodada73 §2): options, the wrapper record around the engine XML, the source path
std::vector<std::uint8_t> state (int version, std::uint32_t flags, const std::string& xml, const std::string& source)
{
    std::vector<std::uint8_t> b, r1, r2;
    put32 (b, version);
    put32 (r1, 727);
    put32 (r1, 582);
    put32 (r1, (std::int32_t) flags);
    r1.resize (0x28, 0);
    record (b, 1, r1);
    std::vector<std::uint8_t> chunk { 'V', 'C', '2', '!' };
    put32 (chunk, (std::int32_t) xml.size() + 1);
    chunk.insert (chunk.end(), xml.begin(), xml.end());
    chunk.push_back (0);
    put32 (r2, -9);
    r2.push_back (0x0c);
    put32 (r2, -2);
    put64 (r2, (std::int64_t) chunk.size());
    put32 (r2, 0);
    r2.insert (r2.end(), chunk.begin(), chunk.end());
    record (b, 2, r2);
    record (b, 3, std::vector<std::uint8_t> (source.begin(), source.end()));
    record (b, 4, std::vector<std::uint8_t> (12, 0));
    record (b, 5, std::vector<std::uint8_t> (16, 0));
    record (b, 6, std::vector<std::uint8_t> (32, 0));
    return b;
}

const char* xmlWithTrack (bool edited)
{
    return edited ? "<NewToneProject NumOfTracks=\"1\"><Tracks><Trk_0 TrackNum=\"0\" Type=\"class CzplJPositionablePSOLASource\" "
                    "FilePath=\"C:\\tmp\\a&amp;b.wav\"><AudioObject SelfBPM=\"120.0\" OriginalFileLength=\"1000\" GlobalTune=\"0.0\">"
                    "<Pitch_0 fOrigPitch=\"60.0\" fCurPitch=\"60.0\" iOrigLength=\"500\" iCurLength=\"500\"/>"
                    "<Pitch_1 fOrigPitch=\"62.0\" fCurPitch=\"64.0\" iOrigLength=\"500\" iCurLength=\"500\"/></AudioObject></Trk_0></Tracks></NewToneProject>"
                  : "<NewToneProject NumOfTracks=\"1\"><Tracks><Trk_0 TrackNum=\"0\" Type=\"class CzplJTimelineWarpSource\" "
                    "FilePath=\"x.wav\"><AudioObject SelfBPM=\"80.0\" OriginalFileLength=\"1000\">"
                    "<Pitch_0 fOrigPitch=\"60.0\" fCurPitch=\"60.0\" fCurVolume=\"1.0\"/></AudioObject></Trk_0></Tracks></NewToneProject>";
}
} // namespace newtonetest

TEST (newtone_state)
{
    using namespace newtonetest;
    const auto a = lynx::newtonefx::parse (state (0, 5, xmlWithTrack (true), "%FLStudioUserData%\\vox.wav"));
    CHECK (a.has_value());
    CHECK (a->version == 0 && a->editorWidth == 727 && a->editorHeight == 582 && a->slave());
    CHECK (a->sourcePath == "%FLStudioUserData%\\vox.wav");
    CHECK (a->tracks.size() == 1 && a->tracks[0].notes == 2 && a->tracks[0].frames == 1000 && ! a->tracks[0].timeWarp);
    CHECK (a->tracks[0].filePath == "C:\\tmp\\a&b.wav" && a->tracks[0].selfBpm == 120.0);
    CHECK (a->tracks[0].edited && a->edited()); // note 1 moved from 62 to 64

    const auto b = lynx::newtonefx::parse (state (1, 0x41, xmlWithTrack (false), ""));
    CHECK (b.has_value() && ! b->slave() && b->sourcePath.empty());
    CHECK (b->tracks.size() == 1 && b->tracks[0].timeWarp && ! b->edited());

    // an empty project, as the installation's presets
    const auto c = lynx::newtonefx::parse (state (0, 1, "<NewToneProject NumOfTracks=\"0\"><Tracks/></NewToneProject>", ""));
    CHECK (c.has_value() && c->tracks.empty());

    // broken: other version, truncated record, no options record
    CHECK (! lynx::newtonefx::parse (state (2, 1, "", "")));
    auto cut = state (0, 1, xmlWithTrack (true), "");
    cut.resize (cut.size() - 3);
    CHECK (! lynx::newtonefx::parse (cut));
    CHECK (! lynx::newtonefx::parse ({ 0, 0, 0, 0 }));

    // the FL import keeps the state and takes the slave switch as the editable parameter
    const auto raw = state (0, 5, xmlWithTrack (false), "");
    const auto slot = lynx::effectFromFl ("Newtone", raw, 120.0);
    CHECK (slot && slot->kind == lynx::EffectKind::newtone && slot->state == raw && slot->params[0] == 1.0f);
    const auto slot2 = lynx::effectFromFl ("Newtime", state (1, 0x41, xmlWithTrack (false), ""), 120.0);
    CHECK (slot2 && slot2->kind == lynx::EffectKind::newtime && slot2->params[0] == 0.0f);
    CHECK (lynx::formatEffectParam (lynx::EffectKind::newtone, 0, 1.0f, 120.0) == "on");

    // the installation's own states (plugin database and the playlist's pitch/warp correction tracks) [DATA]
    if (! realFlpDir.empty())
    {
        int found = 0, read = 0;
        for (const char* rel : { "System/PitchCorrectionTrack.fst", "System/WarpCorrectionTrack.fst",
                                 "Patches/Plugin database/Effects/Misc/Newtone.fst", "Patches/Plugin database/Effects/Misc/Newtime.fst" })
        {
            const auto path = std::filesystem::path (realFlpDir) / rel;
            if (! std::filesystem::exists (path))
                continue;
            ++found;
            std::ifstream f (path, std::ios::binary);
            std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
            const auto fx = lynx::effectFromFlPreset (bytes, 120.0);
            read += fx && (fx->kind == lynx::EffectKind::newtone || fx->kind == lynx::EffectKind::newtime) && ! fx->state.empty()
                    && fx->params[0] == 0.0f && lynx::newtonefx::parse (fx->state)->tracks.empty();
        }
        std::printf ("  %d Newtone/Newtime states, %d read\n", found, read);
        CHECK (read == found);
    }
}

TEST (newtone_engine)
{
    // REA_rodada73 §3-4 [EXEC]: the input passes unchanged unless slave mode is on and the song plays; then the source
    // sample is added from the song time on (at the source's own rate)
    auto sample = std::make_shared<lynx::SampleData>();
    sample->sampleRate = 44100.0;
    for (int i = 0; i < 3000; ++i)
        sample->left.push_back (0.001f * (float) (i % 97) - 0.04f);
    const double sr = 44100.0, bpm = 120.0;
    const auto tps = lynx::ticksPerSample (bpm, sr);

    auto fx = lynx::newtonefx::make (sample);
    fx->prepare (sr);
    lynx::EffectParams on {}, off {};
    on[0] = 1.0f;

    std::vector<float> l (256), r (256), in (256);
    auto block = [&] (double tick, bool playing, const lynx::EffectParams& p) {
        for (int i = 0; i < 256; ++i)
            l[(std::size_t) i] = r[(std::size_t) i] = in[(std::size_t) i] = 0.25f * std::sin (0.01f * (float) (i + tick));
        fx->setSongPosition (tick);
        fx->setPlaying (playing);
        fx->process (l.data(), r.data(), 256, p, bpm);
    };
    auto same = [&] { return std::equal (l.begin(), l.end(), in.begin()) && std::equal (r.begin(), r.end(), in.begin()); };

    block (0.0, true, off);
    CHECK (same()); // slave off
    block (0.0, false, on);
    CHECK (same()); // not playing

    // playing from the start: in + source, sample by sample, over consecutive blocks
    bool exact = true;
    for (int b = 0; b < 4; ++b)
    {
        block (b * 256 * tps, true, on);
        for (int i = 0; i < 256; ++i)
            exact = exact && l[(std::size_t) i] == in[(std::size_t) i] + sample->left[(std::size_t) (b * 256 + i)]
                    && r[(std::size_t) i] == l[(std::size_t) i];
    }
    CHECK (exact);

    // a jump re-seeks to the song time; past the end of the sample only the input remains
    block (2000 * tps, true, on);
    CHECK (l[0] == in[0] + sample->left[2000] && l[255] == in[255] + sample->left[2255]);
    block (2900 * tps, true, on);
    CHECK (l[99] == in[99] + sample->left[2999] && l[100] == in[100]);
    // stopping and playing again: from the new song time
    block (0.0, false, on);
    CHECK (same());
    block (10 * tps, true, on);
    CHECK (l[0] == in[0] + sample->left[10]);

    // without its sample: a pass-through
    auto none = lynx::makeEffect (lynx::EffectKind::newtime);
    none->prepare (sr);
    for (int i = 0; i < 256; ++i)
        l[(std::size_t) i] = r[(std::size_t) i] = in[(std::size_t) i] = 0.1f * (float) i / 256.0f;
    none->setPlaying (true);
    none->process (l.data(), r.data(), 256, on, bpm);
    CHECK (same());
}

TEST (newtone_rack_follows_transport)
{
    // the mixer gives the effect the transport, and keeps it running on a silent insert (no smart disable)
    auto song = lynx::Song::makeDefault();
    for (auto& p : song.patterns)
        p.notes.clear();
    song.bpm = 120.0;
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::newtone);
    slot.params[0] = 1.0f;
    slot.samplePath = "newtone-test.wav";
    song.mixer[1].effects.push_back (slot);

    auto sample = std::make_shared<lynx::SampleData>();
    sample->sampleRate = 48000.0;
    sample->left.assign (48000, 0.5f);
    int loads = 0;
    lynx::RackState::SampleProvider provider = [&] (const std::string& path) -> std::shared_ptr<const lynx::SampleData> {
        ++loads;
        return path == "newtone-test.wav" ? sample : nullptr;
    };
    const double sr = 48000.0;
    auto state = lynx::RackState::build (song, nullptr, sr, provider);
    CHECK (loads == 1);

    lynx::RackRenderer rr;
    rr.prepare (sr);
    std::vector<float> l (512), r (512);
    auto peakOf = [&] (bool playing, double tick) {
        float peak = 0.0f;
        for (int b = 0; b < 8; ++b)
        {
            rr.process (state.get(), playing, tick + b * 512 * lynx::ticksPerSample (song.bpm, sr), 512, l.data(), r.data());
            for (auto v : l)
                peak = std::max (peak, std::abs (v));
        }
        return peak;
    };
    CHECK (peakOf (false, 0.0) == 0.0f);
    CHECK (peakOf (true, 0.0) > 0.1f);
}
#include "HardcoreTests.inc"
#include "DistructorTests.inc"

// --- missing files (core/MissingFiles.h) ------------------------------------------------------------------------

TEST (missing_files_names_and_scores)
{
    CHECK (lynx::pathFileName ("C:\\Samples\\Drums\\Kick 01.wav") == "Kick 01.wav");
    CHECK (lynx::pathFileName ("/home/u/loops/beat.flac") == "beat.flac");
    CHECK (lynx::pathFileName ("D:/mixed\\sep/x.wav/") == "x.wav");
    CHECK (lynx::pathFileName ("plain.wav") == "plain.wav");
    CHECK (lynx::relocationScore ("C:\\Old\\Drums\\Kicks\\kick.wav", "/new/Drums/Kicks/KICK.WAV") == 3);
    CHECK (lynx::relocationScore ("C:\\Old\\Drums\\Kicks\\kick.wav", "/new/other/kick.wav") == 1);
    CHECK (lynx::relocationScore ("C:\\Old\\kick.wav", "/new/kick2.wav") == 0);
}

TEST (missing_files_find_in_song)
{
    lynx::Song song;
    lynx::ChannelDef a, b, c, d, e;
    a.name = "Kick", a.instrument = lynx::InstrumentKind::sampler, a.samplePath = "C:\\gone\\kick.wav";
    b.name = "Vox", b.instrument = lynx::InstrumentKind::audioClip, b.samplePath = "C:\\gone\\vox.wav";
    c.name = "Here", c.instrument = lynx::InstrumentKind::sampler, c.samplePath = "C:\\here\\snare.wav";
    d.name = "GMS", d.instrument = lynx::InstrumentKind::gms, d.samplePath = "C:\\FL\\gms waves"; // installation data: not listed
    e.name = "Kick 2", e.instrument = lynx::InstrumentKind::sampler, e.samplePath = "c:/GONE/KICK.wav"; // same file, other spelling
    lynx::SampleZone z;
    z.path = "C:\\gone\\piano_c4.wav";
    a.zones.push_back (z);
    song.channels = { a, b, c, d, e };
    song.mixer.resize (3);
    lynx::EffectSlot fx;
    fx.samplePath = "C:\\gone\\tune.wav";
    lynx::EffectSlot parallel;
    parallel.branches.push_back ({ { fx }, 1.0f });
    song.mixer[2].effects.push_back (parallel);
    song.mixer[2].name = "Lead";

    const auto exists = [] (const std::string& p) { return p.find ("here") != std::string::npos || p.find ("FL") != std::string::npos; };
    auto missing = lynx::findMissingSamples (song, exists);
    CHECK (missing.size() == 4);
    CHECK (missing.size() == 4 && missing[0].path == "C:\\gone\\kick.wav" && missing[0].usedBy.size() == 2 && missing[0].usedBy[1] == "Kick 2");
    CHECK (missing.size() == 4 && missing[1].path == "C:\\gone\\piano_c4.wav" && missing[1].usedBy[0] == "Kick");
    CHECK (missing.size() == 4 && missing[2].usedBy[0] == "Audio clip: Vox");
    CHECK (missing.size() == 4 && missing[3].usedBy[0] == "Lead: effect");

    // candidates from a search: the deeper path match wins; one stays missing
    const std::vector<std::string> hits { "/lib/other/kick.wav", "/lib/gone/kick.wav", "/lib/x/vox.wav", "/lib/y/tune.wav" };
    CHECK (lynx::relocateByName (missing, hits) == 3);
    CHECK (missing[0].found == "/lib/gone/kick.wav");
    CHECK (missing[1].found.empty());
    missing[3].skipped = true; // skipped: not applied
    const auto map = lynx::relocationMap (missing);
    CHECK (map.size() == 2);
    CHECK (lynx::applyRelocations (song, map) == 3); // both spellings of kick.wav + vox
    CHECK (song.channels[0].samplePath == "/lib/gone/kick.wav" && song.channels[4].samplePath == "/lib/gone/kick.wav");
    CHECK (song.channels[1].samplePath == "/lib/x/vox.wav");
    CHECK (song.channels[2].samplePath == "C:\\here\\snare.wav");
    CHECK (song.mixer[2].effects[0].branches[0].chain[0].samplePath == "C:\\gone\\tune.wav");
    lynx::Song copy = song;
    CHECK (lynx::applyRelocations (copy, { { "C:\\gone\\tune.wav", "/t.wav" } }) == 1 && copy.mixer[2].effects[0].branches[0].chain[0].samplePath == "/t.wav");
}

TEST (missing_files_folder_search)
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "lynx-missing-test";
    fs::remove_all (root);
    fs::create_directories (root / "a" / "deep" / "Kicks");
    fs::create_directories (root / "b");
    for (const auto& f : { root / "a" / "deep" / "Kicks" / "Kick.WAV", root / "b" / "kick.wav", root / "b" / "hat.wav", root / "b" / "unrelated.wav" })
        std::ofstream (f) << "x";

    std::vector<lynx::MissingFile> missing { { "C:\\Old\\Kicks\\kick.wav", { "Kick" }, {}, false }, { "C:\\Old\\hat.wav", { "Hat" }, {}, false },
                                             { "C:\\Old\\ride.wav", { "Ride" }, {}, false } };
    int listed = 0;
    auto list = [&] (const std::string& folder, const std::function<bool (const std::string&, bool)>& onEntry) {
        for (const auto& entry : fs::directory_iterator (folder))
        {
            ++listed;
            if (! onEntry (entry.path().string(), entry.is_directory()))
                return;
        }
    };
    const auto hits = lynx::searchFolderForMissing (missing, root.string(), list);
    CHECK (hits.size() == 3);
    CHECK (lynx::relocateByName (missing, hits) == 2);
    CHECK (missing[0].found.find ("Kicks") != std::string::npos); // the copy in a "Kicks" folder matches better
    CHECK (missing[1].found.find ("hat.wav") != std::string::npos);
    CHECK (missing[2].found.empty());

    // a bounded walk stops early; a cancelled one finds nothing
    CHECK (lynx::searchFolderForMissing (missing, root.string(), list, 2).size() <= 1);
    CHECK (lynx::searchFolderForMissing (missing, root.string(), list, 0, [] { return true; }).empty());
    // nothing left to find: no walk at all
    missing[2].skipped = true;
    listed = 0;
    CHECK (lynx::searchFolderForMissing (missing, root.string(), list).empty() && listed == 0);
    fs::remove_all (root);
}

TEST (missing_files_folder_list)
{
    const auto folders = lynx::parseFolderList ("C:\\A\r\n  C:\\B  \n\nc:\\a\nD:/x\n");
    CHECK (folders.size() == 3 && folders[0] == "C:\\A" && folders[1] == "C:\\B" && folders[2] == "D:/x");
    auto list = folders;
    lynx::rememberFolder (list, "D:\\x");
    CHECK (list.size() == 3 && list[0] == "D:\\x" && list[1] == "C:\\A");
    lynx::rememberFolder (list, "E:\\new", 2);
    CHECK (list.size() == 2 && list[0] == "E:\\new" && list[1] == "D:\\x");
    CHECK (lynx::parseFolderList (lynx::folderListText (list)) == list);
}

TEST (missing_files_flp_report_and_relocation)
{
    // an .flp sampler whose sample is gone: listed with its channel; a relocation map makes the import use the copy
    flp::Project project;
    flp::Channel ch;
    ch.index = 0;
    ch.type = flp::ChannelType::sampler;
    ch.name = "Snare";
    ch.samplePath = "C:\\Producer\\Packs\\snare 7.wav";
    project.channels.push_back (ch);

    lynx::FlpImportOptions options;
    options.fileExists = [] (const std::string& p) { return p == "/found/snare 7.wav"; };
    lynx::FlpImportReport report;
    options.report = &report;
    auto song = lynx::importFlp (project, options);
    CHECK (report.missingFiles.size() == 1);
    CHECK (report.missingFiles.size() == 1 && report.missingFiles[0].path == "C:\\Producer\\Packs\\snare 7.wav" && report.missingFiles[0].usedBy[0] == "Snare");
    CHECK (song.channels.size() == 1 && song.channels[0].samplePath.empty());

    lynx::FlpImportReport again;
    options.report = &again;
    options.relocations = { { "C:\\Producer\\Packs\\snare 7.wav", "/found/snare 7.wav" } };
    song = lynx::importFlp (project, options);
    CHECK (again.missingFiles.empty());
    CHECK (song.channels.size() == 1 && song.channels[0].samplePath == "/found/snare 7.wav" && song.channels[0].instrument == lynx::InstrumentKind::sampler);
}

TEST (project_info_history_and_flp_import)
{
    lynx::Song a;
    auto b = a;
    b.info.title = "Night Drive";
    CHECK (lynx::describeChangeText (a, b) == "Project info");
    auto c = a;
    c.beatsPerBar = 3;
    CHECK (lynx::describeChangeText (a, c) == "Time signature");

    flp::Project project;
    project.title = "Song", project.artist = "Me", project.genre = "House", project.url = "https://x", project.comments = "line 1\rline 2";
    const auto song = lynx::importFlp (project);
    CHECK (song.info.title == "Song" && song.info.author == "Me" && song.info.genre == "House" && song.info.url == "https://x");
    CHECK (song.info.comments == "line 1\nline 2");
}
#include "UiFixTests.inc"

// ---------------------------------------------------------------------------
// Piano Roll tools of round 83 (expected values worked out from FL's formulas, docs/analise/REA_rodada83.md)

namespace
{
lynx::Note prNote (std::int64_t start, std::int64_t length, int key = 60, float velocity = 100.0f / 128.0f)
{
    lynx::Note n;
    n.start = start;
    n.length = length;
    n.key = key;
    n.velocity = velocity;
    return n;
}

std::vector<int> allOf (const lynx::Pattern& p)
{
    std::vector<int> v;
    for (int i = 0; i < (int) p.notes.size(); ++i)
        v.push_back (i);
    return v;
}
} // namespace

TEST (prtools_score_file)
{
    using namespace lynx::prtools;
    FlpBuilder b (96);
    b.ascii (0xC7, "20.9.0");
    b.u8 (0x41); b.u16 (0);
    auto notes = noteRecord (0, 0, 24, 60, 100);
    auto n2 = noteRecord (48, 0, 24, 67, 64);
    n2[19] = 4; // colour 4
    notes.insert (notes.end(), n2.begin(), n2.end());
    b.data (0xE0, notes);
    const auto score = parseScore (b.bytes);
    CHECK (score && score->notes.size() == 2);
    if (score && score->notes.size() == 2)
    {
        CHECK (score->notes[0].start == 0 && score->notes[0].length == 240 && score->notes[0].key == 60);
        CHECK (score->notes[1].start == 480 && score->notes[1].colour == 4 && score->notes[1].velocity == 0.5f);
        CHECK (score->length() == 720);
    }
    CHECK (! parseScore (std::vector<std::uint8_t> { 1, 2, 3 }));

    const auto grid = gridScore (240);
    CHECK (grid.notes.size() == 4 && grid.length() == 960 && grid.notes[3].start == 720);
    CHECK (gridScore (240, 1.0f).notes[1].start == 240 + 120);

    if (! realFlpDir.empty()) // the installation's templates
    {
        std::ifstream f (realFlpDir + "/Patches/Scores/Quantization/Default.fsc", std::ios::binary);
        std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (f)), {});
        const auto fsc = parseScore (bytes);
        CHECK (fsc && fsc->notes.size() == 16 && fsc->length() == 3840 && fsc->notes[1].start == 240);

        // an old (FL 3.5, 20-byte records) chopping pattern with colour-3 notes, read as colour 12
        std::ifstream g (realFlpDir + "/Patches/Scores/Chopping/Default.fsc", std::ios::binary);
        std::vector<std::uint8_t> chopBytes ((std::istreambuf_iterator<char> (g)), {});
        const auto chop = parseScore (chopBytes);
        CHECK (chop && ! chop->notes.empty());
    }
}

TEST (prtools_quantize)
{
    using namespace lynx;
    using namespace lynx::prtools;
    const auto grid = gridScore (240);

    Pattern p;
    p.notes = { prNote (1010, 500), prNote (1130, 100) };
    QuantizeOptions o; // start strength 64/128, sensitivity 36 (180 ticks)
    quantize (p, allOf (p), grid, o);
    CHECK (p.notes[0].start == 985 && p.notes[0].length == 500); // 1010 - (50 * 64) / 128
    CHECK (p.notes[1].start == 1165);                            // toward 1200: 1130 + (70 * 64) / 128

    p.notes = { prNote (1010, 500), prNote (1130, 100) };
    o.start = 1.0f;
    o.sensitivity = 12; // 60 ticks: 1130 is 70 away from 1200
    quantize (p, allOf (p), grid, o);
    CHECK (p.notes[0].start == 960 && p.notes[1].start == 1130);

    p.notes = { prNote (1010, 500) };
    o.sensitivity = 36;
    o.durationMode = QuantizeOptions::leaveEnd;
    quantize (p, allOf (p), grid, o);
    CHECK (p.notes[0].start == 960 && p.notes[0].length == 550); // the end (1509) stays

    p.notes = { prNote (1010, 500) };
    o.durationMode = QuantizeOptions::quantizeEnd;
    o.duration = 1.0f;
    quantize (p, allOf (p), grid, o);
    CHECK (p.notes[0].start == 960 && p.notes[0].length == 1439 - 960 + 1); // end 1509 -> the nearest end 1439

    // levels: a template velocity of 50/100 halves the velocity; 0 removes the note
    Score half = grid;
    for (auto& n : half.notes)
        n.velocity = 50.0f / 128.0f;
    p.notes = { prNote (960, 100, 60, 0.8f) };
    o = {};
    quantize (p, allOf (p), half, o);
    CHECK (std::abs (p.notes[0].velocity - 0.4f) < 1e-5f);
    for (auto& n : half.notes)
        n.velocity = 0.0f;
    quantize (p, allOf (p), half, o);
    CHECK (p.notes.empty());
}

TEST (prtools_chop)
{
    using namespace lynx;
    using namespace lynx::prtools;
    Score pat;
    pat.notes = { ScoreNote { 0, 240, 60 }, ScoreNote { 480, 240, 67 } }; // period 720

    Pattern p;
    p.notes = { prNote (0, 1920, 50) };
    ChopOptions o;
    chop (p, allOf (p), pat, o);
    CHECK (p.notes.size() == 5);
    if (p.notes.size() == 5)
    {
        CHECK (p.notes[0].start == 0 && p.notes[0].key == 50 && p.notes[1].start == 480 && p.notes[1].key == 57);
        CHECK (p.notes[2].start == 720 && p.notes[3].start == 1200 && p.notes[4].start == 1440 && p.notes[4].length == 240);
    }

    // absolute: the pattern stays on the song grid; a piece before the note start is cut
    p.notes = { prNote (100, 600, 50) };
    chop (p, allOf (p), pat, o);
    CHECK (p.notes.size() == 2 && p.notes[0].start == 100 && p.notes[0].length == 140 && p.notes[1].start == 480);
    o.absolute = false;
    p.notes = { prNote (100, 600, 50) };
    chop (p, allOf (p), pat, o);
    CHECK (p.notes.size() == 2 && p.notes[0].start == 100 && p.notes[0].length == 240 && p.notes[1].start == 580
           && p.notes[1].length == 120);

    // time mul x2 (wheel 5); colour 4 holds to the end in the first round only
    o = {};
    o.timeMul = 5;
    p.notes = { prNote (0, 1920, 60) };
    chop (p, allOf (p), pat, o);
    CHECK (p.notes.size() == 3 && p.notes[1].start == 960 && p.notes[1].length == 480 && p.notes[2].start == 1440);
    pat.notes[1].colour = 4;
    o.timeMul = 4;
    p.notes = { prNote (0, 1920, 60) };
    chop (p, allOf (p), pat, o);
    CHECK (p.notes.size() == 4 && p.notes[1].length == 1920 - 480 && p.notes[3].start == 1440);
}

TEST (prtools_flam)
{
    using namespace lynx;
    using namespace lynx::prtools;
    Pattern p;
    p.notes = { prNote (960, 480, 60, 0.75f) };
    flam (p, allOf (p), flamPreset (FlamPreset::doubleStroke), 120.0); // +1 step after
    CHECK (p.notes.size() == 2 && p.notes[0].start == 960 && p.notes[1].start == 1200 && p.notes[1].length == 480);
    CHECK (std::abs (p.notes[1].velocity - 0.75f * 85.0f / 128.0f) < 1e-6f);

    p.notes = { prNote (960, 480) };
    flam (p, allOf (p), flamPreset (FlamPreset::earlyDoubleStroke), 120.0); // the flam one step before, shortened
    CHECK (p.notes.size() == 2 && p.notes[0].start == 720 && p.notes[0].length == 240 && p.notes[1].start == 960);

    // absolute: -64 = -33.3 ms = -64 ticks at 120 BPM (960 PPQ); the original moves, the flam stays on the grid
    CHECK (flamOffset (flamPreset (FlamPreset::flam), 120.0) == -64);
    p.notes = { prNote (960, 480) };
    flam (p, allOf (p), flamPreset (FlamPreset::flam), 120.0);
    CHECK (p.notes.size() == 2 && p.notes[0].start == 960 && p.notes[0].length == 64 && p.notes[1].start == 1024);
    p.notes = { prNote (960, 480) };
    flam (p, allOf (p), flamPreset (FlamPreset::earlyFlam), 120.0);
    CHECK (p.notes.size() == 2 && p.notes[0].start == 896 && p.notes[1].start == 960 && p.notes[1].length == 480);
}

TEST (prtools_lfo)
{
    using namespace lynx;
    using namespace lynx::prtools;
    CHECK (lfoPeriod (20480) == 3840.0 && lfoPeriod (0) == 960.0 * 128 && lfoPeriod (40960) == 120.0);

    LfoOptions o; // sine, phase 1/2: starts at the bottom (value 1/2, range 1/4)
    const auto c = lfoCurve (o, 3840);
    CHECK (std::abs (c[0] - 0.25f) < 1e-4f && std::abs (c[960] - 0.5f) < 1e-3f && std::abs (c[1920] - 0.75f) < 1e-3f);

    o.shape = LfoOptions::square;
    o.phase = 0; // phase 0.5: Round (0.5) = 0 (half to even) -> +1, then -1
    const auto sq = lfoCurve (o, 3840);
    CHECK (sq[0] == 0.75f && sq[10] == 0.25f && sq[2000] == 0.75f);

    o = {};
    o.useEnd = true;
    o.endRange = 0; // the range fades out
    const auto fade = lfoCurve (o, 3840);
    CHECK (std::abs (fade[3839] - 0.5f) < 0.01f);

    Pattern p;
    p.notes = { prNote (0, 100), prNote (960, 100), prNote (1920, 100) };
    lfo (p, allOf (p), notetools::Level::velocity, LfoOptions {});
    CHECK (std::abs (p.notes[0].velocity - 0.25f) < 1e-4f && std::abs (p.notes[1].velocity - 0.5f) < 1e-3f
           && std::abs (p.notes[2].velocity - 0.75f) < 1e-3f);
}

TEST (prtools_claw)
{
    using namespace lynx;
    using namespace lynx::prtools;
    ClawOptions o; // beat, trash the 4th quarter, stretch, remove short notes
    Pattern p;
    p.notes = { prNote (0, 240), prNote (960, 240), prNote (750, 100), prNote (1440, 240) };
    claw (p, allOf (p), o);
    CHECK (p.notes.size() == 3); // the note inside the trashed slice is gone
    if (p.notes.size() == 3)
    {
        CHECK (p.notes[0].start == 0 && p.notes[0].length == 320);
        CHECK (p.notes[1].start == 960 && p.notes[1].length == 320);
        CHECK (p.notes[2].start == 1600 && p.notes[2].length == 320); // 1440 - 240 = 1200 -> x 4/3
    }

    o.trash = 9; // none: only the warp, which is neutral
    p.notes = { prNote (100, 300) };
    claw (p, allOf (p), o);
    CHECK (p.notes.size() == 1 && p.notes[0].start == 100 && p.notes[0].length == 300);

    o.timeDistortion = 1024; // strong bend: the period's ends stay, the middle moves
    p.notes = { prNote (0, 480), prNote (960, 480) };
    claw (p, allOf (p), o);
    CHECK (p.notes[0].start == 0 && p.notes[1].start == 960 && p.notes[0].length > 480 && p.notes[1].length > 480);

    o.trash = 0; // the first quarter
    o.timeDistortion = 0;
    o.stretch = false;
    p.notes = { prNote (0, 240), prNote (240, 240) };
    claw (p, allOf (p), o);
    CHECK (p.notes.size() == 1 && p.notes[0].start == 0 && p.notes[0].length == 240);
}

TEST (prtools_articulate)
{
    using namespace lynx;
    using namespace lynx::prtools;
    Pattern p;
    p.notes = { prNote (0, 100), prNote (480, 100), prNote (960, 100) };
    articulate (p, allOf (p), articulatePreset (ArticulatePreset::legato), 4);
    CHECK (p.notes[0].length == 480 && p.notes[1].length == 480 && p.notes[2].length == 3840 - 960); // to the bar line

    p.notes = { prNote (0, 100), prNote (480, 100), prNote (960, 2880) };
    articulate (p, allOf (p), articulatePreset (ArticulatePreset::staccato), 4);
    CHECK (p.notes[0].length == 96 && p.notes[1].length == 96 && p.notes[2].length == 576); // ends on a bar: own length

    auto o = articulatePreset (ArticulatePreset::legato);
    o.gap = 20;
    p.notes = { prNote (0, 100), prNote (480, 100) };
    articulate (p, allOf (p), o, 4);
    CHECK (p.notes[0].length == 460);

    // chop chords: a held note is cut where the next one starts
    p.notes = { prNote (0, 960, 48), prNote (480, 480, 60) };
    articulate (p, allOf (p), articulatePreset (ArticulatePreset::chopChordsOnly), 4);
    CHECK (p.notes.size() == 3 && p.notes[0].length == 480 && p.notes[1].start == 480 && p.notes[2].start == 480);

    // variation is seeded (MT19937): same seed, same result; lengths only get shorter
    o = articulatePreset (ArticulatePreset::legato);
    o.variation = 50;
    o.seed = 7;
    Pattern a, b2;
    a.notes = b2.notes = { prNote (0, 100), prNote (480, 100), prNote (960, 100) };
    articulate (a, allOf (a), o, 4);
    articulate (b2, allOf (b2), o, 4);
    CHECK (a.notes[0].length == b2.notes[0].length && a.notes[0].length <= 480 && a.notes[0].length >= 240);
}

TEST (prtools_chords)
{
    using namespace lynx::prtools;
    auto c = parseProgression ("I-V-vi-IV", 0, false);
    CHECK (c && c->size() == 4);
    if (c && c->size() == 4)
    {
        CHECK ((*c)[0].root == 0 && (*c)[1].root == 7 && (*c)[2].root == 9 && (*c)[3].root == 5);
        CHECK ((*c)[2].intervals == std::vector<int> ({ 0, 3, 7 }) && chordName ((*c)[2]) == "Am");
    }

    c = parseProgression ("ii 7-V 7-I \xE2\x88\x86-IV/\xE2\x99\xADVII", 0, false);
    CHECK (c && c->size() == 4);
    if (c && c->size() == 4)
    {
        CHECK ((*c)[0].intervals == std::vector<int> ({ 0, 3, 7, 10 }) && chordName ((*c)[0]) == "Dm7");
        CHECK ((*c)[2].intervals == std::vector<int> ({ 0, 4, 7, 11 }) && chordName ((*c)[2]) == "Cmaj7");
        CHECK ((*c)[3].root == 5 && (*c)[3].bass == 10 && chordName ((*c)[3]) == "F/A#");
    }

    c = parseProgression ("i-v-\xE2\x99\xADVI-iv", 9, true); // A minor: Am Em F Dm
    CHECK (c && c->size() == 4 && (*c)[2].root == 5 && (*c)[3].root == 2 && (*c)[1].intervals[1] == 3);
    CHECK (parseProgression ("Isus2 add9", 0, false) && parseProgression ("Isus2 add9", 0, false)->front().intervals
                                                             == std::vector<int> ({ 0, 2, 7, 14 }));
    CHECK (! parseProgression ("I-X-V", 0, false));

    const auto presets = parseProgressionPresets ("-Major\r\n\r\nI-V-vi-IV: Hopeful\r\nI-vi-IV-V: Optimistic\r\n\r\n-Minor\r\n\r\ni-iv-v-i\r\n");
    CHECK (presets.size() == 3 && presets[0].numerals == "I-V-vi-IV" && ! presets[0].minor && presets[2].minor);

    const auto rhythms = parseRhythmPresets ("Default,4,>,-,-,-,>,-,-,-,>,-,-,-,>,-,-,-\nRandom 1,4,r,16,2,1.5\n");
    CHECK (rhythms.size() == 1 && rhythms[0].cells == ">--->--->--->---");

    const auto g = generateProgression (0, false, 8, 0.3f, 5);
    CHECK (g.size() == 8 && g[0].root == 0 && g == generateProgression (0, false, 8, 0.3f, 5));
    for (std::uint32_t seed = 1; seed < 40; ++seed) // numerals read back as the same chords
    {
        const auto gen = generateProgression (2, seed % 2 == 0, 8, 1.0f, seed);
        const auto back = parseProgression (toNumerals (gen, 2), 2, seed % 2 == 0);
        CHECK (back && *back == gen);
    }
    CHECK (toNumerals (*parseProgression ("I-V-vi-IV/bVII", 0, false), 0) == "I-V-vi-IV/bVII");

    lynx::Pattern p;
    const auto chords = *parseProgression ("I-IV", 0, false);
    ChordWriteOptions w;
    const auto added = writeProgression (p, 2, 0, chords, w);
    CHECK (added.size() == 8 && p.notes.size() == 8 && p.notes[0].channel == 2);
    CHECK (std::any_of (p.notes.begin(), p.notes.end(), [] (auto& n) { return n.key == 48; })); // bass C3
    w.rhythm = ">-`->-`->-`->-`-";
    lynx::Pattern r;
    writeProgression (r, 0, 0, chords, w);
    CHECK (r.notes.size() == 8 && r.notes[0].length == 1920); // '>' + '-' of 960-tick cells
}

TEST (prtools_riff_machine)
{
    using namespace lynx;
    using namespace lynx::prtools;
    Pattern p;
    p.notes = { prNote (0, 100, 70) };
    p.notes[0].channel = 1;
    RiffOptions o;
    o.channel = 1;
    o.bars = 2;
    const auto notes = riffMachine (p, { 0 }, o, 4);
    CHECK (! notes.empty() && notes.size() == p.notes.size());
    CHECK (std::all_of (p.notes.begin(), p.notes.end(), [] (auto& n) {
        return n.channel == 1 && n.start >= 0 && n.start < 2 * 3840 && n.key >= 36 && n.key <= 84;
    }));

    // only the first step: the progression shape, chopped from one note at key 48
    o.stepsUpTo = 0;
    Pattern q;
    const auto first = riffMachine (q, {}, o, 4);
    CHECK (first.size() == builtInRiffProgression (0).notes.size() * 2 && q.notes[0].key == 48);
}

TEST (event_editor_lanes)
{
    using namespace lynx;
    Pattern p;
    const AutomationTarget vol { AutomationTargetKind::channelVolume, 2 };
    auto& lane = eventedit::laneFor (p, vol);
    CHECK (p.events.size() == 1 && &eventedit::laneFor (p, vol) == &lane && eventedit::findLane (p, { AutomationTargetKind::channelPan, 2 }) == nullptr);

    eventedit::fill (lane, 0, 960, 0.25f, 240);
    CHECK (lane.events.size() == 4 && lane.events[3].tick == 720 && lane.events[3].value == 0.25f);
    CHECK (lane.valueAt (-5.0) == 0.25f && lane.valueAt (5000.0) == 0.25f);

    eventedit::line (lane, 960, 1920, 0.0f, 1.0f, 240); // 4 cells: 0, 1/3, 2/3, 1
    CHECK (lane.events.size() == 8 && lane.events[4].value == 0.0f && lane.events[7].value == 1.0f);
    CHECK (std::abs (lane.valueAt (1500.0) - 2.0f / 3.0f) < 1e-6f); // held from 1440

    eventedit::setEvent (lane, 1440, 0.1f);
    CHECK (lane.events.size() == 8 && lane.valueAt (1440.0) == 0.1f);
    eventedit::setEvent (lane, 100, 0.9f);
    CHECK (lane.events.size() == 9 && lane.events[1].tick == 100);

    eventedit::scale (lane, 0, 960, 2.0f, 0.0f, false);
    CHECK (lane.events[0].value == 0.5f && lane.events[1].value == 1.0f); // clamped
    eventedit::scale (lane, 0, 960, 0.0f, 0.0f, true); // bipolar: toward the centre
    CHECK (lane.events[0].value == 0.5f && lane.events[2].value == 0.5f);

    eventedit::erase (lane, 0, 960);
    CHECK (lane.events.size() == 4 && lane.events.front().tick == 960);

    auto copy = lane;
    eventedit::randomize (lane, 0, 4000, 0.2f, 3);
    eventedit::randomize (copy, 0, 4000, 0.2f, 3);
    CHECK (lane.events[1].value == copy.events[1].value && std::abs (lane.events[1].value - 1.0f / 3.0f) <= 0.2f + 1e-6f);

    prtools::LfoOptions o;
    eventedit::applyLfo (lane, 0, 3840, o, 960); // FL's LFO: bottom, middle, top, middle
    CHECK (lane.events.size() == 4 && std::abs (lane.events[0].value - 0.25f) < 1e-4f && std::abs (lane.events[2].value - 0.75f) < 1e-3f);

    p.events.push_back ({ { AutomationTargetKind::channelPan, 1 }, {} });
    eventedit::removeEmptyLanes (p);
    CHECK (p.events.size() == 1);

    // undo label
    auto before = Song::makeDefault();
    auto after = before;
    after.patterns[0].events.push_back ({ vol, { { 0, 0.5f } } });
    CHECK (describeChangeText (before, after) == "Edit events: " + after.patterns[0].name);

    // channel removal re-targets the lanes like automations
    auto song = Song::makeDefault();
    song.patterns[0].events.push_back ({ { AutomationTargetKind::channelVolume, 3 }, { { 0, 0.5f } } });
    song.removeChannel (1);
    CHECK (song.patterns[0].events[0].target.index == 2);
}

TEST (event_editor_playback)
{
    // pattern events on the Master volume: silent first half of each pattern, sound in the second half
    auto song = lynx::Song::makeDefault();
    song.bpm = 120.0;
    const auto bar = (std::int64_t) lynx::ppq * 4;
    song.patterns[0].lengthTicks = bar;
    song.patterns[0].notes.clear();
    for (int s = 0; s < 16; ++s)
        song.patterns[0].notes.push_back ({ s * 240, 200, 0, 60 });
    song.patterns[0].events.push_back ({ { lynx::AutomationTargetKind::mixerVolume, 0 }, { { 0, 0.0f }, { bar / 2, 0.8f } } });

    const auto peaks = [&] (lynx::PlayMode mode) {
        song.mode = mode;
        song.playlist.clear();
        song.playlist.push_back ({ 0, 0, bar, bar, 0 }); // one clip, in the second bar
        const double sr = 48000.0;
        auto state = lynx::RackState::build (song, nullptr, sr);
        lynx::RackRenderer r;
        r.prepare (sr, 512);
        std::vector<float> l (512), rr (512);
        const auto tps = lynx::ticksPerSample (song.bpm, sr);
        std::array<float, 6> peak {}; // half bars
        for (double tick = 0.0; tick < (double) (bar * 3); tick += 512 * tps)
        {
            r.process (state.get(), true, tick, 512, l.data(), rr.data());
            // blocks inside a half bar, away from its edges (the gain glides over one chunk at a change)
            const auto blockEnd = tick + 512 * tps;
            const auto half = (std::size_t) (tick / (double) (bar / 2));
            const auto edge = (double) half * (double) (bar / 2);
            if (half > 5 || tick - edge < 2 * 512 * tps || blockEnd > edge + (double) (bar / 2))
                continue;
            for (auto v : l)
                peak[half] = std::max (peak[half], std::abs (v));
        }
        return std::make_pair (state->eventLanes.size(), peak);
    };

    const auto [lanesSong, song3] = peaks (lynx::PlayMode::song);
    CHECK (lanesSong == 1);
    CHECK (song3[2] < 1.0e-4f && song3[3] > 0.01f); // inside the clip: silent, then sound

    const auto [lanesPattern, loop] = peaks (lynx::PlayMode::pattern);
    CHECK (lanesPattern == 1);
    CHECK (loop[0] < 1.0e-4f && loop[1] > 0.01f && loop[2] < 1.0e-4f && loop[3] > 0.01f);
}
TEST (flp_pattern_events_import)
{
    // REA_rodada85 §1: event 0xDF holds a pattern's events {u32 tick, u32 controlID, i32 raw value}, 0xD8 the values
    // reset when the song starts; they become pattern event lanes in FL's normalised scale
    FlpBuilder b;
    b.u8 (0x40); b.u16 (0); // channel 0
    b.u8 (0x15); b.u8 (0);  // sampler
    b.u8 (0x41); b.u16 (1); // pattern 1: events only, no notes
    auto rec = [] (std::vector<std::uint8_t>& v, std::uint32_t tick, std::uint32_t id, std::int32_t value) {
        for (auto w : { tick, id, (std::uint32_t) value })
            for (int i = 0; i < 4; ++i)
                v.push_back ((std::uint8_t) (w >> (8 * i)));
    };
    std::vector<std::uint8_t> events;
    rec (events, 0, 0x70001FC0u, 16000);  // Master volume: top
    rec (events, 96, 0x70001FC0u, 8000);
    rec (events, 96, 0x70001FC0u, 4000);  // same tick: the last one wins
    rec (events, 0, 0x00000001u, 0);      // channel 0 pan: hard left
    rec (events, 48, 0x00000001u, 12800); // hard right
    rec (events, 0, 0x00008000u, 5);      // a plugin parameter of a sampler: scale unknown, skipped
    b.data (0xDF, events);
    std::vector<std::uint8_t> init;
    rec (init, 0, 0x70001FC1u, -6400);    // Master pan reset to hard left at song start
    b.data (0xD8, init);

    const auto project = flp::parseProject (b.bytes);
    CHECK (project.initControls.size() == 1);
    const flp::Pattern* src = nullptr;
    for (const auto& p : project.patterns)
        if (p.index == 1)
            src = &p;
    CHECK (src != nullptr && src->controlEvents.size() == 6);

    lynx::FlpImportReport report;
    lynx::FlpImportOptions options;
    options.report = &report;
    const auto song = lynx::importFlp (project, options);
    const lynx::PatternEvents* volume = nullptr;
    const lynx::PatternEvents* pan = nullptr;
    int lanes = 0;
    for (const auto& p : song.patterns)
        for (const auto& lane : p.events)
        {
            ++lanes;
            if (lane.target.kind == lynx::AutomationTargetKind::mixerVolume && lane.target.index == 0)
                volume = &lane;
            if (lane.target.kind == lynx::AutomationTargetKind::channelPan)
                pan = &lane;
        }
    CHECK (lanes == 2 && volume != nullptr && pan != nullptr);
    if (volume != nullptr && volume->events.size() == 2)
    {
        CHECK (volume->events[0].tick == 0 && volume->events[1].tick == lynx::ppq);
        CHECK (volume->events[0].value > volume->events[1].value); // 4000 (not 8000) is the one kept
    }
    else
        CHECK (false);
    if (pan != nullptr && pan->events.size() == 2)
        CHECK (pan->events[0].value < 0.01f && pan->events[1].value > 0.99f && pan->events[1].tick == lynx::ppq / 2);
    else
        CHECK (false);
    CHECK (report.converted["Pattern events"] == 2);
    CHECK (song.targetValue ({ lynx::AutomationTargetKind::mixerPan, 0 }) < 0.01f);
}

#include "PrefsTests.inc"
#include "core/ProjectSafety.h"
#include "core/SearchIndex.h"
#include "core/PluginCatalog.h"
#include "SafetyTests118.inc"
#include "Rea134Tests.inc"
#include "Rea139Tests.inc"
#include "Rea143Tests.inc"
#include "Rea143aTests.inc"
#include "Rea143bTests.inc"
#include "Rea146aTests.inc"
#include "Rea146bTests.inc"
#include "Rea146Tests.inc"
#include "Rea149Tests.inc"
#include "PluginCatalogTests151.inc"
#include "Rea151Tests.inc"
#include "AudioEditTests.inc"
#include "InputTests.inc"
#include "../asio/InputDrift.h"
#include "AsioDriftTests.inc"

TEST (level_audit_demo_imports_round89)
{
    // REA_rodada89 §4: two demo projects whose FL renders were much louder than LYNX's
    if (realFlpDir.empty())
        return;
    auto load = [] (const std::filesystem::path& path) -> std::optional<lynx::Song> {
        if (! std::filesystem::exists (path))
            return std::nullopt;
        std::ifstream in (path, std::ios::binary);
        const std::vector<std::uint8_t> file ((std::istreambuf_iterator<char> (in)), {});
        return lynx::importFlp (flp::parseProject (file));
    };
    const auto demos = std::filesystem::path (realFlpDir) / "Demo projects";

    // FL 12+: a route to insert 100+ without a level record plays at 100% (the percussion bus of "Lightning")
    if (const auto song = load (demos / "Product demos" / "Square a Saw - Lightning (VEELA Vocals Resynthesized).flp"))
    {
        int highRoutes = 0, highAtZero = 0;
        for (const auto& t : song->mixer)
            for (const auto& send : t.sends)
                if (send.target >= 100)
                {
                    ++highRoutes;
                    highAtZero += send.level <= 0.0f;
                }
        std::printf ("  Lightning: %d routes to inserts 100+, %d at level 0\n", highRoutes, highAtZero);
        CHECK (highRoutes > 0 && highAtZero == 0);
    }

    // Fruity Compressors FL saved as VST2 inside Fruity Wrapper (RawFL): the exact engine with the saved controls
    if (const auto song = load (demos / "Visual" / "ZGameEditor Visualizer" / "Treben - Gimbal and Sinan - RawFL.flp"))
    {
        int compressors = 0, withSettings = 0;
        for (const auto& t : song->mixer)
            for (const auto& fx : t.effects)
                if (fx.kind == lynx::EffectKind::fruityCompressor)
                {
                    ++compressors;
                    const auto defaults = lynx::makeEffectSlot (lynx::EffectKind::fruityCompressor);
                    withSettings += fx.params != defaults.params;
                }
        std::printf ("  RawFL: %d wrapped Fruity Compressors on the exact engine, %d with their own settings\n", compressors, withSettings);
        CHECK (compressors >= 10 && withSettings >= 8);
    }
}
#include "UiLook90Tests.inc"
#include "UiFl104Tests.inc"
#include "DialogsTests84.inc"
#include "SubstituteTests.inc"
#include "MixSuiteTests96.inc"
#include "PanelsTests105.inc"

// --- native plugin editors (round 93): FL's arrangements ---------------------------------------------------------

TEST (plugin_layouts_name_real_parameters)
{
    using namespace lynx;
    // every value control edits a parameter of its effect, inside the body; FL arrangements place (or list in a menu)
    // every parameter of the effect
    for (const auto kind : plugui::flLayoutKinds())
    {
        const auto& l = plugui::layoutFor (kind);
        const auto& info = effectInfo (kind);
        CHECK (plugui::hasFlLayout (kind));
        CHECK (l.width > 100 && l.height > 60);
        std::vector<bool> placed ((std::size_t) plugui::paramCount (kind), false);

        for (const auto& item : l.items)
        {
            CHECK (item.x >= 0.0f && item.y >= 0.0f && item.x + item.w <= (float) l.width + 0.5f && item.y + item.h <= (float) l.height + 0.5f);
            if (item.param != nullptr)
            {
                const auto p = plugui::paramIndex (kind, item.param);
                CHECK (p >= 0);
                if (p >= 0)
                    placed[(std::size_t) p] = true;
            }
            if (item.type == plugui::Item::radio || item.type == plugui::Item::tabs || item.type == plugui::Item::menu)
            {
                const auto labels = plugui::optionLabels (item);
                CHECK (labels.size() >= 2);
                CHECK (item.values.empty() || item.values.size() == labels.size());
            }
            if (item.type == plugui::Item::knob && item.h + 0.5f < item.w) // (a caption above its knob would hide it)
            {
                std::printf ("  %s: knob '%s' is shorter than it is wide\n", info.name, item.param != nullptr ? item.param : "");
                CHECK (false);
            }
            CHECK (item.param2 == nullptr || plugui::paramIndex (kind, item.param2) >= 0);
            if (item.param2 != nullptr && plugui::paramIndex (kind, item.param2) >= 0)
                placed[(std::size_t) plugui::paramIndex (kind, item.param2)] = true;
            for (const auto* when : { item.whenParam, item.when2Param })
                CHECK (when == nullptr || plugui::paramIndex (kind, when) >= 0);
        }
        for (const auto* name : l.menuSwitches)
        {
            const auto p = plugui::paramIndex (kind, name);
            CHECK (p >= 0);
            if (p >= 0)
                placed[(std::size_t) p] = true;
        }
        for (const auto* name : l.editedElsewhere)
        {
            const auto p = plugui::paramIndex (kind, name);
            CHECK (p >= 0);
            if (p >= 0)
                placed[(std::size_t) p] = true;
        }
        for (std::size_t p = 0; p < placed.size(); ++p)
            if (! placed[p])
            {
                std::printf ("  %s: parameter '%s' has no control\n", info.name, plugui::paramName (kind, (int) p));
                CHECK (false);
            }
    }
}

TEST (maximus_editor_edits_the_slot_state)
{
    using namespace lynx;
    // the Maximus page edits the 59 parameters stored in the slot's state (REA_rodada8 §1.2)
    std::array<std::array<int, 13>, 4> bands {};
    for (auto& b : bands)
        b = { 0, 1000, 1000, 0, 0, 1000, 1115, 0, 2, 5000, 3, 2882, 0 };
    std::array<std::vector<std::array<double, 4>>, 8> envs;
    for (auto& e : envs)
        e = { { 0.0, 0.0, 0.0, 2 }, { 0.5, 0.5, 0.0, 2 }, { 0.5, 0.5, 0.0, 2 } };
    const auto state = maximusTestState ({ 19800, 1, 48065, 1, 1115, 1000, 32768 }, bands, envs, 0);
    const auto slot = maximusfx::pack ({ hlfx::MaximusBuild::shared, -1, state });

    CHECK (maximusfx::stateParams().size() == 59);
    CHECK (plugui::paramIndex (EffectKind::maximus, "Master release") == 7 + 13 * 3 + 9);
    CHECK (maximusfx::stateParam (slot, 0).value_or (-1) == 19800);
    CHECK (maximusfx::stateParam (slot, 7 + 13 + 9).value_or (-1) == 5000);
    CHECK (maximusfx::stateParamText (0, 19800) == "200 Hz");
    CHECK (maximusfx::stateParamText (2, 48065) == "3000 Hz");
    CHECK (maximusfx::stateParamText (8, 1000) == "+0.0 dB");
    CHECK (maximusfx::stateParamText (7 + 6, 0) == "Default");
    CHECK (maximusfx::stateParamText (7 + 13 * 3 + 6, 1115) == "2.00 ms");
    CHECK (maximusfx::stateParamText (6, -1) == "Off");

    const auto edited = maximusfx::withStateParam (slot, 7 + 13 + 1, 2000); // mid pre gain → +18 dB
    CHECK (maximusfx::stateParam (edited, 7 + 13 + 1).value_or (-1) == 2000);
    CHECK (maximusfx::stateParam (edited, 7 + 1).value_or (-1) == 1000);
    CHECK (maximusfx::stateParam (maximusfx::withStateParam (slot, 0, 99999), 0).value_or (-1) == 65536); // clamped
    const auto blob = maximusfx::unpack (edited);
    const auto original = maximusfx::unpack (slot);
    CHECK (blob && original && (hlfx::makeMaximusPreset (blob->state) != nullptr) == (hlfx::makeMaximusPreset (original->state) != nullptr));

    const auto curve = maximusfx::curvePoints (slot, 3);
    CHECK (curve.size() == 3 && std::abs (curve[1].first - 0.5f) < 1e-6f && std::abs (curve[2].first - 1.0f) < 1e-6f);
    CHECK (maximusfx::selectedBand (slot) >= 0);
    CHECK (plugui::hasFlLayout (EffectKind::maximus));
}

TEST (plugin_layout_options_match_the_engine_values)
{
    using namespace lynx;
    // the switches show FL's captions for the engine's own values
    const auto text = [] (EffectKind kind, const char* param, int option) {
        for (const auto& item : plugui::layoutFor (kind).items)
            if (item.param != nullptr && std::strcmp (item.param, param) == 0 && item.options != nullptr)
                return formatEffectParam (kind, plugui::paramIndex (kind, param), plugui::optionValue (item, option), 120.0);
        return std::string ("?");
    };
    CHECK (text (EffectKind::fruityDelay3, "Delay type", 0) == "Fake stereo");
    CHECK (text (EffectKind::fruityDelay3, "Delay type", 2) == "Ping pong");
    CHECK (text (EffectKind::fruityDelay3, "Delay type", 3) == "Off");
    CHECK (text (EffectKind::fruityDelay3, "Filter mode", 1) == "High pass");
    CHECK (text (EffectKind::fruityDelay3, "Saturation type", 1) == "Saturate");
    CHECK (text (EffectKind::fruityMultiband, "Low mode", 1) == "off");
    CHECK (text (EffectKind::fruityMultiband, "High mode", 2) == "bypass");
    CHECK (text (EffectKind::luxeVerb, "Pitch type", 1) == "feed-forward");
    CHECK (text (EffectKind::luxeVerb, "Freeze mode", 2) == "sustain");
    CHECK (text (EffectKind::stereoEnhancer, "Invert", 2) == "invert R");
    CHECK (text (EffectKind::parametricEq, "B3 type", 4) == formatEffectParam (EffectKind::parametricEq, peq::param (2, 0), peq::typeValue (peq::BandType::lowCut), 120.0));

    // the option nearest to a value; pages and conditions
    plugui::LayoutItem radio;
    radio.type = plugui::Item::radio;
    radio.options = "A|B|C|D";
    CHECK (plugui::optionLabels (radio).size() == 4);
    CHECK (plugui::optionAt (radio, 0.30f) == 1);
    CHECK (plugui::optionAt (radio, 0.95f) == 3);
    CHECK (std::abs (plugui::optionValue (radio, 2) - 2.0f / 3.0f) < 1e-6f);

    const auto& lux = plugui::layoutFor (EffectKind::luxeVerb);
    auto params = makeEffectSlot (EffectKind::luxeVerb).params;
    const auto pitchType = plugui::paramIndex (EffectKind::luxeVerb, "Pitch type");
    int feedForward = 0, feedback = 0;
    for (float v : { 0.0f, 1.0f })
    {
        params[(std::size_t) pitchType] = v;
        for (const auto& item : lux.items)
            if (item.param != nullptr && std::strcmp (item.param, "Feed-forward pitch 1") == 0 && plugui::isShown (EffectKind::luxeVerb, item, 0, params))
                ++feedForward;
            else if (item.param != nullptr && std::strcmp (item.param, "Feedback pitch") == 0 && plugui::isShown (EffectKind::luxeVerb, item, 0, params))
                ++feedback;
    }
    CHECK (feedForward == 1); // only in feed-forward mode
    CHECK (feedback == 2);    // value plate + fader, only in feedback mode

    const auto& limiter = plugui::layoutFor (EffectKind::fruityLimiter);
    CHECK (limiter.pages.size() == 2);
    int ceilingShown = 0;
    for (int page = 0; page < 2; ++page)
        for (const auto& item : limiter.items)
            if (item.param != nullptr && std::strcmp (item.param, "Ceiling") == 0 && plugui::isShown (EffectKind::fruityLimiter, item, page, params))
                ++ceilingShown;
    CHECK (ceilingShown == 1); // LIMIT page only

    // every other effect still gets a (generated) window
    CHECK (! plugui::hasFlLayout (EffectKind::distortion));
    CHECK (! plugui::layoutFor (EffectKind::distortion).items.empty());
}
#include "FabMeasureTests99.inc"
#include "DynamicsTests108.inc"
#include "LynxTuneTests95.inc"
#include "LynxVerbTests100.inc"
#include "FacetTests103.inc"
#include "VocalLevel107Tests.inc"
#include "Substitutes113Tests.inc"
#include "Tune120Tests.inc"
#include "EchoSuiteTests101.inc"
#include "FruitySendTests105.inc"
#include "CrashFuzzTests111.inc"
#include "LookTests114.inc"
#include "FixedBlockTests118.inc"
#include "UiFl115Tests.inc"
#include "LoadTraceTests112.inc"
#include "VoiceToolsTests119.inc"
#include "VoiceMatchTests123.inc"
#include "LoadTests121.inc"
#include "Parity128Tests.inc"
#include "Parity128aTests.inc"
#include "Parity128bTests.inc"
#include "Parity128cTests.inc"
#include "Match124Tests.inc"
#include "Match124xTests.inc"
#include "Vmatch127Tests.inc"
#include "ParityTests129.inc"
#include "Parity135vTests.inc"
#include "Bugs131Tests.inc"
#include "Parity135wTests.inc"
#include "Feat133Tests.inc"
#include "Parity135fTests.inc"
#include "Feat132Tests.inc"
#include "Parity135sTests.inc"
#include "VoiceMatch136Tests.inc"
#include "Parity137Tests.inc"
#include "Parity144Tests.inc"
#include "Parity144aTests.inc"
#include "Parity144bTests.inc"
#include "Parity147Tests.inc"
#include "Parity147aTests.inc"
#include "Parity147bTests.inc"
#include "Parity150Tests.inc"
#include "Parity150aTests.inc"
#include "Parity150bTests.inc"
#include "Parity152Tests.inc"
#include "Parity152aTests.inc"
#include "Parity152bTests.inc"
#include "Parity152cTests.inc"
#include "Patcher141Tests.inc"
#include "Patcher162Tests.inc"
#include "Vox138Tests.inc"
#include "Vox145Tests.inc"
#include "Vox148Tests.inc"
#include "Natives142Tests.inc"

TEST (emphasizer_matches_fl_engine)
{
    // REA_rodada154: FL's Emphasizer through the import (state → slot → effect) against goldens of the installation's
    // Emphasizer_x64.dll (rate set before the state, 2304 samples of silence as FL's mixer runs it before a render,
    // 512-frame calls, 44.1 kHz): FNV-1a of the output's float bits
    auto signal = [] {
        std::vector<float> l (24000), r (24000);
        std::uint32_t s = 12345;
        const float amps[] { 0.3f, 1.5f, 0.05f, 3.0f, 0.8f, 0.0f, 2.0f, 0.4f };
        for (int i = 0; i < 24000; ++i)
        {
            const float a = amps[(i / 3000) % 8];
            s = s * 1664525u + 1013904223u;
            l[(std::size_t) i] = (float) ((s >> 8) / 8388608.0 - 1.0) * a;
            s = s * 1664525u + 1013904223u;
            r[(std::size_t) i] = (float) ((s >> 8) / 8388608.0 - 1.0) * a;
        }
        return std::pair { l, r };
    };
    struct Case { std::array<int, 6> raw; std::uint64_t hash; };
    const Case cases[] { { { 0, 5013, 0, 0, 0, 10000 }, 0xa009608f060eb32eull }, { { 6000, 9000, 1, 0, 0, 8000 }, 0x6fc7ec82cc9d9c01ull },
                         { { 8000, 2000, 0, 1, 0, 10000 }, 0xb10a1f3bd998198cull }, { { 3000, 5013, 1, 0, 1, 10000 }, 0x57a8fa4d8b890ac0ull } };
    for (const auto& c : cases)
    {
        std::vector<std::uint8_t> state (4 + 24 + 1, 0);
        const std::int32_t version = 2;
        std::memcpy (state.data(), &version, 4);
        std::memcpy (state.data() + 4, c.raw.data(), 24);
        state[28] = 1;
        const auto slot = lynx::effectFromFl ("Emphasizer", state);
        CHECK (slot && slot->kind == lynx::EffectKind::emphasizer);
        if (! slot)
            continue;
        auto fx = lynx::makeEffect (slot->kind);
        fx->prepare (44100.0);
        auto [l, r] = signal();
        for (int i = 0; i < 24000; i += 512) // the golden's 512-frame engine calls (level matching is per call)
            fx->process (l.data() + i, r.data() + i, std::min (512, 24000 - i), slot->params, 120.0);
        std::uint64_t h = 0xcbf29ce484222325ull;
        for (int i = 0; i < 24000; ++i)
            for (float v : { l[(std::size_t) i], r[(std::size_t) i] })
            {
                std::uint32_t u;
                std::memcpy (&u, &v, 4);
                h = (h ^ u) * 0x100000001b3ull;
            }
        CHECK (h == c.hash);
    }
    // version 1 (no output control) keeps the 0 dB default; texts
    const std::vector<std::uint8_t> v1 { 1, 0, 0, 0, 0, 0, 0, 0, 0x95, 0x13, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    const auto s1 = lynx::emphasizer::parse (v1);
    CHECK (s1 && s1->raw[1] == 5013 && s1->raw[5] == 10000);
    CHECK (lynx::formatEffectParam (lynx::EffectKind::emphasizer, 0, 0.5f, 120.0) == "+12.0 dB");
    CHECK (lynx::formatEffectParam (lynx::EffectKind::emphasizer, 5, 1.0f, 120.0) == "0.0 dB");
    CHECK (lynx::formatEffectParam (lynx::EffectKind::emphasizer, 2, 1.0f, 120.0) == "Soft");
}

TEST (fl_frequency_shifter_matches_fl_engine)
{
    // REA_rodada154 §4: FL's Frequency Shifter (import → slot → effect) against goldens of the installation's
    // Engine_x64.dll driven by its front end (rate before the state, 1536 samples of silence first, transport stopped, 120 BPM): FNV-1a
    // of the output
    struct Case { std::array<int, 10> raw; double rate; std::uint64_t hash; };
    const Case cases[] { { { 10000, 0, -60480, 0, 10000, 10000, 5000, 10000, 1, 5000 }, 44100.0, 0x462af5f13683c90eull },
                         { { 7000, 2, 40000, 3000, 10000, 10000, 8000, 6000, 0, 2000 }, 44100.0, 0xcd597b95bf1bbb1aull },
                         { { 6000, 1, -30000, 6000, 3000, 7000, 3000, 4000, 0, 5000 }, 48000.0, 0x5592232115cb3acull },
                         { { 10000, 0, 51542, 2000, 8000, 10000, 5000, 10000, 0, 5000 }, 48000.0, 0x63d3051ba14bd00full } };
    for (const auto& c : cases)
    {
        std::vector<std::uint8_t> state (4 + 40 + 8, 0);
        const std::int32_t version = 4;
        std::memcpy (state.data(), &version, 4);
        std::memcpy (state.data() + 4, c.raw.data(), 40);
        const auto slot = lynx::effectFromFl ("Frequency Shifter", state);
        CHECK (slot && slot->kind == lynx::EffectKind::flFrequencyShifter);
        if (! slot)
            continue;
        auto fx = lynx::makeEffect (slot->kind);
        fx->prepare (c.rate);
        std::vector<float> l (24000), r (24000);
        std::uint32_t s = 12345;
        const float amps[] { 0.3f, 1.5f, 0.05f, 3.0f, 0.8f, 0.0f, 2.0f, 0.4f };
        for (int i = 0; i < 24000; ++i)
        {
            const float a = amps[(i / 3000) % 8];
            s = s * 1664525u + 1013904223u;
            l[(std::size_t) i] = (float) ((s >> 8) / 8388608.0 - 1.0) * a;
            s = s * 1664525u + 1013904223u;
            r[(std::size_t) i] = (float) ((s >> 8) / 8388608.0 - 1.0) * a;
        }
        for (int i = 0; i < 24000; i += 800)
            fx->process (l.data() + i, r.data() + i, 800, slot->params, 120.0);
        std::uint64_t h = 0xcbf29ce484222325ull;
        for (int i = 0; i < 24000; ++i)
            for (float v : { l[(std::size_t) i], r[(std::size_t) i] })
            {
                std::uint32_t u;
                std::memcpy (&u, &v, 4);
                h = (h ^ u) * 0x100000001b3ull;
            }
        CHECK (h == c.hash);
    }
    // the engine's tanhf against points of the library's
    CHECK (lynx::fsfl::tanhf_ (0.0f) == 0.0f && lynx::fsfl::tanhf_ (20.0f) == 1.0f && lynx::fsfl::tanhf_ (-20.0f) == -1.0f);
    CHECK (std::fabs (lynx::fsfl::tanhf_ (0.5f) - 0.462117165f) < 1e-7f && std::fabs (lynx::fsfl::tanhf_ (2.0f) - 0.964027584f) < 1e-7f);
    CHECK (lynx::formatEffectParam (lynx::EffectKind::flFrequencyShifter, 1, 1.0f, 120.0) == "Ring modulator");
}

TEST (fl_low_lifter_matches_fl_engine)
{
    // REA_rodada154 §5: FL's Low Lifter (import → slot → effect, 512-frame calls) against goldens of the installation's
    // engine (ildsp_x64.dll's Faust program driven by Low Lifter_x64.dll, rate before the state): FNV-1a of the output
    struct Case { std::array<int, 15> raw; double rate; std::uint64_t hash; };
    const Case cases[] { { { 1, 10000, 4999, 7210, 1, 0, 0, 0, 5000, 10000, 7750, 0, 0, 1, 1 }, 44100.0, 0xcbb7eae8f31ca6d9ull },
                         { { 1, 7506, 5000, 3277, 1, 0, 6757, 5053, 9608, 10000, 7778, 5281, 1, 1, 1 }, 44100.0, 0xdb2d3712d4fb16c5ull },
                         { { 1, 5095, 6737, 5109, 1, 1, 541, 1479, 5451, 8197, 690, 6833, 1, 1, 1 }, 48000.0, 0x273c262202984447ull } };
    for (const auto& c : cases)
    {
        std::vector<std::uint8_t> state (4 + 60 + 18, 0);
        const std::int32_t version = 6;
        std::memcpy (state.data(), &version, 4);
        std::memcpy (state.data() + 4, c.raw.data(), 60);
        const auto slot = lynx::effectFromFl ("Low Lifter", state);
        CHECK (slot && slot->kind == lynx::EffectKind::flLowLifter);
        if (! slot)
            continue;
        auto fx = lynx::makeEffect (slot->kind);
        fx->prepare (c.rate);
        std::vector<float> l (24000), r (24000);
        std::uint32_t s = 12345;
        const float amps[] { 0.3f, 1.5f, 0.05f, 3.0f, 0.8f, 0.0f, 2.0f, 0.4f };
        for (int i = 0; i < 24000; ++i)
        {
            const float a = amps[(i / 3000) % 8];
            s = s * 1664525u + 1013904223u;
            l[(std::size_t) i] = (float) ((s >> 8) / 8388608.0 - 1.0) * a;
            s = s * 1664525u + 1013904223u;
            r[(std::size_t) i] = (float) ((s >> 8) / 8388608.0 - 1.0) * a;
        }
        for (int i = 0; i < 24000; i += 512)
            fx->process (l.data() + i, r.data() + i, std::min (512, 24000 - i), slot->params, 120.0);
        std::uint64_t h = 0xcbf29ce484222325ull;
        for (int i = 0; i < 24000; ++i)
            for (float v : { l[(std::size_t) i], r[(std::size_t) i] })
            {
                std::uint32_t u;
                std::memcpy (&u, &v, 4);
                h = (h ^ u) * 0x100000001b3ull;
            }
        CHECK (h == c.hash);
    }
    CHECK (lynx::formatEffectParam (lynx::EffectKind::flLowLifter, 10, 0.775f, 120.0) == "80 Hz");
    CHECK (lynx::formatEffectParam (lynx::EffectKind::flLowLifter, 3, 0.721f, 120.0) == "-6.0 dB");
}
#include "Rea153Tests.inc"
#include "Render155Tests.inc"
#include "Tune157Tests.inc"
#include "Tune165Tests.inc"
#include "HwOut156Tests.inc"
#include "SoundFont158Tests.inc"
#include "Mp3157Tests.inc"
#include "Quality160Tests.inc"
#include "CrashReport159Tests.inc"
#include "Sampler162Tests.inc"
#include "Workflow163cTests.inc"
#include "ClipPitch164Tests.inc"
#include "Misc164mTests.inc"
#include "Theme162Tests.inc"
#include "PluginPresets162Tests.inc"
#include "Look162Tests.inc"
#include "EnvController162Tests.inc"
#include "Browser162Tests.inc"
#include "BrowserView163Tests.inc"
#include "Transport162Tests.inc"
#include "Workflow163Tests.inc"
#include "Rec163Tests.inc"
#include "Ui163Tests.inc"
#include "HwInsert164Tests.inc"
#include "Layer164Tests.inc"
#include "Patcher164vTests.inc"
#include "Vfx164Tests.inc"
#include "Misc164Tests.inc"
#include "Visual164Tests.inc"
#include "Patcher164fTests.inc"
#include "LinuxMouse165Tests.inc"
#include "Fl165Tests.inc"
