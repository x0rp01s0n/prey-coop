#include "CoopAreaStateJournal.h"
#include "CoopRuntimeGuards.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ostream>
#include <sstream>

#include <Prey/CryEntitySystem/IEntityClass.h>
#include <Prey/GameDll/ark/player/ArkPlayer.h>

namespace
{
using CoopRuntimeGuards::IsLikelyRuntimeCppObject;
using CoopRuntimeGuards::ReadRuntimeCString;
using CoopRuntimeGuards::TryGuardedCall;

constexpr uint32_t EventBit(EEntityEvent event)
{
    const uint32_t index = static_cast<uint32_t>(event);
    return index < 31 ? (1u << index) : 0x80000000u;
}

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string JsonEscape(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value)
    {
        switch (ch)
        {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20)
            {
                char escaped[8] = {};
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned char>(ch));
                out += escaped;
            }
            else
            {
                out.push_back(ch);
            }
            break;
        }
    }
    return out;
}

bool ExportJsonlFiltered(
    const std::unordered_map<uint64_t, CoopAreaStateJournal::DirtyEntity>& dirtyEntities,
    const std::unordered_map<std::string, CoopAreaStateJournal::LevelStats>& levelStats,
    uint32_t observedEventCount,
    uint32_t trackedEventCount,
    uint32_t guardSkips,
    uint32_t boundarySaveCount,
    uint32_t boundaryLoadCount,
    const std::string& lastEvent,
    const std::string* onlyLevel,
    std::ostream& output,
    size_t maxEntityRows)
{
    if (!output)
        return false;

    size_t dirtyCount = 0;
    if (onlyLevel)
    {
        for (const auto& [guid, dirty] : dirtyEntities)
        {
            (void)guid;
            if (dirty.levelName == *onlyLevel)
                ++dirtyCount;
        }
    }
    else
    {
        dirtyCount = dirtyEntities.size();
    }

    output
        << "{\"type\":\"area_journal_summary\""
        << ",\"levels\":" << (onlyLevel ? 1 : levelStats.size())
        << ",\"dirtyEntities\":" << dirtyCount
        << ",\"observedEvents\":" << observedEventCount
        << ",\"trackedEvents\":" << trackedEventCount
        << ",\"guardSkips\":" << guardSkips
        << ",\"saveBoundaries\":" << boundarySaveCount
        << ",\"loadBoundaries\":" << boundaryLoadCount
        << ",\"level\":\"" << JsonEscape(onlyLevel ? *onlyLevel : std::string("*")) << "\""
        << ",\"lastEvent\":\"" << JsonEscape(lastEvent) << "\""
        << "}\n";

    for (const auto& [levelName, stats] : levelStats)
    {
        if (onlyLevel && levelName != *onlyLevel)
            continue;

        output
            << "{\"type\":\"area_journal_level\""
            << ",\"level\":\"" << JsonEscape(levelName) << "\""
            << ",\"dirtyEvents\":" << stats.dirtyEvents
            << ",\"dirtyEntities\":" << stats.dirtyEntities
            << ",\"saveBoundaries\":" << stats.saveBoundaries
            << ",\"loadBoundaries\":" << stats.loadBoundaries
            << "}\n";
    }

    size_t written = 0;
    for (const auto& [guid, dirty] : dirtyEntities)
    {
        if (onlyLevel && dirty.levelName != *onlyLevel)
            continue;
        if (written >= maxEntityRows)
            break;

        output
            << "{\"type\":\"area_journal_entity\""
            << ",\"level\":\"" << JsonEscape(dirty.levelName) << "\""
            << ",\"guid\":" << guid
            << ",\"entityId\":" << dirty.entityId
            << ",\"class\":\"" << JsonEscape(dirty.className) << "\""
            << ",\"name\":\"" << JsonEscape(dirty.entityName) << "\""
            << ",\"pos\":[" << dirty.position.x << "," << dirty.position.y << "," << dirty.position.z << "]"
            << ",\"rot\":[" << dirty.rotation.w << "," << dirty.rotation.v.x << "," << dirty.rotation.v.y << "," << dirty.rotation.v.z << "]"
            << ",\"scale\":[" << dirty.scale.x << "," << dirty.scale.y << "," << dirty.scale.z << "]"
            << ",\"flags\":" << dirty.flags
            << ",\"eventMask\":" << dirty.eventMask
            << ",\"xformEvents\":" << dirty.xformEvents
            << ",\"removeEvents\":" << dirty.removeEvents
            << ",\"hidden\":" << (dirty.hidden ? 1 : 0)
            << ",\"removed\":" << (dirty.removed ? 1 : 0)
            << "}\n";
        ++written;
    }

    if (written < dirtyCount)
    {
        output
            << "{\"type\":\"area_journal_truncated\""
            << ",\"written\":" << written
            << ",\"total\":" << dirtyCount
            << "}\n";
    }

    return static_cast<bool>(output);
}
}

void CoopAreaStateJournal::NoteEntityEvent(const std::string& levelName, IEntity& entity, const SEntityEvent& event)
{
    ++m_observedEventCount;

    if (event.event != ENTITY_EVENT_XFORM &&
        event.event != ENTITY_EVENT_DONE &&
        event.event != ENTITY_EVENT_HIDE &&
        event.event != ENTITY_EVENT_UNHIDE &&
        event.event != ENTITY_EVENT_ENABLE_PHYSICS &&
        event.event != ENTITY_EVENT_PHYSICS_CHANGE_STATE)
    {
        return;
    }

    MarkDirty(levelName, entity, &event, event.event == ENTITY_EVENT_DONE);
}

void CoopAreaStateJournal::NoteEntityRemoved(const std::string& levelName, IEntity& entity)
{
    ++m_observedEventCount;
    MarkDirty(levelName, entity, nullptr, true);
}

void CoopAreaStateJournal::NoteLevelStateSave(const std::string& levelName, LevelStatePhase phase)
{
    if (phase == LevelStatePhase::BeforeOriginal)
    {
        ++m_boundarySaveCount;
        ++m_levelStats[levelName].saveBoundaries;
        RefreshLevelDirtyCount(levelName);
    }

    const LevelStats& stats = m_levelStats[levelName];
    std::ostringstream out;
    out
        << (phase == LevelStatePhase::BeforeOriginal ? "before" : "after")
        << " native area save level=" << levelName
        << " dirtyEntities=" << stats.dirtyEntities
        << " dirtyEvents=" << stats.dirtyEvents
        << " totalDirty=" << m_dirtyEntities.size();
    m_lastEvent = out.str();
}

void CoopAreaStateJournal::NoteLevelStateLoad(const std::string& levelName, LevelStatePhase phase)
{
    if (phase == LevelStatePhase::BeforeOriginal)
    {
        ++m_boundaryLoadCount;
        ++m_levelStats[levelName].loadBoundaries;
        RefreshLevelDirtyCount(levelName);
    }

    const LevelStats& stats = m_levelStats[levelName];
    std::ostringstream out;
    out
        << (phase == LevelStatePhase::BeforeOriginal ? "before" : "after")
        << " native area load level=" << levelName
        << " knownDirtyEntities=" << stats.dirtyEntities
        << " dirtyEvents=" << stats.dirtyEvents
        << " totalDirty=" << m_dirtyEntities.size();
    m_lastEvent = out.str();
}

bool CoopAreaStateJournal::ExportJsonl(std::ostream& output, size_t maxEntityRows) const
{
    return ExportJsonlFiltered(
        m_dirtyEntities,
        m_levelStats,
        m_observedEventCount,
        m_trackedEventCount,
        m_guardSkips,
        m_boundarySaveCount,
        m_boundaryLoadCount,
        m_lastEvent,
        nullptr,
        output,
        maxEntityRows);
}

bool CoopAreaStateJournal::ExportLevelJsonl(const std::string& levelName, std::ostream& output, size_t maxEntityRows) const
{
    return ExportJsonlFiltered(
        m_dirtyEntities,
        m_levelStats,
        m_observedEventCount,
        m_trackedEventCount,
        m_guardSkips,
        m_boundarySaveCount,
        m_boundaryLoadCount,
        m_lastEvent,
        &levelName,
        output,
        maxEntityRows);
}

bool CoopAreaStateJournal::TryCaptureEntity(const std::string& levelName, IEntity& entity, DirtyEntity& snapshot, std::string& reason) const
{
    if (!IsLikelyRuntimeCppObject(&entity))
    {
        reason = "invalid entity pointer";
        return false;
    }

    snapshot.levelName = levelName;

    if (!TryGuardedCall("area journal IEntity::GetId", [&entity]() { return entity.GetId(); }, snapshot.entityId, &reason) ||
        !TryGuardedCall("area journal IEntity::GetGuid", [&entity]() { return entity.GetGuid(); }, snapshot.guid, &reason) ||
        !TryGuardedCall("area journal IEntity::GetWorldPos", [&entity]() { return entity.GetWorldPos(); }, snapshot.position, &reason) ||
        !TryGuardedCall("area journal IEntity::GetWorldRotation", [&entity]() { return entity.GetWorldRotation(); }, snapshot.rotation, &reason) ||
        !TryGuardedCall("area journal IEntity::GetScale", [&entity]() { return Vec3(entity.GetScale()); }, snapshot.scale, &reason) ||
        !TryGuardedCall("area journal IEntity::GetFlags", [&entity]() { return entity.GetFlags(); }, snapshot.flags, &reason))
    {
        return false;
    }

    bool hidden = false;
    if (TryGuardedCall("area journal IEntity::IsHidden", [&entity]() { return entity.IsHidden(); }, hidden, &reason))
        snapshot.hidden = hidden;

    const char* entityNameRaw = nullptr;
    if (TryGuardedCall("area journal IEntity::GetName", [&entity]() { return entity.GetName(); }, entityNameRaw, &reason))
        snapshot.entityName = ReadRuntimeCString(entityNameRaw, 128);

    IEntityClass* entityClass = nullptr;
    if (TryGuardedCall("area journal IEntity::GetClass", [&entity]() { return entity.GetClass(); }, entityClass, &reason) &&
        IsLikelyRuntimeCppObject(entityClass))
    {
        const char* classNameRaw = nullptr;
        if (TryGuardedCall("area journal IEntityClass::GetName", [entityClass]() { return entityClass->GetName(); }, classNameRaw, &reason))
            snapshot.className = ReadRuntimeCString(classNameRaw, 128);
    }

    return snapshot.guid != 0;
}

bool CoopAreaStateJournal::ShouldTrackEntity(const DirtyEntity& snapshot) const
{
    if (snapshot.levelName.empty() || snapshot.levelName == "unknown" || snapshot.guid == 0)
        return false;

    const std::string className = ToLowerAscii(snapshot.className);
    const std::string entityName = ToLowerAscii(snapshot.entityName);

    if (className.empty())
        return false;

    if (entityName.rfind("coop_", 0) == 0 || entityName.find("coopprototype") != std::string::npos)
        return false;

    if (className == "player" ||
        className.find("arkplayer") != std::string::npos ||
        className.find("arkhuman") != std::string::npos ||
        className.find("arknpc") != std::string::npos ||
        className.find("arkmimic") != std::string::npos ||
        className.find("arkphantom") != std::string::npos ||
        className.find("arknightmare") != std::string::npos ||
        className.find("arkoperator") != std::string::npos ||
        className.find("arkweaver") != std::string::npos ||
        className.find("arkcystoid") != std::string::npos ||
        className.find("apextentacle") != std::string::npos ||
        className.find("arklight") != std::string::npos ||
        className.find("leveltransition") != std::string::npos ||
        className.find("trigger") != std::string::npos ||
        className.find("volume") != std::string::npos ||
        className.find("flowgraph") != std::string::npos)
    {
        return false;
    }

    return true;
}

void CoopAreaStateJournal::MarkDirty(const std::string& levelName, IEntity& entity, const SEntityEvent* event, bool removed)
{
    EntityId entityId = INVALID_ENTITYID;
    std::string reason;
    TryGuardedCall("area journal cached IEntity::GetId", [&entity]() { return entity.GetId(); }, entityId, &reason);

    if (ArkPlayer::GetInstancePtr())
    {
        IEntity* playerEntity = ArkPlayer::GetInstance().GetEntity();
        if (playerEntity && entityId != INVALID_ENTITYID && playerEntity->GetId() == entityId)
        {
            m_untrackedEntityIds.insert(entityId);
            ++m_guardSkips;
            m_lastEvent = "entity event skipped: local player";
            return;
        }
    }

    if (event && event->event == ENTITY_EVENT_XFORM && entityId != INVALID_ENTITYID && m_untrackedEntityIds.find(entityId) != m_untrackedEntityIds.end())
        return;

    if (event && event->event == ENTITY_EVENT_XFORM && entityId != INVALID_ENTITYID)
    {
        const auto idIt = m_entityIdToGuid.find(entityId);
        if (idIt != m_entityIdToGuid.end())
        {
            const auto dirtyIt = m_dirtyEntities.find(idIt->second);
            if (dirtyIt != m_dirtyEntities.end() &&
                dirtyIt->second.levelName == levelName &&
                !dirtyIt->second.removed)
            {
                DirtyEntity latest;
                if (TryCaptureEntity(levelName, entity, latest, reason) && ShouldTrackEntity(latest))
                {
                    latest.eventMask = dirtyIt->second.eventMask;
                    latest.xformEvents = dirtyIt->second.xformEvents;
                    latest.removeEvents = dirtyIt->second.removeEvents;
                    latest.removed = dirtyIt->second.removed;
                    dirtyIt->second = latest;
                }

                DirtyEntity& dirty = dirtyIt->second;
                dirty.eventMask |= EventBit(event->event);
                ++dirty.xformEvents;
                ++m_trackedEventCount;

                LevelStats& stats = m_levelStats[levelName];
                ++stats.dirtyEvents;

                if ((dirty.xformEvents % 256u) == 0)
                {
                    std::ostringstream out;
                    out
                        << "dirty entity cached level=" << levelName
                        << " guid=" << dirty.guid
                        << " id=" << dirty.entityId
                        << " xforms=" << dirty.xformEvents;
                    m_lastEvent = out.str();
                }
                return;
            }
        }
    }

    DirtyEntity snapshot;
    if (!TryCaptureEntity(levelName, entity, snapshot, reason) || !ShouldTrackEntity(snapshot))
    {
        if (entityId != INVALID_ENTITYID)
            m_untrackedEntityIds.insert(entityId);

        ++m_guardSkips;
        m_lastEvent = "entity event skipped: " + (reason.empty() ? std::string("not trackable") : reason);
        return;
    }

    DirtyEntity& dirty = m_dirtyEntities[snapshot.guid];
    const uint32_t previousEventMask = dirty.eventMask;
    const uint32_t previousXformEvents = dirty.xformEvents;
    const uint32_t previousRemoveEvents = dirty.removeEvents;

    dirty = snapshot;
    dirty.eventMask = previousEventMask;
    dirty.xformEvents = previousXformEvents;
    dirty.removeEvents = previousRemoveEvents;

    if (event)
    {
        dirty.eventMask |= EventBit(event->event);
        if (event->event == ENTITY_EVENT_XFORM)
            ++dirty.xformEvents;
    }

    if (removed)
    {
        dirty.removed = true;
        ++dirty.removeEvents;
        dirty.eventMask |= EventBit(ENTITY_EVENT_DONE);
    }

    if (snapshot.entityId != INVALID_ENTITYID)
        m_entityIdToGuid[snapshot.entityId] = snapshot.guid;

    ++m_trackedEventCount;
    LevelStats& stats = m_levelStats[levelName];
    ++stats.dirtyEvents;
    RefreshLevelDirtyCount(levelName);

    std::ostringstream out;
    out
        << "dirty entity level=" << levelName
        << " guid=" << snapshot.guid
        << " id=" << snapshot.entityId
        << " class=" << (snapshot.className.empty() ? "-" : snapshot.className)
        << " removed=" << (dirty.removed ? 1 : 0);
    m_lastEvent = out.str();
}

void CoopAreaStateJournal::RefreshLevelDirtyCount(const std::string& levelName)
{
    uint32_t count = 0;
    for (const auto& [guid, dirty] : m_dirtyEntities)
    {
        (void)guid;
        if (dirty.levelName == levelName)
            ++count;
    }

    m_levelStats[levelName].dirtyEntities = count;
}
