#pragma once

#include "CoopNativeFragmentImportPlanner.h"
#include "CoopNativeEntityIdAllocator.h"
#include "CoopNativeFragmentPayload.h"

#include <cstdint>
#include <string>
#include <vector>

namespace CoopNativeFragmentMaterializer
{
struct MaterializedStorePatchOp
{
    enum class Kind : uint8_t
    {
        ReplaceExistingReadNode,
        InsertReadNode,
        ReplaceAttrRecord,
        InsertAttrRecord,
        ReplaceChildRecord,
        InsertChildRecord,
        RebaseAttrCursor,
        RebaseChildCursor,
        RemapItemEntityId,
        RemapOwnerId,
    };

    Kind kind = Kind::ReplaceExistingReadNode;
    uint32_t sourceRangeIndex = 0;
    uint32_t sourceGroupKind = 0;
    uint32_t sourceRangeKind = 0;
    uint32_t sourceOrdinal = 0xFFFFFFFFu;
    uint32_t targetNodeIndex = 0;
    uint32_t sourceCursor = 0;
    uint32_t targetCursor = 0;
    uint32_t sourceCount = 0;
    uint32_t byteSize = 0;
    uint32_t sourceItemEntityId = 0;
    uint32_t targetItemEntityId = 0;
    uint32_t sourceOwnerId = 0;
    uint32_t targetOwnerId = 0;
    bool requiresAllocation = false;
    bool safe = false;
};

struct MaterializerPlan
{
    bool attempted = false;
    bool ok = false;
    std::string reason;
    uint32_t ops = 0;
    uint32_t safeOps = 0;
    uint32_t unsafeOps = 0;
    uint32_t unsupportedOps = 0;
    uint32_t materializerInputOps = 0;
    uint32_t replaceNodeOps = 0;
    uint32_t insertNodeOps = 0;
    uint32_t replaceAttrOps = 0;
    uint32_t insertAttrOps = 0;
    uint32_t replaceChildOps = 0;
    uint32_t insertChildOps = 0;
    uint32_t rebaseAttrOps = 0;
    uint32_t rebaseChildOps = 0;
    uint32_t itemRemapOps = 0;
    uint32_t ownerRemapOps = 0;
    uint32_t allocationOps = 0;
    uint32_t transcodeOps = 0;
    uint32_t oracleMappedOps = 0;
    uint32_t oracleMissingOps = 0;
    uint32_t fullTransplantOps = 0;
    uint32_t entityAllocationOps = 0;
    uint32_t missingSubtreeInsertOps = 0;
    uint32_t newItemInsertOps = 0;
    uint32_t sourceItems = 0;
    uint32_t targetItems = 0;
    std::vector<MaterializedStorePatchOp> patchOps;
};

MaterializerPlan BuildReadOnlyMaterializerPlan(
    const CoopNativeFragmentPayload::ParsedPayload& source,
    const CoopNativeFragmentImportPlanner::TargetInventoryFragmentBundle& target,
    const CoopNativeFragmentImportPlanner::NativeFragmentImportPlan& importPlan,
    const CoopNativeFragmentImportPlanner::NativeFragmentRebuildPlan& rebuildPlan);

MaterializerPlan ApplyEntityIdAllocationPlan(
    const MaterializerPlan& plan,
    const CoopNativeEntityIdAllocator::AllocationPlan& allocationPlan);

std::string BuildMaterializerStatus(const MaterializerPlan& plan);
}
