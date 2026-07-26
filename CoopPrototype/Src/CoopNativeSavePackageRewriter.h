#pragma once

#include "CoopNativeFragmentPayload.h"
#include "CoopNativeSaveStoreApi.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace CoopNativeSavePackageRewriter
{
struct RewriteInput
{
    std::filesystem::path packageSavePath;
    std::filesystem::path packageSlotPath;
    CoopNativeFragmentPayload::ParsedPayload nativeFragment;
    std::vector<uint8_t> nativeFragmentBytes;
    uint32_t hostSaveBytes = 0;
    uint32_t hostSaveChecksum = 0;
    std::string reason;
};

struct RewritePlan
{
    bool attempted = false;
    bool ok = false;
    bool packageReadable = false;
    bool packageHeaderOk = false;
    bool packageFooterOk = false;
    bool payloadReady = false;
    bool sourceWriteRangesReady = false;
    bool writeNodeOracleReady = false;
    bool nativeWriteBackendReady = false;
    bool nativeSectionWriterReady = false;
    bool nativeReadWriteSectionCopierReady = false;
    bool gameStateSectionRewriteReady = false;
    std::string reason;
    uint32_t packageBytes = 0;
    uint32_t hostSaveBytes = 0;
    uint32_t hostSaveChecksum = 0;
    uint32_t payloadBytes = 0;
    uint32_t sourceItems = 0;
    uint32_t sourceRanges = 0;
    uint32_t sourceWriteNodeRanges = 0;
    uint32_t sourceWriteAttrRanges = 0;
    uint32_t sourceWriteChildRanges = 0;
    uint32_t sourceReadRanges = 0;
    uint32_t oracleRecords = 0;
    uint64_t schemaHash = 0;
    uint64_t contentHash = 0;
    CoopNativeSaveStoreApi::WriteStoreBuilderBackendStatus writeBackend;
    CoopNativeSaveStoreApi::ScratchActiveSaveWriterProbeResult scratchWriter;
    CoopNativeSaveStoreApi::ScratchActiveSaveSectionProbeResult scratchSectionWriter;
};

RewritePlan BuildRewritePlan(const RewriteInput& input);
std::string BuildStatus(const RewritePlan& plan);
bool WriteRewritePlanMeta(
    const std::filesystem::path& path,
    const RewriteInput& input,
    const RewritePlan& plan,
    std::string* outReason = nullptr);
}
