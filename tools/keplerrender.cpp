// Renders a Kepler state with LYNX's engine the way the REA round-46 EXEC harness drives FL's engine DLL (verification,
// REA_rodada46 §6): keplerrender <state.bin> <engine_x64.dll> <out.f32> <length> <rate> [tempo=X] [blk=N]
// [warm=N] [midivol=V] [midipan=P] [transport]   (FL's export: warm-up, channel levels as MIDI, song playing)
// [note=start,key,initVol,finalPan,pitchCents,off ...]   (off = −1: held). Output: interleaved stereo float32.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "core/Kepler.h"

int main (int argc, char** argv)
{
    if (argc < 6)
    {
        std::fprintf (stderr, "usage: keplerrender state.bin engine_x64.dll out.f32 length rate [tempo=X] [blk=N] "
                              "[note=start,key,initVol,finalPan,pitchCents,off ...]\n");
        return 2;
    }
    std::ifstream in (argv[1], std::ios::binary);
    const std::vector<std::uint8_t> state ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    const int length = std::atoi (argv[4]);
    const double rate = std::atof (argv[5]);
    lynx::kepler::RefOptions opt;
    std::vector<lynx::kepler::RefNote> notes;
    for (int i = 6; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto val = [&] { return a.substr (a.find ('=') + 1); };
        if (a.rfind ("tempo=", 0) == 0)
            opt.tempo = std::atof (val().c_str());
        else if (a.rfind ("blk=", 0) == 0)
            opt.block = std::atoi (val().c_str());
        else if (a.rfind ("warm=", 0) == 0)
            opt.warmup = std::atoi (val().c_str());
        else if (a.rfind ("midivol=", 0) == 0)
            opt.midiVolume = std::atoi (val().c_str());
        else if (a.rfind ("midipan=", 0) == 0)
            opt.midiPan = std::atoi (val().c_str());
        else if (a == "transport")
            opt.transport = true;
        else if (a.rfind ("note=", 0) == 0)
        {
            std::stringstream ss (val());
            std::string t;
            std::vector<double> v;
            while (std::getline (ss, t, ','))
                v.push_back (std::atof (t.c_str()));
            if (v.size() < 6)
                continue;
            lynx::kepler::RefNote n;
            n.start = (int) v[0];
            n.key = (int) v[1];
            n.initVol = v[2];
            n.finalPan = v[3];
            n.pitchCents = v[4];
            n.release = (int) v[5];
            notes.push_back (n);
        }
    }
    if (! lynx::keplerStateUsable (state))
    {
        std::fprintf (stderr, "state not usable\n");
        return 1;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const auto out = lynx::kepler::renderReference (state, argv[2], notes, rate, length, opt);
    const auto ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
    if (out.empty())
    {
        std::fprintf (stderr, "render failed (engine DLL not usable?)\n");
        return 1;
    }
    std::ofstream o (argv[3], std::ios::binary);
    o.write (reinterpret_cast<const char*> (out.data()), (std::streamsize) (out.size() * sizeof (float)));
    std::printf ("rendered %d samples in %.1f ms\n", length, ms);
    return 0;
}
