#include "CoopNativeSaveBridge.h"

#include <utility>

namespace CoopNativeSaveBridge
{
SerializerEventKind ClassifySerializerSource(std::string_view source)
{
    if (source.find("ArkInventory::FullSerialize(local-player)") != std::string_view::npos)
        return SerializerEventKind::LocalPlayerInventory;
    if (source.find("CArkItem::FullSerialize(player-inventory-item)") != std::string_view::npos)
        return SerializerEventKind::PlayerInventoryItem;

    return SerializerEventKind::Unknown;
}

bool IsTargetReadStoreAugmentPoint(std::string_view source)
{
    // The target read-store can only be augmented after Vanilla has actually
    // materialized item nodes. The inventory serializer gives us cell ids; the
    // item serializer is the native materialization point for CArkItem subtrees.
    return ClassifySerializerSource(source) == SerializerEventKind::PlayerInventoryItem;
}

bool TargetReadFragmentCapture::MatchesInventoryScope(uint64_t scopeSeq) const
{
    return m_active && m_activeInventoryScopeSeq == scopeSeq;
}

void TargetReadFragmentCapture::BeginInventoryScope(
    const NativeInventoryFragmentScopeInfo& info,
    const char* reason)
{
    m_locator.BeginRun(
        m_nextRunId++,
        reason && reason[0] ? reason : "target local-player inventory read");
    m_active = true;
    m_augmentAttempted = false;
    m_activeInventoryScopeSeq = info.scopeSeq;
    m_locator.OnLocalPlayerInventoryScopeEnter(info);
    m_lastEvent =
        "target inventory enter scope=" + std::to_string(info.scopeSeq) +
        " section=" + (info.sectionName.empty() ? std::string("-") : info.sectionName);
}

void TargetReadFragmentCapture::ExitInventoryScope(uint64_t scopeSeq)
{
    if (!MatchesInventoryScope(scopeSeq))
        return;

    m_locator.OnLocalPlayerInventoryScopeExit(scopeSeq);
    m_activeInventoryScopeSeq = 0;
}

void TargetReadFragmentCapture::EnterItemScope(const NativeItemFragmentScopeInfo& info)
{
    if (!m_active)
        return;

    m_locator.OnPlayerInventoryItemScopeEnter(info);
    m_lastEvent =
        "target item enter scope=" + std::to_string(info.scopeSeq) +
        " entity=" + std::to_string(info.itemEntityId);
}

void TargetReadFragmentCapture::ExitItemScope(uint64_t scopeSeq)
{
    if (!m_active)
        return;

    m_locator.OnPlayerInventoryItemScopeExit(scopeSeq);
}

void TargetReadFragmentCapture::RecordInventoryCellEntityId(
    unsigned entityId,
    const std::string& source,
    const std::string& path)
{
    if (!m_active)
        return;

    m_locator.OnInventoryCellEntityId(entityId, source, path);
    m_lastEvent = "target inventory cell entity=" + std::to_string(entityId);
}

void TargetReadFragmentCapture::RecordDecodedStoreNode(const NativeDecodedStoreNodeInfo& info)
{
    if (!m_active)
        return;

    m_locator.OnDecodedStoreNode(info);
    m_lastEvent = "target node " + m_locator.GetLastEvent();
}

bool TargetReadFragmentCapture::MarkAugmentAttempted()
{
    if (m_augmentAttempted)
        return false;

    m_augmentAttempted = true;
    return true;
}

void TargetReadFragmentCapture::MergeReadStoreBundle(
    const CoopNativeGameStateFragmentLocator::NativeInventoryFragmentBundle& bundle,
    const char* reason)
{
    if (!m_active)
        return;

    m_locator.MergeReadStoreBundle(bundle, reason && reason[0] ? reason : "read-store fragment augment");
    m_lastEvent = "target read-store augment " + m_locator.BuildFragmentBundleStatus();
}

void TargetReadFragmentCapture::Complete(const char* reason)
{
    if (!m_active)
        return;

    m_locator.FinalizeRun(reason && reason[0] ? reason : "target read fragment complete");
    m_active = false;
    m_augmentAttempted = false;
    m_activeInventoryScopeSeq = 0;
}

CoopNativeGameStateFragmentLocator::NativeInventoryFragmentBundle
TargetReadFragmentCapture::BuildBundle() const
{
    return m_locator.BuildFragmentBundle();
}

std::string TargetReadFragmentCapture::BuildBundleStatus() const
{
    return m_locator.BuildFragmentBundleStatus();
}

void TargetReadFragmentCapture::SetLastEvent(std::string event)
{
    m_lastEvent = event.empty() ? std::string("-") : std::move(event);
}
}
