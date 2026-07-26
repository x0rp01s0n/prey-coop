#pragma once

#include <cstdint>
#include <string>

namespace CoopDamagePolicy
{
enum class Source : uint8_t
{
    None,
    LocalPlayer,
    LocalPlayerProjectile,
};

enum class Reason : uint8_t
{
    NotRemoteProxy,
    SessionNotReady,
    InvalidDamage,
    RemotePlayerDowned,
    FriendlyFireDisabled,
    SourceNotLocalPlayer,
    ForwardFriendlyFire,
};

struct ProxyHitContext
{
    bool targetIsRemoteProxy = false;
    bool sessionReady = false;
    bool friendlyFireEnabled = false;
    bool remotePlayerDowned = false;
    float damage = 0.0f;
    uint32_t localPlayerEntityId = 0;
    uint32_t shooterEntityId = 0;
    uint32_t projectileOwnerEntityId = 0;
};

struct Decision
{
    bool suppressNativeHit = false;
    bool emitFriendlyFire = false;
    Source source = Source::None;
    Reason reason = Reason::NotRemoteProxy;
};

Decision EvaluateProxyHit(const ProxyHitContext& context);
Source EvaluateEnemyDamageRequestSource(
    uint32_t localPlayerEntityId,
    uint32_t shooterEntityId,
    uint32_t projectileOwnerEntityId);
const char* ToString(Source source);
const char* ToString(Reason reason);
bool RunSelfTest(std::string& detail);
}
