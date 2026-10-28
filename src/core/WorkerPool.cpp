#include "core/WorkerPool.h"

#include <chrono>

#include "core/Denormals.h"

#if defined(_WIN32)
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#elif defined(__APPLE__)
 #include <dispatch/dispatch.h>
#else
 #include <semaphore.h>
#endif

#if defined(__SSE2__) || defined(_M_X64) || defined(__x86_64__)
 #include "core/Simd.h"
#endif

namespace lynx
{
namespace
{
inline void cpuRelax() noexcept
{
#if defined(__SSE2__) || defined(_M_X64) || defined(__x86_64__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile ("yield");
#endif
}
} // namespace

#if defined(_WIN32)
struct WorkerPool::Semaphore
{
    Semaphore() : handle (CreateSemaphoreW (nullptr, 0, 0x7fffffff, nullptr)) {}
    ~Semaphore()
    {
        if (handle != nullptr)
            CloseHandle (handle);
    }
    void release (int n) noexcept { ReleaseSemaphore (handle, n, nullptr); }
    void acquire() noexcept { WaitForSingleObject (handle, INFINITE); }
    HANDLE handle;
};
#elif defined(__APPLE__)
// libdispatch semaphore: a Mach semaphore underneath, woken by the kernel at once
struct WorkerPool::Semaphore
{
    Semaphore() : handle (dispatch_semaphore_create (0)) {}
    ~Semaphore() { dispatch_release (handle); }
    void release (int n) noexcept
    {
        for (int i = 0; i < n; ++i)
            dispatch_semaphore_signal (handle);
    }
    void acquire() noexcept { dispatch_semaphore_wait (handle, DISPATCH_TIME_FOREVER); }
    dispatch_semaphore_t handle;
};
#else
// POSIX semaphore: glibc implements it on a futex (uncontended post/wait stay in user space, a sleeper is woken
// by the kernel at once)
struct WorkerPool::Semaphore
{
    Semaphore() { sem_init (&handle, 0, 0); }
    ~Semaphore() { sem_destroy (&handle); }
    void release (int n) noexcept
    {
        for (int i = 0; i < n; ++i)
            sem_post (&handle);
    }
    void acquire() noexcept
    {
        while (sem_wait (&handle) != 0) {} // EINTR: a signal interrupted the wait
    }
    sem_t handle;
};
#endif

void WorkerPool::relax() noexcept
{
    cpuRelax();
}

WorkerPool::WorkerPool (int n, std::function<void()> onThreadStart) : wake (std::make_unique<Semaphore>())
{
    // ticket 0: count 0, nothing to take until the first parallelFor

    for (int i = 0; i < n; ++i)
        threads.emplace_back ([this, onThreadStart] { workerLoop (onThreadStart); });
}

WorkerPool::~WorkerPool()
{
    quit.store (true);
    wake->release ((int) threads.size());

    for (auto& t : threads)
        t.join();
}

bool WorkerPool::hasWork() const noexcept
{
    constexpr auto mask = (std::uint64_t (1) << indexBits) - 1;
    const auto t = ticket.load (std::memory_order_acquire);
    return (t & mask) < ((t >> indexBits) & mask);
}

bool WorkerPool::isParked() const noexcept
{
    return (ticket.load (std::memory_order_acquire) >> (2 * indexBits)) == parkedGeneration.load (std::memory_order_acquire);
}

void WorkerPool::workerLoop (std::function<void()> onStart)
{
    const ScopedFlushDenormals noDenormals;

    if (onStart)
        onStart();

    using clock = std::chrono::steady_clock;

    while (! quit.load (std::memory_order_relaxed))
    {
        // spin a little: within a block the next parallelFor (next mixer layer or chunk) follows right away
        auto spinStart = clock::now();
        auto found = false;

        for (int i = 0;; ++i)
        {
            if (hasWork())
            {
                found = true;
                break;
            }

            cpuRelax();

            if ((i & 63) == 63)
            {
                if (quit.load (std::memory_order_relaxed))
                    return;
                if (isParked()) // the block is over: sleep now
                    break;
                if (clock::now() - spinStart > std::chrono::microseconds (spinMicros))
                    break;
            }
        }

        if (found)
        {
            work (true);
            continue;
        }

        // sleep. Announce it first, then look once more: run() publishes the ticket before reading `sleeping`,
        // so either it sees this worker asleep and releases the semaphore, or this check sees the work
        sleeping.fetch_add (1, std::memory_order_seq_cst);

        if (hasWork())
        {
            sleeping.fetch_sub (1, std::memory_order_seq_cst);
            work (true);
            continue;
        }

        wake->acquire();
        sleeping.fetch_sub (1, std::memory_order_seq_cst);
        wokenUp.fetch_add (1, std::memory_order_relaxed);

        if (quit.load())
            return;

        work (true);
    }
}

bool WorkerPool::work (bool isWorker) noexcept
{
    constexpr auto mask = (std::uint64_t (1) << indexBits) - 1;
    auto any = false;

    while (true)
    {
        const auto t = ticket.fetch_add (1, std::memory_order_acq_rel);
        const auto i = t & mask, n = (t >> indexBits) & mask;

        if (i >= n)
            return any;

        call (context, (int) i);
        any = true;
        if (isWorker)
            byWorkers.fetch_add (1, std::memory_order_relaxed);
        done.fetch_add (1, std::memory_order_acq_rel);
    }
}

void WorkerPool::run (int n, void* ctx, Call fn) noexcept
{
    if (n <= 0)
        return;

    if (n == 1 || threads.empty() || n >= (1 << indexBits))
    {
        for (int i = 0; i < n; ++i)
            fn (ctx, i);
        return;
    }

    context = ctx;
    call = fn;
    done.store (0, std::memory_order_relaxed);
    ++generation;
    const auto base = generation << (2 * indexBits);
    ticket.store (base | ((std::uint64_t) n << indexBits), std::memory_order_seq_cst); // from here on the workers may take indices

    // wake only the sleepers that are needed (spinning workers already see the ticket)
    if (const auto asleep = sleeping.load (std::memory_order_seq_cst); asleep > 0)
        wake->release (std::min (asleep, n - 1));

    work (false);

    for (int spins = 0; done.load (std::memory_order_acquire) < n; ++spins)
    {
        cpuRelax();
        if ((spins & 1023) == 1023)
            std::this_thread::yield();
    }

    ticket.store (base, std::memory_order_release); // count 0: late workers take nothing
}
} // namespace lynx
