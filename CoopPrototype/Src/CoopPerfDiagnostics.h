#pragma once

#include <cstdint>
#include <filesystem>

namespace CoopPerfDiagnostics
{
enum class Metric : uint8_t
{
    InventoryRestoreAttempt,
    SidecarPayloadSerialize,
    SidecarAtomicWrite,
    RecoveryJournalPrepare,
    RecoveryJournalAtomicWrite,
    Count,
};

bool Enabled();
bool Capturing();
bool ConfigureOutputDirectory(const std::filesystem::path& directory);

class ScopedTimer
{
public:
    explicit ScopedTimer(Metric metric);
    ~ScopedTimer();
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

    void Stop();

private:
    Metric m_metric;
    uint64_t m_startNs = 0;
};

class UpdateScope
{
public:
    UpdateScope(float frameTime, float realFrameTime, unsigned updateFlags);
    ~UpdateScope();
    UpdateScope(const UpdateScope&) = delete;
    UpdateScope& operator=(const UpdateScope&) = delete;

    bool Active() const { return m_startNs != 0; }
    void SetContext(
        const char* networkRole,
        bool gameReady,
        bool sessionReady,
        bool loadGuard,
        uint32_t pendingInventory);

private:
    uint64_t m_startNs = 0;
    unsigned m_updateFlags = 0;
};
}
