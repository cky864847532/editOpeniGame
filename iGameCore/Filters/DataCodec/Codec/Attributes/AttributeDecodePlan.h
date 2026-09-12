#ifndef DATACODEC_CODEC_ATTRIBUTES_ATTRIBUTEDECODEPLAN_H
#define DATACODEC_CODEC_ATTRIBUTES_ATTRIBUTEDECODEPLAN_H

#include "DataCodec/API/Params/CodecStorageParams.h"
#include <span>

namespace datacodec::decodeimpl::detail {

inline bool ResolveAttributeIntraParents(const CodecStorageParams& params, std::size_t index,
    std::vector<std::size_t>& parents, std::string* error = nullptr) {
    parents.clear();
    if (index >= params.attrParams.size()) {
        return validation::AssignError(error, "attribute dependency index is out of range");
    }
    for (const auto& layout : params.attrParams[index].blockLayouts) {
        if (layout.referenceKind != NumericArrayReferenceKind::IntraArray) { continue; }
        const auto parent = static_cast<std::size_t>(layout.localParentFieldIndex);
        if (parent >= params.attrParams.size() || parent == index) {
            return validation::AssignError(error, "attribute intra-field parent index is invalid");
        }
        if (std::find(parents.begin(), parents.end(), parent) == parents.end()) { parents.push_back(parent); }
    }
    return true;
}

// 容量计划与实际解码共享相同的属性前驱顺序
inline bool ResolveAttributeExecutionOrder(const CodecStorageParams& params,
    std::span<const std::size_t> targets, std::vector<std::size_t>& order, std::string* error = nullptr) {
    order.clear();
    std::vector<std::vector<std::size_t>> parents(params.attrParams.size());
    for (std::size_t i = 0u; i < parents.size(); ++i) {
        if (!ResolveAttributeIntraParents(params, i, parents[i], error)) { return false; }
    }
    std::vector<std::uint8_t> visited(parents.size(), 0u);
    // 显式栈避免恶意长依赖链耗尽调用栈
    std::vector<std::pair<std::size_t, std::size_t>> stack;
    for (const auto target : targets) {
        if (target >= parents.size()) { return validation::AssignError(error, "attribute target index is out of range"); }
        if (visited[target] == 2u) { continue; }
        stack.emplace_back(target, 0u);
        while (!stack.empty()) {
            auto& [index, cursor] = stack.back();
            visited[index] = 1u;
            if (cursor == parents[index].size()) {
                visited[index] = 2u;
                order.push_back(index);
                stack.pop_back();
                continue;
            }
            const auto parent = parents[index][cursor++];
            if (visited[parent] == 1u) { return validation::AssignError(error, "attribute dependency graph contains a cycle"); }
            if (visited[parent] == 0u) { stack.emplace_back(parent, 0u); }
        }
    }
    return true;
}

}
#endif
