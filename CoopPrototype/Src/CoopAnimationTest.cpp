#include "ModMain.h"
#include "CoopFilesystem.h"
#include "CoopRuntimeConfig.h"
#include "CoopRuntimeGuards.h"
#include "CoopRuntimeLog.h"
#include "CoopSerialSequence.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <string_view>
#include <vector>

#include <EntityUtils.h>
#include <Chairloader/ChairloaderEnv.h>
#include <Chairloader/IChairLogger.h>
#include <Chairloader/IChairXmlUtils.h>
#include <Prey/CryEntitySystem/IEntity.h>
#include <Prey/CryEntitySystem/IEntityClass.h>
#include <Prey/CryEntitySystem/IEntitySystem.h>
#include <Prey/CryScriptSystem/IScriptSystem.h>
#include <Prey/CryScriptSystem/ScriptHelpers.h>
#include <Prey/CrySystem/ISystem.h>
#include <Prey/GameDll/ark/npc/ArkNpc.h>
#include <Prey/GameDll/ark/player/ArkPlayer.h>

namespace
{
using CoopRuntimeGuards::TryGuardedCall;
using CoopRuntimeGuards::TryGuardedVoidCall;
using CoopRuntimeGuards::TryReadRuntimeValue;

constexpr uint64_t kAnimationTestDefaultProxyArchetype = 10739735956144685611ULL;
constexpr float kAnimationTestProxyForwardOffsetMeters = 2.0f;
constexpr float kPendingSemanticAsyncActionWindowSeconds = 0.75f;

constexpr size_t kNpcBodyStateTransitionActionOffset = 0x18;
constexpr size_t kNpcBodyStateStoredActionOffset = 0x38;
constexpr size_t kIActionContextOffset = 0x08;
constexpr size_t kIActionActiveTimeOffset = 0x10;
constexpr size_t kIActionQueueTimeOffset = 0x14;
constexpr size_t kIActionForcedScopeMaskOffset = 0x18;
constexpr size_t kIActionInstalledScopeMaskOffset = 0x1C;
constexpr size_t kIActionSubContextOffset = 0x20;
constexpr size_t kIActionPriorityOffset = 0x24;
constexpr size_t kIActionStatusOffset = 0x28;
constexpr size_t kIActionFlagsOffset = 0x2C;
constexpr size_t kIActionRootScopeOffset = 0x30;
constexpr size_t kIActionFragmentIdOffset = 0x38;
constexpr size_t kIActionFragTagsOffset = 0x3C;
constexpr size_t kIActionOptionIdxOffset = 0x48;
constexpr size_t kIActionUserTokenOffset = 0x4C;
constexpr size_t kIActionRefCountOffset = 0x50;
constexpr size_t kIActionSpeedBiasOffset = 0x54;
constexpr size_t kIActionAnimWeightOffset = 0x58;
constexpr size_t kArkNpcAnimActionNpcOffset = 0x80;
constexpr size_t kMannequinTagStateByteCount = 12;
constexpr uint32_t kIActionBlendOutFlag = 1u << 0;
constexpr uint32_t kIActionStoppingFlag = 1u << 13;
constexpr int kIActionStatusPending = 1;
constexpr int kIActionStatusFinished = 4;
constexpr size_t kCharacterGetSkeletonAnimVtableIndex = 5;
constexpr size_t kSkeletonSetDesiredMotionParamVtableIndex = 24;
constexpr int kMotionParamTravelSpeed = 0;
constexpr int kMotionParamTravelAngle = 2;

constexpr const char* kAnimationTestClipProbeCandidates[] = {
    "stand_tac_runStart_nw_fwd_fast_lf_3p_01",
    "stand_tac_walkStart_nw_fwd_fast_lf_3p_01",
    "relaxed_tac_runStart_nw_fwd_slow_lf_3p_01",
    "relaxed_tac_walkStart_nw_fwd_slow_lf_3p_01",
    "stealth_tac_runStart_nw_fwd_fast_lf_3p_01",
    "stealth_tac_walkStart_nw_fwd_fast_lf_3p_01",
    "stand_tac_walkStart_pistol_fwd_fast_lf_3p_01",
    "stealth_tac_walkStart_pistol_fwd_fast_lf_3p_01",
    "RA_hearunknown",
    "RA_hearunknown1",
    "RA_hearunknown2",
    "RA_hearunknown3",
    "goo",
    "prone_toCombat_nw_01",
};

struct MannequinFragmentNameTable
{
    bool humanAttempted = false;
    bool playerAttempted = false;
    bool mimicAttempted = false;
    bool phantomAttempted = false;
    bool nightmareAttempted = false;
    bool poltergeistAttempted = false;
    bool telepathAttempted = false;
    bool technopathAttempted = false;
    bool weaverAttempted = false;
    bool operatorMilitaryAttempted = false;
    bool operatorAttempted = false;
    std::vector<std::string> human;
    std::vector<std::string> player;
    std::vector<std::string> mimic;
    std::vector<std::string> phantom;
    std::vector<std::string> nightmare;
    std::vector<std::string> poltergeist;
    std::vector<std::string> telepath;
    std::vector<std::string> technopath;
    std::vector<std::string> weaver;
    std::vector<std::string> operatorMilitary;
    std::vector<std::string> op;
    std::string humanStatus = "not_loaded";
    std::string playerStatus = "not_loaded";
    std::string mimicStatus = "not_loaded";
    std::string phantomStatus = "not_loaded";
    std::string nightmareStatus = "not_loaded";
    std::string poltergeistStatus = "not_loaded";
    std::string telepathStatus = "not_loaded";
    std::string technopathStatus = "not_loaded";
    std::string weaverStatus = "not_loaded";
    std::string operatorMilitaryStatus = "not_loaded";
    std::string operatorStatus = "not_loaded";
};

struct MannequinSnippetEntry
{
    std::string fragment;
    std::string tags;
    std::string fragTags;
    std::string animations;
    std::string animationFlags;
    std::string procedures;
    std::string weight;
    std::string cooldown;
    std::string blendOut;
    int ordinal = 0;
};

std::string ToLowerAsciiAnimation(std::string value);
bool ContainsToken(const std::string& haystack, const char* needle);
std::vector<const char*> MannequinDatabasePaths(const std::string& kind);
MannequinSnippetEntry BuildSnippetEntry(pugi::xml_node fragmentNode, pugi::xml_node variantNode, int ordinal);

bool RequestBlockedMannequinActionStop(const void* action)
{
    if (!action ||
        !CoopRuntimeGuards::IsReadableRuntimePointer(
            action,
            kIActionFlagsOffset + sizeof(uint32_t)))
    {
        return false;
    }

    auto* bytes = reinterpret_cast<std::byte*>(const_cast<void*>(action));
    auto* status = reinterpret_cast<int*>(bytes + kIActionStatusOffset);
    auto* flags = reinterpret_cast<uint32_t*>(bytes + kIActionFlagsOffset);
    if (*status == kIActionStatusPending)
    {
        return CoopRuntimeGuards::TryWriteRuntimeValue(
            status,
            kIActionStatusFinished);
    }

    return CoopRuntimeGuards::TryWriteRuntimeValue(
        flags,
        *flags | kIActionBlendOutFlag | kIActionStoppingFlag);
}

bool IsAnimationTestGameReady()
{
    return gEnv && gEnv->pEntitySystem && ArkPlayer::GetInstancePtr() && ArkPlayer::GetInstance().GetEntity();
}

std::string PointerHex(const void* pointer)
{
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << reinterpret_cast<std::uintptr_t>(pointer);
    return out.str();
}

std::string AnimationStatusToken(std::string value)
{
    if (value.empty())
        return "-";
    for (char& ch : value)
    {
        if (ch <= ' ' || ch == '=' || ch == '"' || ch == '\'' || ch == '`')
            ch = '_';
    }
    return value;
}

bool LoadMannequinFragmentNamesFromXml(const char* path, std::vector<std::string>& outNames, std::string& status)
{
    outNames.clear();
    if (!gCL || !gCL->pXmlUtils || !path || !path[0])
    {
        status = "no_xml_utils";
        return false;
    }

    pugi::xml_parse_result parseResult;
    pugi::xml_document doc = gCL->pXmlUtils->LoadXmlFromFile(path, &parseResult);
    if (!parseResult)
    {
        status = std::string("parse_failed_") + parseResult.description();
        return false;
    }

    pugi::xml_node tags = doc.child("TagDefinition").child("Tags");
    if (!tags)
    {
        status = "missing_TagDefinition_Tags";
        return false;
    }

    for (pugi::xml_node tag = tags.child("Tag"); tag; tag = tag.next_sibling("Tag"))
    {
        const char* name = tag.attribute("name").as_string(nullptr);
        outNames.emplace_back(name ? name : "");
    }

    status = "ok_" + std::to_string(outNames.size());
    return !outNames.empty();
}

bool LoadMannequinFragmentNamesWithFallback(
    std::initializer_list<const char*> paths,
    std::vector<std::string>& outNames,
    std::string& status)
{
    std::string lastStatus;
    for (const char* path : paths)
    {
        if (LoadMannequinFragmentNamesFromXml(path, outNames, lastStatus))
        {
            status = std::string("ok_") + path + "_" + std::to_string(outNames.size());
            return true;
        }
    }

    status = lastStatus.empty() ? "failed_no_paths" : lastStatus;
    return false;
}

std::string MannequinKindFromText(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (value.find("mimic") != std::string::npos)
        return "mimic";
    if (value.find("phantom") != std::string::npos ||
        value.find("etheric") != std::string::npos ||
        value.find("thermal") != std::string::npos ||
        value.find("voltaic") != std::string::npos ||
        value.find("beta") != std::string::npos)
    {
        return "phantom";
    }
    if (value.find("nightmare") != std::string::npos)
        return "nightmare";
    if (value.find("poltergeist") != std::string::npos)
        return "poltergeist";
    if (value.find("telepath") != std::string::npos)
        return "telepath";
    if (value.find("technopath") != std::string::npos)
        return "technopath";
    if (value.find("weaver") != std::string::npos)
        return "weaver";
    // Military Operators ship a different ControllerDef, fragment-id table,
    // and ADB from the science/medical/engineering Operator family. Detect
    // this before the generic token or fragment ids such as Melee/Hacking are
    // silently decoded as unrelated generic-Operator cutscene fragments.
    if (value.find("militaryoperator") != std::string::npos ||
        value.find("operator military") != std::string::npos ||
        value.find("operatormilitary") != std::string::npos)
    {
        return "operator_military";
    }
    if (value.find("operator") != std::string::npos)
        return "operator";
    return "human";
}

std::string MannequinKindFromEntity(IEntity* entity)
{
    if (!entity || !CoopRuntimeGuards::IsLikelyRuntimeCppObject(entity, sizeof(void*) * 4))
        return "human";

    std::string text;
    std::string guardReason;
    IEntityArchetype* archetype = nullptr;
    IEntityClass* entityClass = nullptr;
    const char* rawName = nullptr;

    if (TryGuardedCall(
            "mannequin kind IEntity::GetArchetype",
            [entity]() { return entity->GetArchetype(); },
            archetype,
            &guardReason) &&
        archetype &&
        CoopRuntimeGuards::IsLikelyRuntimeCppObject(archetype, sizeof(void*) * 4))
    {
        const char* archetypeName = nullptr;
        if (TryGuardedCall(
                "mannequin kind archetype GetName",
                [archetype]() { return archetype->GetName(); },
                archetypeName,
                &guardReason))
        {
            text += CoopRuntimeGuards::ReadRuntimeCString(archetypeName, 128);
        }
    }

    if (TryGuardedCall(
            "mannequin kind IEntity::GetClass",
            [entity]() { return entity->GetClass(); },
            entityClass,
            &guardReason) &&
        entityClass &&
        CoopRuntimeGuards::IsLikelyRuntimeCppObject(entityClass, sizeof(void*) * 4))
    {
        const char* className = nullptr;
        if (TryGuardedCall(
                "mannequin kind class GetName",
                [entityClass]() { return entityClass->GetName(); },
                className,
                &guardReason))
        {
            const std::string classText = CoopRuntimeGuards::ReadRuntimeCString(className, 128);
            if (!text.empty())
                text += " ";
            text += classText;
        }
    }

    if (TryGuardedCall(
            "mannequin kind IEntity::GetName",
            [entity]() { return entity->GetName(); },
            rawName,
            &guardReason))
    {
        const std::string entityName = CoopRuntimeGuards::ReadRuntimeCString(rawName, 128);
        if (!text.empty())
            text += " ";
        text += entityName;
    }

    return MannequinKindFromText(text);
}

void EnsureFragmentNamesForKind(MannequinFragmentNameTable& table, const std::string& kind)
{
    auto load = [](bool& attempted, std::vector<std::string>& names, std::string& status, std::initializer_list<const char*> paths)
    {
        if (attempted && !names.empty())
            return;
        attempted = true;
        LoadMannequinFragmentNamesWithFallback(paths, names, status);
    };

    if (kind == "mimic")
    {
        load(
            table.mimicAttempted,
            table.mimic,
            table.mimicStatus,
            {
                "Animations/Mannequin/ADB/Ai_Mimic_FragmentIds.xml",
                "Animations/Mannequin/adb/ai_mimic_fragmentids.xml",
            });
        return;
    }
    if (kind == "phantom")
    {
        load(
            table.phantomAttempted,
            table.phantom,
            table.phantomStatus,
            {
                "Animations/Mannequin/ADB/Ai_Phantom_FragmentIds.xml",
                "Animations/Mannequin/adb/ai_phantom_fragmentids.xml",
            });
        return;
    }
    if (kind == "nightmare")
    {
        load(
            table.nightmareAttempted,
            table.nightmare,
            table.nightmareStatus,
            {
                "Animations/Mannequin/ADB/Ai_Nightmare_FragmentIds.xml",
                "Animations/Mannequin/adb/ai_nightmare_fragmentids.xml",
            });
        return;
    }
    if (kind == "poltergeist")
    {
        load(
            table.poltergeistAttempted,
            table.poltergeist,
            table.poltergeistStatus,
            {
                "Animations/Mannequin/ADB/Ai_Poltergeist_FragmentIds.xml",
                "Animations/Mannequin/adb/ai_poltergeist_fragmentids.xml",
            });
        return;
    }
    if (kind == "telepath")
    {
        load(
            table.telepathAttempted,
            table.telepath,
            table.telepathStatus,
            {
                "Animations/Mannequin/ADB/Ai_Telepath_FragmentIds.xml",
                "Animations/Mannequin/adb/ai_telepath_fragmentids.xml",
            });
        return;
    }
    if (kind == "technopath")
    {
        // Technopath has no own Mannequin ADB dump. Its floating movement
        // presentation is authored in the Weaver fragment set.
        load(
            table.technopathAttempted,
            table.technopath,
            table.technopathStatus,
            {
                "Animations/Mannequin/ADB/Ai_Weaver_FragmentIds.xml",
                "Animations/Mannequin/adb/ai_weaver_fragmentids.xml",
            });
        return;
    }
    if (kind == "weaver")
    {
        load(
            table.weaverAttempted,
            table.weaver,
            table.weaverStatus,
            {
                "Animations/Mannequin/ADB/Ai_Weaver_FragmentIds.xml",
                "Animations/Mannequin/adb/ai_weaver_fragmentids.xml",
            });
        return;
    }
    if (kind == "operator_military")
    {
        load(
            table.operatorMilitaryAttempted,
            table.operatorMilitary,
            table.operatorMilitaryStatus,
            {
                "Animations/Mannequin/ADB/Ai_OperatorMilitary_FragmentIds.xml",
                "Animations/Mannequin/adb/ai_operatormilitary_fragmentids.xml",
            });
        return;
    }
    if (kind == "operator")
    {
        load(
            table.operatorAttempted,
            table.op,
            table.operatorStatus,
            {
                "Animations/Mannequin/ADB/Ai_Operator_FragmentIds.xml",
                "Animations/Mannequin/adb/ai_operator_fragmentids.xml",
            });
    }
}

MannequinFragmentNameTable& GetMannequinFragmentNameTable()
{
    static MannequinFragmentNameTable table;
    if (!table.humanAttempted || table.human.empty())
    {
        table.humanAttempted = true;
        LoadMannequinFragmentNamesWithFallback(
            {
                "Animations/Mannequin/ADB/Ai_Human_FragmentIds.xml",
                "Animations/Mannequin/adb/ai_human_fragmentids.xml",
            },
            table.human,
            table.humanStatus);
    }

    if (!table.playerAttempted || table.player.empty())
    {
        table.playerAttempted = true;
        LoadMannequinFragmentNamesWithFallback(
            {
                "Animations/Mannequin/ADB/ArkPlayerFragments.xml",
                "Animations/Mannequin/adb/arkplayerfragments.xml",
            },
            table.player,
            table.playerStatus);
    }

    return table;
}

const char* FragmentNameAt(const std::vector<std::string>& names, int fragmentId)
{
    if (fragmentId < 0 || static_cast<size_t>(fragmentId) >= names.size())
        return nullptr;

    const std::string& name = names[static_cast<size_t>(fragmentId)];
    return name.empty() ? nullptr : name.c_str();
}

bool IsKnownMannequinFragmentId(int fragmentId)
{
    MannequinFragmentNameTable& table = GetMannequinFragmentNameTable();
    return FragmentNameAt(table.human, fragmentId) || FragmentNameAt(table.player, fragmentId);
}

std::string ResolveMannequinFragmentName(int fragmentId)
{
    if (fragmentId < 0)
        return "-";

    MannequinFragmentNameTable& table = GetMannequinFragmentNameTable();
    const char* humanName = FragmentNameAt(table.human, fragmentId);
    const char* playerName = FragmentNameAt(table.player, fragmentId);

    if (humanName && playerName)
    {
        if (std::strcmp(humanName, playerName) == 0)
            return std::string("both:") + humanName;
        return std::string("human:") + humanName + "|player:" + playerName;
    }

    if (humanName)
        return std::string("human:") + humanName;
    if (playerName)
        return std::string("player:") + playerName;

    return "unknown";
}

const std::vector<std::string>& FragmentNamesForKind(MannequinFragmentNameTable& table, const std::string& kind, std::string& status)
{
    EnsureFragmentNamesForKind(table, kind);
    if (kind == "mimic")
    {
        status = table.mimicStatus;
        return table.mimic;
    }
    if (kind == "phantom")
    {
        status = table.phantomStatus;
        return table.phantom;
    }
    if (kind == "nightmare")
    {
        status = table.nightmareStatus;
        return table.nightmare;
    }
    if (kind == "poltergeist")
    {
        status = table.poltergeistStatus;
        return table.poltergeist;
    }
    if (kind == "telepath")
    {
        status = table.telepathStatus;
        return table.telepath;
    }
    if (kind == "technopath")
    {
        status = table.technopathStatus;
        return table.technopath;
    }
    if (kind == "weaver")
    {
        status = table.weaverStatus;
        return table.weaver;
    }
    if (kind == "operator_military")
    {
        status = table.operatorMilitaryStatus;
        return table.operatorMilitary;
    }
    if (kind == "operator")
    {
        status = table.operatorStatus;
        return table.op;
    }

    status = table.humanStatus;
    return table.human;
}

std::string ResolveMannequinFragmentNameForKind(const std::string& kind, int fragmentId)
{
    if (fragmentId < 0)
        return "-";

    MannequinFragmentNameTable& table = GetMannequinFragmentNameTable();
    std::string status;
    const std::vector<std::string>& names = FragmentNamesForKind(table, kind, status);
    if (const char* name = FragmentNameAt(names, fragmentId))
        return kind + ":" + name;

    return kind + ":unknown";
}

int ResolveMannequinFragmentIdForKind(const std::string& kind, const char* fragmentName)
{
    if (!fragmentName || !fragmentName[0])
        return -1;

    std::string requested = fragmentName;
    const size_t colon = requested.find(':');
    if (colon != std::string::npos)
        requested = requested.substr(colon + 1);

    MannequinFragmentNameTable& table = GetMannequinFragmentNameTable();
    std::string status;
    const std::vector<std::string>& names = FragmentNamesForKind(table, kind, status);
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (names[i] == requested)
            return static_cast<int>(i);
    }

    auto lowerAscii = [](std::string value)
    {
        std::transform(
            value.begin(),
            value.end(),
            value.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    };
    const std::string requestedLower = lowerAscii(requested);
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (lowerAscii(names[i]) == requestedLower)
            return static_cast<int>(i);
    }

    return -1;
}

bool ResolveMannequinVariantForKind(
    const std::string& kind,
    const std::string& fragmentNameWithKind,
    int ordinal,
    MannequinSnippetEntry& outEntry,
    std::string& detail)
{
    if (!gCL || !gCL->pXmlUtils)
    {
        detail = "no_xml_utils";
        return false;
    }

    std::string fragmentName = fragmentNameWithKind;
    const size_t colon = fragmentName.find(':');
    if (colon != std::string::npos)
        fragmentName = fragmentName.substr(colon + 1);
    if (fragmentName.empty() || fragmentName == "-" || fragmentName == "unknown" || ordinal < 0)
    {
        detail = "bad_fragment_or_ordinal";
        return false;
    }

    std::string lastStatus = "no_paths";
    for (const char* path : MannequinDatabasePaths(kind))
    {
        pugi::xml_parse_result parseResult;
        pugi::xml_document doc = gCL->pXmlUtils->LoadXmlFromFile(path, &parseResult);
        if (!parseResult)
        {
            lastStatus = std::string("parse_failed_") + path + "_" + parseResult.description();
            continue;
        }

        pugi::xml_node fragmentNode = doc.child("AnimDB").child("FragmentList").child(fragmentName.c_str());
        if (!fragmentNode)
        {
            lastStatus = std::string("missing_fragment_") + fragmentName + "_path_" + path;
            continue;
        }

        int currentOrdinal = 0;
        for (pugi::xml_node variantNode = fragmentNode.child("Fragment"); variantNode; variantNode = variantNode.next_sibling("Fragment"))
        {
            if (currentOrdinal == ordinal)
            {
                outEntry = BuildSnippetEntry(fragmentNode, variantNode, currentOrdinal);
                detail =
                    "variant_ok kind=" + kind +
                    " fragment=" + fragmentName +
                    " ordinal=" + std::to_string(ordinal) +
                    " tags=" + outEntry.tags +
                    " fragTags=" + outEntry.fragTags +
                    " path=" + path;
                return true;
            }
            ++currentOrdinal;
        }

        lastStatus = "ordinal_out_of_range_" + std::to_string(ordinal) +
            "_count_" + std::to_string(currentOrdinal) +
            "_fragment_" + fragmentName +
            "_path_" + path;
    }

    detail = lastStatus;
    return false;
}

bool HasAnyVariantToken(const std::string& lower, std::initializer_list<const char*> tokens)
{
    for (const char* token : tokens)
    {
        if (ContainsToken(lower, token))
            return true;
    }
    return false;
}

bool HasMannequinMovementToken(const std::string& lower)
{
    return HasAnyVariantToken(lower, {
        "motion_move",
        "movestart",
        "movestop",
        "move",
        "moving",
        "wander",
        "patrol",
        "lurk",
        "walk",
        "run",
        "bspace",
        "strafe",
        "slope",
        "sprint",
        "charge",
    });
}

std::string MannequinVariantTagsAndAnimations(const MannequinSnippetEntry& entry)
{
    return ToLowerAsciiAnimation(
        entry.tags + "+" +
        entry.fragTags + "+" +
        entry.animations);
}

bool MannequinVariantSupportsRequiredFlags(
    const MannequinSnippetEntry& entry,
    uint32_t flags,
    bool movementLane)
{
    const std::string combined = ToLowerAsciiAnimation(
        entry.tags + "+" +
        entry.fragTags + "+" +
        entry.animations + "+" +
        entry.procedures);
    const std::string movementText = MannequinVariantTagsAndAnimations(entry);

    const bool wantsWalkOrRun =
        movementLane ||
        (flags & (CoopProtocol::kEnemyLocomotionFlagWalking |
            CoopProtocol::kEnemyLocomotionFlagRunning)) != 0;
    const bool wantsDash =
        (flags & (CoopProtocol::kEnemyLocomotionFlagDashing |
            CoopProtocol::kEnemyLocomotionFlagShifting)) != 0;
    const bool wantsMorph = (flags & CoopProtocol::kEnemyLocomotionFlagMorphing) != 0;
    const bool wantsGloo = (flags & CoopProtocol::kEnemyLocomotionFlagGlooed) != 0;
    const bool wantsStunOrCower =
        (flags & (CoopProtocol::kEnemyLocomotionFlagStunned |
            CoopProtocol::kEnemyLocomotionFlagCowering)) != 0;

    if (wantsWalkOrRun && !HasMannequinMovementToken(movementText))
        return false;
    if (wantsDash && !HasAnyVariantToken(combined, {"shift", "dash", "teleport", "jump"}))
        return false;
    if (wantsMorph && !HasAnyVariantToken(combined, {"morph", "mimic"}))
        return false;
    if (wantsGloo && !HasAnyVariantToken(combined, {"gloo", "glooed", "glood"}))
        return false;
    if (wantsStunOrCower && !HasAnyVariantToken(combined, {"stun", "cower", "fear"}))
        return false;

    return true;
}

int ScoreMannequinVariantForFlags(
    const MannequinSnippetEntry& entry,
    uint32_t desiredFlags,
    uint32_t localFlags,
    int remoteOrdinal)
{
    const std::string combined = ToLowerAsciiAnimation(
        entry.tags + "+" +
        entry.fragTags + "+" +
        entry.animations + "+" +
        entry.procedures);
    const std::string movementText = MannequinVariantTagsAndAnimations(entry);
    int score = 0;

    if (entry.ordinal == remoteOrdinal)
        score += 24;

    const bool wantsMovement =
        (desiredFlags & (CoopProtocol::kEnemyLocomotionFlagWalking |
            CoopProtocol::kEnemyLocomotionFlagRunning)) != 0;
    const bool wantsRun = (desiredFlags & CoopProtocol::kEnemyLocomotionFlagRunning) != 0;
    const bool wantsDash =
        (desiredFlags & (CoopProtocol::kEnemyLocomotionFlagDashing |
            CoopProtocol::kEnemyLocomotionFlagShifting)) != 0;
    const bool wantsAttack = (localFlags & CoopProtocol::kEnemyLocomotionFlagAttacking) != 0;
    const bool wantsTurn = (localFlags & CoopProtocol::kEnemyLocomotionFlagTurning) != 0;
    const bool wantsCombat = (desiredFlags & CoopProtocol::kEnemyLocomotionFlagInCombat) != 0;
    const bool wantsHard =
        (desiredFlags & (CoopProtocol::kEnemyLocomotionFlagGlooed |
            CoopProtocol::kEnemyLocomotionFlagStunned |
            CoopProtocol::kEnemyLocomotionFlagCowering |
            CoopProtocol::kEnemyLocomotionFlagMorphing)) != 0;

    if (wantsMovement)
    {
        if (HasMannequinMovementToken(movementText))
            score += 36;
        if (wantsRun && HasAnyVariantToken(combined, {"run", "charge", "sprint"}))
            score += 20;
        if (!wantsRun && HasAnyVariantToken(movementText, {"walk", "wander", "patrol", "lurk"}))
            score += 18;
        if (!wantsRun && HasAnyVariantToken(combined, {"run", "charge", "sprint", "escape"}))
            score -= 24;
        if (!HasMannequinMovementToken(movementText))
            score -= 64;
        if (HasAnyVariantToken(combined, {"idle", "stand"}))
            score -= 10;
    }
    else
    {
        if (HasAnyVariantToken(combined, {"move", "moving", "walk", "run", "charge", "sprint"}))
            score -= 34;
        if (HasAnyVariantToken(combined, {"idle", "stand", "combat"}))
            score += 10;
    }

    if (wantsDash)
    {
        if (HasAnyVariantToken(combined, {"shift", "dash", "teleport", "jump"}))
            score += 45;
    }
    else if (HasAnyVariantToken(combined, {"shift", "dash", "teleport", "jump"}))
    {
        score -= 38;
    }

    if (wantsAttack)
    {
        if (HasAnyVariantToken(combined, {"attack", "fire", "shoot", "cast", "power", "melee", "combat", "hunt"}))
            score += 28;
    }
    if (wantsCombat)
    {
        if (HasAnyVariantToken(combined, {"incombat", "combat", "hunt"}))
            score += 30;
        if (HasAnyVariantToken(combined, {"relaxed", "disable"}))
            score -= 14;
    }
    if (wantsTurn)
    {
        if (HasAnyVariantToken(combined, {"turn", "look", "aim"}))
            score += 16;
        if (wantsMovement)
            score -= 8;
    }
    if (wantsHard)
    {
        if ((desiredFlags & CoopProtocol::kEnemyLocomotionFlagGlooed) != 0 &&
            HasAnyVariantToken(combined, {"gloo", "glooed", "glood"}))
        {
            score += 70;
        }
        if ((desiredFlags & CoopProtocol::kEnemyLocomotionFlagMorphing) != 0 &&
            HasAnyVariantToken(combined, {"morph", "mimic"}))
        {
            score += 55;
        }
        if ((desiredFlags & (CoopProtocol::kEnemyLocomotionFlagStunned |
                CoopProtocol::kEnemyLocomotionFlagCowering)) != 0 &&
            HasAnyVariantToken(combined, {"stun", "cower", "fear"}))
        {
            score += 45;
        }
    }

    if (HasAnyVariantToken(combined, {"disable"}))
        score -= 12;

    return score - entry.ordinal;
}

std::string StripMannequinKindPrefix(const std::string& fragmentName)
{
    const size_t colon = fragmentName.find(':');
    if (colon == std::string::npos)
        return fragmentName;
    return fragmentName.substr(colon + 1);
}

bool IsIntegerText(const std::string& value)
{
    if (value.empty())
        return false;

    size_t i = value[0] == '-' || value[0] == '+' ? 1 : 0;
    if (i >= value.size())
        return false;

    for (; i < value.size(); ++i)
    {
        if (!std::isdigit(static_cast<unsigned char>(value[i])))
            return false;
    }
    return true;
}

std::string ToLowerAsciiAnimation(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool ContainsToken(const std::string& haystack, const char* needle)
{
    return needle && needle[0] && haystack.find(needle) != std::string::npos;
}

enum EnemyMannequinAttackKind : uint32_t
{
    kEnemyMannequinAttackGeneric = 1,
    kEnemyMannequinAttackPsi = 2,
    kEnemyMannequinAttackProjectile = 3,
    kEnemyMannequinAttackElectric = 4,
    kEnemyMannequinAttackThermal = 5,
    kEnemyMannequinAttackMelee = 6,
    kEnemyMannequinAttackGloo = 7,
};

uint32_t ClassifyEnemyMannequinAttackKind(const std::string& lower)
{
    if (ContainsToken(lower, "psiattack") ||
        ContainsToken(lower, "psiblast") ||
        ContainsToken(lower, "power_") ||
        ContainsToken(lower, "cast"))
    {
        return kEnemyMannequinAttackPsi;
    }
    if (ContainsToken(lower, "lightning") ||
        ContainsToken(lower, "emp"))
    {
        return kEnemyMannequinAttackElectric;
    }
    if (ContainsToken(lower, "thermal") ||
        ContainsToken(lower, "fire"))
    {
        return kEnemyMannequinAttackThermal;
    }
    if (ContainsToken(lower, "projectile") ||
        ContainsToken(lower, "shoot") ||
        ContainsToken(lower, "wpn_fire"))
    {
        return kEnemyMannequinAttackProjectile;
    }
    if (ContainsToken(lower, "gloo") ||
        ContainsToken(lower, "breakgloo"))
    {
        return kEnemyMannequinAttackGloo;
    }
    if (ContainsToken(lower, "grabplayer") ||
        ContainsToken(lower, "jumpattack") ||
        ContainsToken(lower, "pounce") ||
        ContainsToken(lower, "lunge") ||
        ContainsToken(lower, "leap") ||
        ContainsToken(lower, "melee") ||
        ContainsToken(lower, "attack"))
    {
        return kEnemyMannequinAttackMelee;
    }
    return kEnemyMannequinAttackGeneric;
}

uint16_t ClassifyEnemyAbilityFxKindForFragment(const std::string& lower)
{
    if (ContainsToken(lower, "poltergeist:power_lift") ||
        ContainsToken(lower, "power_lift"))
    {
        return CoopProtocol::kEnemyAbilityFxPoltergeistLift;
    }
    if (ContainsToken(lower, "poltergeist:power_throw") ||
        ContainsToken(lower, "power_throw"))
    {
        return CoopProtocol::kEnemyAbilityFxPoltergeistThrow;
    }
    if (ContainsToken(lower, "weaver:create_cystoid") ||
        ContainsToken(lower, "create_cystoid"))
    {
        return CoopProtocol::kEnemyAbilityFxWeaverCreateCystoid;
    }
    if (ContainsToken(lower, "weaver:alarm_call") ||
        ContainsToken(lower, "alarm_call"))
    {
        return CoopProtocol::kEnemyAbilityFxWeaverAlarmCall;
    }
    if (ContainsToken(lower, "psiattack"))
        return CoopProtocol::kEnemyAbilityFxPsiAttack;
    return CoopProtocol::kEnemyAbilityFxNone;
}

struct EnemyMannequinClassification
{
    uint32_t flags = 0;
    uint32_t attackKind = 0;
};

EnemyMannequinClassification ClassifyEnemyMannequinFragment(int fragmentId, const std::string& fragmentName)
{
    const std::string lower = ToLowerAsciiAnimation(fragmentName);
    EnemyMannequinClassification result;
    (void)fragmentId;
    const uint16_t abilityFxKind = ClassifyEnemyAbilityFxKindForFragment(lower);

    const bool physicsRecovery =
        ContainsToken(lower, "fall") ||
        ContainsToken(lower, "land") ||
        ContainsToken(lower, "stumble") ||
        ContainsToken(lower, "ragdoll") ||
        ContainsToken(lower, "recovery") ||
        ContainsToken(lower, "recover");
    if (physicsRecovery)
        result.flags |= CoopProtocol::kEnemyLocomotionFlagHitReacting;

    if (ContainsToken(lower, "motion_move") ||
        ContainsToken(lower, "idle_to_move") ||
        ContainsToken(lower, "idletomove") ||
        ContainsToken(lower, "walk") ||
        ContainsToken(lower, "run") ||
        ContainsToken(lower, "move"))
    {
        if (!physicsRecovery)
        {
            result.flags |= CoopProtocol::kEnemyLocomotionFlagWalking;
            if (ContainsToken(lower, "run") || ContainsToken(lower, "charge"))
                result.flags |= CoopProtocol::kEnemyLocomotionFlagRunning;
        }
    }

    if (ContainsToken(lower, "idleturn") ||
        ContainsToken(lower, "idle_turn") ||
        ContainsToken(lower, "turn") ||
        ContainsToken(lower, "juketurn") ||
        ContainsToken(lower, "patrolidle") ||
        ContainsToken(lower, "wanderidle") ||
        ContainsToken(lower, "search") ||
        ContainsToken(lower, "distractor") ||
        ContainsToken(lower, "look") ||
        ContainsToken(lower, "notice") ||
        ContainsToken(lower, "inspect") ||
        ContainsToken(lower, "interact") ||
        ContainsToken(lower, "speakerreact") ||
        ContainsToken(lower, "lightreact") ||
        ContainsToken(lower, "lurk"))
    {
        result.flags |= CoopProtocol::kEnemyLocomotionFlagTurning;
    }

    const bool authoredBurstMovement =
        ContainsToken(lower, "shift") ||
        ContainsToken(lower, "dash") ||
        ContainsToken(lower, "teleport") ||
        ContainsToken(lower, "jump") ||
        CoopEnemyControlPolicy::FragmentNameIsAuthoredBurstMovement(fragmentName);
    if (authoredBurstMovement)
    {
        if (CoopEnemyControlPolicy::FragmentNameIsPhantomDash(fragmentName))
        {
            result.flags |= CoopProtocol::kEnemyLocomotionFlagDashing |
                CoopProtocol::kEnemyLocomotionFlagShifting;
        }
        else
        {
            result.flags |= CoopProtocol::kEnemyLocomotionFlagLunging;
        }
    }

    if (ContainsToken(lower, "morph"))
        result.flags |= CoopProtocol::kEnemyLocomotionFlagMorphing;

    if (ContainsToken(lower, "glood") ||
        ContainsToken(lower, "glooed") ||
        ContainsToken(lower, "gloo_pose") ||
        ContainsToken(lower, "breakgloo"))
    {
        result.flags |= CoopProtocol::kEnemyLocomotionFlagGlooed;
    }

    if (ContainsToken(lower, "stun"))
        result.flags |= CoopProtocol::kEnemyLocomotionFlagStunned;
    if (ContainsToken(lower, "cower") || ContainsToken(lower, "fear"))
        result.flags |= CoopProtocol::kEnemyLocomotionFlagCowering;
    if (ContainsToken(lower, "reaction") ||
        ContainsToken(lower, "hit") ||
        ContainsToken(lower, "damage"))
    {
        result.flags |= CoopProtocol::kEnemyLocomotionFlagHitReacting;
    }

    if (abilityFxKind != CoopProtocol::kEnemyAbilityFxNone ||
        ContainsToken(lower, "wpn_fire") ||
        ContainsToken(lower, "attack") ||
        ContainsToken(lower, "power_") ||
        ContainsToken(lower, "psiattack") ||
        ContainsToken(lower, "psiblast") ||
        ContainsToken(lower, "lightning") ||
        ContainsToken(lower, "emp") ||
        ContainsToken(lower, "shoot") ||
        ContainsToken(lower, "fire") ||
        ContainsToken(lower, "projectile") ||
        ContainsToken(lower, "cast") ||
        ContainsToken(lower, "melee") ||
        ContainsToken(lower, "breakgloo") ||
        CoopEnemyControlPolicy::FragmentNameIsAuthoredMovementAction(fragmentName))
    {
        result.flags |= CoopProtocol::kEnemyLocomotionFlagAttacking;
        result.attackKind = abilityFxKind != CoopProtocol::kEnemyAbilityFxNone
            ? kEnemyMannequinAttackPsi
            : ClassifyEnemyMannequinAttackKind(lower);
    }

    if (result.flags != 0)
        result.flags |= CoopProtocol::kEnemyLocomotionFlagMannequinDriven;

    return result;
}

bool IsEnemyMannequinCarryFragment(const char* fragmentName)
{
    return CoopEnemyControlPolicy::FragmentNameCarriesPassiveMovement(
        fragmentName && fragmentName[0] ? std::string_view(fragmentName) : std::string_view());
}

std::string BuildMannequinFragmentDump(const std::string& kind, int start, int count, const std::string& filter)
{
    MannequinFragmentNameTable& table = GetMannequinFragmentNameTable();
    const bool player = kind == "player" || kind == "arkplayer";
    const std::vector<std::string>& names = player ? table.player : table.human;
    const std::string& status = player ? table.playerStatus : table.humanStatus;
    const char* tableName = player ? "player" : "human";

    start = std::max(0, start);
    count = std::clamp(count, 1, 48);
    const std::string lowerFilter = filter == "-" || filter == "*" ? std::string() : ToLowerAsciiAnimation(filter);

    std::ostringstream out;
    out
        << "anim_fragments"
        << " table=" << tableName
        << " status=" << status
        << " total=" << names.size()
        << " start=" << start
        << " count=" << count
        << " filter=" << (lowerFilter.empty() ? std::string("-") : lowerFilter);

    int emitted = 0;
    for (int i = start; i < static_cast<int>(names.size()) && emitted < count; ++i)
    {
        const std::string& name = names[static_cast<size_t>(i)];
        if (!lowerFilter.empty() && ToLowerAsciiAnimation(name).find(lowerFilter) == std::string::npos)
            continue;

        out << " " << i << "=" << (name.empty() ? "-" : name);
        ++emitted;
    }
    out << " emitted=" << emitted;

    return out.str();
}

const char* NonEmptyAttr(pugi::xml_node node, const char* name)
{
    const char* value = node.attribute(name).as_string(nullptr);
    return value && value[0] ? value : "-";
}

void AppendCsvToken(std::string& out, const std::string& value)
{
    if (value.empty())
        return;
    if (!out.empty())
        out += ",";
    out += value;
}

std::string CompactSnippetText(std::string value)
{
    for (char& ch : value)
    {
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
            ch = '_';
    }
    return value.empty() ? std::string("-") : value;
}

std::vector<const char*> MannequinDatabasePaths(const std::string& kind)
{
    if (kind == "player" || kind == "arkplayer" || kind == "player3p")
    {
        return {
            "Animations/Mannequin/ADB/arkplayerdatabase3p.adb",
            "Animations/Mannequin/adb/arkplayerdatabase3p.adb",
            "Animations/Mannequin/ADB/ArkPlayerDatabase3P.adb",
        };
    }
    if (kind == "mimic")
    {
        return {
            "Animations/Mannequin/ADB/ai_mimic_database.adb",
            "Animations/Mannequin/adb/ai_mimic_database.adb",
            "Animations/Mannequin/ADB/Ai_Mimic_Database.adb",
        };
    }
    if (kind == "phantom")
    {
        return {
            "Animations/Mannequin/ADB/ai_phantom_database.adb",
            "Animations/Mannequin/adb/ai_phantom_database.adb",
            "Animations/Mannequin/ADB/Ai_Phantom_Database.adb",
        };
    }
    if (kind == "nightmare")
    {
        return {
            "Animations/Mannequin/ADB/ai_nightmare_database.adb",
            "Animations/Mannequin/adb/ai_nightmare_database.adb",
            "Animations/Mannequin/ADB/Ai_Nightmare_Database.adb",
        };
    }
    if (kind == "poltergeist")
    {
        return {
            "Animations/Mannequin/ADB/ai_poltergeist_database.adb",
            "Animations/Mannequin/adb/ai_poltergeist_database.adb",
            "Animations/Mannequin/ADB/Ai_Poltergeist_Database.adb",
        };
    }
    if (kind == "telepath")
    {
        return {
            "Animations/Mannequin/ADB/ai_telepath_database.adb",
            "Animations/Mannequin/adb/ai_telepath_database.adb",
            "Animations/Mannequin/ADB/Ai_Telepath_Database.adb",
        };
    }
    if (kind == "technopath")
    {
        // There is no ai_technopath_database.adb in the shipped Mannequin data.
        // The authored Technopath movement cues found in the dump live in the
        // Weaver database (for example Play_TechnoPath_Vox_Move).
        return {
            "Animations/Mannequin/ADB/ai_weaver_database.adb",
            "Animations/Mannequin/adb/ai_weaver_database.adb",
            "Animations/Mannequin/ADB/Ai_Weaver_Database.adb",
        };
    }
    if (kind == "weaver")
    {
        return {
            "Animations/Mannequin/ADB/ai_weaver_database.adb",
            "Animations/Mannequin/adb/ai_weaver_database.adb",
            "Animations/Mannequin/ADB/Ai_Weaver_Database.adb",
        };
    }
    if (kind == "operator_military")
    {
        return {
            "Animations/Mannequin/ADB/ai_operatormilitary_database.adb",
            "Animations/Mannequin/adb/ai_operatormilitary_database.adb",
            "Animations/Mannequin/ADB/Ai_OperatorMilitary_Database.adb",
        };
    }
    if (kind == "operator")
    {
        return {
            "Animations/Mannequin/ADB/ai_operator_database.adb",
            "Animations/Mannequin/adb/ai_operator_database.adb",
            "Animations/Mannequin/ADB/Ai_Operator_Database.adb",
        };
    }

    return {
        "Animations/Mannequin/ADB/ai_human_database.adb",
        "Animations/Mannequin/adb/ai_human_database.adb",
        "Animations/Mannequin/ADB/Ai_Human_Database.adb",
    };
}

std::string MannequinTableName(const std::string& kind)
{
    if (kind == "player" || kind == "arkplayer" || kind == "player3p")
        return "player3p";
    if (kind == "mimic" ||
        kind == "phantom" ||
        kind == "nightmare" ||
        kind == "poltergeist" ||
        kind == "telepath" ||
        kind == "technopath" ||
        kind == "weaver" ||
        kind == "operator_military" ||
        kind == "operator")
    {
        return kind;
    }
    return "human";
}

bool SnippetMatchesFilter(
    const MannequinSnippetEntry& entry,
    const std::string& lowerQuery,
    const std::string& lowerTagFilter)
{
    const std::string haystack = ToLowerAsciiAnimation(
        entry.fragment + " " +
        entry.tags + " " +
        entry.fragTags + " " +
        entry.animations + " " +
        entry.animationFlags + " " +
        entry.procedures);

    if (!lowerQuery.empty() && haystack.find(lowerQuery) == std::string::npos)
        return false;
    if (!lowerTagFilter.empty() && haystack.find(lowerTagFilter) == std::string::npos)
        return false;
    return true;
}

MannequinSnippetEntry BuildSnippetEntry(pugi::xml_node fragmentNode, pugi::xml_node variantNode, int ordinal)
{
    MannequinSnippetEntry entry;
    entry.fragment = fragmentNode.name() && fragmentNode.name()[0] ? fragmentNode.name() : "-";
    entry.tags = NonEmptyAttr(variantNode, "Tags");
    entry.fragTags = NonEmptyAttr(variantNode, "FragTags");
    entry.weight = NonEmptyAttr(variantNode, "Weight");
    entry.cooldown = NonEmptyAttr(variantNode, "Cooldown");
    entry.blendOut = NonEmptyAttr(variantNode, "BlendOutDuration");
    entry.ordinal = ordinal;

    for (pugi::xml_node layer = variantNode.child("AnimLayer"); layer; layer = layer.next_sibling("AnimLayer"))
    {
        for (pugi::xml_node anim = layer.child("Animation"); anim; anim = anim.next_sibling("Animation"))
        {
            AppendCsvToken(entry.animations, NonEmptyAttr(anim, "name"));
            AppendCsvToken(entry.animationFlags, NonEmptyAttr(anim, "flags"));
        }
    }

    for (pugi::xml_node layer = variantNode.child("ProcLayer"); layer; layer = layer.next_sibling("ProcLayer"))
    {
        for (pugi::xml_node proc = layer.child("Procedural"); proc; proc = proc.next_sibling("Procedural"))
            AppendCsvToken(entry.procedures, NonEmptyAttr(proc, "type"));
    }

    if (entry.animations.empty())
        entry.animations = "-";
    if (entry.animationFlags.empty())
        entry.animationFlags = "-";
    if (entry.procedures.empty())
        entry.procedures = "-";
    return entry;
}

std::string BuildMannequinSnippetDump(
    const std::string& kind,
    const std::string& query,
    int count,
    const std::string& tagFilter)
{
    if (!gCL || !gCL->pXmlUtils)
        return "anim_snippets_failed reason=no_xml_utils";

    const std::string tableName = MannequinTableName(kind);
    const std::string lowerQuery = query.empty() || query == "-" || query == "*"
        ? std::string()
        : ToLowerAsciiAnimation(query);
    const std::string lowerTagFilter = tagFilter.empty() || tagFilter == "-" || tagFilter == "*"
        ? std::string()
        : ToLowerAsciiAnimation(tagFilter);
    count = std::clamp(count, 1, 32);

    std::string lastStatus = "no_paths";
    for (const char* path : MannequinDatabasePaths(kind))
    {
        pugi::xml_parse_result parseResult;
        pugi::xml_document doc = gCL->pXmlUtils->LoadXmlFromFile(path, &parseResult);
        if (!parseResult)
        {
            lastStatus = std::string("parse_failed_") + path + "_" + parseResult.description();
            continue;
        }

        pugi::xml_node fragmentList = doc.child("AnimDB").child("FragmentList");
        if (!fragmentList)
        {
            lastStatus = std::string("missing_FragmentList_") + path;
            continue;
        }

        std::ostringstream out;
        out
            << "anim_snippets"
            << " table=" << tableName
            << " db=" << path
            << " query=" << (lowerQuery.empty() ? std::string("-") : lowerQuery)
            << " tag=" << (lowerTagFilter.empty() ? std::string("-") : lowerTagFilter)
            << " count=" << count;

        int scanned = 0;
        int emitted = 0;
        for (pugi::xml_node fragmentNode = fragmentList.first_child(); fragmentNode && emitted < count; fragmentNode = fragmentNode.next_sibling())
        {
            if (fragmentNode.type() != pugi::node_element)
                continue;

            int ordinal = 0;
            for (pugi::xml_node variantNode = fragmentNode.child("Fragment"); variantNode && emitted < count; variantNode = variantNode.next_sibling("Fragment"))
            {
                MannequinSnippetEntry entry = BuildSnippetEntry(fragmentNode, variantNode, ordinal++);
                ++scanned;
                if (!SnippetMatchesFilter(entry, lowerQuery, lowerTagFilter))
                    continue;

                out
                    << " #" << emitted
                    << "{frag=" << CompactSnippetText(entry.fragment)
                    << ",ord=" << entry.ordinal
                    << ",tags=" << CompactSnippetText(entry.tags)
                    << ",fragTags=" << CompactSnippetText(entry.fragTags)
                    << ",anim=" << CompactSnippetText(entry.animations)
                    << ",flags=" << CompactSnippetText(entry.animationFlags)
                    << ",proc=" << CompactSnippetText(entry.procedures)
                    << ",w=" << CompactSnippetText(entry.weight)
                    << ",cd=" << CompactSnippetText(entry.cooldown)
                    << ",out=" << CompactSnippetText(entry.blendOut)
                    << "}";
                ++emitted;
            }
        }

        out << " scanned=" << scanned << " emitted=" << emitted;
        return out.str();
    }

    return "anim_snippets_failed table=" + tableName + " reason=" + CompactSnippetText(lastStatus);
}

std::string BuildProxyAnimationStateCatalog(const std::string& filter)
{
    const std::string lowerFilter = filter.empty() || filter == "-" || filter == "*"
        ? std::string()
        : ToLowerAsciiAnimation(filter);

    struct CatalogEntry
    {
        const char* state;
        const char* nativePath;
        const char* fragments;
        const char* note;
    };

    constexpr CatalogEntry entries[] = {
        {"normal", "EndAnimatedStunned+StopStun+StopCowering", "Motion_Idle", "clears_blocking_states"},
        {"downed", "BeginAnimatedStunned->StartCowering->StartStun", "Stunned,Cowering", "current_downed_path"},
        {"downed_pose", "PoseHold combat_forceresist_front_out_empty@0.25", "ForceResist", "current_best_frozen_downed_candidate"},
        {"crouch", "StartCowering", "Cowering", "temporary_crouch_proxy"},
        {"crouch_pose", "PoseHold fear_cower_c_empty@0.30", "Cowering", "current_best_frozen_crouch_candidate"},
        {"hit", "Resist", "ForceResist,Reaction_Death", "flinch_or_force_feedback"},
        {"stunned", "StartStun", "Stunned", "timed_stun"},
        {"raise_start", "StartRaiseFromCorpse", "BlendRagdoll,Stunned", "candidate_getup_path"},
        {"raise_finish", "FinishRaiseFromCorpse", "Motion_Idle", "end_getup_path"},
        {"move", "deferred", "Motion_Move", "needs_locomotion_pose_sync"},
        {"fall", "deferred", "Fall,Land_Bump,Land_Stumble", "needs_movement_state_sync"},
    };

    std::ostringstream out;
    out << "anim_state_catalog";
    int emitted = 0;
    for (const CatalogEntry& entry : entries)
    {
        const std::string haystack = ToLowerAsciiAnimation(
            std::string(entry.state) + " " + entry.nativePath + " " + entry.fragments + " " + entry.note);
        if (!lowerFilter.empty() && haystack.find(lowerFilter) == std::string::npos)
            continue;

        out
            << " #" << emitted
            << "{state=" << entry.state
            << ",native=" << entry.nativePath
            << ",fragments=" << entry.fragments
            << ",note=" << entry.note
            << "}";
        ++emitted;
    }
    out << " emitted=" << emitted;
    return out.str();
}

std::filesystem::path GetCoopAnimationCatalogRoot()
{
    std::filesystem::path root = CoopFilesystem::EnvironmentPath("USERPROFILE");
    if (root.empty())
        return std::filesystem::path("CoopPrototype") / "AnimCatalog";

    root /= "Saved Games";
    root /= "Arkane Studios";
    root /= "Prey";
    root /= "CoopPrototype";
    root /= "AnimCatalog";
    return root;
}

bool WriteMannequinSnippetCatalog(const std::string& kind, std::string& detail)
{
    if (!gCL || !gCL->pXmlUtils)
    {
        detail = "anim_snippet_catalog_failed reason=no_xml_utils";
        return false;
    }

    const std::string tableName = MannequinTableName(kind);
    std::string lastStatus = "no_paths";
    for (const char* path : MannequinDatabasePaths(kind))
    {
        pugi::xml_parse_result parseResult;
        pugi::xml_document doc = gCL->pXmlUtils->LoadXmlFromFile(path, &parseResult);
        if (!parseResult)
        {
            lastStatus = std::string("parse_failed_") + path + "_" + parseResult.description();
            continue;
        }

        pugi::xml_node fragmentList = doc.child("AnimDB").child("FragmentList");
        if (!fragmentList)
        {
            lastStatus = std::string("missing_FragmentList_") + path;
            continue;
        }

        std::filesystem::path outputRoot = GetCoopAnimationCatalogRoot();
        std::error_code ec;
        std::filesystem::create_directories(outputRoot, ec);
        if (ec)
        {
            detail = "anim_snippet_catalog_failed reason=create_dir_" + CompactSnippetText(ec.message());
            return false;
        }

        const std::filesystem::path outputPath = outputRoot / (tableName + "_snippets.tsv");
        std::ofstream file(outputPath, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            detail = "anim_snippet_catalog_failed reason=open_failed path=" + CoopFilesystem::ToUtf8(outputPath);
            return false;
        }

        file << "table\tdatabase\tfragment\tordinal\ttags\tfragTags\tanimations\tanimationFlags\tprocedures\tweight\tcooldown\tblendOut\n";

        int fragmentCount = 0;
        int variantCount = 0;
        for (pugi::xml_node fragmentNode = fragmentList.first_child(); fragmentNode; fragmentNode = fragmentNode.next_sibling())
        {
            if (fragmentNode.type() != pugi::node_element)
                continue;

            ++fragmentCount;
            int ordinal = 0;
            for (pugi::xml_node variantNode = fragmentNode.child("Fragment"); variantNode; variantNode = variantNode.next_sibling("Fragment"))
            {
                MannequinSnippetEntry entry = BuildSnippetEntry(fragmentNode, variantNode, ordinal++);
                file
                    << tableName << '\t'
                    << path << '\t'
                    << entry.fragment << '\t'
                    << entry.ordinal << '\t'
                    << entry.tags << '\t'
                    << entry.fragTags << '\t'
                    << entry.animations << '\t'
                    << entry.animationFlags << '\t'
                    << entry.procedures << '\t'
                    << entry.weight << '\t'
                    << entry.cooldown << '\t'
                    << entry.blendOut << '\n';
                ++variantCount;
            }
        }

        detail =
            "anim_snippet_catalog_ok table=" + tableName +
            " fragments=" + std::to_string(fragmentCount) +
            " variants=" + std::to_string(variantCount) +
            " path=" + CoopFilesystem::ToUtf8(outputPath);
        return true;
    }

    detail = "anim_snippet_catalog_failed table=" + tableName + " reason=" + CompactSnippetText(lastStatus);
    return false;
}

template <typename T>
bool ReadFieldAt(const void* base, size_t offset, T& outValue)
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(base);
    return bytes && TryReadRuntimeValue(reinterpret_cast<const T*>(bytes + offset), outValue);
}

struct ActionBaseProbe
{
    const void* base = nullptr;
    int score = -1;
    std::uintptr_t vtable = 0;
    std::uintptr_t context = 0;
    int fragmentId = -1;
    int status = -1;
    uint32_t flags = 0;
    int priority = 0;
    int refCount = -1;
    ArkNpc* npc = nullptr;
};

ActionBaseProbe ProbeActionBaseCandidate(const void* base)
{
    ActionBaseProbe result;
    result.base = base;
    if (!CoopRuntimeGuards::IsReadableRuntimePointer(base, kIActionAnimWeightOffset + sizeof(float)))
        return result;

    float speedBias = 0.0f;
    float animWeight = 0.0f;
    bool ok = true;
    ok = ReadFieldAt(base, 0x00, result.vtable) && ok;
    ok = ReadFieldAt(base, kIActionContextOffset, result.context) && ok;
    ok = ReadFieldAt(base, kIActionPriorityOffset, result.priority) && ok;
    ok = ReadFieldAt(base, kIActionStatusOffset, result.status) && ok;
    ok = ReadFieldAt(base, kIActionFlagsOffset, result.flags) && ok;
    ok = ReadFieldAt(base, kIActionFragmentIdOffset, result.fragmentId) && ok;
    ok = ReadFieldAt(base, kIActionRefCountOffset, result.refCount) && ok;
    ok = ReadFieldAt(base, kIActionSpeedBiasOffset, speedBias) && ok;
    ok = ReadFieldAt(base, kIActionAnimWeightOffset, animWeight) && ok;
    ReadFieldAt(base, kArkNpcAnimActionNpcOffset, result.npc);
    if (!ok)
        return result;

    int score = 0;
    if (CoopRuntimeGuards::IsRuntimePointerInLoadedModule(reinterpret_cast<const void*>(result.vtable)))
        score += 3;
    if (result.context == 0 || CoopRuntimeGuards::IsReadableRuntimePointer(reinterpret_cast<const void*>(result.context), sizeof(void*)))
        score += 1;
    if (IsKnownMannequinFragmentId(result.fragmentId))
        score += 12;
    else if (result.fragmentId >= -1 && result.fragmentId < 100000)
        score += 2;
    else
        score -= 6;
    if (result.status >= 0 && result.status <= 4)
        score += 2;
    if ((result.flags & 0xFFF00000u) == 0)
        score += 1;
    if (result.priority >= 0 && result.priority < 32)
        score += 1;
    if (result.refCount >= 0 && result.refCount < 1024)
        score += 2;
    if (speedBias > -4.0f && speedBias < 16.0f)
        score += 1;
    if (animWeight > -4.0f && animWeight < 16.0f)
        score += 1;
    // Fragment 0 is valid and can therefore make unrelated nearby memory
    // look like an IAction. ArkNpcAnimAction's native NPC member is the
    // unambiguous discriminator for the real object base.
    if (result.npc &&
        CoopRuntimeGuards::IsLikelyRuntimeCppObject(result.npc, sizeof(void*) * 4))
    {
        score += 16;
    }
    else
    {
        score -= 12;
    }
    result.score = score;
    return result;
}

ActionBaseProbe FindBestActionBaseNear(const void* actionLikePointer)
{
    ActionBaseProbe best;
    if (!actionLikePointer)
        return best;

    const auto* bytes = reinterpret_cast<const uint8_t*>(actionLikePointer);
    for (int offset = -0x80; offset <= 0x20; offset += 8)
    {
        const void* candidate = bytes + offset;
        ActionBaseProbe probe = ProbeActionBaseCandidate(candidate);
        if (probe.score > best.score)
            best = probe;
    }
    return best;
}

std::string DumpActionRawQwords(const void* pointer)
{
    constexpr int kRawQwordCount = 20;
    if (!CoopRuntimeGuards::IsReadableRuntimePointer(pointer, sizeof(std::uintptr_t) * kRawQwordCount))
        return "raw_unreadable";

    std::ostringstream out;
    out << "raw";
    for (int i = 0; i < kRawQwordCount; ++i)
    {
        std::uintptr_t value = 0;
        if (!ReadFieldAt(pointer, i * sizeof(std::uintptr_t), value))
            break;
        out << i << "=" << PointerHex(reinterpret_cast<void*>(value));
        if (i != kRawQwordCount - 1)
            out << ",";
    }
    return out.str();
}

std::string DumpMannequinTagStateBytes(const void* tagState)
{
    if (!tagState || !CoopRuntimeGuards::IsReadableRuntimePointer(tagState, kMannequinTagStateByteCount))
        return "-";

    std::ostringstream hex;
    std::ostringstream bits;
    hex << std::hex << std::setfill('0');
    bool anyBit = false;
    for (size_t byteIndex = 0; byteIndex < kMannequinTagStateByteCount; ++byteIndex)
    {
        uint8_t value = 0;
        if (!ReadFieldAt(tagState, byteIndex, value))
            return "unreadable";

        hex << std::setw(2) << static_cast<unsigned>(value);
        for (int bitIndex = 0; bitIndex < 8; ++bitIndex)
        {
            if ((value & (1u << bitIndex)) == 0)
                continue;
            if (anyBit)
                bits << ",";
            bits << (byteIndex * 8 + static_cast<size_t>(bitIndex));
            anyBit = true;
        }
    }

    return "0x" + hex.str() + "_bits_" + (anyBit ? bits.str() : std::string("-"));
}

std::string DumpResolvedFragmentIntsNear(const void* pointer)
{
    if (!pointer)
        return "fragmentScan=-";

    const auto* bytes = reinterpret_cast<const uint8_t*>(pointer);
    std::ostringstream out;
    out << "fragmentScan";
    int found = 0;
    for (int offset = -0x100; offset <= 0x180 && found < 12; offset += 4)
    {
        int value = -1;
        if (!ReadFieldAt(bytes + offset, 0, value))
            continue;
        if (!IsKnownMannequinFragmentId(value))
            continue;

        out << found
            << "=off_" << offset
            << "_id_" << value
            << "_" << ResolveMannequinFragmentName(value);
        ++found;
        if (found < 12)
            out << ",";
    }

    if (found == 0)
        out << "=-";
    return out.str();
}

struct MannequinActionSnapshot
{
    bool present = false;
    bool ok = false;
    const void* input = nullptr;
    const void* base = nullptr;
    int baseScore = -1;
    int fragmentId = -1;
    uint32_t optionIdx = CoopProtocol::kInvalidMannequinOrdinal;
    bool optionOk = false;
    int status = -1;
    uint32_t flags = 0;
    int priority = -1;
    int subContext = 0;
    uint32_t forcedScopeMask = 0;
    uint32_t installedScopeMask = 0;
    float activeTime = 0.0f;
    float queueTime = 0.0f;
    float speedBias = 0.0f;
    float animWeight = 0.0f;
    std::uintptr_t rootScope = 0;
    std::uintptr_t context = 0;
    ArkNpc* npc = nullptr;
    std::array<uint8_t, kMannequinTagStateByteCount> fragTagState = {};
    std::string fragTags = "-";
};

MannequinActionSnapshot CaptureMannequinActionSnapshot(const void* actionLikePointer)
{
    MannequinActionSnapshot snapshot;
    snapshot.input = actionLikePointer;
    snapshot.present = actionLikePointer != nullptr;
    if (!actionLikePointer || !CoopRuntimeGuards::IsLikelyRuntimeCppObject(actionLikePointer, sizeof(void*) * 4))
        return snapshot;

    const ActionBaseProbe bestBase = FindBestActionBaseNear(actionLikePointer);
    snapshot.base = bestBase.score >= 8 ? bestBase.base : actionLikePointer;
    snapshot.baseScore = bestBase.score;
    if (!CoopRuntimeGuards::IsReadableRuntimePointer(snapshot.base, kIActionAnimWeightOffset + sizeof(float)))
        return snapshot;

    bool ok = true;
    ok = ReadFieldAt(snapshot.base, kIActionContextOffset, snapshot.context) && ok;
    ok = ReadFieldAt(snapshot.base, kIActionActiveTimeOffset, snapshot.activeTime) && ok;
    ok = ReadFieldAt(snapshot.base, kIActionQueueTimeOffset, snapshot.queueTime) && ok;
    ok = ReadFieldAt(snapshot.base, kIActionForcedScopeMaskOffset, snapshot.forcedScopeMask) && ok;
    ok = ReadFieldAt(snapshot.base, kIActionInstalledScopeMaskOffset, snapshot.installedScopeMask) && ok;
    ok = ReadFieldAt(snapshot.base, kIActionSubContextOffset, snapshot.subContext) && ok;
    ok = ReadFieldAt(snapshot.base, kIActionPriorityOffset, snapshot.priority) && ok;
    ok = ReadFieldAt(snapshot.base, kIActionStatusOffset, snapshot.status) && ok;
    ok = ReadFieldAt(snapshot.base, kIActionFlagsOffset, snapshot.flags) && ok;
    ok = ReadFieldAt(snapshot.base, kIActionRootScopeOffset, snapshot.rootScope) && ok;
    ok = ReadFieldAt(snapshot.base, kIActionFragmentIdOffset, snapshot.fragmentId) && ok;
    snapshot.optionOk = ReadFieldAt(snapshot.base, kIActionOptionIdxOffset, snapshot.optionIdx);
    ok = snapshot.optionOk && ok;
    ok = ReadFieldAt(snapshot.base, kIActionSpeedBiasOffset, snapshot.speedBias) && ok;
    ok = ReadFieldAt(snapshot.base, kIActionAnimWeightOffset, snapshot.animWeight) && ok;
    ReadFieldAt(snapshot.base, kArkNpcAnimActionNpcOffset, snapshot.npc);
    std::memcpy(
        snapshot.fragTagState.data(),
        reinterpret_cast<const uint8_t*>(snapshot.base) + kIActionFragTagsOffset,
        snapshot.fragTagState.size());
    snapshot.fragTags = DumpMannequinTagStateBytes(
        reinterpret_cast<const uint8_t*>(snapshot.base) + kIActionFragTagsOffset);
    snapshot.ok = ok && IsKnownMannequinFragmentId(snapshot.fragmentId);
    return snapshot;
}

std::string FormatMannequinActionSnapshot(const MannequinActionSnapshot& snapshot, const char* label)
{
    const char* prefix = label && label[0] ? label : "action";
    std::ostringstream out;
    out
        << prefix << "Present=" << (snapshot.present ? 1 : 0)
        << " " << prefix << "Ok=" << (snapshot.ok ? 1 : 0)
        << " " << prefix << "Input=" << PointerHex(snapshot.input)
        << " " << prefix << "Base=" << PointerHex(snapshot.base)
        << " " << prefix << "BaseScore=" << snapshot.baseScore
        << " " << prefix << "Fragment=" << snapshot.fragmentId
        << " " << prefix << "OptionOk=" << (snapshot.optionOk ? 1 : 0)
        << " " << prefix << "Option=" << snapshot.optionIdx
        << " " << prefix << "Status=" << snapshot.status
        << " " << prefix << "Flags=0x" << std::hex << snapshot.flags << std::dec
        << " " << prefix << "Priority=" << snapshot.priority
        << " " << prefix << "SubContext=" << snapshot.subContext
        << " " << prefix << "ForcedScope=0x" << std::hex << snapshot.forcedScopeMask
        << " " << prefix << "InstalledScope=0x" << snapshot.installedScopeMask << std::dec
        << " " << prefix << "Active=" << snapshot.activeTime
        << " " << prefix << "Queue=" << snapshot.queueTime
        << " " << prefix << "Speed=" << snapshot.speedBias
        << " " << prefix << "Weight=" << snapshot.animWeight
        << " " << prefix << "Context=" << PointerHex(reinterpret_cast<void*>(snapshot.context))
        << " " << prefix << "RootScope=" << PointerHex(reinterpret_cast<void*>(snapshot.rootScope))
        << " " << prefix << "Npc=" << PointerHex(snapshot.npc)
        << " " << prefix << "FragTags=" << snapshot.fragTags;
    return out.str();
}


Vec3 GetAnimationTestSpawnPos()
{
    ArkPlayer& player = ArkPlayer::GetInstance();
    IEntity* playerEntity = player.GetEntity();
    const Quat playerRotation = playerEntity->GetRotation();
    return playerEntity->GetWorldPos() + playerRotation * Vec3(0.0f, kAnimationTestProxyForwardOffsetMeters, 0.0f);
}

Quat GetAnimationTestSpawnRot()
{
    return ArkPlayer::GetInstance().GetEntity()->GetRotation();
}

int ParseIntArg(const std::vector<std::string>& args, size_t index, int fallback)
{
    return index < args.size() ? std::atoi(args[index].c_str()) : fallback;
}

float ParseFloatArg(const std::vector<std::string>& args, size_t index, float fallback)
{
    if (index >= args.size())
        return fallback;

    char* end = nullptr;
    const float value = std::strtof(args[index].c_str(), &end);
    return end && end != args[index].c_str() ? value : fallback;
}

bool AnimProxyEnvFlagEnabled(const char* name)
{
    return CoopRuntimeConfig::Flag(name);
}

bool ParseUint64Text(const std::string& text, uint64_t& out)
{
    if (text.empty())
        return false;

    char* end = nullptr;
    const int base = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? 16 : 10;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, base);
    if (!end || end == text.c_str() || *end != '\0')
        return false;

    out = static_cast<uint64_t>(parsed);
    return true;
}

bool IsChrModelPath(const std::string& path)
{
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    constexpr const char* suffix = ".chr";
    return lower.size() >= 4 && lower.compare(lower.size() - 4, 4, suffix) == 0;
}

std::string JoinRuntimeArgs(const std::vector<std::string>& args, size_t first)
{
    std::string out;
    for (size_t i = first; i < args.size(); ++i)
    {
        if (!out.empty())
            out += " ";
        out += args[i];
    }
    return out;
}

bool ReadAnimProxyQueueDepth(IScriptTable* table, int slot, int layer, int& outQueueDepth)
{
    outQueueDepth = -1;
    if (!table)
        return false;

    IScriptSystem* scriptSystem = table->GetScriptSystem();
    if (!scriptSystem || !scriptSystem->BeginCall(table, "GetNumQueuedAnimations"))
        return false;

    scriptSystem->PushFuncParam(table);
    scriptSystem->PushFuncParam(slot);
    scriptSystem->PushFuncParam(layer);
    return scriptSystem->EndCall(outQueueDepth);
}

std::string ScriptAnyTypeName(ScriptAnyType type)
{
    switch (type)
    {
    case ANY_ANY: return "any";
    case ANY_TNIL: return "nil";
    case ANY_TBOOLEAN: return "bool";
    case ANY_THANDLE: return "handle";
    case ANY_TNUMBER: return "number";
    case ANY_TSTRING: return "string";
    case ANY_TTABLE: return "table";
    case ANY_TFUNCTION: return "function";
    case ANY_TUSERDATA: return "userdata";
    case ANY_TVECTOR: return "vector";
    default: return "unknown";
    }
}

std::string ScriptAnyValueText(const ScriptAnyValue& value)
{
    switch (value.type)
    {
    case ANY_TNIL:
        return "nil";
    case ANY_TBOOLEAN:
        return value.b ? "true" : "false";
    case ANY_TNUMBER:
        return std::to_string(value.number);
    case ANY_TSTRING:
        return value.str ? value.str : "";
    case ANY_THANDLE:
    case ANY_TUSERDATA:
        return std::to_string(reinterpret_cast<std::uintptr_t>(value.ptr));
    case ANY_TVECTOR:
        return "(" + std::to_string(value.vec3.x) + "," + std::to_string(value.vec3.y) + "," + std::to_string(value.vec3.z) + ")";
    default:
        return ScriptAnyTypeName(value.type);
    }
}

bool ScriptAnyTruthyOrVoid(const ScriptAnyValue& value)
{
    if (value.type == ANY_TBOOLEAN)
        return value.b;
    if (value.type == ANY_TNUMBER)
        return std::fabs(value.number) > 0.0001f;
    if (value.type == ANY_TNIL || value.type == ANY_ANY)
        return true;
    return true;
}

bool BeginAnimProxyScriptCall(IScriptTable* table, const char* method, IScriptSystem*& outScriptSystem)
{
    outScriptSystem = nullptr;
    if (!table)
        return false;

    IScriptSystem* scriptSystem = table->GetScriptSystem();
    if (!scriptSystem || !scriptSystem->BeginCall(table, method))
        return false;

    scriptSystem->PushFuncParam(table);
    outScriptSystem = scriptSystem;
    return true;
}

bool CallAnimProxyScriptAny(IScriptTable* table, const char* method, ScriptAnyValue& outValue)
{
    IScriptSystem* scriptSystem = nullptr;
    if (!BeginAnimProxyScriptCall(table, method, scriptSystem))
        return false;
    return scriptSystem->EndCallAny(outValue);
}

bool CallAnimProxyScriptAny(IScriptTable* table, const char* method, int p1, ScriptAnyValue& outValue)
{
    IScriptSystem* scriptSystem = nullptr;
    if (!BeginAnimProxyScriptCall(table, method, scriptSystem))
        return false;
    scriptSystem->PushFuncParam(p1);
    return scriptSystem->EndCallAny(outValue);
}

bool CallAnimProxyScriptAny(IScriptTable* table, const char* method, int p1, int p2, ScriptAnyValue& outValue)
{
    IScriptSystem* scriptSystem = nullptr;
    if (!BeginAnimProxyScriptCall(table, method, scriptSystem))
        return false;
    scriptSystem->PushFuncParam(p1);
    scriptSystem->PushFuncParam(p2);
    return scriptSystem->EndCallAny(outValue);
}

bool CallAnimProxyScriptAny(IScriptTable* table, const char* method, int p1, const char* p2, ScriptAnyValue& outValue)
{
    IScriptSystem* scriptSystem = nullptr;
    if (!BeginAnimProxyScriptCall(table, method, scriptSystem))
        return false;
    scriptSystem->PushFuncParam(p1);
    scriptSystem->PushFuncParam(p2);
    return scriptSystem->EndCallAny(outValue);
}

bool CallAnimProxyStartAnimation(
    IScriptTable* table,
    int slot,
    const std::string& animationName,
    int layer,
    float blend,
    float speed,
    ScriptAnyValue& outValue)
{
    IScriptSystem* scriptSystem = nullptr;
    if (!BeginAnimProxyScriptCall(table, "StartAnimation", scriptSystem))
        return false;
    scriptSystem->PushFuncParam(slot);
    scriptSystem->PushFuncParam(animationName.c_str());
    scriptSystem->PushFuncParam(layer);
    scriptSystem->PushFuncParam(blend);
    scriptSystem->PushFuncParam(speed);
    return scriptSystem->EndCallAny(outValue);
}

bool ReadAnimProxyCurrentAnimation(IScriptTable* table, int slot, std::string& outCurrent, std::string& outType)
{
    outCurrent = "-";
    outType = "-";
    ScriptAnyValue value;
    if (!CallAnimProxyScriptAny(table, "GetCurAnimation", slot, value))
        return false;
    outType = ScriptAnyTypeName(value.type);
    outCurrent = ScriptAnyValueText(value);
    return true;
}

bool ReadAnimProxyAnimationTime(IScriptTable* table, int slot, int layer, float& outTime, std::string& outType)
{
    outTime = -1.0f;
    outType = "-";
    ScriptAnyValue value;
    if (!CallAnimProxyScriptAny(table, "GetAnimationTime", slot, layer, value))
        return false;
    outType = ScriptAnyTypeName(value.type);
    return value.CopyTo(outTime);
}

bool ReadAnimProxyAnimationLength(IScriptTable* table, int slot, const std::string& animationName, float& outLength, std::string& outType)
{
    outLength = -1.0f;
    outType = "-";
    ScriptAnyValue value;
    if (!CallAnimProxyScriptAny(table, "GetAnimationLength", slot, animationName.c_str(), value))
        return false;
    outType = ScriptAnyTypeName(value.type);
    return value.CopyTo(outLength);
}
}

IEntity* ModMain::GetAnimationTestProxyEntity() const
{
    if (!gEnv || !gEnv->pEntitySystem || m_animationTestProxyEntityId == INVALID_ENTITYID)
        return nullptr;
    return gEnv->pEntitySystem->GetEntity(m_animationTestProxyEntityId);
}

void ModMain::SpawnAnimationTestProxy()
{
    if (!IsAnimationTestGameReady())
    {
        ++m_animationTestProxyFailures;
        m_lastAnimationTestEvent = "spawn_failed_game_not_ready";
        CoopRuntimeLog::Write("Animation test proxy spawn ignored: game is not ready");
        return;
    }

    RemoveAnimationTestProxy();

    Vec3 position = GetAnimationTestSpawnPos();
    Quat rotation = GetAnimationTestSpawnRot();
    uint64_t archetypeId = kAnimationTestDefaultProxyArchetype;
    if (!ParseUint64Text(m_animationTestArchetypeText, archetypeId))
    {
        ++m_animationTestProxyFailures;
        m_lastAnimationTestEvent = "spawn_failed_bad_archetype archetype=" + m_animationTestArchetypeText;
        CoopRuntimeLog::Write("Animation test proxy spawn failed: bad archetype " + m_animationTestArchetypeText);
        return;
    }

    const std::vector<EntityId> beforeSpawn = CaptureRuntimeEntityIdSnapshot("spawn animation test proxy before");
    IEntity* entity = EntityUtils::SpawnNpc("CoopAnimTestProxy", position, rotation, archetypeId);
    RecordCoopSpawnDiagnostics("CoopAnimTestProxy", beforeSpawn, entity);
    if (!entity)
    {
        ++m_animationTestProxyFailures;
        m_lastAnimationTestEvent = "spawn_failed_no_entity";
        CoopRuntimeLog::Write("Animation test proxy spawn failed");
        return;
    }

    m_animationTestProxyEntityId = entity->GetId();
    MarkCoopRuntimeEntity(*entity, false);
    ApplyProxyName(*entity, "AnimTestProxy");
    ApplyProxyNoPropCollision(*entity, "animation test proxy spawn");

    if (ArkNpc* npc = EntityUtils::GetArkNpc(entity))
    {
        TryGuardedVoidCall("anim proxy PushDisableAiTree", [npc]() { npc->PushDisableAiTree(); });
        TryGuardedVoidCall("anim proxy PushDisableAttentionObjectAndPerceivables", [npc]() { npc->PushDisableAttentionObjectAndPerceivables(); });
        TryGuardedVoidCall("anim proxy PushDisableNpcHealthUI", [npc]() { npc->PushDisableNpcHealthUI(); });
        TryGuardedVoidCall("anim proxy PushDisableHitReactions", [npc]() { ArkNpc::FPushDisableHitReactions(npc); });
        TryGuardedVoidCall("anim proxy PushDisableDeathReactions", [npc]() { npc->PushDisableDeathReactions(); });
        RecoverLiveNetworkNpc(*npc);
        m_animationTestNativeHitReactionsEnabled = false;
    }

    ++m_animationTestProxySpawns;
    m_lastAnimationTestEvent =
        "spawned entity=" + std::to_string(m_animationTestProxyEntityId) +
        " archetype=" + std::to_string(archetypeId);
    CoopRuntimeLog::Write(
        "Animation test proxy spawned entity=" + std::to_string(m_animationTestProxyEntityId) +
        " archetype=" + std::to_string(archetypeId));
}

void ModMain::RemoveAnimationTestProxy()
{
    if (m_animationTestProxyEntityId == INVALID_ENTITYID)
        return;

    ClearAnimationTestProxyPoseHold("remove_animation_test_proxy");

    const EntityId entityId = m_animationTestProxyEntityId;
    m_animationTestProxyEntityId = INVALID_ENTITYID;
    m_animationTestNativeHitReactionsEnabled = false;
    if (gEnv && gEnv->pEntitySystem)
    {
        PrepareCoopEntityForRemoval(entityId, false, false, "remove animation test proxy");
        RemoveCoopEntityGuarded(entityId, true, "remove animation test proxy");
    }

    m_lastAnimationTestEvent = "removed entity=" + std::to_string(entityId);
    CoopRuntimeLog::Write("Animation test proxy removed entity=" + std::to_string(entityId));
}

bool ModMain::LoadAnimationTestProxyModel(const std::string& modelPath, int slot, std::string& detail)
{
    IEntity* entity = GetAnimationTestProxyEntity();
    if (!entity)
    {
        detail = "model_failed_no_anim_proxy";
        m_lastAnimationTestEvent = detail;
        ++m_animationTestProxyFailures;
        return false;
    }
    if (modelPath.empty())
    {
        detail = "model_failed_empty_path";
        m_lastAnimationTestEvent = detail;
        ++m_animationTestProxyFailures;
        return false;
    }
    if (!CoopRuntimeConfig::UnsafeFlag("COOP_ALLOW_ANIM_PROXY_MODEL_HOTLOAD"))
    {
        detail = "model_rejected_hotload_spawn_archetype_instead path=" + modelPath;
        m_lastAnimationTestEvent = detail;
        ++m_animationTestProxyFailures;
        CoopRuntimeLog::Write(
            "Animation test proxy rejected model hotload " + modelPath +
            "; spawn the matching ArkHuman archetype instead");
        return false;
    }
    if (IsChrModelPath(modelPath) &&
        !CoopRuntimeConfig::UnsafeFlag("COOP_ALLOW_ANIM_PROXY_CHR_MODEL_LOAD"))
    {
        detail = "model_rejected_chr_use_cdf_or_set_COOP_ALLOW_ANIM_PROXY_CHR_MODEL_LOAD path=" + modelPath;
        m_lastAnimationTestEvent = detail;
        ++m_animationTestProxyFailures;
        CoopRuntimeLog::Write("Animation test proxy rejected raw chr model path " + modelPath);
        return false;
    }

    int loadedSlot = -1;
    std::string guardReason;
    const bool ok = TryGuardedCall(
        "anim proxy LoadCharacter",
        [entity, slot, &modelPath]() { return entity->LoadCharacter(slot, modelPath.c_str()); },
        loadedSlot,
        &guardReason) &&
        loadedSlot >= 0;

    detail =
        std::string(ok ? "model_ok" : "model_failed") +
        " requestedSlot=" + std::to_string(slot) +
        " loadedSlot=" + std::to_string(loadedSlot) +
        " path=" + modelPath +
        (guardReason.empty() ? std::string() : " reason=" + guardReason);
    m_lastAnimationTestEvent = detail;
    if (!ok)
        ++m_animationTestProxyFailures;
    return ok;
}

bool ModMain::PlayAnimationTestProxyAnimation(
    const std::string& animationName,
    int slot,
    int layer,
    float blend,
    float speed,
    std::string& detail)
{
    IEntity* entity = GetAnimationTestProxyEntity();
    if (!entity)
    {
        detail = "play_failed_no_anim_proxy";
        m_lastAnimationTestEvent = detail;
        ++m_animationTestProxyFailures;
        return false;
    }
    if (animationName.empty())
    {
        detail = "play_failed_empty_anim";
        m_lastAnimationTestEvent = detail;
        ++m_animationTestProxyFailures;
        return false;
    }

    ClearAnimationTestProxyPoseHold("manual_play");

    IScriptTable* table = entity->GetScriptTable();
    if (!table)
    {
        detail = "play_failed_no_script_table entity=" + std::to_string(entity->GetId());
        m_lastAnimationTestEvent = detail;
        ++m_animationTestProxyFailures;
        return false;
    }

    std::string beforeCurrent;
    std::string beforeCurrentType;
    const bool beforeCurrentOk = ReadAnimProxyCurrentAnimation(table, slot, beforeCurrent, beforeCurrentType);
    float beforeTime = -1.0f;
    std::string beforeTimeType;
    const bool beforeTimeOk = ReadAnimProxyAnimationTime(table, slot, layer, beforeTime, beforeTimeType);
    float animationLength = -1.0f;
    std::string animationLengthType;
    const bool animationLengthOk = ReadAnimProxyAnimationLength(table, slot, animationName, animationLength, animationLengthType);

    bool playOk = false;
    std::string guardReason;
    ScriptAnyValue playReturn;
    const bool guarded = TryGuardedCall(
        "anim proxy StartAnimation",
        [table, slot, &animationName, layer, blend, speed, &playReturn]()
        {
            return CallAnimProxyStartAnimation(table, slot, animationName, layer, blend, speed, playReturn);
        },
        playOk,
        &guardReason);
    playOk = guarded && playOk && ScriptAnyTruthyOrVoid(playReturn);

    std::string afterCurrent;
    std::string afterCurrentType;
    const bool afterCurrentOk = ReadAnimProxyCurrentAnimation(table, slot, afterCurrent, afterCurrentType);
    float afterTime = -1.0f;
    std::string afterTimeType;
    const bool afterTimeOk = ReadAnimProxyAnimationTime(table, slot, layer, afterTime, afterTimeType);
    int queueDepth = -1;
    ReadAnimProxyQueueDepth(table, slot, layer, queueDepth);

    const bool ok = guarded && playOk;
    detail =
        std::string(ok ? "play_ok" : "play_failed") +
        " entity=" + std::to_string(entity->GetId()) +
        " anim=" + animationName +
        " slot=" + std::to_string(slot) +
        " layer=" + std::to_string(layer) +
        " blend=" + std::to_string(blend) +
        " speed=" + std::to_string(speed) +
        " queue=" + std::to_string(queueDepth) +
        " length=" + (animationLengthOk ? std::to_string(animationLength) : std::string("fail")) +
        "/" + animationLengthType +
        " ret=" + ScriptAnyTypeName(playReturn.type) + ":" + ScriptAnyValueText(playReturn) +
        " curBefore=" + (beforeCurrentOk ? beforeCurrent : std::string("fail")) +
        "/" + beforeCurrentType +
        " curAfter=" + (afterCurrentOk ? afterCurrent : std::string("fail")) +
        "/" + afterCurrentType +
        " timeBefore=" + (beforeTimeOk ? std::to_string(beforeTime) : std::string("fail")) +
        "/" + beforeTimeType +
        " timeAfter=" + (afterTimeOk ? std::to_string(afterTime) : std::string("fail")) +
        "/" + afterTimeType +
        (guardReason.empty() ? std::string() : " reason=" + guardReason);
    m_lastAnimationTestEvent = detail;
    if (ok)
        ++m_animationTestProxyPlays;
    else
        ++m_animationTestProxyFailures;
    return ok;
}

bool ModMain::StopAnimationTestProxyAnimation(int slot, int layer, std::string& detail)
{
    ClearAnimationTestProxyPoseHold("manual_stop");

    IEntity* entity = GetAnimationTestProxyEntity();
    IScriptTable* table = entity ? entity->GetScriptTable() : nullptr;
    if (!entity || !table)
    {
        detail = "stop_failed_no_anim_proxy";
        m_lastAnimationTestEvent = detail;
        ++m_animationTestProxyFailures;
        return false;
    }

    bool stopOk = false;
    std::string guardReason;
    const bool guarded = TryGuardedCall(
        "anim proxy StopAnimation",
        [table, slot, layer]() { return Script::CallMethod(table, "StopAnimation", slot, layer); },
        stopOk,
        &guardReason);
    detail =
        std::string(guarded && stopOk ? "stop_ok" : "stop_failed") +
        " entity=" + std::to_string(entity->GetId()) +
        " slot=" + std::to_string(slot) +
        " layer=" + std::to_string(layer) +
        (guardReason.empty() ? std::string() : " reason=" + guardReason);
    m_lastAnimationTestEvent = detail;
    if (!(guarded && stopOk))
        ++m_animationTestProxyFailures;
    return guarded && stopOk;
}

bool ModMain::ResetAnimationTestProxyAnimation(int slot, int layer, std::string& detail)
{
    ClearAnimationTestProxyPoseHold("manual_reset");

    IEntity* entity = GetAnimationTestProxyEntity();
    IScriptTable* table = entity ? entity->GetScriptTable() : nullptr;
    if (!entity || !table)
    {
        detail = "reset_failed_no_anim_proxy";
        m_lastAnimationTestEvent = detail;
        ++m_animationTestProxyFailures;
        return false;
    }

    bool resetOk = false;
    std::string guardReason;
    const bool guarded = TryGuardedCall(
        "anim proxy ResetAnimation",
        [table, slot, layer]() { return Script::CallMethod(table, "ResetAnimation", slot, layer); },
        resetOk,
        &guardReason);
    detail =
        std::string(guarded && resetOk ? "reset_ok" : "reset_failed") +
        " entity=" + std::to_string(entity->GetId()) +
        " slot=" + std::to_string(slot) +
        " layer=" + std::to_string(layer) +
        (guardReason.empty() ? std::string() : " reason=" + guardReason);
    m_lastAnimationTestEvent = detail;
    if (!(guarded && resetOk))
        ++m_animationTestProxyFailures;
    return guarded && resetOk;
}

bool ModMain::SetAnimationTestProxyTime(int slot, int layer, float normalizedTime, std::string& detail)
{
    IEntity* entity = GetAnimationTestProxyEntity();
    IScriptTable* table = entity ? entity->GetScriptTable() : nullptr;
    if (!entity || !table)
    {
        detail = "time_failed_no_anim_proxy";
        m_lastAnimationTestEvent = detail;
        ++m_animationTestProxyFailures;
        return false;
    }

    normalizedTime = std::clamp(normalizedTime, 0.0f, 1.0f);
    if (m_animationTestPoseHoldActive &&
        slot == m_animationTestPoseHoldSlot &&
        layer == m_animationTestPoseHoldLayer)
    {
        m_animationTestPoseHoldTime = normalizedTime;
        m_animationTestPoseHoldAccumulator = m_animationTestPoseHoldInterval;
    }

    bool timeOk = false;
    std::string guardReason;
    const bool guarded = TryGuardedCall(
        "anim proxy SetAnimationTime",
        [table, slot, layer, normalizedTime]()
        {
            return Script::CallMethod(table, "SetAnimationTime", slot, layer, normalizedTime);
        },
        timeOk,
        &guardReason);
    detail =
        std::string(guarded && timeOk ? "time_ok" : "time_failed") +
        " entity=" + std::to_string(entity->GetId()) +
        " slot=" + std::to_string(slot) +
        " layer=" + std::to_string(layer) +
        " t=" + std::to_string(normalizedTime) +
        (guardReason.empty() ? std::string() : " reason=" + guardReason);
    m_lastAnimationTestEvent = detail;
    if (!(guarded && timeOk))
        ++m_animationTestProxyFailures;
    return guarded && timeOk;
}

bool ModMain::ProbeAnimationTestProxy(std::string& detail) const
{
    IEntity* entity = GetAnimationTestProxyEntity();
    if (!entity)
    {
        detail = "probe_no_anim_proxy";
        return false;
    }

    const Vec3 pos = entity->GetWorldPos();
    const bool hasCharacter = entity->GetCharacter(m_animationTestSlot) != nullptr;
    int queueDepth = -1;
    std::string currentAnimation = "-";
    std::string currentAnimationType = "-";
    float animationTime = -1.0f;
    std::string animationTimeType = "-";
    if (IScriptTable* table = entity->GetScriptTable())
    {
        ReadAnimProxyQueueDepth(table, m_animationTestSlot, m_animationTestLayer, queueDepth);
        ReadAnimProxyCurrentAnimation(table, m_animationTestSlot, currentAnimation, currentAnimationType);
        ReadAnimProxyAnimationTime(table, m_animationTestSlot, m_animationTestLayer, animationTime, animationTimeType);
    }

    std::ostringstream out;
    out
        << "probe_ok"
        << " entity=" << entity->GetId()
        << " name=" << (entity->GetName() ? entity->GetName() : "-")
        << " pos=(" << pos.x << "," << pos.y << "," << pos.z << ")"
        << " slot=" << m_animationTestSlot
        << " layer=" << m_animationTestLayer
        << " character=" << (hasCharacter ? 1 : 0)
        << " queue=" << queueDepth
        << " current=" << currentAnimation << "/" << currentAnimationType
        << " time=" << animationTime << "/" << animationTimeType
        << " archetype=" << m_animationTestArchetypeText
        << " model=" << m_animationTestModelPath
        << " last=" << m_lastAnimationTestEvent;
    detail = out.str();
    return true;
}

bool ModMain::ProbeAnimationTestProxyClipNames(int slot, std::string& detail)
{
    IEntity* entity = GetAnimationTestProxyEntity();
    IScriptTable* table = entity ? entity->GetScriptTable() : nullptr;
    if (!entity || !table)
    {
        detail = "clip_probe_failed_no_anim_proxy";
        m_lastAnimationTestEvent = detail;
        ++m_animationTestProxyFailures;
        return false;
    }

    int valid = 0;
    std::string firstValid;
    std::ostringstream out;
    out << "clip_probe_ok entity=" << entity->GetId() << " slot=" << slot << " valid=";

    std::ostringstream hits;
    for (const char* candidate : kAnimationTestClipProbeCandidates)
    {
        float length = -1.0f;
        std::string valueType;
        if (!ReadAnimProxyAnimationLength(table, slot, candidate, length, valueType))
            continue;

        if (length > 0.001f)
        {
            if (firstValid.empty())
                firstValid = candidate;
            if (valid < 8)
            {
                if (valid != 0)
                    hits << ",";
                hits << candidate << ":" << length;
            }
            ++valid;
        }
    }

    out << valid << " hits=" << (hits.str().empty() ? std::string("-") : hits.str());
    if (!firstValid.empty())
    {
        m_animationTestName = firstValid;
        out << " selected=" << firstValid;
    }
    else
    {
        out << " selected=- reason=no_raw_clip_length_from_probe_candidates";
    }

    detail = out.str();
    m_lastAnimationTestEvent = detail;
    if (valid == 0)
        ++m_animationTestProxyFailures;
    return valid > 0;
}

bool ModMain::ApplyProxyCharacterAnimationState(IEntity& entity, const std::string& state, float duration, std::string& detail)
{
    ArkNpc* npc = EntityUtils::GetArkNpc(&entity);
    if (!npc)
    {
        detail = "proxy_state_failed_no_arknpc entity=" + std::to_string(entity.GetId()) + " state=" + state;
        m_lastAnimationTestEvent = detail;
        ++m_animationTestProxyFailures;
        return false;
    }

    const std::string normalized = ToLowerAsciiAnimation(state.empty() ? std::string("status") : state);
    const float safeDuration = duration > 0.0f ? duration : 2.0f;
    std::string guardReason;
    std::ostringstream actions;
    bool actionOk = true;
    bool usedNativeState = false;

    auto appendAction = [&actions](const char* name, bool ok)
    {
        if (actions.tellp() > 0)
            actions << ",";
        actions << name << ":" << (ok ? 1 : 0);
    };

    auto stopBlockingStates = [&]()
    {
        bool ok = TryGuardedVoidCall("proxy state ArkNpc::EndAnimatedStunned", [npc]() { npc->EndAnimatedStunned(); }, &guardReason);
        appendAction("end_stunned", ok);
        actionOk = ok && actionOk;

        ok = TryGuardedVoidCall("proxy state ArkNpc::StopStun", [npc]() { npc->StopStun(); }, &guardReason);
        appendAction("stop_stun", ok);
        actionOk = ok && actionOk;

        bool result = false;
        ok = TryGuardedCall("proxy state ArkNpc::StopCowering", [npc]() { return npc->StopCowering(); }, result, &guardReason);
        appendAction("stop_cowering", ok && result);
        actionOk = ok && actionOk;
    };

    if (normalized == "status")
    {
        usedNativeState = true;
    }
    else if (normalized == "normal" || normalized == "idle" || normalized == "stand" || normalized == "upright" || normalized == "revived")
    {
        if (entity.GetId() == m_animationTestProxyEntityId)
            ClearAnimationTestProxyPoseHold("proxy_state_normal");
        stopBlockingStates();
        usedNativeState = true;
    }
    else if (normalized == "revive")
    {
        if (entity.GetId() == m_animationTestProxyEntityId)
            ClearAnimationTestProxyPoseHold("proxy_state_revive");
        stopBlockingStates();
        usedNativeState = true;
    }
    else if (normalized == "crouch_pose" || normalized == "crouchpose")
    {
        if (entity.GetId() != m_animationTestProxyEntityId)
        {
            detail = "proxy_state_failed_pose_hold_only_test_proxy entity=" + std::to_string(entity.GetId()) + " state=" + normalized;
            m_lastAnimationTestEvent = detail;
            ++m_animationTestProxyFailures;
            return false;
        }
        return StartAnimationTestProxyPoseHold(
            "crouch_pose",
            "fear_cower_c_empty",
            0.30f,
            m_animationTestSlot,
            m_animationTestLayer,
            0.01f,
            detail);
    }
    else if (normalized == "downed_pose" || normalized == "downedpose")
    {
        if (entity.GetId() != m_animationTestProxyEntityId)
        {
            detail = "proxy_state_failed_pose_hold_only_test_proxy entity=" + std::to_string(entity.GetId()) + " state=" + normalized;
            m_lastAnimationTestEvent = detail;
            ++m_animationTestProxyFailures;
            return false;
        }
        return StartAnimationTestProxyPoseHold(
            "downed_pose",
            "combat_forceresist_front_out_empty",
            0.25f,
            m_animationTestSlot,
            m_animationTestLayer,
            0.05f,
            detail);
    }
    else if (normalized == "standup" || normalized == "raise" || normalized == "raise_start")
    {
        actionOk = TryGuardedVoidCall(
            "proxy state ArkNpc::StartRaiseFromCorpse",
            [npc]() { npc->StartRaiseFromCorpse(true); },
            &guardReason);
        appendAction("raise_start", actionOk);
        usedNativeState = true;
    }
    else if (normalized == "raise_finish")
    {
        actionOk = TryGuardedVoidCall(
            "proxy state ArkNpc::FinishRaiseFromCorpse",
            [npc]() { npc->FinishRaiseFromCorpse(); },
            &guardReason);
        appendAction("raise_finish", actionOk);
        usedNativeState = true;
    }
    else if (normalized == "downed" || normalized == "knockdown" || normalized == "incapacitated")
    {
        bool finalOk = false;
        bool result = false;
        bool ok = TryGuardedCall(
            "proxy state ArkNpc::BeginAnimatedStunned",
            [npc]() { return npc->BeginAnimatedStunned(); },
            result,
            &guardReason);
        appendAction("begin_stunned", ok && result);
        finalOk = ok && result;
        usedNativeState = true;

        if (!finalOk)
        {
            result = false;
            ok = TryGuardedCall("proxy state ArkNpc::StartCowering", [npc]() { return npc->StartCowering(); }, result, &guardReason);
            appendAction("start_cowering_fallback", ok && result);
            finalOk = ok && result;
        }

        if (!finalOk)
        {
            const EntityId instigatorId = ArkPlayer::GetInstancePtr() && ArkPlayer::GetInstance().GetEntity()
                ? ArkPlayer::GetInstance().GetEntity()->GetId()
                : 0;
            ok = TryGuardedVoidCall(
                "proxy state ArkNpc::StartStun",
                [npc, instigatorId, safeDuration]() { npc->StartStun(instigatorId, std::max(2.0f, safeDuration)); },
                &guardReason);
            appendAction("start_stun_fallback", ok);
            finalOk = ok;
        }
        actionOk = finalOk;
    }
    else if (normalized == "crouch" || normalized == "cower" || normalized == "crawl")
    {
        bool result = false;
        actionOk = TryGuardedCall("proxy state ArkNpc::StartCowering", [npc]() { return npc->StartCowering(); }, result, &guardReason) && result;
        appendAction("start_cowering", actionOk);
        usedNativeState = true;
    }
    else if (normalized == "stunned" || normalized == "stun")
    {
        const EntityId instigatorId = ArkPlayer::GetInstancePtr() && ArkPlayer::GetInstance().GetEntity()
            ? ArkPlayer::GetInstance().GetEntity()->GetId()
            : 0;
        actionOk = TryGuardedVoidCall(
            "proxy state ArkNpc::StartStun",
            [npc, instigatorId, safeDuration]() { npc->StartStun(instigatorId, safeDuration); },
            &guardReason);
        appendAction("start_stun", actionOk);
        usedNativeState = true;
    }
    else if (normalized == "resist" || normalized == "hit" || normalized == "flinch")
    {
        const Vec3 forcePosition = ArkPlayer::GetInstancePtr() && ArkPlayer::GetInstance().GetEntity()
            ? ArkPlayer::GetInstance().GetEntity()->GetWorldPos()
            : entity.GetWorldPos();
        bool result = false;
        actionOk = TryGuardedCall(
            "proxy state ArkNpc::Resist",
            [npc, forcePosition, safeDuration]() { return npc->Resist(forcePosition, safeDuration); },
            result,
            &guardReason) && result;
        appendAction("resist", actionOk);
        usedNativeState = true;
    }
    else if (normalized == "walk" || normalized == "run" || normalized == "sprint" || normalized == "move")
    {
        detail =
            "proxy_state_deferred_locomotion entity=" + std::to_string(entity.GetId()) +
            " state=" + normalized +
            " reason=movement_states_need_locomotion_pose_sync_not_one_shot_mannequin_action";
        m_lastAnimationTestEvent = detail;
        return false;
    }

    if (!usedNativeState)
    {
        detail = "proxy_state_failed_unknown_state entity=" + std::to_string(entity.GetId()) + " state=" + state;
        m_lastAnimationTestEvent = detail;
        ++m_animationTestProxyFailures;
        return false;
    }

    bool stunned = false;
    bool resisting = false;
    bool scrunched = false;
    bool raising = false;
    bool cowering = false;
    bool waitAnimatedStunned = false;
    std::string statusReason;
    TryGuardedCall("proxy state ArkNpc::IsStunned", [npc]() { return npc->IsStunned(); }, stunned, &statusReason);
    TryGuardedCall("proxy state ArkNpc::IsResisting", [npc]() { return npc->IsResisting(); }, resisting, &statusReason);
    TryGuardedCall("proxy state ArkNpc::IsScrunched", [npc]() { return npc->IsScrunched(); }, scrunched, &statusReason);
    TryGuardedCall("proxy state ArkNpc::IsInRaiseFromCorpseAnim", [npc]() { return npc->IsInRaiseFromCorpseAnim(); }, raising, &statusReason);
    TryGuardedCall("proxy state ArkNpc::IsCowering", [npc]() { return npc->IsCowering(); }, cowering, &statusReason);
    TryGuardedCall("proxy state ArkNpc::WaitForAnimatedStunned", [npc]() { return npc->WaitForAnimatedStunned(); }, waitAnimatedStunned, &statusReason);

    detail =
        std::string(actionOk ? "proxy_state_ok" : "proxy_state_partial") +
        " entity=" + std::to_string(entity.GetId()) +
        " state=" + normalized +
        " actions=" + (actions.str().empty() ? std::string("-") : actions.str()) +
        " stunned=" + std::to_string(stunned ? 1 : 0) +
        " resisting=" + std::to_string(resisting ? 1 : 0) +
        " scrunched=" + std::to_string(scrunched ? 1 : 0) +
        " raising=" + std::to_string(raising ? 1 : 0) +
        " cowering=" + std::to_string(cowering ? 1 : 0) +
        " waitAnimatedStunned=" + std::to_string(waitAnimatedStunned ? 1 : 0) +
        (guardReason.empty() ? std::string() : " reason=" + guardReason) +
        (statusReason.empty() ? std::string() : " statusReason=" + statusReason) +
        " constructTrace=" + m_lastNpcMannequinConstructTraceEvent;
    m_lastAnimationTestEvent = detail;
    if (normalized != "status")
        StartAnimationTestProxyTrace("state_" + normalized, std::min(std::max(safeDuration + 1.0f, 2.0f), 6.0f));
    if (!actionOk)
        ++m_animationTestProxyFailures;
    return actionOk;
}

bool ModMain::ApplyAnimationTestProxyCharacterState(const std::string& state, float duration, std::string& detail)
{
    IEntity* entity = GetAnimationTestProxyEntity();
    if (!entity)
    {
        detail = "proxy_state_failed_no_anim_proxy state=" + state;
        m_lastAnimationTestEvent = detail;
        ++m_animationTestProxyFailures;
        return false;
    }

    return ApplyProxyCharacterAnimationState(*entity, state, duration, detail);
}

bool ModMain::RunAnimationTestProxyNpcNativeAction(const std::string& action, float duration, std::string& detail)
{
    constexpr std::string_view kNetworkPrefix = "network_";
    const bool targetNetworkProxy = action.rfind(kNetworkPrefix, 0) == 0;
    const std::string nativeAction = targetNetworkProxy
        ? action.substr(kNetworkPrefix.size())
        : action;
    IEntity* entity = targetNetworkProxy ? GetProxyEntity() : GetAnimationTestProxyEntity();
    if (!entity)
    {
        detail = targetNetworkProxy
            ? "npc_native_failed_no_network_proxy"
            : "npc_native_failed_no_anim_proxy";
        m_lastAnimationTestEvent = detail;
        ++m_animationTestProxyFailures;
        return false;
    }

    ArkNpc* npc = EntityUtils::GetArkNpc(entity);
    if (!npc)
    {
        detail = "npc_native_failed_no_arknpc entity=" + std::to_string(entity->GetId());
        m_lastAnimationTestEvent = detail;
        ++m_animationTestProxyFailures;
        return false;
    }

    bool actionOk = true;
    std::string guardReason;
    if (nativeAction == "status")
    {
        actionOk = true;
    }
    else if (nativeAction == "movement_probe")
    {
        // Call the common predicate directly. Calling the generated SDK
        // wrapper here would bypass the installed hook (and its address is not
        // a stable debug ABI); the production Try/Resume/Return/Exact hooks
        // below all enter this same gate before invoking Vanilla.
        actionOk = GateRemoteDrivenEnemyMovement(
            &npc->m_movementDesireManager,
            "DebugProxyMovementProbe");
    }
    else if (nativeAction == "enable_hit_reactions")
    {
        if (!m_animationTestNativeHitReactionsEnabled)
        {
            actionOk = TryGuardedVoidCall(
                "anim proxy ArkNpc::PopDisableHitReactions",
                [npc]() { ArkNpc::FPopDisableHitReactions(npc); },
                &guardReason);
            if (actionOk)
                m_animationTestNativeHitReactionsEnabled = true;
        }
    }
    else if (nativeAction == "disable_hit_reactions")
    {
        if (m_animationTestNativeHitReactionsEnabled)
        {
            actionOk = TryGuardedVoidCall(
                "anim proxy ArkNpc::PushDisableHitReactions",
                [npc]() { ArkNpc::FPushDisableHitReactions(npc); },
                &guardReason);
            if (actionOk)
                m_animationTestNativeHitReactionsEnabled = false;
        }
    }
    else if (nativeAction == "raise_start")
    {
        actionOk = TryGuardedVoidCall(
            "anim proxy ArkNpc::StartRaiseFromCorpse",
            [npc]() { npc->StartRaiseFromCorpse(true); },
            &guardReason);
    }
    else if (nativeAction == "raise_interrupt")
    {
        actionOk = TryGuardedVoidCall(
            "anim proxy ArkNpc::InterruptRaiseFromCorpse",
            [npc]() { npc->InterruptRaiseFromCorpse(); },
            &guardReason);
    }
    else if (nativeAction == "raise_finish")
    {
        actionOk = TryGuardedVoidCall(
            "anim proxy ArkNpc::FinishRaiseFromCorpse",
            [npc]() { npc->FinishRaiseFromCorpse(); },
            &guardReason);
    }
    else if (nativeAction == "raise_phantom_cast")
    {
        actionOk = TryGuardedVoidCall(
            "anim proxy ArkNpc::StartRaisePhantomCast",
            [npc]() { npc->StartRaisePhantomCast(); },
            &guardReason);
    }
    else if (nativeAction == "begin_stunned")
    {
        bool result = false;
        actionOk = TryGuardedCall(
            "anim proxy ArkNpc::BeginAnimatedStunned",
            [npc]() { return npc->BeginAnimatedStunned(); },
            result,
            &guardReason) && result;
    }
    else if (nativeAction == "end_stunned")
    {
        actionOk = TryGuardedVoidCall(
            "anim proxy ArkNpc::EndAnimatedStunned",
            [npc]() { npc->EndAnimatedStunned(); },
            &guardReason);
    }
    else if (nativeAction == "start_stun")
    {
        const EntityId instigatorId = ArkPlayer::GetInstancePtr() && ArkPlayer::GetInstance().GetEntity()
            ? ArkPlayer::GetInstance().GetEntity()->GetId()
            : 0;
        actionOk = TryGuardedVoidCall(
            "anim proxy ArkNpc::StartStun",
            [npc, instigatorId, duration]() { npc->StartStun(instigatorId, duration > 0.0f ? duration : 2.0f); },
            &guardReason);
    }
    else if (nativeAction == "stop_stun")
    {
        actionOk = TryGuardedVoidCall(
            "anim proxy ArkNpc::StopStun",
            [npc]() { npc->StopStun(); },
            &guardReason);
    }
    else if (nativeAction == "resist")
    {
        const Vec3 forcePosition = ArkPlayer::GetInstancePtr() && ArkPlayer::GetInstance().GetEntity()
            ? ArkPlayer::GetInstance().GetEntity()->GetWorldPos()
            : entity->GetWorldPos();
        bool result = false;
        actionOk = TryGuardedCall(
            "anim proxy ArkNpc::Resist",
            [npc, forcePosition, duration]() { return npc->Resist(forcePosition, duration > 0.0f ? duration : 2.0f); },
            result,
            &guardReason) && result;
    }
    else if (nativeAction == "start_cowering")
    {
        bool result = false;
        actionOk = TryGuardedCall(
            "anim proxy ArkNpc::StartCowering",
            [npc]() { return npc->StartCowering(); },
            result,
            &guardReason) && result;
    }
    else if (nativeAction == "stop_cowering")
    {
        bool result = false;
        actionOk = TryGuardedCall(
            "anim proxy ArkNpc::StopCowering",
            [npc]() { return npc->StopCowering(); },
            result,
            &guardReason) && result;
    }
    else
    {
        detail = "npc_native_failed_unknown_action action=" + nativeAction;
        m_lastAnimationTestEvent = detail;
        ++m_animationTestProxyFailures;
        return false;
    }

    bool stunned = false;
    bool resisting = false;
    bool scrunched = false;
    bool raising = false;
    bool cowering = false;
    bool waitAnimatedStunned = false;
    std::string statusReason;
    TryGuardedCall("anim proxy ArkNpc::IsStunned", [npc]() { return npc->IsStunned(); }, stunned, &statusReason);
    TryGuardedCall("anim proxy ArkNpc::IsResisting", [npc]() { return npc->IsResisting(); }, resisting, &statusReason);
    TryGuardedCall("anim proxy ArkNpc::IsScrunched", [npc]() { return npc->IsScrunched(); }, scrunched, &statusReason);
    TryGuardedCall("anim proxy ArkNpc::IsInRaiseFromCorpseAnim", [npc]() { return npc->IsInRaiseFromCorpseAnim(); }, raising, &statusReason);
    TryGuardedCall("anim proxy ArkNpc::IsCowering", [npc]() { return npc->IsCowering(); }, cowering, &statusReason);
    TryGuardedCall("anim proxy ArkNpc::WaitForAnimatedStunned", [npc]() { return npc->WaitForAnimatedStunned(); }, waitAnimatedStunned, &statusReason);

    detail =
        std::string(actionOk ? "npc_native_ok" : "npc_native_failed") +
        " entity=" + std::to_string(entity->GetId()) +
        " action=" + nativeAction +
        " target=" + std::string(targetNetworkProxy ? "network_proxy" : "anim_proxy") +
        " duration=" + std::to_string(duration) +
        " hitReactions=" + std::to_string(m_animationTestNativeHitReactionsEnabled ? 1 : 0) +
        " stunned=" + std::to_string(stunned ? 1 : 0) +
        " resisting=" + std::to_string(resisting ? 1 : 0) +
        " scrunched=" + std::to_string(scrunched ? 1 : 0) +
        " raising=" + std::to_string(raising ? 1 : 0) +
        " cowering=" + std::to_string(cowering ? 1 : 0) +
        " waitAnimatedStunned=" + std::to_string(waitAnimatedStunned ? 1 : 0) +
        (guardReason.empty() ? std::string() : " reason=" + guardReason) +
        (statusReason.empty() ? std::string() : " statusReason=" + statusReason);
    m_lastAnimationTestEvent = detail;
    if (actionOk &&
        !targetNetworkProxy &&
        nativeAction != "status" &&
        nativeAction != "enable_hit_reactions" &&
        nativeAction != "disable_hit_reactions")
    {
        const float traceSeconds = std::min(std::max(duration > 0.0f ? duration + 1.0f : 3.0f, 1.0f), 6.0f);
        StartAnimationTestProxyTrace(nativeAction, traceSeconds);
    }
    if (!actionOk)
        ++m_animationTestProxyFailures;
    return actionOk;
}

bool ModMain::SampleAnimationTestProxyState(float elapsedSeconds, std::string& detail) const
{
    IEntity* entity = GetAnimationTestProxyEntity();
    IScriptTable* table = entity ? entity->GetScriptTable() : nullptr;
    if (!entity || !table)
    {
        detail = "anim_trace_sample_failed_no_proxy";
        return false;
    }

    std::string currentAnimation;
    std::string currentAnimationType;
    const bool currentOk = ReadAnimProxyCurrentAnimation(table, m_animationTestSlot, currentAnimation, currentAnimationType);
    float animationTime = -1.0f;
    std::string animationTimeType;
    const bool timeOk = ReadAnimProxyAnimationTime(table, m_animationTestSlot, m_animationTestLayer, animationTime, animationTimeType);
    int queueDepth = -1;
    ReadAnimProxyQueueDepth(table, m_animationTestSlot, m_animationTestLayer, queueDepth);

    bool stunned = false;
    bool resisting = false;
    bool scrunched = false;
    bool raising = false;
    bool cowering = false;
    bool waitAnimatedStunned = false;
    std::string statusReason;
    if (ArkNpc* npc = EntityUtils::GetArkNpc(entity))
    {
        TryGuardedCall("anim trace ArkNpc::IsStunned", [npc]() { return npc->IsStunned(); }, stunned, &statusReason);
        TryGuardedCall("anim trace ArkNpc::IsResisting", [npc]() { return npc->IsResisting(); }, resisting, &statusReason);
        TryGuardedCall("anim trace ArkNpc::IsScrunched", [npc]() { return npc->IsScrunched(); }, scrunched, &statusReason);
        TryGuardedCall("anim trace ArkNpc::IsInRaiseFromCorpseAnim", [npc]() { return npc->IsInRaiseFromCorpseAnim(); }, raising, &statusReason);
        TryGuardedCall("anim trace ArkNpc::IsCowering", [npc]() { return npc->IsCowering(); }, cowering, &statusReason);
        TryGuardedCall("anim trace ArkNpc::WaitForAnimatedStunned", [npc]() { return npc->WaitForAnimatedStunned(); }, waitAnimatedStunned, &statusReason);
    }

    const Vec3 pos = entity->GetWorldPos();
    std::ostringstream out;
    out
        << "anim_trace_sample"
        << " label=" << m_animationTestTraceLabel
        << " t=" << elapsedSeconds
        << " entity=" << entity->GetId()
        << " slot=" << m_animationTestSlot
        << " layer=" << m_animationTestLayer
        << " queue=" << queueDepth
        << " current=" << (currentOk ? currentAnimation : std::string("fail")) << "/" << currentAnimationType
        << " time=" << (timeOk ? animationTime : -1.0f) << "/" << animationTimeType
        << " stunned=" << (stunned ? 1 : 0)
        << " resisting=" << (resisting ? 1 : 0)
        << " scrunched=" << (scrunched ? 1 : 0)
        << " raising=" << (raising ? 1 : 0)
        << " cowering=" << (cowering ? 1 : 0)
        << " waitAnimatedStunned=" << (waitAnimatedStunned ? 1 : 0)
        << " pos=(" << pos.x << "," << pos.y << "," << pos.z << ")"
        << (statusReason.empty() ? std::string() : " reason=" + statusReason);
    detail = out.str();
    return true;
}

void ModMain::RecordNpcMannequinStoreAction(const char* stage, void* stateSlot, void* transitionPayload, const char* chainTrace)
{
    void* transitionAction = nullptr;
    void* storedAction = nullptr;
    const bool transitionOk = ReadFieldAt(transitionPayload, kNpcBodyStateTransitionActionOffset, transitionAction);
    const bool storedOk = ReadFieldAt(stateSlot, kNpcBodyStateStoredActionOffset, storedAction);
    void* action = transitionAction ? transitionAction : storedAction;
    const bool deepTrace = AnimProxyEnvFlagEnabled("COOP_NPC_MANNEQUIN_STORE_DEEP_TRACE");

    if (!deepTrace)
    {
        ++m_npcMannequinActionTraceCount;
        m_lastNpcMannequinActionTraceEvent =
            std::string("npc_mannequin_action_store_observed stage=") + (stage ? stage : "-") +
            " stateSlot=" + PointerHex(stateSlot) +
            " transition=" + PointerHex(transitionPayload) +
            " transitionRead=" + std::to_string(transitionOk ? 1 : 0) +
            " storedRead=" + std::to_string(storedOk ? 1 : 0) +
            " transitionAction=" + PointerHex(transitionAction) +
            " storedAction=" + PointerHex(storedAction);
        if (chainTrace && chainTrace[0])
            m_lastNpcMannequinActionTraceEvent += std::string(" ") + chainTrace;
        AppendEnemySyncTrace("mannequin_store", m_lastNpcMannequinActionTraceEvent);
        return;
    }

    if (!action || !CoopRuntimeGuards::IsLikelyRuntimeCppObject(action, kIActionFragmentIdOffset + sizeof(int)))
    {
        ++m_npcMannequinActionTraceFailures;
        m_lastNpcMannequinActionTraceEvent =
            std::string("npc_mannequin_action_failed stage=") + (stage ? stage : "-") +
            " stateSlot=" + PointerHex(stateSlot) +
            " transition=" + PointerHex(transitionPayload) +
            " transitionRead=" + std::to_string(transitionOk ? 1 : 0) +
            " storedRead=" + std::to_string(storedOk ? 1 : 0) +
            " transitionAction=" + PointerHex(transitionAction) +
            " storedAction=" + PointerHex(storedAction);
        if (chainTrace && chainTrace[0])
            m_lastNpcMannequinActionTraceEvent += std::string(" ") + chainTrace;
        AppendEnemySyncTrace("mannequin_store", m_lastNpcMannequinActionTraceEvent);
        if (AnimProxyEnvFlagEnabled("COOP_TRACE_NPC_MANNEQUIN_LOGS"))
            CoopRuntimeLog::Write(m_lastNpcMannequinActionTraceEvent);
        return;
    }

    const ActionBaseProbe bestBase = FindBestActionBaseNear(action);
    const void* readBase = bestBase.score >= 8 ? bestBase.base : action;

    std::uintptr_t vtable = 0;
    std::uintptr_t context = 0;
    std::uintptr_t rootScope = 0;
    float activeTime = 0.0f;
    float queueTime = 0.0f;
    uint32_t forcedScopeMask = 0;
    uint32_t installedScopeMask = 0;
    int subContext = 0;
    int priority = 0;
    int status = 0;
    uint32_t flags = 0;
    int fragmentId = -1;
    int optionIdx = 0;
    int userToken = 0;
    int refCount = -1;
    float speedBias = 0.0f;
    float animWeight = 0.0f;

    bool ok = true;
    ok = ReadFieldAt(readBase, 0x00, vtable) && ok;
    ok = ReadFieldAt(readBase, kIActionContextOffset, context) && ok;
    ok = ReadFieldAt(readBase, kIActionActiveTimeOffset, activeTime) && ok;
    ok = ReadFieldAt(readBase, kIActionQueueTimeOffset, queueTime) && ok;
    ok = ReadFieldAt(readBase, kIActionForcedScopeMaskOffset, forcedScopeMask) && ok;
    ok = ReadFieldAt(readBase, kIActionInstalledScopeMaskOffset, installedScopeMask) && ok;
    ok = ReadFieldAt(readBase, kIActionSubContextOffset, subContext) && ok;
    ok = ReadFieldAt(readBase, kIActionPriorityOffset, priority) && ok;
    ok = ReadFieldAt(readBase, kIActionStatusOffset, status) && ok;
    ok = ReadFieldAt(readBase, kIActionFlagsOffset, flags) && ok;
    ok = ReadFieldAt(readBase, kIActionRootScopeOffset, rootScope) && ok;
    ok = ReadFieldAt(readBase, kIActionFragmentIdOffset, fragmentId) && ok;
    ok = ReadFieldAt(readBase, kIActionOptionIdxOffset, optionIdx) && ok;
    ok = ReadFieldAt(readBase, kIActionUserTokenOffset, userToken) && ok;
    ok = ReadFieldAt(readBase, kIActionRefCountOffset, refCount) && ok;
    ok = ReadFieldAt(readBase, kIActionSpeedBiasOffset, speedBias) && ok;
    ok = ReadFieldAt(readBase, kIActionAnimWeightOffset, animWeight) && ok;

    if (ok)
        ++m_npcMannequinActionTraceCount;
    else
        ++m_npcMannequinActionTraceFailures;

    ArkNpc* npc = nullptr;
    EntityId entityId = INVALID_ENTITYID;
    IEntity* entity = nullptr;
    std::string guardReason;
    ReadFieldAt(readBase, kArkNpcAnimActionNpcOffset, npc);
    if (npc && CoopRuntimeGuards::IsLikelyRuntimeCppObject(npc, sizeof(void*) * 4))
    {
        TryGuardedCall("npc mannequin store GetEntityId", [npc]() { return npc->GetEntityId(); }, entityId, &guardReason);
        if (entityId != INVALID_ENTITYID && gEnv && gEnv->pEntitySystem)
            TryGuardedCall("npc mannequin store entity system GetEntity", [entityId]() { return gEnv->pEntitySystem->GetEntity(entityId); }, entity, &guardReason);
        if (!entity)
            TryGuardedCall("npc mannequin store GetEntity", [npc]() { return npc->GetEntity(); }, entity, &guardReason);
    }
    const bool entityValid = entity && CoopRuntimeGuards::IsLikelyRuntimeCppObject(entity, sizeof(void*) * 4);

    const std::string fragmentName = entityValid
        ? ResolveMannequinFragmentNameForKind(MannequinKindFromEntity(entity), fragmentId)
        : ResolveMannequinFragmentName(fragmentId);
    const bool fragmentNameOk = ok && fragmentName != "unknown" && fragmentName != "-";

    std::ostringstream out;
    out
        << "npc_mannequin_action"
        << " stage=" << (stage ? stage : "-")
        << " ok=" << (ok ? 1 : 0)
        << " action=" << PointerHex(action)
        << " base=" << PointerHex(readBase)
        << " baseScore=" << bestBase.score
        << " name=" << fragmentName
        << " nameOk=" << (fragmentNameOk ? 1 : 0)
        << " fragment=" << fragmentId
        << " status=" << status
        << " flags=0x" << std::hex << flags << std::dec
        << " priority=" << priority
        << " subContext=" << subContext
        << " option=" << optionIdx
        << " token=" << userToken
        << " ref=" << refCount
        << " forcedScope=0x" << std::hex << forcedScopeMask
        << " installedScope=0x" << installedScopeMask << std::dec
        << " active=" << activeTime
        << " queue=" << queueTime
        << " speed=" << speedBias
        << " weight=" << animWeight
        << " npc=" << PointerHex(npc)
        << " entity=" << entityId
        << " vtable=" << PointerHex(reinterpret_cast<void*>(vtable))
        << " context=" << PointerHex(reinterpret_cast<void*>(context))
        << " rootScope=" << PointerHex(reinterpret_cast<void*>(rootScope))
        << " transitionAction=" << PointerHex(transitionAction)
        << " storedAction=" << PointerHex(storedAction)
        << " " << DumpActionRawQwords(action)
        << " " << DumpResolvedFragmentIntsNear(action)
        << (guardReason.empty() ? std::string() : " reason=" + guardReason)
        << ((chainTrace && chainTrace[0]) ? std::string(" ") + chainTrace : std::string());
    m_lastNpcMannequinActionTraceEvent = out.str();
    AppendEnemySyncTrace("mannequin_store", m_lastNpcMannequinActionTraceEvent);
    if (AnimProxyEnvFlagEnabled("COOP_TRACE_NPC_MANNEQUIN_LOGS"))
        CoopRuntimeLog::Write(m_lastNpcMannequinActionTraceEvent);
}

void ModMain::BindNpcSemanticAnimatedAction(void* npcPtr, const void* actionPtr, const char* stage)
{
    if (!npcPtr || !actionPtr || !gEnv || !gEnv->pEntitySystem || !IsSessionGameplayReady())
        return;

    auto* npc = reinterpret_cast<ArkNpc*>(npcPtr);
    if (!CoopRuntimeGuards::IsLikelyRuntimeCppObject(npc, sizeof(void*) * 4))
        return;

    EntityId entityId = INVALID_ENTITYID;
    if (!TryGuardedCall(
            "semantic animated action GetEntityId",
            [npc]() { return npc->GetEntityId(); },
            entityId,
            nullptr) ||
        entityId == INVALID_ENTITYID)
    {
        return;
    }

    IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId);
    if (!entity)
        return;

    const MannequinActionSnapshot snapshot = CaptureMannequinActionSnapshot(actionPtr);
    if (!snapshot.ok || !snapshot.base || snapshot.npc != npc)
        return;

    EnemyAuthorityState& state = EnsureEnemyAuthorityState(*entity);
    const bool localIsAuthority =
        (m_networkMode == CoopNetworkMode::Host &&
            (!state.remoteLocomotionAuthority || state.localAttentionClaimed)) ||
        (m_networkMode == CoopNetworkMode::Client && state.localAttentionClaimed);
    std::string concreteOptionDetail;
    const std::string fragmentName = ResolveMannequinFragmentNameForKind(
        MannequinKindFromEntity(entity),
        snapshot.fragmentId);
    const bool concretizedAuthorityRandomOption =
        TryConcretizeLocalAuthorityRandomMannequinOption(
            entity,
            const_cast<void*>(snapshot.base),
            snapshot.fragmentId,
            fragmentName,
            concreteOptionDetail);
    if (concretizedAuthorityRandomOption)
    {
        AppendEnemySyncTrace(
            "semantic_action_bind",
            "native_semantic_action_random_concretized net=" + std::to_string(state.netId) +
                " entity=" + std::to_string(entityId) +
                " fragment=" + std::to_string(snapshot.fragmentId) +
                " action=" + PointerHex(snapshot.base) +
                " detail=" + AnimationStatusToken(concreteOptionDetail) +
                " stage=" + std::string(stage && stage[0] ? stage : "-"));
    }
    uint64_t currentContextId = 0;
    const bool currentContextRead = TryGuardedCall(
        "semantic animated action current context",
        [npc]() { return npc->GetCurrentAbilityContextId(); },
        currentContextId,
        nullptr);
    const float nowSeconds = gEnv && gEnv->pTimer
        ? gEnv->pTimer->GetAsyncCurTime()
        : 0.0f;
    const float pendingAgeSeconds = nowSeconds - state.pendingSemanticObservedAtSeconds;
    const bool freshPendingContext =
        pendingAgeSeconds >= -0.01f &&
        pendingAgeSeconds <= kPendingSemanticAsyncActionWindowSeconds;

    bool boundLocal = false;
    if (localIsAuthority &&
        state.pendingSemanticContextId != 0 &&
        freshPendingContext &&
        currentContextRead &&
        currentContextId == state.pendingSemanticContextId)
    {
        state.pendingSemanticAction = snapshot.base;
        state.pendingSemanticFragmentId = snapshot.fragmentId;
        boundLocal = true;
    }

    if (boundLocal)
    {
        AppendEnemySyncTrace(
            "semantic_action_bind",
            "native_semantic_action_bound net=" + std::to_string(state.netId) +
                " entity=" + std::to_string(entityId) +
                " context=" + std::to_string(state.pendingSemanticContextId) +
                " seq=" + std::to_string(state.localAuthoritySemanticSequence) +
                " fragment=" + std::to_string(snapshot.fragmentId) +
                " action=" + PointerHex(snapshot.base) +
                " route=authority_exact_native" +
                " stage=" + std::string(stage && stage[0] ? stage : "-"));
    }
}

void ModMain::RecordNpcMannequinActionLifecycle(const char* stage, void* action, const char* chainTrace)
{
    const MannequinActionSnapshot snapshot = CaptureMannequinActionSnapshot(action);
    if (!snapshot.ok || !snapshot.base)
    {
        ++m_npcMannequinActionTraceFailures;
        m_lastNpcMannequinActionTraceEvent =
            std::string("npc_mannequin_lifecycle_failed stage=") + (stage ? stage : "-") +
            " " + FormatMannequinActionSnapshot(snapshot, "action") +
            ((chainTrace && chainTrace[0]) ? std::string(" ") + chainTrace : std::string());
        AppendEnemySyncTrace("mannequin_lifecycle", m_lastNpcMannequinActionTraceEvent);
        return;
    }

    ArkNpc* npc = snapshot.npc;
    EntityId entityId = INVALID_ENTITYID;
    IEntity* entity = nullptr;
    std::string guardReason;
    if (npc && CoopRuntimeGuards::IsLikelyRuntimeCppObject(npc, sizeof(void*) * 4))
    {
        TryGuardedCall("npc mannequin lifecycle GetEntityId", [npc]() { return npc->GetEntityId(); }, entityId, &guardReason);
        if (entityId != INVALID_ENTITYID && gEnv && gEnv->pEntitySystem)
            TryGuardedCall("npc mannequin lifecycle entity system GetEntity", [entityId]() { return gEnv->pEntitySystem->GetEntity(entityId); }, entity, &guardReason);
        if (!entity)
            TryGuardedCall("npc mannequin lifecycle GetEntity", [npc]() { return npc->GetEntity(); }, entity, &guardReason);
    }

    const bool entityValid = entity && CoopRuntimeGuards::IsLikelyRuntimeCppObject(entity, sizeof(void*) * 4);
    const std::string fragmentName = entityValid
        ? ResolveMannequinFragmentNameForKind(MannequinKindFromEntity(entity), snapshot.fragmentId)
        : ResolveMannequinFragmentName(snapshot.fragmentId);
    const bool fragmentNameOk = fragmentName != "unknown" && fragmentName != "-";
    const std::string_view stageView(stage ? stage : "");
    const bool actionFragmentStartedAfter =
        stageView.find("ArkNpcAnimAction::OnFragmentStarted") != std::string_view::npos &&
        stageView.find(":after") != std::string_view::npos;
    bool recordedAuthorityExit = false;
    const bool actionTerminalBefore =
        stageView.find("ArkNpcAnimAction::Exit:before") != std::string_view::npos ||
        stageView.find("ArkNpcAnimAction::Fail:before") != std::string_view::npos;
    if (entityValid && actionTerminalBefore)
    {
        EnemyAuthorityState& state = EnsureEnemyAuthorityState(*entity);
        const auto localActionIt = state.localNativeMannequinActions.find(snapshot.base);
        if (localActionIt != state.localNativeMannequinActions.end())
        {
            const EnemyAuthorityState::LocalNativeMannequinAction nativeAction = localActionIt->second;
            recordedAuthorityExit = QueueLocalEnemyMannequinActionEventForHook(
                state,
                nativeAction,
                CoopProtocol::EnemyMannequinActionCommand::Exit,
                fragmentNameOk ? fragmentName.c_str() : "native action exit");
            state.localNativeMannequinActions.erase(localActionIt);
        }
    }
    EnemyMannequinClassification classification =
        ClassifyEnemyMannequinFragment(snapshot.fragmentId, fragmentName);
    bool semanticContextAction = false;
    bool remoteNativeMirrorAction = false;
    if (entityValid && actionFragmentStartedAfter)
    {
        EnemyAuthorityState& state = EnsureEnemyAuthorityState(*entity);
        remoteNativeMirrorAction = std::any_of(
            state.remoteNativeMannequinActions.begin(),
            state.remoteNativeMannequinActions.end(),
            [&snapshot](const auto& entry)
            {
                return entry.second.fragmentId == snapshot.fragmentId &&
                    entry.second.lease != nullptr &&
                    snapshot.base == entry.second.lease.get();
            });
        const bool exactPendingSemanticAction =
            state.pendingSemanticContextId != 0 &&
            state.pendingSemanticAction != nullptr &&
            state.pendingSemanticAction == snapshot.base &&
            state.pendingSemanticFragmentId == snapshot.fragmentId;
        semanticContextAction = exactPendingSemanticAction;
        if (semanticContextAction && classification.flags == 0)
            classification.flags = CoopProtocol::kEnemyLocomotionFlagMannequinDriven;

    }
    const int nativeOptionIdx = snapshot.optionOk
        ? (snapshot.optionIdx == 0xfffffffeu
            ? -2
            : (snapshot.optionIdx < static_cast<uint32_t>(CoopProtocol::kInvalidMannequinOrdinal)
                ? static_cast<int>(snapshot.optionIdx)
                : -1))
        : -1;

    bool recordedAuthority = false;
    if (fragmentNameOk && actionFragmentStartedAfter)
    {
        // An unknown classification is still a real native action. Mirror its
        // exact fragment/ordinal/tag state instead of dropping it or inventing
        // a receiver-side substitute.
        if (classification.flags == 0)
            classification.flags = CoopProtocol::kEnemyLocomotionFlagMannequinDriven;
        recordedAuthority = RecordEnemyMannequinAuthorityState(
            entityId,
            snapshot.fragmentId,
            fragmentName.c_str(),
            stage,
            classification.flags,
            classification.attackKind,
            nativeOptionIdx,
            snapshot.priority >= 0 ? static_cast<uint32_t>(snapshot.priority) : 0u,
            snapshot.base);
    }

    ++m_npcMannequinActionTraceCount;
    std::ostringstream out;
    out
        << "npc_mannequin_lifecycle"
        << " stage=" << (stage ? stage : "-")
        << " recorded=" << (recordedAuthority ? 1 : 0)
        << " recordedExit=" << (recordedAuthorityExit ? 1 : 0)
        << " semanticContextAction=" << (semanticContextAction ? 1 : 0)
        << " remoteNativeMirrorAction=" << (remoteNativeMirrorAction ? 1 : 0)
        << " entity=" << entityId
        << " name=" << fragmentName
        << " nameOk=" << (fragmentNameOk ? 1 : 0)
        << " nativeOption=" << nativeOptionIdx
        << " " << FormatMannequinActionSnapshot(snapshot, "action")
        << (guardReason.empty() ? std::string() : " reason=" + guardReason)
        << ((chainTrace && chainTrace[0]) ? std::string(" ") + chainTrace : std::string());
    m_lastNpcMannequinActionTraceEvent = out.str();
    AppendEnemySyncTrace("mannequin_lifecycle", m_lastNpcMannequinActionTraceEvent);
    if (AnimProxyEnvFlagEnabled("COOP_TRACE_NPC_MANNEQUIN_LOGS"))
        CoopRuntimeLog::Write(m_lastNpcMannequinActionTraceEvent);
}

bool ModMain::ShouldBlockRemoteEnemyMannequinActionStart(
    void* action,
    const char* stage,
    const char* chainTrace)
{
    if (!action ||
        m_networkMode == CoopNetworkMode::Off ||
        !IsSessionGameplayReady() ||
        !gEnv ||
        !gEnv->pEntitySystem)
    {
        return false;
    }

    const MannequinActionSnapshot snapshot = CaptureMannequinActionSnapshot(action);
    if (!snapshot.ok || !snapshot.base)
        return false;

    ArkNpc* npc = snapshot.npc;
    if (!npc || !CoopRuntimeGuards::IsLikelyRuntimeCppObject(npc, sizeof(void*) * 4))
        return false;

    EntityId entityId = INVALID_ENTITYID;
    std::string guardReason;
    if (!TryGuardedCall("remote enemy mannequin start gate GetEntityId", [npc]() { return npc->GetEntityId(); }, entityId, &guardReason) ||
        entityId == INVALID_ENTITYID)
    {
        return false;
    }

    IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId);
    if (!entity)
        return false;

    if (IsRemoteProxyEntityOrSpawnName(*entity))
    {
        // Network player presentation uses direct CryAnimation clips and
        // hand-tuned surrogate freeze frames. Any ArkNpc Mannequin action on
        // the same body is native proxy AI and would briefly replace that
        // packet-driven pose until the following pose update repairs it.
        ++m_enemyMannequinLocalSuppressions;
        std::ostringstream out;
        out
            << "blocked_proxy_native_mannequin"
            << " stage=" << (stage && stage[0] ? stage : "-")
            << " entity=" << entityId
            << " fragment=" << snapshot.fragmentId
            << " count=" << m_enemyMannequinLocalSuppressions
            << (guardReason.empty() ? std::string() : " guard=" + AnimationStatusToken(guardReason))
            << ((chainTrace && chainTrace[0]) ? std::string(" ") + chainTrace : std::string());
        m_lastEnemyMannequinStateEvent = out.str();
        AppendEnemySyncTrace("proxy_ai_gate", m_lastEnemyMannequinStateEvent);
        RequestBlockedMannequinActionStop(snapshot.base);
        return true;
    }

    if (AnimProxyEnvFlagEnabled("COOP_DISABLE_REMOTE_ENEMY_MANNEQUIN_START_GATE") ||
        m_applyingRemoteEnemyDeathCommit ||
        !m_enemyLocomotionSyncEnabled)
        return false;

    const bool clientRemotePuppet = IsClientRemoteEnemyPuppet(*entity);
    if (!clientRemotePuppet && !IsEnemyRuntimeControlCandidate(*entity))
        return false;

    EnemyAuthorityState* state = nullptr;
    const auto netIt = m_enemyNetIdsByEntity.find(entityId);
    if (netIt != m_enemyNetIdsByEntity.end())
        state = FindEnemyAuthorityByNetId(netIt->second);
    if (!state)
    {
        for (auto& entry : m_enemyAuthorities)
        {
            if (entry.second.entityId == entityId)
            {
                state = &entry.second;
                m_enemyNetIdsByEntity[entityId] = entry.first;
                break;
            }
        }
    }
    if (!state)
    {
        if (!clientRemotePuppet)
            return false;

        ++m_enemyMannequinLocalSuppressions;
        std::ostringstream out;
        out
            << "mannequin_start_gate block"
            << " stage=" << (stage && stage[0] ? stage : "-")
            << " entity=" << entityId
            << " fragment=" << snapshot.fragmentId
            << " reason=remote_spawn_puppet_prestate"
            << (guardReason.empty() ? std::string() : " guard=" + AnimationStatusToken(guardReason))
            << ((chainTrace && chainTrace[0]) ? std::string(" ") + chainTrace : std::string());
        m_lastEnemyMannequinStateEvent = out.str();
        AppendEnemySyncTrace("mannequin_start_gate", m_lastEnemyMannequinStateEvent);
        RequestBlockedMannequinActionStop(snapshot.base);
        return true;
    }

    const CoopEnemyControlPolicy::Decision decision =
        CoopEnemyControlPolicy::Evaluate(BuildLocalEnemyControlPolicyContext(*state, *entity));
    if (!decision.remoteDriven || decision.localVanillaAuthority || !decision.BlocksAnyLocalVanilla())
        return false;

    const std::string fragmentNameWithKind =
        ResolveMannequinFragmentNameForKind(MannequinKindFromEntity(entity), snapshot.fragmentId);
    bool npcDead = false;
    TryGuardedCall(
        "remote enemy mannequin start gate IsDead",
        [npc]() { return npc->IsDead(); },
        npcDead,
        &guardReason);
    if (npcDead)
    {
        const std::string lowerFragmentName = ToLowerAsciiAnimation(fragmentNameWithKind);
        const bool deathPresentationAction =
            ContainsToken(lowerFragmentName, "death") ||
            ContainsToken(lowerFragmentName, "dead") ||
            ContainsToken(lowerFragmentName, "die") ||
            ContainsToken(lowerFragmentName, "ragdoll") ||
            ContainsToken(lowerFragmentName, "corpse");
        if (!deathPresentationAction)
        {
            ++m_enemyMannequinLocalSuppressions;
            std::ostringstream out;
            out
                << "mannequin_start_gate block"
                << " stage=" << (stage && stage[0] ? stage : "-")
                << " net=" << state->netId
                << " entity=" << entityId
                << " fragment=" << snapshot.fragmentId
                << " name=" << fragmentNameWithKind
                << " reason=remote_dead_non_death_action"
                << (guardReason.empty() ? std::string() : " guard=" + AnimationStatusToken(guardReason))
                << ((chainTrace && chainTrace[0]) ? std::string(" ") + chainTrace : std::string());
            m_lastEnemyMannequinStateEvent = out.str();
            AppendEnemySyncTrace("mannequin_start_gate", m_lastEnemyMannequinStateEvent);
            if (AnimProxyEnvFlagEnabled("COOP_TRACE_NPC_MANNEQUIN_LOGS"))
                CoopRuntimeLog::Write(m_lastEnemyMannequinStateEvent);
            RequestBlockedMannequinActionStop(snapshot.base);
            return true;
        }
    }
    const EnemyMannequinClassification classification =
        ClassifyEnemyMannequinFragment(snapshot.fragmentId, fragmentNameWithKind);
    const uint32_t actionFlags = classification.flags;
    const uint32_t movementFlags = CoopEnemyControlPolicy::MovementFlags();
    const bool actionIsMovement =
        (actionFlags & (movementFlags |
            CoopProtocol::kEnemyLocomotionFlagDashing |
            CoopProtocol::kEnemyLocomotionFlagShifting |
            CoopProtocol::kEnemyLocomotionFlagMorphing)) != 0;
    const bool actionIsBurstMovement =
        (actionFlags & (CoopProtocol::kEnemyLocomotionFlagDashing |
            CoopProtocol::kEnemyLocomotionFlagShifting |
            CoopProtocol::kEnemyLocomotionFlagMorphing |
            CoopProtocol::kEnemyLocomotionFlagLunging)) != 0;
    const bool actionIsTurn = (actionFlags & CoopProtocol::kEnemyLocomotionFlagTurning) != 0;
    const bool actionIsLocalAttack =
        (actionFlags & CoopProtocol::kEnemyLocomotionFlagAttacking) != 0;
    const bool actionIsSharedBodyState =
        (actionFlags & (CoopEnemyControlPolicy::BodyLockFlags() |
            CoopProtocol::kEnemyLocomotionFlagHitReacting)) != 0;
    const bool actionUnknown = actionFlags == 0;
    const bool remoteNativeMirrorMatch = std::any_of(
        state->remoteNativeMannequinActions.begin(),
        state->remoteNativeMannequinActions.end(),
        [&snapshot](const auto& entry)
        {
            return entry.second.fragmentId == snapshot.fragmentId &&
                entry.second.lease != nullptr &&
                snapshot.base == entry.second.lease.get();
        });
    bool block = false;
    const char* reason = "-";
    if (remoteNativeMirrorMatch)
    {
        // Only an exact native action object created for the received sequence
        // may pass. Observer-local AI and reconstructed ability contexts never
        // receive an exception from this gate.
        block = false;
    }
    else if (!decision.localFocus)
    {
        block = true;
        reason = actionUnknown ? "observer_unknown_local_action" : "observer_local_action";
    }
    else if (actionIsSharedBodyState)
    {
        block = true;
        reason = "local_focus_uses_authority_shared_body_state";
    }
    else if (actionIsBurstMovement && decision.blockMovement)
    {
        block = true;
        reason = "local_focus_uses_authority_burst_movement";
    }
    else if (actionIsMovement && decision.blockMovement)
    {
        block = true;
        reason = "local_focus_uses_authority_continuous_movement";
    }
    else if (actionIsLocalAttack && decision.blockAttack)
    {
        block = true;
        reason = "local_focus_replays_authority_combat";
    }
    else if (!actionIsLocalAttack)
    {
        // Facing remains local through the read-only attention/rotation lane.
        // A separate Notice, Search, Patrol, Fall or unknown full-body action
        // would independently animate the shared body and can keep Vanilla's
        // attack scope occupied even though its root motion is rejected.
        block = true;
        reason = actionUnknown
            ? "local_focus_blocks_unknown_body_action"
            : (actionIsTurn
                ? "local_focus_rotation_uses_facing_lane"
                : "local_focus_blocks_nonattack_body_action");
    }

    if (!block)
    {
        if (decision.localFocus && actionIsLocalAttack)
        {
            ++state->localMannequinCombatAllows;
            RecordRemoteObserverLocalIntentSample(
                *state,
                *entity,
                EnemyAuthorityState::ReadOnlyIntentMannequin |
                    EnemyAuthorityState::ReadOnlyIntentCombat,
                actionFlags,
                0,
                INVALID_ENTITYID,
                stage,
                false);
        }
        return false;
    }

    RecordRemoteObserverLocalIntentSample(
        *state,
        *entity,
        EnemyAuthorityState::ReadOnlyIntentMannequin |
            (actionIsLocalAttack
                ? EnemyAuthorityState::ReadOnlyIntentCombat
                : EnemyAuthorityState::ReadOnlyIntentNone) |
            (actionIsMovement
                ? EnemyAuthorityState::ReadOnlyIntentMovement
                : EnemyAuthorityState::ReadOnlyIntentNone) |
            (actionIsTurn
                ? EnemyAuthorityState::ReadOnlyIntentFacing
                : EnemyAuthorityState::ReadOnlyIntentNone),
        actionFlags,
        0,
        INVALID_ENTITYID,
        stage,
        true);

    ++m_enemyMannequinLocalSuppressions;
    std::ostringstream out;
    out
        << "mannequin_start_gate block"
        << " stage=" << (stage && stage[0] ? stage : "-")
        << " net=" << state->netId
        << " entity=" << entityId
        << " fragment=" << snapshot.fragmentId
        << " name=" << fragmentNameWithKind
        << " flags=" << actionFlags
        << " remoteFragment=" << state->remoteMannequinFragmentId
        << " remoteSeq=" << state->remoteMannequinSequence
        << " remoteFlags=" << state->remoteMannequinFlags
        << " exactNative=" << (remoteNativeMirrorMatch ? 1 : 0)
        << " focus=" << (decision.localFocus ? 1 : 0)
        << " reason=" << reason
        << (guardReason.empty() ? std::string() : " guard=" + AnimationStatusToken(guardReason))
        << ((chainTrace && chainTrace[0]) ? std::string(" ") + chainTrace : std::string());
    m_lastEnemyMannequinStateEvent = out.str();
    AppendEnemySyncTrace("mannequin_start_gate", m_lastEnemyMannequinStateEvent);
    if (AnimProxyEnvFlagEnabled("COOP_TRACE_NPC_MANNEQUIN_LOGS"))
        CoopRuntimeLog::Write(m_lastEnemyMannequinStateEvent);
    RequestBlockedMannequinActionStop(snapshot.base);
    return true;
}

bool ModMain::TryConcretizeLocalAuthorityRandomMannequinOption(
    IEntity* entity,
    void* action,
    int fragmentId,
    const std::string& fragmentName,
    std::string& detail)
{
    detail.clear();
    if (!entity || !action || fragmentId < 0 ||
        fragmentName.empty() || fragmentName == "unknown" || fragmentName == "-")
    {
        return false;
    }

    uint32_t option = 0xffffffffu;
    if (!ReadFieldAt(action, kIActionOptionIdxOffset, option) ||
        option != 0xfffffffeu)
    {
        return false;
    }

    EnemyAuthorityState& state = EnsureEnemyAuthorityState(*entity);
    const bool localIsAuthority =
        (m_networkMode == CoopNetworkMode::Host &&
            (!state.remoteLocomotionAuthority || state.localAttentionClaimed)) ||
        (m_networkMode == CoopNetworkMode::Client && state.localAttentionClaimed);
    if (!localIsAuthority)
    {
        detail = "random_option_left_native_non_authority";
        return false;
    }

    EnemyMannequinClassification classification =
        ClassifyEnemyMannequinFragment(fragmentId, fragmentName);
    if (classification.flags == 0)
        classification.flags = CoopProtocol::kEnemyLocomotionFlagMannequinDriven;
    const uint32_t localFlags = classification.flags & (
        CoopProtocol::kEnemyLocomotionFlagAttacking |
        CoopProtocol::kEnemyLocomotionFlagHitReacting |
        CoopProtocol::kEnemyLocomotionFlagTurning |
        CoopProtocol::kEnemyLocomotionFlagMannequinDriven);
    uint64_t choiceSeed = state.stableEnemyId ^
        (static_cast<uint64_t>(static_cast<uint32_t>(fragmentId)) << 32) ^
        static_cast<uint64_t>(state.localMannequinSequence + 1u) * 0x9E3779B97F4A7C15ULL;
    choiceSeed ^= choiceSeed >> 30;
    choiceSeed *= 0xBF58476D1CE4E5B9ULL;
    choiceSeed ^= choiceSeed >> 27;
    choiceSeed *= 0x94D049BB133111EBULL;
    choiceSeed ^= choiceSeed >> 31;
    if (choiceSeed == 0)
        choiceSeed = 1;

    const int concreteOrdinal = ResolveNpcMannequinOrdinalForRuntime(
        entity,
        fragmentId,
        classification.flags,
        localFlags,
        -1,
        detail,
        choiceSeed);
    if (concreteOrdinal < 0)
        return false;

    auto* actionBytes = reinterpret_cast<std::byte*>(action);
    auto* optionField = reinterpret_cast<uint32_t*>(
        actionBytes + kIActionOptionIdxOffset);
    if (!CoopRuntimeGuards::TryWriteRuntimeValue(
            optionField,
            static_cast<uint32_t>(concreteOrdinal)))
    {
        detail += " write_failed";
        return false;
    }
    return true;
}

void ModMain::RecordNpcMannequinActionConstruct(
    const char* stage,
    void* action,
    void* npcPtr,
    int priority,
    int fragmentId,
    void* tagState,
    int arg6,
    int arg7,
    int arg8,
    const char* chainTrace)
{
    ++m_npcMannequinConstructTraceCount;

    EntityId entityId = INVALID_ENTITYID;
    const char* entityName = "-";
    IEntity* entity = nullptr;
    std::string guardReason;
    ArkNpc* npc = reinterpret_cast<ArkNpc*>(npcPtr);
    if (npc && CoopRuntimeGuards::IsLikelyRuntimeCppObject(npc, sizeof(void*) * 4))
    {
        TryGuardedCall("npc anim construct GetEntityId", [npc]() { return npc->GetEntityId(); }, entityId, &guardReason);
        if (entityId != INVALID_ENTITYID && gEnv && gEnv->pEntitySystem)
            TryGuardedCall("npc anim construct entity system GetEntity", [entityId]() { return gEnv->pEntitySystem->GetEntity(entityId); }, entity, &guardReason);
        if (!entity)
            TryGuardedCall("npc anim construct GetEntity", [npc]() { return npc->GetEntity(); }, entity, &guardReason);
        if (entity && CoopRuntimeGuards::IsLikelyRuntimeCppObject(entity, sizeof(void*) * 4))
        {
            const char* rawEntityName = nullptr;
            if (TryGuardedCall("npc anim construct entity GetName", [entity]() { return entity->GetName(); }, rawEntityName, &guardReason))
            {
                static thread_local std::string safeEntityName;
                safeEntityName = CoopRuntimeGuards::ReadRuntimeCString(rawEntityName, 128);
                entityName = safeEntityName.empty() ? "-" : safeEntityName.c_str();
            }
        }
    }

    const bool entityValid = entity && CoopRuntimeGuards::IsLikelyRuntimeCppObject(entity, sizeof(void*) * 4);
    const std::string fragmentName = entityValid
        ? ResolveMannequinFragmentNameForKind(MannequinKindFromEntity(entity), fragmentId)
        : ResolveMannequinFragmentName(fragmentId);
    const bool fragmentNameOk = fragmentName != "unknown" && fragmentName != "-";
    uint32_t constructedOption = 0xffffffffu;
    const bool constructedOptionOk =
        action && ReadFieldAt(action, kIActionOptionIdxOffset, constructedOption);
    bool concretizedAuthorityRandomOption = false;
    std::string concreteOptionDetail;
    if (entityValid && fragmentNameOk && constructedOptionOk)
    {
        concretizedAuthorityRandomOption =
            TryConcretizeLocalAuthorityRandomMannequinOption(
                entity,
                action,
                fragmentId,
                fragmentName,
                concreteOptionDetail);
        if (concretizedAuthorityRandomOption)
        {
            ReadFieldAt(action, kIActionOptionIdxOffset, constructedOption);
        }
    }
    const std::string inputTagStateBytes = DumpMannequinTagStateBytes(tagState);
    const std::string actionFragTagsBytes = DumpMannequinTagStateBytes(
        reinterpret_cast<const uint8_t*>(action) + kIActionFragTagsOffset);
    std::ostringstream out;
    out
        << "npc_mannequin_construct"
        << " stage=" << (stage ? stage : "-")
        << " action=" << PointerHex(action)
        << " npc=" << PointerHex(npcPtr)
        << " entity=" << entityId
        << " entityName=" << (entityName ? entityName : "-")
        << " priority=" << priority
        << " fragment=" << fragmentId
        << " name=" << fragmentName
        << " nameOk=" << (fragmentNameOk ? 1 : 0)
        << " option=" << (constructedOptionOk ? std::to_string(constructedOption) : std::string("-"))
        << " authorityRandomConcrete=" << (concretizedAuthorityRandomOption ? 1 : 0)
        << (concreteOptionDetail.empty()
            ? std::string()
            : " optionDetail=" + AnimationStatusToken(concreteOptionDetail))
        << " tagState=" << PointerHex(tagState)
        << " tagBytes=" << inputTagStateBytes
        << " actionFragTags=" << actionFragTagsBytes
        << " arg6=" << arg6
        << " arg7=" << arg7
        << " arg8=" << arg8
        << (guardReason.empty() ? std::string() : " reason=" + guardReason)
        << ((chainTrace && chainTrace[0]) ? std::string(" ") + chainTrace : std::string());

    m_lastNpcMannequinConstructTraceEvent = out.str();
    AppendEnemySyncTrace("mannequin_construct", m_lastNpcMannequinConstructTraceEvent);

    // Construction never publishes an action. A queued action can still be
    // superseded before its fragment starts; this boundary only turns the
    // authority's random directive into one concrete choice so a later real
    // Start edge can mirror the same variant instead of rerolling per peer.

    if (AnimProxyEnvFlagEnabled("COOP_TRACE_NPC_MANNEQUIN_LOGS"))
        CoopRuntimeLog::Write(m_lastNpcMannequinConstructTraceEvent);
}

std::string ModMain::ResolveNpcMannequinFragmentNameForRuntime(int fragmentId)
{
    return ResolveMannequinFragmentName(fragmentId);
}

std::string ModMain::ResolveNpcMannequinFragmentNameForRuntime(IEntity* entity, int fragmentId)
{
    if (!entity)
        return ResolveMannequinFragmentName(fragmentId);
    return ResolveMannequinFragmentNameForKind(MannequinKindFromEntity(entity), fragmentId);
}

std::string ModMain::ResolveNpcMannequinKindForRuntime(IEntity* entity)
{
    return MannequinKindFromEntity(entity);
}

int ModMain::ResolveNpcMannequinFragmentIdForRuntime(IEntity* entity, const char* fragmentName)
{
    const std::string kind = MannequinKindFromEntity(entity);
    return ResolveMannequinFragmentIdForKind(kind, fragmentName);
}

bool ModMain::ResolveNpcMannequinVariantTagsForRuntime(
    IEntity* entity,
    int fragmentId,
    int ordinal,
    std::string& outTags,
    std::string& outFragTags,
    std::string& detail)
{
    outTags.clear();
    outFragTags.clear();
    const std::string kind = MannequinKindFromEntity(entity);
    const std::string fragmentName = ResolveMannequinFragmentNameForKind(kind, fragmentId);

    MannequinSnippetEntry entry;
    if (!ResolveMannequinVariantForKind(kind, fragmentName, ordinal, entry, detail))
        return false;

    outTags = entry.tags;
    outFragTags = entry.fragTags;
    return true;
}

uint32_t ModMain::ClassifyNpcMannequinVariantTagsForRuntime(
    IEntity* entity,
    int fragmentId,
    int ordinal,
    std::string& detail)
{
    std::string tags;
    std::string fragTags;
    if (!ResolveNpcMannequinVariantTagsForRuntime(entity, fragmentId, ordinal, tags, fragTags, detail))
        return 0;

    const std::string combined = ToLowerAsciiAnimation(tags + "+" + fragTags);
    uint32_t flags = 0;
    if (ContainsToken(combined, "incombat") ||
        ContainsToken(combined, "combat") ||
        ContainsToken(combined, "hunt"))
    {
        flags |= CoopProtocol::kEnemyLocomotionFlagInCombat;
    }
    if (ContainsToken(combined, "zerog"))
        flags |= CoopProtocol::kEnemyLocomotionFlagMannequinDriven;
    return flags;
}

int ModMain::ResolveNpcMannequinOrdinalForRuntime(
    IEntity* entity,
    int fragmentId,
    uint32_t desiredFlags,
    uint32_t localFlags,
    int remoteOrdinal,
    std::string& detail,
    uint64_t deterministicChoiceSeed)
{
    const std::string kind = MannequinKindFromEntity(entity);
    const std::string fragmentNameWithKind = ResolveMannequinFragmentNameForKind(kind, fragmentId);
    const std::string fragmentName = StripMannequinKindPrefix(fragmentNameWithKind);
    if (!gCL || !gCL->pXmlUtils || fragmentName.empty() || fragmentName == "-" || fragmentName == "unknown")
    {
        detail = "bad_runtime_fragment";
        return -1;
    }

    struct CachedOrdinalResolution
    {
        std::vector<int> bestOrdinals;
        int bestScore = std::numeric_limits<int>::min();
        std::string bestTags;
        std::string bestFragTags;
        std::string path;
        std::string failure;
    };
    static std::unordered_map<std::string, CachedOrdinalResolution> resolutionCache;

    const std::string cacheKey =
        kind + "|" + fragmentName + "|" + std::to_string(desiredFlags) + "|" +
        std::to_string(localFlags) + "|" + std::to_string(remoteOrdinal);
    auto cacheIt = resolutionCache.find(cacheKey);
    if (cacheIt == resolutionCache.end())
    {
        CachedOrdinalResolution cached;
        cached.failure = "no_paths";

        for (const char* path : MannequinDatabasePaths(kind))
        {
            pugi::xml_parse_result parseResult;
            pugi::xml_document doc = gCL->pXmlUtils->LoadXmlFromFile(path, &parseResult);
            if (!parseResult)
            {
                cached.failure = std::string("parse_failed_") + path + "_" + parseResult.description();
                continue;
            }

            pugi::xml_node fragmentNode = doc.child("AnimDB").child("FragmentList").child(fragmentName.c_str());
            if (!fragmentNode)
            {
                cached.failure = std::string("missing_fragment_") + fragmentName + "_path_" + path;
                continue;
            }

            int ordinal = 0;
            for (pugi::xml_node variantNode = fragmentNode.child("Fragment"); variantNode; variantNode = variantNode.next_sibling("Fragment"))
            {
                MannequinSnippetEntry entry = BuildSnippetEntry(fragmentNode, variantNode, ordinal++);
                const int score = ScoreMannequinVariantForFlags(entry, desiredFlags, localFlags, remoteOrdinal);
                if (score > cached.bestScore)
                {
                    cached.bestScore = score;
                    cached.bestOrdinals.assign(1, entry.ordinal);
                    cached.bestTags = entry.tags;
                    cached.bestFragTags = entry.fragTags;
                }
                else if (score == cached.bestScore)
                {
                    cached.bestOrdinals.push_back(entry.ordinal);
                }
            }

            if (!cached.bestOrdinals.empty())
            {
                cached.path = path;
                cached.failure.clear();
                break;
            }

            cached.failure = "fragment_no_variants_" + fragmentName + "_path_" + path;
        }

        cacheIt = resolutionCache.emplace(cacheKey, std::move(cached)).first;
    }

    const CachedOrdinalResolution& cached = cacheIt->second;
    if (cached.bestOrdinals.empty())
    {
        detail = "ordinal_resolve_failed kind=" + kind + " fragment=" + fragmentName + " reason=" + cached.failure;
        return -1;
    }

    const size_t selectedIndex = deterministicChoiceSeed == 0
        ? 0
        : static_cast<size_t>(deterministicChoiceSeed % cached.bestOrdinals.size());
    const int selectedOrdinal = cached.bestOrdinals[selectedIndex];
    detail =
        "ordinal_resolved kind=" + kind +
        " fragment=" + fragmentName +
        " ordinal=" + std::to_string(selectedOrdinal) +
        " choices=" + std::to_string(cached.bestOrdinals.size()) +
        " choice=" + std::to_string(selectedIndex) +
        " score=" + std::to_string(cached.bestScore) +
        " desired=" + std::to_string(desiredFlags) +
        " local=" + std::to_string(localFlags) +
        " remoteOrdinal=" + std::to_string(remoteOrdinal) +
        " tags=" + cached.bestTags +
        " fragTags=" + cached.bestFragTags +
        " path=" + cached.path;
    return selectedOrdinal;
}


bool ModMain::RecordEnemyMannequinAuthorityState(
    EntityId entityId,
    int fragmentId,
    const char* fragmentName,
    const char* stage,
    uint32_t flags,
    uint32_t attackKind,
    int optionIdx,
    uint32_t priority,
    const void* action)
{
    if ((flags == 0 && fragmentId < 0) ||
        entityId == INVALID_ENTITYID ||
        entityId == m_proxyEntityId ||
        m_networkMode == CoopNetworkMode::Off ||
        !gEnv ||
        !gEnv->pEntitySystem)
    {
        return false;
    }

    IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId);
    if (!entity || !EntityUtils::GetArkNpc(entity))
        return false;

    const char* entityNameRaw = entity->GetName();
    const std::string_view entityName(entityNameRaw ? entityNameRaw : "");
    if (entityName.find("CoopProxy") != std::string_view::npos ||
        entityName.find("CoopAnim") != std::string_view::npos)
    {
        return false;
    }

    EnemyAuthorityState& state = EnsureEnemyAuthorityState(*entity);

    const bool localIsAuthority =
        (m_networkMode == CoopNetworkMode::Host && (!state.remoteLocomotionAuthority || state.localAttentionClaimed)) ||
        (m_networkMode == CoopNetworkMode::Client && state.localAttentionClaimed);
    if (!localIsAuthority)
        return false;

    if (entityName.find("CoopEnemyPuppet") != std::string_view::npos &&
        m_networkMode != CoopNetworkMode::Client)
    {
        return false;
    }

    const std::string_view stageView(stage && stage[0] ? stage : "");
    const std::string_view fragmentView(fragmentName && fragmentName[0] ? fragmentName : "");
    const bool actionFragmentStartedStage =
        stageView.find("ArkNpcAnimAction::OnFragmentStarted") != std::string_view::npos;
    const bool actionConstructStage =
        stageView.find("ArkNpcAnimAction::BaseConstruct") != std::string_view::npos;
    const bool nativeActionStage = actionFragmentStartedStage || actionConstructStage;
    if (actionFragmentStartedStage && action &&
        state.localNativeMannequinActions.find(action) != state.localNativeMannequinActions.end())
    {
        AppendEnemySyncTrace(
            "authority_mannequin",
            "authority_native_action_duplicate_start_ignored entity=" + std::to_string(entityId) +
                " net=" + std::to_string(state.netId) +
                " fragment=" + std::to_string(fragmentId));
        return false;
    }
    const float semanticActionNowSeconds = gEnv && gEnv->pTimer
        ? gEnv->pTimer->GetAsyncCurTime()
        : 0.0f;
    const bool freshSemanticActionStart =
        actionFragmentStartedStage &&
        state.pendingSemanticContextId != 0 &&
        state.pendingSemanticAction != nullptr &&
        state.pendingSemanticAction == action &&
        state.pendingSemanticFragmentId == fragmentId;
    const bool hardSemantic =
        (flags & (CoopProtocol::kEnemyLocomotionFlagGlooed |
            CoopProtocol::kEnemyLocomotionFlagDashing |
            CoopProtocol::kEnemyLocomotionFlagShifting |
            CoopProtocol::kEnemyLocomotionFlagMorphing |
            CoopProtocol::kEnemyLocomotionFlagLunging |
            CoopProtocol::kEnemyLocomotionFlagStunned |
            CoopProtocol::kEnemyLocomotionFlagCowering)) != 0;

    uint32_t effectivePriority = priority;
    if (effectivePriority == 0)
    {
        if (hardSemantic)
            effectivePriority = 180;
        else if (nativeActionStage &&
            (flags & (CoopProtocol::kEnemyLocomotionFlagAttacking |
                CoopProtocol::kEnemyLocomotionFlagHitReacting |
                CoopProtocol::kEnemyLocomotionFlagDashing |
                CoopProtocol::kEnemyLocomotionFlagShifting |
                CoopProtocol::kEnemyLocomotionFlagMorphing |
                CoopProtocol::kEnemyLocomotionFlagLunging)) != 0)
        {
            effectivePriority = 150;
        }
        else if (stageView.find("ArkNpcAbility::Perform") != std::string_view::npos ||
            stageView.find("TryPerformAbilityContext") != std::string_view::npos ||
            stageView.find("TryEvaluateAndPerformAbilityContext") != std::string_view::npos ||
            stageView.find("OnUsePower") != std::string_view::npos ||
            fragmentView.find("PsiAttack") != std::string_view::npos)
        {
            effectivePriority = 140;
        }
        else if (nativeActionStage && optionIdx >= 0)
        {
            effectivePriority = 110;
        }
        else if ((flags & CoopProtocol::kEnemyLocomotionFlagAttacking) != 0)
        {
            effectivePriority = 70;
        }
        else if ((flags & (CoopProtocol::kEnemyLocomotionFlagWalking |
            CoopProtocol::kEnemyLocomotionFlagRunning |
            CoopProtocol::kEnemyLocomotionFlagMannequinDriven)) != 0)
        {
            effectivePriority = 50;
        }
        else
        {
            effectivePriority = 40;
        }
    }

    if (!nativeActionStage &&
        state.localNativeMannequinStateSeconds > 0.0f &&
        state.localNativeMannequinResolved &&
        !hardSemantic)
    {
        m_lastEnemyMannequinStateEvent =
            "authority_mannequin_semantic_suppressed entity=" + std::to_string(entityId) +
            " net=" + std::to_string(state.netId) +
            " fragment=" + std::to_string(fragmentId) +
            " flags=" + std::to_string(flags) +
            " nativeSeconds=" + std::to_string(state.localNativeMannequinStateSeconds) +
            " name=" + (fragmentName && fragmentName[0] ? std::string(fragmentName) : std::string("-")) +
            " stage=" + (stage && stage[0] ? std::string(stage) : std::string("-"));
        AppendEnemySyncTrace("authority_mannequin", m_lastEnemyMannequinStateEvent);
        return false;
    }

    if (state.localMannequinStateSeconds > 0.0f &&
        state.localMannequinPriority > effectivePriority &&
        !actionFragmentStartedStage &&
        !freshSemanticActionStart &&
        !hardSemantic)
    {
        m_lastEnemyMannequinStateEvent =
            "authority_mannequin_priority_suppressed entity=" + std::to_string(entityId) +
            " net=" + std::to_string(state.netId) +
            " fragment=" + std::to_string(fragmentId) +
            " flags=" + std::to_string(flags) +
            " attack=" + std::to_string(attackKind) +
            " priority=" + std::to_string(effectivePriority) +
            " activePriority=" + std::to_string(state.localMannequinPriority) +
            " activeFragment=" + std::to_string(state.localMannequinFragmentId) +
            " activeFlags=" + std::to_string(state.localMannequinFlags) +
            " activeAttack=" + std::to_string(state.localMannequinAttackKind) +
            " activeSeconds=" + std::to_string(state.localMannequinStateSeconds) +
            " name=" + (fragmentName && fragmentName[0] ? std::string(fragmentName) : std::string("-")) +
            " stage=" + (stage && stage[0] ? std::string(stage) : std::string("-"));
        AppendEnemySyncTrace("authority_mannequin", m_lastEnemyMannequinStateEvent);
        return false;
    }

    const uint32_t movementFlags = CoopEnemyControlPolicy::MovementFlags();
    const uint32_t hardActionFlags =
        CoopProtocol::kEnemyLocomotionFlagGlooed |
        CoopProtocol::kEnemyLocomotionFlagStunned |
        CoopProtocol::kEnemyLocomotionFlagCowering |
        CoopProtocol::kEnemyLocomotionFlagHitReacting;
    const uint32_t actionMixFlags =
        CoopProtocol::kEnemyLocomotionFlagAttacking |
        CoopProtocol::kEnemyLocomotionFlagHitReacting |
        CoopProtocol::kEnemyLocomotionFlagMannequinDriven;
    uint32_t mixedFlags = flags;
    if ((mixedFlags & actionMixFlags) != 0 &&
        (mixedFlags & hardActionFlags) == 0)
    {
        const uint32_t explicitBurstMovementFlags =
            mixedFlags & CoopEnemyControlPolicy::BurstMovementFlags();
        mixedFlags &= ~movementFlags;

        const uint32_t nativeMovementFlags = state.localNativeLocomotionFlags & movementFlags;
        const uint32_t nativeBurstMovementFlags = nativeMovementFlags & CoopEnemyControlPolicy::BurstMovementFlags();
        const uint32_t authorityBurstMovementFlags =
            explicitBurstMovementFlags |
            (state.localNativeMovementSeconds > 0.0f ? nativeBurstMovementFlags : 0u);
        if (authorityBurstMovementFlags != 0)
            mixedFlags |= authorityBurstMovementFlags | CoopProtocol::kEnemyLocomotionFlagInCombat;
        else if ((mixedFlags & CoopProtocol::kEnemyLocomotionFlagAttacking) != 0)
            mixedFlags |= CoopProtocol::kEnemyLocomotionFlagInCombat;
    }

    uint16_t resolvedOrdinal = CoopProtocol::kInvalidMannequinOrdinal;
    std::string resolvedOrdinalDetail;
    const bool nativeRandomOption = nativeActionStage && optionIdx == -2;
    if (optionIdx >= 0)
    {
        resolvedOrdinal = static_cast<uint16_t>(std::min(optionIdx, 0xfffe));
    }
    else if (!nativeActionStage && fragmentId >= 0)
    {
        const uint32_t localFlags = mixedFlags & (
            CoopProtocol::kEnemyLocomotionFlagAttacking |
            CoopProtocol::kEnemyLocomotionFlagHitReacting |
            CoopProtocol::kEnemyLocomotionFlagTurning |
            CoopProtocol::kEnemyLocomotionFlagMannequinDriven);
        const int fallbackOrdinal = ResolveNpcMannequinOrdinalForRuntime(
            entity,
            fragmentId,
            mixedFlags,
            localFlags,
            -1,
            resolvedOrdinalDetail);
        if (fallbackOrdinal >= 0)
            resolvedOrdinal = static_cast<uint16_t>(std::min(fallbackOrdinal, 0xfffe));
    }

    const bool unresolvedSemanticAction =
        !nativeActionStage &&
        resolvedOrdinal == CoopProtocol::kInvalidMannequinOrdinal &&
        (mixedFlags & CoopProtocol::kEnemyLocomotionFlagAttacking) != 0 &&
        (mixedFlags & hardActionFlags) == 0;
    if (unresolvedSemanticAction)
    {
        m_lastEnemyMannequinStateEvent =
            "authority_mannequin_semantic_unresolved_ordinal_skipped entity=" + std::to_string(entityId) +
            " net=" + std::to_string(state.netId) +
            " fragment=" + std::to_string(fragmentId) +
            " flags=" + std::to_string(mixedFlags) +
            " attack=" + std::to_string(attackKind) +
            " priority=" + std::to_string(effectivePriority) +
            " name=" + (fragmentName && fragmentName[0] ? std::string(fragmentName) : std::string("-")) +
            " stage=" + (stage && stage[0] ? std::string(stage) : std::string("-")) +
            " ordinalDetail=" + (resolvedOrdinalDetail.empty() ? std::string("-") : resolvedOrdinalDetail);
        AppendEnemySyncTrace("authority_mannequin", m_lastEnemyMannequinStateEvent);
        return false;
    }

    const bool carryUntilReplaced = nativeActionStage && IsEnemyMannequinCarryFragment(fragmentName);
    const bool duplicatePassiveConstruct =
        actionConstructStage &&
        CoopEnemyControlPolicy::IsPassiveMannequinFlags(mixedFlags) &&
        state.localMannequinFragmentId == fragmentId &&
        state.localMannequinFlags == mixedFlags &&
        state.localMannequinAttackKind == attackKind &&
        state.localMannequinStateSeconds > 0.0f;
    if (duplicatePassiveConstruct)
    {
        const float refreshSeconds = carryUntilReplaced ? 3.50f : 0.80f;
        state.localMannequinStateSeconds = std::max(state.localMannequinStateSeconds, refreshSeconds);
        if (carryUntilReplaced)
            state.localMannequinCarryUntilReplaced = true;
        m_lastEnemyMannequinStateEvent =
            "authority_mannequin_construct_duplicate_refresh entity=" + std::to_string(entityId) +
            " net=" + std::to_string(state.netId) +
            " fragment=" + std::to_string(fragmentId) +
            " seq=" + std::to_string(state.localMannequinSequence) +
            " flags=" + std::to_string(mixedFlags) +
            " name=" + (fragmentName && fragmentName[0] ? std::string(fragmentName) : std::string("-")) +
            " stage=" + (stage && stage[0] ? std::string(stage) : std::string("-"));
        AppendEnemySyncTrace("authority_mannequin", m_lastEnemyMannequinStateEvent);
        return false;
    }

    state.localMannequinFlags = mixedFlags;
    state.localMannequinAttackKind = attackKind;
    state.localMannequinFragmentId = fragmentId;
    state.localMannequinPriority = effectivePriority;
    state.localMannequinOrdinal = resolvedOrdinal;
    state.localMannequinRandomOption = nativeRandomOption;
    state.localMannequinTagState.fill(0);
    state.localMannequinTagStateValid = false;
    if (action && CoopRuntimeGuards::IsReadableRuntimePointer(
            reinterpret_cast<const uint8_t*>(action) + kIActionFragTagsOffset,
            kMannequinTagStateByteCount))
    {
        std::memcpy(
            state.localMannequinTagState.data(),
            reinterpret_cast<const uint8_t*>(action) + kIActionFragTagsOffset,
            state.localMannequinTagState.size());
        state.localMannequinTagStateValid = true;
    }
    state.localMannequinCarryUntilReplaced = carryUntilReplaced;
    CoopSerialSequence::Advance(state.localMannequinSequence);
    if (actionFragmentStartedStage && action)
    {
        EnemyAuthorityState::LocalNativeMannequinAction nativeAction;
        nativeAction.sequence = state.localMannequinSequence;
        nativeAction.actionFlags = mixedFlags;
        nativeAction.fragmentId = fragmentId;
        nativeAction.priority = static_cast<int>(effectivePriority);
        nativeAction.optionIndex = nativeRandomOption
            ? 0xfffffffeu
            : (state.localMannequinOrdinal != CoopProtocol::kInvalidMannequinOrdinal
                ? static_cast<uint32_t>(state.localMannequinOrdinal)
                : 0xffffffffu);
        nativeAction.tagState = state.localMannequinTagState;
        nativeAction.tagStateValid = state.localMannequinTagStateValid;
        state.localNativeMannequinActions[action] = nativeAction;
        QueueLocalEnemyMannequinActionEventForHook(
            state,
            nativeAction,
            CoopProtocol::EnemyMannequinActionCommand::Start,
            fragmentName && fragmentName[0] ? fragmentName : "native action start");
    }
    if (actionFragmentStartedStage)
    {
        if (freshSemanticActionStart)
        {
            state.localSemanticContextId = state.pendingSemanticContextId;
            state.localSemanticSequence =
                CoopSerialSequence::Advance(state.localAuthoritySemanticSequence);
            state.localSemanticBoundAtSeconds = semanticActionNowSeconds;
            state.localSemanticVariant = state.pendingSemanticVariant;
            state.localSemanticLastContextId = state.localSemanticContextId;
            state.localSemanticLastSequence = state.localSemanticSequence;
            state.localSemanticLastVariant = state.localSemanticVariant;
            state.localSemanticLastFragmentId = fragmentId;
            AppendEnemySyncTrace(
                "semantic_context",
                "authority_semantic_context_bound net=" + std::to_string(state.netId) +
                    " entity=" + std::to_string(entityId) +
                    " context=" + std::to_string(state.localSemanticContextId) +
                    " variant=" + std::to_string(state.localSemanticVariant) +
                    " target=" + std::to_string(state.pendingSemanticTargetEntityId) +
                    " semanticSeq=" + std::to_string(state.localSemanticSequence) +
                    " mannequinSeq=" + std::to_string(state.localMannequinSequence) +
                    " fragment=" + std::to_string(fragmentId));
        }
        else
        {
            state.localSemanticContextId = 0;
            state.localSemanticSequence = 0;
            state.localSemanticBoundAtSeconds = -1000.0f;
            state.localSemanticVariant = 0;
        }
        state.pendingSemanticContextId = 0;
        state.pendingSemanticTargetEntityId = INVALID_ENTITYID;
        state.pendingSemanticObservedAtSeconds = -1000.0f;
        state.pendingSemanticVariant = 0;
        state.pendingSemanticAction = nullptr;
        state.pendingSemanticFragmentId = -1;
    }
    const bool attackLike =
        attackKind != 0 ||
        (mixedFlags & (CoopProtocol::kEnemyLocomotionFlagAttacking |
            CoopProtocol::kEnemyLocomotionFlagHitReacting)) != 0 ||
        effectivePriority >= 140;
    const bool hardState =
        (mixedFlags & (CoopProtocol::kEnemyLocomotionFlagGlooed |
            CoopProtocol::kEnemyLocomotionFlagStunned |
            CoopProtocol::kEnemyLocomotionFlagCowering)) != 0;
    const bool burstMovement =
        (mixedFlags & (CoopProtocol::kEnemyLocomotionFlagDashing |
            CoopProtocol::kEnemyLocomotionFlagShifting |
            CoopProtocol::kEnemyLocomotionFlagMorphing |
            CoopProtocol::kEnemyLocomotionFlagLunging)) != 0;
    const float holdSeconds = state.localMannequinCarryUntilReplaced
        ? 3.50f
        : (hardState
        ? 1.25f
        : (attackLike
            ? 1.10f
            : (burstMovement
                ? 0.85f
                : (nativeActionStage ? 0.80f : 0.45f))));
    state.localMannequinStateSeconds = std::max(state.localMannequinStateSeconds, holdSeconds);
    if (nativeActionStage)
    {
        state.localNativeMannequinStateSeconds = std::max(state.localNativeMannequinStateSeconds, holdSeconds);
        state.localNativeMannequinResolved = optionIdx >= 0;
    }
    else
    {
        state.localNativeMannequinResolved = false;
    }

    ++m_enemyMannequinAuthorityObservations;
    m_lastEnemyMannequinStateEvent =
        "authority_mannequin entity=" + std::to_string(entityId) +
        " net=" + std::to_string(state.netId) +
        " fragment=" + std::to_string(fragmentId) +
        " ordinal=" + std::to_string(state.localMannequinOrdinal) +
        " seq=" + std::to_string(state.localMannequinSequence) +
        " flags=" + std::to_string(mixedFlags) +
        (mixedFlags != flags ? " sourceFlags=" + std::to_string(flags) : "") +
        " attack=" + std::to_string(attackKind) +
        " priority=" + std::to_string(effectivePriority) +
        (state.localMannequinCarryUntilReplaced ? " carry=1" : "") +
        (optionIdx >= 0 ? " ordinalSource=native" : (resolvedOrdinal != CoopProtocol::kInvalidMannequinOrdinal ? " ordinalSource=resolver" : " ordinalSource=missing")) +
        (!resolvedOrdinalDetail.empty() ? " ordinalDetail=" + AnimationStatusToken(resolvedOrdinalDetail) : std::string()) +
        " name=" + (fragmentName && fragmentName[0] ? std::string(fragmentName) : std::string("-")) +
        " stage=" + (stage && stage[0] ? std::string(stage) : std::string("-"));
    AppendEnemySyncTrace("authority_mannequin", m_lastEnemyMannequinStateEvent);

    if (actionFragmentStartedStage)
    {
        const uint16_t abilityFxKind = ClassifyEnemyAbilityFxKindForFragment(ToLowerAsciiAnimation(std::string(fragmentView)));
        if (attackKind != 0)
        {
            m_coverageDiscovery.RecordNpcAbility(
                abilityFxKind != CoopProtocol::kEnemyAbilityFxNone || attackKind != kEnemyMannequinAttackGeneric,
                fragmentName && fragmentName[0] ? fragmentName : "unknown_enemy_action");
        }
        if (abilityFxKind != CoopProtocol::kEnemyAbilityFxNone)
        {
            QueueLocalEnemyAbilityFxEventForHook(
                state,
                *entity,
                abilityFxKind,
                fragmentId,
                state.localMannequinOrdinal,
                state.localMannequinSequence,
                entity->GetWorldPos(),
                entity->GetWorldRotation().GetColumn1(),
                fragmentName && fragmentName[0] ? fragmentName : "native enemy ability");
        }
    }

    if (burstMovement)
    {
        const bool finalShiftStage =
            stageView.find("ShiftEnd:after") != std::string_view::npos;
        bool sentFinalShiftState = false;
        if (finalShiftStage)
        {
            if (m_networkMode == CoopNetworkMode::Host)
            {
                sentFinalShiftState =
                    SendEnemyStateNow(state, "enemy final shift mannequin state send failed");
            }
            else if (m_networkMode == CoopNetworkMode::Client)
            {
                sentFinalShiftState =
                    SendClientEnemyAuthorityStateNow(
                        state,
                        CoopProtocol::kEnemyStateSourceFlagAuthorityClaim |
                            CoopProtocol::kEnemyStateSourceFlagAuthoritySnapshot,
                        "client final shift mannequin state send failed");
                if (sentFinalShiftState)
                    state.localBurstMannequinSendPending = false;
            }

            AppendEnemySyncTrace(
                "authority_mannequin",
                "authority_mannequin_burst_final_shift_send entity=" + std::to_string(entityId) +
                    " net=" + std::to_string(state.netId) +
                    " fragment=" + std::to_string(fragmentId) +
                    " seq=" + std::to_string(state.localMannequinSequence) +
                    " sent=" + std::to_string(sentFinalShiftState ? 1 : 0));
        }

        if (!sentFinalShiftState)
            state.localBurstMannequinSendPending = true;

        AppendEnemySyncTrace(
            "authority_mannequin",
            (sentFinalShiftState
                ? "authority_mannequin_burst_final_shift_sent entity="
                : "authority_mannequin_burst_deferred_send entity=") +
                std::to_string(entityId) +
                    " net=" + std::to_string(state.netId) +
                    " fragment=" + std::to_string(fragmentId) +
                    " seq=" + std::to_string(state.localMannequinSequence));
    }
    return true;
}

bool ModMain::QueueLocalEnemyMannequinActionEventForHook(
    EnemyAuthorityState& state,
    const EnemyAuthorityState::LocalNativeMannequinAction& action,
    CoopProtocol::EnemyMannequinActionCommand command,
    const char* reason)
{
    if (m_networkMode == CoopNetworkMode::Off ||
        !IsSessionGameplayReady() ||
        state.netId == 0 ||
        action.sequence == 0 ||
        state.authorityOwnerAccountToken != GetLocalAccountToken())
    {
        return false;
    }

    CoopProtocol::EnemyMannequinActionPacket packet = {};
    packet.sequence = CoopSerialSequence::Advance(m_enemyMannequinActionEventSequence);
    packet.worldEpoch = m_localWorldEpoch;
    packet.levelId = m_localLevelId;
    packet.enemyNetId = state.netId;
    packet.enemyArchetypeId = state.archetypeId;
    packet.authorityOwnerAccountToken = state.authorityOwnerAccountToken;
    packet.authorityEpoch = state.authorityEpoch;
    packet.actionSequence = action.sequence;
    packet.command = static_cast<uint16_t>(command);
    packet.actionFlags = action.actionFlags;
    packet.fragmentId = action.fragmentId;
    packet.priority = action.priority;
    packet.optionIndex = action.optionIndex;
    if (action.tagStateValid)
    {
        packet.flags |= CoopProtocol::kEnemyMannequinActionFlagTagStateValid;
        std::copy(action.tagState.begin(), action.tagState.end(), std::begin(packet.tagState));
    }

    bool sent = false;
    if (m_networkMode == CoopNetworkMode::Host)
    {
        sent = SendEnemyMannequinActionTo(
            packet,
            m_remoteAddress,
            m_remotePort,
            "native enemy action edge send failed");
    }
    else if (m_networkMode == CoopNetworkMode::Client &&
        m_hasRemoteEndpoint && m_remoteAddress != 0 && m_remotePort != 0)
    {
        sent = SendEnemyMannequinActionTo(
            packet,
            m_remoteAddress,
            m_remotePort,
            "client native enemy action edge send failed");
    }

    AppendEnemySyncTrace(
        "native_action_edge",
        std::string(sent ? "sent" : "suppressed") +
            " native_action_edge net=" + std::to_string(state.netId) +
            " actionSeq=" + std::to_string(action.sequence) +
            " eventSeq=" + std::to_string(packet.sequence) +
            " command=" + std::to_string(packet.command) +
            " fragment=" + std::to_string(action.fragmentId) +
            " option=" + std::to_string(action.optionIndex) +
            " priority=" + std::to_string(action.priority) +
            " tagState=" + std::to_string(action.tagStateValid ? 1 : 0) +
            " owner=" + std::to_string(state.authorityOwnerAccountToken) +
            " epoch=" + std::to_string(state.authorityEpoch) +
            " reason=" + (reason && reason[0] ? std::string(reason) : std::string("-")));
    return sent;
}

void ModMain::StartAnimationTestProxyTrace(const std::string& label, float seconds)
{
    m_animationTestTraceActive = seconds > 0.0f;
    m_animationTestTraceRemaining = std::min(std::max(seconds, 0.0f), 6.0f);
    m_animationTestTraceElapsed = 0.0f;
    m_animationTestTraceAccumulator = m_animationTestTraceInterval;
    m_animationTestTraceSamples = 0;
    m_animationTestTraceLabel = label.empty() ? std::string("-") : label;
    m_animationTestTraceSummary =
        "anim_trace_started label=" + m_animationTestTraceLabel +
        " seconds=" + std::to_string(m_animationTestTraceRemaining);
    m_lastAnimationTestEvent = m_animationTestTraceSummary;
    CoopRuntimeLog::Write(m_animationTestTraceSummary);
}

void ModMain::TickAnimationTestProxyTrace(float frameTime)
{
    if (!m_animationTestTraceActive)
        return;

    const float dt = std::min(std::max(0.0f, frameTime), 0.1f);
    m_animationTestTraceRemaining = std::max(0.0f, m_animationTestTraceRemaining - dt);
    m_animationTestTraceElapsed += dt;
    m_animationTestTraceAccumulator += dt;

    if (m_animationTestTraceSamples == 0 || m_animationTestTraceAccumulator >= m_animationTestTraceInterval)
    {
        m_animationTestTraceAccumulator = 0.0f;
        std::string sample;
        if (SampleAnimationTestProxyState(m_animationTestTraceElapsed, sample))
        {
            ++m_animationTestTraceSamples;
            m_animationTestTraceSummary = sample;
            CoopRuntimeLog::Write(sample);
        }
        else
        {
            m_animationTestTraceSummary = sample;
            m_lastAnimationTestEvent = sample;
            m_animationTestTraceActive = false;
            CoopRuntimeLog::Write(sample);
            return;
        }
    }

    if (m_animationTestTraceRemaining <= 0.0f)
    {
        m_animationTestTraceActive = false;
        m_lastAnimationTestEvent =
            "anim_trace_done label=" + m_animationTestTraceLabel +
            " samples=" + std::to_string(m_animationTestTraceSamples) +
            " last=" + m_animationTestTraceSummary;
        CoopRuntimeLog::Write(m_lastAnimationTestEvent);
    }
}

bool ModMain::StartAnimationTestProxyPoseHold(
    const std::string& poseName,
    const std::string& animationName,
    float normalizedTime,
    int slot,
    int layer,
    float blend,
    std::string& detail)
{
    IEntity* entity = GetAnimationTestProxyEntity();
    IScriptTable* table = entity ? entity->GetScriptTable() : nullptr;
    if (!entity || !table)
    {
        detail = "pose_hold_failed_no_anim_proxy pose=" + poseName;
        m_lastAnimationTestEvent = detail;
        ++m_animationTestProxyFailures;
        return false;
    }
    if (animationName.empty())
    {
        detail = "pose_hold_failed_empty_anim pose=" + poseName;
        m_lastAnimationTestEvent = detail;
        ++m_animationTestProxyFailures;
        return false;
    }

    normalizedTime = std::clamp(normalizedTime, 0.0f, 1.0f);
    blend = std::max(0.0f, blend);

    ClearAnimationTestProxyPoseHold("pose_hold_restart");

    bool stopOk = false;
    std::string guardReason;
    TryGuardedCall(
        "anim pose hold StopAnimation",
        [table, slot, layer]() { return Script::CallMethod(table, "StopAnimation", slot, layer); },
        stopOk,
        &guardReason);

    bool resetOk = false;
    TryGuardedCall(
        "anim pose hold ResetAnimation",
        [table, slot, layer]() { return Script::CallMethod(table, "ResetAnimation", slot, layer); },
        resetOk,
        &guardReason);

    ScriptAnyValue playReturn;
    bool playOk = false;
    const bool playGuarded = TryGuardedCall(
        "anim pose hold StartAnimation",
        [table, slot, &animationName, layer, blend, &playReturn]()
        {
            return CallAnimProxyStartAnimation(table, slot, animationName, layer, blend, 0.0f, playReturn);
        },
        playOk,
        &guardReason);
    playOk = playGuarded && playOk && ScriptAnyTruthyOrVoid(playReturn);

    bool timeOk = false;
    const bool timeGuarded = TryGuardedCall(
        "anim pose hold SetAnimationTime",
        [table, slot, layer, normalizedTime]()
        {
            return Script::CallMethod(table, "SetAnimationTime", slot, layer, normalizedTime);
        },
        timeOk,
        &guardReason);

    int queueDepth = -1;
    ReadAnimProxyQueueDepth(table, slot, layer, queueDepth);

    const bool ok = playOk && timeGuarded && timeOk;
    if (ok)
    {
        m_animationTestPoseHoldActive = true;
        m_animationTestPoseHoldEntityId = entity->GetId();
        m_animationTestPoseHoldName = poseName.empty() ? std::string("custom") : poseName;
        m_animationTestPoseHoldClip = animationName;
        m_animationTestPoseHoldSlot = slot;
        m_animationTestPoseHoldLayer = layer;
        m_animationTestPoseHoldTime = normalizedTime;
        m_animationTestPoseHoldBlend = blend;
        m_animationTestPoseHoldAccumulator = m_animationTestPoseHoldInterval;
        m_animationTestPoseHoldTicks = 0;
    }

    detail =
        std::string(ok ? "pose_hold_ok" : "pose_hold_failed") +
        " pose=" + (poseName.empty() ? std::string("custom") : poseName) +
        " entity=" + std::to_string(entity->GetId()) +
        " anim=" + animationName +
        " slot=" + std::to_string(slot) +
        " layer=" + std::to_string(layer) +
        " time=" + std::to_string(normalizedTime) +
        " blend=" + std::to_string(blend) +
        " queue=" + std::to_string(queueDepth) +
        " stop=" + std::to_string(stopOk ? 1 : 0) +
        " reset=" + std::to_string(resetOk ? 1 : 0) +
        " play=" + std::to_string(playOk ? 1 : 0) +
        " ret=" + ScriptAnyTypeName(playReturn.type) + ":" + ScriptAnyValueText(playReturn) +
        " setTime=" + std::to_string((timeGuarded && timeOk) ? 1 : 0) +
        (guardReason.empty() ? std::string() : " reason=" + guardReason);
    m_animationTestPoseHoldLast = detail;
    m_lastAnimationTestEvent = detail;
    if (ok)
        ++m_animationTestProxyPlays;
    else
        ++m_animationTestProxyFailures;
    return ok;
}

void ModMain::ClearAnimationTestProxyPoseHold(const char* reason)
{
    if (!m_animationTestPoseHoldActive &&
        m_animationTestPoseHoldEntityId == INVALID_ENTITYID &&
        m_animationTestPoseHoldLast == "-")
    {
        return;
    }

    m_animationTestPoseHoldActive = false;
    m_animationTestPoseHoldEntityId = INVALID_ENTITYID;
    m_animationTestPoseHoldAccumulator = 0.0f;
    m_animationTestPoseHoldLast =
        "pose_hold_cleared reason=" + std::string(reason && reason[0] ? reason : "-") +
        " pose=" + m_animationTestPoseHoldName +
        " clip=" + m_animationTestPoseHoldClip +
        " ticks=" + std::to_string(m_animationTestPoseHoldTicks);
}

void ModMain::TickAnimationTestProxyPoseHold(float frameTime)
{
    if (!m_animationTestPoseHoldActive)
        return;

    IEntity* entity = GetAnimationTestProxyEntity();
    IScriptTable* table = entity ? entity->GetScriptTable() : nullptr;
    if (!entity || !table || entity->GetId() != m_animationTestPoseHoldEntityId)
    {
        ClearAnimationTestProxyPoseHold("pose_entity_missing");
        return;
    }

    const float dt = std::min(std::max(frameTime, 0.0f), 0.1f);
    m_animationTestPoseHoldAccumulator += dt;
    if (m_animationTestPoseHoldAccumulator < m_animationTestPoseHoldInterval)
        return;

    m_animationTestPoseHoldAccumulator = 0.0f;
    bool timeOk = false;
    std::string guardReason;
    const bool guarded = TryGuardedCall(
        "anim pose hold tick SetAnimationTime",
        [table, this]()
        {
            return Script::CallMethod(
                table,
                "SetAnimationTime",
                m_animationTestPoseHoldSlot,
                m_animationTestPoseHoldLayer,
                m_animationTestPoseHoldTime);
        },
        timeOk,
        &guardReason);

    if (!guarded || !timeOk)
    {
        m_animationTestPoseHoldLast =
            "pose_hold_tick_failed pose=" + m_animationTestPoseHoldName +
            " clip=" + m_animationTestPoseHoldClip +
            " ticks=" + std::to_string(m_animationTestPoseHoldTicks) +
            (guardReason.empty() ? std::string() : " reason=" + guardReason);
        m_animationTestPoseHoldActive = false;
        ++m_animationTestProxyFailures;
        return;
    }

    ++m_animationTestPoseHoldTicks;
    if ((m_animationTestPoseHoldTicks % 20) == 0)
    {
        m_animationTestPoseHoldLast =
            "pose_hold_tick pose=" + m_animationTestPoseHoldName +
            " clip=" + m_animationTestPoseHoldClip +
            " time=" + std::to_string(m_animationTestPoseHoldTime) +
            " ticks=" + std::to_string(m_animationTestPoseHoldTicks);
    }
}

bool ModMain::ApplyAdditionalRemoteProxyAnimation(
    IEntity& entity,
    const std::string& animationName,
    float normalizedTime,
    bool restart,
    float blend,
    std::string& detail)
{
    IScriptTable* table = entity.GetScriptTable();
    if (!table || animationName.empty())
    {
        detail = "additional_remote_animation_missing_script_or_clip";
        return false;
    }

    blend = std::max(0.0f, blend);
    std::string guardReason;
    bool playOk = true;
    if (restart)
    {
        bool ignored = false;
        TryGuardedCall(
            "additional remote animation StopAnimation",
            [table]() { return Script::CallMethod(table, "StopAnimation", 0, 0); },
            ignored,
            &guardReason);
        TryGuardedCall(
            "additional remote animation ResetAnimation",
            [table]() { return Script::CallMethod(table, "ResetAnimation", 0, 0); },
            ignored,
            &guardReason);

        ScriptAnyValue playReturn;
        const bool guarded = TryGuardedCall(
            "additional remote animation StartAnimation",
            [table, &animationName, blend, &playReturn]()
            {
                return CallAnimProxyStartAnimation(table, 0, animationName, 0, blend, 0.0f, playReturn);
            },
            playOk,
            &guardReason);
        playOk = guarded && playOk && ScriptAnyTruthyOrVoid(playReturn);
    }

    normalizedTime = std::clamp(normalizedTime, 0.0f, 1.0f);
    bool timeOk = false;
    const bool timeGuarded = TryGuardedCall(
        "additional remote animation SetAnimationTime",
        [table, normalizedTime]()
        {
            return Script::CallMethod(table, "SetAnimationTime", 0, 0, normalizedTime);
        },
        timeOk,
        &guardReason);

    const bool ok = playOk && timeGuarded && timeOk;
    detail =
        std::string(ok ? "additional_remote_animation_ok" : "additional_remote_animation_failed") +
        " entity=" + std::to_string(entity.GetId()) +
        " clip=" + animationName +
        " time=" + std::to_string(normalizedTime) +
        " restart=" + std::to_string(restart ? 1 : 0) +
        " blend=" + std::to_string(blend) +
        (guardReason.empty() ? std::string() : " reason=" + guardReason);
    return ok;
}

bool ModMain::ApplyRemotePlayerMotionParams(
    IEntity& entity,
    float travelSpeed,
    float travelAngle,
    float frameTime,
    std::string* reason)
{
    ICharacterInstance* character = nullptr;
    if (!TryGuardedCall(
            "remote player motion GetCharacter",
            [&entity]() { return entity.GetCharacter(0); },
            character,
            reason) ||
        !character ||
        !CoopRuntimeGuards::IsLikelyRuntimeCppObject(character, sizeof(void*)))
    {
        if (reason && reason->empty())
            *reason = "missing_character";
        return false;
    }

    void** characterVtable = *reinterpret_cast<void***>(character);
    if (!CoopRuntimeGuards::IsReadableRuntimePointer(
            characterVtable,
            (kCharacterGetSkeletonAnimVtableIndex + 1) * sizeof(void*)) ||
        !CoopRuntimeGuards::IsExecutableRuntimePointer(
            characterVtable[kCharacterGetSkeletonAnimVtableIndex]))
    {
        if (reason)
            *reason = "invalid_character_vtable";
        return false;
    }

    using GetSkeletonAnimFn = void* (*)(void*);
    void* skeletonAnim = nullptr;
    if (!TryGuardedCall(
            "remote player motion GetISkeletonAnim",
            [character, characterVtable]()
            {
                return reinterpret_cast<GetSkeletonAnimFn>(
                    characterVtable[kCharacterGetSkeletonAnimVtableIndex])(character);
            },
            skeletonAnim,
            reason) ||
        !skeletonAnim ||
        !CoopRuntimeGuards::IsLikelyRuntimeCppObject(skeletonAnim, sizeof(void*)))
    {
        if (reason && reason->empty())
            *reason = "missing_skeleton_anim";
        return false;
    }

    void** skeletonVtable = *reinterpret_cast<void***>(skeletonAnim);
    if (!CoopRuntimeGuards::IsReadableRuntimePointer(
            skeletonVtable,
            (kSkeletonSetDesiredMotionParamVtableIndex + 1) * sizeof(void*)) ||
        !CoopRuntimeGuards::IsExecutableRuntimePointer(
            skeletonVtable[kSkeletonSetDesiredMotionParamVtableIndex]))
    {
        if (reason)
            *reason = "invalid_skeleton_vtable";
        return false;
    }

    using SetDesiredMotionParamFn = void (*)(void*, int, float, float);
    const auto setDesiredMotionParam = reinterpret_cast<SetDesiredMotionParamFn>(
        skeletonVtable[kSkeletonSetDesiredMotionParamVtableIndex]);
    return TryGuardedVoidCall(
        "remote player motion SetDesiredMotionParams",
        [skeletonAnim, setDesiredMotionParam, travelSpeed, travelAngle, frameTime]()
        {
            setDesiredMotionParam(
                skeletonAnim,
                kMotionParamTravelSpeed,
                travelSpeed,
                frameTime);
            setDesiredMotionParam(
                skeletonAnim,
                kMotionParamTravelAngle,
                travelAngle,
                frameTime);
        },
        reason);
}

const char* ModMain::SelectRemotePlayerZeroGShiftClip(float travelAngle, bool moving) const
{
    (void)travelAngle;
    (void)moving;

    // All four authored shift clips are non-looping transitions rather than
    // true Zero-G locomotion. This single hand-tuned frame keeps both hands in
    // the production weapon pose while root translation supplies direction.
    return "ShiftPose_ZeroG_Forward_A";
}

void ModMain::ApplyRemotePlayerZeroGPresentation(
    IEntity& entity,
    const Vec3& remoteVelocity,
    const Quat& remoteRotation,
    bool moving)
{
    if (!moving)
    {
        m_remotePoseZeroGTravelSpeed = 0.0f;
    }
    else
    {
        const float remoteSpeed = remoteVelocity.GetLength();
        if (remoteSpeed > 0.20f)
        {
            m_remotePoseZeroGTravelSpeed = std::min(remoteSpeed, 10.0f);
            const Vec3 localVelocity = remoteRotation.GetInverted() * remoteVelocity;
            if (localVelocity.x * localVelocity.x + localVelocity.y * localVelocity.y > 0.01f)
                m_remotePoseZeroGTravelAngle = std::atan2(localVelocity.x, localVelocity.y);
        }
    }

    // ArkHuman proxies use ai_human_database.adb. Prey's authored player
    // Zero-G loop/blendspace names live in arkplayerdatabase3p.adb and direct
    // StartAnimation rejects them on this proxy. The human database does
    // provide authored Zero-G shift poses. Hold the visually validated forward
    // frame for every direction instead of looping or switching between
    // non-looping transitions.
    const char* clip = SelectRemotePlayerZeroGShiftClip(
        m_remotePoseZeroGTravelAngle,
        moving);
    std::string detail;
    const bool animationStarted = StartRemoteProxyPoseHold(
        moving ? "zero_g_direction_pose" : "zero_g_idle_pose",
        clip,
        0.10f,
        0,
        0,
        0.06f,
        detail);
    std::string motionReason;
    const bool motionApplied = ApplyRemotePlayerMotionParams(
        entity,
        moving ? m_remotePoseZeroGTravelSpeed : 0.0f,
        m_remotePoseZeroGTravelAngle,
        1.0f / 30.0f,
        &motionReason);
    detail +=
        " animation=" + std::to_string(animationStarted ? 1 : 0) +
        " speed=" + std::to_string(moving ? m_remotePoseZeroGTravelSpeed : 0.0f) +
        " angle=" + std::to_string(m_remotePoseZeroGTravelAngle) +
        " motionParams=" + std::to_string(motionApplied ? 1 : 0) +
        (motionReason.empty() ? std::string() : " motionReason=" + motionReason);
    m_remoteProxyPoseHoldLast = detail;
}

void ModMain::ApplyAdditionalRemotePlayerZeroGMotion(
    RemotePeerSession& peer,
    IEntity& entity,
    const Vec3& remoteVelocity,
    const Quat& remoteRotation,
    bool moving)
{
    if (!moving)
    {
        peer.zeroGTravelSpeed = 0.0f;
    }
    else
    {
        const float remoteSpeed = remoteVelocity.GetLength();
        if (remoteSpeed > 0.20f)
        {
            peer.zeroGTravelSpeed = std::min(remoteSpeed, 10.0f);
            const Vec3 localVelocity = remoteRotation.GetInverted() * remoteVelocity;
            if (localVelocity.x * localVelocity.x + localVelocity.y * localVelocity.y > 0.01f)
                peer.zeroGTravelAngle = std::atan2(localVelocity.x, localVelocity.y);
        }
    }

    std::string motionReason;
    if (!ApplyRemotePlayerMotionParams(
            entity,
            moving ? peer.zeroGTravelSpeed : 0.0f,
            peer.zeroGTravelAngle,
            1.0f / 30.0f,
            &motionReason) &&
        !motionReason.empty())
    {
        m_networkStatus = "additional zero-g motion params failed: " + motionReason;
    }
}

bool ModMain::UpdateLocalPlayerZeroGMoving(bool zeroG, bool rawMoving)
{
    if (!zeroG)
    {
        m_localPoseZeroGMoveHoldSeconds = 0.0f;
    }
    else if (rawMoving)
    {
        // Zero-G velocity can briefly sample as zero between thrust updates.
        // Keep the directional pose through that gap so the observer does not
        // alternate between idle and thrust on otherwise continuous travel.
        m_localPoseZeroGMoveHoldSeconds = 0.75f;
    }
    return rawMoving || (zeroG && m_localPoseZeroGMoveHoldSeconds > 0.0f);
}

bool ModMain::StartRemoteProxyPoseHold(
    const std::string& poseName,
    const std::string& animationName,
    float normalizedTime,
    int slot,
    int layer,
    float blend,
    std::string& detail)
{
    IEntity* entity = GetProxyEntity();
    IScriptTable* table = entity ? entity->GetScriptTable() : nullptr;
    if (!entity || !table)
    {
        detail = "remote_proxy_pose_failed_no_proxy pose=" + poseName;
        m_remoteProxyPoseHoldLast = detail;
        ++m_animationTestProxyFailures;
        return false;
    }
    if (animationName.empty())
    {
        detail = "remote_proxy_pose_failed_empty_anim pose=" + poseName;
        m_remoteProxyPoseHoldLast = detail;
        ++m_animationTestProxyFailures;
        return false;
    }

    normalizedTime = std::clamp(normalizedTime, 0.0f, 1.0f);
    blend = std::max(0.0f, blend);

    const std::string effectivePose = poseName.empty() ? std::string("custom") : poseName;
    if (m_remoteProxyPoseHoldActive &&
        m_remoteProxyPoseHoldEntityId == entity->GetId() &&
        m_remoteProxyPoseHoldClip == animationName &&
        m_remoteProxyPoseHoldSlot == slot &&
        m_remoteProxyPoseHoldLayer == layer &&
        std::fabs(m_remoteProxyPoseHoldTime - normalizedTime) < 0.001f &&
        std::fabs(m_remoteProxyPoseHoldBlend - blend) < 0.001f)
    {
        // Moving and idle Zero-G deliberately share one frozen surrogate
        // frame. Update its diagnostic state without restarting the authored
        // transition: Stop/Reset/Start can briefly reapply the clip root before
        // the next network transform restores the peer rotation.
        m_remoteProxyPoseHoldName = effectivePose;
        detail =
            "remote_proxy_pose_existing pose=" + effectivePose +
            " entity=" + std::to_string(entity->GetId()) +
            " anim=" + animationName +
            " time=" + std::to_string(normalizedTime) +
            " ticks=" + std::to_string(m_remoteProxyPoseHoldTicks);
        m_remoteProxyPoseHoldLast = detail;
        return true;
    }

    ClearRemoteProxyPoseHold("remote_proxy_pose_restart");

    bool stopOk = false;
    std::string guardReason;
    TryGuardedCall(
        "remote proxy pose StopAnimation",
        [table, slot, layer]() { return Script::CallMethod(table, "StopAnimation", slot, layer); },
        stopOk,
        &guardReason);

    bool resetOk = false;
    TryGuardedCall(
        "remote proxy pose ResetAnimation",
        [table, slot, layer]() { return Script::CallMethod(table, "ResetAnimation", slot, layer); },
        resetOk,
        &guardReason);

    ScriptAnyValue playReturn;
    bool playOk = false;
    const bool playGuarded = TryGuardedCall(
        "remote proxy pose StartAnimation",
        [table, slot, &animationName, layer, blend, &playReturn]()
        {
            return CallAnimProxyStartAnimation(table, slot, animationName, layer, blend, 0.0f, playReturn);
        },
        playOk,
        &guardReason);
    playOk = playGuarded && playOk && ScriptAnyTruthyOrVoid(playReturn);

    bool timeOk = false;
    const bool timeGuarded = TryGuardedCall(
        "remote proxy pose SetAnimationTime",
        [table, slot, layer, normalizedTime]()
        {
            return Script::CallMethod(table, "SetAnimationTime", slot, layer, normalizedTime);
        },
        timeOk,
        &guardReason);

    int queueDepth = -1;
    ReadAnimProxyQueueDepth(table, slot, layer, queueDepth);

    const bool ok = playOk && timeGuarded && timeOk;
    if (ok)
    {
        m_remoteProxyPoseHoldActive = true;
        m_remoteProxyPoseHoldEntityId = entity->GetId();
        m_remoteProxyPoseHoldName = effectivePose;
        m_remoteProxyPoseHoldClip = animationName;
        m_remoteProxyPoseHoldSlot = slot;
        m_remoteProxyPoseHoldLayer = layer;
        m_remoteProxyPoseHoldTime = normalizedTime;
        m_remoteProxyPoseHoldBlend = blend;
        m_remoteProxyPoseHoldAccumulator = m_remoteProxyPoseHoldInterval;
        m_remoteProxyPoseHoldTicks = 0;
        m_remoteProxyPoseHoldLoop = false;
        m_remoteProxyPoseHoldDuration = 1.0f;
    }

    detail =
        std::string(ok ? "remote_proxy_pose_ok" : "remote_proxy_pose_failed") +
        " pose=" + effectivePose +
        " entity=" + std::to_string(entity->GetId()) +
        " anim=" + animationName +
        " slot=" + std::to_string(slot) +
        " layer=" + std::to_string(layer) +
        " time=" + std::to_string(normalizedTime) +
        " blend=" + std::to_string(blend) +
        " queue=" + std::to_string(queueDepth) +
        " stop=" + std::to_string(stopOk ? 1 : 0) +
        " reset=" + std::to_string(resetOk ? 1 : 0) +
        " play=" + std::to_string(playOk ? 1 : 0) +
        " ret=" + ScriptAnyTypeName(playReturn.type) + ":" + ScriptAnyValueText(playReturn) +
        " setTime=" + std::to_string((timeGuarded && timeOk) ? 1 : 0) +
        (guardReason.empty() ? std::string() : " reason=" + guardReason);
    m_remoteProxyPoseHoldLast = detail;
    if (ok)
        ++m_animationTestProxyPlays;
    else
        ++m_animationTestProxyFailures;
    return ok;
}

bool ModMain::StartRemoteProxyAnimationLoop(
    const std::string& stateName,
    const std::string& animationName,
    float durationSeconds,
    int slot,
    int layer,
    float blend,
    std::string& detail)
{
    IEntity* entity = GetProxyEntity();
    IScriptTable* table = entity ? entity->GetScriptTable() : nullptr;
    if (!entity || !table)
    {
        detail = "remote_proxy_loop_failed_no_proxy state=" + stateName;
        m_remoteProxyPoseHoldLast = detail;
        ++m_animationTestProxyFailures;
        return false;
    }
    if (animationName.empty())
    {
        detail = "remote_proxy_loop_failed_empty_anim state=" + stateName;
        m_remoteProxyPoseHoldLast = detail;
        ++m_animationTestProxyFailures;
        return false;
    }

    durationSeconds = std::max(0.1f, durationSeconds);
    blend = std::max(0.0f, blend);
    const std::string effectiveState = stateName.empty() ? std::string("loop") : stateName;

    if (m_remoteProxyPoseHoldActive &&
        m_remoteProxyPoseHoldLoop &&
        m_remoteProxyPoseHoldEntityId == entity->GetId() &&
        m_remoteProxyPoseHoldName == effectiveState &&
        m_remoteProxyPoseHoldClip == animationName &&
        m_remoteProxyPoseHoldSlot == slot &&
        m_remoteProxyPoseHoldLayer == layer &&
        std::fabs(m_remoteProxyPoseHoldDuration - durationSeconds) < 0.001f &&
        std::fabs(m_remoteProxyPoseHoldBlend - blend) < 0.001f)
    {
        detail =
            "remote_proxy_loop_existing state=" + effectiveState +
            " entity=" + std::to_string(entity->GetId()) +
            " anim=" + animationName +
            " time=" + std::to_string(m_remoteProxyPoseHoldTime) +
            " ticks=" + std::to_string(m_remoteProxyPoseHoldTicks);
        m_remoteProxyPoseHoldLast = detail;
        return true;
    }

    ClearRemoteProxyPoseHold("remote_proxy_loop_restart");

    bool stopOk = false;
    std::string guardReason;
    TryGuardedCall(
        "remote proxy loop StopAnimation",
        [table, slot, layer]() { return Script::CallMethod(table, "StopAnimation", slot, layer); },
        stopOk,
        &guardReason);

    bool resetOk = false;
    TryGuardedCall(
        "remote proxy loop ResetAnimation",
        [table, slot, layer]() { return Script::CallMethod(table, "ResetAnimation", slot, layer); },
        resetOk,
        &guardReason);

    ScriptAnyValue playReturn;
    bool playOk = false;
    const bool playGuarded = TryGuardedCall(
        "remote proxy loop StartAnimation",
        [table, slot, &animationName, layer, blend, &playReturn]()
        {
            return CallAnimProxyStartAnimation(table, slot, animationName, layer, blend, 0.0f, playReturn);
        },
        playOk,
        &guardReason);
    playOk = playGuarded && playOk && ScriptAnyTruthyOrVoid(playReturn);

    bool timeOk = false;
    const bool timeGuarded = TryGuardedCall(
        "remote proxy loop SetAnimationTime",
        [table, slot, layer]()
        {
            return Script::CallMethod(table, "SetAnimationTime", slot, layer, 0.01f);
        },
        timeOk,
        &guardReason);

    int queueDepth = -1;
    ReadAnimProxyQueueDepth(table, slot, layer, queueDepth);

    const bool ok = playOk && timeGuarded && timeOk;
    if (ok)
    {
        m_remoteProxyPoseHoldActive = true;
        m_remoteProxyPoseHoldEntityId = entity->GetId();
        m_remoteProxyPoseHoldName = effectiveState;
        m_remoteProxyPoseHoldClip = animationName;
        m_remoteProxyPoseHoldSlot = slot;
        m_remoteProxyPoseHoldLayer = layer;
        // A few borrowed ArkHuman clips expose their bind pose exactly at the
        // boundary before the first authored key.
        m_remoteProxyPoseHoldTime = 0.01f;
        m_remoteProxyPoseHoldBlend = blend;
        m_remoteProxyPoseHoldAccumulator = m_remoteProxyPoseHoldInterval;
        m_remoteProxyPoseHoldTicks = 0;
        m_remoteProxyPoseHoldLoop = true;
        m_remoteProxyPoseHoldDuration = durationSeconds;
    }

    detail =
        std::string(ok ? "remote_proxy_loop_ok" : "remote_proxy_loop_failed") +
        " state=" + effectiveState +
        " entity=" + std::to_string(entity->GetId()) +
        " anim=" + animationName +
        " slot=" + std::to_string(slot) +
        " layer=" + std::to_string(layer) +
        " duration=" + std::to_string(durationSeconds) +
        " blend=" + std::to_string(blend) +
        " queue=" + std::to_string(queueDepth) +
        " stop=" + std::to_string(stopOk ? 1 : 0) +
        " reset=" + std::to_string(resetOk ? 1 : 0) +
        " play=" + std::to_string(playOk ? 1 : 0) +
        " ret=" + ScriptAnyTypeName(playReturn.type) + ":" + ScriptAnyValueText(playReturn) +
        " setTime=" + std::to_string((timeGuarded && timeOk) ? 1 : 0) +
        (guardReason.empty() ? std::string() : " reason=" + guardReason);
    m_remoteProxyPoseHoldLast = detail;
    if (ok)
        ++m_animationTestProxyPlays;
    else
        ++m_animationTestProxyFailures;
    return ok;
}

bool ModMain::StartRemoteProxyAnimationOnce(
    const std::string& stateName,
    const std::string& animationName,
    int slot,
    int layer,
    float blend,
    float speed,
    std::string& detail)
{
    IEntity* entity = GetProxyEntity();
    IScriptTable* table = entity ? entity->GetScriptTable() : nullptr;
    if (!entity || !table)
    {
        detail = "remote_proxy_once_failed_no_proxy state=" + stateName;
        m_remoteProxyPoseHoldLast = detail;
        ++m_animationTestProxyFailures;
        return false;
    }
    if (animationName.empty())
    {
        detail = "remote_proxy_once_failed_empty_anim state=" + stateName;
        m_remoteProxyPoseHoldLast = detail;
        ++m_animationTestProxyFailures;
        return false;
    }

    blend = std::max(0.0f, blend);
    speed = std::max(0.01f, speed);
    const std::string effectiveState = stateName.empty() ? std::string("once") : stateName;

    bool stopOk = false;
    std::string guardReason;
    TryGuardedCall(
        "remote proxy once StopAnimation",
        [table, slot, layer]() { return Script::CallMethod(table, "StopAnimation", slot, layer); },
        stopOk,
        &guardReason);

    bool resetOk = false;
    TryGuardedCall(
        "remote proxy once ResetAnimation",
        [table, slot, layer]() { return Script::CallMethod(table, "ResetAnimation", slot, layer); },
        resetOk,
        &guardReason);

    ScriptAnyValue playReturn;
    bool playOk = false;
    const bool playGuarded = TryGuardedCall(
        "remote proxy once StartAnimation",
        [table, slot, &animationName, layer, blend, speed, &playReturn]()
        {
            return CallAnimProxyStartAnimation(table, slot, animationName, layer, blend, speed, playReturn);
        },
        playOk,
        &guardReason);
    playOk = playGuarded && playOk && ScriptAnyTruthyOrVoid(playReturn);

    int queueDepth = -1;
    ReadAnimProxyQueueDepth(table, slot, layer, queueDepth);

    detail =
        std::string(playOk ? "remote_proxy_once_ok" : "remote_proxy_once_failed") +
        " state=" + effectiveState +
        " entity=" + std::to_string(entity->GetId()) +
        " anim=" + animationName +
        " slot=" + std::to_string(slot) +
        " layer=" + std::to_string(layer) +
        " blend=" + std::to_string(blend) +
        " speed=" + std::to_string(speed) +
        " queue=" + std::to_string(queueDepth) +
        " stop=" + std::to_string(stopOk ? 1 : 0) +
        " reset=" + std::to_string(resetOk ? 1 : 0) +
        " ret=" + ScriptAnyTypeName(playReturn.type) + ":" + ScriptAnyValueText(playReturn) +
        (guardReason.empty() ? std::string() : " reason=" + guardReason);
    m_remoteProxyPoseHoldLast = detail;
    if (playOk)
    {
        m_remotePoseActionOverlayActive = true;
        m_remotePoseActionOverlayEntityId = entity->GetId();
        m_remotePoseActionOverlayLayer = layer;
        ++m_animationTestProxyPlays;
    }
    else
    {
        ++m_animationTestProxyFailures;
    }
    return playOk;
}

void ModMain::ClearRemoteProxyActionOverlay(const char* reason)
{
    if (!m_remotePoseActionOverlayActive)
        return;

    bool stopOk = false;
    bool resetOk = false;
    std::string guardReason;
    IEntity* entity = gEnv && gEnv->pEntitySystem &&
            m_remotePoseActionOverlayEntityId != INVALID_ENTITYID
        ? gEnv->pEntitySystem->GetEntity(m_remotePoseActionOverlayEntityId)
        : nullptr;
    if (entity)
    {
        if (IScriptTable* table = entity->GetScriptTable())
        {
            TryGuardedCall(
                "remote proxy action overlay StopAnimation",
                [table, this]() { return Script::CallMethod(table, "StopAnimation", 0, m_remotePoseActionOverlayLayer); },
                stopOk,
                &guardReason);
            TryGuardedCall(
                "remote proxy action overlay ResetAnimation",
                [table, this]() { return Script::CallMethod(table, "ResetAnimation", 0, m_remotePoseActionOverlayLayer); },
                resetOk,
                &guardReason);
        }
    }

    m_remotePoseActionOverlayActive = false;
    m_remotePoseActionOverlayEntityId = INVALID_ENTITYID;
    m_lastPoseActionEvent =
        "remote_pose_action_overlay_clear reason=" + std::string(reason && reason[0] ? reason : "-") +
        " layer=" + std::to_string(m_remotePoseActionOverlayLayer) +
        " stop=" + std::to_string(stopOk ? 1 : 0) +
        " reset=" + std::to_string(resetOk ? 1 : 0) +
        (guardReason.empty() ? std::string() : " guard=" + guardReason);
}

void ModMain::ClearRemoteProxyPoseHold(const char* reason)
{
    if (!m_remoteProxyPoseHoldActive &&
        m_remoteProxyPoseHoldEntityId == INVALID_ENTITYID &&
        m_remoteProxyPoseHoldLast == "-")
    {
        return;
    }

    const EntityId entityId = m_remoteProxyPoseHoldEntityId;
    const int slot = m_remoteProxyPoseHoldSlot;
    const int layer = m_remoteProxyPoseHoldLayer;
    bool stopOk = false;
    bool resetOk = false;
    std::string guardReason;
    if (gEnv && gEnv->pEntitySystem && entityId != INVALID_ENTITYID)
    {
        if (IEntity* entity = gEnv->pEntitySystem->GetEntity(entityId))
        {
            if (IScriptTable* table = entity->GetScriptTable())
            {
                TryGuardedCall(
                    "remote proxy pose clear StopAnimation",
                    [table, slot, layer]() { return Script::CallMethod(table, "StopAnimation", slot, layer); },
                    stopOk,
                    &guardReason);
                TryGuardedCall(
                    "remote proxy pose clear ResetAnimation",
                    [table, slot, layer]() { return Script::CallMethod(table, "ResetAnimation", slot, layer); },
                    resetOk,
                    &guardReason);
            }
        }
    }

    m_remoteProxyPoseHoldActive = false;
    m_remoteProxyPoseHoldEntityId = INVALID_ENTITYID;
    m_remoteProxyPoseHoldAccumulator = 0.0f;
    m_remoteProxyPoseHoldLoop = false;
    m_remoteProxyPoseHoldLast =
        "remote_proxy_pose_cleared reason=" + std::string(reason && reason[0] ? reason : "-") +
        " pose=" + m_remoteProxyPoseHoldName +
        " clip=" + m_remoteProxyPoseHoldClip +
        " ticks=" + std::to_string(m_remoteProxyPoseHoldTicks) +
        " stop=" + std::to_string(stopOk ? 1 : 0) +
        " reset=" + std::to_string(resetOk ? 1 : 0) +
        (guardReason.empty() ? std::string() : " guard=" + guardReason);
}

void ModMain::TickRemoteProxyPoseHold(float frameTime)
{
    if (!m_remoteProxyPoseHoldActive)
        return;

    IEntity* entity = gEnv && gEnv->pEntitySystem && m_remoteProxyPoseHoldEntityId != INVALID_ENTITYID
        ? gEnv->pEntitySystem->GetEntity(m_remoteProxyPoseHoldEntityId)
        : nullptr;
    IScriptTable* table = entity ? entity->GetScriptTable() : nullptr;
    if (!entity || !table)
    {
        ClearRemoteProxyPoseHold("remote_proxy_pose_entity_missing");
        return;
    }

    const float dt = std::min(std::max(frameTime, 0.0f), 0.1f);
    m_remoteProxyPoseHoldAccumulator += dt;
    if (m_remoteProxyPoseHoldAccumulator < m_remoteProxyPoseHoldInterval)
        return;

    const float stepSeconds = m_remoteProxyPoseHoldAccumulator;
    m_remoteProxyPoseHoldAccumulator = 0.0f;
    if (m_remoteProxyPoseHoldLoop)
    {
        constexpr float kLoopEndpointInset = 0.01f;
        constexpr float kLoopUsableRange = 1.0f - 2.0f * kLoopEndpointInset;
        float phase = (m_remoteProxyPoseHoldTime - kLoopEndpointInset) / kLoopUsableRange;
        phase += stepSeconds / std::max(0.1f, m_remoteProxyPoseHoldDuration);
        phase -= std::floor(phase);
        // Never scrub through normalized 0/1; those frames can flash the
        // ArkHuman bind pose between otherwise valid idle loop frames.
        m_remoteProxyPoseHoldTime = kLoopEndpointInset + phase * kLoopUsableRange;
    }

    bool timeOk = false;
    std::string guardReason;
    const bool guarded = TryGuardedCall(
        "remote proxy pose tick SetAnimationTime",
        [table, this]()
        {
            return Script::CallMethod(
                table,
                "SetAnimationTime",
                m_remoteProxyPoseHoldSlot,
                m_remoteProxyPoseHoldLayer,
                m_remoteProxyPoseHoldTime);
        },
        timeOk,
        &guardReason);

    if (!guarded || !timeOk)
    {
        m_remoteProxyPoseHoldLast =
            "remote_proxy_pose_tick_failed pose=" + m_remoteProxyPoseHoldName +
            " clip=" + m_remoteProxyPoseHoldClip +
            " ticks=" + std::to_string(m_remoteProxyPoseHoldTicks) +
            (guardReason.empty() ? std::string() : " reason=" + guardReason);
        m_remoteProxyPoseHoldActive = false;
        ++m_animationTestProxyFailures;
        return;
    }

    ++m_remoteProxyPoseHoldTicks;
    if ((m_remoteProxyPoseHoldTicks % 20) == 0)
    {
        m_remoteProxyPoseHoldLast =
            "remote_proxy_pose_tick pose=" + m_remoteProxyPoseHoldName +
            " clip=" + m_remoteProxyPoseHoldClip +
            " time=" + std::to_string(m_remoteProxyPoseHoldTime) +
            " loop=" + std::to_string(m_remoteProxyPoseHoldLoop ? 1 : 0) +
            " ticks=" + std::to_string(m_remoteProxyPoseHoldTicks);
    }
}

bool ModMain::DebugAnimationTestProxyCommand(const std::string& command, const std::vector<std::string>& args, std::string& detail)
{
    if (command == "coop_anim_proxy_spawn")
    {
        if (!args.empty())
            m_animationTestArchetypeText = args.front();
        SpawnAnimationTestProxy();
        detail = m_lastAnimationTestEvent;
        return GetAnimationTestProxyEntity() != nullptr;
    }
    if (command == "coop_anim_proxy_archetype")
    {
        if (args.empty())
        {
            detail = "archetype=" + m_animationTestArchetypeText;
            m_lastAnimationTestEvent = detail;
            return true;
        }

        uint64_t parsed = 0;
        if (!ParseUint64Text(args.front(), parsed))
        {
            detail = "archetype_failed_bad_id " + args.front();
            m_lastAnimationTestEvent = detail;
            return false;
        }

        m_animationTestArchetypeText = args.front();
        detail = "archetype_ok " + m_animationTestArchetypeText;
        m_lastAnimationTestEvent = detail;
        return true;
    }
    if (command == "coop_anim_proxy_remove")
    {
        RemoveAnimationTestProxy();
        detail = m_lastAnimationTestEvent;
        return true;
    }
    if (command == "coop_anim_proxy_probe")
    {
        const bool ok = ProbeAnimationTestProxy(detail);
        m_lastAnimationTestEvent = detail;
        return ok;
    }
    if (command == "coop_anim_proxy_probe_clips")
    {
        const int slot = ParseIntArg(args, 0, m_animationTestSlot);
        return ProbeAnimationTestProxyClipNames(slot, detail);
    }
    if (command == "coop_anim_proxy_npc")
    {
        const std::string action = args.empty() ? std::string("status") : args.front();
        const float duration = ParseFloatArg(args, 1, 2.0f);
        return RunAnimationTestProxyNpcNativeAction(action, duration, detail);
    }
    if (command == "coop_anim_proxy_state")
    {
        const std::string state = args.empty() ? std::string("status") : args.front();
        const float duration = ParseFloatArg(args, 1, 2.0f);
        return ApplyAnimationTestProxyCharacterState(state, duration, detail);
    }
    if (command == "coop_anim_proxy_pose")
    {
        std::string poseName = args.empty() ? std::string("crouch") : args.front();
        std::string clipName = poseName;
        float normalizedTime = ParseFloatArg(args, 1, 0.30f);
        float blend = ParseFloatArg(args, 4, 0.01f);

        const std::string normalizedPose = ToLowerAsciiAnimation(poseName);
        if (normalizedPose == "crouch" || normalizedPose == "crouch_pose" || normalizedPose == "crouchpose")
        {
            poseName = "crouch_pose";
            clipName = "fear_cower_c_empty";
            if (args.size() < 2)
                normalizedTime = 0.20f;
            if (args.size() < 5)
                blend = 0.01f;
        }
        else if (normalizedPose == "downed" || normalizedPose == "downed_pose" || normalizedPose == "downedpose")
        {
            poseName = "downed_pose";
            clipName = "combat_forceresist_front_out_empty";
            if (args.size() < 2)
                normalizedTime = 0.25f;
            if (args.size() < 5)
                blend = 0.05f;
        }
        else if (args.size() >= 2)
        {
            poseName = "custom";
            clipName = args.front();
        }

        const int slot = ParseIntArg(args, 2, m_animationTestSlot);
        const int layer = ParseIntArg(args, 3, m_animationTestLayer);
        return StartAnimationTestProxyPoseHold(poseName, clipName, normalizedTime, slot, layer, blend, detail);
    }
    if (command == "coop_proxy_pose")
    {
        std::string poseName = args.empty() ? std::string("status") : args.front();
        const std::string normalizedPose = ToLowerAsciiAnimation(poseName);
        if (normalizedPose == "clear" || normalizedPose == "normal" || normalizedPose == "stand")
        {
            ClearRemoteProxyPoseHold("manual_proxy_pose_clear");
            detail = m_remoteProxyPoseHoldLast;
            return true;
        }
        if (normalizedPose == "status")
        {
            detail =
                "remote_proxy_pose_status active=" + std::to_string(m_remoteProxyPoseHoldActive ? 1 : 0) +
                " pose=" + m_remoteProxyPoseHoldName +
                " clip=" + m_remoteProxyPoseHoldClip +
                " entity=" + std::to_string(m_remoteProxyPoseHoldEntityId) +
                " time=" + std::to_string(m_remoteProxyPoseHoldTime) +
                " loop=" + std::to_string(m_remoteProxyPoseHoldLoop ? 1 : 0) +
                " ticks=" + std::to_string(m_remoteProxyPoseHoldTicks) +
                " last=" + m_remoteProxyPoseHoldLast;
            return true;
        }

        if (normalizedPose == "idle" || normalizedPose == "idle_loop")
        {
            const float duration = ParseFloatArg(args, 1, 2.0f);
            const int slot = ParseIntArg(args, 2, 0);
            const int layer = ParseIntArg(args, 3, 0);
            const float blend = ParseFloatArg(args, 4, 0.08f);
            return StartRemoteProxyAnimationLoop(
                "idle_loop",
                "Relaxed_Idle_A_Empty",
                duration,
                slot,
                layer,
                blend,
                detail);
        }
        if (normalizedPose == "walk" || normalizedPose == "walk_loop")
        {
            const float duration = ParseFloatArg(args, 1, 1.15f);
            const int slot = ParseIntArg(args, 2, 0);
            const int layer = ParseIntArg(args, 3, 0);
            const float blend = ParseFloatArg(args, 4, 0.08f);
            return StartRemoteProxyAnimationLoop(
                "walk_loop",
                "4d-Bspace_Relaxed_MoveTurnStrafeSlope_Empty",
                duration,
                slot,
                layer,
                blend,
                detail);
        }
        if (normalizedPose == "run" || normalizedPose == "run_loop")
        {
            const float duration = ParseFloatArg(args, 1, 0.85f);
            const int slot = ParseIntArg(args, 2, 0);
            const int layer = ParseIntArg(args, 3, 0);
            const float blend = ParseFloatArg(args, 4, 0.08f);
            return StartRemoteProxyAnimationLoop(
                "run_loop",
                "Fear_Run_Forward_Empty",
                duration,
                slot,
                layer,
                blend,
                detail);
        }
        if (normalizedPose == "hacking" || normalizedPose == "hack" || normalizedPose == "hacking_loop")
        {
            const float duration = ParseFloatArg(args, 1, 1.0f);
            const int slot = ParseIntArg(args, 2, 0);
            const int layer = ParseIntArg(args, 3, 0);
            const float blend = ParseFloatArg(args, 4, 0.08f);
            return StartRemoteProxyAnimationLoop(
                "hacking_loop",
                "Arboretum_Sq04_DahlHack_Dahl",
                duration,
                slot,
                layer,
                blend,
                detail);
        }
        if (normalizedPose == "zerog" || normalizedPose == "zero_g" ||
            normalizedPose == "zerog_idle" || normalizedPose == "zero_g_idle")
        {
            const float normalizedTime = ParseFloatArg(args, 1, 0.10f);
            const int slot = ParseIntArg(args, 2, 0);
            const int layer = ParseIntArg(args, 3, 0);
            const float blend = ParseFloatArg(args, 4, 0.06f);
            const bool started = StartRemoteProxyPoseHold(
                "zero_g_idle_pose",
                "ShiftPose_ZeroG_Forward_A",
                normalizedTime,
                slot,
                layer,
                blend,
                detail);
            std::string motionReason;
            IEntity* proxy = GetProxyEntity();
            const bool motionApplied = proxy &&
                ApplyRemotePlayerMotionParams(*proxy, 0.0f, 0.0f, 1.0f / 30.0f, &motionReason);
            detail +=
                " motionParams=" + std::to_string(motionApplied ? 1 : 0) +
                (motionReason.empty() ? std::string() : " motionReason=" + motionReason);
            return started && motionApplied;
        }
        const bool zeroGMove =
            normalizedPose == "zerog_move" || normalizedPose == "zero_g_move" ||
            normalizedPose == "zerog_forward" || normalizedPose == "zero_g_forward" ||
            normalizedPose == "zerog_backward" || normalizedPose == "zero_g_backward" ||
            normalizedPose == "zerog_left" || normalizedPose == "zero_g_left" ||
            normalizedPose == "zerog_right" || normalizedPose == "zero_g_right";
        if (zeroGMove)
        {
            const float normalizedTime = ParseFloatArg(args, 1, 0.10f);
            const int slot = ParseIntArg(args, 2, 0);
            const int layer = ParseIntArg(args, 3, 0);
            const float blend = ParseFloatArg(args, 4, 0.06f);
            constexpr float kHalfPi = 1.57079632679f;
            float travelAngle = 0.0f;
            if (normalizedPose.find("backward") != std::string::npos)
                travelAngle = 2.0f * kHalfPi;
            else if (normalizedPose.find("left") != std::string::npos)
                travelAngle = -kHalfPi;
            else if (normalizedPose.find("right") != std::string::npos)
                travelAngle = kHalfPi;
            const char* clip = SelectRemotePlayerZeroGShiftClip(travelAngle, true);
            const bool started = StartRemoteProxyPoseHold(
                "zero_g_direction_pose",
                clip,
                normalizedTime,
                slot,
                layer,
                blend,
                detail);
            std::string motionReason;
            IEntity* proxy = GetProxyEntity();
            const bool motionApplied = proxy &&
                ApplyRemotePlayerMotionParams(*proxy, 2.0f, travelAngle, 1.0f / 30.0f, &motionReason);
            detail +=
                " travelAngle=" + std::to_string(travelAngle) +
                " motionParams=" + std::to_string(motionApplied ? 1 : 0) +
                (motionReason.empty() ? std::string() : " motionReason=" + motionReason);
            return started && motionApplied;
        }
        if (normalizedPose == "pistol_idle" || normalizedPose == "pistol_idle_loop" || normalizedPose == "weapon_idle")
        {
            const float duration = ParseFloatArg(args, 1, 2.0f);
            const int slot = ParseIntArg(args, 2, 0);
            const int layer = ParseIntArg(args, 3, 0);
            const float blend = ParseFloatArg(args, 4, 0.08f);
            return StartRemoteProxyAnimationLoop(
                "pistol_idle_loop",
                "combat_aiming_idle_pistol",
                duration,
                slot,
                layer,
                blend,
                detail);
        }
        if (normalizedPose == "pistol_walk" || normalizedPose == "pistol_walk_loop" || normalizedPose == "weapon_walk")
        {
            const float duration = ParseFloatArg(args, 1, 1.05f);
            const int slot = ParseIntArg(args, 2, 0);
            const int layer = ParseIntArg(args, 3, 0);
            const float blend = ParseFloatArg(args, 4, 0.08f);
            return StartRemoteProxyAnimationLoop(
                "pistol_walk_loop",
                "4d-Bspace_Combat_Aiming_MoveTurnStrafeSlope_Pistol",
                duration,
                slot,
                layer,
                blend,
                detail);
        }

        std::string clipName = poseName;
        float normalizedTime = ParseFloatArg(args, 1, 0.20f);
        float blend = ParseFloatArg(args, 4, 0.01f);
        if (normalizedPose == "crouch" || normalizedPose == "crouch_pose" || normalizedPose == "crouchpose")
        {
            // This is a hand-tuned frame from a cowering clip, not a crouch
            // animation. Keep the debug command aligned with production hold.
            poseName = "crouch_pose";
            clipName = "fear_cower_c_empty";
            if (args.size() < 2)
                normalizedTime = 0.20f;
            if (args.size() < 5)
                blend = 0.01f;
        }
        else if (normalizedPose == "downed" || normalizedPose == "downed_pose" || normalizedPose == "downedpose")
        {
            poseName = "downed_pose";
            clipName = "combat_forceresist_front_out_empty";
            if (args.size() < 2)
                normalizedTime = 0.25f;
            if (args.size() < 5)
                blend = 0.05f;
        }
        else if (args.size() >= 2)
        {
            poseName = "custom";
            clipName = args.front();
        }

        const int slot = ParseIntArg(args, 2, 0);
        const int layer = ParseIntArg(args, 3, 0);
        return StartRemoteProxyPoseHold(poseName, clipName, normalizedTime, slot, layer, blend, detail);
    }
    if (command == "coop_anim_proxy_trace")
    {
        const std::string label = args.empty() ? std::string("manual") : args.front();
        const float seconds = ParseFloatArg(args, 1, 3.0f);
        StartAnimationTestProxyTrace(label, seconds);
        detail = m_lastAnimationTestEvent;
        return true;
    }
    if (command == "coop_anim_fragments")
    {
        const std::string kind = args.empty() ? std::string("human") : args.front();
        int start = 0;
        int count = 32;
        std::string filter;
        if (args.size() >= 2)
        {
            if (IsIntegerText(args[1]))
            {
                start = ParseIntArg(args, 1, 0);
                count = ParseIntArg(args, 2, 32);
                if (args.size() >= 4)
                    filter = args[3];
            }
            else
            {
                filter = args[1];
                count = ParseIntArg(args, 2, 32);
            }
        }
        detail = BuildMannequinFragmentDump(kind, start, count, filter);
        m_lastAnimationTestEvent = detail;
        return true;
    }
    if (command == "coop_anim_snippets")
    {
        const std::string kind = args.empty() ? std::string("human") : args.front();
        std::string query;
        int count = 16;
        std::string tagFilter;
        if (args.size() >= 2)
            query = args[1];
        if (args.size() >= 3)
            count = ParseIntArg(args, 2, 16);
        if (args.size() >= 4)
            tagFilter = args[3];

        detail = BuildMannequinSnippetDump(kind, query, count, tagFilter);
        m_lastAnimationTestEvent = detail;
        return detail.find("_failed") == std::string::npos;
    }
    if (command == "coop_anim_snippet_catalog")
    {
        const std::string kind = args.empty() ? std::string("human") : args.front();
        const bool ok = WriteMannequinSnippetCatalog(kind, detail);
        m_lastAnimationTestEvent = detail;
        if (!ok)
            ++m_animationTestProxyFailures;
        return ok;
    }
    if (command == "coop_anim_state_catalog")
    {
        const std::string filter = args.empty() ? std::string() : args.front();
        detail = BuildProxyAnimationStateCatalog(filter);
        m_lastAnimationTestEvent = detail;
        return true;
    }
    if (command == "coop_anim_proxy_model")
    {
        if (args.empty())
        {
            detail = "model_failed_missing_path";
            m_lastAnimationTestEvent = detail;
            return false;
        }
        const int slot = ParseIntArg(args, 1, m_animationTestSlot);
        return LoadAnimationTestProxyModel(args.front(), slot, detail);
    }
    if (command == "coop_anim_proxy_play")
    {
        if (args.empty())
        {
            detail = "play_failed_missing_anim";
            m_lastAnimationTestEvent = detail;
            return false;
        }
        const int slot = ParseIntArg(args, 1, m_animationTestSlot);
        const int layer = ParseIntArg(args, 2, m_animationTestLayer);
        const float blend = ParseFloatArg(args, 3, m_animationTestBlend);
        const float speed = ParseFloatArg(args, args.size() > 5 ? 5 : 4, m_animationTestSpeed);
        return PlayAnimationTestProxyAnimation(args.front(), slot, layer, blend, speed, detail);
    }
    if (command == "coop_anim_proxy_stop")
    {
        const int slot = ParseIntArg(args, 0, m_animationTestSlot);
        const int layer = ParseIntArg(args, 1, m_animationTestLayer);
        return StopAnimationTestProxyAnimation(slot, layer, detail);
    }
    if (command == "coop_anim_proxy_reset")
    {
        const int slot = ParseIntArg(args, 0, m_animationTestSlot);
        const int layer = ParseIntArg(args, 1, m_animationTestLayer);
        return ResetAnimationTestProxyAnimation(slot, layer, detail);
    }
    if (command == "coop_anim_proxy_time")
    {
        const float normalizedTime = ParseFloatArg(args, 0, m_animationTestTime);
        const int slot = ParseIntArg(args, 1, m_animationTestSlot);
        const int layer = ParseIntArg(args, 2, m_animationTestLayer);
        return SetAnimationTestProxyTime(slot, layer, normalizedTime, detail);
    }

    detail = "anim_proxy_unknown_command " + command + " " + JoinRuntimeArgs(args, 0);
    m_lastAnimationTestEvent = detail;
    return false;
}
