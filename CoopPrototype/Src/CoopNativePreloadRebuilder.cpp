#include "CoopNativePreloadRebuilder.h"

#include <sstream>

namespace
{
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
}

namespace CoopNativePreloadRebuilder
{
PreloadRebuildPlan BuildPreloadRebuildPlan(
    const CoopNativeFragmentPayload::ParsedPayload& source,
    const CoopNativeFragmentResolver::InventoryFragmentResolution& resolution)
{
    PreloadRebuildPlan plan;
    plan.attempted = true;
    plan.sourceItems = source.itemGroups;
    plan.targetItems = resolution.targetItems;

    const CoopNativeFragmentMaterializer::MaterializerPlan& materializer = resolution.materializerPlan;
    plan.materializerOps = materializer.ops;
    plan.fullTransplantOps = materializer.fullTransplantOps;
    plan.insertNodeOps = materializer.insertNodeOps;
    plan.insertAttrOps = materializer.insertAttrOps;
    plan.insertChildOps = materializer.insertChildOps;
    plan.replaceNodeOps = materializer.replaceNodeOps;
    plan.replaceAttrOps = materializer.replaceAttrOps;
    plan.replaceChildOps = materializer.replaceChildOps;
    plan.rebaseAttrOps = materializer.rebaseAttrOps;
    plan.rebaseChildOps = materializer.rebaseChildOps;
    plan.itemRemapOps = materializer.itemRemapOps;
    plan.entityIdAllocations = resolution.entityAllocations;
    plan.entityIdAllocationPlanReady = resolution.entityIdAllocationPlan.ok ? 1u : 0u;
    plan.unsupportedOps = materializer.unsupportedOps;
    plan.oracleMissingOps = materializer.oracleMissingOps;

    // These are only blockers for direct ReadStore mutation. The preload route should
    // let the engine build the write store and later materialize the read store.
    plan.readStoreOnlyAllocatorOps =
        materializer.allocationOps +
        materializer.insertNodeOps +
        materializer.insertAttrOps +
        materializer.insertChildOps;
    plan.writeStoreMaterializeOps =
        materializer.insertNodeOps +
        materializer.insertAttrOps +
        materializer.insertChildOps +
        materializer.rebaseAttrOps +
        materializer.rebaseChildOps +
        materializer.transcodeOps;

    plan.writeStoreBackend = CoopNativeSaveStoreApi::CheckWriteStoreBuilderBackend();
    plan.nativeWriteStoreBackendReady = plan.writeStoreBackend.ok ? 1u : 0u;

    if (!source.ok)
    {
        plan.reason = source.reason.empty() ? "payload_not_ok" : "payload_" + source.reason;
        return plan;
    }
    if (!resolution.target.ok)
    {
        plan.reason = resolution.target.reason.empty() ? "target_not_ok" : "target_" + resolution.target.reason;
        return plan;
    }
    if (resolution.importPlan.missingTargets != 0 ||
        resolution.importPlan.unsupportedOps != 0 ||
        resolution.importPlan.shapeMismatchOps != 0)
    {
        plan.reason = resolution.importPlan.reason.empty() ? "import_plan_not_ok" : "import_" + resolution.importPlan.reason;
        return plan;
    }
    if (plan.unsupportedOps != 0)
    {
        plan.reason = "unsupported_materializer_ops";
        return plan;
    }
    if (plan.oracleMissingOps != 0)
    {
        plan.reason = "write_node_oracle_missing";
        return plan;
    }
    if (materializer.entityAllocationOps != 0)
    {
        plan.reason = "entity_id_allocation_unresolved";
        return plan;
    }
    if (resolution.entityAllocations != 0 && !resolution.entityIdAllocationPlan.ok)
    {
        plan.reason = "entity_id_allocation_plan_missing";
        return plan;
    }
    if (!plan.writeStoreBackend.ok)
    {
        plan.reason = "native_write_store_backend_" +
            (plan.writeStoreBackend.reason.empty() ? std::string("blocked") : plan.writeStoreBackend.reason);
        return plan;
    }

    // The final missing layer is the native read-section -> write-section
    // GameState copier. Keep this explicit so we do not accidentally fall back
    // to ReadStore mutation or Give/Clear inventory hacks.
    plan.reason = "native_read_to_write_gamestate_copier_required";
    plan.ok = false;
    return plan;
}

std::string BuildStatus(const PreloadRebuildPlan& plan)
{
    std::ostringstream out;
    out << (plan.attempted ? 1 : 0)
        << "/" << (plan.ok ? "ok" : "blocked")
        << "/reason=" << StatusToken(plan.reason)
        << "/items=" << plan.sourceItems << "/" << plan.targetItems
        << "/ops=" << plan.materializerOps
        << "/full=" << plan.fullTransplantOps
        << "/replace=" << plan.replaceNodeOps << "," << plan.replaceAttrOps << "," << plan.replaceChildOps
        << "/insert=" << plan.insertNodeOps << "," << plan.insertAttrOps << "," << plan.insertChildOps
        << "/rebase=" << plan.rebaseAttrOps << "," << plan.rebaseChildOps
        << "/remap=" << plan.itemRemapOps
        << "/idAlloc=" << plan.entityIdAllocations << "/" << plan.entityIdAllocationPlanReady
        << "/writeMat=" << plan.writeStoreMaterializeOps
        << "/readStoreOnlyAlloc=" << plan.readStoreOnlyAllocatorOps
        << "/unsupported=" << plan.unsupportedOps
        << "/oracleMissing=" << plan.oracleMissingOps
        << "/nativeWrite=" << plan.nativeWriteStoreBackendReady
        << "/sectionRewrite=" << plan.savePackageSectionRewriteReady
        << "/backend=" << StatusToken(CoopNativeSaveStoreApi::BuildWriteStoreBuilderBackendStatus(plan.writeStoreBackend));
    return out.str();
}
}
