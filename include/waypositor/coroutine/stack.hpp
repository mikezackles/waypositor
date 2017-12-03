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

#ifndef UUID_FE1896DA_56C7_4EAC_820A_B28A397F0CB6
#define UUID_FE1896DA_56C7_4EAC_820A_B28A397F0CB6

#include <boost/align/aligned_allocator.hpp>

#include <vector>

namespace waypositor { namespace coroutine {
  // A type-safe coroutine stack! After creation, this shouldn't allocate
  // except if the stack needs to grow. Suspend/resume amounts to copying a
  // pointer and an offset through the thread pool's queues.
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

    // A type for representing a stack frame
    template <typename T, typename ParentPointer>
    class Frame final {
    private:
      // The parent frame's stack pointer, or equivalent for the root pointer
      ParentPointer mParentPointer;
      // The frame data
      T mData;
    public:
      template <typename ...Args>
      Frame(ParentPointer parent_pointer, Args&&... args)
        : mParentPointer{std::move(parent_pointer)}
        , mData(std::forward<Args>(args)...)
      {}

      // Dereferencing these on anything but the active frame is bad news!
      ParentPointer &parent_pointer() { return mParentPointer; }
      ParentPointer const &parent_pointer() const { return mParentPointer; }

      T &data() { return mData; }
      T const &data() const { return mData; }
    };

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
      std::size_t mOffset;

      auto &frame() {
        return mStack->frame<T, ParentPointer>(mOffset);
      }

      auto const &frame() const {
        return mStack->frame<T, ParentPointer>(mOffset);
      }
    public:
      Pointer(Pointer const &);
      Pointer &operator=(Pointer const &);
      Pointer(Pointer &&other)
        : mStack{other.mStack}, mOffset{other.mOffset}
      { other.mStack = nullptr; }
      Pointer &operator=(Pointer &&other) {
        if (this == &other) return *this;
        mStack = other.mStack;
        mOffset = other.mOffset;
        other.mStack = nullptr;
        return *this;
      }
      ~Pointer() {
        if (!*this) return;
        // The frame pointer has gone out of scope. Reactivate the parent
        // frame, or trigger the stack completion callback.
        mStack->pop<T, ParentLogic, ParentPointer>(mOffset);
      }

      Pointer(Stack &stack, std::size_t offset)
        : mStack{&stack}, mOffset{offset} {}

      explicit operator bool() const { return mStack != nullptr; }

      Context &context() { return mStack->context<Context>(); }
      Context const &context() const { return mStack->context<Context>(); }

      // Return information to the parent frame. This doesn't actually resume
      // the parent. That doesn't happen until the current stack pointer goes
      // out of scope. (This should guarantee that the parent resumes even if
      // coreturn is never called.) It also means the child frame is free to
      // call coreturn multiple times before it exits.
      template <typename ...Args>
      void coreturn(Args... args) {
        assert(*this);
        // Reach through the parent pointer to call coreturn. Usually this
        // means calling it on the frame data. The ReturnTag allows the parent
        // frame to invoke multiple coroutines with different coreturn
        // callbacks.
        this->frame().parent_pointer()->coreturn(
          ReturnTag{}, std::move(args)...
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
      , // Arguments for constructing the frame data. These are copied to
        // avoid inadvertently referencing into a stack that gets resized.
        // (Note that this doesn't prevent transferring ownership via
        // std::move.)
        typename ...Args
      >
      void coinvoke(Args... args) {
        assert(*this);
        mStack->call<Context, U, MyReturnTag, MyLogic>(
          // Transfer ownership of this stack pointer back to the stack
          std::move(*this)
        , std::move(args)...
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
    void pop(std::size_t offset) {
      using FrameT = Frame<T, ParentPointer>;
      // Get the current frame
      FrameT &doomed = this->frame<T, ParentPointer>(offset);
      // The new size of the stack after the pop
      std::size_t new_size = offset + sizeof(FrameT);
      // Reactivate the parent frame's logic, and pass it ownership of the
      // parent's frame pointer
      ParentLogic logic{std::move(doomed.parent_pointer())};
      // Destroy the current frame
      doomed.~FrameT();
      // Resize the stack. Shrinking a vector does not reallocate, so this
      // space remains free for reuse. (We could probably get away without
      // doing an explicit resize.)
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

      // Make space. This invalidates anything referencing into the existing
      // stack!!
      mStore.resize(aligned + sizeof(FrameT));

      // Construct a frame
      new (&mStore[aligned]) FrameT(
        std::move(parent_pointer), std::forward<Args>(args)...
      );

      // Create a frame pointer and pass ownership to T's Logic callback
      using PointerT = Pointer<
        Context, T, ReturnTag, ParentLogic, ParentPointer
      >;
      typename T::template Logic<PointerT>{PointerT{*this, aligned}}();
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
      // Contains the parent pointer supplied at stack creation
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
        std::unique_ptr<Stack> stack, ParentPointer parent
      )
        : mStack{std::move(stack)}, mParentPointer{std::move(parent)}
      {}

      // Dispatch the coreturn call to the parent pointer
      ParentPointer &operator->() { return mParentPointer; }
      ParentPointer const &operator->() const { return mParentPointer; }
    };

    // Helpers for constructing a one-off stack with no parent pointer
    struct NullTag {};
    struct NullPointer {
      NullPointer *operator->() { return this; }
      NullPointer const *operator->() const { return this; }
      void coreturn(NullTag) {}
    };
    template <typename Unused>
    struct NullLogic {
      NullLogic(Unused) {}
      void operator()() {}
    };

    struct Private {};

  public:
    Stack(
      Private // Make the constructor effectively private
    , void *context
    ) : mContext{context}
    { /*mStore.reserve(1 << 10);*/ } // Start with a 1KB stack for now

    // Kick off the stack! Note that a stack can be reused if it's empty
    template <
      // The coroutine to invoke first
      typename T
    , // coreturn will be invoked through the ParentPointer instance using
      // this tag
      typename ParentReturnTag
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
      , typename RootPointer<ParentPointer>::template Logic<
          NullLogic<ParentPointer>
        >
      >(
        std::move(root_pointer), std::forward<Args>(args)...
      );
    }

    // This is like std::thread::detach. It spins up a coroutine stack without
    // worrying about what happens when it finishes.
    template <
      // The coroutine to invoke first
      typename T
    , // The stack's special context instance (e.g., Connection)
      typename Context
    , // Arguments for constructing the T instance
      typename ...Args
    >
    static void spawn(
      Context &context, Args&&... args
    ) {
      spawn<T, NullTag>(
        context, NullPointer{}, std::forward<Args>(args)...
      );
    }
  };
}}

#endif

