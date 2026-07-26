#include "CoopEnemyControlPolicy.h"

#include "CoopProtocol.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace CoopEnemyControlPolicy
{
namespace
{
bool ComputeRemoteDriven(const Context& context)
{
    if (context.networkMode == NetworkMode::Host)
        return context.remoteLocomotionAuthority || context.remoteAuthorityHasAttention;

    if (context.networkMode == NetworkMode::Client)
        return context.remoteLocomotionAuthority || context.hasLastPosition;

    return false;
}

bool ComputeLocalVanillaAuthority(const Context& context)
{
    // A confirmed lease is the simulation authority even if awareness flickers
    // for a frame. Attention selects/transfers ownership; it must not partially
    // puppet the process that already owns the enemy.
    if (!context.localAuthorityBlocked && context.localLeaseOwner)
        return true;

    if (!context.localAuthorityBlocked &&
        context.localAttentionClaimed &&
        context.localHasAttention &&
        !context.remoteLocomotionAuthority)
    {
        return true;
    }

    if (context.networkMode == NetworkMode::Host &&
        !context.remoteLocomotionAuthority &&
        !context.remoteAuthorityHasAttention)
    {
        return true;
    }

    return false;
}

EnemyControlMode ResolveModeInternal(const Context& context)
{
    const bool remoteDriven = ComputeRemoteDriven(context);
    if (ComputeLocalVanillaAuthority(context) || !remoteDriven)
        return EnemyControlMode::LocalOwner;

    if (context.localTargetMixEnabled &&
        !context.localAuthorityBlocked &&
        (context.localHasAttention ||
            context.localAttentionClaimed ||
            context.localRotationOverrideActive ||
            context.remoteTargetsLocalPlayer))
    {
        return EnemyControlMode::LocalTargetRemoteOwner;
    }

    return EnemyControlMode::RemoteObserver;
}

bool ActionOwnsFullBody(uint32_t actionFlags)
{
    return HasHardOverrideFlags(actionFlags) || HasBodyLockFlags(actionFlags);
}

std::string ToLowerAscii(std::string_view value)
{
    std::string result(value);
    std::transform(
        result.begin(),
        result.end(),
        result.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

bool ContainsToken(const std::string& value, const char* token)
{
    return token && token[0] && value.find(token) != std::string::npos;
}

}

bool Decision::BlocksAnyLocalVanilla() const
{
    return blockMovement ||
        blockTurn ||
        blockLook ||
        blockFacing ||
        blockAttack ||
        mirrorRemoteActions;
}

uint32_t MovementFlags()
{
    return ContinuousMovementFlags() | BurstMovementFlags();
}

uint32_t ContinuousMovementFlags()
{
    return CoopProtocol::kEnemyLocomotionFlagWalking |
        CoopProtocol::kEnemyLocomotionFlagRunning;
}

uint32_t BurstMovementFlags()
{
    return CoopProtocol::kEnemyLocomotionFlagDashing |
        CoopProtocol::kEnemyLocomotionFlagShifting |
        CoopProtocol::kEnemyLocomotionFlagMorphing |
        CoopProtocol::kEnemyLocomotionFlagLunging;
}

uint32_t ActionFlagsMask()
{
    return HardOverrideFlags() |
        CoopProtocol::kEnemyLocomotionFlagAttacking |
        CoopProtocol::kEnemyLocomotionFlagHitReacting |
        CoopProtocol::kEnemyLocomotionFlagStunned |
        CoopProtocol::kEnemyLocomotionFlagCowering |
        CoopProtocol::kEnemyLocomotionFlagRagdolled;
}

uint32_t BodyLockFlags()
{
    return CoopProtocol::kEnemyLocomotionFlagGlooed |
        CoopProtocol::kEnemyLocomotionFlagStunned |
        CoopProtocol::kEnemyLocomotionFlagCowering |
        CoopProtocol::kEnemyLocomotionFlagRagdolled;
}

uint32_t HardOverrideFlags()
{
    return CoopProtocol::kEnemyLocomotionFlagGlooed |
        CoopProtocol::kEnemyLocomotionFlagDashing |
        CoopProtocol::kEnemyLocomotionFlagShifting |
        CoopProtocol::kEnemyLocomotionFlagMorphing |
        CoopProtocol::kEnemyLocomotionFlagLunging;
}

uint32_t ActionOverlayCarryFlags()
{
    return CoopProtocol::kEnemyLocomotionFlagMannequinDriven |
        CoopProtocol::kEnemyLocomotionFlagInCombat |
        CoopProtocol::kEnemyLocomotionFlagWalking |
        CoopProtocol::kEnemyLocomotionFlagRunning;
}

bool HasMovementFlags(uint32_t flags)
{
    return (flags & MovementFlags()) != 0;
}

bool HasHardOverrideFlags(uint32_t flags)
{
    return (flags & HardOverrideFlags()) != 0;
}

bool HasBodyLockFlags(uint32_t flags)
{
    return (flags & BodyLockFlags()) != 0;
}

bool IsPassiveMannequinFlags(uint32_t flags)
{
    const uint32_t passiveFlags =
        CoopProtocol::kEnemyLocomotionFlagMannequinDriven |
        CoopProtocol::kEnemyLocomotionFlagTurning |
        CoopProtocol::kEnemyLocomotionFlagInCombat;
    const uint32_t activeFlags =
        CoopProtocol::kEnemyLocomotionFlagAttacking |
        CoopProtocol::kEnemyLocomotionFlagHitReacting |
        CoopProtocol::kEnemyLocomotionFlagStunned |
        CoopProtocol::kEnemyLocomotionFlagCowering |
        CoopProtocol::kEnemyLocomotionFlagGlooed |
        CoopProtocol::kEnemyLocomotionFlagDashing |
        CoopProtocol::kEnemyLocomotionFlagShifting |
        CoopProtocol::kEnemyLocomotionFlagMorphing |
        CoopProtocol::kEnemyLocomotionFlagLunging;
    return (flags & passiveFlags) != 0 && (flags & activeFlags) == 0;
}

bool IsPassiveMannequinCoveredByMovement(uint32_t flags)
{
    return HasMovementFlags(flags) && IsPassiveMannequinFlags(flags);
}

bool FragmentNameIsAuthoredMovementAction(std::string_view fragmentName)
{
    const std::string lower = ToLowerAscii(fragmentName);
    if (lower.empty())
        return false;

    return ContainsToken(lower, "grabplayer") ||
        ContainsToken(lower, "jumpattack") ||
        ContainsToken(lower, "pounce") ||
        ContainsToken(lower, "lunge") ||
        ContainsToken(lower, "leap") ||
        ContainsToken(lower, "sidestep") ||
        ContainsToken(lower, "dodge") ||
        ContainsToken(lower, "strafe") ||
        ContainsToken(lower, "charge") ||
        ContainsToken(lower, "ram");
}

bool FragmentNameIsAuthoredBurstMovement(std::string_view fragmentName)
{
    const std::string lower = ToLowerAscii(fragmentName);
    if (lower.empty())
        return false;

    return ContainsToken(lower, "grabplayer_in") ||
        ContainsToken(lower, "grabplayer_jump") ||
        ContainsToken(lower, "jumpattack") ||
        ContainsToken(lower, "pounce") ||
        ContainsToken(lower, "lunge") ||
        ContainsToken(lower, "leap") ||
        ContainsToken(lower, "sidestep") ||
        ContainsToken(lower, "dodge") ||
        ContainsToken(lower, "charge") ||
        ContainsToken(lower, "ram");
}

bool FragmentNameIsPhantomDash(std::string_view fragmentName)
{
    const std::string lower = ToLowerAscii(fragmentName);
    if (lower.empty() || !ContainsToken(lower, "phantom:"))
        return false;

    return ContainsToken(lower, "shift") ||
        ContainsToken(lower, "dash") ||
        ContainsToken(lower, "teleport") ||
        ContainsToken(lower, "charge");
}

bool FragmentNameCarriesPassiveMovement(std::string_view fragmentName)
{
    const std::string lower = ToLowerAscii(fragmentName);
    if (lower.empty())
        return false;

    if (ContainsToken(lower, "notice") ||
        ContainsToken(lower, "distractor") ||
        ContainsToken(lower, "interact") ||
        ContainsToken(lower, "speakerreact") ||
        ContainsToken(lower, "lightreact") ||
        ContainsToken(lower, "land") ||
        ContainsToken(lower, "stumble") ||
        ContainsToken(lower, "ragdoll") ||
        ContainsToken(lower, "recovery") ||
        ContainsToken(lower, "shift") ||
        ContainsToken(lower, "attack") ||
        ContainsToken(lower, "melee") ||
        ContainsToken(lower, "psi") ||
        ContainsToken(lower, "gloo"))
    {
        return false;
    }

    return ContainsToken(lower, "patrolidle") ||
        ContainsToken(lower, "wanderidle") ||
        ContainsToken(lower, "search") ||
        ContainsToken(lower, "motion_move") ||
        ContainsToken(lower, "idletomove") ||
        ContainsToken(lower, "idle_to_move") ||
        ContainsToken(lower, "lurk");
}

bool FragmentNameCarriesAuthoredMovement(std::string_view fragmentName)
{
    return FragmentNameCarriesPassiveMovement(fragmentName) ||
        FragmentNameIsAuthoredMovementAction(fragmentName);
}

EnemyControlMode ResolveMode(const Context& context)
{
    return ResolveModeInternal(context);
}

const char* ModeName(EnemyControlMode mode)
{
    switch (mode)
    {
    case EnemyControlMode::LocalOwner:
        return "LocalOwner";
    case EnemyControlMode::LocalTargetRemoteOwner:
        return "LocalTargetRemoteOwner";
    case EnemyControlMode::RemoteObserver:
        return "RemoteObserver";
    default:
        return "Unknown";
    }
}

Decision Evaluate(const Context& context)
{
    Decision decision;
    decision.mode = ResolveModeInternal(context);
    decision.remoteDriven = ComputeRemoteDriven(context);
    decision.localVanillaAuthority = decision.mode == EnemyControlMode::LocalOwner;
    decision.localFocus = decision.mode == EnemyControlMode::LocalTargetRemoteOwner;
    decision.localPhysicsAuthority = decision.mode == EnemyControlMode::LocalOwner;
    // Enemy replicas keep their native collider. Movement/transform and
    // damage authority are already gated at their real hooks; rewriting every
    // physics part to no-response proved unsafe across Mimicry and authority
    // handoff, and could permanently damage the eventual Vanilla owner body.
    decision.blockWorldCollision = false;

    switch (decision.mode)
    {
    case EnemyControlMode::LocalOwner:
        return decision;

    case EnemyControlMode::LocalTargetRemoteOwner:
        // The lease owner supplies locomotion and shared body states. Native
        // local perception owns turn, aim and combat against this process's
        // real player; damage to that player is therefore victim-local.
        decision.blockMovement = true;
        decision.blockTurn = false;
        decision.blockLook = false;
        decision.blockFacing = false;
        decision.blockAttack = false;
        decision.mirrorRemoteActions = true;
        decision.preserveLocalCombat = true;
        return decision;

    case EnemyControlMode::RemoteObserver:
    default:
        // Phase one is a strict presentation mirror. Perception may still
        // discover a future authority claim, but movement, body turn, look,
        // facing and combat output all belong to the current lease owner.
        // Local target mixing is enabled only after this mirror passes in both
        // authority directions.
        decision.blockMovement = true;
        decision.blockTurn = true;
        decision.blockLook = true;
        decision.blockFacing = true;
        decision.blockAttack = true;
        decision.mirrorRemoteActions = true;
        decision.preserveLocalCombat = false;
        return decision;
    }
}

bool RunAttentionSelfTest(std::string& detail)
{
    Context context;
    context.networkMode = NetworkMode::Client;
    context.remoteLocomotionAuthority = true;
    context.hasLastPosition = true;

    const Decision clientObserver = Evaluate(context);

    context.remoteTargetsLocalPlayer = true;
    const Decision strictClientTarget = Evaluate(context);

    context.localTargetMixEnabled = true;
    const Decision mixedClientTarget = Evaluate(context);

    context.remoteTargetsLocalPlayer = false;
    context.remoteTargetLocallyRepresented = true;
    const Decision proxyTarget = Evaluate(context);

    context.remoteTargetLocallyRepresented = false;
    context.networkMode = NetworkMode::Host;
    const Decision hostObserver = Evaluate(context);

    context = {};
    context.networkMode = NetworkMode::Client;
    const Decision clientLocal = Evaluate(context);

    context = {};
    context.networkMode = NetworkMode::Client;
    context.remoteLocomotionAuthority = true;
    context.hasLastPosition = true;
    context.localLeaseOwner = true;
    const Decision clientLeaseOwnerWithoutAttention = Evaluate(context);

    RemoteActionContext localTargetLunge;
    localTargetLunge.mode = EnemyControlMode::LocalTargetRemoteOwner;
    localTargetLunge.hasRemoteActionPacket = true;
    localTargetLunge.actionFlags =
        CoopProtocol::kEnemyLocomotionFlagLunging |
        CoopProtocol::kEnemyLocomotionFlagMannequinDriven;
    const RemoteActionDecision localTargetLungeDecision =
        EvaluateRemoteAction(localTargetLunge);

    RemoteActionContext localTargetDash = localTargetLunge;
    localTargetDash.actionFlags =
        CoopProtocol::kEnemyLocomotionFlagDashing |
        CoopProtocol::kEnemyLocomotionFlagMannequinDriven;
    const RemoteActionDecision localTargetDashDecision =
        EvaluateRemoteAction(localTargetDash);

    RemoteActionContext localTargetWalk = localTargetLunge;
    localTargetWalk.actionFlags =
        CoopProtocol::kEnemyLocomotionFlagWalking |
        CoopProtocol::kEnemyLocomotionFlagMannequinDriven;
    const RemoteActionDecision localTargetWalkDecision =
        EvaluateRemoteAction(localTargetWalk);

    RemoteActionContext localTargetAttack = localTargetLunge;
    localTargetAttack.actionFlags =
        CoopProtocol::kEnemyLocomotionFlagAttacking |
        CoopProtocol::kEnemyLocomotionFlagInCombat |
        CoopProtocol::kEnemyLocomotionFlagMannequinDriven;
    const RemoteActionDecision localTargetAttackWithoutLocalCombatDecision =
        EvaluateRemoteAction(localTargetAttack);
    localTargetAttack.localCombatActionActive = true;
    const RemoteActionDecision localTargetAttackWithLocalCombatDecision =
        EvaluateRemoteAction(localTargetAttack);

    RemoteActionContext localTargetAttackWhileWalking = localTargetAttack;
    localTargetAttackWhileWalking.actionFlags |=
        CoopProtocol::kEnemyLocomotionFlagWalking;
    const RemoteActionDecision localTargetAttackWhileWalkingDecision =
        EvaluateRemoteAction(localTargetAttackWhileWalking);

    RemoteMotionContext authorityAttackWalk;
    authorityAttackWalk.mode = EnemyControlMode::LocalTargetRemoteOwner;
    authorityAttackWalk.locomotionFlags =
        CoopProtocol::kEnemyLocomotionFlagWalking |
        CoopProtocol::kEnemyLocomotionFlagAttacking |
        CoopProtocol::kEnemyLocomotionFlagMannequinDriven;
    authorityAttackWalk.authorityPacketLocomotionFlags = authorityAttackWalk.locomotionFlags;
    authorityAttackWalk.mannequinSequence = 1;
    authorityAttackWalk.mannequinFragmentId = 49;
    authorityAttackWalk.packetSpeed = 1.5f;
    authorityAttackWalk.packetTargetStepLen = 0.08f;
    authorityAttackWalk.packetDeltaLen = 0.08f;
    authorityAttackWalk.tickSeconds = 0.05f;
    authorityAttackWalk.movementIntentSpeedThreshold = 0.1f;
    authorityAttackWalk.targetMotionInferenceMinStep = 0.05f;
    authorityAttackWalk.targetMotionInferenceMinSpeed = 0.85f;
    authorityAttackWalk.movementHoldSeconds = 0.4f;
    authorityAttackWalk.activeRemoteActionBlocksMotionInference = true;
    const RemoteMotionDecision authorityAttackWalkDecision =
        ResolveRemoteMotion(authorityAttackWalk);

    const bool ok =
        clientObserver.mode == EnemyControlMode::RemoteObserver &&
        clientObserver.blockMovement && clientObserver.blockAttack &&
        clientObserver.blockTurn && clientObserver.blockLook && clientObserver.blockFacing &&
        clientObserver.mirrorRemoteActions &&
        strictClientTarget.mode == EnemyControlMode::RemoteObserver &&
        strictClientTarget.blockMovement && strictClientTarget.blockAttack &&
        strictClientTarget.blockTurn && strictClientTarget.blockLook && strictClientTarget.blockFacing &&
        strictClientTarget.mirrorRemoteActions && !strictClientTarget.preserveLocalCombat &&
        mixedClientTarget.mode == EnemyControlMode::LocalTargetRemoteOwner &&
        mixedClientTarget.blockMovement && !mixedClientTarget.blockAttack &&
        !mixedClientTarget.blockTurn && !mixedClientTarget.blockLook && !mixedClientTarget.blockFacing &&
        mixedClientTarget.mirrorRemoteActions && mixedClientTarget.preserveLocalCombat &&
        proxyTarget.mode == EnemyControlMode::RemoteObserver &&
        proxyTarget.blockMovement && proxyTarget.blockAttack &&
        proxyTarget.blockTurn && proxyTarget.blockLook && proxyTarget.blockFacing &&
        proxyTarget.mirrorRemoteActions && !proxyTarget.preserveLocalCombat &&
        hostObserver.mode == EnemyControlMode::RemoteObserver &&
        hostObserver.blockMovement && hostObserver.blockAttack &&
        hostObserver.blockTurn && hostObserver.blockLook && hostObserver.blockFacing &&
        clientLocal.mode == EnemyControlMode::LocalOwner &&
        !clientLocal.BlocksAnyLocalVanilla() &&
        clientLeaseOwnerWithoutAttention.mode == EnemyControlMode::LocalOwner &&
        clientLeaseOwnerWithoutAttention.localVanillaAuthority &&
        !clientLeaseOwnerWithoutAttention.BlocksAnyLocalVanilla() &&
        FragmentNameIsAuthoredBurstMovement("mimic:SideStep") &&
        FragmentNameIsAuthoredBurstMovement("phantom:ShiftChargeAttack") &&
        !FragmentNameIsPhantomDash("mimic:SideStep") &&
        FragmentNameIsPhantomDash("phantom:ShiftChargeAttack") &&
        FragmentNameIsAuthoredMovementAction("operator:Strafe") &&
        !FragmentNameIsAuthoredMovementAction("mimic:Melee") &&
        !localTargetLungeDecision.suppressActionPacket &&
        !localTargetDashDecision.suppressActionPacket &&
        localTargetWalkDecision.suppressActionPacket &&
        localTargetAttackWithoutLocalCombatDecision.suppressActionPacket &&
        localTargetAttackWithLocalCombatDecision.suppressActionPacket &&
        localTargetAttackWhileWalkingDecision.suppressActionPacket &&
        !authorityAttackWalkDecision.activeActionBlockedMovementAnimation &&
        authorityAttackWalkDecision.currentMovementAnimationEvidence &&
        (authorityAttackWalkDecision.targetMotionFlags &
            CoopProtocol::kEnemyLocomotionFlagWalking) != 0;
    detail = ok ? "ok_6_modes_4_fragments_authority_movement_local_combat_action_walk" : "failed_attention_policy_matrix";
    return ok;
}

RemoteActionDecision EvaluateRemoteAction(const RemoteActionContext& context)
{
    RemoteActionDecision decision;
    const bool localTargetRemoteOwner = context.mode == EnemyControlMode::LocalTargetRemoteOwner;
    const bool remoteObserver = context.mode == EnemyControlMode::RemoteObserver;
    const uint32_t actionCarryFlags =
        (context.effectiveLocomotionFlags |
            context.remoteTargetMotionFlags |
            context.remoteVisualMotionFlags) &
        ActionOverlayCarryFlags();
    const bool passiveAuthorityMannequin =
        context.hasRemoteActionPacket &&
        remoteObserver &&
        IsPassiveMannequinFlags(context.actionFlags);
    const bool passiveAuthorityNonCarryAction =
        passiveAuthorityMannequin &&
        !context.authorityMannequinMovementCarry;
    decision.wantsMovement =
        HasMovementFlags(actionCarryFlags) &&
        !passiveAuthorityNonCarryAction;
    // Passive authority fragments such as WanderIdle can be the visible label
    // while the remote target stream is still carrying locomotion. In that case
    // the existing movement lane owns layer 0 for its idle grace window; starting
    // the passive fragment as another layer-0 action immediately stops the
    // movement lane and causes walk/idle thrash on observers.
    const bool passiveAuthorityCoveredByActiveMovement =
        passiveAuthorityMannequin &&
        context.authorityMannequinMovementCarry &&
        context.activeRemoteMovementPresent &&
        (
            context.remoteTargetMotionSeconds > 0.0f ||
            HasMovementFlags(context.remoteTargetMotionFlags));
    decision.hardOverride = HasHardOverrideFlags(context.actionFlags);
    decision.bodyLockAction = HasBodyLockFlags(context.actionFlags);
    decision.actionUsesFullBody = ActionOwnsFullBody(context.actionFlags);
    const bool localFocusSharedAuthorityAction =
        HasHardOverrideFlags(context.actionFlags) ||
        HasBodyLockFlags(context.actionFlags) ||
        (context.actionFlags & CoopProtocol::kEnemyLocomotionFlagHitReacting) != 0;
    if (localTargetRemoteOwner &&
        context.hasRemoteActionPacket &&
        !localFocusSharedAuthorityAction)
    {
        // The remote combat action occupies the same native Mannequin scope
        // that Vanilla needs in order to select the victim-local attack. Drop
        // only that competing presentation as soon as this process enters the
        // local-target lane. Authority position plus Walking/Running drives the
        // locomotion/leg lane; local Vanilla AI owns attack and facing. Exact
        // authority dashes, jumps/lunges, morphs, body locks and hit reactions
        // retain their native action because they move or lock the shared body.
        decision.suppressActionPacket = true;
        decision.suppressActionReason = "local_target_combat_owns_presentation";
    }

    decision.skipPassiveMovementCarryIdle = false;
    decision.skipPassiveAction =
        passiveAuthorityMannequin &&
        !context.mirrorPassiveAuthorityActions;
    decision.skipPassiveActionWhileMovement =
        passiveAuthorityCoveredByActiveMovement;

    decision.actionLaneFlags = context.actionFlags;
    if (!decision.actionUsesFullBody)
        decision.actionLaneFlags |= actionCarryFlags;

    decision.observerActionOwnsFullBody =
        context.hasRemoteActionPacket &&
        remoteObserver &&
        !passiveAuthorityMannequin &&
        decision.actionUsesFullBody;

    const bool activeRemoteActionPassive =
        IsPassiveMannequinFlags(context.activeRemoteActionFlags);
    const bool activeActionOwnsFullBody =
        context.activeRemoteActionPresent &&
        !activeRemoteActionPassive &&
        ActionOwnsFullBody(context.activeRemoteActionFlags);
    decision.observerFullBodyActionActive =
        remoteObserver &&
        activeActionOwnsFullBody &&
        (
            !decision.wantsMovement ||
            HasHardOverrideFlags(context.activeRemoteActionFlags) ||
            HasBodyLockFlags(context.activeRemoteActionFlags)) &&
        context.activeRemoteActionPriority >= 60 &&
        context.now < context.activeRemoteActionUntilTime;

    const bool remoteVisualMovementActive =
        context.remoteVisualMotionSeconds > 0.0f &&
        HasMovementFlags(context.remoteVisualMotionFlags);
    decision.allowMovementLane =
        !decision.observerActionOwnsFullBody &&
        !decision.observerFullBodyActionActive &&
        (decision.wantsMovement || remoteVisualMovementActive);
    decision.allowObserverIdleLane =
        !decision.observerActionOwnsFullBody &&
        !decision.observerFullBodyActionActive &&
        remoteObserver &&
        !decision.wantsMovement &&
        !decision.skipPassiveActionWhileMovement;

    if (decision.skipPassiveActionWhileMovement)
    {
        decision.actionLayer = 1;
        decision.actionStopsMovement = false;
    }
    else
    {
        decision.actionLayer =
            (remoteObserver && !passiveAuthorityMannequin && !decision.actionUsesFullBody)
                ? 1
                : ((remoteObserver || decision.actionUsesFullBody) ? 0 : 1);
        decision.actionStopsMovement = decision.actionLayer == 0;
    }

    decision.allowActionOnlyOverlayFallback =
        remoteObserver &&
        decision.wantsMovement &&
        context.activeRemoteMovementPresent &&
        !decision.actionStopsMovement &&
        !decision.hardOverride &&
        !decision.bodyLockAction;
    return decision;
}

RemoteMotionDecision ResolveRemoteMotion(const RemoteMotionContext& context)
{
    RemoteMotionDecision decision;
    decision.locomotionFlags = context.locomotionFlags;

    const bool localTargetRemoteOwner =
        context.mode == EnemyControlMode::LocalTargetRemoteOwner;
    const bool remoteObserver =
        context.mode == EnemyControlMode::RemoteObserver;
    const uint32_t continuousMovementFlags = ContinuousMovementFlags();
    const uint32_t movementFlags = MovementFlags();
    const uint32_t previousContinuousFlags = context.previousLocomotionFlags & continuousMovementFlags;
    const uint32_t hardMovementBlockingActionFlags =
        BodyLockFlags() |
        HardOverrideFlags() |
        CoopProtocol::kEnemyLocomotionFlagHitReacting;
    const bool hasAuthorityMannequin =
        context.mannequinSequence != 0 &&
        context.mannequinFragmentId >= 0;
    decision.explicitBurstEvent =
        context.explicitBurstEvent &&
        (decision.locomotionFlags & BurstMovementFlags()) != 0;
    // Burst/Shift is still an explicit action/movement event, but it must not
    // bypass the receiver transform smoother. Hard endpoint snaps make the
    // remote body fight CryAnimation/root-motion in mixed-attention cases.
    decision.snapBurstToAuthority = false;

    if (localTargetRemoteOwner)
    {
        // Mixed attention has one deliberately small locomotion contract:
        // authority Walking/Running owns the legs while the local AI owns
        // target, facing and attacks. An authority attack/idle fragment, an
        // ignored FX event, or receiver-side target drift must never invent or
        // suppress those leg bits. Position smoothing remains independent.
        const uint32_t authorityContinuousFlags =
            context.authorityPacketLocomotionFlags & continuousMovementFlags;
        decision.locomotionFlags &= ~continuousMovementFlags;
        decision.locomotionFlags |= authorityContinuousFlags;

        const float tick = std::max(context.tickSeconds, 0.001f);
        const float targetSpeed = context.packetTargetStepLen > 0.0001f
            ? context.packetTargetStepLen / tick
            : 0.0f;
        const float packetSpeed = std::isfinite(context.packetSpeed)
            ? std::max(0.0f, context.packetSpeed)
            : 0.0f;
        decision.inferredTargetSpeed = targetSpeed;
        decision.filteredTargetSpeed = std::clamp(
            std::max(packetSpeed, targetSpeed),
            0.0f,
            12.0f);
        decision.currentMovementAnimationEvidence = authorityContinuousFlags != 0;
        decision.motionSeconds = authorityContinuousFlags != 0
            ? context.movementHoldSeconds
            : 0.0f;
        decision.targetMotionFlags = authorityContinuousFlags;
        return decision;
    }

    const bool explicitAuthorityMovementBits =
        (context.authorityPacketLocomotionFlags & movementFlags) != 0;
    const bool activeActionBlockForbidsInferredMovement =
        context.activeRemoteActionBlocksMotionInference &&
        !explicitAuthorityMovementBits &&
        !context.authorityGlooed;
    if (activeActionBlockForbidsInferredMovement &&
        (decision.locomotionFlags & movementFlags) != 0)
    {
        decision.locomotionFlags &= ~movementFlags;
        decision.strippedUnconfirmedActionMovement = true;
    }

    const bool effectiveActionWithContinuousMovement =
        hasAuthorityMannequin &&
        (decision.locomotionFlags & ActionFlagsMask()) != 0 &&
        (decision.locomotionFlags & continuousMovementFlags) != 0 &&
        (decision.locomotionFlags & BurstMovementFlags()) == 0;
    const bool authorityActionWithoutExplicitMovement =
        hasAuthorityMannequin &&
        (decision.locomotionFlags & ActionFlagsMask()) != 0 &&
        (decision.locomotionFlags & (continuousMovementFlags | BurstMovementFlags())) == 0;
    const bool explicitAuthorityContinuousMotionEvidence =
        (context.authorityPacketLocomotionFlags & continuousMovementFlags) != 0 &&
        (
            (
                std::isfinite(context.packetSpeed) &&
                context.packetSpeed > context.movementIntentSpeedThreshold &&
                context.packetTargetStepLen >= context.targetMotionInferenceMinStep * 0.70f) ||
            context.packetTargetStepLen >=
                std::max(context.targetMotionInferenceMinStep * 0.70f, 0.035f));
    const bool actionMovementAnimationAllowed =
        !hasAuthorityMannequin ||
        context.authorityActionAllowsMovementAnimation ||
        explicitAuthorityContinuousMotionEvidence ||
        (decision.locomotionFlags & (HardOverrideFlags() | BurstMovementFlags())) != 0;
    const bool activeActionAllowsMovementAnimation =
        !context.activeRemoteActionBlocksMotionInference ||
        explicitAuthorityMovementBits ||
        (
            hasAuthorityMannequin &&
            context.authorityActionAllowsMovementAnimation) ||
        (decision.locomotionFlags & (HardOverrideFlags() | BurstMovementFlags())) != 0;
    decision.activeActionBlockedMovementAnimation =
        context.activeRemoteActionBlocksMotionInference &&
        !activeActionAllowsMovementAnimation;
    const bool remoteDrivenActionWithoutExplicitMovement =
        (localTargetRemoteOwner || context.mode == EnemyControlMode::RemoteObserver) &&
        authorityActionWithoutExplicitMovement;
    const bool receiverMustTreatRemoteActionAsStationary =
        remoteDrivenActionWithoutExplicitMovement &&
        (remoteObserver || localTargetRemoteOwner);
    const bool targetStepConfirmsContinuousMovement =
        context.packetTargetStepLen >= std::max(context.targetMotionInferenceMinStep * 2.5f, 0.12f) ||
        (
            context.packetTargetStepLen >= context.targetMotionInferenceMinStep &&
            context.packetDeltaLen > 0.10f);
    const bool packetConfirmsContinuousMovement =
        (
            std::isfinite(context.packetSpeed) &&
            context.packetSpeed > context.movementIntentSpeedThreshold &&
            context.packetTargetStepLen >= context.targetMotionInferenceMinStep) ||
        targetStepConfirmsContinuousMovement;
    const bool targetStepConfirmsAnimationMovement =
        context.packetTargetStepLen >= std::max(context.targetMotionInferenceMinStep * 0.70f, 0.035f);
    const bool explicitAuthorityActionMovement =
        effectiveActionWithContinuousMovement &&
        std::isfinite(context.packetSpeed) &&
        context.packetSpeed > context.movementIntentSpeedThreshold;
    if (effectiveActionWithContinuousMovement &&
        !packetConfirmsContinuousMovement &&
        !explicitAuthorityActionMovement)
    {
        decision.locomotionFlags &= ~continuousMovementFlags;
        decision.strippedUnconfirmedActionMovement = true;
    }

    bool passiveAuthorityMannequin =
        hasAuthorityMannequin &&
        IsPassiveMannequinFlags(decision.locomotionFlags);
    const bool heldPassiveAuthorityMannequin =
        context.heldAuthorityMannequin &&
        passiveAuthorityMannequin;
    if (heldPassiveAuthorityMannequin && (decision.locomotionFlags & movementFlags) != 0)
    {
        decision.locomotionFlags &= ~movementFlags;
        decision.strippedHeldPassiveMovement = true;
        passiveAuthorityMannequin = IsPassiveMannequinFlags(decision.locomotionFlags);
    }
    const bool passiveMannequinWithoutContinuousMovement =
        passiveAuthorityMannequin &&
        (decision.locomotionFlags & continuousMovementFlags) == 0;
    const bool passiveNonCarryAuthorityMannequin =
        passiveMannequinWithoutContinuousMovement &&
        !context.authorityMannequinMovementCarry;
    const float passiveInferredMaxWalkSpeed =
        std::isfinite(context.passiveInferredMaxWalkSpeed)
            ? std::max(0.1f, context.passiveInferredMaxWalkSpeed)
            : 1.8f;

    const float tick = std::max(context.tickSeconds, 0.001f);
    const float instantaneousTargetSpeed =
        context.packetTargetStepLen > 0.0001f
            ? context.packetTargetStepLen / tick
            : 0.0f;
    const float previousFilteredSpeed =
        std::isfinite(context.previousFilteredTargetSpeed)
            ? std::clamp(context.previousFilteredTargetSpeed, 0.0f, 12.0f)
            : 0.0f;
    const bool hasTargetStep = context.packetTargetStepLen > 0.006f;
    const float blend = hasTargetStep ? 0.45f : 0.18f;
    float filteredTargetSpeed = previousFilteredSpeed + (instantaneousTargetSpeed - previousFilteredSpeed) * blend;
    if (!hasTargetStep)
        filteredTargetSpeed *= 0.65f;
    if (!std::isfinite(filteredTargetSpeed))
        filteredTargetSpeed = 0.0f;
    decision.filteredTargetSpeed = std::clamp(filteredTargetSpeed, 0.0f, 12.0f);
    decision.inferredTargetSpeed = instantaneousTargetSpeed;
    if (passiveMannequinWithoutContinuousMovement)
        decision.filteredTargetSpeed = std::min(decision.filteredTargetSpeed, passiveInferredMaxWalkSpeed);

    const bool remoteActionMayInferTargetMotion =
        remoteDrivenActionWithoutExplicitMovement &&
        context.authorityMannequinMovementCarry;
    const bool remoteDrivenActionStrongTargetMotionEvidence =
        remoteDrivenActionWithoutExplicitMovement &&
        remoteActionMayInferTargetMotion &&
        !activeActionBlockForbidsInferredMovement &&
        localTargetRemoteOwner &&
        !context.authorityGlooed &&
        context.packetDeltaLen > std::max(context.targetMotionInferenceMinStep, 0.075f) &&
        (
            targetStepConfirmsContinuousMovement ||
            packetConfirmsContinuousMovement ||
            (
                context.packetTargetStepLen >= std::max(context.targetMotionInferenceMinStep * 2.0f, 0.10f) &&
                std::isfinite(instantaneousTargetSpeed) &&
                instantaneousTargetSpeed > context.targetMotionInferenceMinSpeed * 1.25f) ||
            (
                context.packetTargetStepLen >= std::max(context.targetMotionInferenceMinStep, 0.050f) &&
                context.packetDeltaLen > context.catchupIntentDistance * 0.75f &&
                decision.filteredTargetSpeed > context.targetMotionInferenceMinSpeed));
    decision.remoteActionTargetMotionEvidence = remoteDrivenActionStrongTargetMotionEvidence;

    const bool passiveNonCarryStrongTargetMotionEvidence =
        passiveNonCarryAuthorityMannequin &&
        !activeActionBlockForbidsInferredMovement &&
        !remoteObserver &&
        (
            targetStepConfirmsContinuousMovement ||
            (
                context.packetTargetStepLen >= std::max(context.targetMotionInferenceMinStep * 2.0f, 0.10f) &&
                std::isfinite(instantaneousTargetSpeed) &&
                instantaneousTargetSpeed > context.targetMotionInferenceMinSpeed) ||
            (
                context.packetTargetStepLen >= context.targetMotionInferenceMinStep &&
                context.packetDeltaLen > 0.10f &&
                std::isfinite(instantaneousTargetSpeed) &&
                instantaneousTargetSpeed > context.targetMotionInferenceMinSpeed * 0.75f) ||
            (
                context.packetTargetStepLen >= std::max(context.targetMotionInferenceMinStep, 0.035f) &&
                context.packetDeltaLen > context.catchupIntentDistance * 0.75f &&
                decision.filteredTargetSpeed > context.targetMotionInferenceMinSpeed * 0.65f));
    const bool passiveNonCarryBlocksTargetMotion =
        passiveNonCarryAuthorityMannequin &&
        !passiveNonCarryStrongTargetMotionEvidence;
    decision.passiveNonCarryActionMotionBlocked = passiveNonCarryBlocksTargetMotion;

    const bool passiveTargetStepMovementEvidence =
        (!passiveNonCarryAuthorityMannequin || passiveNonCarryStrongTargetMotionEvidence) &&
        (
            targetStepConfirmsContinuousMovement ||
            (
                passiveMannequinWithoutContinuousMovement &&
                context.packetTargetStepLen >= context.targetMotionInferenceMinStep &&
                context.packetDeltaLen > 0.065f) ||
            (
                std::isfinite(context.packetSpeed) &&
                context.packetSpeed > context.movementIntentSpeedThreshold &&
                context.packetTargetStepLen >= context.targetMotionInferenceMinStep &&
                context.packetDeltaLen > 0.10f));
    const float previousMovementHoldStepThreshold =
        std::max(context.targetMotionInferenceMinStep * 1.6f, 0.08f);
    const bool previousMovementHoldResidualEvidence =
        context.packetTargetStepLen >= previousMovementHoldStepThreshold ||
        (
            context.packetTargetStepLen >= context.targetMotionInferenceMinStep &&
            context.packetDeltaLen > context.catchupIntentDistance * 0.50f);
    const bool previousMovementHoldSpeedEvidence =
        std::isfinite(instantaneousTargetSpeed) &&
        (
            instantaneousTargetSpeed > context.targetMotionInferenceMinSpeed * 0.50f ||
                decision.filteredTargetSpeed > context.targetMotionInferenceMinSpeed * 0.65f);
    const bool previousMovementHoldCatchupEvidence =
        context.packetDeltaLen > context.catchupIntentDistance * 0.50f &&
        decision.filteredTargetSpeed > context.targetMotionInferenceMinSpeed * 0.45f;
    const bool actionCanCarryPreviousMovement =
        authorityActionWithoutExplicitMovement &&
        context.authorityMannequinMovementCarry &&
        (decision.locomotionFlags & CoopProtocol::kEnemyLocomotionFlagAttacking) != 0 &&
        (decision.locomotionFlags & hardMovementBlockingActionFlags) == 0;
    const bool actionPreviousMovementGrace =
        actionCanCarryPreviousMovement &&
        context.previousMotionSeconds > 0.0f &&
        previousContinuousFlags != 0;
    const bool previousMovementHoldEvidence =
        !activeActionBlockForbidsInferredMovement &&
        context.previousMotionSeconds > 0.0f &&
        previousContinuousFlags != 0 &&
        (
            (
                (previousMovementHoldResidualEvidence || previousMovementHoldCatchupEvidence) &&
                previousMovementHoldSpeedEvidence) ||
            actionPreviousMovementGrace);
    const bool passiveActionMovementHoldEvidence =
        passiveMannequinWithoutContinuousMovement &&
        (!passiveNonCarryAuthorityMannequin || passiveNonCarryStrongTargetMotionEvidence) &&
        previousMovementHoldEvidence;
    decision.passiveActionMovementHoldEvidence = passiveActionMovementHoldEvidence;
    const bool passiveActionStrongMovementEvidence =
        passiveTargetStepMovementEvidence ||
        passiveActionMovementHoldEvidence;
    decision.blockedPassiveDriftOnlyMotion =
        passiveMannequinWithoutContinuousMovement &&
        context.packetDeltaLen > context.catchupIntentDistance &&
        !passiveTargetStepMovementEvidence;
    decision.heldPassiveMovementEvidence =
        heldPassiveAuthorityMannequin &&
        passiveTargetStepMovementEvidence;

    const bool remoteActionTargetStepMovementEvidence =
        !activeActionBlockForbidsInferredMovement &&
        (!receiverMustTreatRemoteActionAsStationary ||
            remoteDrivenActionStrongTargetMotionEvidence) &&
        remoteDrivenActionWithoutExplicitMovement &&
        remoteActionMayInferTargetMotion &&
        context.packetTargetStepLen >= std::max(context.targetMotionInferenceMinStep * 1.5f, 0.075f) &&
        std::isfinite(instantaneousTargetSpeed) &&
        instantaneousTargetSpeed > context.targetMotionInferenceMinSpeed &&
        (
            context.packetDeltaLen > std::max(context.targetMotionInferenceMinStep * 1.2f, 0.060f) ||
            decision.filteredTargetSpeed > context.targetMotionInferenceMinSpeed * 0.65f);
    const bool actionTargetStepMovementEvidence =
        (
            !receiverMustTreatRemoteActionAsStationary &&
            (
                targetStepConfirmsContinuousMovement ||
                packetConfirmsContinuousMovement ||
                remoteActionTargetStepMovementEvidence)) ||
        passiveActionStrongMovementEvidence ||
        passiveActionMovementHoldEvidence;
    // This weak action-only motion hint is only for the mixed local-focus lane:
    // the local target keeps combat/turning while the remote owner supplies
    // locomotion. Pure observers must derive movement from confirmed/inferred
    // authority target motion; otherwise an action packet can keep a stale
    // walk lane alive and root-move the mirror away from the owner.
    decision.localTargetActionMotionEvidence =
        localTargetRemoteOwner &&
        remoteDrivenActionWithoutExplicitMovement &&
        !activeActionBlockForbidsInferredMovement &&
        actionTargetStepMovementEvidence;
    if (decision.localTargetActionMotionEvidence)
    {
        const float actionMotionSpeed = std::max(instantaneousTargetSpeed, decision.filteredTargetSpeed);
        if (std::isfinite(actionMotionSpeed) && actionMotionSpeed > context.movementIntentSpeedThreshold)
        {
            decision.actionTargetMotionSpeed = actionMotionSpeed;
            decision.actionTargetMotionFlags =
                actionMotionSpeed > context.receiverInferRunSpeed
                    ? CoopProtocol::kEnemyLocomotionFlagRunning
                    : CoopProtocol::kEnemyLocomotionFlagWalking;
        }
    }

    decision.targetMotionInferenceBlockedByAction =
        activeActionBlockForbidsInferredMovement ||
        (hasAuthorityMannequin &&
            (
            (receiverMustTreatRemoteActionAsStationary &&
                !remoteDrivenActionStrongTargetMotionEvidence) ||
            (remoteDrivenActionWithoutExplicitMovement &&
                !actionTargetStepMovementEvidence &&
                !remoteDrivenActionStrongTargetMotionEvidence) ||
            (decision.locomotionFlags & hardMovementBlockingActionFlags) != 0 ||
            (context.activeRemoteActionBlocksMotionInference &&
                authorityActionWithoutExplicitMovement &&
                !actionTargetStepMovementEvidence &&
                !remoteDrivenActionStrongTargetMotionEvidence) ||
            (heldPassiveAuthorityMannequin && !decision.heldPassiveMovementEvidence) ||
            passiveNonCarryBlocksTargetMotion ||
            (passiveMannequinWithoutContinuousMovement && !passiveActionStrongMovementEvidence)));
    if (context.activeRemoteActionBlocksMotionInference &&
        (decision.locomotionFlags & continuousMovementFlags) == 0 &&
        !actionTargetStepMovementEvidence &&
        !previousMovementHoldEvidence &&
        !remoteDrivenActionStrongTargetMotionEvidence)
    {
        decision.targetMotionInferenceBlockedByAction = true;
    }
    if (decision.targetMotionInferenceBlockedByAction &&
        (!passiveActionMovementHoldEvidence || activeActionBlockForbidsInferredMovement))
    {
        decision.filteredTargetSpeed = 0.0f;
    }

    if (!context.inferenceDisabled &&
        !context.authorityGlooed &&
        !decision.targetMotionInferenceBlockedByAction &&
        (decision.locomotionFlags & movementFlags) == 0 &&
        context.hasExistingState &&
        context.existingRemoteLocomotionAuthority)
    {
        const bool targetMoved =
            context.packetTargetStepLen >= context.targetMotionInferenceMinStep &&
            std::isfinite(instantaneousTargetSpeed) &&
            instantaneousTargetSpeed > context.targetMotionInferenceMinSpeed &&
            (
                decision.filteredTargetSpeed > context.targetMotionInferenceMinSpeed ||
                passiveActionStrongMovementEvidence);
        decision.confirmedTargetMotion = targetMoved;
        const bool residualMotionEvidence =
            heldPassiveAuthorityMannequin
                ? decision.heldPassiveMovementEvidence
                : passiveMannequinWithoutContinuousMovement
                ? passiveActionStrongMovementEvidence
                : (previousMovementHoldResidualEvidence ||
                    previousMovementHoldCatchupEvidence ||
                    actionPreviousMovementGrace);
        const bool mayHoldPreviousMovement =
            context.previousMotionSeconds > 0.0f &&
            previousContinuousFlags != 0 &&
            residualMotionEvidence;
        const bool needsCatchupWalk =
            context.packetDeltaLen > context.catchupIntentDistance &&
            (targetMoved || mayHoldPreviousMovement);

        if (targetMoved || needsCatchupWalk)
        {
            decision.inferredCatchupDrift = needsCatchupWalk && !targetMoved;
            const float authorityMotionSpeed = std::max(decision.filteredTargetSpeed, instantaneousTargetSpeed);
            if (targetMoved || previousContinuousFlags == 0)
            {
                decision.inferredMovementFlags =
                    authorityMotionSpeed > context.receiverInferRunSpeed
                        ? CoopProtocol::kEnemyLocomotionFlagRunning
                        : CoopProtocol::kEnemyLocomotionFlagWalking;
            }
            else
            {
                decision.inferredMovementFlags = previousContinuousFlags;
                decision.heldPreviousMovement = true;
            }
        }
    }

    if (decision.inferredMovementFlags != 0)
    {
        decision.locomotionFlags |= decision.inferredMovementFlags;
    }
    decision.currentMovementAnimationEvidence =
        actionMovementAnimationAllowed &&
        activeActionAllowsMovementAnimation &&
        (
            decision.explicitBurstEvent ||
            targetStepConfirmsAnimationMovement ||
            decision.confirmedTargetMotion ||
            (
                decision.heldPreviousMovement &&
                targetStepConfirmsAnimationMovement));

    const bool hasContinuousMovement = (decision.locomotionFlags & continuousMovementFlags) != 0;
    if (context.authorityGlooed)
    {
        decision.motionSeconds = 0.0f;
        decision.targetMotionFlags = 0;
        decision.filteredTargetSpeed = 0.0f;
    }
    else if (hasContinuousMovement && decision.currentMovementAnimationEvidence)
    {
        decision.motionSeconds = context.movementHoldSeconds;
        decision.targetMotionFlags = decision.locomotionFlags & continuousMovementFlags;
    }
    else if (hasContinuousMovement)
    {
        // Keep the pose/transform correction, but do not keep a movement
        // animation lane alive from catch-up drift alone.
        decision.motionSeconds = 0.0f;
        decision.targetMotionFlags = 0;
        decision.movementHoldSuppressedByMissingAnimationEvidence = true;
    }
    else
    {
        decision.motionSeconds = std::max(0.0f, context.previousMotionSeconds - tick);
        const bool residualMotionEvidence =
            heldPassiveAuthorityMannequin
                ? decision.heldPassiveMovementEvidence
                : passiveMannequinWithoutContinuousMovement
                ? passiveActionStrongMovementEvidence
                : (previousMovementHoldResidualEvidence ||
                    previousMovementHoldCatchupEvidence ||
                    actionPreviousMovementGrace);
        const bool wouldHoldPreviousContinuousMovement =
            decision.motionSeconds > 0.0f &&
            previousContinuousFlags != 0 &&
            residualMotionEvidence;
        const bool holdPreviousContinuousMovement =
            wouldHoldPreviousContinuousMovement &&
            !decision.targetMotionInferenceBlockedByAction;
        if (holdPreviousContinuousMovement)
        {
            decision.locomotionFlags |= previousContinuousFlags;
            decision.targetMotionFlags = previousContinuousFlags;
            decision.heldPreviousMovement = true;
        }
        else
        {
            decision.targetMotionFlags = 0;
            decision.blockedPreviousMovementHold =
                wouldHoldPreviousContinuousMovement &&
                decision.targetMotionInferenceBlockedByAction;
        }
        if (!holdPreviousContinuousMovement && decision.motionSeconds <= 0.0f)
            decision.filteredTargetSpeed = 0.0f;
    }

    return decision;
}

bool AllowsLocalVanillaControl(const Context& context)
{
    return Evaluate(context).localVanillaAuthority;
}

bool IsRemoteDriven(const Context& context)
{
    return Evaluate(context).remoteDriven;
}

bool BlocksLocalVanillaControl(const Context& context)
{
    return Evaluate(context).BlocksAnyLocalVanilla();
}

bool BlocksLocalVanillaControlIntent(const Context& context, Intent intent)
{
    const Decision decision = Evaluate(context);

    switch (intent)
    {
    case Intent::Movement:
        return decision.blockMovement;
    case Intent::Turn:
        return decision.blockTurn;
    case Intent::Look:
        return decision.blockLook;
    case Intent::Facing:
        return decision.blockFacing;
    case Intent::Attack:
        return decision.blockAttack;
    case Intent::Any:
    default:
        return decision.BlocksAnyLocalVanilla();
    }
}
}
