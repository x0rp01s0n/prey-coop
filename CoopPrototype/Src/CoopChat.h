#pragma once

#include "CoopProtocol.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>

class CoopChat final
{
public:
    using DatagramSender = std::function<bool(const void* data, size_t size, const char* failurePrefix)>;

    CoopChat() = default;
    ~CoopChat() = default;

    CoopChat(const CoopChat&) = delete;
    CoopChat& operator=(const CoopChat&) = delete;

    struct HudState
    {
        std::string text;
    };

    void SetDatagramSender(DatagramSender sender);
    void SetIdentity(uint64_t accountToken, const std::string& username);
    void SetConnected(bool connected);
    void Tick(float frameTime, float nowSeconds);
    HudState BuildHudState(float nowSeconds) const;
    bool HandleNativeWindowMessage(unsigned message, uint64_t wParam, int64_t lParam);
    bool SendText(const std::string& text);
    bool CanAcceptTextPacket(const CoopProtocol::TextChatPacket& packet) const;
    bool HandleTextPacket(const CoopProtocol::TextChatPacket& packet, const std::string& username, float nowSeconds);
    void RemoveSender(uint64_t accountToken);
    bool IsInputOpen() const { return m_chatOpen; }
    std::string BuildTelemetry() const;
    void Reset();

private:
    struct ChatLine
    {
        std::string username;
        std::string text;
        float receivedAt = 0.0f;
    };

    void AppendInputUtf16(uint16_t value);
    void SubmitInput();
    void AddChatLine(const std::string& username, const std::string& text, float nowSeconds);
    static std::string LimitUtf8(const std::string& text);
    static bool IsValidUtf8(const char* text, size_t length, size_t* outCharacters);
    static std::string WideCharToUtf8(uint32_t codepoint);

    DatagramSender m_sendDatagram;
    uint64_t m_accountToken = 0;
    std::string m_username = "Player";
    bool m_connected = false;
    bool m_chatOpen = false;
    bool m_ignoreNextChar = false;
    uint32_t m_textSequence = 0;
    uint16_t m_pendingHighSurrogate = 0;
    std::string m_input;
    std::deque<ChatLine> m_chatLines;
    std::unordered_map<uint64_t, uint32_t> m_lastTextSequences;
    float m_lastTickSeconds = 0.0f;
    uint64_t m_textSent = 0;
    uint64_t m_textReceived = 0;
};
