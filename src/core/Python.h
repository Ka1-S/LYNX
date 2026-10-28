#pragma once

// LYNX's embedded Python (round 165d): the pocketpy interpreter (third_party/pocketpy, MIT), small enough to ship
// inside LYNX, runs the scripts LYNX meets in FL Studio projects (VFX Script) and can run other LYNX scripts.
//
//  - One thread ("the Python thread") runs every script. pocketpy keeps its state in globals, so nothing else may
//    touch it: work reaches that thread through runSync() (UI / tests: wait for it) or through a Service that the
//    audio thread wakes with wake() (lock-free: an atomic and a semaphore).
//  - Up to 15 independent interpreters ("VMs", pocketpy's limit is 16 and VM 0 is LYNX's scratch VM): each script
//    instance takes one, so its globals, modules and random state are its own.
//  - Sandbox: scripts cannot open files, run programs or import modules from disk (only pocketpy's built-in
//    modules and the ones LYNX adds). A script that runs too long is interrupted (TimeoutError), so a bad script
//    never hangs LYNX.
//  - LYNX's additions to pocketpy's standard library, for scripts written for CPython 3.12: `re` (sub, match,
//    search, fullmatch, findall, split, compile, escape), the 3-argument pow(), sys.float_info, random.gauss(),
//    random.seed() with any value.
//
// Using it for other scripts (e.g. Piano Roll scripts): inside runSync(), acquireVm() a VM, install your module with
// pocketpy's C API (pocketpy.h: py_newmodule, py_bindfunc, py_exec…), setTimeLimit() around every call into the
// script, takeError() after a failed call, use (0) + releaseVm() when done. core/VfxScript.cpp is the example.

#include <functional>
#include <string>

namespace lynx::python
{
// Run `job` on the Python thread and wait for it (never from the audio thread). Jobs run one at a time.
void runSync (const std::function<void()>& job);

// Work the audio thread hands over: serve() runs on the Python thread after every wake(). (UI thread: add / remove;
// remove waits until the Python thread is not serving it.)
struct Service
{
    virtual ~Service() = default;
    virtual void serve() noexcept = 0;
};
void addService (Service* service);
void removeService (Service* service);
// Wakes the Python thread (any thread, real-time safe)
void wake() noexcept;

// --- Python thread only ------------------------------------------------------------------------------------------
// A fresh interpreter (LYNX's library installed, sandboxed) and its release; -1 when all are in use
int acquireVm();
void releaseVm (int vm);
// Switch to that interpreter
void use (int vm);
// The time limit of the Python code run from now on (ms; 0 = none): past it the code raises TimeoutError
void setTimeLimit (double milliseconds) noexcept;
// The pending exception as text (traceback, last line = type and message), cleared
std::string takeError();
// Where print() of the current VM goes (per VM; null = nowhere)
void setPrint (std::function<void (const std::string&)> print);
} // namespace lynx::python
