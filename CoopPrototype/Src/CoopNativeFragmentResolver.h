#pragma once

#include "CoopNativeFragmentImportPlanner.h"
#include "CoopNativeEntityIdAllocator.h"
#include "CoopNativeFragmentMaterializer.h"
#include "CoopNativeFragmentPayload.h"
#include "CoopNativeGameStateFragmentLocator.h"

#include <cstdint>
#include <string>
#include <vector>

namespace CoopNativeFragmentResolver
{
enum class FragmentKind : uint32_t
{
    Unknown = 0,
    Inventory = 1,
    Item = 2,
    Player = 3,
    AbilityState = 4,
    ChipsetState = 5,
};

enum class ReferenceKind : uint32_t
{
    Unknown = 0,
    InventoryCellItemEntity = 1,
    OwnerEntity = 2,
    ChildNode = 3,
    AttrCursor = 4,
    ChildCursor = 5,
};

enum class ItemEntityRemapMode : uint32_t
{
    None = 0,
    ReuseExistingTargetId = 1,
    RewriteExistingTargetId = 2,
    AllocateNewTargetId = 3,
    MissingTarget = 4,
};

struct StoreAnchor
{
    FragmentKind fragmentKind = FragmentKind::Unknown;
    uint32_t entityId = 0;
    uint32_t ordinal = 0xFFFFFFFFu;
    uint32_t nodeIndex = 0;
    uint32_t nodeId = 0;
    uint32_t attrCursor = 0;
    uint32_t attrCount = 0;
    uint32_t childCursor = 0;
    uint32_t childCount = 0;
    bool readStore = false;
    bool valid = false;
};

struct StoreReferenceEdge
{
    ReferenceKind kind = ReferenceKind::Unknown;
    StoreAnchor from;
    StoreAnchor to;
    uint32_t sourceValue = 0;
    uint32_t targetValue = 0;
    bool resolved = false;
    bool requiresRemap = false;
};

struct StoreReferenceGraph
{
    std::vector<StoreAnchor> anchors;
    std::vector<StoreReferenceEdge> edges;
    uint32_t resolvedEdges = 0;
    uint32_t missingEdges = 0;
    uint32_t remapEdges = 0;
};

struct ReferenceSafetyAudit
{
    bool attempted = false;
    bool ok = false;
    bool referenceSafe = false;
    bool typeSafe = false;
    bool applySafe = false;
    std::string reason;
    uint32_t sourceItemIds = 0;
    uint32_t sourceItemGroups = 0;
    uint32_t sourceItemIdentityMismatch = 0;
    uint32_t sourceItemNodeRanges = 0;
    uint32_t sourceItemAttrRanges = 0;
    uint32_t sourceItemChildRanges = 0;
    uint32_t sourceItemsMissingNodes = 0;
    uint32_t sourceInventoryNodeRanges = 0;
    uint32_t sourceInventoryBackingRanges = 0;
    uint32_t sourceAttrNames = 0;
    uint32_t sourceAttrTypeZero = 0;
    uint32_t sourceEntityIdAttrs = 0;
    uint32_t sourceOwnerAttrs = 0;
    uint32_t targetItemIds = 0;
    uint32_t targetItemTargets = 0;
    uint32_t targetDuplicateItemIds = 0;
    uint32_t targetMissingRefs = 0;
    uint32_t targetMissingRefsCovered = 0;
    uint32_t itemIdCollisions = 0;
    uint32_t remapReuseExisting = 0;
    uint32_t remapRewriteExisting = 0;
    uint32_t remapAllocateNew = 0;
    uint32_t remapMissingTarget = 0;
    uint32_t remapNativeAllocatorReady = 0;
    uint32_t insertRequired = 0;
    uint32_t allocatorRequired = 0;
    uint32_t remapRequired = 0;
    uint32_t transcodeRequired = 0;
    uint32_t unsupportedOps = 0;
};

struct NativeResolverReadiness
{
    bool nativeEntityAllocatorReady = false;
};

struct ItemReference
{
    uint32_t sourceOrdinal = 0xFFFFFFFFu;
    uint32_t targetOrdinal = 0xFFFFFFFFu;
    uint32_t sourceEntityId = 0;
    uint32_t targetEntityId = 0;
    uint32_t targetNodeIndex = 0;
    uint32_t plannedTargetNodeIndex = 0;
    uint32_t targetAttrCursor = 0;
    uint32_t targetAttrCount = 0;
    bool hasSource = false;
    bool hasTarget = false;
    bool hasPlannedTarget = false;
    bool requiresEntityAllocation = false;
    bool requiresEntityRemap = false;
    ItemEntityRemapMode remapMode = ItemEntityRemapMode::None;
};

struct InventoryFragmentResolution
{
    bool attempted = false;
    bool ok = false;
    std::string reason;
    CoopNativeFragmentPayload::ParsedPayload source;
    CoopNativeFragmentImportPlanner::TargetInventoryFragmentBundle target;
    CoopNativeFragmentImportPlanner::NativeFragmentImportPlan importPlan;
    CoopNativeFragmentImportPlanner::NativeFragmentRebuildPlan rebuildPlan;
    CoopNativeFragmentMaterializer::MaterializerPlan materializerPlan;
    CoopNativeEntityIdAllocator::AllocationPlan entityIdAllocationPlan;
    StoreReferenceGraph referenceGraph;
    ReferenceSafetyAudit safetyAudit;
    std::vector<ItemReference> itemReferences;
    uint32_t sourceItems = 0;
    uint32_t targetItems = 0;
    uint32_t resolvedItems = 0;
    uint32_t missingTargets = 0;
    uint32_t plannedTargets = 0;
    uint32_t entityRemaps = 0;
    uint32_t entityAllocations = 0;
};

InventoryFragmentResolution ResolveInventoryFragment(
    const CoopNativeFragmentPayload::ParsedPayload& source,
    const CoopNativeGameStateFragmentLocator::NativeInventoryFragmentBundle& targetBundle,
    const NativeResolverReadiness& readiness = {});

std::string BuildResolutionStatus(const InventoryFragmentResolution& resolution);
}
