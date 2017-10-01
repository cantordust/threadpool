#include "threadpool.hpp"

static std::mt19937_64 rng;
static std::uniform_int_distribution<uint> d_task(0, 11);
static std::uniform_int_distribution<uint> d_workers(4, 15);
static std::uniform_int_distribution<uint> d_task_sleep(100, 5000);
static std::uniform_int_distribution<uint> d_main_sleep(5000, 25000);

void void_void()
{
	uint sleep(d_task_sleep(rng));
	std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
}

uint uint_void()
{
	uint sleep(d_task_sleep(rng));
	std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
	return sleep;
}

std::string string_void()
{
	uint sleep(d_task_sleep(rng));
	std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
	return std::to_string(sleep);
}

void void_uint(const uint _num)
{
	uint sleep(d_task_sleep(rng));
	std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
}

using namespace Async;

void schedule(ThreadPool& _tp,
			  const uint _tasks)
{
	for (uint t = 1; t <= _tasks; ++t)
	{
		switch (d_task(rng) % 4)
		{
			case 0:
			{
				auto future = _tp.enqueue(void_void);
				break;
			}

			case 1:
			{
				auto future = _tp.enqueue(uint_void);
				break;
			}

			case 2:
			{
				auto future = _tp.enqueue(string_void);
				break;
			}

			case 3:
			{
				auto future = _tp.enqueue(void_uint, t);
				break;
			}
		}
	}
}

int main(void)
{
	rng.seed(static_cast<uint>(std::chrono::high_resolution_clock().now().time_since_epoch().count()));

	uint main_sleep(0);
	uint tasks(20);
	uint iterations(5);

	for (uint it = 1; it <= iterations; ++it)
	{
		dlog() << "\n************ Iteration " << it << "/" << iterations << " ************\n";

		uint workers(4);
		ThreadPool tp(workers);

		dlog() << "--> Scheduling " << tasks << " task(s)";

		schedule(tp, tasks);

		workers = d_workers(rng);
		dlog() << "--> Resizing pool to " << workers << " worker(s)";
		tp.resize(workers);
		dlog() << "--> Scheduling " << tasks << " task(s)";
		schedule(tp, tasks);

		/// Synchronise
		dlog() << "Waiting for tasks to complete...";
		tp.wait();
		dlog() << "Tasks completed!";

		main_sleep = d_main_sleep(rng);
		dlog() << "--> Main thread sleeping for " << main_sleep << " ms";
		std::this_thread::sleep_for(std::chrono::milliseconds(main_sleep));

		workers = d_workers(rng);
		dlog() << "--> Resizing pool to " << workers << " worker(s)";
		tp.resize(workers);
		dlog() << "--> Scheduling " << tasks << " task(s)";
		schedule(tp, tasks);

		workers = d_workers(rng);
		dlog() << "--> Resizing pool to " << workers << " worker(s)";
		tp.resize(workers);

		main_sleep = d_main_sleep(rng);
		dlog() << "--> Main thread sleeping for " << main_sleep << " ms";
		std::this_thread::sleep_for(std::chrono::milliseconds(main_sleep));

		workers = d_workers(rng);
		dlog() << "--> Resizing pool to " << workers << " worker(s)";
		tp.resize(workers);
		dlog() << "--> Scheduling " << tasks << " task(s)";
		schedule(tp, tasks);

		dlog() << "--> Waiting for ThreadPool destructor...";

	}

	dlog() << "***** " << iterations << " iteration(s) completed successfully! *****";
	dlog() << "Exiting main...";

	return 0;
}
