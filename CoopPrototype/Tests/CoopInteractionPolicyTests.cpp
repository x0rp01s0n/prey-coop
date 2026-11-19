#include "CoopInteractionPolicy.h"
#include "CoopEnemyStateWire.h"

#include <cassert>
#include <iostream>

int main()
{
    using namespace CoopInteractionPolicy;
    PendingHack pending;
    pending.id = 1;
    pending.worldEpoch = 3;
    pending.areaId = 4;
    pending.deadlineMs = 5000;
    assert(pending.IsCurrent(3, 4, 100));
    assert(!pending.IsCurrent(2, 4, 100));
    assert(!pending.IsCurrent(3, 5, 100));
    assert(!pending.IsCurrent(3, 4, 5000));
    assert(pending.NeedsSend(10, 2, 9, 100));
    pending.sentOwner = 10;
    pending.sentEpoch = 2;
    pending.sentSaveKey = 9;
    pending.retryAtMs = 350;
    assert(!pending.NeedsSend(10, 2, 9, 400));
    assert(!pending.NeedsSend(11, 3, 9, 349));
    assert(pending.NeedsSend(11, 3, 9, 350));
    assert(pending.NeedsSend(10, 3, 9, 350));
    assert(pending.NeedsSend(10, 2, 10, 350));
    assert(pending.MatchesReply(1, 10, 2));
    assert(!pending.MatchesReply(2, 10, 2));
    assert(!pending.MatchesReply(1, 11, 2));
    assert(!pending.MatchesReply(1, 10, 3));
    for (uint32_t epoch = 3; epoch < 1003; ++epoch)
    {
        const auto oldOwner = pending.sentOwner;
        const auto oldEpoch = pending.sentEpoch;
        const uint64_t owner = epoch % 2 ? 11 : 10;
        assert(pending.NeedsSend(owner, epoch, 9, 400));
        pending.sentOwner = owner;
        pending.sentEpoch = epoch;
        assert(!pending.MatchesReply(1, oldOwner, oldEpoch));
        assert(pending.MatchesReply(1, owner, epoch));
    }
    pending = {};
    assert(!pending.IsCurrent(3, 4, 100));
    assert(!pending.MatchesReply(1, 10, 1002));
    assert(RevisionAtLeast(1, 0xffffffffu));
    assert(!RevisionAtLeast(0xffffffffu, 1));

    for (uint32_t grant : {40000u, 0xffffffffu, 1u})
    {
        uint32_t outgoing = grant == 1u ? 0xffffffffu : 10u;
        CoopSerialSequence::Observe(grant, outgoing);
        assert(CoopSerialSequence::IsAfter(CoopSerialSequence::Advance(outgoing), grant));
    }

    CoopProtocol::EnemyInteractionPacket interaction;
    interaction.requestId = 7;
    interaction.worldEpoch = 3;
    interaction.hostSaveKeyHash = 9;
    interaction.areaId = 4;
    interaction.enemyNetId = 42;
    interaction.stableEnemyId = 43;
    interaction.authorityOwnerAccountToken = 11;
    interaction.targetAccountToken = 11;
    interaction.authorityEpoch = 2;
    assert(CanRelay(interaction, 3, 9, 10, 4, 4));
    assert(!CanRelay(interaction, 2, 9, 10, 4, 4));
    assert(!CanRelay(interaction, 3, 8, 10, 4, 4));
    assert(!CanRelay(interaction, 3, 9, 10, 5, 4));
    assert(!CanRelay(interaction, 3, 9, 10, 4, 5));
    assert(!CanRelay(interaction, 3, 9, 11, 4, 4));
    interaction.targetAccountToken = 12;
    assert(!CanRelay(interaction, 3, 9, 10, 4, 4));
    interaction.command = static_cast<uint16_t>(CoopProtocol::EnemyInteractionCommand::Accepted);
    interaction.interactionRevision = 1;
    assert(CanRelay(interaction, 3, 9, 11, 4, 4));
    assert(!CanRelay(interaction, 3, 9, 10, 4, 4));
    interaction.command = static_cast<uint16_t>(CoopProtocol::EnemyInteractionCommand::Rejected);
    assert(CanRelay(interaction, 3, 9, 11, 4, 4));
    interaction.reserved = 1;
    assert(!CanRelay(interaction, 3, 9, 11, 4, 4));
    interaction.reserved = 0;
    interaction.stableEnemyId = 0;
    assert(!HasValidIdentity(interaction));
    interaction.stableEnemyId = 43;
    interaction.version--;
    assert(!HasValidIdentity(interaction));

    CoopProtocol::TestMimicStatePacket source;
    source.enemyNetId = 42;
    source.archetypeId = 739;
    source.entityGuid = 456;
    source.authorityOwnerAccountToken = 123;
    source.authorityEpoch = 7;
    source.interactionRevision = 0xfffffff0u;
    CoopProtocol::EnemyStateWirePacket wire;
    assert(CoopEnemyStateWire::Encode(source, wire));
    assert(wire.interactionRevision == source.interactionRevision);
    CoopProtocol::TestMimicStatePacket restored;
    assert(CoopEnemyStateWire::Decode(wire, source.archetypeId, source.entityGuid, restored));
    assert(restored.interactionRevision == source.interactionRevision);
    for (uint32_t flags : {CoopProtocol::kEnemyLocomotionFlagRagdolled,
             CoopProtocol::kEnemyLocomotionFlagRagdolled | CoopProtocol::kEnemyLocomotionFlagStunned,
             0x1ffffu})
    {
        source.locomotionFlags = flags;
        assert(CoopEnemyStateWire::Encode(source, wire));
        assert(CoopEnemyStateWire::Decode(wire, source.archetypeId, source.entityGuid, restored));
        assert(restored.locomotionFlags == flags);
        assert(restored.authorityEpoch == source.authorityEpoch);
        assert(restored.authorityOwnerAccountToken == source.authorityOwnerAccountToken);
    }
    std::cout << "Interaction policy and snapshot revision checks passed\n";
}
