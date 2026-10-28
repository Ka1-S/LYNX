// Round 165g-flbeta165h: Harmor against FL's DLL (docs/analise/REA_rodada165g-flbeta165h.md)

namespace
{
std::uint32_t harmor165hBits (float x)
{
    std::uint32_t u;
    std::memcpy (&u, &x, 4);
    return u;
}

float harmor165hFloat (std::uint32_t u)
{
    float x;
    std::memcpy (&x, &u, 4);
    return x;
}

// mono samples of one held note (block 512, 44.1 kHz, 120 BPM)
std::vector<float> harmor165hRender (const std::vector<std::uint8_t>& st, int length)
{
    const auto y = lynx::harmor::renderReference (st, { { 0, 60, 0.8, 1.0, 0.0, -1 } }, 44100.0, length);
    std::vector<float> m ((std::size_t) length);
    for (int k = 0; k < length && 2 * (std::size_t) k < y.size(); ++k)
        m[(std::size_t) k] = y[2 * (std::size_t) k];
    return m;
}

double harmor165hRms (const std::vector<float>& y, int a, int b)
{
    double s = 0.0;
    for (int k = a; k < b; ++k)
        s += (double) y[(std::size_t) k] * y[(std::size_t) k];
    return std::sqrt (s / std::max (1, b - a));
}
} // namespace

TEST (harmor165h_ipp_a11_models_match_the_dll)
{
    // §3: outputs of FL's ippsExp_32f_A11 / ippsLn_32f_A11 (AVX2 path) for inputs where they differ from the correctly
    // rounded result; the models reproduce them bit for bit
    const std::pair<std::uint32_t, std::uint32_t> ex[] = { { 0x3d07d961, 0x3f845146 }, { 0x3c23f1c2, 0x3f814996 }, { 0xbf237869, 0x3f072f20 },
        { 0x3e14dcea, 0x3f9409a6 }, { 0x3e11f757, 0x3f939e8b }, { 0xbdb80473, 0x3f6a0165 }, { 0x3b9bebe2, 0x3f809c50 } };
    for (const auto& [x, y] : ex)
    {
        CHECK (harmor165hBits (lynx::harmor::ippExpA11Model (harmor165hFloat (x))) == y);
        CHECK (harmor165hBits ((float) std::exp ((double) harmor165hFloat (x))) != y); // (not plain exp)
    }
    const std::pair<std::uint32_t, std::uint32_t> ln[] = { { 0x43e22d0b, 0x40c3a9a4 }, { 0x4465fea3, 0x40da6111 }, { 0x450882e0, 0x40f60c40 },
        { 0x456a93cf, 0x4103af9c }, { 0x4468c2f5, 0x40dac309 }, { 0x426b995d, 0x40826d41 }, { 0x7aa1bdca, 0x42a40cf6 } };
    for (const auto& [x, y] : ln)
        CHECK (harmor165hBits (lynx::harmor::ippLnA11Model (harmor165hFloat (x))) == y);
    CHECK (lynx::harmor::ippExpA11Model (0.0f) == 1.0f);
    CHECK (std::isinf (lynx::harmor::ippLnA11Model (0.0f)) && lynx::harmor::ippLnA11Model (0.0f) < 0.0f);
}

TEST (harmor165h_precision_average_changes_the_sound_slightly)
{
    // global misc +28 = 0 (precision "Average") swaps in the 11-bit IPP functions and the nearest-entry table reads:
    // close to, but not exactly, the "High" sound
    auto st = harmorTestState (false);
    const auto prec = lynx::harmorParamOffset (st, -1, 0) + 53 * 4 + 24 + 28;
    CHECK (prec > 0 && prec + 4 <= (int) st.size());
    std::int32_t one = 1, zero = 0;
    std::memcpy (st.data() + prec, &one, 4);
    const auto hi = harmor165hRender (st, 8192);
    std::memcpy (st.data() + prec, &zero, 4);
    const auto avg = harmor165hRender (st, 8192);
    double d = 0.0;
    for (std::size_t k = 0; k < hi.size(); ++k)
        d = std::max (d, (double) std::abs (hi[k] - avg[k]));
    CHECK (d > 0.0);
    CHECK (d < 0.01 * harmor165hRms (hi, 0, 8192) * 4.0);
}

TEST (harmor165h_note_start_delay)
{
    // §1: part parameter 2 delays every note: p ≥ 0 → p·0.3125/tempo s, p < 0 → (2^(−p/48) − 1)/255 s, then the
    // note plays as usual (FL: Harmony, Rotochip)
    auto st = harmorTestState (false);
    const auto at = lynx::harmorParamOffset (st, 0, 2);
    CHECK (at > 0);
    const auto plain = harmor165hRender (st, 16384);
    CHECK (harmor165hRms (plain, 0, 2048) > 1e-4);
    auto withDelay = [&] (std::int32_t p) {
        auto s = st;
        std::memcpy (s.data() + at, &p, 4);
        return harmor165hRender (s, 16384);
    };
    for (const auto& [p, start] : { std::pair<int, int> { 96, 11025 }, { -200, 2933 } }) // 0.25 s; (2^(200/48) − 1)/255 s
    {
        const auto y = withDelay (p);
        bool silent = true;
        for (int k = 0; k < start; ++k)
            silent = silent && y[(std::size_t) k] == 0.0f;
        CHECK (silent);
        CHECK (harmor165hRms (y, start, start + 2048) > 1e-4);
        // the delayed note is the plain one, later
        CHECK (std::abs (harmor165hRms (y, start, start + 4096) - harmor165hRms (plain, 0, 4096)) < 0.05 * harmor165hRms (plain, 0, 4096));
    }
}

TEST (harmor165h_attack_fade)
{
    // §2: part misc +4 = v fades every record in over (v/1000)²·25 ms (a raised-cosine table), and the partials then
    // start at their levels instead of gliding up from silence (FL: Morning)
    auto st = harmorTestState (false);
    const auto misc = lynx::harmorParamOffset (st, 0, 0) + 386 * 4 + 0x30;
    std::int32_t v = 0;
    std::memcpy (&v, st.data() + misc + 4, 4);
    CHECK (v == 0);
    const auto plain = harmor165hRender (st, 8192);
    v = 1000; // 25 ms = 1102 samples
    std::memcpy (st.data() + misc + 4, &v, 4);
    const auto faded = harmor165hRender (st, 8192);
    CHECK (harmor165hRms (faded, 0, 200) < 0.2 * harmor165hRms (plain, 0, 200));
    CHECK (harmor165hRms (faded, 500, 700) > harmor165hRms (faded, 0, 200) * 4.0);
    const auto a = harmor165hRms (faded, 3000, 5000), b = harmor165hRms (plain, 3000, 5000);
    CHECK (b > 0.0 && std::abs (20.0 * std::log10 (a / b)) < 0.5);
}

TEST (harmor165h_velocity_speeds_up_the_volume_envelope)
{
    // §5: part parameter 15 scales the volume envelope's time by 2 − 2·velocity at the note's start (FL: Old vox)
    auto st = harmorTestState (false);
    const auto at = lynx::harmorParamOffset (st, 0, 15);
    CHECK (at > 0);
    auto render = [&] (std::int32_t p15, double vel) {
        auto s = st;
        std::memcpy (s.data() + at, &p15, 4);
        const auto y = lynx::harmor::renderReference (s, { { 0, 60, vel, 1.0, 0.0, -1 } }, 44100.0, 8192);
        std::vector<float> m (8192);
        for (std::size_t k = 0; k < m.size(); ++k)
            m[k] = y[2 * k];
        return m;
    };
    CHECK (render (1, 0.5) == render (0, 0.5)); // velocity 0.5: scale 1
    const auto fast = render (1, 0.8), plain = render (0, 0.8);
    CHECK (fast != plain);
    CHECK (harmor165hRms (fast, 0, 200) > 1.3 * harmor165hRms (plain, 0, 200)); // the attack is 2.5× shorter
}
