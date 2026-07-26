#include "ModMain.h"

#include <algorithm>
#include <exception>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "CoopRuntimeGuards.h"
#include "CoopRuntimeConfig.h"
#include "CoopRuntimeLog.h"
#include <EntityUtils.h>
#include <Chairloader/IChairLogger.h>
#include <Prey/CryEntitySystem/EntityArchetype.h>
#include <Prey/CryEntitySystem/Entity.h>
#include <Prey/CryEntitySystem/EntitySystem.h>
#include <Prey/CryEntitySystem/IEntityClass.h>
#include <Prey/CryEntitySystem/IEntitySystem.h>
#include <Prey/GameDll/ark/ArkHealthExtension.h>
#include <Prey/GameDll/ark/npc/ArkNpc.h>

namespace
{
using CoopRuntimeGuards::IsLikelyRuntimeCppObject;
using CoopRuntimeGuards::TryGuardedCall;
using CoopRuntimeGuards::TryGuardedVoidCall;

void LogCoopLifecycle(const std::string& msg)
{
    const bool verbose = CoopRuntimeConfig::Flag("COOP_TRACE_ENTITY_LIFECYCLE");
    const bool important =
        msg.find("failed") != std::string::npos ||
        msg.find("refused") != std::string::npos ||
        msg.find("invalid") != std::string::npos;
    if (verbose)
        CoopRuntimeLog::Write(msg);
    else if (important)
        CoopRuntimeLog::WriteRateLimited("entity_lifecycle_failure", msg, 2.0, 4);
}

void AddUniqueEntityId(std::vector<EntityId>& ids, EntityId entityId)
{
    if (entityId == INVALID_ENTITYID)
        return;
    if (std::find(ids.begin(), ids.end(), entityId) == ids.end())
        ids.push_back(entityId);
}

bool EnvFlagEnabledLifecycle(const char* name)
{
    return CoopRuntimeConfig::Flag(name);
}

std::string EntityDebugLabel(IEntity* entity)
{
    if (!entity)
        return "missing";

    std::string guardReason;
    EntityId entityId = INVALID_ENTITYID;
    const char* name = nullptr;
    uint32_t flags = 0;
    Vec3 pos(ZERO);
    IEntityClass* entityClass = nullptr;
    const char* className = nullptr;
    IEntityArchetype* archetype = nullptr;
    uint64_t archetypeId = 0;
    int childCount = -1;
    int slotCount = -1;
    EntityId parentId = INVALID_ENTITYID;
    unsigned aiObjectId = 0;
    bool hasPhysics = false;
    bool isHidden = false;
    bool isInvisible = false;
    bool arkNpc = false;
    bool npcDead = false;
    bool npcRagdolled = false;
    float health = -1.0f;
    float maxHealth = -1.0f;

    TryGuardedCall("entity lifecycle debug GetId", [entity]() { return entity->GetId(); }, entityId, &guardReason);
    TryGuardedCall("entity lifecycle debug GetName", [entity]() { return entity->GetName(); }, name, &guardReason);
    TryGuardedCall("entity lifecycle debug GetFlags", [entity]() { return entity->GetFlags(); }, flags, &guardReason);
    TryGuardedCall("entity lifecycle debug GetWorldPos", [entity]() { return entity->GetWorldPos(); }, pos, &guardReason);
    if (TryGuardedCall("entity lifecycle debug GetClass", [entity]() { return entity->GetClass(); }, entityClass, &guardReason) &&
        entityClass)
    {
        TryGuardedCall("entity lifecycle debug class name", [entityClass]() { return entityClass->GetName(); }, className, &guardReason);
    }
    if (TryGuardedCall("entity lifecycle debug GetArchetype", [entity]() { return entity->GetArchetype(); }, archetype, &guardReason) &&
        archetype)
    {
        TryGuardedCall("entity lifecycle debug archetype id", [archetype]() { return archetype->GetId(); }, archetypeId, &guardReason);
    }
    TryGuardedCall("entity lifecycle debug GetChildCount", [entity]() { return entity->GetChildCount(); }, childCount, &guardReason);
    TryGuardedCall("entity lifecycle debug GetSlotCount", [entity]() { return entity->GetSlotCount(); }, slotCount, &guardReason);
    TryGuardedCall("entity lifecycle debug GetAIObjectID", [entity]() { return entity->GetAIObjectID(); }, aiObjectId, &guardReason);
    TryGuardedCall("entity lifecycle debug IsHidden", [entity]() { return entity->IsHidden(); }, isHidden, &guardReason);
    TryGuardedCall("entity lifecycle debug IsInvisible", [entity]() { return entity->IsInvisible(); }, isInvisible, &guardReason);
    IPhysicalEntity* physics = nullptr;
    if (TryGuardedCall("entity lifecycle debug GetPhysics", [entity]() { return entity->GetPhysics(); }, physics, &guardReason))
        hasPhysics = physics != nullptr;
    IEntity* parent = nullptr;
    if (TryGuardedCall("entity lifecycle debug GetParent", [entity]() { return entity->GetParent(); }, parent, &guardReason) &&
        parent)
    {
        TryGuardedCall("entity lifecycle debug parent GetId", [parent]() { return parent->GetId(); }, parentId, &guardReason);
    }
    ArkNpc* npc = nullptr;
    if (TryGuardedCall("entity lifecycle debug EntityUtils::GetArkNpc", [entity]() { return EntityUtils::GetArkNpc(entity); }, npc, &guardReason) &&
        npc &&
        IsLikelyRuntimeCppObject(npc))
    {
        arkNpc = true;
        TryGuardedCall("entity lifecycle debug ArkNpc::IsDead", [npc]() { return npc->IsDead(); }, npcDead, &guardReason);
        TryGuardedCall("entity lifecycle debug ArkNpc::IsRagdolled", [npc]() { return npc->IsRagdolled(); }, npcRagdolled, &guardReason);
    }
    if (entityId != INVALID_ENTITYID)
    {
        ArkHealthExtension* healthExtension = nullptr;
        if (TryGuardedCall("entity lifecycle debug ArkHealthExtension::GetExtension", [entityId]() { return ArkHealthExtension::GetExtension(entityId); }, healthExtension, &guardReason) &&
            healthExtension &&
            IsLikelyRuntimeCppObject(healthExtension, sizeof(void*) * 2))
        {
            TryGuardedCall("entity lifecycle debug health", [healthExtension]() { return healthExtension->m_health; }, health, &guardReason);
            TryGuardedCall("entity lifecycle debug max health", [healthExtension]() { return healthExtension->m_maxHealth; }, maxHealth, &guardReason);
        }
    }

    std::ostringstream out;
    out << "id=" << entityId
        << " name=" << (name && name[0] ? name : "-")
        << " class=" << (className && className[0] ? className : "-")
        << " arch=" << archetypeId
        << " flags=0x" << std::hex << flags << std::dec
        << " pos=" << pos.x << "," << pos.y << "," << pos.z
        << " parent=" << parentId
        << " children=" << childCount
        << " slots=" << slotCount
        << " ai=" << aiObjectId
        << " physics=" << (hasPhysics ? 1 : 0)
        << " hidden=" << (isHidden ? 1 : 0)
        << " invisible=" << (isInvisible ? 1 : 0)
        << " npc=" << (arkNpc ? 1 : 0)
        << " npcDead=" << (npcDead ? 1 : 0)
        << " ragdoll=" << (npcRagdolled ? 1 : 0)
        << " health=" << health << "/" << maxHealth;
    return out.str();
}
}

std::vector<EntityId> ModMain::CaptureRuntimeEntityIdSnapshot(const char* reason) const
{
    (void)reason;
    std::vector<EntityId> ids;
    if (!gEnv || !gEnv->pEntitySystem)
        return ids;

    IEntityIt* iterator = nullptr;
    std::string guardReason;
    if (!TryGuardedCall(
            "spawn diag IEntitySystem::GetEntityIterator",
            []() { return gEnv->pEntitySystem->GetEntityIterator(); },
            iterator,
            &guardReason) ||
        !iterator)
    {
        return ids;
    }

    if (!TryGuardedVoidCall("spawn diag iterator MoveFirst", [iterator]() { iterator->MoveFirst(); }, &guardReason))
        return ids;

    while (true)
    {
        bool isEnd = true;
        if (!TryGuardedCall("spawn diag iterator IsEnd", [iterator]() { return iterator->IsEnd(); }, isEnd, &guardReason) ||
            isEnd)
        {
            break;
        }

        IEntity* entity = nullptr;
        if (!TryGuardedCall("spawn diag iterator Next", [iterator]() { return iterator->Next(); }, entity, &guardReason) ||
            !entity)
        {
            continue;
        }

        EntityId entityId = INVALID_ENTITYID;
        if (TryGuardedCall("spawn diag IEntity::GetId", [entity]() { return entity->GetId(); }, entityId, &guardReason) &&
            entityId != INVALID_ENTITYID)
        {
            ids.push_back(entityId);
        }
    }

    TryGuardedVoidCall("spawn diag iterator Release", [iterator]() { iterator->Release(); }, &guardReason);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

void ModMain::RecordCoopSpawnDiagnostics(const char* label, const std::vector<EntityId>& beforeIds, IEntity* rootEntity)
{
    ++m_spawnDiagnosticsRuns;

    std::vector<EntityId> afterIds = CaptureRuntimeEntityIdSnapshot(label);
    std::vector<EntityId> newIds;
    for (EntityId id : afterIds)
    {
        if (!std::binary_search(beforeIds.begin(), beforeIds.end(), id))
            newIds.push_back(id);
    }

    m_spawnDiagnosticsNewEntities = static_cast<uint32_t>(newIds.size());

    std::ostringstream out;
    out << "spawn diag label=" << (label ? label : "unknown")
        << " newEntities=" << newIds.size();
    if (rootEntity)
        out << " root={" << EntityDebugLabel(rootEntity) << "}";

    uint32_t details = 0;
    for (EntityId id : newIds)
    {
        if (details >= 6)
            break;

        IEntity* entity = nullptr;
        std::string reason;
        if (!ResolveCoopEntityForRemoval(id, entity, reason))
            continue;

        out << " new" << details << "={" << EntityDebugLabel(entity) << "}";
        ++details;
    }

    m_lastSpawnDiagnosticsEvent = out.str();
    LogCoopLifecycle(m_lastSpawnDiagnosticsEvent);
}

bool ModMain::ResolveCoopEntityForRemoval(EntityId entityId, IEntity*& outEntity, std::string& outReason) const
{
    outEntity = nullptr;
    outReason.clear();

    if (entityId == INVALID_ENTITYID)
    {
        outReason = "invalid entity id";
        return false;
    }

    if (!gEnv || !gEnv->pEntitySystem)
    {
        outReason = "entity system unavailable";
        return false;
    }

    IEntity* entity = nullptr;
    if (!TryGuardedCall(
            "entity lifecycle IEntitySystem::GetEntity",
            [entityId]() { return gEnv->pEntitySystem->GetEntity(entityId); },
            entity,
            &outReason))
    {
        return false;
    }

    if (!entity)
    {
        outReason = "entity already missing";
        return false;
    }

    if (!IsLikelyRuntimeCppObject(entity))
    {
        outReason = "entity pointer is not a likely runtime C++ object";
        return false;
    }

    bool garbage = false;
    if (TryGuardedCall("entity lifecycle IEntity::IsGarbage", [entity]() { return entity->IsGarbage(); }, garbage, &outReason) &&
        garbage)
    {
        outReason = "entity is already garbage";
        return false;
    }

    outEntity = entity;
    outReason.clear();
    return true;
}

bool ModMain::PrepareCoopEntityForRemoval(EntityId entityId, bool proxyEntity, bool clientPuppet, const char* reason)
{
    IEntity* entity = nullptr;
    std::string guardReason;
    if (!ResolveCoopEntityForRemoval(entityId, entity, guardReason))
    {
        m_lastGuardedEntityRemoveEvent =
            "prepare remove skipped entity=" + std::to_string(entityId) +
            " reason=" + guardReason;
        return false;
    }

    // Restore the living collider before quarantine/removal. Some cleanup
    // paths reuse the body instead of deleting it immediately; dropping the
    // snapshot here left that body permanently in remote-mirror physics and a
    // later authority owner could walk through world geometry.
    if (m_remoteEnemyPhysicsSnapshots.find(entityId) != m_remoteEnemyPhysicsSnapshots.end())
    {
        SetRemoteEnemyMirrorPhysics(*entity, false, "prepare entity removal");
        if (m_remoteEnemyPhysicsSnapshots.erase(entityId) != 0)
            ++m_remoteEnemyPhysicsDrops;
    }

    uint32_t failures = 0;

    if (proxyEntity)
    {
        if (!TryGuardedVoidCall(
                "entity lifecycle unregister revive",
                [this, entityId]() { UnregisterProxyReviveInteraction(entityId); },
                &guardReason))
        {
            ++failures;
        }

        // Complex-attention registration belongs only to the compatibility
        // proxy. Additional multiplayer proxies have independent revive
        // registrations, so retiring one must not unregister every other peer.
        if (entityId == m_proxyEntityId && !TryGuardedVoidCall(
                    "entity lifecycle unregister complex attention",
                    [this]() { UnregisterProxyComplexAttention(); },
                    &guardReason))
        {
            ++failures;
        }

        if (!TryGuardedVoidCall(
                "entity lifecycle clear proxy as enemy target",
                [this, entityId]() { ClearProxyAsEnemyTarget(entityId); },
                &guardReason))
        {
            ++failures;
        }
    }

    if (!TryGuardedVoidCall(
            "entity lifecycle mark runtime flags",
            [this, entity, clientPuppet]() { MarkCoopRuntimeEntity(*entity, clientPuppet); },
            &guardReason))
    {
        ++failures;
    }

    if (!TryGuardedVoidCall(
            "entity lifecycle hide and invisible",
            [entity]()
            {
                entity->Hide(true);
                entity->Invisible(true);
            },
            &guardReason))
    {
        ++failures;
    }

    ArkNpc* npc = nullptr;
    if (TryGuardedCall(
            "entity lifecycle EntityUtils::GetArkNpc",
            [entity]() { return EntityUtils::GetArkNpc(entity); },
            npc,
            &guardReason) &&
        npc &&
        IsLikelyRuntimeCppObject(npc))
    {
        auto guardedNpcCall = [&](const char* label, auto&& call)
        {
            std::string localReason;
            if (!TryGuardedVoidCall(label, std::forward<decltype(call)>(call), &localReason))
            {
                ++failures;
                guardReason = localReason;
            }
        };

        guardedNpcCall("entity lifecycle npc ClearAllAttention", [npc]() { npc->ClearAllAttention(); });
        guardedNpcCall("entity lifecycle npc FlowGraphIgnoreDistractions", [npc]() { npc->FlowGraphIgnoreDistractions(true, true); });
        guardedNpcCall("entity lifecycle npc DeactivateWander", [npc]() { npc->DeactivateWander(); });
        guardedNpcCall("entity lifecycle npc OnCombatEnd", [npc]() { npc->OnCombatEnd(); });
        guardedNpcCall("entity lifecycle npc PushDisableAttentionObjectAndPerceivables", [npc]() { npc->PushDisableAttentionObjectAndPerceivables(); });
        guardedNpcCall("entity lifecycle npc PushDisableAttentionAndSenses", [npc]() { npc->PushDisableAttentionAndSenses(); });
        guardedNpcCall("entity lifecycle npc PushDisableSenses", [npc]() { npc->PushDisableSenses(); });
        guardedNpcCall("entity lifecycle npc PushDisableVisible", [npc]() { npc->PushDisableVisible(); });
        guardedNpcCall("entity lifecycle npc PushDisableAudible", [npc]() { npc->PushDisableAudible(); });
    }

    CEntitySystem* const entitySystem = gEnv && gEnv->pEntitySystem
        ? reinterpret_cast<CEntitySystem*>(gEnv->pEntitySystem)
        : nullptr;
    if (entitySystem && !TryGuardedVoidCall(
            "entity lifecycle remove entity event listeners",
            [entitySystem, entity]()
            {
                entitySystem->RemoveEntityEventListeners(static_cast<CEntity*>(entity));
            },
            &guardReason))
    {
        ++failures;
    }

    if (m_runtimeTransitionCleanupPrepared && !TryGuardedVoidCall(
            "entity lifecycle mark transition entity garbage",
            [entity]()
            {
                // ArkHuman runtime proxies can retain a dead component-event
                // listener during world teardown. Let the native level reset
                // reclaim them without dispatching ENTITY_EVENT_DONE through
                // that stale listener.
                static_cast<CEntity*>(entity)->m_bGarbage = 1;
            },
            &guardReason))
    {
        ++failures;
    }

    if (!TryGuardedVoidCall(
            "entity lifecycle clear unremovable flag",
            [entity]()
            {
                entity->SetFlags(entity->GetFlags() & ~static_cast<uint32_t>(ENTITY_FLAG_UNREMOVABLE));
            },
            &guardReason))
    {
        ++failures;
    }

    m_lastGuardedEntityRemoveEvent =
        "prepare remove entity=" + EntityDebugLabel(entity) +
        " proxy=" + std::to_string(proxyEntity ? 1 : 0) +
        " puppet=" + std::to_string(clientPuppet ? 1 : 0) +
        " failures=" + std::to_string(failures) +
        " reason=" + (reason ? reason : "unknown");
    if (!guardReason.empty())
        m_lastGuardedEntityRemoveEvent += " guard=" + guardReason;

    LogCoopLifecycle(m_lastGuardedEntityRemoveEvent);
    return failures == 0;
}

bool ModMain::RemoveCoopEntityGuarded(EntityId entityId, bool forceNow, const char* reason)
{
    ++m_guardedEntityRemoveAttempts;

    IEntity* entity = nullptr;
    std::string guardReason;
    if (!ResolveCoopEntityForRemoval(entityId, entity, guardReason))
    {
        ++m_guardedEntityRemoveSuccesses;
        m_lastGuardedEntityRemoveEvent =
            "remove skipped entity=" + std::to_string(entityId) +
            " reason=" + guardReason;
        return true;
    }

    const std::string entityLabel = EntityDebugLabel(entity);
    if (!TryGuardedVoidCall(
            "entity lifecycle IEntitySystem::RemoveEntity",
            [entityId, forceNow]()
            {
                if (gEnv && gEnv->pEntitySystem)
                    gEnv->pEntitySystem->RemoveEntity(entityId, forceNow);
            },
            &guardReason))
    {
        ++m_guardedEntityRemoveFailures;
        m_lastGuardedEntityRemoveEvent =
            "remove failed entity=" + entityLabel +
            " reason=" + (reason ? reason : "unknown") +
            " guard=" + guardReason;
        LogCoopLifecycle(m_lastGuardedEntityRemoveEvent);
        return false;
    }

    ++m_guardedEntityRemoveSuccesses;
    m_lastGuardedEntityRemoveEvent =
        "removed entity=" + entityLabel +
        " force=" + std::to_string(forceNow ? 1 : 0) +
        " reason=" + (reason ? reason : "unknown");
    LogCoopLifecycle(m_lastGuardedEntityRemoveEvent);
    return true;
}

void ModMain::QueueNetworkRuntimeCleanup(const char* reason)
{
    std::vector<EntityId> ids = m_pendingNetworkRuntimeCleanupEntityIds;
    AddUniqueEntityId(ids, m_proxyEntityId);
    AddUniqueEntityId(ids, m_mimicEntityId);
    AddRuntimeEnemyEntityIds(ids);

    if (ids.empty())
        return;

    if (!m_pendingNetworkRuntimeCleanup)
        m_pendingNetworkRuntimeCleanupProxyId = m_proxyEntityId;
    else if (m_pendingNetworkRuntimeCleanupProxyId == INVALID_ENTITYID)
        m_pendingNetworkRuntimeCleanupProxyId = m_proxyEntityId;

    m_pendingNetworkRuntimeCleanupEntityIds = std::move(ids);
    m_pendingNetworkRuntimeCleanup = true;
    m_pendingNetworkRuntimeCleanupDelaySeconds = std::max(m_pendingNetworkRuntimeCleanupDelaySeconds, 0.25f);
    m_pendingNetworkRuntimeCleanupReason = reason && reason[0] ? reason : "network cleanup";
    ++m_guardedEntityRemoveDeferrals;

    m_lastGuardedEntityRemoveEvent =
        "queued runtime cleanup ids=" + std::to_string(m_pendingNetworkRuntimeCleanupEntityIds.size()) +
        " reason=" + m_pendingNetworkRuntimeCleanupReason;
    LogCoopLifecycle(m_lastGuardedEntityRemoveEvent);
}

void ModMain::SoftQuarantineNetworkRuntimeEntitiesForDisconnect(const char* reason)
{
    const std::string cleanupReason = reason && reason[0] ? reason : "peer disconnected";
    std::vector<EntityId> ids;
    uint32_t hidden = 0;
    uint32_t missing = 0;
    uint32_t failed = 0;
    std::string stage = "collect_begin";
    EntityId stageEntityId = INVALID_ENTITYID;
    bool completed = false;

    try
    {
        stage = "collect_proxy";
        AddUniqueEntityId(ids, m_proxyEntityId);
        stage = "collect_mimic";
        AddUniqueEntityId(ids, m_mimicEntityId);
        stage = "collect_enemy_puppets";
        AddRuntimeEnemyEntityIds(ids);

        m_lastGuardedEntityRemoveEvent =
            "soft disconnect quarantine collect ids=" + std::to_string(ids.size()) +
            " reason=" + cleanupReason;
        LogCoopLifecycle(m_lastGuardedEntityRemoveEvent);

        for (EntityId entityId : ids)
        {
            stageEntityId = entityId;
            IEntity* entity = nullptr;
            std::string guardReason;
            stage = "resolve_entity";
            if (!ResolveCoopEntityForRemoval(entityId, entity, guardReason))
            {
                ++missing;
                m_lastGuardedEntityRemoveEvent =
                    "soft disconnect quarantine missing entity=" + std::to_string(entityId) +
                    " reason=" + guardReason;
                LogCoopLifecycle(m_lastGuardedEntityRemoveEvent);
                continue;
            }

            stage = "hide_entity";
            if (!TryGuardedVoidCall(
                    "entity lifecycle soft quarantine hide",
                    [this, entity]()
                    {
                        MarkCoopRuntimeEntity(*entity, true);
                        entity->Hide(true);
                        entity->Invisible(true);
                    },
                    &guardReason))
            {
                ++failed;
                m_lastGuardedEntityRemoveEvent =
                    "soft disconnect quarantine hide failed entity=" + std::to_string(entityId) +
                    " reason=" + guardReason;
                LogCoopLifecycle(m_lastGuardedEntityRemoveEvent);
                continue;
            }

            ++hidden;
        }

        if (m_proxyEntityId != INVALID_ENTITYID)
        {
            std::string guardReason;
            stageEntityId = m_proxyEntityId;
            stage = "unregister_revive";
            if (!TryGuardedVoidCall(
                    "entity lifecycle soft quarantine unregister revive",
                    [this]() { UnregisterProxyReviveInteraction(); },
                    &guardReason))
            {
                ++failed;
                LogCoopLifecycle("soft disconnect quarantine unregister revive failed reason=" + guardReason);
            }

            stage = "unregister_attention";
            if (!TryGuardedVoidCall(
                    "entity lifecycle soft quarantine unregister attention",
                    [this]() { UnregisterProxyComplexAttention(); },
                    &guardReason))
            {
                ++failed;
                LogCoopLifecycle("soft disconnect quarantine unregister attention failed reason=" + guardReason);
            }

            stage = "clear_enemy_target";
            if (!TryGuardedVoidCall(
                    "entity lifecycle soft quarantine clear enemy target",
                    [this, proxyId = m_proxyEntityId]() { ClearProxyAsEnemyTarget(proxyId); },
                    &guardReason))
            {
                ++failed;
                LogCoopLifecycle("soft disconnect quarantine clear enemy target failed reason=" + guardReason);
            }

            stage = "reset_proxy_state";
            m_proxyEntityId = INVALID_ENTITYID;
            m_proxyWasConfigured = false;
            ResetProxyLifecycleRuntimeState(cleanupReason.c_str());
            SetProxyLifecycleState(CoopProxyLifecycleState::Destroyed, cleanupReason.c_str());
            ResetProxyHealthBaseline();
        }

        completed = true;
    }
    catch (const std::exception& ex)
    {
        ++failed;
        m_lastGuardedEntityRemoveEvent =
            "soft disconnect quarantine exception stage=" + stage +
            " entity=" + std::to_string(stageEntityId) +
            " reason=" + ex.what();
        LogCoopLifecycle(m_lastGuardedEntityRemoveEvent);
    }
    catch (...)
    {
        ++failed;
        m_lastGuardedEntityRemoveEvent =
            "soft disconnect quarantine exception stage=" + stage +
            " entity=" + std::to_string(stageEntityId) +
            " reason=unknown";
        LogCoopLifecycle(m_lastGuardedEntityRemoveEvent);
    }

    if (!completed)
    {
        m_proxyEntityId = INVALID_ENTITYID;
        m_proxyWasConfigured = false;
        ResetProxyHealthBaseline();
    }

    m_mimicEntityId = INVALID_ENTITYID;
    m_animationTestProxyEntityId = INVALID_ENTITYID;
    ClearAllRemoteEnemyMovementDesires("soft disconnect quarantine cleanup");
    m_enemyPuppets.clear();
    m_enemyNetIdsByEntity.clear();
    m_enemyAuthorities.clear();
    m_pendingNetworkRuntimeCleanup = false;
    m_pendingNetworkRuntimeCleanupDelaySeconds = 0.0f;
    m_pendingNetworkRuntimeCleanupProxyId = INVALID_ENTITYID;
    m_pendingNetworkRuntimeCleanupEntityIds.clear();
    m_pendingNetworkRuntimeCleanupReason.clear();

    m_lastGuardedEntityRemoveEvent =
        "soft disconnect quarantine hidden=" + std::to_string(hidden) +
        " missing=" + std::to_string(missing) +
        " failed=" + std::to_string(failed) +
        " reason=" + cleanupReason;
    LogCoopLifecycle(m_lastGuardedEntityRemoveEvent);
}

void ModMain::TickNetworkRuntimeCleanup(float frameTime)
{
    if (!m_pendingNetworkRuntimeCleanup)
        return;

    if (m_pendingNetworkRuntimeCleanupDelaySeconds > 0.0f)
    {
        m_pendingNetworkRuntimeCleanupDelaySeconds -= std::max(0.0f, frameTime);
        return;
    }

    if (m_saveLoadGuardActive ||
        m_pendingPostLoadResync ||
        m_arkLevelTransitionLoadActive ||
        m_runtimeTransitionCleanupPrepared ||
        !gEnv ||
        !gEnv->pEntitySystem)
    {
        m_pendingNetworkRuntimeCleanupDelaySeconds = 0.25f;
        return;
    }

    const std::vector<EntityId> ids = m_pendingNetworkRuntimeCleanupEntityIds;
    const EntityId proxyId = m_pendingNetworkRuntimeCleanupProxyId;
    const std::string cleanupReason = m_pendingNetworkRuntimeCleanupReason.empty() ?
        std::string("network cleanup") :
        m_pendingNetworkRuntimeCleanupReason;

    uint32_t removedOrMissing = 0;
    uint32_t quarantined = 0;
    uint32_t failed = 0;
    const bool nativeRemove =
        CoopRuntimeConfig::UnsafeFlag("COOP_ENABLE_RUNTIME_ENTITY_REMOVE") &&
        !EnvFlagEnabledLifecycle("COOP_DISABLE_RUNTIME_ENTITY_REMOVE");
    for (EntityId entityId : ids)
    {
        const bool isProxy = entityId == proxyId || entityId == m_proxyEntityId;
        const bool isPuppet = IsEnemyPuppetEntity(entityId);
        const bool prepared = PrepareCoopEntityForRemoval(entityId, isProxy, isPuppet, cleanupReason.c_str());
        if (prepared)
            ++quarantined;

        const bool done = !nativeRemove || RemoveCoopEntityGuarded(entityId, false, cleanupReason.c_str());
        if (done)
        {
            if (nativeRemove)
                ++removedOrMissing;
            if (entityId == m_proxyEntityId || entityId == proxyId)
            {
                m_proxyEntityId = INVALID_ENTITYID;
                m_proxyWasConfigured = false;
                ResetProxyLifecycleRuntimeState(cleanupReason.c_str());
                SetProxyLifecycleState(CoopProxyLifecycleState::Destroyed, cleanupReason.c_str());
                ResetProxyHealthBaseline();
            }
            if (entityId == m_mimicEntityId)
                m_mimicEntityId = INVALID_ENTITYID;
            if (entityId == m_animationTestProxyEntityId)
                m_animationTestProxyEntityId = INVALID_ENTITYID;
        }
        else
        {
            ++failed;
        }
    }

    m_pendingNetworkRuntimeCleanup = false;
    m_pendingNetworkRuntimeCleanupDelaySeconds = 0.0f;
    m_pendingNetworkRuntimeCleanupProxyId = INVALID_ENTITYID;
    m_pendingNetworkRuntimeCleanupEntityIds.clear();
    m_pendingNetworkRuntimeCleanupReason.clear();

    if (failed == 0)
    {
        ClearAllRemoteEnemyMovementDesires("runtime cleanup success");
        m_enemyPuppets.clear();
        m_enemyNetIdsByEntity.clear();
        m_enemyAuthorities.clear();
    }

    m_lastGuardedEntityRemoveEvent =
        "runtime cleanup applied quarantined=" + std::to_string(quarantined) +
        " nativeRemove=" + std::to_string(nativeRemove ? 1 : 0) +
        " removedOrMissing=" + std::to_string(removedOrMissing) +
        " failed=" + std::to_string(failed) +
        " reason=" + cleanupReason;
    LogCoopLifecycle(m_lastGuardedEntityRemoveEvent);
}
