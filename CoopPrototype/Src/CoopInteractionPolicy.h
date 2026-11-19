#pragma once

#include <cstdint>
#include "CoopProtocol.h"
#include "CoopSerialSequence.h"

namespace CoopInteractionPolicy
{
constexpr uint64_t kRequestTimeoutMilliseconds = 15000;
constexpr uint64_t kSendRetryMilliseconds = 250;

inline bool RevisionAtLeast(uint32_t incoming, uint32_t expected)
{
    return incoming == expected || CoopSerialSequence::IsAfter(incoming, expected);
}

inline bool HasValidIdentity(const CoopProtocol::EnemyInteractionPacket& packet)
{
    using Command = CoopProtocol::EnemyInteractionCommand;
    const auto command = static_cast<Command>(packet.command);
    return packet.magic == CoopProtocol::kPacketMagic &&
        packet.version == CoopProtocol::kProtocolVersion &&
        packet.type == static_cast<uint16_t>(CoopProtocol::PacketType::EnemyInteraction) &&
        packet.requestId != 0 && packet.enemyNetId != 0 && packet.stableEnemyId != 0 &&
        packet.authorityOwnerAccountToken != 0 && packet.authorityEpoch != 0 &&
        packet.targetAccountToken != 0 && packet.reserved == 0 &&
        (command == Command::HackCompleted || command == Command::Accepted || command == Command::Rejected);
}

inline bool CanRelay(const CoopProtocol::EnemyInteractionPacket& packet,
    uint32_t world, uint64_t saveKey, uint64_t source, uint64_t sourceArea, uint64_t targetArea)
{
    if (!HasValidIdentity(packet) || packet.worldEpoch != world ||
        packet.hostSaveKeyHash == 0 || packet.hostSaveKeyHash != saveKey ||
        packet.areaId == 0 || packet.areaId != sourceArea || packet.areaId != targetArea ||
        source == 0 || packet.targetAccountToken == source)
        return false;
    return packet.command == static_cast<uint16_t>(CoopProtocol::EnemyInteractionCommand::HackCompleted)
        ? packet.targetAccountToken == packet.authorityOwnerAccountToken
        : source == packet.authorityOwnerAccountToken;
}

struct PendingHack
{
    uint32_t id = 0;
    uint32_t worldEpoch = 0;
    uint64_t areaId = 0;
    uint64_t deadlineMs = 0;
    uint64_t retryAtMs = 0;
    uint64_t sentOwner = 0;
    uint32_t sentEpoch = 0;
    uint64_t sentSaveKey = 0;

    bool IsCurrent(uint32_t world, uint64_t area, uint64_t now) const
    {
        return id != 0 && worldEpoch == world && areaId == area && now < deadlineMs;
    }

    bool NeedsSend(uint64_t owner, uint32_t epoch, uint64_t saveKey, uint64_t now) const
    {
        return now >= retryAtMs &&
            (sentOwner != owner || sentEpoch != epoch || sentSaveKey != saveKey);
    }

    bool MatchesReply(uint32_t request, uint64_t owner, uint32_t epoch) const
    {
        return id != 0 && id == request && sentOwner == owner && sentEpoch == epoch;
    }
};
}
