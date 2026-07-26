#include "ModMain.h"
#include "CoopEnemyIntentGate.h"
#include "CoopEnemyMovementGate.h"
#include "CoopRuntimeConfig.h"
#include "CoopRuntimeLog.h"
#include "CoopSerialSequence.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <windows.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "CoopRuntimeGuards.h"
#include <EntityUtils.h>
#include <Chairloader/IChairLogger.h>
#include <Prey/ArkEnums.h>
#include <Prey/CryAction/IGameObject.h>
#include <Prey/CryEntitySystem/IEntitySystem.h>
#include <Prey/CryGame/Game.h>
#include <Prey/CryGame/IGameFramework.h>
#include <Prey/CryPhysics/physinterface.h>
#include <Prey/CryScriptSystem/IScriptSystem.h>
#include <Prey/CryAISystem/MovementRequestResult.h>
#include <Prey/GameDll/ark/ArkHealthExtension.h>
#include <Prey/GameDll/ark/ArkFactionManager.h>
#include <Prey/GameDll/ark/ArkOperatorLaserHelper.h>
#include <Prey/GameDll/ark/arkglooeffectutils.h>
#include <Prey/GameDll/ark/arkprojectilegoophysicsmanager.h>
#include <Prey/GameDll/ark/attention/ArkAttentionManager.h>
#include <Prey/GameDll/ark/cystoid/ArkCystoid.h>
#include <Prey/GameDll/ark/cystoid/ArkCystoidManager.h>
#include <Prey/GameDll/ark/cystoid/ArkCystoidNest.h>
#include <Prey/GameDll/ark/npc/ArkNpc.h>
#include <Prey/GameDll/ark/npc/desires/ArkNpcFacingDesireManager.h>
#include <Prey/GameDll/ark/npc/desires/ArkNpcLookDesireManager.h>
#include <Prey/GameDll/ark/npc/desires/ArkNpcMovementDesireManager.h>
#include <Prey/GameDll/ark/player/ArkPlayer.h>
#include <Prey/GameDll/ark/player/ArkPlayerAwarenessComponent.h>
#include <Prey/GameDll/ark/player/ArkPlayerUIComponent.h>
#include <Prey/GameDll/ark/player/psipower/ArkPsiPowerCreatePhantom.h>
#include <Prey/GameDll/ark/player/psipower/ArkPsiPowerComponent.h>

using CoopRuntimeGuards::TryGuardedVoidCall;
using CoopRuntimeGuards::TryGuardedCall;
using CoopRuntimeGuards::TryWriteRuntimeValue;

namespace
{
static inline auto s_allocateNativeNpcAnimAction =
    PreyFunction<void*(size_t)>(0x1B6D740);
static inline auto s_constructNativeNpcAnimAction =
    PreyFunction<void*(void*, void*, int, int, void*, int, int, int)>(0x11E2E30);

constexpr size_t kNativeNpcAnimActionSize = 0xB8;
constexpr size_t kNativeIActionStatusOffset = 0x28;
constexpr size_t kNativeIActionFlagsOffset = 0x2C;
constexpr size_t kNativeIActionOptionIndexOffset = 0x48;
constexpr size_t kNativeIActionReferenceCountOffset = 0x50;
// IMannequin has a virtual destructor before UnloadAll/ReloadAll/GetDatabase/
// CreateActionController; FindActionController is therefore MSVC vtable slot 5.
constexpr size_t kMannequinFindActionControllerVtableIndex = 5;
constexpr size_t kActionControllerQueueVtableIndex = 18;
constexpr size_t kNativeIActionDoDeleteVtableIndex = 26;
// PreyDll 0x1805D8DB1..0x1805D8DDE proves this exact native chain:
// IEntity::GetCharacter(0), ICharacterInstance vtable +0x28, then
// ISkeletonAnim vtable +0xC0 for SetDesiredMotionParam(id, value, frameTime).
// Keep this tiny ABI bridge here instead of importing a mismatched public
// CryAnimation header; Chairloader intentionally only exposes the character
// instance as an opaque type.
constexpr size_t kCharacterGetSkeletonAnimVtableIndex = 5;
constexpr size_t kSkeletonSetDesiredMotionParamVtableIndex = 24;
constexpr int kMotionParamTravelSpeed = 0;
constexpr int kMotionParamTravelAngle = 2;
constexpr uint32_t kNativeIActionBlendOutFlag = 1u << 0;
constexpr uint32_t kNativeIActionInterruptableFlag = 1u << 2;
constexpr uint32_t kNativeIActionStoppingFlag = 1u << 13;
constexpr int kNativeIActionStatusPending = 1;
constexpr int kNativeIActionStatusFinished = 4;

void StopAndReleaseRemoteNativeAction(void* action) noexcept
{
    if (!action ||
        !CoopRuntimeGuards::IsReadableRuntimePointer(
            action,
            kNativeIActionReferenceCountOffset + sizeof(int)))
    {
        return;
    }

    // ArkNpcAnimAction does not create slave actions. This is therefore the
    // complete native IAction::Stop transition for the exact action we made:
    // the controller owns the actual blend/removal on its next update.
    auto* actionBytes = reinterpret_cast<std::byte*>(action);
    auto* status = reinterpret_cast<int*>(actionBytes + kNativeIActionStatusOffset);
    auto* flags = reinterpret_cast<uint32_t*>(actionBytes + kNativeIActionFlagsOffset);
    if (*status == kNativeIActionStatusPending)
    {
        // A not-yet-installed predecessor cannot blend out. ForceFinish is
        // the native queue-cancellation state transition in this one case.
        *status = kNativeIActionStatusFinished;
        *flags &= ~kNativeIActionInterruptableFlag;
    }
    else
    {
        *flags |= kNativeIActionBlendOutFlag | kNativeIActionStoppingFlag;
    }

    auto* referenceCount = reinterpret_cast<int*>(
        actionBytes + kNativeIActionReferenceCountOffset);
    --(*referenceCount);
    if (*referenceCount > 0)
        return;

    void** actionVtable = *reinterpret_cast<void***>(action);
    if (!CoopRuntimeGuards::IsReadableRuntimePointer(
            actionVtable,
            (kNativeIActionDoDeleteVtableIndex + 1) * sizeof(void*)) ||
        !CoopRuntimeGuards::IsExecutableRuntimePointer(
            actionVtable[kNativeIActionDoDeleteVtableIndex]))
    {
        return;
    }

    using DeleteActionFn = void (*)(void*);
    reinterpret_cast<DeleteActionFn>(
        actionVtable[kNativeIActionDoDeleteVtableIndex])(action);
}

constexpr uint64_t kProxyArchetype = 10739735956144685611ULL;
constexpr uint64_t kMimicArchetype = 718;
constexpr uint64_t kCystoidArchetype = 719;
constexpr uint64_t kTestMimicNetId = 1;
constexpr float kProxyNetTickSeconds = 0.05f;
constexpr float kMimicStateTickSeconds = 0.05f;
constexpr float kEnemyMimicryHeartbeatSeconds = 4.0f;
constexpr float kEnemyStateIdleHeartbeatSeconds = 1.0f;
constexpr float kEnemyStateMidHeartbeatSeconds = 2.0f;
constexpr float kEnemyStateFarHeartbeatSeconds = 4.0f;
constexpr float kEnemyStateIdleChangeIntervalSeconds = 0.20f;
constexpr float kEnemyStateActiveChangeIntervalSeconds = 0.10f;
constexpr float kEnemyStateMidInterestDistanceSq = 40.0f * 40.0f;
constexpr float kEnemyStateFarInterestDistanceSq = 75.0f * 75.0f;
constexpr float kEnemyStatePositionThreshold = 0.015f;
constexpr float kEnemyStateRotationDotThreshold = 0.9999619f;
constexpr float kEnemyStateDirectionDotThreshold = 0.9990f;
constexpr float kEnemyStateScalarThreshold = 0.01f;
constexpr float kEnemyHealthReconcileThreshold = 0.01f;
constexpr float kProxyForwardOffsetMeters = 2.0f;
constexpr float kEnemyMimicryTargetResolveDistanceSq = 4.0f * 4.0f;

bool IsFiniteMimicryTargetPosition(const Vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

IEntity* ResolveNpcMimicryTarget(
    const CoopProtocol::EnemyAbilityFxEventPacket& packet,
    const IEntity& enemyEntity,
    std::string& route,
    std::string& guardReason)
{
    route = "missing";
    if (!gEnv || !gEnv->pEntitySystem)
        return nullptr;

    const uint64_t targetGuid = packet.controllingTechnopathStableKey;
    if (targetGuid != 0)
    {
        EntityId targetId = INVALID_ENTITYID;
        if (TryGuardedCall(
                "remote npc mimicry FindEntityByGuid",
                [targetGuid]()
                {
                    return gEnv->pEntitySystem->FindEntityByGuid(static_cast<EntityGUID>(targetGuid));
                },
                targetId,
                &guardReason) &&
            targetId != INVALID_ENTITYID)
        {
            IEntity* targetEntity = gEnv->pEntitySystem->GetEntity(targetId);
            if (targetEntity)
            {
                route = "guid";
                return targetEntity;
            }
        }
    }

    const uint64_t targetArchetypeId = packet.abilityTargetArchetypeId;
    const Vec3 targetPosition(packet.px, packet.py, packet.pz);
    if (targetArchetypeId == 0 || !IsFiniteMimicryTargetPosition(targetPosition))
        return nullptr;

    IEntityIt* iterator = gEnv->pEntitySystem->GetEntityIterator();
    if (!iterator)
        return nullptr;

    IEntity* best = nullptr;
    float bestDistanceSq = kEnemyMimicryTargetResolveDistanceSq;
    iterator->MoveFirst();
    while (!iterator->IsEnd())
    {
        IEntity* candidate = iterator->Next();
        if (!candidate || candidate->GetId() == enemyEntity.GetId())
            continue;

        const IEntityArchetype* archetype = nullptr;
        uint64_t archetypeId = 0;
        Vec3 candidatePosition(ZERO);
        if (!TryGuardedCall(
                "remote npc mimicry target GetArchetype",
                [candidate]() { return candidate->GetArchetype(); },
                archetype,
                &guardReason) ||
            !archetype ||
            !TryGuardedCall(
                "remote npc mimicry target archetype GetId",
                [archetype]() { return archetype->GetId(); },
                archetypeId,
                &guardReason) ||
            archetypeId != targetArchetypeId ||
            !TryGuardedCall(
                "remote npc mimicry target GetWorldPos",
                [candidate]() { return candidate->GetWorldPos(); },
                candidatePosition,
                &guardReason) ||
            !IsFiniteMimicryTargetPosition(candidatePosition))
        {
            continue;
        }

        const float distanceSq = (candidatePosition - targetPosition).GetLengthSquared();
        if (distanceSq <= bestDistanceSq)
        {
            best = candidate;
            bestDistanceSq = distanceSq;
        }
    }
    iterator->Release();

    if (best)
        route = "archetype_position";
    return best;
}

uint64_t BuildCorpsePhantomStableId(uint64_t sourceStableEnemyId, uint64_t phantomArchetypeId)
{
    if (sourceStableEnemyId == 0 || phantomArchetypeId == 0)
        return 0;

    uint64_t childStableId = 14695981039346656037ull;
    const auto mix = [&childStableId](uint64_t value)
    {
        for (int byte = 0; byte < 8; ++byte)
        {
            childStableId ^= (value >> (byte * 8)) & 0xffu;
            childStableId *= 1099511628211ull;
        }
    };
    mix(0x434f525053455048ull); // "CORPSEPH"
    mix(sourceStableEnemyId);
    mix(phantomArchetypeId);
    return childStableId == 0 ? 1 : childStableId;
}

uint64_t BuildEthericDoppelgangerStableId(uint64_t sourceStableEnemyId, uint32_t castGeneration)
{
    if (sourceStableEnemyId == 0 || castGeneration == 0)
        return 0;

    uint64_t childStableId = 14695981039346656037ull;
    const auto mix = [&childStableId](uint64_t value)
    {
        for (int byte = 0; byte < 8; ++byte)
        {
            childStableId ^= (value >> (byte * 8)) & 0xffu;
            childStableId *= 1099511628211ull;
        }
    };
    mix(0x4554484552444f50ull); // "ETHERDOP"
    mix(sourceStableEnemyId);
    mix(castGeneration);
    return childStableId == 0 ? 1 : childStableId;
}
constexpr float kMimicForwardOffsetMeters = 5.0f;
constexpr float kMimicPuppetSoftSnapDistance = 0.35f;
constexpr float kMimicPuppetHardSnapDistance = 3.0f;
constexpr float kEnemyLocomotionBindDistanceMeters = 8.0f;
constexpr float kEnemyLocomotionSoftSnapDistance = 0.45f;
constexpr float kEnemyLocomotionHardSnapDistance = 4.0f;
constexpr float kEnemyRemoteDashHardSnapDistance = 0.75f;
constexpr float kEnemyRemoteMoveSoftCorrectionDistance = 0.65f;
constexpr float kEnemyRemoteMoveMaxSoftCorrectionPerTick = 0.85f;
constexpr float kEnemyRemoteSmoothTinySnapDistance = 0.015f;
constexpr float kEnemyRemoteSmoothHardJumpDistance = 2.5f;
constexpr float kEnemyRemoteSmoothBurstHardJumpDistance = 0.75f;
constexpr float kEnemyRemoteSmoothAssumedFrameRate = 60.0f;
constexpr float kEnemyRemoteSmoothHardJumpMinFrames = 30.0f;
// Phantom Shift is authored as a short, sharp displacement. Ordinary remote
// correction deliberately takes about half a second, but applying that same
// envelope to Shift turns it into a fast walk. Preserve a few render frames
// for continuity while converging the burst in about 0.2 seconds.
constexpr float kEnemyRemoteSmoothBurstHardJumpMinFrames = 12.0f;
constexpr float kEnemyRemoteBurstTransformSmoothingSeconds =
    kEnemyRemoteSmoothBurstHardJumpMinFrames / kEnemyRemoteSmoothAssumedFrameRate;
constexpr float kEnemyRemoteSmoothMinCorrectionSpeed = 0.35f;
constexpr float kEnemyRemoteSmoothCorrectionSpeedPerMeter = 6.0f;
constexpr float kEnemyRemoteSmoothCatchupCorrectionSpeedPerMeter = 6.0f;
constexpr float kEnemyRemoteSmoothMaxCorrectionSpeed = 6.5f;
constexpr float kEnemyRemoteSmoothBurstCorrectionSpeedPerMeter = 8.0f;
// Base Phantom Shift is authored at 44 m/s in ArkNpcs.xml. The receiver still
// respects the bounded 12-frame envelope below, but its ceiling must not turn
// an authored Shift into a slower glide.
constexpr float kEnemyRemoteSmoothBurstMaxCorrectionSpeed = 44.0f;
constexpr float kEnemyRemoteSmoothMinRotationSpeed = 2.0f;
constexpr float kEnemyRemoteSmoothRotationSpeedPerRadian = 10.0f;
constexpr float kEnemyRemoteSmoothMaxRotationSpeed = 30.0f;
constexpr float kEnemyRemoteSmoothBurstRotationSpeedPerRadian = 8.0f;
constexpr float kEnemyRemoteSmoothBurstMaxRotationSpeed = 18.0f;
// Keep roughly two active network samples buffered. One 100 ms sample was
// smooth between arrivals but still exposed short receive-jitter holds; the
// extra latency is preferable to a visible stop/start staircase.
constexpr float kEnemyRemoteInterpolationDelayScale = 2.0f;
constexpr float kEnemyRemoteInterpolationMinDelaySeconds = 0.20f;
constexpr float kEnemyRemoteInterpolationMaxDelaySeconds = 0.45f;
constexpr float kEnemyRemoteInterpolationMinSampleSeconds = 0.02f;
constexpr float kEnemyRemoteInterpolationMaxSampleSeconds = 0.50f;
constexpr float kEnemyReadOnlyFacingMixResponse = 10.0f;
constexpr float kEnemyRemotePacketSmoothApplyMinDistance = 0.18f;
constexpr float kEnemyRemotePacketMotionApplyMinDistance = 0.035f;
constexpr float kEnemyRemoteCatchupIntentDistance = 0.24f;
constexpr float kEnemyRemoteMovementIntentHoldSeconds = 0.40f;
// Keep observers on the replicated authority point. Prediction made Weaver
// passive/focus lanes chase stale targets even after action-motion blocking.
constexpr float kEnemyRemoteTargetPredictionSeconds = 0.0f;
constexpr float kEnemyRemoteTargetPredictionMaxDistance = 0.0f;
constexpr float kEnemyRemoteInferredTargetPredictionSeconds = 0.0f;
constexpr float kEnemyRemoteInferredTargetPredictionMaxDistance = 0.0f;
constexpr float kEnemyRemotePassiveCarryTargetPredictionSeconds = 0.0f;
constexpr float kEnemyRemotePassiveCarryTargetPredictionMaxDistance = 0.0f;
constexpr float kEnemyRemotePassiveCarryMaxTravelSpeed = 3.6f;
// Passive authority movement still chases the replicated point, not a predicted
// lead. Keep the cap high enough that accumulated packet lag can be recovered
// smoothly instead of leaving the observer permanently behind the authority.
constexpr float kEnemyRemotePassiveCarryMaxCorrectionSpeed = 6.5f;
constexpr float kEnemyRemoteTargetMotionInferenceMinStep = 0.050f;
constexpr float kEnemyRemoteTargetMotionInferenceMinSpeed = 0.85f;
constexpr float kEnemyRemoteVisualMoveStepThreshold = 0.018f;
constexpr float kEnemyRemoteVisualMoveHoldSeconds = 0.10f;
constexpr float kEnemyRemoteLegBlendStartSpeed = 0.12f;
constexpr float kEnemyRemoteLegBlendStopSpeed = 0.06f;
constexpr float kEnemyRemoteLegBlendMaxSpeed = 6.0f;
constexpr float kEnemyRemoteLegBlendRiseResponse = 14.0f;
constexpr float kEnemyRemoteLegBlendFallResponse = 8.0f;
constexpr float kEnemyRemoteVisualTargetStepMin = 0.045f;
constexpr float kEnemyRemoteVisualTargetStepMax = 0.50f;
constexpr float kEnemyRemoteReceiverMinWalkSpeed = 1.0f;
constexpr float kEnemyRemoteReceiverMinRunSpeed = 3.0f;
constexpr float kEnemyRemoteReceiverInferRunSpeed = 5.7f;
constexpr float kEnemyRemoteReceiverMinDashSpeed = 6.0f;
constexpr float kEnemyRemotePassiveInferredMaxWalkSpeed = 1.8f;
constexpr float kEnemyRemoteReceiverMaxWalkSpeed = 6.0f;
constexpr float kEnemyRemoteReceiverMaxRunSpeed = 8.0f;
constexpr float kEnemyRemoteReceiverMaxDashSpeed = 44.0f;
constexpr float kEnemyRemoteActionAnchorSnapDistance = 0.10f;
constexpr float kEnemyIdleAnchorSnapDistance = 0.08f;
constexpr float kEnemyLocalRotationOverrideGraceSeconds = 0.35f;
constexpr float kEnemyRemoteAuthorityTimeoutSeconds = 3.5f;
constexpr float kEnemyNativeAttentionEdgeGraceSeconds = 1.5f;

bool EnemyStatePacketMateriallyDiffers(
    const CoopProtocol::TestMimicStatePacket& current,
    const CoopProtocol::TestMimicStatePacket& previous)
{
    if (current.archetypeId != previous.archetypeId ||
        current.enemyNetId != previous.enemyNetId ||
        current.entityGuid != previous.entityGuid ||
        current.targetAccountToken != previous.targetAccountToken ||
        current.authorityOwnerAccountToken != previous.authorityOwnerAccountToken ||
        current.authorityEpoch != previous.authorityEpoch ||
        current.authorityAttentionLevel != previous.authorityAttentionLevel ||
        current.commitSequence != previous.commitSequence ||
        current.sourceFlags != previous.sourceFlags ||
        current.locomotionFlags != previous.locomotionFlags ||
        current.locomotionLevel != previous.locomotionLevel ||
        current.attackKind != previous.attackKind ||
        current.mannequinFragmentId != previous.mannequinFragmentId ||
        current.mannequinSequence != previous.mannequinSequence ||
        current.mannequinOrdinal != previous.mannequinOrdinal ||
        current.mannequinReserved != previous.mannequinReserved ||
        current.mannequinPriority != previous.mannequinPriority ||
        current.mannequinTagStateValid != previous.mannequinTagStateValid ||
        !std::equal(
            std::begin(current.mannequinTagState),
            std::end(current.mannequinTagState),
            std::begin(previous.mannequinTagState)) ||
        current.semanticContextId != previous.semanticContextId ||
        current.semanticSequence != previous.semanticSequence ||
        current.semanticVariant != previous.semanticVariant ||
        current.semanticReserved[0] != previous.semanticReserved[0] ||
        current.flags != previous.flags)
    {
        return true;
    }

    const Vec3 currentPosition(current.px, current.py, current.pz);
    const Vec3 previousPosition(previous.px, previous.py, previous.pz);
    if ((currentPosition - previousPosition).GetLengthSquared() >
        kEnemyStatePositionThreshold * kEnemyStatePositionThreshold)
    {
        return true;
    }

    const Quat currentRotation(current.qw, current.qx, current.qy, current.qz);
    const Quat previousRotation(previous.qw, previous.qx, previous.qy, previous.qz);
    if (std::abs(currentRotation | previousRotation) < kEnemyStateRotationDotThreshold)
        return true;

    const Vec3 currentDirection(current.mx, current.my, current.mz);
    const Vec3 previousDirection(previous.mx, previous.my, previous.mz);
    if (currentDirection.GetLengthSquared() > 0.0001f && previousDirection.GetLengthSquared() > 0.0001f &&
        currentDirection.GetNormalizedSafe(Vec3(0.0f, 1.0f, 0.0f)).Dot(
            previousDirection.GetNormalizedSafe(Vec3(0.0f, 1.0f, 0.0f))) < kEnemyStateDirectionDotThreshold)
    {
        return true;
    }

    const auto scalarChanged = [](float a, float b, float epsilon) {
        return !std::isfinite(a) || !std::isfinite(b) || std::abs(a - b) > epsilon;
    };
    return scalarChanged(current.vx, previous.vx, kEnemyStateScalarThreshold) ||
        scalarChanged(current.vy, previous.vy, kEnemyStateScalarThreshold) ||
        scalarChanged(current.vz, previous.vz, kEnemyStateScalarThreshold) ||
        scalarChanged(current.health, previous.health, kEnemyStateScalarThreshold) ||
        scalarChanged(current.maxHealth, previous.maxHealth, kEnemyStateScalarThreshold) ||
        scalarChanged(current.lastDamage, previous.lastDamage, kEnemyStateScalarThreshold) ||
        scalarChanged(current.speed, previous.speed, 0.05f);
}

float EnemyStateMinimumTransmitInterval(
    const CoopProtocol::TestMimicStatePacket& packet,
    float interestDistanceSq)
{
    const uint32_t immediateFlags =
        CoopProtocol::kEnemyLocomotionFlagDashing |
        CoopProtocol::kEnemyLocomotionFlagShifting |
        CoopProtocol::kEnemyLocomotionFlagMorphing |
        CoopProtocol::kEnemyLocomotionFlagLunging |
        CoopProtocol::kEnemyLocomotionFlagAttacking |
        CoopProtocol::kEnemyLocomotionFlagHitReacting |
        CoopProtocol::kEnemyLocomotionFlagStunned |
        CoopProtocol::kEnemyLocomotionFlagGlooed |
        CoopProtocol::kEnemyLocomotionFlagRagdolled;
    if ((packet.locomotionFlags & immediateFlags) != 0 ||
        (packet.flags & (CoopProtocol::kTestMimicStateFlagHitCommit |
            CoopProtocol::kTestMimicStateFlagDeathCommit)) != 0)
    {
        return kMimicStateTickSeconds;
    }

    const uint32_t activeFlags =
        CoopProtocol::kEnemyLocomotionFlagWalking |
        CoopProtocol::kEnemyLocomotionFlagRunning |
        CoopProtocol::kEnemyLocomotionFlagTurning |
        CoopProtocol::kEnemyLocomotionFlagInCombat;
    if ((packet.locomotionFlags & activeFlags) != 0 ||
        (packet.sourceFlags & CoopProtocol::kEnemyStateSourceFlagAuthorityHasAttention) != 0)
    {
        if (interestDistanceSq > kEnemyStateFarInterestDistanceSq)
            return 0.35f;
        if (interestDistanceSq > kEnemyStateMidInterestDistanceSq)
            return 0.20f;
        return kEnemyStateActiveChangeIntervalSeconds;
    }
    if (interestDistanceSq > kEnemyStateFarInterestDistanceSq)
        return 0.75f;
    if (interestDistanceSq > kEnemyStateMidInterestDistanceSq)
        return 0.40f;
    return kEnemyStateIdleChangeIntervalSeconds;
}

float EnemyStateHeartbeatSeconds(float interestDistanceSq)
{
    if (interestDistanceSq > kEnemyStateFarInterestDistanceSq)
        return kEnemyStateFarHeartbeatSeconds;
    if (interestDistanceSq > kEnemyStateMidInterestDistanceSq)
        return kEnemyStateMidHeartbeatSeconds;
    return kEnemyStateIdleHeartbeatSeconds;
}
constexpr float kEnemyRemoteAuthorityBlockedStealSeconds = 10.0f;
// Attention ranks are reliable state, not locomotion. A one-second liveness
// refresh keeps failover prompt without putting every visible NPC on the
// high-rate reliable lane.
constexpr float kEnemyAttentionCandidateHeartbeatSeconds = 1.0f;
constexpr float kEnemyMovementIntentSpeedThreshold = 0.75f;
constexpr int kEnemyIdleTurnFragmentId = 2;
constexpr uint32_t kMimicDeathCommitRepeatCount = 30;

ArkCystoid* GetArkCystoidExtensionFromEntity(IEntity* entity)
{
    if (!entity || !gEnv || !gEnv->pGame)
        return nullptr;

    IGameFramework* framework = gEnv->pGame->GetIGameFramework();
    if (!framework)
        return nullptr;

    IGameObject* gameObject = nullptr;
    std::string reason;
    if (!TryGuardedCall(
            "cystoid extension GetGameObject",
            [framework, entity]() { return framework->GetGameObject(entity->GetId()); },
            gameObject,
            &reason) ||
        !gameObject)
    {
        return nullptr;
    }

    auto queryCystoidExtension = [&](const char* extensionName) -> ArkCystoid*
    {
        if (!extensionName || !extensionName[0])
            return nullptr;

        IGameObjectExtension* extension = nullptr;
        IGameObjectSystem::ExtensionID extensionId = 0;
        if (TryGuardedCall(
                "cystoid extension GetExtensionId",
                [gameObject, extensionName]() { return gameObject->GetExtensionId(extensionName); },
                extensionId,
                &reason) &&
            extensionId != 0 &&
            TryGuardedCall(
                "cystoid extension QueryExtension id",
                [gameObject, extensionId]() { return gameObject->QueryExtension(extensionId); },
                extension,
                &reason) &&
            extension)
        {
            return static_cast<ArkCystoid*>(extension);
        }

        extension = nullptr;
        if (TryGuardedCall(
                "cystoid extension QueryExtension name",
                [gameObject, extensionName]() { return gameObject->QueryExtension(extensionName); },
                extension,
                &reason) &&
            extension)
        {
            return static_cast<ArkCystoid*>(extension);
        }
        return nullptr;
    };

    if (ArkCystoid* extension = queryCystoidExtension("ArkAlienJelly"))
    {
        return extension;
    }

    if (ArkCystoid* extension = queryCystoidExtension("ArkCystoid"))
    {
        return extension;
    }

    return nullptr;
}

ArkCystoidNest* GetArkCystoidNestExtensionFromEntity(IEntity* entity)
{
    if (!entity || !gEnv || !gEnv->pGame)
        return nullptr;

    IGameFramework* framework = gEnv->pGame->GetIGameFramework();
    if (!framework)
        return nullptr;

    IGameObject* gameObject = nullptr;
    std::string reason;
    if (!TryGuardedCall(
            "cystoid nest extension GetGameObject",
            [framework, entity]() { return framework->GetGameObject(entity->GetId()); },
            gameObject,
            &reason) ||
        !gameObject)
    {
        return nullptr;
    }

    IGameObjectExtension* extension = nullptr;
    IGameObjectSystem::ExtensionID extensionId = 0;
    if (TryGuardedCall(
            "cystoid nest extension GetExtensionId",
            [gameObject]() { return gameObject->GetExtensionId("ArkCystoidNest"); },
            extensionId,
            &reason) &&
        extensionId != 0 &&
        TryGuardedCall(
            "cystoid nest extension QueryExtension id",
            [gameObject, extensionId]() { return gameObject->QueryExtension(extensionId); },
            extension,
            &reason) &&
        extension)
    {
        return static_cast<ArkCystoidNest*>(extension);
    }

    extension = nullptr;
    if (TryGuardedCall(
            "cystoid nest extension QueryExtension name",
            [gameObject]() { return gameObject->QueryExtension("ArkCystoidNest"); },
            extension,
            &reason) &&
        extension)
    {
        return static_cast<ArkCystoidNest*>(extension);
    }

    return nullptr;
}

bool RequestCystoidExplosionThroughManager(IEntity& entity, std::string* outReason)
{
    if (!gEnv || !gEnv->pGame)
    {
        if (outReason)
            *outReason = "missing_game";
        return false;
    }

    CGame* game = static_cast<CGame*>(gEnv->pGame);
    ArkCystoidManager* manager = game && game->m_pArkCystoidManager ? game->m_pArkCystoidManager.get() : nullptr;
    if (!manager)
    {
        if (outReason)
            *outReason = "missing_cystoid_manager";
        return false;
    }

    return TryGuardedVoidCall(
        "remote cystoid manager RequestExplosion",
        [&entity, manager]() { manager->RequestExplosion(entity.GetId()); },
        outReason);
}
constexpr float kEnemyRegistryDirtyDebounceSeconds = 0.1f;
constexpr float kProxyCombatStimulusSeconds = 0.25f;
constexpr float kProxyCombatStimulusRangeMeters = 32.0f;
constexpr size_t kMaxProxyCombatStimulusPerTick = 8;
constexpr size_t kMaxEnemyStatesPerTick = 32;
constexpr uint32_t kCoopRuntimeEntityFlags = ENTITY_FLAG_NO_SAVE | ENTITY_FLAG_PROCEDURAL | ENTITY_FLAG_NEVER_NETWORK_STATIC;
constexpr uint32_t kCoopClientPuppetFlags = kCoopRuntimeEntityFlags | ENTITY_FLAG_CLIENTSIDE_STATE;
constexpr float kEnemyNativeMovementIntentSeconds = 0.35f;
constexpr float kEnemyRemoteMannequinStateSeconds = 1.10f;
constexpr float kEnemyRemotePassiveMannequinStateSeconds = 8.00f;
constexpr float kEnemyRemoteActionMotionInferenceBlockSeconds = 1.25f;
constexpr float kEnemyRemoteActiveActionMotionBlockSeconds = 0.45f;
constexpr std::ptrdiff_t kArkNpcMovementDesireElementDesireOffset = 0x18;
constexpr std::ptrdiff_t kArkNpcMovementDesireSpeedLiteralOffset = 0x7C;
constexpr std::ptrdiff_t kArkNpcMovementDesireShiftFlagOffset = 0xA4;
constexpr std::ptrdiff_t kArkNpcMovementDesireJumpStyleOffset = 0xBC;

std::string ToLowerAsciiEnemySync(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool ContainsEnemySyncToken(const std::string& haystack, const char* needle)
{
    return needle && needle[0] && haystack.find(needle) != std::string::npos;
}

uint32_t MergeHeldRemoteMannequinFlags(uint32_t packetFlags, uint32_t heldFlags)
{
    const uint32_t movementFlags = CoopEnemyControlPolicy::MovementFlags();
    if (CoopEnemyControlPolicy::IsPassiveMannequinFlags(heldFlags))
    {
        packetFlags &= ~movementFlags;
        heldFlags &= ~movementFlags;
        return packetFlags | heldFlags;
    }

    if ((packetFlags & movementFlags) == 0)
        heldFlags &= ~movementFlags;
    return packetFlags | heldFlags;
}

bool CanHoldRemoteMannequinState(uint32_t flags)
{
    return (flags & CoopEnemyControlPolicy::BurstMovementFlags()) == 0;
}

bool CanHoldRemoteMannequinStateForPacket(uint32_t packetFlags, uint32_t heldFlags)
{
    if (!CanHoldRemoteMannequinState(heldFlags))
        return false;
    (void)packetFlags;
    return true;
}

uint32_t ClassifyEnemyMannequinActionForGate(int fragmentId, const char* fragmentName)
{
    const std::string lower = ToLowerAsciiEnemySync(fragmentName && fragmentName[0] ? std::string(fragmentName) : std::string());
    uint32_t flags = 0;

    (void)fragmentId;

    const bool physicsRecovery =
        ContainsEnemySyncToken(lower, "fall") ||
        ContainsEnemySyncToken(lower, "land") ||
        ContainsEnemySyncToken(lower, "stumble") ||
        ContainsEnemySyncToken(lower, "ragdoll") ||
        ContainsEnemySyncToken(lower, "recovery") ||
        ContainsEnemySyncToken(lower, "recover");
    if (physicsRecovery)
        flags |= CoopProtocol::kEnemyLocomotionFlagHitReacting;

    if (ContainsEnemySyncToken(lower, "motion_move") ||
        ContainsEnemySyncToken(lower, "idle_to_move") ||
        ContainsEnemySyncToken(lower, "idletomove") ||
        ContainsEnemySyncToken(lower, "walk") ||
        ContainsEnemySyncToken(lower, "run") ||
        ContainsEnemySyncToken(lower, "move") ||
        ContainsEnemySyncToken(lower, "strafe"))
    {
        if (!physicsRecovery)
        {
            flags |= CoopProtocol::kEnemyLocomotionFlagWalking;
            if (ContainsEnemySyncToken(lower, "run") || ContainsEnemySyncToken(lower, "charge"))
                flags |= CoopProtocol::kEnemyLocomotionFlagRunning;
        }
    }

    if (ContainsEnemySyncToken(lower, "idleturn") ||
        ContainsEnemySyncToken(lower, "idle_turn") ||
        ContainsEnemySyncToken(lower, "turn") ||
        ContainsEnemySyncToken(lower, "juketurn"))
    {
        flags |= CoopProtocol::kEnemyLocomotionFlagTurning;
    }

    if (ContainsEnemySyncToken(lower, "patrolidle") ||
        ContainsEnemySyncToken(lower, "wanderidle") ||
        ContainsEnemySyncToken(lower, "search") ||
        ContainsEnemySyncToken(lower, "distractor") ||
        ContainsEnemySyncToken(lower, "look") ||
        ContainsEnemySyncToken(lower, "notice") ||
        ContainsEnemySyncToken(lower, "inspect") ||
        ContainsEnemySyncToken(lower, "interact") ||
        ContainsEnemySyncToken(lower, "speakerreact") ||
        ContainsEnemySyncToken(lower, "lightreact") ||
        ContainsEnemySyncToken(lower, "lurk"))
    {
        flags |= CoopProtocol::kEnemyLocomotionFlagTurning;
    }

    const bool authoredBurstMovement =
        ContainsEnemySyncToken(lower, "shift") ||
        ContainsEnemySyncToken(lower, "dash") ||
        ContainsEnemySyncToken(lower, "teleport") ||
        ContainsEnemySyncToken(lower, "jump") ||
        ContainsEnemySyncToken(lower, "sidestep") ||
        ContainsEnemySyncToken(lower, "dodge") ||
        ContainsEnemySyncToken(lower, "charge") ||
        ContainsEnemySyncToken(lower, "ram") ||
        CoopEnemyControlPolicy::FragmentNameIsAuthoredBurstMovement(fragmentName && fragmentName[0]
            ? std::string_view(fragmentName)
            : std::string_view());
    if (authoredBurstMovement)
    {
        if (CoopEnemyControlPolicy::FragmentNameIsPhantomDash(
                fragmentName && fragmentName[0] ? std::string_view(fragmentName) : std::string_view()))
        {
            flags |= CoopProtocol::kEnemyLocomotionFlagDashing |
                CoopProtocol::kEnemyLocomotionFlagShifting;
        }
        else
        {
            flags |= CoopProtocol::kEnemyLocomotionFlagLunging;
        }
    }

    if (ContainsEnemySyncToken(lower, "morph"))
        flags |= CoopProtocol::kEnemyLocomotionFlagMorphing;
    if (ContainsEnemySyncToken(lower, "glood") ||
        ContainsEnemySyncToken(lower, "glooed") ||
        ContainsEnemySyncToken(lower, "gloo_pose"))
    {
        flags |= CoopProtocol::kEnemyLocomotionFlagGlooed;
    }
    if (ContainsEnemySyncToken(lower, "stun"))
        flags |= CoopProtocol::kEnemyLocomotionFlagStunned;
    if (ContainsEnemySyncToken(lower, "cower") || ContainsEnemySyncToken(lower, "fear"))
        flags |= CoopProtocol::kEnemyLocomotionFlagCowering;
    if (ContainsEnemySyncToken(lower, "reaction") ||
        ContainsEnemySyncToken(lower, "hit") ||
        ContainsEnemySyncToken(lower, "damage"))
    {
        flags |= CoopProtocol::kEnemyLocomotionFlagHitReacting;
    }
    if (ContainsEnemySyncToken(lower, "wpn_fire") ||
        ContainsEnemySyncToken(lower, "attack") ||
        ContainsEnemySyncToken(lower, "power_") ||
        ContainsEnemySyncToken(lower, "psiattack") ||
        ContainsEnemySyncToken(lower, "psiblast") ||
        ContainsEnemySyncToken(lower, "lightning") ||
        ContainsEnemySyncToken(lower, "emp") ||
        ContainsEnemySyncToken(lower, "shoot") ||
        ContainsEnemySyncToken(lower, "fire") ||
        ContainsEnemySyncToken(lower, "projectile") ||
        ContainsEnemySyncToken(lower, "cast") ||
        ContainsEnemySyncToken(lower, "melee") ||
        ContainsEnemySyncToken(lower, "breakgloo") ||
        CoopEnemyControlPolicy::FragmentNameIsAuthoredMovementAction(fragmentName && fragmentName[0]
            ? std::string_view(fragmentName)
            : std::string_view()))
    {
        flags |= CoopProtocol::kEnemyLocomotionFlagAttacking;
    }

    // Unknown/passive native fragments can still replace layer 0. Keep them
    // visible to the central mixer so authority Motion_Move is not overwritten
    // by a local Taunt/CombatIdle while this peer only owns target-facing.
    if (flags == 0 && !lower.empty() && lower != "unknown" && lower != "-")
        flags |= CoopProtocol::kEnemyLocomotionFlagMannequinDriven;

    return flags;
}

bool IsEnemyMannequinMovementCarryFragmentName(const std::string& fragmentName)
{
    return CoopEnemyControlPolicy::FragmentNameCarriesAuthoredMovement(fragmentName);
}

bool IsEnemyMannequinPassiveMovementCarryFragmentName(const std::string& fragmentName)
{
    return CoopEnemyControlPolicy::FragmentNameCarriesPassiveMovement(fragmentName);
}

int EnemyMannequinPriority(uint32_t flags)
{
    if ((flags & CoopProtocol::kEnemyLocomotionFlagGlooed) != 0)
        return 100;
    if ((flags & (CoopProtocol::kEnemyLocomotionFlagDashing |
            CoopProtocol::kEnemyLocomotionFlagShifting |
            CoopProtocol::kEnemyLocomotionFlagMorphing |
            CoopProtocol::kEnemyLocomotionFlagLunging)) != 0)
    {
        return 80;
    }
    if ((flags & (CoopProtocol::kEnemyLocomotionFlagAttacking |
            CoopProtocol::kEnemyLocomotionFlagHitReacting |
            CoopProtocol::kEnemyLocomotionFlagStunned |
            CoopProtocol::kEnemyLocomotionFlagCowering)) != 0)
    {
        return 60;
    }
    if ((flags & (CoopProtocol::kEnemyLocomotionFlagWalking |
            CoopProtocol::kEnemyLocomotionFlagRunning)) != 0)
    {
        return 40;
    }
    if ((flags & CoopProtocol::kEnemyLocomotionFlagTurning) != 0)
        return 20;
    return 0;
}

void LogCoop(std::string_view msg)
{
    CoopRuntimeLog::Write(msg);
}

bool IsGameReady()
{
    return gEnv && gEnv->pEntitySystem && ArkPlayer::GetInstancePtr() && ArkPlayer::GetInstance().GetEntity();
}

float EnemyMovementFallbackSpeed(MovementStyle::Speed speed)
{
    switch (speed)
    {
    case MovementStyle::Speed::Sprint:
        return 8.0f;
    case MovementStyle::Speed::Run:
        return 5.7f;
    case MovementStyle::Speed::Walk:
    default:
        return 2.25f;
    }
}

uint32_t EnemyMovementFlagsFromStyle(const MovementStyle& style, float speed, bool phantomDash)
{
    uint32_t flags = 0;
    if (style.m_speed == MovementStyle::Speed::Run ||
        style.m_speed == MovementStyle::Speed::Sprint ||
        speed > 5.7f)
    {
        flags |= CoopProtocol::kEnemyLocomotionFlagRunning;
    }
    else if (speed > kEnemyMovementIntentSpeedThreshold)
    {
        flags |= CoopProtocol::kEnemyLocomotionFlagWalking;
    }

    if (style.m_bShift || style.m_jumpStyle != MovementStyle::JumpStyle::None)
    {
        if (phantomDash)
            flags |= CoopProtocol::kEnemyLocomotionFlagDashing | CoopProtocol::kEnemyLocomotionFlagShifting;
        else
            flags |= CoopProtocol::kEnemyLocomotionFlagLunging;
    }
    return flags;
}

Vec3 NormalizeDirectionOr(const Vec3& value, const Vec3& fallback);
bool EnvFlagEnabled(const char* name);

bool IsFiniteVec3(const Vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

ArkNpcMovementDesire* ReadActiveMovementDesireFromManager(ArkNpcMovementDesireManager* manager)
{
    if (!manager || !CoopRuntimeGuards::IsLikelyRuntimeCppObject(manager, sizeof(void*) * 4))
        return nullptr;

    void* activeElement = nullptr;
    if (!CoopRuntimeGuards::TryReadRuntimeValue(reinterpret_cast<void* const*>(&manager->m_desires.m_head.m_pNext), activeElement) ||
        !activeElement ||
        activeElement == static_cast<void*>(&manager->m_desires.m_head))
    {
        return nullptr;
    }

    ArkNpcMovementDesire* desire = nullptr;
    if (!CoopRuntimeGuards::TryReadRuntimeValue(
            reinterpret_cast<ArkNpcMovementDesire* const*>(
                reinterpret_cast<const std::byte*>(activeElement) + kArkNpcMovementDesireElementDesireOffset),
            desire) ||
        !desire ||
        !CoopRuntimeGuards::IsLikelyRuntimeCppObject(desire, sizeof(void*) * 4))
    {
        return nullptr;
    }
    return desire;
}

bool ReadMovementDesireSnapshot(
    ArkNpcMovementDesire* desire,
    Vec3 fallbackDirection,
    bool phantomDash,
    Vec3& outDirection,
    float& outSpeed,
    uint32_t& outFlags)
{
    if (!desire || !CoopRuntimeGuards::IsLikelyRuntimeCppObject(desire, sizeof(void*) * 4))
        return false;

    const auto* desireBytes = reinterpret_cast<const std::byte*>(desire);
    float speedLiteral = 0.0f;
    bool shift = false;
    int jumpStyle = 0;
    CoopRuntimeGuards::TryReadRuntimeValue(
        reinterpret_cast<const float*>(desireBytes + kArkNpcMovementDesireSpeedLiteralOffset),
        speedLiteral);
    CoopRuntimeGuards::TryReadRuntimeValue(
        reinterpret_cast<const bool*>(desireBytes + kArkNpcMovementDesireShiftFlagOffset),
        shift);
    CoopRuntimeGuards::TryReadRuntimeValue(
        reinterpret_cast<const int*>(desireBytes + kArkNpcMovementDesireJumpStyleOffset),
        jumpStyle);

    outSpeed = std::isfinite(speedLiteral) && speedLiteral > 0.1f ? speedLiteral : 2.25f;
    outFlags = outSpeed > 5.7f
        ? CoopProtocol::kEnemyLocomotionFlagRunning
        : CoopProtocol::kEnemyLocomotionFlagWalking;
    if (shift || jumpStyle != 0)
    {
        if (phantomDash)
            outFlags |= CoopProtocol::kEnemyLocomotionFlagDashing | CoopProtocol::kEnemyLocomotionFlagShifting;
        else
            outFlags |= CoopProtocol::kEnemyLocomotionFlagLunging;
    }

    // Exact MoveTo destinations are captured from MovementRequest. This fallback
    // avoids reading a variant target while the desire manager is transitioning.
    outDirection = NormalizeDirectionOr(fallbackDirection, fallbackDirection);
    return outFlags != 0;
}

void ApplyNpcGlooFrozenNative(ArkNpc& npc)
{
    npc.m_movementDesireManager.CancelMovement();
    IEntity* entity = nullptr;
    std::string reason;
    TryGuardedCall("enemy gloo native GetEntity", [&npc]() -> IEntity* { return npc.GetEntity(); }, entity, &reason);
    if (entity)
    {
        ArkGlooEffectAccumulated* effect = nullptr;
        if (TryGuardedCall("enemy gloo native GetGlooEffect", [&npc]() { return npc.GetGlooEffect(); }, effect, &reason) &&
            effect)
        {
            TryGuardedVoidCall(
                "enemy gloo native SkipToFrozen",
                [effect, entity]()
                {
                    effect->SkipToFrozen(*entity);
                });
        }
    }

    npc.m_bIsFrozenInGloo = true;
    TryGuardedVoidCall("enemy gloo native StartGlooEffects", [&npc]() { npc.StartGlooEffects(true); });
    TryGuardedVoidCall("enemy gloo native OnGlooFrozen", [&npc]() { npc.OnGlooFrozen(); });
}

void ClearNpcGlooFrozenNative(ArkNpc& npc)
{
    npc.m_bIsFrozenInGloo = false;
    TryGuardedVoidCall("enemy gloo native OnGlooBroken", [&npc]() { npc.OnGlooBroken(); });
    TryGuardedVoidCall("enemy gloo native StopGlooEffects", [&npc]() { npc.StopGlooEffects(); });
}

Vec3 NormalizeDirectionOr(const Vec3& value, const Vec3& fallback)
{
    const float lengthSq = value.GetLengthSquared();
    if (lengthSq <= 0.0001f)
        return fallback;

    return value * (1.0f / std::sqrt(lengthSq));
}

uint8_t ClampNativeAttentionLevel(EArkAttentionLevel level)
{
    const int value = static_cast<int>(level);
    return value >= static_cast<int>(EArkAttentionLevel::unknown) &&
        value <= static_cast<int>(EArkAttentionLevel::known)
        ? static_cast<uint8_t>(value)
        : CoopEnemyAuthorityPolicy::kUnknownAttention;
}

uint8_t LocalPlayerEnemyAttentionLevel(const IEntity& enemy)
{
    if (!ArkPlayer::GetInstancePtr())
        return CoopEnemyAuthorityPolicy::kUnknownAttention;

    IEntity* localPlayerEntity = ArkPlayer::GetInstance().GetEntity();
    if (!localPlayerEntity)
        return CoopEnemyAuthorityPolicy::kUnknownAttention;

    const EntityId enemyId = enemy.GetId();
    const EntityId localPlayerId = localPlayerEntity->GetId();
    CGame* game = gEnv && gEnv->pGame ? static_cast<CGame*>(gEnv->pGame) : nullptr;
    ArkAttentionManager* attentionManager = game && game->m_pArkAttentionManager
        ? game->m_pArkAttentionManager.get()
        : nullptr;
    if (attentionManager)
    {
        bool tracksComplex = false;
        if (TryGuardedCall(
                "enemy attention level tracks complex player",
                [attentionManager, enemyId, localPlayerId]()
                {
                    return attentionManager->IsSubjectTrackingComplexObject(enemyId, localPlayerId);
                },
                tracksComplex) &&
            tracksComplex)
        {
            EArkAttentionLevel nativeLevel = EArkAttentionLevel::unknown;
            if (TryGuardedCall(
                    "enemy attention level GetComplexAttentionLevel",
                    [attentionManager, enemyId, localPlayerId]()
                    {
                        return attentionManager->GetComplexAttentionLevel(enemyId, localPlayerId);
                    },
                    nativeLevel))
            {
                return ClampNativeAttentionLevel(nativeLevel);
            }
        }
    }

    ArkPlayerComponent& playerComponent = ArkPlayer::GetInstance().m_playerComponent;
    ArkPlayerAwarenessComponent& awareness = playerComponent.GetAwarenessComponent();
    const auto stateIt = awareness.m_awarenessStates.find(enemyId);
    if (stateIt != awareness.m_awarenessStates.end())
    {
        const ArkPlayerAwarenessComponent::AwarenessState& state = stateIt->second;
        if (!state.m_bHidden)
            return ClampNativeAttentionLevel(state.m_level);
    }

    ArkPlayerUIComponent& ui = playerComponent.GetUIComponent();
    for (const ArkPlayerUIComponent::MarkerEntry& marker : ui.m_markerEntries)
    {
        if (marker.m_entity != enemyId)
            continue;

        const bool visible =
            marker.m_awarenessUpdateType == ArkPlayerUIComponent::EArkAwarenessUpdateType::visible ||
            marker.m_awarenessUpdateType == ArkPlayerUIComponent::EArkAwarenessUpdateType::fullAwareness ||
            marker.m_bShowingMarkerEntry ||
            marker.m_bAwarenessAnimating;
        if (visible)
            return ClampNativeAttentionLevel(marker.m_awarenessLevel);
    }

    if (ArkNpc* npc = EntityUtils::GetArkNpc(const_cast<IEntity*>(&enemy)))
    {
        unsigned topAttentionTarget = INVALID_ENTITYID;
        if (TryGuardedCall(
                "enemy awareness GetTopAttentionTargetEntityId",
                [npc]() { return npc->GetTopAttentionTargetEntityId(); },
                topAttentionTarget) &&
            topAttentionTarget == localPlayerId)
        {
            // The exact pair should normally be available above. A native top
            // target is still a real non-zero claim if the pair was created in
            // the same frame and the manager/UI views have not caught up yet.
            return static_cast<uint8_t>(EArkAttentionLevel::noticed);
        }
    }
    return CoopEnemyAuthorityPolicy::kUnknownAttention;
}

bool LocalPlayerHasEnemyAwareness(const IEntity& enemy)
{
    return LocalPlayerEnemyAttentionLevel(enemy) > CoopEnemyAuthorityPolicy::kUnknownAttention;
}

bool IsLocalPlayerAuthorityBlockedByModalStateImpl()
{
    if (!gEnv)
        return true;

    if (gEnv->pSystem && gEnv->pSystem->IsPaused())
        return true;

    if (gEnv->pTimer && gEnv->pTimer->IsTimerPaused(ITimer::ETIMER_GAME))
        return true;

    if (!ArkPlayer::GetInstancePtr())
        return true;

    const ArkPlayer& player = ArkPlayer::GetInstance();
    if (!player.m_input.m_modeStack.empty())
    {
        const ArkPlayerInput::Mode mode = player.m_input.m_modeStack.back().m_mode;
        if (mode != ArkPlayerInput::Mode::player)
        {
            // Prey pushes its single-player menu mode when a window loses
            // focus even with focus-loss pausing disabled. That background
            // marker must not disable local enemy awareness/combat in a live
            // multiplayer session. A real foreground pause/PDA remains
            // authority-blocking as before.
            const HWND gameWindow =
                gEnv->pSystem ? reinterpret_cast<HWND>(gEnv->pSystem->GetHWND()) : nullptr;
            const bool defocusedSyntheticMenu =
                mode == ArkPlayerInput::Mode::menu &&
                gameWindow &&
                GetForegroundWindow() != gameWindow;
            if (!defocusedSyntheticMenu)
                return true;
        }
    }

    return false;
}

bool EnvFlagEnabled(const char* name)
{
    return CoopRuntimeConfig::Flag(name);
}

void ApplyEnemyPositionOnly(IEntity& entity, const Vec3& position)
{
    entity.SetPos(position, 0, true, true);
}

bool TrySetEnemyLegMotionParams(
    IEntity& entity,
    float travelSpeed,
    float travelAngle,
    float frameTime,
    std::string* outReason = nullptr)
{
    ICharacterInstance* character = nullptr;
    if (!TryGuardedCall(
            "enemy leg blend GetCharacter",
            [&entity]() { return entity.GetCharacter(0); },
            character,
            outReason) ||
        !character ||
        !CoopRuntimeGuards::IsLikelyRuntimeCppObject(character, sizeof(void*)))
    {
        if (outReason && outReason->empty())
            *outReason = "missing_character";
        return false;
    }

    void** characterVtable = *reinterpret_cast<void***>(character);
    if (!CoopRuntimeGuards::IsReadableRuntimePointer(
            characterVtable,
            (kCharacterGetSkeletonAnimVtableIndex + 1) * sizeof(void*)) ||
        !CoopRuntimeGuards::IsExecutableRuntimePointer(
            characterVtable[kCharacterGetSkeletonAnimVtableIndex]))
    {
        if (outReason)
            *outReason = "invalid_character_vtable";
        return false;
    }

    using GetSkeletonAnimFn = void* (*)(void*);
    void* skeletonAnim = nullptr;
    if (!TryGuardedCall(
            "enemy leg blend GetISkeletonAnim",
            [character, characterVtable]()
            {
                return reinterpret_cast<GetSkeletonAnimFn>(
                    characterVtable[kCharacterGetSkeletonAnimVtableIndex])(character);
            },
            skeletonAnim,
            outReason) ||
        !skeletonAnim ||
        !CoopRuntimeGuards::IsLikelyRuntimeCppObject(skeletonAnim, sizeof(void*)))
    {
        if (outReason && outReason->empty())
            *outReason = "missing_skeleton_anim";
        return false;
    }

    void** skeletonVtable = *reinterpret_cast<void***>(skeletonAnim);
    if (!CoopRuntimeGuards::IsReadableRuntimePointer(
            skeletonVtable,
            (kSkeletonSetDesiredMotionParamVtableIndex + 1) * sizeof(void*)) ||
        !CoopRuntimeGuards::IsExecutableRuntimePointer(
            skeletonVtable[kSkeletonSetDesiredMotionParamVtableIndex]))
    {
        if (outReason)
            *outReason = "invalid_skeleton_vtable";
        return false;
    }

    using SetDesiredMotionParamFn = void (*)(void*, int, float, float);
    const auto setDesiredMotionParam = reinterpret_cast<SetDesiredMotionParamFn>(
        skeletonVtable[kSkeletonSetDesiredMotionParamVtableIndex]);
    return TryGuardedVoidCall(
        "enemy leg blend SetDesiredMotionParams",
        [skeletonAnim, setDesiredMotionParam, travelSpeed, travelAngle, frameTime]()
        {
            // Resolve the opaque character/skeleton ABI once per frame, then
            // update the two owned blend dimensions through the same native
            // function pointer. No other motion parameter is touched.
            setDesiredMotionParam(
                skeletonAnim,
                kMotionParamTravelSpeed,
                travelSpeed,
                frameTime);
            setDesiredMotionParam(
                skeletonAnim,
                kMotionParamTravelAngle,
                travelAngle,
                frameTime);
        },
        outReason);
}

float ComputeRemoteEnemySmoothStepForTick(
    float distance,
    float tickSeconds,
    float baseTravelSpeed,
    bool burstMovement,
    float maxCorrectionSpeedOverride,
    bool& hardJump,
    float& correctionSpeed)
{
    const float hardJumpDistance = burstMovement
        ? kEnemyRemoteSmoothBurstHardJumpDistance
        : kEnemyRemoteSmoothHardJumpDistance;
    hardJump = distance >= hardJumpDistance;
    if (distance <= kEnemyRemoteSmoothTinySnapDistance)
    {
        correctionSpeed = 0.0f;
        return distance;
    }

    const float catchupDistance = burstMovement
        ? 0.0f
        : std::max(0.0f, distance - kEnemyRemoteCatchupIntentDistance);
    float maxCorrectionSpeed = burstMovement
        ? kEnemyRemoteSmoothBurstMaxCorrectionSpeed
        : kEnemyRemoteSmoothMaxCorrectionSpeed;
    if (std::isfinite(maxCorrectionSpeedOverride) && maxCorrectionSpeedOverride > 0.0f)
        maxCorrectionSpeed = std::min(maxCorrectionSpeed, maxCorrectionSpeedOverride);

    float correctionOnlySpeed = std::clamp(
        kEnemyRemoteSmoothMinCorrectionSpeed +
            distance *
                (burstMovement
                    ? kEnemyRemoteSmoothBurstCorrectionSpeedPerMeter
                    : kEnemyRemoteSmoothCorrectionSpeedPerMeter) +
            catchupDistance * kEnemyRemoteSmoothCatchupCorrectionSpeedPerMeter,
        kEnemyRemoteSmoothMinCorrectionSpeed,
        maxCorrectionSpeed);
    const float travelSpeed = std::isfinite(baseTravelSpeed)
        ? std::clamp(
            baseTravelSpeed,
            0.0f,
            burstMovement
                ? kEnemyRemoteReceiverMaxDashSpeed
                : kEnemyRemoteReceiverMaxRunSpeed)
        : 0.0f;
    // Travel speed follows normal authority motion. Correction speed is only
    // allowed to dominate when the local puppet has actually fallen behind.
    correctionSpeed = std::max(correctionOnlySpeed, travelSpeed);

    const float clampedTickSeconds = std::clamp(tickSeconds, 0.001f, 0.05f);
    float maxStep = correctionSpeed * clampedTickSeconds;
    // Bursts/shifts are discrete authority motion for animation, but the
    // transform path still has to stay smooth. Let the action layer show the
    // shift while the replicated position converges through the same bounded
    // hard-gap envelope as ordinary correction.
    if (hardJump)
    {
        const float hardJumpFrames = burstMovement
            ? kEnemyRemoteSmoothBurstHardJumpMinFrames
            : kEnemyRemoteSmoothHardJumpMinFrames;
        const float hardJumpSeconds =
            hardJumpFrames / kEnemyRemoteSmoothAssumedFrameRate;
        const float hardJumpStep =
            distance * (clampedTickSeconds / std::max(hardJumpSeconds, 0.001f));
        maxStep = std::min(maxStep, hardJumpStep);
        correctionSpeed = maxStep / clampedTickSeconds;
    }

    // Never impose a distance-sized minimum per render frame. At 120+ FPS the
    // old 1.5 cm floor consumed a whole network sample in a few frames and then
    // waited at its endpoint, which looked like smooth but visible steps.
    return std::clamp(maxStep, 0.0f, distance);
}

float ComputeRemoteEnemySmoothStep(
    float distance,
    float baseTravelSpeed,
    bool burstMovement,
    float maxCorrectionSpeedOverride,
    bool& hardJump,
    float& correctionSpeed)
{
    return ComputeRemoteEnemySmoothStepForTick(
        distance,
        kMimicStateTickSeconds,
        baseTravelSpeed,
        burstMovement,
        maxCorrectionSpeedOverride,
        hardJump,
        correctionSpeed);
}

float ComputeRemoteEnemySmoothBaseTravelSpeed(uint32_t locomotionFlags, uint32_t mannequinFlags, float remoteSpeed)
{
    if (!std::isfinite(remoteSpeed) || remoteSpeed <= kEnemyMovementIntentSpeedThreshold)
        return 0.0f;

    const uint32_t flags = locomotionFlags | mannequinFlags;
    if ((flags & (CoopProtocol::kEnemyLocomotionFlagDashing |
            CoopProtocol::kEnemyLocomotionFlagShifting |
            CoopProtocol::kEnemyLocomotionFlagMorphing |
            CoopProtocol::kEnemyLocomotionFlagLunging)) != 0)
    {
        return std::clamp(remoteSpeed, 0.0f, kEnemyRemoteReceiverMaxDashSpeed);
    }

    if ((flags & (CoopProtocol::kEnemyLocomotionFlagWalking |
            CoopProtocol::kEnemyLocomotionFlagRunning)) != 0)
    {
        return std::clamp(remoteSpeed, 0.0f, kEnemyRemoteReceiverMaxRunSpeed);
    }

    // Passive Mannequin fragments can still move the authority through native
    // root motion. Use their wire speed only for smooth positional catch-up;
    // do not convert them into Walk/Run or Motion_Move action intent.
    if (CoopEnemyControlPolicy::IsPassiveMannequinFlags(flags))
        return std::clamp(remoteSpeed, 0.0f, kEnemyRemoteReceiverMaxWalkSpeed);

    return 0.0f;
}

float ComputeRemoteEnemyRotationAngle(const Quat& currentRotation, const Quat& targetRotation)
{
    const float dot = std::clamp(std::fabs(currentRotation | targetRotation), 0.0f, 1.0f);
    return 2.0f * std::acos(dot);
}

float ComputeRemoteEnemyRotationAlphaForTick(
    float angle,
    bool positionHardJump,
    float tickSeconds,
    bool burstMovement,
    bool& hardRotation,
    float& rotationSpeed)
{
    hardRotation = positionHardJump && !burstMovement;
    if (angle <= 0.002f)
    {
        rotationSpeed = 0.0f;
        return 1.0f;
    }

    rotationSpeed = std::clamp(
        kEnemyRemoteSmoothMinRotationSpeed + angle *
            (burstMovement
                ? kEnemyRemoteSmoothBurstRotationSpeedPerRadian
                : kEnemyRemoteSmoothRotationSpeedPerRadian),
        kEnemyRemoteSmoothMinRotationSpeed,
        burstMovement
            ? kEnemyRemoteSmoothBurstMaxRotationSpeed
            : kEnemyRemoteSmoothMaxRotationSpeed);

    const float clampedTickSeconds = std::clamp(tickSeconds, 0.001f, 0.05f);
    float maxStep = rotationSpeed * clampedTickSeconds;
    if (hardRotation)
    {
        const float hardJumpSeconds =
            kEnemyRemoteSmoothHardJumpMinFrames / kEnemyRemoteSmoothAssumedFrameRate;
        const float hardJumpStep =
            angle * (clampedTickSeconds / std::max(hardJumpSeconds, 0.001f));
        maxStep = std::min(maxStep, hardJumpStep);
    }

    return std::clamp(maxStep / angle, 0.0f, 1.0f);
}

float ComputeRemoteEnemyRotationAlpha(
    float angle,
    bool positionHardJump,
    bool burstMovement,
    bool& hardRotation,
    float& rotationSpeed)
{
    return ComputeRemoteEnemyRotationAlphaForTick(
        angle,
        positionHardJump,
        kMimicStateTickSeconds,
        burstMovement,
        hardRotation,
        rotationSpeed);
}

float EnemyAnimationNowSeconds()
{
    return gEnv && gEnv->pTimer ? gEnv->pTimer->GetAsyncCurTime() : 0.0f;
}


Vec3 GetOffsetFromPlayer(float metersForward)
{
    ArkPlayer& player = ArkPlayer::GetInstance();
    IEntity* playerEntity = player.GetEntity();
    const Quat playerRot = playerEntity->GetRotation();
    return playerEntity->GetWorldPos() + playerRot * Vec3(0.0f, metersForward, 0.0f);
}
}


bool ModMain::IsLocalPlayerAuthorityBlockedByModalState() const
{
    return IsLocalPlayerAuthorityBlockedByModalStateImpl();
}

bool ModMain::TriggerRemoteEnemyBurstVisualEffect(
    EnemyAuthorityState& state,
    IEntity& entity,
    uint64_t enemyNetId,
    int fragmentId,
    uint32_t sequence,
    const Vec3& startPosition,
    const Vec3& endPosition,
    const Vec3&,
    const char* reason)
{
    if (EnvFlagEnabled("COOP_DISABLE_REMOTE_ENEMY_BURST_FX"))
    {
        ++m_enemyBurstFxSkips;
        m_lastEnemyFxEvent =
            "remote_enemy_burst_fx_disabled net=" + std::to_string(enemyNetId) +
            " entity=" + std::to_string(entity.GetId());
        return false;
    }

    if (!IsFiniteVec3(startPosition) || !IsFiniteVec3(endPosition))
    {
        ++m_enemyBurstFxFailures;
        m_lastEnemyFxEvent =
            "remote_enemy_burst_fx_bad_position net=" + std::to_string(enemyNetId) +
            " entity=" + std::to_string(entity.GetId());
        AppendEnemySyncTrace("enemy_fx", m_lastEnemyFxEvent);
        return false;
    }

    state.remoteBurstFxCooldownSeconds =
        std::max(0.0f, state.remoteBurstFxCooldownSeconds - kMimicStateTickSeconds);

    const bool hasSequenceKey = sequence != 0 && fragmentId >= 0;
    if (hasSequenceKey &&
        state.remoteBurstFxSequence == sequence &&
        state.remoteBurstFxFragmentId == fragmentId)
    {
        ++m_enemyBurstFxSkips;
        return false;
    }

    if (!hasSequenceKey && state.remoteBurstFxCooldownSeconds > 0.0f)
    {
        ++m_enemyBurstFxSkips;
        return false;
    }

    const Vec3 travel = endPosition - startPosition;
    const float distance = travel.GetLength();
    ArkNpc* npc = EntityUtils::GetArkNpc(&entity);
    std::string guardReason;
    const bool ok = npc && TryGuardedVoidCall(
        "remote enemy Phantom Shift character effect",
        [npc]()
        {
            // Exact visual half of ArkNpc::ShiftTelegraph. Vanilla restarts
            // this character-effect type; the Phantom archetype resolves it
            // to Phantom_ShiftStart, attaching four limb streaks plus the
            // ShiftStartDissolve smoke to the actual skeleton. Never replace
            // this with DuplicateEffect_01: Etheric Doppelganger is a separate
            // NPC ability with its own gameplay lifecycle.
            npc->StopCharacterEffect(ArkCharacterEffectType::shiftTelegraph);
            npc->StartCharacterEffect(ArkCharacterEffectType::shiftTelegraph);
        },
        &guardReason);

    if (ok)
    {
        ++m_enemyBurstFxTriggers;
        state.remoteBurstFxSequence = sequence;
        state.remoteBurstFxFragmentId = fragmentId;
        state.remoteBurstFxCooldownSeconds = 0.25f;
        m_lastEnemyFxEvent =
            "remote_enemy_burst_fx net=" + std::to_string(enemyNetId) +
            " entity=" + std::to_string(entity.GetId()) +
            " fragment=" + std::to_string(fragmentId) +
            " seq=" + std::to_string(sequence) +
            " distance=" + std::to_string(distance) +
            " characterEffect=shiftTelegraph" +
            " attachmentSet=Phantom_ShiftStart" +
            " reason=" + (reason && reason[0] ? reason : "-");
    }
    else
    {
        ++m_enemyBurstFxFailures;
        m_lastEnemyFxEvent =
            "remote_enemy_burst_fx_failed net=" + std::to_string(enemyNetId) +
            " entity=" + std::to_string(entity.GetId()) +
            " fragment=" + std::to_string(fragmentId) +
            " seq=" + std::to_string(sequence) +
            " reason=" + (npc
                ? (guardReason.empty() ? std::string("native_call_failed") : guardReason)
                : std::string("no_ark_npc"));
    }

    AppendEnemySyncTrace("enemy_fx", m_lastEnemyFxEvent);
    return ok;
}


bool ModMain::TriggerRemoteEnemyAbilityVisualEffect(
    EnemyAuthorityState& state,
    IEntity& entity,
    const CoopProtocol::EnemyAbilityFxEventPacket& packet,
    const char* reason)
{
    if (EnvFlagEnabled("COOP_DISABLE_REMOTE_ENEMY_ABILITY_FX"))
    {
        ++m_enemyBurstFxSkips;
        m_lastEnemyFxEvent =
            "remote_enemy_ability_fx_disabled net=" + std::to_string(packet.enemyNetId) +
            " entity=" + std::to_string(entity.GetId()) +
            " kind=" + std::to_string(packet.abilityKind);
        AppendEnemySyncTrace("enemy_fx", m_lastEnemyFxEvent);
        return false;
    }

    Vec3 position(packet.px, packet.py, packet.pz);
    if (!IsFiniteVec3(position))
        position = entity.GetWorldPos();
    const Vec3 direction = NormalizeDirectionOr(
        Vec3(packet.dx, packet.dy, packet.dz),
        entity.GetWorldRotation().GetColumn1());
    const Vec3 enemyFxPosition = entity.GetWorldPos() + Vec3(0.0f, 0.0f, 0.45f);
    const Vec3 impactFxPosition = position + direction * 1.20f + Vec3(0.0f, 0.0f, 0.12f);

    bool ok = false;
    auto trigger = [&](const char* effectName, const Vec3& fxPosition, float scale, const char* layerReason)
    {
        ok = TriggerWorldParticleEffect(effectName, fxPosition, direction, scale, layerReason) || ok;
    };
    auto queue = [&](const char* effectName, const Vec3& fxPosition, float scale, float delaySeconds, const char* layerReason)
    {
        QueueWorldParticleEffect(effectName, fxPosition, direction, scale, delaySeconds, layerReason);
        ok = true;
    };

    switch (packet.abilityKind)
    {
    case CoopProtocol::kEnemyAbilityFxNpcMimicryBegin:
    {
        const uint64_t targetGuid = packet.controllingTechnopathStableKey;
        const uint64_t targetArchetypeId = packet.abilityTargetArchetypeId;
        ArkNpc* npc = EntityUtils::GetArkNpc(&entity);
        if (!npc)
        {
            m_lastEnemyFxEvent =
                "remote_enemy_mimicry_begin_missing_npc net=" + std::to_string(packet.enemyNetId) +
                " entity=" + std::to_string(entity.GetId());
            AppendEnemySyncTrace("enemy_fx", m_lastEnemyFxEvent);
            break;
        }

        const int rawReason = packet.mannequinFragmentId;
        const EArkNpcMimicryReason mimicryReason =
            rawReason >= static_cast<int>(EArkNpcMimicryReason::none) &&
                rawReason < static_cast<int>(EArkNpcMimicryReason::_count)
            ? static_cast<EArkNpcMimicryReason>(rawReason)
            : EArkNpcMimicryReason::none;
        const bool ignorePsi =
            (packet.flags & CoopProtocol::kEnemyAbilityFxFlagMimicIgnorePsi) != 0;
        std::string guardReason;
        const bool rememberedMimicryActive = state.localMimicryActive;
        const uint64_t rememberedTargetGuid = state.localMimicryTargetGuid;
        const uint64_t rememberedTargetArchetypeId = state.localMimicryTargetArchetypeId;
        const Vec3 rememberedTargetPosition = state.localMimicryTargetPosition;
        state.localMimicryStateKnown = true;
        state.localMimicryActive = true;
        state.localMimicryIgnorePsi = ignorePsi;
        state.localMimicryTargetGuid = targetGuid;
        state.localMimicryTargetArchetypeId = targetArchetypeId;
        state.localMimicryTargetPosition = position;
        state.localMimicryReason = mimicryReason;
        CoopSerialSequence::Observe(packet.mannequinSequence, state.localMimicryEventSequence);

        bool mimicResult = false;
        bool alreadyMimicking = false;
        unsigned existingTargetId = 0;
        const bool stateReadOk =
            TryGuardedCall(
                "remote npc mimicry IsMimicking",
                [npc]() { return npc->IsMimicking(); },
                alreadyMimicking,
                &guardReason) &&
            (!alreadyMimicking || TryGuardedCall(
                "remote npc mimicry GetMimicingEntityId",
                [npc]() { return npc->GetMimicingEntityId(); },
                existingTargetId,
                &guardReason));
        const bool descriptorAlreadyCurrent =
            stateReadOk &&
            alreadyMimicking &&
            rememberedMimicryActive &&
            ((targetGuid != 0 && rememberedTargetGuid == targetGuid) ||
                (targetGuid == 0 && targetArchetypeId != 0 &&
                    rememberedTargetArchetypeId == targetArchetypeId &&
                    (rememberedTargetPosition - position).GetLengthSquared() <= 0.25f));

        std::string targetRoute;
        IEntity* targetEntity = descriptorAlreadyCurrent
            ? nullptr
            : ResolveNpcMimicryTarget(packet, entity, targetRoute, guardReason);
        const EntityId targetId = targetEntity ? targetEntity->GetId() : INVALID_ENTITYID;
        if (descriptorAlreadyCurrent)
        {
            ok = true;
            targetRoute = "remembered_current";
        }
        else if (!targetEntity)
        {
            m_lastEnemyFxEvent =
                "remote_enemy_mimicry_begin_missing_target net=" + std::to_string(packet.enemyNetId) +
                " entity=" + std::to_string(entity.GetId()) +
                " targetGuid=" + std::to_string(targetGuid) +
                " targetArch=" + std::to_string(targetArchetypeId) +
                " targetPos=" + std::to_string(position.x) + "," +
                    std::to_string(position.y) + "," + std::to_string(position.z);
            AppendEnemySyncTrace("enemy_fx", m_lastEnemyFxEvent);
            break;
        }
        else
        {
            m_applyingRemoteEnemyAbilityFxEvent = true;
            const bool callOk = TryGuardedCall(
                "remote npc MimicEntity",
                [npc, targetEntity, mimicryReason, ignorePsi]()
                {
                    return npc->MimicEntity(*targetEntity, mimicryReason, ignorePsi);
                },
                mimicResult,
                &guardReason);
            m_applyingRemoteEnemyAbilityFxEvent = false;
            ok = callOk && mimicResult;
        }
        m_lastEnemyFxEvent =
            std::string(ok
                ? (descriptorAlreadyCurrent || (alreadyMimicking && existingTargetId == targetId)
                    ? "remote_enemy_mimicry_begin_current"
                    : "remote_enemy_mimicry_begin_applied")
                : "remote_enemy_mimicry_begin_failed") +
            " net=" + std::to_string(packet.enemyNetId) +
            " entity=" + std::to_string(entity.GetId()) +
            " target=" + std::to_string(targetId) +
            " targetGuid=" + std::to_string(targetGuid) +
            " targetArch=" + std::to_string(targetArchetypeId) +
            " targetRoute=" + targetRoute +
            " reason=" + std::to_string(rawReason) +
            " guard=" + (guardReason.empty() ? std::string("-") : guardReason);
        AppendEnemySyncTrace("enemy_fx", m_lastEnemyFxEvent);
        break;
    }
    case CoopProtocol::kEnemyAbilityFxNpcMimicryEnd:
    {
        ArkNpc* npc = EntityUtils::GetArkNpc(&entity);
        if (!npc)
            break;

        std::string guardReason;
        bool endResult = false;
        bool isMimicking = true;
        state.localMimicryStateKnown = true;
        state.localMimicryActive = false;
        state.localMimicryIgnorePsi = false;
        state.localMimicryTargetGuid = 0;
        state.localMimicryTargetArchetypeId = 0;
        state.localMimicryTargetPosition = entity.GetWorldPos();
        state.localMimicryReason = EArkNpcMimicryReason::none;
        CoopSerialSequence::Observe(packet.mannequinSequence, state.localMimicryEventSequence);
        const bool stateReadOk = TryGuardedCall(
            "remote npc mimicry end IsMimicking",
            [npc]() { return npc->IsMimicking(); },
            isMimicking,
            &guardReason);
        if (stateReadOk && !isMimicking)
        {
            ok = true;
        }
        else
        {
            m_applyingRemoteEnemyAbilityFxEvent = true;
            const bool callOk = TryGuardedCall(
                "remote npc EndMimicry",
                [npc]() { return npc->EndMimicry(); },
                endResult,
                &guardReason);
            m_applyingRemoteEnemyAbilityFxEvent = false;
            ok = callOk && endResult;
        }
        m_lastEnemyFxEvent =
            std::string(ok
                ? (!isMimicking ? "remote_enemy_mimicry_end_current" : "remote_enemy_mimicry_end_applied")
                : "remote_enemy_mimicry_end_failed") +
            " net=" + std::to_string(packet.enemyNetId) +
            " entity=" + std::to_string(entity.GetId()) +
            " guard=" + (guardReason.empty() ? std::string("-") : guardReason);
        AppendEnemySyncTrace("enemy_fx", m_lastEnemyFxEvent);
        break;
    }
    case CoopProtocol::kEnemyAbilityFxPoltergeistLift:
        trigger(
            "Characters.Aliens.Phantom.Poltergeist.Cast.BodyCast_00",
            enemyFxPosition,
            0.85f,
            "remote enemy ability poltergeist lift cast");
        trigger(
            "Characters.Aliens.Poltergeist.Lift.WarmUp_00",
            impactFxPosition,
            0.70f,
            "remote enemy ability poltergeist lift warmup");
        queue(
            "Characters.Aliens.Poltergeist.Lift.LiftVortex",
            impactFxPosition,
            0.62f,
            0.08f,
            "remote enemy ability poltergeist lift vortex");
        break;
    case CoopProtocol::kEnemyAbilityFxPoltergeistThrow:
        trigger(
            "Characters.Aliens.Phantom.Poltergeist.Cast.BodyCast_00",
            enemyFxPosition,
            0.82f,
            "remote enemy ability poltergeist throw cast");
        trigger(
            "Characters.Aliens.Phantom.Poltergeist.Lift_00",
            impactFxPosition,
            0.64f,
            "remote enemy ability poltergeist throw lift");
        queue(
            "Characters.Aliens.Poltergeist.Lift.LiftCast",
            impactFxPosition,
            0.58f,
            0.08f,
            "remote enemy ability poltergeist throw pulse");
        break;
    case CoopProtocol::kEnemyAbilityFxWeaverCreateCystoid:
        trigger(
            "Characters.Aliens.Weaver.Cast.BodyCast",
            enemyFxPosition,
            0.72f,
            "remote enemy ability weaver create cystoid cast");
        break;
    case CoopProtocol::kEnemyAbilityFxWeaverAlarmCall:
        trigger(
            "Characters.Aliens.Weaver.Cast.BodyCast",
            enemyFxPosition,
            0.78f,
            "remote enemy ability weaver alarm call cast");
        break;
    case CoopProtocol::kEnemyAbilityFxPsiAttack:
        trigger(
            "Characters.AffectedFromPlayerPowers.Fear.npc_FearBomb",
            impactFxPosition,
            0.55f,
            "remote enemy ability psi attack generic");
        break;
    case CoopProtocol::kEnemyAbilityFxCystoidNestTrigger:
    {
        MarkNetworkConsumedCystoidNest(
            packet.enemyNetId,
            entity.GetId(),
            position,
            "remote cystoid nest ability");

        ArkCystoidNest* nest = GetArkCystoidNestExtensionFromEntity(&entity);
        if (nest)
        {
            std::string guardReason;
            EntityId forcedTarget = INVALID_ENTITYID;
            if (ArkPlayer::GetInstancePtr())
            {
                if (IEntity* playerEntity = ArkPlayer::GetInstance().GetEntity())
                    forcedTarget = playerEntity->GetId();
            }

            MarkNetworkOriginatedCystoidNestTriggerZone(
                position,
                "remote cystoid nest ForceTrigger pre");
            MarkNetworkOriginatedCystoidEntity(
                entity.GetId(),
                position,
                "remote cystoid nest entity");

            m_applyingRemoteEnemyAbilityFxEvent = true;
            ++m_remoteCystoidNestTriggerDepth;
            const bool triggerOk = TryGuardedVoidCall(
                "remote cystoid nest ForceTrigger",
                [nest, forcedTarget]()
                {
                    nest->ForceTrigger(forcedTarget != INVALID_ENTITYID ? static_cast<unsigned>(forcedTarget) : 0u);
                },
                &guardReason);
            if (m_remoteCystoidNestTriggerDepth > 0)
                --m_remoteCystoidNestTriggerDepth;
            m_applyingRemoteEnemyAbilityFxEvent = false;
            ok = triggerOk || ok;
            if (!triggerOk)
            {
                m_lastEnemyFxEvent =
                    "remote_enemy_cystoid_nest_trigger_native_failed net=" + std::to_string(packet.enemyNetId) +
                    " entity=" + std::to_string(entity.GetId()) +
                    " forcedTarget=" + std::to_string(forcedTarget) +
                    " reason=" + (guardReason.empty() ? std::string("-") : guardReason);
                AppendEnemySyncTrace("enemy_fx", m_lastEnemyFxEvent);
            }
            else
            {
                MarkNetworkOriginatedCystoidNestTriggerZone(
                    position,
                    "remote cystoid nest ForceTrigger post");
            }
        }

        ForceNetworkConsumedCystoidNestState(
            packet.enemyNetId,
            &entity,
            position,
            entity.GetWorldRotation(),
            "remote cystoid nest ability consumed");

        if (!ok)
        {
            trigger(
                "Characters.Aliens.Cystoid.Nest.CystoidSpawn",
                position,
                0.8f,
                "remote enemy ability cystoid nest trigger fallback");
        }
        break;
    }
    case CoopProtocol::kEnemyAbilityFxCystoidExplode:
    {
        ArkCystoid* cystoid = GetArkCystoidExtensionFromEntity(&entity);
        {
            std::string guardReason;
            m_applyingRemoteEnemyAbilityFxEvent = true;
            const bool explodeOk = cystoid
                ? TryGuardedVoidCall(
                    "remote cystoid Explode",
                    [cystoid]() { cystoid->Explode(); },
                    &guardReason)
                : RequestCystoidExplosionThroughManager(entity, &guardReason);
            m_applyingRemoteEnemyAbilityFxEvent = false;
            ok = explodeOk || ok;
            if (!explodeOk)
            {
                m_lastEnemyFxEvent =
                    "remote_enemy_cystoid_explode_native_failed net=" + std::to_string(packet.enemyNetId) +
                    " entity=" + std::to_string(entity.GetId()) +
                    " path=" + std::string(cystoid ? "extension" : "manager") +
                    " reason=" + (guardReason.empty() ? std::string("-") : guardReason);
                AppendEnemySyncTrace("enemy_fx", m_lastEnemyFxEvent);
            }
        }

        if (!ok)
        {
            trigger(
                "Characters.Aliens.Cystoid.Explosion.Explosion",
                position,
                0.85f,
                "remote enemy ability cystoid explode fallback");
        }
        break;
    }
    default:
        ++m_enemyBurstFxSkips;
        m_lastEnemyFxEvent =
            "remote_enemy_ability_fx_unknown net=" + std::to_string(packet.enemyNetId) +
            " entity=" + std::to_string(entity.GetId()) +
            " kind=" + std::to_string(packet.abilityKind);
        AppendEnemySyncTrace("enemy_fx", m_lastEnemyFxEvent);
        return false;
    }

    if (ok)
    {
        ++m_enemyBurstFxTriggers;
        m_lastEnemyFxEvent =
            "remote_enemy_ability_fx net=" + std::to_string(packet.enemyNetId) +
            " entity=" + std::to_string(entity.GetId()) +
            " kind=" + std::to_string(packet.abilityKind) +
            " seq=" + std::to_string(packet.sequence) +
            " mseq=" + std::to_string(packet.mannequinSequence) +
            " fragment=" + std::to_string(packet.mannequinFragmentId) +
            " reason=" + (reason && reason[0] ? reason : "-");
    }
    else
    {
        ++m_enemyBurstFxFailures;
        m_lastEnemyFxEvent =
            "remote_enemy_ability_fx_failed net=" + std::to_string(packet.enemyNetId) +
            " entity=" + std::to_string(entity.GetId()) +
            " kind=" + std::to_string(packet.abilityKind) +
            " seq=" + std::to_string(packet.sequence);
    }

    AppendEnemySyncTrace("enemy_fx", m_lastEnemyFxEvent);
    return ok;
}

bool ModMain::LocalPlayerHasEnemyAwarenessForCoop(const IEntity& enemy) const
{
    bool overrideValue = false;
    if (TryGetDebugEnemyAttentionOverride(enemy, overrideValue))
        return overrideValue;
    return LocalPlayerHasEnemyAwareness(enemy);
}

uint8_t ModMain::LocalPlayerEnemyAttentionLevelForCoop(const IEntity& enemy) const
{
    uint8_t overrideLevel = CoopEnemyAuthorityPolicy::kUnknownAttention;
    if (TryGetDebugEnemyAttentionLevelOverride(enemy, overrideLevel))
        return overrideLevel;
    return LocalPlayerEnemyAttentionLevel(enemy);
}

EntityId ModMain::ResolveLocallyRepresentedEnemyTarget(uint64_t accountToken) const
{
    if (accountToken == 0 || !gEnv || !gEnv->pEntitySystem)
        return INVALID_ENTITYID;

    if (accountToken == GetLocalAccountToken() && ArkPlayer::GetInstancePtr())
    {
        const IEntity* player = ArkPlayer::GetInstance().GetEntity();
        return player ? player->GetId() : INVALID_ENTITYID;
    }

    const auto peerIt = m_remotePeers.find(accountToken);
    if (peerIt == m_remotePeers.end() || peerIt->second.proxyEntityId == INVALID_ENTITYID)
        return INVALID_ENTITYID;
    return gEnv->pEntitySystem->GetEntity(peerIt->second.proxyEntityId)
        ? peerIt->second.proxyEntityId
        : INVALID_ENTITYID;
}

void ModMain::SyncRemoteEnemyPresentationTarget(
    EnemyAuthorityState& state,
    IEntity& enemy,
    uint64_t targetAccountToken,
    uint32_t mannequinSequence,
    const char* reason)
{
    const EntityId targetEntityId = ResolveLocallyRepresentedEnemyTarget(targetAccountToken);
    const bool targetChanged =
        state.remotePresentationTargetAccountToken != targetAccountToken ||
        state.remotePresentationTargetEntityId != targetEntityId;
    const bool actionChanged =
        mannequinSequence != 0 &&
        state.remotePresentationTargetMannequinSequence != mannequinSequence;
    if (!targetChanged && !actionChanged)
        return;

    const EntityId oldTargetEntityId = state.remotePresentationTargetEntityId;
    state.remotePresentationTargetAccountToken = targetAccountToken;
    state.remotePresentationTargetEntityId = targetEntityId;
    if (mannequinSequence != 0)
        state.remotePresentationTargetMannequinSequence = mannequinSequence;

    ArkNpc* npc = EntityUtils::GetArkNpc(&enemy);
    if (!npc || !CoopRuntimeGuards::IsLikelyRuntimeCppObject(npc, sizeof(void*) * 4))
        return;

    std::string guardReason;
    if (targetChanged &&
        oldTargetEntityId != INVALID_ENTITYID &&
        oldTargetEntityId != targetEntityId &&
        IsRemoteProxyEntity(oldTargetEntityId))
    {
        TryGuardedVoidCall(
            "remote enemy presentation target lost",
            [npc, oldTargetEntityId]() { npc->OnLostAttentionTarget(oldTargetEntityId, false); },
            &guardReason);
    }

    const bool targetIsRemoteProxy =
        targetEntityId != INVALID_ENTITYID &&
        IsRemoteProxyEntity(targetEntityId);
    if (!targetIsRemoteProxy)
        return;

    unsigned topTarget = INVALID_ENTITYID;
    TryGuardedCall(
        "remote enemy presentation target read top",
        [npc]() { return npc->GetTopAttentionTargetEntityId(); },
        topTarget,
        &guardReason);
    if (topTarget == targetEntityId)
        return;

    EntityId localPlayerEntityId = INVALID_ENTITYID;
    if (ArkPlayer::GetInstancePtr())
    {
        if (const IEntity* localPlayer = ArkPlayer::GetInstance().GetEntity())
            localPlayerEntityId = localPlayer->GetId();
    }

    const CoopEnemyControlPolicy::Decision controlDecision =
        CoopEnemyControlPolicy::Evaluate(BuildLocalEnemyControlPolicyContext(state, enemy));
    const bool preserveLocalCombatTarget =
        localPlayerEntityId != INVALID_ENTITYID &&
        controlDecision.remoteDriven &&
        !controlDecision.localVanillaAuthority &&
        controlDecision.preserveLocalCombat &&
        controlDecision.localFocus;

    // Exact authority actions may update the authority target, but must not replace
    // a native local attention edge. The awareness component can remain non-zero
    // while Vanilla briefly drops its top target between combat plans, so preserve
    // the real local player for the whole attentive interval rather than only while
    // it happens to be the current top target.
    if (localPlayerEntityId != INVALID_ENTITYID &&
        (topTarget == localPlayerEntityId || preserveLocalCombatTarget))
    {
        bool targetRestored = topTarget == localPlayerEntityId;
        if (!targetRestored)
        {
            if (topTarget != INVALID_ENTITYID && IsRemoteProxyEntity(topTarget))
            {
                TryGuardedVoidCall(
                    "remote enemy presentation evict proxy before local restore",
                    [npc, topTarget]() { npc->OnLostAttentionTarget(topTarget, false); },
                    &guardReason);
            }
            TryGuardedVoidCall(
                "remote enemy presentation restore local target",
                [npc, localPlayerEntityId]()
                {
                    ArkNpc::FOnNewAttentionTarget(npc, localPlayerEntityId, false);
                },
                &guardReason);
            unsigned restoredTopTarget = INVALID_ENTITYID;
            if (TryGuardedCall(
                    "remote enemy presentation reread restored top",
                    [npc]() { return npc->GetTopAttentionTargetEntityId(); },
                    restoredTopTarget,
                    &guardReason))
            {
                topTarget = restoredTopTarget;
                targetRestored = restoredTopTarget == localPlayerEntityId;
            }
        }
        m_lastEnemyAuthorityEvent =
            "preserved local native attention during remote enemy presentation sync net=" +
            std::to_string(state.netId) +
            " entity=" + std::to_string(enemy.GetId()) +
            " account=" + std::to_string(targetAccountToken) +
            " remoteTarget=" + std::to_string(targetEntityId) +
            " localTarget=" + std::to_string(localPlayerEntityId) +
            " restored=" + std::to_string(targetRestored ? 1 : 0) +
            " actionSeq=" + std::to_string(mannequinSequence) +
            " route=read_only_local_mix" +
            " reason=" + (reason && reason[0] ? std::string(reason) : std::string("-")) +
            (guardReason.empty() ? std::string() : " guard=" + guardReason);
        AppendEnemySyncTrace("attention", m_lastEnemyAuthorityEvent);
        return;
    }

    if (IEntity* targetEntity = gEnv->pEntitySystem->GetEntity(targetEntityId))
        RegisterProxyComplexAttention(*targetEntity);
    TryGuardedVoidCall(
        "remote enemy presentation target proxy update",
        [npc, targetEntityId]() { npc->OnAttentionProxyUpdated(targetEntityId); },
        &guardReason);
    const bool targetApplied = TryGuardedVoidCall(
        "remote enemy presentation target new target",
        [npc, targetEntityId]() { ArkNpc::FOnNewAttentionTarget(npc, targetEntityId, false); },
        &guardReason);

    m_lastEnemyAuthorityEvent =
        "synced remote enemy presentation target net=" + std::to_string(state.netId) +
        " entity=" + std::to_string(enemy.GetId()) +
        " account=" + std::to_string(targetAccountToken) +
        " target=" + std::to_string(targetEntityId) +
        " previousTop=" + std::to_string(topTarget) +
        " actionSeq=" + std::to_string(mannequinSequence) +
        " applied=" + std::to_string(targetApplied ? 1 : 0) +
        " reason=" + (reason && reason[0] ? std::string(reason) : std::string("-")) +
        (guardReason.empty() ? std::string() : " guard=" + guardReason);
    AppendEnemySyncTrace("attention", m_lastEnemyAuthorityEvent);
}


void ModMain::OnNativeNpcAttentionTargetChanged(
    ArkNpc* npc,
    EntityId targetEntityId,
    bool gained,
    bool delayed)
{
    if (!npc ||
        m_networkMode == CoopNetworkMode::Off ||
        !m_enemyAttentionAuthoritySyncEnabled ||
        !IsEnemyReplicationGameplayReady() ||
        !gEnv ||
        !gEnv->pEntitySystem ||
        !ArkPlayer::GetInstancePtr())
    {
        return;
    }

    IEntity* localPlayer = ArkPlayer::GetInstance().GetEntity();
    if (!localPlayer)
        return;

    EntityId enemyEntityId = INVALID_ENTITYID;
    if (!TryGuardedCall(
            "enemy attention edge GetEntityId",
            [npc]() { return npc->GetEntityId(); },
            enemyEntityId,
            nullptr) ||
        enemyEntityId == INVALID_ENTITYID ||
        IsRemoteProxyEntity(enemyEntityId))
    {
        return;
    }

    IEntity* enemy = gEnv->pEntitySystem->GetEntity(enemyEntityId);
    if (!enemy || !IsEnemyRuntimeControlCandidate(*enemy))
        return;

    EnemyAuthorityState* state = nullptr;
    if (const auto netIt = m_enemyNetIdsByEntity.find(enemyEntityId);
        netIt != m_enemyNetIdsByEntity.end())
    {
        state = FindEnemyAuthorityByNetId(netIt->second);
    }
    if (!state &&
        (m_networkMode == CoopNetworkMode::Host ||
            IsClientAreaAuthorityActive()))
    {
        state = &EnsureEnemyAuthorityState(*enemy);
    }
    if (!state)
        return;

    const EntityId localPlayerId = localPlayer->GetId();
    unsigned topTarget = INVALID_ENTITYID;
    TryGuardedCall(
        "enemy attention edge GetTopAttentionTargetEntityId",
        [npc]() { return npc->GetTopAttentionTargetEntityId(); },
        topTarget,
        nullptr);
    const bool targetIsLocalPlayer = targetEntityId == localPlayerId;
    bool localPlayerIsTopTarget = topTarget == localPlayerId;

    if (gained)
        ++m_enemyAttentionGainedEdges;
    else
        ++m_enemyAttentionLostEdges;
    if (targetIsLocalPlayer)
    {
        if (gained)
            ++m_enemyAttentionLocalGainedEdges;
        else
            ++m_enemyAttentionLocalLostEdges;
    }

    if (gained && targetIsLocalPlayer)
    {
        // The AI tree can promote and replace a target entirely between the
        // 20 Hz authority samples. Preserve the exact native edge long enough
        // for the normal claim/grant path to consume it; polling remains only
        // a save-load/already-attentive fallback.
        state->localNativeAttentionSeconds = kEnemyNativeAttentionEdgeGraceSeconds;

        // A previously installed authority-presentation proxy may still
        // compete with the real local player inside Vanilla's attention set.
        // Remove only that presentation target when Vanilla promotes the real
        // player. SyncRemoteEnemyPresentationTarget already preserves a local
        // top target, so subsequent authority snapshots cannot steal it back.
        const EntityId remotePresentationTargetEntityId =
            state->remotePresentationTargetEntityId;
        if (remotePresentationTargetEntityId != INVALID_ENTITYID &&
            remotePresentationTargetEntityId != localPlayerId &&
            IsRemoteProxyEntity(remotePresentationTargetEntityId))
        {
            TryGuardedVoidCall(
                "local enemy attention evict presentation proxy",
                [npc, remotePresentationTargetEntityId]()
                {
                    npc->OnLostAttentionTarget(remotePresentationTargetEntityId, false);
                });
        }
    }
    else if (!localPlayerIsTopTarget &&
        ((!gained && targetIsLocalPlayer) || (gained && !targetIsLocalPlayer)))
    {
        state->localNativeAttentionSeconds = 0.0f;
    }

    const CoopEnemyControlPolicy::Decision controlDecision =
        CoopEnemyControlPolicy::Evaluate(BuildLocalEnemyControlPolicyContext(*state, *enemy));
    if (controlDecision.remoteDriven && !controlDecision.localVanillaAuthority)
    {
        const bool preserveAttentiveLocalCombat =
            controlDecision.localFocus && controlDecision.preserveLocalCombat;
        if (preserveAttentiveLocalCombat && !localPlayerIsTopTarget)
        {
            // Plan transitions may report a transient loss of the real player and
            // then promote the authority owner's proxy. The awareness bar is the
            // authoritative local-attention signal; while it remains non-zero,
            // immediately restore the real local player so the next Vanilla combat
            // plan still selects victim-local attacks.
            if (topTarget != INVALID_ENTITYID && IsRemoteProxyEntity(topTarget))
            {
                TryGuardedVoidCall(
                    "local enemy attention evict promoted presentation proxy",
                    [npc, topTarget]() { npc->OnLostAttentionTarget(topTarget, false); });
            }
            TryGuardedVoidCall(
                "local enemy attention restore attentive player",
                [npc, localPlayerId]()
                {
                    ArkNpc::FOnNewAttentionTarget(npc, localPlayerId, false);
                });
            unsigned restoredTopTarget = INVALID_ENTITYID;
            if (TryGuardedCall(
                    "local enemy attention reread restored top",
                    [npc]() { return npc->GetTopAttentionTargetEntityId(); },
                    restoredTopTarget,
                    nullptr))
            {
                topTarget = restoredTopTarget;
                localPlayerIsTopTarget = restoredTopTarget == localPlayerId;
            }
            AppendEnemySyncTrace(
                "local_combat_target",
                "restored attentive local combat target"
                " net=" + std::to_string(state->netId) +
                    " entity=" + std::to_string(enemyEntityId) +
                    " eventTarget=" + std::to_string(targetEntityId) +
                    " local=" + std::to_string(localPlayerId) +
                    " restored=" + std::to_string(localPlayerIsTopTarget ? 1 : 0) +
                    " locomotion=authority combat=local");
        }

        const bool localAttentionWasActive = state->localReadOnlyAttentionActive;
        state->localReadOnlyAttentionActive =
            preserveAttentiveLocalCombat ||
            localPlayerIsTopTarget ||
            (gained && targetIsLocalPlayer);
        if (state->localReadOnlyAttentionActive)
        {
            // A remote-owned NPC keeps its Vanilla AI tree alive so it can
            // evaluate this process's real player, but the authority target's
            // mirrored combat state does not enter the observer's local combat
            // FSM. Promote the real native attention edge through Vanilla's
            // normal combat entry once. This opens local ability selection and
            // victim-local damage without creating movement or network damage.
            bool localCombatAlreadyActive = false;
            TryGuardedCall(
                "local enemy attention read combat state",
                [npc]() { return npc->m_bIsInCombat; },
                localCombatAlreadyActive,
                nullptr);
            if (!localCombatAlreadyActive)
            {
                const bool enteredLocalCombat = TryGuardedVoidCall(
                    "local enemy attention enter combat",
                    [npc]() { npc->OnCombatBegin(); });
                AppendEnemySyncTrace(
                    "local_combat",
                    "native local attention combat begin"
                    " net=" + std::to_string(state->netId) +
                        " entity=" + std::to_string(enemyEntityId) +
                        " target=" + std::to_string(localPlayerId) +
                        " applied=" + std::to_string(enteredLocalCombat ? 1 : 0) +
                        " locomotion=remote damage=victim_local");
            }

            state->localReadOnlyAttentionTargetEntityId = localPlayerId;
            state->localReadOnlyAttentionTargetPosition = localPlayer->GetWorldPos();
            state->localReadOnlyAttentionTargetPositionValid = true;
            state->localReadOnlyAttentionObservedAtSeconds = gEnv->pTimer
                ? gEnv->pTimer->GetAsyncCurTime()
                : state->localReadOnlyIntentObservedAtSeconds;
            RecordRemoteObserverLocalIntentSample(
                *state,
                *enemy,
                EnemyAuthorityState::ReadOnlyIntentAttention,
                CoopProtocol::kEnemyLocomotionFlagInCombat,
                0,
                localPlayerId,
                gained ? "ArkNpc::OnNewAttentionTarget" : "ArkNpc::OnLostAttentionTarget/top-retained",
                false);
        }
        else
        {
            state->localReadOnlyAttentionTargetEntityId = INVALID_ENTITYID;
            state->localReadOnlyAttentionTargetPositionValid = false;
            if (localAttentionWasActive)
            {
                bool localCombatActive = false;
                TryGuardedCall(
                    "local enemy attention loss read combat state",
                    [npc]() { return npc->m_bIsInCombat; },
                    localCombatActive,
                    nullptr);
                if (localCombatActive)
                {
                    const bool endedLocalCombat = TryGuardedVoidCall(
                        "local enemy attention leave combat",
                        [npc]() { npc->OnCombatEnd(); });
                    AppendEnemySyncTrace(
                        "local_combat",
                        "native local attention combat end"
                        " net=" + std::to_string(state->netId) +
                            " entity=" + std::to_string(enemyEntityId) +
                            " target=" + std::to_string(localPlayerId) +
                            " applied=" + std::to_string(endedLocalCombat ? 1 : 0) +
                            " locomotion=remote damage=victim_local");
                }
            }
        }
    }

    m_lastEnemyAuthorityEvent =
        std::string("native attention ") + (gained ? "gained" : "lost") +
        " net=" + std::to_string(state->netId) +
        " entity=" + std::to_string(enemyEntityId) +
        " target=" + std::to_string(targetEntityId) +
        " local=" + std::to_string(targetIsLocalPlayer ? 1 : 0) +
        " top=" + std::to_string(topTarget) +
        " hint=" + std::to_string(state->localNativeAttentionSeconds) +
        " delayed=" + std::to_string(delayed ? 1 : 0);
    AppendEnemySyncTrace("attention", m_lastEnemyAuthorityEvent);
}

CoopEnemyControlPolicy::Context ModMain::BuildLocalEnemyControlPolicyContext(
    const EnemyAuthorityState& state,
    const IEntity& entity) const
{
    CoopEnemyControlPolicy::Context context;
    switch (m_networkMode)
    {
    case CoopNetworkMode::Host:
        context.networkMode = CoopEnemyControlPolicy::NetworkMode::Host;
        break;
    case CoopNetworkMode::Client:
        context.networkMode = CoopEnemyControlPolicy::NetworkMode::Client;
        break;
    case CoopNetworkMode::Off:
    default:
        context.networkMode = CoopEnemyControlPolicy::NetworkMode::Off;
        break;
    }

    context.localAuthorityBlocked = m_localPlayerDowned || IsLocalPlayerAuthorityBlockedByModalState();
    context.localHasAttention = !context.localAuthorityBlocked && LocalPlayerHasEnemyAwarenessForCoop(entity);
    context.localAttentionClaimed = state.localAttentionClaimed;
    context.localLeaseOwner =
        state.authorityOwnerAccountToken != 0 &&
        state.authorityOwnerAccountToken == GetLocalAccountToken();
    context.localRotationOverrideActive = state.localRotationOverrideSeconds > 0.0f;
    context.remoteLocomotionAuthority = state.remoteLocomotionAuthority;
    context.remoteAuthorityHasAttention = state.remoteAuthorityHasAttention;
    context.remoteTargetsLocalPlayer =
        state.remoteTargetAccountToken != 0 &&
        state.remoteTargetAccountToken == GetLocalAccountToken();
    context.remoteTargetLocallyRepresented =
        ResolveLocallyRepresentedEnemyTarget(state.remoteTargetAccountToken) != INVALID_ENTITYID;
    context.hasLastPosition = state.hasLastPosition;
    context.localTargetMixEnabled = true;
    return context;
}

void ModMain::ResetEnemySemanticReplicationState(EnemyAuthorityState& state)
{
    state.pendingSemanticContextId = 0;
    state.pendingSemanticTargetEntityId = INVALID_ENTITYID;
    state.pendingSemanticObservedAtSeconds = -1000.0f;
    state.pendingSemanticVariant = 0;
    state.pendingSemanticAction = nullptr;
    state.pendingSemanticFragmentId = -1;
    state.localSemanticContextId = 0;
    state.localSemanticSequence = 0;
    state.localSemanticBoundAtSeconds = -1000.0f;
    state.localSemanticVariant = 0;
    state.localSemanticLastContextId = 0;
    state.localSemanticLastSequence = 0;
    state.localSemanticLastVariant = 0;
    state.localSemanticLastFragmentId = -1;
    state.localAuthoritySemanticSequence = 0;
    state.remoteAuthoritySemanticSequence = 0;
    state.remoteSemanticContextId = 0;
    state.remoteSemanticSequence = 0;
    state.remoteSemanticVariant = 0;
    state.localLocomotionSemanticSeconds = 0.0f;
    state.localLocomotionSemanticContextId = 0;
    state.localLocomotionSemanticSequence = 0;
    state.localLocomotionSemanticVariant = 0;
    state.localLocomotionSemanticLastContextId = 0;
    state.localLocomotionSemanticLastSequence = 0;
    state.localLocomotionSemanticLastVariant = 0;
    state.remoteLocomotionSemanticContextId = 0;
    state.remoteLocomotionSemanticSequence = 0;
    state.remoteLocomotionSemanticVariant = 0;
    state.remoteLocomotionSemanticAppliedContextId = 0;
    state.remoteLocomotionSemanticAppliedSequence = 0;
    state.remoteLocomotionSemanticAppliedVariant = 0;
    state.localPresentationSemanticSeconds = 0.0f;
    state.localPresentationSemanticContextId = 0;
    state.localPresentationSemanticSequence = 0;
    state.localPresentationSemanticVariant = 0;
    state.localPresentationSemanticLastContextId = 0;
    state.localPresentationSemanticLastSequence = 0;
    state.localPresentationSemanticLastVariant = 0;
    state.remotePresentationSemanticContextId = 0;
    state.remotePresentationSemanticSequence = 0;
    state.remotePresentationSemanticVariant = 0;
    state.localPresentationSemanticNativeOutcome = CoopProtocol::kEnemySemanticNativeOutcomeAbilityManager;
    state.localPresentationSemanticLastNativeOutcome = CoopProtocol::kEnemySemanticNativeOutcomeAbilityManager;
    state.remotePresentationSemanticNativeOutcome = CoopProtocol::kEnemySemanticNativeOutcomeAbilityManager;
}


void ModMain::ClearRemoteEnemyPresentationForLocalAuthority(
    EnemyAuthorityState& state,
    IEntity& entity,
    const char* reason)
{
    // Authority takeover must be a pure policy transition. StopAnimation or
    // ResetAnimation here also resets controller-owned state on spawned NPCs,
    // leaving the new Vanilla owner able to store an ability action without
    // ever starting it. Releasing the exact remote action leases is enough;
    // the new owner resumes the untouched native controller.
    state.remoteMannequinSequence = 0;
    state.remoteMannequinFragmentId = -1;
    state.remoteMannequinOrdinal = CoopProtocol::kInvalidMannequinOrdinal;
    state.remoteMannequinFlags = 0;
    state.remoteMannequinAttackKind = 0;
    state.remoteMannequinPriority = 0;
    state.remoteMannequinTagState.fill(0);
    state.remoteMannequinTagStateValid = false;
    state.remoteMannequinRandomOption = false;
    state.remoteMannequinCarryMovement = false;
    state.remoteNativeMannequinActions.clear();
    state.remoteNativeMannequinRetiredOrder.clear();
    state.remoteNativeMannequinRetiredActions.clear();
    state.remoteNativeMirrorQueuedSequence = 0;
    state.remoteNativeMirrorDiagnosedSequence = 0;
    state.remoteNativeMirrorSuppressedSequence = 0;
    state.remoteNativeMirrorRepairSequence = 0;
    state.remoteNativeMirrorRepairWaitSeconds = 0.0f;
    state.remoteMannequinStateSeconds = 0.0f;
    state.remoteMovementHoldSeconds = 0.0f;
    state.remoteTargetMotionSeconds = 0.0f;
    state.remoteTargetMotionFlags = 0;
    state.remoteVisualMotionSeconds = 0.0f;
    state.remoteVisualMotionFlags = 0;
    state.remoteActionMotionBlockSeconds = 0.0f;
    if (state.remoteAuthorityRagdollApplied)
    {
        if (ArkNpc* npc = EntityUtils::GetArkNpc(&entity))
        {
            bool cleared = false;
            if (TryGuardedCall(
                    "remote enemy authority transition PopIndefiniteRagdoll",
                    [npc]() { return npc->PopIndefiniteRagdoll(); },
                    cleared,
                    nullptr) &&
                cleared)
            {
                ++m_remoteEnemyAuthorityRagdollClears;
            }
            else
            {
                ++m_remoteEnemyAuthorityRagdollFailures;
            }
        }
        state.remoteAuthorityRagdollApplied = false;
    }
    ResetEnemySemanticReplicationState(state);

    m_lastEnemyMannequinStateEvent =
        "cleared_remote_presentation_for_local_authority net=" + std::to_string(state.netId) +
        " entity=" + std::to_string(entity.GetId()) +
        " exactNativeActions=cleared" +
        " reason=" + (reason && reason[0] ? std::string(reason) : std::string("-"));
    AppendEnemySyncTrace("remote_anim", m_lastEnemyMannequinStateEvent);
}

bool ModMain::InterruptEnemyAbilityForAuthorityTransition(
    EnemyAuthorityState& state,
    IEntity& entity,
    const char* reason)
{
    ArkNpc* npc = EntityUtils::GetArkNpc(&entity);
    if (!npc ||
        !CoopRuntimeGuards::IsLikelyRuntimeCppObject(npc, sizeof(void*) * 4))
    {
        return false;
    }

    bool dead = false;
    bool performingBefore = false;
    uint64_t contextBefore = 0;
    std::string guardReason;
    if (!TryGuardedCall(
            "enemy authority transition IsDead",
            [npc]() { return npc->IsDead(); },
            dead,
            &guardReason) ||
        dead ||
        !TryGuardedCall(
            "enemy authority transition IsPerformingAbility",
            [npc]() { return npc->IsPerformingAbility(); },
            performingBefore,
            &guardReason) ||
        !performingBefore)
    {
        return false;
    }

    TryGuardedCall(
        "enemy authority transition GetCurrentAbilityContextId",
        [npc]() { return npc->GetCurrentAbilityContextId(); },
        contextBefore,
        &guardReason);

    bool nativeResult = false;
    const bool invoked = TryGuardedCall(
        "enemy authority transition InterruptAbility",
        [npc]() { return npc->InterruptAbility(); },
        nativeResult,
        &guardReason);
    bool performingAfter = performingBefore;
    uint64_t contextAfter = contextBefore;
    TryGuardedCall(
        "enemy authority transition reread IsPerformingAbility",
        [npc]() { return npc->IsPerformingAbility(); },
        performingAfter,
        &guardReason);
    TryGuardedCall(
        "enemy authority transition reread GetCurrentAbilityContextId",
        [npc]() { return npc->GetCurrentAbilityContextId(); },
        contextAfter,
        &guardReason);

    AppendEnemySyncTrace(
        "authority_ability",
        "interrupted pre-handoff native ability"
        " net=" + std::to_string(state.netId) +
            " entity=" + std::to_string(entity.GetId()) +
            " context=" + std::to_string(contextBefore) +
            "->" + std::to_string(contextAfter) +
            " performing=" + std::to_string(performingBefore ? 1 : 0) +
            "->" + std::to_string(performingAfter ? 1 : 0) +
            " invoked=" + std::to_string(invoked ? 1 : 0) +
            " nativeResult=" + std::to_string(nativeResult ? 1 : 0) +
            " reason=" + (reason && reason[0] ? std::string(reason) : std::string("-")) +
            (guardReason.empty() ? std::string() : " guard=" + guardReason));
    return invoked && !performingAfter;
}

void ModMain::RestoreLocalEnemyVanillaAuthority(
    EnemyAuthorityState& state,
    IEntity& entity,
    const char* reason)
{
    ClearRemoteEnemyPresentationForLocalAuthority(state, entity, reason);

    ArkNpc* npc = EntityUtils::GetArkNpc(&entity);
    if (!npc ||
        !CoopRuntimeGuards::IsLikelyRuntimeCppObject(npc, sizeof(void*) * 4) ||
        !ArkPlayer::GetInstancePtr())
    {
        return;
    }

    bool dead = false;
    if (!TryGuardedCall(
            "local enemy authority restore IsDead",
            [npc]() { return npc->IsDead(); },
            dead,
            nullptr) ||
        dead)
    {
        return;
    }

    const bool interruptedAbility = InterruptEnemyAbilityForAuthorityTransition(
        state,
        entity,
        reason && reason[0] ? reason : "restore local Vanilla authority");

    const EntityId stalePresentationTarget = state.remotePresentationTargetEntityId;
    state.remotePresentationTargetAccountToken = 0;
    state.remotePresentationTargetEntityId = INVALID_ENTITYID;
    state.remotePresentationTargetMannequinSequence = 0;
    state.localReadOnlyAttentionActive = false;
    state.localReadOnlyAttentionTargetEntityId = INVALID_ENTITYID;
    state.localReadOnlyAttentionTargetPositionValid = false;

    std::string guardReason;
    bool retiredRemoteTarget = false;
    if (stalePresentationTarget != INVALID_ENTITYID &&
        stalePresentationTarget != entity.GetId() &&
        IsRemoteProxyEntity(stalePresentationTarget))
    {
        retiredRemoteTarget = TryGuardedVoidCall(
            "local enemy authority retire remote presentation target",
            [npc, stalePresentationTarget]()
            {
                npc->OnLostAttentionTarget(stalePresentationTarget, false);
            },
            &guardReason);
    }

    IEntity* localPlayer = ArkPlayer::GetInstance().GetEntity();
    const EntityId localPlayerId = localPlayer ? localPlayer->GetId() : INVALID_ENTITYID;
    unsigned topTarget = INVALID_ENTITYID;
    bool inCombatBefore = false;
    TryGuardedCall(
        "local enemy authority read top attention target",
        [npc]() { return npc->GetTopAttentionTargetEntityId(); },
        topTarget,
        &guardReason);
    TryGuardedCall(
        "local enemy authority read combat state",
        [npc]() { return npc->m_bIsInCombat; },
        inCombatBefore,
        &guardReason);

    bool endedStaleCombatPlan = false;
    bool replayedLocalAttention = false;
    bool beganCombatFallback = false;
    bool inCombatAfter = inCombatBefore;
    if (localPlayerId != INVALID_ENTITYID &&
        topTarget == localPlayerId)
    {
        // The observer can already be marked in combat while its AI tree is
        // parked in an authority-owned MoveTo/SideStep branch. End that stale
        // plan before replaying the real target so the new owner enters a fresh
        // Vanilla combat selection rather than inheriting the observer branch.
        if (inCombatBefore)
        {
            endedStaleCombatPlan = TryGuardedVoidCall(
                "local enemy authority end stale combat plan",
                [npc]() { npc->OnCombatEnd(); },
                &guardReason);
        }
        replayedLocalAttention = TryGuardedVoidCall(
            "local enemy authority replay local attention",
            [npc, localPlayerId]()
            {
                ArkNpc::FOnNewAttentionTarget(npc, localPlayerId, false);
            },
            &guardReason);
        TryGuardedCall(
            "local enemy authority reread combat state",
            [npc]() { return npc->m_bIsInCombat; },
            inCombatAfter,
            &guardReason);
        if (!inCombatAfter)
        {
            beganCombatFallback = TryGuardedVoidCall(
                "local enemy authority native combat begin fallback",
                [npc]() { npc->OnCombatBegin(); },
                &guardReason);
            TryGuardedCall(
                "local enemy authority final combat state",
                [npc]() { return npc->m_bIsInCombat; },
                inCombatAfter,
                &guardReason);
        }
    }

    m_lastEnemyAuthorityEvent =
        "restored local Vanilla enemy authority net=" + std::to_string(state.netId) +
        " entity=" + std::to_string(entity.GetId()) +
        " staleTarget=" + std::to_string(stalePresentationTarget) +
        " retired=" + std::to_string(retiredRemoteTarget ? 1 : 0) +
        " localTarget=" + std::to_string(localPlayerId) +
        " top=" + std::to_string(topTarget) +
        " combat=" + std::to_string(inCombatBefore ? 1 : 0) +
        "->" + std::to_string(inCombatAfter ? 1 : 0) +
        " interruptedAbility=" + std::to_string(interruptedAbility ? 1 : 0) +
        " endedPlan=" + std::to_string(endedStaleCombatPlan ? 1 : 0) +
        " replay=" + std::to_string(replayedLocalAttention ? 1 : 0) +
        " fallback=" + std::to_string(beganCombatFallback ? 1 : 0) +
        " reason=" + (reason && reason[0] ? std::string(reason) : std::string("-")) +
        (guardReason.empty() ? std::string() : " guard=" + guardReason);
    AppendEnemySyncTrace("authority_restore", m_lastEnemyAuthorityEvent);
}

void ModMain::RetargetLocalFocusedOperatorLaserForHook(
    ArkOperatorLaserHelper* helper,
    ArkNpc* npc)
{
    ++m_operatorLaserUpdates;
    if (!helper || !npc || m_networkMode == CoopNetworkMode::Off)
    {
        ++m_operatorLaserTargetSkips;
        return;
    }
    if (helper->m_stage == ArkOperatorLaserHelper::Stage::Off)
    {
        ++m_operatorLaserTargetSkips;
        return;
    }
    ++m_operatorLaserActiveUpdates;

    IEntity* enemyEntity = nullptr;
    std::string reason;
    if (!TryGuardedCall(
            "operator laser ArkNpc::GetEntity",
            [npc]() -> IEntity* { return npc->GetEntity(); },
            enemyEntity,
            &reason) ||
        !enemyEntity)
    {
        ++m_operatorLaserTargetSkips;
        m_lastOperatorLaserEvent = "skip_enemy";
        return;
    }

    EntityId enemyEntityId = INVALID_ENTITYID;
    if (!TryGuardedCall(
            "operator laser enemy GetId",
            [enemyEntity]() { return enemyEntity->GetId(); },
            enemyEntityId,
            &reason))
    {
        ++m_operatorLaserTargetSkips;
        return;
    }

    const auto netIt = m_enemyNetIdsByEntity.find(enemyEntityId);
    EnemyAuthorityState* state = netIt == m_enemyNetIdsByEntity.end()
        ? nullptr
        : FindEnemyAuthorityByNetId(netIt->second);
    if (!state || state->entityId != enemyEntityId)
    {
        ++m_operatorLaserTargetSkips;
        m_lastOperatorLaserEvent =
            "active_missing_state_entity_" + std::to_string(enemyEntityId) +
            "_stage_" + std::to_string(static_cast<int>(helper->m_stage));
        return;
    }

    const CoopEnemyControlPolicy::Decision decision =
        CoopEnemyControlPolicy::Evaluate(BuildLocalEnemyControlPolicyContext(*state, *enemyEntity));
    if (decision.mode != CoopEnemyControlPolicy::EnemyControlMode::LocalTargetRemoteOwner ||
        !decision.preserveLocalCombat)
    {
        ++m_operatorLaserTargetSkips;
        if (m_operatorLaserActiveUpdates <= 4 || (m_operatorLaserActiveUpdates % 120u) == 0)
        {
            m_lastOperatorLaserEvent =
                "active_skip_net_" + std::to_string(state->netId) +
                "_entity_" + std::to_string(enemyEntityId) +
                "_stage_" + std::to_string(static_cast<int>(helper->m_stage)) +
                "_mode_" + CoopEnemyControlPolicy::ModeName(decision.mode);
        }
        return;
    }

    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    IEntity* playerEntity = player ? player->GetEntity() : nullptr;
    if (!playerEntity)
    {
        ++m_operatorLaserTargetSkips;
        return;
    }

    AABB playerBounds(AABB::RESET);
    if (!TryGuardedVoidCall(
            "operator laser player GetWorldBounds",
            [playerEntity, &playerBounds]() { playerEntity->GetWorldBounds(playerBounds); },
            &reason) ||
        playerBounds.IsReset())
    {
        ++m_operatorLaserTargetSkips;
        return;
    }

    const Vec3 localTarget = playerBounds.GetCenter();
    if (!TryWriteRuntimeValue(&helper->m_laserTarget, localTarget))
    {
        ++m_operatorLaserTargetSkips;
        m_lastOperatorLaserEvent = "skip_target_write";
        return;
    }

    ++m_operatorLaserTargetOverrides;
    if (m_operatorLaserTargetOverrides <= 4 || (m_operatorLaserTargetOverrides % 120u) == 0)
    {
        m_lastOperatorLaserEvent =
            "override_net_" + std::to_string(state->netId) +
            "_entity_" + std::to_string(enemyEntityId);
    }
}

EntityId ModMain::ResolveRemoteDrivenEnemyLocalCombatTarget(
    void* npcPtr,
    const char* stage,
    uint64_t contextId,
    EntityId requestedTargetEntityId)
{
    if (!npcPtr ||
        !gEnv ||
        !gEnv->pEntitySystem ||
        m_networkMode == CoopNetworkMode::Off ||
        !m_enemyLocomotionSyncEnabled ||
        !IsEnemyReplicationGameplayReady())
    {
        return requestedTargetEntityId;
    }

    const std::string_view stageView(stage && stage[0] ? stage : "");
    const CoopEnemyIntentGate::AbilityOwnership ownership =
        CoopEnemyIntentGate::ClassifyAbilityOwnership(contextId);
    const bool localDecisionBoundary =
        stageView.find("TryPerformAnyAbility") != std::string_view::npos ||
        stageView.find("TryPerformAbilityContext") != std::string_view::npos ||
        stageView.find("TryEvaluateAndPerformAbilityContext") != std::string_view::npos ||
        stageView.find("OnAttack") != std::string_view::npos ||
        ownership == CoopEnemyIntentGate::AbilityOwnership::LocalCombat ||
        ownership == CoopEnemyIntentGate::AbilityOwnership::AuthorityMovementLocalCombat;
    if (!localDecisionBoundary)
        return requestedTargetEntityId;

    // A prop, another NPC or a world target belongs to the authored ability.
    // Only replace the remote-player proxy that represents the authority
    // owner's target in this process. TryPerformAnyAbility is deliberately
    // and every explicit context wrapper are included before native prerequisite
    // evaluation so Vanilla evaluates the decision against this process's real
    // player. Movement ownership is still enforced later at Ability::Perform.
    if (requestedTargetEntityId == INVALID_ENTITYID ||
        !IsRemoteProxyEntity(requestedTargetEntityId))
    {
        return requestedTargetEntityId;
    }

    auto* npc = reinterpret_cast<ArkNpc*>(npcPtr);
    if (!CoopRuntimeGuards::IsLikelyRuntimeCppObject(npc, sizeof(void*) * 4))
        return requestedTargetEntityId;

    EntityId enemyEntityId = INVALID_ENTITYID;
    if (!TryGuardedCall(
            "local combat target GetEntityId",
            [npc]() { return npc->GetEntityId(); },
            enemyEntityId,
            nullptr) ||
        enemyEntityId == INVALID_ENTITYID)
    {
        return requestedTargetEntityId;
    }

    IEntity* enemyEntity = gEnv->pEntitySystem->GetEntity(enemyEntityId);
    const auto netIt = m_enemyNetIdsByEntity.find(enemyEntityId);
    EnemyAuthorityState* state = netIt == m_enemyNetIdsByEntity.end()
        ? nullptr
        : FindEnemyAuthorityByNetId(netIt->second);
    if (!enemyEntity || !state)
        return requestedTargetEntityId;

    const CoopEnemyControlPolicy::Context controlContext =
        BuildLocalEnemyControlPolicyContext(*state, *enemyEntity);
    const CoopEnemyControlPolicy::Decision decision =
        CoopEnemyControlPolicy::Evaluate(controlContext);
    if (!controlContext.localHasAttention ||
        decision.mode != CoopEnemyControlPolicy::EnemyControlMode::LocalTargetRemoteOwner ||
        !decision.preserveLocalCombat)
    {
        return requestedTargetEntityId;
    }

    ArkPlayer* localPlayer = ArkPlayer::GetInstancePtr();
    IEntity* localPlayerEntity = localPlayer ? localPlayer->GetEntity() : nullptr;
    if (!localPlayerEntity)
        return requestedTargetEntityId;

    const EntityId localPlayerEntityId = localPlayerEntity->GetId();
    ++state->localCombatTargetRetargets;
    if (state->localCombatTargetRetargets <= 4 ||
        (state->localCombatTargetRetargets % 120u) == 0)
    {
        AppendEnemySyncTrace(
            "local_combat_target",
            "retargeted local combat from authority proxy"
            " net=" + std::to_string(state->netId) +
                " entity=" + std::to_string(enemyEntityId) +
                " context=" + std::to_string(contextId) +
                " requested=" + std::to_string(requestedTargetEntityId) +
                " local=" + std::to_string(localPlayerEntityId) +
                " stage=" + std::string(stageView) +
                " locomotion=authority combat=local");
    }
    return localPlayerEntityId;
}

bool ModMain::ShouldSuppressRemoteOperatorLaserDamage(ArkNpc* npc)
{
    if (!npc || m_networkMode == CoopNetworkMode::Off || !gEnv || !gEnv->pEntitySystem)
        return false;

    EntityId entityId = INVALID_ENTITYID;
    if (!TryGuardedCall(
            "operator laser damage GetEntityId",
            [npc]() { return npc->GetEntityId(); },
            entityId,
            nullptr) ||
        entityId == INVALID_ENTITYID)
    {
        return false;
    }

    const auto netIt = m_enemyNetIdsByEntity.find(entityId);
    EnemyAuthorityState* state = netIt == m_enemyNetIdsByEntity.end()
        ? nullptr
        : FindEnemyAuthorityByNetId(netIt->second);
    IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId);
    if (!state || !entity)
        return false;

    const CoopEnemyControlPolicy::Decision decision =
        CoopEnemyControlPolicy::Evaluate(BuildLocalEnemyControlPolicyContext(*state, *entity));
    if (decision.localVanillaAuthority ||
        !decision.remoteDriven ||
        (decision.mode == CoopEnemyControlPolicy::EnemyControlMode::LocalTargetRemoteOwner &&
            decision.preserveLocalCombat))
        return false;

    ++m_operatorLaserRemoteDamageSuppressions;
    if (m_operatorLaserRemoteDamageSuppressions <= 4 ||
        (m_operatorLaserRemoteDamageSuppressions % 120u) == 0)
    {
        m_lastOperatorLaserEvent =
            "suppressed_remote_damage_net_" + std::to_string(state->netId) +
            "_entity_" + std::to_string(entityId) +
            "_mode_" + CoopEnemyControlPolicy::ModeName(decision.mode);
    }
    return true;
}

uint64_t ModMain::CaptureNativeCorpsePhantomSourceStableId(EntityId corpseEntityId, uint64_t* outSourceEnemyNetId)
{
    ++m_corpsePhantomUpdates;
    if (outSourceEnemyNetId)
        *outSourceEnemyNetId = 0;
    if (corpseEntityId == INVALID_ENTITYID || !gEnv || !gEnv->pEntitySystem)
        return 0;

    if (outSourceEnemyNetId)
    {
        const auto netIt = m_enemyNetIdsByEntity.find(corpseEntityId);
        if (netIt != m_enemyNetIdsByEntity.end())
            *outSourceEnemyNetId = netIt->second;
    }

    IEntity* corpse = nullptr;
    if (!TryGuardedCall(
            "corpse phantom source GetEntity",
            [corpseEntityId]() { return gEnv->pEntitySystem->GetEntity(corpseEntityId); },
            corpse,
            nullptr) ||
        !corpse)
    {
        return 0;
    }

    uint64_t archetypeId = 0;
    uint64_t nativeGuid = 0;
    IEntityArchetype* archetype = nullptr;
    TryGuardedCall("corpse phantom source GetArchetype", [corpse]() { return corpse->GetArchetype(); }, archetype, nullptr);
    if (archetype)
        TryGuardedCall("corpse phantom source archetype id", [archetype]() { return archetype->GetId(); }, archetypeId, nullptr);
    TryGuardedCall("corpse phantom source guid", [corpse]() -> EntityGUID { return corpse->GetGuid(); }, nativeGuid, nullptr);
    return ResolveEnemyStableId(*corpse, archetypeId, nativeGuid);
}

void ModMain::OnNativeCorpsePhantomUpdateResult(
    EntityId corpseEntityId,
    EntityId previousPhantomEntityId,
    EntityId phantomEntityId,
    uint64_t phantomArchetypeId,
    uint64_t sourceEnemyNetId,
    uint64_t sourceStableEnemyId,
    bool completed)
{
    if (phantomEntityId == INVALID_ENTITYID ||
        phantomEntityId == previousPhantomEntityId ||
        sourceStableEnemyId == 0 ||
        phantomArchetypeId == 0 ||
        m_networkMode == CoopNetworkMode::Off)
    {
        return;
    }

    const uint64_t childStableId = BuildCorpsePhantomStableId(sourceStableEnemyId, phantomArchetypeId);

    m_enemyStableSpawnIdsByEntity[phantomEntityId] = childStableId;
    m_enemyRaisedFromCorpseSourcesByEntity[phantomEntityId] = sourceStableEnemyId;
    m_pendingEnemyRegistryEntityIds.insert(phantomEntityId);
    // The lifecycle queue already identifies the exact new entity. Re-arming
    // the full level scan here made corpse initialization in a newly entered
    // area rescan every entity several times per second.
    ++m_corpsePhantomResults;
    m_lastCorpsePhantomEvent =
        "native_result_corpse_" + std::to_string(corpseEntityId) +
        "_source_" + std::to_string(sourceStableEnemyId) +
        "_phantom_" + std::to_string(phantomEntityId) +
        "_stable_" + std::to_string(childStableId) +
        "_archetype_" + std::to_string(phantomArchetypeId) +
        "_completed_" + std::to_string(completed ? 1 : 0);

    if (m_networkMode == CoopNetworkMode::Client &&
        !IsClientAreaAuthorityActive() &&
        IsEnemyReplicationGameplayReady() &&
        m_hasRemoteEndpoint &&
        m_socket != kInvalidNetworkSocket &&
        sourceEnemyNetId != 0)
    {
        CoopProtocol::CorpsePhantomRequestPacket packet = {};
        packet.sequence = CoopSerialSequence::Advance(m_corpsePhantomRequestSequence);
        packet.areaId = m_localLevelId;
        packet.sourceEnemyNetId = sourceEnemyNetId;
        packet.sourceStableEnemyId = sourceStableEnemyId;
        packet.phantomArchetypeId = phantomArchetypeId;
        packet.childStableEnemyId = childStableId;
        if (SendCorpsePhantomRequestTo(
                packet,
                m_remoteAddress,
                m_remotePort,
                "corpse phantom request send failed"))
        {
            ++m_corpsePhantomRequestsSent;
            m_lastCorpsePhantomEvent += "_request_" + std::to_string(packet.sequence);
        }
    }
}

void ModMain::TickDebugCorpsePhantom(float frameTime)
{
    if (!m_debugCorpsePhantomPending)
        return;

    m_debugCorpsePhantomSeconds += std::max(frameTime, 0.0f);
    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    IArkPsiPower* rawPower = player
        ? player->GetPsiPowerComponent().GetIArkPsiPower(EArkPsiPowers::createPhantom)
        : nullptr;
    ArkPsiPowerCreatePhantom* createPower = static_cast<ArkPsiPowerCreatePhantom*>(rawPower);
    if (!createPower || m_debugCorpsePhantomSeconds > 15.0f)
    {
        m_debugCorpsePhantomPending = false;
        m_lastCorpsePhantomEvent = createPower
            ? "debug_update_timeout"
            : "debug_update_missing_power";
        return;
    }

    bool updateHandled = false;
    std::string reason;
    const bool updateOk = TryGuardedCall(
        "debug corpse phantom Update",
        [createPower, frameTime]()
        {
            return createPower->m_currentCorpsePhantom.Update(std::max(frameTime, 0.0f));
        },
        updateHandled,
        &reason);
    if (!updateOk)
    {
        m_debugCorpsePhantomPending = false;
        m_lastCorpsePhantomEvent = "debug_update_guard";
        return;
    }

    EntityId phantomEntityId = INVALID_ENTITYID;
    TryGuardedCall(
        "debug corpse phantom result id",
        [createPower]() { return createPower->m_currentCorpsePhantom.m_phantomEntityId; },
        phantomEntityId,
        nullptr);
    if (m_corpsePhantomResults > m_debugCorpsePhantomResultBaseline ||
        phantomEntityId != INVALID_ENTITYID)
    {
        m_debugCorpsePhantomPending = false;
    }
}

bool ModMain::BeginNativeCorpsePhantomSpawn(
    EntityId sourceEntityId,
    uint64_t phantomArchetypeId,
    const char* context)
{
    if (m_debugCorpsePhantomPending ||
        sourceEntityId == INVALID_ENTITYID ||
        phantomArchetypeId == 0 ||
        !gEnv ||
        !gEnv->pEntitySystem)
    {
        return false;
    }

    IEntity* sourceEntity = nullptr;
    IEntityArchetype* phantomArchetype = nullptr;
    TryGuardedCall(
        "corpse phantom begin GetEntity",
        [sourceEntityId]() { return gEnv->pEntitySystem->GetEntity(sourceEntityId); },
        sourceEntity,
        nullptr);
    TryGuardedCall(
        "corpse phantom begin GetArchetype",
        [phantomArchetypeId]() { return gEnv->pEntitySystem->GetEntityArchetype(phantomArchetypeId); },
        phantomArchetype,
        nullptr);

    ArkNpc* sourceNpc = sourceEntity ? EntityUtils::GetArkNpc(sourceEntity) : nullptr;
    bool sourceDead = false;
    if (sourceNpc)
        TryGuardedCall("corpse phantom begin IsDead", [sourceNpc]() { return sourceNpc->IsDead(); }, sourceDead, nullptr);

    ArkPlayer* player = ArkPlayer::GetInstancePtr();
    IArkPsiPower* rawPower = player
        ? player->GetPsiPowerComponent().GetIArkPsiPower(EArkPsiPowers::createPhantom)
        : nullptr;
    ArkPsiPowerCreatePhantom* createPower = static_cast<ArkPsiPowerCreatePhantom*>(rawPower);
    if (!sourceEntity || !sourceNpc || !sourceDead || !phantomArchetype || !createPower)
    {
        m_lastCorpsePhantomEvent =
            std::string(context ? context : "corpse_phantom") +
            "_start_rejected_source_" + std::to_string(sourceEntityId) +
            "_dead_" + std::to_string(sourceDead ? 1 : 0) +
            "_archetype_" + std::to_string(phantomArchetypeId) +
            "_power_" + std::to_string(createPower ? 1 : 0);
        return false;
    }

    std::string guardReason;
    const bool started = TryGuardedVoidCall(
        "corpse phantom native Spawn",
        [createPower, sourceEntity, phantomArchetype]()
        {
            createPower->m_currentCorpsePhantom.Spawn(*sourceEntity, *phantomArchetype);
        },
        &guardReason);
    if (!started)
    {
        m_lastCorpsePhantomEvent =
            std::string(context ? context : "corpse_phantom") +
            "_start_guard_" + guardReason;
        return false;
    }

    m_debugCorpsePhantomPending = true;
    m_debugCorpsePhantomSeconds = 0.0f;
    m_debugCorpsePhantomResultBaseline = m_corpsePhantomResults;
    m_lastCorpsePhantomEvent =
        std::string(context ? context : "corpse_phantom") +
        "_started_source_" + std::to_string(sourceEntityId) +
        "_archetype_" + std::to_string(phantomArchetypeId);
    return true;
}

void ModMain::HandleCorpsePhantomRequest(const CoopProtocol::CorpsePhantomRequestPacket& packet)
{
    if ((packet.flags & CoopProtocol::kPhantomChildRequestFlagEthericDoppelganger) != 0)
    {
        HandleEthericDoppelgangerRequest(packet);
        return;
    }

    ++m_corpsePhantomRequestsReceived;
    if ((m_networkMode != CoopNetworkMode::Host && !IsClientAreaAuthorityActive()) ||
        !IsEnemyReplicationGameplayReady() ||
        packet.sequence == 0 ||
        CoopSerialSequence::IsStaleOrDuplicate(packet.sequence, m_lastCorpsePhantomRequestSequence) ||
        packet.areaId == 0 ||
        packet.areaId != m_localLevelId ||
        packet.sourceEnemyNetId == 0 ||
        packet.sourceStableEnemyId == 0 ||
        packet.phantomArchetypeId == 0 ||
        packet.childStableEnemyId != BuildCorpsePhantomStableId(
            packet.sourceStableEnemyId,
            packet.phantomArchetypeId))
    {
        ++m_corpsePhantomRequestsDropped;
        m_lastCorpsePhantomEvent =
            "request_rejected_seq_" + std::to_string(packet.sequence) +
            "_source_" + std::to_string(packet.sourceStableEnemyId);
        return;
    }

    EnemyAuthorityState* sourceState = FindEnemyAuthorityByNetId(packet.sourceEnemyNetId);
    if (!sourceState ||
        sourceState->stableEnemyId != packet.sourceStableEnemyId ||
        sourceState->entityId == INVALID_ENTITYID)
    {
        ++m_corpsePhantomRequestsDropped;
        m_lastCorpsePhantomEvent =
            "request_source_mismatch_net_" + std::to_string(packet.sourceEnemyNetId) +
            "_stable_" + std::to_string(packet.sourceStableEnemyId);
        return;
    }

    CoopSerialSequence::Observe(packet.sequence, m_lastCorpsePhantomRequestSequence);
    const bool duplicate = std::any_of(
        m_pendingCorpsePhantomSpawnRequests.begin(),
        m_pendingCorpsePhantomSpawnRequests.end(),
        [&packet](const PendingCorpsePhantomSpawnRequest& pending)
        {
            return pending.sourceStableEnemyId == packet.sourceStableEnemyId;
        });
    if (duplicate || m_pendingCorpsePhantomSpawnRequests.size() >= 8)
    {
        ++m_corpsePhantomRequestsDropped;
        m_lastCorpsePhantomEvent =
            duplicate ? "request_duplicate_source_" : "request_queue_full_source_";
        m_lastCorpsePhantomEvent += std::to_string(packet.sourceStableEnemyId);
        return;
    }

    m_pendingCorpsePhantomSpawnRequests.push_back({
        sourceState->entityId,
        packet.sourceEnemyNetId,
        packet.sourceStableEnemyId,
        packet.phantomArchetypeId,
        packet.childStableEnemyId});
    m_lastCorpsePhantomEvent =
        "request_queued_seq_" + std::to_string(packet.sequence) +
        "_source_" + std::to_string(packet.sourceStableEnemyId);
}

void ModMain::TickPendingCorpsePhantomSpawnRequests()
{
    if ((m_networkMode != CoopNetworkMode::Host && !IsClientAreaAuthorityActive()) ||
        m_debugCorpsePhantomPending ||
        m_pendingCorpsePhantomSpawnRequests.empty() ||
        !gEnv ||
        !gEnv->pEntitySystem)
    {
        return;
    }

    const PendingCorpsePhantomSpawnRequest pending = m_pendingCorpsePhantomSpawnRequests.front();
    m_pendingCorpsePhantomSpawnRequests.pop_front();

    for (auto& entry : m_enemyAuthorities)
    {
        EnemyAuthorityState& child = entry.second;
        if (child.stableEnemyId != pending.childStableEnemyId)
            continue;

        EnsureEnemyRosterAnnounced(child, "corpse phantom duplicate roster resend failed");
        ++m_corpsePhantomRequestsApplied;
        m_lastCorpsePhantomEvent =
            "request_already_materialized_net_" + std::to_string(child.netId) +
            "_stable_" + std::to_string(child.stableEnemyId);
        return;
    }

    const auto sourceStableIt = m_enemyStableSpawnIdsByEntity.find(pending.sourceEntityId);
    EnemyAuthorityState* sourceState = FindEnemyAuthorityByNetId(pending.sourceEnemyNetId);
    if (!sourceState ||
        sourceState->entityId != pending.sourceEntityId ||
        sourceState->stableEnemyId != pending.sourceStableEnemyId ||
        (sourceStableIt != m_enemyStableSpawnIdsByEntity.end() &&
            sourceStableIt->second != pending.sourceStableEnemyId))
    {
        ++m_corpsePhantomRequestsDropped;
        m_lastCorpsePhantomEvent =
            "request_source_changed_net_" + std::to_string(pending.sourceEnemyNetId);
        return;
    }

    if (BeginNativeCorpsePhantomSpawn(
            pending.sourceEntityId,
            pending.phantomArchetypeId,
            "remote_request"))
    {
        ++m_corpsePhantomRequestsApplied;
    }
    else
    {
        ++m_corpsePhantomRequestsDropped;
    }
}

bool ModMain::RegisterLocalEthericDoppelgangerCandidate(
    IEntity& entity,
    ArkNpc& npc,
    const char* reason)
{
    const EntityId childEntityId = entity.GetId();
    bool isDoppelganger = false;
    EntityId ownerEntityId = INVALID_ENTITYID;
    if (!TryGuardedCall(
            "etheric doppelganger IsEthericDoppelganger",
            [&npc]() { return npc.IsEthericDoppelganger(); },
            isDoppelganger,
            nullptr) ||
        !isDoppelganger)
    {
        return false;
    }
    TryGuardedCall(
        "etheric doppelganger owner id",
        [&npc]() { return static_cast<EntityId>(npc.GetEthericDoppelgangerOwnerId()); },
        ownerEntityId,
        nullptr);

    IEntity* ownerEntity = ownerEntityId != INVALID_ENTITYID && gEnv && gEnv->pEntitySystem
        ? gEnv->pEntitySystem->GetEntity(ownerEntityId)
        : nullptr;
    EnemyAuthorityState* sourceState = nullptr;
    if (const auto netIt = m_enemyNetIdsByEntity.find(ownerEntityId);
        netIt != m_enemyNetIdsByEntity.end())
    {
        sourceState = FindEnemyAuthorityByNetId(netIt->second);
    }
    const bool localAreaAuthority =
        m_networkMode == CoopNetworkMode::Host || IsClientAreaAuthorityActive();
    if (!sourceState && ownerEntity && localAreaAuthority && IsEnemyReplicationCandidate(*ownerEntity))
        sourceState = &EnsureEnemyAuthorityState(*ownerEntity);
    if (!sourceState || sourceState->stableEnemyId == 0 || ownerEntityId == INVALID_ENTITYID)
    {
        MarkCoopRuntimeEntity(entity, true);
        PrepareCoopEntityForRemoval(
            childEntityId, false, true, "reject ownerless Etheric Doppelganger");
        RemoveCoopEntityGuarded(
            childEntityId, false, "reject ownerless Etheric Doppelganger");
        ++m_ethericDoppelgangerRequestsDropped;
        m_lastEthericDoppelgangerEvent =
            "local_child_missing_source entity=" + std::to_string(childEntityId) +
            " owner=" + std::to_string(ownerEntityId);
        return true;
    }

    if (const auto stableIt = m_enemyStableSpawnIdsByEntity.find(childEntityId);
        stableIt != m_enemyStableSpawnIdsByEntity.end() && stableIt->second != 0)
    {
        return true;
    }

    if (sourceState->activeEthericDoppelgangerStableEnemyId != 0)
    {
        EntityId existingChildId = INVALID_ENTITYID;
        for (const auto& stableEntry : m_enemyStableSpawnIdsByEntity)
        {
            if (stableEntry.second == sourceState->activeEthericDoppelgangerStableEnemyId)
            {
                existingChildId = stableEntry.first;
                break;
            }
        }
        if (existingChildId != INVALID_ENTITYID && existingChildId != childEntityId &&
            gEnv && gEnv->pEntitySystem && gEnv->pEntitySystem->GetEntity(existingChildId))
        {
            MarkCoopRuntimeEntity(entity, !localAreaAuthority);
            PrepareCoopEntityForRemoval(
                childEntityId, false, true, "reject duplicate Etheric Doppelganger");
            RemoveCoopEntityGuarded(
                childEntityId, false, "reject duplicate Etheric Doppelganger");
            ++m_ethericDoppelgangerRequestsDropped;
            m_lastEthericDoppelgangerEvent =
                "rejected_second_local_child source=" + std::to_string(sourceState->netId) +
                " existing=" + std::to_string(existingChildId) +
                " duplicate=" + std::to_string(childEntityId);
            return true;
        }
        sourceState->activeEthericDoppelgangerStableEnemyId = 0;
    }

    const uint64_t localAccountToken = GetLocalAccountToken();
    if (!localAreaAuthority &&
        (sourceState->authorityOwnerAccountToken != localAccountToken ||
            sourceState->remoteLocomotionAuthority))
    {
        MarkCoopRuntimeEntity(entity, true);
        PrepareCoopEntityForRemoval(
            childEntityId, false, true, "reject observer-created Etheric Doppelganger");
        RemoveCoopEntityGuarded(
            childEntityId, false, "reject observer-created Etheric Doppelganger");
        ++m_ethericDoppelgangerRequestsDropped;
        m_lastEthericDoppelgangerEvent =
            "rejected_non_authority_child source=" + std::to_string(sourceState->netId) +
            " child=" + std::to_string(childEntityId);
        return true;
    }

    uint32_t generation = CoopSerialSequence::Advance(sourceState->ethericDoppelgangerGeneration);
    if (generation == 0)
        generation = CoopSerialSequence::Advance(sourceState->ethericDoppelgangerGeneration);
    const uint64_t childStableId =
        BuildEthericDoppelgangerStableId(sourceState->stableEnemyId, generation);
    if (childStableId == 0)
        return true;

    m_enemyStableSpawnIdsByEntity[childEntityId] = childStableId;
    m_enemyEthericDoppelgangerSourcesByEntity[childEntityId] = sourceState->stableEnemyId;
    m_enemyEthericDoppelgangerGenerationsByEntity[childEntityId] = generation;
    MarkCoopRuntimeEntity(entity, !localAreaAuthority);
    sourceState->activeEthericDoppelgangerStableEnemyId = childStableId;
    TryGuardedVoidCall(
        "link local Etheric Doppelganger",
        [sourceState, &npc, ownerEntityId, childEntityId]()
        {
            npc.SetIsEthericDoppelganger(true);
            npc.SetEthericDoppengangerOwnerId(ownerEntityId);
            // The extracted ArkNpcAbility_EthericDoppelganger contract uses a
            // ten-second child lifetime. Reapply it on the owning simulation
            // because a multiplayer authority transition can otherwise leave
            // the native lifetime effect disabled on a persistent body.
            npc.SetTimeUntilDeath(10.0f);
            if (gEnv && gEnv->pEntitySystem)
            {
                if (IEntity* sourceEntity = gEnv->pEntitySystem->GetEntity(sourceState->entityId))
                {
                    if (ArkNpc* sourceNpc = EntityUtils::GetArkNpc(sourceEntity))
                        sourceNpc->SetEthericDoppelgangerId(childEntityId);
                }
            }
        },
        nullptr);

    if (!localAreaAuthority)
    {
        CoopProtocol::CorpsePhantomRequestPacket packet = {};
        packet.sequence = CoopSerialSequence::Advance(m_corpsePhantomRequestSequence);
        packet.areaId = m_localLevelId;
        packet.sourceEnemyNetId = sourceState->netId;
        packet.sourceStableEnemyId = sourceState->stableEnemyId;
        packet.phantomArchetypeId = entity.GetArchetype() ? entity.GetArchetype()->GetId() : 0;
        packet.childStableEnemyId = childStableId;
        packet.flags = CoopProtocol::kPhantomChildRequestFlagEthericDoppelganger;
        packet.reserved = generation;
        if (packet.phantomArchetypeId != 0 &&
            SendCorpsePhantomRequestTo(
                packet,
                m_remoteAddress,
                m_remotePort,
                "Etheric Doppelganger lifecycle request send failed"))
        {
            m_enemyEthericDoppelgangerRequestsSentByEntity.insert(childEntityId);
            ++m_ethericDoppelgangerRequestsSent;
        }
        else
        {
            ++m_ethericDoppelgangerRequestsDropped;
        }
    }

    m_lastEthericDoppelgangerEvent =
        "registered_local_child sourceNet=" + std::to_string(sourceState->netId) +
        " sourceStable=" + std::to_string(sourceState->stableEnemyId) +
        " child=" + std::to_string(childEntityId) +
        " childStable=" + std::to_string(childStableId) +
        " generation=" + std::to_string(generation) +
        " areaAuthority=" + std::to_string(localAreaAuthority ? 1 : 0) +
        " reason=" + std::string(reason && reason[0] ? reason : "-");
    AppendEnemySyncTrace("etheric_doppelganger", m_lastEthericDoppelgangerEvent);
    return true;
}

bool ModMain::ApplyEthericDoppelgangerRelation(
    EnemyAuthorityState& childState,
    IEntity& childEntity,
    const char* reason)
{
    if ((childState.rosterFlags & CoopProtocol::kEnemyRosterFlagEthericDoppelganger) == 0 ||
        childState.sourceStableEnemyId == 0)
    {
        return false;
    }

    EnemyAuthorityState* sourceState = nullptr;
    for (auto& entry : m_enemyAuthorities)
    {
        if (entry.second.stableEnemyId == childState.sourceStableEnemyId)
        {
            sourceState = &entry.second;
            break;
        }
    }
    if (!sourceState || sourceState->entityId == INVALID_ENTITYID ||
        !gEnv || !gEnv->pEntitySystem)
    {
        return false;
    }

    IEntity* sourceEntity = gEnv->pEntitySystem->GetEntity(sourceState->entityId);
    ArkNpc* sourceNpc = sourceEntity ? EntityUtils::GetArkNpc(sourceEntity) : nullptr;
    ArkNpc* childNpc = EntityUtils::GetArkNpc(&childEntity);
    if (!sourceNpc || !childNpc)
        return false;

    std::string guardReason;
    if (!TryGuardedVoidCall(
            "apply Etheric Doppelganger native relation",
            [sourceNpc, childNpc, sourceEntityId = sourceEntity->GetId(), childEntityId = childEntity.GetId()]()
            {
                childNpc->SetIsEthericDoppelganger(true);
                childNpc->SetEthericDoppengangerOwnerId(sourceEntityId);
                sourceNpc->SetEthericDoppelgangerId(childEntityId);
            },
            &guardReason))
    {
        m_lastEthericDoppelgangerEvent =
            "relation_failed childNet=" + std::to_string(childState.netId) +
            " reason=" + guardReason;
        return false;
    }

    childState.ethericDoppelgangerRelationApplied = true;
    sourceState->activeEthericDoppelgangerStableEnemyId = childState.stableEnemyId;
    sourceState->ethericDoppelgangerGeneration = std::max(
        sourceState->ethericDoppelgangerGeneration,
        childState.ethericDoppelgangerGeneration);
    m_enemyEthericDoppelgangerSourcesByEntity[childEntity.GetId()] = childState.sourceStableEnemyId;
    m_enemyEthericDoppelgangerGenerationsByEntity[childEntity.GetId()] =
        childState.ethericDoppelgangerGeneration;
    ++m_ethericDoppelgangerRelationsApplied;
    m_lastEthericDoppelgangerEvent =
        "relation_applied sourceNet=" + std::to_string(sourceState->netId) +
        " childNet=" + std::to_string(childState.netId) +
        " sourceEntity=" + std::to_string(sourceEntity->GetId()) +
        " childEntity=" + std::to_string(childEntity.GetId()) +
        " generation=" + std::to_string(childState.ethericDoppelgangerGeneration) +
        " reason=" + std::string(reason && reason[0] ? reason : "-");
    AppendEnemySyncTrace("etheric_doppelganger", m_lastEthericDoppelgangerEvent);
    return true;
}

void ModMain::HandleEthericDoppelgangerRequest(
    const CoopProtocol::CorpsePhantomRequestPacket& packet)
{
    ++m_ethericDoppelgangerRequestsReceived;
    const uint64_t sourceAccountToken = m_activePacketSourceAccountToken;
    const uint32_t generation = packet.reserved;
    if ((m_networkMode != CoopNetworkMode::Host && !IsClientAreaAuthorityActive()) ||
        !IsEnemyReplicationGameplayReady() ||
        packet.sequence == 0 ||
        packet.areaId == 0 ||
        packet.areaId != m_localLevelId ||
        packet.sourceEnemyNetId == 0 ||
        packet.sourceStableEnemyId == 0 ||
        packet.phantomArchetypeId == 0 ||
        generation == 0 ||
        packet.childStableEnemyId !=
            BuildEthericDoppelgangerStableId(packet.sourceStableEnemyId, generation))
    {
        ++m_ethericDoppelgangerRequestsDropped;
        m_lastEthericDoppelgangerEvent =
            "request_rejected seq=" + std::to_string(packet.sequence);
        return;
    }

    EnemyAuthorityState* sourceState = FindEnemyAuthorityByNetId(packet.sourceEnemyNetId);
    if (!sourceState ||
        sourceState->stableEnemyId != packet.sourceStableEnemyId ||
        sourceState->entityId == INVALID_ENTITYID ||
        sourceAccountToken == 0 ||
        sourceState->authorityOwnerAccountToken != sourceAccountToken)
    {
        ++m_ethericDoppelgangerRequestsDropped;
        m_lastEthericDoppelgangerEvent =
            "request_source_mismatch sourceNet=" + std::to_string(packet.sourceEnemyNetId) +
            " sender=" + std::to_string(sourceAccountToken) +
            " owner=" + std::to_string(sourceState ? sourceState->authorityOwnerAccountToken : 0);
        return;
    }

    for (auto& entry : m_enemyAuthorities)
    {
        EnemyAuthorityState& existingChild = entry.second;
        if (existingChild.stableEnemyId != packet.childStableEnemyId)
            continue;
        EnsureEnemyRosterAnnounced(existingChild, "Etheric Doppelganger duplicate roster resend failed");
        ++m_ethericDoppelgangerRequestsApplied;
        m_lastEthericDoppelgangerEvent =
            "request_already_materialized childNet=" + std::to_string(existingChild.netId);
        return;
    }

    if (sourceState->activeEthericDoppelgangerStableEnemyId != 0 &&
        sourceState->activeEthericDoppelgangerStableEnemyId != packet.childStableEnemyId)
    {
        ++m_ethericDoppelgangerRequestsDropped;
        m_lastEthericDoppelgangerEvent =
            "request_second_active_rejected sourceNet=" + std::to_string(sourceState->netId);
        return;
    }

    IEntity* sourceEntity = gEnv && gEnv->pEntitySystem
        ? gEnv->pEntitySystem->GetEntity(sourceState->entityId)
        : nullptr;
    if (!sourceEntity)
    {
        ++m_ethericDoppelgangerRequestsDropped;
        return;
    }

    const std::string replicaName =
        "CoopEthericDoppelganger_" + std::to_string(packet.childStableEnemyId);
    Vec3 spawnPosition = sourceEntity->GetWorldPos();
    Quat spawnRotation = sourceEntity->GetWorldRotation();
    IEntity* childEntity = EntityUtils::SpawnNpc(
        replicaName.c_str(),
        spawnPosition,
        spawnRotation,
        packet.phantomArchetypeId);
    ArkNpc* childNpc = childEntity ? EntityUtils::GetArkNpc(childEntity) : nullptr;
    if (!childEntity || !childNpc)
    {
        ++m_ethericDoppelgangerRequestsDropped;
        m_lastEthericDoppelgangerEvent =
            "request_spawn_failed sourceNet=" + std::to_string(sourceState->netId);
        return;
    }

    // The area-authority copy runs Vanilla AI/physics. Observer replicas are
    // marked client-side later by the normal remote binding path.
    MarkCoopRuntimeEntity(*childEntity, false);
    m_enemyStableSpawnIdsByEntity[childEntity->GetId()] = packet.childStableEnemyId;
    m_enemyEthericDoppelgangerSourcesByEntity[childEntity->GetId()] = packet.sourceStableEnemyId;
    m_enemyEthericDoppelgangerGenerationsByEntity[childEntity->GetId()] = generation;
    sourceState->activeEthericDoppelgangerStableEnemyId = packet.childStableEnemyId;
    sourceState->ethericDoppelgangerGeneration = std::max(
        sourceState->ethericDoppelgangerGeneration,
        generation);
    TryGuardedVoidCall(
        "initialize requested Etheric Doppelganger",
        [childNpc, sourceEntityId = sourceEntity->GetId()]()
        {
            childNpc->SetIsEthericDoppelganger(true);
            childNpc->SetEthericDoppengangerOwnerId(sourceEntityId);
            childNpc->SetTimeUntilDeath(10.0f);
        },
        nullptr);

    EnemyAuthorityState& childState = EnsureEnemyAuthorityState(*childEntity);
    // The lifecycle request came from the current locomotion authority.  The
    // area authority only materializes the canonical body; it must not steal
    // the child just because it allocated the shared net id.
    childState.authorityOwnerAccountToken = sourceState->authorityOwnerAccountToken;
    childState.authorityEpoch = sourceState->authorityEpoch;
    childState.authorityAttentionLevel = sourceState->authorityAttentionLevel;
    childState.localAttentionClaimed = false;
    childState.remoteLocomotionAuthority =
        childState.authorityOwnerAccountToken != GetLocalAccountToken();
    childState.remoteAuthorityHasAttention =
        childState.remoteLocomotionAuthority &&
        childState.authorityAttentionLevel > CoopEnemyAuthorityPolicy::kUnknownAttention;
    ApplyEthericDoppelgangerRelation(
        childState, *childEntity, "materialized authority lifecycle request");
    if (!EnsureEnemyRosterAnnounced(
            childState, "Etheric Doppelganger roster announce failed"))
    {
        ++m_ethericDoppelgangerRequestsDropped;
        return;
    }

    ++m_ethericDoppelgangerRequestsApplied;
    m_lastEthericDoppelgangerEvent =
        "request_materialized sourceNet=" + std::to_string(packet.sourceEnemyNetId) +
        " childNet=" + std::to_string(childState.netId) +
        " childEntity=" + std::to_string(childEntity->GetId()) +
        " generation=" + std::to_string(generation);
    AppendEnemySyncTrace("etheric_doppelganger", m_lastEthericDoppelgangerEvent);
}

void ModMain::TickPendingRemoteCorpsePhantomResults()
{
    if (m_pendingRemoteCorpsePhantomResults.empty() || !gEnv || !gEnv->pEntitySystem)
        return;

    const PendingRemoteCorpsePhantomResult pending = m_pendingRemoteCorpsePhantomResults.front();
    m_pendingRemoteCorpsePhantomResults.pop_front();

    IEntity* sourceEntity = nullptr;
    if (!TryGuardedCall(
            "pending raised corpse GetEntity",
            [pending]() { return gEnv->pEntitySystem->GetEntity(pending.sourceEntityId); },
            sourceEntity,
            nullptr) ||
        !sourceEntity)
    {
        m_lastCorpsePhantomEvent =
            "remote_source_already_absent_entity_" + std::to_string(pending.sourceEntityId);
        return;
    }

    const auto stableIt = m_enemyStableSpawnIdsByEntity.find(pending.sourceEntityId);
    if (stableIt == m_enemyStableSpawnIdsByEntity.end() ||
        stableIt->second != pending.sourceStableEnemyId)
    {
        m_lastCorpsePhantomEvent =
            "remote_source_identity_changed_entity_" + std::to_string(pending.sourceEntityId);
        return;
    }

    ArkNpc* sourceNpc = nullptr;
    bool sourceDead = false;
    TryGuardedCall(
        "pending raised corpse GetArkNpc",
        [sourceEntity]() { return EntityUtils::GetArkNpc(sourceEntity); },
        sourceNpc,
        nullptr);
    if (sourceNpc)
    {
        TryGuardedCall(
            "pending raised corpse IsDead",
            [sourceNpc]() { return sourceNpc->IsDead(); },
            sourceDead,
            nullptr);
    }
    if (!sourceNpc || !sourceDead)
    {
        m_lastCorpsePhantomEvent =
            "remote_source_not_dead_entity_" + std::to_string(pending.sourceEntityId);
        return;
    }

    const bool previousApply = m_applyingRemoteCorpsePhantomResult;
    m_applyingRemoteCorpsePhantomResult = true;
    const bool removed = TryGuardedVoidCall(
        "pending raised corpse RemoveEntity",
        [pending]() { gEnv->pEntitySystem->RemoveEntity(pending.sourceEntityId, true); },
        nullptr);
    m_applyingRemoteCorpsePhantomResult = previousApply;
    if (!removed)
    {
        m_lastCorpsePhantomEvent =
            "remote_source_remove_guard_entity_" + std::to_string(pending.sourceEntityId);
        return;
    }

    ++m_corpsePhantomSourceRemovals;
    m_lastCorpsePhantomEvent =
        "remote_source_removed_entity_" + std::to_string(pending.sourceEntityId) +
        "_stable_" + std::to_string(pending.sourceStableEnemyId) +
        "_child_" + std::to_string(pending.childStableEnemyId);
}

bool ModMain::ShouldAllowLocalVanillaEnemyControl(const EnemyAuthorityState& state, const IEntity& entity) const
{
    return CoopEnemyControlPolicy::AllowsLocalVanillaControl(
        BuildLocalEnemyControlPolicyContext(state, entity));
}

bool ModMain::ShouldBlockLocalVanillaEnemyControl(const EnemyAuthorityState& state, const IEntity& entity) const
{
    return CoopEnemyControlPolicy::BlocksLocalVanillaControl(
        BuildLocalEnemyControlPolicyContext(state, entity));
}

bool ModMain::ShouldBlockLocalVanillaEnemyControlIntent(
    const EnemyAuthorityState& state,
    const IEntity& entity,
    LocalEnemyVanillaControlIntent intent) const
{
    return CoopEnemyControlPolicy::BlocksLocalVanillaControlIntent(
        BuildLocalEnemyControlPolicyContext(state, entity),
        intent);
}

float ModMain::ConsumeRemoteEnemyTransformTickSeconds(
    EnemyAuthorityState& state,
    float fallbackTickSeconds,
    bool rewritePosition)
{
    float tickSeconds = fallbackTickSeconds > 0.0f ? fallbackTickSeconds : (1.0f / 60.0f);
    const float nowSeconds = gEnv && gEnv->pTimer ? gEnv->pTimer->GetAsyncCurTime() : -1.0f;
    if (rewritePosition && nowSeconds >= 0.0f)
    {
        if (state.remoteTransformRewriteLastSeconds >= 0.0f)
            tickSeconds = nowSeconds - state.remoteTransformRewriteLastSeconds;
        state.remoteTransformRewriteLastSeconds = nowSeconds;
    }
    return std::clamp(tickSeconds, 0.001f, 0.05f);
}

uint32_t ModMain::UpdateRemoteEnemyVisualMotionFromStep(
    EnemyAuthorityState& state,
    float visibleStep,
    float visibleSpeed,
    uint32_t sourceFlags,
    float elapsedSeconds,
    bool decayWhenIdle)
{
    const uint32_t continuousMovementFlags = CoopEnemyControlPolicy::ContinuousMovementFlags();
    const uint32_t burstMovementFlags = CoopEnemyControlPolicy::BurstMovementFlags();
    const uint32_t sourceMovementFlags = sourceFlags & (continuousMovementFlags | burstMovementFlags);

    if ((sourceMovementFlags & burstMovementFlags) != 0)
    {
        state.remoteVisualMotionSeconds = 0.0f;
        if (visibleStep >= kEnemyRemoteVisualMoveStepThreshold)
        {
            state.remoteVisualMotionFlags = sourceMovementFlags & burstMovementFlags;
            state.remoteVisualSpeed = std::isfinite(visibleSpeed)
                ? std::max(0.0f, visibleSpeed)
                : 0.0f;
            return state.remoteVisualMotionFlags;
        }

        state.remoteVisualMotionFlags = 0;
        state.remoteVisualSpeed = 0.0f;
        return 0;
    }

    if ((sourceMovementFlags & continuousMovementFlags) != 0 &&
        visibleStep >= kEnemyRemoteVisualMoveStepThreshold)
    {
        const bool sourceSaysRun =
            (sourceMovementFlags & CoopProtocol::kEnemyLocomotionFlagRunning) != 0;
        const bool sourceSaysWalk =
            (sourceMovementFlags & CoopProtocol::kEnemyLocomotionFlagWalking) != 0;
        if (sourceSaysRun && !sourceSaysWalk)
        {
            state.remoteVisualMotionFlags = CoopProtocol::kEnemyLocomotionFlagRunning;
        }
        else if (sourceSaysWalk && !sourceSaysRun)
        {
            state.remoteVisualMotionFlags = CoopProtocol::kEnemyLocomotionFlagWalking;
        }
        else
        {
            state.remoteVisualMotionFlags =
                visibleSpeed > kEnemyRemoteReceiverInferRunSpeed
                    ? CoopProtocol::kEnemyLocomotionFlagRunning
                    : CoopProtocol::kEnemyLocomotionFlagWalking;
        }
        state.remoteVisualMotionSeconds = kEnemyRemoteVisualMoveHoldSeconds;
        state.remoteVisualSpeed = std::isfinite(visibleSpeed)
            ? std::max(0.0f, visibleSpeed)
            : 0.0f;
        return state.remoteVisualMotionFlags;
    }

    if (decayWhenIdle)
    {
        state.remoteVisualMotionSeconds =
            std::max(0.0f, state.remoteVisualMotionSeconds - std::max(elapsedSeconds, 0.0f));
    }

    if (state.remoteVisualMotionSeconds > 0.0f &&
        (sourceMovementFlags & continuousMovementFlags) != 0)
    {
        state.remoteVisualMotionFlags &= continuousMovementFlags;
        return state.remoteVisualMotionFlags;
    }

    if (state.remoteVisualMotionSeconds <= 0.0f)
    {
        state.remoteVisualMotionFlags = 0;
        state.remoteVisualSpeed = 0.0f;
    }
    return 0;
}

bool ModMain::ComputeRemoteEnemyTransformSmoothing(
    EnemyAuthorityState& state,
    const Vec3& currentPosition,
    const Quat& currentRotation,
    bool rewritePosition,
    bool rewriteRotation,
    float fallbackTickSeconds,
    Vec3& outPosition,
    Quat& outRotation,
    RemoteEnemyTransformSmoothingResult& outResult)
{
    outResult = {};
    outResult.tickSeconds = ConsumeRemoteEnemyTransformTickSeconds(state, fallbackTickSeconds, rewritePosition);
    const uint32_t burstMovementFlags = CoopEnemyControlPolicy::BurstMovementFlags();
    // A dash flag can be a one-packet edge even though its authored motion and
    // FX continue for several frames. Keep ordinary sample interpolation
    // suspended for the complete bounded burst hold, rather than resuming on
    // the first packet whose flag has cleared and pulling the body back toward
    // a pre-dash sample.
    const bool burstSmoothingActive = state.remoteBurstTransformSeconds > 0.0f;
    const uint32_t smoothingLocomotionFlags =
        burstSmoothingActive ? state.remoteLocomotionFlags : (state.remoteLocomotionFlags & ~burstMovementFlags);
    const uint32_t smoothingMannequinFlags =
        burstSmoothingActive ? state.remoteMannequinFlags : (state.remoteMannequinFlags & ~burstMovementFlags);
    outResult.burst = burstSmoothingActive;
    outPosition = currentPosition;
    outRotation = currentRotation;

    if (rewritePosition)
    {
        Vec3 smoothingTarget = state.remoteTargetPosition;
        state.remoteInterpolationActive = false;
        state.remoteInterpolationDelaySeconds = 0.0f;
        const float nowSeconds = EnemyAnimationNowSeconds();
        if (!burstSmoothingActive &&
            state.remotePositionSampleCount >= 2 &&
            nowSeconds >= 0.0f)
        {
            const size_t sampleCount = std::min<size_t>(
                state.remotePositionSampleCount,
                state.remotePositionSamples.size());
            const float interpolationDelay = std::clamp(
                state.remotePositionSampleIntervalSeconds * kEnemyRemoteInterpolationDelayScale,
                kEnemyRemoteInterpolationMinDelaySeconds,
                kEnemyRemoteInterpolationMaxDelaySeconds);
            const float renderSeconds = nowSeconds - interpolationDelay;
            state.remoteInterpolationDelaySeconds = interpolationDelay;

            const EnemyAuthorityState::RemotePositionSample& oldest =
                state.remotePositionSamples[0];
            const EnemyAuthorityState::RemotePositionSample& newest =
                state.remotePositionSamples[sampleCount - 1];
            if (renderSeconds <= oldest.receivedAtSeconds)
            {
                smoothingTarget = oldest.position;
                state.remoteInterpolationActive = true;
            }
            else if (renderSeconds < newest.receivedAtSeconds)
            {
                for (size_t index = 1; index < sampleCount; ++index)
                {
                    const EnemyAuthorityState::RemotePositionSample& previous =
                        state.remotePositionSamples[index - 1];
                    const EnemyAuthorityState::RemotePositionSample& next =
                        state.remotePositionSamples[index];
                    if (renderSeconds > next.receivedAtSeconds)
                        continue;

                    const float sampleSeconds = std::max(
                        next.receivedAtSeconds - previous.receivedAtSeconds,
                        0.001f);
                    const float alpha = std::clamp(
                        (renderSeconds - previous.receivedAtSeconds) / sampleSeconds,
                        0.0f,
                        1.0f);
                    smoothingTarget = previous.position +
                        (next.position - previous.position) * alpha;
                    state.remoteInterpolationActive = true;
                    break;
                }
            }
            else
            {
                smoothingTarget = newest.position;
                const uint32_t continuousMovementFlags =
                    CoopEnemyControlPolicy::ContinuousMovementFlags();
                if ((state.remoteLocomotionFlags & continuousMovementFlags) != 0 &&
                    nowSeconds - newest.receivedAtSeconds > interpolationDelay)
                {
                    ++state.remoteInterpolationUnderruns;
                }
            }
            ++state.remoteInterpolationFrames;
        }
        state.remoteInterpolationTargetPosition = smoothingTarget;

        const Vec3 delta = smoothingTarget - currentPosition;
        const float distance = delta.GetLength();
        m_enemyRemoteMaxBacklogDistance = std::max(m_enemyRemoteMaxBacklogDistance, distance);
        if (distance <= kEnemyRemoteSmoothTinySnapDistance)
        {
            outPosition = smoothingTarget;
            outResult.positionStep = distance;
            outResult.positionSpeed = distance /
                std::max(outResult.tickSeconds, 0.001f);
            outResult.moved = distance > 0.0001f;
        }
        else
        {
            outResult.positionStep = ComputeRemoteEnemySmoothStepForTick(
                distance,
                outResult.tickSeconds,
                ComputeRemoteEnemySmoothBaseTravelSpeed(
                    smoothingLocomotionFlags,
                    smoothingMannequinFlags,
                    state.remoteSpeed),
                outResult.burst,
                -1.0f,
                outResult.hardPosition,
                outResult.positionSpeed);
            outPosition = currentPosition + delta * (outResult.positionStep / distance);
            outResult.moved = true;
            m_enemyRemoteMaxCatchupSpeed = std::max(
                m_enemyRemoteMaxCatchupSpeed,
                outResult.positionSpeed);
        }
    }

    if (rewriteRotation)
    {
        Quat targetRotation = state.remoteTargetRotation;
        targetRotation.Normalize();
        const float angle = ComputeRemoteEnemyRotationAngle(currentRotation, targetRotation);
        if (angle <= 0.002f)
        {
            outRotation = targetRotation;
            outResult.rotated = false;
        }
        else
        {
            outResult.rotationAlpha = ComputeRemoteEnemyRotationAlphaForTick(
                angle,
                outResult.hardPosition,
                outResult.tickSeconds,
                outResult.burst,
                outResult.hardRotation,
                outResult.rotationSpeed);
            outRotation = Quat::CreateNlerp(currentRotation, targetRotation, outResult.rotationAlpha);
            outRotation.Normalize();
            outResult.rotated = true;
        }
    }

    if (rewritePosition)
    {
        state.lastPosition = outPosition;
        UpdateRemoteEnemyVisualMotionFromStep(
            state,
            outResult.positionStep,
            outResult.positionSpeed,
            state.remoteLocomotionFlags | state.remoteTargetMotionFlags,
            outResult.tickSeconds,
            true);
    }
    if (rewriteRotation)
        state.lastRotation = outRotation;

    return outResult.moved || outResult.rotated;
}

bool ModMain::TryBuildReadOnlyLocalFacingMixTarget(
    const EnemyAuthorityState& state,
    const IEntity& entity,
    float nowSeconds,
    Quat& outRotation) const
{
    const bool trackedLocalAwareness =
        state.localReadOnlyAttentionActive ||
        state.localRotationOverrideSeconds > 0.0f;
    if (EnvFlagEnabled("COOP_DISABLE_READ_ONLY_LOCAL_FACING_MIX") ||
        nowSeconds < 0.0f ||
        m_localPlayerDowned ||
        IsLocalPlayerAuthorityBlockedByModalState() ||
        !trackedLocalAwareness ||
        !ArkPlayer::GetInstancePtr())
    {
        return false;
    }

    IEntity* localPlayer = ArkPlayer::GetInstance().GetEntity();
    if (!localPlayer)
        return false;

    const uint32_t hardAuthorityFlags =
        CoopProtocol::kEnemyLocomotionFlagGlooed |
        CoopProtocol::kEnemyLocomotionFlagDashing |
        CoopProtocol::kEnemyLocomotionFlagShifting |
        CoopProtocol::kEnemyLocomotionFlagMorphing |
        CoopProtocol::kEnemyLocomotionFlagLunging |
        CoopProtocol::kEnemyLocomotionFlagHitReacting |
        CoopProtocol::kEnemyLocomotionFlagStunned |
        CoopProtocol::kEnemyLocomotionFlagCowering;
    if (((state.remoteLocomotionFlags | state.remoteMannequinFlags) & hardAuthorityFlags) != 0)
        return false;

    // The attention edge records identity, while the ordinary awareness path
    // keeps localRotationOverrideSeconds alive for suspicion and combat. Aim
    // at the real player's current position throughout that native window;
    // limiting this to the edge's 350 ms sample made sustained attacks face
    // the authority target again even though local combat stayed active.
    Vec3 direction = localPlayer->GetWorldPos() - entity.GetWorldPos();
    direction.z = 0.0f;
    const float directionLengthSq = direction.GetLengthSquared();
    if (!std::isfinite(direction.x) ||
        !std::isfinite(direction.y) ||
        directionLengthSq <= 0.0025f)
    {
        return false;
    }

    direction *= 1.0f / std::sqrt(directionLengthSq);
    outRotation = Quat::CreateRotationZ(std::atan2(-direction.x, direction.y));
    outRotation.Normalize();
    return true;
}

bool ModMain::TryGetRemoteDrivenEnemyTransformOverride(
    IEntity* entity,
    const char* stage,
    bool overridePosition,
    bool overrideRotation,
    Vec3& ioPosition,
    Quat& ioRotation)
{
    if (!entity ||
        !gEnv ||
        !gEnv->pEntitySystem ||
        m_networkMode == CoopNetworkMode::Off)
    {
        return false;
    }

    if (!CoopRuntimeGuards::IsLikelyRuntimeCppObject(entity, sizeof(void*) * 4))
        return false;

    EntityId entityId = INVALID_ENTITYID;
    std::string reason;
    if (!TryGuardedCall("remote enemy transform gate GetId", [entity]() { return entity->GetId(); }, entityId, &reason) ||
        entityId == INVALID_ENTITYID)
    {
        return false;
    }

    if (EnvFlagEnabled("COOP_DISABLE_REMOTE_ENEMY_TRANSFORM_REWRITE") ||
        m_remoteEnemyTransformWriteDepth != 0 ||
        !m_enemyLocomotionSyncEnabled ||
        !IsEnemyReplicationGameplayReady())
    {
        return false;
    }

    const auto netIt = m_enemyNetIdsByEntity.find(entityId);
    if (netIt == m_enemyNetIdsByEntity.end())
        return false;

    EnemyAuthorityState* state = FindEnemyAuthorityByNetId(netIt->second);
    if (!state || !state->hasLastPosition || state->entityId != entityId)
        return false;

    const IEntityArchetype* currentArchetype = nullptr;
    uint64_t currentArchetypeId = 0;
    if (!TryGuardedCall(
            "remote enemy transform gate GetArchetype",
            [entity]() { return entity->GetArchetype(); },
            currentArchetype,
            &reason) ||
        !currentArchetype ||
        !TryGuardedCall(
            "remote enemy transform gate archetype GetId",
            [currentArchetype]() { return currentArchetype->GetId(); },
            currentArchetypeId,
            &reason) ||
        currentArchetypeId == 0 ||
        currentArchetypeId != state->archetypeId)
    {
        m_enemyNetIdsByEntity.erase(netIt);
        m_enemyStableSpawnIdsByEntity.erase(entityId);
        if (state->entityId == entityId)
        {
            state->entityId = INVALID_ENTITYID;
            state->hasLastPosition = false;
            state->remoteLocomotionAuthority = false;
            state->remoteAuthorityHasAttention = false;
            state->authorityAttentionLevel = CoopEnemyAuthorityPolicy::kUnknownAttention;
            state->attentionCandidates.clear();
            state->remoteTargetAccountToken = 0;
            state->remoteTransformNeedsAuthoritySnap = true;
        }
        ClearRemoteEnemyMovementDesire(state->netId, "rejected stale enemy entity-id binding");
        m_lastEnemyLocomotionEvent =
            "rejected stale enemy binding net=" + std::to_string(state->netId) +
            " entity=" + std::to_string(entityId) +
            " expectedArch=" + std::to_string(state->archetypeId) +
            " actualArch=" + std::to_string(currentArchetypeId);
        AppendEnemySyncTrace("roster", m_lastEnemyLocomotionEvent);
        return false;
    }

    if (const auto rosterIt = m_enemyRosterByNetId.find(state->netId);
        rosterIt != m_enemyRosterByNetId.end() && rosterIt->second.archetypeId != currentArchetypeId)
    {
        m_enemyNetIdsByEntity.erase(entityId);
        m_enemyStableSpawnIdsByEntity.erase(entityId);
        state->entityId = INVALID_ENTITYID;
        state->hasLastPosition = false;
        state->remoteLocomotionAuthority = false;
        state->remoteAuthorityHasAttention = false;
        state->authorityAttentionLevel = CoopEnemyAuthorityPolicy::kUnknownAttention;
        state->attentionCandidates.clear();
        state->remoteTargetAccountToken = 0;
        state->remoteTransformNeedsAuthoritySnap = true;
        ClearRemoteEnemyMovementDesire(state->netId, "rejected roster archetype mismatch");
        return false;
    }

    const CoopEnemyControlPolicy::Context controlContext =
        BuildLocalEnemyControlPolicyContext(*state, *entity);
    const CoopEnemyControlPolicy::Decision decision =
        CoopEnemyControlPolicy::Evaluate(controlContext);
    if (!decision.remoteDriven || decision.localVanillaAuthority)
        return false;

    const bool rewritePosition = overridePosition && decision.blockMovement;
    // In the mixed observer lane, native locomotion is still allowed to run
    // for local perception/combat. Its integrator also writes the body
    // rotation, though, and would otherwise erase the controlled local-facing
    // result between Coop ticks. Preserve that mixer-owned rotation just as we
    // do for a strict remote observer; hard authority body actions fall back to
    // the replicated rotation below.
    const bool rewriteRotation =
        overrideRotation && (decision.blockTurn || decision.localFocus);
    if (!rewritePosition && !rewriteRotation)
        return false;

    Vec3 currentPosition = Vec3(ZERO);
    Quat currentRotation = Quat::CreateIdentity();
    if (!TryGuardedCall("remote enemy transform gate GetWorldPos", [entity]() { return entity->GetWorldPos(); }, currentPosition, &reason))
        return false;
    if (!TryGuardedCall("remote enemy transform gate GetWorldRotation", [entity]() { return entity->GetWorldRotation(); }, currentRotation, &reason))
        return false;
    currentRotation.Normalize();

    const float nowSeconds = gEnv && gEnv->pTimer ? gEnv->pTimer->GetAsyncCurTime() : -1.0f;
    Quat localFacingMixTarget = Quat::CreateIdentity();
    const bool preserveControlledLocalFacing =
        rewriteRotation &&
        TryBuildReadOnlyLocalFacingMixTarget(
            *state,
            *entity,
            nowSeconds,
            localFacingMixTarget);
    // Native movement hooks are gates, not secondary integrators. Advancing
    // the follower here made its cadence depend on how many SetPos/SetWorldTM
    // calls Vanilla happened to issue in a frame. Preserve the transform and
    // let TickRemoteEnemySmoothing be the single render-frame writer.
    if (rewritePosition)
        ioPosition = currentPosition;
    if (rewriteRotation)
        ioRotation = currentRotation;

    ++m_enemyTransformRequestBlocks;
    if ((m_enemyTransformRequestBlocks & 0x3fu) == 1u)
    {
        m_lastEnemyLocomotionEvent =
            "remote_transform_block"
            " stage=" + std::string(stage && stage[0] ? stage : "-") +
            " net=" + std::to_string(state->netId) +
            " entity=" + std::to_string(entityId) +
            " pos=" + std::to_string(rewritePosition ? 1 : 0) +
            " rot=" + std::to_string(rewriteRotation ? 1 : 0) +
            " localFacingPreserve=" + std::to_string(preserveControlledLocalFacing ? 1 : 0) +
            " targetDelta=" + std::to_string((state->remoteTargetPosition - currentPosition).GetLength()) +
            " centralWriter=1" +
            " count=" + std::to_string(m_enemyTransformRequestBlocks);
        AppendEnemySyncTrace("pose", m_lastEnemyLocomotionEvent);
    }
    return true;
}

bool ModMain::TryGetDebugEnemyAttentionOverride(const IEntity& enemy, bool& outHasAttention) const
{
    uint8_t attentionLevel = CoopEnemyAuthorityPolicy::kUnknownAttention;
    if (!TryGetDebugEnemyAttentionLevelOverride(enemy, attentionLevel))
        return false;
    outHasAttention = attentionLevel > CoopEnemyAuthorityPolicy::kUnknownAttention;
    return true;
}

bool ModMain::TryGetDebugEnemyAttentionLevelOverride(
    const IEntity& enemy,
    uint8_t& outAttentionLevel) const
{
    if (!m_debugEnemyAttentionOverrideActive)
        return false;

    if (m_debugEnemyAttentionOverrideNetId != 0)
    {
        const auto netIt = m_enemyNetIdsByEntity.find(enemy.GetId());
        const bool reverseBindingMatches =
            netIt != m_enemyNetIdsByEntity.end() &&
            netIt->second == m_debugEnemyAttentionOverrideNetId;
        const auto authorityIt = m_enemyAuthorities.find(m_debugEnemyAttentionOverrideNetId);
        const bool canonicalBindingMatches =
            authorityIt != m_enemyAuthorities.end() &&
            authorityIt->second.entityId == enemy.GetId();
        // Roster rebinding can briefly leave the reverse EntityId index behind
        // the canonical net-id state. Debug fixtures must select the requested
        // replicated enemy rather than silently falling back to native attention.
        if (!reverseBindingMatches && !canonicalBindingMatches)
            return false;
    }

    outAttentionLevel = m_debugEnemyAttentionOverrideLevel;
    return true;
}

void ModMain::PrepareCoopRuntimeEntitiesForLevelTransition(const char* reason)
{
    if (m_runtimeTransitionCleanupPrepared)
        return;

    m_runtimeTransitionCleanupPrepared = true;
    ++m_runtimeTransitionCleanupCount;
    m_sessionGameplayReady = false;
    m_clientAreaAuthorityActive = false;
    // Reliable sequence numbers are endpoint-wide. Keep already queued
    // packets across an area transition so the receiver never waits forever
    // for a sequence whose payload was discarded locally. Gameplay producers
    // are gated by sessionGameplayReady until the new area is interactive.
    m_networkTickAccumulator = 0.0f;
    m_mimicStateTickAccumulator = 0.0f;

    std::vector<EntityId> runtimeEntityIds;
    auto addRuntimeEntityId = [&runtimeEntityIds](EntityId entityId)
    {
        if (entityId == INVALID_ENTITYID)
            return;
        if (std::find(runtimeEntityIds.begin(), runtimeEntityIds.end(), entityId) == runtimeEntityIds.end())
            runtimeEntityIds.push_back(entityId);
    };

    AddRemoteProxyEntityIds(runtimeEntityIds);
    addRuntimeEntityId(m_mimicEntityId);
    AddRuntimeEnemyEntityIds(runtimeEntityIds);

    std::vector<EntityId> runtimeProxyEntityIds;
    AddRemoteProxyEntityIds(runtimeProxyEntityIds);
    m_transitionRuntimeProxyEntityIds.insert(
        runtimeProxyEntityIds.begin(),
        runtimeProxyEntityIds.end());
    m_transitionRuntimeEntityIds.insert(
        runtimeEntityIds.begin(),
        runtimeEntityIds.end());

    const uint32_t trackedEntities = static_cast<uint32_t>(runtimeEntityIds.size());

    std::string guardReason;
    if (!TryGuardedVoidCall(
        "transition cleanup unregister revive",
        [this]() { UnregisterProxyReviveInteraction(); },
        &guardReason))
    {
        ++m_runtimeTransitionCleanupFailures;
    }

    if (CoopRuntimeConfig::UnsafeFlag("COOP_ALLOW_PROXY_COMPLEX_UNREGISTER_DURING_TRANSITION") &&
        !TryGuardedVoidCall(
            "transition cleanup unregister complex attention",
            [this]() { UnregisterProxyComplexAttention(); },
            &guardReason))
    {
        ++m_runtimeTransitionCleanupFailures;
    }

    // The native entity system owns these entities, its worker references and
    // all ArkNpc manager registrations. Mutating or deleting them before the
    // native level teardown changes that order and can leave either an
    // ArkNpcAbilityManager or a worker job with a stale owner. Only quiesce
    // Coop producers and update bookkeeping here.
    if (m_proxyEntityId != INVALID_ENTITYID)
        SetProxyLifecycleState(
            CoopProxyLifecycleState::SuspendedTransition,
            reason ? reason : "transition cleanup");

    m_runtimeTransitionCleanupEntities += trackedEntities;
    m_lastRuntimeTransitionCleanupEvent =
        "transition runtime cleanup reason=" + std::string(reason ? reason : "unknown") +
        " tracked=" + std::to_string(trackedEntities) +
        " arkRemovalPending=" + std::to_string(trackedEntities) +
        " entityMutation=none" +
        " failures=" + std::to_string(m_runtimeTransitionCleanupFailures);
    if (!guardReason.empty())
        m_lastRuntimeTransitionCleanupEvent += " lastGuard=" + guardReason;
    LogCoop(m_lastRuntimeTransitionCleanupEvent);
}

void ModMain::DrawCoopDebug()
{
    ImGui::Separator();
    if (!ImGui::CollapsingHeader("Coop Debug"))
        return;

    ImGui::Text("Enemy authorities: %zu  puppets: %zu  max states/tick: %zu  scan: %.2fs",
        m_enemyAuthorities.size(),
        m_enemyPuppets.size(),
        kMaxEnemyStatesPerTick,
        kEnemyRegistryDirtyDebounceSeconds);
    ImGui::Text("Area authority: clientLocal=%d activations=%u scans=%u hits=%u last=%s",
        m_clientAreaAuthorityActive ? 1 : 0,
        m_clientAreaAuthorityActivations,
        m_clientAreaAuthorityScans,
        m_clientAreaAuthorityHits,
        m_lastAreaAuthorityEvent.c_str());
    ImGui::Text("Proxy target bindings: %u  force target: %s",
        m_lastProxyTargetBindings,
        m_forceHostEnemiesTargetProxy ? "on" : "off");
    ImGui::Text("Proxy simple attention: stimulated=%u tracked=%u enabled=%s",
        m_lastProxySimpleAttentionStimulus,
        m_lastProxySimpleAttentionTracked,
        m_stimulateHostEnemySimpleAttentionOnProxy ? "yes" : "no");
    ImGui::Text("Proxy complex attention: attention=%d visual=%d aural=%d room=%d live=%d tracked=%u",
        m_proxyComplexAttentionRegistered ? 1 : 0,
        m_proxyComplexVisualRegistered ? 1 : 0,
        m_proxyComplexAuralRegistered ? 1 : 0,
        m_proxyComplexRoomRegistered ? 1 : 0,
        IsProxyComplexAttentionRegistered() ? 1 : 0,
        m_lastProxyComplexAttentionTracked);
    ImGui::Text("Proxy combat stimulus: %u  ability attempts/successes: %u/%u  range: %.1fm",
        m_lastProxyCombatStimulusCount,
        m_lastProxyAbilityAttempts,
        m_lastProxyAbilitySuccesses,
        kProxyCombatStimulusRangeMeters);
    ImGui::Text("AI top targets: enemies=%u host=%u proxy=%u other=%u nearest host/proxy %.1fm/%.1fm",
        m_aiDebugEnemyCount,
        m_aiDebugHostTopTargetCount,
        m_aiDebugProxyTopTargetCount,
        m_aiDebugOtherTopTargetCount,
        m_aiDebugNearestHostDistance,
        m_aiDebugNearestProxyDistance);

    if (ImGui::Button("Recover proxy"))
    {
        RestoreProxyRuntimeHealth();
        if (ArkNpc* npc = GetProxyNpc())
            RecoverLiveNetworkNpc(*npc);
        if (IEntity* proxyEntity = GetProxyEntity())
            ApplySurvivorFactionToProxy(*proxyEntity);
    }
    ImGui::SameLine();
    if (ImGui::Button("Respawn proxy only"))
    {
        Vec3 position = GetOffsetFromPlayer(kProxyForwardOffsetMeters);
        Quat rotation = ArkPlayer::GetInstance().GetEntity()->GetRotation();
        if (IEntity* proxyEntity = GetProxyEntity())
        {
            position = proxyEntity->GetWorldPos();
            rotation = proxyEntity->GetWorldRotation();
        }
        RemoveProxyOnly();
        SpawnProxyOnly(position, rotation);
    }
    if (m_networkMode == CoopNetworkMode::Host)
    {
        if (ImGui::Button("Scan host enemies now"))
            ScanHostEnemyRegistry();
        ImGui::SameLine();
        if (ImGui::Button("Bind proxy target now"))
            BindHostEnemiesToProxyTarget();
        ImGui::SameLine();
        if (ImGui::Button("Stimulate proxy combat now"))
            TickHostProxyCombatStimulus(kProxyCombatStimulusSeconds);
    }
    else if (m_networkMode == CoopNetworkMode::Client)
    {
        if (ImGui::Button("Cull client local enemies now"))
            CullClientLocalEnemies();
    }

    if (!m_enableExpensiveAiDebug)
    {
        ImGui::TextWrapped("Detailed NPC perception rows are disabled. Enable 'Expensive AI perception debug' in Advanced tuning only while paused or isolated.");
        return;
    }

    DrawNpcDebugLine("Proxy", m_proxyEntityId, 0, "remote-player-proxy");
    DrawNpcDebugLine("Mimic", m_mimicEntityId, kTestMimicNetId, m_mimicIsPuppet ? "test-puppet" : "test-authority");

    size_t shown = 0;
    for (const auto& entry : m_enemyAuthorities)
    {
        if (shown++ >= 12)
            break;
        const std::string label = "HostEnemy " + std::to_string(entry.first);
        DrawNpcDebugLine(label.c_str(), entry.second.entityId, entry.first, "host-authority");
    }

    shown = 0;
    for (const auto& entry : m_enemyPuppets)
    {
        if (shown++ >= 12)
            break;
        const std::string label = "Puppet " + std::to_string(entry.first);
        DrawNpcDebugLine(label.c_str(), entry.second.entityId, entry.first, entry.second.dead ? "puppet-dead" : "puppet-alive");
    }
}

void ModMain::SpawnProxy()
{
    if (!IsGameReady())
    {
        LogCoop("Spawn proxy ignored: game is not ready");
        return;
    }

    Vec3 pos = GetOffsetFromPlayer(kProxyForwardOffsetMeters);
    Quat rot = ArkPlayer::GetInstance().GetEntity()->GetRotation();
    LogCoop("Spawning proxy");
    RemoveProxyOnly();
    IEntity* entity = SpawnProxyOnly(pos, rot);

    if (!entity)
    {
        LogCoop("Spawn proxy failed");
        return;
    }

    m_followLocalPlayer = false;
    LogCoop("Proxy spawned");
}

void ModMain::ConfigureProxy()
{
    ArkNpc* npc = GetProxyNpc();
    if (!npc)
    {
        LogCoop("Configure proxy failed: proxy is not an ArkNpc");
        return;
    }

    LogCoop("Configuring proxy");
    SetupProxyNpc(*npc);
    if (IEntity* proxyEntity = GetProxyEntity())
        ApplyProxyName(*proxyEntity, GetRemoteUsernameOrFallback());
    LogCoop("Proxy configured");
}

IEntity* ModMain::SpawnMimicAt(const Vec3& position, const Quat& rotation, const char* name, bool puppet)
{
    if (!IsGameReady())
        return nullptr;

    const std::vector<EntityId> beforeSpawn = CaptureRuntimeEntityIdSnapshot("spawn mimic before");

    if (m_mimicEntityId != INVALID_ENTITYID)
    {
        PrepareCoopEntityForRemoval(m_mimicEntityId, false, m_mimicIsPuppet, "spawn mimic replace old mimic");
        RemoveCoopEntityGuarded(m_mimicEntityId, true, "spawn mimic replace old mimic");
    }

    Vec3 pos = position;
    Quat rot = rotation;
    IEntity* entity = EntityUtils::SpawnNpc(name, pos, rot, kMimicArchetype);
    RecordCoopSpawnDiagnostics(name ? name : "mimic", beforeSpawn, entity);
    m_mimicEntityId = entity ? entity->GetId() : INVALID_ENTITYID;
    m_mimicIsPuppet = entity ? puppet : false;
    m_mimicHealthAvailable = false;
    m_hasLastMimicAuthorityPos = false;
    m_lastMimicHealth = 0.0f;
    m_lastMimicMaxHealth = 0.0f;
    m_lastMimicDamage = 0.0f;
    m_sentMimicDeadState = false;
    m_mimicDeathCommitRepeatsRemaining = 0;

    if (entity && puppet)
    {
        MarkCoopRuntimeEntity(*entity, true);
        if (ArkNpc* npc = EntityUtils::GetArkNpc(entity))
            SetupMimicPuppet(*npc);

        EnemyPuppetState& puppetState = m_enemyPuppets[kTestMimicNetId];
        puppetState.entityId = entity->GetId();
        puppetState.archetypeId = kMimicArchetype;
        puppetState.dead = false;
    }
    else if (entity)
    {
        MarkCoopRuntimeEntity(*entity, false);
        m_hadAuthoritativeMimic = true;
    }

    return entity;
}

IEntity* ModMain::SpawnDebugEnemyAt(uint64_t archetypeId, const Vec3& position, const Quat& rotation, const char* name)
{
    if (!IsGameReady() || archetypeId == 0)
        return nullptr;

    Vec3 spawnPosition = position;
    Quat spawnRotation = rotation;
    const std::vector<EntityId> beforeSpawn = CaptureRuntimeEntityIdSnapshot("spawn debug enemy before");
    IEntity* entity = EntityUtils::SpawnNpc(name ? name : "CoopDebugEnemy", spawnPosition, spawnRotation, archetypeId);
    RecordCoopSpawnDiagnostics(name ? name : "debug_enemy", beforeSpawn, entity);
    if (!entity)
    {
        m_lastEnemyAuthorityEvent =
            "debug enemy spawn failed arch=" + std::to_string(archetypeId);
        m_networkStatus =
            "spawn enemy failed: archetype " + std::to_string(archetypeId) +
            " unavailable; verify the game assets and CoopPrototype build";
        return nullptr;
    }

    MarkCoopRuntimeEntity(*entity, false);
    if (std::find(m_debugSpawnedEnemyEntityIds.begin(), m_debugSpawnedEnemyEntityIds.end(), entity->GetId()) ==
        m_debugSpawnedEnemyEntityIds.end())
    {
        m_debugSpawnedEnemyEntityIds.push_back(entity->GetId());
    }

    ArkTurret* turret = nullptr;
    std::string turretGuardReason;
    if (TryGuardedCall("debug enemy spawn EntityUtils::GetArkTurret", [entity]() { return EntityUtils::GetArkTurret(entity); }, turret, &turretGuardReason) &&
        turret)
    {
        const bool snapshotQueued = QueueLocalTurretSnapshotEventForEntity(*entity, "debug turret spawned");
        m_lastEnemyAuthorityEvent =
            "debug static turret spawned entity=" + std::to_string(entity->GetId()) +
            " arch=" + std::to_string(archetypeId) +
            " snapshot=" + std::to_string(snapshotQueued ? 1 : 0);
        m_networkStatus = m_lastEnemyAuthorityEvent;
        LogCoop(m_lastEnemyAuthorityEvent);
        return entity;
    }

    EnemyAuthorityState& state = EnsureEnemyAuthorityState(*entity);
    state.localAttentionClaimed = false;
    state.localAttentionPendingSeconds = 0.0f;
    state.localAttentionClaimedSeconds = 0.0f;
    state.localAttentionLostSeconds = 0.0f;
    state.remoteLocomotionAuthority = false;
    state.remoteAuthorityBlocked = false;
    state.remoteAuthorityHasAttention = false;
    state.authorityAttentionLevel = CoopEnemyAuthorityPolicy::kUnknownAttention;
    state.attentionCandidates.clear();
    state.remoteTargetAccountToken = 0;
    state.remoteAuthoritySilentSeconds = 0.0f;
    state.remoteAuthorityBlockedSeconds = 0.0f;

    m_hadAuthoritativeMimic = true;
    m_lastEnemyAuthorityEvent =
        "debug enemy spawned net=" + std::to_string(state.netId) +
        " entity=" + std::to_string(entity->GetId()) +
        " arch=" + std::to_string(archetypeId);
    m_networkStatus = m_lastEnemyAuthorityEvent;
    LogCoop(m_lastEnemyAuthorityEvent);
    return entity;
}

void ModMain::ApplyMimicStateToPuppet(const CoopProtocol::TestMimicStatePacket& packet)
{
    ApplyEnemyStateToPuppet(packet);
}

bool ModMain::ShouldSuppressRemoteCystoidChildState(uint64_t archetypeId, const Vec3& position, const char* reason)
{
    if (m_networkMode != CoopNetworkMode::Client || archetypeId != kCystoidArchetype)
        return false;

    if (!IsFiniteVec3(position))
        return true;

    const bool invalidOriginState = position.GetLengthSquared() < 1.0f;
    const bool remoteNestChildState =
        invalidOriginState ||
        (!m_networkOriginatedCystoidNestZones.empty() &&
            IsPositionInNetworkOriginatedCystoidNestZone(position, reason));
    if (!remoteNestChildState)
        return false;

    m_lastEnemyLocomotionEvent =
        "suppressed_remote_cystoid_child_state arch=" + std::to_string(archetypeId) +
        " pos=(" + std::to_string(position.x) + "," + std::to_string(position.y) + "," + std::to_string(position.z) + ")" +
        " zones=" + std::to_string(m_networkOriginatedCystoidNestZones.size()) +
        " reason=" + (reason && reason[0] ? std::string(reason) : std::string("-"));
    AppendEnemySyncTrace("pose", m_lastEnemyLocomotionEvent);
    return true;
}

void ModMain::MarkNetworkConsumedCystoidNest(uint64_t enemyNetId, EntityId entityId, const Vec3& position, const char* reason)
{
    if (enemyNetId == 0)
        return;

    m_networkConsumedCystoidNestNetIds.insert(enemyNetId);
    if (entityId != INVALID_ENTITYID && entityId != 0)
        m_networkConsumedCystoidNestEntityIds.insert(entityId);

    m_lastEnemyAbilityFxEvent =
        "marked_remote_cystoid_nest_consumed net=" + std::to_string(enemyNetId) +
        " entity=" + std::to_string(entityId) +
        " pos=(" + std::to_string(position.x) + "," + std::to_string(position.y) + "," + std::to_string(position.z) + ")" +
        " count=" + std::to_string(m_networkConsumedCystoidNestNetIds.size()) +
        " reason=" + (reason && reason[0] ? std::string(reason) : std::string("-"));
    AppendEnemySyncTrace("enemy_ability_fx", m_lastEnemyAbilityFxEvent);
}

bool ModMain::IsNetworkConsumedCystoidNest(uint64_t enemyNetId, EntityId entityId) const
{
    if (enemyNetId != 0 && m_networkConsumedCystoidNestNetIds.find(enemyNetId) != m_networkConsumedCystoidNestNetIds.end())
        return true;

    return entityId != INVALID_ENTITYID &&
        entityId != 0 &&
        m_networkConsumedCystoidNestEntityIds.find(entityId) != m_networkConsumedCystoidNestEntityIds.end();
}

bool ModMain::ForceNetworkConsumedCystoidNestState(
    uint64_t enemyNetId,
    IEntity* entity,
    const Vec3& position,
    const Quat& rotation,
    const char* reason)
{
    if (!IsNetworkConsumedCystoidNest(enemyNetId, entity ? entity->GetId() : INVALID_ENTITYID))
        return false;

    if (entity)
    {
        entity->SetPosRotScale(position, rotation, entity->GetScale(), 0);
        ApplyEntityPhysicsVelocity(*entity, Vec3(ZERO));
        TryGuardedVoidCall(
            "remote consumed cystoid nest hide",
            [entity]() { entity->Hide(true); },
            nullptr);
        SetEntityHealthFromAuthority(entity->GetId(), 0.0f, true, true);
        if (enemyNetId != 0)
            m_enemyNetIdsByEntity[entity->GetId()] = enemyNetId;
    }

    if (const auto authorityIt = m_enemyAuthorities.find(enemyNetId); authorityIt != m_enemyAuthorities.end())
    {
        authorityIt->second.remoteLocomotionAuthority = false;
        authorityIt->second.remoteAuthorityHasAttention = false;
        authorityIt->second.authorityAttentionLevel = CoopEnemyAuthorityPolicy::kUnknownAttention;
        authorityIt->second.attentionCandidates.clear();
        authorityIt->second.remoteTargetAccountToken = 0;
        authorityIt->second.hasLastPosition = false;
    }
    if (const auto puppetIt = m_enemyPuppets.find(enemyNetId); puppetIt != m_enemyPuppets.end())
        puppetIt->second.dead = true;

    ClearRemoteEnemyMovementDesire(enemyNetId, reason ? reason : "remote consumed cystoid nest");

    m_lastEnemyLocomotionEvent =
        "forced_remote_cystoid_nest_consumed net=" + std::to_string(enemyNetId) +
        " entity=" + std::to_string(entity ? entity->GetId() : INVALID_ENTITYID) +
        " reason=" + (reason && reason[0] ? std::string(reason) : std::string("-"));
    AppendEnemySyncTrace("pose", m_lastEnemyLocomotionEvent);
    return true;
}

void ModMain::DiscardRemoteEnemyBinding(uint64_t enemyNetId, const char* reason)
{
    if (enemyNetId == 0)
        return;

    auto removeRuntimeEntity = [&](EntityId entityId)
    {
        if (entityId == INVALID_ENTITYID || !gEnv || !gEnv->pEntitySystem)
            return;

        IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId);
        if (!entity)
            return;

        const uint32_t flags = entity->GetFlags();
        const bool runtimeEntity =
            (flags & kCoopRuntimeEntityFlags) == kCoopRuntimeEntityFlags ||
            IsCoopRuntimeEntityName(entity->GetName());
        if (!runtimeEntity)
            return;

        PrepareCoopEntityForRemoval(entityId, false, true, reason ? reason : "discard remote enemy binding");
        RemoveCoopEntityGuarded(entityId, false, reason ? reason : "discard remote enemy binding");
    };

    const auto puppetIt = m_enemyPuppets.find(enemyNetId);
    if (puppetIt != m_enemyPuppets.end())
    {
        const EntityId entityId = puppetIt->second.entityId;
        m_enemyNetIdsByEntity.erase(entityId);
        m_enemyStableSpawnIdsByEntity.erase(entityId);
        removeRuntimeEntity(entityId);
        m_enemyPuppets.erase(puppetIt);
    }

    const auto authorityIt = m_enemyAuthorities.find(enemyNetId);
    if (authorityIt != m_enemyAuthorities.end())
    {
        const EntityId entityId = authorityIt->second.entityId;
        if ((authorityIt->second.rosterFlags &
                CoopProtocol::kEnemyRosterFlagEthericDoppelganger) != 0)
        {
            for (auto& sourceEntry : m_enemyAuthorities)
            {
                EnemyAuthorityState& sourceState = sourceEntry.second;
                if (sourceState.stableEnemyId != authorityIt->second.sourceStableEnemyId)
                    continue;
                sourceState.activeEthericDoppelgangerStableEnemyId = 0;
                if (gEnv && gEnv->pEntitySystem &&
                    sourceState.entityId != INVALID_ENTITYID)
                {
                    if (IEntity* sourceEntity =
                            gEnv->pEntitySystem->GetEntity(sourceState.entityId))
                    {
                        if (ArkNpc* sourceNpc = EntityUtils::GetArkNpc(sourceEntity))
                        {
                            TryGuardedVoidCall(
                                "discard Etheric Doppelganger native relation",
                                [sourceNpc]()
                                {
                                    sourceNpc->SetEthericDoppelgangerId(INVALID_ENTITYID);
                                },
                                nullptr);
                        }
                    }
                }
                break;
            }
        }
        m_enemyNetIdsByEntity.erase(entityId);
        m_enemyStableSpawnIdsByEntity.erase(entityId);
        m_enemyEthericDoppelgangerSourcesByEntity.erase(entityId);
        m_enemyEthericDoppelgangerGenerationsByEntity.erase(entityId);
        m_enemyEthericDoppelgangerRequestsSentByEntity.erase(entityId);
        ClearRemoteEnemyMovementDesire(enemyNetId, reason ? reason : "discard remote enemy binding");
        removeRuntimeEntity(entityId);
        m_enemyAuthorities.erase(authorityIt);
    }

    // Keep sequence guards intact: this helper removes only the invalid body
    // binding, not packet dedupe state for already rejected remote child spam.
}

void ModMain::ApplyEnemyStateToPuppet(const CoopProtocol::TestMimicStatePacket& packet)
{
    const uint64_t enemyNetId = packet.enemyNetId ? packet.enemyNetId : kTestMimicNetId;
    const auto existingPuppetIt = m_enemyPuppets.find(enemyNetId);
    const uint64_t previousPuppetArchetype =
        existingPuppetIt != m_enemyPuppets.end() ? existingPuppetIt->second.archetypeId : 0;
    uint64_t archetypeId = packet.archetypeId ? packet.archetypeId : previousPuppetArchetype;
    if (archetypeId == 0 && enemyNetId == kTestMimicNetId)
        archetypeId = kMimicArchetype;
    if (archetypeId == 0)
        return;

    const bool dead = (packet.flags & CoopProtocol::kTestMimicStateFlagDead) != 0;
    const bool hidden = (packet.flags & CoopProtocol::kTestMimicStateFlagHidden) != 0;
    const bool healthKnown = (packet.flags & CoopProtocol::kTestMimicStateFlagHealthKnown) != 0;
    const Vec3 position(packet.px, packet.py, packet.pz);
    const Quat rotation(packet.qw, packet.qx, packet.qy, packet.qz);
    if (IsNetworkConsumedCystoidNest(enemyNetId, INVALID_ENTITYID))
    {
        IEntity* consumedEntity = nullptr;
        if (existingPuppetIt != m_enemyPuppets.end() &&
            existingPuppetIt->second.entityId != INVALID_ENTITYID &&
            gEnv &&
            gEnv->pEntitySystem)
        {
            consumedEntity = gEnv->pEntitySystem->GetEntity(existingPuppetIt->second.entityId);
        }
        if (!consumedEntity)
        {
            const auto authorityIt = m_enemyAuthorities.find(enemyNetId);
            if (authorityIt != m_enemyAuthorities.end() &&
                authorityIt->second.entityId != INVALID_ENTITYID &&
                gEnv &&
                gEnv->pEntitySystem)
            {
                consumedEntity = gEnv->pEntitySystem->GetEntity(authorityIt->second.entityId);
            }
        }
        ForceNetworkConsumedCystoidNestState(
            enemyNetId,
            consumedEntity,
            position,
            rotation,
            "remote puppet consumed cystoid nest state");
        return;
    }
    if (ShouldSuppressRemoteCystoidChildState(archetypeId, position, "remote puppet state"))
    {
        DiscardRemoteEnemyBinding(enemyNetId, "suppressed remote cystoid child puppet state");
        return;
    }

    EnemyPuppetState& puppet = m_enemyPuppets[enemyNetId];
    puppet.archetypeId = archetypeId;

    if (enemyNetId == kTestMimicNetId)
    {
        if (healthKnown)
        {
            m_mimicHealthAvailable = true;
            m_lastMimicHealth = packet.health;
            m_lastMimicMaxHealth = packet.maxHealth;
        }
        else
        {
            m_mimicHealthAvailable = false;
        }
    }

    IEntity* entity = nullptr;
    if (gEnv && gEnv->pEntitySystem && puppet.entityId != INVALID_ENTITYID)
        entity = gEnv->pEntitySystem->GetEntity(puppet.entityId);

    if (!entity && enemyNetId == kTestMimicNetId && gEnv && gEnv->pEntitySystem &&
        m_mimicEntityId != INVALID_ENTITYID && m_mimicIsPuppet)
    {
        entity = gEnv->pEntitySystem->GetEntity(m_mimicEntityId);
        if (entity)
            puppet.entityId = m_mimicEntityId;
    }

    if (!entity)
    {
        entity = TryAdoptClientLocalEnemyAsPuppet(enemyNetId, archetypeId, position);
    }

    if (!entity && hidden)
    {
        puppet.dead = false;
        return;
    }

    if (!entity)
    {
        const std::string puppetName = "CoopEnemyPuppet_" + std::to_string(enemyNetId);
        Vec3 spawnPosition = position;
        Quat spawnRotation = rotation;
        entity = EntityUtils::SpawnNpc(puppetName.c_str(), spawnPosition, spawnRotation, archetypeId);
        if (entity)
        {
            puppet.entityId = entity->GetId();
            MarkCoopRuntimeEntity(*entity, true);
            if (ArkNpc* npc = EntityUtils::GetArkNpc(entity))
                SetupMimicPuppet(*npc);

            if (enemyNetId == kTestMimicNetId)
            {
                m_mimicEntityId = puppet.entityId;
                m_mimicIsPuppet = true;
            }
        }
    }

    if (!entity)
        return;

    if (enemyNetId == kTestMimicNetId)
    {
        m_mimicEntityId = entity->GetId();
        m_mimicIsPuppet = true;
    }

    if (dead)
    {
        ApplyEnemyDeathCommitToLocal(
            enemyNetId,
            archetypeId,
            &position,
            &rotation,
            &packet,
            "legacy enemy state awaiting native death presentation");
        return;
    }

    if (hidden)
    {
        entity->SetPosRotScale(position, rotation, entity->GetScale(), 0);
        ApplyEntityPhysicsVelocity(*entity, Vec3(ZERO));
        entity->Hide(true);
        if (healthKnown)
            SetEntityHealthFromAuthority(entity->GetId(), packet.health, false, false);
        puppet.dead = false;
        return;
    }

    entity->Hide(false);

    const Vec3 currentPosition = entity->GetWorldPos();
    const Vec3 delta = position - currentPosition;
    const float deltaSq = delta.GetLengthSquared();
    Vec3 appliedPosition = position;
    if (deltaSq < kMimicPuppetHardSnapDistance * kMimicPuppetHardSnapDistance)
    {
        const float blend = deltaSq > kMimicPuppetSoftSnapDistance * kMimicPuppetSoftSnapDistance ? 0.55f : 0.25f;
        appliedPosition = currentPosition + delta * blend;
    }

    entity->SetPosRotScale(appliedPosition, rotation, entity->GetScale(), 0);
    ApplyEntityPhysicsVelocity(*entity, Vec3(ZERO));

    if (healthKnown)
        SetEntityHealthFromAuthority(entity->GetId(), packet.health, false, false);

    puppet.dead = false;
}

IEntity* ModMain::TryBindClientLocalEnemyForLocomotion(
    uint64_t enemyNetId,
    uint64_t archetypeId,
    const Vec3& position,
    const Quat& rotation,
    uint64_t sourceGuid,
    bool allowSpawn)
{
    if ((m_networkMode != CoopNetworkMode::Client && m_networkMode != CoopNetworkMode::Host) ||
        enemyNetId == 0 ||
        archetypeId == 0 ||
        !gEnv ||
        !gEnv->pEntitySystem)
    {
        return nullptr;
    }

    const EnemyRosterRecord* rosterRecord = nullptr;
    if (const auto rosterIt = m_enemyRosterByNetId.find(enemyNetId); rosterIt != m_enemyRosterByNetId.end())
        rosterRecord = &rosterIt->second;
    const bool storyCritical = rosterRecord &&
        (rosterRecord->flags & CoopProtocol::kEnemyRosterFlagStoryCritical) != 0;

    if (IsNetworkConsumedCystoidNest(enemyNetId, INVALID_ENTITYID))
    {
        m_lastEnemyLocomotionEvent =
            "ignored remote bind for consumed cystoid nest net=" + std::to_string(enemyNetId);
        AppendEnemySyncTrace("pose", m_lastEnemyLocomotionEvent);
        return nullptr;
    }

    if (ShouldSuppressRemoteCystoidChildState(archetypeId, position, "remote enemy bind"))
    {
        DiscardRemoteEnemyBinding(enemyNetId, "suppressed remote cystoid child bind");
        return nullptr;
    }

    auto isDeadOrHidden = [this](IEntity& entity) -> bool
    {
        std::string reason;
        bool hidden = false;
        if (TryGuardedCall("enemy locomotion bind IsHidden", [&entity]() { return entity.IsHidden(); }, hidden, &reason) && hidden)
            return true;

        if (ArkNpc* npc = EntityUtils::GetArkNpc(&entity))
        {
            bool dead = false;
            if (TryGuardedCall("enemy locomotion bind ArkNpc::IsDead", [npc]() { return npc->IsDead(); }, dead, &reason) && dead)
                return true;
        }

        float health = 0.0f;
        float maxHealth = 0.0f;
        return ReadEntityHealth(entity.GetId(), health, maxHealth) && health <= 0.0f;
    };

    auto entityCanUseLooseRuntimeGuidMatch = [&](IEntity& entity) -> bool
    {
        bool loadedFromLevel = false;
        TryGuardedCall(
            "enemy locomotion bind IsLoadedFromLevelFile",
            [&entity]() { return entity.IsLoadedFromLevelFile(); },
            loadedFromLevel,
            nullptr);
        return !loadedFromLevel &&
            (IsSpawnedRuntimeEnemyName(entity.GetName()) || IsCoopRuntimeEntityName(entity.GetName()));
    };

    auto entityMatchesSourceGuid = [&](IEntity& entity) -> bool
    {
        if (sourceGuid == 0)
            return true;

        uint64_t localGuid = 0;
        TryGuardedCall(
            "enemy locomotion bind GetGuid",
            [&entity]() -> EntityGUID { return entity.GetGuid(); },
            localGuid,
            nullptr);
        if (storyCritical)
        {
            if (sourceGuid != rosterRecord->stableEnemyId)
                return false;
            if (rosterRecord->entityGuid != 0)
                return localGuid == rosterRecord->entityGuid;

            const IEntityArchetype* localArchetype = entity.GetArchetype();
            const uint64_t localArchetypeId = localArchetype ? localArchetype->GetId() : 0;
            return ResolveEnemyStableId(entity, localArchetypeId, localGuid) == rosterRecord->stableEnemyId &&
                (BuildEnemyRosterFlags(entity) & CoopProtocol::kEnemyRosterFlagStoryCritical) != 0;
        }

        if (localGuid == sourceGuid ||
            (rosterRecord && rosterRecord->entityGuid != 0 && localGuid == rosterRecord->entityGuid))
            return true;

        const auto stableSpawnIt = m_enemyStableSpawnIdsByEntity.find(entity.GetId());
        if (stableSpawnIt != m_enemyStableSpawnIdsByEntity.end())
            return stableSpawnIt->second == sourceGuid;

        const IEntityArchetype* localArchetype = entity.GetArchetype();
        const uint64_t localArchetypeId = localArchetype ? localArchetype->GetId() : 0;
        if (ResolveEnemyStableId(entity, localArchetypeId, localGuid) == sourceGuid)
            return true;

        const auto netIt = m_enemyNetIdsByEntity.find(entity.GetId());
        if (netIt != m_enemyNetIdsByEntity.end() && netIt->second == enemyNetId)
        {
            const auto authorityIt = m_enemyAuthorities.find(enemyNetId);
            if (authorityIt != m_enemyAuthorities.end() &&
                authorityIt->second.entityGuid == sourceGuid)
            {
                return true;
            }
        }

        // Runtime-spawned NPCs do not preserve the same EntityGUID on both
        // peers. Treat the remote GUID as a strong exact-match hint first, but
        // fall back to nearest same-archetype binding for spawned Coop test
        // bodies instead of creating an extra replica.
        return !storyCritical && entityCanUseLooseRuntimeGuidMatch(entity);
    };

    auto isMatchingLiveEnemy = [&](IEntity* entity) -> bool
    {
        if (!entity)
            return false;

        const EntityId entityId = entity->GetId();
        if (entityId == INVALID_ENTITYID || IsRemoteProxyEntity(entityId))
            return false;

        if (!EntityUtils::GetArkNpc(entity))
            return false;

        const IEntityArchetype* archetype = entity->GetArchetype();
        if (!archetype || archetype->GetId() != archetypeId)
            return false;

        if (isDeadOrHidden(*entity))
            return false;

        if (!entityMatchesSourceGuid(*entity))
            return false;

        for (const auto& entry : m_enemyAuthorities)
        {
            if (sourceGuid == 0 && entry.first != enemyNetId && entry.second.entityId == entityId)
                return false;
        }

        return true;
    };

    auto removeRuntimeDuplicate = [&](EntityId duplicateId, const char* reason)
    {
        if (duplicateId == INVALID_ENTITYID || IsRemoteProxyEntity(duplicateId) || !gEnv || !gEnv->pEntitySystem)
            return;

        IEntity* duplicate = gEnv->pEntitySystem->GetEntity(duplicateId);
        if (!duplicate)
            return;

        const uint32_t flags = duplicate->GetFlags();
        const bool runtimeFlags = (flags & kCoopRuntimeEntityFlags) == kCoopRuntimeEntityFlags;
        if (!runtimeFlags && !IsCoopRuntimeEntityName(duplicate->GetName()))
            return;

        PrepareCoopEntityForRemoval(duplicateId, false, true, reason ? reason : "replace duplicate enemy body");
        RemoveCoopEntityGuarded(duplicateId, false, reason ? reason : "replace duplicate enemy body");
    };

    auto bindEntity = [&](IEntity& entity, const char* reason) -> IEntity*
    {
        const EntityId entityId = entity.GetId();
        if (entityId == INVALID_ENTITYID)
            return nullptr;

        for (auto& entry : m_enemyAuthorities)
        {
            if (entry.first == enemyNetId || entry.second.entityId != entityId)
                continue;

            entry.second.entityId = INVALID_ENTITYID;
            entry.second.hasLastPosition = false;
            entry.second.remoteLocomotionAuthority = false;
            entry.second.remoteAuthorityHasAttention = false;
            entry.second.authorityAttentionLevel = CoopEnemyAuthorityPolicy::kUnknownAttention;
            entry.second.attentionCandidates.clear();
            entry.second.remoteTargetAccountToken = 0;
            entry.second.remoteMannequinSequence = 0;
            entry.second.remoteMannequinFragmentId = -1;
            entry.second.remoteMannequinOrdinal = CoopProtocol::kInvalidMannequinOrdinal;
            entry.second.remoteMannequinFlags = 0;
            entry.second.remoteMannequinPriority = 0;
            entry.second.remoteMannequinTagState.fill(0);
            entry.second.remoteMannequinTagStateValid = false;
            entry.second.remoteMannequinRandomOption = false;
            entry.second.remoteMannequinCarryMovement = false;
            entry.second.localNativeMannequinActions.clear();
            entry.second.remoteNativeMannequinActions.clear();
            entry.second.remoteNativeMannequinRetiredOrder.clear();
            entry.second.remoteNativeMannequinRetiredActions.clear();
            entry.second.remoteNativeMirrorQueuedSequence = 0;
            entry.second.remoteNativeMirrorDiagnosedSequence = 0;
            entry.second.remoteNativeMirrorSuppressedSequence = 0;
            entry.second.remoteNativeMirrorRepairSequence = 0;
            entry.second.remoteNativeMirrorRepairWaitSeconds = 0.0f;
            ResetEnemySemanticReplicationState(entry.second);
        }

        for (auto it = m_enemyPuppets.begin(); it != m_enemyPuppets.end();)
        {
            if (it->first != enemyNetId && it->second.entityId == entityId)
                it = m_enemyPuppets.erase(it);
            else
                ++it;
        }

        const auto previousAuthorityIt = m_enemyAuthorities.find(enemyNetId);
        const EntityId previousAuthorityEntityId =
            previousAuthorityIt != m_enemyAuthorities.end() ? previousAuthorityIt->second.entityId : INVALID_ENTITYID;

        if (previousAuthorityEntityId != INVALID_ENTITYID && previousAuthorityEntityId != entityId)
        {
            m_enemyNetIdsByEntity.erase(previousAuthorityEntityId);
            m_enemyStableSpawnIdsByEntity.erase(previousAuthorityEntityId);
            removeRuntimeDuplicate(previousAuthorityEntityId, "replace duplicate remote enemy authority body");
        }

        const auto puppetIt = m_enemyPuppets.find(enemyNetId);
        if (puppetIt != m_enemyPuppets.end())
        {
            const EntityId previousPuppetEntityId = puppetIt->second.entityId;
            if (previousPuppetEntityId != INVALID_ENTITYID && previousPuppetEntityId != entityId)
            {
                m_enemyNetIdsByEntity.erase(previousPuppetEntityId);
                m_enemyStableSpawnIdsByEntity.erase(previousPuppetEntityId);
                removeRuntimeDuplicate(previousPuppetEntityId, "replace duplicate remote enemy puppet body");
            }
            m_enemyPuppets.erase(puppetIt);
        }

        EnemyAuthorityState& state = m_enemyAuthorities[enemyNetId];
        const bool resetRemoteBindingState =
            !state.remoteLocomotionAuthority ||
            state.entityId != entityId ||
            state.archetypeId != archetypeId ||
            (sourceGuid != 0 && state.entityGuid != 0 && state.entityGuid != sourceGuid);
        if (resetRemoteBindingState)
        {
            state.remoteMannequinSequence = 0;
            state.remoteMannequinFragmentId = -1;
            state.remoteMannequinOrdinal = CoopProtocol::kInvalidMannequinOrdinal;
            state.remoteMannequinFlags = 0;
            state.remoteMannequinAttackKind = 0;
            state.remoteMannequinPriority = 0;
            state.remoteMannequinTagState.fill(0);
            state.remoteMannequinTagStateValid = false;
            state.remoteMannequinRandomOption = false;
            state.remoteMannequinCarryMovement = false;
            state.localNativeMannequinActions.clear();
            state.remoteNativeMannequinActions.clear();
            state.remoteNativeMannequinRetiredOrder.clear();
            state.remoteNativeMannequinRetiredActions.clear();
            state.remoteNativeMirrorQueuedSequence = 0;
            state.remoteNativeMirrorDiagnosedSequence = 0;
            state.remoteNativeMirrorSuppressedSequence = 0;
            state.remoteNativeMirrorRepairSequence = 0;
            state.remoteNativeMirrorRepairWaitSeconds = 0.0f;
            state.remoteMannequinStateSeconds = 0.0f;
            state.remoteMovementHoldSeconds = 0.0f;
            state.remoteTargetMotionFilteredSpeed = 0.0f;
            state.remoteTargetMotionSeconds = 0.0f;
            state.remoteVisualMotionSeconds = 0.0f;
            state.remoteActionMotionBlockSeconds = 0.0f;
            state.localFocusCombatSeconds = 0.0f;
            state.remoteTargetMotionFlags = 0;
            state.remoteVisualMotionFlags = 0;
            state.remoteAuthorityPacketLocomotionFlags = 0;
            state.localNativeMovementSeconds = 0.0f;
            state.localNativeMoveSpeed = 0.0f;
            state.localNativeLocomotionFlags = 0;
            state.localPersistentStatusFlags = 0;
            state.localNativeMannequinStateSeconds = 0.0f;
            state.localNativeMannequinResolved = false;
            state.localMannequinStateSeconds = 0.0f;
            state.localMannequinFlags = 0;
            state.localMannequinAttackKind = 0;
            state.localMannequinSequence = 0;
            state.localMannequinPriority = 0;
            state.localMannequinOrdinal = CoopProtocol::kInvalidMannequinOrdinal;
            state.localMannequinFragmentId = -1;
            state.localMannequinTagState.fill(0);
            state.localMannequinTagStateValid = false;
            state.localMannequinCarryUntilReplaced = false;
            ResetEnemySemanticReplicationState(state);
            state.localAttentionClaimed = false;
            state.localAttentionPendingSeconds = 0.0f;
            state.localAttentionClaimedSeconds = 0.0f;
            state.localAttentionLostSeconds = 0.0f;
            state.localRotationOverrideSeconds = 0.0f;
            state.remoteTransformRewriteLastSeconds = -1.0f;
            state.remoteRotationRewriteLastSeconds = -1.0f;
            state.remotePositionSamples = {};
            state.remotePositionSampleCount = 0;
            state.remotePositionSampleIntervalSeconds = kEnemyStateActiveChangeIntervalSeconds;
            state.remoteInterpolationDelaySeconds = 0.0f;
            state.remoteInterpolationActive = false;
            state.remoteTransformNeedsAuthoritySnap = true;
        }
        state.entityId = entityId;
        state.netId = enemyNetId;
        state.archetypeId = archetypeId;
        if (rosterRecord)
        {
            state.stableEnemyId = rosterRecord->stableEnemyId;
            state.sourceStableEnemyId = rosterRecord->sourceStableEnemyId;
            state.rosterAreaId = rosterRecord->areaId;
            state.rosterVersion = rosterRecord->version;
            state.rosterFlags = rosterRecord->flags;
            // Ordinary parent roster records intentionally carry generation
            // zero.  Do not let a temporary remote-authority bind erase the
            // parent's monotonic Doppelganger cast counter; otherwise the
            // first recast after a handoff reuses generation 1 and its stable
            // child identity.  Lifecycle generations belong to child records.
            if ((rosterRecord->flags &
                    CoopProtocol::kEnemyRosterFlagEthericDoppelganger) != 0)
            {
                state.ethericDoppelgangerGeneration = rosterRecord->lifecycleGeneration;
            }
            m_enemyStableSpawnIdsByEntity[entityId] = rosterRecord->stableEnemyId;
        }
        if (sourceGuid != 0)
            state.entityGuid = sourceGuid;
        else
            TryGuardedCall(
                "enemy locomotion bind source GetGuid",
                [&entity]() -> EntityGUID { return entity.GetGuid(); },
                state.entityGuid,
                nullptr);
        uint64_t localEntityGuid = 0;
        TryGuardedCall(
            "enemy locomotion bind local GetGuid",
            [&entity]() -> EntityGUID { return entity.GetGuid(); },
            localEntityGuid,
            nullptr);
        if (sourceGuid != 0 && localEntityGuid == 0)
            m_enemyStableSpawnIdsByEntity[entityId] = sourceGuid;
        state.lastPosition = entity.GetWorldPos();
        state.lastRotation = entity.GetWorldRotation();
        state.remoteTargetPosition = resetRemoteBindingState ? position : state.lastPosition;
        state.remoteRawTargetPosition = resetRemoteBindingState ? position : state.remoteRawTargetPosition;
        state.remoteTargetRotation = resetRemoteBindingState ? rotation : state.lastRotation;
        state.hasLastPosition = true;
        m_enemyNetIdsByEntity[entityId] = enemyNetId;
        MarkCoopRuntimeEntity(entity, true);

        m_lastEnemyLocomotionEvent =
            std::string(reason ? reason : "bound remote enemy body") +
            " net=" + std::to_string(enemyNetId) +
            " entity=" + std::to_string(entityId) +
            " arch=" + std::to_string(archetypeId) +
            (resetRemoteBindingState ? " resetRemoteState=1" : "");
        return &entity;
    };

    auto spawnReplica = [&]() -> IEntity*
    {
        if (!allowSpawn || storyCritical)
        {
            ++m_enemyLocomotionDrops;
            m_lastEnemyLocomotionEvent =
                "bind existing failed net=" + std::to_string(enemyNetId) +
                " arch=" + std::to_string(archetypeId) +
                (sourceGuid != 0 ? " guid=" + std::to_string(sourceGuid) : std::string());
            return nullptr;
        }

        const std::string replicaName = "CoopEnemyReplica_" + std::to_string(enemyNetId);
        Vec3 spawnPosition = position;
        Quat spawnRotation = rotation;
        IEntity* spawned = EntityUtils::SpawnNpc(replicaName.c_str(), spawnPosition, spawnRotation, archetypeId);
        if (!spawned)
        {
            ++m_enemyLocomotionDrops;
            m_lastEnemyLocomotionEvent =
                "bind/spawn failed net=" + std::to_string(enemyNetId) +
                " arch=" + std::to_string(archetypeId) +
                (sourceGuid != 0 ? " guid=" + std::to_string(sourceGuid) : std::string());
            return nullptr;
        }

        MarkCoopRuntimeEntity(*spawned, true);
        EnemyAuthorityState& state = m_enemyAuthorities[enemyNetId];
        state = EnemyAuthorityState {};
        state.entityId = spawned->GetId();
        state.netId = enemyNetId;
        state.archetypeId = archetypeId;
        state.entityGuid = sourceGuid;
        state.lastPosition = spawned->GetWorldPos();
        state.lastRotation = spawned->GetWorldRotation();
        state.remoteTargetPosition = position;
        state.remoteRawTargetPosition = position;
        state.remoteTargetRotation = rotation;
        state.hasLastPosition = true;
        state.remoteTransformNeedsAuthoritySnap = true;
        if (rosterRecord)
        {
            state.stableEnemyId = rosterRecord->stableEnemyId;
            state.sourceStableEnemyId = rosterRecord->sourceStableEnemyId;
            state.rosterAreaId = rosterRecord->areaId;
            state.rosterVersion = rosterRecord->version;
            state.rosterFlags = rosterRecord->flags;
            if ((rosterRecord->flags &
                    CoopProtocol::kEnemyRosterFlagEthericDoppelganger) != 0)
            {
                state.ethericDoppelgangerGeneration = rosterRecord->lifecycleGeneration;
            }
        }
        m_enemyNetIdsByEntity[spawned->GetId()] = enemyNetId;
        if (sourceGuid != 0)
            m_enemyStableSpawnIdsByEntity[spawned->GetId()] = sourceGuid;
        ++m_enemyLocomotionBinds;
        m_lastEnemyLocomotionEvent =
            "spawned missing local enemy net=" + std::to_string(enemyNetId) +
            " entity=" + std::to_string(spawned->GetId()) +
            " arch=" + std::to_string(archetypeId) +
            (sourceGuid != 0 ? " guid=" + std::to_string(sourceGuid) : std::string());
        return spawned;
    };

    if (rosterRecord && rosterRecord->stableEnemyId != 0)
    {
        for (const auto& stableEntry : m_enemyStableSpawnIdsByEntity)
        {
            if (stableEntry.second != rosterRecord->stableEnemyId)
                continue;

            IEntity* stableEntity = gEnv->pEntitySystem->GetEntity(stableEntry.first);
            if (isMatchingLiveEnemy(stableEntity))
                return bindEntity(*stableEntity, "bound remote enemy by roster stable id");
        }
    }

    const auto puppetIt = m_enemyPuppets.find(enemyNetId);
    if (puppetIt != m_enemyPuppets.end() && puppetIt->second.entityId != INVALID_ENTITYID)
    {
        if (IEntity* puppetEntity = gEnv->pEntitySystem->GetEntity(puppetIt->second.entityId))
        {
            if (isMatchingLiveEnemy(puppetEntity))
                return bindEntity(*puppetEntity, "promoted existing same-net enemy puppet");

            m_enemyNetIdsByEntity.erase(puppetIt->second.entityId);
            m_enemyStableSpawnIdsByEntity.erase(puppetIt->second.entityId);
            m_enemyPuppets.erase(puppetIt);
            m_lastEnemyLocomotionEvent =
                "discarded dead/hidden same-net puppet net=" + std::to_string(enemyNetId) +
                " entity=" + std::to_string(puppetEntity->GetId());
        }
    }

    const auto existingIt = m_enemyAuthorities.find(enemyNetId);
    if (existingIt != m_enemyAuthorities.end() && existingIt->second.entityId != INVALID_ENTITYID)
    {
        if (IEntity* existingEntity = gEnv->pEntitySystem->GetEntity(existingIt->second.entityId))
        {
            if (isMatchingLiveEnemy(existingEntity))
                return bindEntity(*existingEntity, "reused existing remote enemy authority body");

            m_enemyNetIdsByEntity.erase(existingIt->second.entityId);
            m_enemyStableSpawnIdsByEntity.erase(existingIt->second.entityId);
            existingIt->second.entityId = INVALID_ENTITYID;
            existingIt->second.hasLastPosition = false;
            m_lastEnemyLocomotionEvent =
                "discarded dead/hidden binding net=" + std::to_string(enemyNetId) +
                " entity=" + std::to_string(existingEntity->GetId());
        }
    }

    if (sourceGuid != 0)
    {
        EntityId guidEntityId = INVALID_ENTITYID;
        const bool guidOk = TryGuardedCall(
            "enemy locomotion bind FindEntityByGuid",
            [sourceGuid]() -> EntityId
            {
                return gEnv->pEntitySystem->FindEntityByGuid(static_cast<EntityGUID>(sourceGuid));
            },
            guidEntityId,
            nullptr);
        if (guidOk && guidEntityId != INVALID_ENTITYID)
        {
            if (IEntity* guidEntity = gEnv->pEntitySystem->GetEntity(guidEntityId))
            {
                if (isMatchingLiveEnemy(guidEntity))
                    return bindEntity(*guidEntity, "bound remote enemy by host guid");

                m_lastEnemyLocomotionEvent =
                    "rejected host guid enemy bind net=" + std::to_string(enemyNetId) +
                    " entity=" + std::to_string(guidEntityId) +
                    " arch=" + std::to_string(archetypeId) +
                    " guid=" + std::to_string(sourceGuid);
            }
        }

        m_lastEnemyLocomotionEvent =
            "host guid enemy bind falling back to nearest runtime candidate net=" + std::to_string(enemyNetId) +
            " arch=" + std::to_string(archetypeId) +
            " guid=" + std::to_string(sourceGuid);
    }

    IEntityIt* iterator = gEnv->pEntitySystem->GetEntityIterator();
    if (!iterator)
        return nullptr;

    EntityId bestEntityId = INVALID_ENTITYID;
    float bestDistanceSq = kEnemyLocomotionBindDistanceMeters * kEnemyLocomotionBindDistanceMeters;

    iterator->MoveFirst();
    while (!iterator->IsEnd())
    {
        IEntity* entity = iterator->Next();
        if (!isMatchingLiveEnemy(entity))
            continue;

        const EntityId entityId = entity->GetId();

        bool boundAsOtherPuppet = false;
        for (const auto& entry : m_enemyPuppets)
        {
            if (entry.first != enemyNetId && entry.second.entityId == entityId)
            {
                boundAsOtherPuppet = true;
                break;
            }
        }
        if (boundAsOtherPuppet)
            continue;

        const float distanceSq = (entity->GetWorldPos() - position).GetLengthSquared();
        if (distanceSq >= bestDistanceSq)
            continue;

        bestDistanceSq = distanceSq;
        bestEntityId = entityId;
    }
    iterator->Release();

    if (bestEntityId == INVALID_ENTITYID)
        return spawnReplica();

    IEntity* entity = gEnv->pEntitySystem->GetEntity(bestEntityId);
    if (!entity)
        return nullptr;

    ++m_enemyLocomotionBinds;
    const float bestDistance = std::sqrt(std::max(0.0f, bestDistanceSq));
    const std::string bindReason = "recycled nearest same-archetype enemy body dist=" + std::to_string(bestDistance);
    return bindEntity(*entity, bindReason.c_str());
}

void ModMain::ApplyEnemyLocomotionStateToLocal(const CoopProtocol::TestMimicStatePacket& packet)
{
    if (!m_enemyLocomotionSyncEnabled ||
        (m_networkMode != CoopNetworkMode::Client && m_networkMode != CoopNetworkMode::Host) ||
        !IsGameReady())
    {
        return;
    }

    const uint64_t enemyNetId = packet.enemyNetId ? packet.enemyNetId : kTestMimicNetId;
    const uint64_t archetypeId = packet.archetypeId ? packet.archetypeId : kMimicArchetype;
    if (enemyNetId == 0 || archetypeId == 0)
        return;

    Vec3 targetPosition(packet.px, packet.py, packet.pz);
    const Vec3 packetTargetPosition = targetPosition;
    const Quat targetRotation(packet.qw, packet.qx, packet.qy, packet.qz);
    if (IsNetworkConsumedCystoidNest(enemyNetId, INVALID_ENTITYID))
    {
        IEntity* consumedEntity = nullptr;
        const auto authorityIt = m_enemyAuthorities.find(enemyNetId);
        if (authorityIt != m_enemyAuthorities.end() &&
            authorityIt->second.entityId != INVALID_ENTITYID &&
            gEnv &&
            gEnv->pEntitySystem)
        {
            consumedEntity = gEnv->pEntitySystem->GetEntity(authorityIt->second.entityId);
        }
        ForceNetworkConsumedCystoidNestState(
            enemyNetId,
            consumedEntity,
            targetPosition,
            targetRotation,
            "remote locomotion consumed cystoid nest state");
        return;
    }

    if ((packet.flags & CoopProtocol::kTestMimicStateFlagDead) != 0)
    {
        ApplyEnemyDeathCommitToLocal(
            enemyNetId,
            archetypeId,
            &targetPosition,
            &targetRotation,
            &packet,
            "enemy locomotion death commit");
        return;
    }

    if (ShouldSuppressRemoteCystoidChildState(archetypeId, targetPosition, "remote locomotion state"))
    {
        DiscardRemoteEnemyBinding(enemyNetId, "suppressed remote cystoid child locomotion state");
        return;
    }

    if ((packet.flags & CoopProtocol::kTestMimicStateFlagHidden) != 0)
    {
        m_lastEnemyLocomotionEvent =
            "ignored hidden in locomotion v0 net=" + std::to_string(enemyNetId);
        return;
    }

    bool allowDynamicSpawn = false;
    if (const auto rosterIt = m_enemyRosterByNetId.find(enemyNetId); rosterIt != m_enemyRosterByNetId.end())
    {
        allowDynamicSpawn =
            (rosterIt->second.flags & CoopProtocol::kEnemyRosterFlagDynamicSpawn) != 0 &&
            (rosterIt->second.flags & CoopProtocol::kEnemyRosterFlagStoryCritical) == 0;
    }
    IEntity* entity = TryBindClientLocalEnemyForLocomotion(
        enemyNetId,
        archetypeId,
        targetPosition,
        targetRotation,
        packet.entityGuid,
        m_networkMode == CoopNetworkMode::Client && allowDynamicSpawn);
    if (!entity)
        return;

    // Health follows the exact per-enemy authority lease. The session Host is
    // the default owner and relay, but a Client claim makes that Client the
    // authoritative simulation source until the lease is released or revoked.
    if (packet.authorityOwnerAccountToken != 0 &&
        packet.authorityOwnerAccountToken != GetLocalAccountToken() &&
        (packet.flags & CoopProtocol::kTestMimicStateFlagHealthKnown) != 0)
    {
        if (!std::isfinite(packet.health) || packet.health < 0.0f)
        {
            ++m_enemyHealthReconcileFailures;
            m_lastEnemyHealthEvent =
                "rejected invalid authoritative health net=" + std::to_string(enemyNetId);
        }
        else if (packet.health > 0.0f)
        {
            float localHealth = 0.0f;
            float localMaxHealth = 0.0f;
            if (!ReadEntityHealth(entity->GetId(), localHealth, localMaxHealth))
            {
                ++m_enemyHealthReconcileFailures;
                m_lastEnemyHealthEvent =
                    "failed to read local health net=" + std::to_string(enemyNetId) +
                    " entity=" + std::to_string(entity->GetId());
            }
            else if (std::fabs(localHealth - packet.health) > kEnemyHealthReconcileThreshold)
            {
                if (SetEntityHealthFromAuthority(entity->GetId(), packet.health, false, false))
                {
                    ++m_enemyHealthReconciles;
                    m_lastEnemyHealthEvent =
                        "applied authoritative health net=" + std::to_string(enemyNetId) +
                        " entity=" + std::to_string(entity->GetId()) +
                        " from=" + std::to_string(localHealth) +
                        " to=" + std::to_string(packet.health);
                    AppendEnemySyncTrace("health", m_lastEnemyHealthEvent);
                }
                else
                {
                    ++m_enemyHealthReconcileFailures;
                    m_lastEnemyHealthEvent =
                        "failed to apply authoritative health net=" + std::to_string(enemyNetId) +
                        " entity=" + std::to_string(entity->GetId());
                }
            }
        }
        else
        {
            // A zero-health packet must travel through the reliable death
            // commit above; do not create a zero-health living NPC while that
            // commit is in flight.
            m_lastEnemyHealthEvent =
                "awaiting death commit for zero health net=" + std::to_string(enemyNetId);
        }
    }

    if (auto rosterIt = m_enemyRosterByNetId.find(enemyNetId);
        rosterIt != m_enemyRosterByNetId.end() &&
        (rosterIt->second.flags & CoopProtocol::kEnemyRosterFlagRaisedFromCorpse) != 0)
    {
        const auto nativeRaisedIt = m_enemyRaisedFromCorpseSourcesByEntity.find(entity->GetId());
        if (nativeRaisedIt != m_enemyRaisedFromCorpseSourcesByEntity.end() &&
            nativeRaisedIt->second == rosterIt->second.sourceStableEnemyId)
        {
            rosterIt->second.raisedPresentationApplied = true;
        }
    }

    if (const auto rosterIt = m_enemyRosterByNetId.find(enemyNetId);
        rosterIt != m_enemyRosterByNetId.end() &&
        (rosterIt->second.flags & CoopProtocol::kEnemyRosterFlagRaisedFromCorpse) != 0 &&
        !rosterIt->second.raisedPresentationApplied)
    {
        if (ArkNpc* raisedNpc = EntityUtils::GetArkNpc(entity))
        {
            std::string raiseReason;
            if (TryGuardedVoidCall(
                    "remote raised phantom StartRaiseFromCorpse",
                    [raisedNpc]() { raisedNpc->StartRaiseFromCorpse(true); },
                    &raiseReason))
            {
                rosterIt->second.raisedPresentationApplied = true;
                ++m_corpsePhantomRaiseApplies;
                m_lastCorpsePhantomEvent =
                    "remote_raise_applied_net_" + std::to_string(enemyNetId) +
                    "_entity_" + std::to_string(entity->GetId()) +
                    "_source_" + std::to_string(rosterIt->second.sourceStableEnemyId);
            }
            else
            {
                m_lastCorpsePhantomEvent =
                    "remote_raise_failed_net_" + std::to_string(enemyNetId) +
                    "_reason_" + raiseReason;
            }
        }
    }

    if (const auto rosterIt = m_enemyRosterByNetId.find(enemyNetId);
        rosterIt != m_enemyRosterByNetId.end() &&
        (rosterIt->second.flags & CoopProtocol::kEnemyRosterFlagEthericDoppelganger) != 0)
    {
        EnemyAuthorityState& childState = m_enemyAuthorities[enemyNetId];
        childState.stableEnemyId = rosterIt->second.stableEnemyId;
        childState.sourceStableEnemyId = rosterIt->second.sourceStableEnemyId;
        childState.rosterAreaId = rosterIt->second.areaId;
        childState.rosterVersion = rosterIt->second.version;
        childState.rosterFlags = rosterIt->second.flags;
        childState.ethericDoppelgangerGeneration = rosterIt->second.lifecycleGeneration;
        m_enemyStableSpawnIdsByEntity[entity->GetId()] = childState.stableEnemyId;
        m_enemyEthericDoppelgangerSourcesByEntity[entity->GetId()] =
            childState.sourceStableEnemyId;
        m_enemyEthericDoppelgangerGenerationsByEntity[entity->GetId()] =
            childState.ethericDoppelgangerGeneration;
        if (!childState.ethericDoppelgangerRelationApplied)
        {
            ApplyEthericDoppelgangerRelation(
                childState, *entity, "remote roster locomotion bind");
        }
    }

    EnemyAuthorityState* existingState = nullptr;
    const auto existingIt = m_enemyAuthorities.find(enemyNetId);
    if (existingIt != m_enemyAuthorities.end())
    {
        existingState = &existingIt->second;
        const bool authorityGenerationChanged =
            existingState->authorityOwnerAccountToken != packet.authorityOwnerAccountToken ||
            existingState->authorityEpoch != packet.authorityEpoch;
        if (authorityGenerationChanged)
        {
            ResetEnemySemanticReplicationState(*existingState);
            existingState->remoteAuthorityPacketLocomotionFlags = 0;
            existingState->localNativeMannequinActions.clear();
            existingState->remoteNativeMannequinActions.clear();
            existingState->remoteNativeMannequinRetiredOrder.clear();
            existingState->remoteNativeMannequinRetiredActions.clear();
            existingState->remoteNativeMirrorQueuedSequence = 0;
            existingState->remoteNativeMirrorDiagnosedSequence = 0;
            existingState->remoteNativeMirrorSuppressedSequence = 0;
            existingState->remoteNativeMirrorRepairSequence = 0;
            existingState->remoteNativeMirrorRepairWaitSeconds = 0.0f;
        }
    }

    const bool localAuthorityBlockedForSnapshot = m_localPlayerDowned || IsLocalPlayerAuthorityBlockedByModalState();
    const bool localHasAttention = !localAuthorityBlockedForSnapshot && LocalPlayerHasEnemyAwarenessForCoop(*entity);
    const bool remoteAuthorityBlocked =
        (packet.sourceFlags & CoopProtocol::kEnemyStateSourceFlagAuthorityBlocked) != 0;
    const bool remoteAuthorityHasAttention =
        (packet.sourceFlags & CoopProtocol::kEnemyStateSourceFlagAuthorityHasAttention) != 0;
    const bool remoteExplicitlyOwnsEnemy =
        remoteAuthorityHasAttention &&
        !remoteAuthorityBlocked;
    if (existingState &&
        existingState->localAttentionClaimed &&
        remoteExplicitlyOwnsEnemy)
    {
        existingState->localAttentionClaimed = false;
        existingState->localAttentionClaimedSeconds = 0.0f;
        existingState->localAttentionPendingSeconds = 0.0f;
        existingState->localAttentionLostSeconds = 0.0f;
        m_lastEnemyAuthorityEvent =
            "client local claim superseded by explicit remote authority net=" + std::to_string(enemyNetId);
    }

    CoopEnemyControlPolicy::Context controlContext;
    controlContext.networkMode =
        m_networkMode == CoopNetworkMode::Host
            ? CoopEnemyControlPolicy::NetworkMode::Host
            : CoopEnemyControlPolicy::NetworkMode::Client;
    controlContext.localAuthorityBlocked = localAuthorityBlockedForSnapshot;
    controlContext.localHasAttention = localHasAttention;
    controlContext.localAttentionClaimed = existingState && existingState->localAttentionClaimed;
    controlContext.localRotationOverrideActive =
        existingState && existingState->localRotationOverrideSeconds > 0.0f;
    controlContext.remoteLocomotionAuthority = true;
    controlContext.remoteAuthorityHasAttention = remoteAuthorityHasAttention;
    controlContext.remoteTargetsLocalPlayer =
        packet.targetAccountToken != 0 &&
        packet.targetAccountToken == GetLocalAccountToken();
    controlContext.remoteTargetLocallyRepresented =
        ResolveLocallyRepresentedEnemyTarget(packet.targetAccountToken) != INVALID_ENTITYID;
    controlContext.hasLastPosition = true;
    controlContext.localTargetMixEnabled = true;
    const CoopEnemyControlPolicy::Decision controlDecision =
        CoopEnemyControlPolicy::Evaluate(controlContext);
    SetRemoteEnemyMirrorPhysics(
        *entity,
        controlDecision.blockWorldCollision,
        "remote enemy mirror policy");

    uint32_t effectiveLocomotionFlags = packet.locomotionFlags;
    int effectiveMannequinFragmentId = packet.mannequinFragmentId;
    uint32_t effectiveMannequinSequence = packet.mannequinSequence;
    uint16_t effectiveMannequinOrdinal = packet.mannequinOrdinal;
    int32_t effectiveMannequinPriority = packet.mannequinPriority;
    std::array<uint8_t, 12> effectiveMannequinTagState = {};
    std::copy(
        std::begin(packet.mannequinTagState),
        std::end(packet.mannequinTagState),
        effectiveMannequinTagState.begin());
    bool effectiveMannequinTagStateValid = packet.mannequinTagStateValid != 0;
    bool effectiveMannequinRandomOption =
        (packet.mannequinReserved & CoopProtocol::kEnemyMannequinReservedNativeRandomOption) != 0;
    uint32_t effectiveAttackKind = packet.attackKind;
    bool effectiveMannequinCarriesMovement =
        (packet.mannequinReserved & CoopProtocol::kEnemyMannequinReservedCarryMovement) != 0;
    const bool heldRemoteMannequinForPose =
        packet.mannequinSequence == 0 &&
        existingState &&
        existingState->remoteMannequinStateSeconds > 0.0f &&
        existingState->remoteMannequinSequence != 0 &&
        existingState->remoteMannequinFragmentId >= 0 &&
        CanHoldRemoteMannequinStateForPacket(packet.locomotionFlags, existingState->remoteMannequinFlags);
    if (heldRemoteMannequinForPose)
    {
        effectiveLocomotionFlags = MergeHeldRemoteMannequinFlags(
            effectiveLocomotionFlags,
            existingState->remoteMannequinFlags);
        effectiveMannequinFragmentId = existingState->remoteMannequinFragmentId;
        effectiveMannequinSequence = existingState->remoteMannequinSequence;
        effectiveMannequinOrdinal = existingState->remoteMannequinOrdinal;
        effectiveMannequinPriority = existingState->remoteMannequinPriority;
        effectiveMannequinTagState = existingState->remoteMannequinTagState;
        effectiveMannequinTagStateValid = existingState->remoteMannequinTagStateValid;
        effectiveMannequinRandomOption = existingState->remoteMannequinRandomOption;
        effectiveAttackKind = existingState->remoteMannequinAttackKind;
        effectiveMannequinCarriesMovement = existingState->remoteMannequinCarryMovement;
    }
    const bool authorityGlooed =
        (effectiveLocomotionFlags & CoopProtocol::kEnemyLocomotionFlagGlooed) != 0;
    const bool authorityRagdolled =
        (effectiveLocomotionFlags & CoopProtocol::kEnemyLocomotionFlagRagdolled) != 0;
    const bool wasRemoteGlooed =
        existingState &&
        (existingState->remoteLocomotionFlags & CoopProtocol::kEnemyLocomotionFlagGlooed) != 0;
    const bool authorityMindControlled =
        (effectiveLocomotionFlags & CoopProtocol::kEnemyLocomotionFlagMindControlled) != 0;
    const bool authorityPsiSuppressed =
        (effectiveLocomotionFlags & CoopProtocol::kEnemyLocomotionFlagPsiSuppressed) != 0;
    const bool authorityHacked =
        (packet.mannequinReserved & CoopProtocol::kEnemyMannequinReservedHacked) != 0;
    const bool authorityCorrupted =
        (packet.mannequinReserved & CoopProtocol::kEnemyMannequinReservedCorrupted) != 0;
    const bool authorityFactionValid =
        (packet.mannequinReserved & CoopProtocol::kEnemyMannequinReservedFactionValid) != 0;
    const unsigned authorityFaction = static_cast<unsigned>(
        (packet.mannequinReserved & CoopProtocol::kEnemyMannequinReservedFactionMask) >>
        CoopProtocol::kEnemyMannequinReservedFactionShift);
    const uint32_t movementFlags = CoopEnemyControlPolicy::MovementFlags();
    const uint32_t continuousMovementFlags = CoopEnemyControlPolicy::ContinuousMovementFlags();
    const bool wasAuthorityMoving =
        existingState &&
        (existingState->remoteLocomotionFlags & continuousMovementFlags) != 0;
    float remoteActionMotionBlockSeconds =
        existingState
            ? std::max(0.0f, existingState->remoteActionMotionBlockSeconds - kMimicStateTickSeconds)
            : 0.0f;
    const bool activeRemoteActionBlocksMotionInference =
        remoteActionMotionBlockSeconds > 0.0f;
    const uint32_t actionOnlyFlags =
        CoopProtocol::kEnemyLocomotionFlagAttacking |
        CoopProtocol::kEnemyLocomotionFlagHitReacting |
        CoopProtocol::kEnemyLocomotionFlagStunned |
        CoopProtocol::kEnemyLocomotionFlagCowering |
        CoopProtocol::kEnemyLocomotionFlagRagdolled |
        CoopProtocol::kEnemyLocomotionFlagMannequinDriven;
    const uint32_t appliedLane =
        controlDecision.localFocus
            ? CoopProtocol::kEnemyLocomotionLevelLocalFocus
            : (remoteAuthorityHasAttention
                ? CoopProtocol::kEnemyLocomotionLevelFullAuthority
                : CoopProtocol::kEnemyLocomotionLevelAuthorityLocomotionLocalCombat);
    const bool wasRemoteLocomotionAuthority =
        existingState && existingState->remoteLocomotionAuthority;
    const bool remoteAuthoritySnapPending =
        existingState && existingState->remoteTransformNeedsAuthoritySnap;

    // Never let the short rotation grace refresh itself merely because it is
    // already active. Only current native awareness may extend the mix; once
    // Vanilla loses suspicion/attention, packet traffic must allow it to
    // expire back to the strict remote mirror.
    if (localHasAttention && existingState)
        existingState->localRotationOverrideSeconds = kEnemyLocalRotationOverrideGraceSeconds;

    const bool keepLocalRotation = controlDecision.localFocus && !controlDecision.blockTurn;

    const Vec3 currentPosition = entity->GetWorldPos();
    const Vec3 packetDelta = packetTargetPosition - currentPosition;
    const float packetDeltaSq = packetDelta.GetLengthSquared();
    const float packetDeltaLen = std::sqrt(packetDeltaSq);
    const Vec3 previousRemotePacketTarget =
        existingState && existingState->hasLastPosition
            ? existingState->remoteRawTargetPosition
            : packetTargetPosition;
    const Vec3 packetTargetStep = packetTargetPosition - previousRemotePacketTarget;
    const float packetTargetStepLen = packetTargetStep.GetLength();
    std::string effectiveAuthorityFragmentName;
    bool effectiveAuthorityFragmentCarriesMovement = false;
    bool rejectedReservedMovementCarryForFragment = false;
    if (effectiveMannequinSequence != 0 && effectiveMannequinFragmentId >= 0)
    {
        effectiveAuthorityFragmentName = ResolveNpcMannequinFragmentNameForRuntime(entity, effectiveMannequinFragmentId);
        const bool fragmentNameCarriesMovement =
            IsEnemyMannequinMovementCarryFragmentName(effectiveAuthorityFragmentName);
        rejectedReservedMovementCarryForFragment =
            effectiveMannequinCarriesMovement &&
            !fragmentNameCarriesMovement;
        effectiveAuthorityFragmentCarriesMovement =
            fragmentNameCarriesMovement;
    }
    const bool passiveAuthorityMovementCarry =
        effectiveAuthorityFragmentCarriesMovement &&
        IsEnemyMannequinPassiveMovementCarryFragmentName(effectiveAuthorityFragmentName);
    const uint32_t burstMovementFlags = CoopEnemyControlPolicy::BurstMovementFlags();
    const uint32_t preMotionDecisionLocomotionFlags = effectiveLocomotionFlags;
    const bool authorityStationaryMannequinBlocksMotionInference =
        !controlDecision.localFocus &&
        effectiveMannequinSequence != 0 &&
        effectiveMannequinFragmentId >= 0 &&
        !effectiveAuthorityFragmentCarriesMovement &&
        (preMotionDecisionLocomotionFlags & (continuousMovementFlags | burstMovementFlags)) == 0 &&
        (preMotionDecisionLocomotionFlags & CoopProtocol::kEnemyLocomotionFlagGlooed) == 0;
    if (authorityStationaryMannequinBlocksMotionInference)
    {
        remoteActionMotionBlockSeconds = std::max(
            remoteActionMotionBlockSeconds,
            kEnemyRemoteActionMotionInferenceBlockSeconds);
    }
    const bool remoteActionBlocksMotionInferenceNow =
        activeRemoteActionBlocksMotionInference ||
        authorityStationaryMannequinBlocksMotionInference;
    // Burst presentation is sourced strictly from the raw authority wire.
    // Held/local combat Mannequin mixing can legally add action flags, but it
    // must never synthesize another Phantom Shift edge.
    const uint32_t incomingBurstFlags = packet.locomotionFlags & burstMovementFlags;
    const uint32_t previousBurstFlags = existingState
        ? (existingState->remoteAuthorityPacketLocomotionFlags & burstMovementFlags)
        : 0u;
    const bool burstMannequinSequenceChanged =
        effectiveMannequinSequence != 0 &&
        effectiveMannequinFragmentId >= 0 &&
        (!existingState ||
            existingState->remoteMannequinSequence != effectiveMannequinSequence ||
            existingState->remoteMannequinFragmentId != effectiveMannequinFragmentId);
    const bool burstLargeAuthorityDelta =
        packetDeltaLen >= kEnemyRemoteSmoothBurstHardJumpDistance ||
        packetTargetStepLen >= kEnemyRemoteSmoothBurstHardJumpDistance;
    const bool explicitRemoteBurstEvent =
        incomingBurstFlags != 0 &&
        (
            burstMannequinSequenceChanged ||
            previousBurstFlags == 0 ||
            burstLargeAuthorityDelta);
    CoopEnemyControlPolicy::RemoteMotionContext motionContext;
    motionContext.mode = controlDecision.mode;
    motionContext.locomotionFlags = effectiveLocomotionFlags;
    motionContext.authorityPacketLocomotionFlags = packet.locomotionFlags;
    motionContext.previousLocomotionFlags = existingState ? existingState->remoteLocomotionFlags : 0;
    motionContext.mannequinSequence = static_cast<int>(effectiveMannequinSequence);
    motionContext.mannequinFragmentId = effectiveMannequinFragmentId;
    motionContext.packetSpeed = packet.speed;
    motionContext.packetTargetStepLen = packetTargetStepLen;
    motionContext.packetDeltaLen = packetDeltaLen;
    motionContext.previousFilteredTargetSpeed =
        existingState ? existingState->remoteTargetMotionFilteredSpeed : 0.0f;
    motionContext.previousMotionSeconds =
        existingState ? existingState->remoteTargetMotionSeconds : 0.0f;
    motionContext.tickSeconds = kMimicStateTickSeconds;
    motionContext.movementIntentSpeedThreshold = kEnemyMovementIntentSpeedThreshold;
    motionContext.targetMotionInferenceMinStep = kEnemyRemoteTargetMotionInferenceMinStep;
    motionContext.targetMotionInferenceMinSpeed = kEnemyRemoteTargetMotionInferenceMinSpeed;
    motionContext.catchupIntentDistance = kEnemyRemoteCatchupIntentDistance;
    motionContext.receiverInferRunSpeed = kEnemyRemoteReceiverInferRunSpeed;
    motionContext.passiveInferredMaxWalkSpeed = kEnemyRemotePassiveInferredMaxWalkSpeed;
    motionContext.movementHoldSeconds = kEnemyRemoteMovementIntentHoldSeconds;
    motionContext.hasExistingState = existingState != nullptr;
    motionContext.existingRemoteLocomotionAuthority = existingState && existingState->remoteLocomotionAuthority;
    motionContext.authorityGlooed = authorityGlooed;
    motionContext.inferenceDisabled = EnvFlagEnabled("COOP_DISABLE_REMOTE_ENEMY_TARGET_MOTION_INFERENCE");
    motionContext.heldAuthorityMannequin = heldRemoteMannequinForPose;
    motionContext.authorityMannequinMovementCarry = effectiveAuthorityFragmentCarriesMovement;
    // An attack action may occupy the upper-body Mannequin scope while the
    // authority continues ordinary walking. Explicit Walking/Running plus an
    // actual packet path step is stronger evidence than the action fragment
    // name, and keeps the replicated legs moving without inventing motion.
    const bool explicitAuthorityContinuousMotion =
        (effectiveLocomotionFlags & continuousMovementFlags) != 0 &&
        ((std::isfinite(packet.speed) && packet.speed > kEnemyMovementIntentSpeedThreshold &&
            packetTargetStepLen >= kEnemyRemotePacketMotionApplyMinDistance) ||
            packetTargetStepLen >= kEnemyRemoteTargetMotionInferenceMinStep);
    const bool authorityActionAllowsMovementAnimation =
        effectiveMannequinSequence == 0 ||
        effectiveMannequinFragmentId < 0 ||
        effectiveAuthorityFragmentCarriesMovement ||
        explicitAuthorityContinuousMotion ||
        (effectiveLocomotionFlags & (CoopEnemyControlPolicy::HardOverrideFlags() |
            CoopEnemyControlPolicy::BurstMovementFlags())) != 0 ||
        (effectiveLocomotionFlags & CoopEnemyControlPolicy::ActionFlagsMask()) == 0;
    motionContext.authorityActionAllowsMovementAnimation = authorityActionAllowsMovementAnimation;
    // Ordinary authority actions are deliberately not presented in auth0att1.
    // They therefore cannot veto the independent authority locomotion/leg lane.
    motionContext.activeRemoteActionBlocksMotionInference =
        !controlDecision.localFocus && remoteActionBlocksMotionInferenceNow;
    motionContext.explicitBurstEvent = explicitRemoteBurstEvent;
    const CoopEnemyControlPolicy::RemoteMotionDecision motionDecision =
        CoopEnemyControlPolicy::ResolveRemoteMotion(motionContext);
    const uint32_t inferredAuthorityMovementFlags = motionDecision.inferredMovementFlags;
    const float inferredAuthorityTargetSpeed = motionDecision.inferredTargetSpeed;
    const bool inferredAuthorityCatchupDrift = motionDecision.inferredCatchupDrift;
    const bool remoteMovementHeldFromPrevious = motionDecision.heldPreviousMovement;
    const bool confirmedAuthorityTargetMotion = motionDecision.confirmedTargetMotion;
    const bool strippedUnconfirmedActionMovement = motionDecision.strippedUnconfirmedActionMovement;
    const bool strippedHeldPassiveMovement = motionDecision.strippedHeldPassiveMovement;
    const bool heldPassiveMovementEvidence = motionDecision.heldPassiveMovementEvidence;
    const bool targetMotionInferenceBlockedByAction = motionDecision.targetMotionInferenceBlockedByAction;
    const bool blockedPreviousMovementHold = motionDecision.blockedPreviousMovementHold;
    const bool blockedPassiveDriftOnlyMotion = motionDecision.blockedPassiveDriftOnlyMotion;
    const bool passiveActionMovementHoldEvidence = motionDecision.passiveActionMovementHoldEvidence;
    const bool passiveNonCarryActionMotionBlocked = motionDecision.passiveNonCarryActionMotionBlocked;
    const bool localTargetActionMotionEvidence = motionDecision.localTargetActionMotionEvidence;
    const bool remoteActionTargetMotionEvidence = motionDecision.remoteActionTargetMotionEvidence;
    const uint32_t rawEffectiveLocomotionFlags = motionDecision.locomotionFlags;
    effectiveLocomotionFlags = rawEffectiveLocomotionFlags;
    const bool remoteDashOrShift =
        (effectiveLocomotionFlags &
            (CoopProtocol::kEnemyLocomotionFlagDashing | CoopProtocol::kEnemyLocomotionFlagShifting)) != 0;
    const bool remoteBurstMovement =
        (effectiveLocomotionFlags & CoopEnemyControlPolicy::BurstMovementFlags()) != 0;
    if (!authorityGlooed &&
        !remoteBurstMovement &&
        (motionDecision.activeActionBlockedMovementAnimation || targetMotionInferenceBlockedByAction))
    {
        remoteActionMotionBlockSeconds = std::max(
            remoteActionMotionBlockSeconds,
            kEnemyRemoteActiveActionMotionBlockSeconds);
    }
    const bool strippedContinuousMovementDuringBurst =
        remoteBurstMovement && (effectiveLocomotionFlags & continuousMovementFlags) != 0;
    if (strippedContinuousMovementDuringBurst)
        effectiveLocomotionFlags &= ~continuousMovementFlags;
    const float previousRemoteBurstTransformSeconds =
        existingState ? std::max(0.0f, existingState->remoteBurstTransformSeconds - kMimicStateTickSeconds) : 0.0f;
    // Do not tie this hold to the current packet's burst bit. Phantom Shift is
    // commonly an edge, while the visible dash spans several render frames.
    // The first burst packet resets the interpolation history below; this
    // timer keeps interpolation paused until fresh post-dash samples exist.
    const float nextRemoteBurstTransformSeconds =
        motionDecision.explicitBurstEvent
            ? kEnemyRemoteBurstTransformSmoothingSeconds
            : previousRemoteBurstTransformSeconds;
    const bool remoteBurstSmoothingActive = nextRemoteBurstTransformSeconds > 0.0f;
    const bool authorityMovementFlagIntent =
        (effectiveLocomotionFlags & continuousMovementFlags) != 0;
    // Burst locomotion (Phantom Shift, Mimic morph hops, etc.) is an action
    // event plus a pose correction, not a native "walk there" desire on
    // observers. Treating burst smoothing as movement feeds the receiver's
    // MovementDesire/MoveIntegrator and makes dash look like fast walking.
    const bool authorityMovementIntent = authorityMovementFlagIntent;
    const bool authorityMovementAnimationIntent =
        authorityMovementIntent &&
        motionDecision.currentMovementAnimationEvidence &&
        (
            !inferredAuthorityCatchupDrift ||
            confirmedAuthorityTargetMotion);
    const bool continuousMotionBlockedByAction =
        !remoteBurstSmoothingActive &&
        !authorityGlooed &&
        (
            motionDecision.activeActionBlockedMovementAnimation ||
            targetMotionInferenceBlockedByAction ||
            (authorityMovementIntent && !motionDecision.currentMovementAnimationEvidence));
    const uint32_t animationLocomotionFlags = authorityMovementAnimationIntent
        ? effectiveLocomotionFlags
        : (effectiveLocomotionFlags & ~continuousMovementFlags);
    const bool authorityActionIntent =
        !authorityMovementIntent &&
        effectiveMannequinSequence != 0 &&
        effectiveMannequinFragmentId >= 0 &&
        (effectiveLocomotionFlags & actionOnlyFlags) != 0;
    const bool remoteMoveIntegrator = authorityMovementIntent && !authorityGlooed;
    const bool remoteRun =
        (effectiveLocomotionFlags & CoopProtocol::kEnemyLocomotionFlagRunning) != 0;
    const Vec3 packetMoveDirection =
        NormalizeDirectionOr(Vec3(packet.mx, packet.my, packet.mz), targetRotation.GetColumn1());
    const bool deriveMoveDirectionFromTarget =
        authorityMovementIntent &&
        (packetDeltaSq > 0.0025f || packetTargetStepLen > 0.05f);
    const Vec3 effectiveRemoteMoveDirection = deriveMoveDirectionFromTarget
        ? (packetTargetStepLen > 0.05f
            ? NormalizeDirectionOr(packetTargetStep, packetMoveDirection)
            : NormalizeDirectionOr(packetDelta, packetMoveDirection))
        : packetMoveDirection;
    float effectiveRemoteSpeed = packet.speed;
    if (authorityMovementIntent)
    {
        const float minimumSpeed = remoteDashOrShift
            ? kEnemyRemoteReceiverMinDashSpeed
            : (remoteRun ? kEnemyRemoteReceiverMinRunSpeed : kEnemyRemoteReceiverMinWalkSpeed);
        const float maximumSpeed = remoteDashOrShift
            ? kEnemyRemoteReceiverMaxDashSpeed
            : (remoteRun ? kEnemyRemoteReceiverMaxRunSpeed : kEnemyRemoteReceiverMaxWalkSpeed);
        const float effectiveMaximumSpeed = passiveAuthorityMovementCarry
            ? std::min(maximumSpeed, kEnemyRemotePassiveCarryMaxTravelSpeed)
            : maximumSpeed;
        const float packetMotionSpeed =
            std::isfinite(effectiveRemoteSpeed) && effectiveRemoteSpeed > kEnemyMovementIntentSpeedThreshold
                ? effectiveRemoteSpeed
                : 0.0f;
        const float authorityMotionSpeed = std::max(packetMotionSpeed, motionDecision.filteredTargetSpeed);
        const float requestedSpeed =
            authorityMotionSpeed > kEnemyMovementIntentSpeedThreshold
                ? authorityMotionSpeed
                : minimumSpeed;
        effectiveRemoteSpeed = std::clamp(requestedSpeed, minimumSpeed, effectiveMaximumSpeed);
    }
    float remoteTargetPredictionDistance = 0.0f;
    const bool inferredRemoteTargetPrediction =
        inferredAuthorityMovementFlags != 0;
    if (!authorityGlooed &&
        authorityMovementAnimationIntent &&
        !remoteBurstSmoothingActive &&
        (effectiveLocomotionFlags & continuousMovementFlags) != 0 &&
        effectiveRemoteSpeed > kEnemyMovementIntentSpeedThreshold &&
        (packetTargetStepLen >= kEnemyRemoteTargetMotionInferenceMinStep ||
            motionDecision.motionSeconds > 0.0f))
    {
        const Vec3 rawTargetDelta = packetTargetPosition - currentPosition;
        const float lagAlongAuthorityDirection = rawTargetDelta.Dot(effectiveRemoteMoveDirection);
        if (lagAlongAuthorityDirection > -0.05f)
        {
            const float predictionSeconds = inferredRemoteTargetPrediction
                ? kEnemyRemoteInferredTargetPredictionSeconds
                : passiveAuthorityMovementCarry
                ? kEnemyRemotePassiveCarryTargetPredictionSeconds
                : kEnemyRemoteTargetPredictionSeconds;
            const float predictionMaxDistance = inferredRemoteTargetPrediction
                ? kEnemyRemoteInferredTargetPredictionMaxDistance
                : passiveAuthorityMovementCarry
                ? kEnemyRemotePassiveCarryTargetPredictionMaxDistance
                : kEnemyRemoteTargetPredictionMaxDistance;
            remoteTargetPredictionDistance = std::clamp(
                effectiveRemoteSpeed * predictionSeconds,
                0.0f,
                predictionMaxDistance);
            targetPosition = packetTargetPosition + effectiveRemoteMoveDirection * remoteTargetPredictionDistance;
        }
    }
    const Vec3 delta = targetPosition - currentPosition;
    const float deltaSq = delta.GetLengthSquared();
    const float deltaLen = std::sqrt(deltaSq);
    const bool authorityCatchupCorrection =
        !authorityGlooed &&
        authorityMovementFlagIntent &&
        deltaSq > kEnemyRemoteCatchupIntentDistance * kEnemyRemoteCatchupIntentDistance;
    const bool remoteIdleCorrection =
        !authorityGlooed &&
        !authorityMovementFlagIntent &&
        deltaSq > kEnemyRemoteCatchupIntentDistance * kEnemyRemoteCatchupIntentDistance;
    CoopProtocol::TestMimicStatePacket effectivePacket = packet;
    effectivePacket.locomotionFlags = animationLocomotionFlags;
    effectivePacket.mannequinFragmentId = effectiveMannequinFragmentId;
    effectivePacket.mannequinSequence = effectiveMannequinSequence;
    effectivePacket.mannequinOrdinal = effectiveMannequinOrdinal;
    effectivePacket.mannequinPriority = effectiveMannequinPriority;
    std::copy(
        effectiveMannequinTagState.begin(),
        effectiveMannequinTagState.end(),
        std::begin(effectivePacket.mannequinTagState));
    effectivePacket.mannequinTagStateValid = effectiveMannequinTagStateValid ? 1u : 0u;
    if (effectiveMannequinRandomOption)
        effectivePacket.mannequinReserved |= CoopProtocol::kEnemyMannequinReservedNativeRandomOption;
    else
        effectivePacket.mannequinReserved &= ~CoopProtocol::kEnemyMannequinReservedNativeRandomOption;
    effectivePacket.attackKind = effectiveAttackKind;
    effectivePacket.speed = effectiveRemoteSpeed;
    effectivePacket.mx = effectiveRemoteMoveDirection.x;
    effectivePacket.my = effectiveRemoteMoveDirection.y;
    effectivePacket.mz = effectiveRemoteMoveDirection.z;
    CoopProtocol::TestMimicStatePacket remoteAnimationPacket = effectivePacket;
    const uint32_t transformLocomotionFlags =
        remoteBurstSmoothingActive
            ? effectiveLocomotionFlags
            : ((continuousMotionBlockedByAction
                    ? (effectiveLocomotionFlags & ~continuousMovementFlags)
                    : effectiveLocomotionFlags) &
                ~CoopEnemyControlPolicy::BurstMovementFlags());
    const uint32_t transformPacketLocomotionFlags =
        remoteBurstSmoothingActive
            ? effectivePacket.locomotionFlags
            : ((continuousMotionBlockedByAction
                    ? (effectivePacket.locomotionFlags & ~continuousMovementFlags)
                    : effectivePacket.locomotionFlags) &
                ~CoopEnemyControlPolicy::BurstMovementFlags());
    float remoteSmoothBaseTravelSpeed =
        ComputeRemoteEnemySmoothBaseTravelSpeed(
            transformLocomotionFlags,
            transformPacketLocomotionFlags,
            effectiveRemoteSpeed);
    bool remotePassiveStepSmoothSpeedCarrier = false;
    if (remoteSmoothBaseTravelSpeed <= kEnemyMovementIntentSpeedThreshold &&
        !authorityMovementIntent &&
        packetTargetStepLen >= kEnemyRemoteTargetMotionInferenceMinStep &&
        CoopEnemyControlPolicy::IsPassiveMannequinFlags(
            effectiveLocomotionFlags | effectivePacket.locomotionFlags))
    {
        const float packetStepSpeed = packetTargetStepLen / kMimicStateTickSeconds;
        if (std::isfinite(packetStepSpeed) && packetStepSpeed > kEnemyMovementIntentSpeedThreshold)
        {
            remoteSmoothBaseTravelSpeed =
                std::clamp(packetStepSpeed, 0.0f, kEnemyRemoteReceiverMaxWalkSpeed);
            remotePassiveStepSmoothSpeedCarrier = true;
        }
    }
    const bool remotePassiveSmoothSpeedCarrier =
        remoteSmoothBaseTravelSpeed > kEnemyMovementIntentSpeedThreshold &&
        !authorityMovementIntent &&
        CoopEnemyControlPolicy::IsPassiveMannequinFlags(
            effectiveLocomotionFlags | effectivePacket.locomotionFlags);
    bool remotePoseHardCorrected = false;
    bool remotePoseHeldForNativeMove = false;
    bool remotePoseSoftCorrected = false;
    bool remotePoseDashCorrected = false;
    bool remotePoseActionHeld = false;
    bool remotePoseIdleAnchored = false;
    bool remotePoseSmoothed = false;
    bool remotePoseSmoothHard = false;
    bool remotePoseSmoothTiny = false;
    bool remoteRotationSmoothed = false;
    bool remoteRotationSmoothHard = false;
    float remotePoseSmoothStep = 0.0f;
    float remotePoseSmoothSpeed = 0.0f;
    float remoteRotationAngle = 0.0f;
    float remoteRotationSmoothAlpha = 1.0f;
    float remoteRotationSmoothSpeed = 0.0f;

    Quat currentRotation = entity->GetWorldRotation();
    currentRotation.Normalize();
    Vec3 appliedPosition = currentPosition;
    Quat appliedRotation = currentRotation;
    const bool remoteExplicitBurstEvent = motionDecision.explicitBurstEvent;
    bool remoteExplicitBurstCorrected = false;
    bool remoteExplicitBurstSnap = false;
    const bool remoteInitialAuthoritySnap =
        remoteAuthoritySnapPending &&
        !authorityGlooed &&
        deltaLen > kEnemyRemoteSmoothTinySnapDistance;
    if (remoteInitialAuthoritySnap)
    {
        appliedPosition = targetPosition;
        remotePoseSmoothed = true;
        remotePoseSmoothHard = false;
        remotePoseSoftCorrected = false;
        remotePoseDashCorrected = false;
        remotePoseIdleAnchored = false;
        remotePoseSmoothStep = deltaLen;
        remotePoseSmoothSpeed = deltaLen / std::max(kMimicStateTickSeconds, 0.001f);
    }
    else if (remoteExplicitBurstEvent &&
        motionDecision.snapBurstToAuthority &&
        deltaLen > kEnemyRemoteSmoothTinySnapDistance)
    {
        // A real Burst/Shift is an authored teleport-style locomotion event,
        // not ordinary path following. Let the action/FX carry the visual dash
        // and put the mirror on the authority endpoint immediately; otherwise
        // observers see the phantom "fast-walk" through the correction path.
        appliedPosition = targetPosition;
        remotePoseSmoothed = true;
        remotePoseSmoothHard = false;
        remotePoseSoftCorrected = false;
        remotePoseDashCorrected = true;
        remotePoseIdleAnchored = false;
        remotePoseSmoothStep = deltaLen;
        remotePoseSmoothSpeed = deltaLen / kMimicStateTickSeconds;
        remoteExplicitBurstCorrected = true;
        remoteExplicitBurstSnap = true;
    }
    else if (remoteExplicitBurstEvent && deltaLen > kEnemyRemoteSmoothTinySnapDistance)
    {
        // Shift/Dash is a discrete locomotion event. Keep it on the central
        // dash-event lane, but still move through the normal rubber-band
        // envelope. Snapping the full delta in one packet makes the remote
        // body visibly fight animation/root-motion during Phantom Shift.
        bool burstHardJump = false;
        remotePoseSmoothStep =
            ComputeRemoteEnemySmoothStep(
                deltaLen,
                remoteSmoothBaseTravelSpeed,
                true,
                -1.0f,
                burstHardJump,
                remotePoseSmoothSpeed);
        remotePoseSmoothed = true;
        remotePoseSmoothHard = false;
        remotePoseSoftCorrected = true;
        remotePoseDashCorrected = true;
        remotePoseIdleAnchored = false;
        appliedPosition = currentPosition + delta * (remotePoseSmoothStep / deltaLen);
        remoteExplicitBurstCorrected = true;
        // This branch is the smooth burst-correction lane. Even if the final
        // smoothing step reaches the endpoint in this tick, it is not the hard
        // snap policy path above and must not be reported as dashSnap.
        remoteExplicitBurstSnap = false;
    }
    else if (deltaLen <= kEnemyRemoteSmoothTinySnapDistance)
    {
        remotePoseSmoothTiny = deltaLen > 0.0001f;
        remotePoseActionHeld = authorityActionIntent;
        appliedPosition = targetPosition;
    }
    else
    {
        remotePoseSmoothStep =
            ComputeRemoteEnemySmoothStep(
                deltaLen,
                remoteSmoothBaseTravelSpeed,
                remoteBurstSmoothingActive,
                passiveAuthorityMovementCarry
                    ? kEnemyRemotePassiveCarryMaxCorrectionSpeed
                    : -1.0f,
                remotePoseSmoothHard,
                remotePoseSmoothSpeed);
        remotePoseSmoothed = true;
        remotePoseSoftCorrected = !remotePoseSmoothHard;
        remotePoseDashCorrected = remoteBurstSmoothingActive;
        remotePoseIdleAnchored = !authorityMovementIntent;
        appliedPosition = currentPosition + delta * (remotePoseSmoothStep / deltaLen);
    }

    if (!keepLocalRotation)
    {
        remoteRotationAngle = ComputeRemoteEnemyRotationAngle(currentRotation, targetRotation);
        const bool mirrorAuthorityRotationDirectly =
            controlDecision.mode == CoopEnemyControlPolicy::EnemyControlMode::RemoteObserver;
        if (remoteInitialAuthoritySnap || mirrorAuthorityRotationDirectly)
        {
            remoteRotationSmoothed = remoteRotationAngle > 0.002f;
            remoteRotationSmoothAlpha = 1.0f;
            remoteRotationSmoothSpeed =
                remoteRotationAngle / std::max(kMimicStateTickSeconds, 0.001f);
            appliedRotation = targetRotation;
        }
        else
        {
            remoteRotationSmoothAlpha = ComputeRemoteEnemyRotationAlpha(
                remoteRotationAngle,
                remotePoseSmoothHard,
                remoteBurstSmoothingActive,
                remoteRotationSmoothHard,
                remoteRotationSmoothSpeed);
            remoteRotationSmoothed = remoteRotationAngle > 0.002f;
            if (remoteRotationSmoothed)
            {
                appliedRotation = Quat::CreateNlerp(currentRotation, targetRotation, remoteRotationSmoothAlpha);
                appliedRotation.Normalize();
            }
            else
            {
                appliedRotation = targetRotation;
            }
        }
    }

    ApplyEntityPhysicsVelocity(*entity, Vec3(ZERO));
    if (authorityGlooed)
    {
        CArkProjectileGooPhysicsManager::ZeroOutVelocities(entity->GetId());
        ArkNpc* npc = EntityUtils::GetArkNpc(entity);
        const bool needsGlooApply = npc && (!wasRemoteGlooed || !npc->m_bIsFrozenInGloo);
        if (needsGlooApply)
        {
            m_applyingRemoteEnemyGlooState = true;
            TryGuardedVoidCall(
                "remote enemy apply gloo locomotion state",
                [npc]()
                {
                    ApplyNpcGlooFrozenNative(*npc);
                });
            m_applyingRemoteEnemyGlooState = false;
        }
    }
    else if (wasRemoteGlooed)
    {
        if (ArkNpc* npc = EntityUtils::GetArkNpc(entity))
        {
            m_applyingRemoteEnemyGlooState = true;
            TryGuardedVoidCall(
                "remote enemy clear gloo locomotion state",
                [npc]()
                {
                    ClearNpcGlooFrozenNative(*npc);
                });
            m_applyingRemoteEnemyGlooState = false;
        }
    }

    if (ArkNpc* npc = EntityUtils::GetArkNpc(entity))
    {
        bool localMindControlled = false;
        bool localPsiSuppressed = false;
        bool localHacked = false;
        bool localCorrupted = false;
        TryGuardedCall("remote enemy read mind controlled", [npc]() { return npc->IsMindControlled(); }, localMindControlled);
        TryGuardedCall("remote enemy read psi suppressed", [npc]() { return npc->IsPsiSuppressed(); }, localPsiSuppressed);
        TryGuardedCall("remote enemy read hacked", [npc]() { return npc->IsHacked(); }, localHacked);
        TryGuardedCall("remote enemy read corrupted", [npc]() { return npc->IsCorrupted(); }, localCorrupted);

        if (authorityMindControlled != localMindControlled)
        {
            m_applyingRemoteEnemyPersistentStatus = true;
            TryGuardedVoidCall(
                authorityMindControlled
                    ? "remote enemy apply mind controlled"
                    : "remote enemy clear mind controlled",
                [npc, authorityMindControlled]()
                {
                    if (authorityMindControlled)
                        npc->PerformMindControlled();
                    else
                        npc->PerformStopMindControlled();
                });
            m_applyingRemoteEnemyPersistentStatus = false;
        }
        if (authorityPsiSuppressed != localPsiSuppressed)
        {
            m_applyingRemoteEnemyPersistentStatus = true;
            TryGuardedVoidCall(
                authorityPsiSuppressed
                    ? "remote enemy apply psi suppressed"
                    : "remote enemy clear psi suppressed",
                [npc, authorityPsiSuppressed]()
                {
                    if (authorityPsiSuppressed)
                        npc->StartPsiSuppressed();
                    else
                        npc->EndPsiSuppressed();
                });
            m_applyingRemoteEnemyPersistentStatus = false;
        }
        if (authorityCorrupted != localCorrupted)
        {
            m_applyingRemoteEnemyPersistentStatus = true;
            TryGuardedVoidCall(
                authorityCorrupted
                    ? "remote enemy apply corrupted"
                    : "remote enemy clear corrupted",
                [npc, authorityCorrupted]()
                {
                    if (authorityCorrupted)
                        npc->Corrupt(false, INVALID_ENTITYID);
                    else
                        npc->UnCorrupt(false);
                });
            m_applyingRemoteEnemyPersistentStatus = false;
        }
        if (authorityHacked && !localHacked)
        {
            m_applyingRemoteEnemyPersistentStatus = true;
            TryGuardedVoidCall("remote enemy apply hacked", [npc]() { npc->Hack(); });
            m_applyingRemoteEnemyPersistentStatus = false;
        }
    }

    if (authorityFactionValid && gEnv && gEnv->pGame)
    {
        if (IArkFactionManager* factionInterface = gEnv->pGame->GetIArkFactionManager())
        {
            ArkFactionManager* factionManager = static_cast<ArkFactionManager*>(factionInterface);
            const unsigned localFaction = factionInterface->GetEntityFaction(entity->GetId());
            if (localFaction != authorityFaction && factionManager->IsValidFaction(authorityFaction))
            {
                TryGuardedVoidCall(
                    "remote enemy apply authority faction",
                    [factionManager, entity, authorityFaction]()
                    {
                        factionManager->SetEntityFaction(entity->GetId(), authorityFaction);
                    });
            }
        }
    }

    EnemyAuthorityState& state = m_enemyAuthorities[enemyNetId];
    state.entityId = entity->GetId();
    state.netId = enemyNetId;
    state.archetypeId = archetypeId;
    if (authorityRagdolled != state.remoteAuthorityRagdollApplied)
    {
        ArkNpc* npc = EntityUtils::GetArkNpc(entity);
        bool nativeResult = false;
        const bool invoked = npc && TryGuardedCall(
            authorityRagdolled
                ? "remote enemy apply authority PushIndefiniteRagdoll"
                : "remote enemy clear authority PopIndefiniteRagdoll",
            [npc, authorityRagdolled]()
            {
                return authorityRagdolled
                    ? npc->PushIndefiniteRagdoll()
                    : npc->PopIndefiniteRagdoll();
            },
            nativeResult,
            nullptr);
        if (invoked && nativeResult)
        {
            state.remoteAuthorityRagdollApplied = authorityRagdolled;
            if (authorityRagdolled)
                ++m_remoteEnemyAuthorityRagdollApplies;
            else
                ++m_remoteEnemyAuthorityRagdollClears;
            m_lastRemoteEnemyRagdollEvent =
                std::string(authorityRagdolled
                    ? "applied_authority_enemy_ragdoll"
                    : "cleared_authority_enemy_ragdoll") +
                " net=" + std::to_string(enemyNetId) +
                " entity=" + std::to_string(entity->GetId());
        }
        else
        {
            ++m_remoteEnemyAuthorityRagdollFailures;
            m_lastRemoteEnemyRagdollEvent =
                std::string(authorityRagdolled
                    ? "failed_authority_enemy_ragdoll_apply"
                    : "failed_authority_enemy_ragdoll_clear") +
                " net=" + std::to_string(enemyNetId) +
                " entity=" + std::to_string(entity->GetId());
        }
        AppendEnemySyncTrace("ragdoll_sync", m_lastRemoteEnemyRagdollEvent);
    }
    state.lastPosition = currentPosition;
    state.lastRotation = currentRotation;
    const float positionSampleSeconds = EnemyAnimationNowSeconds();
    const bool resetPositionSamples =
        remoteInitialAuthoritySnap ||
        remoteExplicitBurstCorrected ||
        remoteExplicitBurstSnap ||
        authorityGlooed != wasRemoteGlooed ||
        !wasRemoteLocomotionAuthority;
    if (positionSampleSeconds >= 0.0f)
    {
        if (resetPositionSamples || state.remotePositionSampleCount == 0)
        {
            state.remotePositionSamples = {};
            state.remotePositionSamples[0].position = packetTargetPosition;
            state.remotePositionSamples[0].receivedAtSeconds = positionSampleSeconds;
            state.remotePositionSampleCount = 1;
            state.remotePositionSampleIntervalSeconds = kEnemyStateActiveChangeIntervalSeconds;
            state.remoteInterpolationTargetPosition = packetTargetPosition;
            state.remoteInterpolationActive = false;
        }
        else
        {
            const size_t sampleCount = std::min<size_t>(
                state.remotePositionSampleCount,
                state.remotePositionSamples.size());
            EnemyAuthorityState::RemotePositionSample& latest =
                state.remotePositionSamples[sampleCount - 1];
            const float arrivalInterval =
                positionSampleSeconds - latest.receivedAtSeconds;
            if (arrivalInterval >= kEnemyRemoteInterpolationMinSampleSeconds)
            {
                if (arrivalInterval <= kEnemyRemoteInterpolationMaxSampleSeconds)
                {
                    state.remotePositionSampleIntervalSeconds =
                        state.remotePositionSampleIntervalSeconds * 0.75f +
                        arrivalInterval * 0.25f;
                }

                size_t insertIndex = sampleCount;
                if (sampleCount == state.remotePositionSamples.size())
                {
                    std::move(
                        state.remotePositionSamples.begin() + 1,
                        state.remotePositionSamples.end(),
                        state.remotePositionSamples.begin());
                    insertIndex = state.remotePositionSamples.size() - 1;
                }
                else
                {
                    state.remotePositionSampleCount =
                        static_cast<uint8_t>(sampleCount + 1);
                }
                state.remotePositionSamples[insertIndex].position = packetTargetPosition;
                state.remotePositionSamples[insertIndex].receivedAtSeconds = positionSampleSeconds;
            }
            else
            {
                // Coalesce packets drained in the same render frame. Replaying
                // each one would reintroduce a packet-sized staircase.
                latest.position = packetTargetPosition;
            }
        }
    }
    state.remoteTargetPosition = targetPosition;
    state.remoteRawTargetPosition = packetTargetPosition;
    state.remoteTargetRotation = targetRotation;
    state.remoteMoveDirection = effectiveRemoteMoveDirection;
    state.remoteSpeed = effectiveRemoteSpeed;
    state.remoteAuthorityPacketLocomotionFlags = packet.locomotionFlags;
    state.remoteLocomotionFlags = effectiveLocomotionFlags;
    state.remoteLocomotionLevel = appliedLane;
    state.remoteMovementHoldSeconds = motionDecision.motionSeconds;
    state.remoteTargetMotionSeconds = motionDecision.motionSeconds;
    state.remoteBurstTransformSeconds = nextRemoteBurstTransformSeconds;
    state.remoteTargetMotionFilteredSpeed = motionDecision.filteredTargetSpeed;
    state.remoteTargetMotionFlags = motionDecision.targetMotionFlags;
    state.remoteActionMotionBlockSeconds = remoteActionMotionBlockSeconds;
    state.remoteTransformNeedsAuthoritySnap = false;
    if (targetMotionInferenceBlockedByAction)
    {
        state.remoteTargetMotionSeconds = 0.0f;
        state.remoteTargetMotionFlags = 0;
        state.remoteVisualMotionSeconds = 0.0f;
        state.remoteVisualMotionFlags = 0;
    }
    state.localFocusCombatSeconds = std::max(0.0f, state.localFocusCombatSeconds - kMimicStateTickSeconds);
    const bool remotePhantomDash =
        incomingBurstFlags != 0 &&
        ResolveNpcMannequinKindForRuntime(entity) == "phantom";
    bool remoteBurstFxTriggered = false;
    if (remoteExplicitBurstEvent &&
        remotePhantomDash &&
        !authorityGlooed)
    {
        // The authority pose stream is the source of truth here. A mixed-focus
        // observer may be running a local attack action while the authority
        // performs Shift, so requiring the current Mannequin fragment to also
        // be Shift would lose the already-confirmed displacement event.
        remoteBurstFxTriggered = TriggerRemoteEnemyBurstVisualEffect(
            state,
            *entity,
            enemyNetId,
            effectiveMannequinFragmentId,
            effectiveMannequinSequence,
            currentPosition,
            targetPosition,
            effectiveRemoteMoveDirection,
            "remote authority phantom dash");
    }
    if (remoteExplicitBurstCorrected || remoteExplicitBurstSnap)
    {
        state.remoteBurstSnapSequence = effectiveMannequinSequence;
        state.remoteBurstSnapFragmentId = effectiveMannequinFragmentId;
    }
    else if (!remoteBurstMovement)
    {
        state.remoteBurstSnapSequence = 0;
        state.remoteBurstSnapFragmentId = -1;
    }
    bool remotePacketSmoothApplied = false;
    std::string remotePacketSmoothApplyReason;
    const bool remotePacketContinuousMotionApply =
        !remoteBurstSmoothingActive &&
        controlDecision.blockMovement &&
        !authorityGlooed &&
        (remotePoseSmoothed || remotePoseSmoothTiny) &&
        deltaLen >= kEnemyRemotePacketMotionApplyMinDistance &&
        (
            authorityMovementFlagIntent ||
            motionDecision.confirmedTargetMotion ||
            motionDecision.inferredMovementFlags != 0 ||
            (
                (motionDecision.targetMotionFlags & continuousMovementFlags) != 0 &&
                packetTargetStepLen >= kEnemyRemoteTargetMotionInferenceMinStep));
    const bool explicitPacketTransformApply =
        CoopRuntimeConfig::UnsafeFlag("COOP_ENABLE_REMOTE_ENEMY_PACKET_TRANSFORM_APPLY");
    if (explicitPacketTransformApply)
        remotePacketSmoothApplyReason = "env";
    else if (remoteExplicitBurstCorrected || remoteExplicitBurstSnap)
        remotePacketSmoothApplyReason = "dashEvent";
    if (remoteInitialAuthoritySnap)
        remotePacketSmoothApplyReason = "initialBind";
    const bool allowPacketPositionApply = !remotePacketSmoothApplyReason.empty();
    const bool allowPacketRotationApply = allowPacketPositionApply;
    // Ordinary snapshots only replace the destination. Applying a second,
    // packet-sized correction here made a nominally smooth 60 Hz follower take
    // a visible step every 100 ms. The frame tick below now owns the complete
    // continuous path and its distance-proportional catch-up speed. Initial
    // binding and authored Burst/Shift events remain explicit endpoint events.
    const bool remotePacketSmoothDeferredToTick =
        !explicitPacketTransformApply &&
        remotePacketSmoothApplyReason.empty() &&
        (remotePoseSmoothed || remotePoseSmoothTiny || remoteRotationSmoothed) &&
        (remotePacketContinuousMotionApply ||
            remoteBurstSmoothingActive ||
            remotePoseSmoothHard ||
            authorityCatchupCorrection ||
            remoteIdleCorrection ||
            deltaLen >= kEnemyRemotePacketSmoothApplyMinDistance);
    if (remotePacketSmoothDeferredToTick)
        ++m_enemyPacketTransformDeferrals;
    const bool shouldApplyPacketPosition =
        (
            remoteInitialAuthoritySnap ||
            (
                allowPacketPositionApply &&
                !authorityGlooed &&
                (remotePoseSmoothed || remotePoseSmoothTiny))) &&
        (appliedPosition - currentPosition).GetLengthSquared() > 0.00000025f;
    const bool shouldApplyPacketRotation =
        (allowPacketRotationApply || remoteInitialAuthoritySnap) &&
        (!authorityGlooed || remoteInitialAuthoritySnap) &&
        !keepLocalRotation &&
        remoteRotationSmoothed;
    const float visibleAnimationStep =
        shouldApplyPacketPosition
            ? (appliedPosition - currentPosition).GetLength()
            : 0.0f;
    const float visibleAnimationSpeed =
        visibleAnimationStep / std::max(kMimicStateTickSeconds, 0.001f);
    const bool visualContinuousMotionBlockedByAction = continuousMotionBlockedByAction;
    const bool authorityTargetVisualMotionEvidence =
        !remoteBurstSmoothingActive &&
        !authorityGlooed &&
        !visualContinuousMotionBlockedByAction &&
        (
            authorityMovementAnimationIntent ||
            (motionDecision.actionTargetMotionFlags & continuousMovementFlags) != 0) &&
        (
            motionDecision.confirmedTargetMotion ||
            motionDecision.heldPreviousMovement ||
            motionDecision.localTargetActionMotionEvidence ||
            (motionDecision.targetMotionFlags & continuousMovementFlags) != 0 ||
            state.remoteTargetMotionSeconds > 0.0f) &&
        packetTargetStepLen >= kEnemyRemoteVisualTargetStepMin &&
        packetTargetStepLen <= kEnemyRemoteVisualTargetStepMax;
    const bool authorityActionVisualMotionEvidence =
        !remoteBurstSmoothingActive &&
        !authorityGlooed &&
        !visualContinuousMotionBlockedByAction &&
        !authorityMovementAnimationIntent &&
        (motionDecision.actionTargetMotionFlags & continuousMovementFlags) != 0 &&
        motionDecision.localTargetActionMotionEvidence &&
        packetTargetStepLen >= kEnemyRemoteVisualTargetStepMin &&
        packetTargetStepLen <= kEnemyRemoteVisualTargetStepMax;
    const float visualMotionEvidenceStep =
        std::max(
            visibleAnimationStep,
            authorityTargetVisualMotionEvidence ? packetTargetStepLen : 0.0f);
    const float visualMotionEvidenceSpeed =
        std::max(
            visibleAnimationSpeed,
            authorityTargetVisualMotionEvidence
                ? std::clamp(
                    packetTargetStepLen / std::max(kMimicStateTickSeconds, 0.001f),
                    0.0f,
                    kEnemyRemoteReceiverMaxRunSpeed)
                : 0.0f);
    if (visualContinuousMotionBlockedByAction &&
        (state.remoteVisualMotionFlags & continuousMovementFlags) != 0)
    {
        state.remoteVisualMotionFlags &= ~continuousMovementFlags;
        if ((state.remoteVisualMotionFlags & CoopEnemyControlPolicy::BurstMovementFlags()) == 0)
            state.remoteVisualMotionSeconds = 0.0f;
    }
    const uint32_t visualTransformLocomotionFlags =
        visualContinuousMotionBlockedByAction
            ? (transformLocomotionFlags & ~continuousMovementFlags)
            : transformLocomotionFlags;
    const uint32_t visualAnimationSourceFlags =
        (shouldApplyPacketPosition || authorityTargetVisualMotionEvidence)
        ? (visualTransformLocomotionFlags |
            motionDecision.targetMotionFlags |
            (authorityActionVisualMotionEvidence
                ? (motionDecision.actionTargetMotionFlags & continuousMovementFlags)
                : 0u))
        : 0u;
    const uint32_t visualAnimationMotionFlags =
        UpdateRemoteEnemyVisualMotionFromStep(
            state,
            visualMotionEvidenceStep,
            visualMotionEvidenceSpeed,
            visualAnimationSourceFlags,
            kMimicStateTickSeconds,
            true);
    uint32_t actionMixerMotionFlags = visualAnimationMotionFlags;
    const uint32_t authoritySemanticContinuousFlags =
        (
            authorityMovementAnimationIntent
                ? (effectiveLocomotionFlags | motionDecision.targetMotionFlags)
                : 0u) |
        (authorityActionVisualMotionEvidence
            ? (motionDecision.actionTargetMotionFlags & continuousMovementFlags)
            : 0u);
    const uint32_t authoritySemanticContinuousFlagsMasked =
        authoritySemanticContinuousFlags &
        continuousMovementFlags;
    const uint32_t authoritySemanticBurstFlags =
        effectiveLocomotionFlags & CoopEnemyControlPolicy::BurstMovementFlags();
    const uint32_t authoritySemanticMotionFlags =
        authoritySemanticContinuousFlagsMasked | authoritySemanticBurstFlags;
    const bool preserveAuthorityContinuousMotionForAction =
        (actionMixerMotionFlags & continuousMovementFlags) == 0 &&
        (authoritySemanticMotionFlags & continuousMovementFlags) != 0 &&
        authorityTargetVisualMotionEvidence &&
        (
            motionDecision.confirmedTargetMotion ||
            motionDecision.heldPreviousMovement ||
            motionDecision.localTargetActionMotionEvidence ||
            effectiveAuthorityFragmentCarriesMovement);
    if (preserveAuthorityContinuousMotionForAction)
        actionMixerMotionFlags |= authoritySemanticMotionFlags & continuousMovementFlags;
    const bool actionMixerMotionSuppressedByVisual =
        (visualAnimationMotionFlags & continuousMovementFlags) == 0 &&
        (authoritySemanticMotionFlags & continuousMovementFlags) != 0 &&
        !preserveAuthorityContinuousMotionForAction;
    const uint32_t authorityBurstMotionFlags =
        authoritySemanticMotionFlags & CoopEnemyControlPolicy::BurstMovementFlags();
    const bool carryAuthorityBurstMotionForAction =
        authorityBurstMotionFlags != 0 &&
        (
            motionDecision.explicitBurstEvent ||
            (visualAnimationMotionFlags & CoopEnemyControlPolicy::BurstMovementFlags()) != 0 ||
            visibleAnimationStep >= kEnemyRemoteVisualMoveStepThreshold);
    if (carryAuthorityBurstMotionForAction)
        actionMixerMotionFlags |= authorityBurstMotionFlags;
    const bool actionMixerBurstSuppressedByVisual =
        authorityBurstMotionFlags != 0 &&
        !carryAuthorityBurstMotionForAction;
    const bool remoteVisualBurstSuppressed =
        remoteBurstMovement &&
        visualAnimationMotionFlags == 0 &&
        visibleAnimationStep < kEnemyRemoteVisualMoveStepThreshold;
    const uint32_t visualAnimationMotionMask =
        continuousMovementFlags |
        CoopEnemyControlPolicy::BurstMovementFlags() |
        CoopProtocol::kEnemyLocomotionFlagTurning;
    const uint32_t hardActionIdentityFlags =
        (effectivePacket.locomotionFlags &
            (CoopEnemyControlPolicy::HardOverrideFlags() & ~CoopEnemyControlPolicy::BurstMovementFlags())) |
        (carryAuthorityBurstMotionForAction
            ? (effectivePacket.locomotionFlags & CoopEnemyControlPolicy::BurstMovementFlags())
            : 0u);
    effectivePacket.locomotionFlags =
        (effectivePacket.locomotionFlags & ~visualAnimationMotionMask) |
        actionMixerMotionFlags |
        hardActionIdentityFlags;
    if (shouldApplyPacketPosition || shouldApplyPacketRotation)
    {
        struct ScopedRemoteEnemyPacketTransformWrite
        {
            explicit ScopedRemoteEnemyPacketTransformWrite(uint32_t& depth)
                : m_depth(depth)
            {
                ++m_depth;
            }

            ~ScopedRemoteEnemyPacketTransformWrite()
            {
                --m_depth;
            }

            uint32_t& m_depth;
        } transformWrite(m_remoteEnemyTransformWriteDepth);

        if (shouldApplyPacketPosition && shouldApplyPacketRotation)
            entity->SetPosRotScale(appliedPosition, appliedRotation, entity->GetScale(), 0);
        else if (shouldApplyPacketPosition)
            ApplyEnemyPositionOnly(*entity, appliedPosition);
        else
            entity->SetRotation(appliedRotation, 0);

        state.lastPosition = shouldApplyPacketPosition ? appliedPosition : currentPosition;
        state.lastRotation = shouldApplyPacketRotation ? appliedRotation : currentRotation;
        if (gEnv && gEnv->pTimer)
        {
            const float nowSeconds = gEnv->pTimer->GetAsyncCurTime();
            if (shouldApplyPacketPosition)
                state.remoteTransformRewriteLastSeconds = nowSeconds;
            if (shouldApplyPacketRotation)
                state.remoteRotationRewriteLastSeconds = nowSeconds;
        }
        ApplyEntityPhysicsVelocity(*entity, Vec3(ZERO));
        remotePacketSmoothApplied = true;
    }
    if (packet.mannequinSequence != 0)
    {
        const bool changed =
            packet.mannequinSequence != state.remoteMannequinSequence ||
            packet.mannequinFragmentId != state.remoteMannequinFragmentId ||
            packet.mannequinOrdinal != state.remoteMannequinOrdinal ||
            packet.mannequinPriority != state.remoteMannequinPriority ||
            ((packet.mannequinReserved & CoopProtocol::kEnemyMannequinReservedNativeRandomOption) != 0) !=
                state.remoteMannequinRandomOption ||
            packet.mannequinTagStateValid != (state.remoteMannequinTagStateValid ? 1u : 0u) ||
            !std::equal(
                std::begin(packet.mannequinTagState),
                std::end(packet.mannequinTagState),
                state.remoteMannequinTagState.begin());
        state.remoteMannequinSequence = packet.mannequinSequence;
        state.remoteMannequinFragmentId = packet.mannequinFragmentId;
        state.remoteMannequinOrdinal = packet.mannequinOrdinal;
        state.remoteMannequinPriority = packet.mannequinPriority;
        std::copy(
            std::begin(packet.mannequinTagState),
            std::end(packet.mannequinTagState),
            state.remoteMannequinTagState.begin());
        state.remoteMannequinTagStateValid = packet.mannequinTagStateValid != 0;
        state.remoteMannequinRandomOption =
            (packet.mannequinReserved & CoopProtocol::kEnemyMannequinReservedNativeRandomOption) != 0;
        const uint32_t remoteMannequinStateMask =
            CoopProtocol::kEnemyLocomotionFlagWalking |
            CoopProtocol::kEnemyLocomotionFlagRunning |
            CoopProtocol::kEnemyLocomotionFlagDashing |
            CoopProtocol::kEnemyLocomotionFlagShifting |
            CoopProtocol::kEnemyLocomotionFlagMorphing |
            CoopProtocol::kEnemyLocomotionFlagLunging |
            CoopProtocol::kEnemyLocomotionFlagAttacking |
            CoopProtocol::kEnemyLocomotionFlagHitReacting |
            CoopProtocol::kEnemyLocomotionFlagStunned |
            CoopProtocol::kEnemyLocomotionFlagCowering |
            CoopProtocol::kEnemyLocomotionFlagGlooed |
            CoopProtocol::kEnemyLocomotionFlagTurning |
            CoopProtocol::kEnemyLocomotionFlagInCombat |
            CoopProtocol::kEnemyLocomotionFlagMannequinDriven;
        state.remoteMannequinFlags = remoteAnimationPacket.locomotionFlags & remoteMannequinStateMask;
        state.remoteMannequinCarryMovement = effectiveAuthorityFragmentCarriesMovement;
        const bool canHoldRemoteMannequin =
            CanHoldRemoteMannequinStateForPacket(packet.locomotionFlags, state.remoteMannequinFlags);
        const bool passiveRemoteMannequin =
            CoopEnemyControlPolicy::IsPassiveMannequinFlags(state.remoteMannequinFlags);
        state.remoteMannequinStateSeconds = canHoldRemoteMannequin
            ? (passiveRemoteMannequin
                ? kEnemyRemotePassiveMannequinStateSeconds
                : kEnemyRemoteMannequinStateSeconds)
            : 0.0f;
        state.remoteMannequinAttackKind = packet.attackKind;
        if (authorityStationaryMannequinBlocksMotionInference)
        {
            state.remoteActionMotionBlockSeconds = std::max(
                state.remoteActionMotionBlockSeconds,
                kEnemyRemoteActionMotionInferenceBlockSeconds);
        }
        if (changed)
        {
            m_lastEnemyMannequinStateEvent =
                "remote_mannequin net=" + std::to_string(enemyNetId) +
                " entity=" + std::to_string(entity->GetId()) +
                " fragment=" + std::to_string(packet.mannequinFragmentId) +
                " ordinal=" + std::to_string(packet.mannequinOrdinal) +
                " priority=" + std::to_string(packet.mannequinPriority) +
                " randomOption=" + std::to_string(state.remoteMannequinRandomOption ? 1 : 0) +
                " tagState=" + std::to_string(packet.mannequinTagStateValid) +
                " seq=" + std::to_string(packet.mannequinSequence) +
                " flags=" + std::to_string(state.remoteMannequinFlags) +
                (inferredAuthorityMovementFlags != 0
                    ? " inferredTargetMotion=" + std::to_string(inferredAuthorityMovementFlags)
                    : std::string()) +
                " attack=" + std::to_string(packet.attackKind);
        }
    }
    else
    {
        if (state.remoteMannequinStateSeconds > 0.0f)
        {
            state.remoteMannequinStateSeconds = std::max(
                0.0f,
                state.remoteMannequinStateSeconds - kMimicStateTickSeconds);
        }
        if (state.remoteMannequinStateSeconds <= 0.0f)
        {
            // Non-holdable transient fragments start with a zero TTL. Clear
            // them on the first authority packet without an action as well;
            // otherwise their last fragment/flags remain latched forever.
            state.remoteMannequinSequence = 0;
            state.remoteMannequinFragmentId = -1;
            state.remoteMannequinOrdinal = CoopProtocol::kInvalidMannequinOrdinal;
            state.remoteMannequinFlags = 0;
            state.remoteMannequinAttackKind = 0;
            state.remoteMannequinPriority = 0;
            state.remoteMannequinTagState.fill(0);
            state.remoteMannequinTagStateValid = false;
            state.remoteMannequinRandomOption = false;
            state.remoteMannequinCarryMovement = false;
            state.remoteNativeMirrorDiagnosedSequence = 0;
        }
    }
    state.remoteLocomotionAuthority = true;
    state.remoteAuthoritySilentSeconds = 0.0f;
    state.remoteAuthorityBlocked = remoteAuthorityBlocked;
    state.authorityAttentionLevel = packet.authorityAttentionLevel;
    state.remoteAuthorityHasAttention =
        remoteAuthorityHasAttention &&
        packet.authorityAttentionLevel > CoopEnemyAuthorityPolicy::kUnknownAttention;
    state.remoteTargetAccountToken = packet.targetAccountToken;
    const bool authorityLocomotionSemantic =
        (packet.semanticReserved[0] & CoopProtocol::kEnemySemanticReservedAuthorityLocomotion) != 0 &&
        packet.semanticContextId != 0 &&
        packet.semanticSequence != 0 &&
        CoopEnemyIntentGate::IsAuthorityLocomotionAbilityContext(packet.semanticContextId);
    const bool authorityPresentationSemantic =
        (packet.semanticReserved[0] & CoopProtocol::kEnemySemanticReservedAuthorityPresentation) != 0 &&
        packet.semanticContextId != 0 &&
        packet.semanticSequence != 0;
    const bool hasAuthoritySemantic =
        packet.semanticContextId != 0 && packet.semanticSequence != 0;
    const bool staleAuthoritySemantic =
        hasAuthoritySemantic &&
        state.remoteAuthoritySemanticSequence != 0 &&
        packet.semanticSequence != state.remoteAuthoritySemanticSequence &&
        CoopSerialSequence::IsStaleOrDuplicate(
            packet.semanticSequence,
            state.remoteAuthoritySemanticSequence);
    if (staleAuthoritySemantic)
    {
        AppendEnemySyncTrace(
            "semantic_observe",
            "remote_semantic_stale_ignored net=" + std::to_string(enemyNetId) +
                " entity=" + std::to_string(entity->GetId()) +
                " context=" + std::to_string(packet.semanticContextId) +
                " seq=" + std::to_string(packet.semanticSequence) +
                " frontier=" + std::to_string(state.remoteAuthoritySemanticSequence));
    }
    else if (authorityLocomotionSemantic)
    {
        const bool newerLocomotionSemantic =
            state.remoteAuthoritySemanticSequence == 0 ||
            !CoopSerialSequence::IsStaleOrDuplicate(
                packet.semanticSequence,
                state.remoteAuthoritySemanticSequence);
        if (newerLocomotionSemantic)
        {
            CoopSerialSequence::Observe(
                packet.semanticSequence,
                state.remoteAuthoritySemanticSequence);
            state.remoteLocomotionSemanticContextId = packet.semanticContextId;
            state.remoteLocomotionSemanticSequence = packet.semanticSequence;
            state.remoteLocomotionSemanticVariant = packet.semanticVariant;
            state.remoteLocomotionSemanticAppliedContextId = packet.semanticContextId;
            state.remoteLocomotionSemanticAppliedSequence = packet.semanticSequence;
            state.remoteLocomotionSemanticAppliedVariant = packet.semanticVariant;
            AppendEnemySyncTrace(
                "semantic_locomotion",
                "remote_locomotion_semantic_applied net=" + std::to_string(enemyNetId) +
                    " entity=" + std::to_string(entity->GetId()) +
                    " context=" + std::to_string(packet.semanticContextId) +
                    " variant=" + std::to_string(packet.semanticVariant) +
                    " seq=" + std::to_string(packet.semanticSequence) +
                    " route=presentation_only");
        }
        state.remoteSemanticContextId = 0;
        state.remoteSemanticSequence = 0;
        state.remoteSemanticVariant = 0;
        state.remotePresentationSemanticContextId = 0;
        state.remotePresentationSemanticSequence = 0;
        state.remotePresentationSemanticVariant = 0;
        state.remotePresentationSemanticNativeOutcome =
            CoopProtocol::kEnemySemanticNativeOutcomeAbilityManager;
    }
    else if (authorityPresentationSemantic)
    {
        state.remoteLocomotionSemanticContextId = 0;
        state.remoteLocomotionSemanticSequence = 0;
        state.remoteLocomotionSemanticVariant = 0;
        state.remotePresentationSemanticContextId = packet.semanticContextId;
        state.remotePresentationSemanticSequence = packet.semanticSequence;
        state.remotePresentationSemanticVariant = packet.semanticVariant;
        state.remotePresentationSemanticNativeOutcome = packet.semanticReserved[1];
        state.remoteSemanticContextId = packet.semanticContextId;
        state.remoteSemanticSequence = packet.semanticSequence;
        state.remoteSemanticVariant = packet.semanticVariant;
    }
    else
    {
        state.remoteLocomotionSemanticContextId = 0;
        state.remoteLocomotionSemanticSequence = 0;
        state.remoteLocomotionSemanticVariant = 0;
        state.remotePresentationSemanticContextId = 0;
        state.remotePresentationSemanticSequence = 0;
        state.remotePresentationSemanticVariant = 0;
        state.remotePresentationSemanticNativeOutcome =
            CoopProtocol::kEnemySemanticNativeOutcomeAbilityManager;
        state.remoteSemanticContextId = packet.semanticContextId;
        state.remoteSemanticSequence = packet.semanticSequence;
        state.remoteSemanticVariant = packet.semanticVariant;
    }
    state.authorityOwnerAccountToken = packet.authorityOwnerAccountToken;
    state.authorityEpoch = packet.authorityEpoch;
    SyncRemoteEnemyPresentationTarget(
        state,
        *entity,
        packet.targetAccountToken,
        packet.mannequinSequence,
        "remote authority state");
    if (!remoteAuthorityBlocked)
        state.remoteAuthorityBlockedSeconds = 0.0f;
    if (controlDecision.blockMovement)
    {
        if (ArkNpc* npc = EntityUtils::GetArkNpc(entity))
        {
            ArkNpcMovementDesireManager* manager = &npc->m_movementDesireManager;
            bool hasNativeMovementRequest = false;
            TryGuardedCall(
                "remote enemy inspect local movement request",
                [manager]() { return manager->HasMovementRequest(); },
                hasNativeMovementRequest);
            const bool hasActiveMovementDesire =
                ReadActiveMovementDesireFromManager(manager) != nullptr;
            if (hasActiveMovementDesire || hasNativeMovementRequest)
            {
                // A desire may have been installed during the brief
                // attention/lease transition before the request hook knew the
                // entity was remote-driven. HasMovementRequest also catches a
                // request whose desire-list node has already been retired but
                // whose native movement-controller request is still alive.
                // Leaving either form active keeps a local walk body action
                // alive while the authority is standing still.
                TryGuardedVoidCall(
                    "remote enemy cancel active local movement path",
                    [manager]()
                    {
                        manager->StopMovement();
                        manager->CancelMovement();
                    });
                AppendEnemySyncTrace(
                    "locomotion",
                    "cancelled active local movement path net=" + std::to_string(enemyNetId) +
                        " entity=" + std::to_string(entity->GetId()) +
                        " mode=" + CoopEnemyControlPolicy::ModeName(controlDecision.mode));
            }
        }
    }
    if (state.remoteLocomotionAuthority && !wasRemoteLocomotionAuthority)
    {
        if (ArkNpc* npc = EntityUtils::GetArkNpc(entity))
        {
            // Retire only the old local path before creating the authority
            // presentation desire. Doing this after SyncRemoteEnemyMovementDesire
            // cancelled the newly-created mirror request on the first packet.
            TryGuardedVoidCall(
                "remote enemy cancel stale local movement on authority handoff",
                [npc]()
                {
                    npc->m_movementDesireManager.StopMovement();
                    npc->m_movementDesireManager.CancelMovement();
                });
        }
    }
    SyncRemoteEnemyMovementDesire(
        *entity,
        state,
        targetPosition,
        state.remoteMoveDirection,
        effectiveRemoteSpeed,
        effectiveLocomotionFlags,
        authorityMovementAnimationIntent);
    if (wasRemoteLocomotionAuthority && !authorityMovementIntent && wasAuthorityMoving)
    {
        if (ArkNpc* npc = EntityUtils::GetArkNpc(entity))
        {
            TryGuardedVoidCall(
                "remote enemy force movement stop on idle",
                [npc]()
                {
                    npc->m_movementDesireManager.StopMovement();
                    npc->m_movementDesireManager.CancelMovement();
                });
        }
    }

    ApplyRemoteEnemyMannequinAnimation(
        *entity,
        state,
        remoteAnimationPacket,
        controlDecision.localFocus,
        remoteAuthorityHasAttention,
        "client_remote_authority");

    if (localHasAttention)
        state.localRotationOverrideSeconds = std::max(
            state.localRotationOverrideSeconds,
            kEnemyLocalRotationOverrideGraceSeconds);
    state.hasLastPosition = true;
    m_enemyLocomotionLastFlags = effectiveLocomotionFlags;
    m_enemyLocomotionLastLevel = appliedLane;
    m_enemyLocomotionLastAttackKind = effectiveAttackKind;
    m_enemyLocomotionLastMannequinFragmentId = effectiveMannequinFragmentId;
    m_enemyLocomotionLastMannequinSequence = effectiveMannequinSequence;
    m_enemyLocomotionLastSpeed = effectiveRemoteSpeed;

    ++m_enemyLocomotionApplies;
    m_lastEnemyLocomotionEvent =
        "applied net=" + std::to_string(enemyNetId) +
        " entity=" + std::to_string(entity->GetId()) +
        " lane=" + std::to_string(appliedLane) +
        " mode=" + CoopEnemyControlPolicy::ModeName(controlDecision.mode) +
        " flags=" + std::to_string(effectiveLocomotionFlags) +
        " wireFlags=" + std::to_string(packet.locomotionFlags) +
        " policyFlags=" + std::to_string(rawEffectiveLocomotionFlags) +
        " sourceFlags=" + std::to_string(packet.sourceFlags) +
        " fragment=" + std::to_string(effectiveMannequinFragmentId) +
        " wireFragment=" + std::to_string(packet.mannequinFragmentId) +
        " mseq=" + std::to_string(effectiveMannequinSequence) +
        " wireMseq=" + std::to_string(packet.mannequinSequence) +
        " speed=" + std::to_string(effectiveRemoteSpeed) +
        (std::fabs(effectiveRemoteSpeed - packet.speed) > 0.001f
            ? " wireSpeed=" + std::to_string(packet.speed)
            : "") +
        (inferredAuthorityMovementFlags != 0
            ? " inferredTargetMotion=" + std::to_string(inferredAuthorityMovementFlags) +
                " targetSpeed=" + std::to_string(inferredAuthorityTargetSpeed)
            : std::string()) +
        (inferredAuthorityCatchupDrift ? " catchupDrift=1" : "") +
        (confirmedAuthorityTargetMotion ? " targetMotion=1" : "") +
        (remoteMovementHeldFromPrevious ? " moveHold=1" : "") +
        (strippedUnconfirmedActionMovement ? " strippedActionMove=1" : "") +
        (strippedHeldPassiveMovement ? " heldPassiveStrip=1" : "") +
        (heldPassiveMovementEvidence ? " heldPassiveMotion=1" : "") +
        (remoteActionBlocksMotionInferenceNow ? " actionMotionBlock=1" : "") +
        (remoteActionMotionBlockSeconds > 0.0f
            ? " actionBlockT=" + std::to_string(remoteActionMotionBlockSeconds)
            : std::string()) +
        (targetMotionInferenceBlockedByAction ? " inferenceBlockedAction=1" : "") +
        (blockedPreviousMovementHold ? " blockedMoveHold=1" : "") +
        (blockedPassiveDriftOnlyMotion ? " passiveDriftBlocked=1" : "") +
        (passiveActionMovementHoldEvidence ? " passiveActionMoveHold=1" : "") +
        (passiveNonCarryActionMotionBlocked ? " passiveNonCarryMoveBlocked=1" : "") +
        (authorityStationaryMannequinBlocksMotionInference ? " setActionMotionBlock=1" : "") +
        (localTargetActionMotionEvidence ? " localTargetActionMotion=1" : "") +
        (remoteActionTargetMotionEvidence ? " remoteActionMotion=1" : "") +
        (authorityActionVisualMotionEvidence
            ? " actionVisualMotion=" + std::to_string(motionDecision.actionTargetMotionFlags) +
                " actionVisualSpeed=" + std::to_string(motionDecision.actionTargetMotionSpeed)
            : std::string()) +
        (remotePassiveSmoothSpeedCarrier ? " passiveSmoothCarrier=1" : "") +
        (remotePassiveStepSmoothSpeedCarrier ? " passiveStepSmoothCarrier=1" : "") +
        (effectiveAuthorityFragmentCarriesMovement ? " authorityMoveCarrier=1" : "") +
        (rejectedReservedMovementCarryForFragment ? " rejectedCarryBit=1" : "") +
        (passiveAuthorityMovementCarry ? " passiveAuthorityCarry=1" : "") +
        (remoteTargetPredictionDistance > 0.0f
            ? " targetLead=" + std::to_string(remoteTargetPredictionDistance)
            : std::string()) +
        " targetStep=" + std::to_string(packetTargetStepLen) +
        " packetDelta=" + std::to_string(packetDeltaLen) +
        " filteredTargetSpeed=" + std::to_string(motionDecision.filteredTargetSpeed) +
        " motionHold=" + std::to_string(motionDecision.motionSeconds) +
        " visualStep=" + std::to_string(visibleAnimationStep) +
        " visualSpeed=" + std::to_string(visibleAnimationSpeed) +
        (authorityTargetVisualMotionEvidence
            ? " visualTargetStep=" + std::to_string(packetTargetStepLen) +
                " visualEvidenceStep=" + std::to_string(visualMotionEvidenceStep) +
                " visualEvidenceSpeed=" + std::to_string(visualMotionEvidenceSpeed)
            : std::string()) +
        (visualAnimationMotionFlags != 0
            ? " visualMove=" + std::to_string(visualAnimationMotionFlags)
            : std::string()) +
        (actionMixerMotionFlags != visualAnimationMotionFlags
            ? " actionMixMove=" + std::to_string(actionMixerMotionFlags)
            : std::string()) +
        (actionMixerMotionSuppressedByVisual ? " actionMixMoveSuppressed=1" : "") +
        (actionMixerBurstSuppressedByVisual ? " actionMixBurstSuppressed=1" : "") +
        (remoteVisualBurstSuppressed ? " visualBurstSuppressed=1" : "") +
        (visualContinuousMotionBlockedByAction ? " visualMoveBlockedByAction=1" : "") +
        (state.remoteVisualMotionSeconds > 0.0f
            ? " visualHold=" + std::to_string(state.remoteVisualMotionSeconds)
            : std::string()) +
        " driftSq=" + std::to_string(deltaSq) +
        (remoteMoveIntegrator ? " moveIntegrator=1" : "") +
        (authorityMovementIntent && !motionDecision.currentMovementAnimationEvidence
            ? " moveAnimEvidenceBlocked=1"
            : "") +
        (motionDecision.movementHoldSuppressedByMissingAnimationEvidence ? " animHoldSuppressed=1" : "") +
        (authorityMovementIntent && !authorityActionAllowsMovementAnimation
            ? " actionMoveAnimBlockedByFragment=1"
            : "") +
        (motionDecision.activeActionBlockedMovementAnimation ? " actionMoveAnimBlockedByActiveAction=1" : "") +
        (!authorityMovementAnimationIntent && authorityMovementIntent ? " catchupPoseOnly=1" : "") +
        (remoteBurstMovement && !authorityMovementIntent ? " burstPoseOnly=1" : "") +
        (authorityCatchupCorrection ? " catchupIntent=1" : "") +
        (remoteIdleCorrection ? " idleCorrection=1" : "") +
        (remotePoseHeldForNativeMove ? " poseHold=1" : "") +
        (remotePoseDashCorrected ? " poseDash=1" : "") +
        (remotePoseSoftCorrected ? " poseSoft=1" : "") +
        (remotePoseActionHeld ? " poseActionHold=1" : "") +
        (remotePoseHardCorrected ? " poseHard=1" : "") +
        (remotePoseIdleAnchored ? " poseIdleAnchor=1" : "") +
        (remotePoseSmoothed
            ? " smoothStep=" + std::to_string(remotePoseSmoothStep) +
                " smoothSpeed=" + std::to_string(remotePoseSmoothSpeed)
            : "") +
        (remoteInitialAuthoritySnap ? " initialSnap=1" : "") +
        (remoteExplicitBurstEvent ? " dashEvent=1" : "") +
        (remoteExplicitBurstSnap ? " dashSnap=1" : "") +
        (remoteBurstFxTriggered ? " burstFx=1" : "") +
        (remoteBurstMovement ? " burst=1" : "") +
        (strippedContinuousMovementDuringBurst ? " burstStripMove=1" : "") +
        (remoteBurstSmoothingActive ? " burstSmoothing=1" : "") +
        (remoteBurstSmoothingActive ? " burstInterpolationPaused=1" : "") +
        (remoteBurstMovement && !remoteBurstSmoothingActive ? " burstActionOnly=1" : "") +
        (nextRemoteBurstTransformSeconds > 0.0f
            ? " burstSmoothHold=" + std::to_string(nextRemoteBurstTransformSeconds)
            : std::string()) +
        (remotePoseSmoothHard ? " smoothHard=1" : "") +
        (remotePoseSmoothTiny ? " smoothTiny=1" : "") +
        (remoteRotationSmoothed
            ? " rotSmooth=" + std::to_string(remoteRotationSmoothAlpha) +
                " rotSpeed=" + std::to_string(remoteRotationSmoothSpeed)
            : "") +
        (remoteRotationSmoothHard ? " rotHard=1" : "") +
        (remotePacketSmoothApplied
            ? " packetSmoothApply=1 packetSmoothReason=" + remotePacketSmoothApplyReason
            : "") +
        (remotePacketSmoothDeferredToTick ? " packetSmoothDeferred=tick" : "") +
        (heldRemoteMannequinForPose ? " held=1" : "") +
        (authorityActionIntent ? " actionHold=1" : "") +
        (keepLocalRotation ? " localRot=1" : "") +
        (authorityGlooed ? " gloo=1" : "");
    if (remoteMoveIntegrator || remotePoseHeldForNativeMove || remotePoseSmoothed || deltaSq > 0.0025f)
        AppendEnemySyncTrace("pose", m_lastEnemyLocomotionEvent);
}

bool ModMain::ApplyEnemyDeathCommitToLocal(
    uint64_t enemyNetId,
    uint64_t archetypeId,
    const Vec3* position,
    const Quat* rotation,
    const CoopProtocol::TestMimicStatePacket* deathPacket,
    const char* reason)
{
    if (enemyNetId == 0 || archetypeId == 0 || !gEnv || !gEnv->pEntitySystem)
        return false;

    IEntity* entity = nullptr;
    EnemyAuthorityState* state = nullptr;
    const auto authorityIt = m_enemyAuthorities.find(enemyNetId);
    if (authorityIt != m_enemyAuthorities.end())
    {
        state = &authorityIt->second;
        if (state->entityId != INVALID_ENTITYID)
            entity = gEnv->pEntitySystem->GetEntity(state->entityId);
    }
    if (!entity)
    {
        const auto puppetIt = m_enemyPuppets.find(enemyNetId);
        if (puppetIt != m_enemyPuppets.end() && puppetIt->second.entityId != INVALID_ENTITYID)
            entity = gEnv->pEntitySystem->GetEntity(puppetIt->second.entityId);
    }
    if (!entity)
    {
        const auto bindingIt = std::find_if(
            m_enemyNetIdsByEntity.begin(),
            m_enemyNetIdsByEntity.end(),
            [enemyNetId](const auto& entry) { return entry.second == enemyNetId; });
        if (bindingIt != m_enemyNetIdsByEntity.end())
            entity = gEnv->pEntitySystem->GetEntity(bindingIt->first);
    }
    if (!entity)
    {
        m_lastEnemyLocomotionEvent =
            "deferred death commit missing exact body net=" + std::to_string(enemyNetId);
        return false;
    }

    const IEntityArchetype* localArchetype = entity->GetArchetype();
    if (!localArchetype || localArchetype->GetId() != archetypeId)
    {
        m_lastEnemyLocomotionEvent =
            "blocked death commit archetype mismatch net=" + std::to_string(enemyNetId);
        return false;
    }

    if (position && rotation)
    {
        struct ScopedDeathTransformWrite
        {
            explicit ScopedDeathTransformWrite(uint32_t& depth)
                : m_depth(depth)
            {
                ++m_depth;
            }

            ~ScopedDeathTransformWrite()
            {
                if (m_depth > 0)
                    --m_depth;
            }

            uint32_t& m_depth;
        } transformWrite(m_remoteEnemyTransformWriteDepth);
        entity->SetPosRotScale(*position, rotation->GetNormalized(), entity->GetScale(), 0);
    }
    ApplyEntityPhysicsVelocity(*entity, Vec3(ZERO));
    ClearRemoteEnemyMovementDesire(enemyNetId, "enemy death commit");

    ArkNpc* npc = EntityUtils::GetArkNpc(entity);
    bool alreadyDead = false;
    bool nativeDeathFinalized = false;
    if (npc)
    {
        TryGuardedCall("enemy death commit IsDead", [npc]() { return npc->IsDead(); }, alreadyDead, nullptr);
        nativeDeathFinalized = alreadyDead &&
            !npc->m_aiTreeEnabled.IsUnanimous() &&
            !npc->m_abilitiesEnabled.IsUnanimous() &&
            !npc->m_attentiveSubjectEnabled.IsUnanimous() &&
            !npc->m_attentionObjectEnabled.IsUnanimous();
    }

    const auto presentationIt = m_enemyDeathPresentations.find(enemyNetId);
    const CoopProtocol::EnemyDeathPresentationPacket* deathPresentation =
        presentationIt != m_enemyDeathPresentations.end()
            ? &presentationIt->second
            : nullptr;

    // A zero-health snapshot proves only the authority result. It cannot
    // manufacture the native corpse lifecycle. Wait for the reliable signal
    // package that caused the death instead of ever writing HP=0 or calling
    // OnKill directly on the observer.
    if (npc && !alreadyDead && !deathPresentation)
    {
        PendingEnemyDeathCommit& pending = m_pendingEnemyDeathCommits[enemyNetId];
        pending.archetypeId = archetypeId;
        if (position)
            pending.position = *position;
        if (rotation)
            pending.rotation = rotation->GetNormalized();
        if (deathPacket)
        {
            pending.statePacket = *deathPacket;
            pending.hasStatePacket = true;
        }
        m_lastEnemyLocomotionEvent =
            "staged death commit awaiting native signal net=" + std::to_string(enemyNetId);
        AppendEnemySyncTrace("death", m_lastEnemyLocomotionEvent);
        return true;
    }

    struct ScopedRemoteEnemyDeathApply
    {
        explicit ScopedRemoteEnemyDeathApply(bool& active)
            : m_active(active), m_previous(active)
        {
            m_active = true;
        }

        ~ScopedRemoteEnemyDeathApply()
        {
            m_active = m_previous;
        }

        bool& m_active;
        bool m_previous;
    } deathApply(m_applyingRemoteEnemyDeathCommit);

    // A mirrored locomotion/attack lease can otherwise outlive the reliable
    // death edge and resume its standing animation after Reaction_Death. Stop
    // those exact remote actions before asking Vanilla to own the corpse.
    if (state)
        ClearRemoteEnemyPresentationForLocalAuthority(*state, *entity, "enemy death commit");

    bool nativeHitAttempted = false;
    if (npc && !nativeDeathFinalized)
    {
        if (!deathPresentation)
        {
            m_lastEnemyLocomotionEvent =
                "blocked death commit without native signal net=" + std::to_string(enemyNetId);
            return false;
        }

        // Align only the still-living positive health baseline that existed
        // on authority immediately before the lethal package. The package is
        // then delivered through the same ArkSignalSystem path as Vanilla;
        // Vanilla alone decides HP, mimic breakup, reaction, ragdoll and loot.
        if (!alreadyDead && deathPresentation->targetHealthBeforeHit > 0.0f)
        {
            SetEntityHealthFromAuthority(
                entity->GetId(),
                deathPresentation->targetHealthBeforeHit,
                false,
                false);
        }
        nativeHitAttempted = TryApplyVanillaEnemyDeathHit(*npc, *entity, *deathPresentation);

        bool deadAfter = false;
        TryGuardedCall("enemy death signal IsDead", [npc]() { return npc->IsDead(); }, deadAfter, nullptr);
        nativeDeathFinalized = deadAfter &&
            !npc->m_aiTreeEnabled.IsUnanimous() &&
            !npc->m_abilitiesEnabled.IsUnanimous() &&
            !npc->m_attentiveSubjectEnabled.IsUnanimous() &&
            !npc->m_attentionObjectEnabled.IsUnanimous();
    }

    if (!nativeDeathFinalized)
    {
        m_lastEnemyLocomotionEvent =
            "native death signal did not finalize net=" + std::to_string(enemyNetId) +
            " attempted=" + std::to_string(nativeHitAttempted ? 1 : 0);
        AppendEnemySyncTrace("death", m_lastEnemyLocomotionEvent);
        return false;
    }

    if (deathPresentation)
    {
        const bool authorityHidden =
            (deathPresentation->flags &
                CoopProtocol::kEnemyDeathPresentationFlagAuthorityHidden) != 0;
        TryGuardedVoidCall(
            "enemy death presentation authoritative visibility",
            [entity, authorityHidden]() { entity->Hide(authorityHidden); },
            nullptr);
    }
    else if (deathPacket)
    {
        const bool authorityHidden =
            (deathPacket->flags & CoopProtocol::kTestMimicStateFlagHidden) != 0;
        TryGuardedVoidCall(
            "enemy death commit authoritative visibility",
            [entity, authorityHidden]() { entity->Hide(authorityHidden); },
            nullptr);
    }

    m_pendingEnemyDeathCommits.erase(enemyNetId);

    // OnKill may replace living physics with ragdoll physics. Reapply the
    // remote-mirror policy after that transition (and on every bounded death
    // repeat) so local gravity/collision cannot pull the replicated corpse
    // away from the authority's settled transform.
    ApplyProxyNoPropCollision(*entity, "remote enemy death mirror");
    ApplyEntityPhysicsVelocity(*entity, Vec3(ZERO));

    if (state)
    {
        if ((state->rosterFlags & CoopProtocol::kEnemyRosterFlagEthericDoppelganger) != 0)
        {
            for (auto& sourceEntry : m_enemyAuthorities)
            {
                EnemyAuthorityState& sourceState = sourceEntry.second;
                if (sourceState.stableEnemyId != state->sourceStableEnemyId)
                    continue;
                sourceState.activeEthericDoppelgangerStableEnemyId = 0;
                if (gEnv && gEnv->pEntitySystem &&
                    sourceState.entityId != INVALID_ENTITYID)
                {
                    if (IEntity* sourceEntity =
                            gEnv->pEntitySystem->GetEntity(sourceState.entityId))
                    {
                        if (ArkNpc* sourceNpc = EntityUtils::GetArkNpc(sourceEntity))
                        {
                            TryGuardedVoidCall(
                                "remote Etheric Doppelganger death relation",
                                [sourceNpc]()
                                {
                                    sourceNpc->SetEthericDoppelgangerId(INVALID_ENTITYID);
                                },
                                nullptr);
                        }
                    }
                }
                break;
            }
        }
        state->remoteLocomotionAuthority = false;
        state->remoteAuthorityHasAttention = false;
        state->authorityAttentionLevel = CoopEnemyAuthorityPolicy::kUnknownAttention;
        state->attentionCandidates.clear();
        state->remoteTargetAccountToken = 0;
        state->remoteAuthorityBlocked = false;
        state->remoteAuthorityPacketLocomotionFlags = 0;
        state->remoteLocomotionFlags = 0;
        state->remoteMannequinStateSeconds = 0.0f;
        state->sentDeadState = true;
        if (position && rotation)
        {
            state->lastPosition = *position;
            state->lastRotation = *rotation;
            state->hasLastPosition = true;
        }
    }
    if (auto puppetIt = m_enemyPuppets.find(enemyNetId); puppetIt != m_enemyPuppets.end())
        puppetIt->second.dead = true;

    m_lastEnemyLocomotionEvent =
        "applied death commit net=" + std::to_string(enemyNetId) +
        " entity=" + std::to_string(entity->GetId()) +
        " alreadyDead=" + std::to_string(alreadyDead ? 1 : 0) +
        " nativeHit=" + std::to_string(nativeHitAttempted ? 1 : 0) +
        " exactHit=" + std::to_string(deathPresentation ? 1 : 0) +
        " reason=" + (reason && reason[0] ? reason : "-");
    AppendEnemySyncTrace("death", m_lastEnemyLocomotionEvent);
    return true;
}

void ModMain::TickPendingEnemyDeathCommits(float frameTime)
{
    (void)frameTime;
    if (m_pendingEnemyDeathCommits.empty())
        return;

    std::vector<uint64_t> ready;
    ready.reserve(m_pendingEnemyDeathCommits.size());
    for (const auto& entry : m_pendingEnemyDeathCommits)
    {
        if (m_enemyDeathPresentations.find(entry.first) != m_enemyDeathPresentations.end())
            ready.push_back(entry.first);
    }

    for (uint64_t enemyNetId : ready)
    {
        const auto pendingIt = m_pendingEnemyDeathCommits.find(enemyNetId);
        if (pendingIt == m_pendingEnemyDeathCommits.end())
            continue;

        PendingEnemyDeathCommit pending = pendingIt->second;
        ApplyEnemyDeathCommitToLocal(
            enemyNetId,
            pending.archetypeId,
            &pending.position,
            &pending.rotation,
            pending.hasStatePacket ? &pending.statePacket : nullptr,
            "deferred native death presentation");
    }
}

bool ModMain::ApplyRemoteEnemyMannequinAnimation(
    IEntity& entity,
    EnemyAuthorityState& state,
    const CoopProtocol::TestMimicStatePacket& packet,
    bool localHasAttention,
    bool remoteAuthorityHasAttention,
    const char* reason,
    bool reliableActionEdge)
{
    (void)remoteAuthorityHasAttention;

    if (EnvFlagEnabled("COOP_DISABLE_REMOTE_ENEMY_ACTION_MIRROR"))
        return false;

    constexpr bool touched = false;

    int fragmentId = packet.mannequinFragmentId;
    uint32_t sequence = packet.mannequinSequence;
    uint16_t ordinal = packet.mannequinOrdinal;
    int priority = packet.mannequinPriority;
    uint32_t actionFlags = packet.locomotionFlags;
    bool nativeRandomOption =
        (packet.mannequinReserved & CoopProtocol::kEnemyMannequinReservedNativeRandomOption) != 0;
    std::array<uint8_t, 12> tagState = {};
    bool tagStateValid = packet.mannequinTagStateValid != 0;
    if (tagStateValid)
    {
        std::copy(
            std::begin(packet.mannequinTagState),
            std::end(packet.mannequinTagState),
            tagState.begin());
    }

    const bool heldNativeAction =
        sequence == 0 &&
        state.remoteMannequinStateSeconds > 0.0f &&
        state.remoteMannequinSequence != 0 &&
        state.remoteMannequinFragmentId >= 0;
    if (heldNativeAction)
    {
        fragmentId = state.remoteMannequinFragmentId;
        sequence = state.remoteMannequinSequence;
        ordinal = state.remoteMannequinOrdinal;
        priority = state.remoteMannequinPriority;
        actionFlags = state.remoteMannequinFlags;
        nativeRandomOption = state.remoteMannequinRandomOption;
        tagState = state.remoteMannequinTagState;
        tagStateValid = state.remoteMannequinTagStateValid;
    }

    const float now = EnemyAnimationNowSeconds();

    if (sequence == 0 || fragmentId < 0)
        return touched;

    if (!reliableActionEdge &&
        state.remoteNativeMannequinRetiredActions.find(sequence) !=
            state.remoteNativeMannequinRetiredActions.end())
    {
        return touched;
    }

    const CoopEnemyControlPolicy::Decision controlDecision =
        CoopEnemyControlPolicy::Evaluate(BuildLocalEnemyControlPolicyContext(state, entity));
    Quat localAttentionFacing = Quat::CreateIdentity();
    const bool freshNativeLocalCombatFocus =
        controlDecision.localFocus ||
        TryBuildReadOnlyLocalFacingMixTarget(
            state,
            entity,
            now,
            localAttentionFacing);
    CoopEnemyControlPolicy::RemoteActionContext actionContext;
    // Snapshot-time awareness can flicker for one poll while the native
    // attention edge remains current. Keep that exact edge authoritative for
    // the local combat scope so a remote upper-body attack cannot be queued in
    // the gap and starve Vanilla's victim-local attack selection.
    actionContext.mode = freshNativeLocalCombatFocus
        ? CoopEnemyControlPolicy::EnemyControlMode::LocalTargetRemoteOwner
        : controlDecision.mode;
    actionContext.hasRemoteActionPacket = true;
    actionContext.actionFlags = actionFlags;
    actionContext.localHasAttention = localHasAttention;
    actionContext.localCombatActionActive =
        state.localMannequinStateSeconds > 0.0f &&
        state.localMannequinSequence != 0 &&
        state.localMannequinFragmentId >= 0 &&
        (state.localMannequinFlags & CoopProtocol::kEnemyLocomotionFlagAttacking) != 0;
    const CoopEnemyControlPolicy::RemoteActionDecision actionDecision =
        CoopEnemyControlPolicy::EvaluateRemoteAction(actionContext);
    if (actionDecision.suppressActionPacket)
    {
        // Local-target Vanilla combat needs this Mannequin scope free before
        // it can select its own attack. Authority locomotion still comes from
        // the transform follower. Retire every authority presentation lease,
        // not only the newest sequence: a reliable action queued during the
        // brief attention transition can otherwise keep the combat scope and
        // starve all later local attacks even after its sequence is no longer
        // present in the high-rate state.
        state.remoteNativeMannequinActions.clear();
        if (state.remoteNativeMirrorSuppressedSequence != sequence)
        {
            state.remoteNativeMirrorSuppressedSequence = sequence;
            ++m_enemyRemoteAnimationSkips;
            m_lastEnemyMannequinStateEvent =
                "remote_native_exact_suppressed_local_combat net=" + std::to_string(state.netId) +
                " entity=" + std::to_string(entity.GetId()) +
                " fragment=" + std::to_string(fragmentId) +
                " seq=" + std::to_string(sequence) +
                " flags=" + std::to_string(actionFlags) +
                " localAttention=" + std::to_string(localHasAttention ? 1 : 0) +
                " reason=" + (actionDecision.suppressActionReason
                    ? std::string(actionDecision.suppressActionReason)
                    : std::string("-"));
            AppendEnemySyncTrace("remote_anim", m_lastEnemyMannequinStateEvent);
        }
        return true;
    }
    state.remoteNativeMirrorSuppressedSequence = 0;

    if (state.remoteNativeMannequinActions.find(sequence) !=
        state.remoteNativeMannequinActions.end())
    {
        return touched;
    }

    // High-rate state retains a delayed repair route for joins or retired
    // reliable traffic, but it is no longer the primary action transport.
    // This gives the ordered native Start edge time to arrive first.
    if (!reliableActionEdge)
    {
        if (state.remoteNativeMirrorRepairSequence != sequence)
        {
            state.remoteNativeMirrorRepairSequence = sequence;
            state.remoteNativeMirrorRepairWaitSeconds = 0.0f;
        }
        state.remoteNativeMirrorRepairWaitSeconds += kMimicStateTickSeconds;
        if (state.remoteNativeMirrorRepairWaitSeconds < 0.35f)
            return touched;
    }

    if (!tagStateValid ||
        (ordinal == CoopProtocol::kInvalidMannequinOrdinal && !nativeRandomOption))
    {
        ++m_enemyRemoteAnimationSkips;
        m_lastEnemyMannequinStateEvent =
            "remote_native_exact_wait_concrete net=" + std::to_string(state.netId) +
            " entity=" + std::to_string(entity.GetId()) +
            " fragment=" + std::to_string(fragmentId) +
            " seq=" + std::to_string(sequence) +
            " ordinal=" + std::to_string(ordinal) +
            " randomOption=" + std::to_string(nativeRandomOption ? 1 : 0) +
            " tagState=" + std::to_string(tagStateValid ? 1 : 0) +
            " reason=" + (reason && reason[0] ? std::string(reason) : std::string("-"));
        AppendEnemySyncTrace("remote_anim", m_lastEnemyMannequinStateEvent);
        return touched;
    }

    const auto diagnoseNativeSetupFailure =
        [&](const char* stage, const std::string& detail = std::string())
    {
        if (state.remoteNativeMirrorDiagnosedSequence != sequence)
        {
            state.remoteNativeMirrorDiagnosedSequence = sequence;
            ++m_enemyRemoteAnimationSkips;
            m_lastEnemyMannequinStateEvent =
                "remote_native_exact_setup_blocked net=" + std::to_string(state.netId) +
                " entity=" + std::to_string(entity.GetId()) +
                " fragment=" + std::to_string(fragmentId) +
                " seq=" + std::to_string(sequence) +
                " stage=" + (stage && stage[0] ? std::string(stage) : std::string("-")) +
                (detail.empty() ? std::string() : " detail=" + detail);
            AppendEnemySyncTrace("remote_anim", m_lastEnemyMannequinStateEvent);
        }
        return touched;
    };

    ArkNpc* npc = EntityUtils::GetArkNpc(&entity);
    IGameFramework* framework = gEnv && gEnv->pGame
        ? gEnv->pGame->GetIGameFramework()
        : nullptr;
    if (!npc || !framework ||
        !CoopRuntimeGuards::IsLikelyRuntimeCppObject(npc, sizeof(void*) * 4))
    {
        return diagnoseNativeSetupFailure(
            "npc_or_framework",
            "npc=" + std::to_string(npc ? 1 : 0) +
                " framework=" + std::to_string(framework ? 1 : 0));
    }

    void* mannequin = nullptr;
    std::string guardReason;
    if (!TryGuardedCall(
            "exact remote enemy GetMannequinInterface",
            [framework]() -> void*
            {
                return &framework->GetMannequinInterface();
            },
            mannequin,
            &guardReason) ||
        !CoopRuntimeGuards::IsLikelyRuntimeCppObject(mannequin, sizeof(void*)))
    {
        return diagnoseNativeSetupFailure(
            "mannequin_interface",
            guardReason.empty() ? std::string("invalid") : guardReason);
    }

    void** mannequinVtable = *reinterpret_cast<void***>(mannequin);
    if (!CoopRuntimeGuards::IsReadableRuntimePointer(
            mannequinVtable,
            (kMannequinFindActionControllerVtableIndex + 1) * sizeof(void*)) ||
        !CoopRuntimeGuards::IsExecutableRuntimePointer(
            mannequinVtable[kMannequinFindActionControllerVtableIndex]))
    {
        return diagnoseNativeSetupFailure("mannequin_vtable");
    }

    using FindActionControllerFn = void* (*)(void*, const IEntity*);
    auto findActionController = reinterpret_cast<FindActionControllerFn>(
        mannequinVtable[kMannequinFindActionControllerVtableIndex]);
    void* actionController = nullptr;
    if (!TryGuardedCall(
            "exact remote enemy FindActionController",
            [findActionController, mannequin, &entity]()
            {
                return findActionController(mannequin, &entity);
            },
            actionController,
            &guardReason) ||
        !CoopRuntimeGuards::IsLikelyRuntimeCppObject(actionController, sizeof(void*)))
    {
        return diagnoseNativeSetupFailure(
            "find_action_controller",
            guardReason.empty() ? std::string("invalid") : guardReason);
    }

    void** controllerVtable = *reinterpret_cast<void***>(actionController);
    if (!CoopRuntimeGuards::IsReadableRuntimePointer(
            controllerVtable,
            (kActionControllerQueueVtableIndex + 1) * sizeof(void*)) ||
        !CoopRuntimeGuards::IsExecutableRuntimePointer(
            controllerVtable[kActionControllerQueueVtableIndex]))
    {
        return diagnoseNativeSetupFailure("controller_vtable");
    }

    void* actionStorage = nullptr;
    if (!TryGuardedCall(
            "allocate exact remote npc action",
            []() { return s_allocateNativeNpcAnimAction(kNativeNpcAnimActionSize); },
            actionStorage,
            &guardReason) ||
        !actionStorage)
    {
        return diagnoseNativeSetupFailure(
            "allocate_action",
            guardReason.empty() ? std::string("null") : guardReason);
    }

    void* action = nullptr;
    const int nativePriority = std::max(0, priority);
    if (!TryGuardedCall(
            "construct exact remote npc action",
            [actionStorage, npc, nativePriority, fragmentId, &tagState]()
            {
                return s_constructNativeNpcAnimAction(
                    actionStorage,
                    npc,
                    nativePriority,
                    fragmentId,
                    tagState.data(),
                    0,
                    0,
                    0);
            },
            action,
            &guardReason) ||
        !action)
    {
        return diagnoseNativeSetupFailure(
            "construct_action",
            guardReason.empty() ? std::string("null") : guardReason);
    }

    auto* actionBytes = reinterpret_cast<std::byte*>(action);
    *reinterpret_cast<uint32_t*>(actionBytes + kNativeIActionOptionIndexOffset) =
        nativeRandomOption ? 0xfffffffeu : static_cast<uint32_t>(ordinal);
    auto* referenceCount = reinterpret_cast<int*>(
        actionBytes + kNativeIActionReferenceCountOffset);
    ++(*referenceCount); // local smart-pointer ownership during Queue
    ++(*referenceCount); // state lease used by the exact-action start gate
    std::shared_ptr<void> actionLease(action, &StopAndReleaseRemoteNativeAction);

    constexpr size_t kMaxConcurrentRemoteNativeActions = 32;
    if (state.remoteNativeMannequinActions.size() >= kMaxConcurrentRemoteNativeActions)
    {
        const auto oldest = std::min_element(
            state.remoteNativeMannequinActions.begin(),
            state.remoteNativeMannequinActions.end(),
            [](const auto& left, const auto& right)
            {
                return left.second.queuedAtSeconds < right.second.queuedAtSeconds;
            });
        if (oldest != state.remoteNativeMannequinActions.end())
        {
            AppendEnemySyncTrace(
                "remote_anim",
                "remote_native_exact_pruned_stale net=" + std::to_string(state.netId) +
                    " actionSeq=" + std::to_string(oldest->first) +
                    " fragment=" + std::to_string(oldest->second.fragmentId));
            state.remoteNativeMannequinActions.erase(oldest);
        }
    }

    EnemyAuthorityState::RemoteNativeMannequinAction remoteAction;
    remoteAction.lease = std::move(actionLease);
    remoteAction.fragmentId = fragmentId;
    remoteAction.queuedAtSeconds = now;
    state.remoteNativeMannequinActions.emplace(sequence, std::move(remoteAction));

    using QueueActionFn = void (*)(void*, void*, float);
    auto queueAction = reinterpret_cast<QueueActionFn>(
        controllerVtable[kActionControllerQueueVtableIndex]);
    const bool queued = TryGuardedVoidCall(
        "queue exact remote npc action",
        [queueAction, actionController, action]()
        {
            queueAction(actionController, action, -1.0f);
        },
        &guardReason);

    --(*referenceCount);

    if (!queued)
    {
        state.remoteNativeMannequinActions.erase(sequence);
        ++m_enemyRemoteAnimationFailures;
        m_lastEnemyMannequinStateEvent =
            "remote_native_exact_queue_failed net=" + std::to_string(state.netId) +
            " entity=" + std::to_string(entity.GetId()) +
            " fragment=" + std::to_string(fragmentId) +
            " seq=" + std::to_string(sequence) +
            " guard=" + (guardReason.empty() ? std::string("-") : guardReason);
        AppendEnemySyncTrace("remote_anim", m_lastEnemyMannequinStateEvent);
        return touched;
    }

    state.remoteNativeMirrorQueuedSequence = sequence;
    state.remoteNativeMirrorDiagnosedSequence = 0;
    state.remoteNativeMirrorRepairSequence = sequence;
    state.remoteNativeMirrorRepairWaitSeconds = 0.0f;
    ++m_enemyRemoteAnimationApplies;
    m_lastEnemyMannequinStateEvent =
        "remote_native_exact_queued net=" + std::to_string(state.netId) +
        " entity=" + std::to_string(entity.GetId()) +
        " fragment=" + std::to_string(fragmentId) +
        " seq=" + std::to_string(sequence) +
        " ordinal=" + std::to_string(ordinal) +
        " randomOption=" + std::to_string(nativeRandomOption ? 1 : 0) +
        " priority=" + std::to_string(nativePriority) +
        " reliableEdge=" + std::to_string(reliableActionEdge ? 1 : 0) +
        " route=mannequin_action_controller";
    AppendEnemySyncTrace("remote_anim", m_lastEnemyMannequinStateEvent);
    return true;
}

bool ModMain::ShouldMarkEnemyAuthorityHasAttention(
    const EnemyAuthorityState& state,
    bool localAuthorityBlocked) const
{
    if (localAuthorityBlocked ||
        state.entityId == INVALID_ENTITYID ||
        !gEnv ||
        !gEnv->pEntitySystem)
    {
        return false;
    }

    IEntity* entity = gEnv->pEntitySystem->GetEntity(state.entityId);
    return entity &&
        LocalPlayerEnemyAttentionLevelForCoop(*entity) >
            CoopEnemyAuthorityPolicy::kUnknownAttention;
}

bool ModMain::SendClientEnemyAuthorityStateNow(EnemyAuthorityState& state, uint32_t sourceFlags, const char* failurePrefix)
{
    if (m_networkMode != CoopNetworkMode::Client ||
        m_socket == kInvalidNetworkSocket ||
        !m_hasRemoteEndpoint ||
        !IsEnemyReplicationGameplayReady())
    {
        return false;
    }

    CoopProtocol::TestMimicStatePacket packet = {};
    if (!BuildEnemyStatePacket(state, packet))
        return false;

    packet.sourceFlags |= sourceFlags;
    packet.authorityOwnerAccountToken = GetLocalAccountToken();
    packet.authorityEpoch = std::max<uint32_t>(1, state.authorityEpoch);
    const bool localAuthorityBlocked = IsLocalPlayerAuthorityBlockedByModalState() || m_localPlayerDowned;
    packet.authorityAttentionLevel = localAuthorityBlocked
        ? CoopEnemyAuthorityPolicy::kUnknownAttention
        : state.localAttentionLevel;
    if (packet.authorityAttentionLevel > CoopEnemyAuthorityPolicy::kUnknownAttention)
    {
        packet.sourceFlags |= CoopProtocol::kEnemyStateSourceFlagAuthorityHasAttention;
        packet.targetAccountToken = GetLocalAccountToken();
    }
    if (localAuthorityBlocked)
        packet.sourceFlags |= CoopProtocol::kEnemyStateSourceFlagAuthorityBlocked;
    uint32_t hostAddress = m_remoteAddress;
    uint16_t hostPort = m_remotePort;
    if (!ResolveSessionHostEndpoint(hostAddress, hostPort) ||
        !SendReliablePayloadTo(
            static_cast<uint16_t>(CoopProtocol::PacketType::TestMimicState),
            &packet,
            sizeof(packet),
            hostAddress,
            hostPort,
            failurePrefix))
    {
        return false;
    }

    if ((sourceFlags & CoopProtocol::kEnemyStateSourceFlagAuthorityClaim) != 0)
        ++m_enemyAuthorityClaimsSent;
    if ((sourceFlags & CoopProtocol::kEnemyStateSourceFlagAuthorityRelease) != 0)
        ++m_enemyAuthorityReleasesSent;
    if ((sourceFlags & CoopProtocol::kEnemyStateSourceFlagAuthoritySnapshot) != 0)
        ++m_enemyAuthoritySnapshotsSent;

    return true;
}


bool ModMain::ShouldBlockRemoteDrivenEnemyLook(void* lookManagerPtr, const char* stage)
{
    if (EnvFlagEnabled("COOP_DISABLE_REMOTE_ENEMY_LOOK_GATE") ||
        !lookManagerPtr ||
        !gEnv ||
        !gEnv->pEntitySystem ||
        m_networkMode == CoopNetworkMode::Off ||
        !IsSessionGameplayReady())
    {
        return false;
    }

    auto* manager = reinterpret_cast<ArkNpcLookDesireManager*>(lookManagerPtr);
    if (!CoopRuntimeGuards::IsLikelyRuntimeCppObject(manager, sizeof(void*) * 4))
        return false;

    ArkNpc* npc = nullptr;
    std::string reason;
    if (!TryGuardedCall(
            "remote enemy look gate read npc",
            [manager]() -> ArkNpc*
            {
                return manager->m_pArkNpc;
            },
            npc,
            &reason) ||
        !npc ||
        !CoopRuntimeGuards::IsLikelyRuntimeCppObject(npc, sizeof(void*) * 4))
    {
        return false;
    }

    EntityId entityId = INVALID_ENTITYID;
    if (!TryGuardedCall("remote enemy look gate GetEntityId", [npc]() { return npc->GetEntityId(); }, entityId, &reason) ||
        entityId == INVALID_ENTITYID)
    {
        return false;
    }

    IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId);
    if (!entity)
        return false;
    if (IsRemoteProxyEntityOrSpawnName(*entity))
        return true;
    if (!m_enemyLocomotionSyncEnabled || !IsEnemyReplicationGameplayReady())
        return false;
    if (!IsEnemyRuntimeControlCandidate(*entity))
        return false;

    const auto netIt = m_enemyNetIdsByEntity.find(entityId);
    if (netIt == m_enemyNetIdsByEntity.end())
        return false;

    EnemyAuthorityState* state = FindEnemyAuthorityByNetId(netIt->second);
    if (!state)
        return false;

    const CoopEnemyControlPolicy::Decision decision =
        CoopEnemyControlPolicy::Evaluate(BuildLocalEnemyControlPolicyContext(*state, *entity));
    if (!decision.blockLook)
    {
        if (decision.localFocus)
        {
            ++state->localLookIntentAllows;
            RecordRemoteObserverLocalIntentSample(
                *state,
                *entity,
                EnemyAuthorityState::ReadOnlyIntentLook,
                CoopProtocol::kEnemyLocomotionFlagTurning,
                0,
                INVALID_ENTITYID,
                stage,
                false);
        }
        return false;
    }

    RecordRemoteObserverLocalIntentSample(
        *state,
        *entity,
        EnemyAuthorityState::ReadOnlyIntentLook,
        CoopProtocol::kEnemyLocomotionFlagTurning,
        0,
        INVALID_ENTITYID,
        stage,
        true);

    ++m_enemyLookRequestBlocks;
    m_lastEnemyLookEvent =
        "blocked remote enemy look stage=" + std::string(stage && stage[0] ? stage : "-") +
        " net=" + std::to_string(state->netId) +
        " entity=" + std::to_string(entityId) +
        " source=local_vanilla_blocked" +
        " count=" + std::to_string(m_enemyLookRequestBlocks);
    return true;
}

bool ModMain::ShouldBlockRemoteDrivenEnemyLookaround(void* npcPtr, const char* stage)
{
    if (EnvFlagEnabled("COOP_DISABLE_REMOTE_ENEMY_LOOKAROUND_GATE") ||
        !npcPtr ||
        !gEnv ||
        !gEnv->pEntitySystem ||
        m_networkMode == CoopNetworkMode::Off ||
        !IsSessionGameplayReady())
    {
        return false;
    }

    auto* npc = reinterpret_cast<ArkNpc*>(npcPtr);
    if (!CoopRuntimeGuards::IsLikelyRuntimeCppObject(npc, sizeof(void*) * 4))
        return false;

    EntityId entityId = INVALID_ENTITYID;
    std::string reason;
    if (!TryGuardedCall("remote enemy lookaround gate GetEntityId", [npc]() { return npc->GetEntityId(); }, entityId, &reason) ||
        entityId == INVALID_ENTITYID)
    {
        return false;
    }

    IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId);
    if (!entity)
        return false;
    if (IsRemoteProxyEntityOrSpawnName(*entity))
        return true;
    if (!m_enemyLocomotionSyncEnabled || !IsEnemyReplicationGameplayReady())
        return false;
    if (!IsEnemyRuntimeControlCandidate(*entity))
        return false;

    const auto netIt = m_enemyNetIdsByEntity.find(entityId);
    if (netIt == m_enemyNetIdsByEntity.end())
        return false;

    EnemyAuthorityState* state = FindEnemyAuthorityByNetId(netIt->second);
    if (!state)
        return false;

    const CoopEnemyControlPolicy::Decision decision =
        CoopEnemyControlPolicy::Evaluate(BuildLocalEnemyControlPolicyContext(*state, *entity));
    if (!decision.blockLook)
    {
        if (decision.localFocus)
        {
            ++state->localLookIntentAllows;
            RecordRemoteObserverLocalIntentSample(
                *state,
                *entity,
                EnemyAuthorityState::ReadOnlyIntentLook,
                CoopProtocol::kEnemyLocomotionFlagTurning,
                0,
                INVALID_ENTITYID,
                stage,
                false);
        }
        return false;
    }

    RecordRemoteObserverLocalIntentSample(
        *state,
        *entity,
        EnemyAuthorityState::ReadOnlyIntentLook,
        CoopProtocol::kEnemyLocomotionFlagTurning,
        0,
        INVALID_ENTITYID,
        stage,
        true);

    ++m_enemyLookRequestBlocks;
    m_lastEnemyLookEvent =
        "blocked remote enemy lookaround stage=" + std::string(stage && stage[0] ? stage : "-") +
        " net=" + std::to_string(state->netId) +
        " entity=" + std::to_string(entityId) +
        " source=local_vanilla_blocked" +
        " count=" + std::to_string(m_enemyLookRequestBlocks);
    return true;
}

bool ModMain::ShouldBlockRemoteDrivenEnemyFacing(void* facingManagerPtr, const char* stage)
{
    if (EnvFlagEnabled("COOP_DISABLE_REMOTE_ENEMY_FACING_GATE") ||
        !facingManagerPtr ||
        !gEnv ||
        !gEnv->pEntitySystem ||
        m_networkMode == CoopNetworkMode::Off ||
        !IsSessionGameplayReady())
    {
        return false;
    }

    auto* manager = reinterpret_cast<ArkNpcFacingDesireManager*>(facingManagerPtr);
    if (!CoopRuntimeGuards::IsLikelyRuntimeCppObject(manager, sizeof(void*) * 4))
        return false;

    ArkNpc* npc = nullptr;
    std::string reason;
    if (!TryGuardedCall(
            "remote enemy facing gate read npc",
            [manager]() -> ArkNpc*
            {
                return manager->m_pArkNpc;
            },
            npc,
            &reason) ||
        !npc ||
        !CoopRuntimeGuards::IsLikelyRuntimeCppObject(npc, sizeof(void*) * 4))
    {
        return false;
    }

    EntityId entityId = INVALID_ENTITYID;
    if (!TryGuardedCall("remote enemy facing gate GetEntityId", [npc]() { return npc->GetEntityId(); }, entityId, &reason) ||
        entityId == INVALID_ENTITYID)
    {
        return false;
    }

    IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId);
    if (!entity)
        return false;
    if (IsRemoteProxyEntityOrSpawnName(*entity))
        return true;
    if (!m_enemyLocomotionSyncEnabled || !IsEnemyReplicationGameplayReady())
        return false;
    if (!IsEnemyRuntimeControlCandidate(*entity))
        return false;

    const auto netIt = m_enemyNetIdsByEntity.find(entityId);
    if (netIt == m_enemyNetIdsByEntity.end())
        return false;

    EnemyAuthorityState* state = FindEnemyAuthorityByNetId(netIt->second);
    if (!state)
        return false;

    const CoopEnemyControlPolicy::Decision decision =
        CoopEnemyControlPolicy::Evaluate(BuildLocalEnemyControlPolicyContext(*state, *entity));
    if (!decision.blockFacing)
    {
        if (decision.localFocus)
        {
            ++state->localFacingIntentAllows;
            RecordRemoteObserverLocalIntentSample(
                *state,
                *entity,
                EnemyAuthorityState::ReadOnlyIntentFacing,
                CoopProtocol::kEnemyLocomotionFlagTurning,
                0,
                INVALID_ENTITYID,
                stage,
                false);
        }
        return false;
    }

    RecordRemoteObserverLocalIntentSample(
        *state,
        *entity,
        EnemyAuthorityState::ReadOnlyIntentFacing,
        CoopProtocol::kEnemyLocomotionFlagTurning,
        0,
        INVALID_ENTITYID,
        stage,
        true);

    ++m_enemyLookRequestBlocks;
    m_lastEnemyLookEvent =
        "blocked remote enemy facing stage=" + std::string(stage && stage[0] ? stage : "-") +
        " net=" + std::to_string(state->netId) +
        " entity=" + std::to_string(entityId) +
        " source=local_vanilla_blocked" +
        " count=" + std::to_string(m_enemyLookRequestBlocks);
    return true;
}

void ModMain::RecordAuthorityEnemyMovementDesire(void* movementManagerPtr, const char* stage)
{
    if (EnvFlagEnabled("COOP_DISABLE_ENEMY_NATIVE_MOVEMENT_CAPTURE") ||
        !movementManagerPtr ||
        !gEnv ||
        !gEnv->pEntitySystem ||
        m_networkMode == CoopNetworkMode::Off ||
        !m_enemyLocomotionSyncEnabled ||
        !IsEnemyReplicationGameplayReady())
    {
        return;
    }

    auto* manager = reinterpret_cast<ArkNpcMovementDesireManager*>(movementManagerPtr);
    if (!CoopRuntimeGuards::IsLikelyRuntimeCppObject(manager, sizeof(void*) * 4))
        return;

    ArkNpc* npc = nullptr;
    std::string reason;
    if (!TryGuardedCall(
            "authority enemy movement desire read npc",
            [manager]() -> ArkNpc*
            {
                return manager->m_pArkNpc;
            },
            npc,
            &reason) ||
        !npc ||
        !CoopRuntimeGuards::IsLikelyRuntimeCppObject(npc, sizeof(void*) * 4))
    {
        return;
    }

    EntityId entityId = INVALID_ENTITYID;
    if (!TryGuardedCall("authority enemy movement desire GetEntityId", [npc]() { return npc->GetEntityId(); }, entityId, &reason) ||
        entityId == INVALID_ENTITYID)
    {
        return;
    }

    IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId);
    if (!entity || !IsEnemyRuntimeControlCandidate(*entity))
        return;

    EnemyAuthorityState& state = EnsureEnemyAuthorityState(*entity);
    if (m_networkMode == CoopNetworkMode::Client &&
        state.remoteLocomotionAuthority &&
        !state.localAttentionClaimed)
    {
        return;
    }

    ArkNpcMovementDesire* desire = ReadActiveMovementDesireFromManager(manager);
    Vec3 direction(ZERO);
    float speed = 0.0f;
    uint32_t flags = 0;
    if (!ReadMovementDesireSnapshot(
            desire,
            entity->GetWorldRotation().GetColumn1(),
            ResolveNpcMannequinKindForRuntime(entity) == "phantom",
            direction,
            speed,
            flags))
    {
        state.localNativeMovementSeconds = 0.0f;
        state.localNativeMoveSpeed = 0.0f;
        state.localNativeLocomotionFlags = 0;
        m_lastEnemyLocomotionEvent =
            "cleared authority movement desire stage=" + std::string(stage && stage[0] ? stage : "-") +
            " net=" + std::to_string(state.netId) +
            " entity=" + std::to_string(entityId) +
            " reason=no_active_desire";
        return;
    }

    state.localNativeMoveDirection = direction;
    state.localNativeMoveSpeed = speed;
    state.localNativeLocomotionFlags = flags;
    state.localNativeMovementSeconds = kEnemyNativeMovementIntentSeconds;
    m_lastEnemyLocomotionEvent =
        "captured authority movement desire stage=" + std::string(stage && stage[0] ? stage : "-") +
        " net=" + std::to_string(state.netId) +
        " entity=" + std::to_string(entityId) +
        " speed=" + std::to_string(speed) +
        " flags=" + std::to_string(flags);
}

void ModMain::RecordAuthorityEnemyMovementRequest(void* movementManagerPtr, const MovementRequest& request, const char* stage)
{
    if (EnvFlagEnabled("COOP_DISABLE_ENEMY_NATIVE_MOVEMENT_CAPTURE") ||
        !movementManagerPtr ||
        !gEnv ||
        !gEnv->pEntitySystem ||
        m_networkMode == CoopNetworkMode::Off ||
        !m_enemyLocomotionSyncEnabled ||
        !IsEnemyReplicationGameplayReady())
    {
        return;
    }

    auto* manager = reinterpret_cast<ArkNpcMovementDesireManager*>(movementManagerPtr);
    if (!CoopRuntimeGuards::IsLikelyRuntimeCppObject(manager, sizeof(void*) * 4))
        return;

    ArkNpc* npc = nullptr;
    std::string reason;
    if (!TryGuardedCall(
            "authority enemy movement request read npc",
            [manager]() -> ArkNpc*
            {
                return manager->m_pArkNpc;
            },
            npc,
            &reason) ||
        !npc ||
        !CoopRuntimeGuards::IsLikelyRuntimeCppObject(npc, sizeof(void*) * 4))
    {
        return;
    }

    EntityId entityId = INVALID_ENTITYID;
    if (!TryGuardedCall("authority enemy movement request GetEntityId", [npc]() { return npc->GetEntityId(); }, entityId, &reason) ||
        entityId == INVALID_ENTITYID)
    {
        return;
    }

    IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId);
    if (!entity || !IsEnemyRuntimeControlCandidate(*entity))
        return;

    EnemyAuthorityState& state = EnsureEnemyAuthorityState(*entity);
    if (m_networkMode == CoopNetworkMode::Client &&
        state.remoteLocomotionAuthority &&
        !state.localAttentionClaimed)
    {
        return;
    }

    if (request.type != MovementRequest::Type::MoveTo)
    {
        state.localNativeMovementSeconds = 0.0f;
        state.localNativeMoveSpeed = 0.0f;
        state.localNativeLocomotionFlags = 0;
        return;
    }

    const Vec3 currentPosition = entity->GetWorldPos();
    Vec3 direction = NormalizeDirectionOr(request.destination - currentPosition, entity->GetWorldRotation().GetColumn1());
    float speed = request.style.m_hasSpeedLiteral
        ? request.style.m_speedLiteral
        : EnemyMovementFallbackSpeed(request.style.m_speed);
    if (!std::isfinite(speed) || speed < 0.0f)
        speed = 0.0f;
    const uint32_t flags = EnemyMovementFlagsFromStyle(
        request.style,
        speed,
        ResolveNpcMannequinKindForRuntime(entity) == "phantom");
    if (flags == 0)
    {
        state.localNativeMovementSeconds = 0.0f;
        state.localNativeMoveSpeed = 0.0f;
        state.localNativeLocomotionFlags = 0;
        return;
    }

    state.localNativeMoveDirection = direction;
    state.localNativeMoveSpeed = speed;
    state.localNativeLocomotionFlags = flags;
    state.localNativeMovementSeconds = kEnemyNativeMovementIntentSeconds;
    m_lastEnemyLocomotionEvent =
        "captured authority movement request stage=" + std::string(stage && stage[0] ? stage : "-") +
        " net=" + std::to_string(state.netId) +
        " entity=" + std::to_string(entityId) +
        " speed=" + std::to_string(speed) +
        " flags=" + std::to_string(flags);
}

void ModMain::RecordAuthorityEnemyMovementResult(
    void* movementManagerPtr,
    const MovementRequestResult& result,
    const char* stage)
{
    if (!movementManagerPtr ||
        !gEnv ||
        !gEnv->pEntitySystem ||
        m_networkMode == CoopNetworkMode::Off ||
        !m_enemyLocomotionSyncEnabled ||
        !IsEnemyReplicationGameplayReady())
    {
        return;
    }

    auto* manager = reinterpret_cast<ArkNpcMovementDesireManager*>(movementManagerPtr);
    if (!CoopRuntimeGuards::IsLikelyRuntimeCppObject(manager, sizeof(void*) * 4))
        return;

    ArkNpc* npc = nullptr;
    EntityId entityId = INVALID_ENTITYID;
    std::string reason;
    if (!TryGuardedCall(
            "authority enemy movement result read npc",
            [manager]() -> ArkNpc* { return manager->m_pArkNpc; },
            npc,
            &reason) ||
        !npc ||
        !CoopRuntimeGuards::IsLikelyRuntimeCppObject(npc, sizeof(void*) * 4) ||
        !TryGuardedCall(
            "authority enemy movement result GetEntityId",
            [npc]() { return npc->GetEntityId(); },
            entityId,
            &reason) ||
        entityId == INVALID_ENTITYID)
    {
        return;
    }

    IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId);
    if (!entity || !IsEnemyRuntimeControlCandidate(*entity))
        return;

    EnemyAuthorityState& state = EnsureEnemyAuthorityState(*entity);
    const bool localOwner =
        !state.remoteLocomotionAuthority &&
        state.authorityOwnerAccountToken == GetLocalAccountToken();
    const int resultCode = static_cast<int>(result.result);
    const int failureReason = static_cast<int>(result.failureReason);
    m_lastEnemyLocomotionEvent =
        "native movement result stage=" + std::string(stage && stage[0] ? stage : "-") +
        " net=" + std::to_string(state.netId) +
        " entity=" + std::to_string(entityId) +
        " owner=" + std::to_string(localOwner ? 1 : 0) +
        " callbackRequest=" + std::to_string(result.requestID.id) +
        " managerRequest=" + std::to_string(manager->m_movementRequestId.id) +
        " stopRequest=" + std::to_string(manager->m_stopRequestId.id) +
        " result=" + std::to_string(resultCode) +
        " failure=" + std::to_string(failureReason);
    AppendEnemySyncTrace("movement_result", m_lastEnemyLocomotionEvent);
}

void ModMain::OnNativeNpcGlooStateChanged(ArkNpc* npc, const char* stage, bool frozen)
{
    if (!npc ||
        m_applyingRemoteEnemyGlooState ||
        m_applyingRemoteGooResult ||
        m_networkMode == CoopNetworkMode::Off ||
        m_socket == kInvalidNetworkSocket ||
        !m_hasRemoteEndpoint ||
        !IsEnemyReplicationGameplayReady() ||
        !gEnv ||
        !gEnv->pEntitySystem)
    {
        return;
    }

    EntityId entityId = INVALID_ENTITYID;
    std::string reason;
    if (!TryGuardedCall("npc gloo state GetEntityId", [npc]() { return npc->GetEntityId(); }, entityId, &reason) ||
        entityId == INVALID_ENTITYID)
    {
        return;
    }

    IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId);
    if (!entity || !IsEnemyRuntimeControlCandidate(*entity))
        return;

    if (IsRemoteProxyEntity(entityId))
        return;

    if (frozen)
        npc->m_bIsFrozenInGloo = true;

    EnemyAuthorityState* state = nullptr;
    const auto netIt = m_enemyNetIdsByEntity.find(entityId);
    if (netIt != m_enemyNetIdsByEntity.end())
        state = FindEnemyAuthorityByNetId(netIt->second);
    if (!state &&
        (m_networkMode == CoopNetworkMode::Host ||
            (m_networkMode == CoopNetworkMode::Client && m_clientAreaAuthorityActive)))
    {
        // This native callback already identifies the exact enemy. Register
        // that entity directly instead of turning a frequent GLOO-state edge
        // into a full-world scan. Observers still wait for their current Area
        // Authority's roster so they cannot allocate colliding local net ids.
        state = &EnsureEnemyAuthorityState(*entity);
    }
    if (!state)
        return;

    state->entityId = entityId;
    state->remoteLocomotionFlags = frozen
        ? (state->remoteLocomotionFlags | CoopProtocol::kEnemyLocomotionFlagGlooed)
        : (state->remoteLocomotionFlags & ~CoopProtocol::kEnemyLocomotionFlagGlooed);

    const uint64_t localAccountToken = GetLocalAccountToken();
    const bool localOwnsEnemy =
        !state->remoteLocomotionAuthority &&
        state->authorityOwnerAccountToken == localAccountToken;
    bool sent = false;
    if (localOwnsEnemy &&
        (m_networkMode == CoopNetworkMode::Host || IsClientAreaAuthorityActive()))
    {
        sent = SendEnemyStateNow(*state, "enemy gloo state send failed");
    }
    else if (localOwnsEnemy && m_networkMode == CoopNetworkMode::Client)
    {
        sent = SendClientEnemyAuthorityStateNow(
            *state,
            CoopProtocol::kEnemyStateSourceFlagAuthorityClaim |
                CoopProtocol::kEnemyStateSourceFlagAuthoritySnapshot,
            "enemy gloo authority state send failed");
    }

    m_lastEnemyLocomotionEvent =
        "native enemy gloo state stage=" + std::string(stage && stage[0] ? stage : "-") +
        " net=" + std::to_string(state->netId) +
        " entity=" + std::to_string(entityId) +
        " frozen=" + std::to_string(frozen ? 1 : 0) +
        " sent=" + std::to_string(sent ? 1 : 0);
}

void ModMain::OnNativeNpcPersistentStatusChanged(
    ArkNpc* npc,
    uint32_t statusFlag,
    bool enabled,
    const char* stage)
{
    const uint32_t supportedFlags =
        CoopProtocol::kEnemyLocomotionFlagMindControlled |
        CoopProtocol::kEnemyLocomotionFlagPsiSuppressed;
    if (!npc ||
        m_applyingRemoteEnemyPersistentStatus ||
        (statusFlag & supportedFlags) == 0 ||
        (statusFlag & ~supportedFlags) != 0 ||
        m_networkMode == CoopNetworkMode::Off ||
        !IsEnemyReplicationGameplayReady() ||
        !gEnv ||
        !gEnv->pEntitySystem)
    {
        return;
    }

    EntityId entityId = INVALID_ENTITYID;
    if (!TryGuardedCall(
            "npc persistent status GetEntityId",
            [npc]() { return npc->GetEntityId(); },
            entityId,
            nullptr) ||
        entityId == INVALID_ENTITYID)
    {
        return;
    }

    IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId);
    if (!entity || !IsEnemyRuntimeControlCandidate(*entity) || IsRemoteProxyEntity(entityId))
        return;

    EnemyAuthorityState* state = nullptr;
    if (const auto netIt = m_enemyNetIdsByEntity.find(entityId); netIt != m_enemyNetIdsByEntity.end())
        state = FindEnemyAuthorityByNetId(netIt->second);
    if (!state &&
        (m_networkMode == CoopNetworkMode::Host || IsClientAreaAuthorityActive()))
        state = &EnsureEnemyAuthorityState(*entity);
    if (!state)
        return;

    if (enabled)
        state->localPersistentStatusFlags |= statusFlag;
    else
        state->localPersistentStatusFlags &= ~statusFlag;

    m_lastEnemyLocomotionEvent =
        "native enemy persistent status stage=" + std::string(stage && stage[0] ? stage : "-") +
        " net=" + std::to_string(state->netId) +
        " entity=" + std::to_string(entityId) +
        " flag=" + std::to_string(statusFlag) +
        " enabled=" + std::to_string(enabled ? 1 : 0) +
        " localFlags=" + std::to_string(state->localPersistentStatusFlags);
    AppendEnemySyncTrace("status", m_lastEnemyLocomotionEvent);
}

bool ModMain::ShouldBlockRemoteEnemyCollisionCallback(
    void* npcPtr,
    const char* stage,
    EntityId instigatorEntityId)
{
    (void)stage;
    (void)instigatorEntityId;
    if (!npcPtr ||
        m_networkMode == CoopNetworkMode::Off ||
        !m_enemyLocomotionSyncEnabled ||
        !IsEnemyReplicationGameplayReady() ||
        !gEnv ||
        !gEnv->pEntitySystem)
    {
        return false;
    }

    auto* npc = reinterpret_cast<ArkNpc*>(npcPtr);
    if (!CoopRuntimeGuards::IsLikelyRuntimeCppObject(npc, sizeof(void*) * 4))
        return false;

    EntityId entityId = INVALID_ENTITYID;
    if (!TryGuardedCall(
            "remote enemy collision policy GetEntityId",
            [npc]() { return npc->GetEntityId(); },
            entityId,
            nullptr) ||
        entityId == INVALID_ENTITYID)
    {
        return false;
    }

    IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId);
    if (!entity || !IsEnemyRuntimeControlCandidate(*entity))
        return false;

    EnemyAuthorityState* state = nullptr;
    const auto netIt = m_enemyNetIdsByEntity.find(entityId);
    if (netIt != m_enemyNetIdsByEntity.end())
        state = FindEnemyAuthorityByNetId(netIt->second);
    if (!state)
        return false;

    const CoopEnemyControlPolicy::Decision decision =
        CoopEnemyControlPolicy::Evaluate(BuildLocalEnemyControlPolicyContext(*state, *entity));
    return decision.remoteDriven && !decision.localVanillaAuthority;
}

bool ModMain::ShouldBlockRemoteEnemyRagdoll(void* npcPtr, const char* stage)
{
    if (!npcPtr ||
        m_networkMode == CoopNetworkMode::Off ||
        !m_enemyLocomotionSyncEnabled ||
        !IsEnemyReplicationGameplayReady() ||
        !gEnv ||
        !gEnv->pEntitySystem)
    {
        return false;
    }

    auto* npc = reinterpret_cast<ArkNpc*>(npcPtr);
    if (!CoopRuntimeGuards::IsLikelyRuntimeCppObject(npc, sizeof(void*) * 4))
        return false;

    EntityId entityId = INVALID_ENTITYID;
    if (!TryGuardedCall(
            "remote enemy ragdoll policy GetEntityId",
            [npc]() { return npc->GetEntityId(); },
            entityId,
            nullptr) ||
        entityId == INVALID_ENTITYID)
    {
        return false;
    }

    if (IsRemoteProxyEntity(entityId))
        return ShouldSuppressProxyNpcNativeState(entityId, stage ? stage : "Ragdoll");

    IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId);
    if (!entity || !IsEnemyRuntimeControlCandidate(*entity))
        return false;

    EnemyAuthorityState* state = nullptr;
    if (const auto netIt = m_enemyNetIdsByEntity.find(entityId); netIt != m_enemyNetIdsByEntity.end())
        state = FindEnemyAuthorityByNetId(netIt->second);
    if (!state)
        return false;

    const CoopEnemyControlPolicy::Decision decision =
        CoopEnemyControlPolicy::Evaluate(BuildLocalEnemyControlPolicyContext(*state, *entity));
    if (!decision.remoteDriven || decision.localVanillaAuthority || m_applyingRemoteEnemyDeathCommit)
        return false;

    // Ragdoll is shared body state. Local attention may choose attacks and
    // facing, but a local collision/explosion must not collapse a body owned
    // by another peer. An exact authority hit/recovery action is already
    // identified on the wire as HitReacting and remains native pass-through.
    const uint32_t authorityBodyFlags =
        state->remoteLocomotionFlags | state->remoteMannequinFlags;
    if ((authorityBodyFlags & (
            CoopProtocol::kEnemyLocomotionFlagHitReacting |
            CoopProtocol::kEnemyLocomotionFlagRagdolled)) != 0)
    {
        ++m_remoteEnemyAuthorityRagdollPasses;
        m_lastRemoteEnemyRagdollEvent =
            "allowed_authority_enemy_ragdoll stage=" +
            std::string(stage && stage[0] ? stage : "-") +
            " net=" + std::to_string(state->netId) +
            " entity=" + std::to_string(entityId) +
            " flags=" + std::to_string(authorityBodyFlags);
        AppendEnemySyncTrace("ragdoll_gate", m_lastRemoteEnemyRagdollEvent);
        return false;
    }

    ++m_remoteEnemyRagdollSuppressions;
    m_lastRemoteEnemyRagdollEvent =
        "blocked_remote_enemy_ragdoll stage=" +
        std::string(stage && stage[0] ? stage : "-") +
        " net=" + std::to_string(state->netId) +
        " entity=" + std::to_string(entityId) +
        " mode=" + std::string(CoopEnemyControlPolicy::ModeName(decision.mode)) +
        " flags=" + std::to_string(authorityBodyFlags) +
        " count=" + std::to_string(m_remoteEnemyRagdollSuppressions);
    AppendEnemySyncTrace("ragdoll_gate", m_lastRemoteEnemyRagdollEvent);
    return true;
}

void ModMain::TickRemoteEnemySmoothing(float frameTime)
{
    if (EnvFlagEnabled("COOP_DISABLE_REMOTE_ENEMY_SMOOTH_TICK") ||
        m_networkMode == CoopNetworkMode::Off ||
        !m_enemyLocomotionSyncEnabled ||
        !IsEnemyReplicationGameplayReady() ||
        !gEnv ||
        !gEnv->pEntitySystem)
    {
        return;
    }

    const float tickSeconds = std::clamp(std::max(frameTime, 0.0f), 0.0f, 0.05f);
    if (tickSeconds <= 0.0f)
        return;
    const float nowSeconds = gEnv && gEnv->pTimer ? gEnv->pTimer->GetAsyncCurTime() : -1.0f;

    struct ScopedRemoteEnemyTransformWrite
    {
        explicit ScopedRemoteEnemyTransformWrite(uint32_t& depth)
            : m_depth(depth)
        {
            ++m_depth;
        }

        ~ScopedRemoteEnemyTransformWrite()
        {
            --m_depth;
        }

        uint32_t& m_depth;
    };

    for (auto& entry : m_enemyAuthorities)
    {
        EnemyAuthorityState& state = entry.second;
        if (!state.hasLastPosition ||
            state.entityId == INVALID_ENTITYID)
        {
            continue;
        }

        IEntity* entity = gEnv->pEntitySystem->GetEntity(state.entityId);
        if (!entity || !IsEnemyRuntimeControlCandidate(*entity))
            continue;

        ArkNpc* npc = EntityUtils::GetArkNpc(entity);
        if (!npc || npc->IsDead())
            continue;

        const CoopEnemyControlPolicy::Decision controlDecision =
            CoopEnemyControlPolicy::Evaluate(BuildLocalEnemyControlPolicyContext(state, *entity));
        SetRemoteEnemyMirrorPhysics(
            *entity,
            controlDecision.blockWorldCollision,
            "enemy smoothing authority policy");
        if (!controlDecision.remoteDriven || controlDecision.localVanillaAuthority)
        {
            // Vanilla immediately resumes writing its own motion parameters.
            // Drop our bookkeeping without injecting a synthetic zero frame
            // into the newly authoritative NPC.
            state.remoteLegBlendSpeed = 0.0f;
            state.remoteLegBlendAngle = 0.0f;
            state.remoteLegBlendOwned = false;
            continue;
        }

        Quat localFacingMixTarget = Quat::CreateIdentity();
        // On a mixed observer, Vanilla owns the target and combat decision but
        // the blocked movement planner does not reliably emit a separate
        // FacingDesire request. Follow the native local attention edge here so
        // authority locomotion cannot leave the body aimed at the remote
        // player while a victim-local attack runs. Hard authored body actions
        // are rejected by TryBuildReadOnlyLocalFacingMixTarget.
        const bool applyReadOnlyLocalFacingMix =
            (controlDecision.blockTurn || controlDecision.localFocus) &&
            TryBuildReadOnlyLocalFacingMixTarget(
                state,
                *entity,
                nowSeconds,
                localFacingMixTarget);
        const bool requestedSmoothPosition = controlDecision.blockMovement;
        const bool requestedSmoothRotation =
            controlDecision.blockTurn && !applyReadOnlyLocalFacingMix;
        if (!requestedSmoothPosition && !requestedSmoothRotation)
            continue;
        // This is the sole continuous transform writer. Do not suppress a
        // render tick merely because the previous frame was less than 8 ms
        // ago; that reduced 120+ FPS motion to an every-other-frame staircase.
        const bool smoothPosition = requestedSmoothPosition;
        const bool smoothRotation = requestedSmoothRotation;
        if (!smoothPosition && !smoothRotation && !applyReadOnlyLocalFacingMix)
            continue;

        Vec3 currentPosition = entity->GetWorldPos();
        Quat currentRotation = entity->GetWorldRotation();
        currentRotation.Normalize();

        Vec3 appliedPosition = currentPosition;
        Quat appliedRotation = currentRotation;
        RemoteEnemyTransformSmoothingResult smooth;
        if (!ComputeRemoteEnemyTransformSmoothing(
                state,
                currentPosition,
                currentRotation,
                smoothPosition,
                smoothRotation,
                tickSeconds,
                appliedPosition,
                appliedRotation,
                smooth))
        {
            if (!applyReadOnlyLocalFacingMix)
                continue;
        }

        bool localFacingMixed = false;
        if (applyReadOnlyLocalFacingMix)
        {
            const float facingAlpha = std::clamp(
                1.0f - std::exp(-kEnemyReadOnlyFacingMixResponse * tickSeconds),
                0.0f,
                1.0f);
            const float facingAngle = ComputeRemoteEnemyRotationAngle(currentRotation, localFacingMixTarget);
            if (facingAngle > 0.002f)
            {
                appliedRotation = Quat::CreateNlerp(currentRotation, localFacingMixTarget, facingAlpha);
                appliedRotation.Normalize();
                localFacingMixed = true;
            }
            ++state.localReadOnlyFacingMixApplies;
            ++m_enemyReadOnlyFacingMixApplies;
            if (nowSeconds < state.localReadOnlyFacingMixTraceAtSeconds ||
                nowSeconds - state.localReadOnlyFacingMixTraceAtSeconds >= 1.0f)
            {
                state.localReadOnlyFacingMixTraceAtSeconds = nowSeconds;
                AppendEnemySyncTrace(
                    "local_facing_mix",
                    "applied controlled read_only facing"
                    " net=" + std::to_string(state.netId) +
                        " entity=" + std::to_string(state.entityId) +
                        " attentionTarget=" + std::to_string(state.localReadOnlyAttentionTargetEntityId) +
                        " alpha=" + std::to_string(facingAlpha) +
                        " angle=" + std::to_string(facingAngle) +
                        " positionWriter=authority_follower" +
                        (controlDecision.localFocus
                            ? " actionWriter=native_local_combat localAiWriter=attention_target"
                            : " actionWriter=exact_native_authority localAiWriter=blocked"));
            }
        }
        else if (state.localReadOnlyAttentionActive)
        {
            ++state.localReadOnlyFacingMixRejects;
            ++m_enemyReadOnlyFacingMixRejects;
        }

        const uint32_t legBlendBlockingFlags =
            CoopProtocol::kEnemyLocomotionFlagDashing |
            CoopProtocol::kEnemyLocomotionFlagShifting |
            CoopProtocol::kEnemyLocomotionFlagMorphing |
            CoopProtocol::kEnemyLocomotionFlagLunging |
            CoopProtocol::kEnemyLocomotionFlagGlooed |
            CoopProtocol::kEnemyLocomotionFlagStunned |
            CoopProtocol::kEnemyLocomotionFlagCowering |
            CoopProtocol::kEnemyLocomotionFlagHitReacting;
        const bool localTargetLegBlendMode =
            controlDecision.mode ==
                CoopEnemyControlPolicy::EnemyControlMode::LocalTargetRemoteOwner;
        const uint32_t authorityContinuousMotion =
            state.remoteVisualMotionFlags &
            CoopEnemyControlPolicy::ContinuousMovementFlags();
        const bool mixAuthorityLegMotion =
            localTargetLegBlendMode &&
            authorityContinuousMotion != 0 &&
            ((state.remoteLocomotionFlags | state.remoteMannequinFlags) &
                legBlendBlockingFlags) == 0;
        if (mixAuthorityLegMotion)
        {
            // This does not queue an action, create a movement desire, move the
            // entity, or replace the local combat pose. It only drives the two
            // locomotion dimensions of the currently playing native blendspace,
            // so authority displacement reaches the legs while local targeting,
            // facing and upper-body attacks remain Vanilla.
            const Vec3 visibleDelta = appliedPosition - currentPosition;
            const float visibleSpeed = visibleDelta.GetLength() /
                std::max(smooth.tickSeconds, 0.001f);
            float targetLegSpeed = visibleSpeed >= kEnemyRemoteLegBlendStartSpeed
                ? std::min(visibleSpeed, kEnemyRemoteLegBlendMaxSpeed)
                : 0.0f;
            if (targetLegSpeed <= 0.0f &&
                state.remoteLegBlendSpeed <= kEnemyRemoteLegBlendStopSpeed)
            {
                state.remoteLegBlendSpeed = 0.0f;
            }
            else
            {
                const float response = targetLegSpeed > state.remoteLegBlendSpeed
                    ? kEnemyRemoteLegBlendRiseResponse
                    : kEnemyRemoteLegBlendFallResponse;
                const float alpha = std::clamp(
                    1.0f - std::exp(-response * smooth.tickSeconds),
                    0.0f,
                    1.0f);
                state.remoteLegBlendSpeed +=
                    (targetLegSpeed - state.remoteLegBlendSpeed) * alpha;
                if (targetLegSpeed <= 0.0f &&
                    state.remoteLegBlendSpeed <= kEnemyRemoteLegBlendStopSpeed)
                {
                    state.remoteLegBlendSpeed = 0.0f;
                }
            }

            if (visibleSpeed >= kEnemyRemoteLegBlendStartSpeed)
            {
                Vec3 travelDirection = visibleDelta;
                travelDirection.z = 0.0f;
                if (travelDirection.GetLengthSquared() > 0.000001f)
                {
                    travelDirection.Normalize();
                    Vec3 forward = appliedRotation.GetColumn1();
                    Vec3 right = appliedRotation.GetColumn0();
                    forward.z = 0.0f;
                    right.z = 0.0f;
                    if (forward.GetLengthSquared() > 0.000001f &&
                        right.GetLengthSquared() > 0.000001f)
                    {
                        forward.Normalize();
                        right.Normalize();
                        state.remoteLegBlendAngle = std::atan2(
                            travelDirection.Dot(right),
                            travelDirection.Dot(forward));
                    }
                }
            }

            std::string motionParamFailure;
            const bool motionParamsApplied = TrySetEnemyLegMotionParams(
                *entity,
                state.remoteLegBlendSpeed,
                state.remoteLegBlendAngle,
                smooth.tickSeconds,
                &motionParamFailure);
            if (motionParamsApplied)
            {
                state.remoteLegBlendOwned = true;
                ++state.remoteLegBlendApplies;
                ++m_enemyRemoteLegBlendApplies;
            }
            else
            {
                ++state.remoteLegBlendFailures;
                ++m_enemyRemoteLegBlendFailures;
            }

            if (nowSeconds < state.remoteLegBlendTraceAtSeconds ||
                nowSeconds - state.remoteLegBlendTraceAtSeconds >= 1.0f)
            {
                state.remoteLegBlendTraceAtSeconds = nowSeconds;
                AppendEnemySyncTrace(
                    "leg_mix",
                    "remote authority lower_body motion"
                    " net=" + std::to_string(state.netId) +
                        " entity=" + std::to_string(state.entityId) +
                        " visualSpeed=" + std::to_string(visibleSpeed) +
                        " blendSpeed=" + std::to_string(state.remoteLegBlendSpeed) +
                        " travelAngle=" + std::to_string(state.remoteLegBlendAngle) +
                        " applied=" + std::to_string(motionParamsApplied ? 1 : 0) +
                        (motionParamFailure.empty()
                            ? std::string()
                            : " failure=" + motionParamFailure) +
                        " positionWriter=authority_follower"
                        " actionWriter=native_local_combat");
            }
        }
        else if (state.remoteLegBlendOwned)
        {
            // Relinquish only the two parameters we owned. This is especially
            // important when a full-body dash, morph or hit reaction starts:
            // an old nonzero TravelSpeed must not leak into that authored
            // action. LocalOwner is never reached here because this loop exits
            // above before touching native authority animation.
            std::string motionParamFailure;
            const bool motionParamsReset = TrySetEnemyLegMotionParams(
                *entity,
                0.0f,
                0.0f,
                smooth.tickSeconds,
                &motionParamFailure);
            state.remoteLegBlendSpeed = 0.0f;
            state.remoteLegBlendAngle = 0.0f;
            state.remoteLegBlendOwned = false;
            if (motionParamsReset)
            {
                ++state.remoteLegBlendApplies;
                ++m_enemyRemoteLegBlendApplies;
            }
            else
            {
                ++state.remoteLegBlendFailures;
                ++m_enemyRemoteLegBlendFailures;
            }
            AppendEnemySyncTrace(
                "leg_mix",
                "released remote authority lower_body motion"
                " net=" + std::to_string(state.netId) +
                    " entity=" + std::to_string(state.entityId) +
                    " mode=" + CoopEnemyControlPolicy::ModeName(controlDecision.mode) +
                    " blockedFlags=" + std::to_string(
                        (state.remoteLocomotionFlags | state.remoteMannequinFlags) &
                            legBlendBlockingFlags) +
                    " applied=" + std::to_string(motionParamsReset ? 1 : 0) +
                    (motionParamFailure.empty()
                        ? std::string()
                        : " failure=" + motionParamFailure));
        }

        ScopedRemoteEnemyTransformWrite transformWrite(m_remoteEnemyTransformWriteDepth);
        if (smooth.moved && (smooth.rotated || localFacingMixed))
            entity->SetPosRotScale(appliedPosition, appliedRotation, entity->GetScale(), 0);
        else if (smooth.moved)
            ApplyEnemyPositionOnly(*entity, appliedPosition);
        else if (smooth.rotated || localFacingMixed)
            entity->SetRotation(appliedRotation, 0);
        else
            continue;

        ApplyEntityPhysicsVelocity(*entity, Vec3(ZERO));
        ++m_enemyTransformSmoothTicks;
        if ((m_enemyTransformSmoothTicks & 0x7fu) == 1u)
        {
            m_lastEnemyLocomotionEvent =
                "remote_smooth_tick"
                " net=" + std::to_string(state.netId) +
                " entity=" + std::to_string(state.entityId) +
                " pos=" + std::to_string(smooth.moved ? 1 : 0) +
                " rot=" + std::to_string(smooth.rotated ? 1 : 0) +
                " localFacingMix=" + std::to_string(applyReadOnlyLocalFacingMix ? 1 : 0) +
                " localFacingStep=" + std::to_string(localFacingMixed ? 1 : 0) +
                " step=" + std::to_string(smooth.positionStep) +
                " speed=" + std::to_string(smooth.positionSpeed) +
                " rotAlpha=" + std::to_string(smooth.rotationAlpha) +
                " rotSpeed=" + std::to_string(smooth.rotationSpeed) +
                " tick=" + std::to_string(smooth.tickSeconds) +
                (state.remoteVisualMotionFlags != 0
                    ? " visualMove=" + std::to_string(state.remoteVisualMotionFlags) +
                        " visualHold=" + std::to_string(state.remoteVisualMotionSeconds)
                    : std::string()) +
                (smooth.burst ? " burst=1" : "") +
                (smooth.hardPosition ? " hard=1" : "") +
                (smooth.hardRotation ? " rotHard=1" : "") +
                " count=" + std::to_string(m_enemyTransformSmoothTicks);
            AppendEnemySyncTrace("pose", m_lastEnemyLocomotionEvent);
        }
    }
}

void ModMain::TickClientEnemyAuthorityClaims(float frameTime)
{
    if (!m_enemyAttentionAuthoritySyncEnabled ||
        !m_enemyLocomotionSyncEnabled ||
        m_syncTestMimicSpawn ||
        m_networkMode != CoopNetworkMode::Client ||
        IsClientAreaAuthorityActive() ||
        m_socket == kInvalidNetworkSocket ||
        !m_hasRemoteEndpoint ||
        !IsEnemyReplicationGameplayReady() ||
        !gEnv ||
        !gEnv->pEntitySystem ||
        !ArkPlayer::GetInstancePtr())
    {
        return;
    }

    m_enemyAuthorityTickAccumulator += std::max(0.0f, frameTime);
    if (m_enemyAuthorityTickAccumulator < kMimicStateTickSeconds)
        return;

    const float tickSeconds = m_enemyAuthorityTickAccumulator;
    m_enemyAuthorityTickAccumulator = 0.0f;

    for (auto& entry : m_enemyAuthorities)
    {
        EnemyAuthorityState& state = entry.second;
        ++state.clientClaimTickCount;
        if (state.entityId == INVALID_ENTITYID)
        {
            state.clientClaimDecision = 1;
            continue;
        }

        IEntity* entity = gEnv->pEntitySystem->GetEntity(state.entityId);
        if (!entity || !IsEnemyRuntimeControlCandidate(*entity))
        {
            state.clientClaimDecision = 2;
            continue;
        }

        ArkNpc* npc = EntityUtils::GetArkNpc(entity);
        if (!npc || npc->IsDead())
        {
            state.clientClaimDecision = 3;
            continue;
        }

        state.localRotationOverrideSeconds = std::max(
            0.0f,
            state.localRotationOverrideSeconds - tickSeconds);
        state.localNativeAttentionSeconds = std::max(
            0.0f,
            state.localNativeAttentionSeconds - tickSeconds);
        state.localAttentionAdvertisementSeconds += tickSeconds;
        if (state.remoteLocomotionAuthority)
        {
            state.remoteAuthoritySilentSeconds += tickSeconds;
            if (state.remoteAuthorityBlocked)
                state.remoteAuthorityBlockedSeconds = std::min(
                    state.remoteAuthorityBlockedSeconds + tickSeconds,
                    kEnemyRemoteAuthorityBlockedStealSeconds);
            else
                state.remoteAuthorityBlockedSeconds = 0.0f;

            // Silence is not a lease grant. The Host echoes an accepted owner
            // token/epoch; until that explicit grant arrives, this Client must
            // keep remote simulation blocked and continue reasserting its claim.
        }

        const bool localAuthorityBlocked =
            m_localPlayerDowned || IsLocalPlayerAuthorityBlockedByModalState();
        uint8_t localAttentionLevel = localAuthorityBlocked
            ? CoopEnemyAuthorityPolicy::kUnknownAttention
            : LocalPlayerEnemyAttentionLevelForCoop(*entity);
        state.localAttentionLevel = localAttentionLevel;
        if (localAttentionLevel > CoopEnemyAuthorityPolicy::kUnknownAttention)
        {
            state.localAttentionLostSeconds = 0.0f;
            state.localRotationOverrideSeconds = kEnemyLocalRotationOverrideGraceSeconds;
        }

        const uint64_t localAccountToken = GetLocalAccountToken();
        const bool localOwnsLease =
            state.authorityOwnerAccountToken == localAccountToken &&
            !state.remoteLocomotionAuthority;
        state.localAttentionClaimed = localOwnsLease &&
            localAttentionLevel > CoopEnemyAuthorityPolicy::kUnknownAttention;
        const bool levelChanged =
            state.lastAdvertisedAttentionLevel != localAttentionLevel;
        const bool heartbeatDue =
            state.localAttentionAdvertisementSeconds >= kEnemyAttentionCandidateHeartbeatSeconds;
        if (!levelChanged && !heartbeatDue)
        {
            state.clientClaimDecision = localAttentionLevel > 0 ? 5 : 9;
            continue;
        }

        uint32_t sourceFlags = CoopProtocol::kEnemyStateSourceFlagAuthoritySnapshot;
        const char* failurePrefix = "enemy authority candidate snapshot send failed";
        if (localAttentionLevel > CoopEnemyAuthorityPolicy::kUnknownAttention)
        {
            sourceFlags |= CoopProtocol::kEnemyStateSourceFlagAuthorityClaim;
            failurePrefix = levelChanged
                ? "enemy attention rank claim send failed"
                : "enemy attention rank heartbeat send failed";
            state.clientClaimDecision = levelChanged ? 6 : 7;
        }
        else if (state.lastAdvertisedAttentionLevel > CoopEnemyAuthorityPolicy::kUnknownAttention ||
            (localOwnsLease &&
                (!state.hasAdvertisedAttentionAuthorityEpoch ||
                    state.lastAdvertisedAttentionAuthorityEpoch != state.authorityEpoch)))
        {
            sourceFlags |= CoopProtocol::kEnemyStateSourceFlagAuthorityRelease;
            failurePrefix = localAuthorityBlocked
                ? "enemy authority blocked release send failed"
                : "enemy attention rank release send failed";
            state.clientClaimDecision = 11;
        }
        else
        {
            state.clientClaimDecision = localAuthorityBlocked ? 8 : 9;
            state.localAttentionAdvertisementSeconds = 0.0f;
            continue;
        }

        if (SendClientEnemyAuthorityStateNow(state, sourceFlags, failurePrefix))
        {
            state.lastAdvertisedAttentionLevel = localAttentionLevel;
            state.lastAdvertisedAttentionAuthorityEpoch = state.authorityEpoch;
            state.hasAdvertisedAttentionAuthorityEpoch = true;
            state.localAttentionAdvertisementSeconds = 0.0f;
            if (localAttentionLevel == CoopEnemyAuthorityPolicy::kUnknownAttention)
                state.localAttentionClaimed = false;
            m_lastEnemyAuthorityEvent =
                "client attention candidate net=" + std::to_string(state.netId) +
                " level=" + std::to_string(localAttentionLevel) +
                " owner=" + std::to_string(state.authorityOwnerAccountToken) +
                " localOwner=" + std::to_string(localOwnsLease ? 1 : 0);
        }
    }
}

bool ModMain::UpdateEnemyAttentionCandidate(
    EnemyAuthorityState& state,
    uint64_t accountToken,
    uint8_t attentionLevel,
    bool blocked,
    uint64_t targetAccountToken,
    uint32_t sequence,
    bool enforceSequence)
{
    if (accountToken == 0 || attentionLevel > CoopEnemyAuthorityPolicy::kKnownAttention)
        return false;

    EnemyAuthorityState::AttentionCandidateState& candidate =
        state.attentionCandidates[accountToken];
    if (enforceSequence && sequence != 0 &&
        CoopSerialSequence::IsStaleOrDuplicate(sequence, candidate.lastSequence))
    {
        return false;
    }
    if (enforceSequence && sequence != 0)
        CoopSerialSequence::Observe(sequence, candidate.lastSequence);

    const uint8_t effectiveLevel = blocked
        ? CoopEnemyAuthorityPolicy::kUnknownAttention
        : attentionLevel;
    const bool enteredOrChangedLevel =
        effectiveLevel > CoopEnemyAuthorityPolicy::kUnknownAttention &&
        (candidate.attentionLevel != effectiveLevel || candidate.blocked);
    if (enteredOrChangedLevel)
    {
        candidate.firstAtLevelOrder = ++state.attentionClaimOrderCounter;
        if (candidate.firstAtLevelOrder == 0)
            candidate.firstAtLevelOrder = ++state.attentionClaimOrderCounter;
    }
    else if (effectiveLevel == CoopEnemyAuthorityPolicy::kUnknownAttention)
    {
        candidate.firstAtLevelOrder = 0;
    }

    candidate.attentionLevel = effectiveLevel;
    candidate.targetAccountToken = effectiveLevel > CoopEnemyAuthorityPolicy::kUnknownAttention
        ? (targetAccountToken != 0 ? targetAccountToken : accountToken)
        : 0;
    candidate.silentSeconds = 0.0f;
    candidate.blocked = blocked;
    return true;
}

CoopEnemyAuthorityPolicy::Decision ModMain::SelectEnemyAttentionAuthority(
    const EnemyAuthorityState& state,
    uint64_t areaAuthorityAccountToken) const
{
    std::vector<CoopEnemyAuthorityPolicy::Candidate> candidates;
    candidates.reserve(state.attentionCandidates.size());
    for (const auto& entry : state.attentionCandidates)
    {
        candidates.push_back({
            entry.first,
            entry.second.attentionLevel,
            entry.second.firstAtLevelOrder,
            entry.second.blocked});
    }
    return CoopEnemyAuthorityPolicy::Select(areaAuthorityAccountToken, candidates);
}

bool ModMain::ApplyEnemyAttentionAuthorityDecisionOnAreaAuthority(
    EnemyAuthorityState& state,
    const CoopEnemyAuthorityPolicy::Decision& decision,
    const CoopProtocol::TestMimicStatePacket* sourcePacket,
    uint64_t sourceAccountToken,
    const char* reason)
{
    const bool localAreaAuthority =
        m_networkMode == CoopNetworkMode::Host || IsClientAreaAuthorityActive();
    if (!localAreaAuthority || decision.ownerAccountToken == 0)
        return false;

    const uint64_t areaAuthorityAccountToken = GetLocalAccountToken();
    const uint64_t previousOwner = state.authorityOwnerAccountToken != 0
        ? state.authorityOwnerAccountToken
        : areaAuthorityAccountToken;
    const uint8_t previousLevel = state.authorityAttentionLevel;
    const bool ownerChanged = previousOwner != decision.ownerAccountToken;
    const bool levelChanged = previousLevel != decision.attentionLevel;

    const auto winnerIt = state.attentionCandidates.find(decision.ownerAccountToken);
    const bool winnerBlocked = winnerIt != state.attentionCandidates.end() && winnerIt->second.blocked;
    const uint64_t winnerTarget = winnerIt != state.attentionCandidates.end()
        ? winnerIt->second.targetAccountToken
        : 0;

    state.authorityOwnerAccountToken = decision.ownerAccountToken;
    state.authorityAttentionLevel = decision.attentionLevel;
    state.remoteLocomotionAuthority =
        decision.ownerAccountToken != areaAuthorityAccountToken;
    state.remoteAuthorityHasAttention =
        state.remoteLocomotionAuthority &&
        decision.attentionLevel > CoopEnemyAuthorityPolicy::kUnknownAttention;
    state.remoteAuthorityBlocked = state.remoteLocomotionAuthority && winnerBlocked;
    state.remoteTargetAccountToken = state.remoteAuthorityHasAttention
        ? (winnerTarget != 0 ? winnerTarget : decision.ownerAccountToken)
        : 0;
    state.localAttentionClaimed =
        !state.remoteLocomotionAuthority &&
        decision.attentionLevel > CoopEnemyAuthorityPolicy::kUnknownAttention;
    state.localAttentionClaimedSeconds = state.localAttentionClaimed
        ? state.localAttentionClaimedSeconds
        : 0.0f;
    state.localAttentionPendingSeconds = 0.0f;
    state.localAttentionLostSeconds = 0.0f;
    state.localAuthorityBlockedSeconds = 0.0f;
    state.remoteAuthoritySilentSeconds = 0.0f;
    state.remoteAuthorityBlockedSeconds = 0.0f;

    if (!ownerChanged)
        return levelChanged;

    ResetEnemySemanticReplicationState(state);
    state.authorityEpoch = CoopSerialSequence::Next(std::max<uint32_t>(1, state.authorityEpoch));
    state.remoteAuthorityLastSequence = winnerIt != state.attentionCandidates.end()
        ? winnerIt->second.lastSequence
        : 0;
    state.hasLastTransmittedStatePacket = false;

    IEntity* entity = gEnv && gEnv->pEntitySystem && state.entityId != INVALID_ENTITYID
        ? gEnv->pEntitySystem->GetEntity(state.entityId)
        : nullptr;
    if (entity)
    {
        if (!state.remoteLocomotionAuthority)
        {
            ClearRemoteEnemyMovementDesire(state.netId, "attention arbitration local handoff");
            SetRemoteEnemyMirrorPhysics(*entity, false, "attention arbitration local handoff");
            ApplyEntityPhysicsVelocity(*entity, Vec3(ZERO));
            RestoreLocalEnemyVanillaAuthority(
                state,
                *entity,
                reason && reason[0] ? reason : "attention arbitration local handoff");
        }
        else
        {
            InterruptEnemyAbilityForAuthorityTransition(
                state,
                *entity,
                reason && reason[0] ? reason : "attention arbitration remote handoff");
            CoopProtocol::TestMimicStatePacket handoff = {};
            bool haveHandoff = false;
            if (sourcePacket && sourceAccountToken == decision.ownerAccountToken)
            {
                handoff = *sourcePacket;
                haveHandoff = true;
            }
            else
            {
                haveHandoff = BuildEnemyStatePacket(state, handoff);
            }
            if (haveHandoff)
            {
                handoff.authorityOwnerAccountToken = decision.ownerAccountToken;
                handoff.authorityEpoch = state.authorityEpoch;
                handoff.authorityAttentionLevel = decision.attentionLevel;
                handoff.sourceFlags |=
                    CoopProtocol::kEnemyStateSourceFlagAuthorityClaim |
                    CoopProtocol::kEnemyStateSourceFlagAuthoritySnapshot;
                if (decision.attentionLevel > CoopEnemyAuthorityPolicy::kUnknownAttention)
                    handoff.sourceFlags |= CoopProtocol::kEnemyStateSourceFlagAuthorityHasAttention;
                else
                    handoff.sourceFlags &= ~CoopProtocol::kEnemyStateSourceFlagAuthorityHasAttention;
                handoff.targetAccountToken = state.remoteTargetAccountToken;
                ApplyEnemyLocomotionStateToLocal(handoff);
            }
        }
    }

    CoopProtocol::TestMimicStatePacket grant = {};
    if (BuildEnemyStatePacket(state, grant))
    {
        grant.authorityOwnerAccountToken = decision.ownerAccountToken;
        grant.authorityEpoch = state.authorityEpoch;
        grant.authorityAttentionLevel = decision.attentionLevel;
        grant.sourceFlags =
            CoopProtocol::kEnemyStateSourceFlagAuthorityClaim |
            CoopProtocol::kEnemyStateSourceFlagAuthoritySnapshot;
        if (decision.attentionLevel > CoopEnemyAuthorityPolicy::kUnknownAttention)
            grant.sourceFlags |= CoopProtocol::kEnemyStateSourceFlagAuthorityHasAttention;
        if (state.remoteAuthorityBlocked)
            grant.sourceFlags |= CoopProtocol::kEnemyStateSourceFlagAuthorityBlocked;
        grant.targetAccountToken = state.remoteLocomotionAuthority
            ? state.remoteTargetAccountToken
            : (decision.attentionLevel > CoopEnemyAuthorityPolicy::kUnknownAttention
                ? areaAuthorityAccountToken
                : 0);
        SendTestMimicStateTo(
            grant,
            m_remoteAddress,
            m_remotePort,
            "enemy attention authority grant relay failed");
    }

    ++m_enemyAuthorityClaimsAccepted;
    m_lastEnemyAuthorityEvent =
        "attention authority handoff net=" + std::to_string(state.netId) +
        " from=" + std::to_string(previousOwner) +
        " to=" + std::to_string(decision.ownerAccountToken) +
        " level=" + std::to_string(decision.attentionLevel) +
        " order=" + std::to_string(decision.firstAtLevelOrder) +
        " fallback=" + std::to_string(decision.fallback ? 1 : 0) +
        " epoch=" + std::to_string(state.authorityEpoch) +
        " reason=" + (reason && reason[0] ? reason : "-");
    AppendEnemySyncTrace("authority", m_lastEnemyAuthorityEvent);
    return true;
}

void ModMain::HandleRemoteEnemyAuthorityStateOnAreaAuthority(const CoopProtocol::TestMimicStatePacket& packet)
{
    const bool clientAreaAuthority = IsClientAreaAuthorityActive();
    if (!m_enemyAttentionAuthoritySyncEnabled ||
        !m_enemyLocomotionSyncEnabled ||
        m_syncTestMimicSpawn ||
        (m_networkMode != CoopNetworkMode::Host && !clientAreaAuthority) ||
        !IsGameReady())
    {
        return;
    }

    if (clientAreaAuthority &&
        packet.authorityOwnerAccountToken != GetLocalAccountToken())
    {
        ++m_enemyAuthorityCandidateReceives;
    }

    const uint64_t enemyNetId = packet.enemyNetId ? packet.enemyNetId : kTestMimicNetId;
    const uint64_t sourceAccountToken = m_networkMode == CoopNetworkMode::Host
        ? m_activePacketSourceAccountToken
        : packet.authorityOwnerAccountToken;
    const auto sourcePeerIt = m_remotePeers.find(sourceAccountToken);
    if (enemyNetId == 0 ||
        sourceAccountToken == 0 ||
        sourcePeerIt == m_remotePeers.end() ||
        !sourcePeerIt->second.sessionReady ||
        !IsKnownSameLevel(m_localLevelName, sourcePeerIt->second.levelName) ||
        packet.sequence == 0 ||
        packet.authorityOwnerAccountToken != sourceAccountToken ||
        packet.authorityEpoch == 0 ||
        packet.authorityAttentionLevel > CoopEnemyAuthorityPolicy::kKnownAttention)
    {
        ++m_enemyAuthorityClaimsDenied;
        m_lastEnemyAuthorityEvent =
            "AreaAuthority denied unauthenticated enemy candidate net=" + std::to_string(enemyNetId) +
            " source=" + std::to_string(sourceAccountToken) +
            " owner=" + std::to_string(packet.authorityOwnerAccountToken) +
            " level=" + std::to_string(packet.authorityAttentionLevel);
        return;
    }

    EnemyAuthorityState* state = FindEnemyAuthorityByNetId(enemyNetId);
    // Remote packets can only address identities established by the reliable
    // roster. They never discover entities by scanning the local world.
    if (!state || state->entityId == INVALID_ENTITYID || !gEnv || !gEnv->pEntitySystem)
    {
        ++m_enemyAuthorityClaimsDenied;
        m_lastEnemyAuthorityEvent =
            "AreaAuthority denied missing enemy net=" + std::to_string(enemyNetId);
        return;
    }

    const uint64_t areaAuthorityAccountToken = GetLocalAccountToken();
    if (state->authorityOwnerAccountToken == 0)
        state->authorityOwnerAccountToken = areaAuthorityAccountToken;
    if (state->authorityEpoch == 0)
        state->authorityEpoch = 1;

    const bool claim = (packet.sourceFlags & CoopProtocol::kEnemyStateSourceFlagAuthorityClaim) != 0;
    const bool release = (packet.sourceFlags & CoopProtocol::kEnemyStateSourceFlagAuthorityRelease) != 0;
    const bool snapshot = (packet.sourceFlags & CoopProtocol::kEnemyStateSourceFlagAuthoritySnapshot) != 0;
    const bool remoteBlocked = (packet.sourceFlags & CoopProtocol::kEnemyStateSourceFlagAuthorityBlocked) != 0;
    const bool reportsAttention =
        (packet.sourceFlags & CoopProtocol::kEnemyStateSourceFlagAuthorityHasAttention) != 0;
    if ((!claim && !release && !snapshot) ||
        (reportsAttention !=
            (packet.authorityAttentionLevel > CoopEnemyAuthorityPolicy::kUnknownAttention)) ||
        (claim && packet.authorityAttentionLevel == CoopEnemyAuthorityPolicy::kUnknownAttention) ||
        (release && packet.authorityAttentionLevel != CoopEnemyAuthorityPolicy::kUnknownAttention) ||
        (reportsAttention && packet.targetAccountToken != sourceAccountToken))
    {
        ++m_enemyAuthorityClaimsDenied;
        m_lastEnemyAuthorityEvent =
            "AreaAuthority denied inconsistent enemy candidate net=" + std::to_string(enemyNetId) +
            " source=" + std::to_string(sourceAccountToken) +
            " level=" + std::to_string(packet.authorityAttentionLevel) +
            " flags=" + std::to_string(packet.sourceFlags) +
            " target=" + std::to_string(packet.targetAccountToken);
        return;
    }
    if (packet.authorityEpoch != state->authorityEpoch)
    {
        ++m_enemyAuthorityClaimsDenied;
        m_lastEnemyAuthorityEvent =
            "AreaAuthority denied stale enemy candidate epoch net=" + std::to_string(enemyNetId) +
            " source=" + std::to_string(sourceAccountToken) +
            " epoch=" + std::to_string(packet.authorityEpoch) +
            "/" + std::to_string(state->authorityEpoch);
        return;
    }

    IEntity* entity = gEnv->pEntitySystem->GetEntity(state->entityId);
    if (!entity)
        return;

    const uint8_t remoteAttentionLevel = release || remoteBlocked
        ? CoopEnemyAuthorityPolicy::kUnknownAttention
        : packet.authorityAttentionLevel;
    if (!UpdateEnemyAttentionCandidate(
            *state,
            sourceAccountToken,
            remoteAttentionLevel,
            remoteBlocked,
            packet.targetAccountToken,
            packet.sequence,
            true))
    {
        ++m_enemyAuthorityClaimsDenied;
        m_lastEnemyAuthorityEvent =
            "host ignored stale enemy candidate net=" + std::to_string(enemyNetId) +
            " source=" + std::to_string(sourceAccountToken) +
            " seq=" + std::to_string(packet.sequence);
        return;
    }

    // The AreaAuthority player has no tie advantage. Its periodic sample may
    // already carry an earlier first-at-level order; otherwise the candidate
    // packet that just arrived is, by definition, the first observed claim.
    const bool localBlocked =
        m_localPlayerDowned || IsLocalPlayerAuthorityBlockedByModalState();
    const uint8_t localAttentionLevel = localBlocked
        ? CoopEnemyAuthorityPolicy::kUnknownAttention
        : LocalPlayerEnemyAttentionLevelForCoop(*entity);
    state->localAttentionLevel = localAttentionLevel;
    UpdateEnemyAttentionCandidate(
        *state,
        areaAuthorityAccountToken,
        localAttentionLevel,
        localBlocked,
        localAttentionLevel > CoopEnemyAuthorityPolicy::kUnknownAttention
            ? areaAuthorityAccountToken
            : 0,
        0,
        false);

    const uint64_t previousOwner = state->authorityOwnerAccountToken;
    const CoopEnemyAuthorityPolicy::Decision decision =
        SelectEnemyAttentionAuthority(*state, areaAuthorityAccountToken);
    const bool ownerChanged = decision.ownerAccountToken != previousOwner;
    ApplyEnemyAttentionAuthorityDecisionOnAreaAuthority(
        *state,
        decision,
        &packet,
        sourceAccountToken,
        "received attention candidate");

    state = FindEnemyAuthorityByNetId(enemyNetId);
    if (!state || decision.ownerAccountToken != sourceAccountToken || release)
        return;

    CoopProtocol::TestMimicStatePacket acceptedPacket = packet;
    acceptedPacket.authorityOwnerAccountToken = sourceAccountToken;
    acceptedPacket.authorityEpoch = state->authorityEpoch;
    acceptedPacket.authorityAttentionLevel = decision.attentionLevel;
    acceptedPacket.targetAccountToken = decision.attentionLevel > 0
        ? sourceAccountToken
        : 0;
    if (!ownerChanged)
        ApplyEnemyLocomotionStateToLocal(acceptedPacket);

    state = FindEnemyAuthorityByNetId(enemyNetId);
    if (!state)
        return;
    state->authorityOwnerAccountToken = sourceAccountToken;
    state->authorityAttentionLevel = decision.attentionLevel;
    state->remoteAuthorityHasAttention = decision.attentionLevel > 0;
    state->remoteTargetAccountToken = acceptedPacket.targetAccountToken;
    state->remoteAuthoritySilentSeconds = 0.0f;
    const auto ownerCandidateIt = state->attentionCandidates.find(sourceAccountToken);
    state->remoteAuthorityLastSequence = ownerCandidateIt != state->attentionCandidates.end()
        ? ownerCandidateIt->second.lastSequence
        : packet.sequence;
    if ((acceptedPacket.flags & CoopProtocol::kTestMimicStateFlagDead) != 0)
    {
        if ((state->rosterFlags & CoopProtocol::kEnemyRosterFlagAlive) != 0)
        {
            state->rosterFlags &= ~CoopProtocol::kEnemyRosterFlagAlive;
            ++state->rosterVersion;
            state->rosterAnnouncedVersion = 0;
        }
        EnsureEnemyRosterAnnounced(*state, "remote authority death roster failed");
    }

    SendTestMimicStateTo(
        acceptedPacket,
        m_remoteAddress,
        m_remotePort,
        "enemy attention authority owner snapshot relay failed",
        ownerChanged ? 0 : sourceAccountToken);
    ++m_enemyAuthorityRemoteApplies;
    m_lastEnemyAuthorityEvent =
        "AreaAuthority relayed attention owner net=" + std::to_string(enemyNetId) +
        " owner=" + std::to_string(sourceAccountToken) +
        " level=" + std::to_string(decision.attentionLevel) +
        " epoch=" + std::to_string(state->authorityEpoch);
}

bool ModMain::SetEntityHealthFromAuthority(EntityId entityId, float health, bool takingDamage, bool killWhenZero)
{
    if (!gEnv || !gEnv->pEntitySystem || entityId == INVALID_ENTITYID)
        return false;

    IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId);
    if (!entity)
        return false;

    ArkHealthExtension* healthExtension = ArkHealthExtension::GetExtension(entityId);
    if (!healthExtension)
        return false;

    float clampedHealth = std::max(0.0f, health);
    if (healthExtension->m_maxHealth > 0.0f)
        clampedHealth = std::min(clampedHealth, healthExtension->m_maxHealth);

    if (ArkNpc* npc = EntityUtils::GetArkNpc(entity))
    {
        const bool wasDead = npc->IsDead();
        const bool nativeDeathFinalized = wasDead &&
            !npc->m_aiTreeEnabled.IsUnanimous() &&
            !npc->m_abilitiesEnabled.IsUnanimous() &&
            !npc->m_attentiveSubjectEnabled.IsUnanimous() &&
            !npc->m_attentionObjectEnabled.IsUnanimous();
        const bool needsNativeKill = killWhenZero &&
            clampedHealth <= 0.0f &&
            (!wasDead || !nativeDeathFinalized);

        healthExtension->m_health = clampedHealth;
        npc->OnHealthChanged(clampedHealth, takingDamage);
        // Do not re-check IsDead here: the health write itself makes that
        // predicate true, which previously skipped OnKill and left observers
        // with a zero-health NPC whose AI and visible body were still alive.
        if (needsNativeKill)
            npc->OnKill(false);
    }
    else
    {
        healthExtension->m_health = clampedHealth;
    }

    return true;
}

void ModMain::ApplyEntityPhysicsVelocity(IEntity& entity, const Vec3& velocity) const
{
    IPhysicalEntity* physics = entity.GetPhysics();
    if (!physics)
        return;

    pe_action_set_velocity action;
    action.v = velocity;
    action.w = Vec3(ZERO);
    physics->Action(&action);
}

void ModMain::SpawnMimicNearProxy()
{
    if (!IsGameReady())
    {
        LogCoop("Spawn mimic ignored: game is not ready");
        return;
    }

    IEntity* anchorEntity = nullptr;
    if (m_networkMode == CoopNetworkMode::Client)
        anchorEntity = ArkPlayer::GetInstance().GetEntity();
    else
        anchorEntity = GetProxyEntity();

    if (!anchorEntity)
        anchorEntity = ArkPlayer::GetInstance().GetEntity();

    Quat rot = anchorEntity->GetRotation();
    Vec3 pos = anchorEntity->GetWorldPos() + rot * Vec3(0.0f, kMimicForwardOffsetMeters, 0.0f);

    if (m_syncTestMimicSpawn && IsEnemyReplicationGameplayReady() && m_socket != kInvalidNetworkSocket && m_hasRemoteEndpoint && m_networkMode == CoopNetworkMode::Client)
    {
        CoopEvents::Context routeContext = {};
        routeContext.kind = CoopEvents::Kind::EnemySpawnRequest;
        routeContext.domain = CoopEvents::AuthorityDomain::AreaOwned;
        routeContext.sourceRole = CoopEvents::EntityRole::LocalPlayer;
        routeContext.targetRole = CoopEvents::EntityRole::HostEnemyAuthority;
        routeContext.localMode = m_networkMode;
        routeContext.sourceEntityId = ArkPlayer::GetInstance().GetEntity()->GetId();
        routeContext.sameLevel = true;
        routeContext.localAuthority = false;

        const CoopEvents::Route route = RouteCoopEvent(routeContext);
        if (!route.shouldEmitNetwork)
            return;

        CoopProtocol::TestMimicSpawnPacket packet = {};
        if (BuildTestMimicSpawnPacket(packet, pos, rot, CoopProtocol::kTestMimicSpawnFlagRequest))
        {
            SendTestMimicSpawnTo(packet, m_remoteAddress, m_remotePort, "test mimic spawn request failed");
            m_networkStatus = "sent test mimic spawn request";
        }
        return;
    }

    LogCoop("Spawning mimic");
    IEntity* entity = SpawnMimicAt(pos, rot, "CoopProxyTestMimic", false);
    LogCoop(entity ? "Mimic spawned" : "Spawn mimic failed");

    if (entity && m_syncTestMimicSpawn && IsEnemyReplicationGameplayReady() && m_socket != kInvalidNetworkSocket && m_hasRemoteEndpoint)
    {
        CoopEvents::Context routeContext = {};
        routeContext.kind = CoopEvents::Kind::EnemySpawnCommit;
        routeContext.domain = CoopEvents::AuthorityDomain::AreaOwned;
        routeContext.sourceRole = CoopEvents::EntityRole::HostEnemyAuthority;
        routeContext.targetRole = CoopEvents::EntityRole::EnemyPuppet;
        routeContext.localMode = m_networkMode;
        routeContext.sourceEntityId = m_mimicEntityId;
        routeContext.sameLevel = true;
        routeContext.localAuthority = true;
        routeContext.irreversible = true;

        const CoopEvents::Route route = RouteCoopEvent(routeContext);
        if (!route.shouldEmitNetwork)
            return;

        CoopProtocol::TestMimicSpawnPacket packet = {};
        if (BuildTestMimicSpawnPacket(packet, pos, rot, CoopProtocol::kTestMimicSpawnFlagAuthority))
            SendTestMimicSpawnTo(packet, m_remoteAddress, m_remotePort, "test mimic spawn send failed");
        SendAuthoritativeMimicStateNow("test mimic immediate state send failed");
    }
}

void ModMain::RemoveSpawnedEntities()
{
    if (m_saveLoadGuardActive)
    {
        ResetRuntimeWorldRefsForLoad("remove spawned during load guard");
        return;
    }

    const bool proxyDestroySafe =
        m_proxyEntityId == INVALID_ENTITYID ||
        IsProxyNativeDestroySafe("remove spawned entities");

    bool keepSuspendedProxy = false;
    if (gEnv && gEnv->pEntitySystem)
    {
        std::vector<EntityId> proxyIds;
        AddRemoteProxyEntityIds(proxyIds);
        if (m_proxyEntityId != INVALID_ENTITYID)
        {
            if (proxyDestroySafe)
            {
                PrepareCoopEntityForRemoval(m_proxyEntityId, true, false, "remove spawned entities");
                RemoveCoopEntityGuarded(m_proxyEntityId, true, "remove spawned entities");
            }
            else
            {
                SuspendProxyForArk(CoopProxyLifecycleState::SuspendedTransition, "remove spawned deferred");
                keepSuspendedProxy = true;
            }
        }
        for (EntityId proxyId : proxyIds)
        {
            if (proxyId == m_proxyEntityId)
                continue;
            PrepareCoopEntityForRemoval(proxyId, true, false, "remove spawned entities additional proxy");
            if (proxyDestroySafe)
                RemoveCoopEntityGuarded(proxyId, true, "remove spawned entities additional proxy");
            else
                keepSuspendedProxy = true;
        }
        if (m_animationTestProxyEntityId != INVALID_ENTITYID)
        {
            PrepareCoopEntityForRemoval(m_animationTestProxyEntityId, false, false, "remove spawned entities anim proxy");
            RemoveCoopEntityGuarded(m_animationTestProxyEntityId, true, "remove spawned entities anim proxy");
        }
        if (m_mimicEntityId != INVALID_ENTITYID)
        {
            PrepareCoopEntityForRemoval(m_mimicEntityId, false, m_mimicIsPuppet, "remove spawned entities mimic");
            RemoveCoopEntityGuarded(m_mimicEntityId, true, "remove spawned entities mimic");
        }
        for (const EntityId entityId : m_debugSpawnedEnemyEntityIds)
        {
            if (entityId != INVALID_ENTITYID && entityId != m_mimicEntityId)
            {
                PrepareCoopEntityForRemoval(entityId, false, false, "remove spawned entities debug enemy quarantine");
            }
        }
        for (const auto& entry : m_enemyPuppets)
        {
            if (entry.second.entityId != INVALID_ENTITYID && entry.second.entityId != m_mimicEntityId)
            {
                PrepareCoopEntityForRemoval(entry.second.entityId, false, true, "remove spawned entities puppet");
                RemoveCoopEntityGuarded(entry.second.entityId, true, "remove spawned entities puppet");
            }
        }

        std::vector<EntityId> looseSpawnedEnemyIds;
        if (IEntityIt* iterator = gEnv->pEntitySystem->GetEntityIterator())
        {
            iterator->MoveFirst();
            while (!iterator->IsEnd())
            {
                IEntity* entity = iterator->Next();
                if (!entity)
                    continue;

                const EntityId entityId = entity->GetId();
                if (entityId == INVALID_ENTITYID ||
                    IsRemoteProxyEntity(entityId) ||
                    entityId == m_animationTestProxyEntityId ||
                    entityId == m_mimicEntityId)
                {
                    continue;
                }

                bool loadedFromLevel = false;
                TryGuardedCall(
                    "remove spawned enemy IsLoadedFromLevelFile",
                    [entity]() { return entity->IsLoadedFromLevelFile(); },
                    loadedFromLevel,
                    nullptr);
                if (loadedFromLevel)
                    continue;

                const char* rawName = entity->GetName();
                const uint32_t flags = entity->GetFlags();
                const bool coopRuntimeEnemy =
                    IsCoopRuntimeEntityName(rawName) ||
                    (flags & kCoopRuntimeEntityFlags) == kCoopRuntimeEntityFlags;
                const bool spawnedRuntimeEnemy = IsSpawnedRuntimeEnemyName(rawName);
                if (!IsEnemyReplicationCandidate(*entity) && !spawnedRuntimeEnemy && !coopRuntimeEnemy)
                    continue;
                if (spawnedRuntimeEnemy || coopRuntimeEnemy)
                    looseSpawnedEnemyIds.push_back(entityId);
            }
            iterator->Release();
        }

        uint32_t looseQuarantined = 0;
        uint32_t looseQuarantineFailed = 0;
        for (const EntityId entityId : looseSpawnedEnemyIds)
        {
            if (entityId == INVALID_ENTITYID || entityId == m_mimicEntityId)
                continue;

            const bool clientPuppet = IsEnemyPuppetEntity(entityId);
            if (PrepareCoopEntityForRemoval(entityId, false, clientPuppet, "remove spawned entities loose enemy quarantine"))
                ++looseQuarantined;
            else
                ++looseQuarantineFailed;
        }
        if (looseQuarantined != 0 || looseQuarantineFailed != 0)
        {
            LogCoop(
                "remove spawned entities loose runtime enemies quarantined=" +
                std::to_string(looseQuarantined) +
                " failed=" + std::to_string(looseQuarantineFailed) +
                " nativeRemove=0");
        }
    }

    if (!keepSuspendedProxy)
    {
        m_proxyEntityId = INVALID_ENTITYID;
        ResetProxyLifecycleRuntimeState("remove spawned entities");
        SetProxyLifecycleState(CoopProxyLifecycleState::Destroyed, "remove spawned entities");
    }
    for (auto& entry : m_remotePeers)
    {
        entry.second.proxyEntityId = INVALID_ENTITYID;
        entry.second.poseAnimationClip.clear();
    }
    m_mimicEntityId = INVALID_ENTITYID;
    m_animationTestProxyEntityId = INVALID_ENTITYID;
    m_debugSpawnedEnemyEntityIds.clear();
    m_debugEnemyAttentionOverrideActive = false;
    m_debugEnemyAttentionOverrideLevel = CoopEnemyAuthorityPolicy::kUnknownAttention;
    m_debugEnemyAttentionOverrideNetId = 0;
    m_lastDebugEnemyAttentionEvent = "remove spawned entities";
    ClearAllRemoteEnemyMovementDesires("remove spawned entities");
    ResetRemoteEnemyMirrorPhysics("remove spawned entities");
    m_enemyPuppets.clear();
    m_enemyNetIdsByEntity.clear();
    m_enemyStableSpawnIdsByEntity.clear();
    m_enemyRaisedFromCorpseSourcesByEntity.clear();
    m_enemyEthericDoppelgangerSourcesByEntity.clear();
    m_enemyEthericDoppelgangerGenerationsByEntity.clear();
    m_enemyEthericDoppelgangerRequestsSentByEntity.clear();
    m_pendingEthericDoppelgangerInitFrames.clear();
    m_pendingRemoteCorpsePhantomResults.clear();
    m_pendingCorpsePhantomSpawnRequests.clear();
    m_enemyAuthorities.clear();
    m_enemyRosterByNetId.clear();
    m_pendingEnemyDeathCommits.clear();
    m_enemyDeathPresentations.clear();
    m_pendingEnemyDamageSignals.clear();
    m_enemyDeathPresentationLastSequences.clear();
    m_enemyLocomotionLastSequences.clear();
    m_networkConsumedCystoidNestNetIds.clear();
    m_networkConsumedCystoidNestEntityIds.clear();
    // Keep the session-local allocator monotone. EnemyRoster records are
    // reliable and clients may still retain a tombstone/record for an old
    // net id; reusing that id after debug/runtime cleanup makes the new
    // stable identity conflict with the prior record.
    m_lastEnemyLocomotionEvent = "remove spawned entities";
    m_enemyAuthorityTickAccumulator = 0.0f;
    m_lastEnemyAuthorityEvent = "remove spawned entities";
    m_proxyTickAccumulator = 0.0f;
    m_mimicStateTickAccumulator = 0.0f;
    m_followLocalPlayer = false;
    m_proxyWasConfigured = false;
    m_mimicIsPuppet = false;
    m_hadAuthoritativeMimic = false;
    m_sentMimicDeadState = false;
    m_remotePlayerDowned = false;
    m_mimicDeathCommitRepeatsRemaining = 0;
    m_mimicHealthAvailable = false;
    m_lastMimicHealth = 0.0f;
    m_lastMimicMaxHealth = 0.0f;
    m_lastMimicDamage = 0.0f;
    m_hasLastMimicAuthorityPos = false;
    m_clientAreaAuthorityActive = false;
    m_lastAreaAuthorityEvent = "cleared with spawned entities";
    ResetProxyHealthBaseline();
    LogCoop("Removed spawned entities");
}

void ModMain::HideSpawnedEntitiesForLevelMismatch(const char* reason)
{
    if (m_saveLoadGuardActive)
        return;

    SuspendProxyForArk(CoopProxyLifecycleState::SuspendedRemoteLevel, reason ? reason : "level mismatch");
    m_sessionGameplayReady = false;
    m_clientAreaAuthorityActive = false;

    std::vector<EntityId> entityIds;
    auto addEntityId = [&entityIds](EntityId entityId)
    {
        if (entityId == INVALID_ENTITYID)
            return;
        if (std::find(entityIds.begin(), entityIds.end(), entityId) == entityIds.end())
            entityIds.push_back(entityId);
    };

    addEntityId(m_mimicEntityId);
    AddRuntimeEnemyEntityIds(entityIds);

    uint32_t hidden = 0;
    if (gEnv && gEnv->pEntitySystem)
    {
        for (EntityId entityId : entityIds)
        {
            IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId);
            if (!entity)
                continue;

            MarkCoopRuntimeEntity(*entity, IsEnemyPuppetEntity(entityId));
            entity->Hide(true);
            entity->Invisible(true);
            ++hidden;
        }
    }

    m_lastAreaAuthorityEvent =
        std::string("hidden spawned entities for level mismatch") +
        " reason=" + (reason ? reason : "unknown") +
        " tracked=" + std::to_string(entityIds.size()) +
        " hidden=" + std::to_string(hidden) +
        " npcStateMutation=none";
    LogCoop(m_lastAreaAuthorityEvent);
}

const char* ModMain::GetProxyLifecycleStateName(CoopProxyLifecycleState state) const
{
    switch (state)
    {
    case CoopProxyLifecycleState::Empty:
        return "Empty";
    case CoopProxyLifecycleState::ActiveSameLevel:
        return "ActiveSameLevel";
    case CoopProxyLifecycleState::SuspendedRemoteLevel:
        return "SuspendedRemoteLevel";
    case CoopProxyLifecycleState::SuspendedTransition:
        return "SuspendedTransition";
    case CoopProxyLifecycleState::Destroyed:
        return "Destroyed";
    default:
        return "Unknown";
    }
}

void ModMain::SetProxyLifecycleState(CoopProxyLifecycleState state, const char* reason)
{
    if (m_proxyLifecycleState == state)
        return;

    m_proxyLifecycleState = state;
    ++m_proxyLifecycleSerial;
    m_lastProxyLifecycleEvent =
        "proxy lifecycle #" + std::to_string(m_proxyLifecycleSerial) +
        " state=" + GetProxyLifecycleStateName(state) +
        " reason=" + (reason ? reason : "unknown") +
        " entity=" + std::to_string(m_proxyEntityId);
    LogCoop(m_lastProxyLifecycleEvent);
}

bool ModMain::IsProxyNativeDestroySafe(const char* reason) const
{
    (void)reason;

    if (m_saveLoadGuardActive ||
        m_pendingPostLoadResync ||
        m_arkLevelTransitionLoadActive ||
        m_runtimeTransitionCleanupPrepared)
    {
        return false;
    }

    if (m_proxyLifecycleState == CoopProxyLifecycleState::SuspendedRemoteLevel ||
        m_proxyLifecycleState == CoopProxyLifecycleState::SuspendedTransition)
    {
        return false;
    }

    return gEnv && gEnv->pEntitySystem;
}

void ModMain::ResetProxyLifecycleRuntimeState(const char* reason)
{
    m_proxyLifecycleDisableSensesPushed = false;
    m_proxyLifecycleDisableVisiblePushed = false;
    m_proxyLifecycleDisableAudiblePushed = false;
    SetProxyLifecycleState(CoopProxyLifecycleState::Empty, reason);
}

void ModMain::SuspendProxyForArk(CoopProxyLifecycleState state, const char* reason)
{
    if (state != CoopProxyLifecycleState::SuspendedRemoteLevel &&
        state != CoopProxyLifecycleState::SuspendedTransition)
    {
        state = CoopProxyLifecycleState::SuspendedRemoteLevel;
    }

    m_followLocalPlayer = false;
    m_sessionGameplayReady = false;

    if (m_proxyEntityId == INVALID_ENTITYID || !gEnv || !gEnv->pEntitySystem)
    {
        SetProxyLifecycleState(CoopProxyLifecycleState::Empty, reason);
        return;
    }

    std::string guardReason;
    uint32_t failures = 0;
    uint32_t pushed = 0;
    uint32_t popped = 0;

    if (!TryGuardedVoidCall(
            "proxy lifecycle suspend unregister revive",
            [this]() { UnregisterProxyReviveInteraction(m_proxyEntityId); },
            &guardReason))
    {
        ++failures;
        m_proxyReviveInteractionEntityId = INVALID_ENTITYID;
        m_proxyReviveInteractionRegistered = false;
    }

    IEntity* entity = gEnv->pEntitySystem->GetEntity(m_proxyEntityId);
    if (!entity)
    {
        ResetProxyLifecycleRuntimeState(reason);
        return;
    }

    MarkCoopRuntimeEntity(*entity, false);
    if (!TryGuardedVoidCall(
            "proxy lifecycle suspend hide",
            [entity]()
            {
                entity->Hide(true);
                entity->Invisible(true);
            },
            &guardReason))
    {
        ++failures;
    }

    if (ArkNpc* npc = EntityUtils::GetArkNpc(entity))
    {
        auto pushOnce = [&](bool& flag, const char* label, auto&& call)
        {
            if (flag)
                return;
            std::string localGuard;
            if (TryGuardedVoidCall(label, std::forward<decltype(call)>(call), &localGuard))
            {
                flag = true;
                ++pushed;
            }
            else
            {
                ++failures;
                guardReason = localGuard;
            }
        };

        pushOnce(
            m_proxyLifecycleDisableSensesPushed,
            "proxy lifecycle suspend PushDisableSenses",
            [npc]() { npc->PushDisableSenses(); });
        pushOnce(
            m_proxyLifecycleDisableVisiblePushed,
            "proxy lifecycle suspend PushDisableVisible",
            [npc]() { npc->PushDisableVisible(); });
        pushOnce(
            m_proxyLifecycleDisableAudiblePushed,
            "proxy lifecycle suspend PushDisableAudible",
            [npc]() { npc->PushDisableAudible(); });

    }

    (void)popped;
    if (m_proxyLifecycleState != state)
    {
        m_proxyLifecycleState = state;
        ++m_proxyLifecycleSerial;
    }

    m_lastProxyLifecycleEvent =
        "proxy lifecycle #" + std::to_string(m_proxyLifecycleSerial) +
        " state=" + GetProxyLifecycleStateName(m_proxyLifecycleState) +
        " reason=" + (reason ? reason : "unknown") +
        " entity=" + std::to_string(m_proxyEntityId) +
        " pushed=" + std::to_string(pushed) +
        " failures=" + std::to_string(failures);
    if (!guardReason.empty())
        m_lastProxyLifecycleEvent += " lastGuard=" + guardReason;
    LogCoop(m_lastProxyLifecycleEvent);
}

void ModMain::ResumeProxyForSameLevel(const char* reason)
{
    if (m_proxyEntityId == INVALID_ENTITYID || !gEnv || !gEnv->pEntitySystem)
    {
        ResetProxyLifecycleRuntimeState(reason);
        return;
    }

    IEntity* entity = gEnv->pEntitySystem->GetEntity(m_proxyEntityId);
    if (!entity)
    {
        ResetProxyLifecycleRuntimeState(reason);
        return;
    }

    std::string guardReason;
    uint32_t failures = 0;
    uint32_t popped = 0;

    MarkCoopRuntimeEntity(*entity, false);
    if (!TryGuardedVoidCall(
            "proxy lifecycle resume show",
            [entity]()
            {
                entity->Hide(false);
                entity->Invisible(false);
            },
            &guardReason))
    {
        ++failures;
    }

    if (ArkNpc* npc = EntityUtils::GetArkNpc(entity))
    {
        auto popIfPushed = [&](bool& flag, const char* label, auto&& call)
        {
            if (!flag)
                return;
            std::string localGuard;
            if (TryGuardedVoidCall(label, std::forward<decltype(call)>(call), &localGuard))
            {
                flag = false;
                ++popped;
            }
            else
            {
                ++failures;
                guardReason = localGuard;
            }
        };

        popIfPushed(
            m_proxyLifecycleDisableAudiblePushed,
            "proxy lifecycle resume PopDisableAudible",
            [npc]() { npc->PopDisableAudible(); });
        popIfPushed(
            m_proxyLifecycleDisableVisiblePushed,
            "proxy lifecycle resume PopDisableVisible",
            [npc]() { npc->PopDisableVisible(); });
        popIfPushed(
            m_proxyLifecycleDisableSensesPushed,
            "proxy lifecycle resume PopDisableSenses",
            [npc]() { npc->PopDisableSenses(); });

        RecoverLiveNetworkNpc(*npc);
    }

    ApplyProxyName(*entity, GetRemoteUsernameOrFallback());
    ApplySurvivorFactionToProxy(*entity);
    RegisterProxyReviveInteraction(*entity);

    if (m_proxyLifecycleState != CoopProxyLifecycleState::ActiveSameLevel)
    {
        m_proxyLifecycleState = CoopProxyLifecycleState::ActiveSameLevel;
        ++m_proxyLifecycleSerial;
    }

    m_lastProxyLifecycleEvent =
        "proxy lifecycle #" + std::to_string(m_proxyLifecycleSerial) +
        " state=" + GetProxyLifecycleStateName(m_proxyLifecycleState) +
        " reason=" + (reason ? reason : "unknown") +
        " entity=" + std::to_string(m_proxyEntityId) +
        " popped=" + std::to_string(popped) +
        " failures=" + std::to_string(failures);
    if (!guardReason.empty())
        m_lastProxyLifecycleEvent += " lastGuard=" + guardReason;
    LogCoop(m_lastProxyLifecycleEvent);
}

void ModMain::TickProxyFollow(float frameTime)
{
    if (!m_followLocalPlayer || !IsGameReady())
        return;

    if (m_networkMode != CoopNetworkMode::Off &&
        !CoopRuntimeConfig::UnsafeFlag("COOP_ENABLE_NETWORK_PROXY_FOLLOW"))
        return;

    IEntity* proxyEntity = GetProxyEntity();
    if (!proxyEntity)
        return;

    m_proxyTickAccumulator += frameTime;
    if (m_proxyTickAccumulator < kProxyNetTickSeconds)
        return;

    m_proxyTickAccumulator = 0.0f;

    ArkPlayer& player = ArkPlayer::GetInstance();
    IEntity* playerEntity = player.GetEntity();
    if (!playerEntity)
        return;

    const Vec3 followPosition = GetOffsetFromPlayer(kProxyForwardOffsetMeters);
    const Quat followRotation = playerEntity->GetRotation();
    proxyEntity->SetPosRotScale(
        followPosition,
        followRotation,
        proxyEntity->GetScale(),
        0);

    if (m_keepDistractionsSuppressed)
    {
        if (ArkNpc* npc = EntityUtils::GetArkNpc(proxyEntity))
        {
            npc->FlowGraphIgnoreDistractions(true, true);
            npc->DeactivateWander();
        }
    }
}

void ModMain::TickProxyDamageSync()
{
    // Health polling cannot identify the author. It must never become a network damage source.
    if (!m_useHookedNpcDamage)
        ResetProxyHealthBaseline();
}

void ModMain::TickHostProxyCombatStimulus(float frameTime)
{
    (void)frameTime;

    if (m_networkMode != CoopNetworkMode::Host)
        return;

    m_lastProxyTargetBindings = 0;
    m_lastProxySimpleAttentionTracked = CountHostEnemiesTrackingProxySimple();
    m_lastProxyComplexAttentionTracked = CountHostEnemiesTrackingProxyComplex();
}

void ModMain::TickEnemyMimicryStateHeartbeat(float frameTime)
{
    if (m_networkMode == CoopNetworkMode::Off ||
        !IsEnemyReplicationGameplayReady() ||
        m_socket == kInvalidNetworkSocket ||
        !m_hasRemoteEndpoint)
    {
        m_enemyMimicryHeartbeatAccumulator = 0.0f;
        return;
    }

    m_enemyMimicryHeartbeatAccumulator += frameTime;
    if (m_enemyMimicryHeartbeatAccumulator < kEnemyMimicryHeartbeatSeconds)
        return;

    m_enemyMimicryHeartbeatAccumulator = 0.0f;
    if (!gEnv || !gEnv->pEntitySystem)
        return;

    // Mimicry is a durable NPC state, unlike a one-shot morph effect. Reannounce
    // only active authority-owned disguises at a low cadence so a late peer can
    // enter the same vanilla MimicEntity state without adding it to the 20 Hz
    // locomotion snapshot or replaying any local AI decision.
    for (auto& entry : m_enemyAuthorities)
    {
        EnemyAuthorityState& state = entry.second;
        IEntity* entity = state.entityId != INVALID_ENTITYID
            ? gEnv->pEntitySystem->GetEntity(state.entityId)
            : nullptr;
        ArkNpc* npc = entity ? EntityUtils::GetArkNpc(entity) : nullptr;
        if (!entity || !npc)
            continue;

        std::string guardReason;
        bool isMimicking = false;
        if (!TryGuardedCall(
                "enemy mimicry heartbeat IsMimicking",
                [npc]() { return npc->IsMimicking(); },
                isMimicking,
                &guardReason))
        {
            continue;
        }

        const CoopEnemyControlPolicy::Decision controlDecision =
            CoopEnemyControlPolicy::Evaluate(BuildLocalEnemyControlPolicyContext(state, *entity));
        if (!controlDecision.localVanillaAuthority)
        {
            // EndMimicry can reject while another native Mimic action is
            // retiring. Once the authority has published an inactive state,
            // keep reconciling the observer instead of leaving its AI trapped
            // inside the old prop body indefinitely.
            if (controlDecision.remoteDriven &&
                state.localMimicryStateKnown &&
                !state.localMimicryActive &&
                isMimicking)
            {
                bool endResult = false;
                m_applyingRemoteEnemyAbilityFxEvent = true;
                const bool callOk = TryGuardedCall(
                    "remote npc mimicry end reconcile",
                    [npc]() { return npc->EndMimicry(); },
                    endResult,
                    &guardReason);
                m_applyingRemoteEnemyAbilityFxEvent = false;
                AppendEnemySyncTrace(
                    "enemy_fx",
                    std::string(callOk && endResult
                        ? "remote_enemy_mimicry_end_reconciled"
                        : "remote_enemy_mimicry_end_retry") +
                        " net=" + std::to_string(state.netId) +
                        " entity=" + std::to_string(entity->GetId()) +
                        " guard=" + (guardReason.empty() ? std::string("-") : guardReason));
            }
            continue;
        }

        if (!isMimicking)
        {
            if (state.localMimicryActive)
            {
                QueueLocalEnemyMimicryEventForHook(
                    npc,
                    nullptr,
                    false,
                    EArkNpcMimicryReason::none,
                    false);
            }
            continue;
        }

        unsigned targetEntityId = 0;
        EArkNpcMimicryReason mimicryReason = state.localMimicryReason;
        const bool targetIdRead = TryGuardedCall(
                "enemy mimicry heartbeat GetMimicingEntityId",
                [npc]() { return npc->GetMimicingEntityId(); },
                targetEntityId,
                &guardReason) &&
            targetEntityId != 0;
        TryGuardedCall(
            "enemy mimicry heartbeat GetMimicryReason",
            [npc]() { return npc->GetMimicryReason(); },
            mimicryReason,
            &guardReason);

        IEntity* targetEntity = targetIdRead
            ? gEnv->pEntitySystem->GetEntity(static_cast<EntityId>(targetEntityId))
            : nullptr;
        if (targetEntity)
        {
            QueueLocalEnemyMimicryEventForHook(
                npc,
                targetEntity,
                true,
                mimicryReason,
                state.localMimicryIgnorePsi);
            continue;
        }

        // MimicAndReplaceEntity may retire a runtime target after the native
        // transition. Preserve the descriptor captured by OnStartedMimicking
        // so active-state heartbeats and later authority handoffs do not depend
        // on the source process's now-dead EntityId.
        if (!state.localMimicryActive ||
            (state.localMimicryTargetGuid == 0 && state.localMimicryTargetArchetypeId == 0))
        {
            continue;
        }

        CoopSerialSequence::Advance(state.localMimicryEventSequence);
        uint16_t eventFlags = CoopProtocol::kEnemyAbilityFxFlagWorldEvent;
        if (state.localMimicryIgnorePsi)
            eventFlags |= CoopProtocol::kEnemyAbilityFxFlagMimicIgnorePsi;
        QueueLocalEnemyAbilityFxEventForHook(
            state,
            *entity,
            CoopProtocol::kEnemyAbilityFxNpcMimicryBegin,
            static_cast<int>(mimicryReason),
            CoopProtocol::kInvalidMannequinOrdinal,
            state.localMimicryEventSequence,
            state.localMimicryTargetPosition,
            entity->GetWorldRotation().GetColumn1(),
            "native npc mimicry stored-target heartbeat",
            state.localMimicryTargetGuid,
            eventFlags,
            state.localMimicryTargetArchetypeId);
    }
}

void ModMain::TickLocalEnemyAreaAuthoritySync(float frameTime)
{
    const bool localAreaAuthority =
        m_networkMode == CoopNetworkMode::Host || IsClientAreaAuthorityActive();
    if ((!m_syncTestMimicSpawn && !m_enemyLocomotionSyncEnabled) ||
        !localAreaAuthority ||
        !IsEnemyReplicationGameplayReady() ||
        m_socket == kInvalidNetworkSocket || !m_hasRemoteEndpoint)
    {
        return;
    }

    if (m_enemyRegistryNeedsScan)
    {
        m_enemyRegistryScanAccumulator += frameTime;
        if (m_enemyRegistryScanAccumulator >= kEnemyRegistryDirtyDebounceSeconds)
        {
            m_enemyRegistryScanAccumulator = 0.0f;
            m_enemyRegistryNeedsScan = false;
            ScanLocalEnemyAuthorityRegistry("host enemy registry");
        }
    }

    m_mimicStateTickAccumulator += frameTime;
    if (m_mimicStateTickAccumulator < kMimicStateTickSeconds)
        return;

    m_mimicStateTickAccumulator = 0.0f;

    if (m_enemyAuthorities.empty())
        return;

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

    size_t sentStates = 0;
    std::vector<uint64_t> staleNetIds;
    std::vector<std::pair<uint64_t, EntityId>> deadEthericDoppelgangersToRemove;
    for (auto& entry : m_enemyAuthorities)
    {
        if (sentStates >= kMaxEnemyStatesPerTick)
            break;

        EnemyAuthorityState& state = entry.second;
        state.localSnapshotSilenceSeconds += kMimicStateTickSeconds;
        bool dead = state.deathCommitRepeatsRemaining > 0;
        const bool entityGone = !gEnv || !gEnv->pEntitySystem || state.entityId == INVALID_ENTITYID ||
            !gEnv->pEntitySystem->GetEntity(state.entityId);
        const bool timedEthericDoppelganger =
            (state.rosterFlags & CoopProtocol::kEnemyRosterFlagEthericDoppelganger) != 0;
        if (entityGone && timedEthericDoppelganger)
        {
            if ((state.rosterFlags & CoopProtocol::kEnemyRosterFlagRemoved) == 0)
            {
                state.rosterFlags &= ~CoopProtocol::kEnemyRosterFlagAlive;
                state.rosterFlags |= CoopProtocol::kEnemyRosterFlagRemoved;
                ++state.rosterVersion;
                state.rosterAnnouncedVersion = 0;
            }
            if (EnsureEnemyRosterAnnounced(
                    state, "Etheric Doppelganger timed removal roster failed"))
            {
                for (auto& sourceEntry : m_enemyAuthorities)
                {
                    EnemyAuthorityState& sourceState = sourceEntry.second;
                    if (sourceState.stableEnemyId != state.sourceStableEnemyId)
                        continue;
                    sourceState.activeEthericDoppelgangerStableEnemyId = 0;
                    if (gEnv && gEnv->pEntitySystem &&
                        sourceState.entityId != INVALID_ENTITYID)
                    {
                        if (IEntity* sourceEntity =
                                gEnv->pEntitySystem->GetEntity(sourceState.entityId))
                        {
                            if (ArkNpc* sourceNpc = EntityUtils::GetArkNpc(sourceEntity))
                            {
                                TryGuardedVoidCall(
                                    "clear expired Etheric Doppelganger relation",
                                    [sourceNpc]()
                                    {
                                        sourceNpc->SetEthericDoppelgangerId(INVALID_ENTITYID);
                                    },
                                    nullptr);
                            }
                        }
                    }
                    break;
                }
                m_enemyStableSpawnIdsByEntity.erase(state.entityId);
                m_enemyEthericDoppelgangerSourcesByEntity.erase(state.entityId);
                m_enemyEthericDoppelgangerGenerationsByEntity.erase(state.entityId);
                m_enemyEthericDoppelgangerRequestsSentByEntity.erase(state.entityId);
                staleNetIds.push_back(entry.first);
                m_lastEthericDoppelgangerEvent =
                    "timed_removal_announced childNet=" + std::to_string(state.netId) +
                    " sourceStable=" + std::to_string(state.sourceStableEnemyId) +
                    " generation=" + std::to_string(state.ethericDoppelgangerGeneration);
                AppendEnemySyncTrace("etheric_doppelganger", m_lastEthericDoppelgangerEvent);
            }
            continue;
        }
        if (entityGone && state.hasLastPosition && !state.sentDeadState)
        {
            dead = true;
            state.deathCommitRepeatsRemaining = std::max(state.deathCommitRepeatsRemaining, kMimicDeathCommitRepeatCount);
        }

        if (!dead && gEnv && gEnv->pEntitySystem && state.entityId != INVALID_ENTITYID)
        {
            if (IEntity* entity = gEnv->pEntitySystem->GetEntity(state.entityId))
            {
                if (ArkNpc* npc = EntityUtils::GetArkNpc(entity))
                {
                    dead = npc->IsDead();
                    state.localRotationOverrideSeconds = std::max(
                        0.0f,
                        state.localRotationOverrideSeconds - kMimicStateTickSeconds);
                    state.localNativeAttentionSeconds = std::max(
                        0.0f,
                        state.localNativeAttentionSeconds - kMimicStateTickSeconds);

                    for (auto candidateIt = state.attentionCandidates.begin();
                        candidateIt != state.attentionCandidates.end();)
                    {
                        if (candidateIt->first == GetLocalAccountToken())
                        {
                            ++candidateIt;
                            continue;
                        }

                        candidateIt->second.silentSeconds += kMimicStateTickSeconds;
                        const auto peerIt = m_remotePeers.find(candidateIt->first);
                        const bool peerAvailable =
                            peerIt != m_remotePeers.end() &&
                            peerIt->second.sessionReady &&
                            IsKnownSameLevel(m_localLevelName, peerIt->second.levelName);
                        if (!peerAvailable ||
                            candidateIt->second.silentSeconds >= kEnemyRemoteAuthorityTimeoutSeconds)
                        {
                            candidateIt = state.attentionCandidates.erase(candidateIt);
                        }
                        else
                        {
                            ++candidateIt;
                        }
                    }

                    const bool hostAuthorityBlocked =
                        m_localPlayerDowned || IsLocalPlayerAuthorityBlockedByModalState();
                    uint8_t localAttentionLevel = hostAuthorityBlocked
                        ? CoopEnemyAuthorityPolicy::kUnknownAttention
                        : LocalPlayerEnemyAttentionLevelForCoop(*entity);
                    state.localAttentionLevel = localAttentionLevel;
                    if (localAttentionLevel > CoopEnemyAuthorityPolicy::kUnknownAttention)
                        state.localRotationOverrideSeconds = kEnemyLocalRotationOverrideGraceSeconds;

                    UpdateEnemyAttentionCandidate(
                        state,
                        GetLocalAccountToken(),
                        localAttentionLevel,
                        hostAuthorityBlocked,
                        localAttentionLevel > CoopEnemyAuthorityPolicy::kUnknownAttention
                            ? GetLocalAccountToken()
                            : 0,
                        0,
                        false);
                    const CoopEnemyAuthorityPolicy::Decision authorityDecision =
                        SelectEnemyAttentionAuthority(state, GetLocalAccountToken());
                    ApplyEnemyAttentionAuthorityDecisionOnAreaAuthority(
                        state,
                        authorityDecision,
                        nullptr,
                        0,
                        "attention level sample");

                    state.localAttentionClaimed =
                        state.authorityOwnerAccountToken == GetLocalAccountToken() &&
                        state.authorityAttentionLevel > CoopEnemyAuthorityPolicy::kUnknownAttention;
                    if (state.localAttentionClaimed)
                    {
                        state.localAttentionClaimedSeconds += kMimicStateTickSeconds;
                        state.localAttentionLostSeconds = 0.0f;
                    }
                    else
                    {
                        state.localAttentionClaimedSeconds = 0.0f;
                    }
                }

                float health = 0.0f;
                float maxHealth = 0.0f;
                if (ReadEntityHealth(state.entityId, health, maxHealth) && health <= 0.0f)
                    dead = true;
            }
        }

        if (dead && timedEthericDoppelganger)
        {
            state.ethericDoppelgangerDeadSeconds += kMimicStateTickSeconds;
            for (auto& sourceEntry : m_enemyAuthorities)
            {
                EnemyAuthorityState& sourceState = sourceEntry.second;
                if (sourceState.stableEnemyId != state.sourceStableEnemyId)
                    continue;
                sourceState.activeEthericDoppelgangerStableEnemyId = 0;
                if (gEnv && gEnv->pEntitySystem &&
                    sourceState.entityId != INVALID_ENTITYID)
                {
                    if (IEntity* sourceEntity =
                            gEnv->pEntitySystem->GetEntity(sourceState.entityId))
                    {
                        if (ArkNpc* sourceNpc = EntityUtils::GetArkNpc(sourceEntity))
                        {
                            TryGuardedVoidCall(
                                "clear killed Etheric Doppelganger relation",
                                [sourceNpc]()
                                {
                                    sourceNpc->SetEthericDoppelgangerId(INVALID_ENTITYID);
                                },
                                nullptr);
                        }
                    }
                }
                break;
            }

            // A native death commit is repeated for 1.5 seconds. Do not hide
            // the body before that reliable presentation window completes;
            // remote-authority deaths arrive with sentDeadState already set
            // and still receive the same minimum one-second local grace.
            if (!state.ethericDoppelgangerRemovalQueued &&
                state.sentDeadState &&
                state.deathCommitRepeatsRemaining == 0 &&
                state.ethericDoppelgangerDeadSeconds >= 1.0f &&
                state.entityId != INVALID_ENTITYID)
            {
                state.ethericDoppelgangerRemovalQueued = true;
                deadEthericDoppelgangersToRemove.emplace_back(entry.first, state.entityId);
            }
        }
        else if (timedEthericDoppelganger)
        {
            state.ethericDoppelgangerDeadSeconds = 0.0f;
        }

        CoopEvents::Context routeContext = {};
        routeContext.kind = dead ? CoopEvents::Kind::EnemyDeathCommit : CoopEvents::Kind::EnemyStateSnapshot;
        routeContext.domain = CoopEvents::AuthorityDomain::AreaOwned;
        routeContext.sourceRole = CoopEvents::EntityRole::HostEnemyAuthority;
        routeContext.targetRole = CoopEvents::EntityRole::EnemyPuppet;
        routeContext.localMode = m_networkMode;
        routeContext.sourceEntityId = state.entityId;
        routeContext.sameLevel = true;
        routeContext.localAuthority = true;
        routeContext.irreversible = dead;

        const CoopEvents::Route route = RouteCoopEvent(routeContext);
        if (m_enemyLocomotionSyncEnabled && !m_syncTestMimicSpawn)
        {
            if (state.remoteLocomotionAuthority)
                continue;

            if (dead)
            {
                // A native corpse can continue settling briefly after OnKill.
                // Refresh the authoritative transform for every bounded death
                // repeat so the final packet carries the settled body pose,
                // rather than the pre-ragdoll kill position.
                if (gEnv && gEnv->pEntitySystem && state.entityId != INVALID_ENTITYID)
                {
                    if (IEntity* corpse = gEnv->pEntitySystem->GetEntity(state.entityId))
                    {
                        state.lastPosition = corpse->GetWorldPos();
                        state.lastRotation = corpse->GetWorldRotation();
                        state.hasLastPosition = true;
                    }
                }
                SendEnemyStateNow(state, "enemy death commit send failed");
                continue;
            }

            CoopProtocol::TestMimicStatePacket packet = {};
            bool rosterReady = false;
            if (BuildEnemyStatePacket(state, packet))
            {
                m_networkTelemetry.RecordProducerAttempt(
                    static_cast<uint16_t>(CoopProtocol::PacketType::TestMimicState));
                packet.sourceFlags |= CoopProtocol::kEnemyStateSourceFlagAuthoritySnapshot;
                const bool localAuthorityBlocked =
                    IsLocalPlayerAuthorityBlockedByModalState() || m_localPlayerDowned;
                if (ShouldMarkEnemyAuthorityHasAttention(state, localAuthorityBlocked))
                    packet.sourceFlags |= CoopProtocol::kEnemyStateSourceFlagAuthorityHasAttention;
                if (localAuthorityBlocked)
                    packet.sourceFlags |= CoopProtocol::kEnemyStateSourceFlagAuthorityBlocked;

                // The high-rate locomotion path bypasses SendEnemyStateNow,
                // so it must establish the reliable roster boundary itself.
                // Never let an unreliable pose invent or reuse client-local
                // net identity before this exact record is acknowledged.
                rosterReady = EnsureEnemyRosterAnnounced(
                    state,
                    "enemy locomotion roster announce failed");
            }
            const Vec3 packetPosition(packet.px, packet.py, packet.pz);
            const float interestDistanceSq = std::min(
                hasLocalPlayerPosition
                    ? (packetPosition - localPlayerPosition).GetLengthSquared()
                    : std::numeric_limits<float>::max(),
                hasRemotePlayerPosition
                    ? (packetPosition - remotePlayerPosition).GetLengthSquared()
                    : std::numeric_limits<float>::max());
            const bool forceSnapshot =
                !state.hasLastTransmittedStatePacket ||
                state.localBurstMannequinSendPending ||
                state.localSnapshotSilenceSeconds >= EnemyStateHeartbeatSeconds(interestDistanceSq) ||
                (packet.flags & (CoopProtocol::kTestMimicStateFlagHitCommit |
                    CoopProtocol::kTestMimicStateFlagDeathCommit)) != 0;
            const bool changedSnapshot =
                state.hasLastTransmittedStatePacket &&
                EnemyStatePacketMateriallyDiffers(packet, state.lastTransmittedStatePacket);
            const bool changeCadenceReady =
                state.localSnapshotSilenceSeconds >= EnemyStateMinimumTransmitInterval(
                    packet,
                    interestDistanceSq);
            const bool emitSnapshot = rosterReady &&
                (forceSnapshot || (changedSnapshot && changeCadenceReady));
            if (!emitSnapshot && packet.sequence != 0)
            {
                m_networkTelemetry.RecordProducerSuppressed(
                    static_cast<uint16_t>(CoopProtocol::PacketType::TestMimicState),
                    rosterReady
                        ? CoopNetworkTelemetry::ProducerSuppressionReason::Unchanged
                        : CoopNetworkTelemetry::ProducerSuppressionReason::Readiness);
            }
            if (emitSnapshot &&
                packet.sequence != 0 &&
                SendTestMimicStateTo(packet, m_remoteAddress, m_remotePort, "enemy locomotion send failed"))
            {
                ++sentStates;
                state.localBurstMannequinSendPending = false;
                state.lastTransmittedStatePacket = packet;
                state.hasLastTransmittedStatePacket = true;
                state.localSnapshotSilenceSeconds = 0.0f;
            }
        }
        else if (route.shouldEmitNetwork && SendEnemyStateNow(state, "enemy state send failed"))
        {
            ++sentStates;
        }

        if (entityGone && state.deathCommitRepeatsRemaining == 0 && state.sentDeadState)
            staleNetIds.push_back(entry.first);
    }

    // Removing an entity can synchronously invoke IEntitySystemSink::OnRemove,
    // so defer it until the authority-map iteration has finished.
    for (const auto& pending : deadEthericDoppelgangersToRemove)
    {
        const auto stateIt = m_enemyAuthorities.find(pending.first);
        if (stateIt == m_enemyAuthorities.end() ||
            stateIt->second.entityId != pending.second)
        {
            continue;
        }

        PrepareCoopEntityForRemoval(
            pending.second,
            false,
            false,
            "retire killed Etheric Doppelganger");
        if (RemoveCoopEntityGuarded(
                pending.second,
                true,
                "retire killed Etheric Doppelganger"))
        {
            m_lastEthericDoppelgangerEvent =
                "killed_body_removed childNet=" + std::to_string(pending.first) +
                " entity=" + std::to_string(pending.second) +
                " deathSeconds=" +
                    std::to_string(stateIt->second.ethericDoppelgangerDeadSeconds);
            AppendEnemySyncTrace("etheric_doppelganger", m_lastEthericDoppelgangerEvent);
        }
        else
        {
            stateIt->second.ethericDoppelgangerRemovalQueued = false;
        }
    }

    for (const uint64_t netId : staleNetIds)
    {
        const auto stateIt = m_enemyAuthorities.find(netId);
        if (stateIt != m_enemyAuthorities.end())
        {
            if (stateIt->second.entityId != INVALID_ENTITYID)
                m_enemyNetIdsByEntity.erase(stateIt->second.entityId);
                m_enemyStableSpawnIdsByEntity.erase(stateIt->second.entityId);
            m_enemyAuthorities.erase(stateIt);
        }
    }
}

bool ModMain::SendAuthoritativeMimicStateNow(const char* failurePrefix)
{
    if (m_networkMode != CoopNetworkMode::Host || m_socket == kInvalidNetworkSocket || !m_hasRemoteEndpoint)
        return false;

    if (gEnv && gEnv->pEntitySystem && m_mimicEntityId != INVALID_ENTITYID && !m_mimicIsPuppet)
    {
        if (IEntity* mimicEntity = gEnv->pEntitySystem->GetEntity(m_mimicEntityId))
        {
            if (IsEnemyReplicationCandidate(*mimicEntity))
            {
                EnemyAuthorityState& state = EnsureEnemyAuthorityState(*mimicEntity);
                return SendEnemyStateNow(state, failurePrefix);
            }
        }
    }

    CoopProtocol::TestMimicStatePacket packet = {};
    if (!BuildTestMimicStatePacket(packet))
        return false;

    if (SendTestMimicStateTo(packet, m_remoteAddress, m_remotePort, failurePrefix))
    {
        if ((packet.flags & CoopProtocol::kTestMimicStateFlagHealthKnown) != 0)
        {
            m_mimicHealthAvailable = true;
            m_lastMimicHealth = packet.health;
            m_lastMimicMaxHealth = packet.maxHealth;
        }

        if ((packet.flags & CoopProtocol::kTestMimicStateFlagDead) != 0)
        {
            m_sentMimicDeadState = true;
            if (m_mimicDeathCommitRepeatsRemaining > 0)
                --m_mimicDeathCommitRepeatsRemaining;
        }
        else if ((packet.flags & CoopProtocol::kTestMimicStateFlagHitCommit) != 0)
        {
            m_lastMimicDamage = 0.0f;
        }

        return true;
    }

    return false;
}

bool ModMain::SendEnemyStateNow(EnemyAuthorityState& state, const char* failurePrefix)
{
    if (m_networkMode == CoopNetworkMode::Off || m_socket == kInvalidNetworkSocket || !m_hasRemoteEndpoint)
        return false;

    CoopProtocol::TestMimicStatePacket packet = {};
    if (!BuildEnemyStatePacket(state, packet))
        return false;
    const bool packetDead = (packet.flags & CoopProtocol::kTestMimicStateFlagDead) != 0;
    const bool rosterAlive = (state.rosterFlags & CoopProtocol::kEnemyRosterFlagAlive) != 0;
    if (packetDead == rosterAlive)
    {
        if (packetDead)
            state.rosterFlags &= ~CoopProtocol::kEnemyRosterFlagAlive;
        else
            state.rosterFlags |= CoopProtocol::kEnemyRosterFlagAlive;
        ++state.rosterVersion;
        state.rosterAnnouncedVersion = 0;
    }
    if (!EnsureEnemyRosterAnnounced(state, "enemy roster announce failed"))
        return false;
    const bool localAuthorityBlocked = IsLocalPlayerAuthorityBlockedByModalState() || m_localPlayerDowned;
    if (!state.remoteLocomotionAuthority && gEnv && gEnv->pEntitySystem && state.entityId != INVALID_ENTITYID)
    {
        if (IEntity* entity = gEnv->pEntitySystem->GetEntity(state.entityId))
        {
            state.localAttentionLevel = localAuthorityBlocked
                ? CoopEnemyAuthorityPolicy::kUnknownAttention
                : LocalPlayerEnemyAttentionLevelForCoop(*entity);
            state.authorityAttentionLevel = state.localAttentionLevel;
            packet.authorityAttentionLevel = state.authorityAttentionLevel;
        }
    }
    if (packet.authorityAttentionLevel > CoopEnemyAuthorityPolicy::kUnknownAttention)
    {
        packet.sourceFlags |= CoopProtocol::kEnemyStateSourceFlagAuthorityHasAttention;
        if (!state.remoteLocomotionAuthority)
            packet.targetAccountToken = GetLocalAccountToken();
    }
    if (localAuthorityBlocked)
        packet.sourceFlags |= CoopProtocol::kEnemyStateSourceFlagAuthorityBlocked;

    m_networkTelemetry.RecordProducerAttempt(
        static_cast<uint16_t>(CoopProtocol::PacketType::TestMimicState));
    const Vec3 packetPosition(packet.px, packet.py, packet.pz);
    float interestDistanceSq = std::numeric_limits<float>::max();
    if (ArkPlayer::GetInstancePtr())
    {
        if (IEntity* playerEntity = ArkPlayer::GetInstance().GetEntity())
            interestDistanceSq = (packetPosition - playerEntity->GetWorldPos()).GetLengthSquared();
    }
    if (IEntity* proxyEntity = GetProxyEntity())
    {
        interestDistanceSq = std::min(
            interestDistanceSq,
            (packetPosition - proxyEntity->GetWorldPos()).GetLengthSquared());
    }
    const bool forceSnapshot =
        !state.hasLastTransmittedStatePacket ||
        state.localBurstMannequinSendPending ||
        state.localSnapshotSilenceSeconds >= EnemyStateHeartbeatSeconds(interestDistanceSq) ||
        (packet.flags & (CoopProtocol::kTestMimicStateFlagHitCommit |
            CoopProtocol::kTestMimicStateFlagDeathCommit)) != 0;
    const bool changedSnapshot =
        state.hasLastTransmittedStatePacket &&
        EnemyStatePacketMateriallyDiffers(packet, state.lastTransmittedStatePacket);
    const bool changeCadenceReady =
        state.localSnapshotSilenceSeconds >= EnemyStateMinimumTransmitInterval(packet, interestDistanceSq);
    if (!forceSnapshot && (!changedSnapshot || !changeCadenceReady))
    {
        m_networkTelemetry.RecordProducerSuppressed(
            static_cast<uint16_t>(CoopProtocol::PacketType::TestMimicState),
            CoopNetworkTelemetry::ProducerSuppressionReason::Unchanged);
        return false;
    }

    if (!SendTestMimicStateTo(packet, m_remoteAddress, m_remotePort, failurePrefix))
        return false;

    state.localBurstMannequinSendPending = false;
    state.lastTransmittedStatePacket = packet;
    state.hasLastTransmittedStatePacket = true;
    state.localSnapshotSilenceSeconds = 0.0f;

    if ((packet.flags & CoopProtocol::kTestMimicStateFlagHealthKnown) != 0 && state.netId == kTestMimicNetId)
    {
        m_mimicHealthAvailable = true;
        m_lastMimicHealth = packet.health;
        m_lastMimicMaxHealth = packet.maxHealth;
    }

    if ((packet.flags & CoopProtocol::kTestMimicStateFlagDead) != 0)
    {
        state.sentDeadState = true;
        if (state.deathCommitRepeatsRemaining > 0)
            --state.deathCommitRepeatsRemaining;

        if (state.netId == kTestMimicNetId)
        {
            m_sentMimicDeadState = true;
            m_mimicDeathCommitRepeatsRemaining = state.deathCommitRepeatsRemaining;
        }
    }
    else if ((packet.flags & CoopProtocol::kTestMimicStateFlagHitCommit) != 0)
    {
        state.lastDamage = 0.0f;
        if (state.netId == kTestMimicNetId)
            m_lastMimicDamage = 0.0f;
    }

    return true;
}

bool ModMain::ReadProxyHealth(float& health, float& maxHealth) const
{
    return ReadEntityHealth(m_proxyEntityId, health, maxHealth);
}

bool ModMain::ReadEntityHealth(EntityId entityId, float& health, float& maxHealth) const
{
    if (!gEnv || !gEnv->pEntitySystem || entityId == INVALID_ENTITYID)
        return false;

    std::string guardReason;
    IEntity* entity = nullptr;
    if (!TryGuardedCall(
            "ReadEntityHealth IEntitySystem::GetEntity",
            [entityId]() { return gEnv && gEnv->pEntitySystem ? gEnv->pEntitySystem->GetEntity(entityId) : nullptr; },
            entity,
            &guardReason) ||
        !entity)
    {
        return false;
    }

    ArkHealthExtension* healthExtension = nullptr;
    if (!TryGuardedCall(
            "ReadEntityHealth ArkHealthExtension::GetExtension id",
            [entityId]() { return ArkHealthExtension::GetExtension(entityId); },
            healthExtension,
            &guardReason) ||
        !healthExtension)
    {
        return false;
    }

    float currentHealth = 0.0f;
    float currentMaxHealth = 0.0f;
    if (!TryGuardedCall(
            "ReadEntityHealth health",
            [healthExtension]() { return healthExtension->m_health; },
            currentHealth,
            &guardReason) ||
        !TryGuardedCall(
            "ReadEntityHealth maxHealth",
            [healthExtension]() { return healthExtension->m_maxHealth; },
            currentMaxHealth,
            &guardReason))
    {
        return false;
    }

    health = currentHealth;
    maxHealth = currentMaxHealth;
    return maxHealth > 0.0f;
}

void ModMain::ResetProxyHealthBaseline()
{
    m_hasProxyHealthBaseline = false;
    m_proxyHealthAvailable = false;
    m_lastProxyHealth = 0.0f;
    m_lastProxyMaxHealth = 0.0f;
}
