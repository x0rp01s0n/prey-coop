#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <Prey/CryNetwork/ISerialize.h>

struct NativeSideBlobCaptureState;

class CoopSaveStateBridge
{
public:
    enum class SchemaSemantic : uint8_t
    {
        Unknown,
        Section,
        GameState,
        PersistentState,
        PlayerCore,
        PlayerVitals,
        PlayerInventory,
        PlayerInventoryCell,
        Inventory,
        InventoryCell,
        Item,
        Weapon,
        Ability,
        Neuromod,
        Keycard,
        Keycode,
        FabricationPlan,
        Objective,
        Status,
        WorldEntity,
        Transform,
        LevelState,
        Count,
    };
    static constexpr size_t kSchemaSemanticCount = static_cast<size_t>(SchemaSemantic::Count);

    enum class SectionApi : uint8_t
    {
        LegacySave,
        LegacyLoad,
        ActiveSave,
        ActiveLoad,
    };

    enum class TracePhase : uint8_t
    {
        None,
        ConcreteLoadInitEnter,
        ConcreteLoadInitExitRootReady,
        ConcreteSaveCompleteEnter,
        ConcreteSaveCompleteExit,
        ActiveLoadNotify,
        ActiveSaveNotify,
        LoadGetSectionEnter,
        LoadGetSectionExit,
        LoadHaveSectionEnter,
        LoadHaveSectionExit,
        LoadCompleteEnter,
        LoadCompleteExit,
        SaveAddSectionEnter,
        SaveAddSectionExit,
        ArkLoadCurrentLevelStateEnter,
        ArkLoadCurrentLevelStateExit,
        ArkSaveCurrentLevelStateEnter,
        ArkSaveCurrentLevelStateExit,
        ArkSerializePersistentStateEnter,
        ArkSerializePersistentStateExit,
        ArkPlayerSerializeEnter,
        ArkPlayerSerializeExit,
        ArkInventorySerializeEnter,
        ArkInventorySerializeExit,
        SerializerOp,
        SectionEvent,
    };

    struct SerializerFingerprint
    {
        ISerialize* impl = nullptr;
        const void* vtable = nullptr;
        std::string text;
    };

    struct SectionEvent
    {
        uint64_t sequence = 0;
        SectionApi api = SectionApi::LegacySave;
        std::string source;
        std::string sectionName;
        bool reading = false;
        int target = eST_SaveGame;
        bool ok = false;
        bool hasSerializer = false;
        bool activeInterface = false;
        bool gameState = false;
        bool persistentState = false;
        bool nativePatchCandidate = false;
        SerializerFingerprint fingerprint;
        std::string serializerText;
    };

    struct NativeSaveSectionState
    {
        std::string sectionName;
        TSerialize* serializer = nullptr;
        std::unique_ptr<TSerialize>* serializerOwner = nullptr;
        SerializerFingerprint fingerprint;
        bool reading = true;
        int target = eST_SaveGame;
        bool ok = false;
        bool gameState = false;
        bool persistentState = false;
        bool allowMutation = false;
        std::string reason;
    };

    struct CoopPlayerSaveSection
    {
        const NativeSideBlobCaptureState* nativeCapture = nullptr;
        bool hasNativeCapture = false;
        std::string username;
        std::string levelName;
        std::string saveKey;
    };

    struct CoopSaveMergeResult
    {
        bool attempted = false;
        bool candidate = false;
        bool patched = false;
        bool passthrough = true;
        bool deferred = false;
        bool failed = false;
        std::string reason;
        std::string detail;
        TSerialize* patchedState = nullptr;
    };

    void Reset();
    uint64_t GetSequence() const { return m_sequence; }

    SectionEvent ObserveSection(
        SectionApi api,
        const char* source,
        const char* rawSectionName,
        TSerialize* serializer,
        bool fallbackReading,
        int fallbackTarget,
        bool fallbackOk);

    SectionEvent ObserveLoadUniquePtrSection(
        SectionApi api,
        const char* source,
        const char* rawSectionName,
        std::unique_ptr<TSerialize>* serializerOwner);

    uint64_t BeginTracePhase(
        TracePhase phase,
        SchemaSemantic semantic,
        const char* label,
        const void* thisPtr,
        const void* serializerPtr,
        const char* detail,
        bool reading = false,
        int target = eST_SaveGame,
        bool ok = true);

    void EndTracePhase(
        uint64_t enterSequence,
        TracePhase phase,
        SchemaSemantic semantic,
        const char* label,
        const void* thisPtr,
        const void* serializerPtr,
        const char* detail,
        bool reading = false,
        int target = eST_SaveGame,
        bool ok = true);

    void RecordSectionEvent(const SectionEvent& event);

    void RecordSerializerOp(
        const char* source,
        const char* op,
        const char* path,
        const char* typeName,
        SchemaSemantic semantic,
        bool reading,
        int depth,
        const char* detail);
    void RecordSerializerStoreProbe(
        const char* source,
        const char* op,
        const char* path,
        const void* serializerPtr,
        const char* detail);

    bool ShouldRecordBoundaryProbe(
        const char* label,
        const char* sectionName,
        bool result);
    std::string FindSectionForSerializer(TSerialize serializer) const;
    std::string FindSectionForSerializerImpl(const void* serializerImpl) const;
    CoopSaveMergeResult MergeCoopSave(NativeSaveSectionState& vanillaSave, const CoopPlayerSaveSection& coopSection);

    static SerializerFingerprint BuildSerializerFingerprint(TSerialize serializer);
    static const char* ApiName(SectionApi api);
    static const char* PhaseName(TracePhase phase);
    static const char* SemanticName(SchemaSemantic semantic);
    static bool IsGameStateSection(std::string_view sectionName);
    static bool IsPersistentStateSection(std::string_view sectionName);
    static SchemaSemantic ClassifySectionName(std::string_view sectionName);
    static SchemaSemantic ClassifySerializerPath(
        std::string_view source,
        std::string_view path,
        std::string_view typeName,
        std::string_view op);
    static std::string BuildCompactStatus(const SectionEvent& event);
    static std::string BuildCompactStatus(const CoopSaveMergeResult& result);
    std::string BuildAtlasStatus() const;

private:
    uint64_t NextSequence();
    bool IsAtlasEnabled() const;
    void EnsureAtlasRun();
    void WriteAtlasEvent(
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
        const char* detail);
    void WriteAtlasSectionEvent(const SectionEvent& event);
    void WriteAtlasSummary();

    uint64_t m_sequence = 0;
    bool m_atlasRunStarted = false;
    std::string m_atlasRunId;
    std::string m_atlasRootPath;
    std::string m_lastAtlasEvent = "-";
    uint64_t m_atlasEvents = 0;
    uint64_t m_atlasSectionEvents = 0;
    uint64_t m_atlasSerializerOps = 0;
    uint64_t m_atlasStoreProbes = 0;
    uint64_t m_atlasUnknownOps = 0;
    std::array<uint64_t, kSchemaSemanticCount> m_atlasSemanticBuckets = {};
    std::unordered_set<std::string> m_atlasBoundaryProbeKeys;
    std::unordered_map<std::uintptr_t, std::string> m_serializerSections;
};
