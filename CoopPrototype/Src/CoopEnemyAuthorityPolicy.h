#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace CoopEnemyAuthorityPolicy
{
constexpr uint8_t kUnknownAttention = 0;
constexpr uint8_t kKnownAttention = 4;

struct Candidate
{
    uint64_t accountToken = 0;
    uint8_t attentionLevel = kUnknownAttention;
    uint64_t firstAtLevelOrder = 0;
    bool blocked = false;
};

struct Decision
{
    uint64_t ownerAccountToken = 0;
    uint8_t attentionLevel = kUnknownAttention;
    uint64_t firstAtLevelOrder = 0;
    bool fallback = true;
};

Decision Select(
    uint64_t areaAuthorityAccountToken,
    const std::vector<Candidate>& candidates);

bool RunSelfTest(std::string& detail);
}
