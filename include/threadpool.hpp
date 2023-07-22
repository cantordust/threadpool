#ifndef THREADPOOL_HPP
#define THREADPOOL_HPP

// #include <deque>
// #include <condition_variable>
#include <array>
#include <vector>
#include <future>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <iostream>
#include <syncstream>
#include <type_traits>
#include <typeinfo>
#include <unordered_set>
#include <unordered_map>

std::mutex out_mtx;

/// Rudimentary debug printing.
template<typename ... Args>
void log(Args&& ... _args)
{
    std::lock_guard lk{ out_mtx };
    std::array<int, sizeof...(_args)> status{ (std::cout << std::forward<Args>(_args), 0) ... };
    std::cout << '\n';
}

namespace Async
{
    using uint = unsigned int;
    using auint = std::atomic<uint>;
    using flag = std::atomic<bool>;
    using lguard = std::lock_guard<std::mutex>;
    using tid = std::thread::id;
    template<class K, class V>
    using hmap = std::unordered_map<K, V>;
    template<class K>
    using hset = std::unordered_set<K>;
    template<class T>
    using sptr = std::shared_ptr<T>;
    template<class T>
    using uptr = std::unique_ptr<T>;

    struct TaskBase
    {
        virtual ~TaskBase() {};
        virtual void operator()() = 0;
    };

    using Task = uptr<TaskBase>;

    template<typename F, typename Ret, typename ... Args>
    struct TaskExecutor: TaskBase
    {
        std::function<Ret(Args&&...)> _func;
        std::tuple<Args&& ...> _args;

        constexpr TaskExecutor(F&& func, Args&& ... args)
            :
            _func(std::forward<F>(func)),
            _args(std::forward<Args>(args)...)
        {
        }

        ~TaskExecutor() {};

        void operator()() override final
        {
            std::apply(_func, _args);
        }
    };

    ///=============================================================================
    ///	Main feature
    ///=============================================================================

    /// @class Threadpool
    class ThreadPool
    {

#ifdef TP_BENCH
    public:

        uint enqueue_duration{ 0 };
        uint calls{ 0 };

        auint swap_duration{ 0 };
        auint swaps{ 0 };

#endif

    private:

        /// Task queue.
        std::vector<Task> buffer;

        /// Mutex for the lock guards.
        std::mutex mtx;

        // Flag for signalling threads that they should quit.
        flag halt{ false };

        // Worker counter
        auint workers{ 0 };

        std::vector<std::jthread> threads;

    public:

        ThreadPool(const uint size = std::thread::hardware_concurrency())
        {
            buffer.reserve(1024);

            for (uint i = 0; i < size; ++i)
            {
                threads.emplace_back(add_worker());
            }

        }

        ~ThreadPool() noexcept
        {
            halt.store(true);

            // semaphore.notify_all();

            log("Waiting for tasks to finish...");

            while (workers.load() > 0);
        }

        template<typename F, typename ... Args>
        void enqueue(F&& fun, Args&& ... args)
        {

            if (halt.load()) return;


#ifdef TP_BENCH
            auto start(std::chrono::steady_clock::now());
#endif
            {
                const lguard lg{ mtx };
                buffer.emplace_back(make_task(std::forward<F>(fun), std::forward<Args>(args)...));
                //                log("Enqueue | buffer size ", buffer.size());
            }

#ifdef TP_BENCH
            uint ns(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
            enqueue_duration += ns;
            ++calls;
            // log("Enqueue took ", ns, " ns");
            // log("New task enqueued (Received: ", calls, ", enqueued: ", buffer.size(), ")");
#endif
        }

        void stop()
        {
            if (halt.load())
            {
                log("Threadpool already stopped.");
                return;
            }

            log("Stopping threadpool...");

            {

                lguard lg{ mtx };

                halt.store(true);
                /// Empty the queue
                buffer.clear();
            }
        }

    private:

        template<typename Func, typename ... Args>
        Task make_task(Func&& fun, Args&& ... args)
        {
            using Ret = typename std::invoke_result<Func, Args...>::type;
            return std::make_unique<TaskExecutor<Func, Ret, Args...>>(std::forward<Func>(fun), std::forward<Args>(args)...);
        }

        std::jthread add_worker()
        {
            return std::jthread([&]() mutable
                {
                    ++workers;

                    std::vector<Task> _buffer;

                    _buffer.reserve(1024);

                    while (!(halt.load() && buffer.empty()))
                    {

#ifdef TP_BENCH
                        auto start(std::chrono::steady_clock::now());
#endif
                        {
                            const lguard lg(mtx);
                            //                            log("Swapping | _buffer size: ", _buffer.size(), " | buffer size: ", buffer.size());
                            _buffer.swap(buffer);
                        }

#ifdef TP_BENCH
                        uint ns(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
                        swap_duration += ns;
                        ++swaps;
#endif

                        for (uint i = 0; i < _buffer.size(); ++i)
                        {
                            /// Execute the task
                            (*_buffer[i])();
                        }
                        _buffer.clear();
                        // log("Finished | buffer size: ", _buffer.size());
                    }

                    --workers;

                });

        }

    };
}
#endif // THREADPOOL_HPP
