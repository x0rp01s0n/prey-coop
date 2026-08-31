#include "CoopPreloadSaveMerge.h"

#include "CoopFilesystem.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace
{
constexpr uint32_t kFnv1aOffsetBasis = 2166136261u;
constexpr uint32_t kFnv1aPrime = 16777619u;
constexpr uint64_t kMaxSlotCopyBytes = 64ull * 1024ull * 1024ull;
constexpr uint32_t kMaxSidePayloadBytes = 32u * 1024u * 1024u;

uint32_t UpdateFnv1a(uint32_t hash, const uint8_t* bytes, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        hash ^= bytes[i];
        hash *= kFnv1aPrime;
    }
    return hash;
}

std::string Hex32(uint32_t value)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

std::string Hex64(uint64_t value)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << value;
    return out.str();
}

std::string StatusToken(std::string value)
{
    if (value.empty())
        return "-";

    for (char& ch : value)
    {
        if (ch <= ' ' || ch == '"' || ch == '\'' || ch == '\\')
            ch = '_';
    }
    return value;
}

bool IsSafeSaveSlotFileName(const std::string& name)
{
    if (name.empty() || name.size() > 128)
        return false;
    if (name == "." || name == "..")
        return false;

    for (char ch : name)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || ch == '/' || ch == '\\' || ch == ':' || ch == '*' ||
            ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|')
        {
            return false;
        }
    }
    return true;
}

bool ComputeFileChecksumAndSize(
    const std::filesystem::path& path,
    uint32_t& outSize,
    uint32_t& outChecksum)
{
    outSize = 0;
    outChecksum = kFnv1aOffsetBasis;

    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
        return false;

    const auto fileSize = std::filesystem::file_size(path, error);
    if (error || fileSize > std::numeric_limits<uint32_t>::max())
        return false;

    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;

    std::array<char, 64 * 1024> buffer = {};
    uint64_t total = 0;
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count <= 0)
            break;
        total += static_cast<uint64_t>(count);
        if (total > std::numeric_limits<uint32_t>::max())
            return false;
        outChecksum = UpdateFnv1a(
            outChecksum,
            reinterpret_cast<const uint8_t*>(buffer.data()),
            static_cast<size_t>(count));
    }

    outSize = static_cast<uint32_t>(total);
    return outSize == static_cast<uint32_t>(fileSize);
}

std::filesystem::path BuildMergedSlotPath(const std::filesystem::path& hostSavePath, uint32_t transferId)
{
    const std::filesystem::path sourceSlot = hostSavePath.parent_path();
    const std::filesystem::path sourceRoot = sourceSlot.parent_path();
    const std::filesystem::path mergeRoot = sourceRoot / "PreloadMerged";

    std::ostringstream name;
    name << CoopFilesystem::ToUtf8(sourceSlot.filename()) << "_merge_"
         << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << transferId;
    return mergeRoot / CoopFilesystem::FromUtf8(name.str());
}

bool CopySlotFiles(
    const std::filesystem::path& sourceSlot,
    const std::filesystem::path& targetSlot,
    CoopPreloadSaveMerge::MergeResult& result)
{
    std::error_code error;
    if (!std::filesystem::is_directory(sourceSlot, error) || error)
    {
        result.reason = "source_slot_missing";
        return false;
    }

    std::filesystem::remove_all(targetSlot, error);
    error.clear();
    std::filesystem::create_directories(targetSlot, error);
    if (error)
    {
        result.reason = "target_slot_mkdir_failed";
        return false;
    }

    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(sourceSlot, error))
    {
        if (error)
        {
            result.reason = "source_slot_iterate_failed";
            return false;
        }

        std::error_code fileError;
        if (!entry.is_regular_file(fileError) || fileError)
            continue;

        const std::string name = CoopFilesystem::ToUtf8(entry.path().filename());
        if (!IsSafeSaveSlotFileName(name))
            continue;

        const auto fileSize = entry.file_size(fileError);
        if (fileError || fileSize > std::numeric_limits<uint32_t>::max())
            continue;

        if (result.copiedBytes + static_cast<uint64_t>(fileSize) > kMaxSlotCopyBytes)
        {
            result.reason = "slot_copy_too_large";
            return false;
        }

        std::filesystem::copy_file(
            entry.path(),
            targetSlot / CoopFilesystem::FromUtf8(name),
            std::filesystem::copy_options::overwrite_existing,
            fileError);
        if (fileError)
        {
            result.reason = "slot_copy_failed_" + name;
            return false;
        }

        ++result.copiedFiles;
        result.copiedBytes += static_cast<uint64_t>(fileSize);
    }

    return true;
}

bool WriteSidecarPayload(const std::filesystem::path& targetSlot, const std::vector<uint8_t>& payload)
{
    if (payload.empty() || payload.size() > kMaxSidePayloadBytes)
        return false;

    std::ofstream output(targetSlot / "coop_player_fragment.cnfp", std::ios::binary | std::ios::trunc);
    if (!output)
        return false;

    output.write(
        reinterpret_cast<const char*>(payload.data()),
        static_cast<std::streamsize>(payload.size()));
    return static_cast<bool>(output);
}

bool WriteNativeSnapshotSave(
    const std::filesystem::path& targetSlot,
    const std::vector<uint8_t>& snapshotSave,
    CoopPreloadSaveMerge::MergeResult& result)
{
    result.nativeSnapshotSavePath = targetSlot / "coop_player_snapshot_save.CSF";
    result.nativeSnapshotSaveBytes = static_cast<uint32_t>(
        std::min<size_t>(snapshotSave.size(), UINT32_MAX));
    result.nativeSnapshotSaveChecksum = 0;

    if (snapshotSave.empty())
        return true;

    if (snapshotSave.size() > kMaxSlotCopyBytes)
    {
        result.reason = "native_snapshot_save_too_large";
        return false;
    }

    std::ofstream output(result.nativeSnapshotSavePath, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        result.reason = "native_snapshot_save_write_failed";
        return false;
    }

    output.write(
        reinterpret_cast<const char*>(snapshotSave.data()),
        static_cast<std::streamsize>(snapshotSave.size()));
    if (!output)
    {
        result.reason = "native_snapshot_save_write_failed";
        return false;
    }

    uint32_t writtenBytes = 0;
    uint32_t writtenChecksum = 0;
    if (!ComputeFileChecksumAndSize(result.nativeSnapshotSavePath, writtenBytes, writtenChecksum))
    {
        result.reason = "native_snapshot_save_verify_failed";
        return false;
    }

    result.wroteNativeSnapshotSave = true;
    result.nativeSnapshotSaveBytes = writtenBytes;
    result.nativeSnapshotSaveChecksum = writtenChecksum;
    return true;
}

bool WriteMergeMeta(
    const CoopPreloadSaveMerge::MergeInput& input,
    const CoopPreloadSaveMerge::MergeResult& result)
{
    std::ofstream meta(result.mergedSlotPath / "coop_preload_merge.meta", std::ios::binary | std::ios::trunc);
    if (!meta)
        return false;

    meta << "version=1\n";
    meta << "mode=package_copy_pending_gamestate_rebuilder\n";
    meta << "patched=" << (result.patched ? 1 : 0) << "\n";
    meta << "transferId=" << input.transferId << "\n";
    meta << "sourceSave=" << CoopFilesystem::ToUtf8(input.hostSavePath) << "\n";
    meta << "hostSaveBytes=" << input.hostSaveBytes << "\n";
    meta << "hostSaveChecksum=" << Hex32(input.hostSaveChecksum) << "\n";
    meta << "outputSave=" << CoopFilesystem::ToUtf8(result.mergedSavePath) << "\n";
    meta << "outputSaveBytes=" << result.outputSaveBytes << "\n";
    meta << "outputSaveChecksum=" << Hex32(result.outputSaveChecksum) << "\n";
    meta << "nativeItems=" << input.nativeFragment.itemGroups << "\n";
    meta << "nativePayloadBytes=" << input.nativeFragment.totalBytes << "\n";
    meta << "nativeSchemaHash=" << Hex64(input.nativeFragment.schemaHash) << "\n";
    meta << "nativeContentHash=" << Hex64(input.nativeFragment.contentHash) << "\n";
    meta << "nativeSnapshotSave=" << (result.wroteNativeSnapshotSave ? 1 : 0) << "\n";
    meta << "nativeSnapshotSavePath=" << CoopFilesystem::ToUtf8(result.nativeSnapshotSavePath) << "\n";
    meta << "nativeSnapshotSaveBytes=" << result.nativeSnapshotSaveBytes << "\n";
    meta << "nativeSnapshotSaveChecksum=" << Hex32(result.nativeSnapshotSaveChecksum) << "\n";
    meta << "nativeRewriteStatus=" << CoopNativeSavePackageRewriter::BuildStatus(result.rewritePlan) << "\n";
    meta << "reason=" << input.reason << "\n";
    return static_cast<bool>(meta);
}
}

namespace CoopPreloadSaveMerge
{
MergeResult BuildPreloadMergedSavePackage(const MergeInput& input)
{
    MergeResult result;
    result.attempted = true;

    if (input.hostSavePath.empty())
    {
        result.reason = "missing_host_save_path";
        return result;
    }
    if (!input.nativeFragment.ok)
    {
        result.reason = "native_fragment_not_ok";
        return result;
    }
    if (input.nativeFragmentBytes.empty())
    {
        result.reason = "native_fragment_bytes_missing";
        return result;
    }

    const std::filesystem::path sourceSlot = input.hostSavePath.parent_path();
    result.mergedSlotPath = BuildMergedSlotPath(input.hostSavePath, input.transferId);
    result.mergedSavePath = result.mergedSlotPath / "save.CSF";

    if (!CopySlotFiles(sourceSlot, result.mergedSlotPath, result))
        return result;

    if (!WriteSidecarPayload(result.mergedSlotPath, input.nativeFragmentBytes))
    {
        result.reason = "native_fragment_payload_write_failed";
        return result;
    }

    if (!WriteNativeSnapshotSave(result.mergedSlotPath, input.nativeSnapshotSaveBytes, result))
        return result;

    if (!ComputeFileChecksumAndSize(result.mergedSavePath, result.outputSaveBytes, result.outputSaveChecksum))
    {
        result.reason = "merged_save_unreadable";
        return result;
    }

    CoopNativeSavePackageRewriter::RewriteInput rewriteInput;
    rewriteInput.packageSavePath = result.mergedSavePath;
    rewriteInput.packageSlotPath = result.mergedSlotPath;
    rewriteInput.nativeFragment = input.nativeFragment;
    rewriteInput.nativeFragmentBytes = input.nativeFragmentBytes;
    rewriteInput.hostSaveBytes = input.hostSaveBytes;
    rewriteInput.hostSaveChecksum = input.hostSaveChecksum;
    rewriteInput.reason = input.reason;
    result.rewritePlan = CoopNativeSavePackageRewriter::BuildRewritePlan(rewriteInput);
    std::string rewriteMetaReason;
    CoopNativeSavePackageRewriter::WriteRewritePlanMeta(
        result.mergedSlotPath / "coop_native_rewriter_plan.meta",
        rewriteInput,
        result.rewritePlan,
        &rewriteMetaReason);

    result.copied = true;
    result.patched = false;

    if (input.requirePatchedPackage)
    {
        result.reason = result.rewritePlan.reason.empty()
            ? "gamestate_rebuilder_missing"
            : result.rewritePlan.reason;
        WriteMergeMeta(input, result);
        return result;
    }

    if (!WriteMergeMeta(input, result))
    {
        result.reason = "merge_meta_write_failed";
        return result;
    }

    result.ok = true;
    result.reason = result.rewritePlan.reason.empty()
        ? "package_copy_ready_gamestate_rebuilder_pending"
        : "package_copy_ready_" + result.rewritePlan.reason;
    return result;
}

std::string BuildStatus(const MergeResult& result)
{
    std::ostringstream out;
    out
        << "attempted=" << (result.attempted ? 1 : 0)
        << "/ok=" << (result.ok ? 1 : 0)
        << "/copied=" << (result.copied ? 1 : 0)
        << "/patched=" << (result.patched ? 1 : 0)
        << "/files=" << result.copiedFiles
        << "/bytes=" << result.copiedBytes
        << "/save=" << result.outputSaveBytes
        << "/crc=" << Hex32(result.outputSaveChecksum)
        << "/nativeSnapshot=" << (result.wroteNativeSnapshotSave ? 1 : 0)
        << "/" << result.nativeSnapshotSaveBytes
        << "/" << Hex32(result.nativeSnapshotSaveChecksum)
        << "/reason=" << StatusToken(result.reason)
        << "/rewrite=" << StatusToken(CoopNativeSavePackageRewriter::BuildStatus(result.rewritePlan))
        << "/path=" << StatusToken(CoopFilesystem::ToUtf8(result.mergedSavePath));
    return out.str();
}
}
