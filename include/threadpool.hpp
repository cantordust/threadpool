#ifndef THREADPOOL_HPP
#define THREADPOOL_HPP

#include <deque>
#include <condition_variable>
#include <future>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>

#include <iostream>
#ifdef TP_DEBUG

#include <string>
#include <sstream>

#endif

namespace Async
{
	using uint = unsigned int;
	using auint = std::atomic<uint>;
	using toggle = std::atomic<bool>;
	using flag = std::atomic_flag;
	using ulock = std::unique_lock<std::mutex>;
	using glock = std::lock_guard<std::mutex>;
	using cv = std::condition_variable;

#ifdef TP_DEBUG

	namespace Debug
	{
		/// Mutex for outputting to std::cout
		static std::mutex dp_mutex;

		/// Rudimentary debug printer class.
		class Printer
		{
			std::stringstream buf;

		public:

			template<typename ... Args>
			Printer(Args&& ... _args)
			{
				std::array<int, sizeof...(_args)> status{(buf << std::forward<Args>(_args), 0) ...};
			}

			~Printer()
			{
				glock lk(dp_mutex);
				std::cout << buf.str() << "\n";
			}
		};
	}
#define DP(...)	{ Debug::Printer( __VA_ARGS__ ); }

#else
#define DP(...)

#endif

	/// @class Atomic flag guard
	class fguard
	{
	private:

		flag& f;

	public:

		fguard() = delete;

		explicit fguard(flag& _f)
			:
			  f(_f)
		{
			while (f.test_and_set(std::memory_order_acquire));
		}

		~fguard()
		{
			f.clear(std::memory_order_release);
		}

		fguard(const fguard&) = delete;
		fguard& operator=(const fguard&) = delete;
	};

	/// @class Lock-free queue
	template<typename T>
	class lfq
	{
	private:

		flag f = ATOMIC_FLAG_INIT;
		std::deque<T> queue;

	public:

		void push(const T& _t)
		{
			fguard fg(f);
			queue.push_back(_t);
		}

		void push(T&& _t)
		{
			fguard fg(f);
			queue.emplace_back(_t);
		}

		bool pop(T& _t)
		{
			fguard fg(f);
			if (queue.empty())
			{
				return false;
			}
			_t = std::move(queue.front());
			queue.pop_front();
			return true;
		}

		template<typename ... Args>
		void emplace(Args&& ... _args)
		{
			fguard fg(f);
			queue.emplace_back(std::forward<Args>(_args)...);
		}

		bool is_empty()
		{
			fguard fg(f);
			return queue.empty();
		}

		auto size()
		{
			fguard fg(f);
			return queue.size();
		}

		void clear()
		{
			fguard fg(f);
			queue.clear();
		}
	};

	/// @class Threadpool
	class ThreadPool
	{

#ifdef TP_BENCH
	public:

		uint enqueue_duration = 0;
#endif

	private:

		/// Task queue
		lfq<std::function<void()>> queue;

		/// Mutex for the condition variables
		std::mutex mtx;

		/// Condition variable used for signalling
		/// other threads that the processing has finished.
		cv finished;

		/// Condition variable for signalling threads to start processing the queues.
		cv semaphore;

		struct Flags
		{
			toggle stop;
			toggle prune;
			toggle pause;
			Flags()
				:
				  stop(false),
				  prune(false),
				  pause(false)
			{}
		} flags;

		struct Stats
		{
			auint received;
			auint assigned;
			auint completed;
			auint aborted;

			Stats()
				:
				  received(0),
				  assigned(0),
				  completed(0),
				  aborted(0)
			{}
		} stats;

		struct Workers
		{
			auint count;
			auint target_count;
			Workers(const uint _target_count)
				:
				  count(0),
				  target_count(_target_count)
			{}
		} workers;

	public:

		ThreadPool(const uint _init_count = std::thread::hardware_concurrency())
			:
			  workers(_init_count)
		{
			resize(_init_count);
		}

		~ThreadPool()
		{
			flags.stop.store(true);
			wait();

			DP("=====[ Task statistics ]=====",
			   "\nReceived:\t", stats.received,
			   "\nAssigned:\t", stats.assigned,
			   "\nCompleted:\t", stats.completed,
			   "\nAborted:\t", stats.aborted);

			if (stats.received != stats.assigned + stats.completed + stats.aborted)
			{
				DP("Some tasks have been lost along the way!");
			}
		}

		template<typename F, typename ... Args>
		auto enqueue(F&& _f, Args&&... _args)
		{
			using ret_t = typename std::result_of<F& (Args&...)>::type;

#ifdef TP_BENCH
			auto start(std::chrono::high_resolution_clock::now());
#endif

			/// Using a conditional wrapper to avoid dangling references.
			/// Courtesy of https://stackoverflow.com/a/46565491/4639195.
			auto task(std::make_shared<std::packaged_task<ret_t()>>(std::bind(std::forward<F>(_f), wrap(std::forward<Args>(_args))...)));

			std::future<ret_t> result(task->get_future());

			{
				if (!flags.stop)
				{
					++stats.received;

					queue.push([=]{ (*task)(); });

					// DP("New task received (Received: ", stats.received, ", enqueued: ", queue.size(), ")");

					semaphore.notify_one();
				}
			}

#ifdef TP_BENCH
			uint ns(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - start).count());
			enqueue_duration += ns;
			std::cout << "Enqueue took " << ns << " ns\n";
#endif

			return result;
		}

		void resize(const uint _count)
		{
			if (flags.stop)
			{
				return;
			}

			workers.target_count.store(_count);
			flags.prune.store((workers.count > workers.target_count));
			if (workers.count < workers.target_count)
			{
				for (uint i = 0; i < workers.target_count - workers.count; ++i)
				{
					add_worker();
				}
			}
		}

		void stop()
		{
			if (flags.stop)
			{
				DP("Threadpool already stopped.");
				return;
			}

			DP("Stopping threadpool...");

			flags.stop.store(true);

			/// Empty the queue
			stats.aborted += queue.size();
			queue.clear();
		}

		void wait()
		{
			semaphore.notify_all();

			DP("Waiting for tasks to finish...");

			ulock lk(mtx);
			finished.wait(lk, [&]
			{
				return (queue.is_empty() && !stats.assigned);
			});
		}

		void pause()
		{
			flags.pause.store(true);
		}

		void resume()
		{
			flags.pause.store(false);
			semaphore.notify_all();
		}

		uint worker_count()
		{
			return workers.count;
		}

		uint tasks_enqueued()
		{
			return stats.assigned;
		}

		uint tasks_received()
		{
			return stats.received;
		}

		uint tasks_completed()
		{
			return stats.completed;
		}

		uint tasks_aborted()
		{
			return stats.aborted;
		}

	private:

		void add_worker()
		{
			std::thread([&]
			{
				std::function<void()> task;

				uint worker_id(++workers.count);
				DP("\tWorker ", worker_id, " in thread ", std::this_thread::get_id(), " ready");

				while (true)
				{
					{
						ulock lk(mtx);

						/// Block execution until we have something to process.
						semaphore.wait(lk, [&]
						{
							return flags.stop || flags.prune || !flags.pause || !queue.is_empty();
						});
					}

					if (flags.prune ||
						(flags.stop &&
						 queue.is_empty()))
					{
						break;
					}

					if (queue.pop(task))
					{
						/// Update the stats
						++stats.assigned;

						DP(stats.assigned, " task(s) assigned (", queue.size(), " enqueued)");

						/// Execute the task
						task();

						/// Update the stats
						--stats.assigned;
						++stats.completed;

						DP(stats.assigned, " task(s) assigned (", queue.size(), " enqueued)");
					}

					if (!stats.assigned)
					{
						DP("Signalling that all tasks have been processed...");

						finished.notify_all();
					}
				}

				--workers.count;
				flags.prune.store(workers.count > workers.target_count);

				DP("\tWorker ", worker_id, " in thread ", std::this_thread::get_id(), " exiting...");

			}).detach();
		}

		template <class T>
		std::reference_wrapper<T> wrap(T& val)
		{
			return std::ref(val);
		}

		template <class T>
		T&&	wrap(T&& val)
		{
			return std::forward<T>(val);
		}
	};
}
#endif // THREADPOOL_HPP
