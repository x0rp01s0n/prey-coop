#include "ModMain.h"
#include "CoopSerialSequence.h"

#include <cmath>

#include <Prey/CryGame/Game.h>
#include <Prey/GameDll/ark/ArkTimeScaleManager.h>

namespace
{
uint64_t BuildTimeEventId(uint64_t source, uint32_t sequence, uint32_t revision, uint16_t command)
{
    uint64_t hash = 14695981039346656037ull;
    const uint64_t values[] = {0x54494d4544494c41ull, source, sequence, revision, command};
    for (uint64_t value : values)
    {
        for (int byte = 0; byte < 8; ++byte)
        {
            hash ^= (value >> (byte * 8)) & 0xffu;
            hash *= 1099511628211ull;
        }
    }
    return hash == 0 ? 1 : hash;
}

ArkTimeScaleManager* GetTimeScaleManager()
{
    return g_pGame ? g_pGame->m_pArkTimeScaleManager.get() : nullptr;
}
}

bool ModMain::BuildTimeDilationPacket(
    CoopProtocol::TimeDilationPacket& packet,
    CoopProtocol::TimeDilationCommand command,
    unsigned timers,
    float scale) const
{
    if (m_localWorldEpoch == 0 || !std::isfinite(scale) || scale <= 0.0f || scale > 4.0f)
        return false;
    packet = {};
    packet.sequence = m_timeDilationSequence;
    packet.worldEpoch = m_localWorldEpoch;
    packet.hostSaveKeyHash = CurrentHostSaveKeyHash();
    packet.sourcePeerHash = GetLocalAccountToken();
    packet.revision = m_timeDilationRevision;
    packet.timers = timers;
    packet.command = static_cast<uint16_t>(command);
    packet.scale = scale;
    packet.eventId = BuildTimeEventId(packet.sourcePeerHash, packet.sequence, packet.revision, packet.command);
    return packet.hostSaveKeyHash != 0 && packet.sourcePeerHash != 0;
}

bool ModMain::SendTimeDilationTo(
    const CoopProtocol::TimeDilationPacket& packet,
    uint32_t address,
    uint16_t port,
    const char* failurePrefix)
{
    if (!SendReliablePayloadTo(
            static_cast<uint16_t>(CoopProtocol::PacketType::TimeDilation),
            &packet, sizeof(packet), address, port, failurePrefix))
    {
        ++m_timeDilationDropped;
        return false;
    }
    ++m_timeDilationSent;
    return true;
}

void ModMain::OnNativeTimeScaleOverride(
    ArkTimeScaleManager*, unsigned timers, float scale, int handle)
{
    if (m_timeDilationApplyDepth != 0 || m_networkMode == CoopNetworkMode::Off ||
        !m_hasRemoteEndpoint || !IsSessionGameplayReady() || !std::isfinite(scale))
    {
        return;
    }
    m_timeDilationLocalHandle = handle;
    m_timeDilationTimers = timers;
    m_timeDilationScale = scale;
    m_timeDilationOwnerHash = GetLocalAccountToken();
    CoopSerialSequence::Advance(m_timeDilationSequence);
    if (m_networkMode == CoopNetworkMode::Host)
        ++m_timeDilationRevision;
    CoopProtocol::TimeDilationPacket packet;
    const auto command = m_networkMode == CoopNetworkMode::Host
        ? CoopProtocol::TimeDilationCommand::Start
        : CoopProtocol::TimeDilationCommand::RequestStart;
    if (BuildTimeDilationPacket(packet, command, timers, scale))
        SendTimeDilationTo(packet, m_remoteAddress, m_remotePort, "time dilation start failed");
    m_lastTimeDilationEvent = "native_start_scale_" + std::to_string(scale);
}

void ModMain::OnNativeTimeScaleUpdate(ArkTimeScaleManager* manager, int handle, float scale)
{
    if (handle != m_timeDilationLocalHandle || !std::isfinite(scale) ||
        std::fabs(scale - m_timeDilationScale) < 0.001f)
        return;
    OnNativeTimeScaleOverride(manager, m_timeDilationTimers, scale, handle);
}

void ModMain::OnNativeTimeScaleClear(ArkTimeScaleManager*, int handle)
{
    if (m_timeDilationApplyDepth != 0 || handle != m_timeDilationLocalHandle ||
        m_networkMode == CoopNetworkMode::Off || !m_hasRemoteEndpoint || !IsSessionGameplayReady())
    {
        return;
    }
    CoopSerialSequence::Advance(m_timeDilationSequence);
    if (m_networkMode == CoopNetworkMode::Host)
        ++m_timeDilationRevision;
    CoopProtocol::TimeDilationPacket packet;
    const auto command = m_networkMode == CoopNetworkMode::Host
        ? CoopProtocol::TimeDilationCommand::End
        : CoopProtocol::TimeDilationCommand::RequestEnd;
    if (BuildTimeDilationPacket(packet, command, m_timeDilationTimers, 1.0f))
        SendTimeDilationTo(packet, m_remoteAddress, m_remotePort, "time dilation end failed");
    m_timeDilationLocalHandle = -1;
    m_timeDilationOwnerHash = 0;
    m_timeDilationTimers = 0;
    m_timeDilationScale = 1.0f;
    m_lastTimeDilationEvent = "native_end";
}

void ModMain::HandleTimeDilation(const CoopProtocol::TimeDilationPacket& packet)
{
    ++m_timeDilationReceived;
    const auto command = static_cast<CoopProtocol::TimeDilationCommand>(packet.command);
    const uint64_t localSave = CurrentHostSaveKeyHash();
    if (packet.sourcePeerHash == 0 || packet.worldEpoch != m_localWorldEpoch ||
        (packet.hostSaveKeyHash != 0 && localSave != 0 &&
            !IsCurrentOrRecentHostSaveKeyHash(packet.hostSaveKeyHash)) ||
        !std::isfinite(packet.scale) || packet.scale <= 0.0f || packet.scale > 4.0f)
    {
        ++m_timeDilationDropped;
        m_lastTimeDilationEvent = "guard_drop";
        return;
    }

    ArkTimeScaleManager* manager = GetTimeScaleManager();
    if (!manager)
    {
        ++m_timeDilationDropped;
        m_lastTimeDilationEvent = "missing_manager";
        return;
    }
        const uint64_t localPeer = GetLocalAccountToken();

    if (command == CoopProtocol::TimeDilationCommand::RequestStart)
    {
        if (m_networkMode != CoopNetworkMode::Host)
        {
            ++m_timeDilationDropped;
            return;
        }
        ++m_timeDilationRevision;
        ++m_timeDilationApplyDepth;
        if (m_timeDilationRemoteHandle >= 0)
            manager->ClearTimeScaleOverride(m_timeDilationRemoteHandle);
        m_timeDilationRemoteHandle = manager->OverrideTimeScale(packet.timers, packet.scale);
        --m_timeDilationApplyDepth;
        m_timeDilationOwnerHash = packet.sourcePeerHash;
        m_timeDilationTimers = packet.timers;
        m_timeDilationScale = packet.scale;
        ++m_timeDilationApplied;
        CoopSerialSequence::Advance(m_timeDilationSequence);
        CoopProtocol::TimeDilationPacket response;
        if (BuildTimeDilationPacket(response, CoopProtocol::TimeDilationCommand::Start, packet.timers, packet.scale))
        {
            response.sourcePeerHash = packet.sourcePeerHash;
            response.eventId = BuildTimeEventId(response.sourcePeerHash, response.sequence, response.revision, response.command);
            SendTimeDilationTo(response, m_remoteAddress, m_remotePort, "time dilation grant start failed");
        }
        m_lastTimeDilationEvent = "host_applied_request_start";
        return;
    }

    if (command == CoopProtocol::TimeDilationCommand::Start)
    {
        if (packet.revision < m_timeDilationRevision)
            return;
        m_timeDilationRevision = packet.revision;
        m_timeDilationOwnerHash = packet.sourcePeerHash;
        m_timeDilationTimers = packet.timers;
        m_timeDilationScale = packet.scale;
        if (packet.sourcePeerHash != localPeer)
        {
            ++m_timeDilationApplyDepth;
            if (m_timeDilationRemoteHandle >= 0)
                manager->ClearTimeScaleOverride(m_timeDilationRemoteHandle);
            m_timeDilationRemoteHandle = manager->OverrideTimeScale(packet.timers, packet.scale);
            --m_timeDilationApplyDepth;
        }
        ++m_timeDilationApplied;
        m_lastTimeDilationEvent = "applied_start";
        return;
    }

    if (command == CoopProtocol::TimeDilationCommand::RequestEnd)
    {
        if (m_networkMode != CoopNetworkMode::Host || m_timeDilationOwnerHash != packet.sourcePeerHash)
        {
            ++m_timeDilationDropped;
            return;
        }
        ++m_timeDilationRevision;
        ++m_timeDilationApplyDepth;
        if (m_timeDilationRemoteHandle >= 0)
            manager->ClearTimeScaleOverride(m_timeDilationRemoteHandle);
        --m_timeDilationApplyDepth;
        m_timeDilationRemoteHandle = -1;
        m_timeDilationOwnerHash = 0;
        m_timeDilationScale = 1.0f;
        ++m_timeDilationApplied;
        CoopSerialSequence::Advance(m_timeDilationSequence);
        CoopProtocol::TimeDilationPacket response;
        if (BuildTimeDilationPacket(response, CoopProtocol::TimeDilationCommand::End, packet.timers, 1.0f))
            SendTimeDilationTo(response, m_remoteAddress, m_remotePort, "time dilation grant end failed");
        m_lastTimeDilationEvent = "host_applied_request_end";
        return;
    }

    if (command == CoopProtocol::TimeDilationCommand::End)
    {
        if (packet.revision < m_timeDilationRevision)
            return;
        m_timeDilationRevision = packet.revision;
        ++m_timeDilationApplyDepth;
        if (m_timeDilationRemoteHandle >= 0)
            manager->ClearTimeScaleOverride(m_timeDilationRemoteHandle);
        --m_timeDilationApplyDepth;
        m_timeDilationRemoteHandle = -1;
        m_timeDilationOwnerHash = 0;
        m_timeDilationScale = 1.0f;
        ++m_timeDilationApplied;
        m_lastTimeDilationEvent = "applied_end";
        return;
    }

    ++m_timeDilationDropped;
}

void ModMain::ResetTimeDilationState(const char* reason)
{
    m_timeDilationSequence = 0;
    m_timeDilationRevision = 0;
    m_timeDilationSent = 0;
    m_timeDilationReceived = 0;
    m_timeDilationApplied = 0;
    m_timeDilationDropped = 0;
    m_timeDilationApplyDepth = 0;
    m_timeDilationOwnerHash = 0;
    m_timeDilationTimers = 0;
    m_timeDilationScale = 1.0f;
    m_timeDilationLocalHandle = -1;
    m_timeDilationRemoteHandle = -1;
    m_lastTimeDilationEvent = reason && reason[0] ? reason : "reset";
}
