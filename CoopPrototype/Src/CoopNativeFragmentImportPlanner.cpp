#include "CoopNativeFragmentImportPlanner.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace
{
constexpr uint32_t kInvalidSourceOrdinal = 0xFFFFFFFFu;
constexpr uint32_t kInventoryContainerSourceOrdinal = 0xFFFFFFFEu;

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

std::string Hex64(uint64_t value)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << value;
    return out.str();
}

template <typename T>
void AppendIdList(std::ostringstream& out, const char* label, const std::vector<T>& ids, size_t limit = 8)
{
    out << std::dec << "/" << label << "=";
    if (ids.empty())
    {
        out << "-";
        return;
    }

    const size_t count = std::min(ids.size(), limit);
    for (size_t i = 0; i < count; ++i)
    {
        if (i > 0)
            out << ",";
        out << ids[i];
    }
    if (ids.size() > count)
        out << ",more" << (ids.size() - count);
}

CoopNativeFragmentImportPlanner::TargetNodeShape MakeNodeShape(
    const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node,
    uint32_t groupKind = 0,
    uint32_t entityId = 0,
    uint32_t itemOrdinal = 0xFFFFFFFFu)
{
    CoopNativeFragmentImportPlanner::TargetNodeShape shape;
    shape.nodeIndex = node.nodeIndex;
    shape.nodeId = node.nodeId;
    shape.attrCursor = node.attrCursor;
    shape.attrCount = node.attrCount;
    shape.childCursor = node.childCursor;
    shape.childCount = node.childCount;
    shape.groupKind = groupKind;
    shape.entityId = entityId;
    shape.itemOrdinal = itemOrdinal;
    shape.readStore = node.readStore;
    return shape;
}

void AddTargetNode(
    CoopNativeFragmentImportPlanner::TargetInventoryFragmentBundle& target,
    const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node,
    uint32_t groupKind = 0,
    uint32_t entityId = 0,
    uint32_t itemOrdinal = 0xFFFFFFFFu)
{
    if (!node.valid)
        return;

    const auto alreadyKnown = std::any_of(
        target.nodes.begin(),
        target.nodes.end(),
        [&node](const CoopNativeFragmentImportPlanner::TargetNodeShape& existing)
        {
            return existing.nodeIndex == node.nodeIndex && existing.nodeId == node.nodeId;
        });
    if (alreadyKnown)
        return;

    if (node.readStore)
        ++target.readStoreNodes;
    else
        ++target.writeStoreNodes;
    target.targetStoreNodes = std::max(target.targetStoreNodes, node.nodeCount);
    if (node.attrCount != 0)
        ++target.observedAttrVectors;
    if (node.childCount != 0)
        ++target.observedChildVectors;

    target.nodes.push_back(MakeNodeShape(node, groupKind, entityId, itemOrdinal));
}

void AddTargetOrdinalNode(
    std::vector<CoopNativeFragmentImportPlanner::TargetNodeShape>& nodes,
    const CoopNativeFragmentImportPlanner::TargetNodeShape& shape)
{
    if (shape.nodeIndex == 0 && shape.nodeId == 0)
        return;

    const auto alreadyKnown = std::any_of(
        nodes.begin(),
        nodes.end(),
        [&shape](const CoopNativeFragmentImportPlanner::TargetNodeShape& existing)
        {
            return existing.nodeIndex == shape.nodeIndex && existing.nodeId == shape.nodeId;
        });
    if (!alreadyKnown)
        nodes.push_back(shape);
}

void SortTargetOrdinalNodes(std::vector<CoopNativeFragmentImportPlanner::TargetNodeShape>& nodes)
{
    std::sort(
        nodes.begin(),
        nodes.end(),
        [](const CoopNativeFragmentImportPlanner::TargetNodeShape& lhs,
            const CoopNativeFragmentImportPlanner::TargetNodeShape& rhs)
        {
            if (lhs.itemOrdinal != rhs.itemOrdinal)
                return lhs.itemOrdinal < rhs.itemOrdinal;
            if (lhs.nodeIndex != rhs.nodeIndex)
                return lhs.nodeIndex < rhs.nodeIndex;
            return lhs.nodeId < rhs.nodeId;
        });
}

bool IsNodeRange(uint32_t rangeKind);
bool IsAttrVectorRange(uint32_t rangeKind);
bool IsChildVectorRange(uint32_t rangeKind);

const CoopNativeFragmentImportPlanner::TargetNodeShape* FindTargetNode(
    const CoopNativeFragmentImportPlanner::TargetInventoryFragmentBundle& target,
    const CoopNativeFragmentPayload::PayloadRangeRecord& sourceRange,
    uint32_t sourceOrdinal)
{
    auto backingCountsMatch =
        [&sourceRange](const CoopNativeFragmentImportPlanner::TargetNodeShape& node) -> bool
        {
            if (IsAttrVectorRange(sourceRange.rangeKind))
                return node.attrCount == sourceRange.attrCount;
            if (IsChildVectorRange(sourceRange.rangeKind))
                return node.childCount == sourceRange.childCount;
            return true;
        };

    const bool itemRange =
        sourceRange.groupKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadGroupKind::Item);
    if (itemRange)
    {
        if (!IsNodeRange(sourceRange.rangeKind))
        {
            for (const CoopNativeFragmentImportPlanner::TargetNodeShape& node : target.nodes)
            {
                if (node.groupKind != sourceRange.groupKind)
                    continue;
                if (node.itemOrdinal != sourceOrdinal)
                    continue;
                if (backingCountsMatch(node))
                    return &node;
            }
        }

        if (sourceOrdinal >= target.itemRootNodes.size())
            return nullptr;

        const CoopNativeFragmentImportPlanner::TargetNodeShape& node =
            target.itemRootNodes[sourceOrdinal];
        return &node;
    }

    const bool inventoryRange =
        sourceRange.groupKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadGroupKind::Inventory);
    if (inventoryRange)
    {
        if (!IsNodeRange(sourceRange.rangeKind))
        {
            for (const CoopNativeFragmentImportPlanner::TargetNodeShape& node : target.nodes)
            {
                if (node.groupKind != sourceRange.groupKind)
                    continue;
                if (backingCountsMatch(node))
                    return &node;
            }
        }

        if (sourceOrdinal >= target.inventoryRootNodes.size())
            return nullptr;

        const CoopNativeFragmentImportPlanner::TargetNodeShape& node =
            target.inventoryRootNodes[sourceOrdinal];
        return &node;
    }

    for (const CoopNativeFragmentImportPlanner::TargetNodeShape& node : target.nodes)
    {
        if (node.groupKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadGroupKind::Item))
            continue;
        if (node.nodeId != sourceRange.firstNodeId)
            continue;
        if (sourceRange.attrCount != 0 && node.attrCount != sourceRange.attrCount)
            continue;
        if (sourceRange.childCount != 0 && node.childCount != sourceRange.childCount)
            continue;
        return &node;
    }
    return nullptr;
}

CoopNativeFragmentImportPlanner::StoreRangeRewriteOp::Kind RewriteKindForRange(uint32_t rangeKind)
{
    using PayloadRangeKind = CoopNativeFragmentPayload::PayloadRangeKind;
    using RewriteKind = CoopNativeFragmentImportPlanner::StoreRangeRewriteOp::Kind;

    if (rangeKind == static_cast<uint32_t>(PayloadRangeKind::AttrDataPool) ||
        rangeKind == static_cast<uint32_t>(PayloadRangeKind::WriteAttrVector))
    {
        return RewriteKind::ReplaceOrAppendAttrVector;
    }
    if (rangeKind == static_cast<uint32_t>(PayloadRangeKind::ChildIndexBlock) ||
        rangeKind == static_cast<uint32_t>(PayloadRangeKind::WriteChildVector))
    {
        return RewriteKind::ReplaceOrAppendChildVector;
    }
    return RewriteKind::ReplaceNodeBlockRange;
}

bool IsWriteStoreBackingRange(uint32_t rangeKind)
{
    return
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteAttrVector) ||
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteChildVector);
}

bool IsNodeRange(uint32_t rangeKind)
{
    return
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::NodeBlock) ||
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteNodeBlock);
}

bool IsWriteNodeRange(uint32_t rangeKind)
{
    return rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteNodeBlock);
}

bool IsBackingRange(uint32_t rangeKind)
{
    return
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::AttrDataPool) ||
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::ChildIndexBlock) ||
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteAttrVector) ||
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteChildVector);
}

bool IsAttrVectorRange(uint32_t rangeKind)
{
    return
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::AttrDataPool) ||
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteAttrVector);
}

bool IsChildVectorRange(uint32_t rangeKind)
{
    return
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::ChildIndexBlock) ||
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteChildVector);
}

bool IsWriteVectorRange(uint32_t rangeKind)
{
    return
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteAttrVector) ||
        rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteChildVector);
}

bool IsInventoryRange(const CoopNativeFragmentPayload::PayloadRangeRecord& range)
{
    return range.groupKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadGroupKind::Inventory);
}

template <typename T>
bool ContainsValue(const std::vector<T>& values, T value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool CanPreserveSourceItemEntityIdForInsert(
    const CoopNativeFragmentImportPlanner::TargetInventoryFragmentBundle& target,
    uint32_t sourceEntityId)
{
    if (sourceEntityId == 0)
        return false;

    // If the target inventory already references this id but the item subtree
    // was not materialized/found, the correct transplant is to insert the
    // missing subtree under that existing id. Allocating/remapping a new item id
    // would desync the already-read Inventory/storedItems cell.
    return
        ContainsValue(target.inventoryEntityIds, sourceEntityId) ||
        ContainsValue(target.missingItemEntityIds, sourceEntityId);
}

bool IsItemRange(const CoopNativeFragmentPayload::PayloadRangeRecord& range)
{
    return range.groupKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadGroupKind::Item);
}

uint32_t CountInventoryNodeRanges(const CoopNativeFragmentPayload::ParsedPayload& source)
{
    uint32_t count = 0;
    for (const CoopNativeFragmentPayload::PayloadRangeRecord& range : source.ranges)
    {
        if (IsInventoryRange(range) && IsNodeRange(range.rangeKind))
            ++count;
    }
    return count;
}

bool ShouldFoldSourceInventoryContainer(
    const CoopNativeFragmentPayload::ParsedPayload& source,
    const CoopNativeFragmentPayload::PayloadRangeRecord& range,
    bool alreadyFolded)
{
    if (alreadyFolded || !IsInventoryRange(range) || !IsNodeRange(range.rangeKind))
        return false;

    // Write-store inventory capture currently yields one storedItems container node
    // followed by one value-node per inventory cell. The target read-store matcher
    // only exposes those value nodes, so the container is materializer input, not
    // a missing target insert.
    const uint32_t inventoryNodeRanges = CountInventoryNodeRanges(source);
    return source.itemGroups != 0 && inventoryNodeRanges == source.itemGroups + 1;
}

bool IsMaterializerOnlySourceRange(
    const CoopNativeFragmentPayload::PayloadRangeRecord& range,
    uint32_t sourceOrdinal)
{
    return
        IsInventoryRange(range) &&
        (IsNodeRange(range.rangeKind) || IsBackingRange(range.rangeKind)) &&
        sourceOrdinal == kInventoryContainerSourceOrdinal;
}

std::unordered_map<uint32_t, uint32_t> BuildSourceItemOrdinals(
    const CoopNativeFragmentPayload::ParsedPayload& source)
{
    std::unordered_map<uint32_t, uint32_t> ordinals;
    for (const CoopNativeFragmentPayload::PayloadRangeRecord& range : source.ranges)
    {
        if (range.groupKind != static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadGroupKind::Item) ||
            range.entityId == 0)
        {
            continue;
        }

        if (ordinals.find(range.entityId) == ordinals.end())
            ordinals.emplace(range.entityId, static_cast<uint32_t>(ordinals.size()));
    }
    return ordinals;
}

std::vector<uint32_t> BuildSourceRangeOrdinals(
    const CoopNativeFragmentPayload::ParsedPayload& source,
    const std::unordered_map<uint32_t, uint32_t>& itemOrdinals)
{
    std::vector<uint32_t> ordinals(source.ranges.size(), kInvalidSourceOrdinal);
    std::unordered_map<uint32_t, uint32_t> inventoryOrdinalsByOwnerNode;
    uint32_t inventoryOrdinal = 0;
    bool foldedInventoryContainer = false;
    for (size_t i = 0; i < source.ranges.size(); ++i)
    {
        const CoopNativeFragmentPayload::PayloadRangeRecord& range = source.ranges[i];
        if (IsInventoryRange(range))
        {
            if (IsNodeRange(range.rangeKind))
            {
                if (ShouldFoldSourceInventoryContainer(source, range, foldedInventoryContainer))
                {
                    ordinals[i] = kInventoryContainerSourceOrdinal;
                    inventoryOrdinalsByOwnerNode.emplace(range.beginIndex, ordinals[i]);
                    foldedInventoryContainer = true;
                    continue;
                }

                ordinals[i] = inventoryOrdinal++;
                inventoryOrdinalsByOwnerNode.emplace(range.beginIndex, ordinals[i]);
                continue;
            }

            const auto ownerIt = inventoryOrdinalsByOwnerNode.find(range.beginIndex);
            if (ownerIt != inventoryOrdinalsByOwnerNode.end())
                ordinals[i] = ownerIt->second;
            continue;
        }
        if (IsItemRange(range))
        {
            const auto it = itemOrdinals.find(range.entityId);
            if (it != itemOrdinals.end())
                ordinals[i] = it->second;
        }
    }
    return ordinals;
}

uint64_t MakeImportOwnerKey(uint32_t groupKind, uint32_t sourceOrdinal, uint32_t entityId)
{
    return
        (static_cast<uint64_t>(groupKind) << 56) |
        (static_cast<uint64_t>(sourceOrdinal & 0x00FFFFFFu) << 32) |
        static_cast<uint64_t>(entityId);
}
}

namespace CoopNativeFragmentImportPlanner
{
TargetInventoryFragmentBundle BuildTargetBundle(
    const CoopNativeGameStateFragmentLocator::NativeInventoryFragmentBundle& bundle)
{
    TargetInventoryFragmentBundle target;
    target.reason = bundle.reason.empty() ? "missing_target" : bundle.reason;
    target.targetGameStateSerializerFingerprint = bundle.inventoryCellValueNode.storePtr;
    target.targetSchemaHash = bundle.schemaHash;
    target.targetContentHash = bundle.contentHash;
    target.inventoryNodes = bundle.observedInventoryNodeCount;
    target.inventoryRanges = bundle.observedInventoryRangeCount;
    target.itemTargets = static_cast<uint32_t>(bundle.itemFragments.size());
    target.inventoryEntityCount = bundle.inventoryEntityCount;
    target.missingRefs = bundle.missingItemReferences;
    target.inventoryEntityIds.assign(bundle.inventoryEntityIds.begin(), bundle.inventoryEntityIds.end());
    target.resolvedItemEntityIds.assign(bundle.resolvedItemEntityIds.begin(), bundle.resolvedItemEntityIds.end());
    target.missingItemEntityIds.assign(bundle.missingItemEntityIds.begin(), bundle.missingItemEntityIds.end());
    target.knownItemEntityIds.assign(bundle.knownItemEntityIds.begin(), bundle.knownItemEntityIds.end());

    AddTargetNode(
        target,
        bundle.inventoryCellValueNode,
        static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadGroupKind::Inventory));
    for (const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node : bundle.inventoryObservedNodes)
    {
        AddTargetOrdinalNode(
            target.inventoryRootNodes,
            MakeNodeShape(
                node,
                static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadGroupKind::Inventory)));
        AddTargetNode(
            target,
            node,
            static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadGroupKind::Inventory));
    }
    for (size_t itemIndex = 0; itemIndex < bundle.itemFragments.size(); ++itemIndex)
    {
        const CoopNativeGameStateFragmentLocator::ItemFragmentRef& item = bundle.itemFragments[itemIndex];
        const uint32_t itemOrdinal = static_cast<uint32_t>(itemIndex);
        AddTargetNode(
            target,
            item.node,
            static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadGroupKind::Item),
            item.entityId,
            itemOrdinal);
        if (item.node.valid)
        {
            AddTargetOrdinalNode(
                target.itemRootNodes,
                MakeNodeShape(
                    item.node,
                    static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadGroupKind::Item),
                    item.entityId,
                    itemOrdinal));
        }
        for (const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node : item.observedNodes)
        {
            AddTargetNode(
                target,
                node,
                static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadGroupKind::Item),
                item.entityId,
                itemOrdinal);
        }
    }
    SortTargetOrdinalNodes(target.inventoryRootNodes);
    SortTargetOrdinalNodes(target.itemRootNodes);
    target.itemTargets = static_cast<uint32_t>(target.itemRootNodes.size());

    const bool hasInventoryTarget = bundle.inventoryCellValueNode.valid && target.inventoryNodes != 0;
    if (!hasInventoryTarget)
        target.reason = "locator_" + target.reason;
    else if (target.nodes.empty() || target.readStoreNodes == 0)
        target.reason = "target_store_missing";
    else if (target.writeStoreNodes != 0)
        target.reason = "target_not_read_store";
    else
    {
        target.ok = true;
        target.reason = bundle.ok ? "ok" : "partial_" + target.reason;
    }

    return target;
}

NativeFragmentImportPlan BuildReadOnlyImportPlan(
    const CoopNativeFragmentPayload::ParsedPayload& source,
    const TargetInventoryFragmentBundle& target)
{
    NativeFragmentImportPlan plan;
    plan.attempted = true;
    plan.sourceSchemaHash = source.schemaHash;
    plan.targetSchemaHash = target.targetSchemaHash;

    if (!source.ok)
    {
        plan.reason = source.reason.empty() ? "missing_payload" : "payload_" + source.reason;
        return plan;
    }
    if (!target.ok)
    {
        plan.reason = "missing_target";
        plan.missingTargets = 1;
        return plan;
    }

    const bool schemaMatches = source.schemaHash == target.targetSchemaHash;
    const std::unordered_map<uint32_t, uint32_t> sourceItemOrdinals = BuildSourceItemOrdinals(source);
    const std::vector<uint32_t> sourceRangeOrdinals = BuildSourceRangeOrdinals(source, sourceItemOrdinals);

    plan.ops.reserve(source.ranges.size());
    for (size_t i = 0; i < source.ranges.size(); ++i)
    {
        const CoopNativeFragmentPayload::PayloadRangeRecord& range = source.ranges[i];
        const bool itemRange = IsItemRange(range);
        const bool inventoryRange = IsInventoryRange(range);
        const uint32_t sourceOrdinal = i < sourceRangeOrdinals.size()
            ? sourceRangeOrdinals[i]
            : kInvalidSourceOrdinal;
        const bool materializerInputOnly = IsMaterializerOnlySourceRange(range, sourceOrdinal);

        StoreRangeRewriteOp op;
        op.kind = RewriteKindForRange(range.rangeKind);
        op.sourceRangeIndex = static_cast<uint32_t>(i);
        op.sourceGroupKind = range.groupKind;
        op.sourceRangeKind = range.rangeKind;
        op.sourceEntityId = range.entityId;
        op.sourceOrdinal = sourceOrdinal;
        op.sourceOffset = range.byteOffset;
        op.byteSize = range.byteCount;
        op.materializerInputOnly = materializerInputOnly;

        if (materializerInputOnly)
        {
            ++plan.materializerInputOps;
            plan.ops.push_back(op);
            continue;
        }

        const TargetNodeShape* targetNode = FindTargetNode(target, range, sourceOrdinal);
        const bool missingItemInsertTarget = itemRange && sourceOrdinal >= target.itemRootNodes.size();
        const bool missingInventoryInsertTarget = inventoryRange && sourceOrdinal >= target.inventoryRootNodes.size();
        if (!targetNode)
        {
            if (missingItemInsertTarget || missingInventoryInsertTarget)
            {
                op.requiresInsert = true;
                if (missingItemInsertTarget &&
                    CanPreserveSourceItemEntityIdForInsert(target, range.entityId))
                {
                    op.targetGroupKind = range.groupKind;
                    op.targetEntityId = range.entityId;
                    op.targetItemOrdinal = sourceOrdinal;
                }
                ++plan.requiresInsertOps;
                if (IsNodeRange(range.rangeKind))
                    ++plan.insertNodeOps;
                else if (IsBackingRange(range.rangeKind))
                    ++plan.insertBackingOps;
            }
            else
            {
                ++plan.missingTargets;
            }
        }
        else
        {
            op.targetNodeIndex = targetNode->nodeIndex;
            op.targetGroupKind = targetNode->groupKind;
            op.targetEntityId = targetNode->entityId;
            op.targetItemOrdinal = targetNode->itemOrdinal;
            op.targetCursorBefore =
                op.kind == StoreRangeRewriteOp::Kind::ReplaceOrAppendChildVector
                    ? targetNode->childCursor
                    : targetNode->attrCursor;
            op.targetCursorAfter = op.targetCursorBefore;
            op.shapeCompatible =
                targetNode->groupKind == range.groupKind &&
                (!itemRange || targetNode->itemOrdinal == sourceOrdinal);
            if (op.shapeCompatible && IsNodeRange(range.rangeKind))
                ++plan.replaceExistingOps;
            else if (!op.shapeCompatible)
                ++plan.shapeMismatchOps;
        }

        if (IsWriteStoreBackingRange(range.rangeKind))
        {
            ++plan.backingVectorOps;
            ++plan.rebaseOps;
            op.requiresCursorRebase = true;
            if (missingItemInsertTarget)
                op.requiresInsert = true;
        }
        else
        {
            op.supported =
                targetNode != nullptr &&
                op.shapeCompatible &&
                !op.requiresInsert &&
                !op.requiresCursorRebase;
        }

        plan.ops.push_back(op);
    }

    plan.rewriteOps = static_cast<uint32_t>(plan.ops.size());
    if (plan.missingTargets != 0)
        plan.reason = "missing_target";
    else if (plan.shapeMismatchOps != 0)
    {
        plan.unsupportedOps += plan.shapeMismatchOps;
        plan.reason = "shape_mismatch";
    }
    else if (plan.requiresInsertOps != 0)
        plan.reason = plan.rebaseOps != 0 ? "insert_rebase_required" : "insert_required";
    else if (plan.rebaseOps != 0)
        plan.reason = "cursor_rebase_required";
    else if (plan.unsupportedOps != 0)
        plan.reason = "unsupported_ops";
    else if (!schemaMatches)
        plan.reason = "schema_mismatch";
    else
    {
        plan.ok = true;
        plan.reason = "ok";
    }

    return plan;
}

NativeFragmentRebuildPlan BuildReadOnlyRebuildPlan(
    const CoopNativeFragmentPayload::ParsedPayload& source,
    const TargetInventoryFragmentBundle& target,
    const NativeFragmentImportPlan& importPlan)
{
    NativeFragmentRebuildPlan plan;
    plan.attempted = true;
    plan.targetExistingNodes = target.targetStoreNodes;
    plan.targetPlannedNodeBase = target.targetStoreNodes;
    plan.targetPlannedNodeEnd = target.targetStoreNodes;

    if (!source.ok)
    {
        plan.reason = source.reason.empty() ? "missing_payload" : "payload_" + source.reason;
        return plan;
    }
    if (!target.ok)
    {
        plan.reason = "missing_target";
        ++plan.blockedOps;
        return plan;
    }
    if (!importPlan.attempted)
    {
        plan.reason = "missing_import_plan";
        ++plan.blockedOps;
        return plan;
    }
    if (importPlan.missingTargets != 0 || importPlan.shapeMismatchOps != 0 || importPlan.unsupportedOps != 0)
    {
        plan.reason = "import_plan_blocked";
        plan.blockedOps =
            importPlan.missingTargets + importPlan.shapeMismatchOps + importPlan.unsupportedOps;
        return plan;
    }

    std::unordered_map<uint64_t, uint32_t> plannedInsertNodesByOwner;
    uint32_t nextPlannedNodeIndex = target.targetStoreNodes;
    for (const StoreRangeRewriteOp& op : importPlan.ops)
    {
        if (!op.requiresInsert || !IsNodeRange(op.sourceRangeKind))
            continue;

        const uint64_t ownerKey =
            MakeImportOwnerKey(op.sourceGroupKind, op.sourceOrdinal, op.sourceEntityId);
        if (plannedInsertNodesByOwner.find(ownerKey) != plannedInsertNodesByOwner.end())
            continue;

        plannedInsertNodesByOwner.emplace(ownerKey, nextPlannedNodeIndex++);
    }

    plan.steps.reserve(importPlan.ops.size());
    for (const StoreRangeRewriteOp& op : importPlan.ops)
    {
        StoreRangeImportStep step;
        step.sourceRangeIndex = op.sourceRangeIndex;
        step.sourceGroupKind = op.sourceGroupKind;
        step.sourceRangeKind = op.sourceRangeKind;
        step.sourceEntityId = op.sourceEntityId;
        step.sourceOrdinal = op.sourceOrdinal;
        step.targetNodeIndex = op.targetNodeIndex;
        step.plannedTargetNodeIndex = op.targetNodeIndex;
        step.byteSize = op.byteSize;
        step.targetExists = !op.requiresInsert && !op.materializerInputOnly;
        step.insert = op.requiresInsert;
        step.cursorRebase = op.requiresCursorRebase;
        step.attrVector = IsAttrVectorRange(op.sourceRangeKind);
        step.childVector = IsChildVectorRange(op.sourceRangeKind);
        step.nodeRecordMaterialization = IsWriteNodeRange(op.sourceRangeKind);
        step.readPoolAllocation = step.attrVector || step.childVector;
        step.writeVectorTranscode = IsWriteVectorRange(op.sourceRangeKind);
        step.materializerInputOnly = op.materializerInputOnly;

        const uint64_t ownerKey =
            MakeImportOwnerKey(op.sourceGroupKind, op.sourceOrdinal, op.sourceEntityId);
        const auto plannedNodeIt = plannedInsertNodesByOwner.find(ownerKey);
        if (plannedNodeIt != plannedInsertNodesByOwner.end())
        {
            step.plannedTargetNodeIndex = plannedNodeIt->second;
            step.insertNodeAllocated = true;
        }
        else if (op.requiresInsert && !IsNodeRange(op.sourceRangeKind))
        {
            ++plan.unresolvedInsertBackings;
        }

        if (op.sourceRangeIndex < source.ranges.size())
        {
            const CoopNativeFragmentPayload::PayloadRangeRecord& range = source.ranges[op.sourceRangeIndex];
            step.sourceCursor = step.childVector ? range.childCursor : range.attrCursor;
            step.sourceCount = step.childVector ? range.childCount : range.attrCount;
        }

        plan.rawBytes += op.byteSize;
        if (IsNodeRange(op.sourceRangeKind))
        {
            if (op.materializerInputOnly)
            {
                // Source-only write-store wrapper; materializer consumes it but
                // must not allocate a target node for same-shape replacement.
            }
            else if (op.requiresInsert)
                ++plan.insertNodeOps;
            else
                ++plan.replaceNodeOps;
            if (step.nodeRecordMaterialization)
                ++plan.nodeRecordMaterializations;
        }
        if (step.attrVector)
        {
            ++plan.attrVectorImports;
            ++plan.readPoolAllocations;
        }
        if (step.childVector)
        {
            ++plan.childVectorImports;
            ++plan.readPoolAllocations;
        }
        if (step.writeVectorTranscode)
            ++plan.writeVectorTranscodes;
        if (op.requiresCursorRebase)
            ++plan.cursorRemaps;

        plan.steps.push_back(step);
    }

    plan.assignedInsertedNodes = static_cast<uint32_t>(plannedInsertNodesByOwner.size());
    plan.targetPlannedNodeEnd = nextPlannedNodeIndex;
    plan.allocatorRequired =
        plan.insertNodeOps + plan.attrVectorImports + plan.childVectorImports + plan.cursorRemaps;
    plan.materializerRequired =
        plan.nodeRecordMaterializations + plan.writeVectorTranscodes;
    if (plan.unresolvedInsertBackings != 0)
    {
        plan.reason = "insert_owner_unresolved";
        plan.blockedOps = plan.unresolvedInsertBackings;
        return plan;
    }
    if (plan.materializerRequired != 0 && plan.allocatorRequired != 0)
    {
        plan.reason = "full_transplant_materializer_allocator_required";
        plan.blockedOps = plan.materializerRequired + plan.allocatorRequired;
        return plan;
    }
    if (plan.materializerRequired != 0)
    {
        plan.reason = "materializer_required";
        plan.blockedOps = plan.materializerRequired;
        return plan;
    }
    if (plan.allocatorRequired != 0)
    {
        plan.reason = "allocator_required";
        plan.blockedOps = plan.allocatorRequired;
        return plan;
    }

    plan.ok = true;
    plan.reason = "ok";
    return plan;
}

std::string BuildTargetStatus(const TargetInventoryFragmentBundle& target)
{
    std::ostringstream out;
    out << (target.ok ? 1 : 0)
        << "/" << StatusToken(target.reason)
        << "/schema=" << Hex64(target.targetSchemaHash)
        << "/content=" << Hex64(target.targetContentHash)
        << "/invNodes=" << target.inventoryNodes
        << "/items=" << target.itemTargets << "/" << target.inventoryEntityCount
        << "/attrs=" << target.observedAttrVectors
        << "/childs=" << target.observedChildVectors
        << "/read=" << target.readStoreNodes
        << "/write=" << target.writeStoreNodes
        << "/missing=" << target.missingRefs
        << "/storeNodes=" << target.targetStoreNodes;
    AppendIdList(out, "cellIds", target.inventoryEntityIds);
    AppendIdList(out, "resolvedIds", target.resolvedItemEntityIds);
    AppendIdList(out, "missingIds", target.missingItemEntityIds);
    AppendIdList(out, "knownIds", target.knownItemEntityIds);
    return out.str();
}

std::string BuildImportPlanStatus(const NativeFragmentImportPlan& plan)
{
    std::ostringstream out;
    out << (plan.attempted ? 1 : 0)
        << "/" << (plan.ok ? "ok" : "blocked")
        << "/reason=" << StatusToken(plan.reason)
        << "/source=" << Hex64(plan.sourceSchemaHash)
        << "/target=" << Hex64(plan.targetSchemaHash)
        << "/ops=" << plan.rewriteOps
        << "/unsupported=" << plan.unsupportedOps
        << "/missing=" << plan.missingTargets
        << "/insert=" << plan.requiresInsertOps
        << "/rebase=" << plan.rebaseOps
        << "/replace=" << plan.replaceExistingOps
        << "/insertNode=" << plan.insertNodeOps
        << "/insertBacking=" << plan.insertBackingOps
        << "/backing=" << plan.backingVectorOps
        << "/shapeMismatch=" << plan.shapeMismatchOps
        << "/materializerInput=" << plan.materializerInputOps;
    return out.str();
}

std::string BuildRebuildPlanStatus(const NativeFragmentRebuildPlan& plan)
{
    std::ostringstream out;
    out << (plan.attempted ? 1 : 0)
        << "/" << (plan.ok ? "ok" : "blocked")
        << "/reason=" << StatusToken(plan.reason)
        << "/replaceNode=" << plan.replaceNodeOps
        << "/insertNode=" << plan.insertNodeOps
        << "/attrImport=" << plan.attrVectorImports
        << "/childImport=" << plan.childVectorImports
        << "/remap=" << plan.cursorRemaps
        << "/raw=" << plan.rawBytes
        << "/targetNodes=" << plan.targetExistingNodes
        << "/plannedNodes=" << plan.targetPlannedNodeBase << "-" << plan.targetPlannedNodeEnd
        << "/assignedNodes=" << plan.assignedInsertedNodes
        << "/unresolvedBacking=" << plan.unresolvedInsertBackings
        << "/nodeMaterialize=" << plan.nodeRecordMaterializations
        << "/readPoolAlloc=" << plan.readPoolAllocations
        << "/writeTranscode=" << plan.writeVectorTranscodes
        << "/materializer=" << plan.materializerRequired
        << "/allocator=" << plan.allocatorRequired
        << "/blocked=" << plan.blockedOps
        << "/steps=" << plan.steps.size();
    return out.str();
}

std::string BuildPatchStatus(
    const NativeFragmentImportPlan& plan,
    bool mutationEnabled,
    bool selfReplaceEnabled)
{
    if (!mutationEnabled)
        return "disabled";
    if (!plan.ok)
        return "blocked:" + StatusToken(plan.reason.empty() ? std::string("import_plan_not_ready") : plan.reason);
    if (selfReplaceEnabled)
        return "selfreplace_ok";
    return "blocked:cross_store_patch_disabled";
}
}
