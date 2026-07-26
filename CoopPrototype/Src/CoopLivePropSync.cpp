#include "ModMain.h"
#include "CoopRuntimeConfig.h"
#include "CoopRuntimeLog.h"
#include "CoopSerialSequence.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include "CoopRuntimeGuards.h"
#include <EntityUtils.h>
#include <Chairloader/IChairLogger.h>
#include <Prey/ArkEnums.h>
#include <Prey/CryEntitySystem/IEntityClass.h>
#include <Prey/CryEntitySystem/IEntitySystem.h>
#include <Prey/CryGame/Game.h>
#include <Prey/CryGame/IGame.h>
#include <Prey/CryGame/IGameFramework.h>
#include <Prey/CryPhysics/physinterface.h>
#include <Prey/CryCore/Platform/CryWindows.h>
#include <Prey/CrySystem/IConsole.h>
#include <Prey/CrySystem/ITimer.h>
#include <Prey/GameDll/ark/ArkTimeScaleManager.h>
#include <Prey/GameDll/ark/npc/ArkNpc.h>
#include <Prey/GameDll/ark/player/ArkPlayer.h>
#include <Prey/GameDll/ark/player/ArkPlayerCarry.h>
#include <Prey/GameDll/ark/weapons/arkprojectile.h>
#include <Prey/GameDll/arkitem.h>

namespace
{
using CoopRuntimeGuards::IsLikelyRuntimeCppObject;
using CoopRuntimeGuards::PreflightRuntimePointer;
using CoopRuntimeGuards::ReadRuntimeCString;
using CoopRuntimeGuards::RuntimeAccess;
using CoopRuntimeGuards::TryGuardedCall;
using CoopRuntimeGuards::TryGuardedVoidCall;

constexpr float kLivePropSyncSeconds = 0.016f;
constexpr float kLivePropCarriedSendSeconds = 0.033f;
constexpr float kLivePropMovingSendSeconds = 0.075f;
constexpr float kLivePropSettleSendSeconds = 0.150f;
constexpr float kLivePropIdleSendSeconds = 1.000f;
constexpr float kLivePropMidIdleSendSeconds = 2.000f;
constexpr float kLivePropFarIdleSendSeconds = 4.000f;
constexpr float kLivePropMidInterestDistanceSq = 30.0f * 30.0f;
constexpr float kLivePropFarInterestDistanceSq = 65.0f * 65.0f;
constexpr float kLivePropLocalCarryAuthorityGraceSeconds = 1.50f;
constexpr float kLivePropReleaseFlightAuthoritySeconds = 2.00f;
constexpr float kLivePropClientThrowAuthoritySeconds = 0.55f;
constexpr float kLivePropClientDropAuthoritySeconds = 0.25f;
constexpr float kLivePropClientReleaseForceSendSeconds = 1.50f;
constexpr float kLivePropClientMotionProposalSeconds = 0.12f;
constexpr float kLivePropClientCollisionAuthoritySeconds = 0.24f;
constexpr float kLivePropClientCollisionChainAuthoritySeconds = 0.18f;
constexpr float kLivePropHostTakeoverSuppressSeconds = 0.35f;
constexpr float kLivePropRemoteAuthoritySuppressSeconds = 0.40f;
constexpr float kLivePropRemoteBallisticSeconds = 0.45f;
constexpr float kLivePropReleaseMotionLatchSeconds = 0.42f;
constexpr float kLivePropClientAuthorityBlendSeconds = 0.75f;
constexpr float kLivePropHostImpulseBlendSeconds = 0.20f;
constexpr float kLivePropHostClientAuthorityBlendSeconds = 0.35f;
constexpr float kLivePropPendingThrowSeconds = 0.35f;
constexpr float kPeerDisconnectSessionTickKickSeconds = 0.5f;
constexpr float kLivePropMinPositionDeltaSq = 0.0004f;
constexpr float kLivePropCarriedMinPositionDeltaSq = 0.000025f;
constexpr float kLivePropMinScaleDeltaSq = 0.0001f;
constexpr float kLivePropMinApplySeconds = 0.012f;
constexpr float kLivePropSettleActiveSeconds = 1.25f;
constexpr float kLivePropVelocityActiveSq = 0.01f;
constexpr float kLivePropAngularVelocityActiveSq = 0.01f;
constexpr float kLivePropReleaseImpulseVelocitySq = 0.04f;
constexpr float kLivePropReleaseImpulseAngularVelocitySq = 0.04f;
constexpr float kLivePropDerivedDropMaxSpeed = 4.25f;
constexpr float kLivePropBallisticVelocitySq = 0.25f;
constexpr float kLivePropBallisticAngularVelocitySq = 0.25f;
constexpr float kLivePropRemoteApplyPositionDeltaSq = 0.000025f;
constexpr float kLivePropBallisticSoftCorrectDeltaSq = 0.25f;
constexpr float kLivePropBallisticHardCorrectDeltaSq = 4.0f;
constexpr float kLivePropBallisticLaunchAlignDeltaSq = 2.25f;
constexpr float kLivePropBallisticEmergencyCorrectDeltaSq = 36.0f;
constexpr float kLivePropBallisticVelocityCoupleDeltaSq = 0.01f;
constexpr float kLivePropBallisticMaxCorrectionSpeed = 14.0f;
constexpr float kLivePropBallisticMaxClientAuthorityCorrectionSpeed = 22.0f;
constexpr float kLivePropBallisticMaxDesiredSpeed = 46.0f;
constexpr float kLivePropBallisticMinRemainingBlendSeconds = 0.06f;
constexpr float kLivePropBallisticClientVelocityAlphaMin = 0.18f;
constexpr float kLivePropBallisticClientVelocityAlphaMax = 0.70f;
constexpr float kLivePropBallisticHostVelocityAlphaMin = 0.45f;
constexpr float kLivePropBallisticHostVelocityAlphaMax = 0.90f;
constexpr float kLivePropMaxRemoteTargetDeltaSq = 25000000.0f;
constexpr float kLivePropMaxAbsCoordinate = 10000.0f;
constexpr float kLivePropMaxVelocitySq = 2500.0f;
constexpr float kLivePropMaxAngularVelocitySq = 2500.0f;
constexpr float kLivePropCollisionPlayerDistanceSq = 4.0f;
constexpr float kLivePropXformCarryDiscoveryDistanceSq = 4.0f;
constexpr float kLivePropCollisionRecentQueueSeconds = 0.012f;
constexpr uint64_t kLivePropSyntheticGuidMask = 0x8000000000000000ULL;
constexpr float kLobbyMainLiftCargoMinX = 312.5f;
constexpr float kLobbyMainLiftCargoMaxX = 324.5f;
constexpr float kLobbyMainLiftCargoMinY = 715.5f;
constexpr float kLobbyMainLiftCargoMaxY = 721.5f;
constexpr float kLobbyMainLiftCargoMinZ = 332.0f;
constexpr float kLobbyMainLiftCargoMaxZ = 906.0f;
constexpr float kPeerThrottleStartSeconds = 5.0f;
constexpr float kPeerThrottleBusyStartSeconds = 60.0f;
constexpr float kPeerHostLoadNoticeGraceSeconds = 240.0f;
constexpr float kInitialConnectTimeoutSeconds = 25.0f;
constexpr float kPeerConnectionThrottleSeconds = 10.0f;
constexpr float kPeerTimeoutCountdownSeconds = 300.0f;
constexpr float kPeerTimeoutResumeFreshAgeSeconds = 0.50f;
constexpr float kPeerTimeoutResumeStableSeconds = 2.0f;
constexpr float kPeerTimeoutNoticeDurationSeconds = 1.25f;
constexpr size_t kMaxLivePropSendsPerTick = 6;

EntityId g_livePropLastCarriedEntityId = INVALID_ENTITYID;
uint64_t g_livePropLastCarriedGuid = 0;
EntityId g_livePropCarrySuppressEntityId = INVALID_ENTITYID;
uint64_t g_livePropCarrySuppressGuid = 0;
float g_livePropCarrySuppressUntilTime = -1000.0f;
EntityId g_livePropPendingThrowEntityId = INVALID_ENTITYID;
float g_livePropPendingThrowUntilTime = -1000.0f;

bool IsLobbyMainLiftLooseCargoPosition(
    const std::string& normalizedLevel,
    const Vec3& position)
{
    // Every peer runs the shipped ScriptControlledPhysics mover on its own
    // timeline. Absolute LiveProp positions for loose cabin cargo therefore
    // belong to a different moving reference frame and must not be written
    // into the local cabin. Doing so makes CryPhysics fold world-space cargo
    // contacts into the lift's compound parts, expanding its AABB on every
    // trip until the Client stalls and its character falls through the floor.
    return normalizedLevel == "campaign/research/lobby" &&
        position.x >= kLobbyMainLiftCargoMinX &&
        position.x <= kLobbyMainLiftCargoMaxX &&
        position.y >= kLobbyMainLiftCargoMinY &&
        position.y <= kLobbyMainLiftCargoMaxY &&
        position.z >= kLobbyMainLiftCargoMinZ &&
        position.z <= kLobbyMainLiftCargoMaxZ;
}

float NowSeconds()
{
    return gEnv && gEnv->pTimer ? gEnv->pTimer->GetAsyncCurTime() : 0.0f;
}

bool IsCarrySuppressActiveFor(EntityId entityId, uint64_t guid, float now)
{
    return g_livePropCarrySuppressEntityId != INVALID_ENTITYID &&
        now <= g_livePropCarrySuppressUntilTime &&
        (entityId == g_livePropCarrySuppressEntityId ||
            (guid != 0 && guid == g_livePropCarrySuppressGuid));
}

bool IsLocalPlayerNearPosition(const Vec3& position, float maxDistanceSq)
{
    if (!ArkPlayer::GetInstancePtr())
        return false;

    IEntity* playerEntity = nullptr;
    std::string reason;
    if (!TryGuardedCall("live prop local player entity", []() { return ArkPlayer::GetInstance().GetEntity(); }, playerEntity, &reason) ||
        !playerEntity)
    {
        return false;
    }

    Vec3 playerPosition(ZERO);
    if (!TryGuardedCall("live prop local player position", [playerEntity]() { return playerEntity->GetWorldPos(); }, playerPosition, &reason) ||
        !std::isfinite(playerPosition.x) ||
        !std::isfinite(playerPosition.y) ||
        !std::isfinite(playerPosition.z))
    {
        return false;
    }

    return (position - playerPosition).GetLengthSquared() <= maxDistanceSq;
}

void LogCoop(std::string_view msg)
{
    CoopRuntimeLog::Write(msg);
}

bool EnvFlagEnabled(const char* name)
{
    return CoopRuntimeConfig::Flag(name);
}

bool EnvFlagDefaultEnabled(const char* name)
{
    return CoopRuntimeConfig::FlagDefaultEnabled(name);
}

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
    {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string ReadFixedString(const char* value, size_t maxLength)
{
    size_t length = 0;
    while (length < maxLength && value[length] != '\0')
        ++length;
    return std::string(value, length);
}

bool IsBlockedLivePropClass(const std::string& lowerClassName)
{
    return lowerClassName.empty() ||
        lowerClassName.find("projectile") != std::string::npos ||
        lowerClassName == "arktechnopathemphazard" ||
        // Doors and other authored devices own their transform, animation and
        // collision through native device state. Treating an animated door as
        // a loose rigid body replays its closed physics pose on observers even
        // while the typed Door state correctly says that it is open.
        lowerClassName.find("arkdoor") != std::string::npos ||
        lowerClassName.find("arkturret") != std::string::npos ||
        lowerClassName.find("arkinteractive") != std::string::npos ||
        lowerClassName.find("arkkeypad") != std::string::npos ||
        lowerClassName.find("arkkeycard") != std::string::npos ||
        lowerClassName.find("arkworkstation") != std::string::npos ||
        lowerClassName.find("arkfabricator") != std::string::npos ||
        lowerClassName.find("arkrecycler") != std::string::npos ||
        lowerClassName.find("arkdispenser") != std::string::npos ||
        // ArkWorldUI entities are generated presentation children. Their
        // SetPos input is owner-local even though GetWorldPos is world-space;
        // replaying them through LiveProp therefore applies the owner
        // transform twice and can make a transition screen disappear.
        lowerClassName.find("worldui") != std::string::npos ||
        // Kiosks and touch panels are authored devices. In particular,
        // ArkGenericElevatorKiosk is a physical part of the main-lift cabin;
        // granting it a contact LiveProp lease feeds its world transform back
        // into the parent physics hierarchy and expands the lift AABB across
        // thousands of spatial cells.
        lowerClassName.find("kiosk") != std::string::npos ||
        lowerClassName.find("arkgravshaft") != std::string::npos ||
        lowerClassName.find("arkelevator") != std::string::npos ||
        lowerClassName == "player" ||
        lowerClassName.find("arkplayer") != std::string::npos ||
        lowerClassName.find("arkhuman") != std::string::npos ||
        lowerClassName.find("arknpc") != std::string::npos ||
        lowerClassName.find("arkmimic") != std::string::npos ||
        lowerClassName.find("arkphantom") != std::string::npos ||
        lowerClassName.find("arknightmare") != std::string::npos ||
        lowerClassName.find("arkoperator") != std::string::npos ||
        lowerClassName.find("arkweaver") != std::string::npos ||
        lowerClassName.find("arkcystoid") != std::string::npos ||
        lowerClassName.find("apextentacle") != std::string::npos ||
        lowerClassName == "arkcargocontainer" ||
        lowerClassName == "arkrotator" ||
        lowerClassName.find("arklight") != std::string::npos ||
        lowerClassName.find("leveltransition") != std::string::npos ||
        lowerClassName.find("trigger") != std::string::npos ||
        lowerClassName.find("volume") != std::string::npos ||
        lowerClassName.find("flowgraph") != std::string::npos;
}

bool IsBlockedLivePropIdentity(const std::string& lowerValue)
{
    auto containsDelimitedToken = [&lowerValue](std::string_view token)
    {
        size_t offset = 0;
        while ((offset = lowerValue.find(token, offset)) != std::string::npos)
        {
            const bool leftBoundary =
                offset == 0 ||
                !std::isalnum(static_cast<unsigned char>(lowerValue[offset - 1]));
            const size_t rightOffset = offset + token.size();
            const bool rightBoundary =
                rightOffset == lowerValue.size() ||
                !std::isalnum(static_cast<unsigned char>(lowerValue[rightOffset]));
            if (leftBoundary && rightBoundary)
                return true;
            ++offset;
        }
        return false;
    };

    return lowerValue.find("projectile") != std::string::npos ||
        lowerValue.find("arkprojectiles.") != std::string::npos ||
        containsDelimitedToken("gloo") ||
        containsDelimitedToken("goo");
}

std::string ReadLivePropLevelName(const CoopProtocol::LivePropTransformPacket& packet)
{
    return ReadFixedString(packet.levelName, sizeof(packet.levelName));
}

bool IsSyntheticLivePropGuid(uint64_t guid)
{
    return (guid & kLivePropSyntheticGuidMask) != 0;
}

uint64_t BuildSyntheticLivePropGuid(EntityId entityId)
{
    if (entityId == INVALID_ENTITYID)
        return 0;

    return kLivePropSyntheticGuidMask | static_cast<uint32_t>(entityId);
}

EntityId DecodeSyntheticLivePropEntityId(uint64_t guid)
{
    if (!IsSyntheticLivePropGuid(guid))
        return INVALID_ENTITYID;

    return static_cast<EntityId>(guid & 0xFFFFFFFFULL);
}

bool IsLivePropCorpseNpc(ArkNpc* npc, bool localCarried, std::string& reason)
{
    if (!npc || !IsLikelyRuntimeCppObject(npc))
    {
        return false;
    }

    if (localCarried)
        return true;

    bool npcDead = false;
    if (TryGuardedCall("live prop ArkNpc::IsDead", [npc]() { return npc->IsDead(); }, npcDead, &reason) &&
        npcDead)
    {
        return true;
    }

    bool npcRagdolled = false;
    if (TryGuardedCall("live prop ArkNpc::IsRagdolled", [npc]() { return npc->IsRagdolled(); }, npcRagdolled, &reason) &&
        npcRagdolled)
    {
        return true;
    }

    reason = "live npc";
    return false;
}

bool IsLivePropCorpseEntity(IEntity& entity, bool localCarried, std::string& reason)
{
    ArkNpc* npc = nullptr;
    if (!TryGuardedCall("live prop EntityUtils::GetArkNpc", [&entity]() { return EntityUtils::GetArkNpc(&entity); }, npc, &reason) ||
        !npc)
    {
        return false;
    }

    return IsLivePropCorpseNpc(npc, localCarried, reason);
}

bool IsLocalPlayerCarryingEntity(EntityId entityId)
{
    if (entityId == INVALID_ENTITYID || !ArkPlayer::GetInstancePtr())
        return false;

    unsigned carriedEntityId = INVALID_ENTITYID;
    std::string reason;
    return TryGuardedCall(
            "live prop local ArkPlayerCarry picked entity",
            []() -> unsigned
            {
                return ArkPlayer::GetInstance().m_interaction.m_playerCarry.m_pickedUpEntityId;
            },
            carriedEntityId,
            &reason) &&
        carriedEntityId == entityId;
}

EntityId GetLocalCarriedEntityId()
{
    if (!ArkPlayer::GetInstancePtr())
        return INVALID_ENTITYID;

    unsigned carriedEntityId = INVALID_ENTITYID;
    std::string reason;
    if (!TryGuardedCall(
            "live prop local carried entity id",
            []() -> unsigned
            {
                return ArkPlayer::GetInstance().m_interaction.m_playerCarry.m_pickedUpEntityId;
            },
            carriedEntityId,
            &reason))
    {
        return INVALID_ENTITYID;
    }

    return carriedEntityId;
}

bool IsFiniteFloat(float value)
{
    return std::isfinite(value) && std::fabs(value) <= kLivePropMaxAbsCoordinate;
}

bool IsFiniteVec3(const Vec3& value)
{
    return IsFiniteFloat(value.x) && IsFiniteFloat(value.y) && IsFiniteFloat(value.z);
}

bool IsFiniteQuat(const Quat& value)
{
    return std::isfinite(value.w) &&
        std::isfinite(value.v.x) &&
        std::isfinite(value.v.y) &&
        std::isfinite(value.v.z);
}

bool IsReasonableScale(const Vec3& value)
{
    return IsFiniteVec3(value) &&
        value.x > 0.0001f &&
        value.y > 0.0001f &&
        value.z > 0.0001f &&
        value.x <= 100.0f &&
        value.y <= 100.0f &&
        value.z <= 100.0f;
}

Vec3 SanitizeLivePropVelocity(const Vec3& value)
{
    if (!IsFiniteVec3(value) || value.GetLengthSquared() > kLivePropMaxVelocitySq)
        return Vec3(ZERO);
    return value;
}

Vec3 SanitizeLivePropAngularVelocity(const Vec3& value)
{
    if (!IsFiniteVec3(value) || value.GetLengthSquared() > kLivePropMaxAngularVelocitySq)
        return Vec3(ZERO);
    return value;
}

Vec3 ClampLivePropVectorLength(const Vec3& value, float maxLength)
{
    if (!IsFiniteVec3(value) || maxLength <= 0.0f)
        return Vec3(ZERO);

    const float lengthSq = value.GetLengthSquared();
    const float maxLengthSq = maxLength * maxLength;
    if (lengthSq <= maxLengthSq)
        return value;

    const float length = std::sqrt(lengthSq);
    if (length <= 0.0001f)
        return Vec3(ZERO);

    return value * (maxLength / length);
}

bool CapturePhysicsDynamics(IEntity& entity, Vec3& velocity, Vec3& angularVelocity)
{
    velocity = Vec3(ZERO);
    angularVelocity = Vec3(ZERO);

    IPhysicalEntity* physics = nullptr;
    std::string reason;
    if (!TryGuardedCall("live prop IEntity::GetPhysics", [&entity]() { return entity.GetPhysics(); }, physics, &reason) ||
        !physics)
    {
        return false;
    }

    pe_status_dynamics dynamics;
    int statusResult = 0;
    if (!TryGuardedCall(
            "live prop physics GetStatus dynamics",
            [physics, &dynamics]() { return physics->GetStatus(&dynamics); },
            statusResult,
            &reason) ||
        statusResult <= 0)
    {
        return false;
    }

    velocity = dynamics.v;
    angularVelocity = dynamics.w;
    return true;
}

std::string FormatVec3Compact(const Vec3& value)
{
    return std::to_string(value.x) + "," + std::to_string(value.y) + "," + std::to_string(value.z);
}

bool TryReadLivePropCollisionPayload(const SEntityEvent& event, EventPhysCollision& outCollision, std::string& reason)
{
    const auto* collision = reinterpret_cast<const EventPhysCollision*>(event.nParam[0]);
    if (!collision)
    {
        reason = "collision event has no payload";
        return false;
    }

    if (!PreflightRuntimePointer(
            "live prop collision payload",
            collision,
            sizeof(EventPhysCollision),
            RuntimeAccess::Read,
            &reason))
    {
        return false;
    }

    return TryGuardedVoidCall(
        "live prop collision copy payload",
        [collision, &outCollision]()
        {
            std::memcpy(&outCollision, collision, sizeof(EventPhysCollision));
        },
        &reason);
}

IEntity* ResolveLivePropCollisionPhysicsEntity(IPhysicalEntity* physics, std::string& reason)
{
    if (!physics || physics == WORLD_ENTITY || !gEnv || !gEnv->pEntitySystem)
        return nullptr;

    if (!IsLikelyRuntimeCppObject(physics))
    {
        reason = "collision physics pointer is not a runtime object";
        return nullptr;
    }

    IEntity* entity = nullptr;
    if (!TryGuardedCall(
            "live prop collision GetEntityFromPhysics",
            [physics]() { return gEnv->pEntitySystem->GetEntityFromPhysics(physics); },
            entity,
            &reason))
    {
        return nullptr;
    }

    return entity;
}

std::string DescribeCarryEntityForTrace(EntityId entityId)
{
    if (entityId == INVALID_ENTITYID || !gEnv || !gEnv->pEntitySystem)
        return " entity=invalid";

    IEntity* entity = nullptr;
    std::string reason;
    if (!TryGuardedCall("carry trace GetEntity", [entityId]() { return gEnv->pEntitySystem->GetEntity(entityId); }, entity, &reason) ||
        !entity)
    {
        return " entity=" + std::to_string(entityId) + " missing";
    }

    uint64_t guid = 0;
    const char* rawName = "";
    IEntityClass* entityClass = nullptr;
    const char* rawClassName = "";
    Vec3 position(ZERO);
    Vec3 velocity(ZERO);
    Vec3 angularVelocity(ZERO);
    TryGuardedCall("carry trace IEntity::GetGuid", [entity]() { return entity->GetGuid(); }, guid, &reason);
    TryGuardedCall("carry trace IEntity::GetName", [entity]() { return entity->GetName(); }, rawName, &reason);
    if (TryGuardedCall("carry trace IEntity::GetClass", [entity]() { return entity->GetClass(); }, entityClass, &reason) &&
        entityClass)
    {
        TryGuardedCall("carry trace IEntityClass::GetName", [entityClass]() { return entityClass->GetName(); }, rawClassName, &reason);
    }
    TryGuardedCall("carry trace IEntity::GetWorldPos", [entity]() { return entity->GetWorldPos(); }, position, &reason);
    CapturePhysicsDynamics(*entity, velocity, angularVelocity);

    return " entity=" + std::to_string(entityId) +
        " guid=" + std::to_string(guid) +
        " name=" + ReadRuntimeCString(rawName, 48) +
        " class=" + ReadRuntimeCString(rawClassName, 48) +
        " pos=" + FormatVec3Compact(position) +
        " vel=" + FormatVec3Compact(velocity) +
        " angVel=" + FormatVec3Compact(angularVelocity);
}

bool TryRepairHugeLivePropPhysicsBounds(IEntity& entity, IPhysicalEntity* physics, std::string& reason)
{
    if (!physics)
        return true;

    pe_status_pos status;
    int statusResult = 0;
    if (!TryGuardedCall(
            "live prop physics bounds status",
            [physics, &status]() { return physics->GetStatus(&status); },
            statusResult,
            &reason) ||
        statusResult <= 0)
    {
        return true;
    }

    const Vec3 extents = status.BBox[1] - status.BBox[0];
    if (!IsFiniteVec3(status.BBox[0]) ||
        !IsFiniteVec3(status.BBox[1]) ||
        extents.x > 50.0f ||
        extents.y > 50.0f ||
        extents.z > 50.0f)
    {
        TryGuardedVoidCall(
            "live prop repair huge physics bounds disable",
            [&entity]() { entity.EnablePhysics(false); },
            &reason);
        TryGuardedVoidCall(
            "live prop repair huge physics bounds enable",
            [&entity]() { entity.EnablePhysics(true); },
            &reason);
    }

    return true;
}

bool ApplyPhysicsTransform(IEntity& entity, const Vec3& position, const Quat& rotation, const Vec3& scale, std::string& reason)
{
    (void)position;
    (void)rotation;
    (void)scale;

    IPhysicalEntity* physics = nullptr;
    if (!TryGuardedCall("live prop apply physics GetPhysics", [&entity]() { return entity.GetPhysics(); }, physics, &reason) ||
        !physics)
    {
        return true;
    }

    TryRepairHugeLivePropPhysicsBounds(entity, physics, reason);

    pe_action_awake awake;
    awake.bAwake = 1;
    awake.minAwakeTime = 0.15f;
    TryGuardedVoidCall(
        "live prop apply physics awake",
        [physics, &awake]() { physics->Action(&awake); },
        &reason);
    return true;
}

bool ApplyPhysicsVelocityAndWake(IEntity& entity, const Vec3& velocity, const Vec3& angularVelocity, float minAwakeTime, std::string& reason)
{
    IPhysicalEntity* physics = nullptr;
    if (!TryGuardedCall("live prop apply velocity GetPhysics", [&entity]() { return entity.GetPhysics(); }, physics, &reason) ||
        !physics)
    {
        return true;
    }

    pe_action_set_velocity velocityAction;
    velocityAction.v = SanitizeLivePropVelocity(velocity);
    velocityAction.w = SanitizeLivePropAngularVelocity(angularVelocity);
    if (!TryGuardedVoidCall(
            "live prop apply physics velocity",
            [physics, &velocityAction]()
            {
                physics->Action(&velocityAction);
            },
            &reason))
    {
        return false;
    }

    pe_action_awake awake;
    awake.bAwake = 1;
    awake.minAwakeTime = minAwakeTime;
    TryGuardedVoidCall(
        "live prop apply velocity awake",
        [physics, &awake]() { physics->Action(&awake); },
        &reason);
    return true;
}

bool StopAndSleepPhysics(
    IEntity& entity,
    const Vec3& position,
    const Quat& rotation,
    std::string& reason)
{
    IPhysicalEntity* physics = nullptr;
    if (!TryGuardedCall("live prop settle GetPhysics", [&entity]() { return entity.GetPhysics(); }, physics, &reason) ||
        !physics)
    {
        return true;
    }

    pe_params_pos physicsPose;
    physicsPose.pos = position;
    physicsPose.q = rotation;
    int poseResult = 0;
    if (!TryGuardedCall(
            "live prop settle physics pose",
            [physics, &physicsPose]() { return physics->SetParams(&physicsPose); },
            poseResult,
            &reason) ||
        poseResult == 0)
    {
        return false;
    }

    pe_action_set_velocity velocityAction;
    velocityAction.v = Vec3(ZERO);
    velocityAction.w = Vec3(ZERO);
    if (!TryGuardedVoidCall(
            "live prop settle zero velocity",
            [physics, &velocityAction]() { physics->Action(&velocityAction); },
            &reason))
    {
        return false;
    }

    pe_action_awake awake;
    awake.bAwake = 0;
    return TryGuardedVoidCall(
        "live prop settle sleep",
        [physics, &awake]() { physics->Action(&awake); },
        &reason);
}

bool SetEntityPhysicsEnabledGuarded(IEntity& entity, bool enabled, std::string& reason)
{
    return TryGuardedVoidCall(
        enabled ? "live prop EnablePhysics true" : "live prop EnablePhysics false",
        [&entity, enabled]()
        {
            entity.EnablePhysics(enabled);
        },
        &reason);
}

bool RotationChangedEnough(const Quat& a, const Quat& b, float epsilon)
{
    return std::fabs(a.w - b.w) >= epsilon ||
        std::fabs(a.v.x - b.v.x) >= epsilon ||
        std::fabs(a.v.y - b.v.y) >= epsilon ||
        std::fabs(a.v.z - b.v.z) >= epsilon;
}
}

void ModMain::ResetLivePropSyncState(const char* reason)
{
    m_liveProps.clear();
    m_areaOverlayLivePropCandidates.clear();
    m_areaOverlayLivePropCandidateUntilTime = -1000.0f;
    m_livePropSequence = 0;
    m_sentLivePropPackets = 0;
    m_receivedLivePropPackets = 0;
    m_appliedLivePropPackets = 0;
    m_droppedLivePropPackets = 0;
    m_sentLivePropBytes = 0;
    m_receivedLivePropBytes = 0;
    m_livePropCarriedApplies = 0;
    m_livePropMovingApplies = 0;
    m_livePropIdleApplies = 0;
    m_livePropBallisticStarts = 0;
    m_livePropBallisticApplies = 0;
    m_livePropBallisticCorrections = 0;
    m_livePropThrowCalls = 0;
    m_livePropThrowSuccesses = 0;
    m_livePropStopCarryCalls = 0;
    m_livePropStopCarryThrown = 0;
    m_livePropStopCarrySerializeSkips = 0;
    m_livePropCollisionEvents = 0;
    m_livePropCollisionAuthorityGrants = 0;
    m_livePropCollisionChainGrants = 0;
    m_livePropAttackCollisionGrants = 0;
    m_livePropEnemyBodyCollisionGrants = 0;
    m_livePropRemoteProxyCollisionSuppressions = 0;
    m_livePropRemoteProxyContactSuppressUntil.clear();
    m_livePropTickAccumulator = 0.0f;
    m_livePropApplyActive = false;
    g_livePropLastCarriedEntityId = INVALID_ENTITYID;
    g_livePropLastCarriedGuid = 0;
    g_livePropCarrySuppressEntityId = INVALID_ENTITYID;
    g_livePropCarrySuppressGuid = 0;
    g_livePropCarrySuppressUntilTime = -1000.0f;
    g_livePropPendingThrowEntityId = INVALID_ENTITYID;
    g_livePropPendingThrowUntilTime = -1000.0f;
    m_lastLivePropEvent = reason ? reason : "live prop reset";
    m_lastLivePropCarryEvent = m_lastLivePropEvent;
    ResetLivePropDebugTrace(0, reason ? reason : "live prop reset");
}

void ModMain::PromoteAreaOverlayLiveProp(LivePropState&& snapshot, float authorityUntilTime)
{
    const float now = NowSeconds();
    LivePropState& state = m_liveProps[snapshot.guid];
    const uint32_t lastReceivedSequence = state.lastReceivedSequence;
    const float lastAppliedTime = state.lastAppliedTime;

    state = std::move(snapshot);
    state.lastReceivedSequence = lastReceivedSequence;
    state.lastSentTime = -1000.0f;
    state.lastAppliedTime = lastAppliedTime;
    state.localAuthorityUntilTime = authorityUntilTime;
    state.remoteAuthorityUntilTime = -1000.0f;
    state.forceSendUntilTime = authorityUntilTime;
    state.contactAuthorityUntilTime = -1000.0f;
    state.releaseMotionUntilTime = -1000.0f;
    state.remoteBallisticUntilTime = -1000.0f;
    state.remoteBlendStartTime = -1000.0f;
    state.remoteBlendDuration = kLivePropHostImpulseBlendSeconds;
    state.flags &= ~(CoopProtocol::kLivePropTransformFlagCarried |
        CoopProtocol::kLivePropTransformFlagClientAuthority |
        CoopProtocol::kLivePropTransformFlagImpulse);
    state.flags |= CoopProtocol::kLivePropTransformFlagActive;
    state.carried = false;
    state.activelyMoving = true;
    state.remoteBallisticActive = false;
    state.remoteBallisticJustStarted = false;
    state.remoteLaunchVelocityApplied = false;
    state.pendingRemoteApply = false;
    state.remoteApplyStepsRemaining = 0;
    state.leasePhase = LivePropLeasePhase::FlightLocal;
    state.activeUntilTime = authorityUntilTime;
    state.lastQueuedTime = now;
    state.dirty = true;
    AppendLivePropDebugTrace(
        "area_overlay.promote",
        state.guid,
        state.entityId,
        &state,
        "host authoritative settle capture");
}

void ModMain::SeedLivePropsAfterAreaOverlay(const std::vector<uint64_t>& transformedGuids)
{
    m_areaOverlayLivePropCandidates.clear();
    m_areaOverlayLivePropCandidateUntilTime = -1000.0f;
    if (m_networkMode != CoopNetworkMode::Host ||
        transformedGuids.empty() ||
        !m_hasRemoteSession ||
        !m_hasRemoteEndpoint ||
        !IsSessionGameplayReady() ||
        !IsKnownSameLevel(m_localLevelName, m_remoteLevelName))
    {
        return;
    }

    const float authorityUntilTime = NowSeconds() + kLivePropSettleActiveSeconds;
    for (const uint64_t guid : transformedGuids)
    {
        if (guid == 0)
            continue;

        const bool wasTracked = m_liveProps.find(guid) != m_liveProps.end();
        std::string reason;
        IEntity* entity = ResolveLivePropEntity(guid, INVALID_ENTITYID, reason);
        if (!entity)
            continue;

        IPhysicalEntity* physics = nullptr;
        if (!TryGuardedCall(
                "area overlay live prop GetPhysics",
                [entity]() { return entity->GetPhysics(); },
                physics,
                &reason) ||
            !physics)
        {
            continue;
        }

        LivePropState snapshot;
        if (!CaptureLivePropState(*entity, false, snapshot, reason, wasTracked))
            continue;

        if (wasTracked || snapshot.activelyMoving)
            PromoteAreaOverlayLiveProp(std::move(snapshot), authorityUntilTime);
        else
            m_areaOverlayLivePropCandidates.insert(guid);
    }

    if (!m_areaOverlayLivePropCandidates.empty())
        m_areaOverlayLivePropCandidateUntilTime = authorityUntilTime;
}

void ModMain::ResetLivePropDebugTrace(uint64_t focusGuid, const char* reason)
{
    m_livePropDebugFocusGuid = focusGuid;
    m_livePropDebugTraceSerial = 0;
    m_livePropDebugTrace.clear();
    m_lastLivePropDebugTrace =
        std::string("debug_focus=") + std::to_string(focusGuid) +
        " reason=" + (reason && reason[0] ? reason : "reset");
    LogCoop("live prop debug trace reset " + m_lastLivePropDebugTrace);
}

void ModMain::AppendLivePropDebugTrace(
    const char* stage,
    uint64_t guid,
    EntityId entityId,
    const LivePropState* state,
    const char* detail)
{
    if (guid == 0 && entityId != INVALID_ENTITYID && gEnv && gEnv->pEntitySystem)
    {
        std::string reason;
        IEntity* entity = nullptr;
        if (TryGuardedCall("live prop debug GetEntity", [entityId]() { return gEnv->pEntitySystem->GetEntity(entityId); }, entity, &reason) &&
            entity)
        {
            TryGuardedCall("live prop debug GetGuid", [entity]() { return entity->GetGuid(); }, guid, &reason);
        }
    }

    if (guid == 0)
        return;

    if (m_livePropDebugFocusGuid == 0 &&
        (m_livePropTraceEvents || m_livePropOpenEventFilter || EnvFlagEnabled("COOP_LIVE_PROP_TRACE_EVENTS")))
    {
        m_livePropDebugFocusGuid = guid;
    }

    if (m_livePropDebugFocusGuid == 0 || guid != m_livePropDebugFocusGuid)
        return;

    std::string line =
        std::to_string(++m_livePropDebugTraceSerial) +
        ":" + (stage && stage[0] ? stage : "unknown") +
        ":guid=" + std::to_string(guid);
    if (entityId != INVALID_ENTITYID)
        line += ":id=" + std::to_string(entityId);

    if (state)
    {
        line +=
            ":carried=" + std::to_string(state->carried ? 1 : 0) +
            ":active=" + std::to_string(state->activelyMoving ? 1 : 0) +
            ":flags=" + std::to_string(state->flags) +
            ":pos=" + FormatVec3Compact(state->position) +
            ":target=" + FormatVec3Compact(state->targetPosition) +
            ":vel=" + FormatVec3Compact(state->velocity) +
            ":phase=" + GetLivePropLeasePhaseName(state->leasePhase) +
            ":auth=" + std::to_string(state->localAuthorityUntilTime > NowSeconds() ? 1 : 0) +
            ":remote=" + std::to_string(state->remoteAuthorityUntilTime > NowSeconds() ? 1 : 0);
    }

    if (detail && detail[0])
        line += ":detail=" + std::string(detail);

    constexpr size_t kLivePropDebugTraceMaxEntries = 16;
    m_livePropDebugTrace.push_back(line);
    while (m_livePropDebugTrace.size() > kLivePropDebugTraceMaxEntries)
        m_livePropDebugTrace.pop_front();

    m_lastLivePropDebugTrace = line;
    LogCoop("live_prop_debug " + line);
}

const char* ModMain::GetLivePropLeasePhaseName(LivePropLeasePhase phase) const
{
    switch (phase)
    {
    case LivePropLeasePhase::Settled:
        return "Settled";
    case LivePropLeasePhase::CarriedLocal:
        return "CarriedLocal";
    case LivePropLeasePhase::CarriedRemote:
        return "CarriedRemote";
    case LivePropLeasePhase::FlightLocal:
        return "FlightLocal";
    case LivePropLeasePhase::FlightRemote:
        return "FlightRemote";
    case LivePropLeasePhase::Settling:
        return "Settling";
    default:
        return "Unknown";
    }
}

bool ModMain::IsLivePropLocalLeasePhase(LivePropLeasePhase phase) const
{
    return phase == LivePropLeasePhase::CarriedLocal ||
        phase == LivePropLeasePhase::FlightLocal;
}

bool ModMain::IsLivePropRemoteLeasePhase(LivePropLeasePhase phase) const
{
    return phase == LivePropLeasePhase::CarriedRemote ||
        phase == LivePropLeasePhase::FlightRemote;
}

bool ModMain::IsLivePropRemoteProtected(const LivePropState& state, float now) const
{
    const bool remotePhase = IsLivePropRemoteLeasePhase(state.leasePhase);
    const bool remoteCarried =
        state.carried ||
        (state.flags & CoopProtocol::kLivePropTransformFlagCarried) != 0;
    const bool remoteBallistic =
        state.remoteBallisticActive &&
        now <= state.remoteBallisticUntilTime;
    const bool remoteActive =
        state.activelyMoving ||
        (state.flags & CoopProtocol::kLivePropTransformFlagActive) != 0 ||
        (state.flags & CoopProtocol::kLivePropTransformFlagImpulse) != 0;
    return state.remoteAuthorityUntilTime > now &&
        (remotePhase || remoteCarried || remoteBallistic || remoteActive);
}

bool ModMain::ShouldTrackLivePropEntity(IEntity& entity, std::string& reason, bool allowCarryableItem) const
{
    EntityId entityId = INVALID_ENTITYID;
    if (!TryGuardedCall("live prop IEntity::GetId", [&entity]() { return entity.GetId(); }, entityId, &reason) ||
        entityId == INVALID_ENTITYID)
    {
        return false;
    }

    if (entityId == m_proxyEntityId || entityId == m_mimicEntityId || IsEnemyPuppetEntity(entityId))
    {
        reason = "coop runtime entity";
        return false;
    }
    if (m_sharedDropByEntityId.find(entityId) != m_sharedDropByEntityId.end())
    {
        reason = "shared drop entity";
        return false;
    }
    if (m_remoteGooResultEntityIds.find(entityId) != m_remoteGooResultEntityIds.end() ||
        m_remoteGooEntityToSourceIds.find(entityId) != m_remoteGooEntityToSourceIds.end())
    {
        reason = "network goo result entity";
        return false;
    }

    if (ArkPlayer::GetInstancePtr())
    {
        IEntity* playerEntity = ArkPlayer::GetInstance().GetEntity();
        if (playerEntity && playerEntity->GetId() == entityId)
        {
            reason = "local player";
            return false;
        }
    }

    const bool localCarried = IsLocalPlayerCarryingEntity(entityId);
    allowCarryableItem = allowCarryableItem || localCarried;

    std::string npcReason;
    bool corpseNpc = false;
    ArkNpc* npc = nullptr;
    if (!TryGuardedCall("live prop EntityUtils::GetArkNpc", [&entity]() { return EntityUtils::GetArkNpc(&entity); }, npc, &npcReason))
    {
        reason = npcReason.empty() ? "npc probe failed" : npcReason;
        return false;
    }
    if (npc)
    {
        corpseNpc = IsLikelyRuntimeCppObject(npc) &&
            IsLivePropCorpseNpc(npc, localCarried, npcReason);
        if (corpseNpc)
        {
            return true;
        }

        reason = npcReason.empty() ? "live npc" : npcReason;
        return false;
    }

    CArkItem* arkItem = nullptr;
    if (!allowCarryableItem &&
        TryGuardedCall("live prop CArkItem::GetItemFromEntityId", [entityId]() { return CArkItem::GetItemFromEntityId(entityId); }, arkItem, &reason) &&
        arkItem &&
        IsLikelyRuntimeCppObject(arkItem))
    {
        reason = "ark inventory item";
        return false;
    }

    const char* rawName = nullptr;
    TryGuardedCall("live prop IEntity::GetName", [&entity]() { return entity.GetName(); }, rawName, &reason);
    const std::string entityName = ToLowerAscii(ReadRuntimeCString(rawName, 128));
    if (IsBlockedLivePropIdentity(entityName))
    {
        reason = "untracked projectile entity " + entityName;
        return false;
    }
    if (entityName.rfind("coop_", 0) == 0 ||
        entityName.rfind("coopshareddrop_", 0) == 0 ||
        entityName.find("coopprototype") != std::string::npos)
    {
        reason = "coop named entity";
        return false;
    }

    IEntityClass* entityClass = nullptr;
    if (!TryGuardedCall("live prop IEntity::GetClass", [&entity]() { return entity.GetClass(); }, entityClass, &reason) ||
        !IsLikelyRuntimeCppObject(entityClass))
    {
        if (allowCarryableItem)
            return true;
        return false;
    }

    const char* rawClass = nullptr;
    TryGuardedCall("live prop IEntityClass::GetName", [entityClass]() { return entityClass->GetName(); }, rawClass, &reason);
    const std::string className = ToLowerAscii(ReadRuntimeCString(rawClass, 128));
    const bool carriedTurretTransform =
        allowCarryableItem && className.find("arkturret") != std::string::npos;

    IEntityArchetype* archetype = nullptr;
    const bool hasArchetype =
        TryGuardedCall("live prop IEntity::GetArchetype", [&entity]() { return entity.GetArchetype(); }, archetype, &reason) &&
        IsLikelyRuntimeCppObject(archetype);
    bool carriedArkLightTransform = false;
    if (allowCarryableItem && className == "arklight" && hasArchetype)
    {
        IScriptTable* properties = nullptr;
        bool propertyRead = false;
        bool isCarryable = false;
        std::string propertyReason;
        carriedArkLightTransform =
            TryGuardedCall(
                "live prop carryable ArkLight GetProperties",
                [archetype]() { return archetype->GetProperties(); },
                properties,
                &propertyReason) &&
            IsLikelyRuntimeCppObject(properties) &&
            TryGuardedCall(
                "live prop carryable ArkLight bIsCarryable",
                [properties, &isCarryable]() { return properties->GetValue("bIsCarryable", isCarryable); },
                propertyRead,
                &propertyReason) &&
            propertyRead &&
            isCarryable;
    }

    // Carryable floor lamps use ArkLight and can remain PE_STATIC while at
    // rest. Admit only archetypes marked bIsCarryable through the proven
    // carry/collision path; ordinary lights keep native device transforms.
    if (IsBlockedLivePropClass(className) &&
        !carriedTurretTransform &&
        !carriedArkLightTransform)
    {
        reason = "untracked class " + className;
        return false;
    }

    // Turrets retain their typed state/AI lane, but their native carry system
    // still needs the proven LiveProp transform lease while a player holds or
    // releases one. No idle turret is admitted through this exception.

    if (hasArchetype)
    {
        const char* rawArchetypeName = nullptr;
        TryGuardedCall("live prop IEntityArchetype::GetName", [archetype]() { return archetype->GetName(); }, rawArchetypeName, &reason);
        const std::string archetypeName = ToLowerAscii(ReadRuntimeCString(rawArchetypeName, 160));
        if (IsBlockedLivePropIdentity(archetypeName))
        {
            reason = "untracked projectile archetype " + archetypeName;
            return false;
        }
    }

    if (!allowCarryableItem &&
        (className.empty() ||
            className.find("arkitem") != std::string::npos ||
            className.find("arkpickup") != std::string::npos ||
            className.find("inventory") != std::string::npos ||
            className.find("arkweapon") != std::string::npos ||
            className.find("arkammo") != std::string::npos ||
            className.find("neuromod") != std::string::npos))
    {
        reason = "untracked class " + className;
        return false;
    }

    return true;
}

void ModMain::RetireLivePropTrackingForSharedDrop(EntityId entityId)
{
    if (entityId == INVALID_ENTITYID)
        return;

    for (auto it = m_liveProps.begin(); it != m_liveProps.end();)
    {
        if (it->second.entityId != entityId)
        {
            ++it;
            continue;
        }
        m_areaOverlayLivePropCandidates.erase(it->first);
        it = m_liveProps.erase(it);
    }
}

bool ModMain::CaptureLivePropState(IEntity& entity, bool removed, LivePropState& outState, std::string& reason, bool allowCarryableItem) const
{
    if (!ShouldTrackLivePropEntity(entity, reason, allowCarryableItem))
        return false;

    outState = {};
    outState.levelName = NormalizeLevelName(GetCurrentLevelName());
    outState.levelId = HashLevelName(outState.levelName);
    if (outState.levelName.empty() || outState.levelName == "unknown")
    {
        reason = "unknown level";
        return false;
    }

    uint64_t rawGuid = 0;
    if (!TryGuardedCall("live prop IEntity::GetId capture", [&entity]() { return entity.GetId(); }, outState.entityId, &reason) ||
        !TryGuardedCall("live prop IEntity::GetGuid capture", [&entity]() { return entity.GetGuid(); }, rawGuid, &reason) ||
        !TryGuardedCall("live prop IEntity::GetWorldPos capture", [&entity]() { return entity.GetWorldPos(); }, outState.position, &reason) ||
        !TryGuardedCall("live prop IEntity::GetWorldRotation capture", [&entity]() { return entity.GetWorldRotation(); }, outState.rotation, &reason) ||
        !TryGuardedCall("live prop IEntity::GetScale capture", [&entity]() { return Vec3(entity.GetScale()); }, outState.scale, &reason))
    {
        return false;
    }

    outState.guid = rawGuid != 0 ? rawGuid : BuildSyntheticLivePropGuid(outState.entityId);
    if (outState.guid == 0)
    {
        reason = "zero guid";
        return false;
    }

    bool hidden = false;
    if (TryGuardedCall("live prop IEntity::IsHidden capture", [&entity]() { return entity.IsHidden(); }, hidden, &reason) && hidden)
        outState.flags |= CoopProtocol::kLivePropTransformFlagHidden;
    if (removed)
        outState.flags |= CoopProtocol::kLivePropTransformFlagRemoved;

    outState.carried = IsLocalPlayerCarryingEntity(outState.entityId);
    if (outState.carried && IsCarrySuppressActiveFor(outState.entityId, outState.guid, NowSeconds()))
        outState.carried = false;
    if (!outState.carried &&
        IsLobbyMainLiftLooseCargoPosition(outState.levelName, outState.position))
    {
        reason = "peer-local main lift loose cargo";
        return false;
    }

    std::string corpseReason;
    const bool corpseNpc = IsLivePropCorpseEntity(entity, outState.carried, corpseReason);
    if (outState.carried && !corpseNpc && EnvFlagDefaultEnabled("COOP_LIVE_PROP_USE_CARRY_TARGET") && ArkPlayer::GetInstancePtr())
    {
        QuatT carryTarget(IDENTITY);
        std::string carryReason;
        if (TryGuardedCall(
                "live prop ArkPlayerCarry GetLerpTargetLocation",
                [&entity]() -> QuatT
                {
                    auto& carry = ArkPlayer::GetInstance().m_interaction.m_playerCarry;
                    return carry.GetLerpTargetLocation(&entity, carry.m_pickupEntityOriginalRotation);
                },
                carryTarget,
                &carryReason) &&
            IsFiniteVec3(carryTarget.t) &&
            IsFiniteQuat(carryTarget.q))
        {
            outState.position = carryTarget.t;
            outState.rotation = carryTarget.q;
            outState.rotation.Normalize();
        }
    }

    if (!IsFiniteVec3(outState.position) ||
        !IsFiniteQuat(outState.rotation) ||
        !IsReasonableScale(outState.scale))
    {
        reason = "invalid transform";
        return false;
    }

    if (outState.carried)
        outState.flags |= CoopProtocol::kLivePropTransformFlagCarried;

    if (!outState.carried)
        CapturePhysicsDynamics(entity, outState.velocity, outState.angularVelocity);
    if (!IsFiniteVec3(outState.velocity) || outState.velocity.GetLengthSquared() > kLivePropMaxVelocitySq)
        outState.velocity = Vec3(ZERO);
    if (!IsFiniteVec3(outState.angularVelocity) || outState.angularVelocity.GetLengthSquared() > kLivePropMaxAngularVelocitySq)
        outState.angularVelocity = Vec3(ZERO);
    outState.activelyMoving =
        outState.carried ||
        outState.velocity.GetLengthSquared() >= kLivePropVelocityActiveSq ||
        outState.angularVelocity.GetLengthSquared() >= kLivePropAngularVelocityActiveSq;
    if (outState.activelyMoving)
        outState.flags |= CoopProtocol::kLivePropTransformFlagActive;
    if (!removed && (outState.carried || outState.activelyMoving))
        outState.flags &= ~CoopProtocol::kLivePropTransformFlagHidden;
    outState.activeUntilTime =
        outState.activelyMoving ? NowSeconds() + kLivePropSettleActiveSeconds : NowSeconds();

    outState.hasSnapshot = true;
    outState.lastQueuedTime = NowSeconds();
    return true;
}

bool ModMain::BuildLivePropTransformPacket(CoopProtocol::LivePropTransformPacket& packet, const LivePropState& state)
{
    if (!state.hasSnapshot || state.guid == 0 || state.levelName.empty())
        return false;

    packet = {};
    packet.magic = CoopProtocol::kPacketMagic;
    packet.version = CoopProtocol::kProtocolVersion;
    packet.type = static_cast<uint16_t>(CoopProtocol::PacketType::LivePropTransform);
    packet.sequence = CoopSerialSequence::Advance(m_livePropSequence);
    packet.worldEpoch = m_localWorldEpoch;
    packet.levelId = state.levelId;
    packet.guid = state.guid;
    packet.px = state.position.x;
    packet.py = state.position.y;
    packet.pz = state.position.z;
    packet.qw = state.rotation.w;
    packet.qx = state.rotation.v.x;
    packet.qy = state.rotation.v.y;
    packet.qz = state.rotation.v.z;
    packet.sx = state.scale.x;
    packet.sy = state.scale.y;
    packet.sz = state.scale.z;
    packet.vx = state.velocity.x;
    packet.vy = state.velocity.y;
    packet.vz = state.velocity.z;
    packet.wx = state.angularVelocity.x;
    packet.wy = state.angularVelocity.y;
    packet.wz = state.angularVelocity.z;
    packet.flags = state.flags;
    packet.sourceAccountToken = GetLocalAccountToken();
    const auto turretAuthorityIt = m_turretAuthorities.find(state.guid);
    if (turretAuthorityIt != m_turretAuthorities.end())
        packet.authorityEpoch = turretAuthorityIt->second.epoch;
    if (state.activelyMoving || (state.flags & CoopProtocol::kLivePropTransformFlagImpulse) != 0)
        packet.flags |= CoopProtocol::kLivePropTransformFlagActive;
    if (m_networkMode == CoopNetworkMode::Client &&
        (state.localAuthorityUntilTime > NowSeconds() ||
            (state.flags & CoopProtocol::kLivePropTransformFlagCarried) != 0 ||
            (state.flags & CoopProtocol::kLivePropTransformFlagImpulse) != 0))
    {
        packet.flags |= CoopProtocol::kLivePropTransformFlagClientAuthority;
    }
    CopyFixedString(packet.levelName, sizeof(packet.levelName), state.levelName);
    return true;
}

void ModMain::QueueLivePropTransform(IEntity& entity, bool removed, const char* reason)
{
    if (!m_enableLivePropSync ||
        m_livePropApplyActive ||
        m_areaOverlayApplyActive ||
        m_networkMode == CoopNetworkMode::Off ||
        !m_hasRemoteSession ||
        !m_hasRemoteEndpoint ||
        !IsSessionGameplayReady() ||
        !IsKnownSameLevel(m_localLevelName, m_remoteLevelName))
    {
        return;
    }

    LivePropState snapshot;
    std::string guardReason;
    if (!CaptureLivePropState(entity, removed, snapshot, guardReason))
        return;
    AppendLivePropDebugTrace("queue.capture", snapshot.guid, snapshot.entityId, &snapshot, reason ? reason : "entity event");

    const float now = NowSeconds();
    if (snapshot.carried && IsCarrySuppressActiveFor(snapshot.entityId, snapshot.guid, now))
    {
        m_lastLivePropEvent =
            "suppressed stale carry xform guid=" + std::to_string(snapshot.guid) +
            " reason=" + (reason ? reason : "release suppress");
        AppendLivePropDebugTrace("queue.suppress.stale_carry", snapshot.guid, snapshot.entityId, &snapshot, m_lastLivePropEvent.c_str());
        return;
    }

    auto existingIt = m_liveProps.find(snapshot.guid);
    if (snapshot.carried)
    {
        LivePropState previousState;
        const bool hadPreviousState = existingIt != m_liveProps.end();
        if (hadPreviousState)
            previousState = existingIt->second;

        Vec3 carryVelocityEstimate = previousState.releaseVelocity;
        if (hadPreviousState && previousState.carried)
        {
            const float dt = std::clamp(now - previousState.lastQueuedTime, 0.016f, 0.25f);
            Vec3 derivedVelocity = (snapshot.position - previousState.position) / dt;
            if (IsFiniteVec3(derivedVelocity) &&
                derivedVelocity.GetLengthSquared() >= kLivePropReleaseImpulseVelocitySq)
            {
                carryVelocityEstimate = ClampLivePropVectorLength(derivedVelocity, kLivePropDerivedDropMaxSpeed);
            }
        }

        LivePropState& state = m_liveProps[snapshot.guid];
        const bool needsGrabReset =
            !hadPreviousState ||
            !previousState.carried ||
            previousState.pendingRemoteApply ||
            previousState.remoteBallisticActive ||
            previousState.remoteAuthorityUntilTime > now ||
            previousState.releaseMotionUntilTime > now ||
            previousState.velocity.GetLengthSquared() >= kLivePropVelocityActiveSq ||
            previousState.angularVelocity.GetLengthSquared() >= kLivePropAngularVelocityActiveSq;

        state = snapshot;
        state.lastReceivedSequence = hadPreviousState ? previousState.lastReceivedSequence : 0;
        state.lastSentTime = needsGrabReset ? -1000.0f : previousState.lastSentTime;
        state.lastAppliedTime = hadPreviousState ? previousState.lastAppliedTime : -1000.0f;
        state.localAuthorityUntilTime = now + kLivePropLocalCarryAuthorityGraceSeconds;
        state.remoteAuthorityUntilTime = -1000.0f;
        state.forceSendUntilTime = now + kLivePropLocalCarryAuthorityGraceSeconds;
        state.contactAuthorityUntilTime = -1000.0f;
        state.releaseMotionUntilTime = -1000.0f;
        state.releaseVelocity = carryVelocityEstimate;
        state.releaseAngularVelocity = Vec3(ZERO);
        state.velocity = Vec3(ZERO);
        state.angularVelocity = Vec3(ZERO);
        state.pendingRemoteApply = false;
        state.remoteApplyStepsRemaining = 0;
        state.remoteBallisticActive = false;
        state.remoteBallisticUntilTime = -1000.0f;
        state.remoteBallisticJustStarted = false;
        state.remoteLaunchVelocityApplied = false;
        state.remoteBlendStartTime = -1000.0f;
        state.remoteBlendDuration = 0.35f;
        state.flags &= ~(CoopProtocol::kLivePropTransformFlagImpulse | CoopProtocol::kLivePropTransformFlagRemoved);
        state.flags |= CoopProtocol::kLivePropTransformFlagCarried | CoopProtocol::kLivePropTransformFlagActive;
        state.carried = true;
        state.activelyMoving = true;
        state.leasePhase = LivePropLeasePhase::CarriedLocal;
        state.activeUntilTime = now + kLivePropSettleActiveSeconds;
        state.dirty = true;

        if (needsGrabReset)
        {
            std::string zeroReason;
            m_livePropApplyActive = true;
            ApplyPhysicsVelocityAndWake(entity, Vec3(ZERO), Vec3(ZERO), 0.02f, zeroReason);
            m_livePropApplyActive = false;
        }

        m_lastLivePropEvent =
            "queued local grab live prop guid=" + std::to_string(state.guid) +
            " reset=" + std::to_string(needsGrabReset ? 1 : 0) +
            " reason=" + (reason ? reason : "carried prop");
        AppendLivePropDebugTrace("queue.local_grab", state.guid, state.entityId, &state, m_lastLivePropEvent.c_str());
        return;
    }

    const bool existingLocalBurst = existingIt != m_liveProps.end() && existingIt->second.localAuthorityUntilTime > now;
    const bool localFreeAuthority = m_networkMode == CoopNetworkMode::Host || IsClientAreaAuthorityActive();
    const bool localCarryAuthority = snapshot.carried;
    const bool hasLocalAuthority = localCarryAuthority || existingLocalBurst || (localFreeAuthority && !snapshot.carried);
    if (!hasLocalAuthority)
    {
        const bool existingRemoteProtected =
            existingIt != m_liveProps.end() &&
            IsLivePropRemoteProtected(existingIt->second, now);
        const bool canSendClientMotionProposal =
            m_networkMode == CoopNetworkMode::Client &&
            existingIt != m_liveProps.end() &&
            !existingRemoteProtected &&
            !snapshot.carried &&
            !removed &&
            snapshot.activelyMoving &&
            IsLocalPlayerNearPosition(snapshot.position, 16.0f);
        const bool likelyImmediateRemoteEcho =
            existingIt != m_liveProps.end() &&
            existingIt->second.remoteAuthorityUntilTime > now &&
            now - existingIt->second.lastAppliedTime >= 0.0f &&
            now - existingIt->second.lastAppliedTime < 0.035f;
        if (canSendClientMotionProposal && !likelyImmediateRemoteEcho)
        {
            LivePropState& state = m_liveProps[snapshot.guid];
            const uint32_t lastReceivedSequence = state.lastReceivedSequence;
            const float lastSentTime = state.lastSentTime;
            const float lastAppliedTime = state.lastAppliedTime;
            const float remoteAuthorityUntilTime = state.remoteAuthorityUntilTime;
            const float contactAuthorityUntilTime = state.contactAuthorityUntilTime;
            const float releaseMotionUntilTime = state.releaseMotionUntilTime;
            const Vec3 releaseVelocity = state.releaseVelocity;
            const Vec3 releaseAngularVelocity = state.releaseAngularVelocity;
            const bool pendingRemoteApply = state.pendingRemoteApply;
            const uint8_t remoteApplyStepsRemaining = state.remoteApplyStepsRemaining;
            state = snapshot;
            state.lastReceivedSequence = lastReceivedSequence;
            state.lastSentTime = lastSentTime;
            state.lastAppliedTime = lastAppliedTime;
            state.remoteAuthorityUntilTime = remoteAuthorityUntilTime;
            state.contactAuthorityUntilTime = contactAuthorityUntilTime;
            state.releaseMotionUntilTime = releaseMotionUntilTime;
            state.releaseVelocity = releaseVelocity;
            state.releaseAngularVelocity = releaseAngularVelocity;
            state.pendingRemoteApply = pendingRemoteApply;
            state.remoteApplyStepsRemaining = remoteApplyStepsRemaining;
            state.carried = false;
            state.flags &= ~CoopProtocol::kLivePropTransformFlagCarried;
            state.flags |= CoopProtocol::kLivePropTransformFlagActive | CoopProtocol::kLivePropTransformFlagImpulse;
            state.activelyMoving = true;
            state.activeUntilTime = now + kLivePropSettleActiveSeconds;
            state.localAuthorityUntilTime = now + kLivePropClientMotionProposalSeconds;
            state.forceSendUntilTime = now + kLivePropClientMotionProposalSeconds;
            state.remoteBallisticActive = false;
            state.remoteBallisticUntilTime = -1000.0f;
            state.dirty = true;
            m_lastLivePropEvent =
                "queued client motion proposal guid=" + std::to_string(state.guid) +
                " reason=" + (reason ? reason : "entity event");
            AppendLivePropDebugTrace("queue.client_motion", state.guid, state.entityId, &state, m_lastLivePropEvent.c_str());
            return;
        }

        if (existingIt != m_liveProps.end())
            existingIt->second.dirty = false;
        m_lastLivePropEvent =
            "suppressed live prop no local authority guid=" + std::to_string(snapshot.guid) +
            " carried=" + std::to_string(snapshot.carried ? 1 : 0) +
            " reason=" + (reason ? reason : "entity event");
        AppendLivePropDebugTrace("queue.suppress.no_authority", snapshot.guid, snapshot.entityId, &snapshot, m_lastLivePropEvent.c_str());
        return;
    }

    if (existingIt != m_liveProps.end() &&
        existingIt->second.remoteAuthorityUntilTime > now &&
        existingIt->second.localAuthorityUntilTime <= now)
    {
        existingIt->second.dirty = false;
        m_lastLivePropEvent =
            "suppressed live prop echo guid=" + std::to_string(snapshot.guid) +
            " carried=" + std::to_string(snapshot.carried ? 1 : 0) +
            " reason=" + (reason ? reason : "remote authority");
        AppendLivePropDebugTrace("queue.suppress.echo", snapshot.guid, snapshot.entityId, &snapshot, m_lastLivePropEvent.c_str());
        return;
    }

    LivePropState& state = m_liveProps[snapshot.guid];
    const bool carried = snapshot.carried;
    const bool moving = snapshot.activelyMoving;
    const float positionDeltaSq = (snapshot.position - state.position).GetLengthSquared();
    const float positionThresholdSq = carried ? kLivePropCarriedMinPositionDeltaSq : kLivePropMinPositionDeltaSq;
    const bool significant =
        !state.hasSnapshot ||
        carried ||
        moving ||
        removed ||
        positionDeltaSq >= positionThresholdSq ||
        (snapshot.scale - state.scale).GetLengthSquared() >= kLivePropMinScaleDeltaSq ||
        RotationChangedEnough(snapshot.rotation, state.rotation, carried ? 0.0001f : 0.0005f) ||
        snapshot.flags != state.flags;
    if (!significant)
        return;

    const uint32_t lastReceivedSequence = state.lastReceivedSequence;
    const float lastSentTime = state.lastSentTime;
    const float lastAppliedTime = state.lastAppliedTime;
    const float localAuthorityUntilTime = state.localAuthorityUntilTime;
    const float remoteAuthorityUntilTime = state.remoteAuthorityUntilTime;
    const float forceSendUntilTime = state.forceSendUntilTime;
    const float contactAuthorityUntilTime = state.contactAuthorityUntilTime;
    const float releaseMotionUntilTime = state.releaseMotionUntilTime;
    const Vec3 releaseVelocity = state.releaseVelocity;
    const Vec3 releaseAngularVelocity = state.releaseAngularVelocity;
    const bool pendingRemoteApply = state.pendingRemoteApply;
    const uint8_t remoteApplyStepsRemaining = state.remoteApplyStepsRemaining;
    const uint32_t previousFlags = state.flags;
    const Vec3 previousVelocity = state.velocity;
    const Vec3 previousAngularVelocity = state.angularVelocity;
    state = snapshot;
    state.lastReceivedSequence = lastReceivedSequence;
    state.lastSentTime = lastSentTime;
    state.lastAppliedTime = lastAppliedTime;
    state.localAuthorityUntilTime = carried ?
        std::max(localAuthorityUntilTime, NowSeconds() + kLivePropLocalCarryAuthorityGraceSeconds) :
        localAuthorityUntilTime;
    state.remoteAuthorityUntilTime = carried && localCarryAuthority ? -1000.0f : remoteAuthorityUntilTime;
    state.forceSendUntilTime = forceSendUntilTime;
    state.contactAuthorityUntilTime = contactAuthorityUntilTime;
    state.releaseMotionUntilTime = releaseMotionUntilTime;
    state.releaseVelocity = releaseVelocity;
    state.releaseAngularVelocity = releaseAngularVelocity;
    state.pendingRemoteApply = pendingRemoteApply;
    state.remoteApplyStepsRemaining = remoteApplyStepsRemaining;
    state.remoteBallisticActive = false;
    state.remoteBallisticUntilTime = -1000.0f;
    state.leasePhase =
        carried ? LivePropLeasePhase::CarriedLocal :
        moving ? LivePropLeasePhase::FlightLocal :
        LivePropLeasePhase::Settled;
    const bool releaseMotionActive =
        !carried &&
        now <= state.releaseMotionUntilTime &&
        (state.releaseVelocity.GetLengthSquared() >= kLivePropReleaseImpulseVelocitySq ||
            state.releaseAngularVelocity.GetLengthSquared() >= kLivePropReleaseImpulseAngularVelocitySq);
    if (releaseMotionActive)
    {
        state.flags |= CoopProtocol::kLivePropTransformFlagActive;
        if (state.velocity.GetLengthSquared() < kLivePropBallisticVelocitySq &&
            state.releaseVelocity.GetLengthSquared() >= kLivePropBallisticVelocitySq)
        {
            state.velocity = state.releaseVelocity;
        }
        if (state.angularVelocity.GetLengthSquared() < kLivePropBallisticAngularVelocitySq &&
            state.releaseAngularVelocity.GetLengthSquared() >= kLivePropBallisticAngularVelocitySq)
        {
            state.angularVelocity = state.releaseAngularVelocity;
        }
        state.activelyMoving = true;
        state.activeUntilTime = std::max(state.activeUntilTime, state.releaseMotionUntilTime);
    }
    if ((previousFlags & CoopProtocol::kLivePropTransformFlagImpulse) != 0 && now <= state.forceSendUntilTime)
    {
        state.flags |= CoopProtocol::kLivePropTransformFlagActive;
        state.flags |= CoopProtocol::kLivePropTransformFlagImpulse;
        if (state.velocity.GetLengthSquared() < kLivePropBallisticVelocitySq &&
            previousVelocity.GetLengthSquared() >= kLivePropBallisticVelocitySq)
        {
            state.velocity = previousVelocity;
        }
        if (state.angularVelocity.GetLengthSquared() < kLivePropBallisticAngularVelocitySq &&
            previousAngularVelocity.GetLengthSquared() >= kLivePropBallisticAngularVelocitySq)
        {
            state.angularVelocity = previousAngularVelocity;
        }
        state.activelyMoving = true;
    }
    state.dirty = true;
    m_lastLivePropEvent =
        "queued live prop guid=" + std::to_string(state.guid) +
        " level=" + state.levelName +
        " carried=" + std::to_string(state.carried ? 1 : 0) +
        " moving=" + std::to_string(state.activelyMoving ? 1 : 0) +
	        " reason=" + (reason ? reason : "entity event");
    AppendLivePropDebugTrace("queue.dirty", state.guid, state.entityId, &state, m_lastLivePropEvent.c_str());
}

bool ModMain::IsLocalLivePropImpulseSource(
    EntityId sourceEntityId,
    bool allowUnattributedSource,
    std::string* detail)
{
    if (detail)
        detail->clear();

    if (sourceEntityId == INVALID_ENTITYID || sourceEntityId == 0)
    {
        if (detail)
            *detail = allowUnattributedSource ? "native_impulse" : "missing_source";
        return allowUnattributedSource;
    }

    IEntity* localPlayer = ArkPlayer::GetInstancePtr()
        ? ArkPlayer::GetInstance().GetEntity()
        : nullptr;
    if (localPlayer && sourceEntityId == localPlayer->GetId())
    {
        if (detail)
            *detail = "local_player";
        return true;
    }
    if (IsRemoteProxyEntity(sourceEntityId))
    {
        if (detail)
            *detail = "remote_player_proxy";
        return false;
    }

    // Projectiles and equipped weapons retain the native owner entity. Resolve
    // that owner before evaluating an enemy lease or local player source.
    EntityId ownerEntityId = INVALID_ENTITYID;
    CArkProjectile* projectile = nullptr;
    if (TryGuardedCall(
            "live prop impulse projectile resolve",
            [sourceEntityId]() { return CArkProjectile::GetProjectileFromEntityId(sourceEntityId); },
            projectile,
            nullptr) &&
        projectile)
    {
        TryGuardedCall(
            "live prop impulse projectile owner",
            [projectile]() { return static_cast<EntityId>(projectile->m_ownerId); },
            ownerEntityId,
            nullptr);
    }
    if ((ownerEntityId == INVALID_ENTITYID || ownerEntityId == 0) && gEnv && gEnv->pEntitySystem)
    {
        CArkItem* item = nullptr;
        if (TryGuardedCall(
                "live prop impulse item resolve",
                [sourceEntityId]() { return CArkItem::GetItemFromEntityId(sourceEntityId); },
                item,
                nullptr) &&
            item)
        {
            TryGuardedCall(
                "live prop impulse item owner",
                [item]() { return static_cast<EntityId>(item->m_ownerId); },
                ownerEntityId,
                nullptr);
        }
    }
    if (ownerEntityId != INVALID_ENTITYID && ownerEntityId != 0 && ownerEntityId != sourceEntityId)
        return IsLocalLivePropImpulseSource(ownerEntityId, false, detail);

    const auto netIt = m_enemyNetIdsByEntity.find(sourceEntityId);
    if (netIt != m_enemyNetIdsByEntity.end() && gEnv && gEnv->pEntitySystem)
    {
        EnemyAuthorityState* state = FindEnemyAuthorityByNetId(netIt->second);
        IEntity* enemy = gEnv->pEntitySystem->GetEntity(sourceEntityId);
        if (!state || !enemy)
        {
            if (detail)
                *detail = "enemy_state_missing";
            return false;
        }

        const CoopEnemyControlPolicy::Decision decision =
            CoopEnemyControlPolicy::Evaluate(BuildLocalEnemyControlPolicyContext(*state, *enemy));
        if (decision.localVanillaAuthority)
        {
            if (detail)
                *detail = "local_enemy_authority";
            return true;
        }
        if (decision.mode == CoopEnemyControlPolicy::EnemyControlMode::LocalTargetRemoteOwner &&
            decision.preserveLocalCombat &&
            state->localFocusCombatSeconds > 0.0f)
        {
            if (detail)
                *detail = "observer_local_enemy_attack";
            return true;
        }

        if (detail)
            *detail = "remote_enemy_without_local_attack";
        return false;
    }

    if (detail)
        *detail = allowUnattributedSource ? "unattributed_native_impulse" : "unowned_source";
    return allowUnattributedSource;
}

void ModMain::OnNativeLivePropAttackImpulseForHook(
    IEntity* hitEntity,
    IPhysicalEntity* hitPhysics,
    EntityId sourceEntityId,
    bool allowUnattributedSource,
    const char* reason)
{
    if (!m_enableLivePropSync || m_livePropApplyActive || m_areaOverlayApplyActive)
        return;

    std::string sourceDetail;
    if (!IsLocalLivePropImpulseSource(sourceEntityId, allowUnattributedSource, &sourceDetail))
        return;

    std::string guardReason;
    if (!hitEntity)
        hitEntity = ResolveLivePropCollisionPhysicsEntity(hitPhysics, guardReason);
    if (!hitEntity || IsRemoteProxyEntity(hitEntity->GetId()))
        return;

    QueueLivePropCollisionAuthority(
        *hitEntity,
        false,
        false,
        false,
        true,
        false,
        (std::string(reason && reason[0] ? reason : "native attack impulse") +
            " source=" + sourceDetail).c_str());
}

void ModMain::OnNativeLivePropExplosionImpulseForHook(
    EntityId senderEntityId,
    EntityId instigatorEntityId,
    const std::vector<IPhysicalEntity*>& affectedPhysics,
    const char* reason)
{
    if (!m_enableLivePropSync || m_livePropApplyActive || m_areaOverlayApplyActive)
        return;

    EntityId sourceEntityId = instigatorEntityId != INVALID_ENTITYID && instigatorEntityId != 0
        ? instigatorEntityId
        : senderEntityId;
    std::string sourceDetail;
    if (!IsLocalLivePropImpulseSource(sourceEntityId, false, &sourceDetail))
        return;

    std::unordered_set<EntityId> visited;
    for (IPhysicalEntity* physics : affectedPhysics)
    {
        std::string guardReason;
        IEntity* entity = ResolveLivePropCollisionPhysicsEntity(physics, guardReason);
        if (!entity || IsRemoteProxyEntity(entity->GetId()) || !visited.insert(entity->GetId()).second)
            continue;

        QueueLivePropCollisionAuthority(
            *entity,
            false,
            false,
            false,
            true,
            false,
            (std::string(reason && reason[0] ? reason : "native explosion impulse") +
                " source=" + sourceDetail).c_str());
    }
}

bool ModMain::QueueLivePropCollisionAuthority(
    IEntity& entity,
    bool chained,
    bool localPlayerContact,
    bool localCarryContact,
    bool localAttackContact,
    bool localEnemyBodyContact,
    const char* reason)
{
    if (!m_enableLivePropSync ||
        m_livePropApplyActive ||
        m_areaOverlayApplyActive ||
        !m_hasRemoteSession ||
        !m_hasRemoteEndpoint ||
        !IsSessionGameplayReady() ||
        !IsKnownSameLevel(m_localLevelName, m_remoteLevelName))
    {
        return false;
    }

    LivePropState snapshot;
    std::string guardReason;
    if (!CaptureLivePropState(entity, false, snapshot, guardReason, true))
        return false;

    const float now = NowSeconds();
    auto existingIt = m_liveProps.find(snapshot.guid);
    const bool existingLocalBurst =
        existingIt != m_liveProps.end() &&
        existingIt->second.localAuthorityUntilTime > now;
    const bool hadContactAuthority =
        existingIt != m_liveProps.end() &&
        existingIt->second.contactAuthorityUntilTime > now;
    const bool localCarryNow = IsLocalPlayerCarryingEntity(snapshot.entityId);
    if (localCarryNow)
        return false;
    const bool nearLocalPlayer = IsLocalPlayerNearPosition(snapshot.position, kLivePropCollisionPlayerDistanceSq);
    const bool directLocalContact =
        localCarryContact ||
        localPlayerContact ||
        localAttackContact ||
        localEnemyBodyContact;

    if (existingIt != m_liveProps.end())
    {
        const LivePropState& existing = existingIt->second;
        const bool protectedRemoteCarry =
            existing.carried ||
            (existing.flags & CoopProtocol::kLivePropTransformFlagCarried) != 0 ||
            existing.leasePhase == LivePropLeasePhase::CarriedRemote;
        const bool protectedRemoteFlight =
            !existingLocalBurst &&
            IsLivePropRemoteProtected(existing, now);
        if (protectedRemoteFlight &&
            (protectedRemoteCarry || !directLocalContact))
        {
            m_lastLivePropEvent =
                "suppressed collision authority guid=" + std::to_string(snapshot.guid) +
                " reason=remote flight/carried protected chain=" + std::to_string(chained ? 1 : 0);
            AppendLivePropDebugTrace("collision.suppress.remote_flight", snapshot.guid, snapshot.entityId, &existing, m_lastLivePropEvent.c_str());
            return false;
        }
    }

    if (!chained && !existingLocalBurst && !hadContactAuthority && !directLocalContact)
        return false;

    if (hadContactAuthority &&
        now - existingIt->second.lastQueuedTime >= 0.0f &&
        now - existingIt->second.lastQueuedTime < kLivePropCollisionRecentQueueSeconds)
    {
        return true;
    }

    LivePropState& state = m_liveProps[snapshot.guid];
    const uint32_t lastReceivedSequence = state.lastReceivedSequence;
    const float lastSentTime = state.lastSentTime;
    const float lastAppliedTime = state.lastAppliedTime;
    const float localAuthorityUntilTime = state.localAuthorityUntilTime;
    const float remoteAuthorityUntilTime = state.remoteAuthorityUntilTime;
    const float forceSendUntilTime = state.forceSendUntilTime;
    const float contactAuthorityUntilTime = state.contactAuthorityUntilTime;
    const bool pendingRemoteApply = state.pendingRemoteApply;
    const uint8_t remoteApplyStepsRemaining = state.remoteApplyStepsRemaining;
    const Vec3 previousVelocity = state.velocity;
    const Vec3 previousAngularVelocity = state.angularVelocity;

    state = snapshot;
    state.lastReceivedSequence = lastReceivedSequence;
    state.lastSentTime = hadContactAuthority ? lastSentTime : -1000.0f;
    state.lastAppliedTime = lastAppliedTime;
    state.localAuthorityUntilTime = std::max(
        localAuthorityUntilTime,
        now + (chained ? kLivePropClientCollisionChainAuthoritySeconds : kLivePropClientCollisionAuthoritySeconds));
    state.remoteAuthorityUntilTime = remoteAuthorityUntilTime > now && !directLocalContact ? remoteAuthorityUntilTime : -1000.0f;
    state.forceSendUntilTime = std::max(
        forceSendUntilTime,
        now + (chained ? kLivePropClientCollisionChainAuthoritySeconds : kLivePropClientCollisionAuthoritySeconds));
    state.contactAuthorityUntilTime = std::max(
        contactAuthorityUntilTime,
        now + (chained ? kLivePropClientCollisionChainAuthoritySeconds : kLivePropClientCollisionAuthoritySeconds));
    state.pendingRemoteApply = pendingRemoteApply;
    state.remoteApplyStepsRemaining = remoteApplyStepsRemaining;
    state.carried = false;
    state.flags &= ~CoopProtocol::kLivePropTransformFlagCarried;
    state.flags |= CoopProtocol::kLivePropTransformFlagActive;
    if (state.velocity.GetLengthSquared() < kLivePropVelocityActiveSq &&
        previousVelocity.GetLengthSquared() >= kLivePropVelocityActiveSq)
    {
        state.velocity = previousVelocity;
    }
    if (state.angularVelocity.GetLengthSquared() < kLivePropAngularVelocityActiveSq &&
        previousAngularVelocity.GetLengthSquared() >= kLivePropAngularVelocityActiveSq)
    {
        state.angularVelocity = previousAngularVelocity;
    }
    if (state.velocity.GetLengthSquared() >= kLivePropVelocityActiveSq ||
        state.angularVelocity.GetLengthSquared() >= kLivePropAngularVelocityActiveSq)
    {
        state.flags |= CoopProtocol::kLivePropTransformFlagImpulse;
    }
    state.activelyMoving = true;
    state.activeUntilTime = now + kLivePropSettleActiveSeconds;
    state.remoteBallisticActive = false;
    state.remoteBallisticUntilTime = -1000.0f;
    state.leasePhase = LivePropLeasePhase::FlightLocal;
    state.lastQueuedTime = now;
    state.dirty = true;

    if (chained)
        ++m_livePropCollisionChainGrants;
    else
        ++m_livePropCollisionAuthorityGrants;
    if (!chained && localAttackContact)
        ++m_livePropAttackCollisionGrants;
    if (!chained && localEnemyBodyContact)
        ++m_livePropEnemyBodyCollisionGrants;

    m_lastLivePropEvent =
        "queued collision authority guid=" + std::to_string(state.guid) +
        " chain=" + std::to_string(chained ? 1 : 0) +
        " playerContact=" + std::to_string(localPlayerContact ? 1 : 0) +
        " carryContact=" + std::to_string(localCarryContact ? 1 : 0) +
        " attackContact=" + std::to_string(localAttackContact ? 1 : 0) +
        " enemyBodyContact=" + std::to_string(localEnemyBodyContact ? 1 : 0) +
        " nearPlayer=" + std::to_string(nearLocalPlayer ? 1 : 0) +
        " existingLocal=" + std::to_string(existingLocalBurst ? 1 : 0) +
        " reason=" + (reason ? reason : "collision");
    AppendLivePropDebugTrace(
        chained ? "collision.grant.chain" : "collision.grant.direct",
        state.guid,
        state.entityId,
        &state,
        m_lastLivePropEvent.c_str());
    return true;
}

bool ModMain::QueueLivePropXformContactAuthority(IEntity& entity, const char* reason)
{
    if (!m_enableLivePropSync ||
        m_livePropApplyActive ||
        m_areaOverlayApplyActive ||
        !m_hasRemoteSession ||
        !m_hasRemoteEndpoint ||
        !IsSessionGameplayReady() ||
        !IsKnownSameLevel(m_localLevelName, m_remoteLevelName) ||
        !gEnv ||
        !gEnv->pEntitySystem)
    {
        return false;
    }

    const EntityId carriedEntityId = GetLocalCarriedEntityId();
    if (carriedEntityId == INVALID_ENTITYID)
        return false;

    IEntity* carriedEntity = nullptr;
    std::string guardReason;
    if (!TryGuardedCall("live prop xform discovery carried GetEntity", [carriedEntityId]() { return gEnv->pEntitySystem->GetEntity(carriedEntityId); }, carriedEntity, &guardReason) ||
        !carriedEntity)
    {
        return false;
    }

    Vec3 carriedPosition(ZERO);
    if (!TryGuardedCall("live prop xform discovery carried position", [carriedEntity]() { return carriedEntity->GetWorldPos(); }, carriedPosition, &guardReason) ||
        !IsFiniteVec3(carriedPosition))
    {
        return false;
    }

    LivePropState snapshot;
    if (!CaptureLivePropState(entity, false, snapshot, guardReason, true))
        return false;

    if (snapshot.entityId == carriedEntityId || IsLocalPlayerCarryingEntity(snapshot.entityId))
        return false;

    if ((snapshot.position - carriedPosition).GetLengthSquared() > kLivePropXformCarryDiscoveryDistanceSq)
        return false;

    return QueueLivePropCollisionAuthority(
        entity,
        false,
        false,
        true,
        false,
        false,
        reason ? reason : "xform carried contact");
}

bool ModMain::QueueLivePropXformLocalPlayerAuthority(IEntity& entity, const char* reason)
{
    if (!m_enableLivePropSync ||
        m_livePropApplyActive ||
        m_areaOverlayApplyActive ||
        !m_hasRemoteSession ||
        !m_hasRemoteEndpoint ||
        !IsSessionGameplayReady() ||
        !IsKnownSameLevel(m_localLevelName, m_remoteLevelName))
    {
        return false;
    }

    LivePropState snapshot;
    std::string guardReason;
    if (!CaptureLivePropState(entity, false, snapshot, guardReason, true))
        return false;

    if (snapshot.carried || !snapshot.activelyMoving)
        return false;

    if (!IsLocalPlayerNearPosition(snapshot.position, kLivePropCollisionPlayerDistanceSq))
        return false;

    const float now = NowSeconds();
    const auto proxySuppressIt = m_livePropRemoteProxyContactSuppressUntil.find(snapshot.guid);
    if (proxySuppressIt != m_livePropRemoteProxyContactSuppressUntil.end() &&
        proxySuppressIt->second > now)
    {
        AppendLivePropDebugTrace(
            "xform.local_player_contact.suppress.proxy",
            snapshot.guid,
            snapshot.entityId,
            nullptr,
            reason ? reason : "xform local player contact");
        return false;
    }
    auto existingIt = m_liveProps.find(snapshot.guid);
    if (existingIt != m_liveProps.end() &&
        (existingIt->second.remoteAuthorityUntilTime > now ||
            IsLivePropRemoteProtected(existingIt->second, now)))
    {
        AppendLivePropDebugTrace(
            "xform.local_player_contact.suppress.remote",
            snapshot.guid,
            snapshot.entityId,
            &existingIt->second,
            reason ? reason : "xform local player contact");
        return false;
    }

    return QueueLivePropCollisionAuthority(
        entity,
        false,
        true,
        false,
        false,
        false,
        reason ? reason : "xform local player contact");
}

void ModMain::HandleLivePropCollisionEvent(IEntity& entity, const SEntityEvent& event)
{
    ++m_livePropCollisionEvents;

    IEntity* candidates[3] = { &entity, nullptr, nullptr };
    bool candidatePlayerContact[3] = {};
    bool candidateCarryContact[3] = {};
    bool candidateAttackContact[3] = {};
    bool candidateEnemyBodyContact[3] = {};
    bool candidateRemoteProxyContact[3] = {};
    bool directGranted[3] = {};
    size_t candidateCount = 1;
    constexpr size_t kInvalidCandidateIndex = static_cast<size_t>(-1);
    auto addCandidate = [&](IEntity* candidate)
    {
        if (!candidate)
            return kInvalidCandidateIndex;
        const EntityId candidateId = candidate->GetId();
        if (candidateId != INVALID_ENTITYID)
        {
            const auto netIt = m_enemyNetIdsByEntity.find(candidateId);
            if (netIt != m_enemyNetIdsByEntity.end())
            {
                const auto authorityIt = m_enemyAuthorities.find(netIt->second);
                if (authorityIt != m_enemyAuthorities.end())
                {
                    const CoopEnemyControlPolicy::Decision decision =
                        CoopEnemyControlPolicy::Evaluate(BuildLocalEnemyControlPolicyContext(authorityIt->second, *candidate));
                    if (decision.blockWorldCollision)
                        return kInvalidCandidateIndex;
                }
            }
        }
        for (size_t i = 0; i < candidateCount; ++i)
        {
            if (candidates[i] == candidate)
                return i;
        }
        if (candidateCount < 3)
        {
            const size_t index = candidateCount;
            candidates[candidateCount++] = candidate;
            return index;
        }
        return kInvalidCandidateIndex;
    };
    auto markCandidateContact = [&](IEntity* candidate,
                                    bool playerContact,
                                    bool carryContact,
                                    bool attackContact,
                                    bool enemyBodyContact,
                                    bool remoteProxyContact)
    {
        const size_t index = addCandidate(candidate);
        if (index == kInvalidCandidateIndex)
            return;
        candidatePlayerContact[index] = candidatePlayerContact[index] || playerContact;
        candidateCarryContact[index] = candidateCarryContact[index] || carryContact;
        candidateAttackContact[index] = candidateAttackContact[index] || attackContact;
        candidateEnemyBodyContact[index] = candidateEnemyBodyContact[index] || enemyBodyContact;
        candidateRemoteProxyContact[index] = candidateRemoteProxyContact[index] || remoteProxyContact;
    };

    std::string reason;
    EventPhysCollision collision = {};
    EntityId localPlayerEntityId = INVALID_ENTITYID;
    IEntity* localPlayerEntity = nullptr;
    IPhysicalEntity* localPlayerPhysics = nullptr;
    EntityId localCarriedEntityId = GetLocalCarriedEntityId();
    IEntity* localCarriedEntity = nullptr;
    IPhysicalEntity* localCarriedPhysics = nullptr;
    if (localCarriedEntityId != INVALID_ENTITYID && gEnv && gEnv->pEntitySystem)
    {
        if (TryGuardedCall("live prop collision carried GetEntity", [localCarriedEntityId]() { return gEnv->pEntitySystem->GetEntity(localCarriedEntityId); }, localCarriedEntity, &reason) &&
            localCarriedEntity)
        {
            TryGuardedCall("live prop collision carried GetPhysics", [localCarriedEntity]() { return localCarriedEntity->GetPhysics(); }, localCarriedPhysics, &reason);
        }
    }
    if (ArkPlayer::GetInstancePtr())
    {
        if (TryGuardedCall("live prop collision local player entity", []() { return ArkPlayer::GetInstance().GetEntity(); }, localPlayerEntity, &reason) &&
            localPlayerEntity)
        {
            TryGuardedCall("live prop collision local player id", [localPlayerEntity]() { return localPlayerEntity->GetId(); }, localPlayerEntityId, &reason);
            TryGuardedCall("live prop collision local player GetPhysics", [localPlayerEntity]() { return localPlayerEntity->GetPhysics(); }, localPlayerPhysics, &reason);
        }
    }

    auto isEntity = [](IEntity* lhs, IEntity* rhs)
    {
        return lhs && rhs && lhs == rhs;
    };
    auto isEntityId = [&](IEntity* candidate, EntityId entityId)
    {
        if (!candidate || entityId == INVALID_ENTITYID)
            return false;
        EntityId candidateId = INVALID_ENTITYID;
        return TryGuardedCall("live prop collision candidate id", [candidate]() { return candidate->GetId(); }, candidateId, &reason) &&
            candidateId == entityId;
    };

    if (TryReadLivePropCollisionPayload(event, collision, reason))
    {
        IPhysicalEntity* sourcePhysics = nullptr;
        TryGuardedCall("live prop collision source GetPhysics", [&entity]() { return entity.GetPhysics(); }, sourcePhysics, &reason);

        IPhysicalEntity* physicsEntries[2] = { collision.pEntity[0], collision.pEntity[1] };
        IEntity* collisionEntities[2] = {};
        for (size_t side = 0; side < 2; ++side)
        {
            IPhysicalEntity* physics = physicsEntries[side];
            if (!physics)
                continue;
            if (sourcePhysics && physics == sourcePhysics)
                collisionEntities[side] = &entity;
            else
                collisionEntities[side] = ResolveLivePropCollisionPhysicsEntity(physics, reason);
            addCandidate(collisionEntities[side]);
        }

        auto physicsSideMatches = [&](size_t side, IEntity* candidate)
        {
            if (!candidate)
                return false;
            if (isEntity(collisionEntities[side], candidate))
                return true;
            IPhysicalEntity* candidatePhysics = nullptr;
            return TryGuardedCall("live prop collision candidate GetPhysics", [candidate]() { return candidate->GetPhysics(); }, candidatePhysics, &reason) &&
                candidatePhysics &&
                physicsEntries[side] == candidatePhysics;
        };
        auto sideIsLocalPlayer = [&](size_t side)
        {
            return (localPlayerPhysics && physicsEntries[side] == localPlayerPhysics) ||
                isEntity(collisionEntities[side], localPlayerEntity) ||
                isEntityId(collisionEntities[side], localPlayerEntityId);
        };
        auto sideIsLocalCarry = [&](size_t side)
        {
            return (localCarriedPhysics && physicsEntries[side] == localCarriedPhysics) ||
                isEntity(collisionEntities[side], localCarriedEntity) ||
                isEntityId(collisionEntities[side], localCarriedEntityId);
        };
        const Vec3 relativeContactVelocity = collision.vloc[0] - collision.vloc[1];
        const bool meaningfulBodyContact =
            (IsFiniteVec3(relativeContactVelocity) &&
                relativeContactVelocity.GetLengthSquared() >= kLivePropVelocityActiveSq) ||
            (std::isfinite(collision.normImpulse) && std::fabs(collision.normImpulse) >= 0.01f);
        auto classifyGameplaySource = [&](size_t side, bool& attackContact, bool& enemyBodyContact, bool& remoteProxyContact)
        {
            attackContact = false;
            enemyBodyContact = false;
            remoteProxyContact = false;
            IEntity* source = collisionEntities[side];
            if (!source)
                return;

            EntityId sourceEntityId = INVALID_ENTITYID;
            if (!TryGuardedCall(
                    "live prop collision source id",
                    [source]() { return source->GetId(); },
                    sourceEntityId,
                    &reason) ||
                sourceEntityId == INVALID_ENTITYID)
            {
                return;
            }

            if (IsRemoteProxyEntity(sourceEntityId))
            {
                remoteProxyContact = true;
                return;
            }

            CArkProjectile* projectile = nullptr;
            if (TryGuardedCall(
                    "live prop collision projectile resolve",
                    [sourceEntityId]() { return CArkProjectile::GetProjectileFromEntityId(sourceEntityId); },
                    projectile,
                    nullptr) &&
                projectile)
            {
                EntityId ownerEntityId = INVALID_ENTITYID;
                if (TryGuardedCall(
                        "live prop collision projectile owner",
                        [projectile]() { return static_cast<EntityId>(projectile->m_ownerId); },
                        ownerEntityId,
                        nullptr))
                {
                    std::string sourceDetail;
                    attackContact = IsLocalLivePropImpulseSource(ownerEntityId, false, &sourceDetail);
                    remoteProxyContact = IsRemoteProxyEntity(ownerEntityId);
                }
                return;
            }

            const auto netIt = m_enemyNetIdsByEntity.find(sourceEntityId);
            if (netIt == m_enemyNetIdsByEntity.end())
                return;
            EnemyAuthorityState* state = FindEnemyAuthorityByNetId(netIt->second);
            if (!state)
                return;

            const CoopEnemyControlPolicy::Decision decision =
                CoopEnemyControlPolicy::Evaluate(BuildLocalEnemyControlPolicyContext(*state, *source));
            // Resting contacts can be reported every physics tick. Only the
            // current enemy authority may claim a prop, and only when that
            // contact actually carries motion or impulse.
            enemyBodyContact = decision.localVanillaAuthority && meaningfulBodyContact;
            attackContact =
                !enemyBodyContact &&
                decision.mode == CoopEnemyControlPolicy::EnemyControlMode::LocalTargetRemoteOwner &&
                decision.preserveLocalCombat &&
                state->localFocusCombatSeconds > 0.0f;
        };

        for (size_t index = 0; index < candidateCount; ++index)
        {
            IEntity* candidate = candidates[index];
            for (size_t side = 0; side < 2; ++side)
            {
                if (!physicsSideMatches(side, candidate))
                    continue;
                const size_t otherSide = side == 0 ? 1 : 0;
                bool attackContact = false;
                bool enemyBodyContact = false;
                bool remoteProxyContact = false;
                classifyGameplaySource(
                    otherSide,
                    attackContact,
                    enemyBodyContact,
                    remoteProxyContact);
                markCandidateContact(
                    candidate,
                    sideIsLocalPlayer(otherSide),
                    sideIsLocalCarry(otherSide),
                    attackContact,
                    enemyBodyContact,
                    remoteProxyContact);
            }
        }
    }

    const float now = NowSeconds();
    for (size_t i = 0; i < candidateCount; ++i)
    {
        if (!candidateRemoteProxyContact[i] ||
            candidatePlayerContact[i] ||
            candidateCarryContact[i] ||
            candidateAttackContact[i] ||
            candidateEnemyBodyContact[i])
        {
            continue;
        }

        LivePropState snapshot;
        std::string suppressReason;
        if (!CaptureLivePropState(*candidates[i], false, snapshot, suppressReason, true))
            continue;
        m_livePropRemoteProxyContactSuppressUntil[snapshot.guid] = now + 0.30f;
        ++m_livePropRemoteProxyCollisionSuppressions;

        const auto existingIt = m_liveProps.find(snapshot.guid);
        AppendLivePropDebugTrace(
            "collision.suppress.remote_proxy",
            snapshot.guid,
            snapshot.entityId,
            existingIt != m_liveProps.end() ? &existingIt->second : nullptr,
            "remote proxy cannot own prop collision");
    }

    bool anyDirectGrant = false;
    for (size_t i = 0; i < candidateCount; ++i)
    {
        directGranted[i] = QueueLivePropCollisionAuthority(
            *candidates[i],
            false,
            candidatePlayerContact[i],
            candidateCarryContact[i],
            candidateAttackContact[i],
            candidateEnemyBodyContact[i],
            "collision contact");
        anyDirectGrant = anyDirectGrant || directGranted[i];
    }

    if (!anyDirectGrant)
        return;

    for (size_t i = 0; i < candidateCount; ++i)
    {
        if (!directGranted[i])
            QueueLivePropCollisionAuthority(
                *candidates[i],
                true,
                candidatePlayerContact[i],
                candidateCarryContact[i],
                candidateAttackContact[i],
                candidateEnemyBodyContact[i],
                "collision chain");
    }
}

void ModMain::OnArkPlayerCarryThrowHook(ArkPlayerCarry* carry, const char* phase, bool result)
{
    const std::string phaseText = phase && phase[0] ? phase : "unknown";
    if (phaseText == "before")
        ++m_livePropThrowCalls;
    else if (phaseText == "after" && result)
        ++m_livePropThrowSuccesses;

    EntityId entityId = INVALID_ENTITYID;
    bool throwFlag = false;
    bool justThrown = false;
    std::string reason;
    if (carry)
    {
        TryGuardedCall("carry throw trace picked id", [carry]() { return carry->m_pickedUpEntityId; }, entityId, &reason);
        TryGuardedCall("carry throw trace throw flag", [carry]() { return carry->m_bThrowCarriedEntity; }, throwFlag, &reason);
        TryGuardedCall("carry throw trace just thrown", [carry]() { return carry->m_bJustThrown; }, justThrown, &reason);
    }

    if (phaseText == "before" && entityId != INVALID_ENTITYID)
    {
        g_livePropPendingThrowEntityId = entityId;
        g_livePropPendingThrowUntilTime = NowSeconds() + kLivePropPendingThrowSeconds;
    }

    m_lastLivePropCarryEvent =
        "throw phase=" + phaseText +
        " result=" + std::to_string(result ? 1 : 0) +
        " throwFlag=" + std::to_string(throwFlag ? 1 : 0) +
        " justThrown=" + std::to_string(justThrown ? 1 : 0) +
        DescribeCarryEntityForTrace(entityId);
    AppendLivePropDebugTrace(
        ("carry.throw." + phaseText).c_str(),
        0,
        entityId,
        nullptr,
        m_lastLivePropCarryEvent.c_str());
}

void ModMain::OnArkPlayerCarryStopHook(EntityId entityId, float impulse, bool applyAngularImpulse, bool thrown, bool fromSerialize)
{
    ++m_livePropStopCarryCalls;
    if (fromSerialize)
        ++m_livePropStopCarrySerializeSkips;

    const float now = NowSeconds();
    const bool pendingThrowFallback =
        !thrown &&
        entityId != INVALID_ENTITYID &&
        entityId == g_livePropPendingThrowEntityId &&
        now <= g_livePropPendingThrowUntilTime;
    const bool effectiveThrown = thrown || pendingThrowFallback;
    const float effectiveImpulse = pendingThrowFallback && impulse <= 0.0f ? 18.0f : impulse;
    const bool effectiveApplyAngularImpulse = applyAngularImpulse || pendingThrowFallback;
    if (effectiveThrown)
        ++m_livePropStopCarryThrown;
    if (entityId == g_livePropPendingThrowEntityId)
    {
        g_livePropPendingThrowEntityId = INVALID_ENTITYID;
        g_livePropPendingThrowUntilTime = -1000.0f;
    }

    m_lastLivePropCarryEvent =
        std::string("stop carry pre entity=") + std::to_string(entityId) +
        " impulse=" + std::to_string(impulse) +
        " angular=" + std::to_string(applyAngularImpulse ? 1 : 0) +
        " thrown=" + std::to_string(thrown ? 1 : 0) +
        " effectiveThrown=" + std::to_string(effectiveThrown ? 1 : 0) +
        " pendingThrow=" + std::to_string(pendingThrowFallback ? 1 : 0) +
        " serialize=" + std::to_string(fromSerialize ? 1 : 0) +
        DescribeCarryEntityForTrace(entityId);

    if (fromSerialize ||
        !m_enableLivePropSync ||
        m_livePropApplyActive ||
        m_areaOverlayApplyActive ||
        m_networkMode == CoopNetworkMode::Off ||
        !m_hasRemoteSession ||
        !m_hasRemoteEndpoint ||
        !IsSessionGameplayReady() ||
        !IsKnownSameLevel(m_localLevelName, m_remoteLevelName) ||
        entityId == INVALID_ENTITYID ||
        !gEnv ||
        !gEnv->pEntitySystem)
    {
        return;
    }

    IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId);
    if (!entity)
        return;

    std::string reason;
    LivePropState snapshot;
    if (!CaptureLivePropState(*entity, false, snapshot, reason, true))
    {
        m_lastLivePropEvent =
            std::string("carry stop ignored entity=") + std::to_string(entityId) +
            " reason=" + reason;
        m_lastLivePropCarryEvent = m_lastLivePropEvent + DescribeCarryEntityForTrace(entityId);
        return;
    }

    LivePropState& state = m_liveProps[snapshot.guid];
    if (state.remoteAuthorityUntilTime > now && state.localAuthorityUntilTime <= now && !effectiveThrown)
    {
        m_lastLivePropEvent = "carry stop ignored guid=" + std::to_string(snapshot.guid) + " reason=remote authority";
        m_lastLivePropCarryEvent = m_lastLivePropEvent + DescribeCarryEntityForTrace(entityId);
        return;
    }
    if (effectiveThrown && state.remoteAuthorityUntilTime > now)
        m_lastLivePropCarryEvent = "throw overrides remote authority guid=" + std::to_string(snapshot.guid);

    const LivePropState previousState = state;
    const uint32_t lastReceivedSequence = previousState.lastReceivedSequence;
    const float lastAppliedTime = previousState.lastAppliedTime;
    state = snapshot;
    state.lastReceivedSequence = lastReceivedSequence;
    state.lastAppliedTime = lastAppliedTime;
    state.flags &= ~CoopProtocol::kLivePropTransformFlagCarried;
    state.flags |= CoopProtocol::kLivePropTransformFlagActive;
    state.carried = false;
    state.activelyMoving = true;
    state.remoteBallisticActive = false;
    state.remoteBallisticUntilTime = -1000.0f;
    state.remoteBallisticJustStarted = false;
    state.remoteLaunchVelocityApplied = false;
    state.leasePhase = LivePropLeasePhase::FlightLocal;

    bool synthesizedThrowVelocity = false;
    if (effectiveThrown && ArkPlayer::GetInstancePtr())
    {
        Quat viewRotation(IDENTITY);
        std::string viewReason;
        if (TryGuardedCall("live prop throw synth view", []() -> Quat { return ArkPlayer::GetInstance().GetViewRotation(); }, viewRotation, &viewReason))
        {
            Vec3 forward = viewRotation.GetColumn1();
            forward.NormalizeSafe(FORWARD_DIRECTION);
            const float throwSpeed = std::clamp(effectiveImpulse > 0.0f ? effectiveImpulse : 12.0f, 8.0f, 28.0f);
            const float nativeSpeedSq = state.velocity.GetLengthSquared();
            const float nativeForwardSpeed = state.velocity.Dot(forward);
            const bool weakNativeThrow =
                nativeSpeedSq < 36.0f ||
                nativeForwardSpeed < throwSpeed * 0.45f;
            if (weakNativeThrow)
            {
                state.velocity = forward * throwSpeed;
                if (state.velocity.z > -1.0f)
                    state.velocity.z += 0.85f;
                synthesizedThrowVelocity = true;
            }
            if ((synthesizedThrowVelocity || effectiveApplyAngularImpulse) &&
                state.angularVelocity.GetLengthSquared() < kLivePropBallisticAngularVelocitySq)
            {
                state.angularVelocity = Vec3(0.0f, 0.0f, throwSpeed * 0.35f);
            }
        }
    }

    bool derivedReleaseVelocity = false;
    if (!effectiveThrown &&
        previousState.hasSnapshot &&
        previousState.carried &&
        state.velocity.GetLengthSquared() < kLivePropReleaseImpulseVelocitySq)
    {
        Vec3 derivedVelocity = previousState.releaseVelocity;
        if (derivedVelocity.GetLengthSquared() < kLivePropReleaseImpulseVelocitySq)
        {
            const float dt = std::clamp(now - previousState.lastQueuedTime, 0.016f, 0.25f);
            derivedVelocity = (state.position - previousState.position) / dt;
        }
        if (IsFiniteVec3(derivedVelocity) && derivedVelocity.GetLengthSquared() >= kLivePropReleaseImpulseVelocitySq)
        {
            state.velocity = ClampLivePropVectorLength(derivedVelocity, kLivePropDerivedDropMaxSpeed);
            derivedReleaseVelocity = true;
        }
    }

    const bool releaseHasMotion =
        state.velocity.GetLengthSquared() >= kLivePropReleaseImpulseVelocitySq ||
        state.angularVelocity.GetLengthSquared() >= kLivePropReleaseImpulseAngularVelocitySq;
    const bool releaseImpulse = effectiveThrown || releaseHasMotion;
    if (releaseImpulse)
        state.flags |= CoopProtocol::kLivePropTransformFlagImpulse;

    const bool clientOrigin = m_networkMode == CoopNetworkMode::Client;
    const float authoritySeconds = clientOrigin ?
        (releaseHasMotion ? kLivePropReleaseFlightAuthoritySeconds :
            effectiveThrown ? kLivePropClientThrowAuthoritySeconds : kLivePropClientDropAuthoritySeconds) :
        (effectiveThrown ? kLivePropReleaseFlightAuthoritySeconds : 0.45f);
    const float forceSendSeconds =
        clientOrigin && releaseHasMotion ?
        std::min(authoritySeconds, kLivePropClientReleaseForceSendSeconds) :
        authoritySeconds;
    state.activeUntilTime = now + authoritySeconds;
    state.localAuthorityUntilTime = now + authoritySeconds;
    state.remoteAuthorityUntilTime = -1000.0f;
    state.forceSendUntilTime = now + forceSendSeconds;
    if (releaseHasMotion)
    {
        state.releaseVelocity = state.velocity;
        state.releaseAngularVelocity = state.angularVelocity;
        state.releaseMotionUntilTime = now + std::min(authoritySeconds, kLivePropReleaseMotionLatchSeconds);
    }
    else
    {
        state.releaseVelocity = Vec3(ZERO);
        state.releaseAngularVelocity = Vec3(ZERO);
        state.releaseMotionUntilTime = -1000.0f;
    }
    state.pendingRemoteApply = false;
    state.remoteApplyStepsRemaining = 0;
    state.dirty = true;
    state.lastSentTime = -1000.0f;
    g_livePropCarrySuppressEntityId = entityId;
    g_livePropCarrySuppressGuid = state.guid;
    g_livePropCarrySuppressUntilTime = now + authoritySeconds;
    g_livePropLastCarriedEntityId = INVALID_ENTITYID;
    g_livePropLastCarriedGuid = 0;

    m_lastLivePropEvent =
        std::string("carry stop live prop guid=") + std::to_string(state.guid) +
        " thrown=" + std::to_string(effectiveThrown ? 1 : 0) +
        " nativeThrown=" + std::to_string(thrown ? 1 : 0) +
        " pendingThrow=" + std::to_string(pendingThrowFallback ? 1 : 0) +
        " impulse=" + std::to_string(effectiveImpulse) +
        " releaseImpulse=" + std::to_string(releaseImpulse ? 1 : 0) +
        " releaseMotion=" + std::to_string(releaseHasMotion ? 1 : 0) +
        " angular=" + std::to_string(effectiveApplyAngularImpulse ? 1 : 0) +
        " clientOrigin=" + std::to_string(clientOrigin ? 1 : 0) +
        " authority=" + std::to_string(authoritySeconds) +
        " forceSend=" + std::to_string(forceSendSeconds);
    m_lastLivePropCarryEvent =
        m_lastLivePropEvent +
        " flags=" + std::to_string(state.flags) +
        " synthVel=" + std::to_string(synthesizedThrowVelocity ? 1 : 0) +
        " derivedVel=" + std::to_string(derivedReleaseVelocity ? 1 : 0) +
        " vel=" + FormatVec3Compact(state.velocity) +
        " angVel=" + FormatVec3Compact(state.angularVelocity);
    AppendLivePropDebugTrace("carry.stop", state.guid, entityId, &state, m_lastLivePropCarryEvent.c_str());
}

void ModMain::TickLivePropSync(float frameTime)
{
    if (!m_enableLivePropSync ||
        m_networkMode == CoopNetworkMode::Off ||
        m_socket == kInvalidNetworkSocket ||
        !m_hasRemoteSession ||
        !m_hasRemoteEndpoint ||
        !IsSessionGameplayReady() ||
        !IsKnownSameLevel(m_localLevelName, m_remoteLevelName))
    {
        return;
    }

    const float now = NowSeconds();
    for (auto it = m_livePropRemoteProxyContactSuppressUntil.begin();
         it != m_livePropRemoteProxyContactSuppressUntil.end();)
    {
        if (it->second <= now)
            it = m_livePropRemoteProxyContactSuppressUntil.erase(it);
        else
            ++it;
    }
    if (m_networkMode == CoopNetworkMode::Host &&
        !m_areaOverlayLivePropCandidates.empty() &&
        now <= m_areaOverlayLivePropCandidateUntilTime)
    {
        for (auto it = m_areaOverlayLivePropCandidates.begin(); it != m_areaOverlayLivePropCandidates.end();)
        {
            std::string reason;
            IEntity* entity = ResolveLivePropEntity(*it, INVALID_ENTITYID, reason);
            LivePropState snapshot;
            if (entity && CaptureLivePropState(*entity, false, snapshot, reason) && snapshot.activelyMoving)
            {
                PromoteAreaOverlayLiveProp(std::move(snapshot), m_areaOverlayLivePropCandidateUntilTime);
                it = m_areaOverlayLivePropCandidates.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    else if (!m_areaOverlayLivePropCandidates.empty())
    {
        m_areaOverlayLivePropCandidates.clear();
        m_areaOverlayLivePropCandidateUntilTime = -1000.0f;
    }

    uint64_t carriedGuid = 0;
    EntityId carriedEntityId = GetLocalCarriedEntityId();
    if (g_livePropCarrySuppressEntityId != INVALID_ENTITYID)
    {
        if (now > g_livePropCarrySuppressUntilTime ||
            (carriedEntityId != INVALID_ENTITYID &&
                !IsCarrySuppressActiveFor(carriedEntityId, g_livePropCarrySuppressGuid, now)))
        {
            g_livePropCarrySuppressEntityId = INVALID_ENTITYID;
            g_livePropCarrySuppressGuid = 0;
            g_livePropCarrySuppressUntilTime = -1000.0f;
        }
        else if (carriedEntityId == g_livePropCarrySuppressEntityId)
        {
            carriedEntityId = INVALID_ENTITYID;
        }
        else if (carriedEntityId == INVALID_ENTITYID)
        {
            g_livePropCarrySuppressEntityId = INVALID_ENTITYID;
            g_livePropCarrySuppressGuid = 0;
            g_livePropCarrySuppressUntilTime = -1000.0f;
        }
    }

    auto queueReleasedCarriedProp = [&](EntityId releasedEntityId, uint64_t releasedGuid, const char* label)
    {
        if (releasedEntityId == INVALID_ENTITYID && releasedGuid == 0)
            return;

        std::string reason;
        IEntity* releasedEntity = nullptr;
        if (releasedGuid != 0)
            releasedEntity = ResolveLivePropEntity(releasedGuid, releasedEntityId, reason);
        else if (gEnv && gEnv->pEntitySystem)
            releasedEntity = gEnv->pEntitySystem->GetEntity(releasedEntityId);
        if (!releasedEntity)
            return;

        uint64_t guid = releasedGuid;
        if (guid == 0)
            TryGuardedCall("live prop release IEntity::GetGuid", [releasedEntity]() { return releasedEntity->GetGuid(); }, guid, &reason);
        if (guid == 0)
            return;

        LivePropState snapshot;
        if (!CaptureLivePropState(*releasedEntity, false, snapshot, reason, true))
            QueueLivePropTransform(*releasedEntity, false, label ? label : "released carried prop");

        auto stateIt = m_liveProps.find(guid);
        if (stateIt == m_liveProps.end() && snapshot.hasSnapshot)
            stateIt = m_liveProps.emplace(guid, snapshot).first;
        if (stateIt == m_liveProps.end())
            return;

        LivePropState& state = stateIt->second;
        const LivePropState previousState = state;
        const uint32_t lastReceivedSequence = state.lastReceivedSequence;
        const float lastSentTime = state.lastSentTime;
        if (snapshot.hasSnapshot)
        {
            state = snapshot;
            state.lastReceivedSequence = lastReceivedSequence;
            state.lastSentTime = lastSentTime;
        }
        state.flags &= ~CoopProtocol::kLivePropTransformFlagCarried;
        state.flags |= CoopProtocol::kLivePropTransformFlagActive;
        bool derivedReleaseVelocity = false;
        if (previousState.hasSnapshot &&
            previousState.carried &&
            state.velocity.GetLengthSquared() < kLivePropReleaseImpulseVelocitySq)
        {
            Vec3 derivedVelocity = previousState.releaseVelocity;
            if (derivedVelocity.GetLengthSquared() < kLivePropReleaseImpulseVelocitySq)
            {
                const float dt = std::clamp(now - previousState.lastQueuedTime, 0.016f, 0.25f);
                derivedVelocity = (state.position - previousState.position) / dt;
            }
            if (IsFiniteVec3(derivedVelocity) && derivedVelocity.GetLengthSquared() >= kLivePropReleaseImpulseVelocitySq)
            {
                state.velocity = ClampLivePropVectorLength(derivedVelocity, kLivePropDerivedDropMaxSpeed);
                derivedReleaseVelocity = true;
            }
        }
        const bool releaseHasMotion =
            state.velocity.GetLengthSquared() >= kLivePropReleaseImpulseVelocitySq ||
            state.angularVelocity.GetLengthSquared() >= kLivePropReleaseImpulseAngularVelocitySq;
        if (releaseHasMotion)
        {
            state.flags |= CoopProtocol::kLivePropTransformFlagImpulse;
            state.releaseVelocity = state.velocity;
            state.releaseAngularVelocity = state.angularVelocity;
            state.releaseMotionUntilTime = now + kLivePropReleaseMotionLatchSeconds;
        }
        else
        {
            state.releaseVelocity = Vec3(ZERO);
            state.releaseAngularVelocity = Vec3(ZERO);
            state.releaseMotionUntilTime = -1000.0f;
        }
        state.carried = false;
        state.activelyMoving = true;
        state.remoteBallisticActive = false;
        state.remoteBallisticUntilTime = -1000.0f;
        state.remoteBallisticJustStarted = false;
        state.remoteLaunchVelocityApplied = false;
        state.leasePhase = LivePropLeasePhase::FlightLocal;
        const bool clientOrigin = m_networkMode == CoopNetworkMode::Client;
        const float authoritySeconds =
            clientOrigin && releaseHasMotion ? kLivePropReleaseFlightAuthoritySeconds :
            clientOrigin ? kLivePropClientDropAuthoritySeconds :
            0.45f;
        const float forceSendSeconds =
            clientOrigin && releaseHasMotion ?
            std::min(authoritySeconds, kLivePropClientReleaseForceSendSeconds) :
            authoritySeconds;
        state.activeUntilTime = now + authoritySeconds;
        state.localAuthorityUntilTime = now + authoritySeconds;
        state.remoteAuthorityUntilTime = -1000.0f;
        state.forceSendUntilTime = now + forceSendSeconds;
        state.pendingRemoteApply = false;
        state.remoteApplyStepsRemaining = 0;
        state.dirty = true;
        state.lastSentTime = -1000.0f;
        m_lastLivePropEvent =
            "released live prop authority guid=" + std::to_string(guid) +
            " clientOrigin=" + std::to_string(clientOrigin ? 1 : 0) +
            " releaseMotion=" + std::to_string(releaseHasMotion ? 1 : 0) +
            " derivedVel=" + std::to_string(derivedReleaseVelocity ? 1 : 0) +
            " authority=" + std::to_string(authoritySeconds) +
            " forceSend=" + std::to_string(forceSendSeconds);
        AppendLivePropDebugTrace("tick.release", state.guid, state.entityId, &state, m_lastLivePropEvent.c_str());
    };

    if (g_livePropLastCarriedEntityId != INVALID_ENTITYID &&
        g_livePropLastCarriedEntityId != carriedEntityId)
    {
        queueReleasedCarriedProp(g_livePropLastCarriedEntityId, g_livePropLastCarriedGuid, "released carried prop");
        g_livePropLastCarriedEntityId = INVALID_ENTITYID;
        g_livePropLastCarriedGuid = 0;
    }

    if (carriedEntityId != INVALID_ENTITYID && gEnv && gEnv->pEntitySystem)
    {
        IEntity* carriedEntity = gEnv->pEntitySystem->GetEntity(carriedEntityId);
        if (carriedEntity)
        {
            std::string carriedReason;
            TryGuardedCall(
                "live prop carried IEntity::GetGuid",
                [carriedEntity]() { return carriedEntity->GetGuid(); },
                carriedGuid,
                &carriedReason);
            if (carriedGuid != 0)
            {
                auto carriedIt = m_liveProps.find(carriedGuid);
                if (carriedIt != m_liveProps.end() &&
                    carriedIt->second.remoteAuthorityUntilTime > now &&
                    carriedIt->second.localAuthorityUntilTime <= now)
                {
                    // A completed native pickup is the strongest possible
                    // local ownership signal. Preempt the stale remote flight
                    // lease instead of undoing Vanilla's successful grab.
                    carriedIt->second.remoteAuthorityUntilTime = -1000.0f;
                    carriedIt->second.pendingRemoteApply = false;
                    carriedIt->second.remoteApplyStepsRemaining = 0;
                    carriedIt->second.remoteBallisticActive = false;
                    carriedIt->second.remoteBallisticUntilTime = -1000.0f;
                    carriedIt->second.remoteBallisticJustStarted = false;
                    carriedIt->second.remoteLaunchVelocityApplied = false;
                    carriedIt->second.leasePhase = LivePropLeasePhase::CarriedLocal;
                    carriedIt->second.localAuthorityUntilTime =
                        now + kLivePropLocalCarryAuthorityGraceSeconds;
                    carriedIt->second.forceSendUntilTime =
                        now + kLivePropLocalCarryAuthorityGraceSeconds;
                    m_lastLivePropEvent =
                        "local carry preempted remote lease guid=" +
                        std::to_string(carriedGuid);
                    AppendLivePropDebugTrace(
                        "tick.carry.preempt_remote",
                        carriedIt->second.guid,
                        carriedIt->second.entityId,
                        &carriedIt->second,
                        m_lastLivePropEvent.c_str());
                }
            }

            if (carriedEntityId != INVALID_ENTITYID)
                QueueLivePropTransform(*carriedEntity, false, "carried prop");
            if (carriedGuid != 0)
            {
                auto carriedIt = m_liveProps.find(carriedGuid);
                if (carriedIt != m_liveProps.end())
                {
                    carriedIt->second.velocity = Vec3(ZERO);
                    carriedIt->second.angularVelocity = Vec3(ZERO);
                    carriedIt->second.releaseAngularVelocity = Vec3(ZERO);
                    carriedIt->second.releaseMotionUntilTime = -1000.0f;
                    carriedIt->second.pendingRemoteApply = false;
                    carriedIt->second.remoteApplyStepsRemaining = 0;
                    carriedIt->second.remoteBallisticActive = false;
                    carriedIt->second.remoteBallisticUntilTime = -1000.0f;
                    carriedIt->second.remoteBallisticJustStarted = false;
                    carriedIt->second.remoteLaunchVelocityApplied = false;
                    carriedIt->second.remoteBlendStartTime = -1000.0f;
                    carriedIt->second.flags &= ~CoopProtocol::kLivePropTransformFlagImpulse;
                    carriedIt->second.flags |= CoopProtocol::kLivePropTransformFlagCarried | CoopProtocol::kLivePropTransformFlagActive;
                    carriedIt->second.carried = true;
                    carriedIt->second.activelyMoving = true;
                    carriedIt->second.leasePhase = LivePropLeasePhase::CarriedLocal;
                    carriedIt->second.localAuthorityUntilTime = now + kLivePropLocalCarryAuthorityGraceSeconds;
                    carriedIt->second.remoteAuthorityUntilTime = -1000.0f;
                    carriedIt->second.forceSendUntilTime = now + kLivePropLocalCarryAuthorityGraceSeconds;
                }
            }
            if (carriedEntityId != INVALID_ENTITYID)
            {
                g_livePropLastCarriedEntityId = carriedEntityId;
                g_livePropLastCarriedGuid = carriedGuid;
            }
        }
    }

    for (auto& entry : m_liveProps)
    {
        LivePropState& state = entry.second;
        if (!state.hasSnapshot ||
            state.carried ||
            now > state.forceSendUntilTime ||
            state.localAuthorityUntilTime <= now ||
            !IsKnownSameLevel(state.levelName, m_localLevelName))
        {
            continue;
        }

        std::string reason;
        IEntity* entity = ResolveLivePropEntity(state.guid, state.entityId, reason);
        if (!entity)
            continue;

        if (IsLocalPlayerCarryingEntity(entity->GetId()) &&
            !IsCarrySuppressActiveFor(entity->GetId(), state.guid, now))
            continue;

        LivePropState snapshot;
        if (!CaptureLivePropState(*entity, false, snapshot, reason, true))
            continue;

        const uint32_t lastReceivedSequence = state.lastReceivedSequence;
        const float lastSentTime = state.lastSentTime;
        const float localAuthorityUntilTime = state.localAuthorityUntilTime;
        const float forceSendUntilTime = state.forceSendUntilTime;
        const float contactAuthorityUntilTime = state.contactAuthorityUntilTime;
        const float releaseMotionUntilTime = state.releaseMotionUntilTime;
        const Vec3 releaseVelocity = state.releaseVelocity;
        const Vec3 releaseAngularVelocity = state.releaseAngularVelocity;
        const uint32_t previousFlags = state.flags;
        const Vec3 previousVelocity = state.velocity;
        const Vec3 previousAngularVelocity = state.angularVelocity;
        state = snapshot;
        state.lastReceivedSequence = lastReceivedSequence;
        state.lastSentTime = lastSentTime;
        state.localAuthorityUntilTime = localAuthorityUntilTime;
        state.remoteAuthorityUntilTime = -1000.0f;
        state.forceSendUntilTime = forceSendUntilTime;
        state.contactAuthorityUntilTime = contactAuthorityUntilTime;
        state.releaseMotionUntilTime = releaseMotionUntilTime;
        state.releaseVelocity = releaseVelocity;
        state.releaseAngularVelocity = releaseAngularVelocity;
        state.carried = false;
        state.flags &= ~CoopProtocol::kLivePropTransformFlagCarried;
        state.flags |= CoopProtocol::kLivePropTransformFlagActive;
        const bool releaseMotionActive =
            now <= state.releaseMotionUntilTime &&
            (state.releaseVelocity.GetLengthSquared() >= kLivePropReleaseImpulseVelocitySq ||
                state.releaseAngularVelocity.GetLengthSquared() >= kLivePropReleaseImpulseAngularVelocitySq);
        if (releaseMotionActive)
        {
            if (state.velocity.GetLengthSquared() < kLivePropBallisticVelocitySq &&
                state.releaseVelocity.GetLengthSquared() >= kLivePropBallisticVelocitySq)
            {
                state.velocity = state.releaseVelocity;
            }
            if (state.angularVelocity.GetLengthSquared() < kLivePropBallisticAngularVelocitySq &&
                state.releaseAngularVelocity.GetLengthSquared() >= kLivePropBallisticAngularVelocitySq)
            {
                state.angularVelocity = state.releaseAngularVelocity;
            }
            state.activelyMoving = true;
            state.activeUntilTime = std::max(state.activeUntilTime, state.releaseMotionUntilTime);
        }
        if ((previousFlags & CoopProtocol::kLivePropTransformFlagImpulse) != 0 && now <= forceSendUntilTime)
        {
            state.flags |= CoopProtocol::kLivePropTransformFlagActive;
            state.flags |= CoopProtocol::kLivePropTransformFlagImpulse;
            if (state.velocity.GetLengthSquared() < kLivePropBallisticVelocitySq &&
                previousVelocity.GetLengthSquared() >= kLivePropBallisticVelocitySq)
            {
                state.velocity = previousVelocity;
            }
            if (state.angularVelocity.GetLengthSquared() < kLivePropBallisticAngularVelocitySq &&
                previousAngularVelocity.GetLengthSquared() >= kLivePropBallisticAngularVelocitySq)
            {
                state.angularVelocity = previousAngularVelocity;
            }
        }
        state.activelyMoving = true;
        state.activeUntilTime = std::max(state.activeUntilTime, forceSendUntilTime);
        state.remoteBallisticActive = false;
        state.remoteBallisticUntilTime = -1000.0f;
        state.leasePhase = LivePropLeasePhase::FlightLocal;
        state.pendingRemoteApply = false;
        state.remoteApplyStepsRemaining = 0;
        state.dirty = true;
        AppendLivePropDebugTrace("tick.local_authority_capture", state.guid, state.entityId, &state, "force send active prop");
    }

    size_t appliedRemoteSteps = 0;
    for (auto& entry : m_liveProps)
    {
        LivePropState& state = entry.second;
        if (!state.pendingRemoteApply || !state.hasSnapshot || !IsKnownSameLevel(state.levelName, m_localLevelName))
            continue;
        if (state.localAuthorityUntilTime > now)
            continue;

        std::string reason;
        IEntity* entity = ResolveLivePropEntity(state.guid, state.entityId, reason);
        if (!entity)
        {
            state.pendingRemoteApply = false;
            state.remoteApplyStepsRemaining = 0;
            ++m_droppedLivePropPackets;
            m_lastLivePropEvent = "dropped pending live prop guid=" + std::to_string(state.guid) + " reason=" + reason;
            continue;
        }

        const EntityId entityId = entity->GetId();
        if (IsLocalPlayerCarryingEntity(entityId))
        {
            state.pendingRemoteApply = false;
            state.remoteApplyStepsRemaining = 0;
            ++m_droppedLivePropPackets;
            m_lastLivePropEvent = "dropped pending live prop guid=" + std::to_string(state.guid) + " reason=locally carried entity";
            continue;
        }

        const bool carried = (state.flags & CoopProtocol::kLivePropTransformFlagCarried) != 0;
        const bool activeMotion = !carried && state.activelyMoving;
        const bool finalStep = state.remoteApplyStepsRemaining <= 1;
        const bool applyOk = activeMotion ?
            ApplyLivePropRemoteBallisticState(*entity, state, reason) :
            ApplyLivePropRemoteState(*entity, state, finalStep, reason);
        if (!applyOk)
        {
            state.pendingRemoteApply = false;
            state.remoteApplyStepsRemaining = 0;
            ++m_droppedLivePropPackets;
            m_lastLivePropEvent = "apply pending live prop failed guid=" + std::to_string(state.guid) + " reason=" + reason;
            continue;
        }
        if (activeMotion)
            ++m_livePropMovingApplies;

        ++appliedRemoteSteps;
        if (appliedRemoteSteps >= kMaxLivePropSendsPerTick)
            break;
    }

    m_livePropTickAccumulator += std::max(0.0f, frameTime);
    if (m_livePropTickAccumulator < kLivePropSyncSeconds)
        return;

    m_livePropTickAccumulator = 0.0f;
    size_t sent = 0;
    std::string sendSummary;
    Vec3 localPlayerPosition(ZERO);
    Vec3 remotePlayerPosition(ZERO);
    bool hasLocalPlayerPosition = false;
    bool hasRemotePlayerPosition = false;
    if (ArkPlayer::GetInstancePtr())
    {
        if (IEntity* playerEntity = ArkPlayer::GetInstance().GetEntity())
        {
            localPlayerPosition = playerEntity->GetWorldPos();
            hasLocalPlayerPosition = true;
        }
    }
    if (IEntity* proxyEntity = GetProxyEntity())
    {
        remotePlayerPosition = proxyEntity->GetWorldPos();
        hasRemotePlayerPosition = true;
    }

    auto trySendState = [&](LivePropState& state, const char* label) -> bool
    {
        if (!state.dirty || !state.hasSnapshot || !IsKnownSameLevel(state.levelName, m_localLevelName))
            return false;

        constexpr uint16_t packetType =
            static_cast<uint16_t>(CoopProtocol::PacketType::LivePropTransform);
        m_networkTelemetry.RecordProducerAttempt(packetType);

        const bool carried = (state.flags & CoopProtocol::kLivePropTransformFlagCarried) != 0;
        const bool removed = (state.flags & CoopProtocol::kLivePropTransformFlagRemoved) != 0;
        if (!carried &&
            IsLobbyMainLiftLooseCargoPosition(state.levelName, state.position))
        {
            state.dirty = false;
            state.pendingRemoteApply = false;
            state.remoteApplyStepsRemaining = 0;
            m_networkTelemetry.RecordProducerSuppressed(
                packetType,
                CoopNetworkTelemetry::ProducerSuppressionReason::InvalidState);
            m_lastLivePropEvent =
                "suppressed peer-local main lift loose cargo guid=" +
                std::to_string(state.guid);
            return false;
        }
        const bool localCarryNow =
            carried &&
            IsLocalPlayerCarryingEntity(state.entityId) &&
            !IsCarrySuppressActiveFor(state.entityId, state.guid, now);
        const bool localBurst = state.localAuthorityUntilTime > now;
        const bool localFreeAuthority = m_networkMode == CoopNetworkMode::Host || IsClientAreaAuthorityActive();
        const bool localLeasePhase = IsLivePropLocalLeasePhase(state.leasePhase);
        if (carried && !localCarryNow)
        {
            state.flags &= ~CoopProtocol::kLivePropTransformFlagCarried;
            state.carried = false;
            state.pendingRemoteApply = false;
            state.remoteApplyStepsRemaining = 0;
            state.dirty = false;
            ++m_droppedLivePropPackets;
            m_networkTelemetry.RecordProducerSuppressed(
                packetType,
                CoopNetworkTelemetry::ProducerSuppressionReason::InvalidState);
            m_lastLivePropEvent =
                "suppressed stale carried live prop send guid=" + std::to_string(state.guid) +
                " localBurst=" + std::to_string(localBurst ? 1 : 0);
            AppendLivePropDebugTrace("send.suppress.stale_carried", state.guid, state.entityId, &state, m_lastLivePropEvent.c_str());
            return false;
        }

        const bool hasLocalAuthority =
            localCarryNow ||
            (!carried && localBurst && localLeasePhase) ||
            (!carried && localFreeAuthority);
        if (state.remoteAuthorityUntilTime > now && !localCarryNow && !localBurst)
        {
            m_networkTelemetry.RecordProducerSuppressed(
                packetType,
                CoopNetworkTelemetry::ProducerSuppressionReason::Authority);
            return false;
        }
        if (!hasLocalAuthority)
        {
            if (!localFreeAuthority)
                state.dirty = false;
            m_lastLivePropEvent =
                "suppressed live prop send no authority guid=" + std::to_string(state.guid) +
                " carried=" + std::to_string(carried ? 1 : 0);
            m_networkTelemetry.RecordProducerSuppressed(
                packetType,
                CoopNetworkTelemetry::ProducerSuppressionReason::Authority);
            return false;
        }

        const bool forceHighRate = now <= state.forceSendUntilTime || state.localAuthorityUntilTime > now;
        const float interestDistanceSq = std::min(
            hasLocalPlayerPosition
                ? (state.position - localPlayerPosition).GetLengthSquared()
                : std::numeric_limits<float>::max(),
            hasRemotePlayerPosition
                ? (state.position - remotePlayerPosition).GetLengthSquared()
                : std::numeric_limits<float>::max());
        const float idleSendInterval =
            interestDistanceSq > kLivePropFarInterestDistanceSq ? kLivePropFarIdleSendSeconds :
            interestDistanceSq > kLivePropMidInterestDistanceSq ? kLivePropMidIdleSendSeconds :
            kLivePropIdleSendSeconds;
        const float movingSendInterval =
            interestDistanceSq > kLivePropFarInterestDistanceSq ? 0.200f :
            interestDistanceSq > kLivePropMidInterestDistanceSq ? 0.120f :
            kLivePropMovingSendSeconds;
        const float settleSendInterval =
            interestDistanceSq > kLivePropFarInterestDistanceSq ? 0.500f :
            interestDistanceSq > kLivePropMidInterestDistanceSq ? 0.250f :
            kLivePropSettleSendSeconds;
        const float sendInterval =
            removed ? 0.0f :
            (carried || forceHighRate) ? kLivePropCarriedSendSeconds :
            state.activelyMoving ? movingSendInterval :
            now <= state.activeUntilTime ? settleSendInterval :
            idleSendInterval;
        if ((now - state.lastSentTime) < sendInterval)
        {
            m_networkTelemetry.RecordProducerSuppressed(
                packetType,
                CoopNetworkTelemetry::ProducerSuppressionReason::RateLimited);
            return false;
        }

        CoopProtocol::LivePropTransformPacket packet = {};
        if (!BuildLivePropTransformPacket(packet, state))
        {
            m_networkTelemetry.RecordProducerSuppressed(
                packetType,
                CoopNetworkTelemetry::ProducerSuppressionReason::InvalidState);
            return false;
        }
        if (!SendLivePropTransformTo(packet, m_remoteAddress, m_remotePort, "live prop send failed"))
            return false;

        const bool sentImpulse = (state.flags & CoopProtocol::kLivePropTransformFlagImpulse) != 0;
        state.dirty = false;
        state.lastSentTime = now;
        if (sentImpulse)
            state.flags &= ~CoopProtocol::kLivePropTransformFlagImpulse;
        sendSummary =
            std::string("sent ") + (label ? label : "live prop") +
            " guid=" + std::to_string(state.guid) +
            " carried=" + std::to_string(carried ? 1 : 0) +
            " active=" + std::to_string(state.activelyMoving ? 1 : 0) +
            " impulse=" + std::to_string(sentImpulse ? 1 : 0) +
            " clientAuth=" + std::to_string((packet.flags & CoopProtocol::kLivePropTransformFlagClientAuthority) != 0 ? 1 : 0) +
            " localAuth=" + std::to_string(state.localAuthorityUntilTime > now ? 1 : 0) +
            " areaAuth=" + std::to_string(localFreeAuthority ? 1 : 0) +
            " remoteSuppressed=" + std::to_string(state.remoteAuthorityUntilTime > now ? 1 : 0);
        AppendLivePropDebugTrace("send", state.guid, state.entityId, &state, sendSummary.c_str());
        return true;
    };

    if (carriedGuid != 0)
    {
        auto carriedIt = m_liveProps.find(carriedGuid);
        if (carriedIt != m_liveProps.end() && trySendState(carriedIt->second, "carried live prop"))
            ++sent;
    }

    for (auto& entry : m_liveProps)
    {
        if (entry.first == carriedGuid)
            continue;

        LivePropState& state = entry.second;
        if (!trySendState(state, "live prop"))
            continue;

        ++sent;
        if (sent >= kMaxLivePropSendsPerTick)
            break;
    }

    if (sent > 0)
        m_lastLivePropEvent = sendSummary.empty() ? "sent live props " + std::to_string(sent) : sendSummary;
}

IEntity* ModMain::ResolveLivePropEntity(uint64_t guid, EntityId cachedEntityId, std::string& reason)
{
    if (!gEnv || !gEnv->pEntitySystem || guid == 0)
    {
        reason = "no entity system";
        return nullptr;
    }

    if (IsSyntheticLivePropGuid(guid))
    {
        const EntityId syntheticEntityId = DecodeSyntheticLivePropEntityId(guid);
        const EntityId entityId = cachedEntityId != INVALID_ENTITYID ? cachedEntityId : syntheticEntityId;
        if (entityId != INVALID_ENTITYID &&
            BuildSyntheticLivePropGuid(entityId) == guid)
        {
            IEntity* entity = nullptr;
            if (TryGuardedCall("live prop synthetic GetEntity", [entityId]() { return gEnv->pEntitySystem->GetEntity(entityId); }, entity, &reason) &&
                entity)
            {
                return entity;
            }
        }

        if (syntheticEntityId != INVALID_ENTITYID && syntheticEntityId != cachedEntityId)
        {
            IEntity* entity = nullptr;
            if (TryGuardedCall("live prop synthetic decoded GetEntity", [syntheticEntityId]() { return gEnv->pEntitySystem->GetEntity(syntheticEntityId); }, entity, &reason) &&
                entity)
            {
                return entity;
            }
        }

        reason = "synthetic entity not found";
        return nullptr;
    }

    if (cachedEntityId != INVALID_ENTITYID)
    {
        IEntity* cached = gEnv->pEntitySystem->GetEntity(cachedEntityId);
        uint64_t cachedGuid = 0;
        if (cached &&
            TryGuardedCall("live prop cached guid", [cached]() { return cached->GetGuid(); }, cachedGuid, &reason) &&
            cachedGuid == guid)
        {
            return cached;
        }
    }

    IEntityIt* iterator = gEnv->pEntitySystem->GetEntityIterator();
    if (!iterator)
    {
        reason = "no iterator";
        return nullptr;
    }

    IEntity* found = nullptr;
    iterator->MoveFirst();
    while (!iterator->IsEnd())
    {
        IEntity* entity = iterator->Next();
        if (!entity)
            continue;

        uint64_t entityGuid = 0;
        if (TryGuardedCall("live prop scan guid", [entity]() { return entity->GetGuid(); }, entityGuid, &reason) &&
            entityGuid == guid)
        {
            found = entity;
            break;
        }
    }
    iterator->Release();

    if (!found)
        reason = "entity not found";
    return found;
}

bool ModMain::ApplyLivePropRemoteState(IEntity& entity, LivePropState& state, bool finalStep, std::string& reason)
{
    const float now = NowSeconds();
    const bool carried = (state.flags & CoopProtocol::kLivePropTransformFlagCarried) != 0;
    if (!finalStep && !carried && now - state.lastAppliedTime < kLivePropMinApplySeconds)
        return true;

    Vec3 currentPosition = Vec3(ZERO);
    Quat currentRotation = Quat::CreateIdentity();
    Vec3 currentScale = Vec3Constants<float>::fVec3_One;
    if (!TryGuardedCall("live prop apply GetWorldPos", [&entity]() { return entity.GetWorldPos(); }, currentPosition, &reason) ||
        !TryGuardedCall("live prop apply GetWorldRotation", [&entity]() { return entity.GetWorldRotation(); }, currentRotation, &reason) ||
        !TryGuardedCall("live prop apply GetScale", [&entity]() { return Vec3(entity.GetScale()); }, currentScale, &reason))
    {
        return false;
    }

    if (!IsFiniteVec3(currentPosition) ||
        !IsFiniteQuat(currentRotation) ||
        !IsReasonableScale(currentScale) ||
        !IsFiniteVec3(state.targetPosition) ||
        !IsFiniteQuat(state.targetRotation) ||
        !IsReasonableScale(state.targetScale))
    {
        reason = "invalid apply transform";
        return false;
    }

    const float targetDeltaSq = (state.targetPosition - currentPosition).GetLengthSquared();
    const bool hardSnap = targetDeltaSq > kLivePropMaxRemoteTargetDeltaSq;
    const bool closeEnough =
        targetDeltaSq < kLivePropRemoteApplyPositionDeltaSq &&
        (state.targetScale - currentScale).GetLengthSquared() < kLivePropMinScaleDeltaSq &&
        !RotationChangedEnough(state.targetRotation, currentRotation, 0.00025f);
    const bool snapNow = carried || hardSnap || (finalStep && closeEnough);
    const float alpha = snapNow ? 1.0f : (finalStep ? 0.35f : 0.45f);
    const Vec3 nextPosition = currentPosition + (state.targetPosition - currentPosition) * alpha;
    Quat nextRotation = Quat::CreateNlerp(currentRotation, state.targetRotation, alpha);
    nextRotation.Normalize();
    const Vec3 nextScale = currentScale + (state.targetScale - currentScale) * alpha;

    const bool needsTransform =
        finalStep ||
        targetDeltaSq >= kLivePropRemoteApplyPositionDeltaSq ||
        (state.targetScale - currentScale).GetLengthSquared() >= kLivePropMinScaleDeltaSq ||
        RotationChangedEnough(state.targetRotation, currentRotation, 0.00025f);
    const bool activePacket = (state.flags & CoopProtocol::kLivePropTransformFlagActive) != 0;
    const bool settleCorrection = !carried && !activePacket;
    const bool shouldTouchPhysics =
        carried ||
        hardSnap ||
        (!settleCorrection && targetDeltaSq > kLivePropBallisticSoftCorrectDeltaSq) ||
        (settleCorrection && targetDeltaSq > 1.0f);

    m_livePropApplyActive = true;
    bool ok = true;
    if (carried)
        ok = SetEntityPhysicsEnabledGuarded(entity, false, reason);
    else
        ok = SetEntityPhysicsEnabledGuarded(entity, true, reason);

    if (needsTransform)
    {
        ok = ok && TryGuardedVoidCall(
            "live prop SetPosRotScale interpolated",
            [&entity, &nextPosition, &nextRotation, &nextScale]()
            {
                entity.SetPosRotScale(nextPosition, nextRotation, nextScale, ENTITY_XFORM_USER);
            },
            &reason);
    }

    if (ok && shouldTouchPhysics)
        ok = ApplyPhysicsTransform(entity, nextPosition, nextRotation, nextScale, reason);

    if (ok)
    {
        const bool hidden = (state.flags & CoopProtocol::kLivePropTransformFlagHidden) != 0;
        ok = TryGuardedVoidCall(
            "live prop Hide interpolated",
            [&entity, hidden]() { entity.Hide(hidden); },
            &reason);
    }

    if (ok && EnvFlagDefaultEnabled("COOP_LIVE_PROP_APPLY_VELOCITY") && !settleCorrection && !carried && state.activelyMoving)
    {
        ok = ApplyPhysicsVelocityAndWake(entity, state.velocity, state.angularVelocity, 0.35f, reason);
    }
    else if (ok && !carried && !state.activelyMoving)
    {
        // Active also denotes a short ownership lease. A zero-motion lease
        // must not wake the receiver and let gravity move it away from the
        // authoritative settled pose after the final packet.
        ok = StopAndSleepPhysics(entity, nextPosition, nextRotation, reason);
    }
    m_livePropApplyActive = false;

    if (!ok)
        return false;

    state.position = snapNow ? state.targetPosition : nextPosition;
    state.rotation = snapNow ? state.targetRotation : nextRotation;
    state.scale = snapNow ? state.targetScale : nextScale;
    state.lastAppliedTime = now;
    if (snapNow || carried || (!finalStep && state.remoteApplyStepsRemaining == 0))
    {
        state.pendingRemoteApply = false;
        state.remoteApplyStepsRemaining = 0;
        if (carried)
            state.leasePhase = LivePropLeasePhase::CarriedRemote;
        else if (!state.activelyMoving)
            state.leasePhase = LivePropLeasePhase::Settled;
    }
    else if (finalStep)
    {
        state.pendingRemoteApply = true;
        state.remoteApplyStepsRemaining = 1;
    }
    else
    {
        --state.remoteApplyStepsRemaining;
    }
    return true;
}

bool ModMain::ApplyLivePropRemoteBallisticState(IEntity& entity, LivePropState& state, std::string& reason)
{
    const float now = NowSeconds();
    Vec3 currentPosition = Vec3(ZERO);
    Quat currentRotation = Quat::CreateIdentity();
    Vec3 currentScale = Vec3Constants<float>::fVec3_One;
    if (!TryGuardedCall("live prop ballistic GetWorldPos", [&entity]() { return entity.GetWorldPos(); }, currentPosition, &reason) ||
        !TryGuardedCall("live prop ballistic GetWorldRotation", [&entity]() { return entity.GetWorldRotation(); }, currentRotation, &reason) ||
        !TryGuardedCall("live prop ballistic GetScale", [&entity]() { return Vec3(entity.GetScale()); }, currentScale, &reason))
    {
        return false;
    }

    if (!IsFiniteVec3(currentPosition) ||
        !IsFiniteQuat(currentRotation) ||
        !IsReasonableScale(currentScale) ||
        !IsFiniteVec3(state.targetPosition) ||
        !IsFiniteQuat(state.targetRotation) ||
        !IsReasonableScale(state.targetScale))
    {
        reason = "invalid ballistic transform";
        return false;
    }

    const Vec3 targetDelta = state.targetPosition - currentPosition;
    const float targetDeltaSq = targetDelta.GetLengthSquared();
    const bool clientAuthorityPacket = (state.flags & CoopProtocol::kLivePropTransformFlagClientAuthority) != 0;
    const bool emergencyCorrect = targetDeltaSq >= kLivePropBallisticEmergencyCorrectDeltaSq;
    const float positionAlpha = emergencyCorrect ? 0.18f : 0.0f;
    const bool hasLaunchVelocity =
        state.velocity.GetLengthSquared() >= kLivePropReleaseImpulseVelocitySq ||
        state.angularVelocity.GetLengthSquared() >= kLivePropReleaseImpulseAngularVelocitySq;
    const bool shouldApplyLaunchVelocity =
        state.remoteBallisticJustStarted &&
        !state.remoteLaunchVelocityApplied &&
        hasLaunchVelocity;
    Vec3 nextPosition = currentPosition;
    Quat nextRotation = currentRotation;
    Vec3 nextScale = currentScale;

    m_livePropApplyActive = true;
    bool ok = true;
    ok = SetEntityPhysicsEnabledGuarded(entity, true, reason);
    if (ok && emergencyCorrect)
    {
        ++m_livePropBallisticCorrections;
        nextPosition = currentPosition + (state.targetPosition - currentPosition) * positionAlpha;
        nextRotation = Quat::CreateNlerp(currentRotation, state.targetRotation, positionAlpha);
        nextRotation.Normalize();
        nextScale = currentScale + (state.targetScale - currentScale) * positionAlpha;
        ok = TryGuardedVoidCall(
            "live prop authority blend SetPosRotScale correction",
            [&entity, &nextPosition, &nextRotation, &nextScale]()
            {
                entity.SetPosRotScale(nextPosition, nextRotation, nextScale, ENTITY_XFORM_USER);
            },
            &reason);
        if (ok)
            ok = ApplyPhysicsTransform(entity, nextPosition, nextRotation, nextScale, reason);
    }

    if (ok)
    {
        const bool hidden = (state.flags & CoopProtocol::kLivePropTransformFlagHidden) != 0;
        ok = TryGuardedVoidCall(
            "live prop ballistic Hide",
            [&entity, hidden]() { entity.Hide(hidden); },
            &reason);
    }

    if (ok && shouldApplyLaunchVelocity && EnvFlagDefaultEnabled("COOP_LIVE_PROP_APPLY_VELOCITY"))
    {
        bool launchAligned = false;
        // A Client-authority packet is the handoff pose from the peer that
        // actually moved or released the prop. Align the Host to that pose
        // before it assumes the central physics simulation; importing only
        // the velocity from an older Host position makes long falls settle in
        // different places on every peer.
        if (clientAuthorityPacket ||
            targetDeltaSq <= kLivePropBallisticLaunchAlignDeltaSq)
        {
            nextPosition = state.targetPosition;
            nextRotation = state.targetRotation;
            nextScale = state.targetScale;
            ok = TryGuardedVoidCall(
                "live prop launch SetPosRotScale",
                [&entity, &nextPosition, &nextRotation, &nextScale]()
                {
                    entity.SetPosRotScale(nextPosition, nextRotation, nextScale, ENTITY_XFORM_USER);
                },
                &reason);
            if (ok)
                ok = ApplyPhysicsTransform(entity, nextPosition, nextRotation, nextScale, reason);
            launchAligned = ok;
        }

        if (ok)
            ok = ApplyPhysicsVelocityAndWake(entity, state.velocity, state.angularVelocity, 0.55f, reason);

        m_livePropApplyActive = false;
        if (!ok)
            return false;

        state.position = nextPosition;
        state.rotation = nextRotation;
        state.scale = nextScale;
        state.lastAppliedTime = now;
        state.remoteBallisticJustStarted = false;
        state.remoteLaunchVelocityApplied = true;
        state.pendingRemoteApply = false;
        state.remoteApplyStepsRemaining = 0;
        ++m_livePropBallisticApplies;

        const std::string detail =
            "deltaSq=" + std::to_string(targetDeltaSq) +
            " clientAuth=" + std::to_string(clientAuthorityPacket ? 1 : 0) +
            " aligned=" + std::to_string(launchAligned ? 1 : 0) +
            " launchVel=" + FormatVec3Compact(state.velocity) +
            " launchAngVel=" + FormatVec3Compact(state.angularVelocity);
        AppendLivePropDebugTrace("apply.ballistic.launch", state.guid, state.entityId, &state, detail.c_str());
        return true;
    }

    const bool shouldMirrorAuthoritativePose =
        !clientAuthorityPacket &&
        state.activelyMoving &&
        targetDeltaSq >= kLivePropRemoteApplyPositionDeltaSq;
    if (ok && shouldMirrorAuthoritativePose)
    {
        const float mirrorAlpha =
            targetDeltaSq >= kLivePropBallisticHardCorrectDeltaSq ? 0.72f :
            targetDeltaSq >= kLivePropBallisticSoftCorrectDeltaSq ? 0.48f :
            0.32f;
        nextPosition = currentPosition + (state.targetPosition - currentPosition) * mirrorAlpha;
        nextRotation = Quat::CreateNlerp(currentRotation, state.targetRotation, mirrorAlpha);
        nextRotation.Normalize();
        nextScale = currentScale + (state.targetScale - currentScale) * mirrorAlpha;
        ok = TryGuardedVoidCall(
            "live prop mirror authoritative SetPosRotScale",
            [&entity, &nextPosition, &nextRotation, &nextScale]()
            {
                entity.SetPosRotScale(nextPosition, nextRotation, nextScale, ENTITY_XFORM_USER);
            },
            &reason);
        if (ok)
            ok = ApplyPhysicsTransform(entity, nextPosition, nextRotation, nextScale, reason);
        if (ok)
        {
            const std::string detail =
                "deltaSq=" + std::to_string(targetDeltaSq) +
                " alpha=" + std::to_string(mirrorAlpha) +
                " target=" + FormatVec3Compact(state.targetPosition);
            AppendLivePropDebugTrace("apply.ballistic.mirror_pose", state.guid, state.entityId, &state, detail.c_str());
        }
    }

    const bool shouldCoupleVelocity =
        shouldApplyLaunchVelocity ||
        state.activelyMoving ||
        targetDeltaSq >= kLivePropBallisticVelocityCoupleDeltaSq ||
        state.angularVelocity.GetLengthSquared() >= kLivePropReleaseImpulseAngularVelocitySq;
    if (ok && EnvFlagDefaultEnabled("COOP_LIVE_PROP_APPLY_VELOCITY") && shouldCoupleVelocity)
    {
        Vec3 currentVelocity(ZERO);
        Vec3 currentAngularVelocity(ZERO);
        CapturePhysicsDynamics(entity, currentVelocity, currentAngularVelocity);
        currentVelocity = SanitizeLivePropVelocity(currentVelocity);
        currentAngularVelocity = SanitizeLivePropAngularVelocity(currentAngularVelocity);

        const float blendDuration = std::max(kLivePropBallisticMinRemainingBlendSeconds, state.remoteBlendDuration);
        const float blendAge = state.remoteBlendStartTime > -999.0f ?
            std::max(0.0f, now - state.remoteBlendStartTime) :
            blendDuration;
        const float blendT = std::clamp(blendAge / blendDuration, 0.0f, 1.0f);
        const float remainingBlendSeconds =
            std::max(kLivePropBallisticMinRemainingBlendSeconds, blendDuration - std::min(blendAge, blendDuration * 0.95f));
        Vec3 correctionVelocity(ZERO);
        if (clientAuthorityPacket)
        {
            correctionVelocity = ClampLivePropVectorLength(
                targetDelta * (1.0f / remainingBlendSeconds),
                kLivePropBallisticMaxClientAuthorityCorrectionSpeed);
        }
        else if (targetDeltaSq >= kLivePropBallisticHardCorrectDeltaSq)
        {
            correctionVelocity = ClampLivePropVectorLength(
                targetDelta * (0.25f / remainingBlendSeconds),
                kLivePropBallisticMaxCorrectionSpeed * 0.25f);
        }
        Vec3 desiredVelocity = hasLaunchVelocity ? state.velocity : Vec3(ZERO);
        desiredVelocity = ClampLivePropVectorLength(desiredVelocity + correctionVelocity, kLivePropBallisticMaxDesiredSpeed);
        const float alphaMin = clientAuthorityPacket ?
            kLivePropBallisticHostVelocityAlphaMin :
            kLivePropBallisticClientVelocityAlphaMin;
        const float alphaMax = clientAuthorityPacket ?
            kLivePropBallisticHostVelocityAlphaMax :
            kLivePropBallisticClientVelocityAlphaMax;
        const float velocityAlpha = shouldApplyLaunchVelocity ?
            1.0f :
            std::clamp(alphaMin + (alphaMax - alphaMin) * blendT, alphaMin, alphaMax);
        const Vec3 nextVelocity = currentVelocity + (desiredVelocity - currentVelocity) * velocityAlpha;
        const Vec3 nextAngularVelocity =
            currentAngularVelocity + (state.angularVelocity - currentAngularVelocity) * std::min(1.0f, velocityAlpha * 0.85f);
        ok = ApplyPhysicsVelocityAndWake(entity, nextVelocity, nextAngularVelocity, 0.55f, reason);
        if (ok)
        {
            const std::string detail =
                "deltaSq=" + std::to_string(targetDeltaSq) +
                " clientAuth=" + std::to_string(clientAuthorityPacket ? 1 : 0) +
                " launch=" + std::to_string(shouldApplyLaunchVelocity ? 1 : 0) +
                " blendT=" + std::to_string(blendT) +
                " alpha=" + std::to_string(velocityAlpha) +
                " desiredVel=" + FormatVec3Compact(desiredVelocity) +
                " nextVel=" + FormatVec3Compact(nextVelocity);
            AppendLivePropDebugTrace("apply.ballistic.velocity", state.guid, state.entityId, &state, detail.c_str());
        }
        if (ok)
            state.remoteLaunchVelocityApplied = true;
    }

    m_livePropApplyActive = false;
    if (!ok)
        return false;

    state.position = nextPosition;
    state.rotation = nextRotation;
    state.scale = nextScale;
    state.lastAppliedTime = now;
    state.remoteBallisticJustStarted = false;
    state.pendingRemoteApply = false;
    state.remoteApplyStepsRemaining = 0;
    ++m_livePropBallisticApplies;
    AppendLivePropDebugTrace("apply.ballistic.done", state.guid, state.entityId, &state, "pending=0");
    return true;
}

void ModMain::HandleLivePropTransform(const CoopProtocol::LivePropTransformPacket& packet)
{
    ++m_receivedLivePropPackets;
    m_receivedLivePropBytes += sizeof(CoopProtocol::LivePropTransformPacket);

    if (!m_enableLivePropSync ||
        packet.guid == 0 ||
        packet.worldEpoch != m_localWorldEpoch ||
        !IsSessionGameplayReady())
    {
        ++m_droppedLivePropPackets;
        return;
    }

    const std::string packetLevel = NormalizeLevelName(ReadLivePropLevelName(packet));
    if (!IsKnownSameLevel(packetLevel, m_localLevelName))
    {
        ++m_droppedLivePropPackets;
        m_lastLivePropEvent = "dropped live prop for level " + packetLevel;
        return;
    }

    Vec3 packetPosition(packet.px, packet.py, packet.pz);
    Quat packetRotation(packet.qw, packet.qx, packet.qy, packet.qz);
    Vec3 packetScale(packet.sx, packet.sy, packet.sz);
    Vec3 packetVelocity(packet.vx, packet.vy, packet.vz);
    Vec3 packetAngularVelocity(packet.wx, packet.wy, packet.wz);
    if (!IsFiniteVec3(packetPosition) ||
        !IsFiniteQuat(packetRotation) ||
        !IsReasonableScale(packetScale))
    {
        ++m_droppedLivePropPackets;
        m_lastLivePropEvent = "dropped live prop guid=" + std::to_string(packet.guid) + " reason=invalid packet transform";
        return;
    }
    packetRotation.Normalize();
    packetVelocity = SanitizeLivePropVelocity(packetVelocity);
    packetAngularVelocity = SanitizeLivePropAngularVelocity(packetAngularVelocity);
    const bool packetCarried = (packet.flags & CoopProtocol::kLivePropTransformFlagCarried) != 0;
    const bool packetActive = (packet.flags & CoopProtocol::kLivePropTransformFlagActive) != 0;
    const bool packetImpulse = (packet.flags & CoopProtocol::kLivePropTransformFlagImpulse) != 0;
    const bool packetClientAuthority = (packet.flags & CoopProtocol::kLivePropTransformFlagClientAuthority) != 0;
    const float now = NowSeconds();
    if (!packetCarried &&
        IsLobbyMainLiftLooseCargoPosition(packetLevel, packetPosition))
    {
        ++m_droppedLivePropPackets;
        m_lastLivePropEvent =
            "dropped peer-local main lift loose cargo guid=" +
            std::to_string(packet.guid);
        m_liveProps.erase(packet.guid);
        return;
    }

    LivePropState& state = m_liveProps[packet.guid];
    if (CoopSerialSequence::IsStaleOrDuplicate(packet.sequence, state.lastReceivedSequence))
    {
        ++m_droppedLivePropPackets;
        return;
    }
    if (packetCarried && state.remoteBallisticActive && now <= state.remoteBallisticUntilTime)
    {
        ++m_droppedLivePropPackets;
        m_lastLivePropEvent =
            "dropped stale carried live prop guid=" + std::to_string(packet.guid) +
            " reason=remote ballistic active";
        AppendLivePropDebugTrace("recv.drop.stale_carried", packet.guid, state.entityId, &state, m_lastLivePropEvent.c_str());
        return;
    }

    std::string reason;
    IEntity* entity = ResolveLivePropEntity(packet.guid, state.entityId, reason);
    if (!entity || !ShouldTrackLivePropEntity(*entity, reason, true))
    {
        ++m_droppedLivePropPackets;
        m_lastLivePropEvent = "dropped live prop guid=" + std::to_string(packet.guid) + " reason=" + reason;
        m_liveProps.erase(packet.guid);
        return;
    }

    const EntityId entityId = entity->GetId();
    const bool releaseSuppressActive =
        g_livePropCarrySuppressEntityId != INVALID_ENTITYID &&
        now <= g_livePropCarrySuppressUntilTime &&
        (entityId == g_livePropCarrySuppressEntityId ||
            (g_livePropCarrySuppressGuid != 0 && packet.guid == g_livePropCarrySuppressGuid));
    if (IsLocalPlayerCarryingEntity(entityId))
    {
        if (m_networkMode == CoopNetworkMode::Client && !packetCarried && releaseSuppressActive)
        {
            TryGuardedVoidCall(
                "live prop accept remote release DropCarriedEntity",
                []()
                {
                    ArkPlayer::GetInstance().m_interaction.m_playerCarry.DropCarriedEntity();
                },
                &reason);
        }
        else
        {
            ++m_droppedLivePropPackets;
            m_lastLivePropEvent =
                "dropped live prop guid=" + std::to_string(packet.guid) +
                " reason=locally carried entity packetCarried=" + std::to_string(packetCarried ? 1 : 0);
            return;
        }
    }

    // A completed remote Vanilla pickup is stronger than a stale local
    // flight/collision lease. A simultaneous local pickup was already
    // rejected above by IsLocalPlayerCarryingEntity, so accepting carried
    // here cannot steal an object from the local player's hands.
    const bool localAuthorityActive =
        state.localAuthorityUntilTime > now &&
        !packetCarried;
    const float previousLocalAuthorityUntilTime = state.localAuthorityUntilTime;
    const float previousForceSendUntilTime = state.forceSendUntilTime;
    const float previousContactAuthorityUntilTime = state.contactAuthorityUntilTime;

    Vec3 currentPosition = Vec3(ZERO);
    if (!TryGuardedCall("live prop receive GetWorldPos", [entity]() { return entity->GetWorldPos(); }, currentPosition, &reason) ||
        !IsFiniteVec3(currentPosition))
    {
        ++m_droppedLivePropPackets;
        m_lastLivePropEvent = "dropped live prop guid=" + std::to_string(packet.guid) + " reason=invalid current position";
        return;
    }

    const bool hadSnapshot = state.hasSnapshot;
    const bool wasRemoteBallistic = state.remoteBallisticActive && now <= state.remoteBallisticUntilTime;
    const bool previousRemoteLaunchVelocityApplied = state.remoteLaunchVelocityApplied;
    const LivePropLeasePhase previousLeasePhase = state.leasePhase;

    state.entityId = entityId;
    state.guid = packet.guid;
    state.levelName = packetLevel;
    state.levelId = packet.levelId;
    state.targetPosition = packetPosition;
    state.targetRotation = packetRotation;
    state.targetScale = packetScale;
    state.velocity = packetVelocity;
    state.angularVelocity = packetAngularVelocity;
    state.releaseVelocity = Vec3(ZERO);
    state.releaseAngularVelocity = Vec3(ZERO);
    state.releaseMotionUntilTime = -1000.0f;
    state.flags = packet.flags;
    CoopSerialSequence::Observe(packet.sequence, state.lastReceivedSequence);
    state.localAuthorityUntilTime = localAuthorityActive ? previousLocalAuthorityUntilTime : -1000.0f;
    state.forceSendUntilTime = localAuthorityActive ? previousForceSendUntilTime : -1000.0f;
    state.contactAuthorityUntilTime = localAuthorityActive ? previousContactAuthorityUntilTime : -1000.0f;
    state.dirty = false;
    state.hasSnapshot = true;
    state.carried = packetCarried;
    state.remoteBallisticJustStarted = false;
    state.remoteLaunchVelocityApplied = wasRemoteBallistic ? previousRemoteLaunchVelocityApplied : false;
    // Active also extends collision ownership while a body is at rest. Only
    // an impulse or measured motion selects the ballistic physics path.
    state.activelyMoving =
        state.carried ||
        packetImpulse ||
        state.velocity.GetLengthSquared() >= kLivePropVelocityActiveSq ||
        state.angularVelocity.GetLengthSquared() >= kLivePropAngularVelocityActiveSq;
    state.activeUntilTime = now + kLivePropSettleActiveSeconds;
    const bool ballisticVelocity =
        state.velocity.GetLengthSquared() >= kLivePropBallisticVelocitySq ||
        state.angularVelocity.GetLengthSquared() >= kLivePropBallisticAngularVelocitySq;
    const bool ballisticPacket = !state.carried && state.activelyMoving && (packetImpulse || ballisticVelocity);
    const bool startBallistic =
        ballisticPacket &&
        (!hadSnapshot ||
            !wasRemoteBallistic ||
            packetImpulse ||
            (!previousRemoteLaunchVelocityApplied && ballisticVelocity));
    const bool remoteFlightPacket = !packetCarried && (packetActive || packetImpulse || ballisticVelocity || wasRemoteBallistic);
    const bool acceptHostHandoffPacket =
        m_networkMode == CoopNetworkMode::Client &&
        !packetClientAuthority &&
        localAuthorityActive &&
        previousLeasePhase == LivePropLeasePhase::FlightLocal &&
        remoteFlightPacket;
    const float remoteSuppressSeconds =
        packetCarried ? kLivePropLocalCarryAuthorityGraceSeconds :
        remoteFlightPacket ?
            (m_networkMode == CoopNetworkMode::Host ? kLivePropHostTakeoverSuppressSeconds : kLivePropReleaseFlightAuthoritySeconds) :
            kLivePropRemoteAuthoritySuppressSeconds;
    state.remoteAuthorityUntilTime = now + remoteSuppressSeconds;
    state.leasePhase =
        packetCarried ? LivePropLeasePhase::CarriedRemote :
        remoteFlightPacket ? LivePropLeasePhase::FlightRemote :
        LivePropLeasePhase::Settling;
    if (acceptHostHandoffPacket)
    {
        state.localAuthorityUntilTime = -1000.0f;
        state.forceSendUntilTime = -1000.0f;
        state.contactAuthorityUntilTime = -1000.0f;
        state.remoteAuthorityUntilTime = now + kLivePropReleaseFlightAuthoritySeconds;
        state.leasePhase = LivePropLeasePhase::FlightRemote;
    }
    if (remoteFlightPacket)
    {
        if (startBallistic || state.remoteBlendStartTime <= 0.0f)
            state.remoteBlendStartTime = now;
        state.remoteBlendDuration = packetClientAuthority ?
            kLivePropHostClientAuthorityBlendSeconds :
            m_networkMode == CoopNetworkMode::Host ?
            kLivePropHostImpulseBlendSeconds :
            kLivePropClientAuthorityBlendSeconds;
    }
    else if (!state.activelyMoving || state.carried)
    {
        state.remoteBlendStartTime = -1000.0f;
        state.remoteBlendDuration = 0.35f;
    }
    if (ballisticPacket)
    {
        state.remoteBallisticActive = true;
        state.remoteBallisticUntilTime = now + kLivePropRemoteBallisticSeconds;
        state.remoteBallisticJustStarted = startBallistic;
        state.remoteLaunchVelocityApplied = startBallistic ? false : previousRemoteLaunchVelocityApplied;
        if (startBallistic)
            ++m_livePropBallisticStarts;
    }
    else if (!state.activelyMoving || state.carried)
    {
        state.remoteBallisticActive = false;
        state.remoteBallisticJustStarted = false;
        state.remoteLaunchVelocityApplied = false;
        state.remoteBallisticUntilTime = -1000.0f;
    }
    {
        const std::string detail =
            "seq=" + std::to_string(packet.sequence) +
            " carried=" + std::to_string(packetCarried ? 1 : 0) +
            " active=" + std::to_string(packetActive ? 1 : 0) +
            " impulse=" + std::to_string(packetImpulse ? 1 : 0) +
            " clientAuth=" + std::to_string(packetClientAuthority ? 1 : 0) +
            " ballistic=" + std::to_string(ballisticPacket ? 1 : 0) +
            " start=" + std::to_string(startBallistic ? 1 : 0) +
            " vel=" + FormatVec3Compact(packetVelocity);
        AppendLivePropDebugTrace("recv.packet", state.guid, state.entityId, &state, detail.c_str());
    }

    const bool removed = (packet.flags & CoopProtocol::kLivePropTransformFlagRemoved) != 0;
    const bool settleFromBallistic = !state.carried && !state.activelyMoving && wasRemoteBallistic;
    if (m_networkMode == CoopNetworkMode::Host &&
        packetClientAuthority &&
        !removed &&
        !state.carried &&
        state.activelyMoving)
    {
        const float hostDeltaSq = (packetPosition - currentPosition).GetLengthSquared();
        const bool ok = ApplyLivePropRemoteBallisticState(*entity, state, reason);
        if (!ok)
        {
            ++m_droppedLivePropPackets;
            m_lastLivePropEvent = "apply client-authority live prop failed guid=" + std::to_string(packet.guid) + " reason=" + reason;
            return;
        }

        state.lastAppliedTime = now;
        state.pendingRemoteApply = false;
        state.remoteApplyStepsRemaining = 0;
        state.remoteAuthorityUntilTime = -1000.0f;
        state.localAuthorityUntilTime = now + kLivePropReleaseFlightAuthoritySeconds;
        state.forceSendUntilTime = now + kLivePropClientReleaseForceSendSeconds;
        state.contactAuthorityUntilTime = -1000.0f;
        state.leasePhase = LivePropLeasePhase::FlightLocal;
        state.remoteBallisticActive = false;
        state.remoteBallisticUntilTime = -1000.0f;
        state.flags &= ~CoopProtocol::kLivePropTransformFlagClientAuthority;
        state.flags |= CoopProtocol::kLivePropTransformFlagActive;
        if (packetImpulse || ballisticVelocity)
            state.flags |= CoopProtocol::kLivePropTransformFlagImpulse;
        state.dirty = true;
        state.lastSentTime = -1000.0f;
        ++m_appliedLivePropPackets;
        ++m_livePropMovingApplies;
        m_lastLivePropEvent =
            "imported client-authority live prop as host flight guid=" + std::to_string(packet.guid) +
            " active=1 impulse=" + std::to_string(packetImpulse ? 1 : 0) +
            " deltaSq=" + std::to_string(hostDeltaSq);
        AppendLivePropDebugTrace("recv.client_auth.import_host_flight", state.guid, state.entityId, &state, m_lastLivePropEvent.c_str());
        return;
    }

    if (localAuthorityActive && !removed && !acceptHostHandoffPacket)
    {
        state.remoteBallisticJustStarted = false;
        state.remoteLaunchVelocityApplied = true;
        state.pendingRemoteApply = true;
        state.remoteApplyStepsRemaining = state.carried ? 0 : 2;
        m_lastLivePropEvent =
            "deferred live prop guid=" + std::to_string(packet.guid) +
            " reason=local authority target active=" + std::to_string(state.activelyMoving ? 1 : 0) +
            " impulse=" + std::to_string(packetImpulse ? 1 : 0);
        AppendLivePropDebugTrace("recv.defer.local_authority", state.guid, state.entityId, &state, m_lastLivePropEvent.c_str());
        return;
    }

    bool ok = true;
    if (removed)
    {
        m_livePropApplyActive = true;
        ok = TryGuardedVoidCall(
            "live prop RemoveEntity",
            [this, entity]()
            {
                const EntityId entityId = entity->GetId();
                entity->SetFlags(entity->GetFlags() & ~static_cast<uint32_t>(ENTITY_FLAG_UNREMOVABLE));
                gEnv->pEntitySystem->RemoveEntity(entityId, true);
            },
            &reason);
        m_livePropApplyActive = false;
    }
    else
    {
        if (state.carried)
        {
            state.pendingRemoteApply = false;
            state.remoteApplyStepsRemaining = 0;
            ok = ApplyLivePropRemoteState(*entity, state, true, reason);
            if (ok)
                ++m_livePropCarriedApplies;
        }
        else if (state.activelyMoving)
        {
            state.pendingRemoteApply = false;
            state.remoteApplyStepsRemaining = 0;
            ok = ApplyLivePropRemoteBallisticState(*entity, state, reason);
            if (ok)
                ++m_livePropMovingApplies;
        }
        else
        {
            state.pendingRemoteApply = true;
            state.remoteApplyStepsRemaining = settleFromBallistic ? 10 : 6;
            ok = ApplyLivePropRemoteState(*entity, state, false, reason);
            if (ok)
                ++m_livePropIdleApplies;
        }
    }

    if (!ok)
    {
        ++m_droppedLivePropPackets;
        m_lastLivePropEvent = "apply live prop failed guid=" + std::to_string(packet.guid) + " reason=" + reason;
        return;
    }

    ++m_appliedLivePropPackets;
    m_lastLivePropEvent =
        "applied live prop guid=" + std::to_string(packet.guid) +
        " carried=" + std::to_string(state.carried ? 1 : 0) +
        " active=" + std::to_string(state.activelyMoving ? 1 : 0) +
        " ballistic=" + std::to_string(state.remoteBallisticActive ? 1 : 0) +
        " impulse=" + std::to_string(packetImpulse ? 1 : 0) +
        " clientAuth=" + std::to_string(packetClientAuthority ? 1 : 0) +
        " interp=" + std::to_string(state.pendingRemoteApply ? 1 : 0);
    AppendLivePropDebugTrace("recv.apply.done", state.guid, state.entityId, &state, m_lastLivePropEvent.c_str());
}

bool ModMain::SendLivePropTransformTo(const CoopProtocol::LivePropTransformPacket& packet, uint32_t address, uint16_t port, const char* failurePrefix)
{
    bool sent = false;
    bool allSent = true;
    if (m_networkMode == CoopNetworkMode::Host && !m_remotePeers.empty())
    {
        for (const auto& entry : m_remotePeers)
        {
            const RemotePeerSession& peer = entry.second;
            if (peer.address == 0 || peer.port == 0 || !peer.sessionReady ||
                !IsKnownSameLevel(m_localLevelName, peer.levelName))
                continue;
            sent = true;
            allSent = SendPacketTo(&packet, sizeof(packet), peer.address, peer.port, failurePrefix) && allSent;
        }
    }
    else
    {
        sent = true;
        allSent = SendPacketTo(&packet, sizeof(packet), address, port, failurePrefix);
    }
    if (!sent && m_networkMode == CoopNetworkMode::Host)
    {
        m_networkTelemetry.RecordProducerSuppressed(
            static_cast<uint16_t>(CoopProtocol::PacketType::LivePropTransform),
            CoopNetworkTelemetry::ProducerSuppressionReason::Readiness);
        return true;
    }
    if (!sent || !allSent)
        return false;

    ++m_sentLivePropPackets;
    m_sentLivePropBytes += sizeof(packet);
    return true;
}

bool ModMain::SendDisconnectNoticeTo(uint32_t address, uint16_t port, uint32_t reason, const char* failurePrefix)
{
    CoopProtocol::DisconnectNoticePacket packet = {};
    packet.magic = CoopProtocol::kPacketMagic;
    packet.version = CoopProtocol::kProtocolVersion;
    packet.type = static_cast<uint16_t>(CoopProtocol::PacketType::DisconnectNotice);
    packet.sequence = CoopSerialSequence::Advance(m_controlSequence);
    packet.accountToken = GetLocalAccountToken();
    packet.worldEpoch = m_localWorldEpoch;
    packet.reason = reason;
    CopyFixedString(packet.username, sizeof(packet.username), GetLocalUsername());
    return SendPacketTo(&packet, sizeof(packet), address, port, failurePrefix);
}

void ModMain::HandleDisconnectNotice(const CoopProtocol::DisconnectNoticePacket& packet)
{
    LogCoop("disconnect notice received");
    if (m_networkMode == CoopNetworkMode::Host && packet.accountToken != 0)
    {
        RemoveRemotePeer(packet.accountToken, "peer disconnected", true);
        return;
    }
    DisconnectRemotePeer("peer disconnected");
}

void ModMain::ApplyPeerConnectionLostFreeze(const char* reason)
{
    if (m_networkMode != CoopNetworkMode::Client)
        return;

    if (m_peerConnectionLostFreezeActive)
        return;

    m_peerConnectionLostFreezeActive = true;
    m_peerConnectionLostTimeScaleHandle = -1;

    std::string guardReason;
    int handle = -1;
    const bool ok = TryGuardedCall(
        "connection lost timescale freeze",
        []()
        {
            ArkTimeScaleManager* manager = g_pGame ? g_pGame->m_pArkTimeScaleManager.get() : nullptr;
            if (!manager)
                return -1;

            const unsigned timers =
                static_cast<unsigned>(ArkTimeScaleManager::EArkTimerFlag::Game) |
                static_cast<unsigned>(ArkTimeScaleManager::EArkTimerFlag::Player);
            return manager->OverrideTimeScale(timers, 0.0f);
        },
        handle,
        &guardReason);

    if (!ok || handle < 0)
    {
        m_peerConnectionLostFreezeActive = false;
        m_lastSessionWorldEvent = "connection lost freeze failed: " + (guardReason.empty() ? std::string("no ArkTimeScaleManager") : guardReason);
        LogCoop(m_lastSessionWorldEvent);
        return;
    }

    m_peerConnectionLostTimeScaleHandle = handle;
    LogCoop(std::string("connection lost timescale freeze applied: ") + (reason && reason[0] ? reason : "timeout"));
}

void ModMain::ReleasePeerConnectionLostFreeze(const char* reason)
{
    if (!m_peerConnectionLostFreezeActive && m_peerConnectionLostTimeScaleHandle < 0)
        return;

    const int handle = m_peerConnectionLostTimeScaleHandle;
    if (handle < 0)
    {
        m_peerConnectionLostFreezeActive = false;
        return;
    }

    std::string guardReason;
    const bool ok = TryGuardedVoidCall(
        "connection lost timescale release",
        [handle]()
        {
            ArkTimeScaleManager* manager = g_pGame ? g_pGame->m_pArkTimeScaleManager.get() : nullptr;
            if (manager)
                manager->ClearTimeScaleOverride(handle);
        },
        &guardReason);

    if (!ok)
    {
        m_lastSessionWorldEvent = "connection lost freeze release failed: " + guardReason;
        LogCoop(m_lastSessionWorldEvent);
        return;
    }

    m_peerConnectionLostFreezeActive = false;
    m_peerConnectionLostTimeScaleHandle = -1;
    LogCoop(std::string("connection lost timescale freeze released: ") + (reason && reason[0] ? reason : "recovered"));
}

void ModMain::TickPeerTimeout(float frameTime)
{
    (void)frameTime;
    if (m_networkMode == CoopNetworkMode::Host)
    {
        const float now = NowSeconds();
        const float disconnectAge =
            kPeerThrottleBusyStartSeconds +
            kPeerConnectionThrottleSeconds +
            kPeerTimeoutCountdownSeconds;
        std::vector<uint64_t> timedOutPeers;
        for (const auto& entry : m_remotePeers)
        {
            if (entry.second.lastPacketTime >= 0.0f &&
                now - entry.second.lastPacketTime >= disconnectAge)
            {
                timedOutPeers.push_back(entry.first);
            }
        }
        for (uint64_t accountToken : timedOutPeers)
        {
            ++m_peerTimeoutCount;
            RemoveRemotePeer(accountToken, "client timed out", true);
        }
        return;
    }

    if (m_networkMode == CoopNetworkMode::Client &&
        !m_hasRemoteSession &&
        m_clientConnectStartTime >= 0.0f &&
        !(m_remoteHostLoadNoticeGraceUntil > NowSeconds()) &&
        NowSeconds() - m_clientConnectStartTime >= kInitialConnectTimeoutSeconds)
    {
        StopNetwork();
        m_networkStatus = "connection timed out: no response from host";
        m_sessionStatus = "connection timed out";
        QueueCoopHudFeedback("COOP HOST DID NOT RESPOND", 5.0f);
        LogCoop(m_networkStatus);
        return;
    }

    if (m_networkMode == CoopNetworkMode::Off ||
        !m_hasRemoteSession ||
        !m_sessionGameplayReady ||
        m_lastPacketTime < 0.0f)
    {
        ClearPeerTimeoutWarning("timeout inactive");
        return;
    }

    const float now = NowSeconds();
    const float age = now - m_lastPacketTime;
    if (m_networkMode == CoopNetworkMode::Client &&
        m_remoteHostLoadNoticeGraceUntil > now)
    {
        ClearPeerTimeoutWarning("host load announced");
        return;
    }
    if (m_remoteHostLoadNoticeGraceUntil >= 0.0f &&
        now >= m_remoteHostLoadNoticeGraceUntil)
    {
        m_remoteHostLoadNoticeStartTime = -1.0f;
        m_remoteHostLoadNoticeGraceUntil = -1.0f;
    }
    const bool busyWithLoadOrTransfer =
        m_saveLoadGuardActive ||
        m_arkLevelTransitionLoadActive ||
        m_waitingForPostLoadContinue ||
        m_pendingPostLoadResync ||
        m_saveTransferSnapshotPending ||
        m_saveTransferSending ||
        m_saveTransferReceiving ||
        m_playerStateTransferSending ||
        m_playerStateTransferReceiving ||
        m_areaJournalTransferSending ||
        m_areaJournalTransferReceiving ||
        m_nativeEntityLifecycleTraceEnabled ||
        m_nativeNpcSpawnTraceEnabled;
    const float throttleStartSeconds = busyWithLoadOrTransfer ? kPeerThrottleBusyStartSeconds : kPeerThrottleStartSeconds;
    if (m_peerTimeoutWarningActive)
    {
        if (age <= kPeerTimeoutResumeFreshAgeSeconds)
        {
            if (m_peerTimeoutResumeCandidateStartTime < 0.0f)
                m_peerTimeoutResumeCandidateStartTime = now;

            if (now - m_peerTimeoutResumeCandidateStartTime >= kPeerTimeoutResumeStableSeconds)
            {
                ClearPeerTimeoutWarning("connection stable");
                return;
            }
        }
        else
        {
            m_peerTimeoutResumeCandidateStartTime = -1.0f;
        }
    }
    else if (age < throttleStartSeconds)
    {
        ClearPeerTimeoutWarning("packet age recovered");
        return;
    }

    const float throttleElapsedFromAge = std::max(0.0f, age - throttleStartSeconds);
    if (!m_peerTimeoutWarningActive && throttleElapsedFromAge < kPeerConnectionThrottleSeconds)
    {
        if (!m_peerConnectionThrottleActive)
        {
            m_peerConnectionThrottleActive = true;
            m_peerConnectionThrottleStartTime = now - throttleElapsedFromAge;
            m_lastPeerThrottleHudOverlayLogTime = -1000.0f;
            m_peerConnectionThrottleReason =
                m_networkMode == CoopNetworkMode::Client ?
                    "server connection throttled" :
                    "client connection throttled";
            LogCoop(m_peerConnectionThrottleReason);
        }
        return;
    }

    if (!m_peerTimeoutWarningActive)
    {
        m_peerConnectionThrottleActive = false;
        m_lastPeerThrottleHudOverlayLogTime = -1000.0f;
        m_peerTimeoutWarningActive = true;
        m_peerTimeoutWarningStartTime = now - std::max(0.0f, throttleElapsedFromAge - kPeerConnectionThrottleSeconds);
        m_peerTimeoutResumeCandidateStartTime = -1.0f;
        m_lastPeerTimeoutHudOverlayLogTime = -1000.0f;
        m_peerTimeoutWarningReason =
            m_networkMode == CoopNetworkMode::Client ?
                "server timeout warning" :
                "client timeout warning";
        m_networkStatus = m_peerTimeoutWarningReason;
        m_sessionStatus =
            m_networkMode == CoopNetworkMode::Client ?
                "server timeout" :
                "client timeout";
        if (m_networkMode == CoopNetworkMode::Client)
            QueueCoopHudFeedback("(SERVER TIMEOUT) Trying to reconnect", 1.5f);
        LogCoop(m_peerTimeoutWarningReason);
        ApplyPeerConnectionLostFreeze(m_peerTimeoutWarningReason.c_str());
    }

    const float warningElapsed = std::max(0.0f, now - m_peerTimeoutWarningStartTime);
    if (m_networkMode == CoopNetworkMode::Client)
    {
        const int remaining = std::max(0, static_cast<int>(std::ceil(kPeerTimeoutCountdownSeconds - warningElapsed)));
        m_nullUi.ShowNotice(
            NullUiNoticeSlot::Timeout,
            NullUiLayer::PreyHud,
            "(SERVER TIMEOUT) Disconnecting in " + std::to_string(remaining),
            now,
            kPeerTimeoutNoticeDurationSeconds);

        if (!m_peerConnectionLostFreezeActive || m_peerConnectionLostTimeScaleHandle < 0)
        {
            ApplyPeerConnectionLostFreeze(m_peerTimeoutWarningReason.c_str());
        }
        else
        {
            const int handle = m_peerConnectionLostTimeScaleHandle;
            std::string guardReason;
            if (!TryGuardedVoidCall(
                    "connection lost timescale maintain",
                    [handle]()
                    {
                        ArkTimeScaleManager* manager = g_pGame ? g_pGame->m_pArkTimeScaleManager.get() : nullptr;
                        if (manager)
                            manager->UpdateTimeScaleOverride(handle, 0.0f);
                    },
                    &guardReason) &&
                !guardReason.empty())
            {
                m_lastSessionWorldEvent = "connection lost freeze maintain failed: " + guardReason;
            }
        }
    }

    if (warningElapsed < kPeerTimeoutCountdownSeconds)
        return;

    ++m_peerTimeoutCount;
    DisconnectRemotePeer(
        m_networkMode == CoopNetworkMode::Client ?
            "host timed out" :
            "client timed out");
}

void ModMain::DisconnectRemotePeer(const char* reason)
{
    const std::string status = reason && reason[0] ? reason : "peer disconnected";
    const CoopNetworkMode previousMode = m_networkMode;
    LogCoop("disconnect remote peer begin mode=" + std::string(GetNetworkModeName()) +
        " reason=" + status);
    if (previousMode == CoopNetworkMode::Host)
    {
        if (m_activeRemotePeerToken != 0 &&
            m_remotePeers.find(m_activeRemotePeerToken) != m_remotePeers.end())
        {
            const uint64_t disconnectedToken = m_activeRemotePeerToken;
            RemoveRemotePeer(disconnectedToken, status.c_str(), true);
            ClearPeerTimeoutWarning("peer disconnected");
            ResetWorldSyncControlState(status.c_str());
            ResetSaveTransferState(status.c_str());
            ResetPlayerStateTransferState(status.c_str());
            ResetAreaJournalTransferState(status.c_str());
            m_networkTickAccumulator = 0.0f;
            m_sessionTickAccumulator = kPeerDisconnectSessionTickKickSeconds;
            QueueCoopHudFeedback("COOP PLAYER DISCONNECTED", 3.0f);
            LogCoop(m_networkStatus);
            return;
        }
        ClearPeerTimeoutWarning("peer disconnected");
        LogCoop("disconnect remote peer host quarantine begin");
        SoftQuarantineNetworkRuntimeEntitiesForDisconnect(status.c_str());
        LogCoop("disconnect remote peer host quarantine done");
        ResetProxyHealthBaseline();
        ResetWorldSyncControlState(status.c_str());
        ResetSaveTransferState(status.c_str());
        ResetPlayerStateTransferState(status.c_str());
        ResetAreaJournalTransferState(status.c_str());
        ResetLivePropSyncState(status.c_str());
        LogCoop("disconnect remote peer host transfer state reset done");

        m_networkTickAccumulator = 0.0f;
        m_sessionTickAccumulator = kPeerDisconnectSessionTickKickSeconds;
        m_hasLastLocalPlayerPos = false;
        m_hasRemoteEndpoint = false;
        m_hasRemoteSession = false;
        m_sessionGameplayReady = false;
        m_remoteAddress = 0;
        m_remotePort = 0;
        m_remoteLevelName.clear();
        m_remoteLevelId = 0;
        m_remoteModBuild = 0;
        m_remoteWorldEpoch = 0;
        m_remoteSessionFlags = 0;
        m_lastRemoteUsername.clear();
        m_remoteAccountToken = 0;
        m_remotePlayerModelArchetypeId = 10739735956144685671ull;
        m_lastRuntimeCleanupRemoteUsername.clear();
        m_reliableSendQueue.clear();
        m_reliableEndpointStates.clear();
        m_remotePeers.clear();
        m_activeRemotePeerToken = 0;
        m_primaryRemotePeerToken = 0;
        m_activePacketSourceAccountToken = 0;
        m_reliableSendSequence = 0;
        m_reliableRecvSequence = 0;
        m_reliableAckedSequence = 0;
        m_reliableBacklogDisconnectPending = false;
        m_debugPauseReliableSends = false;
        m_debugDropReliableAcks = false;
        m_debugReliableMaxAgeSeconds = 0.0f;
        m_debugReliableMaxSendAttempts = 0;
        m_lastReliableEvent = status;
        m_remotePlayerDowned = false;
        m_remoteReviveHoldSeconds = 0.0f;
        m_remoteReviveHoldProgress = 0.0f;
        m_remoteReviveDistance = 0.0f;
        m_remoteRevivePromptActive = false;
        m_pendingReviveRemote = false;
        m_pendingUnstuckToRemote = false;
        m_clientAreaAuthorityActive = false;
        m_pendingRemoteAreaHandoffRequestLevel.clear();
        m_lastSessionWorldEvent = status;
        m_networkStatus = status + "; host listening on UDP " + std::to_string(m_networkPort);
        m_sessionStatus = "waiting for client session";
        QueueCoopHudFeedback("COOP PLAYER DISCONNECTED", 3.0f);
        LogCoop(m_networkStatus);
        LogCoop("disconnect remote peer host complete");
        return;
    }

    StopNetwork();
    if (previousMode == CoopNetworkMode::Client)
        m_multiplayerRestartRequired = false;
    ClearPeerTimeoutWarning("peer disconnected");
    m_networkStatus = status;
    m_sessionStatus = "disconnected";
    m_lastSessionWorldEvent = status;
    QueueCoopHudFeedback("DISCONNECTED - " + status, 3.0f);
    LogCoop(status);

    if (previousMode == CoopNetworkMode::Client)
        ReturnClientToMultiplayerFlowAfterDisconnect(status.c_str());
    LogCoop("disconnect remote peer client/off complete");
}

void ModMain::ClearPeerTimeoutWarning(const char* reason)
{
    const bool wasThrottleActive = m_peerConnectionThrottleActive;
    const bool wasTimeoutActive = m_peerTimeoutWarningActive;
    if (!wasThrottleActive && !wasTimeoutActive)
        return;

    m_peerConnectionThrottleActive = false;
    m_peerTimeoutWarningActive = false;
    m_peerConnectionThrottleStartTime = -1.0f;
    m_peerTimeoutWarningStartTime = -1.0f;
    m_peerTimeoutResumeCandidateStartTime = -1.0f;
    m_lastPeerThrottleHudOverlayLogTime = -1000.0f;
    m_lastPeerTimeoutHudOverlayLogTime = -1000.0f;
    m_peerConnectionThrottleReason = reason && reason[0] ? reason : "connection throttle cleared";
    m_peerTimeoutWarningReason = reason && reason[0] ? reason : "timeout cleared";
    ReleasePeerConnectionLostFreeze(m_peerTimeoutWarningReason.c_str());
    m_nullUi.ClearNotice(NullUiNoticeSlot::Timeout);
    if (wasTimeoutActive && m_networkMode == CoopNetworkMode::Client)
    {
        m_networkStatus = "connection resumed";
        QueueCoopHudFeedback("CONNECTION RESUMED", 1.5f);
    }
    if (wasTimeoutActive)
        LogCoop("peer timeout warning cleared: " + m_peerTimeoutWarningReason);
}

void ModMain::ReturnClientToMultiplayerFlowAfterDisconnect(const char* reason)
{
    if (EnvFlagEnabled("COOP_DISABLE_DISCONNECT_MAIN_MENU"))
        return;

    const char* modeEnv = std::getenv("COOP_DISCONNECT_MAIN_MENU_MODE");
    const std::string mode = modeEnv && modeEnv[0] ? ToLowerAscii(modeEnv) : std::string("browser");
    if (mode == "0" || mode == "none" || mode == "off")
    {
        m_lastSessionWorldEvent = "main menu command skipped by mode";
        LogCoop(m_lastSessionWorldEvent);
        return;
    }

    const char* commandEnv = std::getenv("COOP_DISCONNECT_MAIN_MENU_COMMAND");
    const std::string command = commandEnv && commandEnv[0] ? std::string(commandEnv) : std::string();
    if (mode == "console" || !command.empty())
    {
        if (command.empty() || command == "0")
            return;

        if (!gEnv || !gEnv->pConsole)
        {
            m_lastSessionWorldEvent = "main menu command skipped: no console";
            LogCoop(m_lastSessionWorldEvent);
            return;
        }

        std::string guardReason;
        if (!TryGuardedVoidCall(
                "coop disconnect main menu command",
                [&command]()
                {
                    gEnv->pConsole->ExecuteString(command.c_str(), false, true);
                },
                &guardReason))
        {
            m_lastSessionWorldEvent =
                "main menu command failed: " +
                (guardReason.empty() ? std::string("unknown") : guardReason);
            LogCoop(m_lastSessionWorldEvent);
            return;
        }

        m_lastSessionWorldEvent =
            "queued main menu command after disconnect reason=" +
            std::string(reason && reason[0] ? reason : "unknown") +
            " command=" + command;
        LogCoop(m_lastSessionWorldEvent);
        return;
    }

    // Keep the loaded single-player world intact and return to the production
    // browser. Prey's native game-session teardown is unsafe after a received
    // Host save has created runtime proxies, while this path lets the player
    // reconnect, pick another server, or close the overlay and continue solo.
    const bool gameReady = gEnv && gEnv->pEntitySystem &&
        ArkPlayer::GetInstancePtr() && ArkPlayer::GetInstance().GetEntity();
    if (gameReady)
        OpenMultiplayerFromNativePauseMenu();
    else
        OpenMultiplayerFromNativeMainMenu();
    m_lastSessionWorldEvent =
        "returned to multiplayer browser after disconnect reason=" +
        std::string(reason && reason[0] ? reason : "unknown");
    LogCoop(m_lastSessionWorldEvent);
}
