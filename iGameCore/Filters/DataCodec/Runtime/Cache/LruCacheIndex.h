#ifndef DATACODEC_RUNTIME_CACHE_LRUCACHEINDEX_H
#define DATACODEC_RUNTIME_CACHE_LRUCACHEINDEX_H

#include <cstddef>
#include <list>
#include <optional>
#include <unordered_map>
#include <vector>

namespace datacodec {

// 仅维护条目数与 LRU 顺序，数组容量由唯一数据 owner 持有
template<typename Key, typename Hash>
class LruCacheIndex final {
public:
    void Configure(const std::size_t entryLimit) noexcept { m_entryLimit = entryLimit; }

    void Touch(const Key& key) {
        const auto found = m_records.find(key);
        if (found != m_records.end()) { m_order.splice(m_order.begin(), m_order, found->second); }
    }

    void InsertOrAssign(const Key& key, const bool mostRecent = true) {
        if (m_records.contains(key)) {
            if (mostRecent) { Touch(key); }
            return;
        }
        const auto position = mostRecent ? m_order.insert(m_order.begin(), key) : m_order.insert(m_order.end(), key);
        try { m_records.emplace(key, position); }
        catch (...) { m_order.erase(position); throw; }
    }

    void Erase(const Key& key) noexcept {
        const auto found = m_records.find(key);
        if (found == m_records.end()) { return; }
        m_order.erase(found->second);
        m_records.erase(found);
    }

    void Clear() noexcept { m_records.clear(); m_order.clear(); }
    [[nodiscard]] bool OverBudget() const noexcept { return m_records.size() > m_entryLimit; }
    [[nodiscard]] bool CanAdmitSingle() const noexcept { return m_entryLimit != 0u; }
    [[nodiscard]] const Key* LeastRecentlyUsedKey() const noexcept { return m_order.empty() ? nullptr : &m_order.back(); }
    [[nodiscard]] std::optional<Key> LeastRecentlyUsed() const {
        if (m_order.empty()) { return std::nullopt; }
        return m_order.back();
    }
    [[nodiscard]] std::vector<Key> LeastRecentlyUsedOrder() const { return {m_order.rbegin(), m_order.rend()}; }
    [[nodiscard]] std::size_t Size() const noexcept { return m_records.size(); }

private:
    std::size_t m_entryLimit{1u};
    std::list<Key> m_order;
    std::unordered_map<Key, typename std::list<Key>::iterator, Hash> m_records;
};

} // DataCodec 命名空间
#endif
