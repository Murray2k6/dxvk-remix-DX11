#pragma once
#include <cstdint>
#include <cstddef>
#include <iterator>
#include <list>
#include <unordered_map>

namespace dx11_capture {
  // Geometry-version aliases reference one owned server mesh per content hash.
  // Eviction destroys an API mesh only after its final alias has retired.
  class MeshCache {
  public:
    static constexpr uint64_t MaximumBytes = 256ull << 20;
    static constexpr size_t MaximumAliases = 8192;

    uint32_t find(uint64_t key, uint64_t frame) {
      const auto entry = m_aliases.find(key);
      if (entry == m_aliases.end()) return 0;
      entry->second.frame = frame;
      m_lru.splice(m_lru.end(), m_lru, entry->second.position);
      return m_meshes.at(entry->second.hash).uid;
    }

    uint32_t findContent(uint64_t hash) const {
      const auto mesh = m_meshes.find(hash);
      return mesh == m_meshes.end() ? 0 : mesh->second.uid;
    }

    template<typename Destroy>
    bool reserve(uint64_t bytes, uint64_t frame, Destroy destroy) {
      if (bytes > MaximumBytes) return false;
      while (m_aliases.size() >= MaximumAliases || bytes > MaximumBytes - m_bytes) {
        if (m_lru.empty()) return false;
        auto alias = m_aliases.find(m_lru.front());
        // Current-frame DrawInstance commands may still refer to this mesh.
        if (alias->second.frame == frame) return false;
        auto mesh = m_meshes.find(alias->second.hash);
        if (--mesh->second.references == 0) {
          destroy(mesh->second.uid);
          m_bytes -= mesh->second.bytes;
          m_meshes.erase(mesh);
        }
        m_lru.pop_front();
        m_aliases.erase(alias);
      }
      return true;
    }

    void insert(uint64_t key, uint64_t hash, uint32_t uid, uint64_t bytes, uint64_t frame) {
      if (m_aliases.find(key) != m_aliases.end()) return;
      auto mesh = m_meshes.find(hash);
      if (mesh == m_meshes.end()) {
        mesh = m_meshes.emplace(hash, Mesh { uid, bytes, 0 }).first;
        m_bytes += bytes;
      }
      m_lru.push_back(key);
      m_aliases.emplace(key, Alias { hash, frame, std::prev(m_lru.end()) });
      ++mesh->second.references;
    }

    uint64_t bytes() const { return m_bytes; }
    size_t aliases() const { return m_aliases.size(); }
    size_t meshes() const { return m_meshes.size(); }

  private:
    struct Mesh { uint32_t uid; uint64_t bytes; size_t references; };
    struct Alias { uint64_t hash; uint64_t frame; std::list<uint64_t>::iterator position; };
    std::unordered_map<uint64_t, Mesh> m_meshes;
    std::unordered_map<uint64_t, Alias> m_aliases;
    std::list<uint64_t> m_lru;
    uint64_t m_bytes = 0;
  };
}
