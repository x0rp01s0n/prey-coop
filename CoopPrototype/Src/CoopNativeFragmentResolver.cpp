#include "CoopNativeFragmentResolver.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace
{
constexpr uint32_t kInvalidOrdinal = 0xFFFFFFFFu;

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

bool IsItemRange(const CoopNativeFragmentPayload::PayloadRangeRecord& range)
{
    return range.groupKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadGroupKind::Item);
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

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::vector<uint32_t> BuildSourceItemEntityOrder(const CoopNativeFragmentPayload::ParsedPayload& source)
{
    std::vector<uint32_t> order;
    for (const CoopNativeFragmentPayload::PayloadRangeRecord& range : source.ranges)
    {
        if (!IsItemRange(range) || range.entityId == 0)
            continue;

        if (std::find(order.begin(), order.end(), range.entityId) == order.end())
            order.push_back(range.entityId);
    }
    return order;
}

void AppendUniqueId(std::vector<uint32_t>& ids, uint32_t id)
{
    if (id == 0)
        return;
    if (std::find(ids.begin(), ids.end(), id) == ids.end())
        ids.push_back(id);
}

uint32_t CountDuplicateIds(const std::vector<uint32_t>& ids)
{
    std::unordered_set<uint32_t> seen;
    uint32_t duplicates = 0;
    for (uint32_t id : ids)
    {
        if (id == 0)
            continue;
        if (!seen.insert(id).second)
            ++duplicates;
    }
    return duplicates;
}

CoopNativeFragmentResolver::ReferenceSafetyAudit BuildReferenceSafetyAudit(
    const CoopNativeFragmentResolver::InventoryFragmentResolution& resolution,
    const std::vector<uint32_t>& sourceItems,
    const CoopNativeFragmentResolver::NativeResolverReadiness& readiness)
{
    CoopNativeFragmentResolver::ReferenceSafetyAudit audit;
    audit.attempted = true;
    audit.sourceItemIds = static_cast<uint32_t>(sourceItems.size());
    audit.sourceItemGroups = resolution.source.itemGroups;
    audit.targetItemIds = static_cast<uint32_t>(resolution.target.resolvedItemEntityIds.size());
    audit.targetItemTargets = resolution.target.itemTargets;
    audit.targetMissingRefs = resolution.target.missingRefs;
    audit.insertRequired = resolution.importPlan.requiresInsertOps;
    audit.allocatorRequired = resolution.materializerPlan.allocationOps + resolution.materializerPlan.entityAllocationOps;
    audit.remapRequired = resolution.entityRemaps;
    audit.transcodeRequired = resolution.materializerPlan.transcodeOps + resolution.materializerPlan.oracleMissingOps;
    audit.remapNativeAllocatorReady = readiness.nativeEntityAllocatorReady ? 1u : 0u;
    audit.unsupportedOps =
        resolution.importPlan.unsupportedOps +
        resolution.importPlan.missingTargets +
        resolution.importPlan.shapeMismatchOps +
        resolution.materializerPlan.unsupportedOps;

    if (audit.sourceItemGroups != audit.sourceItemIds)
        audit.sourceItemIdentityMismatch = 1;

    audit.targetDuplicateItemIds = CountDuplicateIds(resolution.target.resolvedItemEntityIds);
    std::unordered_set<uint32_t> targetKnownIds;
    for (uint32_t id : resolution.target.resolvedItemEntityIds)
    {
        if (id != 0)
            targetKnownIds.insert(id);
    }
    for (uint32_t id : resolution.target.knownItemEntityIds)
    {
        if (id != 0)
            targetKnownIds.insert(id);
    }

    std::unordered_map<uint32_t, uint32_t> sourceItemNodeRanges;
    for (const CoopNativeFragmentPayload::PayloadRangeRecord& range : resolution.source.ranges)
    {
        if (IsItemRange(range))
        {
            if (IsNodeRange(range.rangeKind))
            {
                ++audit.sourceItemNodeRanges;
                if (range.entityId != 0)
                    ++sourceItemNodeRanges[range.entityId];
            }
            else if (IsAttrRange(range.rangeKind))
                ++audit.sourceItemAttrRanges;
            else if (IsChildRange(range.rangeKind))
                ++audit.sourceItemChildRanges;
        }
        else if (IsInventoryRange(range))
        {
            if (IsNodeRange(range.rangeKind))
                ++audit.sourceInventoryNodeRanges;
            else if (IsAttrRange(range.rangeKind) || IsChildRange(range.rangeKind))
                ++audit.sourceInventoryBackingRanges;
        }
    }

    for (uint32_t id : sourceItems)
    {
        if (id != 0 && sourceItemNodeRanges.find(id) == sourceItemNodeRanges.end())
            ++audit.sourceItemsMissingNodes;
    }

    audit.sourceAttrNames = static_cast<uint32_t>(resolution.source.attrNames.size());
    for (const CoopNativeFragmentPayload::PayloadAttrNameRecord& attr : resolution.source.attrNames)
    {
        if (attr.type == 0)
            ++audit.sourceAttrTypeZero;

        const std::string lowerName = ToLowerAscii(attr.name);
        if (lowerName == "entityid" || lowerName == "entity_id")
            ++audit.sourceEntityIdAttrs;
        if (lowerName.find("owner") != std::string::npos)
            ++audit.sourceOwnerAttrs;
    }

    for (const CoopNativeFragmentResolver::ItemReference& ref : resolution.itemReferences)
    {
        switch (ref.remapMode)
        {
        case CoopNativeFragmentResolver::ItemEntityRemapMode::ReuseExistingTargetId:
            ++audit.remapReuseExisting;
            break;
        case CoopNativeFragmentResolver::ItemEntityRemapMode::RewriteExistingTargetId:
            ++audit.remapRewriteExisting;
            break;
        case CoopNativeFragmentResolver::ItemEntityRemapMode::AllocateNewTargetId:
            ++audit.remapAllocateNew;
            break;
        case CoopNativeFragmentResolver::ItemEntityRemapMode::MissingTarget:
            ++audit.remapMissingTarget;
            break;
        default:
            break;
        }

        if (!ref.hasSource || ref.sourceEntityId == 0)
            continue;
        if (ref.hasPlannedTarget &&
            !ref.hasTarget &&
            ref.sourceEntityId != 0 &&
            ref.targetEntityId == ref.sourceEntityId)
        {
            ++audit.targetMissingRefsCovered;
        }

        if (ref.requiresEntityAllocation &&
            targetKnownIds.find(ref.sourceEntityId) != targetKnownIds.end())
        {
            ++audit.itemIdCollisions;
        }
    }

    if (audit.targetMissingRefsCovered > audit.targetMissingRefs)
        audit.targetMissingRefs = 0;
    else
        audit.targetMissingRefs -= audit.targetMissingRefsCovered;

    audit.referenceSafe =
        resolution.source.ok &&
        resolution.target.ok &&
        audit.sourceItemIdentityMismatch == 0 &&
        audit.targetDuplicateItemIds == 0 &&
        audit.sourceItemsMissingNodes == 0 &&
        audit.targetMissingRefs == 0 &&
        resolution.importPlan.missingTargets == 0;
    audit.typeSafe =
        resolution.source.ok &&
        audit.unsupportedOps == 0 &&
        audit.transcodeRequired == 0;
    audit.applySafe =
        audit.referenceSafe &&
        audit.typeSafe &&
        resolution.materializerPlan.ok;

    if (!resolution.source.ok)
        audit.reason = resolution.source.reason.empty() ? "payload_not_ok" : "payload_" + resolution.source.reason;
    else if (!resolution.target.ok)
        audit.reason = "target_" + resolution.target.reason;
    else if (audit.sourceItemIdentityMismatch != 0)
        audit.reason = "source_item_identity_mismatch";
    else if (audit.targetDuplicateItemIds != 0)
        audit.reason = "target_duplicate_item_ids";
    else if (audit.sourceItemsMissingNodes != 0)
        audit.reason = "source_item_node_missing";
    else if (audit.targetMissingRefs != 0)
        audit.reason = "target_missing_item_refs";
    else if (resolution.importPlan.missingTargets != 0)
        audit.reason = "import_missing_targets";
    else if (audit.unsupportedOps != 0)
        audit.reason = "unsupported_or_shape_mismatch";
    else if (audit.transcodeRequired != 0)
        audit.reason = "write_to_read_transcode_required";
    else if (audit.itemIdCollisions != 0)
        audit.reason = "ok_allocator_required_id_collision";
    else if (audit.remapAllocateNew != 0 && audit.remapNativeAllocatorReady == 0)
        audit.reason = "ok_native_entity_allocator_required";
    else if (audit.allocatorRequired != 0 || audit.insertRequired != 0)
        audit.reason = "ok_allocator_required";
    else if (audit.remapRewriteExisting != 0)
        audit.reason = "ok_existing_entity_remap_required";
    else if (audit.remapRequired != 0)
        audit.reason = "ok_remap_required";
    else if (audit.applySafe)
        audit.reason = "ok_same_shape_apply_safe";
    else
        audit.reason = "ok_" + (resolution.materializerPlan.reason.empty()
            ? std::string("materializer_blocked")
            : resolution.materializerPlan.reason);

    audit.ok = audit.referenceSafe && audit.typeSafe;
    return audit;
}

CoopNativeFragmentResolver::StoreAnchor MakeTargetAnchor(
    CoopNativeFragmentResolver::FragmentKind kind,
    uint32_t entityId,
    uint32_t ordinal,
    const CoopNativeFragmentImportPlanner::TargetNodeShape& node)
{
    CoopNativeFragmentResolver::StoreAnchor anchor;
    anchor.fragmentKind = kind;
    anchor.entityId = entityId;
    anchor.ordinal = ordinal;
    anchor.nodeIndex = node.nodeIndex;
    anchor.nodeId = node.nodeId;
    anchor.attrCursor = node.attrCursor;
    anchor.attrCount = node.attrCount;
    anchor.childCursor = node.childCursor;
    anchor.childCount = node.childCount;
    anchor.readStore = node.readStore;
    anchor.valid = node.nodeIndex != 0 || node.nodeId != 0;
    return anchor;
}

CoopNativeFragmentResolver::StoreAnchor MakeEntityAnchor(
    CoopNativeFragmentResolver::FragmentKind kind,
    uint32_t entityId,
    uint32_t ordinal)
{
    CoopNativeFragmentResolver::StoreAnchor anchor;
    anchor.fragmentKind = kind;
    anchor.entityId = entityId;
    anchor.ordinal = ordinal;
    anchor.valid = entityId != 0;
    return anchor;
}

const CoopNativeFragmentImportPlanner::StoreRangeImportStep* FindPlannedInsertStep(
    const CoopNativeFragmentImportPlanner::NativeFragmentRebuildPlan& plan,
    uint32_t groupKind,
    uint32_t sourceOrdinal,
    uint32_t sourceEntityId)
{
    for (const CoopNativeFragmentImportPlanner::StoreRangeImportStep& step : plan.steps)
    {
        if (!step.insert || !step.insertNodeAllocated || step.plannedTargetNodeIndex == 0)
            continue;
        if (step.sourceGroupKind != groupKind ||
            step.sourceOrdinal != sourceOrdinal ||
            step.sourceEntityId != sourceEntityId)
        {
            continue;
        }
        return &step;
    }
    return nullptr;
}
}

namespace CoopNativeFragmentResolver
{
InventoryFragmentResolution ResolveInventoryFragment(
    const CoopNativeFragmentPayload::ParsedPayload& source,
    const CoopNativeGameStateFragmentLocator::NativeInventoryFragmentBundle& targetBundle,
    const NativeResolverReadiness& readiness)
{
    InventoryFragmentResolution resolution;
    resolution.attempted = true;
    resolution.source = source;
    resolution.sourceItems = source.itemGroups;
    resolution.target = CoopNativeFragmentImportPlanner::BuildTargetBundle(targetBundle);
    resolution.targetItems = resolution.target.itemTargets;
    resolution.importPlan =
        CoopNativeFragmentImportPlanner::BuildReadOnlyImportPlan(resolution.source, resolution.target);
    resolution.rebuildPlan =
        CoopNativeFragmentImportPlanner::BuildReadOnlyRebuildPlan(
            resolution.source,
            resolution.target,
            resolution.importPlan);
    resolution.materializerPlan =
        CoopNativeFragmentMaterializer::BuildReadOnlyMaterializerPlan(
            resolution.source,
            resolution.target,
            resolution.importPlan,
            resolution.rebuildPlan);

    const std::vector<uint32_t> sourceItems = BuildSourceItemEntityOrder(source);
    for (uint32_t ordinal = 0; ordinal < resolution.target.inventoryRootNodes.size(); ++ordinal)
    {
        const CoopNativeFragmentImportPlanner::TargetNodeShape& node = resolution.target.inventoryRootNodes[ordinal];
        resolution.referenceGraph.anchors.push_back(
            MakeTargetAnchor(FragmentKind::Inventory, node.entityId, ordinal, node));
    }
    for (uint32_t ordinal = 0; ordinal < resolution.target.itemRootNodes.size(); ++ordinal)
    {
        const CoopNativeFragmentImportPlanner::TargetNodeShape& node = resolution.target.itemRootNodes[ordinal];
        resolution.referenceGraph.anchors.push_back(
            MakeTargetAnchor(FragmentKind::Item, node.entityId, ordinal, node));
    }

    const uint32_t itemCount = std::max<uint32_t>(
        static_cast<uint32_t>(sourceItems.size()),
        static_cast<uint32_t>(resolution.target.resolvedItemEntityIds.size()));
    resolution.itemReferences.reserve(itemCount);
    for (uint32_t ordinal = 0; ordinal < itemCount; ++ordinal)
    {
        ItemReference ref;
        ref.sourceOrdinal = ordinal;
        ref.targetOrdinal = ordinal;
        if (ordinal < sourceItems.size())
        {
            ref.hasSource = true;
            ref.sourceEntityId = sourceItems[ordinal];
        }
        if (ordinal < resolution.target.resolvedItemEntityIds.size())
        {
            ref.hasTarget = true;
            ref.targetEntityId = resolution.target.resolvedItemEntityIds[ordinal];
        }
        if (ordinal < resolution.target.itemRootNodes.size())
        {
            const CoopNativeFragmentImportPlanner::TargetNodeShape& node = resolution.target.itemRootNodes[ordinal];
            ref.targetNodeIndex = node.nodeIndex;
            ref.targetAttrCursor = node.attrCursor;
            ref.targetAttrCount = node.attrCount;
            if (ref.targetEntityId == 0)
                ref.targetEntityId = node.entityId;
            ref.hasTarget = node.nodeIndex != 0 || node.nodeId != 0;
        }
        if (ref.hasSource && !ref.hasTarget)
        {
            const CoopNativeFragmentImportPlanner::StoreRangeImportStep* plannedStep =
                FindPlannedInsertStep(
                    resolution.rebuildPlan,
                    static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadGroupKind::Item),
                    ordinal,
                    ref.sourceEntityId);
            if (plannedStep)
            {
                ref.hasPlannedTarget = true;
                ref.plannedTargetNodeIndex = plannedStep->plannedTargetNodeIndex;
                ref.targetNodeIndex = plannedStep->plannedTargetNodeIndex;
                if (ref.targetEntityId == 0 &&
                    ref.sourceEntityId != 0 &&
                    (ContainsValue(resolution.target.inventoryEntityIds, ref.sourceEntityId) ||
                        ContainsValue(resolution.target.missingItemEntityIds, ref.sourceEntityId)))
                {
                    ref.targetEntityId = ref.sourceEntityId;
                }
            }
        }
        ref.requiresEntityRemap =
            ref.hasSource &&
            ref.hasTarget &&
            ref.sourceEntityId != 0 &&
            ref.targetEntityId != 0 &&
            ref.sourceEntityId != ref.targetEntityId;
        ref.requiresEntityAllocation =
            ref.hasSource &&
            ref.hasPlannedTarget &&
            !ref.hasTarget &&
            ref.sourceEntityId != 0 &&
            ref.targetEntityId == 0;
        if (ref.hasSource && ref.hasTarget)
        {
            ref.remapMode = ref.requiresEntityRemap
                ? ItemEntityRemapMode::RewriteExistingTargetId
                : ItemEntityRemapMode::ReuseExistingTargetId;
        }
        else if (ref.hasSource && ref.hasPlannedTarget && ref.targetEntityId == ref.sourceEntityId)
            ref.remapMode = ItemEntityRemapMode::ReuseExistingTargetId;
        else if (ref.hasSource && ref.hasPlannedTarget && ref.targetEntityId != 0)
            ref.remapMode = ItemEntityRemapMode::RewriteExistingTargetId;
        else if (ref.hasSource && ref.hasPlannedTarget)
            ref.remapMode = ItemEntityRemapMode::AllocateNewTargetId;
        else if (ref.hasSource)
            ref.remapMode = ItemEntityRemapMode::MissingTarget;

        if (ref.hasSource && ref.hasTarget)
            ++resolution.resolvedItems;
        else if (ref.hasSource && ref.hasPlannedTarget)
            ++resolution.plannedTargets;
        else if (ref.hasSource && !ref.hasTarget)
            ++resolution.missingTargets;
        if (ref.requiresEntityRemap)
            ++resolution.entityRemaps;
        if (ref.requiresEntityAllocation)
            ++resolution.entityAllocations;
        resolution.itemReferences.push_back(ref);

        StoreReferenceEdge edge;
        edge.kind = ReferenceKind::InventoryCellItemEntity;
        edge.from = MakeEntityAnchor(FragmentKind::Inventory, ref.sourceEntityId, ordinal);
        if (ordinal < resolution.target.inventoryRootNodes.size())
        {
            const CoopNativeFragmentImportPlanner::TargetNodeShape& inventoryNode =
                resolution.target.inventoryRootNodes[ordinal];
            edge.from.nodeIndex = inventoryNode.nodeIndex;
            edge.from.nodeId = inventoryNode.nodeId;
            edge.from.attrCursor = inventoryNode.attrCursor;
            edge.from.attrCount = inventoryNode.attrCount;
            edge.from.childCursor = inventoryNode.childCursor;
            edge.from.childCount = inventoryNode.childCount;
            edge.from.readStore = inventoryNode.readStore;
            edge.from.valid = true;
        }
        edge.to = MakeEntityAnchor(FragmentKind::Item, ref.targetEntityId, ordinal);
        if (ordinal < resolution.target.itemRootNodes.size())
        {
            const CoopNativeFragmentImportPlanner::TargetNodeShape& itemNode =
                resolution.target.itemRootNodes[ordinal];
            edge.to.nodeIndex = itemNode.nodeIndex;
            edge.to.nodeId = itemNode.nodeId;
            edge.to.attrCursor = itemNode.attrCursor;
            edge.to.attrCount = itemNode.attrCount;
            edge.to.childCursor = itemNode.childCursor;
            edge.to.childCount = itemNode.childCount;
            edge.to.readStore = itemNode.readStore;
            edge.to.valid = true;
        }
        else if (ref.hasPlannedTarget)
        {
            edge.to.nodeIndex = ref.plannedTargetNodeIndex;
            edge.to.valid = true;
        }
        edge.sourceValue = ref.sourceEntityId;
        edge.targetValue = ref.targetEntityId;
        edge.resolved = ref.hasSource && (ref.hasTarget || ref.hasPlannedTarget);
        edge.requiresRemap = ref.requiresEntityRemap || ref.requiresEntityAllocation;
        if (edge.resolved)
            ++resolution.referenceGraph.resolvedEdges;
        else
            ++resolution.referenceGraph.missingEdges;
        if (edge.requiresRemap)
            ++resolution.referenceGraph.remapEdges;
        resolution.referenceGraph.edges.push_back(edge);
    }

    std::vector<CoopNativeEntityIdAllocator::AllocationRequest> allocationRequests;
    std::vector<uint32_t> forbiddenIds;
    for (uint32_t id : sourceItems)
        AppendUniqueId(forbiddenIds, id);
    for (uint32_t id : resolution.target.inventoryEntityIds)
        AppendUniqueId(forbiddenIds, id);
    for (uint32_t id : resolution.target.resolvedItemEntityIds)
        AppendUniqueId(forbiddenIds, id);
    for (uint32_t id : resolution.target.missingItemEntityIds)
        AppendUniqueId(forbiddenIds, id);
    for (uint32_t id : resolution.target.knownItemEntityIds)
        AppendUniqueId(forbiddenIds, id);

    for (const ItemReference& ref : resolution.itemReferences)
    {
        if (ref.remapMode != ItemEntityRemapMode::AllocateNewTargetId)
            continue;

        CoopNativeEntityIdAllocator::AllocationRequest request;
        request.sourceOrdinal = ref.sourceOrdinal;
        request.sourceEntityId = ref.sourceEntityId;
        allocationRequests.push_back(request);
    }
    if (!allocationRequests.empty())
        resolution.entityIdAllocationPlan =
            CoopNativeEntityIdAllocator::BuildDryRunPlan(allocationRequests, forbiddenIds);
    if (resolution.entityIdAllocationPlan.ok)
    {
        resolution.materializerPlan =
            CoopNativeFragmentMaterializer::ApplyEntityIdAllocationPlan(
                resolution.materializerPlan,
                resolution.entityIdAllocationPlan);
    }

    resolution.safetyAudit = BuildReferenceSafetyAudit(resolution, sourceItems, readiness);

    if (!resolution.source.ok)
        resolution.reason = resolution.source.reason.empty() ? "missing_payload" : "payload_" + resolution.source.reason;
    else if (!resolution.target.ok)
        resolution.reason = "target_" + resolution.target.reason;
    else if (resolution.missingTargets != 0)
        resolution.reason = "missing_item_targets";
    else if (!resolution.materializerPlan.ok)
        resolution.reason = "materializer_" + resolution.materializerPlan.reason;
    else
    {
        resolution.ok = true;
        resolution.reason = "ok";
    }

    return resolution;
}

std::string BuildResolutionStatus(const InventoryFragmentResolution& resolution)
{
    std::ostringstream out;
    out << (resolution.attempted ? 1 : 0)
        << "/" << (resolution.ok ? "ok" : "blocked")
        << "/reason=" << StatusToken(resolution.reason)
        << "/sourceItems=" << resolution.sourceItems
        << "/targetItems=" << resolution.targetItems
        << "/resolved=" << resolution.resolvedItems
        << "/planned=" << resolution.plannedTargets
        << "/missing=" << resolution.missingTargets
        << "/remap=" << resolution.entityRemaps
        << "/allocIds=" << resolution.entityAllocations
        << "/refs=" << resolution.itemReferences.size()
        << "/anchors=" << resolution.referenceGraph.anchors.size()
        << "/edges=" << resolution.referenceGraph.edges.size()
        << "/" << resolution.referenceGraph.resolvedEdges
        << "/" << resolution.referenceGraph.missingEdges
        << "/" << resolution.referenceGraph.remapEdges
        << "/audit=" << (resolution.safetyAudit.ok ? 1 : 0)
        << ":" << StatusToken(resolution.safetyAudit.reason)
        << "/safe=" << (resolution.safetyAudit.referenceSafe ? 1 : 0)
        << (resolution.safetyAudit.typeSafe ? 1 : 0)
        << (resolution.safetyAudit.applySafe ? 1 : 0)
        << "/srcIds=" << resolution.safetyAudit.sourceItemIds
        << "/" << resolution.safetyAudit.sourceItemGroups
        << "/srcRanges=" << resolution.safetyAudit.sourceItemNodeRanges
        << "," << resolution.safetyAudit.sourceItemAttrRanges
        << "," << resolution.safetyAudit.sourceItemChildRanges
        << "/invRanges=" << resolution.safetyAudit.sourceInventoryNodeRanges
        << "," << resolution.safetyAudit.sourceInventoryBackingRanges
        << "/attrs=" << resolution.safetyAudit.sourceAttrNames
        << "," << resolution.safetyAudit.sourceAttrTypeZero
        << "," << resolution.safetyAudit.sourceEntityIdAttrs
        << "," << resolution.safetyAudit.sourceOwnerAttrs
        << "/tgtIds=" << resolution.safetyAudit.targetItemIds
        << "/" << resolution.safetyAudit.targetItemTargets
        << "/missCover=" << resolution.safetyAudit.targetMissingRefsCovered
        << "/dup=" << resolution.safetyAudit.targetDuplicateItemIds
        << "/collide=" << resolution.safetyAudit.itemIdCollisions
        << "/remapPlan=" << resolution.safetyAudit.remapReuseExisting
        << "," << resolution.safetyAudit.remapRewriteExisting
        << "," << resolution.safetyAudit.remapAllocateNew
        << "," << resolution.safetyAudit.remapMissingTarget
        << "," << resolution.safetyAudit.remapNativeAllocatorReady
        << "/need=" << resolution.safetyAudit.insertRequired
        << "," << resolution.safetyAudit.allocatorRequired
        << "," << resolution.safetyAudit.remapRequired
        << "," << resolution.safetyAudit.transcodeRequired
        << "," << resolution.safetyAudit.unsupportedOps
        << "/idPlan=" << CoopNativeEntityIdAllocator::BuildPlanStatus(resolution.entityIdAllocationPlan);
    if (!resolution.itemReferences.empty())
    {
        out << "/map=";
        const size_t limit = std::min<size_t>(resolution.itemReferences.size(), 6);
        for (size_t i = 0; i < limit; ++i)
        {
            const ItemReference& ref = resolution.itemReferences[i];
            if (i != 0)
                out << ",";
            out << ref.sourceOrdinal
                << ":" << ref.sourceEntityId
                << ">" << ref.targetEntityId
                << "@n" << ref.targetNodeIndex;
            if (ref.hasPlannedTarget)
                out << "p" << ref.plannedTargetNodeIndex;
            if (ref.requiresEntityRemap)
                out << "r";
            if (ref.requiresEntityAllocation)
                out << "a";
            switch (ref.remapMode)
            {
            case ItemEntityRemapMode::ReuseExistingTargetId:
                out << "R";
                break;
            case ItemEntityRemapMode::RewriteExistingTargetId:
                out << "W";
                break;
            case ItemEntityRemapMode::AllocateNewTargetId:
                out << "A";
                break;
            case ItemEntityRemapMode::MissingTarget:
                out << "M";
                break;
            default:
                break;
            }
        }
        if (resolution.itemReferences.size() > limit)
            out << ",more" << (resolution.itemReferences.size() - limit);
    }
    return out.str();
}
}
