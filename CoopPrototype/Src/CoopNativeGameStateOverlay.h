#pragma once

#include "CoopNativeSideBlob.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include <Prey/CryNetwork/ISerialize.h>

namespace CoopNativeGameStateOverlay
{
inline const char* SafeName(const char* value)
{
    return value && value[0] ? value : "-";
}

struct InventoryOverlayStats
{
    bool active = false;
    bool compatible = false;
    bool requiresEntityGraphPatch = false;
    uint32_t capturedItems = 0;
    uint32_t hostCells = 0;
    uint32_t patchedCells = 0;
};

struct ItemOverlayStats
{
    bool active = false;
    uint32_t patchedValues = 0;
};

class InventorySerialize final : public ISerialize
{
public:
    InventorySerialize(ISerialize* inner, const NativeSideBlobCaptureState* capture)
        : m_inner(inner)
        , m_capture(capture)
    {
        m_stats.active = m_inner && m_capture && m_capture->sawInventoryWrite;
        m_stats.capturedItems = m_capture ? static_cast<uint32_t>(std::min<size_t>(m_capture->items.size(), UINT32_MAX)) : 0;
    }

    const InventoryOverlayStats& Stats() const { return m_stats; }

    void ReadStringValue(const char* name, SSerializeString& curValue, uint32 policy) override
    {
        m_inner->ReadStringValue(name, curValue, policy);
    }

    void WriteStringValue(const char* name, SSerializeString& buffer, uint32 policy) override
    {
        m_inner->WriteStringValue(name, buffer, policy);
    }

    void Update(ISerializeUpdateFunction* pUpdate) override { m_inner->Update(pUpdate); }
    void FlagPartialRead() override { m_inner->FlagPartialRead(); }

    void BeginGroup(const char* szName) override
    {
        const std::string path = BuildPath(szName);
        m_inner->BeginGroup(szName);
        if (path == "Inventory/storedItems/i")
            m_currentCellIndex = m_nextCellIndex++;
        m_groupStack.emplace_back(SafeName(szName));
    }

    bool BeginOptionalGroup(const char* szName, bool condition) override
    {
        const bool result = m_inner->BeginOptionalGroup(szName, condition);
        if (result)
            m_groupStack.emplace_back(SafeName(szName));
        return result;
    }

    void EndGroup() override
    {
        const std::string path = CurrentPath();
        m_inner->EndGroup();
        if (path == "Inventory/storedItems/i")
            m_currentCellIndex = kNoCell;
        if (!m_groupStack.empty())
            m_groupStack.pop_back();
    }

    bool IsReading() const override { return m_inner->IsReading(); }
    bool ShouldCommitValues() const override { return m_inner->ShouldCommitValues(); }
    ESerializationTarget GetSerializationTarget() const override { return m_inner->GetSerializationTarget(); }
    bool Ok() const override { return m_inner->Ok(); }

#define SERIALIZATION_TYPE(T) \
    void Value(const char* name, T& x, uint32 policy) override \
    { \
        const std::string path = BuildPath(name); \
        m_inner->Value(name, x, policy); \
        OverrideValue(path, x); \
    }
#include <Prey/CryNetwork/SerializationTypes.h>
#undef SERIALIZATION_TYPE

#define SERIALIZATION_TYPE(T) \
    void ValueWithDefault(const char* name, T& x, const T& defaultValue) override \
    { \
        const std::string path = BuildPath(name); \
        m_inner->ValueWithDefault(name, x, defaultValue); \
        OverrideValue(path, x); \
    }
#include <Prey/CryNetwork/SerializationTypes.h>
    SERIALIZATION_TYPE(SSerializeString)
#undef SERIALIZATION_TYPE

private:
    static constexpr size_t kNoCell = static_cast<size_t>(-1);

    std::string CurrentPath() const
    {
        std::string path;
        for (const std::string& part : m_groupStack)
        {
            if (!path.empty())
                path.push_back('/');
            path += part;
        }
        return path;
    }

    std::string BuildPath(const char* name) const
    {
        const char* safeName = SafeName(name);
        std::string path = CurrentPath();
        if (!path.empty())
            path.push_back('/');
        path += safeName;
        return path;
    }

    template <typename T>
    void AssignIntegral(T& out, uint64_t value)
    {
        if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
            out = static_cast<T>(value);
    }

    template <typename T>
    void OverrideValue(const std::string& path, T& x)
    {
        if (!m_stats.active || !m_capture || !m_inner->IsReading())
            return;

        if (path == "Inventory/storedItems/Size")
        {
            if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
            {
                m_stats.hostCells = static_cast<uint32_t>(std::max<int64_t>(0, static_cast<int64_t>(x)));
                m_stats.compatible = m_stats.hostCells == m_stats.capturedItems;
                m_stats.requiresEntityGraphPatch = !m_stats.compatible;
            }
            return;
        }

        if (!m_stats.compatible || m_currentCellIndex == kNoCell || m_currentCellIndex >= m_capture->items.size())
            return;

        const NativeCapturedItemState& item = m_capture->items[m_currentCellIndex];
        if (path == "Inventory/storedItems/i/v/x")
            AssignIntegral(x, item.x);
        else if (path == "Inventory/storedItems/i/v/y")
            AssignIntegral(x, item.y);
        else if (path == "Inventory/storedItems/i/v/width")
            AssignIntegral(x, std::max(1, item.width));
        else if (path == "Inventory/storedItems/i/v/height")
            AssignIntegral(x, std::max(1, item.height));
        else
            return;

        ++m_stats.patchedCells;
    }

    ISerialize* m_inner = nullptr;
    const NativeSideBlobCaptureState* m_capture = nullptr;
    InventoryOverlayStats m_stats;
    std::vector<std::string> m_groupStack;
    size_t m_currentCellIndex = kNoCell;
    size_t m_nextCellIndex = 0;
};

class ItemSerialize final : public ISerialize
{
public:
    ItemSerialize(ISerialize* inner, const NativeCapturedItemState* item, unsigned ownerId)
        : m_inner(inner)
        , m_item(item)
        , m_ownerId(ownerId)
    {
        m_stats.active = m_inner && m_item;
    }

    const ItemOverlayStats& Stats() const { return m_stats; }

    void ReadStringValue(const char* name, SSerializeString& curValue, uint32 policy) override
    {
        m_inner->ReadStringValue(name, curValue, policy);
    }

    void WriteStringValue(const char* name, SSerializeString& buffer, uint32 policy) override
    {
        m_inner->WriteStringValue(name, buffer, policy);
    }

    void Update(ISerializeUpdateFunction* pUpdate) override { m_inner->Update(pUpdate); }
    void FlagPartialRead() override { m_inner->FlagPartialRead(); }
    void BeginGroup(const char* szName) override { m_inner->BeginGroup(szName); }
    bool BeginOptionalGroup(const char* szName, bool condition) override { return m_inner->BeginOptionalGroup(szName, condition); }
    void EndGroup() override { m_inner->EndGroup(); }
    bool IsReading() const override { return m_inner->IsReading(); }
    bool ShouldCommitValues() const override { return m_inner->ShouldCommitValues(); }
    ESerializationTarget GetSerializationTarget() const override { return m_inner->GetSerializationTarget(); }
    bool Ok() const override { return m_inner->Ok(); }

#define SERIALIZATION_TYPE(T) \
    void Value(const char* name, T& x, uint32 policy) override \
    { \
        m_inner->Value(name, x, policy); \
        OverrideValue(SafeName(name), x); \
    }
#include <Prey/CryNetwork/SerializationTypes.h>
#undef SERIALIZATION_TYPE

#define SERIALIZATION_TYPE(T) \
    void ValueWithDefault(const char* name, T& x, const T& defaultValue) override \
    { \
        m_inner->ValueWithDefault(name, x, defaultValue); \
        OverrideValue(SafeName(name), x); \
    }
#include <Prey/CryNetwork/SerializationTypes.h>
    SERIALIZATION_TYPE(SSerializeString)
#undef SERIALIZATION_TYPE

private:
    template <typename T>
    void AssignIntegral(T& out, int64_t value)
    {
        if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
            out = static_cast<T>(value);
    }

    template <typename T>
    void AssignBool(T& out, bool value)
    {
        if constexpr (std::is_same_v<T, bool>)
            out = value;
    }

    template <typename T>
    void OverrideValue(const char* name, T& x)
    {
        if (!m_stats.active || !m_item || !m_inner->IsReading())
            return;

        if (std::strcmp(name, "m_count") == 0)
            AssignIntegral(x, static_cast<uint64_t>(std::max(1, m_item->count)));
        else if (std::strcmp(name, "selectedArchetype") == 0)
            AssignIntegral(x, m_item->archetypeId);
        else if (std::strcmp(name, "ownerId") == 0)
            AssignIntegral(x, static_cast<uint64_t>(m_ownerId != 0 ? m_ownerId : m_item->ownerId));
        else if (std::strcmp(name, "favorite") == 0)
            AssignBool(x, (m_item->flags & 0x1u) != 0);
        else if (std::strcmp(name, "junk") == 0)
            AssignBool(x, (m_item->flags & 0x2u) != 0);
        else
            return;

        ++m_stats.patchedValues;
    }

    ISerialize* m_inner = nullptr;
    const NativeCapturedItemState* m_item = nullptr;
    unsigned m_ownerId = 0;
    ItemOverlayStats m_stats;
};
} // namespace CoopNativeGameStateOverlay
