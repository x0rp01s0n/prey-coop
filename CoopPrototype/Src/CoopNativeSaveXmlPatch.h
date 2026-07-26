#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

class XmlNodeRef;

namespace CoopNativeSaveXmlPatch
{
struct PathIndexResult
{
    bool ok = false;
    bool written = false;
    uint32_t nodes = 0;
    uint32_t hits = 0;
    std::string guardReason;
};

PathIndexResult WritePathIndex(
    const XmlNodeRef& root,
    const std::filesystem::path& path,
    uint32_t maxNodes,
    uint32_t maxDepth);
} // namespace CoopNativeSaveXmlPatch
