#include "threadpool.hpp"
#include <random>
#include <iostream>
#include <string>

using namespace Async;

static std::mt19937_64 rng;
static std::uniform_int_distribution<uint> d_function(0, 3);
static std::uniform_int_distribution<uint> d_tasks(10, 25);
static std::uniform_int_distribution<uint> d_workers(4, 20);
static std::uniform_int_distribution<uint> d_sleep(100, 1000);
static std::uniform_real_distribution<double> d_stop(0.0, 1.0);
static std::uniform_real_distribution<double> d_pause(0.0, 1.0);


uint rnd_sleep()
{
    static std::mutex mtx;
    glock lk(mtx);
    return d_sleep(rng);
}

void void_void()
{
    static auint call_count(0);
    uint tl_count(++call_count);
#ifdef TP_LOG
    Debug::log("\tvoid void_void() call # ", tl_count);
#endif /// TP_LOG
    uint sleep(rnd_sleep());
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
#ifdef TP_LOG
    Debug::log("\tvoid void_void() # ", tl_count, " exiting...");
#endif /// TP_LOG
}

uint uint_void()
{
    static auint call_count(0);
    uint tl_count(++call_count);
#ifdef TP_LOG
    Debug::log("\tuint uint_void() call # ", tl_count);
#endif /// TP_LOG
    uint sleep(rnd_sleep());
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
#ifdef TP_LOG
    Debug::log("\tuint uint_void() # ", tl_count, "  exiting...");
#endif /// TP_LOG
    return sleep;
}

std::string string_void()
{
    static auint call_count(0);
    uint tl_count(++call_count);
#ifdef TP_LOG
    Debug::log("\tstd::string string_void() call # ", tl_count);
#endif /// TP_LOG
    uint sleep(rnd_sleep());
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
#ifdef TP_LOG
    Debug::log("\tstd::string string_void() # ", tl_count, "  exiting...");
#endif /// TP_LOG
    return std::to_string(sleep);
}

void void_uint(auint& _reference)
{
    static auint call_count(0);
    uint tl_count(++call_count);
#ifdef TP_LOG
    Debug::log("\tvoid void_uint(uint&) call # ", tl_count);
    Debug::log("\tvoid void_uint(uint&) reference is ", ++_reference);
#endif /// TP_LOG
    uint sleep(rnd_sleep());
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
#ifdef TP_LOG
    Debug::log("\tvoid void_uint(const uint _num) # ", tl_count, " exiting...");
#endif /// TP_LOG
}

template<typename T>
void print(T&& _t)
{
    Debug::log("\tValue is ready: ", _t);
}

void schedule(ThreadPool& _tp, const uint _tasks, auint& _reference)
{
    for (uint t = 1; t <= _tasks; ++t)
    {
        switch (d_function(rng) % 4)
        {
        case 0:
            {
                _tp.enqueue(&void_void);
                break;
            }

        case 1:
            {
                _tp.enqueue(&uint_void);
                break;
            }

        case 2:
            {
                _tp.enqueue(&string_void);
                break;
            }

        case 3:
            {
                _tp.enqueue(&void_uint, _reference);
                break;
            }
        }
    }
}

int main(void)
{
    rng.seed(static_cast<uint>(std::chrono::high_resolution_clock().now().time_since_epoch().count()));

    uint tasks(d_tasks(rng));
    uint iterations(10);
    uint runs(10);

#ifdef TP_BENCH
    double enqueue_duration(0.0);
    double total_tasks(0.0);
#endif /// TP_BENCH

    for (uint it = 1; it <= iterations; ++it)
    {
        Debug::log("\n===============[ Iteration ", it, "/", iterations, " ]===============\n");

        uint workers(std::thread::hardware_concurrency());

        auint reference(0);

        ThreadPool tp(workers);

        for (uint run = 0; run < runs; ++run)
        {
            Debug::log("(main) Scheduling ", tasks, " task(s).");
            schedule(tp, (tasks = d_tasks(rng)), reference);

            /// Synchronise
            tp.sync();
            Debug::log("(main) Tasks completed.");

            workers = d_workers(rng);

            Debug::log("(main) Resizing pool to ", workers, " worker(s).");
            tp.resize(workers);

            Debug::log("(main) Scheduling ", tasks, " task(s).");
            schedule(tp, (tasks = d_tasks(rng)), reference);

            if (d_stop(rng) < 0.1)
            {
                tp.stop();
                Debug::log("(main) Threadpool stopped.");
            }

            if (d_pause(rng) < 0.33)
            {
                Debug::log("(main) Pausing threadpool...");
                tp.pause();
                uint sleep(3 * d_sleep(rng));

                Debug::log("(main) Main sleeping for ", sleep, " milliseconds.");
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep));

                Debug::log("(main) Resuming threadpool...");
                tp.resume();
            }

            /// Synchronise
            tp.sync();
            Debug::log("(main) Tasks completed.");
        }

        Debug::log("(main) Destroying ThreadPool...");

#ifdef TP_BENCH
        enqueue_duration += tp.enqueue_duration;
        total_tasks += tp.tasks_received();
#endif /// TP_BENCH

    }

    Debug::log("\n===============[ The End ]===============\n");

#ifdef TP_BENCH
        Debug::log("(main) Average enqueue time: ", enqueue_duration / total_tasks, " ns");
#endif /// TP_BENCH

    Debug::log("(main) ", iterations, " iteration(s) completed successfully! Exiting main.");

    return 0;
}
