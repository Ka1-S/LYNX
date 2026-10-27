// Round 165d: VFX Script runs Python (core/Python.h, core/VfxScript.h)

#include "core/Python.h"
#include "core/VfxScript.h"

namespace vfxs165
{
using lynx::vfx::Event;

std::vector<std::uint8_t> state (const std::string& source, const std::vector<std::pair<std::string, std::string>>& params = {})
{
    auto s = lynx::vfx::defaultScript();
    s.setSource (source);
    if (! params.empty())
        s.setParameters (params);
    return lynx::vfx::writeScript (s);
}

struct Timed
{
    long at = 0; // absolute sample
    Event e;
};

// Plays `total` samples in blocks of n (the song playing from tick 0 at `bpm`, 44.1 kHz); `in` at absolute samples
std::vector<Timed> play (lynx::vfx::Processor& p, std::vector<Timed> in, long total, int n = 512, bool playing = true, double bpm = 120.0)
{
    std::vector<Timed> out;
    lynx::vfx::Transport t;
    t.sampleRate = 44100.0;
    t.bpm = bpm;
    t.playing = playing;
    std::size_t next = 0;
    for (long start = 0; start < total; start += n)
    {
        std::vector<Event> block;
        while (next < in.size() && in[next].at < start + n)
        {
            auto e = in[next++].e;
            e.offset = (int) (in[next - 1].at - start);
            block.push_back (e);
        }
        t.ticks = (double) start * bpm * 960.0 / (60.0 * t.sampleRate);
        lynx::vfx::EventList list;
        p.process (block.data(), (int) block.size(), n, t, list);
        list.sortByOffset();
        for (int k = 0; k < list.size(); ++k)
            out.push_back ({ start + list[k].offset, list[k] });
    }
    return out;
}

Timed on (long at, int key, float velocity = 0.8f, int colour = 0)
{
    return { at, vfx164::on (key, 0, colour, velocity) };
}

Timed off (long at, int key, int colour = 0)
{
    return { at, vfx164::off (key, 0, colour) };
}

std::vector<int> keysOn (const std::vector<Timed>& out)
{
    std::vector<int> keys;
    for (const auto& o : out)
        if (o.e.type == Event::Type::on)
            keys.push_back (o.e.voice.key);
    return keys;
}

const char* passThrough = R"(
import flvfx as vfx

class Child(vfx.Voice):
    parent = None

def onTriggerVoice(v):
    for shift in (0, 4, 7):
        c = Child(v)
        c.parent = v
        c.note += shift
        c.trigger()

def onReleaseVoice(v):
    for c in list(vfx.context.voices):
        if c.parent is v:
            c.release()
)";
} // namespace vfxs165

TEST (vfx_script_runs_python_on_the_notes)
{
    using namespace lynx::vfx;
    auto p = makeProcessor (Kind::script, vfxs165::state (vfxs165::passThrough));
    const auto out = vfxs165::play (*p, { vfxs165::on (100, 60, 0.5f), vfxs165::off (3000, 60) }, 4096);
    // three notes on at the note's sample, three offs at the release's
    CHECK (out.size() == 6);
    if (out.size() != 6)
        return;
    CHECK ((vfxs165::keysOn (out) == std::vector<int> { 60, 64, 67 }));
    for (int k = 0; k < 3; ++k)
    {
        CHECK (out[(std::size_t) k].at == 100 && std::abs (out[(std::size_t) k].e.voice.velocity - 0.5f) < 1e-6f);
        CHECK (out[(std::size_t) k + 3].at == 3000 && out[(std::size_t) k + 3].e.type == Event::Type::off);
    }
    CHECK (out[3].e.id == out[0].e.id);
}

TEST (vfx_script_ticks_play_notes_at_their_sample)
{
    using namespace lynx::vfx;
    // an eighth-note pulse (48 of 96 ticks), each half as long, from the song's start
    const char* src = R"(
import flvfx as vfx
def onTick():
    if vfx.context.isPlaying and vfx.context.ticks % 48 == 0:
        v = vfx.Voice()
        v.note = 72
        v.length = 24
        v.trigger()
)";
    auto p = makeProcessor (Kind::script, vfxs165::state (src));
    const auto out = vfxs165::play (*p, {}, 44100);
    // 120 bpm: a tick is 44100 · 60 / (120 · 96) = 229.6875 samples; an eighth 11025
    std::vector<long> ons, offs;
    for (const auto& o : out)
        (o.e.type == Event::Type::on ? ons : offs).push_back (o.at);
    if (ons != std::vector<long> { 0, 11025, 22050, 33075 })
    {
        for (const auto& o : out)
            std::printf ("  %s at %ld key %d\n", o.e.type == Event::Type::on ? "on" : "off", o.at, o.e.voice.key);
    }
    CHECK ((ons == std::vector<long> { 0, 11025, 22050, 33075 }));
    CHECK ((offs == std::vector<long> { 5513, 16538, 27563, 38588 })); // (24 ticks = 5512.5 samples, rounded)
    CHECK (p->busy());
}

TEST (vfx_script_controls_values_and_output_controllers)
{
    using namespace lynx::vfx;
    const char* src = R"(
import flvfx as vfx
vfx.addOutputController('Level', 0.25)
def onTick():
    f = vfx.context.form
    vfx.setOutputController('Level', f.getInputValue('Main: Amount'))
    vfx.setOutputController('Steps', f.getInputValue('Steps') / 10.0)
    vfx.setOutputController('Mode', 1.0 if f.GetInputValue('Mode') == 2 and f.getInputValue('Name') == 'abc' else 0.0)
def createDialog():
    form = vfx.ScriptDialog('Title', 'What it does')
    form.addGroup('Main')
    form.addInputKnob('Amount', 0.5, -1, 1, hint='the amount')
    form.endGroup()
    form.addInputKnobInt('Steps', 3, 1, 8)
    form.AddInputCombo('Mode', 'One,Two,Three', 0)
    form.addInputCheckbox('On', 1)
    form.addInputText('Name', 'x')
    vfx.addOutputController('Steps', 0)
    vfx.addOutputController('Mode', 0)
    return form
)";
    auto settings = defaultScript();
    settings.setSource (src);
    const auto info = script::describe (settings);
    CHECK (info->compiled);
    if (! info->compiled)
    {
        std::printf ("  %s\n", info->error.c_str());
        return;
    }
    CHECK (info->title == "Title" && info->description == "What it does");
    CHECK (info->controls.size() == 5);
    CHECK (info->controls[0].name == "Main: Amount" && info->controls[0].type == script::Control::Type::knob && info->controls[0].hint == "the amount");
    CHECK (info->controls[2].options == (std::vector<std::string> { "One", "Two", "Three" }));
    CHECK ((info->outputs == std::vector<std::string> { "Level", "Steps", "Mode" }));
    // FL's encodings: a knob as 2^18-normalized, an int / combo as the number, a text as it is
    settings.setParameters ({ { "Main: Amount", "196608" }, { "Steps", "7" }, { "Mode", "2" }, { "On", "0" }, { "Name", "abc" } });
    auto v = script::valuesOf (*info, settings);
    CHECK (std::abs (v.numbers[0] - 0.5) < 1e-9 && v.numbers[1] == 7.0 && v.numbers[2] == 2.0 && v.numbers[3] == 0.0 && v.texts[4] == "abc");
    auto again = settings;
    script::writeValues (again, *info, v);
    CHECK (again.parameters() == settings.parameters());
    CHECK (script::encodeValue (info->controls[0], 0.0, {}) == "131072"); // (0 in −1..1: the middle)

    auto p = makeProcessor (Kind::script, writeScript (settings));
    CHECK (std::abs (p->controlOutput (0) - 0.25f) < 1e-6f); // (the default, set at the top level)
    vfxs165::play (*p, {}, 2048);
    CHECK (std::abs (p->controlOutput (0) - 0.5f) < 1e-6f);
    CHECK (std::abs (p->controlOutput (1) - 0.7f) < 1e-6f);
    CHECK (p->controlOutput (2) == 1.0f);
    // an automation input (Patcher cable) moves a control: 0..1 over its range
    p->setParameter (0, 0.0f);
    p->setParameter (1, 1.0f);
    vfxs165::play (*p, {}, 1024);
    CHECK (p->controlOutput (0) == 0.0f); // (−1, clamped to the output's 0..1)
    CHECK (std::abs (p->controlOutput (1) - 0.8f) < 1e-6f);
}

TEST (vfx_script_errors_are_reported_and_never_hang)
{
    using namespace lynx::vfx;
    // a syntax error: the line and the message, as FL's editor shows them
    const auto bad = script::describe (std::string ("import flvfx as vfx\ndef onTick(:\n    pass\n"));
    CHECK (! bad->compiled && bad->error.find ("SyntaxError") != std::string::npos && bad->error.find ("line 2") != std::string::npos);
    // a runtime error in a callback: logged once, then counted; the script keeps running
    State st;
    st.kind = Kind::script;
    st.tag = 0x165d0001;
    const char* src = R"(
import flvfx as vfx
print('hello from the script')
n = 0
def onTick():
    global n
    n += 1
    if n % 2 == 0:
        raise ValueError('even tick')
    vfx.setOutputController('n', min(1.0, n / 1000.0))
vfx.addOutputController('n', 0)
)";
    auto p = makeProcessor (Kind::script, vfxs165::state (src));
    p->setTag (st.tag);
    vfxs165::play (*p, {}, 8192);
    const auto monitor = script::monitorOf (st.tag);
    CHECK (monitor != nullptr);
    if (monitor == nullptr)
        return;
    const auto log = monitor->log();
    CHECK (log.find ("hello from the script") != std::string::npos);
    CHECK (log.find ("ValueError: even tick") != std::string::npos);
    CHECK (log.find ("the same error again") != std::string::npos);
    CHECK (monitor->status().empty());
    CHECK (p->controlOutput (0) > 0.01f);

    // an endless loop: interrupted, the instance stops and says why; the audio thread never waits long
    script::setRealtime (true);
    auto loop = makeProcessor (Kind::script, vfxs165::state ("import flvfx as vfx\ndef onTriggerVoice(v):\n    while True:\n        try:\n            pass\n        except:\n            pass\n"));
    loop->setTag (0x165d0002);
    const auto begin = std::chrono::steady_clock::now();
    double worst = 0.0;
    std::vector<double> times;
    for (int b = 0; b < 400; ++b)
    {
        lynx::vfx::EventList out;
        lynx::vfx::Transport t;
        auto e = vfx164::on (60);
        const auto before = std::chrono::steady_clock::now();
        loop->process (&e, b == 0 ? 1 : 0, 512, t, out);
        const auto ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - before).count();
        worst = std::max (worst, ms);
        times.push_back (ms);
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
    script::setRealtime (false);
    const auto m2 = script::monitorOf (0x165d0002);
    CHECK (m2 != nullptr && m2->status().find ("took too long") != std::string::npos);
    std::sort (times.begin(), times.end());
    const auto p95 = times[times.size() * 95 / 100];
    std::printf ("  endless loop: stopped; block wait 95th percentile %.2f ms, worst %.2f ms, %.0f ms in all\n", p95, worst,
                 std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - begin).count());
    // the wait is bounded by the budget (2 ms at most); the worst block also carries the OS scheduler's noise on a
    // loaded machine (a test thread has no real-time priority)
    CHECK (p95 < 2.5);
    CHECK (worst < 50.0);
}

TEST (vfx_script_memory_runaway_is_stopped)
{
    using namespace lynx::vfx;
    // a script that keeps allocating is stopped at LYNX's heap limit (768 MB), with a message; the memory comes back
    auto p = makeProcessor (Kind::script, vfxs165::state ("import flvfx as vfx\nkeep = []\ndef onTick():\n    while True:\n        try:\n            keep.append([0] * 4000000)\n        except:\n            pass\n"));
    p->setTag (0x165d0005);
    vfxs165::play (*p, {}, 1024);
    auto m = script::monitorOf (0x165d0005);
    CHECK (m != nullptr && m->status().find ("more than 768 MB") != std::string::npos);
    if (m != nullptr)
        std::printf ("  %s\n", m->status().c_str());
    m.reset();
    p.reset(); // (its VM is released: the next script starts with a small heap)
    auto q = makeProcessor (Kind::script, vfxs165::state ("import flvfx as vfx\nbig = [0] * 1000000\ndef onTick():\n    vfx.setOutputController('ok', 1)\nvfx.addOutputController('ok', 0)\n"));
    vfxs165::play (*q, {}, 512);
    CHECK (q->controlOutput (0) == 1.0f);
}

TEST (vfx_script_voice_changes_reach_the_sounding_note)
{
    using namespace lynx::vfx;
    const char* src = R"(
import flvfx as vfx
def onTriggerVoice(v):
    c = vfx.Voice(v)
    c.parent = v
    c.trigger()
def onTick():
    for c in vfx.context.voices:
        c.copyFrom(c.parent)
        c.note += 0.5
        c.pan = -1
)";
    auto p = makeProcessor (Kind::script, vfxs165::state (src));
    const auto out = vfxs165::play (*p, { vfxs165::on (0, 60) }, 1024);
    bool pitch = false, pan = false;
    for (const auto& o : out)
        if (o.e.type == Event::Type::prop)
        {
            pitch = pitch || (o.e.prop == Prop::pitch && std::abs (o.e.value - 50.0f) < 1e-3f);
            pan = pan || (o.e.prop == Prop::pan && o.e.value == -1.0f);
        }
    CHECK (pitch && pan);
    CHECK (! out.empty() && out[0].e.type == Event::Type::on && out[0].e.voice.key == 60);
}

TEST (vfx_script_python_has_what_fl_scripts_use)
{
    using namespace lynx::vfx;
    // CPython features pocketpy lacks that FL's factory scripts use: LYNX adds or patches them
    const char* src = "import flvfx as vfx\nimport re, random, sys, math, time\n"
                      "text = '''two lines \\\nas one'''\n"
                      "class Mine(vfx.Voice):\n    def __init__(self):\n        super().__init__()\n        self.extra: int = 3\n"
                      "def check():\n"
                      "    assert re.sub(r'choices\\((.*?)\\)( |$)', r'choices(\\1)[0] ', 'choices([1,2],k=1)') == 'choices([1,2],k=1)[0] '\n"
                      "    assert re.findall(r'\\d+', 'a1b22c333') == ['1', '22', '333']\n"
                      "    m = re.match(r'(?P<w>[a-z]+)(\\d)', 'ab7x')\n"
                      "    assert m.group('w') == 'ab' and m.group(2) == '7' and m.span() == (0, 3)\n"
                      "    assert re.split(r',\\s*', 'a, b,c') == ['a', 'b', 'c']\n"
                      "    assert pow(2, 10, 1000) == 24 and pow(2, 3) == 8\n"
                      "    assert sys.float_info.epsilon < 1e-15\n"
                      "    random.seed(1.5)\n"
                      "    g = [random.gauss(0, 1) for i in range(2000)]\n"
                      "    assert abs(sum(g) / len(g)) < 0.15\n"
                      "    assert next((x for x in [1, 5, 9] if x > 4), None) == 5\n"
                      "    assert sum(x * x for x in range(4)) == 14\n"
                      "    assert text == 'two lines as one'\n"
                      "    assert Mine().extra == 3 and Mine().note == 60.0\n"
                      "    assert time.time_ns() > 0\n"
                      "    vfx.setOutputController('ok', 1)\n"
                      "vfx.addOutputController('ok', 0)\n"
                      "def onTick():\n    check()\n";
    auto p = makeProcessor (Kind::script, vfxs165::state (src));
    p->setTag (0x165d0003);
    vfxs165::play (*p, {}, 512);
    CHECK (p->controlOutput (0) == 1.0f);
    if (p->controlOutput (0) != 1.0f)
        if (const auto m = script::monitorOf (0x165d0003))
            std::printf ("  %s\n", m->log().c_str());
    // the sandbox: no files, no processes
    auto q = makeProcessor (Kind::script, vfxs165::state ("import flvfx as vfx\nimport os\n"));
    q->setTag (0x165d0004);
    const auto m = script::monitorOf (0x165d0004);
    CHECK (m != nullptr && m->status().find ("ImportError") != std::string::npos);
    CHECK (! script::describe (std::string ("x = open('a.txt', 'w')\n"))->compiled);
}

TEST (vfx_script_late_answers_play_at_the_next_block)
{
    using namespace lynx::vfx;
    // a script slower than the real-time budget: its note comes one block later, at the block's start
    const char* src = R"(
import flvfx as vfx, time
def onTriggerVoice(v):
    t = time.time() + 0.006
    while time.time() < t:
        pass
    c = vfx.Voice(v)
    c.trigger()
)";
    auto p = makeProcessor (Kind::script, vfxs165::state (src));
    script::setRealtime (true);
    lynx::vfx::Transport t;
    lynx::vfx::EventList first, second;
    auto e = vfx164::on (60, 100);
    p->process (&e, 1, 512, t, first);
    std::this_thread::sleep_for (std::chrono::milliseconds (30));
    p->process (nullptr, 0, 512, t, second);
    script::setRealtime (false);
    CHECK (first.size() == 0);
    CHECK (second.size() == 1 && second[0].offset == 0 && second[0].voice.key == 60);
    // offline (export) it waits: the note is at its sample
    lynx::vfx::EventList exact;
    auto p2 = makeProcessor (Kind::script, vfxs165::state (src));
    p2->process (&e, 1, 512, t, exact);
    CHECK (exact.size() == 1 && exact[0].offset == 100);
}

TEST (vfx_script_runs_fl_factory_scripts)
{
    using namespace lynx::vfx;
    if (realFlpDir.empty())
        return;
    namespace fs = std::filesystem;
    const auto dir = fs::path (realFlpDir) / "Patches" / "Plugin presets" / "Effects" / "VFX Script";
    if (! fs::is_directory (dir))
        return;
    auto load = [&] (const fs::path& file) -> std::optional<ScriptSettings> {
        std::ifstream in (file, std::ios::binary);
        const std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (in)), {});
        const auto st = stateFromFlPreset (bytes, Kind::script);
        return st ? parseScript (*st) : std::nullopt;
    };
    int files = 0, compiled = 0;
    double slowest = 0.0;
    std::string slowestName;
    for (const auto& entry : fs::recursive_directory_iterator (dir))
    {
        if (entry.path().extension() != ".fst")
            continue;
        const auto s = load (entry.path());
        if (! s)
            continue;
        ++files;
        const auto info = script::describe (*s);
        if (info->compiled)
            ++compiled;
        else
            std::printf ("  %s: %s\n", entry.path().filename().string().c_str(), info->error.c_str());
        // each one runs 2 s with a held chord and the song playing, without an error
        auto p = makeProcessor (Kind::script, writeScript (*s));
        p->setTag (0x165d1000 + (std::uint32_t) files);
        const auto begin = std::chrono::steady_clock::now();
        vfxs165::play (*p, { vfxs165::on (0, 60), vfxs165::on (0, 64), vfxs165::on (0, 67), vfxs165::off (44100, 60), vfxs165::off (44100, 64), vfxs165::off (44100, 67) },
                       88200);
        const auto ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - begin).count();
        if (ms > slowest)
            slowest = ms, slowestName = entry.path().filename().string();
        if (const auto m = script::monitorOf (0x165d1000 + (std::uint32_t) files))
            if (m->log().find ("Error") != std::string::npos)
                std::printf ("  %s ran with errors:\n%s\n", entry.path().filename().string().c_str(), m->log().c_str());
    }
    std::printf ("  %d of %d FL scripts compile; slowest: %s, 2 s of audio in %.1f ms\n", compiled, files, slowestName.c_str(), slowest);
    CHECK (files >= 80 && compiled == files);

    // FL's Snap To Scale (spec S6): Enable, C major, Down: 61 → 60, 63 → 62; Up: 61 → 62
    if (auto s = load (dir / "Snap To Scale.fst"))
    {
        const auto info = script::describe (*s);
        auto v = script::valuesOf (*info, *s);
        auto set = [&] (const char* name, double value) {
            if (const auto i = info->index (name); i >= 0)
                v.numbers[(std::size_t) i] = value;
        };
        set ("Enable", 1);
        set ("Key", 0);
        set ("Scale", 0);
        set ("Snap Direction", 1);
        script::writeValues (*s, *info, v);
        auto p = makeProcessor (Kind::script, writeScript (*s));
        auto keys = vfxs165::keysOn (vfxs165::play (*p, { vfxs165::on (0, 61), vfxs165::on (10, 63), vfxs165::on (20, 60) }, 512));
        CHECK ((keys == std::vector<int> { 60, 62, 60 }));
        set ("Snap Direction", 0);
        script::writeValues (*s, *info, v);
        p->configure (writeScript (*s));
        keys = vfxs165::keysOn (vfxs165::play (*p, { vfxs165::on (0, 61) }, 512));
        CHECK ((keys == std::vector<int> { 62 }));
    }
    // FL's Arpeggiator (Up, 2 per beat, 0.75 long): C E G held → C E G C… every 48 ticks
    if (auto s = load (dir / "Arpeggiator.fst"))
    {
        auto p = makeProcessor (Kind::script, writeScript (*s));
        const auto out = vfxs165::play (*p, { vfxs165::on (0, 60), vfxs165::on (0, 64), vfxs165::on (0, 67) }, 44100);
        std::vector<int> keys;
        std::vector<long> at;
        for (const auto& o : out)
            if (o.e.type == Event::Type::on)
                keys.push_back (o.e.voice.key), at.push_back (o.at);
        CHECK (keys.size() >= 4 && keys[0] == 60 && keys[1] == 64 && keys[2] == 67 && keys[3] == 60);
        CHECK (at.size() >= 2 && at[1] - at[0] == 11025);
    }
    // FL's Note Formula (spec S7): "note + 12"
    if (auto s = load (dir / "Note Formula Examples" / "1 - Raise the pitch by one octave.fst"))
    {
        auto p = makeProcessor (Kind::script, writeScript (*s));
        const auto keys = vfxs165::keysOn (vfxs165::play (*p, { vfxs165::on (300, 60) }, 2048));
        CHECK ((keys == std::vector<int> { 72 }));
    }
}
