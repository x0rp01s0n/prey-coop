#pragma once

#include "CoopNativeFragmentPayload.h"
#include "CoopNativeSavePackageRewriter.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace CoopPreloadSaveMerge
{
struct MergeInput
{
    std::filesystem::path hostSavePath;
    uint32_t transferId = 0;
    uint32_t hostSaveBytes = 0;
    uint32_t hostSaveChecksum = 0;
    CoopNativeFragmentPayload::ParsedPayload nativeFragment;
    std::vector<uint8_t> nativeFragmentBytes;
    std::vector<uint8_t> nativeSnapshotSaveBytes;
    uint32_t nativeSnapshotSaveChecksum = 0;
    std::string reason;
    bool requirePatchedPackage = false;
};

struct MergeResult
{
    bool attempted = false;
    bool ok = false;
    bool copied = false;
    bool patched = false;
    std::string reason;
    std::filesystem::path mergedSlotPath;
    std::filesystem::path mergedSavePath;
    std::filesystem::path nativeSnapshotSavePath;
    uint32_t outputSaveBytes = 0;
    uint32_t outputSaveChecksum = 0;
    bool wroteNativeSnapshotSave = false;
    uint32_t nativeSnapshotSaveBytes = 0;
    uint32_t nativeSnapshotSaveChecksum = 0;
    uint32_t copiedFiles = 0;
    uint64_t copiedBytes = 0;
    CoopNativeSavePackageRewriter::RewritePlan rewritePlan;
};

MergeResult BuildPreloadMergedSavePackage(const MergeInput& input);
std::string BuildStatus(const MergeResult& result);
}
