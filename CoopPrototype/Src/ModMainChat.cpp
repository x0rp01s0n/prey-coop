#include "ModMain.h"
#include "CoopChat.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
float ChatNowSeconds()
{
    return gEnv && gEnv->pTimer ? gEnv->pTimer->GetAsyncCurTime() : 0.0f;
}
}

void ModMain::InitializeChat()
{
    if (!m_chat)
        m_chat = new CoopChat();

    m_chat->SetDatagramSender(
        [this](const void* data, size_t size, const char* failurePrefix)
        {
            return SendChatDatagram(data, static_cast<int>(size), failurePrefix);
        });
    m_chat->SetIdentity(GetLocalAccountToken(), GetLocalUsername());
    m_nullUi.ClearChatText();
}

void ModMain::ShutdownChat()
{
    delete m_chat;
    m_chat = nullptr;
    m_nullUi.ClearChatText();
}

bool ModMain::HandleChatWindowMessage(unsigned message, uint64_t wParam, int64_t lParam)
{
    return m_chat && m_chat->HandleNativeWindowMessage(message, wParam, lParam);
}

void ModMain::TickChat(float frameTime, float nowSeconds)
{
    if (!m_chat)
        return;

    m_chat->SetIdentity(GetLocalAccountToken(), GetLocalUsername());
    const bool connected =
        m_networkMode != CoopNetworkMode::Off &&
        m_socket != kInvalidNetworkSocket &&
        (m_networkMode == CoopNetworkMode::Host || m_hasRemoteSession);
    m_chat->SetConnected(connected);
    m_chat->Tick(frameTime, nowSeconds);
    m_nullUi.SetChatText(m_chat->BuildHudState(nowSeconds).text);
}

bool ModMain::IsChatInputOpen() const
{
    return m_chat && m_chat->IsInputOpen();
}

std::string ModMain::BuildChatTelemetry() const
{
    return m_chat ? m_chat->BuildTelemetry() : "uninitialized";
}

bool ModMain::SendChatTextCommand(const std::string& text)
{
    return m_chat && m_chat->SendText(text);
}

void ModMain::ResetChat()
{
    if (m_chat)
        m_chat->Reset();
    m_nullUi.ClearChatText();
}

void ModMain::RemoveChatSender(uint64_t accountToken)
{
    if (m_chat)
        m_chat->RemoveSender(accountToken);
}

bool ModMain::CanAcceptChatTextPacket(const CoopProtocol::TextChatPacket& packet) const
{
    return m_chat && m_chat->CanAcceptTextPacket(packet);
}

bool ModMain::HandleChatTextPacket(
    const CoopProtocol::TextChatPacket& packet,
    const std::string& username,
    float nowSeconds)
{
    return m_chat && m_chat->HandleTextPacket(packet, username, nowSeconds);
}

bool ModMain::AcceptChatTextRate(uint64_t accountToken, float nowSeconds)
{
    if (accountToken == 0 || !std::isfinite(nowSeconds))
        return false;

    ChatTextRateState& state = m_chatTextRates[accountToken];
    if (state.windowStart < 0.0f || nowSeconds - state.windowStart >= 3.0f)
    {
        state.windowStart = nowSeconds;
        state.lastMessage = -1000.0f;
        state.count = 0;
    }
    if (nowSeconds - state.lastMessage < 0.20f || state.count >= 8)
        return false;
    state.lastMessage = nowSeconds;
    ++state.count;
    return true;
}

bool ModMain::SendChatDatagram(const void* packet, int packetSize, const char* failurePrefix)
{
    if (!packet || packetSize <= 0 || m_socket == kInvalidNetworkSocket ||
        m_networkMode == CoopNetworkMode::Off)
        return false;

    if (m_networkMode == CoopNetworkMode::Client)
    {
        uint32_t address = 0;
        uint16_t port = 0;
        if (!ResolveSessionHostEndpoint(address, port))
            return false;
        return SendPacketTo(packet, packetSize, address, port, failurePrefix);
    }

    bool sent = false;
    bool allSent = true;
    for (const auto& entry : m_remotePeers)
    {
        const RemotePeerSession& peer = entry.second;
        if (peer.accountToken == 0 || peer.address == 0 || peer.port == 0)
            continue;
        sent = true;
        allSent = SendPacketTo(packet, packetSize, peer.address, peer.port, failurePrefix) && allSent;
    }
    // A host can still use chat locally before another player has joined.
    return m_networkMode == CoopNetworkMode::Host ? (!sent || allSent) : (sent && allSent);
}

void ModMain::RelayChatTextToPeers(
    const void* packet,
    int packetSize,
    uint32_t fromAddress,
    uint16_t fromPort,
    const char* failurePrefix)
{
    if (m_networkMode != CoopNetworkMode::Host || !packet || packetSize <= 0)
        return;

    // Text chat is session-global, unlike area-scoped gameplay datagrams.
    // Relay it to connected peers even while they are in another area or
    // still finishing a world transition.
    for (const auto& entry : m_remotePeers)
    {
        const RemotePeerSession& peer = entry.second;
        if (peer.accountToken == 0 || peer.address == 0 || peer.port == 0 ||
            (peer.address == fromAddress && peer.port == fromPort))
            continue;
        SendPacketTo(packet, packetSize, peer.address, peer.port, failurePrefix);
    }
}

bool ModMain::HandleChatDatagram(
    const CoopProtocol::PacketHeader& header,
    const void* packetData,
    int packetBytes,
    uint32_t fromAddress,
    uint16_t fromPort)
{
    if (header.type != static_cast<uint16_t>(CoopProtocol::PacketType::TextChat))
        return false;
    if (!packetData || packetBytes != static_cast<int>(sizeof(CoopProtocol::TextChatPacket)))
        return true;

    CoopProtocol::TextChatPacket packet = {};
    std::memcpy(&packet, packetData, sizeof(packet));
    const float now = ChatNowSeconds();
    const RemotePeerSession* sourcePeer = FindRemotePeerByEndpoint(fromAddress, fromPort);
    if (m_networkMode == CoopNetworkMode::Host)
    {
        if (!sourcePeer || packet.sourceAccountToken == 0 ||
            sourcePeer->accountToken != packet.sourceAccountToken ||
            !CanAcceptChatTextPacket(packet) ||
            !AcceptChatTextRate(packet.sourceAccountToken, now) ||
            !HandleChatTextPacket(packet, sourcePeer->username, now))
            return true;
        RelayChatTextToPeers(&packet, sizeof(packet), fromAddress, fromPort, "chat relay failed");
        return true;
    }

    if (m_networkMode != CoopNetworkMode::Client || !sourcePeer ||
        sourcePeer->accountToken != m_primaryRemotePeerToken ||
        packet.sourceAccountToken == 0 ||
        packet.sourceAccountToken == GetLocalAccountToken())
        return true;

    const auto peerIt = m_remotePeers.find(packet.sourceAccountToken);
    if (peerIt == m_remotePeers.end() || !CanAcceptChatTextPacket(packet))
        return true;
    HandleChatTextPacket(packet, peerIt->second.username, now);
    return true;
}
