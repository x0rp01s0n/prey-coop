#include "CoopEnemyIntentGate.h"

#include "ModMain.h"
#include "CoopProtocol.h"
#include "CoopRuntimeConfig.h"
#include "CoopRuntimeGuards.h"

#include <algorithm>
#include <string_view>

#include <EntityUtils.h>
#include <Prey/GameDll/ark/npc/ArkNpc.h>
#include <Prey/GameDll/ark/player/ArkPlayer.h>

using CoopRuntimeGuards::TryGuardedCall;
using CoopRuntimeGuards::TryGuardedVoidCall;

namespace CoopEnemyIntentGate
{
namespace
{
uint32_t MovementMask()
{
    return CoopProtocol::kEnemyLocomotionFlagWalking |
        CoopProtocol::kEnemyLocomotionFlagRunning |
        CoopProtocol::kEnemyLocomotionFlagDashing |
        CoopProtocol::kEnemyLocomotionFlagShifting |
        CoopProtocol::kEnemyLocomotionFlagMorphing |
        CoopProtocol::kEnemyLocomotionFlagLunging |
        CoopProtocol::kEnemyLocomotionFlagMannequinDriven;
}

uint32_t PhysicalMovementMask()
{
    return CoopProtocol::kEnemyLocomotionFlagWalking |
        CoopProtocol::kEnemyLocomotionFlagRunning |
        CoopProtocol::kEnemyLocomotionFlagDashing |
        CoopProtocol::kEnemyLocomotionFlagShifting |
        CoopProtocol::kEnemyLocomotionFlagMorphing |
        CoopProtocol::kEnemyLocomotionFlagLunging;
}

uint32_t HardOverrideMask()
{
    return CoopProtocol::kEnemyLocomotionFlagGlooed |
        CoopProtocol::kEnemyLocomotionFlagDashing |
        CoopProtocol::kEnemyLocomotionFlagShifting |
        CoopProtocol::kEnemyLocomotionFlagMorphing |
        CoopProtocol::kEnemyLocomotionFlagLunging |
        CoopProtocol::kEnemyLocomotionFlagStunned |
        CoopProtocol::kEnemyLocomotionFlagCowering |
        CoopProtocol::kEnemyLocomotionFlagRagdolled;
}

uint32_t LocalCombatMask()
{
    return CoopProtocol::kEnemyLocomotionFlagAttacking |
        CoopProtocol::kEnemyLocomotionFlagHitReacting |
        CoopProtocol::kEnemyLocomotionFlagStunned |
        CoopProtocol::kEnemyLocomotionFlagCowering |
        CoopProtocol::kEnemyLocomotionFlagRagdolled;
}

bool EnvEnabled(const char* name)
{
    return CoopRuntimeConfig::Flag(name);
}

bool Contains(std::string_view haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string_view::npos;
}

IntentKind ClassifyKindFromFlags(uint32_t flags)
{
    if ((flags & CoopProtocol::kEnemyLocomotionFlagGlooed) != 0)
        return IntentKind::Gloo;
    if ((flags & (CoopProtocol::kEnemyLocomotionFlagDashing | CoopProtocol::kEnemyLocomotionFlagShifting)) != 0)
        return IntentKind::Shift;
    if ((flags & CoopProtocol::kEnemyLocomotionFlagMorphing) != 0)
        return IntentKind::Morph;
    if ((flags & CoopProtocol::kEnemyLocomotionFlagTurning) != 0)
        return IntentKind::Turn;
    if ((flags & CoopProtocol::kEnemyLocomotionFlagAttacking) != 0)
        return IntentKind::Attack;
    if ((flags & (CoopProtocol::kEnemyLocomotionFlagStunned | CoopProtocol::kEnemyLocomotionFlagCowering)) != 0)
        return IntentKind::HardState;
    if ((flags & MovementMask()) != 0)
        return IntentKind::Movement;
    return IntentKind::Unknown;
}

bool IsMovementIntent(IntentKind kind, uint32_t flags)
{
    return kind == IntentKind::Movement ||
        kind == IntentKind::Shift ||
        kind == IntentKind::Morph ||
        (flags & PhysicalMovementMask()) != 0 ||
        (
            (flags & CoopProtocol::kEnemyLocomotionFlagMannequinDriven) != 0 &&
            (flags & LocalCombatMask()) == 0);
}

bool IsLocalCombatIntent(IntentKind kind, uint32_t flags)
{
    return kind == IntentKind::Attack ||
        kind == IntentKind::Ability ||
        (flags & CoopProtocol::kEnemyLocomotionFlagAttacking) != 0;
}
}

uint32_t ClassifyAbilityContextFlags(uint64_t contextId)
{
    // IDs come from Ark/Npc/NpcAbilityContexts.xml. These contexts start
    // authored displacement before ArkNpcMovementDesireManager sees a path,
    // so a non-authority peer must reject them at the ability boundary too.
    switch (contextId)
    {
    case 110: // Mimic_Sidestep_DodgeAim
    case 112: // Mimic_Sidestep_From_Stagger
    case 113: // Mimic_Sidestep
        return CoopProtocol::kEnemyLocomotionFlagLunging |
            CoopProtocol::kEnemyLocomotionFlagMannequinDriven;

    case 1005: // MilitaryOperator_Strafe_NoLos
    case 1006: // MilitaryOperator_Strafe_Los
        return CoopProtocol::kEnemyLocomotionFlagWalking |
            CoopProtocol::kEnemyLocomotionFlagMannequinDriven;

    case 5: // Phantom_HitReactShift
        return CoopProtocol::kEnemyLocomotionFlagDashing |
            CoopProtocol::kEnemyLocomotionFlagShifting |
            CoopProtocol::kEnemyLocomotionFlagHitReacting |
            CoopProtocol::kEnemyLocomotionFlagMannequinDriven;

    case 21: // Phantom_Shift_Charge_Attack
    case 22: // ShiftChargeAttack_OutOfLurk
        return CoopProtocol::kEnemyLocomotionFlagDashing |
            CoopProtocol::kEnemyLocomotionFlagShifting |
            CoopProtocol::kEnemyLocomotionFlagAttacking |
            CoopProtocol::kEnemyLocomotionFlagMannequinDriven;

    case 109: // MimicMimic_JumpAttack
    case 111: // Mimic_GrabAttack
    case 121: // Mimic_JumpAttackFromMimicry
    case 122: // Mimic_JumpAttack_Unreachable
    case 1002: // MilitaryOperator_Ram
        return CoopProtocol::kEnemyLocomotionFlagLunging |
            CoopProtocol::kEnemyLocomotionFlagAttacking |
            CoopProtocol::kEnemyLocomotionFlagMannequinDriven;

    default:
        return 0;
    }
}

AbilityOwnership ClassifyAbilityOwnership(uint64_t contextId)
{
    switch (contextId)
    {
    case 5:   // Phantom_HitReactShift
    case 110: // Mimic_Sidestep_DodgeAim
    case 112: // Mimic_Sidestep_From_Stagger
    case 113: // Mimic_Sidestep
        return AbilityOwnership::AuthorityMovement;

    case 21:   // Phantom_Shift_Charge_Attack
    case 22:   // ShiftChargeAttack_OutOfLurk
    case 109:  // MimicMimic_JumpAttack
    case 111:  // Mimic_GrabAttack
    case 121:  // Mimic_JumpAttackFromMimicry
    case 122:  // Mimic_JumpAttack_Unreachable
    case 1002: // MilitaryOperator_Ram
    case 1005: // MilitaryOperator_Strafe_NoLos
    case 1006: // MilitaryOperator_Strafe_Los
        return AbilityOwnership::AuthorityMovementLocalCombat;

    case 6:
    case 8:
    case 18:
    case 19:
    case 20:
    case 23:
    case 24:
    case 25:
    case 27:
    case 105:
    case 106:
    case 107:
    case 108:
    // Mimic taunts are target-local combat presentation. Treating them as
    // shared world state makes the observer AI retry the same blocked taunt
    // forever and starves its native melee/jump-attack selection.
    case 114:
    case 120:
    case 1004:
    case 1100:
    case 1200:
    case 10001:
    case 10003:
    case 10004:
    case 10005:
    case 10006:
    case 10007:
    case 10009:
    case 100000:
    case 100002:
    case 100003:
    case 100004:
    case 100005:
    case 100006:
    case 100101:
    case 100103:
    case 100109:
    case 100113:
        return AbilityOwnership::LocalCombat;

    case 4:
    case 13:
    case 15:
    case 26:
    case 10106:
    case 10107:
    case 10108:
    case 10109:
    case 100100:
    case 100104:
    case 100105:
    case 100106:
    case 100108:
    case 100110:
    case 100111:
    case 100112:
    case 100114:
    case 100115:
    case 1000000:
    case 1000002:
        return AbilityOwnership::LocalCombat;

    case 7:
    case 9:
    case 12:
    case 16:
    case 17:
    case 28:
    case 29:
    case 103:
    case 2000:
    case 2001:
    case 2002:
    case 2003:
    case 10008:
    case 100001:
    case 100007:
    case 100008:
    case 100102:
    case 100107:
    case 100200:
    case 100201:
    case 100250:
    case 100251:
    case 100252:
    case 100253:
    case 100254:
    case 100255:
    case 100256:
    case 100257:
    case 1000001:
    case 1000100:
        return AbilityOwnership::AuthorityWorldState;

    default:
        return AbilityOwnership::Unknown;
    }
}

bool IsAuthorityLocomotionAbilityContext(uint64_t contextId)
{
    return (ClassifyAbilityContextFlags(contextId) & PhysicalMovementMask()) != 0;
}

bool IsConsumableLocalAuthorityDecisionContext(uint64_t contextId)
{
    // Every authored displacement context belongs to the locomotion authority.
    // On an attentive observer, complete it at the ability boundary so Vanilla
    // can record its normal cooldown without creating a second body action.
    // The caller separately reports pure positioning branches as completed and
    // mixed attack/movement branches as unavailable.
    // Mimic taunts are also full-body, non-damaging actions. Their local
    // prerequisites and cooldown remain Vanilla, but their body presentation
    // belongs to the authority. Consuming them as completed prevents a blocked
    // Taunt action from parking the observer AI before its next real attack.
    // Etheric Doppelganger is shared world state rather than local combat. It
    // needs the same prerequisite/cooldown-only treatment: rejecting context
    // 12 before Vanilla records completion makes the observer retry it forever
    // and starves its target-local Psi/ranged attacks.
    return IsAuthorityLocomotionAbilityContext(contextId) ||
        contextId == 114 ||
        contextId == 120 ||
        contextId == CoopProtocol::kEnemyAbilityContextEthericDoppelganger;
}

bool ShouldCompleteConsumedLocalAuthorityDecision(uint64_t contextId)
{
    // A pure positioning decision such as Mimic SideStep belongs entirely to
    // the locomotion owner. Report the observer copy as completed so its AI
    // leaves that branch and may select a stationary local attack. Mixed
    // movement/attack abilities remain unavailable after their root action is
    // consumed; they must not claim that a local attack actually happened.
    return ClassifyAbilityOwnership(contextId) == AbilityOwnership::AuthorityMovement ||
        contextId == 114 ||
        contextId == 120 ||
        contextId == CoopProtocol::kEnemyAbilityContextEthericDoppelganger;
}

uint32_t ClassifyIntentFlags(const char* stage, uint64_t contextId, uint32_t semanticFlags)
{
    uint32_t flags = semanticFlags;
    const std::string_view stageView(stage && stage[0] ? stage : "");
    const uint32_t abilityContextFlags = ClassifyAbilityContextFlags(contextId);
    flags |= abilityContextFlags;
    const bool authorityLocomotionAbility = (abilityContextFlags & PhysicalMovementMask()) != 0;

    if (Contains(stageView, "OnAttack"))
        flags |= CoopProtocol::kEnemyLocomotionFlagAttacking;
    if (!authorityLocomotionAbility &&
        (Contains(stageView, "OnUsePower") ||
            Contains(stageView, "TryPerformAbilityContext") ||
            Contains(stageView, "TryEvaluateAndPerformAbilityContext") ||
            Contains(stageView, "ArkNpcAbility::Perform")))
    {
        flags |= CoopProtocol::kEnemyLocomotionFlagAttacking;
    }
    if (Contains(stageView, "AnimatedMovement") ||
        Contains(stageView, "TryToRequestMovement") ||
        Contains(stageView, "TryToResumeMovement"))
    {
        flags |= CoopProtocol::kEnemyLocomotionFlagMannequinDriven;
    }
    if (Contains(stageView, "BeginAnimatedDistraction") ||
        Contains(stageView, "PerformPatrolIdle"))
    {
        flags |= CoopProtocol::kEnemyLocomotionFlagTurning |
            CoopProtocol::kEnemyLocomotionFlagMannequinDriven;
    }
    if (Contains(stageView, "Shift"))
    {
        flags |= CoopProtocol::kEnemyLocomotionFlagDashing |
            CoopProtocol::kEnemyLocomotionFlagShifting |
            CoopProtocol::kEnemyLocomotionFlagMannequinDriven;
    }
    if (Contains(stageView, "Mimic"))
        flags |= CoopProtocol::kEnemyLocomotionFlagMorphing;
    if (Contains(stageView, "Gloo"))
        flags |= CoopProtocol::kEnemyLocomotionFlagGlooed;

    if (!authorityLocomotionAbility && contextId != 0 &&
        (Contains(stageView, "Ability") || Contains(stageView, "PerformAbility")))
    {
        flags |= CoopProtocol::kEnemyLocomotionFlagAttacking;
    }

    return flags;
}

const char* IntentKindName(IntentKind kind)
{
    switch (kind)
    {
    case IntentKind::Movement:
        return "movement";
    case IntentKind::Turn:
        return "turn";
    case IntentKind::Attack:
        return "attack";
    case IntentKind::Ability:
        return "ability";
    case IntentKind::Shift:
        return "shift";
    case IntentKind::Morph:
        return "morph";
    case IntentKind::Gloo:
        return "gloo";
    case IntentKind::HardState:
        return "hard";
    default:
        return "unknown";
    }
}

Result EvaluateRemoteIntent(const RemoteIntentContext& context, const char* stage, uint64_t contextId)
{
    Result result;
    const std::string_view stageView(stage && stage[0] ? stage : "");
    if (EnvEnabled("COOP_DISABLE_REMOTE_ENEMY_INTENT_GATE") || !context.remoteDriven)
        return result;

    result.kind = ClassifyKindFromFlags(context.localIntentFlags);
    if (result.kind == IntentKind::Unknown &&
        (Contains(stageView, "Ability") || Contains(stageView, "PerformAbility") || contextId != 0))
        result.kind = IntentKind::Ability;

    if (result.kind == IntentKind::Unknown || result.kind == IntentKind::Gloo)
    {
        result.decision = Decision::AllowNative;
        result.detail =
            "allow kind=" + std::string(IntentKindName(result.kind)) +
            " stage=" + std::string(stageView) +
            " reason=unmanaged_or_gloo";
        return result;
    }

    const uint32_t remoteFlags =
        context.remoteMannequinFlags ? context.remoteMannequinFlags : context.remoteLocomotionFlags;
    const bool remoteHardOverride = (remoteFlags & HardOverrideMask()) != 0;
    const bool localMovementIntent = IsMovementIntent(result.kind, context.localIntentFlags);
    const bool localCombatIntent = IsLocalCombatIntent(result.kind, context.localIntentFlags);
    const AbilityOwnership abilityOwnership = ClassifyAbilityOwnership(contextId);
    bool block = false;
    const char* reason = "allow";
    if (context.localHasAttention)
    {
        if (abilityOwnership == AbilityOwnership::AuthorityWorldState)
        {
            block = true;
            reason = "local_focus_preserves_authority_world_state";
        }
        else if (abilityOwnership == AbilityOwnership::AuthorityMovement)
        {
            block = true;
            reason = "local_focus_uses_authority_movement_ability";
        }
        else if (result.kind == IntentKind::HardState)
        {
            block = true;
            reason = "local_focus_uses_authority_shared_body_state";
        }
        else if (abilityOwnership == AbilityOwnership::AuthorityMovementLocalCombat &&
            localCombatIntent &&
            !context.localAttackBlocked)
        {
            // Run the native attack/targeting portion. Movement requests and
            // root displacement are still rejected by the movement and
            // transform gates, so the lease owner remains the only locomotion
            // source for mixed abilities such as JumpAttack, Ram and Strafe.
            block = false;
            reason = "local_focus_preserves_mixed_ability_combat_only";
        }
        else if (localCombatIntent && !context.localAttackBlocked)
        {
            block = false;
            reason = "local_focus_preserves_native_combat";
        }
        else if (result.kind == IntentKind::Turn && !context.localTurnBlocked)
        {
            block = false;
            reason = "local_focus_preserves_native_turn";
        }
        else if (context.localMovementBlocked && localMovementIntent)
        {
            block = true;
            reason = context.authorityMovementIntent
                ? "local_focus_without_authority_uses_authority_movement"
                : "local_focus_without_authority_blocks_local_movement";
        }
        else if (remoteHardOverride && localMovementIntent)
        {
            block = true;
            reason = "local_focus_remote_hard_blocks_local_movement";
        }
        else if (localMovementIntent)
        {
            block = true;
            reason = context.authorityMovementIntent
                ? "local_focus_authority_replaces_local_movement"
                : "local_focus_authority_idle_blocks_local_movement";
        }
        else if (result.kind == IntentKind::Turn || result.kind == IntentKind::Ability)
        {
            block = false;
            reason = "local_focus_preserves_native_turn_and_combat";
        }
    }
    else
    {
        if (localMovementIntent)
        {
            block = true;
            reason = context.authorityMovementIntent
                ? "observer_authority_replaces_local_movement"
                : "observer_authority_idle_blocks_local_movement";
        }
        else if (result.kind == IntentKind::Turn)
        {
            block = true;
            reason = "observer_blocks_local_turn";
        }
        else if (result.kind == IntentKind::HardState)
        {
            block = true;
            reason = "observer_uses_authority_shared_body_state";
        }
        else if (localCombatIntent || result.kind == IntentKind::Ability)
        {
            block = true;
            reason = "observer_blocks_local_attack_or_ability";
        }
    }

    result.decision = block ? Decision::BlockNative : Decision::AllowNative;
    result.detail =
        std::string(block ? "block" : "allow") +
        " kind=" + IntentKindName(result.kind) +
        " stage=" + std::string(stageView) +
        " net=" + std::to_string(context.enemyNetId) +
        " entity=" + std::to_string(context.entityId) +
        " localAttention=" + std::to_string(context.localHasAttention ? 1 : 0) +
        " localVanillaBlocked=" + std::to_string(context.localVanillaControlBlocked ? 1 : 0) +
        " localMoveBlocked=" + std::to_string(context.localMovementBlocked ? 1 : 0) +
        " localTurnBlocked=" + std::to_string(context.localTurnBlocked ? 1 : 0) +
        " localAttackBlocked=" + std::to_string(context.localAttackBlocked ? 1 : 0) +
        " authorityMove=" + std::to_string(context.authorityMovementIntent ? 1 : 0) +
        " localFlags=" + std::to_string(context.localIntentFlags) +
        " remoteFlags=" + std::to_string(remoteFlags) +
        " reason=" + reason;
    return result;
}

bool RunSelfTest(std::string& detail)
{
    RemoteIntentContext context;
    context.entityId = 7;
    context.enemyNetId = 11;
    context.remoteDriven = true;
    context.localHasAttention = true;
    context.localVanillaControlBlocked = true;
    context.localMovementBlocked = true;
    context.localTurnBlocked = false;
    context.localAttackBlocked = false;
    context.authorityMovementIntent = true;
    context.mirrorAuthorityPresentation = true;
    context.authorityActionActive = true;

    const uint32_t sideStepFlags = ClassifyIntentFlags("ArkNpc::TryPerformAbilityContext0", 113, 0);
    context.remoteMannequinFlags = sideStepFlags;
    context.localIntentFlags = sideStepFlags;
    const Result sideStep = EvaluateRemoteIntent(context, "ArkNpc::TryPerformAbilityContext0", 113);

    context.localIntentFlags = ClassifyIntentFlags("ArkNpc::TryPerformAbilityContext0", 1005, 0);
    const Result strafe = EvaluateRemoteIntent(context, "ArkNpc::TryPerformAbilityContext0", 1005);

    context.localIntentFlags = ClassifyIntentFlags("ArkNpc::TryPerformAbilityContext0", 1002, 0);
    const Result ram = EvaluateRemoteIntent(context, "ArkNpc::TryPerformAbilityContext0", 1002);

    context.localIntentFlags = ClassifyIntentFlags("ArkNpc::TryPerformAbilityContext0", 105, 0);
    context.remoteMannequinFlags = CoopProtocol::kEnemyLocomotionFlagAttacking;
    const Result melee = EvaluateRemoteIntent(context, "ArkNpc::TryPerformAbilityContext0", 105);

    context.localIntentFlags = ClassifyIntentFlags("ArkNpc::TryPerformAbilityContext2", 114, 0);
    const Result mimicTaunt = EvaluateRemoteIntent(context, "ArkNpc::TryPerformAbilityContext2", 114);

    context.localIntentFlags = ClassifyIntentFlags("ArkNpc::MimicEntity", 0, CoopProtocol::kEnemyLocomotionFlagMorphing);
    const Result mimicry = EvaluateRemoteIntent(context, "ArkNpc::MimicEntity", 0);

    context.localIntentFlags = CoopProtocol::kEnemyLocomotionFlagTurning;
    const Result turn = EvaluateRemoteIntent(context, "ArkNpc::BeginAnimatedDistraction", 0);

    const bool ok =
        sideStep.decision == Decision::BlockNative && sideStep.kind == IntentKind::Movement &&
        (sideStepFlags & CoopProtocol::kEnemyLocomotionFlagLunging) != 0 &&
        (sideStepFlags & (CoopProtocol::kEnemyLocomotionFlagDashing |
            CoopProtocol::kEnemyLocomotionFlagShifting)) == 0 &&
        strafe.decision == Decision::BlockNative && strafe.kind == IntentKind::Movement &&
        ram.decision == Decision::AllowNative && ram.kind == IntentKind::Attack &&
        melee.decision == Decision::AllowNative && melee.kind == IntentKind::Attack &&
        mimicTaunt.decision == Decision::AllowNative && mimicTaunt.kind == IntentKind::Attack &&
        IsConsumableLocalAuthorityDecisionContext(110) &&
        IsConsumableLocalAuthorityDecisionContext(112) &&
        IsConsumableLocalAuthorityDecisionContext(113) &&
        IsConsumableLocalAuthorityDecisionContext(5) &&
        IsConsumableLocalAuthorityDecisionContext(21) &&
        IsConsumableLocalAuthorityDecisionContext(22) &&
        IsConsumableLocalAuthorityDecisionContext(109) &&
        IsConsumableLocalAuthorityDecisionContext(111) &&
        IsConsumableLocalAuthorityDecisionContext(121) &&
        IsConsumableLocalAuthorityDecisionContext(122) &&
        IsConsumableLocalAuthorityDecisionContext(1002) &&
        IsConsumableLocalAuthorityDecisionContext(1005) &&
        IsConsumableLocalAuthorityDecisionContext(1006) &&
        ShouldCompleteConsumedLocalAuthorityDecision(5) &&
        ShouldCompleteConsumedLocalAuthorityDecision(110) &&
        ShouldCompleteConsumedLocalAuthorityDecision(112) &&
        ShouldCompleteConsumedLocalAuthorityDecision(113) &&
        !ShouldCompleteConsumedLocalAuthorityDecision(21) &&
        !ShouldCompleteConsumedLocalAuthorityDecision(109) &&
        !ShouldCompleteConsumedLocalAuthorityDecision(1002) &&
        !ShouldCompleteConsumedLocalAuthorityDecision(1005) &&
        IsConsumableLocalAuthorityDecisionContext(114) &&
        IsConsumableLocalAuthorityDecisionContext(120) &&
        ShouldCompleteConsumedLocalAuthorityDecision(114) &&
        ShouldCompleteConsumedLocalAuthorityDecision(120) &&
        IsConsumableLocalAuthorityDecisionContext(
            CoopProtocol::kEnemyAbilityContextEthericDoppelganger) &&
        ShouldCompleteConsumedLocalAuthorityDecision(
            CoopProtocol::kEnemyAbilityContextEthericDoppelganger) &&
        mimicry.decision == Decision::BlockNative && mimicry.kind == IntentKind::Morph &&
        turn.decision == Decision::AllowNative && turn.kind == IntentKind::Turn;
    detail = ok
        ? "ok_authority_locomotion_local_turn_combat_and_mixed_ability_split"
        : "failed_authority_locomotion_matrix";
    return ok;
}
}

void ModMain::RecordRemoteObserverLocalIntentSample(
    EnemyAuthorityState& state,
    IEntity& entity,
    uint32_t intentKinds,
    uint32_t intentFlags,
    uint64_t contextId,
    EntityId targetEntityId,
    const char* stage,
    bool blocked)
{
    if (intentKinds == EnemyAuthorityState::ReadOnlyIntentNone)
        return;

    const float nowSeconds = gEnv && gEnv->pTimer
        ? gEnv->pTimer->GetAsyncCurTime()
        : state.localReadOnlyIntentObservedAtSeconds;

    // Attention is its own input lane. Do not let an unrelated OnAttack or
    // LookAround argument overwrite the local target that the controlled
    // presentation mixer consumes. Reading the already-resolved native top
    // target here is event/hook driven; it does not run another AI decision and
    // it does not write to the NPC.
    IEntity* localPlayer = ArkPlayer::GetInstancePtr()
        ? ArkPlayer::GetInstance().GetEntity()
        : nullptr;
    ArkNpc* npc = EntityUtils::GetArkNpc(&entity);
    unsigned topTarget = INVALID_ENTITYID;
    const bool topTargetRead = npc && TryGuardedCall(
        "read-only local intent top attention target",
        [npc]() { return npc->GetTopAttentionTargetEntityId(); },
        topTarget,
        nullptr);
    if (localPlayer && topTargetRead)
    {
        const EntityId localPlayerId = localPlayer->GetId();
        const bool targetsLocalPlayer = topTarget == localPlayerId;
        state.localReadOnlyAttentionActive = targetsLocalPlayer;
        if (targetsLocalPlayer)
        {
            intentKinds |= EnemyAuthorityState::ReadOnlyIntentAttention;
            state.localReadOnlyAttentionTargetEntityId = localPlayerId;
            state.localReadOnlyAttentionTargetPosition = localPlayer->GetWorldPos();
            state.localReadOnlyAttentionTargetPositionValid = true;
            state.localReadOnlyAttentionObservedAtSeconds = nowSeconds;
        }
        else
        {
            state.localReadOnlyAttentionTargetEntityId = INVALID_ENTITYID;
            state.localReadOnlyAttentionTargetPositionValid = false;
        }
    }
    constexpr float kIntentCombineSeconds = 0.50f;
    const bool priorSampleCurrent =
        nowSeconds >= state.localReadOnlyIntentObservedAtSeconds &&
        nowSeconds - state.localReadOnlyIntentObservedAtSeconds <= kIntentCombineSeconds;
    const uint32_t previousKinds = state.localReadOnlyIntentKinds;
    const uint32_t previousFlags = state.localReadOnlyIntentFlags;
    const EntityId previousTarget = state.localReadOnlyIntentTargetEntityId;

    if (!priorSampleCurrent)
    {
        state.localReadOnlyIntentKinds = EnemyAuthorityState::ReadOnlyIntentNone;
        state.localReadOnlyIntentFlags = 0;
        state.localReadOnlyIntentContextId = 0;
        state.localReadOnlyIntentTargetEntityId = INVALID_ENTITYID;
    }

    state.localReadOnlyIntentKinds |= intentKinds;
    state.localReadOnlyIntentFlags |= intentFlags;
    if (contextId != 0)
        state.localReadOnlyIntentContextId = contextId;
    if (targetEntityId != INVALID_ENTITYID)
        state.localReadOnlyIntentTargetEntityId = targetEntityId;
    state.localReadOnlyIntentObservedAtSeconds = nowSeconds;
    ++state.localReadOnlyIntentCaptures;
    ++m_enemyReadOnlyIntentCaptures;
    if (blocked)
    {
        ++state.localReadOnlyIntentBlocks;
        ++m_enemyReadOnlyIntentBlocks;
    }

    const bool changed =
        previousKinds != state.localReadOnlyIntentKinds ||
        previousFlags != state.localReadOnlyIntentFlags ||
        previousTarget != state.localReadOnlyIntentTargetEntityId;
    const bool traceDue =
        changed ||
        nowSeconds < state.localReadOnlyIntentTraceAtSeconds ||
        nowSeconds - state.localReadOnlyIntentTraceAtSeconds >= 1.0f;
    if (!traceDue)
        return;

    state.localReadOnlyIntentTraceAtSeconds = nowSeconds;
    AppendEnemySyncTrace(
        "local_intent_observe",
        "read_only local_intent"
        " net=" + std::to_string(state.netId) +
            " entity=" + std::to_string(entity.GetId()) +
            " kinds=" + std::to_string(state.localReadOnlyIntentKinds) +
            " flags=" + std::to_string(state.localReadOnlyIntentFlags) +
            " context=" + std::to_string(state.localReadOnlyIntentContextId) +
            " target=" + std::to_string(state.localReadOnlyIntentTargetEntityId) +
            " attentionTarget=" + std::to_string(state.localReadOnlyAttentionTargetEntityId) +
            " attention=" + std::to_string(state.localReadOnlyAttentionActive ? 1 : 0) +
            " blocked=" + std::to_string(blocked ? 1 : 0) +
            " stage=" + (stage && stage[0] ? std::string(stage) : std::string("-")) +
            " route=observe_before_mutation");
}

bool ModMain::ShouldBlockRemoteDrivenEnemyIntent(
    void* npcPtr,
    const char* stage,
    uint64_t contextId,
    EntityId targetEntityId,
    uint32_t semanticFlags)
{
    if (!npcPtr ||
        !gEnv ||
        !gEnv->pEntitySystem ||
        m_networkMode == CoopNetworkMode::Off ||
        !m_enemyLocomotionSyncEnabled ||
        !IsSessionGameplayReady())
    {
        return false;
    }

    auto* npc = reinterpret_cast<ArkNpc*>(npcPtr);
    if (!CoopRuntimeGuards::IsLikelyRuntimeCppObject(npc, sizeof(void*) * 4))
        return false;

    EntityId entityId = INVALID_ENTITYID;
    std::string reason;
    if (!TryGuardedCall("remote enemy intent gate GetEntityId", [npc]() { return npc->GetEntityId(); }, entityId, &reason) ||
        entityId == INVALID_ENTITYID)
    {
        return false;
    }

    IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId);
    if (!entity)
        return false;

    const bool clientRemotePuppet = IsClientRemoteEnemyPuppet(*entity);
    if (!clientRemotePuppet && !IsEnemyRuntimeControlCandidate(*entity))
        return false;

    const std::string_view stageView(stage && stage[0] ? stage : "");
    const bool passiveLocalAiAction =
        stageView.find("ArkNpc::BeginAnimatedDistraction") != std::string_view::npos ||
        stageView.find("ArkNpc::PerformPatrolIdle") != std::string_view::npos;

    auto tracePassiveReject = [&](const char* rejectReason) -> bool
    {
        if (passiveLocalAiAction)
        {
            m_lastEnemyMannequinStateEvent =
                "intent_gate skip"
                " kind=passive_local_ai"
                " stage=" + std::string(stageView) +
                " entity=" + std::to_string(entityId) +
                " reason=" + (rejectReason && rejectReason[0] ? std::string(rejectReason) : std::string("-"));
            AppendEnemySyncTrace("intent", m_lastEnemyMannequinStateEvent);
        }
        return false;
    };

    EnemyAuthorityState* state = nullptr;
    uint64_t enemyNetId = 0;
    const auto netIt = m_enemyNetIdsByEntity.find(entityId);
    if (netIt != m_enemyNetIdsByEntity.end())
    {
        enemyNetId = netIt->second;
        state = FindEnemyAuthorityByNetId(enemyNetId);
    }
    if (!state)
    {
        ScanLocalEnemyAuthorityRegistry("remote enemy intent rescan");
        const auto retryIt = m_enemyNetIdsByEntity.find(entityId);
        if (retryIt != m_enemyNetIdsByEntity.end())
        {
            enemyNetId = retryIt->second;
            state = FindEnemyAuthorityByNetId(enemyNetId);
        }
    }
    if (!state)
    {
        for (auto& entry : m_enemyAuthorities)
        {
            if (entry.second.entityId == entityId)
            {
                enemyNetId = entry.first;
                state = &entry.second;
                m_enemyNetIdsByEntity[entityId] = enemyNetId;
                break;
            }
        }
    }
    if (!state)
    {
        if (!clientRemotePuppet)
            return tracePassiveReject("no_enemy_authority_state");

        m_lastEnemyMannequinStateEvent =
            "intent_gate block"
            " kind=remote_spawn_puppet_prestate"
            " stage=" + std::string(stageView) +
            " entity=" + std::to_string(entityId) +
            " reason=observer_blocks_local_ai_before_authority_binding";
        AppendEnemySyncTrace("intent", m_lastEnemyMannequinStateEvent);
        ++m_enemyMannequinLocalSuppressions;
        return true;
    }

    const CoopEnemyControlPolicy::Context controlContext =
        BuildLocalEnemyControlPolicyContext(*state, *entity);
    const CoopEnemyControlPolicy::Decision controlDecision =
        CoopEnemyControlPolicy::Evaluate(controlContext);
    if (controlDecision.localVanillaAuthority)
        return tracePassiveReject("local_vanilla_authority");

    if (!controlDecision.remoteDriven || !controlDecision.BlocksAnyLocalVanilla())
        return tracePassiveReject("not_remote_driven_or_no_blocks");

    // A reliable authority EndMimicry edge records the inactive desired state
    // before Vanilla is called. Vanilla may reject that first call while a
    // Mimic action is retiring; allow its later native retry to finish the same
    // transition instead of trapping the observer in the stale prop body.
    if (stageView.find("ArkNpc::EndMimicry") != std::string_view::npos &&
        state->localMimicryStateKnown &&
        !state->localMimicryActive)
    {
        AppendEnemySyncTrace(
            "intent",
            "intent_gate allow kind=morph"
            " stage=" + std::string(stageView) +
                " net=" + std::to_string(state->netId) +
                " entity=" + std::to_string(entityId) +
                " reason=authority_mimicry_end_convergence");
        return false;
    }

    if (!controlDecision.localFocus &&
        passiveLocalAiAction &&
        controlDecision.remoteDriven &&
        !controlDecision.localVanillaAuthority &&
        controlDecision.BlocksAnyLocalVanilla())
    {
        m_lastEnemyMannequinStateEvent =
            "intent_gate block"
            " kind=passive_local_ai"
            " stage=" + std::string(stageView) +
            " net=" + std::to_string(state->netId) +
            " entity=" + std::to_string(entityId) +
            " remoteFragment=" + std::to_string(state->remoteMannequinFragmentId) +
            " remoteSeq=" + std::to_string(state->remoteMannequinSequence) +
            " remoteFlags=" + std::to_string(state->remoteMannequinFlags) +
            " reason=observer_blocks_passive_local_ai";
        AppendEnemySyncTrace("intent", m_lastEnemyMannequinStateEvent);
        ++m_enemyMannequinLocalSuppressions;
        return true;
    }

    const uint32_t movementFlags =
        CoopProtocol::kEnemyLocomotionFlagWalking |
        CoopProtocol::kEnemyLocomotionFlagRunning |
        CoopProtocol::kEnemyLocomotionFlagDashing |
        CoopProtocol::kEnemyLocomotionFlagShifting |
        CoopProtocol::kEnemyLocomotionFlagMorphing |
        CoopProtocol::kEnemyLocomotionFlagLunging |
        CoopProtocol::kEnemyLocomotionFlagMannequinDriven;
    const bool authorityMovementIntent =
        ((state->remoteLocomotionFlags | state->remoteMannequinFlags) & movementFlags) != 0;

    CoopEnemyIntentGate::RemoteIntentContext context;
    context.entityId = entityId;
    context.enemyNetId = state->netId;
    context.localIntentFlags = CoopEnemyIntentGate::ClassifyIntentFlags(stage, contextId, semanticFlags);
    context.remoteLocomotionFlags = state->remoteLocomotionFlags;
    context.remoteMannequinFlags = state->remoteMannequinFlags;
    context.remoteDriven = controlDecision.remoteDriven;
    context.localHasAttention = controlDecision.localFocus;
    context.localVanillaControlBlocked = controlDecision.BlocksAnyLocalVanilla();
    context.localMovementBlocked = controlDecision.blockMovement;
    context.localTurnBlocked = controlDecision.blockTurn;
    context.localAttackBlocked = controlDecision.blockAttack;
    context.authorityMovementIntent = authorityMovementIntent;
    context.mirrorAuthorityPresentation =
        state->remotePresentationTargetEntityId != INVALID_ENTITYID &&
        IsRemoteProxyEntity(state->remotePresentationTargetEntityId);
    context.authorityActionActive =
        state->remoteMannequinSequence != 0 &&
        state->remoteMannequinFragmentId >= 0;

    CoopEnemyIntentGate::Result result =
        CoopEnemyIntentGate::EvaluateRemoteIntent(context, stage, contextId);
    if (result.decision == CoopEnemyIntentGate::Decision::NotApplicable)
        return false;

    m_lastEnemyMannequinStateEvent = "intent_gate " + result.detail;
    AppendEnemySyncTrace("intent", m_lastEnemyMannequinStateEvent);
    if (result.decision == CoopEnemyIntentGate::Decision::BlockNative)
    {
        uint32_t readOnlyKinds = EnemyAuthorityState::ReadOnlyIntentNone;
        switch (result.kind)
        {
        case CoopEnemyIntentGate::IntentKind::Movement:
        case CoopEnemyIntentGate::IntentKind::Shift:
        case CoopEnemyIntentGate::IntentKind::Morph:
            readOnlyKinds = EnemyAuthorityState::ReadOnlyIntentMovement;
            break;
        case CoopEnemyIntentGate::IntentKind::Turn:
            readOnlyKinds = EnemyAuthorityState::ReadOnlyIntentFacing;
            break;
        case CoopEnemyIntentGate::IntentKind::Attack:
        case CoopEnemyIntentGate::IntentKind::HardState:
            readOnlyKinds = EnemyAuthorityState::ReadOnlyIntentCombat;
            break;
        case CoopEnemyIntentGate::IntentKind::Ability:
            readOnlyKinds = EnemyAuthorityState::ReadOnlyIntentAbility;
            break;
        default:
            break;
        }
        RecordRemoteObserverLocalIntentSample(
            *state,
            *entity,
            readOnlyKinds,
            context.localIntentFlags,
            contextId,
            targetEntityId,
            stage,
            true);
        ++m_enemyMannequinLocalSuppressions;
        return true;
    }

    if (controlDecision.localFocus)
    {
        uint32_t readOnlyKinds = EnemyAuthorityState::ReadOnlyIntentNone;
        const CoopEnemyIntentGate::AbilityOwnership abilityOwnership =
            CoopEnemyIntentGate::ClassifyAbilityOwnership(contextId);
        const bool allowedAbilityCombat =
            abilityOwnership == CoopEnemyIntentGate::AbilityOwnership::AuthorityMovementLocalCombat ||
            abilityOwnership == CoopEnemyIntentGate::AbilityOwnership::LocalCombat;
        if (allowedAbilityCombat)
        {
            readOnlyKinds = EnemyAuthorityState::ReadOnlyIntentCombat |
                EnemyAuthorityState::ReadOnlyIntentAbility;
            ++state->localCombatIntentAllows;
            state->localFocusCombatSeconds = std::max(state->localFocusCombatSeconds, 0.75f);
        }
        switch (result.kind)
        {
        case CoopEnemyIntentGate::IntentKind::Turn:
            readOnlyKinds = EnemyAuthorityState::ReadOnlyIntentFacing;
            ++state->localTurnIntentAllows;
            break;
        case CoopEnemyIntentGate::IntentKind::Attack:
        case CoopEnemyIntentGate::IntentKind::Ability:
            readOnlyKinds = EnemyAuthorityState::ReadOnlyIntentCombat |
                EnemyAuthorityState::ReadOnlyIntentAbility;
            if (!allowedAbilityCombat)
                ++state->localCombatIntentAllows;
            state->localFocusCombatSeconds = std::max(state->localFocusCombatSeconds, 0.75f);
            break;
        default:
            break;
        }
        RecordRemoteObserverLocalIntentSample(
            *state,
            *entity,
            readOnlyKinds,
            context.localIntentFlags,
            contextId,
            targetEntityId,
            stage,
            false);
    }
    return false;
}

bool ModMain::RearmRemoteDrivenEnemyLocalCombatAfterEnd(
    void* npcPtr,
    const char* stage)
{
    if (!npcPtr ||
        !gEnv ||
        !gEnv->pEntitySystem ||
        m_networkMode == CoopNetworkMode::Off ||
        !m_enemyLocomotionSyncEnabled ||
        !m_enemyAttentionAuthoritySyncEnabled ||
        !IsSessionGameplayReady() ||
        !ArkPlayer::GetInstancePtr())
    {
        return false;
    }

    auto* npc = reinterpret_cast<ArkNpc*>(npcPtr);
    if (!CoopRuntimeGuards::IsLikelyRuntimeCppObject(npc, sizeof(void*) * 4))
        return false;

    EntityId entityId = INVALID_ENTITYID;
    if (!TryGuardedCall(
            "remote enemy local combat gate GetEntityId",
            [npc]() { return npc->GetEntityId(); },
            entityId,
            nullptr) ||
        entityId == INVALID_ENTITYID)
    {
        return false;
    }

    IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId);
    IEntity* localPlayer = ArkPlayer::GetInstance().GetEntity();
    const auto netIt = m_enemyNetIdsByEntity.find(entityId);
    EnemyAuthorityState* state = netIt == m_enemyNetIdsByEntity.end()
        ? nullptr
        : FindEnemyAuthorityByNetId(netIt->second);
    if (!entity ||
        !localPlayer ||
        !state ||
        !state->localReadOnlyAttentionActive ||
        state->localReadOnlyAttentionTargetEntityId != localPlayer->GetId() ||
        !state->localReadOnlyAttentionTargetPositionValid)
        return false;

    const CoopEnemyControlPolicy::Decision decision =
        CoopEnemyControlPolicy::Evaluate(BuildLocalEnemyControlPolicyContext(*state, *entity));
    if (!decision.remoteDriven ||
        decision.localVanillaAuthority ||
        !decision.preserveLocalCombat)
    {
        return false;
    }

    bool dead = false;
    bool inCombat = true;
    unsigned topTarget = INVALID_ENTITYID;
    if (!TryGuardedCall(
            "remote enemy local combat gate IsDead",
            [npc]() { return npc->IsDead(); },
            dead,
            nullptr) ||
        dead ||
        !TryGuardedCall(
            "remote enemy local combat gate GetTopAttentionTargetEntityId",
            [npc]() { return npc->GetTopAttentionTargetEntityId(); },
            topTarget,
            nullptr) ||
        topTarget != localPlayer->GetId() ||
        !TryGuardedCall(
            "remote enemy local combat rearm read combat state",
            [npc]() { return npc->m_bIsInCombat; },
            inCombat,
            nullptr) ||
        inCombat)
    {
        return false;
    }

    const EntityId localPlayerId = localPlayer->GetId();
    std::string guardReason;
    const bool replayedAttention = TryGuardedVoidCall(
        "remote enemy local combat rearm replay attention",
        [npc, localPlayerId]()
        {
            ArkNpc::FOnNewAttentionTarget(npc, localPlayerId, false);
        },
        &guardReason);

    bool inCombatAfterReplay = false;
    TryGuardedCall(
        "remote enemy local combat rearm reread combat state",
        [npc]() { return npc->m_bIsInCombat; },
        inCombatAfterReplay,
        &guardReason);
    bool beganCombatFallback = false;
    if (!inCombatAfterReplay)
    {
        beganCombatFallback = TryGuardedVoidCall(
            "remote enemy local combat rearm OnCombatBegin fallback",
            [npc]() { npc->OnCombatBegin(); },
            &guardReason);
        TryGuardedCall(
            "remote enemy local combat rearm final combat state",
            [npc]() { return npc->m_bIsInCombat; },
            inCombatAfterReplay,
            &guardReason);
    }

    const bool rearmed = replayedAttention && inCombatAfterReplay;
    if (rearmed)
        ++state->localCombatEndRearms;
    AppendEnemySyncTrace(
        "local_combat",
        "rearmed native local combat after plan end"
        " net=" + std::to_string(state->netId) +
            " entity=" + std::to_string(entityId) +
            " target=" + std::to_string(topTarget) +
            " applied=" + std::to_string(rearmed ? 1 : 0) +
            " replay=" + std::to_string(replayedAttention ? 1 : 0) +
            " fallback=" + std::to_string(beganCombatFallback ? 1 : 0) +
            " combat=" + std::to_string(inCombatAfterReplay ? 1 : 0) +
            " rearms=" + std::to_string(state->localCombatEndRearms) +
            " stage=" + (stage && stage[0] ? std::string(stage) : std::string("-")) +
            (guardReason.empty() ? std::string() : " guard=" + guardReason) +
            " locomotion=remote damage=victim_local");
    return rearmed;
}

bool ModMain::ShouldConsumeRemoteDrivenEnemyAuthorityAbility(
    void* npcPtr,
    uint64_t contextId)
{
    if (!npcPtr ||
        !CoopEnemyIntentGate::IsConsumableLocalAuthorityDecisionContext(contextId) ||
        !gEnv ||
        !gEnv->pEntitySystem ||
        m_networkMode == CoopNetworkMode::Off ||
        !m_enemyLocomotionSyncEnabled ||
        !IsSessionGameplayReady())
    {
        return false;
    }

    auto* npc = reinterpret_cast<ArkNpc*>(npcPtr);
    if (!CoopRuntimeGuards::IsLikelyRuntimeCppObject(npc, sizeof(void*) * 4))
        return false;

    EntityId entityId = INVALID_ENTITYID;
    if (!TryGuardedCall(
            "remote enemy movement ability filter GetEntityId",
            [npc]() { return npc->GetEntityId(); },
            entityId,
            nullptr) ||
        entityId == INVALID_ENTITYID)
    {
        return false;
    }

    IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId);
    const auto netIt = m_enemyNetIdsByEntity.find(entityId);
    EnemyAuthorityState* state = netIt == m_enemyNetIdsByEntity.end()
        ? nullptr
        : FindEnemyAuthorityByNetId(netIt->second);
    if (!entity || !state)
        return false;

    const CoopEnemyControlPolicy::Decision decision =
        CoopEnemyControlPolicy::Evaluate(BuildLocalEnemyControlPolicyContext(*state, *entity));
    return decision.mode == CoopEnemyControlPolicy::EnemyControlMode::LocalTargetRemoteOwner &&
        decision.remoteDriven &&
        decision.preserveLocalCombat &&
        decision.blockMovement;
}
