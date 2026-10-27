// --- Round 165a: Ember Saturator ← Decapitator (docs/PARITY.md, REA_rodada165ha) -----------------------------------
// Punish's measured law per style, the Auto link (Output follows Drive in the editor), the Classic law upgrade of older
// projects, and the one-screen Classic layout.

namespace p165a
{
// small-signal gain (dB) of Ember's Classic voicing at 1 kHz, −70 dBFS
inline double classicGainDb (int style, float drive, bool punish, bool measuredLaw = true)
{
    namespace e = lynx::mixsuite::ember;
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::ember);
    slot.params[(size_t) e::voicing] = 1.0f;
    slot.params[(size_t) e::classicStyle] = (float) style / 4.0f;
    slot.params[(size_t) e::classicDrive] = drive / 10.0f;
    slot.params[(size_t) e::classicPunish] = punish ? 1.0f : 0.0f;
    slot.params[(size_t) e::classicLaw] = measuredLaw ? 1.0f : 0.0f;
    auto fx = lynx::makeEffect (slot);
    fx->prepare (48000.0);
    fx->reset();
    const int n = 24000;
    std::vector<float> l ((size_t) n), r ((size_t) n);
    for (int i = 0; i < n; ++i)
        l[(size_t) i] = r[(size_t) i] = (float) (std::pow (10.0, -70.0 / 20.0) * std::sin (2.0 * 3.141592653589793 * 1000.0 * i / 48000.0));
    for (int at = 0; at < n; at += 512)
        fx->process (l.data() + at, r.data() + at, std::min (512, n - at), slot.params, 120.0);
    double s = 0.0;
    for (int i = n / 2; i < n; ++i) s += (double) l[(size_t) i] * l[(size_t) i];
    return 20.0 * std::log10 (std::sqrt (s / (n / 2)) * std::sqrt (2.0)) + 70.0;
}
} // namespace p165a

TEST (ember_classic_punish_follows_the_measured_law_round165)
{
    // [EXEC trial, REA_rodada165ha §1] the gain Punish adds (dB) at drive 1 / 5 / 10, per style A E N T P
    const double trial[5][3] { { 14.24, 15.84, 17.92 }, { 14.50, 16.40, 18.72 }, { 14.25, 14.64, 15.19 },
                               { 14.52, 16.62, 19.11 }, { 14.64, 17.64, 21.32 } };
    const float drives[3] { 1.0f, 5.0f, 10.0f };
    for (int st = 0; st < 5; ++st)
        for (int k = 0; k < 3; ++k)
        {
            const auto added = p165a::classicGainDb (st, drives[k], true) - p165a::classicGainDb (st, drives[k], false);
            CHECK (std::abs (added - trial[st][k]) < 0.3);
        }
    // a project saved before round 165 keeps round 136's +9.75 dB
    const auto old = p165a::classicGainDb (0, 5.0f, true, false) - p165a::classicGainDb (0, 5.0f, false, false);
    CHECK (std::abs (old - 9.75) < 0.1);
}

TEST (ember_classic_law_upgrade_and_count_round165)
{
    using namespace lynx;
    namespace em = mixsuite::ember;
    CHECK ((int) em::count <= maxEffectParams);
    auto old = makeEffectSlot (EffectKind::ember);
    CHECK (old.params[(size_t) em::classicLaw] == 1.0f);  // new instances: the measured law
    CHECK (old.params[(size_t) em::classicAuto] == 1.0f); // Auto on, the reference's default
    upgradeLoadedEffectParams (old, (std::size_t) em::classicAuto);
    CHECK (old.params[(size_t) em::classicLaw] == 0.0f);
    auto fresh = makeEffectSlot (EffectKind::ember);
    upgradeLoadedEffectParams (fresh, (std::size_t) em::count);
    CHECK (fresh.params[(size_t) em::classicLaw] == 1.0f);
}

TEST (ember_classic_auto_moves_output_with_drive_round165)
{
    // [EXEC her 63 saved states] the reference's Auto: Output = −1.6 dB per Drive unit (drive 2.0 → −3.2 dB, 5.9 → −9.4)
    using namespace lynx;
    namespace em = mixsuite::ember;
    auto slot = makeEffectSlot (EffectKind::ember);
    slot.params[(size_t) em::voicing] = 1.0f;
    slot.params[(size_t) em::classicDrive] = 0.0f;
    slot.params[(size_t) em::classicOutput] = 1.0f;
    auto e = em::linkedEdit (slot.params, em::classicDrive, 0.59f);
    CHECK (e.size() == 2);
    CHECK (e[1].first == em::classicOutput);
    CHECK (std::abs (emberclassic::outputDb (e[1].second) - (-9.44)) < 1e-3);
    // relative: a hand-set Output keeps its offset; the knob's range clamps
    slot.params[(size_t) em::classicDrive] = 0.2f;
    slot.params[(size_t) em::classicOutput] = (24.0f - 6.0f) / 24.0f; // −6 dB
    e = em::linkedEdit (slot.params, em::classicDrive, 0.1f);
    CHECK (std::abs (emberclassic::outputDb (e[1].second) - (-4.4)) < 1e-3);
    e = em::linkedEdit (slot.params, em::classicDrive, 1.0f);
    CHECK (std::abs (emberclassic::outputDb (e[1].second) - (-18.8)) < 1e-3);
    // Auto off, or another parameter: only the edit itself
    slot.params[(size_t) em::classicAuto] = 0.0f;
    CHECK (em::linkedEdit (slot.params, em::classicDrive, 0.5f).size() == 1);
    slot.params[(size_t) em::classicAuto] = 1.0f;
    CHECK (em::linkedEdit (slot.params, em::classicTone, 0.5f).size() == 1);
}

TEST (ember_classic_layout_is_the_one_screen_panel_round165)
{
    using namespace lynx;
    const auto& l = plugui::layoutFor (EffectKind::ember);
    bool vu = false, autoToggle = false, styleRow = false;
    for (const auto& it : l.items)
    {
        const bool classic = it.whenParam != nullptr && std::string (it.whenParam) == "Voicing" && it.whenMin >= 0.5f;
        if (classic && it.type == plugui::Item::display && std::string (it.display) == "vumeter") vu = true;
        if (classic && it.param != nullptr && std::string (it.param) == "Classic auto") autoToggle = true;
        if (classic && it.param != nullptr && std::string (it.param) == "Classic style" && it.type == plugui::Item::tabs
            && it.w > 4 * it.h)
            styleRow = true; // the five styles side by side along the bottom
        if (classic) // inside the editor
            CHECK (it.x >= 0 && it.y >= 0 && it.x + it.w <= l.width + 1 && it.y + it.h <= l.height + 1);
    }
    CHECK (vu);
    CHECK (autoToggle);
    CHECK (styleRow);
}
