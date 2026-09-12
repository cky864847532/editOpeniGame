#include "DataCodec/API/Entry/DecodeStorageAnalysis.h"
#include "DataCodec/Workflow/Decode/DecodeStoragePlan.h"

namespace datacodec {
DecodeStorageAnalysisResult AnalyzeDecodeStorage(const DecodeStorageAnalysisRequest& request) {
    return storageplan::Analyze(request);
}
}
