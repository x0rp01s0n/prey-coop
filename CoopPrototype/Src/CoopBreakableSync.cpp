#include "ModMain.h"
#include "CoopRuntimeGuards.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>

#include <Prey/CryAction/IGameObject.h>
#include <Prey/CryGame/IGame.h>
#include <Prey/CryGame/IGameFramework.h>
#include <Prey/CryMath/CryHalf.inl>
#include <Prey/CrySystem/ISystem.h>
#include <Prey/GameDll/ark/arkbreakable.h>
#include <Prey/GameDll/ark/player/ArkPlayer.h>

namespace
{
constexpr float kBreakableHealthEpsilon = 0.0005f;
constexpr float kMaxHalfFloat = 65504.0f;
constexpr uint64_t kDebugBreakableArchetype = 221ull;
constexpr EntityGUID kDebugBreakableGuid = 0x434f4f5042524b31ull; // "COOPBRK1"
constexpr uint64_t kDebugScalableBreakableArchetype = 10739735956144680886ull;
constexpr EntityGUID kDebugScalableBreakableGuid = 0x434f4f5053424b31ull; // "COOPSBK1"

auto s_hookCArkBreakableSetHealth = CArkBreakable::FSetHealth.MakeHook();

std::string BreakableStatusToken(std::string value)
{
    if (value.empty())
        return "-";
    for (char& ch : value)
    {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '.')
            ch = '_';
    }
    return value;
}

CArkBreakable* GetBreakableExtension(IEntity& entity, std::string* outReason)
{
    std::string localReason;
    std::string& reason = outReason ? *outReason : localReason;
    if (!gEnv || !gEnv->pGame)
    {
        reason = "missing_game_runtime";
        return nullptr;
    }

    IGameFramework* framework = gEnv->pGame->GetIGameFramework();
    IGameObject* gameObject = nullptr;
    if (!framework ||
        !CoopRuntimeGuards::TryGuardedCall(
            "breakable GetGameObject",
            [framework, &entity]() { return framework->GetGameObject(entity.GetId()); },
            gameObject,
            &reason) ||
        !gameObject)
    {
        reason = "missing_breakable_game_object";
        return nullptr;
    }

    constexpr const char* kExtensionNames[] = {"ArkBreakable", "ArkScalableBreakable"};
    for (const char* extensionName : kExtensionNames)
    {
        IGameObjectExtension* extension = nullptr;
        IGameObjectSystem::ExtensionID extensionId = 0;
        if (CoopRuntimeGuards::TryGuardedCall(
                "breakable GetExtensionId",
                [gameObject, extensionName]() { return gameObject->GetExtensionId(extensionName); },
                extensionId,
                &reason) &&
            extensionId != 0 &&
            CoopRuntimeGuards::TryGuardedCall(
                "breakable QueryExtension id",
                [gameObject, extensionId]() { return gameObject->QueryExtension(extensionId); },
                extension,
                &reason) &&
            extension)
        {
            return static_cast<CArkBreakable*>(extension);
        }

        extension = nullptr;
        if (CoopRuntimeGuards::TryGuardedCall(
                "breakable QueryExtension name",
                [gameObject, extensionName]() { return gameObject->QueryExtension(extensionName); },
                extension,
                &reason) &&
            extension)
        {
            return static_cast<CArkBreakable*>(extension);
        }
    }

    reason = "missing_breakable_extension";
    return nullptr;
}

uint16_t EncodeBreakableHealth(float health)
{
    const float bounded = std::clamp(health, 0.0f, kMaxHalfFloat);
    uint16_t encoded = static_cast<uint16_t>(CryConvertFloatToHalf(bounded));
    while (encoded > 0 && CryConvertHalfToFloat(static_cast<CryHalf>(encoded)) > bounded)
        --encoded;
    return encoded;
}

bool DecodeBreakableHealth(uint16_t encoded, float& health)
{
    health = CryConvertHalfToFloat(static_cast<CryHalf>(encoded));
    return std::isfinite(health) && health >= 0.0f && health <= kMaxHalfFloat;
}

CArkBreakable* GetDebugBreakable(IEntity** outEntity, std::string& reason)
{
    if (outEntity)
        *outEntity = nullptr;
    if (!gEnv || !gEnv->pEntitySystem)
    {
        reason = "missing_entity_system";
        return nullptr;
    }

    IEntity* entity = nullptr;
    constexpr EntityGUID kDebugGuids[] = {kDebugScalableBreakableGuid, kDebugBreakableGuid};
    for (const EntityGUID guid : kDebugGuids)
    {
        EntityId entityId = INVALID_ENTITYID;
        if (CoopRuntimeGuards::TryGuardedCall(
                "debug breakable FindEntityByGuid",
                [guid]() { return gEnv->pEntitySystem->FindEntityByGuid(guid); },
                entityId,
                &reason) &&
            entityId != INVALID_ENTITYID &&
            CoopRuntimeGuards::TryGuardedCall(
                "debug breakable GetEntity",
                [entityId]() { return gEnv->pEntitySystem->GetEntity(entityId); },
                entity,
                &reason) &&
            entity)
        {
            break;
        }
    }
    if (!entity)
    {
        reason = "missing_debug_breakable";
        return nullptr;
    }

    if (outEntity)
        *outEntity = entity;
    return GetBreakableExtension(*entity, &reason);
}

CArkBreakable* GetBreakableByGuid(uint64_t guid, IEntity** outEntity, std::string& reason)
{
    if (outEntity)
        *outEntity = nullptr;
    if (!gEnv || !gEnv->pEntitySystem || guid == 0)
    {
        reason = "missing_breakable_target";
        return nullptr;
    }

    EntityId entityId = INVALID_ENTITYID;
    IEntity* entity = nullptr;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "target breakable FindEntityByGuid",
            [guid]() { return gEnv->pEntitySystem->FindEntityByGuid(static_cast<EntityGUID>(guid)); },
            entityId,
            &reason) ||
        entityId == INVALID_ENTITYID ||
        !CoopRuntimeGuards::TryGuardedCall(
            "target breakable GetEntity",
            [entityId]() { return gEnv->pEntitySystem->GetEntity(entityId); },
            entity,
            &reason) ||
        !entity)
    {
        reason = "missing_breakable_guid_" + std::to_string(guid);
        return nullptr;
    }

    CArkBreakable* breakable = GetBreakableExtension(*entity, &reason);
    if (breakable && outEntity)
        *outEntity = entity;
    return breakable;
}

CArkBreakable* FindNearestAuthoredBreakable(IEntity** outEntity, std::string& reason)
{
    if (outEntity)
        *outEntity = nullptr;
    if (!gEnv || !gEnv->pEntitySystem || !ArkPlayer::GetInstancePtr() || !ArkPlayer::GetInstance().GetEntity())
    {
        reason = "missing_breakable_runtime";
        return nullptr;
    }

    IEntityIt* rawIterator = nullptr;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "nearest breakable GetEntityIterator",
            []() { return gEnv->pEntitySystem->GetEntityIterator(); },
            rawIterator,
            &reason) ||
        !rawIterator)
    {
        return nullptr;
    }

    const Vec3 playerPosition = ArkPlayer::GetInstance().GetEntity()->GetWorldPos();
    IEntityItPtr iterator = rawIterator;
    CoopRuntimeGuards::TryGuardedVoidCall("nearest breakable MoveFirst", [&iterator]() { iterator->MoveFirst(); }, &reason);
    IEntity* bestEntity = nullptr;
    CArkBreakable* bestBreakable = nullptr;
    float bestDistanceSq = std::numeric_limits<float>::max();
    while (true)
    {
        bool atEnd = false;
        if (!CoopRuntimeGuards::TryGuardedCall("nearest breakable IsEnd", [&iterator]() { return iterator->IsEnd(); }, atEnd, &reason) || atEnd)
            break;

        IEntity* candidate = nullptr;
        if (!CoopRuntimeGuards::TryGuardedCall("nearest breakable Next", [&iterator]() { return iterator->Next(); }, candidate, &reason) || !candidate)
            break;

        uint64_t guid = 0;
        IEntityClass* entityClass = nullptr;
        const char* className = nullptr;
        if (!CoopRuntimeGuards::TryGuardedCall("nearest breakable GetGuid", [candidate]() { return candidate->GetGuid(); }, guid, &reason) || guid == 0 ||
            !CoopRuntimeGuards::TryGuardedCall("nearest breakable GetClass", [candidate]() { return candidate->GetClass(); }, entityClass, &reason) || !entityClass ||
            !CoopRuntimeGuards::TryGuardedCall("nearest breakable class GetName", [entityClass]() { return entityClass->GetName(); }, className, &reason) ||
            !className || std::strcmp(className, "ArkBreakable") != 0)
        {
            continue;
        }

        CArkBreakable* candidateBreakable = GetBreakableExtension(*candidate, &reason);
        float health = 0.0f;
        Vec3 position(ZERO);
        if (!candidateBreakable ||
            !CoopRuntimeGuards::TryGuardedCall("nearest breakable health", [candidateBreakable]() { return candidateBreakable->m_health; }, health, &reason) ||
            health < 7.0f ||
            !CoopRuntimeGuards::TryGuardedCall("nearest breakable position", [candidate]() { return candidate->GetWorldPos(); }, position, &reason))
        {
            continue;
        }

        const float distanceSq = (position - playerPosition).GetLengthSquared();
        if (distanceSq < bestDistanceSq)
        {
            bestDistanceSq = distanceSq;
            bestEntity = candidate;
            bestBreakable = candidateBreakable;
        }
    }

    if (!bestBreakable)
    {
        reason = "missing_authored_breakable";
        return nullptr;
    }
    if (outEntity)
        *outEntity = bestEntity;
    return bestBreakable;
}

void CArkBreakable_SetHealth_Hook(CArkBreakable* breakable, const float health)
{
    float before = 0.0f;
    std::string reason;
    const bool beforeOk = breakable && CoopRuntimeGuards::TryGuardedCall(
        "breakable SetHealth read before",
        [breakable]() { return breakable->m_health; },
        before,
        &reason);

    s_hookCArkBreakableSetHealth.InvokeOrig(breakable, health);

    float after = 0.0f;
    const bool afterOk = breakable && CoopRuntimeGuards::TryGuardedCall(
        "breakable SetHealth read after",
        [breakable]() { return breakable->m_health; },
        after,
        &reason);
    if (gMod && beforeOk && afterOk)
        gMod->OnNativeBreakableHealthChanged(breakable, before, after, "CArkBreakable::SetHealth");
}
}

void ModMain::InitBreakableSyncHooks()
{
    s_hookCArkBreakableSetHealth.SetHookFunc(&CArkBreakable_SetHealth_Hook);
}

void ModMain::OnNativeBreakableHealthChanged(
    CArkBreakable* breakable,
    float before,
    float after,
    const char* reason)
{
    ++m_breakableHealthHookCalls;
    if (!breakable || !std::isfinite(before) || !std::isfinite(after) ||
        after + kBreakableHealthEpsilon >= before ||
        m_applyingRemoteAreaObjectEvent ||
        m_saveLoadGuardActive || m_arkLevelTransitionLoadActive ||
        IsPostLoadNativeQuarantineActive() ||
        m_networkMode == CoopNetworkMode::Off || !IsSessionGameplayReady())
    {
        ++m_breakableHealthEventSkips;
        return;
    }

    IEntity* entity = nullptr;
    EntityGUID guid = 0;
    std::string guardReason;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "breakable SetHealth GetEntity",
            [breakable]() { return breakable->GetEntity(); },
            entity,
            &guardReason) ||
        !entity ||
        !CoopRuntimeGuards::TryGuardedCall(
            "breakable SetHealth GetGuid",
            [entity]() { return entity->GetGuid(); },
            guid,
            &guardReason) ||
        guid == 0)
    {
        ++m_breakableHealthEventSkips;
        m_lastBreakableHealthEvent =
            "skip_missing_guid_reason_" + BreakableStatusToken(guardReason);
        return;
    }

    const uint16_t encodedHealth = EncodeBreakableHealth(after);
    if (!QueueLocalAreaObjectEventForHook(
            CoopProtocol::kAreaObjectEventBreakableHealth,
            static_cast<uint64_t>(guid),
            encodedHealth,
            0,
            reason))
    {
        ++m_breakableHealthEventSkips;
        m_lastBreakableHealthEvent =
            "queue_failed_guid_" + std::to_string(static_cast<uint64_t>(guid)) +
            "_health_" + std::to_string(after);
        return;
    }

    ++m_breakableHealthEventsQueued;
    m_lastBreakableHealthEvent =
        "queued_guid_" + std::to_string(static_cast<uint64_t>(guid)) +
        "_before_" + std::to_string(before) +
        "_after_" + std::to_string(after) +
        "_encoded_" + std::to_string(encodedHealth);
}

bool ModMain::ApplyAreaObjectBreakableHealth(
    uint64_t targetGuid,
    uint16_t encodedHealth,
    std::string& detail)
{
    float requestedHealth = 0.0f;
    if (targetGuid == 0 || !DecodeBreakableHealth(encodedHealth, requestedHealth) ||
        !gEnv || !gEnv->pEntitySystem)
    {
        ++m_breakableHealthEventSkips;
        detail = "invalid_breakable_health_payload";
        return false;
    }

    EntityId entityId = INVALID_ENTITYID;
    IEntity* entity = nullptr;
    std::string reason;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "breakable apply FindEntityByGuid",
            [targetGuid]() { return gEnv->pEntitySystem->FindEntityByGuid(static_cast<EntityGUID>(targetGuid)); },
            entityId,
            &reason) ||
        entityId == INVALID_ENTITYID ||
        !CoopRuntimeGuards::TryGuardedCall(
            "breakable apply GetEntity",
            [entityId]() { return gEnv->pEntitySystem->GetEntity(entityId); },
            entity,
            &reason) ||
        !entity)
    {
        if (requestedHealth <= kBreakableHealthEpsilon)
        {
            detail = "already_missing_broken_breakable_guid_" + std::to_string(targetGuid);
            return true;
        }
        ++m_breakableHealthEventSkips;
        detail = "missing_breakable_guid_" + std::to_string(targetGuid);
        return false;
    }

    CArkBreakable* breakable = GetBreakableExtension(*entity, &reason);
    float before = 0.0f;
    if (!breakable ||
        !CoopRuntimeGuards::TryGuardedCall(
            "breakable apply read before",
            [breakable]() { return breakable->m_health; },
            before,
            &reason) ||
        !std::isfinite(before))
    {
        ++m_breakableHealthEventSkips;
        detail = "missing_breakable_extension_guid_" + std::to_string(targetGuid) +
            "_reason_" + BreakableStatusToken(reason);
        return false;
    }

    const float desiredHealth = std::min(before, requestedHealth);
    if (before <= desiredHealth + kBreakableHealthEpsilon)
    {
        detail = "already_breakable_health_guid_" + std::to_string(targetGuid) +
            "_health_" + std::to_string(before);
        return true;
    }

    if (!CoopRuntimeGuards::TryGuardedVoidCall(
            "breakable apply SetHealth",
            [breakable, desiredHealth]() { breakable->SetHealth(desiredHealth); },
            &reason))
    {
        ++m_breakableHealthEventSkips;
        detail = "breakable_set_health_failed_guid_" + std::to_string(targetGuid) +
            "_reason_" + BreakableStatusToken(reason);
        return false;
    }

    float after = before;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "breakable apply read after",
            [breakable]() { return breakable->m_health; },
            after,
            &reason) ||
        !std::isfinite(after) || after > desiredHealth + kBreakableHealthEpsilon)
    {
        ++m_breakableHealthEventSkips;
        detail = "breakable_health_verify_failed_guid_" + std::to_string(targetGuid) +
            "_before_" + std::to_string(before) +
            "_desired_" + std::to_string(desiredHealth) +
            "_after_" + std::to_string(after);
        return false;
    }

    ++m_breakableHealthEventsApplied;
    m_lastBreakableHealthEvent =
        "applied_guid_" + std::to_string(targetGuid) +
        "_before_" + std::to_string(before) +
        "_after_" + std::to_string(after) +
        "_encoded_" + std::to_string(encodedHealth);
    detail = m_lastBreakableHealthEvent;
    return true;
}

bool ModMain::DebugSpawnBreakableSyncTarget(std::string& detail, bool scalable)
{
    std::string reason;
    IEntity* entity = nullptr;
    CArkBreakable* breakable = GetDebugBreakable(&entity, reason);
    if (!breakable)
    {
        if (!gEnv || !gEnv->pEntitySystem ||
            !ArkPlayer::GetInstancePtr() || !ArkPlayer::GetInstance().GetEntity())
        {
            detail = "debug_breakable_runtime_unavailable";
            return false;
        }

        const uint64_t archetypeId = scalable ? kDebugScalableBreakableArchetype : kDebugBreakableArchetype;
        const EntityGUID guid = scalable ? kDebugScalableBreakableGuid : kDebugBreakableGuid;
        IEntityArchetype* archetype = nullptr;
        const bool hasArchetype = CoopRuntimeGuards::TryGuardedCall(
                "debug breakable GetEntityArchetype",
                [archetypeId]() { return gEnv->pEntitySystem->GetEntityArchetype(archetypeId); },
                archetype,
                &reason) && archetype;
        if (!hasArchetype)
        {
            if (scalable)
            {
                detail = "missing_debug_breakable_archetype_" + std::to_string(archetypeId);
                return false;
            }
            breakable = FindNearestAuthoredBreakable(&entity, reason);
        }
        else
        {
            IEntity* playerEntity = ArkPlayer::GetInstance().GetEntity();
            SEntitySpawnParams params;
            params.guid = guid;
            params.sName = scalable ? "CoopScalableBreakableSyncTest" : "CoopBreakableSyncTest";
            params.pArchetype = archetype;
            params.pClass = archetype->GetClass();
            params.vPosition = playerEntity->GetWorldPos() + playerEntity->GetWorldRotation().GetColumn1() * 3.0f;
            params.qRotation = playerEntity->GetWorldRotation();
            if (!CoopRuntimeGuards::TryGuardedCall(
                    "debug breakable SpawnEntityFromArchetype",
                    [&params, archetype]() { return gEnv->pEntitySystem->SpawnEntityFromArchetype(archetype, params, true); },
                    entity,
                    &reason) ||
                !entity)
            {
                if (scalable)
                {
                    detail = "debug_breakable_spawn_failed_reason_" + BreakableStatusToken(reason);
                    return false;
                }
                breakable = FindNearestAuthoredBreakable(&entity, reason);
            }
            else
            {
                breakable = GetBreakableExtension(*entity, &reason);
            }
        }
    }

    if (!breakable || !entity)
    {
        detail = "debug_breakable_extension_failed_reason_" + BreakableStatusToken(reason);
        return false;
    }

    float health = 0.0f;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "debug breakable read spawned health",
            [breakable]() { return breakable->m_health; },
            health,
            &reason))
    {
        detail = "debug_breakable_health_read_failed_reason_" + BreakableStatusToken(reason);
        return false;
    }
    detail = "debug_breakable_entity_" + std::to_string(entity->GetId()) +
        "_guid_" + std::to_string(static_cast<uint64_t>(entity->GetGuid())) +
        "_health_" + std::to_string(health);
    return true;
}

bool ModMain::DebugProbeBreakableSyncTarget(std::string& detail, uint64_t targetGuid)
{
    std::string reason;
    IEntity* entity = nullptr;
    CArkBreakable* breakable = targetGuid != 0 ?
        GetBreakableByGuid(targetGuid, &entity, reason) : GetDebugBreakable(&entity, reason);
    float health = 0.0f;
    if (!breakable || !entity ||
        !CoopRuntimeGuards::TryGuardedCall(
            "debug breakable probe health",
            [breakable]() { return breakable->m_health; },
            health,
            &reason))
    {
        detail = "debug_breakable_probe_failed_reason_" + BreakableStatusToken(reason);
        return false;
    }
    detail = "debug_breakable_entity_" + std::to_string(entity->GetId()) +
        "_guid_" + std::to_string(static_cast<uint64_t>(entity->GetGuid())) +
        "_health_" + std::to_string(health) +
        "_broken_" + std::to_string(health <= kBreakableHealthEpsilon ? 1 : 0);
    return true;
}

bool ModMain::DebugSetBreakableSyncTargetHealth(float health, std::string& detail, uint64_t targetGuid)
{
    if (!std::isfinite(health) || health < 0.0f || health > kMaxHalfFloat)
    {
        detail = "invalid_debug_breakable_health";
        return false;
    }

    std::string reason;
    IEntity* entity = nullptr;
    CArkBreakable* breakable = targetGuid != 0 ?
        GetBreakableByGuid(targetGuid, &entity, reason) : GetDebugBreakable(&entity, reason);
    if (!breakable || !entity ||
        !CoopRuntimeGuards::TryGuardedVoidCall(
            "debug breakable SetHealth",
            [breakable, health]() { breakable->SetHealth(health); },
            &reason))
    {
        detail = "debug_breakable_set_failed_reason_" + BreakableStatusToken(reason);
        return false;
    }
    return DebugProbeBreakableSyncTarget(detail, targetGuid);
}
