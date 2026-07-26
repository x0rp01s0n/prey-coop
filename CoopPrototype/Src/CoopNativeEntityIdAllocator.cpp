#include "CoopNativeEntityIdAllocator.h"

#include "CoopRuntimeConfig.h"
#include "CoopRuntimeGuards.h"

#include <Prey/CryEntitySystem/IEntitySystem.h>
#include <Prey/CrySystem/ISystem.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace
{
using CoopRuntimeGuards::TryGuardedCall;

std::string StatusToken(std::string value)
{
    if (value.empty())
        return "-";

    for (char& ch : value)
    {
        if (ch <= ' ' || ch == '"' || ch == '\'' || ch == '\\')
            ch = '_';
    }
    return value;
}

bool EnvFlagEnabled(const char* name)
{
    return CoopRuntimeConfig::Flag(name);
}

bool ContainsId(const std::unordered_set<uint32_t>& ids, uint32_t id)
{
    return id != 0 && ids.find(id) != ids.end();
}

uint32_t MaxId(const std::unordered_set<uint32_t>& ids)
{
    uint32_t maxId = 0;
    for (uint32_t id : ids)
        maxId = std::max(maxId, id);
    return maxId;
}

bool IsRuntimeIdUsed(uint32_t id, uint32_t& guards)
{
    if (id == 0 || !gEnv || !gEnv->pEntitySystem)
        return true;

    bool used = true;
    std::string reason;
    const bool ok = TryGuardedCall(
        "native entity id allocator IsIDUsed",
        [id]() { return gEnv->pEntitySystem->IsIDUsed(id); },
        used,
        &reason);
    if (!ok)
    {
        ++guards;
        return true;
    }
    return used;
}
}

namespace CoopNativeEntityIdAllocator
{
bool IsNativeEntityAllocatorAvailable(uint32_t* outGuards)
{
    uint32_t guards = 0;
    if (!gEnv || !gEnv->pEntitySystem)
    {
        if (outGuards)
            *outGuards = 1;
        return false;
    }

    bool used = false;
    std::string reason;
    const bool ok = TryGuardedCall(
        "native entity id allocator availability IsIDUsed",
        []() { return gEnv->pEntitySystem->IsIDUsed(0); },
        used,
        &reason);
    if (!ok)
        ++guards;

    if (outGuards)
        *outGuards = guards;
    return ok;
}

AllocationPlan BuildDryRunPlan(
    const std::vector<AllocationRequest>& requests,
    const std::vector<uint32_t>& forbiddenIds)
{
    AllocationPlan plan;
    plan.attempted = true;
    plan.requested = static_cast<uint32_t>(requests.size());

    uint32_t availabilityGuards = 0;
    plan.nativeAvailable = IsNativeEntityAllocatorAvailable(&availabilityGuards);
    plan.guards += availabilityGuards;
    const bool validateRuntimeIds = EnvFlagEnabled("COOP_NATIVE_ENTITY_ID_ALLOCATOR_VALIDATE_RUNTIME");

    std::unordered_set<uint32_t> forbidden;
    forbidden.reserve(forbiddenIds.size() + requests.size() + 16);
    for (uint32_t id : forbiddenIds)
    {
        if (id != 0)
            forbidden.insert(id);
    }
    for (const AllocationRequest& request : requests)
    {
        if (request.sourceEntityId != 0)
            forbidden.insert(request.sourceEntityId);
    }
    plan.forbiddenIds = static_cast<uint32_t>(forbidden.size());

    if (!plan.nativeAvailable && !EnvFlagEnabled("COOP_NATIVE_ENTITY_ID_ALLOCATOR_ALLOW_DRY_WITHOUT_NATIVE"))
    {
        plan.reason = "native_entity_allocator_unavailable";
        return plan;
    }

    uint32_t next = MaxId(forbidden);
    if (next < 0x10000u)
        next = 0x10000u;
    if (next < std::numeric_limits<uint32_t>::max())
        ++next;
    plan.firstCandidate = next;

    plan.entries.reserve(requests.size());
    for (const AllocationRequest& request : requests)
    {
        if (request.sourceEntityId == 0)
        {
            plan.reason = "source_entity_id_zero";
            return plan;
        }

        uint32_t guardBudget = 0;
        for (;;)
        {
            if (++guardBudget > 1000000u)
            {
                plan.reason = "entity_id_search_guard";
                return plan;
            }

            if (next == 0 || next == std::numeric_limits<uint32_t>::max())
            {
                plan.reason = "entity_id_space_exhausted";
                return plan;
            }

            if (ContainsId(forbidden, next))
            {
                ++plan.collisionSkips;
                ++next;
                continue;
            }

            if (validateRuntimeIds && plan.nativeAvailable && IsRuntimeIdUsed(next, plan.guards))
            {
                ++plan.runtimeUsedSkips;
                ++next;
                continue;
            }

            AllocationEntry entry;
            entry.sourceOrdinal = request.sourceOrdinal;
            entry.sourceEntityId = request.sourceEntityId;
            entry.targetEntityId = next;
            plan.entries.push_back(entry);
            forbidden.insert(next);
            ++next;
            break;
        }
    }

    plan.planned = static_cast<uint32_t>(plan.entries.size());
    plan.nextCandidate = next;
    plan.ok = plan.planned == plan.requested && plan.guards == 0;
    if (plan.reason.empty())
        plan.reason = plan.ok ? "ok" : "planned_with_guards";
    return plan;
}

std::string BuildPlanStatus(const AllocationPlan& plan)
{
    std::ostringstream out;
    out << (plan.attempted ? 1 : 0)
        << "/" << (plan.ok ? "ok" : "blocked")
        << "/reason=" << StatusToken(plan.reason)
        << "/native=" << (plan.nativeAvailable ? 1 : 0)
        << "/req=" << plan.requested
        << "/planned=" << plan.planned
        << "/forbid=" << plan.forbiddenIds
        << "/runtimeSkip=" << plan.runtimeUsedSkips
        << "/collideSkip=" << plan.collisionSkips
        << "/first=" << plan.firstCandidate
        << "/next=" << plan.nextCandidate
        << "/guards=" << plan.guards;

    if (!plan.entries.empty())
    {
        out << "/map=";
        const size_t limit = std::min<size_t>(plan.entries.size(), 6);
        for (size_t i = 0; i < limit; ++i)
        {
            const AllocationEntry& entry = plan.entries[i];
            if (i != 0)
                out << ",";
            out << entry.sourceOrdinal
                << ":" << entry.sourceEntityId
                << ">" << entry.targetEntityId;
        }
        if (plan.entries.size() > limit)
            out << ",more" << (plan.entries.size() - limit);
    }
    return out.str();
}
}
