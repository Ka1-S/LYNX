// Renders a Fruity Granulizer state with LYNX's engine exactly like the round-67 EXEC harness drives the DLL
// (verification harness, REA_rodada67 §4): granulizerrender <state.bin> <sample.raw> <channels> <format 0=int16 1=float>
//   <sample rate> <out.f32> <length> <rate> [blk=N] [seed=N] [tempo=BPM] [beats=N] [slices=auto|<file of starts>]
//   [note=start,key,finalVol,finalPan,pitchCents,release ...]
// The state is normalised first; the raw sample is interleaved frames. Writes interleaved stereo float32.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/FlAutoSlicer.h"
#include "core/Granulizer.h"

int main (int argc, char** argv)
{
    if (argc < 9)
    {
        std::fprintf (stderr, "usage: granulizerrender state.bin sample.raw ch fmt srate out.f32 length rate [blk=N] [seed=N] [note=s,k,fv,fp,pc,off ...]\n");
        return 2;
    }
    std::ifstream in (argv[1], std::ios::binary);
    const std::vector<std::uint8_t> raw ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    const auto state = lynx::granulizerNormalisedState (raw, {});
    std::ifstream sin (argv[2], std::ios::binary);
    const std::vector<char> bytes ((std::istreambuf_iterator<char> (sin)), std::istreambuf_iterator<char>());
    const int ch = std::atoi (argv[3]), fmt = std::atoi (argv[4]);
    lynx::SampleData smp;
    smp.sampleRate = std::atof (argv[5]);
    const auto bpf = (std::size_t) ch * (fmt != 0 ? 4 : 2);
    const auto frames = bytes.size() / bpf;
    for (std::size_t i = 0; i < frames; ++i)
        for (int c = 0; c < ch; ++c)
        {
            float v;
            if (fmt != 0)
                std::memcpy (&v, bytes.data() + i * bpf + 4 * (std::size_t) c, 4);
            else
            {
                std::int16_t s;
                std::memcpy (&s, bytes.data() + i * bpf + 2 * (std::size_t) c, 2);
                v = (float) s * 3.0517578125e-05f;
            }
            (c == 0 ? smp.left : smp.right).push_back (v);
        }
    const int length = std::atoi (argv[7]);
    const double rate = std::atof (argv[8]);
    lynx::granulizer::RefOptions opt;
    std::vector<lynx::granulizer::RefNote> notes;
    for (int i = 9; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a.rfind ("blk=", 0) == 0)
            opt.block = std::atoi (a.c_str() + 4);
        else if (a.rfind ("seed=", 0) == 0)
            opt.seed = (std::uint32_t) std::strtoul (a.c_str() + 5, nullptr, 10);
        else if (a.rfind ("tempo=", 0) == 0)
            opt.tempo = std::atof (a.c_str() + 6);
        else if (a.rfind ("points=", 0) == 0)
            opt.points = std::atoi (a.c_str() + 7);
        else if (a.rfind ("beats=", 0) == 0)
            opt.beats = (float) std::atof (a.c_str() + 6);
        else if (a == "slices=auto")
            opt.slices = lynx::flslice::autoSlice (smp.left.data(), ch > 1 ? smp.right.data() : nullptr, (std::int64_t) frames, smp.sampleRate);
        else if (a.rfind ("slices=", 0) == 0)
        {
            if (std::FILE* sf = std::fopen (a.c_str() + 7, "r"))
            {
                long long v;
                while (std::fscanf (sf, "%lld", &v) == 1)
                    opt.slices.push_back (v);
                std::fclose (sf);
            }
        }
        else if (a.rfind ("note=", 0) == 0)
        {
            lynx::granulizer::RefNote n;
            if (std::sscanf (a.c_str() + 5, "%d,%d,%lf,%lf,%lf,%d", &n.start, &n.key, &n.finalVol, &n.finalPan, &n.pitchOffset, &n.release) == 6)
                notes.push_back (n);
        }
    }
    const auto out = lynx::granulizer::renderReference (state, smp, notes, rate, length, opt);
    if (out.empty())
    {
        std::fprintf (stderr, "state not usable\n");
        return 1;
    }
    std::FILE* f = std::fopen (argv[6], "wb");
    if (f == nullptr)
        return 1;
    std::fwrite (out.data(), sizeof (float), out.size(), f);
    std::fclose (f);
    return 0;
}
