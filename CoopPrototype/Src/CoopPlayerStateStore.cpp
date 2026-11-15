#include "CoopPlayerStateStore.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace CoopPlayerStateStore
{
namespace
{
namespace fs = std::filesystem;

bool Hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool SafeKey(const std::string& value)
{
    return !value.empty() && value.size() <= 192 &&
        value != "unknown_save" && value != "checkpoints" &&
        std::all_of(value.begin(), value.end(), [](char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-';
        });
}

bool SafeFingerprint(const std::string& value)
{
    if (value.size() >= 16 && value.size() <= 128 &&
        std::all_of(value.begin(), value.end(), Hex))
        return true;
    const size_t separator = value.find('_');
    if (separator == std::string::npos || separator == 0 || separator > 20 ||
        !std::all_of(value.begin(), value.begin() + separator,
            [](char c) { return c >= '0' && c <= '9'; }))
        return false;
    const std::string hash = value.substr(separator + 1);
    return hash.size() >= 3 && hash.size() <= 18 && hash.compare(0, 2, "0x") == 0 &&
        std::all_of(hash.begin() + 2, hash.end(), Hex);
}

bool AccountFile(const std::string& name)
{
    const std::string prefix = "player_account_";
    const std::string suffix = ".state";
    if (name.size() <= prefix.size() + suffix.size() ||
        name.compare(0, prefix.size(), prefix) != 0 ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
        return false;
    std::string account = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    if (account.compare(0, 2, "0x") == 0)
        account.erase(0, 2);
    if (account.empty() || account.size() > 16 || !std::all_of(account.begin(), account.end(), Hex))
        return false;
    return account.find_first_not_of('0') != std::string::npos;
}

std::string Hex64(uint64_t value)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << value;
    return stream.str();
}

uint64_t NewTimeline(uint64_t previous = 0)
{
    static std::atomic<uint64_t> sequence{0};
    uint64_t value = static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    value ^= ++sequence * 0x9e3779b97f4a7c15ULL;
    try
    {
        std::random_device random;
        value ^= static_cast<uint64_t>(random()) << 32;
        value ^= random();
    }
    catch (...)
    {
    }
    if (value == 0 || value == previous)
        value = previous == UINT64_MAX ? 1 : previous + 1;
    return value;
}

bool Directory(const fs::path& path, bool create, std::string& error)
{
    if (path.empty())
    {
        error = "empty directory path";
        return false;
    }
    std::error_code ec;
    const fs::path absolute = fs::absolute(path, ec).lexically_normal();
    if (ec || absolute.empty())
    {
        error = "invalid directory path";
        return false;
    }
    fs::path current;
    for (const auto& component : absolute)
    {
        current /= component;
        ec.clear();
        fs::file_status status = fs::symlink_status(current, ec);
        if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == fs::file_type::not_found))
        {
            if (!create)
            {
                error = "directory missing";
                return false;
            }
            ec.clear();
            fs::create_directory(current, ec);
            if (ec)
            {
                error = "cannot create directory: " + ec.message();
                return false;
            }
            status = fs::symlink_status(current, ec);
        }
        if (ec || !fs::is_directory(status) || fs::is_symlink(status))
        {
            error = "directory is inaccessible or not a plain directory";
            return false;
        }
    }
    return true;
}

bool PlainFile(const fs::path& path, bool allowMissing, std::string& error)
{
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == fs::file_type::not_found))
    {
        if (allowMissing)
            return true;
        error = "file missing";
        return false;
    }
    if (ec || !fs::is_regular_file(status) || fs::is_symlink(status))
    {
        error = "file is inaccessible or not a plain file";
        return false;
    }
    return true;
}

bool SyncFile(const fs::path& path)
{
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const bool ok = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    return ok;
#else
    const int file = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (file < 0)
        return false;
    const bool ok = fsync(file) == 0;
    close(file);
    return ok;
#endif
}

bool SyncDirectory(const fs::path& path)
{
#ifdef _WIN32
    (void)path;
    return true; // MoveFileEx below requests write-through for the directory entry.
#else
    const int directory = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (directory < 0)
        return false;
    const bool ok = fsync(directory) == 0;
    close(directory);
    return ok;
#endif
}

bool AtomicWrite(const fs::path& destination, const std::string& bytes, std::string& error)
{
    fs::path staged = destination;
    staged += ".tmp";
    if (!Directory(destination.parent_path(), true, error) ||
        !PlainFile(destination, true, error) || !PlainFile(staged, true, error))
        return false;
    {
        std::ofstream output(staged, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output)
        {
            error = "staged file write failed";
            return false;
        }
    }
    if (!SyncFile(staged))
    {
        error = "staged file flush failed";
        return false;
    }
#ifdef _WIN32
    if (!MoveFileExW(staged.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        error = "atomic file replacement failed: " + std::to_string(GetLastError());
        return false;
    }
#else
    std::error_code ec;
    fs::rename(staged, destination, ec);
    if (ec)
    {
        error = "atomic file replacement failed: " + ec.message();
        return false;
    }
#endif
    if (!SyncDirectory(destination.parent_path()))
    {
        error = "directory flush failed";
        return false;
    }
    return true;
}

struct Metadata
{
    uint64_t timeline = 0;
    std::string saveKey;
    std::string fingerprint;
    bool ready = false;
};

std::string MetadataBytes(const Metadata& metadata)
{
    return "version=1\ntimeline=" + Hex64(metadata.timeline) +
        "\nsaveKey=" + metadata.saveKey + "\nfingerprint=" + metadata.fingerprint +
        "\nready=" + (metadata.ready ? "1\n" : "0\n");
}

bool ReadFields(const fs::path& path, std::map<std::string, std::string>& fields,
    std::vector<std::string>* files, std::string& error)
{
    if (!PlainFile(path, false, error))
        return false;
    std::error_code ec;
    if (fs::file_size(path, ec) > 1024 * 1024 || ec)
    {
        error = "metadata file is too large or unreadable";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    std::string line;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const size_t separator = line.find('=');
        if (separator == std::string::npos || separator == 0)
        {
            error = "malformed metadata line";
            return false;
        }
        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);
        if (files && key == "file")
            files->push_back(value);
        else if (!fields.emplace(key, value).second)
        {
            error = "duplicate metadata field";
            return false;
        }
    }
    if (!input.eof())
    {
        error = "metadata read failed";
        return false;
    }
    return true;
}

bool ReadMetadata(const fs::path& path, Metadata& metadata, std::string& error)
{
    std::map<std::string, std::string> fields;
    if (!ReadFields(path, fields, nullptr, error))
        return false;
    if (fields.size() != 5 || fields["version"] != "1" ||
        !SafeKey(fields["saveKey"]) || !SafeFingerprint(fields["fingerprint"]) ||
        (fields["ready"] != "0" && fields["ready"] != "1"))
    {
        error = "invalid timeline metadata";
        return false;
    }
    const std::string& timeline = fields["timeline"];
    const auto parsed = std::from_chars(timeline.data(), timeline.data() + timeline.size(), metadata.timeline, 16);
    if (timeline.size() != 16 || parsed.ec != std::errc() ||
        parsed.ptr != timeline.data() + timeline.size() || metadata.timeline == 0)
    {
        error = "invalid timeline identifier";
        return false;
    }
    metadata.saveKey = fields["saveKey"];
    metadata.fingerprint = fields["fingerprint"];
    metadata.ready = fields["ready"] == "1";
    return true;
}

bool AccountFiles(const fs::path& directory, std::vector<std::string>& files,
    const Validator& validate, bool strict, std::string& error)
{
    if (!Directory(directory, false, error))
        return false;
    std::error_code ec;
    std::set<uint64_t> accounts;
    fs::directory_iterator iterator(directory, ec), end;
    while (!ec && iterator != end)
    {
        const fs::path path = iterator->path();
        const fs::file_status status = iterator->symlink_status(ec);
        if (ec || !fs::is_regular_file(status) || fs::is_symlink(status))
        {
            error = "state directory contains a non-plain file";
            return false;
        }
        const std::string name = path.filename().string();
        if (!AccountFile(name))
        {
            if (strict)
            {
                error = "checkpoint contains an unexpected filename";
                return false;
            }
        }
        else
        {
            constexpr size_t prefixLength = sizeof("player_account_") - 1;
            constexpr size_t suffixLength = sizeof(".state") - 1;
            std::string account = name.substr(prefixLength, name.size() - prefixLength - suffixLength);
            if (account.compare(0, 2, "0x") == 0)
                account.erase(0, 2);
            uint64_t token = 0;
            std::from_chars(account.data(), account.data() + account.size(), token, 16);
            if (!accounts.insert(token).second)
            {
                error = "state directory contains duplicate account identities";
                return false;
            }
            if (!validate(path, error))
            {
                error = "invalid account state " + name + ": " + error;
                return false;
            }
            files.push_back(name);
        }
        iterator.increment(ec);
    }
    if (ec)
    {
        error = "cannot enumerate account states: " + ec.message();
        return false;
    }
    std::sort(files.begin(), files.end());
    return true;
}

bool CopyAccounts(const fs::path& source, const fs::path& destination,
    const std::vector<std::string>& files, const Validator& validate, std::string& error)
{
    for (const std::string& name : files)
    {
        const fs::path from = source / name;
        const fs::path to = destination / name;
        if (!PlainFile(from, false, error) || !PlainFile(to, true, error))
            return false;
        std::error_code ec;
        fs::copy_file(from, to, fs::copy_options::none, ec);
        if (ec || !validate(to, error) || !SyncFile(to))
        {
            error = "account state copy failed " + name + ": " + (ec ? ec.message() : error);
            return false;
        }
    }
    if (!SyncDirectory(destination))
    {
        error = "account state directory flush failed";
        return false;
    }
    return true;
}

bool NewDirectory(const fs::path& parent, const char* prefix, fs::path& result, std::string& error)
{
    if (!Directory(parent, true, error))
        return false;
    for (unsigned attempt = 0; attempt < 8; ++attempt)
    {
        result = parent / (std::string(prefix) + Hex64(NewTimeline()));
        std::error_code ec;
        if (fs::create_directory(result, ec))
            return true;
        if (ec)
        {
            error = "cannot stage state directory: " + ec.message();
            return false;
        }
    }
    error = "cannot allocate unique state generation";
    return false;
}

bool ReadCheckpoint(const fs::path& checkpoint, const std::string& fingerprint,
    fs::path& generation, std::vector<std::string>& files, const Validator& validate, std::string& error)
{
    if (!Directory(checkpoint, false, error))
        return false;
    std::map<std::string, std::string> fields;
    if (!ReadFields(checkpoint / "manifest.state", fields, &files, error))
        return false;
    const std::string& name = fields["generation"];
    if (fields.size() != 4 || fields["version"] != "1" ||
        fields["fingerprint"] != fingerprint || !SafeKey(fields["saveKey"]) ||
        name.size() != 20 || name.compare(0, 4, "gen_") != 0 ||
        !std::all_of(name.begin() + 4, name.end(), Hex))
    {
        error = "invalid checkpoint manifest";
        return false;
    }
    std::set<std::string> unique;
    for (const std::string& file : files)
    {
        if (!AccountFile(file) || !unique.insert(file).second)
        {
            error = "invalid or duplicate checkpoint account filename";
            return false;
        }
    }
    generation = checkpoint / name;
    std::vector<std::string> actual;
    if (!AccountFiles(generation, actual, validate, true, error))
        return false;
    std::sort(files.begin(), files.end());
    if (files != actual)
    {
        error = "checkpoint account manifest does not match generation";
        return false;
    }
    return true;
}
}

Store::Store(std::filesystem::path hostRoot, Validator validator)
    : m_root(std::move(hostRoot)), m_validator(std::move(validator))
{
}

bool MatchesTimeline(uint64_t currentTimeline, uint64_t incomingTimeline,
    uint32_t currentEpoch, uint32_t incomingEpoch, bool loading)
{
    return !loading && currentTimeline != 0 && currentTimeline == incomingTimeline &&
        currentEpoch != 0 && currentEpoch == incomingEpoch;
}

std::filesystem::path Store::LiveDirectory(const std::string& saveKey) const
{
    return !m_root.empty() && SafeKey(saveKey) ? m_root / saveKey : fs::path();
}

std::filesystem::path Store::CheckpointDirectory(const std::string& fingerprint) const
{
    return !m_root.empty() && SafeFingerprint(fingerprint) ? m_root / "checkpoints" / fingerprint : fs::path();
}

bool Store::Open(const std::string& saveKey, const std::string& fingerprint, std::string& error)
{
    error.clear();
    m_timeline = 0;
    m_ready = false;
    m_recoveryAllowed = false;
    if (!m_validator || !SafeKey(saveKey) || !SafeFingerprint(fingerprint) || !Directory(m_root, true, error))
    {
        if (error.empty())
            error = "invalid player-state store identity or validator";
        return false;
    }
    Metadata metadata;
    const fs::path metadataPath = m_root / "active_timeline.state";
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(metadataPath, ec);
    const bool missing = ec == std::errc::no_such_file_or_directory ||
        (!ec && status.type() == fs::file_type::not_found);
    if (missing)
    {
        metadata = {NewTimeline(), saveKey, fingerprint, true};
        if (!Directory(LiveDirectory(saveKey), true, error) ||
            !AtomicWrite(metadataPath, MetadataBytes(metadata), error))
            return false;
        m_timeline = metadata.timeline;
        m_saveKey = saveKey;
        m_fingerprint = fingerprint;
        m_ready = true;
        return true;
    }
    if (!ReadMetadata(metadataPath, metadata, error))
    {
        const bool restored = Rebase(saveKey, fingerprint, error);
        m_recoveryAllowed = false;
        return restored;
    }
    if (!metadata.ready || metadata.saveKey != saveKey || metadata.fingerprint != fingerprint)
    {
        m_timeline = metadata.timeline;
        return Rebase(saveKey, fingerprint, error);
    }
    if (!Directory(LiveDirectory(saveKey), true, error))
        return false;
    m_timeline = metadata.timeline;
    m_saveKey = saveKey;
    m_fingerprint = fingerprint;
    m_ready = true;
    m_recoveryAllowed = true;
    error.clear();
    return true;
}

bool Store::AdvanceLiveSave(const std::string& previousSaveKey, const std::string& nextSaveKey, std::string& error)
{
    error.clear();
    const auto fail = [this, &error](const std::string& message)
    {
        error = message;
        m_timeline = 0;
        m_ready = false;
        m_recoveryAllowed = false;
        return false;
    };
    if (!m_ready || m_timeline == 0 || m_saveKey != previousSaveKey ||
        !SafeKey(previousSaveKey) || !SafeKey(nextSaveKey))
        return fail("forward live save requires the current initialized source scope");
    if (previousSaveKey == nextSaveKey)
        return true;

    const fs::path source = LiveDirectory(previousSaveKey);
    if (!Directory(source, true, error))
        return fail(error);
    std::vector<std::string> files;
    if (!AccountFiles(source, files, m_validator, false, error))
        return fail(error);
    fs::path staged;
    if (!NewDirectory(m_root, "advance_", staged, error) ||
        !CopyAccounts(source, staged, files, m_validator, error))
        return fail(error);

    const fs::path destination = LiveDirectory(nextSaveKey);
    const fs::path retired = m_root / ("retired_" + Hex64(NewTimeline(m_timeline)));
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(destination, ec);
    const bool exists = !ec && status.type() != fs::file_type::not_found;
    if ((ec && ec != std::errc::no_such_file_or_directory) ||
        (exists && (!fs::is_directory(status) || fs::is_symlink(status))))
        return fail("forward live destination is inaccessible or not a plain directory");
    if (exists)
    {
        ec.clear();
        fs::rename(destination, retired, ec);
        if (ec)
            return fail("cannot retire forward live destination: " + ec.message());
    }
    ec.clear();
    fs::rename(staged, destination, ec);
    if (ec)
        return fail("cannot publish forward live state: " + ec.message());
    if (!SyncDirectory(m_root))
        return fail("forward live directory flush failed");
    if (exists)
        fs::remove_all(retired, ec);
    return true;
}

void Store::DiscardStagedCheckpoint()
{
    std::error_code ignored;
    if (!m_stagedCheckpoint.empty())
        fs::remove_all(m_stagedCheckpoint, ignored);
    m_stagedCheckpoint.clear();
    m_stagedTimeline = 0;
}

bool Store::StageCheckpoint(const std::string& saveKey, std::string& error)
{
    error.clear();
    if (!m_ready || m_timeline == 0 || !m_validator || !SafeKey(saveKey) || !m_stagedCheckpoint.empty())
    {
        error = "checkpoint staging requires an initialized timeline and no pending snapshot";
        return false;
    }
    const fs::path source = LiveDirectory(saveKey);
    std::vector<std::string> files;
    if (!Directory(source, true, error) || !AccountFiles(source, files, m_validator, false, error))
        return false;
    fs::path staged;
    if (!NewDirectory(m_root, "pending_checkpoint_", staged, error))
        return false;
    if (!CopyAccounts(source, staged, files, m_validator, error))
    {
        std::error_code ignored;
        fs::remove_all(staged, ignored);
        return false;
    }
    m_stagedCheckpoint = std::move(staged);
    m_stagedTimeline = m_timeline;
    return true;
}

bool Store::CaptureCheckpoint(const std::string& saveKey, const std::string& fingerprint, std::string& error)
{
    error.clear();
    if (!m_ready || m_timeline == 0 || !m_validator || !SafeKey(saveKey) || !SafeFingerprint(fingerprint))
    {
        error = "checkpoint capture requires an initialized live timeline";
        return false;
    }
    if (!m_stagedCheckpoint.empty() && m_stagedTimeline != m_timeline)
    {
        error = "staged checkpoint belongs to an abandoned timeline";
        return false;
    }
    const fs::path source = m_stagedCheckpoint.empty() ? LiveDirectory(saveKey) : m_stagedCheckpoint;
    const fs::path checkpoint = CheckpointDirectory(fingerprint);
    if (!Directory(source, true, error) || !Directory(checkpoint, true, error))
        return false;
    std::vector<std::string> files;
    if (!AccountFiles(source, files, m_validator, false, error))
        return false;
    fs::path generation;
    if (!NewDirectory(checkpoint, "gen_", generation, error) ||
        !CopyAccounts(source, generation, files, m_validator, error))
        return false;
    std::string manifest = "version=1\nfingerprint=" + fingerprint +
        "\nsaveKey=" + saveKey + "\ngeneration=" + generation.filename().string() + "\n";
    for (const std::string& file : files)
        manifest += "file=" + file + "\n";
    if (!AtomicWrite(checkpoint / "manifest.state", manifest, error))
        return false;
    const Metadata metadata{m_timeline, saveKey, fingerprint, true};
    if (!AtomicWrite(m_root / "active_timeline.state", MetadataBytes(metadata), error))
    {
        m_timeline = 0;
        m_ready = false;
        m_recoveryAllowed = false;
        return false;
    }
    m_saveKey = saveKey;
    m_fingerprint = fingerprint;
    m_recoveryAllowed = true;
    DiscardStagedCheckpoint();
    return true;
}

bool Store::Rebase(const std::string& saveKey, const std::string& fingerprint, std::string& error)
{
    DiscardStagedCheckpoint();
    error.clear();
    const uint64_t previousTimeline = m_timeline;
    m_timeline = 0;
    m_ready = false;
    m_recoveryAllowed = false;
    if (!m_validator || !SafeKey(saveKey) || !SafeFingerprint(fingerprint) || !Directory(m_root, true, error))
    {
        if (error.empty())
            error = "invalid checkpoint restore identity or validator";
        return false;
    }
    Metadata metadata{NewTimeline(previousTimeline), saveKey, fingerprint, false};
    if (!AtomicWrite(m_root / "active_timeline.state", MetadataBytes(metadata), error))
        return false;

    fs::path generation;
    std::vector<std::string> files;
    std::string checkpointError;
    const bool hasCheckpoint = ReadCheckpoint(CheckpointDirectory(fingerprint), fingerprint,
        generation, files, m_validator, checkpointError);
    fs::path staged;
    if (!NewDirectory(m_root, "restore_", staged, error))
        return false;
    if (hasCheckpoint && !CopyAccounts(generation, staged, files, m_validator, error))
        return false;

    const fs::path live = LiveDirectory(saveKey);
    const fs::path retired = m_root / ("retired_" + Hex64(metadata.timeline));
    std::error_code ec;
    const fs::file_status liveStatus = fs::symlink_status(live, ec);
    const bool hasLive = !ec && liveStatus.type() != fs::file_type::not_found;
    if ((ec && ec != std::errc::no_such_file_or_directory) ||
        (hasLive && (!fs::is_directory(liveStatus) || fs::is_symlink(liveStatus))))
    {
        error = "live state is inaccessible or not a plain directory";
        return false;
    }
    if (hasLive)
    {
        ec.clear();
        fs::rename(live, retired, ec);
        if (ec)
        {
            error = "cannot retire previous live state: " + ec.message();
            return false;
        }
    }
    ec.clear();
    fs::rename(staged, live, ec);
    if (ec)
    {
        error = "cannot publish restored live state: " + ec.message();
        return false;
    }
    if (!SyncDirectory(m_root))
    {
        error = "restored live directory flush failed";
        return false;
    }
    metadata.ready = true;
    if (!AtomicWrite(m_root / "active_timeline.state", MetadataBytes(metadata), error))
        return false;
    m_timeline = metadata.timeline;
    m_saveKey = saveKey;
    m_fingerprint = fingerprint;
    m_ready = true;
    m_recoveryAllowed = true;
    if (hasLive)
        fs::remove_all(retired, ec);
    if (!hasCheckpoint)
    {
        error = "checkpoint unavailable; live state reset empty: " + checkpointError;
        return false;
    }
    return true;
}

bool Store::RotateTimeline(const std::string& saveKey, const std::string& fingerprint, std::string& error)
{
    return Rebase(saveKey, fingerprint, error);
}

bool Store::RestoreCheckpoint(const std::string& saveKey, const std::string& fingerprint, std::string& error)
{
    return Rebase(saveKey, fingerprint, error);
}
}
