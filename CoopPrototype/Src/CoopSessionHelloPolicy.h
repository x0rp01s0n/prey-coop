#pragma once

#include <cstdint>

#include "CoopSerialSequence.h"

namespace CoopSessionHelloPolicy
{
constexpr float kRuntimeRestartSilenceSeconds = 2.0f;

enum class Decision : uint8_t
{
    Reject,
    Accept,
    Restart,
};

struct Freshness
{
    uint64_t runtimeNonce = 0;
    uint32_t sequence = 0;
    float acceptedAt = -1.0f;
    uint32_t address = 0;
    uint16_t port = 0;
};

constexpr Decision Classify(
    uint32_t sequence,
    uint32_t lastSequence,
    uint64_t incomingRuntimeNonce,
    uint64_t currentRuntimeNonce,
    bool incomingRuntimeNonceRetired,
    float secondsSinceAcceptedHello,
    bool admissionRequired,
    bool admissionAuthorized)
{
    if (sequence == 0)
        return Decision::Reject;
    if (incomingRuntimeNonce != 0 && incomingRuntimeNonceRetired)
        return Decision::Reject;
    if (admissionRequired && !admissionAuthorized)
        return Decision::Reject;

    const bool changedRuntimeSession = incomingRuntimeNonce != 0 &&
        currentRuntimeNonce != 0 &&
        incomingRuntimeNonce != currentRuntimeNonce;
    if (!changedRuntimeSession)
    {
        return CoopSerialSequence::IsNewer(sequence, lastSequence)
            ? Decision::Accept
            : Decision::Reject;
    }

    if (!(secondsSinceAcceptedHello >= kRuntimeRestartSilenceSeconds) ||
        !admissionAuthorized)
    {
        return Decision::Reject;
    }

    return Decision::Restart;
}

constexpr Decision ClassifyTombstoneRecovery(
    uint64_t incomingRuntimeNonce,
    uint64_t tombstonedRuntimeNonce,
    bool incomingRuntimeNonceRetired,
    float secondsSinceAcceptedHello,
    bool admissionAuthorized)
{
    if (incomingRuntimeNonce == 0 ||
        incomingRuntimeNonce == tombstonedRuntimeNonce ||
        incomingRuntimeNonceRetired ||
        !(secondsSinceAcceptedHello >= kRuntimeRestartSilenceSeconds) ||
        !admissionAuthorized)
    {
        return Decision::Reject;
    }

    return Decision::Restart;
}

constexpr uint64_t RuntimeNonceAfterAcceptedHello(uint64_t incomingNonce, uint64_t currentNonce)
{
    return incomingNonce != 0 ? incomingNonce : currentNonce;
}

constexpr bool HasEndpoint(const Freshness& freshness)
{
    return freshness.address != 0 && freshness.port != 0;
}

constexpr bool HasHelloHistory(const Freshness& freshness)
{
    return freshness.runtimeNonce != 0 || freshness.sequence != 0;
}

constexpr bool ShouldUseTombstone(bool hasActivePeer, const Freshness& active, bool hasTombstone)
{
    return hasTombstone && (!hasActivePeer || !HasHelloHistory(active));
}

constexpr bool MatchesEndpoint(const Freshness& freshness, uint32_t address, uint16_t port)
{
    return !HasEndpoint(freshness) ||
        (freshness.address == address && freshness.port == port);
}

constexpr bool EndpointChangeAllowed(bool endpointMatches, Decision decision)
{
    return endpointMatches || decision == Decision::Restart;
}

static_assert(Classify(8u, 9u, 42u, 42u, false, 0.0f, false, true) == Decision::Reject, "stale same-session Hello");
static_assert(Classify(1u, 0xffffffffu, 42u, 42u, false, 0.0f, false, true) == Decision::Accept, "sequence wrap");
static_assert(Classify(1u, 99u, 22u, 11u, false, 1.99f, false, true) == Decision::Reject, "restart before silence");
static_assert(Classify(1u, 99u, 22u, 11u, false, 2.0f, false, true) == Decision::Restart, "restart after silence");
static_assert(Classify(100u, 99u, 11u, 22u, true, 30.0f, false, true) == Decision::Reject, "delayed previous nonce");
static_assert(Classify(1u, 99u, 11u, 33u, true, 30.0f, false, true) == Decision::Reject, "A to B to C then delayed A replay");
static_assert(Classify(100u, 99u, 22u, 11u, false, 30.0f, false, false) == Decision::Reject, "unauthorized restart");
static_assert(Classify(100u, 99u, 22u, 11u, false, 30.0f, true, false) == Decision::Reject, "password-rejected peer admission");
static_assert(Classify(8u, 7u, 0u, 22u, false, 0.0f, false, true) == Decision::Accept, "zero nonce is not a restart");
static_assert(Classify(0u, 7u, 0u, 22u, false, 0.0f, false, true) == Decision::Reject, "zero sequence");
static_assert(RuntimeNonceAfterAcceptedHello(0u, 22u) == 22u, "zero nonce must not clear a known nonce");
static_assert(RuntimeNonceAfterAcceptedHello(33u, 22u) == 33u, "nonzero nonce updates the session");
}
