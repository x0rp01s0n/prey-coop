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
// (including VirtualQuery preflight reads) and the number of VirtualQuery
// calls made by the guard layer. Windows-only cost centers — see RESULTS.md.
struct GuardTelemetryTotals
{
    uint64_t guardedCallTotalNs = 0;
    uint64_t virtualQueryCalls = 0;
};
GuardTelemetryTotals GetGuardTelemetryTotals();
bool IsGuardedCallbackActive();

using RuntimeGuardCallback = bool (*)(void* context);
bool TryRunGuardedCallback(
    const char* operation,
    RuntimeGuardCallback callback,
    void* context,
    std::string* outReason = nullptr);

template <typename T>
bool TryReadRuntimeValue(const T* pointer, T& outValue)
{
    if (!PreflightRuntimePointer("read runtime value", pointer, sizeof(T), RuntimeAccess::Read))
        return false;

    std::memcpy(&outValue, pointer, sizeof(T));
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
