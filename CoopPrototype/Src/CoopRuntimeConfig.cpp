#include "CoopRuntimeConfig.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace
{
constexpr std::size_t kGlobalSlots = 1024;
constexpr std::size_t kLocalSlots = 128;
constexpr uint64_t kHashMask = (uint64_t{1} << 62) - 1;
constexpr uint64_t kStateShift = 62;

enum class FlagState : uint64_t
{
    Missing = 1,
    Disabled = 2,
    Enabled = 3,
};

struct LocalEntry
{
    const char* name = nullptr;
    FlagState state = FlagState::Missing;
};

std::array<std::atomic<uint64_t>, kGlobalSlots> g_flagCache{};
thread_local std::array<LocalEntry, kLocalSlots> g_localFlagCache{};

uint64_t HashName(const char* name)
{
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char* cursor = reinterpret_cast<const unsigned char*>(name); *cursor; ++cursor)
    {
        hash ^= *cursor;
        hash *= 1099511628211ull;
    }
    hash &= kHashMask;
    return hash == 0 ? 1 : hash;
}

FlagState ReadEnvironment(const char* name)
{
    const char* value = std::getenv(name);
    if (!value || !value[0])
        return FlagState::Missing;
    return std::atoi(value) != 0 ? FlagState::Enabled : FlagState::Disabled;
}

FlagState ResolveState(const char* name)
{
    if (!name || !name[0])
        return FlagState::Missing;

    const std::uintptr_t pointer = reinterpret_cast<std::uintptr_t>(name);
    LocalEntry& local = g_localFlagCache[(pointer >> 3) % kLocalSlots];
    if (local.name == name)
        return local.state;

    const uint64_t hash = HashName(name);
    const std::size_t first = static_cast<std::size_t>(hash % kGlobalSlots);
    for (std::size_t probe = 0; probe < kGlobalSlots; ++probe)
    {
        std::atomic<uint64_t>& slot = g_flagCache[(first + probe) % kGlobalSlots];
        uint64_t token = slot.load(std::memory_order_acquire);
        if ((token & kHashMask) == hash && token != 0)
        {
            local = {name, static_cast<FlagState>(token >> kStateShift)};
            return local.state;
        }
        if (token != 0)
            continue;

        const FlagState state = ReadEnvironment(name);
        const uint64_t candidate = hash | (static_cast<uint64_t>(state) << kStateShift);
        if (slot.compare_exchange_strong(token, candidate, std::memory_order_release, std::memory_order_acquire) ||
            ((token & kHashMask) == hash && token != 0))
        {
            local = {name, token == 0 ? state : static_cast<FlagState>(token >> kStateShift)};
            return local.state;
        }
    }

    return ReadEnvironment(name);
}
}

bool CoopRuntimeConfig::Flag(const char* name)
{
    return ResolveState(name) == FlagState::Enabled;
}

bool CoopRuntimeConfig::FlagDefaultEnabled(const char* name)
{
    return ResolveState(name) != FlagState::Disabled;
}

bool CoopRuntimeConfig::UnsafeFlag(const char* name)
{
    return Flag("COOP_ALLOW_UNSAFE_EXPERIMENTS") && Flag(name);
}
