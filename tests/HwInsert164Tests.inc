// --- Hardware insert: send to device outputs, return from inputs, pinged round trip (core/HardwareInsert.h) - round 164d

#include "core/HardwareInsert.h"

#include <random>

namespace hw164
{
// A device with a cable from output channels [2, 3] to input channels [0, 1]: what leaves in the output stream at sample
// t comes back in the input stream at t + roundTrip (the callback hands out input it already has: roundTrip >= block).
struct LoopDevice
{
    int roundTrip = 0;
    std::vector<float> outA, outB; // the device's output stream (channels 2, 3)
    lynx::HardwareOutput bus;
    lynx::LiveInput input;
    std::vector<float> inA, inB; // this callback's input (channels 0, 1)

    float returned (const std::vector<float>& out, std::int64_t t) const
    {
        const auto from = t - roundTrip;
        return from >= 0 && from < (std::int64_t) out.size() ? out[(std::size_t) from] : 0.0f;
    }

    // one callback of `n` samples processed in chunks of `chunk` by `fx` (as the renderer does): returns its output
    void callback (lynx::Effect& fx, const lynx::EffectParams& p, float* l, float* r, int n, int chunk)
    {
        const auto at = (std::int64_t) outA.size();
        inA.assign ((std::size_t) n, 0.0f);
        inB.assign ((std::size_t) n, 0.0f);
        for (int i = 0; i < n; ++i)
        {
            inA[(std::size_t) i] = returned (outA, at + i);
            inB[(std::size_t) i] = returned (outB, at + i);
        }
        input.numChannels = 2;
        input.channels[0] = inA.data();
        input.channels[1] = inB.data();
        bus.active.store (true);
        for (int done = 0; done < n; done += chunk)
        {
            const auto m = std::min (chunk, n - done);
            fx.setHardwareIo (&input, &bus, done);
            fx.process (l + done, r + done, m, p, 120.0);
        }
        // drain: device channels 0..3, all open
        std::vector<float> o0 ((std::size_t) n), o1 ((std::size_t) n), o2 ((std::size_t) n), o3 ((std::size_t) n);
        float* outs[] { o0.data(), o1.data(), o2.data(), o3.data() };
        bus.drainInto (outs, 4, [] (int c) { return c < 4 ? c : -1; }, n);
        outA.insert (outA.end(), o2.begin(), o2.end());
        outB.insert (outB.end(), o3.begin(), o3.end());
    }
};

lynx::EffectParams routed (int roundTrip)
{
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::hardwareInsert);
    auto p = slot.params;
    p[lynx::hwinsert::output] = lynx::hwinsert::channelValue (2);
    p[lynx::hwinsert::input] = lynx::hwinsert::channelValue (0);
    p[lynx::hwinsert::inputStereo] = 1.0f;
    p[lynx::hwinsert::roundTrip] = lynx::hwinsert::roundTripValue (roundTrip);
    return p;
}
} // namespace hw164

TEST (hw164_params_and_defaults)
{
    using namespace lynx;
    const auto slot = makeEffectSlot (EffectKind::hardwareInsert);
    CHECK ((int) effectInfo (EffectKind::hardwareInsert).params.size() == hwinsert::count);
    CHECK (hwinsert::channelOf (slot.params[hwinsert::output]) == -1 && hwinsert::channelOf (slot.params[hwinsert::input]) == -1); // nothing routed at first
    CHECK (std::abs (hwinsert::dbOf (slot.params[hwinsert::send])) < 1e-6f && std::abs (hwinsert::dbOf (slot.params[hwinsert::returnGain])) < 1e-6f);
    for (int c = -1; c <= hwinsert::maxChannel; ++c)
        CHECK (hwinsert::channelOf (hwinsert::channelValue (c)) == c);
    for (int s : { 0, 1, 64, 371, 4096, 65535 })
        CHECK (hwinsert::roundTripOf (hwinsert::roundTripValue (s)) == s);
    CHECK (formatEffectParam (EffectKind::hardwareInsert, hwinsert::output, hwinsert::channelValue (2), 120.0) == "Out 3");
    CHECK (formatEffectParam (EffectKind::hardwareInsert, hwinsert::send, hwinsert::dbValue (-6.0f), 120.0) == "-6.0 dB");
    CHECK (formatEffectParam (EffectKind::hardwareInsert, hwinsert::roundTrip, hwinsert::roundTripValue (512), 120.0) == "512 smp");
    // its round trip is the slot's latency before the first block (PDC is planned from it)
    auto s2 = slot;
    s2.params[hwinsert::roundTrip] = hwinsert::roundTripValue (777);
    CHECK (makeEffect (s2)->latencySamples() == 777);
    // the Patcher's list leaves it out, the mixer's offers it; the song counts the active ones
    Song song = Song::makeDefault();
    CHECK (hwinsert::countActive (song) == 0 && ! hwinsert::returnsFromInput (song));
    song.mixer[1].effects.push_back (s2);
    CHECK (hwinsert::countActive (song) == 1 && ! hwinsert::returnsFromInput (song));
    song.mixer[1].effects[0].params[hwinsert::input] = hwinsert::channelValue (0);
    CHECK (hwinsert::returnsFromInput (song));
    song.mixer[1].effects[0].enabled = false;
    CHECK (hwinsert::countActive (song) == 0);
}

TEST (hw164_return_lines_up_with_the_dry_path)
{
    using namespace lynx;
    // through the cable at its exact round trip, the wet signal is the dry signal delayed by the reported latency,
    // bit for bit, for any block / chunk size; 50 % mix = the same (both paths line up); phase inverted at 50 % = silence
    std::mt19937 rng (164);
    std::uniform_real_distribution<float> u (-0.5f, 0.5f);
    for (const auto [roundTrip, block, chunk] : { std::tuple { 512, 512, 512 }, std::tuple { 371, 128, 64 }, std::tuple { 1000, 480, 128 }, std::tuple { 64, 64, 17 } })
    {
        for (const auto mode : { 0, 1, 2 }) // 100 % wet, 50 %, 50 % inverted
        {
            hw164::LoopDevice dev;
            dev.roundTrip = roundTrip;
            auto p = hw164::routed (roundTrip);
            p[hwinsert::dryWet] = mode == 0 ? 1.0f : 0.5f;
            p[hwinsert::invert] = mode == 2 ? 1.0f : 0.0f;
            auto fx = makeEffect (makeEffectSlot (EffectKind::hardwareInsert));
            fx->prepare (48000.0);
            std::vector<float> inL, inR, outL, outR;
            for (int b = 0; b < 40; ++b)
            {
                std::vector<float> l ((std::size_t) block), r ((std::size_t) block);
                for (int i = 0; i < block; ++i)
                    l[(std::size_t) i] = u (rng), r[(std::size_t) i] = u (rng);
                inL.insert (inL.end(), l.begin(), l.end());
                inR.insert (inR.end(), r.begin(), r.end());
                dev.callback (*fx, p, l.data(), r.data(), block, chunk);
                outL.insert (outL.end(), l.begin(), l.end());
                outR.insert (outR.end(), r.begin(), r.end());
            }
            CHECK (fx->latencySamples() == roundTrip);
            double worst = 0.0;
            for (std::size_t t = (std::size_t) roundTrip; t < outL.size(); ++t)
            {
                const auto wantL = mode == 2 ? 0.0f : inL[t - (std::size_t) roundTrip];
                const auto wantR = mode == 2 ? 0.0f : inR[t - (std::size_t) roundTrip];
                worst = std::max ({ worst, (double) std::abs (outL[t] - wantL), (double) std::abs (outR[t] - wantR) });
            }
            if (worst > 1e-6)
                std::printf ("  round trip %d block %d chunk %d mode %d: worst %g\n", roundTrip, block, chunk, mode, worst);
            CHECK (worst <= 1e-6);
        }
    }
}

TEST (hw164_send_return_gains_and_mono)
{
    using namespace lynx;
    hw164::LoopDevice dev;
    dev.roundTrip = 256;
    auto p = hw164::routed (256);
    p[hwinsert::send] = hwinsert::dbValue (-6.0f);
    p[hwinsert::returnGain] = hwinsert::dbValue (6.0f);
    p[hwinsert::outputMono] = 1.0f;   // one output: (L+R)/2
    p[hwinsert::inputStereo] = 0.0f;  // one input, to both sides
    auto fx = makeEffect (makeEffectSlot (EffectKind::hardwareInsert));
    fx->prepare (48000.0);
    std::vector<float> outL, outR;
    for (int b = 0; b < 8; ++b)
    {
        std::vector<float> l (256, 0.4f), r (256, 0.2f);
        dev.callback (*fx, p, l.data(), r.data(), 256, 256);
        outL.insert (outL.end(), l.begin(), l.end());
        outR.insert (outR.end(), r.begin(), r.end());
    }
    // sent: 0.3 × 0.501 on Out 3 only; back on In 1 to both sides × 1.995 ≈ 0.3
    const auto expected = 0.3f * hwinsert::gainOf (hwinsert::dbValue (-6.0f)) * hwinsert::gainOf (hwinsert::dbValue (6.0f));
    CHECK (std::abs (outL[1000] - expected) < 1e-5f && std::abs (outR[1000] - expected) < 1e-5f);
    CHECK (std::abs (expected - 0.3f) < 0.001f);
    CHECK (dev.outB[1000] == 0.0f); // (nothing on the second output)
    CHECK (std::abs (fx->controlOutput (hwinsert::returnPeak) - expected) < 1e-5f);
}

TEST (hw164_offline_passes_the_dry_signal_at_its_latency)
{
    using namespace lynx;
    // an export (no device output buses): the dry signal, delayed by the round trip, so every other path stays aligned
    auto p = hw164::routed (300);
    auto fx = makeEffect (makeEffectSlot (EffectKind::hardwareInsert));
    fx->prepare (48000.0);
    std::vector<float> in, out;
    for (int b = 0; b < 10; ++b)
    {
        std::vector<float> l (128), r (128);
        for (int i = 0; i < 128; ++i)
            l[(std::size_t) i] = r[(std::size_t) i] = (float) std::sin (0.01 * (b * 128 + i));
        in.insert (in.end(), l.begin(), l.end());
        fx->setHardwareIo (nullptr, nullptr, 0);
        fx->process (l.data(), r.data(), 128, p, 120.0);
        out.insert (out.end(), l.begin(), l.end());
    }
    for (std::size_t t = 300; t < out.size(); ++t)
        CHECK (out[t] == in[t - 300]);
    CHECK (std::abs (fx->controlOutput (hwinsert::status) * 16.0f - 1.0f) < 1e-6f); // "offline"
}

TEST (hw164_ping_measures_its_own_path)
{
    using namespace lynx;
    // the Ping button (a momentary value) measures exactly the send -> return path, while the track keeps playing
    for (const auto [roundTrip, block] : { std::pair { 371, 128 }, std::pair { 1536, 512 }, std::pair { 64, 64 } })
    {
        hw164::LoopDevice dev;
        dev.roundTrip = roundTrip;
        auto p = hw164::routed (0);
        auto fx = makeEffect (makeEffectSlot (EffectKind::hardwareInsert));
        fx->prepare (48000.0);
        std::mt19937 rng (7);
        std::uniform_real_distribution<float> u (-0.05f, 0.05f);
        auto run = [&] (int blocks) {
            for (int b = 0; b < blocks; ++b)
            {
                std::vector<float> l ((std::size_t) block), r ((std::size_t) block);
                for (int i = 0; i < block; ++i)
                    l[(std::size_t) i] = r[(std::size_t) i] = u (rng); // music under the ping (held back from the send meanwhile)
                dev.callback (*fx, p, l.data(), r.data(), block, block);
            }
        };
        run (4);
        CHECK (fx->controlOutput (hwinsert::pingState) == 0.0f);
        p[hwinsert::ping] = 1.0f; // pressed
        run (2);
        CHECK (std::abs (fx->controlOutput (hwinsert::pingState) - 0.25f) < 1e-6f);
        run ((int) (9 * 12000 / block) + 4); // 8 clicks a quarter second apart at 48 kHz
        CHECK (std::abs (fx->controlOutput (hwinsert::pingState) - 0.5f) < 1e-6f);
        CHECK (hwinsert::roundTripOf (fx->controlOutput (hwinsert::pingSamples)) == roundTrip);
        CHECK (std::abs (fx->controlOutput (hwinsert::pingSerial) * 1024.0f - 1.0f) < 1e-6f);
    }
    // nothing wired back: the ping fails with "no signal"
    {
        hw164::LoopDevice dev;
        dev.roundTrip = 100000; // (never returns within the ping)
        auto p = hw164::routed (0);
        auto fx = makeEffect (makeEffectSlot (EffectKind::hardwareInsert));
        fx->prepare (48000.0);
        std::vector<float> l (256), r (256);
        dev.callback (*fx, p, l.data(), r.data(), 256, 256);
        p[hwinsert::ping] = 1.0f;
        for (int b = 0; b < 450; ++b)
        {
            std::fill (l.begin(), l.end(), 0.0f);
            std::fill (r.begin(), r.end(), 0.0f);
            dev.callback (*fx, p, l.data(), r.data(), 256, 256);
        }
        CHECK (std::abs (fx->controlOutput (hwinsert::pingState) - 0.625f) < 1e-6f);
    }
}
