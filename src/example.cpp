#include "threadpool.hpp"
#include <random>
#include <iostream>
#include <syncstream>
#include <string>

using namespace Async;

static std::mt19937_64 rng;
static std::uniform_int_distribution<uint> d_function(0, 3);
static std::uniform_int_distribution<uint> d_tasks(10, 25);
static std::uniform_int_distribution<uint> d_workers(4, 20);
static std::uniform_int_distribution<uint> d_sleep(100, 1000);
static std::uniform_int_distribution<uint> d_ref(0, 10000);
static std::uniform_real_distribution<double> d_stop(0.0, 1.0);
static std::uniform_real_distribution<double> d_pause(0.0, 1.0);

inline static flag sleep_flag = ATOMIC_FLAG_INIT;
std::mutex rng_mtx;

uint rnd_sleep()
{
    std::lock_guard lk{ rng_mtx };
    return d_sleep(rng);
}

void func1()
{
    static auint call_count{0};
    uint tl_count(++call_count);
    // uint sleep(rnd_sleep());
    // log("func1() call # ", tl_count, " | sleeping for ", sleep, " ms");
    // std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
    // log("func1() call # ", tl_count, " exiting...");
}

void func2()
{
    static auint call_count{0};
    uint tl_count(++call_count);
    // uint sleep(rnd_sleep());
    // log("uint func2() call # ", tl_count, " | sleeping for ", sleep, " ms");
    // std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
    // log("uint func2() call # ", tl_count, " exiting...");;
}

void func3(std::string str)
{
    static auint call_count{0};
    uint tl_count(++call_count);
    // uint sleep(rnd_sleep());
    // log("func3() called # ", tl_count, " times | str: ", str, " | sleeping for ", sleep, " ms");
    // std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
    // log("func3() call # ", tl_count, "  exiting...");
}

void func4(const uint& num)
{
    static auint call_count{0};
    uint tl_count(++call_count);
    // uint sleep(rnd_sleep());
    // log("func4 call # ", tl_count, " times | num: ", num, " | sleeping for ", sleep, " ms");
    // std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
    // log("func4() # ", tl_count, " exiting...");
}


void schedule(ThreadPool& _tp, const uint _tasks, const uint& _reference)
{
    for (uint t = 1; t <= _tasks; ++t)
    {
        switch (d_function(rng) % 4)
        {
        case 0:
            {
                _tp.enqueue(func1);
                break;
            }

        case 1:
            {
                _tp.enqueue(func2);
                break;
            }

        case 2:
            {
                _tp.enqueue(func3, "string");
                break;
            }

        case 3:
            {
                _tp.enqueue(func4, _reference);
                break;
            }
        }
    }
}

int main(void)
{
    rng.seed(static_cast<uint>(std::chrono::high_resolution_clock().now().time_since_epoch().count()));

    uint tasks(d_tasks(rng));
    uint iterations(100);
    uint runs(1000);


    for (uint it = 0; it < iterations; ++it)
    {
        log("\n************ Iteration ", it + 1, "/", iterations, " ************");

        uint ref{d_ref(rng)};
        uint workers(std::thread::hardware_concurrency());
        ThreadPool tp(workers);

        for (uint run = 0; run < runs; ++run)
        {
            // log("(main) Scheduling ", tasks, " task(s).");
            schedule(tp, d_tasks(rng), ref);

            // if (d_stop(rng) < 0.1)
            // {
            // 	tp.stop();
            // 	log("(main) Threadpool stopped.");
            // }

            // if (d_pause(rng) < 0.33)
            // {
            // 	log("(main) Pausing threadpool...");
            // 	tp.pause();
            // 	uint sleep(3 * d_sleep(rng));

            // 	log("(main) Main sleeping for ", sleep, " milliseconds.");
            // 	std::this_thread::sleep_for(std::chrono::milliseconds(sleep));

            // 	log("(main) Resuming threadpool...");
            // 	tp.resume();
            // }
        }

        log("--> Destroying ThreadPool...");

#ifdef TP_BENCH
        log("Average enqueue time: ", static_cast<double>(tp.enqueue_duration) / static_cast<double>(tp.calls == 0 ? 1.0 : tp.calls), " ns");
        log("Average swap time: ", static_cast<double>(tp.swap_duration.load()) / static_cast<double>(tp.swaps.load() == 0 ? 1.0 : tp.swaps.load()), " ns");
#endif

    }

    log("***** ", iterations, " iteration(s) completed successfully! Exiting main.");

    return 0;
}
