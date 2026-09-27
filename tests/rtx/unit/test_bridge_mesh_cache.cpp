#include "../../../bridge_dx11_work/src/client_dx11/dx11_mesh_cache.h"
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

static void require(bool condition, const char* message) {
  if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

int main() {
  using dx11_capture::MeshCache;
  std::unordered_set<uint32_t> destroyed;
  const auto destroy = [&](uint32_t uid) {
    require(destroyed.insert(uid).second, "server mesh destroyed more than once");
  };

  // Multiple geometry versions can have identical bytes and share one API mesh.
  MeshCache aliases;
  require(aliases.reserve(MeshCache::MaximumBytes, 1, destroy), "initial budget");
  aliases.insert(1, 101, 501, MeshCache::MaximumBytes, 1);
  aliases.insert(2, 101, 501, MeshCache::MaximumBytes, 1);
  aliases.insert(2, 101, 501, MeshCache::MaximumBytes, 1);
  require(aliases.meshes() == 1 && aliases.aliases() == 2
       && aliases.bytes() == MeshCache::MaximumBytes, "duplicate mesh/alias accounting");
  require(aliases.findContent(101) == 501, "canonical mesh lookup");
  require(!aliases.reserve(1, 1, destroy) && destroyed.empty(), "current frame mesh evicted");
  require(aliases.find(2, 2) == 501, "reuse did not preserve mesh identity");
  require(!aliases.reserve(1, 2, destroy), "current frame alias did not pin canonical mesh");
  require(aliases.aliases() == 1 && aliases.meshes() == 1 && destroyed.empty(), "premature canonical destruction");
  require(aliases.reserve(1, 3, destroy), "retired mesh could not be reclaimed");
  require(destroyed.count(501) == 1 && !aliases.findContent(101)
       && aliases.aliases() == 0 && aliases.bytes() == 0, "final alias did not release server allocation");
  require(!aliases.reserve(UINT64_MAX, 4, destroy), "overflowing allocation admitted");

  // Alias cardinality is bounded even when all aliases reference one tiny mesh.
  MeshCache cardinality;
  for (size_t i = 0; i < MeshCache::MaximumAliases; ++i) {
    require(cardinality.reserve(i == 0 ? 4 : 0, 1, destroy), "alias capacity underflow");
    cardinality.insert(i + 1, 202, 502, 4, 1);
  }
  require(!cardinality.reserve(0, 1, destroy), "current frame exceeded alias bound");
  require(cardinality.reserve(0, 2, destroy), "old alias not evicted");
  require(cardinality.aliases() == MeshCache::MaximumAliases - 1
       && cardinality.meshes() == 1 && destroyed.count(502) == 0, "alias eviction destroyed a shared mesh");

  // Dynamic geometry must remain bounded over many frames and destroy every
  // server allocation exactly once after the last frame that references it.
  MeshCache dynamic;
  uint32_t nextUid = 1000;
  constexpr uint64_t meshBytes = 8ull << 20;
  for (uint64_t frame = 1; frame <= 10000; ++frame) {
    for (unsigned draw = 0; draw != 8; ++draw) {
      const uint32_t uid = ++nextUid;
      require(dynamic.reserve(meshBytes, frame, destroy), "bounded dynamic frame rejected");
      dynamic.insert(uid, uid, uid, meshBytes, frame);
      require(dynamic.find(uid, frame) == uid && !destroyed.count(uid), "active mesh was destroyed");
    }
    require(dynamic.bytes() <= MeshCache::MaximumBytes
         && dynamic.aliases() <= MeshCache::MaximumAliases, "dynamic cache exceeded memory/cardinality bound");
  }
  require(dynamic.reserve(MeshCache::MaximumBytes, 10001, destroy), "final dynamic reclamation");
  require(dynamic.meshes() == 0 && dynamic.aliases() == 0 && dynamic.bytes() == 0, "dynamic allocations leaked");
  for (uint32_t uid = 1001; uid <= nextUid; ++uid)
    require(destroyed.count(uid) == 1, "server allocation missing DestroyMesh");
  std::puts("Bridge mesh ownership, alias deduplication, current-frame pinning and 10,000-frame memory bounds passed.");
}
