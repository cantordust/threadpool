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

#ifdef TP_DEBUG

#include <iostream>
#include <string>
#include <sstream>

#endif

namespace Async
{
	using uint = unsigned int;
	using auint = std::atomic<uint>;
	using flag = std::atomic<bool>;
	using ulock = std::unique_lock<std::mutex>;
	using glock = std::lock_guard<std::mutex>;
	using cv = std::condition_variable;
	template<typename T1, typename T2, typename ... Rest>
	using hmap = std::unordered_map<T1, T2, Rest ...>;

#ifdef TP_DEBUG

	/// Mutex for outputting to std::cout
	static std::mutex dp_mutex;

	/// Rudimentary debug printer class.
	class dp
	{
		std::stringstream buf;

	public:

		template<typename ... Args>
		dp(Args&& ... _args)
		{
			std::array<int, sizeof...(_args)> status{(buf << std::forward<Args>(_args), 0) ...};
		}

		~dp()
		{
			glock lk(dp_mutex);
			std::cout << buf.str() << "\n";
		}
	};

#endif

	/// @class Atomic flag guard
	class fguard
	{
	private:

		std::atomic_flag& f;

	public:

		fguard() = delete;

		explicit fguard(std::atomic_flag& _f)
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

		std::atomic_flag flag = ATOMIC_FLAG_INIT;
		std::deque<T> queue;

	public:

		void push(const T& _t)
		{
			fguard fg(flag);
			queue.push_back(_t);
		}

		void push(T&& _t)
		{
			fguard fg(flag);
			queue.emplace_back(_t);
		}

		bool pop(T& _t)
		{
			fguard fg(flag);
			if (queue.empty())
			{
				return false;
			}
			_t = queue.front();
			queue.pop_front();
			return true;
		}

		bool is_empty()
		{
			fguard fg(flag);
			return queue.empty();
		}

		auto size()
		{
			fguard fg(flag);
			return queue.size();
		}

		void clear()
		{
			fguard fg(flag);
			queue.clear();
		}
	};

	/// @class Threadpool
	class ThreadPool
	{
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
			flag stop;
			flag prune;
			flag pause;
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

		/// Registered functions.
		/// @todo Implementation.
		hmap<uint, std::function<void()>> functions;

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

#ifdef TP_DEBUG

			dp("=====[ Task statistics ]=====",
			   "\nReceived:\t", stats.received,
			   "\nAssigned:\t", stats.assigned,
			   "\nCompleted:\t", stats.completed,
			   "\nAborted:\t", stats.aborted);

			if (stats.received != stats.assigned + stats.completed + stats.aborted)
			{
				dp("Some tasks have been lost along the way!");
				exit(1);
			}
#endif
		}

		template<typename F, typename ... Args>
		auto enqueue(F&& _f, Args&&... _args)
		{
			using ret_t = typename std::result_of<F& (Args&...)>::type;

			/// Using a conditional wrapper to avoid dangling references.
			/// Courtesy of https://stackoverflow.com/a/46565491/4639195.
			auto task(std::make_shared<std::packaged_task<ret_t()>>(std::bind(std::forward<F>(_f), wrap(std::forward<Args>(_args))...)));

			std::future<ret_t> result(task->get_future());

			{
				if (!flags.stop)
				{
					++stats.received;
					{
						queue.push([=]{ (*task)(); });
#ifdef TP_DEBUG
						dp("New task received (Received: ", stats.received, ", enqueued: ", queue.size(), ")");
#endif
					}
					semaphore.notify_one();
				}
			}

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
#ifdef TP_DEBUG
				dp("Threadpool already stopped.");
#endif
				return;
			}
#ifdef TP_DEBUG
			dp("Stopping threadpool...");
#endif
			flags.stop.store(true);

			/// Empty the queue
			stats.aborted += queue.size();
			queue.clear();
		}

		void wait()
		{
			semaphore.notify_all();
#ifdef TP_DEBUG
			dp("Waiting for tasks to finish...");
#endif
			ulock lk(mtx);
			finished.wait(lk, [&] { return (queue.is_empty() && !stats.assigned); });
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
#ifdef TP_DEBUG
				uint worker_id(++workers.count);
				dp("\tWorker ", worker_id, " in thread ", std::this_thread::get_id(), " ready");
#endif
				while (true)
				{
					{
						ulock lk(mtx);
						/// Block execution until we have something to process.
						semaphore.wait(lk, [&]{ return flags.stop || flags.prune || !flags.pause || !queue.is_empty(); });
					}

					if (flags.prune ||
						(flags.stop &&
						 queue.is_empty()))
					{
						break;
					}

					if (queue.pop(task))
					{
						/// Execute the task
						++stats.assigned;

#ifdef TP_DEBUG
						dp(stats.assigned, " task(s) assigned (", queue.size(), " enqueued)");
#endif
						task();

						--stats.assigned;
						++stats.completed;
#ifdef TP_DEBUG
						dp(stats.assigned, " task(s) assigned (", queue.size(), " enqueued)");
#endif
					}

					if (!stats.assigned)
					{
#ifdef TP_DEBUG
						dp("Signalling that all tasks have been processed...");
#endif
						finished.notify_all();
					}
				}

				--workers.count;
				flags.prune.store(workers.count > workers.target_count);

#ifdef TP_DEBUG
				dp("\tWorker ", worker_id, " in thread ", std::this_thread::get_id(), " exiting...");
#endif
			}).detach();
		}

//		template<typename F, typename ... Args>
//		uint add_func(F&& _f, Args&& ... _args)
//		{
////			using ret_t = typename std::result_of<F&>::type;
//			std::function<void()> f([&]() -> decltype (_f(_args...)) { return _f(_args...); });

//		}

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
