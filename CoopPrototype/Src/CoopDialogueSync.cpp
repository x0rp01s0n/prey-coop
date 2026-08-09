#include "ModMain.h"
#include "CoopRuntimeGuards.h"
#include "CoopSerialSequence.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include <Prey/Ark/ArkCharacter.h>
#include <Prey/CryAction/IGameObject.h>
#include <Prey/CryEntitySystem/IEntitySystem.h>
#include <Prey/CryGame/IGameFramework.h>
#include <Prey/GameDll/ark/ArkSpeakerExtension.h>
#include <Prey/GameDll/ark/dialog/ArkConversation.h>
#include <Prey/GameDll/ark/dialog/ArkDialogPlayer.h>
#include <Prey/GameDll/ark/dialog/ArkDialogPlayerPA.h>
#include <Prey/GameDll/ark/dialog/ArkDialogPlayerTranscribe.h>
#include <Prey/GameDll/ark/dialog/ArkResponse.h>
#include <Prey/GameDll/ark/dialog/ArkResponseQuery.h>
#include <Prey/GameDll/ark/dialog/arkpadialogmanager.h>
#include <Prey/GameDll/ark/dialog/arkresponsetypes.h>
#include <Prey/GameDll/ark/player/ArkPlayer.h>
#include <Prey/GameDll/ark/player/ArkPlayerComponent.h>

namespace
{
using CoopRuntimeGuards::IsLikelyRuntimeCppObject;
using CoopRuntimeGuards::ReadRuntimeCString;
using CoopRuntimeGuards::TryGuardedCall;
using CoopRuntimeGuards::TryGuardedVoidCall;

constexpr float kDialogueLeaseTimeoutSeconds = 20.0f;
constexpr float kDialogueLeaseActivitySeconds = 3.0f;
constexpr float kDialogueReplayRetrySeconds = 0.25f;

void MixFnv64(uint64_t& hash, uint64_t value)
{
    for (int byte = 0; byte < 8; ++byte)
    {
        hash ^= (value >> (byte * 8)) & 0xffu;
        hash *= 1099511628211ull;
    }
}

uint64_t BuildDialogueLeaseEventId(
    uint64_t source,
    uint64_t dialogueId,
    uint32_t sequence,
    uint16_t command)
{
    uint64_t hash = 14695981039346656037ull;
    MixFnv64(hash, 0x4449414c4f475545ull); // "DIALOGUE"
    MixFnv64(hash, source);
    MixFnv64(hash, dialogueId);
    MixFnv64(hash, sequence);
    MixFnv64(hash, command);
    return hash == 0 ? 1 : hash;
}

uint64_t BuildDialogueTriggerEventId(
    uint64_t source,
    uint64_t dialogueId,
    uint32_t sequence,
    uint64_t speakerKey,
    uint64_t ruleId)
{
    uint64_t hash = 14695981039346656037ull;
    MixFnv64(hash, 0x4449414c54524752ull); // "DIALTRGR"
    MixFnv64(hash, source);
    MixFnv64(hash, dialogueId);
    MixFnv64(hash, sequence);
    MixFnv64(hash, speakerKey);
    MixFnv64(hash, ruleId);
    return hash == 0 ? 1 : hash;
}

template <size_t N>
bool CopyWireText(const std::string& value, char (&target)[N])
{
    static_assert(N > 0);
    if (value.size() >= N)
        return false;

    std::memset(target, 0, N);
    if (!value.empty())
        std::memcpy(target, value.data(), value.size());
    return true;
}

bool EncodeDialogueQueryFact(
    const ArkResponseFact& source,
    CoopProtocol::DialogueQueryFactWire& target)
{
    target = {};
    target.key = source.key;
    target.numberBits = source.value.num.integer;

    const std::string stringValue =
        source.value.str.c_str()
            ? std::string(source.value.str.c_str())
            : std::string();

    if (stringValue.size() >= CoopProtocol::kDialogueQueryStringBytes)
        return false;

    target.stringLength = static_cast<uint16_t>(stringValue.size());
    if (!stringValue.empty())
    {
        std::memcpy(
            target.stringValue,
            stringValue.data(),
            stringValue.size());
    }
    return true;
}

bool DecodeDialogueQueryFact(
    const CoopProtocol::DialogueQueryFactWire& source,
    ArkResponseFact& target)
{
    if (source.key == 0 ||
        source.stringLength >= CoopProtocol::kDialogueQueryStringBytes)
    {
        return false;
    }

    target.key = source.key;
    target.value.num.integer = source.numberBits;
    if (source.stringLength != 0)
    {
        target.value.str.assign(
            source.stringValue,
            source.stringLength);
    }
    else
    {
        target.value.str.clear();
    }
    return true;
}

bool DialoguePacketCarriesTrigger(
    const CoopProtocol::DialogueLeasePacket& packet)
{
    const uint16_t specialSpeaker =
        packet.flags &
        CoopProtocol::kDialogueSpecialSpeakerMask;
    const bool specialSpeakerValid =
        specialSpeaker != 0 &&
        (specialSpeaker & (specialSpeaker - 1)) == 0;

    return
        packet.triggerEventId != 0 &&
        packet.dialogueId != 0 &&
        packet.ruleId != 0 &&
        (packet.speakerEntityGuid != 0 ||
            packet.speakerCharacterId != 0 ||
            specialSpeakerValid) &&
        packet.queryFactCount <=
            CoopProtocol::kMaxDialogueQueryFacts;
}

bool DialoguePacketConceptIsValid(
    const CoopProtocol::DialogueLeasePacket& packet)
{
    if ((packet.flags & CoopProtocol::kDialogueHasConcept) == 0)
        return true;

    return std::memchr(
        packet.conceptText,
        '\0',
        sizeof(packet.conceptText)) != nullptr;
}

} // namespace

bool ModMain::BuildDialogueLeasePacket(
    CoopProtocol::DialogueLeasePacket& packet,
    CoopProtocol::DialogueLeaseCommand command,
    uint64_t dialogueId,
    uint64_t targetPeerHash) const
{
    if (dialogueId == 0 ||
        m_localWorldEpoch == 0 ||
        m_localLevelId == 0)
    {
        return false;
    }

    if (m_dialogueLeaseDescriptor.dialogueId == dialogueId)
        packet = m_dialogueLeaseDescriptor;
    else
        packet = {};

    packet.magic = CoopProtocol::kPacketMagic;
    packet.version = CoopProtocol::kProtocolVersion;
    packet.type =
        static_cast<uint16_t>(
            CoopProtocol::PacketType::DialogueLease);
    packet.sequence = m_dialogueLeaseSequence;
    packet.worldEpoch = m_localWorldEpoch;
    packet.hostSaveKeyHash = CurrentHostSaveKeyHash();
    packet.areaId = m_localLevelId;
    packet.sourcePeerHash = GetLocalAccountToken();
    packet.targetPeerHash = targetPeerHash;
    packet.dialogueId = dialogueId;
    packet.ownerPeerHash = m_dialogueLeaseOwnerHash;
    packet.leaseEpoch = m_dialogueLeaseEpoch;
    packet.command = static_cast<uint16_t>(command);
    packet.eventId = BuildDialogueLeaseEventId(
        packet.sourcePeerHash,
        dialogueId,
        packet.sequence,
        packet.command);

    return
        packet.hostSaveKeyHash != 0 &&
        packet.sourcePeerHash != 0;
}

bool ModMain::CaptureDialogueSpeakerIdentity(
    ArkSpeakerBase* speaker,
    int paChannel,
    uint64_t& speakerEntityGuid,
    uint64_t& speakerCharacterId,
    uint16_t& speakerIdentityFlags,
    int32_t& resolvedPaChannel,
    std::string& detail) const
{
    detail.clear();
    speakerEntityGuid = 0;
    speakerCharacterId = 0;
    speakerIdentityFlags = 0;
    resolvedPaChannel = paChannel;

    if (!speaker ||
        !IsLikelyRuntimeCppObject(
            speaker,
            sizeof(void*) * 4))
    {
        detail = "invalid_speaker";
        return false;
    }

    IEntity* speakerEntity = nullptr;
    TryGuardedCall(
        "dialogue capture speaker entity",
        [speaker]()
        {
            return speaker->m_pEntity;
        },
        speakerEntity,
        nullptr);

    if (speakerEntity)
    {
        TryGuardedCall(
            "dialogue capture speaker guid",
            [speakerEntity]()
            {
                return static_cast<uint64_t>(
                    speakerEntity->GetGuid());
            },
            speakerEntityGuid,
            nullptr);
    }

    const ArkCharacter* character = nullptr;
    TryGuardedCall(
        "dialogue capture speaker character",
        [speaker]()
        {
            return speaker->m_pCharacter;
        },
        character,
        nullptr);

    if (character)
    {
        TryGuardedCall(
            "dialogue capture character id",
            [character]()
            {
                return character->m_ID;
            },
            speakerCharacterId,
            nullptr);
    }

    if (ArkPlayer::GetInstancePtr())
    {
        ArkPlayerComponent& playerComponent =
            ArkPlayer::GetInstance().m_playerComponent;
        if (speaker == playerComponent.m_pPlayerSpeaker.get())
        {
            speakerIdentityFlags =
                CoopProtocol::kDialogueSpeakerPlayer;
        }
        else if (speaker == playerComponent.m_pSuitSpeaker.get())
        {
            speakerIdentityFlags =
                CoopProtocol::kDialogueSpeakerSuit;
        }
        else if (speaker ==
            playerComponent.m_pDiscRifleSpeaker.get())
        {
            speakerIdentityFlags =
                CoopProtocol::kDialogueSpeakerDiscRifle;
        }
        else if (speaker ==
            playerComponent.m_pTranscribeSpeaker.get())
        {
            speakerIdentityFlags =
                CoopProtocol::kDialogueSpeakerTranscribe;
        }
    }

    if (speakerIdentityFlags == 0)
    {
        // PlayPAAnnouncement can reach TriggerRule with an unset paChannel.
        // The manager-owned speaker address is the authoritative channel
        // identity and is stable in every process.
        IArkPADialogManager* interfaceManager =
            gEnv && gEnv->pGame
                ? gEnv->pGame->GetIArkPADialogManager()
                : nullptr;
        CArkPADialogManager* manager =
            static_cast<CArkPADialogManager*>(
                interfaceManager);
        if (manager)
        {
            for (size_t channel = 0;
                 channel < manager->m_speakers.size();
                 ++channel)
            {
                if (speaker == &manager->m_speakers[channel])
                {
                    speakerIdentityFlags =
                        CoopProtocol::kDialogueSpeakerPA;
                    resolvedPaChannel =
                        static_cast<int>(channel);
                    break;
                }
            }
        }
    }

    if (speakerIdentityFlags == 0)
    {
        EArkDialogPlayerType playerType =
            EArkDialogPlayerType::invalid;
        TryGuardedCall(
            "dialogue capture speaker player type",
            [speaker]()
            {
                return speaker->GetPlayerType();
            },
            playerType,
            nullptr);
        if (playerType == EArkDialogPlayerType::pa &&
            resolvedPaChannel >= 0 && resolvedPaChannel < 4)
        {
            speakerIdentityFlags =
                CoopProtocol::kDialogueSpeakerPA;
        }
    }

    if (speakerEntityGuid == 0 &&
        speakerCharacterId == 0 &&
        speakerIdentityFlags == 0)
    {
        uint64_t voiceId = 0;
        TryGuardedCall(
            "dialogue capture speaker voice",
            [speaker]()
            {
                return speaker->m_voiceId != 0
                    ? speaker->m_voiceId
                    : speaker->m_playbackVoice;
            },
            voiceId,
            nullptr);
        if (voiceId != 0)
        {
            speakerCharacterId = voiceId;
            speakerIdentityFlags =
                CoopProtocol::kDialogueSpeakerVoiceIdentity;
        }
    }

    if (speakerEntityGuid == 0 &&
        speakerCharacterId == 0 &&
        speakerIdentityFlags == 0)
    {
        detail = "speaker_has_no_stable_identity";
        return false;
    }

    detail = "speaker_identity_captured";
    return true;
}

void ModMain::RememberDialogueSpeakerForHook(
    ArkSpeakerBase* speaker,
    const char* reason)
{
    uint64_t entityGuid = 0;
    uint64_t characterId = 0;
    uint16_t identityFlags = 0;
    int32_t paChannel = -1;
    std::string detail;
    if (!CaptureDialogueSpeakerIdentity(
            speaker,
            -1,
            entityGuid,
            characterId,
            identityFlags,
            paChannel,
            detail))
    {
        return;
    }

    if (entityGuid != 0)
        m_dialogueSpeakersByGuid[entityGuid] = speaker;
    if (characterId != 0 &&
        (identityFlags &
            CoopProtocol::kDialogueSpeakerVoiceIdentity) == 0)
    {
        m_dialogueSpeakersByCharacterId[characterId] = speaker;
    }

    (void)reason;
}

bool ModMain::CaptureDialogueTriggerPacket(
    CoopProtocol::DialogueLeasePacket& packet,
    ArkSpeakerBase* speaker,
    ArkConversation* conversation,
    uint64_t ruleId,
    bool ignoreVoiceRequirement,
    const char* concept,
    const ArkResponseQuery* query,
    int paChannel,
    bool isLiveAudio,
    int priority,
    std::string& detail)
{
    detail.clear();

    if (!speaker ||
        !conversation ||
        ruleId == 0 ||
        !IsLikelyRuntimeCppObject(
            speaker,
            sizeof(void*) * 4))
    {
        detail = "invalid_trigger_arguments";
        return false;
    }

    uint64_t runtimeConversationId = 0;
    if (!TryGuardedCall(
            "dialogue capture runtime conversation id",
            [conversation]()
            {
                return conversation->m_conversationId;
            },
            runtimeConversationId,
            &detail) ||
        runtimeConversationId == 0)
    {
        detail = "missing_runtime_conversation_id";
        return false;
    }

    uint64_t dialogueId = ruleId;
    const auto existingStoryId =
        m_dialogueStoryIdByRuntimeId.find(
            runtimeConversationId);
    if (existingStoryId !=
            m_dialogueStoryIdByRuntimeId.end() &&
        existingStoryId->second != 0)
    {
        dialogueId = existingStoryId->second;
    }
    else
    {
        BindDialogueRuntimeId(
            runtimeConversationId,
            dialogueId);
    }

    uint64_t speakerEntityGuid = 0;
    uint64_t speakerCharacterId = 0;
    uint16_t speakerIdentityFlags = 0;
    int32_t resolvedPaChannel = paChannel;
    if (!CaptureDialogueSpeakerIdentity(
            speaker,
            paChannel,
            speakerEntityGuid,
            speakerCharacterId,
            speakerIdentityFlags,
            resolvedPaChannel,
            detail))
    {
        return false;
    }

    packet = {};
    packet.sequence = m_dialogueLeaseSequence;
    packet.worldEpoch = m_localWorldEpoch;
    packet.hostSaveKeyHash = CurrentHostSaveKeyHash();
    packet.areaId = m_localLevelId;
    packet.sourcePeerHash = GetLocalAccountToken();
    packet.ownerPeerHash = packet.sourcePeerHash;
    packet.dialogueId = dialogueId;
    packet.speakerEntityGuid = speakerEntityGuid;
    packet.speakerCharacterId = speakerCharacterId;
    packet.ruleId = ruleId;
    packet.paChannel = resolvedPaChannel;
    packet.priority = priority;
    packet.command =
        static_cast<uint16_t>(
            CoopProtocol::DialogueLeaseCommand::Request);
    packet.flags |= speakerIdentityFlags;

    if (ignoreVoiceRequirement)
    {
        packet.flags |=
            CoopProtocol::kDialogueIgnoreVoiceRequirement;
    }
    if (isLiveAudio)
        packet.flags |= CoopProtocol::kDialogueLiveAudio;

    bool important = false;
    TryGuardedCall(
        "dialogue capture important",
        [conversation]()
        {
            return conversation->m_bImportant;
        },
        important,
        nullptr);
    if (important)
        packet.flags |= CoopProtocol::kDialogueImportant;

    if (concept && concept[0])
    {
        const std::string conceptText =
            ReadRuntimeCString(
                concept,
                CoopProtocol::kDialogueConceptBytes);
        if (conceptText.empty() ||
            !CopyWireText(
                conceptText,
                packet.conceptText))
        {
            detail =
                "concept_too_long_or_unreadable";
            return false;
        }
        packet.flags |= CoopProtocol::kDialogueHasConcept;
    }

    if (query)
    {
        size_t factCount = 0;
        if (!TryGuardedCall(
                "dialogue capture query fact count",
                [query]()
                {
                    return query->m_facts.size();
                },
                factCount,
                &detail))
        {
            detail = "query_fact_count_failed";
            return false;
        }

        if (factCount >
            CoopProtocol::kMaxDialogueQueryFacts)
        {
            detail =
                "query_fact_count_exceeds_wire_" +
                std::to_string(factCount);
            return false;
        }

        for (size_t index = 0;
             index < factCount;
             ++index)
        {
            const ArkResponseFact* fact = nullptr;
            if (!TryGuardedCall(
                    "dialogue capture query fact",
                    [query, index]()
                    {
                        return &query->m_facts[index];
                    },
                    fact,
                    &detail) ||
                !fact ||
                !EncodeDialogueQueryFact(
                    *fact,
                    packet.queryFacts[index]))
            {
                detail =
                    "query_fact_encode_failed_" +
                    std::to_string(index);
                return false;
            }
        }

        packet.queryFactCount =
            static_cast<uint16_t>(factCount);
        if (factCount != 0)
            packet.flags |= CoopProtocol::kDialogueHasQuery;
    }

    const ArkResponse* response = nullptr;
    TryGuardedCall(
        "dialogue capture selected response",
        [speaker]()
        {
            return speaker->m_pCurrentResponse;
        },
        response,
        nullptr);
    if (response)
    {
        TryGuardedCall(
            "dialogue capture selected response id",
            [response]()
            {
                return response->m_id;
            },
            packet.expectedResponseId,
            nullptr);
    }

    packet.triggerEventId =
        BuildDialogueTriggerEventId(
            packet.sourcePeerHash,
            packet.dialogueId,
            packet.sequence,
            speakerEntityGuid != 0
                ? speakerEntityGuid
                : (speakerCharacterId != 0
                    ? speakerCharacterId
                    : (static_cast<uint64_t>(
                            speakerIdentityFlags) << 32) |
                        static_cast<uint32_t>(resolvedPaChannel)),
            packet.ruleId);
    packet.eventId =
        BuildDialogueLeaseEventId(
            packet.sourcePeerHash,
            packet.dialogueId,
            packet.sequence,
            packet.command);

    if (speakerEntityGuid != 0)
    {
        m_dialogueSpeakersByGuid[
            speakerEntityGuid] = speaker;
    }
    if (speakerCharacterId != 0)
    {
        m_dialogueSpeakersByCharacterId[
            speakerCharacterId] = speaker;
    }

    detail =
        "captured_dialogue_" +
        std::to_string(packet.dialogueId) +
        "_trigger_" +
        std::to_string(packet.triggerEventId);
    return true;
}

bool ModMain::SendDialogueLeaseTo(
    const CoopProtocol::DialogueLeasePacket& packet,
    uint32_t address,
    uint16_t port,
    const char* failurePrefix)
{
    if (!SendReliablePayloadTo(
            static_cast<uint16_t>(
                CoopProtocol::PacketType::DialogueLease),
            &packet,
            sizeof(packet),
            address,
            port,
            failurePrefix))
    {
        ++m_dialogueLeaseDropped;
        return false;
    }

    ++m_dialogueLeaseSent;
    return true;
}

bool ModMain::RequestDialogueLease(
    const CoopProtocol::DialogueLeasePacket& request,
    const char* reason)
{
    if (m_networkMode != CoopNetworkMode::Client ||
        !m_hasRemoteEndpoint ||
        !IsSessionGameplayReady() ||
        !DialoguePacketCarriesTrigger(request))
    {
        return false;
    }

    // Every Request command needs a fresh command sequence/eventId. The
    // triggerEventId stays unchanged because it identifies the one native
    // TriggerRule instance across Request/Grant/Activity/Release.
    CoopProtocol::DialogueLeasePacket packet = request;
    CoopSerialSequence::Advance(m_dialogueLeaseSequence);
    packet.sequence = m_dialogueLeaseSequence;
    packet.magic = CoopProtocol::kPacketMagic;
    packet.version = CoopProtocol::kProtocolVersion;
    packet.type = static_cast<uint16_t>(
        CoopProtocol::PacketType::DialogueLease);
    packet.worldEpoch = m_localWorldEpoch;
    packet.hostSaveKeyHash = CurrentHostSaveKeyHash();
    packet.areaId = m_localLevelId;
    packet.sourcePeerHash = GetLocalAccountToken();
    packet.targetPeerHash = 0;
    packet.ownerPeerHash = packet.sourcePeerHash;
    packet.leaseEpoch = 0;
    packet.command = static_cast<uint16_t>(
        CoopProtocol::DialogueLeaseCommand::Request);

    if (packet.hostSaveKeyHash == 0 ||
        packet.areaId == 0 ||
        packet.sourcePeerHash == 0)
    {
        return false;
    }

    packet.eventId = BuildDialogueLeaseEventId(
        packet.sourcePeerHash,
        packet.dialogueId,
        packet.sequence,
        packet.command);

    if (!SendDialogueLeaseTo(
            packet,
            m_remoteAddress,
            m_remotePort,
            "dialogue request failed"))
    {
        return false;
    }

    m_dialogueLeasePendingId = packet.dialogueId;
    m_dialogueLeaseTriggerEventId =
        packet.triggerEventId;
    m_dialogueLeaseRuleId = packet.ruleId;
    m_dialogueLeaseDescriptor = packet;
    m_dialogueLeasePendingCompletion = false;
    m_dialogueLeaseSeconds = 0.0f;
    m_dialogueLeaseActivitySendSeconds = 0.0f;
    m_lastDialogueLeaseEvent =
        "requested_dialogue_" +
        std::to_string(packet.dialogueId) +
        "_trigger_" +
        std::to_string(packet.triggerEventId) +
        "_reason_" +
        (reason && reason[0] ? reason : "-");
    return true;
}

void ModMain::ObserveLocalDialogueTrigger(
    ArkSpeakerBase* speaker,
    ArkConversation* conversation,
    uint64_t ruleId,
    bool ignoreVoiceRequirement,
    const char* concept,
    const ArkResponseQuery* query,
    int paChannel,
    bool isLiveAudio,
    int priority)
{
    if (IsReplayingRemoteDialogue() ||
        m_networkMode == CoopNetworkMode::Off ||
        !m_hasRemoteEndpoint ||
        !IsSessionGameplayReady())
    {
        return;
    }

    CoopSerialSequence::Advance(
        m_dialogueLeaseSequence);

    CoopProtocol::DialogueLeasePacket packet;
    std::string detail;
    if (!CaptureDialogueTriggerPacket(
            packet,
            speaker,
            conversation,
            ruleId,
            ignoreVoiceRequirement,
            concept,
            query,
            paChannel,
            isLiveAudio,
            priority,
            detail))
    {
        ++m_dialogueLeaseDropped;
        m_lastDialogueLeaseEvent =
            "dialogue_capture_failed_" + detail;
        return;
    }

    const uint64_t localPeer =
        GetLocalAccountToken();

    if ((m_dialogueLeaseDialogueId ==
                packet.dialogueId &&
            m_dialogueLeaseOwnerHash != 0) ||
        m_dialogueLeasePendingId ==
            packet.dialogueId)
    {
        // A later TriggerRule inside the same native conversation is activity,
        // not a second network-owned dialogue instance.
        if (m_dialogueLeaseOwnerHash == localPeer &&
            m_dialogueLeaseDialogueId ==
                packet.dialogueId)
        {
            m_dialogueLeaseSeconds = 0.0f;
            if (packet.expectedResponseId != 0)
            {
                m_dialogueLeaseDescriptor
                    .expectedResponseId =
                    packet.expectedResponseId;
            }
        }

        m_lastDialogueLeaseEvent =
            "dialogue_trigger_coalesced_" +
            std::to_string(packet.dialogueId);
        return;
    }

    if (m_dialogueLeaseOwnerHash != 0 ||
        m_dialogueLeasePendingId != 0)
    {
        // Preserve the existing single active lease contract. A future
        // multi-dialogue map can lift this without changing the packet format.
        ++m_dialogueLeaseDenied;
        m_lastDialogueLeaseEvent =
            "dialogue_trigger_blocked_by_active_" +
            std::to_string(
                m_dialogueLeaseDialogueId != 0
                    ? m_dialogueLeaseDialogueId
                    : m_dialogueLeasePendingId) +
            "_candidate_" +
            std::to_string(packet.dialogueId);
        return;
    }

    if (m_networkMode == CoopNetworkMode::Host)
    {
        ++m_dialogueLeaseEpoch;
        if (m_dialogueLeaseEpoch == 0)
            ++m_dialogueLeaseEpoch;

        packet.ownerPeerHash = localPeer;
        packet.leaseEpoch = m_dialogueLeaseEpoch;
        packet.command =
            static_cast<uint16_t>(
                CoopProtocol::DialogueLeaseCommand::Grant);
        packet.eventId =
            BuildDialogueLeaseEventId(
                localPeer,
                packet.dialogueId,
                packet.sequence,
                packet.command);

        m_dialogueLeaseDialogueId =
            packet.dialogueId;
        m_dialogueLeasePendingId = 0;
        m_dialogueLeaseOwnerHash = localPeer;
        m_dialogueLeaseTriggerEventId =
            packet.triggerEventId;
        m_dialogueLeaseRuleId = packet.ruleId;
        m_dialogueLeaseDescriptor = packet;
        m_dialogueLeaseSeconds = 0.0f;
        m_dialogueLeaseActivitySendSeconds = 0.0f;
        m_dialogueLeasePendingCompletion = false;
        m_appliedDialogueTriggerIds.insert(
            packet.triggerEventId);

        SendDialogueLeaseTo(
            packet,
            m_remoteAddress,
            m_remotePort,
            "dialogue host grant failed");

        QueueLocalStoryEventForHook(
            CoopProtocol::kStoryEventConversationStarted,
            packet.dialogueId,
            1,
            0,
            "ArkSpeakerBase::TriggerRule lease grant");
        if (packet.expectedResponseId != 0)
        {
            QueueLocalStoryEventForHook(
                CoopProtocol::kStoryEventDialogueLine,
                packet.expectedResponseId,
                1,
                0,
                "ArkSpeakerBase::TriggerRule lease grant");
        }

        ++m_dialogueLeaseApplied;
        m_lastDialogueLeaseEvent =
            "host_granted_dialogue_" +
            std::to_string(packet.dialogueId) +
            "_trigger_" +
            std::to_string(packet.triggerEventId);
        return;
    }

    RequestDialogueLease(
        packet,
        "ArkSpeakerBase::TriggerRule");
}

void ModMain::ObserveLocalDialogueActivity(
    uint64_t dialogueId,
    uint64_t lineId,
    const char* reason)
{
    if (dialogueId == 0 ||
        IsReplayingRemoteDialogue())
    {
        return;
    }

    const uint64_t localPeer =
        GetLocalAccountToken();
    if (m_dialogueLeaseDialogueId != dialogueId ||
        m_dialogueLeaseOwnerHash != localPeer)
    {
        return;
    }

    m_dialogueLeaseSeconds = 0.0f;

    if (lineId != 0)
    {
        m_dialogueLeaseDescriptor
            .expectedResponseId = lineId;
        QueueLocalStoryEventForHook(
            CoopProtocol::kStoryEventDialogueLine,
            lineId,
            1,
            0,
            reason);
    }
}

void ModMain::OnLocalDialogueCompleted(
    uint64_t dialogueId,
    bool complete,
    const char* reason)
{
    if (dialogueId == 0 ||
        IsReplayingRemoteDialogue())
    {
        return;
    }

    const uint64_t localPeer =
        GetLocalAccountToken();

    if (m_dialogueLeasePendingId == dialogueId)
    {
        // A very short client dialogue may finish before its Grant round trip.
        // Release it immediately when the authoritative Grant arrives.
        m_dialogueLeasePendingCompletion = true;
        return;
    }

    if (m_dialogueLeaseDialogueId != dialogueId ||
        m_dialogueLeaseOwnerHash != localPeer)
    {
        return;
    }

    QueueLocalStoryEventForHook(
        CoopProtocol::kStoryEventConversationEnded,
        dialogueId,
        complete ? 1 : 0,
        0,
        reason);
    ReleaseLocalDialogueLease(reason);
}

bool ModMain::ReleaseLocalDialogueLease(
    const char* reason)
{
    const uint64_t localPeer =
        GetLocalAccountToken();

    if (m_dialogueLeaseDialogueId == 0 ||
        m_dialogueLeaseOwnerHash != localPeer)
    {
        return false;
    }

    const uint64_t dialogueId =
        m_dialogueLeaseDialogueId;

    CoopSerialSequence::Advance(
        m_dialogueLeaseSequence);

    CoopProtocol::DialogueLeasePacket packet;
    if (!BuildDialogueLeasePacket(
            packet,
            CoopProtocol::DialogueLeaseCommand::Release,
            dialogueId))
    {
        return false;
    }

    packet.ownerPeerHash = localPeer;
    packet.triggerEventId =
        m_dialogueLeaseTriggerEventId;

    const bool sent = SendDialogueLeaseTo(
        packet,
        m_remoteAddress,
        m_remotePort,
        "dialogue release failed");

    m_lastDialogueLeaseEvent =
        "released_dialogue_" +
        std::to_string(dialogueId) +
        "_trigger_" +
        std::to_string(
            m_dialogueLeaseTriggerEventId) +
        "_reason_" +
        (reason && reason[0] ? reason : "-");

    ClearActiveDialogueLease(
        "local dialogue release");
    return sent;
}

ArkSpeakerBase* ModMain::ResolveDialogueSpeaker(
    uint64_t speakerEntityGuid,
    uint64_t speakerCharacterId,
    uint16_t flags,
    int32_t paChannel,
    std::string& detail)
{
    detail.clear();

    const auto speakerUsable =
        [](ArkSpeakerBase* speaker)
        {
            return speaker &&
                IsLikelyRuntimeCppObject(
                    speaker,
                    sizeof(void*) * 4);
        };

    const uint16_t specialSpeaker =
        flags & CoopProtocol::kDialogueSpecialSpeakerMask;
    if (specialSpeaker != 0)
    {
        if ((specialSpeaker & (specialSpeaker - 1)) != 0)
        {
            detail = "dialogue_special_speaker_flags_invalid";
            return nullptr;
        }

        ArkSpeakerBase* resolved = nullptr;
        if (specialSpeaker ==
            CoopProtocol::kDialogueSpeakerPA)
        {
            if (!gEnv || !gEnv->pGame ||
                paChannel < 0 || paChannel >= 4)
            {
                detail = "dialogue_pa_speaker_unavailable";
                return nullptr;
            }

            IArkPADialogManager* interfaceManager =
                gEnv->pGame->GetIArkPADialogManager();
            CArkPADialogManager* manager =
                static_cast<CArkPADialogManager*>(
                    interfaceManager);
            if (manager)
            {
                resolved = &manager->m_speakers[
                    static_cast<size_t>(paChannel)];
            }
        }
        else if (ArkPlayer::GetInstancePtr())
        {
            ArkPlayerComponent& playerComponent =
                ArkPlayer::GetInstance().m_playerComponent;
            switch (specialSpeaker)
            {
            case CoopProtocol::kDialogueSpeakerPlayer:
                resolved =
                    playerComponent.m_pPlayerSpeaker.get();
                break;
            case CoopProtocol::kDialogueSpeakerSuit:
                resolved =
                    playerComponent.m_pSuitSpeaker.get();
                break;
            case CoopProtocol::kDialogueSpeakerDiscRifle:
                resolved = playerComponent
                    .m_pDiscRifleSpeaker.get();
                break;
            case CoopProtocol::kDialogueSpeakerTranscribe:
                resolved = playerComponent
                    .m_pTranscribeSpeaker.get();
                break;
            default:
                break;
            }
        }

        if (speakerUsable(resolved))
            return resolved;

        detail =
            "dialogue_special_speaker_not_resolved_flags_" +
            std::to_string(specialSpeaker);
        return nullptr;
    }

    const bool voiceIdentity =
        (flags &
            CoopProtocol::kDialogueSpeakerVoiceIdentity) != 0;

    const auto speakerMatchesIdentity =
        [&](ArkSpeakerBase* speaker)
        {
            if (!speakerUsable(speaker))
                return false;

            if (speakerEntityGuid != 0)
            {
                IEntity* entity = nullptr;
                uint64_t guid = 0;
                return TryGuardedCall(
                        "dialogue cached speaker entity",
                        [speaker]()
                        {
                            return speaker->m_pEntity;
                        },
                        entity,
                        nullptr) &&
                    entity &&
                    TryGuardedCall(
                        "dialogue cached speaker guid",
                        [entity]()
                        {
                            return static_cast<uint64_t>(
                                entity->GetGuid());
                        },
                        guid,
                        nullptr) &&
                    guid == speakerEntityGuid;
            }

            if (voiceIdentity)
            {
                uint64_t voiceId = 0;
                return speakerCharacterId != 0 &&
                    TryGuardedCall(
                        "dialogue cached speaker voice",
                        [speaker]()
                        {
                            return speaker->m_voiceId != 0
                                ? speaker->m_voiceId
                                : speaker->m_playbackVoice;
                        },
                        voiceId,
                        nullptr) &&
                    voiceId == speakerCharacterId;
            }

            const ArkCharacter* character = nullptr;
            uint64_t characterId = 0;
            return speakerCharacterId != 0 &&
                TryGuardedCall(
                    "dialogue cached speaker character",
                    [speaker]()
                    {
                        return speaker->m_pCharacter;
                    },
                    character,
                    nullptr) &&
                character &&
                TryGuardedCall(
                    "dialogue cached speaker character id",
                    [character]()
                    {
                        return character->m_ID;
                    },
                    characterId,
                    nullptr) &&
                characterId == speakerCharacterId;
        };

    if (speakerEntityGuid != 0)
    {
        const auto found =
            m_dialogueSpeakersByGuid.find(
                speakerEntityGuid);
        if (found !=
                m_dialogueSpeakersByGuid.end())
        {
            if (speakerMatchesIdentity(found->second))
                return found->second;
            m_dialogueSpeakersByGuid.erase(found);
        }
    }

    if (speakerCharacterId != 0 && !voiceIdentity)
    {
        const auto found =
            m_dialogueSpeakersByCharacterId.find(
                speakerCharacterId);
        if (found !=
                m_dialogueSpeakersByCharacterId.end())
        {
            if (speakerMatchesIdentity(found->second))
                return found->second;
            m_dialogueSpeakersByCharacterId.erase(found);
        }
    }

    if (!gEnv ||
        !gEnv->pEntitySystem ||
        !gEnv->pGame)
    {
        detail = "dialogue_runtime_unavailable";
        return nullptr;
    }

    IGameFramework* framework =
        gEnv->pGame->GetIGameFramework();
    if (!framework)
    {
        detail = "dialogue_framework_unavailable";
        return nullptr;
    }

    auto speakerFromEntity =
        [&](IEntity* entity) -> ArkSpeakerBase*
        {
            if (!entity)
                return nullptr;

            IGameObject* gameObject = nullptr;
            if (!TryGuardedCall(
                    "dialogue resolve GetGameObject",
                    [framework, entity]()
                    {
                        return framework->GetGameObject(
                            entity->GetId());
                    },
                    gameObject,
                    nullptr) ||
                !gameObject)
            {
                return nullptr;
            }

            IGameObjectExtension* extension = nullptr;
            const char* extensionNames[] = {
                "ArkSpeaker",
                "ArkSpeakerExtension"
            };

            for (const char* extensionName :
                 extensionNames)
            {
                IGameObjectSystem::ExtensionID
                    extensionId = 0;
                if (TryGuardedCall(
                        "dialogue resolve GetExtensionId",
                        [gameObject, extensionName]()
                        {
                            return gameObject->GetExtensionId(
                                extensionName);
                        },
                        extensionId,
                        nullptr) &&
                    extensionId != 0)
                {
                    TryGuardedCall(
                        "dialogue resolve QueryExtension id",
                        [gameObject, extensionId]()
                        {
                            return gameObject->QueryExtension(
                                extensionId);
                        },
                        extension,
                        nullptr);
                }

                if (!extension)
                {
                    TryGuardedCall(
                        "dialogue resolve QueryExtension name",
                        [gameObject, extensionName]()
                        {
                            return gameObject->QueryExtension(
                                extensionName);
                        },
                        extension,
                        nullptr);
                }

                if (extension)
                    break;
            }

            if (!extension)
                return nullptr;

            ArkSpeakerExtension* speakerExtension =
                static_cast<ArkSpeakerExtension*>(
                    extension);
            ArkSpeakerBase* resolved =
                speakerExtension
                    ? speakerExtension->m_pSpeaker.get()
                    : nullptr;
            return speakerUsable(resolved)
                ? resolved
                : nullptr;
        };

    if (speakerEntityGuid != 0)
    {
        EntityId entityId = INVALID_ENTITYID;
        if (TryGuardedCall(
                "dialogue resolve FindEntityByGuid",
                [speakerEntityGuid]()
                {
                    return gEnv->pEntitySystem
                        ->FindEntityByGuid(
                            static_cast<EntityGUID>(
                                speakerEntityGuid));
                },
                entityId,
                nullptr) &&
            entityId != INVALID_ENTITYID)
        {
            IEntity* entity =
                gEnv->pEntitySystem->GetEntity(
                    entityId);
            if (ArkSpeakerBase* speaker =
                    speakerFromEntity(entity))
            {
                m_dialogueSpeakersByGuid[
                    speakerEntityGuid] = speaker;
                if (speakerCharacterId != 0)
                {
                    m_dialogueSpeakersByCharacterId[
                        speakerCharacterId] = speaker;
                }
                return speaker;
            }
        }
    }

    if (speakerCharacterId == 0)
    {
        detail = "dialogue_speaker_guid_not_resolved";
        return nullptr;
    }

    IEntityIt* iterator =
        gEnv->pEntitySystem->GetEntityIterator();
    if (!iterator)
    {
        detail = "dialogue_entity_iterator_unavailable";
        return nullptr;
    }

    ArkSpeakerBase* result = nullptr;
    iterator->MoveFirst();
    while (!iterator->IsEnd())
    {
        IEntity* entity = iterator->Next();
        ArkSpeakerBase* candidate =
            speakerFromEntity(entity);
        if (!candidate)
            continue;

        if (voiceIdentity)
        {
            uint64_t voiceId = 0;
            if (TryGuardedCall(
                    "dialogue resolve candidate voice",
                    [candidate]()
                    {
                        return candidate->m_voiceId != 0
                            ? candidate->m_voiceId
                            : candidate->m_playbackVoice;
                    },
                    voiceId,
                    nullptr) &&
                voiceId == speakerCharacterId)
            {
                result = candidate;
                break;
            }
            continue;
        }

        const ArkCharacter* character = nullptr;
        uint64_t characterId = 0;
        if (TryGuardedCall(
                "dialogue resolve candidate character",
                [candidate]()
                {
                    return candidate->m_pCharacter;
                },
                character,
                nullptr) &&
            character &&
            TryGuardedCall(
                "dialogue resolve candidate character id",
                [character]()
                {
                    return character->m_ID;
                },
                characterId,
                nullptr) &&
            characterId == speakerCharacterId)
        {
            result = candidate;
            if (entity)
            {
                uint64_t guid = 0;
                TryGuardedCall(
                    "dialogue resolve candidate guid",
                    [entity]()
                    {
                        return static_cast<uint64_t>(
                            entity->GetGuid());
                    },
                    guid,
                    nullptr);
                if (guid != 0)
                {
                    m_dialogueSpeakersByGuid[
                        guid] = candidate;
                }
            }
            break;
        }
    }
    iterator->Release();

    if (result)
    {
        if (!voiceIdentity)
        {
            m_dialogueSpeakersByCharacterId[
                speakerCharacterId] = result;
        }
        return result;
    }

    detail =
        "dialogue_speaker_not_resolved_guid_" +
        std::to_string(speakerEntityGuid) +
        "_character_" +
        std::to_string(speakerCharacterId);
    return nullptr;
}

bool ModMain::ReplayGrantedDialogue(
    const CoopProtocol::DialogueLeasePacket& packet,
    std::string& detail)
{
    detail.clear();

    if (!DialoguePacketCarriesTrigger(packet) ||
        !DialoguePacketConceptIsValid(packet))
    {
        detail = "invalid_dialogue_trigger_packet";
        return false;
    }

    if (m_appliedDialogueTriggerIds.find(
            packet.triggerEventId) !=
        m_appliedDialogueTriggerIds.end())
    {
        detail = "dialogue_trigger_already_applied";
        return true;
    }

    ArkSpeakerBase* speaker =
        ResolveDialogueSpeaker(
            packet.speakerEntityGuid,
            packet.speakerCharacterId,
            packet.flags,
            packet.paChannel,
            detail);
    if (!speaker)
        return false;

    ArkResponseQuery rebuiltQuery;
    ArkResponseQuery* query = nullptr;
    if ((packet.flags &
            CoopProtocol::kDialogueHasQuery) != 0)
    {
        for (uint16_t index = 0;
             index < packet.queryFactCount;
             ++index)
        {
            ArkResponseFact fact;
            if (!DecodeDialogueQueryFact(
                    packet.queryFacts[index],
                    fact))
            {
                detail =
                    "dialogue_query_decode_failed_" +
                    std::to_string(index);
                return false;
            }
            rebuiltQuery.m_facts.push_back(
                std::move(fact));
        }
        query = &rebuiltQuery;
    }

    const bool ignoreVoiceRequirement =
        (packet.flags &
            CoopProtocol::kDialogueIgnoreVoiceRequirement) != 0;
    const bool isLiveAudio =
        (packet.flags &
            CoopProtocol::kDialogueLiveAudio) != 0;
    const char* concept =
        (packet.flags &
            CoopProtocol::kDialogueHasConcept) != 0
            ? packet.conceptText
            : nullptr;

    ++m_remoteDialogueReplayDepth;

    ArkConversation* conversation = nullptr;
    std::string guardReason;
    const bool invoked = TryGuardedCall(
        "remote dialogue TriggerRule",
        [&]()
        {
            return InvokeOriginalDialogueTrigger(
                speaker,
                packet.ruleId,
                ignoreVoiceRequirement,
                concept,
                query,
                packet.paChannel,
                isLiveAudio,
                packet.priority);
        },
        conversation,
        &guardReason);

    if (m_remoteDialogueReplayDepth != 0)
        --m_remoteDialogueReplayDepth;

    if (!invoked || !conversation)
    {
        detail =
            "remote_dialogue_trigger_failed_" +
            (guardReason.empty()
                ? std::string("no_conversation")
                : guardReason);
        return false;
    }

    uint64_t runtimeConversationId = 0;
    TryGuardedCall(
        "remote dialogue runtime id",
        [conversation]()
        {
            return conversation->m_conversationId;
        },
        runtimeConversationId,
        nullptr);

    const bool completedDuringReplay =
        runtimeConversationId != 0 &&
        m_remoteDialogueCompletedDuringReplayIds.erase(
            runtimeConversationId) != 0;

    if (runtimeConversationId != 0 &&
        !completedDuringReplay)
    {
        BindDialogueRuntimeId(
            runtimeConversationId,
            packet.dialogueId);
        m_remoteDialogueRuntimeIds.insert(
            runtimeConversationId);
        m_remoteDialogueStoryIds.insert(
            packet.dialogueId);
    }

    m_appliedDialogueTriggerIds.insert(
        packet.triggerEventId);

    const ArkResponse* selectedResponse = nullptr;
    uint64_t selectedResponseId = 0;
    TryGuardedCall(
        "remote dialogue selected response",
        [speaker]()
        {
            return speaker->m_pCurrentResponse;
        },
        selectedResponse,
        nullptr);
    if (selectedResponse)
    {
        TryGuardedCall(
            "remote dialogue selected response id",
            [selectedResponse]()
            {
                return selectedResponse->m_id;
            },
            selectedResponseId,
            nullptr);
    }

    detail =
        "triggered_native_dialogue_" +
        std::to_string(packet.dialogueId) +
        "_rule_" +
        std::to_string(packet.ruleId) +
        "_response_" +
        std::to_string(selectedResponseId) +
        "_expected_" +
        std::to_string(packet.expectedResponseId) +
        "_completedInline_" +
        std::to_string(completedDuringReplay ? 1 : 0);
    return true;
}

bool ModMain::ShouldSuppressDialogueStoryEvent(
    uint64_t id) const
{
    if (m_remoteDialogueReplayDepth != 0)
        return true;

    if (id != 0 &&
        m_remoteDialogueStoryIds.find(id) !=
            m_remoteDialogueStoryIds.end())
    {
        return true;
    }

    return
        id != 0 &&
        m_dialogueLeaseDialogueId == id &&
        m_dialogueLeaseOwnerHash != 0 &&
        m_dialogueLeaseOwnerHash !=
            GetLocalAccountToken();
}

bool ModMain::ShouldSuppressRemoteDialogueRetrigger(
    uint64_t ruleId)
{
    const bool suppress =
        ruleId != 0 &&
        m_dialogueLeaseRuleId == ruleId &&
        m_dialogueLeaseOwnerHash != 0 &&
        m_dialogueLeaseOwnerHash != GetLocalAccountToken();
    if (!suppress)
        return false;

    // The observer already replayed this exact native TriggerRule. Invoking it
    // again stops the active transcribe/call and restarts it from time zero.
    // Different rules remain available for real continuation lines.
    m_dialogueLeaseSeconds = 0.0f;
    m_lastDialogueLeaseEvent =
        "suppressed_active_remote_dialogue_retrigger_" +
        std::to_string(m_dialogueLeaseDialogueId) +
        "_rule_" + std::to_string(ruleId);
    return true;
}

void ModMain::HandleDialogueLease(
    const CoopProtocol::DialogueLeasePacket& packet)
{
    ++m_dialogueLeaseReceived;

    const uint64_t localPeer =
        GetLocalAccountToken();
    const auto command =
        static_cast<
            CoopProtocol::DialogueLeaseCommand>(
                packet.command);

    const bool sourceMatchesTransport =
        m_activePacketSourceAccountToken == 0 ||
        m_activePacketSourceAccountToken ==
            packet.sourcePeerHash;

    if (packet.magic != CoopProtocol::kPacketMagic ||
        packet.version !=
            CoopProtocol::kProtocolVersion ||
        packet.type !=
            static_cast<uint16_t>(
                CoopProtocol::PacketType::DialogueLease) ||
        packet.dialogueId == 0 ||
        packet.eventId == 0 ||
        packet.sourcePeerHash == 0 ||
        packet.worldEpoch != m_localWorldEpoch ||
        packet.areaId != m_localLevelId ||
        !IsCurrentOrRecentHostSaveKeyHash(
            packet.hostSaveKeyHash) ||
        (packet.targetPeerHash != 0 &&
            packet.targetPeerHash != localPeer) ||
        !sourceMatchesTransport ||
        packet.queryFactCount >
            CoopProtocol::kMaxDialogueQueryFacts ||
        !DialoguePacketConceptIsValid(packet))
    {
        ++m_dialogueLeaseDropped;
        m_lastDialogueLeaseEvent =
            "guard_drop_command_" +
            std::to_string(packet.command);
        return;
    }

    if (m_appliedDialogueLeaseEventIds.find(
            packet.eventId) !=
        m_appliedDialogueLeaseEventIds.end())
    {
        m_lastDialogueLeaseEvent =
            "duplicate_dialogue_command_" +
            std::to_string(packet.command) +
            "_event_" +
            std::to_string(packet.eventId);
        return;
    }

    auto setActiveLease =
        [&](const CoopProtocol::DialogueLeasePacket& grant)
        {
            m_dialogueLeaseDialogueId =
                grant.dialogueId;
            m_dialogueLeasePendingId = 0;
            m_dialogueLeaseOwnerHash =
                grant.ownerPeerHash;
            m_dialogueLeaseTriggerEventId =
                grant.triggerEventId;
            m_dialogueLeaseRuleId =
                grant.ruleId;
            m_dialogueLeaseEpoch =
                grant.leaseEpoch;
            m_dialogueLeaseDescriptor =
                grant;
            m_dialogueLeaseSeconds = 0.0f;
            m_dialogueLeaseActivitySendSeconds = 0.0f;
        };

    auto replayOrDefer =
        [&](const CoopProtocol::DialogueLeasePacket& grant)
        {
            if (grant.ownerPeerHash == localPeer)
            {
                const bool firstOwnerApply =
                    m_appliedDialogueTriggerIds.insert(
                        grant.triggerEventId).second;

                if (firstOwnerApply)
                {
                    QueueLocalStoryEventForHook(
                        CoopProtocol::kStoryEventConversationStarted,
                        grant.dialogueId,
                        1,
                        0,
                        "dialogue lease owner grant");
                    if (grant.expectedResponseId != 0)
                    {
                        QueueLocalStoryEventForHook(
                            CoopProtocol::kStoryEventDialogueLine,
                            grant.expectedResponseId,
                            1,
                            0,
                            "dialogue lease owner grant");
                    }
                }

                if (m_dialogueLeasePendingCompletion)
                {
                    m_dialogueLeasePendingCompletion = false;
                    ReleaseLocalDialogueLease(
                        "dialogue completed before grant");
                }
                return;
            }

            std::string replayDetail;
            if (!ReplayGrantedDialogue(
                    grant,
                    replayDetail))
            {
                m_pendingRemoteDialogueReplay =
                    grant;
                m_pendingRemoteDialogueReplayActive =
                    true;
                m_pendingRemoteDialogueReplaySeconds =
                    kDialogueReplayRetrySeconds;
                m_lastDialogueLeaseEvent =
                    "dialogue_replay_deferred_" +
                    replayDetail;
                return;
            }

            m_pendingRemoteDialogueReplayActive =
                false;
            m_pendingRemoteDialogueReplay = {};
            m_pendingRemoteDialogueReplaySeconds = 0.0f;
            m_lastDialogueLeaseEvent =
                replayDetail;
        };

    if (command ==
        CoopProtocol::DialogueLeaseCommand::Request)
    {
        if (m_networkMode !=
                CoopNetworkMode::Host ||
            !DialoguePacketCarriesTrigger(packet))
        {
            ++m_dialogueLeaseDropped;
            return;
        }

        if (m_dialogueLeaseOwnerHash != 0)
        {
            if (m_dialogueLeaseOwnerHash ==
                    packet.sourcePeerHash &&
                m_dialogueLeaseTriggerEventId ==
                    packet.triggerEventId)
            {
                CoopSerialSequence::Advance(
                    m_dialogueLeaseSequence);
                CoopProtocol::DialogueLeasePacket
                    response =
                        m_dialogueLeaseDescriptor;
                response.sequence =
                    m_dialogueLeaseSequence;
                response.sourcePeerHash =
                    localPeer;
                response.targetPeerHash = 0;
                response.ownerPeerHash =
                    m_dialogueLeaseOwnerHash;
                response.leaseEpoch =
                    m_dialogueLeaseEpoch;
                response.command =
                    static_cast<uint16_t>(
                        CoopProtocol::DialogueLeaseCommand::Grant);
                response.eventId =
                    BuildDialogueLeaseEventId(
                        localPeer,
                        response.dialogueId,
                        response.sequence,
                        response.command);
                SendDialogueLeaseTo(
                    response,
                    m_remoteAddress,
                    m_remotePort,
                    "dialogue grant resend failed");
                m_appliedDialogueLeaseEventIds.insert(
                    packet.eventId);
                return;
            }

            CoopSerialSequence::Advance(
                m_dialogueLeaseSequence);
            CoopProtocol::DialogueLeasePacket
                response = packet;
            response.sequence =
                m_dialogueLeaseSequence;
            response.sourcePeerHash = localPeer;
            response.targetPeerHash =
                packet.sourcePeerHash;
            response.ownerPeerHash =
                m_dialogueLeaseOwnerHash;
            response.leaseEpoch =
                m_dialogueLeaseEpoch;
            response.command =
                static_cast<uint16_t>(
                    CoopProtocol::DialogueLeaseCommand::Deny);
            response.eventId =
                BuildDialogueLeaseEventId(
                    localPeer,
                    response.dialogueId,
                    response.sequence,
                    response.command);
            SendDialogueLeaseTo(
                response,
                m_remoteAddress,
                m_remotePort,
                "dialogue deny failed");
            ++m_dialogueLeaseDenied;
            m_lastDialogueLeaseEvent =
                "denied_dialogue_" +
                std::to_string(packet.dialogueId);
            m_appliedDialogueLeaseEventIds.insert(
                packet.eventId);
            return;
        }

        ++m_dialogueLeaseEpoch;
        if (m_dialogueLeaseEpoch == 0)
            ++m_dialogueLeaseEpoch;

        CoopSerialSequence::Advance(
            m_dialogueLeaseSequence);
        CoopProtocol::DialogueLeasePacket
            response = packet;
        response.sequence =
            m_dialogueLeaseSequence;
        response.sourcePeerHash = localPeer;
        response.targetPeerHash = 0;
        response.ownerPeerHash =
            packet.sourcePeerHash;
        response.leaseEpoch =
            m_dialogueLeaseEpoch;
        response.command =
            static_cast<uint16_t>(
                CoopProtocol::DialogueLeaseCommand::Grant);
        response.eventId =
            BuildDialogueLeaseEventId(
                localPeer,
                response.dialogueId,
                response.sequence,
                response.command);

        setActiveLease(response);
        SendDialogueLeaseTo(
            response,
            m_remoteAddress,
            m_remotePort,
            "dialogue grant failed");
        replayOrDefer(response);

        ++m_dialogueLeaseApplied;
        m_lastDialogueLeaseEvent =
            "granted_remote_dialogue_" +
            std::to_string(response.dialogueId) +
            "_trigger_" +
            std::to_string(
                response.triggerEventId);
        m_appliedDialogueLeaseEventIds.insert(
            packet.eventId);
        return;
    }

    if (command ==
        CoopProtocol::DialogueLeaseCommand::Grant)
    {
        if (m_networkMode !=
                CoopNetworkMode::Client ||
            packet.leaseEpoch == 0 ||
            packet.ownerPeerHash == 0 ||
            !DialoguePacketCarriesTrigger(packet))
        {
            ++m_dialogueLeaseDropped;
            return;
        }

        if (m_dialogueLeaseOwnerHash != 0 &&
            (packet.leaseEpoch <
                    m_dialogueLeaseEpoch ||
                (packet.leaseEpoch ==
                        m_dialogueLeaseEpoch &&
                    packet.triggerEventId !=
                        m_dialogueLeaseTriggerEventId)))
        {
            ++m_dialogueLeaseDropped;
            return;
        }

        setActiveLease(packet);
        replayOrDefer(packet);

        ++m_dialogueLeaseApplied;
        m_lastDialogueLeaseEvent =
            packet.ownerPeerHash == localPeer
                ? "grant_owner_no_replay_dialogue_" +
                    std::to_string(packet.dialogueId)
                : "grant_replayed_dialogue_" +
                    std::to_string(packet.dialogueId);
        m_appliedDialogueLeaseEventIds.insert(
            packet.eventId);
        return;
    }

    if (command ==
        CoopProtocol::DialogueLeaseCommand::Deny)
    {
        ++m_dialogueLeaseDenied;
        if (m_dialogueLeasePendingId ==
                packet.dialogueId &&
            (m_dialogueLeaseTriggerEventId == 0 ||
                packet.triggerEventId ==
                    m_dialogueLeaseTriggerEventId))
        {
            m_dialogueLeasePendingId = 0;
            m_dialogueLeaseTriggerEventId = 0;
            m_dialogueLeaseRuleId = 0;
            m_dialogueLeaseDescriptor = {};
            m_dialogueLeasePendingCompletion = false;
        }
        m_lastDialogueLeaseEvent =
            "deny_applied_dialogue_" +
            std::to_string(packet.dialogueId);
        m_appliedDialogueLeaseEventIds.insert(
            packet.eventId);
        return;
    }

    if (command ==
        CoopProtocol::DialogueLeaseCommand::Activity)
    {
        if (m_networkMode ==
                CoopNetworkMode::Host &&
            m_dialogueLeaseOwnerHash ==
                packet.sourcePeerHash &&
            m_dialogueLeaseDialogueId ==
                packet.dialogueId &&
            m_dialogueLeaseEpoch ==
                packet.leaseEpoch &&
            m_dialogueLeaseTriggerEventId ==
                packet.triggerEventId)
        {
            m_dialogueLeaseSeconds = 0.0f;
            ++m_dialogueLeaseApplied;
            m_appliedDialogueLeaseEventIds.insert(
                packet.eventId);
        }
        else
        {
            ++m_dialogueLeaseDropped;
        }
        return;
    }

    if (command ==
        CoopProtocol::DialogueLeaseCommand::Release)
    {
        if (m_networkMode ==
            CoopNetworkMode::Host)
        {
            if (m_dialogueLeaseOwnerHash !=
                    packet.sourcePeerHash ||
                m_dialogueLeaseDialogueId !=
                    packet.dialogueId ||
                m_dialogueLeaseEpoch !=
                    packet.leaseEpoch ||
                m_dialogueLeaseTriggerEventId !=
                    packet.triggerEventId)
            {
                ++m_dialogueLeaseDropped;
                return;
            }

            CoopSerialSequence::Advance(
                m_dialogueLeaseSequence);
            CoopProtocol::DialogueLeasePacket
                response =
                    m_dialogueLeaseDescriptor;
            response.sequence =
                m_dialogueLeaseSequence;
            response.sourcePeerHash = localPeer;
            response.targetPeerHash = 0;
            response.ownerPeerHash =
                m_dialogueLeaseOwnerHash;
            response.command =
                static_cast<uint16_t>(
                    CoopProtocol::DialogueLeaseCommand::Release);
            response.eventId =
                BuildDialogueLeaseEventId(
                    localPeer,
                    response.dialogueId,
                    response.sequence,
                    response.command);
            SendDialogueLeaseTo(
                response,
                m_remoteAddress,
                m_remotePort,
                "dialogue release broadcast failed");
        }
        else if (m_dialogueLeaseDialogueId != 0 &&
            (m_dialogueLeaseDialogueId !=
                    packet.dialogueId ||
                m_dialogueLeaseEpoch !=
                    packet.leaseEpoch ||
                m_dialogueLeaseTriggerEventId !=
                    packet.triggerEventId))
        {
            ++m_dialogueLeaseDropped;
            return;
        }

        m_appliedDialogueLeaseEventIds.insert(
            packet.eventId);
        ++m_dialogueLeaseApplied;
        m_lastDialogueLeaseEvent =
            "release_applied_dialogue_" +
            std::to_string(packet.dialogueId);
        ClearActiveDialogueLease(
            "remote dialogue release");
        return;
    }

    ++m_dialogueLeaseDropped;
}

void ModMain::ClearActiveDialogueLease(
    const char* reason)
{
    m_dialogueLeaseDialogueId = 0;
    m_dialogueLeasePendingId = 0;
    m_dialogueLeaseOwnerHash = 0;
    m_dialogueLeaseTriggerEventId = 0;
    m_dialogueLeaseRuleId = 0;
    m_dialogueLeaseSeconds = 0.0f;
    m_dialogueLeaseActivitySendSeconds = 0.0f;
    m_dialogueLeasePendingCompletion = false;
    m_dialogueLeaseDescriptor = {};
    m_pendingRemoteDialogueReplayActive = false;
    m_pendingRemoteDialogueReplay = {};
    m_pendingRemoteDialogueReplaySeconds = 0.0f;

    if (reason && reason[0])
        m_lastDialogueLeaseEvent = reason;
}

void ModMain::TickDialogueLease(float frameTime)
{
    if (frameTime <= 0.0f)
        return;

    if (!m_pendingDialogueWritebacks.empty())
    {
        m_pendingDialogueWritebackRetrySeconds -= frameTime;
        if (m_pendingDialogueWritebackRetrySeconds <= 0.0f)
        {
            m_pendingDialogueWritebackRetrySeconds = 0.25f;
            for (auto it = m_pendingDialogueWritebacks.begin();
                 it != m_pendingDialogueWritebacks.end();)
            {
                std::string detail;
                m_applyingRemoteStoryEvent = true;
                const bool applied =
                    ApplyStoryEventMutation(*it, detail);
                m_applyingRemoteStoryEvent = false;
                if (!applied)
                {
                    ++it;
                    continue;
                }

                if (it->eventId != 0)
                    m_appliedStoryEventIds.insert(it->eventId);
                m_storyRevision = std::max(
                    m_storyRevision,
                    it->postVersion != 0
                        ? it->postVersion
                        : m_storyRevision + 1);
                m_lastStoryEventId = it->eventId;
                ++m_appliedStoryEventPackets;
                m_lastStoryEvent =
                    "applied_deferred_story_event_" +
                    std::to_string(it->eventId) +
                    "_kind_" +
                    std::to_string(it->eventKind) +
                    "_target_" +
                    std::to_string(it->targetId) +
                    "_rev_" +
                    std::to_string(m_storyRevision) +
                    "_detail_" + detail;
                it = m_pendingDialogueWritebacks.erase(it);
            }
        }
    }

    if (m_pendingRemoteDialogueReplayActive)
    {
        m_pendingRemoteDialogueReplaySeconds -=
            frameTime;
        if (m_pendingRemoteDialogueReplaySeconds <=
                0.0f &&
            m_dialogueLeaseDialogueId ==
                m_pendingRemoteDialogueReplay
                    .dialogueId &&
            m_dialogueLeaseTriggerEventId ==
                m_pendingRemoteDialogueReplay
                    .triggerEventId)
        {
            std::string detail;
            if (ReplayGrantedDialogue(
                    m_pendingRemoteDialogueReplay,
                    detail))
            {
                m_pendingRemoteDialogueReplayActive =
                    false;
                m_pendingRemoteDialogueReplay = {};
                m_pendingRemoteDialogueReplaySeconds =
                    0.0f;
                m_lastDialogueLeaseEvent = detail;
            }
            else
            {
                m_pendingRemoteDialogueReplaySeconds =
                    kDialogueReplayRetrySeconds;
                m_lastDialogueLeaseEvent =
                    "dialogue_replay_retry_" +
                    detail;
            }
        }
    }

    if (m_dialogueLeaseOwnerHash == 0)
        return;

    m_dialogueLeaseSeconds += frameTime;

    const uint64_t localPeer =
        GetLocalAccountToken();

    if (m_dialogueLeaseOwnerHash == localPeer)
    {
        if (m_networkMode ==
            CoopNetworkMode::Client)
        {
            m_dialogueLeaseActivitySendSeconds +=
                frameTime;
            if (m_dialogueLeaseActivitySendSeconds >=
                kDialogueLeaseActivitySeconds)
            {
                CoopSerialSequence::Advance(
                    m_dialogueLeaseSequence);
                CoopProtocol::DialogueLeasePacket
                    activity;
                if (BuildDialogueLeasePacket(
                        activity,
                        CoopProtocol::DialogueLeaseCommand::Activity,
                        m_dialogueLeaseDialogueId))
                {
                    activity.triggerEventId =
                        m_dialogueLeaseTriggerEventId;
                    activity.ownerPeerHash =
                        localPeer;
                    SendDialogueLeaseTo(
                        activity,
                        m_remoteAddress,
                        m_remotePort,
                        "dialogue activity failed");
                }
                m_dialogueLeaseActivitySendSeconds =
                    0.0f;
            }
        }
        return;
    }

    if (m_networkMode !=
            CoopNetworkMode::Host ||
        m_dialogueLeaseSeconds <
            kDialogueLeaseTimeoutSeconds)
    {
        return;
    }

    CoopSerialSequence::Advance(
        m_dialogueLeaseSequence);
    CoopProtocol::DialogueLeasePacket release;
    if (BuildDialogueLeasePacket(
            release,
            CoopProtocol::DialogueLeaseCommand::Release,
            m_dialogueLeaseDialogueId))
    {
        release.ownerPeerHash =
            m_dialogueLeaseOwnerHash;
        release.triggerEventId =
            m_dialogueLeaseTriggerEventId;
        SendDialogueLeaseTo(
            release,
            m_remoteAddress,
            m_remotePort,
            "dialogue timeout release failed");
    }

    const uint64_t timedOutDialogue =
        m_dialogueLeaseDialogueId;
    ClearActiveDialogueLease(
        "dialogue lease timeout");
    m_lastDialogueLeaseEvent =
        "lease_timeout_dialogue_" +
        std::to_string(timedOutDialogue);
}

void ModMain::ResetDialogueLeaseState(
    const char* reason)
{
    m_dialogueLeaseDialogueId = 0;
    m_dialogueLeasePendingId = 0;
    m_dialogueLeaseOwnerHash = 0;
    m_dialogueLeaseTriggerEventId = 0;
    m_dialogueLeaseRuleId = 0;
    m_dialogueLeaseSequence = 0;
    m_dialogueLeaseEpoch = 0;
    m_dialogueLeaseSent = 0;
    m_dialogueLeaseReceived = 0;
    m_dialogueLeaseApplied = 0;
    m_dialogueLeaseDropped = 0;
    m_dialogueLeaseDenied = 0;
    m_remoteDialogueReplayDepth = 0;
    m_dialogueLeaseSeconds = 0.0f;
    m_dialogueLeaseActivitySendSeconds = 0.0f;
    m_pendingRemoteDialogueReplaySeconds = 0.0f;
    m_dialogueLeasePendingCompletion = false;
    m_pendingRemoteDialogueReplayActive = false;
    m_dialogueLeaseDescriptor = {};
    m_pendingRemoteDialogueReplay = {};
    m_appliedDialogueLeaseEventIds.clear();
    m_appliedDialogueTriggerIds.clear();
    m_remoteDialogueRuntimeIds.clear();
    m_remoteDialogueCompletedDuringReplayIds.clear();
    m_remoteDialogueStoryIds.clear();
    m_dialogueSpeakersByGuid.clear();
    m_dialogueSpeakersByCharacterId.clear();
    m_pendingDialogueWritebacks.clear();
    m_pendingDialogueWritebackRetrySeconds = 0.0f;
    m_dialogueStoryIdByRuntimeId.clear();
    m_lastDialogueLeaseEvent =
        reason && reason[0]
            ? reason
            : "reset";
}
