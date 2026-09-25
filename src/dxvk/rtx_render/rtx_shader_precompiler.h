/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
#pragma once

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>

#include "../../util/thread.h"
#include "../../util/util_env.h"
#include "../../util/log/log.h"

namespace dxvk {

  // DX11_V292_PRECOMPILER_WIDGET: bridge between the d3d11 layer (which owns
  // the game-file shader scanner and the DXBC cache preloader) and the Remix
  // developer menu, which renders a Shader Precompiler widget on top of it.
  // This mirrors the Fossilize / Steam shader pre-caching model: everything
  // the game has ever used - or that the scanner can harvest from the game's
  // own files - is compiled up front, on demand, with visible progress,
  // instead of stalling gameplay at each first use.
  class RtxShaderPrecompiler {
  public:
    enum class Phase : uint32_t {
      Idle = 0,
      Scanning = 1,
      Compiling = 2,
    };

    struct Status {
      Phase    phase;
      bool     runnerAvailable;
      uint32_t scanFilesExamined;
      uint32_t scanFilesTotal;
      uint32_t scanNewShaders;
      uint32_t cachedShadersOnDisk;
      uint32_t loadedShaders;
      uint32_t rejectedShaders;
    };

    static Status status() {
      Status result;
      result.phase = s_phase.load(std::memory_order_acquire);
      result.runnerAvailable = s_runnerOwner.load(std::memory_order_acquire) != nullptr;
      result.scanFilesExamined = s_scanFilesExamined.load(std::memory_order_acquire);
      result.scanFilesTotal = s_scanFilesTotal.load(std::memory_order_acquire);
      result.scanNewShaders = s_scanNewShaders.load(std::memory_order_acquire);
      result.cachedShadersOnDisk = s_cachedShadersOnDisk.load(std::memory_order_acquire);
      result.loadedShaders = s_loadedShaders.load(std::memory_order_acquire);
      result.rejectedShaders = s_rejectedShaders.load(std::memory_order_acquire);
      return result;
    }

    static bool busy() {
      return s_activeOwner.load(std::memory_order_acquire) != nullptr;
    }

    static bool cancelRequested() {
      return s_cancelRequested.load(std::memory_order_acquire);
    }

    // --- progress reporting (called by the d3d11 runner implementation) ---

    static void setPhase(Phase phase) {
      s_phase.store(phase, std::memory_order_release);
    }

    static void reportScanProgress(uint32_t examined, uint32_t total, uint32_t newShaders) {
      s_scanFilesExamined.store(examined, std::memory_order_release);
      s_scanFilesTotal.store(total, std::memory_order_release);
      s_scanNewShaders.store(newShaders, std::memory_order_release);
    }

    static void reportCacheCounts(uint32_t onDisk, uint32_t loaded, uint32_t rejected) {
      s_cachedShadersOnDisk.store(onDisk, std::memory_order_release);
      s_loadedShaders.store(loaded, std::memory_order_release);
      s_rejectedShaders.store(rejected, std::memory_order_release);
    }

    // --- runner registration (d3d11 device side) ---

    static void setRunner(void* owner, std::function<void(bool fullRescan)> runner) {
      std::lock_guard<dxvk::mutex> lock(mutex());
      s_runner = std::move(runner);
      s_runnerOwner.store(owner, std::memory_order_release);
    }

    // Registration can move to a new device while the old device's job runs.
    // Wait for the active owner independently of the current registration.
    static void clearRunner(void* owner) {
      if (!owner) return;
      std::unique_lock<dxvk::mutex> lock(mutex());
      if (s_runnerOwner.load(std::memory_order_relaxed) == owner) {
        s_runner = nullptr;
        s_runnerOwner.store(nullptr, std::memory_order_release);
      }
      if (s_activeOwner.load(std::memory_order_relaxed) == owner) {
        s_cancelRequested.store(true, std::memory_order_release);
        completed().wait(lock, [owner] {
          return s_activeOwner.load(std::memory_order_relaxed) != owner;
        });
      }
    }

    // --- UI side ---

    // Starts a precompile job on a background thread. fullRescan also
    // re-reads the game's own data files (ignoring the completed-scan
    // marker) before compiling. Returns false when no runner is registered
    // or a job is already running.
    static bool start(bool fullRescan) {
      std::lock_guard<dxvk::mutex> lock(mutex());
      const auto owner = s_runnerOwner.load(std::memory_order_relaxed);
      if (!s_runner || !owner || s_activeOwner.load(std::memory_order_relaxed))
        return false;

      bool claimedActiveSlot = false;
      try {
        // Copy before claiming the active slot: a throwing callable copy must
        // not leave a phantom job that a device destructor waits for forever.
        auto runner = s_runner;
        (void) completed();
        s_cancelRequested.store(false, std::memory_order_release);
        s_activeOwner.store(owner, std::memory_order_release);
        s_phase.store(fullRescan ? Phase::Scanning : Phase::Compiling,
          std::memory_order_release);
        claimedActiveSlot = true;
        dxvk::thread worker([runner = std::move(runner), fullRescan]() mutable {
          JobCompletion completion { runner };
          try {
            env::setThreadName("rtx-precompiler");
            runner(fullRescan);
          } catch (const std::exception& error) {
            Logger::warn(std::string("Shader precompiler failed: ") + error.what());
          } catch (...) {
            Logger::warn("Shader precompiler failed with an unknown exception");
          }
        });
        worker.detach();
        return true;
      } catch (...) {
        // Also release the active slot when thread creation fails.
        if (claimedActiveSlot)
          finishJobLocked();
        return false;
      }
    }

  private:
    struct JobCompletion {
      std::function<void(bool)>& runner;
      ~JobCompletion() {
        // This also runs when the callback or exception logging throws.
        // Destroy captures before signaling the owner may be destroyed.
        runner = nullptr;
        std::lock_guard<dxvk::mutex> lock(mutex());
        finishJobLocked();
      }
    };

    static dxvk::mutex& mutex() {
      // Leaked deliberately: the detached worker may outlive static dtors.
      static dxvk::mutex* s_mutex = new dxvk::mutex();
      return *s_mutex;
    }

    static dxvk::condition_variable& completed() {
      static auto* condition = new dxvk::condition_variable();
      return *condition;
    }

    static void finishJobLocked() {
      s_cancelRequested.store(false, std::memory_order_release);
      s_phase.store(Phase::Idle, std::memory_order_release);
      s_activeOwner.store(nullptr, std::memory_order_release);
      completed().notify_all();
    }

    inline static std::function<void(bool)> s_runner;               // guarded by mutex()
    inline static std::atomic<void*> s_runnerOwner { nullptr };
    inline static std::atomic<void*> s_activeOwner { nullptr };     // writes guarded by mutex()
    inline static std::atomic<Phase> s_phase { Phase::Idle };
    inline static std::atomic<bool> s_cancelRequested { false };
    inline static std::atomic<uint32_t> s_scanFilesExamined { 0u };
    inline static std::atomic<uint32_t> s_scanFilesTotal { 0u };
    inline static std::atomic<uint32_t> s_scanNewShaders { 0u };
    inline static std::atomic<uint32_t> s_cachedShadersOnDisk { 0u };
    inline static std::atomic<uint32_t> s_loadedShaders { 0u };
    inline static std::atomic<uint32_t> s_rejectedShaders { 0u };
  };

} // namespace dxvk
