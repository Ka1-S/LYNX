#include "core/Patcher.h"
#include "core/ControlSurface.h"
#include "core/VfxScript.h"
#include "core/XyController.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <tuple>

#include "core/FlSynths.h"
#include "core/FrequencySplitter.h"
#include "core/MidiControls.h"
#include "core/Pdc.h"

namespace lynx::patcher
{
namespace
{
class PassEffect final : public Effect
{
public:
    void prepare (double) noexcept override {}
    void reset() noexcept override {}
    void process (float*, float*, int, const EffectParams&, double) noexcept override {}
};

// what makes a running node reusable: everything but the values the graph hands it every block
bool sameEffectSetup (const EffectSlot& a, const EffectSlot& b)
{
    if (a.kind != b.kind || a.plugin.id != b.plugin.id || a.state != b.state || a.samplePath != b.samplePath || ! (a.plugin == b.plugin))
        return false;
    if (a.kind == EffectKind::parallel)
        return a == b; // its children are built in
    if (a.kind == EffectKind::patcher)
        return sameStructure (a.patcher, b.patcher);
    // effects whose setup depends on parameters at creation (latency, mode) are rebuilt by the rack as well
    return true;
}

bool sameInstrumentSetup (const ChannelDef& a, const ChannelDef& b)
{
    // a VFX node takes new settings while it runs (its held voices end as they started): only its kind is structure
    if (a.instrument == InstrumentKind::vfx || b.instrument == InstrumentKind::vfx)
        return a.instrument == b.instrument && vfx::kindOf (a) == vfx::kindOf (b);
    if (a.instrument != b.instrument)
        return false;
    // the rack's own rule for a channel (RackState::build): what configure() cannot take means a new instance; the rest
    // reaches the running one (round 164f: an engine edited in its editor inside a Patcher keeps playing, as on a
    // channel, instead of starting again on each knob move)
    switch (a.instrument)
    {
        case InstrumentKind::plugin:
            return a.plugin.id == b.plugin.id && a.plugin == b.plugin && a.pluginState == b.pluginState;
        case InstrumentKind::patcher: return sameStructure (a.patcher, b.patcher);
        case InstrumentKind::midiOut: return a.pluginState == b.pluginState; // (its MIDI channel is in the graph's routes)
        case InstrumentKind::controller: // (a Keyboard Controller has 1 output, an Envelope Controller 8)
            return parseKeyboardController (a.controllerState).has_value() == parseKeyboardController (b.controllerState).has_value();
        case InstrumentKind::sampler:
        case InstrumentKind::audioClip:
        case InstrumentKind::flkeys:
            return a.samplePath == b.samplePath && a.zones == b.zones && a.modelParams == b.modelParams && a.sampler.keepPitch == b.sampler.keepPitch
                   && (! a.sampler.keepPitch || (a.sampler.stretchBeats == b.sampler.stretchBeats && a.sampler.stretchMethod == b.sampler.stretchMethod));
        case InstrumentKind::granulizer:
        case InstrumentKind::soundfont: return a.samplePath == b.samplePath;
        case InstrumentKind::directwave: return a.zones == b.zones && a.modelParams == b.modelParams && a.pluginState == b.pluginState;
        case InstrumentKind::dashboard: // (its outputs are structure)
            return std::equal (a.patcher.surface.begin(), a.patcher.surface.end(), b.patcher.surface.begin(), b.patcher.surface.end(),
                               [] (const SurfaceControl& x, const SurfaceControl& y) { return x.kind == y.kind; });
        default: return true;
    }
}

float mapControl (const std::vector<float>& curve, float v) noexcept
{
    v = std::clamp (v, 0.0f, 1.0f);
    if (curve.size() < 2)
        return v;
    const auto x = v * (float) (curve.size() - 1);
    const auto i = std::min ((std::size_t) x, curve.size() - 2);
    const auto f = x - (float) i;
    return curve[i] + (curve[i + 1] - curve[i]) * f;
}
} // namespace

std::optional<PatchNode::BusHead> busHeadOf (const PatchNode& node, int bus)
{
    if (bus <= 0 || node.generator)
        return std::nullopt;
    for (const auto& h : node.busHeads)
        if (h.bus == bus)
            return h;
    if (node.effect.kind == EffectKind::frequencySplitter && bus <= 3) // LYNX's own: band sends 1 low, 2 mid, 3 high
    {
        PatchNode::BusHead head;
        head.bus = bus;
        auto slot = node.effect;
        slot.params[(std::size_t) fsplit::outputLow] = bus == 1 ? 1.0f : 0.0f;
        slot.params[(std::size_t) fsplit::outputMid] = bus == 2 ? 1.0f : 0.0f;
        slot.params[(std::size_t) fsplit::outputHigh] = bus == 3 ? 1.0f : 0.0f;
        head.head = slot;
        return head;
    }
    return std::nullopt;
}

NodePorts nodePorts (const PatchNode& node)
{
    NodePorts p;
    if (node.generator)
    {
        p.audioIns = 0;
        p.midiIn = true;
        if (node.channel.instrument == InstrumentKind::controller) // Envelope Controller: 8 outputs; Keyboard Controller: 1
            p.controlOuts = parseKeyboardController (node.channel.controllerState) ? 1 : 8;
        if (node.channel.instrument == InstrumentKind::dashboard) // round 164: one per control (an XY pad: two)
            p.controlOuts = surface::firstOutput (node.channel.patcher.surface, (int) node.channel.patcher.surface.size());
        if (node.channel.instrument == InstrumentKind::controller || node.channel.instrument == InstrumentKind::midiOut
            || node.channel.instrument == InstrumentKind::dashboard || node.channel.instrument == InstrumentKind::layer)
            p.audioOuts = 0; // (no sound of their own)
        // a MIDI Out node; a hosted plugin's notes out (round 164f: an arpeggiator / MIDI effect plugin plays the
        // nodes cabled to it, as FL's Patcher shows a plugin's MIDI output)
        p.midiOut = node.channel.instrument == InstrumentKind::midiOut || node.channel.instrument == InstrumentKind::plugin;
        p.midiOuts = p.midiOut ? 1 : 0;
        if (const auto kind = vfx::kindOf (node.channel)) // a VFX node: voices in, voices out (round 164b)
        {
            p.audioOuts = 0;
            p.midiOut = true;
            p.midiOuts = vfx::outputCount (*kind);
            if (*kind == vfx::Kind::script) // its output controllers (round 165d)
                if (const auto state = vfx::readState (node.channel.pluginState))
                    if (const auto settings = vfx::parseScript (state->fl))
                        p.controlOuts = (int) vfx::script::describe (*settings)->outputs.size();
        }
        if (node.channel.instrument == InstrumentKind::patcher)
            p.audioOuts = 1 + (int) node.channel.patcher.outputTracks.size();
        return p;
    }
    p.audioIns = 2; // main + side-chain
    p.midiIn = node.effect.kind == EffectKind::plugin;
    int highest = 0;
    for (const auto& h : node.busHeads)
        highest = std::max (highest, h.bus);
    if (node.effect.kind == EffectKind::frequencySplitter)
        highest = std::max (highest, 3);
    p.audioOuts = 1 + highest;
    if (node.effect.kind == EffectKind::peakController)
        p.controlOuts = 3;
    else if (node.effect.kind == EffectKind::formulaController)
        p.controlOuts = 1;
    else if (xy::isXyController (node.effect.kind)) // round 164: X, Y (, Z)
        p.controlOuts = xy::axesOf (node.effect.kind);
    else if (node.effect.kind == EffectKind::controlSurface) // round 164: one per control (an XY pad: two)
        p.controlOuts = surface::firstOutput (node.effect.patcher.surface, (int) node.effect.patcher.surface.size());
    return p;
}

bool sameStructure (const PatcherDoc& a, const PatcherDoc& b)
{
    if (a.nodes.size() != b.nodes.size() || a.wires.size() != b.wires.size() || a.outputTracks != b.outputTracks || ! (a.published == b.published))
        return false;
    // the Surface's controls: their outputs are structure, their values are not (round 151)
    if (a.surface.size() != b.surface.size())
        return false;
    for (std::size_t i = 0; i < a.surface.size(); ++i)
        if (a.surface[i].kind != b.surface[i].kind)
            return false;
    for (std::size_t i = 0; i < a.nodes.size(); ++i)
    {
        const auto& x = a.nodes[i];
        const auto& y = b.nodes[i];
        if (x.generator != y.generator || x.busHeads.size() != y.busHeads.size())
            return false;
        if (x.generator ? ! sameInstrumentSetup (x.channel, y.channel) : ! sameEffectSetup (x.effect, y.effect))
            return false;
        for (std::size_t h = 0; h < x.busHeads.size(); ++h)
            if (x.busHeads[h].bus != y.busHeads[h].bus || x.busHeads[h].head.has_value() != y.busHeads[h].head.has_value()
                || (x.busHeads[h].head && ! sameEffectSetup (*x.busHeads[h].head, *y.busHeads[h].head)))
                return false;
    }
    for (std::size_t i = 0; i < a.wires.size(); ++i)
    {
        const auto& x = a.wires[i];
        const auto& y = b.wires[i];
        if (x.kind != y.kind || x.from != y.from || x.fromPort != y.fromPort || x.to != y.to || x.toPort != y.toPort || x.curve != y.curve
            || (x.kind != PatchWire::Kind::audio && x.enabled != y.enabled))
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------

struct Graph::Node
{
    bool generator = false;
    EffectSlot setup;   // effect nodes
    ChannelDef channel; // generator nodes
    std::shared_ptr<Effect> effect;
    std::shared_ptr<Instrument> instrument;
    bool plugin = false; // values reach it through setParameter (control cables), not the params array
    vfx::Processor* vfx = nullptr;                    // a VFX node's processor (owned by its instrument)
    std::vector<MidiTarget> midiOutTargets;           // a plugin node: what its MIDI output pin reaches
    std::vector<std::vector<MidiTarget>> vfxTargets;  // [voice output] what it reaches

    struct Head
    {
        int bus = 1;
        std::shared_ptr<Effect> effect; // null = direct
        EffectSlot slot;
        float gain = 1.0f;
    };
    std::vector<Head> heads;
    std::vector<std::array<std::vector<float>, 2>> out; // [0] main output, [1 + h] head h
    bool sidechain = false;
    int inLatency = 0, outLatency = 0;
    std::vector<int> mainInputs, sideInputs; // wire indices
};

struct Graph::AudioWire
{
    int from = patchFromHost, fromBuffer = 0; // node, and its output buffer (0 main, 1 + h head); host: 0 input, 1 side-chain
    int to = patchToHost, toBus = 0;
    std::shared_ptr<DelayLine> delay;
    int docIndex = 0;
};

struct Graph::ControlWire
{
    int from = 0, output = 0, to = 0, param = 0;
    std::vector<float> curve;
    int docIndex = 0;
};

struct Graph::VfxQueue
{
    vfx::EventList in, out; // the voices that reached the node in this piece; what it sends
};

struct Graph::NoteOut
{
    int node = 0, midiChannel = -1;
    vfx::Event event;
};

struct Graph::Values
{
    struct NodeValues
    {
        EffectParams params {};
        bool enabled = true;
        float mix = 1.0f;
        std::vector<EffectParams> heads;
        std::vector<float> headGains;
    };
    std::vector<NodeValues> nodes;
    std::vector<float> gains;
    std::vector<char> on;
    std::vector<float> surface; // the Surface node's control outputs
};

Graph::Graph (const PatcherDoc& doc, double sampleRate, const Factories& factories, const Graph* previous) : rate (sampleRate)
{
    const auto count = (int) doc.nodes.size();
    std::vector<char> taken (previous != nullptr ? previous->nodes.size() : 0, 0);

    for (int i = 0; i < count; ++i)
    {
        const auto& src = doc.nodes[(std::size_t) i];
        auto node = std::make_unique<Node>();
        node->generator = src.generator;

        // a running instance with the same setup (the previous graph's node at the same index first)
        auto reuse = [&] () -> const Node* {
            if (previous == nullptr)
                return nullptr;
            auto matches = [&] (std::size_t k) {
                const auto& p = *previous->nodes[k];
                return ! taken[k] && p.generator == src.generator
                       && (src.generator ? p.instrument != nullptr && sameInstrumentSetup (p.channel, src.channel)
                                         : p.effect != nullptr && sameEffectSetup (p.setup, src.effect));
            };
            if ((std::size_t) i < previous->nodes.size() && matches ((std::size_t) i))
            {
                taken[(std::size_t) i] = 1;
                return previous->nodes[(std::size_t) i].get();
            }
            for (std::size_t k = 0; k < previous->nodes.size(); ++k)
                if (matches (k))
                {
                    taken[k] = 1;
                    return previous->nodes[k].get();
                }
            return nullptr;
        }();

        if (src.generator)
        {
            node->channel = src.channel;
            node->plugin = src.channel.instrument == InstrumentKind::plugin;
            if (reuse != nullptr)
            {
                node->instrument = reuse->instrument;
                node->instrument->configure (src.channel, 140.0);
            }
            else if (factories.instrument)
                node->instrument = factories.instrument (src.channel);
        }
        else
        {
            node->setup = src.effect;
            node->plugin = src.effect.kind == EffectKind::plugin;
            if (reuse != nullptr)
                node->effect = reuse->effect;
            else if (factories.effect)
                node->effect = factories.effect (src.effect);
            if (node->effect == nullptr)
                node->effect = std::make_shared<PassEffect>();
        }

        // extra outputs used by some cable
        for (const auto& w : doc.wires)
            if (w.kind == PatchWire::Kind::audio && w.from == i && w.fromPort > 0
                && std::none_of (node->heads.begin(), node->heads.end(), [&] (const Node::Head& h) { return h.bus == w.fromPort; }))
                if (const auto head = busHeadOf (src, w.fromPort))
                {
                    Node::Head h;
                    h.bus = head->bus;
                    h.gain = head->gain;
                    if (head->head)
                    {
                        h.slot = *head->head;
                        const Node::Head* old = nullptr;
                        if (reuse != nullptr)
                            for (const auto& o : reuse->heads)
                                if (o.bus == h.bus && o.effect != nullptr && sameEffectSetup (o.slot, h.slot))
                                    old = &o;
                        h.effect = old != nullptr ? old->effect : (factories.effect ? factories.effect (h.slot) : nullptr);
                        if (h.effect == nullptr)
                            h.effect = std::make_shared<PassEffect>();
                    }
                    node->heads.push_back (std::move (h));
                }

        node->out.resize (1 + node->heads.size());
        for (auto& o : node->out)
            for (auto& side : o)
                side.assign ((std::size_t) capacity, 0.0f);
        nodes.push_back (std::move (node));
    }

    // cables
    int highestAux = 0;
    for (std::size_t w = 0; w < doc.wires.size(); ++w)
    {
        const auto& src = doc.wires[w];
        const auto validNode = [count] (int n) { return n >= 0 && n < count; };
        if (src.kind == PatchWire::Kind::audio)
        {
            AudioWire a;
            a.docIndex = (int) w;
            a.from = src.from;
            a.to = src.to;
            a.toBus = std::max (0, src.toPort);
            if (src.from == patchFromHost)
                a.fromBuffer = src.fromPort == 1 ? 1 : 0;
            else if (validNode (src.from))
            {
                if (src.fromPort > 0)
                {
                    const auto& heads = nodes[(std::size_t) src.from]->heads;
                    const auto h = std::find_if (heads.begin(), heads.end(), [&] (const Node::Head& x) { return x.bus == src.fromPort; });
                    if (h == heads.end())
                        continue; // an output the node does not have: silence
                    a.fromBuffer = 1 + (int) (h - heads.begin());
                }
            }
            else
                continue;
            if (src.to == patchToHost)
                highestAux = std::max (highestAux, a.toBus);
            else if (! validNode (src.to) || nodes[(std::size_t) src.to]->generator || a.toBus > 1)
                continue;
            wires.push_back (std::move (a));
        }
        else if (src.kind == PatchWire::Kind::midi)
        {
            if (src.from == patchFromHost && src.fromPort == 0 && validNode (src.to) && src.enabled
                && std::find (noteTargets.begin(), noteTargets.end(), src.to) == noteTargets.end())
                noteTargets.push_back (src.to);
        }
        else if ((validNode (src.from) || (src.from == patchSurface && std::max (0, src.fromPort) < doc.surfaceOutputs())) && validNode (src.to)
                 && src.enabled && src.toPort >= 0
                 && (src.from == patchSurface || std::max (0, src.fromPort) < nodePorts (doc.nodes[(std::size_t) src.from]).controlOuts)) // (a source with that output)
        {
            ControlWire c;
            c.from = src.from;
            c.output = std::max (0, src.fromPort);
            c.to = src.to;
            c.param = src.toPort;
            c.curve = src.curve;
            c.docIndex = (int) w;
            if (! nodes[(std::size_t) c.to]->plugin && ! nodes[(std::size_t) c.to]->generator && c.param >= maxEffectParams)
                continue;
            controls.push_back (std::move (c));
        }
    }
    highestAux = std::max (highestAux, (int) doc.outputTracks.size());

    // MIDI (round 162): what each "From LYNX" pin reaches, through MIDI Out nodes (they set the MIDI channel); VFX
    // nodes (round 164b) take the voices into their processors and send theirs on from their voice outputs
    pinTargets.assign ((std::size_t) patchMidiPortPin (patchMidiPorts), {});
    for (int pin = 0; pin < (int) pinTargets.size(); ++pin)
        pinTargets[(std::size_t) pin] = midiTargetsOf (doc, patchFromHost, pin);
    vfxQueues.resize ((std::size_t) count);
    for (int i = 0; i < count; ++i)
    {
        auto& node = *nodes[(std::size_t) i];
        if (node.instrument == nullptr || (node.vfx = node.instrument->noteProcessor()) == nullptr)
            continue;
        node.vfx->prepare (rate);
        const auto kind = node.vfx->kind();
        for (int k = 0; k < vfx::outputCount (kind); ++k)
            node.vfxTargets.push_back (midiTargetsOf (doc, i, k));
        vfxQueues[(std::size_t) i] = std::make_unique<VfxQueue>();
        hasVfx = true;
    }
    if (hasVfx)
    {
        // sources first (Kahn over the VFX → VFX links); a loop runs in index order (its voices wait a piece)
        std::vector<int> pendingInputs ((std::size_t) count, 0);
        for (int i = 0; i < count; ++i)
            for (const auto& targets : nodes[(std::size_t) i]->vfxTargets)
                for (const auto& t : targets)
                    if (t.vfx && t.node != i)
                        ++pendingInputs[(std::size_t) t.node];
        std::vector<char> placed ((std::size_t) count, 0);
        for (bool progress = true; progress;)
        {
            progress = false;
            for (int i = 0; i < count; ++i)
                if (! placed[(std::size_t) i] && nodes[(std::size_t) i]->vfx != nullptr && pendingInputs[(std::size_t) i] == 0)
                {
                    placed[(std::size_t) i] = 1;
                    vfxOrder.push_back (i);
                    progress = true;
                    for (const auto& targets : nodes[(std::size_t) i]->vfxTargets)
                        for (const auto& t : targets)
                            if (t.vfx && t.node != i)
                                --pendingInputs[(std::size_t) t.node];
                }
        }
        for (int i = 0; i < count; ++i)
            if (! placed[(std::size_t) i] && nodes[(std::size_t) i]->vfx != nullptr)
                vfxOrder.push_back (i);
    }
    // hosted plugin nodes with a cable on their MIDI output (round 164f)
    for (int i = 0; i < count; ++i)
    {
        auto& node = *nodes[(std::size_t) i];
        if (! node.generator || node.instrument == nullptr || node.vfx != nullptr || doc.nodes[(std::size_t) i].channel.instrument != InstrumentKind::plugin)
            continue;
        node.midiOutTargets = midiTargetsOf (doc, i, 0);
        if (! node.midiOutTargets.empty())
            midiSources.push_back (i);
    }
    noteOuts.reserve (vfx::EventList::capacity);

    published = doc.published;
    publishedValues.assign (published.size(), std::numeric_limits<float>::quiet_NaN());
    aux.resize ((std::size_t) highestAux);
    for (auto& a : aux)
        for (auto& side : a)
            side.assign ((std::size_t) capacity, 0.0f);

    // order: nodes once all their audio sources ran (Kahn); what is left (loops) in index order
    std::vector<int> pendingInputs ((std::size_t) count, 0);
    for (const auto& w : wires)
        if (w.from >= 0 && w.to >= 0 && w.from != w.to)
            ++pendingInputs[(std::size_t) w.to];
    std::vector<char> placed ((std::size_t) count, 0);
    for (bool progress = true; progress;)
    {
        progress = false;
        for (int i = 0; i < count; ++i)
            if (! placed[(std::size_t) i] && pendingInputs[(std::size_t) i] == 0)
            {
                placed[(std::size_t) i] = 1;
                order.push_back (i);
                progress = true;
                for (const auto& w : wires)
                    if (w.from == i && w.to >= 0 && w.to != i)
                        --pendingInputs[(std::size_t) w.to];
            }
    }
    for (int i = 0; i < count; ++i)
        if (! placed[(std::size_t) i])
            order.push_back (i);

    for (std::size_t k = 0; k < wires.size(); ++k)
        if (wires[k].to >= 0)
        {
            auto& node = *nodes[(std::size_t) wires[k].to];
            (wires[k].toBus == 0 ? node.mainInputs : node.sideInputs).push_back ((int) k);
            node.sidechain = node.sidechain || wires[k].toBus == 1;
        }

    // delay compensation (FL: a cable from a source with less latency than the destination's goes through a delay)
    std::vector<int> position ((std::size_t) count, 0);
    for (std::size_t k = 0; k < order.size(); ++k)
        position[(std::size_t) order[k]] = (int) k;
    auto runsBefore = [&] (const AudioWire& w) { return w.from < 0 || w.to < 0 || position[(std::size_t) w.from] < position[(std::size_t) w.to]; };
    auto sourceLatency = [&] (const AudioWire& w) { return w.from >= 0 ? nodes[(std::size_t) w.from]->outLatency : 0; };
    for (const auto i : order)
    {
        auto& node = *nodes[(std::size_t) i];
        int in = 0;
        for (const auto k : node.mainInputs)
            if (runsBefore (wires[(std::size_t) k]))
                in = std::max (in, sourceLatency (wires[(std::size_t) k]));
        for (const auto k : node.sideInputs)
            if (runsBefore (wires[(std::size_t) k]))
                in = std::max (in, sourceLatency (wires[(std::size_t) k]));
        node.inLatency = in;
        const auto own = node.generator ? (node.instrument != nullptr ? node.instrument->latencySamples() : 0)
                                        : (doc.nodes[(std::size_t) i].effect.enabled ? node.effect->latencySamples() : 0);
        node.outLatency = in + std::max (0, own);
    }
    std::vector<int> outLatency ((std::size_t) highestAux + 1, 0);
    for (const auto& w : wires)
        if (w.to == patchToHost)
            outLatency[(std::size_t) w.toBus] = std::max (outLatency[(std::size_t) w.toBus], sourceLatency (w));
    latency = outLatency.empty() ? 0 : *std::max_element (outLatency.begin(), outLatency.end());
    for (auto& w : wires)
    {
        if (! runsBefore (w))
            continue;
        const auto target = w.to >= 0 ? nodes[(std::size_t) w.to]->inLatency : latency; // every output aligned to the longest
        const auto difference = target - sourceLatency (w);
        if (difference > 0)
            w.delay = std::make_shared<DelayLine> (difference);
    }

    outputs.resize (aux.size() + 1);
    for (std::size_t k = 0; k < wires.size(); ++k)
        if (wires[k].to == patchToHost && wires[k].toBus < (int) outputs.size())
            outputs[(std::size_t) wires[k].toBus].push_back ((int) k);

    for (auto& s : scratch)
        s.assign ((std::size_t) capacity, 0.0f);
    zeros.assign ((std::size_t) capacity, 0.0f);
    active = std::make_unique<Values>();
    takeValues (doc, 140.0);
    applyPending();
}

Graph::~Graph()
{
    delete pending.exchange (nullptr);
    delete retired.exchange (nullptr);
}

void Graph::takeValues (const PatcherDoc& doc, double bpm)
{
    auto values = std::make_unique<Values>();
    values->nodes.resize (nodes.size());
    for (std::size_t i = 0; i < nodes.size() && i < doc.nodes.size(); ++i)
    {
        const auto& src = doc.nodes[i];
        auto& v = values->nodes[i];
        if (! src.generator)
        {
            v.params = src.effect.params;
            v.enabled = src.effect.enabled;
            v.mix = src.effect.mix;
        }
        for (const auto& h : nodes[i]->heads)
        {
            const auto head = busHeadOf (src, h.bus);
            v.heads.push_back (head && head->head ? head->head->params : h.slot.params);
            v.headGains.push_back (head ? head->gain : h.gain);
        }
        if (src.generator && nodes[i]->instrument != nullptr)
            nodes[i]->instrument->configure (src.channel, bpm);
    }
    for (const auto& w : wires)
    {
        const auto& src = doc.wires[(std::size_t) w.docIndex];
        values->gains.push_back (std::isfinite (src.gain) ? src.gain : 1.0f);
        values->on.push_back (src.enabled ? 1 : 0);
    }
    for (int k = 0; k < doc.surfaceOutputs(); ++k)
        values->surface.push_back (std::clamp (doc.surfaceOutput (k), 0.0f, 1.0f));
    delete retired.exchange (nullptr);
    delete pending.exchange (values.release());
}

void Graph::applyPending() noexcept
{
    if (auto* incoming = pending.exchange (nullptr))
    {
        auto* old = active.release();
        active.reset (incoming);
        delete retired.exchange (old); // (normally null: the UI frees it on its next call)
    }
}

void Graph::prepare (double sampleRate) noexcept
{
    if (sampleRate == rate)
        return;
    rate = sampleRate;
    for (auto& n : nodes)
    {
        if (n->effect != nullptr)
            n->effect->prepare (sampleRate);
        if (n->instrument != nullptr)
            n->instrument->prepare (sampleRate);
        for (auto& h : n->heads)
            if (h.effect != nullptr)
                h.effect->prepare (sampleRate);
    }
}

void Graph::reset() noexcept
{
    for (auto& n : nodes)
    {
        if (n->effect != nullptr)
            n->effect->reset();
        for (auto& h : n->heads)
            if (h.effect != nullptr)
                h.effect->reset();
        for (auto& o : n->out)
            for (auto& side : o)
                std::fill (side.begin(), side.end(), 0.0f);
    }
}

void Graph::setSongPosition (double position) noexcept
{
    ticks = position;
    playing = hostTimeline().playing.load (std::memory_order_relaxed);
    for (auto& n : nodes)
    {
        if (n->effect != nullptr)
            n->effect->setSongPosition (position);
        if (n->instrument != nullptr)
            n->instrument->setSongPosition (position);
        for (auto& h : n->heads)
            if (h.effect != nullptr)
                h.effect->setSongPosition (position);
    }
}

void Graph::setPlaying (bool on) noexcept
{
    playing = on;
    for (auto& n : nodes)
        if (n->effect != nullptr)
            n->effect->setPlaying (on);
}

void Graph::setHeldNotes (const NoteMask* channels, int count) noexcept
{
    for (auto& n : nodes)
        if (n->effect != nullptr)
            n->effect->setHeldNotes (channels, count);
}

void Graph::noteOn (int key, float velocity) noexcept { deliver (0, true, key, velocity, -1); }
void Graph::noteOff (int key) noexcept { deliver (0, false, key, 0.0f, -1); }
void Graph::noteOnMidi (int key, float velocity, int midiChannel) noexcept { deliver (0, true, key, velocity, midiChannel); }
void Graph::noteOffMidi (int key, int midiChannel) noexcept { deliver (0, false, key, 0.0f, midiChannel); }

std::vector<Graph::MidiTarget> Graph::midiTargetsOf (const PatcherDoc& doc, int from, int pin) const
{
    std::vector<MidiTarget> targets;
    const auto count = (int) doc.nodes.size();
    std::vector<char> seen ((std::size_t) count, 0);
    // (source, its pin, the channel set on the way)
    std::vector<std::tuple<int, int, int>> todo { { from, pin, -1 } };
    while (! todo.empty())
    {
        const auto [source, sourcePin, channel] = todo.back();
        todo.pop_back();
        for (const auto& w : doc.wires)
        {
            if (w.kind != PatchWire::Kind::midi || ! w.enabled || w.from != source || w.fromPort != sourcePin || w.to < 0 || w.to >= count
                || seen[(std::size_t) w.to])
                continue;
            seen[(std::size_t) w.to] = 1;
            const auto& target = doc.nodes[(std::size_t) w.to];
            if (! target.generator)
                continue;
            if (target.channel.instrument == InstrumentKind::midiOut)
                todo.emplace_back (w.to, 0, midictl::parseMidiOut (target.channel.pluginState).channel);
            else
                targets.push_back ({ w.to, channel, target.channel.instrument == InstrumentKind::vfx });
        }
    }
    return targets;
}

void Graph::deliver (int pin, bool on, int key, float velocity, int midiChannel) noexcept
{
    if (pin < 0 || pin >= (int) pinTargets.size())
        return;
    for (const auto& t : pinTargets[(std::size_t) pin])
    {
        if (t.vfx)
        {
            // into the VFX node's processor at the next piece's start, with the note's properties
            if (auto& q = vfxQueues[(std::size_t) t.node])
            {
                vfx::Event e;
                e.type = on ? vfx::Event::Type::on : vfx::Event::Type::off;
                e.voice.key = key;
                e.voice.velocity = velocity;
                e.voice.pan = nextVoice.pan;
                e.voice.cents = nextVoice.cents;
                e.voice.modX = nextVoice.modX;
                e.voice.modY = nextVoice.modY;
                e.voice.release = nextVoice.release;
                e.voice.porta = nextVoice.porta;
                const auto channel = t.midiChannel >= 0 ? t.midiChannel : midiChannel;
                e.voice.colour = channel >= 0 ? (channel & 15) : nextVoice.colour;
                e.id = vfx::inputId (key, e.voice.colour);
                q->in.add (e);
            }
            continue;
        }
        if (auto& i = nodes[(std::size_t) t.node]->instrument)
        {
            const auto channel = t.midiChannel >= 0 ? t.midiChannel : midiChannel;
            if (channel < 0)
                on ? i->noteOn (key, velocity) : i->noteOff (key);
            else
                on ? i->noteOnMidi (key, velocity, channel) : i->noteOffMidi (key, channel);
        }
    }
    if (! on)
        nextVoice.release = 0.5f;
    else
        nextVoice = {};
}

void Graph::noteOnPort (int port, int key, float velocity, int midiChannel) noexcept
{
    if (port >= 0 && port < patchMidiPorts)
        deliver (patchMidiPortPin (port), true, key, velocity, midiChannel);
}

void Graph::noteOffPort (int port, int key, int midiChannel) noexcept
{
    if (port >= 0 && port < patchMidiPorts)
        deliver (patchMidiPortPin (port), false, key, 0.0f, midiChannel);
}

void Graph::queueTimed (int pin, int key, float velocity, int midiChannel, int offset) noexcept
{
    if (pin < 0 || pin >= (int) pinTargets.size() || pinTargets[(std::size_t) pin].empty() || timedCount >= timedCapacity)
        return;
    timed[(std::size_t) timedCount++] = { pin, key, midiChannel, std::max (0, offset), velocity };
}

void Graph::queueNoteOnPort (int port, int key, float velocity, int midiChannel, int offset) noexcept
{
    if (port >= 0 && port < patchMidiPorts)
        queueTimed (patchMidiPortPin (port), key, std::max (velocity, 1.0e-6f), midiChannel, offset);
}

void Graph::queueNoteOffPort (int port, int key, int midiChannel, int offset) noexcept
{
    if (port >= 0 && port < patchMidiPorts)
        queueTimed (patchMidiPortPin (port), key, 0.0f, midiChannel, offset);
}

void Graph::routeNote (const std::vector<MidiTarget>& targets, int sourceNode, bool on, int key, float velocity, int midiChannel, int offset) noexcept
{
    for (const auto& t : targets)
    {
        const auto channel = t.midiChannel >= 0 ? t.midiChannel : midiChannel;
        vfx::Event e;
        e.type = on ? vfx::Event::Type::on : vfx::Event::Type::off;
        e.offset = offset;
        e.voice.key = key;
        e.voice.velocity = velocity;
        e.voice.colour = channel >= 0 ? (channel & 15) : 0;
        e.id = vfx::inputId (key, e.voice.colour);
        if (sourceNode >= 0)
            e.id += (sourceNode + 1) * (1 << 20); // (ids of different sources never meet)
        if (t.vfx)
        {
            if (auto& q = vfxQueues[(std::size_t) t.node])
                q->in.add (e);
        }
        else if (noteOuts.size() < noteOuts.capacity())
            noteOuts.push_back ({ t.node, channel, e });
    }
}

template <typename F>
void Graph::forNoteTargets (F&& f) noexcept
{
    if (pinTargets.empty())
        return;
    for (const auto& t : pinTargets[0])
        if (auto& i = nodes[(std::size_t) t.node]->instrument)
            f (*i);
}

void Graph::midiMessage (const std::uint8_t* bytes, int size) noexcept
{
    forNoteTargets ([&] (Instrument& i) { i.midiMessage (bytes, size); });
}
void Graph::pitchBend (float semitones) noexcept
{
    forNoteTargets ([&] (Instrument& i) { i.pitchBend (semitones); });
}
void Graph::controlChange (int cc, float value) noexcept
{
    forNoteTargets ([&] (Instrument& i) { i.controlChange (cc, value); });
}

bool Graph::takesMidiPort (int port) const noexcept
{
    return port >= 0 && port < patchMidiPorts && ! pinTargets[(std::size_t) patchMidiPortPin (port)].empty();
}

void Graph::allNotesOff() noexcept
{
    timedCount = 0;
    // the VFX nodes' voices end (their instruments stop below anyway); their queues empty
    for (const auto i : vfxOrder)
    {
        auto& q = *vfxQueues[(std::size_t) i];
        q.in.clear();
        q.out.clear();
        nodes[(std::size_t) i]->vfx->releaseAll (q.out);
        q.out.clear();
    }
    for (auto& n : nodes)
        if (n->instrument != nullptr)
            n->instrument->allNotesOff();
}

void Graph::slideTo (int key, float cents, float samples) noexcept
{
    for (const auto t : noteTargets)
        if (auto& i = nodes[(std::size_t) t]->instrument)
            i->slideTo (key, cents, samples);
}

void Graph::setNextNoteModulation (float modX, float modY) noexcept
{
    nextVoice.modX = modX;
    nextVoice.modY = modY;
}

void Graph::setNextNoteRelease (float velocity) noexcept { nextVoice.release = velocity; }
void Graph::setNextNoteColour (int colour) noexcept { nextVoice.colour = colour & 15; }

void Graph::setNextNoteExpression (float pan, float cents, bool porta) noexcept
{
    nextVoice.pan = pan;
    nextVoice.cents = cents;
    nextVoice.porta = porta;
    for (const auto t : noteTargets)
        if (auto& i = nodes[(std::size_t) t]->instrument)
            i->setNextNoteExpression (pan, cents, porta);
}

bool Graph::hasInstruments() const noexcept
{
    return std::any_of (nodes.begin(), nodes.end(), [] (const auto& n) { return n->instrument != nullptr; });
}

bool Graph::instrumentsIdle() const noexcept
{
    // (a VFX node that holds voices or plays on its own — a sequencer — keeps the graph running)
    return std::all_of (nodes.begin(), nodes.end(), [] (const auto& n) {
        return (n->instrument == nullptr || n->instrument->idle()) && (n->vfx == nullptr || ! n->vfx->busy());
    });
}

float Graph::controlOutput (int) const noexcept { return 0.0f; }

void Graph::setPublished (int k, float v) noexcept
{
    if (k < 0 || k >= (int) published.size())
        return;
    const auto& p = published[(std::size_t) k];
    if (p.node < 0 || p.node >= (int) nodes.size() || p.param < 0)
        return;
    const auto value = mapControl (p.curve, v);
    auto& node = *nodes[(std::size_t) p.node];
    if (node.generator)
    {
        if (node.instrument != nullptr)
            node.instrument->setParameter (p.param, value);
    }
    else if (node.plugin)
        node.effect->setParameter (p.param, value);
    else if (p.param < maxEffectParams)
        publishedValues[(std::size_t) k] = value; // (applied at the start of each block, over the UI's values)
}

const float* Graph::auxOutput (int index, int side) const noexcept
{
    return index >= 0 && index < (int) aux.size() ? aux[(std::size_t) index][(std::size_t) (side & 1)].data() : nullptr;
}

std::shared_ptr<Effect> Graph::effectAt (int node) const
{
    return node >= 0 && node < (int) nodes.size() ? nodes[(std::size_t) node]->effect : nullptr;
}

std::shared_ptr<Instrument> Graph::instrumentAt (int node) const
{
    return node >= 0 && node < (int) nodes.size() ? nodes[(std::size_t) node]->instrument : nullptr;
}

void Graph::process (const float* inL, const float* inR, const float* scL, const float* scR, float* outL, float* outR, int n, double bpm) noexcept
{
    for (int done = 0; done < n; done += capacity)
    {
        const auto piece = std::min (capacity, n - done);
        processPiece (inL + done, inR + done, scL != nullptr ? scL + done : nullptr, scR != nullptr ? scR + done : nullptr, outL + done,
                      outR + done, piece, bpm);
    }
}

void Graph::processPiece (const float* inL, const float* inR, const float* scL, const float* scR, float* outL, float* outR, int n, double bpm) noexcept
{
    applyPending();
    auto& values = *active;
    for (std::size_t k = 0; k < publishedValues.size(); ++k)
        if (! std::isnan (publishedValues[k]))
            values.nodes[(std::size_t) published[k].node].params[(std::size_t) published[k].param] = publishedValues[k];

    // control cables: the source's latest output moves the target's parameter (one block of lag, as the mixer's links)
    for (const auto& c : controls)
    {
        auto raw = 0.0f;
        if (c.from == patchSurface) // the Surface tab's control (round 151)
            raw = c.output < (int) values.surface.size() ? values.surface[(std::size_t) c.output] : 0.0f;
        else
        {
            const auto& source = *nodes[(std::size_t) c.from];
            raw = source.effect != nullptr && ! source.generator ? source.effect->controlOutput (c.output)
                  : source.instrument != nullptr                 ? source.instrument->controlOutput (c.output)
                                                                 : 0.0f;
        }
        const auto v = mapControl (c.curve, raw);
        auto& target = *nodes[(std::size_t) c.to];
        if (target.generator)
        {
            if (target.instrument != nullptr)
                target.instrument->setParameter (c.param, v);
        }
        else if (target.plugin)
            target.effect->setParameter (c.param, v);
        else
            values.nodes[(std::size_t) c.to].params[(std::size_t) c.param] = v;
    }

    noteOuts.clear();
    // notes queued at their sample (the rack's notes into a mixer-slot Patcher, round 164f): the ones in this piece
    if (timedCount > 0)
    {
        int kept = 0;
        for (int k = 0; k < timedCount; ++k)
        {
            auto t = timed[(std::size_t) k];
            if (t.offset < n)
                routeNote (pinTargets[(std::size_t) t.pin], -1, t.velocity > 0.0f, t.key, t.velocity, t.midiChannel, t.offset);
            else
            {
                t.offset -= n;
                timed[(std::size_t) kept++] = t;
            }
        }
        timedCount = kept;
    }
    // what the hosted plugin nodes sent out in the last piece (round 164f), at this piece's start
    for (const auto i : midiSources)
    {
        std::array<Instrument::MidiOutNote, 128> sent;
        auto& node = *nodes[(std::size_t) i];
        const auto count = node.instrument->takeMidiOutput (sent.data(), (int) sent.size());
        for (int k = 0; k < count; ++k)
            routeNote (node.midiOutTargets, i, sent[(std::size_t) k].velocity > 0.0f, sent[(std::size_t) k].key, sent[(std::size_t) k].velocity, -1, 0);
    }
    if (hasVfx)
        runVfx (n, bpm);
    if (noteOuts.empty())
    {
        renderNodes (inL, inR, scL, scR, outL, outR, 0, n, bpm);
        return;
    }
    // what the VFX nodes, the queued notes and the plugins play lands at its sample: the piece is rendered in parts
    // between those samples
    sortNoteOuts();
    std::size_t next = 0;
    for (int position = 0; position < n;)
    {
        while (next < noteOuts.size() && noteOuts[next].event.offset <= position)
            playNoteOut (noteOuts[next++]);
        const auto end = next < noteOuts.size() ? std::min (n, noteOuts[next].event.offset) : n;
        if (end > position)
            renderNodes (inL + position, inR + position, scL != nullptr ? scL + position : nullptr, scR != nullptr ? scR + position : nullptr,
                         outL + position, outR + position, position, end - position, bpm);
        position = end;
    }
    while (next < noteOuts.size())
        playNoteOut (noteOuts[next++]);
}

void Graph::runVfx (int n, double bpm) noexcept
{
    vfx::Transport t;
    t.sampleRate = rate;
    t.bpm = bpm;
    t.ticks = ticks;
    t.playing = playing;
    for (const auto i : vfxOrder)
    {
        auto& q = *vfxQueues[(std::size_t) i];
        q.in.sortByOffset();
        q.out.clear();
        nodes[(std::size_t) i]->vfx->process (&q.in[0], q.in.size(), n, t, q.out);
        q.in.clear();
        for (int k = 0; k < q.out.size(); ++k)
            routeVfxOutput (i, q.out[k]);
        q.out.clear();
    }
    ticks += (double) n * bpm * (double) ppq / (60.0 * rate);
}

void Graph::sortNoteOuts() noexcept
{
    // (stable insertion sort in place: std::stable_sort may allocate on the audio thread)
    for (std::size_t i = 1; i < noteOuts.size(); ++i)
    {
        const auto o = noteOuts[i];
        auto j = i;
        for (; j > 0 && o.event.offset < noteOuts[j - 1].event.offset; --j)
            noteOuts[j] = noteOuts[j - 1];
        noteOuts[j] = o;
    }
}

void Graph::routeVfxOutput (int node, const vfx::Event& e) noexcept
{
    const auto& targets = nodes[(std::size_t) node]->vfxTargets;
    if (e.output < 0 || e.output >= (int) targets.size())
        return;
    for (const auto& t : targets[(std::size_t) e.output])
    {
        if (t.vfx)
        {
            if (auto& q = vfxQueues[(std::size_t) t.node])
            {
                auto in = e;
                in.output = 0;
                in.id = (node + 1) * (1 << 20) + (e.id & ((1 << 20) - 1)); // (ids of different sources never meet)
                if (t.midiChannel >= 0)
                    in.voice.colour = t.midiChannel & 15;
                q->in.add (in);
            }
        }
        else if (noteOuts.size() < noteOuts.capacity())
            noteOuts.push_back ({ t.node, t.midiChannel, e });
    }
}

void Graph::playNoteOut (const NoteOut& o) noexcept
{
    auto& i = nodes[(std::size_t) o.node]->instrument;
    if (i == nullptr)
        return;
    const auto& e = o.event;
    const auto& v = e.voice;
    const auto channel = o.midiChannel >= 0 ? o.midiChannel : v.colour;
    switch (e.type)
    {
        case vfx::Event::Type::on:
            i->setNextNoteExpression (v.pan, v.cents, v.porta);
            i->setNextNoteModulation (v.modX, v.modY);
            i->setNextNoteColour (v.colour);
            if (channel > 0)
                i->noteOnMidi (v.key, v.velocity, channel);
            else
                i->noteOn (v.key, v.velocity);
            break;
        case vfx::Event::Type::off:
            i->setNextNoteRelease (v.release);
            if (channel > 0)
                i->noteOffMidi (v.key, channel);
            else
                i->noteOff (v.key);
            break;
        case vfx::Event::Type::prop:
            i->voiceProperty (v.key, (int) e.prop, e.value);
            break;
    }
}

void Graph::renderNodes (const float* inL, const float* inR, const float* scL, const float* scR, float* outL, float* outR, int start, int n,
                         double bpm) noexcept
{
    // (inL…outR point at this part of the piece; the nodes' buffers are indexed from `start`)
    auto& values = *active;
    auto sourceOf = [&] (const AudioWire& w, int side) -> const float* {
        if (w.from == patchFromHost)
        {
            if (w.fromBuffer == 1)
                return (side == 0 ? scL : scR) != nullptr ? (side == 0 ? scL : scR) : zeros.data() + start;
            return side == 0 ? inL : inR;
        }
        return nodes[(std::size_t) w.from]->out[(std::size_t) w.fromBuffer][(std::size_t) side].data() + start;
    };
    auto gather = [&] (const std::vector<int>& list, float* l, float* r) {
        std::fill (l, l + n, 0.0f);
        std::fill (r, r + n, 0.0f);
        for (const auto k : list)
        {
            const auto& w = wires[(std::size_t) k];
            const auto on = values.on[(std::size_t) k] != 0;
            const auto gain = on ? values.gains[(std::size_t) k] : 0.0f;
            const auto* sl = sourceOf (w, 0);
            const auto* sr = sourceOf (w, 1);
            if (w.delay != nullptr)
            {
                w.delay->mixInto (on ? sl : nullptr, on ? sr : nullptr, gain, l, r, n); // (keeps its line running)
                continue;
            }
            if (! on || gain == 0.0f)
                continue;
            for (int i = 0; i < n; ++i)
            {
                l[i] += sl[i] * gain;
                r[i] += sr[i] * gain;
            }
        }
    };

    auto* inBufL = scratch[0].data();
    auto* inBufR = scratch[1].data();
    auto* scBufL = scratch[2].data();
    auto* scBufR = scratch[3].data();

    for (const auto i : order)
    {
        auto& node = *nodes[(std::size_t) i];
        auto& v = values.nodes[(std::size_t) i];
        auto* oL = node.out[0][0].data() + start;
        auto* oR = node.out[0][1].data() + start;

        if (node.generator)
        {
            std::fill (oL, oL + n, 0.0f);
            std::fill (oR, oR + n, 0.0f);
            if (node.instrument != nullptr)
                node.instrument->render (oL, oR, n, 1.0f, 1.0f);
            continue;
        }

        gather (node.mainInputs, inBufL, inBufR);
        std::copy (inBufL, inBufL + n, oL);
        std::copy (inBufR, inBufR + n, oR);
        if (v.enabled && v.mix > 0.0f)
        {
            if (node.sidechain)
            {
                gather (node.sideInputs, scBufL, scBufR);
                node.effect->setSidechain (scBufL, scBufR);
            }
            node.effect->process (oL, oR, n, v.params, bpm);
            if (v.mix < 0.999f)
                for (int k = 0; k < n; ++k)
                {
                    oL[k] = inBufL[k] + (oL[k] - inBufL[k]) * v.mix;
                    oR[k] = inBufR[k] + (oR[k] - inBufR[k]) * v.mix;
                }
        }
        for (std::size_t h = 0; h < node.heads.size(); ++h)
        {
            auto* hL = node.out[1 + h][0].data() + start;
            auto* hR = node.out[1 + h][1].data() + start;
            std::copy (inBufL, inBufL + n, hL);
            std::copy (inBufR, inBufR + n, hR);
            if (node.heads[h].effect != nullptr)
                node.heads[h].effect->process (hL, hR, n, v.heads[h], bpm);
            const auto g = v.headGains[h];
            if (g != 1.0f)
                for (int k = 0; k < n; ++k)
                {
                    hL[k] *= g;
                    hR[k] *= g;
                }
        }
    }

    // "To LYNX": the main output and the extra ones
    gather (outputs[0], inBufL, inBufR);
    std::copy (inBufL, inBufL + n, outL);
    std::copy (inBufR, inBufR + n, outR);
    for (std::size_t bus = 1; bus < outputs.size(); ++bus)
        gather (outputs[bus], aux[bus - 1][0].data() + start, aux[bus - 1][1].data() + start);
}

// ---------------------------------------------------------------------------------------------------------------

namespace
{
class PatcherEffect final : public Effect
{
public:
    PatcherEffect (const EffectSlot& slot, double sampleRate, const Factories& factories, const Graph* previous)
        : graph (slot.patcher, sampleRate, factories, previous)
    {
    }

    void prepare (double sampleRate) noexcept override { graph.prepare (sampleRate); }
    void reset() noexcept override { graph.reset(); }
    void process (float* l, float* r, int n, const EffectParams&, double bpm) noexcept override
    {
        for (int done = 0; done < n; done += Graph::capacity)
        {
            const auto piece = std::min (Graph::capacity, n - done);
            graph.process (l + done, r + done, side ? sideL + done : nullptr, side ? sideR + done : nullptr, l + done, r + done, piece, bpm);
            // the first extra output goes to its insert like a Fruity Send (the rack pulls it from there)
            if (sendL != nullptr && sendR != nullptr && graph.auxOutputs() > 0)
            {
                const auto* al = graph.auxOutput (0, 0);
                const auto* ar = graph.auxOutput (0, 1);
                for (int i = 0; i < piece; ++i)
                {
                    sendL[done + i] += al[i];
                    sendR[done + i] += ar[i];
                }
            }
        }
        side = false;
    }
    void setSendOutput (float* l, float* r) noexcept override
    {
        sendL = l;
        sendR = r;
    }
    void setParameter (int index, float value) noexcept override { graph.setPublished (index, value); }
    void setSongPosition (double ticks) noexcept override { graph.setSongPosition (ticks); }
    void setPlaying (bool playing) noexcept override { graph.setPlaying (playing); }
    void setSidechain (const float* l, const float* r) noexcept override
    {
        sideL = l;
        sideR = r;
        side = l != nullptr && r != nullptr;
    }
    void setHeldNotes (const NoteMask* channels, int count) noexcept override { graph.setHeldNotes (channels, count); }
    int latencySamples() const noexcept override { return graph.latencySamples(); }

    Graph graph;

private:
    const float* sideL = nullptr;
    const float* sideR = nullptr;
    bool side = false;
    float* sendL = nullptr;
    float* sendR = nullptr;
};

class PatcherInstrument final : public Instrument
{
public:
    PatcherInstrument (const ChannelDef& channel, double sampleRate, const Factories& factories, const Graph* previous)
        : graph (channel.patcher, sampleRate, factories, previous)
    {
        for (auto& s : buffer)
            s.assign ((std::size_t) Graph::capacity, 0.0f);
        accumulated.resize ((std::size_t) graph.auxOutputs());
        tracks = std::make_unique<std::atomic<int>[]> ((std::size_t) std::max (1, graph.auxOutputs()));
        for (auto& a : accumulated)
            for (auto& side : a)
                side.assign (auxCapacity, 0.0f);
        configure (channel, 140.0);
    }

    void prepare (double sampleRate) noexcept override { graph.prepare (sampleRate); }
    void noteOn (int key, float velocity) noexcept override { graph.noteOn (key, velocity); }
    void noteOff (int key) noexcept override { graph.noteOff (key); }
    void allNotesOff() noexcept override { graph.allNotesOff(); }
    void midiMessage (const std::uint8_t* bytes, int size) noexcept override { graph.midiMessage (bytes, size); }
    void pitchBend (float semitones) noexcept override { graph.pitchBend (semitones); }
    void controlChange (int cc, float value) noexcept override { graph.controlChange (cc, value); }
    void noteOnMidi (int key, float velocity, int midiChannel) noexcept override { graph.noteOnMidi (key, velocity, midiChannel); }
    void noteOffMidi (int key, int midiChannel) noexcept override { graph.noteOffMidi (key, midiChannel); }
    void noteOnPort (int port, int key, float velocity, int midiChannel) noexcept override { graph.noteOnPort (port, key, velocity, midiChannel); }
    void noteOffPort (int port, int key, int midiChannel) noexcept override { graph.noteOffPort (port, key, midiChannel); }
    void slideTo (int key, float cents, float samples) noexcept override { graph.slideTo (key, cents, samples); }
    void setNextNoteExpression (float pan, float cents, bool porta) noexcept override { graph.setNextNoteExpression (pan, cents, porta); }
    void setNextNoteModulation (float modX, float modY) noexcept override { graph.setNextNoteModulation (modX, modY); }
    void setNextNoteRelease (float velocity) noexcept override { graph.setNextNoteRelease (velocity); }
    void setNextNoteColour (int colour) noexcept override { graph.setNextNoteColour (colour); }
    void setSongPosition (double ticks) noexcept override { graph.setSongPosition (ticks); }
    void setParameter (int index, float value) noexcept override { graph.setPublished (index, value); }
    int latencySamples() const noexcept override { return graph.latencySamples(); }
    bool idle() const noexcept override { return idleWhenQuiet.load (std::memory_order_relaxed) && graph.instrumentsIdle(); }

    // (UI thread, while the audio thread renders: only atomics change here; the graph swaps its values itself)
    void configure (const ChannelDef& channel, double bpm) noexcept override
    {
        tempo.store (bpm, std::memory_order_relaxed);
        graph.takeValues (channel.patcher, bpm);
        if (tracks != nullptr)
            for (int k = 1; k <= graph.auxOutputs(); ++k)
                tracks[(std::size_t) k - 1].store (patcher::auxOutputTrack (channel, k), std::memory_order_relaxed);
        // only instruments (no effects that ring on): silent when its instruments are
        idleWhenQuiet.store (std::all_of (channel.patcher.nodes.begin(), channel.patcher.nodes.end(), [] (const PatchNode& n) { return n.generator; }),
                             std::memory_order_relaxed);
    }

    void render (float* left, float* right, int n, float gainLeft, float gainRight) noexcept override
    {
        static const std::vector<float> silence ((std::size_t) Graph::capacity, 0.0f);
        for (int done = 0; done < n; done += Graph::capacity)
        {
            const auto piece = std::min (Graph::capacity, n - done);
            graph.process (silence.data(), silence.data(), nullptr, nullptr, buffer[0].data(), buffer[1].data(), piece,
                          tempo.load (std::memory_order_relaxed));
            for (int i = 0; i < piece; ++i)
            {
                left[done + i] += buffer[0][(std::size_t) i] * gainLeft;
                right[done + i] += buffer[1][(std::size_t) i] * gainRight;
            }
            for (int k = 0; k < graph.auxOutputs(); ++k)
            {
                const auto* al = graph.auxOutput (k, 0);
                const auto* ar = graph.auxOutput (k, 1);
                auto& acc = accumulated[(std::size_t) k];
                for (int i = 0; i < piece && cursor + i < (int) auxCapacity; ++i)
                {
                    acc[0][(std::size_t) (cursor + i)] += al[i] * gainLeft;
                    acc[1][(std::size_t) (cursor + i)] += ar[i] * gainRight;
                }
            }
            cursor += piece;
        }
    }

    void beginChunk (int n) noexcept override
    {
        cursor = 0;
        for (auto& a : accumulated)
            for (auto& side : a)
                std::fill (side.begin(), side.begin() + std::min<std::ptrdiff_t> (n, (std::ptrdiff_t) side.size()), 0.0f);
    }
    int auxOutputCount() const noexcept override { return (int) accumulated.size(); }
    int auxOutputTrack (int k) const noexcept override
    {
        return k >= 0 && k < graph.auxOutputs() && tracks != nullptr ? tracks[(std::size_t) k].load (std::memory_order_relaxed) : -1;
    }
    const float* auxOutputData (int k, int side) const noexcept override
    {
        return k >= 0 && k < (int) accumulated.size() ? accumulated[(std::size_t) k][(std::size_t) (side & 1)].data() : nullptr;
    }

    Graph graph;

private:
    static constexpr std::size_t auxCapacity = 16384;
    std::array<std::vector<float>, 2> buffer;
    std::vector<std::array<std::vector<float>, 2>> accumulated;
    std::unique_ptr<std::atomic<int>[]> tracks; // [extra output] its insert
    int cursor = 0;
    std::atomic<bool> idleWhenQuiet { true };
    std::atomic<double> tempo { 140.0 };
};
} // namespace

std::unique_ptr<Effect> makeEffect (const EffectSlot& slot, double sampleRate, const Factories& factories, const Effect* previous)
{
    const auto* old = dynamic_cast<const PatcherEffect*> (previous);
    return std::make_unique<PatcherEffect> (slot, sampleRate, factories, old != nullptr ? &old->graph : nullptr);
}

std::unique_ptr<Instrument> makeInstrument (const ChannelDef& channel, double sampleRate, const Factories& factories, const Instrument* previous)
{
    const auto* old = dynamic_cast<const PatcherInstrument*> (previous);
    return std::make_unique<PatcherInstrument> (channel, sampleRate, factories, old != nullptr ? &old->graph : nullptr);
}

Graph* graphOf (Effect* effect) noexcept
{
    auto* p = dynamic_cast<PatcherEffect*> (effect);
    return p != nullptr ? &p->graph : nullptr;
}

Graph* graphOf (Instrument* instrument) noexcept
{
    auto* p = dynamic_cast<PatcherInstrument*> (instrument);
    return p != nullptr ? &p->graph : nullptr;
}

bool acceptValues (Effect* effect, const EffectSlot& slot)
{
    if (auto* g = graphOf (effect))
    {
        g->takeValues (slot.patcher);
        return true;
    }
    return false;
}

bool acceptValues (Instrument* instrument, const ChannelDef& channel)
{
    if (auto* g = graphOf (instrument))
    {
        instrument->configure (channel, 140.0); // (the rack configures it again with the song's tempo)
        return true;
    }
    return false;
}

int auxOutputTrack (const ChannelDef& channel, int k) noexcept
{
    if (k <= 0)
        return channel.mixerTrack;
    if (k <= (int) channel.patcher.outputTracks.size() && channel.patcher.outputTracks[(std::size_t) k - 1] >= 0)
        return channel.patcher.outputTracks[(std::size_t) k - 1];
    return std::min (channel.mixerTrack + k, maxMixerTracks - 1);
}

Factories builtInFactories (double sampleRate, double bpm)
{
    Factories f;
    f.effect = [sampleRate] (const EffectSlot& slot) -> std::shared_ptr<Effect> {
        if (slot.kind == EffectKind::plugin)
            return nullptr;
        if (slot.kind == EffectKind::patcher)
            return makeEffect (slot, sampleRate, builtInFactories (sampleRate));
        std::shared_ptr<Effect> e = lynx::makeEffect (slot);
        e->prepare (sampleRate);
        return e;
    };
    f.instrument = [sampleRate, bpm] (const ChannelDef& channel) -> std::shared_ptr<Instrument> {
        if (channel.instrument == InstrumentKind::plugin)
            return nullptr;
        if (channel.instrument == InstrumentKind::patcher)
            return makeInstrument (channel, sampleRate, builtInFactories (sampleRate, bpm));
        std::shared_ptr<Instrument> i = lynx::makeInstrument (channel.instrument);
        i->prepare (sampleRate);
        i->configure (channel, bpm);
        return i;
    };
    return f;
}

int addNode (PatcherDoc& doc, PatchNode node)
{
    doc.nodes.push_back (std::move (node));
    return (int) doc.nodes.size() - 1;
}

void removeNode (PatcherDoc& doc, int node)
{
    if (node < 0 || node >= (int) doc.nodes.size())
        return;
    doc.nodes.erase (doc.nodes.begin() + node);
    std::erase_if (doc.wires, [node] (const PatchWire& w) { return w.from == node || w.to == node; });
    for (auto& w : doc.wires)
    {
        if (w.from > node)
            --w.from;
        if (w.to > node)
            --w.to;
    }
}

int addSurfaceControl (PatcherDoc& doc, SurfaceControl control)
{
    doc.surface.push_back (std::move (control));
    return (int) doc.surface.size() - 1;
}

void removeSurfaceControl (PatcherDoc& doc, int index)
{
    if (index < 0 || index >= (int) doc.surface.size())
        return;
    int first = 0;
    for (int i = 0; i < index; ++i)
        first += doc.surface[(std::size_t) i].outputs();
    const auto count = doc.surface[(std::size_t) index].outputs();
    doc.surface.erase (doc.surface.begin() + index);
    // its cables go; the later controls' outputs move down
    std::erase_if (doc.wires, [first, count] (const PatchWire& w) { return w.from == patchSurface && w.fromPort >= first && w.fromPort < first + count; });
    for (auto& w : doc.wires)
        if (w.from == patchSurface && w.fromPort >= first + count)
            w.fromPort -= count;
}

bool connect (PatcherDoc& doc, const PatchWire& wire)
{
    const auto count = (int) doc.nodes.size();
    const auto validFrom = wire.from == patchFromHost || (wire.from >= 0 && wire.from < count)
                           || (wire.from == patchSurface && wire.kind == PatchWire::Kind::control && wire.fromPort >= 0 && wire.fromPort < doc.surfaceOutputs());
    const auto validTo = wire.to == patchToHost || (wire.to >= 0 && wire.to < count);
    if (! validFrom || ! validTo || (wire.from == wire.to && wire.from >= 0 && wire.kind != PatchWire::Kind::control))
        return false;
    if (wire.kind == PatchWire::Kind::control && ((wire.from < 0 && wire.from != patchSurface) || wire.to < 0))
        return false;
    if (wire.to >= 0 && doc.nodes[(std::size_t) wire.to].generator && wire.kind == PatchWire::Kind::audio)
        return false;
    for (const auto& w : doc.wires)
        if (w.kind == wire.kind && w.from == wire.from && w.fromPort == wire.fromPort && w.to == wire.to && w.toPort == wire.toPort)
            return false;
    doc.wires.push_back (wire);
    return true;
}

std::string nodeName (const PatchNode& node)
{
    auto blank = [] (const std::string& t) { return std::all_of (t.begin(), t.end(), [] (unsigned char c) { return std::isspace (c) != 0; }); };
    if (! blank (node.label))
        return node.label;
    if (node.generator)
        return ! blank (node.channel.plugin.name) ? node.channel.plugin.name : ! blank (node.channel.name) ? node.channel.name : "Instrument";
    if (node.effect.kind == EffectKind::plugin)
        return ! blank (node.effect.plugin.name) ? node.effect.plugin.name : "Plugin";
    return effectInfo (node.effect.kind).name;
}
// --- MIDI through the I/O nodes (round 162) ---

namespace
{
void addSorted (std::vector<int>& v, int x)
{
    if (std::find (v.begin(), v.end(), x) == v.end())
        v.insert (std::upper_bound (v.begin(), v.end(), x), x);
}
} // namespace

std::vector<int> shownFromMidiPorts (const PatcherDoc& doc)
{
    std::vector<int> ports;
    for (const auto p : doc.fromMidiPorts)
        if (p >= 0 && p < patchMidiPorts)
            addSorted (ports, p);
    for (const auto& w : doc.wires)
        if (w.kind == PatchWire::Kind::midi && w.from == patchFromHost && w.fromPort > 0 && patchPinMidiPort (w.fromPort) < patchMidiPorts)
            addSorted (ports, patchPinMidiPort (w.fromPort));
    return ports;
}

std::vector<int> shownToMidiPorts (const PatcherDoc& doc)
{
    std::vector<int> ports;
    for (const auto p : doc.toMidiPorts)
        if (p >= 0 && p < patchMidiPorts)
            addSorted (ports, p);
    for (const auto& w : doc.wires)
        if (w.kind == PatchWire::Kind::midi && w.to == patchToHost && w.toPort >= 0 && w.toPort < patchMidiPorts)
            addSorted (ports, w.toPort);
    return ports;
}

int shownMidiOutputs (const PatcherDoc& doc, int node)
{
    if (node < 0 || node >= (int) doc.nodes.size())
        return 0;
    const auto ports = nodePorts (doc.nodes[(std::size_t) node]);
    const auto kind = vfx::kindOf (doc.nodes[(std::size_t) node].channel);
    if (! kind)
        return ports.midiOuts;
    int highest = -1;
    for (const auto& w : doc.wires)
        if (w.kind == PatchWire::Kind::midi && w.from == node)
            highest = std::max (highest, w.fromPort);
    return std::clamp (std::max (vfx::outputsShown (*kind), highest + 2), 1, ports.midiOuts);
}

bool fromNotesShown (const PatcherDoc& doc, bool generator)
{
    if (std::any_of (doc.wires.begin(), doc.wires.end(),
                     [] (const PatchWire& w) { return w.kind == PatchWire::Kind::midi && w.from == patchFromHost && w.fromPort == 0; }))
        return true;
    return generator && ! doc.fromNotesHidden; // (a mixer slot has no notes of its own)
}

std::vector<int> midiInputPorts (const PatcherDoc& doc)
{
    std::vector<int> ports;
    for (const auto& w : doc.wires)
        if (w.kind == PatchWire::Kind::midi && w.enabled && w.from == patchFromHost && w.fromPort > 0
            && patchPinMidiPort (w.fromPort) < patchMidiPorts)
            addSorted (ports, patchPinMidiPort (w.fromPort));
    return ports;
}

std::vector<MidiPortRoute> midiOutputsOf (const PatcherDoc& doc, int fromPin)
{
    std::vector<MidiPortRoute> out;
    const auto count = (int) doc.nodes.size();
    std::vector<char> seen ((std::size_t) count, 0);
    std::vector<std::tuple<int, int, int>> todo { { patchFromHost, fromPin, -1 } };
    while (! todo.empty())
    {
        const auto [from, pin, channel] = todo.back();
        todo.pop_back();
        for (const auto& w : doc.wires)
        {
            if (w.kind != PatchWire::Kind::midi || ! w.enabled || w.from != from || w.fromPort != pin)
                continue;
            if (w.to == patchToHost)
            {
                const MidiPortRoute r { w.toPort, channel };
                if (w.toPort >= 0 && w.toPort < patchMidiPorts && std::find (out.begin(), out.end(), r) == out.end())
                    out.push_back (r);
            }
            else if (w.to >= 0 && w.to < count && ! seen[(std::size_t) w.to])
            {
                const auto& node = doc.nodes[(std::size_t) w.to];
                if (node.generator && node.channel.instrument == InstrumentKind::midiOut)
                {
                    seen[(std::size_t) w.to] = 1;
                    todo.emplace_back (w.to, 0, midictl::parseMidiOut (node.channel.pluginState).channel);
                }
            }
        }
    }
    return out;
}

void deactivateUnusedMidiPins (PatcherDoc& doc, bool fromNode)
{
    auto used = [&] (int pin) {
        return std::any_of (doc.wires.begin(), doc.wires.end(), [&] (const PatchWire& w) {
            return w.kind == PatchWire::Kind::midi && (fromNode ? w.from == patchFromHost && w.fromPort == pin : w.to == patchToHost && w.toPort == pin);
        });
    };
    if (fromNode)
    {
        std::erase_if (doc.fromMidiPorts, [&] (int p) { return ! used (patchMidiPortPin (p)); });
        doc.fromNotesHidden = ! used (0);
    }
    else
        std::erase_if (doc.toMidiPorts, [&] (int p) { return ! used (p); });
}
} // namespace lynx::patcher
