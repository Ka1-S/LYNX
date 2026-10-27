#include "core/Python.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <semaphore>
#include <thread>
#include <vector>

extern "C"
{
#include "pocketpy.h"
extern volatile int py_lynx_interrupt; // (third_party/pocketpy/pocketpy.c, LYNX's addition)
}

namespace lynx::python
{
namespace
{
using Clock = std::chrono::steady_clock;

// --- LYNX's library ----------------------------------------------------------------------------------------------

// What a script written for CPython 3.12 expects and pocketpy lacks (our own code)
constexpr const char* prelude = R"PY(
import builtins, sys, random, math, time

def _pow(base, exp, mod=None):
    if mod is None:
        return base ** exp
    return (base ** exp) % mod
builtins.pow = _pow

class _FloatInfo:
    epsilon = 2.220446049250313e-16
    max = 1.7976931348623157e+308
    min = 2.2250738585072014e-308
    dig = 15
    mant_dig = 53
    radix = 2
sys.float_info = _FloatInfo()
sys.maxsize = 9223372036854775807

_seed_int = random.seed
def _seed(a=None, version=2):
    if a is None:
        a = time.time_ns()
    elif isinstance(a, float):
        a = int(a * 1000003.0) ^ int(a)
    elif isinstance(a, bool):
        a = int(a)
    elif not isinstance(a, int):
        a = hash(a)
    _seed_int(a)
random.seed = _seed

_gauss_spare = [None]
def _gauss(mu=0.0, sigma=1.0):
    z = _gauss_spare[0]
    _gauss_spare[0] = None
    if z is None:
        angle = random.random() * 2.0 * math.pi
        radius = math.sqrt(-2.0 * math.log(1.0 - random.random()))
        z = math.cos(angle) * radius
        _gauss_spare[0] = math.sin(angle) * radius
    return mu + z * sigma
random.gauss = _gauss
random.normalvariate = _gauss

def _randrange(start, stop=None, step=1):
    if stop is None:
        start, stop = 0, start
    n = (stop - start + step - 1) // step if step > 0 else (start - stop - step - 1) // (-step)
    if n <= 0:
        raise ValueError('empty range for randrange()')
    return start + step * random.randint(0, n - 1)
random.randrange = _randrange
)PY";

// `re` on top of one native search (std::regex, ECMAScript syntax after a few Python → ECMAScript rewrites)
constexpr const char* reModule = R"PY(
import _lynxre

I = IGNORECASE = 2
M = MULTILINE = 8
S = DOTALL = 16
X = VERBOSE = 64
A = ASCII = 256

class Match:
    def __init__(self, string, spans, names, re):
        self.string = string
        self._spans = spans
        self._names = names
        self.re = re
        self.pos = 0
    def span(self, g=0):
        return self._spans[self._index(g)]
    def start(self, g=0):
        return self._spans[self._index(g)][0]
    def end(self, g=0):
        return self._spans[self._index(g)][1]
    def _index(self, g):
        if isinstance(g, str):
            return self._names[g]
        return g
    def group(self, *groups):
        if len(groups) == 0:
            groups = (0,)
        out = []
        for g in groups:
            s, e = self._spans[self._index(g)]
            out.append(None if s < 0 else self.string[s:e])
        return out[0] if len(out) == 1 else tuple(out)
    def __getitem__(self, g):
        return self.group(g)
    def groups(self, default=None):
        out = []
        for i in range(1, len(self._spans)):
            s, e = self._spans[i]
            out.append(default if s < 0 else self.string[s:e])
        return tuple(out)
    def groupdict(self, default=None):
        d = {}
        for k in self._names:
            v = self.group(k)
            d[k] = default if v is None else v
        return d
    def expand(self, template):
        return _expand(self, template)
    def __bool__(self):
        return True
    def __repr__(self):
        return "<re.Match object; span=" + str(self.span()) + ", match=" + repr(self.group()) + ">"

def _expand(m, repl):
    out = ''
    i = 0
    n = len(repl)
    while i < n:
        c = repl[i]
        if c == '\\' and i + 1 < n:
            d = repl[i + 1]
            if d.isdigit():
                j = i + 1
                while j < n and j < i + 3 and repl[j].isdigit():
                    j += 1
                v = m.group(int(repl[i + 1:j]))
                out += '' if v is None else v
                i = j
                continue
            if d == 'g' and i + 2 < n and repl[i + 2] == '<':
                j = repl.index('>', i + 3)
                key = repl[i + 3:j]
                v = m.group(int(key) if key.isdigit() else key)
                out += '' if v is None else v
                i = j + 1
                continue
            esc = {'n': '\n', 't': '\t', 'r': '\r', '\\': '\\', 'a': '\a', 'f': '\f', 'v': '\v'}
            if d in esc:
                out += esc[d]
                i += 2
                continue
            out += c + d
            i += 2
            continue
        out += c
        i += 1
    return out

class Pattern:
    def __init__(self, pattern, flags=0):
        self.pattern = pattern
        self.flags = flags
        self._names = _lynxre.names(pattern)
        self.groups = _lynxre.groups(pattern, flags)
        self.groupindex = self._names
    def _find(self, string, pos, mode):
        spans = _lynxre.find(self.pattern, self.flags, string, pos, mode)
        if spans is None:
            return None
        m = Match(string, spans, self._names, self)
        m.pos = pos
        return m
    def search(self, string, pos=0, endpos=None):
        if endpos is not None:
            string = string[:endpos]
        return self._find(string, pos, 0)
    def match(self, string, pos=0, endpos=None):
        if endpos is not None:
            string = string[:endpos]
        return self._find(string, pos, 1)
    def fullmatch(self, string, pos=0, endpos=None):
        if endpos is not None:
            string = string[:endpos]
        return self._find(string, pos, 2)
    def finditer(self, string, pos=0, endpos=None):
        if endpos is not None:
            string = string[:endpos]
        out = []
        n = len(string)
        while pos <= n:
            m = self._find(string, pos, 0)
            if m is None:
                break
            out.append(m)
            s, e = m.span()
            pos = e + 1 if e == s else e
        return out
    def findall(self, string, pos=0, endpos=None):
        out = []
        for m in self.finditer(string, pos, endpos):
            if self.groups == 0:
                out.append(m.group())
            elif self.groups == 1:
                v = m.group(1)
                out.append('' if v is None else v)
            else:
                out.append(m.groups(''))
        return out
    def subn(self, repl, string, count=0):
        out = ''
        last = 0
        done = 0
        for m in self.finditer(string):
            if count and done >= count:
                break
            s, e = m.span()
            out += string[last:s]
            out += repl(m) if callable(repl) else _expand(m, repl)
            last = e
            done += 1
        out += string[last:]
        return (out, done)
    def sub(self, repl, string, count=0):
        return self.subn(repl, string, count)[0]
    def split(self, string, maxsplit=0):
        out = []
        last = 0
        done = 0
        for m in self.finditer(string):
            if maxsplit and done >= maxsplit:
                break
            s, e = m.span()
            out.append(string[last:s])
            for g in m.groups():
                out.append(g)
            last = e
            done += 1
        out.append(string[last:])
        return out

_cache = {}
def compile(pattern, flags=0):
    if isinstance(pattern, Pattern):
        return pattern
    key = (pattern, flags)
    p = _cache.get(key)
    if p is None:
        p = Pattern(pattern, flags)
        _cache[key] = p
    return p

def search(pattern, string, flags=0):
    return compile(pattern, flags).search(string)
def match(pattern, string, flags=0):
    return compile(pattern, flags).match(string)
def fullmatch(pattern, string, flags=0):
    return compile(pattern, flags).fullmatch(string)
def findall(pattern, string, flags=0):
    return compile(pattern, flags).findall(string)
def finditer(pattern, string, flags=0):
    return compile(pattern, flags).finditer(string)
def sub(pattern, repl, string, count=0, flags=0):
    return compile(pattern, flags).sub(repl, string, count)
def subn(pattern, repl, string, count=0, flags=0):
    return compile(pattern, flags).subn(repl, string, count)
def split(pattern, string, maxsplit=0, flags=0):
    return compile(pattern, flags).split(string, maxsplit)
def escape(pattern):
    out = ''
    for c in pattern:
        if c.isalnum() or c == '_' or ord(c) > 127:
            out += c
        else:
            out += '\\' + c
    return out
def purge():
    _cache.clear()

error = ValueError
)PY";

// Python regular expression → ECMAScript (std::regex): named groups become plain groups, \A \Z become ^ $, DOTALL
// turns "." into "[\s\S]"
std::string toEcma (const std::string& p, bool dotAll)
{
    std::string out;
    bool inClass = false;
    for (std::size_t i = 0; i < p.size(); ++i)
    {
        const auto c = p[i];
        if (c == '\\' && i + 1 < p.size())
        {
            const auto d = p[i + 1];
            if (! inClass && d == 'A')
                out += '^';
            else if (! inClass && (d == 'Z' || d == 'z'))
                out += '$';
            else
                out += std::string { c, d };
            ++i;
            continue;
        }
        if (inClass)
        {
            if (c == ']')
                inClass = false;
            out += c;
            continue;
        }
        if (c == '[')
        {
            inClass = true;
            out += c;
            if (i + 1 < p.size() && p[i + 1] == '^')
                out += p[++i];
            if (i + 1 < p.size() && p[i + 1] == ']')
                out += p[++i];
            continue;
        }
        if (c == '(' && p.compare (i, 4, "(?P<") == 0)
        {
            const auto close = p.find ('>', i);
            if (close != std::string::npos)
            {
                out += '(';
                i = close;
                continue;
            }
        }
        if (c == '(' && p.compare (i, 4, "(?P=") == 0)
            throw std::regex_error (std::regex_constants::error_backref);
        if (c == '.' && dotAll)
        {
            out += "[\\s\\S]";
            continue;
        }
        out += c;
    }
    return out;
}

const std::regex& regexFor (const std::string& pattern, int flags)
{
    static std::map<std::pair<std::string, int>, std::unique_ptr<std::regex>> cache; // (Python thread only)
    auto& slot = cache[{ pattern, flags }];
    if (slot == nullptr)
    {
        auto f = std::regex::ECMAScript;
        if ((flags & 2) != 0)
            f |= std::regex::icase;
        if ((flags & 8) != 0)
            f |= std::regex::multiline;
        slot = std::make_unique<std::regex> (toEcma (pattern, (flags & 16) != 0), f);
        if (cache.size() > 256)
        {
            auto keep = std::move (slot);
            cache.clear();
            return *(cache[{ pattern, flags }] = std::move (keep));
        }
    }
    return *slot;
}

// UTF-8 byte offset ↔ character index (Python indexes strings by character)
int charIndex (const char* s, std::size_t bytes)
{
    int n = 0;
    for (std::size_t i = 0; i < bytes; ++i)
        if ((static_cast<unsigned char> (s[i]) & 0xC0) != 0x80)
            ++n;
    return n;
}

std::size_t byteOffset (const char* s, std::size_t size, int chars)
{
    std::size_t i = 0;
    int n = 0;
    for (; i < size; ++i)
        if ((static_cast<unsigned char> (s[i]) & 0xC0) != 0x80)
        {
            if (n == chars)
                return i;
            ++n;
        }
    return size;
}

bool reError (const std::exception& e)
{
    return py_exception (tp_ValueError, "bad regular expression (%s)", e.what());
}

// _lynxre.find(pattern, flags, string, pos, mode): mode 0 search, 1 match (anchored at pos), 2 fullmatch →
// a list of (start, end) per group (−1, −1 = did not take part), or None
bool reFind (int argc, py_StackRef argv)
{
    PY_CHECK_ARGC (5);
    PY_CHECK_ARG_TYPE (0, tp_str);
    PY_CHECK_ARG_TYPE (1, tp_int);
    PY_CHECK_ARG_TYPE (2, tp_str);
    PY_CHECK_ARG_TYPE (3, tp_int);
    PY_CHECK_ARG_TYPE (4, tp_int);
    int size = 0;
    const char* text = py_tostrn (py_arg (2), &size);
    const auto mode = (int) py_toint (py_arg (4));
    const auto start = byteOffset (text, (std::size_t) size, (int) std::max<py_i64> (0, py_toint (py_arg (3))));
    try
    {
        const auto& re = regexFor (py_tostr (py_arg (0)), (int) py_toint (py_arg (1)));
        std::cmatch m;
        auto flags = std::regex_constants::match_default;
        if (start > 0)
            flags |= std::regex_constants::match_prev_avail;
        if (mode >= 1)
            flags |= std::regex_constants::match_continuous;
        const auto found = std::regex_search (text + start, text + size, m, re, flags);
        if (! found || (mode == 2 && m[0].second != text + size))
        {
            py_newnone (py_retval());
            return true;
        }
        py_Ref list = py_pushtmp();
        py_newlist (list);
        for (std::size_t g = 0; g < m.size(); ++g)
        {
            py_Ref item = py_pushtmp();
            auto* pair = py_newtuple (item, 2);
            if (m[g].matched)
            {
                py_newint (&pair[0], charIndex (text, (std::size_t) (m[g].first - text)));
                py_newint (&pair[1], charIndex (text, (std::size_t) (m[g].second - text)));
            }
            else
            {
                py_newint (&pair[0], -1);
                py_newint (&pair[1], -1);
            }
            py_list_append (list, item);
            py_pop();
        }
        py_assign (py_retval(), list);
        py_pop();
        return true;
    }
    catch (const std::exception& e)
    {
        return reError (e);
    }
}

// _lynxre.groups(pattern, flags): the number of groups (raises on a bad pattern)
bool reGroups (int argc, py_StackRef argv)
{
    PY_CHECK_ARGC (2);
    PY_CHECK_ARG_TYPE (0, tp_str);
    PY_CHECK_ARG_TYPE (1, tp_int);
    try
    {
        py_newint (py_retval(), (py_i64) regexFor (py_tostr (py_arg (0)), (int) py_toint (py_arg (1))).mark_count());
        return true;
    }
    catch (const std::exception& e)
    {
        return reError (e);
    }
}

// _lynxre.names(pattern): {name: group number} of the (?P<name>…) groups
bool reNames (int argc, py_StackRef argv)
{
    PY_CHECK_ARGC (1);
    PY_CHECK_ARG_TYPE (0, tp_str);
    const std::string p = py_tostr (py_arg (0));
    py_Ref dict = py_pushtmp();
    py_newdict (dict);
    int group = 0;
    bool inClass = false;
    for (std::size_t i = 0; i < p.size(); ++i)
    {
        if (p[i] == '\\')
        {
            ++i;
            continue;
        }
        if (inClass)
        {
            inClass = p[i] != ']';
            continue;
        }
        if (p[i] == '[')
            inClass = true;
        else if (p[i] == '(' && (i + 1 >= p.size() || p[i + 1] != '?' || p.compare (i, 4, "(?P<") == 0))
        {
            ++group;
            if (p.compare (i, 4, "(?P<") == 0)
            {
                const auto close = p.find ('>', i);
                if (close != std::string::npos)
                {
                    py_Ref key = py_pushtmp();
                    py_newstr (key, p.substr (i + 4, close - i - 4).c_str());
                    py_Ref value = py_pushtmp();
                    py_newint (value, group);
                    if (! py_dict_setitem (dict, key, value))
                        return false;
                    py_shrink (2);
                }
            }
        }
    }
    py_assign (py_retval(), dict);
    py_pop();
    return true;
}

// --- the Python thread ---------------------------------------------------------------------------------------------

struct Host
{
    std::thread thread, supervisor;
    std::counting_semaphore<1 << 20> wakeUp { 0 };
    std::atomic<bool> stopping { false };

    std::mutex jobsLock;
    std::deque<std::function<void()>> jobs;

    std::mutex servicesLock; // held by the Python thread while it serves (remove waits for it)
    std::vector<Service*> services;

    std::atomic<std::int64_t> deadline { 0 }; // steady-clock ns; 0 = no limit running
    std::mutex supervisorLock;
    std::condition_variable supervisorWake;

    std::array<bool, 16> used {};
    std::array<std::function<void (const std::string&)>, 16> prints;
    std::atomic<std::thread::id> pythonThread {};

    Host()
    {
        thread = std::thread ([this] { run(); });
        supervisor = std::thread ([this] { supervise(); });
    }

    ~Host()
    {
        stopping = true;
        wakeUp.release();
        {
            std::lock_guard<std::mutex> l (supervisorLock);
        }
        supervisorWake.notify_all();
        if (thread.joinable())
            thread.join();
        if (supervisor.joinable())
            supervisor.join();
    }

    void run()
    {
        pythonThread = std::this_thread::get_id();
        py_initialize();
        used[0] = true;
        install();
        while (! stopping)
        {
            wakeUp.acquire();
            if (stopping)
                break;
            for (;;)
            {
                std::function<void()> job;
                {
                    std::lock_guard<std::mutex> l (jobsLock);
                    if (jobs.empty())
                        break;
                    job = std::move (jobs.front());
                    jobs.pop_front();
                }
                job();
            }
            std::lock_guard<std::mutex> l (servicesLock);
            for (auto* s : services)
                s->serve();
        }
        // (pocketpy's VMs are left to the process exit: finalizing them here could race static destructors)
    }

    void supervise()
    {
        std::unique_lock<std::mutex> l (supervisorLock);
        while (! stopping)
        {
            supervisorWake.wait_for (l, std::chrono::milliseconds (10));
            const auto d = deadline.load();
            if (d != 0 && Clock::now().time_since_epoch().count() > d)
            {
                py_lynx_interrupt = 1;
                deadline = 0;
            }
        }
    }

    // a fresh VM: LYNX's library, sandboxed imports, print to the VM's own sink
    void install()
    {
        auto* cb = py_callbacks();
        cb->importfile = [] (const char*, int*) -> char* { return nullptr; };
        cb->print = [] (const char* text) {
            auto& p = host().prints[(std::size_t) py_currentvm()];
            if (p)
                p (text);
        };
        cb->flush = [] {};
        cb->getchr = [] { return -1; };
        py_GlobalRef re = py_newmodule ("_lynxre");
        py_bindfunc (re, "find", reFind);
        py_bindfunc (re, "groups", reGroups);
        py_bindfunc (re, "names", reNames);
        setTimeLimit (2000.0);
        if (! py_exec (prelude, "<lynx>", EXEC_MODE, nullptr))
            py_clearexc (nullptr);
        if (! py_exec (reModule, "re", EXEC_MODE, py_newmodule ("re")))
            py_clearexc (nullptr);
        setTimeLimit (0.0);
    }

    static Host& host()
    {
        static Host h;
        return h;
    }
};

Host& host() { return Host::host(); }
} // namespace

void runSync (const std::function<void()>& job)
{
    auto& h = host();
    if (std::this_thread::get_id() == h.pythonThread.load())
    {
        job();
        return;
    }
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    {
        std::lock_guard<std::mutex> l (h.jobsLock);
        h.jobs.push_back ([&] {
            job();
            std::lock_guard<std::mutex> l2 (m);
            done = true;
            cv.notify_all();
        });
    }
    h.wakeUp.release();
    std::unique_lock<std::mutex> l (m);
    cv.wait (l, [&] { return done; });
}

void addService (Service* service)
{
    auto& h = host();
    std::lock_guard<std::mutex> l (h.servicesLock);
    if (std::find (h.services.begin(), h.services.end(), service) == h.services.end())
        h.services.push_back (service);
}

void removeService (Service* service)
{
    auto& h = host();
    std::lock_guard<std::mutex> l (h.servicesLock);
    h.services.erase (std::remove (h.services.begin(), h.services.end(), service), h.services.end());
}

void wake() noexcept { host().wakeUp.release(); }

int acquireVm()
{
    auto& h = host();
    for (int i = 1; i < 16; ++i)
        if (! h.used[(std::size_t) i])
        {
            h.used[(std::size_t) i] = true;
            py_switchvm (i);
            py_resetvm();
            h.prints[(std::size_t) i] = nullptr;
            h.install();
            return i;
        }
    return -1;
}

void releaseVm (int vm)
{
    auto& h = host();
    if (vm <= 0 || vm >= 16 || ! h.used[(std::size_t) vm])
        return;
    py_switchvm (vm);
    py_resetvm(); // (frees the script's objects now)
    h.prints[(std::size_t) vm] = nullptr;
    h.used[(std::size_t) vm] = false;
    py_switchvm (0);
}

void use (int vm) { py_switchvm (std::clamp (vm, 0, 15)); }

void setTimeLimit (double milliseconds) noexcept
{
    auto& h = host();
    if (milliseconds <= 0.0)
    {
        h.deadline = 0;
        py_lynx_interrupt = 0;
        return;
    }
    h.deadline = (Clock::now() + std::chrono::microseconds ((std::int64_t) (milliseconds * 1000.0))).time_since_epoch().count();
    py_lynx_interrupt = 0;
}

std::string takeError()
{
    char* text = py_formatexc();
    std::string out = text != nullptr ? text : "error";
    if (text != nullptr)
        py_free (text);
    py_clearexc (nullptr);
    while (! out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

void setPrint (std::function<void (const std::string&)> print) { host().prints[(std::size_t) py_currentvm()] = std::move (print); }
} // namespace lynx::python
