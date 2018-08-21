#ifndef THREADPOOL_HPP
#define THREADPOOL_HPP

#include <deque>
#include <condition_variable>
#include <future>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <iostream>

#ifdef TP_DEBUG
#define LOG(...)	{ Debug::log( __VA_ARGS__ ); }
#else
#define LOG(...)
#endif

#ifdef TP_THROW
#define TP_EXCEPT noexcept(false)
#else
#define TP_EXCEPT noexcept(true)
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

	namespace LockFree
	{
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

			fguard (const fguard& _other) = delete;

			fguard& operator = (const fguard& _other) = delete;
		};
	}

	///=============================================================================
	///	Debug printing
	///=============================================================================

	namespace Debug
	{
		inline static flag output_flag = ATOMIC_FLAG_INIT;

		/// Rudimentary debug printing.
		template<typename ... Args>
		void log(Args&& ... _args)
		{
			LockFree::fguard fg(output_flag);
			std::array<int, sizeof...(_args)> status{(std::cout << std::forward<Args>(_args), 0) ...};
			std::cout << '\n';
		}
	}

	namespace Storage
	{
		///=====================================
		/// Lock-free queue
		///=====================================

		template<typename T>
		class lfq
		{
		private:

			using fguard = LockFree::fguard;

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
				queue.emplace_back(std::move(_t));
			}

			template<typename ... Args>
			void emplace(Args&& ... _args)
			{
				fguard fg(f);
				queue.emplace_back(std::forward<Args>(_args)...);
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
	}

	///=============================================================================
	///	Main feature
	///=============================================================================

	/// @class Threadpool
	class ThreadPool
	{

#ifdef TP_BENCH
	public:
		uint enqueue_duration = 0;
#endif

	private:

		/// Task queue
		Storage::lfq<std::function<void()>> queue;

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

		~ThreadPool() TP_EXCEPT
		{
			flags.stop.store(true);
			wait();

			LOG("=====[ Task statistics ]=====",
			   "\nReceived:\t", stats.received,
			   "\nAssigned:\t", stats.assigned,
			   "\nCompleted:\t", stats.completed,
			   "\nAborted:\t", stats.aborted,
			   "\n")

			if (stats.received != stats.assigned + stats.completed + stats.aborted)
			{
#ifdef TP_THROW
				throw std::out_of_range("\n!!! ERROR: !!! Some tasks have been lost along the way!\n");
#else
				LOG("\n!!! ERROR: !!! Some tasks have been lost along the way!\n")
#endif
			}
		}

		template<typename F, typename ... Args>
		auto enqueue(F&& _f, Args&&... _args)
		{
			using Ret = typename std::result_of<F& (Args&...)>::type;

			if (flags.stop)
			{
				return std::future<Ret>();
			}

#ifdef TP_BENCH
			auto start(std::chrono::high_resolution_clock::now());
#endif
			++stats.received;

			/// Using a conditional wrapper to avoid dangling references.
			/// Courtesy of https://stackoverflow.com/a/46565491/4639195.
			auto task(std::make_shared<std::packaged_task<Ret()>>(std::bind(std::forward<F>(_f), wrap(std::forward<Args>(_args))...)));

			std::future<Ret> result(task->get_future());

			queue.push([=]{ (*task)(); });

			semaphore.notify_one();

#ifdef TP_BENCH
			uint ns(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - start).count());
			enqueue_duration += ns;
			LOG("Enqueue took ", ns, " ns\n")
#endif
			LOG("New task received (Received: ", stats.received, ", enqueued: ", queue.size(), ")")

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
				LOG("Threadpool already stopped.")
				return;
			}

			LOG("Stopping threadpool...")

			flags.stop.store(true);

			/// Empty the queue
			stats.aborted += queue.size();
			queue.clear();
		}

		void wait()
		{
			semaphore.notify_all();

			LOG("Waiting for tasks to finish...")

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
				LOG("\tWorker ", worker_id, " in thread ", std::this_thread::get_id(), " ready")

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

						LOG(stats.assigned, " task(s) assigned (", queue.size(), " enqueued)")

						/// Execute the task
						task();

						/// Update the stats
						--stats.assigned;
						++stats.completed;

						LOG(stats.assigned, " task(s) assigned (", queue.size(), " enqueued)")
					}

					if (!stats.assigned)
					{
						LOG("Signalling that all tasks have been processed...")

						finished.notify_all();
					}
				}

				--workers.count;
				flags.prune.store(workers.count > workers.target_count);

				LOG("\tWorker ", worker_id, " in thread ", std::this_thread::get_id(), " exiting...")

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
