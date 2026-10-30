#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace lynx
{
// One background thread that runs small jobs in the order they were posted: file writes the message thread must not
// wait for. On Windows every JUCE file write ends with FlushFileBuffers, which waits for the disk and took 2 s on a
// busy one (issue #13: "UI not responding" each time the audio device changed, from saving audio-device.xml on the
// message thread). The thread starts with the first job; waitIdle() waits for every job posted so far; the
// destructor runs what is still queued, so nothing posted is lost when LYNX quits. Jobs must capture what they need
// by value (they may run after the poster is gone).
class SerialJobs
{
public:
    SerialJobs() = default;
    SerialJobs (const SerialJobs&) = delete;
    SerialJobs& operator= (const SerialJobs&) = delete;
    ~SerialJobs() { stop(); }

    void post (std::function<void()> job)
    {
        {
            std::unique_lock<std::mutex> lock (mutex);
            if (! stopping)
            {
                queue.push_back (std::move (job));
                if (! worker.joinable())
                    worker = std::thread ([this] { run(); });
                wake.notify_one();
                return;
            }
        }
        runSafely (job); // after stop(): at once, on the caller's thread
    }

    // Blocks until every job posted before this call has run.
    void waitIdle()
    {
        std::unique_lock<std::mutex> lock (mutex);
        idle.wait (lock, [this] { return queue.empty() && ! busy; });
    }

    // Runs the queued jobs, then ends the thread. Later posts run on the caller's thread.
    void stop()
    {
        {
            std::unique_lock<std::mutex> lock (mutex);
            stopping = true;
            wake.notify_all();
        }
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id())
            worker.join();
    }

    bool isIdle()
    {
        std::unique_lock<std::mutex> lock (mutex);
        return queue.empty() && ! busy;
    }

private:
    static void runSafely (const std::function<void()>& job) noexcept
    {
        try
        {
            if (job)
                job();
        }
        catch (...) {}
    }

    void run()
    {
        std::unique_lock<std::mutex> lock (mutex);
        for (;;)
        {
            wake.wait (lock, [this] { return stopping || ! queue.empty(); });
            if (queue.empty())
                return; // stopping, nothing left
            auto job = std::move (queue.front());
            queue.pop_front();
            busy = true;
            lock.unlock();
            runSafely (job);
            lock.lock();
            busy = false;
            if (queue.empty())
                idle.notify_all();
        }
    }

    std::mutex mutex;
    std::condition_variable wake, idle;
    std::deque<std::function<void()>> queue;
    std::thread worker;
    bool busy = false, stopping = false;
};
} // namespace lynx
