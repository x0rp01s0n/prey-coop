#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <Prey/CryEntitySystem/IEntity.h>

class CoopRuntimeExtractor
{
public:
    using ControlCommandHandler = std::function<bool(const std::string& command, const std::vector<std::string>& args, std::string& statusLine)>;

    void SetControlCommandHandler(ControlCommandHandler handler);
    void Tick(float frameTime);
    void TickControlOnly(float frameTime);
    void Draw();
    void ResetRuntimeRefs(const char* reason);
    std::string GetLatestExportPath() const;

private:
    struct EntityRow
    {
        EntityId entityId = INVALID_ENTITYID;
        uint64_t guid = 0;
        uint64_t archetypeId = 0;
        std::uintptr_t entityPtr = 0;
        std::uintptr_t classPtr = 0;
        std::uintptr_t archetypePtr = 0;
        Vec3 position = Vec3(ZERO);
        uint32_t flags = 0;
        bool loadedFromLevel = false;
        bool garbage = false;
        std::string name;
        std::string className;
        std::string archetypeName;
    };

    struct ClassRow
    {
        std::uintptr_t classPtr = 0;
        uint32_t flags = 0;
        std::string name;
        std::string scriptFile;
    };

    struct ExtensionProbeRow
    {
        EntityId entityId = INVALID_ENTITYID;
        std::uintptr_t extensionPtr = 0;
        std::uintptr_t vtablePtr = 0;
        bool likelyObject = false;
        std::string entityName;
        std::string className;
        std::string extensionName;
        std::string modulePath;
    };

    std::string GetCurrentLevelName() const;
    std::string GetExtractorRootPath() const;
    std::string GetCommandFilePath() const;
    std::string GetStatusFilePath() const;
    bool ScanEntities();
    bool ScanClassRegistry();
    bool ProbeExtensionAcrossSnapshot(const std::string& extensionName);
    bool ExportJsonl();
    void ProcessCommandFile(float frameTime, bool controlOnly);
    void ProcessCommandText(const std::string& text, bool controlOnly);
    void WriteStatusFile() const;
    bool CaptureEntityRow(IEntity& entity, EntityRow& outRow);
    bool EntityMatchesFilter(const EntityRow& row) const;
    void ClearSnapshots(const char* reason);
    void DrawEntityRows();
    void DrawClassRows();
    void DrawExtensionRows();

    std::vector<EntityRow> m_entityRows;
    std::vector<ClassRow> m_classRows;
    std::vector<ExtensionProbeRow> m_extensionRows;
    std::string m_entityFilter;
    std::string m_extensionProbeName = "ArkLevelTransitionDoor";
    std::string m_lastStatus = "not scanned";
    std::string m_lastGuardFailure = "-";
    std::string m_lastCommandText;
    std::string m_lastCommandStatus = "-";
    std::string m_lastExportPath = "-";
    std::string m_lastLevelName = "unknown";
    ControlCommandHandler m_controlCommandHandler;
    float m_commandPollAccumulator = 0.0f;
    uint32_t m_scanCount = 0;
    uint32_t m_classScanCount = 0;
    uint32_t m_extensionProbeCount = 0;
    uint32_t m_exportCount = 0;
    uint32_t m_guardSkips = 0;
    uint32_t m_entitiesSeen = 0;
    uint32_t m_entitiesFiltered = 0;
    uint32_t m_extensionMisses = 0;
    int m_maxEntityRows = 512;
    int m_maxClassRows = 1024;
    int m_maxExtensionProbeRows = 512;
    bool m_pollCommandFile = true;
    bool m_showOnlyFilteredEntities = true;
};
