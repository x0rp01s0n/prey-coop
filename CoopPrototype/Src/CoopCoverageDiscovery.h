#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

class CoopCoverageDiscovery final
{
public:
    void Reset();

    void RecordEntitySpawn(uint32_t entityId, std::string_view className);
    void RecordEntityRemoved(uint32_t entityId);
    void RecordEntityEvent(uint32_t entityId, int eventType);
    void RecordDamageSource(bool stableIdentity, std::string_view detail);
    void RecordProjectile(bool classified, std::string_view detail);
    void RecordNpcAbility(bool classified, std::string_view detail);
    void RecordStoryCommit(bool classified, std::string_view detail);

    std::string BuildCompactReport() const;
    std::string BuildDetailedReport() const;

private:
    struct ClassObservation
    {
        uint64_t spawns = 0;
        uint64_t persistentEvents = 0;
    };

    static bool IsClassCoveredByKnownPolicy(std::string_view className);
    static std::string Normalize(std::string_view value);
    static std::string BoundedDetail(std::string_view value);

    static constexpr size_t kMaxUnclassifiedClasses = 64;
    static constexpr size_t kMaxTrackedEntities = 2048;

    uint64_t m_entitySpawns = 0;
    uint64_t m_classifiedEntitySpawns = 0;
    uint64_t m_unclassifiedEntitySpawns = 0;
    uint64_t m_unclassifiedPersistentEvents = 0;
    uint64_t m_droppedEntityTracking = 0;
    uint64_t m_damageSources = 0;
    uint64_t m_unstableDamageSources = 0;
    uint64_t m_projectiles = 0;
    uint64_t m_unclassifiedProjectiles = 0;
    uint64_t m_npcAbilities = 0;
    uint64_t m_unclassifiedNpcAbilities = 0;
    uint64_t m_storyCommits = 0;
    uint64_t m_unclassifiedStoryCommits = 0;

    std::unordered_map<std::string, ClassObservation> m_unclassifiedClasses;
    std::unordered_map<uint32_t, std::string> m_unclassifiedEntities;
    std::string m_lastUnclassifiedClass = "-";
    std::string m_lastUnstableDamage = "-";
    std::string m_lastUnclassifiedProjectile = "-";
    std::string m_lastUnclassifiedNpcAbility = "-";
    std::string m_lastUnclassifiedStoryCommit = "-";
};
