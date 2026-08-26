#include "CoopRuntimeGuards.h"

#include <winsock2.h>
#include <windows.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <mutex>

namespace CoopRuntimeGuards
{
namespace
{
struct RuntimeMemoryQuery
{
    std::uintptr_t value = 0;
    std::uintptr_t regionStart = 0;
    std::uintptr_t regionEnd = 0;
    DWORD protection = 0;
    bool committed = false;
    bool guard = false;
    bool noAccess = false;
    bool readable = false;
    bool writable = false;
    bool executable = false;
};

struct RuntimeGuardState
{
    std::mutex mutex;
    RuntimeGuardSnapshot snapshot;
};

thread_local int g_guardedCallbackDepth = 0;

struct SehCapture
{
    unsigned code = 0;
    void* address = nullptr;
};

RuntimeGuardState& GetGuardState()
{
    static RuntimeGuardState state;
    return state;
}

// Self-profiling: total guarded-call time and VirtualQuery calls. These are
// the Windows-only hot-path costs (under Wine/Proton VirtualQuery is a
// user-space walk; on native Windows it is a kernel syscall per call), so we
// track them to quantify platform-specific overhead from telemetry alone.
std::atomic<uint64_t> g_guardedCallTotalNs{0};
std::atomic<uint64_t> g_virtualQueryCalls{0};

uint64_t GuardTelemetryNowNs()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

const char* AccessName(RuntimeAccess access)
{
    switch (access)
    {
    case RuntimeAccess::Read:
        return "read";
    case RuntimeAccess::Write:
        return "write";
    case RuntimeAccess::Execute:
        return "execute";
    default:
        return "unknown";
    }
}

std::string PointerString(const void* pointer)
{
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(pointer)));
    return buffer;
}

std::string ExceptionCodeString(unsigned code)
{
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08X", code);
    return buffer;
}

bool QueryRuntimeMemory(const void* pointer, size_t minBytes, RuntimeMemoryQuery& outQuery, std::string* outReason)
{
    const auto value = reinterpret_cast<std::uintptr_t>(pointer);
    outQuery = {};
    outQuery.value = value;
    g_virtualQueryCalls.fetch_add(1, std::memory_order_relaxed);

    if (value < 0x10000 || value == ~std::uintptr_t{ 0 })
    {
        if (outReason)
            *outReason = "address is null/sentinel/small";
        return false;
    }

    MEMORY_BASIC_INFORMATION memoryInfo = {};
    if (VirtualQuery(pointer, &memoryInfo, sizeof(memoryInfo)) == 0)
    {
        if (outReason)
            *outReason = "VirtualQuery failed; address is not mapped in current process";
        return false;
    }

    outQuery.committed = memoryInfo.State == MEM_COMMIT;
    outQuery.guard = (memoryInfo.Protect & PAGE_GUARD) != 0;
    outQuery.noAccess = (memoryInfo.Protect & PAGE_NOACCESS) != 0;
    outQuery.protection = memoryInfo.Protect & 0xff;
    outQuery.regionStart = reinterpret_cast<std::uintptr_t>(memoryInfo.BaseAddress);
    outQuery.regionEnd = outQuery.regionStart + memoryInfo.RegionSize;

    if (!outQuery.committed)
    {
        if (outReason)
            *outReason = "page is not committed";
        return false;
    }

    if (outQuery.guard || outQuery.noAccess)
    {
        if (outReason)
            *outReason = outQuery.guard ? "page is guarded" : "page is no-access";
        return false;
    }

    if (value < outQuery.regionStart || value >= outQuery.regionEnd)
    {
        if (outReason)
            *outReason = "address is outside reported region";
        return false;
    }

    if (minBytes > outQuery.regionEnd - value)
    {
        if (outReason)
            *outReason = "requested span crosses memory region boundary";
        return false;
    }

    const DWORD protection = outQuery.protection;
    outQuery.readable =
        protection == PAGE_READONLY ||
        protection == PAGE_READWRITE ||
        protection == PAGE_WRITECOPY ||
        protection == PAGE_EXECUTE_READ ||
        protection == PAGE_EXECUTE_READWRITE ||
        protection == PAGE_EXECUTE_WRITECOPY;
    outQuery.writable =
        protection == PAGE_READWRITE ||
        protection == PAGE_WRITECOPY ||
        protection == PAGE_EXECUTE_READWRITE ||
        protection == PAGE_EXECUTE_WRITECOPY;
    outQuery.executable =
        protection == PAGE_EXECUTE ||
        protection == PAGE_EXECUTE_READ ||
        protection == PAGE_EXECUTE_READWRITE ||
        protection == PAGE_EXECUTE_WRITECOPY;

    return true;
}

void RecordGuardPreflightFailure(
    const char* operation,
    const void* pointer,
    size_t bytes,
    const std::string& reason)
{
    RuntimeGuardState& state = GetGuardState();
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.snapshot.preflightFailures;
    ++state.snapshot.guardedCallFailures;
    state.snapshot.lastOperation = operation && operation[0] ? operation : "-";
    state.snapshot.lastReason = reason;
    state.snapshot.lastPointer = reinterpret_cast<std::uintptr_t>(pointer);
    state.snapshot.lastBytes = bytes;
    state.snapshot.lastExceptionCode = 0;
    state.snapshot.lastExceptionAddress = 0;
}

void RecordGuardedCall(bool success, const char* operation, const std::string& reason, const SehCapture* seh)
{
    RuntimeGuardState& state = GetGuardState();
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.snapshot.guardedCalls;
    if (!success)
    {
        ++state.snapshot.guardedCallFailures;
        if (seh && seh->code != 0)
            ++state.snapshot.sehExceptions;
        state.snapshot.lastOperation = operation && operation[0] ? operation : "-";
        state.snapshot.lastReason = reason;
        state.snapshot.lastExceptionCode = seh ? seh->code : 0;
        state.snapshot.lastExceptionAddress = seh ? reinterpret_cast<std::uintptr_t>(seh->address) : 0;
    }
}

int RuntimeGuardExceptionFilter(unsigned code, EXCEPTION_POINTERS* pointers, SehCapture* outCapture)
{
    if (outCapture)
    {
        outCapture->code = code;
        outCapture->address = pointers && pointers->ExceptionRecord ? pointers->ExceptionRecord->ExceptionAddress : nullptr;
    }

    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
        return EXCEPTION_EXECUTE_HANDLER;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

bool TryRunGuardedCallbackRaw(RuntimeGuardCallback callback, void* context, SehCapture* outCapture)
{
    if (!callback)
        return false;

    const uint64_t startNs = GuardTelemetryNowNs();
    ++g_guardedCallbackDepth;
    bool result = false;
#if defined(_MSC_VER) || defined(__clang__)
    __try
    {
        result = callback(context);
    }
    __except (RuntimeGuardExceptionFilter(GetExceptionCode(), GetExceptionInformation(), outCapture))
    {
        result = false;
    }
#else
    result = callback(context);
#endif

    --g_guardedCallbackDepth;
    g_guardedCallTotalNs.fetch_add(GuardTelemetryNowNs() - startNs, std::memory_order_relaxed);
    return result;
}
}

bool IsGuardedCallbackActive()
{
    return g_guardedCallbackDepth > 0;
}

bool IsRuntimePointerMappedInCurrentProcess(const void* pointer, size_t minBytes)
{
    RuntimeMemoryQuery query;
    return QueryRuntimeMemory(pointer, minBytes, query, nullptr);
}

bool PreflightRuntimePointer(
    const char* operation,
    const void* pointer,
    size_t minBytes,
    RuntimeAccess access,
    std::string* outReason)
{
    RuntimeMemoryQuery query;
    std::string reason;
    if (!QueryRuntimeMemory(pointer, minBytes, query, &reason))
    {
        reason = std::string(operation && operation[0] ? operation : "runtime pointer") +
            " " + AccessName(access) + " " + PointerString(pointer) +
            " failed: " + reason;
        if (outReason)
            *outReason = reason;
        RecordGuardPreflightFailure(operation, pointer, minBytes, reason);
        return false;
    }

    bool allowed = false;
    switch (access)
    {
    case RuntimeAccess::Read:
        allowed = query.readable;
        break;
    case RuntimeAccess::Write:
        allowed = query.writable;
        break;
    case RuntimeAccess::Execute:
        allowed = query.executable;
        break;
    default:
        allowed = false;
        break;
    }

    if (!allowed)
    {
        reason = std::string(operation && operation[0] ? operation : "runtime pointer") +
            " " + AccessName(access) + " " + PointerString(pointer) +
            " failed: page protection does not allow access";
        if (outReason)
            *outReason = reason;
        RecordGuardPreflightFailure(operation, pointer, minBytes, reason);
        return false;
    }

    if (outReason)
        outReason->clear();
    return true;
}

bool IsReadableRuntimePointer(const void* pointer, size_t minBytes)
{
    RuntimeMemoryQuery query;
    return QueryRuntimeMemory(pointer, minBytes, query, nullptr) && query.readable;
}

bool IsWritableRuntimePointer(const void* pointer, size_t minBytes)
{
    RuntimeMemoryQuery query;
    return QueryRuntimeMemory(pointer, minBytes, query, nullptr) && query.writable;
}

bool IsExecutableRuntimePointer(const void* pointer, size_t minBytes)
{
    RuntimeMemoryQuery query;
    return QueryRuntimeMemory(pointer, minBytes, query, nullptr) && query.executable;
}

bool IsRuntimePointerInLoadedModule(const void* pointer)
{
    if (!pointer)
        return false;

    HMODULE module = nullptr;
    return GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(pointer),
        &module) != FALSE;
}

std::string GetRuntimeModulePath(const void* pointer)
{
    if (!pointer)
        return {};

    HMODULE module = nullptr;
    if (GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(pointer),
        &module) == FALSE)
    {
        return {};
    }

    char path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(module, path, static_cast<DWORD>(sizeof(path)));
    if (length == 0 || length >= sizeof(path))
        return {};

    return std::string(path, length);
}

bool IsLikelyRuntimeCppObject(const void* object, size_t minBytes)
{
    if (!IsReadableRuntimePointer(object, minBytes))
        return false;

    const void* vtable = nullptr;
    if (!TryReadRuntimeValue(reinterpret_cast<const void* const*>(object), vtable))
        return false;

    return IsReadableRuntimePointer(vtable, sizeof(void*)) && IsRuntimePointerInLoadedModule(vtable);
}

std::string ReadRuntimeCString(const char* pointer, size_t maxLength)
{
    if (!pointer || maxLength == 0)
        return {};

    std::string value;
    value.reserve(std::min<size_t>(maxLength, 64));
    for (size_t i = 0; i < maxLength; ++i)
    {
        char ch = '\0';
        if (!TryReadRuntimeValue(pointer + i, ch))
            break;
        if (ch == '\0')
            break;
        value.push_back(ch);
    }

    return value;
}

bool RuntimeCStringEquals(const char* pointer, const char* expected, size_t maxLength)
{
    if (!pointer || !expected || maxLength == 0)
        return false;

    for (size_t i = 0; i < maxLength; ++i)
    {
        char actual = '\0';
        if (!TryReadRuntimeValue(pointer + i, actual))
            return false;

        const char expectedCh = expected[i];
        if (actual != expectedCh)
            return false;

        if (actual == '\0')
            return true;
    }

    return false;
}

GuardTelemetryTotals GetGuardTelemetryTotals()
{
    GuardTelemetryTotals totals;
    totals.guardedCallTotalNs = g_guardedCallTotalNs.load(std::memory_order_relaxed);
    totals.virtualQueryCalls = g_virtualQueryCalls.load(std::memory_order_relaxed);
    return totals;
}

RuntimeGuardSnapshot GetRuntimeGuardSnapshot()
{
    RuntimeGuardState& state = GetGuardState();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.snapshot;
}

bool TryRunGuardedCallback(
    const char* operation,
    RuntimeGuardCallback callback,
    void* context,
    std::string* outReason)
{
    SehCapture seh = {};
    const bool success = TryRunGuardedCallbackRaw(callback, context, &seh);
    if (success)
    {
        if (outReason)
            outReason->clear();
        RecordGuardedCall(true, operation, {}, nullptr);
        return true;
    }

    std::string reason;
    if (seh.code != 0)
    {
        reason = std::string(operation && operation[0] ? operation : "guarded native call") +
            " failed: SEH " + ExceptionCodeString(seh.code) +
            " at " + PointerString(seh.address);
    }
    else
    {
        reason = std::string(operation && operation[0] ? operation : "guarded native call") +
            " failed: callback returned false";
    }

    if (outReason)
        *outReason = reason;
    RecordGuardedCall(false, operation, reason, &seh);
    return false;
}
}
