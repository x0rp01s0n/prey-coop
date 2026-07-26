#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace CoopNativeEntityIdAllocator
{
struct AllocationRequest
{
    uint32_t sourceOrdinal = 0xFFFFFFFFu;
    uint32_t sourceEntityId = 0;
};

struct AllocationEntry
{
    uint32_t sourceOrdinal = 0xFFFFFFFFu;
    uint32_t sourceEntityId = 0;
    uint32_t targetEntityId = 0;
};

struct AllocationPlan
{
    bool attempted = false;
    bool ok = false;
    bool nativeAvailable = false;
    std::string reason;
    uint32_t requested = 0;
    uint32_t planned = 0;
    uint32_t forbiddenIds = 0;
    uint32_t runtimeUsedSkips = 0;
    uint32_t collisionSkips = 0;
    uint32_t guards = 0;
    uint32_t firstCandidate = 0;
    uint32_t nextCandidate = 0;
    std::vector<AllocationEntry> entries;
};

bool IsNativeEntityAllocatorAvailable(uint32_t* outGuards = nullptr);

AllocationPlan BuildDryRunPlan(
    const std::vector<AllocationRequest>& requests,
    const std::vector<uint32_t>& forbiddenIds);

std::string BuildPlanStatus(const AllocationPlan& plan);
}
