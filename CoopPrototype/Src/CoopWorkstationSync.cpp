#include "CoopWorkstationSync.h"

#include "CoopRuntimeGuards.h"
#include "ModMain.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

#include <Prey/ArkEnums.h>
#include <Prey/CryAction/IGameObject.h>
#include <Prey/CryEntitySystem/IEntitySystem.h>
#include <Prey/CryGame/IGame.h>
#include <Prey/CryGame/IGameFramework.h>
#include <Prey/GameDll/ark/player/ArkPlayer.h>
#include <Prey/GameDll/ark/worldui/ArkStationWorldUI.h>
#include <Prey/GameDll/ark/worldui/ArkWorkstationScreen.h>

namespace
{
using CoopRuntimeGuards::TryGuardedCall;
using CoopRuntimeGuards::TryGuardedVoidCall;

std::string StatusToken(std::string value)
{
    if (value.empty())
        return "-";
    for (char& ch : value)
    {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isspace(uch) != 0 || ch == ';' || ch == '|' || ch == '=')
            ch = '_';
    }
    return value;
}

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool TryParseUint64(std::string_view text, uint64_t& outValue)
{
    if (text.empty())
        return false;
    std::string copy(text);
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(copy.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0')
        return false;
    outValue = static_cast<uint64_t>(parsed);
    return true;
}

std::string ReadFixedString(const char* value, size_t capacity)
{
    if (!value || capacity == 0)
        return {};
    const void* terminator = std::memchr(value, '\0', capacity);
    if (!terminator)
        return {};
    return std::string(value, static_cast<const char*>(terminator));
}

bool TryReadWorkstationLocked(
    ArkStationWorldUI* workstation,
    bool& locked,
    const char* label,
    std::string* reason)
{
    return workstation &&
        TryGuardedCall(label, [workstation]() { return workstation->m_bLocked; }, locked, reason);
}

bool TryReadWorkstationView(
    ArkStationWorldUI* workstation,
    EArkStationWorldUIState& state,
    uint64_t& currentId,
    const char* label,
    std::string* reason)
{
    if (!workstation)
        return false;
    const bool stateOk = TryGuardedCall(
        label, [workstation]() { return workstation->m_state; }, state, reason);
    const bool idOk = TryGuardedCall(
        label, [workstation]() { return workstation->m_currentId; }, currentId, reason);
    return stateOk && idOk;
}

ArkWorkstationScreen* GetWorkstationExtension(IEntity* entity)
{
    if (!entity || !gEnv || !gEnv->pGame)
        return nullptr;
    IGameFramework* framework = gEnv->pGame->GetIGameFramework();
    if (!framework)
        return nullptr;

    IGameObject* gameObject = nullptr;
    std::string reason;
    if (!TryGuardedCall(
            "workstation view GetGameObject",
            [framework, entity]() { return framework->GetGameObject(entity->GetId()); },
            gameObject,
            &reason) ||
        !gameObject)
    {
        return nullptr;
    }

    IGameObjectExtension* extension = nullptr;
    IGameObjectSystem::ExtensionID extensionId = 0;
    if (TryGuardedCall(
            "workstation view GetExtensionId",
            [gameObject]() { return gameObject->GetExtensionId("ArkWorkstationScreen"); },
            extensionId,
            &reason) &&
        extensionId != 0 &&
        TryGuardedCall(
            "workstation view QueryExtension id",
            [gameObject, extensionId]() { return gameObject->QueryExtension(extensionId); },
            extension,
            &reason) &&
        extension)
    {
        return static_cast<ArkWorkstationScreen*>(extension);
    }

    extension = nullptr;
    if (TryGuardedCall(
            "workstation view QueryExtension name",
            [gameObject]() { return gameObject->QueryExtension("ArkWorkstationScreen"); },
            extension,
            &reason) &&
        extension)
    {
        return static_cast<ArkWorkstationScreen*>(extension);
    }
    return nullptr;
}

ArkWorkstationScreen* FindWorkstationByGuid(
    uint64_t stableId,
    EntityId* outEntityId,
    uint64_t* outGuid,
    float* outDistance,
    std::string* outDetail)
{
    if (outEntityId)
        *outEntityId = INVALID_ENTITYID;
    if (outGuid)
        *outGuid = 0;
    if (outDistance)
        *outDistance = -1.0f;
    if (stableId == 0 || !gEnv || !gEnv->pEntitySystem)
        return nullptr;

    std::string reason;
    EntityId entityId = INVALID_ENTITYID;
    if (!TryGuardedCall(
            "workstation view FindEntityByGuid",
            [stableId]() { return gEnv->pEntitySystem->FindEntityByGuid(static_cast<EntityGUID>(stableId)); },
            entityId,
            &reason) ||
        entityId == INVALID_ENTITYID)
    {
        if (outDetail)
            *outDetail = "workstation_missing_guid_" + std::to_string(stableId);
        return nullptr;
    }

    IEntity* entity = nullptr;
    if (!TryGuardedCall(
            "workstation view GetEntity",
            [entityId]() { return gEnv->pEntitySystem->GetEntity(entityId); },
            entity,
            &reason) ||
        !entity)
    {
        return nullptr;
    }

    ArkWorkstationScreen* workstation = GetWorkstationExtension(entity);
    if (!workstation)
    {
        if (outDetail)
            *outDetail = "workstation_missing_extension_guid_" + std::to_string(stableId);
        return nullptr;
    }
    if (outEntityId)
        *outEntityId = entityId;
    if (outGuid)
        *outGuid = stableId;
    if (outDistance && ArkPlayer::GetInstancePtr() && ArkPlayer::GetInstance().GetEntity())
        *outDistance = (entity->GetWorldPos() - ArkPlayer::GetInstance().GetEntity()->GetWorldPos()).GetLength();
    if (outDetail)
        *outDetail = "workstation_guid_stable";
    return workstation;
}

ArkWorkstationScreen* FindNearestWorkstation(
    EntityId* outEntityId,
    uint64_t* outGuid,
    float* outDistance,
    std::string* outDetail)
{
    if (outEntityId)
        *outEntityId = INVALID_ENTITYID;
    if (outGuid)
        *outGuid = 0;
    if (outDistance)
        *outDistance = -1.0f;
    if (!gEnv || !gEnv->pEntitySystem || !ArkPlayer::GetInstancePtr() ||
        !ArkPlayer::GetInstance().GetEntity())
    {
        if (outDetail)
            *outDetail = "nearest_workstation_unavailable";
        return nullptr;
    }

    IEntityIt* rawIterator = nullptr;
    std::string reason;
    if (!TryGuardedCall(
            "nearest workstation view GetEntityIterator",
            []() { return gEnv->pEntitySystem->GetEntityIterator(); },
            rawIterator,
            &reason) ||
        !rawIterator)
    {
        return nullptr;
    }

    const Vec3 playerPos = ArkPlayer::GetInstance().GetEntity()->GetWorldPos();
    IEntityItPtr iterator = rawIterator;
    TryGuardedVoidCall("nearest workstation view MoveFirst", [&iterator]() { iterator->MoveFirst(); }, nullptr);
    ArkWorkstationScreen* best = nullptr;
    EntityId bestEntityId = INVALID_ENTITYID;
    uint64_t bestGuid = 0;
    float bestDistanceSq = std::numeric_limits<float>::max();
    uint32_t scanned = 0;
    uint32_t candidates = 0;
    std::string bestName = "-";
    while (true)
    {
        bool isEnd = false;
        if (TryGuardedCall(
                "nearest workstation view IsEnd",
                [&iterator]() { return iterator->IsEnd(); },
                isEnd,
                &reason) &&
            isEnd)
        {
            break;
        }
        IEntity* entity = nullptr;
        if (!TryGuardedCall(
                "nearest workstation view Next",
                [&iterator]() { return iterator->Next(); },
                entity,
                &reason) ||
            !entity)
        {
            break;
        }
        ++scanned;
        ArkWorkstationScreen* workstation = GetWorkstationExtension(entity);
        if (!workstation)
            continue;
        uint64_t guid = 0;
        TryGuardedCall("nearest workstation view GetGuid", [entity]() { return entity->GetGuid(); }, guid, nullptr);
        if (guid == 0)
            continue;
        ++candidates;
        const float distanceSq = (entity->GetWorldPos() - playerPos).GetLengthSquared();
        if (distanceSq >= bestDistanceSq)
            continue;
        best = workstation;
        bestDistanceSq = distanceSq;
        bestGuid = guid;
        TryGuardedCall("nearest workstation view GetId", [entity]() { return entity->GetId(); }, bestEntityId, nullptr);
        const char* rawName = nullptr;
        if (TryGuardedCall(
                "nearest workstation view GetName",
                [entity]() { return entity->GetName(); },
                rawName,
                nullptr) &&
            rawName)
        {
            bestName = rawName;
        }
    }

    if (outEntityId)
        *outEntityId = bestEntityId;
    if (outGuid)
        *outGuid = bestGuid;
    if (outDistance)
        *outDistance = best ? std::sqrt(std::max(0.0f, bestDistanceSq)) : -1.0f;
    if (outDetail)
    {
        std::ostringstream detail;
        detail << "nearest_workstation scanned=" << scanned << " candidates=" << candidates
               << " id=" << bestEntityId << " guid=" << bestGuid << " name=" << StatusToken(bestName)
               << " dist=" << (best ? std::sqrt(std::max(0.0f, bestDistanceSq)) : -1.0f);
        *outDetail = detail.str();
    }
    return best;
}

uint64_t GetWorkstationStableId(ArkStationWorldUI* workstation, std::string* outDetail)
{
    if (!workstation)
    {
        if (outDetail)
            *outDetail = "workstation_missing";
        return 0;
    }
    IEntity* entity = nullptr;
    std::string reason;
    if (!TryGuardedCall(
            "workstation view GetEntity",
            [workstation]() { return workstation->GetEntity(); },
            entity,
            &reason) ||
        !entity)
    {
        if (outDetail)
            *outDetail = "workstation_missing_entity_" + StatusToken(reason);
        return 0;
    }
    uint64_t guid = 0;
    TryGuardedCall("workstation view stable GetGuid", [entity]() { return entity->GetGuid(); }, guid, &reason);
    if (outDetail)
        *outDetail = guid != 0 ? "workstation_guid" : "workstation_missing_guid";
    return guid;
}

struct WorkstationView
{
    EArkStationWorldUIState state = EArkStationWorldUIState::Invalid;
    uint64_t currentId = 0;
    bool valid = false;
};

WorkstationView ReadWorkstationView(ArkStationWorldUI* workstation, const char* label)
{
    WorkstationView view;
    view.valid = TryReadWorkstationView(workstation, view.state, view.currentId, label, nullptr);
    return view;
}

void EmitWorkstationViewChange(
    ArkStationWorldUI* workstation,
    const WorkstationView& before,
    const char* reason)
{
    if (!gMod || !workstation)
        return;
    const WorkstationView after = ReadWorkstationView(workstation, "workstation view after native interaction");
    const bool changed = after.valid &&
        (!before.valid || before.state != after.state || before.currentId != after.currentId);
    gMod->OnLocalAreaObjectWorkstationViewChanged(
        workstation,
        static_cast<uint16_t>(after.state),
        after.currentId,
        changed,
        reason);
}

static auto s_hookWorkstationSetState = ArkStationWorldUI::FSetState.MakeHook();
static auto s_hookWorkstationOnButtonPress = ArkStationWorldUI::FOnButtonPress.MakeHook();
static auto s_hookWorkstationOnShowDetail = ArkStationWorldUI::FOnShowDetail.MakeHook();
// ArkStationWorldUI sends its selected ArkUtilityButton id through these two
// string-valued entity outputs. UtilityUsed updates native utility state;
// UtilityOutput is the authored FlowGraph action boundary.
static PreyFunction<bool(IEntity* const, const char* const, const char* const)>
    s_funcWorkstationStringOutput(0x11AE480);
static auto s_hookWorkstationStringOutput = s_funcWorkstationStringOutput.MakeHook();
static thread_local uint32_t s_workstationViewHookDepth = 0;

bool ArkEntity_WorkstationStringOutput_Hook(
    IEntity* entity,
    const char* outputName,
    const char* value)
{
    ArkWorkstationScreen* workstation = GetWorkstationExtension(entity);
    const bool utilityUsed = outputName && std::strcmp(outputName, "UtilityUsed") == 0;
    const bool utilityOutput = outputName && std::strcmp(outputName, "UtilityOutput") == 0;
    uint64_t utilityButtonId = 0;
    const bool validUtilityAction =
        workstation &&
        (utilityUsed || utilityOutput) &&
        value &&
        TryParseUint64(value, utilityButtonId) &&
        utilityButtonId != 0;

    const bool routeToAreaAuthority =
        validUtilityAction &&
        gMod &&
        gMod->ShouldRouteLocalWorkstationUtilityToAreaAuthorityForHook();
    if (routeToAreaAuthority)
    {
        // A non-authority player's workstation may update its local UI, but it
        // must not consume the utility or execute the mission FlowGraph. The
        // exact UtilityOutput id becomes a transient request to AreaAuthority.
        if (utilityOutput)
        {
            gMod->OnLocalAreaObjectWorkstationUtilityPressed(
                workstation,
                utilityButtonId,
                "ArkStationWorldUI UtilityOutput routed to AreaAuthority");
        }
        return true;
    }

    const bool result = s_hookWorkstationStringOutput.InvokeOrig(entity, outputName, value);
    if (validUtilityAction && utilityOutput && gMod)
    {
        gMod->OnLocalAreaObjectWorkstationUtilityPressed(
            workstation,
            utilityButtonId,
            "ArkStationWorldUI UtilityOutput AreaAuthority commit");
    }
    return result;
}

void ArkStationWorldUI_SetState_ViewHook(
    ArkStationWorldUI* workstation,
    EArkStationWorldUIState state,
    bool force)
{
    const bool outermost = s_workstationViewHookDepth == 0;
    const WorkstationView before = outermost
        ? ReadWorkstationView(workstation, "workstation view before SetState")
        : WorkstationView{};
    ++s_workstationViewHookDepth;
    s_hookWorkstationSetState.InvokeOrig(workstation, state, force);
    --s_workstationViewHookDepth;
    if (outermost)
        EmitWorkstationViewChange(workstation, before, "ArkStationWorldUI::SetState");
}

void ArkStationWorldUI_OnButtonPress_ViewHook(
    ArkStationWorldUI* workstation,
    IUIElement* const sender,
    SUIEventDesc const& event,
    SUIArguments const& args)
{
    const bool outermost = s_workstationViewHookDepth == 0;
    const WorkstationView before = outermost
        ? ReadWorkstationView(workstation, "workstation view before OnButtonPress")
        : WorkstationView{};
    ++s_workstationViewHookDepth;
    s_hookWorkstationOnButtonPress.InvokeOrig(workstation, sender, event, args);
    --s_workstationViewHookDepth;
    if (outermost)
        EmitWorkstationViewChange(workstation, before, "ArkStationWorldUI::OnButtonPress");
}

void ArkStationWorldUI_OnShowDetail_ViewHook(
    ArkStationWorldUI* workstation,
    IUIElement* const sender,
    SUIEventDesc const& event,
    SUIArguments const& args)
{
    const bool outermost = s_workstationViewHookDepth == 0;
    const WorkstationView before = outermost
        ? ReadWorkstationView(workstation, "workstation view before OnShowDetail")
        : WorkstationView{};
    ++s_workstationViewHookDepth;
    s_hookWorkstationOnShowDetail.InvokeOrig(workstation, sender, event, args);
    --s_workstationViewHookDepth;
    if (outermost)
        EmitWorkstationViewChange(workstation, before, "ArkStationWorldUI::OnShowDetail");
}
} // namespace

void InstallCoopWorkstationViewHooks()
{
    s_hookWorkstationSetState.SetHookFunc(&ArkStationWorldUI_SetState_ViewHook);
    s_hookWorkstationOnButtonPress.SetHookFunc(&ArkStationWorldUI_OnButtonPress_ViewHook);
    s_hookWorkstationOnShowDetail.SetHookFunc(&ArkStationWorldUI_OnShowDetail_ViewHook);
    s_hookWorkstationStringOutput.SetHookFunc(&ArkEntity_WorkstationStringOutput_Hook);
}

void ModMain::OnLocalAreaObjectWorkstationViewChanged(
    ArkStationWorldUI* workstation,
    uint16_t state,
    uint64_t currentId,
    bool changed,
    const char* reason)
{
    if (!changed || !workstation)
        return;
    if ((m_applyingRemoteAreaObjectEvent || m_remoteAreaObjectEchoSuppressSeconds > 0.0f) &&
        m_localAreaObjectCommandMutationDepth == 0)
    {
        ++m_reentrantAreaObjectEventSkips;
        m_lastAreaObjectEvent =
            "area_object_skip_remote_echo_kind_" +
            std::to_string(CoopProtocol::kAreaObjectEventWorkstationView) +
            "_reason_" + StatusToken(reason && reason[0] ? std::string(reason) : std::string("-"));
        return;
    }
    if (state < static_cast<uint16_t>(EArkStationWorldUIState::Locked) ||
        state > static_cast<uint16_t>(EArkStationWorldUIState::ChildDefined))
    {
        m_lastAreaObjectEvent = "area_object_workstation_view_invalid_state_" + std::to_string(state);
        return;
    }

    std::string detail;
    const uint64_t stableId = GetWorkstationStableId(workstation, &detail);
    if (stableId == 0)
    {
        m_lastAreaObjectEvent =
            "area_object_workstation_view_missing_stable_id_detail_" + StatusToken(detail);
        return;
    }

    const std::string currentIdText = std::to_string(currentId);
    QueueLocalAreaObjectEventForHook(
        CoopProtocol::kAreaObjectEventWorkstationView,
        stableId,
        state,
        0,
        reason,
        0,
        currentIdText.c_str());
}

void ModMain::OnLocalAreaObjectWorkstationUtilityPressed(
    ArkStationWorldUI* workstation,
    uint64_t utilityButtonId,
    const char* reason)
{
    if (!workstation || utilityButtonId == 0)
        return;
    if ((m_applyingRemoteAreaObjectEvent || m_remoteAreaObjectEchoSuppressSeconds > 0.0f) &&
        m_localAreaObjectCommandMutationDepth == 0)
    {
        ++m_reentrantAreaObjectEventSkips;
        m_lastAreaObjectEvent =
            "area_object_skip_remote_echo_kind_" +
            std::to_string(CoopProtocol::kAreaObjectEventWorkstationUtilityPressed);
        return;
    }

    std::string detail;
    const uint64_t stableId = GetWorkstationStableId(workstation, &detail);
    if (stableId == 0)
    {
        m_lastAreaObjectEvent =
            "area_object_workstation_utility_missing_stable_id_detail_" + StatusToken(detail);
        return;
    }

    const std::string utilityButtonText = std::to_string(utilityButtonId);
    QueueLocalAreaObjectEventForHook(
        CoopProtocol::kAreaObjectEventWorkstationUtilityPressed,
        stableId,
        1,
        0,
        reason,
        0,
        utilityButtonText.c_str());
}

bool ModMain::ShouldRouteLocalWorkstationUtilityToAreaAuthorityForHook() const
{
    return m_networkMode == CoopNetworkMode::Client &&
        IsSessionGameplayReady() &&
        !IsClientAreaAuthorityActive() &&
        !m_applyingRemoteAreaObjectEvent;
}

bool ModMain::DebugHandleRuntimeWorkstationCommand(
    const std::string& command,
    const std::vector<std::string>& args,
    std::string& action)
{
    const bool setMode = command == "coop_area_workstation_set";
    const bool utilityMode = command == "coop_area_workstation_utility";
    const std::string target = args.empty() ? std::string("nearest") : args.front();
    if (setMode && args.size() < 3)
    {
        action = "area_workstation_set_usage";
        m_networkStatus = "area_workstation_set_usage_target_locked_or_view_value_optional_current_id";
        return false;
    }
    if (utilityMode && args.size() < 2)
    {
        action = "area_workstation_utility_usage";
        m_networkStatus = "area_workstation_utility_usage_target_utility_button_id";
        return false;
    }

    ArkWorkstationScreen* workstation = nullptr;
    EntityId entityId = INVALID_ENTITYID;
    uint64_t guid = 0;
    float distance = -1.0f;
    std::string targetDetail;
    if (target == "nearest")
    {
        workstation = FindNearestWorkstation(&entityId, &guid, &distance, &targetDetail);
    }
    else if (target.rfind("guid:", 0) == 0 || target.rfind("stable:", 0) == 0)
    {
        const size_t prefixLength = target.rfind("stable:", 0) == 0 ? 7 : 5;
        uint64_t parsedGuid = 0;
        if (TryParseUint64(std::string_view(target).substr(prefixLength), parsedGuid) && parsedGuid != 0)
            workstation = FindWorkstationByGuid(parsedGuid, &entityId, &guid, &distance, &targetDetail);
        else
            targetDetail = "invalid_workstation_guid_target";
    }
    else
    {
        IEntity* entity = ResolveRuntimeEntityTarget(target, targetDetail);
        if (entity)
        {
            workstation = GetWorkstationExtension(entity);
            TryGuardedCall("runtime workstation GetId", [entity]() { return entity->GetId(); }, entityId, nullptr);
            TryGuardedCall("runtime workstation GetGuid", [entity]() { return entity->GetGuid(); }, guid, nullptr);
            if (ArkPlayer::GetInstancePtr() && ArkPlayer::GetInstance().GetEntity())
                distance = (entity->GetWorldPos() - ArkPlayer::GetInstance().GetEntity()->GetWorldPos()).GetLength();
        }
    }

    bool locked = true;
    EArkStationWorldUIState viewState = EArkStationWorldUIState::Invalid;
    uint64_t currentId = 0;
    std::string reason;
    if (!workstation)
    {
        action = utilityMode
            ? "area_workstation_utility_missing"
            : setMode ? "area_workstation_set_missing" : "area_workstation_probe_missing";
        m_networkStatus = action + "_target_" + StatusToken(target) + "_detail_" + StatusToken(targetDetail);
        return false;
    }

    TryReadWorkstationLocked(workstation, locked, "runtime workstation locked", &reason);
    TryReadWorkstationView(workstation, viewState, currentId, "runtime workstation view", &reason);
    bool ok = true;
    if (utilityMode)
    {
        uint64_t utilityButtonId = 0;
        IEntity* outputEntity = nullptr;
        const bool utilityIdOk = TryParseUint64(args[1], utilityButtonId) && utilityButtonId != 0;
        const bool entityOk = utilityIdOk && TryGuardedCall(
            "runtime workstation utility GetEntity",
            [workstation]() { return workstation->GetEntity(); },
            outputEntity,
            &reason) && outputEntity;
        const std::string utilityButtonText = std::to_string(utilityButtonId);
        bool usedResult = false;
        bool outputResult = false;
        const bool usedOk = entityOk && TryGuardedCall(
            "runtime workstation UtilityUsed",
            [outputEntity, &utilityButtonText]()
            {
                return s_funcWorkstationStringOutput(
                    outputEntity, "UtilityUsed", utilityButtonText.c_str());
            },
            usedResult,
            &reason);
        const bool outputOk = usedOk && TryGuardedCall(
            "runtime workstation UtilityOutput",
            [outputEntity, &utilityButtonText]()
            {
                return s_funcWorkstationStringOutput(
                    outputEntity, "UtilityOutput", utilityButtonText.c_str());
            },
            outputResult,
            &reason);
        ok = utilityIdOk && entityOk && usedOk && outputOk;
        action = ok ? "area_workstation_utility" : "area_workstation_utility_failed";
        targetDetail +=
            " utility=" + std::to_string(utilityButtonId) +
            " used=" + std::to_string(usedResult ? 1 : 0) +
            " output=" + std::to_string(outputResult ? 1 : 0) +
            " routed=" + std::to_string(ShouldRouteLocalWorkstationUtilityToAreaAuthorityForHook() ? 1 : 0);
    }
    else if (setMode)
    {
        const std::string field = ToLowerAscii(args[1]);
        if (field == "locked" || field == "lock")
        {
            const bool desired = std::atoi(args[2].c_str()) != 0;
            bool callResult = false;
            ++m_localAreaObjectCommandMutationDepth;
            ok = TryGuardedCall(
                "runtime workstation SetLockedFromScript",
                [workstation, desired]() { return workstation->SetLockedFromScript(desired); },
                callResult,
                &reason);
            if (m_localAreaObjectCommandMutationDepth > 0)
                --m_localAreaObjectCommandMutationDepth;
            TryReadWorkstationLocked(workstation, locked, "runtime workstation locked after set", &reason);
            ok = ok && locked == desired;
        }
        else if (field == "view" || field == "state")
        {
            const int desiredRaw = std::atoi(args[2].c_str());
            uint64_t desiredCurrentId = 0;
            const bool currentIdOk = args.size() < 4 || TryParseUint64(args[3], desiredCurrentId);
            if (desiredRaw < static_cast<int>(EArkStationWorldUIState::Locked) ||
                desiredRaw > static_cast<int>(EArkStationWorldUIState::ChildDefined) ||
                !currentIdOk)
            {
                ok = false;
                reason = "invalid_view_state_or_current_id";
            }
            else
            {
                const EArkStationWorldUIState desiredState = static_cast<EArkStationWorldUIState>(desiredRaw);
                ++m_localAreaObjectCommandMutationDepth;
                const bool assignBeforeOk = TryGuardedVoidCall(
                    "runtime workstation current id before",
                    [workstation, desiredCurrentId]() { workstation->m_currentId = desiredCurrentId; },
                    &reason);
                const bool stateOk = TryGuardedVoidCall(
                    "runtime workstation SetState",
                    [workstation, desiredState]() { workstation->SetState(desiredState, true); },
                    &reason);
                const bool assignAfterOk = TryGuardedVoidCall(
                    "runtime workstation current id after",
                    [workstation, desiredCurrentId]() { workstation->m_currentId = desiredCurrentId; },
                    &reason);
                TryGuardedVoidCall(
                    "runtime workstation RefreshUI",
                    [workstation]() { workstation->RefreshUI(false, false); },
                    nullptr);
                if (m_localAreaObjectCommandMutationDepth > 0)
                    --m_localAreaObjectCommandMutationDepth;
                const bool readOk = TryReadWorkstationView(
                    workstation, viewState, currentId, "runtime workstation view after set", &reason);
                // A zero selection asks the native UI to choose its first valid
                // email/utility. That stable id is what the hook broadcasts.
                const bool currentMatches = desiredCurrentId == 0 || currentId == desiredCurrentId;
                ok = assignBeforeOk && stateOk && assignAfterOk && readOk &&
                    viewState == desiredState && currentMatches;
            }
        }
        else
        {
            ok = false;
            reason = "unknown_field_" + field;
        }
        action = ok ? "area_workstation_set" : "area_workstation_set_failed";
    }
    else
    {
        action = "area_workstation_probe";
    }

    m_networkStatus = action + "_id_" + std::to_string(entityId) +
        "_guid_" + std::to_string(guid) +
        "_locked_" + std::to_string(locked ? 1 : 0) +
        "_state_" + std::to_string(static_cast<uint16_t>(viewState)) +
        "_current_" + std::to_string(currentId) +
        "_dist_" + std::to_string(distance) +
        "_detail_" + StatusToken(targetDetail.empty() ? std::string("-") : targetDetail) +
        "_reason_" + StatusToken(reason.empty() ? std::string("-") : reason);
    return ok;
}

bool ModMain::ApplyAreaObjectWorkstationView(
    const CoopProtocol::AreaObjectEventPacket& packet,
    std::string& detail)
{
    const uint16_t rawState = packet.value;
    if (rawState < static_cast<uint16_t>(EArkStationWorldUIState::Locked) ||
        rawState > static_cast<uint16_t>(EArkStationWorldUIState::ChildDefined) ||
        std::memchr(packet.textValue, '\0', sizeof(packet.textValue)) == nullptr)
    {
        detail = "invalid_workstation_view_state_" + std::to_string(rawState);
        return false;
    }
    if (packet.targetClassHash != HashStoryString("ArkWorkstationScreen"))
    {
        detail = "invalid_workstation_view_class_" + std::to_string(packet.targetClassHash);
        return false;
    }

    uint64_t desiredCurrentId = 0;
    const std::string currentIdText = ReadFixedString(packet.textValue, sizeof(packet.textValue));
    if (!TryParseUint64(currentIdText, desiredCurrentId))
    {
        detail = "invalid_workstation_view_current_id_" + StatusToken(currentIdText);
        return false;
    }

    EntityId entityId = INVALID_ENTITYID;
    uint64_t guid = 0;
    float distance = -1.0f;
    std::string lookupDetail;
    ArkWorkstationScreen* workstation = FindWorkstationByGuid(
        packet.targetGuid, &entityId, &guid, &distance, &lookupDetail);
    if (!workstation)
    {
        detail = "missing_workstation_view_guid_" + std::to_string(packet.targetGuid) +
            "_detail_" + StatusToken(lookupDetail);
        return false;
    }

    const EArkStationWorldUIState desiredState = static_cast<EArkStationWorldUIState>(rawState);
    EArkStationWorldUIState beforeState = EArkStationWorldUIState::Invalid;
    EArkStationWorldUIState afterState = EArkStationWorldUIState::Invalid;
    uint64_t beforeCurrentId = 0;
    uint64_t afterCurrentId = 0;
    std::string reason;
    const bool beforeOk = TryReadWorkstationView(
        workstation, beforeState, beforeCurrentId, "area object apply workstation view before", &reason);
    if (beforeOk && beforeState == desiredState && beforeCurrentId == desiredCurrentId)
    {
        detail = "already_workstation_view_state_" + std::to_string(rawState) +
            "_current_" + std::to_string(desiredCurrentId) + "_guid_" + std::to_string(guid);
        return true;
    }

    const bool assignBeforeOk = TryGuardedVoidCall(
        "area object apply workstation current id before",
        [workstation, desiredCurrentId]() { workstation->m_currentId = desiredCurrentId; },
        &reason);
    const bool stateOk = TryGuardedVoidCall(
        "area object apply workstation SetState",
        [workstation, desiredState]() { workstation->SetState(desiredState, true); },
        &reason);
    const bool assignAfterOk = TryGuardedVoidCall(
        "area object apply workstation current id after",
        [workstation, desiredCurrentId]() { workstation->m_currentId = desiredCurrentId; },
        &reason);
    const bool refreshOk = TryGuardedVoidCall(
        "area object apply workstation RefreshUI",
        [workstation]() { workstation->RefreshUI(false, false); },
        &reason);
    const bool afterOk = TryReadWorkstationView(
        workstation, afterState, afterCurrentId, "area object apply workstation view after", &reason);
    const bool matches = afterOk && afterState == desiredState && afterCurrentId == desiredCurrentId;
    detail = assignBeforeOk && stateOk && assignAfterOk && matches
        ? ("applied_workstation_view_state_" + std::to_string(rawState) +
            "_current_" + std::to_string(desiredCurrentId) +
            "_id_" + std::to_string(entityId) + "_guid_" + std::to_string(guid) +
            "_refresh_" + std::to_string(refreshOk ? 1 : 0))
        : ("failed_workstation_view_state_" + std::to_string(static_cast<uint16_t>(afterState)) +
            "_current_" + std::to_string(afterCurrentId) +
            "_guid_" + std::to_string(packet.targetGuid) +
            "_reason_" + StatusToken(reason.empty() ? std::string("-") : reason));
    return assignBeforeOk && stateOk && assignAfterOk && matches;
}

bool ModMain::ApplyAreaObjectWorkstationUtilityPressed(
    const CoopProtocol::AreaObjectEventPacket& packet,
    std::string& detail)
{
    if (packet.targetClassHash != HashStoryString("ArkWorkstationScreen") ||
        packet.value != 1 ||
        packet.count != 0 ||
        std::memchr(packet.textValue, '\0', sizeof(packet.textValue)) == nullptr)
    {
        detail = "invalid_workstation_utility_packet_guid_" + std::to_string(packet.targetGuid);
        return false;
    }

    uint64_t utilityButtonId = 0;
    const std::string utilityButtonText = ReadFixedString(packet.textValue, sizeof(packet.textValue));
    if (!TryParseUint64(utilityButtonText, utilityButtonId) || utilityButtonId == 0)
    {
        detail = "invalid_workstation_utility_id_" + StatusToken(utilityButtonText);
        return false;
    }

    EntityId entityId = INVALID_ENTITYID;
    uint64_t guid = 0;
    float distance = -1.0f;
    std::string lookupDetail;
    ArkWorkstationScreen* workstation = FindWorkstationByGuid(
        packet.targetGuid, &entityId, &guid, &distance, &lookupDetail);
    if (!workstation)
    {
        detail = "missing_workstation_utility_guid_" + std::to_string(packet.targetGuid) +
            "_detail_" + StatusToken(lookupDetail);
        return false;
    }

    const bool localAreaAuthority =
        m_networkMode == CoopNetworkMode::Host || IsClientAreaAuthorityActive();
    if (m_applyingRemoteAreaObjectEvent && !localAreaAuthority)
    {
        // The reliable event is an input request. Same-area observers wait for
        // authoritative mission results instead of executing the FlowGraph.
        detail = "accepted_workstation_utility_observer_id_" + std::to_string(utilityButtonId) +
            "_entity_" + std::to_string(entityId) +
            "_guid_" + std::to_string(guid);
        return true;
    }

    IEntity* outputEntity = nullptr;
    std::string reason;
    const bool entityOk = TryGuardedCall(
        "area object apply workstation utility GetEntity",
        [workstation]() { return workstation->GetEntity(); },
        outputEntity,
        &reason) && outputEntity;
    bool usedResult = false;
    bool outputResult = false;
    const bool usedOk = entityOk && TryGuardedCall(
        "area object apply workstation UtilityUsed",
        [outputEntity, &utilityButtonText]()
        {
            return s_funcWorkstationStringOutput(
                outputEntity, "UtilityUsed", utilityButtonText.c_str());
        },
        usedResult,
        &reason);
    const bool outputOk = usedOk && TryGuardedCall(
        "area object apply workstation UtilityOutput",
        [outputEntity, &utilityButtonText]()
        {
            return s_funcWorkstationStringOutput(
                outputEntity, "UtilityOutput", utilityButtonText.c_str());
        },
        outputResult,
        &reason);
    const bool ok = entityOk && usedOk && outputOk;
    detail = ok
        ? ("applied_workstation_utility_id_" + std::to_string(utilityButtonId) +
            "_entity_" + std::to_string(entityId) +
            "_guid_" + std::to_string(guid) +
            "_dist_" + std::to_string(distance) +
            "_used_" + std::to_string(usedResult ? 1 : 0) +
            "_output_" + std::to_string(outputResult ? 1 : 0))
        : ("failed_workstation_utility_id_" + std::to_string(utilityButtonId) +
            "_guid_" + std::to_string(packet.targetGuid) +
            "_reason_" + StatusToken(reason.empty() ? std::string("-") : reason));
    return ok;
}
