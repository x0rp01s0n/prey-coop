#pragma once

#include "CoopProtocol.h"

namespace CoopEnemyStateWire
{
bool Encode(
    const CoopProtocol::TestMimicStatePacket& source,
    CoopProtocol::EnemyStateWirePacket& target);

bool Decode(
    const CoopProtocol::EnemyStateWirePacket& source,
    uint64_t archetypeId,
    uint64_t stableEnemyId,
    CoopProtocol::TestMimicStatePacket& target);
}
