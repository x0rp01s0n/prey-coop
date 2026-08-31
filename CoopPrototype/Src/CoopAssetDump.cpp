#include "ModMain.h"
#include "CoopFilesystem.h"
#include "CoopRuntimeLog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <io.h>
#include <sstream>
#include <vector>

#include <Chairloader/ChairloaderEnv.h>
#include <Chairloader/IChairLogger.h>
#include <Chairloader/IChairXmlUtils.h>
#include <Prey/CrySystem/File/ICryPak.h>
#include <Prey/CrySystem/ISystem.h>

namespace
{

struct CryPakAssetEntry
{
    std::string path;
    uint64_t size = 0;
};

std::string ToLowerAsciiAsset(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string NormalizeCryPakDirectory(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    while (!value.empty() && value.front() == '/')
        value.erase(value.begin());
    while (!value.empty() && value.back() == '/')
        value.pop_back();
    return value;
}

std::string NormalizeCryPakFilter(std::string value)
{
    if (value == "-" || value == "*" || value == "\"\"" || value == "''")
        return {};
    return ToLowerAsciiAsset(value);
}

std::string SanitizeAssetPathComponent(std::string value)
{
    for (char& ch : value)
    {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (!std::isalnum(uch) && ch != '-' && ch != '_')
            ch = '_';
    }

    while (!value.empty() && value.front() == '_')
        value.erase(value.begin());
    while (!value.empty() && value.back() == '_')
        value.pop_back();

    return value.empty() ? std::string("Assets") : value;
}

void LogAssetDump(const std::string& message)
{
    CoopRuntimeLog::Write(message);
}

std::filesystem::path GetCoopAssetDumpRoot()
{
    std::filesystem::path root = CoopFilesystem::EnvironmentPath("USERPROFILE");
    if (root.empty())
        return std::filesystem::path("CoopPrototype") / "AssetDump";

    root /= "Saved Games";
    root /= "Arkane Studios";
    root /= "Prey";
    root /= "CoopPrototype";
    root /= "AssetDump";
    return root;
}

std::filesystem::path BuildSafeAssetOutputPath(const std::filesystem::path& outputRoot, const std::string& cryPakPath)
{
    std::filesystem::path relative;
    std::string component;
    for (const char ch : cryPakPath)
    {
        if (ch == '/' || ch == '\\')
        {
            if (!component.empty())
            {
                relative /= component;
                component.clear();
            }
            continue;
        }

        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) || ch == '.' || ch == '-' || ch == '_')
            component.push_back(ch);
        else
            component.push_back('_');
    }

    if (!component.empty())
        relative /= component;

    return outputRoot / relative;
}

bool AssetMatchesFilter(const std::string& path, const std::string& lowerFilter)
{
    return lowerFilter.empty() || ToLowerAsciiAsset(path).find(lowerFilter) != std::string::npos;
}

bool TryOpenAnimationsPak(std::string& note)
{
    if (!gEnv || !gEnv->pCryPak)
        return false;

    const unsigned flags =
        ICryArchive::FLAGS_RELATIVE_PATHS_ONLY |
        ICryArchive::FLAGS_OPTIMIZED_READ_ONLY |
        ICryArchive::FLAGS_IN_MEMORY_CPU;

    bool ok = false;
    ok = gEnv->pCryPak->OpenPack("GameSDK/Animations.pak", flags) || ok;
    ok = gEnv->pCryPak->OpenPack("Animations.pak", flags) || ok;
    note = ok ? "opened_animations_pak" : "animations_pak_open_failed";
    return ok;
}

bool EnumerateCryPakAssets(
    const std::string& rootDirectory,
    const std::string& lowerFilter,
    size_t limit,
    std::vector<CryPakAssetEntry>& outEntries,
    uint32_t& outDirsScanned,
    std::string& outError)
{
    outEntries.clear();
    outDirsScanned = 0;
    if (!gEnv || !gEnv->pCryPak)
    {
        outError = "no CryPak";
        return false;
    }

    const std::string normalizedRoot = NormalizeCryPakDirectory(rootDirectory);
    if (normalizedRoot.empty())
    {
        outError = "empty root";
        return false;
    }

    std::deque<std::string> pending;
    pending.push_back(normalizedRoot);

    constexpr uint32_t kMaxDirectories = 8192;
    while (!pending.empty() && outDirsScanned < kMaxDirectories && outEntries.size() < limit)
    {
        std::string directory = pending.front();
        pending.pop_front();
        if (!directory.empty() && directory.back() != '/')
            directory.push_back('/');

        _finddata64i32_t data = {};
        const std::string pattern = directory + "*";
        const int64_t handle = gEnv->pCryPak->FindFirst(pattern.c_str(), &data, 0, true);
        ++outDirsScanned;
        if (handle == -1)
            continue;

        do
        {
            const char* rawName = data.name;
            if (!rawName || !rawName[0] || std::strcmp(rawName, ".") == 0 || std::strcmp(rawName, "..") == 0)
                continue;

            const bool isDirectory = (data.attrib & _A_SUBDIR) != 0;
            std::string child = directory + rawName;
            std::replace(child.begin(), child.end(), '\\', '/');
            if (isDirectory)
            {
                pending.push_back(child);
                continue;
            }

            if (!AssetMatchesFilter(child, lowerFilter))
                continue;

            CryPakAssetEntry entry;
            entry.path = std::move(child);
            entry.size = static_cast<uint64_t>(std::max<int64_t>(0, data.size));
            outEntries.push_back(std::move(entry));
        }
        while (outEntries.size() < limit && gEnv->pCryPak->FindNext(handle, &data) == 0);

        gEnv->pCryPak->FindClose(handle);
    }

    std::sort(
        outEntries.begin(),
        outEntries.end(),
        [](const CryPakAssetEntry& lhs, const CryPakAssetEntry& rhs) { return lhs.path < rhs.path; });
    return true;
}

bool ReadCryPakAssetToMemory(const CryPakAssetEntry& entry, std::vector<char>& outData, uint64_t& copiedBytes)
{
    copiedBytes = 0;
    outData.clear();
    if (!gEnv || !gEnv->pCryPak)
        return false;

    FILE* input = gEnv->pCryPak->FOpen(entry.path.c_str(), "rb", ICryPak::FOPEN_HINT_QUIET);
    if (!input)
        return false;

    if (entry.size > 0 && entry.size < 256ull * 1024ull * 1024ull)
        outData.reserve(static_cast<size_t>(entry.size));

    std::array<char, 64 * 1024> buffer = {};
    for (;;)
    {
        const uint64_t read = gEnv->pCryPak->FReadRaw(buffer.data(), 1, buffer.size(), input);
        if (read == 0)
            break;

        outData.insert(outData.end(), buffer.data(), buffer.data() + static_cast<size_t>(read));
        copiedBytes += read;
    }

    gEnv->pCryPak->FClose(input);
    return copiedBytes == outData.size();
}

bool TryWriteDecodedCryXml(const std::vector<char>& data, const std::filesystem::path& outputPath, bool& decodedXml)
{
    decodedXml = false;
    if (!gCL || !gCL->pXmlUtils || data.empty())
        return false;

    const boost::span<const char> span(data.data(), data.size());
    if (!gCL->pXmlUtils->IsBinaryXml(span))
        return false;

    decodedXml = true;
    pugi::xml_parse_result parseResult;
    pugi::xml_document xmlDoc = gCL->pXmlUtils->LoadXmlFromBuffer(span, &parseResult);
    if (!parseResult)
    {
        CoopRuntimeLog::Write(
            "asset_extract CryXml decode failed out=" + CoopFilesystem::ToUtf8(outputPath) +
            " reason=" + parseResult.description());
        return false;
    }

    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;

    constexpr unsigned kFormatFlags = pugi::format_indent | pugi::format_indent_attributes;
    xmlDoc.save(output, "    ", kFormatFlags);
    return !!output;
}

bool CopyCryPakAssetToDisk(
    const CryPakAssetEntry& entry,
    const std::filesystem::path& outputRoot,
    uint64_t& copiedBytes,
    bool& decodedXml)
{
    decodedXml = false;
    std::vector<char> data;
    if (!ReadCryPakAssetToMemory(entry, data, copiedBytes))
        return false;

    const std::filesystem::path outputPath = BuildSafeAssetOutputPath(outputRoot, entry.path);
    std::error_code error;
    std::filesystem::create_directories(outputPath.parent_path(), error);
    if (error)
        return false;

    bool wasCryXml = false;
    if (TryWriteDecodedCryXml(data, outputPath, wasCryXml))
    {
        decodedXml = true;
        return true;
    }

    if (wasCryXml)
        return false;

    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;

    if (!data.empty())
        output.write(data.data(), static_cast<std::streamsize>(data.size()));
    return !!output;
}

std::string FirstAssetSample(const std::vector<CryPakAssetEntry>& entries)
{
    if (entries.empty())
        return "-";

    std::ostringstream out;
    const size_t sampleCount = std::min<size_t>(entries.size(), 6);
    for (size_t i = 0; i < sampleCount; ++i)
    {
        if (i)
            out << ",";
        out << entries[i].path;
    }
    if (entries.size() > sampleCount)
        out << ",...";
    return out.str();
}

bool WriteAssetManifest(
    const std::filesystem::path& outputRoot,
    const std::string& root,
    const std::string& filter,
    const std::vector<CryPakAssetEntry>& entries,
    uint64_t copiedBytes)
{
    std::error_code error;
    std::filesystem::create_directories(outputRoot, error);
    if (error)
        return false;

    std::ofstream manifest(outputRoot / "manifest.txt", std::ios::trunc);
    if (!manifest)
        return false;

    manifest << "CoopPrototype CryPak asset dump\n";
    manifest << "root=" << root << "\n";
    manifest << "filter=" << (filter.empty() ? "-" : filter) << "\n";
    manifest << "files=" << entries.size() << "\n";
    manifest << "bytes=" << copiedBytes << "\n";
    for (const CryPakAssetEntry& entry : entries)
        manifest << entry.size << "\t" << entry.path << "\n";
    return !!manifest;
}

std::string DumpOpenPakSummary(size_t limit)
{
    if (!gEnv || !gEnv->pCryPak)
        return "asset_paks_failed no CryPak";

    ICryPak::PakInfo* pakInfo = gEnv->pCryPak->GetPakInfo();
    if (!pakInfo)
        return "asset_paks_failed no pak info";

    std::ostringstream out;
    out << "asset_paks count=" << pakInfo->numOpenPaks;
    const size_t count = std::min<size_t>(pakInfo->numOpenPaks, limit);
    for (size_t i = 0; i < count; ++i)
    {
        const ICryPak::PakInfo::Pak& pak = pakInfo->arrPaks[i];
        out << " [" << i << "]=" << (pak.szFilePath ? pak.szFilePath : "-");
    }
    if (pakInfo->numOpenPaks > count)
        out << " ...";

    gEnv->pCryPak->FreePakInfo(pakInfo);
    return out.str();
}

} // namespace

bool ModMain::DebugCryPakAssetCommand(const std::string& command, const std::vector<std::string>& args, std::string& detail)
{
    if (command == "coop_asset_paks")
    {
        const size_t limit = args.empty() ? 32 : static_cast<size_t>(std::clamp(std::atoi(args.front().c_str()), 1, 256));
        detail = DumpOpenPakSummary(limit);
        LogAssetDump(detail);
        return detail.find("_failed") == std::string::npos;
    }

    if (!gEnv || !gEnv->pCryPak)
    {
        detail = "asset command failed: no CryPak";
        LogAssetDump(detail);
        return false;
    }

    const bool extract = command == "coop_asset_extract" || command == "coop_asset_extract_mannequin";
    std::string root = command == "coop_asset_extract_mannequin" ? "Animations/Mannequin" : "Animations/Mannequin";
    std::string filter = command == "coop_asset_extract_mannequin" ? ".xml" : "";
    size_t limit = extract ? 4096 : 256;

    if (command != "coop_asset_extract_mannequin")
    {
        if (!args.empty())
            root = args[0];
        if (args.size() >= 2)
            filter = args[1];
        if (args.size() >= 3)
            limit = static_cast<size_t>(std::clamp(std::atoi(args[2].c_str()), 1, 16384));
    }
    else if (!args.empty())
    {
        limit = static_cast<size_t>(std::clamp(std::atoi(args.front().c_str()), 1, 16384));
    }

    root = NormalizeCryPakDirectory(root);
    const std::string lowerFilter = NormalizeCryPakFilter(filter);

    std::vector<CryPakAssetEntry> entries;
    uint32_t dirsScanned = 0;
    std::string error;
    bool listed = EnumerateCryPakAssets(root, lowerFilter, limit, entries, dirsScanned, error);
    std::string openNote = "-";
    if (listed && entries.empty())
    {
        TryOpenAnimationsPak(openNote);
        listed = EnumerateCryPakAssets(root, lowerFilter, limit, entries, dirsScanned, error);
    }

    if (!listed)
    {
        detail = "asset command failed root=" + root + " reason=" + error;
        LogAssetDump(detail);
        return false;
    }

    if (!extract)
    {
        detail =
            "asset_find root=" + root +
            " filter=" + (lowerFilter.empty() ? std::string("-") : lowerFilter) +
            " dirs=" + std::to_string(dirsScanned) +
            " files=" + std::to_string(entries.size()) +
            " opened=" + openNote +
            " sample=" + FirstAssetSample(entries);
        LogAssetDump(detail);
        return !entries.empty();
    }

    const std::filesystem::path outputRoot = GetCoopAssetDumpRoot() / SanitizeAssetPathComponent(root);
    uint64_t copiedBytes = 0;
    size_t copiedFiles = 0;
    size_t decodedXmlFiles = 0;
    for (const CryPakAssetEntry& entry : entries)
    {
        uint64_t fileBytes = 0;
        bool decodedXml = false;
        if (CopyCryPakAssetToDisk(entry, outputRoot, fileBytes, decodedXml))
        {
            ++copiedFiles;
            if (decodedXml)
                ++decodedXmlFiles;
            copiedBytes += fileBytes;
        }
    }

    const bool manifestOk = WriteAssetManifest(outputRoot, root, lowerFilter, entries, copiedBytes);
    detail =
        "asset_extract root=" + root +
        " filter=" + (lowerFilter.empty() ? std::string("-") : lowerFilter) +
        " dirs=" + std::to_string(dirsScanned) +
        " found=" + std::to_string(entries.size()) +
        " copied=" + std::to_string(copiedFiles) +
        " decodedXml=" + std::to_string(decodedXmlFiles) +
        " bytes=" + std::to_string(copiedBytes) +
        " manifest=" + std::to_string(manifestOk ? 1 : 0) +
        " opened=" + openNote +
        " out=" + CoopFilesystem::ToUtf8(outputRoot) +
        " sample=" + FirstAssetSample(entries);
    LogAssetDump(detail);
    return copiedFiles > 0 && copiedFiles == entries.size() && manifestOk;
}
