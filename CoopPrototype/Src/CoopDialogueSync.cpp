#include "ModMain.h"
#include "CoopSerialSequence.h"

namespace
{
constexpr float kDialogueLeaseTimeoutSeconds = 20.0f;

uint64_t BuildDialogueLeaseEventId(uint64_t source, uint64_t dialogueId, uint32_t sequence, uint16_t command)
{
    uint64_t hash = 14695981039346656037ull;
    const uint64_t values[] = {0x4449414c4f475545ull, source, dialogueId, sequence, command};
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
}

bool ModMain::BuildDialogueLeasePacket(
    CoopProtocol::DialogueLeasePacket& packet,
    CoopProtocol::DialogueLeaseCommand command,
    uint64_t dialogueId,
    uint64_t targetPeerHash) const
{
    if (dialogueId == 0 || m_localWorldEpoch == 0 || m_localLevelId == 0)
        return false;

    packet = {};
    packet.sequence = m_dialogueLeaseSequence;
    packet.worldEpoch = m_localWorldEpoch;
    packet.hostSaveKeyHash = CurrentHostSaveKeyHash();
    packet.areaId = m_localLevelId;
    packet.sourcePeerHash = GetLocalAccountToken();
    packet.targetPeerHash = targetPeerHash;
    packet.dialogueId = dialogueId;
    packet.leaseEpoch = m_dialogueLeaseEpoch;
    packet.command = static_cast<uint16_t>(command);
    packet.eventId = BuildDialogueLeaseEventId(
        packet.sourcePeerHash, dialogueId, packet.sequence, packet.command);
    return packet.hostSaveKeyHash != 0 && packet.sourcePeerHash != 0;
}

bool ModMain::SendDialogueLeaseTo(
    const CoopProtocol::DialogueLeasePacket& packet,
    uint32_t address,
    uint16_t port,
    const char* failurePrefix)
{
    if (!SendReliablePayloadTo(
            static_cast<uint16_t>(CoopProtocol::PacketType::DialogueLease),
            &packet,
            sizeof(packet),
            address,
            port,
            failurePrefix))
    {
        ++m_dialogueLeaseDropped;
        return false;
    }
    ++m_dialogueLeaseSent;
    return true;
}

bool ModMain::RequestDialogueLease(uint64_t dialogueId, const char* reason)
{
    if (dialogueId == 0 || m_networkMode == CoopNetworkMode::Off ||
        !m_hasRemoteEndpoint || !IsSessionGameplayReady())
    {
        return false;
    }

    const uint64_t localPeer = GetLocalAccountToken();
    if (m_networkMode == CoopNetworkMode::Host)
    {
        if (m_dialogueLeaseOwnerHash != 0 && m_dialogueLeaseOwnerHash != localPeer)
        {
            ++m_dialogueLeaseDenied;
            return false;
        }
        if (m_dialogueLeaseOwnerHash == 0 || m_dialogueLeaseDialogueId != dialogueId)
        {
            ++m_dialogueLeaseEpoch;
            if (m_dialogueLeaseEpoch == 0)
                ++m_dialogueLeaseEpoch;
        }
        m_dialogueLeaseOwnerHash = localPeer;
        m_dialogueLeaseDialogueId = dialogueId;
        m_dialogueLeaseSeconds = 0.0f;
        CoopSerialSequence::Advance(m_dialogueLeaseSequence);
        CoopProtocol::DialogueLeasePacket packet;
        if (!BuildDialogueLeasePacket(packet, CoopProtocol::DialogueLeaseCommand::Grant, dialogueId) ||
            !SendDialogueLeaseTo(packet, m_remoteAddress, m_remotePort, "dialogue host grant failed"))
        {
            return false;
        }
        ++m_dialogueLeaseApplied;
        m_lastDialogueLeaseEvent = "host_granted_dialogue_" + std::to_string(dialogueId) +
            "_reason_" + (reason && reason[0] ? reason : "-");
        return true;
    }

    if (m_dialogueLeaseOwnerHash == localPeer && m_dialogueLeaseDialogueId == dialogueId)
    {
        m_dialogueLeaseSeconds = 0.0f;
        return true;
    }
    CoopSerialSequence::Advance(m_dialogueLeaseSequence);
    CoopProtocol::DialogueLeasePacket packet;
    if (!BuildDialogueLeasePacket(packet, CoopProtocol::DialogueLeaseCommand::Request, dialogueId) ||
        !SendDialogueLeaseTo(packet, m_remoteAddress, m_remotePort, "dialogue request failed"))
    {
        return false;
    }
    m_dialogueLeasePendingId = dialogueId;
    m_lastDialogueLeaseEvent = "requested_dialogue_" + std::to_string(dialogueId);
    return true;
}

void ModMain::ObserveLocalDialogueActivity(uint64_t dialogueId, uint64_t lineId, const char* reason)
{
    if (dialogueId == 0)
        return;
    const bool starting = m_dialogueLeaseDialogueId != dialogueId && m_dialogueLeasePendingId != dialogueId;
    RequestDialogueLease(dialogueId, reason);
    if (starting)
        QueueLocalStoryEventForHook(CoopProtocol::kStoryEventConversationStarted, dialogueId, 1, 0, reason);
    if (lineId != 0)
        QueueLocalStoryEventForHook(CoopProtocol::kStoryEventDialogueLine, lineId, 1, 0, reason);
}

void ModMain::OnLocalDialogueCompleted(uint64_t dialogueId, bool complete, const char* reason)
{
    if (dialogueId == 0)
        return;
    QueueLocalStoryEventForHook(
        CoopProtocol::kStoryEventConversationEnded,
        dialogueId,
        complete ? 1 : 0,
        0,
        reason);
    if (m_dialogueLeaseDialogueId == dialogueId)
        ReleaseLocalDialogueLease(reason);
    if (m_dialogueLeasePendingId == dialogueId)
        m_dialogueLeasePendingId = 0;
}

bool ModMain::ReleaseLocalDialogueLease(const char* reason)
{
    const uint64_t localPeer = GetLocalAccountToken();
    if (m_dialogueLeaseDialogueId == 0 || m_dialogueLeaseOwnerHash != localPeer)
        return false;

    const uint64_t dialogueId = m_dialogueLeaseDialogueId;
    CoopSerialSequence::Advance(m_dialogueLeaseSequence);
    CoopProtocol::DialogueLeasePacket packet;
    const bool built = BuildDialogueLeasePacket(
        packet, CoopProtocol::DialogueLeaseCommand::Release, dialogueId);
    if (m_networkMode == CoopNetworkMode::Host)
    {
        m_dialogueLeaseOwnerHash = 0;
        m_dialogueLeaseDialogueId = 0;
        m_dialogueLeaseSeconds = 0.0f;
    }
    const bool sent = built && SendDialogueLeaseTo(
        packet, m_remoteAddress, m_remotePort, "dialogue release failed");
    m_lastDialogueLeaseEvent = "released_dialogue_" + std::to_string(dialogueId) +
        "_reason_" + (reason && reason[0] ? reason : "-");
    return sent;
}

void ModMain::HandleDialogueLease(const CoopProtocol::DialogueLeasePacket& packet)
{
    ++m_dialogueLeaseReceived;
    const uint64_t localPeer = GetLocalAccountToken();
    const auto command = static_cast<CoopProtocol::DialogueLeaseCommand>(packet.command);
    if (packet.dialogueId == 0 || packet.sourcePeerHash == 0 ||
        packet.worldEpoch != m_localWorldEpoch ||
        !IsCurrentOrRecentHostSaveKeyHash(packet.hostSaveKeyHash) ||
        (packet.targetPeerHash != 0 && packet.targetPeerHash != localPeer))
    {
        ++m_dialogueLeaseDropped;
        m_lastDialogueLeaseEvent = "guard_drop_command_" + std::to_string(packet.command);
        return;
    }

    if (command == CoopProtocol::DialogueLeaseCommand::Request)
    {
        if (m_networkMode != CoopNetworkMode::Host)
        {
            ++m_dialogueLeaseDropped;
            return;
        }

        CoopSerialSequence::Advance(m_dialogueLeaseSequence);
        CoopProtocol::DialogueLeasePacket response;
        if (m_dialogueLeaseOwnerHash != 0 && m_dialogueLeaseOwnerHash != packet.sourcePeerHash)
        {
            ++m_dialogueLeaseDenied;
            BuildDialogueLeasePacket(
                response, CoopProtocol::DialogueLeaseCommand::Deny,
                packet.dialogueId, packet.sourcePeerHash);
            SendDialogueLeaseTo(response, m_remoteAddress, m_remotePort, "dialogue deny failed");
            m_lastDialogueLeaseEvent = "denied_dialogue_" + std::to_string(packet.dialogueId);
            return;
        }

        if (m_dialogueLeaseOwnerHash == 0 || m_dialogueLeaseDialogueId != packet.dialogueId)
        {
            ++m_dialogueLeaseEpoch;
            if (m_dialogueLeaseEpoch == 0)
                ++m_dialogueLeaseEpoch;
        }
        m_dialogueLeaseOwnerHash = packet.sourcePeerHash;
        m_dialogueLeaseDialogueId = packet.dialogueId;
        m_dialogueLeaseSeconds = 0.0f;
        BuildDialogueLeasePacket(
            response, CoopProtocol::DialogueLeaseCommand::Grant,
            packet.dialogueId, packet.sourcePeerHash);
        response.leaseEpoch = m_dialogueLeaseEpoch;
        SendDialogueLeaseTo(response, m_remoteAddress, m_remotePort, "dialogue grant failed");
        ++m_dialogueLeaseApplied;
        m_lastDialogueLeaseEvent = "granted_remote_dialogue_" + std::to_string(packet.dialogueId);
        return;
    }

    if (command == CoopProtocol::DialogueLeaseCommand::Grant)
    {
        if (m_networkMode != CoopNetworkMode::Client || packet.leaseEpoch == 0)
        {
            ++m_dialogueLeaseDropped;
            return;
        }
        m_dialogueLeaseOwnerHash = localPeer;
        m_dialogueLeaseDialogueId = packet.dialogueId;
        m_dialogueLeasePendingId = 0;
        m_dialogueLeaseEpoch = packet.leaseEpoch;
        m_dialogueLeaseSeconds = 0.0f;
        ++m_dialogueLeaseApplied;
        m_lastDialogueLeaseEvent = "grant_applied_dialogue_" + std::to_string(packet.dialogueId);
        return;
    }

    if (command == CoopProtocol::DialogueLeaseCommand::Deny)
    {
        ++m_dialogueLeaseDenied;
        if (m_dialogueLeasePendingId == packet.dialogueId)
            m_dialogueLeasePendingId = 0;
        m_lastDialogueLeaseEvent = "deny_applied_dialogue_" + std::to_string(packet.dialogueId);
        return;
    }

    if (command == CoopProtocol::DialogueLeaseCommand::Activity)
    {
        if (m_networkMode == CoopNetworkMode::Host &&
            m_dialogueLeaseOwnerHash == packet.sourcePeerHash &&
            m_dialogueLeaseDialogueId == packet.dialogueId)
        {
            m_dialogueLeaseSeconds = 0.0f;
            ++m_dialogueLeaseApplied;
        }
        return;
    }

    if (command == CoopProtocol::DialogueLeaseCommand::Release)
    {
        if (m_networkMode == CoopNetworkMode::Host &&
            (m_dialogueLeaseOwnerHash != packet.sourcePeerHash ||
             m_dialogueLeaseDialogueId != packet.dialogueId))
        {
            ++m_dialogueLeaseDropped;
            return;
        }
        if (m_networkMode == CoopNetworkMode::Host)
        {
            CoopSerialSequence::Advance(m_dialogueLeaseSequence);
            CoopProtocol::DialogueLeasePacket response;
            BuildDialogueLeasePacket(
                response, CoopProtocol::DialogueLeaseCommand::Release,
                packet.dialogueId);
            response.leaseEpoch = m_dialogueLeaseEpoch;
            SendDialogueLeaseTo(response, m_remoteAddress, m_remotePort, "dialogue release broadcast failed");
        }
        m_dialogueLeaseOwnerHash = 0;
        m_dialogueLeaseDialogueId = 0;
        m_dialogueLeaseSeconds = 0.0f;
        ++m_dialogueLeaseApplied;
        m_lastDialogueLeaseEvent = "release_applied_dialogue_" + std::to_string(packet.dialogueId);
        return;
    }

    ++m_dialogueLeaseDropped;
}

void ModMain::TickDialogueLease(float frameTime)
{
    if (frameTime <= 0.0f || m_dialogueLeaseOwnerHash == 0)
        return;
    m_dialogueLeaseSeconds += frameTime;
    if (m_networkMode != CoopNetworkMode::Host ||
        m_dialogueLeaseSeconds < kDialogueLeaseTimeoutSeconds)
    {
        return;
    }

    const uint64_t dialogueId = m_dialogueLeaseDialogueId;
    CoopSerialSequence::Advance(m_dialogueLeaseSequence);
    CoopProtocol::DialogueLeasePacket packet;
    if (BuildDialogueLeasePacket(packet, CoopProtocol::DialogueLeaseCommand::Release, dialogueId))
        SendDialogueLeaseTo(packet, m_remoteAddress, m_remotePort, "dialogue timeout release failed");
    m_dialogueLeaseOwnerHash = 0;
    m_dialogueLeaseDialogueId = 0;
    m_dialogueLeasePendingId = 0;
    m_dialogueLeaseSeconds = 0.0f;
    m_lastDialogueLeaseEvent = "lease_timeout_dialogue_" + std::to_string(dialogueId);
}

void ModMain::ResetDialogueLeaseState(const char* reason)
{
    m_dialogueLeaseDialogueId = 0;
    m_dialogueLeasePendingId = 0;
    m_dialogueLeaseOwnerHash = 0;
    m_dialogueLeaseSequence = 0;
    m_dialogueLeaseEpoch = 0;
    m_dialogueLeaseSent = 0;
    m_dialogueLeaseReceived = 0;
    m_dialogueLeaseApplied = 0;
    m_dialogueLeaseDropped = 0;
    m_dialogueLeaseDenied = 0;
    m_dialogueLeaseSeconds = 0.0f;
    m_lastDialogueLeaseEvent = reason && reason[0] ? reason : "reset";
}
