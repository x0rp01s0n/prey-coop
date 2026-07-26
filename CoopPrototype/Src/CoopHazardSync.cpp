#include "ModMain.h"
#include "CoopRuntimeGuards.h"
#include "CoopSerialSequence.h"

#include <cmath>
#include <cstring>
#include <limits>

#include <Prey/CryEntitySystem/IEntitySystem.h>
#include <Prey/CryGame/IGame.h>
#include <Prey/CryGame/IGameFramework.h>
#include <Prey/CrySystem/ISystem.h>
#include <Prey/Ark/arksignalsystemdata.h>
#include <Prey/GameDll/ark/arkeffectutils.h>
#include <Prey/GameDll/ark/environment/ArkLeakable.h>
#include <Prey/GameDll/ark/environment/ArkExplosiveTank.h>
#include <Prey/GameDll/ark/environment/ArkSurfaceHazard.h>
#include <Prey/GameDll/ark/environment/ArkAreaHazard.h>
#include <Prey/GameDll/ark/environment/ArkElectricalBox.h>
#include <Prey/GameDll/ark/environment/ArkChargeTrap.h>
#include <Prey/GameDll/ark/ArkRepairableObject.h>
#include <Prey/GameDll/ark/arkgravshaftentity.h>
#include <Prey/GameDll/ark/player/ArkPlayer.h>
#include <Prey/GameDll/ark/player/psipower/ArkPsiPowerComponent.h>
#include <Prey/GameDll/ark/player/psipower/ArkPlayerLiftEntityZeroG.h>
#include <Prey/GameDll/ark/player/psipower/ArkPsiPowerLift.h>
#include <Prey/GameDll/ark/weapons/arkprojectilerecyclergrenade.h>
#include <Prey/GameDll/ark/weapons/arkprojectile.h>
#include <Prey/GameDll/ark/weapons/arkprojectilegrenade.h>

namespace
{
// ArkLeakable::RepairLeak returns an MSVC vector iterator through a hidden RDX
// result pointer. The generated STL wrapper uses our build's iterator layout,
// which is not ABI-compatible with Prey's raw release iterator at this call.
static inline auto s_arkLeakableRepairLeakRaw =
    PreyFunction<void(ArkLeakable*, ArkLeakable::LeakInfo**, ArkLeakable::LeakInfo*)>(0x13C81A0);

constexpr uint64_t kEmpGrenadeProjectileArchetype = 3149325216973951492ull;
constexpr uint64_t kLureGrenadeProjectileArchetype = 3149325216973952309ull;
constexpr uint64_t kNullwaveGrenadeProjectileArchetype = 3149325216973952349ull;
constexpr uint64_t kRecyclerGrenadeProjectileArchetype = 3149325216973952536ull;
constexpr uint64_t kRecyclerTestChamberProjectileArchetype = 3149325216973956419ull;
constexpr uint64_t kRecyclerTrapProjectileArchetype = 3149325216989684629ull;

CoopProtocol::HazardEventKind GrenadeResultKind(uint64_t archetypeId)
{
    switch (archetypeId)
    {
    case kEmpGrenadeProjectileArchetype: return CoopProtocol::HazardEventKind::EmpDetonate;
    case kLureGrenadeProjectileArchetype: return CoopProtocol::HazardEventKind::LureDetonate;
    case kNullwaveGrenadeProjectileArchetype: return CoopProtocol::HazardEventKind::NullwaveDetonate;
    default: return static_cast<CoopProtocol::HazardEventKind>(0);
    }
}

bool IsGrenadeResultKind(CoopProtocol::HazardEventKind kind)
{
    return kind == CoopProtocol::HazardEventKind::RecyclerDetonate ||
        kind == CoopProtocol::HazardEventKind::EmpDetonate ||
        kind == CoopProtocol::HazardEventKind::LureDetonate ||
        kind == CoopProtocol::HazardEventKind::NullwaveDetonate;
}

bool GrenadeResultArchetypeMatches(CoopProtocol::HazardEventKind kind, uint64_t archetypeId)
{
    switch (kind)
    {
    case CoopProtocol::HazardEventKind::RecyclerDetonate:
        return archetypeId == kRecyclerGrenadeProjectileArchetype ||
            archetypeId == kRecyclerTestChamberProjectileArchetype ||
            archetypeId == kRecyclerTrapProjectileArchetype;
    case CoopProtocol::HazardEventKind::EmpDetonate:
        return archetypeId == kEmpGrenadeProjectileArchetype;
    case CoopProtocol::HazardEventKind::LureDetonate:
        return archetypeId == kLureGrenadeProjectileArchetype;
    case CoopProtocol::HazardEventKind::NullwaveDetonate:
        return archetypeId == kNullwaveGrenadeProjectileArchetype;
    default:
        return false;
    }
}

bool IsFiniteHazardTransform(const CoopProtocol::HazardEventPacket& packet)
{
    const float values[] = {
        packet.px, packet.py, packet.pz,
        packet.qw, packet.qx, packet.qy, packet.qz,
    };
    for (float value : values)
    {
        if (!std::isfinite(value))
            return false;
    }
    const float rotationLengthSq =
        packet.qw * packet.qw + packet.qx * packet.qx +
        packet.qy * packet.qy + packet.qz * packet.qz;
    return rotationLengthSq > 0.25f && rotationLengthSq < 2.25f;
}

void ApplyRepairableFortifiedState(ArkRepairable& repairable, bool fortified)
{
    const bool wasFortified = repairable.m_bFortified;
    repairable.m_bFortified = fortified;
    if (fortified && !wasFortified)
        repairable.m_listener.OnFortified();
}

bool IsFiniteLeakPayload(const CoopProtocol::HazardEventPacket& packet)
{
    const float values[] = {
        packet.px, packet.py, packet.pz,
        packet.dx, packet.dy, packet.dz,
        packet.scalar,
    };
    for (float value : values)
    {
        if (!std::isfinite(value))
            return false;
    }
    const Vec3 direction(packet.dx, packet.dy, packet.dz);
    return packet.scalar >= 0.0f && packet.scalar <= 1000.0f &&
        direction.GetLengthSquared() > 0.01f && direction.GetLengthSquared() < 4.0f;
}

bool IsFinitePsiLiftPayload(const CoopProtocol::HazardEventPacket& packet)
{
    const Vec3 axis(packet.dx, packet.dy, packet.dz);
    const unsigned level = packet.flags & 0xffu;
    return level < 3u && packet.scalar >= 0.25f && packet.scalar <= 50.0f &&
        axis.GetLengthSquared() > 0.25f && axis.GetLengthSquared() < 2.25f;
}

ArkLeakable* ResolveLeakableByGuid(uint64_t guid, uint64_t archetypeId, std::string& reason)
{
    if (guid == 0 || !gEnv || !gEnv->pEntitySystem || !gEnv->pGame)
    {
        reason = "missing_runtime_or_guid";
        return nullptr;
    }

    EntityId entityId = INVALID_ENTITYID;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "hazard leak FindEntityByGuid",
            [guid]() { return gEnv->pEntitySystem->FindEntityByGuid(static_cast<EntityGUID>(guid)); },
            entityId,
            &reason) || entityId == INVALID_ENTITYID)
    {
        return nullptr;
    }

    IEntity* entity = nullptr;
    IEntityArchetype* archetype = nullptr;
    uint64_t localArchetypeId = 0;
    if (!CoopRuntimeGuards::TryGuardedCall("hazard leak GetEntity", [entityId]() { return gEnv->pEntitySystem->GetEntity(entityId); }, entity, &reason) ||
        !entity ||
        !CoopRuntimeGuards::TryGuardedCall("hazard leak GetArchetype", [entity]() { return entity->GetArchetype(); }, archetype, &reason) ||
        !archetype ||
        !CoopRuntimeGuards::TryGuardedCall("hazard leak archetype GetId", [archetype]() { return archetype->GetId(); }, localArchetypeId, &reason) ||
        localArchetypeId != archetypeId)
    {
        reason = "leak_archetype_mismatch";
        return nullptr;
    }

    IGameFramework* framework = gEnv->pGame->GetIGameFramework();
    IGameObject* gameObject = nullptr;
    IGameObjectExtension* extension = nullptr;
    IGameObjectSystem::ExtensionID extensionId = 0;
    if (!framework ||
        !CoopRuntimeGuards::TryGuardedCall("hazard leak GetGameObject", [framework, entityId]() { return framework->GetGameObject(entityId); }, gameObject, &reason) ||
        !gameObject)
    {
        return nullptr;
    }
    if (CoopRuntimeGuards::TryGuardedCall(
            "hazard leak GetExtensionId",
            [gameObject]() { return gameObject->GetExtensionId("ArkLeakable"); },
            extensionId,
            &reason) && extensionId != 0 &&
        CoopRuntimeGuards::TryGuardedCall(
            "hazard leak QueryExtension id",
            [gameObject, extensionId]() { return gameObject->QueryExtension(extensionId); },
            extension,
            &reason) && extension)
    {
        return static_cast<ArkLeakable*>(extension);
    }
    extension = nullptr;
    return CoopRuntimeGuards::TryGuardedCall(
            "hazard leak QueryExtension name",
            [gameObject]() { return gameObject->QueryExtension("ArkLeakable"); },
            extension,
            &reason) && extension
        ? static_cast<ArkLeakable*>(extension)
        : nullptr;
}

template <typename T>
T* ResolveHazardExtensionByGuid(
    uint64_t guid,
    uint64_t archetypeId,
    const char* extensionName,
    std::string& reason)
{
    if (guid == 0 || !extensionName || !gEnv ||
        !gEnv->pEntitySystem || !gEnv->pGame)
    {
        reason = "missing_runtime_or_identity";
        return nullptr;
    }

    EntityId entityId = INVALID_ENTITYID;
    IEntity* entity = nullptr;
    IEntityArchetype* archetype = nullptr;
    uint64_t localArchetypeId = 0;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "persistent hazard FindEntityByGuid",
            [guid]() { return gEnv->pEntitySystem->FindEntityByGuid(static_cast<EntityGUID>(guid)); },
            entityId,
            &reason) ||
        entityId == INVALID_ENTITYID ||
        !CoopRuntimeGuards::TryGuardedCall(
            "persistent hazard GetEntity",
            [entityId]() { return gEnv->pEntitySystem->GetEntity(entityId); },
            entity,
            &reason) ||
        !entity ||
        !CoopRuntimeGuards::TryGuardedCall(
            "persistent hazard GetArchetype",
            [entity]() { return entity->GetArchetype(); },
            archetype,
            &reason))
    {
        reason = "persistent_hazard_identity_mismatch";
        return nullptr;
    }
    if (archetype &&
        !CoopRuntimeGuards::TryGuardedCall(
            "persistent hazard archetype GetId",
            [archetype]() { return archetype->GetId(); },
            localArchetypeId,
            &reason))
    {
        reason = "persistent_hazard_identity_mismatch";
        return nullptr;
    }
    // Authored/save-restored environmental objects can legitimately have no
    // runtime archetype pointer. Exact GUID and concrete entity class remain
    // stable; validate the archetype as an additional identity component only
    // when the sender supplied one.
    if (archetypeId != 0 && localArchetypeId != archetypeId)
    {
        reason = "persistent_hazard_identity_mismatch";
        return nullptr;
    }

    IGameFramework* framework = gEnv->pGame->GetIGameFramework();
    IGameObject* gameObject = nullptr;
    IGameObjectExtension* extension = nullptr;
    IGameObjectSystem::ExtensionID extensionId = 0;
    if (!framework ||
        !CoopRuntimeGuards::TryGuardedCall(
            "persistent hazard GetGameObject",
            [framework, entityId]() { return framework->GetGameObject(entityId); },
            gameObject,
            &reason) ||
        !gameObject)
    {
        reason = "persistent_hazard_extension_missing";
        return nullptr;
    }

    IEntityClass* entityClass = nullptr;
    const char* className = nullptr;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "persistent hazard GetClass",
            [entity]() { return entity->GetClass(); },
            entityClass,
            &reason) ||
        !entityClass ||
        !CoopRuntimeGuards::TryGuardedCall(
            "persistent hazard class GetName",
            [entityClass]() { return entityClass->GetName(); },
            className,
            &reason) ||
        !className || std::strcmp(className, extensionName) != 0)
    {
        reason = "persistent_hazard_class_mismatch";
        return nullptr;
    }

    if (CoopRuntimeGuards::TryGuardedCall(
            "persistent hazard GetExtensionId",
            [gameObject, extensionName]() { return gameObject->GetExtensionId(extensionName); },
            extensionId,
            &reason) &&
        extensionId != 0 &&
        CoopRuntimeGuards::TryGuardedCall(
            "persistent hazard QueryExtension id",
            [gameObject, extensionId]() { return gameObject->QueryExtension(extensionId); },
            extension,
            &reason) &&
        extension)
    {
        return static_cast<T*>(extension);
    }

    extension = nullptr;
    if (CoopRuntimeGuards::TryGuardedCall(
            "persistent hazard QueryExtension name",
            [gameObject, extensionName]() { return gameObject->QueryExtension(extensionName); },
            extension,
            &reason) &&
        extension)
    {
        return static_cast<T*>(extension);
    }

    // Surface/area/leak classes share ArkEnvironmentalObject's registered
    // game-object extension; their concrete entity class selects the subtype.
    constexpr const char* kEnvironmentalExtension = "ArkEnvironmentalObject";
    extension = nullptr;
    extensionId = 0;
    if (CoopRuntimeGuards::TryGuardedCall(
            "persistent hazard base GetExtensionId",
            [gameObject]() { return gameObject->GetExtensionId(kEnvironmentalExtension); },
            extensionId,
            &reason) &&
        extensionId != 0 &&
        CoopRuntimeGuards::TryGuardedCall(
            "persistent hazard base QueryExtension id",
            [gameObject, extensionId]() { return gameObject->QueryExtension(extensionId); },
            extension,
            &reason) &&
        extension)
    {
        return static_cast<T*>(extension);
    }

    extension = nullptr;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "persistent hazard base QueryExtension name",
            [gameObject]() { return gameObject->QueryExtension(kEnvironmentalExtension); },
            extension,
            &reason) ||
        !extension)
    {
        reason = "persistent_hazard_extension_missing";
        return nullptr;
    }
    return static_cast<T*>(extension);
}

bool ApplyGravShaftAggregateState(
    CArkGravShaftEntity* shaft,
    uint16_t state,
    std::string& reason)
{
    constexpr uint16_t kValidStateMask =
        CoopProtocol::kGravShaftStateEnabled |
        CoopProtocol::kGravShaftStateBroken |
        CoopProtocol::kGravShaftStateDisrupted |
        CoopProtocol::kGravShaftStateReversed;
    if (!shaft || (state & ~kValidStateMask) != 0)
    {
        reason = "invalid_grav_shaft_state";
        return false;
    }

    const bool enabled = (state & CoopProtocol::kGravShaftStateEnabled) != 0;
    const bool broken = (state & CoopProtocol::kGravShaftStateBroken) != 0;
    const bool disrupted = (state & CoopProtocol::kGravShaftStateDisrupted) != 0;
    const bool reversed = (state & CoopProtocol::kGravShaftStateReversed) != 0;
    const bool called = CoopRuntimeGuards::TryGuardedVoidCall(
        "grav shaft aggregate native apply",
        [shaft, enabled, broken, disrupted, reversed]()
        {
            if (shaft->m_bEnabled != enabled)
            {
                IEntity* entity = shaft->GetEntity();
                if (entity)
                {
                    SEntityEvent event(ENTITY_EVENT_SCRIPT_EVENT);
                    event.nParam[0] = reinterpret_cast<INT_PTR>(enabled ? "Enable" : "Disable");
                    entity->SendEvent(event);
                }
            }
            if (shaft->m_repairable.m_bBroken != broken)
                ArkRepairableLite::FSetBroken(&shaft->m_repairable, broken, true);
            if (shaft->m_bDisrupted != disrupted)
                shaft->SetDisrupted(disrupted);
            if (shaft->m_bReversed != reversed)
            {
                shaft->ReverseTravelDirection();
                // Native ProcessEvent toggles this field immediately after
                // ReverseTravelDirection has rebuilt the vectors/nav links.
                shaft->m_bReversed = reversed;
            }
        },
        &reason);
    return called &&
        shaft->m_bEnabled == enabled &&
        shaft->m_repairable.m_bBroken == broken &&
        shaft->m_bDisrupted == disrupted &&
        shaft->m_bReversed == reversed;
}
}

bool ModMain::RepairFirstNativeLeak(ArkLeakable* leakable, const char* context, std::string* detail)
{
    if (!leakable || leakable->m_leaks.empty())
        return false;

    ArkLeakable::LeakInfo* const first = leakable->m_leaks.data();
    ArkLeakable::LeakInfo* next = nullptr;
    const size_t before = leakable->m_leaks.size();
    const bool called = CoopRuntimeGuards::TryGuardedVoidCall(
        context ? context : "ArkLeakable raw RepairLeak",
        [leakable, first, &next]() { s_arkLeakableRepairLeakRaw(leakable, &next, first); },
        detail);
    return called && leakable->m_leaks.size() < before;
}

uint64_t ModMain::BuildHazardEventId(
    uint16_t eventKind,
    uint64_t archetypeId,
    const Vec3& position,
    uint32_t sequence) const
{
    uint64_t hash = 14695981039346656037ull;
    auto mix = [&hash](uint64_t value)
    {
        for (int byte = 0; byte < 8; ++byte)
        {
            hash ^= (value >> (byte * 8)) & 0xffu;
            hash *= 1099511628211ull;
        }
    };
    mix(GetLocalAccountToken());
    mix(CurrentHostSaveKeyHash());
    mix(m_localWorldEpoch);
    mix(m_localLevelId);
    mix(eventKind);
    mix(archetypeId);
    mix(sequence);
    mix(static_cast<uint32_t>(std::lround(position.x * 100.0f)));
    mix(static_cast<uint32_t>(std::lround(position.y * 100.0f)));
    mix(static_cast<uint32_t>(std::lround(position.z * 100.0f)));
    return hash == 0 ? 1 : hash;
}

bool ModMain::SendHazardEventTo(
    const CoopProtocol::HazardEventPacket& packet,
    uint32_t address,
    uint16_t port,
    const char* failurePrefix)
{
    if (!SendReliablePayloadTo(
            static_cast<uint16_t>(CoopProtocol::PacketType::HazardEvent),
            &packet,
            sizeof(packet),
            address,
            port,
            failurePrefix))
    {
        return false;
    }
    ++m_hazardEventSent;
    return true;
}

void ModMain::OnNativePlayerGrenadeDetonated(CArkProjectileGrenade* grenade, const char* reason)
{
    if (!grenade || m_hazardEventApplyDepth != 0 ||
        m_networkMode == CoopNetworkMode::Off || !m_hasRemoteEndpoint || !IsSessionGameplayReady())
    {
        return;
    }

    IEntity* entity = nullptr;
    IEntityArchetype* archetype = nullptr;
    uint64_t archetypeId = 0;
    EntityId ownerId = INVALID_ENTITYID;
    Vec3 position(ZERO);
    Quat rotation(IDENTITY);
    std::string guardReason;
    const IEntity* localPlayerEntity = ArkPlayer::GetInstancePtr()
        ? ArkPlayer::GetInstance().GetEntity()
        : nullptr;
    const EntityId localPlayerId = localPlayerEntity
        ? localPlayerEntity->GetId()
        : INVALID_ENTITYID;
    if (localPlayerId == INVALID_ENTITYID ||
        !CoopRuntimeGuards::TryGuardedCall("hazard grenade owner", [grenade]() { return static_cast<EntityId>(grenade->m_ownerId); }, ownerId, &guardReason) ||
        ownerId != localPlayerId ||
        !CoopRuntimeGuards::TryGuardedCall("hazard grenade GetEntity", [grenade]() { return grenade->GetEntity(); }, entity, &guardReason) ||
        !entity ||
        !CoopRuntimeGuards::TryGuardedCall("hazard grenade GetArchetype", [entity]() { return entity->GetArchetype(); }, archetype, &guardReason) ||
        !archetype ||
        !CoopRuntimeGuards::TryGuardedCall("hazard grenade archetype GetId", [archetype]() { return archetype->GetId(); }, archetypeId, &guardReason) ||
        !CoopRuntimeGuards::TryGuardedCall("hazard grenade GetWorldPos", [entity]() { return entity->GetWorldPos(); }, position, &guardReason) ||
        !CoopRuntimeGuards::TryGuardedCall("hazard grenade GetWorldRotation", [entity]() { return entity->GetWorldRotation(); }, rotation, &guardReason))
    {
        return;
    }

    const CoopProtocol::HazardEventKind kind = GrenadeResultKind(archetypeId);
    if (!IsGrenadeResultKind(kind) || kind == CoopProtocol::HazardEventKind::RecyclerDetonate)
        return;

    CoopProtocol::HazardEventPacket packet;
    packet.sequence = CoopSerialSequence::Advance(m_hazardEventSequence);
    packet.worldEpoch = m_localWorldEpoch;
    packet.eventId = BuildHazardEventId(static_cast<uint16_t>(kind), archetypeId, position, packet.sequence);
    packet.hostSaveKeyHash = CurrentHostSaveKeyHash();
    packet.areaId = m_localLevelId;
    packet.sourcePeerHash = GetLocalAccountToken();
    packet.archetypeId = archetypeId;
    packet.eventKind = static_cast<uint16_t>(kind);
    packet.px = position.x;
    packet.py = position.y;
    packet.pz = position.z;
    packet.qw = rotation.w;
    packet.qx = rotation.v.x;
    packet.qy = rotation.v.y;
    packet.qz = rotation.v.z;

    if (!SendHazardEventTo(packet, m_remoteAddress, m_remotePort, "player grenade result send failed"))
    {
        ++m_hazardEventDropped;
        return;
    }
    m_lastHazardEvent =
        "sent_grenade_result_kind_" + std::to_string(packet.eventKind) +
        "_event_" + std::to_string(packet.eventId) +
        "_reason_" + (reason && reason[0] ? reason : "-");
}

bool ModMain::DebugDetonatePlayerGrenadeResult(const std::string& kindName, std::string& detail)
{
    uint64_t archetypeId = 0;
    if (kindName == "emp")
        archetypeId = kEmpGrenadeProjectileArchetype;
    else if (kindName == "lure")
        archetypeId = kLureGrenadeProjectileArchetype;
    else if (kindName == "nullwave")
        archetypeId = kNullwaveGrenadeProjectileArchetype;
    else
    {
        detail = "unknown_kind_" + kindName;
        return false;
    }

    if (!gEnv || !gEnv->pEntitySystem || !ArkPlayer::GetInstancePtr())
    {
        detail = "game_not_ready";
        return false;
    }
    IEntity* playerEntity = ArkPlayer::GetInstance().GetEntity();
    if (!playerEntity)
    {
        detail = "player_missing";
        return false;
    }

    IEntityArchetype* archetype = nullptr;
    std::string guardReason;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "debug grenade GetEntityArchetype",
            [archetypeId]() { return gEnv->pEntitySystem->GetEntityArchetype(archetypeId); },
            archetype,
            &guardReason) ||
        !archetype)
    {
        detail = "archetype_missing_" + std::to_string(archetypeId);
        return false;
    }

    SEntitySpawnParams params;
    const std::string entityName =
        "CoopDebugGrenade_" + kindName + "_" +
        std::to_string(CoopSerialSequence::Advance(m_hazardEventSequence));
    params.sName = entityName.c_str();
    params.pArchetype = archetype;
    params.pClass = archetype->GetClass();
    params.vPosition = playerEntity->GetWorldPos() + playerEntity->GetWorldRotation().GetColumn1() * 3.0f;
    params.qRotation = playerEntity->GetWorldRotation();
    params.vScale = Vec3(1.0f);

    IEntity* entity = nullptr;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "debug grenade SpawnEntityFromArchetype",
            [&params, archetype]() { return gEnv->pEntitySystem->SpawnEntityFromArchetype(archetype, params, true); },
            entity,
            &guardReason) ||
        !entity)
    {
        detail = "spawn_failed_" + guardReason;
        return false;
    }

    CArkProjectile* projectile = nullptr;
    CoopRuntimeGuards::TryGuardedCall(
        "debug grenade GetProjectileFromEntityId",
        [entity]() { return CArkProjectile::GetProjectileFromEntityId(entity->GetId()); },
        projectile,
        &guardReason);
    if (!projectile)
    {
        gEnv->pEntitySystem->RemoveEntity(entity->GetId(), true);
        detail = "projectile_missing_" + guardReason;
        return false;
    }

    if (!CoopRuntimeGuards::TryGuardedVoidCall(
            "debug grenade set owner",
            [projectile, playerEntity]() { projectile->m_ownerId = playerEntity->GetId(); },
            &guardReason))
    {
        detail = "owner_failed_" + guardReason;
        return false;
    }
    CArkProjectileGrenade* grenade = static_cast<CArkProjectileGrenade*>(projectile);
    const bool detonated = CoopRuntimeGuards::TryGuardedVoidCall(
        "debug grenade DoDetonation",
        [grenade]() { grenade->DoDetonation(); },
        &guardReason);
    detail =
        "kind_" + kindName +
        "_entity_" + std::to_string(entity->GetId()) +
        "_archetype_" + std::to_string(archetypeId) +
        "_detonated_" + std::to_string(detonated ? 1 : 0) +
        "_reason_" + (guardReason.empty() ? std::string("-") : guardReason);
    return detonated;
}

void ModMain::OnNativeRecyclerGrenadeDetonated(
    CArkProjectileRecyclerGrenade* grenade,
    bool result,
    const char* reason)
{
    if (!result || !grenade || m_hazardEventApplyDepth != 0 ||
        m_networkMode == CoopNetworkMode::Off || !m_hasRemoteEndpoint || !IsSessionGameplayReady())
    {
        return;
    }

    IEntity* entity = nullptr;
    IEntityArchetype* archetype = nullptr;
    EntityId entityId = INVALID_ENTITYID;
    uint64_t archetypeId = 0;
    Vec3 position(ZERO);
    Quat rotation(IDENTITY);
    std::string guardReason;
    if (!CoopRuntimeGuards::TryGuardedCall("hazard recycler GetEntity", [grenade]() { return grenade->GetEntity(); }, entity, &guardReason) ||
        !entity ||
        !CoopRuntimeGuards::TryGuardedCall("hazard recycler GetId", [entity]() { return entity->GetId(); }, entityId, &guardReason) ||
        entityId == INVALID_ENTITYID ||
        !CoopRuntimeGuards::TryGuardedCall("hazard recycler GetArchetype", [entity]() { return entity->GetArchetype(); }, archetype, &guardReason) ||
        !archetype ||
        !CoopRuntimeGuards::TryGuardedCall("hazard recycler archetype GetId", [archetype]() { return archetype->GetId(); }, archetypeId, &guardReason) ||
        archetypeId == 0 ||
        !CoopRuntimeGuards::TryGuardedCall("hazard recycler GetWorldPos", [entity]() { return entity->GetWorldPos(); }, position, &guardReason) ||
        !CoopRuntimeGuards::TryGuardedCall("hazard recycler GetWorldRotation", [entity]() { return entity->GetWorldRotation(); }, rotation, &guardReason))
    {
        ++m_hazardEventDropped;
        m_lastHazardEvent = "recycler_capture_failed_" + guardReason;
        return;
    }
    if (m_remoteHazardEntityIds.find(entityId) != m_remoteHazardEntityIds.end() ||
        !m_sentLocalHazardEntityIds.insert(entityId).second)
    {
        return;
    }

    CoopProtocol::HazardEventPacket packet;
    packet.sequence = CoopSerialSequence::Advance(m_hazardEventSequence);
    packet.worldEpoch = m_localWorldEpoch;
    packet.eventId = BuildHazardEventId(
        static_cast<uint16_t>(CoopProtocol::HazardEventKind::RecyclerDetonate),
        archetypeId,
        position,
        packet.sequence);
    packet.hostSaveKeyHash = CurrentHostSaveKeyHash();
    packet.areaId = m_localLevelId;
    packet.sourcePeerHash = GetLocalAccountToken();
    packet.archetypeId = archetypeId;
    packet.eventKind = static_cast<uint16_t>(CoopProtocol::HazardEventKind::RecyclerDetonate);
    packet.px = position.x;
    packet.py = position.y;
    packet.pz = position.z;
    packet.qw = rotation.w;
    packet.qx = rotation.v.x;
    packet.qy = rotation.v.y;
    packet.qz = rotation.v.z;

    if (!SendHazardEventTo(packet, m_remoteAddress, m_remotePort, "hazard recycler send failed"))
    {
        m_sentLocalHazardEntityIds.erase(entityId);
        ++m_hazardEventDropped;
        return;
    }
    m_lastHazardEvent =
        "sent_recycler_event_" + std::to_string(packet.eventId) +
        "_arch_" + std::to_string(archetypeId) +
        "_reason_" + (reason && reason[0] ? reason : "-");
}

bool ModMain::QueueLocalLeakHazardEvent(
    ArkLeakable* leakable,
    CoopProtocol::HazardEventKind kind,
    const Vec3& position,
    const Vec3& direction,
    float length,
    uint16_t flags,
    const char* reason)
{
    if (!leakable || m_hazardEventApplyDepth != 0 || m_networkMode == CoopNetworkMode::Off ||
        !m_hasRemoteEndpoint || !IsSessionGameplayReady())
    {
        return false;
    }

    IEntity* entity = nullptr;
    IEntityArchetype* archetype = nullptr;
    uint64_t guid = 0;
    uint64_t archetypeId = 0;
    std::string guardReason;
    if (!CoopRuntimeGuards::TryGuardedCall("hazard leak GetEntity", [leakable]() { return leakable->GetEntity(); }, entity, &guardReason) ||
        !entity ||
        !CoopRuntimeGuards::TryGuardedCall("hazard leak GetGuid", [entity]() { return entity->GetGuid(); }, guid, &guardReason) || guid == 0 ||
        !CoopRuntimeGuards::TryGuardedCall("hazard leak GetArchetype", [entity]() { return entity->GetArchetype(); }, archetype, &guardReason) ||
        !archetype ||
        !CoopRuntimeGuards::TryGuardedCall("hazard leak archetype GetId", [archetype]() { return archetype->GetId(); }, archetypeId, &guardReason) ||
        archetypeId == 0 || !std::isfinite(length))
    {
        ++m_hazardEventDropped;
        m_lastHazardEvent = "leak_capture_failed_" + guardReason;
        return false;
    }

    CoopProtocol::HazardEventPacket packet;
    packet.sequence = CoopSerialSequence::Advance(m_hazardEventSequence);
    packet.worldEpoch = m_localWorldEpoch;
    packet.eventId = BuildHazardEventId(static_cast<uint16_t>(kind), guid, position, packet.sequence);
    packet.hostSaveKeyHash = CurrentHostSaveKeyHash();
    packet.areaId = m_localLevelId;
    packet.sourcePeerHash = GetLocalAccountToken();
    packet.archetypeId = archetypeId;
    packet.targetGuid = guid;
    packet.eventKind = static_cast<uint16_t>(kind);
    packet.flags = flags;
    packet.px = position.x;
    packet.py = position.y;
    packet.pz = position.z;
    packet.dx = direction.x;
    packet.dy = direction.y;
    packet.dz = direction.z;
    packet.scalar = length;

    if (!SendHazardEventTo(packet, m_remoteAddress, m_remotePort, "hazard leak send failed"))
    {
        ++m_hazardEventDropped;
        return false;
    }
    m_lastHazardEvent =
        "sent_leak_kind_" + std::to_string(packet.eventKind) +
        "_event_" + std::to_string(packet.eventId) +
        "_guid_" + std::to_string(guid) +
        "_reason_" + (reason && reason[0] ? reason : "-");
    return true;
}

bool ModMain::QueueLocalPersistentHazardState(
    IEntity* entity,
    CoopProtocol::HazardEventKind kind,
    uint16_t state,
    const char* reason)
{
    if (!entity || m_hazardEventApplyDepth != 0 || m_networkMode == CoopNetworkMode::Off ||
        !m_hasRemoteEndpoint || !IsSessionGameplayReady())
    {
        return false;
    }

    uint64_t guid = 0;
    IEntityArchetype* archetype = nullptr;
    uint64_t archetypeId = 0;
    Vec3 position(ZERO);
    Quat rotation(IDENTITY);
    std::string guardReason;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "persistent hazard GetGuid",
            [entity]() -> EntityGUID { return entity->GetGuid(); },
            guid,
            &guardReason) ||
        guid == 0 ||
        !CoopRuntimeGuards::TryGuardedCall(
            "persistent hazard GetArchetype",
            [entity]() { return entity->GetArchetype(); },
            archetype,
            &guardReason) ||
        !CoopRuntimeGuards::TryGuardedCall(
            "persistent hazard GetWorldPos",
            [entity]() { return entity->GetWorldPos(); },
            position,
            &guardReason) ||
        !CoopRuntimeGuards::TryGuardedCall(
            "persistent hazard GetWorldRotation",
            [entity]() { return entity->GetWorldRotation(); },
            rotation,
            &guardReason))
    {
        ++m_hazardEventDropped;
        m_lastHazardEvent = "persistent_hazard_capture_failed_" + guardReason;
        return false;
    }
    if (archetype &&
        !CoopRuntimeGuards::TryGuardedCall(
            "persistent hazard archetype GetId",
            [archetype]() { return archetype->GetId(); },
            archetypeId,
            &guardReason))
    {
        ++m_hazardEventDropped;
        m_lastHazardEvent = "persistent_hazard_capture_failed_" + guardReason;
        return false;
    }

    CoopProtocol::HazardEventPacket packet;
    packet.sequence = CoopSerialSequence::Advance(m_hazardEventSequence);
    packet.worldEpoch = m_localWorldEpoch;
    packet.eventId = BuildHazardEventId(static_cast<uint16_t>(kind), guid, position, packet.sequence);
    packet.hostSaveKeyHash = CurrentHostSaveKeyHash();
    packet.areaId = m_localLevelId;
    packet.sourcePeerHash = GetLocalAccountToken();
    packet.archetypeId = archetypeId;
    packet.targetGuid = guid;
    packet.eventKind = static_cast<uint16_t>(kind);
    packet.flags = state;
    packet.px = position.x;
    packet.py = position.y;
    packet.pz = position.z;
    packet.qw = rotation.w;
    packet.qx = rotation.v.x;
    packet.qy = rotation.v.y;
    packet.qz = rotation.v.z;

    if (!SendHazardEventTo(packet, m_remoteAddress, m_remotePort, "persistent hazard state send failed"))
    {
        ++m_hazardEventDropped;
        return false;
    }

    m_lastHazardEvent =
        "sent_persistent_hazard_kind_" + std::to_string(packet.eventKind) +
        "_state_" + std::to_string(state) +
        "_guid_" + std::to_string(guid) +
        "_reason_" + (reason && reason[0] ? reason : "-");
    return true;
}

void ModMain::OnNativeExplosiveTankExploded(
    ArkExplosiveTank* tank,
    bool changed,
    const char* reason)
{
    if (!changed || !tank)
        return;

    IEntity* entity = nullptr;
    std::string guardReason;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "explosive tank GetEntity",
            [tank]() { return tank->GetEntity(); },
            entity,
            &guardReason) ||
        !entity)
    {
        return;
    }

    uint64_t guid = 0;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "explosive tank GetGuid",
            [entity]() -> EntityGUID { return entity->GetGuid(); },
            guid,
            &guardReason) ||
        guid == 0)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_sentExplosiveTankEventMutex);
        if (!m_sentExplosiveTankEventGuids.insert(guid).second)
            return;
    }

    if (!QueueLocalPersistentHazardState(
            entity,
            CoopProtocol::HazardEventKind::ExplosiveTankExplode,
            static_cast<uint16_t>(EArkExplosiveTankState::Destroyed),
            reason))
    {
        std::lock_guard<std::mutex> lock(m_sentExplosiveTankEventMutex);
        m_sentExplosiveTankEventGuids.erase(guid);
    }
}

void ModMain::OnNativeSurfaceHazardStateChanged(
    ArkSurfaceHazard* hazard,
    uint16_t state,
    bool changed,
    bool observerLocalPlayerSignalRequest,
    const char* reason)
{
    if (!changed || !hazard)
        return;

    const bool localAreaAuthority =
        m_networkMode == CoopNetworkMode::Host || IsClientAreaAuthorityActive();
    // Observer hazards still run native timers and receive neighbour/AI
    // signals. Only a signal sourced by this process's real player may become
    // an authority request; forwarding every OnReceiveSignal transition
    // creates a feedback lifecycle on each observer.
    if (!localAreaAuthority && !observerLocalPlayerSignalRequest)
    {
        ++m_surfaceHazardNonAuthoritySuppressions;
        return;
    }
    if (!localAreaAuthority)
        ++m_surfaceHazardObserverRequests;

    QueueLocalPersistentHazardState(
        hazard->GetEntity(),
        CoopProtocol::HazardEventKind::SurfaceHazardState,
        state,
        reason);
}

void ModMain::OnNativeAreaHazardStateChanged(
    ArkAreaHazard* hazard,
    bool active,
    bool changed,
    const char* reason)
{
    if (changed && hazard)
    {
        QueueLocalPersistentHazardState(
            hazard->GetEntity(),
            CoopProtocol::HazardEventKind::AreaHazardState,
            active ? 1u : 0u,
            reason);
    }
}

void ModMain::OnNativeElectricalBoxStateChanged(
    IEntity* entity,
    bool changed,
    const char* reason)
{
    if (!changed || !entity)
        return;

    std::string guardReason;
    IEntityClass* entityClass = nullptr;
    const char* className = nullptr;
    IEntityArchetype* archetype = nullptr;
    uint64_t archetypeId = 0;
    uint64_t guid = 0;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "electrical box GetClass",
            [entity]() { return entity->GetClass(); },
            entityClass,
            &guardReason) ||
        !entityClass ||
        !CoopRuntimeGuards::TryGuardedCall(
            "electrical box class GetName",
            [entityClass]() { return entityClass->GetName(); },
            className,
            &guardReason) ||
        !className || std::strcmp(className, "ArkElectricalBox") != 0 ||
        !CoopRuntimeGuards::TryGuardedCall(
            "electrical box GetArchetype",
            [entity]() { return entity->GetArchetype(); },
            archetype,
            &guardReason) ||
        !archetype ||
        !CoopRuntimeGuards::TryGuardedCall(
            "electrical box archetype GetId",
            [archetype]() { return archetype->GetId(); },
            archetypeId,
            &guardReason) ||
        !CoopRuntimeGuards::TryGuardedCall(
            "electrical box GetGuid",
            [entity]() -> EntityGUID { return entity->GetGuid(); },
            guid,
            &guardReason) ||
        guid == 0)
        return;

    std::string resolveReason;
    ArkElectricalBox* box = ResolveHazardExtensionByGuid<ArkElectricalBox>(
        guid, archetypeId, "ArkElectricalBox", resolveReason);
    if (!box)
        return;

    uint16_t state = 0;
    if (box->m_bPowered)
        state |= CoopProtocol::kElectricalBoxStatePowered;
    if (box->m_repairable.m_bBroken)
        state |= CoopProtocol::kElectricalBoxStateBroken;
    if (box->m_disruptable.m_bDisrupted)
        state |= CoopProtocol::kElectricalBoxStateDisrupted;
    if (box->m_repairable.m_bFortified)
        state |= CoopProtocol::kElectricalBoxStateFortified;
    QueueLocalPersistentHazardState(
        entity,
        CoopProtocol::HazardEventKind::ElectricalBoxState,
        state,
        reason);
}

void ModMain::OnNativeRepairableStateChanged(
    IEntity* entity,
    bool changed,
    const char* reason)
{
    if (!changed || !entity)
        return;

    IEntityClass* entityClass = nullptr;
    const char* className = nullptr;
    std::string guardReason;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "repairable state GetClass",
            [entity]() { return entity->GetClass(); },
            entityClass,
            &guardReason) ||
        !entityClass ||
        !CoopRuntimeGuards::TryGuardedCall(
            "repairable state class GetName",
            [entityClass]() { return entityClass->GetName(); },
            className,
            &guardReason) ||
        !className)
    {
        return;
    }

    if (std::strcmp(className, "ArkElectricalBox") == 0)
    {
        OnNativeElectricalBoxStateChanged(entity, true, reason);
        return;
    }
    if (std::strcmp(className, "ArkChargeTrap") != 0 &&
        std::strcmp(className, "ArkRepairableObject") != 0)
    {
        return;
    }

    IEntityArchetype* archetype = entity->GetArchetype();
    const uint64_t archetypeId = archetype ? archetype->GetId() : 0;
    ArkRepairable* repairable = nullptr;
    std::string resolveReason;
    if (std::strcmp(className, "ArkChargeTrap") == 0)
    {
        ArkChargeTrap* trap = ResolveHazardExtensionByGuid<ArkChargeTrap>(
            entity->GetGuid(), archetypeId, "ArkChargeTrap", resolveReason);
        repairable = trap ? &trap->m_repairable : nullptr;
    }
    else
    {
        ArkRepairableObject* object = ResolveHazardExtensionByGuid<ArkRepairableObject>(
            entity->GetGuid(), archetypeId, "ArkRepairableObject", resolveReason);
        repairable = object ? &object->m_repairable : nullptr;
    }
    if (!repairable)
        return;

    uint16_t state = repairable->m_bBroken ? CoopProtocol::kRepairableStateBroken : 0u;
    if (repairable->m_bFortified)
        state |= CoopProtocol::kRepairableStateFortified;
    QueueLocalPersistentHazardState(
        entity,
        CoopProtocol::HazardEventKind::RepairableState,
        state,
        reason);
}

void ModMain::OnNativeGravShaftStateChanged(
    IEntity* entity,
    bool changed,
    const char* reason)
{
    if (!changed || !entity)
        return;

    IEntityClass* entityClass = nullptr;
    const char* className = nullptr;
    IEntityArchetype* archetype = nullptr;
    uint64_t archetypeId = 0;
    uint64_t guid = 0;
    std::string guardReason;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "grav shaft GetClass",
            [entity]() { return entity->GetClass(); },
            entityClass,
            &guardReason) ||
        !entityClass ||
        !CoopRuntimeGuards::TryGuardedCall(
            "grav shaft class GetName",
            [entityClass]() { return entityClass->GetName(); },
            className,
            &guardReason) ||
        !className || std::strcmp(className, "ArkGravShaft") != 0 ||
        !CoopRuntimeGuards::TryGuardedCall(
            "grav shaft GetArchetype",
            [entity]() { return entity->GetArchetype(); },
            archetype,
            &guardReason) ||
        !CoopRuntimeGuards::TryGuardedCall(
            "grav shaft GetGuid",
            [entity]() -> EntityGUID { return entity->GetGuid(); },
            guid,
            &guardReason) ||
        guid == 0)
    {
        return;
    }
    if (archetype &&
        !CoopRuntimeGuards::TryGuardedCall(
            "grav shaft archetype GetId",
            [archetype]() { return archetype->GetId(); },
            archetypeId,
            &guardReason))
    {
        return;
    }

    std::string resolveReason;
    CArkGravShaftEntity* shaft = ResolveHazardExtensionByGuid<CArkGravShaftEntity>(
        guid, archetypeId, "ArkGravShaft", resolveReason);
    if (!shaft)
        return;

    uint16_t state = 0;
    if (shaft->m_bEnabled)
        state |= CoopProtocol::kGravShaftStateEnabled;
    if (shaft->m_repairable.m_bBroken)
        state |= CoopProtocol::kGravShaftStateBroken;
    if (shaft->m_bDisrupted)
        state |= CoopProtocol::kGravShaftStateDisrupted;
    if (shaft->m_bReversed)
        state |= CoopProtocol::kGravShaftStateReversed;
    QueueLocalPersistentHazardState(
        entity,
        CoopProtocol::HazardEventKind::GravShaftState,
        state,
        reason);
}

bool ModMain::DebugSpawnPersistentAreaHazard(std::string& detail)
{
    constexpr uint64_t kArchetypeId = 10396455764390116210ull;
    constexpr uint64_t kGuid = 0x434f4f5041524541ull; // "COOPAREA"
    detail.clear();
    if (!gEnv || !gEnv->pEntitySystem || !ArkPlayer::GetInstancePtr() ||
        !ArkPlayer::GetInstance().GetEntity())
    {
        detail = "runtime_unavailable";
        return false;
    }

    EntityId entityId = INVALID_ENTITYID;
    IEntity* entity = nullptr;
    std::string reason;
    CoopRuntimeGuards::TryGuardedCall(
        "area hazard test FindEntityByGuid",
        []() { return gEnv->pEntitySystem->FindEntityByGuid(static_cast<EntityGUID>(kGuid)); },
        entityId,
        &reason);
    if (entityId != INVALID_ENTITYID)
    {
        CoopRuntimeGuards::TryGuardedCall(
            "area hazard test GetEntity",
            [entityId]() { return gEnv->pEntitySystem->GetEntity(entityId); },
            entity,
            &reason);
    }

    if (!entity)
    {
        IEntityArchetype* archetype = nullptr;
        if (!CoopRuntimeGuards::TryGuardedCall(
                "area hazard test GetEntityArchetype",
                []() { return gEnv->pEntitySystem->GetEntityArchetype(kArchetypeId); },
                archetype,
                &reason) ||
            !archetype)
        {
            detail = "missing_area_hazard_archetype";
            return false;
        }

        IEntity* playerEntity = ArkPlayer::GetInstance().GetEntity();
        SEntitySpawnParams params;
        params.guid = static_cast<EntityGUID>(kGuid);
        params.sName = "CoopAreaHazardTest";
        params.pArchetype = archetype;
        params.pClass = archetype->GetClass();
        params.vPosition =
            playerEntity->GetWorldPos() + playerEntity->GetWorldRotation().GetColumn1() * 3.0f;
        params.qRotation = playerEntity->GetWorldRotation();
        if (!CoopRuntimeGuards::TryGuardedCall(
                "area hazard test SpawnEntityFromArchetype",
                [&params, archetype]()
                {
                    return gEnv->pEntitySystem->SpawnEntityFromArchetype(archetype, params, true);
                },
                entity,
                &reason) ||
            !entity)
        {
            detail = "area_hazard_spawn_failed_" + (reason.empty() ? std::string("unknown") : reason);
            return false;
        }
        entityId = entity->GetId();
    }

    ArkAreaHazard* hazard = ResolveHazardExtensionByGuid<ArkAreaHazard>(
        kGuid, kArchetypeId, "ArkAreaHazard", reason);
    if (!hazard)
    {
        detail = "area_hazard_extension_missing_" + (reason.empty() ? std::string("unknown") : reason);
        return false;
    }

    detail =
        "entity_" + std::to_string(entityId) +
        "_guid_" + std::to_string(kGuid) +
        "_archetype_" + std::to_string(kArchetypeId) +
        "_active_" + std::to_string(hazard->m_bHazardActive ? 1 : 0);
    return true;
}

bool ModMain::DebugSpawnPersistentGravShaft(std::string& detail)
{
    constexpr uint64_t kArchetypeId = 371ull;
    constexpr uint64_t kGuid = 0x434f4f5047524156ull; // "COOPGRAV"
    detail.clear();
    if (!gEnv || !gEnv->pEntitySystem || !ArkPlayer::GetInstancePtr() ||
        !ArkPlayer::GetInstance().GetEntity())
    {
        detail = "runtime_unavailable";
        return false;
    }

    EntityId entityId = INVALID_ENTITYID;
    IEntity* entity = nullptr;
    std::string reason;
    CoopRuntimeGuards::TryGuardedCall(
        "grav shaft test FindEntityByGuid",
        []() { return gEnv->pEntitySystem->FindEntityByGuid(static_cast<EntityGUID>(kGuid)); },
        entityId,
        &reason);
    if (entityId != INVALID_ENTITYID)
    {
        CoopRuntimeGuards::TryGuardedCall(
            "grav shaft test GetEntity",
            [entityId]() { return gEnv->pEntitySystem->GetEntity(entityId); },
            entity,
            &reason);
    }

    if (!entity)
    {
        IEntityArchetype* archetype = nullptr;
        if (!CoopRuntimeGuards::TryGuardedCall(
                "grav shaft test GetEntityArchetype",
                []() { return gEnv->pEntitySystem->GetEntityArchetype(kArchetypeId); },
                archetype,
                &reason) ||
            !archetype)
        {
            detail = "missing_grav_shaft_archetype";
            return false;
        }

        IEntity* playerEntity = ArkPlayer::GetInstance().GetEntity();
        SEntitySpawnParams params;
        params.guid = static_cast<EntityGUID>(kGuid);
        params.sName = "CoopGravShaftTest";
        params.pArchetype = archetype;
        params.pClass = archetype->GetClass();
        params.vPosition =
            playerEntity->GetWorldPos() + playerEntity->GetWorldRotation().GetColumn1() * 4.0f;
        params.qRotation = playerEntity->GetWorldRotation();
        if (!CoopRuntimeGuards::TryGuardedCall(
                "grav shaft test SpawnEntityFromArchetype",
                [&params, archetype]()
                {
                    return gEnv->pEntitySystem->SpawnEntityFromArchetype(archetype, params, true);
                },
                entity,
                &reason) ||
            !entity)
        {
            detail = "grav_shaft_spawn_failed_" + (reason.empty() ? std::string("unknown") : reason);
            return false;
        }
        entityId = entity->GetId();
    }

    CArkGravShaftEntity* shaft = ResolveHazardExtensionByGuid<CArkGravShaftEntity>(
        kGuid, kArchetypeId, "ArkGravShaft", reason);
    if (!shaft)
    {
        detail = "grav_shaft_extension_missing_" + (reason.empty() ? std::string("unknown") : reason);
        return false;
    }

    uint16_t state = 0;
    if (shaft->m_bEnabled)
        state |= CoopProtocol::kGravShaftStateEnabled;
    if (shaft->m_repairable.m_bBroken)
        state |= CoopProtocol::kGravShaftStateBroken;
    if (shaft->m_bDisrupted)
        state |= CoopProtocol::kGravShaftStateDisrupted;
    if (shaft->m_bReversed)
        state |= CoopProtocol::kGravShaftStateReversed;
    detail =
        "entity_" + std::to_string(entityId) +
        "_guid_" + std::to_string(kGuid) +
        "_archetype_" + std::to_string(kArchetypeId) +
        "_state_" + std::to_string(state);
    return true;
}

bool ModMain::DebugSpawnPersistentRepairableObject(std::string& detail)
{
    constexpr uint64_t kArchetypeId = 399ull;
    constexpr uint64_t kGuid = 0x434f4f5052455041ull; // "COOPREPA"
    detail.clear();
    if (!gEnv || !gEnv->pEntitySystem || !ArkPlayer::GetInstancePtr() ||
        !ArkPlayer::GetInstance().GetEntity())
    {
        detail = "runtime_unavailable";
        return false;
    }

    EntityId entityId = INVALID_ENTITYID;
    IEntity* entity = nullptr;
    std::string reason;
    CoopRuntimeGuards::TryGuardedCall(
        "repairable object test FindEntityByGuid",
        []() { return gEnv->pEntitySystem->FindEntityByGuid(static_cast<EntityGUID>(kGuid)); },
        entityId,
        &reason);
    if (entityId != INVALID_ENTITYID)
    {
        CoopRuntimeGuards::TryGuardedCall(
            "repairable object test GetEntity",
            [entityId]() { return gEnv->pEntitySystem->GetEntity(entityId); },
            entity,
            &reason);
    }

    if (!entity)
    {
        IEntityArchetype* archetype = nullptr;
        if (!CoopRuntimeGuards::TryGuardedCall(
                "repairable object test GetEntityArchetype",
                []() { return gEnv->pEntitySystem->GetEntityArchetype(kArchetypeId); },
                archetype,
                &reason) ||
            !archetype)
        {
            detail = "missing_repairable_object_archetype";
            return false;
        }

        IEntity* playerEntity = ArkPlayer::GetInstance().GetEntity();
        SEntitySpawnParams params;
        params.guid = static_cast<EntityGUID>(kGuid);
        params.sName = "CoopRepairableObjectTest";
        params.pArchetype = archetype;
        params.pClass = archetype->GetClass();
        params.vPosition =
            playerEntity->GetWorldPos() + playerEntity->GetWorldRotation().GetColumn1() * 3.0f;
        params.qRotation = playerEntity->GetWorldRotation();
        if (!CoopRuntimeGuards::TryGuardedCall(
                "repairable object test SpawnEntityFromArchetype",
                [&params, archetype]()
                {
                    return gEnv->pEntitySystem->SpawnEntityFromArchetype(archetype, params, true);
                },
                entity,
                &reason) ||
            !entity)
        {
            detail = "repairable_object_spawn_failed_" +
                (reason.empty() ? std::string("unknown") : reason);
            return false;
        }
        entityId = entity->GetId();
    }

    ArkRepairableObject* object = ResolveHazardExtensionByGuid<ArkRepairableObject>(
        kGuid, kArchetypeId, "ArkRepairableObject", reason);
    if (!object)
    {
        detail = "repairable_object_extension_missing_" +
            (reason.empty() ? std::string("unknown") : reason);
        return false;
    }
    detail =
        "entity_" + std::to_string(entityId) +
        "_guid_" + std::to_string(kGuid) +
        "_archetype_" + std::to_string(kArchetypeId) +
        "_broken_" + std::to_string(object->m_repairable.m_bBroken ? 1 : 0);
    return true;
}

bool ModMain::DebugSpawnPersistentExplosiveTank(std::string& detail, float forwardDistance)
{
    constexpr uint64_t kArchetypeId = 10396455764390116141ull;
    constexpr uint64_t kGuid = 0x434f4f5054414e4bull; // "COOPTANK"
    detail.clear();
    if (!gEnv || !gEnv->pEntitySystem || !ArkPlayer::GetInstancePtr() ||
        !ArkPlayer::GetInstance().GetEntity())
    {
        detail = "runtime_unavailable";
        return false;
    }

    EntityId entityId = INVALID_ENTITYID;
    IEntity* entity = nullptr;
    std::string reason;
    CoopRuntimeGuards::TryGuardedCall(
        "explosive tank test FindEntityByGuid",
        []() { return gEnv->pEntitySystem->FindEntityByGuid(static_cast<EntityGUID>(kGuid)); },
        entityId,
        &reason);
    if (entityId != INVALID_ENTITYID)
    {
        CoopRuntimeGuards::TryGuardedCall(
            "explosive tank test GetEntity",
            [entityId]() { return gEnv->pEntitySystem->GetEntity(entityId); },
            entity,
            &reason);
    }

    if (!entity)
    {
        IEntityArchetype* archetype = nullptr;
        if (!CoopRuntimeGuards::TryGuardedCall(
                "explosive tank test GetEntityArchetype",
                []() { return gEnv->pEntitySystem->GetEntityArchetype(kArchetypeId); },
                archetype,
                &reason) ||
            !archetype)
        {
            detail = "missing_explosive_tank_archetype";
            return false;
        }

        IEntity* playerEntity = ArkPlayer::GetInstance().GetEntity();
        Vec3 playerPosition(ZERO);
        Quat playerRotation(IDENTITY);
        if (!CoopRuntimeGuards::TryGuardedCall(
                "explosive tank test player position",
                [playerEntity]() { return playerEntity->GetWorldPos(); },
                playerPosition,
                &reason) ||
            !CoopRuntimeGuards::TryGuardedCall(
                "explosive tank test player rotation",
                [playerEntity]() { return playerEntity->GetWorldRotation(); },
                playerRotation,
                &reason))
        {
            detail = "player_transform_unavailable_" + reason;
            return false;
        }

        SEntitySpawnParams params;
        params.guid = static_cast<EntityGUID>(kGuid);
        params.sName = "CoopExplosiveTankTest";
        params.pArchetype = archetype;
        params.pClass = archetype->GetClass();
        params.vPosition = playerPosition + playerRotation.GetColumn1() * forwardDistance;
        params.qRotation = playerRotation;
        if (!CoopRuntimeGuards::TryGuardedCall(
                "explosive tank test SpawnEntityFromArchetype",
                [&params, archetype]()
                {
                    return gEnv->pEntitySystem->SpawnEntityFromArchetype(archetype, params, true);
                },
                entity,
                &reason) ||
            !entity ||
            !CoopRuntimeGuards::TryGuardedCall(
                "explosive tank test entity GetId",
                [entity]() { return entity->GetId(); },
                entityId,
                &reason))
        {
            detail = "explosive_tank_spawn_failed_" +
                (reason.empty() ? std::string("unknown") : reason);
            return false;
        }
    }

    ArkExplosiveTank* tank = ResolveHazardExtensionByGuid<ArkExplosiveTank>(
        kGuid, kArchetypeId, "ArkExplosiveTank", reason);
    if (!tank)
    {
        detail = "explosive_tank_extension_missing_" +
            (reason.empty() ? std::string("unknown") : reason);
        return false;
    }

    EArkExplosiveTankState state = EArkExplosiveTankState::Invalid;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "explosive tank test read state",
            [tank]() { return tank->m_state; },
            state,
            &reason))
    {
        detail = "explosive_tank_state_unavailable_" + reason;
        return false;
    }
    detail =
        "entity_" + std::to_string(entityId) +
        "_guid_" + std::to_string(kGuid) +
        "_archetype_" + std::to_string(kArchetypeId) +
        "_distance_" + std::to_string(forwardDistance) +
        "_state_" + std::to_string(static_cast<uint16_t>(state));
    return true;
}

bool ModMain::DebugStageAuthoredExplosiveTank(
    uint64_t targetGuid,
    float forwardDistance,
    std::string& detail)
{
    detail.clear();
    if (!gEnv || !gEnv->pEntitySystem || !ArkPlayer::GetInstancePtr() ||
        !ArkPlayer::GetInstance().GetEntity())
    {
        detail = "runtime_unavailable";
        return false;
    }

    constexpr uint64_t kDynamicArchetype = 10396455764390116141ull;
    constexpr uint64_t kStaticArchetype = 10396455764390116209ull;
    constexpr uint64_t kOxygenArchetype = 3149325216984495995ull;
    const auto isSupportedArchetype = [](uint64_t id)
    {
        return id == kDynamicArchetype || id == kStaticArchetype || id == kOxygenArchetype;
    };

    IEntity* playerEntity = ArkPlayer::GetInstance().GetEntity();
    const Vec3 playerPosition = playerEntity->GetWorldPos();
    IEntity* best = nullptr;
    ArkExplosiveTank* bestTank = nullptr;
    uint64_t bestArchetypeId = 0;
    float bestDistanceSq = std::numeric_limits<float>::max();
    std::string reason;

    IEntityIt* iterator = gEnv->pEntitySystem->GetEntityIterator();
    if (!iterator)
    {
        detail = "iterator_missing";
        return false;
    }
    iterator->MoveFirst();
    while (!iterator->IsEnd())
    {
        IEntity* entity = iterator->Next();
        if (!entity)
            continue;
        IEntityClass* entityClass = entity->GetClass();
        const char* className = entityClass ? entityClass->GetName() : nullptr;
        const uint64_t guid = entity->GetGuid();
        IEntityArchetype* archetype = entity->GetArchetype();
        const uint64_t archetypeId = archetype ? archetype->GetId() : 0;
        if (!className || std::strcmp(className, "ArkExplosiveTank") != 0 ||
            guid == 0 || (targetGuid != 0 && guid != targetGuid) ||
            !isSupportedArchetype(archetypeId))
        {
            continue;
        }

        ArkExplosiveTank* tank = ResolveHazardExtensionByGuid<ArkExplosiveTank>(
            guid, archetypeId, "ArkExplosiveTank", reason);
        if (!tank || tank->m_state == EArkExplosiveTankState::Destroyed)
            continue;

        const float distanceSq = (entity->GetWorldPos() - playerPosition).GetLengthSquared();
        if (distanceSq < bestDistanceSq)
        {
            best = entity;
            bestTank = tank;
            bestArchetypeId = archetypeId;
            bestDistanceSq = distanceSq;
        }
    }
    iterator->Release();

    if (!best || !bestTank)
    {
        detail = targetGuid == 0 ? "authored_tank_missing" :
            "authored_tank_missing_guid_" + std::to_string(targetGuid);
        return false;
    }

    // Stage above the local victim so the proof cannot be invalidated by a
    // railing, wall edge, or floor transition in the selected save fixture.
    const Vec3 targetPosition = playerPosition + Vec3(0.0f, 0.0f, forwardDistance);
    if (!CoopRuntimeGuards::TryGuardedVoidCall(
            "authored explosive tank stage SetPos",
            [best, targetPosition]() { best->SetPos(targetPosition, 0, true, true); },
            &reason))
    {
        detail = "authored_tank_stage_failed_" + reason;
        return false;
    }

    const Vec3 stagedPosition = best->GetWorldPos();
    detail =
        "entity_" + std::to_string(best->GetId()) +
        "_guid_" + std::to_string(best->GetGuid()) +
        "_archetype_" + std::to_string(bestArchetypeId) +
        "_state_" + std::to_string(static_cast<uint16_t>(bestTank->m_state)) +
        "_distance_" + std::to_string((stagedPosition - playerPosition).GetLength()) +
        "_offsetZ_" + std::to_string(stagedPosition.z - playerPosition.z) +
        "_signal_" + std::to_string(bestTank->m_explosionSignalPackage) +
        "_radius_" + std::to_string(bestTank->m_minRadius) +
        "_" + std::to_string(bestTank->m_maxRadius) +
        "_delay_" + std::to_string(bestTank->m_explodeDelay);
    return true;
}

bool ModMain::DebugExplodePersistentExplosiveTank(std::string& detail, uint64_t targetGuid)
{
    detail.clear();
    if (!gEnv || !gEnv->pEntitySystem || targetGuid == 0)
    {
        detail = "invalid_runtime_or_guid";
        return false;
    }
    std::string reason;
    const EntityId entityId = gEnv->pEntitySystem->FindEntityByGuid(static_cast<EntityGUID>(targetGuid));
    IEntity* entity = entityId != INVALID_ENTITYID ? gEnv->pEntitySystem->GetEntity(entityId) : nullptr;
    IEntityArchetype* archetype = entity ? entity->GetArchetype() : nullptr;
    const uint64_t archetypeId = archetype ? archetype->GetId() : 0;
    ArkExplosiveTank* tank = ResolveHazardExtensionByGuid<ArkExplosiveTank>(
        targetGuid, archetypeId, "ArkExplosiveTank", reason);
    if (!tank)
    {
        detail = "explosive_tank_extension_missing_" +
            (reason.empty() ? std::string("unknown") : reason);
        return false;
    }

    EArkExplosiveTankState before = EArkExplosiveTankState::Invalid;
    EArkExplosiveTankState after = EArkExplosiveTankState::Invalid;
    const bool called = CoopRuntimeGuards::TryGuardedCall(
            "explosive tank test read before",
            [tank]() { return tank->m_state; },
            before,
            &reason) &&
        (before == EArkExplosiveTankState::Destroyed ||
            CoopRuntimeGuards::TryGuardedVoidCall(
                "explosive tank test Explode",
                [tank]() { tank->Explode(); },
                &reason)) &&
        CoopRuntimeGuards::TryGuardedCall(
            "explosive tank test read after",
            [tank]() { return tank->m_state; },
            after,
            &reason);
    detail =
        "guid_" + std::to_string(targetGuid) +
        "_before_" + std::to_string(static_cast<uint16_t>(before)) +
        "_after_" + std::to_string(static_cast<uint16_t>(after)) +
        "_reason_" + (reason.empty() ? std::string("-") : reason);
    return called && after == EArkExplosiveTankState::Destroyed;
}

bool ModMain::DebugTriggerPersistentExplosiveTank(std::string& detail, uint64_t targetGuid)
{
    detail.clear();
    if (!gEnv || !gEnv->pEntitySystem || targetGuid == 0)
    {
        detail = "invalid_runtime_or_guid";
        return false;
    }
    std::string reason;
    const EntityId entityId = gEnv->pEntitySystem->FindEntityByGuid(static_cast<EntityGUID>(targetGuid));
    IEntity* entity = entityId != INVALID_ENTITYID ? gEnv->pEntitySystem->GetEntity(entityId) : nullptr;
    IEntityArchetype* archetype = entity ? entity->GetArchetype() : nullptr;
    const uint64_t archetypeId = archetype ? archetype->GetId() : 0;
    ArkExplosiveTank* tank = ResolveHazardExtensionByGuid<ArkExplosiveTank>(
        targetGuid, archetypeId, "ArkExplosiveTank", reason);
    if (!tank)
    {
        detail = "explosive_tank_extension_missing_" +
            (reason.empty() ? std::string("unknown") : reason);
        return false;
    }

    EArkExplosiveTankState before = EArkExplosiveTankState::Invalid;
    EArkExplosiveTankState after = EArkExplosiveTankState::Invalid;
    const bool called = CoopRuntimeGuards::TryGuardedCall(
            "explosive tank trigger test read before",
            [tank]() { return tank->m_state; },
            before,
            &reason) &&
        before != EArkExplosiveTankState::Destroyed &&
        CoopRuntimeGuards::TryGuardedVoidCall(
            "explosive tank trigger test TriggerExplosion",
            [tank]() { tank->TriggerExplosion(); },
            &reason) &&
        CoopRuntimeGuards::TryGuardedCall(
            "explosive tank trigger test read after",
            [tank]() { return tank->m_state; },
            after,
            &reason);
    detail =
        "guid_" + std::to_string(targetGuid) +
        "_before_" + std::to_string(static_cast<uint16_t>(before)) +
        "_after_" + std::to_string(static_cast<uint16_t>(after)) +
        "_reason_" + (reason.empty() ? std::string("-") : reason);
    return called &&
        (after == EArkExplosiveTankState::PreExplode ||
            after == EArkExplosiveTankState::Destroyed);
}

bool ModMain::DebugSetNearestPersistentHazardState(
    bool areaHazard,
    uint16_t state,
    std::string& detail)
{
    detail.clear();
    if (!gEnv || !gEnv->pEntitySystem || !ArkPlayer::GetInstancePtr() || !ArkPlayer::GetInstance().GetEntity())
    {
        detail = "runtime_unavailable";
        return false;
    }
    if ((!areaHazard &&
            (state < static_cast<uint16_t>(EArkSurfaceHazardState::Inert) ||
                state > static_cast<uint16_t>(EArkSurfaceHazardState::Depleted))) ||
        (areaHazard && state > 1u))
    {
        detail = "invalid_state";
        return false;
    }

    const Vec3 playerPosition = ArkPlayer::GetInstance().GetEntity()->GetWorldPos();
    const char* expectedClass = areaHazard ? "ArkAreaHazard" : "ArkSurfaceHazard";
    IEntityIt* iterator = gEnv->pEntitySystem->GetEntityIterator();
    if (!iterator)
    {
        detail = "iterator_missing";
        return false;
    }

    IEntity* best = nullptr;
    float bestDistanceSq = std::numeric_limits<float>::max();
    iterator->MoveFirst();
    while (!iterator->IsEnd())
    {
        IEntity* entity = iterator->Next();
        if (!entity)
            continue;
        IEntityClass* entityClass = entity->GetClass();
        const char* className = entityClass ? entityClass->GetName() : nullptr;
        if (!className || std::strcmp(className, expectedClass) != 0)
            continue;
        const float distanceSq = (entity->GetWorldPos() - playerPosition).GetLengthSquared();
        if (distanceSq < bestDistanceSq)
        {
            bestDistanceSq = distanceSq;
            best = entity;
        }
    }
    iterator->Release();
    if (!best)
    {
        detail = "target_missing";
        return false;
    }

    const uint64_t guid = best->GetGuid();
    IEntityArchetype* archetype = best->GetArchetype();
    const uint64_t archetypeId = archetype ? archetype->GetId() : 0;
    std::string reason;
    bool applied = false;
    if (areaHazard)
    {
        ArkAreaHazard* hazard = ResolveHazardExtensionByGuid<ArkAreaHazard>(
            guid, archetypeId, "ArkAreaHazard", reason);
        if (hazard)
        {
            const bool desired = state != 0;
            applied = CoopRuntimeGuards::TryGuardedVoidCall(
                "debug area hazard set state",
                [hazard, desired]()
                {
                    if (desired)
                        hazard->StartHazard();
                    else
                        hazard->ClearHazard();
                },
                &reason) && hazard->m_bHazardActive == desired;
        }
    }
    else
    {
        ArkSurfaceHazard* hazard = ResolveHazardExtensionByGuid<ArkSurfaceHazard>(
            guid, archetypeId, "ArkSurfaceHazard", reason);
        if (hazard)
        {
            const EArkSurfaceHazardState desired = static_cast<EArkSurfaceHazardState>(state);
            bool changed = false;
            const bool called = CoopRuntimeGuards::TryGuardedCall(
                "debug surface hazard SetState",
                [hazard, desired]() { return hazard->SetState(desired, ArkSurfaceHazard::ForceType::none); },
                changed,
                &reason);
            applied = called && hazard->m_state == desired;
        }
    }

    if (applied && !areaHazard &&
        m_networkMode == CoopNetworkMode::Client &&
        !IsClientAreaAuthorityActive())
    {
        ++m_surfaceHazardObserverRequests;
        QueueLocalPersistentHazardState(
            best,
            CoopProtocol::HazardEventKind::SurfaceHazardState,
            state,
            "debug observer surface hazard request");
    }

    detail =
        "entity_" + std::to_string(best->GetId()) +
        "_guid_" + std::to_string(guid) +
        "_archetype_" + std::to_string(archetypeId) +
        "_state_" + std::to_string(state) +
        "_distance_" + std::to_string(std::sqrt(bestDistanceSq)) +
        "_reason_" + (reason.empty() ? std::string("-") : reason);
    return applied;
}

bool ModMain::DebugSetNearestElectricalBoxState(uint16_t state, std::string& detail)
{
    constexpr uint16_t kValidStateMask =
        CoopProtocol::kElectricalBoxStatePowered |
        CoopProtocol::kElectricalBoxStateBroken |
        CoopProtocol::kElectricalBoxStateDisrupted |
        CoopProtocol::kElectricalBoxStateFortified;
    detail.clear();
    if ((state & ~kValidStateMask) != 0 || !gEnv || !gEnv->pEntitySystem ||
        !ArkPlayer::GetInstancePtr() || !ArkPlayer::GetInstance().GetEntity())
    {
        detail = "invalid_state_or_runtime";
        return false;
    }

    const Vec3 playerPosition = ArkPlayer::GetInstance().GetEntity()->GetWorldPos();
    IEntityIt* iterator = gEnv->pEntitySystem->GetEntityIterator();
    IEntity* best = nullptr;
    float bestDistanceSq = std::numeric_limits<float>::max();
    if (iterator)
    {
        iterator->MoveFirst();
        while (!iterator->IsEnd())
        {
            IEntity* entity = iterator->Next();
            IEntityClass* entityClass = entity ? entity->GetClass() : nullptr;
            const char* className = entityClass ? entityClass->GetName() : nullptr;
            if (!className || std::strcmp(className, "ArkElectricalBox") != 0)
                continue;
            const float distanceSq = (entity->GetWorldPos() - playerPosition).GetLengthSquared();
            if (distanceSq < bestDistanceSq)
            {
                best = entity;
                bestDistanceSq = distanceSq;
            }
        }
        iterator->Release();
    }
    if (!best)
    {
        detail = "target_missing";
        return false;
    }

    IEntityArchetype* archetype = best->GetArchetype();
    const uint64_t archetypeId = archetype ? archetype->GetId() : 0;
    std::string reason;
    ArkElectricalBox* box = ResolveHazardExtensionByGuid<ArkElectricalBox>(
        best->GetGuid(), archetypeId, "ArkElectricalBox", reason);
    if (!box)
    {
        detail = "extension_missing_" + reason;
        return false;
    }

    const bool powered = (state & CoopProtocol::kElectricalBoxStatePowered) != 0;
    const bool broken = (state & CoopProtocol::kElectricalBoxStateBroken) != 0;
    const bool disrupted = (state & CoopProtocol::kElectricalBoxStateDisrupted) != 0;
    const bool fortified = (state & CoopProtocol::kElectricalBoxStateFortified) != 0;
    ++m_hazardEventApplyDepth;
    const bool called = CoopRuntimeGuards::TryGuardedVoidCall(
        "debug electrical box aggregate state",
        [box, powered, broken, disrupted, fortified]()
        {
            box->SetPowered(powered, INVALID_ENTITYID);
            ArkRepairable::FSetBroken(&box->m_repairable, broken, true);
            ApplyRepairableFortifiedState(box->m_repairable, fortified);
            box->m_disruptable.SetDisrupted(disrupted, true);
        },
        &reason);
    --m_hazardEventApplyDepth;
    const bool matched = called && box->m_bPowered == powered &&
        box->m_repairable.m_bBroken == broken &&
        box->m_repairable.m_bFortified == fortified &&
        box->m_disruptable.m_bDisrupted == disrupted;
    if (matched)
        OnNativeElectricalBoxStateChanged(best, true, "debug electrical aggregate");
    detail =
        "entity_" + std::to_string(best->GetId()) +
        "_guid_" + std::to_string(best->GetGuid()) +
        "_archetype_" + std::to_string(archetypeId) +
        "_state_" + std::to_string(state) +
        "_distance_" + std::to_string(std::sqrt(bestDistanceSq)) +
        "_reason_" + (reason.empty() ? std::string("-") : reason);
    return matched;
}

bool ModMain::DebugSetNearestRepairableState(uint16_t state, std::string& detail)
{
    detail.clear();
    constexpr uint16_t kValidStateMask =
        CoopProtocol::kRepairableStateBroken |
        CoopProtocol::kRepairableStateFortified;
    if ((state & ~kValidStateMask) != 0 || !gEnv || !gEnv->pEntitySystem || !ArkPlayer::GetInstancePtr() ||
        !ArkPlayer::GetInstance().GetEntity())
    {
        detail = "runtime_unavailable";
        return false;
    }

    const Vec3 playerPosition = ArkPlayer::GetInstance().GetEntity()->GetWorldPos();
    IEntityIt* iterator = gEnv->pEntitySystem->GetEntityIterator();
    IEntity* best = nullptr;
    const char* bestClassName = nullptr;
    float bestDistanceSq = std::numeric_limits<float>::max();
    if (iterator)
    {
        iterator->MoveFirst();
        while (!iterator->IsEnd())
        {
            IEntity* entity = iterator->Next();
            IEntityClass* entityClass = entity ? entity->GetClass() : nullptr;
            const char* className = entityClass ? entityClass->GetName() : nullptr;
            if (!className || (std::strcmp(className, "ArkChargeTrap") != 0 &&
                    std::strcmp(className, "ArkRepairableObject") != 0))
            {
                continue;
            }
            // Dynamic charge/projectile helpers can share ArkChargeTrap's
            // class without a persistent identity. The persistent-state lane
            // intentionally targets authored/save-restored objects only.
            if (entity->GetGuid() == 0)
                continue;
            const float distanceSq = (entity->GetWorldPos() - playerPosition).GetLengthSquared();
            if (distanceSq < bestDistanceSq)
            {
                best = entity;
                bestClassName = className;
                bestDistanceSq = distanceSq;
            }
        }
        iterator->Release();
    }
    if (!best || !bestClassName)
    {
        detail = "target_missing";
        return false;
    }

    IEntityArchetype* archetype = best->GetArchetype();
    const uint64_t archetypeId = archetype ? archetype->GetId() : 0;
    ArkRepairable* repairable = nullptr;
    std::string reason;
    if (std::strcmp(bestClassName, "ArkChargeTrap") == 0)
    {
        ArkChargeTrap* trap = ResolveHazardExtensionByGuid<ArkChargeTrap>(
            best->GetGuid(), archetypeId, "ArkChargeTrap", reason);
        repairable = trap ? &trap->m_repairable : nullptr;
    }
    else
    {
        ArkRepairableObject* object = ResolveHazardExtensionByGuid<ArkRepairableObject>(
            best->GetGuid(), archetypeId, "ArkRepairableObject", reason);
        repairable = object ? &object->m_repairable : nullptr;
    }
    if (!repairable)
    {
        detail =
            "extension_missing_entity_" + std::to_string(best->GetId()) +
            "_guid_" + std::to_string(best->GetGuid()) +
            "_class_" + bestClassName +
            "_archetype_" + std::to_string(archetypeId) +
            "_reason_" + reason;
        return false;
    }

    const bool broken = (state & CoopProtocol::kRepairableStateBroken) != 0;
    const bool fortified = (state & CoopProtocol::kRepairableStateFortified) != 0;
    ++m_hazardEventApplyDepth;
    bool changed = false;
    const bool called = CoopRuntimeGuards::TryGuardedCall(
        "debug repairable SetBroken",
        [repairable, broken]() { return ArkRepairable::FSetBroken(repairable, broken, true); },
        changed,
        &reason);
    if (called)
        ApplyRepairableFortifiedState(*repairable, fortified);
    --m_hazardEventApplyDepth;
    const bool matched = called && repairable->m_bBroken == broken &&
        repairable->m_bFortified == fortified;
    if (matched)
        OnNativeRepairableStateChanged(best, true, "debug repairable aggregate");
    detail =
        "entity_" + std::to_string(best->GetId()) +
        "_guid_" + std::to_string(best->GetGuid()) +
        "_class_" + bestClassName +
        "_archetype_" + std::to_string(archetypeId) +
        "_state_" + std::to_string(state) +
        "_broken_" + std::to_string(broken ? 1 : 0) +
        "_fortified_" + std::to_string(fortified ? 1 : 0) +
        "_distance_" + std::to_string(std::sqrt(bestDistanceSq)) +
        "_reason_" + (reason.empty() ? std::string("-") : reason);
    return matched;
}

bool ModMain::DebugSetNearestGravShaftState(uint16_t state, std::string& detail)
{
    constexpr uint16_t kValidStateMask =
        CoopProtocol::kGravShaftStateEnabled |
        CoopProtocol::kGravShaftStateBroken |
        CoopProtocol::kGravShaftStateDisrupted |
        CoopProtocol::kGravShaftStateReversed;
    detail.clear();
    if ((state & ~kValidStateMask) != 0 || !gEnv || !gEnv->pEntitySystem ||
        !ArkPlayer::GetInstancePtr() || !ArkPlayer::GetInstance().GetEntity())
    {
        detail = "invalid_state_or_runtime";
        return false;
    }

    const Vec3 playerPosition = ArkPlayer::GetInstance().GetEntity()->GetWorldPos();
    IEntityIt* iterator = gEnv->pEntitySystem->GetEntityIterator();
    IEntity* best = nullptr;
    float bestDistanceSq = std::numeric_limits<float>::max();
    if (iterator)
    {
        iterator->MoveFirst();
        while (!iterator->IsEnd())
        {
            IEntity* entity = iterator->Next();
            IEntityClass* entityClass = entity ? entity->GetClass() : nullptr;
            const char* className = entityClass ? entityClass->GetName() : nullptr;
            if (!className || std::strcmp(className, "ArkGravShaft") != 0 || entity->GetGuid() == 0)
                continue;
            const float distanceSq = (entity->GetWorldPos() - playerPosition).GetLengthSquared();
            if (distanceSq < bestDistanceSq)
            {
                best = entity;
                bestDistanceSq = distanceSq;
            }
        }
        iterator->Release();
    }
    if (!best)
    {
        detail = "target_missing";
        return false;
    }

    IEntityArchetype* archetype = best->GetArchetype();
    const uint64_t archetypeId = archetype ? archetype->GetId() : 0;
    std::string reason;
    CArkGravShaftEntity* shaft = ResolveHazardExtensionByGuid<CArkGravShaftEntity>(
        best->GetGuid(), archetypeId, "ArkGravShaft", reason);
    if (!shaft)
    {
        detail = "extension_missing_" + reason;
        return false;
    }

    ++m_hazardEventApplyDepth;
    const bool matched = ApplyGravShaftAggregateState(shaft, state, reason);
    --m_hazardEventApplyDepth;
    if (matched)
        OnNativeGravShaftStateChanged(best, true, "debug grav shaft aggregate");
    detail =
        "entity_" + std::to_string(best->GetId()) +
        "_guid_" + std::to_string(best->GetGuid()) +
        "_archetype_" + std::to_string(archetypeId) +
        "_state_" + std::to_string(state) +
        "_distance_" + std::to_string(std::sqrt(bestDistanceSq)) +
        "_reason_" + (reason.empty() ? std::string("-") : reason);
    return matched;
}

void ModMain::OnNativeLeakAdded(
    ArkLeakable* leakable,
    const Vec3& position,
    const Vec3& direction,
    float length,
    const char* reason)
{
    QueueLocalLeakHazardEvent(leakable, CoopProtocol::HazardEventKind::LeakAdded, position, direction, length, 0, reason);
}

void ModMain::OnNativeLeakRemoved(
    ArkLeakable* leakable,
    const Vec3& position,
    const Vec3& direction,
    float length,
    const char* reason)
{
    QueueLocalLeakHazardEvent(leakable, CoopProtocol::HazardEventKind::LeakRemoved, position, direction, length, 0, reason);
}

void ModMain::OnNativeLeakValveStateChanged(ArkLeakable* leakable, bool open, const char* reason)
{
    QueueLocalLeakHazardEvent(
        leakable,
        CoopProtocol::HazardEventKind::LeakValveState,
        Vec3(ZERO),
        Vec3(0.0f, 1.0f, 0.0f),
        0.0f,
        open ? 1u : 0u,
        reason);
}

void ModMain::OnNativePsiLiftFieldStarted(CArkPsiPowerLift* power, bool result, const char* reason)
{
    if (!result || !power || m_hazardEventApplyDepth != 0 ||
        m_networkMode == CoopNetworkMode::Off || !m_hasRemoteEndpoint || !IsSessionGameplayReady())
    {
        return;
    }

    Vec3 position(ZERO);
    Vec3 axis(0.0f, 0.0f, 1.0f);
    float height = 0.0f;
    int level = -1;
    std::string guardReason;
    if (!CoopRuntimeGuards::TryGuardedVoidCall(
            "hazard psi lift capture",
            [power, &position, &axis, &height, &level]()
            {
                position = power->m_targetingLift.m_position;
                axis = power->m_targetingLift.m_axis;
                height = power->m_targetingLift.m_height;
                level = power->m_level;
            },
            &guardReason) ||
        level < 0 || level > 2 || !std::isfinite(height) || height < 0.25f || height > 50.0f ||
        axis.GetLengthSquared() < 0.25f || axis.GetLengthSquared() > 2.25f)
    {
        ++m_hazardEventDropped;
        m_lastHazardEvent = "psi_lift_capture_failed_" + guardReason;
        return;
    }

    CoopProtocol::HazardEventPacket packet;
    packet.sequence = CoopSerialSequence::Advance(m_hazardEventSequence);
    packet.worldEpoch = m_localWorldEpoch;
    packet.eventId = BuildHazardEventId(
        static_cast<uint16_t>(CoopProtocol::HazardEventKind::PsiLiftField),
        static_cast<uint64_t>(EArkPsiPowers::lift) + 1u,
        position,
        packet.sequence);
    packet.hostSaveKeyHash = CurrentHostSaveKeyHash();
    packet.areaId = m_localLevelId;
    packet.sourcePeerHash = GetLocalAccountToken();
    packet.archetypeId = static_cast<uint64_t>(EArkPsiPowers::lift) + 1u;
    packet.eventKind = static_cast<uint16_t>(CoopProtocol::HazardEventKind::PsiLiftField);
    packet.flags = static_cast<uint16_t>(level);
    packet.px = position.x;
    packet.py = position.y;
    packet.pz = position.z;
    packet.dx = axis.x;
    packet.dy = axis.y;
    packet.dz = axis.z;
    packet.scalar = height;

    if (!SendHazardEventTo(packet, m_remoteAddress, m_remotePort, "hazard psi lift send failed"))
    {
        ++m_hazardEventDropped;
        return;
    }
    m_lastHazardEvent =
        "sent_psi_lift_event_" + std::to_string(packet.eventId) +
        "_level_" + std::to_string(level) +
        "_reason_" + (reason && reason[0] ? reason : "-");
}

void ModMain::HandleHazardEvent(const CoopProtocol::HazardEventPacket& packet)
{
    ++m_hazardEventReceived;
    if (packet.eventId == 0 || packet.areaId != m_localLevelId ||
        packet.worldEpoch != m_localWorldEpoch ||
        !IsCurrentOrRecentHostSaveKeyHash(packet.hostSaveKeyHash) ||
        !IsFiniteHazardTransform(packet) || packet.archetypeId == 0)
    {
        ++m_hazardEventDropped;
        m_lastHazardEvent = "hazard_guard_drop_event_" + std::to_string(packet.eventId);
        return;
    }
    if (!m_appliedHazardEventIds.insert(packet.eventId).second)
    {
        m_lastHazardEvent = "hazard_duplicate_event_" + std::to_string(packet.eventId);
        return;
    }
    const auto kind = static_cast<CoopProtocol::HazardEventKind>(packet.eventKind);
    if (kind == CoopProtocol::HazardEventKind::PsiLiftField)
    {
        if (!IsFinitePsiLiftPayload(packet))
        {
            ++m_hazardEventDropped;
            m_lastHazardEvent = "hazard_psi_lift_payload_drop_event_" + std::to_string(packet.eventId);
            return;
        }

        CArkPsiPowerLift* liftPower = nullptr;
        std::string detail;
        const bool resolved = CoopRuntimeGuards::TryGuardedVoidCall(
            "hazard psi lift resolve power",
            [&liftPower]()
            {
                IArkPsiPower* base = ArkPlayer::GetInstance().GetPsiPowerComponent().GetIArkPsiPower(EArkPsiPowers::lift);
                if (base && base->GetEnum() == EArkPsiPowers::lift)
                    liftPower = static_cast<CArkPsiPowerLift*>(base);
            },
            &detail);
        if (!resolved || !liftPower)
        {
            ++m_hazardEventDropped;
            m_lastHazardEvent = "hazard_psi_lift_power_missing_" + detail;
            return;
        }

        IEntity* playerEntity = ArkPlayer::GetInstance().GetEntity();
        if (!playerEntity)
        {
            ++m_hazardEventDropped;
            m_lastHazardEvent = "hazard_psi_lift_player_missing";
            return;
        }

        const int previousLevel = liftPower->m_level;
        const size_t normalBefore = liftPower->m_playerLiftVolumeManager.m_liftVolumes.size();
        const size_t zeroGBefore = liftPower->m_playerLiftVolumeZeroGManager.m_liftVolumes.size();
        const float radius = liftPower->m_properties.m_Unique.m_Radius;
        bool started = false;
        alignas(ArkPsiLift) std::byte savedLiftStorage[sizeof(ArkPsiLift)];
        alignas(ArkPsiLift) std::byte generatedLiftStorage[sizeof(ArkPsiLift)];
        ArkPsiLift* const savedLift = reinterpret_cast<ArkPsiLift*>(savedLiftStorage);
        ArkPsiLift* const generatedLift = reinterpret_cast<ArkPsiLift*>(generatedLiftStorage);
        bool savedLiftConstructed = false;
        bool generatedLiftConstructed = false;

        ++m_hazardEventApplyDepth;
        const bool called = CoopRuntimeGuards::TryGuardedVoidCall(
            "hazard psi lift native Start_Derived",
            [&]()
            {
                ArkPsiLift::FArkPsiLiftOv2(savedLift, std::move(liftPower->m_targetingLift));
                savedLiftConstructed = true;
                ArkPsiLift::FArkPsiLiftOv1(
                    generatedLift,
                    radius,
                    packet.scalar,
                    playerEntity->GetId());
                generatedLiftConstructed = true;
                ArkPsiLift::FoperatorEqOv1(&liftPower->m_targetingLift, std::move(*generatedLift));
                ArkPsiLift::FBitNotArkPsiLift(generatedLift);
                generatedLiftConstructed = false;
                liftPower->m_level = static_cast<int>(packet.flags & 0xffu);
                liftPower->m_targetingLift.SetAxis(Vec3(packet.dx, packet.dy, packet.dz).GetNormalized());
                liftPower->m_targetingLift.SetPosition(Vec3(packet.px, packet.py, packet.pz));
                liftPower->m_targetingLift.SetHeight(packet.scalar);
                started = liftPower->Start_Derived();
                ArkPsiLift::FoperatorEqOv1(&liftPower->m_targetingLift, std::move(*savedLift));
                ArkPsiLift::FBitNotArkPsiLift(savedLift);
                savedLiftConstructed = false;
                liftPower->m_level = previousLevel;
            },
            &detail);

        if (generatedLiftConstructed)
            CoopRuntimeGuards::TryGuardedVoidCall("hazard psi lift cleanup generated", [&]() { ArkPsiLift::FBitNotArkPsiLift(generatedLift); }, nullptr);
        if (savedLiftConstructed)
        {
            CoopRuntimeGuards::TryGuardedVoidCall(
                "hazard psi lift restore targeting",
                [&]()
                {
                    ArkPsiLift::FoperatorEqOv1(&liftPower->m_targetingLift, std::move(*savedLift));
                    ArkPsiLift::FBitNotArkPsiLift(savedLift);
                    liftPower->m_level = previousLevel;
                },
                nullptr);
        }
        --m_hazardEventApplyDepth;

        const size_t normalAfter = liftPower->m_playerLiftVolumeManager.m_liftVolumes.size();
        const size_t zeroGAfter = liftPower->m_playerLiftVolumeZeroGManager.m_liftVolumes.size();
        if (!called || !started || (normalAfter <= normalBefore && zeroGAfter <= zeroGBefore))
        {
            ++m_hazardEventDropped;
            m_lastHazardEvent =
                "hazard_psi_lift_apply_failed_called_" + std::to_string(called ? 1 : 0) +
                "_started_" + std::to_string(started ? 1 : 0) +
                "_normal_" + std::to_string(normalBefore) + "_" + std::to_string(normalAfter) +
                "_zeroG_" + std::to_string(zeroGBefore) + "_" + std::to_string(zeroGAfter) +
                "_reason_" + detail;
            return;
        }

        ++m_hazardEventApplied;
        m_lastHazardEvent =
            "applied_psi_lift_event_" + std::to_string(packet.eventId) +
            "_normal_" + std::to_string(normalBefore) + "_" + std::to_string(normalAfter) +
            "_zeroG_" + std::to_string(zeroGBefore) + "_" + std::to_string(zeroGAfter);
        return;
    }
    if (kind == CoopProtocol::HazardEventKind::SurfaceHazardState ||
        kind == CoopProtocol::HazardEventKind::AreaHazardState ||
        kind == CoopProtocol::HazardEventKind::ElectricalBoxState ||
        kind == CoopProtocol::HazardEventKind::RepairableState ||
        kind == CoopProtocol::HazardEventKind::GravShaftState ||
        kind == CoopProtocol::HazardEventKind::ExplosiveTankExplode)
    {
        if (packet.targetGuid == 0 ||
            (kind == CoopProtocol::HazardEventKind::SurfaceHazardState &&
                (packet.flags < static_cast<uint16_t>(EArkSurfaceHazardState::Inert) ||
                    packet.flags > static_cast<uint16_t>(EArkSurfaceHazardState::Depleted))) ||
            (kind == CoopProtocol::HazardEventKind::AreaHazardState && packet.flags > 1u) ||
            (kind == CoopProtocol::HazardEventKind::ElectricalBoxState &&
                (packet.flags & ~(CoopProtocol::kElectricalBoxStatePowered |
                    CoopProtocol::kElectricalBoxStateBroken |
                    CoopProtocol::kElectricalBoxStateDisrupted |
                    CoopProtocol::kElectricalBoxStateFortified)) != 0) ||
            (kind == CoopProtocol::HazardEventKind::RepairableState &&
                (packet.flags & ~(CoopProtocol::kRepairableStateBroken |
                    CoopProtocol::kRepairableStateFortified)) != 0) ||
            (kind == CoopProtocol::HazardEventKind::GravShaftState &&
                (packet.flags & ~(CoopProtocol::kGravShaftStateEnabled |
                    CoopProtocol::kGravShaftStateBroken |
                    CoopProtocol::kGravShaftStateDisrupted |
                    CoopProtocol::kGravShaftStateReversed)) != 0) ||
            (kind == CoopProtocol::HazardEventKind::ExplosiveTankExplode &&
                packet.flags != static_cast<uint16_t>(EArkExplosiveTankState::Destroyed)))
        {
            ++m_hazardEventDropped;
            m_lastHazardEvent = "persistent_hazard_payload_drop_event_" + std::to_string(packet.eventId);
            return;
        }

        std::string detail;
        bool applied = false;
        ++m_hazardEventApplyDepth;
        if (kind == CoopProtocol::HazardEventKind::SurfaceHazardState)
        {
            ArkSurfaceHazard* hazard = ResolveHazardExtensionByGuid<ArkSurfaceHazard>(
                packet.targetGuid,
                packet.archetypeId,
                "ArkSurfaceHazard",
                detail);
            if (hazard)
            {
                const EArkSurfaceHazardState desired = static_cast<EArkSurfaceHazardState>(packet.flags);
                const bool called = CoopRuntimeGuards::TryGuardedCall(
                    "persistent surface hazard SetState",
                    [hazard, desired]()
                    {
                        return hazard->SetState(desired, ArkSurfaceHazard::ForceType::serialize);
                    },
                    applied,
                    &detail);
                applied = called && (applied || hazard->m_state == desired) && hazard->m_state == desired;
            }
        }
        else if (kind == CoopProtocol::HazardEventKind::AreaHazardState)
        {
            ArkAreaHazard* hazard = ResolveHazardExtensionByGuid<ArkAreaHazard>(
                packet.targetGuid,
                packet.archetypeId,
                "ArkAreaHazard",
                detail);
            if (hazard)
            {
                const bool desiredActive = packet.flags != 0;
                const bool called = CoopRuntimeGuards::TryGuardedVoidCall(
                    "persistent area hazard set active",
                    [hazard, desiredActive]()
                    {
                        if (desiredActive)
                            hazard->StartHazard();
                        else
                            hazard->ClearHazard();
                    },
                    &detail);
                applied = called && hazard->m_bHazardActive == desiredActive;
            }
        }
        else if (kind == CoopProtocol::HazardEventKind::ElectricalBoxState)
        {
            ArkElectricalBox* box = ResolveHazardExtensionByGuid<ArkElectricalBox>(
                packet.targetGuid,
                packet.archetypeId,
                "ArkElectricalBox",
                detail);
            if (box)
            {
                const bool powered = (packet.flags & CoopProtocol::kElectricalBoxStatePowered) != 0;
                const bool broken = (packet.flags & CoopProtocol::kElectricalBoxStateBroken) != 0;
                const bool disrupted = (packet.flags & CoopProtocol::kElectricalBoxStateDisrupted) != 0;
                const bool fortified = (packet.flags & CoopProtocol::kElectricalBoxStateFortified) != 0;
                const bool called = CoopRuntimeGuards::TryGuardedVoidCall(
                    "persistent electrical box aggregate state",
                    [box, powered, broken, disrupted, fortified]()
                    {
                        box->SetPowered(powered, INVALID_ENTITYID);
                        ArkRepairable::FSetBroken(&box->m_repairable, broken, true);
                        ApplyRepairableFortifiedState(box->m_repairable, fortified);
                        box->m_disruptable.SetDisrupted(disrupted, true);
                    },
                    &detail);
                applied = called && box->m_bPowered == powered &&
                    box->m_repairable.m_bBroken == broken &&
                    box->m_repairable.m_bFortified == fortified &&
                    box->m_disruptable.m_bDisrupted == disrupted;
            }
        }
        else if (kind == CoopProtocol::HazardEventKind::RepairableState)
        {
            const bool broken = (packet.flags & CoopProtocol::kRepairableStateBroken) != 0;
            const bool fortified = (packet.flags & CoopProtocol::kRepairableStateFortified) != 0;
            ArkRepairable* repairable = nullptr;
            ArkChargeTrap* trap = ResolveHazardExtensionByGuid<ArkChargeTrap>(
                packet.targetGuid,
                packet.archetypeId,
                "ArkChargeTrap",
                detail);
            if (trap)
            {
                repairable = &trap->m_repairable;
            }
            else
            {
                ArkRepairableObject* object = ResolveHazardExtensionByGuid<ArkRepairableObject>(
                    packet.targetGuid,
                    packet.archetypeId,
                    "ArkRepairableObject",
                    detail);
                repairable = object ? &object->m_repairable : nullptr;
            }
            if (repairable)
            {
                bool changed = false;
                const bool called = CoopRuntimeGuards::TryGuardedCall(
                    "persistent repairable SetBroken",
                    [repairable, broken]() { return ArkRepairable::FSetBroken(repairable, broken, true); },
                    changed,
                    &detail);
                if (called)
                    ApplyRepairableFortifiedState(*repairable, fortified);
                applied = called && repairable->m_bBroken == broken &&
                    repairable->m_bFortified == fortified;
            }
        }
        else if (kind == CoopProtocol::HazardEventKind::ExplosiveTankExplode)
        {
            ArkExplosiveTank* tank = ResolveHazardExtensionByGuid<ArkExplosiveTank>(
                packet.targetGuid,
                packet.archetypeId,
                "ArkExplosiveTank",
                detail);
            if (tank)
            {
                EArkExplosiveTankState state = EArkExplosiveTankState::Invalid;
                bool read = CoopRuntimeGuards::TryGuardedCall(
                    "persistent explosive tank read state",
                    [tank]() { return tank->m_state; },
                    state,
                    &detail);
                bool called = true;
                if (read && state != EArkExplosiveTankState::Destroyed)
                {
                    called = CoopRuntimeGuards::TryGuardedVoidCall(
                        "persistent explosive tank Explode",
                        [tank]() { tank->Explode(); },
                        &detail);
                    read = called && CoopRuntimeGuards::TryGuardedCall(
                        "persistent explosive tank read final state",
                        [tank]() { return tank->m_state; },
                        state,
                        &detail);
                }
                applied = called && read && state == EArkExplosiveTankState::Destroyed;
            }
        }
        else
        {
            CArkGravShaftEntity* shaft = ResolveHazardExtensionByGuid<CArkGravShaftEntity>(
                packet.targetGuid,
                packet.archetypeId,
                "ArkGravShaft",
                detail);
            if (shaft)
                applied = ApplyGravShaftAggregateState(shaft, packet.flags, detail);
        }
        --m_hazardEventApplyDepth;

        if (!applied)
        {
            ++m_hazardEventDropped;
            m_lastHazardEvent =
                "persistent_hazard_apply_failed_event_" + std::to_string(packet.eventId) +
                "_reason_" + detail;
            return;
        }

        ++m_hazardEventApplied;
        m_lastHazardEvent =
            "applied_persistent_hazard_kind_" + std::to_string(packet.eventKind) +
            "_state_" + std::to_string(packet.flags) +
            "_guid_" + std::to_string(packet.targetGuid);
        return;
    }
    if (!IsGrenadeResultKind(kind))
    {
        if (packet.targetGuid == 0 || !IsFiniteLeakPayload(packet))
        {
            ++m_hazardEventDropped;
            m_lastHazardEvent = "hazard_leak_payload_drop_event_" + std::to_string(packet.eventId);
            return;
        }

        std::string detail;
        ArkLeakable* leakable = ResolveLeakableByGuid(packet.targetGuid, packet.archetypeId, detail);
        if (!leakable)
        {
            ++m_hazardEventDropped;
            m_lastHazardEvent = "hazard_leak_target_missing_" + detail;
            return;
        }

        const Vec3 position(packet.px, packet.py, packet.pz);
        const Vec3 direction(packet.dx, packet.dy, packet.dz);
        bool applied = false;
        ++m_hazardEventApplyDepth;
        if (kind == CoopProtocol::HazardEventKind::LeakAdded)
        {
            bool alreadyPresent = false;
            for (const ArkLeakable::LeakInfo& localLeak : leakable->m_leaks)
            {
                if ((localLeak.m_pos - position).GetLengthSquared() <= 0.0004f &&
                    localLeak.m_dir.Dot(direction) >= 0.995f)
                {
                    alreadyPresent = true;
                    break;
                }
            }
            if (alreadyPresent)
            {
                applied = true;
            }
            else
            {
                ArkLeakable::LeakInfo leak;
                leak.m_pos = position;
                leak.m_dir = direction.GetNormalizedSafe(Vec3(0.0f, 1.0f, 0.0f));
                leak.m_vfxSlot = -1;
                leak.m_linkedEntity = INVALID_ENTITYID;
                leak.m_causeId = INVALID_ENTITYID;
                leak.m_length = packet.scalar;
                leak.m_timeSinceCreation = 0.0f;
                applied = CoopRuntimeGuards::TryGuardedCall(
                    "hazard leak AddLeak",
                    [leakable, &leak]() { return leakable->AddLeak(leak); },
                    applied,
                    &detail) && applied;
            }
        }
        else if (kind == CoopProtocol::HazardEventKind::LeakRemoved)
        {
            applied = true;
            while (!leakable->m_leaks.empty())
            {
                const bool removed = RepairFirstNativeLeak(leakable, "hazard leak RepairLeak", &detail);
                if (!removed)
                {
                    applied = false;
                    break;
                }
            }
        }
        else if (kind == CoopProtocol::HazardEventKind::LeakValveState)
        {
            const bool desiredOpen = (packet.flags & 1u) != 0;
            applied = CoopRuntimeGuards::TryGuardedVoidCall(
                "hazard leak SetValveState",
                [leakable, desiredOpen]() { leakable->SetValveState(desiredOpen, true); },
                &detail) && leakable->m_bValveOpen == desiredOpen;
        }
        --m_hazardEventApplyDepth;

        if (!applied)
        {
            ++m_hazardEventDropped;
            m_lastHazardEvent = "hazard_leak_apply_failed_" + detail;
            return;
        }
        ++m_hazardEventApplied;
        m_lastHazardEvent =
            "applied_leak_kind_" + std::to_string(packet.eventKind) +
            "_event_" + std::to_string(packet.eventId) +
            "_guid_" + std::to_string(packet.targetGuid);
        return;
    }

    if (!gEnv || !gEnv->pEntitySystem ||
        !GrenadeResultArchetypeMatches(kind, packet.archetypeId))
    {
        ++m_hazardEventDropped;
        return;
    }

    IEntityArchetype* archetype = nullptr;
    std::string guardReason;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "hazard recycler GetEntityArchetype",
            [&packet]() { return gEnv->pEntitySystem->GetEntityArchetype(packet.archetypeId); },
            archetype,
            &guardReason) ||
        !archetype)
    {
        ++m_hazardEventDropped;
        m_lastHazardEvent = "hazard_missing_archetype_" + std::to_string(packet.archetypeId);
        return;
    }

    SEntitySpawnParams params;
    const std::string name = "CoopHazardGrenade_" + std::to_string(packet.eventId);
    params.sName = name.c_str();
    params.pArchetype = archetype;
    params.pClass = archetype->GetClass();
    params.vPosition = Vec3(packet.px, packet.py, packet.pz);
    params.qRotation = Quat(packet.qw, packet.qx, packet.qy, packet.qz).GetNormalized();
    params.vScale = Vec3(1.0f);

    IEntity* entity = nullptr;
    ++m_hazardEventApplyDepth;
    const bool spawned = CoopRuntimeGuards::TryGuardedCall(
        "hazard recycler SpawnEntityFromArchetype",
        [&params, archetype]() { return gEnv->pEntitySystem->SpawnEntityFromArchetype(archetype, params, true); },
        entity,
        &guardReason);
    bool detonated = false;
    if (spawned && entity)
    {
        m_remoteHazardEntityIds.insert(entity->GetId());
        if (kind == CoopProtocol::HazardEventKind::RecyclerDetonate)
        {
            CArkProjectileRecyclerGrenade* grenade = nullptr;
            CoopRuntimeGuards::TryGuardedCall(
                "hazard recycler GetProjectileGrenadeFromEntityId",
                [entity]() { return CArkProjectileRecyclerGrenade::GetProjectileGrenadeFromEntityId(entity->GetId()); },
                grenade,
                &guardReason);
            if (grenade)
            {
                CoopRuntimeGuards::TryGuardedCall(
                    "hazard recycler Detonate",
                    [grenade]() { return grenade->Detonate(); },
                    detonated,
                    &guardReason);
            }
        }
        else
        {
            CArkProjectile* projectile = nullptr;
            CoopRuntimeGuards::TryGuardedCall(
                "hazard grenade GetProjectileFromEntityId",
                [entity]() { return CArkProjectile::GetProjectileFromEntityId(entity->GetId()); },
                projectile,
                &guardReason);
            if (projectile)
            {
                const EntityId remoteOwnerId = m_proxyEntityId != INVALID_ENTITYID
                    ? m_proxyEntityId
                    : 0;
                CoopRuntimeGuards::TryGuardedVoidCall(
                    "hazard grenade set remote owner",
                    [projectile, remoteOwnerId]() { projectile->m_ownerId = remoteOwnerId; },
                    nullptr);
                CArkProjectileGrenade* grenade = static_cast<CArkProjectileGrenade*>(projectile);
                detonated = CoopRuntimeGuards::TryGuardedVoidCall(
                    "hazard grenade DoDetonation",
                    [grenade]() { grenade->DoDetonation(); },
                    &guardReason);
            }
        }
    }
    --m_hazardEventApplyDepth;

    if (!spawned || !entity || !detonated)
    {
        if (entity)
            gEnv->pEntitySystem->RemoveEntity(entity->GetId(), true);
        ++m_hazardEventDropped;
        m_lastHazardEvent = "hazard_grenade_apply_failed_" + guardReason;
        return;
    }

    ++m_hazardEventApplied;
    m_lastHazardEvent =
        "applied_grenade_event_" + std::to_string(packet.eventId) +
        "_kind_" + std::to_string(packet.eventKind) +
        "_entity_" + std::to_string(entity->GetId()) +
        "_arch_" + std::to_string(packet.archetypeId);
}

void ModMain::ResetHazardEventState(const char* reason)
{
    m_hazardEventSequence = 0;
    m_hazardEventSent = 0;
    m_hazardEventReceived = 0;
    m_hazardEventApplied = 0;
    m_hazardEventDropped = 0;
    m_hazardEventApplyDepth = 0;
    m_surfaceHazardObserverRequests = 0;
    m_surfaceHazardNonAuthoritySuppressions = 0;
    m_appliedHazardEventIds.clear();
    {
        std::lock_guard<std::mutex> lock(m_sentExplosiveTankEventMutex);
        m_sentExplosiveTankEventGuids.clear();
    }
    m_sentLocalHazardEntityIds.clear();
    m_remoteHazardEntityIds.clear();
    m_lastHazardEvent = reason && reason[0] ? reason : "reset";
}
