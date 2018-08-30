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
#include <unordered_map>
#include <memory>

#ifdef TP_DEBUG
#define LOG(...) Debug::log( __VA_ARGS__ );
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
	using mutex = std::mutex;
	using cvar = std::condition_variable;
	using thread_id = std::thread::id;
	using ulock = std::unique_lock<mutex>;
	using glock = std::lock_guard<mutex>;
	template<typename T1, typename T2>
	using hmap = std::unordered_map<T1, T2>;
	using Task = std::function<void()>;

	///=============================================================================
	///	Debug printing
	///=============================================================================

	namespace Debug
	{
		inline mutex cout_mutex;

		/// Rudimentary debug printing.
		template<typename ... Args>
		void log(Args&& ... _args)
		{
			glock lk(cout_mutex);
			std::array<int, sizeof...(_args)> status{(std::cout << std::forward<Args>(_args), 0) ...};
			std::cout << '\n';
		}
	}

	struct Semaphore
	{
		mutex mtx;
		cvar cv;

		void wait()
		{
			ulock lk(mtx);
			cv.wait(lk);
		}

		void wait(std::function<bool()> _check)
		{
			ulock lk(mtx);
			cv.wait(lk, _check);
		}

		void notify_one()
		{
			cv.notify_one();
		}

		void notify_all()
		{
			cv.notify_all();
		}
	};

	namespace Storage
	{
		/// Experimental spinlock.
		struct slock
		{
			flag& f;

			slock(flag& _f) noexcept
				:
				  f(_f)
			{
				while(f.test_and_set(std::memory_order_acquire));
			}

			~slock() noexcept
			{
				f.clear(std::memory_order_release);
			}
		};

		///=====================================
		/// Thread-safe queue
		///=====================================

		template<typename T>
		class tsq
		{
		private:

			flag f = ATOMIC_FLAG_INIT;
			Semaphore& s;
			std::deque<T> queue;

		public:

			tsq(Semaphore& _s)
				:
				  s(_s)
			{}

			void push(const T& _t)
			{
				slock lk(f);
				queue.push_back(_t);
				s.notify_one();
			}

			void push(T&& _t)
			{
				slock lk(f);
				queue.emplace_back(std::move(_t));
				s.notify_one();
			}

			template<typename ... Args>
			void emplace(Args&& ... _args)
			{
				slock lk(f);
				queue.emplace_back(std::forward<Args>(_args)...);
				s.notify_one();
			}

			bool pop(T& _t)
			{
				slock lk(f);
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
				slock lk(f);
				return queue.empty();
			}

			void clear()
			{
				slock lk(f);
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
	public:

#ifdef TP_BENCH
		uint enqueue_duration = 0;
#endif

		/// Version string.
		inline static const std::string version{"0.2.6"};

	private:

		/// Task queue
		Storage::tsq<Task> queue;

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

		struct Semaphores
		{
			/// Bell for signalling threads to start
			/// processing the queues.
			Semaphore work;

			/// Bell for signalling that the threadpool
			/// is waiting for all tasks to finish.
			Semaphore sync;
		} semaphores;

		struct Stats
		{
			auint received;
			auint enqueued;
			auint assigned;
			auint completed;
			auint aborted;

			Stats()
				:
				  received(0),
				  enqueued(0),
				  assigned(0),
				  completed(0),
				  aborted(0)
			{}
		} stats;

		struct Workers
		{
			/// Thread bookkeeping
			mutex busy_mtx;
			hmap<thread_id, toggle> busy;

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
			  workers(_init_count),
			  semaphores{},
			  queue(semaphores.work)
		{
			resize(_init_count);
		}

		~ThreadPool() TP_EXCEPT
		{
			LOG("Destroying ThreadPool...");

			/// Indicate that the threadpool has reached EOL.
			flags.stop.store(true);
			sync();

			/// Spin until all workers have exited.
			while (workers.count);

			LOG("\n==========[ Task statistics ]===========",
			   "\nReceived:\t", stats.received,
			   "\nEnqueued:\t", stats.enqueued,
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

#ifdef TP_BENCH
			auto start(std::chrono::high_resolution_clock::now());
#endif

			/// Using a conditional wrapper to avoid dangling references.
			/// Courtesy of https://stackoverflow.com/a/46565491/4639195.
			auto task(std::make_shared<std::packaged_task<Ret()>>(std::bind(std::forward<F>(_f), wrap(std::forward<Args>(_args))...)));

			std::future<Ret> future(task->get_future());

			if (flags.stop)
			{
				return future;
			}

			queue.emplace([=]{ (*task)(); });
			++stats.received;
			++stats.enqueued;

#ifdef TP_BENCH
			uint ns(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - start).count());
			enqueue_duration += ns;
			Debug::log("Enqueue took ", ns, " ns");
#endif
			LOG("New task received (Received: ", stats.received, ", enqueued: ", stats.enqueued, ")")

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
			while (workers.count < workers.target_count)
			{
				thread_id id(add_worker());
				while(workers.busy[id]);
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
			stats.aborted += stats.enqueued;
			stats.enqueued.store(0);
			queue.clear();

			semaphores.sync.notify_all();
		}

		void sync()
		{
			semaphores.work.notify_all();

			LOG("Waiting for tasks to finish...")

			semaphores.sync.wait([&]
			{
				return !stats.enqueued;
			});
		}

		void pause()
		{
			flags.pause.store(true);
		}

		void resume()
		{
			flags.pause.store(false);
			semaphores.work.notify_all();
		}

		uint worker_count()
		{
			return workers.count;
		}

		uint tasks_enqueued()
		{
			return stats.enqueued;
		}

		uint tasks_assigned()
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

		thread_id add_worker()
		{
			std::promise<thread_id> ready_promise;
			std::future<thread_id> id(ready_promise.get_future());

			std::thread([&,rp = std::move(ready_promise)]() mutable
			{
				Task task;

				uint count(++workers.count);
				LOG("\tWorker ", count, " in thread ", std::this_thread::get_id(), " ready");

				{
					glock lk(workers.busy_mtx);
					workers.busy[std::this_thread::get_id()] = true;
				}

				auto& busy(workers.busy[std::this_thread::get_id()]);
				rp.set_value(std::this_thread::get_id());

				busy.store(false);

				while (true)
				{

					/// Block execution until we have something to process.
					semaphores.work.wait([&]
					{
						busy.store(queue.pop(task));
						LOG("Thread ", std::this_thread::get_id(), " waiting for work.");
						return (busy || flags.stop || flags.prune || !flags.pause);
					});

					if (busy)
					{
						/// Update the stats
						++stats.assigned;
						--stats.enqueued;

						LOG(stats.assigned, " task(s) assigned (", stats.enqueued, " enqueued)")

						/// Execute the task
						task();

						busy.store(false);

						/// Update the stats
						--stats.assigned;
						++stats.completed;

						LOG(stats.assigned, " task(s) assigned (", stats.enqueued, " enqueued)");

						if (!stats.enqueued)
						{
							LOG("Signalling that all tasks have been processed...");
							semaphores.sync.notify_all();
						}
					}
					else if (flags.prune ||
							 flags.stop)
					{
						break;
					}

				}

				{
					glock lk(workers.busy_mtx);
					workers.busy.erase(std::this_thread::get_id());
				}

				--workers.count;
				flags.prune.store(workers.count > workers.target_count);

				LOG("\tWorker ", count, " in thread ", std::this_thread::get_id(), " exiting...")

			}).detach();

			return id.get();
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
