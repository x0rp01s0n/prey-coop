#pragma once

#include <cstdint>
#include <string>

#include <Prey/CryCore/StdAfx.h>
#include <Prey/CryEntitySystem/IEntity.h>

class ArkNpc;

namespace CoopEnemyIntentGate
{
enum class Decision
{
    NotApplicable,
    AllowNative,
    BlockNative,
};

enum class IntentKind
{
    Unknown,
    Movement,
    Turn,
    Attack,
    Ability,
    Shift,
    Morph,
    Gloo,
    HardState,
};

enum class AbilityOwnership
{
    Unknown,
    AuthorityMovement,
    AuthorityMovementLocalCombat,
    LocalCombat,
    AuthorityWorldState,
};

struct RemoteIntentContext
{
    EntityId entityId = INVALID_ENTITYID;
    uint64_t enemyNetId = 0;
    uint32_t localIntentFlags = 0;
    uint32_t remoteLocomotionFlags = 0;
    uint32_t remoteMannequinFlags = 0;
    bool remoteDriven = false;
    bool localHasAttention = false;
    bool localVanillaControlBlocked = false;
    bool localMovementBlocked = false;
    bool localTurnBlocked = false;
    bool localAttackBlocked = false;
    bool authorityMovementIntent = false;
    bool mirrorAuthorityPresentation = false;
    bool authorityActionActive = false;
};

struct Result
{
    Decision decision = Decision::NotApplicable;
    IntentKind kind = IntentKind::Unknown;
    std::string detail;
};

uint32_t ClassifyIntentFlags(const char* stage, uint64_t contextId, uint32_t semanticFlags);
uint32_t ClassifyAbilityContextFlags(uint64_t contextId);
AbilityOwnership ClassifyAbilityOwnership(uint64_t contextId);
bool IsAuthorityLocomotionAbilityContext(uint64_t contextId);
bool IsConsumableLocalAuthorityDecisionContext(uint64_t contextId);
bool ShouldCompleteConsumedLocalAuthorityDecision(uint64_t contextId);
const char* IntentKindName(IntentKind kind);
Result EvaluateRemoteIntent(const RemoteIntentContext& context, const char* stage, uint64_t contextId);
bool RunSelfTest(std::string& detail);
}
