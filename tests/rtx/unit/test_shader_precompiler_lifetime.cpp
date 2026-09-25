#include "../../../src/dxvk/rtx_render/rtx_shader_precompiler.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>

dxvk::Logger dxvk::Logger::s_instance("shader-precompiler-test");

using Precompiler = dxvk::RtxShaderPrecompiler;
using namespace std::chrono_literals;

static void require(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message);
    std::_Exit(1);
  }
}

struct Event {
  std::mutex mutex;
  std::condition_variable condition;
  bool signaled = false;
  void signal() {
    std::lock_guard<std::mutex> lock(mutex);
    signaled = true;
    condition.notify_all();
  }
  void wait() {
    std::unique_lock<std::mutex> lock(mutex);
    require(condition.wait_for(lock, 5s, [&] { return signaled; }), "event wait timed out");
  }
};

template<typename Predicate>
static void eventually(Predicate predicate, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!predicate()) {
    require(std::chrono::steady_clock::now() < deadline, message);
    std::this_thread::yield();
  }
}

struct ThrowingCopy {
  bool* fail;
  explicit ThrowingCopy(bool* value) : fail(value) { }
  ThrowingCopy(const ThrowingCopy& other) : fail(other.fail) {
    if (*fail) throw std::runtime_error("intentional callable copy failure");
  }
  void operator()(bool) const { }
};

int main() {
  Event testDone;
  std::thread watchdog([&] {
    std::unique_lock<std::mutex> lock(testDone.mutex);
    require(testDone.condition.wait_for(lock, 30s, [&] { return testDone.signaled; }),
      "precompiler lifetime test exceeded 30 seconds");
  });
  int ownerA = 0, ownerB = 0;
  require(!Precompiler::start(false), "unregistered start must fail");

  Event enteredA, releaseA, enteredB, releaseB;
  Precompiler::setRunner(&ownerA, [&](bool) {
    enteredA.signal();
    releaseA.wait();
  });
  require(Precompiler::start(false), "owner A job did not start");
  enteredA.wait();
  Precompiler::setRunner(&ownerB, [&](bool) { enteredB.signal(); releaseB.wait(); });
  // UI phase is progress only; it cannot authorize another active job.
  Precompiler::setPhase(Precompiler::Phase::Idle);
  require(Precompiler::busy() && !Precompiler::start(false), "active job was lost when UI phase changed");
  std::atomic<bool> clearedA { false };
  std::thread retireA([&] { Precompiler::clearRunner(&ownerA); clearedA.store(true); });
  eventually([] { return Precompiler::cancelRequested(); }, "old owner's active job was not cancelled after registration changed");
  require(!clearedA.load(), "old owner returned while its callback was still active");
  releaseA.signal();
  retireA.join();
  require(!Precompiler::busy() && !Precompiler::cancelRequested(), "completed job left active cancellation behind");
  require(Precompiler::status().runnerAvailable, "retiring A erased B's registration");

  require(Precompiler::start(true), "replacement owner B could not start");
  enteredB.wait();
  Precompiler::clearRunner(&ownerA);
  require(!Precompiler::cancelRequested(), "retiring old owner cancelled replacement owner's job");
  releaseB.signal();
  Precompiler::clearRunner(&ownerB);
  require(!Precompiler::busy() && !Precompiler::status().runnerAvailable, "replacement owner failed to retire");

  Event enteredOld, releaseOld;
  Precompiler::setRunner(&ownerA, [&](bool) { enteredOld.signal(); releaseOld.wait(); });
  require(Precompiler::start(false), "second old-owner job did not start");
  enteredOld.wait();
  Precompiler::setRunner(&ownerB, [](bool) { });
  Precompiler::clearRunner(&ownerB);
  require(!Precompiler::cancelRequested(), "clearing unused replacement registration cancelled old active owner");
  releaseOld.signal();
  Precompiler::clearRunner(&ownerA);

  bool failCopy = false;
  Precompiler::setRunner(&ownerA, ThrowingCopy(&failCopy));
  failCopy = true;
  require(!Precompiler::start(false) && !Precompiler::busy(), "callable-copy failure left a phantom active job");
  failCopy = false;
  require(Precompiler::start(false), "job did not recover after copy failure");
  Precompiler::clearRunner(&ownerA);

  for (bool standardException : { true, false }) {
    Precompiler::setRunner(&ownerA, [standardException](bool) {
      if (standardException) throw std::runtime_error("intentional worker failure");
      throw 42;
    });
    require(Precompiler::start(false), "throwing worker did not start");
    Precompiler::clearRunner(&ownerA);
    require(!Precompiler::busy() && !Precompiler::cancelRequested(), "worker exception left owner or cancellation active");
  }
  std::puts("Shader precompiler owner replacement, cancellation and exception lifetime checks passed.");
  testDone.signal();
  watchdog.join();
}
