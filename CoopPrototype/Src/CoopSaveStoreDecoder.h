#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace CoopSaveStoreDecoder
{
struct DecodeOptions
{
    uint32_t stackEntries = 4;
    uint32_t storeBytes = 0x300;
    uint32_t poolBytes = 0x400;
    uint32_t rawBytes = 32;
    uint32_t maxStoreEntries = 48;
    uint32_t maxPoolCandidates = 16;
    uint32_t nodeWindow = 3;
    bool fullStoreMap = false;
    bool poolStrideScan = false;
};

struct StoreNodeRecord
{
    uint32_t nodeIndex = 0;
    uint32_t nodeId = 0;
    uint32_t childCursor = 0;
    uint32_t childCount = 0;
    uint32_t attrCursor = 0;
    uint32_t attrCount = 0;
    std::uintptr_t nodePtr = 0;
    std::uintptr_t childIndexBlockBegin = 0;
    std::uintptr_t childIndexBlockEnd = 0;
    std::uintptr_t attrVectorBegin = 0;
    std::uintptr_t attrVectorEnd = 0;
    std::uintptr_t childVectorBegin = 0;
    std::uintptr_t childVectorEnd = 0;
    std::vector<uint32_t> childEntryIndices;
    bool readable = false;
    bool valid = false;
};

struct StoreMap
{
    bool ok = false;
    bool readStore = false;
    std::uintptr_t store = 0;
    std::uintptr_t nodeBlockBegin = 0;
    std::uintptr_t nodeBlockEnd = 0;
    uint32_t nodeCount = 0;
    std::uintptr_t attrStringBase = 0;
    std::uintptr_t attrNameOffsetTable = 0;
    std::uintptr_t attrTokenContext = 0;
    std::uintptr_t attrTokenIndexBase = 0;
    std::uintptr_t attrTokenBase = 0;
    std::uintptr_t childNameDataBase = 0;
    std::uintptr_t childNameResolverContext = 0;
    std::uintptr_t childNameOffsetTable = 0;
    StoreNodeRecord currentNode;
    std::vector<StoreNodeRecord> sampledNodes;
};

struct DecodeResult
{
    uint32_t slots = 0;
    uint32_t candidates = 0;
    uint32_t guards = 0;
    bool helperStackSeen = false;
    std::uintptr_t serializerImpl = 0;
    std::uintptr_t helper = 0;
    std::uintptr_t context = 0;
    std::uintptr_t store = 0;
    std::uintptr_t node = 0;
    std::uintptr_t nodeBlockBegin = 0;
    int32_t nodeIndex = -1;
    uint32_t nodeId = 0;
    uint32_t childCursor = 0;
    uint32_t childCount = 0;
    uint32_t attrCursor = 0;
    uint32_t attrCount = 0;
    std::uintptr_t attrVectorBegin = 0;
    std::uintptr_t attrVectorEnd = 0;
    std::uintptr_t childVectorBegin = 0;
    std::uintptr_t childVectorEnd = 0;
    bool nodeReadable = false;
    bool nodeValid = false;
    StoreMap storeMap;
    std::string detail;
};

DecodeOptions OptionsFromEnvironment();
DecodeResult DecodeSerializerStore(const void* serializerImpl, const DecodeOptions& options);
DecodeResult DecodeReadStackEntry(const void* stackEntry, const void* resolvedNode, const DecodeOptions& options);
DecodeResult DecodeWriteStackEntry(const void* stackEntry, const void* resolvedNode, const DecodeOptions& options);
std::string BuildStoreMapStatus(const StoreMap& map);
}
