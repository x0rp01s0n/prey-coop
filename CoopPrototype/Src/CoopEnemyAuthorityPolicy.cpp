#include "CoopEnemyAuthorityPolicy.h"

#include <limits>

CoopEnemyAuthorityPolicy::Decision CoopEnemyAuthorityPolicy::Select(
    uint64_t areaAuthorityAccountToken,
    const std::vector<Candidate>& candidates)
{
    Decision decision;
    decision.ownerAccountToken = areaAuthorityAccountToken;

    for (const Candidate& candidate : candidates)
    {
        if (candidate.accountToken == 0 ||
            candidate.blocked ||
            candidate.attentionLevel == kUnknownAttention ||
            candidate.attentionLevel > kKnownAttention ||
            candidate.firstAtLevelOrder == 0)
        {
            continue;
        }

        const bool higherLevel = candidate.attentionLevel > decision.attentionLevel;
        const bool earlierAtSameLevel =
            candidate.attentionLevel == decision.attentionLevel &&
            (decision.firstAtLevelOrder == 0 ||
                candidate.firstAtLevelOrder < decision.firstAtLevelOrder ||
                (candidate.firstAtLevelOrder == decision.firstAtLevelOrder &&
                    candidate.accountToken < decision.ownerAccountToken));
        if (!higherLevel && !earlierAtSameLevel)
            continue;

        decision.ownerAccountToken = candidate.accountToken;
        decision.attentionLevel = candidate.attentionLevel;
        decision.firstAtLevelOrder = candidate.firstAtLevelOrder;
        decision.fallback = false;
    }

    return decision;
}

bool CoopEnemyAuthorityPolicy::RunSelfTest(std::string& detail)
{
    constexpr uint64_t areaAuthority = 10;
    constexpr uint64_t playerOne = 20;
    constexpr uint64_t playerTwo = 30;
    constexpr uint64_t playerThree = 40;

    auto expect = [&](const std::vector<Candidate>& candidates, uint64_t owner, uint8_t level)
    {
        const Decision decision = Select(areaAuthority, candidates);
        return decision.ownerAccountToken == owner && decision.attentionLevel == level;
    };

    if (!expect({}, areaAuthority, 0))
    {
        detail = "area_authority_fallback_failed";
        return false;
    }
    if (!expect({{playerOne, 1, 1, false}}, playerOne, 1))
    {
        detail = "first_suspicion_claim_failed";
        return false;
    }
    if (!expect({{playerOne, 1, 1, false}, {playerTwo, 2, 2, false}}, playerTwo, 2))
    {
        detail = "higher_attention_preemption_failed";
        return false;
    }
    if (!expect(
            {{playerOne, 4, 4, false}, {playerTwo, 2, 2, false}, {playerThree, 4, 3, false}},
            playerThree,
            4))
    {
        detail = "equal_attention_first_claim_failed";
        return false;
    }
    if (!expect(
            {{areaAuthority, 4, 6, false}, {playerOne, 4, 5, false}},
            playerOne,
            4))
    {
        detail = "area_authority_special_priority_leak";
        return false;
    }
    if (!expect(
            {{playerOne, 4, 1, true}, {playerTwo, 3, 2, false}},
            playerTwo,
            3))
    {
        detail = "blocked_candidate_priority_leak";
        return false;
    }

    detail = "attention_level_then_first_claim_with_area_fallback";
    return true;
}
