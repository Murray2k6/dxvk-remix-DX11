#include "../../../src/dxvk/dxvk_compiler_policy.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

static void require(bool condition, const char* message) {
  if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

int main() {
  using namespace dxvk;
  for (uint32_t cpus : { 0u, 1u, 2u, 4u, 16u, 64u, 256u, UINT32_MAX }) {
    for (uint32_t requested : { 0u, 1u, 4u, 32u, UINT32_MAX }) {
      for (uint64_t memory : { 0ull, 1ull, 1ull << 30, 8ull << 30, UINT64_MAX }) {
        const auto workers = pipelineCompilerThreads(cpus, requested, memory);
        require(workers >= 1 && workers <= 16, "compiler thread count escaped bounds");
        require(workers <= std::max(1u, cpus), "compiler oversubscribed host threads");
        if (memory && memory <= (1ull << 30))
          require(workers == 1, "low-memory host started multiple driver compilers");
        if (!requested)
          require(workers <= 4, "automatic compiler pool exceeded four workers");
      }
      const auto remix = remixCompilerThreads(cpus, requested);
      require(remix >= 1 && remix <= 4, "Remix concurrency escaped bounds");
    }
    for (uint32_t driver : { 0u, 1u, 2u, 32u, UINT32_MAX }) {
      const auto helpers = deferredCompilerHelpers(driver, cpus);
      require(helpers <= 2, "driver concurrency sentinel expanded into unbounded work");
      if (driver <= 1) require(helpers == 0, "completed/serial operation scheduled helpers");
    }
  }
  ShaderPreloadBudget countBudget;
  require(!countBudget.admit(0), "empty bytecode admitted");
  for (uint32_t i = 0; i < ShaderPreloadBudget::MaximumModules; ++i)
    require(countBudget.admit(1), "module budget rejected an in-range shader");
  require(!countBudget.admit(1), "module budget allowed unbounded preload");
  ShaderPreloadBudget byteBudget;
  require(!byteBudget.admit(UINT64_MAX), "bytecode size overflow bypassed memory budget");
  require(byteBudget.admit(ShaderPreloadBudget::MaximumBytecodeBytes), "exact byte budget rejected");
  require(!byteBudget.admit(1), "bytecode budget exceeded");
  std::puts("Compiler CPU/memory bounds, Vulkan unknown-concurrency and preload overflow tests passed.");
}
