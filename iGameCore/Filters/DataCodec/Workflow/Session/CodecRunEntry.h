#ifndef DATACODEC_WORKFLOW_SESSION_CODECRUNENTRY_H
#define DATACODEC_WORKFLOW_SESSION_CODECRUNENTRY_H

#include "DataCodec/API/Entry/DataCodecEncodeEntry.h"
#include "DataCodec/API/Entry/DataCodecDecodeEntry.h"
#include "DataCodec/Runtime/Execution/DataCodecExecutionResources.h"

namespace datacodec {

EncodeResult EncodeInRun(const EncodeRequest&, DataCodecExecutionResources&);
DecodePackageResult DecodePackageInRun(const DecodePackageRequest&, DataCodecExecutionResources&, DecodeSession*);

}

#endif
