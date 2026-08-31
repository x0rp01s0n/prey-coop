#include "CoopIdentityConfig.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <system_error>

#include <pugixml.hpp>

namespace
{
constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

std::string TrimPrintable(std::string value, size_t maxLength)
{
    value.erase(
        std::remove_if(value.begin(), value.end(), [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; }),
        value.end());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    if (value.size() > maxLength)
        value.resize(maxLength);
    return value;
}

bool ReadBoolAttribute(const pugi::xml_node& node, const char* name, bool fallback)
{
    const pugi::xml_attribute attribute = node.attribute(name);
    return attribute ? attribute.as_bool(fallback) : fallback;
}

bool ReplaceFile(const std::filesystem::path& tempPath, const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::rename(tempPath, path, error);
    if (!error)
        return true;

    error.clear();
    std::filesystem::copy_file(tempPath, path, std::filesystem::copy_options::overwrite_existing, error);
    std::error_code removeError;
    std::filesystem::remove(tempPath, removeError);
    return !error;
}
}

uint64_t CoopIdentityConfig::ComputeAccountToken(const std::string& accountId)
{
    uint64_t hash = kFnvOffset;
    constexpr char kNamespace[] = "PreyCoopPrototype.PlayerAccountId.v1:";
    for (const char ch : kNamespace)
    {
        if (ch == '\0')
            break;
        hash ^= static_cast<uint8_t>(ch);
        hash *= kFnvPrime;
    }
    for (const char ch : accountId)
    {
        hash ^= static_cast<uint8_t>(ch);
        hash *= kFnvPrime;
    }

    // Final avalanche keeps short external ids from exposing FNV structure on the wire.
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdull;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ull;
    hash ^= hash >> 33;
    return hash == 0 ? 1 : hash;
}

std::string CoopIdentityConfig::GenerateUuid()
{
    std::array<uint8_t, 16> bytes = {};
    std::random_device random;
    for (uint8_t& value : bytes)
        value = static_cast<uint8_t>(random());

    const uint64_t timeSeed = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    for (size_t i = 0; i < sizeof(timeSeed); ++i)
        bytes[i] ^= static_cast<uint8_t>(timeSeed >> (i * 8));

    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);

    char text[37] = {};
    std::snprintf(
        text,
        sizeof(text),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11],
        bytes[12], bytes[13], bytes[14], bytes[15]);
    return text;
}

bool CoopIdentityConfig::IsValidAccountId(const std::string& value)
{
    if (value.size() < 8 || value.size() > 128)
        return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch)
    {
        return std::isalnum(ch) || ch == '-' || ch == '_' || ch == ':' || ch == '.';
    });
}

bool CoopIdentityConfig::ValidateAndNormalize(std::string& reason)
{
    m_data.username = TrimPrintable(m_data.username, 31);
    if (m_data.username.empty())
        m_data.username = "Player";
    m_data.accountStrategy = TrimPrintable(m_data.accountStrategy, 32);
    m_data.accountId = TrimPrintable(m_data.accountId, 128);
    m_data.lastAddress = TrimPrintable(m_data.lastAddress, 255);
    if (m_data.lastAddress.empty())
        m_data.lastAddress = "127.0.0.1";
    if (m_data.networkPort == 0)
        m_data.networkPort = 27015;
    if (m_data.selectedModelArchetypeId == 0)
        m_data.selectedModelArchetypeId = 10739735956144685671ull;
    m_data.serverName = TrimPrintable(m_data.serverName, 47);
    if (m_data.serverName.empty())
        m_data.serverName = "Prey Multiplayer";
    m_data.maxPlayers = static_cast<uint16_t>(std::clamp<unsigned>(m_data.maxPlayers, 2, 16));
    m_data.serverAccessMode = static_cast<uint16_t>(std::min<unsigned>(m_data.serverAccessMode, 3));
    m_data.serverPassword = TrimPrintable(m_data.serverPassword, 95);
    m_data.serverAllowlist = TrimPrintable(m_data.serverAllowlist, 2048);
    const auto normalizeBookmarks = [](std::vector<CoopServerBookmark>& bookmarks, size_t maxCount)
    {
        std::vector<CoopServerBookmark> normalized;
        normalized.reserve(std::min(bookmarks.size(), maxCount));
        for (CoopServerBookmark bookmark : bookmarks)
        {
            bookmark.name = TrimPrintable(std::move(bookmark.name), 47);
            bookmark.address = TrimPrintable(std::move(bookmark.address), 255);
            if (bookmark.address.empty() || bookmark.port == 0)
                continue;
            const bool duplicate = std::any_of(
                normalized.begin(),
                normalized.end(),
                [&](const CoopServerBookmark& existing)
                {
                    return existing.address == bookmark.address && existing.port == bookmark.port;
                });
            if (!duplicate)
                normalized.push_back(std::move(bookmark));
            if (normalized.size() >= maxCount)
                break;
        }
        bookmarks = std::move(normalized);
    };
    normalizeBookmarks(m_data.favoriteServers, 32);
    normalizeBookmarks(m_data.recentServers, 16);
    if (m_data.schemaVersion == 0 || m_data.schemaVersion > kCurrentSchemaVersion)
    {
        reason = "unsupported schema";
        return false;
    }
    if (!IsValidAccountId(m_data.accountId))
    {
        reason = "invalid account id";
        return false;
    }
    if (m_data.accountStrategy.empty())
        m_data.accountStrategy = "generated_uuid";
    m_data.schemaVersion = kCurrentSchemaVersion;
    m_accountToken = ComputeAccountToken(m_data.accountId);
    reason.clear();
    return true;
}

bool CoopIdentityConfig::LoadExisting()
{
    pugi::xml_document document;
    std::ifstream input(m_path, std::ios::binary);
    if (!input)
    {
        m_status = "parse failed: open";
        return false;
    }
    const pugi::xml_parse_result parsed = document.load(input, pugi::parse_default, pugi::encoding_utf8);
    if (!parsed)
    {
        m_status = std::string("parse failed: ") + parsed.description();
        return false;
    }

    const pugi::xml_node root = document.child("CoopPrototypeConfig");
    if (!root)
    {
        m_status = "parse failed: missing root";
        return false;
    }

    CoopIdentityConfigData loaded;
    loaded.schemaVersion = root.attribute("schemaVersion").as_uint(0);
    const pugi::xml_node identity = root.child("Identity");
    const pugi::xml_node player = root.child("Player");
    const pugi::xml_node network = root.child("Network");
    const pugi::xml_node ui = root.child("Ui");
    const pugi::xml_node diagnostics = root.child("Diagnostics");
    const pugi::xml_node gameplay = root.child("Gameplay");
    const pugi::xml_node hosting = root.child("Hosting");
    loaded.username = identity.attribute("username").as_string();
    loaded.accountStrategy = identity.attribute("strategy").as_string();
    loaded.accountId = identity.attribute("accountId").as_string();
    loaded.selectedModelArchetypeId = player.attribute("modelArchetypeId").as_ullong(10739735956144685671ull);
    loaded.lastAddress = network.attribute("lastAddress").as_string("127.0.0.1");
    const unsigned port = network.attribute("port").as_uint(27015);
    loaded.networkPort = static_cast<uint16_t>(std::clamp(port, 1u, 65535u));
    loaded.showRemoteNameplate = ReadBoolAttribute(ui, "showRemoteNameplate", true);
    loaded.showCoopHud = ReadBoolAttribute(ui, "showCoopHud", true);
    loaded.verboseDiagnostics = ReadBoolAttribute(diagnostics, "verbose", false);
    loaded.friendlyFire = ReadBoolAttribute(gameplay, "friendlyFire", false);
    loaded.serverName = hosting.attribute("serverName").as_string("Prey Multiplayer");
    loaded.maxPlayers = static_cast<uint16_t>(std::clamp(hosting.attribute("maxPlayers").as_uint(4), 2u, 16u));
    loaded.lanVisible = ReadBoolAttribute(hosting, "lanVisible", true);
    loaded.serverAccessMode = static_cast<uint16_t>(std::min(hosting.attribute("accessMode").as_uint(0), 3u));
    loaded.serverPassword = hosting.attribute("password").as_string();
    loaded.serverAllowlist = hosting.attribute("allowlist").as_string();
    const auto loadBookmarks = [](const pugi::xml_node& parent, std::vector<CoopServerBookmark>& output)
    {
        for (const pugi::xml_node server : parent.children("Server"))
        {
            CoopServerBookmark bookmark;
            bookmark.name = server.attribute("name").as_string();
            bookmark.address = server.attribute("address").as_string();
            bookmark.port = static_cast<uint16_t>(std::clamp(server.attribute("port").as_uint(27015), 1u, 65535u));
            output.push_back(std::move(bookmark));
        }
    };
    loadBookmarks(root.child("Favorites"), loaded.favoriteServers);
    loadBookmarks(root.child("RecentServers"), loaded.recentServers);

    m_data = std::move(loaded);
    std::string reason;
    if (!ValidateAndNormalize(reason))
    {
        m_status = "validation failed: " + reason;
        return false;
    }

    m_status = "loaded schema " + std::to_string(m_data.schemaVersion);
    return true;
}

bool CoopIdentityConfig::CreateNew(const std::string& fallbackUsername, const char* reason)
{
    m_data = CoopIdentityConfigData();
    m_data.username = TrimPrintable(fallbackUsername, 31);
    if (m_data.username.empty())
        m_data.username = "Player2";

    const char* externalId = std::getenv("COOP_PLAYER_ACCOUNT_ID");
    if (externalId && IsValidAccountId(externalId))
    {
        m_data.accountStrategy = "environment";
        m_data.accountId = externalId;
    }
    else
    {
        m_data.accountStrategy = "generated_uuid";
        m_data.accountId = GenerateUuid();
    }

    std::string validationReason;
    if (!ValidateAndNormalize(validationReason))
    {
        m_status = "create failed: " + validationReason;
        return false;
    }
    if (!Save())
        return false;
    m_status = std::string("created: ") + (reason ? reason : "new config");
    return true;
}

bool CoopIdentityConfig::RecoverCorrupt(const std::string& fallbackUsername, const char* reason)
{
    std::error_code error;
    std::filesystem::path backup = m_path;
    backup += ".corrupt";
    std::filesystem::remove(backup, error);
    error.clear();
    std::filesystem::rename(m_path, backup, error);
    if (error)
    {
        error.clear();
        std::filesystem::copy_file(m_path, backup, std::filesystem::copy_options::overwrite_existing, error);
        if (!error)
        {
            std::error_code removeError;
            std::filesystem::remove(m_path, removeError);
        }
    }
    return CreateNew(fallbackUsername, reason);
}

bool CoopIdentityConfig::LoadOrCreate(const std::filesystem::path& profileRoot, const std::string& fallbackUsername)
{
    if (profileRoot.empty())
    {
        m_status = "load failed: profile root unavailable";
        return false;
    }
    m_path = profileRoot / "CoopPrototype" / "coop_config.xml";

    std::error_code error;
    if (std::filesystem::exists(m_path, error) && !error)
    {
        if (LoadExisting())
            return true;
        return RecoverCorrupt(fallbackUsername, m_status.c_str());
    }
    return CreateNew(fallbackUsername, "first run");
}

bool CoopIdentityConfig::Save()
{
    if (m_path.empty())
    {
        m_status = "save failed: path unavailable";
        return false;
    }
    std::string reason;
    if (!ValidateAndNormalize(reason))
    {
        m_status = "save failed: " + reason;
        return false;
    }

    std::error_code error;
    std::filesystem::create_directories(m_path.parent_path(), error);
    if (error)
    {
        m_status = "save failed: mkdir";
        return false;
    }

    pugi::xml_document document;
    pugi::xml_node root = document.append_child("CoopPrototypeConfig");
    root.append_attribute("schemaVersion").set_value(kCurrentSchemaVersion);
    pugi::xml_node identity = root.append_child("Identity");
    identity.append_attribute("username").set_value(m_data.username.c_str());
    identity.append_attribute("strategy").set_value(m_data.accountStrategy.c_str());
    identity.append_attribute("accountId").set_value(m_data.accountId.c_str());
    pugi::xml_node player = root.append_child("Player");
    player.append_attribute("modelArchetypeId").set_value(m_data.selectedModelArchetypeId);
    pugi::xml_node network = root.append_child("Network");
    network.append_attribute("lastAddress").set_value(m_data.lastAddress.c_str());
    network.append_attribute("port").set_value(m_data.networkPort);
    pugi::xml_node ui = root.append_child("Ui");
    ui.append_attribute("showRemoteNameplate").set_value(m_data.showRemoteNameplate);
    ui.append_attribute("showCoopHud").set_value(m_data.showCoopHud);
    pugi::xml_node diagnostics = root.append_child("Diagnostics");
    diagnostics.append_attribute("verbose").set_value(m_data.verboseDiagnostics);
    pugi::xml_node gameplay = root.append_child("Gameplay");
    gameplay.append_attribute("friendlyFire").set_value(m_data.friendlyFire);
    pugi::xml_node hosting = root.append_child("Hosting");
    hosting.append_attribute("serverName").set_value(m_data.serverName.c_str());
    hosting.append_attribute("maxPlayers").set_value(m_data.maxPlayers);
    hosting.append_attribute("lanVisible").set_value(m_data.lanVisible);
    hosting.append_attribute("accessMode").set_value(m_data.serverAccessMode);
    hosting.append_attribute("password").set_value(m_data.serverPassword.c_str());
    hosting.append_attribute("allowlist").set_value(m_data.serverAllowlist.c_str());
    const auto saveBookmarks = [](pugi::xml_node parent, const std::vector<CoopServerBookmark>& bookmarks)
    {
        for (const CoopServerBookmark& bookmark : bookmarks)
        {
            pugi::xml_node server = parent.append_child("Server");
            server.append_attribute("name").set_value(bookmark.name.c_str());
            server.append_attribute("address").set_value(bookmark.address.c_str());
            server.append_attribute("port").set_value(bookmark.port);
        }
    };
    saveBookmarks(root.append_child("Favorites"), m_data.favoriteServers);
    saveBookmarks(root.append_child("RecentServers"), m_data.recentServers);

    std::filesystem::path tempPath = m_path;
    tempPath += ".tmp";
    std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        m_status = "save failed: write";
        return false;
    }
    document.save(output, "  ", pugi::format_default, pugi::encoding_utf8);
    output.close();
    if (!output || !ReplaceFile(tempPath, m_path))
    {
        m_status = "save failed: write";
        return false;
    }
    m_status = "saved schema " + std::to_string(kCurrentSchemaVersion);
    return true;
}

bool CoopIdentityConfig::RunSelfTest(const std::filesystem::path& root, std::string& detail)
{
    std::error_code error;
    std::filesystem::remove_all(root, error);
    CoopIdentityConfig first;
    if (!first.LoadOrCreate(root, "IdentityTest"))
    {
        detail = "create_failed_" + first.Status();
        return false;
    }
    const uint64_t token = first.AccountToken();
    const std::string accountId = first.Data().accountId;
    first.MutableData().username = "RenamedPlayer";
    first.MutableData().friendlyFire = true;
    first.MutableData().selectedModelArchetypeId = 15115012442011030433ull;
    first.MutableData().lastAddress = "10.20.30.40";
    first.MutableData().networkPort = 28015;
    first.MutableData().serverName = "Identity Test Server";
    first.MutableData().maxPlayers = 8;
    first.MutableData().lanVisible = false;
    first.MutableData().serverAccessMode = 2;
    first.MutableData().serverAllowlist = "0x1234567890ABCDEF";
    first.MutableData().favoriteServers = {{"Favorite", "10.20.30.40", 28015}};
    first.MutableData().recentServers = {{"Recent", "127.0.0.1", 27015}};
    if (!first.Save())
    {
        detail = "rename_save_failed_" + first.Status();
        return false;
    }
    CoopIdentityConfig second;
    if (!second.LoadOrCreate(root, "WrongFallback") ||
        second.AccountToken() != token ||
        second.Data().accountId != accountId ||
        second.Data().username != "RenamedPlayer" ||
        !second.Data().friendlyFire ||
        second.Data().selectedModelArchetypeId != 15115012442011030433ull ||
        second.Data().lastAddress != "10.20.30.40" ||
        second.Data().networkPort != 28015 ||
        second.Data().serverName != "Identity Test Server" ||
        second.Data().maxPlayers != 8 ||
        second.Data().lanVisible ||
        second.Data().serverAccessMode != 2 ||
        second.Data().serverAllowlist != "0x1234567890ABCDEF" ||
        second.Data().favoriteServers.size() != 1 ||
        second.Data().favoriteServers[0].address != "10.20.30.40" ||
        second.Data().favoriteServers[0].port != 28015 ||
        second.Data().recentServers.size() != 1 ||
        second.Data().recentServers[0].name != "Recent")
    {
        detail = "reload_identity_changed";
        return false;
    }

    {
        std::ofstream corrupt(second.Path(), std::ios::binary | std::ios::trunc);
        corrupt << "<broken";
    }
    CoopIdentityConfig recovered;
    if (!recovered.LoadOrCreate(root, "RecoveredPlayer") ||
        recovered.AccountToken() == 0 ||
        !std::filesystem::exists([&]
        {
            std::filesystem::path backup = recovered.Path();
            backup += ".corrupt";
            return backup;
        }()))
    {
        detail = "corruption_recovery_failed_" + recovered.Status();
        return false;
    }

    detail =
        "ok_token_" + std::to_string(token) +
        "_schema_" + std::to_string(second.Data().schemaVersion) +
        "_friendly_fire_1" +
        "_profile_hosting_bookmarks_1" +
        "_corrupt_recovery_1";
    std::filesystem::remove_all(root, error);
    return true;
}
