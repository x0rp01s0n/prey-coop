#include "CoopNetworkTelemetry.h"

#include "CoopProtocol.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace
{
struct PacketDescriptor
{
    CoopProtocol::PacketType type;
    const char* name;
    size_t size;
    const char* delivery;
};

constexpr std::array<PacketDescriptor, 31> kPacketDescriptors = {{
    {CoopProtocol::PacketType::PlayerPose, "PlayerPose", sizeof(CoopProtocol::PlayerPosePacket), "direct-variable"},
    {CoopProtocol::PacketType::SessionHello, "SessionHello", sizeof(CoopProtocol::SessionHelloPacket), "direct"},
    {CoopProtocol::PacketType::RemotePlayerDamage, "RemotePlayerDamage", sizeof(CoopProtocol::RemotePlayerDamagePacket), "reliable"},
    {CoopProtocol::PacketType::TestMimicSpawn, "TestMimicSpawn", sizeof(CoopProtocol::TestMimicSpawnPacket), "reliable"},
    {CoopProtocol::PacketType::TestMimicState, "TestMimicState", sizeof(CoopProtocol::TestMimicStatePacket), "direct-variable"},
    {CoopProtocol::PacketType::EnemyDamageRequest, "EnemyDamageRequest", sizeof(CoopProtocol::EnemyDamageRequestPacket), "reliable"},
    {CoopProtocol::PacketType::EnemyDeathPresentation, "EnemyDeathPresentation", sizeof(CoopProtocol::EnemyDeathPresentationPacket), "reliable"},
    {CoopProtocol::PacketType::PlayerStatus, "PlayerStatus", sizeof(CoopProtocol::PlayerStatusPacket), "reliable"},
    {CoopProtocol::PacketType::ReliableAck, "ReliableAck", sizeof(CoopProtocol::ReliableAckPacket), "control"},
    {CoopProtocol::PacketType::ReliableEnvelope, "ReliableEnvelope", sizeof(CoopProtocol::ReliableEnvelopePacket), "control-variable"},
    {CoopProtocol::PacketType::WorldSync, "WorldSync", sizeof(CoopProtocol::WorldSyncPacket), "mixed"},
    {CoopProtocol::PacketType::SaveTransfer, "SaveTransfer", sizeof(CoopProtocol::SaveTransferPacket), "reliable"},
    {CoopProtocol::PacketType::PlayerStateTransfer, "PlayerStateTransfer", sizeof(CoopProtocol::PlayerStateTransferPacket), "reliable"},
    {CoopProtocol::PacketType::AreaJournalTransfer, "AreaJournalTransfer", sizeof(CoopProtocol::AreaJournalTransferPacket), "reliable"},
    {CoopProtocol::PacketType::LivePropTransform, "LivePropTransform", sizeof(CoopProtocol::LivePropTransformPacket), "direct"},
    {CoopProtocol::PacketType::DisconnectNotice, "DisconnectNotice", sizeof(CoopProtocol::DisconnectNoticePacket), "direct"},
    {CoopProtocol::PacketType::GooResult, "GooResult", sizeof(CoopProtocol::GooResultPacket), "mixed"},
    {CoopProtocol::PacketType::EnemyProjectileEvent, "EnemyProjectileEvent", sizeof(CoopProtocol::EnemyProjectileEventPacket), "direct"},
    {CoopProtocol::PacketType::EnemyAbilityFxEvent, "EnemyAbilityFxEvent", sizeof(CoopProtocol::EnemyAbilityFxEventPacket), "mixed"},
    {CoopProtocol::PacketType::EnemyMannequinAction, "EnemyMannequinAction", sizeof(CoopProtocol::EnemyMannequinActionPacket), "reliable"},
    {CoopProtocol::PacketType::StoryEvent, "StoryEvent", sizeof(CoopProtocol::StoryEventPacket), "reliable"},
    {CoopProtocol::PacketType::AreaObjectEvent, "AreaObjectEvent", sizeof(CoopProtocol::AreaObjectEventPacket), "reliable"},
    {CoopProtocol::PacketType::AreaLease, "AreaLease", sizeof(CoopProtocol::AreaLeasePacket), "reliable"},
    {CoopProtocol::PacketType::EnemyRoster, "EnemyRoster", sizeof(CoopProtocol::EnemyRosterPacket), "reliable"},
    {CoopProtocol::PacketType::SharedDrop, "SharedDrop", sizeof(CoopProtocol::SharedDropPacket), "reliable"},
    {CoopProtocol::PacketType::SharedStorage, "SharedStorage", sizeof(CoopProtocol::SharedStoragePacket), "reliable"},
    {CoopProtocol::PacketType::HazardEvent, "HazardEvent", sizeof(CoopProtocol::HazardEventPacket), "reliable"},
    {CoopProtocol::PacketType::DialogueLease, "DialogueLease", sizeof(CoopProtocol::DialogueLeasePacket), "reliable"},
    {CoopProtocol::PacketType::TimeDilation, "TimeDilation", sizeof(CoopProtocol::TimeDilationPacket), "reliable"},
    {CoopProtocol::PacketType::CorpsePhantomRequest, "CorpsePhantomRequest", sizeof(CoopProtocol::CorpsePhantomRequestPacket), "reliable"},
    {CoopProtocol::PacketType::TextChat, "TextChat", sizeof(CoopProtocol::TextChatPacket), "direct"},
}};

void Add(CoopNetworkTelemetry::Totals& target, uint64_t packets, uint64_t bytes)
{
    target.packets += packets;
    target.bytes += bytes;
}
} // namespace

void CoopNetworkTelemetry::Reset()
{
    *this = CoopNetworkTelemetry{};
}

uint64_t CoopNetworkTelemetry::CurrentEpoch100ms()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() / 100);
}

size_t CoopNetworkTelemetry::TypeIndex(uint16_t packetType)
{
    return packetType < kPacketTypeSlots ? static_cast<size_t>(packetType) : 0;
}

CoopNetworkTelemetry::RateBucket& CoopNetworkTelemetry::CurrentRateBucket()
{
    const uint64_t epoch = CurrentEpoch100ms();
    RateBucket& bucket = m_rateBuckets[epoch % m_rateBuckets.size()];
    if (bucket.epoch100ms != epoch)
    {
        bucket = {};
        bucket.epoch100ms = epoch;
    }
    return bucket;
}

void CoopNetworkTelemetry::RecordWireSendAttempt(uint16_t packetType, uint32_t bytes)
{
    Add(m_wireSendAttempts, 1, bytes);
    Add(m_types[TypeIndex(packetType)].sendAttempts, 1, bytes);
}

void CoopNetworkTelemetry::RecordWireSendSuccess(uint16_t packetType, uint32_t bytes)
{
    Add(m_wireSent, 1, bytes);
    Add(m_types[TypeIndex(packetType)].sent, 1, bytes);
    Add(CurrentRateBucket().sent[TypeIndex(packetType)], 1, bytes);
}

void CoopNetworkTelemetry::RecordWireSendFailure(uint16_t packetType, uint32_t bytes)
{
    Add(m_wireSendFailures, 1, bytes);
    Add(m_types[TypeIndex(packetType)].sendFailures, 1, bytes);
}

void CoopNetworkTelemetry::RecordWireReceive(uint16_t packetType, uint32_t bytes)
{
    Add(m_wireReceived, 1, bytes);
    Add(m_types[TypeIndex(packetType)].received, 1, bytes);
    Add(CurrentRateBucket().received[TypeIndex(packetType)], 1, bytes);
}

void CoopNetworkTelemetry::RecordProducerAttempt(uint16_t packetType)
{
    ++m_types[TypeIndex(packetType)].producerAttempts;
}

void CoopNetworkTelemetry::RecordProducerSuppressed(
    uint16_t packetType,
    ProducerSuppressionReason reason)
{
    TypeCounters& counters = m_types[TypeIndex(packetType)];
    ++counters.producerSuppressed;
    const size_t reasonIndex = static_cast<size_t>(reason);
    if (reasonIndex < counters.suppressionReasons.size())
        ++counters.suppressionReasons[reasonIndex];
}

void CoopNetworkTelemetry::RecordReliableEnqueue(uint16_t payloadType, uint32_t payloadBytes)
{
    Add(m_reliableEnqueued, 1, payloadBytes);
    Add(m_types[TypeIndex(payloadType)].reliableEnqueued, 1, payloadBytes);
}

void CoopNetworkTelemetry::RecordReliablePayloadReceive(uint16_t payloadType, uint32_t payloadBytes)
{
    Add(m_reliableReceived, 1, payloadBytes);
    Add(m_types[TypeIndex(payloadType)].reliableReceived, 1, payloadBytes);
}

void CoopNetworkTelemetry::RecordReliableResend() { ++m_reliableResends; }
void CoopNetworkTelemetry::RecordReliableAckSent() { ++m_reliableAcksSent; }
void CoopNetworkTelemetry::RecordReliableAckReceived() { ++m_reliableAcksReceived; }
void CoopNetworkTelemetry::RecordReliableDrop() { ++m_reliableDrops; }
void CoopNetworkTelemetry::RecordReliableRetired() { ++m_reliableRetired; }
void CoopNetworkTelemetry::RecordReliableCoalesced() { ++m_reliableCoalesced; }
void CoopNetworkTelemetry::RecordReliableSuperseded() { ++m_reliableSuperseded; }
void CoopNetworkTelemetry::RecordReliableTimeout() { ++m_reliableTimeouts; }

void CoopNetworkTelemetry::UpdateReliableQueue(size_t depth, float oldestAgeSeconds)
{
    m_reliableQueueDepth = depth;
    m_reliableQueueHighWater = std::max<uint64_t>(m_reliableQueueHighWater, depth);
    m_reliableQueueOldestAgeSeconds = std::max(0.0f, oldestAgeSeconds);
}

CoopNetworkTelemetry::RateSnapshot CoopNetworkTelemetry::BuildRateSnapshot() const
{
    RateSnapshot result;
    const uint64_t now = CurrentEpoch100ms();
    for (const RateBucket& bucket : m_rateBuckets)
    {
        if (bucket.epoch100ms == 0 || bucket.epoch100ms > now)
            continue;
        const uint64_t age = now - bucket.epoch100ms;
        if (age >= 100)
            continue;

        Totals sent;
        Totals received;
        for (size_t i = 0; i < kPacketTypeSlots; ++i)
        {
            Add(sent, bucket.sent[i].packets, bucket.sent[i].bytes);
            Add(received, bucket.received[i].packets, bucket.received[i].bytes);
        }
        Add(result.sent10s, sent.packets, sent.bytes);
        Add(result.received10s, received.packets, received.bytes);
        if (age < 10)
        {
            Add(result.sent1s, sent.packets, sent.bytes);
            Add(result.received1s, received.packets, received.bytes);
        }
    }
    return result;
}

std::string CoopNetworkTelemetry::BuildCompactStatus() const
{
    const RateSnapshot rates = BuildRateSnapshot();
    std::ostringstream out;
    out << m_wireSent.packets << "/" << m_wireSent.bytes
        << "/" << m_wireReceived.packets << "/" << m_wireReceived.bytes
        << "/" << rates.sent1s.packets << "/" << rates.sent1s.bytes
        << "/" << rates.received1s.packets << "/" << rates.received1s.bytes
        << "/" << rates.sent10s.packets << "/" << rates.sent10s.bytes
        << "/" << rates.received10s.packets << "/" << rates.received10s.bytes
        << "/" << m_reliableEnqueued.packets << "/" << m_reliableEnqueued.bytes
        << "/" << m_reliableResends
        << "/" << m_reliableQueueDepth << "/" << m_reliableQueueHighWater
        << "/" << std::fixed << std::setprecision(2) << m_reliableQueueOldestAgeSeconds;
    return out.str();
}

std::string CoopNetworkTelemetry::BuildDetailedReport() const
{
    const RateSnapshot rates = BuildRateSnapshot();
    const double efficiency = m_wireSent.bytes == 0
        ? 0.0
        : static_cast<double>(m_reliableEnqueued.bytes) / static_cast<double>(m_wireSent.bytes);
    std::ostringstream out;
    out << "v=1"
        << ",protocol=" << CoopProtocol::kProtocolVersion
        << ",wireTx=" << m_wireSent.packets << "/" << m_wireSent.bytes
        << ",wireRx=" << m_wireReceived.packets << "/" << m_wireReceived.bytes
        << ",tx1s=" << rates.sent1s.packets << "/" << rates.sent1s.bytes
        << ",rx1s=" << rates.received1s.packets << "/" << rates.received1s.bytes
        << ",tx10s=" << rates.sent10s.packets << "/" << rates.sent10s.bytes
        << ",rx10s=" << rates.received10s.packets << "/" << rates.received10s.bytes
        << ",wireFail=" << m_wireSendFailures.packets << "/" << m_wireSendFailures.bytes
        << ",reliable=" << m_reliableEnqueued.packets << "/" << m_reliableEnqueued.bytes
        << "/" << m_reliableReceived.packets << "/" << m_reliableReceived.bytes
        << ",resend=" << m_reliableResends
        << ",ack=" << m_reliableAcksSent << "/" << m_reliableAcksReceived
        << ",drop=" << m_reliableDrops
        << ",retired=" << m_reliableRetired
        << ",coalesced=" << m_reliableCoalesced
        << ",superseded=" << m_reliableSuperseded
        << ",timeout=" << m_reliableTimeouts
        << ",queue=" << m_reliableQueueDepth << "/" << m_reliableQueueHighWater
        << "/" << std::fixed << std::setprecision(2) << m_reliableQueueOldestAgeSeconds
        << ",payloadEfficiency=" << std::setprecision(4) << efficiency
        << ",suppressReasons=";

    bool wroteSuppressionReason = false;
    for (size_t type = 0; type < m_types.size(); ++type)
    {
        const TypeCounters& counters = m_types[type];
        if (counters.producerSuppressed == 0)
            continue;
        if (wroteSuppressionReason)
            out << "|";
        wroteSuppressionReason = true;
        out << type;
        for (const uint64_t count : counters.suppressionReasons)
            out << "." << count;
    }
    if (!wroteSuppressionReason)
        out << "-";

    for (const PacketDescriptor& descriptor : kPacketDescriptors)
    {
        const uint16_t type = static_cast<uint16_t>(descriptor.type);
        const TypeCounters& counters = m_types[TypeIndex(type)];
        out << ";" << descriptor.name
            << "@" << type
            << ":" << descriptor.size
            << ":" << descriptor.delivery
            << ":" << counters.sent.packets << "/" << counters.sent.bytes
            << ":" << counters.received.packets << "/" << counters.received.bytes
            << ":" << counters.reliableEnqueued.packets << "/" << counters.reliableEnqueued.bytes
            << ":" << counters.reliableReceived.packets << "/" << counters.reliableReceived.bytes
            << ":" << counters.producerAttempts << "/" << counters.producerSuppressed;
    }
    return out.str();
}
