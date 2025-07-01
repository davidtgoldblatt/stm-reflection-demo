This is a simple software-transactional memory implementation I wrote to try out
the C++ reflection proposal. All the interesting reflection stuff is in stm.hpp,
starting at around the translateMembers function.

Given some types like this:

struct Inner {
  int a;
  int b;
};

struct Outer {
  Inner c;
  int d;
};

We reflectively generate (this is pseudocode):

struct stm::Val<int> {
  std::atomic<int> v_;
  std::atomic<uint64_t> valEpoch_;

  // Various helper functions.
};

struct stm::Val<Inner> {
  stm::Val<int> a;
  stm::Val<int> b;
};

struct stm::Val<Outer> {
  stm::Val<Inner> c;
  stm::Val<int> d;
};

Using the same general approach you could also write a precise garbage
collector, a serialization library, etc.

The STM implementation itself has some problems:
- No allocation interface (it would be fairly straightforward to bolt on RCU for
  the read-side + rollback for the write side; but this hasn't actually been
  done).
- Use of slow data structures for tracking the read/write sets for write
  transactions.
- Completely untested and unoptimized.

But overall, the implementation approach seems reasonable: we keep a global
epoch counter, and track the epoch in which each guarded scalar was last stored.
Read transactions validate all the values they obtain against their start epoch,
and abort if they read a newer value. Write transactions track read/write sets
thread-locally, and acquire a global lock only at commit time. After a failed
transaction attempt, we fall back to a shared mutex.

This gets us the following properties:
- Read transactions proceed in parallel with non-conflicting write transactions
  (or other read transactions) in a completely non-blocking manner.
- Non-conflicting write transactions serialize on a mutex while committing, but
  don't otherwise interfere during their execution.
- Genuinely conflicting transactions serialize on a mutex.

I think it could be reasonably efficient given some straightforward
improvements.


To build:

- Get the C++ reflection implementation from https://github.com/bloomberg/clang-p2996.
- Configure as follows:

cmake -G Ninja ../llvm \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;libunwind;compiler-rt" \
  -DCLANG_DEFAULT_CXX_STDLIB=libc++       \
  -DCLANG_DEFAULT_UNWINDLIB=libunwind     \
  -DCLANG_DEFAULT_RTLIB=compiler-rt       \
  -DLIBCXX_ENABLE_SHARED=ON               \
  -DLIBCXX_ENABLE_STATIC=ON               \
  -DLIBCXX_ABI_UNSTABLE=ON                \
  -DCMAKE_INSTALL_PREFIX=$HOME/clang-p2996/install

ninja make
ninja install

- You can then compile main.cpp via:

~/clang-p2996/install/bin/clang++ -O1 -freflection-latest -std=c++26  main.cpp
