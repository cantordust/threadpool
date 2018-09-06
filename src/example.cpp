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
	static auint call_count{0};
	uint tl_count(++call_count);
	LOG("\tvoid void_void() call # ", tl_count);
	uint sleep(rnd_sleep());
	std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
	LOG("\tvoid void_void() # ", tl_count, " exiting...");
}

uint uint_void()
{
	static auint call_count{0};
	uint tl_count(++call_count);
	LOG("\tuint uint_void() call # ", tl_count);
	uint sleep(rnd_sleep());
	std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
	LOG("\tuint uint_void() # ", tl_count, "  exiting...");
	return sleep;
}

std::string string_void()
{
	static auint call_count{0};
	uint tl_count(++call_count);
	LOG("\tstd::string string_void() call # ", tl_count);
	uint sleep(rnd_sleep());
	std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
	LOG("\tstd::string string_void() # ", tl_count, "  exiting...");
	return std::to_string(sleep);
}

void void_uint(const uint _num)
{
	static auint call_count{0};
	uint tl_count(++call_count);
	LOG("\tvoid void_uint(const uint) call # ", tl_count);
	uint sleep(rnd_sleep());
	std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
	LOG("\tvoid void_uint(const uint _num) # ", tl_count, " exiting...");
}

void schedule(ThreadPool& _tp, const uint _tasks)
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
				auto future(_tp.enqueue(&uint_void));
//				LOG("\tCase 1: ", future.get());
				break;
			}

		case 2:
			{
				auto future(_tp.enqueue(&string_void));
//				LOG("\tCase 2: ", future.get());
				break;
			}

		case 3:
			{
				_tp.enqueue(&void_uint, t);
				break;
			}
		}
	}
}

int main(void)
{
	rng.seed(static_cast<uint>(std::chrono::high_resolution_clock().now().time_since_epoch().count()));

	using Debug::log;

	uint tasks(d_tasks(rng));
	uint iterations(10);
	uint runs(10);

#ifdef TP_BENCH
	double enqueue_duration(0.0);
	double total_tasks(0.0);
#endif

	for (uint it = 1; it <= iterations; ++it)
	{
		log("\n===============[ Iteration ", it, "/", iterations, " ]===============\n");

		uint workers(std::thread::hardware_concurrency());
		ThreadPool tp(workers);

		for (uint run = 0; run < runs; ++run)
		{
			log("(main) Scheduling ", tasks, " task(s).");
			schedule(tp, (tasks = d_tasks(rng)));

			/// Synchronise
			tp.sync();
			log("(main) Tasks completed.");

			workers = d_workers(rng);

			log("(main) Resizing pool to ", workers, " worker(s).");
			tp.resize(workers);

			log("(main) Scheduling ", tasks, " task(s).");
			schedule(tp, (tasks = d_tasks(rng)));

			if (d_stop(rng) < 0.1)
			{
				tp.stop();
				log("(main) Threadpool stopped.");
			}

			if (d_pause(rng) < 0.33)
			{
				log("(main) Pausing threadpool...");
				tp.pause();
				uint sleep(3 * d_sleep(rng));

				log("(main) Main sleeping for ", sleep, " milliseconds.");
				std::this_thread::sleep_for(std::chrono::milliseconds(sleep));

				log("(main) Resuming threadpool...");
				tp.resume();
			}

			/// Synchronise
			tp.sync();
			log("(main) Tasks completed.");
		}

		log("(main) Destroying ThreadPool...");

#ifdef TP_BENCH
		enqueue_duration += static_cast<double>(tp.enqueue_duration);
		total_tasks += static_cast<double>(tp.tasks_received());
#endif

	}

	log("\n===============[ The End ]===============\n");

#ifdef TP_BENCH
		log("(main) Average enqueue time: ", enqueue_duration / total_tasks, " ns");
#endif

	log("(main) ", iterations, " iteration(s) completed successfully! Exiting main.");

	return 0;
}
