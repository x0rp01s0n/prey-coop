#pragma once

#include "CoopNativeFragmentPayload.h"
#include "CoopNativeFragmentResolver.h"
#include "CoopNativeSaveStoreApi.h"

#include <cstdint>
#include <string>

namespace CoopNativePreloadRebuilder
{
struct PreloadRebuildPlan
{
    bool attempted = false;
    bool ok = false;
    std::string reason;
    uint32_t sourceItems = 0;
    uint32_t targetItems = 0;
    uint32_t materializerOps = 0;
    uint32_t fullTransplantOps = 0;
    uint32_t insertNodeOps = 0;
    uint32_t insertAttrOps = 0;
    uint32_t insertChildOps = 0;
    uint32_t replaceNodeOps = 0;
    uint32_t replaceAttrOps = 0;
    uint32_t replaceChildOps = 0;
    uint32_t rebaseAttrOps = 0;
    uint32_t rebaseChildOps = 0;
    uint32_t itemRemapOps = 0;
    uint32_t entityIdAllocations = 0;
    uint32_t entityIdAllocationPlanReady = 0;
    uint32_t writeStoreMaterializeOps = 0;
    uint32_t nativeWriteStoreBackendReady = 0;
    uint32_t savePackageSectionRewriteReady = 0;
    uint32_t unsupportedOps = 0;
    uint32_t oracleMissingOps = 0;
    uint32_t readStoreOnlyAllocatorOps = 0;
    CoopNativeSaveStoreApi::WriteStoreBuilderBackendStatus writeStoreBackend;
};

PreloadRebuildPlan BuildPreloadRebuildPlan(
    const CoopNativeFragmentPayload::ParsedPayload& source,
    const CoopNativeFragmentResolver::InventoryFragmentResolution& resolution);

std::string BuildStatus(const PreloadRebuildPlan& plan);
}
