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

#ifndef UUID_45D924CC_4AF9_4213_82F6_FD2167EB22CA
#define UUID_45D924CC_4AF9_4213_82F6_FD2167EB22CA

#include <waypositor/coroutine/stack.hpp>

#include <cstddef>
#include <memory>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace waypositor { namespace coroutine {
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
}}

#endif