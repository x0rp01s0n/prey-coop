#include "CoopNativeFragmentApply.h"

#include "CoopNativeSaveStoreApi.h"
#include "CoopRuntimeConfig.h"
#include "CoopRuntimeGuards.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace
{
using CoopRuntimeGuards::PreflightRuntimePointer;
using CoopRuntimeGuards::RuntimeAccess;
using CoopRuntimeGuards::TryReadRuntimeValue;
using CoopRuntimeGuards::TryWriteRuntimeValue;

constexpr size_t kReadStoreNodeBytes = 0x50;
constexpr size_t kWriteAttrRecordBytes = 0x28;

struct WriteAttrRecord
{
    uint32_t type = 0;
    uint32_t nameIndex = 0;
    uint64_t sourceContext = 0;
    uint8_t inlineBytes[16] = {};
};

struct AttrValueMemcpyContext
{
    void* target = nullptr;
    const void* source = nullptr;
    size_t size = 0;
};

bool EnvFlagEnabled(const char* name)
{
    return CoopRuntimeConfig::Flag(name);
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

std::string CountMismatchReason(
    const char* prefix,
    uint32_t sourceRangeIndex,
    uint32_t targetNodeIndex,
    uint32_t sourceCount,
    uint32_t targetCount)
{
    std::ostringstream out;
    out << prefix
        << "_r" << sourceRangeIndex
        << "_tN" << targetNodeIndex
        << "_s" << sourceCount
        << "_t" << targetCount;
    return out.str();
}

CoopNativeSaveStoreApi::StoreHandle MakeStoreHandle(
    const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node)
{
    CoopNativeSaveStoreApi::StoreHandle handle;
    handle.store = node.storePtr;
    handle.nodeBlockBegin = node.nodeBlockBegin;
    handle.nodeBlockEnd = node.nodeBlockEnd;
    TryReadRuntimeValue(
        reinterpret_cast<const std::uintptr_t*>(handle.store + 0x20),
        handle.attrDataBase);
    handle.attrStringBase = node.attrStringBase;
    handle.attrNameOffsetTable = node.attrNameOffsetTable;
    handle.attrTokenContext = node.attrTokenContext;
    handle.attrTokenIndexBase = node.attrTokenIndexBase;
    handle.attrTokenBase = node.attrTokenBase;
    handle.childNameDataBase = node.childNameDataBase;
    handle.childNameResolverContext = node.childNameResolverContext;
    handle.childNameOffsetTable = node.childNameOffsetTable;
    handle.nodeCount = node.nodeCount;
    handle.readStore = node.readStore;
    handle.valid =
        node.valid &&
        node.readStore &&
        node.storePtr != 0 &&
        node.nodeBlockBegin != 0 &&
        node.nodeBlockEnd > node.nodeBlockBegin &&
        node.nodeCount != 0 &&
        node.nodePtr >= node.nodeBlockBegin &&
        node.nodePtr + kReadStoreNodeBytes <= node.nodeBlockEnd;
    return handle;
}

bool IsWriteAttrVectorRange(uint32_t rangeKind)
{
    return rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteAttrVector);
}

bool IsWriteChildVectorRange(uint32_t rangeKind)
{
    return rangeKind == static_cast<uint32_t>(CoopNativeFragmentPayload::PayloadRangeKind::WriteChildVector);
}

bool TryGetNativeAttrPayloadByteSize(uint32_t type, int32_t& outBytes, std::string& reason)
{
    outBytes = -1;
    if (!CoopNativeSaveStoreApi::TryGetAttrTypePayloadSize(type, outBytes, &reason) || outBytes < 0)
    {
        if (reason.empty())
            reason = "native_attr_type_size_unavailable";
        return false;
    }
    return true;
}

bool TryGetRangeBytes(
    const CoopNativeFragmentPayload::ParsedPayload& source,
    uint32_t rangeIndex,
    const CoopNativeFragmentPayload::PayloadRangeRecord*& outRange,
    const uint8_t*& outBytes,
    size_t& outSize,
    std::string& reason)
{
    outRange = nullptr;
    outBytes = nullptr;
    outSize = 0;
    if (rangeIndex >= source.ranges.size())
    {
        reason = "source_range_out_of_bounds";
        return false;
    }

    const CoopNativeFragmentPayload::PayloadRangeRecord& range = source.ranges[rangeIndex];
    if (range.byteOffset > source.rawData.size() ||
        range.byteCount > source.rawData.size() - range.byteOffset)
    {
        reason = "source_range_raw_bounds";
        return false;
    }

    outRange = &range;
    outBytes = source.rawData.data() + range.byteOffset;
    outSize = range.byteCount;
    return true;
}

bool TryReadWriteAttrRecord(
    const uint8_t* bytes,
    size_t size,
    uint32_t ordinal,
    WriteAttrRecord& outRecord)
{
    outRecord = {};
    const size_t offset = static_cast<size_t>(ordinal) * kWriteAttrRecordBytes;
    if (!bytes || offset > size || kWriteAttrRecordBytes > size - offset)
        return false;

    std::memcpy(&outRecord.type, bytes + offset, sizeof(outRecord.type));
    std::memcpy(&outRecord.nameIndex, bytes + offset + 0x4, sizeof(outRecord.nameIndex));
    std::memcpy(&outRecord.sourceContext, bytes + offset + 0x8, sizeof(outRecord.sourceContext));
    std::memcpy(outRecord.inlineBytes, bytes + offset + 0x10, sizeof(outRecord.inlineBytes));
    return true;
}

const CoopNativeFragmentPayload::PayloadAttrNameRecord* FindSourceAttrName(
    const CoopNativeFragmentPayload::ParsedPayload& source,
    uint32_t rangeIndex,
    uint32_t ordinal)
{
    for (const CoopNativeFragmentPayload::PayloadAttrNameRecord& record : source.attrNames)
    {
        if (record.rangeIndex == rangeIndex && record.ordinal == ordinal)
            return &record;
    }
    return nullptr;
}

bool IsMaterializerInputAttrVector(const uint8_t* bytes, size_t size, uint32_t& outRecordCount)
{
    outRecordCount = 0;
    if (!bytes || size == 0 || (size % kWriteAttrRecordBytes) != 0)
        return false;

    const uint32_t recordCount = static_cast<uint32_t>(size / kWriteAttrRecordBytes);
    for (uint32_t ordinal = 0; ordinal < recordCount; ++ordinal)
    {
        WriteAttrRecord sourceAttr;
        if (!TryReadWriteAttrRecord(bytes, size, ordinal, sourceAttr))
            return false;

        // Type 0 write records are not finalized read-store attributes. They
        // are materializer input and must not be matched by target attr count.
        if (sourceAttr.type != 0)
            return false;
    }

    outRecordCount = recordCount;
    return true;
}

bool ShouldPreserveTargetIdentityAttribute(const std::string& sourceName)
{
    // Same-shape V0 reuses the target item/entity identity. Source-side
    // identity attrs belong to the later remap/insert materializer layer; if
    // we require them to exist or overwrite them here, we can corrupt the
    // target read-store identity graph before Vanilla deserialize runs.
    if (sourceName == "UniqueId")
        return true;

    // Runtime pause/lerp bookkeeping can be emitted by the source write
    // serializer when an item is mid-state, but absent from a clean target
    // read node. It is not inventory content and should not block the first
    // same-shape data proof.
    return
        sourceName == "PauseStart" ||
        sourceName == "PauseEnd" ||
        sourceName == "PauseDuration";
}

const CoopNativeGameStateFragmentLocator::FragmentNodeRef* FindTargetNodeByIndex(
    const CoopNativeGameStateFragmentLocator::NativeInventoryFragmentBundle& target,
    uint32_t nodeIndex)
{
    auto matches = [nodeIndex](const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node)
    {
        return node.valid && node.readStore && node.nodeIndex == nodeIndex;
    };

    if (matches(target.inventoryCellValueNode))
        return &target.inventoryCellValueNode;
    for (const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node : target.inventoryObservedNodes)
    {
        if (matches(node))
            return &node;
    }
    for (const CoopNativeGameStateFragmentLocator::ItemFragmentRef& item : target.itemFragments)
    {
        if (matches(item.node))
            return &item.node;
        for (const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node : item.observedNodes)
        {
            if (matches(node))
                return &node;
        }
    }
    return nullptr;
}

bool TryComputeReadAttrCursorForOrdinal(
    const CoopNativeSaveStoreApi::StoreHandle& store,
    uint32_t attrValueCursor,
    uint32_t attrTokenCursor,
    uint32_t attrCount,
    uint32_t ordinal,
    uint32_t& outDataCursor,
    uint16_t& outToken,
    std::string& reason)
{
    if (!store.valid || !store.readStore || store.attrDataBase == 0)
    {
        reason = "target_attr_data_pool_missing";
        return false;
    }
    if (ordinal >= attrCount)
    {
        reason = "target_attr_ordinal_out_of_bounds";
        return false;
    }

    uint32_t cursor = attrValueCursor;
    for (uint32_t i = 0; i <= ordinal; ++i)
    {
        uint16_t token = 0;
        if (!CoopNativeSaveStoreApi::TryReadAttrNameToken(store, attrTokenCursor, i, token, &reason))
            return false;

        const uint32_t type = static_cast<uint32_t>(token & 0x3Fu);
        if (i == ordinal)
        {
            outDataCursor = cursor;
            outToken = token;
            return true;
        }

        if (type == 6)
        {
            uint32_t stringBytes = 0;
            const std::uintptr_t sizeAddress = store.attrDataBase + cursor;
            if (!PreflightRuntimePointer(
                    "native fragment apply string attr size",
                    reinterpret_cast<const void*>(sizeAddress),
                    sizeof(uint32_t),
                    RuntimeAccess::Read,
                    &reason) ||
                !TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(sizeAddress), stringBytes))
            {
                if (reason.empty())
                    reason = "string_attr_size_read_failed";
                return false;
            }
            if (stringBytes > 4u * 1024u * 1024u || cursor > UINT32_MAX - stringBytes - sizeof(uint32_t))
            {
                reason = "string_attr_size_out_of_bounds";
                return false;
            }
            cursor += sizeof(uint32_t) + stringBytes;
            continue;
        }

        int32_t bytes = -1;
        if (!TryGetNativeAttrPayloadByteSize(type, bytes, reason))
        {
            if (reason.empty())
                reason = "unsupported_target_attr_type";
            return false;
        }
        cursor += static_cast<uint32_t>(bytes);
    }

    reason = "target_attr_cursor_walk_failed";
    return false;
}

bool TryFindReadAttrByName(
    const CoopNativeSaveStoreApi::StoreHandle& store,
    uint32_t attrValueCursor,
    uint32_t attrTokenCursor,
    uint32_t attrCount,
    const std::string& sourceName,
    uint32_t preferredOrdinal,
    uint32_t& outOrdinal,
    uint32_t& outDataCursor,
    uint16_t& outToken,
    std::string& reason)
{
    if (!store.valid || !store.readStore || store.attrDataBase == 0)
    {
        reason = "target_attr_data_pool_missing";
        return false;
    }
    if (sourceName.empty())
    {
        reason = "source_attr_name_empty";
        return false;
    }

    auto tryOrdinal =
        [&](uint32_t ordinal, uint32_t dataCursor, uint16_t token) -> bool
        {
            const char* targetNamePtr = nullptr;
            std::string localReason;
            if (!CoopNativeSaveStoreApi::TryResolveAttrName(store, dataCursor, token, targetNamePtr, &localReason) ||
                !targetNamePtr)
            {
                reason = localReason.empty() ? "target_attr_name_resolve_failed" : localReason;
                return false;
            }

            const std::string targetName = CoopRuntimeGuards::ReadRuntimeCString(targetNamePtr, 256);
            if (targetName != sourceName)
                return false;

            outOrdinal = ordinal;
            outDataCursor = dataCursor;
            outToken = token;
            return true;
        };

    uint32_t cursor = attrValueCursor;
    for (uint32_t ordinal = 0; ordinal < attrCount; ++ordinal)
    {
        uint16_t token = 0;
        if (!CoopNativeSaveStoreApi::TryReadAttrNameToken(store, attrTokenCursor, ordinal, token, &reason))
            return false;

        if (ordinal == preferredOrdinal && tryOrdinal(ordinal, cursor, token))
        {
            return true;
        }

        const uint32_t type = static_cast<uint32_t>(token & 0x3Fu);
        if (type == 6)
        {
            uint32_t stringBytes = 0;
            const std::uintptr_t sizeAddress = store.attrDataBase + cursor;
            if (!PreflightRuntimePointer(
                    "native fragment apply string attr size",
                    reinterpret_cast<const void*>(sizeAddress),
                    sizeof(uint32_t),
                    RuntimeAccess::Read,
                    &reason) ||
                !TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(sizeAddress), stringBytes))
            {
                if (reason.empty())
                    reason = "string_attr_size_read_failed";
                return false;
            }
            if (stringBytes > 4u * 1024u * 1024u || cursor > UINT32_MAX - stringBytes - sizeof(uint32_t))
            {
                reason = "string_attr_size_out_of_bounds";
                return false;
            }
            cursor += sizeof(uint32_t) + stringBytes;
            continue;
        }

        int32_t bytes = -1;
        if (!TryGetNativeAttrPayloadByteSize(type, bytes, reason))
        {
            if (reason.empty())
                reason = "unsupported_target_attr_type";
            return false;
        }
        cursor += static_cast<uint32_t>(bytes);
    }

    cursor = attrValueCursor;
    for (uint32_t ordinal = 0; ordinal < attrCount; ++ordinal)
    {
        uint16_t token = 0;
        if (!CoopNativeSaveStoreApi::TryReadAttrNameToken(store, attrTokenCursor, ordinal, token, &reason))
            return false;

        if (tryOrdinal(ordinal, cursor, token))
            return true;

        const uint32_t type = static_cast<uint32_t>(token & 0x3Fu);
        if (type == 6)
        {
            uint32_t stringBytes = 0;
            const std::uintptr_t sizeAddress = store.attrDataBase + cursor;
            if (!PreflightRuntimePointer(
                    "native fragment apply string attr size",
                    reinterpret_cast<const void*>(sizeAddress),
                    sizeof(uint32_t),
                    RuntimeAccess::Read,
                    &reason) ||
                !TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(sizeAddress), stringBytes))
            {
                if (reason.empty())
                    reason = "string_attr_size_read_failed";
                return false;
            }
            if (stringBytes > 4u * 1024u * 1024u || cursor > UINT32_MAX - stringBytes - sizeof(uint32_t))
            {
                reason = "string_attr_size_out_of_bounds";
                return false;
            }
            cursor += sizeof(uint32_t) + stringBytes;
            continue;
        }

        int32_t bytes = -1;
        if (!TryGetNativeAttrPayloadByteSize(type, bytes, reason))
        {
            if (reason.empty())
                reason = "unsupported_target_attr_type";
            return false;
        }
        cursor += static_cast<uint32_t>(bytes);
    }

    reason = "target_attr_name_not_found";
    return false;
}

bool TryRewriteAttrTokenType(
    const CoopNativeSaveStoreApi::StoreHandle& store,
    uint32_t attrTokenCursor,
    uint32_t ordinal,
    uint32_t sourceType,
    CoopNativeFragmentApply::ApplyResult& result)
{
    if (sourceType > 0x3Fu)
    {
        result.reason = "source_attr_type_out_of_bounds";
        ++result.guards;
        return false;
    }

    CoopNativeSaveStoreApi::AttrTokenView view;
    std::string reason;
    if (!CoopNativeSaveStoreApi::TryReadAttrTokenView(store, attrTokenCursor, ordinal, view, &reason))
    {
        result.reason = reason.empty() ? "target_attr_token_read_failed" : reason;
        ++result.guards;
        return false;
    }

    const uint16_t rewritten = static_cast<uint16_t>((view.token & ~0x3Fu) | static_cast<uint16_t>(sourceType));
    if (rewritten == view.token)
        return true;

    if (!PreflightRuntimePointer(
            "native fragment apply attr token type",
            reinterpret_cast<void*>(view.tokenAddress),
            sizeof(uint16_t),
            RuntimeAccess::Write,
            &result.reason) ||
        !TryWriteRuntimeValue(reinterpret_cast<uint16_t*>(view.tokenAddress), rewritten))
    {
        if (result.reason.empty())
            result.reason = "attr_token_type_write_failed";
        ++result.guards;
        return false;
    }

    uint16_t verify = 0;
    if (!TryReadRuntimeValue(reinterpret_cast<const uint16_t*>(view.tokenAddress), verify) || verify != rewritten)
    {
        result.reason = "attr_token_type_verify_failed";
        ++result.guards;
        return false;
    }

    ++result.attrTokenWrites;
    return true;
}

bool TryWriteAttrValueBytes(
    const CoopNativeSaveStoreApi::StoreHandle& store,
    uint32_t dataCursor,
    const WriteAttrRecord& sourceAttr,
    uint32_t byteCount,
    CoopNativeFragmentApply::ApplyResult& result)
{
    if (byteCount == 0)
        return true;
    if (byteCount > sizeof(sourceAttr.inlineBytes))
    {
        result.reason = "source_attr_inline_value_too_small";
        ++result.attrUnsupported;
        return false;
    }

    const std::uintptr_t targetAddress = store.attrDataBase + dataCursor;
    if (targetAddress < store.attrDataBase)
    {
        result.reason = "target_attr_data_address_overflow";
        ++result.guards;
        return false;
    }

    if (!PreflightRuntimePointer(
            "native fragment apply attr value",
            reinterpret_cast<void*>(targetAddress),
            byteCount,
            RuntimeAccess::Write,
            &result.reason))
    {
        ++result.guards;
        return false;
    }

    const auto* sourceBytes = sourceAttr.inlineBytes;
    auto* targetBytes = reinterpret_cast<uint8_t*>(targetAddress);
    AttrValueMemcpyContext context { targetBytes, sourceBytes, byteCount };
    if (!CoopRuntimeGuards::TryRunGuardedCallback(
            "native fragment apply attr value memcpy",
            [](void* rawContext) -> bool
            {
                auto* context = static_cast<AttrValueMemcpyContext*>(rawContext);
                std::memcpy(context->target, context->source, context->size);
                return true;
            },
            &context,
            &result.reason))
    {
        if (result.reason.empty())
            result.reason = "attr_value_write_failed";
        ++result.guards;
        return false;
    }

    ++result.attrValueWrites;
    return true;
}

bool IsSameShapeMaterializerSafe(
    const CoopNativeFragmentMaterializer::MaterializerPlan& materializerPlan,
    CoopNativeFragmentApply::ApplyResult& result)
{
    if (materializerPlan.insertNodeOps != 0 ||
        materializerPlan.insertAttrOps != 0 ||
        materializerPlan.insertChildOps != 0 ||
        materializerPlan.allocationOps != 0 ||
        materializerPlan.unsafeOps != 0 ||
        materializerPlan.unsupportedOps != 0 ||
        materializerPlan.transcodeOps != 0 ||
        materializerPlan.oracleMissingOps != 0 ||
        materializerPlan.fullTransplantOps != 0 ||
        materializerPlan.itemRemapOps != 0 ||
        materializerPlan.ownerRemapOps != 0)
    {
        result.reason = "not_same_shape_apply_safe";
        return false;
    }
    return true;
}

bool TryApplyAttrVectorOp(
    const CoopNativeFragmentPayload::ParsedPayload& source,
    const CoopNativeGameStateFragmentLocator::NativeInventoryFragmentBundle& target,
    const CoopNativeFragmentMaterializer::MaterializedStorePatchOp& op,
    CoopNativeFragmentApply::ApplyResult& result,
    bool writeValues)
{
    const CoopNativeFragmentPayload::PayloadRangeRecord* range = nullptr;
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    if (!TryGetRangeBytes(source, op.sourceRangeIndex, range, bytes, size, result.reason))
    {
        ++result.guards;
        return false;
    }
    (void)bytes;
    if (!range || !IsWriteAttrVectorRange(range->rangeKind))
    {
        result.reason = "source_attr_range_not_write_vector";
        ++result.attrUnsupported;
        return false;
    }
    if ((size % kWriteAttrRecordBytes) != 0)
    {
        result.reason = "source_attr_vector_size_mismatch";
        ++result.attrUnsupported;
        return false;
    }

    const uint32_t recordCount = static_cast<uint32_t>(size / kWriteAttrRecordBytes);
    if (op.sourceCount != 0 && op.sourceCount != recordCount)
    {
        result.reason = "source_attr_op_count_mismatch";
        ++result.attrUnsupported;
        return false;
    }

    uint32_t materializerInputCount = 0;
    const bool materializerInputAttrVector =
        IsMaterializerInputAttrVector(bytes, size, materializerInputCount);

    const CoopNativeGameStateFragmentLocator::FragmentNodeRef* node =
        FindTargetNodeByIndex(target, op.targetNodeIndex);
    if (!node)
    {
        result.reason = "target_attr_node_missing";
        ++result.guards;
        return false;
    }
    if (materializerInputAttrVector)
    {
        // This source range still belongs to the write-store/finalizer side.
        // V1 can validate the target node and preserve it, but it must not
        // pretend source write materializer input records are complete read
        // attributes just because a target count happens to match.
        result.attrPreservedNameTokens += materializerInputCount;
        return true;
    }

    const CoopNativeSaveStoreApi::StoreHandle store = MakeStoreHandle(*node);
    if (!store.valid)
    {
        result.reason = "target_attr_store_invalid";
        ++result.guards;
        return false;
    }

    for (uint32_t ordinal = 0; ordinal < recordCount; ++ordinal)
    {
        WriteAttrRecord sourceAttr;
        if (!TryReadWriteAttrRecord(bytes, size, ordinal, sourceAttr))
        {
            result.reason = "source_attr_record_read_failed";
            ++result.attrUnsupported;
            return false;
        }

        if (sourceAttr.type == 6)
        {
            ++result.attrPreservedNameTokens;
            continue;
        }

        const CoopNativeFragmentPayload::PayloadAttrNameRecord* sourceName =
            FindSourceAttrName(source, op.sourceRangeIndex, ordinal);
        if (!sourceName || sourceName->name.empty())
        {
            result.reason = "source_attr_name_missing";
            ++result.attrUnsupported;
            return false;
        }
        if (sourceName->type != sourceAttr.type || sourceName->sourceNameIndex != sourceAttr.nameIndex)
        {
            result.reason = "source_attr_name_metadata_mismatch";
            ++result.attrUnsupported;
            return false;
        }
        if (ShouldPreserveTargetIdentityAttribute(sourceName->name))
        {
            ++result.attrPreservedNameTokens;
            continue;
        }

        int32_t sourceBytes = -1;
        if (!TryGetNativeAttrPayloadByteSize(sourceAttr.type, sourceBytes, result.reason))
        {
            if (result.reason.empty())
                result.reason = "unsupported_source_attr_type";
            ++result.attrUnsupported;
            return false;
        }

        uint32_t targetOrdinal = 0;
        uint32_t targetDataCursor = 0;
        uint16_t targetToken = 0;
        if (!TryFindReadAttrByName(
                store,
                node->attrCursor,
                node->nodeId,
                node->attrCount,
                sourceName->name,
                ordinal,
                targetOrdinal,
                targetDataCursor,
                targetToken,
                result.reason))
        {
            result.reason =
                StatusToken(result.reason) +
                "_r" + std::to_string(op.sourceRangeIndex) +
                "_o" + std::to_string(ordinal) +
                "_name_" + StatusToken(sourceName->name);
            ++result.guards;
            return false;
        }

        const uint32_t targetType = static_cast<uint32_t>(targetToken & 0x3Fu);
        int32_t targetBytes = -1;
        if (!TryGetNativeAttrPayloadByteSize(targetType, targetBytes, result.reason))
        {
            if (result.reason.empty())
                result.reason = "unsupported_target_attr_type";
            ++result.attrUnsupported;
            return false;
        }
        if (targetBytes != sourceBytes)
        {
            result.reason = "attr_type_size_mismatch";
            ++result.attrUnsupported;
            return false;
        }

        if (!writeValues)
            continue;

        if (!TryRewriteAttrTokenType(store, node->nodeId, targetOrdinal, sourceAttr.type, result))
            return false;
        if (!TryWriteAttrValueBytes(store, targetDataCursor, sourceAttr, static_cast<uint32_t>(sourceBytes), result))
            return false;
    }

    return true;
}

bool TryPreserveChildVectorOp(
    const CoopNativeFragmentPayload::ParsedPayload& source,
    const CoopNativeGameStateFragmentLocator::NativeInventoryFragmentBundle& target,
    const CoopNativeFragmentMaterializer::MaterializedStorePatchOp& op,
    CoopNativeFragmentApply::ApplyResult& result)
{
    const CoopNativeFragmentPayload::PayloadRangeRecord* range = nullptr;
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    if (!TryGetRangeBytes(source, op.sourceRangeIndex, range, bytes, size, result.reason))
    {
        ++result.guards;
        return false;
    }
    if (!range || !IsWriteChildVectorRange(range->rangeKind))
    {
        result.reason = "source_child_range_not_write_vector";
        ++result.attrUnsupported;
        return false;
    }
    if ((size % sizeof(uint32_t)) != 0)
    {
        result.reason = "source_child_vector_size_mismatch";
        ++result.attrUnsupported;
        return false;
    }

    const uint32_t recordCount = static_cast<uint32_t>(size / sizeof(uint32_t));
    if (op.sourceCount != 0 && op.sourceCount != recordCount)
    {
        result.reason = "source_child_op_count_mismatch";
        ++result.attrUnsupported;
        return false;
    }

    const CoopNativeGameStateFragmentLocator::FragmentNodeRef* node =
        FindTargetNodeByIndex(target, op.targetNodeIndex);
    if (!node)
    {
        result.reason = "target_child_node_missing";
        ++result.guards;
        return false;
    }
    if (node->childCount != recordCount)
    {
        result.reason = CountMismatchReason(
            "target_child_count_mismatch",
            op.sourceRangeIndex,
            op.targetNodeIndex,
            recordCount,
            node->childCount);
        ++result.attrUnsupported;
        return false;
    }
    if (node->childCount != 0)
    {
        const std::uintptr_t bytesInTarget =
            node->childIndexBlockEnd > node->childIndexBlockBegin
                ? node->childIndexBlockEnd - node->childIndexBlockBegin
                : 0;
        if (node->childIndexBlockBegin == 0 || bytesInTarget < static_cast<std::uintptr_t>(recordCount) * sizeof(uint32_t))
        {
            result.reason = "target_child_index_block_missing";
            ++result.guards;
            return false;
        }

        if (!PreflightRuntimePointer(
                "native fragment apply preserve child vector",
                reinterpret_cast<const void*>(node->childIndexBlockBegin),
                static_cast<size_t>(recordCount) * sizeof(uint32_t),
                RuntimeAccess::Read,
                &result.reason))
        {
            ++result.guards;
            return false;
        }
    }

    result.childIndexPreserved += recordCount;
    return true;
}

bool ValidateSameShapePatchOps(
    const CoopNativeFragmentPayload::ParsedPayload& source,
    const CoopNativeGameStateFragmentLocator::NativeInventoryFragmentBundle& target,
    const CoopNativeFragmentMaterializer::MaterializerPlan& materializerPlan,
    CoopNativeFragmentApply::ApplyResult& result)
{
    using Kind = CoopNativeFragmentMaterializer::MaterializedStorePatchOp::Kind;

    CoopNativeFragmentApply::ApplyResult validation = result;
    for (const CoopNativeFragmentMaterializer::MaterializedStorePatchOp& op : materializerPlan.patchOps)
    {
        if (!op.safe || op.requiresAllocation)
        {
            result.reason = "unsafe_patch_op";
            return false;
        }

        switch (op.kind)
        {
        case Kind::ReplaceExistingReadNode:
            if (!FindTargetNodeByIndex(target, op.targetNodeIndex))
            {
                result.reason = "target_node_missing";
                ++result.guards;
                return false;
            }
            break;
        case Kind::ReplaceAttrRecord:
            if (!TryApplyAttrVectorOp(source, target, op, validation, false))
            {
                result.reason = validation.reason;
                result.guards += validation.guards;
                result.attrUnsupported += validation.attrUnsupported;
                return false;
            }
            break;
        case Kind::ReplaceChildRecord:
            if (!TryPreserveChildVectorOp(source, target, op, validation))
            {
                result.reason = validation.reason;
                result.guards += validation.guards;
                result.attrUnsupported += validation.attrUnsupported;
                return false;
            }
            break;
        case Kind::RebaseAttrCursor:
        case Kind::RebaseChildCursor:
            break;
        case Kind::InsertReadNode:
        case Kind::InsertAttrRecord:
        case Kind::InsertChildRecord:
        case Kind::RemapItemEntityId:
        case Kind::RemapOwnerId:
            result.reason = "unsupported_patch_op";
            ++result.attrUnsupported;
            return false;
        }
    }

    return true;
}

template <typename T>
bool RewriteSameValue(uint8_t* base, size_t offset, const char* operation, CoopNativeFragmentApply::ApplyResult& result)
{
    T value {};
    T verify {};
    T* field = reinterpret_cast<T*>(base + offset);
    if (!PreflightRuntimePointer(operation, field, sizeof(T), RuntimeAccess::Write, &result.reason))
    {
        ++result.guards;
        return false;
    }
    if (!TryReadRuntimeValue(reinterpret_cast<const T*>(field), value) ||
        !TryWriteRuntimeValue(field, value) ||
        !TryReadRuntimeValue(reinterpret_cast<const T*>(field), verify) ||
        verify != value)
    {
        result.reason = std::string(operation) + "_failed";
        ++result.guards;
        return false;
    }
    return true;
}

bool RewriteSameNode(
    const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node,
    CoopNativeFragmentApply::ApplyResult& result)
{
    if (!node.valid || !node.readStore || node.nodePtr == 0)
    {
        result.reason = "invalid_target_node";
        ++result.guards;
        return false;
    }

    if (!PreflightRuntimePointer(
            "native fragment apply read-store node",
            reinterpret_cast<void*>(node.nodePtr),
            kReadStoreNodeBytes,
            RuntimeAccess::Write,
            &result.reason))
    {
        ++result.guards;
        return false;
    }

    uint8_t* base = reinterpret_cast<uint8_t*>(node.nodePtr);
    if (!RewriteSameValue<uint32_t>(base, 0x10, "native fragment apply node id", result) ||
        !RewriteSameValue<uint32_t>(base, 0x18, "native fragment apply node child cursor", result) ||
        !RewriteSameValue<uint32_t>(base, 0x1C, "native fragment apply node child count", result) ||
        !RewriteSameValue<uint32_t>(base, 0x20, "native fragment apply node attr count", result) ||
        !RewriteSameValue<uint32_t>(base, 0x24, "native fragment apply node attr cursor", result) ||
        !RewriteSameValue<uint8_t>(base, 0x48, "native fragment apply node valid", result))
    {
        return false;
    }
    result.nodeWrites += 6;

    const CoopNativeSaveStoreApi::StoreHandle store = MakeStoreHandle(node);
    if (node.attrCount != 0)
    {
        if (!store.valid || store.attrTokenContext == 0 || store.attrTokenIndexBase == 0 || store.attrTokenBase == 0)
        {
            result.reason = "target_attr_token_pools_missing";
            ++result.guards;
            return false;
        }

        const uint32_t attrLimit = std::min<uint32_t>(node.attrCount, 256u);
        for (uint32_t ordinal = 0; ordinal < attrLimit; ++ordinal)
        {
            std::string reason;
            if (!CoopNativeSaveStoreApi::TryRewriteSameAttrNameToken(
                    store,
                    node.nodeId,
                    ordinal,
                    nullptr,
                    &reason))
            {
                result.reason = reason.empty() ? "attr_token_rewrite_failed" : reason;
                ++result.guards;
                return false;
            }
            ++result.attrTokenWrites;
        }
    }

    if (node.childCount != 0 && node.childIndexBlockBegin != 0 && node.childIndexBlockEnd > node.childIndexBlockBegin)
    {
        const std::uintptr_t bytes = node.childIndexBlockEnd - node.childIndexBlockBegin;
        if (bytes > 0x10000u || (bytes % sizeof(uint32_t)) != 0)
        {
            result.reason = "child_index_block_outside_guarded_range";
            ++result.guards;
            return false;
        }

        uint32_t* begin = reinterpret_cast<uint32_t*>(node.childIndexBlockBegin);
        const uint32_t count = static_cast<uint32_t>(bytes / sizeof(uint32_t));
        for (uint32_t i = 0; i < count; ++i)
        {
            uint32_t value = 0;
            uint32_t verify = 0;
            if (!PreflightRuntimePointer(
                    "native fragment apply child index",
                    begin + i,
                    sizeof(uint32_t),
                    RuntimeAccess::Write,
                    &result.reason) ||
                !TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(begin + i), value) ||
                !TryWriteRuntimeValue(begin + i, value) ||
                !TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(begin + i), verify) ||
                verify != value)
            {
                if (result.reason.empty())
                    result.reason = "child_index_same_value_write_failed";
                ++result.guards;
                return false;
            }
            ++result.childIndexWrites;
        }
    }

    return true;
}

void AddNode(
    std::vector<CoopNativeGameStateFragmentLocator::FragmentNodeRef>& nodes,
    std::unordered_set<std::uintptr_t>& seen,
    const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node)
{
    if (!node.valid || node.nodePtr == 0)
        return;
    if (seen.insert(node.nodePtr).second)
        nodes.push_back(node);
}
}

namespace CoopNativeFragmentApply
{
ApplyResult TryApplySameShapeNoop(
    const CoopNativeFragmentPayload::ParsedPayload& source,
    const CoopNativeGameStateFragmentLocator::NativeInventoryFragmentBundle& target,
    const CoopNativeFragmentMaterializer::MaterializerPlan& materializerPlan)
{
    ApplyResult result;
    result.attempted = true;
    result.noop = true;
    result.enabled = EnvFlagEnabled("COOP_NATIVE_FRAGMENT_APPLY_SAME_SHAPE_NOOP");
    if (!result.enabled)
    {
        result.reason = "disabled";
        return result;
    }

    if (!CoopNativeSaveStoreApi::IsWriteApiEnabled())
    {
        result.reason = "write_api_disabled";
        return result;
    }
    if (!source.ok)
    {
        result.reason = "source_payload_not_ok";
        return result;
    }
    if (!target.ok)
    {
        result.reason = "target_not_ok";
        return result;
    }
    if (!materializerPlan.ok)
    {
        result.reason = "materializer_not_ok";
        return result;
    }
    if (materializerPlan.insertNodeOps != 0 ||
        materializerPlan.insertAttrOps != 0 ||
        materializerPlan.insertChildOps != 0 ||
        materializerPlan.allocationOps != 0 ||
        materializerPlan.unsafeOps != 0 ||
        materializerPlan.unsupportedOps != 0)
    {
        result.reason = "not_same_shape_noop_safe";
        return result;
    }

    std::vector<CoopNativeGameStateFragmentLocator::FragmentNodeRef> nodes;
    std::unordered_set<std::uintptr_t> seen;
    AddNode(nodes, seen, target.inventoryCellValueNode);
    for (const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node : target.inventoryObservedNodes)
        AddNode(nodes, seen, node);
    for (const CoopNativeGameStateFragmentLocator::ItemFragmentRef& item : target.itemFragments)
    {
        AddNode(nodes, seen, item.node);
        for (const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node : item.observedNodes)
            AddNode(nodes, seen, node);
    }

    result.targetNodes = static_cast<uint32_t>(std::min<size_t>(nodes.size(), UINT32_MAX));
    if (nodes.empty())
    {
        result.reason = "no_target_nodes";
        return result;
    }

    for (const CoopNativeGameStateFragmentLocator::FragmentNodeRef& node : nodes)
    {
        if (!RewriteSameNode(node, result))
            return result;
    }

    result.ok = true;
    result.reason = "same_shape_noop_applied";
    return result;
}

ApplyResult TryApplySameShapeNumericAttributes(
    const CoopNativeFragmentPayload::ParsedPayload& source,
    const CoopNativeGameStateFragmentLocator::NativeInventoryFragmentBundle& target,
    const CoopNativeFragmentMaterializer::MaterializerPlan& materializerPlan)
{
    using Kind = CoopNativeFragmentMaterializer::MaterializedStorePatchOp::Kind;

    ApplyResult result;
    result.attempted = true;
    result.enabled = EnvFlagEnabled("COOP_NATIVE_FRAGMENT_APPLY_SAME_SHAPE_REBASE");
    if (!result.enabled)
    {
        result.reason = "disabled";
        return result;
    }

    if (!CoopNativeSaveStoreApi::IsWriteApiEnabled())
    {
        result.reason = "write_api_disabled";
        return result;
    }
    if (!source.ok)
    {
        result.reason = "source_payload_not_ok";
        return result;
    }
    if (!target.ok)
    {
        result.reason = "target_not_ok";
        return result;
    }
    if (!materializerPlan.attempted)
    {
        result.reason = "materializer_not_attempted";
        return result;
    }
    if (!materializerPlan.ok || materializerPlan.reason != "same_shape_readonly")
    {
        result.reason = "materializer_not_same_shape_readonly";
        return result;
    }
    if (!IsSameShapeMaterializerSafe(materializerPlan, result))
        return result;
    if (!ValidateSameShapePatchOps(source, target, materializerPlan, result))
        return result;

    std::unordered_set<uint32_t> seenTargetNodes;
    for (const CoopNativeFragmentMaterializer::MaterializedStorePatchOp& op : materializerPlan.patchOps)
    {
        if (!op.safe || op.requiresAllocation)
        {
            result.reason = "unsafe_patch_op";
            return result;
        }

        switch (op.kind)
        {
        case Kind::ReplaceExistingReadNode:
        {
            const CoopNativeGameStateFragmentLocator::FragmentNodeRef* node =
                FindTargetNodeByIndex(target, op.targetNodeIndex);
            if (!node)
            {
                result.reason = "target_node_missing";
                ++result.guards;
                return result;
            }

            if (seenTargetNodes.insert(op.targetNodeIndex).second)
                ++result.targetNodes;
            if (!RewriteSameNode(*node, result))
                return result;
            break;
        }
        case Kind::ReplaceAttrRecord:
            if (!TryApplyAttrVectorOp(source, target, op, result, true))
                return result;
            break;
        case Kind::ReplaceChildRecord:
            if (!TryPreserveChildVectorOp(source, target, op, result))
                return result;
            break;
        case Kind::RebaseAttrCursor:
        case Kind::RebaseChildCursor:
            // V0 applies into existing target cursors. Cursor ops are validated
            // by the corresponding attr/child op and do not write source cursors.
            break;
        case Kind::InsertReadNode:
        case Kind::InsertAttrRecord:
        case Kind::InsertChildRecord:
        case Kind::RemapItemEntityId:
        case Kind::RemapOwnerId:
            result.reason = "unsupported_patch_op";
            ++result.attrUnsupported;
            return result;
        }
    }

    result.ok = true;
    result.reason = "same_shape_numeric_attrs_applied";
    return result;
}

ApplyResult TryApplyFullTransplantPlan(
    const CoopNativeFragmentPayload::ParsedPayload& source,
    const CoopNativeGameStateFragmentLocator::NativeInventoryFragmentBundle& target,
    const CoopNativeFragmentMaterializer::MaterializerPlan& materializerPlan)
{
    ApplyResult result;
    result.attempted = true;
    result.enabled = EnvFlagEnabled("COOP_NATIVE_FRAGMENT_APPLY_FULL_TRANSPLANT");
    result.fullTransplantOps = materializerPlan.fullTransplantOps;
    result.allocationOps = materializerPlan.allocationOps;
    result.entityAllocationOps = materializerPlan.entityAllocationOps;
    result.missingSubtreeInsertOps = materializerPlan.missingSubtreeInsertOps;
    result.newItemInsertOps = materializerPlan.newItemInsertOps;
    result.remapOps = materializerPlan.itemRemapOps + materializerPlan.ownerRemapOps;
    result.transcodeOps = materializerPlan.transcodeOps;
    if (!result.enabled)
    {
        result.reason = "disabled";
        return result;
    }

    if (!CoopNativeSaveStoreApi::IsWriteApiEnabled())
    {
        result.reason = "write_api_disabled";
        return result;
    }
    if (!source.ok)
    {
        result.reason = "source_payload_not_ok";
        return result;
    }
    if (!target.ok)
    {
        result.reason = "target_not_ok";
        return result;
    }
    if (materializerPlan.fullTransplantOps == 0)
    {
        result.enabled = false;
        result.reason = materializerPlan.ok
            ? "same_shape_not_full_transplant"
            : "full_transplant_plan_empty";
        return result;
    }
    if (!EnvFlagEnabled("COOP_NATIVE_FRAGMENT_ALLOW_READ_STORE_FULL_TRANSPLANT"))
    {
        result.reason = "full_transplant_read_store_apply_disabled_preload_save_merge_required";
        return result;
    }
    if (materializerPlan.unsupportedOps != 0)
    {
        result.reason = "full_transplant_unsupported_ops";
        result.attrUnsupported = materializerPlan.unsupportedOps;
        return result;
    }
    if (materializerPlan.entityAllocationOps != 0)
    {
        result.reason = "full_transplant_entity_id_allocator_missing";
        return result;
    }
    if (materializerPlan.allocationOps != 0)
    {
        result.reason =
            materializerPlan.missingSubtreeInsertOps != 0 && materializerPlan.newItemInsertOps == 0
                ? "full_transplant_missing_subtree_read_store_allocator_missing"
                : "full_transplant_read_store_allocator_missing";
        return result;
    }
    if (materializerPlan.transcodeOps != 0)
    {
        result.reason = "full_transplant_write_to_read_transcoder_missing";
        return result;
    }
    if (materializerPlan.itemRemapOps != 0 || materializerPlan.ownerRemapOps != 0)
    {
        result.reason = "full_transplant_remap_writer_missing";
        return result;
    }

    result.reason = "full_transplant_no_mutation_backend";
    return result;
}

std::string BuildApplyStatus(const ApplyResult& result)
{
    std::ostringstream out;
    out << (result.attempted ? 1 : 0)
        << "/" << (result.ok ? "ok" : "blocked")
        << "/enabled=" << (result.enabled ? 1 : 0)
        << "/noop=" << (result.noop ? 1 : 0)
        << "/reason=" << StatusToken(result.reason)
        << "/nodes=" << result.targetNodes
        << "/nodeWrites=" << result.nodeWrites
        << "/attrTokenWrites=" << result.attrTokenWrites
        << "/attrValueWrites=" << result.attrValueWrites
        << "/attrPreserve=" << result.attrPreservedNameTokens
        << "/attrUnsupported=" << result.attrUnsupported
        << "/childIndexWrites=" << result.childIndexWrites
        << "/childPreserve=" << result.childIndexPreserved
        << "/full=" << result.fullTransplantOps
        << "/alloc=" << result.allocationOps
        << "/allocIds=" << result.entityAllocationOps
        << "/missingSubtree=" << result.missingSubtreeInsertOps
        << "/newItem=" << result.newItemInsertOps
        << "/remap=" << result.remapOps
        << "/transcode=" << result.transcodeOps
        << "/guards=" << result.guards;
    return out.str();
}
}
