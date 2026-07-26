#include "ModMain.h"
#include "CoopRuntimeLog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

#include <imgui.h>
#include <Prey/CryAction/FlashUI/FlashUI.h>
#include <Prey/Cry3DEngine/I3DEngine.h>
#include <Prey/CryGame/Game.h>
#include <Prey/CryRenderer/IRenderAuxGeom.h>
#include <Prey/CryRenderer/IRenderer.h>
#include <Prey/GameDll/ark/player/ArkPlayer.h>
#include <Prey/GameDll/ark/player/ArkPlayerInput.h>
#include <Prey/GameDll/ark/ui/arkuihud.h>

namespace
{
constexpr float kRemoteReviveDistanceMeters = 2.8f;
constexpr float kDownedFeedbackDurationSeconds = 3.8f;
constexpr float kDownedFeedbackRefreshSeconds = 3.0f;
constexpr float kSystemFeedbackMinDurationSeconds = 0.25f;
constexpr float kTimeoutFeedbackDurationSeconds = 1.2f;
constexpr float kTimeoutFeedbackRefreshSeconds = 0.75f;
constexpr float kPeerTimeoutCountdownSeconds = 300.0f;
constexpr float kJoinOverlayMaxSeconds = 180.0f;
constexpr float kJoinOverlayNoProgressFallbackSeconds = 110.0f;

struct PostFxCandidate
{
    const char* label;
    const char* paramName;
    float defaultValue;
    float testValue;
};

constexpr PostFxCandidate kPostFxCandidates[] = {
    { "Global saturation", "Global_User_Saturation", 1.0f, 0.0f },
    { "ColorGrading saturation", "ColorGrading_Saturation", 1.0f, 0.0f },
    { "Global brightness", "Global_User_Brightness", 1.0f, 0.85f },
    { "Global contrast", "Global_User_Contrast", 1.0f, 1.15f },
    { "Filter blur amount", "FilterBlurring_Amount", 0.0f, 0.45f },
    { "Filter blur type", "FilterBlurring_Type", 0.0f, 0.0f },
    { "Chroma shift", "FilterArtifacts_ChromaShift", 0.0f, 0.08f },
    { "Filter grain", "FilterGrain_Amount", 0.0f, 1.0f },
    { "Ark vignette falloff", "ArkVignette_Falloff", 0.2f, 1.0f },
    { "Distance desat active", "ArkDistanceDesaturation_Active", 0.0f, 1.0f },
    { "Distance desat amount", "ArkDistanceDesaturation_Amount", 0.0f, 1.0f },
    { "Alien glitch", "ArkPostAliens_Glitch", 0.0f, 0.35f },
    { "Alien glitch distance", "ArkPostAliens_GlitchDistance", 0.0f, 0.35f },
    { "Visual interlacing", "VisualArtifacts_Interlacing", 0.0f, 0.5f },
    { "Visual vsync", "VisualArtifacts_Vsync", 0.0f, 1.0f },
};

float GetHudNowSeconds()
{
    return gEnv && gEnv->pTimer ? gEnv->pTimer->GetAsyncCurTime() : 0.0f;
}

uint32_t ClampTransferBytes(uint32_t chunks, uint32_t totalBytes, uint32_t chunkSize)
{
    if (totalBytes == 0 || chunkSize == 0)
        return 0;

    const uint64_t bytes = static_cast<uint64_t>(chunks) * static_cast<uint64_t>(chunkSize);
    return static_cast<uint32_t>(std::min<uint64_t>(bytes, totalBytes));
}
}

void ModMain::OnArkUIHUDUpdateHook(CArkUIHUD* hud)
{
    ++m_uiHudUpdateHookCalls;
    if (hud)
    {
        m_lastArkUIHUD = hud;
        m_lastArkUIHUDPointer = reinterpret_cast<std::uintptr_t>(hud);
    }
}

void ModMain::OnArkUIHUDPreRenderHook(CArkUIHUD* hud)
{
    ++m_uiHudPreRenderHookCalls;
    if (hud)
    {
        m_lastArkUIHUD = hud;
        m_lastArkUIHUDPointer = reinterpret_cast<std::uintptr_t>(hud);
    }
}

void ModMain::RefreshUiLayerDebug()
{
    m_uiDebugRows.clear();
    m_uiLayerElementCount = 0;
    m_uiLayerSortedCount = 0;
    m_uiLayerVisibleCount = 0;
    m_uiLayerHudFlagCount = 0;
    m_flashUIPointer = gEnv && gEnv->pFlashUI ? reinterpret_cast<std::uintptr_t>(gEnv->pFlashUI) : 0;

    IUIElement* hudElement = CArkUIHUD::GetHUDUIElement();
    IUIElement* markerElement = CArkUIHUD::GetMarkerUIElement();
    m_hudUIElementPointer = reinterpret_cast<std::uintptr_t>(hudElement);
    m_markerUIElementPointer = reinterpret_cast<std::uintptr_t>(markerElement);

    if (!gEnv || !gEnv->pFlashUI)
    {
        m_uiDebugStatus = "no gEnv->pFlashUI";
        return;
    }

    CFlashUI* flashUI = reinterpret_cast<CFlashUI*>(gEnv->pFlashUI);
    CFlashUI::FUpdateSortedElements(flashUI);
    m_uiLayerElementCount = static_cast<uint32_t>(std::max(0, CFlashUI::FGetUIElementCount(flashUI)));

    const CFlashUI::TSortedElementList& sortedElements = CFlashUI::FGetSortedElements(flashUI);
    m_uiLayerSortedCount = static_cast<uint32_t>(sortedElements.size());

    size_t shown = 0;
    for (const auto& entry : sortedElements)
    {
        if (shown >= 80)
            break;

        IUIElement* element = entry.second;
        if (!element)
            continue;

        const bool visible = element->IsVisible();
        const bool isHud = element->HasFlag(IUIElement::eFUI_IS_HUD);
        if (visible)
            ++m_uiLayerVisibleCount;
        if (isHud)
            ++m_uiLayerHudFlagCount;

        int viewportX = 0;
        int viewportY = 0;
        int viewportW = 0;
        int viewportH = 0;
        float viewportScale = 0.0f;
        element->GetViewPort(viewportX, viewportY, viewportW, viewportH, viewportScale);
        const IUIElement::SUIConstraints& constraints = element->GetConstraints();

        char row[768];
        std::snprintf(row,
            sizeof(row),
            "#%02zu layer=%d ptr=%p name=%s group=%s flash=%s visible=%d init=%d valid=%d alpha=%.2f hud=%d lazy=%d vp=%d,%d %dx%d scale=%.2f consType=%d pos=%d,%d size=%dx%d",
            shown,
            entry.first,
            static_cast<void*>(element),
            element->GetName() ? element->GetName() : "-",
            element->GetGroupName() ? element->GetGroupName() : "-",
            element->GetFlashFile() ? element->GetFlashFile() : "-",
            visible ? 1 : 0,
            element->IsInit() ? 1 : 0,
            element->IsValid() ? 1 : 0,
            element->GetAlpha(),
            isHud ? 1 : 0,
            element->NeedLazyRender() ? 1 : 0,
            viewportX,
            viewportY,
            viewportW,
            viewportH,
            viewportScale,
            static_cast<int>(constraints.eType),
            constraints.iLeft,
            constraints.iTop,
            constraints.iWidth,
            constraints.iHeight);
        m_uiDebugRows.emplace_back(row);
        ++shown;
    }

    m_uiDebugStatus = "scanned " + std::to_string(m_uiLayerSortedCount) + " sorted UI elements";
}

void ModMain::TriggerHudFeedbackTest()
{
    if (!m_lastArkUIHUD)
    {
        m_uiDebugStatus = "cannot DisplayFeedback: no CArkUIHUD hook instance captured";
        return;
    }

    string message = "COOP HUD FEEDBACK TEST";
    m_lastArkUIHUD->DisplayFeedback(message, 4.0f);
    m_uiDebugStatus = "called CArkUIHUD::DisplayFeedback";
}

void ModMain::AddHudTargetTest()
{
    if (!m_lastArkUIHUD)
    {
        m_uiDebugStatus = "cannot AddTarget: no CArkUIHUD hook instance captured";
        return;
    }

    if (m_uiDebugTargetId >= 0)
        RemoveHudTargetTest();

    wstring text(L"COOP TARGET TEST");
    m_uiDebugTargetId = m_lastArkUIHUD->AddTarget(text, Vec2(0.5f, 0.35f), true);
    m_uiDebugStatus = "called CArkUIHUD::AddTarget id " + std::to_string(m_uiDebugTargetId);
}

void ModMain::RemoveHudTargetTest()
{
    if (m_lastArkUIHUD && m_uiDebugTargetId >= 0)
        m_lastArkUIHUD->RemoveTarget(m_uiDebugTargetId);

    m_uiDebugTargetId = -1;
    m_uiDebugStatus = "removed HUD target test";
}

void ModMain::QueueCoopHudFeedback(const std::string& message, float durationSeconds)
{
    if (message.empty())
        return;

    m_queuedHudFeedbackMessage = message;
    m_queuedHudFeedbackDuration = std::max(kSystemFeedbackMinDurationSeconds, durationSeconds);
    m_hasQueuedHudFeedback = true;
}

void ModMain::DrawUiLayerDebug()
{
    ImGui::Separator();
    if (!ImGui::CollapsingHeader("UI Layer Debug"))
        return;

    ImGui::Text("Status: %s", m_uiDebugStatus.c_str());
    ImGui::Text("Hooks: CArkUIHUD OnUpdate=%u OnPreRender=%u lastHud=0x%llx",
        m_uiHudUpdateHookCalls,
        m_uiHudPreRenderHookCalls,
        static_cast<unsigned long long>(m_lastArkUIHUDPointer));
    ImGui::Text("Coop HUD draw: renderRegistered=%d endFrame=%u mainUpdate=%u imgui=%u",
        m_coopRenderListenerRegistered ? 1 : 0,
        m_coopRenderEndFrameCalls,
        m_coopMainUpdateHudDrawCalls,
        m_coopMainUpdateImGuiDrawCalls);
    ImGui::Text("FlashUI=0x%llx HUDElement=0x%llx MarkerElement=0x%llx",
        static_cast<unsigned long long>(m_flashUIPointer),
        static_cast<unsigned long long>(m_hudUIElementPointer),
        static_cast<unsigned long long>(m_markerUIElementPointer));
    ImGui::Text("Elements: total=%u sorted=%u visible=%u hudFlag=%u targetTestId=%d",
        m_uiLayerElementCount,
        m_uiLayerSortedCount,
        m_uiLayerVisibleCount,
        m_uiLayerHudFlagCount,
        m_uiDebugTargetId);

    if (ImGui::Button("Refresh UI layers"))
        RefreshUiLayerDebug();
    ImGui::SameLine();
    if (ImGui::Button("HUD feedback test"))
        TriggerHudFeedbackTest();
    ImGui::SameLine();
    if (ImGui::Button("HUD target test"))
        AddHudTargetTest();
    ImGui::SameLine();
    if (ImGui::Button("Remove HUD target"))
        RemoveHudTargetTest();

    ImGui::TextWrapped("Use this block to identify the active Flash UI layers before adding a real coop overlay. Rows are captured only when Refresh is clicked.");
    for (const std::string& row : m_uiDebugRows)
        ImGui::TextUnformatted(row.c_str());
}

bool ModMain::IsPostFxParamAvailable(const char* paramName) const
{
    if (!paramName || !paramName[0])
        return false;

    if (gEnv && gEnv->pRenderer && gEnv->pRenderer->EF_IsPostEffectParam(paramName))
        return true;

    if (gEnv && gEnv->p3DEngine && gEnv->p3DEngine->IsPostEffectParam(paramName))
        return true;

    return false;
}

void ModMain::ApplyPostFxParam(const char* paramName, float value, bool force)
{
    if (!paramName || !paramName[0] || !gEnv || !gEnv->p3DEngine)
        return;
    if (!IsPostFxParamAvailable(paramName))
        return;

    gEnv->p3DEngine->SetPostEffectParam(paramName, value, force);
}

void ModMain::ApplyPostFxParamVec4(const char* paramName, const Vec4& value, bool force)
{
    if (!paramName || !paramName[0] || !gEnv || !gEnv->p3DEngine)
        return;
    if (!IsPostFxParamAvailable(paramName))
        return;

    gEnv->p3DEngine->SetPostEffectParamVec4(paramName, value, force);
}

void ModMain::ResetDownedPostFx()
{
    ApplyPostFxParam("Global_User_Saturation", 1.0f, true);
    ApplyPostFxParam("ColorGrading_Saturation", 1.0f, true);
    ApplyPostFxParam("Global_User_Brightness", 1.0f, true);
    ApplyPostFxParam("Global_User_Contrast", 1.0f, true);
    ApplyPostFxParam("FilterBlurring_Amount", 0.0f, true);
    ApplyPostFxParam("FilterArtifacts_ChromaShift", 0.0f, true);
    ApplyPostFxParam("ArkVignette_Falloff", 0.2f, true);
    ApplyPostFxParamVec4("ArkVignette_Border", Vec4(0.0f, 0.0f, 0.0f, 0.0f), true);
    ApplyPostFxParamVec4("ArkVignette_Color", Vec4(0.0f, 0.0f, 0.0f, 1.0f), true);
    m_downedPostFxApplied = false;
    m_postFxDebugStatus = "reset downed PostFX params";
}

void ModMain::ApplyDownedPostFx(bool active)
{
    if (!active)
    {
        if (m_downedPostFxApplied)
            ResetDownedPostFx();
        return;
    }

    ApplyPostFxParam("Global_User_Saturation", m_downedPostFxSaturation, true);
    ApplyPostFxParam("ColorGrading_Saturation", m_downedPostFxSaturation, true);
    ApplyPostFxParam("Global_User_Brightness", m_downedPostFxBrightness, true);
    ApplyPostFxParam("Global_User_Contrast", m_downedPostFxContrast, true);
    ApplyPostFxParam("FilterBlurring_Amount", m_downedPostFxBlur, true);
    ApplyPostFxParam("FilterArtifacts_ChromaShift", m_downedPostFxChroma, true);
    ApplyPostFxParam("ArkVignette_Falloff", 1.0f, true);
    ApplyPostFxParamVec4("ArkVignette_Border",
        Vec4(m_downedPostFxVignetteBorder, m_downedPostFxVignetteBorder, 0.60f, 0.50f),
        true);
    ApplyPostFxParamVec4("ArkVignette_Color", Vec4(0.0f, 0.0f, 0.0f, 1.0f), true);
    m_downedPostFxApplied = true;
}

void ModMain::TickDownedPostFx()
{
    const bool active = m_enableDownedPostFx &&
        m_downedModeEnabled &&
        (m_localPlayerDowned || m_previewDownedPostFx);
    ApplyDownedPostFx(active);
}

void ModMain::RefreshPostFxDebug()
{
    m_postFxDebugRows.clear();
    uint32_t availableCount = 0;
    for (size_t i = 0; i < sizeof(kPostFxCandidates) / sizeof(kPostFxCandidates[0]); ++i)
    {
        const PostFxCandidate& candidate = kPostFxCandidates[i];
        const bool available = IsPostFxParamAvailable(candidate.paramName);
        if (available)
            ++availableCount;

        float current = 0.0f;
        if (available && gEnv && gEnv->p3DEngine)
            gEnv->p3DEngine->GetPostEffectParam(candidate.paramName, current);

        char row[384];
        std::snprintf(row,
            sizeof(row),
            "#%02zu available=%d current=%.3f default=%.3f test=%.3f %s (%s)",
            i,
            available ? 1 : 0,
            current,
            candidate.defaultValue,
            candidate.testValue,
            candidate.label,
            candidate.paramName);
        m_postFxDebugRows.emplace_back(row);
    }

    m_postFxDebugStatus = "scanned " + std::to_string(availableCount) + "/" +
        std::to_string(sizeof(kPostFxCandidates) / sizeof(kPostFxCandidates[0])) +
        " candidate params";
}

void ModMain::DrawPostFxDebug()
{
    ImGui::Separator();
    if (!ImGui::CollapsingHeader("PostFX Debug"))
        return;

    ImGui::Text("Status: %s", m_postFxDebugStatus.c_str());
    ImGui::Checkbox("Enable downed PostFX", &m_enableDownedPostFx);
    ImGui::SameLine();
    ImGui::Checkbox("Preview downed PostFX", &m_previewDownedPostFx);
    ImGui::Text("Applied: %s", m_downedPostFxApplied ? "yes" : "no");
    ImGui::SliderFloat("Downed saturation", &m_downedPostFxSaturation, -1.5f, 1.0f);
    ImGui::SliderFloat("Downed brightness", &m_downedPostFxBrightness, 0.25f, 2.0f);
    ImGui::SliderFloat("Downed contrast", &m_downedPostFxContrast, 0.25f, 3.0f);
    ImGui::SliderFloat("Downed blur", &m_downedPostFxBlur, 0.0f, 2.0f);
    ImGui::SliderFloat("Downed chroma", &m_downedPostFxChroma, 0.0f, 0.75f);
    ImGui::SliderFloat("Downed vignette", &m_downedPostFxVignetteBorder, 0.0f, 1.5f);

    int maxCandidate = static_cast<int>(sizeof(kPostFxCandidates) / sizeof(kPostFxCandidates[0])) - 1;
    ImGui::SliderInt("Candidate", &m_postFxSelectedCandidate, 0, maxCandidate);
    const PostFxCandidate& candidate = kPostFxCandidates[std::clamp(m_postFxSelectedCandidate, 0, maxCandidate)];
    ImGui::Text("Selected: %s (%s)", candidate.label, candidate.paramName);
    if (ImGui::Button("Refresh PostFX params"))
        RefreshPostFxDebug();
    ImGui::SameLine();
    if (ImGui::Button("Apply selected test"))
    {
        ApplyPostFxParam(candidate.paramName, candidate.testValue, true);
        m_postFxDebugStatus = std::string("applied test ") + candidate.paramName + "=" + std::to_string(candidate.testValue);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset selected"))
    {
        ApplyPostFxParam(candidate.paramName, candidate.defaultValue, true);
        m_postFxDebugStatus = std::string("reset ") + candidate.paramName + "=" + std::to_string(candidate.defaultValue);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset downed PostFX"))
        ResetDownedPostFx();

    ImGui::TextWrapped("Confirmed useful params from ArkPostEffectParams.xml: Global_User_Saturation, ColorGrading_Saturation, FilterBlurring_Amount, FilterArtifacts_ChromaShift, ArkVignette_*.");
    for (const std::string& row : m_postFxDebugRows)
        ImGui::TextUnformatted(row.c_str());
}

void ModMain::DrawRemoteNameplate()
{
    if (!m_showRemoteNameplate || !IsSessionGameplayReady())
        return;

    IEntity* proxyEntity = GetProxyEntity();
    if (!proxyEntity || !gEnv || !gEnv->pRenderer || !gEnv->pAuxGeomRenderer)
        return;

    Vec3 labelPosition = proxyEntity->GetWorldPos() + Vec3(0.0f, 0.0f, 1.9f);
    float screenX = 0.0f;
    float screenY = 0.0f;
    float screenZ = 0.0f;
    if (!gEnv->pRenderer->ProjectToScreen(labelPosition.x, labelPosition.y, labelPosition.z, &screenX, &screenY, &screenZ))
        return;

    if (screenZ < 0.0f || screenZ > 1.0f)
        return;

    float labelX = screenX;
    float labelY = screenY;
    if (screenX >= -10.0f && screenX <= 110.0f && screenY >= -10.0f && screenY <= 110.0f)
    {
        labelX = screenX * 8.0f;
        labelY = screenY * 6.0f;
    }
    else
    {
        const int width = std::max(1, gEnv->pRenderer->GetOverlayWidth());
        const int height = std::max(1, gEnv->pRenderer->GetOverlayHeight());
        labelX = screenX * (800.0f / static_cast<float>(width));
        labelY = screenY * (600.0f / static_cast<float>(height));
    }

    if (labelX < -40.0f || labelX > 840.0f || labelY < -40.0f || labelY > 640.0f)
        return;

    std::string label = GetRemoteUsernameOrFallback();
    if (m_proxyHealthAvailable && m_lastProxyMaxHealth > 0.0f)
        label += "  " + std::to_string(static_cast<int>(m_lastProxyHealth)) + "/" + std::to_string(static_cast<int>(m_lastProxyMaxHealth));

    std::string promptLabel;
    std::string progressLabel;
    if (m_remotePlayerDowned)
    {
        if (m_remoteRevivePromptActive)
        {
            const int percent = static_cast<int>(m_remoteReviveHoldProgress * 100.0f + 0.5f);
            promptLabel = "[F] Revive " + std::to_string(percent) + "%";

            constexpr int kReviveBarSegments = 14;
            const int filledSegments = std::clamp(
                static_cast<int>(m_remoteReviveHoldProgress * static_cast<float>(kReviveBarSegments) + 0.5f),
                0,
                kReviveBarSegments);
            progressLabel = "REVIVE [";
            for (int i = 0; i < kReviveBarSegments; ++i)
                progressLabel += i < filledSegments ? '=' : '-';
            progressLabel += "] " + std::to_string(percent) + "%";
        }
        else if (m_remoteReviveDistance > 0.0f && m_remoteReviveDistance <= kRemoteReviveDistanceMeters + 1.5f)
        {
            promptLabel = "[F] Revive";
        }
        else
        {
            promptLabel = "DOWNED";
        }
    }

    const float color[4] = {
        m_remotePlayerDowned ? 1.0f : 0.35f,
        m_remotePlayerDowned ? 0.78f : 0.9f,
        m_remotePlayerDowned ? 0.25f : 1.0f,
        1.0f
    };
    const float promptColor[4] = { 0.95f, 0.95f, 1.0f, 1.0f };
    gEnv->pAuxGeomRenderer->Draw2dLabel(labelX, labelY, 1.35f, color, true, "%s", label.c_str());
    if (!promptLabel.empty())
        gEnv->pAuxGeomRenderer->Draw2dLabel(labelX, labelY + 13.0f, 1.15f, promptColor, true, "%s", promptLabel.c_str());
    if (!progressLabel.empty())
        gEnv->pAuxGeomRenderer->Draw2dLabel(labelX, labelY + 27.0f, 1.05f, promptColor, true, "%s", progressLabel.c_str());
}

void ModMain::DrawCoopHudOverlayPreRender()
{
    const float now = GetHudNowSeconds();
    if (m_hasQueuedHudFeedback)
    {
        m_nullUi.ShowNotice(
            NullUiNoticeSlot::System,
            NullUiLayer::PreyHud,
            m_queuedHudFeedbackMessage,
            now,
            m_queuedHudFeedbackDuration);
        m_hasQueuedHudFeedback = false;
        m_queuedHudFeedbackMessage.clear();
    }

    DrawPeerTimeoutHudOverlay();
    m_nullUi.DrawPreyHud(now);
}

void ModMain::DrawPeerTimeoutHudOverlay()
{
    if (!m_peerTimeoutWarningActive || m_networkMode != CoopNetworkMode::Client)
    {
        m_lastPeerThrottleHudOverlayLogTime = -1000.0f;
        m_lastPeerTimeoutHudOverlayLogTime = -1000.0f;
        m_nullUi.ClearNotice(NullUiNoticeSlot::Timeout);
        return;
    }

    const float now = GetHudNowSeconds();
    if (now - m_lastPeerTimeoutHudOverlayLogTime < kTimeoutFeedbackRefreshSeconds)
        return;

    m_lastPeerTimeoutHudOverlayLogTime = now;
    const float elapsed = m_peerTimeoutWarningStartTime >= 0.0f ? std::max(0.0f, now - m_peerTimeoutWarningStartTime) : 0.0f;
    const int remaining = std::max(0, static_cast<int>(std::ceil(kPeerTimeoutCountdownSeconds - elapsed)));
    const std::string text = "(SERVER TIMEOUT) Disconnecting in " + std::to_string(remaining);
    m_nullUi.ShowNotice(NullUiNoticeSlot::Timeout, NullUiLayer::PreyHud, text, now, kTimeoutFeedbackDurationSeconds);
}

bool ModMain::ShouldBlockJoinInput() const
{
    if (m_networkMode != CoopNetworkMode::Client)
        return false;

    if (m_peerTimeoutWarningActive)
        return false;

    const bool nativeLoadScreenActive =
        m_saveLoadGuardActive &&
        !m_waitingForPostLoadContinue;
    const bool awaitingInitialHostState =
        m_clientAwaitingHostPlayerState &&
        !m_sessionGameplayReady &&
        !m_waitingForPostLoadContinue &&
        !m_saveLoadGuardActive;

    return m_saveTransferReceiving ||
        m_saveTransferSnapshotPending ||
        nativeLoadScreenActive ||
        m_pendingHostWorldLoadAfterPlayerState ||
        awaitingInitialHostState ||
        (m_pendingReceivedPlayerStateApply && !m_waitingForPostLoadContinue) ||
        (m_playerStateTransferReceiving &&
            (m_playerStateTransferFlags & CoopProtocol::kPlayerStateTransferFlagHostAuthoritative) != 0);
}

void ModMain::ApplyJoinInputBlock(const char* reason)
{
    (void)reason;
    if ((!ShouldBlockJoinInput() && !m_showMultiplayerUi) || !ArkPlayer::GetInstancePtr())
    {
        ReleaseJoinInputBlock("join block inactive");
        return;
    }

    try
    {
        ArkPlayerInput& input = ArkPlayer::GetInstance().m_input;
        input.ClearMovement();
        input.m_bSprint = false;
        input.m_bUseHeld = false;
        input.m_bTriggeredUse = false;
        input.m_bTriggeredHoldUse = false;
        input.m_bTriggeredSpecialUse = false;
        input.m_bZeroGBraking = false;
        input.m_bSprintInhibited = true;
        input.m_bJumpInhibited = true;
        m_joinInputBlocked = true;
    }
    catch (...)
    {
        m_joinInputBlocked = false;
        m_networkStatus = "join input block threw";
        CoopRuntimeLog::Write(m_networkStatus);
    }
}

void ModMain::ReleaseJoinInputBlock(const char* reason)
{
    if (!m_joinInputBlocked)
        return;

    m_joinInputBlocked = false;
    if (!ArkPlayer::GetInstancePtr())
        return;

    try
    {
        ArkPlayerInput& input = ArkPlayer::GetInstance().m_input;
        input.m_bSprintInhibited = false;
        input.m_bJumpInhibited = false;
        input.ClearMovement();

        const std::string_view releaseReason = reason ? std::string_view(reason) : std::string_view();
        const bool chairloaderMenuClosed =
            releaseReason == "Chairloader menu closed" ||
            releaseReason == "multiplayer menu closed";
        if (chairloaderMenuClosed)
        {
            if (g_pGame && g_pGame->m_pActiveUserManager)
            {
                ArkActiveUserManagerBase* activeUserManager = g_pGame->m_pActiveUserManager.get();
                if (activeUserManager->m_bListening)
                    activeUserManager->SetListening(false);
            }
            if (gEnv && gEnv->pGame && gEnv->pGame->GetIGameFramework())
                gEnv->pGame->GetIGameFramework()->PauseGame(false, true, 0, false);
            if (gEnv && gEnv->pGame)
                gEnv->pGame->RequestPause(false);

            auto& modeStack = input.m_modeStack;
            modeStack.erase(
                std::remove_if(
                    modeStack.begin(),
                    modeStack.end(),
                    [](const ArkPlayerInput::ModeAndHandle& mode)
                    {
                        return mode.m_mode == ArkPlayerInput::Mode::menu;
                    }),
                modeStack.end());
            if (modeStack.empty())
                input.EnableInputMode(ArkPlayerInput::Mode::player);
            input.EnableActionMapForMode(ArkPlayerInput::Mode::player, true);
            input.EnablePlayerInputMode(true);
            CoopRuntimeLog::Write("restored gameplay input after " + std::string(releaseReason));
        }
    }
    catch (...)
    {
        m_networkStatus = "join input release threw";
        CoopRuntimeLog::Write(m_networkStatus);
    }
}

void ModMain::TickJoinOverlay(float frameTime)
{
    (void)frameTime;

    const float now = GetHudNowSeconds();
    const bool shouldShow = ShouldBlockJoinInput();
    uint32_t progressBytes = 0;
    if (m_saveTransferReceiving)
    {
        progressBytes = ClampTransferBytes(
            m_saveTransferReceivedChunks,
            m_saveTransferTotalBytes,
            static_cast<uint32_t>(CoopProtocol::kSaveTransferDataSize));
    }
    else if (m_playerStateTransferReceiving)
    {
        progressBytes = ClampTransferBytes(
            m_playerStateTransferReceivedChunks,
            m_playerStateTransferTotalBytes,
            static_cast<uint32_t>(CoopProtocol::kPlayerStateTransferDataSize));
    }

    if (!shouldShow)
    {
        if (m_joinOverlayActive)
        {
            m_joinOverlayActive = false;
            m_joinOverlayStartTime = -1.0f;
            m_joinOverlayLastProgressTime = -1.0f;
            m_joinOverlayLastProgressBytes = 0;
        }
        ReleaseJoinInputBlock("join overlay inactive");
        return;
    }

    if (!m_joinOverlayActive)
    {
        m_joinOverlayActive = true;
        m_joinOverlayStartTime = now;
        m_joinOverlayLastProgressTime = now;
        m_joinOverlayLastProgressBytes = progressBytes;
    }

    if (progressBytes != m_joinOverlayLastProgressBytes)
    {
        m_joinOverlayLastProgressBytes = progressBytes;
        m_joinOverlayLastProgressTime = now;
    }

    const float activeSeconds = m_joinOverlayStartTime >= 0.0f ? now - m_joinOverlayStartTime : 0.0f;
    const float progressAge = m_joinOverlayLastProgressTime >= 0.0f ? now - m_joinOverlayLastProgressTime : 0.0f;
    if (activeSeconds > kJoinOverlayMaxSeconds ||
        (m_saveTransferReceiving && progressBytes > 0 && progressAge > kJoinOverlayNoProgressFallbackSeconds))
    {
        m_joinOverlayActive = false;
        ReleaseJoinInputBlock("join overlay fallback timeout");
        m_lastSaveTransferEvent = "join overlay fallback timeout";
        CoopRuntimeLog::Write(m_lastSaveTransferEvent);
        DisconnectRemotePeer("join timed out");
        return;
    }

    ApplyJoinInputBlock("join overlay");
}

void ModMain::ClearJoinOverlayState(const char* reason)
{
    m_joinOverlayActive = false;
    m_joinOverlayStartTime = -1.0f;
    m_joinOverlayLastProgressTime = -1.0f;
    m_joinOverlayLastProgressBytes = 0;
    m_joinOverlayStageOverride.clear();
    m_nullUi.ClearNotice(NullUiNoticeSlot::System);
    ReleaseJoinInputBlock(reason ? reason : "join complete");
}

void ModMain::DrawJoinOverlayImGui(float nowSeconds)
{
    (void)nowSeconds;
    if (!m_joinOverlayActive || !ImGui::GetCurrentContext())
        return;

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* drawList = viewport ? ImGui::GetForegroundDrawList(viewport) : nullptr;
    if (!drawList)
        return;

    const ImVec2 pos = viewport->Pos;
    const ImVec2 size = viewport->Size;
    if (size.x <= 1.0f || size.y <= 1.0f)
        return;

    std::string stage = "Preparing host world";
    uint32_t received = 0;
    uint32_t total = 0;
    if (!m_joinOverlayStageOverride.empty())
    {
        stage = m_joinOverlayStageOverride;
        total = 1;
        received = 1;
    }
    else if (m_saveTransferReceiving)
    {
        stage = "Downloading map";
        total = m_saveTransferTotalBytes;
        received = ClampTransferBytes(
            m_saveTransferReceivedChunks,
            total,
            static_cast<uint32_t>(CoopProtocol::kSaveTransferDataSize));
    }
    else if (m_pendingHostWorldLoadAfterPlayerState || (m_saveLoadGuardActive && !m_waitingForPostLoadContinue))
    {
        stage = "Loading host map";
        total = 1;
        received = 1;
    }
    else if (m_pendingReceivedPlayerStateApply)
    {
        stage = "Spawning player";
        total = 1;
        received = 1;
    }
    else if (m_playerStateTransferReceiving)
    {
        stage = "Syncing player state";
        total = m_playerStateTransferTotalBytes;
        received = ClampTransferBytes(
            m_playerStateTransferReceivedChunks,
            total,
            static_cast<uint32_t>(CoopProtocol::kPlayerStateTransferDataSize));
    }
    else if (m_clientAwaitingHostPlayerState)
    {
        stage = "Joining server";
        total = 1;
        received = 1;
    }

    const float progress = total > 0 ?
        std::clamp(static_cast<float>(received) / static_cast<float>(total), 0.0f, 1.0f) :
        0.0f;
    const std::string hostName = GetRemoteUsernameOrFallback();
    const std::string title = "Joining " + (hostName.empty() ? std::string("host") : hostName);
    const std::string detail = stage + " " + std::to_string(received) + " / " + std::to_string(total);

    drawList->AddRectFilled(
        pos,
        ImVec2(pos.x + size.x, pos.y + size.y),
        IM_COL32(0, 0, 0, 168));

    const float panelWidth = std::min(620.0f, std::max(320.0f, size.x - 64.0f));
    const float panelX = pos.x + (size.x - panelWidth) * 0.5f;
    const float panelY = pos.y + size.y * 0.42f;
    const float barHeight = 12.0f;
    const ImVec2 titleSize = ImGui::CalcTextSize(title.c_str());
    const ImVec2 detailSize = ImGui::CalcTextSize(detail.c_str());
    const ImU32 textColor = IM_COL32(236, 244, 255, 255);
    const ImU32 mutedColor = IM_COL32(180, 202, 226, 255);
    const ImU32 trackColor = IM_COL32(38, 55, 72, 235);
    const ImU32 fillColor = IM_COL32(72, 164, 224, 255);

    drawList->AddText(
        nullptr,
        28.0f,
        ImVec2(panelX + (panelWidth - titleSize.x * (28.0f / std::max(1.0f, ImGui::GetFontSize()))) * 0.5f, panelY),
        textColor,
        title.c_str());
    drawList->AddText(
        nullptr,
        18.0f,
        ImVec2(panelX + (panelWidth - detailSize.x * (18.0f / std::max(1.0f, ImGui::GetFontSize()))) * 0.5f, panelY + 42.0f),
        mutedColor,
        detail.c_str());

    const float barY = panelY + 78.0f;
    drawList->AddRectFilled(
        ImVec2(panelX, barY),
        ImVec2(panelX + panelWidth, barY + barHeight),
        trackColor,
        3.0f);
    drawList->AddRectFilled(
        ImVec2(panelX, barY),
        ImVec2(panelX + panelWidth * progress, barY + barHeight),
        fillColor,
        3.0f);
}

void ModMain::DrawDownedHudOverlay()
{
    const bool shouldShow = m_showCoopHudOverlay &&
        m_downedModeEnabled &&
        (m_localPlayerDowned || m_previewCoopHudOverlay);

    if (!shouldShow)
    {
        m_lastDownedHudOverlayLogTime = -1000.0f;
        m_nullUi.ClearNotice(NullUiNoticeSlot::Downed);
        return;
    }

    const float now = GetHudNowSeconds();
    if (now - m_lastDownedHudOverlayLogTime < kDownedFeedbackRefreshSeconds)
        return;

    m_lastDownedHudOverlayLogTime = now;
    std::string message = "COOP DOWNED HUD PREVIEW";
    if (m_teamWipe)
        message = "TEAM WIPE - Waiting for host checkpoint";
    else if (m_localPlayerDowned)
        message = "YOU ARE DOWNED - Find another player to revive you";
    m_nullUi.ShowNotice(NullUiNoticeSlot::Downed, NullUiLayer::PreyHud, message, now, kDownedFeedbackDurationSeconds);
}

void ModMain::ClearDownedHudOverlay()
{
    m_lastDownedHudOverlayLogTime = -1000.0f;
    m_previewCoopHudOverlay = false;
    m_nullUi.ClearNotice(NullUiNoticeSlot::Downed);
}
