#include "CoopEventRouter.h"

#include "ModMain.h"

namespace CoopEvents
{
Route Router::RouteEvent(const Context& context) const
{
    if (!context.sameLevel &&
        context.kind != Kind::StoryProgression &&
        context.kind != Kind::PlayerPoseSnapshot)
    {
        return Drop("not same level");
    }

    switch (context.kind)
    {
    case Kind::LocalPresentation:
        return LocalOnly("presentation is local");

    case Kind::PlayerPoseSnapshot:
        return RemoteUnreliable("pose snapshot");

    case Kind::PlayerDamageCommit:
        if (context.domain != AuthorityDomain::PlayerOwned)
            return Drop("player damage must be player-owned");
        if (context.targetRole != EntityRole::RemotePlayerProxy)
            return Drop("player damage target is not remote proxy");
        if (!context.localAuthority)
            return Drop("no authority for player damage");
        return RemoteReliable("player damage commit");

    case Kind::EnemySpawnRequest:
        if (context.localAuthority)
            return Drop("authority does not request spawn");
        return HostReliable("enemy spawn request");

    case Kind::EnemySpawnCommit:
        if (!context.localAuthority)
            return Drop("non-authority cannot commit enemy spawn");
        return RemoteReliable("enemy spawn commit");

    case Kind::EnemyStateSnapshot:
        if (!context.localAuthority)
            return Drop("non-authority cannot snapshot enemy");
        return RemoteUnreliable("enemy state snapshot");

    case Kind::EnemyDamageRequest:
        if (context.localAuthority)
            return Drop("authority does not request enemy damage");
        return HostReliable("enemy damage request");

    case Kind::EnemyHitObserved:
        if (!context.localAuthority)
            return HostReliable("enemy hit claim");
        return RemoteUnreliable("enemy hit updated state");

    case Kind::EnemyDeathCommit:
        if (!context.localAuthority)
            return Drop("non-authority cannot commit enemy death");
        return RemoteReliable("enemy death commit");

    case Kind::PlayerActionUse:
    case Kind::PlayerActionFire:
        return HostReliable("player action intent");

    case Kind::PickupPersonal:
        return LocalOnly("personal loot");

    case Kind::PickupSharedDrop:
        return RemoteReliable("shared dropped item");

    case Kind::StoryProgression:
        return BroadcastReliable("story progression");

    case Kind::DoorStateCommit:
        if (!context.localAuthority)
            return HostReliable("door state intent");
        return RemoteReliable("door state commit");

    case Kind::Unknown:
    default:
        return Drop("unknown event");
    }
}

Route Router::Drop(const char* reason) const
{
    return { RouteAction::Drop, Reliability::None, false, reason };
}

Route Router::LocalOnly(const char* reason) const
{
    return { RouteAction::LocalOnly, Reliability::None, false, reason };
}

Route Router::RemoteUnreliable(const char* reason) const
{
    return { RouteAction::RemoteUnreliable, Reliability::Unreliable, true, reason };
}

Route Router::RemoteReliable(const char* reason) const
{
    return { RouteAction::RemoteReliable, Reliability::ReliableOrdered, true, reason };
}

Route Router::HostReliable(const char* reason) const
{
    return { RouteAction::HostReliable, Reliability::ReliableOrdered, true, reason };
}

Route Router::BroadcastReliable(const char* reason) const
{
    return { RouteAction::BroadcastReliable, Reliability::ReliableOrdered, true, reason };
}

const char* ToString(Kind kind)
{
    switch (kind)
    {
    case Kind::Unknown: return "Unknown";
    case Kind::LocalPresentation: return "LocalPresentation";
    case Kind::PlayerPoseSnapshot: return "PlayerPoseSnapshot";
    case Kind::PlayerDamageCommit: return "PlayerDamageCommit";
    case Kind::EnemySpawnRequest: return "EnemySpawnRequest";
    case Kind::EnemySpawnCommit: return "EnemySpawnCommit";
    case Kind::EnemyStateSnapshot: return "EnemyStateSnapshot";
    case Kind::EnemyDamageRequest: return "EnemyDamageRequest";
    case Kind::EnemyHitObserved: return "EnemyHitObserved";
    case Kind::EnemyDeathCommit: return "EnemyDeathCommit";
    case Kind::PlayerActionUse: return "PlayerActionUse";
    case Kind::PlayerActionFire: return "PlayerActionFire";
    case Kind::PickupPersonal: return "PickupPersonal";
    case Kind::PickupSharedDrop: return "PickupSharedDrop";
    case Kind::StoryProgression: return "StoryProgression";
    case Kind::DoorStateCommit: return "DoorStateCommit";
    default: return "Invalid";
    }
}

const char* ToString(RouteAction action)
{
    switch (action)
    {
    case RouteAction::Drop: return "Drop";
    case RouteAction::LocalOnly: return "LocalOnly";
    case RouteAction::RemoteUnreliable: return "RemoteUnreliable";
    case RouteAction::RemoteReliable: return "RemoteReliable";
    case RouteAction::HostReliable: return "HostReliable";
    case RouteAction::BroadcastReliable: return "BroadcastReliable";
    default: return "Invalid";
    }
}

const char* ToString(Reliability reliability)
{
    switch (reliability)
    {
    case Reliability::None: return "None";
    case Reliability::Unreliable: return "Unreliable";
    case Reliability::ReliableOrdered: return "ReliableOrdered";
    default: return "Invalid";
    }
}
}
