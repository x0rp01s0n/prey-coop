#include "CoopEnemyStateWire.h"
#include "CoopEnemyAuthorityPolicy.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace
{
int16_t EncodeUnit(float value)
{
    if (!std::isfinite(value))
        return 0;
    return static_cast<int16_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
}

float DecodeUnit(int16_t value)
{
    return static_cast<float>(value) * (1.0f / 32767.0f);
}

bool IsFinite(float value)
{
    return std::isfinite(value);
}
}

bool CoopEnemyStateWire::Encode(
    const CoopProtocol::TestMimicStatePacket& source,
    CoopProtocol::EnemyStateWirePacket& target)
{
    if (source.enemyNetId == 0 || source.authorityOwnerAccountToken == 0 || source.authorityEpoch == 0 ||
        source.sourceFlags > std::numeric_limits<uint16_t>::max() ||
        source.locomotionFlags > std::numeric_limits<uint16_t>::max() ||
        source.flags > std::numeric_limits<uint16_t>::max() ||
        source.locomotionLevel > std::numeric_limits<uint8_t>::max() ||
        source.authorityAttentionLevel > CoopEnemyAuthorityPolicy::kKnownAttention ||
        source.mannequinTagStateValid > 1 ||
        source.semanticVariant > 3 ||
        !IsFinite(source.px) || !IsFinite(source.py) || !IsFinite(source.pz) ||
        !IsFinite(source.health) || !IsFinite(source.maxHealth) || !IsFinite(source.lastDamage))
    {
        return false;
    }

    target = {};
    target.magic = source.magic;
    target.version = source.version;
    target.type = source.type;
    target.sequence = source.sequence;
    target.enemyNetId = source.enemyNetId;
    target.px = source.px;
    target.py = source.py;
    target.pz = source.pz;

    float qw = source.qw;
    float qx = source.qx;
    float qy = source.qy;
    float qz = source.qz;
    const float rotationLengthSq = qw * qw + qx * qx + qy * qy + qz * qz;
    if (!IsFinite(rotationLengthSq) || rotationLengthSq < 0.000001f)
    {
        qw = 1.0f;
        qx = qy = qz = 0.0f;
    }
    else
    {
        const float inverseLength = 1.0f / std::sqrt(rotationLengthSq);
        qw *= inverseLength;
        qx *= inverseLength;
        qy *= inverseLength;
        qz *= inverseLength;
    }
    target.qw = EncodeUnit(qw);
    target.qx = EncodeUnit(qx);
    target.qy = EncodeUnit(qy);
    target.qz = EncodeUnit(qz);

    float mx = source.mx;
    float my = source.my;
    float mz = source.mz;
    const float directionLengthSq = mx * mx + my * my + mz * mz;
    if (!IsFinite(directionLengthSq) || directionLengthSq < 0.000001f)
    {
        mx = 0.0f;
        my = 1.0f;
        mz = 0.0f;
    }
    else
    {
        const float inverseLength = 1.0f / std::sqrt(directionLengthSq);
        mx *= inverseLength;
        my *= inverseLength;
        mz *= inverseLength;
    }
    target.mx = EncodeUnit(mx);
    target.my = EncodeUnit(my);
    target.mz = EncodeUnit(mz);

    const float speed = IsFinite(source.speed) ? std::max(0.0f, source.speed) : 0.0f;
    target.speedCentimetersPerSecond = static_cast<uint16_t>(std::lround(
        std::min(speed * 100.0f, static_cast<float>(std::numeric_limits<uint16_t>::max()))));
    target.health = source.health;
    target.maxHealth = source.maxHealth;
    target.lastDamage = source.lastDamage;
    target.commitSequence = source.commitSequence;
    target.sourceFlags = static_cast<uint16_t>(source.sourceFlags);
    target.locomotionFlags = static_cast<uint16_t>(source.locomotionFlags);
    target.stateFlags = static_cast<uint16_t>(source.flags);
    target.locomotionLevel = static_cast<uint8_t>(source.locomotionLevel);
    target.authorityAttentionLevel = source.authorityAttentionLevel;
    target.attackKind = source.attackKind;
    target.mannequinFragmentId = source.mannequinFragmentId;
    target.mannequinSequence = source.mannequinSequence;
    target.mannequinOrdinal = source.mannequinOrdinal;
    target.mannequinReserved = source.mannequinReserved;
    target.mannequinPriority = source.mannequinPriority;
    std::copy(
        std::begin(source.mannequinTagState),
        std::end(source.mannequinTagState),
        std::begin(target.mannequinTagState));
    target.mannequinTagStateValid = source.mannequinTagStateValid;
    std::copy(
        std::begin(source.mannequinTagStateReserved),
        std::end(source.mannequinTagStateReserved),
        std::begin(target.mannequinTagStateReserved));
    target.semanticContextId = source.semanticContextId;
    target.semanticSequence = source.semanticSequence;
    target.semanticVariant = source.semanticVariant;
    std::copy(
        std::begin(source.semanticReserved),
        std::end(source.semanticReserved),
        std::begin(target.semanticReserved));
    target.targetAccountToken = source.targetAccountToken;
    target.authorityOwnerAccountToken = source.authorityOwnerAccountToken;
    target.authorityEpoch = source.authorityEpoch;
    return true;
}

bool CoopEnemyStateWire::Decode(
    const CoopProtocol::EnemyStateWirePacket& source,
    uint64_t archetypeId,
    uint64_t stableEnemyId,
    CoopProtocol::TestMimicStatePacket& target)
{
    if (source.magic != CoopProtocol::kPacketMagic ||
        source.version != CoopProtocol::kProtocolVersion ||
        source.type != static_cast<uint16_t>(CoopProtocol::PacketType::TestMimicState) ||
        source.enemyNetId == 0 || archetypeId == 0 || stableEnemyId == 0 ||
        source.authorityOwnerAccountToken == 0 || source.authorityEpoch == 0 ||
        source.authorityAttentionLevel > CoopEnemyAuthorityPolicy::kKnownAttention ||
        source.mannequinTagStateValid > 1 ||
        source.semanticVariant > 3 ||
        !IsFinite(source.px) || !IsFinite(source.py) || !IsFinite(source.pz) ||
        !IsFinite(source.health) || !IsFinite(source.maxHealth) || !IsFinite(source.lastDamage))
    {
        return false;
    }

    target = {};
    target.magic = source.magic;
    target.version = source.version;
    target.type = source.type;
    target.sequence = source.sequence;
    target.archetypeId = archetypeId;
    target.enemyNetId = source.enemyNetId;
    target.entityGuid = stableEnemyId;
    target.px = source.px;
    target.py = source.py;
    target.pz = source.pz;

    float qw = DecodeUnit(source.qw);
    float qx = DecodeUnit(source.qx);
    float qy = DecodeUnit(source.qy);
    float qz = DecodeUnit(source.qz);
    const float rotationLengthSq = qw * qw + qx * qx + qy * qy + qz * qz;
    if (!IsFinite(rotationLengthSq) || rotationLengthSq < 0.000001f)
    {
        qw = 1.0f;
        qx = qy = qz = 0.0f;
    }
    else
    {
        const float inverseLength = 1.0f / std::sqrt(rotationLengthSq);
        qw *= inverseLength;
        qx *= inverseLength;
        qy *= inverseLength;
        qz *= inverseLength;
    }
    target.qw = qw;
    target.qx = qx;
    target.qy = qy;
    target.qz = qz;

    float mx = DecodeUnit(source.mx);
    float my = DecodeUnit(source.my);
    float mz = DecodeUnit(source.mz);
    const float directionLengthSq = mx * mx + my * my + mz * mz;
    if (!IsFinite(directionLengthSq) || directionLengthSq < 0.000001f)
    {
        mx = 0.0f;
        my = 1.0f;
        mz = 0.0f;
    }
    else
    {
        const float inverseLength = 1.0f / std::sqrt(directionLengthSq);
        mx *= inverseLength;
        my *= inverseLength;
        mz *= inverseLength;
    }
    target.mx = mx;
    target.my = my;
    target.mz = mz;
    target.speed = static_cast<float>(source.speedCentimetersPerSecond) * 0.01f;
    target.health = source.health;
    target.maxHealth = source.maxHealth;
    target.lastDamage = source.lastDamage;
    target.commitSequence = source.commitSequence;
    target.sourceFlags = source.sourceFlags;
    target.locomotionFlags = source.locomotionFlags;
    target.flags = source.stateFlags;
    target.locomotionLevel = source.locomotionLevel;
    target.authorityAttentionLevel = source.authorityAttentionLevel;
    target.attackKind = source.attackKind;
    target.mannequinFragmentId = source.mannequinFragmentId;
    target.mannequinSequence = source.mannequinSequence;
    target.mannequinOrdinal = source.mannequinOrdinal;
    target.mannequinReserved = source.mannequinReserved;
    target.mannequinPriority = source.mannequinPriority;
    std::copy(
        std::begin(source.mannequinTagState),
        std::end(source.mannequinTagState),
        std::begin(target.mannequinTagState));
    target.mannequinTagStateValid = source.mannequinTagStateValid;
    std::copy(
        std::begin(source.mannequinTagStateReserved),
        std::end(source.mannequinTagStateReserved),
        std::begin(target.mannequinTagStateReserved));
    target.semanticContextId = source.semanticContextId;
    target.semanticSequence = source.semanticSequence;
    target.semanticVariant = source.semanticVariant;
    std::copy(
        std::begin(source.semanticReserved),
        std::end(source.semanticReserved),
        std::begin(target.semanticReserved));
    target.targetAccountToken = source.targetAccountToken;
    target.authorityOwnerAccountToken = source.authorityOwnerAccountToken;
    target.authorityEpoch = source.authorityEpoch;
    return true;
}
