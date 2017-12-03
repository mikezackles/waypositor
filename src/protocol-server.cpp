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

#include <waypositor/coroutine/stack.hpp>
#include <waypositor/logger.hpp>

#include <cstdlib>

#include <optional>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <experimental/filesystem>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/write.hpp>

namespace waypositor {
  namespace filesystem = std::experimental::filesystem;
  namespace asio = boost::asio;
  using Domain = asio::local::stream_protocol;
  using namespace std::literals;

  namespace coroutine {
    template <typename Context>
    class Forker final {
    private:
      struct ReturnTag {};

      class Lookup;
      // An entry in the lookup. This is owned by both the Lookup instance and
      // the associated Stack. It maintains a weak_ptr to the Lookup instance
      // that owns it so that destruction of the Stack's pointer can remove the
      // Entry from the Lookup.
      struct Entry final {
        std::size_t id;
        std::weak_ptr<Lookup> maybe_owner;
        Context context;
        template <typename ...Args>
        Entry(
          std::size_t id_, std::shared_ptr<Lookup> const &owner
        , Args&&... args
        ) : id{id_}, maybe_owner{owner}
          , context{id_, std::forward<Args>(args)...}
        {}

        void coreturn(ReturnTag) {
          // Forker does not return information to the calling coroutine, so we
          // don't do anything.
        }
      };

      // This represents the Stack's ownership of an Entry. If it goes out of
      // scope, it attempts to remove the associated Entry from its owning
      // Lookup instance, assuming the Lookup instance still exists.
      class KeepaliveHandle final {
      private:
        std::shared_ptr<Entry> mEntry;
      public:
        KeepaliveHandle(KeepaliveHandle &&) = default;
        KeepaliveHandle &operator=(KeepaliveHandle &&) = default;
        ~KeepaliveHandle() {
          if (!mEntry) return;
          auto owner = mEntry->maybe_owner.lock();
          if (!owner) return;
          owner->erase(mEntry->id);
        }

        explicit KeepaliveHandle(std::shared_ptr<Entry> entry)
          : mEntry{std::move(entry)}
        {}

        Entry *operator->() { return mEntry.get(); }
        Entry const *operator->() const { return mEntry.get(); }
        Entry &operator*() { return *mEntry; }
        Entry const &operator*() const { return *mEntry; }
      };

      // Lookups contain these. OwnerHandle maintains shared ownership over an
      // Entry with KeepaliveHandle. Destruction requests shutdown of the
      // Context.
      class OwnerHandle final {
      private:
        std::shared_ptr<Entry> mEntry;
      public:
        // These should be deleted, but there was a bug in gcc:
        // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80654
        OwnerHandle(OwnerHandle const &);
        OwnerHandle &operator=(OwnerHandle const &);
        OwnerHandle(OwnerHandle &&) = default;
        OwnerHandle &operator=(OwnerHandle &&) = default;
        ~OwnerHandle() {
          if (!mEntry) return;
          // This call should make async operations fail. (E.g., for an ASIO
          // connection it should destroy the socket.) That should tear down the
          // coroutine stack and eventually destroy the context.
          mEntry->context.shutdown();
        }

        explicit OwnerHandle(std::shared_ptr<Entry> entry)
          : mEntry{std::move(entry)}
        {}
      };

      // This stores all the Context instances and destroying it allows us to
      // cleanly shut everything down.
      class Lookup final {
      private:
        std::unordered_map<std::size_t, OwnerHandle> mLookup;
        std::mutex mLock;
      public:
        template <typename ...Args>
        void emplace(Args&&... args) {
          auto lock = std::lock_guard(mLock);
          mLookup.emplace(std::forward<Args>(args)...);
        }

        void erase(std::size_t id) {
          auto lock = std::lock_guard(mLock);
          mLookup.erase(id);
        }
      };

      // An internal version of fork to do tuple unpacking
      template <
        typename Coroutine
      , typename ...CoroArgs, std::size_t ...CoroIndices
      , typename ...ContextArgs, std::size_t ...ContextIndices
      >
      void fork_impl(
        std::tuple<ContextArgs...> context_args
      , std::index_sequence<ContextIndices...>
      , std::tuple<CoroArgs...> coro_args
      , std::index_sequence<CoroIndices...>
      ) {
        // gcc reports unused-but-set warnings when these tuples are empty.
        // Suppress them.
        (void)context_args;
        (void)coro_args;

        // Make a shared pointer to the context
        auto pointer = std::make_shared<Entry>(
          // Pass the id to the context constructor. (Useful for logging.)
          mCurrentId, mLookup
        , std::forward<ContextArgs>(std::get<ContextIndices>(context_args))...
        );

        // And a copy. Dual ownership is required to allow us to shut down
        // contexts unexpectedly.
        auto copy = pointer;

        // Put an ownership handle in the lookup. If it goes away, a context
        // shutdown will be requested.
        mLookup->emplace(mCurrentId, OwnerHandle{std::move(copy)});

        // Kick off the associated coroutine stack
        KeepaliveHandle keepalive{std::move(pointer)};
        Stack::spawn<Coroutine, ReturnTag>(
          keepalive->context, std::move(keepalive)
        , std::forward<CoroArgs>(std::get<CoroIndices>(coro_args))...
        );

        mCurrentId++;
      }

    public:
      // Fork a new coroutine stack and create a Context instance to go with it
      template <
        typename Coroutine, typename ...ContextArgs, typename ...CoroArgs
      >
      void fork(
        std::piecewise_construct_t
      , std::tuple<ContextArgs...> context_args
      , std::tuple<CoroArgs...> coro_args
      ) {
        fork_impl<Coroutine>(
          std::move(context_args)
        , std::make_index_sequence<sizeof...(ContextArgs)>{}
        , std::move(coro_args)
        , std::make_index_sequence<sizeof...(CoroArgs)>{}
        );
      }
    private:
      std::shared_ptr<Lookup> mLookup{std::make_shared<Lookup>()};
      std::size_t mCurrentId{0};
    };

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
      template <typename Destination>
      void async_read(Destination &destination) {
        mSelf.context().async_read(
          asio::buffer(&destination, sizeof(Destination))
        , std::move(static_cast<Logic &>(*this))
        );
      }

      template <typename Source>
      void async_write(Source &source) {
        static_assert(std::is_integral_v<Source>);
        mSelf.context().async_write(
          asio::buffer(&source, sizeof(Source))
        , std::move(static_cast<Logic &>(*this))
        );
      }

      void async_write(std::string_view source) {
        mSelf.context().async_write(
          asio::buffer(source.data(), source.size() + 1)
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

      void dispatch(uint32_t object_id, uint16_t opcode) {
        mSelf.context().dispatch(object_id, opcode);
      }

      template <typename T, typename ...Args>
      void create(Args&&... args) {
        mSelf.context().template create<T>(std::forward<Args>(args)...);
      }

      template <typename T, typename ...Args>
      void spawn(Args&&... args) {
        mSelf.context().template spawn<T>(std::forward<Args>(args)...);
      }

      uint32_t next_serial() { return mSelf.context().next_serial(); }

      void sync(uint32_t callback_id) { mSelf.context().sync(callback_id); }

      auto write_lock_guard() { return mSelf.context().write_lock_guard(); }

      State &frame() { return *mSelf; }
      State const &frame() const { return *mSelf; }
    };

    template <typename Derived>
    struct FrameMixin {
      template <typename StackPointer>
      using LogicMixin = LogicMixin<Derived, StackPointer>;
    };
  }

  class SendHeader final : public coroutine::FrameMixin<SendHeader> {
  private:
    enum class State { OBJECT_ID, OPCODE, MESSAGE_SIZE, FINISHED };
    State mState{State::OBJECT_ID};
    uint32_t mObjectId;
    uint16_t mOpcode;
    uint16_t mMessageSize;
    static constexpr uint16_t header_size =
      sizeof(mObjectId) + sizeof(mOpcode) + sizeof(mMessageSize)
    ;

    static constexpr uint16_t total_size(uint16_t message_size) {
      return header_size + message_size;
    }

  public:
    explicit SendHeader(
      uint32_t object_id, uint16_t opcode, uint16_t message_size
    ) : mObjectId{object_id}, mOpcode{opcode}
      , mMessageSize{total_size(message_size)}
    {}

    template <typename StackPointer>
    class Logic : public LogicMixin<StackPointer> {
    public:
      Logic(StackPointer frame) : LogicMixin<StackPointer>(std::move(frame)) {}

      void resume() {
        switch (this->frame().mState) {
        case State::OBJECT_ID:
          this->frame().mState = State::OPCODE;
          this->async_write(this->frame().mObjectId);
          return;
        case State::OPCODE:
          this->frame().mState = State::MESSAGE_SIZE;
          this->async_write(this->frame().mOpcode);
          return;
        case State::MESSAGE_SIZE:
          this->log_info("Message size: ", this->frame().mMessageSize);
          this->frame().mState = State::FINISHED;
          this->async_write(this->frame().mMessageSize);
          return;
        case State::FINISHED:
          this->coreturn();
          return;
        }
      }
    };
  };

  class SendSync final : public coroutine::FrameMixin<SendSync> {
  private:
    enum class State { HEADER, DATA, FINISHED, ERROR };
    State mState{State::HEADER};
    uint32_t mCallbackId;
    uint32_t mSerial;
  public:
    SendSync(uint32_t callback_id) : mCallbackId{callback_id} {}

    template <typename StackPointer>
    class Logic : public LogicMixin<StackPointer> {
    public:
      Logic(StackPointer frame) : LogicMixin<StackPointer>(std::move(frame)) {}

      void resume() {
        switch (this->frame().mState) {
        case State::HEADER:
          this->frame().mState = State::ERROR;
          this->template coinvoke<SendHeader, HeaderReturn>(
            this->frame().mCallbackId, uint16_t(0)
          , static_cast<uint16_t>(sizeof(uint32_t))
          );
          return;
        case State::ERROR:
          return;
        case State::DATA:
          this->frame().mSerial = this->next_serial();
          this->frame().mState = State::FINISHED;
          this->async_write(this->frame().mSerial);
          return;
        case State::FINISHED:
          this->coreturn();
          return;
        }
      }
    };

    struct HeaderReturn {};
    void coreturn(HeaderReturn) { mState = State::DATA; }
  };

  class Connection final {
  private:
    struct SyncReturnTag {};
  public:
    class Sync final {
    private:
      class Impl {
      private:
        std::atomic<uint32_t> mCallbackId{1};
        Connection &mConnection;

      public:
        Connection &get() { return mConnection; }
        Connection const &get() const { return mConnection; }
        void set_callback(uint32_t callback_id) { mCallbackId = callback_id; }

        Impl(Connection &connection) : mConnection{connection} {}
        ~Impl() {
          // This is owned by a std::shared_ptr: there can no longer be concurrent
          // access
          uint32_t callback_id = mCallbackId;
          // 1 is the id of the display singleton, so we know it cannot be used
          // for a callback id. This ensures that we don't attempt to emit an
          // event on shutdown if none was requested.
          if (callback_id == 1) return;
          mConnection.log_info("SYNC: ", callback_id);

          // Emit sync event
          coroutine::Stack::spawn<SendSync>(mConnection, callback_id);
        }
      };
      std::shared_ptr<Impl> mImpl;

      friend class Connection;
      void set_callback(uint32_t callback_id) {
        mImpl->set_callback(callback_id);
      }
    public:
      Sync(Connection &connection)
        : mImpl{std::make_shared<Impl>(connection)}
      {}
      Connection *operator->() { return &mImpl->get(); }
      Connection const *operator->() const { return &mImpl->get(); }
      Connection &operator*() { return mImpl->get(); }
      Connection const &operator*() const { return mImpl->get(); }
    };

    class Dispatchable {
    public:
      virtual void dispatch(Sync sync, uint16_t opcode) = 0;
      virtual ~Dispatchable() = default;
    };

    Connection(
      std::size_t id, Logger &log, asio::io_service &asio
    , Domain::socket socket
    ) : mId{id}, mLog{log}, mAsio{asio}
      , mSocket{std::move(socket)}
    { this->log_info("Accepted"); }
    ~Connection() {
      // Hack to avoid honoring an outstanding sync if the entire connection is
      // coming down
      mSync.set_callback(1);
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

    template <typename T, typename ...Args>
    void create(uint32_t id, Args&&... args) {
      auto lock = std::lock_guard(mDispatchablesMutex);
      mDispatchables.emplace(id, std::make_unique<T>(
        std::forward<Args>(args)...
      ));
    }

    void destroy(uint32_t id) {
      auto lock = std::lock_guard(mDispatchablesMutex);
      mDispatchables.erase(id);
    }

    template <typename T, typename ...Args>
    void spawn(Args&&... args) {
      auto copy = mSync;
      coroutine::Stack::spawn<T, SyncReturnTag>(
        *this, std::move(copy), std::forward<Args>(args)...
      );
    }

    void coreturn(SyncReturnTag) { /* Do nothing */ }

    void sync(uint32_t callback_id) {
      // Destroy the previous Sync instance and replace it with a new one. Every
      // dispatch has shared ownership of a Sync instance via a shared_ptr.
      // Destroying the Connection's Sync pointer means that the moment all
      // outstanding dispatches complete, a sync event will be emitted.
      mSync = Sync(*this);
      // Signal which callback id to use for emitting a sync event.
      mSync.set_callback(callback_id);
    }

    void dispatch(uint32_t object_id, uint16_t opcode) {
      auto lock = std::lock_guard(mDispatchablesMutex);
      if (auto it = mDispatchables.find(object_id);
          it != mDispatchables.end()) {
        it->second->dispatch(mSync, opcode);
      } else {
        // Error
      }
    }

    uint32_t next_serial() { return mEventSerial++; }

    class WriteLock {
    private:
      Connection *mConnection;
      struct Private {};
    public:
      WriteLock(Connection &connection)
        : mConnection{&connection} {}
      WriteLock(WriteLock &&other) : mConnection{other.mConnection}
      { other.mConnection = nullptr; }
      WriteLock &operator=(WriteLock &&other) {
        if (this == &other) return *this;
        mConnection = other.mConnection;
        other.mConnection = nullptr;
        return *this;
      }
      ~WriteLock() {
        if (mConnection != nullptr) mConnection->mWriteLock = false;
      }

      static std::optional<WriteLock> create(Connection &connection) {
        bool expected = false;
        auto lock = connection.mWriteLock.compare_exchange_weak(expected, true);
        if (lock) {
          return std::make_optional<WriteLock>(connection);
        } else {
          return std::nullopt;
        }
      }
    };

    auto write_lock_guard() { return WriteLock::create(*this); }

  private:
    std::size_t mId;
    Logger &mLog;
    asio::io_service &mAsio;
    std::optional<Domain::socket> mSocket;
    std::mutex mSocketMutex{};
    std::unordered_map<
      uint32_t, std::unique_ptr<Dispatchable>
    > mDispatchables{};
    std::mutex mDispatchablesMutex{};
    Sync mSync{*this};
    std::atomic<uint32_t> mEventSerial{0};
    std::atomic<bool> mWriteLock{false};
  };

  class SendDelete final : public coroutine::FrameMixin<SendDelete> {
  private:
    enum class State { WAITING, HEADER, SEND, DONE, ERROR };
    State mState{State::HEADER};
    uint32_t mDeletedId;
    std::optional<Connection::WriteLock> mWriteLock;

    static constexpr uint32_t object_id = 1; // singleton
    static constexpr uint16_t opcode = 1;
  public:
    SendDelete(uint32_t deleted_id) : mDeletedId{deleted_id} {}

    template <typename StackPointer>
    class Logic : public LogicMixin<StackPointer> {
    public:
      Logic(StackPointer frame) : LogicMixin<StackPointer>(std::move(frame)) {}

      void resume() {
        switch (this->frame().mState) {
        case State::WAITING:
          this->frame().mWriteLock = this->write_lock_guard();
          if (!this->frame().mWriteLock) {
            this->suspend();
            return;
          }
          // Fall through
        case State::HEADER:
          this->frame().mState = State::ERROR;
          this->template coinvoke<SendHeader, HeaderResult>(
            object_id, opcode, sizeof(this->frame().mDeletedId)
          );
          return;
        case State::SEND:
          this->frame().mState = State::DONE;
          this->async_write(this->frame().mDeletedId);
          return;
        case State::DONE:
          this->coreturn();
          return;
        case State::ERROR:
          return;
        }
      }
    };

    struct HeaderResult {};
    void coreturn(HeaderResult) { mState = State::SEND; }
  };

  class SendString final : public coroutine::FrameMixin<SendString> {
  //private:
  public:
    enum class State { LENGTH, MESSAGE, PADDING, FINISHED };
    State mState{State::LENGTH};
    uint32_t mLength;
    // This is intended to point at something constexpr
    std::string_view mMessage;
    static constexpr uint32_t null_terminator = 1;
    static constexpr uint32_t mask = 3;
    static constexpr auto raw_padding = "\0\0\0\0"sv;

    static constexpr uint32_t padded_length(std::string_view data) {
      uint32_t size = data.size();
      return (size + null_terminator + mask) & ~mask;
    }

    static constexpr uint32_t padding_length(std::string_view data) {
      uint32_t size = data.size();
      return ~(size + null_terminator + mask) & mask;
    }

    std::string_view padding() const {
      return raw_padding.substr(0, padding_length(mMessage));
    }

  public:
    static constexpr uint16_t total_length(std::string_view data) {
      return sizeof(mLength) + padded_length(data);
    }

    SendString(std::string_view data)
      : mLength{static_cast<uint32_t>(data.size()) + null_terminator}
      , mMessage{data}
    {}

    template <typename StackPointer>
    class Logic : public LogicMixin<StackPointer> {
    public:
      Logic(StackPointer frame) : LogicMixin<StackPointer>(std::move(frame)) {}

      void resume() {
        switch (this->frame().mState) {
        case State::LENGTH:
          this->log_info("LEN: ", this->frame().mLength);
          this->frame().mState = State::MESSAGE;
          this->async_write(this->frame().mLength);
          return;
        case State::MESSAGE:
          this->frame().mState = State::PADDING;
          this->async_write(this->frame().mMessage);
          return;
        case State::PADDING:
          this->frame().mState = State::FINISHED;
          this->async_write(this->frame().padding());
          return;
        case State::FINISHED:
          this->coreturn();
          return;
        }
      }
    };
  };

  class AnnounceInterface final
    : public coroutine::FrameMixin<AnnounceInterface> {
  private:
    enum class State { WAITING, HEADER, ID, NAME, VERSION, FINISHED, ERROR };
    State mState{State::HEADER};
    uint32_t mObjectId;
    uint32_t mInterfaceId;
    std::string_view mInterfaceName;
    uint32_t mVersion;
    std::optional<Connection::WriteLock> mWriteLock;

    static constexpr uint16_t opcode = 0;

  public:
    AnnounceInterface(
      uint32_t object_id, uint32_t interface_id
    , std::string_view interface_name, uint32_t version
    ) : mObjectId{object_id}, mInterfaceId{interface_id}
      , mInterfaceName{interface_name}, mVersion{version}
    {}

    template <typename StackPointer>
    class Logic : public LogicMixin<StackPointer> {
    public:
      Logic(StackPointer frame) : LogicMixin<StackPointer>(std::move(frame)) {}

      void resume() {
        switch (this->frame().mState) {
        case State::WAITING:
          this->frame().mWriteLock = this->write_lock_guard();
          if (!this->frame().mWriteLock) {
            this->suspend();
            return;
          }
          // Fall through
        case State::HEADER:
          this->log_info("Announcing interface ", this->frame().mInterfaceName);

          this->log_info(
            "String length: "
          , SendString::total_length(this->frame().mInterfaceName)
          );
          this->frame().mState = State::ERROR;
          this->template coinvoke<SendHeader, HeaderResult>(
            this->frame().mObjectId, opcode
          , sizeof(this->frame().mInterfaceId) +
            SendString::total_length(this->frame().mInterfaceName) +
            sizeof(this->frame().mVersion)
          );
          return;
        case State::ID:
          (void)this->frame().mState;
          (void)this->frame().mVersion;
          this->frame().mState = State::NAME;
          this->async_write(this->frame().mInterfaceId);
          return;
        case State::NAME:
          this->frame().mState = State::ERROR;
          this->template coinvoke<SendString, NameResult>(
            this->frame().mInterfaceName
          );
          return;
        case State::VERSION:
          this->frame().mState = State::FINISHED;
          this->async_write(this->frame().mVersion);
          return;
        case State::FINISHED:
          this->coreturn();
          return;
        case State::ERROR:
          return;
        }
      }
    };

    struct HeaderResult {};
    void coreturn(HeaderResult) { mState = State::ID; }

    struct NameResult {};
    void coreturn(NameResult) { mState = State::VERSION; }
  };

  // It might be more efficient to read this in one fell swoop instead of one
  // field at a time, but for now this both readable and portable.
  class HeaderParser final : private coroutine::FrameMixin<HeaderParser> {
  private:
    enum class State { OBJECT_ID, OPCODE, MESSAGE_SIZE, FINISHED };
    State mState{State::OBJECT_ID};
    uint32_t mObjectId;
    uint16_t mOpcode;
    uint16_t mMessageSize;

  public:
    template <typename StackPointer>
    class Logic : public LogicMixin<StackPointer> {
    public:
      Logic(StackPointer frame) : LogicMixin<StackPointer>(std::move(frame)) {}

      void resume() {
        switch (this->frame().mState) {
        case State::OBJECT_ID:
          this->frame().mState = State::OPCODE;
          this->async_read(this->frame().mObjectId);
          return;
        case State::OPCODE:
          this->frame().mState = State::MESSAGE_SIZE;
          this->async_read(this->frame().mOpcode);
          return;
        case State::MESSAGE_SIZE:
          this->frame().mState = State::FINISHED;
          this->async_read(this->frame().mMessageSize);
          return;
        case State::FINISHED:
          this->frame().mState = State::OBJECT_ID;
          this->coreturn(this->frame().mObjectId, this->frame().mOpcode);
          return;
        }
      }
    };
  };

  class Registry final : public Connection::Dispatchable {
  public:
    void dispatch(
      Connection::Sync sync, uint16_t opcode
    ) override {
      sync->log_info("Registry request: ", opcode);
    }
  };

  class GetRegistryRequest final
    : private coroutine::FrameMixin<GetRegistryRequest> {
  private:
    enum class State { ID, CREATE, COMPOSITOR, FINISHED, ERROR };
    State mState{State::ID};
    uint32_t mRegistryId;

  public:
    template <typename StackPointer>
    class Logic : public LogicMixin<StackPointer> {
    public:
      Logic(StackPointer frame) : LogicMixin<StackPointer>(std::move(frame)) {}

      void resume() {
        switch (this->frame().mState) {
        case State::ID:
          this->frame().mState = State::CREATE;
          this->async_read(this->frame().mRegistryId);
          return;
        case State::CREATE:
          this->template create<Registry>(this->frame().mRegistryId);
          // fall through
        case State::COMPOSITOR:
          this->frame().mState = State::ERROR;
          this->template coinvoke<AnnounceInterface, AnnounceResult>(
            this->frame().mRegistryId, 1, "wl_compositor"sv, 4
          );
          return;
        case State::FINISHED:
          this->coreturn();
          return;
        case State::ERROR:
          return;
        }
      }
    };

    struct AnnounceResult {};
    void coreturn(AnnounceResult) { mState = State::FINISHED; }
  };

  class DisplayDispatch final : private coroutine::FrameMixin<DisplayDispatch> {
  private:
    enum class State {
      DISPATCH, PARSE_SYNC, DELETE_CALLBACK, SYNC_DONE, ERROR
    };
    State mState{State::DISPATCH};
    uint16_t mOpcode;
    uint32_t mSyncCallbackId;

  public:
    DisplayDispatch(uint16_t opcode) : mOpcode{opcode} {}

    template <typename StackPointer>
    class Logic : public LogicMixin<StackPointer> {
    public:
      Logic(StackPointer frame) : LogicMixin<StackPointer>(std::move(frame)) {}

      // Sync is a special case. We do it here out of laziness :)
      void resume() {
        switch (this->frame().mState) {
        case State::DISPATCH:
          switch (this->frame().mOpcode) {
          case 0: // sync
            this->log_info("display::sync");
            this->frame().mState = State::PARSE_SYNC;
            break;
          case 1: // get registry
            this->log_info("display::get_registry");
            this->template spawn<GetRegistryRequest>();
            this->coreturn();
            return;
          default:
            this->log_error("Invalid opcode for Display");
            this->log_error("TODO - report this to client");
            this->coreturn();
            return;
          }
        case State::PARSE_SYNC:
          this->frame().mState = State::DELETE_CALLBACK;
          this->async_read(this->frame().mSyncCallbackId);
          return;
        case State::DELETE_CALLBACK:
          this->frame().mState = State::ERROR;
          this->template coinvoke<SendDelete, DeleteComplete>(
            this->frame().mSyncCallbackId
          );
          return;
        case State::SYNC_DONE:
          this->log_info("Sync requested: ", this->frame().mSyncCallbackId);
          this->sync(this->frame().mSyncCallbackId);
          this->coreturn();
          return;
        case State::ERROR:
          return;
        }
      }
    };

    struct DeleteComplete {};
    void coreturn(DeleteComplete) { mState = State::SYNC_DONE; }
  };

  class Dispatcher final : private coroutine::FrameMixin<Dispatcher> {
  private:
    struct DispatchResult {};
    struct HeaderResult {};

    enum class State { PARSE, GOT_HEADER, ERROR };
    State mState{State::PARSE};
    uint32_t mObjectId;
    uint16_t mOpcode;
  public:
    template <typename StackPointer>
    class Logic : public LogicMixin<StackPointer> {
    public:
      Logic(StackPointer frame) : LogicMixin<StackPointer>(std::move(frame)) {}

      void resume() {
        switch (this->frame().mState) {
        case State::GOT_HEADER:
          this->log_info(
            "Request ["
          , "object: ", this->frame().mObjectId, ", "
          , "opcode: ", this->frame().mOpcode
          , "]"
          );
          if (this->frame().mObjectId == 1) {
            this->frame().mState = State::ERROR;
            this->template coinvoke<DisplayDispatch, DispatchResult>(
              this->frame().mOpcode
            );
            return;
          } else {
            this->dispatch(this->frame().mObjectId, this->frame().mOpcode);
          }
          // Fall through
        case State::PARSE:
          this->frame().mState = State::ERROR;
          this->template coinvoke<HeaderParser, HeaderResult>();
          return;
        case State::ERROR:
          // Do nothing;
          return;
        }
      }
    };

    void coreturn(DispatchResult) { mState = State::PARSE; }

    void coreturn(HeaderResult, uint32_t object_id, uint16_t opcode) {
      mObjectId = object_id;
      mOpcode = opcode;
      mState = State::GOT_HEADER;
    }
  };

  class Listener final {
  private:
    enum class State { STOPPED, LISTENING, ACCEPTED };
    Logger &mLog;
    asio::io_service &mAsio;
    Domain::acceptor mAcceptor;
    Domain::socket mSocket;
    std::optional<coroutine::Forker<Connection>> mConnections;
    State mState;

    class Worker final {
    private:
      Listener *self;
    public:
      Worker(Listener &self_) : self{&self_} {}
      Worker(Worker const &);
      Worker &operator=(Worker const &);
      Worker(Worker &&other) noexcept : self{other.self} {
        other.self = nullptr;
      }
      Worker &operator=(Worker &&other) {
        if (this == &other) return *this;
        self = other.self;
        other.self = nullptr;
        return *this;
      }
      ~Worker() = default;

      void operator()(boost::system::error_code const &error = {}) {
        assert(*this);
        if (error) {
          self->mLog.error("(Listener) ASIO error: ", error.message());
          return;
        }

        switch (self->mState) {
        case State::STOPPED:
          self->mLog.info("(Listener) stopped by request");
          return;
        case State::LISTENING:
          self->mState = State::ACCEPTED;
          self->mAcceptor.async_accept(self->mSocket, std::move(*this));
          return;
        case State::ACCEPTED:
          self->mConnections->fork<Dispatcher>(
            std::piecewise_construct
          , std::forward_as_tuple(
              self->mLog, self->mAsio, std::move(self->mSocket)
            )
          , std::forward_as_tuple()
          );
          self->mState = State::LISTENING;
          self->mAsio.post(std::move(*this));
          return;
        }
      }

      explicit operator bool() const { return self != nullptr && *self; }
    };

    struct Private {};
  public:
    void launch() { Worker{*this}(); }

    void stop() {
      mState = State::STOPPED;
      mConnections = std::nullopt;

      boost::system::error_code error;
      mAcceptor.cancel(error);
      if (error) {
        mLog.error("ASIO: ", error.message());
      }
    }

    explicit operator bool() const {
      return mState == State::STOPPED || static_cast<bool>(mConnections);
    }

    Listener(
      Private // effectively make this constructor private
    , Logger &log, asio::io_service &asio, filesystem::path const &path
    ) : mLog{log}, mAsio{asio}
      , mAcceptor{asio, path.native()}, mSocket{asio}
      , mConnections{std::make_optional<coroutine::Forker<Connection>>()}
      , mState{State::LISTENING}
    {}

    template <typename Name>
    static std::optional<Listener> create(
      Logger &log, asio::io_service &asio, Name &&socket_name
    ) {
      char const *xdg_runtime = std::getenv("XDG_RUNTIME_DIR");
      if (xdg_runtime == nullptr) {
        log.error("XDG_RUNTIME_DIR must be set");
        return std::nullopt;
      }
      filesystem::path socket{xdg_runtime};
      socket /= socket_name;

      if (filesystem::exists(socket)) {
        std::error_code error;
        filesystem::remove(socket, error);
        if (error) {
          log.error("Couldn't remove existing socket");
          return std::nullopt;
        }
      }

      log.info("Listening on ", socket);

      return std::make_optional<Listener>(Private{}, log, asio, socket);
    }
  };
}

int main() {
  using namespace waypositor;
  Logger log{"Main"};

  asio::io_service asio{};
  auto listener = Listener::create(log, asio, "wayland-0");
  if (!listener) return EXIT_FAILURE;
  listener->launch();

  asio::signal_set signals{asio, SIGINT, SIGTERM};
  signals.async_wait([&](
    boost::system::error_code const &error, int /*signal*/
  ) {
    if (error) {
      log.error("ASIO: ", error.message());
      return;
    }
    listener->stop();
  });

  asio.run();
  return EXIT_SUCCESS;
}