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
#include "dlog.hpp"

namespace Async
{
	typedef std::unique_lock<std::mutex> ulock;
	typedef std::lock_guard<std::mutex> glock;
	typedef unsigned int uint;

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

			/// Process all remaing tasks and
			/// stop accepting new ones
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
					uint worker_id(0);
					ulock lk(mtx);
					worker_id = ++workers.count;
#ifdef DLOG_IN_THREADPOOL
					dlog() << "\tWorker in thread " << std::this_thread::get_id() << " ready";
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
#ifdef DLOG_IN_THREADPOOL
							dlog() << tasks.assigned << " task(s) assigned (" << tasks.queue.size() << " enqueued)";
#endif
							lk.unlock();

							/// Grab the topmost task and execute it
							function();

							lk.lock();

							--tasks.assigned;
#ifdef DLOG_IN_THREADPOOL
							dlog() << tasks.assigned << " task(s) assigned (" << tasks.queue.size() << " enqueued)";
#endif
							/// Notify all waiting threads that
							/// we have processed all tasks.
							if (tasks.queue.size() == 0 &&
								tasks.assigned == 0)
							{
#ifdef DLOG_IN_THREADPOOL
							dlog() << "Signalling that all tasks have been processed...";
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
#ifdef DLOG_IN_THREADPOOL
					dlog() << "\tWorker in thread " << std::this_thread::get_id() << " exiting";
#endif
				}).detach();
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
#ifdef DLOG_IN_THREADPOOL
				dlog() << "Notifying all threads that ThreadPool has reached EOL...";
#endif
				ulock lk(mtx);
				halt = true;
				workers.semaphore.notify_all();

				/// Make sure that the queue is empty and there are no processes assigned
				kill_switch.wait(lk, [&]{ return (tasks.assigned == 0 && workers.count == 0); });
#ifdef DLOG_IN_THREADPOOL
				dlog() << tasks.received << " task(s) completed successfully!";
#endif
			}

			template<typename F, typename ... Args>
			std::future<typename std::result_of<F(Args...)>::type> enqueue(F&& _f, Args&&... _args)
			{
				using ret_t = typename std::result_of<F(Args...)>::type;

				auto task(std::make_shared<std::packaged_task<ret_t()>>(std::bind(std::forward<F>(_f), std::forward<Args>(_args)...)));
				std::future<ret_t> result = task->get_future();

				{
					glock lk(mtx);
					++tasks.received;
					tasks.queue.emplace([=]{ (*task)(); });
#ifdef DLOG_IN_THREADPOOL
					dlog() << "New task received (" << tasks.received << " in total), " << tasks.queue.size() << " task(s) enqueued";
#endif
					workers.semaphore.notify_one();
				}

				return result;
			}

			inline void resize(const uint _pool_size)
			{
				uint new_pool_size(_pool_size);
				if (new_pool_size == workers.max)
				{
					return;
				}
				else if (new_pool_size == 0)
				{
					new_pool_size = std::thread::hardware_concurrency();
				}

				ulock lk(mtx);
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
				/// Stop the thread pool,
				/// empty the queue and
				/// kill all the threads
				glock lk(mtx);
				halt = true;
				while (!tasks.queue.empty())
				{
					tasks.queue.pop();
				}
				tasks.assigned = 0;
				workers.semaphore.notify_all();
				tasks.finished.notify_all();
			}

			inline void wait()
			{
				ulock lk(mtx);
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

			inline uint get_queue_size()
			{
				glock lk(mtx);
				return tasks.queue.size();
			}
	};
}
#endif // MAIN_HPP
