#include "ModMain.h"
#include "CoopRuntimeLog.h"
#include "CoopRuntimeGuards.h"
#include "CoopPtrHygiene.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <Chairloader/IChairLogger.h>
#include <Prey/ArkEnums.h>
#include <Prey/CryEntitySystem/IEntitySystem.h>
#include <EntityUtils.h>
#include <Prey/CryGame/Game.h>
#include <Prey/CryGame/IGameFramework.h>
#include <Prey/CrySystem/ISystem.h>
#include <Prey/CrySystem/ITimer.h>
#include <Prey/GameDll/ark/attention/ArkAttentionManager.h>
#include <Prey/GameDll/ark/npc/ArkNpc.h>
#include <Prey/GameDll/ark/player/ArkFocusModeComponent.h>
#include <Prey/GameDll/ark/player/ArkPlayer.h>
#include <Prey/GameDll/ark/player/ArkPlayerComponent.h>
#include <Prey/GameDll/ark/player/ArkPlayerHealthComponent.h>
#include <Prey/GameDll/ark/player/ArkPlayerInput.h>
#include <Prey/GameDll/ark/player/ArkPsiComponent.h>
#include <Prey/GameDll/ark/player/ArkPlayerWeaponComponent.h>
#include <Prey/GameDll/ark/player/ArkQuickSelectComponent.h>
#include <Prey/GameDll/ark/player/ability/ArkAbilityComponent.h>
#include <Prey/GameDll/ark/player/psipower/ArkPsiPowerComponent.h>
#include <Prey/GameDll/ark/signalsystem/arksignalcontext.h>
#include <Prey/GameDll/ark/signalsystem/arksignalmanager.h>

namespace
{
using CoopRuntimeGuards::IsReadableRuntimePointer;
using CoopRuntimeGuards::TryGuardedCall;
using CoopRuntimeGuards::TryGuardedVoidCall;

constexpr float kRemoteDownedRecoveryTickSeconds = 0.5f;
constexpr float kRemoteDownedTeleportDistanceMeters = 28.0f;
constexpr float kRemoteDownedTeleportOffsetMeters = 2.0f;
constexpr float kRemoteReviveDistanceMeters = 2.8f;
constexpr float kRemoteReviveFacingDot = 0.25f;
constexpr float kRemoteReviveHoldSeconds = 2.0f;
constexpr float kPostReviveDownedGraceSeconds = 1.5f;
constexpr float kLocalDownedAttentionClearSeconds = 0.25f;
constexpr float kLocalDownedFocusCleanupSeconds = 0.25f;
constexpr float kLocalPlayerHealthWriteEpsilon = 0.01f;
constexpr float kUnstuckTeleportOffsetMeters = 2.0f;
constexpr float kDownedHealth = 1.0f;
constexpr int kLocalAttentionObserveOnly = 0;
constexpr int kLocalAttentionNpcClearOnPlayer = 1;
constexpr int kLocalAttentionManagerSimple = 2;
constexpr int kLocalAttentionManagerSimpleComplex = 3;
constexpr int kLocalAttentionFullLegacy = 4;
constexpr int kRemoteReviveDirectBurstCount = 3;
constexpr float kDamageDedupeExactTtlSeconds = 1.00f;
constexpr float kDamageDedupeRawLocalTtlSeconds = 0.45f;
constexpr float kDamageSignalPendingTtlSeconds = 0.75f;
constexpr size_t kDamageDedupeMaxEntries = 96;
constexpr uint64_t kDamageDedupeFnvOffset = 1469598103934665603ull;
constexpr uint64_t kDamageDedupeFnvPrime = 1099511628211ull;
constexpr uint32_t kDamagePositionFnvOffset = 2166136261u;
constexpr uint32_t kDamagePositionFnvPrime = 16777619u;
constexpr float kDamagePositionQuantization = 4.0f;
constexpr float kDamageTimeBucketSeconds = 0.5f;

void LogCoop(std::string_view msg)
{
    CoopRuntimeLog::Write(msg);
}

bool IsGameReady()
{
    return gEnv && gEnv->pEntitySystem && ArkPlayer::GetInstancePtr() && ArkPlayer::GetInstance().GetEntity();
}

float DamageDedupeNow()
{
    return gEnv && gEnv->pTimer ? gEnv->pTimer->GetAsyncCurTime() : 0.0f;
}

int DamageBucket(float damage)
{
    return static_cast<int>(std::lround(std::max(0.0f, damage) * 10.0f));
}

void HashDamageValue(uint64_t& hash, uint64_t value)
{
    hash ^= value;
    hash *= kDamageDedupeFnvPrime;
}

void HashDamageSigned(uint64_t& hash, int64_t value)
{
    HashDamageValue(hash, static_cast<uint64_t>(value));
}

void HashDamagePositionValue(uint32_t& hash, uint32_t value)
{
    hash ^= value;
    hash *= kDamagePositionFnvPrime;
}

uint32_t PackSignedDamageCoordinate(float value)
{
    if (!std::isfinite(value))
        return 0u;
    return static_cast<uint32_t>(static_cast<int32_t>(std::lround(value * kDamagePositionQuantization)));
}

bool IsLocalPlayerHealthComponent(const ArkPlayerHealthComponent* healthComponent)
{
    if (!healthComponent || !ArkPlayer::GetInstancePtr())
        return false;

    ArkPlayerHealthComponent* localComponent = &ArkPlayer::GetInstance().m_playerComponent.GetHealthComponent();
    if (healthComponent != localComponent && CoopPtrHygiene::Enabled())
    {
        char line[160];
        std::snprintf(line, sizeof(line),
            "ptr_hygiene tag=local_player_health_mismatch got=0x%016llX expected=0x%016llX",
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(healthComponent)),
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(localComponent)));
        CoopRuntimeLog::WriteRateLimited("local_player_health_mismatch", line, 1.0, 3);
    }
    return healthComponent == localComponent;
}

void NormalizeHealthFeedbackAfterRestore(ArkPlayerHealthComponent& healthComponent, float health)
{
    healthComponent.m_damageSinceRegen = 0.0f;
    healthComponent.m_elapsedSinceDamaged = 0.0f;
    healthComponent.m_bRegening = false;
    healthComponent.m_bDeathMenuOpened = false;
    healthComponent.m_feedback.m_prevHealth = health;
    healthComponent.m_feedback.m_bTookDamageThisTick = false;
    healthComponent.UpdateHUD(false, health);
}

void RunSidecarPostDeserializeRepairs(ArkPlayer& player, std::string& detail)
{
    auto appendFailure = [&detail](const std::string& reason)
    {
        if (reason.empty())
            return;
        if (!detail.empty())
            detail += "; ";
        detail += reason;
    };

    std::string reason;
    if (!TryGuardedVoidCall(
            "coop sidecar health PostSerialize",
            [&]()
            {
                player.m_playerComponent.GetHealthComponent().PostSerialize();
            },
            &reason))
    {
        appendFailure(reason);
    }

    reason.clear();
    if (!TryGuardedVoidCall(
            "coop sidecar ability PostSerialize",
            [&]()
            {
                player.m_playerComponent.GetAbilityComponent().PostSerialize();
            },
            &reason))
    {
        appendFailure(reason);
    }

    reason.clear();
    if (!TryGuardedVoidCall(
            "coop sidecar psi PostSerialize",
            [&]()
            {
                player.m_playerComponent.GetPsiComponent().PostSerialize();
            },
            &reason))
    {
        appendFailure(reason);
    }

    reason.clear();
    if (!TryGuardedVoidCall(
            "coop sidecar psi power PostSerialize",
            [&]()
            {
                player.GetPsiPowerComponent().PostSerialize();
            },
            &reason))
    {
        appendFailure(reason);
    }

    if (player.m_playerComponent.m_pFocusModeComponent)
    {
        reason.clear();
        if (!TryGuardedVoidCall(
                "coop sidecar focus PostSerialize",
                [&]()
                {
                    player.m_playerComponent.m_pFocusModeComponent->PostSerialize();
                },
                &reason))
        {
            appendFailure(reason);
        }
    }

    reason.clear();
    if (!TryGuardedVoidCall(
            "coop sidecar weapon PostSerialize",
            [&]()
            {
                player.m_weaponComponent.PostSerialize();
            },
            &reason))
    {
        appendFailure(reason);
    }

    reason.clear();
    if (!TryGuardedVoidCall(
            "coop sidecar quickselect refresh",
            [&]()
            {
                ArkQuickSelectComponent& quickSelect = player.m_playerComponent.GetQuickSelectComponent();
                quickSelect.CloseQuickSelect();
                quickSelect.RefreshFilterFeedback();
            },
            &reason))
    {
        appendFailure(reason);
    }
}

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool IsAllowedDownedAction(const std::string& action)
{
    return action == "moveforward" ||
        action == "moveback" ||
        action == "moveleft" ||
        action == "moveright" ||
        action == "jump" ||
        action == "rotateyaw" ||
        action == "rotatepitch" ||
        action == "xi_movex" ||
        action == "xi_movey" ||
        action == "xi_rotateyaw" ||
        action == "xi_rotatepitch" ||
        action == "xi_reticle_movex" ||
        action == "xi_reticle_movey" ||
        action == "hmd_rotateyaw" ||
        action == "hmd_rotatepitch" ||
        action == "hud_mousex" ||
        action == "hud_mousey";
}

bool IsBlockedDownedAction(const std::string& action)
{
    static constexpr std::string_view kExactBlockedActions[] = {
        "preuse",
        "use",
        "itemprepickup",
        "itempickup",
        "lookat",
        "ripout",
        "carry",
        "specialuse",
        "attack1",
        "attack1_xi",
        "attack2_xi",
        "attack1_cine",
        "attack2_cine",
        "reload",
        "modify",
        "nextweapon",
        "prevweapon",
        "nextitem",
        "previtem",
        "toggle_explosive",
        "toggle_weapon",
        "toggle_special",
        "drop",
        "toggle_grenade",
        "handgrenade",
        "xi_handgrenade",
        "grenade",
        "xi_grenade",
        "zoom",
        "zoom_toggle",
        "zoom_in",
        "zoom_out",
        "xi_zoom",
        "firemode",
        "weapon_change_firemode",
        "mouse_wheel",
        "mouse_wheel_infiction_close",
        "use_psi_item",
        "use_healing_item",
        "use_food_item",
        "use_armor_item",
        "psimode",
        "activate_psipower",
        "deactivate_psipower",
        "toggle_scope",
        "togglepda",
        "pdanextpage",
        "pdaprevpage",
        "toggleinventory",
        "toggleobjectives",
        "toggleabilities",
        "togglemap",
        "toggledata",
        "togglestatus",
        "equip_last_weapon",
        "increasetimedilation",
        "decreasetimedilation",
        "normaltimedilation",
        "toggle_flashlight",
    };

    for (std::string_view blocked : kExactBlockedActions)
    {
        if (action == blocked)
            return true;
    }

    return action.find("attack") != std::string::npos ||
        action.find("weapon") != std::string::npos ||
        action.find("grenade") != std::string::npos ||
        action.find("power") != std::string::npos ||
        action.find("focusmode") != std::string::npos ||
        action.find("ability") != std::string::npos ||
        action.find("abilities") != std::string::npos ||
        action.find("inventory") != std::string::npos;
}

void RefreshNativeRemoteRevivePrompt(ArkPlayer& player, float holdDuration)
{
    constexpr int kHoldUsePromptIndex = static_cast<int>(EArkInteractionMode::holdUse);
    if (kHoldUsePromptIndex < 0 ||
        kHoldUsePromptIndex >= static_cast<int>(player.m_interaction.m_buttonPrompts.m_buttonPrompts.size()))
    {
        return;
    }

    CGame* game = gEnv && gEnv->pGame ? static_cast<CGame*>(gEnv->pGame) : nullptr;
    const CCryName actionId = game && game->m_pGameActions ? game->m_pGameActions->use : CCryName("use");
    const string actionMap("player");
    const wstring promptText(L"Revive");

    ArkButtonPrompt& prompt = player.m_interaction.m_buttonPrompts.m_buttonPrompts[static_cast<size_t>(kHoldUsePromptIndex)];
    prompt.SetValues(actionId, actionMap, promptText, true, true, holdDuration);
    if (player.m_interaction.m_buttonPrompts.m_maxUsed <= kHoldUsePromptIndex)
        player.m_interaction.m_buttonPrompts.m_maxUsed = static_cast<uint8_t>(kHoldUsePromptIndex + 1);
}

bool IsDownedCancelAction(const std::string& action, int activationMode)
{
    if ((activationMode & eAAM_OnRelease) != 0)
        return true;

    return action == "deactivate_psipower" ||
        action == "normaltimedilation";
}

void ResetFocusTimeScaler(ArkTimeScaler& scaler, ArkTimeScaleManager* manager, uint32_t& clearedCount)
{
    const int handle = scaler.m_timeScaleHandle;
    if (manager && handle >= 0)
    {
        manager->ClearTimeScaleOverride(handle);
        ++clearedCount;
    }

    scaler.m_initialTimeScale = 1.0f;
    scaler.m_targetTimeScale = 1.0f;
    scaler.m_currentTimeScale = 1.0f;
    scaler.m_elapsedSec = 0.0f;
    scaler.m_durationSec = 0.0f;
    scaler.m_timeScaleHandle = -1;
}

bool IsLocalAttentionClearSafeGameplay(std::string& reason)
{
    if (!gEnv)
    {
        reason = "no env";
        return false;
    }

    if (gEnv->pSystem && gEnv->pSystem->IsPaused())
    {
        reason = "system paused";
        return false;
    }

    if (gEnv->pTimer && gEnv->pTimer->IsTimerPaused(ITimer::ETIMER_GAME))
    {
        reason = "game timer paused";
        return false;
    }

    if (ArkPlayer::GetInstancePtr())
    {
        const ArkPlayer& player = ArkPlayer::GetInstance();
        if (!player.m_input.m_modeStack.empty())
        {
            const ArkPlayerInput::Mode mode = player.m_input.m_modeStack.back().m_mode;
            if (mode != ArkPlayerInput::Mode::player)
            {
                reason = "input mode " + std::to_string(static_cast<int>(mode));
                return false;
            }
        }
    }

    reason = "ok";
    return true;
}
}

bool ModMain::ShouldSuppressArkPlayerDamage(float damage)
{
    if (!m_downedModeEnabled || damage <= 0.0f || !IsSessionGameplayReady() || !ArkPlayer::GetInstancePtr())
        return false;

    if (m_networkMode == CoopNetworkMode::Off)
        return false;

    ArkPlayer& player = ArkPlayer::GetInstance();
    if (m_localPlayerDowned)
        return true;

    const float health = player.GetHealth();
    const bool fatalDamage = health <= kDownedHealth || health - damage <= kDownedHealth;
    if (fatalDamage && m_allowNativeDeathFeedbackForDowned && !m_nativeDeathFeedbackActive)
        return false;

    return fatalDamage;
}

void ModMain::PruneDamageDedupe(float now)
{
    while (!m_damageDedupeEntries.empty() &&
        m_damageDedupeEntries.front().expiresAt <= now)
    {
        m_damageDedupeEntries.pop_front();
    }

    while (m_damageDedupeEntries.size() > kDamageDedupeMaxEntries)
        m_damageDedupeEntries.pop_front();
}

uint64_t ModMain::BuildProxyDamageDedupeKey(const HitInfo& hitInfo, float damage) const
{
    const uint64_t sourceStableId = ResolveDamageSourceStableId(hitInfo, nullptr);
    const uint32_t positionHash = BuildDamagePositionHash(hitInfo.pos);
    const uint64_t sourceKeyHash = BuildDamageSourceKeyHash(
        sourceStableId,
        0,
        hitInfo.uniqueId,
        hitInfo.projectileId != 0 ? hitInfo.projectileId : static_cast<uint32_t>(hitInfo.projectileClassId),
        static_cast<uint32_t>(hitInfo.type),
        0,
        positionHash);
    if (sourceKeyHash != 0)
    {
        uint64_t keyedHash = kDamageDedupeFnvOffset;
        HashDamageValue(keyedHash, sourceKeyHash);
        HashDamageSigned(keyedHash, DamageBucket(damage));
        return keyedHash == 0 ? 1 : keyedHash;
    }

    uint64_t hash = kDamageDedupeFnvOffset;
    HashDamageSigned(hash, DamageBucket(damage));
    HashDamageValue(hash, hitInfo.shooterId);
    HashDamageValue(hash, hitInfo.weaponId);
    HashDamageValue(hash, hitInfo.projectileId);
    HashDamageValue(hash, hitInfo.projectileClassId);
    HashDamageValue(hash, hitInfo.weaponClassId);
    HashDamageSigned(hash, hitInfo.type);
    HashDamageSigned(hash, hitInfo.material);
    HashDamageSigned(hash, hitInfo.bulletType);
    HashDamageSigned(hash, hitInfo.partId);
    HashDamageValue(hash, hitInfo.explosion ? 1u : 0u);
    HashDamageValue(hash, hitInfo.critical ? 1u : 0u);
    HashDamageValue(hash, hitInfo.hitViaProxy ? 1u : 0u);
    HashDamageValue(hash, hitInfo.aimed ? 1u : 0u);
    HashDamageValue(hash, hitInfo.knocksDown ? 1u : 0u);
    HashDamageValue(hash, hitInfo.knocksDownLeg ? 1u : 0u);
    return hash == 0 ? 1 : hash;
}

uint64_t ModMain::BuildRemoteDamageDedupeKey(const CoopProtocol::RemotePlayerDamagePacket& packet) const
{
    if ((packet.flags & CoopProtocol::kRemoteDamageFlagSourceKey) != 0)
    {
        uint64_t sourceKeyHash = BuildDamageSourceKeyHash(
            packet.sourceStableId,
            packet.sourceGeneration,
            packet.attackSeq,
            packet.projectileOrdinal,
            packet.damageType,
            0,
            packet.positionHash);
        if (sourceKeyHash == 0)
            sourceKeyHash = packet.sourceKeyHash;

        if (sourceKeyHash != 0)
        {
            uint64_t keyedHash = kDamageDedupeFnvOffset;
            HashDamageValue(keyedHash, sourceKeyHash);
            HashDamageSigned(keyedHash, DamageBucket(packet.damage));
            return keyedHash == 0 ? 1 : keyedHash;
        }
    }

    uint64_t hash = kDamageDedupeFnvOffset;
    HashDamageSigned(hash, DamageBucket(packet.damage));
    HashDamageValue(hash, packet.shooterId);
    HashDamageValue(hash, packet.weaponId);
    HashDamageValue(hash, packet.projectileId);
    HashDamageValue(hash, packet.projectileClassId);
    HashDamageValue(hash, packet.weaponClassId);
    HashDamageSigned(hash, packet.hitType);
    HashDamageSigned(hash, packet.material);
    HashDamageSigned(hash, packet.bulletType);
    HashDamageSigned(hash, packet.partId);
    HashDamageValue(hash, (packet.flags & CoopProtocol::kRemoteDamageFlagExplosion) != 0 ? 1u : 0u);
    HashDamageValue(hash, (packet.flags & CoopProtocol::kRemoteDamageFlagCritical) != 0 ? 1u : 0u);
    HashDamageValue(hash, (packet.flags & CoopProtocol::kRemoteDamageFlagHitViaProxy) != 0 ? 1u : 0u);
    HashDamageValue(hash, (packet.flags & CoopProtocol::kRemoteDamageFlagAimed) != 0 ? 1u : 0u);
    HashDamageValue(hash, (packet.flags & CoopProtocol::kRemoteDamageFlagKnocksDown) != 0 ? 1u : 0u);
    HashDamageValue(hash, (packet.flags & CoopProtocol::kRemoteDamageFlagKnocksDownLeg) != 0 ? 1u : 0u);
    return hash == 0 ? 1 : hash;
}

uint64_t ModMain::ResolveDamageSourceStableId(const HitInfo& hitInfo, bool* outStable) const
{
    if (outStable)
        *outStable = false;

    const EntityId candidates[] = {
        hitInfo.projectileId,
        hitInfo.weaponId,
        hitInfo.shooterId,
    };

    for (EntityId entityId : candidates)
    {
        if (entityId == INVALID_ENTITYID || entityId == 0 || !gEnv || !gEnv->pEntitySystem)
            continue;

        IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId);
        if (!entity)
            continue;

        EntityGUID guid = 0;
        std::string reason;
        if (TryGuardedCall("damage source stable IEntity::GetGuid", [entity]() -> EntityGUID { return entity->GetGuid(); }, guid, &reason) &&
            guid != 0)
        {
            if (outStable)
                *outStable = true;
            return static_cast<uint64_t>(guid);
        }
    }

    const bool hasRuntimeSourceId =
        (hitInfo.projectileId != INVALID_ENTITYID && hitInfo.projectileId != 0) ||
        (hitInfo.weaponId != INVALID_ENTITYID && hitInfo.weaponId != 0) ||
        (hitInfo.shooterId != INVALID_ENTITYID && hitInfo.shooterId != 0);

    uint64_t hash = kDamageDedupeFnvOffset;
    HashDamageValue(hash, m_localLevelId);
    HashDamageValue(hash, hitInfo.projectileId);
    HashDamageValue(hash, hitInfo.weaponId);
    HashDamageValue(hash, hitInfo.shooterId);
    if (!hasRuntimeSourceId)
    {
        HashDamageValue(hash, hitInfo.projectileClassId);
        HashDamageValue(hash, hitInfo.weaponClassId);
    }
    HashDamageSigned(hash, hitInfo.type);
    return hash == 0 ? 1 : hash;
}

uint32_t ModMain::BuildDamagePositionHash(const Vec3& position) const
{
    uint32_t hash = kDamagePositionFnvOffset;
    HashDamagePositionValue(hash, PackSignedDamageCoordinate(position.x));
    HashDamagePositionValue(hash, PackSignedDamageCoordinate(position.y));
    HashDamagePositionValue(hash, PackSignedDamageCoordinate(position.z));
    return hash == 0 ? 1u : hash;
}

uint32_t ModMain::BuildDamageTimeBucket() const
{
    const float now = DamageDedupeNow();
    if (!std::isfinite(now) || now < 0.0f)
        return 0u;
    return static_cast<uint32_t>(std::floor(now / kDamageTimeBucketSeconds));
}

uint64_t ModMain::BuildDamageSourceKeyHash(
    uint64_t sourceStableId,
    uint32_t sourceGeneration,
    uint32_t attackSeq,
    uint32_t projectileOrdinal,
    uint32_t damageType,
    uint32_t timeBucket,
    uint32_t positionHash) const
{
    if (sourceStableId == 0 && attackSeq == 0 && projectileOrdinal == 0 && damageType == 0 && positionHash == 0)
        return 0;

    uint64_t hash = kDamageDedupeFnvOffset;
    HashDamageValue(hash, m_localLevelId);
    HashDamageValue(hash, sourceStableId);
    HashDamageValue(hash, sourceGeneration);
    HashDamageValue(hash, attackSeq);
    HashDamageValue(hash, projectileOrdinal);
    HashDamageValue(hash, damageType);
    HashDamageValue(hash, timeBucket);
    HashDamageValue(hash, positionHash);
    return hash == 0 ? 1 : hash;
}

void ModMain::RememberDamageDedupeEvent(uint64_t key, float damage, bool localPlayerRaw, const char* source, float ttlSeconds)
{
    if (!m_damageDedupeEnabled || damage <= 0.0f)
        return;

    const float now = DamageDedupeNow();
    PruneDamageDedupe(now);

    const int bucket = DamageBucket(damage);
    for (DamageDedupeEntry& existing : m_damageDedupeEntries)
    {
        if (existing.key == key &&
            existing.damageBucket == bucket &&
            existing.localPlayerRaw == localPlayerRaw)
        {
            existing.expiresAt = now + std::max(0.05f, ttlSeconds);
            existing.source = source && source[0] ? source : "-";
            return;
        }
    }

    DamageDedupeEntry entry = {};
    entry.key = key;
    entry.damageBucket = bucket;
    entry.expiresAt = now + std::max(0.05f, ttlSeconds);
    entry.localPlayerRaw = localPlayerRaw;
    entry.source = source && source[0] ? source : "-";
    m_damageDedupeEntries.push_back(std::move(entry));
    while (m_damageDedupeEntries.size() > kDamageDedupeMaxEntries)
        m_damageDedupeEntries.pop_front();
}

bool ModMain::ShouldDropDuplicateRemotePlayerDamage(const CoopProtocol::RemotePlayerDamagePacket& packet)
{
    if (!m_damageDedupeEnabled || packet.damage <= 0.0f)
        return false;

    const float now = DamageDedupeNow();
    PruneDamageDedupe(now);

    const uint64_t key = BuildRemoteDamageDedupeKey(packet);
    const int bucket = DamageBucket(packet.damage);
    const bool likelyProxyDuplicate =
        (packet.flags & (CoopProtocol::kRemoteDamageFlagExplosion | CoopProtocol::kRemoteDamageFlagHitViaProxy)) != 0 ||
        packet.projectileId != 0 ||
        packet.weaponId != 0 ||
        packet.projectileClassId != 0 ||
        packet.weaponClassId != 0 ||
        packet.shooterId != 0;

    for (auto it = m_damageDedupeEntries.begin(); it != m_damageDedupeEntries.end(); ++it)
    {
        if (!it->localPlayerRaw && it->key != 0 && it->key == key)
        {
            ++m_damageDedupeRemoteDrops;
            ++m_damageDedupeExactDrops;
            m_lastDamageDedupeEvent =
                "drop remote damage exact source=" + it->source +
                " damage=" + std::to_string(packet.damage);
            m_damageDedupeEntries.erase(it);
            return true;
        }

        if (likelyProxyDuplicate && it->localPlayerRaw && std::abs(it->damageBucket - bucket) <= 1)
        {
            ++m_damageDedupeRemoteDrops;
            ++m_damageDedupeRawDrops;
            m_lastDamageDedupeEvent =
                "drop remote damage raw-local source=" + it->source +
                " damage=" + std::to_string(packet.damage);
            m_damageDedupeEntries.erase(it);
            return true;
        }
    }

    m_lastDamageDedupeEvent =
        "accept remote damage damage=" + std::to_string(packet.damage) +
        " entries=" + std::to_string(m_damageDedupeEntries.size());
    return false;
}

void ModMain::OnArkPlayerSignalHitObserved(const HitInfo& hitInfo, uint64_t packageId, EntityId packageSourceId)
{
    if (!m_damageDedupeEnabled ||
        m_applyingRemotePlayerDamage ||
        hitInfo.damage <= 0.0f ||
        m_networkMode == CoopNetworkMode::Off ||
        !IsSessionGameplayReady())
    {
        return;
    }

    ++m_damageDedupeSignalObservations;

    IEntity* playerEntity = nullptr;
    std::string reason;
    if (!TryGuardedCall(
        "player signal damage local entity",
        []() -> IEntity* { return ArkPlayer::GetInstancePtr() ? ArkPlayer::GetInstance().GetEntity() : nullptr; },
        playerEntity,
        &reason) ||
        !playerEntity)
    {
        ++m_damageDedupeSignalSkips;
        m_lastPlayerSignalDamageEvent = "skip no local player entity";
        return;
    }

    const EntityId playerEntityId = playerEntity->GetId();
    if (hitInfo.targetId != INVALID_ENTITYID &&
        hitInfo.targetId != 0 &&
        hitInfo.targetId != playerEntityId)
    {
        ++m_damageDedupeSignalSkips;
        m_lastPlayerSignalDamageEvent =
            "skip target " + std::to_string(hitInfo.targetId) +
            " local " + std::to_string(playerEntityId);
        return;
    }

    HitInfo resolved = hitInfo;
    if (resolved.targetId == INVALID_ENTITYID || resolved.targetId == 0)
        resolved.targetId = playerEntityId;
    if (resolved.shooterId == INVALID_ENTITYID || resolved.shooterId == 0)
        resolved.shooterId = packageSourceId;
    if (!std::isfinite(resolved.pos.x) ||
        !std::isfinite(resolved.pos.y) ||
        !std::isfinite(resolved.pos.z) ||
        (std::abs(resolved.pos.x) < 0.001f &&
            std::abs(resolved.pos.y) < 0.001f &&
            std::abs(resolved.pos.z) < 0.001f))
    {
        resolved.pos = playerEntity->GetWorldPos();
    }

    const uint64_t key = BuildProxyDamageDedupeKey(resolved, resolved.damage);
    RememberDamageDedupeEvent(key, resolved.damage, false, "local_player_signal_hit", kDamageDedupeExactTtlSeconds);
    ++m_damageDedupeSignalKeys;
    m_lastPlayerSignalDamageEvent =
        "signal hit key=" + std::to_string(key) +
        " damage=" + std::to_string(resolved.damage) +
        " pkg=" + std::to_string(packageId) +
        " source=" + std::to_string(packageSourceId) +
        " target=" + std::to_string(resolved.targetId);
}

// CArkSignalContext is a 16-byte { int32 variant discriminant, 8-byte payload }:
//   0 = boost::blank, 1 = HitInfo const*, 2 = SExplosionContainer*.
// The native GetDamage*/GetHitInfo() accessors abort the entire process (SEH
// cannot catch it) whenever the variant does not hold a HitInfo const*, so
// inspect the payload directly instead of calling them.
static bool TryGetSignalContextHitInfo(
    const ArkSignalSystem::CArkSignalContext& context,
    const HitInfo** outHitInfo,
    int* outVariantIndex)
{
    const uint8_t* base = reinterpret_cast<const uint8_t*>(&context);
    int32_t index = -1;
    if (!CoopRuntimeGuards::TryReadRuntimeValue(
            reinterpret_cast<const int32_t const*>(base), index))
    {
        return false;
    }
    const void* payload = nullptr;
    if (!CoopRuntimeGuards::TryReadRuntimeValue(
            reinterpret_cast<const void* const*>(base + 8), payload))
    {
        return false;
    }
    if (outVariantIndex)
        *outVariantIndex = index;
    if (index != 1 ||
        !CoopRuntimeGuards::IsReadableRuntimePointer(payload, sizeof(HitInfo)))
    {
        return false;
    }
    *outHitInfo = static_cast<const HitInfo*>(payload);
    return true;
}

void ModMain::OnArkSignalPackageObserved(
    EntityId targetEntityId,
    EntityId senderEntityId,
    EntityId instigatorEntityId,
    uint64_t packageId,
    const ArkSignalSystem::CArkSignalContext& context,
    float scale,
    const char* stage)
{
    if (!m_damageDedupeEnabled ||
        m_applyingRemotePlayerDamage ||
        m_networkMode == CoopNetworkMode::Off ||
        !IsSessionGameplayReady())
    {
        return;
    }

    IEntity* playerEntity = nullptr;
    std::string reason;
    if (!TryGuardedCall(
        "signal package local player entity",
        []() -> IEntity* { return ArkPlayer::GetInstancePtr() ? ArkPlayer::GetInstance().GetEntity() : nullptr; },
        playerEntity,
        &reason) ||
        !playerEntity)
    {
        return;
    }

    const EntityId playerEntityId = playerEntity->GetId();
    if (targetEntityId != playerEntityId)
        return;

    ++m_damageDedupeSignalObservations;

    PendingLocalDamageSignal pending = {};
    pending.valid = true;
    pending.expiresAt = DamageDedupeNow() + kDamageSignalPendingTtlSeconds;
    pending.targetId = playerEntityId;
    pending.sourceId = senderEntityId;
    pending.instigatorId = instigatorEntityId;
    pending.packageId = packageId;
    pending.scale = std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
    pending.position = playerEntity->GetWorldPos();

    const HitInfo* hitInfo = nullptr;
    int variantIndex = -1;
    TryGetSignalContextHitInfo(context, &hitInfo, &variantIndex);
    if (hitInfo)
    {
        if (hitInfo->shooterId != INVALID_ENTITYID && hitInfo->shooterId != 0)
            pending.instigatorId = hitInfo->shooterId;
        if (hitInfo->weaponId != INVALID_ENTITYID && hitInfo->weaponId != 0)
            pending.weaponId = hitInfo->weaponId;
        pending.packageOrdinal = hitInfo->uniqueId;
        if (hitInfo->projectileId != INVALID_ENTITYID && hitInfo->projectileId != 0)
            pending.sourceId = hitInfo->projectileId;
        else if (hitInfo->weaponId != INVALID_ENTITYID && hitInfo->weaponId != 0)
            pending.sourceId = hitInfo->weaponId;
        else if (hitInfo->shooterId != INVALID_ENTITYID && hitInfo->shooterId != 0)
            pending.sourceId = hitInfo->shooterId;
        if (std::isfinite(hitInfo->pos.x) &&
            std::isfinite(hitInfo->pos.y) &&
            std::isfinite(hitInfo->pos.z) &&
            (std::abs(hitInfo->pos.x) > 0.001f ||
                std::abs(hitInfo->pos.y) > 0.001f ||
                std::abs(hitInfo->pos.z) > 0.001f))
        {
            pending.position = hitInfo->pos;
        }
        pending.direction = hitInfo->dir;
        pending.hitType = hitInfo->type;
    }
    else
    {
        // The context variant does not hold a HitInfo const* (blank or
        // explosion context, e.g. healing/water/sink signal packages). The
        // native damage getters abort the process on such contexts, so the
        // pending fields stay at the fallback values set above and only a
        // breadcrumb is written.
        const std::string breadcrumb =
            "signal_ctx_no_hit pkg=" + std::to_string(packageId) +
            " variant=" + std::to_string(variantIndex);
        CoopRuntimeLog::WriteRateLimited("signal_ctx_no_hit", breadcrumb, 1.0, 3);
    }

    if ((pending.sourceId == INVALID_ENTITYID || pending.sourceId == 0) &&
        pending.instigatorId != INVALID_ENTITYID &&
        pending.instigatorId != 0)
    {
        pending.sourceId = pending.instigatorId;
    }

    m_pendingLocalDamageSignal = pending;
    m_lastPlayerSignalDamageEvent =
        "signal_pkg stage=" + std::string(stage && stage[0] ? stage : "-") +
        " pkg=" + std::to_string(packageId) +
        " source=" + std::to_string(pending.sourceId) +
        " instigator=" + std::to_string(pending.instigatorId) +
        " target=" + std::to_string(targetEntityId);
}

void ModMain::OnArkSignalPackageObserved(
    EntityId targetEntityId,
    const ArkSignalSystem::Package& package,
    const char* stage)
{
    // Cheap pre-check before any guarded (VirtualQuery-backed) reads below:
    // the forwarded overload early-outs on these same conditions, so in
    // offline / disabled states every guarded call here was pure overhead.
    // This hook fires for EVERY signal package delivered to ANY receiver
    // (lights, doors, physics props), so keep the offline path allocation-
    // and syscall-free.
    if (!m_damageDedupeEnabled || m_networkMode == CoopNetworkMode::Off)
        return;

    uint64_t packageId = package.m_id;
    TryGuardedCall(
        "signal package GetId",
        [&package]() -> uint64_t { return package.GetId(); },
        packageId,
        nullptr);

    EntityId instigatorEntityId = package.m_sourceId;
    // The native GetDamageInstigatorId() aborts the process when the context
    // variant does not hold a HitInfo const*, so read the payload directly.
    {
        const HitInfo* contextHitInfo = nullptr;
        if (TryGetSignalContextHitInfo(package.m_context, &contextHitInfo, nullptr))
        {
            unsigned contextInstigator = 0;
            if (CoopRuntimeGuards::TryReadRuntimeValue(
                    &contextHitInfo->shooterId, contextInstigator) &&
                contextInstigator != INVALID_ENTITYID &&
                contextInstigator != 0)
            {
                instigatorEntityId = contextInstigator;
            }
        }
    }

    OnArkSignalPackageObserved(
        targetEntityId,
        package.m_sourceId,
        instigatorEntityId,
        packageId,
        package.m_context,
        1.0f,
        stage);
}

void ModMain::OnArkPlayerDamageObserved(float damage)
{
    if (!m_damageDedupeEnabled ||
        m_applyingRemotePlayerDamage ||
        damage <= 0.0f ||
        m_networkMode == CoopNetworkMode::Off ||
        !IsSessionGameplayReady())
    {
        return;
    }

    ++m_damageDedupeLocalObservations;
    RememberDamageDedupeEvent(0, damage, true, "local_player_take_damage", kDamageDedupeRawLocalTtlSeconds);
    m_lastDamageDedupeEvent =
        "observe local player damage=" + std::to_string(damage) +
        " entries=" + std::to_string(m_damageDedupeEntries.size());
}

void ModMain::OnArkPlayerHealthDropObserved(const ArkPlayerHealthComponent* healthComponent, float newHealth, const char* source)
{
    if (!m_damageDedupeEnabled ||
        m_applyingRemotePlayerDamage ||
        m_internalPlayerHealthWrite ||
        m_networkMode == CoopNetworkMode::Off ||
        !IsSessionGameplayReady() ||
        !healthComponent ||
        !IsLocalPlayerHealthComponent(healthComponent))
    {
        return;
    }

    float previousHealth = 0.0f;
    if (!TryGuardedCall(
        "player health drop previous health",
        [healthComponent]() -> float { return healthComponent->GetHealth(); },
        previousHealth,
        nullptr))
    {
        return;
    }

    if (!std::isfinite(previousHealth) || !std::isfinite(newHealth))
        return;

    constexpr float kHealthDropEpsilon = 0.05f;
    if (newHealth + kHealthDropEpsilon >= previousHealth)
        return;

    const float damage = std::min(previousHealth - newHealth, 10000.0f);
    const float now = DamageDedupeNow();
    if (!m_pendingLocalDamageSignal.valid || m_pendingLocalDamageSignal.expiresAt < now)
    {
        m_pendingLocalDamageSignal.valid = false;
        OnArkPlayerDamageObserved(damage);
        return;
    }

    IEntity* playerEntity = nullptr;
    std::string reason;
    if (!TryGuardedCall(
        "player health drop local entity",
        []() -> IEntity* { return ArkPlayer::GetInstancePtr() ? ArkPlayer::GetInstance().GetEntity() : nullptr; },
        playerEntity,
        &reason) ||
        !playerEntity)
    {
        ++m_damageDedupeSignalSkips;
        m_lastPlayerSignalDamageEvent = "health_drop skip no local player";
        return;
    }

    HitInfo resolved = {};
    resolved.targetId = playerEntity->GetId();
    resolved.damage = damage;
    resolved.shooterId =
        (m_pendingLocalDamageSignal.instigatorId != INVALID_ENTITYID && m_pendingLocalDamageSignal.instigatorId != 0) ?
        m_pendingLocalDamageSignal.instigatorId :
        m_pendingLocalDamageSignal.sourceId;
    resolved.weaponId = m_pendingLocalDamageSignal.weaponId;
    resolved.projectileId = m_pendingLocalDamageSignal.sourceId;
    resolved.uniqueId =
        m_pendingLocalDamageSignal.packageOrdinal != 0 ?
        m_pendingLocalDamageSignal.packageOrdinal :
        static_cast<uint32_t>(m_pendingLocalDamageSignal.packageId & 0xffffffffu);
    resolved.type = m_pendingLocalDamageSignal.hitType;
    resolved.pos = m_pendingLocalDamageSignal.position;
    resolved.dir = m_pendingLocalDamageSignal.direction;
    if (!std::isfinite(resolved.pos.x) ||
        !std::isfinite(resolved.pos.y) ||
        !std::isfinite(resolved.pos.z) ||
        (std::abs(resolved.pos.x) < 0.001f &&
            std::abs(resolved.pos.y) < 0.001f &&
            std::abs(resolved.pos.z) < 0.001f))
    {
        resolved.pos = playerEntity->GetWorldPos();
    }

    bool coverageStableDamageSource = false;
    ResolveDamageSourceStableId(resolved, &coverageStableDamageSource);
    m_coverageDiscovery.RecordDamageSource(
        coverageStableDamageSource,
        source && source[0] ? source : "local_signal_health_drop");

    const uint64_t key = BuildProxyDamageDedupeKey(resolved, damage);
    RememberDamageDedupeEvent(key, damage, false, "local_signal_health_drop", kDamageDedupeExactTtlSeconds);
    ++m_damageDedupeSignalKeys;
    ++m_damageDedupeHealthSignals;
    m_lastPlayerSignalDamageEvent =
        "health_drop_key=" + std::to_string(key) +
        " damage=" + std::to_string(damage) +
        " pkg=" + std::to_string(m_pendingLocalDamageSignal.packageId) +
        " source=" + std::to_string(m_pendingLocalDamageSignal.sourceId) +
        " instigator=" + std::to_string(m_pendingLocalDamageSignal.instigatorId) +
        " via=" + std::string(source && source[0] ? source : "-");
    m_pendingLocalDamageSignal.valid = false;
}

void ModMain::OnArkPlayerDamageSuppressed(float damage)
{
    if (!ArkPlayer::GetInstancePtr())
        return;

    if (!m_localPlayerDowned)
    {
        SetLocalPlayerHealthSafe(m_downedEngineHealthFloor, "fatal damage clamp");
        EnterLocalDowned(0, true);
        m_networkStatus = "local player downed by fatal damage " + std::to_string(damage);
    }
    else
    {
        if (ArkPlayer::GetInstance().GetHealth() < m_downedEngineHealthFloor)
            SetLocalPlayerHealthSafe(m_downedEngineHealthFloor, "suppressed downed damage clamp");
        m_networkStatus = "ignored damage while downed " + std::to_string(damage);
    }
}

bool ModMain::ShouldSuppressArkPlayerHealthSet(const ArkPlayerHealthComponent* healthComponent, float health, bool damagedByRecyclerGrenade)
{
    (void)damagedByRecyclerGrenade;

    if (!m_downedModeEnabled || !IsLocalPlayerHealthComponent(healthComponent))
        return false;

    if (m_internalPlayerHealthWrite)
        return false;

    if (m_networkMode == CoopNetworkMode::Off)
        return false;

    if (m_teamWipe && m_networkMode == CoopNetworkMode::Host)
        return false;

    if (m_allowNativeDeathFeedbackForDowned && !m_localPlayerDowned)
        return false;

    return health <= kDownedHealth || (m_localPlayerDowned && health < m_downedEngineHealthFloor);
}

void ModMain::OnArkPlayerHealthSetSuppressed(const ArkPlayerHealthComponent* healthComponent, float health, bool damagedByRecyclerGrenade)
{
    (void)healthComponent;
    (void)damagedByRecyclerGrenade;

    ++m_suppressedDownedHealthSets;
    SetLocalPlayerHealthSafe(m_downedEngineHealthFloor, "suppressed health set");

    if (!m_localPlayerDowned)
    {
        EnterLocalDowned(0, true, false);
        m_networkStatus = "suppressed fatal health set " + std::to_string(health);
    }
    else
    {
        m_networkStatus = "clamped downed health set " + std::to_string(health);
    }
}

bool ModMain::ShouldRunArkPlayerNativeDeathFeedback(const ArkPlayerHealthComponent* healthComponent, bool byRecyclerGrenade) const
{
    (void)byRecyclerGrenade;

    if (!m_downedModeEnabled || !m_allowNativeDeathFeedbackForDowned || !IsLocalPlayerHealthComponent(healthComponent))
        return false;

    if (m_networkMode == CoopNetworkMode::Off)
        return false;

    if (m_teamWipe && m_networkMode == CoopNetworkMode::Host)
        return false;

    return !m_localPlayerDowned && !m_nativeDeathFeedbackActive;
}

void ModMain::OnArkPlayerNativeDeathFeedbackStarting(const ArkPlayerHealthComponent* healthComponent, bool byRecyclerGrenade)
{
    (void)healthComponent;
    (void)byRecyclerGrenade;

    m_nativeDeathFeedbackActive = true;
    ++m_nativeDeathFeedbackRuns;
    m_networkStatus = "native death feedback starting";
}

void ModMain::OnArkPlayerNativeDeathFeedbackFinished(const ArkPlayerHealthComponent* healthComponent, bool byRecyclerGrenade)
{
    (void)healthComponent;

    m_nativeDeathFeedbackActive = false;
    RecoverLocalPlayerFromNativeDeathState(byRecyclerGrenade ? "native recycler death feedback" : "native death feedback");
    SetLocalPlayerHealthSafe(m_downedEngineHealthFloor, byRecyclerGrenade ? "native recycler death downed clamp" : "native death downed clamp");
    if (!m_localPlayerDowned)
        EnterLocalDowned(byRecyclerGrenade ? CoopProtocol::kPlayerStatusFlagUnreachableRecovery : 0, true, false);

    m_networkStatus = byRecyclerGrenade ? "converted native recycler death to downed" : "converted native death to downed";
}

bool ModMain::ShouldSuppressArkPlayerDeath(const ArkPlayerHealthComponent* healthComponent, bool byRecyclerGrenade)
{
    (void)byRecyclerGrenade;

    if (!m_downedModeEnabled || !IsLocalPlayerHealthComponent(healthComponent))
        return false;

    if (m_networkMode == CoopNetworkMode::Off)
        return false;

    if (m_allowNativeDeathFeedbackForDowned && !m_localPlayerDowned && !m_nativeDeathFeedbackActive)
        return false;

    return !(m_teamWipe && m_networkMode == CoopNetworkMode::Host);
}

void ModMain::OnArkPlayerDeathSuppressed(const ArkPlayerHealthComponent* healthComponent, bool byRecyclerGrenade)
{
    (void)healthComponent;

    ++m_suppressedDownedDeathEvents;
    SetLocalPlayerHealthSafe(m_downedEngineHealthFloor, byRecyclerGrenade ? "suppressed recycler death" : "suppressed player death");
    if (!m_localPlayerDowned)
        EnterLocalDowned(byRecyclerGrenade ? CoopProtocol::kPlayerStatusFlagUnreachableRecovery : 0, true, false);

    m_networkStatus = byRecyclerGrenade ? "suppressed recycler death into downed" : "suppressed player death into downed";
}

bool ModMain::ShouldSuppressArkPlayerForceKill(const ArkPlayerHealthComponent* healthComponent)
{
    if (!m_downedModeEnabled || !IsLocalPlayerHealthComponent(healthComponent))
        return false;

    if (m_networkMode == CoopNetworkMode::Off)
        return false;

    return !(m_teamWipe && m_networkMode == CoopNetworkMode::Host);
}

void ModMain::OnArkPlayerForceKillSuppressed(const ArkPlayerHealthComponent* healthComponent)
{
    (void)healthComponent;

    ++m_suppressedDownedForceKills;
    SetLocalPlayerHealthSafe(m_downedEngineHealthFloor, "suppressed force kill");
    if (!m_localPlayerDowned)
        EnterLocalDowned(0, true, false);

    m_networkStatus = "suppressed force kill into downed";
}

bool ModMain::ShouldSuppressArkPlayerRagdollize(const ArkPlayer* player) const
{
    if (!m_downedModeEnabled || !player || !ArkPlayer::GetInstancePtr())
        return false;

    if (player != ArkPlayer::GetInstancePtr())
        return false;

    if (m_networkMode == CoopNetworkMode::Off)
        return false;

    if (m_teamWipe && m_networkMode == CoopNetworkMode::Host)
        return false;

    return m_localPlayerDowned || m_nativeDeathFeedbackActive;
}

void ModMain::OnArkPlayerRagdollizeSuppressed(float verticalSpeed)
{
    (void)verticalSpeed;

    ++m_suppressedDownedRagdollizes;
    if (!m_localPlayerDowned && !m_nativeDeathFeedbackActive)
    {
        SetLocalPlayerHealthSafe(m_downedEngineHealthFloor, "ragdoll converted to downed");
        EnterLocalDowned(0, true, false);
    }
    m_networkStatus = "suppressed player ragdoll for downed";
}

bool ModMain::ShouldOverrideArkPlayerIsDead(const ArkPlayer* player) const
{
    if (!m_downedModeEnabled || !player || !ArkPlayer::GetInstancePtr())
        return false;

    if (player != ArkPlayer::GetInstancePtr())
        return false;

    if (m_networkMode == CoopNetworkMode::Off)
        return false;

    return m_localPlayerDowned && !(m_teamWipe && m_networkMode == CoopNetworkMode::Host) && !m_nativeDeathFeedbackActive;
}

bool ModMain::ShouldOverrideArkPlayerHealthIsDead(const ArkPlayerHealthComponent* healthComponent) const
{
    if (!m_downedModeEnabled || !IsLocalPlayerHealthComponent(healthComponent))
        return false;

    if (m_networkMode == CoopNetworkMode::Off)
        return false;

    return m_localPlayerDowned && !(m_teamWipe && m_networkMode == CoopNetworkMode::Host) && !m_nativeDeathFeedbackActive;
}

void ModMain::OnArkPlayerIsDeadOverridden(const char* source)
{
    ++m_overriddenDownedIsDeadQueries;
    m_lastLocalAttentionClearStage = source ? source : "isdead";
}

bool ModMain::ShouldSuppressArkPlayerDeathMovementState() const
{
    if (!m_downedModeEnabled)
        return false;

    if (m_networkMode == CoopNetworkMode::Off)
        return false;

    if (m_teamWipe && m_networkMode == CoopNetworkMode::Host)
        return false;

    return m_localPlayerDowned && !m_nativeDeathFeedbackActive;
}

void ModMain::OnArkPlayerDeathMovementStateSuppressed(const char* source)
{
    ++m_suppressedDownedDeathMovementStates;
    if (!m_localPlayerDowned)
        EnterLocalDowned(0, true, false);

    RecoverLocalPlayerFromNativeDeathState(source ? source : "death movement state");
    m_networkStatus = std::string("suppressed player death movement state: ") + (source ? source : "unknown");
}

bool ModMain::ShouldSuppressArkPlayerDeathScreenOpen()
{
    if (!m_downedModeEnabled)
        return false;

    if (m_networkMode == CoopNetworkMode::Off)
        return false;

    if (m_teamWipe && m_networkMode == CoopNetworkMode::Host)
        return false;

    if (!m_localPlayerDowned && ArkPlayer::GetInstancePtr() && ArkPlayer::GetInstance().GetHealth() <= kDownedHealth)
        return true;

    return m_localPlayerDowned;
}

bool ModMain::ShouldNeutralizeNativeTimeScale(float scale) const
{
    if (!m_blockNativeTimeScaleWhileDowned || !m_localPlayerDowned)
        return false;

    if (m_networkMode == CoopNetworkMode::Off)
        return false;

    if (m_peerConnectionLostFreezeActive)
        return false;

    return std::abs(scale - 1.0f) > 0.001f;
}

void ModMain::RecordTimeScaleOverride(unsigned timers, float scale, int handle, bool neutralized)
{
    ++m_timeScaleOverrideCount;
    if (neutralized)
        ++m_timeScaleNeutralizedCount;

    m_lastTimeScaleTimers = timers;
    m_lastTimeScaleValue = scale;
    m_lastTimeScaleHandle = handle;
    m_lastTimeScaleEvent = neutralized ? "override neutralized" : "override";
}

void ModMain::RecordTimeScaleUpdate(int handle, float scale, bool neutralized)
{
    ++m_timeScaleUpdateCount;
    if (neutralized)
        ++m_timeScaleNeutralizedCount;

    m_lastTimeScaleValue = scale;
    m_lastTimeScaleHandle = handle;
    m_lastTimeScaleEvent = neutralized ? "update neutralized" : "update";
}

void ModMain::RecordTimeScaleClear(int handle)
{
    ++m_timeScaleClearCount;
    m_lastTimeScaleHandle = handle;
    m_lastTimeScaleValue = 1.0f;
    m_lastTimeScaleEvent = "clear";
}

bool ModMain::ShouldSuppressFocusModeStart(bool openMenu) const
{
    (void)openMenu;

    return m_blockNativeTimeScaleWhileDowned &&
        m_localPlayerDowned &&
        m_networkMode != CoopNetworkMode::Off;
}

void ModMain::RecordFocusModeStart(bool openMenu, bool suppressed, bool result)
{
    (void)openMenu;

    ++m_focusModeStartCount;
    if (suppressed)
        ++m_focusModeSuppressedStartCount;
    m_lastTimeScaleEvent = suppressed ? "focus start suppressed" : (result ? "focus start" : "focus start failed");
}

void ModMain::RecordFocusModeStop(bool fromTargeting)
{
    (void)fromTargeting;

    ++m_focusModeStopCount;
    m_lastTimeScaleEvent = "focus stop";
}

void ModMain::StopLocalFocusModeForDowned(const char* reason)
{
    if (!m_blockNativeTimeScaleWhileDowned || m_networkMode == CoopNetworkMode::Off || !ArkPlayer::GetInstancePtr())
        return;

    try
    {
        ArkPlayer& player = ArkPlayer::GetInstance();
        ArkFocusModeComponent* focusMode = player.m_playerComponent.m_pFocusModeComponent.get();
        if (!focusMode)
            return;

        const bool wasTargeting = focusMode->IsTargeting();
        focusMode->Stop(wasTargeting);

        ArkTimeScaleManager* timeScaleManager = g_pGame ? g_pGame->m_pArkTimeScaleManager.get() : nullptr;
        ResetFocusTimeScaler(focusMode->m_gameTimeScaler, timeScaleManager, m_focusTimeScaleClearCount);
        ResetFocusTimeScaler(focusMode->m_playerTimeScaler, timeScaleManager, m_focusTimeScaleClearCount);

        ++m_focusModeCleanupCount;
        m_lastTimeScaleHandle = -1;
        m_lastTimeScaleValue = 1.0f;
        m_lastTimeScaleEvent = std::string("focus cleanup ") + (reason ? reason : "unknown");
    }
    catch (...)
    {
        m_networkStatus = std::string("downed focus cleanup threw during ") + (reason ? reason : "unknown");
        LogCoop(m_networkStatus);
    }
}

void ModMain::ClearLocalPlayerModalStateAfterSidecarApply(const char* reason)
{
    if (m_networkMode == CoopNetworkMode::Off || !ArkPlayer::GetInstancePtr())
        return;

    try
    {
        ArkPlayer& player = ArkPlayer::GetInstance();
        if (!m_receivedPlayerStateInventoryPreparedForNativeLoad ||
            !m_receivedPlayerStateAbilitiesPreparedForNativeLoad)
        {
            std::string repairDetail;
            RunSidecarPostDeserializeRepairs(player, repairDetail);
            if (!repairDetail.empty())
                LogCoop("sidecar native post-deserialize repair warnings: " + repairDetail);
        }
        NormalizeHealthFeedbackAfterRestore(player.m_playerComponent.GetHealthComponent(), player.GetHealth());

        ArkFocusModeComponent* focusMode = player.m_playerComponent.m_pFocusModeComponent.get();
        if (focusMode)
        {
            const bool wasTargeting = focusMode->IsTargeting();
            focusMode->Stop(wasTargeting);
            focusMode->EnableInputMode(false);

            ArkTimeScaleManager* timeScaleManager = g_pGame ? g_pGame->m_pArkTimeScaleManager.get() : nullptr;
            ResetFocusTimeScaler(focusMode->m_gameTimeScaler, timeScaleManager, m_focusTimeScaleClearCount);
            ResetFocusTimeScaler(focusMode->m_playerTimeScaler, timeScaleManager, m_focusTimeScaleClearCount);
        }

        CArkPsiComponent& psiComponent = player.m_playerComponent.GetPsiComponent();
        psiComponent.Stop();

        ArkPsiPowerComponent& psiPower = player.GetPsiPowerComponent();
        psiPower.Stop();
        psiPower.StopLatentPowers();
        psiPower.ClearUITargets();
        psiPower.DisableTargetedPowers(false);
        psiPower.m_bPreventWeaponFireOnHold = false;
        psiPower.m_bTargetedPowersDisabled = false;

        ArkPlayerInput& input = player.m_input;
        // A joining client deserializes the Host's ArkPlayer before its own
        // account sidecar is applied. World-UI, examination and hacking modes
        // are interaction-local and must never survive that handoff. During a
        // regular in-world sidecar refresh, preserve those valid local modes
        // and clear only transient power/focus modes as before.
        const bool clearInheritedModalState =
            m_pendingReceivedPlayerStateApply ||
            m_forceNextPlayerSidecarResetTransientState;
        const auto shouldClearMode = [clearInheritedModalState](ArkPlayerInput::Mode mode)
        {
            if (mode == ArkPlayerInput::Mode::player)
                return false;
            if (clearInheritedModalState)
                return true;
            return mode == ArkPlayerInput::Mode::focusmode ||
                mode == ArkPlayerInput::Mode::psi_scanning_fanfare ||
                mode == ArkPlayerInput::Mode::ether_duplicate ||
                mode == ArkPlayerInput::Mode::mimic_grab;
        };
        std::vector<int> modalHandles;
        modalHandles.reserve(input.m_modeStack.size());
        for (const ArkPlayerInput::ModeAndHandle& mode : input.m_modeStack)
        {
            if (shouldClearMode(mode.m_mode))
                modalHandles.push_back(mode.m_handle);
        }

        for (int handle : modalHandles)
        {
            if (handle >= 0)
                input.DisableInputMode(handle);
        }

        auto& modeStack = input.m_modeStack;
        modeStack.erase(std::remove_if(modeStack.begin(), modeStack.end(), [&shouldClearMode](const ArkPlayerInput::ModeAndHandle& mode)
        {
            return shouldClearMode(mode.m_mode);
        }), modeStack.end());

        if (modeStack.empty())
            input.EnableInputMode(ArkPlayerInput::Mode::player);
        input.EnableActionMapForMode(ArkPlayerInput::Mode::player, true);
        input.EnablePlayerInputMode(true);
        input.ClearMovement();
        input.m_bSprint = false;
        input.m_bUseHeld = false;
        input.m_bTriggeredUse = false;
        input.m_bTriggeredHoldUse = false;
        input.m_bTriggeredSpecialUse = false;
        input.m_bZeroGBraking = false;
        input.m_bSprintInhibited = false;
        input.m_bJumpInhibited = false;
        input.m_bRotationInhibited = false;

        if (clearInheritedModalState)
        {
            // Loading a Host save can restore the single-player focus-loss
            // pause after the load guard already considered the world ready.
            // A connected client must resume without stealing desktop focus.
            if (gEnv && gEnv->pConsole)
                gEnv->pConsole->ExecuteString("g_pauseOnLoseFocus 0", true, false);
            if (gEnv && gEnv->pGame && gEnv->pGame->GetIGameFramework())
                gEnv->pGame->GetIGameFramework()->PauseGame(false, true, 0, false);
            if (gEnv && gEnv->pGame)
                gEnv->pGame->RequestPause(false);
        }

        ++m_focusModeCleanupCount;
        m_lastTimeScaleHandle = -1;
        m_lastTimeScaleValue = 1.0f;
        m_lastTimeScaleEvent = std::string("sidecar modal cleanup ") + (reason ? reason : "unknown");
    }
    catch (...)
    {
        m_networkStatus = std::string("sidecar modal cleanup threw during ") + (reason ? reason : "unknown");
        LogCoop(m_networkStatus);
    }
}

void ModMain::OnArkPlayerDeathScreenOpenSuppressed()
{
    ++m_suppressedDownedDeathScreens;
    SetLocalPlayerHealthSafe(m_downedEngineHealthFloor, "suppressed death screen");
    if (!m_localPlayerDowned)
        EnterLocalDowned(0, true, false);

    m_networkStatus = "suppressed death screen while downed";
}

bool ModMain::ShouldSuppressArkPlayerAction(const CCryName& action, int activationMode, float value)
{
    (void)value;

    if (m_joinOverlayActive && ShouldBlockJoinInput())
    {
        const std::string actionName = ToLowerAscii(action.c_str());
        ++m_suppressedDownedActions;
        m_lastSuppressedDownedAction = actionName.empty() ? std::string("join_block") : "join_" + actionName;
        return true;
    }

    if (!m_downedModeEnabled || !m_lockLocalInputWhileDowned || !m_localPlayerDowned)
        return false;

    if (m_networkMode == CoopNetworkMode::Off || !IsSessionGameplayReady())
        return false;

    const std::string actionName = ToLowerAscii(action.c_str());
    if (actionName.empty() || IsAllowedDownedAction(actionName))
        return false;

    if (IsDownedCancelAction(actionName, activationMode))
        return false;

    if (!IsBlockedDownedAction(actionName))
        return false;

    ++m_suppressedDownedActions;
    m_lastSuppressedDownedAction = actionName;
    return true;
}

void ModMain::SendRemoteReviveCommand()
{
    SetRemotePlayerDowned(false);
    m_remoteReviveSuppressDownedPoseSeconds = kPostReviveDownedGraceSeconds;
    m_remoteReviveHoldSeconds = 0.0f;
    m_remoteReviveHoldProgress = 0.0f;
    m_remoteRevivePromptActive = false;

    if (!m_hasRemoteEndpoint || !IsGameReady())
    {
        m_networkStatus = "remote revive skipped: no endpoint";
        return;
    }

    IEntity* proxyEntity = GetProxyEntity();
    IEntity* playerEntity = ArkPlayer::GetInstance().GetEntity();
    if (!playerEntity)
    {
        m_networkStatus = "remote revive skipped: no player entity";
        return;
    }

    const Vec3 position = proxyEntity ? proxyEntity->GetWorldPos() : playerEntity->GetWorldPos();
    const Quat rotation = proxyEntity ? proxyEntity->GetWorldRotation() : playerEntity->GetWorldRotation();
    CoopProtocol::PlayerStatusPacket packet = {};
    if (!BuildPlayerStatusPacket(
        packet,
        CoopProtocol::kPlayerStatusFlagRevived | CoopProtocol::kPlayerStatusFlagTargetLocalPlayer,
        0,
        position,
        rotation,
        m_reviveHealth,
        m_reviveHealth))
    {
        m_networkStatus = "remote revive build failed";
        return;
    }

    const bool reliableSent = SendPlayerStatusTo(packet, m_remoteAddress, m_remotePort, "remote revive reliable send failed");
    int directSent = 0;
    for (int i = 0; i < kRemoteReviveDirectBurstCount; ++i)
    {
        if (SendPacketTo(&packet, sizeof(packet), m_remoteAddress, m_remotePort, "remote revive direct send failed"))
            ++directSent;
    }

    if (reliableSent || directSent > 0)
    {
        m_networkStatus =
            "sent remote revive command seq " + std::to_string(packet.sequence) +
            " reliable " + std::to_string(reliableSent ? 1 : 0) +
            " direct " + std::to_string(directSent);
    }
    else
    {
        m_networkStatus = "remote revive send failed";
    }
}

bool ModMain::SetLocalPlayerHealthSafe(float health, const char* reason)
{
    if (m_localPlayerHealthWritesDisabled || !ArkPlayer::GetInstancePtr())
        return false;

    try
    {
        ArkPlayer& player = ArkPlayer::GetInstance();
        if (CoopPtrHygiene::Enabled())
        {
            char extra[64];
            std::snprintf(extra, sizeof(extra), "health=%.3f", health);
            CoopPtrHygiene::LogPtrWith("player_health_write", &player.m_playerComponent.GetHealthComponent(), extra);
        }
        const float currentHealth = player.GetHealth();
        if (std::isfinite(currentHealth) &&
            std::isfinite(health) &&
            std::fabs(currentHealth - health) <= kLocalPlayerHealthWriteEpsilon)
        {
            NormalizeHealthFeedbackAfterRestore(player.m_playerComponent.GetHealthComponent(), health);
            return true;
        }

        m_internalPlayerHealthWrite = true;
        ArkPlayerHealthComponent& healthComponent = player.m_playerComponent.GetHealthComponent();
        NormalizeHealthFeedbackAfterRestore(healthComponent, health);
        player.SetHealth(health);
        NormalizeHealthFeedbackAfterRestore(healthComponent, health);
        m_internalPlayerHealthWrite = false;
        return true;
    }
    catch (...)
    {
        m_internalPlayerHealthWrite = false;
        m_localPlayerHealthWritesDisabled = true;
        m_networkStatus = std::string("ArkPlayer::SetHealth threw during ") + (reason ? reason : "unknown");
        LogCoop(m_networkStatus);
        return false;
    }
}

bool ModMain::SetLocalPlayerStanceSafe(int stance, const char* reason)
{
    if (m_localPlayerStanceWritesDisabled || !ArkPlayer::GetInstancePtr())
        return false;

    try
    {
        ArkPlayer::GetInstance().SetStance(static_cast<EStance>(stance));
        return true;
    }
    catch (...)
    {
        m_localPlayerStanceWritesDisabled = true;
        m_networkStatus = std::string("ArkPlayer::SetStance threw during ") + (reason ? reason : "unknown");
        LogCoop(m_networkStatus);
        return false;
    }
}

void ModMain::ApplyLocalDownedStance(ArkPlayer& player)
{
    if (!m_forceLocalCrawlWhileDowned || m_localPlayerStanceWritesDisabled)
        return;

    try
    {
        player.m_movementFSM.SetRequestedStance(EStance::STANCE_SNEAK);
    }
    catch (...)
    {
        m_localPlayerStanceWritesDisabled = true;
        m_networkStatus = "downed stance request threw";
        LogCoop(m_networkStatus);
    }
}

void ModMain::ApplyLocalDownedControls(float frameTime)
{
    (void)frameTime;

    if (!m_localPlayerDowned || !ArkPlayer::GetInstancePtr())
    {
        ReleaseLocalDownedControls();
        return;
    }

    ArkPlayer& player = ArkPlayer::GetInstance();

    if (m_lockLocalInputWhileDowned)
    {
        player.m_input.m_bSprint = false;
        player.m_input.m_bUseHeld = false;
        player.m_input.m_bTriggeredUse = false;
        player.m_input.m_bTriggeredHoldUse = false;
        player.m_input.m_bTriggeredSpecialUse = false;
        player.m_input.m_bZeroGBraking = false;
        player.m_input.m_bSprint = false;
        player.m_input.m_bSprintInhibited = true;
        m_localDownedInputDisabled = true;

        if (!m_localDownedWeaponDisabled && !m_localDownedEquipmentWritesDisabled)
        {
            try
            {
                ArkPlayerWeaponComponent& weaponComponent = player.m_weaponComponent;
                weaponComponent.m_bCanEquip = false;
                weaponComponent.m_toBeEquippedWeaponId = 0;

                ArkPsiPowerComponent& psiPowerComponent = player.GetPsiPowerComponent();
                if (!m_localDownedSavedPsiPowers)
                {
                    m_localDownedSavedSelectedPower = static_cast<int>(psiPowerComponent.m_selectedPower);
                    m_localDownedSavedEquippedPower = static_cast<int>(psiPowerComponent.m_equippedPower);
                    m_localDownedSavedPsiPowers = true;
                }
                psiPowerComponent.m_bTargetedPowersDisabled = true;

                m_localDownedWeaponDisabled = true;
            }
            catch (...)
            {
                m_localDownedEquipmentWritesDisabled = true;
                m_networkStatus = "downed equipment writes threw";
                LogCoop(m_networkStatus);
            }
        }
    }

    ApplyLocalDownedStance(player);
}

void ModMain::ReleaseLocalDownedControls()
{
    if (!m_localDownedInputDisabled && !m_localDownedWeaponDisabled)
        return;

    if (!ArkPlayer::GetInstancePtr())
    {
        m_localDownedInputDisabled = false;
        m_localDownedWeaponDisabled = false;
        m_localDownedSavedPsiPowers = false;
        return;
    }

    try
    {
        ArkPlayer& player = ArkPlayer::GetInstance();
        player.m_input.m_bSprintInhibited = false;
        player.m_input.m_bJumpInhibited = false;
        player.m_weaponComponent.m_bCanEquip = true;
        ArkPsiPowerComponent& psiPowerComponent = player.GetPsiPowerComponent();
        psiPowerComponent.m_bTargetedPowersDisabled = false;
        if (m_localDownedSavedPsiPowers)
        {
            psiPowerComponent.m_selectedPower = static_cast<EArkPsiPowers>(m_localDownedSavedSelectedPower);
            psiPowerComponent.m_equippedPower = static_cast<EArkPsiPowers>(m_localDownedSavedEquippedPower);
        }
    }
    catch (...)
    {
        m_networkStatus = "downed input release threw";
        LogCoop(m_networkStatus);
    }

    m_localDownedInputDisabled = false;
    m_localDownedWeaponDisabled = false;
    m_localDownedSavedPsiPowers = false;
}

void ModMain::ApplyLocalPlayerDownedAttentionState(ArkPlayer& player)
{
    if (!m_disableLocalPlayerAttentionWhileDowned)
    {
        ReleaseLocalPlayerDownedAttentionState();
        return;
    }

    if (m_localDownedAttentionObjectDisabled)
        return;

    try
    {
        player.LimitAttentionOnUnseenPlayer();
        player.m_attentionObject.DisableRoomPerceivable();
        player.m_attentionObject.DisableAuralPerceivable();
        player.m_attentionObject.DisableVisualPerceivable();
        player.m_attentionObject.DisableAttentionObject();
        m_localDownedAttentionObjectDisabled = true;
        ++m_localAttentionObjectDisableCalls;
        m_lastLocalAttentionClearStage = "player attention object disabled";
    }
    catch (...)
    {
        ++m_localAttentionObjectErrors;
        m_networkStatus = "downed player attention disable threw";
        LogCoop(m_networkStatus);
    }
}

void ModMain::ReleaseLocalPlayerDownedAttentionState()
{
    if (!m_localDownedAttentionObjectDisabled)
        return;

    if (!ArkPlayer::GetInstancePtr())
    {
        m_localDownedAttentionObjectDisabled = false;
        return;
    }

    try
    {
        ArkPlayer& player = ArkPlayer::GetInstance();
        player.ReleaseAttentionLimitOnPlayer();
        player.m_attentionObject.EnableAttentionObject();
        player.m_attentionObject.EnableVisualPerceivable();
        player.m_attentionObject.EnableAuralPerceivable();
        player.m_attentionObject.EnableRoomPerceivable();
        m_localDownedAttentionObjectDisabled = false;
        ++m_localAttentionObjectReleaseCalls;
        m_lastLocalAttentionClearStage = "player attention object released";
    }
    catch (...)
    {
        ++m_localAttentionObjectErrors;
        m_localDownedAttentionObjectDisabled = false;
        m_networkStatus = "downed player attention release threw";
        LogCoop(m_networkStatus);
    }
}

void ModMain::RecoverLocalPlayerFromNativeDeathState(const char* reason)
{
    if (!ArkPlayer::GetInstancePtr())
        return;

    try
    {
        ArkPlayer& player = ArkPlayer::GetInstance();
        const bool wasDeathState =
            player.m_movementFSM.m_currentStateId == EArkPlayerMovementStateId::death ||
            player.m_movementFSM.m_currentStateId == EArkPlayerMovementStateId::deathByRecyclerGrenade;

        player.Revive();
        player.PhysicalizeAndResetAnimatedCharacter();
        player.m_movementFSM.RestrictMovement(false);
        player.m_movementFSM.m_bMovementRestricted = false;
        player.m_movementFSM.m_bJumpRequested = false;
        player.m_movementFSM.m_bInputJumpPressed = false;

        if (wasDeathState ||
            player.m_movementFSM.m_currentStateId == EArkPlayerMovementStateId::death ||
            player.m_movementFSM.m_currentStateId == EArkPlayerMovementStateId::deathByRecyclerGrenade)
        {
            player.m_movementFSM.m_currentStateId = EArkPlayerMovementStateId::ground;
        }

        ++m_nativeDeathRecoveries;
        m_lastLocalAttentionClearStage = reason ? reason : "native death recovery";
    }
    catch (...)
    {
        m_networkStatus = std::string("native death recovery threw during ") + (reason ? reason : "unknown");
        LogCoop(m_networkStatus);
    }
}

void ModMain::EnterLocalDowned(uint32_t reason, bool sendStatus, bool clampHealth)
{
    if (!m_downedModeEnabled || !ArkPlayer::GetInstancePtr())
        return;

    const bool wasAlreadyDowned = m_localPlayerDowned;
    m_localPlayerDowned = true;
    m_localDownedAttentionClearAccumulator = 0.0f;
    m_localDownedFocusCleanupAccumulator = 0.0f;
    if (clampHealth)
        SetLocalPlayerHealthSafe(m_downedEngineHealthFloor, "enter local downed");

    if (!wasAlreadyDowned)
        StopLocalFocusModeForDowned("enter");

    ApplyLocalPlayerDownedAttentionState(ArkPlayer::GetInstance());
    ApplyLocalDownedControls(0.0f);

    if (m_remotePlayerDowned)
        m_teamWipe = true;

    if (sendStatus)
    {
        uint32_t flags = CoopProtocol::kPlayerStatusFlagDowned;
        if (m_teamWipe)
            flags |= CoopProtocol::kPlayerStatusFlagTeamWipe;
        SendLocalPlayerStatus(flags, reason);
    }
}

void ModMain::ReviveLocalPlayer(float health, bool sendStatus)
{
    if (!ArkPlayer::GetInstancePtr())
        return;

    if (CoopPtrHygiene::Enabled())
    {
        char extra[64];
        std::snprintf(extra, sizeof(extra), "health=%.3f", health);
        CoopPtrHygiene::LogPtrWith("player_revive", &ArkPlayer::GetInstance().m_playerComponent.GetHealthComponent(), extra);
    }
    m_localPlayerDowned = false;
    m_teamWipe = false;
    m_localReviveSuppressDownedStatusSeconds = kPostReviveDownedGraceSeconds;
    m_localDownedAttentionClearAccumulator = 0.0f;
    m_localDownedFocusCleanupAccumulator = 0.0f;
    StopLocalFocusModeForDowned("revive");
    RecoverLocalPlayerFromNativeDeathState("revive");
    ReleaseLocalPlayerDownedAttentionState();
    ReleaseLocalDownedControls();
    ClearDownedHudOverlay();
    SetLocalPlayerHealthSafe(std::max(health, m_reviveHealth), "revive local");

    if (sendStatus)
        SendLocalPlayerStatus(CoopProtocol::kPlayerStatusFlagRevived, 0);
}

void ModMain::SetRemotePlayerDowned(bool downed)
{
    if (downed && m_remoteReviveSuppressDownedPoseSeconds > 0.0f)
        return;

    if (m_remotePlayerDowned == downed)
        return;

    m_remotePlayerDowned = downed;
    if (downed)
        m_proxyStandStanceApplied = false;
    if (m_localPlayerDowned && downed)
        m_teamWipe = true;

    if (IEntity* proxyEntity = GetProxyEntity())
    {
        RegisterProxyReviveInteraction(*proxyEntity);
        if (downed)
        {
            ApplyProxyDownedState(*proxyEntity);
            std::string detail;
            StartRemoteProxyPoseHold(
                "downed_pose",
                "combat_forceresist_front_out_empty",
                0.25f,
                0,
                0,
                0.01f,
                detail);
        }
        else
        {
            ClearRemoteProxyPoseHold("remote_player_revived");
            ApplySurvivorFactionToProxy(*proxyEntity);
            RestoreProxyRuntimeHealth();
            std::string detail;
            ApplyProxyCharacterAnimationState(*proxyEntity, "normal", 1.0f, detail);
        }
        ApplyRemoteProxyDownedVisual(*proxyEntity, downed);
    }
}

void ModMain::TickRemoteReviveInteraction(float frameTime)
{
    const float delta = std::max(0.0f, frameTime);
    if (m_remoteReviveSuppressDownedPoseSeconds > 0.0f)
        m_remoteReviveSuppressDownedPoseSeconds = std::max(0.0f, m_remoteReviveSuppressDownedPoseSeconds - delta);
    if (m_localReviveSuppressDownedStatusSeconds > 0.0f)
        m_localReviveSuppressDownedStatusSeconds = std::max(0.0f, m_localReviveSuppressDownedStatusSeconds - delta);

    m_remoteRevivePromptActive = false;
    m_remoteReviveDistance = 0.0f;
    m_remoteReviveNativeCompleteThisFrame = false;

    if (!m_downedModeEnabled ||
        !m_remotePlayerDowned ||
        m_localPlayerDowned ||
        !IsSessionGameplayReady() ||
        !IsGameReady())
    {
        m_remoteReviveHoldSeconds = 0.0f;
        m_remoteReviveHoldProgress = 0.0f;
        return;
    }

    IEntity* proxyEntity = GetProxyEntity();
    IEntity* playerEntity = ArkPlayer::GetInstance().GetEntity();
    if (!proxyEntity || !playerEntity)
    {
        m_remoteReviveHoldSeconds = 0.0f;
        m_remoteReviveHoldProgress = 0.0f;
        return;
    }

    const Vec3 toProxy = proxyEntity->GetWorldPos() - playerEntity->GetWorldPos();
    const float distanceSquared = toProxy.GetLengthSquared();
    m_remoteReviveDistance = std::sqrt(std::max(0.0f, distanceSquared));
    const bool inRange = distanceSquared <= kRemoteReviveDistanceMeters * kRemoteReviveDistanceMeters;

    Vec3 flatToProxy = toProxy;
    flatToProxy.z = 0.0f;
    Vec3 flatForward = playerEntity->GetWorldRotation() * Vec3(0.0f, 1.0f, 0.0f);
    flatForward.z = 0.0f;

    bool facingProxy = true;
    const float flatToProxyLenSq = flatToProxy.GetLengthSquared();
    const float flatForwardLenSq = flatForward.GetLengthSquared();
    if (flatToProxyLenSq > 0.0001f && flatForwardLenSq > 0.0001f)
    {
        const float denom = std::sqrt(flatToProxyLenSq * flatForwardLenSq);
        facingProxy = denom > 0.0001f && (flatForward.Dot(flatToProxy) / denom) >= kRemoteReviveFacingDot;
    }

    m_remoteRevivePromptActive = inRange && facingProxy;
    if (!m_remoteRevivePromptActive)
    {
        m_remoteReviveHoldSeconds = 0.0f;
        m_remoteReviveHoldProgress = 0.0f;
        return;
    }

    ArkPlayer& player = ArkPlayer::GetInstance();
    if (m_useNativeReviveInteraction && m_proxyReviveInteractionRegistered)
        RefreshNativeRemoteRevivePrompt(player, kRemoteReviveHoldSeconds);

    const bool useHeld = player.m_input.m_bUseHeld || player.m_input.m_bTriggeredUse || player.m_input.m_bTriggeredHoldUse;
    if (!useHeld)
    {
        m_remoteReviveHoldSeconds = 0.0f;
        m_remoteReviveHoldProgress = 0.0f;
        return;
    }

    m_remoteReviveHoldSeconds += std::max(0.0f, frameTime);
    m_remoteReviveHoldProgress = std::min(1.0f, m_remoteReviveHoldSeconds / kRemoteReviveHoldSeconds);
    player.m_input.m_bTriggeredUse = false;
    player.m_input.m_bTriggeredHoldUse = false;
    player.m_input.m_bTriggeredSpecialUse = false;

    if (m_remoteReviveHoldProgress >= 1.0f)
    {
        m_remoteReviveHoldSeconds = 0.0f;
        m_remoteReviveHoldProgress = 0.0f;
        SendRemoteReviveCommand();
        m_networkStatus = "hold F revived remote player";
    }
}

void ModMain::ClearLocalPlayerAsEnemyTarget()
{
    if (m_networkMode != CoopNetworkMode::Host || !ArkPlayer::GetInstancePtr() || !gEnv || !gEnv->pGame || !gEnv->pEntitySystem)
        return;

    IEntity* playerEntity = ArkPlayer::GetInstance().GetEntity();
    if (!playerEntity)
        return;

    if (!IsReadableRuntimePointer(playerEntity))
    {
        ++m_localAttentionClearPointerSkips;
        m_lastLocalAttentionClearStage = "skip bad player entity pointer";
        m_lastLocalAttentionClearEnemy = INVALID_ENTITYID;
        return;
    }

    std::string unsafeReason;
    if (!IsLocalAttentionClearSafeGameplay(unsafeReason))
    {
        ++m_localAttentionClearUnsafeSkips;
        m_lastLocalAttentionClearStage = "skip " + unsafeReason;
        m_lastLocalAttentionClearEnemy = INVALID_ENTITYID;
        return;
    }

    const EntityId playerEntityId = playerEntity->GetId();
    CGame* game = static_cast<CGame*>(gEnv->pGame);
    ArkAttentionManager* attentionManager = game && game->m_pArkAttentionManager ? game->m_pArkAttentionManager.get() : nullptr;
    if (attentionManager && !IsReadableRuntimePointer(attentionManager))
    {
        ++m_localAttentionClearPointerSkips;
        m_lastLocalAttentionClearStage = "skip bad attention manager pointer";
        attentionManager = nullptr;
    }

    ++m_localAttentionClearAttempts;
    m_lastLocalAttentionClearStage = "begin";
    m_lastLocalAttentionClearEnemy = INVALID_ENTITYID;

    const uint32_t totalEnemies = static_cast<uint32_t>(m_enemyAuthorities.size());
    if (totalEnemies == 0)
    {
        m_localDownedAttentionScanCursor = 0;
        m_lastLocalAttentionClearStage = "no enemy authorities";
        return;
    }

    if (m_localDownedAttentionScanCursor >= totalEnemies)
        m_localDownedAttentionScanCursor = 0;

    const uint32_t maxEnemiesThisTick = static_cast<uint32_t>(std::max(1, m_localDownedAttentionMaxEnemiesPerTick));
    uint32_t index = 0;
    uint32_t touchedThisTick = 0;
    uint32_t nextCursor = 0;

    auto setStage = [&](const char* stage, EntityId enemyId)
    {
        m_lastLocalAttentionClearStage = stage ? stage : "-";
        m_lastLocalAttentionClearEnemy = enemyId;
        if (m_verboseLocalDownedAttentionLog)
        {
            LogCoop(std::string("local player attention clear stage=") +
                m_lastLocalAttentionClearStage +
                " enemy=" + std::to_string(enemyId) +
                " mode=" + std::to_string(m_localDownedAttentionClearMode));
        }
    };

    for (const auto& entry : m_enemyAuthorities)
    {
        if (index++ < m_localDownedAttentionScanCursor)
            continue;

        nextCursor = index;
        const EnemyAuthorityState& state = entry.second;
        if (state.entityId == INVALID_ENTITYID || state.entityId == playerEntityId)
            continue;

        IEntity* enemyEntity = gEnv->pEntitySystem->GetEntity(state.entityId);
        if (!IsReadableRuntimePointer(enemyEntity))
        {
            ++m_localAttentionClearPointerSkips;
            setStage("skip bad enemy entity pointer", state.entityId);
            continue;
        }

        ArkNpc* enemyNpc = enemyEntity ? EntityUtils::GetArkNpc(enemyEntity) : nullptr;
        if (!IsReadableRuntimePointer(enemyNpc))
        {
            ++m_localAttentionClearPointerSkips;
            setStage("skip bad enemy npc pointer", state.entityId);
            continue;
        }

        if (!enemyNpc || enemyNpc->IsDead())
            continue;

        ++touchedThisTick;
        ++m_localAttentionClearEnemiesTouched;
        setStage("inspect", state.entityId);

        bool hasSimple = false;
        bool hasComplex = false;
        bool isTopTarget = false;

        if (m_localDownedAttentionClearMode == kLocalAttentionObserveOnly ||
            m_localDownedAttentionClearMode == kLocalAttentionManagerSimple ||
            m_localDownedAttentionClearMode == kLocalAttentionManagerSimpleComplex ||
            m_localDownedAttentionClearMode == kLocalAttentionFullLegacy)
        {
            if (attentionManager)
            {
                setStage("probe simple", state.entityId);
                hasSimple = attentionManager->HasSimpleAttention(state.entityId, playerEntityId);
                if (hasSimple)
                    ++m_localAttentionClearSimpleHits;

                setStage("probe complex", state.entityId);
                hasComplex = attentionManager->IsSubjectTrackingComplexObject(state.entityId, playerEntityId);
                if (hasComplex)
                    ++m_localAttentionClearComplexHits;
            }

            setStage("probe top target", state.entityId);
            isTopTarget = ArkNpc::FGetTopAttentionTargetEntityId(enemyNpc) == playerEntityId;
            if (isTopTarget)
                ++m_localAttentionClearTopTargetHits;
        }

        if (m_localDownedAttentionClearMode == kLocalAttentionNpcClearOnPlayer)
        {
            setStage("npc ClearAttentionOnPlayer", state.entityId);
            enemyNpc->ClearAttentionOnPlayer();
            ++m_localAttentionClearNpcClearPlayerCalls;
        }
        else if (m_localDownedAttentionClearMode == kLocalAttentionManagerSimple)
        {
            if (attentionManager && hasSimple)
            {
                setStage("manager ClearSimpleAttention", state.entityId);
                attentionManager->ClearSimpleAttention(state.entityId, playerEntityId);
                ++m_localAttentionClearManagerSimpleCalls;
            }
        }
        else if (m_localDownedAttentionClearMode == kLocalAttentionManagerSimpleComplex)
        {
            if (attentionManager && hasSimple)
            {
                setStage("manager ClearSimpleAttention", state.entityId);
                attentionManager->ClearSimpleAttention(state.entityId, playerEntityId);
                ++m_localAttentionClearManagerSimpleCalls;
            }

            if (attentionManager && hasComplex)
            {
                setStage("manager ClearComplexAttention", state.entityId);
                attentionManager->ClearComplexAttention(state.entityId, playerEntityId);
                ++m_localAttentionClearManagerComplexCalls;
            }
        }
        else if (m_localDownedAttentionClearMode == kLocalAttentionFullLegacy)
        {
            if (attentionManager && hasSimple)
            {
                setStage("legacy ClearSimpleAttention", state.entityId);
                attentionManager->ClearSimpleAttention(state.entityId, playerEntityId);
                ++m_localAttentionClearManagerSimpleCalls;
            }

            if (attentionManager && hasComplex)
            {
                setStage("legacy ClearComplexAttention", state.entityId);
                attentionManager->ClearComplexAttention(state.entityId, playerEntityId);
                ++m_localAttentionClearManagerComplexCalls;
            }

            if (isTopTarget)
            {
                setStage("legacy OnLostAttentionTarget", state.entityId);
                enemyNpc->OnLostAttentionTarget(playerEntityId, false);
                setStage("legacy ClearAllAttention", state.entityId);
                enemyNpc->ClearAllAttention();
                setStage("legacy OnCombatEnd", state.entityId);
                enemyNpc->OnCombatEnd();
                ++m_localAttentionClearLegacyNpcCalls;
            }
        }

        if (touchedThisTick >= maxEnemiesThisTick)
            break;
    }

    m_localDownedAttentionScanCursor = nextCursor >= totalEnemies ? 0 : nextCursor;

    if (m_rebindProxyAfterLocalDownedAttentionClear && touchedThisTick > 0 && !m_remotePlayerDowned && GetProxyEntity())
    {
        setStage("rebind proxy target", INVALID_ENTITYID);
        BindHostEnemiesToProxyTarget();
        ++m_localAttentionClearProxyRebinds;
    }

    m_lastLocalAttentionClearStage = "done";
}

bool ModMain::SendLocalPlayerStatus(uint32_t flags, uint32_t reason, const Vec3* overridePosition, const Quat* overrideRotation)
{
    if (m_socket == kInvalidNetworkSocket || !IsGameReady())
        return false;

    IEntity* playerEntity = ArkPlayer::GetInstance().GetEntity();
    if (!playerEntity)
        return false;

    const Vec3 position = overridePosition ? *overridePosition : playerEntity->GetWorldPos();
    const Quat rotation = overrideRotation ? *overrideRotation : playerEntity->GetWorldRotation();
    const float health = ArkPlayer::GetInstance().GetHealth();
    const float maxHealth = ArkPlayer::GetInstance().GetMaxHealth();

    CoopProtocol::PlayerStatusPacket packet = {};
    if (!BuildPlayerStatusPacket(packet, flags, reason, position, rotation, health, maxHealth))
        return false;

    if (m_hasRemoteEndpoint)
        return SendPlayerStatusTo(packet, m_remoteAddress, m_remotePort, "player status send failed");

    if (m_networkMode == CoopNetworkMode::Client)
    {
        sockaddr_in targetAddress = {};
        targetAddress.sin_family = AF_INET;
        targetAddress.sin_port = htons(static_cast<u_short>(m_networkPort));
        if (inet_pton(AF_INET, m_hostAddress.c_str(), &targetAddress.sin_addr) != 1)
            return false;
        return SendPlayerStatusTo(packet, targetAddress.sin_addr.s_addr, targetAddress.sin_port, "player status send failed");
    }

    return false;
}

bool ModMain::TeleportLocalPlayer(const Vec3& position, const Quat& rotation)
{
    if (!IsGameReady())
        return false;

    IEntity* playerEntity = ArkPlayer::GetInstance().GetEntity();
    if (!playerEntity)
        return false;

    playerEntity->SetPosRotScale(position, rotation, playerEntity->GetScale(), 0);
    m_lastLocalPlayerPos = position;
    m_hasLastLocalPlayerPos = true;
    return true;
}

bool ModMain::TeleportLocalPlayerNearRemote(uint32_t reason)
{
    if (!IsGameReady())
        return false;

    IEntity* proxyEntity = GetProxyEntity();
    IEntity* playerEntity = ArkPlayer::GetInstance().GetEntity();
    if (!proxyEntity || !playerEntity)
        return false;

    const Quat rotation = proxyEntity->GetWorldRotation();
    const Vec3 position = proxyEntity->GetWorldPos() + rotation * Vec3(kUnstuckTeleportOffsetMeters, 0.0f, 0.0f);
    if (!TeleportLocalPlayer(position, rotation))
        return false;

    SendLocalPlayerStatus(CoopProtocol::kPlayerStatusFlagTeleport, reason, &position, &rotation);
    return true;
}

bool ModMain::TeleportRemoteProxyNearLocal(uint32_t reason)
{
    if (m_networkMode != CoopNetworkMode::Host || !IsGameReady())
        return false;

    IEntity* proxyEntity = GetProxyEntity();
    IEntity* playerEntity = ArkPlayer::GetInstance().GetEntity();
    if (!proxyEntity || !playerEntity)
        return false;

    const Vec3 delta = proxyEntity->GetWorldPos() - playerEntity->GetWorldPos();
    if (delta.GetLengthSquared() <= kRemoteDownedTeleportDistanceMeters * kRemoteDownedTeleportDistanceMeters)
        return false;

    const Quat rotation = playerEntity->GetWorldRotation();
    const Vec3 position = playerEntity->GetWorldPos() + rotation * Vec3(kRemoteDownedTeleportOffsetMeters, 0.0f, 0.0f);
    ApplyRemoteProxyTransform(
        *proxyEntity,
        position,
        rotation,
        "downed remote proxy teleport");
    ResetProxyHealthBaseline();
    ApplyProxyDownedState(*proxyEntity);

    CoopProtocol::PlayerStatusPacket packet = {};
    if (!BuildPlayerStatusPacket(
        packet,
        CoopProtocol::kPlayerStatusFlagDowned |
            CoopProtocol::kPlayerStatusFlagTeleport |
            CoopProtocol::kPlayerStatusFlagUnreachableRecovery |
            CoopProtocol::kPlayerStatusFlagTargetLocalPlayer,
        reason,
        position,
        rotation,
        m_downedEngineHealthFloor,
        m_downedEngineHealthFloor))
    {
        return false;
    }

    if (m_hasRemoteEndpoint && SendPlayerStatusTo(packet, m_remoteAddress, m_remotePort, "remote downed teleport send failed"))
    {
        m_networkStatus = "teleported downed remote near host";
        return true;
    }

    return false;
}

void ModMain::TickDownedState(float frameTime)
{
    if (!m_downedModeEnabled)
    {
        ReleaseLocalPlayerDownedAttentionState();
        ReleaseLocalDownedControls();
        return;
    }

    if (m_localPlayerDowned && ArkPlayer::GetInstancePtr())
    {
        ArkPlayer& player = ArkPlayer::GetInstance();
        ApplyLocalPlayerDownedAttentionState(player);
        if (player.m_movementFSM.m_currentStateId == EArkPlayerMovementStateId::death ||
            player.m_movementFSM.m_currentStateId == EArkPlayerMovementStateId::deathByRecyclerGrenade)
        {
            RecoverLocalPlayerFromNativeDeathState("downed tick death state");
        }

        ApplyLocalDownedControls(frameTime);
        if (player.GetHealth() < m_downedEngineHealthFloor)
            SetLocalPlayerHealthSafe(m_downedEngineHealthFloor, "downed tick clamp");

        if (m_blockNativeTimeScaleWhileDowned)
        {
            m_localDownedFocusCleanupAccumulator += frameTime;
            if (m_localDownedFocusCleanupAccumulator >= kLocalDownedFocusCleanupSeconds)
            {
                m_localDownedFocusCleanupAccumulator = 0.0f;
                StopLocalFocusModeForDowned("tick");
            }
        }
        else
        {
            m_localDownedFocusCleanupAccumulator = 0.0f;
        }

        if (m_clearLocalPlayerAttentionWhileDowned && !m_disableLocalPlayerAttentionWhileDowned)
        {
            m_localDownedAttentionClearAccumulator += frameTime;
            if (m_localDownedAttentionClearAccumulator >= kLocalDownedAttentionClearSeconds)
            {
                m_localDownedAttentionClearAccumulator = 0.0f;
                ClearLocalPlayerAsEnemyTarget();
            }
        }
        else
        {
            m_localDownedAttentionClearAccumulator = 0.0f;
        }
    }
    else
    {
        m_localDownedAttentionClearAccumulator = 0.0f;
        m_localDownedFocusCleanupAccumulator = 0.0f;
        ReleaseLocalPlayerDownedAttentionState();
        ReleaseLocalDownedControls();
    }

    if (m_remotePlayerDowned)
    {
        if (IEntity* proxyEntity = GetProxyEntity())
        {
            ApplyRemoteProxyDownedVisual(*proxyEntity, true);
            if (!m_remoteProxyPoseHoldActive ||
                m_remoteProxyPoseHoldEntityId != proxyEntity->GetId() ||
                m_remoteProxyPoseHoldName != "downed_pose")
            {
                std::string detail;
                StartRemoteProxyPoseHold(
                    "downed_pose",
                    "combat_forceresist_front_out_empty",
                    0.25f,
                    0,
                    0,
                    0.01f,
                    detail);
                m_proxyStandStanceApplied = false;
            }
        }
    }

    if (m_networkMode == CoopNetworkMode::Host && m_remotePlayerDowned && m_autoTeleportRemoteDownedToHost)
    {
        m_remoteDownedRecoveryAccumulator += frameTime;
        if (m_remoteDownedRecoveryAccumulator >= kRemoteDownedRecoveryTickSeconds)
        {
            m_remoteDownedRecoveryAccumulator = 0.0f;
            TeleportRemoteProxyNearLocal(CoopProtocol::kPlayerStatusFlagUnreachableRecovery);
        }
    }
}
