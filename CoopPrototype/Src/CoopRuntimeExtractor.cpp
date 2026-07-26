#include "CoopRuntimeExtractor.h"

#include "CoopRuntimeConfig.h"
#include "CoopRuntimeLog.h"
#include "CoopRuntimeGuards.h"

#include <winsock2.h>
#include <windows.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>

#include <Chairloader/IChairLogger.h>
#include <Prey/CryAction/IGameObject.h>
#include <Prey/CryGame/IGame.h>
#include <Prey/CryGame/IGameFramework.h>
#include <Prey/CryEntitySystem/IEntityClass.h>
#include <Prey/CryEntitySystem/IEntitySystem.h>
#include <Prey/CrySystem/ISystem.h>

namespace
{
using CoopRuntimeGuards::GetRuntimeModulePath;
using CoopRuntimeGuards::GetRuntimeGuardSnapshot;
using CoopRuntimeGuards::IsLikelyRuntimeCppObject;
using CoopRuntimeGuards::ReadRuntimeCString;
using CoopRuntimeGuards::TryGuardedCall;
using CoopRuntimeGuards::TryGuardedVoidCall;
using CoopRuntimeGuards::TryReadRuntimeValue;

constexpr float kCommandPollSeconds = 0.25f;

void LogExtractor(std::string_view msg)
{
    const bool verbose = CoopRuntimeConfig::Flag("COOP_TRACE_RUNTIME_EXTRACTOR");
    const bool failure = msg.rfind("FAILED", 0) == 0 || msg.find("failed") != std::string_view::npos;
    if (!verbose && !failure)
        return;

    CoopRuntimeLog::Write("[Extractor] " + std::string(msg));
}

std::string ToLowerAscii(std::string value)
{
    for (char& ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

std::string Trim(std::string value)
{
    const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();

    if (begin >= end)
        return {};

    return std::string(begin, end);
}

bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle)
{
    if (needle.empty())
        return true;

    return ToLowerAscii(haystack).find(ToLowerAscii(needle)) != std::string::npos;
}

std::string NormalizeLevelName(std::string levelName)
{
    for (char& ch : levelName)
    {
        if (ch == '\\')
            ch = '/';
        else
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return levelName.empty() ? "unknown" : levelName;
}

std::string JsonEscape(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value)
    {
        switch (ch)
        {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20)
            {
                char escaped[8] = {};
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned char>(ch));
                out += escaped;
            }
            else
            {
                out.push_back(ch);
            }
            break;
        }
    }
    return out;
}

std::string ReadWholeFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string PointerHex(std::uintptr_t value)
{
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
    return buffer;
}
}

void CoopRuntimeExtractor::Tick(float frameTime)
{
    if (m_pollCommandFile)
        ProcessCommandFile(frameTime, false);
}

void CoopRuntimeExtractor::TickControlOnly(float frameTime)
{
    if (m_pollCommandFile)
        ProcessCommandFile(frameTime, true);
}

void CoopRuntimeExtractor::SetControlCommandHandler(ControlCommandHandler handler)
{
    m_controlCommandHandler = std::move(handler);
}

void CoopRuntimeExtractor::Draw()
{
    ImGui::Separator();
    if (!ImGui::CollapsingHeader("Runtime Extractor"))
        return;

    const CoopRuntimeGuards::RuntimeGuardSnapshot guard = GetRuntimeGuardSnapshot();
    ImGui::Text("Status: %s", m_lastStatus.c_str());
    ImGui::Text("Guard: calls=%llu failures=%llu preflight=%llu seh=%llu last=%s",
        static_cast<unsigned long long>(guard.guardedCalls),
        static_cast<unsigned long long>(guard.guardedCallFailures),
        static_cast<unsigned long long>(guard.preflightFailures),
        static_cast<unsigned long long>(guard.sehExceptions),
        guard.lastOperation.empty() ? "-" : guard.lastOperation.c_str());
    ImGui::TextWrapped("Guard reason: %s", guard.lastReason.empty() ? "-" : guard.lastReason.c_str());
    ImGui::Text("Scans: entity=%u class=%u ext=%u export=%u guardSkips=%u",
        m_scanCount,
        m_classScanCount,
        m_extensionProbeCount,
        m_exportCount,
        m_guardSkips);
    ImGui::Text("Entities: shown=%zu seen=%u filtered=%u  Extensions: found=%zu misses=%u",
        m_entityRows.size(),
        m_entitiesSeen,
        m_entitiesFiltered,
        m_extensionRows.size(),
        m_extensionMisses);
    ImGui::Checkbox("Poll command file", &m_pollCommandFile);
    ImGui::SameLine();
    ImGui::Checkbox("Filter entity list", &m_showOnlyFilteredEntities);
    ImGui::InputText("Entity filter", &m_entityFilter);
    ImGui::InputText("Extension probe", &m_extensionProbeName);
    ImGui::SliderInt("Max entity rows", &m_maxEntityRows, 32, 4096);
    ImGui::SliderInt("Max extension rows", &m_maxExtensionProbeRows, 16, 2048);

    if (ImGui::Button("Scan entities"))
        ScanEntities();
    ImGui::SameLine();
    if (ImGui::Button("Scan classes"))
        ScanClassRegistry();
    ImGui::SameLine();
    if (ImGui::Button("Probe extension"))
        ProbeExtensionAcrossSnapshot(m_extensionProbeName);
    ImGui::SameLine();
    if (ImGui::Button("Export JSONL"))
        ExportJsonl();
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
        ClearSnapshots("manual clear");

    ImGui::TextWrapped("Command: %s", GetCommandFilePath().c_str());
    ImGui::TextWrapped("Export: %s", m_lastExportPath.c_str());
    ImGui::TextWrapped("Last command: %s", m_lastCommandStatus.c_str());

    if (ImGui::BeginTabBar("CoopRuntimeExtractorTabs"))
    {
        if (ImGui::BeginTabItem("Entities"))
        {
            DrawEntityRows();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Classes"))
        {
            DrawClassRows();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Extensions"))
        {
            DrawExtensionRows();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void CoopRuntimeExtractor::ResetRuntimeRefs(const char* reason)
{
    ClearSnapshots(reason && reason[0] ? reason : "runtime reset");
}

std::string CoopRuntimeExtractor::GetCurrentLevelName() const
{
    IGameFramework* framework = nullptr;
    std::string guardReason;
    if (!gEnv || !gEnv->pGame ||
        !TryGuardedCall("IGame::GetIGameFramework", []() { return gEnv->pGame->GetIGameFramework(); }, framework, &guardReason) ||
        !IsLikelyRuntimeCppObject(framework))
    {
        return "unknown";
    }

    const char* levelName = nullptr;
    if (!TryGuardedCall("IGameFramework::GetLevelName", [framework]() { return framework->GetLevelName(); }, levelName, &guardReason))
        return "unknown";
    if (!levelName || !levelName[0])
        return "unknown";

    return NormalizeLevelName(levelName);
}

std::string CoopRuntimeExtractor::GetExtractorRootPath() const
{
    const char* overridePath = std::getenv("COOP_EXTRACTOR_ROOT");
    if (overridePath && overridePath[0])
        return overridePath;

    const char* userProfile = std::getenv("USERPROFILE");
    if (userProfile && userProfile[0])
    {
        std::filesystem::path root(userProfile);
        root /= "Saved Games";
        root /= "Arkane Studios";
        root /= "Prey";
        root /= "CoopRuntimeExtractor";
        return root.string();
    }

    return "CoopRuntimeExtractor";
}

std::string CoopRuntimeExtractor::GetCommandFilePath() const
{
    const char* overridePath = std::getenv("COOP_EXTRACTOR_COMMAND_FILE");
    if (overridePath && overridePath[0])
        return overridePath;

    std::filesystem::path path(GetExtractorRootPath());
    path /= "command.txt";
    return path.string();
}

std::string CoopRuntimeExtractor::GetStatusFilePath() const
{
    std::filesystem::path path(GetExtractorRootPath());
    path /= "status.txt";
    return path.string();
}

std::string CoopRuntimeExtractor::GetLatestExportPath() const
{
    std::filesystem::path path(GetExtractorRootPath());
    path /= "latest.jsonl";
    return path.string();
}

bool CoopRuntimeExtractor::CaptureEntityRow(IEntity& entity, EntityRow& outRow)
{
    if (!IsLikelyRuntimeCppObject(&entity))
        return false;

    std::string guardReason;
    outRow.entityPtr = reinterpret_cast<std::uintptr_t>(&entity);
    if (!TryGuardedCall("IEntity::GetId", [&entity]() { return entity.GetId(); }, outRow.entityId, &guardReason) ||
        !TryGuardedCall("IEntity::GetGuid", [&entity]() { return entity.GetGuid(); }, outRow.guid, &guardReason) ||
        !TryGuardedCall("IEntity::GetWorldPos", [&entity]() { return entity.GetWorldPos(); }, outRow.position, &guardReason) ||
        !TryGuardedCall("IEntity::GetFlags", [&entity]() { return entity.GetFlags(); }, outRow.flags, &guardReason) ||
        !TryGuardedCall("IEntity::IsLoadedFromLevelFile", [&entity]() { return entity.IsLoadedFromLevelFile(); }, outRow.loadedFromLevel, &guardReason) ||
        !TryGuardedCall("IEntity::IsGarbage", [&entity]() { return entity.IsGarbage(); }, outRow.garbage, &guardReason))
    {
        m_lastGuardFailure = guardReason;
        return false;
    }

    const char* namePtr = nullptr;
    if (TryGuardedCall("IEntity::GetName", [&entity]() { return entity.GetName(); }, namePtr, &guardReason))
        outRow.name = ReadRuntimeCString(namePtr, 160);
    else
        m_lastGuardFailure = guardReason;

    IEntityClass* entityClass = nullptr;
    if (!TryGuardedCall("IEntity::GetClass", [&entity]() { return entity.GetClass(); }, entityClass, &guardReason))
    {
        m_lastGuardFailure = guardReason;
        entityClass = nullptr;
    }
    if (IsLikelyRuntimeCppObject(entityClass))
    {
        outRow.classPtr = reinterpret_cast<std::uintptr_t>(entityClass);
        const char* classNamePtr = nullptr;
        if (TryGuardedCall("IEntityClass::GetName", [entityClass]() { return entityClass->GetName(); }, classNamePtr, &guardReason))
            outRow.className = ReadRuntimeCString(classNamePtr, 160);
        else
            m_lastGuardFailure = guardReason;
    }

    const IEntityArchetype* archetype = nullptr;
    if (!TryGuardedCall("IEntity::GetArchetype", [&entity]() { return entity.GetArchetype(); }, archetype, &guardReason))
    {
        m_lastGuardFailure = guardReason;
        archetype = nullptr;
    }
    if (IsLikelyRuntimeCppObject(archetype))
    {
        outRow.archetypePtr = reinterpret_cast<std::uintptr_t>(archetype);
        const char* archetypeNamePtr = nullptr;
        if (TryGuardedCall("IEntityArchetype::GetName", [archetype]() { return archetype->GetName(); }, archetypeNamePtr, &guardReason))
            outRow.archetypeName = ReadRuntimeCString(archetypeNamePtr, 220);
        else
            m_lastGuardFailure = guardReason;
        if (!TryGuardedCall("IEntityArchetype::GetId", [archetype]() { return archetype->GetId(); }, outRow.archetypeId, &guardReason))
            m_lastGuardFailure = guardReason;
    }

    if (outRow.name.empty())
        outRow.name = "-";
    if (outRow.className.empty())
        outRow.className = "-";
    if (outRow.archetypeName.empty())
        outRow.archetypeName = "-";
    return true;
}

bool CoopRuntimeExtractor::EntityMatchesFilter(const EntityRow& row) const
{
    const std::string filter = Trim(m_entityFilter);
    return filter.empty() ||
        ContainsCaseInsensitive(row.name, filter) ||
        ContainsCaseInsensitive(row.className, filter) ||
        ContainsCaseInsensitive(row.archetypeName, filter) ||
        std::to_string(row.entityId).find(filter) != std::string::npos ||
        std::to_string(row.guid).find(filter) != std::string::npos;
}

bool CoopRuntimeExtractor::ScanEntities()
{
    ++m_scanCount;
    m_entityRows.clear();
    m_entitiesSeen = 0;
    m_entitiesFiltered = 0;
    m_lastLevelName = GetCurrentLevelName();

    if (!gEnv || !gEnv->pEntitySystem || !IsLikelyRuntimeCppObject(gEnv->pEntitySystem))
    {
        m_lastStatus = "entity scan failed: no valid entity system";
        WriteStatusFile();
        return false;
    }

    std::string guardReason;
    IEntityIt* rawIterator = nullptr;
    if (!TryGuardedCall("IEntitySystem::GetEntityIterator", []() { return gEnv->pEntitySystem->GetEntityIterator(); }, rawIterator, &guardReason))
    {
        ++m_guardSkips;
        m_lastGuardFailure = guardReason;
        m_lastStatus = "entity scan failed: guarded iterator call failed";
        WriteStatusFile();
        return false;
    }

    IEntityItPtr iterator = rawIterator;
    if (!iterator)
    {
        m_lastStatus = "entity scan failed: no iterator";
        WriteStatusFile();
        return false;
    }

    const int maxRows = std::max(1, m_maxEntityRows);
    if (!TryGuardedVoidCall("IEntityIt::MoveFirst", [&iterator]() { iterator->MoveFirst(); }, &guardReason))
    {
        ++m_guardSkips;
        m_lastGuardFailure = guardReason;
        m_lastStatus = "entity scan failed: guarded MoveFirst failed";
        WriteStatusFile();
        return false;
    }

    while (true)
    {
        bool isEnd = true;
        if (!TryGuardedCall("IEntityIt::IsEnd", [&iterator]() { return iterator->IsEnd(); }, isEnd, &guardReason))
        {
            ++m_guardSkips;
            m_lastGuardFailure = guardReason;
            break;
        }
        if (isEnd)
            break;

        IEntity* entity = nullptr;
        if (!TryGuardedCall("IEntityIt::Next", [&iterator]() { return iterator->Next(); }, entity, &guardReason))
        {
            ++m_guardSkips;
            m_lastGuardFailure = guardReason;
            break;
        }
        ++m_entitiesSeen;
        if (!entity || !IsLikelyRuntimeCppObject(entity))
        {
            ++m_guardSkips;
            continue;
        }

        EntityRow row;
        if (!CaptureEntityRow(*entity, row))
        {
            ++m_guardSkips;
            continue;
        }

        if (m_showOnlyFilteredEntities && !EntityMatchesFilter(row))
        {
            ++m_entitiesFiltered;
            continue;
        }

        if (static_cast<int>(m_entityRows.size()) < maxRows)
            m_entityRows.push_back(std::move(row));
    }

    m_lastStatus =
        "entity scan OK level=" + m_lastLevelName +
        " rows=" + std::to_string(m_entityRows.size()) +
        " seen=" + std::to_string(m_entitiesSeen);
    WriteStatusFile();
    return true;
}

bool CoopRuntimeExtractor::ScanClassRegistry()
{
    ++m_classScanCount;
    m_classRows.clear();

    if (!gEnv || !gEnv->pEntitySystem || !IsLikelyRuntimeCppObject(gEnv->pEntitySystem))
    {
        m_lastStatus = "class scan failed: no valid entity system";
        WriteStatusFile();
        return false;
    }

    std::string guardReason;
    IEntityClassRegistry* registry = nullptr;
    if (!TryGuardedCall("IEntitySystem::GetClassRegistry", []() { return gEnv->pEntitySystem->GetClassRegistry(); }, registry, &guardReason))
    {
        ++m_guardSkips;
        m_lastGuardFailure = guardReason;
        m_lastStatus = "class scan failed: guarded registry call failed";
        WriteStatusFile();
        return false;
    }
    if (!IsLikelyRuntimeCppObject(registry))
    {
        ++m_guardSkips;
        m_lastStatus = "class scan failed: invalid registry";
        WriteStatusFile();
        return false;
    }

    const int maxRows = std::max(1, m_maxClassRows);
    if (!TryGuardedVoidCall("IEntityClassRegistry::IteratorMoveFirst", [registry]() { registry->IteratorMoveFirst(); }, &guardReason))
    {
        ++m_guardSkips;
        m_lastGuardFailure = guardReason;
        m_lastStatus = "class scan failed: guarded IteratorMoveFirst failed";
        WriteStatusFile();
        return false;
    }

    while (true)
    {
        IEntityClass* entityClass = nullptr;
        if (!TryGuardedCall("IEntityClassRegistry::IteratorNext", [registry]() { return registry->IteratorNext(); }, entityClass, &guardReason))
        {
            ++m_guardSkips;
            m_lastGuardFailure = guardReason;
            break;
        }
        if (!entityClass)
            break;

        if (!IsLikelyRuntimeCppObject(entityClass))
        {
            ++m_guardSkips;
            continue;
        }

        ClassRow row;
        row.classPtr = reinterpret_cast<std::uintptr_t>(entityClass);
        const char* classNamePtr = nullptr;
        const char* scriptFilePtr = nullptr;
        if (TryGuardedCall("IEntityClass::GetName", [entityClass]() { return entityClass->GetName(); }, classNamePtr, &guardReason))
            row.name = ReadRuntimeCString(classNamePtr, 160);
        else
            m_lastGuardFailure = guardReason;
        if (TryGuardedCall("IEntityClass::GetScriptFile", [entityClass]() { return entityClass->GetScriptFile(); }, scriptFilePtr, &guardReason))
            row.scriptFile = ReadRuntimeCString(scriptFilePtr, 260);
        else
            m_lastGuardFailure = guardReason;
        if (!TryGuardedCall("IEntityClass::GetFlags", [entityClass]() { return entityClass->GetFlags(); }, row.flags, &guardReason))
            m_lastGuardFailure = guardReason;
        if (row.name.empty())
            row.name = "-";
        if (row.scriptFile.empty())
            row.scriptFile = "-";

        if (static_cast<int>(m_classRows.size()) < maxRows)
            m_classRows.push_back(std::move(row));
    }

    std::sort(m_classRows.begin(), m_classRows.end(), [](const ClassRow& lhs, const ClassRow& rhs) {
        return lhs.name < rhs.name;
    });

    m_lastStatus = "class scan OK rows=" + std::to_string(m_classRows.size());
    WriteStatusFile();
    return true;
}

bool CoopRuntimeExtractor::ProbeExtensionAcrossSnapshot(const std::string& extensionName)
{
    ++m_extensionProbeCount;
    m_extensionRows.clear();
    m_extensionMisses = 0;

    const std::string trimmedName = Trim(extensionName);
    if (trimmedName.empty())
    {
        m_lastStatus = "extension probe failed: empty name";
        WriteStatusFile();
        return false;
    }

    if (m_entityRows.empty() && !ScanEntities())
        return false;

    std::string guardReason;
    IGameFramework* framework = nullptr;
    if (!gEnv || !gEnv->pGame ||
        !TryGuardedCall("IGame::GetIGameFramework", []() { return gEnv->pGame->GetIGameFramework(); }, framework, &guardReason) ||
        !IsLikelyRuntimeCppObject(framework))
    {
        if (!guardReason.empty())
            m_lastGuardFailure = guardReason;
        m_lastStatus = "extension probe failed: no valid game framework";
        WriteStatusFile();
        return false;
    }

    const int maxRows = std::max(1, m_maxExtensionProbeRows);
    for (const EntityRow& entityRow : m_entityRows)
    {
        IGameObjectExtension* extension = nullptr;
        if (!TryGuardedCall(
            "IGameFramework::QueryGameObjectExtension",
            [framework, &entityRow, &trimmedName]() { return framework->QueryGameObjectExtension(entityRow.entityId, trimmedName.c_str()); },
            extension,
            &guardReason))
        {
            ++m_guardSkips;
            m_lastGuardFailure = guardReason;
            continue;
        }
        if (!extension)
        {
            ++m_extensionMisses;
            continue;
        }

        ExtensionProbeRow row;
        row.entityId = entityRow.entityId;
        row.entityName = entityRow.name;
        row.className = entityRow.className;
        row.extensionName = trimmedName;
        row.extensionPtr = reinterpret_cast<std::uintptr_t>(extension);
        row.likelyObject = IsLikelyRuntimeCppObject(extension);

        const void* vtable = nullptr;
        if (TryReadRuntimeValue(reinterpret_cast<const void* const*>(extension), vtable))
        {
            row.vtablePtr = reinterpret_cast<std::uintptr_t>(vtable);
            row.modulePath = GetRuntimeModulePath(vtable);
        }
        if (row.modulePath.empty())
            row.modulePath = "-";

        if (static_cast<int>(m_extensionRows.size()) < maxRows)
            m_extensionRows.push_back(std::move(row));
    }

    m_lastStatus =
        "extension probe OK " + trimmedName +
        " found=" + std::to_string(m_extensionRows.size()) +
        " misses=" + std::to_string(m_extensionMisses);
    WriteStatusFile();
    return true;
}

bool CoopRuntimeExtractor::ExportJsonl()
{
    ++m_exportCount;
    const std::filesystem::path exportPath(GetLatestExportPath());
    std::error_code error;
    std::filesystem::create_directories(exportPath.parent_path(), error);
    if (error)
    {
        m_lastStatus = "export failed: create dir " + error.message();
        WriteStatusFile();
        return false;
    }

    std::ofstream output(exportPath, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        m_lastStatus = "export failed: open latest.jsonl";
        WriteStatusFile();
        return false;
    }

    output
        << "{\"kind\":\"meta\""
        << ",\"level\":\"" << JsonEscape(m_lastLevelName) << "\""
        << ",\"entities\":" << m_entityRows.size()
        << ",\"classes\":" << m_classRows.size()
        << ",\"extensions\":" << m_extensionRows.size()
        << ",\"guardSkips\":" << m_guardSkips
        << "}\n";

    for (const EntityRow& row : m_entityRows)
    {
        output
            << "{\"kind\":\"entity\""
            << ",\"id\":" << row.entityId
            << ",\"guid\":" << row.guid
            << ",\"name\":\"" << JsonEscape(row.name) << "\""
            << ",\"class\":\"" << JsonEscape(row.className) << "\""
            << ",\"archetype\":\"" << JsonEscape(row.archetypeName) << "\""
            << ",\"archetypeId\":" << row.archetypeId
            << ",\"x\":" << row.position.x
            << ",\"y\":" << row.position.y
            << ",\"z\":" << row.position.z
            << ",\"flags\":" << row.flags
            << ",\"loadedFromLevel\":" << (row.loadedFromLevel ? "true" : "false")
            << ",\"garbage\":" << (row.garbage ? "true" : "false")
            << ",\"entityPtr\":\"" << PointerHex(row.entityPtr) << "\""
            << ",\"classPtr\":\"" << PointerHex(row.classPtr) << "\""
            << ",\"archetypePtr\":\"" << PointerHex(row.archetypePtr) << "\""
            << "}\n";
    }

    for (const ClassRow& row : m_classRows)
    {
        output
            << "{\"kind\":\"class\""
            << ",\"name\":\"" << JsonEscape(row.name) << "\""
            << ",\"script\":\"" << JsonEscape(row.scriptFile) << "\""
            << ",\"flags\":" << row.flags
            << ",\"classPtr\":\"" << PointerHex(row.classPtr) << "\""
            << "}\n";
    }

    for (const ExtensionProbeRow& row : m_extensionRows)
    {
        output
            << "{\"kind\":\"extension\""
            << ",\"entityId\":" << row.entityId
            << ",\"entity\":\"" << JsonEscape(row.entityName) << "\""
            << ",\"class\":\"" << JsonEscape(row.className) << "\""
            << ",\"extension\":\"" << JsonEscape(row.extensionName) << "\""
            << ",\"likelyObject\":" << (row.likelyObject ? "true" : "false")
            << ",\"extensionPtr\":\"" << PointerHex(row.extensionPtr) << "\""
            << ",\"vtablePtr\":\"" << PointerHex(row.vtablePtr) << "\""
            << ",\"module\":\"" << JsonEscape(row.modulePath) << "\""
            << "}\n";
    }

    m_lastExportPath = exportPath.string();
    m_lastStatus = "export OK " + m_lastExportPath;
    WriteStatusFile();
    LogExtractor(m_lastStatus);
    return true;
}

void CoopRuntimeExtractor::ProcessCommandFile(float frameTime, bool controlOnly)
{
    m_commandPollAccumulator += frameTime;
    if (m_commandPollAccumulator < kCommandPollSeconds)
        return;
    m_commandPollAccumulator = 0.0f;

    const std::filesystem::path commandPath(GetCommandFilePath());
    std::error_code error;
    if (!std::filesystem::is_regular_file(commandPath, error) || error)
        return;

    const std::string text = Trim(ReadWholeFile(commandPath));
    if (text.empty() || text == m_lastCommandText)
        return;

    m_lastCommandText = text;
    ProcessCommandText(text, controlOnly);
}

void CoopRuntimeExtractor::ProcessCommandText(const std::string& text, bool controlOnly)
{
    std::istringstream lines(text);
    std::string line;
    bool ok = true;
    uint32_t commandCount = 0;
    while (std::getline(lines, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == '#')
            continue;

        ++commandCount;
        std::istringstream parts(line);
        std::string command;
        parts >> command;
        command = ToLowerAscii(command);

        if (controlOnly && command.rfind("coop_", 0) != 0)
        {
            ok = false;
            m_lastStatus = "deferred unsafe extractor command during load: " + command;
            continue;
        }

        if (command == "scan" || command == "scan_entities")
        {
            std::string filter;
            std::getline(parts, filter);
            filter = Trim(filter);
            m_entityFilter = filter;
            ok = ScanEntities() && ok;
        }
        else if (command == "scan_classes")
        {
            ok = ScanClassRegistry() && ok;
        }
        else if (command == "probe_extension")
        {
            std::string extensionName;
            std::getline(parts, extensionName);
            extensionName = Trim(extensionName);
            if (!extensionName.empty())
                m_extensionProbeName = extensionName;
            ok = ProbeExtensionAcrossSnapshot(m_extensionProbeName) && ok;
        }
        else if (command == "export" || command == "export_jsonl")
        {
            if (m_entityRows.empty())
                ScanEntities();
            ok = ExportJsonl() && ok;
        }
        else if (command == "clear")
        {
            ClearSnapshots("command clear");
        }
        else
        {
            std::vector<std::string> args;
            std::string arg;
            while (parts >> arg)
                args.push_back(arg);

            std::string handlerStatus;
            if (m_controlCommandHandler)
            {
                const bool handled = m_controlCommandHandler(command, args, handlerStatus);
                ok = handled && ok;
                if (!handlerStatus.empty())
                    m_lastStatus = handlerStatus;
                else if (!handled)
                    m_lastStatus = "coop control command failed: " + line;
            }
            else
            {
                ok = false;
                m_lastStatus = "unknown extractor command: " + line;
            }
        }
    }

    m_lastCommandStatus =
        std::string(ok ? "OK " : "FAILED ") +
        std::to_string(commandCount) +
        " command(s)";
    WriteStatusFile();
    LogExtractor(m_lastCommandStatus);
}

void CoopRuntimeExtractor::WriteStatusFile() const
{
    const std::filesystem::path statusPath(GetStatusFilePath());
    std::error_code error;
    std::filesystem::create_directories(statusPath.parent_path(), error);
    if (error)
        return;

    std::ofstream output(statusPath, std::ios::binary | std::ios::trunc);
    if (!output)
        return;

    output
        << "status=" << m_lastStatus << "\n"
        << "command=" << m_lastCommandStatus << "\n"
        << "level=" << m_lastLevelName << "\n"
        << "entities=" << m_entityRows.size() << "\n"
        << "classes=" << m_classRows.size() << "\n"
        << "extensions=" << m_extensionRows.size() << "\n"
        << "guardSkips=" << m_guardSkips << "\n"
        << "export=" << m_lastExportPath << "\n";
}

void CoopRuntimeExtractor::ClearSnapshots(const char* reason)
{
    m_entityRows.clear();
    m_classRows.clear();
    m_extensionRows.clear();
    m_entitiesSeen = 0;
    m_entitiesFiltered = 0;
    m_extensionMisses = 0;
    m_lastStatus = std::string("snapshots cleared: ") + (reason && reason[0] ? reason : "-");
    WriteStatusFile();
}

void CoopRuntimeExtractor::DrawEntityRows()
{
    ImGui::BeginChild("CoopRuntimeExtractorEntities", ImVec2(0.0f, 180.0f), true);
    for (size_t i = 0; i < m_entityRows.size(); ++i)
    {
        const EntityRow& row = m_entityRows[i];
        ImGui::Text(
            "#%zu id=%u guid=%llu class=%s name=%s arch=%s/%llu pos=(%.1f %.1f %.1f) flags=0x%08X%s%s",
            i,
            row.entityId,
            static_cast<unsigned long long>(row.guid),
            row.className.c_str(),
            row.name.c_str(),
            row.archetypeName.c_str(),
            static_cast<unsigned long long>(row.archetypeId),
            row.position.x,
            row.position.y,
            row.position.z,
            row.flags,
            row.loadedFromLevel ? " level" : "",
            row.garbage ? " garbage" : "");
    }
    ImGui::EndChild();
}

void CoopRuntimeExtractor::DrawClassRows()
{
    ImGui::BeginChild("CoopRuntimeExtractorClasses", ImVec2(0.0f, 180.0f), true);
    for (size_t i = 0; i < m_classRows.size(); ++i)
    {
        const ClassRow& row = m_classRows[i];
        ImGui::Text(
            "#%zu %s flags=0x%08X script=%s ptr=%s",
            i,
            row.name.c_str(),
            row.flags,
            row.scriptFile.c_str(),
            PointerHex(row.classPtr).c_str());
    }
    ImGui::EndChild();
}

void CoopRuntimeExtractor::DrawExtensionRows()
{
    ImGui::BeginChild("CoopRuntimeExtractorExtensions", ImVec2(0.0f, 180.0f), true);
    for (size_t i = 0; i < m_extensionRows.size(); ++i)
    {
        const ExtensionProbeRow& row = m_extensionRows[i];
        ImGui::Text(
            "#%zu id=%u %s entity=%s class=%s ext=%s ptr=%s vtable=%s module=%s",
            i,
            row.entityId,
            row.likelyObject ? "ok" : "guard",
            row.entityName.c_str(),
            row.className.c_str(),
            row.extensionName.c_str(),
            PointerHex(row.extensionPtr).c_str(),
            PointerHex(row.vtablePtr).c_str(),
            row.modulePath.c_str());
    }
    ImGui::EndChild();
}
