#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

class CoopNetworkTelemetry final
{
public:
    static constexpr size_t kPacketTypeSlots = 32;

    enum class ProducerSuppressionReason : uint8_t
    {
        Unchanged = 0,
        RateLimited,
        Authority,
        Readiness,
        InvalidState,
        Budget,
        Count,
    };

    struct Totals
    {
        uint64_t packets = 0;
        uint64_t bytes = 0;
    };

    struct RateSnapshot
    {
        Totals sent1s;
        Totals received1s;
        Totals sent10s;
        Totals received10s;
    };

    void Reset();

    void RecordWireSendAttempt(uint16_t packetType, uint32_t bytes);
    void RecordWireSendSuccess(uint16_t packetType, uint32_t bytes);
    void RecordWireSendFailure(uint16_t packetType, uint32_t bytes);
    void RecordWireReceive(uint16_t packetType, uint32_t bytes);
    void RecordProducerAttempt(uint16_t packetType);
    void RecordProducerSuppressed(
        uint16_t packetType,
        ProducerSuppressionReason reason = ProducerSuppressionReason::Unchanged);

    void RecordReliableEnqueue(uint16_t payloadType, uint32_t payloadBytes);
    void RecordReliablePayloadReceive(uint16_t payloadType, uint32_t payloadBytes);
    void RecordReliableResend();
    void RecordReliableAckSent();
    void RecordReliableAckReceived();
    void RecordReliableDrop();
    void RecordReliableRetired();
    void RecordReliableCoalesced();
    void RecordReliableSuperseded();
    void RecordReliableTimeout();
    void UpdateReliableQueue(size_t depth, float oldestAgeSeconds);

    std::string BuildCompactStatus() const;
    std::string BuildDetailedReport() const;

private:
    struct TypeCounters
    {
        Totals sendAttempts;
        Totals sent;
        Totals sendFailures;
        Totals received;
        Totals reliableEnqueued;
        Totals reliableReceived;
        uint64_t producerAttempts = 0;
        uint64_t producerSuppressed = 0;
        std::array<uint64_t, static_cast<size_t>(ProducerSuppressionReason::Count)> suppressionReasons = {};
    };

    struct RateBucket
    {
        uint64_t epoch100ms = 0;
        std::array<Totals, kPacketTypeSlots> sent = {};
        std::array<Totals, kPacketTypeSlots> received = {};
    };

    static uint64_t CurrentEpoch100ms();
    static size_t TypeIndex(uint16_t packetType);
    RateBucket& CurrentRateBucket();
    RateSnapshot BuildRateSnapshot() const;

    std::array<TypeCounters, kPacketTypeSlots> m_types = {};
    std::array<RateBucket, 100> m_rateBuckets = {};
    Totals m_wireSendAttempts;
    Totals m_wireSent;
    Totals m_wireSendFailures;
    Totals m_wireReceived;
    Totals m_reliableEnqueued;
    Totals m_reliableReceived;
    uint64_t m_reliableResends = 0;
    uint64_t m_reliableAcksSent = 0;
    uint64_t m_reliableAcksReceived = 0;
    uint64_t m_reliableDrops = 0;
    uint64_t m_reliableRetired = 0;
    uint64_t m_reliableCoalesced = 0;
    uint64_t m_reliableSuperseded = 0;
    uint64_t m_reliableTimeouts = 0;
    uint64_t m_reliableQueueDepth = 0;
    uint64_t m_reliableQueueHighWater = 0;
    float m_reliableQueueOldestAgeSeconds = 0.0f;
};
