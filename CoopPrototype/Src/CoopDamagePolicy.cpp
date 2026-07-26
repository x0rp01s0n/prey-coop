#include "CoopDamagePolicy.h"

#include <cmath>

namespace CoopDamagePolicy
{
Decision EvaluateProxyHit(const ProxyHitContext& context)
{
    Decision decision;
    if (!context.targetIsRemoteProxy)
        return decision;

    decision.suppressNativeHit = true;
    if (!context.sessionReady)
    {
        decision.reason = Reason::SessionNotReady;
        return decision;
    }
    if (!std::isfinite(context.damage) || context.damage <= 0.0f)
    {
        decision.reason = Reason::InvalidDamage;
        return decision;
    }
    decision.source = EvaluateEnemyDamageRequestSource(
        context.localPlayerEntityId,
        context.shooterEntityId,
        context.projectileOwnerEntityId);
    if (decision.source == Source::None)
    {
        decision.reason = Reason::SourceNotLocalPlayer;
        return decision;
    }

    if (context.remotePlayerDowned)
    {
        decision.reason = Reason::RemotePlayerDowned;
        return decision;
    }
    if (!context.friendlyFireEnabled)
    {
        decision.reason = Reason::FriendlyFireDisabled;
        return decision;
    }

    decision.emitFriendlyFire = true;
    decision.reason = Reason::ForwardFriendlyFire;
    return decision;
}

Source EvaluateEnemyDamageRequestSource(
    uint32_t localPlayerEntityId,
    uint32_t shooterEntityId,
    uint32_t projectileOwnerEntityId)
{
    if (localPlayerEntityId == 0 || localPlayerEntityId == 0xffffffffu)
        return Source::None;
    if (shooterEntityId == localPlayerEntityId)
        return Source::LocalPlayer;
    if (projectileOwnerEntityId == localPlayerEntityId)
        return Source::LocalPlayerProjectile;
    return Source::None;
}

const char* ToString(Source source)
{
    switch (source)
    {
    case Source::None: return "none";
    case Source::LocalPlayer: return "local_player";
    case Source::LocalPlayerProjectile: return "local_projectile";
    default: return "invalid";
    }
}

const char* ToString(Reason reason)
{
    switch (reason)
    {
    case Reason::NotRemoteProxy: return "not_remote_proxy";
    case Reason::SessionNotReady: return "session_not_ready";
    case Reason::InvalidDamage: return "invalid_damage";
    case Reason::RemotePlayerDowned: return "remote_player_downed";
    case Reason::FriendlyFireDisabled: return "friendly_fire_disabled";
    case Reason::SourceNotLocalPlayer: return "source_not_local_player";
    case Reason::ForwardFriendlyFire: return "forward_friendly_fire";
    default: return "invalid";
    }
}

bool RunSelfTest(std::string& detail)
{
    ProxyHitContext context;
    context.targetIsRemoteProxy = true;
    context.sessionReady = true;
    context.friendlyFireEnabled = true;
    context.damage = 17.0f;
    context.localPlayerEntityId = 100;

    context.shooterEntityId = 100;
    const Decision direct = EvaluateProxyHit(context);

    context.shooterEntityId = 200;
    context.projectileOwnerEntityId = 100;
    const Decision projectile = EvaluateProxyHit(context);

    context.projectileOwnerEntityId = 300;
    const Decision enemy = EvaluateProxyHit(context);

    context.friendlyFireEnabled = false;
    context.shooterEntityId = 100;
    const Decision disabled = EvaluateProxyHit(context);

    context.friendlyFireEnabled = true;
    context.targetIsRemoteProxy = false;
    const Decision ordinaryNpc = EvaluateProxyHit(context);

    context.targetIsRemoteProxy = true;
    context.remotePlayerDowned = true;
    const Decision downed = EvaluateProxyHit(context);

    const Source enemyDamageDirect = EvaluateEnemyDamageRequestSource(100, 100, 0);
    const Source enemyDamageProjectile = EvaluateEnemyDamageRequestSource(100, 200, 100);
    const Source enemyDamageNpc = EvaluateEnemyDamageRequestSource(100, 200, 300);
    const Source enemyDamageMissingPlayer = EvaluateEnemyDamageRequestSource(0xffffffffu, 0xffffffffu, 0);

    const bool ok =
        direct.suppressNativeHit && direct.emitFriendlyFire && direct.source == Source::LocalPlayer &&
        projectile.suppressNativeHit && projectile.emitFriendlyFire && projectile.source == Source::LocalPlayerProjectile &&
        enemy.suppressNativeHit && !enemy.emitFriendlyFire && enemy.reason == Reason::SourceNotLocalPlayer &&
        disabled.suppressNativeHit && !disabled.emitFriendlyFire && disabled.reason == Reason::FriendlyFireDisabled &&
        !ordinaryNpc.suppressNativeHit && !ordinaryNpc.emitFriendlyFire &&
        downed.suppressNativeHit && !downed.emitFriendlyFire && downed.reason == Reason::RemotePlayerDowned &&
        enemyDamageDirect == Source::LocalPlayer &&
        enemyDamageProjectile == Source::LocalPlayerProjectile &&
        enemyDamageNpc == Source::None &&
        enemyDamageMissingPlayer == Source::None;
    detail = ok ? "ok_10_cases" : "failed_policy_matrix";
    return ok;
}
}
