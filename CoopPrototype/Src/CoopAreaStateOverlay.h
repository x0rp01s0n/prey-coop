#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct CoopAreaStateOverlayApplyStats
{
    uint32_t rows = 0;
    uint32_t levelRows = 0;
    uint32_t matchedEntities = 0;
    uint32_t transformedEntities = 0;
    uint32_t hiddenEntities = 0;
    uint32_t removedEntities = 0;
    uint32_t missingEntities = 0;
    uint32_t skippedRows = 0;
    uint32_t errors = 0;
    std::string lastEvent = "-";
};

bool ApplyCoopAreaStateOverlayJsonl(
    const std::filesystem::path& path,
    const std::string& levelName,
    CoopAreaStateOverlayApplyStats& outStats,
    std::vector<uint64_t>* outTransformedGuids = nullptr);
