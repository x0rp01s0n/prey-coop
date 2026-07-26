#pragma once

#include <cstddef>
#include <cstdint>

namespace CoopProtocol
{
constexpr uint32_t kPacketMagic = 0x504F4F43; // "COOP" on little endian
constexpr uint16_t kProtocolVersion = 237;
constexpr uint32_t kModBuild = 20260725;
constexpr size_t kUsernameSize = 32;
constexpr size_t kPasswordSize = 32;
constexpr size_t kServerNameSize = 48;
constexpr size_t kLevelNameSize = 96;
constexpr size_t kSavePathSize = 96;
constexpr size_t kSaveKeySize = 64;
constexpr size_t kWeaponClassNameSize = 64;
constexpr size_t kMaxPacketSize = 1200;
constexpr size_t kReliablePayloadSize = 1164;
constexpr size_t kSaveTransferDataSize = 1024;
constexpr size_t kPlayerStateTransferDataSize = 960;
constexpr size_t kAreaJournalTransferDataSize = 956;
constexpr uint32_t kAreaJournalTransferFlagSnapshot = 1u << 0;
constexpr uint32_t kAreaJournalTransferFlagAuthorityHandoff = 1u << 1;
constexpr size_t kMimicModelPathSize = 192;
constexpr size_t kMaxEnemyDamageSignalValues = 32;
constexpr uint32_t kPlayerPoseStanceMask = 0x000000ffu;
constexpr uint32_t kPlayerPoseFlagMoving = 1u << 8;
constexpr uint32_t kPlayerPoseFlagDowned = 1u << 9;
constexpr uint32_t kPlayerPoseFlagWeaponEquipped = 1u << 10;
constexpr uint32_t kPlayerPoseWeaponClassShift = 11;
constexpr uint32_t kPlayerPoseWeaponClassMask = 0x00007800u;
constexpr uint32_t kPlayerPoseActionFire = 1u << 15;
constexpr uint32_t kPlayerPoseActionReload = 1u << 16;
constexpr uint32_t kPlayerPoseActionMelee = 1u << 17;
constexpr uint32_t kPlayerPoseActionSwitch = 1u << 18;
constexpr uint32_t kPlayerPoseFlagRunning = 1u << 19;
constexpr uint32_t kPlayerPoseActionPsi = 1u << 20;
constexpr uint32_t kPlayerPoseActionPsiImpact = 1u << 21;
constexpr uint32_t kPlayerPoseFlagMimicActive = 1u << 22;
constexpr uint32_t kPlayerPoseFlagCrawl = 1u << 23;
constexpr uint32_t kPlayerPoseFlagSlide = 1u << 24;
constexpr uint32_t kPlayerPoseFlagMimicVisual = 1u << 25;
constexpr uint32_t kPlayerPoseFlagHacking = 1u << 26;
constexpr uint32_t kPlayerPoseFlagZeroG = 1u << 27;
constexpr uint32_t kPlayerPoseWeaponClassPistol = 1u;
constexpr uint32_t kPlayerPoseWeaponClassShotgun = 2u;
constexpr uint32_t kPlayerPoseWeaponClassWrench = 3u;
constexpr uint32_t kPlayerPoseWeaponClassGooGun = 4u;
constexpr uint32_t kPlayerPoseWeaponClassStunGun = 5u;
constexpr uint32_t kPlayerPoseWeaponClassLaser = 6u;
constexpr uint32_t kPlayerPoseWeaponClassRecycler = 7u;
constexpr uint32_t kPlayerPoseWeaponClassEmp = 8u;
constexpr uint32_t kPlayerPoseWeaponClassNullwave = 9u;
constexpr uint32_t kPlayerPoseWeaponClassLure = 10u;
constexpr uint32_t kPlayerPoseWeaponClassToyGun = 11u;
constexpr uint16_t kPlayerPosePsiFxGeneric = 1u;
constexpr uint16_t kPlayerPosePsiFxKinetic = 2u;
constexpr uint16_t kPlayerPosePsiFxThermal = 3u;
constexpr uint16_t kPlayerPosePsiFxElectric = 4u;
constexpr uint16_t kPlayerPosePsiFxMind = 5u;
constexpr uint16_t kPlayerPosePsiFxShift = 6u;
constexpr uint16_t kPlayerPosePsiFxNullwave = 7u;
constexpr uint16_t kPlayerPosePsiFxMimic = 8u;
constexpr uint16_t kPlayerPosePsiFxThermalTrigger = 9u;
constexpr uint16_t kPlayerPosePsiFxShiftDuplicate = 10u;
constexpr uint16_t kPlayerPosePsiFxShiftArrive = 11u;
constexpr uint32_t kTestMimicSpawnFlagRequest = 1u << 0;
constexpr uint32_t kTestMimicSpawnFlagAuthority = 1u << 1;
constexpr uint32_t kTestMimicStateFlagDead = 1u << 0;
constexpr uint32_t kTestMimicStateFlagHealthKnown = 1u << 1;
constexpr uint32_t kTestMimicStateFlagDeathCommit = 1u << 2;
constexpr uint32_t kTestMimicStateFlagHitCommit = 1u << 3;
constexpr uint32_t kTestMimicStateFlagHidden = 1u << 4;
constexpr uint32_t kEnemyRosterFlagAlive = 1u << 0;
constexpr uint32_t kEnemyRosterFlagStoryCritical = 1u << 1;
constexpr uint32_t kEnemyRosterFlagDynamicSpawn = 1u << 2;
constexpr uint32_t kEnemyRosterFlagLevelAuthored = 1u << 3;
constexpr uint32_t kEnemyRosterFlagRemoved = 1u << 4;
constexpr uint32_t kEnemyRosterFlagRaisedFromCorpse = 1u << 5;
constexpr uint32_t kEnemyRosterFlagEthericDoppelganger = 1u << 6;
// CorpsePhantomRequest is the existing reliable child-NPC lifecycle channel.
// A flagged request carries the Etheric Phantom's temporary Doppelganger
// instead of the player Create Phantom result. `reserved` is its cast generation.
constexpr uint32_t kPhantomChildRequestFlagEthericDoppelganger = 1u << 0;
constexpr uint32_t kEnemyStateSourceFlagAuthorityClaim = 1u << 0;
constexpr uint32_t kEnemyStateSourceFlagAuthorityRelease = 1u << 1;
constexpr uint32_t kEnemyStateSourceFlagAuthoritySnapshot = 1u << 2;
constexpr uint32_t kEnemyStateSourceFlagAuthorityBlocked = 1u << 3;
constexpr uint32_t kEnemyStateSourceFlagAuthorityHasAttention = 1u << 4;
constexpr uint32_t kEnemyLocomotionFlagWalking = 1u << 0;
constexpr uint32_t kEnemyLocomotionFlagRunning = 1u << 1;
constexpr uint32_t kEnemyLocomotionFlagDashing = 1u << 2;
constexpr uint32_t kEnemyLocomotionFlagShifting = 1u << 3;
constexpr uint32_t kEnemyLocomotionFlagMorphing = 1u << 4;
constexpr uint32_t kEnemyLocomotionFlagAttacking = 1u << 5;
constexpr uint32_t kEnemyLocomotionFlagHitReacting = 1u << 6;
constexpr uint32_t kEnemyLocomotionFlagStunned = 1u << 7;
constexpr uint32_t kEnemyLocomotionFlagCowering = 1u << 8;
constexpr uint32_t kEnemyLocomotionFlagGlooed = 1u << 9;
constexpr uint32_t kEnemyLocomotionFlagTurning = 1u << 10;
constexpr uint32_t kEnemyLocomotionFlagMannequinDriven = 1u << 11;
constexpr uint32_t kEnemyLocomotionFlagInCombat = 1u << 12;
constexpr uint32_t kEnemyLocomotionFlagMindControlled = 1u << 13;
constexpr uint32_t kEnemyLocomotionFlagPsiSuppressed = 1u << 14;
// Authored non-Phantom burst displacement (Mimic pounce/side-step, Operator
// ram, etc.). It deliberately does not carry Phantom Shift/Dash visuals.
constexpr uint32_t kEnemyLocomotionFlagLunging = 1u << 15;
// Native ArkNpc ragdoll is a replicated shared-body state. It is separate
// from the broader HitReacting action flag so a remote observer cannot enter
// ragdoll from its own collision while local attention/combat is mixed in.
constexpr uint32_t kEnemyLocomotionFlagRagdolled = 1u << 16;
constexpr uint16_t kEnemyMannequinReservedCarryMovement = 1u << 0;
constexpr uint16_t kEnemyMannequinReservedHacked = 1u << 1;
constexpr uint16_t kEnemyMannequinReservedFactionValid = 1u << 2;
constexpr uint16_t kEnemyMannequinReservedCorrupted = 1u << 3;
// The authority action used Mannequin's native OPTION_IDX_RANDOM value. This
// is action identity, not permission for the receiver to resolve a substitute
// clip through the extracted ADB tables.
constexpr uint16_t kEnemyMannequinReservedNativeRandomOption = 1u << 7;
constexpr uint16_t kEnemyMannequinReservedFactionShift = 8;
constexpr uint16_t kEnemyMannequinReservedFactionMask = 0xff00u;
// semanticReserved[0]: the context is an authority movement decision. The
// observer consumes it as presentation evidence and must not execute a second
// AI path or ability instance.
constexpr uint8_t kEnemySemanticReservedAuthorityLocomotion = 1u << 0;
// semanticReserved[0]: the context owns an authority action whose Vanilla
// result did not construct a Mannequin fragment. The observer enters the same
// native ability instance; no substitute fragment is manufactured. Gameplay
// output remains authority-only at its native damage/physics boundaries.
constexpr uint8_t kEnemySemanticReservedAuthorityPresentation = 1u << 1;
// semanticReserved[1]: exact native presentation outcome selected by Vanilla.
constexpr uint8_t kEnemySemanticNativeOutcomeAbilityManager = 0;
constexpr uint8_t kEnemySemanticNativeOutcomeUnanimatedBody = 1;
constexpr uint8_t kEnemySemanticNativeOutcomeHitReactShiftBody = 2;
constexpr uint64_t kEnemyAbilityContextEthericDoppelganger = 12;
constexpr uint64_t kEnemyAbilityContextMilitaryOperatorSwipe = 1004;
constexpr uint16_t kInvalidMannequinOrdinal = 0xffffu;
constexpr uint32_t kEnemyLocomotionLevelFullAuthority = 0;
constexpr uint32_t kEnemyLocomotionLevelAuthorityLocomotionLocalCombat = 1;
constexpr uint32_t kEnemyLocomotionLevelLocalFocus = 2;
constexpr uint32_t kGooResultFlagTerrain = 1u << 0;
constexpr uint32_t kGooResultFlagDestroy = 1u << 1;
constexpr uint32_t kGooResultFlagDynamicAttach = 1u << 2;
constexpr uint32_t kGooResultFlagEntityStatic = 1u << 3;
constexpr uint32_t kGooResultFlagGooStatic = 1u << 4;
constexpr uint32_t kGooResultFlagVisualOnly = 1u << 5;
constexpr uint32_t kGooResultFlagHasFinalPosition = 1u << 6;
constexpr uint32_t kPlayerStatusFlagDowned = 1u << 0;
constexpr uint32_t kPlayerStatusFlagRevived = 1u << 1;
constexpr uint32_t kPlayerStatusFlagTeamWipe = 1u << 2;
constexpr uint32_t kPlayerStatusFlagTeleport = 1u << 3;
constexpr uint32_t kPlayerStatusFlagUnreachableRecovery = 1u << 4;
constexpr uint32_t kPlayerStatusFlagTargetLocalPlayer = 1u << 5;
constexpr uint32_t kRemoteDamageFlagAimed = 1u << 0;
constexpr uint32_t kRemoteDamageFlagKnocksDown = 1u << 1;
constexpr uint32_t kRemoteDamageFlagKnocksDownLeg = 1u << 2;
constexpr uint32_t kRemoteDamageFlagHitViaProxy = 1u << 3;
constexpr uint32_t kRemoteDamageFlagExplosion = 1u << 4;
constexpr uint32_t kRemoteDamageFlagCritical = 1u << 5;
constexpr uint32_t kRemoteDamageFlagSourceKey = 1u << 6;
constexpr uint32_t kRemoteDamageFlagStableSource = 1u << 7;
constexpr uint32_t kRemoteDamageFlagFriendlyFire = 1u << 8;
constexpr uint32_t kEnemyDeathPresentationFlagRemote = 1u << 9;
constexpr uint32_t kEnemyDeathPresentationFlagAuthorityHidden = 1u << 11;
constexpr uint32_t kEnemyDamageRequestFlagLocalPlayerSource = 1u << 16;
constexpr uint32_t kEnemyDamageRequestFlagLocalPlayerProjectile = 1u << 17;
constexpr uint32_t kSessionFlagWorldReady = 1u << 0;
constexpr uint32_t kSessionFlagHostAuthority = 1u << 1;
constexpr uint32_t kSessionFlagNeedsHostWorld = 1u << 2;
constexpr uint32_t kSessionFlagFriendlyFire = 1u << 3;
constexpr uint32_t kWorldSyncFlagLoadSave = 1u << 0;
constexpr uint32_t kWorldSyncFlagHasSavePath = 1u << 1;
constexpr uint32_t kWorldSyncFlagNeedsSaveTransfer = 1u << 2;
constexpr uint32_t kPlayerStateTransferFlagEmptyDefault = 1u << 0;
constexpr uint32_t kPlayerStateTransferFlagUploadToHost = 1u << 1;
constexpr uint32_t kPlayerStateTransferFlagHostAuthoritative = 1u << 2;
constexpr uint32_t kLivePropTransformFlagHidden = 1u << 0;
constexpr uint32_t kLivePropTransformFlagRemoved = 1u << 1;
constexpr uint32_t kLivePropTransformFlagCarried = 1u << 2;
constexpr uint32_t kLivePropTransformFlagActive = 1u << 3;
constexpr uint32_t kLivePropTransformFlagImpulse = 1u << 4;
constexpr uint32_t kLivePropTransformFlagClientAuthority = 1u << 5;
constexpr uint32_t kEnemyProjectileFlagCritical = 1u << 0;
constexpr uint32_t kEnemyProjectileFlagPooled = 1u << 1;
constexpr uint32_t kEnemyProjectileFlagSpawnFromCamera = 1u << 2;
constexpr uint16_t kEnemyAbilityFxNone = 0;
constexpr uint16_t kEnemyAbilityFxPoltergeistLift = 1;
constexpr uint16_t kEnemyAbilityFxPoltergeistThrow = 2;
constexpr uint16_t kEnemyAbilityFxWeaverCreateCystoid = 3;
constexpr uint16_t kEnemyAbilityFxWeaverAlarmCall = 4;
constexpr uint16_t kEnemyAbilityFxPsiAttack = 5;
constexpr uint16_t kEnemyAbilityFxCystoidExplode = 6;
constexpr uint16_t kEnemyAbilityFxCystoidNestTrigger = 7;
constexpr uint16_t kEnemyAbilityFxTurretFire = 8;
constexpr uint16_t kEnemyAbilityFxTurretSnapshot = 9;
constexpr uint16_t kEnemyAbilityFxTurretBrokenState = 10;
constexpr uint16_t kEnemyAbilityFxTurretStateRequest = 11;
constexpr uint16_t kEnemyAbilityFxTurretAuthorityClaimRequest = 12;
constexpr uint16_t kEnemyAbilityFxTurretAuthorityReleaseRequest = 13;
constexpr uint16_t kEnemyAbilityFxNpcMimicryBegin = 14;
constexpr uint16_t kEnemyAbilityFxNpcMimicryEnd = 15;
constexpr uint16_t kEnemyAbilityFxFlagNativeAction = 1u << 0;
constexpr uint16_t kEnemyAbilityFxFlagWorldEvent = 1u << 1;
constexpr uint16_t kEnemyAbilityFxFlagTurretBroken = 1u << 2;
constexpr uint16_t kEnemyAbilityFxFlagTurretForced = 1u << 3;
constexpr uint16_t kEnemyAbilityFxFlagTurretHacked = 1u << 4;
constexpr uint16_t kEnemyAbilityFxFlagTurretDeployed = 1u << 5;
constexpr uint16_t kEnemyAbilityFxFlagTurretStunned = 1u << 6;
constexpr uint16_t kEnemyAbilityFxFlagTurretMachineMinded = 1u << 7;
constexpr uint16_t kEnemyAbilityFxFlagTurretPickupMode = 1u << 8;
constexpr uint16_t kEnemyAbilityFxFlagTurretTechnopathControlled = 1u << 9;
constexpr uint16_t kEnemyAbilityFxFlagTurretUpright = 1u << 10;
constexpr uint16_t kEnemyAbilityFxFlagTurretFortified = 1u << 11;
constexpr uint16_t kEnemyAbilityFxFlagTurretCarried = 1u << 12;
constexpr uint16_t kEnemyAbilityFxFlagTurretFallbackAuthority = 1u << 13;
constexpr uint16_t kEnemyAbilityFxFlagMimicIgnorePsi = 1u << 14;
constexpr uint32_t kEnemyDamageRequestFlagTurretSource = 1u << 18;
constexpr uint16_t kStoryEventGrantKeycard = 1;
constexpr uint16_t kStoryEventGrantKeycode = 2;
constexpr uint16_t kStoryEventGrantFabricationPlan = 3;
constexpr uint16_t kStoryEventObjectiveAssigned = 4;
constexpr uint16_t kStoryEventObjectiveCompleted = 5;
constexpr uint16_t kStoryEventObjectiveFailed = 6;
constexpr uint16_t kStoryEventObjectiveUnassigned = 7;
constexpr uint16_t kStoryEventUtilityEnabled = 8;
constexpr uint16_t kStoryEventUtilityHidden = 9;
constexpr uint16_t kStoryEventUtilityButtonEnabled = 10;
constexpr uint16_t kStoryEventUtilityButtonHidden = 11;
constexpr uint16_t kStoryEventCollectNote = 12;
constexpr uint16_t kStoryEventCollectAudioLog = 13;
constexpr uint16_t kStoryEventCollectEmail = 14;
constexpr uint16_t kStoryEventCollectEmailDownload = 15;
constexpr uint16_t kStoryEventRosterPassword = 16;
constexpr uint16_t kStoryEventRosterDiscovered = 17;
constexpr uint16_t kStoryEventRosterKilled = 18;
constexpr uint16_t kStoryEventTaskActivated = 19;
constexpr uint16_t kStoryEventTaskCompleted = 20;
constexpr uint16_t kStoryEventTaskFailed = 21;
constexpr uint16_t kStoryEventTaskDeactivated = 22;
constexpr uint16_t kStoryEventObjectiveDescription = 23;
constexpr uint16_t kStoryEventConversationStatus = 24;
constexpr uint16_t kStoryEventGlobalBool = 25;
constexpr uint16_t kStoryEventResponseRuleUsed = 26;
constexpr uint16_t kStoryEventResponseUsed = 27;
constexpr uint16_t kStoryEventConditionExecuted = 28;
constexpr uint16_t kStoryEventConditionEnabled = 29;
constexpr uint16_t kStoryEventRemoteEvent = 30;
constexpr uint16_t kStoryEventConversationStarted = 31;
constexpr uint16_t kStoryEventDialogueLine = 32;
constexpr uint16_t kStoryEventConversationEnded = 33;
constexpr uint16_t kStoryEventCutsceneIntent = 34;
constexpr uint16_t kStoryEventGlobalInt = 35;
constexpr uint16_t kStoryEventGlobalString = 36;
constexpr size_t kStoryTextValueCapacity = 32;
constexpr uint16_t kStoryEventFlagUnlimited = 1u << 0;
constexpr uint16_t kStoryEventFlagShowHud = 1u << 1;
constexpr uint16_t kStoryEventFlagResponseWriteback = 1u << 2;
constexpr uint16_t kStoryEventFlagResponseConversation = 1u << 3;
constexpr uint16_t kStoryEventFlagResponseComplete = 1u << 4;
constexpr uint16_t kAreaObjectEventDoorOpen = 1;
constexpr uint16_t kAreaObjectEventDoorLocked = 2;
constexpr uint16_t kAreaObjectEventDoorPowered = 3;
// Protocol 117 carries the native Door tuple and the Lua-side power inputs
// that drive cloned door-model emissive materials. The legacy event ids
// remain reserved so old journals fail predictably instead of being
// reinterpreted as a different object family.
constexpr uint32_t kAreaObjectDoorStateFlagSnapshot = 1u << 0;
constexpr uint32_t kAreaObjectDoorStateFlagPowered = 1u << 1;
constexpr uint32_t kAreaObjectDoorStateFlagLocked = 1u << 2;
constexpr uint32_t kAreaObjectDoorStateFlagScriptState = 1u << 3;
constexpr uint32_t kAreaObjectDoorStateFlagPowerSupplied = 1u << 4;
constexpr uint32_t kAreaObjectDoorStateFlagDisrupted = 1u << 5;
constexpr uint32_t kAreaObjectDoorStateFlagMask =
    kAreaObjectDoorStateFlagSnapshot |
    kAreaObjectDoorStateFlagPowered |
    kAreaObjectDoorStateFlagLocked |
    kAreaObjectDoorStateFlagScriptState |
    kAreaObjectDoorStateFlagPowerSupplied |
    kAreaObjectDoorStateFlagDisrupted;
constexpr uint16_t kAreaObjectEventWorldItemRemoved = 4;
constexpr uint16_t kAreaObjectEventKeypadLocked = 5;
constexpr uint16_t kAreaObjectEventKeycardReaderLocked = 6;
constexpr uint16_t kAreaObjectEventInteractiveScreenPowered = 7;
constexpr uint16_t kAreaObjectEventInteractiveMachinePowered = 8;
constexpr uint16_t kAreaObjectEventOperatorDispenserLocked = 9;
constexpr uint16_t kAreaObjectEventOperatorDispenserState = 10;
constexpr uint16_t kAreaObjectEventFabricatorInteractionState = 11;
constexpr uint16_t kAreaObjectEventElevatorKioskState = 12;
constexpr uint16_t kAreaObjectEventContainerOpen = 13;
constexpr uint16_t kAreaObjectEventInteractiveObjectActive = 14;
constexpr uint16_t kAreaObjectEventInteractiveObjectDisabled = 15;
constexpr uint16_t kAreaObjectEventSwitchOn = 16;
constexpr uint16_t kAreaObjectEventBreakableHealth = 17;
constexpr uint16_t kAreaObjectEventLevelGameTokenBool = 18;
constexpr uint16_t kAreaObjectEventLevelGameTokenInt = 19;
constexpr uint16_t kAreaObjectEventLevelGameTokenString = 20;
constexpr uint16_t kAreaObjectEventApexTentacleSpawnerEnabled = 21;
constexpr uint16_t kAreaObjectEventCargoContainerMotion = 22;
constexpr uint16_t kAreaObjectEventCargoContainerDoorsOpen = 23;
constexpr uint16_t kAreaObjectEventRotatorActive = 24;
constexpr uint16_t kAreaObjectEventWorkstationLocked = 25;
constexpr uint16_t kAreaObjectEventWorkstationView = 26;
constexpr uint16_t kAreaObjectEventKioskButtonState = 27;
constexpr uint16_t kAreaObjectEventKioskButtonPressed = 28;
constexpr uint16_t kAreaObjectEventElevatorTransit = 29;
constexpr uint16_t kAreaObjectEventWorkstationUtilityPressed = 30;
constexpr uint16_t kAreaObjectEventGenericElevatorKioskButtonPressed = 31;
constexpr uint16_t kAreaObjectEventKioskButtonHeader = 32;
constexpr uint16_t kAreaObjectEventKioskButtonBody = 33;
constexpr uint16_t kAreaObjectEventKioskButtonVisible = 34;
constexpr uint16_t kAreaObjectEventKioskHeader = 35;
constexpr uint16_t kAreaObjectEventKioskBody = 36;
constexpr uint16_t kAreaObjectEventMainLiftOutage = 37;

constexpr bool IsTransientAreaObjectEvent(uint16_t eventKind)
{
    return eventKind == kAreaObjectEventKioskButtonPressed ||
        eventKind == kAreaObjectEventElevatorTransit ||
        eventKind == kAreaObjectEventWorkstationUtilityPressed ||
        eventKind == kAreaObjectEventGenericElevatorKioskButtonPressed ||
        eventKind == kAreaObjectEventMainLiftOutage;
}
// Scalar game-token/utility values retain their 32-byte validation bound.
// Kiosk localization keys can be longer (for example the 41-byte Arboretum
// Loading Bay body key), so the shared wire slot must preserve the exact key.
constexpr size_t kAreaObjectTextValueCapacity = 32;
constexpr size_t kAreaObjectWireTextValueCapacity = 96;

enum class WorldSyncCommand : uint32_t
{
    HostWorldOffer = 1,
    HostLoadCommand = 2,
    ClientNeedsHostWorld = 3,
    ClientWorldReady = 4,
    HostLoadStarting = 5,
};

enum class SaveTransferCommand : uint32_t
{
    Start = 1,
    Chunk = 2,
    Complete = 3,
    Abort = 4,
};

enum class PlayerStateTransferCommand : uint32_t
{
    Start = 1,
    Chunk = 2,
    Complete = 3,
    Abort = 4,
    Request = 5,
    HostSaveIdentity = 6,
};

enum class AreaJournalTransferCommand : uint32_t
{
    Start = 1,
    Chunk = 2,
    Complete = 3,
    Abort = 4,
    Request = 5,
};

enum class AreaLeaseCommand : uint32_t
{
    Grant = 1,
    Release = 2,
    Ready = 3,
    Freeze = 4,
};

enum class PacketType : uint16_t
{
    PlayerPose = 0x01,
    SessionHello = 0x03,
    RemotePlayerDamage = 0x05,
    TestMimicSpawn = 0x06,
    TestMimicState = 0x07,
    EnemyDamageRequest = 0x08,
    PlayerStatus = 0x09,
    ReliableAck = 0x0A,
    ReliableEnvelope = 0x0B,
    WorldSync = 0x0C,
    SaveTransfer = 0x0D,
    PlayerStateTransfer = 0x0E,
    AreaJournalTransfer = 0x0F,
    LivePropTransform = 0x10,
    DisconnectNotice = 0x11,
    GooResult = 0x12,
    EnemyProjectileEvent = 0x13,
    EnemyAbilityFxEvent = 0x14,
    StoryEvent = 0x15,
    AreaObjectEvent = 0x16,
    AreaLease = 0x17,
    EnemyRoster = 0x18,
    SharedDrop = 0x19,
    SharedStorage = 0x1A,
    HazardEvent = 0x1B,
    DialogueLease = 0x1C,
    TimeDilation = 0x1D,
    CorpsePhantomRequest = 0x1E,
    PeerPresence = 0x1F,
    ServerQuery = 0x20,
    ServerAdvertisement = 0x21,
    SessionReject = 0x22,
    EnemyMannequinAction = 0x23,
    EnemyDeathPresentation = 0x24,
};

enum class EnemyMannequinActionCommand : uint16_t
{
    Start = 1,
    Exit = 2,
};

constexpr uint16_t kEnemyMannequinActionFlagTagStateValid = 1u << 0;

enum class SharedDropCommand : uint16_t
{
    Spawn = 1,
    PickupRequest = 2,
    PickupCommit = 3,
    Remove = 4,
};

enum class SharedStorageCommand : uint16_t
{
    Register = 1,
    OpenRequest = 2,
    OpenGrant = 3,
    OpenDeny = 4,
    SnapshotBegin = 5,
    SnapshotItem = 6,
    SnapshotEnd = 7,
    CommitBegin = 8,
    CommitItem = 9,
    CommitEnd = 10,
    Release = 11,
};

enum class HazardEventKind : uint16_t
{
    RecyclerDetonate = 1,
    LeakAdded = 2,
    LeakRemoved = 3,
    LeakValveState = 4,
    PsiLiftField = 5,
    SurfaceHazardState = 6,
    AreaHazardState = 7,
    ElectricalBoxState = 8,
    RepairableState = 9,
    GravShaftState = 10,
    EmpDetonate = 11,
    LureDetonate = 12,
    NullwaveDetonate = 13,
    ExplosiveTankExplode = 14,
};

constexpr uint16_t kElectricalBoxStatePowered = 1u << 0;
constexpr uint16_t kElectricalBoxStateBroken = 1u << 1;
constexpr uint16_t kElectricalBoxStateDisrupted = 1u << 2;
constexpr uint16_t kElectricalBoxStateFortified = 1u << 3;
constexpr uint16_t kRepairableStateBroken = 1u << 0;
constexpr uint16_t kRepairableStateFortified = 1u << 1;
constexpr uint16_t kGravShaftStateEnabled = 1u << 0;
constexpr uint16_t kGravShaftStateBroken = 1u << 1;
constexpr uint16_t kGravShaftStateDisrupted = 1u << 2;
constexpr uint16_t kGravShaftStateReversed = 1u << 3;

enum class DialogueLeaseCommand : uint16_t
{
    Request = 1,
    Grant = 2,
    Deny = 3,
    Release = 4,
    Activity = 5,
};

enum class TimeDilationCommand : uint16_t
{
    RequestStart = 1,
    Start = 2,
    RequestEnd = 3,
    End = 4,
};

constexpr size_t kSharedStorageMaxWeaponMods = 4;

#pragma pack(push, 1)
struct PacketHeader
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = 0;
    uint32_t sequence = 0;
};

struct PlayerPosePacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::PlayerPose);
    uint32_t sequence = 0;
    uint64_t sourceAccountToken = 0;
    uint32_t worldEpoch = 0;
    uint32_t levelEpoch = 0;
    uint64_t levelId = 0;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float qw = 1.0f;
    float qx = 0.0f;
    float qy = 0.0f;
    float qz = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float vz = 0.0f;
    uint32_t flags = 0;
    uint16_t fireSerial = 0;
    uint16_t reloadSerial = 0;
    uint16_t meleeSerial = 0;
    uint16_t switchSerial = 0;
    uint16_t psiSerial = 0;
    uint16_t psiFxKind = 0;
    int16_t psiPowerId = 0;
    uint16_t psiImpactSerial = 0;
    uint16_t psiImpactFxKind = 0;
    int16_t psiImpactPowerId = 0;
    float aimDx = 0.0f;
    float aimDy = 1.0f;
    float aimDz = 0.0f;
    float psiImpactX = 0.0f;
    float psiImpactY = 0.0f;
    float psiImpactZ = 0.0f;
    uint16_t mimicVisualSerial = 0;
    uint16_t mimicModelPathLength = 0;
    uint32_t mimicSourceEntityId = 0;
    uint64_t mimicSourceGuid = 0;
    float mimicPx = 0.0f;
    float mimicPy = 0.0f;
    float mimicPz = 0.0f;
    float mimicQw = 1.0f;
    float mimicQx = 0.0f;
    float mimicQy = 0.0f;
    float mimicQz = 0.0f;
    float mimicSx = 1.0f;
    float mimicSy = 1.0f;
    float mimicSz = 1.0f;
    char mimicModelPath[kMimicModelPathSize] = {};
};

constexpr size_t kPlayerPoseBaseWireSize = offsetof(PlayerPosePacket, mimicModelPath);

constexpr size_t PlayerPoseWireSize(uint16_t mimicModelPathLength)
{
    return kPlayerPoseBaseWireSize +
        (mimicModelPathLength < kMimicModelPathSize
            ? static_cast<size_t>(mimicModelPathLength)
            : kMimicModelPathSize);
}

constexpr bool IsValidPlayerPoseWireSize(size_t wireSize, uint16_t mimicModelPathLength)
{
    return mimicModelPathLength < kMimicModelPathSize &&
        wireSize == PlayerPoseWireSize(mimicModelPathLength);
}

struct SessionHelloPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::SessionHello);
    uint32_t sequence = 0;
    uint64_t accountToken = 0;
    uint64_t modelArchetypeId = 0;
    char username[kUsernameSize] = {};
    char password[kPasswordSize] = {};
    char levelName[kLevelNameSize] = {};
    uint32_t modBuild = kModBuild;
    uint32_t flags = 0;
    uint64_t levelId = 0;
    uint32_t worldEpoch = 0;
    uint32_t levelEpoch = 0;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
};

struct AreaLeasePacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::AreaLease);
    uint32_t sequence = 0;
    uint32_t command = static_cast<uint32_t>(AreaLeaseCommand::Grant);
    uint32_t worldEpoch = 0;
    uint32_t leaseEpoch = 0;
    uint32_t areaLevelEpoch = 0;
    uint64_t areaId = 0;
    uint64_t authorityPeerToken = 0;
    uint64_t snapshotSequence = 0;
    char levelName[kLevelNameSize] = {};
};

struct RemotePlayerDamagePacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::RemotePlayerDamage);
    uint32_t sequence = 0;
    uint64_t targetAccountToken = 0;
    float damage = 0.0f;
    float proxyHealthBefore = 0.0f;
    float proxyHealthAfter = 0.0f;
    float proxyMaxHealth = 0.0f;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    uint32_t flags = 0;
    int32_t hitType = 0;
    int32_t material = 0;
    int32_t bulletType = 0;
    int32_t partId = 0;
    uint32_t shooterId = 0;
    uint32_t weaponId = 0;
    uint32_t projectileId = 0;
    uint16_t projectileClassId = 0;
    uint16_t weaponClassId = 0;
    float dx = 0.0f;
    float dy = 0.0f;
    float dz = 0.0f;
    float nx = 0.0f;
    float ny = 0.0f;
    float nz = 0.0f;
    uint32_t worldEpoch = 0;
    uint64_t levelId = 0;
    uint64_t sourceStableId = 0;
    uint64_t sourceKeyHash = 0;
    uint32_t sourceGeneration = 0;
    uint32_t attackSeq = 0;
    uint32_t projectileOrdinal = 0;
    uint32_t damageType = 0;
    uint32_t timeBucket = 0;
    uint32_t positionHash = 0;
};

struct TestMimicSpawnPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::TestMimicSpawn);
    uint32_t sequence = 0;
    uint64_t archetypeId = 0;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float qw = 1.0f;
    float qx = 0.0f;
    float qy = 0.0f;
    float qz = 0.0f;
    uint32_t flags = 0;
};

struct TestMimicStatePacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::TestMimicState);
    uint32_t sequence = 0;
    uint64_t archetypeId = 0;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float qw = 1.0f;
    float qx = 0.0f;
    float qy = 0.0f;
    float qz = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float vz = 0.0f;
    float health = 0.0f;
    float maxHealth = 0.0f;
    float lastDamage = 0.0f;
    uint32_t commitSequence = 0;
    uint64_t enemyNetId = 0;
    uint64_t entityGuid = 0;
    uint32_t sourceFlags = 0;
    uint32_t locomotionFlags = 0;
    uint32_t locomotionLevel = 0;
    uint32_t attackKind = 0;
    int32_t mannequinFragmentId = -1;
    uint32_t mannequinSequence = 0;
    uint16_t mannequinOrdinal = kInvalidMannequinOrdinal;
    uint16_t mannequinReserved = 0;
    int32_t mannequinPriority = 0;
    uint8_t mannequinTagState[12] = {};
    uint8_t mannequinTagStateValid = 0;
    uint8_t mannequinTagStateReserved[3] = {};
    uint64_t semanticContextId = 0;
    uint32_t semanticSequence = 0;
    uint8_t semanticVariant = 0;
    uint8_t semanticReserved[3] = {};
    float mx = 0.0f;
    float my = 1.0f;
    float mz = 0.0f;
    float speed = 0.0f;
    uint32_t flags = 0;
    uint64_t targetAccountToken = 0;
    uint64_t authorityOwnerAccountToken = 0;
    uint32_t authorityEpoch = 0;
    uint8_t authorityAttentionLevel = 0;
    uint8_t authorityReserved[3] = {};
};

// High-rate enemy snapshots use roster identity and quantized unit vectors.
// The full packet remains the in-process representation and debug fallback.
struct EnemyStateWirePacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::TestMimicState);
    uint32_t sequence = 0;
    uint64_t enemyNetId = 0;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    int16_t qw = 32767;
    int16_t qx = 0;
    int16_t qy = 0;
    int16_t qz = 0;
    int16_t mx = 0;
    int16_t my = 32767;
    int16_t mz = 0;
    uint16_t speedCentimetersPerSecond = 0;
    float health = 0.0f;
    float maxHealth = 0.0f;
    float lastDamage = 0.0f;
    uint32_t commitSequence = 0;
    uint16_t sourceFlags = 0;
    uint16_t locomotionFlags = 0;
    uint16_t stateFlags = 0;
    uint8_t locomotionLevel = 0;
    uint8_t authorityAttentionLevel = 0;
    uint32_t attackKind = 0;
    int32_t mannequinFragmentId = -1;
    uint32_t mannequinSequence = 0;
    uint16_t mannequinOrdinal = kInvalidMannequinOrdinal;
    uint16_t mannequinReserved = 0;
    int32_t mannequinPriority = 0;
    uint8_t mannequinTagState[12] = {};
    uint8_t mannequinTagStateValid = 0;
    uint8_t mannequinTagStateReserved[3] = {};
    uint64_t semanticContextId = 0;
    uint32_t semanticSequence = 0;
    uint8_t semanticVariant = 0;
    uint8_t semanticReserved[3] = {};
    uint64_t targetAccountToken = 0;
    uint64_t authorityOwnerAccountToken = 0;
    uint32_t authorityEpoch = 0;
};

// Direct enemy snapshots keep their 144-byte wire budget. A folded logical
// sender tag occupies four bytes that were already reserved by both packet
// representations. It disambiguates admitted players that share one UDP
// endpoint (local Wine prefixes or multiple machines behind one relay/NAT).
constexpr uint32_t EnemyStateSourceAccountTag(uint64_t accountToken)
{
    uint32_t tag = static_cast<uint32_t>(accountToken) ^
        static_cast<uint32_t>(accountToken >> 32) ^ 0x9e3779b9u;
    return tag != 0 ? tag : 1u;
}

template <typename TPacket>
inline void SetEnemyStateSourceAccountTag(TPacket& packet, uint64_t accountToken)
{
    const uint32_t tag = EnemyStateSourceAccountTag(accountToken);
    packet.mannequinTagStateReserved[0] = static_cast<uint8_t>(tag & 0xffu);
    packet.mannequinTagStateReserved[1] = static_cast<uint8_t>((tag >> 8) & 0xffu);
    packet.mannequinTagStateReserved[2] = static_cast<uint8_t>((tag >> 16) & 0xffu);
    packet.semanticReserved[2] = static_cast<uint8_t>((tag >> 24) & 0xffu);
}

template <typename TPacket>
constexpr uint32_t GetEnemyStateSourceAccountTag(const TPacket& packet)
{
    return static_cast<uint32_t>(packet.mannequinTagStateReserved[0]) |
        (static_cast<uint32_t>(packet.mannequinTagStateReserved[1]) << 8) |
        (static_cast<uint32_t>(packet.mannequinTagStateReserved[2]) << 16) |
        (static_cast<uint32_t>(packet.semanticReserved[2]) << 24);
}

struct EnemyRosterPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::EnemyRoster);
    uint32_t sequence = 0;
    uint64_t enemyNetId = 0;
    uint64_t stableEnemyId = 0;
    uint64_t areaId = 0;
    uint64_t archetypeId = 0;
    uint64_t entityGuid = 0;
    uint64_t sourceStableEnemyId = 0;
    uint32_t rosterVersion = 0;
    uint32_t flags = 0;
    uint32_t lifecycleGeneration = 0;
    uint32_t reserved = 0;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
};

struct CorpsePhantomRequestPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::CorpsePhantomRequest);
    uint32_t sequence = 0;
    uint64_t areaId = 0;
    uint64_t sourceEnemyNetId = 0;
    uint64_t sourceStableEnemyId = 0;
    uint64_t phantomArchetypeId = 0;
    uint64_t childStableEnemyId = 0;
    uint32_t flags = 0;
    uint32_t reserved = 0;
};

struct SharedDropPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::SharedDrop);
    uint32_t sequence = 0;
    uint32_t worldEpoch = 0;
    uint32_t objectVersion = 0;
    uint64_t hostSaveKeyHash = 0;
    uint64_t areaId = 0;
    uint64_t stableSpawnId = 0;
    uint64_t sourcePeerHash = 0;
    uint64_t targetPeerHash = 0;
    uint64_t archetypeId = 0;
    uint16_t command = static_cast<uint16_t>(SharedDropCommand::Spawn);
    uint16_t flags = 0;
    int32_t count = 0;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float qw = 1.0f;
    float qx = 0.0f;
    float qy = 0.0f;
    float qz = 0.0f;
};

struct SharedStoragePacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::SharedStorage);
    uint32_t sequence = 0;
    uint32_t worldEpoch = 0;
    uint32_t storageVersion = 0;
    uint32_t transactionId = 0;
    uint32_t leaseEpoch = 0;
    uint64_t hostSaveKeyHash = 0;
    uint64_t areaId = 0;
    uint64_t storageGuid = 0;
    uint64_t sourcePeerHash = 0;
    uint64_t targetPeerHash = 0;
    uint64_t archetypeId = 0;
    uint32_t itemFlags = 0;
    uint16_t command = static_cast<uint16_t>(SharedStorageCommand::Register);
    uint16_t flags = 0;
    uint16_t itemIndex = 0;
    uint16_t itemTotal = 0;
    int32_t count = 0;
    int16_t x = -1;
    int16_t y = -1;
    int16_t width = -1;
    int16_t height = -1;
    int16_t category = -1;
    uint16_t weaponModCount = 0;
    float weaponCondition = 0.0f;
    int16_t weaponAmmoLoaded = 0;
    int16_t weaponAmmoCount = 0;
    uint64_t weaponModIds[kSharedStorageMaxWeaponMods] = {};
    int16_t weaponModLevels[kSharedStorageMaxWeaponMods] = {};
};

struct HazardEventPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::HazardEvent);
    uint32_t sequence = 0;
    uint32_t worldEpoch = 0;
    uint64_t eventId = 0;
    uint64_t hostSaveKeyHash = 0;
    uint64_t areaId = 0;
    uint64_t sourcePeerHash = 0;
    uint64_t archetypeId = 0;
    uint64_t targetGuid = 0;
    uint16_t eventKind = static_cast<uint16_t>(HazardEventKind::RecyclerDetonate);
    uint16_t flags = 0;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float qw = 1.0f;
    float qx = 0.0f;
    float qy = 0.0f;
    float qz = 0.0f;
    float dx = 0.0f;
    float dy = 0.0f;
    float dz = 0.0f;
    float scalar = 0.0f;
};

struct DialogueLeasePacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::DialogueLease);
    uint32_t sequence = 0;
    uint32_t worldEpoch = 0;
    uint64_t eventId = 0;
    uint64_t hostSaveKeyHash = 0;
    uint64_t areaId = 0;
    uint64_t sourcePeerHash = 0;
    uint64_t targetPeerHash = 0;
    uint64_t dialogueId = 0;
    uint32_t leaseEpoch = 0;
    uint16_t command = static_cast<uint16_t>(DialogueLeaseCommand::Request);
    uint16_t flags = 0;
};

struct TimeDilationPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::TimeDilation);
    uint32_t sequence = 0;
    uint32_t worldEpoch = 0;
    uint64_t eventId = 0;
    uint64_t hostSaveKeyHash = 0;
    uint64_t sourcePeerHash = 0;
    uint32_t revision = 0;
    uint32_t timers = 0;
    uint16_t command = static_cast<uint16_t>(TimeDilationCommand::RequestStart);
    uint16_t flags = 0;
    float scale = 1.0f;
    float duration = 0.0f;
};

struct EnemyDamageRequestPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::EnemyDamageRequest);
    uint32_t sequence = 0;
    char username[kUsernameSize] = {};
    uint64_t archetypeId = 0;
    uint64_t enemyNetId = 0;
    uint64_t damagePackageId = 0;
    float damagePackageScale = 0.0f;
    float damage = 0.0f;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float dx = 0.0f;
    float dy = 0.0f;
    float dz = 0.0f;
    uint32_t flags = 0;
    int32_t hitType = 0;
    int32_t material = 0;
    int32_t bulletType = 0;
    int32_t partId = 0;
    uint32_t shooterId = 0;
    uint32_t weaponId = 0;
    uint32_t projectileId = 0;
    uint32_t uniqueId = 0;
    uint32_t groupId = 0;
    uint16_t projectileClassId = 0;
    uint16_t weaponClassId = 0;
    float nx = 0.0f;
    float ny = 0.0f;
    float nz = 0.0f;
    float impulseScale = 0.0f;
    float radius = 0.0f;
    float angle = 0.0f;
    float damageMin = 0.0f;
    int32_t penetrationCount = 0;
    uint64_t sourceTurretStableKey = 0;
    uint32_t sourceTurretAuthorityEpoch = 0;
    uint16_t signalValueCount = 0;
    uint16_t reserved = 0;
    uint64_t signalIds[kMaxEnemyDamageSignalValues] = {};
    float signalValues[kMaxEnemyDamageSignalValues] = {};
};

// A damage request is evaluated only by the current enemy authority. Once
// Vanilla confirms death there, this one-shot result carries the original hit
// semantics to observers so they create the same corpse presentation instead
// of inferring a generic death from health==0.
struct EnemyDeathPresentationPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::EnemyDeathPresentation);
    uint32_t sequence = 0;
    uint32_t worldEpoch = 0;
    uint64_t levelId = 0;
    uint64_t enemyNetId = 0;
    uint64_t stableEnemyId = 0;
    uint64_t enemyArchetypeId = 0;
    uint64_t authorityOwnerAccountToken = 0;
    uint64_t damageSourceAccountToken = 0;
    uint64_t sourceTurretStableKey = 0;
    uint64_t damagePackageId = 0;
    uint32_t authorityEpoch = 0;
    uint32_t sourceTurretAuthorityEpoch = 0;
    uint32_t flags = 0;
    float damagePackageScale = 0.0f;
    float targetHealthBeforeHit = 0.0f;
    float damage = 0.0f;
    float damageMin = 0.0f;
    float hitX = 0.0f;
    float hitY = 0.0f;
    float hitZ = 0.0f;
    float dirX = 0.0f;
    float dirY = 1.0f;
    float dirZ = 0.0f;
    float normalX = 0.0f;
    float normalY = -1.0f;
    float normalZ = 0.0f;
    float entityX = 0.0f;
    float entityY = 0.0f;
    float entityZ = 0.0f;
    float entityQw = 1.0f;
    float entityQx = 0.0f;
    float entityQy = 0.0f;
    float entityQz = 0.0f;
    float impulseScale = 0.0f;
    float radius = 0.0f;
    float angle = 0.0f;
    int32_t hitType = 0;
    int32_t material = 0;
    int32_t bulletType = 0;
    int32_t partId = 0;
    uint32_t uniqueId = 0;
    uint32_t groupId = 0;
    uint16_t projectileClassId = 0;
    uint16_t weaponClassId = 0;
    int32_t penetrationCount = 0;
    uint16_t signalValueCount = 0;
    uint16_t reserved = 0;
    uint64_t signalIds[kMaxEnemyDamageSignalValues] = {};
    float signalValues[kMaxEnemyDamageSignalValues] = {};
};

struct EnemyProjectileEventPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::EnemyProjectileEvent);
    uint32_t sequence = 0;
    uint32_t worldEpoch = 0;
    uint64_t levelId = 0;
    uint64_t enemyNetId = 0;
    uint64_t enemyArchetypeId = 0;
    uint64_t projectileArchetypeId = 0;
    uint32_t sourceProjectileId = 0;
    uint32_t ownerEntityId = 0;
    uint32_t flags = 0;
    uint32_t extendedFlags = 0;
    uint32_t groupId = 0;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float dx = 0.0f;
    float dy = 1.0f;
    float dz = 0.0f;
    char weaponClassName[kWeaponClassNameSize] = {};
};

struct EnemyAbilityFxEventPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::EnemyAbilityFxEvent);
    uint32_t sequence = 0;
    uint32_t worldEpoch = 0;
    uint64_t levelId = 0;
    uint64_t enemyNetId = 0;
    uint64_t enemyArchetypeId = 0;
    uint16_t abilityKind = kEnemyAbilityFxNone;
    uint16_t flags = 0;
    int32_t mannequinFragmentId = -1;
    uint32_t mannequinSequence = 0;
    uint16_t mannequinOrdinal = kInvalidMannequinOrdinal;
    uint16_t reserved = 0;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float dx = 0.0f;
    float dy = 1.0f;
    float dz = 0.0f;
    uint64_t authorityOwnerAccountToken = 0;
    uint32_t authorityEpoch = 0;
    float health = 0.0f;
    float maxHealth = 0.0f;
    uint64_t controllingTechnopathStableKey = 0;
    // Mimicry targets may be runtime props without an EntityGUID. The target
    // archetype plus the event position lets the observer select the same
    // local prop without relying on process-local EntityIds.
    uint64_t abilityTargetArchetypeId = 0;
};

// Reliable native Mannequin lifecycle edge. The observer queues this exact
// action; it must not translate the fragment into a guessed animation clip.
struct EnemyMannequinActionPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::EnemyMannequinAction);
    uint32_t sequence = 0;
    uint32_t worldEpoch = 0;
    uint64_t levelId = 0;
    uint64_t enemyNetId = 0;
    uint64_t enemyArchetypeId = 0;
    uint64_t authorityOwnerAccountToken = 0;
    uint32_t authorityEpoch = 0;
    uint32_t actionSequence = 0;
    uint16_t command = static_cast<uint16_t>(EnemyMannequinActionCommand::Start);
    uint16_t flags = 0;
    uint32_t actionFlags = 0;
    int32_t fragmentId = -1;
    int32_t priority = 0;
    uint32_t optionIndex = 0xffffffffu;
    uint8_t tagState[12] = {};
};

struct StoryEventPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::StoryEvent);
    uint32_t sequence = 0;
    uint64_t eventId = 0;
    uint32_t storyRevision = 0;
    uint32_t worldEpoch = 0;
    uint64_t hostSaveKeyHash = 0;
    uint64_t levelId = 0;
    uint64_t sourcePeerHash = 0;
    uint64_t targetId = 0;
    int32_t count = 0;
    uint16_t eventKind = 0;
    uint16_t flags = 0;
    uint32_t preVersion = 0;
    uint32_t postVersion = 0;
    char textValue[kStoryTextValueCapacity] = {};
};

struct AreaObjectEventPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::AreaObjectEvent);
    uint32_t sequence = 0;
    uint64_t eventId = 0;
    uint32_t areaRevision = 0;
    uint32_t worldEpoch = 0;
    uint64_t hostSaveKeyHash = 0;
    uint64_t levelId = 0;
    uint64_t sourcePeerHash = 0;
    uint64_t targetGuid = 0;
    uint64_t targetClassHash = 0;
    uint16_t eventKind = 0;
    uint16_t value = 0;
    int32_t count = 0;
    uint32_t flags = 0;
    uint32_t preVersion = 0;
    uint32_t postVersion = 0;
    char textValue[kAreaObjectWireTextValueCapacity] = {};
};

struct PlayerStatusPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::PlayerStatus);
    uint32_t sequence = 0;
    uint64_t sourceAccountToken = 0;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float qw = 1.0f;
    float qx = 0.0f;
    float qy = 0.0f;
    float qz = 0.0f;
    float health = 0.0f;
    float maxHealth = 0.0f;
    uint32_t flags = 0;
    uint32_t reason = 0;
};

struct WorldSyncPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::WorldSync);
    uint32_t sequence = 0;
    uint32_t command = 0;
    uint32_t worldEpoch = 0;
    uint32_t flags = 0;
    uint64_t levelId = 0;
    char levelName[kLevelNameSize] = {};
    char savePath[kSavePathSize] = {};
};

struct SaveTransferPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::SaveTransfer);
    uint32_t sequence = 0;
    uint32_t command = 0;
    uint32_t transferId = 0;
    uint32_t worldEpoch = 0;
    uint32_t totalBytes = 0;
    uint32_t chunkIndex = 0;
    uint32_t chunkCount = 0;
    uint16_t chunkBytes = 0;
    uint16_t reserved = 0;
    uint32_t checksum = 0;
    char levelName[kLevelNameSize] = {};
    uint8_t data[kSaveTransferDataSize] = {};
};

struct PlayerStateTransferPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::PlayerStateTransfer);
    uint32_t sequence = 0;
    uint32_t command = 0;
    uint32_t transferId = 0;
    uint32_t worldEpoch = 0;
    uint32_t totalBytes = 0;
    uint32_t chunkIndex = 0;
    uint32_t chunkCount = 0;
    uint16_t chunkBytes = 0;
    uint16_t reserved = 0;
    uint32_t checksum = 0;
    uint32_t flags = 0;
    uint64_t accountToken = 0;
    char username[kUsernameSize] = {};
    char saveKey[kSaveKeySize] = {};
    uint8_t data[kPlayerStateTransferDataSize] = {};
};

struct AreaJournalTransferPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::AreaJournalTransfer);
    uint32_t sequence = 0;
    uint32_t command = 0;
    uint32_t transferId = 0;
    uint32_t worldEpoch = 0;
    uint32_t totalBytes = 0;
    uint32_t chunkIndex = 0;
    uint32_t chunkCount = 0;
    uint16_t chunkBytes = 0;
    uint16_t reserved = 0;
    uint32_t checksum = 0;
    uint32_t flags = 0;
    uint32_t areaLeaseEpoch = 0;
    uint32_t areaLevelEpoch = 0;
    uint64_t areaId = 0;
    uint64_t hostSaveKeyHash = 0;
    uint64_t snapshotSequence = 0;
    char username[kUsernameSize] = {};
    char levelName[kLevelNameSize] = {};
    uint8_t data[kAreaJournalTransferDataSize] = {};
};

struct LivePropTransformPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::LivePropTransform);
    uint32_t sequence = 0;
    uint32_t worldEpoch = 0;
    uint64_t levelId = 0;
    uint64_t guid = 0;
    char levelName[kLevelNameSize] = {};
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float qw = 1.0f;
    float qx = 0.0f;
    float qy = 0.0f;
    float qz = 0.0f;
    float sx = 1.0f;
    float sy = 1.0f;
    float sz = 1.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float vz = 0.0f;
    float wx = 0.0f;
    float wy = 0.0f;
    float wz = 0.0f;
    uint32_t flags = 0;
    uint64_t sourceAccountToken = 0;
    uint32_t authorityEpoch = 0;
};

struct GooResultPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::GooResult);
    uint32_t sequence = 0;
    uint32_t worldEpoch = 0;
    uint64_t levelId = 0;
    char username[kUsernameSize] = {};
    char levelName[kLevelNameSize] = {};
    uint32_t projectileId = 0;
    uint32_t ownerId = 0;
    uint32_t flags = 0;
    uint64_t sourceGuid = 0;
    uint64_t targetEnemyNetId = 0;
    uint32_t targetEntityId = 0;
    int32_t hitPhysicsId = 0;
    int32_t attachedPhysicsId = 0;
    int32_t partId = 0;
    int32_t surfaceIndex = 0;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float finalPx = 0.0f;
    float finalPy = 0.0f;
    float finalPz = 0.0f;
    float dx = 0.0f;
    float dy = 1.0f;
    float dz = 0.0f;
    float spawnSize = 1.0f;
    float entityMass = 0.0f;
    float gooMass = 0.0f;
};

struct DisconnectNoticePacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::DisconnectNotice);
    uint32_t sequence = 0;
    uint64_t accountToken = 0;
    uint32_t worldEpoch = 0;
    uint32_t reason = 0;
    char username[kUsernameSize] = {};
};

struct ReliableAckPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::ReliableAck);
    uint32_t sequence = 0;
    uint32_t ackSequence = 0;
    uint32_t worldEpoch = 0;
};

struct ReliableEnvelopePacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::ReliableEnvelope);
    uint32_t sequence = 0;
    uint64_t sourceAccountToken = 0;
    uint32_t reliableSequence = 0;
    uint32_t ackSequence = 0;
    uint32_t worldEpoch = 0;
    uint16_t payloadType = 0;
    uint16_t payloadSize = 0;
    uint8_t payload[kReliablePayloadSize] = {};
};

enum class PeerPresenceCommand : uint16_t
{
    AddOrUpdate = 1,
    Remove = 2,
};

struct PeerPresencePacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::PeerPresence);
    uint32_t sequence = 0;
    uint64_t accountToken = 0;
    uint64_t modelArchetypeId = 0;
    uint64_t levelId = 0;
    uint32_t worldEpoch = 0;
    uint32_t levelEpoch = 0;
    uint32_t sessionFlags = 0;
    uint16_t command = static_cast<uint16_t>(PeerPresenceCommand::AddOrUpdate);
    uint16_t reserved = 0;
    char username[kUsernameSize] = {};
    char levelName[kLevelNameSize] = {};
};

struct ServerQueryPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::ServerQuery);
    uint32_t sequence = 0;
    uint64_t nonce = 0;
};

struct ServerAdvertisementPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::ServerAdvertisement);
    uint32_t sequence = 0;
    uint64_t nonce = 0;
    uint64_t hostAccountToken = 0;
    uint64_t levelId = 0;
    uint32_t modBuild = kModBuild;
    uint16_t gamePort = 0;
    uint16_t playerCount = 0;
    uint16_t maxPlayers = 0;
    uint16_t flags = 0;
    char serverName[kServerNameSize] = {};
    char hostUsername[kUsernameSize] = {};
    char levelName[kLevelNameSize] = {};
};

constexpr uint16_t kServerAdvertisementPassworded = 1u << 0;
constexpr uint16_t kServerAdvertisementLanVisible = 1u << 1;

enum class SessionRejectReason : uint16_t
{
    ServerFull = 1,
    WrongPassword = 2,
    DuplicateIdentity = 3,
    InvalidIdentity = 4,
    Kicked = 5,
};

struct SessionRejectPacket
{
    uint32_t magic = kPacketMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = static_cast<uint16_t>(PacketType::SessionReject);
    uint32_t sequence = 0;
    uint16_t reason = 0;
    uint16_t reserved = 0;
    char message[96] = {};
};
constexpr size_t kReliableEnvelopeHeaderSize = offsetof(ReliableEnvelopePacket, payload);
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 12);
static_assert(kPlayerPoseBaseWireSize == sizeof(PlayerPosePacket) - kMimicModelPathSize);
static_assert(kPlayerPoseBaseWireSize >= sizeof(PacketHeader));
static_assert(sizeof(PlayerPosePacket) <= kMaxPacketSize);
static_assert(sizeof(SessionHelloPacket) <= kMaxPacketSize);
static_assert(sizeof(AreaLeasePacket) <= kReliablePayloadSize);
static_assert(sizeof(RemotePlayerDamagePacket) <= kMaxPacketSize);
static_assert(sizeof(TestMimicSpawnPacket) <= kMaxPacketSize);
static_assert(sizeof(TestMimicStatePacket) <= kMaxPacketSize);
static_assert(sizeof(EnemyStateWirePacket) == 144);
static_assert(sizeof(EnemyStateWirePacket) < sizeof(TestMimicStatePacket));
static_assert(sizeof(EnemyDamageRequestPacket) <= kMaxPacketSize);
static_assert(sizeof(EnemyDeathPresentationPacket) <= kReliablePayloadSize);
static_assert(sizeof(EnemyProjectileEventPacket) <= kMaxPacketSize);
static_assert(sizeof(EnemyAbilityFxEventPacket) <= kReliablePayloadSize);
static_assert(sizeof(EnemyMannequinActionPacket) <= kReliablePayloadSize);
static_assert(sizeof(StoryEventPacket) <= kReliablePayloadSize);
static_assert(sizeof(AreaObjectEventPacket) <= kReliablePayloadSize);
static_assert(sizeof(EnemyRosterPacket) <= kReliablePayloadSize);
static_assert(sizeof(CorpsePhantomRequestPacket) <= kReliablePayloadSize);
static_assert(sizeof(SharedDropPacket) <= kReliablePayloadSize);
static_assert(sizeof(SharedStoragePacket) <= kReliablePayloadSize);
static_assert(sizeof(HazardEventPacket) <= kReliablePayloadSize);
static_assert(sizeof(DialogueLeasePacket) <= kReliablePayloadSize);
static_assert(sizeof(TimeDilationPacket) <= kReliablePayloadSize);
static_assert(sizeof(PeerPresencePacket) <= kReliablePayloadSize);
static_assert(sizeof(PlayerStatusPacket) <= kMaxPacketSize);
static_assert(sizeof(RemotePlayerDamagePacket) <= kReliablePayloadSize);
static_assert(sizeof(TestMimicSpawnPacket) <= kReliablePayloadSize);
static_assert(sizeof(EnemyDamageRequestPacket) <= kReliablePayloadSize);
static_assert(sizeof(PlayerStatusPacket) <= kReliablePayloadSize);
static_assert(sizeof(WorldSyncPacket) <= kReliablePayloadSize);
static_assert(sizeof(SaveTransferPacket) <= kReliablePayloadSize);
static_assert(sizeof(PlayerStateTransferPacket) <= kReliablePayloadSize);
static_assert(sizeof(AreaJournalTransferPacket) <= kReliablePayloadSize);
static_assert(sizeof(LivePropTransformPacket) <= kMaxPacketSize);
static_assert(sizeof(GooResultPacket) <= kReliablePayloadSize);
static_assert(sizeof(DisconnectNoticePacket) <= kMaxPacketSize);
static_assert(sizeof(ReliableAckPacket) <= kMaxPacketSize);
static_assert(sizeof(ReliableEnvelopePacket) == kMaxPacketSize);
static_assert(kReliableEnvelopeHeaderSize + kReliablePayloadSize == kMaxPacketSize);

inline bool IsValidHeader(const PacketHeader& header)
{
    return header.magic == kPacketMagic && header.version == kProtocolVersion;
}
}
