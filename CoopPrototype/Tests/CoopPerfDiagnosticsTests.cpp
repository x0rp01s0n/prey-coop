#include "CoopPerfDiagnostics.h"
#include "CoopHookStats.h"
#include "CoopRuntimeGuards.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

namespace
{
uint64_t g_guardNs = 0;
uint64_t g_vqCalls = 0;
uint64_t g_vqNs = 0;
int g_hookResets = 0;

void TestDisabledDoesNotStartTimersOrCreateOutput(const std::filesystem::path& outputDirectory)
{
    assert(!CoopPerfDiagnostics::Enabled());
    assert(!CoopPerfDiagnostics::Capturing());
    assert(!CoopPerfDiagnostics::ConfigureOutputDirectory(outputDirectory));
    CoopPerfDiagnostics::UpdateScope update(0.016f, 0.017f, 0);
    assert(!update.Active());
    assert(!std::filesystem::exists(outputDirectory));
}

void TestEnabledWritesBoundedWindow(const std::filesystem::path& outputDirectory)
{
    assert(CoopPerfDiagnostics::Enabled());
    assert(!CoopPerfDiagnostics::Capturing());
    assert(CoopPerfDiagnostics::ConfigureOutputDirectory(outputDirectory));
    assert(CoopPerfDiagnostics::Capturing());
    {
        CoopPerfDiagnostics::ScopedTimer timer(CoopPerfDiagnostics::Metric::InventoryRestoreAttempt);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    {
        CoopPerfDiagnostics::ScopedTimer timer(CoopPerfDiagnostics::Metric::SidecarPayloadSerialize);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto started = std::chrono::steady_clock::now();
    bool sawLoadGuard = false;
    for (unsigned frame = 0; std::chrono::steady_clock::now() - started < std::chrono::milliseconds(2200); ++frame)
    {
        {
            CoopPerfDiagnostics::UpdateScope update(0.016f, 0.017f, frame % 2 ? 1u : 2u);
            update.SetContext(frame < 60 ? "host" : "client", true, frame > 10, sawLoadGuard, frame % 7);
            if (frame == 5)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                sawLoadGuard = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        g_guardNs += 100;
        ++g_vqCalls;
        g_vqNs += 25;
    }

    std::filesystem::path logPath;
    for (const auto& entry : std::filesystem::directory_iterator(outputDirectory))
    {
        if (entry.path().filename().string().rfind("coop_perf_diagnostics_", 0) == 0)
            logPath = entry.path();
    }
    assert(!logPath.empty());
    std::ifstream input(logPath, std::ios::binary);
    assert(input);
    const std::string log((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    assert(log.find("diagnosticVersion=lag-diagnostic-1") != std::string::npos);
    assert(log.find("protocol=250 modBuild=") != std::string::npos);
    assert(log.find("renderedFps=unavailable") != std::string::npos);
    assert(log.find("intervalP50Ms=") != std::string::npos);
    assert(log.find("intervalP95Ms=") != std::string::npos);
    assert(log.find("intervalP99Ms=") != std::string::npos);
    assert(log.find("intervalMaxStallMs=") != std::string::npos);
    assert(log.find("slow50ms=1") != std::string::npos);
    assert(log.find("networkRole=client") != std::string::npos);
    assert(log.find("networkRoleMask=6") != std::string::npos);
    assert(log.find("gameReadyUpdates=") != std::string::npos);
    assert(log.find("sessionReadyUpdates=") != std::string::npos);
    assert(log.find("loadGuardUpdates=") != std::string::npos);
    assert(log.find("pendingInventoryMax=") != std::string::npos);
    assert(log.find("restoreCalls=1") != std::string::npos);
    assert(log.find("serializeCalls=1") != std::string::npos);
    assert(log.find("countedVQCalls=") != std::string::npos);
    assert(log.find("effectiveReadMode=direct") != std::string::npos);
    assert(log.find("hookStatsCurrentWindow=testHook:1:1") != std::string::npos);
    assert(log.find("item=") == std::string::npos);
    assert(log.find("archetype") == std::string::npos);
    assert(g_hookResets > 0);
}
}

namespace CoopRuntimeConfig
{
bool Flag(const char* name)
{
    const char* value = std::getenv(name);
    return value && value[0] && std::atoi(value) != 0;
}
}

namespace CoopHookStats
{
std::string BuildReport()
{
    return "testHook:1:1";
}

void ResetAll()
{
    ++g_hookResets;
}
}

namespace CoopRuntimeGuards
{
GuardTelemetryTotals GetGuardTelemetryTotals()
{
    GuardTelemetryTotals totals;
    totals.guardedCallTotalNs = g_guardNs;
    return totals;
}

std::string GetGuardOpReport()
{
    return "test guard op:1:1";
}

uint64_t GetGuardVqCalls()
{
    return g_vqCalls;
}

uint64_t GetGuardVqTotalNs()
{
    return g_vqNs;
}

const char* RuntimeReadEffectiveModeName()
{
    return "direct";
}

const char* RuntimeObjectProbeEffectiveModeName()
{
    return "direct";
}
}

int main(int argc, char** argv)
{
    assert(argc == 3);
    const std::filesystem::path outputDirectory(argv[2]);
    if (std::string(argv[1]) == "disabled")
        TestDisabledDoesNotStartTimersOrCreateOutput(outputDirectory);
    else
        TestEnabledWritesBoundedWindow(outputDirectory);
    std::cout << "performance diagnostics tests passed\n";
    return 0;
}
