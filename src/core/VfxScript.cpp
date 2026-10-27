#include "core/VfxScript.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "core/Python.h"

extern "C"
{
#include "pocketpy.h"
}

namespace lynx::vfx::script
{
namespace
{
// ---------------------------------------------------------------------------------------------------------------
// The flvfx module (Python side). Written from FL's published API reference; LYNX's own code. The host part
// (_lynxvfx) is native: it turns triggers / releases / property changes into LYNX voice events at the current sample
// and reads / writes the controls and the output controllers.
constexpr const char* flvfxSource = R"PY(
import _lynxvfx as _host

class _Context:
    def __init__(self):
        self.ticks = 0
        self.PPQ = 96
        self.isPlaying = False
        self.tempo = 120.0
        self._voices = []
        self.form = None

    # a snapshot: a script may release voices while it walks them (FL's tutorial scripts do)
    @property
    def voices(self):
        return list(self._voices)

context = _Context()
_clock = [0]

class Voice:
    def __init__(self, source=None):
        self.note = 60.0
        self.finePitch = 0.0
        self.velocity = 0.8
        self.releaseVelocity = 0.5
        self.pan = 0.0
        self.fcut = 0.0
        self.fres = 0.0
        self.color = 0
        self.length = 0
        self.output = 0
        self._id = 0
        self._sent = None
        self._until = 0
        if source is not None:
            self.copyFrom(source)

    def copyFrom(self, other):
        self.note = other.note
        self.finePitch = other.finePitch
        self.velocity = other.velocity
        self.releaseVelocity = other.releaseVelocity
        self.pan = other.pan
        self.fcut = other.fcut
        self.fres = other.fres
        self.color = other.color
        self.length = other.length
        self.output = other.output

    def trigger(self):
        i = getattr(self, '_id', 0)
        if i < 0:
            raise RuntimeError('Incoming voice cannot be triggered. Make a copy of the voice and trigger that instead.')
        if i > 0:
            raise RuntimeError('A voice can only be triggered once.')
        pitch = float(self.note) + float(self.finePitch)
        self._id = _host.on(float(self.note), float(self.finePitch), float(self.velocity), float(self.releaseVelocity),
                            float(self.pan), float(self.fcut), float(self.fres), int(self.color), int(self.output))
        self._sent = [pitch, self.velocity, self.pan, self.fcut, self.fres]
        n = int(self.length)
        if self.length > 0 and n < 1:
            n = 1
        self._until = _clock[0] + n if n > 0 else 0
        context._voices.append(self)

    def release(self):
        i = getattr(self, '_id', 0)
        if i <= 0:
            raise RuntimeError('Trying to release a voice that never been triggered.')
        vs = context._voices
        for k in range(len(vs)):
            if vs[k] is self:
                vs.pop(k)
                _host.off(i, float(self.releaseVelocity))
                return

    Trigger = trigger
    Release = release
    CopyFrom = copyFrom

_controlMethods = ['addInputKnob', 'addInputKnobInt', 'addInputCheckbox', 'addInputCombo', 'addInputText', 'addInput',
                   'addInputSurface', 'addGroup', 'endGroup', 'getInputValue', 'setNormalizedValue']

class ScriptDialog:
    def __init__(self, title='', description=''):
        self._title = str(title)
        self._description = str(description)
        self._group = ''
        self._controls = []
        self._index = {}

    def _add(self, kind, label, default, lo, hi, options, hint):
        label = str(label)
        name = label if self._group == '' else self._group + ': ' + label
        if name not in self._index:
            self._index[name] = len(self._controls)
        self._controls.append((kind, name, self._group, label, default, lo, hi, options, str(hint)))

    def addInputKnob(self, name, default, min, max, hint=''):
        self._add(0, name, float(default), float(min), float(max), None, hint)
    def addInputKnobInt(self, name, default, min, max, hint=''):
        self._add(1, name, int(default), int(min), int(max), None, hint)
    def addInputCheckbox(self, name, default, hint=''):
        self._add(2, name, 1 if default else 0, 0, 1, None, hint)
    def addInputCombo(self, name, options, default, hint=''):
        if isinstance(options, str):
            options = options.split(',')
        options = [str(o) for o in options]
        self._add(3, name, int(default), 0, max(0, len(options) - 1), options, hint)
    def addInputText(self, name, default='', hint=''):
        self._add(4, name, str(default), 0, 0, None, hint)
    def addInput(self, name, default=0, hint=''):
        self._add(5, name, float(default), 0.0, 1.0, None, hint)
    def addInputSurface(self, name=''):
        self._add(6, name, 0, 0, 0, None, '')
    def addGroup(self, name):
        self._group = str(name)
    def endGroup(self):
        self._group = ''
    def getInputValue(self, name):
        i = self._index.get(name)
        if i is None:
            return _host.extra(str(name))
        return _host.get(i)
    def setNormalizedValue(self, name, value):
        i = self._index.get(name)
        if i is None:
            _host.setextra(str(name), float(value))
        else:
            _host.set(i, float(value))

    def __getattr__(self, name):
        low = name.lower()
        for m in _controlMethods:
            if m.lower() == low:
                return getattr(self, m)
        raise AttributeError(name)

_outputs = []
_outputIndex = {}

def addOutputController(name, default=0.0):
    name = str(name)
    if name in _outputIndex:
        return
    _outputIndex[name] = len(_outputs)
    _outputs.append((name, float(default)))
    _host.ctl(len(_outputs) - 1, float(default))

def setOutputController(name, value):
    i = _outputIndex.get(name)
    if i is not None:
        _host.ctl(i, float(value))

AddOutputController = addOutputController
SetOutputController = setOutputController

# --- LYNX's glue (called by the host) ---------------------------------------------------------------------------
_incoming = {}

def _in_on(i, note, fine, vel, rel, pan, fcut, fres, color):
    v = Voice()
    v.note = note
    v.finePitch = fine
    v.velocity = vel
    v.releaseVelocity = rel
    v.pan = pan
    v.fcut = fcut
    v.fres = fres
    v.color = color
    v._id = -1
    _incoming[i] = v
    return v

def _in_off(i, rel):
    v = _incoming.pop(i, None)
    if v is not None:
        v.releaseVelocity = rel
    return v

def _in_prop(i, which, value):
    v = _incoming.get(i)
    if v is None:
        return
    if which == 0:
        v.velocity = value
    elif which == 1:
        v.finePitch = value
    elif which == 2:
        v.pan = value
    elif which == 3:
        v.fcut = value
    else:
        v.fres = value

def _sync():
    for v in context._voices:
        s = v._sent
        if s is None:
            continue
        p = float(v.note) + float(v.finePitch)
        if p != s[0]:
            s[0] = p
            _host.prop(v._id, 1, p)
        if v.velocity != s[1]:
            s[1] = v.velocity
            _host.prop(v._id, 0, float(v.velocity))
        if v.pan != s[2]:
            s[2] = v.pan
            _host.prop(v._id, 2, float(v.pan))
        if v.fcut != s[3]:
            s[3] = v.fcut
            _host.prop(v._id, 3, float(v.fcut))
        if v.fres != s[4]:
            s[4] = v.fres
            _host.prop(v._id, 4, float(v.fres))

def _tick(t):
    context.ticks = t
    _clock[0] += 1
    c = _clock[0]
    vs = context._voices
    if len(vs) > 0:
        for v in [x for x in vs if x._until > 0 and x._until <= c]:
            v.release()

def _set(ppq, tempo, playing):
    context.PPQ = ppq
    context.tempo = tempo
    context.isPlaying = playing

def _clear():
    for v in context._voices:
        _host.off(v._id, float(v.releaseVelocity))
    context._voices.clear()
    _incoming.clear()
)PY";

// ---------------------------------------------------------------------------------------------------------------
// INI helpers on the state's text

double parseDouble (const std::string& s, double fallback)
{
    try
    {
        std::size_t used = 0;
        const auto v = std::stod (s, &used);
        return used > 0 && std::isfinite (v) ? v : fallback;
    }
    catch (...)
    {
        return fallback;
    }
}

float floatOfBits (std::int64_t bits)
{
    const auto u = (std::uint32_t) bits;
    float f = 0.0f;
    std::memcpy (&f, &u, sizeof f);
    return std::isfinite (f) ? f : 0.0f;
}

std::int64_t bitsOfFloat (float f)
{
    std::uint32_t u = 0;
    std::memcpy (&u, &f, sizeof f);
    return (std::int64_t) u;
}

// ---------------------------------------------------------------------------------------------------------------
// The running instance

std::atomic<bool> realtimeFlag { false };

constexpr int maxControls = 256, maxOutputs = 64, ringSize = 8, maxIn = 256, maxOut = 768, logCap = 64 * 1024;

struct Job
{
    std::atomic<int> state { 0 }; // 0 free, 1 waiting for the Python thread, 2 answered
    int n = 0;
    Transport transport;
    bool releaseAll = false;
    std::array<Event, maxIn> in {};
    int inCount = 0;
    std::array<Event, maxOut> out {};
    int outCount = 0;
};

class Runtime;
std::mutex registryLock;
std::unordered_map<std::uint32_t, std::weak_ptr<Runtime>> registry;

class Runtime final : public Monitor, public python::Service, public std::enable_shared_from_this<Runtime>
{
public:
    Runtime() { ring = std::make_unique<std::array<Job, ringSize>>(); }

    ~Runtime() override
    {
        python::removeService (this);
        if (vm > 0)
            python::runSync ([this] { python::releaseVm (vm); });
    }

    // --- UI thread ------------------------------------------------------------------------------------------------
    void setTag (std::uint32_t t)
    {
        if (t == tag || t == 0)
            return;
        tag = t;
        std::lock_guard<std::mutex> l (registryLock);
        registry[t] = weak_from_this();
    }

    // Compile (a new source) or take new control values (same source)
    void configure (const ScriptSettings& settings)
    {
        const auto src = settings.source();
        pendingValues = settings;
        if (! started || src != source)
        {
            source = src;
            started = true;
            compile (settings, false);
            return;
        }
        // the same script: the values the user changed (not the ones the script set itself) reach it
        const auto info = currentInfo();
        if (info == nullptr)
            return;
        const auto now = valuesOf (*info, settings);
        if (now.numbers.size() == stateValues.numbers.size())
        {
            for (std::size_t k = 0; k < now.numbers.size() && k < (std::size_t) maxControls; ++k)
                if (now.numbers[k] != stateValues.numbers[k])
                    numbers[k].store (now.numbers[k]);
            std::lock_guard<std::mutex> l (textLock);
            for (std::size_t k = 0; k < now.texts.size() && k < texts.size(); ++k)
                if (now.texts[k] != stateValues.texts[k])
                    texts[k] = now.texts[k];
        }
        stateValues = now;
    }

    void compile (const ScriptSettings& settings, bool keepValues)
    {
        python::runSync ([&] { compileOnPythonThread (settings, keepValues); });
        python::addService (this);
    }

    // --- Monitor ------------------------------------------------------------------------------------------------------
    std::string log() const override
    {
        std::lock_guard<std::mutex> l (logLock);
        return logText;
    }
    std::uint64_t logVersion() const override { return logCounter.load(); }
    void clearLog() override
    {
        std::lock_guard<std::mutex> l (logLock);
        logText.clear();
        ++logCounter;
    }
    std::string status() const override
    {
        std::lock_guard<std::mutex> l (logLock);
        return statusText;
    }
    Values liveValues() const override
    {
        Values v;
        const auto info = currentInfo();
        if (info == nullptr)
            return v;
        v.numbers.resize (info->controls.size());
        v.texts.resize (info->controls.size());
        for (std::size_t k = 0; k < info->controls.size() && k < (std::size_t) maxControls; ++k)
            v.numbers[k] = numbers[k].load();
        std::lock_guard<std::mutex> l (textLock);
        for (std::size_t k = 0; k < v.texts.size() && k < texts.size(); ++k)
            v.texts[k] = texts[k];
        return v;
    }
    bool takeScriptChanges (Values& out) override
    {
        if (! scriptChanged.exchange (false))
            return false;
        out = liveValues();
        return true;
    }
    std::vector<float> outputValues() const override
    {
        std::vector<float> v ((std::size_t) std::min (outputCount.load(), maxOutputs));
        for (std::size_t k = 0; k < v.size(); ++k)
            v[k] = outputs[k].load();
        return v;
    }
    void restart() override
    {
        auto settings = pendingValues;
        compile (settings, true);
    }

    // --- audio thread -------------------------------------------------------------------------------------------------
    void process (const Event* in, int count, int n, const Transport& transport, EventList& out) noexcept
    {
        auto& r = *ring;
        // answers that came after their block: now, at this block's start
        while (r[(std::size_t) (readIndex % ringSize)].state.load (std::memory_order_acquire) == 2)
        {
            auto& j = r[(std::size_t) (readIndex % ringSize)];
            for (int k = 0; k < j.outCount; ++k)
            {
                auto e = j.out[(std::size_t) k];
                e.offset = 0;
                out.add (e);
            }
            j.state.store (0, std::memory_order_release);
            ++readIndex;
        }
        auto& job = r[(std::size_t) (writeIndex % ringSize)];
        if (job.state.load (std::memory_order_acquire) != 0) // (the Python thread is far behind: keep the notes)
        {
            for (int k = 0; k < count && carryCount < maxIn; ++k)
                if (in[k].type != Event::Type::prop)
                {
                    carry[(std::size_t) carryCount] = in[k];
                    carry[(std::size_t) carryCount++].offset = 0;
                }
            return;
        }
        job.n = n;
        job.transport = transport;
        job.releaseAll = releaseRequested.exchange (false);
        job.inCount = 0;
        for (int k = 0; k < carryCount; ++k)
            job.in[(std::size_t) job.inCount++] = carry[(std::size_t) k];
        carryCount = 0;
        for (int k = 0; k < count && job.inCount < maxIn; ++k)
            job.in[(std::size_t) job.inCount++] = in[k];
        job.outCount = 0;
        job.state.store (1, std::memory_order_release);
        ++writeIndex;
        python::wake();

        if (readIndex != writeIndex - 1)
            return; // (older answers still pending: this one comes later too)
        const auto realtime = realtimeFlag.load (std::memory_order_relaxed);
        const auto budget = std::chrono::nanoseconds ((std::int64_t) (std::clamp (0.3 * n / std::max (1.0, transport.sampleRate), 0.0002, 0.002) * 1e9));
        const auto start = std::chrono::steady_clock::now();
        int spins = 0;
        while (job.state.load (std::memory_order_acquire) != 2)
        {
            if (++spins > 64)
            {
                if (realtime && std::chrono::steady_clock::now() - start > budget)
                    return; // late: plays at the next block's start
                std::this_thread::yield();
            }
        }
        for (int k = 0; k < job.outCount; ++k)
            out.add (job.out[(std::size_t) k]);
        job.state.store (0, std::memory_order_release);
        ++readIndex;
    }

    void requestReleaseAll() noexcept { releaseRequested = true; }

    void setNormalized (int k, float v01) noexcept
    {
        const auto info = infoForAudio.load (std::memory_order_acquire);
        if (info == nullptr || k < 0 || k >= (int) info->controls.size() || k >= maxControls)
            return;
        const auto& c = info->controls[(std::size_t) k];
        if (c.numeric())
            numbers[(std::size_t) k].store (c.fromNormalized (v01));
    }

    float output (int k) const noexcept { return k >= 0 && k < maxOutputs ? outputs[(std::size_t) k].load (std::memory_order_relaxed) : 0.0f; }
    bool busy() const noexcept { return wantsTicks.load (std::memory_order_relaxed) || activeOutputs.load (std::memory_order_relaxed) > 0; }

    // --- the Python thread --------------------------------------------------------------------------------------------
    void serve() noexcept override
    {
        auto& r = *ring;
        while (r[(std::size_t) (serveIndex % ringSize)].state.load (std::memory_order_acquire) == 1)
        {
            auto& job = r[(std::size_t) (serveIndex % ringSize)];
            run (job);
            job.state.store (2, std::memory_order_release);
            ++serveIndex;
        }
    }

    // _lynxvfx's natives reach the instance through the VM's context
    static Runtime* current() { return static_cast<Runtime*> (py_getvmctx()); }

    Event* emit()
    {
        if (job == nullptr || job->outCount >= maxOut)
            return nullptr;
        auto& e = job->out[(std::size_t) job->outCount++];
        e = Event {};
        e.offset = std::clamp (offset, 0, std::max (0, job->n - 1));
        return &e;
    }

    struct Sounding
    {
        int id = 0, key = 60, colour = 0, output = 0;
        bool audible = true;
    };
    std::vector<Sounding> sounding; // (Python thread)
    std::vector<Event> pendingOffs; // offs of the voices of a script that was compiled again / released (Python thread)

    int nativeOn (double note, double fine, double velocity, double release, double pan, double fcut, double fres, int colour, int output)
    {
        nextId = (nextId % ((1 << 20) - 2)) + 1;
        Sounding s;
        s.id = nextId;
        s.key = (int) std::lround (std::isfinite (note) ? note : 60.0);
        s.audible = s.key >= 0 && s.key <= 127 && velocity > 0.0 && output >= 0 && output < vfx::outputCount (Kind::script);
        s.key = std::clamp (s.key, 0, 127);
        s.colour = colour & 15;
        s.output = std::clamp (output, 0, vfx::outputCount (Kind::script) - 1);
        sounding.push_back (s);
        activeOutputs.store ((int) sounding.size());
        if (s.audible)
            if (auto* e = emit())
            {
                e->type = Event::Type::on;
                e->output = s.output;
                e->id = s.id;
                e->voice.key = s.key;
                e->voice.cents = (float) ((note + fine - s.key) * 100.0);
                e->voice.velocity = (float) std::clamp (velocity, 0.0, 1.0);
                e->voice.release = (float) std::clamp (release, 0.0, 1.0);
                e->voice.pan = (float) std::clamp (pan, -1.0, 1.0);
                e->voice.modX = (float) std::clamp ((fcut + 1.0) * 0.5, 0.0, 1.0);
                e->voice.modY = (float) std::clamp ((fres + 1.0) * 0.5, 0.0, 1.0);
                e->voice.colour = s.colour;
            }
        return s.id;
    }

    void nativeOff (int id, double release)
    {
        const auto it = std::find_if (sounding.begin(), sounding.end(), [id] (const Sounding& s) { return s.id == id; });
        if (it == sounding.end())
            return;
        if (it->audible)
            if (auto* e = emit())
            {
                e->type = Event::Type::off;
                e->output = it->output;
                e->id = id;
                e->voice.key = it->key;
                e->voice.colour = it->colour;
                e->voice.release = (float) std::clamp (release, 0.0, 1.0);
            }
        sounding.erase (it);
        activeOutputs.store ((int) sounding.size());
    }

    void nativeProp (int id, int which, double value)
    {
        const auto it = std::find_if (sounding.begin(), sounding.end(), [id] (const Sounding& s) { return s.id == id; });
        if (it == sounding.end() || ! it->audible)
            return;
        if (auto* e = emit())
        {
            e->type = Event::Type::prop;
            e->output = it->output;
            e->id = id;
            e->voice.key = it->key;
            e->voice.colour = it->colour;
            switch (which)
            {
                case 0: e->prop = Prop::velocity; e->value = (float) std::clamp (value, 0.0, 1.0); break;
                case 1: e->prop = Prop::pitch; e->value = (float) ((value - it->key) * 100.0); break;
                case 2: e->prop = Prop::pan; e->value = (float) std::clamp (value, -1.0, 1.0); break;
                case 3: e->prop = Prop::modX; e->value = (float) std::clamp ((value + 1.0) * 0.5, 0.0, 1.0); break;
                default: e->prop = Prop::modY; e->value = (float) std::clamp ((value + 1.0) * 0.5, 0.0, 1.0); break;
            }
        }
    }

    // the controls, as the script reads / sets them
    bool nativeGet (int k)
    {
        const auto* info = compiledInfo.get();
        if (info == nullptr || k < 0 || k >= (int) info->controls.size() || k >= maxControls)
        {
            py_newint (py_retval(), 0);
            return true;
        }
        const auto& c = info->controls[(std::size_t) k];
        const auto v = numbers[(std::size_t) k].load();
        switch (c.type)
        {
            case Control::Type::knobInt:
            case Control::Type::checkbox:
            case Control::Type::combo: py_newint (py_retval(), (py_i64) std::llround (v)); break;
            case Control::Type::text:
            {
                std::lock_guard<std::mutex> l (textLock);
                py_newstr (py_retval(), (std::size_t) k < texts.size() ? texts[(std::size_t) k].c_str() : "");
                break;
            }
            default: py_newfloat (py_retval(), v); break;
        }
        return true;
    }

    void nativeSet (int k, double v01)
    {
        const auto* info = compiledInfo.get();
        if (info == nullptr || k < 0 || k >= (int) info->controls.size() || k >= maxControls)
            return;
        const auto& c = info->controls[(std::size_t) k];
        if (! c.numeric())
            return;
        const auto v = c.fromNormalized (v01);
        if (numbers[(std::size_t) k].exchange (v) != v)
            scriptChanged = true;
    }

    std::map<std::string, double> extras; // values of names the script does not declare (Python thread)

    void setOutput (int k, double v)
    {
        if (k < 0 || k >= maxOutputs)
            return;
        outputs[(std::size_t) k].store ((float) std::clamp (std::isfinite (v) ? v : 0.0, 0.0, 1.0), std::memory_order_relaxed);
        outputCount.store (std::max (outputCount.load(), k + 1));
    }

    void appendLog (const std::string& text)
    {
        std::lock_guard<std::mutex> l (logLock);
        logText += text;
        if (logText.size() > (std::size_t) logCap)
            logText.erase (0, logText.size() - (std::size_t) logCap / 2);
        ++logCounter;
    }

    void setStatus (const std::string& text)
    {
        std::lock_guard<std::mutex> l (logLock);
        statusText = text;
        ++logCounter;
    }

    std::shared_ptr<const Info> currentInfo() const
    {
        std::lock_guard<std::mutex> l (infoLock);
        return publishedInfo;
    }

private:
    void compileOnPythonThread (const ScriptSettings& settings, bool keepValues);
    void run (Job& j) noexcept;
    bool call (const char* callback, int argc, py_TValue* argv);
    bool callGlue (const char* fn, int argc, py_TValue* argv);
    void callFailed (const std::string& error);

    std::unique_ptr<std::array<Job, ringSize>> ring;
    std::int64_t writeIndex = 0, readIndex = 0; // (audio thread)
    std::int64_t serveIndex = 0;                // (Python thread)
    std::array<Event, maxIn> carry {};
    int carryCount = 0;
    std::atomic<bool> releaseRequested { false };

    std::uint32_t tag = 0;
    bool started = false;
    std::string source;
    ScriptSettings pendingValues;
    Values stateValues;

    int vm = 0;
    bool ok = false, stopped = false;
    std::shared_ptr<Info> compiledInfo;               // (Python thread; read by the natives)
    std::atomic<const Info*> infoForAudio { nullptr }; // (kept alive by `keepInfo`)
    std::vector<std::shared_ptr<Info>> keepInfo;
    mutable std::mutex infoLock;
    std::shared_ptr<const Info> publishedInfo;

    std::array<std::atomic<double>, maxControls> numbers {};
    mutable std::mutex textLock;
    std::vector<std::string> texts;
    std::atomic<bool> scriptChanged { false };
    std::array<std::atomic<float>, maxOutputs> outputs {};
    std::atomic<int> outputCount { 0 };
    std::atomic<bool> wantsTicks { false };
    std::atomic<int> activeOutputs { 0 };

    mutable std::mutex logLock;
    std::string logText, statusText;
    std::atomic<std::uint64_t> logCounter { 0 };
    std::string lastError;
    int repeats = 0;

    // the job being run (Python thread)
    Job* job = nullptr;
    int offset = 0;
    int nextId = 0;
    double freeTicks = 0.0;
    bool wasPlaying = false;
    bool hasTick = false, hasTrigger = false, hasRelease = false;
};

// --- _lynxvfx (native) ---------------------------------------------------------------------------------------------

bool nOn (int argc, py_StackRef argv)
{
    PY_CHECK_ARGC (9);
    double d[7] = {};
    for (int i = 0; i < 7; ++i)
        if (! py_castfloat (py_arg (i), &d[i]))
            return false;
    PY_CHECK_ARG_TYPE (7, tp_int);
    PY_CHECK_ARG_TYPE (8, tp_int);
    auto* rt = Runtime::current();
    const auto id = rt != nullptr ? rt->nativeOn (d[0], d[1], d[2], d[3], d[4], d[5], d[6], (int) py_toint (py_arg (7)), (int) py_toint (py_arg (8))) : 1;
    py_newint (py_retval(), id);
    return true;
}

bool nOff (int argc, py_StackRef argv)
{
    PY_CHECK_ARGC (2);
    PY_CHECK_ARG_TYPE (0, tp_int);
    double rel = 0.5;
    if (! py_castfloat (py_arg (1), &rel))
        return false;
    if (auto* rt = Runtime::current())
        rt->nativeOff ((int) py_toint (py_arg (0)), rel);
    py_newnone (py_retval());
    return true;
}

bool nProp (int argc, py_StackRef argv)
{
    PY_CHECK_ARGC (3);
    PY_CHECK_ARG_TYPE (0, tp_int);
    PY_CHECK_ARG_TYPE (1, tp_int);
    double v = 0.0;
    if (! py_castfloat (py_arg (2), &v))
        return false;
    if (auto* rt = Runtime::current())
        rt->nativeProp ((int) py_toint (py_arg (0)), (int) py_toint (py_arg (1)), v);
    py_newnone (py_retval());
    return true;
}

bool nCtl (int argc, py_StackRef argv)
{
    PY_CHECK_ARGC (2);
    PY_CHECK_ARG_TYPE (0, tp_int);
    double v = 0.0;
    if (! py_castfloat (py_arg (1), &v))
        return false;
    if (auto* rt = Runtime::current())
        rt->setOutput ((int) py_toint (py_arg (0)), v);
    py_newnone (py_retval());
    return true;
}

bool nGet (int argc, py_StackRef argv)
{
    PY_CHECK_ARGC (1);
    PY_CHECK_ARG_TYPE (0, tp_int);
    if (auto* rt = Runtime::current())
        return rt->nativeGet ((int) py_toint (py_arg (0)));
    py_newint (py_retval(), 0);
    return true;
}

bool nSet (int argc, py_StackRef argv)
{
    PY_CHECK_ARGC (2);
    PY_CHECK_ARG_TYPE (0, tp_int);
    double v = 0.0;
    if (! py_castfloat (py_arg (1), &v))
        return false;
    if (auto* rt = Runtime::current())
        rt->nativeSet ((int) py_toint (py_arg (0)), v);
    py_newnone (py_retval());
    return true;
}

bool nExtra (int argc, py_StackRef argv)
{
    PY_CHECK_ARGC (1);
    PY_CHECK_ARG_TYPE (0, tp_str);
    double v = 0.0;
    if (auto* rt = Runtime::current())
        if (const auto it = rt->extras.find (py_tostr (py_arg (0))); it != rt->extras.end())
            v = it->second;
    py_newfloat (py_retval(), v);
    return true;
}

bool nSetExtra (int argc, py_StackRef argv)
{
    PY_CHECK_ARGC (2);
    PY_CHECK_ARG_TYPE (0, tp_str);
    double v = 0.0;
    if (! py_castfloat (py_arg (1), &v))
        return false;
    if (auto* rt = Runtime::current())
        rt->extras[py_tostr (py_arg (0))] = std::clamp (v, 0.0, 1.0);
    py_newnone (py_retval());
    return true;
}

// Makes the current VM ready for a script: _lynxvfx + flvfx. False (error text) if flvfx failed (never expected).
bool installFlvfx (std::string& error)
{
    py_GlobalRef host = py_newmodule ("_lynxvfx");
    py_bindfunc (host, "on", nOn);
    py_bindfunc (host, "off", nOff);
    py_bindfunc (host, "prop", nProp);
    py_bindfunc (host, "ctl", nCtl);
    py_bindfunc (host, "get", nGet);
    py_bindfunc (host, "set", nSet);
    py_bindfunc (host, "extra", nExtra);
    py_bindfunc (host, "setextra", nSetExtra);
    if (! py_exec (flvfxSource, "flvfx", EXEC_MODE, py_newmodule ("flvfx")))
    {
        error = python::takeError();
        return false;
    }
    return true;
}

std::string strOf (py_Ref v)
{
    if (py_isstr (v))
        return py_tostr (v);
    if (py_isnone (v))
        return {};
    if (! py_str (v))
    {
        python::takeError();
        return {};
    }
    return py_tostr (py_retval());
}

double numOf (py_Ref v)
{
    double d = 0.0;
    if (py_isbool (v))
        return py_tobool (v) ? 1.0 : 0.0;
    if (! py_castfloat (v, &d))
    {
        python::takeError();
        return 0.0;
    }
    return d;
}

// Source text the way FL keeps it (CRLF) → what the interpreter reads
std::string normalizedSource (const std::string& source)
{
    std::string s;
    s.reserve (source.size() + 1);
    for (std::size_t i = 0; i < source.size(); ++i)
    {
        if (source[i] == '\r')
        {
            if (i + 1 < source.size() && source[i + 1] == '\n')
                continue;
            s += '\n';
            continue;
        }
        s += source[i];
    }
    if (s.rfind ("\xEF\xBB\xBF", 0) == 0)
        s.erase (0, 3);
    for (auto& c : s) // (FL's editor may keep tabs; Python takes them as indentation either way)
        if (c == '\v' || c == '\f')
            c = ' ';
    s += '\n';
    return s;
}

// Runs the script in the current VM and reads what it declared (Python thread). `info.compiled` false: error.
void compileInto (const std::string& source, Info& info, std::function<void (const std::string&)> print)
{
    python::setPrint (std::move (print));
    std::string error;
    if (! installFlvfx (error))
    {
        info.error = error;
        return;
    }
    python::setTimeLimit (3000.0);
    const auto text = normalizedSource (source);
    if (! py_exec (text.c_str(), "<script>", EXEC_MODE, nullptr))
    {
        info.error = python::takeError();
        python::setTimeLimit (0.0);
        return;
    }
    info.hasTick = py_getglobal (py_name ("onTick")) != nullptr;
    info.hasTrigger = py_getglobal (py_name ("onTriggerVoice")) != nullptr;
    info.hasRelease = py_getglobal (py_name ("onReleaseVoice")) != nullptr;
    py_GlobalRef module = py_getmodule ("flvfx");
    if (auto* create = py_getglobal (py_name ("createDialog")))
    {
        info.hasDialog = true;
        py_TValue f = *create;
        if (! py_call (&f, 0, nullptr))
        {
            info.error = python::takeError();
            python::setTimeLimit (0.0);
            return;
        }
        py_TValue form = *py_retval();
        // vfx.context.form = form
        if (auto* ctx = py_getdict (module, py_name ("context")))
        {
            py_TValue c = *ctx;
            if (! py_setattr (&c, py_name ("form"), &form))
                python::takeError();
        }
        if (! py_getattr (&form, py_name ("_controls")))
        {
            python::takeError();
            info.error = "createDialog() must return a vfx.ScriptDialog";
            python::setTimeLimit (0.0);
            return;
        }
        py_TValue list = *py_retval();
        if (py_getattr (&form, py_name ("_title")))
            info.title = strOf (py_retval());
        else
            python::takeError();
        if (py_getattr (&form, py_name ("_description")))
            info.description = strOf (py_retval());
        else
            python::takeError();
        if (py_islist (&list))
        {
            const auto count = py_list_len (&list);
            for (int i = 0; i < count; ++i)
            {
                py_TValue item = *py_list_getitem (&list, i);
                if (! py_istuple (&item) || py_tuple_len (&item) < 9)
                    continue;
                Control c;
                const auto type = (int) numOf (py_tuple_getitem (&item, 0));
                c.type = (Control::Type) std::clamp (type, 0, 6);
                c.name = strOf (py_tuple_getitem (&item, 1));
                c.group = strOf (py_tuple_getitem (&item, 2));
                c.label = strOf (py_tuple_getitem (&item, 3));
                if (c.type == Control::Type::text)
                    c.defaultText = strOf (py_tuple_getitem (&item, 4));
                else
                    c.defaultValue = numOf (py_tuple_getitem (&item, 4));
                c.minimum = numOf (py_tuple_getitem (&item, 5));
                c.maximum = numOf (py_tuple_getitem (&item, 6));
                py_TValue options = *py_tuple_getitem (&item, 7);
                if (py_islist (&options))
                    for (int k = 0; k < py_list_len (&options); ++k)
                        c.options.push_back (strOf (py_list_getitem (&options, k)));
                c.hint = strOf (py_tuple_getitem (&item, 8));
                if (c.type == Control::Type::surface) // (the embedded surface itself: its controls come from the state)
                    continue;
                if (info.index (c.name) >= 0)
                    continue; // (FL keeps the first control of a name)
                info.controls.push_back (std::move (c));
            }
        }
    }
    if (auto* outs = py_getdict (module, py_name ("_outputs")))
    {
        py_TValue list = *outs;
        if (py_islist (&list))
            for (int i = 0; i < py_list_len (&list); ++i)
            {
                py_TValue item = *py_list_getitem (&list, i);
                if (py_istuple (&item) && py_tuple_len (&item) >= 2)
                {
                    info.outputs.push_back (strOf (py_tuple_getitem (&item, 0)));
                    info.outputDefaults.push_back (numOf (py_tuple_getitem (&item, 1)));
                }
            }
    }
    python::setTimeLimit (0.0);
    info.compiled = true;
}

void Runtime::compileOnPythonThread (const ScriptSettings& settings, bool keepValues)
{
    // the voices of the previous run end
    for (const auto& s : sounding)
        if (s.audible)
        {
            Event e;
            e.type = Event::Type::off;
            e.output = s.output;
            e.id = s.id;
            e.voice.key = s.key;
            e.voice.colour = s.colour;
            pendingOffs.push_back (e);
        }
    sounding.clear();
    activeOutputs = 0;
    extras.clear();

    const auto previous = liveValues();
    const auto previousInfo = currentInfo();
    if (vm > 0)
        python::releaseVm (vm);
    vm = python::acquireVm();
    ok = false;
    stopped = false;
    lastError.clear();
    auto info = std::make_shared<Info>();
    if (vm < 0)
    {
        vm = 0;
        info->error = "Too many VFX Scripts are running (LYNX runs 15 at most).";
        setStatus (info->error);
        appendLog (info->error + "\n");
    }
    else
    {
        py_setvmctx (this);
        compiledInfo = info; // (the natives read it while the script runs its top level)
        std::fill (outputs.begin(), outputs.end(), 0.0f);
        outputCount = 0;
        compileInto (source, *info, [this] (const std::string& t) { appendLog (t); });
        if (! info->compiled)
        {
            setStatus (info->error);
            appendLog (info->error + "\n");
        }
        else
            setStatus ({});
    }
    // the controls' values: the state's (a new script) or the ones it had (restart)
    auto full = std::make_shared<Info> (withSurfaceControls (*info, settings));
    auto values = valuesOf (*full, settings);
    if (keepValues && previousInfo != nullptr)
        for (std::size_t k = 0; k < full->controls.size(); ++k)
        {
            const auto old = previousInfo->index (full->controls[k].name);
            if (old >= 0 && (std::size_t) old < previous.numbers.size())
            {
                values.numbers[k] = previous.numbers[(std::size_t) old];
                values.texts[k] = previous.texts[(std::size_t) old];
            }
        }
    for (std::size_t k = 0; k < values.numbers.size() && k < (std::size_t) maxControls; ++k)
        numbers[k].store (values.numbers[k]);
    {
        std::lock_guard<std::mutex> l (textLock);
        texts = values.texts;
    }
    for (std::size_t k = info->controls.size(); k < full->controls.size(); ++k) // (surface values by name)
        extras[full->controls[k].name] = values.numbers[k];
    stateValues = valuesOf (*full, settings);
    compiledInfo = full;
    keepInfo.push_back (full);
    if (keepInfo.size() > 8) // (the audio thread may still hold an older pointer for a moment; 8 compiles back is plenty)
        keepInfo.erase (keepInfo.begin());
    infoForAudio.store (full.get(), std::memory_order_release);
    {
        std::lock_guard<std::mutex> l (infoLock);
        publishedInfo = full;
    }
    hasTick = info->hasTick;
    hasTrigger = info->hasTrigger;
    hasRelease = info->hasRelease;
    ok = info->compiled;
    wantsTicks = ok && hasTick;
    if (ok && py_getmodule ("flvfx") != nullptr)
    {
        // (the outputs' defaults were set by addOutputController)
    }
    python::use (0);
}

bool Runtime::callGlue (const char* fn, int argc, py_TValue* argv)
{
    py_GlobalRef module = py_getmodule ("flvfx");
    auto* f = module != nullptr ? py_getdict (module, py_name (fn)) : nullptr;
    if (f == nullptr)
        return false;
    py_TValue fv = *f;
    if (py_call (&fv, argc, argv))
        return true;
    callFailed (python::takeError());
    return false;
}

bool Runtime::call (const char* callback, int argc, py_TValue* argv)
{
    auto* f = py_getglobal (py_name (callback));
    if (f == nullptr)
        return true;
    py_TValue fv = *f;
    if (py_call (&fv, argc, argv))
        return true;
    callFailed (python::takeError());
    return false;
}

void Runtime::callFailed (const std::string& error)
{
    if (error.rfind ("MemoryError: the script used more than", 0) == 0)
    {
        stopped = true;
        const auto why = "Stopped: " + error.substr (13) + " (Restart runs it again).";
        setStatus (why);
        appendLog (why + "\n");
        return;
    }
    if (error.find ("TimeoutError") != std::string::npos)
    {
        stopped = true;
        const auto why = std::string ("Stopped: the script took too long (it must answer within the audio block).\n") + error;
        setStatus (why);
        appendLog (why + "\n");
        return;
    }
    // (the same error every tick is logged once, then counted)
    if (error == lastError)
    {
        if (++repeats == 1 || repeats % 1000 == 0)
            appendLog ("(the same error again: " + std::to_string (repeats + 1) + " times)\n");
        return;
    }
    lastError = error;
    repeats = 0;
    appendLog (error + "\n");
}

void Runtime::run (Job& j) noexcept
{
    job = &j;
    offset = 0;
    for (const auto& e : pendingOffs)
        if (j.outCount < maxOut)
            j.out[(std::size_t) j.outCount++] = e;
    pendingOffs.clear();
    if (! ok || stopped || vm <= 0)
    {
        job = nullptr;
        return;
    }
    python::use (vm);
    python::setTimeLimit (realtimeFlag.load() ? 250.0 : 3000.0);
    auto& t = j.transport;
    const auto bpm = std::max (1.0, t.bpm);
    const auto spt = t.sampleRate * 60.0 / (bpm * ppq); // samples per script tick
    {
        py_TValue a[3];
        py_newint (&a[0], ppq);
        py_newfloat (&a[1], bpm);
        py_newbool (&a[2], t.playing);
        callGlue ("_set", 3, a);
    }
    if (j.releaseAll)
    {
        offset = 0;
        callGlue ("_clear", 0, nullptr);
    }

    // the block's ticks: the song's while it plays, a free clock while it is stopped
    double first = 0.0;
    double songTicks = t.ticks * ppq / 960.0;
    if (t.playing)
    {
        first = songTicks;
        wasPlaying = true;
    }
    else
    {
        if (wasPlaying)
            freeTicks = 0.0;
        wasPlaying = false;
        first = freeTicks;
        freeTicks += j.n / spt;
    }
    auto k = (std::int64_t) std::ceil (first - 1e-7);
    std::size_t e = 0;
    auto doEvent = [&] (const Event& ev) {
        offset = ev.offset;
        py_TValue a[9];
        switch (ev.type)
        {
            case Event::Type::on:
            {
                py_newint (&a[0], ev.id);
                py_newfloat (&a[1], (double) ev.voice.key);
                py_newfloat (&a[2], ev.voice.cents / 100.0);
                py_newfloat (&a[3], ev.voice.velocity);
                py_newfloat (&a[4], ev.voice.release);
                py_newfloat (&a[5], ev.voice.pan);
                py_newfloat (&a[6], ev.voice.modX * 2.0 - 1.0);
                py_newfloat (&a[7], ev.voice.modY * 2.0 - 1.0);
                py_newint (&a[8], ev.voice.colour);
                if (! callGlue ("_in_on", 9, a))
                    return;
                py_TValue v = *py_retval();
                if (hasTrigger)
                    call ("onTriggerVoice", 1, &v);
                break;
            }
            case Event::Type::off:
            {
                py_newint (&a[0], ev.id);
                py_newfloat (&a[1], ev.voice.release);
                if (! callGlue ("_in_off", 2, a))
                    return;
                py_TValue v = *py_retval();
                if (! py_isnone (&v) && hasRelease)
                    call ("onReleaseVoice", 1, &v);
                break;
            }
            case Event::Type::prop:
            {
                py_newint (&a[0], ev.id);
                const auto which = ev.prop == Prop::velocity ? 0 : ev.prop == Prop::pitch ? 1 : ev.prop == Prop::pan ? 2 : ev.prop == Prop::modX ? 3 : 4;
                py_newint (&a[1], which);
                py_newfloat (&a[2], ev.prop == Prop::pitch ? ev.value / 100.0 : ev.prop == Prop::modX || ev.prop == Prop::modY ? ev.value * 2.0 - 1.0 : ev.value);
                callGlue ("_in_prop", 3, a);
                return;
            }
        }
        if (! stopped)
            callGlue ("_sync", 0, nullptr);
    };
    for (;; ++k)
    {
        const auto at = ((double) k - first) * spt; // the tick's sample in the block
        if (at >= j.n)
            break;
        const auto sample = (int) std::lround (at);
        // the notes up to the tick's sample come first (a note on a beat is held when that beat's tick runs)
        while (e < (std::size_t) j.inCount && j.in[e].offset <= sample && ! stopped)
            doEvent (j.in[e++]);
        if (stopped)
            break;
        offset = sample;
        py_TValue a[1];
        py_newint (&a[0], t.playing ? (py_i64) k : (py_i64) std::floor (songTicks));
        callGlue ("_tick", 1, a);
        if (hasTick && ! stopped)
            call ("onTick", 0, nullptr);
        if (! stopped)
            callGlue ("_sync", 0, nullptr);
    }
    while (e < (std::size_t) j.inCount && ! stopped)
        doEvent (j.in[e++]);
    python::setTimeLimit (0.0);
    python::use (0);
    job = nullptr;
}

// ---------------------------------------------------------------------------------------------------------------

class ScriptProcessor final : public Processor
{
public:
    explicit ScriptProcessor (const std::vector<std::uint8_t>& fl) : runtime (std::make_shared<Runtime>()) { configure (fl); }

    Kind kind() const noexcept override { return Kind::script; }
    void releaseAll (EventList&) noexcept override { runtime->requestReleaseAll(); }
    void process (const Event* in, int count, int n, const Transport& transport, EventList& out) noexcept override
    {
        runtime->process (in, count, n, transport, out);
    }
    bool busy() const noexcept override { return runtime->busy(); }
    bool configure (const std::vector<std::uint8_t>& flState) override
    {
        const auto s = parseScript (flState);
        if (! s)
            return false;
        runtime->configure (*s);
        return true;
    }
    float controlOutput (int output) const noexcept override { return runtime->output (output); }
    void setParameter (int index, float value) noexcept override { runtime->setNormalized (index, value); }
    void setTag (std::uint32_t tag) override { runtime->setTag (tag); }

private:
    std::shared_ptr<Runtime> runtime;
};

// --- describe(): the declarations, compiled once per source ----------------------------------------------------------
std::mutex describeLock;
std::map<std::string, std::shared_ptr<const Info>> described;
} // namespace

// ---------------------------------------------------------------------------------------------------------------

double Control::fromNormalized (double v01) const noexcept
{
    v01 = std::clamp (std::isfinite (v01) ? v01 : 0.0, 0.0, 1.0);
    switch (type)
    {
        case Type::knob: return minimum + (maximum - minimum) * v01;
        case Type::knobInt:
        case Type::combo: return std::round (minimum + (maximum - minimum) * v01);
        case Type::checkbox: return v01 >= 0.5 ? 1.0 : 0.0;
        case Type::value:
        case Type::surface: return v01;
        case Type::text: return 0.0;
    }
    return v01;
}

double Control::toNormalized (double value) const noexcept
{
    if (type == Type::text)
        return 0.0;
    if (type == Type::checkbox)
        return value >= 0.5 ? 1.0 : 0.0;
    if (maximum <= minimum)
        return 0.0;
    return std::clamp ((value - minimum) / (maximum - minimum), 0.0, 1.0);
}

double Control::clampValue (double value) const noexcept
{
    if (! std::isfinite (value))
        value = defaultValue;
    switch (type)
    {
        case Type::knob: return std::clamp (value, std::min (minimum, maximum), std::max (minimum, maximum));
        case Type::knobInt:
        case Type::combo: return std::clamp (std::round (value), std::min (minimum, maximum), std::max (minimum, maximum));
        case Type::checkbox: return value >= 0.5 ? 1.0 : 0.0;
        case Type::value:
        case Type::surface: return std::clamp (value, 0.0, 1.0);
        case Type::text: return 0.0;
    }
    return value;
}

int Info::index (const std::string& controlName) const noexcept
{
    for (std::size_t i = 0; i < controls.size(); ++i)
        if (controls[i].name == controlName)
            return (int) i;
    return -1;
}

std::shared_ptr<const Info> describe (const std::string& source)
{
    {
        std::lock_guard<std::mutex> l (describeLock);
        if (const auto it = described.find (source); it != described.end())
            return it->second;
    }
    auto info = std::make_shared<Info>();
    python::runSync ([&] {
        const auto vm = python::acquireVm(); // (a scratch VM, given back)
        if (vm < 0)
        {
            info->error = "Too many VFX Scripts are running (LYNX runs 15 at most).";
            return;
        }
        py_setvmctx (nullptr);
        compileInto (source, *info, [info] (const std::string& t) { info->log += t; });
        python::releaseVm (vm);
    });
    std::lock_guard<std::mutex> l (describeLock);
    if (described.size() > 64)
        described.clear();
    described[source] = info;
    return info;
}

std::shared_ptr<const Info> describe (const ScriptSettings& settings) { return describe (settings.source()); }

Values defaults (const Info& info)
{
    Values v;
    for (const auto& c : info.controls)
    {
        v.numbers.push_back (c.type == Control::Type::text ? 0.0 : c.clampValue (c.defaultValue));
        v.texts.push_back (c.type == Control::Type::text ? c.defaultText : std::string());
    }
    return v;
}

double decodeValue (const Control& c, const std::string& stored)
{
    switch (c.type)
    {
        case Control::Type::knob:
        case Control::Type::value:
        {
            const auto raw = parseDouble (stored, std::nan (""));
            if (! std::isfinite (raw))
                return c.clampValue (c.defaultValue);
            const auto lo = c.type == Control::Type::value ? 0.0 : c.minimum, hi = c.type == Control::Type::value ? 1.0 : c.maximum;
            return c.clampValue (lo + (hi - lo) * raw / 262144.0);
        }
        case Control::Type::surface: return std::clamp ((double) floatOfBits ((std::int64_t) parseDouble (stored, 0.0)), 0.0, 1.0);
        case Control::Type::text: return 0.0;
        default: return c.clampValue (parseDouble (stored, c.defaultValue));
    }
}

std::string encodeValue (const Control& c, double value, const std::string& text)
{
    switch (c.type)
    {
        case Control::Type::knob:
        case Control::Type::value:
        {
            const auto lo = c.type == Control::Type::value ? 0.0 : c.minimum, hi = c.type == Control::Type::value ? 1.0 : c.maximum;
            const auto n = hi > lo ? (value - lo) / (hi - lo) : 0.0;
            return std::to_string ((long long) std::llround (std::clamp (n, 0.0, 1.0) * 262144.0));
        }
        case Control::Type::surface: return std::to_string ((long long) bitsOfFloat ((float) std::clamp (value, 0.0, 1.0)));
        case Control::Type::text: return text;
        default: return std::to_string ((long long) std::llround (value));
    }
}

Values valuesOf (const Info& info, const ScriptSettings& settings)
{
    auto v = defaults (info);
    const auto params = settings.parameters();
    for (std::size_t k = 0; k < info.controls.size(); ++k)
    {
        const auto& c = info.controls[k];
        for (const auto& [name, stored] : params)
            if (name == c.name)
            {
                if (c.type == Control::Type::text)
                    v.texts[k] = stored;
                else
                    v.numbers[k] = decodeValue (c, stored);
                break;
            }
    }
    return v;
}

Info withSurfaceControls (const Info& info, const ScriptSettings& settings)
{
    auto out = info;
    if (! settings.hasSurface())
        return out;
    for (const auto& [name, stored] : settings.parameters())
        if (out.index (name) < 0)
        {
            Control c;
            c.type = Control::Type::surface;
            c.name = name;
            c.group = "Surface";
            c.label = name;
            c.defaultValue = 0.0;
            out.controls.push_back (c);
        }
    return out;
}

void writeValues (ScriptSettings& settings, const Info& info, const Values& values)
{
    // [Parameters]: every control in creation order, then the lines of names the script does not declare
    std::vector<std::pair<std::string, std::string>> lines;
    for (std::size_t k = 0; k < info.controls.size() && k < values.numbers.size(); ++k)
        lines.push_back ({ info.controls[k].name, encodeValue (info.controls[k], values.numbers[k], k < values.texts.size() ? values.texts[k] : std::string()) });
    for (const auto& [name, stored] : settings.parameters())
        if (info.index (name) < 0)
            lines.push_back ({ name, stored });
    settings.setParameters (lines);
}

std::shared_ptr<Monitor> monitorOf (std::uint32_t tag)
{
    std::lock_guard<std::mutex> l (registryLock);
    if (const auto it = registry.find (tag); it != registry.end())
        return it->second.lock();
    return nullptr;
}

std::unique_ptr<Processor> makeProcessor (const std::vector<std::uint8_t>& flState) { return std::make_unique<ScriptProcessor> (flState); }

void setRealtime (bool on) noexcept { realtimeFlag.store (on); }

const char* apiReference() noexcept
{
    return R"(VFX Script runs a Python script on the notes (voices) that reach it. LYNX runs it with its built-in Python
(pocketpy): the language and the modules math, random, re, time, sys; scripts cannot open files.

import flvfx as vfx

vfx.context            the host's state (read only)
  .ticks               song position in ticks (PPQ ticks per quarter note)
  .PPQ                 ticks per quarter note
  .isPlaying           True while the song plays
  .tempo               tempo in BPM
  .voices              the voices this script triggered and has not released yet
  .form                the ScriptDialog createDialog() returned

vfx.Voice()            a new voice;  vfx.Voice(other) copies another voice
  .note                pitch as a MIDI note number (60 = C5 in FL's naming); fractions are microtuning
  .finePitch           extra pitch, in notes
  .velocity            0..1
  .releaseVelocity     0..1
  .pan                 -1 (left) .. 1 (right)
  .fcut / .fres        Mod X / Mod Y, -1..1
  .color               note colour 0..15 (MIDI channel)
  .length              ticks; above 0 the voice releases itself after that many ticks
  .output              voice output 0.. (Patcher pins "Voice output 1"…)
  .trigger()           starts the voice (an incoming voice can't be triggered: copy it)
  .release()           ends it
  .copyFrom(other)     takes all the properties of another voice (follow slides)
  Changing a triggered voice's note, finePitch, velocity, pan, fcut or fres changes the sounding note.

vfx.ScriptDialog(title, description)
  .addInputKnob(name, default, min, max, hint)
  .addInputKnobInt(name, default, min, max, hint)
  .addInputCheckbox(name, default, hint)
  .addInputCombo(name, options, defaultIndex, hint)     options: a list or "a,b,c"
  .addInputText(name, default, hint)
  .addInput(name, default, hint)
  .addInputSurface(name)
  .addGroup(name) / .endGroup()                       controls inside are named "Group: Name"
  .getInputValue(name)
  .setNormalizedValue(name, value01)

vfx.addOutputController(name, default)   a control output (Patcher: cable it to any parameter)
vfx.setOutputController(name, value01)

Callbacks (all optional):
  createDialog()            builds the controls; returns the ScriptDialog
  onTriggerVoice(voice)     a note arrived (it is not passed on unless the script triggers a copy)
  onReleaseVoice(voice)     that note ended (the same object)
  onTick()                  every tick, playing or not

print() writes to the Log tab.)";
}
} // namespace lynx::vfx::script
