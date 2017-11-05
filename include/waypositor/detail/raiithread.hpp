#ifndef UUID_26FEC1E9_7A51_45A7_AE29_9AC2E40D3CF9
#define UUID_26FEC1E9_7A51_45A7_AE29_9AC2E40D3CF9

#include <thread>

namespace waypositor { namespace detail {
  class RAIIThread {
  private:
    std::thread mThread;
  public:
    RAIIThread() = default;
    template <typename ...Args>
    RAIIThread(Args&&... args) : mThread{std::forward<Args>(args)...} {}
    RAIIThread(RAIIThread const &) = delete;
    RAIIThread &operator=(RAIIThread const &) = delete;
    RAIIThread(RAIIThread &&) = default;
    RAIIThread &operator=(RAIIThread &&) = default;
    ~RAIIThread() { if (*this) mThread.join(); }

    explicit operator bool() const { return mThread.joinable(); }
    std::thread::id get_id() { return mThread.get_id(); }
  };
}}

#endif