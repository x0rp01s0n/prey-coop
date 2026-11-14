#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace CoopPlayerStateStore
{
using Validator = std::function<bool(const std::filesystem::path&, std::string&)>;

bool MatchesTimeline(uint64_t currentTimeline, uint64_t incomingTimeline,
    uint32_t currentEpoch, uint32_t incomingEpoch, bool loading);

class Store
{
public:
    Store(std::filesystem::path hostRoot, Validator validator);

    // Reopen only a known continuing world. The caller must use RotateTimeline
    // after a process/world restart, even if disk identity is unchanged.
    bool Open(const std::string& saveKey, const std::string& fingerprint, std::string& error);
    bool RotateTimeline(const std::string& saveKey, const std::string& fingerprint, std::string& error);

    bool AdvanceLiveSave(const std::string& previousSaveKey, const std::string& nextSaveKey, std::string& error);

    // Only a successful native save may call this. Later live uploads must not.
    // A forward save advances checkpoint identity without changing lineage.
    bool CaptureCheckpoint(const std::string& saveKey, const std::string& fingerprint, std::string& error);
    bool RestoreCheckpoint(const std::string& saveKey, const std::string& fingerprint, std::string& error);

    uint64_t Timeline() const { return m_timeline; }
    const std::string& SaveKey() const { return m_saveKey; }
    const std::string& Fingerprint() const { return m_fingerprint; }
    bool RecoveryAllowed() const { return m_recoveryAllowed; }

    std::filesystem::path LiveDirectory(const std::string& saveKey) const;
    std::filesystem::path CheckpointDirectory(const std::string& fingerprint) const;

private:
    bool Rebase(const std::string& saveKey, const std::string& fingerprint, std::string& error);

    std::filesystem::path m_root;
    Validator m_validator;
    uint64_t m_timeline = 0;
    std::string m_saveKey;
    std::string m_fingerprint;
    bool m_recoveryAllowed = false;
    bool m_ready = false;
};
}
