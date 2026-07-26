#include "CoopNativeSaveXmlPatch.h"

#include "CoopRuntimeGuards.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <Prey/CrySystem/XML/IXml.h>

namespace
{
std::string JsonEscape(std::string_view value)
{
    std::ostringstream out;
    for (const unsigned char ch : value)
    {
        switch (ch)
        {
        case '\\':
            out << "\\\\";
            break;
        case '"':
            out << "\\\"";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (ch < 0x20)
            {
                out << "\\u"
                    << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch)
                    << std::dec << std::setfill(' ');
            }
            else
            {
                out << static_cast<char>(ch);
            }
            break;
        }
    }
    return out.str();
}

std::string TrimDiagnosticText(const char* value, size_t maxLength)
{
    if (!value || !value[0])
        return {};

    std::string result;
    for (size_t i = 0; value[i] && result.size() < maxLength; ++i)
    {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        if (std::iscntrl(ch) || std::isspace(ch))
            result.push_back('_');
        else
            result.push_back(static_cast<char>(ch));
    }
    if (value[result.size()])
        result += "...";
    return result;
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string PathAppend(const std::string& parentPath, const char* tag, int childIndex)
{
    std::ostringstream out;
    if (parentPath.empty())
        out << "/";
    else
        out << parentPath << "/";

    const std::string safeTag = TrimDiagnosticText(tag, 64);
    out << (safeTag.empty() ? std::string("-") : safeTag) << "[" << childIndex << "]";
    return out.str();
}

void AppendIndexUnsafe(
    const XmlNodeRef& node,
    const std::string& path,
    int depth,
    int maxDepth,
    uint32_t maxNodes,
    uint32_t& visited,
    uint32_t& hits,
    std::ostringstream& out)
{
    if (visited >= maxNodes)
        return;

    IXmlNode* rawNode = static_cast<IXmlNode*>(node);
    if (!rawNode)
        return;

    ++visited;
    const char* tagRaw = rawNode->getTag();
    const std::string tag = TrimDiagnosticText(tagRaw, 96);
    const int attrCount = rawNode->getNumAttributes();
    const int childCount = rawNode->getChildCount();
    const std::string content = TrimDiagnosticText(rawNode->getContent(), 160);

    std::ostringstream attrsText;
    std::ostringstream searchable;
    searchable << tag << " " << content;

    const int attrLimit = std::min(attrCount, 64);
    for (int attrIndex = 0; attrIndex < attrLimit; ++attrIndex)
    {
        const char* key = nullptr;
        const char* value = nullptr;
        if (!rawNode->getAttributeByIndex(attrIndex, &key, &value))
            continue;

        const std::string safeKey = TrimDiagnosticText(key, 96);
        const std::string safeValue = TrimDiagnosticText(value, 192);
        if (attrsText.tellp() > 0)
            attrsText << " ";
        attrsText << safeKey << "=" << safeValue;
        searchable << " " << safeKey << " " << safeValue;
    }

    const std::string lowerSearch = LowerAscii(searchable.str());
    std::vector<std::string> tokens;
    constexpr const char* kInterestingTokens[] = {
        "gamestate",
        "persistentstate",
        "player",
        "inventory",
        "storeditems",
        "entityid",
        "selectedarchetype",
        "ownerid",
        "arkitem",
        "carkitem",
        "weapon",
        "health",
        "psi",
        "neuromod",
    };
    for (const char* token : kInterestingTokens)
    {
        if (token && token[0] && lowerSearch.find(token) != std::string::npos)
            tokens.emplace_back(token);
    }
    if (!tokens.empty())
        ++hits;

    out << "{\"path\":\"" << JsonEscape(path)
        << "\",\"depth\":" << depth
        << ",\"tag\":\"" << JsonEscape(tag)
        << "\",\"attrs\":" << attrCount
        << ",\"children\":" << childCount
        << ",\"attrText\":\"" << JsonEscape(attrsText.str())
        << "\",\"content\":\"" << JsonEscape(content)
        << "\",\"hit\":";
    if (tokens.empty())
    {
        out << "[]";
    }
    else
    {
        out << "[";
        for (size_t i = 0; i < tokens.size(); ++i)
        {
            if (i != 0)
                out << ",";
            out << "\"" << JsonEscape(tokens[i]) << "\"";
        }
        out << "]";
    }
    out << "}\n";

    if (depth >= maxDepth)
        return;

    const int childLimit = std::min(childCount, static_cast<int>(maxNodes - visited));
    for (int childIndex = 0; childIndex < childLimit && visited < maxNodes; ++childIndex)
    {
        XmlNodeRef child = rawNode->getChild(childIndex);
        IXmlNode* rawChild = static_cast<IXmlNode*>(child);
        AppendIndexUnsafe(
            child,
            PathAppend(path, rawChild ? rawChild->getTag() : nullptr, childIndex),
            depth + 1,
            maxDepth,
            maxNodes,
            visited,
            hits,
            out);
    }
}
} // namespace

namespace CoopNativeSaveXmlPatch
{
PathIndexResult WritePathIndex(
    const XmlNodeRef& root,
    const std::filesystem::path& path,
    uint32_t maxNodes,
    uint32_t maxDepth)
{
    PathIndexResult result;

    std::ostringstream index;
    result.ok = CoopRuntimeGuards::TryGuardedVoidCall(
        "coop native xml path index dump",
        [&]()
        {
            IXmlNode* rawRoot = static_cast<IXmlNode*>(root);
            AppendIndexUnsafe(
                root,
                PathAppend(std::string(), rawRoot ? rawRoot->getTag() : nullptr, 0),
                0,
                static_cast<int>(maxDepth),
                maxNodes,
                result.nodes,
                result.hits,
                index);
        },
        &result.guardReason);

    if (!result.ok)
        index << "{\"error\":\"" << JsonEscape(result.guardReason) << "\"}\n";

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (output)
    {
        output << index.str();
        result.written = true;
    }

    return result;
}
} // namespace CoopNativeSaveXmlPatch
