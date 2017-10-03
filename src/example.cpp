#include "threadpool.hpp"

std::mutex sleep_mtx;
static std::mt19937_64 rng;
static std::uniform_int_distribution<uint> d_task(0, 11);
static std::uniform_int_distribution<uint> d_workers(4, 15);
static std::uniform_int_distribution<uint> d_sleep(100, 5000);
static std::uniform_real_distribution<double> d_stop(0.0, 1.0);

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
	uint runs(4);

	for (uint it = 1; it <= iterations; ++it)
	{
		dlog() << "\n************ Iteration " << it << "/" << iterations << " ************\n";

		uint workers(std::thread::hardware_concurrency());
		ThreadPool tp(workers);

		for (uint run = 0; run < runs; ++run)
		{
			dlog() << "--> Scheduling " << tasks << " task(s)";
			schedule(tp, tasks);

			/// Synchronise
			dlog() << "Waiting for tasks to complete...";
			tp.wait();
			dlog() << "Tasks completed!";

			workers = d_workers(rng);

			dlog() << "--> Resizing pool to " << workers << " worker(s)";
			tp.resize(workers);

			dlog() << "--> Scheduling " << tasks << " task(s)";
			schedule(tp, tasks);

			if (d_stop(rng) < 0.5)
			{
				tp.stop();
				dlog() << "Threadpool stopped.";
			}
		}

		dlog() << "--> Waiting for ThreadPool destructor...";

	}

	dlog() << "***** " << iterations << " iteration(s) completed successfully! *****";
	dlog() << "Exiting main...";

	return 0;
}
