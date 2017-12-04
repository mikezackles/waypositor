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

#ifndef UUID_F5BC4CB9_A4F7_400C_B278_82468A78326B
#define UUID_F5BC4CB9_A4F7_400C_B278_82468A78326B

#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>

namespace waypositor { namespace coroutine {
  // Factor out some boilerplate
  template <typename State, typename StackPointer>
  class LogicMixin {
  private:
    using Logic = typename State::template Logic<StackPointer>;
    StackPointer mSelf;
  public:
    LogicMixin(StackPointer self) : mSelf{std::move(self)} {}

    // ASIO error-handling boilerplate
    void operator()(
      boost::system::error_code const &error = {}, std::size_t = 0
    ) {
      if (error) {
        mSelf.context().log_error("ASIO error: ", error.message());
        return;
      }

      // Resume the actual logic
      static_cast<Logic *>(this)->resume();
    }

  protected:
    auto &context() { return mSelf.context(); }

    template <typename Destination>
    void async_read(Destination &destination) {
      mSelf.context().async_read(
        boost::asio::buffer(&destination, sizeof(Destination))
      , std::move(static_cast<Logic &>(*this))
      );
    }

    template <typename Source>
    void async_write(Source &source) {
      static_assert(std::is_integral_v<Source>);
      mSelf.context().async_write(
        boost::asio::buffer(&source, sizeof(Source))
      , std::move(static_cast<Logic &>(*this))
      );
    }

    void async_write(std::string_view source) {
      mSelf.context().async_write(
        boost::asio::buffer(source.data(), source.size() + 1)
      , std::move(static_cast<Logic &>(*this))
      );
    }

    template <typename ...Args>
    void log_info(Args&&... args) {
      mSelf.context().log_info(std::forward<Args>(args)...);
    }

    template <typename ...Args>
    void log_error(Args&&... args) {
      mSelf.context().log_error(std::forward<Args>(args)...);
    }

    void suspend() {
      mSelf.context().post(std::move(static_cast<Logic &>(*this)));
    }

    // Invoke a new coroutine
    template <
      // The frame data for the coroutine. This type should define a type
      // U::Logic<FramePointer> that accepts ownership of the FramePointer
      // created by this function in its constructor. It should also define
      // a member function U::coreturn(ReturnTag, ...)
      typename U
    , // The tag the child logic should use when invoking coreturn on the
      // frame data
      typename ReturnTag
    , // Arguments for constructing the frame data
      typename ...Args
    >
    void coinvoke(Args&&... args) {
      mSelf.template coinvoke<U, ReturnTag, Logic>(
        std::forward<Args>(args)...
      );
    }

    template <typename ...Args>
    void coreturn(Args&&... args) {
      mSelf.coreturn(std::forward<Args>(args)...);
    }

    State &frame() { return *mSelf; }
    State const &frame() const { return *mSelf; }
  };

  template <typename Derived>
  struct FrameMixin {
    template <typename StackPointer>
    using LogicMixin = LogicMixin<Derived, StackPointer>;
  };
}}

#endif
