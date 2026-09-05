#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <Prey/CryEntitySystem/IEntity.h>

class CoopAreaStateJournal
{
public:
    enum class LevelStatePhase : uint8_t
    {
        BeforeOriginal,
        AfterOriginal,
    };
 
    struct DirtyEntity
    {
        std::string levelName;
        std::string className;
        std::string entityName;
        EntityId entityId = INVALID_ENTITYID;
        uint64_t guid = 0;
        Vec3 position = ZERO;
        Quat rotation = Quat::CreateIdentity();
        Vec3 scale = Vec3Constants<float>::fVec3_One;
        uint32_t flags = 0;
        uint32_t eventMask = 0;
        uint32_t xformEvents = 0;
        uint32_t removeEvents = 0;
        bool hidden = false;
        bool removed = false;
    };

    struct LevelStats
    {
        uint32_t dirtyEvents = 0;
        uint32_t dirtyEntities = 0;
        uint32_t saveBoundaries = 0;
        uint32_t loadBoundaries = 0;
    };

    void NoteEntityEvent(const std::string& levelName, IEntity& entity, const SEntityEvent& event);
    void NoteEntityRemoved(const std::string& levelName, IEntity& entity);
    void NoteLevelStateSave(const std::string& levelName, LevelStatePhase phase);
    void NoteLevelStateLoad(const std::string& levelName, LevelStatePhase phase);
    void Reset();

    uint32_t GetObservedEventCount() const { return m_observedEventCount; }
    uint32_t GetTrackedEventCount() const { return m_trackedEventCount; }
    uint32_t GetDirtyEntityCount() const { return static_cast<uint32_t>(m_dirtyEntities.size()); }
    uint32_t GetTrackedLevelCount() const { return static_cast<uint32_t>(m_levelStats.size()); }
    uint32_t GetBoundarySaveCount() const { return m_boundarySaveCount; }
    uint32_t GetBoundaryLoadCount() const { return m_boundaryLoadCount; }
    const std::string& GetLastEvent() const { return m_lastEvent; }
    bool ExportJsonl(std::ostream& output, size_t maxEntityRows = 4096) const;
    bool ExportLevelJsonl(const std::string& levelName, std::ostream& output, size_t maxEntityRows = 4096) const;

private:
    bool TryCaptureEntity(const std::string& levelName, IEntity& entity, DirtyEntity& snapshot, std::string& reason) const;
    bool ShouldTrackEntity(const DirtyEntity& snapshot) const;
    void MarkDirty(const std::string& levelName, IEntity& entity, const SEntityEvent* event, bool removed);
    void RefreshLevelDirtyCount(const std::string& levelName);

    std::unordered_map<uint64_t, DirtyEntity> m_dirtyEntities;
    std::unordered_map<EntityId, uint64_t> m_entityIdToGuid;
    std::unordered_set<EntityId> m_untrackedEntityIds;
    std::unordered_map<std::string, LevelStats> m_levelStats;
    uint32_t m_observedEventCount = 0;
    uint32_t m_trackedEventCount = 0;
    uint32_t m_guardSkips = 0;
    uint32_t m_boundarySaveCount = 0;
    uint32_t m_boundaryLoadCount = 0;
    std::string m_lastEvent = "-";
};
