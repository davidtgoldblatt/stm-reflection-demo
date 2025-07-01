#include "stm.hpp"
#include <iostream>
#include <thread>

static void doWork() {
  volatile int x = 0;
  for (int i = 0; i < 100; i++) {
    x = x;
  }
}

struct S {
  int x;
  int y;
};

int main() {
  const int niters = 10000000;
  stm::Ctx ctx;
  stm::Val<S> stmVal;
  {
    std::jthread writer([&]() {
      for (int i = 0; i < niters; i++) {
        ctx.writeTx([&]() {
          stmVal.x = i;
          doWork();
          stmVal.y = i;
        });
      }
    });
    std::jthread reader([&]() {
      for (int i = 0; i < niters; i++) {
        ctx.readTx([&]() {
          int x = stmVal.x;
          doWork();
          int y = stmVal.y;
          // This assert never fires!
          assert(x == y);
        });
      }
    });
  }

  std::cout << "Read retry fraction: "
            << (double)ctx.readRetries_.load() / niters << "\n";
  std::cout << "Write retry fraction: "
            << (double)ctx.writeRetries_.load() / niters << "\n";
}
