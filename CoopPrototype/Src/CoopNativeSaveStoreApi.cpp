#include "CoopNativeSaveStoreApi.h"

#include "CoopRuntimeConfig.h"
#include "CoopRuntimeGuards.h"

#include <Chairloader/PreyFunction.h>
#include <Prey/CryNetwork/ISerialize.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

namespace
{
using CoopRuntimeGuards::PreflightRuntimePointer;
using CoopRuntimeGuards::RuntimeAccess;
using CoopRuntimeGuards::TryGuardedCall;
using CoopRuntimeGuards::TryReadRuntimeValue;
using CoopRuntimeGuards::TryWriteRuntimeValue;

constexpr size_t kReadNodeStride = 0x50;
constexpr size_t kWriteNodeStride = 0x80;
constexpr size_t kWriteAttrRecordBytes = 0x28;
constexpr size_t kWriteStoreNameRecordBytes = 0x10;

PreyFunction<void*(void* const, uint32_t)> s_funcReadNodeByIndex(0x4DC650);
PreyFunction<void*(void* const, void* const)> s_funcCopyRootReadStackEntry(0x4DC680);
PreyFunction<void*(void* const)> s_funcCurrentStackEntryToNode(0x4DB260);
PreyFunction<void*(void* const, void* const, const char* const)> s_funcFindChildGroup(0x4DAFF0);
PreyFunction<void*(void* const, uint32_t)> s_funcReadChildEntryByIndex(0x4DC380);
PreyFunction<uint8_t(void* const, const char* const, void* const)> s_funcFindAttributeByName(0x4DABD0);
PreyFunction<void(void* const)> s_funcReleaseStackEntry(0x4DAC80);
PreyFunction<const char*(void* const, int)> s_funcResolveChildNameToken(0x4DD460);
PreyFunction<uint16_t(void* const, int, uint32_t)> s_funcReadAttrNameToken(0x4D9F50);
PreyFunction<void(void* const, uint32_t, uint16_t)> s_funcInitAttrCursor(0x4D98F0);
PreyFunction<const char*(void* const)> s_funcAttrCursorName(0x4D94D0);
PreyFunction<void*(void* const, void* const)> s_funcInitReadValueFromNode(0x4D8BF0);
PreyFunction<void(void* const, uint32_t* const)> s_funcReadU32Attribute(0x4D8DB0);
PreyFunction<void(void* const, int32_t* const)> s_funcReadI32Attribute(0x4D8C60);
PreyFunction<void(void* const, float* const)> s_funcReadF32Attribute(0x4D8DD0);
PreyFunction<void(void* const, uint64_t* const)> s_funcReadU64Attribute(0x4D9410);
PreyFunction<void(void* const, bool* const)> s_funcReadBoolAttribute(0x4D9460);
PreyFunction<uint16_t(void* const)> s_funcMaterializeWriteAttrToken(0x4D3170);
PreyFunction<int32_t(int32_t)> s_funcAttrTypePayloadSize(0x4D1AE0);
PreyFunction<void*(void* const)> s_funcWriteStackEntryToNode(0x4D4F70);
PreyFunction<void*(void* const, uint32_t)> s_funcWriteNodeByIndex(0x4D6E90);
PreyFunction<void*(void* const, uint64_t)> s_funcWriteCreateNode(0x4D6BC0);
PreyFunction<void*(void* const, void* const, uint64_t)> s_funcWriteCreateChildGroup(0x4D4920);
PreyFunction<void(void* const, void* const)> s_funcWriteAppendAttribute(0x4CC4B0);
PreyFunction<void(void* const)> s_funcWriteFinalizeNode(0x4D4EB0);
PreyFunction<void(void* const, void* const)> s_funcWriteSaveStackPush(0x4CF560);
PreyFunction<void(void* const, void* const)> s_funcCopyWriteRootStackEntry(0x4D6EA0);
PreyFunction<void*(void* const, const char* const, uint32_t)> s_funcActiveSaveFactory(0x52D6F0);
PreyFunction<uint8_t(void* const, const char* const)> s_funcActiveSaveInit(0x52F8A0);
PreyFunction<TSerialize*(void* const, TSerialize* const, const char* const)> s_funcActiveSaveAddSectionApi(0x52D420);

struct NativeAttrCursor
{
    uint32_t type = 0;
    uint32_t nameIndex = 0;
    uint32_t cursor = 0;
    uint32_t pad = 0;
    std::uintptr_t store = 0;
};

struct WriteStoreNameContext
{
    std::uintptr_t blockVectorBegin = 0;
    std::uintptr_t blockVectorEnd = 0;
    std::uintptr_t nameRecordBegin = 0;
    std::uintptr_t nameRecordEnd = 0;
};

struct WriteStoreNameRecord
{
    uint16_t stringOffset = 0;
    uint16_t blockIndex = 0;
    uint64_t hash = 0;
};

struct NativeStackEntryView
{
    std::uintptr_t store = 0;
    int32_t nodeIndex = -1;
    int32_t generation = 0;
    std::uintptr_t node = 0;
};

struct WriteNodeProbeView
{
    std::uintptr_t store = 0;
    uint32_t nodeIndex = 0;
    int32_t finalizeState = 0;
    uint32_t nameToken = 0;
    uint32_t childCount = 0;
    std::uintptr_t attrBegin = 0;
    std::uintptr_t attrEnd = 0;
    std::uintptr_t childBegin = 0;
    std::uintptr_t childEnd = 0;
    uint8_t finalized = 0;
    uint8_t valid = 0;
    uint32_t storeFinalizeCursor = 0;
};

bool EnvFlagEnabled(const char* name)
{
    return CoopRuntimeConfig::Flag(name);
}

std::string HexU64(uint64_t value)
{
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
    return buffer;
}

std::string PointerHex(std::uintptr_t value)
{
    return HexU64(static_cast<uint64_t>(value));
}

std::string PointerHex(const void* value)
{
    return PointerHex(reinterpret_cast<std::uintptr_t>(value));
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

bool ReadNodeView(std::uintptr_t nodePtr, uint32_t nodeIndex, CoopNativeSaveStoreApi::NodeView& outNode, std::string* outReason)
{
    outNode = {};
    outNode.nodePtr = nodePtr;
    outNode.nodeIndex = nodeIndex;
    if (nodePtr == 0)
    {
        if (outReason)
            *outReason = "node pointer is null";
        return false;
    }

    std::string reason;
    if (!PreflightRuntimePointer(
            "native save store api read node",
            reinterpret_cast<const void*>(nodePtr),
            kReadNodeStride,
            RuntimeAccess::Read,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    const auto* base = reinterpret_cast<const uint8_t*>(nodePtr);
    uint8_t valid = 0;
    TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(base + 0x10), outNode.nodeId);
    TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(base + 0x18), outNode.childCursor);
    TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(base + 0x1C), outNode.childCount);
    TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(base + 0x20), outNode.attrCount);
    TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(base + 0x24), outNode.attrCursor);
    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(base + 0x28), outNode.childIndexBlockBegin);
    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(base + 0x30), outNode.childIndexBlockEnd);
    TryReadRuntimeValue(reinterpret_cast<const uint8_t*>(base + 0x48), valid);
    outNode.valid = valid != 0;
    return true;
}

uint32_t VectorRecordCount(std::uintptr_t begin, std::uintptr_t end, size_t stride)
{
    if (begin == 0 || end < begin || stride == 0)
        return 0;
    const std::uintptr_t bytes = end - begin;
    if ((bytes % stride) != 0)
        return 0;
    return static_cast<uint32_t>(std::min<std::uintptr_t>(bytes / stride, 0xffffffffu));
}

bool ReadWriteNodeProbeView(std::uintptr_t nodePtr, WriteNodeProbeView& outView, std::string* outReason)
{
    outView = {};
    if (nodePtr == 0)
    {
        if (outReason)
            *outReason = "node pointer is null";
        return false;
    }

    std::string reason;
    if (!PreflightRuntimePointer(
            "native save store api write node probe",
            reinterpret_cast<const void*>(nodePtr),
            kWriteNodeStride,
            RuntimeAccess::Read,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    const auto* base = reinterpret_cast<const uint8_t*>(nodePtr);
    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(base + 0x10), outView.store);
    TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(base + 0x18), outView.nodeIndex);
    TryReadRuntimeValue(reinterpret_cast<const int32_t*>(base + 0x1C), outView.finalizeState);
    TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(base + 0x20), outView.nameToken);
    TryReadRuntimeValue(reinterpret_cast<const uint8_t*>(base + 0x40), outView.finalized);
    TryReadRuntimeValue(reinterpret_cast<const uint8_t*>(base + 0x41), outView.valid);
    TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(base + 0x44), outView.childCount);
    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(base + 0x50), outView.attrBegin);
    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(base + 0x58), outView.attrEnd);
    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(base + 0x68), outView.childBegin);
    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(base + 0x70), outView.childEnd);

    if (outView.store != 0 &&
        PreflightRuntimePointer(
            "native save store api write store finalize cursor",
            reinterpret_cast<const void*>(outView.store + 0x400),
            sizeof(uint32_t),
            RuntimeAccess::Read,
            nullptr))
    {
        TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(outView.store + 0x400), outView.storeFinalizeCursor);
    }

    return true;
}

bool CheckExecutableFunction(const char* label, const void* functionPtr, CoopNativeSaveStoreApi::WriteAllocatorProbeResult& result)
{
    ++result.functionChecks;
    std::string reason;
    const bool ok = PreflightRuntimePointer(
        label,
        functionPtr,
        1,
        RuntimeAccess::Execute,
        &reason);
    if (ok)
    {
        ++result.functionsReady;
        return true;
    }

    ++result.guards;
    if (result.reason.empty())
        result.reason = std::string("function_unreadable_") + label;
    return false;
}

bool CheckBuilderExecutableFunction(
    const char* label,
    const void* functionPtr,
    CoopNativeSaveStoreApi::WriteStoreBuilderBackendStatus& result)
{
    ++result.functionChecks;
    std::string reason;
    const bool ok = PreflightRuntimePointer(
        label,
        functionPtr,
        1,
        RuntimeAccess::Execute,
        &reason);
    if (ok)
    {
        ++result.functionsReady;
        return true;
    }

    ++result.guards;
    if (result.reason.empty())
        result.reason = std::string("function_unreadable_") + label;
    return false;
}

bool CheckScratchWriterExecutableFunction(
    const char* label,
    const void* functionPtr,
    CoopNativeSaveStoreApi::ScratchActiveSaveWriterProbeResult& result)
{
    ++result.functionChecks;
    std::string reason;
    const bool ok = PreflightRuntimePointer(
        label,
        functionPtr,
        1,
        RuntimeAccess::Execute,
        &reason);
    if (ok)
    {
        ++result.functionsReady;
        return true;
    }

    ++result.guards;
    if (result.reason.empty())
        result.reason = std::string("function_unreadable_") + label;
    return false;
}

bool CheckScratchSectionExecutableFunction(
    const char* label,
    const void* functionPtr,
    CoopNativeSaveStoreApi::ScratchActiveSaveSectionProbeResult& result)
{
    ++result.functionChecks;
    std::string reason;
    const bool ok = PreflightRuntimePointer(
        label,
        functionPtr,
        1,
        RuntimeAccess::Execute,
        &reason);
    if (ok)
    {
        ++result.functionsReady;
        return true;
    }

    ++result.guards;
    if (result.reason.empty())
        result.reason = std::string("function_unreadable_") + label;
    return false;
}

CoopNativeSaveStoreApi::NodeView MakeDecodedNodeView(const CoopSaveStoreDecoder::StoreNodeRecord& node)
{
    CoopNativeSaveStoreApi::NodeView view;
    view.nodePtr = node.nodePtr;
    view.nodeIndex = node.nodeIndex;
    view.nodeId = node.nodeId;
    view.childCursor = node.childCursor;
    view.childCount = node.childCount;
    view.attrCursor = node.attrCursor;
    view.attrCount = node.attrCount;
    view.childIndexBlockBegin = node.childIndexBlockBegin;
    view.childIndexBlockEnd = node.childIndexBlockEnd;
    view.valid = node.valid;
    return view;
}

uint32_t CountNodeMismatches(
    const CoopNativeSaveStoreApi::NodeView& lhs,
    const CoopNativeSaveStoreApi::NodeView& rhs)
{
    uint32_t mismatches = 0;
    mismatches += lhs.nodePtr == rhs.nodePtr ? 0u : 1u;
    mismatches += lhs.nodeIndex == rhs.nodeIndex ? 0u : 1u;
    mismatches += lhs.nodeId == rhs.nodeId ? 0u : 1u;
    mismatches += lhs.childCursor == rhs.childCursor ? 0u : 1u;
    mismatches += lhs.childCount == rhs.childCount ? 0u : 1u;
    mismatches += lhs.attrCursor == rhs.attrCursor ? 0u : 1u;
    mismatches += lhs.attrCount == rhs.attrCount ? 0u : 1u;
    mismatches += lhs.childIndexBlockBegin == rhs.childIndexBlockBegin ? 0u : 1u;
    mismatches += lhs.childIndexBlockEnd == rhs.childIndexBlockEnd ? 0u : 1u;
    mismatches += lhs.valid == rhs.valid ? 0u : 1u;
    return mismatches;
}

NativeStackEntryView DecodeNativeStackEntry(const std::array<std::uintptr_t, 3>& entry)
{
    NativeStackEntryView view;
    view.store = entry[0];
    const auto* bytes = reinterpret_cast<const uint8_t*>(entry.data());
    std::memcpy(&view.nodeIndex, bytes + 0x8, sizeof(view.nodeIndex));
    std::memcpy(&view.generation, bytes + 0xC, sizeof(view.generation));
    std::memcpy(&view.node, bytes + 0x10, sizeof(view.node));
    return view;
}

bool TryReleaseStackEntryGuarded(std::array<std::uintptr_t, 3>& entry, uint32_t& guards)
{
    const NativeStackEntryView view = DecodeNativeStackEntry(entry);
    if (view.store == 0)
        return true;

    std::string reason;
    bool releaseResult = false;
    const bool callOk = TryGuardedCall(
        "native save store api ReleaseStackEntry",
        [&]() -> bool
        {
            s_funcReleaseStackEntry(entry.data());
            return true;
        },
        releaseResult,
        &reason);
    if (!callOk || !releaseResult)
        ++guards;
    entry = {};
    return callOk && releaseResult;
}

bool ProbeReadStoreSection(
    const CoopNativeSaveStoreApi::StoreHandle& store,
    const char* sectionName,
    CoopNativeSaveStoreApi::CheckResult& result)
{
    ++result.loadSectionChecks;
    result.gameStateSectionReason.clear();

    if (!store.valid || !store.readStore)
    {
        result.gameStateSectionReason = "store handle is not a valid read store";
        return false;
    }
    if (!sectionName || !sectionName[0])
    {
        result.gameStateSectionReason = "section name is empty";
        return false;
    }

    std::string reason;
    if (!PreflightRuntimePointer(
            "native save store api CopyRootReadStackEntry function",
            s_funcCopyRootReadStackEntry.GetVoidPtr(),
            1,
            RuntimeAccess::Execute,
            &reason) ||
        !PreflightRuntimePointer(
            "native save store api CurrentStackEntryToNode function",
            s_funcCurrentStackEntryToNode.GetVoidPtr(),
            1,
            RuntimeAccess::Execute,
            &reason) ||
        !PreflightRuntimePointer(
            "native save store api FindChildGroup function",
            s_funcFindChildGroup.GetVoidPtr(),
            1,
            RuntimeAccess::Execute,
            &reason) ||
        !PreflightRuntimePointer(
            "native save store api ReleaseStackEntry function",
            s_funcReleaseStackEntry.GetVoidPtr(),
            1,
            RuntimeAccess::Execute,
            &reason))
    {
        ++result.loadSectionGuards;
        result.gameStateSectionReason = reason.empty() ? "load section function preflight failed" : reason;
        return false;
    }

    std::array<std::uintptr_t, 3> rootEntry = {};
    std::array<std::uintptr_t, 3> sectionEntry = {};
    void* rootEntryResult = nullptr;
    bool ok = TryGuardedCall(
        "native save store api CopyRootReadStackEntry",
        [&]() -> void*
        {
            return s_funcCopyRootReadStackEntry(
                reinterpret_cast<void*>(store.store),
                rootEntry.data());
        },
        rootEntryResult,
        &reason);
    if (!ok || !rootEntryResult)
    {
        ++result.loadSectionGuards;
        result.gameStateSectionReason = reason.empty() ? "copy root read stack entry failed" : reason;
        return false;
    }

    void* rootNode = nullptr;
    ok = TryGuardedCall(
        "native save store api CurrentStackEntryToNode(root)",
        [&]() -> void*
        {
            return s_funcCurrentStackEntryToNode(rootEntry.data());
        },
        rootNode,
        &reason);
    if (!ok || !rootNode)
    {
        ++result.loadSectionGuards;
        result.gameStateSectionReason = reason.empty() ? "current root node failed" : reason;
        TryReleaseStackEntryGuarded(rootEntry, result.loadSectionGuards);
        return false;
    }

    void* sectionEntryResult = nullptr;
    ok = TryGuardedCall(
        "native save store api FindChildGroup(GameState)",
        [&]() -> void*
        {
            return s_funcFindChildGroup(
                rootNode,
                sectionEntry.data(),
                sectionName);
        },
        sectionEntryResult,
        &reason);

    const NativeStackEntryView sectionView = DecodeNativeStackEntry(sectionEntry);
    if (!ok || !sectionEntryResult || sectionView.nodeIndex < 0)
    {
        ++result.loadSectionGuards;
        result.gameStateSectionReason =
            reason.empty()
                ? "section not found"
                : reason;
        TryReleaseStackEntryGuarded(sectionEntry, result.loadSectionGuards);
        TryReleaseStackEntryGuarded(rootEntry, result.loadSectionGuards);
        return false;
    }

    void* sectionNode = nullptr;
    ok = TryGuardedCall(
        "native save store api CurrentStackEntryToNode(section)",
        [&]() -> void*
        {
            return s_funcCurrentStackEntryToNode(sectionEntry.data());
        },
        sectionNode,
        &reason);
    if (!ok || !sectionNode)
    {
        ++result.loadSectionGuards;
        result.gameStateSectionReason = reason.empty() ? "current section node failed" : reason;
        TryReleaseStackEntryGuarded(sectionEntry, result.loadSectionGuards);
        TryReleaseStackEntryGuarded(rootEntry, result.loadSectionGuards);
        return false;
    }

    CoopNativeSaveStoreApi::NodeView nativeSectionNode;
    if (!ReadNodeView(
            reinterpret_cast<std::uintptr_t>(sectionNode),
            static_cast<uint32_t>(sectionView.nodeIndex),
            nativeSectionNode,
            &reason))
    {
        ++result.loadSectionGuards;
        result.gameStateSectionReason = reason.empty() ? "section node read failed" : reason;
        TryReleaseStackEntryGuarded(sectionEntry, result.loadSectionGuards);
        TryReleaseStackEntryGuarded(rootEntry, result.loadSectionGuards);
        return false;
    }

    result.gameStateSectionFound = true;
    result.gameStateSectionNodeIndex = sectionView.nodeIndex;
    result.gameStateSectionNodeId = nativeSectionNode.nodeId;
    result.gameStateSectionNodePtr = nativeSectionNode.nodePtr;
    result.gameStateSectionReason = "ok";

    TryReleaseStackEntryGuarded(sectionEntry, result.loadSectionGuards);
    TryReleaseStackEntryGuarded(rootEntry, result.loadSectionGuards);
    return true;
}
}

namespace CoopNativeSaveStoreApi
{
StoreHandle MakeReadStoreHandle(const CoopSaveStoreDecoder::StoreMap& map)
{
    StoreHandle handle;
    handle.store = map.store;
    handle.nodeBlockBegin = map.nodeBlockBegin;
    handle.nodeBlockEnd = map.nodeBlockEnd;
    TryReadRuntimeValue(
        reinterpret_cast<const std::uintptr_t*>(handle.store + 0x20),
        handle.attrDataBase);
    handle.attrStringBase = map.attrStringBase;
    handle.attrNameOffsetTable = map.attrNameOffsetTable;
    handle.attrTokenContext = map.attrTokenContext;
    handle.attrTokenIndexBase = map.attrTokenIndexBase;
    handle.attrTokenBase = map.attrTokenBase;
    handle.childNameResolverContext = map.childNameResolverContext;
    TryReadRuntimeValue(
        reinterpret_cast<const std::uintptr_t*>(handle.childNameResolverContext + 0x8),
        handle.childNameDataBase);
    TryReadRuntimeValue(
        reinterpret_cast<const std::uintptr_t*>(handle.childNameResolverContext + 0x20),
        handle.childNameOffsetTable);
    handle.nodeCount = map.nodeCount;
    handle.readStore = map.readStore;
    handle.valid =
        map.ok &&
        map.readStore &&
        map.store != 0 &&
        map.nodeBlockBegin != 0 &&
        map.nodeBlockEnd > map.nodeBlockBegin &&
        map.nodeCount != 0;
    return handle;
}

bool TryReadNodeByIndex(const StoreHandle& store, uint32_t nodeIndex, NodeView& outNode, std::string* outReason)
{
    outNode = {};
    if (!store.valid || !store.readStore)
    {
        if (outReason)
            *outReason = "store handle is not a valid read store";
        return false;
    }

    if (nodeIndex >= store.nodeCount)
    {
        if (outReason)
            *outReason = "node index outside read store";
        return false;
    }

    std::string reason;
    if (!PreflightRuntimePointer(
            "native save store api read store",
            reinterpret_cast<const void*>(store.store),
            0x128,
            RuntimeAccess::Read,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    if (!PreflightRuntimePointer(
            "native save store api ReadNodeByIndex function",
            s_funcReadNodeByIndex.GetVoidPtr(),
            1,
            RuntimeAccess::Execute,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    void* node = nullptr;
    if (!TryGuardedCall(
            "native save store api ReadNodeByIndex",
            [&]() -> void*
            {
                return s_funcReadNodeByIndex(reinterpret_cast<void*>(store.store), nodeIndex);
            },
            node,
            &reason) ||
        !node)
    {
        if (outReason)
            *outReason = reason.empty() ? "ReadNodeByIndex returned null" : reason;
        return false;
    }

    const std::uintptr_t nodePtr = reinterpret_cast<std::uintptr_t>(node);
    const std::uintptr_t expectedBegin = store.nodeBlockBegin;
    const std::uintptr_t expectedEnd = store.nodeBlockEnd;
    if (nodePtr < expectedBegin || nodePtr + kReadNodeStride > expectedEnd)
    {
        if (outReason)
        {
            *outReason =
                "ReadNodeByIndex returned pointer outside node block node=" + PointerHex(nodePtr) +
                " block=" + PointerHex(expectedBegin) + ".." + PointerHex(expectedEnd);
        }
        return false;
    }

    return ReadNodeView(nodePtr, nodeIndex, outNode, outReason);
}

bool TryFindReadSection(
    const StoreHandle& store,
    const char* sectionName,
    NodeView& outNode,
    std::string* outReason)
{
    outNode = {};

    CheckResult result;
    if (!ProbeReadStoreSection(store, sectionName, result))
    {
        if (outReason)
            *outReason = result.gameStateSectionReason.empty() ? "section not found" : result.gameStateSectionReason;
        return false;
    }

    const uint32_t sectionNodeIndex = static_cast<uint32_t>(result.gameStateSectionNodeIndex);
    if (!TryReadNodeByIndex(store, sectionNodeIndex, outNode, outReason))
        return false;

    return true;
}

bool TryEnumerateChildEntryIndices(
    const NodeView& node,
    std::vector<uint32_t>& outChildEntryIndices,
    uint32_t maxEntries,
    std::string* outReason)
{
    outChildEntryIndices.clear();
    if (!node.valid || node.nodePtr == 0)
    {
        if (outReason)
            *outReason = "node is not valid";
        return false;
    }
    if (node.childCount == 0)
        return true;
    if (node.childIndexBlockBegin == 0 ||
        node.childIndexBlockEnd <= node.childIndexBlockBegin ||
        ((node.childIndexBlockEnd - node.childIndexBlockBegin) % sizeof(uint32_t)) != 0)
    {
        if (outReason)
            *outReason = "node child index block is invalid";
        return false;
    }

    const std::uintptr_t blockBytes = node.childIndexBlockEnd - node.childIndexBlockBegin;
    const uint32_t availableBlocks = static_cast<uint32_t>(std::min<std::uintptr_t>(
        blockBytes / sizeof(uint32_t),
        65536));
    const uint32_t requiredBlocks =
        (node.childCount >> 6) + ((node.childCount & 0x3fu) != 0 ? 1u : 0u);
    const uint32_t blocks = std::min(availableBlocks, requiredBlocks);
    if (blocks == 0)
        return true;

    std::string reason;
    if (!PreflightRuntimePointer(
            "native save store api child entry blocks",
            reinterpret_cast<const void*>(node.childIndexBlockBegin),
            static_cast<size_t>(blocks) * sizeof(uint32_t),
            RuntimeAccess::Read,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    outChildEntryIndices.reserve(std::min(node.childCount, maxEntries));
    uint32_t remaining = node.childCount;
    for (uint32_t block = 0; block < blocks && remaining > 0 && outChildEntryIndices.size() < maxEntries; ++block)
    {
        uint32_t baseEntryIndex = 0;
        if (!TryReadRuntimeValue(
                reinterpret_cast<const uint32_t*>(node.childIndexBlockBegin + static_cast<std::uintptr_t>(block) * sizeof(uint32_t)),
                baseEntryIndex))
        {
            if (outReason)
                *outReason = "child entry block read failed";
            return false;
        }

        const uint32_t entriesInBlock = std::min<uint32_t>(remaining, 64);
        for (uint32_t i = 0; i < entriesInBlock && outChildEntryIndices.size() < maxEntries; ++i)
            outChildEntryIndices.push_back(baseEntryIndex + i);
        remaining -= entriesInBlock;
    }

    return true;
}

bool TryReadChildEntryByIndex(
    const StoreHandle& store,
    uint32_t childEntryIndex,
    NodeView& outNode,
    std::string* outReason)
{
    outNode = {};
    if (!store.valid || !store.readStore)
    {
        if (outReason)
            *outReason = "store handle is not a valid read store";
        return false;
    }
    if (childEntryIndex > 0x1000000u)
    {
        if (outReason)
            *outReason = "child entry index outside guarded range";
        return false;
    }

    std::string reason;
    if (!PreflightRuntimePointer(
            "native save store api read store",
            reinterpret_cast<const void*>(store.store),
            0x128,
            RuntimeAccess::Read,
            &reason) ||
        !PreflightRuntimePointer(
            "native save store api ReadChildEntryByIndex function",
            s_funcReadChildEntryByIndex.GetVoidPtr(),
            1,
            RuntimeAccess::Execute,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    void* node = nullptr;
    if (!TryGuardedCall(
            "native save store api ReadChildEntryByIndex",
            [&]() -> void*
            {
                return s_funcReadChildEntryByIndex(reinterpret_cast<void*>(store.store), childEntryIndex);
            },
            node,
            &reason) ||
        !node)
    {
        if (outReason)
            *outReason = reason.empty() ? "ReadChildEntryByIndex returned null" : reason;
        return false;
    }

    const std::uintptr_t nodePtr = reinterpret_cast<std::uintptr_t>(node);
    if (nodePtr < store.nodeBlockBegin ||
        nodePtr + kReadNodeStride > store.nodeBlockEnd ||
        ((nodePtr - store.nodeBlockBegin) % kReadNodeStride) != 0)
    {
        if (outReason)
        {
            *outReason =
                "ReadChildEntryByIndex returned node outside node block node=" + PointerHex(nodePtr) +
                " block=" + PointerHex(store.nodeBlockBegin) + ".." + PointerHex(store.nodeBlockEnd);
        }
        return false;
    }

    const uint32_t nodeIndex = static_cast<uint32_t>((nodePtr - store.nodeBlockBegin) / kReadNodeStride);
    return ReadNodeView(nodePtr, nodeIndex, outNode, outReason);
}

bool TryResolveReadNodeName(
    const NodeView& node,
    const char*& outName,
    std::string* outReason)
{
    outName = nullptr;
    if (!node.valid || node.nodePtr == 0)
    {
        if (outReason)
            *outReason = "node is not valid";
        return false;
    }

    std::string reason;
    const std::uintptr_t namePointerSlot = node.nodePtr + 0x40;
    if (!PreflightRuntimePointer(
            "native save store api read node name slot",
            reinterpret_cast<const void*>(namePointerSlot),
            sizeof(std::uintptr_t),
            RuntimeAccess::Read,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    std::uintptr_t namePtr = 0;
    if (!TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(namePointerSlot), namePtr) ||
        namePtr == 0)
    {
        if (outReason)
            *outReason = "node name pointer read failed";
        return false;
    }

    if (!PreflightRuntimePointer(
            "native save store api read node name",
            reinterpret_cast<const void*>(namePtr),
            1,
            RuntimeAccess::Read,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    const std::string value = CoopRuntimeGuards::ReadRuntimeCString(
        reinterpret_cast<const char*>(namePtr),
        256);
    if (value.empty())
    {
        if (outReason)
            *outReason = "node name is empty";
        return false;
    }

    outName = reinterpret_cast<const char*>(namePtr);
    return true;
}

bool TryResolveChildNameToken(const StoreHandle& store, uint32_t nameToken, const char*& outName, std::string* outReason)
{
    outName = nullptr;
    if (!store.valid || !store.readStore)
    {
        if (outReason)
            *outReason = "store handle is not a valid read store";
        return false;
    }

    if (nameToken > 0x100000u)
    {
        if (outReason)
            *outReason = "name token outside guarded range";
        return false;
    }

    std::string reason;
    if (!PreflightRuntimePointer(
            "native save store api child name resolver context",
            reinterpret_cast<const void*>(store.childNameResolverContext),
            0x28,
            RuntimeAccess::Read,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    const std::uintptr_t offsetAddress =
        store.childNameOffsetTable + static_cast<std::uintptr_t>(nameToken) * sizeof(uint32_t);
    if (!store.childNameDataBase || !store.childNameOffsetTable ||
        !PreflightRuntimePointer(
            "native save store api child name offset",
            reinterpret_cast<const void*>(offsetAddress),
            sizeof(uint32_t),
            RuntimeAccess::Read,
            &reason))
    {
        if (outReason)
            *outReason = reason.empty() ? "child name pools unavailable" : reason;
        return false;
    }

    if (!PreflightRuntimePointer(
            "native save store api ResolveChildNameToken function",
            s_funcResolveChildNameToken.GetVoidPtr(),
            1,
            RuntimeAccess::Execute,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    const char* resolved = nullptr;
    if (!TryGuardedCall(
            "native save store api ResolveChildNameToken",
            [&]() -> const char*
            {
                return s_funcResolveChildNameToken(
                    reinterpret_cast<void*>(store.childNameResolverContext),
                    static_cast<int>(nameToken));
            },
            resolved,
            &reason) ||
        !resolved)
    {
        if (outReason)
            *outReason = reason.empty() ? "ResolveChildNameToken returned null" : reason;
        return false;
    }

    outName = resolved;
    return true;
}

bool TryFindChildGroup(
    const StoreHandle& store,
    const NodeView& parentNode,
    const char* childName,
    NodeView& outNode,
    std::string* outReason)
{
    outNode = {};
    if (!store.valid || !store.readStore)
    {
        if (outReason)
            *outReason = "store handle is not a valid read store";
        return false;
    }
    if (!parentNode.valid || parentNode.nodePtr == 0)
    {
        if (outReason)
            *outReason = "parent node is not valid";
        return false;
    }
    if (!childName || !childName[0])
    {
        if (outReason)
            *outReason = "child name is empty";
        return false;
    }

    std::string reason;
    if (!PreflightRuntimePointer(
            "native save store api parent node",
            reinterpret_cast<const void*>(parentNode.nodePtr),
            kReadNodeStride,
            RuntimeAccess::Read,
            &reason) ||
        !PreflightRuntimePointer(
            "native save store api FindChildGroup function",
            s_funcFindChildGroup.GetVoidPtr(),
            1,
            RuntimeAccess::Execute,
            &reason) ||
        !PreflightRuntimePointer(
            "native save store api CurrentStackEntryToNode function",
            s_funcCurrentStackEntryToNode.GetVoidPtr(),
            1,
            RuntimeAccess::Execute,
            &reason) ||
        !PreflightRuntimePointer(
            "native save store api ReleaseStackEntry function",
            s_funcReleaseStackEntry.GetVoidPtr(),
            1,
            RuntimeAccess::Execute,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    std::array<std::uintptr_t, 3> childEntry = {};
    void* childEntryResult = nullptr;
    bool ok = TryGuardedCall(
        "native save store api FindChildGroup",
        [&]() -> void*
        {
            return s_funcFindChildGroup(
                reinterpret_cast<void*>(parentNode.nodePtr),
                childEntry.data(),
                childName);
        },
        childEntryResult,
        &reason);

    const NativeStackEntryView childView = DecodeNativeStackEntry(childEntry);
    if (!ok || !childEntryResult || childView.nodeIndex < 0)
    {
        uint32_t guards = 0;
        TryReleaseStackEntryGuarded(childEntry, guards);
        if (outReason)
            *outReason = reason.empty() ? "child group not found" : reason;
        return false;
    }

    void* childNode = nullptr;
    ok = TryGuardedCall(
        "native save store api CurrentStackEntryToNode(child)",
        [&]() -> void*
        {
            return s_funcCurrentStackEntryToNode(childEntry.data());
        },
        childNode,
        &reason);
    if (!ok || !childNode)
    {
        uint32_t guards = 0;
        TryReleaseStackEntryGuarded(childEntry, guards);
        if (outReason)
            *outReason = reason.empty() ? "current child node failed" : reason;
        return false;
    }

    const bool readOk =
        ReadNodeView(
            reinterpret_cast<std::uintptr_t>(childNode),
            static_cast<uint32_t>(childView.nodeIndex),
            outNode,
            &reason);
    uint32_t guards = 0;
    TryReleaseStackEntryGuarded(childEntry, guards);
    if (!readOk)
    {
        if (outReason)
            *outReason = reason.empty() ? "child node read failed" : reason;
        return false;
    }

    return true;
}

bool TryReadAttrNameToken(const StoreHandle& store, uint32_t attrTokenCursor, uint32_t attrOrdinal, uint16_t& outToken, std::string* outReason)
{
    outToken = 0;
    AttrTokenView view;
    if (!TryReadAttrTokenView(store, attrTokenCursor, attrOrdinal, view, outReason))
        return false;

    outToken = view.token;
    return true;
}

bool TryReadAttrTokenView(
    const StoreHandle& store,
    uint32_t attrTokenCursor,
    uint32_t attrOrdinal,
    AttrTokenView& outView,
    std::string* outReason)
{
    outView = {};
    outView.attrCursor = attrTokenCursor;
    outView.attrOrdinal = attrOrdinal;
    if (!store.valid || !store.readStore)
    {
        if (outReason)
            *outReason = "store handle is not a valid read store";
        return false;
    }

    if (attrTokenCursor > 0x400000u || attrOrdinal > 0x10000u)
    {
        if (outReason)
            *outReason = "attr cursor outside guarded range";
        return false;
    }

    std::string reason;
    if (!PreflightRuntimePointer(
            "native save store api attr token context",
            reinterpret_cast<const void*>(store.attrTokenContext),
            0x30,
            RuntimeAccess::Read,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    if (!store.attrTokenIndexBase || !store.attrTokenBase)
    {
        if (outReason)
            *outReason = "attr token pools unavailable";
        return false;
    }

    const std::uintptr_t indexAddress =
        store.attrTokenIndexBase + static_cast<std::uintptr_t>(attrTokenCursor) * sizeof(uint16_t);
    if (!PreflightRuntimePointer(
            "native save store api attr token index",
            reinterpret_cast<const void*>(indexAddress),
            sizeof(uint16_t),
            RuntimeAccess::Read,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    uint16_t tokenByteOffset = 0;
    if (!TryReadRuntimeValue(reinterpret_cast<const uint16_t*>(indexAddress), tokenByteOffset))
    {
        if (outReason)
            *outReason = "attr token index read failed";
        return false;
    }

    const std::uintptr_t tokenAddress =
        store.attrTokenBase +
        static_cast<std::uintptr_t>(tokenByteOffset) +
        static_cast<std::uintptr_t>(attrOrdinal) * sizeof(uint16_t);
    if (!PreflightRuntimePointer(
            "native save store api attr token",
            reinterpret_cast<const void*>(tokenAddress),
            sizeof(uint16_t),
            RuntimeAccess::Read,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    if (!PreflightRuntimePointer(
            "native save store api ReadAttrNameToken function",
            s_funcReadAttrNameToken.GetVoidPtr(),
            1,
            RuntimeAccess::Execute,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    uint16_t token = 0;
    if (!TryGuardedCall(
            "native save store api ReadAttrNameToken",
            [&]() -> uint16_t
            {
                return s_funcReadAttrNameToken(
                    reinterpret_cast<void*>(store.attrTokenContext),
                    static_cast<int>(attrTokenCursor),
                    attrOrdinal);
            },
            token,
            &reason))
    {
        if (outReason)
            *outReason = reason.empty() ? "ReadAttrNameToken failed" : reason;
        return false;
    }

    outView.token = token;
    outView.tokenByteOffset = tokenByteOffset;
    outView.tokenAddress = tokenAddress;
    outView.valid = true;
    return true;
}

bool TryRewriteSameAttrNameToken(
    const StoreHandle& store,
    uint32_t attrTokenCursor,
    uint32_t attrOrdinal,
    AttrTokenView* outView,
    std::string* outReason)
{
    if (!IsWriteApiEnabled())
    {
        if (outReason)
            *outReason = "write api disabled";
        return false;
    }

    AttrTokenView view;
    if (!TryReadAttrTokenView(store, attrTokenCursor, attrOrdinal, view, outReason))
        return false;

    std::string reason;
    if (!PreflightRuntimePointer(
            "native save store api same attr token rewrite",
            reinterpret_cast<void*>(view.tokenAddress),
            sizeof(uint16_t),
            RuntimeAccess::Write,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    if (!TryWriteRuntimeValue(reinterpret_cast<uint16_t*>(view.tokenAddress), view.token))
    {
        if (outReason)
            *outReason = "attr token same-value write failed";
        return false;
    }

    uint16_t verify = 0;
    if (!TryReadRuntimeValue(reinterpret_cast<const uint16_t*>(view.tokenAddress), verify) || verify != view.token)
    {
        if (outReason)
            *outReason = "attr token same-value verify failed";
        return false;
    }

    if (outView)
        *outView = view;
    return true;
}

bool TryResolveAttrName(const StoreHandle& store, uint32_t attrValueCursor, uint16_t attrToken, const char*& outName, std::string* outReason)
{
    outName = nullptr;
    if (!store.valid || !store.readStore)
    {
        if (outReason)
            *outReason = "store handle is not a valid read store";
        return false;
    }

    const uint32_t nameIndex = static_cast<uint32_t>(attrToken >> 6);
    if (nameIndex > 0x100000u)
    {
        if (outReason)
            *outReason = "attr name index outside guarded range";
        return false;
    }

    std::string reason;
    const std::uintptr_t offsetAddress =
        store.attrNameOffsetTable + static_cast<std::uintptr_t>(nameIndex) * sizeof(uint32_t);
    if (!store.attrStringBase || !store.attrNameOffsetTable ||
        !PreflightRuntimePointer(
            "native save store api attr name offset",
            reinterpret_cast<const void*>(offsetAddress),
            sizeof(uint32_t),
            RuntimeAccess::Read,
            &reason))
    {
        if (outReason)
            *outReason = reason.empty() ? "attr name pools unavailable" : reason;
        return false;
    }

    if (!PreflightRuntimePointer(
            "native save store api InitAttrCursor function",
            s_funcInitAttrCursor.GetVoidPtr(),
            1,
            RuntimeAccess::Execute,
            &reason) ||
        !PreflightRuntimePointer(
            "native save store api AttrCursorName function",
            s_funcAttrCursorName.GetVoidPtr(),
            1,
            RuntimeAccess::Execute,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    NativeAttrCursor cursor;
    bool initOk = false;
    if (!TryGuardedCall(
            "native save store api InitAttrCursor",
            [&]() -> bool
            {
                s_funcInitAttrCursor(&cursor, attrValueCursor, attrToken);
                return true;
            },
            initOk,
            &reason))
    {
        if (outReason)
            *outReason = reason.empty() ? "InitAttrCursor failed" : reason;
        return false;
    }
    cursor.store = store.store;

    const char* resolved = nullptr;
    if (!TryGuardedCall(
            "native save store api AttrCursorName",
            [&]() -> const char*
            {
                return s_funcAttrCursorName(&cursor);
            },
            resolved,
            &reason) ||
        !resolved)
    {
        if (outReason)
            *outReason = reason.empty() ? "AttrCursorName returned null" : reason;
        return false;
    }

    outName = resolved;
    return true;
}

bool TryFindReadAttribute(
    const StoreHandle& store,
    const NodeView& node,
    const char* attrName,
    ReadAttributeView& outAttribute,
    std::string* outReason)
{
    outAttribute = {};
    if (!store.valid || !store.readStore)
    {
        if (outReason)
            *outReason = "store handle is not a valid read store";
        return false;
    }
    if (!node.valid || node.nodePtr == 0)
    {
        if (outReason)
            *outReason = "node is not valid";
        return false;
    }
    if (!attrName || !attrName[0])
    {
        if (outReason)
            *outReason = "attr name is empty";
        return false;
    }

    std::string reason;
    if (!PreflightRuntimePointer(
            "native save store api read attr node",
            reinterpret_cast<const void*>(node.nodePtr),
            kReadNodeStride,
            RuntimeAccess::Read,
            &reason) ||
        !PreflightRuntimePointer(
            "native save store api read attr name",
            attrName,
            1,
            RuntimeAccess::Read,
            &reason) ||
        !PreflightRuntimePointer(
            "native save store api InitReadValueFromNode function",
            s_funcInitReadValueFromNode.GetVoidPtr(),
            1,
            RuntimeAccess::Execute,
            &reason) ||
        !PreflightRuntimePointer(
            "native save store api FindAttributeByName function",
            s_funcFindAttributeByName.GetVoidPtr(),
            1,
            RuntimeAccess::Execute,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    std::uintptr_t nodeStore = 0;
    if (!TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(node.nodePtr), nodeStore) ||
        nodeStore == 0)
    {
        if (outReason)
            *outReason = "node store pointer read failed";
        return false;
    }
    if (nodeStore != store.store)
    {
        if (outReason)
        {
            *outReason =
                "node store pointer mismatch nodeStore=" + PointerHex(nodeStore) +
                " handleStore=" + PointerHex(store.store);
        }
        return false;
    }

    void* initResult = nullptr;
    if (!TryGuardedCall(
            "native save store api InitReadValueFromNode",
            [&]() -> void*
            {
                return s_funcInitReadValueFromNode(
                    outAttribute.nativeValue.data(),
                    reinterpret_cast<void*>(nodeStore));
            },
            initResult,
            &reason) ||
        !initResult)
    {
        if (outReason)
            *outReason = reason.empty() ? "InitReadValueFromNode failed" : reason;
        return false;
    }

    uint8_t found = 0;
    if (!TryGuardedCall(
            "native save store api FindAttributeByName",
            [&]() -> uint8_t
            {
                return s_funcFindAttributeByName(
                    reinterpret_cast<void*>(node.nodePtr),
                    attrName,
                    outAttribute.nativeValue.data());
            },
            found,
            &reason) ||
        found == 0)
    {
        if (outReason)
            *outReason = reason.empty() ? "attribute not found" : reason;
        return false;
    }

    const auto* bytes = outAttribute.nativeValue.data();
    std::memcpy(&outAttribute.type, bytes + 0x0, sizeof(outAttribute.type));
    std::memcpy(&outAttribute.nameIndex, bytes + 0x4, sizeof(outAttribute.nameIndex));
    std::memcpy(&outAttribute.attrCursor, bytes + 0x8, sizeof(outAttribute.attrCursor));
    outAttribute.token =
        static_cast<uint16_t>(
            ((outAttribute.nameIndex & 0x3FFu) << 6) |
            (outAttribute.type & 0x3Fu));
    outAttribute.valid = true;
    return true;
}

bool TryGetReadAttributePayloadByteCount(
    const StoreHandle& store,
    uint32_t attrValueCursor,
    uint32_t attrType,
    uint32_t& outByteCount,
    std::string* outReason)
{
    outByteCount = 0;
    if (!store.valid || !store.readStore)
    {
        if (outReason)
            *outReason = "store handle is not a valid read store";
        return false;
    }
    if (!store.attrDataBase)
    {
        if (outReason)
            *outReason = "attr data pool unavailable";
        return false;
    }
    if (attrType > 0x3Fu || attrValueCursor > 0x4000000u)
    {
        if (outReason)
            *outReason = "attr payload cursor/type outside guarded range";
        return false;
    }

    constexpr uint32_t kMaxReadAttrPayloadBytes = 4u * 1024u * 1024u;
    if (attrType == 6)
    {
        uint32_t stringBytes = 0;
        const std::uintptr_t lengthAddress = store.attrDataBase + attrValueCursor;
        std::string reason;
        if (!PreflightRuntimePointer(
                "native save store api string attr length",
                reinterpret_cast<const void*>(lengthAddress),
                sizeof(stringBytes),
                RuntimeAccess::Read,
                &reason) ||
            !TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(lengthAddress), stringBytes))
        {
            if (outReason)
                *outReason = reason.empty() ? "string attr length read failed" : reason;
            return false;
        }
        if (stringBytes > kMaxReadAttrPayloadBytes - sizeof(stringBytes))
        {
            if (outReason)
                *outReason = "string attr payload too large";
            return false;
        }

        outByteCount = static_cast<uint32_t>(sizeof(stringBytes)) + stringBytes;
        return true;
    }

    int32_t payloadBytes = -1;
    if (!TryGetAttrTypePayloadSize(attrType, payloadBytes, outReason) || payloadBytes < 0)
        return false;
    if (static_cast<uint32_t>(payloadBytes) > kMaxReadAttrPayloadBytes)
    {
        if (outReason)
            *outReason = "attr payload too large";
        return false;
    }

    outByteCount = static_cast<uint32_t>(payloadBytes);
    return true;
}

bool TryEnumerateReadAttributes(
    const StoreHandle& store,
    const NodeView& node,
    std::vector<ReadAttributeRecord>& outAttributes,
    uint32_t maxAttrs,
    std::string* outReason)
{
    outAttributes.clear();
    if (!store.valid || !store.readStore)
    {
        if (outReason)
            *outReason = "store handle is not a valid read store";
        return false;
    }
    if (!node.valid || node.nodePtr == 0)
    {
        if (outReason)
            *outReason = "node is not valid";
        return false;
    }
    if (node.attrCount == 0 || maxAttrs == 0)
        return true;

    std::uintptr_t nodeStore = 0;
    if (!TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(node.nodePtr), nodeStore) ||
        nodeStore != store.store)
    {
        if (outReason)
            *outReason = "node store pointer mismatch";
        return false;
    }

    const uint32_t attrLimit = std::min<uint32_t>(node.attrCount, std::min<uint32_t>(maxAttrs, 4096u));
    const uint32_t attrTokenCursor = node.nodeId;
    uint32_t valueCursor = node.attrCursor;
    for (uint32_t ordinal = 0; ordinal < attrLimit; ++ordinal)
    {
        AttrTokenView tokenView;
        if (!TryReadAttrTokenView(store, attrTokenCursor, ordinal, tokenView, outReason))
            return false;

        ReadAttributeRecord record;
        record.ordinal = ordinal;
        record.valueCursor = valueCursor;
        record.view.attrCursor = valueCursor;
        record.view.token = tokenView.token;
        record.view.type = static_cast<uint32_t>(tokenView.token & 0x3Fu);
        record.view.nameIndex = static_cast<uint32_t>(tokenView.token >> 6);
        record.view.valid = true;

        uint32_t byteCount = 0;
        if (!TryGetReadAttributePayloadByteCount(store, valueCursor, record.view.type, byteCount, outReason))
            return false;
        record.valueByteCount = byteCount;

        if (record.valueByteCount > 0)
        {
            const std::uintptr_t valueAddress = store.attrDataBase + valueCursor;
            std::string reason;
            if (!PreflightRuntimePointer(
                    "native save store api read attr payload",
                    reinterpret_cast<const void*>(valueAddress),
                    record.valueByteCount,
                    RuntimeAccess::Read,
                    &reason))
            {
                if (outReason)
                    *outReason = reason;
                return false;
            }

            const uint32_t copyBytes = std::min<uint32_t>(record.valueByteCount, static_cast<uint32_t>(record.view.nativeValue.size()));
            std::memcpy(record.view.nativeValue.data(), reinterpret_cast<const void*>(valueAddress), copyBytes);
            const uint32_t bitsBytes = std::min<uint32_t>(record.valueByteCount, static_cast<uint32_t>(sizeof(record.valueBits)));
            std::memcpy(&record.valueBits, reinterpret_cast<const void*>(valueAddress), bitsBytes);
            record.valueReadable = true;
        }

        const char* rawName = nullptr;
        if (TryResolveAttrName(store, valueCursor, tokenView.token, rawName, nullptr) && rawName && rawName[0])
        {
            record.name = CoopRuntimeGuards::ReadRuntimeCString(rawName, 160);
            record.nameResolved = !record.name.empty();
        }

        if (record.nameResolved)
        {
            ReadAttributeView nativeAttribute;
            if (TryFindReadAttribute(store, node, record.name.c_str(), nativeAttribute, nullptr))
            {
                record.view = nativeAttribute;
                const bool maybeIntegral =
                    nativeAttribute.type == 0 ||
                    nativeAttribute.type == 1 ||
                    nativeAttribute.type == 7 ||
                    nativeAttribute.type == 8 ||
                    nativeAttribute.type == 9 ||
                    nativeAttribute.type == 10 ||
                    nativeAttribute.type == 11 ||
                    nativeAttribute.type >= 19;
                if (maybeIntegral)
                {
                    uint32_t nativeU32 = 0;
                    if (TryReadU32Attribute(nativeAttribute, nativeU32, nullptr))
                    {
                        record.nativeU32 = nativeU32;
                        record.nativeU32Readable = true;
                    }
                }
            }
        }

        outAttributes.push_back(std::move(record));

        if (record.valueByteCount > 0)
        {
            if (valueCursor > 0xFFFFFFFFu - record.valueByteCount)
            {
                if (outReason)
                    *outReason = "attr value cursor overflow";
                return false;
            }
            valueCursor += record.valueByteCount;
        }
    }

    return true;
}

bool TryReadU32Attribute(const ReadAttributeView& attribute, uint32_t& outValue, std::string* outReason)
{
    outValue = 0;
    if (!attribute.valid)
    {
        if (outReason)
            *outReason = "attribute is not valid";
        return false;
    }

    std::string reason;
    if (!PreflightRuntimePointer(
            "native save store api ReadU32Attribute function",
            s_funcReadU32Attribute.GetVoidPtr(),
            1,
            RuntimeAccess::Execute,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    bool readOk = false;
    if (!TryGuardedCall(
            "native save store api ReadU32Attribute",
            [&]() -> bool
            {
                s_funcReadU32Attribute(
                    const_cast<uint8_t*>(attribute.nativeValue.data()),
                    &outValue);
                return true;
            },
            readOk,
            &reason) ||
        !readOk)
    {
        if (outReason)
            *outReason = reason.empty() ? "ReadU32Attribute failed" : reason;
        return false;
    }
    return true;
}

bool TryReadI32Attribute(const ReadAttributeView& attribute, int32_t& outValue, std::string* outReason)
{
    outValue = 0;
    if (!attribute.valid)
    {
        if (outReason)
            *outReason = "attribute is not valid";
        return false;
    }

    std::string reason;
    if (!PreflightRuntimePointer(
            "native save store api ReadI32Attribute function",
            s_funcReadI32Attribute.GetVoidPtr(),
            1,
            RuntimeAccess::Execute,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    bool readOk = false;
    if (!TryGuardedCall(
            "native save store api ReadI32Attribute",
            [&]() -> bool
            {
                s_funcReadI32Attribute(
                    const_cast<uint8_t*>(attribute.nativeValue.data()),
                    &outValue);
                return true;
            },
            readOk,
            &reason) ||
        !readOk)
    {
        if (outReason)
            *outReason = reason.empty() ? "ReadI32Attribute failed" : reason;
        return false;
    }
    return true;
}

bool TryReadF32Attribute(const ReadAttributeView& attribute, float& outValue, std::string* outReason)
{
    outValue = 0.0f;
    if (!attribute.valid)
    {
        if (outReason)
            *outReason = "attribute is not valid";
        return false;
    }

    std::string reason;
    if (!PreflightRuntimePointer(
            "native save store api ReadF32Attribute function",
            s_funcReadF32Attribute.GetVoidPtr(),
            1,
            RuntimeAccess::Execute,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    bool readOk = false;
    if (!TryGuardedCall(
            "native save store api ReadF32Attribute",
            [&]() -> bool
            {
                s_funcReadF32Attribute(
                    const_cast<uint8_t*>(attribute.nativeValue.data()),
                    &outValue);
                return true;
            },
            readOk,
            &reason) ||
        !readOk)
    {
        if (outReason)
            *outReason = reason.empty() ? "ReadF32Attribute failed" : reason;
        return false;
    }
    return true;
}

bool TryReadU64Attribute(const ReadAttributeView& attribute, uint64_t& outValue, std::string* outReason)
{
    outValue = 0;
    if (!attribute.valid)
    {
        if (outReason)
            *outReason = "attribute is not valid";
        return false;
    }

    std::string reason;
    if (!PreflightRuntimePointer(
            "native save store api ReadU64Attribute function",
            s_funcReadU64Attribute.GetVoidPtr(),
            1,
            RuntimeAccess::Execute,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    bool readOk = false;
    if (!TryGuardedCall(
            "native save store api ReadU64Attribute",
            [&]() -> bool
            {
                s_funcReadU64Attribute(
                    const_cast<uint8_t*>(attribute.nativeValue.data()),
                    &outValue);
                return true;
            },
            readOk,
            &reason) ||
        !readOk)
    {
        if (outReason)
            *outReason = reason.empty() ? "ReadU64Attribute failed" : reason;
        return false;
    }
    return true;
}

bool TryReadBoolAttribute(const ReadAttributeView& attribute, bool& outValue, std::string* outReason)
{
    outValue = false;
    if (!attribute.valid)
    {
        if (outReason)
            *outReason = "attribute is not valid";
        return false;
    }

    std::string reason;
    if (!PreflightRuntimePointer(
            "native save store api ReadBoolAttribute function",
            s_funcReadBoolAttribute.GetVoidPtr(),
            1,
            RuntimeAccess::Execute,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    bool readOk = false;
    if (!TryGuardedCall(
            "native save store api ReadBoolAttribute",
            [&]() -> bool
            {
                s_funcReadBoolAttribute(
                    const_cast<uint8_t*>(attribute.nativeValue.data()),
                    &outValue);
                return true;
            },
            readOk,
            &reason) ||
        !readOk)
    {
        if (outReason)
            *outReason = reason.empty() ? "ReadBoolAttribute failed" : reason;
        return false;
    }
    return true;
}

bool TryMaterializeWriteAttrToken(const void* writeAttrRecord, uint16_t& outToken, std::string* outReason)
{
    outToken = 0;
    std::string reason;
    if (!PreflightRuntimePointer(
            "native save store api write attr record",
            writeAttrRecord,
            kWriteAttrRecordBytes,
            RuntimeAccess::Read,
            &reason) ||
        !PreflightRuntimePointer(
            "native save store api MaterializeWriteAttrToken function",
            s_funcMaterializeWriteAttrToken.GetVoidPtr(),
            1,
            RuntimeAccess::Execute,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    uint16_t token = 0;
    if (!TryGuardedCall(
            "native save store api MaterializeWriteAttrToken",
            [&]() -> uint16_t
            {
                return s_funcMaterializeWriteAttrToken(const_cast<void*>(writeAttrRecord));
            },
            token,
            &reason))
    {
        if (outReason)
            *outReason = reason.empty() ? "MaterializeWriteAttrToken failed" : reason;
        return false;
    }

    outToken = token;
    return true;
}

bool TryGetAttrTypePayloadSize(uint32_t attrType, int32_t& outBytes, std::string* outReason)
{
    outBytes = -1;
    if (attrType > 0x3Fu)
    {
        if (outReason)
            *outReason = "attr type outside guarded range";
        return false;
    }

    std::string reason;
    if (!PreflightRuntimePointer(
            "native save store api AttrTypePayloadSize function",
            s_funcAttrTypePayloadSize.GetVoidPtr(),
            1,
            RuntimeAccess::Execute,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    int32_t bytes = -1;
    if (!TryGuardedCall(
            "native save store api AttrTypePayloadSize",
            [&]() -> int32_t
            {
                return s_funcAttrTypePayloadSize(static_cast<int32_t>(attrType));
            },
            bytes,
            &reason))
    {
        if (outReason)
            *outReason = reason.empty() ? "AttrTypePayloadSize failed" : reason;
        return false;
    }

    outBytes = bytes;
    return bytes >= 0;
}

bool TryResolveWriteStoreName(
    std::uintptr_t writeStore,
    uint32_t nameIndex,
    const char*& outName,
    std::string* outReason)
{
    outName = nullptr;
    if (writeStore == 0)
    {
        if (outReason)
            *outReason = "write store is null";
        return false;
    }
    if (nameIndex > 0x100000u)
    {
        if (outReason)
            *outReason = "write store name index outside guarded range";
        return false;
    }

    const std::uintptr_t contextPtr = writeStore + 0xF8;
    std::string reason;
    if (!PreflightRuntimePointer(
            "native save store api write name context",
            reinterpret_cast<const void*>(contextPtr),
            0x38,
            RuntimeAccess::Read,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    WriteStoreNameContext context;
    if (!TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(contextPtr + 0x00), context.blockVectorBegin) ||
        !TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(contextPtr + 0x08), context.blockVectorEnd) ||
        !TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(contextPtr + 0x28), context.nameRecordBegin) ||
        !TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(contextPtr + 0x30), context.nameRecordEnd))
    {
        if (outReason)
            *outReason = "write store name context read failed";
        return false;
    }

    if (context.nameRecordBegin == 0 ||
        context.nameRecordEnd < context.nameRecordBegin ||
        ((context.nameRecordEnd - context.nameRecordBegin) % kWriteStoreNameRecordBytes) != 0)
    {
        if (outReason)
            *outReason = "write store name record vector invalid";
        return false;
    }

    const uint64_t nameCount64 = (context.nameRecordEnd - context.nameRecordBegin) / kWriteStoreNameRecordBytes;
    if (nameIndex >= nameCount64)
    {
        if (outReason)
            *outReason = "write store name index outside vector";
        return false;
    }

    const std::uintptr_t recordPtr =
        context.nameRecordBegin + static_cast<std::uintptr_t>(nameIndex) * kWriteStoreNameRecordBytes;
    if (!PreflightRuntimePointer(
            "native save store api write name record",
            reinterpret_cast<const void*>(recordPtr),
            kWriteStoreNameRecordBytes,
            RuntimeAccess::Read,
            &reason))
    {
        if (outReason)
            *outReason = reason;
        return false;
    }

    WriteStoreNameRecord record;
    if (!TryReadRuntimeValue(reinterpret_cast<const uint16_t*>(recordPtr + 0x0), record.stringOffset) ||
        !TryReadRuntimeValue(reinterpret_cast<const uint16_t*>(recordPtr + 0x2), record.blockIndex) ||
        !TryReadRuntimeValue(reinterpret_cast<const uint64_t*>(recordPtr + 0x8), record.hash))
    {
        if (outReason)
            *outReason = "write store name record read failed";
        return false;
    }

    if (context.blockVectorBegin == 0 ||
        context.blockVectorEnd < context.blockVectorBegin ||
        ((context.blockVectorEnd - context.blockVectorBegin) % sizeof(std::uintptr_t)) != 0)
    {
        if (outReason)
            *outReason = "write store name block vector invalid";
        return false;
    }

    const uint64_t blockCount64 = (context.blockVectorEnd - context.blockVectorBegin) / sizeof(std::uintptr_t);
    if (record.blockIndex >= blockCount64)
    {
        if (outReason)
            *outReason = "write store name block index outside vector";
        return false;
    }

    const std::uintptr_t blockSlotPtr =
        context.blockVectorBegin + static_cast<std::uintptr_t>(record.blockIndex) * sizeof(std::uintptr_t);
    std::uintptr_t blockPtr = 0;
    std::uintptr_t stringBase = 0;
    if (!PreflightRuntimePointer(
            "native save store api write name block slot",
            reinterpret_cast<const void*>(blockSlotPtr),
            sizeof(std::uintptr_t),
            RuntimeAccess::Read,
            &reason) ||
        !TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(blockSlotPtr), blockPtr) ||
        !PreflightRuntimePointer(
            "native save store api write name block",
            reinterpret_cast<const void*>(blockPtr + 0x10),
            sizeof(std::uintptr_t),
            RuntimeAccess::Read,
            &reason) ||
        !TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(blockPtr + 0x10), stringBase))
    {
        if (outReason)
            *outReason = reason.empty() ? "write store name block read failed" : reason;
        return false;
    }

    const std::uintptr_t stringPtr = stringBase + record.stringOffset;
    if (stringPtr < stringBase ||
        !PreflightRuntimePointer(
            "native save store api write name string",
            reinterpret_cast<const void*>(stringPtr),
            1,
            RuntimeAccess::Read,
            &reason))
    {
        if (outReason)
            *outReason = reason.empty() ? "write store name string invalid" : reason;
        return false;
    }

    const std::string value = CoopRuntimeGuards::ReadRuntimeCString(
        reinterpret_cast<const char*>(stringPtr),
        256);
    if (value.empty())
    {
        if (outReason)
            *outReason = "write store name string empty";
        return false;
    }

    outName = reinterpret_cast<const char*>(stringPtr);
    return true;
}

CheckResult CheckReadNodeByIndex(const CoopSaveStoreDecoder::StoreMap& map)
{
    CheckResult result;
    result.attempted = true;
    result.store = MakeReadStoreHandle(map);
    result.decodedNode = MakeDecodedNodeView(map.currentNode);

    if (!result.store.valid)
    {
        result.reason = map.ok ? "not_read_store" : "store_map_not_ok";
        return result;
    }

    if (!map.currentNode.readable || !map.currentNode.valid)
    {
        result.reason = "decoded_current_node_not_valid";
        return result;
    }

    std::string reason;
    if (!TryReadNodeByIndex(result.store, map.currentNode.nodeIndex, result.nativeNode, &reason))
    {
        ++result.guards;
        result.reason = reason.empty() ? "native_read_failed" : reason;
        return result;
    }

    result.mismatches = CountNodeMismatches(result.nativeNode, result.decodedNode);
    result.ok = result.mismatches == 0;
    result.reason = result.ok ? "ok" : "node_mismatch";

    if (result.ok)
    {
        result.nameToken = result.decodedNode.nodeId;
        const char* nativeName = nullptr;
        std::string nameReason;
        if (TryResolveChildNameToken(result.store, result.nameToken, nativeName, &nameReason))
        {
            uint32_t computedOffset = 0;
            const std::uintptr_t offsetAddress =
                result.store.childNameOffsetTable + static_cast<std::uintptr_t>(result.nameToken) * sizeof(uint32_t);
            if (TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(offsetAddress), computedOffset))
            {
                result.nativeNamePtr = reinterpret_cast<std::uintptr_t>(nativeName);
                result.computedNamePtr = result.store.childNameDataBase + computedOffset;
                result.nameChecks = 1;
                result.nameMismatches = result.nativeNamePtr == result.computedNamePtr ? 0u : 1u;
                if (result.nameMismatches != 0)
                {
                    result.ok = false;
                    result.reason = "name_token_mismatch";
                }
            }
            else
            {
                ++result.guards;
            }
        }
    }

    if (result.ok && result.decodedNode.attrCount > 0)
    {
        result.attrCursor = result.decodedNode.attrCursor;
        result.attrOrdinal = 0;
        const uint32_t attrTokenCursor = result.decodedNode.nodeId;
        result.attrTokenCursor = attrTokenCursor;
        std::string attrReason;
        uint16_t nativeToken = 0;
        if (TryReadAttrNameToken(result.store, attrTokenCursor, result.attrOrdinal, nativeToken, &attrReason))
        {
            result.nativeAttrToken = nativeToken;
            uint16_t tokenByteOffset = 0;
            const std::uintptr_t indexAddress =
                result.store.attrTokenIndexBase + static_cast<std::uintptr_t>(attrTokenCursor) * sizeof(uint16_t);
            if (TryReadRuntimeValue(reinterpret_cast<const uint16_t*>(indexAddress), tokenByteOffset))
            {
                const std::uintptr_t tokenAddress =
                    result.store.attrTokenBase +
                    static_cast<std::uintptr_t>(tokenByteOffset) +
                    static_cast<std::uintptr_t>(result.attrOrdinal) * sizeof(uint16_t);
                TryReadRuntimeValue(reinterpret_cast<const uint16_t*>(tokenAddress), result.computedAttrToken);
                ++result.attrChecks;
                if (result.nativeAttrToken != result.computedAttrToken)
                    ++result.attrMismatches;
            }
            else
            {
                ++result.guards;
            }

            const char* nativeAttrName = nullptr;
            if (TryResolveAttrName(result.store, result.attrCursor, result.nativeAttrToken, nativeAttrName, &attrReason))
            {
                const uint32_t nameIndex = static_cast<uint32_t>(result.nativeAttrToken >> 6);
                uint32_t computedOffset = 0;
                const std::uintptr_t offsetAddress =
                    result.store.attrNameOffsetTable + static_cast<std::uintptr_t>(nameIndex) * sizeof(uint32_t);
                if (TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(offsetAddress), computedOffset))
                {
                    result.nativeAttrNamePtr = reinterpret_cast<std::uintptr_t>(nativeAttrName);
                    result.computedAttrNamePtr = result.store.attrStringBase + computedOffset;
                    ++result.attrChecks;
                    if (result.nativeAttrNamePtr != result.computedAttrNamePtr)
                        ++result.attrMismatches;
                }
                else
                {
                    ++result.guards;
                }
            }
        }

        const uint32_t maxProbeAttrs = std::min<uint32_t>(result.decodedNode.attrCount, 16u);
        for (uint32_t probeOrdinal = 0; probeOrdinal < maxProbeAttrs; ++probeOrdinal)
        {
            AttrTokenView probeToken;
            const char* probeName = nullptr;
            if (!TryReadAttrTokenView(result.store, attrTokenCursor, probeOrdinal, probeToken, &attrReason) ||
                !TryResolveAttrName(result.store, result.attrCursor, probeToken.token, probeName, &attrReason) ||
                !probeName)
            {
                continue;
            }

            ReadAttributeView readAttribute;
            ++result.nativeReadValueChecks;
            if (!TryFindReadAttribute(result.store, result.decodedNode, probeName, readAttribute, &attrReason))
            {
                ++result.nativeReadValueFailures;
                result.nativeReadValueReason = attrReason.empty() ? "find_attr_failed" : attrReason;
                continue;
            }

            result.nativeReadValueType = readAttribute.type;
            bool readValueOk = false;
            if (readAttribute.type == 2 || readAttribute.type == 0xBu)
            {
                float value = 0.0f;
                if (TryReadF32Attribute(readAttribute, value, &attrReason))
                {
                    uint32_t bits = 0;
                    std::memcpy(&bits, &value, sizeof(bits));
                    result.nativeReadValueBits = bits;
                    readValueOk = true;
                }
            }
            else if (readAttribute.type == 5)
            {
                uint64_t value = 0;
                if (TryReadU64Attribute(readAttribute, value, &attrReason))
                {
                    result.nativeReadValueBits = value;
                    readValueOk = true;
                }
            }
            else if (
                readAttribute.type == 1 ||
                readAttribute.type == 7 ||
                readAttribute.type == 8 ||
                readAttribute.type == 9 ||
                readAttribute.type == 10 ||
                (readAttribute.type >= 0x13u && readAttribute.type <= 0x1Eu))
            {
                int32_t value = 0;
                if (TryReadI32Attribute(readAttribute, value, &attrReason))
                {
                    result.nativeReadValueBits = static_cast<uint32_t>(value);
                    readValueOk = true;
                }
            }
            else
            {
                result.nativeReadValueReason = "non_scalar_type_" + std::to_string(readAttribute.type);
                continue;
            }

            if (readValueOk)
            {
                result.nativeReadValueReason = std::string("ok_") + StatusToken(probeName);
                break;
            }

            ++result.nativeReadValueFailures;
            result.nativeReadValueReason = attrReason.empty() ? "read_scalar_failed" : attrReason;
        }

        if (result.nativeReadValueChecks == 0 && result.nativeReadValueReason.empty())
            result.nativeReadValueReason = "no_attr_probe";

        if (result.attrMismatches != 0)
        {
            result.ok = false;
            result.reason = "attr_mismatch";
        }
    }

    ProbeReadStoreSection(result.store, "GameState", result);
    return result;
}

std::string BuildCheckStatus(const CheckResult& result)
{
    std::ostringstream out;
    out << (result.attempted ? 1 : 0)
        << "/" << (result.ok ? "ok" : "blocked")
        << "/reason=" << StatusToken(result.reason)
        << "/read=" << (result.store.readStore ? 1 : 0)
        << "/store=" << PointerHex(result.store.store)
        << "/nodes=" << result.store.nodeCount
        << "/idx=" << result.decodedNode.nodeIndex
        << "/nativePtr=" << PointerHex(result.nativeNode.nodePtr)
        << "/decodedPtr=" << PointerHex(result.decodedNode.nodePtr)
        << "/nativeId=" << result.nativeNode.nodeId
        << "/decodedId=" << result.decodedNode.nodeId
        << "/mismatch=" << result.mismatches
        << "/nameToken=" << result.nameToken
        << "/nameCheck=" << result.nameChecks
        << "/nameMismatch=" << result.nameMismatches
        << "/nativeName=" << PointerHex(result.nativeNamePtr)
        << "/computedName=" << PointerHex(result.computedNamePtr)
        << "/attrCursor=" << result.attrCursor
        << "/attrTokenCursor=" << result.attrTokenCursor
        << "/attrOrdinal=" << result.attrOrdinal
        << "/attrCheck=" << result.attrChecks
        << "/attrMismatch=" << result.attrMismatches
        << "/nativeAttrToken=" << result.nativeAttrToken
        << "/computedAttrToken=" << result.computedAttrToken
        << "/nativeAttrName=" << PointerHex(result.nativeAttrNamePtr)
        << "/computedAttrName=" << PointerHex(result.computedAttrNamePtr)
        << "/readValue=" << result.nativeReadValueChecks << "/" << result.nativeReadValueFailures
        << "/readValueType=" << result.nativeReadValueType
        << "/readValueBits=" << HexU64(result.nativeReadValueBits)
        << "/readValueReason=" << StatusToken(result.nativeReadValueReason)
        << "/loadSection=" << result.loadSectionChecks
        << "/gameState=" << (result.gameStateSectionFound ? 1 : 0)
        << "/gameStateIdx=" << result.gameStateSectionNodeIndex
        << "/gameStateId=" << result.gameStateSectionNodeId
        << "/gameStatePtr=" << PointerHex(result.gameStateSectionNodePtr)
        << "/loadGuards=" << result.loadSectionGuards
        << "/loadReason=" << StatusToken(result.gameStateSectionReason)
        << "/guards=" << result.guards;
    return out.str();
}

WriteStoreProbeResult ProbeWriteStoreTrampolines(
    void* storeOwner,
    const std::array<std::uintptr_t, 3>& stackEntry)
{
    WriteStoreProbeResult result;
    result.attempted = true;
    result.storeOwner = reinterpret_cast<std::uintptr_t>(storeOwner);
    result.functionChecks = 7;

    if (!IsWriteProbeEnabled())
    {
        result.reason = "disabled";
        return result;
    }

    const auto* entryBytes = reinterpret_cast<const uint8_t*>(stackEntry.data());
    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(entryBytes), result.stackStore);
    TryReadRuntimeValue(reinterpret_cast<const int32_t*>(entryBytes + 0x8), result.nodeIndex);
    TryReadRuntimeValue(reinterpret_cast<const int32_t*>(entryBytes + 0xC), result.generation);

    if (result.stackStore == 0 || result.nodeIndex < 0 || result.nodeIndex > 100000)
    {
        result.reason =
            "bad_stack_entry store=" + PointerHex(result.stackStore) +
            " idx=" + std::to_string(result.nodeIndex);
        ++result.guards;
        return result;
    }

    std::string reason;
    void* resolvedNode = nullptr;
    const bool resolved = TryGuardedCall(
        "native save store api write stack entry to node",
        [&]() -> void*
        {
            return s_funcWriteStackEntryToNode(const_cast<std::uintptr_t*>(stackEntry.data()));
        },
        resolvedNode,
        &reason);
    ++result.nativeLookups;
    if (!resolved)
    {
        ++result.guards;
    }
    else if (resolvedNode)
    {
        result.resolvedNode = reinterpret_cast<std::uintptr_t>(resolvedNode);
    }

    void* indexedNode = nullptr;
    const bool indexed = TryGuardedCall(
        "native save store api write node by index",
        [&]() -> void*
        {
            return s_funcWriteNodeByIndex(
                reinterpret_cast<void*>(result.stackStore),
                static_cast<uint32_t>(result.nodeIndex));
        },
        indexedNode,
        &reason);
    ++result.nativeLookups;
    if (indexed && indexedNode)
    {
        result.nodeByIndex = reinterpret_cast<std::uintptr_t>(indexedNode);
        if (result.resolvedNode != 0 && result.nodeByIndex != result.resolvedNode)
            ++result.mismatches;
    }
    else
    {
        ++result.guards;
    }

    const std::uintptr_t nodeToRead = result.resolvedNode != 0 ? result.resolvedNode : result.nodeByIndex;
    if (nodeToRead == 0)
    {
        result.reason = "write_node_lookup_failed";
        if (!reason.empty())
            result.reason += ":" + StatusToken(reason);
        ++result.guards;
        return result;
    }

    WriteNodeProbeView nodeView;
    if (!ReadWriteNodeProbeView(nodeToRead, nodeView, &reason))
    {
        result.reason = "write_node_read_failed";
        if (!reason.empty())
            result.reason += ":" + StatusToken(reason);
        ++result.guards;
        return result;
    }

    if (nodeView.store != 0 && nodeView.store != result.stackStore)
        ++result.mismatches;
    if (nodeView.nodeIndex != static_cast<uint32_t>(result.nodeIndex))
        ++result.mismatches;

    result.nameToken = nodeView.nameToken;
    result.childCount = nodeView.childCount;
    result.attrRecords = VectorRecordCount(nodeView.attrBegin, nodeView.attrEnd, kWriteAttrRecordBytes);
    result.childRecords = VectorRecordCount(nodeView.childBegin, nodeView.childEnd, sizeof(uint32_t));
    result.finalizeCursor = nodeView.storeFinalizeCursor;
    result.valid = nodeView.valid;
    result.finalized = nodeView.finalized;
    result.ok = result.mismatches == 0 && result.guards == 0;
    result.reason = result.ok ? "ok" : "checked_with_warnings";
    return result;
}

std::string BuildWriteStoreProbeStatus(const WriteStoreProbeResult& result)
{
    std::ostringstream out;
    out << (result.ok ? "ok" : "blocked")
        << "/attempted=" << (result.attempted ? 1 : 0)
        << "/reason=" << StatusToken(result.reason)
        << "/storeOwner=" << PointerHex(result.storeOwner)
        << "/stackStore=" << PointerHex(result.stackStore)
        << "/idx=" << result.nodeIndex
        << "/gen=" << result.generation
        << "/node=" << PointerHex(result.resolvedNode)
        << "/byIndex=" << PointerHex(result.nodeByIndex)
        << "/nameToken=" << result.nameToken
        << "/attrs=" << result.attrRecords
        << "/children=" << result.childRecords
        << "/childCount=" << result.childCount
        << "/finalizeCursor=" << result.finalizeCursor
        << "/valid=" << static_cast<uint32_t>(result.valid)
        << "/finalized=" << static_cast<uint32_t>(result.finalized)
        << "/func=" << result.functionChecks
        << "/lookup=" << result.nativeLookups
        << "/mismatch=" << result.mismatches
        << "/guards=" << result.guards;
    return out.str();
}

WriteAllocatorProbeResult ProbeWriteAllocatorReadiness(
    void* storeOwner,
    const std::array<std::uintptr_t, 3>& stackEntry,
    uint32_t requestedNodeAllocs,
    uint32_t requestedBackingAllocs,
    uint32_t requestedEntityAllocs)
{
    WriteAllocatorProbeResult result;
    result.attempted = true;
    result.storeOwner = reinterpret_cast<std::uintptr_t>(storeOwner);
    result.requestedNodeAllocs = requestedNodeAllocs;
    result.requestedBackingAllocs = requestedBackingAllocs;
    result.requestedEntityAllocs = requestedEntityAllocs;

    if (!IsWriteAllocatorProbeEnabled())
    {
        result.reason = "disabled";
        return result;
    }

    result.readyStackEntryToNode = CheckExecutableFunction(
        "WriteStackEntryToNode",
        s_funcWriteStackEntryToNode.GetVoidPtr(),
        result) ? 1u : 0u;
    result.readyNodeByIndex = CheckExecutableFunction(
        "WriteNodeByIndex",
        s_funcWriteNodeByIndex.GetVoidPtr(),
        result) ? 1u : 0u;
    result.readyCreateNode = CheckExecutableFunction(
        "WriteCreateNode",
        s_funcWriteCreateNode.GetVoidPtr(),
        result) ? 1u : 0u;
    result.readyCreateChildGroup = CheckExecutableFunction(
        "WriteCreateChildGroup",
        s_funcWriteCreateChildGroup.GetVoidPtr(),
        result) ? 1u : 0u;
    result.readyAppendAttribute = CheckExecutableFunction(
        "WriteAppendAttribute",
        s_funcWriteAppendAttribute.GetVoidPtr(),
        result) ? 1u : 0u;
    result.readyFinalizeNode = CheckExecutableFunction(
        "WriteFinalizeNode",
        s_funcWriteFinalizeNode.GetVoidPtr(),
        result) ? 1u : 0u;
    result.readySaveStackPush = CheckExecutableFunction(
        "WriteSaveStackPush",
        s_funcWriteSaveStackPush.GetVoidPtr(),
        result) ? 1u : 0u;

    const auto* entryBytes = reinterpret_cast<const uint8_t*>(stackEntry.data());
    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(entryBytes), result.stackStore);
    TryReadRuntimeValue(reinterpret_cast<const int32_t*>(entryBytes + 0x8), result.nodeIndex);
    TryReadRuntimeValue(reinterpret_cast<const int32_t*>(entryBytes + 0xC), result.generation);

    if (result.stackStore == 0 || result.nodeIndex < 0 || result.nodeIndex > 100000)
    {
        result.reason =
            "bad_stack_entry store=" + PointerHex(result.stackStore) +
            " idx=" + std::to_string(result.nodeIndex);
        ++result.guards;
        return result;
    }

    std::string reason;
    if (!PreflightRuntimePointer(
            "native save store api write allocator store",
            reinterpret_cast<const void*>(result.stackStore),
            0x408,
            RuntimeAccess::Read,
            &reason))
    {
        result.reason = "write_store_unreadable";
        if (!reason.empty())
            result.reason += ":" + StatusToken(reason);
        ++result.guards;
        return result;
    }

    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(result.stackStore + 0x0), result.nodeTableBegin);
    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(result.stackStore + 0x8), result.nodeTableEnd);
    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(result.stackStore + 0x10), result.nodeTableCapacity);
    TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(result.stackStore + 0x3F8), result.nextFreeNodeIndex);
    TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(result.stackStore + 0x3FC), result.storeGeneration);
    TryReadRuntimeValue(reinterpret_cast<const uint32_t*>(result.stackStore + 0x400), result.finalizeCursor);

    if (result.nodeTableBegin == 0 ||
        result.nodeTableEnd < result.nodeTableBegin ||
        ((result.nodeTableEnd - result.nodeTableBegin) % sizeof(std::uintptr_t)) != 0)
    {
        result.reason = "bad_node_table_vector";
        ++result.guards;
        return result;
    }

    if (!PreflightRuntimePointer(
            "native save store api write allocator node table",
            reinterpret_cast<const void*>(result.nodeTableBegin),
            static_cast<size_t>(result.nodeTableEnd - result.nodeTableBegin),
            RuntimeAccess::Read,
            &reason))
    {
        result.reason = "node_table_unreadable";
        if (!reason.empty())
            result.reason += ":" + StatusToken(reason);
        ++result.guards;
        return result;
    }

    result.nodeTableSlots = VectorRecordCount(result.nodeTableBegin, result.nodeTableEnd, sizeof(std::uintptr_t));
    if (result.nextFreeNodeIndex < result.nodeTableSlots)
        result.availableNodeSlots = result.nodeTableSlots - result.nextFreeNodeIndex;

    void* rootNode = nullptr;
    const bool indexed = TryGuardedCall(
        "native save store api write allocator node by index",
        [&]() -> void*
        {
            return s_funcWriteNodeByIndex(
                reinterpret_cast<void*>(result.stackStore),
                static_cast<uint32_t>(result.nodeIndex));
        },
        rootNode,
        &reason);
    if (!indexed || !rootNode)
    {
        result.reason = "root_node_lookup_failed";
        if (!reason.empty())
            result.reason += ":" + StatusToken(reason);
        ++result.guards;
        return result;
    }

    result.rootNode = reinterpret_cast<std::uintptr_t>(rootNode);
    WriteNodeProbeView rootView;
    if (!ReadWriteNodeProbeView(result.rootNode, rootView, &reason))
    {
        result.reason = "root_node_read_failed";
        if (!reason.empty())
            result.reason += ":" + StatusToken(reason);
        ++result.guards;
        return result;
    }

    if (rootView.store != 0 && rootView.store != result.stackStore)
        ++result.mismatches;
    if (rootView.nodeIndex != static_cast<uint32_t>(result.nodeIndex))
        ++result.mismatches;

    result.rootChildCount = rootView.childCount;
    result.rootAttrRecords = VectorRecordCount(rootView.attrBegin, rootView.attrEnd, kWriteAttrRecordBytes);
    result.rootChildRecords = VectorRecordCount(rootView.childBegin, rootView.childEnd, sizeof(uint32_t));

    if (requestedNodeAllocs != 0 && result.availableNodeSlots < requestedNodeAllocs)
    {
        result.reason =
            "node_capacity_low requested=" + std::to_string(requestedNodeAllocs) +
            " available=" + std::to_string(result.availableNodeSlots);
        ++result.guards;
        return result;
    }

    result.ok =
        result.guards == 0 &&
        result.mismatches == 0 &&
        result.functionsReady == result.functionChecks &&
        result.nodeTableSlots > 0 &&
        result.rootNode != 0;
    if (result.reason.empty())
        result.reason = result.ok ? "ok" : "checked_with_warnings";
    return result;
}

std::string BuildWriteAllocatorProbeStatus(const WriteAllocatorProbeResult& result)
{
    std::ostringstream out;
    out << (result.ok ? "ok" : "blocked")
        << "/attempted=" << (result.attempted ? 1 : 0)
        << "/reason=" << StatusToken(result.reason)
        << "/storeOwner=" << PointerHex(result.storeOwner)
        << "/stackStore=" << PointerHex(result.stackStore)
        << "/idx=" << result.nodeIndex
        << "/gen=" << result.generation
        << "/root=" << PointerHex(result.rootNode)
        << "/table=" << PointerHex(result.nodeTableBegin) << ".." << PointerHex(result.nodeTableEnd)
        << "/" << PointerHex(result.nodeTableCapacity)
        << "/slots=" << result.nodeTableSlots
        << "/nextFree=" << result.nextFreeNodeIndex
        << "/available=" << result.availableNodeSlots
        << "/storeGen=" << result.storeGeneration
        << "/finalizeCursor=" << result.finalizeCursor
        << "/request=" << result.requestedNodeAllocs << "," << result.requestedBackingAllocs << "," << result.requestedEntityAllocs
        << "/rootAttrs=" << result.rootAttrRecords
        << "/rootChildren=" << result.rootChildRecords
        << "/rootChildCount=" << result.rootChildCount
        << "/func=" << result.functionsReady << "/" << result.functionChecks
        << "/ready=" << result.readyStackEntryToNode
        << result.readyNodeByIndex
        << result.readyCreateNode
        << result.readyCreateChildGroup
        << result.readyAppendAttribute
        << result.readyFinalizeNode
        << result.readySaveStackPush
        << "/mismatch=" << result.mismatches
        << "/guards=" << result.guards;
    return out.str();
}

WriteStoreBuilderBackendStatus CheckWriteStoreBuilderBackend()
{
    WriteStoreBuilderBackendStatus result;
    result.attempted = true;

    result.readyStackEntryToNode = CheckBuilderExecutableFunction(
        "WriteStackEntryToNode",
        s_funcWriteStackEntryToNode.GetVoidPtr(),
        result) ? 1u : 0u;
    result.readyNodeByIndex = CheckBuilderExecutableFunction(
        "WriteNodeByIndex",
        s_funcWriteNodeByIndex.GetVoidPtr(),
        result) ? 1u : 0u;
    result.readyCreateNode = CheckBuilderExecutableFunction(
        "WriteCreateNode",
        s_funcWriteCreateNode.GetVoidPtr(),
        result) ? 1u : 0u;
    result.readyCreateChildGroup = CheckBuilderExecutableFunction(
        "WriteCreateChildGroup",
        s_funcWriteCreateChildGroup.GetVoidPtr(),
        result) ? 1u : 0u;
    result.readyAppendAttribute = CheckBuilderExecutableFunction(
        "WriteAppendAttribute",
        s_funcWriteAppendAttribute.GetVoidPtr(),
        result) ? 1u : 0u;
    result.readyFinalizeNode = CheckBuilderExecutableFunction(
        "WriteFinalizeNode",
        s_funcWriteFinalizeNode.GetVoidPtr(),
        result) ? 1u : 0u;
    result.readySaveStackPush = CheckBuilderExecutableFunction(
        "WriteSaveStackPush",
        s_funcWriteSaveStackPush.GetVoidPtr(),
        result) ? 1u : 0u;

    result.ok = result.guards == 0 && result.functionsReady == result.functionChecks && result.functionChecks != 0;
    if (result.reason.empty())
        result.reason = result.ok ? "ok" : "checked_with_warnings";
    return result;
}

std::string BuildWriteStoreBuilderBackendStatus(const WriteStoreBuilderBackendStatus& result)
{
    std::ostringstream out;
    out << (result.ok ? "ok" : "blocked")
        << "/attempted=" << (result.attempted ? 1 : 0)
        << "/reason=" << StatusToken(result.reason)
        << "/func=" << result.functionsReady << "/" << result.functionChecks
        << "/ready=" << result.readyStackEntryToNode
        << result.readyNodeByIndex
        << result.readyCreateNode
        << result.readyCreateChildGroup
        << result.readyAppendAttribute
        << result.readyFinalizeNode
        << result.readySaveStackPush
        << "/guards=" << result.guards;
    return out.str();
}

bool IsWriteStoreBuilderBackendAvailable()
{
    return CheckWriteStoreBuilderBackend().ok;
}

ScratchActiveSaveWriterProbeResult ProbeScratchActiveSaveWriter()
{
    ScratchActiveSaveWriterProbeResult result;
    result.attempted = true;

    result.readyFactory = CheckScratchWriterExecutableFunction(
        "ActiveISaveGame::Factory",
        s_funcActiveSaveFactory.GetVoidPtr(),
        result) ? 1u : 0u;
    if (!result.readyFactory)
    {
        if (result.reason.empty())
            result.reason = "factory_unavailable";
        return result;
    }

    void* activeSave = nullptr;
    std::string reason;
    const bool constructCallOk = TryGuardedCall(
        "native scratch active save factory",
        [&]() -> void*
        {
            // Same factory path as the engine's full ActiveISaveGame object.
            // Null parent/save-name keeps this as a lifecycle probe; no
            // section is added and no file write is triggered here.
            return s_funcActiveSaveFactory(nullptr, nullptr, 0);
        },
        activeSave,
        &reason);
    if (!constructCallOk || !activeSave)
    {
        ++result.guards;
        result.reason = reason.empty() ? "factory_returned_null" : reason;
        return result;
    }

    result.constructed = true;
    result.activeSave = reinterpret_cast<std::uintptr_t>(activeSave);
    result.writeStore = result.activeSave + 0x50;

    bool destroyAttempted = false;
    auto destroyActiveSave = [&]()
    {
        if (!activeSave || destroyAttempted)
            return;
        destroyAttempted = true;

        void* vtablePtr = nullptr;
        if (!TryReadRuntimeValue(reinterpret_cast<void* const*>(activeSave), vtablePtr) || !vtablePtr)
        {
            ++result.guards;
            if (result.reason.empty())
                result.reason = "active_save_vtable_read_failed";
            return;
        }

        result.vtable = reinterpret_cast<std::uintptr_t>(vtablePtr);
        void* dtorPtr = nullptr;
        if (!TryReadRuntimeValue(reinterpret_cast<void* const*>(vtablePtr), dtorPtr) || !dtorPtr)
        {
            ++result.guards;
            if (result.reason.empty())
                result.reason = "active_save_virtual_dtor_read_failed";
            return;
        }

        result.virtualDtor = reinterpret_cast<std::uintptr_t>(dtorPtr);
        result.readyVirtualDtor = CheckScratchWriterExecutableFunction(
            "ActiveISaveGame::virtual_dtor",
            dtorPtr,
            result) ? 1u : 0u;
        if (!result.readyVirtualDtor)
            return;

        using VirtualDtor = void* (*)(void*, uint32_t);
        void* dtorResult = nullptr;
        std::string destroyReason;
        const bool destroyed = TryGuardedCall(
            "native scratch active save destroy",
            [&]() -> void*
            {
                return reinterpret_cast<VirtualDtor>(dtorPtr)(activeSave, 1);
            },
            dtorResult,
            &destroyReason);
        result.destroyed = destroyed;
        if (!destroyed)
        {
            ++result.guards;
            if (result.reason.empty())
                result.reason = destroyReason.empty() ? "active_save_destroy_failed" : destroyReason;
        }
    };

    const bool activeReadable = PreflightRuntimePointer(
        "native scratch active save object",
        activeSave,
        0x470,
        RuntimeAccess::Read,
        &reason);
    if (!activeReadable)
    {
        ++result.guards;
        result.reason = reason.empty() ? "active_save_object_unreadable" : reason;
        destroyActiveSave();
        return result;
    }

    result.writeStoreReadable = PreflightRuntimePointer(
        "native scratch active save write store",
        reinterpret_cast<void*>(result.writeStore),
        0x410,
        RuntimeAccess::Read,
        &reason) ? 1u : 0u;
    result.writeStoreWritable = PreflightRuntimePointer(
        "native scratch active save write store",
        reinterpret_cast<void*>(result.writeStore),
        0x410,
        RuntimeAccess::Write,
        &reason) ? 1u : 0u;
    if (!result.writeStoreReadable || !result.writeStoreWritable)
    {
        ++result.guards;
        result.reason = reason.empty() ? "write_store_unavailable" : reason;
        destroyActiveSave();
        return result;
    }

    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(result.activeSave + 0x458), result.sectionCacheBegin);
    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(result.activeSave + 0x460), result.sectionCacheEnd);
    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(result.activeSave + 0x468), result.sectionCacheCapacity);
    result.sectionCacheEmpty =
        result.sectionCacheBegin == 0 &&
        result.sectionCacheEnd == 0 &&
        result.sectionCacheCapacity == 0 ? 1u : 0u;
    if (!result.sectionCacheEmpty)
    {
        ++result.guards;
        result.reason = "section_cache_not_empty_after_construct";
        destroyActiveSave();
        return result;
    }

    destroyActiveSave();
    if (!result.destroyed)
    {
        if (result.reason.empty())
            result.reason = "destroy_failed";
        return result;
    }

    result.ok = true;
    result.reason = "ok";
    return result;
}

std::string BuildScratchActiveSaveWriterProbeStatus(const ScratchActiveSaveWriterProbeResult& result)
{
    std::ostringstream out;
    out
        << "attempted=" << (result.attempted ? 1 : 0)
        << "/ok=" << (result.ok ? 1 : 0)
        << "/reason=" << StatusToken(result.reason)
        << "/constructed=" << (result.constructed ? 1 : 0)
        << "/destroyed=" << (result.destroyed ? 1 : 0)
        << "/active=" << PointerHex(result.activeSave)
        << "/vt=" << PointerHex(result.vtable)
        << "/dtor=" << PointerHex(result.virtualDtor)
        << "/writeStore=" << PointerHex(result.writeStore)
        << "/rw=" << result.writeStoreReadable << "," << result.writeStoreWritable
        << "/cache=" << PointerHex(result.sectionCacheBegin)
        << "," << PointerHex(result.sectionCacheEnd)
        << "," << PointerHex(result.sectionCacheCapacity)
        << "/empty=" << result.sectionCacheEmpty
        << "/func=" << result.functionsReady << "/" << result.functionChecks
        << "/ready=" << result.readyFactory << result.readyVirtualDtor
        << "/guards=" << result.guards;
    return out.str();
}

bool IsScratchActiveSaveWriterProbeEnabled()
{
    return EnvFlagEnabled("COOP_NATIVE_SCRATCH_SAVE_WRITER_PROBE");
}

ScratchActiveSaveSectionProbeResult ProbeScratchActiveSaveSectionWriter()
{
    ScratchActiveSaveSectionProbeResult result;
    result.attempted = true;

    result.readyFactory = CheckScratchSectionExecutableFunction(
        "ActiveISaveGame::Factory",
        s_funcActiveSaveFactory.GetVoidPtr(),
        result) ? 1u : 0u;
    result.readyInit = CheckScratchSectionExecutableFunction(
        "ActiveISaveGame::Init",
        s_funcActiveSaveInit.GetVoidPtr(),
        result) ? 1u : 0u;
    result.readyAddSection = CheckScratchSectionExecutableFunction(
        "ActiveISaveGame::AddSection",
        s_funcActiveSaveAddSectionApi.GetVoidPtr(),
        result) ? 1u : 0u;
    result.readyCopyRoot = CheckScratchSectionExecutableFunction(
        "SaveStore::CopyWriteRootStackEntry",
        s_funcCopyWriteRootStackEntry.GetVoidPtr(),
        result) ? 1u : 0u;
    result.readyResolveRoot = CheckScratchSectionExecutableFunction(
        "SaveStore::WriteStackEntryToNode",
        s_funcWriteStackEntryToNode.GetVoidPtr(),
        result) ? 1u : 0u;

    if (!result.readyFactory || !result.readyInit || !result.readyAddSection)
    {
        if (result.reason.empty())
            result.reason = "required_function_unavailable";
        return result;
    }

    void* activeSave = nullptr;
    std::string reason;
    const bool constructCallOk = TryGuardedCall(
        "native scratch active save section factory",
        [&]() -> void*
        {
            return s_funcActiveSaveFactory(nullptr, "CoopScratchNativeSave", 0);
        },
        activeSave,
        &reason);
    if (!constructCallOk || !activeSave)
    {
        ++result.guards;
        result.reason = reason.empty() ? "factory_returned_null" : reason;
        return result;
    }

    result.constructed = true;
    result.activeSave = reinterpret_cast<std::uintptr_t>(activeSave);
    result.writeStore = result.activeSave + 0x50;

    bool destroyAttempted = false;
    auto destroyActiveSave = [&]()
    {
        if (!activeSave || destroyAttempted)
            return;
        destroyAttempted = true;

        void* vtablePtr = nullptr;
        if (!TryReadRuntimeValue(reinterpret_cast<void* const*>(activeSave), vtablePtr) || !vtablePtr)
        {
            ++result.guards;
            if (result.reason.empty())
                result.reason = "active_save_vtable_read_failed";
            return;
        }

        result.vtable = reinterpret_cast<std::uintptr_t>(vtablePtr);
        void* dtorPtr = nullptr;
        if (!TryReadRuntimeValue(reinterpret_cast<void* const*>(vtablePtr), dtorPtr) || !dtorPtr)
        {
            ++result.guards;
            if (result.reason.empty())
                result.reason = "active_save_virtual_dtor_read_failed";
            return;
        }

        result.virtualDtor = reinterpret_cast<std::uintptr_t>(dtorPtr);
        result.readyVirtualDtor = CheckScratchSectionExecutableFunction(
            "ActiveISaveGame::virtual_dtor",
            dtorPtr,
            result) ? 1u : 0u;
        if (!result.readyVirtualDtor)
            return;

        using VirtualDtor = void* (*)(void*, uint32_t);
        void* dtorResult = nullptr;
        std::string destroyReason;
        const bool destroyed = TryGuardedCall(
            "native scratch active save section destroy",
            [&]() -> void*
            {
                return reinterpret_cast<VirtualDtor>(dtorPtr)(activeSave, 1);
            },
            dtorResult,
            &destroyReason);
        result.destroyed = destroyed;
        if (!destroyed)
        {
            ++result.guards;
            if (result.reason.empty())
                result.reason = destroyReason.empty() ? "active_save_destroy_failed" : destroyReason;
        }
    };

    if (!PreflightRuntimePointer(
            "native scratch active save section object",
            activeSave,
            0x470,
            RuntimeAccess::Read,
            &reason) ||
        !PreflightRuntimePointer(
            "native scratch active save section write store",
            reinterpret_cast<void*>(result.writeStore),
            0x410,
            RuntimeAccess::Write,
            &reason))
    {
        ++result.guards;
        result.reason = reason.empty() ? "scratch_active_save_unavailable" : reason;
        destroyActiveSave();
        return result;
    }

    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(result.activeSave + 0x458), result.sectionCacheBeginBefore);
    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(result.activeSave + 0x460), result.sectionCacheEndBefore);
    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(result.activeSave + 0x468), result.sectionCacheCapacityBefore);

    uint8_t initResult = 0;
    const bool initCallOk = TryGuardedCall(
        "native scratch active save Init",
        [&]() -> uint8_t
        {
            return s_funcActiveSaveInit(activeSave, "CoopScratchNativeSave");
        },
        initResult,
        &reason);
    result.initialized = initCallOk && initResult != 0;
    if (!result.initialized)
    {
        ++result.guards;
        result.reason = reason.empty() ? "active_save_init_failed" : reason;
        destroyActiveSave();
        return result;
    }

    bool sectionResult = false;
    const bool sectionCallOk = TryGuardedCall(
        "native scratch active save AddSection/write marker",
        [&]() -> bool
        {
            alignas(TSerialize) std::byte serializerStorage[sizeof(TSerialize)] = {};
            TSerialize* serializer = reinterpret_cast<TSerialize*>(serializerStorage);
            TSerialize* returned = s_funcActiveSaveAddSectionApi(
                activeSave,
                serializer,
                "CoopScratchNativeSection");
            result.addSectionCalled = true;
            if (!returned)
                return false;

            result.sectionAdded = true;
            result.serializerImpl = reinterpret_cast<std::uintptr_t>(GetImpl(*returned));
            int marker = 0x23402341;
            returned->Value("marker", marker);
            result.serializerOk = returned->Ok();
            return result.serializerOk;
        },
        sectionResult,
        &reason);
    if (!sectionCallOk || !sectionResult)
    {
        ++result.guards;
        result.reason = reason.empty() ? "add_section_write_failed" : reason;
        destroyActiveSave();
        return result;
    }

    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(result.activeSave + 0x458), result.sectionCacheBeginAfter);
    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(result.activeSave + 0x460), result.sectionCacheEndAfter);
    TryReadRuntimeValue(reinterpret_cast<const std::uintptr_t*>(result.activeSave + 0x468), result.sectionCacheCapacityAfter);
    result.sectionCacheEntriesAfter = VectorRecordCount(
        result.sectionCacheBeginAfter,
        result.sectionCacheEndAfter,
        sizeof(std::uintptr_t));

    if (result.readyCopyRoot && result.readyResolveRoot)
    {
        std::array<std::uintptr_t, 3> rootEntry = {};
        bool copied = false;
        const bool copyOk = TryGuardedCall(
            "native scratch active save copy root stack entry",
            [&]() -> bool
            {
                s_funcCopyWriteRootStackEntry(
                    reinterpret_cast<void*>(result.writeStore),
                    rootEntry.data());
                return true;
            },
            copied,
            &reason);
        if (!copyOk || !copied)
        {
            ++result.guards;
            if (result.reason.empty())
                result.reason = reason.empty() ? "copy_root_failed" : reason;
        }
        else
        {
            void* rootNode = nullptr;
            const bool resolved = TryGuardedCall(
                "native scratch active save resolve root node",
                [&]() -> void*
                {
                    return s_funcWriteStackEntryToNode(rootEntry.data());
                },
                rootNode,
                &reason);
            if (!resolved || !rootNode)
            {
                ++result.guards;
                if (result.reason.empty())
                    result.reason = reason.empty() ? "resolve_root_failed" : reason;
            }
            else
            {
                WriteNodeProbeView rootView;
                if (!ReadWriteNodeProbeView(reinterpret_cast<std::uintptr_t>(rootNode), rootView, &reason))
                {
                    ++result.guards;
                    if (result.reason.empty())
                        result.reason = reason.empty() ? "read_root_failed" : reason;
                }
                else
                {
                    result.rootChildCount = rootView.childCount;
                    result.rootAttrRecords = VectorRecordCount(rootView.attrBegin, rootView.attrEnd, kWriteAttrRecordBytes);
                    result.rootChildRecords = VectorRecordCount(rootView.childBegin, rootView.childEnd, sizeof(uint32_t));
                }
            }
        }
    }

    if (result.sectionCacheEntriesAfter == 0)
    {
        ++result.guards;
        if (result.reason.empty())
            result.reason = "section_cache_empty_after_add";
    }
    if (result.rootChildCount == 0)
    {
        ++result.guards;
        if (result.reason.empty())
            result.reason = "root_has_no_section_child";
    }

    destroyActiveSave();
    if (!result.destroyed)
    {
        if (result.reason.empty())
            result.reason = "destroy_failed";
        return result;
    }

    result.ok =
        result.guards == 0 &&
        result.sectionAdded &&
        result.serializerOk &&
        result.sectionCacheEntriesAfter != 0 &&
        result.rootChildCount != 0;
    if (result.reason.empty())
        result.reason = result.ok ? "ok" : "checked_with_warnings";
    return result;
}

std::string BuildScratchActiveSaveSectionProbeStatus(const ScratchActiveSaveSectionProbeResult& result)
{
    std::ostringstream out;
    out
        << "attempted=" << (result.attempted ? 1 : 0)
        << "/ok=" << (result.ok ? 1 : 0)
        << "/reason=" << StatusToken(result.reason)
        << "/constructed=" << (result.constructed ? 1 : 0)
        << "/initialized=" << (result.initialized ? 1 : 0)
        << "/destroyed=" << (result.destroyed ? 1 : 0)
        << "/add=" << (result.addSectionCalled ? 1 : 0)
        << "," << (result.sectionAdded ? 1 : 0)
        << "," << (result.serializerOk ? 1 : 0)
        << "/active=" << PointerHex(result.activeSave)
        << "/vt=" << PointerHex(result.vtable)
        << "/dtor=" << PointerHex(result.virtualDtor)
        << "/writeStore=" << PointerHex(result.writeStore)
        << "/ser=" << PointerHex(result.serializerImpl)
        << "/cacheBefore=" << PointerHex(result.sectionCacheBeginBefore)
        << "," << PointerHex(result.sectionCacheEndBefore)
        << "," << PointerHex(result.sectionCacheCapacityBefore)
        << "/cacheAfter=" << PointerHex(result.sectionCacheBeginAfter)
        << "," << PointerHex(result.sectionCacheEndAfter)
        << "," << PointerHex(result.sectionCacheCapacityAfter)
        << "/cacheEntries=" << result.sectionCacheEntriesAfter
        << "/root=" << result.rootChildCount
        << "," << result.rootAttrRecords
        << "," << result.rootChildRecords
        << "/func=" << result.functionsReady << "/" << result.functionChecks
        << "/ready=" << result.readyFactory
        << result.readyInit
        << result.readyAddSection
        << result.readyVirtualDtor
        << result.readyCopyRoot
        << result.readyResolveRoot
        << "/guards=" << result.guards;
    return out.str();
}

bool IsScratchActiveSaveSectionProbeEnabled()
{
    return EnvFlagEnabled("COOP_NATIVE_SCRATCH_SAVE_SECTION_PROBE");
}

bool IsWriteApiEnabled()
{
    return EnvFlagEnabled("COOP_NATIVE_SAVE_STORE_WRITE_API") &&
        CoopRuntimeConfig::UnsafeFlag("COOP_ENABLE_NATIVE_COOP_SAVE_MERGE_MUTATION");
}

bool IsWriteProbeEnabled()
{
    return EnvFlagEnabled("COOP_NATIVE_SAVE_STORE_WRITE_TRAMPOLINE_PROBE") ||
        EnvFlagEnabled("COOP_NATIVE_SAVE_STORE_WRITE_API");
}

bool IsWriteAllocatorProbeEnabled()
{
    return EnvFlagEnabled("COOP_NATIVE_SAVE_STORE_ALLOCATOR_PROBE") ||
        EnvFlagEnabled("COOP_NATIVE_SAVE_STORE_WRITE_API");
}
}
