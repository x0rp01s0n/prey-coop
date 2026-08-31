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
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>

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
    // Lock-free per-call counter (issue #2): atomically incremented on every
    // guarded call; folded into the snapshot by GetRuntimeGuardSnapshot.
    std::atomic<uint64_t> guardedCallsAtomic{0};
};

thread_local int g_guardedCallbackDepth = 0;

struct SehCapture
{
    unsigned code = 0;
    void* address = nullptr;
    void* faultAddress = nullptr;
};

RuntimeGuardState& GetGuardState()
{
    static RuntimeGuardState state;
    return state;
}

// Self-profiling: total guarded-call count/time and actual VirtualQuery API
// calls. These are the Windows-only hot-path costs (under Wine/Proton
// VirtualQuery is a user-space walk; on native Windows it is a kernel syscall
// per call), so we
// track them to quantify platform-specific overhead from telemetry alone.
std::atomic<uint64_t> g_guardedCallTotalNs{0};
std::atomic<uint64_t> g_virtualQueryCalls{0};

// VirtualQuery timing is kept separate from the general guarded-call timer so
// the release read fast path can prove that successful direct reads do not
// enter VQ. Guard-page restoration uses these counters only on its failure
// path.
std::atomic<uint64_t> s_vqTotalNs{0};
std::atomic<uint64_t> s_vqCalls{0};

// Direct-read/object-probe experiment counters.  These stay lock-free on the
// hot path and are folded into 10-second deltas by ModMain::MainUpdate.
std::atomic<uint64_t> g_runtimeReadAttempts{0};
std::atomic<uint64_t> g_runtimeReadFailures{0};
std::atomic<uint64_t> g_runtimeReadAccessViolationFailures{0};
std::atomic<uint64_t> g_runtimeReadInPageErrorFailures{0};
std::atomic<uint64_t> g_runtimeReadGuardPageFailures{0};
std::atomic<uint64_t> g_runtimeReadOtherFailures{0};
std::atomic<uint32_t> g_runtimeReadLastExceptionCode{0};
std::atomic<uint64_t> g_runtimeGuardRestoreAttempts{0};
std::atomic<uint64_t> g_runtimeGuardRestoreSuccesses{0};
std::atomic<uint64_t> g_runtimeGuardRestoreFailures{0};
std::atomic<uint64_t> g_runtimeGuardRestoreAlreadyPresent{0};
std::atomic<uint64_t> g_runtimeObjectProbeAttempts{0};
std::atomic<uint64_t> g_runtimeObjectProbeFailures{0};

// Per-operation guard cost attribution: fixed bucket table keyed by the
// operation-name pointer identity (all call sites pass string literals).
// Lock-free; a few ns per guarded call.
constexpr std::size_t kGuardOpBuckets = 32;
struct GuardOpBucket
{
    std::atomic<const char*> name{nullptr};
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> totalNs{0};
    std::atomic<uint64_t> successes{0};
    std::atomic<uint64_t> failures{0};
    std::atomic<uint64_t> failNs{0};
    std::atomic<uint32_t> lastExceptionCode{0};
};
GuardOpBucket g_guardOpBuckets[kGuardOpBuckets];

GuardOpBucket* GuardOpSlot(const char* name)
{
    if (!name || !name[0])
        name = "?";
    std::size_t hash = 1469598103934665603ull;
    for (const char* c = name; *c; ++c)
    {
        hash ^= static_cast<unsigned char>(*c);
        hash *= 1099511628211ull;
    }
    const std::size_t first = hash % kGuardOpBuckets;
    for (std::size_t probe = 0; probe < kGuardOpBuckets; ++probe)
    {
        GuardOpBucket& bucket = g_guardOpBuckets[(first + probe) % kGuardOpBuckets];
        if (bucket.name.load(std::memory_order_acquire) == name)
            return &bucket;
        const char* expected = nullptr;
        if (bucket.name.load(std::memory_order_acquire) == nullptr &&
            bucket.name.compare_exchange_strong(expected, name))
        {
            return &bucket;
        }
    }
    return nullptr;
}

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

int RuntimeReadExceptionFilter(unsigned code, EXCEPTION_POINTERS* pointers, SehCapture* outCapture);

enum class RuntimeReadMode
{
    VqPreflight,
    Direct,
};

RuntimeReadMode GetRuntimeReadMode()
{
    // This is intentionally read once per process. It keeps environment lookup
    // and string work out of the hot path. Release uses direct SEH reads by
    // default; the VQ mode is retained as an explicit A/B fallback.
    static const RuntimeReadMode mode = [] {
        char value[16] = {};
        const DWORD length = GetEnvironmentVariableA(
            "COOP_RUNTIME_READ_MODE", value, static_cast<DWORD>(sizeof(value)));
        if (length > 0 && length < sizeof(value) && std::strcmp(value, "vq") == 0)
            return RuntimeReadMode::VqPreflight;
        return RuntimeReadMode::Direct;
    }();
    return mode;
}

enum class RuntimeObjectProbeMode
{
    VqPreflight,
    Direct,
};

enum class RuntimeGuardPagePolicy
{
    Preserve,
    AllowConsume,
};

RuntimeObjectProbeMode GetRuntimeObjectProbeMode()
{
    static const RuntimeObjectProbeMode mode = [] {
        char value[16] = {};
        const DWORD length = GetEnvironmentVariableA(
            "COOP_RUNTIME_OBJECT_PROBE_MODE", value, static_cast<DWORD>(sizeof(value)));
        if (length > 0 && length < sizeof(value) &&
            (std::strcmp(value, "direct") == 0 || std::strcmp(value, "seh") == 0))
            return RuntimeObjectProbeMode::Direct;
        return RuntimeObjectProbeMode::VqPreflight;
    }();
    return mode;
}

RuntimeGuardPagePolicy GetRuntimeGuardPagePolicy()
{
    static const RuntimeGuardPagePolicy policy = [] {
        char value[8] = {};
        const DWORD length = GetEnvironmentVariableA(
            "COOP_RUNTIME_ALLOW_GUARD_CONSUME", value, static_cast<DWORD>(sizeof(value)));
        if (length == 1 && value[0] == '1')
            return RuntimeGuardPagePolicy::AllowConsume;
        return RuntimeGuardPagePolicy::Preserve;
    }();
    return policy;
}

SIZE_T CountedVirtualQuery(const void* pointer, MEMORY_BASIC_INFORMATION& memoryInfo)
{
    g_virtualQueryCalls.fetch_add(1, std::memory_order_relaxed);
    const uint64_t startNs = GuardTelemetryNowNs();
    const SIZE_T result = VirtualQuery(pointer, &memoryInfo, sizeof(memoryInfo));
    s_vqTotalNs.fetch_add(GuardTelemetryNowNs() - startNs, std::memory_order_relaxed);
    s_vqCalls.fetch_add(1, std::memory_order_relaxed);
    return result;
}

void RestoreGuardPageAfterException(const SehCapture& capture)
{
    if (capture.code != EXCEPTION_GUARD_PAGE ||
        GetRuntimeGuardPagePolicy() == RuntimeGuardPagePolicy::AllowConsume)
    {
        return;
    }

    g_runtimeGuardRestoreAttempts.fetch_add(1, std::memory_order_relaxed);
    if (!capture.faultAddress)
    {
        g_runtimeGuardRestoreFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // The OS consumes PAGE_GUARD before dispatching the exception. Query and
    // restore only the page that faulted, and only on this rare failure path;
    // successful direct reads never execute either API.
    MEMORY_BASIC_INFORMATION memoryInfo = {};
    if (CountedVirtualQuery(capture.faultAddress, memoryInfo) == 0 ||
        memoryInfo.State != MEM_COMMIT || memoryInfo.Protect == 0)
    {
        g_runtimeGuardRestoreFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if ((memoryInfo.Protect & PAGE_GUARD) != 0)
    {
        g_runtimeGuardRestoreAlreadyPresent.fetch_add(1, std::memory_order_relaxed);
        g_runtimeGuardRestoreSuccesses.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    DWORD oldProtect = 0;
    if (VirtualProtect(
        capture.faultAddress,
        1,
        memoryInfo.Protect | PAGE_GUARD,
        &oldProtect) == FALSE)
    {
        g_runtimeGuardRestoreFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_runtimeGuardRestoreSuccesses.fetch_add(1, std::memory_order_relaxed);
}

bool UseDirectRuntimeRead()
{
    return GetRuntimeReadMode() == RuntimeReadMode::Direct;
}

bool UseDirectRuntimeObjectProbe()
{
    // Object probing remains VQ-backed by default. Direct probing is an
    // independent, explicit experiment and does not silently broaden the
    // release policy used by the typed value reader.
    return GetRuntimeObjectProbeMode() == RuntimeObjectProbeMode::Direct;
}

void RecordRuntimeReadFailure(unsigned code)
{
    g_runtimeReadFailures.fetch_add(1, std::memory_order_relaxed);
    g_runtimeReadLastExceptionCode.store(code, std::memory_order_relaxed);
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
        g_runtimeReadAccessViolationFailures.fetch_add(1, std::memory_order_relaxed);
        break;
    case EXCEPTION_IN_PAGE_ERROR:
        g_runtimeReadInPageErrorFailures.fetch_add(1, std::memory_order_relaxed);
        break;
    case EXCEPTION_GUARD_PAGE:
        g_runtimeReadGuardPageFailures.fetch_add(1, std::memory_order_relaxed);
        break;
    default:
        g_runtimeReadOtherFailures.fetch_add(1, std::memory_order_relaxed);
        break;
    }
}

void RecordRuntimeObjectProbeFailure()
{
    g_runtimeObjectProbeFailures.fetch_add(1, std::memory_order_relaxed);
}

// Probe a runtime span without VirtualQuery in the explicit direct mode.
// IsLikelyRuntimeCppObject is called from several hot hooks; the old
// validation paid for a Windows VirtualQuery before subsequently reading the
// same bytes under SEH. Touch only one byte per covered page (plus the final
// byte) so large object-size checks do not become a second memcpy-sized hot
// path. The default VQ mode remains conservative. Direct mode is an explicit
// experiment and re-arms PAGE_GUARD after a guard exception unless the
// separate COOP_RUNTIME_ALLOW_GUARD_CONSUME=1 opt-in is present.
bool TryProbeRuntimeReadSeh(const void* pointer, size_t size)
{
    g_runtimeObjectProbeAttempts.fetch_add(1, std::memory_order_relaxed);
    if (!pointer || size == 0)
    {
        RecordRuntimeObjectProbeFailure();
        return false;
    }

    if (!UseDirectRuntimeObjectProbe())
    {
        const bool result = IsReadableRuntimePointer(pointer, size);
        if (!result)
            RecordRuntimeObjectProbeFailure();
        return result;
    }

    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(pointer);
    if (size > (std::numeric_limits<std::uintptr_t>::max)() - start)
    {
        RecordRuntimeObjectProbeFailure();
        return false;
    }
    const std::uintptr_t last = start + size - 1;
    static const std::uintptr_t pageSize = [] {
        SYSTEM_INFO systemInfo = {};
        GetSystemInfo(&systemInfo);
        return static_cast<std::uintptr_t>(systemInfo.dwPageSize ? systemInfo.dwPageSize : 4096u);
    }();

    bool result = false;
    SehCapture capture;
    __try
    {
        volatile unsigned char observed = 0;
        std::uintptr_t address = start;
        for (;;)
        {
            const volatile unsigned char* byte =
                reinterpret_cast<const volatile unsigned char*>(address);
            observed = static_cast<unsigned char>(observed ^ *byte);

            const std::uintptr_t pageOffset = address % pageSize;
            const std::uintptr_t bytesToNextPage = pageSize - pageOffset;
            if (bytesToNextPage > last - address)
                break;
            address += bytesToNextPage;
        }
        if (address != last)
        {
            const volatile unsigned char* finalByte =
                reinterpret_cast<const volatile unsigned char*>(last);
            observed = static_cast<unsigned char>(observed ^ *finalByte);
        }
        (void)observed;
        result = true;
    }
    __except (RuntimeReadExceptionFilter(
        GetExceptionCode(), GetExceptionInformation(), &capture))
    {
        result = false;
    }
    if (!result)
    {
        RestoreGuardPageAfterException(capture);
        RecordRuntimeObjectProbeFailure();
    }
    return result;
}

constexpr size_t kRuntimeReadInlineBytes = 512;

int RuntimeReadExceptionFilter(
    unsigned code,
    EXCEPTION_POINTERS* pointers,
    SehCapture* outCapture)
{
    if (outCapture)
    {
        outCapture->code = code;
        outCapture->address = pointers && pointers->ExceptionRecord
            ? pointers->ExceptionRecord->ExceptionAddress
            : nullptr;
        if (code == EXCEPTION_GUARD_PAGE && pointers && pointers->ExceptionRecord &&
            pointers->ExceptionRecord->NumberParameters >= 2)
        {
            outCapture->faultAddress = reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(pointers->ExceptionRecord->ExceptionInformation[1]));
        }
    }

    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_GUARD_PAGE:
        return EXCEPTION_EXECUTE_HANDLER;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

// The common reads are scalar values and fit in this per-thread scratch
// buffer.  Larger reads use the process heap only when needed and retain the
// allocation for the thread, so the hot path does not allocate per call.
struct RuntimeReadScratch
{
    std::array<unsigned char, kRuntimeReadInlineBytes> inlineBytes{};
    unsigned char* extendedBytes = nullptr;
    size_t extendedCapacity = 0;

    ~RuntimeReadScratch()
    {
        if (extendedBytes)
            HeapFree(GetProcessHeap(), 0, extendedBytes);
    }
};

RuntimeReadScratch& GetRuntimeReadScratch()
{
    thread_local RuntimeReadScratch scratch;
    return scratch;
}

bool QueryRuntimeMemory(const void* pointer, size_t minBytes, RuntimeMemoryQuery& outQuery, std::string* outReason)
{
    const auto value = reinterpret_cast<std::uintptr_t>(pointer);
    outQuery = {};
    outQuery.value = value;

    if (value < 0x10000 || value == ~std::uintptr_t{ 0 })
    {
        if (outReason)
            *outReason = "address is null/sentinel/small";
        return false;
    }

    MEMORY_BASIC_INFORMATION memoryInfo = {};
    const SIZE_T vqResult = CountedVirtualQuery(pointer, memoryInfo);
    if (vqResult == 0)
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
    // Lock-free fast path (issue #2): this runs for EVERY guarded call on
    // every thread. The original per-call mutex serialised all threads and,
    // with ~11k guarded calls/second, blocked the main thread behind
    // background telemetry bookkeeping (measured: main thread 91% blocked in
    // ntdll wait). Counters are atomics now; the failure strings are only
    // updated on failures (rare) under the mutex.
    RuntimeGuardState& state = GetGuardState();
    state.guardedCallsAtomic.fetch_add(1, std::memory_order_relaxed);
    if (success)
        return;

    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.snapshot.guardedCallFailures;
    if (seh && seh->code != 0)
        ++state.snapshot.sehExceptions;
    state.snapshot.lastOperation = operation && operation[0] ? operation : "-";
    state.snapshot.lastReason = reason;
    state.snapshot.lastExceptionCode = seh ? seh->code : 0;
    state.snapshot.lastExceptionAddress = seh ? reinterpret_cast<std::uintptr_t>(seh->address) : 0;
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

bool TryReadRuntimeValueImpl(const void* pointer, void* outValue, size_t size, bool validateOutput)
{
    g_runtimeReadAttempts.fetch_add(1, std::memory_order_relaxed);

    if (!pointer || !outValue || size == 0)
    {
        RecordRuntimeReadFailure(0);
        return false;
    }

    // Direct SEH is the release fast path. The old VQ preflight/copy path is
    // retained behind COOP_RUNTIME_READ_MODE=vq for controlled A/B runs.
    if (!UseDirectRuntimeRead() &&
        !PreflightRuntimePointer("read runtime value", pointer, size, RuntimeAccess::Read))
    {
        RecordRuntimeReadFailure(0);
        return false;
    }

    // Only the raw void* API validates its caller-owned destination. The
    // typed wrapper passes a live local T and deliberately avoids this second
    // VQ call; in explicit VQ mode it retains the original source-only
    // preflight.
    if (validateOutput &&
        !PreflightRuntimePointer(
            "read runtime value output", outValue, size, RuntimeAccess::Write))
    {
        RecordRuntimeReadFailure(0);
        return false;
    }

    if (!validateOutput)
    {
        // The typed wrapper supplies a trusted, live local destination. A
        // source fault can therefore leave this staging object partial while
        // the caller's output remains untouched until this function returns
        // successfully. This is also the one-copy direct fast path.
        bool readComplete = false;
        SehCapture capture;
        __try
        {
            std::memcpy(outValue, pointer, size);
            readComplete = true;
        }
        __except (RuntimeReadExceptionFilter(
            GetExceptionCode(), GetExceptionInformation(), &capture))
        {
        }
        if (!readComplete)
        {
            RestoreGuardPageAfterException(capture);
            RecordRuntimeReadFailure(capture.code);
        }
        return readComplete;
    }

    RuntimeReadScratch& scratch = GetRuntimeReadScratch();
    unsigned char* temporary = scratch.inlineBytes.data();
    if (size > kRuntimeReadInlineBytes)
    {
        if (size > (std::numeric_limits<size_t>::max)() - 7u)
        {
            RecordRuntimeReadFailure(0);
            return false;
        }
        const size_t requiredCapacity = (size + 7u) & ~size_t{ 7u };
        if (requiredCapacity > scratch.extendedCapacity)
        {
            void* resized = scratch.extendedBytes
                ? HeapReAlloc(GetProcessHeap(), 0, scratch.extendedBytes, requiredCapacity)
                : HeapAlloc(GetProcessHeap(), 0, requiredCapacity);
            if (!resized)
            {
                RecordRuntimeReadFailure(0);
                return false;
            }
            scratch.extendedBytes = static_cast<unsigned char*>(resized);
            scratch.extendedCapacity = requiredCapacity;
        }
        temporary = scratch.extendedBytes;
    }

    bool readComplete = false;
    SehCapture readCapture;
    __try
    {
        // The conservative path keeps the native copy after source VQ;
        // staging protects the raw destination from source faults. Direct
        // mode deliberately performs the same copy under MSVC SEH, without
        // a source VQ.
        std::memcpy(temporary, pointer, size);
        readComplete = true;
    }
    __except (RuntimeReadExceptionFilter(
        GetExceptionCode(), GetExceptionInformation(), &readCapture))
    {
    }

    if (!readComplete)
    {
        RestoreGuardPageAfterException(readCapture);
        RecordRuntimeReadFailure(readCapture.code);
        return false;
    }

    bool commitComplete = false;
    SehCapture commitCapture;
    __try
    {
        std::memcpy(outValue, temporary, size);
        commitComplete = true;
    }
    __except (RuntimeReadExceptionFilter(
        GetExceptionCode(), GetExceptionInformation(), &commitCapture))
    {
    }
    if (!commitComplete)
    {
        // The source is fully staged, but a raw destination can still race
        // with protection/lifetime changes during this non-atomic commit.
        RestoreGuardPageAfterException(commitCapture);
        RecordRuntimeReadFailure(commitCapture.code);
        return false;
    }
    return true;
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

bool TryRuntimeReadProbeSeh(const void* pointer, size_t size)
{
    return TryProbeRuntimeReadSeh(pointer, size);
}

const char* RuntimeReadModeName()
{
    return GetRuntimeReadMode() == RuntimeReadMode::VqPreflight ? "vq" : "direct";
}

const char* RuntimeReadEffectiveModeName()
{
    return UseDirectRuntimeRead() ? "direct" : "vq";
}

const char* RuntimeObjectProbeModeName()
{
    return GetRuntimeObjectProbeMode() == RuntimeObjectProbeMode::VqPreflight ? "vq" : "direct";
}

const char* RuntimeObjectProbeEffectiveModeName()
{
    return UseDirectRuntimeObjectProbe() ? "direct" : "vq";
}

const char* RuntimeGuardPagePolicyName()
{
    return GetRuntimeGuardPagePolicy() == RuntimeGuardPagePolicy::Preserve
        ? "preserve"
        : "allowConsume";
}

RuntimeReadTelemetry GetRuntimeReadTelemetry()
{
    RuntimeReadTelemetry telemetry;
    telemetry.attempts = g_runtimeReadAttempts.load(std::memory_order_relaxed);
    telemetry.failures = g_runtimeReadFailures.load(std::memory_order_relaxed);
    telemetry.accessViolationFailures =
        g_runtimeReadAccessViolationFailures.load(std::memory_order_relaxed);
    telemetry.inPageErrorFailures =
        g_runtimeReadInPageErrorFailures.load(std::memory_order_relaxed);
    telemetry.guardPageFailures =
        g_runtimeReadGuardPageFailures.load(std::memory_order_relaxed);
    telemetry.otherFailures = g_runtimeReadOtherFailures.load(std::memory_order_relaxed);
    telemetry.guardPageRestoreAttempts =
        g_runtimeGuardRestoreAttempts.load(std::memory_order_relaxed);
    telemetry.guardPageRestoreSuccesses =
        g_runtimeGuardRestoreSuccesses.load(std::memory_order_relaxed);
    telemetry.guardPageRestoreFailures =
        g_runtimeGuardRestoreFailures.load(std::memory_order_relaxed);
    telemetry.guardPageRestoreAlreadyPresent =
        g_runtimeGuardRestoreAlreadyPresent.load(std::memory_order_relaxed);
    telemetry.lastExceptionCode = g_runtimeReadLastExceptionCode.load(std::memory_order_relaxed);
    return telemetry;
}

RuntimeObjectProbeTelemetry GetRuntimeObjectProbeTelemetry()
{
    RuntimeObjectProbeTelemetry telemetry;
    telemetry.attempts = g_runtimeObjectProbeAttempts.load(std::memory_order_relaxed);
    telemetry.failures = g_runtimeObjectProbeFailures.load(std::memory_order_relaxed);
    return telemetry;
}

uint64_t GetGuardVqCalls()
{
    return s_vqCalls.load(std::memory_order_relaxed);
}

uint64_t GetGuardVqTotalNs()
{
    return s_vqTotalNs.load(std::memory_order_relaxed);
}

bool TryReadRuntimeValueSeh(const void* pointer, void* outValue, size_t size)
{
    return TryReadRuntimeValueImpl(pointer, outValue, size, true);
}

namespace detail
{
bool TryReadRuntimeValueTrustedSeh(const void* pointer, void* outValue, size_t size)
{
    return TryReadRuntimeValueImpl(pointer, outValue, size, false);
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
    // This is separate from TryReadRuntimeValue's one-value fix: it removes
    // the two read-side VirtualQuery preflights that object validation used
    // to add around that read.  Write/execute checks retain their VQ-backed
    // protection semantics because they cannot be replaced by a read probe.
    if (!TryProbeRuntimeReadSeh(object, minBytes))
        return false;

    const void* vtable = nullptr;
    if (!TryReadRuntimeValue(reinterpret_cast<const void* const*>(object), vtable))
        return false;

    return TryProbeRuntimeReadSeh(vtable, sizeof(void*)) && IsRuntimePointerInLoadedModule(vtable);
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
    const RuntimeGuardSnapshot snapshot = GetRuntimeGuardSnapshot();
    totals.sehExceptions = snapshot.sehExceptions;
    totals.guardedCallFailures = snapshot.guardedCallFailures;
    totals.preflightFailures = snapshot.preflightFailures;
    return totals;
}

uint64_t GuardTelemetryNowNsPublic()
{
    return GuardTelemetryNowNs();
}

std::string GetGuardOpReport()
{
    struct Entry
    {
        const char* name;
        uint64_t calls;
        uint64_t ns;
        uint64_t failures;
        uint64_t failNs;
    };
    Entry entries[kGuardOpBuckets];
    std::size_t count = 0;
    for (GuardOpBucket& bucket : g_guardOpBuckets)
    {
        const char* name = bucket.name.load(std::memory_order_acquire);
        if (!name)
            continue;
        if (count < kGuardOpBuckets)
        {
            entries[count++] = Entry{
                name,
                bucket.calls.load(std::memory_order_relaxed),
                bucket.totalNs.load(std::memory_order_relaxed),
                bucket.failures.load(std::memory_order_relaxed),
                bucket.failNs.load(std::memory_order_relaxed)};
        }
    }
    std::sort(entries, entries + count, [](const Entry& a, const Entry& b) { return a.ns > b.ns; });
    std::string out;
    const std::size_t limit = std::min<std::size_t>(count, 5);
    for (std::size_t i = 0; i < limit; ++i)
    {
        if (entries[i].ns == 0)
            break;
        if (!out.empty())
            out += '|';
        out += entries[i].name;
        out += ':';
        out += std::to_string(entries[i].calls);
        out += ':';
        out += std::to_string(entries[i].ns / 1000);
        out += ':';
        out += std::to_string(entries[i].failures);
        out += ':';
        out += std::to_string(entries[i].failNs / 1000);
    }
    return out;
}

RuntimeGuardSnapshot GetRuntimeGuardSnapshot()
{
    RuntimeGuardState& state = GetGuardState();
    RuntimeGuardSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        snapshot = state.snapshot;
    }
    snapshot.guardedCalls = state.guardedCallsAtomic.load(std::memory_order_relaxed);
    return snapshot;
}

bool TryRunGuardedCallback(
    const char* operation,
    RuntimeGuardCallback callback,
    void* context,
    std::string* outReason)
{
    GuardOpBucket* opSlot = GuardOpSlot(operation);
    const uint64_t opStartNs = GuardTelemetryNowNs();
    SehCapture seh = {};
    const bool success = TryRunGuardedCallbackRaw(callback, context, &seh);
    if (opSlot)
    {
        const uint64_t opNs = GuardTelemetryNowNs() - opStartNs;
        opSlot->calls.fetch_add(1, std::memory_order_relaxed);
        opSlot->totalNs.fetch_add(opNs, std::memory_order_relaxed);
        if (success)
            opSlot->successes.fetch_add(1, std::memory_order_relaxed);
        else
        {
            opSlot->failures.fetch_add(1, std::memory_order_relaxed);
            opSlot->failNs.fetch_add(opNs, std::memory_order_relaxed);
            if (seh.code != 0)
                opSlot->lastExceptionCode.store(seh.code, std::memory_order_relaxed);
        }
    }
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
