#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct CoopServerBookmark
{
    std::string name;
    std::string address;
    uint16_t port = 27015;
};

struct CoopIdentityConfigData
{
    uint32_t schemaVersion = 4;
    std::string username = "Player2";
    std::string accountStrategy = "generated_uuid";
    std::string accountId;
    uint64_t selectedModelArchetypeId = 10739735956144685671ull;
    std::string lastAddress = "127.0.0.1";
    uint16_t networkPort = 27015;
    bool showRemoteNameplate = true;
    bool showCoopHud = true;
    bool verboseDiagnostics = false;
    bool friendlyFire = false;
    std::string serverName = "Prey Multiplayer";
    uint16_t maxPlayers = 4;
    bool lanVisible = true;
    uint16_t serverAccessMode = 0;
    std::string serverPassword;
    std::string serverAllowlist;
    std::vector<CoopServerBookmark> favoriteServers;
    std::vector<CoopServerBookmark> recentServers;
};

class CoopIdentityConfig final
{
public:
    static constexpr uint32_t kCurrentSchemaVersion = 4;

    bool LoadOrCreate(const std::filesystem::path& profileRoot, const std::string& fallbackUsername);
    bool Save();

    const CoopIdentityConfigData& Data() const { return m_data; }
    CoopIdentityConfigData& MutableData() { return m_data; }
    uint64_t AccountToken() const { return m_accountToken; }
    const std::filesystem::path& Path() const { return m_path; }
    const std::string& Status() const { return m_status; }

    static uint64_t ComputeAccountToken(const std::string& accountId);
    static bool RunSelfTest(const std::filesystem::path& root, std::string& detail);

private:
    bool LoadExisting();
    bool CreateNew(const std::string& fallbackUsername, const char* reason);
    bool RecoverCorrupt(const std::string& fallbackUsername, const char* reason);
    bool ValidateAndNormalize(std::string& reason);

    static std::string GenerateUuid();
    static bool IsValidAccountId(const std::string& value);

    CoopIdentityConfigData m_data;
    std::filesystem::path m_path;
    uint64_t m_accountToken = 0;
    std::string m_status = "not loaded";
};
