#ifndef DATACODEC_RUNTIME_CACHE_DECODEREFERENCECACHE_H
#define DATACODEC_RUNTIME_CACHE_DECODEREFERENCECACHE_H

#include "DataCodec/Runtime/Cache/DecodeCache/DecodedTopologyCache.h"
#include "DataCodec/API/Adapter/DecodeCacheIdentity.h"
#include "DataCodec/Codec/Reference/DecodedReference.h"
#include "DataCodec/Runtime/Cache/LruCacheIndex.h"
#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace datacodec {

struct DecodedTopologyReference {
    std::shared_ptr<DecodedTopologyCache> store;
    std::shared_ptr<bytestore::ByteStoreSession> byteStoreSession;
};

struct DecodeReferenceLeaf {
    bool requiresAttribute{false};
    bool requiresGeometry{false};
    std::unordered_set<std::string> requiredTopology;
    std::optional<DecodedAttributeReference> attribute;
    std::optional<DecodedGeometryReference> geometry;
    std::unordered_map<std::string, DecodedTopologyReference> topology;
};

struct DecodeReferenceFrame {
    std::uint32_t frameIndex{0u};
    bool complete{false};
    std::unordered_map<BlockPath, DecodeReferenceLeaf> leaves;


};

struct DecodeReferenceCacheStats {
    std::uint64_t lookups{0u};
    std::uint64_t hits{0u};
    std::uint64_t misses{0u};
    std::uint64_t publishes{0u};
    std::uint64_t evictions{0u};
    std::size_t residentFrames{0u};
};

class DecodeReferenceCache final {
public:
    explicit DecodeReferenceCache(DataCodecExecutionResources* run = nullptr) noexcept : m_run(run) {}
    using FramePointer = std::shared_ptr<DecodeReferenceFrame>;

    class RequiredFrameLease final {
    public:
        RequiredFrameLease() = default;
        RequiredFrameLease(const RequiredFrameLease&) = delete;
        RequiredFrameLease& operator=(const RequiredFrameLease&) = delete;
        RequiredFrameLease(RequiredFrameLease&& other) noexcept
            : m_cache(std::exchange(other.m_cache, nullptr)), m_key(std::move(other.m_key)) {}
        RequiredFrameLease& operator=(RequiredFrameLease&& other) noexcept {
            if (this != &other) {
                Reset();
                m_cache = std::exchange(other.m_cache, nullptr);
                m_key = std::move(other.m_key);
            }
            return *this;
        }
        ~RequiredFrameLease() { Reset(); }
        void Reset() noexcept {
            if (auto* cache = std::exchange(m_cache, nullptr)) { cache->ReleaseRequired(m_key); }
        }
    private:
        friend class DecodeReferenceCache;
        RequiredFrameLease(DecodeReferenceCache* cache, DecodeReferenceKey key)
            : m_cache(cache), m_key(std::move(key)) {}
        DecodeReferenceCache* m_cache{nullptr};
        DecodeReferenceKey m_key;
    };

    // driver 在准备前驱前登记，许可仅覆盖本条命令的实际依赖
    RequiredFrameLease RequireFrame(const DecodeReferenceKey& key) {
        RequiredFrameLease lease(nullptr, key);
        {
            std::lock_guard lock(m_mutex);
            auto& required = m_required[key];
            if (required.count == 0u) {
                const auto cached = m_frames.find(key);
                if (cached != m_frames.end()) { required.frame = cached->second; }
            }
            ++required.count;
            lease.m_cache = this;
        }
        return lease;
    }

    void Configure(const std::size_t frameLimit) {
        std::vector<FramePointer> evicted;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_index.Configure(frameLimit);
            PruneLocked({}, evicted);
            RefreshStatsLocked();
        }
    }

    [[nodiscard]] FramePointer Find(const DecodeReferenceKey& key) {
        std::vector<FramePointer> evicted;
        FramePointer frame;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_stats.lookups;
            const auto required = m_required.find(key);
            if (required != m_required.end() && required->second.frame) {
                ++m_stats.hits;
                return required->second.frame;
            }
            const auto iterator = m_frames.find(key);
            if (iterator == m_frames.end()) {
                ++m_stats.misses;
                PruneLocked(std::nullopt, evicted);
                RefreshStatsLocked();
            } else {
                ++m_stats.hits;
                m_index.Touch(key);
                frame = iterator->second;
                PruneLocked(key, evicted);
                RefreshStatsLocked();
            }
        }
        return frame;
    }

    FramePointer Publish(const DecodeReferenceKey& key, DecodeReferenceFrame frame) {
        if (!key.source.IsStable() || frame.leaves.empty()) { return {}; }
        std::vector<FramePointer> evicted;
        FramePointer published;
        {
            std::lock_guard lock(m_mutex);
            const auto existing = m_frames.find(key);
            const auto required = m_required.find(key);
            if (required != m_required.end() && required->second.frame) {
                MergeFrame(*required->second.frame, frame);
            } else if (existing != m_frames.end()) {
                MergeFrame(*existing->second, frame);
            }
            frame.frameIndex = key.keyFrameIndex;
            frame.complete = IsComplete(frame);
            published = std::make_shared<DecodeReferenceFrame>(std::move(frame));
            if (required != m_required.end()) {
                evicted.push_back(std::move(required->second.frame));
                required->second.frame = published;
            }
            if (m_index.CanAdmitSingle() && (!m_run || m_run->OptionalRetentionAllowed())) {
                if (existing != m_frames.end()) {
                    evicted.push_back(std::move(existing->second));
                    existing->second = published;
                } else {
                    m_frames.emplace(key, published);
                }
                m_index.InsertOrAssign(key);
                PruneLocked(key, evicted);
            }
            ++m_stats.publishes;
            RefreshStatsLocked();
        }
        return published;
    }

    void InvalidateSource(const DecodeSourceIdentity& source) {
        std::vector<FramePointer> released;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto iterator = m_frames.begin(); iterator != m_frames.end();) {
                if (iterator->first.source.stableId != source.stableId) {
                    ++iterator;
                    continue;
                }
                released.push_back(std::move(iterator->second));
                m_index.Erase(iterator->first);
                iterator = m_frames.erase(iterator);
            }
            RefreshStatsLocked();
        }
    }

    void Clear() {
        std::unordered_map<DecodeReferenceKey, FramePointer, DecodeReferenceKeyHash> released;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            released.swap(m_frames);
            m_index.Clear();
            RefreshStatsLocked();
        }
    }

    [[nodiscard]] std::vector<std::uint32_t> ResidentFrameIndices(
        const DecodeSourceIdentity& source) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::uint32_t> frames;
        for (const auto& [key, frame] : m_frames) {
            (void)frame;
            if (key.source.stableId == source.stableId) { frames.push_back(key.keyFrameIndex); }
        }
        std::sort(frames.begin(), frames.end());
        frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
        return frames;
    }

    [[nodiscard]] DecodeReferenceCacheStats Statistics() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stats;
    }

    // 仅解除一个可选缓存引用，数据仍可由活跃消费者持有
    bool TrimOne() {
        FramePointer released;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto* key = m_index.LeastRecentlyUsedKey();
            if (key == nullptr) { return false; }
            const auto found = m_frames.find(*key);
            if (found != m_frames.end()) {
                released = std::move(found->second);
                m_frames.erase(found);
            }
            m_index.Erase(*key);
            ++m_stats.evictions;
            RefreshStatsLocked();
        }
        return true;
    }

private:
    struct RequiredFrame {
        std::size_t count{0u};
        FramePointer frame;
    };

    void ReleaseRequired(const DecodeReferenceKey& key) noexcept {
        FramePointer released;
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_required.find(key);
            if (found == m_required.end()) { return; }
            if (--found->second.count == 0u) {
                released = std::move(found->second.frame);
                m_required.erase(found);
            }
        }
    }

    std::unordered_map<DecodeReferenceKey, RequiredFrame, DecodeReferenceKeyHash> m_required;
    DataCodecExecutionResources* m_run{nullptr};
    static void MergeFrame(const DecodeReferenceFrame& existing, DecodeReferenceFrame& incoming) {
        for (const auto& [path, oldLeaf] : existing.leaves) {
            auto& leaf = incoming.leaves[path];
            leaf.requiresAttribute = leaf.requiresAttribute || oldLeaf.requiresAttribute;
            leaf.requiresGeometry = leaf.requiresGeometry || oldLeaf.requiresGeometry;
            leaf.requiredTopology.insert(
                oldLeaf.requiredTopology.begin(),
                oldLeaf.requiredTopology.end());
            if (!leaf.attribute.has_value() && oldLeaf.attribute.has_value()) {
                leaf.attribute = oldLeaf.attribute;
            }
            if (!leaf.geometry.has_value() && oldLeaf.geometry.has_value()) {
                leaf.geometry = oldLeaf.geometry;
            }
            for (const auto& [key, topology] : oldLeaf.topology) {
                leaf.topology.try_emplace(key, topology);
            }
        }
    }

    [[nodiscard]] static bool IsComplete(const DecodeReferenceFrame& frame) {
        for (const auto& [path, leaf] : frame.leaves) {
            (void)path;
            if (leaf.requiresAttribute &&
                (!leaf.attribute.has_value() ||
                 leaf.attribute->store == nullptr ||
                 !leaf.attribute->store->IsComplete())) {
                return false;
            }
            if (leaf.requiresGeometry &&
                (!leaf.geometry.has_value() ||
                 leaf.geometry->store == nullptr ||
                 !leaf.geometry->store->IsComplete())) {
                return false;
            }
            for (const auto& key : leaf.requiredTopology) {
                const auto topology = leaf.topology.find(key);
                if (topology == leaf.topology.end() ||
                    topology->second.store == nullptr ||
                    !topology->second.store->complete) {
                    return false;
                }
            }
        }
        return !frame.leaves.empty();
    }

    void PruneLocked(
        const std::optional<DecodeReferenceKey>& protectedKey,
        std::vector<FramePointer>& evicted) {
        for (const auto& candidate : m_index.LeastRecentlyUsedOrder()) {
            if (!m_index.OverBudget()) { break; }
            if (protectedKey.has_value() && candidate == *protectedKey) { continue; }
            const auto iterator = m_frames.find(candidate);
            if (iterator == m_frames.end()) {
                m_index.Erase(candidate);
                continue;
            }
            evicted.push_back(std::move(iterator->second));
            m_frames.erase(iterator);
            m_index.Erase(candidate);
            ++m_stats.evictions;
        }
    }

    void RefreshStatsLocked() noexcept {
        m_stats.residentFrames = m_index.Size();
    }

    mutable std::mutex m_mutex;
    std::unordered_map<DecodeReferenceKey, FramePointer, DecodeReferenceKeyHash> m_frames;
    LruCacheIndex<DecodeReferenceKey, DecodeReferenceKeyHash> m_index;
    DecodeReferenceCacheStats m_stats;
};

} // namespace datacodec

#endif
