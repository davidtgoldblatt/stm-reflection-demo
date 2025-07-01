#pragma once

// A reflection-based software-transactional memory implementation. Allows
// arbitrary reader/reader and reader/writer concurrency for non-conflicting
// transactions. Writer/writer contention is mediated through a lock held just
// during commit for non-conflicting transactions. When conflicting transactions
// are detected, we just fall back to a shared_mutex.

#include <atomic>
#include <cassert>
#include <map>
#include <memory>
#include <meta>
#include <set>
#include <shared_mutex>

namespace stm {

struct Ctx;

namespace impl {

template <typename T> struct ValHelper;
struct PendingWriteBase;

struct ThreadCtx {
  Ctx *ctx;
  bool isWrite;
  // Epoch at which the transaction *started*.
  uint64_t epoch;

  // Really, these could be hash map / set (or some smarter data structure that
  // fast-paths common cases). But there's something broken about my libc++
  // build at the reflection implementation commit that hits some sort of
  // missing symbol in the hashers.

  // At commit time, write transaction need to validate that all the values they
  // read are still up to date, and to actually perform the writes that were
  // thread-private until now. These are pointers to those epoch values.
  std::set<std::atomic<uint64_t> *> pendingReads;
  // Key is the address of the ValHelper<T> written to.
  std::map<void *, std::unique_ptr<PendingWriteBase>> pendingWrites;
};

struct PendingWriteBase {
  virtual ~PendingWriteBase() = default;
  // *dst is a ValHelper<T>
  virtual bool canCommit(void *dst) = 0;
  // *dst is a ValHelper<T>
  virtual void commit(void *dst) = 0;
};

template <typename T> struct PendingWrite : PendingWriteBase {
  PendingWrite(T value) : value_(value) {}
  virtual ~PendingWrite() = default;

  virtual bool canCommit(void *dst) override final {
    ValHelper<T> *typedDst = (ValHelper<T> *)dst;
    return typedDst->canCommit();
  }

  virtual void commit(void *dst) override final {
    ValHelper<T> *typedDst = (ValHelper<T> *)dst;
    typedDst->commit(value_);
  }

  T value_;
};

inline thread_local ThreadCtx tctx;

struct TxFailed {};

consteval std::vector<std::meta::info> translateMembers(std::meta::info orig) {
  std::vector<std::meta::info> transformedMembers;
  auto ctx = std::meta::access_context::current();
  for (std::meta::info m : nonstatic_data_members_of(orig, ctx)) {
    std::meta::info new_type = substitute(^^ValHelper, {
                                                           type_of(m)});
    std::meta::data_member_options options = {.name = identifier_of(m)};
    std::meta::info new_member = std::meta::data_member_spec(new_type, options);
    transformedMembers.push_back(new_member);
  }
  return transformedMembers;
}

template <typename T> struct AggregateHelper {
  struct type;
  consteval { define_aggregate(^^type, translateMembers(^^T)); }
};

template <typename T>
  requires(std::is_scalar_v<T>)
struct ValHelper<T> {
  std::atomic<T> val_{};
  std::atomic<uint64_t> valEpoch_{0};

  T getFromRead() {
    T val = val_.load(std::memory_order_acquire);
    uint64_t epoch = valEpoch_.load(std::memory_order_relaxed);
    if (epoch > tctx.epoch) {
      throw TxFailed();
    }
    return val;
  }

  T getFromWrite() {
    auto it = tctx.pendingWrites.find(this);
    if (it != tctx.pendingWrites.end()) {
      return static_cast<PendingWrite<T> *>(it->second.get())->value_;
    }
    tctx.pendingReads.insert(&valEpoch_);
    return getFromRead();
  }

  T get() {
    assert(tctx.ctx != nullptr);
    if (tctx.isWrite) {
      return getFromWrite();
    } else {
      return getFromRead();
    }
  }

  void set(T newVal) {
    assert(tctx.ctx != nullptr);
    assert(tctx.isWrite);

    auto pendingWrite = std::make_unique<PendingWrite<T>>(newVal);
    tctx.pendingWrites.emplace(this, std::move(pendingWrite));
  }

  T operator=(T val) {
    set(val);
    return val;
  }

  operator T() { return get(); }

  bool canCommit() {
    return tctx.epoch >= valEpoch_.load(std::memory_order_relaxed);
  }

  void commit(T val) {
    assert(tctx.ctx != nullptr);
    assert(tctx.isWrite);

    valEpoch_.store(tctx.epoch, std::memory_order_relaxed);
    val_.store(val, std::memory_order_release);
  }
};

template <typename T>
  requires(std::is_aggregate_v<T> && std::is_trivially_copyable_v<T>)
struct ValHelper<T> : AggregateHelper<T>::type {};

inline bool canCommit() {
  for (auto &elem : tctx.pendingReads) {
    // elem here is a pointer to a value's epoch counter.
    if (elem->load(std::memory_order_relaxed) > tctx.epoch) {
      return false;
    }
  }
  for (auto &elem : tctx.pendingWrites) {
    // elem.first here is the address of the ValHelper<T>, elem.second is the
    // PendingWrite.
    if (!elem.second->canCommit(elem.first)) {
      return false;
    }
  }
  return true;
}

inline void doCommit() {
  for (auto &elem : tctx.pendingWrites) {
    elem.second->commit(elem.first);
  }
}

} // namespace impl

template <typename T> using Val = impl::ValHelper<T>;

struct Ctx {
  template <typename Func> void readTx(Func &&f) {
    // no recursive transactions
    assert(impl::tctx.ctx == nullptr);
    impl::tctx.ctx = this;
    impl::tctx.isWrite = false;

    impl::tctx.epoch = epoch_.load(std::memory_order_acquire);

    try {
      f();
    } catch (const impl::TxFailed &e) {
      readRetries_.fetch_add(1, std::memory_order_relaxed);
      mu_.lock_shared();
      impl::tctx.epoch = epoch_.load(std::memory_order_acquire);
      f();
      mu_.unlock_shared();
    }

    impl::tctx.ctx = nullptr;
  }

  template <typename Func> void writeTx(Func &&f) {
    // no recursive transactions
    assert(impl::tctx.ctx == nullptr);
    impl::tctx.ctx = this;
    impl::tctx.isWrite = true;

    bool txSuccess = true;
    try {
      impl::tctx.epoch = epoch_.load(std::memory_order_relaxed);
      // Execute the function without the lock.
      f();
      // Grab it when it's time to commit.
      mu_.lock();
      txSuccess = impl::canCommit();
      if (txSuccess) {
        impl::tctx.epoch = epoch_.load(std::memory_order_relaxed) + 1;
        impl::doCommit();
        epoch_.store(impl::tctx.epoch, std::memory_order_release);
      }
      mu_.unlock();

    } catch (const impl::TxFailed &e) {
      txSuccess = false;
    }

    if (!txSuccess) {
      writeRetries_.fetch_add(1, std::memory_order_relaxed);
      mu_.lock();
      impl::tctx.epoch = epoch_.load(std::memory_order_relaxed);
      f();
      impl::tctx.epoch++;
      impl::doCommit();
      epoch_.store(impl::tctx.epoch, std::memory_order_release);
      mu_.unlock();
    }

    impl::tctx.pendingReads.clear();
    impl::tctx.pendingWrites.clear();

    impl::tctx.ctx = nullptr;
  }

  std::atomic<uint64_t> readRetries_{0};
  std::atomic<uint64_t> writeRetries_{0};
  std::shared_mutex mu_;
  std::atomic<uint64_t> epoch_{0};
};

} // namespace stm
