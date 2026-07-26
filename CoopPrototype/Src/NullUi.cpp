#include "NullUi.h"

#include <algorithm>
#include <cmath>

#include <imgui.h>
#include <Prey/CryRenderer/IRenderAuxGeom.h>
#include <Prey/CryRenderer/IRenderer.h>
#include <Prey/CrySystem/ISystem.h>

namespace
{
constexpr float kVirtualWidth = 800.0f;
constexpr float kSystemNoticeX = 26.0f;
constexpr float kSystemNoticeY = 42.0f;
constexpr float kPriorityNoticeY = 88.0f;
constexpr float kNoticeLineHeight = 24.0f;
constexpr float kMinNoticeDurationSeconds = 0.15f;
constexpr float kNoticeFadeSeconds = 0.25f;
constexpr float kSystemFontSize = 1.05f;
constexpr float kPriorityFontSize = 1.65f;
constexpr float kImGuiSystemFontSize = 18.0f;
constexpr float kImGuiPriorityFontSize = 28.0f;

size_t SlotIndex(NullUiNoticeSlot slot)
{
    const size_t index = static_cast<size_t>(slot);
    return index < static_cast<size_t>(NullUiNoticeSlot::Count) ? index : 0;
}
}

void NullUi::ShowNotice(NullUiNoticeSlot slot, NullUiLayer layer, const std::string& text, float nowSeconds, float durationSeconds)
{
    Notice* notice = FindNotice(slot);
    if (!notice)
        return;

    if (text.empty())
    {
        ClearNotice(slot);
        return;
    }

    notice->text = text;
    notice->layer = layer;
    notice->startSeconds = std::max(0.0f, nowSeconds);
    notice->durationSeconds = std::max(kMinNoticeDurationSeconds, durationSeconds);
    notice->active = true;
}

void NullUi::ClearNotice(NullUiNoticeSlot slot)
{
    Notice* notice = FindNotice(slot);
    if (!notice)
        return;

    notice->text.clear();
    notice->durationSeconds = 0.0f;
    notice->active = false;
}

void NullUi::ClearAll()
{
    for (Notice& notice : m_notices)
    {
        notice.text.clear();
        notice.durationSeconds = 0.0f;
        notice.active = false;
    }
}

bool NullUi::HasVisibleNotice(float nowSeconds) const
{
    for (const Notice& notice : m_notices)
    {
        if (IsNoticeVisible(notice, nowSeconds))
            return true;
    }

    return false;
}

void NullUi::DrawPreyHud(float nowSeconds)
{
    if (!gEnv || !gEnv->pRenderer || !gEnv->pAuxGeomRenderer)
        return;

    const Notice* systemNotice = FindNotice(NullUiNoticeSlot::System);
    if (systemNotice && systemNotice->layer != NullUiLayer::ImGuiOverlay)
    {
        if (!IsNoticeVisible(*systemNotice, nowSeconds))
        {
            if (Notice* mutableNotice = FindNotice(NullUiNoticeSlot::System))
                mutableNotice->active = false;
        }
        else
        {
            const float alpha = GetNoticeAlpha(*systemNotice, nowSeconds);
            const float color[4] = { 0.86f, 0.92f, 1.0f, alpha };
            gEnv->pAuxGeomRenderer->Draw2dLabel(
                kSystemNoticeX,
                kSystemNoticeY,
                kSystemFontSize,
                color,
                false,
                "%s",
                systemNotice->text.c_str());
        }
    }

    struct PriorityEntry
    {
        const Notice* notice = nullptr;
        NullUiNoticeSlot slot = NullUiNoticeSlot::Timeout;
    };

    const PriorityEntry ordered[] = {
        { FindNotice(NullUiNoticeSlot::Timeout), NullUiNoticeSlot::Timeout },
        { FindNotice(NullUiNoticeSlot::Downed), NullUiNoticeSlot::Downed },
    };

    float y = kPriorityNoticeY;
    for (const PriorityEntry& entry : ordered)
    {
        const Notice* notice = entry.notice;
        if (!notice || notice->layer == NullUiLayer::ImGuiOverlay)
            continue;

        if (!IsNoticeVisible(*notice, nowSeconds))
        {
            if (Notice* mutableNotice = FindNotice(entry.slot))
                mutableNotice->active = false;
            continue;
        }

        const float alpha = GetNoticeAlpha(*notice, nowSeconds);
        const float color[4] = { 1.0f, 0.96f, 1.0f, alpha };

        gEnv->pAuxGeomRenderer->Draw2dLabel(
            kVirtualWidth * 0.5f,
            y,
            kPriorityFontSize,
            color,
            true,
            "%s",
            notice->text.c_str());
        y += kNoticeLineHeight;
    }
}

void NullUi::DrawImGuiOverlay(float nowSeconds)
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* drawList = viewport ? ImGui::GetForegroundDrawList(viewport) : nullptr;
    if (!drawList)
        return;

    const ImVec2 viewportPos = viewport->Pos;
    const ImVec2 displaySize = viewport->Size;
    if (displaySize.x <= 1.0f || displaySize.y <= 1.0f)
        return;

    auto textColor = [](float alpha) -> ImU32
    {
        const int a = static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
        return IM_COL32(230, 242, 255, a);
    };

    auto shadowColor = [](float alpha) -> ImU32
    {
        const int a = static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 190.0f + 0.5f);
        return IM_COL32(0, 0, 0, a);
    };

    auto drawText = [&](const ImVec2& pos, float fontSize, ImU32 color, ImU32 shadow, const std::string& text)
    {
        drawList->AddText(nullptr, fontSize, ImVec2(pos.x + 2.0f, pos.y + 2.0f), shadow, text.c_str());
        drawList->AddText(nullptr, fontSize, pos, color, text.c_str());
    };

    const Notice* systemNotice = FindNotice(NullUiNoticeSlot::System);
    if (systemNotice)
    {
        if (!IsNoticeVisible(*systemNotice, nowSeconds))
        {
            if (Notice* mutableNotice = FindNotice(NullUiNoticeSlot::System))
                mutableNotice->active = false;
        }
        else
        {
            const float alpha = GetNoticeAlpha(*systemNotice, nowSeconds);
            drawText(
                ImVec2(viewportPos.x + 26.0f, viewportPos.y + 54.0f),
                kImGuiSystemFontSize,
                textColor(alpha),
                shadowColor(alpha),
                systemNotice->text);
        }
    }

    struct PriorityEntry
    {
        const Notice* notice = nullptr;
        NullUiNoticeSlot slot = NullUiNoticeSlot::Timeout;
    };

    const PriorityEntry ordered[] = {
        { FindNotice(NullUiNoticeSlot::Timeout), NullUiNoticeSlot::Timeout },
        { FindNotice(NullUiNoticeSlot::Downed), NullUiNoticeSlot::Downed },
    };

    float y = viewportPos.y + std::max(88.0f, displaySize.y * 0.40f);
    for (const PriorityEntry& entry : ordered)
    {
        const Notice* notice = entry.notice;
        if (!notice)
            continue;

        if (!IsNoticeVisible(*notice, nowSeconds))
        {
            if (Notice* mutableNotice = FindNotice(entry.slot))
                mutableNotice->active = false;
            continue;
        }

        const float alpha = GetNoticeAlpha(*notice, nowSeconds);
        const ImVec2 textSize = ImGui::CalcTextSize(notice->text.c_str());
        const float widthScale = kImGuiPriorityFontSize / std::max(1.0f, ImGui::GetFontSize());
        const float textWidth = textSize.x * widthScale;
        const ImVec2 pos(viewportPos.x + std::max(24.0f, (displaySize.x - textWidth) * 0.5f), y);
        drawText(
            pos,
            kImGuiPriorityFontSize,
            textColor(alpha),
            shadowColor(alpha),
            notice->text);
        y += kImGuiPriorityFontSize + 8.0f;
    }
}

NullUi::Notice* NullUi::FindNotice(NullUiNoticeSlot slot)
{
    return &m_notices[SlotIndex(slot)];
}

const NullUi::Notice* NullUi::FindNotice(NullUiNoticeSlot slot) const
{
    return &m_notices[SlotIndex(slot)];
}

bool NullUi::IsNoticeVisible(const Notice& notice, float nowSeconds) const
{
    if (!notice.active || notice.text.empty())
        return false;

    return nowSeconds <= notice.startSeconds + notice.durationSeconds;
}

float NullUi::GetNoticeAlpha(const Notice& notice, float nowSeconds) const
{
    const float elapsed = std::max(0.0f, nowSeconds - notice.startSeconds);
    const float remaining = notice.durationSeconds - elapsed;
    if (remaining >= kNoticeFadeSeconds)
        return 1.0f;

    return std::clamp(remaining / kNoticeFadeSeconds, 0.0f, 1.0f);
}
