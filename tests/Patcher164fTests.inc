// Round 164f: Patcher leftovers — sample-accurate MIDI into mixer-slot Patchers, MIDI output pins on plugin nodes,
// the Envelope Controller's Mod X / Y as automation targets, engine edits that keep the running node.

namespace patcher164f
{
// A hosted plugin stand-in that plays an arpeggio: each render sends out a note on (then its off on the next)
class FakeArp final : public lynx::Instrument
{
public:
    void prepare (double) noexcept override {}
    void noteOn (int, float) noexcept override {}
    void noteOff (int) noexcept override {}
    void allNotesOff() noexcept override {}
    void render (float*, float*, int, float, float) noexcept override
    {
        pending[count++] = { key, on ? 0.0f : 0.7f };
        on = ! on;
    }
    int takeMidiOutput (MidiOutNote* out, int max) noexcept override
    {
        const auto n = std::min (max, count);
        std::copy (pending.begin(), pending.begin() + n, out);
        count = 0;
        return n;
    }
    int key = 64;
    bool on = false;
    std::array<MidiOutNote, 8> pending {};
    int count = 0;
};

// What reaches an instrument
class Recorder final : public lynx::Instrument
{
public:
    void prepare (double) noexcept override {}
    void noteOn (int k, float) noexcept override { ons.push_back (k); }
    void noteOff (int k) noexcept override { offs.push_back (k); }
    void allNotesOff() noexcept override {}
    void render (float*, float*, int, float, float) noexcept override {}
    std::vector<int> ons, offs;
};

lynx::PatchNode generator (lynx::InstrumentKind kind)
{
    lynx::PatchNode n;
    n.generator = true;
    n.channel.instrument = kind;
    return n;
}

// the first sample of the master louder than a whisper (−1: none)
int onset (const std::vector<float>& l)
{
    for (std::size_t i = 0; i < l.size(); ++i)
        if (std::abs (l[i]) > 1.0e-5f)
            return (int) i;
    return -1;
}
} // namespace patcher164f

TEST (patcher164f_mixer_slot_patcher_notes_land_at_their_sample)
{
    using namespace patcher164f;
    // a MIDI Out channel on port 4 plays a note at a tick inside a block; a mixer-slot Patcher on insert 1 feeds a
    // synth from its "MIDI port 4" pin. The synth starts at the note's sample, not at the block's start (round 162 put
    // it at the chunk's start).
    // renders the song with its note at `noteTick`; returns the first audible sample
    const auto render = [] (std::int64_t noteTick)
    {
        auto song = lynx::Song::makeDefault();
        song.mixer.resize (3);
        song.channels.resize (1);
        song.channels[0] = {};
        song.channels[0].instrument = lynx::InstrumentKind::midiOut;
        lynx::midictl::MidiOutSettings s;
        s.port = 4;
        lynx::midictl::writeMidiOut (song.channels[0].pluginState, s);
        song.channels[0].mixerTrack = 1;
        lynx::EffectSlot slot = lynx::makeEffectSlot (lynx::EffectKind::patcher);
        slot.patcher.nodes = { generator (lynx::InstrumentKind::synth) };
        slot.patcher.wires = { patcher162::midi (lynx::patchFromHost, lynx::patchMidiPortPin (4), 0, 0), patcher141::audio (0, lynx::patchToHost) };
        song.mixer[1].effects = { slot };
        lynx::Pattern p;
        p.notes.push_back ({ noteTick, lynx::ppq, 0, 60, 0.8f });
        song.patterns = { p };
        lynx::Clip clip;
        clip.pattern = 0;
        clip.length = lynx::ppq * 4;
        song.playlist = { clip };
        song.mode = lynx::PlayMode::song;

        auto state = lynx::RackState::build (song, nullptr, 48000.0);
        lynx::RackRenderer r;
        r.prepare (48000.0, 512);
        std::vector<float> all, l (512), rr (512);
        double tick = 0.0;
        for (int b = 0; b < 40; ++b, tick += 512.0 * song.bpm * lynx::ppq / 60.0 / 48000.0)
        {
            r.process (state.get(), true, tick, 512, l.data(), rr.data(), nullptr);
            all.insert (all.end(), l.begin(), l.end());
        }
        return std::make_pair (onset (all), (int) std::ceil ((double) noteTick / lynx::ppq * 60.0 / song.bpm * 48000.0));
    };
    // the path's own delay to the first audible sample (the synth's attack, the insert's latency compensation): a note
    // on a block's first sample
    const auto [reference, referenceAt] = render (0);
    CHECK (referenceAt == 0 && reference >= 0);
    // the same note inside a block sounds that long after its own sample (at the block's start it would sound
    // expected % 512 samples early)
    const auto [first, expected] = render (lynx::ppq / 2); // 0.5 beat
    CHECK (expected % 512 > 16); // (the note falls inside a block)
    if (std::abs (first - (expected + reference)) > 1)
        std::printf ("  patcher164f: onset %d, note at sample %d, reference onset %d\n", first, expected, reference);
    CHECK (std::abs (first - (expected + reference)) <= 1);
}

TEST (patcher164f_graph_queued_notes_play_at_their_offset_across_pieces)
{
    using namespace patcher164f;
    lynx::PatcherDoc doc;
    doc.nodes = { generator (lynx::InstrumentKind::synth) };
    doc.wires = { patcher162::midi (lynx::patchFromHost, lynx::patchMidiPortPin (0), 0, 0), patcher141::audio (0, lynx::patchToHost) };
    lynx::patcher::Graph g (doc, 48000.0, lynx::patcher::builtInFactories (48000.0));
    std::vector<float> in (256, 0.0f), l (256), r (256);
    g.queueNoteOnPort (0, 69, 0.9f, -1, 300); // 44 samples into the second block of 256
    g.process (in.data(), in.data(), nullptr, nullptr, l.data(), r.data(), 256, 120.0);
    CHECK (onset (l) == -1);
    g.process (in.data(), in.data(), nullptr, nullptr, l.data(), r.data(), 256, 120.0);
    const auto first = onset (l);
    CHECK (first >= 43 && first <= 52);
    // a port nothing is cabled to takes nothing; all notes off empties the queue
    g.queueNoteOnPort (9, 69, 0.9f, -1, 0);
    g.queueNoteOnPort (0, 72, 0.9f, -1, 100000);
    g.allNotesOff();
    lynx::patcher::Graph quiet (doc, 48000.0, lynx::patcher::builtInFactories (48000.0));
    quiet.queueNoteOnPort (9, 69, 0.9f, -1, 0);
    quiet.process (in.data(), in.data(), nullptr, nullptr, l.data(), r.data(), 256, 120.0);
    CHECK (onset (l) == -1);
}

TEST (patcher164f_plugin_node_midi_output_plays_the_nodes_cabled_to_it)
{
    using namespace patcher164f;
    lynx::PatcherDoc doc;
    doc.nodes = { generator (lynx::InstrumentKind::plugin), generator (lynx::InstrumentKind::synth) };
    doc.wires = { patcher162::midi (0, 0, 1, 0) };
    // a hosted plugin node shows a MIDI output pin
    const auto ports = lynx::patcher::nodePorts (doc.nodes[0]);
    CHECK (ports.midiOut && ports.midiOuts == 1 && lynx::patcher::shownMidiOutputs (doc, 0) == 1);

    auto arp = std::make_shared<FakeArp>();
    auto rec = std::make_shared<Recorder>();
    auto f = lynx::patcher::builtInFactories (48000.0);
    f.instrument = [arp, rec] (const lynx::ChannelDef& c) -> std::shared_ptr<lynx::Instrument> {
        if (c.instrument == lynx::InstrumentKind::plugin)
            return arp;
        return rec;
    };
    lynx::patcher::Graph g (doc, 48000.0, f);
    std::vector<float> in (128, 0.0f), l (128), r (128);
    g.process (in.data(), in.data(), nullptr, nullptr, l.data(), r.data(), 128, 120.0); // the arp sends its note on
    CHECK (rec->ons.empty());                                                              // (heard at the next piece)
    g.process (in.data(), in.data(), nullptr, nullptr, l.data(), r.data(), 128, 120.0);
    CHECK (rec->ons == std::vector<int> { 64 });
    g.process (in.data(), in.data(), nullptr, nullptr, l.data(), r.data(), 128, 120.0);
    CHECK (rec->offs == std::vector<int> { 64 });

    // without a cable on the pin the plugin's notes go nowhere
    doc.wires.clear();
    auto rec2 = std::make_shared<Recorder>();
    f.instrument = [arp, rec2] (const lynx::ChannelDef& c) -> std::shared_ptr<lynx::Instrument> {
        return c.instrument == lynx::InstrumentKind::plugin ? std::shared_ptr<lynx::Instrument> (arp) : std::shared_ptr<lynx::Instrument> (rec2);
    };
    lynx::patcher::Graph g2 (doc, 48000.0, f);
    for (int k = 0; k < 3; ++k)
        g2.process (in.data(), in.data(), nullptr, nullptr, l.data(), r.data(), 128, 120.0);
    CHECK (rec2->ons.empty());
}

TEST (patcher164f_envelope_controller_mod_xy_are_automation_targets)
{
    using namespace patcher164f;
    // continuous output, bipolar, Mod X map = a straight line: output 1 follows Mod X
    auto ec = lynx::defaultEnvelopeController();
    ec.continuous = true;
    ec[0].bipolar = true;
    ec[0].base = 0.5f;
    ec[0].maps[lynx::mapModX] = ec162::line (0.0f, 1.0f);
    ec.modX = 0.5f;
    lynx::ChannelDef ch;
    ch.instrument = lynx::InstrumentKind::controller;
    ch.controllerState = lynx::writeEnvelopeController (ec);
    auto i = lynx::makeInstrument (lynx::InstrumentKind::controller);
    i->prepare (48000.0);
    i->configure (ch, 140.0);
    std::vector<float> l (256), r (256);
    i->render (l.data(), r.data(), 256, 1.0f, 1.0f);
    CHECK (std::abs (i->controlOutput (0) - 0.5f) < 1.0e-3f);
    i->setParameter (lynx::envelopeControllerModXParam, 0.9f); // automation / a link
    i->render (l.data(), r.data(), 256, 1.0f, 1.0f);
    CHECK (std::abs (i->controlOutput (0) - 0.9f) < 1.0e-3f);
    i->setParameter (5, 0.1f); // (another index: nothing)
    i->render (l.data(), r.data(), 256, 1.0f, 1.0f);
    CHECK (std::abs (i->controlOutput (0) - 0.9f) < 1.0e-3f);
    // the knob moved in the editor wins over the last automation value
    ec.modX = 0.2f;
    ch.controllerState = lynx::writeEnvelopeController (ec);
    i->configure (ch, 140.0);
    i->render (l.data(), r.data(), 256, 1.0f, 1.0f);
    CHECK (std::abs (i->controlOutput (0) - 0.2f) < 1.0e-3f);

    // the song reads / writes the value where it lives (the controller's state)
    lynx::Song song;
    song.channels = { ch };
    const lynx::AutomationTarget tx { lynx::AutomationTargetKind::channelParam, 0, 0, lynx::envelopeControllerModXParam };
    const lynx::AutomationTarget ty { lynx::AutomationTargetKind::channelParam, 0, 0, lynx::envelopeControllerModYParam };
    CHECK (std::abs (song.targetValue (tx) - 0.2f) < 1.0e-6f);
    CHECK (song.setTargetValue (ty, 0.75f));
    CHECK (std::abs (song.targetValue (ty) - 0.75f) < 1.0e-6f);
    CHECK (std::abs (lynx::parseEnvelopeController (song.channels[0].controllerState)->modY - 0.75f) < 1.0e-6f);

    // the rack drives it: an automation clip on Mod X moves the running controller's output
    song = lynx::Song::makeDefault();
    song.channels = { ch };
    song.channels[0].mixerTrack = 0;
    lynx::Automation a;
    a.target = tx;
    a.points = { { 0, 1.0f, 0.0f }, { lynx::ppq * 4, 1.0f, 0.0f } };
    song.automations = { a };
    lynx::Clip clip;
    clip.kind = lynx::ClipKind::automation;
    clip.automation = 0;
    clip.length = lynx::ppq * 4;
    song.playlist = { clip };
    song.patterns.clear();
    song.mode = lynx::PlayMode::song;
    auto state = lynx::RackState::build (song, nullptr, 48000.0);
    lynx::RackRenderer rr;
    rr.prepare (48000.0, 256);
    rr.process (state.get(), true, 0.0, 256, l.data(), r.data(), nullptr);
    rr.process (state.get(), true, 10.0, 256, l.data(), r.data(), nullptr);
    CHECK (std::abs (state->instruments[0]->controlOutput (0) - 1.0f) < 1.0e-3f);
}

TEST (patcher164f_engine_edits_keep_the_running_node)
{
    using namespace patcher164f;
    // a 3x Osc node edited (its parameters) keeps its instance, as a Channel Rack channel does; a sampler with another
    // sample, an Envelope Controller turned into a Keyboard Controller or another kind are built again
    lynx::PatcherDoc doc;
    doc.nodes = { generator (lynx::InstrumentKind::threeosc), generator (lynx::InstrumentKind::sampler) };
    doc.nodes[0].channel.modelParams = std::vector<float> (64, 0.25f);
    doc.wires = { patcher141::audio (0, lynx::patchToHost) };
    const auto f = lynx::patcher::builtInFactories (48000.0);
    lynx::patcher::Graph g1 (doc, 48000.0, f);
    auto edited = doc;
    edited.nodes[0].channel.modelParams[3] = 0.75f;
    edited.nodes[0].channel.pluginState = { 1, 2, 3 };
    CHECK (lynx::patcher::sameStructure (doc, edited));
    lynx::patcher::Graph g2 (edited, 48000.0, f, &g1);
    CHECK (g2.instrumentAt (0) == g1.instrumentAt (0));
    edited.nodes[1].channel.samplePath = "other.wav";
    CHECK (! lynx::patcher::sameStructure (doc, edited));
    lynx::patcher::Graph g3 (edited, 48000.0, f, &g2);
    CHECK (g3.instrumentAt (0) == g1.instrumentAt (0) && g3.instrumentAt (1) != g2.instrumentAt (1));
    auto kinds = doc;
    kinds.nodes[0].channel.instrument = lynx::InstrumentKind::minisynth;
    CHECK (! lynx::patcher::sameStructure (doc, kinds));
    // a MIDI Out node's settings are routes: another MIDI channel is another structure
    lynx::PatcherDoc mo;
    mo.nodes = { patcher162::midiOutNode (3) };
    auto mo2 = mo;
    mo2.nodes[0] = patcher162::midiOutNode (4);
    CHECK (! lynx::patcher::sameStructure (mo, mo2));
}
