#ifndef MAIN_HPP
#define MAIN_HPP

#include <iostream>
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
	typedef std::unique_lock<std::mutex> ulock;
	typedef std::lock_guard<std::mutex> glock;
	typedef unsigned int uint;

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

		struct
		{
			uint assigned = 0;
			uint received = 0;

			/// Task queue
			std::queue<std::function<void()>> queue;

			/// Condition variable used for signalling
			/// other threads that the processing has finished.
			std::condition_variable finished;
		} tasks;

		struct
		{
			uint max = 0;
			uint count = 0;
			std::condition_variable semaphore;
			std::condition_variable ready;
		} workers;

		/// Stop accepting new tasks.
		bool halt;

		/// Used to indicate that we want to kill some threads
		bool prune;

		/// Kill switch indicating that it is
		/// OK to destroy the ThreadPool object
		std::condition_variable kill_switch;

		/// All shared data should be modified after locking this mutex
		std::mutex mtx;

		inline void add_worker()
		{
			std::thread([&]
			{
				ulock lk(mtx);
				uint worker_id(++workers.count);

#ifdef TP_DEBUG
				dp() << "\tWorker " << worker_id << " in thread " << std::this_thread::get_id() << " ready";
#endif
				workers.ready.notify_one();

				while (true)
				{
					/// Block execution until we have something to process
					workers.semaphore.wait(lk, [&]{ return (prune || halt || !tasks.queue.empty()); });
					if (halt && tasks.queue.empty())
					{
						break;
					}
					else if (prune)
					{
						if (workers.count > workers.max)
						{
							break;
						}
						else
						{
							prune = false;
							workers.ready.notify_one();
						}
					}
					else if (!tasks.queue.empty())
					{
						std::function<void()> function(std::move(tasks.queue.front()));
						tasks.queue.pop();
						++tasks.assigned;
#ifdef TP_DEBUG
						dp() << tasks.assigned << " task(s) assigned (" << tasks.queue.size() << " enqueued)";
#endif
						lk.unlock();

						/// Execute the task
						function();

						lk.lock();

						--tasks.assigned;
#ifdef TP_DEBUG
						dp() << tasks.assigned << " task(s) assigned (" << tasks.queue.size() << " enqueued)";
#endif
						/// Notify all waiting threads that
						/// we have processed all tasks.
						if (tasks.queue.empty() &&
							tasks.assigned == 0)
						{
#ifdef TP_DEBUG
							dp() << "Signalling that all tasks have been processed...";
#endif
							tasks.finished.notify_all();
						}
					}
				}

				--workers.count;

				workers.ready.notify_one();
				if (halt)
				{
					kill_switch.notify_one();
				}
#ifdef TP_DEBUG
				dp() << "\tWorker in thread " << std::this_thread::get_id() << " exiting";
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

		ThreadPool(const uint _pool_size = std::thread::hardware_concurrency())
			:
			  halt(false),
			  prune(false)
		{
			resize(_pool_size);
		}

		~ThreadPool()
		{
#ifdef TP_DEBUG
			dp() << "Notifying all threads that threadpool has reached EOL...";
#endif
			ulock lk(mtx);
			halt = true;
			workers.semaphore.notify_all();

			/// Make sure that the queue is empty and there are no processes assigned
			kill_switch.wait(lk, [&]
			{
				return (tasks.assigned == 0 && tasks.queue.empty() && workers.count == 0);
			});
			tasks.finished.notify_all();

#ifdef TP_DEBUG
			dp() << tasks.received << " task(s) completed successfully!";
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
				glock lk(mtx);
				if (!halt)
				{
					++tasks.received;
					tasks.queue.emplace([=]{ (*task)(); });
#ifdef TP_DEBUG
					dp() << "New task received (" << tasks.received << " in total), " << tasks.queue.size() << " task(s) enqueued";
#endif
					workers.semaphore.notify_one();
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

		inline void resize(const uint _pool_size)
		{
			ulock lk(mtx);
			if (halt)
			{
#ifdef TP_DEBUG
				dp() << "Threadpool stopped, resizing not allowed.";
#endif
				return;
			}

			uint new_pool_size(_pool_size);
			if (new_pool_size == workers.max)
			{
				return;
			}
			else if (new_pool_size == 0)
			{
				new_pool_size = std::thread::hardware_concurrency();
			}

			workers.max = new_pool_size;
			if (workers.max > workers.count)
			{
				for (uint i = 0; i < workers.max - workers.count; ++i)
				{
					add_worker();
				}
			}
			else
			{
				prune = true;
				workers.semaphore.notify_all();
			}
			workers.ready.wait(lk, [&]{ return workers.count == workers.max; });
		}

		inline void stop()
		{
			{
				glock lk(mtx);
				if (halt)
				{
#ifdef TP_DEBUG
					dp() << "Threadpool already stopped.";
#endif
					return;
				}
			}

			glock lk(mtx);
			halt = true;

			/// Empty the queue
			while (!tasks.queue.empty())
			{
				tasks.queue.pop();
			}
		}

		inline void wait()
		{
			ulock lk(mtx);
			if (halt)
			{
				return;
			}
			tasks.finished.wait(lk, [&]{ return (tasks.queue.empty() && tasks.assigned == 0); });
		}

		inline void pause()
		{
			/// \todo Pause
		}

		inline void resume()
		{
			/// \todo Resume
		}

		inline uint queue_size()
		{
			glock lk(mtx);
			return tasks.queue.size();
		}

		inline uint worker_count()
		{
			glock lk(mtx);
			return workers.count;
		}
	};
}
#endif // MAIN_HPP
