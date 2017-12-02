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