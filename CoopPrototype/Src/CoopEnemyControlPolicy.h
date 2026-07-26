#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace CoopEnemyControlPolicy
{
enum class NetworkMode
{
    Off,
    Host,
    Client,
};

enum class Intent
{
    Any,
    Movement,
    Turn,
    Look,
    Facing,
    Attack,
};

enum class EnemyControlMode
{
    LocalOwner,
    LocalTargetRemoteOwner,
    RemoteObserver,
};

struct Context
{
    NetworkMode networkMode = NetworkMode::Off;
    bool localAuthorityBlocked = false;
    bool localHasAttention = false;
    bool localAttentionClaimed = false;
    bool localLeaseOwner = false;
    bool localRotationOverrideActive = false;
    bool remoteLocomotionAuthority = false;
    bool remoteAuthorityHasAttention = false;
    bool remoteTargetsLocalPlayer = false;
    bool remoteTargetLocallyRepresented = false;
    bool hasLastPosition = false;
    // Keep the first production phase as a strict authority mirror. The local
    // target mixer is retained for the later attention/combat mixdown phase,
    // but it must be opted into explicitly by that phase.
    bool localTargetMixEnabled = false;
};

struct Decision
{
    EnemyControlMode mode = EnemyControlMode::LocalOwner;
    bool remoteDriven = false;
    bool localVanillaAuthority = false;
    bool localFocus = false;
    bool blockMovement = false;
    bool blockTurn = false;
    bool blockLook = false;
    bool blockFacing = false;
    bool blockAttack = false;
    bool mirrorRemoteActions = false;
    bool preserveLocalCombat = false;
    bool localPhysicsAuthority = true;
    bool blockWorldCollision = false;

    bool BlocksAnyLocalVanilla() const;
};

struct RemoteActionContext
{
    EnemyControlMode mode = EnemyControlMode::LocalOwner;
    uint32_t effectiveLocomotionFlags = 0;
    uint32_t actionFlags = 0;
    uint32_t activeRemoteActionFlags = 0;
    int activeRemoteActionPriority = 0;
    float activeRemoteActionUntilTime = -1000.0f;
    float remoteTargetMotionSeconds = 0.0f;
    uint32_t remoteTargetMotionFlags = 0;
    float remoteVisualMotionSeconds = 0.0f;
    uint32_t remoteVisualMotionFlags = 0;
    float now = 0.0f;
    bool hasRemoteActionPacket = false;
    bool activeRemoteActionPresent = false;
    bool activeRemoteMovementPresent = false;
    bool localHasAttention = false;
    bool localCombatActionActive = false;
    bool mirrorPassiveAuthorityActions = false;
    bool authorityMannequinMovementCarry = false;
};

struct RemoteActionDecision
{
    uint32_t actionLaneFlags = 0;
    bool wantsMovement = false;
    bool hardOverride = false;
    bool bodyLockAction = false;
    bool actionUsesFullBody = false;
    bool observerActionOwnsFullBody = false;
    bool observerFullBodyActionActive = false;
    bool allowMovementLane = false;
    bool allowObserverIdleLane = false;
    bool actionStopsMovement = false;
    bool skipPassiveAction = false;
    bool skipPassiveActionWhileMovement = false;
    bool skipPassiveMovementCarryIdle = false;
    bool suppressActionPacket = false;
    bool allowActionOnlyOverlayFallback = false;
    const char* suppressActionReason = "-";
    int actionLayer = 0;
};

struct RemoteMotionContext
{
    EnemyControlMode mode = EnemyControlMode::LocalOwner;
    uint32_t locomotionFlags = 0;
    uint32_t authorityPacketLocomotionFlags = 0;
    uint32_t previousLocomotionFlags = 0;
    int mannequinSequence = 0;
    int mannequinFragmentId = -1;
    float packetSpeed = 0.0f;
    float packetTargetStepLen = 0.0f;
    float packetDeltaLen = 0.0f;
    float previousFilteredTargetSpeed = 0.0f;
    float previousMotionSeconds = 0.0f;
    float tickSeconds = 0.05f;
    float movementIntentSpeedThreshold = 0.75f;
    float targetMotionInferenceMinStep = 0.05f;
    float targetMotionInferenceMinSpeed = 0.85f;
    float catchupIntentDistance = 0.24f;
    float receiverInferRunSpeed = 5.7f;
    float passiveInferredMaxWalkSpeed = 1.8f;
    float movementHoldSeconds = 0.40f;
    bool hasExistingState = false;
    bool existingRemoteLocomotionAuthority = false;
    bool authorityGlooed = false;
    bool inferenceDisabled = false;
    bool heldAuthorityMannequin = false;
    bool authorityMannequinMovementCarry = false;
    bool authorityActionAllowsMovementAnimation = true;
    bool activeRemoteActionBlocksMotionInference = false;
    bool explicitBurstEvent = false;
};

struct RemoteMotionDecision
{
    uint32_t locomotionFlags = 0;
    uint32_t inferredMovementFlags = 0;
    uint32_t targetMotionFlags = 0;
    uint32_t actionTargetMotionFlags = 0;
    float filteredTargetSpeed = 0.0f;
    float inferredTargetSpeed = 0.0f;
    float actionTargetMotionSpeed = 0.0f;
    float motionSeconds = 0.0f;
    bool strippedUnconfirmedActionMovement = false;
    bool strippedHeldPassiveMovement = false;
    bool heldPassiveMovementEvidence = false;
    bool targetMotionInferenceBlockedByAction = false;
    bool blockedPreviousMovementHold = false;
    bool blockedPassiveDriftOnlyMotion = false;
    bool inferredCatchupDrift = false;
    bool heldPreviousMovement = false;
    bool passiveActionMovementHoldEvidence = false;
    bool passiveNonCarryActionMotionBlocked = false;
    bool confirmedTargetMotion = false;
    bool localTargetActionMotionEvidence = false;
    bool remoteActionTargetMotionEvidence = false;
    bool explicitBurstEvent = false;
    bool snapBurstToAuthority = false;
    bool currentMovementAnimationEvidence = false;
    bool movementHoldSuppressedByMissingAnimationEvidence = false;
    bool activeActionBlockedMovementAnimation = false;
};

uint32_t MovementFlags();
uint32_t ContinuousMovementFlags();
uint32_t BurstMovementFlags();
uint32_t ActionFlagsMask();
uint32_t BodyLockFlags();
uint32_t HardOverrideFlags();
uint32_t ActionOverlayCarryFlags();
bool HasMovementFlags(uint32_t flags);
bool HasHardOverrideFlags(uint32_t flags);
bool HasBodyLockFlags(uint32_t flags);
bool IsPassiveMannequinFlags(uint32_t flags);
bool IsPassiveMannequinCoveredByMovement(uint32_t flags);
bool FragmentNameCarriesAuthoredMovement(std::string_view fragmentName);
bool FragmentNameCarriesPassiveMovement(std::string_view fragmentName);
bool FragmentNameIsAuthoredMovementAction(std::string_view fragmentName);
bool FragmentNameIsAuthoredBurstMovement(std::string_view fragmentName);
bool FragmentNameIsPhantomDash(std::string_view fragmentName);
EnemyControlMode ResolveMode(const Context& context);
const char* ModeName(EnemyControlMode mode);
Decision Evaluate(const Context& context);
bool RunAttentionSelfTest(std::string& detail);
RemoteActionDecision EvaluateRemoteAction(const RemoteActionContext& context);
RemoteMotionDecision ResolveRemoteMotion(const RemoteMotionContext& context);
bool AllowsLocalVanillaControl(const Context& context);
bool IsRemoteDriven(const Context& context);
bool BlocksLocalVanillaControl(const Context& context);
bool BlocksLocalVanillaControlIntent(const Context& context, Intent intent);
}
