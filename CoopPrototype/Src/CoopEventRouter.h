#pragma once

#include <cstdint>

#include <Prey/CryEntitySystem/IEntity.h>

enum class CoopNetworkMode;

namespace CoopEvents
{
enum class Kind : uint16_t
{
    Unknown,
    LocalPresentation,
    PlayerPoseSnapshot,
    PlayerDamageCommit,
    EnemySpawnRequest,
    EnemySpawnCommit,
    EnemyStateSnapshot,
    EnemyDamageRequest,
    EnemyHitObserved,
    EnemyDeathCommit,
    PlayerActionUse,
    PlayerActionFire,
    PickupPersonal,
    PickupSharedDrop,
    StoryProgression,
    DoorStateCommit,
};

enum class AuthorityDomain : uint8_t
{
    LocalPresentation,
    PlayerOwned,
    AreaOwned,
    StoryOwned,
};

enum class EntityRole : uint8_t
{
    Unknown,
    LocalPlayer,
    RemotePlayerProxy,
    HostEnemyAuthority,
    EnemyPuppet,
    PersonalLoot,
    SharedDroppedItem,
    StoryEntity,
    LocalPresentation,
};

enum class RouteAction : uint8_t
{
    Drop,
    LocalOnly,
    RemoteUnreliable,
    RemoteReliable,
    HostReliable,
    BroadcastReliable,
};

enum class Reliability : uint8_t
{
    None,
    Unreliable,
    ReliableOrdered,
};

struct Context
{
    Kind kind = Kind::Unknown;
    AuthorityDomain domain = AuthorityDomain::LocalPresentation;
    EntityRole sourceRole = EntityRole::Unknown;
    EntityRole targetRole = EntityRole::Unknown;
    CoopNetworkMode localMode{};
    EntityId sourceEntityId = INVALID_ENTITYID;
    EntityId targetEntityId = INVALID_ENTITYID;
    float damage = 0.0f;
    bool sameLevel = false;
    bool localAuthority = false;
    bool irreversible = false;
};

struct Route
{
    RouteAction action = RouteAction::Drop;
    Reliability reliability = Reliability::None;
    bool shouldEmitNetwork = false;
    const char* reason = "drop";
};

class Router
{
public:
    Route RouteEvent(const Context& context) const;

private:
    Route Drop(const char* reason) const;
    Route LocalOnly(const char* reason) const;
    Route RemoteUnreliable(const char* reason) const;
    Route RemoteReliable(const char* reason) const;
    Route HostReliable(const char* reason) const;
    Route BroadcastReliable(const char* reason) const;
};

const char* ToString(Kind kind);
const char* ToString(RouteAction action);
const char* ToString(Reliability reliability);
}
