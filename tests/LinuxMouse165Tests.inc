// Round 165 (linuxmouse165): Fruity WaveShaper's curve edited in LYNX (FL's point editor) — the state written back,
// point edits, and the running effect taking a new curve without being rebuilt.

#include "core/FruityShapers.h"

TEST (linuxmouse165_waveshaper_state_round_trip)
{
    namespace fs = lynx::flshape;
    const auto stA = waveShaperTestState (150, 100, 120, false, true, 1, true, true);
    const auto a = fs::parseWaveShaper (stA);
    CHECK (a.has_value());
    if (! a)
        return;
    const auto bytes = fs::writeWaveShaper (*a, stA);
    const auto b = fs::parseWaveShaper (bytes);
    CHECK (b && b->pre == 150 && b->mix == 100 && b->post == 120 && b->center && ! b->unipolar && b->oversampling == 1 && b->hq && b->smooth);
    CHECK (b && b->points.size() == a->points.size());
    if (b)
    {
        for (std::size_t i = 0; i < a->points.size() && i < b->points.size(); ++i)
            CHECK (b->points[i].y == a->points[i].y && b->points[i].tension == a->points[i].tension && b->points[i].mode == a->points[i].mode
                   && std::abs (b->points[i].dt - a->points[i].dt) < 1e-6f);
        CHECK (fs::shaperTable (b->points, false) == fs::shaperTable (a->points, false));
        CHECK (fs::shaperTable (b->points, true) == fs::shaperTable (a->points, true));
    }
    // the map's 20-byte header and the editor flag come from the old state (same layout, same size)
    CHECK (bytes.size() == stA.size());
    if (bytes.size() == stA.size())
        CHECK (std::equal (stA.end() - 26, stA.end(), bytes.end() - 26));

    // an empty slot: FL's default instance with the slot's controls, default curve (0,0)→(1,1)
    float params[8] = { 0.5f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };
    const auto fresh = fs::waveShaperForEditing ({}, params);
    CHECK (fresh.pre == 128 && fresh.mix == 128 && fresh.unipolar && fresh.points.size() == 2);
    const auto fb = fs::parseWaveShaper (fs::writeWaveShaper (fresh));
    CHECK (fb && fb->unipolar && fb->points.size() == 2 && fb->points[1].dt == 1.0f && fb->points[1].y == 1.0f && fb->points[0].y == 0.0f);
}

TEST (linuxmouse165_waveshaper_point_edits)
{
    namespace fs = lynx::flshape;
    auto pts = fs::waveShaperForEditing ({}).points; // (0,0) (1,1)
    // add between the ends, in time order
    CHECK (fs::addWaveShaperPoint (pts, 0.5f, 0.2f) == 1);
    CHECK (fs::addWaveShaperPoint (pts, 0.25f, 0.8f) == 1);
    auto t = fs::waveShaperTimes (pts);
    CHECK (pts.size() == 4 && t[1] == 0.25f && t[2] == 0.5f && t[3] == 1.0f && pts[1].y == 0.8f && pts[2].y == 0.2f);
    // the ends keep their time; inner points stay between their neighbours; levels clamp to 0…1
    CHECK (fs::moveWaveShaperPoint (pts, 0, 0.4f, 0.3f) == 0);
    CHECK (fs::moveWaveShaperPoint (pts, 3, 0.1f, 1.5f) == 3);
    CHECK (fs::moveWaveShaperPoint (pts, 1, 0.9f, -1.0f) == 1);
    t = fs::waveShaperTimes (pts);
    CHECK (t[0] == 0.0f && pts[0].y == 0.3f && t[3] == 1.0f && pts[3].y == 1.0f && t[1] == 0.5f && pts[1].y == 0.0f);
    // the ends can't be removed; an inner point can, and the next one keeps its time
    CHECK (! fs::removeWaveShaperPoint (pts, 0) && ! fs::removeWaveShaperPoint (pts, 3));
    CHECK (fs::removeWaveShaperPoint (pts, 1));
    t = fs::waveShaperTimes (pts);
    CHECK (pts.size() == 3 && t[1] == 0.5f && t[2] == 1.0f && pts[1].y == 0.2f);
    // the drawn level follows the table: the default curve is the identity in both polarities
    const auto ident = fs::waveShaperForEditing ({}).points;
    for (const bool uni : { false, true })
    {
        const auto table = fs::shaperTable (ident, uni);
        CHECK (std::abs (fs::waveShaperLevelAt (table, uni, 0.25f) - 0.25f) < 1e-3f && std::abs (fs::waveShaperLevelAt (table, uni, 1.0f) - 1.0f) < 1e-3f);
    }
    CHECK (fs::waveShaperModes().size() == 13 && fs::waveShaperModes().front().mode == 0);
}

TEST (linuxmouse165_waveshaper_takes_a_new_curve_while_running)
{
    namespace fs = lynx::flshape;
    const auto st = waveShaperTestState (100, 128, 128, false, false, 0, false, false); // stateless kernel: exact compare
    auto edited = fs::waveShaperForEditing (st);
    fs::moveWaveShaperPoint (edited.points, 2, 0.6f, 0.1f);
    const auto st2 = fs::writeWaveShaper (edited, st);

    auto slot = lynx::effectFromFl ("Fruity WaveShaper", st, 120.0);
    CHECK (slot.has_value());
    if (! slot)
        return;
    auto running = lynx::makeEffect (*slot);
    running->prepare (44100.0);
    std::vector<float> l (512), r (512);
    auto fill = [&] { for (int n = 0; n < 512; ++n) l[(std::size_t) n] = r[(std::size_t) n] = (float) std::sin (n * 0.05) * 0.9f; };
    fill();
    running->process (l.data(), r.data(), 512, slot->params, 120.0);
    CHECK (running->acceptState (st2));
    fill();
    running->process (l.data(), r.data(), 512, slot->params, 120.0);

    auto slot2 = *slot;
    slot2.state = st2;
    auto rebuilt = lynx::makeEffect (slot2);
    rebuilt->prepare (44100.0);
    std::vector<float> l2 (512), r2 (512);
    for (int n = 0; n < 512; ++n)
        l2[(std::size_t) n] = r2[(std::size_t) n] = (float) std::sin (n * 0.05) * 0.9f;
    rebuilt->process (l2.data(), r2.data(), 512, slot2.params, 120.0);
    CHECK (l == l2 && r == r2);
    CHECK (! running->acceptState ({ 1, 2, 3 })); // not a WaveShaper state: the slot is built again
}
