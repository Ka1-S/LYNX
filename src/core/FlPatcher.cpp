#include "core/FlPatcher.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>

namespace lynx
{
namespace
{
template <typename T>
T at (const std::uint8_t* p, std::size_t offset)
{
    T v {};
    std::memcpy (&v, p + offset, sizeof (T));
    return v;
}

// chunks "u32 id + u64 size + data"; id 0 = group (the content is more chunks)
bool forEachChunk (const std::uint8_t* p, std::size_t size, const std::function<void (std::uint32_t, const std::uint8_t*, std::size_t)>& fn)
{
    std::size_t pos = 0;
    while (pos + 12 <= size)
    {
        const auto id = at<std::uint32_t> (p, pos);
        const auto length = at<std::uint64_t> (p, pos + 4);
        if (length > size - pos - 12)
            return false;
        fn (id, p + pos + 12, (std::size_t) length);
        pos += 12 + (std::size_t) length;
    }
    return pos == size;
}

std::string text (const std::uint8_t* p, std::size_t size, bool wide)
{
    std::string out;
    if (wide) // UTF-16LE → UTF-8 (labels may have any character; plugin names are ASCII)
        for (std::size_t i = 0; i + 1 < size; i += 2)
        {
            std::uint32_t c = (std::uint32_t) p[i] | ((std::uint32_t) p[i + 1] << 8);
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < size)
            {
                const auto low = (std::uint32_t) p[i + 2] | ((std::uint32_t) p[i + 3] << 8);
                if (low >= 0xDC00 && low < 0xE000)
                {
                    c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                    i += 2;
                }
            }
            if (c < 0x80)
                out += (char) c;
            else if (c < 0x800)
                out += (char) (0xC0 | (c >> 6)), out += (char) (0x80 | (c & 0x3F));
            else if (c < 0x10000)
                out += (char) (0xE0 | (c >> 12)), out += (char) (0x80 | ((c >> 6) & 0x3F)), out += (char) (0x80 | (c & 0x3F));
            else
                out += (char) (0xF0 | (c >> 18)), out += (char) (0x80 | ((c >> 12) & 0x3F)), out += (char) (0x80 | ((c >> 6) & 0x3F)),
                    out += (char) (0x80 | (c & 0x3F));
        }
    else // 8-bit (version 3): Windows-1252 / Latin-1 → UTF-8
        for (std::size_t i = 0; i < size; ++i)
            if (p[i] < 0x80)
                out += (char) p[i];
            else
                out += (char) (0xC0 | (p[i] >> 6)), out += (char) (0x80 | (p[i] & 0x3F));
    while (! out.empty() && out.back() == '\0')
        out.pop_back();
    return out;
}

struct Port
{
    int node = -1;      // -1 = Patcher pin
    bool input = false; // node input, or pin that takes the sound out
    int bus = 0;
    int kind = 0;       // 0 audio, 1 MIDI, 2 control
};

struct Connection
{
    std::uint64_t destination = 0, source = 0;
    float gain = 1.0f;
    int kind = -1;         // −1 = port class (flags bit 1 cleared)
    bool disabled = false; // flags bit 0
};

bool isSplitterName (std::string name)
{
    for (auto& ch : name)
        ch = (char) std::tolower ((unsigned char) ch);
    return name == "frequency splitter" || name == "fruity stereo shaper"; // nodes that split by bus
}

// Series-parallel reduction (REA_rodada20 §3.3). Vertices: IN, OUT and, for each node (or splitter node per
// bus), its input and output; edges carry the expression. Repeat: edges with the same endpoints
// become parallel; a vertex with one input and one output becomes series.
struct SpEdge
{
    int from, to;
    PatcherExpr expr;
};

PatcherExpr flattenSeries (PatcherExpr a, PatcherExpr b)
{
    PatcherExpr s;
    s.kind = PatcherExpr::Kind::series;
    s.gain = a.gain * b.gain;
    for (auto* e : { &a, &b })
    {
        if (e->kind == PatcherExpr::Kind::identity)
            continue;
        if (e->kind == PatcherExpr::Kind::series)
            for (auto& c : e->children)
                s.children.push_back (std::move (c));
        else
        {
            e->gain = 1.0f;
            s.children.push_back (std::move (*e));
        }
    }
    if (s.children.empty())
        return { PatcherExpr::Kind::identity, -1, -1, s.gain, {} };
    if (s.children.size() == 1)
    {
        auto only = std::move (s.children.front());
        only.gain = s.gain;
        return only;
    }
    return s;
}

std::optional<PatcherExpr> reduce (std::vector<SpEdge> edges, int source, int sink)
{
    for (int guard = 0; guard < 100000; ++guard)
    {
        if (edges.size() == 1 && edges[0].from == source && edges[0].to == sink)
            return edges[0].expr;
        bool changed = false;

        // parallel
        for (std::size_t i = 0; i < edges.size() && ! changed; ++i)
            for (std::size_t j = i + 1; j < edges.size(); ++j)
                if (edges[i].from == edges[j].from && edges[i].to == edges[j].to)
                {
                    PatcherExpr p;
                    p.kind = PatcherExpr::Kind::parallel;
                    for (auto* e : { &edges[i].expr, &edges[j].expr })
                        if (e->kind == PatcherExpr::Kind::parallel && e->gain == 1.0f)
                            for (auto& c : e->children)
                                p.children.push_back (std::move (c));
                        else
                            p.children.push_back (std::move (*e));
                    edges[i].expr = std::move (p);
                    edges.erase (edges.begin() + (std::ptrdiff_t) j);
                    changed = true;
                    break;
                }
        if (changed)
            continue;

        // series
        std::map<int, int> inCount, outCount;
        for (const auto& e : edges)
        {
            ++outCount[e.from];
            ++inCount[e.to];
        }
        for (const auto& [v, n] : inCount)
        {
            if (v == source || v == sink || n != 1 || outCount[v] != 1)
                continue;
            const auto a = std::find_if (edges.begin(), edges.end(), [v = v] (const SpEdge& e) { return e.to == v; }) - edges.begin();
            const auto b = std::find_if (edges.begin(), edges.end(), [v = v] (const SpEdge& e) { return e.from == v; }) - edges.begin();
            SpEdge merged { edges[(std::size_t) a].from, edges[(std::size_t) b].to,
                            flattenSeries (std::move (edges[(std::size_t) a].expr), std::move (edges[(std::size_t) b].expr)) };
            edges.erase (edges.begin() + std::max (a, b));
            edges.erase (edges.begin() + std::min (a, b));
            edges.push_back (std::move (merged));
            changed = true;
            break;
        }
        if (! changed)
            return std::nullopt;
    }
    return std::nullopt;
}
} // namespace

std::optional<PatcherGraph> parsePatcher (const std::vector<std::uint8_t>& state)
{
    if (state.size() < 16)
        return std::nullopt;
    const auto version = at<std::uint32_t> (state.data(), 0);
    if (version < 3 || version > 4)
        return std::nullopt;
    const auto wide = version >= 4;

    PatcherGraph graph;
    std::map<std::uint64_t, Port> ports;
    std::vector<Connection> connections;
    std::uint64_t inputPin = 0, outputPin = 0;
    bool hasInput = false, hasOutput = false;

    std::vector<std::pair<std::uint64_t, std::pair<float, float>>> positions;
    const auto ok = forEachChunk (state.data() + 4, state.size() - 4, [&] (std::uint32_t id, const std::uint8_t* d, std::size_t n) {
        if (id == 1100 && n >= 12) // an exposed parameter
            graph.exposed.push_back ({ at<std::uint64_t> (d, 0), at<std::int32_t> (d, 8) });
        if (id == 1600 && n >= 16) // (positions may also come as plain records)
            positions.push_back ({ at<std::uint64_t> (d, 0), { at<float> (d, 8), at<float> (d, 12) } });
        if (id != 0)
            return;
        std::vector<std::pair<std::uint32_t, std::pair<const std::uint8_t*, std::size_t>>> sub;
        forEachChunk (d, n, [&] (std::uint32_t k, const std::uint8_t* x, std::size_t m) { sub.push_back ({ k, { x, m } }); });
        if (sub.empty())
            return;

        const auto first = sub.front().first;
        if (first == 5000 && sub.front().second.second >= 4) // node
        {
            PatcherNode node;
            node.generator = at<std::uint32_t> (sub.front().second.first, 0) == 3;
            if (sub.front().second.second >= 16)
            {
                node.id = at<std::uint64_t> (sub.front().second.first, 4);
                node.flags = at<std::uint32_t> (sub.front().second.first, 12);
            }
            const auto index = (int) graph.nodes.size();
            for (const auto& [k, x] : sub)
                if (k == 5008)
                    node.plugin = text (x.first, x.second, wide);
                else if (k == 5010)
                    node.label = text (x.first, x.second, wide);
                else if (k == 5005)
                    node.state.assign (x.first, x.first + x.second);
                else if (k == 5009 && x.second >= 20)
                {
                    ports[at<std::uint64_t> (x.first, 8)] = { index, (at<std::uint32_t> (x.first, 16) & 1) != 0, (int) at<std::uint32_t> (x.first, 4),
                                                             (int) at<std::uint32_t> (x.first, 0) };
                    graph.ports.push_back ({ at<std::uint64_t> (x.first, 8),
                                             { index, (at<std::uint32_t> (x.first, 16) & 1) != 0, (int) at<std::uint32_t> (x.first, 4),
                                               (int) at<std::uint32_t> (x.first, 0), {} } });
                }
            graph.nodes.push_back (std::move (node));
        }
        else if (first == 1400 && sub.front().second.second >= 21) // I/O pin
        {
            const auto* x = sub.front().second.first;
            const auto portId = at<std::uint64_t> (x, 0);
            const auto kind = (int) at<std::uint32_t> (x, 8);
            const auto fromOutside = (at<std::uint32_t> (x, 12) & 1) != 0;
            const auto bus = at<std::int32_t> (x, 17);
            ports[portId] = { -1, ! fromOutside, bus, kind };
            std::string name;
            for (const auto& [k, x2] : sub)
                if (k == 1402)
                    name = text (x2.first, x2.second, wide);
            graph.ports.push_back ({ portId, { -1, ! fromOutside, bus, kind, name } });
            if (kind == 0 && bus == 0 && fromOutside)
                inputPin = portId, hasInput = true;
            if (kind == 0 && bus == 0 && ! fromOutside)
                outputPin = portId, hasOutput = true;
        }
        else if (first == 1300 && sub.front().second.second >= 25) // connection
        {
            const auto* x = sub.front().second.first;
            // flags (offset 20): bit 0 = cable disabled; bit 1 = the class byte (24) is valid (REA_rodada20 §3.1)
            const auto flags = at<std::uint32_t> (x, 20);
            connections.push_back ({ at<std::uint64_t> (x, 0), at<std::uint64_t> (x, 8), at<float> (x, 16), (flags & 2) != 0 ? (int) x[24] : -1,
                                     (flags & 1) != 0 });
        }
        else if (first == 1600 && sub.front().second.second >= 16) // a node's position
            positions.push_back ({ at<std::uint64_t> (sub.front().second.first, 0),
                                   { at<float> (sub.front().second.first, 8), at<float> (sub.front().second.first, 12) } });
    });
    if (! ok)
        return std::nullopt;

    // positions: the nodes by id; the first two others are the I/O nodes (input, then output)
    {
        int io = 0;
        for (const auto& [nodeId, xy] : positions)
        {
            auto node = std::find_if (graph.nodes.begin(), graph.nodes.end(), [nodeId = nodeId] (const PatcherNode& n) { return n.id == nodeId; });
            if (node != graph.nodes.end() && nodeId != 0)
            {
                node->x = xy.first;
                node->y = xy.second;
                node->placed = true;
            }
            else if (io == 0)
            {
                graph.inputX = xy.first;
                graph.inputY = xy.second;
                ++io;
            }
            else if (io == 1)
            {
                graph.outputX = xy.first;
                graph.outputY = xy.second;
                graph.ioPlaced = true;
                ++io;
            }
        }
    }

    // every cable, resolved (class from byte 24 if valid, else the destination port's)
    for (const auto& c : connections)
    {
        auto kind = c.kind;
        if (kind < 0)
        {
            const auto dst = ports.find (c.destination);
            kind = dst != ports.end() ? dst->second.kind : 2;
        }
        graph.cables.push_back ({ c.destination, c.source, c.gain, kind, c.disabled });
    }

    // cable class: byte 24's if valid; otherwise, the destination port's. Disabled cables are dropped.
    std::erase_if (connections, [] (const Connection& c) { return c.disabled; });
    for (auto& c : connections)
        if (c.kind < 0)
        {
            const auto dst = ports.find (c.destination);
            c.kind = dst != ports.end() ? dst->second.kind : 2;
        }

    // main generator: the one reaching the output with the most gain
    float best = -1.0f;
    for (const auto& c : connections)
        if (c.kind == 0 && hasOutput && c.destination == outputPin)
            if (const auto src = ports.find (c.source); src != ports.end() && src->second.node >= 0
                && graph.nodes[(std::size_t) src->second.node].generator && c.gain > best)
            {
                best = c.gain;
                graph.mainGenerator = src->second.node;
            }

    // serial chain: only audio edges on bus 0, each node with one input and one output
    if (hasInput && hasOutput)
    {
        std::map<int, std::vector<int>> next; // -1 = IN, -2 = OUT
        int audioEdges = 0;
        bool simple = true;
        for (const auto& c : connections)
        {
            if (c.kind != 0)
                continue;
            const auto src = ports.find (c.source), dst = ports.find (c.destination);
            if (src == ports.end() || dst == ports.end())
                continue;
            ++audioEdges;
            if (src->second.bus != 0 || dst->second.bus != 0 || std::abs (c.gain - 1.0f) > 1e-3f)
                simple = false;
            const auto from = src->second.node >= 0 ? src->second.node : (c.source == inputPin ? -1 : -3);
            const auto to = dst->second.node >= 0 ? dst->second.node : (c.destination == outputPin ? -2 : -3);
            if (from == -3 || to == -3)
                simple = false;
            next[from].push_back (to);
        }

        std::vector<int> chain;
        auto cursor = -1;
        while (simple && next[cursor].size() == 1)
        {
            cursor = next[cursor].front();
            if (cursor == -2)
                break;
            if (cursor < 0 || graph.nodes[(std::size_t) cursor].generator || chain.size() > graph.nodes.size())
            {
                simple = false;
                break;
            }
            chain.push_back (cursor);
        }
        if (simple && cursor == -2 && audioEdges == (int) chain.size() + 1)
            graph.serialChain = std::move (chain);
    }

    // parallel branches: common serial section from IN, then branches (from IN, from a node with several
    // outputs, or from the bands of a Frequency Splitter), each a simple chain to OUT. Sidechain
    // inputs (bus > 0 at the destination) are left out.
    if (hasInput && hasOutput)
    {
        struct Edge
        {
            int from, to, fromBus;
            float gain;
        };
        std::vector<Edge> edges;
        bool ok = true;
        for (const auto& c : connections)
        {
            if (c.kind != 0)
                continue;
            const auto src = ports.find (c.source), dst = ports.find (c.destination);
            if (src == ports.end() || dst == ports.end())
                continue;
            if (dst->second.bus != 0)
                continue; // sidechain
            const auto from = src->second.node >= 0 ? src->second.node : (c.source == inputPin ? -1 : -3);
            const auto to = dst->second.node >= 0 ? dst->second.node : (c.destination == outputPin ? -2 : -3);
            if (from == -3 || to == -3)
            {
                ok = false;
                continue;
            }
            edges.push_back ({ from, to, src->second.bus, std::isfinite (c.gain) ? c.gain : 1.0f });
        }

        auto isSplitter = [&] (int n) {
            auto name = graph.nodes[(std::size_t) n].plugin;
            for (auto& ch : name)
                ch = (char) std::tolower ((unsigned char) ch);
            return name == "frequency splitter" || name == "fruity stereo shaper"; // nodes that split by bus
        };
        auto outs = [&] (int n) {
            std::vector<std::size_t> r;
            for (std::size_t i = 0; i < edges.size(); ++i)
                if (edges[i].from == n)
                    r.push_back (i);
            return r;
        };
        auto countTo = [&] (int n) { return (int) std::count_if (edges.begin(), edges.end(), [n] (const Edge& e) { return e.to == n; }); };

        // only bus 0 outside the splitter (other outputs, like the Stereo Shaper's, are not reproducible)
        for (const auto& e : edges)
            if (e.fromBus != 0 && (e.from < 0 || ! isSplitter (e.from)))
                ok = false;

        std::vector<bool> consumed (edges.size(), false);
        auto cursor = -1;
        float preGain = 1.0f;
        std::vector<int> pre;
        while (ok)
        {
            const auto o = outs (cursor);
            if (o.size() != 1 || edges[o[0]].to < 0 || countTo (edges[o[0]].to) != 1)
                break;
            consumed[o[0]] = true;
            preGain *= edges[o[0]].gain;
            cursor = edges[o[0]].to;
            if (graph.nodes[(std::size_t) cursor].generator)
                ok = false;
            pre.push_back (cursor);
            if (isSplitter (cursor))
                break;
        }

        std::vector<PatcherGraph::Branch> branches;
        const auto splitterHere = cursor >= 0 && isSplitter (cursor);
        for (const auto i : outs (cursor))
        {
            if (! ok)
                break;
            consumed[i] = true;
            PatcherGraph::Branch branch;
            branch.gain = preGain * edges[i].gain;
            branch.band = splitterHere ? edges[i].fromBus : 0;
            auto at = edges[i].to;
            while (ok && at >= 0)
            {
                const auto o = outs (at);
                if (graph.nodes[(std::size_t) at].generator || isSplitter (at) || countTo (at) != 1 || o.size() != 1
                    || branch.nodes.size() > graph.nodes.size())
                {
                    ok = false;
                    break;
                }
                branch.nodes.push_back (at);
                consumed[o[0]] = true;
                branch.gain *= edges[o[0]].gain;
                at = edges[o[0]].to;
            }
            if (at != -2)
                ok = false;
            branches.push_back (std::move (branch));
        }

        if (ok && std::all_of (consumed.begin(), consumed.end(), [] (bool c) { return c; })
            && (branches.size() >= 2 || (splitterHere && ! branches.empty())))
        {
            if (splitterHere)
            {
                graph.splitter = cursor;
                pre.pop_back();
            }
            graph.preChain = std::move (pre);
            graph.branches = std::move (branches);
        }
    }

    // audio graph reduced to series/parallel (REA_rodada20 §3.3)
    if (hasInput && hasOutput)
    {
        struct Edge
        {
            int from, to, fromBus;
            float gain;
        };
        std::vector<Edge> edges;
        bool valid = true;
        for (const auto& c : connections)
        {
            if (c.kind != 0)
                continue;
            const auto src = ports.find (c.source), dst = ports.find (c.destination);
            if (src == ports.end() || dst == ports.end() || dst->second.bus != 0)
                continue; // sidechain is left out
            const auto from = src->second.node >= 0 ? src->second.node : (c.source == inputPin ? -1 : -3);
            const auto to = dst->second.node >= 0 ? dst->second.node : (c.destination == outputPin ? -2 : -3);
            if (from == -3 || to == -3)
                continue;
            edges.push_back ({ from, to, src->second.bus, std::isfinite (c.gain) ? c.gain : 1.0f });
        }

        // prune: only nodes reachable from IN that also reach OUT
        auto reach = [&] (int start, bool forward) {
            std::set<int> seen { start };
            std::vector<int> stack { start };
            while (! stack.empty())
            {
                const auto v = stack.back();
                stack.pop_back();
                for (const auto& e : edges)
                    if ((forward ? e.from : e.to) == v && seen.insert (forward ? e.to : e.from).second)
                        stack.push_back (forward ? e.to : e.from);
            }
            return seen;
        };
        const auto fromIn = reach (-1, true), toOut = reach (-2, false);
        std::erase_if (edges, [&] (const Edge& e) { return ! fromIn.count (e.from) || ! toOut.count (e.from) || ! fromIn.count (e.to) || ! toOut.count (e.to); });

        // splitters with outputs on bus ≠ 0 become one vertex per used bus
        std::map<int, std::set<int>> splitBuses;
        for (const auto& e : edges)
            if (e.from >= 0 && e.fromBus != 0)
            {
                if (! isSplitterName (graph.nodes[(std::size_t) e.from].plugin))
                    valid = false; // side output of an ordinary node: not reproducible
                splitBuses[e.from].insert (e.fromBus);
            }
        for (const auto& e : edges)
            if (e.from >= 0 && splitBuses.count (e.from))
                splitBuses[e.from].insert (e.fromBus);

        // vertices: IN = 0, OUT = 1; node n (or n per bus): input/output
        std::map<std::pair<int, int>, int> vin, vout;
        int next = 2;
        std::vector<SpEdge> sp;
        auto vertexPair = [&] (int n, int bus) {
            const auto key = std::make_pair (n, bus);
            if (! vin.count (key))
            {
                vin[key] = next++;
                vout[key] = next++;
                PatcherExpr e;
                e.kind = PatcherExpr::Kind::node;
                e.node = n;
                e.bus = bus;
                sp.push_back ({ vin[key], vout[key], std::move (e) });
            }
        };
        auto busesOf = [&] (int n) { return splitBuses.count (n) ? std::vector<int> (splitBuses[n].begin(), splitBuses[n].end()) : std::vector<int> { -1 }; };
        for (const auto& e : edges)
        {
            if ((e.from >= 0 && graph.nodes[(std::size_t) e.from].generator) || (e.to >= 0 && graph.nodes[(std::size_t) e.to].generator))
                valid = false;
            const auto fromBus = e.from >= 0 && splitBuses.count (e.from) ? e.fromBus : -1;
            int from = 0;
            if (e.from >= 0)
            {
                vertexPair (e.from, fromBus);
                from = vout[{ e.from, fromBus }];
            }
            // the cable entering a splitter feeds all of its buses
            for (const auto bus : e.to >= 0 ? busesOf (e.to) : std::vector<int> { -1 })
            {
                int to = 1;
                if (e.to >= 0)
                {
                    vertexPair (e.to, bus);
                    to = vin[{ e.to, bus }];
                }
                sp.push_back ({ from, to, { PatcherExpr::Kind::identity, -1, -1, e.gain, {} } });
            }
        }
        if (valid && ! sp.empty())
            graph.audio = reduce (std::move (sp), 0, 1);
    }

    return graph;
}

int flPatcherMidiPinPort (const PatcherPort& pin)
{
    if (pin.kind != 1)
        return -2;
    if (pin.name == "Notes")
        return -1;
    for (const char* prefix : { "MIDI port ", "MIDI Port " })
        if (pin.name.rfind (prefix, 0) == 0)
        {
            const auto n = std::atoi (pin.name.c_str() + 10);
            return n >= 0 && n < 256 ? n : -2;
        }
    return pin.index >= 0 && pin.index < 256 ? pin.index : -2;
}
} // namespace lynx
