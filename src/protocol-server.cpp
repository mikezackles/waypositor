#include <waypositor/logger.hpp>

#include <cstdlib>

#include <optional>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <experimental/filesystem>

#include <boost/align/aligned_allocator.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/signal_set.hpp>

namespace waypositor {
  namespace filesystem = std::experimental::filesystem;
  namespace asio = boost::asio;
  using Domain = asio::local::stream_protocol;

  class Connection final {
  private:
    class Sync final {
    private:
      std::atomic<uint32_t> mCallbackId{1};
      Connection &mConnection;
    public:
      Connection &connection() { return mConnection; }
      void set_callback(uint32_t callback_id) { mCallbackId = callback_id; }
      Sync(Connection &connection) : mConnection{connection} {}
      ~Sync() {
        // This is owned by a std::shared_ptr: there can no longer be concurrent
        // access
        uint32_t callback_id = mCallbackId;
        // 1 is the id of the display singleton, so we know it cannot be used
        // for a callback id
        if (callback_id == 1) return;
        mConnection.log_info("SYNC: ", callback_id);
        // TODO - Emit sync event
      }
    };
  public:
    class Dispatchable {
    public:
      virtual void dispatch(std::shared_ptr<Sync> sync, uint16_t opcode) = 0;
      virtual ~Dispatchable() = default;
    };

    Connection(
      std::size_t id, Logger &log, asio::io_service &asio
    , Domain::socket socket
    ) : mId{id}, mLog{log}, mAsio{asio}
      , mSocket{std::move(socket)}
    { this->log_info("Accepted"); }
    ~Connection() { this->log_info("Destroyed"); }

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
      mDispatchables.emplace(
        id, std::make_unique<T>(std::forward<Args>(args)...)
      );
    }

    void destroy(uint32_t id) {
      auto lock = std::lock_guard(mDispatchablesMutex);
      mDispatchables.erase(id);
    }

    void sync(uint32_t callback_id) {
      // Signal which callback id to use for emitting a sync event.
      mSync->set_callback(callback_id);
      // Destroy the previous Sync instance and replace it with a new one. Every
      // dispatch has shared ownership of a Sync instance via a shared_ptr.
      // Destroying the Connection's Sync pointer means that the moment all
      // outstanding dispatches complete, a sync event will be emitted.
      mSync = std::make_shared<Sync>(*this);
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
    std::shared_ptr<Sync> mSync{std::make_shared<Sync>(*this)};
  };

  namespace coroutine {
    // A type-safe coroutine stack! After creation, this shouldn't allocate
    // except if the stack needs to grow. Suspend/resume amounts to copying a
    // single pointer through the thread pool's queues.
    //
    // This stack aims to be provably correct because only one
    // (non-moved-from) stack pointer can exist at a time. Ownership must be
    // passed back to the stack to call a new coroutine, and if the active
    // pointer goes out of scope, the frame is popped, activating its parent.
    class Stack final {
    private:
      // Every coroutine stack has a context that provides general utilites
      // necessary across coroutines (e.g., logging, async operations). It
      // should be thread safe. It's type is safely stored inside the
      // stack/frame pointers, allowing this stack to be reused for any type of
      // context.
      void *mContext{nullptr};

      // This is where the stack frames are stored
      using Store = std::vector<
        unsigned char
      , // I'm not clear on the alignment guarantees (if any) for the buffers
        // created by std::vector. This is an attempt to guarantee that maximal
        // alignment is used. That way we know that using aligned offsets will
        // guarantee aligned addresses.
        boost::alignment::aligned_allocator<
          unsigned char, alignof(std::max_align_t)
        >
      >;
      Store mStore{};

      // The offset of the currently active stack frame
      std::size_t mOffset{0};
       
      // A type for representing a stack frame
      template <typename T, typename ParentPointer>
      class Frame final {
      private:
        // The offset of the parent stack frame
        std::size_t mParentOffset;
        // The parent frame's stack pointer, or equivalent for the root pointer
        ParentPointer mParentPointer;
        // The frame data
        T mData;
      public:
        template <typename ...Args>
        Frame(
          std::size_t parent_offset
        , ParentPointer parent_pointer, Args&&... args
        ) : mParentOffset{parent_offset}
          , mParentPointer{std::move(parent_pointer)}
          , mData{std::forward<Args>(args)...}
        {}

        // Dereferencing these on anything but the active frame is bad news!
        ParentPointer &parent_pointer() { return mParentPointer; }
        ParentPointer const &parent_pointer() const { return mParentPointer; }

        std::size_t parent_offset() const { return mParentOffset; }
        T &data() { return mData; }
        T const &data() const { return mData; }
      };

      // Get a reference to the current frame
      template <typename T, typename ParentPointer>
      auto &frame() {
        return reinterpret_cast<Frame<T, ParentPointer> &>(mStore[mOffset]);
      }

      // Get a const reference to the current frame
      template <typename T, typename ParentPointer>
      auto const &frame() const {
        return reinterpret_cast<Frame<T, ParentPointer> &>(mStore[mOffset]);
      }

      // Get a reference to a specific frame
      template <typename T, typename ParentPointer>
      auto &frame(std::size_t offset) {
        return reinterpret_cast<Frame<T, ParentPointer> &>(mStore[offset]);
      }

      // Get a const reference to a specific current frame
      template <typename T, typename ParentPointer>
      auto const &frame(std::size_t offset) const {
        return reinterpret_cast<Frame<T, ParentPointer> &>(mStore[offset]);
      }

      // Cast the context pointer to the correct type
      template <typename Context>
      Context &context() { return *static_cast<Context *>(mContext); }

      // Cast the context pointer to the correct const type
      template <typename Context>
      Context const &context() const {
        return *static_cast<Context *>(mContext);
      }

      // A stack pointer. We use a chain of ownership to avoid using/destroying
      // stack pointers out of order. (Each call passes ownership of the parent
      // frame.) Note that this ownership chain is encoded in the type, so a
      // deeply nested frame's type will include the type of all frames up to
      // that point.
      template <
        // The context type associated with this stack
        typename Context
      , // The type of this frame's data
        typename T
      , // Tag for invoking parent's coreturn callback
        typename ReturnTag
      , // Parent's logic type
        typename ParentLogic
      , // Parent's frame pointer type. This is a Pointer<...> for all frames
        // except the root frame, where it is Root::Pointer<...>.
        typename ParentPointer
      >
      class Pointer final {
      private:
        Stack *mStack;

        auto &frame() {
          return mStack->frame<T, ParentPointer>();
        }

        auto const &frame() const {
          return mStack->frame<T, ParentPointer>();
        }

        auto &parent_frame(std::size_t offset) {
          return mStack->frame<
            typename ParentPointer::Type, typename ParentPointer::Parent
          >(offset);
        }

        auto const &parent_frame(std::size_t offset) const {
          return mStack->frame<
            typename ParentPointer::Type, typename ParentPointer::Parent
          >(offset);
        }
      public:
        using Type = T;
        using Parent = ParentPointer;

        Pointer(Pointer const &);
        Pointer &operator=(Pointer const &);
        Pointer(Pointer &&other)
          : mStack{other.mStack}
        { other.mStack = nullptr; }
        Pointer &operator=(Pointer &&other) {
          if (this == &other) return *this;
          mStack = other.mStack;
          other.mStack = nullptr;
          return *this;
        }
        ~Pointer() {
          if (!*this) return;
          // The frame pointer has gone out of scope. Reactivate the parent
          // frame, or trigger the stack completion callback.
          mStack->pop<T, ParentLogic, ParentPointer>();
        }

        Pointer(Stack &stack) : mStack{&stack} {}

        explicit operator bool() const { return mStack != nullptr; }

        Context &context() { return mStack->context<Context>(); }
        Context const &context() const { return mStack->context<Context>(); }

        // Return information to the parent frame. This doesn't actually resume
        // the parent. That doesn't happen until the current stack pointer goes
        // out of scope. (This should guarantee that the parent resumes even if
        // coreturn is never called.) It also means the child frame is free to
        // call coreturn multiple times before it exits.
        template <typename ...Args>
        void coreturn(Args&&... args) {
          assert(*this);
          // Reach into the parent frame to call coreturn. This (the frame data)
          // is where coroutines should define their coreturn callbacks. The
          // ReturnTag allows the parent frame to invoke multiple coroutines
          // with different coreturn callbacks.
          this->parent_frame(this->frame().parent_offset()).data().coreturn(
            ReturnTag{}, std::forward<Args>(args)...
          );
        }

        // Invoke a new coroutine
        template <
          // The frame data for the coroutine. This type should define a type
          // U::Logic<FramePointer> that accepts ownership of the FramePointer
          // created by this function in its constructor. It should also define
          // a member function U::coreturn(MyReturnTag, ...)
          typename U
        , // The tag the child logic should use when invoking coreturn on the
          // frame data
          typename MyReturnTag
        , // A callable type that manipulates the frame data via the stack
          // pointer. It maintains ownership of the active frame pointer.
          typename MyLogic
        , // Arguments for constructing the frame data
          typename ...Args
        >
        void coinvoke(Args&&... args) {
          assert(*this);
          mStack->call<Context, U, MyReturnTag, MyLogic>(
            // Transfer ownership of this stack pointer back to the stack
            std::move(*this)
          , std::forward<Args>(args)...
          );
        }

        T *operator->() { assert(*this); return &this->frame().data(); }
        T const *operator->() const {
          assert(*this); return &this->frame().data();
        }
        T &operator*() { assert(*this); return this->frame().data(); }
        T const &operator*() const {
          assert(*this); return this->frame().data();
        }
      };

      // Destroy the current stack frame and reactivate the logic for its parent
      template <typename T, typename ParentLogic, typename ParentPointer>
      void pop() {
        using FrameT = Frame<T, ParentPointer>;
        // Get the current frame
        FrameT &doomed = this->frame<T, ParentPointer>();
        // The current offset will be the new size of the stack after the pop
        std::size_t new_size = mOffset;
        // Reset the offset to the offset of the parent frame
        mOffset = doomed.parent_offset();
        // Reactivate the parent frame's logic, and pass it ownership of the
        // parent's frame pointer
        ParentLogic logic{std::move(doomed.parent_pointer())};
        // Destroy the current frame
        doomed.~FrameT();
        // Resize the stack. Shrinking a vector does not reallocate, so this
        // space remains free for reuse. (We could probably get away without
        // doing an explicit resize.) Note that a push followed by a pop does
        // not guarantee that the store has the same size: It may be left with
        // some extra padding.
        mStore.resize(new_size);
        // Resume the parent frame. If this is the root frame it may destroy the
        // stack!
        logic();
      }

      // The logic for setting up a stack frame
      template <
        typename Context, typename T, typename ReturnTag
      , typename ParentLogic, typename ParentPointer, typename ...Args
      >
      void call(ParentPointer parent_pointer, Args&&... args) {
        using FrameT = Frame<T, ParentPointer>;

        // Get an aligned offset
        std::size_t mask = alignof(FrameT) - 1;
        std::size_t offset = mStore.size();
        std::size_t aligned = (offset + mask) & ~mask;

        // Make space
        mStore.resize(aligned + sizeof(FrameT));

        // Construct a frame
        new (&mStore[aligned]) FrameT(
          mOffset, std::move(parent_pointer), std::forward<Args>(args)...
        );

        // Update the offset
        mOffset = aligned;

        // Create a frame pointer and pass ownership to T's Logic callback
        using PointerT = Pointer<
          Context, T, ReturnTag, ParentLogic, ParentPointer
        >;
        typename T::template Logic<PointerT>{PointerT{*this}}();
      }

      // This object lives in the root stack frame. When the call stack
      // finishes, this goes out of scope
      template <typename ParentPointer>
      class RootPointer final {
      private:
        // (For now) this object owns the stack! The stack is destroyed when it
        // goes out of scope. In the future there may be an object pool that
        // allows for Stack reuse.
        std::unique_ptr<Stack> mStack;
        // Contains the parent pointer supplied at stack creation, or
        // std::nullopt if none was supplied
        ParentPointer mParentPointer;
      public:

        // Type for extracting the parent pointer and passing it along to a
        // ParentLogic instance.
        template <typename ParentLogic>
        class Logic final {
        private:
          RootPointer<ParentPointer> mRootPointer;
        public:
          Logic(RootPointer pointer) : mRootPointer{std::move(pointer)} {}

          void operator()() {
            ParentLogic{std::move(mRootPointer.mParentPointer)}();
          }
        };

        RootPointer(
          std::unique_ptr<Stack> stack, ParentPointer parent = std::nullopt
        )
          : mStack{std::move(stack)}, mParentPointer{std::move(parent)}
        {}

        // Dispatch the coreturn call to the parent pointer
        ParentPointer &operator->() { return mParentPointer; }
        ParentPointer const &operator->() const { return mParentPointer; }
      };

      struct Private {};

    public:
      Stack(
        Private // Make the constructor effectively private
      , void *context
      ) : mContext{context}
      { mStore.reserve(1 << 10); } // Start with a 1KB stack for now

      // Kick off the stack! Note that a stack can be reused if it's empty
      template <
        // The coroutine to invoke first
        typename T
      , // coreturn will be invoked through the ParentPointer instance using
        // this tag
        typename ParentReturnTag
      , // As with stack frames, this is the logic type used to take ownership
        // of the ParentPointer instance
        typename ParentLogic
      , // The stack's special context instance (e.g., Connection)
        typename Context
      , // This could be frame pointer for another stack, or it could be a
        // special handle (e.g., Forker<...>::KeepaliveHandle)
        typename ParentPointer
      , // Arguments for constructing the T instance
        typename ...Args
      >
      static void spawn(
        Context &context, ParentPointer parent_pointer, Args&&... args
      ) {
        auto stack_pointer = std::make_unique<Stack>(Private{}, &context);
        auto &stack = *stack_pointer;
        RootPointer root_pointer{
          std::move(stack_pointer), std::move(parent_pointer)
        };
        stack.template call<
          Context, T, ParentReturnTag
        , typename RootPointer<ParentPointer>::template Logic<ParentLogic>
        >(
          std::move(root_pointer), std::forward<Args>(args)...
        );
      }
    };

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

      // Accepts a KeepaliveHandle and drops it on the floor.
      class NullLogic final {
      public:
        NullLogic(KeepaliveHandle) {}
        void operator()() {}
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
        Stack::spawn<Coroutine, ReturnTag, NullLogic>(
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

      template <typename ...Args>
      void log_info(Args&&... args) {
        mSelf.context().log_info(std::forward<Args>(args)...);
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
  }

  //// These are intended to be simple enough to generate with a python script
  //namespace events {
  //  class Display final {
  //  private:
  //  public:
  //  };
  //}

  //namespace requests {
  //  // These are intended to be simple enough to generate with a python script.
  //  // Right now there's a full async pass through the stack for every field,
  //  // but it could be optimized to read all data in a single pass.
  //  template <typename Continuation, typename Implementation>
  //  class Display final {
  //  private:
  //    class GetRegistry final {
  //    private:
  //      struct Frame {
  //        Continuation continuation;
  //        Implementation &implementation;
  //      };
  //      std::optional<Frame> mFrame{std::nullopt};
  //      Parser<uint32_t> mRegistry;
  //      enum class State { PARSE_REGISTRY, DISPATCH };
  //      State mState{State::PARSE_REGISTRY};
  //    public:
  //      class Logic : public AsioOperator<Logic> {
  //      private:
  //        FramePointer<GetRegistry> self;
  //      public:
  //        Logic(GetRegistry &self_) : self{self_} {}
  //        auto &connection() { return self->mFrame.continuation.connection(); }

  //        void run() {
  //          switch (self->mState) {
  //          case State::PARSE_REGISTRY:
  //            self->mState = State::DISPATCHING;
  //            self->mRegistry.parse(std::move(*this));
  //            return;
  //          case State::DISPATCH:
  //            self->mState = State::DONE;
  //            // We're done parsing. Call the actual function
  //            self->mFrame.implementation.get_registry(
  //              self->mRegistry.get()
  //            , std::move(self->mFrame.continuation)
  //            );
  //            return;
  //          };
  //        }
  //      };

  //      void call(Continuation continuation, Implementation &implementation) {
  //        assert(!in_use()); // Otherwise someone else is calling this!
  //        mFrame = std::make_optional<Args>(
  //          std::move(continuation), implementation
  //        );
  //        Logic{*this}();
  //      }
  //      void destruct() { mFrame = std::nullopt; }
  //      bool in_use() { return static_cast<bool>(mFrame); }
  //    };
  //    GetRegistry mGetRegistry;

  //  public:
  //    bool dispatch(
  //      Continuation continuation, Implementation &implementation
  //    , uint16_t opcode
  //    ) {
  //      switch (opcode) {
  //      case 1:
  //        return mGetRegistry.call(std::move(continuation), implementation);
  //      default:
  //        return mContext.emit_error(
  //          std::move(continuation)
  //        , "(Display) unknown opcode: ", implementation.id(), opcode
  //        );
  //      }
  //    }
  //  };
  //}

  //template <typename Continuation>
  //class Display final : public Dispatchable<Continuation> {
  //private:
  //  std::unordered_map<
  //    uint32_t, std::unique_ptr<Dispatchable>
  //  > mTable{};
  //  events::Display mEvents{};
  //  requests::Display mRequests{};

  //public:
  //  constexpr uint32_t id() const { return 1; }

  //  Dispatchable<Continuation> *find(uint32_t object_id) {
  //    if (object_id == 1) {
  //      return this;
  //    } else if (auto it = mTable.find(object_id); it == mTable.end()) {
  //      return nullptr;
  //    } else {
  //      return it->second.get();
  //    }
  //  }

  //  bool dispatch(Continuation continuation, uint16_t opcode) override {
  //    return mRequests.dispatch(std::move(continuation), *this, opcode);
  //  }

  //  template <typename T, typename ...Args>
  //  bool create(uint32_t object_id, Args&&... args) {
  //    if (mTable.find(object_id) != mTable.end()) {
  //      return false;
  //    }
  //    mTable.emplace(
  //      object_id, std::make_unique<T>(std::forward<Args>(args)...)
  //    );
  //    return true;
  //  }

  //  void destroy(uint32_t object_id) {
  //    mTable.erase(object_id);
  //  }

  //  bool emit_error(Continuation continuation, std::string_view error) {
  //    mEvents.error().emit(std::move(continuation), error);
  //  }
  //};

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

  class Dispatcher final : private coroutine::FrameMixin<Dispatcher> {
  private:
    struct ParseResult {};

    enum class State { WORKING, GOT_DATA, ERROR };
    State mState{State::WORKING};
    uint32_t mObjectId;
    uint16_t mOpcode;
  public:

    template <typename StackPointer>
    class Logic : public LogicMixin<StackPointer> {
    public:
      Logic(StackPointer frame) : LogicMixin<StackPointer>(std::move(frame)) {}

      void resume() {
        switch (this->frame().mState) {
        case State::GOT_DATA:
          this->log_info("Finished parsing header");
          //this->log_info("Message Size: ", this->frame().mMessageSize);
          this->log_info("Object ID: ", this->frame().mObjectId);
          this->log_info("Opcode: ", this->frame().mOpcode);
          // Fall through
        case State::WORKING:
          // It's an error if we resume without getting a return value
          this->frame().mState = State::ERROR;
          this->template coinvoke<HeaderParser, ParseResult>();
          return;
        case State::ERROR:
          // Do nothing;
          return;
        }
      }
    };

    void coreturn(ParseResult, uint32_t object_id, uint16_t opcode) {
      mObjectId = object_id;
      mOpcode = opcode;
      mState = State::GOT_DATA;
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