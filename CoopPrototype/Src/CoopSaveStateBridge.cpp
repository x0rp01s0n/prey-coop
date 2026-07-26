#include "CoopSaveStateBridge.h"

#include "CoopNativeFragmentPayload.h"
#include "CoopNativeSideBlob.h"
#include "CoopRuntimeGuards.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>

namespace
{
using CoopRuntimeGuards::GetRuntimeModulePath;
using CoopRuntimeGuards::PreflightRuntimePointer;
using CoopRuntimeGuards::ReadRuntimeCString;
using CoopRuntimeGuards::RuntimeAccess;
using CoopRuntimeGuards::TryGuardedCall;
using CoopRuntimeGuards::TryReadRuntimeValue;

thread_local std::vector<uint64_t> g_saveTraceStack;

std::string PointerHex(const void* ptr)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << reinterpret_cast<std::uintptr_t>(ptr);
    return out.str();
}

std::string PathFileNameOnly(std::string path)
{
    const size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos)
        return path.substr(slash + 1);
    return path;
}

std::string StatusToken(std::string value)
{
    if (value.empty())
        return "-";

    for (char& ch : value)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isspace(c) || ch == '"' || ch == '\'' || ch == '\\')
            ch = '_';
    }
    return value;
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

std::string ThreadIdText()
{
    std::ostringstream out;
    out << std::hash<std::thread::id>{}(std::this_thread::get_id());
    return out.str();
}

std::string PointerJson(const void* ptr)
{
    return ptr ? PointerHex(ptr) : std::string("0x0");
}

std::filesystem::path GetAtlasRootFromEnvironment()
{
    if (const char* overrideRoot = std::getenv("COOP_SAVE_ATLAS_ROOT"); overrideRoot && overrideRoot[0])
        return std::filesystem::path(overrideRoot);

    if (const char* userProfile = std::getenv("USERPROFILE"); userProfile && userProfile[0])
    {
        std::filesystem::path root(userProfile);
        root /= "Saved Games";
        root /= "Arkane Studios";
        root /= "Prey";
        root /= "CoopPrototype";
        root /= "SaveAtlas";
        return root;
    }

    return std::filesystem::path("CoopPrototype") / "SaveAtlas";
}

std::string MakeAtlasRunId()
{
    const auto now = std::chrono::system_clock::now();
    const auto ticks = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::ostringstream out;
    out << "run_" << ticks << "_t" << ThreadIdText();
    return out.str();
}

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool IsActiveApi(CoopSaveStateBridge::SectionApi api)
{
    return api == CoopSaveStateBridge::SectionApi::ActiveSave ||
        api == CoopSaveStateBridge::SectionApi::ActiveLoad;
}
}

void CoopSaveStateBridge::Reset()
{
    m_sequence = 0;
    m_atlasRunStarted = false;
    m_atlasRunId.clear();
    m_atlasRootPath.clear();
    m_lastAtlasEvent = "-";
    m_atlasEvents = 0;
    m_atlasSectionEvents = 0;
    m_atlasSerializerOps = 0;
    m_atlasStoreProbes = 0;
    m_atlasUnknownOps = 0;
    m_atlasSemanticBuckets = {};
    m_atlasBoundaryProbeKeys.clear();
    m_serializerSections.clear();
    g_saveTraceStack.clear();
}

uint64_t CoopSaveStateBridge::NextSequence()
{
    return ++m_sequence;
}

CoopSaveStateBridge::SerializerFingerprint CoopSaveStateBridge::BuildSerializerFingerprint(TSerialize serializer)
{
    SerializerFingerprint result;
    result.impl = GetImpl(serializer);

    const void* vtable = nullptr;
    bool guarded = false;
    std::string reason;
    if (result.impl)
    {
        if (!TryReadRuntimeValue(reinterpret_cast<const void* const*>(result.impl), vtable))
        {
            guarded = true;
            reason = "vtable_read_failed";
        }
    }
    else
    {
        guarded = true;
        reason = "null_impl";
    }

    result.vtable = vtable;
    const void* modulePtr = vtable ? vtable : static_cast<const void*>(result.impl);
    result.text =
        "ser=" + PointerHex(result.impl) +
        "/vt=" + PointerHex(vtable) +
        "/mod=" + PathFileNameOnly(GetRuntimeModulePath(modulePtr));
    if (guarded)
        result.text += "/guard=" + reason;
    return result;
}

const char* CoopSaveStateBridge::ApiName(SectionApi api)
{
    switch (api)
    {
    case SectionApi::LegacySave:
        return "LegacySave";
    case SectionApi::LegacyLoad:
        return "LegacyLoad";
    case SectionApi::ActiveSave:
        return "ActiveSave";
    case SectionApi::ActiveLoad:
        return "ActiveLoad";
    default:
        return "Unknown";
    }
}

const char* CoopSaveStateBridge::SemanticName(SchemaSemantic semantic)
{
    switch (semantic)
    {
    case SchemaSemantic::Unknown:
        return "unknown";
    case SchemaSemantic::Section:
        return "section";
    case SchemaSemantic::GameState:
        return "game-state";
    case SchemaSemantic::PersistentState:
        return "persistent-state";
    case SchemaSemantic::PlayerCore:
        return "player";
    case SchemaSemantic::PlayerVitals:
        return "vitals";
    case SchemaSemantic::PlayerInventory:
        return "player-inventory";
    case SchemaSemantic::PlayerInventoryCell:
        return "player-inventory-cell";
    case SchemaSemantic::Inventory:
        return "inventory";
    case SchemaSemantic::InventoryCell:
        return "inventory-cell";
    case SchemaSemantic::Item:
        return "item";
    case SchemaSemantic::Weapon:
        return "weapon";
    case SchemaSemantic::Ability:
        return "ability";
    case SchemaSemantic::Neuromod:
        return "neuromod";
    case SchemaSemantic::Keycard:
        return "keycard";
    case SchemaSemantic::Keycode:
        return "keycode";
    case SchemaSemantic::FabricationPlan:
        return "fabrication-plan";
    case SchemaSemantic::Objective:
        return "objective";
    case SchemaSemantic::Status:
        return "status";
    case SchemaSemantic::WorldEntity:
        return "world-entity";
    case SchemaSemantic::Transform:
        return "transform";
    case SchemaSemantic::LevelState:
        return "level-state";
    case SchemaSemantic::Count:
        return "count";
    default:
        return "unknown";
    }
}

const char* CoopSaveStateBridge::PhaseName(TracePhase phase)
{
    switch (phase)
    {
    case TracePhase::None:
        return "none";
    case TracePhase::ConcreteLoadInitEnter:
        return "concrete-load-init-enter";
    case TracePhase::ConcreteLoadInitExitRootReady:
        return "concrete-load-init-exit-root-ready";
    case TracePhase::ConcreteSaveCompleteEnter:
        return "concrete-save-complete-enter";
    case TracePhase::ConcreteSaveCompleteExit:
        return "concrete-save-complete-exit";
    case TracePhase::ActiveLoadNotify:
        return "active-load-notify";
    case TracePhase::ActiveSaveNotify:
        return "active-save-notify";
    case TracePhase::LoadGetSectionEnter:
        return "load-get-section-enter";
    case TracePhase::LoadGetSectionExit:
        return "load-get-section-exit";
    case TracePhase::LoadHaveSectionEnter:
        return "load-have-section-enter";
    case TracePhase::LoadHaveSectionExit:
        return "load-have-section-exit";
    case TracePhase::LoadCompleteEnter:
        return "load-complete-enter";
    case TracePhase::LoadCompleteExit:
        return "load-complete-exit";
    case TracePhase::SaveAddSectionEnter:
        return "save-add-section-enter";
    case TracePhase::SaveAddSectionExit:
        return "save-add-section-exit";
    case TracePhase::ArkLoadCurrentLevelStateEnter:
        return "ark-load-current-level-state-enter";
    case TracePhase::ArkLoadCurrentLevelStateExit:
        return "ark-load-current-level-state-exit";
    case TracePhase::ArkSaveCurrentLevelStateEnter:
        return "ark-save-current-level-state-enter";
    case TracePhase::ArkSaveCurrentLevelStateExit:
        return "ark-save-current-level-state-exit";
    case TracePhase::ArkSerializePersistentStateEnter:
        return "ark-serialize-persistent-state-enter";
    case TracePhase::ArkSerializePersistentStateExit:
        return "ark-serialize-persistent-state-exit";
    case TracePhase::ArkPlayerSerializeEnter:
        return "ark-player-serialize-enter";
    case TracePhase::ArkPlayerSerializeExit:
        return "ark-player-serialize-exit";
    case TracePhase::ArkInventorySerializeEnter:
        return "ark-inventory-serialize-enter";
    case TracePhase::ArkInventorySerializeExit:
        return "ark-inventory-serialize-exit";
    case TracePhase::SerializerOp:
        return "serializer-op";
    case TracePhase::SectionEvent:
        return "section-event";
    default:
        return "unknown";
    }
}

bool CoopSaveStateBridge::IsPersistentStateSection(std::string_view sectionName)
{
    return ToLowerAscii(std::string(sectionName)).find("persistentstate") != std::string::npos;
}

bool CoopSaveStateBridge::IsGameStateSection(std::string_view sectionName)
{
    const std::string lower = ToLowerAscii(std::string(sectionName));
    return lower == "gamestate" || lower.find("gamestate") != std::string::npos;
}

CoopSaveStateBridge::SchemaSemantic CoopSaveStateBridge::ClassifySectionName(std::string_view sectionName)
{
    if (IsGameStateSection(sectionName))
        return SchemaSemantic::GameState;
    if (IsPersistentStateSection(sectionName))
        return SchemaSemantic::PersistentState;
    return SchemaSemantic::Section;
}

CoopSaveStateBridge::SchemaSemantic CoopSaveStateBridge::ClassifySerializerPath(
    std::string_view source,
    std::string_view path,
    std::string_view typeName,
    std::string_view op)
{
    (void)typeName;

    const std::string sourceLower = ToLowerAscii(std::string(source));
    const std::string pathLower = ToLowerAscii(std::string(path));
    const std::string opLower = ToLowerAscii(std::string(op));
    const bool localPlayerContext =
        sourceLower.find("local-player") != std::string::npos ||
        pathLower.find("local-player") != std::string::npos;
    const bool inventoryCellPath =
        pathLower.find("storeditems/i/v") != std::string::npos ||
        pathLower.find("entityid") != std::string::npos ||
        pathLower.find("/x") != std::string::npos ||
        pathLower.find("/y") != std::string::npos ||
        pathLower.find("/width") != std::string::npos ||
        pathLower.find("/height") != std::string::npos;

    if (pathLower.find("persistentstate") != std::string::npos ||
        sourceLower.find("persistentstate") != std::string::npos)
    {
        return SchemaSemantic::PersistentState;
    }

    if (opLower == "call-begin" || opLower == "call-end")
    {
        if (sourceLower.find("arkinventory") != std::string::npos && localPlayerContext)
            return SchemaSemantic::PlayerInventory;
        if (sourceLower.find("arkinventory") != std::string::npos)
            return SchemaSemantic::Inventory;
        if (sourceLower.find("arkplayer") != std::string::npos)
            return SchemaSemantic::PlayerCore;
        if (sourceLower.find("carkitem") != std::string::npos ||
            sourceLower.find("arkitem") != std::string::npos)
        {
            return SchemaSemantic::Item;
        }
    }

    if (localPlayerContext &&
        (sourceLower.find("arkinventory") != std::string::npos ||
            pathLower.rfind("inventory", 0) == 0))
    {
        return inventoryCellPath ? SchemaSemantic::PlayerInventoryCell : SchemaSemantic::PlayerInventory;
    }

    if (sourceLower.find("carkitem") != std::string::npos ||
        sourceLower.find("arkitem") != std::string::npos)
    {
        if (pathLower.find("weapon") != std::string::npos ||
            pathLower.find("ammo") != std::string::npos ||
            pathLower.find("condition") != std::string::npos ||
            pathLower.find("equipped") != std::string::npos)
        {
            return SchemaSemantic::Weapon;
        }
        return SchemaSemantic::Item;
    }

    if (sourceLower.find("arkinventory") != std::string::npos ||
        pathLower.rfind("inventory", 0) == 0)
    {
        if (inventoryCellPath)
            return SchemaSemantic::InventoryCell;
        return SchemaSemantic::Inventory;
    }

    if (pathLower.find("weapon") != std::string::npos ||
        pathLower.find("ammo") != std::string::npos ||
        pathLower.find("equipped") != std::string::npos)
    {
        return SchemaSemantic::Weapon;
    }

    if (pathLower.find("ability") != std::string::npos ||
        pathLower.find("neuromod") != std::string::npos ||
        pathLower.find("research") != std::string::npos ||
        pathLower.find("typhon") != std::string::npos)
    {
        if (pathLower.find("neuromod") != std::string::npos)
            return SchemaSemantic::Neuromod;
        return SchemaSemantic::Ability;
    }

    if (pathLower.find("keycard") != std::string::npos ||
        pathLower.find("key_card") != std::string::npos)
    {
        return SchemaSemantic::Keycard;
    }

    if (pathLower.find("keycode") != std::string::npos ||
        pathLower.find("key_code") != std::string::npos)
    {
        return SchemaSemantic::Keycode;
    }

    if (pathLower.find("fabrication") != std::string::npos ||
        pathLower.find("fabplan") != std::string::npos ||
        pathLower.find("plan") != std::string::npos)
    {
        return SchemaSemantic::FabricationPlan;
    }

    if (pathLower.find("objective") != std::string::npos ||
        pathLower.find("mission") != std::string::npos ||
        pathLower.find("quest") != std::string::npos)
    {
        return SchemaSemantic::Objective;
    }

    if (pathLower.find("health") != std::string::npos ||
        pathLower.find("psi") != std::string::npos ||
        pathLower.find("suit") != std::string::npos ||
        pathLower.find("armor") != std::string::npos)
    {
        return SchemaSemantic::PlayerVitals;
    }

    if (pathLower.find("status") != std::string::npos ||
        pathLower.find("trauma") != std::string::npos ||
        pathLower.find("radiation") != std::string::npos)
    {
        return SchemaSemantic::Status;
    }

    if (pathLower.find("pos") != std::string::npos ||
        pathLower.find("position") != std::string::npos ||
        pathLower.find("rot") != std::string::npos ||
        pathLower.find("rotation") != std::string::npos ||
        pathLower.find("quat") != std::string::npos ||
        pathLower.find("scale") != std::string::npos ||
        pathLower.find("transform") != std::string::npos)
    {
        return SchemaSemantic::Transform;
    }

    if (pathLower.find("entity") != std::string::npos ||
        pathLower.find("guid") != std::string::npos ||
        pathLower.find("archetype") != std::string::npos ||
        sourceLower.find("arkgame") != std::string::npos)
    {
        return SchemaSemantic::WorldEntity;
    }

    if (sourceLower.find("arkplayer") != std::string::npos ||
        pathLower.find("player") != std::string::npos)
    {
        return SchemaSemantic::PlayerCore;
    }

    if (sourceLower.find("carkitem") != std::string::npos ||
        sourceLower.find("arkitem") != std::string::npos ||
        pathLower.find("item") != std::string::npos)
    {
        return SchemaSemantic::Item;
    }

    return SchemaSemantic::Unknown;
}

bool CoopSaveStateBridge::IsAtlasEnabled() const
{
    const char* enabled = std::getenv("COOP_SAVE_ATLAS");
    return enabled && enabled[0] && std::atoi(enabled) != 0;
}

void CoopSaveStateBridge::EnsureAtlasRun()
{
    if (m_atlasRunStarted || !IsAtlasEnabled())
        return;

    m_atlasRunStarted = true;
    m_atlasRunId = MakeAtlasRunId();
    const std::filesystem::path runRoot = GetAtlasRootFromEnvironment() / m_atlasRunId;
    m_atlasRootPath = runRoot.string();

    std::error_code error;
    std::filesystem::create_directories(runRoot / "roots", error);
    std::filesystem::create_directories(runRoot / "sections", error);
    std::filesystem::create_directories(runRoot / "hashes", error);

    std::ofstream run(runRoot / "run.json", std::ios::binary | std::ios::trunc);
    if (run)
    {
        run
            << "{"
            << "\"run\":\"" << JsonEscape(m_atlasRunId) << "\","
            << "\"thread\":\"" << JsonEscape(ThreadIdText()) << "\","
            << "\"root\":\"" << JsonEscape(m_atlasRootPath) << "\""
            << "}\n";
    }

    m_lastAtlasEvent = "started_" + m_atlasRunId;
}

void CoopSaveStateBridge::WriteAtlasEvent(
    const char* kind,
    uint64_t sequence,
    uint64_t parentSequence,
    TracePhase phase,
    SchemaSemantic semantic,
    const char* label,
    const void* thisPtr,
    const void* serializerPtr,
    bool reading,
    int target,
    bool ok,
    int depth,
    const char* detail)
{
    if (!IsAtlasEnabled())
        return;

    EnsureAtlasRun();
    if (!m_atlasRunStarted)
        return;

    const size_t semanticIndex = static_cast<size_t>(semantic);
    if (semanticIndex < m_atlasSemanticBuckets.size())
        ++m_atlasSemanticBuckets[semanticIndex];
    if (semantic == SchemaSemantic::Unknown)
        ++m_atlasUnknownOps;

    ++m_atlasEvents;
    m_lastAtlasEvent = std::string(kind && kind[0] ? kind : "event") + "_" + PhaseName(phase);

    const std::filesystem::path runRoot(m_atlasRootPath);
    std::ofstream out(runRoot / "calltree.jsonl", std::ios::binary | std::ios::app);
    if (!out)
        return;

    out
        << "{"
        << "\"seq\":" << sequence << ","
        << "\"parent\":" << parentSequence << ","
        << "\"kind\":\"" << JsonEscape(kind && kind[0] ? kind : "event") << "\","
        << "\"phase\":\"" << PhaseName(phase) << "\","
        << "\"semantic\":\"" << SemanticName(semantic) << "\","
        << "\"label\":\"" << JsonEscape(label && label[0] ? label : "-") << "\","
        << "\"this\":\"" << PointerJson(thisPtr) << "\","
        << "\"serializer\":\"" << PointerJson(serializerPtr) << "\","
        << "\"reading\":" << (reading ? "true" : "false") << ","
        << "\"target\":" << target << ","
        << "\"ok\":" << (ok ? "true" : "false") << ","
        << "\"depth\":" << std::max(0, depth) << ","
        << "\"thread\":\"" << JsonEscape(ThreadIdText()) << "\","
        << "\"detail\":\"" << JsonEscape(detail && detail[0] ? detail : "") << "\""
        << "}\n";
}

void CoopSaveStateBridge::WriteAtlasSectionEvent(const SectionEvent& event)
{
    if (!IsAtlasEnabled())
        return;

    EnsureAtlasRun();
    if (!m_atlasRunStarted)
        return;

    const std::filesystem::path runRoot(m_atlasRootPath);
    std::ofstream out(runRoot / "sections.jsonl", std::ios::binary | std::ios::app);
    if (!out)
        return;

    out
        << "{"
        << "\"seq\":" << event.sequence << ","
        << "\"api\":\"" << ApiName(event.api) << "\","
        << "\"source\":\"" << JsonEscape(event.source) << "\","
        << "\"section\":\"" << JsonEscape(event.sectionName) << "\","
        << "\"reading\":" << (event.reading ? "true" : "false") << ","
        << "\"target\":" << event.target << ","
        << "\"ok\":" << (event.ok ? "true" : "false") << ","
        << "\"active\":" << (event.activeInterface ? "true" : "false") << ","
        << "\"gameState\":" << (event.gameState ? "true" : "false") << ","
        << "\"persistent\":" << (event.persistentState ? "true" : "false") << ","
        << "\"candidate\":" << (event.nativePatchCandidate ? "true" : "false") << ","
        << "\"serializer\":\"" << JsonEscape(event.serializerText) << "\""
        << "}\n";
}

void CoopSaveStateBridge::WriteAtlasSummary()
{
    if (!m_atlasRunStarted)
        return;

    const std::filesystem::path runRoot(m_atlasRootPath);
    std::ofstream out(runRoot / "schema_summary.txt", std::ios::binary | std::ios::trunc);
    if (!out)
        return;

    out << "run=" << m_atlasRunId << "\n";
    out << "events=" << m_atlasEvents << "\n";
    out << "sections=" << m_atlasSectionEvents << "\n";
    out << "serializerOps=" << m_atlasSerializerOps << "\n";
    out << "storeProbes=" << m_atlasStoreProbes << "\n";
    out << "unknownOps=" << m_atlasUnknownOps << "\n";
    for (size_t i = 0; i < m_atlasSemanticBuckets.size(); ++i)
    {
        out << SemanticName(static_cast<SchemaSemantic>(i)) << "=" << m_atlasSemanticBuckets[i] << "\n";
    }
    out << "last=" << m_lastAtlasEvent << "\n";
}

uint64_t CoopSaveStateBridge::BeginTracePhase(
    TracePhase phase,
    SchemaSemantic semantic,
    const char* label,
    const void* thisPtr,
    const void* serializerPtr,
    const char* detail,
    bool reading,
    int target,
    bool ok)
{
    const uint64_t sequence = NextSequence();
    const uint64_t parent = g_saveTraceStack.empty() ? 0 : g_saveTraceStack.back();
    WriteAtlasEvent("enter", sequence, parent, phase, semantic, label, thisPtr, serializerPtr, reading, target, ok, static_cast<int>(g_saveTraceStack.size()), detail);
    if (IsAtlasEnabled())
        g_saveTraceStack.push_back(sequence);
    return sequence;
}

void CoopSaveStateBridge::EndTracePhase(
    uint64_t enterSequence,
    TracePhase phase,
    SchemaSemantic semantic,
    const char* label,
    const void* thisPtr,
    const void* serializerPtr,
    const char* detail,
    bool reading,
    int target,
    bool ok)
{
    const uint64_t sequence = NextSequence();
    WriteAtlasEvent("exit", sequence, enterSequence, phase, semantic, label, thisPtr, serializerPtr, reading, target, ok, static_cast<int>(g_saveTraceStack.size()), detail);
    if (!g_saveTraceStack.empty())
    {
        if (g_saveTraceStack.back() == enterSequence)
            g_saveTraceStack.pop_back();
        else
            g_saveTraceStack.clear();
    }
    WriteAtlasSummary();
}

void CoopSaveStateBridge::RecordSectionEvent(const SectionEvent& event)
{
    if (!IsAtlasEnabled())
        return;

    ++m_atlasSectionEvents;
    const SchemaSemantic semantic =
        event.gameState ? SchemaSemantic::GameState :
        event.persistentState ? SchemaSemantic::PersistentState :
        SchemaSemantic::Section;
    WriteAtlasEvent(
        "section",
        event.sequence,
        g_saveTraceStack.empty() ? 0 : g_saveTraceStack.back(),
        TracePhase::SectionEvent,
        semantic,
        event.sectionName.c_str(),
        nullptr,
        event.fingerprint.impl,
        event.reading,
        event.target,
        event.ok,
        static_cast<int>(g_saveTraceStack.size()),
        event.source.c_str());
    WriteAtlasSectionEvent(event);
    WriteAtlasSummary();
}

void CoopSaveStateBridge::RecordSerializerOp(
    const char* source,
    const char* op,
    const char* path,
    const char* typeName,
    SchemaSemantic semantic,
    bool reading,
    int depth,
    const char* detail)
{
    if (!IsAtlasEnabled())
        return;

    ++m_atlasSerializerOps;
    const std::string label =
        std::string(source && source[0] ? source : "-") +
        ":" +
        std::string(op && op[0] ? op : "-") +
        ":" +
        std::string(path && path[0] ? path : "-") +
        ":" +
        std::string(typeName && typeName[0] ? typeName : "-");
    WriteAtlasEvent(
        "serializer",
        NextSequence(),
        g_saveTraceStack.empty() ? 0 : g_saveTraceStack.back(),
        TracePhase::SerializerOp,
        semantic,
        label.c_str(),
        nullptr,
        nullptr,
        reading,
        eST_SaveGame,
        true,
        depth,
        detail);
}

void CoopSaveStateBridge::RecordSerializerStoreProbe(
    const char* source,
    const char* op,
    const char* path,
    const void* serializerPtr,
    const char* detail)
{
    if (!IsAtlasEnabled())
        return;

    const char* enabled = std::getenv("COOP_SAVE_ATLAS_STORE_PROBES");
    if (!enabled || !enabled[0] || std::atoi(enabled) == 0)
        return;

    EnsureAtlasRun();
    if (!m_atlasRunStarted)
        return;

    ++m_atlasStoreProbes;
    const uint64_t sequence = NextSequence();
    const uint64_t parent = g_saveTraceStack.empty() ? 0 : g_saveTraceStack.back();
    const std::string section = FindSectionForSerializerImpl(serializerPtr);

    const std::filesystem::path runRoot(m_atlasRootPath);
    std::ofstream out(runRoot / "store_probes.jsonl", std::ios::binary | std::ios::app);
    if (!out)
        return;

    out
        << "{"
        << "\"seq\":" << sequence << ","
        << "\"parent\":" << parent << ","
        << "\"source\":\"" << JsonEscape(source && source[0] ? source : "-") << "\","
        << "\"op\":\"" << JsonEscape(op && op[0] ? op : "-") << "\","
        << "\"path\":\"" << JsonEscape(path && path[0] ? path : "-") << "\","
        << "\"section\":\"" << JsonEscape(section.empty() ? std::string("-") : section) << "\","
        << "\"serializer\":\"" << PointerJson(serializerPtr) << "\","
        << "\"thread\":\"" << JsonEscape(ThreadIdText()) << "\","
        << "\"detail\":\"" << JsonEscape(detail && detail[0] ? detail : "") << "\""
        << "}\n";

    m_lastAtlasEvent = "store_probe";
    WriteAtlasSummary();
}

bool CoopSaveStateBridge::ShouldRecordBoundaryProbe(
    const char* label,
    const char* sectionName,
    bool result)
{
    if (!IsAtlasEnabled())
        return false;

    std::string key = label && label[0] ? label : "-";
    key.push_back('|');
    key += sectionName && sectionName[0] ? sectionName : "-";
    key.push_back('|');
    key += result ? "1" : "0";
    return m_atlasBoundaryProbeKeys.insert(key).second;
}

CoopSaveStateBridge::SectionEvent CoopSaveStateBridge::ObserveSection(
    SectionApi api,
    const char* source,
    const char* rawSectionName,
    TSerialize* serializer,
    bool fallbackReading,
    int fallbackTarget,
    bool fallbackOk)
{
    SectionEvent event;
    event.sequence = NextSequence();
    event.api = api;
    event.source = source ? ReadRuntimeCString(source, 128) : std::string();
    if (event.source.empty())
        event.source = ApiName(api);

    event.sectionName = rawSectionName ? ReadRuntimeCString(rawSectionName, 160) : std::string();
    if (event.sectionName.empty())
        event.sectionName = "-";

    event.reading = fallbackReading;
    event.target = fallbackTarget;
    event.ok = fallbackOk;
    event.activeInterface = IsActiveApi(api);
    event.gameState = IsGameStateSection(event.sectionName);
    event.persistentState = IsPersistentStateSection(event.sectionName);
    event.serializerText = "ser=null";

    if (serializer)
    {
        std::string guardReason;
        if (PreflightRuntimePointer(
                "coop save-state bridge serializer",
                serializer,
                sizeof(TSerialize),
                RuntimeAccess::Read,
                &guardReason))
        {
            event.fingerprint = BuildSerializerFingerprint(*serializer);
            event.serializerText = event.fingerprint.text;
            bool serializerReadable =
                event.fingerprint.impl &&
                event.fingerprint.vtable &&
                CoopRuntimeGuards::IsLikelyRuntimeCppObject(event.fingerprint.impl);
            if (!serializerReadable)
                guardReason = "invalid_serializer_impl";
            if (serializerReadable)
            {
                serializerReadable = TryGuardedCall(
                    "save-state bridge serializer IsReading",
                    [serializer]() { return serializer->IsReading(); },
                    event.reading,
                    &guardReason);
            }
            if (serializerReadable)
            {
                serializerReadable = TryGuardedCall(
                    "save-state bridge serializer target",
                    [serializer]() { return static_cast<int>(serializer->GetSerializationTarget()); },
                    event.target,
                    &guardReason);
            }
            if (serializerReadable)
            {
                serializerReadable = TryGuardedCall(
                    "save-state bridge serializer Ok",
                    [serializer]() { return serializer->Ok(); },
                    event.ok,
                    &guardReason);
            }
            event.hasSerializer = serializerReadable;
            if (!serializerReadable)
                event.serializerText += "/guard=" + StatusToken(guardReason);
            if (event.hasSerializer && event.fingerprint.impl && event.sectionName != "-")
            {
                m_serializerSections[reinterpret_cast<std::uintptr_t>(event.fingerprint.impl)] = event.sectionName;
            }
        }
        else
        {
            event.serializerText = "ser=guard:" + StatusToken(guardReason);
        }
    }

    event.nativePatchCandidate =
        event.activeInterface &&
        (event.gameState || event.persistentState) &&
        event.hasSerializer &&
        event.target == eST_SaveGame;

    return event;
}

CoopSaveStateBridge::SectionEvent CoopSaveStateBridge::ObserveLoadUniquePtrSection(
    SectionApi api,
    const char* source,
    const char* rawSectionName,
    std::unique_ptr<TSerialize>* serializerOwner)
{
    TSerialize* serializer = nullptr;
    if (serializerOwner)
    {
        std::string guardReason;
        if (PreflightRuntimePointer(
                "coop save-state bridge unique_ptr",
                serializerOwner,
                sizeof(*serializerOwner),
                RuntimeAccess::Read,
                &guardReason))
        {
            serializer = serializerOwner->get();
        }
    }

    return ObserveSection(
        api,
        source,
        rawSectionName,
        serializer,
        true,
        static_cast<int>(eST_SaveGame),
        false);
}

std::string CoopSaveStateBridge::FindSectionForSerializer(TSerialize serializer) const
{
    const SerializerFingerprint fingerprint = BuildSerializerFingerprint(serializer);
    return FindSectionForSerializerImpl(fingerprint.impl);
}

std::string CoopSaveStateBridge::FindSectionForSerializerImpl(const void* serializerImpl) const
{
    if (!serializerImpl)
        return std::string();

    const auto it = m_serializerSections.find(reinterpret_cast<std::uintptr_t>(serializerImpl));
    if (it == m_serializerSections.end())
        return std::string();
    return it->second;
}

CoopSaveStateBridge::CoopSaveMergeResult CoopSaveStateBridge::MergeCoopSave(
    NativeSaveSectionState& vanillaSave,
    const CoopPlayerSaveSection& coopSection)
{
    CoopSaveMergeResult result;
    result.attempted = true;
    result.patchedState = vanillaSave.serializer;
    result.candidate =
        vanillaSave.gameState &&
        vanillaSave.serializer != nullptr &&
        vanillaSave.reading &&
        vanillaSave.target == eST_SaveGame &&
        vanillaSave.ok;

    if (!result.candidate)
    {
        result.reason = "not_candidate";
        result.detail =
            "section=" + StatusToken(vanillaSave.sectionName.empty() ? std::string("-") : vanillaSave.sectionName) +
            " gameState=" + std::to_string(vanillaSave.gameState ? 1 : 0) +
            " serializer=" + std::to_string(vanillaSave.serializer ? 1 : 0) +
            " reading=" + std::to_string(vanillaSave.reading ? 1 : 0) +
            " target=" + std::to_string(vanillaSave.target) +
            " ok=" + std::to_string(vanillaSave.ok ? 1 : 0);
        return result;
    }

    if (!coopSection.hasNativeCapture || !coopSection.nativeCapture)
    {
        result.reason = "missing_coop_section";
        result.detail =
            "section=" + StatusToken(vanillaSave.sectionName) +
            " user=" + StatusToken(coopSection.username.empty() ? std::string("-") : coopSection.username) +
            " saveKey=" + StatusToken(coopSection.saveKey.empty() ? std::string("-") : coopSection.saveKey);
        return result;
    }

    const NativeSideBlobCaptureState& capture = *coopSection.nativeCapture;
    const bool captureUsable =
        capture.sawPlayerWrite ||
        capture.sawInventoryWrite ||
        capture.sawItemWrite ||
        capture.hasNativeFragmentPayload ||
        !capture.items.empty();
    if (!captureUsable)
    {
        result.reason = "empty_coop_section";
        result.detail =
            "section=" + StatusToken(vanillaSave.sectionName) +
            " user=" + StatusToken(coopSection.username.empty() ? std::string("-") : coopSection.username) +
            " saw=" + std::to_string(capture.sawPlayerWrite ? 1 : 0) +
            std::to_string(capture.sawInventoryWrite ? 1 : 0) +
            std::to_string(capture.sawItemWrite ? 1 : 0) +
            " items=" + std::to_string(capture.items.size()) +
            " nativeFragment=" + std::to_string(capture.hasNativeFragmentPayload ? 1 : 0);
        return result;
    }

    CoopNativeFragmentPayload::ParsedPayload nativeFragmentPayload;
    if (capture.hasNativeFragmentPayload || !capture.nativeFragmentPayload.empty())
    {
        nativeFragmentPayload = CoopNativeFragmentPayload::ParseInventoryPayload(capture.nativeFragmentPayload);
        if (!nativeFragmentPayload.ok)
        {
            result.reason = "native_fragment_payload_invalid";
            result.detail =
                "section=" + StatusToken(vanillaSave.sectionName) +
                " user=" + StatusToken(coopSection.username.empty() ? std::string("-") : coopSection.username) +
                " " + CoopNativeFragmentPayload::BuildStatus(nativeFragmentPayload);
            return result;
        }
    }

    if (!vanillaSave.allowMutation)
    {
        result.reason = nativeFragmentPayload.ok
            ? "readonly_native_fragment_payload_ready"
            : "readonly_locator_not_ready";
        result.detail =
            "section=" + StatusToken(vanillaSave.sectionName) +
            " user=" + StatusToken(coopSection.username.empty() ? std::string("-") : coopSection.username) +
            " level=" + StatusToken(coopSection.levelName.empty() ? std::string("-") : coopSection.levelName) +
            " saveKey=" + StatusToken(coopSection.saveKey.empty() ? std::string("-") : coopSection.saveKey) +
            " saw=" + std::to_string(capture.sawPlayerWrite ? 1 : 0) +
            std::to_string(capture.sawInventoryWrite ? 1 : 0) +
            std::to_string(capture.sawItemWrite ? 1 : 0) +
            " items=" + std::to_string(capture.items.size()) +
            " nativeFragment=" + CoopNativeFragmentPayload::BuildStatus(nativeFragmentPayload) +
            " nativeFragTarget=pending" +
            " nativeFragImportPlan=pending" +
            " nativeFragPatch=disabled" +
            " serializer=" + vanillaSave.fingerprint.text;
        return result;
    }

    const bool payloadHasBacking =
        nativeFragmentPayload.ok &&
        nativeFragmentPayload.backingRanges != 0;

    result.passthrough = true;
    if (payloadHasBacking)
    {
        result.deferred = true;
        result.reason = "deferred_waiting_for_import_plan";
    }
    else
    {
        result.failed = true;
        result.reason = "mutation_blocked_payload_not_rebased";
    }
    result.detail =
        "section=" + StatusToken(vanillaSave.sectionName) +
        " user=" + StatusToken(coopSection.username.empty() ? std::string("-") : coopSection.username) +
        " items=" + std::to_string(capture.items.size()) +
        " nativeFragment=" + CoopNativeFragmentPayload::BuildStatus(nativeFragmentPayload) +
        " nativeFragTarget=pending" +
        " nativeFragImportPlan=pending" +
        " nativeFragPatch=" + std::string(result.deferred ? "deferred" : "blocked") +
        " parsedRanges=" + std::to_string(nativeFragmentPayload.ranges.size()) +
        " rawData=" + std::to_string(nativeFragmentPayload.rawData.size()) +
        " rawOffset=" + std::to_string(nativeFragmentPayload.rawDataOffset);
    return result;
}

std::string CoopSaveStateBridge::BuildCompactStatus(const SectionEvent& event)
{
    return
        "save_state_bridge"
        " seq=" + std::to_string(event.sequence) +
        " api=" + ApiName(event.api) +
        " source=" + StatusToken(event.source) +
        " section=" + StatusToken(event.sectionName) +
        " reading=" + std::to_string(event.reading ? 1 : 0) +
        " target=" + std::to_string(event.target) +
        " ok=" + std::to_string(event.ok ? 1 : 0) +
        " ser=" + std::to_string(event.hasSerializer ? 1 : 0) +
        " gameState=" + std::to_string(event.gameState ? 1 : 0) +
        " persistent=" + std::to_string(event.persistentState ? 1 : 0) +
        " candidate=" + std::to_string(event.nativePatchCandidate ? 1 : 0);
}

std::string CoopSaveStateBridge::BuildCompactStatus(const CoopSaveMergeResult& result)
{
    return
        "merge_coop_save"
        " attempted=" + std::to_string(result.attempted ? 1 : 0) +
        " candidate=" + std::to_string(result.candidate ? 1 : 0) +
        " patched=" + std::to_string(result.patched ? 1 : 0) +
        " passthrough=" + std::to_string(result.passthrough ? 1 : 0) +
        " deferred=" + std::to_string(result.deferred ? 1 : 0) +
        " failed=" + std::to_string(result.failed ? 1 : 0) +
        " reason=" + StatusToken(result.reason.empty() ? std::string("-") : result.reason) +
        " detail=" + StatusToken(result.detail.empty() ? std::string("-") : result.detail);
}

std::string CoopSaveStateBridge::BuildAtlasStatus() const
{
    if (!IsAtlasEnabled())
        return "0/-/0/0/0/0/-";

    return
        std::string(m_atlasRunStarted ? "1" : "0") +
        "/" + (m_atlasRunId.empty() ? std::string("-") : m_atlasRunId) +
        "/" + std::to_string(m_atlasEvents) +
        "/" + std::to_string(m_atlasSectionEvents) +
        "/" + std::to_string(m_atlasSerializerOps) +
        "/" + std::to_string(m_atlasUnknownOps) +
        "/" + StatusToken(m_lastAtlasEvent);
}
