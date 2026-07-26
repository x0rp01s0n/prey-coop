#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <string>

enum class NullUiLayer : uint8_t
{
    PreyHud = 0,
    FullOverlay = 1,
    ImGuiOverlay = 2,
};

enum class NullUiNoticeSlot : uint8_t
{
    System = 0,
    Timeout = 1,
    Downed = 2,
    Count = 3,
};

class NullUi
{
public:
    void ShowNotice(NullUiNoticeSlot slot, NullUiLayer layer, const std::string& text, float nowSeconds, float durationSeconds);
    void ClearNotice(NullUiNoticeSlot slot);
    void ClearAll();
    bool HasVisibleNotice(float nowSeconds) const;
    void DrawPreyHud(float nowSeconds);
    void DrawImGuiOverlay(float nowSeconds);

private:
    struct Notice
    {
        std::string text;
        NullUiLayer layer = NullUiLayer::PreyHud;
        float startSeconds = 0.0f;
        float durationSeconds = 0.0f;
        bool active = false;
    };

    static constexpr size_t kNoticeCount = static_cast<size_t>(NullUiNoticeSlot::Count);

    Notice* FindNotice(NullUiNoticeSlot slot);
    const Notice* FindNotice(NullUiNoticeSlot slot) const;
    bool IsNoticeVisible(const Notice& notice, float nowSeconds) const;
    float GetNoticeAlpha(const Notice& notice, float nowSeconds) const;

    std::array<Notice, kNoticeCount> m_notices;
};
