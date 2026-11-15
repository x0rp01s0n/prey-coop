#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace CoopFilesystem
{
fs::path FromUtf8(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    return fs::path(value);
}
std::string ToUtf8(const fs::path& value) { return value.string(); }
}

fs::path saveRoot;
int currentCampaign = 1;
std::vector<fs::path> reads;
fs::path GetPreySaveGamesRoot() { return saveRoot; }
bool TryGetCurrentCampaignIdentity(int& slot, uint64_t& guid)
{ slot = currentCampaign; guid = 123; return slot >= 0; }
bool IsReadableFile(const fs::path& path, uint32_t* size)
{
    reads.push_back(path);
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error)
        return false;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    *size = static_cast<uint32_t>(fs::file_size(path));
    return true;
}

#include "NativeSaveResolverActual.inc"

namespace
{
unsigned checks = 0;
void Check(bool condition, const char* label)
{
    ++checks;
    if (!condition)
    {
        std::cerr << "FAIL " << label << '\n';
        std::exit(1);
    }
}
void Write(const fs::path& path, const std::string& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << bytes;
    output.close();
    Check(static_cast<bool>(output), "fixture bytes written");
}
std::string Read(const std::string& path)
{
    std::ifstream input(CoopFilesystem::FromUtf8(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
bool Resolves(const std::string& requested, const fs::path& expected, const std::string& bytes)
{
    std::string result;
    return ResolveReadableSaveFile(requested, result) &&
        CoopFilesystem::FromUtf8(result).lexically_normal() == expected.lexically_normal() && Read(result) == bytes;
}
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    const fs::path fixture = fs::absolute(argv[1]);
    saveRoot = fixture / "Saved Games" / "Prey" / "SaveGames";
    const fs::path a = saveRoot / "Campaign0" / "Office" / "save.CSF";
    const fs::path b = saveRoot / "Campaign1" / "Office" / "save.CSF";
    Write(a, "native-A-exact-bytes");
    Write(b, "native-B-other-campaign");
    Write(saveRoot / "Campaign1" / "Missing" / "save.CSF", "not-a-fallback");
    const fs::path cwd = fixture / "cwd";
    Write(cwd / "%USER%" / "SaveGames" / "Campaign0" / "Office" / "save.CSF", "literal-shadow-must-not-win");
    const fs::path originalCwd = fs::current_path();
    fs::current_path(cwd);

    reads.clear();
    Check(Resolves("%USER%/SaveGames/Campaign0/Office/save.CSF", a, "native-A-exact-bytes"),
        "native final-write alias resolves the exact completed file bytes");
    Check(reads.size() == 1 && reads.front() == a,
        "alias goes directly to its explicit campaign, never literal-shadow or active campaign");
    Check(Resolves("%uSeR%\\sAvEgAmEs\\Campaign0\\Office\\save.CSF", a, "native-A-exact-bytes"),
        "native alias prefix is case insensitive and Windows separators are normalized");
    Check(Resolves("%USER%/SaveGames/Campaign0/Office", a, "native-A-exact-bytes"),
        "native directory alias uses the same production save.CSF candidate logic");
    Check(Resolves("%USER%/SaveGames/Campaign1/Office/save.CSF", b, "native-B-other-campaign"),
        "explicit other campaign remains explicit rather than selecting the first directory");

    reads.clear();
    std::string result = "unchanged";
    Check(!ResolveReadableSaveFile("%USER%/SaveGames/Campaign0/Missing/save.CSF", result),
        "missing exact alias never falls forward to a different campaign's same slot");
    Check(reads.size() == 1 && result == "unchanged", "failed exact alias has no guessed fallback or output mutation");
    for (const char* invalid : {
            "%USER%/SaveGames/", "%USER%/SaveGames//Campaign0/Office/save.CSF",
            "%USER%/SaveGames/../Campaign0/Office/save.CSF", "%USER%/SaveGames/./Campaign0/Office/save.CSF",
            "%USER%/SaveGames/C:/Campaign0/Office/save.CSF"})
    {
        reads.clear();
        Check(!ResolveReadableSaveFile(invalid, result) && reads.empty(),
            "malformed or escaping native alias is rejected before filesystem lookup");
    }

    Check(Resolves("Office", b, "native-B-other-campaign"),
        "legacy bare rotating slot still uses the actual active campaign");
    Check(Resolves("Campaign0/Office", a, "native-A-exact-bytes"),
        "legacy explicit save-root relative path remains supported");
    Check(Resolves(a.string(), a, "native-A-exact-bytes"), "legacy absolute native path remains supported");
    const fs::path savedRoot = saveRoot;
    saveRoot.clear();
    reads.clear();
    Check(!ResolveReadableSaveFile("%USER%/SaveGames/Campaign0/Office/save.CSF", result) && reads.empty(),
        "native alias fails closed when the effective user save root is unavailable");
    Check(Resolves(a.string(), a, "native-A-exact-bytes"),
        "absolute path resolution does not require the profile save root");
    saveRoot = savedRoot;
    Check(!ResolveReadableSaveFile("", result) && !ResolveReadableSaveFile("-", result),
        "empty and unset save identities remain invalid");
    fs::current_path(originalCwd);
    std::cout << checks << " production native-save resolver filesystem checks passed\n";
}
