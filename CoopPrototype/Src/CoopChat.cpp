#include "CoopChat.h"
#include "CoopSerialSequence.h"

#include <Windows.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <utility>

namespace
{
constexpr float kChatLineLifetimeSeconds = 8.0f;
constexpr size_t kMaxChatLines = 32;

uint32_t CombineSurrogate(uint16_t high, uint16_t low)
{
    return 0x10000u + ((static_cast<uint32_t>(high) - 0xd800u) << 10) +
        (static_cast<uint32_t>(low) - 0xdc00u);
}
}

void CoopChat::SetDatagramSender(DatagramSender sender)
{
    m_sendDatagram = std::move(sender);
}

void CoopChat::SetIdentity(uint64_t accountToken, const std::string& username)
{
    m_accountToken = accountToken;
    if (!username.empty())
        m_username = username;
}

void CoopChat::SetConnected(bool connected)
{
    if (m_connected == connected)
        return;

    m_connected = connected;
    if (!connected)
    {
        m_chatOpen = false;
        m_input.clear();
        m_pendingHighSurrogate = 0;
        m_ignoreNextChar = false;
    }
}

void CoopChat::Tick(float frameTime, float nowSeconds)
{
    if (std::isfinite(nowSeconds) && nowSeconds >= 0.0f)
        m_lastTickSeconds = nowSeconds;
    else if (std::isfinite(frameTime) && frameTime > 0.0f)
        m_lastTickSeconds += std::min(frameTime, 0.25f);
}

void CoopChat::Reset()
{
    m_connected = false;
    m_chatOpen = false;
    m_ignoreNextChar = false;
    m_pendingHighSurrogate = 0;
    m_input.clear();
    m_chatLines.clear();
    m_lastTextSequences.clear();
    m_textSequence = 0;
    m_textSent = 0;
    m_textReceived = 0;
    m_lastTickSeconds = 0.0f;
}

bool CoopChat::HandleNativeWindowMessage(unsigned message, uint64_t wParam, int64_t lParam)
{
    if (!m_connected)
        return false;

    const bool keyDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool keyUp = message == WM_KEYUP || message == WM_SYSKEYUP;
    const uint32_t key = static_cast<uint32_t>(wParam & 0xffffu);
    const bool chatOpenKey = key == 'Y' || key == VK_OEM_5 || key == VK_OEM_PLUS;

    // The opening key is also delivered as WM_CHAR by the native window.
    // Consume that character so it cannot leak into the first chat line.
    if (message == WM_CHAR && m_ignoreNextChar)
    {
        m_ignoreNextChar = false;
        return true;
    }

    if (!m_chatOpen && keyDown && chatOpenKey &&
        (static_cast<uint64_t>(lParam) & (1ull << 30)) == 0)
    {
        m_chatOpen = true;
        m_ignoreNextChar = true;
        m_pendingHighSurrogate = 0;
        return true;
    }

    if (!m_chatOpen)
        return false;

    // Chat owns keyboard input only. Window lifecycle, focus, mouse and other
    // native messages must still reach the game's existing handlers.
    if (!keyDown && !keyUp && message != WM_CHAR)
        return false;

    if (keyDown && chatOpenKey)
        return true;

    if (keyDown)
    {
        if (key == VK_ESCAPE)
        {
            m_chatOpen = false;
            m_input.clear();
            m_pendingHighSurrogate = 0;
            return true;
        }
        if (key == VK_RETURN)
        {
            m_ignoreNextChar = true;
            SubmitInput();
            return true;
        }
        if (key == VK_BACK || key == VK_LEFT || key == VK_RIGHT ||
            key == VK_UP || key == VK_DOWN || key == VK_HOME || key == VK_END || key == VK_DELETE)
            return true;
    }

    if (message == WM_CHAR)
    {
        if (key == VK_RETURN || key == VK_ESCAPE)
            return true;
        if (key == VK_BACK)
        {
            if (!m_input.empty())
            {
                size_t offset = m_input.size() - 1;
                while (offset > 0 && (static_cast<unsigned char>(m_input[offset]) & 0xc0u) == 0x80u)
                    --offset;
                m_input.erase(offset);
            }
            return true;
        }
        if (key >= 0x20 && key != 0x7f)
            AppendInputUtf16(static_cast<uint16_t>(key));
        return true;
    }

    return keyDown || keyUp;
}

bool CoopChat::SendText(const std::string& text)
{
    if (!m_connected || m_accountToken == 0 || !m_sendDatagram)
        return false;

    const std::string limited = LimitUtf8(text);
    if (limited.empty())
        return false;

    CoopProtocol::TextChatPacket packet = {};
    packet.sequence = CoopSerialSequence::Advance(m_textSequence);
    packet.sourceAccountToken = m_accountToken;
    packet.textLength = static_cast<uint16_t>(limited.size());
    std::memcpy(packet.text, limited.data(), limited.size());
    if (!m_sendDatagram(&packet, sizeof(packet), "chat send failed"))
        return false;

    ++m_textSent;
    AddChatLine(m_username, limited, m_lastTickSeconds);
    m_chatOpen = false;
    m_input.clear();
    m_pendingHighSurrogate = 0;
    return true;
}

bool CoopChat::HandleTextPacket(
    const CoopProtocol::TextChatPacket& packet,
    const std::string& username,
    float nowSeconds)
{
    if (!CanAcceptTextPacket(packet))
        return false;

    m_lastTextSequences[packet.sourceAccountToken] = packet.sequence;
    AddChatLine(
        username.empty() ? "Player" : username,
        std::string(packet.text, packet.text + packet.textLength),
        nowSeconds);
    ++m_textReceived;
    return true;
}

bool CoopChat::CanAcceptTextPacket(const CoopProtocol::TextChatPacket& packet) const
{
    if (packet.sourceAccountToken == 0 || packet.sequence == 0 || packet.textLength == 0 ||
        packet.textLength > CoopProtocol::kTextChatMaxUtf8Bytes)
        return false;

    const auto sequenceIt = m_lastTextSequences.find(packet.sourceAccountToken);
    if (sequenceIt != m_lastTextSequences.end() &&
        CoopSerialSequence::IsStaleOrDuplicate(packet.sequence, sequenceIt->second))
        return false;

    size_t characters = 0;
    return IsValidUtf8(packet.text, packet.textLength, &characters) &&
        characters != 0 && characters <= CoopProtocol::kTextChatMaxUtf8Chars;
}

void CoopChat::RemoveSender(uint64_t accountToken)
{
    m_lastTextSequences.erase(accountToken);
}

CoopChat::HudState CoopChat::BuildHudState(float nowSeconds) const
{
    const float currentSeconds = std::isfinite(nowSeconds) ? nowSeconds : m_lastTickSeconds;
    HudState state;
    size_t lineCount = 0;
    for (auto it = m_chatLines.rbegin(); it != m_chatLines.rend(); ++it)
    {
        const float age = std::max(0.0f, currentSeconds - it->receivedAt);
        if (age >= kChatLineLifetimeSeconds)
            continue;
        const std::string line = it->username + ": " + it->text;
        state.text = state.text.empty() ? line : line + "\n" + state.text;
        if (++lineCount >= 10)
            break;
    }

    if (m_chatOpen)
    {
        if (!state.text.empty())
            state.text += "\n";
        state.text += "CHAT > " + m_input + "_";
    }
    return state;
}

std::string CoopChat::BuildTelemetry() const
{
    std::ostringstream out;
    out << "chat=" << m_textSent << "/" << m_textReceived
        << ",typing=" << (m_chatOpen ? 1 : 0);
    return out.str();
}

void CoopChat::AppendInputUtf16(uint16_t value)
{
    if (value >= 0xd800u && value <= 0xdbffu)
    {
        if (m_pendingHighSurrogate != 0)
            m_input += WideCharToUtf8(m_pendingHighSurrogate);
        m_pendingHighSurrogate = value;
        return;
    }
    if (value >= 0xdc00u && value <= 0xdfffu)
    {
        if (m_pendingHighSurrogate != 0)
        {
            const std::string encoded =
                WideCharToUtf8(CombineSurrogate(m_pendingHighSurrogate, value));
            const std::string candidate = m_input + encoded;
            if (candidate.size() <= CoopProtocol::kTextChatMaxUtf8Bytes &&
                LimitUtf8(candidate).size() == candidate.size())
                m_input = candidate;
            m_pendingHighSurrogate = 0;
        }
        return;
    }
    if (m_pendingHighSurrogate != 0)
    {
        m_input += WideCharToUtf8(m_pendingHighSurrogate);
        m_pendingHighSurrogate = 0;
    }
    const std::string encoded = WideCharToUtf8(value);
    const std::string candidate = m_input + encoded;
    if (candidate.size() > CoopProtocol::kTextChatMaxUtf8Bytes ||
        LimitUtf8(candidate).size() != candidate.size())
        return;
    m_input = candidate;
}

void CoopChat::SubmitInput()
{
    if (!m_input.empty())
        SendText(m_input);
    else
        m_chatOpen = false;
}

void CoopChat::AddChatLine(const std::string& username, const std::string& text, float nowSeconds)
{
    if (text.empty())
        return;
    m_chatLines.push_back({username.empty() ? "Player" : username, text, nowSeconds});
    while (m_chatLines.size() > kMaxChatLines)
        m_chatLines.pop_front();
}

std::string CoopChat::LimitUtf8(const std::string& text)
{
    size_t chars = 0;
    size_t offset = 0;
    while (offset < text.size() && chars < CoopProtocol::kTextChatMaxUtf8Chars)
    {
        const unsigned char first = static_cast<unsigned char>(text[offset]);
        size_t width = 1;
        if (first >= 0xc2u && first <= 0xdfu)
            width = 2;
        else if (first >= 0xe0u && first <= 0xefu)
            width = 3;
        else if (first >= 0xf0u && first <= 0xf4u)
            width = 4;
        if (offset + width > text.size() || !IsValidUtf8(text.data() + offset, width, nullptr))
            break;
        offset += width;
        ++chars;
    }
    return text.substr(0, std::min(offset, CoopProtocol::kTextChatMaxUtf8Bytes));
}

bool CoopChat::IsValidUtf8(const char* text, size_t length, size_t* outCharacters)
{
    if (outCharacters)
        *outCharacters = 0;
    if (!text)
        return false;

    size_t offset = 0;
    size_t characters = 0;
    while (offset < length)
    {
        const unsigned char first = static_cast<unsigned char>(text[offset]);
        size_t width = 0;
        if (first <= 0x7fu)
            width = 1;
        else if (first >= 0xc2u && first <= 0xdfu)
            width = 2;
        else if (first >= 0xe0u && first <= 0xefu)
            width = 3;
        else if (first >= 0xf0u && first <= 0xf4u)
            width = 4;
        else
            return false;
        if (offset + width > length)
            return false;
        uint32_t codepoint = first & (width == 1 ? 0x7fu : width == 2 ? 0x1fu : width == 3 ? 0x0fu : 0x07u);
        for (size_t i = 1; i < width; ++i)
        {
            const unsigned char continuation = static_cast<unsigned char>(text[offset + i]);
            if ((continuation & 0xc0u) != 0x80u)
                return false;
            codepoint = (codepoint << 6) | (continuation & 0x3fu);
        }
        if ((width == 2 && codepoint < 0x80u) ||
            (width == 3 && codepoint < 0x800u) ||
            (width == 4 && codepoint < 0x10000u) ||
            codepoint > 0x10ffffu || (codepoint >= 0xd800u && codepoint <= 0xdfffu) ||
            codepoint <= 0x1fu || (codepoint >= 0x7fu && codepoint <= 0x9fu))
            return false;
        offset += width;
        ++characters;
    }
    if (outCharacters)
        *outCharacters = characters;
    return true;
}

std::string CoopChat::WideCharToUtf8(uint32_t codepoint)
{
    if (codepoint > 0x10ffffu || (codepoint >= 0xd800u && codepoint <= 0xdfffu))
        return {};
    std::string result;
    if (codepoint <= 0x7fu)
        result.push_back(static_cast<char>(codepoint));
    else if (codepoint <= 0x7ffu)
    {
        result.push_back(static_cast<char>(0xc0u | (codepoint >> 6)));
        result.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    }
    else if (codepoint <= 0xffffu)
    {
        result.push_back(static_cast<char>(0xe0u | (codepoint >> 12)));
        result.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3fu)));
        result.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    }
    else
    {
        result.push_back(static_cast<char>(0xf0u | (codepoint >> 18)));
        result.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3fu)));
        result.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3fu)));
        result.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    }
    return result;
}
