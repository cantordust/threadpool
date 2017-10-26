#ifndef MAIN_HPP
#define MAIN_HPP

#include <iostream>
#include <unordered_map>
#include <string>
#include <sstream>
#include <queue>
#include <condition_variable>
#include <future>
#include <thread>
#include <memory>
#include <random>
#include <mutex>
#include <type_traits>

namespace Async
{
	typedef unsigned int uint;

	typedef std::unique_lock<std::mutex> ulock;
	typedef std::lock_guard<std::mutex> glock;

	/// Mutex for outputting to std::cout
	static std::mutex dp_mutex;

	/// Rudimentary debug printer class.
	class dp
	{
		std::stringstream buf;

	public:

		~dp()
		{
			glock lk(dp_mutex);
			std::cout << buf.str() << "\n";
		}

		template<typename T>
		dp& operator<< (const T& _t)
		{
			buf << _t;
			return *this;
		}
	};

	class ThreadPool
	{
	private:

		/// All shared data should be modified after locking this mutex
		std::mutex mtx;

		/// Kill switch indicating that it is
		/// OK to destroy the ThreadPool object
		std::condition_variable kill_switch;

		struct Flags
		{
			/// Empty the queue and stop receiving new tasks.
			bool halt;

			/// Destroy some threads.
			bool prune;

			/// Pause the execution.
			bool pause;

			Flags()
				:
				  halt(false),
				  prune(false),
				  pause(false)
			{}
		} flags;

		struct Tasks
		{
			uint received;
			uint assigned;
			uint completed;
			uint aborted;

			/// Task queue
			std::queue<std::function<void()>> queue;

			/// Condition variable used for signalling
			/// other threads that the processing has finished.
			std::condition_variable finished;

			std::condition_variable semaphore;

			Tasks()
				:
				  received(0),
				  assigned(0),
				  completed(0),
				  aborted(0)
			{}
		} tasks;

		struct Workers
		{
			uint target_count;
			uint count;

			Workers(const uint _target_count)
				:
				  target_count(_target_count),
				  count(0)
			{}
		} workers;

		inline void add_worker()
		{
			std::thread([&]
			{
				ulock lk(mtx);
				uint worker_id(++workers.count);

#ifdef TP_DEBUG
				dp() << "\tWorker " << worker_id << " in thread " << std::this_thread::get_id() << " ready";
#endif
				std::function<void()> function;

				while (true)
				{
					/// Pause the execution.
					tasks.semaphore.wait(lk, [&]{ return !flags.pause; });

					/// Block execution until we have something to process.
					tasks.semaphore.wait(lk, [&]{ return flags.halt || flags.prune || !tasks.queue.empty(); });

					/// Break from the loop	if required
					if ((flags.halt && tasks.queue.empty()) ||
						workers.count > workers.target_count)
					{
						break;
					}

					if (!tasks.queue.empty())
					{
						function = std::move(tasks.queue.front());
						tasks.queue.pop();

						/// Execute the task
						++tasks.assigned;

#ifdef TP_DEBUG
						dp() << tasks.assigned << " task(s) assigned (" << tasks.queue.size() << " enqueued)";
#endif

						lk.unlock();
						function();
						lk.lock();

						--tasks.assigned;
						++tasks.completed;
#ifdef TP_DEBUG
						dp() << tasks.assigned << " task(s) assigned (" << tasks.queue.size() << " enqueued)";
#endif
					}

					/// Notify all waiting threads that
					/// we have processed all tasks.
					if (tasks.assigned == 0 &&
						tasks.queue.empty())
					{
#ifdef TP_DEBUG
						dp() << "Signalling that all tasks have been processed...";
#endif
						tasks.finished.notify_all();
					}
				}

				--workers.count;
				flags.prune = (workers.count > workers.target_count);

				if (workers.count == 0)
				{
					kill_switch.notify_one();
				}
#ifdef TP_DEBUG
				dp() << "\tWorker " << worker_id << " in thread " << std::this_thread::get_id() << " exiting";
#endif

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

	public:

		ThreadPool(const uint _initial_workers = std::thread::hardware_concurrency())
			:
			  workers(_initial_workers)
		{
			resize(_initial_workers);
		}

		~ThreadPool()
		{
#ifdef TP_DEBUG
			dp() << "Notifying all threads that threadpool has reached EOL...";
#endif
			ulock lk(mtx);
			flags.halt = true;
			tasks.semaphore.notify_all();

			/// Make sure that the queue is empty and there are no processes assigned
			kill_switch.wait(lk, [&]
			{
				return (tasks.assigned == 0 && tasks.queue.empty() && workers.count == 0);
			});

			tasks.finished.notify_all();

#ifdef TP_DEBUG
			dp() << "Task statistics:\n"
				 << "\treceived: " << tasks.received << "\n"
				 << "\tassigned: " << tasks.assigned << "\n"
				 << "\tcompleted: " << tasks.completed << "\n"
				 << "\taborted: " << tasks.aborted;
#endif
		}

		template<typename F, typename ... Args>
		auto enqueue(F&& _f, Args&&... _args)
		{
			using ret_t = typename std::result_of<F& (Args&...)>::type;

			/// Using a conditional wrapper to avoid dangling references.
			/// Courtesy of https://stackoverflow.com/a/46565491/4639195.
			auto task(std::make_shared<std::packaged_task<ret_t()>>(std::bind(std::forward<F>(_f), wrap(std::forward<Args>(_args))...)));

			std::future<ret_t> result = task->get_future();

			{
				ulock lk(mtx);
				if (!flags.halt)
				{
					++tasks.received;
					{
						tasks.queue.emplace([=]{ (*task)(); });
#ifdef TP_DEBUG
						dp() << "New task received (" << tasks.received << " in total), " << tasks.queue.size() << " task(s) enqueued";
#endif
					}
					lk.unlock();
					tasks.semaphore.notify_one();
				}
#ifdef TP_DEBUG
				else
				{
					dp() << "Threadpool stopped, not accepting new tasks.";
				}
#endif
			}

			return result;
		}

		inline void resize(const uint _count)
		{
			glock lk(mtx);
			if (flags.halt)
			{
#ifdef TP_DEBUG
				dp() << "Threadpool stopped, resizing not allowed.";
#endif
				return;
			}

			workers.target_count = _count;
			flags.prune = (workers.count > workers.target_count);
			if (workers.count < workers.target_count)
			{
				for (uint i = 0; i < workers.target_count - workers.count; ++i)
				{
					add_worker();
				}
			}
		}

		inline void stop()
		{
			glock lk(mtx);
			if (flags.halt)
			{
#ifdef TP_DEBUG
				dp() << "Threadpool already stopped.";
#endif
				return;
			}

			flags.halt = true;

			/// Empty the queue
			{
				while (!tasks.queue.empty())
				{
					tasks.queue.pop();
					++tasks.aborted;
				}
			}

			tasks.semaphore.notify_all();
		}

		inline void wait()
		{
			ulock lk(mtx);
			if (flags.halt)
			{
#ifdef TP_DEBUG
				dp() << "Threadpool stopped, not waiting.";
#endif
				return;
			}

			tasks.finished.wait(lk, [&] { return (tasks.queue.empty() && tasks.assigned == 0); });
		}

		inline void pause()
		{
			glock lk(mtx);
			flags.pause = true;
		}

		inline void resume()
		{
			glock lk(mtx);
			flags.pause = false;
		}

		inline uint worker_count()
		{
			glock lk(mtx);
			return workers.count;
		}

		inline uint tasks_enqueued()
		{
			glock lk(mtx);
			return tasks.queue.size();
		}

		inline uint tasks_received()
		{
			glock lk(mtx);
			return tasks.received;
		}

		inline uint tasks_completed()
		{
			glock lk(mtx);
			return tasks.completed;
		}

		inline uint tasks_aborted()
		{
			glock lk(mtx);
			return tasks.aborted;
		}

	};
}
#endif // MAIN_HPP
