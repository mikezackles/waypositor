// Copyright (c) 2017, Oblong Industries, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors
//    may be used to endorse or promote products derived from this software
//    without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
// OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
// OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
// EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef UUID_5955DBC6_9ED2_4C07_B898_45B5F57210F0
#define UUID_5955DBC6_9ED2_4C07_B898_45B5F57210F0

#include <waypositor/logger.hpp>

#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

namespace waypositor { namespace coroutine {
  namespace asio = boost::asio;
  template <typename Socket>
  class Connection {
  public:
    Connection(
      std::size_t id, Logger &log, asio::io_service &asio, Socket socket
    ) : mId{id}, mLog{log}, mAsio{asio}
      , mSocket{std::move(socket)}
    { this->log_info("Accepted"); }
    ~Connection() {
      this->log_info("Destroyed");
    }

    template <typename Callback>
    void post(Callback &&callback) {
      mAsio.post(std::move(callback));
    }

    template <typename Buffers, typename Continuation>
    void async_read(Buffers &&buffers, Continuation continuation) {
      auto lock = std::lock_guard(mSocketMutex);
      asio::async_read(
        *mSocket, buffers, std::move(continuation)
      );
    }

    template <typename Buffers, typename Continuation>
    void async_write(Buffers &&buffers, Continuation continuation) {
      auto lock = std::lock_guard(mSocketMutex);
      asio::async_write(
        *mSocket, buffers, std::move(continuation)
      );
    }

    template <typename ...Args>
    void log_info(Args&&... args) {
      mLog.info("(Connection ", mId, ") ", std::forward<Args>(args)...);
    }

    template <typename ...Args>
    void log_error(Args&&... args) {
      mLog.error("(Connection ", mId, ") ", std::forward<Args>(args)...);
    }

    void shutdown() {
      auto lock = std::lock_guard(mSocketMutex);
      mSocket = std::nullopt;
    }
  private:
    std::size_t mId;
    Logger &mLog;
    asio::io_service &mAsio;
    std::optional<Socket> mSocket;
    std::mutex mSocketMutex{};
  };
}}

#endif

