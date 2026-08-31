#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>

namespace CoopRuntimeGuards
{
enum class RuntimeAccess
{
    Read,
    Write,
    Execute,
};

struct RuntimeGuardSnapshot
{
    uint64_t preflightFailures = 0;
    uint64_t sehExceptions = 0;
    uint64_t guardedCalls = 0;
    uint64_t guardedCallFailures = 0;
    std::uintptr_t lastPointer = 0;
    std::uintptr_t lastExceptionAddress = 0;
    uint32_t lastExceptionCode = 0;
    size_t lastBytes = 0;
    std::string lastOperation;
    std::string lastReason;
};

bool IsReadableRuntimePointer(const void* pointer, size_t minBytes = sizeof(void*));
bool IsWritableRuntimePointer(const void* pointer, size_t minBytes = sizeof(void*));
bool IsExecutableRuntimePointer(const void* pointer, size_t minBytes = 1);
bool IsRuntimePointerMappedInCurrentProcess(const void* pointer, size_t minBytes = sizeof(void*));
bool IsRuntimePointerInLoadedModule(const void* pointer);
bool IsLikelyRuntimeCppObject(const void* object, size_t minBytes = sizeof(void*));
bool PreflightRuntimePointer(
    const char* operation,
    const void* pointer,
    size_t minBytes,
    RuntimeAccess access,
    std::string* outReason = nullptr);
std::string GetRuntimeModulePath(const void* pointer);
std::string ReadRuntimeCString(const char* pointer, size_t maxLength);
bool RuntimeCStringEquals(const char* pointer, const char* expected, size_t maxLength);
RuntimeGuardSnapshot GetRuntimeGuardSnapshot();

// Self-profiling totals (lock-free): cumulative time inside guarded callbacks
// (including VirtualQuery preflight reads) and the number of actual VirtualQuery
// API calls made by the guard layer. Windows-only cost centers — see RESULTS.md.
struct GuardTelemetryTotals
{
    uint64_t guardedCallTotalNs = 0;
    uint64_t virtualQueryCalls = 0;
    uint64_t sehExceptions = 0;
    uint64_t guardedCallFailures = 0;
    uint64_t preflightFailures = 0;
};
GuardTelemetryTotals GetGuardTelemetryTotals();

// Top guarded operations by cumulative time: "name:calls:us|name:calls:us..."
// (up to 5 entries; empty when nothing recorded). Window deltas are computed
// by the caller between snapshots.
std::string GetGuardOpReport();
// Raw/untrusted-output read API. The destination is validated with VQ before
// a guarded commit, but concurrent protection/lifetime changes can still make
// that commit fail after writing some bytes; it is not atomic. Source reads
// use direct SEH by default; COOP_RUNTIME_READ_MODE=vq restores the old
// preflight path.
bool TryReadRuntimeValueSeh(const void* pointer, void* outValue, size_t size);
bool TryRuntimeReadProbeSeh(const void* pointer, size_t size);
const char* RuntimeReadModeName();
const char* RuntimeReadEffectiveModeName();
const char* RuntimeObjectProbeModeName();
const char* RuntimeObjectProbeEffectiveModeName();
const char* RuntimeGuardPagePolicyName();

struct RuntimeReadTelemetry
{
    uint64_t attempts = 0;
    uint64_t failures = 0;
    uint64_t accessViolationFailures = 0;
    uint64_t inPageErrorFailures = 0;
    uint64_t guardPageFailures = 0;
    uint64_t otherFailures = 0;
    // PAGE_GUARD re-arm counters; allowConsume intentionally reports zero.
    uint64_t guardPageRestoreAttempts = 0;
    uint64_t guardPageRestoreSuccesses = 0;
    uint64_t guardPageRestoreFailures = 0;
    uint64_t guardPageRestoreAlreadyPresent = 0;
    uint32_t lastExceptionCode = 0;
};
RuntimeReadTelemetry GetRuntimeReadTelemetry();

struct RuntimeObjectProbeTelemetry
{
    uint64_t attempts = 0;
    uint64_t failures = 0;
};
RuntimeObjectProbeTelemetry GetRuntimeObjectProbeTelemetry();

uint64_t GetGuardVqCalls();
uint64_t GetGuardVqTotalNs();

// Wall-clock nanoseconds for experiment instrumentation (steady_clock).
uint64_t GuardTelemetryNowNsPublic();
bool IsGuardedCallbackActive();

namespace detail
{
// Trusted destination used only by the typed T& wrapper above. The caller
// must provide a valid writable object; unlike the raw API this intentionally
// does not probe the destination.
bool TryReadRuntimeValueTrustedSeh(const void* pointer, void* outValue, size_t size);
}

using RuntimeGuardCallback = bool (*)(void* context);
bool TryRunGuardedCallback(
    const char* operation,
    RuntimeGuardCallback callback,
    void* context,
    std::string* outReason = nullptr);

template <typename T>
bool TryReadRuntimeValue(const T* pointer, T& outValue)
{
    // T& is a trusted, live destination owned by the caller. Read into a
    // local value first so a source fault cannot modify the caller's output.
    // The direct path writes only this local under SEH and commits after a
    // complete read; the explicit VQ mode performs its source preflight.
    T staged{};
    if (!detail::TryReadRuntimeValueTrustedSeh(pointer, &staged, sizeof(T)))
        return false;
    std::memcpy(&outValue, &staged, sizeof(T));
    return true;
}

template <typename T>
bool TryWriteRuntimeValue(T* pointer, const T& value)
{
    if (!PreflightRuntimePointer("write runtime value", pointer, sizeof(T), RuntimeAccess::Write))
        return false;

    std::memcpy(pointer, &value, sizeof(T));
    return true;
}

template <typename Func>
bool TryGuardedVoidCall(const char* operation, Func&& func, std::string* outReason = nullptr)
{
    using StoredFunc = std::decay_t<Func>;
    StoredFunc storedFunc(std::forward<Func>(func));
    struct Context
    {
        StoredFunc* func = nullptr;
    } context { &storedFunc };

    return TryRunGuardedCallback(
        operation,
        [](void* rawContext) -> bool
        {
            Context* ctx = static_cast<Context*>(rawContext);
            (*ctx->func)();
            return true;
        },
        &context,
        outReason);
}

template <typename T, typename Func>
bool TryGuardedCall(const char* operation, Func&& func, T& outValue, std::string* outReason = nullptr)
{
    using StoredFunc = std::decay_t<Func>;
    StoredFunc storedFunc(std::forward<Func>(func));
    struct Context
    {
        StoredFunc* func = nullptr;
        T* out = nullptr;
    } context { &storedFunc, &outValue };

    return TryRunGuardedCallback(
        operation,
        [](void* rawContext) -> bool
        {
            Context* ctx = static_cast<Context*>(rawContext);
            *ctx->out = (*ctx->func)();
            return true;
        },
        &context,
        outReason);
}
}
