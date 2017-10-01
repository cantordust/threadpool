#ifndef DLOG_HPP
#define DLOG_HPP

#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <future>
#include <vector>
#include <queue>
#include <memory>

namespace Async
{
	typedef unsigned int uint;
	typedef std::unique_lock<std::mutex> ulock;
	typedef std::lock_guard<std::mutex> glock;
	template<typename T> using sptr = std::shared_ptr<T>;

	// Mutex for accessing the associated stream.
	static std::mutex output_mutex;

	/// \brief The dlog class
	/// \details
	/// Dlog ("debug log") is a tiny header-only debug log library
	/// for printing output from multiple threads into arbitrary streams
	/// without garbling the output.
	class dlog
	{
		private:

			// Static members for various defaults.
			static uint& default_log_level()
			{
				static uint d_log_level = 1;
				return d_log_level;
			}
			static std::string& default_prefix()
			{
				static std::string d_prefix = "";
				return d_prefix;
			}

			static std::string& default_infix()
			{
				static std::string d_infix = "";
				return d_infix;
			}

			static std::string& default_suffix()
			{
				static std::string d_suffix = "";
				return d_suffix;
			}

			// Stream associated with this log.
			std::ostream& strm;

			// Member vars for serialising futures in the right order and
			// managing the internal buffer.

			std::queue<std::function<void()>> output_queue;

			// Strings appended to the input.
			// If not set in the constructor, these
			// are given the default values.
			sptr<std::string> prefix;
			sptr<std::string> infix;
			sptr<std::string> suffix;

			sptr<std::stringstream> buffer;

			// Variable determining whether
			// the input is printed or ignored.
			bool out;

			class AsyncPrinter
			{
				private:

					std::mutex printer_mutex;
					std::queue<std::queue<std::function<void()>>> printer_queue;
					std::condition_variable printer_semaphore;
					std::condition_variable kill_switch;
					uint threads_running;

				public:

					AsyncPrinter()
						:
						  threads_running(0)
					{

					}

					~AsyncPrinter()
					{
						ulock printer_lock(printer_mutex);
						kill_switch.wait(printer_lock, [&]{ return threads_running == 0; });
					}

					inline void enqueue(std::queue<std::function<void()>>&& _queue)
					{
						glock queue_lock(printer_mutex);
						printer_queue.emplace(std::move(_queue));

						std::thread([&]
						{
							std::queue<std::function<void()>> queue;
							std::function<void()> fun;

							{
								ulock printer_lock(printer_mutex);
								printer_semaphore.wait(printer_lock, [&]{ return !printer_queue.empty(); });
								queue = std::move(printer_queue.front());
								printer_queue.pop();
							}

							while (!queue.empty())
							{
								fun = std::move(queue.front());
								queue.pop();
								fun();
							}

							{
								glock printer_lock(printer_mutex);
								--threads_running;
								kill_switch.notify_one();
							}
						}).detach();
					}

					inline void add_request()
					{
						glock printer_lock(printer_mutex);
						++threads_running;
					}

					inline static AsyncPrinter& printer()
					{
						static AsyncPrinter p;
						return p;
					}
			};

		public:

			dlog(const uint _log_level = 0,
				 std::ostream& _strm = std::cout,
				 const std::string _prefix = "",
				 const std::string _infix = "",
				 const std::string _suffix = "")
				:
				  strm(_strm),
				  prefix(std::make_shared<std::string>(_prefix.empty() ? get_default_prefix() : _prefix)),
				  infix(std::make_shared<std::string>(_infix.empty() ? get_default_infix() : _infix)),
				  suffix(std::make_shared<std::string>(_suffix.empty() ? get_default_suffix() : _suffix)),
				  buffer(std::make_shared<std::stringstream>()),
				  out(_log_level == 0 || _log_level >= get_default_log_level())
			{
				if (out)
				{
					AsyncPrinter::printer().add_request();
					auto task(std::make_shared<std::packaged_task<void()>>([buf = buffer, pref = prefix]{ *buf << *pref; }));
					output_queue.emplace([=]{ (*task)(); });
				}
			}

			~dlog()
			{
				if (out)
				{
					auto task(std::make_shared<std::packaged_task<void()>>([buf = buffer, suf = suffix, &stream = strm]
					{
						*buf << *suf << '\n';
						glock output_lock(output_mutex);
						stream << buf->str();
					}));
					output_queue.emplace([=]{ (*task)(); });
					AsyncPrinter::printer().enqueue(std::move(output_queue));
				}
			}

			template<typename T>
			dlog& operator<<(const T& _t)
			{
				if (out)
				{
					std::stringstream ss;
					ss << _t;
					auto task(std::make_shared<std::packaged_task<void()>>([buf = buffer, inf = infix, tp = std::make_shared<std::string>(ss.str())]{ *buf << *inf << *tp; }));
					output_queue.emplace([=]{ (*task)(); });
				}
				return *this;
			}

			dlog& operator<<(std::ostream& (*_fp)(std::ostream&))
			{
				if (out)
				{
					auto task(std::make_shared<std::packaged_task<void()>>([buf = buffer, inf = infix, fp = _fp]{ *buf << *inf << fp; }));
					output_queue.emplace([=]{ (*task)(); });
				}
				return *this;
			}

			template<typename T, typename std::enable_if<!std::is_void<T>::value, int>::type = 0>
			dlog& operator<<(std::future<T>& _future)
			{
				if (out)
				{
					sptr<std::future<T>> future(std::make_shared<std::future<T>>(std::move(_future)));
					auto task(std::make_shared<std::packaged_task<void()>>([buf = buffer, inf = infix, f = future]{ *buf << *inf << f->get(); }));
					output_queue.emplace([=]{ (*task)(); });
				}
				return *this;
			}

			template<typename T, typename std::enable_if<std::is_void<T>::value, int>::type = 0>
			dlog& operator<<(std::future<T>& _future)
			{
				return *this;
			}

			static inline void set_default_log_level(const uint _level)
			{
				default_log_level() = _level;
			}

			static inline const uint& get_default_log_level()
			{
				return default_log_level();
			}

			static inline void set_default_prefix(const std::string& _prefix)
			{
				default_prefix() = _prefix;
			}

			static inline const std::string& get_default_prefix()
			{
				return default_prefix();
			}

			static inline void set_default_infix(const std::string& _infix)
			{
				default_infix() = _infix;
			}

			static inline const std::string& get_default_infix()
			{
				return default_infix();
			}

			static inline void set_default_suffix(const std::string& _suffix)
			{
				default_suffix() = _suffix;
			}

			static inline const std::string& get_default_suffix()
			{
				return default_suffix();
			}
	};
}

#endif // DLOG_HPP
