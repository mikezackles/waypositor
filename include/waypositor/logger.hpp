#ifndef UUID_41EEDDB2_BBF7_4710_92EB_432DE7EE73F0
#define UUID_41EEDDB2_BBF7_4710_92EB_432DE7EE73F0

#include <waypositor/detail/raiithread.hpp>

#include <boost/asio/io_service.hpp>

#include <mutex>
#include <iostream>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>

namespace waypositor {
  namespace asio = boost::asio;

  class Logger final {
  private:
    asio::io_service mASIO;
    std::optional<asio::io_service::work> mWork;
    std::mutex mMutex;
    std::unordered_map<std::thread::id, std::string> mNameLookup;
    detail::RAIIThread mThread;

    template <bool flush, typename ...Messages>
    static void print_helper(std::ostream &ostream, Messages&&... messages) {
      (ostream << ... << messages);
      if constexpr (flush) {
        ostream << std::endl;
      } else {
        ostream << "\n";
      }
    }

    // Should be synchronized by mMutex
    std::string_view thread_name(std::thread::id id) {
      using namespace std::string_view_literals;
      if (auto it = mNameLookup.find(id); it != mNameLookup.end()) {
        return {it->second};
      } else {
        return "???"sv;
      }
    }

    void stop() { mASIO.post([this] { mWork = std::nullopt; }); }

  public:
    Logger(std::string main_thread_name)
      : mASIO{}
      , mWork{mASIO}
      , mMutex{}
      , mNameLookup{}
      , mThread{[this] { mASIO.run(); }}
    {
      this->register_thread(
        std::this_thread::get_id(), std::move(main_thread_name)
      );
    }
    ~Logger() { this->stop(); }

    void register_thread(std::thread::id id, std::string name) {
      std::lock_guard lock{mMutex};
      mNameLookup.emplace(id, std::move(name));
    }

    void unregister_thread(std::thread::id id) {
      std::lock_guard lock{mMutex};
      mNameLookup.erase(id);
    }

    // This is immediate. It's slower, but it shouldn't be running under normal
    // operation. Error messages won't be lost in the event of a crash.
    template <typename ...Messages>
    void error(Messages&&... messages) {
      std::lock_guard lock{mMutex};
      print_helper<true>(
        std::cerr
      , "[", thread_name(std::this_thread::get_id()), "] ", messages...
      );
    }

    template <typename ...Messages>
    void perror(Messages&&... messages) {
      static constexpr std::size_t errno_buffer_size = 256;
      char buffer[errno_buffer_size]{};
      strerror_r(errno, buffer, errno_buffer_size);
      this->error(messages..., ": ", buffer);
    }

    // This queues messages to run on the log thread.
    template <typename ...Messages>
    void info(Messages... messages) {
      auto thread_id = std::this_thread::get_id();
      mASIO.post([
        this, thread_id = std::move(thread_id)
      , messages = std::make_tuple(std::move(messages)...)
      ]() {
        std::apply(
          [this, &thread_id](auto&&... messages_) {
            std::lock_guard lock{mMutex};
            print_helper<false>(
              std::cout, "[", thread_name(thread_id), "] ", messages_...
            );
          }
        , messages
        );
      });
    }
  };
}

#endif