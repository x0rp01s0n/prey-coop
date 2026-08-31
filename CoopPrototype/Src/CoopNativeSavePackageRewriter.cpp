#include "CoopNativeSavePackageRewriter.h"

#include "CoopFilesystem.h"

#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace
{
constexpr uint32_t kCsfFooterMagicPbx0 = 0x50425830; // "0XPB" little endian in LoadStoreInitFromFile
constexpr uint32_t kCsfFooterMagicPbx1 = 0x50425831;
constexpr uint64_t kMaxProbeSaveBytes = 128ull * 1024ull * 1024ull;

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

template <typename T>
bool ReadLe(const std::array<uint8_t, 64>& bytes, size_t offset, T& out)
{
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
        return false;

    out = 0;
    for (size_t i = 0; i < sizeof(T); ++i)
        out |= static_cast<T>(bytes[offset + i]) << (i * 8);
    return true;
}

bool ProbeCsfPackageShape(const std::filesystem::path& path, uint32_t& outBytes, bool& outHeaderOk, bool& outFooterOk)
{
    outBytes = 0;
    outHeaderOk = false;
    outFooterOk = false;

    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
        return false;

    const uint64_t fileSize = std::filesystem::file_size(path, error);
    if (error || fileSize < 64 || fileSize > kMaxProbeSaveBytes || fileSize > std::numeric_limits<uint32_t>::max())
        return false;

    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;

    std::array<char, 8> header = {};
    input.read(header.data(), static_cast<std::streamsize>(header.size()));
    if (!input)
        return false;

    outHeaderOk =
        header[0] == 'C' &&
        header[1] == 'R' &&
        header[2] == 'Y' &&
        header[3] == '3' &&
        header[4] == 'S' &&
        header[5] == 'D' &&
        header[6] == 'K' &&
        header[7] == '@';

    input.clear();
    input.seekg(static_cast<std::streamoff>(fileSize - 64), std::ios::beg);
    std::array<uint8_t, 64> footer = {};
    input.read(reinterpret_cast<char*>(footer.data()), static_cast<std::streamsize>(footer.size()));
    if (!input)
        return false;

    uint32_t footerMagic = 0;
    const bool rawFooterOk =
        ReadLe(footer, 0, footerMagic) &&
        (footerMagic == kCsfFooterMagicPbx0 || footerMagic == kCsfFooterMagicPbx1);

    // Normal Prey saves are wrapped/encrypted CSF containers. The raw PBX
    // footer only appears after Vanilla's file layer has decoded the stream, so
    // a missing raw footer must not block the native preload path. Scratch
    // LoadStore validation remains the authoritative readback check.
    outFooterOk = rawFooterOk || outHeaderOk;
    outBytes = static_cast<uint32_t>(fileSize);
    return true;
}

bool IsWriteNodeRange(uint32_t kind)
{
    return kind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteNodeBlock);
}

bool IsWriteAttrRange(uint32_t kind)
{
    return kind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteAttrVector);
}

bool IsWriteChildRange(uint32_t kind)
{
    return kind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteChildVector);
}

bool IsWriteRange(uint32_t kind)
{
    return IsWriteNodeRange(kind) || IsWriteAttrRange(kind) || IsWriteChildRange(kind);
}

}

namespace CoopNativeSavePackageRewriter
{
RewritePlan BuildRewritePlan(const RewriteInput& input)
{
    RewritePlan plan;
    plan.attempted = true;
    plan.hostSaveBytes = input.hostSaveBytes;
    plan.hostSaveChecksum = input.hostSaveChecksum;
    plan.payloadBytes = static_cast<uint32_t>(input.nativeFragmentBytes.size());
    plan.sourceItems = input.nativeFragment.itemGroups;
    plan.sourceRanges = input.nativeFragment.rangeRecords;
    plan.oracleRecords = input.nativeFragment.oracleRecordCount;
    plan.schemaHash = input.nativeFragment.schemaHash;
    plan.contentHash = input.nativeFragment.contentHash;

    if (input.packageSavePath.empty())
    {
        plan.reason = "missing_package_path";
        return plan;
    }

    plan.packageReadable = ProbeCsfPackageShape(
        input.packageSavePath,
        plan.packageBytes,
        plan.packageHeaderOk,
        plan.packageFooterOk);
    if (!plan.packageReadable)
    {
        plan.reason = "package_unreadable";
        return plan;
    }
    if (!plan.packageHeaderOk)
    {
        plan.reason = "package_header_not_csf";
        return plan;
    }
    if (!plan.packageFooterOk)
    {
        plan.reason = "package_footer_not_native_pbx";
        return plan;
    }

    if (!input.nativeFragment.ok)
    {
        plan.reason = input.nativeFragment.reason.empty()
            ? "payload_not_ok"
            : "payload_" + input.nativeFragment.reason;
        return plan;
    }
    if (input.nativeFragmentBytes.empty())
    {
        plan.reason = "payload_bytes_missing";
        return plan;
    }

    for (const CoopNativeFragmentPayload::PayloadRangeRecord& range : input.nativeFragment.ranges)
    {
        if (IsWriteNodeRange(range.rangeKind))
            ++plan.sourceWriteNodeRanges;
        else if (IsWriteAttrRange(range.rangeKind))
            ++plan.sourceWriteAttrRanges;
        else if (IsWriteChildRange(range.rangeKind))
            ++plan.sourceWriteChildRanges;
        else if (!IsWriteRange(range.rangeKind))
            ++plan.sourceReadRanges;
    }

    plan.payloadReady = true;
    plan.sourceWriteRangesReady =
        plan.sourceWriteNodeRanges != 0 &&
        plan.sourceWriteAttrRanges != 0;
    if (!plan.sourceWriteRangesReady)
    {
        plan.reason = "payload_missing_native_write_ranges";
        return plan;
    }

    plan.writeNodeOracleReady =
        input.nativeFragment.oracleRecordCount != 0 &&
        input.nativeFragment.oracleRecordCount >= plan.sourceWriteNodeRanges;
    if (!plan.writeNodeOracleReady)
    {
        plan.reason = "payload_missing_write_node_oracle";
        return plan;
    }

    plan.writeBackend = CoopNativeSaveStoreApi::CheckWriteStoreBuilderBackend();
    plan.nativeWriteBackendReady = plan.writeBackend.ok;
    if (!plan.nativeWriteBackendReady)
    {
        plan.reason = "native_write_backend_" +
            (plan.writeBackend.reason.empty() ? std::string("blocked") : plan.writeBackend.reason);
        return plan;
    }

    if (CoopNativeSaveStoreApi::IsScratchActiveSaveWriterProbeEnabled())
        plan.scratchWriter = CoopNativeSaveStoreApi::ProbeScratchActiveSaveWriter();
    if (CoopNativeSaveStoreApi::IsScratchActiveSaveSectionProbeEnabled())
        plan.scratchSectionWriter = CoopNativeSaveStoreApi::ProbeScratchActiveSaveSectionWriter();
    plan.nativeSectionWriterReady =
        plan.nativeWriteBackendReady &&
        (!plan.scratchSectionWriter.attempted || plan.scratchSectionWriter.ok);

    // The remaining missing implementation is not another sidecar replay and
    // not direct ReadStore mutation. The bridge must copy/rebuild the selected
    // host GameState read section into a native write section, transplant the
    // remote player's native fragment while writing, and then let Prey finalize
    // the package through ActiveISaveGame::WriteComplete.
    plan.reason = "native_read_to_write_gamestate_copier_required";
    plan.nativeReadWriteSectionCopierReady = false;
    plan.gameStateSectionRewriteReady = false;
    return plan;
}

std::string BuildStatus(const RewritePlan& plan)
{
    std::ostringstream out;
    out
        << "attempted=" << (plan.attempted ? 1 : 0)
        << "/ok=" << (plan.ok ? 1 : 0)
        << "/pkg=" << (plan.packageReadable ? 1 : 0)
        << "," << (plan.packageHeaderOk ? 1 : 0)
        << "," << (plan.packageFooterOk ? 1 : 0)
        << "/" << plan.packageBytes
        << "/payload=" << (plan.payloadReady ? 1 : 0)
        << "/" << plan.payloadBytes
        << "/items=" << plan.sourceItems
        << "/ranges=" << plan.sourceRanges
        << "/write=" << plan.sourceWriteNodeRanges
        << "," << plan.sourceWriteAttrRanges
        << "," << plan.sourceWriteChildRanges
        << "/read=" << plan.sourceReadRanges
        << "/oracle=" << (plan.writeNodeOracleReady ? 1 : 0)
        << "/" << plan.oracleRecords
        << "/nativeWrite=" << (plan.nativeWriteBackendReady ? 1 : 0)
        << "/sectionWriter=" << (plan.nativeSectionWriterReady ? 1 : 0)
        << "/readWriteCopier=" << (plan.nativeReadWriteSectionCopierReady ? 1 : 0)
        << "/section=" << (plan.gameStateSectionRewriteReady ? 1 : 0)
        << "/schema=" << Hex64(plan.schemaHash)
        << "/content=" << Hex64(plan.contentHash)
        << "/reason=" << StatusToken(plan.reason)
        << "/backend=" << StatusToken(CoopNativeSaveStoreApi::BuildWriteStoreBuilderBackendStatus(plan.writeBackend));
    if (plan.scratchWriter.attempted)
        out << "/scratchWriter=" << StatusToken(CoopNativeSaveStoreApi::BuildScratchActiveSaveWriterProbeStatus(plan.scratchWriter));
    if (plan.scratchSectionWriter.attempted)
        out << "/scratchSection=" << StatusToken(CoopNativeSaveStoreApi::BuildScratchActiveSaveSectionProbeStatus(plan.scratchSectionWriter));
    return out.str();
}

bool WriteRewritePlanMeta(
    const std::filesystem::path& path,
    const RewriteInput& input,
    const RewritePlan& plan,
    std::string* outReason)
{
    std::ofstream meta(path, std::ios::binary | std::ios::trunc);
    if (!meta)
    {
        if (outReason)
            *outReason = "open_failed";
        return false;
    }

    meta << "version=1\n";
    meta << "mode=native_gamestate_section_rewriter_plan\n";
    meta << "packageSave=" << CoopFilesystem::ToUtf8(input.packageSavePath) << "\n";
    meta << "attempted=" << (plan.attempted ? 1 : 0) << "\n";
    meta << "ok=" << (plan.ok ? 1 : 0) << "\n";
    meta << "reason=" << plan.reason << "\n";
    meta << "packageReadable=" << (plan.packageReadable ? 1 : 0) << "\n";
    meta << "packageHeaderOk=" << (plan.packageHeaderOk ? 1 : 0) << "\n";
    meta << "packageFooterOk=" << (plan.packageFooterOk ? 1 : 0) << "\n";
    meta << "packageBytes=" << plan.packageBytes << "\n";
    meta << "hostSaveBytes=" << plan.hostSaveBytes << "\n";
    meta << "hostSaveChecksum=" << Hex32(plan.hostSaveChecksum) << "\n";
    meta << "payloadReady=" << (plan.payloadReady ? 1 : 0) << "\n";
    meta << "payloadBytes=" << plan.payloadBytes << "\n";
    meta << "sourceItems=" << plan.sourceItems << "\n";
    meta << "sourceRanges=" << plan.sourceRanges << "\n";
    meta << "sourceWriteNodeRanges=" << plan.sourceWriteNodeRanges << "\n";
    meta << "sourceWriteAttrRanges=" << plan.sourceWriteAttrRanges << "\n";
    meta << "sourceWriteChildRanges=" << plan.sourceWriteChildRanges << "\n";
    meta << "sourceReadRanges=" << plan.sourceReadRanges << "\n";
    meta << "oracleRecords=" << plan.oracleRecords << "\n";
    meta << "schemaHash=" << Hex64(plan.schemaHash) << "\n";
    meta << "contentHash=" << Hex64(plan.contentHash) << "\n";
    meta << "nativeWriteBackend=" << CoopNativeSaveStoreApi::BuildWriteStoreBuilderBackendStatus(plan.writeBackend) << "\n";
    meta << "nativeSectionWriterReady=" << (plan.nativeSectionWriterReady ? 1 : 0) << "\n";
    meta << "nativeReadWriteSectionCopierReady=" << (plan.nativeReadWriteSectionCopierReady ? 1 : 0) << "\n";
    meta << "gameStateSectionRewriteReady=" << (plan.gameStateSectionRewriteReady ? 1 : 0) << "\n";
    if (plan.scratchWriter.attempted)
        meta << "scratchActiveSaveWriter=" << CoopNativeSaveStoreApi::BuildScratchActiveSaveWriterProbeStatus(plan.scratchWriter) << "\n";
    if (plan.scratchSectionWriter.attempted)
        meta << "scratchActiveSaveSection=" << CoopNativeSaveStoreApi::BuildScratchActiveSaveSectionProbeStatus(plan.scratchSectionWriter) << "\n";
    meta << "status=" << BuildStatus(plan) << "\n";

    if (!meta)
    {
        if (outReason)
            *outReason = "write_failed";
        return false;
    }

    if (outReason)
        outReason->clear();
    return true;
}
}
