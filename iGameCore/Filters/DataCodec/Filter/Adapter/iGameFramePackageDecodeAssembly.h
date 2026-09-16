#ifndef iGameFramePackageDecodeAssembly_h
#define iGameFramePackageDecodeAssembly_h

#include "DataCodec/API/Output/DecodedData.h"
#include "DataCodec/API/Adapter/DecodedFrameTypes.h"
#include "DataCodec/API/Output/DecodedData.h"
#include "iGameDataObject.h"

#include <memory>
#include <unordered_map>
#include <utility>

IGAME_NAMESPACE_BEGIN

class iGameDecodeAdapter;

[[nodiscard]] DataObject::Pointer DataObjectFromDecodedFrame(
    const ::datacodec::DecodedFrame::Pointer& frame);

class iGameFramePackageDecodeAssembly final {
public:
    bool Import(const ::datacodec::DecodedData& data, std::string* error = nullptr);
    bool BeginFramePackage(const ::datacodec::DecodedData& framePackage,
                           std::string* error = nullptr);
    bool AddBranch(const ::datacodec::DecodedBranch& branch,
                   std::string* error = nullptr);
    bool CommitLeaf(const ::datacodec::DecodedLeaf& leaf,
                    iGameDecodeAdapter& adapter,
                    std::string* error = nullptr);
    bool EndFramePackage(std::string* error = nullptr);
    void AbortFramePackage();

    [[nodiscard]] std::unique_ptr<iGameDecodeAdapter> CreateiGameSupplementAdapter(
            const ::datacodec::BlockPath& path,
            std::string* error = nullptr) const;

    [[nodiscard]] DataObject::Pointer Output() const noexcept { return m_output; }
    [[nodiscard]] DataObject::Pointer LeafOutput(const ::datacodec::BlockPath& path) const;

private:
    DataObject::Pointer EnsureBranchNode(const ::datacodec::BlockPath& path,
                                         const std::string& name);

    DataObject::Pointer m_root;
    DataObject::Pointer m_output;
    std::unordered_map<::datacodec::BlockPath, DataObject::Pointer> m_nodes;
    ::datacodec::BlockPath m_directLeafPath;
    std::uint32_t m_frameIndex{0u};
    bool m_directLeafOutput{false};
};

IGAME_NAMESPACE_END

#endif
