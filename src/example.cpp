#include "threadpool.hpp"
#include <random>
#include <iostream>
#include <string>
#include <sstream>

std::mutex sleep_mtx;
static std::mt19937_64 rng;
static std::uniform_int_distribution<uint> d_function(0, 3);
static std::uniform_int_distribution<uint> d_tasks(10, 25);
static std::uniform_int_distribution<uint> d_workers(4, 20);
static std::uniform_int_distribution<uint> d_sleep(100, 1000);
static std::uniform_real_distribution<double> d_stop(0.0, 1.0);
static std::uniform_real_distribution<double> d_pause(0.0, 1.0);

uint rnd_sleep()
{
	Async::glock lk(sleep_mtx);
	return d_sleep(rng);
}

void void_void()
{
	uint sleep(rnd_sleep());
	std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
}

uint uint_void()
{
	uint sleep(rnd_sleep());
	std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
	return sleep;
}

std::string string_void()
{
	uint sleep(rnd_sleep());
	std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
	return std::to_string(sleep);
}

void void_uint(const uint _num)
{
	uint sleep(rnd_sleep());
	std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
}

using namespace Async;

void schedule(ThreadPool& _tp, const uint _tasks)
{
	for (uint t = 1; t <= _tasks; ++t)
	{
		switch (d_function(rng) % 4)
		{
		case 0:
			{
				_tp.enqueue(void_void);
				break;
			}

		case 1:
			{
				auto future(_tp.enqueue(uint_void));
				break;
			}

		case 2:
			{
				auto future(_tp.enqueue(string_void));
				break;
			}

		case 3:
			{
				_tp.enqueue(void_uint, t);
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

	for (uint it = 1; it <= iterations; ++it)
	{
		std::cout << "\n************ Iteration " << it << "/" << iterations << " ************\n";

		uint workers(std::thread::hardware_concurrency());
		ThreadPool tp(workers);

		for (uint run = 0; run < runs; ++run)
		{
			std::cout << "(main) Scheduling " << tasks << " task(s)";
			schedule(tp, (tasks = d_tasks(rng)));

			/// Synchronise
			tp.wait();
			std::cout << "(main) Tasks completed.";

			workers = d_workers(rng);

			std::cout << "(main) Resizing pool to " << workers << " worker(s).";
			tp.resize(workers);

			std::cout << "(main) Scheduling " << tasks << " task(s).";
			schedule(tp, (tasks = d_tasks(rng)));

			if (d_stop(rng) < 0.1)
			{
				tp.stop();
				std::cout << "(main) Threadpool stopped.";
			}

			if (d_pause(rng) < 0.33)
			{
				std::cout << "(main) Pausing threadpool...";
				tp.pause();
				uint sleep(3 * d_sleep(rng));

				std::cout << "(main) Main sleeping for " << sleep << " milliseconds.";
				std::this_thread::sleep_for(std::chrono::milliseconds(sleep));

				std::cout << "(main) Resuming threadpool...";
				tp.resume();
			}
		}

		std::cout << "--> Destroying ThreadPool...";

	}

	std::cout << "***** " << iterations << " iteration(s) completed successfully! *****";
	std::cout << "Exiting main...";

	return 0;
}
