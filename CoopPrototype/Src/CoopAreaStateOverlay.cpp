#include "CoopAreaStateOverlay.h"
#include "CoopRuntimeGuards.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string_view>

#include <Prey/CryEntitySystem/IEntity.h>
#include <Prey/CryEntitySystem/IEntityClass.h>
#include <Prey/CryEntitySystem/IEntitySystem.h>
#include <Prey/CrySystem/ISystem.h>
#include <Prey/GameDll/ark/player/ArkPlayer.h>

namespace
{
using CoopRuntimeGuards::IsLikelyRuntimeCppObject;
using CoopRuntimeGuards::TryGuardedCall;
using CoopRuntimeGuards::TryGuardedVoidCall;

constexpr size_t kMaxOverlayRows = 8192;

bool ExtractStringField(std::string_view line, std::string_view key, std::string& outValue)
{
    const size_t keyPos = line.find(key);
    if (keyPos == std::string_view::npos)
        return false;

    size_t valueStart = keyPos + key.size();
    if (valueStart >= line.size() || line[valueStart] != '"')
        return false;
    ++valueStart;

    std::string value;
    value.reserve(64);
    bool escaped = false;
    for (size_t i = valueStart; i < line.size(); ++i)
    {
        const char ch = line[i];
        if (escaped)
        {
            switch (ch)
            {
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            case '"':
            case '\\':
            case '/':
                value.push_back(ch);
                break;
            default:
                value.push_back(ch);
                break;
            }
            escaped = false;
            continue;
        }

        if (ch == '\\')
        {
            escaped = true;
            continue;
        }

        if (ch == '"')
        {
            outValue = std::move(value);
            return true;
        }

        value.push_back(ch);
    }

    return false;
}

bool ExtractUint64Field(std::string_view line, std::string_view key, uint64_t& outValue)
{
    const size_t keyPos = line.find(key);
    if (keyPos == std::string_view::npos)
        return false;

    const size_t valueStart = keyPos + key.size();
    if (valueStart >= line.size())
        return false;

    std::string copy(line.substr(valueStart, std::min<size_t>(32, line.size() - valueStart)));
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(copy.c_str(), &end, 10);
    if (errno != 0 || !end || end == copy.c_str())
        return false;

    outValue = static_cast<uint64_t>(parsed);
    return true;
}

bool ExtractBool01Field(std::string_view line, std::string_view key, bool& outValue)
{
    const size_t keyPos = line.find(key);
    if (keyPos == std::string_view::npos)
        return false;

    const size_t valueStart = keyPos + key.size();
    if (valueStart >= line.size())
        return false;

    if (line[valueStart] == '1')
    {
        outValue = true;
        return true;
    }

    if (line[valueStart] == '0')
    {
        outValue = false;
        return true;
    }

    if (line.substr(valueStart, 4) == "true")
    {
        outValue = true;
        return true;
    }

    if (line.substr(valueStart, 5) == "false")
    {
        outValue = false;
        return true;
    }

    return false;
}

bool ParseFloatToken(const char*& cursor, float& outValue)
{
    while (*cursor == ' ' || *cursor == ',')
        ++cursor;

    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(cursor, &end);
    if (errno != 0 || !end || end == cursor || !std::isfinite(parsed))
        return false;

    outValue = parsed;
    cursor = end;
    return true;
}

bool ExtractVec3Field(std::string_view line, std::string_view key, Vec3& outValue)
{
    const size_t keyPos = line.find(key);
    if (keyPos == std::string_view::npos)
        return false;

    const size_t bracket = line.find('[', keyPos + key.size());
    if (bracket == std::string_view::npos)
        return false;

    std::string copy(line.substr(bracket + 1, std::min<size_t>(128, line.size() - bracket - 1)));
    const char* cursor = copy.c_str();
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (!ParseFloatToken(cursor, x) || !ParseFloatToken(cursor, y) || !ParseFloatToken(cursor, z))
        return false;

    outValue = Vec3(x, y, z);
    return true;
}

bool ExtractQuatField(std::string_view line, std::string_view key, Quat& outValue)
{
    const size_t keyPos = line.find(key);
    if (keyPos == std::string_view::npos)
        return false;

    const size_t bracket = line.find('[', keyPos + key.size());
    if (bracket == std::string_view::npos)
        return false;

    std::string copy(line.substr(bracket + 1, std::min<size_t>(160, line.size() - bracket - 1)));
    const char* cursor = copy.c_str();
    float w = 1.0f;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (!ParseFloatToken(cursor, w) || !ParseFloatToken(cursor, x) || !ParseFloatToken(cursor, y) || !ParseFloatToken(cursor, z))
        return false;

    outValue = Quat(w, x, y, z);
    outValue.Normalize();
    return true;
}

bool IsCoopRuntimeEntityName(const char* name)
{
    if (!name)
        return false;

    std::string value(name);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
    {
        return static_cast<char>(std::tolower(ch));
    });
    return value.rfind("coop_", 0) == 0 || value.find("coopprototype") != std::string::npos;
}

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
    {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool IsBlockedAreaOverlayClass(const std::string& className)
{
    const std::string value = ToLowerAscii(className);
    return value.empty() ||
        value == "player" ||
        value.find("arkplayer") != std::string::npos ||
        value.find("arkhuman") != std::string::npos ||
        value.find("arknpc") != std::string::npos ||
        value.find("arkmimic") != std::string::npos ||
        value.find("arkphantom") != std::string::npos ||
        value.find("arknightmare") != std::string::npos ||
        value.find("arkoperator") != std::string::npos ||
        value.find("arkweaver") != std::string::npos ||
        value.find("arkcystoid") != std::string::npos ||
        value.find("apextentacle") != std::string::npos ||
        value.find("arklight") != std::string::npos ||
        value.find("leveltransition") != std::string::npos ||
        value.find("trigger") != std::string::npos ||
        value.find("volume") != std::string::npos ||
        value.find("flowgraph") != std::string::npos;
}

bool ShouldSkipAreaOverlayEntity(IEntity* entity, const std::string& rowClassName)
{
    if (!entity)
        return true;

    if (IsBlockedAreaOverlayClass(rowClassName))
        return true;

    EntityId entityId = INVALID_ENTITYID;
    std::string reason;
    TryGuardedCall("area overlay GetId guard", [entity]() { return entity->GetId(); }, entityId, &reason);
    if (ArkPlayer::GetInstancePtr())
    {
        IEntity* playerEntity = ArkPlayer::GetInstance().GetEntity();
        if (playerEntity && entityId != INVALID_ENTITYID && playerEntity->GetId() == entityId)
            return true;
    }

    IEntityClass* entityClass = nullptr;
    if (!TryGuardedCall("area overlay GetClass guard", [entity]() { return entity->GetClass(); }, entityClass, &reason) ||
        !IsLikelyRuntimeCppObject(entityClass))
    {
        return true;
    }

    const char* rawClassName = nullptr;
    if (!TryGuardedCall("area overlay class GetName guard", [entityClass]() { return entityClass->GetName(); }, rawClassName, &reason))
        return true;

    return IsBlockedAreaOverlayClass(rawClassName ? std::string(rawClassName) : std::string());
}
}

bool ApplyCoopAreaStateOverlayJsonl(
    const std::filesystem::path& path,
    const std::string& levelName,
    CoopAreaStateOverlayApplyStats& outStats,
    std::vector<uint64_t>* outTransformedGuids)
{
    outStats = {};
    if (outTransformedGuids)
        outTransformedGuids->clear();

    if (!gEnv || !gEnv->pEntitySystem)
    {
        outStats.lastEvent = "overlay apply failed: no entity system";
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        outStats.lastEvent = "overlay apply failed: open failed";
        return false;
    }

    std::string line;
    while (std::getline(input, line))
    {
        if (outStats.rows >= kMaxOverlayRows)
        {
            ++outStats.skippedRows;
            continue;
        }

        if (line.find("\"type\":\"area_journal_entity\"") == std::string::npos)
            continue;

        ++outStats.rows;

        std::string rowLevel;
        if (!ExtractStringField(line, "\"level\":", rowLevel) || rowLevel != levelName)
        {
            ++outStats.skippedRows;
            continue;
        }
        ++outStats.levelRows;

        std::string rowClassName;
        ExtractStringField(line, "\"class\":", rowClassName);

        uint64_t guid = 0;
        Vec3 position = ZERO;
        Vec3 scale = Vec3Constants<float>::fVec3_One;
        Quat rotation = Quat::CreateIdentity();
        bool hidden = false;
        bool removed = false;
        if (!ExtractUint64Field(line, "\"guid\":", guid) ||
            !ExtractVec3Field(line, "\"pos\":", position) ||
            !ExtractQuatField(line, "\"rot\":", rotation) ||
            !ExtractVec3Field(line, "\"scale\":", scale))
        {
            ++outStats.errors;
            continue;
        }

        ExtractBool01Field(line, "\"hidden\":", hidden);
        ExtractBool01Field(line, "\"removed\":", removed);

        EntityId entityId = INVALID_ENTITYID;
        std::string reason;
        if (!TryGuardedCall(
                "area overlay FindEntityByGuid",
                [&]() -> EntityId
                {
                    return gEnv->pEntitySystem->FindEntityByGuid(guid);
                },
                entityId,
                &reason) ||
            entityId == INVALID_ENTITYID)
        {
            ++outStats.missingEntities;
            continue;
        }

        IEntity* entity = nullptr;
        if (!TryGuardedCall(
                "area overlay GetEntity",
                [&]() -> IEntity*
                {
                    return gEnv->pEntitySystem->GetEntity(entityId);
                },
                entity,
                &reason) ||
            !entity ||
            !IsLikelyRuntimeCppObject(entity) ||
            entity->IsGarbage())
        {
            ++outStats.missingEntities;
            continue;
        }

        const char* entityName = nullptr;
        TryGuardedCall("area overlay GetName", [entity]() -> const char* { return entity->GetName(); }, entityName, &reason);
        if (IsCoopRuntimeEntityName(entityName) || ShouldSkipAreaOverlayEntity(entity, rowClassName))
        {
            ++outStats.skippedRows;
            continue;
        }

        ++outStats.matchedEntities;

        if (removed)
        {
            const bool removedOk = TryGuardedVoidCall(
                "area overlay RemoveEntity",
                [&]()
                {
                    entity->SetFlags(entity->GetFlags() & ~static_cast<uint32_t>(ENTITY_FLAG_UNREMOVABLE));
                    gEnv->pEntitySystem->RemoveEntity(entityId, true);
                },
                &reason);
            if (removedOk)
                ++outStats.removedEntities;
            else
                ++outStats.errors;
            continue;
        }

        const bool transformOk = TryGuardedVoidCall(
            "area overlay SetPosRotScale",
            [&]()
            {
                entity->SetPosRotScale(position, rotation, scale, ENTITY_XFORM_USER);
            },
            &reason);
        if (transformOk)
        {
            ++outStats.transformedEntities;
            if (outTransformedGuids)
                outTransformedGuids->push_back(guid);
        }
        else
            ++outStats.errors;

        const bool hideOk = TryGuardedVoidCall(
            "area overlay Hide",
            [&]()
            {
                entity->Hide(hidden);
            },
            &reason);
        if (hideOk && hidden)
            ++outStats.hiddenEntities;
        else if (!hideOk)
            ++outStats.errors;
    }

    outStats.lastEvent =
        "overlay rows=" + std::to_string(outStats.levelRows) +
        " matched=" + std::to_string(outStats.matchedEntities) +
        " xform=" + std::to_string(outStats.transformedEntities) +
        " hidden=" + std::to_string(outStats.hiddenEntities) +
        " removed=" + std::to_string(outStats.removedEntities) +
        " missing=" + std::to_string(outStats.missingEntities) +
        " errors=" + std::to_string(outStats.errors);
    return outStats.errors == 0;
}
