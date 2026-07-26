#include "CoopEnemyMovementGate.h"

#include "ModMain.h"
#include "CoopEnemyControlPolicy.h"
#include "CoopProtocol.h"
#include "CoopRuntimeGuards.h"

#include <string_view>

#include <Prey/GameDll/ark/npc/ArkNpc.h>
#include <Prey/GameDll/ark/npc/desires/ArkNpcMovementDesireManager.h>

using CoopRuntimeGuards::TryGuardedCall;
using CoopRuntimeGuards::TryGuardedVoidCall;

namespace CoopEnemyMovementGate
{
bool ShouldBlockObserverNativeMovement(uint32_t flags)
{
    (void)flags;
    return true;
}

bool RunSelfTest(std::string& detail)
{
    const uint32_t walking = CoopProtocol::kEnemyLocomotionFlagWalking;
    const uint32_t running = CoopProtocol::kEnemyLocomotionFlagRunning;
    const uint32_t burst =
        CoopProtocol::kEnemyLocomotionFlagDashing |
        CoopProtocol::kEnemyLocomotionFlagShifting |
        CoopProtocol::kEnemyLocomotionFlagMorphing |
        CoopProtocol::kEnemyLocomotionFlagLunging;
    const uint32_t bodyLock =
        CoopProtocol::kEnemyLocomotionFlagGlooed |
        CoopProtocol::kEnemyLocomotionFlagStunned |
        CoopProtocol::kEnemyLocomotionFlagCowering |
        CoopProtocol::kEnemyLocomotionFlagHitReacting;

    const bool walkingOk = ShouldBlockObserverNativeMovement(walking);
    const bool runningOk = ShouldBlockObserverNativeMovement(running);
    const bool idleOk = ShouldBlockObserverNativeMovement(0);
    const bool burstOk =
        ShouldBlockObserverNativeMovement(walking | burst) &&
        ShouldBlockObserverNativeMovement(running | burst);
    const bool bodyLockOk =
        ShouldBlockObserverNativeMovement(walking | bodyLock) &&
        ShouldBlockObserverNativeMovement(running | bodyLock);
    detail =
        "walkBlocked=" + std::to_string(walkingOk ? 1 : 0) +
        "_runBlocked=" + std::to_string(runningOk ? 1 : 0) +
        "_idleBlocked=" + std::to_string(idleOk ? 1 : 0) +
        "_burstBlocked=" + std::to_string(burstOk ? 1 : 0) +
        "_bodyLockBlocked=" + std::to_string(bodyLockOk ? 1 : 0);
    return walkingOk && runningOk && idleOk && burstOk && bodyLockOk;
}
}

bool ModMain::IsRemoteProxyEntityOrSpawnName(IEntity& entity) const
{
    const EntityId entityId = entity.GetId();
    if (IsRemoteProxyEntity(entityId))
        return true;

    // SpawnNpc can enter ArkHuman AI before its returned id is stored in the
    // peer slot. Dedicated proxy names close that one-frame pre-bind window.
    const char* rawName = entity.GetName();
    if (!rawName || !rawName[0])
        return false;

    const std::string_view name(rawName);
    return name == "CoopRemoteProxy" || name.rfind("CoopRemoteProxy_", 0) == 0;
}

bool ModMain::GateRemoteDrivenEnemyMovement(void* movementManagerPtr, const char* stage)
{
    if (!movementManagerPtr ||
        !gEnv ||
        !gEnv->pEntitySystem ||
        m_networkMode == CoopNetworkMode::Off ||
        !IsSessionGameplayReady())
    {
        return false;
    }

    auto* manager = reinterpret_cast<ArkNpcMovementDesireManager*>(movementManagerPtr);
    // ArkNpcMovementDesireManager is an embedded, non-polymorphic manager. It
    // has no vtable, so the generic C++-object probe rejects every valid
    // instance. Validate the full data range instead; the guarded m_pArkNpc
    // read below verifies the owning runtime object separately.
    if (!CoopRuntimeGuards::IsReadableRuntimePointer(manager, sizeof(*manager)))
        return false;

    ArkNpc* npc = nullptr;
    std::string reason;
    if (!TryGuardedCall(
            "remote enemy movement gate read npc",
            [manager]() -> ArkNpc* { return manager->m_pArkNpc; },
            npc,
            &reason) ||
        !npc ||
        !CoopRuntimeGuards::IsLikelyRuntimeCppObject(npc, sizeof(void*) * 4))
    {
        return false;
    }

    EntityId entityId = INVALID_ENTITYID;
    if (!TryGuardedCall(
            "remote enemy movement gate GetEntityId",
            [npc]() { return npc->GetEntityId(); },
            entityId,
            &reason) ||
        entityId == INVALID_ENTITYID)
    {
        return false;
    }

    IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId);
    if (!entity)
        return false;

    bool hasNativeMovementRequest = false;
    TryGuardedCall(
        "remote movement gate inspect native request",
        [manager]() { return manager->HasMovementRequest(); },
        hasNativeMovementRequest);

    if (IsRemoteProxyEntityOrSpawnName(*entity))
    {
        // Player proxies are packet-driven avatars, not locally simulated
        // ArkHumans. Setup-time AI disabling is not sufficient: native
        // movement requests can survive a load or already be queued and make
        // the proxy run away for one frame before the next pose packet repairs
        // it. Stop those requests at their common manager boundary. The pose
        // controller writes the skeleton directly and never enters this path.
        if (hasNativeMovementRequest)
        {
            TryGuardedVoidCall(
                "remote proxy block native movement planner",
                [manager]()
                {
                    manager->StopMovement();
                    manager->CancelMovement();
                });

            ++m_enemyMovementRequestBlocks;
            m_lastEnemyLocomotionEvent =
                "blocked_proxy_native_movement stage=" +
                std::string(stage && stage[0] ? stage : "-") +
                " entity=" + std::to_string(entityId) +
                " count=" + std::to_string(m_enemyMovementRequestBlocks);
            AppendEnemySyncTrace("proxy_ai_gate", m_lastEnemyLocomotionEvent);
        }
        return true;
    }

    // Player proxies are part of the base multiplayer session and must remain
    // packet-driven even when optional enemy replication is disabled. Only the
    // enemy branch below depends on the enemy-sync readiness boundary.
    if (!IsEnemyReplicationGameplayReady() ||
        !m_enemyLocomotionSyncEnabled ||
        !IsEnemyRuntimeControlCandidate(*entity))
        return false;

    const auto netIt = m_enemyNetIdsByEntity.find(entityId);
    if (netIt == m_enemyNetIdsByEntity.end())
        return false;

    EnemyAuthorityState* state = FindEnemyAuthorityByNetId(netIt->second);
    if (!state)
        return false;

    const CoopEnemyControlPolicy::Decision controlDecision =
        CoopEnemyControlPolicy::Evaluate(BuildLocalEnemyControlPolicyContext(*state, *entity));
    if (controlDecision.localVanillaAuthority ||
        !controlDecision.remoteDriven ||
        !controlDecision.blockMovement)
    {
        return false;
    }

    const uint32_t authorityFlags =
        state->remoteLocomotionFlags | state->remoteMannequinFlags;
    if (!CoopEnemyMovementGate::ShouldBlockObserverNativeMovement(authorityFlags))
        return false;

    RecordRemoteObserverLocalIntentSample(
        *state,
        *entity,
        EnemyAuthorityState::ReadOnlyIntentMovement,
        CoopProtocol::kEnemyLocomotionFlagWalking |
            CoopProtocol::kEnemyLocomotionFlagMannequinDriven,
        0,
        INVALID_ENTITYID,
        stage,
        true);
    ++state->localMovementIntentBlocks;

    // This is a gate, not a movement implementation. The observer planner is
    // stopped before it can move the entity; the replicated transform and
    // authority lower-body parameters remain its only locomotion.
    TryGuardedVoidCall(
        "remote enemy block observer movement planner",
        [manager]()
        {
            manager->StopMovement();
            manager->CancelMovement();
        });

    ++m_enemyMovementRequestBlocks;
    m_lastEnemyLocomotionEvent =
        "blocked observer native movement stage=" +
        std::string(stage && stage[0] ? stage : "-") +
        " net=" + std::to_string(state->netId) +
        " entity=" + std::to_string(entityId) +
        " mode=" + CoopEnemyControlPolicy::ModeName(controlDecision.mode) +
        " request=" + std::to_string(hasNativeMovementRequest ? 1 : 0) +
        " flags=" + std::to_string(authorityFlags);
    AppendEnemySyncTrace("locomotion", m_lastEnemyLocomotionEvent);
    return true;
}

void ModMain::ClearRemoteEnemyMovementDesire(uint64_t enemyNetId, const char* reason)
{
    // Protocol 147 no longer constructs receiver-side movement desires. Keep
    // bounded cleanup for hot reloads from an older build, but never feed one
    // back into Vanilla movement.
    const auto it = m_remoteEnemyMovementDesires.find(enemyNetId);
    if (it == m_remoteEnemyMovementDesires.end())
        return;

    m_remoteEnemyMovementDesires.erase(it);
    m_lastEnemyLocomotionEvent =
        "retired legacy remote movement desire net=" + std::to_string(enemyNetId) +
        " reason=" + (reason && reason[0] ? std::string(reason) : std::string("-"));
    AppendEnemySyncTrace("locomotion", m_lastEnemyLocomotionEvent);
}

void ModMain::ClearAllRemoteEnemyMovementDesires(const char* reason)
{
    if (m_remoteEnemyMovementDesires.empty())
        return;

    const size_t retired = m_remoteEnemyMovementDesires.size();
    m_remoteEnemyMovementDesires.clear();
    m_lastEnemyLocomotionEvent =
        "retired all legacy remote movement desires count=" + std::to_string(retired) +
        " reason=" + (reason && reason[0] ? std::string(reason) : std::string("-"));
    AppendEnemySyncTrace("locomotion", m_lastEnemyLocomotionEvent);
}

void ModMain::SyncRemoteEnemyMovementDesire(
    IEntity& entity,
    EnemyAuthorityState& state,
    const Vec3& targetPosition,
    const Vec3& moveDirection,
    float speed,
    uint32_t locomotionFlags,
    bool authorityMovementIntent)
{
    (void)entity;
    (void)targetPosition;
    (void)moveDirection;
    (void)speed;
    (void)locomotionFlags;
    (void)authorityMovementIntent;

    if (m_remoteEnemyMovementDesires.find(state.netId) != m_remoteEnemyMovementDesires.end())
        ClearRemoteEnemyMovementDesire(state.netId, "observer movement gate");
}
