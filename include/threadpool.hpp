#ifndef THREADPOOL_HPP
#define THREADPOOL_HPP

#include <queue>
#include <condition_variable>
#include <future>
#include <thread>
#include <memory>
#include <mutex>

#ifdef TP_DEBUG

#include <iostream>
#include <string>
#include <sstream>

#endif

namespace Async
{
	using uint = unsigned int;
	using ulock = std::unique_lock<std::mutex>;
	using glock = std::lock_guard<std::mutex>;

#ifdef TP_DEBUG

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
#endif

	class ThreadPool
	{
	private:

		/// All shared data should be modified after locking this mutex
		std::mutex mtx;

		/// Task queue
		std::queue<std::function<void()>> queue;

		/// Condition variable used for signalling
		/// other threads that the processing has finished.
		std::condition_variable finished;

		std::condition_variable semaphore;

		struct
		{
			bool stop = false;
			bool prune = false;
			bool pause = false;
		} flags;

		struct
		{
			uint received = 0;
			uint assigned = 0;
			uint completed = 0;
			uint aborted = 0;
		} stats;

		struct
		{
			uint count;
			uint target_count;
		} workers;

	public:

		ThreadPool(const uint _init_count = std::thread::hardware_concurrency())
			:
			  workers({0, _init_count})
		{
			resize(_init_count);
		}

		~ThreadPool()
		{
			flags.stop = true;
			wait();

#ifdef TP_DEBUG
			dp() << "Task statistics:\n"
				 << "\treceived: " << stats.received << "\n"
				 << "\tassigned: " << stats.assigned << "\n"
				 << "\tcompleted: " << stats.completed << "\n"
				 << "\taborted: " << stats.aborted;

			if (stats.received != stats.assigned + stats.completed + stats.aborted)
			{
				dp() << "Some tasks have been lost along the way!";
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
				glock lk(mtx);
				if (!flags.stop)
				{
					++stats.received;
					{
						queue.emplace([=]{ (*task)(); });
#ifdef TP_DEBUG
						dp() << "New task received (" << stats.received << " in total), " << queue.size() << " task(s) enqueued";
#endif
					}
					semaphore.notify_one();
				}
			}

			return result;
		}

		inline void resize(const uint _count)
		{
			glock lk(mtx);
			if (flags.stop)
			{
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

			if (flags.stop)
			{
#ifdef TP_DEBUG
				dp() << "Threadpool already stopped.";
#endif
				return;
			}
#ifdef TP_DEBUG
			dp() << "Stopping threadpool...";
#endif
			flags.stop = true;
			/// Empty the queue
			{
				while (!queue.empty())
				{
					queue.pop();
					++stats.aborted;
				}
			}
		}

		inline void wait()
		{
			semaphore.notify_all();
#ifdef TP_DEBUG
			dp() << "Waiting for tasks to finish...";
#endif
			ulock lk(mtx);
			finished.wait(lk, [&] { return (queue.empty() && stats.assigned == 0); });
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
			semaphore.notify_all();
		}

		inline uint worker_count()
		{
			glock lk(mtx);
			return workers.count;
		}

		inline uint tasks_enqueued()
		{
			glock lk(mtx);
			return queue.size();
		}

		inline uint tasks_received()
		{
			glock lk(mtx);
			return stats.received;
		}

		inline uint tasks_completed()
		{
			glock lk(mtx);
			return stats.completed;
		}

		inline uint tasks_aborted()
		{
			glock lk(mtx);
			return stats.aborted;
		}

	private:

		inline void add_worker()
		{
			std::thread([&]
			{
				ulock lk(mtx);
				std::function<void()> task;
				++workers.count;
#ifdef TP_DEBUG
				uint worker_id(workers.count);
				dp() << "\tWorker " << worker_id << " in thread " << std::this_thread::get_id() << " ready";
#endif
				while (true)
				{
					/// Block execution until we have something to process.
					semaphore.wait(lk, [&]{ return flags.stop || flags.prune || !(flags.pause || queue.empty()); });

					if ((flags.stop && queue.empty()) ||
						flags.prune )
					{
						break;
					}

					if (!queue.empty())
					{
						task = std::move(queue.front());
						queue.pop();

						/// Execute the task
						++stats.assigned;

#ifdef TP_DEBUG
						dp() << stats.assigned << " task(s) assigned (" << queue.size() << " enqueued)";
#endif

						lk.unlock();
						task();
						lk.lock();

						--stats.assigned;
						++stats.completed;
#ifdef TP_DEBUG
						dp() << stats.assigned << " task(s) assigned (" << queue.size() << " enqueued)";
#endif
					}

					if (queue.empty() &&
						stats.assigned == 0)
					{
#ifdef TP_DEBUG
						dp() << "Indicating that all tasks have been processed...";
#endif
						finished.notify_all();
					}
				}

				--workers.count;
				flags.prune = (workers.count > workers.target_count);

#ifdef TP_DEBUG
				dp() << "\tWorker " << worker_id << " in thread " << std::this_thread::get_id() << " exiting...";
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
	};
}
#endif // THREADPOOL_HPP
