#include "CoopCoverageDiscovery.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <vector>

#include <Prey/CryEntitySystem/IEntity.h>

namespace
{
template<size_t N>
bool StartsWithAny(std::string_view value, const std::array<std::string_view, N>& prefixes)
{
    return std::any_of(prefixes.begin(), prefixes.end(), [value](std::string_view prefix)
    {
        return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
    });
}
}

void CoopCoverageDiscovery::Reset()
{
    *this = CoopCoverageDiscovery{};
}

std::string CoopCoverageDiscovery::Normalize(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (unsigned char ch : value)
    {
        if (std::isalnum(ch))
            result.push_back(static_cast<char>(std::tolower(ch)));
    }
    return result;
}

std::string CoopCoverageDiscovery::BoundedDetail(std::string_view value)
{
    constexpr size_t kMaxDetail = 96;
    std::string result(value.substr(0, kMaxDetail));
    for (char& ch : result)
    {
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == ',' || ch == '=')
            ch = '_';
    }
    return result.empty() ? std::string("-") : result;
}

bool CoopCoverageDiscovery::IsClassCoveredByKnownPolicy(std::string_view rawClassName)
{
    const std::string name = Normalize(rawClassName);
    static constexpr std::array<std::string_view, 67> kExact = {
        "animentity", "animobject", "arkaliengibletmedium", "arkaliengibletmimicclimb", "arkaliengibletphantomlimb",
        "arkaliengibletsmall", "arkalienjelly", "arkapextentacle", "arkbeta",
        "arkbreakable", "arkcargocontainer", "arkcinematiccharacter", "arkcontainer", "arkcystoidnest",
        "arkdoor", "arkeel", "arketherduplicate", "arkexplosivetank", "arkfabricator",
        "arkgenericelevatorkiosk", "arkglintingrigidbodyex", "arkharvestable", "arkhuman", "arkinteractiveobject",
        "arkkeycardreader", "arkkeypad", "arkkiosk", "arkleakable", "arklight", "arkmedkit", "arkmimic",
        "arkmimicelite", "arknightmare", "arkoperatordispenser", "arkoperatorengineer", "arkoperatormedic",
        "arkoperatormilitary", "arkoperatorscience", "arkpages", "arkphantomvoltaic", "arkplayer", "arkpoltergeist", "arkprojectilegoo",
        "arkprojectilegooball", "arkprojectilerecyclergrenade", "arkrotator", "arkscalablebreakable",
        "arkneuromod", "arkpaspeaker", "arksuitpatch", "arktechnopath", "arktelepath", "arktrackingchip", "arktranscribe",
        "arkturret", "arkweaver", "arkworkstationscreen", "arkworldui", "basicentity", "destroyablelight",
        "environmentlight", "geomentity", "light", "particleeffect", "arkroomportal", "rigidbodyex"
    };
    static constexpr std::array<std::string_view, 15> kPrefixes = {
        "arkalcohol", "arkammopickup", "arkbook", "arkconsumable", "arkcraftingingredient", "arkcure",
        "arkequipmentmod", "arkfabricationplan", "arkfood", "arkitem", "arkkeycard", "arkprojectile",
        "arkpsychoscope", "arkrecyclerjunk", "arkweapon"
    };
    return std::find(kExact.begin(), kExact.end(), name) != kExact.end() || StartsWithAny(name, kPrefixes);
}

void CoopCoverageDiscovery::RecordEntitySpawn(uint32_t entityId, std::string_view className)
{
    ++m_entitySpawns;
    if (IsClassCoveredByKnownPolicy(className))
    {
        ++m_classifiedEntitySpawns;
        return;
    }

    ++m_unclassifiedEntitySpawns;
    const std::string normalized = Normalize(className);
    const std::string key = normalized.empty() ? std::string("unknown") : normalized;
    m_lastUnclassifiedClass = BoundedDetail(className);

    auto it = m_unclassifiedClasses.find(key);
    if (it != m_unclassifiedClasses.end())
    {
        ++it->second.spawns;
    }
    else if (m_unclassifiedClasses.size() < kMaxUnclassifiedClasses)
    {
        m_unclassifiedClasses.emplace(key, ClassObservation{1, 0});
    }

    if (entityId != 0)
    {
        if (m_unclassifiedEntities.size() < kMaxTrackedEntities)
            m_unclassifiedEntities[entityId] = key;
        else
            ++m_droppedEntityTracking;
    }
}

void CoopCoverageDiscovery::RecordEntityRemoved(uint32_t entityId)
{
    m_unclassifiedEntities.erase(entityId);
}

void CoopCoverageDiscovery::RecordEntityEvent(uint32_t entityId, int eventType)
{
    const auto entityIt = m_unclassifiedEntities.find(entityId);
    if (entityIt == m_unclassifiedEntities.end())
        return;

    // Sleep/wake, collision and transform events are runtime noise. Only edges
    // which can change save-visible existence/visibility/activation are counted.
    if (eventType != ENTITY_EVENT_DONE &&
        eventType != ENTITY_EVENT_HIDE &&
        eventType != ENTITY_EVENT_UNHIDE &&
        eventType != ENTITY_EVENT_ENABLE_PHYSICS)
    {
        return;
    }

    ++m_unclassifiedPersistentEvents;
    const auto classIt = m_unclassifiedClasses.find(entityIt->second);
    if (classIt != m_unclassifiedClasses.end())
        ++classIt->second.persistentEvents;
}

void CoopCoverageDiscovery::RecordDamageSource(bool stableIdentity, std::string_view detail)
{
    ++m_damageSources;
    if (!stableIdentity)
    {
        ++m_unstableDamageSources;
        m_lastUnstableDamage = BoundedDetail(detail);
    }
}

void CoopCoverageDiscovery::RecordProjectile(bool classified, std::string_view detail)
{
    ++m_projectiles;
    if (!classified)
    {
        ++m_unclassifiedProjectiles;
        m_lastUnclassifiedProjectile = BoundedDetail(detail);
    }
}

void CoopCoverageDiscovery::RecordNpcAbility(bool classified, std::string_view detail)
{
    ++m_npcAbilities;
    if (!classified)
    {
        ++m_unclassifiedNpcAbilities;
        m_lastUnclassifiedNpcAbility = BoundedDetail(detail);
    }
}

void CoopCoverageDiscovery::RecordStoryCommit(bool classified, std::string_view detail)
{
    ++m_storyCommits;
    if (!classified)
    {
        ++m_unclassifiedStoryCommits;
        m_lastUnclassifiedStoryCommit = BoundedDetail(detail);
    }
}

std::string CoopCoverageDiscovery::BuildCompactReport() const
{
    std::vector<std::pair<std::string, ClassObservation>> classes(m_unclassifiedClasses.begin(), m_unclassifiedClasses.end());
    std::sort(classes.begin(), classes.end(), [](const auto& left, const auto& right)
    {
        if (left.second.persistentEvents != right.second.persistentEvents)
            return left.second.persistentEvents > right.second.persistentEvents;
        if (left.second.spawns != right.second.spawns)
            return left.second.spawns > right.second.spawns;
        return left.first < right.first;
    });

    std::ostringstream out;
    out << "spawns=" << m_entitySpawns
        << "/" << m_classifiedEntitySpawns
        << "/" << m_unclassifiedEntitySpawns
        << ",unknownClasses=" << m_unclassifiedClasses.size()
        << ",trackedUnknown=" << m_unclassifiedEntities.size()
        << ",trackDrops=" << m_droppedEntityTracking
        << ",persistentUnknown=" << m_unclassifiedPersistentEvents
        << ",damage=" << m_damageSources << "/" << m_unstableDamageSources
        << ",projectile=" << m_projectiles << "/" << m_unclassifiedProjectiles
        << ",ability=" << m_npcAbilities << "/" << m_unclassifiedNpcAbilities
        << ",story=" << m_storyCommits << "/" << m_unclassifiedStoryCommits
        << ",lastClass=" << m_lastUnclassifiedClass
        << ",lastDamage=" << m_lastUnstableDamage
        << ",lastProjectile=" << m_lastUnclassifiedProjectile
        << ",lastAbility=" << m_lastUnclassifiedNpcAbility
        << ",lastStory=" << m_lastUnclassifiedStoryCommit;
    for (size_t index = 0; index < std::min<size_t>(classes.size(), 6); ++index)
    {
        out << ",u" << index << "=" << classes[index].first
            << ":" << classes[index].second.spawns
            << ":" << classes[index].second.persistentEvents;
    }
    return out.str();
}

std::string CoopCoverageDiscovery::BuildDetailedReport() const
{
    std::vector<std::pair<std::string, ClassObservation>> classes(m_unclassifiedClasses.begin(), m_unclassifiedClasses.end());
    std::sort(classes.begin(), classes.end(), [](const auto& left, const auto& right)
    {
        if (left.second.persistentEvents != right.second.persistentEvents)
            return left.second.persistentEvents > right.second.persistentEvents;
        if (left.second.spawns != right.second.spawns)
            return left.second.spawns > right.second.spawns;
        return left.first < right.first;
    });

    std::ostringstream out;
    out << BuildCompactReport() << ",allUnknown=";
    if (classes.empty())
        out << '-';
    for (size_t index = 0; index < classes.size(); ++index)
    {
        if (index != 0)
            out << ';';
        out << classes[index].first
            << ':' << classes[index].second.spawns
            << ':' << classes[index].second.persistentEvents;
    }
    return out.str();
}
