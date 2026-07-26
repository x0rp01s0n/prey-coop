#include "CoopNativeFragmentMaterializer.h"

#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace
{
bool IsInventoryRange(const CoopNativeFragmentPayload::PayloadRangeRecord& range)
{
    return range.groupKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadGroupKind::Inventory);
}

bool IsItemRange(const CoopNativeFragmentPayload::PayloadRangeRecord& range)
{
    return range.groupKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadGroupKind::Item);
}

bool IsItemGroupKind(uint32_t groupKind)
{
    return groupKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadGroupKind::Item);
}

bool IsNodeRange(uint32_t rangeKind)
{
    return
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::NodeBlock) ||
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteNodeBlock);
}

bool IsAttrRange(uint32_t rangeKind)
{
    return
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::AttrDataPool) ||
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteAttrVector);
}

bool IsChildRange(uint32_t rangeKind)
{
    return
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::ChildIndexBlock) ||
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteChildVector);
}

bool IsWriteRange(uint32_t rangeKind)
{
    return
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteNodeBlock) ||
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteAttrVector) ||
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteChildVector);
}

bool HasOracleForRange(const CoopNativeFragmentPayload::ParsedPayload& source, uint32_t rangeIndex)
{
    for (const CoopNativeFragmentPayload::PayloadWriteNodeOracleRecord& record : source.oracleRecords)
    {
        if (record.sourceRangeIndex == rangeIndex)
            return true;
    }
    return false;
}

std::string StatusToken(std::string value)
{
    if (value.empty())
        return "-";

    for (char& ch : value)
    {
        if (ch <= ' ' || ch == '"' || ch == '\'' || ch == '\\')
            ch = '_';
    }
    return value;
}

template <typename T>
const T* FindBySourceRange(const std::vector<T>& values, uint32_t sourceRangeIndex)
{
    for (const T& value : values)
    {
        if (value.sourceRangeIndex == sourceRangeIndex)
            return &value;
    }
    return nullptr;
}

void CountOp(
    CoopNativeFragmentMaterializer::MaterializerPlan& plan,
    const CoopNativeFragmentMaterializer::MaterializedStorePatchOp& op)
{
    using Kind = CoopNativeFragmentMaterializer::MaterializedStorePatchOp::Kind;

    switch (op.kind)
    {
    case Kind::ReplaceExistingReadNode:
        ++plan.replaceNodeOps;
        break;
    case Kind::InsertReadNode:
        ++plan.insertNodeOps;
        break;
    case Kind::ReplaceAttrRecord:
        ++plan.replaceAttrOps;
        break;
    case Kind::InsertAttrRecord:
        ++plan.insertAttrOps;
        break;
    case Kind::ReplaceChildRecord:
        ++plan.replaceChildOps;
        break;
    case Kind::InsertChildRecord:
        ++plan.insertChildOps;
        break;
    case Kind::RebaseAttrCursor:
        ++plan.rebaseAttrOps;
        break;
    case Kind::RebaseChildCursor:
        ++plan.rebaseChildOps;
        break;
    case Kind::RemapItemEntityId:
        ++plan.itemRemapOps;
        break;
    case Kind::RemapOwnerId:
        ++plan.ownerRemapOps;
        break;
    }

    if (op.requiresAllocation)
    {
        ++plan.allocationOps;
        if (IsItemGroupKind(op.sourceGroupKind) && op.sourceItemEntityId != 0)
        {
            if (op.targetItemEntityId == op.sourceItemEntityId)
                ++plan.missingSubtreeInsertOps;
            else if (op.targetItemEntityId == 0)
                ++plan.newItemInsertOps;
        }
    }
    if (op.safe)
        ++plan.safeOps;
    else
        ++plan.unsafeOps;
}

void AddOp(
    CoopNativeFragmentMaterializer::MaterializerPlan& plan,
    CoopNativeFragmentMaterializer::MaterializedStorePatchOp op)
{
    CountOp(plan, op);
    plan.patchOps.push_back(op);
}

void RefreshPlanReason(CoopNativeFragmentMaterializer::MaterializerPlan& plan)
{
    plan.fullTransplantOps =
        plan.insertNodeOps +
        plan.insertAttrOps +
        plan.insertChildOps +
        plan.itemRemapOps +
        plan.ownerRemapOps +
        plan.entityAllocationOps +
        plan.allocationOps +
        plan.transcodeOps;
    plan.ok = false;
    if (plan.unsupportedOps != 0)
        plan.reason = "unsupported_ops";
    else if (plan.entityAllocationOps != 0)
        plan.reason = "full_transplant_entity_id_allocator_required";
    else if (plan.missingSubtreeInsertOps != 0 && plan.newItemInsertOps == 0)
        plan.reason = "full_transplant_missing_subtree_read_store_allocator_required";
    else if (plan.insertNodeOps != 0 || plan.insertAttrOps != 0 || plan.insertChildOps != 0 || plan.allocationOps != 0)
        plan.reason = "full_transplant_read_store_allocator_required";
    else if (plan.transcodeOps != 0)
        plan.reason = "full_transplant_write_to_read_transcoder_required";
    else if (plan.itemRemapOps != 0 || plan.ownerRemapOps != 0)
        plan.reason = "full_transplant_remap_writer_required";
    else if (plan.unsafeOps != 0)
        plan.reason = "unsafe_ops";
    else
    {
        plan.ok = true;
        plan.reason = "same_shape_readonly";
    }
}

CoopNativeFragmentMaterializer::MaterializedStorePatchOp MakeBaseOp(
    CoopNativeFragmentMaterializer::MaterializedStorePatchOp::Kind kind,
    const CoopNativeFragmentPayload::PayloadRangeRecord& range,
    const CoopNativeFragmentImportPlanner::StoreRangeRewriteOp& rewriteOp,
    const CoopNativeFragmentImportPlanner::StoreRangeImportStep* step)
{
    CoopNativeFragmentMaterializer::MaterializedStorePatchOp op;
    op.kind = kind;
    op.sourceRangeIndex = rewriteOp.sourceRangeIndex;
    op.sourceGroupKind = rewriteOp.sourceGroupKind;
    op.sourceRangeKind = rewriteOp.sourceRangeKind;
    op.sourceOrdinal = rewriteOp.sourceOrdinal;
    op.targetNodeIndex = step ? step->plannedTargetNodeIndex : rewriteOp.targetNodeIndex;
    op.sourceCursor = IsChildRange(range.rangeKind) ? range.childCursor : range.attrCursor;
    op.targetCursor = rewriteOp.targetCursorBefore;
    op.sourceCount = IsChildRange(range.rangeKind) ? range.childCount : range.attrCount;
    op.byteSize = rewriteOp.byteSize;
    op.sourceItemEntityId = rewriteOp.sourceEntityId;
    op.targetItemEntityId = rewriteOp.targetEntityId;
    op.safe = !rewriteOp.requiresInsert && rewriteOp.shapeCompatible;
    op.requiresAllocation = rewriteOp.requiresInsert;
    return op;
}
}

namespace CoopNativeFragmentMaterializer
{
MaterializerPlan BuildReadOnlyMaterializerPlan(
    const CoopNativeFragmentPayload::ParsedPayload& source,
    const CoopNativeFragmentImportPlanner::TargetInventoryFragmentBundle& target,
    const CoopNativeFragmentImportPlanner::NativeFragmentImportPlan& importPlan,
    const CoopNativeFragmentImportPlanner::NativeFragmentRebuildPlan& rebuildPlan)
{
    MaterializerPlan plan;
    plan.attempted = true;
    plan.sourceItems = source.itemGroups;
    plan.targetItems = target.itemTargets;

    if (!source.ok)
    {
        plan.reason = source.reason.empty() ? "missing_payload" : "payload_" + source.reason;
        return plan;
    }
    if (!target.ok)
    {
        plan.reason = "missing_target";
        ++plan.unsupportedOps;
        return plan;
    }
    if (!importPlan.attempted)
    {
        plan.reason = "missing_import_plan";
        ++plan.unsupportedOps;
        return plan;
    }
    if (!rebuildPlan.attempted)
    {
        plan.reason = "missing_rebuild_plan";
        ++plan.unsupportedOps;
        return plan;
    }
    if (importPlan.missingTargets != 0 || importPlan.shapeMismatchOps != 0 || importPlan.unsupportedOps != 0)
    {
        plan.reason = "import_plan_blocked";
        plan.unsupportedOps =
            importPlan.missingTargets + importPlan.shapeMismatchOps + importPlan.unsupportedOps;
        return plan;
    }

    plan.patchOps.reserve(importPlan.ops.size() * 2);
    std::unordered_set<uint64_t> remappedItems;
    for (const CoopNativeFragmentImportPlanner::StoreRangeRewriteOp& rewriteOp : importPlan.ops)
    {
        if (rewriteOp.sourceRangeIndex >= source.ranges.size())
        {
            ++plan.unsupportedOps;
            continue;
        }

        const CoopNativeFragmentPayload::PayloadRangeRecord& range = source.ranges[rewriteOp.sourceRangeIndex];
        const CoopNativeFragmentImportPlanner::StoreRangeImportStep* step =
            FindBySourceRange(rebuildPlan.steps, rewriteOp.sourceRangeIndex);

        if (rewriteOp.materializerInputOnly)
        {
            ++plan.materializerInputOps;
            continue;
        }

        if (IsNodeRange(range.rangeKind))
        {
            const auto kind = rewriteOp.requiresInsert
                ? MaterializedStorePatchOp::Kind::InsertReadNode
                : MaterializedStorePatchOp::Kind::ReplaceExistingReadNode;
            MaterializedStorePatchOp nodeOp = MakeBaseOp(kind, range, rewriteOp, step);
            nodeOp.requiresAllocation = rewriteOp.requiresInsert;
            AddOp(plan, nodeOp);
        }
        else if (IsAttrRange(range.rangeKind))
        {
            const auto kind = rewriteOp.requiresInsert
                ? MaterializedStorePatchOp::Kind::InsertAttrRecord
                : MaterializedStorePatchOp::Kind::ReplaceAttrRecord;
            MaterializedStorePatchOp attrOp = MakeBaseOp(kind, range, rewriteOp, step);
            attrOp.requiresAllocation = rewriteOp.requiresInsert;
            AddOp(plan, attrOp);

            if (rewriteOp.requiresCursorRebase)
            {
                MaterializedStorePatchOp rebaseOp =
                    MakeBaseOp(MaterializedStorePatchOp::Kind::RebaseAttrCursor, range, rewriteOp, step);
                rebaseOp.requiresAllocation = false;
                AddOp(plan, rebaseOp);
            }
        }
        else if (IsChildRange(range.rangeKind))
        {
            const auto kind = rewriteOp.requiresInsert
                ? MaterializedStorePatchOp::Kind::InsertChildRecord
                : MaterializedStorePatchOp::Kind::ReplaceChildRecord;
            MaterializedStorePatchOp childOp = MakeBaseOp(kind, range, rewriteOp, step);
            childOp.requiresAllocation = rewriteOp.requiresInsert;
            AddOp(plan, childOp);

            if (rewriteOp.requiresCursorRebase)
            {
                MaterializedStorePatchOp rebaseOp =
                    MakeBaseOp(MaterializedStorePatchOp::Kind::RebaseChildCursor, range, rewriteOp, step);
                rebaseOp.requiresAllocation = false;
                AddOp(plan, rebaseOp);
            }
        }
        else
        {
            ++plan.unsupportedOps;
            continue;
        }

        const uint64_t itemRemapKey =
            (static_cast<uint64_t>(rewriteOp.sourceOrdinal) << 32) | static_cast<uint64_t>(range.entityId);
        const bool needsExistingItemRemap =
            IsItemRange(range) &&
            range.entityId != 0 &&
            rewriteOp.targetEntityId != 0 &&
            range.entityId != rewriteOp.targetEntityId;
        const bool needsInsertedItemId =
            IsItemRange(range) &&
            range.entityId != 0 &&
            rewriteOp.requiresInsert &&
            rewriteOp.targetEntityId == 0;
        if ((needsExistingItemRemap || needsInsertedItemId) && remappedItems.insert(itemRemapKey).second)
        {
            MaterializedStorePatchOp remapOp =
                MakeBaseOp(MaterializedStorePatchOp::Kind::RemapItemEntityId, range, rewriteOp, step);
            remapOp.sourceItemEntityId = range.entityId;
            remapOp.targetItemEntityId = rewriteOp.targetEntityId;
            remapOp.requiresAllocation = needsInsertedItemId;
            remapOp.safe = !rewriteOp.requiresInsert && rewriteOp.shapeCompatible;
            if (needsInsertedItemId)
                ++plan.entityAllocationOps;
            AddOp(plan, remapOp);
        }

        if (IsWriteRange(range.rangeKind))
        {
            if (HasOracleForRange(source, rewriteOp.sourceRangeIndex))
                ++plan.oracleMappedOps;
            else
            {
                ++plan.transcodeOps;
                ++plan.oracleMissingOps;
            }
        }
    }

    plan.ops = static_cast<uint32_t>(plan.patchOps.size());
    RefreshPlanReason(plan);

    return plan;
}

MaterializerPlan ApplyEntityIdAllocationPlan(
    const MaterializerPlan& plan,
    const CoopNativeEntityIdAllocator::AllocationPlan& allocationPlan)
{
    MaterializerPlan patched = plan;
    if (!allocationPlan.ok || allocationPlan.entries.empty() || patched.entityAllocationOps == 0)
        return patched;

    std::unordered_map<uint64_t, uint32_t> allocatedIds;
    allocatedIds.reserve(allocationPlan.entries.size());
    for (const CoopNativeEntityIdAllocator::AllocationEntry& entry : allocationPlan.entries)
    {
        const uint64_t key =
            (static_cast<uint64_t>(entry.sourceOrdinal) << 32) |
            static_cast<uint64_t>(entry.sourceEntityId);
        allocatedIds[key] = entry.targetEntityId;
    }

    uint32_t resolvedEntityAllocations = 0;
    for (MaterializedStorePatchOp& op : patched.patchOps)
    {
        if (op.kind != MaterializedStorePatchOp::Kind::RemapItemEntityId ||
            op.targetItemEntityId != 0 ||
            op.sourceItemEntityId == 0)
        {
            continue;
        }

        const uint64_t key =
            (static_cast<uint64_t>(op.sourceOrdinal) << 32) |
            static_cast<uint64_t>(op.sourceItemEntityId);
        const auto idIt = allocatedIds.find(key);
        if (idIt == allocatedIds.end() || idIt->second == 0)
            continue;

        op.targetItemEntityId = idIt->second;
        op.requiresAllocation = false;
        ++resolvedEntityAllocations;
    }

    if (resolvedEntityAllocations == 0)
        return patched;

    patched.entityAllocationOps =
        resolvedEntityAllocations > patched.entityAllocationOps
            ? 0
            : patched.entityAllocationOps - resolvedEntityAllocations;
    patched.allocationOps =
        resolvedEntityAllocations > patched.allocationOps
            ? 0
            : patched.allocationOps - resolvedEntityAllocations;
    patched.newItemInsertOps =
        resolvedEntityAllocations > patched.newItemInsertOps
            ? 0
            : patched.newItemInsertOps - resolvedEntityAllocations;
    RefreshPlanReason(patched);
    return patched;
}

std::string BuildMaterializerStatus(const MaterializerPlan& plan)
{
    std::ostringstream out;
    out << (plan.attempted ? 1 : 0)
        << "/" << (plan.ok ? "ok" : "blocked")
        << "/reason=" << StatusToken(plan.reason)
        << "/ops=" << plan.ops
        << "/safe=" << plan.safeOps
        << "/unsafe=" << plan.unsafeOps
        << "/unsupported=" << plan.unsupportedOps
        << "/input=" << plan.materializerInputOps
        << "/replaceNode=" << plan.replaceNodeOps
        << "/insertNode=" << plan.insertNodeOps
        << "/replaceAttr=" << plan.replaceAttrOps
        << "/insertAttr=" << plan.insertAttrOps
        << "/replaceChild=" << plan.replaceChildOps
        << "/insertChild=" << plan.insertChildOps
        << "/rebaseAttr=" << plan.rebaseAttrOps
        << "/rebaseChild=" << plan.rebaseChildOps
        << "/remapItem=" << plan.itemRemapOps
        << "/remapOwner=" << plan.ownerRemapOps
        << "/alloc=" << plan.allocationOps
        << "/transcode=" << plan.transcodeOps
        << "/oracle=" << plan.oracleMappedOps
        << "/oracleMissing=" << plan.oracleMissingOps
        << "/full=" << plan.fullTransplantOps
        << "/allocIds=" << plan.entityAllocationOps
        << "/missingSubtree=" << plan.missingSubtreeInsertOps
        << "/newItem=" << plan.newItemInsertOps
        << "/items=" << plan.sourceItems << "/" << plan.targetItems;
    return out.str();
}
}
