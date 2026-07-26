#include "ModMain.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Xinput.h>

#include <imgui.h>
#include <imgui_stdlib.h>

namespace
{
struct PlayerModelOption
{
    const char* label;
    uint64_t archetypeId;
};

constexpr std::array<PlayerModelOption, 6> kPlayerModels = {{
    {"Danielle Sho", 10739735956144685671ull},
    {"Morgan Yu (male)", 10739735956144685617ull},
    {"Morgan Yu (female)", 10739735956144685618ull},
    {"Sylvain Bellamy", 10739735956144685611ull},
    {"Grant Lockwood", 10739735956144685776ull},
    {"Mariana Arias", 10739735956144685771ull},
}};

bool IsValidIpv4Address(const std::string& value)
{
    unsigned octets[4] = {};
    char trailing = '\0';
    if (std::sscanf(
            value.c_str(),
            "%u.%u.%u.%u%c",
            &octets[0],
            &octets[1],
            &octets[2],
            &octets[3],
            &trailing) != 4)
    {
        return false;
    }
    return octets[0] <= 255 && octets[1] <= 255 && octets[2] <= 255 && octets[3] <= 255;
}

std::string FormatAccountToken(uint64_t token)
{
    char text[19] = {};
    std::snprintf(text, sizeof(text), "0x%016llX", static_cast<unsigned long long>(token));
    return text;
}

std::string FormatIpv4(uint32_t address)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(&address);
    char text[16] = {};
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
    return text;
}

template<typename Entry>
bool MatchesSearch(const Entry& entry, const std::string& search)
{
    if (search.empty())
        return true;
    std::string haystack = entry.serverName + " " + entry.hostUsername + " " + entry.levelName + " " + FormatIpv4(entry.address);
    std::string needle = search;
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return haystack.find(needle) != std::string::npos;
}

bool BookmarkMatches(const CoopServerBookmark& bookmark, const std::string& address, uint16_t port)
{
    return bookmark.address == address && bookmark.port == port;
}

void DrawSectionLabel(const char* label)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.48f, 0.82f, 0.86f, 1.0f));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

struct GamepadButtonEdges
{
    bool back = false;
    bool previousTab = false;
    bool nextTab = false;
};

GamepadButtonEdges PollXInputButtonEdges(ImGuiIO& io)
{
    using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    static GetStateFn getState = []() -> GetStateFn
    {
        constexpr const char* kLibraries[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
        for (const char* library : kLibraries)
        {
            if (HMODULE module = LoadLibraryA(library))
            {
                if (auto* function = GetProcAddress(module, "XInputGetState"))
                    return reinterpret_cast<GetStateFn>(function);
            }
        }
        return nullptr;
    }();
    static std::array<WORD, XUSER_MAX_COUNT> previousButtons = {};

    GamepadButtonEdges edges;
    if (!getState)
        return edges;
    WORD heldButtons = 0;
    bool hasGamepad = false;
    for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user)
    {
        XINPUT_STATE state = {};
        const bool connected = getState(user, &state) == ERROR_SUCCESS;
        const WORD buttons = connected ? state.Gamepad.wButtons : 0;
        const WORD pressed = static_cast<WORD>(buttons & ~previousButtons[user]);
        previousButtons[user] = buttons;
        hasGamepad = hasGamepad || connected;
        heldButtons = static_cast<WORD>(heldButtons | buttons);
        edges.back = edges.back || (pressed & XINPUT_GAMEPAD_B) != 0;
        edges.previousTab = edges.previousTab || (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
        edges.nextTab = edges.nextTab || (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
    }
    if (hasGamepad)
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, (heldButtons & XINPUT_GAMEPAD_A) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, (heldButtons & XINPUT_GAMEPAD_X) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceUp, (heldButtons & XINPUT_GAMEPAD_Y) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, (heldButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (heldButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, (heldButtons & XINPUT_GAMEPAD_DPAD_UP) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, (heldButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0);
    return edges;
}
}

void ModMain::DrawMultiplayerUi()
{
    if (!m_showMultiplayerUi)
    {
        if (m_multiplayerRestoreChairloaderGuiOnClose)
            CloseMultiplayerUi("multiplayer menu closed");
        else if (!m_joinOverlayActive)
            ReleaseJoinInputBlock("multiplayer menu closed");
        return;
    }

    // The native production path draws from MainUpdate while Chairloader's
    // debug GUI stays disabled. This timestamp keeps input capture tied to an
    // actually rendered multiplayer frame rather than the F1 menu state.
    m_lastChairloaderDrawTickMs = GetTickCount64();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    m_multiplayerUiMouseX = io.MousePos.x;
    m_multiplayerUiMouseY = io.MousePos.y;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left, false))
        ++m_multiplayerUiMouseClicks;
    m_multiplayerUiNavActive = io.NavActive;
    m_multiplayerUiWantCaptureMouse = io.WantCaptureMouse;
    ApplyJoinInputBlock("multiplayer menu");
    TickServerBrowser();
    if (!m_serverBrowserInitialRefresh)
    {
        m_serverBrowserInitialRefresh = true;
        RefreshServerBrowser();
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(42.0f, 28.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f, 10.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.025f, 0.03f, 0.032f, 0.985f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.055f, 0.06f, 0.062f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.30f, 0.38f, 0.39f, 0.75f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.15f, 0.16f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.34f, 0.36f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.72f, 0.46f, 0.12f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.12f, 0.20f, 0.21f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.20f, 0.33f, 0.35f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.72f, 0.46f, 0.12f, 1.0f));

    const bool visible = ImGui::Begin(
        "Prey Multiplayer##Production",
        &m_showMultiplayerUi,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    if (!visible)
    {
        ImGui::End();
        ImGui::PopStyleColor(9);
        ImGui::PopStyleVar(5);
        return;
    }

    const bool offline = m_networkMode == CoopNetworkMode::Off;
    CoopIdentityConfigData& identity = m_identityConfig.MutableData();
    const bool openingInputSuppressed = GetTickCount64() < m_multiplayerInputSuppressUntilMs;
    const GamepadButtonEdges gamepad = PollXInputButtonEdges(io);
    if (!openingInputSuppressed &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || gamepad.back))
    {
        CloseMultiplayerUi("multiplayer back action");
        ImGui::End();
        ImGui::PopStyleColor(9);
        ImGui::PopStyleVar(5);
        return;
    }
    ImGui::BeginDisabled(openingInputSuppressed);

    bool focusTabPrimaryCommand = m_multiplayerUiFocusPrimaryOnOpen;
    m_multiplayerUiFocusPrimaryOnOpen = false;
    if (!openingInputSuppressed && !ImGui::IsAnyItemActive())
    {
        const bool previousTab =
            ImGui::IsKeyPressed(ImGuiKey_Q, false) ||
            ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || gamepad.previousTab;
        const bool nextTab =
            ImGui::IsKeyPressed(ImGuiKey_E, false) ||
            ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || gamepad.nextTab;
        if (previousTab != nextTab)
        {
            m_multiplayerUiTab = (m_multiplayerUiTab + (nextTab ? 1 : 3)) % 4;
            focusTabPrimaryCommand = true;
        }
    }

    ImGui::SetWindowFontScale(1.55f);
    ImGui::TextUnformatted("MULTIPLAYER");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::SameLine(ImGui::GetWindowWidth() - 86.0f);
    if (ImGui::Button("X##close_multiplayer", ImVec2(42.0f, 34.0f)))
        CloseMultiplayerUi("multiplayer close button");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Back");

    const char* tabs[] = {"SERVER BROWSER", "HOST GAME", "PROFILE", "SESSION"};
    for (int tab = 0; tab < 4; ++tab)
    {
        if (tab > 0)
            ImGui::SameLine();
        const bool selected = m_multiplayerUiTab == tab;
        if (selected)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.63f, 0.39f, 0.10f, 1.0f));
        if (ImGui::Button(tabs[tab], ImVec2(172.0f, 38.0f)))
        {
            m_multiplayerUiTab = tab;
            focusTabPrimaryCommand = true;
        }
        if (selected)
            ImGui::PopStyleColor();
    }
    ImGui::Separator();

    const float footerHeight = 56.0f;
    ImGui::BeginChild("multiplayer_content", ImVec2(0.0f, -footerHeight), false, ImGuiWindowFlags_NoScrollbar);

    if (m_multiplayerUiTab == 0)
    {
        for (ServerBrowserEntry& entry : m_serverBrowserEntries)
            entry.favorite = false;
        const auto ensureBookmark = [&](const CoopServerBookmark& bookmark, bool favorite)
        {
            uint32_t numericAddress = 0;
            unsigned octets[4] = {};
            if (std::sscanf(bookmark.address.c_str(), "%u.%u.%u.%u", &octets[0], &octets[1], &octets[2], &octets[3]) != 4)
                return;
            auto* bytes = reinterpret_cast<unsigned char*>(&numericAddress);
            for (size_t i = 0; i < 4; ++i)
                bytes[i] = static_cast<unsigned char>(std::min(octets[i], 255u));
            auto it = std::find_if(m_serverBrowserEntries.begin(), m_serverBrowserEntries.end(), [&](const ServerBrowserEntry& entry)
            {
                return entry.address == numericAddress && entry.port == bookmark.port;
            });
            if (it == m_serverBrowserEntries.end())
            {
                ServerBrowserEntry entry;
                entry.address = numericAddress;
                entry.port = bookmark.port;
                entry.serverName = bookmark.name.empty() ? bookmark.address : bookmark.name;
                entry.levelName = "offline";
                entry.favorite = favorite;
                m_serverBrowserEntries.push_back(std::move(entry));
            }
            else if (favorite)
            {
                it->favorite = true;
            }
        };
        for (const CoopServerBookmark& bookmark : identity.favoriteServers)
            ensureBookmark(bookmark, true);
        for (const CoopServerBookmark& bookmark : identity.recentServers)
            ensureBookmark(bookmark, false);

        ImGui::SetNextItemWidth(300.0f);
        ImGui::InputTextWithHint("##server_search", "Search servers", &m_serverSearch);
        ImGui::SameLine();
        if (ImGui::Button("REFRESH", ImVec2(110.0f, 30.0f)))
            RefreshServerBrowser();
        ImGui::SameLine(0.0f, 26.0f);
        const char* filters[] = {"ALL", "LAN", "FAVORITES", "RECENT"};
        for (int filter = 0; filter < 4; ++filter)
        {
            if (filter > 0)
                ImGui::SameLine();
            const bool selected = m_serverBrowserFilter == filter;
            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.46f, 0.47f, 1.0f));
            if (ImGui::SmallButton(filters[filter]))
                m_serverBrowserFilter = filter;
            if (selected)
                ImGui::PopStyleColor();
        }

        const float detailsWidth = std::clamp(ImGui::GetContentRegionAvail().x * 0.31f, 300.0f, 420.0f);
        ImGui::BeginChild("server_list", ImVec2(-detailsWidth - 18.0f, 0.0f), true);
        DrawSectionLabel("SERVERS");
        int visibleServers = 0;
        for (size_t index = 0; index < m_serverBrowserEntries.size(); ++index)
        {
            ServerBrowserEntry& entry = m_serverBrowserEntries[index];
            const std::string address = FormatIpv4(entry.address);
            const bool recent = std::any_of(identity.recentServers.begin(), identity.recentServers.end(), [&](const CoopServerBookmark& bookmark)
            {
                return BookmarkMatches(bookmark, address, entry.port);
            });
            if (!MatchesSearch(entry, m_serverSearch) ||
                (m_serverBrowserFilter == 1 && entry.lastSeenTime < 0.0f) ||
                (m_serverBrowserFilter == 2 && !entry.favorite) ||
                (m_serverBrowserFilter == 3 && !recent))
            {
                continue;
            }
            ++visibleServers;
            const bool selected = m_selectedServerIndex == static_cast<int>(index);
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::Selectable("##server_row", selected, 0, ImVec2(0.0f, 66.0f)))
                m_selectedServerIndex = static_cast<int>(index);
            const ImVec2 rowMin = ImGui::GetItemRectMin();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddText(ImVec2(rowMin.x + 14.0f, rowMin.y + 9.0f), IM_COL32(235, 239, 236, 255), entry.serverName.empty() ? "Unnamed server" : entry.serverName.c_str());
            const std::string endpoint = address + ":" + std::to_string(entry.port);
            draw->AddText(ImVec2(rowMin.x + 14.0f, rowMin.y + 35.0f), IM_COL32(137, 177, 180, 255), endpoint.c_str());
            const std::string population = entry.maxPlayers > 0
                ? std::to_string(entry.playerCount) + "/" + std::to_string(entry.maxPlayers)
                : "OFFLINE";
            draw->AddText(ImVec2(ImGui::GetItemRectMax().x - 70.0f, rowMin.y + 10.0f), IM_COL32(218, 165, 82, 255), population.c_str());
            if (entry.passworded)
                draw->AddText(ImVec2(ImGui::GetItemRectMax().x - 70.0f, rowMin.y + 36.0f), IM_COL32(194, 204, 204, 255), "LOCKED");
            ImGui::PopID();
        }
        if (visibleServers == 0)
            ImGui::TextDisabled("No servers found");
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("server_details", ImVec2(0.0f, 0.0f), true);
        DrawSectionLabel("SERVER DETAILS");
        ServerBrowserEntry* selected = m_selectedServerIndex >= 0 && m_selectedServerIndex < static_cast<int>(m_serverBrowserEntries.size())
            ? &m_serverBrowserEntries[static_cast<size_t>(m_selectedServerIndex)]
            : nullptr;
        if (selected)
        {
            const std::string address = FormatIpv4(selected->address);
            ImGui::SetWindowFontScale(1.25f);
            ImGui::TextWrapped("%s", selected->serverName.c_str());
            ImGui::SetWindowFontScale(1.0f);
            ImGui::Text("Host  %s", selected->hostUsername.empty() ? "-" : selected->hostUsername.c_str());
            ImGui::Text("Area  %s", selected->levelName.empty() ? "-" : selected->levelName.c_str());
            ImGui::Text("Players  %u / %u", selected->playerCount, selected->maxPlayers);
            ImGui::Text("Ping  %s", selected->pingMs >= 0 ? (std::to_string(selected->pingMs) + " ms").c_str() : "-");
            ImGui::Text("Protocol  %u", CoopProtocol::kProtocolVersion);
            ImGui::Spacing();
            if (selected->passworded)
            {
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("##join_password", &m_joinPassword, ImGuiInputTextFlags_Password);
            }
            const bool serverOnline = selected->lastSeenTime >= 0.0f && selected->modBuild == CoopProtocol::kModBuild;
            ImGui::BeginDisabled(!offline || !serverOnline || (selected->maxPlayers > 0 && selected->playerCount >= selected->maxPlayers));
            if (focusTabPrimaryCommand)
                ImGui::SetKeyboardFocusHere();
            if (ImGui::Button("JOIN SERVER", ImVec2(-1.0f, 38.0f)))
            {
                m_hostAddress = address;
                m_networkPort = selected->port;
                identity.recentServers.erase(
                    std::remove_if(identity.recentServers.begin(), identity.recentServers.end(), [&](const CoopServerBookmark& bookmark)
                    {
                        return BookmarkMatches(bookmark, address, selected->port);
                    }),
                    identity.recentServers.end());
                identity.recentServers.insert(identity.recentServers.begin(), {selected->serverName, address, selected->port});
                if (identity.recentServers.size() > 16)
                    identity.recentServers.resize(16);
                SavePersistentConfig("join server browser");
                m_lastUiNetworkMode = CoopNetworkMode::Client;
                StartClient();
            }
            ImGui::EndDisabled();

            const bool favorite = std::any_of(identity.favoriteServers.begin(), identity.favoriteServers.end(), [&](const CoopServerBookmark& bookmark)
            {
                return BookmarkMatches(bookmark, address, selected->port);
            });
            if (ImGui::Button(favorite ? "REMOVE FAVORITE" : "ADD FAVORITE", ImVec2(-1.0f, 32.0f)))
            {
                identity.favoriteServers.erase(
                    std::remove_if(identity.favoriteServers.begin(), identity.favoriteServers.end(), [&](const CoopServerBookmark& bookmark)
                    {
                        return BookmarkMatches(bookmark, address, selected->port);
                    }),
                    identity.favoriteServers.end());
                if (!favorite)
                    identity.favoriteServers.push_back({selected->serverName, address, selected->port});
                selected->favorite = !favorite;
                SavePersistentConfig("server favorite");
            }
        }
        else
        {
            ImGui::TextDisabled("Select a server");
        }

        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - 165.0f));
        DrawSectionLabel("DIRECT CONNECT");
        ImGui::SetNextItemWidth(-92.0f);
        ImGui::InputText("##direct_address", &m_hostAddress);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::InputInt("##direct_port", &m_networkPort, 0, 0);
        m_networkPort = std::clamp(m_networkPort, 1, 65535);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint(
            "##direct_password",
            "Password (optional)",
            &m_joinPassword,
            ImGuiInputTextFlags_Password);
        const bool validEndpoint = IsValidIpv4Address(m_hostAddress);
        ImGui::BeginDisabled(!offline || !validEndpoint);
        if (focusTabPrimaryCommand && !selected)
            ImGui::SetKeyboardFocusHere();
        if (ImGui::Button("CONNECT", ImVec2(-1.0f, 34.0f)))
        {
            SavePersistentConfig("direct connect");
            m_lastUiNetworkMode = CoopNetworkMode::Client;
            StartClient();
        }
        ImGui::EndDisabled();
        ImGui::EndChild();
    }
    else if (m_multiplayerUiTab == 1)
    {
        const float settingsWidth = std::clamp(ImGui::GetContentRegionAvail().x * 0.55f, 480.0f, 720.0f);
        ImGui::BeginChild("host_settings", ImVec2(settingsWidth, 0.0f), true);
        DrawSectionLabel("HOST SETTINGS");
        const bool canEditHostSettings = offline || m_networkMode == CoopNetworkMode::Host;
        bool hostSettingsChanged = false;
        ImGui::BeginDisabled(!canEditHostSettings);
        ImGui::SetNextItemWidth(-1.0f);
        hostSettingsChanged |= ImGui::InputText("Server name", &m_serverName);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::BeginDisabled(!offline);
        hostSettingsChanged |= ImGui::InputInt("Port", &m_networkPort, 0, 0);
        ImGui::EndDisabled();
        m_networkPort = std::clamp(m_networkPort, 1, 65535);
        const int minimumPlayers = m_networkMode == CoopNetworkMode::Host
            ? std::clamp(static_cast<int>(m_remotePeers.size()) + 1, 2, 16)
            : 2;
        if (m_maxSessionPlayers < minimumPlayers)
        {
            m_maxSessionPlayers = minimumPlayers;
            hostSettingsChanged = true;
        }
        hostSettingsChanged |= ImGui::SliderInt("Max players", &m_maxSessionPlayers, minimumPlayers, 16);
        hostSettingsChanged |= ImGui::Checkbox("Visible on LAN", &m_serverLanVisible);
        const char* accessModes[] = {"OPEN", "PASSWORD", "ALLOWLIST", "FRIENDS"};
        ImGui::TextUnformatted("Access");
        for (int mode = 0; mode < 4; ++mode)
        {
            if (mode > 0)
                ImGui::SameLine();
            const bool selected = m_serverAccessMode == mode;
            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.46f, 0.47f, 1.0f));
            ImGui::BeginDisabled(mode == 3);
            if (ImGui::SmallButton(accessModes[mode]))
            {
                m_serverAccessMode = mode;
                hostSettingsChanged = true;
            }
            ImGui::EndDisabled();
            if (selected)
                ImGui::PopStyleColor();
        }
        if (m_serverAccessMode == 1)
        {
            ImGui::SetNextItemWidth(-1.0f);
            hostSettingsChanged |= ImGui::InputText("Password", &m_serverPassword, ImGuiInputTextFlags_Password);
        }
        else if (m_serverAccessMode == 2)
        {
            ImGui::SetNextItemWidth(-1.0f);
            hostSettingsChanged |= ImGui::InputText("Player account tokens", &m_serverAllowlist);
        }
        else if (m_serverAccessMode == 3)
        {
            ImGui::TextColored(ImVec4(0.92f, 0.62f, 0.24f, 1.0f), "Platform friends provider unavailable");
        }
        bool friendlyFire = identity.friendlyFire;
        if (ImGui::Checkbox("Friendly fire", &friendlyFire))
        {
            identity.friendlyFire = friendlyFire;
            hostSettingsChanged = true;
        }
        ImGui::EndDisabled();
        if (hostSettingsChanged)
            SavePersistentConfig(m_networkMode == CoopNetworkMode::Host ? "live host settings" : "host settings");

        ImGui::Spacing();
        if (focusTabPrimaryCommand)
            ImGui::SetKeyboardFocusHere();
        if (offline)
        {
            ImGui::BeginDisabled(m_serverAccessMode == 3 || (m_serverAccessMode == 1 && m_serverPassword.empty()) ||
                (m_serverAccessMode == 2 && m_serverAllowlist.empty()));
            if (ImGui::Button("START HOSTING##host_primary", ImVec2(220.0f, 42.0f)))
            {
                SavePersistentConfig("host game");
                m_lastUiNetworkMode = CoopNetworkMode::Host;
                StartHost();
            }
            ImGui::EndDisabled();
        }
        else if (m_networkMode == CoopNetworkMode::Host && ImGui::Button("STOP HOSTING##host_primary", ImVec2(220.0f, 42.0f)))
        {
            StopNetwork();
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("host_roster", ImVec2(0.0f, 0.0f), true);
        DrawSectionLabel("SESSION ROSTER");
        ImGui::TextColored(ImVec4(0.44f, 0.86f, 0.58f, 1.0f), "%s", GetLocalUsername().c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("HOST");
        for (const auto& entry : m_remotePeers)
        {
            const RemotePeerSession& peer = entry.second;
            ImGui::PushID(static_cast<int>(peer.accountToken ^ (peer.accountToken >> 32)));
            ImGui::Separator();
            ImGui::Text("%s", peer.username.empty() ? "Coop Player" : peer.username.c_str());
            if (m_networkMode == CoopNetworkMode::Host)
            {
                ImGui::SameLine(ImGui::GetWindowWidth() - 82.0f);
                ImGui::BeginDisabled(m_multiplayerKickRequestedToken != 0);
                if (ImGui::SmallButton("KICK"))
                    m_multiplayerKickRequestedToken = peer.accountToken;
                ImGui::EndDisabled();
            }
            ImGui::TextDisabled("%s", FormatAccountToken(peer.accountToken).c_str());
            ImGui::TextDisabled("%s", peer.levelName.empty() ? "-" : peer.levelName.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    else if (m_multiplayerUiTab == 2)
    {
        EnsurePlayerPortraitTextures();
        ImGui::BeginChild("profile_identity", ImVec2(410.0f, 0.0f), true);
        DrawSectionLabel("PLAYER PROFILE");
        const ImVec2 portraitMin = ImGui::GetCursorScreenPos();
        const ImVec2 portraitMax(portraitMin.x + 128.0f, portraitMin.y + 128.0f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        size_t selectedPortrait = 0;
        for (size_t index = 0; index < kPlayerModels.size(); ++index)
        {
            if (identity.selectedModelArchetypeId == kPlayerModels[index].archetypeId)
                selectedPortrait = index;
        }
        if (ITexture* portrait = GetPlayerPortraitTexture(selectedPortrait))
            draw->AddImage(reinterpret_cast<ImTextureID>(portrait), portraitMin, portraitMax);
        else
            draw->AddRectFilled(portraitMin, portraitMax, IM_COL32(25, 43, 45, 255), 2.0f);
        draw->AddRect(portraitMin, portraitMax, IM_COL32(103, 179, 183, 255), 2.0f, 0, 2.0f);
        ImGui::Dummy(ImVec2(128.0f, 136.0f));
        ImGui::BeginDisabled(!offline);
        ImGui::TextDisabled("USERNAME");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##profile_username", &m_localUsername))
            m_localUsername = SanitizeUsername(m_localUsername);
        const char* strategies[] = {"Generated UUID", "Platform account", "Custom profile"};
        int strategy = identity.accountStrategy == "steam" ? 1 : (identity.accountStrategy == "custom" ? 2 : 0);
        ImGui::TextDisabled("ACCOUNT MODE");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("##profile_account_mode", &strategy, strategies, 3))
        {
            if (strategy == 1 && !m_detectedPlatformAccountId.empty())
            {
                identity.accountStrategy = "steam";
                identity.accountId = m_detectedPlatformAccountId;
            }
            else if (strategy == 2)
            {
                identity.accountStrategy = "custom";
            }
            else
            {
                identity.accountStrategy = "generated_uuid";
                const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
                identity.accountId = "local:" + std::to_string(static_cast<unsigned long long>(stamp));
            }
        }
        ImGui::TextDisabled("PROFILE ID");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::BeginDisabled(strategy != 2);
        ImGui::InputText("##profile_id", &identity.accountId);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::TextDisabled("%s", FormatAccountToken(GetLocalAccountToken()).c_str());
        if (!m_detectedPlatformAccountId.empty())
            ImGui::TextDisabled("Platform detected");
        ImGui::BeginDisabled(!offline);
        if (ImGui::Button("SAVE PROFILE", ImVec2(-1.0f, 36.0f)))
            SavePersistentConfig("profile save");
        ImGui::EndDisabled();
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("profile_models", ImVec2(0.0f, 0.0f), true);
        DrawSectionLabel("PLAYER MODEL");
        int selectedModel = 0;
        for (size_t index = 0; index < kPlayerModels.size(); ++index)
        {
            if (identity.selectedModelArchetypeId == kPlayerModels[index].archetypeId)
                selectedModel = static_cast<int>(index);
        }
        const float modelWidth = std::max(150.0f, (ImGui::GetContentRegionAvail().x - 18.0f) * 0.5f);
        for (int index = 0; index < static_cast<int>(kPlayerModels.size()); ++index)
        {
            if ((index % 2) != 0)
                ImGui::SameLine();
            ImGui::PushID(index);
            const bool selected = selectedModel == index;
            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.63f, 0.39f, 0.10f, 1.0f));
            ImGui::BeginDisabled(!offline);
            if (ImGui::Button(kPlayerModels[static_cast<size_t>(index)].label, ImVec2(modelWidth, 118.0f)))
            {
                identity.selectedModelArchetypeId = kPlayerModels[static_cast<size_t>(index)].archetypeId;
                SavePersistentConfig("player model selection");
            }
            ImGui::EndDisabled();
            if (selected)
                ImGui::PopStyleColor();
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            const ImVec2 imageMin(min.x + 8.0f, min.y + 8.0f);
            const ImVec2 imageMax(min.x + 96.0f, max.y - 8.0f);
            if (ITexture* portrait = GetPlayerPortraitTexture(static_cast<size_t>(index)))
                ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(portrait), imageMin, imageMax);
            else
                ImGui::GetWindowDrawList()->AddRectFilled(imageMin, imageMax, IM_COL32(31 + index * 12, 45, 48 + index * 8, 255), 2.0f);
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    else
    {
        ImGui::BeginChild("session_summary", ImVec2(430.0f, 0.0f), true);
        DrawSectionLabel("CURRENT SESSION");
        ImGui::SetWindowFontScale(1.25f);
        ImGui::TextUnformatted(m_networkMode == CoopNetworkMode::Host
            ? "HOSTING"
            : (m_networkMode == CoopNetworkMode::Client ? "CONNECTED" : "OFFLINE"));
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Spacing();
        ImGui::Text("Status  %s", m_sessionStatus.c_str());
        ImGui::Text("Area  %s", m_localLevelName.empty() ? "-" : m_localLevelName.c_str());
        ImGui::Text("Endpoint  %s:%d", m_networkMode == CoopNetworkMode::Host ? "0.0.0.0" : m_hostAddress.c_str(), m_networkPort);
        ImGui::Text("Protocol  %u", CoopProtocol::kProtocolVersion);
        ImGui::Text("Build  %u", CoopProtocol::kModBuild);
        ImGui::Spacing();
        if (m_networkMode != CoopNetworkMode::Off)
        {
            if (focusTabPrimaryCommand)
                ImGui::SetKeyboardFocusHere();
            if (ImGui::Button("LEAVE SESSION", ImVec2(-1.0f, 42.0f)))
                m_multiplayerLeaveRequested = true;
        }
        else
        {
            ImGui::TextDisabled("No active multiplayer session");
            if (focusTabPrimaryCommand && m_lastUiNetworkMode == CoopNetworkMode::Client)
                ImGui::SetKeyboardFocusHere();
            if (m_lastUiNetworkMode == CoopNetworkMode::Client &&
                ImGui::Button("RECONNECT", ImVec2(-1.0f, 38.0f)))
            {
                StartClient();
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("session_roster", ImVec2(0.0f, 0.0f), true);
        DrawSectionLabel("PLAYERS");
        ImGui::TextColored(ImVec4(0.44f, 0.86f, 0.58f, 1.0f), "%s", GetLocalUsername().c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("YOU%s", m_networkMode == CoopNetworkMode::Host ? " / HOST" : "");
        if (m_remotePeers.empty() && m_hasRemoteSession)
        {
            ImGui::Separator();
            ImGui::Text("%s", m_lastRemoteUsername.empty() ? "Coop Player" : m_lastRemoteUsername.c_str());
            ImGui::TextDisabled("%s", m_remoteLevelName.empty() ? "-" : m_remoteLevelName.c_str());
        }
        for (const auto& entry : m_remotePeers)
        {
            const RemotePeerSession& peer = entry.second;
            ImGui::PushID(static_cast<int>(peer.accountToken ^ (peer.accountToken >> 32)));
            ImGui::Separator();
            ImGui::Text("%s", peer.username.empty() ? "Coop Player" : peer.username.c_str());
            if (m_networkMode == CoopNetworkMode::Host)
            {
                ImGui::SameLine(ImGui::GetWindowWidth() - 82.0f);
                ImGui::BeginDisabled(m_multiplayerKickRequestedToken != 0);
                if (ImGui::SmallButton("KICK"))
                    m_multiplayerKickRequestedToken = peer.accountToken;
                ImGui::EndDisabled();
            }
            ImGui::TextDisabled("%s", FormatAccountToken(peer.accountToken).c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", peer.levelName.empty() ? "-" : peer.levelName.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    ImGui::EndChild();

    ImGui::EndDisabled();

    const bool errorState = m_duplicateAccountRejected ||
        m_networkStatus.find("failed") != std::string::npos ||
        m_networkStatus.find("invalid") != std::string::npos ||
        m_sessionStatus.find("rejected") != std::string::npos ||
        m_sessionStatus.find("duplicate") != std::string::npos;
    const ImVec4 statusColor = errorState
        ? ImVec4(0.95f, 0.35f, 0.30f, 1.0f)
        : (m_sessionGameplayReady ? ImVec4(0.35f, 0.85f, 0.55f, 1.0f) : ImVec4(0.72f, 0.76f, 0.76f, 1.0f));
    ImGui::TextColored(statusColor, "%s", m_sessionStatus.c_str());
    ImGui::SameLine(ImGui::GetWindowWidth() - 300.0f);
    ImGui::TextDisabled("PROTOCOL %u  BUILD %u", CoopProtocol::kProtocolVersion, CoopProtocol::kModBuild);

    if (m_networkMode == CoopNetworkMode::Client && !m_sessionGameplayReady)
    {
        ImGui::OpenPopup("JOINING SESSION");
        ImGui::SetNextWindowSize(ImVec2(460.0f, 210.0f), ImGuiCond_Always);
        if (ImGui::BeginPopupModal("JOINING SESSION", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
        {
            ImGui::SetWindowFontScale(1.25f);
            ImGui::TextUnformatted("CONNECTING");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::ProgressBar(-0.25f * static_cast<float>(ImGui::GetFrameCount() % 8), ImVec2(-1.0f, 8.0f), "");
            ImGui::TextWrapped("%s", m_sessionStatus.c_str());
            ImGui::TextDisabled("%s:%d", m_hostAddress.c_str(), m_networkPort);
            if (ImGui::Button("CANCEL", ImVec2(-1.0f, 34.0f)))
            {
                StopNetwork();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    m_multiplayerUiMouseHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow);
    ImGui::End();
    ImGui::PopStyleColor(9);
    ImGui::PopStyleVar(5);

    if (!m_showMultiplayerUi)
        CloseMultiplayerUi("multiplayer menu closed");
}
