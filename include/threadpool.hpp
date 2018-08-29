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
#define DLOG(...) { Debug::log( __VA_ARGS__ ); }
#else
#define DLOG(...)
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
	using ulock = std::unique_lock<std::mutex>;
	using glock = std::lock_guard<std::mutex>;
	using cv = std::condition_variable;
	using nsec = std::chrono::nanoseconds;
	using usec = std::chrono::microseconds;
	using msec = std::chrono::milliseconds;
	using clock = std::chrono::high_resolution_clock;

	template<typename T>
	static inline constexpr uint duration(const std::chrono::high_resolution_clock::time_point& _tp)
	{
		return std::chrono::duration_cast<T>(std::chrono::high_resolution_clock::now() - _tp).count();
	}

	///=============================================================================
	///	Debug printing
	///=============================================================================

	namespace Debug
	{
		static std::mutex out_mutex;

		/// Rudimentary debug printing.
		template<typename ... Args>
		void log(Args&& ... _args)
		{
			glock lk(out_mutex);
			std::array<int, sizeof...(_args)> status{(std::cout << std::forward<Args>(_args), 0) ...};
			std::cout << '\n';
		}
	}

	///=============================================================================
	///	Storage classes
	///=============================================================================

	namespace Storage
	{
		///=====================================
		/// Task base and executor
		///=====================================

		struct TaskBase
		{
			virtual ~TaskBase() {};
			virtual void operator()() = 0;
			virtual void abort() = 0;
		};

		using TaskPtr = std::unique_ptr<TaskBase>;

		template<typename F, typename Ret, typename ... Args>
		struct TaskExecutor : TaskBase
		{
			std::condition_variable task_cv;
			std::promise<Ret> promise;
			std::function<Ret(Args&&...)> func;
			std::tuple<Args&& ...> args;

			constexpr TaskExecutor(F&& _func, std::promise<Ret>&& _promise, Args&& ... _args) noexcept(true)
				:
				  func(std::move(_func)),
				  promise(std::move(_promise)),
				  args(std::forward<Args>(_args)...)
			{}

			virtual ~TaskExecutor() {};

			virtual void operator()() override final
			{
				if constexpr (std::is_void<Ret>::value)
				{
					std::apply(func, args);
					promise.set_value();
				}
				else
				{
					promise.set_value(std::apply(func, args));
				}
			}

			virtual void abort() override final
			{
				if constexpr (std::is_void<Ret>::value)
				{
					promise.set_value();
				}
				else
				{
					promise.set_value(Ret{});
				}
			}
		};

		///=====================================
		/// Thread-safe queue
		///=====================================

		template<typename T>
		class TSQ
		{

		protected:

			std::mutex mtx;
			std::deque<T> queue;

		public:

			void push(const T& _t)
			{
				glock lk(mtx);
				queue.push_back(_t);
			}

			void push(T&& _t)
			{
				glock lk(mtx);
				queue.emplace_back(std::move(_t));
			}

			template<typename ... Args>
			void emplace(Args&& ... _args)
			{
				glock lk(mtx);
				queue.emplace_back(std::forward<Args>(_args)...);
			}

			bool pop(T& _t)
			{
				glock lk(mtx);
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
				glock lk(mtx);
				return queue.empty();
			}

			std::size_t size()
			{
				glock lk(mtx);
				return queue.size();
			}

			virtual void clear()
			{
				glock lk(mtx);
				queue.clear();
			}
		};
	}

	///=============================================================================
	///	Non-blocking future.
	///=============================================================================

	namespace NonBlocking
	{
		template<typename T>
		class Future
		{
			std::mutex task_mtx;
			std::condition_variable& task_cv;

			std::future<T&> future;

		public:

			Future(std::condition_variable& _task_cv,
				   std::future<T>&& _future) noexcept(true)
				:
				  task_cv(_task_cv),
				  future(std::move(_future))
			{}

			void nb_wait() const
			{
				glock lk(task_mtx);
				task_cv.wait(lk, [&]{ return future.valid(); });
				return future.wait();
			}

			void wait() const
			{
				future.wait();
			}

			void get() const
			{
				future.get();
			}

			bool is_valid() const
			{
				return future.valid();
			}




		};
	} // namespace NonBlocking

	template<typename T>
	using Future = NonBlocking::Future<T>;

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
		class TaskQueue : public Storage::TSQ<Storage::TaskPtr>
		{
		public:

			template<typename F, typename Ret, typename ... Args>
			void emplace(F&& _f, std::promise<Ret>&& _promise, Args&& ... _args)
			{
				glock lk(mtx);
				queue.emplace_back(std::make_unique<Storage::TaskExecutor<F, Ret, Args...>>(std::forward<F>(_f), std::move(_promise), std::forward<Args>(_args)...));
			}

			virtual void clear() override
			{
				glock lk(mtx);
				while (!queue.empty())
				{
					queue.front()->abort();
					queue.pop_front();
				}
			}
		} queue;

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

			DLOG("\n=====[ Task statistics ]=====",
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
				DLOG("\n!!! ERROR: !!! Some tasks have been lost along the way!\n")
		#endif
			}
		}

		template<typename F, typename ... Args>
		auto enqueue(F&& _f, Args&& ... _args) -> std::future<typename std::result_of<F(Args...)>::type>
		{
			using Ret = typename std::result_of<F(Args...)>::type;

#ifdef TP_BENCH
			auto start(clock::now());
#endif

			std::promise<Ret> promise;
			std::future<Ret> future(promise.get_future());

			if (flags.stop)
			{
				if constexpr (std::is_void<Ret>::value)
				{
					promise.set_value();
				}
				else
				{
					promise.set_value(Ret{});
				}
				return future;
			}

			++stats.received;

			queue.emplace(std::forward<F>(_f), std::move(promise), std::forward<Args>(_args)...);

			semaphore.notify_one();

#ifdef TP_BENCH
			uint ns(duration<nsec>(start));
			enqueue_duration += ns;
			Debug::log("Enqueue took ", ns, " ns");
#endif
			DLOG("New task enqueued (Received: ", stats.received, ", enqueued: ", queue.size(), ")")

					return future;
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
				DLOG("Threadpool already stopped.")
						return;
			}

			DLOG("Stopping threadpool...")

					flags.stop.store(true);

			/// Empty the queue
			stats.aborted += queue.size();
			queue.clear();
		}

		void wait()
		{
			semaphore.notify_all();

			DLOG("Waiting for tasks to finish...")

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
				Storage::TaskPtr task(nullptr);

				uint worker_id(++workers.count);
				DLOG("\tWorker ", worker_id, " in thread ", std::this_thread::get_id(), " ready")

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

						DLOG(stats.assigned, " task(s) assigned (", queue.size(), " enqueued)")

								/// Execute the task
								(*task)();

						/// Update the stats
						--stats.assigned;
						++stats.completed;

						DLOG(stats.assigned, " task(s) assigned (", queue.size(), " enqueued)")
					}

					if (!stats.assigned)
					{
						DLOG("Signalling that all tasks have been processed...");

						finished.notify_all();
					}
				}

				--workers.count;
				flags.prune.store(workers.count > workers.target_count);

				DLOG("\tWorker ", worker_id, " in thread ", std::this_thread::get_id(), " exiting...")

			}).detach();
		}
	};
}
#endif // THREADPOOL_HPP
