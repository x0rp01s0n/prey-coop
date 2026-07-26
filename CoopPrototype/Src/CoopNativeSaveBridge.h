#pragma once

#include "CoopNativeGameStateFragmentLocator.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace CoopNativeSaveBridge
{
enum class SerializerEventKind : uint32_t
{
    Unknown = 0,
    LocalPlayerInventory = 1,
    PlayerInventoryItem = 2,
};

SerializerEventKind ClassifySerializerSource(std::string_view source);
bool IsTargetReadStoreAugmentPoint(std::string_view source);

class TargetReadFragmentCapture final
{
public:
    bool IsActive() const { return m_active; }
    bool HasAttemptedAugment() const { return m_augmentAttempted; }
    bool MatchesInventoryScope(uint64_t scopeSeq) const;

    void BeginInventoryScope(const NativeInventoryFragmentScopeInfo& info, const char* reason);
    void ExitInventoryScope(uint64_t scopeSeq);
    void EnterItemScope(const NativeItemFragmentScopeInfo& info);
    void ExitItemScope(uint64_t scopeSeq);
    void RecordInventoryCellEntityId(unsigned entityId, const std::string& source, const std::string& path);
    void RecordDecodedStoreNode(const NativeDecodedStoreNodeInfo& info);
    bool MarkAugmentAttempted();
    void MergeReadStoreBundle(
        const CoopNativeGameStateFragmentLocator::NativeInventoryFragmentBundle& bundle,
        const char* reason);
    void Complete(const char* reason);

    CoopNativeGameStateFragmentLocator::NativeInventoryFragmentBundle BuildBundle() const;
    std::string BuildBundleStatus() const;
    const std::string& LastEvent() const { return m_lastEvent; }
    void SetLastEvent(std::string event);

private:
    CoopNativeGameStateFragmentLocator m_locator;
    uint64_t m_nextRunId = 1;
    uint64_t m_activeInventoryScopeSeq = 0;
    bool m_active = false;
    bool m_augmentAttempted = false;
    std::string m_lastEvent = "-";
};
}
