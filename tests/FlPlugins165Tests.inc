// --- FL plugins LYNX lacked: Fruity Mute 2, Big Clock, Tuner, HTML NoteBook, inert imports (core/FlExtraFx.h) - round 165g

#include "core/FlExtraFx.h"

namespace fl165g
{
std::vector<std::uint8_t> ints (std::initializer_list<std::int32_t> v)
{
    std::vector<std::uint8_t> out;
    for (const auto x : v)
    {
        const auto* b = reinterpret_cast<const std::uint8_t*> (&x);
        out.insert (out.end(), b, b + 4);
    }
    return out;
}
} // namespace fl165g

TEST (fl165g_mute2_channels_and_switch)
{
    using namespace lynx::flextra::mute2;
    // FL: muted below 0.5; channel = round(v · 3) clamped to 2 (Delphi's banker's rounding)
    CHECK (soundOn (0.5f) && soundOn (1.0f) && ! soundOn (0.49f) && ! soundOn (0.0f));
    CHECK (channelOf (0.0f) == Channel::left && channelOf (1.0f / 3.0f) == Channel::both && channelOf (2.0f / 3.0f) == Channel::right);
    CHECK (channelOf (1.0f) == Channel::right && channelOf (0.5f) == Channel::right && channelOf (0.16f) == Channel::left);
    CHECK (paramText (mute, 1.0f) == "On" && paramText (mute, 0.0f) == "Off" && paramText (channel, 1.0f / 3.0f) == "L + R");

    for (const auto c : { Channel::left, Channel::both, Channel::right })
        for (const auto m : { 0.0f, 1.0f })
        {
            std::vector<float> l (64, 0.5f), r (64, -0.25f);
            process (l.data(), r.data(), 64, m, channelValue (c));
            const auto lOn = l[10] != 0.0f, rOn = r[10] != 0.0f;
            if (m >= 0.5f)
                CHECK (lOn && rOn && l[63] == 0.5f && r[0] == -0.25f);
            else
                CHECK (lOn == (c == Channel::right) && rOn == (c == Channel::left));
        }

    // FL's state: version 1, the switch, the selector
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::fruityMute2);
    CHECK (fromFl (fl165g::ints ({ 1, 1, 0 }), slot) && soundOn (slot.params[mute]) && channelOf (slot.params[channel]) == Channel::left);
    CHECK (fromFl (fl165g::ints ({ 1, 0, 2 }), slot) && ! soundOn (slot.params[mute]) && channelOf (slot.params[channel]) == Channel::right);
    CHECK (! fromFl (fl165g::ints ({ 1, 0 }), slot) && ! fromFl (fl165g::ints ({ 7, 0, 0 }), slot));

    // the import: name → effect, through effectFromFl
    const auto fx = lynx::effectFromFl ("Fruity Mute 2", fl165g::ints ({ 1, 0, 1 }), 120.0);
    CHECK (fx && fx->kind == lynx::EffectKind::fruityMute2 && channelOf (fx->params[channel]) == Channel::both);
    CHECK (lynx::effectForFlPlugin ("Fruity Mute 2") == lynx::EffectKind::fruityMute2);

    // the running effect
    auto effect = lynx::makeEffect (lynx::EffectKind::fruityMute2);
    effect->prepare (48000.0);
    lynx::EffectParams p {};
    p[mute] = 0.0f;
    p[channel] = channelValue (Channel::left);
    std::vector<float> l (32, 1.0f), r (32, 1.0f);
    effect->process (l.data(), r.data(), 32, p, 120.0);
    CHECK (l[5] == 0.0f && r[5] == 1.0f);
}

TEST (fl165g_big_clock_text)
{
    using namespace lynx::flextra::bigclock;
    lynx::Song song;
    song.mode = lynx::PlayMode::pattern;
    song.bpm = 120.0;
    const double ticks = 11.0 * 4 * lynx::ppq + 2.0 * lynx::ppq + 1.0 * lynx::ticksPerStep + 170.0; // bar 12, beat 3, step 10 (+170)
    CHECK (clockText (song, ticks, 4, Display::barBeatTick) == "12:03:41");  // 240 + 170 LYNX ticks = 41 FL ticks (96 ppq)
    CHECK (clockText (song, ticks, 4, Display::barStepTick) == "12:10:17");  // step 10 of the bar, 170 ticks = 17
    CHECK (clockText (song, 0.0, 4, Display::barBeatTick) == "1:01:00");
    CHECK (clockText (song, (double) lynx::ppq * 2 * 61.5, 4, Display::minSecHundredths) == "1:01:50"); // 123 beats at 120 BPM = 61.5 s
    CHECK (displayOf (displayValue (Display::minSecHundredths)) == Display::minSecHundredths);
    CHECK (paramText (display, 1.0f) == "Bar:Step:Tick");
    CHECK (lynx::effectFromFl ("Fruity Big Clock", {}, 120.0)->kind == lynx::EffectKind::bigClock);
}

TEST (fl165g_tuner_state_and_detection)
{
    using namespace lynx::flextra::tuner;
    // FL's presets: Default (440 Hz, fast, no bass, Dial = 2), Bass, Horizontal (Basic = 1), Average (reactivity 0)
    auto slot = lynx::makeEffectSlot (lynx::EffectKind::tuner);
    CHECK (fromFl (fl165g::ints ({ 1, 3000, 1, 0, 2 }), slot));
    CHECK (std::abs (referenceHz (slot.params[reference]) - 440.0) < 1e-9 && slot.params[reactivity] == 1.0f && ! bassOn (slot.params[bass])
           && visualOf (slot.params[visual]) == Visual::dial);
    CHECK (fromFl (fl165g::ints ({ 1, 3000, 0, 1, 1 }), slot) && slot.params[reactivity] == 0.5f && bassOn (slot.params[bass])
           && visualOf (slot.params[visual]) == Visual::basic);
    CHECK (paramText (reference, 0.5f) == "440.0 Hz" && paramText (reactivity, 0.5f) == "Average" && paramText (visual, 1.0f) == "Dial");
    CHECK (std::string (noteName (61)) == "C#" && octaveOf (60) == 4 && octaveOf (59) == 3);

    // detection: tones across the range, both reactivities, a different reference
    struct Case { double hz; float react; bool bass; double a4; int note; double cents; };
    const Case cases[] = { { 440.0, 1.0f, false, 440.0, 69, 0.0 },   { 442.3, 1.0f, false, 440.0, 69, 9.0 },
                           { 82.41, 0.0f, false, 440.0, 40, 0.0 },   { 55.0, 0.5f, false, 440.0, 33, 0.0 },
                           { 30.87, 1.0f, true, 440.0, 23, 0.0 },    { 2093.0, 0.0f, false, 440.0, 96, 0.0 },
                           { 4186.0, 1.0f, false, 440.0, 108, 0.0 }, { 432.0, 1.0f, false, 432.0, 69, 0.0 },
                           { 261.63, 0.3f, false, 440.0, 60, 0.0 } };
    for (const auto& c : cases)
    {
        Analyzer a;
        a.prepare (48000.0);
        a.setOptions (c.a4, c.react, c.bass);
        std::vector<float> block (256);
        double phase = 0.0;
        for (int b = 0; b < 48000 * 2 / 256; ++b)
        {
            for (auto& x : block)
            {
                x = (float) (0.3 * std::sin (phase) + 0.1 * std::sin (2.0 * phase));
                phase += 2.0 * 3.141592653589793 * c.hz / 48000.0;
            }
            a.push (block.data(), (int) block.size());
        }
        const auto r = a.reading();
        CHECK (r.valid && r.note == c.note && std::abs (r.cents - c.cents) < 2.0);
        if (! (r.valid && r.note == c.note && std::abs (r.cents - c.cents) < 2.0))
            std::printf ("  tuner %.2f Hz: valid %d note %d cents %.2f hz %.2f\n", c.hz, (int) r.valid, r.note, r.cents, r.hz);
    }
    // silence clears the display
    {
        Analyzer a;
        a.prepare (48000.0);
        std::vector<float> zero (48000, 0.0f);
        a.push (zero.data(), (int) zero.size());
        CHECK (! a.reading().valid);
    }
}

TEST (fl165g_html_notebook)
{
    using namespace lynx::flextra::htmlnotebook;
    // FL's presets: version 1 + a Pascal short string
    std::vector<std::uint8_t> state { 1, 0, 0, 0, 22 };
    const std::string google = "http://www.google.com/";
    state.insert (state.end(), google.begin(), google.end());
    CHECK (address (state) == google && isWebAddress (google) && ! isWebAddress ("C:\\notes\\a.html"));
    CHECK (address ({ 1, 0, 0, 0, 0 }).empty() && address ({}).empty() && address ({ 1, 0, 0, 0, 9, 'a' }).empty());
    CHECK (stateFor (google) == state);
    const auto fx = lynx::effectFromFl ("Fruity HTML NoteBook", state, 120.0);
    CHECK (fx && fx->kind == lynx::EffectKind::htmlNotebook && address (fx->state) == google);

    const auto page = parse ("<html><head><title>My &amp; notes</title><style>p{color:red}</style><script>var a='<p>x</p>';</script></head>"
                             "<body><h1>Title</h1><p>Some   <b>bold</b>\n text &copy; <a href=\"x.html\">link</a></p>"
                             "<ul><li>one</li><li>two</li></ul><!-- hidden --><hr><pre>a  b\nc</pre></body></html>");
    CHECK (page.title == "My & notes");
    CHECK (page.blocks.size() == 6);
    if (page.blocks.size() == 6)
    {
        using K = Block::Kind;
        CHECK (page.blocks[0].kind == K::heading1 && page.blocks[0].runs[0].text == "Title");
        const auto& p = page.blocks[1];
        CHECK (p.kind == K::paragraph && p.runs.size() == 4 && p.runs[0].text == "Some " && p.runs[1].bold && p.runs[1].text == "bold");
        CHECK (p.runs.size() == 4 && p.runs[2].text == " text \xc2\xa9 " && p.runs[3].link == "x.html" && p.runs[3].text == "link");
        CHECK (page.blocks[2].kind == K::listItem && page.blocks[3].kind == K::listItem && page.blocks[3].runs[0].text == "two");
        CHECK (page.blocks[4].kind == K::rule && page.blocks[5].kind == K::preformatted && page.blocks[5].runs[0].text == "a  b\nc");
    }
}

TEST (fl165g_inert_and_project_round_trip)
{
    using namespace lynx;
    CHECK (! flextra::inertPluginNote ("Razer Chroma").empty() && ! flextra::inertPluginNote ("ReWired").empty());
    CHECK (! flextra::inertPluginNote ("FL Studio Mobile Rack FX").empty() && flextra::inertPluginNote ("Fruity Limiter").empty());
    for (const auto k : { EffectKind::fruityMute2, EffectKind::bigClock, EffectKind::tuner, EffectKind::htmlNotebook })
    {
        CHECK (flPluginsForEffect (k).size() == 1);
        CHECK (effectForFlPlugin (flPluginsForEffect (k)[0]) == k);
        auto e = makeEffect (k);
        e->prepare (48000.0);
        std::vector<float> l (64, 0.25f), r (64, 0.25f);
        e->process (l.data(), r.data(), 64, makeEffectSlot (k).params, 120.0);
        // a new Mute 2 lets the sound through until it is switched off; the others never touch the sound
        CHECK (l[3] == 0.25f && r[3] == 0.25f);
    }
}
