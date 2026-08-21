#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
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

// Lock-free bounded ring of the most recent raw log lines, for crash traces.
//
// The crash / purecall / invalid-parameter handlers may run on a faulting
// thread where taking the rate-limit mutex could deadlock, so this ring uses
// no locks and no write-path allocations: fixed-size slots with atomic
// sequence + length. Every line that reaches WriteRaw is recorded (long
// lines are truncated to fit the slot).
//
// Returns up to maxLines of the most recent lines, newest last, each line
// terminated by '\n'. Empty string if nothing has been recorded. Safe to
// call from a SEH/vectored exception handler (atomic loads + memcpy only;
// the returned std::string allocation is the only allocation and happens on
// the handler thread, exactly like the existing trace-file path).
std::string RecentLines(std::size_t maxLines);
}
