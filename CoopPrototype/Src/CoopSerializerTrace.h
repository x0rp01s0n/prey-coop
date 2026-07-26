#pragma once

#include "ModMain.h"

#include <cstring>
#include <string>
#include <vector>

#include <Prey/CryNetwork/ISerialize.h>

namespace CoopSerializerTrace
{
inline const char* SafeName(const char* value)
{
    return value && value[0] ? value : "-";
}

class TracingSerialize final : public ISerialize
{
public:
    TracingSerialize(ModMain* owner, ISerialize* inner, const char* source)
        : m_owner(owner)
        , m_inner(inner)
        , m_source(source ? source : "level_state")
    {
    }

    void ReadStringValue(const char* name, SSerializeString& curValue, uint32 policy) override
    {
        const std::string path = BuildPath(name);
        ForwardTrace("string-read", path.c_str(), "string", ("policy=" + std::to_string(policy)).c_str());
        m_inner->ReadStringValue(name, curValue, policy);
    }

    void WriteStringValue(const char* name, SSerializeString& buffer, uint32 policy) override
    {
        const std::string path = BuildPath(name);
        ForwardTrace("string-write", path.c_str(), "string", ("policy=" + std::to_string(policy) + " len=" + std::to_string(buffer.length())).c_str());
        m_inner->WriteStringValue(name, buffer, policy);
    }

    void Update(ISerializeUpdateFunction* pUpdate) override
    {
        ForwardTrace("update", "-", "-", "");
        m_inner->Update(pUpdate);
    }

    void FlagPartialRead() override
    {
        ForwardTrace("partial-read", "-", "-", "");
        m_inner->FlagPartialRead();
    }

    void BeginGroup(const char* szName) override
    {
        const std::string path = BuildPath(szName);
        ForwardTrace("begin", path.c_str(), "-", "");
        ForwardProbe("pre-begin", path.c_str());
        m_inner->BeginGroup(szName);
        ForwardProbe("post-begin", path.c_str());
        m_groupStack.emplace_back(SafeName(szName));
        ++m_depth;
    }

    bool BeginOptionalGroup(const char* szName, bool condition) override
    {
        const std::string path = BuildPath(szName);
        ForwardProbe("pre-optional", path.c_str());
        const bool result = m_inner->BeginOptionalGroup(szName, condition);
        const std::string detail =
            "condition=" + std::to_string(condition ? 1 : 0) +
            " result=" + std::to_string(result ? 1 : 0);
        ForwardTrace("optional", path.c_str(), "-", detail.c_str());
        ForwardProbe(result ? "post-optional-entered" : "post-optional-skipped", path.c_str());
        if (result)
        {
            m_groupStack.emplace_back(SafeName(szName));
            ++m_depth;
        }
        return result;
    }

    void EndGroup() override
    {
        const std::string path = CurrentPath();
        ForwardTrace("end", path.c_str(), "-", "");
        ForwardProbe("pre-end", path.c_str());
        m_inner->EndGroup();
        ForwardProbe("post-end", path.c_str());
        if (!m_groupStack.empty())
            m_groupStack.pop_back();
        if (m_depth > 0)
            --m_depth;
    }

    bool IsReading() const override
    {
        return m_inner->IsReading();
    }

    bool ShouldCommitValues() const override
    {
        return m_inner->ShouldCommitValues();
    }

    ESerializationTarget GetSerializationTarget() const override
    {
        return m_inner->GetSerializationTarget();
    }

    bool Ok() const override
    {
        return m_inner->Ok();
    }

#define SERIALIZATION_TYPE(T) \
    void Value(const char* name, T& x, uint32 policy) override \
    { \
        const std::string path = BuildPath(name); \
        TraceValue(path.c_str(), #T, policy); \
        ForwardProbe("pre-value", path.c_str()); \
        m_inner->Value(name, x, policy); \
        ForwardSerializedValue(path.c_str(), #T, x); \
        ForwardProbe("post-value", path.c_str()); \
    }
#include <Prey/CryNetwork/SerializationTypes.h>
#undef SERIALIZATION_TYPE

#define SERIALIZATION_TYPE(T) \
    void ValueWithDefault(const char* name, T& x, const T& defaultValue) override \
    { \
        const std::string path = BuildPath(name); \
        TraceValueWithDefault(path.c_str(), #T); \
        ForwardProbe("pre-default", path.c_str()); \
        m_inner->ValueWithDefault(name, x, defaultValue); \
        ForwardProbe("post-default", path.c_str()); \
    }
#include <Prey/CryNetwork/SerializationTypes.h>
    SERIALIZATION_TYPE(SSerializeString)
#undef SERIALIZATION_TYPE

private:
    void TraceValue(const char* path, const char* typeName, uint32 policy)
    {
        const std::string detail = "policy=" + std::to_string(policy);
        ForwardTrace("value", path, typeName, detail.c_str());
    }

    void TraceValueWithDefault(const char* path, const char* typeName)
    {
        ForwardTrace("default", path, typeName, "");
    }

    template <typename T>
    void ForwardSerializedValue(const char*, const char*, const T&)
    {
    }

    void ForwardSerializedValue(const char* path, const char* typeName, const uint32& value)
    {
        if (!m_owner || !m_inner)
            return;
        if (std::strcmp(typeName, "uint32") != 0)
            return;
        m_owner->RecordNativeGameStateInventoryCellEntityId(
            m_source,
            SafeName(path),
            value,
            m_inner->IsReading(),
            static_cast<int>(m_inner->GetSerializationTarget()));
    }

    std::string CurrentPath() const
    {
        if (m_groupStack.empty())
            return "-";

        std::string path;
        for (const std::string& part : m_groupStack)
        {
            if (!path.empty())
                path.push_back('/');
            path += part;
        }
        return path.empty() ? std::string("-") : path;
    }

    std::string BuildPath(const char* name) const
    {
        const char* safeName = SafeName(name);
        if (m_groupStack.empty())
            return safeName;

        std::string path = CurrentPath();
        if (std::strcmp(safeName, "-") != 0)
        {
            path.push_back('/');
            path += safeName;
        }
        return path;
    }

    void ForwardTrace(const char* op, const char* name, const char* typeName, const char* detail)
    {
        if (!m_owner)
            return;
        m_owner->RecordLevelStateSerializerTraceOp(
            m_source,
            op,
            SafeName(name),
            SafeName(typeName),
            m_inner ? m_inner->IsReading() : false,
            m_depth,
            detail ? detail : "");
    }

    void ForwardProbe(const char* op, const char* path)
    {
        if (!m_owner || !m_inner)
            return;
        if (!m_owner->ShouldProbeSerializerNodeContext(m_source))
            return;
        m_owner->RecordSerializerNodeContextProbe(
            m_source,
            SafeName(op),
            SafeName(path),
            m_inner);
    }

    ModMain* m_owner = nullptr;
    ISerialize* m_inner = nullptr;
    const char* m_source = "level_state";
    std::vector<std::string> m_groupStack;
    int m_depth = 0;
};
} // namespace CoopSerializerTrace
