#include "ModMain.h"
#include "CoopRuntimeGuards.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <Prey/CryAction/actiongame.h>
#include <Prey/CryAction/IGameObject.h>
#include <Prey/CryEntitySystem/Entity.h>
#include <Prey/CryGame/IGame.h>
#include <Prey/CryGame/IGameFramework.h>
#include <Prey/CryMath/CryHalf.inl>
#include <Prey/CryPhysics/IPhysics.h>
#include <Prey/CrySystem/ISystem.h>
#include <Prey/GameDll/ark/arkbreakable.h>
#include <Prey/GameDll/ark/player/ArkPlayer.h>

namespace
{
constexpr float kBreakableHealthEpsilon = 0.0005f;
constexpr float kMaxHalfFloat = 65504.0f;
constexpr uint64_t kDebugBreakableArchetype = 221ull;
constexpr EntityGUID kDebugBreakableGuid = 0x434f4f5042524b31ull; // "COOPBRK1"
constexpr uint64_t kDebugScalableBreakableArchetype = 10739735956144680886ull;
constexpr EntityGUID kDebugScalableBreakableGuid = 0x434f4f5053424b31ull; // "COOPSBK1"

auto s_hookCArkBreakableSetHealth = CArkBreakable::FSetHealth.MakeHook();
auto s_hookCEntitySetBreakableGlass = CEntity::FSetBreakableGlass.MakeHook();
auto s_hookCActionGameOnCollisionLoggedBreakable =
    CActionGame::FOnCollisionLogged_Breakable.MakeHook();

struct RegisteredBreakableGlass
{
    EntityGUID guid = 0;
    uint64_t stableId = 0;
    std::unordered_set<int> slots;
};

std::unordered_map<EntityId, RegisteredBreakableGlass> s_registeredBreakableGlass;
std::unordered_map<EntityId, uint64_t> s_nonBreakableGlassEntities;

struct BreakableGlassCollisionCapture
{
    bool active = false;
    EntityId entityId = INVALID_ENTITYID;
    uint64_t stableId = 0;
    int slot = -1;
};

thread_local BreakableGlassCollisionCapture s_breakableGlassCollisionCapture;

uint64_t BuildBreakableGlassStableId(const IEntity& entity)
{
    const EntityGUID guid = entity.GetGuid();
    if (guid != 0)
        return static_cast<uint64_t>(guid);

    uint64_t hash = 14695981039346656037ull;
    auto mixByte = [&hash](uint8_t value)
    {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    auto mix64 = [&mixByte](uint64_t value)
    {
        for (int index = 0; index < 8; ++index)
            mixByte(static_cast<uint8_t>((value >> (index * 8)) & 0xffu));
    };
    auto mixText = [&mixByte](const char* value)
    {
        if (!value)
            return;
        while (*value)
            mixByte(static_cast<uint8_t>(*value++));
    };

    mix64(0x474c415353535442ull); // "GLASSSTB"
    mixText(entity.GetName());
    if (const IEntityClass* entityClass = entity.GetClass())
        mixText(entityClass->GetName());
    const Vec3 position = entity.GetWorldPos();
    mix64(static_cast<uint64_t>(static_cast<int64_t>(std::llround(position.x * 100.0f))));
    mix64(static_cast<uint64_t>(static_cast<int64_t>(std::llround(position.y * 100.0f))));
    mix64(static_cast<uint64_t>(static_cast<int64_t>(std::llround(position.z * 100.0f))));
    return hash == 0 ? 1 : hash;
}

bool RefreshBreakableGlassRegistration(IEntity& entity, bool force = false)
{
    const EntityId entityId = entity.GetId();
    if (entityId == INVALID_ENTITYID)
        return false;

    const uint64_t stableId = BuildBreakableGlassStableId(entity);
    if (const auto registered = s_registeredBreakableGlass.find(entityId);
        registered != s_registeredBreakableGlass.end() &&
        registered->second.stableId == stableId &&
        !registered->second.slots.empty())
    {
        return true;
    }
    if (const auto rejected = s_nonBreakableGlassEntities.find(entityId);
        rejected != s_nonBreakableGlassEntities.end() &&
        rejected->second == stableId && !force)
    {
        return false;
    }

    int slotCount = 0;
    std::string reason;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "breakable glass GetSlotCount",
            [&entity]() { return entity.GetSlotCount(); },
            slotCount,
            &reason) ||
        slotCount <= 0)
    {
        return false;
    }

    std::unordered_set<int> slots;
    const int boundedSlotCount = std::min(slotCount, 256);
    for (int slot = 0; slot < boundedSlotCount; ++slot)
    {
        IArkGlass* glass = nullptr;
        if (CoopRuntimeGuards::TryGuardedCall(
                "breakable glass GetBreakableGlass",
                [&entity, slot]()
                {
                    return CEntity::FGetBreakableGlass(
                        static_cast<CEntity*>(&entity), slot);
                },
                glass,
                &reason) &&
            glass)
        {
            slots.insert(slot);
        }
    }

    if (slots.empty())
    {
        s_nonBreakableGlassEntities[entityId] = stableId;
        return false;
    }

    RegisteredBreakableGlass& registered = s_registeredBreakableGlass[entityId];
    registered.guid = entity.GetGuid();
    registered.stableId = stableId;
    registered.slots.insert(slots.begin(), slots.end());
    s_nonBreakableGlassEntities.erase(entityId);
    return registered.stableId != 0;
}

void RefreshAllBreakableGlassRegistrations()
{
    if (!gEnv || !gEnv->pEntitySystem)
        return;

    IEntityIt* rawIterator = gEnv->pEntitySystem->GetEntityIterator();
    if (!rawIterator)
        return;

    IEntityItPtr iterator = rawIterator;
    iterator->MoveFirst();
    while (!iterator->IsEnd())
    {
        IEntity* entity = iterator->Next();
        if (!entity)
            break;
        RefreshBreakableGlassRegistration(*entity, true);
    }
}

#pragma pack(push, 1)
struct BreakableGlassImpactWire
{
    float point[3] = {};
    float normal[3] = {};
    uint16_t localVelocity[2][3] = {};
    uint16_t mass[2] = {};
    uint16_t penetration = 0;
    uint16_t normalImpulse = 0;
    uint16_t radius = 0;
    int16_t partId[2] = {};
    int16_t materialId[2] = {};
    int16_t primitiveId[2] = {};
};
#pragma pack(pop)

static_assert(sizeof(BreakableGlassImpactWire) == 58);

uint16_t EncodeGlassHalf(float value)
{
    if (!std::isfinite(value))
        value = 0.0f;
    return static_cast<uint16_t>(CryConvertFloatToHalf(
        std::clamp(value, -kMaxHalfFloat, kMaxHalfFloat)));
}

float DecodeGlassHalf(uint16_t value)
{
    return CryConvertHalfToFloat(static_cast<CryHalf>(value));
}

std::string EncodeGlassPayload(const BreakableGlassImpactWire& payload)
{
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* bytes = reinterpret_cast<const uint8_t*>(&payload);
    std::string encoded;
    encoded.reserve(((sizeof(payload) + 2) / 3) * 4);
    for (size_t i = 0; i < sizeof(payload); i += 3)
    {
        const uint32_t value =
            static_cast<uint32_t>(bytes[i]) << 16 |
            (i + 1 < sizeof(payload) ? static_cast<uint32_t>(bytes[i + 1]) << 8 : 0) |
            (i + 2 < sizeof(payload) ? static_cast<uint32_t>(bytes[i + 2]) : 0);
        encoded.push_back(kAlphabet[(value >> 18) & 0x3f]);
        encoded.push_back(kAlphabet[(value >> 12) & 0x3f]);
        encoded.push_back(i + 1 < sizeof(payload) ? kAlphabet[(value >> 6) & 0x3f] : '=');
        encoded.push_back(i + 2 < sizeof(payload) ? kAlphabet[value & 0x3f] : '=');
    }
    return encoded;
}

int DecodeGlassBase64Character(char value)
{
    if (value >= 'A' && value <= 'Z')
        return value - 'A';
    if (value >= 'a' && value <= 'z')
        return value - 'a' + 26;
    if (value >= '0' && value <= '9')
        return value - '0' + 52;
    if (value == '+')
        return 62;
    if (value == '/')
        return 63;
    return -1;
}

bool DecodeGlassPayload(std::string_view encoded, BreakableGlassImpactWire& payload)
{
    constexpr size_t kEncodedSize = ((sizeof(payload) + 2) / 3) * 4;
    if (encoded.size() != kEncodedSize)
        return false;

    std::array<uint8_t, sizeof(payload)> bytes = {};
    size_t output = 0;
    for (size_t i = 0; i < encoded.size(); i += 4)
    {
        const int a = DecodeGlassBase64Character(encoded[i]);
        const int b = DecodeGlassBase64Character(encoded[i + 1]);
        const int c = encoded[i + 2] == '=' ? 0 : DecodeGlassBase64Character(encoded[i + 2]);
        const int d = encoded[i + 3] == '=' ? 0 : DecodeGlassBase64Character(encoded[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0)
            return false;
        const uint32_t value =
            static_cast<uint32_t>(a) << 18 |
            static_cast<uint32_t>(b) << 12 |
            static_cast<uint32_t>(c) << 6 |
            static_cast<uint32_t>(d);
        if (output < bytes.size())
            bytes[output++] = static_cast<uint8_t>((value >> 16) & 0xff);
        if (output < bytes.size() && encoded[i + 2] != '=')
            bytes[output++] = static_cast<uint8_t>((value >> 8) & 0xff);
        if (output < bytes.size() && encoded[i + 3] != '=')
            bytes[output++] = static_cast<uint8_t>(value & 0xff);
    }
    if (output != bytes.size())
        return false;
    std::memcpy(&payload, bytes.data(), sizeof(payload));
    return true;
}

IEntity* ResolveGlassPhysicsEntity(IPhysicalEntity* physics)
{
    if (!physics || physics == WORLD_ENTITY || !gEnv || !gEnv->pEntitySystem)
        return nullptr;

    IEntity* entity = nullptr;
    std::string reason;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "breakable glass GetEntityFromPhysics",
            [physics]() { return gEnv->pEntitySystem->GetEntityFromPhysics(physics); },
            entity,
            &reason))
    {
        return nullptr;
    }
    return entity;
}

std::string BreakableStatusToken(std::string value)
{
    if (value.empty())
        return "-";
    for (char& ch : value)
    {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '.')
            ch = '_';
    }
    return value;
}

CArkBreakable* GetBreakableExtension(IEntity& entity, std::string* outReason)
{
    std::string localReason;
    std::string& reason = outReason ? *outReason : localReason;
    if (!gEnv || !gEnv->pGame)
    {
        reason = "missing_game_runtime";
        return nullptr;
    }

    IGameFramework* framework = gEnv->pGame->GetIGameFramework();
    IGameObject* gameObject = nullptr;
    if (!framework ||
        !CoopRuntimeGuards::TryGuardedCall(
            "breakable GetGameObject",
            [framework, &entity]() { return framework->GetGameObject(entity.GetId()); },
            gameObject,
            &reason) ||
        !gameObject)
    {
        reason = "missing_breakable_game_object";
        return nullptr;
    }

    constexpr const char* kExtensionNames[] = {"ArkBreakable", "ArkScalableBreakable"};
    for (const char* extensionName : kExtensionNames)
    {
        IGameObjectExtension* extension = nullptr;
        IGameObjectSystem::ExtensionID extensionId = 0;
        if (CoopRuntimeGuards::TryGuardedCall(
                "breakable GetExtensionId",
                [gameObject, extensionName]() { return gameObject->GetExtensionId(extensionName); },
                extensionId,
                &reason) &&
            extensionId != 0 &&
            CoopRuntimeGuards::TryGuardedCall(
                "breakable QueryExtension id",
                [gameObject, extensionId]() { return gameObject->QueryExtension(extensionId); },
                extension,
                &reason) &&
            extension)
        {
            return static_cast<CArkBreakable*>(extension);
        }

        extension = nullptr;
        if (CoopRuntimeGuards::TryGuardedCall(
                "breakable QueryExtension name",
                [gameObject, extensionName]() { return gameObject->QueryExtension(extensionName); },
                extension,
                &reason) &&
            extension)
        {
            return static_cast<CArkBreakable*>(extension);
        }
    }

    reason = "missing_breakable_extension";
    return nullptr;
}

uint16_t EncodeBreakableHealth(float health)
{
    const float bounded = std::clamp(health, 0.0f, kMaxHalfFloat);
    uint16_t encoded = static_cast<uint16_t>(CryConvertFloatToHalf(bounded));
    while (encoded > 0 && CryConvertHalfToFloat(static_cast<CryHalf>(encoded)) > bounded)
        --encoded;
    return encoded;
}

bool DecodeBreakableHealth(uint16_t encoded, float& health)
{
    health = CryConvertHalfToFloat(static_cast<CryHalf>(encoded));
    return std::isfinite(health) && health >= 0.0f && health <= kMaxHalfFloat;
}

CArkBreakable* GetDebugBreakable(IEntity** outEntity, std::string& reason)
{
    if (outEntity)
        *outEntity = nullptr;
    if (!gEnv || !gEnv->pEntitySystem)
    {
        reason = "missing_entity_system";
        return nullptr;
    }

    IEntity* entity = nullptr;
    constexpr EntityGUID kDebugGuids[] = {kDebugScalableBreakableGuid, kDebugBreakableGuid};
    for (const EntityGUID guid : kDebugGuids)
    {
        EntityId entityId = INVALID_ENTITYID;
        if (CoopRuntimeGuards::TryGuardedCall(
                "debug breakable FindEntityByGuid",
                [guid]() { return gEnv->pEntitySystem->FindEntityByGuid(guid); },
                entityId,
                &reason) &&
            entityId != INVALID_ENTITYID &&
            CoopRuntimeGuards::TryGuardedCall(
                "debug breakable GetEntity",
                [entityId]() { return gEnv->pEntitySystem->GetEntity(entityId); },
                entity,
                &reason) &&
            entity)
        {
            break;
        }
    }
    if (!entity)
    {
        reason = "missing_debug_breakable";
        return nullptr;
    }

    if (outEntity)
        *outEntity = entity;
    return GetBreakableExtension(*entity, &reason);
}

CArkBreakable* GetBreakableByGuid(uint64_t guid, IEntity** outEntity, std::string& reason)
{
    if (outEntity)
        *outEntity = nullptr;
    if (!gEnv || !gEnv->pEntitySystem || guid == 0)
    {
        reason = "missing_breakable_target";
        return nullptr;
    }

    EntityId entityId = INVALID_ENTITYID;
    IEntity* entity = nullptr;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "target breakable FindEntityByGuid",
            [guid]() { return gEnv->pEntitySystem->FindEntityByGuid(static_cast<EntityGUID>(guid)); },
            entityId,
            &reason) ||
        entityId == INVALID_ENTITYID ||
        !CoopRuntimeGuards::TryGuardedCall(
            "target breakable GetEntity",
            [entityId]() { return gEnv->pEntitySystem->GetEntity(entityId); },
            entity,
            &reason) ||
        !entity)
    {
        reason = "missing_breakable_guid_" + std::to_string(guid);
        return nullptr;
    }

    CArkBreakable* breakable = GetBreakableExtension(*entity, &reason);
    if (breakable && outEntity)
        *outEntity = entity;
    return breakable;
}

CArkBreakable* FindNearestAuthoredBreakable(IEntity** outEntity, std::string& reason)
{
    if (outEntity)
        *outEntity = nullptr;
    if (!gEnv || !gEnv->pEntitySystem || !ArkPlayer::GetInstancePtr() || !ArkPlayer::GetInstance().GetEntity())
    {
        reason = "missing_breakable_runtime";
        return nullptr;
    }

    IEntityIt* rawIterator = nullptr;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "nearest breakable GetEntityIterator",
            []() { return gEnv->pEntitySystem->GetEntityIterator(); },
            rawIterator,
            &reason) ||
        !rawIterator)
    {
        return nullptr;
    }

    const Vec3 playerPosition = ArkPlayer::GetInstance().GetEntity()->GetWorldPos();
    IEntityItPtr iterator = rawIterator;
    CoopRuntimeGuards::TryGuardedVoidCall("nearest breakable MoveFirst", [&iterator]() { iterator->MoveFirst(); }, &reason);
    IEntity* bestEntity = nullptr;
    CArkBreakable* bestBreakable = nullptr;
    float bestDistanceSq = std::numeric_limits<float>::max();
    while (true)
    {
        bool atEnd = false;
        if (!CoopRuntimeGuards::TryGuardedCall("nearest breakable IsEnd", [&iterator]() { return iterator->IsEnd(); }, atEnd, &reason) || atEnd)
            break;

        IEntity* candidate = nullptr;
        if (!CoopRuntimeGuards::TryGuardedCall("nearest breakable Next", [&iterator]() { return iterator->Next(); }, candidate, &reason) || !candidate)
            break;

        uint64_t guid = 0;
        IEntityClass* entityClass = nullptr;
        const char* className = nullptr;
        if (!CoopRuntimeGuards::TryGuardedCall("nearest breakable GetGuid", [candidate]() { return candidate->GetGuid(); }, guid, &reason) || guid == 0 ||
            !CoopRuntimeGuards::TryGuardedCall("nearest breakable GetClass", [candidate]() { return candidate->GetClass(); }, entityClass, &reason) || !entityClass ||
            !CoopRuntimeGuards::TryGuardedCall("nearest breakable class GetName", [entityClass]() { return entityClass->GetName(); }, className, &reason) ||
            !className || std::strcmp(className, "ArkBreakable") != 0)
        {
            continue;
        }

        CArkBreakable* candidateBreakable = GetBreakableExtension(*candidate, &reason);
        float health = 0.0f;
        Vec3 position(ZERO);
        if (!candidateBreakable ||
            !CoopRuntimeGuards::TryGuardedCall("nearest breakable health", [candidateBreakable]() { return candidateBreakable->m_health; }, health, &reason) ||
            health < 7.0f ||
            !CoopRuntimeGuards::TryGuardedCall("nearest breakable position", [candidate]() { return candidate->GetWorldPos(); }, position, &reason))
        {
            continue;
        }

        const float distanceSq = (position - playerPosition).GetLengthSquared();
        if (distanceSq < bestDistanceSq)
        {
            bestDistanceSq = distanceSq;
            bestEntity = candidate;
            bestBreakable = candidateBreakable;
        }
    }

    if (!bestBreakable)
    {
        reason = "missing_authored_breakable";
        return nullptr;
    }
    if (outEntity)
        *outEntity = bestEntity;
    return bestBreakable;
}

void CArkBreakable_SetHealth_Hook(CArkBreakable* breakable, const float health)
{
    float before = 0.0f;
    std::string reason;
    const bool beforeOk = breakable && CoopRuntimeGuards::TryGuardedCall(
        "breakable SetHealth read before",
        [breakable]() { return breakable->m_health; },
        before,
        &reason);

    s_hookCArkBreakableSetHealth.InvokeOrig(breakable, health);

    float after = 0.0f;
    const bool afterOk = breakable && CoopRuntimeGuards::TryGuardedCall(
        "breakable SetHealth read after",
        [breakable]() { return breakable->m_health; },
        after,
        &reason);
    if (gMod && beforeOk && afterOk)
        gMod->OnNativeBreakableHealthChanged(breakable, before, after, "CArkBreakable::SetHealth");
}

int CEntity_SetBreakableGlass_Hook(CEntity* entity, int slot, IArkGlass* glass)
{
    const int result = s_hookCEntitySetBreakableGlass.InvokeOrig(entity, slot, glass);
    if (!entity)
        return result;

    const int actualSlot = slot >= 0 ? slot : result;
    if (actualSlot < 0)
        return result;

    const EntityId entityId = entity->GetId();
    const EntityGUID guid = entity->GetGuid();
    const uint64_t stableId = BuildBreakableGlassStableId(*entity);
    if (entityId == INVALID_ENTITYID || stableId == 0)
        return result;

    if (glass)
    {
        s_nonBreakableGlassEntities.erase(entityId);
        RegisteredBreakableGlass& registered = s_registeredBreakableGlass[entityId];
        if (registered.guid != guid || registered.stableId != stableId)
        {
            registered.guid = guid;
            registered.stableId = stableId;
            registered.slots.clear();
        }
        registered.slots.insert(actualSlot);
        if (s_breakableGlassCollisionCapture.active)
        {
            s_breakableGlassCollisionCapture.entityId = entityId;
            s_breakableGlassCollisionCapture.stableId = stableId;
            s_breakableGlassCollisionCapture.slot = actualSlot;
        }
    }
    else if (const auto registeredIt = s_registeredBreakableGlass.find(entityId);
        registeredIt != s_registeredBreakableGlass.end() &&
        registeredIt->second.guid == guid && registeredIt->second.stableId == stableId)
    {
        registeredIt->second.slots.erase(actualSlot);
        if (registeredIt->second.slots.empty())
        {
            s_registeredBreakableGlass.erase(registeredIt);
            s_nonBreakableGlassEntities[entityId] = stableId;
        }
    }
    return result;
}

void CActionGame_OnCollisionLogged_Breakable_Hook(const EventPhys* event)
{
    EventPhysCollision collision;
    bool collisionOk = false;

    if (event && event->idval == EventPhysCollision::id)
    {
        const auto* source = static_cast<const EventPhysCollision*>(event);
        std::string reason;
        collisionOk = CoopRuntimeGuards::TryGuardedVoidCall(
            "breakable glass collision copy",
            [&collision, source]() { std::memcpy(&collision, source, sizeof(collision)); },
            &reason);
    }

    // Procedural panes do not exist in CEntity's glass registry until Vanilla
    // recognizes a real impact. Capture that native SetBreakableGlass signal
    // instead of guessing at CryEngine material-manager ABI or surface flags.
    const BreakableGlassCollisionCapture previousCapture =
        s_breakableGlassCollisionCapture;
    s_breakableGlassCollisionCapture = {};
    s_breakableGlassCollisionCapture.active = collisionOk;
    s_hookCActionGameOnCollisionLoggedBreakable.InvokeOrig(event);
    const BreakableGlassCollisionCapture captured =
        s_breakableGlassCollisionCapture;
    s_breakableGlassCollisionCapture = previousCapture;

    if (gMod && collisionOk && captured.entityId != INVALID_ENTITYID &&
        captured.stableId != 0 && captured.slot >= 0)
    {
        int glassSide = -1;
        for (int side = 0; side < 2; ++side)
        {
            IEntity* candidate = ResolveGlassPhysicsEntity(collision.pEntity[side]);
            if (candidate && candidate->GetId() == captured.entityId)
            {
                glassSide = side;
                break;
            }
        }
        if (glassSide >= 0)
        {
            gMod->OnNativeBreakableGlassImpact(
                collision, captured.stableId, glassSide, captured.slot);
        }
    }
}
}

void ModMain::InitBreakableSyncHooks()
{
    s_hookCArkBreakableSetHealth.SetHookFunc(&CArkBreakable_SetHealth_Hook);
    s_hookCEntitySetBreakableGlass.SetHookFunc(&CEntity_SetBreakableGlass_Hook);
    s_hookCActionGameOnCollisionLoggedBreakable.SetHookFunc(
        &CActionGame_OnCollisionLogged_Breakable_Hook);
}

void ModMain::OnNativeBreakableHealthChanged(
    CArkBreakable* breakable,
    float before,
    float after,
    const char* reason)
{
    ++m_breakableHealthHookCalls;
    if (!breakable || !std::isfinite(before) || !std::isfinite(after) ||
        after + kBreakableHealthEpsilon >= before ||
        m_applyingRemoteAreaObjectEvent ||
        m_saveLoadGuardActive || m_arkLevelTransitionLoadActive ||
        IsPostLoadNativeQuarantineActive() ||
        m_networkMode == CoopNetworkMode::Off || !IsSessionGameplayReady())
    {
        ++m_breakableHealthEventSkips;
        return;
    }

    IEntity* entity = nullptr;
    EntityGUID guid = 0;
    std::string guardReason;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "breakable SetHealth GetEntity",
            [breakable]() { return breakable->GetEntity(); },
            entity,
            &guardReason) ||
        !entity ||
        !CoopRuntimeGuards::TryGuardedCall(
            "breakable SetHealth GetGuid",
            [entity]() { return entity->GetGuid(); },
            guid,
            &guardReason) ||
        guid == 0)
    {
        ++m_breakableHealthEventSkips;
        m_lastBreakableHealthEvent =
            "skip_missing_guid_reason_" + BreakableStatusToken(guardReason);
        return;
    }

    const uint16_t encodedHealth = EncodeBreakableHealth(after);
    if (!QueueLocalAreaObjectEventForHook(
            CoopProtocol::kAreaObjectEventBreakableHealth,
            static_cast<uint64_t>(guid),
            encodedHealth,
            0,
            reason))
    {
        ++m_breakableHealthEventSkips;
        m_lastBreakableHealthEvent =
            "queue_failed_guid_" + std::to_string(static_cast<uint64_t>(guid)) +
            "_health_" + std::to_string(after);
        return;
    }

    ++m_breakableHealthEventsQueued;
    m_lastBreakableHealthEvent =
        "queued_guid_" + std::to_string(static_cast<uint64_t>(guid)) +
        "_before_" + std::to_string(before) +
        "_after_" + std::to_string(after) +
        "_encoded_" + std::to_string(encodedHealth);
}

void ModMain::OnNativeBreakableGlassImpact(
    const EventPhysCollision& collision,
    uint64_t targetGuid,
    int glassSide,
    int glassSlot)
{
    ++m_breakableGlassHookCalls;
    if (m_applyingRemoteAreaObjectEvent ||
        m_saveLoadGuardActive || m_arkLevelTransitionLoadActive ||
        IsPostLoadNativeQuarantineActive() ||
        m_networkMode == CoopNetworkMode::Off || !IsSessionGameplayReady() ||
        targetGuid == 0 || glassSide < 0 || glassSide > 1 ||
        glassSlot < 0 || glassSlot > std::numeric_limits<uint16_t>::max())
    {
        ++m_breakableGlassEventSkips;
        m_lastBreakableGlassEvent =
            "skip_guid_" + std::to_string(targetGuid) +
            "_side_" + std::to_string(glassSide) +
            "_slot_" + std::to_string(glassSlot);
        return;
    }

    BreakableGlassImpactWire wire = {};
    for (int axis = 0; axis < 3; ++axis)
    {
        wire.point[axis] = collision.pt[axis];
        wire.normal[axis] = collision.n[axis];
        wire.localVelocity[0][axis] = EncodeGlassHalf(collision.vloc[0][axis]);
        wire.localVelocity[1][axis] = EncodeGlassHalf(collision.vloc[1][axis]);
    }
    wire.mass[0] = EncodeGlassHalf(collision.mass[0]);
    wire.mass[1] = EncodeGlassHalf(collision.mass[1]);
    wire.penetration = EncodeGlassHalf(collision.penetration);
    wire.normalImpulse = EncodeGlassHalf(collision.normImpulse);
    wire.radius = EncodeGlassHalf(collision.radius);
    for (int side = 0; side < 2; ++side)
    {
        wire.partId[side] = static_cast<int16_t>(std::clamp(
            collision.partid[side],
            static_cast<int>(std::numeric_limits<int16_t>::min()),
            static_cast<int>(std::numeric_limits<int16_t>::max())));
        wire.materialId[side] = collision.idmat[side];
        wire.primitiveId[side] = collision.iPrim[side];
    }

    if (!std::isfinite(wire.point[0]) || !std::isfinite(wire.point[1]) ||
        !std::isfinite(wire.point[2]) || !std::isfinite(wire.normal[0]) ||
        !std::isfinite(wire.normal[1]) || !std::isfinite(wire.normal[2]))
    {
        ++m_breakableGlassEventSkips;
        m_lastBreakableGlassEvent = "skip_non_finite_impact";
        return;
    }

    const std::string encoded = EncodeGlassPayload(wire);
    if (encoded.size() >= CoopProtocol::kAreaObjectWireTextValueCapacity ||
        !QueueLocalAreaObjectEventForHook(
            CoopProtocol::kAreaObjectEventBreakableGlassImpact,
            targetGuid,
            static_cast<uint16_t>(glassSlot),
            static_cast<uint32_t>(glassSide),
            "CActionGame::OnCollisionLogged_Breakable",
            0,
            encoded.c_str()))
    {
        ++m_breakableGlassEventSkips;
        m_lastBreakableGlassEvent =
            "queue_failed_guid_" + std::to_string(targetGuid) +
            "_side_" + std::to_string(glassSide) +
            "_slot_" + std::to_string(glassSlot);
        return;
    }

    ++m_breakableGlassEventsQueued;
    m_lastBreakableGlassEvent =
        "queued_guid_" + std::to_string(targetGuid) +
        "_side_" + std::to_string(glassSide) +
        "_slot_" + std::to_string(glassSlot) +
        "_point_" + std::to_string(collision.pt.x) + "," +
            std::to_string(collision.pt.y) + "," +
            std::to_string(collision.pt.z);
}

bool ModMain::ApplyAreaObjectBreakableGlassImpact(
    const CoopProtocol::AreaObjectEventPacket& packet,
    std::string& detail)
{
    if (packet.targetGuid == 0 || packet.flags > 1 ||
        packet.targetClassHash != HashStoryString("BreakableGlass") ||
        std::memchr(packet.textValue, '\0', sizeof(packet.textValue)) == nullptr ||
        !gEnv || !gEnv->pEntitySystem)
    {
        ++m_breakableGlassEventSkips;
        detail = "invalid_breakable_glass_payload";
        return false;
    }

    const auto* terminator = static_cast<const char*>(
        std::memchr(packet.textValue, '\0', sizeof(packet.textValue)));
    const size_t encodedLength = static_cast<size_t>(terminator - packet.textValue);
    BreakableGlassImpactWire wire = {};
    if (!DecodeGlassPayload(std::string_view(packet.textValue, encodedLength), wire) ||
        !std::isfinite(wire.point[0]) || !std::isfinite(wire.point[1]) ||
        !std::isfinite(wire.point[2]) || !std::isfinite(wire.normal[0]) ||
        !std::isfinite(wire.normal[1]) || !std::isfinite(wire.normal[2]))
    {
        ++m_breakableGlassEventSkips;
        detail = "invalid_breakable_glass_impact_encoding";
        return false;
    }

    EntityId targetEntityId = INVALID_ENTITYID;
    IEntity* targetEntity = nullptr;
    std::string reason;
    CoopRuntimeGuards::TryGuardedCall(
            "breakable glass apply FindEntityByGuid",
            [&packet]()
            {
                return gEnv->pEntitySystem->FindEntityByGuid(
                    static_cast<EntityGUID>(packet.targetGuid));
            },
            targetEntityId,
            &reason);
    if (targetEntityId != INVALID_ENTITYID)
    {
        CoopRuntimeGuards::TryGuardedCall(
            "breakable glass apply GetEntity",
            [targetEntityId]() { return gEnv->pEntitySystem->GetEntity(targetEntityId); },
            targetEntity,
            &reason);
    }

    if (!targetEntity)
    {
        RefreshAllBreakableGlassRegistrations();
        for (auto it = s_registeredBreakableGlass.begin();
             it != s_registeredBreakableGlass.end();)
        {
            IEntity* candidate = gEnv->pEntitySystem->GetEntity(it->first);
            if (!candidate || it->second.stableId == 0 ||
                it->second.stableId != BuildBreakableGlassStableId(*candidate) ||
                it->second.slots.empty())
            {
                it = s_registeredBreakableGlass.erase(it);
                continue;
            }
            if (it->second.stableId == packet.targetGuid)
            {
                targetEntity = candidate;
                targetEntityId = candidate->GetId();
                break;
            }
            ++it;
        }
    }

    if (!targetEntity)
    {
        ++m_breakableGlassEventSkips;
        detail = "missing_breakable_glass_stable_id_" +
            std::to_string(packet.targetGuid);
        return false;
    }

    const int glassSide = static_cast<int>(packet.flags);
    const int sourceSide = 1 - glassSide;
    IPhysicalEntity* targetPhysics = targetEntity->GetPhysics();
    if (!targetPhysics || targetPhysics == WORLD_ENTITY)
    {
        ++m_breakableGlassEventSkips;
        detail = "missing_breakable_glass_physics_guid_" + std::to_string(packet.targetGuid);
        return false;
    }

    IEntity* sourceEntity = nullptr;
    if (packet.sourcePeerHash == GetLocalAccountToken())
    {
        sourceEntity = ArkPlayer::GetInstancePtr()
            ? ArkPlayer::GetInstance().GetEntity()
            : nullptr;
    }
    else
    {
        const auto sourcePeerIt = m_remotePeers.find(packet.sourcePeerHash);
        if (sourcePeerIt != m_remotePeers.end() &&
            sourcePeerIt->second.proxyEntityId != INVALID_ENTITYID)
        {
            sourceEntity = gEnv->pEntitySystem->GetEntity(sourcePeerIt->second.proxyEntityId);
        }
        if (!sourceEntity)
            sourceEntity = GetProxyEntity();
    }

    EventPhysCollision collision;
    collision.pEntity[glassSide] = targetPhysics;
    collision.pForeignData[glassSide] = targetEntity;
    collision.iForeignData[glassSide] = PHYS_FOREIGN_ID_ENTITY;
    if (sourceEntity && sourceEntity->GetPhysics())
    {
        collision.pEntity[sourceSide] = sourceEntity->GetPhysics();
        collision.pForeignData[sourceSide] = sourceEntity;
        collision.iForeignData[sourceSide] = PHYS_FOREIGN_ID_ENTITY;
    }
    else
    {
        collision.pEntity[sourceSide] = WORLD_ENTITY;
        collision.pForeignData[sourceSide] = nullptr;
        collision.iForeignData[sourceSide] = -1;
    }
    collision.pt = Vec3(wire.point[0], wire.point[1], wire.point[2]);
    collision.n = Vec3(wire.normal[0], wire.normal[1], wire.normal[2]);
    for (int side = 0; side < 2; ++side)
    {
        collision.vloc[side] = Vec3(
            DecodeGlassHalf(wire.localVelocity[side][0]),
            DecodeGlassHalf(wire.localVelocity[side][1]),
            DecodeGlassHalf(wire.localVelocity[side][2]));
        collision.mass[side] = DecodeGlassHalf(wire.mass[side]);
        collision.partid[side] = wire.partId[side];
        collision.idmat[side] = wire.materialId[side];
        collision.iPrim[side] = wire.primitiveId[side];
    }
    collision.penetration = DecodeGlassHalf(wire.penetration);
    collision.normImpulse = DecodeGlassHalf(wire.normalImpulse);
    collision.radius = DecodeGlassHalf(wire.radius);

    if (!CoopRuntimeGuards::TryGuardedVoidCall(
            "breakable glass apply native collision",
            [&collision]()
            {
                CActionGame::OnCollisionLogged_Breakable(&collision);
            },
            &reason))
    {
        ++m_breakableGlassEventSkips;
        detail = "breakable_glass_native_apply_failed_guid_" +
            std::to_string(packet.targetGuid) + "_reason_" +
            BreakableStatusToken(reason);
        return false;
    }

    ++m_breakableGlassEventsApplied;
    m_lastBreakableGlassEvent =
        "applied_guid_" + std::to_string(packet.targetGuid) +
        "_side_" + std::to_string(glassSide) +
        "_slot_" + std::to_string(packet.value);
    detail = m_lastBreakableGlassEvent;
    return true;
}

bool ModMain::ApplyAreaObjectBreakableHealth(
    uint64_t targetGuid,
    uint16_t encodedHealth,
    std::string& detail)
{
    float requestedHealth = 0.0f;
    if (targetGuid == 0 || !DecodeBreakableHealth(encodedHealth, requestedHealth) ||
        !gEnv || !gEnv->pEntitySystem)
    {
        ++m_breakableHealthEventSkips;
        detail = "invalid_breakable_health_payload";
        return false;
    }

    EntityId entityId = INVALID_ENTITYID;
    IEntity* entity = nullptr;
    std::string reason;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "breakable apply FindEntityByGuid",
            [targetGuid]() { return gEnv->pEntitySystem->FindEntityByGuid(static_cast<EntityGUID>(targetGuid)); },
            entityId,
            &reason) ||
        entityId == INVALID_ENTITYID ||
        !CoopRuntimeGuards::TryGuardedCall(
            "breakable apply GetEntity",
            [entityId]() { return gEnv->pEntitySystem->GetEntity(entityId); },
            entity,
            &reason) ||
        !entity)
    {
        if (requestedHealth <= kBreakableHealthEpsilon)
        {
            detail = "already_missing_broken_breakable_guid_" + std::to_string(targetGuid);
            return true;
        }
        ++m_breakableHealthEventSkips;
        detail = "missing_breakable_guid_" + std::to_string(targetGuid);
        return false;
    }

    CArkBreakable* breakable = GetBreakableExtension(*entity, &reason);
    float before = 0.0f;
    if (!breakable ||
        !CoopRuntimeGuards::TryGuardedCall(
            "breakable apply read before",
            [breakable]() { return breakable->m_health; },
            before,
            &reason) ||
        !std::isfinite(before))
    {
        ++m_breakableHealthEventSkips;
        detail = "missing_breakable_extension_guid_" + std::to_string(targetGuid) +
            "_reason_" + BreakableStatusToken(reason);
        return false;
    }

    const float desiredHealth = std::min(before, requestedHealth);
    if (before <= desiredHealth + kBreakableHealthEpsilon)
    {
        detail = "already_breakable_health_guid_" + std::to_string(targetGuid) +
            "_health_" + std::to_string(before);
        return true;
    }

    if (!CoopRuntimeGuards::TryGuardedVoidCall(
            "breakable apply SetHealth",
            [breakable, desiredHealth]() { breakable->SetHealth(desiredHealth); },
            &reason))
    {
        ++m_breakableHealthEventSkips;
        detail = "breakable_set_health_failed_guid_" + std::to_string(targetGuid) +
            "_reason_" + BreakableStatusToken(reason);
        return false;
    }

    float after = before;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "breakable apply read after",
            [breakable]() { return breakable->m_health; },
            after,
            &reason) ||
        !std::isfinite(after) || after > desiredHealth + kBreakableHealthEpsilon)
    {
        ++m_breakableHealthEventSkips;
        detail = "breakable_health_verify_failed_guid_" + std::to_string(targetGuid) +
            "_before_" + std::to_string(before) +
            "_desired_" + std::to_string(desiredHealth) +
            "_after_" + std::to_string(after);
        return false;
    }

    ++m_breakableHealthEventsApplied;
    m_lastBreakableHealthEvent =
        "applied_guid_" + std::to_string(targetGuid) +
        "_before_" + std::to_string(before) +
        "_after_" + std::to_string(after) +
        "_encoded_" + std::to_string(encodedHealth);
    detail = m_lastBreakableHealthEvent;
    return true;
}

bool ModMain::DebugSpawnBreakableSyncTarget(std::string& detail, bool scalable)
{
    std::string reason;
    IEntity* entity = nullptr;
    CArkBreakable* breakable = GetDebugBreakable(&entity, reason);
    if (!breakable)
    {
        if (!gEnv || !gEnv->pEntitySystem ||
            !ArkPlayer::GetInstancePtr() || !ArkPlayer::GetInstance().GetEntity())
        {
            detail = "debug_breakable_runtime_unavailable";
            return false;
        }

        const uint64_t archetypeId = scalable ? kDebugScalableBreakableArchetype : kDebugBreakableArchetype;
        const EntityGUID guid = scalable ? kDebugScalableBreakableGuid : kDebugBreakableGuid;
        IEntityArchetype* archetype = nullptr;
        const bool hasArchetype = CoopRuntimeGuards::TryGuardedCall(
                "debug breakable GetEntityArchetype",
                [archetypeId]() { return gEnv->pEntitySystem->GetEntityArchetype(archetypeId); },
                archetype,
                &reason) && archetype;
        if (!hasArchetype)
        {
            if (scalable)
            {
                detail = "missing_debug_breakable_archetype_" + std::to_string(archetypeId);
                return false;
            }
            breakable = FindNearestAuthoredBreakable(&entity, reason);
        }
        else
        {
            IEntity* playerEntity = ArkPlayer::GetInstance().GetEntity();
            SEntitySpawnParams params;
            params.guid = guid;
            params.sName = scalable ? "CoopScalableBreakableSyncTest" : "CoopBreakableSyncTest";
            params.pArchetype = archetype;
            params.pClass = archetype->GetClass();
            params.vPosition = playerEntity->GetWorldPos() + playerEntity->GetWorldRotation().GetColumn1() * 3.0f;
            params.qRotation = playerEntity->GetWorldRotation();
            if (!CoopRuntimeGuards::TryGuardedCall(
                    "debug breakable SpawnEntityFromArchetype",
                    [&params, archetype]() { return gEnv->pEntitySystem->SpawnEntityFromArchetype(archetype, params, true); },
                    entity,
                    &reason) ||
                !entity)
            {
                if (scalable)
                {
                    detail = "debug_breakable_spawn_failed_reason_" + BreakableStatusToken(reason);
                    return false;
                }
                breakable = FindNearestAuthoredBreakable(&entity, reason);
            }
            else
            {
                breakable = GetBreakableExtension(*entity, &reason);
            }
        }
    }

    if (!breakable || !entity)
    {
        detail = "debug_breakable_extension_failed_reason_" + BreakableStatusToken(reason);
        return false;
    }

    float health = 0.0f;
    if (!CoopRuntimeGuards::TryGuardedCall(
            "debug breakable read spawned health",
            [breakable]() { return breakable->m_health; },
            health,
            &reason))
    {
        detail = "debug_breakable_health_read_failed_reason_" + BreakableStatusToken(reason);
        return false;
    }
    detail = "debug_breakable_entity_" + std::to_string(entity->GetId()) +
        "_guid_" + std::to_string(static_cast<uint64_t>(entity->GetGuid())) +
        "_health_" + std::to_string(health);
    return true;
}

bool ModMain::DebugProbeBreakableSyncTarget(std::string& detail, uint64_t targetGuid)
{
    std::string reason;
    IEntity* entity = nullptr;
    CArkBreakable* breakable = targetGuid != 0 ?
        GetBreakableByGuid(targetGuid, &entity, reason) : GetDebugBreakable(&entity, reason);
    float health = 0.0f;
    if (!breakable || !entity ||
        !CoopRuntimeGuards::TryGuardedCall(
            "debug breakable probe health",
            [breakable]() { return breakable->m_health; },
            health,
            &reason))
    {
        detail = "debug_breakable_probe_failed_reason_" + BreakableStatusToken(reason);
        return false;
    }
    detail = "debug_breakable_entity_" + std::to_string(entity->GetId()) +
        "_guid_" + std::to_string(static_cast<uint64_t>(entity->GetGuid())) +
        "_health_" + std::to_string(health) +
        "_broken_" + std::to_string(health <= kBreakableHealthEpsilon ? 1 : 0);
    return true;
}

bool ModMain::DebugSetBreakableSyncTargetHealth(float health, std::string& detail, uint64_t targetGuid)
{
    if (!std::isfinite(health) || health < 0.0f || health > kMaxHalfFloat)
    {
        detail = "invalid_debug_breakable_health";
        return false;
    }

    std::string reason;
    IEntity* entity = nullptr;
    CArkBreakable* breakable = targetGuid != 0 ?
        GetBreakableByGuid(targetGuid, &entity, reason) : GetDebugBreakable(&entity, reason);
    if (!breakable || !entity ||
        !CoopRuntimeGuards::TryGuardedVoidCall(
            "debug breakable SetHealth",
            [breakable, health]() { breakable->SetHealth(health); },
            &reason))
    {
        detail = "debug_breakable_set_failed_reason_" + BreakableStatusToken(reason);
        return false;
    }
    return DebugProbeBreakableSyncTarget(detail, targetGuid);
}

bool ModMain::DebugBreakableGlassCommand(
    const std::string& target,
    bool impact,
    std::string& detail)
{
    if (!gEnv || !gEnv->pEntitySystem ||
        !ArkPlayer::GetInstancePtr() || !ArkPlayer::GetInstance().GetEntity())
    {
        detail = "breakable_glass_runtime_unavailable";
        return false;
    }

    if (impact)
    {
        detail = "breakable_glass_impact_requires_native_weapon_collision";
        return false;
    }

    RefreshAllBreakableGlassRegistrations();
    IEntity* selectedEntity = nullptr;
    float selectedDistanceSq = std::numeric_limits<float>::max();
    size_t validTargets = 0;
    size_t validSlots = 0;
    const Vec3 playerPosition =
        ArkPlayer::GetInstance().GetEntity()->GetWorldPos();

    if (target != "nearest")
    {
        std::string targetDetail;
        selectedEntity = ResolveRuntimeEntityTarget(target, targetDetail);
        if (!selectedEntity)
        {
            detail = "breakable_glass_target_missing_" +
                BreakableStatusToken(targetDetail);
            return false;
        }
    }

    for (auto it = s_registeredBreakableGlass.begin();
         it != s_registeredBreakableGlass.end();)
    {
        IEntity* entity = gEnv->pEntitySystem->GetEntity(it->first);
        if (!entity || it->second.stableId == 0 ||
            entity->GetGuid() != it->second.guid ||
            it->second.stableId != BuildBreakableGlassStableId(*entity) ||
            it->second.slots.empty())
        {
            it = s_registeredBreakableGlass.erase(it);
            continue;
        }

        ++validTargets;
        validSlots += it->second.slots.size();
        if (target == "nearest")
        {
            const float distanceSq =
                (entity->GetWorldPos() - playerPosition).GetLengthSquared();
            if (distanceSq < selectedDistanceSq)
            {
                selectedEntity = entity;
                selectedDistanceSq = distanceSq;
            }
        }
        ++it;
    }

    const auto registered = selectedEntity
        ? s_registeredBreakableGlass.find(selectedEntity->GetId())
        : s_registeredBreakableGlass.end();
    if (!selectedEntity || registered == s_registeredBreakableGlass.end() ||
        registered->second.slots.empty())
    {
        detail = "breakable_glass_not_yet_native_registered_targets_" +
            std::to_string(validTargets) + "_slots_" +
            std::to_string(validSlots);
        return false;
    }

    detail = "breakable_glass_target_id_" +
        std::to_string(selectedEntity->GetId()) + "_guid_" +
        std::to_string(registered->second.stableId) + "_slot_" +
        std::to_string(*registered->second.slots.begin()) + "_targets_" +
        std::to_string(validTargets) + "_slots_" +
        std::to_string(validSlots);
    return true;
}
