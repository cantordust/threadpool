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

#ifdef TP_BENCH
#include <chrono>
#endif

#define TP_STATS
//#define TP_LOG

#ifdef TP_THROW
#define TP_EXCEPT noexcept(false)
#else
#define TP_EXCEPT noexcept(true)
#endif

namespace Async
{
    ///=============================================================================
    ///	Aliases
    ///=============================================================================

    ///=====================================
    /// Atomic types
    ///=====================================
    using uint = unsigned int;
    using auint = std::atomic<uint>;
    using flag = std::atomic<bool>;

    ///=====================================
    /// Synchronisation	types
    ///=====================================
    /// Ordinary mutex
    using mutex = std::mutex;
    /// Lock-free mutex
    using LFMutex = std::atomic_flag;
    using cvar = std::condition_variable;
    using ulock = std::unique_lock<mutex>;
    using glock = std::lock_guard<mutex>;

    ///=====================================
    /// Hashmap and thread ID
    ///=====================================
    template<typename T1, typename T2>
    using hmap = std::unordered_map<T1, T2>;
    using thread_id = std::thread::id;

#ifdef TP_BENCH
    ///=====================================
    /// Time-related types
    ///=====================================
    using nsec = std::chrono::nanoseconds;
    using usec = std::chrono::microseconds;
    using msec = std::chrono::milliseconds;
    using clk = std::chrono::high_resolution_clock;
#endif /// TP_BENCH

    ///=====================================
    /// Smart pointers
    ///=====================================
    template<typename T>
    using sp = std::shared_ptr<T>;
    template<typename T>
    using up = std::unique_ptr<T>;

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

#ifdef TP_BENCH

    ///=============================================================================
    ///	Time-related functions
    ///=============================================================================

    template<typename T>
    static inline constexpr uint duration(const std::chrono::high_resolution_clock::time_point& _start)
    {
        return std::chrono::duration_cast<T>(std::chrono::high_resolution_clock::now() - _start).count();
    }

#endif /// TP_BENCH

    namespace Sync
    {
        ///=====================================
        /// A generic semaphore / mutex class.
        /// It can wait on arbitrary conditions
        /// and ignores spurious wake-ups.
        ///=====================================
        struct Semaphore
        {
            mutex mtx;
            cvar cv;

            template<typename Func>
            void wait(Func&& _check)
            {
                ulock lk(mtx);
                cv.wait(lk, [&, check = std::forward<Func>(_check)]
                {
                    return (check());
                });
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

        ///=====================================
        /// Lock-free mutex (spinlock).
        ///=====================================
        struct LFLock
        {
            LFMutex& f;

            LFLock(LFMutex& _f) noexcept
                :
                  f(_f)
            {
                while(f.test_and_set(std::memory_order_acquire));
            }

            ~LFLock() noexcept
            {
                f.clear(std::memory_order_release);
            }
        };
    }

    namespace Storage
    {
        using Sync::LFLock;
        using Sync::Semaphore;

//        ///=====================================
//        /// Thread-safe hashmap.
//        ///=====================================
//        template<typename K, typename V>
//        class TSTable
//        {
//        protected:

//            LFMutex mtx = ATOMIC_FLAG_INIT;
//            hmap<K, V> table;

//        public:

//            template<typename Key>
//            auto operator [](Key&& _key) -> decltype (auto)
//            {
//                LFLock lk(mtx);
//                return table[std::forward<Key>(_key)];

//            }
//            template<typename Key>
//            auto erase(Key&& _key) -> decltype (auto)
//            {
//                LFLock lk(mtx);
//                return table.erase(std::forward<Key>(_key));
//            }

//            bool has(const K& _k) const
//            {
//                LFLock lk(mtx);
//                return table.find(_k) != table.end();
//            }

//            bool is_empty() const
//            {
//                LFLock lk(mtx);
//                return table.empty();
//            }

//            void clear()
//            {
//                LFLock lk(mtx);
//                table.clear();
//            }
//        };

        ///=====================================
        /// Thread-safe queue.
        /// This is event-driven in the sense
        /// that it notifies one thread waiting
        /// on the condition variable when a
        /// new entry is inserted.
        ///=====================================
        template<typename T>
        class TSQueue
        {
        protected:

            LFMutex mtx = ATOMIC_FLAG_INIT;
            Semaphore& s;
            std::deque<T> queue;

        public:

            TSQueue(Semaphore& _s)
                :
                  s(_s)
            {}

            void push(const T& _t)
            {
                {
                    LFLock lk(mtx);
                    queue.push_back(_t);
                }
                s.notify_one();
            }

            void push(T&& _t)
            {
                {
                    LFLock lk(mtx);
                    queue.emplace_back(std::move(_t));
                }
                s.notify_one();
            }

            template<typename ... Args>
            void emplace(Args&& ... _args)
            {
                {
                    LFLock lk(mtx);
                    queue.emplace_back(std::forward<Args>(_args)...);
                }
                s.notify_one();
            }

            bool pop(T& _t)
            {
                {
                    LFLock lk(mtx);
                    if (queue.empty())
                    {
                        return false;
                    }
                    _t = std::move(queue.front());
                    queue.pop_front();
                }
                return true;
            }

            bool is_empty() const
            {
                LFLock lk(mtx);
                return queue.empty();
            }

            void clear()
            {
                LFLock lk(mtx);
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
#endif /// TP_BENCH

        /// Version string.
        inline static const std::string version{"0.3.1"};

        template<typename T> class Future;

        ///=====================================
        /// Promise wrapper with validity indicator.
        ///=====================================
        template<typename T, typename std::enable_if<std::is_default_constructible<T>::value>::type ...>
        class Promise
        {
        private:

            /// No synchronisation needed.
            /// This is stored as a read-only
            /// reference in the corresponding
            /// future, and reading happens
            /// after setting the promised value.
            bool invalid{false};

            bool done{false};

            std::promise<T> promise;

            friend class Future<T>;

        public:

            Promise() {}

            Promise(std::promise<T>&& _promise)
                :
                  promise(std::move(_promise))
            {}

            ~Promise()
            {
                if (!done)
                {
                    invalidate();
                }
            }

            void invalidate()
            {
                invalid = true;
                if constexpr (std::is_void<T>::value)
                {
                    set();
                }
                else
                {
                    set(T());
                }
                done = true;
            }

            constexpr void set()
            {
                promise.set_value();
                done = true;
            }

            template<typename Result>
            constexpr void set(Result&& _result)
            {
                promise.set_value(std::forward<Result>(_result));
                done = true;
            }

            std::future<T> get_future()
            {
                return promise.get_future();
            }
        };

        ///=====================================
        /// Future wrapper with validity indicator.
        ///=====================================
        template<typename T>
        class Future
        {
        private:

            const bool& invalid;
            const bool& done;
            std::future<T> future;

        public:

            Future(Promise<T>& _p)
                :
                  future(_p.get_future()),
                  invalid(_p.invalid),
                  done(_p.done)
            {}
        };

        ///=====================================
        /// Task base and executor
        ///=====================================
        class TaskBase : public std::enable_shared_from_this<TaskBase>
        {
        public:

            using TaskPtr = sp<TaskBase>;
            using LFLock = Sync::LFLock;

        protected:

            ThreadPool& tp;

            flag done{false};

            LFMutex mtx = ATOMIC_FLAG_INIT;

            TaskPtr followup{nullptr};

        protected:

            TaskBase(ThreadPool& _tp)
                :
                  tp(_tp)
            {}

        public:

            virtual ~TaskBase() {};
            virtual void run() = 0;

            template<typename Func, typename ... Args>
            void chain(Func&& _func, Args&& ... _args)
            {
                if (tp.flags.stop)
                {
                    return;
                }

                LFLock lk(mtx);
                if (!done)
                {
                    if (!followup)
                    {
                        followup = ThreadPool::make_task(tp, std::forward<Func>(_func), std::forward<Args>(_args)...);
                        followup->set_parent(shared_from_this());
                    }
                    else
                    {
                        followup->chain(std::forward<Func>(_func), std::forward<Args>(_args)...);
                    }
                }
                else
                {
                    tp.enqueue(std::forward<Func>(_func), std::forward<Args>(_args)...);
                }
            }

        private:

            virtual void set_parent(TaskPtr _parent) = 0;

        };

        using TaskPtr = TaskBase::TaskPtr;

        template<typename Func, typename Ret, typename ... Args>
        class TaskExecutor : public TaskBase
        {
        private:

            using LFLock = Sync::LFLock;

            std::function<Ret(Args&&...)> func;
            std::tuple<Args&& ...> args;

            Promise<Ret> promise;
            up<Future<Ret>> future = nullptr;

        public:

            TaskExecutor(ThreadPool& _tp, Func&& _func, Args&& ... _args)
                :
                  TaskBase(_tp),
                  func(std::forward<Func>(_func)),
                  args(std::forward<Args>(_args)...)
            {}

            virtual void run() override final
            {
                if constexpr (std::is_void<Ret>::value)
                {
                    std::apply(func, args);
                    promise.set();
                }
                else
                {
                    promise.set(std::apply(func, args));
                }

                /// Mark the task completed.
                done.store(true);

                if (followup)
                {
                    followup->run();
                }
            }

            ~TaskExecutor()
            {
                if (!done)
                {
                    promise.invalidate();
                }
            };

        private:

            static constexpr TaskExecutor<Func, Ret, Args...>& downcast(const TaskPtr& _task)
            {
                return static_cast<TaskExecutor<Func, Ret, Args...>&>(*_task);
            }

            virtual void set_parent(TaskPtr _parent) override final
            {
                future = std::make_unique<Future<Ret>>(downcast(_parent).promise);
            }

            friend class ThreadPool;
        };

    private:

        struct
        {
            flag stop{false};
            flag prune{false};
            flag pause{false};
        } flags;

#ifdef TP_STATS
        struct
        {
            auint received{0};
            auint enqueued{0};
            auint assigned{0};
            auint completed{0};
            auint aborted{0};
        } stats;
#endif /// TP_STATS

        struct
        {
            struct
            {
                auint current{0};
                auint target{0};
            } count;

//            /// Thread and task bookkeeping
//            Storage::TSTable<thread_id, flag> busy;
        } workers;

        struct
        {
            /// Semaphore for signalling workers
            /// to start processing the queue.
            Storage::Semaphore work;

            /// Semaphore for signalling that all tasks must
            /// finish processing before continuing.
            Storage::Semaphore sync;
        } semaphores;

        /// Task queue
//        Storage::TSQueue<TaskPtr> queue;
        std::deque<TaskPtr> queue;

    public:

        ThreadPool(const uint _init_count = std::thread::hardware_concurrency())
            :
//              semaphores{},
              queue(64)
        {
            resize(_init_count);
        }

        ~ThreadPool() TP_EXCEPT
        {
#ifdef TP_LOG
            Debug::log("Destroying ThreadPool...");
#endif /// TP_LOG

            /// Indicate that the threadpool has reached EOL.
            flags.stop.store(true);
            sync();

            /// Spin until all workers have exited.
            while (workers.count.current > 0);

#ifdef TP_STATS
#ifdef TP_LOG
            Debug::log("\n==========[ Task statistics ]===========",
               "\nReceived:\t", stats.received,
               "\nEnqueued:\t", stats.enqueued,
               "\nAssigned:\t", stats.assigned,
               "\nCompleted:\t", stats.completed,
               "\nAborted:\t", stats.aborted,
               "\n");
#endif /// TP_LOG

            if (stats.received != stats.assigned + stats.completed + stats.aborted)
            {
#ifdef TP_THROW
                throw std::out_of_range("\n!!! ERROR: !!! Some tasks have been lost along the way!\n");
#else /// TP_THROW
#ifdef TP_LOG
                Debug::log("\n!!! ERROR: !!! Some tasks have been lost along the way!\n");
#endif /// TP_LOG
#endif /// TP_THROW
            }
#endif /// TP_STATS
        }

        template<typename Func, typename ... Args>
        TaskPtr enqueue(Func&& _func, Args&&... _args)
        {

#ifdef TP_BENCH
            auto start(clk::now());
#endif /// TP_BENCH
            if (flags.stop)
            {
                return nullptr;
            }

            TaskPtr task(make_task(*this, std::forward<Func>(_func), std::forward<Args>(_args)...));

            {
                glock lk{semaphores.work.mtx};
                queue.push_back(task);
            }

#ifdef TP_BENCH
            uint timespan(duration<nsec>(start));
            enqueue_duration += timespan;
            Debug::log("Enqueue took ", timespan, " ns");
#endif /// TP_BENCH

            semaphores.work.notify_one();

#ifdef TP_STATS
            ++stats.received;
            ++stats.enqueued;
#endif /// TP_STATS

#ifdef TP_LOG
            Debug::log("New task received (Received: ", stats.received, ", enqueued: ", stats.enqueued, ")");
#endif /// TP_LOG

            return task;
        }

        void resize(const uint _count)
        {
            if (flags.stop)
            {
                return;
            }

            workers.count.target.store(_count);
            flags.prune.store((workers.count.current > workers.count.target));
            while (workers.count.current < workers.count.target)
            {
                add_worker();
//                while(workers.busy[id]);
            }
        }

        void stop()
        {
            if (flags.stop)
            {
#ifdef TP_LOG
                Debug::log("Threadpool already stopped.");
#endif /// TP_LOG
                return;
            }
#ifdef TP_LOG
            Debug::log("Stopping threadpool...");
#endif /// TP_LOG

            /// Empty the queue
#ifdef TP_STATS
            stats.aborted += stats.enqueued;
            stats.enqueued.store(0);
#endif /// TP_STATS

            {
                glock lk{semaphores.work.mtx};
                queue.clear();
                flags.stop.store(true);
                workers.count.target.store(0);
            }

            semaphores.sync.notify_all();
        }

        void sync()
        {
            semaphores.work.notify_all();

#ifdef TP_LOG
            Debug::log("Waiting for tasks to finish...");
#endif /// TP_LOG

            semaphores.sync.wait([&]
            {
                return (stats.enqueued == 0);
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
            return workers.count.current;
        }

#ifdef TP_STATS
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
#endif /// TP_STATS

    private:

        template<typename Func, typename ... Args>
        static TaskPtr make_task(ThreadPool& _tp, Func&& _func, Args&& ... _args)
        {
            using Ret = typename std::invoke_result<Func, Args...>::type;
            return std::make_shared<TaskExecutor<Func, Ret, Args...>>(_tp, std::forward<Func>(_func), std::forward<Args>(_args)...);
        }

        thread_id add_worker()
        {
            std::promise<thread_id> ready_promise;
            std::future<thread_id> id(ready_promise.get_future());

            std::thread([&,rp = std::move(ready_promise)]() mutable
            {
                bool busy{false};

                /// Indicate that the worker is busy (initialising).
//                workers.busy[std::this_thread::get_id()].store(true);

                /// Set this worker's thread ID in the busy flag table.
//                auto& busy(workers.busy[std::this_thread::get_id()]);

                rp.set_value(std::this_thread::get_id());

                TaskPtr task(nullptr);

                uint count(++workers.count.current);
#ifdef TP_LOG
                Debug::log("\tWorker ", count, " in thread ", std::this_thread::get_id(), " ready");
#endif /// TP_LOG

//                busy.store(false);
                do
                {
                    /// Block execution until we have something to process.
                    semaphores.work.wait([&]() -> bool
                    {
                        if (queue.empty() || flags.pause)
                        {
                            return false;
                        }
#ifdef TP_LOG
                        Debug::log("Thread ", std::this_thread::get_id(), " waiting for work.");
#endif /// TP_LOG
                        task = std::move(queue.front());
                        queue.pop_front();
                        return true;
                    });

                    if (task)
                    {

#ifdef TP_STATS
                        /// Update the stats.
                        ++stats.assigned;
                        --stats.enqueued;
#endif /// TP_STATS

#ifdef TP_LOG
                        Debug::log(stats.assigned, " task(s) assigned (", stats.enqueued, " enqueued)");
#endif /// TP_LOG

                        /// Run the task.
                        task->run();

                        task = nullptr;

#ifdef TP_STATS
                        /// Update the stats
                        --stats.assigned;
                        ++stats.completed;
#endif /// TP_STATS

#ifdef TP_LOG
                        Debug::log(stats.assigned, " task(s) assigned (", stats.enqueued, " enqueued)");
                        Debug::log("Signalling that all tasks have been processed...");
#endif /// TP_LOG

                        semaphores.sync.notify_one();
                    }
                } while (! (flags.stop || workers.count.current == workers.count.target));

                /// Remove the busy flag from the table.
//                workers.busy.erase(std::this_thread::get_id());

                --workers.count.current;

#ifdef TP_LOG
                Debug::log("\tWorker ", count, " in thread ", std::this_thread::get_id(), " exiting...");
#endif /// TP_LOG

            }).detach();

            return id.get();
        }

        friend class TaskBase;
    };
}
#endif // THREADPOOL_HPP
