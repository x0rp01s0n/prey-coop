#pragma once

#include <cstdint>
#include <string_view>

namespace CoopRuntimeLog
{
void Write(std::string_view message);
bool WriteRateLimited(
    std::string_view key,
    std::string_view message,
    double intervalSeconds = 1.0,
    uint32_t burst = 1);
uint64_t SuppressedCount();
void ResetRateLimits();
}
