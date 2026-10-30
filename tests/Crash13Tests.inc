// Round 165r (crash13): issue #13 — UI freezes from file writes on the message thread (docs/analise/REA_rodada165r.md)
#include "core/SerialJobs.h"

TEST (crash13_serial_jobs_run_in_order_off_the_caller_thread)
{
    lynx::SerialJobs jobs;
    std::vector<int> order;
    std::mutex m;
    std::atomic<int> onCaller { 0 };
    const auto caller = std::this_thread::get_id();
    for (int i = 0; i < 200; ++i)
        jobs.post ([&, i] {
            if (std::this_thread::get_id() == caller)
                ++onCaller;
            const std::lock_guard<std::mutex> lock (m);
            order.push_back (i);
        });
    jobs.waitIdle();
    CHECK (jobs.isIdle());
    CHECK (onCaller == 0);
    CHECK (order.size() == 200);
    bool sorted = true;
    for (std::size_t i = 0; i < order.size(); ++i)
        sorted = sorted && order[i] == (int) i;
    CHECK (sorted);
}

TEST (crash13_serial_jobs_post_returns_while_a_write_waits)
{
    // the message thread posts the audio-device.xml write and goes on, even when the disk takes seconds (2 s measured
    // on Windows inside FlushFileBuffers); here a job that blocks until released stands for the slow flush
    lynx::SerialJobs jobs;
    std::atomic<bool> release { false }, done { false };
    const auto start = std::chrono::steady_clock::now();
    jobs.post ([&] {
        while (! release)
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        done = true;
    });
    jobs.post ([] {});
    const auto posted = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count();
    CHECK (posted < 100.0);
    CHECK (! done);
    CHECK (! jobs.isIdle());
    release = true;
    jobs.waitIdle();
    CHECK (done);
}

TEST (crash13_serial_jobs_destructor_writes_what_is_queued)
{
    // quitting right after a device change still leaves the newest setup on disk
    std::atomic<int> ran { 0 };
    {
        lynx::SerialJobs jobs;
        for (int i = 0; i < 50; ++i)
            jobs.post ([&] {
                std::this_thread::sleep_for (std::chrono::microseconds (200));
                ++ran;
            });
    }
    CHECK (ran == 50);

    // after stop(), a post runs at once on the caller's thread (never lost, never queued forever)
    lynx::SerialJobs stopped;
    stopped.stop();
    bool inline_ = false;
    const auto caller = std::this_thread::get_id();
    stopped.post ([&] { inline_ = std::this_thread::get_id() == caller; });
    CHECK (inline_);
    // a throwing job does not end the thread
    lynx::SerialJobs jobs;
    int after = 0;
    jobs.post ([] { throw std::runtime_error ("disk full"); });
    jobs.post ([&] { after = 1; });
    jobs.waitIdle();
    CHECK (after == 1);
}
