#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "CoopProtocol.h"
#include "CoopDamagePolicy.h"
#include "CoopEnemyAuthorityPolicy.h"
#include "CoopEnemyControlPolicy.h"
#include "CoopEventRouter.h"
#include "CoopNetworkTelemetry.h"
#include "CoopCoverageDiscovery.h"
#include "CoopIdentityConfig.h"
#include "CoopRuntimeExtractor.h"
#include "CoopNativeSideBlob.h"
#include "CoopNativeFragmentPayload.h"
#include "CoopNativeFragmentApply.h"
#include "CoopNativeGameStateFragmentLocator.h"
#include "CoopNativeFragmentImportPlanner.h"
#include "CoopNativeSaveBridge.h"
#include "CoopAreaStateJournal.h"
#include "CoopAreaObjectJournal.h"
#include "CoopAreaStateOverlay.h"
#include "CoopSaveStateBridge.h"
#include "NullUi.h"

#include <Chairloader/IChairRender.h>
#include <Chairloader/ModSDK/ChairloaderModBase.h>
#include <Prey/CryAction/IGameRulesSystem.h>
#include <Prey/CryEntitySystem/IEntity.h>
#include <Prey/CryParticleSystem/IParticles.h>
#include <Prey/CryRenderer/ITexture.h>
#include <Prey/GameDll/EntityUtility/EntityEffects.h>

class ArkNpc;
class CoopChat;
class ArkLevelTransitionDoor;
class ArkWorldUIOwner;
class ArkSaveLoadSystem;
class ArkDialogPlayer;
class ArkVisualPerceptionManager;
class ArkPlayer;
class ArkPlayerComponent;
class ArkKeyCardComponent;
class ArkKeyCodeComponent;
class ArkFabricationPlanComponent;
class ArkNoteComponent;
class ArkAudioLogComponent;
class ArkEmailComponent;
class ArkRosterComponent;
class ArkDoor;
class ArkInteractiveScreen;
class ArkStationWorldUI;
class ArkInteractiveMachine;
class ArkOperatorDispenser;
class ArkOperatorLaserHelper;
class ArkFabricator;
class ArkGenericElevatorKiosk;
class ArkKiosk;
class ArkKeypad;
class ArkKeycardReader;
class ArkResponseManager;
class ArkSpeakerBase;
class ArkConversation;
class ArkResponseQuery;
class ArkGlobalFacts;
class ArkObjectiveComponent;
class ArkUtilityComponent;
class ArkGameStateConditionManager;
class ArkInventory;
class ArkItemSystem;
class ArkGame;
class ArkPlayerCarry;
struct EventPhysCollision;
class ArkPlayerHealthComponent;
class ArkTimeScaleManager;
class CArkUIHUD;
class ArkLauncherMenu;
class ArkPauseMenu;
struct IUIElement;
class CArkItem;
class CArkBreakable;
class CArkExternalInventoryUI;
class CArkProjectileGoo;
class CArkProjectileRecyclerGrenade;
class CArkProjectileGrenade;
class CArkProjectileLureGrenade;
class CArkPsiPowerLift;
class ArkLeakable;
class ArkExplosiveTank;
class ArkSurfaceHazard;
class ArkAreaHazard;
class ArkElectricalBox;
class ArkChargeTrap;
class ArkApexTentacleSpawner;
class ArkCargoContainer;
class CArkGravShaftEntity;
class ArkRepairableObject;
class ArkCystoid;
class ArkCystoidNest;
class ArkNpcMovementDesire;
class CCryName;
class ArkLooseEffect;
class ArkFireAndForgetEffect;
class CoopProxyAttentionObject;
class CCryAction;
class IArkInventory;
class XmlNodeRef;
struct ILogCallback;
struct IParticleEmitter;
struct ISaveGame;
struct ILoadGame;
struct IGameToken;
struct IGameTokenSystem;
struct SEntitySpawnParams;
struct SInputEvent;
struct MovementRequest;
struct MovementRequestResult;
struct HitInfo;
struct ArkInteractionTestResult;
struct ArkInteractionInfo;
enum class EArkInteractionMode;
enum class EArkInteractionType;
enum class EArkDockingBay;
namespace ArkSignalSystem
{
class CArkSignalContext;
class Package;
} // namespace ArkSignalSystem

class ManagedArkLooseEffect final
{
public:
    ManagedArkLooseEffect();
    ~ManagedArkLooseEffect();

    ManagedArkLooseEffect(const ManagedArkLooseEffect&) = delete;
    ManagedArkLooseEffect& operator=(const ManagedArkLooseEffect&) = delete;
    ManagedArkLooseEffect(ManagedArkLooseEffect&&) = delete;
    ManagedArkLooseEffect& operator=(ManagedArkLooseEffect&&) = delete;

    ArkLooseEffect* Get();
    const ArkLooseEffect* Get() const;
    void AbandonForLevelUnload();

private:
    void* m_storage = nullptr;
    bool m_constructed = false;
};
namespace CoopSaveStoreDecoder
{
struct StoreMap;
}

enum class CoopNetworkMode
{
    Off,
    Host,
    Client,
};

enum class CoopProxyLifecycleState
{
    Empty,
    ActiveSameLevel,
    SuspendedRemoteLevel,
    SuspendedTransition,
    Destroyed,
};

class ModMain final : public ChairloaderModBase
{
public:
    using BaseClass = ChairloaderModBase;
    struct GooSpawnContext
    {
        Vec3 position = Vec3(ZERO);
        Vec3 direction = Vec3(0.0f, 1.0f, 0.0f);
        int hitPhysicsId = 0;
        int attachedPhysicsId = 0;
        int partId = 0;
        int surfaceIndex = 0;
        bool terrain = false;
    };

    ~ModMain();

    //---------------------------------------------------------------------------------
    // Mod Methods
    //---------------------------------------------------------------------------------
    
    void SpawnProxy();
    void ConfigureProxy();
    void SpawnMimicNearProxy();
    void RemoveSpawnedEntities();
    void HideSpawnedEntitiesForLevelMismatch(const char* reason);
    void StartHost();
    void StartClient();
    void StopNetwork();
    bool ShouldSuppressProxyNpcAiAction(EntityId entityId, const char* stage);
    bool ShouldBlockRemoteProxyTransformWrite(IEntity* entity, const char* stage);
    bool ShouldSuppressProxyNpcNativeState(EntityId entityId, const char* stage);
    bool ShouldSuppressArkNpcHit(EntityId entityId, const HitInfo& hitInfo) const;
    void SanitizeArkNpcHitForOriginal(EntityId entityId, HitInfo& hitInfo) const;
    void OnArkNpcHitPost(EntityId entityId, const HitInfo& hitInfo, uint64_t damagePackageId = 0);
    void OnArkEnemySignalPackageObserved(
        EntityId targetEntityId,
        EntityId senderEntityId,
        EntityId instigatorEntityId,
        uint64_t packageId,
        const ArkSignalSystem::CArkSignalContext& context,
        float scale);
    bool OnArkEnemySignalPackageMaterialized(
        EntityId targetEntityId,
        const ArkSignalSystem::Package& package);
    void OnArkPlayerSignalHitObserved(const HitInfo& hitInfo, uint64_t packageId, EntityId packageSourceId);
    void OnArkSignalPackageObserved(
        EntityId targetEntityId,
        EntityId senderEntityId,
        EntityId instigatorEntityId,
        uint64_t packageId,
        const ArkSignalSystem::CArkSignalContext& context,
        float scale,
        const char* stage);
    void OnArkSignalPackageObserved(
        EntityId targetEntityId,
        const ArkSignalSystem::Package& package,
        const char* stage);
    void OnArkPlayerDamageObserved(float damage);
    void OnArkPlayerHealthDropObserved(const ArkPlayerHealthComponent* healthComponent, float newHealth, const char* source);
    bool ShouldSuppressArkPlayerDamage(float damage);
    void OnArkPlayerDamageSuppressed(float damage);
    bool ShouldSuppressArkPlayerHealthSet(const ArkPlayerHealthComponent* healthComponent, float health, bool damagedByRecyclerGrenade);
    void OnArkPlayerHealthSetSuppressed(const ArkPlayerHealthComponent* healthComponent, float health, bool damagedByRecyclerGrenade);
    bool ShouldRunArkPlayerNativeDeathFeedback(const ArkPlayerHealthComponent* healthComponent, bool byRecyclerGrenade) const;
    void OnArkPlayerNativeDeathFeedbackStarting(const ArkPlayerHealthComponent* healthComponent, bool byRecyclerGrenade);
    void OnArkPlayerNativeDeathFeedbackFinished(const ArkPlayerHealthComponent* healthComponent, bool byRecyclerGrenade);
    bool ShouldSuppressArkPlayerDeath(const ArkPlayerHealthComponent* healthComponent, bool byRecyclerGrenade);
    void OnArkPlayerDeathSuppressed(const ArkPlayerHealthComponent* healthComponent, bool byRecyclerGrenade);
    bool ShouldSuppressArkPlayerForceKill(const ArkPlayerHealthComponent* healthComponent);
    void OnArkPlayerForceKillSuppressed(const ArkPlayerHealthComponent* healthComponent);
    bool ShouldSuppressArkPlayerRagdollize(const ArkPlayer* player) const;
    void OnArkPlayerRagdollizeSuppressed(float verticalSpeed);
    bool ShouldOverrideArkPlayerIsDead(const ArkPlayer* player) const;
    bool ShouldOverrideArkPlayerHealthIsDead(const ArkPlayerHealthComponent* healthComponent) const;
    void OnArkPlayerIsDeadOverridden(const char* source);
    bool ShouldSuppressArkPlayerDeathMovementState() const;
    void OnArkPlayerDeathMovementStateSuppressed(const char* source);
    bool ShouldSuppressArkPlayerDeathScreenOpen();
    void OnArkPlayerDeathScreenOpenSuppressed();
    bool ShouldNeutralizeNativeTimeScale(float scale) const;
    void RecordTimeScaleOverride(unsigned timers, float scale, int handle, bool neutralized);
    void RecordTimeScaleUpdate(int handle, float scale, bool neutralized);
    void RecordTimeScaleClear(int handle);
    void OnNativeTimeScaleOverride(ArkTimeScaleManager* manager, unsigned timers, float scale, int handle);
    void OnNativeTimeScaleUpdate(ArkTimeScaleManager* manager, int handle, float scale);
    void OnNativeTimeScaleClear(ArkTimeScaleManager* manager, int handle);
    bool ShouldSuppressFocusModeStart(bool openMenu) const;
    void RecordFocusModeStart(bool openMenu, bool suppressed, bool result);
    void RecordFocusModeStop(bool fromTargeting);
    bool StartManagedWorldParticleFromFireAndForget(const ArkFireAndForgetEffect* fireAndForget, const QuatTS& location, float lifeSeconds, const char* reason);
    void TrackManagedWorldParticleEmitter(IParticleEmitter* emitter, const Vec3& position, float lifeSeconds, const char* reason, const char* mode);
    void StopLocalFocusModeForDowned(const char* reason);
    void ClearLocalPlayerModalStateAfterSidecarApply(const char* reason);
    bool ShouldSuppressArkPlayerAction(const CCryName& action, int activationMode, float value);
    void RecordLocalPlayerPoseAction(const CCryName& action, int activationMode, float value);
    void DrawCoopHudOverlayPreRender();
    void OnArkUIHUDUpdateHook(CArkUIHUD* hud);
    void OnArkUIHUDPreRenderHook(CArkUIHUD* hud);
    void QueueCoopHudFeedback(const std::string& message, float durationSeconds = 3.0f);
    void DrawPeerTimeoutHudOverlay();
    void TickJoinOverlay(float frameTime);
    void DrawJoinOverlayImGui(float nowSeconds);
    void DrawMultiplayerUi();
    void ClearJoinOverlayState(const char* reason);
    bool ShouldBlockJoinInput() const;
    void ApplyJoinInputBlock(const char* reason);
    void ReleaseJoinInputBlock(const char* reason);
    void ApplyPeerConnectionLostFreeze(const char* reason);
    void ReleasePeerConnectionLostFreeze(const char* reason);
    bool ShouldSuppressArkDialogPlayerPlay(const ArkDialogPlayer* dialogPlayer) const;
    void OnArkDialogPlayerPlaySuppressed(const ArkDialogPlayer* dialogPlayer);
    bool ShouldSuppressArkVisualPerceptionUpdate(const ArkVisualPerceptionManager* manager) const;
    void OnArkVisualPerceptionUpdateSuppressed(const ArkVisualPerceptionManager* manager, float elapsedTime);
    bool ShouldSuppressArkVisualPerceptionAcquireAll(const ArkVisualPerceptionManager* manager) const;
    void OnArkVisualPerceptionAcquireAllEntered(const ArkVisualPerceptionManager* manager);
    void OnArkVisualPerceptionAcquireAllSkipped(const ArkVisualPerceptionManager* manager, const char* reason);
    void OnArkVisualPerceptionAcquireAllGuarded(const ArkVisualPerceptionManager* manager, bool ok, const char* guardReason);
    bool ShouldGuardArkNpcAbilityOwnerRemoval();
    void RecordSkippedArkNpcAbilityOwnerRemoval(
        const void* manager,
        uint64_t ownerKey,
        const void* ability,
        const char* reason);
    bool IsPostLoadNativeQuarantineActive() const;
    bool ShouldBypassArkNpcAbilityPerformHook() const;
    bool IsRuntimeTransitionCleanupPrepared() const { return m_runtimeTransitionCleanupPrepared; }
    bool ShouldBypassNpcMannequinHooksBeforeSession() const;
    bool ShouldBypassNpcDesireHooksBeforeSession() const;
    bool ShouldTraceNativeEntityLifecycle(EntityId entityId) const;
    void RecordNativeEntityLifecycleTrace(const char* stage, EntityId entityId, ArkNpc* npc, const char* detail);
    uint64_t BeginNativeEntityRemoveTrace(EntityId entityId, bool forceRemoveNow);
    void EndNativeEntityRemoveTrace(uint64_t previousState);
    void RecordRecyclerGrenadeTrace(const char* stage, EntityId projectileEntityId, EntityId targetEntityId, uint32_t flags, const char* detail);
    void RecordGooProjectileTrace(const char* stage, EntityId projectileEntityId, EntityId targetEntityId, uint32_t flags, const char* detail);
    void QueueLocalEnemyProjectileEventForHook(
        EntityId projectileEntityId,
        EntityId ownerEntityId,
        IEntityArchetype* projectileArchetype,
        const Vec3& position,
        const Vec3& direction,
        const char* weaponClassName,
        bool critical,
        bool pooled,
        uint32_t extendedFlags,
        uint32_t groupId,
        bool spawnFromCamera);
    void QueueLocalTurretFireEventForHook(
        EntityId turretEntityId,
        uint64_t turretArchetypeId,
        const Vec3& position,
        const Vec3& direction,
        EntityId targetEntityId,
        const char* reason);
    void QueueLocalEnemyMimicryEventForHook(
        ArkNpc* npc,
        const IEntity* targetEntity,
        bool begin,
        EArkNpcMimicryReason mimicryReason,
        bool ignorePsi);
    bool IsApplyingRemoteEnemyAbilityFxEvent() const { return m_applyingRemoteEnemyAbilityFxEvent; }
    void RegisterOperatorLaserHelperForHook(
        ArkOperatorLaserHelper* helper,
        ArkNpc* npc,
        float damagePerSecond);
    void UnregisterOperatorLaserHelperForHook(ArkOperatorLaserHelper* helper);
    void OnNativeOperatorLaserStartForHook(
        ArkOperatorLaserHelper* helper,
        ArkNpc* npc,
        float chargeDuration);
    void OnNativeOperatorLaserTurnOffForHook(ArkOperatorLaserHelper* helper, ArkNpc* npc);
    void OnNativeOperatorLaserUpdateForHook(ArkOperatorLaserHelper* helper, ArkNpc* npc);
    bool ShouldDeferLocalFocusedOperatorTurnOffForHook(
        ArkOperatorLaserHelper* helper,
        ArkNpc* npc);
    void OnNativeOperatorLaserDamageForHook(ArkNpc* npc);
    void StartLocalFocusedOperatorCombatForHook(
        ArkNpc* npc,
        uint64_t contextId,
        EntityId targetEntityId);
    void RetargetLocalFocusedOperatorLaserForHook(ArkOperatorLaserHelper* helper, ArkNpc* npc);
    bool ShouldSuppressRemoteOperatorLaserDamage(ArkNpc* npc);
    uint64_t CaptureNativeCorpsePhantomSourceStableId(EntityId corpseEntityId, uint64_t* outSourceEnemyNetId = nullptr);
    void OnNativeCorpsePhantomUpdateResult(
        EntityId corpseEntityId,
        EntityId previousPhantomEntityId,
        EntityId phantomEntityId,
        uint64_t phantomArchetypeId,
        uint64_t sourceEnemyNetId,
        uint64_t sourceStableEnemyId,
        bool completed);
    uint32_t QueueLocalTurretSnapshotEventsForHook(const char* reason);
    void QueueLocalTurretSnapshotStateForHook(ArkTurret* turret, const char* reason);
    void QueueLocalTurretBrokenStateEventForHook(ArkTurret* turret, bool broken, bool wasForced, const char* reason);
    void OnNativeTurretInitializedForHook(ArkTurret* turret, const char* reason);
    bool SuppressClientTurretProjectileForHook(EntityId turretEntityId, const char* reason);
    void OnNativeLivePropAttackImpulseForHook(
        IEntity* hitEntity,
        IPhysicalEntity* hitPhysics,
        EntityId sourceEntityId,
        bool allowUnattributedSource,
        const char* reason);
    void OnNativeLivePropExplosionImpulseForHook(
        EntityId senderEntityId,
        EntityId instigatorEntityId,
        const std::vector<IPhysicalEntity*>& affectedPhysics,
        const char* reason);
    void BindNpcSemanticAnimatedAction(void* npcPtr, const void* actionPtr, const char* stage);
    bool GateRemoteDrivenEnemyMovement(void* movementManagerPtr, const char* stage);
    bool ShouldBlockRemoteDrivenEnemyIntent(
        void* npcPtr,
        const char* stage,
        uint64_t contextId = 0,
        EntityId targetEntityId = INVALID_ENTITYID,
        uint32_t semanticFlags = 0);
    bool RearmRemoteDrivenEnemyLocalCombatAfterEnd(void* npcPtr, const char* stage);
    bool ShouldConsumeRemoteDrivenEnemyAuthorityAbility(
        void* npcPtr,
        uint64_t contextId);
    EntityId ResolveRemoteDrivenEnemyLocalCombatTarget(
        void* npcPtr,
        const char* stage,
        uint64_t contextId,
        EntityId requestedTargetEntityId);
    bool ShouldBlockRemoteDrivenEnemyLook(void* lookManagerPtr, const char* stage);
    bool ShouldBlockRemoteDrivenEnemyLookaround(void* npcPtr, const char* stage);
    bool ShouldBlockRemoteDrivenEnemyFacing(void* facingManagerPtr, const char* stage);
    bool ShouldBlockRemoteEnemyCollisionCallback(
        void* npcPtr,
        const char* stage,
        EntityId instigatorEntityId = INVALID_ENTITYID);
    bool ShouldBlockRemoteEnemyRagdoll(void* npcPtr, const char* stage);
    bool TryGetRemoteDrivenEnemyTransformOverride(
        IEntity* entity,
        const char* stage,
        bool overridePosition,
        bool overrideRotation,
        Vec3& ioPosition,
        Quat& ioRotation);
    void RecordAuthorityEnemyMovementDesire(void* movementManagerPtr, const char* stage);
    void RecordAuthorityEnemyMovementRequest(void* movementManagerPtr, const MovementRequest& request, const char* stage);
    void RecordAuthorityEnemyMovementResult(
        void* movementManagerPtr,
        const MovementRequestResult& result,
        const char* stage);
    void OnNativeNpcGlooStateChanged(ArkNpc* npc, const char* stage, bool frozen);
    void OnNativeNpcPersistentStatusChanged(ArkNpc* npc, uint32_t statusFlag, bool enabled, const char* stage);
    void RecordPsiFxSniper(std::string event);
    bool MarkLocalGooResultSource(EntityId projectileEntityId, const char* reason);
    bool TryCorrectRemoteGooFinalTransform(CArkProjectileGoo* goo, EntityId projectileEntityId, const char* reason, bool logTrace = true);
    void RememberLocalGooSpawnContext(EntityId projectileEntityId, const GooSpawnContext& context);
    bool TryGetLocalGooSpawnContext(EntityId projectileEntityId, GooSpawnContext& context) const;
    void ForgetLocalGooSpawnContext(EntityId projectileEntityId);
    void QueueLocalGooResultForHook(
        EntityId projectileEntityId,
        EntityId ownerEntityId,
        const Vec3& position,
        const Vec3& direction,
        float spawnSize,
        int hitPhysicsId,
        int attachedPhysicsId,
        int partId,
        int surfaceIndex,
        uint32_t flags,
        const char* reason,
        const Vec3* finalPositionOverride = nullptr);
    void QueueLocalGooDestroyForHook(EntityId projectileEntityId, EntityId ownerEntityId, const char* reason);
    bool IsNetworkOriginatedGooDestroyActive() const;
    bool IsNetworkOriginatedGooDestroyActive(EntityId gooEntityId) const;
    void MarkNetworkOriginatedGooDestroy(EntityId gooEntityId);
    void ClearNetworkOriginatedGooDestroy(EntityId gooEntityId);
    void QueueLocalGooDynamicAttachForHook(
        EntityId dynamicEntityId,
        EntityId gooEntityId,
        bool isEntityStatic,
        bool isGooStatic,
        float entityMass,
        float gooMass,
        EntityId attachedEntityId,
        const char* reason);
    void RecordNativeNpcSpawnTrace(const char* stage, IEntity* entity, const char* detail);
    void OnArkLevelTransitionConfirmed(ArkLevelTransitionDoor* door);
    void OnArkLevelTransitionDoorUpdated(ArkLevelTransitionDoor* door);
    bool TryCorrectTransitionWorldUIScreenLocation(
        const ArkWorldUIOwner* owner,
        QuatT& location);
    void OnArkSaveLoadSaveCurrentLevelState(ArkSaveLoadSystem* saveLoadSystem, bool beforeOriginal);
    void OnArkSaveLoadLoadCurrentLevelState(ArkSaveLoadSystem* saveLoadSystem, bool beforeOriginal);
    bool OnCEntitySerializeHook(IEntity* entity, bool reading, int target, int flags);
    void OnArkGameSerializeForLevelStateHook(
        ArkGame* game,
        const char* phase,
        bool reading,
        int target,
        bool ok,
        const char* serializerFingerprint,
        std::uintptr_t serializerImpl,
        std::uintptr_t serializerVtable);
    bool ShouldTraceLevelStateSerializer() const;
    bool ShouldTraceLevelStateSerializerSource(const char* source) const;
    const NativeSideBlobCaptureState* GetPendingNativeGameStatePlayerCapture() const;
    const NativeCapturedItemState* GetPendingNativeGameStateItemForEntity(unsigned entityId) const;
    unsigned GetNativeGameStateOverlayOwnerId() const;
    bool ShouldUseNativeGameStatePlayerOverlay() const;
    bool IsActiveNativeSideBlobCaptureItem(unsigned entityId) const;
    bool IsActiveNativeSideBlobCapturePlayerOwnedItem(const CArkItem* item, unsigned entityId) const;
    bool IsTrackedLocalPlayerInventoryItemId(unsigned entityId) const;
    void RememberLocalPlayerInventoryItemIds(const ArkInventory* inventory, const char* reason);
    uint64_t EnterNativeInventorySerializeScope(
        ArkInventory* inventory,
        const void* serializerPtr,
        bool localPlayerInventory,
        bool reading,
        int target,
        const std::string& sectionName);
    void ExitNativeInventorySerializeScope(uint64_t scopeSeq);
    bool IsNativeLocalPlayerInventorySerializeScopeActive(
        const void* serializerPtr,
        bool reading,
        int target,
        const std::string& sectionName) const;
    uint64_t GetNativeInventorySerializeScopeSeq() const;
    std::string GetNativeInventorySerializeScopeSection() const;
    uint64_t EnterNativeItemSerializeScope(
        CArkItem* item,
        unsigned itemEntityId,
        const void* serializerPtr,
        bool localPlayerInventoryItem,
        bool reading,
        int target,
        const std::string& sectionName);
    void ExitNativeItemSerializeScope(uint64_t scopeSeq);
    uint32_t GetLevelStateSerializerTraceLimit() const;
    void RecordLevelStateSerializerTraceOp(
        const char* source,
        const char* op,
        const char* name,
        const char* typeName,
        bool reading,
        int depth,
        const char* detail);
    void RecordNpcMannequinStoreAction(const char* stage, void* stateSlot, void* transitionPayload, const char* chainTrace = nullptr);
    void RecordNpcMannequinActionConstruct(
        const char* stage,
        void* action,
        void* npc,
        int priority,
        int fragmentId,
        void* tagState,
        int arg6,
        int arg7,
        int arg8,
        const char* chainTrace = nullptr);
    void RecordNpcMannequinActionLifecycle(
        const char* stage,
        void* action,
        const char* chainTrace = nullptr);
    bool ShouldBlockRemoteEnemyMannequinActionStart(
        void* action,
        const char* stage,
        const char* chainTrace = nullptr);
    void RecordNpcSemanticTrace(
        const char* stage,
        ArkNpc* npc,
        uint64_t contextId,
        EntityId targetId,
        uint32_t flags,
        const char* detail = nullptr);
    void AppendEnemySyncTrace(const char* channel, const std::string& event);
    void ClearEnemySyncTrace(const char* reason);
    std::string BuildEnemySyncTraceTail(size_t maxEntries) const;
    void RecordRuntimeLogEmission();
    void RecordRuntimeTransformHook(bool rewritten);
    void RecordRuntimeEntityScan(uint64_t candidatesVisited, uint64_t elapsedMicroseconds);
    void ResetRuntimeCostTelemetry();
    std::string BuildRuntimeCostReport() const;
    std::string ResolveNpcMannequinFragmentNameForRuntime(int fragmentId);
    std::string ResolveNpcMannequinFragmentNameForRuntime(IEntity* entity, int fragmentId);
    std::string ResolveNpcMannequinKindForRuntime(IEntity* entity);
    int ResolveNpcMannequinFragmentIdForRuntime(IEntity* entity, const char* fragmentName);
    bool ResolveNpcMannequinVariantTagsForRuntime(
        IEntity* entity,
        int fragmentId,
        int ordinal,
        std::string& outTags,
        std::string& outFragTags,
        std::string& detail);
    uint32_t ClassifyNpcMannequinVariantTagsForRuntime(
        IEntity* entity,
        int fragmentId,
        int ordinal,
        std::string& detail);
    int ResolveNpcMannequinOrdinalForRuntime(
        IEntity* entity,
        int fragmentId,
        uint32_t desiredFlags,
        uint32_t localFlags,
        int remoteOrdinal,
        std::string& detail,
        uint64_t deterministicChoiceSeed = 0);
    bool TryConcretizeLocalAuthorityRandomMannequinOption(
        IEntity* entity,
        void* action,
        int fragmentId,
        const std::string& fragmentName,
        std::string& detail);
    bool RecordEnemyMannequinAuthorityState(
        EntityId entityId,
        int fragmentId,
        const char* fragmentName,
        const char* stage,
        uint32_t flags,
        uint32_t attackKind,
        int optionIdx = -1,
        uint32_t priority = 0,
        const void* action = nullptr);
    void RecordNativeGameStateInventoryCellEntityId(
        const char* source,
        const char* path,
        unsigned entityId,
        bool reading = false,
        int target = eST_SaveGame);
    bool TryAttachNativeFragmentPayload(NativeSideBlobCaptureState& target, const char* reason);
    bool TryExportNativeFragmentPayload(const char* reason);
    void FinalizeNativeSideBlobCaptureFromSaveComplete(const char* reason, bool saveSucceeded);
    void TryApplyNativeFragmentTargetNoopPatch(
        const CoopNativeGameStateFragmentLocator::NativeInventoryFragmentBundle& targetBundle,
        const char* reason);
    bool ShouldProbeSerializerNodeContext(const char* source) const;
    bool ShouldCaptureNativeFragmentTarget(
        bool reading,
        int target,
        const std::string& sectionName) const;
    void RecordSerializerNodeContextProbe(
        const char* source,
        const char* op,
        const char* path,
        ISerialize* serializerImpl);
    void RecordNativeGameStateFragmentCandidate(
        const char* source,
        const char* kind,
        const void* objectPtr,
        const void* serializerPtr,
        bool reading,
        int target,
        const std::string& sectionName);
    void RecordSaveLoadSectionTrace(
        const char* source,
        const char* sectionName,
        bool reading,
        int target,
        bool ok,
        const char* serializerFingerprint);
    CoopSaveStateBridge::SectionEvent ObserveSaveStateBridgeSection(
        CoopSaveStateBridge::SectionApi api,
        const char* source,
        const char* rawSectionName,
        TSerialize* serializer,
        bool fallbackReading,
        int fallbackTarget,
        bool fallbackOk);
    CoopSaveStateBridge::SectionEvent ObserveSaveStateBridgeLoadUniquePtrSection(
        CoopSaveStateBridge::SectionApi api,
        const char* source,
        const char* rawSectionName,
        std::unique_ptr<TSerialize>* serializerOwner);
    CoopSaveStateBridge::CoopSaveMergeResult MergeCoopSaveForLoadedSection(
        const CoopSaveStateBridge::SectionEvent& event,
        std::unique_ptr<TSerialize>* serializerOwner,
        const char* reason);
    std::string FindSaveStateBridgeSectionForSerializer(TSerialize serializer) const;
    void OnSaveStateBridgeSectionEvent(const CoopSaveStateBridge::SectionEvent& event);
    uint64_t BeginSaveStateTracePhase(
        CoopSaveStateBridge::TracePhase phase,
        CoopSaveStateBridge::SchemaSemantic semantic,
        const char* label,
        const void* thisPtr,
        const void* serializerPtr,
        const char* detail,
        bool reading = false,
        int target = eST_SaveGame,
        bool ok = true);
    void EndSaveStateTracePhase(
        uint64_t enterSequence,
        CoopSaveStateBridge::TracePhase phase,
        CoopSaveStateBridge::SchemaSemantic semantic,
        const char* label,
        const void* thisPtr,
        const void* serializerPtr,
        const char* detail,
        bool reading = false,
        int target = eST_SaveGame,
        bool ok = true);
    bool ShouldRecordSaveStateBoundaryProbe(
        const char* label,
        const char* sectionName,
        bool result);
    bool TryPrepareNativePlayerStateXmlPatchFromRoot(const XmlNodeRef& root, const char* reason);
    bool TryPrepareNativePlayerStateXmlPatch(ILoadGame* loadGame, const char* reason);
    void OnArkSaveLoadSerializePersistentStateHook(
        ArkSaveLoadSystem* saveLoadSystem,
        const char* phase,
        bool reading,
        int target,
        bool ok,
        const char* serializerFingerprint,
        std::uintptr_t serializerImpl,
        std::uintptr_t serializerVtable);
    void OnArkPlayerSerializeHook(ArkPlayer* player, const char* functionName, bool reading, int target, bool ok);
    bool PreparePendingPlayerAbilitiesForVanillaPostSerialize(ArkPlayerComponent* component, const char* reason);
    void OnArkPlayerPostSerializeHook(ArkPlayer* player, const char* functionName);
    void OnArkPlayerComponentPostSerializeHook(ArkPlayerComponent* component, const char* functionName);
    void OnArkInventorySnapshotHook(ArkInventory* inventory, const char* functionName, bool reading, int target);
    void OnArkInventorySerializeHook(ArkInventory* inventory, const char* functionName, bool reading, int target, bool ok);
    bool PreparePendingPlayerInventoryForVanillaPostSerialize(ArkInventory* inventory, const char* reason);
    void ObserveNativeGameStateInventoryReferences(
        ArkInventory* inventory,
        bool localPlayerInventory,
        const void* serializerPtr,
        const char* reason);
    void OnArkInventoryPostSerializeHook(ArkInventory* inventory, const char* functionName);
    void OnArkItemSystemSerializeHook(ArkItemSystem* itemSystem, const char* functionName, bool reading, int target, bool ok);
    void OnArkItemSerializeHook(CArkItem* item, const char* functionName, bool reading, int target, bool ok);
    void OnArkItemPostSerializeHook(CArkItem* item, const char* functionName);
    void OnArkItemLifecycleHook(CArkItem* item, const char* functionName);
    void OnArkInventoryMutationHook(const ArkInventory* inventory, const char* functionName, unsigned itemId, unsigned relatedItemId, int x, int y, bool boolResult, unsigned unsignedResult);
    void OnArkItemOwnerMutationHook(CArkItem* item, const char* functionName, unsigned pickerId, const IArkInventory* inventory, bool result);
    void MarkLocalInventoryDirty(const char* reason);
    bool IsLocalInventoryDirtyForSaveKey(const std::string& saveKey) const;
    void ReconcileLocalInventoryDirtyForSaveKey(const std::string& saveKey);
    bool HandleNativeWindowMessage(std::uintptr_t windowHandle, unsigned message, uint64_t wParam, std::int64_t lParam);
    void OnArkItemResetCountHook(CArkItem* item, int count, unsigned ownerIdBefore);
    bool ShouldSuppressPlayerSidecarInventoryFeedback() const;
    void BeginPlayerSidecarInventoryFeedbackSuppression(const char* reason);
    void EndPlayerSidecarInventoryFeedbackSuppression(const char* reason);
    bool EnsureLocalPlayerWeaponRegistered(unsigned itemId, const char* reason);
    bool ResetLocalPlayerWeaponsForInventoryReplacement(const char* reason);
    uint32_t ReconcileLocalPlayerWeaponsFromInventory(const char* reason, bool resetWeaponState);
    void OnArkPlayerCarryThrowHook(ArkPlayerCarry* carry, const char* phase, bool result);
    void OnArkPlayerCarryStopHook(EntityId entityId, float impulse, bool applyAngularImpulse, bool thrown, bool fromSerialize);
    std::string GetSaveGamePathOverrideForHook(const char* path, bool quick, int reason, const char* checkpointName);
    void OnCryActionSaveGameRequested(const char* path, bool quick, bool forceImmediate, int reason, bool ignoreDelay, const char* checkpointName);
    void OnCryActionSaveGameReturned(const char* path, bool quick, bool forceImmediate, int reason, bool ignoreDelay, const char* checkpointName, bool result);
    void OnCryActionNotifySaveGame(ISaveGame* saveGame);
    void OnCryActionNotifySaveGamePostVanilla(ISaveGame* saveGame);
    void OnConcreteSaveComplete(void* saveGame, bool successfulSoFar);
    void BeginNativeFinalStreamCapture(const char* saveName);
    void InjectNativeWriteCompleteSections(ISaveGame* saveGame, const char* saveName);
    bool TryReadNativeWriteCompleteFragmentPayloadSection(ILoadGame* loadGame, const char* reason);
    bool TryLoadHostRemoteNativeFragmentPayloadForWriteComplete(
        std::vector<uint8_t>& outPayloadBytes,
        CoopNativeFragmentPayload::ParsedPayload& outParsed,
        std::string& outSource);
    void FlushNativeFinalStreamCapture(const char* phase, bool result);
    void OnActiveSaveWriteComplete(void* activeSave, const char* saveName, void* fileHandleRef, bool result);
    void OnSaveStoreFinalizeToReadStreamObject(void* writeStore, void* finalObject);
    void OnSaveStoreFinalChunkWriteSink(void* sink, const void* bytes, uint32_t byteCount);
    void OnSaveStoreFinalizeDetachedWriteNode(void* writeNode, bool beforeOriginal);
    void OnSaveStoreLoadStoreInitFromFile(void* loadStore, const char* fileName, bool result);
    bool ProbeScratchNativeLoadStoreFromSave(const std::string& savePathOrName, const char* reason);
    bool CaptureNativeReadStoreFragmentFromStoreMap(
        const CoopSaveStoreDecoder::StoreMap& storeMap,
        const char* fileName,
        const char* reason);
    void ProbeNativeReadStoreGraphSource(
        const CoopSaveStoreDecoder::StoreMap& storeMap,
        const char* fileName,
        const char* reason,
        bool forceDeepMaterialize = false,
        bool forceStartAtGameState = false);
    void OnCryActionNotifyLoadGame(ILoadGame* loadGame);
    void OnCryActionLoadGameRequested(const char* path, bool quick, bool ignoreDelay);
    void OnCryActionLoadGameFinished(const char* path, int result);
    void OnFlashUILoadingProgress(int progressAmount);
    void OnFlashUILoadingComplete();
    void OnFlashUILoadingError(const char* error);
    void OnFlashUIShowLoadingScreen();
    void OnFlashUIHideLoadingScreen();
    void OnFlashUILoadtimeUpdate(float frameTime);
    void OnFlashUILoadtimeRender();
    void OnCoopSystemEvent(int event, std::uintptr_t wparam, std::uintptr_t lparam);
    void OnAnimationQueueLogLine(const char* text, bool newLine);
    void OnPhysicalWorldGetEntitiesInBoxHook(const Vec3& ptmin, const Vec3& ptmax, int objtypes, int szListPrealloc);
    std::string BuildCrashBreadcrumbs() const;
    std::string GetLastAreaObjectEvent() const;
    bool ShouldSuppressNativeSpawnInstrumentation() const noexcept;
    bool ShouldTraceNativeNpcSpawn() const;

    void BindDialogueRuntimeId(uint64_t runtimeId, uint64_t storyId);
    uint64_t ResolveDialogueStoryId(uint64_t runtimeId) const;
    void ForgetDialogueRuntimeId(uint64_t runtimeId);
    bool IsRemoteDialogueRuntimeId(uint64_t runtimeId) const;
    bool ShouldSuppressRemoteDialogueRetrigger(uint64_t ruleId);
    bool IsReplayingRemoteDialogue() const noexcept
    {
        return m_remoteDialogueReplayDepth != 0;
    }
    void ObserveLocalDialogueTrigger(
        ArkSpeakerBase* speaker,
        ArkConversation* conversation,
        uint64_t ruleId,
        bool ignoreVoiceRequirement,
        const char* concept,
        const ArkResponseQuery* query,
        int paChannel,
        bool isLiveAudio,
        int priority);
    ArkConversation* InvokeOriginalDialogueTrigger(
        ArkSpeakerBase* speaker,
        uint64_t ruleId,
        bool ignoreVoiceRequirement,
        const char* concept,
        ArkResponseQuery* query,
        int paChannel,
        bool isLiveAudio,
        int priority);

    //---------------------------------------------------------------------------------
    // Mod Initialization
    //---------------------------------------------------------------------------------
    //! Fills in the DLL info during initialization.
    virtual void FillModInfo(ModDllInfoEx& info) override;

    //! Initializes function hooks before they are installed.
    virtual void InitHooks() override;

    //! Called during CSystem::Init, before any engine modules.
    virtual void InitSystem(const ModInitInfo& initInfo, ModDllInfo& dllInfo) override;

    //! Called after CGame::Init
    virtual void InitGame(bool isHotReloading) override;

    //---------------------------------------------------------------------------------
    // Mod Shutdown
    //---------------------------------------------------------------------------------
    //! Called before CGame::Shutdown.
    virtual void ShutdownGame(bool isHotUnloading) override;

    //! Called before CSystem::Shutdown.
    virtual void ShutdownSystem(bool isHotUnloading) override;

    //---------------------------------------------------------------------------------
    // GUI
    //---------------------------------------------------------------------------------
    //! Called just before MainUpdate to draw GUI. Only called when GUI is visible.
    virtual void Draw() override;

    //---------------------------------------------------------------------------------
    // Main Update Loop
    //---------------------------------------------------------------------------------
    //! Earliest point of update in a frame, before CGame::Update. The timer still tracks time for the previous frame.
    virtual void UpdateBeforeSystem(unsigned updateFlags) override {}

    //! Called before physics is updated for the new frame, best point for queuing physics jobs.
    //! This is like FixedUpdate() in Unity (but not FPS-independent). Use gEnv->pTimer->GetFrameTime() for time delta.
    virtual void UpdateBeforePhysics(unsigned updateFlags) override {}

    //! Called after entities have been updated but before FlowSystem and FlashUI.
    //! This is the main update where most game logic is expected to occur.
    //! Should be preferred if you don't need any special behavior.
    virtual void MainUpdate(unsigned updateFlags) override;

    //! Called after most of game logic has been updated, before CCryAction::PostUpdate.
    virtual void LateUpdate(unsigned updateFlags) override {}

    //---------------------------------------------------------------------------------
    // Mod Interfacing
    //---------------------------------------------------------------------------------
    //! Retrieves an interface for the mod.
    // virtual void* QueryInterface(const char *ifaceName) override;

    //! Called after CSystem::Init, after all engine modules and mods have been initialized. Allows your mod to get interfaces from other mods.
    // virtual void Connect(const std::vector<IChairloaderMod*>& mods) override;

private:
    static constexpr std::uintptr_t kInvalidNetworkSocket = ~std::uintptr_t{ 0 };

    class ProxyReviveInteractionListener;
    class SystemEventListener;
    class CoopEntitySystemSink;
    std::unordered_map<uint64_t, uint64_t> m_dialogueStoryIdByRuntimeId;
    struct NativeInventorySerializeScopeState
    {
        uint32_t depth = 0;
        uint64_t scopeSeq = 0;
        std::uintptr_t inventoryPtr = 0;
        std::uintptr_t serializerPtr = 0;
        bool localPlayerInventory = false;
        bool reading = false;
        int target = 0;
        std::string sectionName;
    };

    class CoopRenderListener final : public IChairRenderListener
    {
    public:
        void SetOwner(ModMain* owner) { m_owner = owner; }

    private:
        int GetChairRenderListenerFlags() override;
        void EndFrame() override;

        ModMain* m_owner = nullptr;
    };

    struct EnemyAuthorityState
    {
        struct RemotePositionSample
        {
            Vec3 position = Vec3(ZERO);
            float receivedAtSeconds = -1.0f;
        };

        enum ReadOnlyIntentKind : uint32_t
        {
            ReadOnlyIntentNone = 0,
            ReadOnlyIntentAttention = 1u << 0,
            ReadOnlyIntentMovement = 1u << 1,
            ReadOnlyIntentFacing = 1u << 2,
            ReadOnlyIntentLook = 1u << 3,
            ReadOnlyIntentCombat = 1u << 4,
            ReadOnlyIntentAbility = 1u << 5,
            ReadOnlyIntentMannequin = 1u << 6,
        };

        struct LocalNativeMannequinAction
        {
            uint32_t sequence = 0;
            uint32_t actionFlags = 0;
            int fragmentId = -1;
            int priority = 0;
            uint32_t optionIndex = 0xffffffffu;
            std::array<uint8_t, 12> tagState = {};
            bool tagStateValid = false;
        };

        struct RemoteNativeMannequinAction
        {
            std::shared_ptr<void> lease;
            int fragmentId = -1;
            float queuedAtSeconds = -1000.0f;
        };

        struct AttentionCandidateState
        {
            uint8_t attentionLevel = CoopEnemyAuthorityPolicy::kUnknownAttention;
            uint64_t firstAtLevelOrder = 0;
            uint64_t targetAccountToken = 0;
            uint32_t lastSequence = 0;
            float silentSeconds = 0.0f;
            bool blocked = false;
        };

        EntityId entityId = INVALID_ENTITYID;
        uint64_t netId = 0;
        uint64_t archetypeId = 0;
        uint64_t entityGuid = 0;
        uint64_t stableEnemyId = 0;
        uint64_t sourceStableEnemyId = 0;
        uint64_t rosterAreaId = 0;
        uint32_t rosterVersion = 1;
        uint32_t rosterFlags = 0;
        uint32_t rosterAnnouncedVersion = 0;
        uint32_t ethericDoppelgangerGeneration = 0;
        uint64_t activeEthericDoppelgangerStableEnemyId = 0;
        bool ethericDoppelgangerRelationApplied = false;
        // Etheric Doppelgangers are transient, non-loot bodies. Preserve the
        // native death presentation briefly, then retire the corpse instead
        // of leaving a permanent lootable NPC after an early kill.
        float ethericDoppelgangerDeadSeconds = 0.0f;
        bool ethericDoppelgangerRemovalQueued = false;
        Vec3 lastPosition = Vec3(ZERO);
        Quat lastRotation = Quat::CreateIdentity();
        Vec3 lastSentPosition = Vec3(ZERO);
        Quat lastSentRotation = Quat::CreateIdentity();
        Vec3 remoteTargetPosition = Vec3(ZERO);
        Vec3 remoteRawTargetPosition = Vec3(ZERO);
        // Follow a short receive-time history instead of racing toward the
        // newest 10-20 Hz packet. This adds a little latency but no prediction.
        std::array<RemotePositionSample, 8> remotePositionSamples = {};
        Vec3 remoteInterpolationTargetPosition = Vec3(ZERO);
        Quat remoteTargetRotation = Quat::CreateIdentity();
        Vec3 remoteMoveDirection = Vec3(0.0f, 1.0f, 0.0f);
        Vec3 localNativeMoveDirection = Vec3(0.0f, 1.0f, 0.0f);
        float lastDamage = 0.0f;
        float remoteSpeed = 0.0f;
        float localNativeMoveSpeed = 0.0f;
        float localNativeMovementSeconds = 0.0f;
        float localNativeAttentionSeconds = 0.0f;
        float localAttentionAdvertisementSeconds = 0.0f;
        // Observer AI may report what it wanted to do, but the hook that
        // records this sample still returns before Vanilla mutates the NPC.
        // Only the controlled presentation mixer may consume these fields.
        float localReadOnlyIntentObservedAtSeconds = -1000.0f;
        float localReadOnlyIntentTraceAtSeconds = -1000.0f;
        float localReadOnlyAttentionObservedAtSeconds = -1000.0f;
        float localReadOnlyFacingMixTraceAtSeconds = -1000.0f;
        uint64_t localReadOnlyIntentContextId = 0;
        EntityId localReadOnlyIntentTargetEntityId = INVALID_ENTITYID;
        EntityId localReadOnlyAttentionTargetEntityId = INVALID_ENTITYID;
        Vec3 localReadOnlyAttentionTargetPosition = Vec3(ZERO);
        uint32_t localReadOnlyIntentKinds = ReadOnlyIntentNone;
        uint32_t localReadOnlyIntentFlags = 0;
        uint32_t localReadOnlyIntentCaptures = 0;
        uint32_t localReadOnlyIntentBlocks = 0;
        uint32_t localCombatIntentAllows = 0;
        uint32_t localCombatTargetRetargets = 0;
        uint32_t localCombatEndRearms = 0;
        uint32_t localTurnIntentAllows = 0;
        uint32_t localLookIntentAllows = 0;
        uint32_t localFacingIntentAllows = 0;
        uint32_t localMovementIntentBlocks = 0;
        uint32_t localMannequinCombatAllows = 0;
        uint32_t localReadOnlyFacingMixApplies = 0;
        uint32_t localReadOnlyFacingMixRejects = 0;
        uint32_t remoteLegBlendApplies = 0;
        uint32_t remoteLegBlendFailures = 0;
        uint32_t remoteInterpolationFrames = 0;
        uint32_t remoteInterpolationUnderruns = 0;
        uint8_t remotePositionSampleCount = 0;
        bool remoteInterpolationActive = false;
        bool localReadOnlyAttentionActive = false;
        bool localReadOnlyAttentionTargetPositionValid = false;
        float localAttentionPendingSeconds = 0.0f;
        float localAttentionClaimedSeconds = 0.0f;
        float localAttentionLostSeconds = 0.0f;
        uint64_t clientClaimTickCount = 0;
        uint64_t attentionClaimOrderCounter = 0;
        uint32_t clientClaimDecision = 0;
        uint32_t lastAdvertisedAttentionAuthorityEpoch = 0;
        uint8_t localAttentionLevel = CoopEnemyAuthorityPolicy::kUnknownAttention;
        uint8_t lastAdvertisedAttentionLevel = CoopEnemyAuthorityPolicy::kUnknownAttention;
        uint8_t authorityAttentionLevel = CoopEnemyAuthorityPolicy::kUnknownAttention;
        bool hasAdvertisedAttentionAuthorityEpoch = false;
        float localRotationOverrideSeconds = 0.0f;
        float localAuthorityBlockedSeconds = 0.0f;
        float remoteAuthoritySilentSeconds = 0.0f;
        float remoteAuthorityBlockedSeconds = 0.0f;
        float remoteTransformRewriteLastSeconds = -1.0f;
        float remoteRotationRewriteLastSeconds = -1.0f;
        float remotePositionSampleIntervalSeconds = 0.10f;
        float remoteInterpolationDelaySeconds = 0.0f;
        float remoteMovementHoldSeconds = 0.0f;
        float remoteTargetMotionFilteredSpeed = 0.0f;
        float remoteTargetMotionSeconds = 0.0f;
        float remoteVisualMotionSeconds = 0.0f;
        float remoteVisualSpeed = 0.0f;
        float remoteLegBlendSpeed = 0.0f;
        float remoteLegBlendAngle = 0.0f;
        float remoteLegBlendTraceAtSeconds = -1000.0f;
        bool remoteLegBlendOwned = false;
        float remoteBurstTransformSeconds = 0.0f;
        float remoteActionMotionBlockSeconds = 0.0f;
        float localFocusCombatSeconds = 0.0f;
        float localMannequinStateSeconds = 0.0f;
        float localNativeMannequinStateSeconds = 0.0f;
        float remoteMannequinStateSeconds = 0.0f;
        float localPoseMovementIntentSeconds = 0.0f;
        float localPoseDashIntentSeconds = 0.0f;
        // Raw locomotion bits from the last authority packet. Presentation
        // mixing may retain or add action flags, so it cannot define a new
        // authority Shift/Dash edge.
        uint32_t remoteAuthorityPacketLocomotionFlags = 0;
        uint32_t remoteLocomotionFlags = 0;
        uint32_t remoteTargetMotionFlags = 0;
        uint32_t remoteVisualMotionFlags = 0;
        uint32_t remoteLocomotionLevel = 0;
        uint32_t remoteMannequinSequence = 0;
        uint32_t remoteMannequinFlags = 0;
        uint32_t remoteMannequinAttackKind = 0;
        int32_t remoteMannequinPriority = 0;
        uint32_t remoteBurstSnapSequence = 0;
        uint32_t remoteBurstFxSequence = 0;
        uint32_t localAbilityFxSequence = 0;
        uint16_t remoteMannequinOrdinal = CoopProtocol::kInvalidMannequinOrdinal;
        uint16_t localAbilityFxKind = CoopProtocol::kEnemyAbilityFxNone;
        uint32_t localNativeLocomotionFlags = 0;
        uint32_t localPersistentStatusFlags = 0;
        uint32_t localMannequinFlags = 0;
        uint32_t localMannequinAttackKind = 0;
        uint32_t localMannequinSequence = 0;
        std::array<uint8_t, 12> localMannequinTagState = {};
        bool localMannequinTagStateValid = false;
        bool localMannequinRandomOption = false;
        // One ordered identity space for every exact Vanilla ability outcome.
        // Concrete Mannequin, body-state, and locomotion outcomes must never
        // be compared using independent counters on the observer.
        uint32_t localAuthoritySemanticSequence = 0;
        uint32_t remoteAuthoritySemanticSequence = 0;
        float localLocomotionSemanticSeconds = 0.0f;
        uint64_t localLocomotionSemanticContextId = 0;
        uint32_t localLocomotionSemanticSequence = 0;
        uint8_t localLocomotionSemanticVariant = 0;
        uint64_t localLocomotionSemanticLastContextId = 0;
        uint32_t localLocomotionSemanticLastSequence = 0;
        uint8_t localLocomotionSemanticLastVariant = 0;
        uint64_t remoteLocomotionSemanticContextId = 0;
        uint32_t remoteLocomotionSemanticSequence = 0;
        uint8_t remoteLocomotionSemanticVariant = 0;
        uint64_t remoteLocomotionSemanticAppliedContextId = 0;
        uint32_t remoteLocomotionSemanticAppliedSequence = 0;
        uint8_t remoteLocomotionSemanticAppliedVariant = 0;
        float localPresentationSemanticSeconds = 0.0f;
        uint64_t localPresentationSemanticContextId = 0;
        uint32_t localPresentationSemanticSequence = 0;
        uint8_t localPresentationSemanticVariant = 0;
        uint8_t localPresentationSemanticNativeOutcome = CoopProtocol::kEnemySemanticNativeOutcomeAbilityManager;
        uint64_t localPresentationSemanticLastContextId = 0;
        uint32_t localPresentationSemanticLastSequence = 0;
        uint8_t localPresentationSemanticLastVariant = 0;
        uint8_t localPresentationSemanticLastNativeOutcome = CoopProtocol::kEnemySemanticNativeOutcomeAbilityManager;
        uint64_t remotePresentationSemanticContextId = 0;
        uint32_t remotePresentationSemanticSequence = 0;
        uint8_t remotePresentationSemanticVariant = 0;
        uint8_t remotePresentationSemanticNativeOutcome = CoopProtocol::kEnemySemanticNativeOutcomeAbilityManager;
        uint64_t pendingSemanticContextId = 0;
        EntityId pendingSemanticTargetEntityId = INVALID_ENTITYID;
        float pendingSemanticObservedAtSeconds = -1000.0f;
        uint8_t pendingSemanticVariant = 0;
        const void* pendingSemanticAction = nullptr;
        int pendingSemanticFragmentId = -1;
        uint64_t localSemanticContextId = 0;
        uint32_t localSemanticSequence = 0;
        float localSemanticBoundAtSeconds = -1000.0f;
        uint8_t localSemanticVariant = 0;
        uint64_t localSemanticLastContextId = 0;
        uint32_t localSemanticLastSequence = 0;
        uint8_t localSemanticLastVariant = 0;
        int localSemanticLastFragmentId = -1;
        uint64_t remoteSemanticContextId = 0;
        uint32_t remoteSemanticSequence = 0;
        uint8_t remoteSemanticVariant = 0;
        uint32_t localMimicryEventSequence = 0;
        uint32_t localMannequinPriority = 0;
        uint16_t localMannequinOrdinal = CoopProtocol::kInvalidMannequinOrdinal;
        std::array<uint8_t, 12> remoteMannequinTagState = {};
        bool remoteMannequinTagStateValid = false;
        bool remoteMannequinRandomOption = false;
        int remoteMannequinFragmentId = -1;
        int localMannequinFragmentId = -1;
        int remoteBurstSnapFragmentId = -1;
        int remoteBurstFxFragmentId = -1;
        CoopProtocol::TestMimicStatePacket lastTransmittedStatePacket = {};
        uint32_t deathCommitRepeatsRemaining = 0;
        uint32_t localDeathPresentationEpochSent = 0;
        // A Mimic can report IsDead before its native breakup/loot transition
        // has disabled every AI lane. Never replay the same lethal signal
        // while waiting for that asynchronous Vanilla finalization.
        uint32_t remoteDeathPresentationSignalAppliedSequence = 0;
        bool hasLastPosition = false;
        bool sentDeadState = false;
        bool localAttentionClaimed = false;
        bool remoteLocomotionAuthority = false;
        bool remoteAuthorityBlocked = false;
        bool remoteAuthorityHasAttention = false;
        uint64_t remoteTargetAccountToken = 0;
        uint64_t remotePresentationTargetAccountToken = 0;
        EntityId remotePresentationTargetEntityId = INVALID_ENTITYID;
        uint32_t remotePresentationTargetMannequinSequence = 0;
        uint64_t authorityOwnerAccountToken = 0;
        uint32_t authorityEpoch = 1;
        uint32_t remoteAuthorityLastSequence = 0;
        std::unordered_map<uint64_t, AttentionCandidateState> attentionCandidates;
        bool remoteTransformNeedsAuthoritySnap = false;
        bool localNativeMannequinResolved = false;
        bool localMannequinCarryUntilReplaced = false;
        bool localBurstMannequinSendPending = false;
        bool localMimicryStateKnown = false;
        bool localMimicryActive = false;
        bool localMimicryIgnorePsi = false;
        uint64_t localMimicryTargetGuid = 0;
        uint64_t localMimicryTargetArchetypeId = 0;
        Vec3 localMimicryTargetPosition = Vec3(ZERO);
        EArkNpcMimicryReason localMimicryReason = EArkNpcMimicryReason::none;
        bool remoteMannequinCarryMovement = false;
        bool remoteAuthorityRagdollApplied = false;
        // Native actions may overlap on different Mannequin scopes. Keep one
        // lease per authority action sequence and retire only its matching
        // reliable Exit edge; replacing every action would change Vanilla
        // controller arbitration.
        std::unordered_map<const void*, LocalNativeMannequinAction> localNativeMannequinActions;
        std::unordered_map<uint32_t, RemoteNativeMannequinAction> remoteNativeMannequinActions;
        // A reliable Exit can overtake the slower state snapshot that still
        // contains the just-ended action. Remember exact retired sequences so
        // snapshot repair cannot resurrect a Vanilla action after its Exit.
        std::deque<uint32_t> remoteNativeMannequinRetiredOrder;
        std::unordered_set<uint32_t> remoteNativeMannequinRetiredActions;
        uint32_t remoteNativeMirrorQueuedSequence = 0;
        uint32_t remoteNativeMirrorDiagnosedSequence = 0;
        uint32_t remoteNativeMirrorSuppressedSequence = 0;
        uint32_t remoteNativeMirrorRepairSequence = 0;
        float remoteNativeMirrorRepairWaitSeconds = 0.0f;
        bool hasLastSentPosition = false;
        bool hasLastTransmittedStatePacket = false;
        float localSnapshotSilenceSeconds = 0.0f;
        float remoteBurstFxCooldownSeconds = 0.0f;
    };

    struct EnemyRosterRecord
    {
        uint64_t stableEnemyId = 0;
        uint64_t areaId = 0;
        uint64_t archetypeId = 0;
        uint64_t entityGuid = 0;
        uint64_t sourceStableEnemyId = 0;
        Vec3 spawnPosition = Vec3(ZERO);
        uint32_t version = 0;
        uint32_t flags = 0;
        uint32_t lifecycleGeneration = 0;
        bool raisedPresentationApplied = false;
    };

    struct PendingRemoteCorpsePhantomResult
    {
        EntityId sourceEntityId = INVALID_ENTITYID;
        uint64_t sourceStableEnemyId = 0;
        uint64_t childStableEnemyId = 0;
    };

    struct PendingCorpsePhantomSpawnRequest
    {
        EntityId sourceEntityId = INVALID_ENTITYID;
        uint64_t sourceEnemyNetId = 0;
        uint64_t sourceStableEnemyId = 0;
        uint64_t phantomArchetypeId = 0;
        uint64_t childStableEnemyId = 0;
    };

    struct RemoteEnemyTransformSmoothingResult
    {
        bool moved = false;
        bool rotated = false;
        bool hardPosition = false;
        bool hardRotation = false;
        bool burst = false;
        float positionStep = 0.0f;
        float positionSpeed = 0.0f;
        float rotationAlpha = 1.0f;
        float rotationSpeed = 0.0f;
        float tickSeconds = 0.0f;
    };

    struct EnemyPuppetState
    {
        EntityId entityId = INVALID_ENTITYID;
        uint64_t archetypeId = 0;
        uint32_t lastSequence = 0;
        bool dead = false;
    };

    struct PendingEnemyDeathCommit
    {
        uint64_t archetypeId = 0;
        Vec3 position = Vec3(ZERO);
        Quat rotation = Quat::CreateIdentity();
        CoopProtocol::TestMimicStatePacket statePacket = {};
        bool hasStatePacket = false;
    };

    struct OperatorLaserBinding
    {
        ArkOperatorLaserHelper* helper = nullptr;
        ArkNpc* npc = nullptr;
        EntityId entityId = INVALID_ENTITYID;
        float damagePerSecond = 0.0f;
        float localCombatSeconds = 0.0f;
        float lastNativeUpdateSeconds = -1000.0f;
        uint64_t localContextId = 0;
        EntityId localTargetEntityId = INVALID_ENTITYID;
        bool nativeTurnOffPending = false;
        bool localCombatActive = false;
    };

    struct RemoteEnemyMovementDesireState
    {
        ArkNpcMovementDesire* desire = nullptr;
        EntityId entityId = INVALID_ENTITYID;
        bool added = false;
    };

    using LocalEnemyVanillaControlIntent = CoopEnemyControlPolicy::Intent;

    struct DamageDedupeEntry
    {
        uint64_t key = 0;
        int damageBucket = 0;
        float expiresAt = 0.0f;
        bool localPlayerRaw = false;
        std::string source;
    };

    struct PendingLocalDamageSignal
    {
        bool valid = false;
        float expiresAt = 0.0f;
        EntityId targetId = INVALID_ENTITYID;
        EntityId sourceId = INVALID_ENTITYID;
        EntityId instigatorId = INVALID_ENTITYID;
        EntityId weaponId = INVALID_ENTITYID;
        uint64_t packageId = 0;
        uint32_t packageOrdinal = 0;
        float scale = 1.0f;
        Vec3 position = ZERO;
        Vec3 direction = ZERO;
        int hitType = 0;
    };

    struct PendingEnemyDamageSignal
    {
        HitInfo hitInfo = {};
        float expiresAt = -1000.0f;
        float scale = 0.0f;
        float targetHealthBeforeHit = 0.0f;
        EntityId senderId = INVALID_ENTITYID;
        EntityId instigatorId = INVALID_ENTITYID;
        uint64_t packageId = 0;
        std::array<uint64_t, CoopProtocol::kMaxEnemyDamageSignalValues> signalIds = {};
        std::array<float, CoopProtocol::kMaxEnemyDamageSignalValues> signalValues = {};
        uint16_t signalValueCount = 0;
        bool hasHitInfo = false;
    };

    struct PendingReliablePacket
    {
        std::array<uint8_t, CoopProtocol::kReliablePayloadSize> payload = {};
        uint32_t sequence = 0;
        uint32_t address = 0;
        uint16_t port = 0;
        uint16_t payloadType = 0;
        uint16_t payloadSize = 0;
        uint64_t sourceAccountToken = 0;
        uint32_t worldEpoch = 0;
        uint32_t sendAttempts = 0;
        uint64_t semanticPrimary = 0;
        uint64_t semanticSecondary = 0;
        uint32_t semanticScope = 0;
        uint16_t semanticKind = 0;
        bool hasSemanticKey = false;
        float enqueuedTime = 0.0f;
        float lastSendTime = -1000.0f;
        std::string failurePrefix;
    };

    struct ReliableEndpointState
    {
        uint32_t sendSequence = 0;
        uint32_t recvSequence = 0;
        uint32_t ackedSequence = 0;
        float lastPacketTime = -1.0f;
    };

    struct HostPlayerStateUploadReceive
    {
        uint32_t transferId = 0;
        uint32_t totalBytes = 0;
        uint32_t chunkCount = 0;
        uint32_t receivedChunks = 0;
        uint32_t checksum = 0;
        uint32_t runningChecksum = 0;
        uint64_t accountToken = 0;
        std::string username;
        std::string saveKey;
        std::string receivePath;
    };

    struct RemotePeerPoseSmoothingState
    {
        Vec3 targetPosition = Vec3(ZERO);
        Quat targetRotation = Quat::CreateIdentity();
        Vec3 presentationVelocity = Vec3(ZERO);
        float lastTargetTime = -1.0f;
        uint32_t targetSequence = 0;
        bool valid = false;
        bool hardSnapPending = false;
    };

    struct RemotePeerSession
    {
        uint64_t accountToken = 0;
        uint64_t modelArchetypeId = 0;
        uint64_t levelId = 0;
        uint32_t address = 0;
        uint16_t port = 0;
        uint32_t modBuild = 0;
        uint32_t sessionFlags = 0;
        uint32_t worldEpoch = 0;
        uint32_t levelEpoch = 0;
        std::string username;
        std::string levelName;
        std::string poseAnimationClip;
        std::string weaponVisualPath;
        Vec3 location = Vec3(ZERO);
        EntityId proxyEntityId = INVALID_ENTITYID;
        EntityId weaponVisualEntityId = INVALID_ENTITYID;
        RemotePeerPoseSmoothingState poseSmoothing;
        int weaponVisualSlotIndex = -1;
        uint32_t weaponVisualClass = 0;
        uint32_t lastPoseSequence = 0;
        uint32_t lastPlayerStatusSequence = 0;
        uint32_t lastDamageSequence = 0;
        uint32_t lastEnemyDamageSequence = 0;
        uint32_t lastWorldSyncSequence = 0;
        uint32_t lastWorldReadyEpoch = 0;
        uint32_t lastWorldReadyLevelId = 0;
        uint32_t playerStateSentWorldEpoch = 0;
        uint32_t areaLeaseEpoch = 0;
        uint32_t areaLeaseLevelEpoch = 0;
        uint32_t areaJournalTransferId = 0;
        uint32_t deferredAreaJournalTransferId = 0;
        uint32_t areaJournalTransferTotalBytes = 0;
        uint32_t areaJournalTransferChunkCount = 0;
        uint32_t areaJournalTransferNextChunk = 0;
        uint32_t areaJournalTransferReceivedChunks = 0;
        uint32_t areaJournalTransferChecksum = 0;
        uint32_t areaJournalTransferRunningChecksum = 0;
        uint32_t areaJournalTransferFlags = 0;
        uint32_t areaSnapshotLeaseEpoch = 0;
        uint32_t areaSnapshotLevelEpoch = 0;
        uint32_t areaJournalTransferAddress = 0;
        uint16_t areaJournalTransferPort = 0;
        float zeroGTravelSpeed = 0.0f;
        float zeroGTravelAngle = 0.0f;
        uint64_t areaLeaseAreaId = 0;
        uint64_t areaLeaseAuthorityPeerToken = 0;
        uint64_t areaLeaseExpectedReadySnapshotSequence = 0;
        uint64_t areaSnapshotAreaId = 0;
        uint64_t areaSnapshotHostSaveKeyHash = 0;
        uint64_t areaSnapshotSequence = 0;
        float areaLeaseHandoffWaitSeconds = 0.0f;
        std::string areaLeaseLevelName;
        std::string lastAreaLeaseEvent = "-";
        std::string areaJournalTransferSourcePath;
        std::string areaJournalTransferReceivePath;
        std::string areaJournalTransferUsername;
        std::string areaJournalTransferLevel;
        std::string deferredAreaJournalTransferSourcePath;
        std::string deferredAreaJournalTransferLevel;
        std::string deferredAreaJournalTransferReason;
        std::string pendingRemoteAreaHandoffRequestLevel;
        std::string lastAreaJournalTransferEvent = "-";
        float lastPacketTime = -1.0f;
        float poseQuarantineUntil = -1.0f;
        bool poseQuarantineActive = false;
        bool areaLeaseActive = false;
        bool areaLeaseDebugHoldReleased = false;
        bool areaLeaseLocalReady = false;
        bool areaLeasePeerReady = false;
        bool areaLeaseSnapshotRequested = false;
        bool areaLeaseAwaitingHandoffSnapshot = false;
        bool areaJournalTransferSending = false;
        bool areaJournalTransferReceiving = false;
        bool areaJournalTransferStarted = false;
        bool areaJournalTransferComplete = false;
        bool deferredAreaJournalTransferPending = false;
        bool weaponVisualCrouched = false;
        bool weaponVisualLowCrouched = false;
        bool weaponVisualZeroG = false;
        bool downed = false;
        bool sessionReady = false;
    };

    struct ServerBrowserEntry
    {
        uint64_t hostAccountToken = 0;
        uint64_t levelId = 0;
        uint32_t address = 0;
        uint16_t port = 0;
        uint16_t playerCount = 0;
        uint16_t maxPlayers = 0;
        uint32_t modBuild = 0;
        std::string serverName;
        std::string hostUsername;
        std::string levelName;
        float lastSeenTime = -1.0f;
        int pingMs = -1;
        bool passworded = false;
        bool favorite = false;
    };

    enum class LivePropLeasePhase : uint8_t
    {
        Settled = 0,
        CarriedLocal,
        CarriedRemote,
        FlightLocal,
        FlightRemote,
        Settling,
    };

    struct LivePropState
    {
        EntityId entityId = INVALID_ENTITYID;
        uint64_t guid = 0;
        uint64_t levelId = 0;
        std::string levelName;
        Vec3 position = Vec3(ZERO);
        Quat rotation = Quat::CreateIdentity();
        Vec3 scale = Vec3Constants<float>::fVec3_One;
        Vec3 velocity = Vec3(ZERO);
        Vec3 angularVelocity = Vec3(ZERO);
        Vec3 releaseVelocity = Vec3(ZERO);
        Vec3 releaseAngularVelocity = Vec3(ZERO);
        Vec3 targetPosition = Vec3(ZERO);
        Quat targetRotation = Quat::CreateIdentity();
        Vec3 targetScale = Vec3Constants<float>::fVec3_One;
        uint32_t flags = 0;
        uint32_t lastReceivedSequence = 0;
        float lastQueuedTime = -1000.0f;
        float lastSentTime = -1000.0f;
        float activeUntilTime = -1000.0f;
        float lastAppliedTime = -1000.0f;
        float localAuthorityUntilTime = -1000.0f;
        float remoteAuthorityUntilTime = -1000.0f;
        float forceSendUntilTime = -1000.0f;
        float contactAuthorityUntilTime = -1000.0f;
        float releaseMotionUntilTime = -1000.0f;
        float remoteBallisticUntilTime = -1000.0f;
        float remoteBlendStartTime = -1000.0f;
        float remoteBlendDuration = 0.35f;
        LivePropLeasePhase leasePhase = LivePropLeasePhase::Settled;
        bool dirty = false;
        bool hasSnapshot = false;
        bool activelyMoving = false;
        bool carried = false;
        bool remoteBallisticActive = false;
        bool remoteBallisticJustStarted = false;
        bool remoteLaunchVelocityApplied = false;
        bool pendingRemoteApply = false;
        uint8_t remoteApplyStepsRemaining = 0;
    };

    struct TurretAuthorityState
    {
        EntityId entityId = INVALID_ENTITYID;
        uint64_t ownerAccountToken = 0;
        uint32_t epoch = 0;
        float lastHealth = -1.0f;
        bool fallbackHostAuthority = true;
        bool confirmed = false;
    };

public:
    struct PlayerInventoryItemState
    {
        uint64_t archetypeId = 0;
        int count = 0;
        int x = -1;
        int y = -1;
        unsigned itemId = 0;
        int width = -1;
        int height = -1;
        uint32_t flags = 0;
        int category = -1;
        bool isWeapon = false;
        float weaponCondition = 0.0f;
        int weaponAmmoLoaded = 0;
        int weaponAmmoCount = 0;
        uint32_t weaponModCount = 0;
        uint32_t weaponModTotalLevel = 0;
        std::vector<std::pair<uint64_t, int>> weaponMods;
    };

    struct PlayerAbilityState
    {
        uint64_t abilityId = 0;
        bool acquired = false;
        bool seen = false;
    };

    struct PlayerResearchState
    {
        uint64_t researchId = 0;
        int scanCount = 0;
    };

    struct PlayerChipsetState
    {
        int type = 0; // 0=suit, 1=psychoscope
        uint64_t archetypeId = 0;
        unsigned itemId = 0;
        int slot = -1;
        bool installed = false;
    };

    struct PlayerStatusState
    {
        int status = 0;
        float amount = 0.0f;
        int level = 0;
        bool suspended = false;
    };

    struct PlayerQuickSelectState
    {
        int bank = 0; // 0=controller, 1=keyboard
        int index = -1;
        int type = 0; // ArkQuickSelectComponent::QuickSelectType
        uint64_t stableId = 0; // weapon archetype or psi power enum
    };

    struct PlayerSidecarState
    {
        std::string username;
        std::string levelName;
        Vec3 position = Vec3(ZERO);
        Quat rotation = Quat::CreateIdentity();
        Quat viewRotation = Quat::CreateIdentity();
        float health = 0.0f;
        float maxHealth = 0.0f;
        float psiPoints = 0.0f;
        float oxygen = 0.0f;
        float maxOxygen = 0.0f;
        uint32_t worldEpoch = 0;
        uint32_t flags = 0;
        bool resetTransientState = false;
        bool hasPosition = false;
        bool hasRotation = false;
        bool hasViewRotation = false;
        bool hasHealth = false;
        bool hasPsi = false;
        bool hasOxygen = false;
        bool oxygenConsuming = false;
        bool hasInventory = false;
        bool hasAbilities = false;
        bool hasChipsets = false;
        bool hasStatuses = false;
        bool hasQuickSelect = false;
        bool hasNativeCapture = false;
        uint64_t equippedWeaponArchetypeId = 0;
        std::vector<PlayerInventoryItemState> inventory;
        std::vector<PlayerAbilityState> abilities;
        std::vector<PlayerResearchState> research;
        std::vector<PlayerChipsetState> chipsets;
        std::vector<PlayerStatusState> statuses;
        std::vector<PlayerQuickSelectState> quickSelect;
        NativeSideBlobCaptureState nativeCapture;
    };

    struct NetworkOriginatedCystoidNestZone
    {
        Vec3 position = Vec3(ZERO);
        float seconds = 0.0f;
    };

    struct NetworkOriginatedCystoidEntity
    {
        EntityId entityId = INVALID_ENTITYID;
        Vec3 origin = Vec3(ZERO);
        float seconds = 0.0f;
    };

    void QueueNativePsiCastForHook(EArkPsiPowers power, float impulseSeconds, const char* reason);
    void QueueNativePsiImpactForHook(uint16_t psiFxKind, int psiPower, const Vec3& impactPosition, const char* reason);
    void SetLocalPoseMimicActiveForHook(bool active, EntityId targetEntityId, EntityId mimickedEntityId, const char* reason);
    void QueueLocalCystoidExplodeEventForHook(ArkCystoid* cystoid, const char* reason);
    void QueueLocalCystoidNestTriggerEventForHook(ArkCystoidNest* nest, EntityId forcedTarget, const char* reason);
    void MaybeMarkNetworkOriginatedCystoidForHook(ArkCystoid* cystoid, const char* reason);
    void OnLocalStoryKeyCardCollected(ArkKeyCardComponent* component, uint64_t id, bool changed, const char* reason);
    void OnLocalStoryKeyCodeCollected(ArkKeyCodeComponent* component, uint64_t id, bool changed, const char* reason);
    void OnLocalStoryFabricationPlanGranted(ArkFabricationPlanComponent* component, uint64_t id, int count, bool unlimited, bool changed, const char* reason);
    void OnLocalStoryObjectiveStateChanged(ArkObjectiveComponent* component, uint64_t id, int state, bool showOnHud, bool changed, const char* reason);
    void OnLocalStoryTaskStateChanged(ArkObjectiveComponent* component, uint64_t id, int state, bool changed, const char* reason);
    void OnLocalStoryObjectiveDescriptionChanged(ArkObjectiveComponent* component, uint64_t id, bool changed, const char* reason);
    void OnLocalStoryConversationStatusChanged(ArkResponseManager* manager, uint64_t id, int status, bool changed, const char* reason);
    void OnLocalStoryGlobalBoolChanged(IGameToken* token, bool value, bool changed, const char* reason);
    void OnLocalStoryGlobalIntChanged(IGameToken* token, int32_t value, bool changed, const char* reason);
    void OnLocalStoryGlobalStringChanged(IGameToken* token, const std::string& value, bool changed, const char* reason);
    void OnLocalAreaGameTokenChanged(IGameToken* token, uint16_t eventKind, uint16_t value, int32_t count, const std::string& textValue, const char* reason);
    void OnLocalStoryResponseUsageChanged(ArkResponseManager* manager, uint16_t eventKind, uint64_t id, uint16_t flags, bool changed, const char* reason, ArkSpeakerBase* speaker = nullptr, int paChannel = -1);
    void RememberDialogueSpeakerForHook(ArkSpeakerBase* speaker, const char* reason);
    void ObserveLocalDialogueActivity(uint64_t dialogueId, uint64_t lineId, const char* reason);
    void OnLocalDialogueCompleted(uint64_t dialogueId, bool complete, const char* reason);
    void OnLocalStoryConditionStateChanged(ArkGameStateConditionManager* manager, uint16_t eventKind, uint64_t id, int count, bool changed, const char* reason);
    void OnLocalStoryRemoteEventTriggered(uint64_t id, const char* reason);
    bool OnLocalMainLiftOutageRemoteEvent(uint64_t id, const char* name);
    void OnLocalStoryUtilityStateChanged(ArkUtilityComponent* component, uint16_t eventKind, uint64_t id, bool value, bool changed, const char* reason);
    void OnLocalStoryReadableCollected(void* component, uint16_t eventKind, uint64_t id, bool changed, const char* reason);
    void OnLocalStoryRosterStateChanged(ArkRosterComponent* component, uint16_t eventKind, uint64_t id, bool changed, const char* reason);
    void SetRemoteDoorTraversalPhysics(ArkDoor* door, bool open, const char* reason);
    void OnLocalAreaObjectDoorStateChanged(ArkDoor* door, uint16_t eventKind, bool value, bool changed, const char* reason);
    void OnLocalAreaObjectInteractiveObjectStateChanged(IEntity* entity, uint16_t eventKind, bool value, bool changed, const char* reason);
    void OnLocalAreaObjectSwitchStateChanged(IEntity* entity, bool on, bool changed, const char* reason);
    void OnLocalAreaObjectApexTentacleSpawnerStateChanged(ArkApexTentacleSpawner* spawner, bool enabled, bool changed, const char* reason);
    void OnLocalAreaObjectCargoContainerMotionStarted(ArkCargoContainer* cargo, bool docking, EArkDockingBay bay, EntityId dockingStationId, bool changed, const char* reason);
    void OnLocalAreaObjectCargoContainerDoorsOpened(ArkCargoContainer* cargo, bool changed, const char* reason);
    void OnLocalAreaObjectRotatorStateChanged(IEntity* entity, bool active, bool changed, const char* reason);
    void OnLocalAreaObjectInteractiveScreenStateChanged(ArkInteractiveScreen* screen, uint16_t eventKind, bool value, bool changed, const char* reason);
    void OnLocalAreaObjectWorkstationStateChanged(ArkStationWorldUI* workstation, bool locked, bool changed, const char* reason);
    void OnLocalAreaObjectWorkstationViewChanged(ArkStationWorldUI* workstation, uint16_t state, uint64_t currentId, bool changed, const char* reason);
    void OnLocalAreaObjectWorkstationUtilityPressed(ArkStationWorldUI* workstation, uint64_t utilityButtonId, const char* reason);
    bool ShouldRouteLocalWorkstationUtilityToAreaAuthorityForHook() const;
    void OnLocalAreaObjectInteractiveMachineStateChanged(ArkInteractiveMachine* machine, uint16_t eventKind, bool value, bool changed, const char* reason);
    void OnLocalAreaObjectOperatorDispenserStateChanged(ArkOperatorDispenser* dispenser, uint16_t eventKind, uint16_t value, bool changed, const char* reason);
    void OnLocalAreaObjectFabricatorStateChanged(ArkFabricator* fabricator, uint16_t eventKind, uint16_t value, bool changed, const char* reason);
    void OnLocalAreaObjectElevatorKioskStateChanged(ArkGenericElevatorKiosk* kiosk, uint16_t eventKind, uint16_t value, bool changed, const char* reason);
    void OnLocalAreaObjectGenericElevatorKioskButtonPressed(ArkGenericElevatorKiosk* kiosk, int button, const char* reason);
    void OnLocalHelicopterPassengerStartForHook(const char* reason);
    void OnLocalAreaObjectKioskButtonStateChanged(ArkKiosk* kiosk, int button, uint16_t value, bool changed, const char* reason);
    void OnLocalAreaObjectKioskPresentationChanged(ArkKiosk* kiosk, uint16_t eventKind, int button, uint16_t value, const char* textValue, bool changed, const char* reason);
    void OnLocalAreaObjectKioskButtonPressed(ArkKiosk* kiosk, int button, const char* reason);
    bool ShouldRouteLocalKioskPressToAreaAuthorityForHook(
        ArkKiosk* kiosk,
        ArkGenericElevatorKiosk* elevatorKiosk = nullptr) const;
    bool ShouldSuppressUnownedKioskOutputForHook() const;
    void RecordSuppressedUnownedKioskOutputForHook(ArkKiosk* kiosk, int button);
    void ArmArkElevatorTransitScan(
        const char* reason,
        uint64_t preferredElevatorGuid = 0);
    void ArmKnownKioskActionRecovery(ArkKiosk* kiosk, int button, const char* reason);
    void RegisterOperatorDispenserAssignedOperatorForHook(ArkOperatorDispenser* dispenser, EntityId operatorId, int slotIndex, const char* reason);
    uint32_t RefreshOperatorDispenserStableSpawnIdsForHook(ArkOperatorDispenser* dispenser, const char* reason);
    uint64_t ResolveOperatorDispenserStableSpawnIdForEnemy(EntityId operatorId, const char* reason);
    void OnLocalAreaObjectKeypadStateChanged(ArkKeypad* keypad, uint16_t eventKind, bool value, bool changed, const char* reason);
    void OnLocalAreaObjectKeycardReaderStateChanged(ArkKeycardReader* reader, uint16_t eventKind, bool value, bool changed, const char* reason);
    void OnLocalAreaObjectContainerStateChanged(ArkInventory* inventory, bool open, const char* reason);
    void OnNativeSharedItemDropped(CArkItem* item, int droppedCount, const char* reason);
    bool ShouldDeferNativeSharedItemPickup(CArkItem* item, EntityId pickerId, const char* reason);
    void OnNativeSharedItemPicked(EntityId itemEntityId, EntityId pickerId, bool success, const char* reason);
    void CaptureLocalPlayerPickupRecovery(EntityId pickerId, const char* reason);
    void OnSharedDropEntityRemoved(EntityId entityId);
    bool ShouldDeferSharedStorageOpen(CArkExternalInventoryUI* ui, ArkInventory* inventory, const char* reason);
    void OnSharedStorageTransfer(CArkItem* item, IArkInventory* source, IArkInventory* target, const char* reason);
    void OnSharedStorageClosed(ArkInventory* inventory, const char* reason);
    void OnNativeRecyclerGrenadeDetonated(CArkProjectileRecyclerGrenade* grenade, bool result, const char* reason);
    void OnNativePlayerGrenadeDetonated(CArkProjectileGrenade* grenade, const char* reason);
    bool DebugDetonatePlayerGrenadeResult(const std::string& kindName, std::string& detail);
    void OnNativeLeakAdded(ArkLeakable* leakable, const Vec3& position, const Vec3& direction, float length, const char* reason);
    void OnNativeLeakRemoved(ArkLeakable* leakable, const Vec3& position, const Vec3& direction, float length, const char* reason);
    void OnNativeLeakValveStateChanged(ArkLeakable* leakable, bool open, const char* reason);
    void OnNativeExplosiveTankExploded(ArkExplosiveTank* tank, bool changed, const char* reason);
    void OnNativeSurfaceHazardStateChanged(
        ArkSurfaceHazard* hazard,
        uint16_t state,
        bool changed,
        bool observerLocalPlayerSignalRequest,
        const char* reason);
    void OnNativeAreaHazardStateChanged(ArkAreaHazard* hazard, bool active, bool changed, const char* reason);
    void OnNativeNpcAttentionTargetChanged(
        ArkNpc* npc,
        EntityId targetEntityId,
        bool gained,
        bool delayed);
    void OnNativeElectricalBoxStateChanged(IEntity* entity, bool changed, const char* reason);
    void OnNativeRepairableStateChanged(IEntity* entity, bool changed, const char* reason);
    void OnNativeGravShaftStateChanged(IEntity* entity, bool changed, const char* reason);
    void OnNativePsiLiftFieldStarted(CArkPsiPowerLift* power, bool result, const char* reason);
    void OnNativeBreakableHealthChanged(CArkBreakable* breakable, float before, float after, const char* reason);
    void OnNativeBreakableGlassImpact(
        const EventPhysCollision& collision,
        uint64_t targetGuid,
        int glassSide,
        int glassSlot);
    bool IsApplyingRemoteHazardEvent() const { return m_hazardEventApplyDepth != 0; }
    bool RepairFirstNativeLeak(ArkLeakable* leakable, const char* context, std::string* detail = nullptr);
    bool DebugSpawnPersistentAreaHazard(std::string& detail);
    bool DebugSetNearestPersistentHazardState(bool areaHazard, uint16_t state, std::string& detail);
    bool DebugSetNearestElectricalBoxState(uint16_t state, std::string& detail);
    bool DebugSetNearestRepairableState(uint16_t state, std::string& detail);
    bool DebugSpawnPersistentRepairableObject(std::string& detail);
    bool DebugSpawnPersistentExplosiveTank(std::string& detail, float forwardDistance = 20.0f);
    bool DebugStageAuthoredExplosiveTank(uint64_t targetGuid, float forwardDistance, std::string& detail);
    bool DebugTriggerPersistentExplosiveTank(std::string& detail, uint64_t targetGuid = 0x434f4f5054414e4bull);
    bool DebugExplodePersistentExplosiveTank(std::string& detail, uint64_t targetGuid = 0x434f4f5054414e4bull);
    bool DebugSpawnPersistentGravShaft(std::string& detail);
    bool DebugSetNearestGravShaftState(uint16_t state, std::string& detail);
    void AddMultiplayerToNativeMainMenu(ArkLauncherMenu* menu);
    void AddMultiplayerToNativePauseMenu(ArkPauseMenu* menu);
    void OpenMultiplayerFromNativeMainMenu();
    void OpenMultiplayerFromNativePauseMenu();
    bool ShouldBlockNativeLauncherInput() const;
    bool ShouldBlockNativePauseInput() const;
    bool ShouldSuppressDefocusedNativePauseMenu() const;
    bool HandleFocusedNativePauseMultiplayerInput(const SInputEvent& event);
    void CloseMultiplayerUi(const char* reason);
    void OnLocalNpcPerformedNoticeChanged(ArkNpc* npc, uint64_t npcGuid, bool performedNotice, const char* reason);
    uint64_t m_lastActiveStoryConversationId = 0;

private:

    void EnsurePlayerPortraitTextures();
    void ReleasePlayerPortraitTextures();
    ITexture* GetPlayerPortraitTexture(size_t index) const;

    struct LevelTransitionDoorDebugRow
    {
        EntityId entityId = INVALID_ENTITYID;
        uint64_t entityGuid = 0;
        uint64_t destinationId = 0;
        Vec3 position = Vec3(ZERO);
        std::string name;
        std::string destinationName;
        std::string arkLocationName;
        bool extensionAvailable = false;
        bool locked = false;
        bool inaccessible = false;
        bool trialGated = false;
        bool transitionGo = false;
    };

    IEntity* GetProxyEntity() const;
    ArkNpc* GetProxyNpc() const;
    const char* GetNetworkModeName() const;
    std::string GetLocalUsername() const;
    uint64_t GetLocalAccountToken() const;
    uint64_t GetRemoteAccountToken() const;
    void LoadPersistentConfig();
    bool SavePersistentConfig(const char* reason);
    bool IsSessionFriendlyFireEnabled() const;
    bool EnsureWinsock();
    bool OpenUdpSocket();
    void CloseUdpSocket();
    void TickNetwork(float frameTime);
    void TickSessionSend(float frameTime);
    void TickClientSend(float frameTime);
    void TickHostSend(float frameTime);
    void TickReceivePackets(const char* failurePrefix);
    bool EnsureServerBrowserSocket();
    void CloseServerBrowserSocket();
    void RefreshServerBrowser();
    void TickServerBrowser();
    void HandleServerQuery(const CoopProtocol::ServerQueryPacket& packet, uint32_t fromAddress, uint16_t fromPort);
    void HandleServerAdvertisement(const CoopProtocol::ServerAdvertisementPacket& packet, uint32_t fromAddress);
    void SendSessionReject(CoopProtocol::SessionRejectReason reason, const char* message, uint32_t address, uint16_t port);
    bool BuildLocalPosePacket(CoopProtocol::PlayerPosePacket& packet, float elapsedTime);
    bool BuildSessionHelloPacket(CoopProtocol::SessionHelloPacket& packet);
    bool BuildAreaLeasePacket(CoopProtocol::AreaLeasePacket& packet, CoopProtocol::AreaLeaseCommand command);
    bool BuildRemotePlayerDamagePacket(CoopProtocol::RemotePlayerDamagePacket& packet, const HitInfo& hitInfo, float damage, float healthBefore, float healthAfter, float maxHealth);
    bool BuildPlayerStatusPacket(CoopProtocol::PlayerStatusPacket& packet, uint32_t flags, uint32_t reason, const Vec3& position, const Quat& rotation, float health, float maxHealth);
    bool BuildTestMimicSpawnPacket(CoopProtocol::TestMimicSpawnPacket& packet, const Vec3& position, const Quat& rotation, uint32_t flags);
    bool BuildTestMimicStatePacket(CoopProtocol::TestMimicStatePacket& packet);
    bool BuildEnemyStatePacket(EnemyAuthorityState& state, CoopProtocol::TestMimicStatePacket& packet);
    bool BuildEnemyDamageRequestPacket(
        CoopProtocol::EnemyDamageRequestPacket& packet,
        const HitInfo& hitInfo,
        uint64_t damagePackageId,
        float damagePackageScale);
    bool BuildWorldSyncPacket(CoopProtocol::WorldSyncPacket& packet, CoopProtocol::WorldSyncCommand command, uint32_t flags);
    bool BuildSaveTransferPacket(CoopProtocol::SaveTransferPacket& packet, CoopProtocol::SaveTransferCommand command, uint32_t transferId);
    bool BuildPlayerStateTransferPacket(CoopProtocol::PlayerStateTransferPacket& packet, CoopProtocol::PlayerStateTransferCommand command, uint32_t transferId);
    bool BuildAreaJournalTransferPacket(CoopProtocol::AreaJournalTransferPacket& packet, CoopProtocol::AreaJournalTransferCommand command, uint32_t transferId);
    bool SendPacketTo(const void* packet, int packetSize, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendChatDatagram(const void* packet, int packetSize, const char* failurePrefix);
    void InitializeChat();
    void ShutdownChat();
    bool HandleChatWindowMessage(unsigned message, uint64_t wParam, int64_t lParam);
    void TickChat(float frameTime, float nowSeconds);
    bool IsChatInputOpen() const;
    std::string BuildChatTelemetry() const;
    bool SendChatTextCommand(const std::string& text);
    void ResetChat();
    void RemoveChatSender(uint64_t accountToken);
    bool CanAcceptChatTextPacket(const CoopProtocol::TextChatPacket& packet) const;
    bool HandleChatTextPacket(const CoopProtocol::TextChatPacket& packet, const std::string& username, float nowSeconds);
    bool HandleChatDatagram(
        const CoopProtocol::PacketHeader& header,
        const void* packetData,
        int packetBytes,
        uint32_t fromAddress,
        uint16_t fromPort);
    bool AcceptChatTextRate(uint64_t accountToken, float nowSeconds);
    bool SendPosePacketTo(const CoopProtocol::PlayerPosePacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendSessionHelloTo(uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendAreaLeaseTo(const CoopProtocol::AreaLeasePacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendRemotePlayerDamageTo(const CoopProtocol::RemotePlayerDamagePacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendPlayerStatusTo(const CoopProtocol::PlayerStatusPacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendTestMimicSpawnTo(const CoopProtocol::TestMimicSpawnPacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendTestMimicStateTo(
        const CoopProtocol::TestMimicStatePacket& packet,
        uint32_t address,
        uint16_t port,
        const char* failurePrefix,
        uint64_t excludedAccountToken = 0);
    bool DecodeTestMimicStateDatagram(const void* data, size_t dataSize, CoopProtocol::TestMimicStatePacket& packet);
    bool SendEnemyRosterTo(const CoopProtocol::EnemyRosterPacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendCorpsePhantomRequestTo(const CoopProtocol::CorpsePhantomRequestPacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendSharedDropTo(const CoopProtocol::SharedDropPacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendSharedStorageTo(const CoopProtocol::SharedStoragePacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendHazardEventTo(const CoopProtocol::HazardEventPacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendDialogueLeaseTo(const CoopProtocol::DialogueLeasePacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendTimeDilationTo(const CoopProtocol::TimeDilationPacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendEnemyDamageRequestTo(const CoopProtocol::EnemyDamageRequestPacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendEnemyDeathPresentationTo(const CoopProtocol::EnemyDeathPresentationPacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendWorldSyncTo(const CoopProtocol::WorldSyncPacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendSaveTransferTo(const CoopProtocol::SaveTransferPacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendPlayerStateTransferTo(const CoopProtocol::PlayerStateTransferPacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendAreaJournalTransferTo(const CoopProtocol::AreaJournalTransferPacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendLivePropTransformTo(const CoopProtocol::LivePropTransformPacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendGooResultTo(const CoopProtocol::GooResultPacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendEnemyProjectileEventTo(const CoopProtocol::EnemyProjectileEventPacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendEnemyAbilityFxEventTo(const CoopProtocol::EnemyAbilityFxEventPacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendEnemyMannequinActionTo(const CoopProtocol::EnemyMannequinActionPacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool BuildStoryEventPacket(CoopProtocol::StoryEventPacket& packet, uint16_t eventKind, uint64_t targetId, int32_t count, uint16_t flags, const char* reason, const char* textValue = nullptr);
    bool SendStoryEventTo(const CoopProtocol::StoryEventPacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool BuildAreaObjectEventPacket(CoopProtocol::AreaObjectEventPacket& packet, uint16_t eventKind, uint64_t targetGuid, uint16_t value, uint32_t flags, const char* reason, int32_t count = 0, const char* textValue = nullptr);
    bool SendAreaObjectEventTo(const CoopProtocol::AreaObjectEventPacket& packet, uint32_t address, uint16_t port, const char* failurePrefix);
    bool SendDisconnectNoticeTo(uint32_t address, uint16_t port, uint32_t reason, const char* failurePrefix);
    bool SendReliablePayloadTo(uint16_t payloadType, const void* payload, uint16_t payloadSize, uint32_t address, uint16_t port, const char* failurePrefix);
    bool QueueReliablePayloadToEndpoint(uint16_t payloadType, const void* payload, uint16_t payloadSize, uint32_t address, uint16_t port, uint64_t sourceAccountToken, const char* failurePrefix);
    bool SendReliableEnvelopeNow(PendingReliablePacket& pending, bool resend);
    bool SendReliableAckTo(uint32_t ackSequence, uint32_t address, uint16_t port, const char* failurePrefix);
    void TickReliableTransport(float frameTime);
    void TickPeerTimeout(float frameTime);
    void DisconnectRemotePeer(const char* reason);
    void ClearPeerTimeoutWarning(const char* reason);
    void ReturnClientToMultiplayerFlowAfterDisconnect(const char* reason);
    void QueueNetworkRuntimeCleanup(const char* reason);
    void SoftQuarantineNetworkRuntimeEntitiesForDisconnect(const char* reason);
    void TickNetworkRuntimeCleanup(float frameTime);
    void HandleReliableAck(const CoopProtocol::ReliableAckPacket& packet, uint32_t fromAddress, uint16_t fromPort);
    void HandleReliableEnvelope(const CoopProtocol::ReliableEnvelopePacket& packet, uint32_t fromAddress, uint16_t fromPort);
    void HandleReliablePayload(uint16_t payloadType, const uint8_t* payload, uint16_t payloadSize);
    void HandleSessionHello(const CoopProtocol::SessionHelloPacket& packet, uint32_t fromAddress, uint16_t fromPort);
    void HandlePeerPresence(const CoopProtocol::PeerPresencePacket& packet);
    bool SendPeerPresenceTo(const RemotePeerSession& peer, CoopProtocol::PeerPresenceCommand command, uint32_t address, uint16_t port, const char* failurePrefix);
    void BroadcastPeerPresence(const RemotePeerSession& peer, CoopProtocol::PeerPresenceCommand command, uint64_t excludedAccountToken = 0);
    void RelayReliablePayloadToPeers(const CoopProtocol::ReliableEnvelopePacket& packet, uint32_t fromAddress, uint16_t fromPort);
    void RelayPoseToPeers(const CoopProtocol::PlayerPosePacket& packet, uint32_t fromAddress, uint16_t fromPort);
    void RelayDatagramToPeers(const void* packet, int packetSize, uint32_t fromAddress, uint16_t fromPort, const char* failurePrefix);
    void RelayChatTextToPeers(const void* packet, int packetSize, uint32_t fromAddress, uint16_t fromPort, const char* failurePrefix);
    bool RouteRemoteAreaEnemyStateDatagram(
        const void* packet,
        int packetSize,
        uint32_t fromAddress,
        uint16_t fromPort);
    bool ResolveSessionHostEndpoint(uint32_t& address, uint16_t& port) const;
    void StoreActiveRemotePeerContext();
    bool ActivateRemotePeerContext(uint64_t accountToken);
    RemotePeerSession* FindRemotePeerByEndpoint(uint32_t address, uint16_t port);
    const RemotePeerSession* FindRemotePeerByEndpoint(uint32_t address, uint16_t port) const;
    void RetireRemotePeerProxyForAreaChange(RemotePeerSession& peer, const char* reason);
    void RemoveRemotePeer(uint64_t accountToken, const char* reason, bool announce);
    void KickRemotePeer(uint64_t accountToken);
    void ClearRemotePeerPoseSmoothing(RemotePeerSession& peer);
    void BeginRemotePeerPoseQuarantine(RemotePeerSession& peer);
    void ResetRemotePeerPoseSmoothing(
        RemotePeerSession& peer,
        const Vec3& position,
        const Quat& rotation,
        bool hardSnapPending);
    bool UpdateRemotePeerPoseTarget(
        RemotePeerSession& peer,
        const CoopProtocol::PlayerPosePacket& packet,
        bool forceHardSnap = false,
        bool* hardSnapApplied = nullptr);
    void TickRemotePlayerProxySmoothing(float frameTime);
    void ApplyAdditionalRemotePose(RemotePeerSession& peer, const CoopProtocol::PlayerPosePacket& packet);
    static uint64_t MakeEndpointKey(uint32_t address, uint16_t port);
    void HandleAreaLease(const CoopProtocol::AreaLeasePacket& packet);
    void HandleDisconnectNotice(const CoopProtocol::DisconnectNoticePacket& packet);
    void HandleRemotePlayerDamage(const CoopProtocol::RemotePlayerDamagePacket& packet);
    void HandlePlayerStatus(const CoopProtocol::PlayerStatusPacket& packet);
    void HandleTestMimicSpawn(const CoopProtocol::TestMimicSpawnPacket& packet);
    void HandleTestMimicState(const CoopProtocol::TestMimicStatePacket& packet);
    void HandleEnemyRoster(const CoopProtocol::EnemyRosterPacket& packet);
    void HandleCorpsePhantomRequest(const CoopProtocol::CorpsePhantomRequestPacket& packet);
    void HandleSharedDrop(const CoopProtocol::SharedDropPacket& packet);
    void HandleSharedStorage(const CoopProtocol::SharedStoragePacket& packet);
    void HandleHazardEvent(const CoopProtocol::HazardEventPacket& packet);
    void HandleDialogueLease(const CoopProtocol::DialogueLeasePacket& packet);
    void HandleTimeDilation(const CoopProtocol::TimeDilationPacket& packet);
    void HandleEnemyDamageRequest(const CoopProtocol::EnemyDamageRequestPacket& packet);
    void HandleEnemyDeathPresentation(const CoopProtocol::EnemyDeathPresentationPacket& packet);
    void HandleLivePropTransform(const CoopProtocol::LivePropTransformPacket& packet);
    void HandleGooResult(const CoopProtocol::GooResultPacket& packet);
    void HandleEnemyProjectileEvent(const CoopProtocol::EnemyProjectileEventPacket& packet);
    void HandleEnemyAbilityFxEvent(const CoopProtocol::EnemyAbilityFxEventPacket& packet);
    void HandleEnemyMannequinAction(const CoopProtocol::EnemyMannequinActionPacket& packet);
    void HandleClientTurretStateRequest(const CoopProtocol::EnemyAbilityFxEventPacket& packet);
    void HandleStoryEvent(const CoopProtocol::StoryEventPacket& packet);
    void HandleAreaObjectEvent(const CoopProtocol::AreaObjectEventPacket& packet);
    void ResetStoryEventState(const char* lastEvent);
    void ResetAreaObjectEventState(const char* lastEvent);
    bool QueueLocalStoryEventForHook(uint16_t eventKind, uint64_t targetId, int32_t count, uint16_t flags, const char* reason, const char* textValue = nullptr, uint64_t contextEntityGuid = 0, uint64_t contextCharacterId = 0, uint16_t contextFlags = 0, int32_t contextChannel = -1);
    bool ApplyStoryEventMutation(const CoopProtocol::StoryEventPacket& packet, std::string& detail);
    bool QueueLocalAreaObjectEventForHook(uint16_t eventKind, uint64_t targetGuid, uint16_t value, uint32_t flags, const char* reason, int32_t count = 0, const char* textValue = nullptr);
    bool ApplyAreaObjectEventMutation(const CoopProtocol::AreaObjectEventPacket& packet, std::string& detail);
    void FinalizeAppliedAreaObjectEvent(const CoopProtocol::AreaObjectEventPacket& packet, const std::string& detail);
    bool ShouldCaptureRemoteAreaObjectConsequenceForHook(uint16_t eventKind) const;
    void TickRemoteDoorPowerConvergence(float frameTime);
    void TickArkElevatorTransitScan(float frameTime);
    bool ObserveArkElevatorTransitForAuthority(IEntity* entity);
    bool IsLocalPlayerInsideMainLiftShaft() const;
    void TickMainLiftOutageEvent(float frameTime);
    void TickMainLiftPassengerCarry(float frameTime);
    void TickKnownKioskActionRecovery(float frameTime);
    bool ApplyAreaObjectWorkstationView(const CoopProtocol::AreaObjectEventPacket& packet, std::string& detail);
    bool ApplyAreaObjectWorkstationUtilityPressed(const CoopProtocol::AreaObjectEventPacket& packet, std::string& detail);
    bool IsLocalStoryKeyCardComponent(const ArkKeyCardComponent* component) const;
    bool IsLocalStoryKeyCodeComponent(const ArkKeyCodeComponent* component) const;
    bool IsLocalStoryFabricationPlanComponent(const ArkFabricationPlanComponent* component) const;
    bool IsLocalStoryObjectiveComponent(const ArkObjectiveComponent* component) const;
    bool IsLocalStoryUtilityComponent(const ArkUtilityComponent* component) const;
    bool IsLocalStoryReadableComponent(const void* component, uint16_t eventKind) const;
    bool IsLocalStoryRosterComponent(const ArkRosterComponent* component) const;
    bool IsLocalStoryResponseManager(const ArkResponseManager* manager) const;
    bool IsLocalStoryConditionManager(const ArkGameStateConditionManager* manager) const;
    ArkObjectiveComponent* GetStoryObjectiveComponent() const;
    ArkResponseManager* GetStoryResponseManager() const;
    ArkGameStateConditionManager* GetStoryConditionManager() const;
    IGameTokenSystem* GetStoryGameTokenSystem() const;
    uint64_t HashStoryString(const std::string& text) const;
    uint64_t BuildStoryEventId(uint16_t eventKind, uint64_t targetId, int32_t count, uint16_t flags, uint32_t sequence) const;
    uint64_t BuildAreaObjectEventId(uint16_t eventKind, uint64_t targetGuid, uint16_t value, int32_t count, uint32_t flags, uint32_t sequence, const char* textValue) const;
    void OnLocalAreaObjectWorldItemRemoved(CArkItem* item, bool changed, const char* reason);
    bool ApplyAreaObjectWorldItemRemoved(uint64_t targetGuid, std::string& detail);
    void InitBreakableSyncHooks();
    bool ApplyAreaObjectBreakableHealth(uint64_t targetGuid, uint16_t encodedHealth, std::string& detail);
    bool ApplyAreaObjectBreakableGlassImpact(const CoopProtocol::AreaObjectEventPacket& packet, std::string& detail);
    bool DebugSpawnBreakableSyncTarget(std::string& detail, bool scalable = false);
    bool DebugProbeBreakableSyncTarget(std::string& detail, uint64_t targetGuid = 0);
    bool DebugSetBreakableSyncTargetHealth(float health, std::string& detail, uint64_t targetGuid = 0);
    bool DebugBreakableGlassCommand(const std::string& target, bool impact, std::string& detail);
    struct SharedDropRecord
    {
        uint64_t stableSpawnId = 0;
        uint64_t areaId = 0;
        uint64_t archetypeId = 0;
        uint64_t ownerPeerHash = 0;
        EntityId localEntityId = INVALID_ENTITYID;
        uint32_t version = 0;
        int count = 0;
        bool live = false;
        bool pickupPending = false;
        bool localPickupGranted = false;
        bool nativePickupInProgress = false;
        uint64_t pickupWinnerPeerHash = 0;
    };
    uint64_t BuildSharedDropStableId(uint64_t archetypeId, const Vec3& position, uint32_t sequence) const;
    bool BuildSharedDropPacket(CoopProtocol::SharedDropPacket& packet, CoopProtocol::SharedDropCommand command, const SharedDropRecord& record, uint64_t targetPeerHash = 0) const;
    bool MaterializeSharedDrop(const CoopProtocol::SharedDropPacket& packet, SharedDropRecord& record, std::string& detail);
    bool RemoveSharedDropLocal(SharedDropRecord& record, bool grantToLocalPlayer, std::string& detail);
    void ResetSharedDropState(const char* reason);
    struct SharedStorageRecord
    {
        uint64_t guid = 0;
        uint64_t areaId = 0;
        uint64_t leaseOwnerHash = 0;
        EntityId localEntityId = INVALID_ENTITYID;
        uint32_t version = 1;
        uint32_t leaseEpoch = 0;
        float leaseSeconds = 0.0f;
        bool registered = false;
        bool localGrantReceived = false;
        uint32_t localGrantTransaction = 0;
    };
    struct SharedStorageAssembly
    {
        uint64_t guid = 0;
        uint64_t sourcePeerHash = 0;
        uint32_t transactionId = 0;
        uint32_t storageVersion = 0;
        uint32_t leaseEpoch = 0;
        uint16_t expectedItems = 0;
        bool endReceived = false;
        bool commit = false;
        std::vector<PlayerInventoryItemState> items;
        std::vector<bool> received;
    };
    bool BuildSharedStoragePacket(CoopProtocol::SharedStoragePacket& packet, CoopProtocol::SharedStorageCommand command, const SharedStorageRecord& record, uint64_t targetPeerHash = 0) const;
    bool SendSharedStorageSnapshot(SharedStorageRecord& record, ArkInventory& inventory, bool commit, bool releaseAfterCommit, uint64_t targetPeerHash, const char* reason);
    bool TryFinalizeSharedStorageAssembly(uint64_t guid, std::string& detail);
    bool CaptureSharedStorageInventory(ArkInventory& inventory, std::vector<PlayerInventoryItemState>& items, std::string& detail);
    bool ReplaceSharedStorageInventory(ArkInventory& inventory, const std::vector<PlayerInventoryItemState>& items, std::string& detail);
    ArkInventory* ResolveSharedStorageInventory(uint64_t guid, EntityId* outEntityId, std::string& detail) const;
    bool RegisterSharedStorage(uint64_t guid, bool broadcast, const char* reason);
    bool IsSharedStorageInventory(const ArkInventory* inventory, uint64_t* outGuid = nullptr) const;
    void ResetSharedStorageState(const char* reason);
    void TickSharedStorage(float frameTime);
    uint64_t BuildHazardEventId(uint16_t eventKind, uint64_t archetypeId, const Vec3& position, uint32_t sequence) const;
    bool QueueLocalLeakHazardEvent(ArkLeakable* leakable, CoopProtocol::HazardEventKind kind, const Vec3& position, const Vec3& direction, float length, uint16_t flags, const char* reason);
    bool QueueLocalPersistentHazardState(IEntity* entity, CoopProtocol::HazardEventKind kind, uint16_t state, const char* reason);
    void ResetHazardEventState(const char* reason);
    bool ApplyEnemyDeathCommitToLocal(
        uint64_t enemyNetId,
        uint64_t archetypeId,
        const Vec3* position,
        const Quat* rotation,
        const CoopProtocol::TestMimicStatePacket* deathPacket,
        const char* reason);
    void TickPendingEnemyDeathCommits(float frameTime);
    bool BuildDialogueLeasePacket(
        CoopProtocol::DialogueLeasePacket& packet,
        CoopProtocol::DialogueLeaseCommand command,
        uint64_t dialogueId,
        uint64_t targetPeerHash = 0) const;
    bool CaptureDialogueTriggerPacket(
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
        std::string& detail);
    bool CaptureDialogueSpeakerIdentity(
        ArkSpeakerBase* speaker,
        int paChannel,
        uint64_t& speakerEntityGuid,
        uint64_t& speakerCharacterId,
        uint16_t& speakerIdentityFlags,
        int32_t& resolvedPaChannel,
        std::string& detail) const;
    bool RequestDialogueLease(
        const CoopProtocol::DialogueLeasePacket& request,
        const char* reason);
    bool ReleaseLocalDialogueLease(const char* reason);
    bool ReplayGrantedDialogue(
        const CoopProtocol::DialogueLeasePacket& packet,
        std::string& detail);
    ArkSpeakerBase* ResolveDialogueSpeaker(
        uint64_t speakerEntityGuid,
        uint64_t speakerCharacterId,
        uint16_t flags,
        int32_t paChannel,
        std::string& detail);
    bool ShouldSuppressDialogueStoryEvent(uint64_t id) const;
    void ClearActiveDialogueLease(const char* reason);
    void TickDialogueLease(float frameTime);
    void ResetDialogueLeaseState(const char* reason);
    bool BuildTimeDilationPacket(CoopProtocol::TimeDilationPacket& packet, CoopProtocol::TimeDilationCommand command, unsigned timers, float scale) const;
    void ResetTimeDilationState(const char* reason);
    uint64_t CurrentHostSaveKeyHash() const;
    bool IsCurrentOrRecentHostSaveKeyHash(uint64_t hash) const;
    void SetCurrentHostSaveStateKey(const std::string& saveKey, bool retainPrevious);
    bool BuildLivePropTransformPacket(CoopProtocol::LivePropTransformPacket& packet, const LivePropState& state);
    bool CaptureLivePropState(IEntity& entity, bool removed, LivePropState& outState, std::string& reason, bool allowCarryableItem = false) const;
    bool ShouldTrackLivePropEntity(IEntity& entity, std::string& reason, bool allowCarryableItem = false) const;
    void RetireLivePropTrackingForSharedDrop(EntityId entityId);
    IEntity* ResolveLivePropEntity(uint64_t guid, EntityId cachedEntityId, std::string& reason);
    const char* GetLivePropLeasePhaseName(LivePropLeasePhase phase) const;
    bool IsLivePropLocalLeasePhase(LivePropLeasePhase phase) const;
    bool IsLivePropRemoteLeasePhase(LivePropLeasePhase phase) const;
    bool IsLivePropRemoteProtected(const LivePropState& state, float now) const;
    void QueueLivePropTransform(IEntity& entity, bool removed, const char* reason);
    void SeedLivePropsAfterAreaOverlay(const std::vector<uint64_t>& transformedGuids);
    void PromoteAreaOverlayLiveProp(LivePropState&& snapshot, float authorityUntilTime);
    void HandleLivePropCollisionEvent(IEntity& entity, const SEntityEvent& event);
    bool QueueLivePropXformContactAuthority(IEntity& entity, const char* reason);
    bool QueueLivePropXformLocalPlayerAuthority(IEntity& entity, const char* reason);
    bool IsLocalLivePropImpulseSource(EntityId sourceEntityId, bool allowUnattributedSource, std::string* detail = nullptr);
    bool QueueLivePropCollisionAuthority(
        IEntity& entity,
        bool chained,
        bool localPlayerContact,
        bool localCarryContact,
        bool localAttackContact,
        bool localEnemyBodyContact,
        const char* reason);
    void TickLivePropSync(float frameTime);
    void ResetLivePropSyncState(const char* reason);
    bool ApplyLivePropRemoteState(IEntity& entity, LivePropState& state, bool finalStep, std::string& reason);
    bool ApplyLivePropRemoteBallisticState(IEntity& entity, LivePropState& state, std::string& reason);
    void ResetLivePropDebugTrace(uint64_t focusGuid, const char* reason);
    void AppendLivePropDebugTrace(const char* stage, uint64_t guid, EntityId entityId, const LivePropState* state, const char* detail);
    void HandleWorldSync(const CoopProtocol::WorldSyncPacket& packet);
    void HandleSaveTransfer(const CoopProtocol::SaveTransferPacket& packet);
    void HandleAreaJournalTransfer(const CoopProtocol::AreaJournalTransferPacket& packet);
    void SendHostWorldOffer(bool loadCommand);
    void QueueClientHostWorldRequest();
    void SendClientWorldReady();
    void TickWorldSyncControl(float frameTime);
    bool TryLoadHostWorldSave(const CoopProtocol::WorldSyncPacket& packet);
    void ResetWorldSyncControlState(const char* lastEvent);
    bool BeginHostSaveTransfer();
    bool RequestHostSaveTransferSnapshot();
    void TickSaveTransfer(float frameTime);
    bool StartHostSaveTransferFromFile(const std::string& sourcePath, uint32_t transferId);
    bool QueueNextHostSaveTransferPacket();
    bool TryLoadReceivedHostSave();
    void ResetSaveTransferState(const char* lastEvent);
    bool BeginHostPlayerStateTransfer(const char* reason, const std::string& requestedSaveKey = {});
    bool BeginClientPlayerStateUpload(const char* reason, const std::string& saveKey = {});
    void QueueClientPlayerStateUpload(const char* reason, const std::string& saveKey = {});
    bool BeginClientIntentionalDisconnect(const char* reason, bool captureImmediately = true);
    void TickClientIntentionalDisconnect(float frameTime);
    bool CaptureClientDisconnectSnapshot();
    void ResumeDeferredNativeWindowClose();
    bool RequestRemotePlayerStateUpload(const char* reason);
    bool BroadcastHostSaveIdentity(const char* reason);
    bool StartClientNativePlayerSnapshotForUpload(const char* reason, const std::string& saveKey);
    bool HasUsableNativePlayerSaveCaptureForCurrentLevel(uint32_t minGeneration = 0) const;
    bool ShouldUseNativePlayerStatePreloadMerge() const;
    bool EnforceClientPlayerStateApplyInvariant(const char* reason);
    bool PrepareReceivedPlayerStateForHostLoad(const char* reason);
    bool PrepareReceivedHostSaveForNativePlayerMerge(const char* reason);
    bool QueueAuthoritativePlayerInventoryRestore(const PlayerSidecarState& state, const char* reason);
    bool TryMergeReceivedPlayerStateDuringNativeLoad(const char* reason);
    bool StartPlayerStateTransferFromFile(const std::string& sourcePath, const std::string& username, uint32_t flags, uint32_t transferId, const std::string& saveKey = {});
    bool QueueNextPlayerStateTransferPacket();
    void TickPlayerStateTransfer(float frameTime);
    void HandlePlayerStateTransfer(const CoopProtocol::PlayerStateTransferPacket& packet);
    void ResetPlayerStateTransferState(const char* lastEvent);
    bool TryApplyReceivedPlayerStateTransfer(const char* reason);
    void SendHostLoadStartingNotice(const char* reason);
    bool BeginAreaJournalTransfer(const char* reason, const std::string& requestedLevelName = {});
    bool ShouldSuppressHostAreaJournalForRemoteOwnedLevel(const std::string& levelName) const;
    bool ExportAreaJournalTransferFile(const std::string& levelName, uint32_t transferId, std::string& outPath);
    bool QueueDeferredAreaJournalTransfer(const char* reason, const std::string& requestedLevelName);
    bool TryStartDeferredAreaJournalTransfer(const char* reason);
    bool StartAreaJournalTransferFromFile(const std::string& sourcePath, const std::string& levelName, uint32_t transferId);
    bool QueueNextAreaJournalTransferPacket();
    void TickAreaJournalTransfer(float frameTime);
    void TickAreaJournalTransferForActivePeer(float frameTime);
    bool RequestRemoteAreaJournal(const char* reason, const std::string& requestedLevelName = {});
    bool StoreReceivedAreaJournalTransfer(const char* reason);
    bool MergeReceivedAreaJournalIntoServerState(const char* reason, bool allowLocalLiveMerge = false);
    bool BeginServerAreaStateTransfer(const std::string& requestedLevelName, const char* reason);
    bool QueueAreaStateOverlayApply(const std::string& levelName, const std::string& sourcePath, const char* reason);
    bool QueueServerAreaStateOverlayForLevel(const std::string& levelName, const char* reason);
    bool QueueReceivedAreaStateOverlayForLevel(const std::string& levelName, const char* reason);
    void TickAreaStateOverlayApply(float frameTime);
    bool TryApplyQueuedAreaStateOverlay(const char* reason);
    bool TryRequestPendingRemoteAreaHandoff(const char* reason);
    void ResetAreaJournalTransferState(const char* lastEvent);
    std::string GetHostPlayerStatePathForUsername(const std::string& username) const;
    std::string GetHostPlayerStatePathForUsernameAndSave(const std::string& username, const std::string& saveKey) const;
    std::string GetHostPlayerStatePathForAccount(uint64_t accountToken) const;
    std::string GetHostPlayerStatePathForAccountAndSave(uint64_t accountToken, const std::string& saveKey) const;
    std::string BuildHostSaveStateKey(const std::string& savePathOrName) const;
    std::string BuildLegacyHostSaveStateKey(const std::string& savePathOrName) const;
    bool WriteDefaultHostPlayerStateFile(const std::string& path, const std::string& username);
    uint32_t SnapshotLatestHostPlayerStatesForSave(const std::string& saveKey, const char* reason);
    bool IsSessionGameplayReady() const;
    bool IsEnemyReplicationGameplayReady() const;
    bool IsClientAreaAuthorityActive() const;
    void TickAreaLease(float frameTime);
    void TickHostAreaLeaseForActivePeer(float frameTime);
    bool ReleaseActiveAreaLease(const char* reason, bool holdReleasedForDebug = false);
    bool SendClientAreaLeaseCommand(CoopProtocol::AreaLeaseCommand command, const char* reason);
    void TickClientAreaAuthority(float frameTime);
    void ResetAreaLeaseState(const char* reason);
    void UpdateSessionGate();
    void ResetReplicationSequenceGuardsForWorldChange(const char* reason);
    void DetachRuntimeEntitiesForIncomingWorldReset(const char* reason);
    std::string GetCurrentLevelName() const;
    uint64_t HashLevelName(const std::string& levelName) const;
    std::string NormalizeLevelName(std::string levelName) const;
    bool IsKnownSameLevel(const std::string& lhs, const std::string& rhs) const;
    std::string SanitizeUsername(std::string username) const;
    void CopyFixedString(char* dest, size_t destSize, const std::string& value) const;
    std::string GetRemoteUsernameOrFallback() const;
    void ApplyProxyName(IEntity& entity, const std::string& username);
    void ApplyProxyCharacterName(ArkNpc& npc, const std::string& username);
    void ApplySurvivorFactionToProxy(IEntity& entity);
    void ApplyProxyDownedState(IEntity& entity);
    void ApplyRemoteProxyDownedVisual(IEntity& entity, bool downed);
    int FindProxyCharacterSlot(IEntity& entity) const;
    bool CaptureMimicVisualFromEntity(
        EntityId entityId,
        std::string& outModelPath,
        Vec3& outPosition,
        Quat& outRotation,
        Vec3& outScale,
        EntityId& outSourceEntityId,
        uint64_t& outSourceGuid,
        std::string& detail) const;
    void ClearRemoteMimicVisual(const char* reason);
    bool ApplyRemoteMimicVisual(IEntity& proxyEntity, const CoopProtocol::PlayerPosePacket& packet);
    bool AttachRemoteMimicVisualToProxy(
        IEntity& proxyEntity,
        const Vec3& scale,
        const std::string& modelPath,
        std::string& detail);
    bool ApplyProxyWeaponVisual(
        IEntity& entity,
        bool equipped,
        uint32_t weaponClass,
        bool crouched,
        bool lowCrouched,
        bool zeroG,
        const char* reason);
    bool ApplyAdditionalProxyWeaponVisual(
        RemotePeerSession& peer,
        IEntity& entity,
        bool equipped,
        uint32_t weaponClass,
        bool crouched,
        bool lowCrouched,
        bool zeroG,
        const char* reason);
    void ClearProxyWeaponVisual(IEntity* entity, const char* reason);
    bool TriggerProxyParticleEffect(
        const char* effectName,
        bool attachToWeaponSlot,
        const Vec3& offset,
        const Vec3& direction,
        float duration,
        float scale,
        const char* reason,
        bool clearExisting = true);
    bool TriggerWorldParticleEffect(
        const char* effectName,
        const Vec3& position,
        const Vec3& direction,
        float scale,
        const char* reason);
    void QueueLocalEnemyAbilityFxEventForHook(
        EnemyAuthorityState& state,
        IEntity& entity,
        uint16_t abilityKind,
        int fragmentId,
        uint16_t ordinal,
        uint32_t mannequinSequence,
        const Vec3& position,
        const Vec3& direction,
        const char* reason,
        uint64_t targetStableId = 0,
        uint16_t extraFlags = 0,
        uint64_t targetArchetypeId = 0);
    uint64_t BuildTurretStableKey(IEntity& entity) const;
    bool LocalOwnsTurretAuthority(IEntity& entity, bool allowHostFallback = true) const;
    bool QueueLocalTurretAuthorityClaimRequest(IEntity& entity, const char* reason);
    bool QueueLocalTurretAuthorityReleaseRequest(IEntity& entity, const char* reason);
    void HandleClientTurretAuthorityRequest(const CoopProtocol::EnemyAbilityFxEventPacket& packet);
    void ApplyTurretAuthorityFromSnapshot(IEntity& entity, const CoopProtocol::EnemyAbilityFxEventPacket& packet, const char* reason);
    void ApplyTurretNativeAiAuthorityPolicy(IEntity& entity, const char* reason);
    void EnableTurretNativeAi(IEntity& entity, const char* reason);
    IEntity* FindEntityForTurretPointer(ArkTurret* turret) const;
    IEntity* FindNearestLocalTurret(const Vec3& position, float maxDistance) const;
    IEntity* ResolveDebugTurretTarget(const std::string& target, float nearestRadius, std::string& detail) const;
    bool QueueLocalTurretSnapshotEventForEntity(
        IEntity& entity,
        const char* reason,
        uint32_t targetAddress = 0,
        uint16_t targetPort = 0);
    uint32_t QueueLocalTurretSnapshotEventsToEndpoint(uint32_t address, uint16_t port, const char* reason);
    uint16_t BuildTurretStateFlags(IEntity& entity, ArkTurret& turret, const char* reason) const;
    bool ApplyRemoteTurretSnapshotEvent(const CoopProtocol::EnemyAbilityFxEventPacket& packet, const Vec3& position, const Vec3& direction);
    bool ApplyRemoteTurretBrokenStateEvent(const CoopProtocol::EnemyAbilityFxEventPacket& packet, const Vec3& position, const Vec3& direction);
    bool ApplyTurretBrokenStateToEntity(IEntity& entity, bool broken, bool wasForced, bool networkOriginated, const char* reason);
    bool ApplyTurretSnapshotStateToEntity(IEntity& entity, const CoopProtocol::EnemyAbilityFxEventPacket& packet, bool networkOriginated, const char* reason);
    IEntity* FindLocalTurretByStableKeyOrNearest(uint64_t stableKey, uint64_t archetypeId, const Vec3& position, float maxDistance) const;
    void DisableRemoteTurretReplicaNativeAi(IEntity& entity, const char* reason);
    void RestoreRemoteTurretReplicaNativeAi(const char* reason);
    void MarkNetworkOriginatedCystoidNestTriggerZone(const Vec3& position, const char* reason);
    void MarkNetworkOriginatedCystoidEntity(EntityId entityId, const Vec3& origin, const char* reason);
    void MarkNetworkConsumedCystoidNest(uint64_t enemyNetId, EntityId entityId, const Vec3& position, const char* reason);
    bool IsNetworkConsumedCystoidNest(uint64_t enemyNetId, EntityId entityId) const;
    bool ForceNetworkConsumedCystoidNestState(uint64_t enemyNetId, IEntity* entity, const Vec3& position, const Quat& rotation, const char* reason);
    bool IsPositionInNetworkOriginatedCystoidNestZone(const Vec3& position, const char* reason);
    bool ShouldSuppressNetworkOriginatedCystoidExplode(EntityId entityId, const Vec3& position, const char* reason);
    bool ShouldSuppressLocalCystoidExplodeForNetworkNest(const Vec3& position, const char* reason);
    bool ShouldSuppressRemoteCystoidChildState(uint64_t archetypeId, const Vec3& position, const char* reason);
    void DiscardRemoteEnemyBinding(uint64_t enemyNetId, const char* reason);
    void TickNetworkOriginatedCystoidNestTriggerZones(float frameTime);
    void TickDebugCorpsePhantom(float frameTime);
    void TickPendingDebugEnemyAbility(float frameTime);
    void TickPendingRemoteCorpsePhantomResults();
    void TickPendingCorpsePhantomSpawnRequests();
    bool BeginNativeCorpsePhantomSpawn(EntityId sourceEntityId, uint64_t phantomArchetypeId, const char* context);
    bool TriggerRemoteEnemyAbilityVisualEffect(
        EnemyAuthorityState& state,
        IEntity& entity,
        const CoopProtocol::EnemyAbilityFxEventPacket& packet,
        const char* reason);
    bool TriggerRemoteEnemyBurstVisualEffect(
        EnemyAuthorityState& state,
        IEntity& entity,
        uint64_t enemyNetId,
        int fragmentId,
        uint32_t sequence,
        const Vec3& startPosition,
        const Vec3& endPosition,
        const Vec3& fallbackDirection,
        const char* reason);
    bool TriggerManagedWorldParticleEffect(
        const char* effectName,
        const Vec3& position,
        const Vec3& direction,
        float scale,
        float lifeSeconds,
        const char* reason);
    void TickManagedWorldParticleEffects(float frameTime);
    void ClearManagedWorldParticleEffectsNear(const Vec3& position, float distance, const char* reason);
    void ClearManagedWorldParticleEffects(const char* reason);
    void ResetWorldPresentationForLoad(const char* reason, bool allowNativeCleanup);
    void QueueWorldParticleEffect(
        const char* effectName,
        const Vec3& position,
        const Vec3& direction,
        float scale,
        float delaySeconds,
        const char* reason);
    void QueueWorldParticlePulseEffect(
        const char* effectName,
        const Vec3& position,
        const Vec3& direction,
        float scale,
        float startDelaySeconds,
        float repeatSeconds,
        float intervalSeconds,
        const char* reason);
    void TickPendingWorldParticleEffects(float frameTime);
    Vec3 ResolveLocalPosePsiImpactPosition(uint16_t psiFxKind, const Vec3& fallbackPosition, const Vec3& fallbackDirection);
    bool QueueSecondaryLocalPosePsiImpact(uint16_t psiFxKind, int psiPower, const Vec3& impactPosition, const char* reason);
    void PromoteQueuedLocalPosePsiImpact(const char* reason);
    void QueueLocalPosePsiImpact(uint16_t psiFxKind, int psiPower, const Vec3& fallbackPosition, const Vec3& fallbackDirection, const char* reason);
    void QueueLocalPosePsiImpactAt(uint16_t psiFxKind, int psiPower, const Vec3& impactPosition, const char* reason);
    void QueueLocalPosePsiCast(EArkPsiPowers power, float impulseSeconds, const char* reason);
    bool TriggerRemoteProxyPsiCastEffect(uint16_t psiFxKind, int psiPower, const Vec3* worldAimDirection, const char* reason);
    bool TriggerRemoteProxyPsiImpactEffect(uint16_t psiFxKind, int psiPower, const Vec3& worldPosition, const Vec3* worldAimDirection, const char* reason);
    bool TriggerRemoteThermalTrapMarkerPulse(const Vec3& position, const Vec3& direction, const char* reason);
    void AddRemoteThermalTrapMarker(const Vec3& position, const Vec3& direction, const char* reason);
    void ClearRemoteThermalTrapMarkersNear(const Vec3& position);
    void TickRemoteThermalTrapMarkers(float frameTime);
    bool TriggerProxyWeaponMuzzleEffect(uint32_t weaponClass, const Vec3* worldAimDirection, const char* reason);
    void ClearProxyWeaponMuzzleEffects(const char* reason);
    void TickProxyWeaponMuzzleEffects(float frameTime);
    bool DebugProxyWeaponVisualCommand(const std::vector<std::string>& args, std::string& detail);
    bool DebugWorldParticleCommand(const std::vector<std::string>& args, std::string& detail);
    bool DebugPsiFxSniperCommand(const std::vector<std::string>& args, std::string& detail);
    std::string BuildPsiFxSniperTail(size_t maxEntries) const;
    void RegisterProxyReviveInteraction(IEntity& entity);
    void UnregisterProxyReviveInteraction(EntityId entityId = INVALID_ENTITYID);
    bool IsRemoteReviveInteractionAvailable(const IEntity* entity) const;
    bool HandleProxyReviveInteraction(IEntity* entity, EArkInteractionMode mode, EArkInteractionType interaction);
    void ScheduleProxyAttentionClear(const char* reason);
    void TickPendingProxyAttentionClear(float frameTime);
    void ClearProxyAsEnemyTarget(EntityId proxyEntityId);
    void RegisterProxyComplexAttention(IEntity& entity);
    void UnregisterProxyComplexAttention();
    bool IsProxyComplexAttentionRegistered() const;
    const char* GetProxyLifecycleStateName(CoopProxyLifecycleState state) const;
    void SetProxyLifecycleState(CoopProxyLifecycleState state, const char* reason);
    void SuspendProxyForArk(CoopProxyLifecycleState state, const char* reason);
    void ResumeProxyForSameLevel(const char* reason);
    bool IsProxyNativeDestroySafe(const char* reason) const;
    void ResetProxyLifecycleRuntimeState(const char* reason);
    uint32_t BindHostEnemiesToProxyTarget();
    uint32_t CountHostEnemiesTrackingProxySimple() const;
    uint32_t CountHostEnemiesTrackingProxyComplex() const;
    const char* GetDispositionName(int disposition) const;
    bool ApplyRemoteProxyTransform(
        IEntity& entity,
        const Vec3& position,
        const Quat& rotation,
        const char* reason,
        std::string* failureReason = nullptr);
    void ApplyRemotePoseToProxy(const CoopProtocol::PlayerPosePacket& packet, bool presentationReplay = false);
    void ProcessPendingDebugActions();
    void SendRemoteReviveCommand();
    bool SetLocalPlayerHealthSafe(float health, const char* reason);
    bool SetLocalPlayerStanceSafe(int stance, const char* reason);
    void ApplyLocalDownedStance(ArkPlayer& player);
    void ApplyLocalDownedControls(float frameTime);
    void ReleaseLocalDownedControls();
    void EnterLocalDowned(uint32_t reason, bool sendStatus, bool clampHealth = true);
    void ReviveLocalPlayer(float health, bool sendStatus);
    void SetRemotePlayerDowned(bool downed);
    void TickRemoteReviveInteraction(float frameTime);
    void ClearLocalPlayerAsEnemyTarget();
    void ApplyLocalPlayerDownedAttentionState(ArkPlayer& player);
    void ReleaseLocalPlayerDownedAttentionState();
    void RecoverLocalPlayerFromNativeDeathState(const char* reason);
    bool SendLocalPlayerStatus(uint32_t flags, uint32_t reason, const Vec3* overridePosition = nullptr, const Quat* overrideRotation = nullptr);
    bool TeleportLocalPlayer(const Vec3& position, const Quat& rotation);
    void SetLocalPlayerViewRotationAfterTeleport(const Quat& rotation);
    bool TeleportLocalPlayerNearRemote(uint32_t reason);
    bool TeleportRemoteProxyNearLocal(uint32_t reason);
    void TickDownedState(float frameTime);
    void TickLocalPlayerFallRecovery(float frameTime);
    void ResetLocalPlayerFallRecovery(const char* reason);
    bool DebugLocalPlayerFallRecoverySelfTest(std::string& detail);
    void SetupProxyNpc(ArkNpc& npc);
    void SetupMimicPuppet(ArkNpc& npc);
    void RecoverLiveNetworkNpc(ArkNpc& npc);
    void ApplyProxyNoPropCollision(IEntity& entity, const char* reason);
    void SetRemoteEnemyMirrorPhysics(IEntity& entity, bool suppressed, const char* reason);
    void ResetRemoteEnemyMirrorPhysics(const char* reason);
    void RestoreProxyRuntimeHealth();
    void RemoveProxyOnly();
    IEntity* GetAnimationTestProxyEntity() const;
    void SpawnAnimationTestProxy();
    void RemoveAnimationTestProxy();
    bool LoadAnimationTestProxyModel(const std::string& modelPath, int slot, std::string& detail);
    bool PlayAnimationTestProxyAnimation(const std::string& animationName, int slot, int layer, float blend, float speed, std::string& detail);
    bool StopAnimationTestProxyAnimation(int slot, int layer, std::string& detail);
    bool ResetAnimationTestProxyAnimation(int slot, int layer, std::string& detail);
    bool SetAnimationTestProxyTime(int slot, int layer, float normalizedTime, std::string& detail);
    bool ProbeAnimationTestProxy(std::string& detail) const;
    bool ProbeAnimationTestProxyClipNames(int slot, std::string& detail);
    bool ApplyProxyCharacterAnimationState(IEntity& entity, const std::string& state, float duration, std::string& detail);
    bool ApplyAnimationTestProxyCharacterState(const std::string& state, float duration, std::string& detail);
    bool RunAnimationTestProxyNpcNativeAction(const std::string& action, float duration, std::string& detail);
    bool SampleAnimationTestProxyState(float elapsedSeconds, std::string& detail) const;
    void StartAnimationTestProxyTrace(const std::string& label, float seconds);
    void TickAnimationTestProxyTrace(float frameTime);
    bool StartAnimationTestProxyPoseHold(
        const std::string& poseName,
        const std::string& animationName,
        float normalizedTime,
        int slot,
        int layer,
        float blend,
        std::string& detail);
    void ClearAnimationTestProxyPoseHold(const char* reason);
    void TickAnimationTestProxyPoseHold(float frameTime);
    bool StartRemoteProxyPoseHold(
        const std::string& poseName,
        const std::string& animationName,
        float normalizedTime,
        int slot,
        int layer,
        float blend,
        std::string& detail);
    bool StartRemoteProxyAnimationLoop(
        const std::string& stateName,
        const std::string& animationName,
        float durationSeconds,
        int slot,
        int layer,
        float blend,
        std::string& detail);
    bool StartRemoteProxyAnimationOnce(
        const std::string& stateName,
        const std::string& animationName,
        int slot,
        int layer,
        float blend,
        float speed,
        std::string& detail);
    bool ApplyAdditionalRemoteProxyAnimation(
        IEntity& entity,
        const std::string& animationName,
        float normalizedTime,
        bool restart,
        float blend,
        std::string& detail);
    bool ApplyRemotePlayerMotionParams(
        IEntity& entity,
        float travelSpeed,
        float travelAngle,
        float frameTime,
        std::string* reason);
    const char* SelectRemotePlayerZeroGShiftClip(float travelAngle, bool moving) const;
    void ApplyRemotePlayerZeroGPresentation(
        IEntity& entity,
        const Vec3& remoteVelocity,
        const Quat& remoteRotation,
        bool moving);
    void ApplyAdditionalRemotePlayerZeroGMotion(
        RemotePeerSession& peer,
        IEntity& entity,
        const Vec3& remoteVelocity,
        const Quat& remoteRotation,
        bool moving);
    bool UpdateLocalPlayerZeroGMoving(bool zeroG, bool rawMoving);
    void ClearRemoteProxyActionOverlay(const char* reason);
    void ClearRemoteProxyPoseHold(const char* reason);
    void TickRemoteProxyPoseHold(float frameTime);
    void TickPoseActionTimers(float frameTime);
    bool DebugAnimationTestProxyCommand(const std::string& command, const std::vector<std::string>& args, std::string& detail);
    std::vector<EntityId> CaptureRuntimeEntityIdSnapshot(const char* reason) const;
    void RecordCoopSpawnDiagnostics(const char* label, const std::vector<EntityId>& beforeIds, IEntity* rootEntity);
    bool PrepareCoopEntityForRemoval(EntityId entityId, bool proxyEntity, bool clientPuppet, const char* reason);
    bool RemoveCoopEntityGuarded(EntityId entityId, bool forceNow, const char* reason);
    bool ResolveCoopEntityForRemoval(EntityId entityId, IEntity*& outEntity, std::string& outReason) const;
    IEntity* SpawnProxyOnly(const Vec3& position, const Quat& rotation);
    void DrawNpcDebugLine(const char* label, EntityId entityId, uint64_t netId, const char* role);
    void DrawCoopDebug();
    void DrawUiLayerDebug();
    void RefreshUiLayerDebug();
    void TriggerHudFeedbackTest();
    void AddHudTargetTest();
    void RemoveHudTargetTest();
    void DrawPostFxDebug();
    void RefreshPostFxDebug();
    void TickDownedPostFx();
    void ApplyDownedPostFx(bool active);
    void ApplyPostFxParam(const char* paramName, float value, bool force);
    void ApplyPostFxParamVec4(const char* paramName, const Vec4& value, bool force);
    void ResetDownedPostFx();
    bool IsPostFxParamAvailable(const char* paramName) const;
    void DrawDownedHudOverlay();
    void ClearDownedHudOverlay();
    void TickAiTargetDebug(float frameTime);
    void MarkCoopRuntimeEntity(IEntity& entity, bool clientPuppet);
    void AddRemoteProxyEntityIds(std::vector<EntityId>& ids) const;
    void AddRuntimeEnemyEntityIds(std::vector<EntityId>& ids) const;
    bool IsRemoteProxyEntity(EntityId entityId) const;
    bool IsRemoteProxyEntityOrSpawnName(IEntity& entity) const;
    bool IsClientRemoteEnemyPuppet(IEntity& entity) const;
    void RemoveStaleCoopRuntimeEntities();
    bool IsCoopRuntimeEntityName(const char* name) const;
    bool ShouldRejectCoopRuntimeEntitySpawn(const SEntitySpawnParams& params);
    void OnCoopRuntimeEntitySpawned(IEntity& entity, SEntitySpawnParams& params, const char* stage);
    void OnCoopRuntimeEntityRemoved(IEntity& entity);
    void OnCoopRuntimeEntityEvent(IEntity& entity, SEntityEvent& event);
    void RegisterCoopEntitySystemSink();
    void UnregisterCoopEntitySystemSink();
    void PurgeCoopRuntimeEntitiesForSave(const char* reason);
    void PrepareCoopRuntimeEntitiesForLevelTransition(const char* reason);
    void BeginNativeSideBlobCapture(const char* reason);
    void CaptureNativePlayerSideBlobSnapshot(ArkPlayer* player, const char* functionName, bool reading, int target, bool ok);
    void CaptureNativeInventorySideBlobSnapshot(ArkInventory* inventory, const char* functionName, bool reading, int target, bool ok);
    void CaptureNativeItemSideBlobSnapshot(CArkItem* item, const char* functionName, bool reading, int target, bool ok);
    void FinalizeNativeSideBlobCaptureForPlayerState(const char* reason);
    bool CaptureNativePlayerSnapshotWithoutSave(const char* reason);
    bool AttachNativeSnapshotSaveToCapture(NativeSideBlobCaptureState& capture, const std::string& savePathOrName, const char* reason);
    bool WriteCapturedNativeSideBlob(ISaveGame* saveGame, std::string& summary);
    bool ReadCapturedNativeSideBlob(ILoadGame* loadGame, std::string& summary);
    bool WriteNativeSaveLoadXmlDiagnostics(const char* phase, const char* reason, const XmlNodeRef& root, bool fullXml);
    bool IsSpawnedRuntimeEnemyName(const char* name) const;
    bool IsEnemyReplicationCandidate(IEntity& entity) const;
    bool IsEnemyRuntimeControlCandidate(IEntity& entity) const;
    bool LocalPlayerHasEnemyAwarenessForCoop(const IEntity& enemy) const;
    uint8_t LocalPlayerEnemyAttentionLevelForCoop(const IEntity& enemy) const;
    EntityId ResolveLocallyRepresentedEnemyTarget(uint64_t accountToken) const;
    void SyncRemoteEnemyPresentationTarget(
        EnemyAuthorityState& state,
        IEntity& enemy,
        uint64_t targetAccountToken,
        uint32_t mannequinSequence,
        const char* reason);
    void ResetEnemySemanticReplicationState(EnemyAuthorityState& state);
    CoopEnemyControlPolicy::Context BuildLocalEnemyControlPolicyContext(
        const EnemyAuthorityState& state,
        const IEntity& entity) const;
    bool InterruptEnemyAbilityForAuthorityTransition(
        EnemyAuthorityState& state,
        IEntity& entity,
        const char* reason);
    void RestoreLocalEnemyVanillaAuthority(
        EnemyAuthorityState& state,
        IEntity& entity,
        const char* reason);
    void ClearRemoteEnemyPresentationForLocalAuthority(
        EnemyAuthorityState& state,
        IEntity& entity,
        const char* reason);
    bool ShouldAllowLocalVanillaEnemyControl(const EnemyAuthorityState& state, const IEntity& entity) const;
    bool ShouldBlockLocalVanillaEnemyControl(const EnemyAuthorityState& state, const IEntity& entity) const;
    bool ShouldBlockLocalVanillaEnemyControlIntent(
        const EnemyAuthorityState& state,
        const IEntity& entity,
        LocalEnemyVanillaControlIntent intent) const;
    bool TryGetDebugEnemyAttentionOverride(const IEntity& enemy, bool& outHasAttention) const;
    bool TryGetDebugEnemyAttentionLevelOverride(const IEntity& enemy, uint8_t& outAttentionLevel) const;
    bool IsEnemyPuppetEntity(EntityId entityId) const;
    uint64_t FindPuppetNetIdByEntityId(EntityId entityId) const;
    EnemyAuthorityState* FindEnemyAuthorityByNetId(uint64_t netId);
    EnemyAuthorityState& EnsureEnemyAuthorityState(IEntity& entity);
    uint64_t ResolveEnemyStableId(IEntity& entity, uint64_t archetypeId, uint64_t nativeGuid) const;
    uint32_t BuildEnemyRosterFlags(IEntity& entity) const;
    bool EnsureEnemyRosterAnnounced(EnemyAuthorityState& state, const char* failurePrefix);
    bool ValidateEnemyPoseRoster(const CoopProtocol::TestMimicStatePacket& packet, EnemyRosterRecord*& outRecord);
    bool RegisterLocalEthericDoppelgangerCandidate(IEntity& entity, ArkNpc& npc, const char* reason);
    bool ApplyEthericDoppelgangerRelation(EnemyAuthorityState& childState, IEntity& childEntity, const char* reason);
    void HandleEthericDoppelgangerRequest(const CoopProtocol::CorpsePhantomRequestPacket& packet);
    void ProcessPendingEnemyRegistryCandidates();
    void ScanLocalEnemyAuthorityRegistry(const char* statusPrefix);
    void NoteLocalEnemyAuthorityHit(
        EntityId entityId,
        const HitInfo& hitInfo,
        const HitInfo& signalHitInfo,
        uint64_t damagePackageId,
        float damagePackageScale,
        float targetHealthBeforeHit);
    IEntity* TryBindClientLocalEnemyForLocomotion(
        uint64_t enemyNetId,
        uint64_t archetypeId,
        const Vec3& position,
        const Quat& rotation,
        uint64_t sourceGuid = 0,
        bool allowSpawn = true);
    IEntity* SpawnDebugEnemyAt(uint64_t archetypeId, const Vec3& position, const Quat& rotation, const char* name);
    void SyncRemoteEnemyMovementDesire(
        IEntity& entity,
        EnemyAuthorityState& state,
        const Vec3& targetPosition,
        const Vec3& moveDirection,
        float speed,
        uint32_t locomotionFlags,
        bool authorityMovementIntent);
    void ClearRemoteEnemyMovementDesire(uint64_t enemyNetId, const char* reason);
    void ClearAllRemoteEnemyMovementDesires(const char* reason);
    float ConsumeRemoteEnemyTransformTickSeconds(
        EnemyAuthorityState& state,
        float fallbackTickSeconds,
        bool rewritePosition);
    uint32_t UpdateRemoteEnemyVisualMotionFromStep(
        EnemyAuthorityState& state,
        float visibleStep,
        float visibleSpeed,
        uint32_t sourceFlags,
        float elapsedSeconds,
        bool decayWhenIdle);
    bool ComputeRemoteEnemyTransformSmoothing(
        EnemyAuthorityState& state,
        const Vec3& currentPosition,
        const Quat& currentRotation,
        bool rewritePosition,
        bool rewriteRotation,
        float fallbackTickSeconds,
        Vec3& outPosition,
        Quat& outRotation,
        RemoteEnemyTransformSmoothingResult& outResult);
    bool TryBuildReadOnlyLocalFacingMixTarget(
        const EnemyAuthorityState& state,
        const IEntity& entity,
        float nowSeconds,
        Quat& outRotation) const;
    void ApplyEnemyLocomotionStateToLocal(const CoopProtocol::TestMimicStatePacket& packet);
    bool ApplyRemoteEnemyMannequinAnimation(
        IEntity& entity,
        EnemyAuthorityState& state,
        const CoopProtocol::TestMimicStatePacket& packet,
        bool localHasAttention,
        bool remoteAuthorityHasAttention,
        const char* reason,
        bool reliableActionEdge = false);
    void RecordRemoteObserverLocalIntentSample(
        EnemyAuthorityState& state,
        IEntity& entity,
        uint32_t intentKinds,
        uint32_t intentFlags,
        uint64_t contextId,
        EntityId targetEntityId,
        const char* stage,
        bool blocked);
    bool QueueLocalEnemyMannequinActionEventForHook(
        EnemyAuthorityState& state,
        const EnemyAuthorityState::LocalNativeMannequinAction& action,
        CoopProtocol::EnemyMannequinActionCommand command,
        const char* reason);
    void TickClientEnemyAuthorityClaims(float frameTime);
    void TickLocalFocusedOperatorCombat(float frameTime);
    void ResetLocalFocusedOperatorCombat(const char* reason);
    void TickRemoteEnemySmoothing(float frameTime);
    bool IsLocalPlayerAuthorityBlockedByModalState() const;
    bool SendClientEnemyAuthorityStateNow(EnemyAuthorityState& state, uint32_t sourceFlags, const char* failurePrefix);
    void HandleRemoteEnemyAuthorityStateOnAreaAuthority(const CoopProtocol::TestMimicStatePacket& packet);
    bool UpdateEnemyAttentionCandidate(
        EnemyAuthorityState& state,
        uint64_t accountToken,
        uint8_t attentionLevel,
        bool blocked,
        uint64_t targetAccountToken,
        uint32_t sequence,
        bool enforceSequence);
    CoopEnemyAuthorityPolicy::Decision SelectEnemyAttentionAuthority(
        const EnemyAuthorityState& state,
        uint64_t areaAuthorityAccountToken) const;
    bool ApplyEnemyAttentionAuthorityDecisionOnAreaAuthority(
        EnemyAuthorityState& state,
        const CoopEnemyAuthorityPolicy::Decision& decision,
        const CoopProtocol::TestMimicStatePacket* sourcePacket,
        uint64_t sourceAccountToken,
        const char* reason);
    bool ShouldMarkEnemyAuthorityHasAttention(const EnemyAuthorityState& state, bool localAuthorityBlocked) const;
    IEntity* TryAdoptClientLocalEnemyAsPuppet(uint64_t enemyNetId, uint64_t archetypeId, const Vec3& position);
    void ScanHostEnemyRegistry();
    void CullClientLocalEnemies();
    IEntity* SpawnMimicAt(const Vec3& position, const Quat& rotation, const char* name, bool puppet);
    void ApplyMimicStateToPuppet(const CoopProtocol::TestMimicStatePacket& packet);
    void ApplyEnemyStateToPuppet(const CoopProtocol::TestMimicStatePacket& packet);
    bool ApplyEnemyDamageRequestToHost(const CoopProtocol::EnemyDamageRequestPacket& packet);
    bool QueueLocalEnemyDeathPresentation(
        EnemyAuthorityState& state,
        IEntity& entity,
        const HitInfo& hitInfo,
        const char* reason,
        uint64_t damagePackageId,
        float damagePackageScale,
        float targetHealthBeforeHit,
        uint64_t damageSourceAccountToken = 0,
        uint64_t sourceTurretStableKey = 0,
        uint32_t sourceTurretAuthorityEpoch = 0);
    bool TryApplyVanillaEnemyDeathHit(
        ArkNpc& npc,
        IEntity& entity,
        const CoopProtocol::EnemyDeathPresentationPacket& presentation);
    bool SetEntityHealthFromAuthority(EntityId entityId, float health, bool takingDamage, bool killWhenZero);
    void ApplyEntityPhysicsVelocity(IEntity& entity, const Vec3& velocity) const;
    void PruneDamageDedupe(float now);
    uint64_t BuildProxyDamageDedupeKey(const HitInfo& hitInfo, float damage) const;
    uint64_t BuildRemoteDamageDedupeKey(const CoopProtocol::RemotePlayerDamagePacket& packet) const;
    uint64_t ResolveDamageSourceStableId(const HitInfo& hitInfo, bool* outStable) const;
    uint32_t BuildDamagePositionHash(const Vec3& position) const;
    uint32_t BuildDamageTimeBucket() const;
    uint64_t BuildDamageSourceKeyHash(
        uint64_t sourceStableId,
        uint32_t sourceGeneration,
        uint32_t attackSeq,
        uint32_t projectileOrdinal,
        uint32_t damageType,
        uint32_t timeBucket,
        uint32_t positionHash) const;
    void RememberDamageDedupeEvent(uint64_t key, float damage, bool localPlayerRaw, const char* source, float ttlSeconds);
    bool ShouldDropDuplicateRemotePlayerDamage(const CoopProtocol::RemotePlayerDamagePacket& packet);
    void TickProxyFollow(float frameTime);
    void TickProxyDamageSync();
    void TickHostProxyCombatStimulus(float frameTime);
    void TickLocalEnemyAreaAuthoritySync(float frameTime);
    void TickEnemyMimicryStateHeartbeat(float frameTime);
    bool SendAuthoritativeMimicStateNow(const char* failurePrefix);
    bool SendEnemyStateNow(EnemyAuthorityState& state, const char* failurePrefix);
    bool ReadProxyHealth(float& health, float& maxHealth) const;
    bool ReadEntityHealth(EntityId entityId, float& health, float& maxHealth) const;
    void ResetProxyHealthBaseline();
    void DrawRemoteNameplate();
    CoopEvents::Route RouteCoopEvent(const CoopEvents::Context& context);
    bool IsReliableTransportReady(const CoopEvents::Route& route) const;
    void RegisterSystemEventListener();
    void UnregisterSystemEventListener();
    void ApplyAutoSkipAfterLoadingScreenFromEnvironment();
    bool TryApplyAutoSkipAfterLoadingScreen();
    void QueueAutoReengageAfterLoadingScreen(const char* reason, bool countAttempt);
    bool TryDrainAutoReengageAfterLoadingScreen(const char* reason);
    bool TryAutoReengageAfterLoadingScreen();
    void ApplyAutoStartFromEnvironment();
    void ApplyAutoTestFromEnvironment();
    void TickAutoLoadFromEnvironment(float frameTime);
    void TickAutoTest(float frameTime);
    void RegisterRuntimeControlCommands();
    bool HandleRuntimeControlCommand(const std::string& command, const std::vector<std::string>& args, std::string& statusLine);
    std::string BuildRuntimeControlStatus() const;
    bool ShouldTraceNativeInventoryDetails() const;
    bool ShouldLogNativeInventoryTraceDetails() const;
    void SetNativeInventoryTrace(bool enabled);
    ArkLevelTransitionDoor* QueryLevelTransitionDoor(IEntity& entity) const;
    std::string GetLocationDebugName(uint64_t locationId) const;
    void RefreshLevelTransitionDebug();
    void TickLevelTransitionPresentationRecovery(float frameTime);
    bool SelectLevelTransitionDoorByDestination(const std::string& destination);
    bool InvokeSelectedLevelTransitionDoor();
    void DrawLevelTransitionDebug();
    void BeginCoopLoadGuard(const char* reason);
    void MarkCoopLoadNativeComplete(const char* reason);
    void TickCoopLoadGuard(float frameTime, bool allowNativeStateMutation = true);
    bool IsPostLoadGameplayInteractive(std::string& reason) const;
    void EndCoopLoadGuard(const char* reason);
    void ActivateClientCoopSaveSlot(const std::string& savePath, uint32_t transferId, const char* reason);
    bool SeedClientCoopLevelStateFromSaveSlot(const char* reason);
    int GetArkSaveLoadCampaignSlotSafe(ArkSaveLoadSystem* saveLoadSystem, const char* reason);
    bool PrepareClientCoopLevelStateBridge(ArkSaveLoadSystem* saveLoadSystem, const char* reason);
    bool SyncClientCoopLevelStateBridgeFromArkTemp(const char* reason);
    void CleanupClientCoopLevelStateBridge(const char* reason);
    void ResetRuntimeWorldRefsForLoad(const char* reason);
    void SetTransitionPhase(const char* phase, const char* reason);
    void RefreshCoopRuntimeEntitySaveFlags(const char* reason);
    std::string BuildRuntimeEntityFlagSnapshot(const char* reason) const;
    std::string BuildRuntimePerceptionSnapshot(const char* reason) const;
    void PrepareNativePerceptionManagersForLevelTransition(const char* reason);
    void UnregisterCoopRuntimeEntitiesFromNativePerception(const char* reason);
    void RemoveRuntimeEntitiesBeforeNativeTransition(const char* reason);
    bool DebugTraceNativeEntityLifecycle(const std::vector<std::string>& args, std::string& detail);
    bool DebugTraceNativeNpcSpawn(const std::vector<std::string>& args, std::string& detail);
    std::string GetPlayerSidecarPath() const;
    std::string GetPlayerSidecarRecoveryJournalPath() const;
    bool SaveLocalPlayerSidecar(const char* reason);
    bool LoadLocalPlayerSidecar(PlayerSidecarState& state);
    bool LoadPlayerSidecarFromPath(const std::string& path, PlayerSidecarState& state);
    bool HasValidPlayerSidecarRecoveryJournal() const;
    uint64_t GetPlayerSidecarRecoveryJournalRevision() const;
    bool WriteLocalPlayerRecoveryJournal(const char* reason);
    bool ClearLocalPlayerRecoveryJournal(
        const std::string& sourcePath,
        uint64_t accountToken,
        const std::string& saveKey,
        uint64_t revision,
        const char* reason);
    bool RecoverLocalPlayerRecoveryJournal(const char* reason);
    bool ApplyLocalPlayerSidecar(const PlayerSidecarState& state, const char* reason);
    bool RestorePlayerSidecarEquipment(const PlayerSidecarState& state, const char* reason);
    void QueuePlayerSidecarInventoryRestore(const std::vector<PlayerInventoryItemState>& items, bool clearInventory, const char* reason);
    bool TryRestorePendingPlayerSidecarInventory(const char* reason, bool nativeRestoreWindow = false);
    void QueuePlayerSidecarChipsetRestore(const std::vector<PlayerChipsetState>& chipsets, const char* reason);
    bool TryRestorePendingPlayerSidecarChipsets(const char* reason);
    bool DebugRunConsoleCommand(const std::string& commandLine, bool deferExecution, std::string& detail);
    bool DebugGiveLocalInventoryArchetype(uint64_t archetypeId, int count, std::string& detail);
    bool DebugGiveLocalInventoryCommand(const std::string& archetypeName, int count, std::string& detail);
    bool DebugClearLocalInventory(std::string& detail);
    bool DebugCountLocalInventoryArchetype(uint64_t archetypeId, int& outCount, std::string& detail);
    bool DebugProbeLocalInventoryArchetype(uint64_t archetypeId, std::string& detail);
    bool DebugSharedDropCommand(const std::string& command, const std::vector<std::string>& args, std::string& detail);
    bool DebugQueueInventoryRestoreFixture(uint64_t archetypeId, int count, bool clearInventory, std::string& detail);
    bool DebugExportLocalInventoryJsonl(std::string& detail);
    bool DebugExportLocalPlayerJsonl(std::string& detail);
    bool DebugExportLocalEquipmentJsonl(std::string& detail);
    bool DebugExportAreaJournalJsonl(std::string& detail);
    bool DebugExportEnemyRegistryJsonl(std::string& detail);
    bool DebugExportTurretRegistryJsonl(std::string& detail);
    bool DebugLoadRuntimeLevel(const std::vector<std::string>& args, std::string& detail);
    bool DebugInvokeMissionLevelTransition(const std::vector<std::string>& args, std::string& detail);
    bool DebugHandleRuntimeWorkstationCommand(const std::string& command, const std::vector<std::string>& args, std::string& action);
    IEntity* ResolveRuntimeEntityTarget(const std::string& target, std::string& detail) const;
    bool DebugMoveRuntimeEntity(const std::vector<std::string>& args, std::string& detail);
    bool DebugMoveRuntimeEntityInFront(const std::vector<std::string>& args, std::string& detail);
    bool DebugSetRuntimeEntityTransform(const std::vector<std::string>& args, std::string& detail);
    bool DebugMoveNearestRuntimePropForTest(const std::vector<std::string>& args, std::string& detail);
    bool DebugProbeRuntimeEntity(const std::vector<std::string>& args, std::string& detail);
    bool DebugRepairHugePhysicsBounds(const std::vector<std::string>& args, std::string& detail);
    bool DebugCryPakAssetCommand(const std::string& command, const std::vector<std::string>& args, std::string& detail);
    bool TryRepairHugePhysicsBoundsEntity(IEntity& entity, const char* context, std::string& detail, bool& needed, bool& repaired);
    bool TryQuarantineHugePhysicsBoundsEntity(IEntity& entity, const char* context, std::string& detail);
    bool ScanAndRepairHugePhysicsBounds(uint32_t maxRepairs, uint32_t maxScans, const char* context, std::string& detail, uint32_t& needed, uint32_t& repaired, uint32_t& failed);
    bool QueueHugePhysicsBoundsRepairFromBounds(unsigned requestedCells, const Vec3& queryMin, const Vec3& queryMax, const char* reason);
    bool QueueHugePhysicsBoundsRepairFromSpatialWarning(const char* logLine);
    bool RepairHugePhysicsBoundsForSpatialWarning(unsigned requestedCells, const Vec3& queryMin, const Vec3& queryMax, std::string& detail);
    void TickPendingHugePhysicsBoundsRepair(float frameTime);
    void TickHugePhysicsBoundsSanitizer(float frameTime);
    bool DebugHideRuntimeEntity(const std::vector<std::string>& args, std::string& detail);
    bool DebugRemoveRuntimeEntity(const std::vector<std::string>& args, std::string& detail);
    void QueuePlayerSidecarSave(const char* reason);
    bool TryApplyPendingPlayerSidecar(const char* reason);
    void QueuePlayerSidecarApply(const char* reason);
    void TickPlayerSidecar(float frameTime);
    const char* GetSystemEventName(int event) const;
    void InstallCrashExceptionHandler();
    void RemoveCrashExceptionHandler();
    void RegisterAnimationQueueLogSniper();
    void UnregisterAnimationQueueLogSniper();
    void RegisterPhysicalWorldBoxQueryHook();
    void UnregisterPhysicalWorldBoxQueryHook();
    void AppendSpatialEntityTraceDetails(std::ostringstream& out, const char* logLine) const;
    void RegisterCoopRenderListener();
    void UnregisterCoopRenderListener();
    void OnCoopRenderEndFrame();

    EntityId m_proxyEntityId = INVALID_ENTITYID;
    EntityId m_mimicEntityId = INVALID_ENTITYID;
    EntityId m_animationTestProxyEntityId = INVALID_ENTITYID;
    EntityId m_nativeEntityLifecycleTraceTarget = INVALID_ENTITYID;
    float m_proxyTickAccumulator = 0.0f;
    float m_mimicStateTickAccumulator = 0.0f;
    float m_enemyMimicryHeartbeatAccumulator = 0.0f;
    float m_networkTickAccumulator = 0.0f;
    float m_sessionTickAccumulator = 0.0f;
    float m_lastPacketTime = -1.0f;
    float m_clientConnectStartTime = -1.0f;
    uint32_t m_protocolMismatchPackets = 0;
    uint16_t m_lastProtocolMismatchVersion = 0;
    std::uintptr_t m_socket = kInvalidNetworkSocket;
    std::uintptr_t m_serverBrowserSocket = kInvalidNetworkSocket;
    CoopNetworkMode m_networkMode = CoopNetworkMode::Off;
    CoopProxyLifecycleState m_proxyLifecycleState = CoopProxyLifecycleState::Empty;
    CoopIdentityConfig m_identityConfig;
    std::string m_detectedPlatformAccountId;
    std::string m_localUsername = "Player2";
    std::string m_hostAddress = "127.0.0.1";
    std::string m_joinPassword;
    std::string m_serverPassword;
    std::string m_serverName = "Prey Multiplayer";
    std::string m_serverSearch;
    std::string m_serverAllowlist;
    std::string m_networkStatus = "offline";
    std::string m_sessionStatus = "not connected";
    std::string m_lastRouteKind = "-";
    std::string m_lastRouteAction = "-";
    std::string m_lastRouteReason = "-";
    std::string m_localLevelName = "unknown";
    std::string m_remoteLevelName;
    std::string m_lastRuntimeCleanupLevelName;
    std::string m_lastRuntimeCleanupRemoteUsername;
    std::string m_lastRemoteUsername;
    uint64_t m_remoteAccountToken = 0;
    uint64_t m_remotePlayerModelArchetypeId = 10739735956144685671ull;
    uint64_t m_lastRuntimeCleanupRemoteAccountToken = 0;
    bool m_duplicateAccountRejected = false;
    std::string m_queuedHudFeedbackMessage;
    std::string m_peerConnectionThrottleReason = "-";
    std::string m_peerTimeoutWarningReason = "-";
    Vec3 m_lastLocalPlayerPos = Vec3(ZERO);
    uint32_t m_remoteAddress = 0;
    uint16_t m_remotePort = 0;
    int m_networkPort = 27015;
    uint64_t m_serverBrowserNonce = 0;
    float m_serverBrowserQueryTime = -1.0f;
    bool m_serverLanVisible = true;
    int m_serverAccessMode = 0;
    uint32_t m_sendSequence = 0;
    uint32_t m_controlSequence = 0;
    uint32_t m_sentPosePackets = 0;
    uint32_t m_receivedPosePackets = 0;
    CoopProtocol::PlayerPosePacket m_lastTransmittedPosePacket = {};
    CoopProtocol::PlayerPosePacket m_lastReceivedPosePacket = {};
    CoopProtocol::PlayerPosePacket m_lastPrimaryRemotePosePacket = {};
    uint32_t m_localPoseFlagsObserved = 0;
    uint32_t m_remotePoseFlagsObserved = 0;
    uint32_t m_localPoseVerticalSamples = 0;
    uint32_t m_remotePoseVerticalSamples = 0;
    uint32_t m_localPoseUpwardSamples = 0;
    uint32_t m_localPoseDownwardSamples = 0;
    uint32_t m_remotePoseUpwardSamples = 0;
    uint32_t m_remotePoseDownwardSamples = 0;
    float m_localPoseMaxVerticalSpeed = 0.0f;
    float m_remotePoseMaxVerticalSpeed = 0.0f;
    float m_localPoseMaxUpwardSpeed = 0.0f;
    float m_localPoseMinDownwardSpeed = 0.0f;
    float m_remotePoseMaxUpwardSpeed = 0.0f;
    float m_remotePoseMinDownwardSpeed = 0.0f;
    float m_localPoseSnapshotSilenceSeconds = 0.0f;
    bool m_hasLastTransmittedPosePacket = false;
    bool m_hasLastPrimaryRemotePosePacket = false;
    bool m_remotePosePresentationReplayPending = false;
    uint8_t m_remotePosePresentationReplayFrames = 0;
    uint32_t m_sentSessionPackets = 0;
    uint32_t m_receivedSessionPackets = 0;
    uint32_t m_areaLeaseSequence = 0;
    uint32_t m_areaLeaseEpochCounter = 0;
    uint32_t m_areaLeaseEpoch = 0;
    uint32_t m_areaLeaseLevelEpoch = 0;
    uint32_t m_areaLeaseSentPackets = 0;
    uint32_t m_areaLeaseReceivedPackets = 0;
    uint32_t m_areaLeaseAppliedPackets = 0;
    uint32_t m_areaLeaseDroppedPackets = 0;
    uint64_t m_areaLeaseAreaId = 0;
    uint64_t m_areaLeaseAuthorityPeerToken = 0;
    bool m_areaLeaseActive = false;
    bool m_areaLeaseDebugHoldReleased = false;
    bool m_areaLeaseLocalReady = false;
    bool m_areaLeasePeerReady = false;
    bool m_areaLeaseSnapshotRequested = false;
    bool m_areaLeaseAwaitingHandoffSnapshot = false;
    float m_areaLeaseHandoffWaitSeconds = 0.0f;
    uint64_t m_areaLeaseExpectedReadySnapshotSequence = 0;
    std::string m_areaLeaseLevelName;
    std::string m_lastAreaLeaseEvent = "-";
    uint32_t m_sentDamagePackets = 0;
    uint32_t m_receivedDamagePackets = 0;
    uint32_t m_damageSequence = 0;
    uint32_t m_lastAppliedDamageSequence = 0;
    uint32_t m_playerStatusSequence = 0;
    uint32_t m_sentPlayerStatusPackets = 0;
    uint32_t m_receivedPlayerStatusPackets = 0;
    uint32_t m_lastPlayerStatusSequence = 0;
    uint32_t m_testSpawnSequence = 0;
    uint32_t m_sentTestSpawnPackets = 0;
    uint32_t m_receivedTestSpawnPackets = 0;
    uint32_t m_lastTestSpawnSequence = 0;
    uint32_t m_mimicStateSequence = 0;
    uint32_t m_sentMimicStatePackets = 0;
    uint32_t m_receivedMimicStatePackets = 0;
    uint32_t m_lastMimicStateSequence = 0;
    uint32_t m_enemyDamageSequence = 0;
    uint32_t m_sentEnemyDamagePackets = 0;
    uint32_t m_receivedEnemyDamagePackets = 0;
    uint32_t m_rejectedNonPlayerEnemyDamageRequests = 0;
    uint32_t m_lastEnemyDamageSequence = 0;
    std::string m_lastEnemyDamageEvent = "-";
    uint32_t m_enemyDeathPresentationSequence = 0;
    uint32_t m_sentEnemyDeathPresentationPackets = 0;
    uint32_t m_receivedEnemyDeathPresentationPackets = 0;
    uint32_t m_appliedEnemyDeathPresentationPackets = 0;
    uint32_t m_droppedEnemyDeathPresentationPackets = 0;
    std::string m_lastEnemyDeathPresentationEvent = "-";
    uint32_t m_gooResultSequence = 0;
    uint32_t m_sentGooResultPackets = 0;
    uint32_t m_receivedGooResultPackets = 0;
    uint32_t m_lastGooResultSequence = 0;
    uint32_t m_appliedGooDestroyPackets = 0;
    uint32_t m_appliedGooDynamicAttachPackets = 0;
    uint32_t m_enemyProjectileSequence = 0;
    uint32_t m_sentEnemyProjectilePackets = 0;
    uint32_t m_receivedEnemyProjectilePackets = 0;
    uint32_t m_appliedEnemyProjectilePackets = 0;
    uint32_t m_droppedEnemyProjectilePackets = 0;
    uint32_t m_enemyAbilityFxSequence = 0;
    uint32_t m_sentEnemyAbilityFxPackets = 0;
    uint32_t m_receivedEnemyAbilityFxPackets = 0;
    uint32_t m_appliedEnemyAbilityFxPackets = 0;
    uint32_t m_droppedEnemyAbilityFxPackets = 0;
    uint32_t m_enemyMannequinActionEventSequence = 0;
    uint32_t m_sentEnemyMannequinActionPackets = 0;
    uint32_t m_receivedEnemyMannequinActionPackets = 0;
    uint32_t m_appliedEnemyMannequinActionPackets = 0;
    uint32_t m_droppedEnemyMannequinActionPackets = 0;
    std::unordered_map<uint64_t, uint32_t> m_enemyMannequinActionLastSequences;
    std::string m_lastEnemyMannequinActionEvent = "-";
    uint32_t m_suppressedClientTurretProjectiles = 0;
    uint32_t m_enemyBurstFxTriggers = 0;
    uint32_t m_enemyBurstFxSkips = 0;
    uint32_t m_enemyBurstFxFailures = 0;
    uint32_t m_operatorLaserUpdates = 0;
    uint32_t m_operatorLaserActiveUpdates = 0;
    uint32_t m_operatorLaserTargetOverrides = 0;
    uint32_t m_operatorLaserTargetSkips = 0;
    uint32_t m_operatorLaserRemoteDamageSuppressions = 0;
    uint32_t m_operatorLaserHelperRegistrations = 0;
    uint32_t m_operatorLaserLocalStarts = 0;
    uint32_t m_operatorLaserLocalTicks = 0;
    uint32_t m_operatorLaserLocalStops = 0;
    uint32_t m_operatorLaserNativeStarts = 0;
    uint32_t m_operatorLaserNativeStops = 0;
    uint32_t m_operatorLaserDeferredStops = 0;
    uint32_t m_operatorLaserDamageCalls = 0;
    uint32_t m_corpsePhantomUpdates = 0;
    uint32_t m_corpsePhantomResults = 0;
    uint32_t m_corpsePhantomSourceRemovals = 0;
    uint32_t m_corpsePhantomRaiseApplies = 0;
    uint32_t m_corpsePhantomRequestSequence = 0;
    uint32_t m_lastCorpsePhantomRequestSequence = 0;
    uint32_t m_corpsePhantomRequestsSent = 0;
    uint32_t m_corpsePhantomRequestsReceived = 0;
    uint32_t m_corpsePhantomRequestsApplied = 0;
    uint32_t m_corpsePhantomRequestsDropped = 0;
    uint32_t m_ethericDoppelgangerRequestsSent = 0;
    uint32_t m_ethericDoppelgangerRequestsReceived = 0;
    uint32_t m_ethericDoppelgangerRequestsApplied = 0;
    uint32_t m_ethericDoppelgangerRequestsDropped = 0;
    uint32_t m_ethericDoppelgangerRelationsApplied = 0;
    std::string m_lastEthericDoppelgangerEvent = "-";
    uint32_t m_storyEventSequence = 0;
    uint32_t m_storyRevision = 0;
    uint32_t m_sentStoryEventPackets = 0;
    uint32_t m_receivedStoryEventPackets = 0;
    uint32_t m_appliedStoryEventPackets = 0;
    uint32_t m_droppedStoryEventPackets = 0;
    uint32_t m_duplicateStoryEventPackets = 0;
    uint32_t m_reentrantStoryEventSkips = 0;
    uint32_t m_storyEventFailures = 0;
    uint64_t m_lastStoryEventId = 0;
    std::string m_lastStoryEvent = "-";
    std::unordered_set<uint64_t> m_appliedStoryEventIds;
    std::unordered_map<uint64_t, uint32_t> m_storyRemoteEventCounts;
    bool m_applyingRemoteStoryEvent = false;
    bool m_applyingRemoteCorpsePhantomResult = false;
    bool m_debugCorpsePhantomPending = false;
    float m_debugCorpsePhantomSeconds = 0.0f;
    uint32_t m_debugCorpsePhantomResultBaseline = 0;
    uint64_t m_pendingDebugEnemyAbilityNetId = 0;
    uint64_t m_pendingDebugEnemyAbilityContextId = 0;
    float m_pendingDebugEnemyAbilitySeconds = 0.0f;
    float m_pendingDebugEnemyAbilityRetrySeconds = 0.0f;
    uint32_t m_pendingDebugEnemyAbilityAttempts = 0;
    uint32_t m_areaObjectEventSequence = 0;
    uint32_t m_areaObjectRevision = 0;
    uint32_t m_sentAreaObjectEventPackets = 0;
    uint32_t m_receivedAreaObjectEventPackets = 0;
    uint32_t m_appliedAreaObjectEventPackets = 0;
    uint32_t m_droppedAreaObjectEventPackets = 0;
    uint32_t m_duplicateAreaObjectEventPackets = 0;
    uint32_t m_breakableHealthHookCalls = 0;
    uint32_t m_breakableHealthEventsQueued = 0;
    uint32_t m_breakableHealthEventsApplied = 0;
    uint32_t m_breakableHealthEventSkips = 0;
    std::string m_lastBreakableHealthEvent = "-";
    uint32_t m_breakableGlassHookCalls = 0;
    uint32_t m_breakableGlassEventsQueued = 0;
    uint32_t m_breakableGlassEventsApplied = 0;
    uint32_t m_breakableGlassEventSkips = 0;
    std::string m_lastBreakableGlassEvent = "-";
    uint32_t m_reentrantAreaObjectEventSkips = 0;
    uint32_t m_areaObjectEventFailures = 0;
    uint32_t m_areaObjectJournalReplayRows = 0;
    uint32_t m_areaObjectJournalReplayApplied = 0;
    uint32_t m_areaObjectJournalReplayRejected = 0;
    uint64_t m_lastAreaObjectEventId = 0;
    std::string m_lastAreaObjectEvent = "-";
    std::unordered_set<uint64_t> m_appliedAreaObjectEventIds;
    struct RemoteDoorPhysicsPartState
    {
        bool valid = false;
        uint32_t flags = 0;
        uint32_t colliderFlags = 0;
    };
    std::unordered_map<EntityId, std::vector<RemoteDoorPhysicsPartState>> m_remoteOpenDoorPhysicsParts;
    struct RemoteEnemyPhysicsSnapshot
    {
        IPhysicalEntity* physics = nullptr;
        int collTypes = 0;
        Vec3 gravity = Vec3(ZERO);
        int zeroG = 0;
        int active = 0;
        int releaseGroundColliderWhenNotActive = 0;
        Vec3 unprojectionDirection = Vec3(ZERO);
        float maxUnprojection = 0.0f;
        uint32_t flags = 0;
        uint32_t arkFlags = 0;
        bool dynamicsValid = false;
        bool dimensionsValid = false;
        bool flagsValid = false;
        bool arkFlagsValid = false;
        std::vector<RemoteDoorPhysicsPartState> parts;
    };
    std::unordered_map<EntityId, RemoteEnemyPhysicsSnapshot> m_remoteEnemyPhysicsSnapshots;
    // Only entities actually mutated by the mirror policy may enter the
    // takeover repair path. Fresh/local owners must remain read-only here.
    std::unordered_set<EntityId> m_remoteEnemyPhysicsTouchedEntities;
    bool m_applyingRemoteAreaObjectEvent = false;
    bool m_applyingRemoteAreaObjectAuthorityInput = false;
    float m_remoteAreaObjectConsequenceCaptureSeconds = 0.0f;
    uint16_t m_remoteAreaObjectConsequenceInputKind = 0;
    uint64_t m_remoteAreaObjectConsequenceInputEventId = 0;
    uint32_t m_remoteAreaObjectAuthorityInputs = 0;
    uint32_t m_capturedAreaObjectAuthorityOutcomes = 0;
    uint32_t m_suppressedUnownedKioskOutputs = 0;
    struct PendingRemoteDoorPowerConvergence
    {
        CoopProtocol::AreaObjectEventPacket packet = {};
        float remainingSeconds = 0.0f;
        float retryAccumulator = 0.0f;
        bool eventFinalized = false;
    };
    std::unordered_map<uint64_t, PendingRemoteDoorPowerConvergence> m_pendingRemoteDoorPowerConvergence;
    uint32_t m_remoteDoorPowerConvergenceQueued = 0;
    uint32_t m_remoteDoorPowerConvergenceRetries = 0;
    uint32_t m_remoteDoorPowerConvergenceCompleted = 0;
    uint32_t m_remoteDoorPowerConvergenceExpired = 0;
    float m_pendingArkElevatorTransitScanSeconds = 0.0f;
    float m_arkElevatorTransitScanAccumulator = 0.0f;
    uint64_t m_pendingArkElevatorTransitScanGuid = 0;
    std::unordered_map<uint64_t, std::string> m_activeArkElevatorTransitByGuid;
    float m_mainLiftOutageScanAccumulator = 0.0f;
    float m_mainLiftOutageEnemyCaptureSeconds = 0.0f;
    bool m_mainLiftOutageAnnounced = false;
    bool m_mainLiftOutagePresented = false;
    bool m_mainLiftOutageAwaitingContinuation = false;
    bool m_mainLiftOutageSuppressedForLocalPlayer = false;
    bool m_mainLiftOutsideOutageAwaitingNativeContinuation = false;
    bool m_replayingMainLiftOutageRemoteEvent = false;
    bool m_pendingMainLiftOutagePresentation = false;
    uint32_t m_pendingMainLiftOutagePresentationDelayFrames = 0;
    uint32_t m_pendingMainLiftOutagePresentationAttempts = 0;
    uint32_t m_mainLiftOutageAnnouncements = 0;
    uint32_t m_mainLiftOutagePresentations = 0;
    uint32_t m_mainLiftOutageDuplicateObservations = 0;
    uint32_t m_mainLiftOutageScopeSkips = 0;
    uint32_t m_mainLiftOutageEnemyRelocations = 0;
    uint64_t m_mainLiftOutageEventId = 0;
    uint64_t m_mainLiftOutageEnemyStableId = 0;
    EntityId m_mainLiftOutageEnemyEntityId = INVALID_ENTITYID;
    std::string m_lastMainLiftOutageEvent = "-";
    EntityId m_mainLiftPassengerEntityId = INVALID_ENTITYID;
    Vec3 m_mainLiftLastPosition = Vec3(ZERO);
    bool m_mainLiftHasLastPosition = false;
    bool m_mainLiftPassengerAttached = false;
    float m_mainLiftPassengerRelativeZ = 0.0f;
    uint32_t m_mainLiftPassengerStationaryFrames = 0;
    uint32_t m_mainLiftPassengerAttachments = 0;
    uint32_t m_mainLiftPassengerCorrections = 0;
    uint32_t m_mainLiftPassengerReleases = 0;
    std::string m_lastMainLiftPassengerEvent = "-";
    // The Lobby lift is a large parented script mover. Its reliable transit
    // event starts the same native SCP move on observers outside packet receive;
    // LiveProp packets are checkpoints only and never drive its hierarchy.
    bool m_remoteMainLiftPresentationActive = false;
    bool m_remoteMainLiftPhysicsSuspended = false;
    Vec3 m_remoteMainLiftTargetPosition = Vec3(ZERO);
    Vec3 m_remoteMainLiftVelocity = Vec3(ZERO);
    float m_remoteMainLiftLastPacketTime = -1000.0f;
    float m_remoteMainLiftPresentationSettleSeconds = 0.0f;
    uint32_t m_remoteMainLiftPresentationFrames = 0;
    std::string m_lastRemoteMainLiftPresentationEvent = "-";
    bool m_pendingRemoteMainLiftTransit = false;
    bool m_pendingRemoteMainLiftObserverReplay = false;
    bool m_replayingRemoteMainLiftKioskPress = false;
    uint32_t m_pendingRemoteMainLiftTransitDelayFrames = 0;
    uint32_t m_pendingRemoteMainLiftTransitAttempts = 0;
    std::string m_pendingRemoteMainLiftTransitLink;
    Vec3 m_pendingRemoteMainLiftTransitTargetPosition = Vec3(ZERO);
    struct PendingKioskActionRecovery
    {
        bool active = false;
        uint64_t dueTickMs = 0;
        uint64_t kioskGuid = 0;
        uint64_t doorGuid = 0;
        int button = -1;
        bool initialDoorPowered = false;
        bool desiredDoorPowered = false;
        uint32_t worldEpoch = 0;
    };
    PendingKioskActionRecovery m_pendingKioskActionRecovery;
    float m_remoteAreaObjectEchoSuppressSeconds = 0.0f;
    uint32_t m_localAreaObjectCommandMutationDepth = 0;
    std::unordered_map<uint64_t, SharedDropRecord> m_sharedDrops;
    std::unordered_map<EntityId, uint64_t> m_sharedDropByEntityId;
    uint32_t m_sharedDropSequence = 0;
    uint32_t m_sharedDropSent = 0;
    uint32_t m_sharedDropReceived = 0;
    uint32_t m_sharedDropApplied = 0;
    uint32_t m_sharedDropDropped = 0;
    uint32_t m_sharedDropPickupRequests = 0;
    uint32_t m_sharedDropPickupCommits = 0;
    uint32_t m_sharedDropPickupSuppressions = 0;
    uint32_t m_sharedDropDuplicateCommits = 0;
    uint32_t m_sharedDropEntityRetirements = 0;
    uint32_t m_sharedDropApplyDepth = 0;
    std::unordered_map<uint64_t, SharedStorageRecord> m_sharedStorages;
    std::unordered_map<uint64_t, SharedStorageAssembly> m_sharedStorageAssemblies;
    CArkExternalInventoryUI* m_pendingSharedStorageUi = nullptr;
    ArkInventory* m_pendingSharedStorageInventory = nullptr;
    CArkExternalInventoryUI* m_activeSharedStorageUi = nullptr;
    ArkInventory* m_activeSharedStorageInventory = nullptr;
    uint64_t m_activeSharedStorageGuid = 0;
    uint32_t m_sharedStorageSequence = 0;
    uint32_t m_sharedStorageTransaction = 0;
    uint32_t m_sharedStorageSent = 0;
    uint32_t m_sharedStorageReceived = 0;
    uint32_t m_sharedStorageApplied = 0;
    uint32_t m_sharedStorageDropped = 0;
    uint32_t m_sharedStorageDenied = 0;
    uint32_t m_sharedStorageApplyDepth = 0;
    std::string m_lastSharedStorageEvent = "-";
    uint32_t m_hazardEventSequence = 0;
    uint32_t m_hazardEventSent = 0;
    uint32_t m_hazardEventReceived = 0;
    uint32_t m_hazardEventApplied = 0;
    uint32_t m_hazardEventDropped = 0;
    uint32_t m_hazardEventApplyDepth = 0;
    uint32_t m_surfaceHazardObserverRequests = 0;
    uint32_t m_surfaceHazardNonAuthoritySuppressions = 0;
    std::unordered_set<uint64_t> m_appliedHazardEventIds;
    std::mutex m_sentExplosiveTankEventMutex;
    std::unordered_set<uint64_t> m_sentExplosiveTankEventGuids;
    std::unordered_set<EntityId> m_sentLocalHazardEntityIds;
    std::unordered_set<EntityId> m_remoteHazardEntityIds;
    std::string m_lastHazardEvent = "-";
    uint64_t m_dialogueLeaseDialogueId = 0;
    uint64_t m_dialogueLeasePendingId = 0;
    uint64_t m_dialogueLeaseOwnerHash = 0;
    uint64_t m_dialogueLeaseTriggerEventId = 0;
    uint64_t m_dialogueLeaseRuleId = 0;
    uint32_t m_dialogueLeaseSequence = 0;
    uint32_t m_dialogueLeaseEpoch = 0;
    uint32_t m_dialogueLeaseSent = 0;
    uint32_t m_dialogueLeaseReceived = 0;
    uint32_t m_dialogueLeaseApplied = 0;
    uint32_t m_dialogueLeaseDropped = 0;
    uint32_t m_dialogueLeaseDenied = 0;
    uint32_t m_remoteDialogueReplayDepth = 0;
    float m_dialogueLeaseSeconds = 0.0f;
    float m_dialogueLeaseActivitySendSeconds = 0.0f;
    float m_pendingRemoteDialogueReplaySeconds = 0.0f;
    bool m_dialogueLeasePendingCompletion = false;
    bool m_pendingRemoteDialogueReplayActive = false;
    CoopProtocol::DialogueLeasePacket m_dialogueLeaseDescriptor = {};
    CoopProtocol::DialogueLeasePacket m_pendingRemoteDialogueReplay = {};
    std::unordered_set<uint64_t> m_appliedDialogueLeaseEventIds;
    std::unordered_set<uint64_t> m_appliedDialogueTriggerIds;
    std::unordered_set<uint64_t> m_remoteDialogueRuntimeIds;
    std::unordered_set<uint64_t> m_remoteDialogueCompletedDuringReplayIds;
    std::unordered_set<uint64_t> m_remoteDialogueStoryIds;
    std::unordered_map<uint64_t, ArkSpeakerBase*> m_dialogueSpeakersByGuid;
    std::unordered_map<uint64_t, ArkSpeakerBase*> m_dialogueSpeakersByCharacterId;
    std::deque<CoopProtocol::StoryEventPacket> m_pendingDialogueWritebacks;
    float m_pendingDialogueWritebackRetrySeconds = 0.0f;
    std::string m_lastDialogueLeaseEvent = "-";
    uint32_t m_timeDilationSequence = 0;
    uint32_t m_timeDilationRevision = 0;
    uint32_t m_timeDilationSent = 0;
    uint32_t m_timeDilationReceived = 0;
    uint32_t m_timeDilationApplied = 0;
    uint32_t m_timeDilationDropped = 0;
    uint32_t m_timeDilationApplyDepth = 0;
    uint64_t m_timeDilationOwnerHash = 0;
    unsigned m_timeDilationTimers = 0;
    float m_timeDilationScale = 1.0f;
    int m_timeDilationLocalHandle = -1;
    int m_timeDilationRemoteHandle = -1;
    std::string m_lastTimeDilationEvent = "-";
    std::string m_lastSharedDropEvent = "-";
    bool m_applyingRemoteGooResult = false;
    uint32_t m_networkOriginatedGooDestroyDepth = 0;
    std::unordered_set<EntityId> m_networkOriginatedGooDestroyEntityIds;
    bool m_applyingRemoteEnemyProjectileEvent = false;
    bool m_applyingRemoteEnemyAbilityFxEvent = false;
    bool m_applyingRemoteEnemyDeathCommit = false;
    uint32_t m_remoteCystoidNestTriggerDepth = 0;
    std::vector<NetworkOriginatedCystoidNestZone> m_networkOriginatedCystoidNestZones;
    std::vector<NetworkOriginatedCystoidEntity> m_networkOriginatedCystoidEntities;
    std::unordered_set<uint64_t> m_networkConsumedCystoidNestNetIds;
    std::unordered_set<EntityId> m_networkConsumedCystoidNestEntityIds;
    bool m_applyingRemoteEnemyGlooState = false;
    bool m_applyingRemoteEnemyPersistentStatus = false;
    uint32_t m_remoteProxyTransformWriteDepth = 0;
    uint32_t m_remoteEnemyTransformWriteDepth = 0;
    std::unordered_set<EntityId> m_remoteGooResultEntityIds;
    std::unordered_set<EntityId> m_sentLocalGooResultSourceEntityIds;
    std::unordered_set<EntityId> m_sentLocalGooHardCommitSourceEntityIds;
    std::unordered_set<EntityId> m_sentLocalGooDestroySourceEntityIds;
    std::unordered_map<EntityId, GooSpawnContext> m_localGooSpawnContexts;
    std::unordered_map<uint32_t, EntityId> m_remoteGooSourceToEntityIds;
    std::unordered_map<uint32_t, uint32_t> m_remoteGooEntityToSourceIds;
    std::unordered_map<uint32_t, uint64_t> m_remoteGooSourceToEnemyNetIds;
    uint32_t m_enemyMovementRequestBlocks = 0;
    uint32_t m_enemyLookRequestBlocks = 0;
    uint32_t m_enemyReadOnlyIntentCaptures = 0;
    uint32_t m_enemyReadOnlyIntentBlocks = 0;
    uint32_t m_enemyReadOnlyFacingMixApplies = 0;
    uint32_t m_enemyReadOnlyFacingMixRejects = 0;
    uint32_t m_enemyRemoteLegBlendApplies = 0;
    uint32_t m_enemyRemoteLegBlendFailures = 0;
    uint32_t m_enemyTransformRequestBlocks = 0;
    uint32_t m_enemyTransformSmoothTicks = 0;
    uint32_t m_enemyPacketTransformDeferrals = 0;
    float m_enemyRemoteMaxBacklogDistance = 0.0f;
    float m_enemyRemoteMaxCatchupSpeed = 0.0f;
    uint32_t m_mimicDeathCommitRepeatsRemaining = 0;
    uint32_t m_routedEventCount = 0;
    uint32_t m_droppedEventCount = 0;
    uint32_t m_requiredReliableEventCount = 0;
    uint32_t m_lastProxyTargetBindings = 0;
    uint32_t m_lastProxyCombatStimulusCount = 0;
    uint32_t m_lastProxyAbilityAttempts = 0;
    uint32_t m_lastProxyAbilitySuccesses = 0;
    uint32_t m_lastProxySimpleAttentionStimulus = 0;
    uint32_t m_lastProxySimpleAttentionTracked = 0;
    uint32_t m_lastProxyComplexAttentionTracked = 0;
    uint32_t m_proxyAttentionClearRuns = 0;
    uint32_t m_proxyAttentionClearGuarded = 0;
    uint32_t m_proxyLifecycleSerial = 0;
    uint32_t m_aiDebugEnemyCount = 0;
    uint32_t m_aiDebugHostTopTargetCount = 0;
    uint32_t m_aiDebugProxyTopTargetCount = 0;
    uint32_t m_aiDebugOtherTopTargetCount = 0;
    uint32_t m_remoteModBuild = 0;
    uint32_t m_localWorldEpoch = 1;
    uint32_t m_remoteWorldEpoch = 0;
    uint32_t m_localLevelEpoch = 1;
    uint32_t m_remoteLevelEpoch = 0;
    uint32_t m_remoteSessionFlags = 0;
    uint64_t m_localLevelId = 0;
    uint64_t m_remoteLevelId = 0;
    Vec3 m_remotePlayerLocation = Vec3(ZERO);
    float m_lastProxyHealth = 0.0f;
    float m_lastProxyMaxHealth = 0.0f;
    float m_lastMimicHealth = 0.0f;
    float m_lastMimicMaxHealth = 0.0f;
    float m_lastMimicDamage = 0.0f;
    float m_lastAppliedDamage = 0.0f;
    int32_t m_lastAppliedDamageType = 0;
    int32_t m_lastAppliedDamageMaterial = 0;
    int32_t m_lastAppliedDamageBulletType = 0;
    uint32_t m_lastAppliedDamageFlags = 0;
    uint32_t m_suppressedDownedActions = 0;
    uint32_t m_suppressedDownedHealthSets = 0;
    uint32_t m_suppressedDownedDeathEvents = 0;
    uint32_t m_suppressedDownedForceKills = 0;
    uint32_t m_suppressedDownedDeathScreens = 0;
    uint32_t m_suppressedDownedRagdollizes = 0;
    uint32_t m_suppressedDownedDeathMovementStates = 0;
    uint32_t m_overriddenDownedIsDeadQueries = 0;
    uint32_t m_nativeDeathFeedbackRuns = 0;
    uint32_t m_nativeDeathRecoveries = 0;
    uint32_t m_suppressedProxyPlayerHits = 0;
    uint32_t m_proxyFriendlyFirePacketsSent = 0;
    uint32_t m_proxyNonPlayerHitsSuppressed = 0;
    uint32_t m_remoteFriendlyFirePacketsRejected = 0;
    uint32_t m_remoteFriendlyFirePacketsApplied = 0;
    std::string m_lastFriendlyFirePolicyEvent = "-";
    uint32_t m_damageDedupeLocalObservations = 0;
    uint32_t m_damageDedupeProxyObservations = 0;
    uint32_t m_damageDedupeRemoteDrops = 0;
    uint32_t m_damageDedupeExactDrops = 0;
    uint32_t m_damageDedupeRawDrops = 0;
    uint32_t m_damageDedupeSignalObservations = 0;
    uint32_t m_damageDedupeSignalKeys = 0;
    uint32_t m_damageDedupeSignalSkips = 0;
    uint32_t m_damageDedupeHealthSignals = 0;
    uint32_t m_proxyNoPropCollisionApplies = 0;
    uint32_t m_proxyNoPropCollisionFailures = 0;
    uint32_t m_proxyNativeAiActionBlocks = 0;
    uint32_t m_remoteProxyTransformWrites = 0;
    uint32_t m_remoteProxyTransformRequestBlocks = 0;
    uint32_t m_remoteEnemyPhysicsCaptures = 0;
    uint32_t m_remoteEnemyPhysicsRestores = 0;
    uint32_t m_remoteEnemyPhysicsFailures = 0;
    uint32_t m_remoteEnemyPhysicsDrops = 0;
    uint32_t m_remoteEnemyRagdollSuppressions = 0;
    uint32_t m_remoteEnemyAuthorityRagdollPasses = 0;
    uint32_t m_remoteEnemyAuthorityRagdollApplies = 0;
    uint32_t m_remoteEnemyAuthorityRagdollClears = 0;
    uint32_t m_remoteEnemyAuthorityRagdollFailures = 0;
    std::string m_lastRemoteEnemyRagdollEvent = "-";
    uint32_t m_animationTestProxySpawns = 0;
    uint32_t m_animationTestProxyPlays = 0;
    uint32_t m_animationTestProxyFailures = 0;
    uint32_t m_npcMannequinActionTraceCount = 0;
    uint32_t m_npcMannequinActionTraceFailures = 0;
    uint32_t m_npcMannequinConstructTraceCount = 0;
    uint32_t m_npcMannequinConstructTraceFailures = 0;
    uint32_t m_npcSemanticTraceCount = 0;
    uint32_t m_npcSemanticTraceFailures = 0;
    uint32_t m_enemySyncTraceSerial = 0;
    uint32_t m_enemyMannequinAuthorityObservations = 0;
    uint32_t m_enemyMannequinLocalSuppressions = 0;
    uint32_t m_enemyRemoteAnimationApplies = 0;
    uint32_t m_enemyRemoteAnimationSkips = 0;
    uint32_t m_enemyRemoteAnimationFailures = 0;
    uint32_t m_remoteProxySlotVisualApplies = 0;
    uint32_t m_remoteProxySlotVisualSkips = 0;
    uint32_t m_proxyWeaponVisualApplies = 0;
    uint32_t m_proxyWeaponVisualClears = 0;
    uint32_t m_proxyWeaponVisualFailures = 0;
    uint32_t m_proxyWeaponMuzzleEffectTriggers = 0;
    uint32_t m_proxyWeaponMuzzleEffectClears = 0;
    uint32_t m_proxyWeaponMuzzleEffectFailures = 0;
    uint32_t m_proxyRepeatedStanceSkips = 0;
    uint32_t m_animationQueueLogHits = 0;
    uint32_t m_animationQueueTraceWrites = 0;
    uint32_t m_physicalWorldBoxHookCalls = 0;
    uint32_t m_physicalWorldBoxHookHugeQueries = 0;
    uint32_t m_physicalWorldBoxHookQueuedRepairs = 0;
    uint32_t m_hugePhysicsBoundsRepairScans = 0;
    uint32_t m_hugePhysicsBoundsRepairHits = 0;
    uint32_t m_hugePhysicsBoundsRepairFailures = 0;
    uint32_t m_hugePhysicsBoundsRepairQuarantines = 0;
    uint32_t m_hugePhysicsBoundsKnownMoverSkips = 0;
    uint32_t m_animationQueueTraceSkips = 0;
    float m_lastAnimationQueueTraceTime = -1000.0f;
    uint32_t m_timeScaleOverrideCount = 0;
    uint32_t m_timeScaleUpdateCount = 0;
    uint32_t m_timeScaleClearCount = 0;
    uint32_t m_timeScaleNeutralizedCount = 0;
    uint32_t m_focusModeStartCount = 0;
    uint32_t m_focusModeStopCount = 0;
    uint32_t m_focusModeSuppressedStartCount = 0;
    uint32_t m_focusModeCleanupCount = 0;
    uint32_t m_focusTimeScaleClearCount = 0;
    uint32_t m_localAttentionClearAttempts = 0;
    uint32_t m_localAttentionClearEnemiesTouched = 0;
    uint32_t m_localAttentionClearSimpleHits = 0;
    uint32_t m_localAttentionClearComplexHits = 0;
    uint32_t m_localAttentionClearTopTargetHits = 0;
    uint32_t m_localAttentionClearNpcClearPlayerCalls = 0;
    uint32_t m_localAttentionClearManagerSimpleCalls = 0;
    uint32_t m_localAttentionClearManagerComplexCalls = 0;
    uint32_t m_localAttentionClearLegacyNpcCalls = 0;
    uint32_t m_localAttentionClearProxyRebinds = 0;
    uint32_t m_localAttentionClearUnsafeSkips = 0;
    uint32_t m_localAttentionClearPointerSkips = 0;
    uint32_t m_localAttentionObjectDisableCalls = 0;
    uint32_t m_localAttentionObjectReleaseCalls = 0;
    uint32_t m_localAttentionObjectErrors = 0;
    uint32_t m_saveGameHookCalls = 0;
    uint32_t m_loadGameHookCalls = 0;
    uint32_t m_loadGuardBeginCount = 0;
    uint32_t m_loadGuardEndCount = 0;
    uint32_t m_loadGuardRuntimeResetCount = 0;
    uint32_t m_systemLoadEventCount = 0;
    uint32_t m_postLoadNativeCompleteCount = 0;
    uint32_t m_postLoadInteractiveReadyCount = 0;
    uint32_t m_postLoadInteractiveWaitCount = 0;
    uint32_t m_deferredAutoReengageRequests = 0;
    uint32_t m_transitionPhaseSerial = 0;
    uint32_t m_flashLoadingProgressCalls = 0;
    uint32_t m_flashLoadingCompleteCalls = 0;
    uint32_t m_flashLoadingErrorCalls = 0;
    uint32_t m_flashShowLoadingScreenCalls = 0;
    uint32_t m_flashHideLoadingScreenCalls = 0;
    uint32_t m_flashLoadtimeUpdateCalls = 0;
    uint32_t m_flashLoadtimeRenderCalls = 0;
    uint32_t m_clientSaveRedirectCount = 0;
    uint32_t m_clientSaveRedirectBypassCount = 0;
    uint32_t m_sessionWorldResetCount = 0;
    uint32_t m_sessionEpochMismatchCount = 0;
    uint32_t m_reliableSendSequence = 0;
    uint32_t m_reliableRecvSequence = 0;
    uint32_t m_reliableAckedSequence = 0;
    uint32_t m_reliableEnqueuedPackets = 0;
    uint32_t m_reliableSentPackets = 0;
    uint32_t m_reliableResentPackets = 0;
    uint32_t m_reliableReceivedPackets = 0;
    uint32_t m_reliableAckSentPackets = 0;
    uint32_t m_reliableAckReceivedPackets = 0;
    uint32_t m_reliableDroppedPackets = 0;
    uint32_t m_reliableRetiredAreaPackets = 0;
    uint32_t m_reliableCoalescedPackets = 0;
    uint32_t m_reliableSupersededPackets = 0;
    uint32_t m_reliableTimeoutDisconnects = 0;
    uint32_t m_reliableDebugDroppedAcks = 0;
    bool m_reliableBacklogDisconnectPending = false;
    bool m_debugPauseReliableSends = false;
    bool m_debugDropReliableAcks = false;
    float m_debugReliableMaxAgeSeconds = 0.0f;
    uint32_t m_debugReliableMaxSendAttempts = 0;
    uint32_t m_rejectedSavedCoopRuntimeSpawns = 0;
    uint32_t m_quarantinedSavedCoopRuntimeSpawns = 0;
    uint32_t m_saveRuntimePurgeCount = 0;
    uint32_t m_runtimeTransitionCleanupCount = 0;
    uint32_t m_runtimeTransitionCleanupEntities = 0;
    uint32_t m_runtimeTransitionCleanupFailures = 0;
    uint32_t m_arkNpcAbilityOwnerRemovalGuardChecks = 0;
    uint32_t m_arkNpcAbilityOwnerRemovalGuardSkips = 0;
    uint32_t m_transitionVisualUnregisterSkips = 0;
    std::unordered_set<EntityId> m_quarantinedSavedCoopRuntimeEntityIds;
    std::unordered_set<EntityId> m_transitionRuntimeProxyEntityIds;
    std::unordered_set<EntityId> m_transitionRuntimeEntityIds;
    uint32_t m_coopRuntimeEntitySerializeCalls = 0;
    uint32_t m_coopRuntimeEntitySerializeLogs = 0;
    uint32_t m_coopRuntimeEntitySerializeSkips = 0;
    uint32_t m_worldSyncSequence = 0;
    uint32_t m_sentWorldSyncPackets = 0;
    uint32_t m_receivedWorldSyncPackets = 0;
    uint32_t m_lastWorldSyncSequence = 0;
    uint32_t m_pendingHostWorldEpoch = 0;
    uint32_t m_lastClientWorldReadySentEpoch = 0;
    uint32_t m_lastClientWorldReadySentLevelId = 0;
    uint32_t m_lastClientWorldReadyReceivedEpoch = 0;
    uint32_t m_lastClientWorldReadyReceivedLevelId = 0;
    float m_deferredClientWorldReadySeconds = 0.0f;
    float m_remoteHostLoadNoticeStartTime = -1.0f;
    float m_remoteHostLoadNoticeGraceUntil = -1.0f;
    uint32_t m_saveTransferSequence = 0;
    uint32_t m_playerStateTransferSequence = 0;
    uint32_t m_areaJournalTransferSequence = 0;
    uint32_t m_saveTransferId = 0;
    uint32_t m_playerStateTransferId = 0;
    uint32_t m_areaJournalTransferId = 0;
    uint32_t m_deferredAreaJournalTransferId = 0;
    uint32_t m_saveTransferTotalBytes = 0;
    uint32_t m_playerStateTransferTotalBytes = 0;
    uint32_t m_areaJournalTransferTotalBytes = 0;
    uint32_t m_saveTransferChunkCount = 0;
    uint32_t m_playerStateTransferChunkCount = 0;
    uint32_t m_areaJournalTransferChunkCount = 0;
    uint32_t m_saveTransferNextChunk = 0;
    uint32_t m_playerStateTransferNextChunk = 0;
    uint32_t m_areaJournalTransferNextChunk = 0;
    uint32_t m_saveTransferReceivedChunks = 0;
    uint32_t m_playerStateTransferReceivedChunks = 0;
    uint32_t m_areaJournalTransferReceivedChunks = 0;
    uint32_t m_saveTransferChecksum = 0;
    uint32_t m_playerStateTransferChecksum = 0;
    uint32_t m_areaJournalTransferChecksum = 0;
    uint32_t m_saveTransferRunningChecksum = 0;
    uint32_t m_playerStateTransferRunningChecksum = 0;
    uint32_t m_areaJournalTransferRunningChecksum = 0;
    uint32_t m_playerStateTransferFlags = 0;
    uint32_t m_areaJournalTransferFlags = 0;
    uint32_t m_areaSnapshotSequenceCounter = 0;
    uint32_t m_areaSnapshotLeaseEpoch = 0;
    uint32_t m_areaSnapshotLevelEpoch = 0;
    uint64_t m_areaSnapshotAreaId = 0;
    uint64_t m_areaSnapshotHostSaveKeyHash = 0;
    uint64_t m_areaSnapshotSequence = 0;
    uint32_t m_playerStateTransferAddress = 0;
    uint32_t m_saveTransferAddress = 0;
    uint16_t m_saveTransferPort = 0;
    uint64_t m_saveTransferAccountToken = 0;
    uint64_t m_pendingHostPlayerStateAccountToken = 0;
    uint32_t m_areaJournalTransferAddress = 0;
    uint16_t m_playerStateTransferPort = 0;
    uint16_t m_areaJournalTransferPort = 0;
    uint32_t m_sentSaveTransferPackets = 0;
    uint32_t m_receivedSaveTransferPackets = 0;
    uint32_t m_sentPlayerStateTransferPackets = 0;
    uint32_t m_receivedPlayerStateTransferPackets = 0;
    uint32_t m_hostPlayerStateUploadRequests = 0;
    uint32_t m_hostPlayerStateUploadCompletions = 0;
    uint32_t m_hostPlayerStateUploadFailures = 0;
    uint32_t m_sentAreaJournalTransferPackets = 0;
    uint32_t m_receivedAreaJournalTransferPackets = 0;
    uint32_t m_storedAreaJournalTransferCount = 0;
    uint32_t m_livePropSequence = 0;
    uint32_t m_sentLivePropPackets = 0;
    uint32_t m_receivedLivePropPackets = 0;
    uint32_t m_appliedLivePropPackets = 0;
    uint32_t m_droppedLivePropPackets = 0;
    uint64_t m_sentLivePropBytes = 0;
    uint64_t m_receivedLivePropBytes = 0;
    uint32_t m_livePropCarriedApplies = 0;
    uint32_t m_livePropMovingApplies = 0;
    uint32_t m_livePropIdleApplies = 0;
    uint32_t m_livePropBallisticStarts = 0;
    uint32_t m_livePropBallisticApplies = 0;
    uint32_t m_livePropBallisticCorrections = 0;
    uint32_t m_livePropThrowCalls = 0;
    uint32_t m_livePropThrowSuccesses = 0;
    uint32_t m_livePropStopCarryCalls = 0;
    uint32_t m_livePropStopCarryThrown = 0;
    uint32_t m_livePropStopCarrySerializeSkips = 0;
    uint32_t m_livePropCollisionEvents = 0;
    uint32_t m_livePropCollisionAuthorityGrants = 0;
    uint32_t m_livePropCollisionChainGrants = 0;
    uint32_t m_livePropAttackCollisionGrants = 0;
    uint32_t m_livePropEnemyBodyCollisionGrants = 0;
    uint32_t m_livePropRemoteProxyCollisionSuppressions = 0;
    std::unordered_map<uint64_t, float> m_livePropRemoteProxyContactSuppressUntil;
    uint64_t m_livePropDebugFocusGuid = 0;
    uint32_t m_livePropDebugTraceSerial = 0;
    std::deque<std::string> m_livePropDebugTrace;
    uint64_t m_livePropEventTraceTotal = 0;
    uint64_t m_livePropEventTraceXform = 0;
    uint64_t m_livePropEventTraceHide = 0;
    uint64_t m_livePropEventTraceUnhide = 0;
    uint64_t m_livePropEventTraceCarried = 0;
    uint64_t m_livePropEventTraceKnown = 0;
    uint64_t m_livePropEventTraceOpened = 0;
    uint64_t m_livePropEventTraceQueueAttempts = 0;
    uint64_t m_livePropEventTraceRejected = 0;
    uint32_t m_livePropEventTraceWindowTotal = 0;
    uint32_t m_livePropEventTraceWindowXform = 0;
    uint32_t m_livePropEventTraceWindowHide = 0;
    uint32_t m_livePropEventTraceWindowUnhide = 0;
    uint32_t m_livePropEventTraceWindowCarried = 0;
    uint32_t m_livePropEventTraceWindowKnown = 0;
    uint32_t m_livePropEventTraceWindowOpened = 0;
    uint32_t m_livePropEventTraceWindowQueueAttempts = 0;
    uint32_t m_livePropEventTraceWindowRejected = 0;
    float m_livePropEventTraceLastLogTime = 0.0f;
    uint32_t m_peerTimeoutCount = 0;
    uint32_t m_guardedEntityRemoveAttempts = 0;
    uint32_t m_guardedEntityRemoveSuccesses = 0;
    uint32_t m_guardedEntityRemoveFailures = 0;
    uint32_t m_guardedEntityRemoveDeferrals = 0;
    uint32_t m_spawnDiagnosticsRuns = 0;
    uint32_t m_spawnDiagnosticsNewEntities = 0;
    uint32_t m_nativeEntityLifecycleTraceEvents = 0;
    uint32_t m_nativeEntityRemoveTraceCalls = 0;
    EntityId m_nativeEntityRemoveCurrentId = INVALID_ENTITYID;
    bool m_nativeEntityRemoveCurrentForce = false;
    uint32_t m_nativeNpcSpawnTraceEvents = 0;
    uint32_t m_serverAreaStateMergeCount = 0;
    uint32_t m_serverAreaStateRejectCount = 0;
    uint64_t m_serverAreaStateReceivedBytes = 0;
    uint32_t m_areaOverlayApplyCount = 0;
    uint32_t m_areaOverlayApplyFailCount = 0;
    uint32_t m_areaOverlayAppliedRows = 0;
    uint32_t m_areaOverlayAppliedEntities = 0;
    uint32_t m_hostPlayerStateSnapshotCopies = 0;
    uint32_t m_hostPlayerStateSnapshotFailures = 0;
    uint32_t m_blockedNonClientPlayerStateApplies = 0;
    uint32_t m_playerStateNativeLoadMerges = 0;
    uint32_t m_playerStateNativeLoadMergeFailures = 0;
    uint32_t m_playerSidecarSaveCount = 0;
    uint32_t m_playerSidecarLoadCount = 0;
    uint32_t m_playerSidecarApplyCount = 0;
    uint32_t m_playerSidecarFailCount = 0;
    uint32_t m_playerSidecarInventorySaved = 0;
    uint32_t m_playerSidecarInventoryApplied = 0;
    uint32_t m_playerSidecarInventoryPending = 0;
    uint32_t m_playerSidecarInventoryRestoreAttempts = 0;
    uint32_t m_playerSidecarInventoryFeedbackSuppressDepth = 0;
    uint32_t m_playerSidecarInventoryFeedbackSuppressed = 0;
    uint32_t m_playerSidecarChipsetsSaved = 0;
    uint32_t m_playerSidecarChipsetsApplied = 0;
    uint32_t m_playerSidecarChipsetsPending = 0;
    uint32_t m_nativeInventorySerializeCalls = 0;
    uint32_t m_nativeInventorySerializeReadCalls = 0;
    uint32_t m_nativeInventoryPostSerializeCalls = 0;
    uint32_t m_nativeInventoryLocalPlayerRestorePoints = 0;
    uint32_t m_nativePlayerSerializeCalls = 0;
    uint32_t m_nativePlayerSerializeReadCalls = 0;
    uint32_t m_nativePlayerPostSerializeCalls = 0;
    uint32_t m_nativePlayerComponentPostSerializeCalls = 0;
    uint32_t m_nativeItemSystemSerializeCalls = 0;
    uint32_t m_nativeItemSerializeCalls = 0;
    uint32_t m_nativeItemPostSerializeCalls = 0;
    uint32_t m_nativeItemLifecycleCalls = 0;
    uint32_t m_nativeInventoryMutationCalls = 0;
    uint32_t m_nativeItemOwnerMutationCalls = 0;
    uint32_t m_nativeInventoryTraceLogs = 0;
    uint32_t m_nativeSideBlobSaveNotifyCalls = 0;
    uint32_t m_nativeSideBlobLoadNotifyCalls = 0;
    uint32_t m_nativeSideBlobWriteSuccesses = 0;
    uint32_t m_nativeSideBlobReadSuccesses = 0;
    uint32_t m_nativeSideBlobFailures = 0;
    uint32_t m_nativeSideBlobMirrorWriteSections = 0;
    uint32_t m_nativeSideBlobMirrorReadSections = 0;
    uint32_t m_nativeSideBlobMirrorItemWriteSections = 0;
    uint32_t m_nativeSideBlobMirrorItemReadSections = 0;
    uint32_t m_nativeSideBlobMirrorFailures = 0;
    uint32_t m_nativeSideBlobCapturedWriteSections = 0;
    uint32_t m_nativeSideBlobCapturedReadSections = 0;
    uint32_t m_nativeSideBlobCapturedItemWrites = 0;
    uint32_t m_nativeSideBlobCapturedItemReads = 0;
    uint32_t m_nativeSideBlobCapturedFailures = 0;
    uint32_t m_nativePlayerXmlPatchAttempts = 0;
    uint32_t m_nativePlayerXmlPatchSuccesses = 0;
    uint32_t m_nativePlayerXmlPatchFailures = 0;
    uint32_t m_nativePlayerXmlDiagnosticsWrites = 0;
    uint32_t m_startupTraceUpdates = 0;
    uint32_t m_debugInventoryGiveCount = 0;
    uint32_t m_debugInventoryClearCount = 0;
    uint32_t m_debugInventoryCountChecks = 0;
    uint32_t m_debugInventoryRestoreFixtureCount = 0;
    uint32_t m_playerSidecarAbilitiesSaved = 0;
    uint32_t m_playerSidecarAbilitiesApplied = 0;
    uint32_t m_levelTransitionDoorScanCount = 0;
    uint32_t m_levelTransitionConfirmedHookCalls = 0;
    uint32_t m_levelTransitionInvokeAttempts = 0;
    uint32_t m_levelTransitionInvokeSuccesses = 0;
    uint32_t m_levelTransitionGuardSkips = 0;
    uint32_t m_levelTransitionEpochKeeps = 0;
    uint32_t m_levelTransitionPresentationRepairs = 0;
    uint32_t m_levelTransitionPresentationCancelCalls = 0;
    uint32_t m_levelTransitionPresentationFailures = 0;
    uint32_t m_levelTransitionScreenLocationCorrections = 0;
    EntityId m_lastLevelTransitionScreenLocationOwnerId = INVALID_ENTITYID;
    std::unordered_map<EntityId, uint64_t> m_levelTransitionPresentationAttemptEpochs;
    uint32_t m_levelStateSaveCalls = 0;
    uint32_t m_levelStateLoadCalls = 0;
    uint32_t m_levelStateArkGameSerializeCalls = 0;
    uint32_t m_levelStateArkGameSerializeReadCalls = 0;
    uint32_t m_levelStatePersistentSerializeCalls = 0;
    uint32_t m_levelStatePersistentSerializeReadCalls = 0;
    uint32_t m_levelStateSerializerVtableDumps = 0;
    uint32_t m_levelStateSerializerTraceCalls = 0;
    uint32_t m_levelStateSerializerTraceOps = 0;
    uint32_t m_levelStateSerializerTraceRepeatedOps = 0;
    uint32_t m_levelStateSerializerTraceDroppedOps = 0;
    uint32_t m_levelStateSerializerTraceLimit = 256;
    uint32_t m_saveSchemaTraceClassifiedOps = 0;
    uint32_t m_saveSchemaTraceUnknownOps = 0;
    std::array<uint32_t, CoopSaveStateBridge::kSchemaSemanticCount> m_saveSchemaTraceBuckets = {};
    uint32_t m_saveLoadSectionTraceCalls = 0;
    uint32_t m_saveLoadSectionTraceReadCalls = 0;
    uint32_t m_saveLoadSectionTraceWriteCalls = 0;
    uint32_t m_saveLoadSectionTraceLogs = 0;
    uint32_t m_saveStateBridgeEvents = 0;
    uint32_t m_saveStateBridgePatchCandidates = 0;
    uint32_t m_coopSaveMergeAttempts = 0;
    uint32_t m_coopSaveMergeCandidates = 0;
    uint32_t m_coopSaveMergePatched = 0;
    uint32_t m_coopSaveMergePassthrough = 0;
    uint32_t m_coopSaveMergeDeferred = 0;
    uint32_t m_coopSaveMergeFailures = 0;
    uint32_t m_serializerNodeProbeAttempts = 0;
    uint32_t m_serializerNodeProbeLogs = 0;
    uint32_t m_serializerNodeProbeSlots = 0;
    uint32_t m_serializerNodeProbeCandidates = 0;
    uint32_t m_serializerNodeProbeGuards = 0;
    uint32_t m_activeSaveWriteCompleteTraceCalls = 0;
    uint32_t m_activeSaveWriteCompleteTraceSuccesses = 0;
    uint32_t m_activeSaveWriteCompleteTraceFailures = 0;
    uint32_t m_nativeFinalStreamSinkCalls = 0;
    uint32_t m_nativeFinalStreamCapturedChunks = 0;
    uint32_t m_nativeFinalStreamCapturedBytes = 0;
    uint32_t m_nativeFinalStreamBufferedBytes = 0;
    uint32_t m_nativeFinalStreamDroppedBytes = 0;
    uint32_t m_nativeFinalStreamDumpWrites = 0;
    uint32_t m_nativeFinalStreamGuards = 0;
    uint32_t m_nativeFinalStreamChecksum = 2166136261u;
    uint32_t m_nativeFinalStoreObjectCalls = 0;
    uint32_t m_nativeFinalStoreObjectTraces = 0;
    uint32_t m_nativeFinalStoreObjectGuards = 0;
    uint32_t m_nativeWriteNodeOracleCalls = 0;
    uint32_t m_nativeWriteNodeOracleTraces = 0;
    uint32_t m_nativeWriteNodeOracleGuards = 0;
    uint32_t m_nativeWriteNodeOracleAttrRecords = 0;
    uint32_t m_nativeWriteNodeOracleChildRecords = 0;
    uint32_t m_nativeWriteNodeOracleStored = 0;
    uint32_t m_nativeWriteNodeOracleDropped = 0;
    std::vector<CoopNativeFragmentPayload::PayloadWriteNodeOracleInputRecord> m_nativeWriteNodeOracleRecords;
    uint32_t m_nativeWriteStoreProbeCalls = 0;
    uint32_t m_nativeWriteStoreProbeSuccesses = 0;
    uint32_t m_nativeWriteStoreProbeGuards = 0;
    uint32_t m_nativeWriteAllocatorProbeCalls = 0;
    uint32_t m_nativeWriteAllocatorProbeSuccesses = 0;
    uint32_t m_nativeWriteAllocatorProbeGuards = 0;
    uint32_t m_nativeLoadStoreInitCalls = 0;
    uint32_t m_nativeLoadStoreInitTraces = 0;
    uint32_t m_nativeLoadStoreInitGuards = 0;
    uint32_t m_nativeReadStoreGraphProbes = 0;
    uint32_t m_nativeReadStoreGraphSuccesses = 0;
    uint32_t m_nativeReadStoreGraphGuards = 0;
    uint32_t m_nativeScratchLoadStoreProbes = 0;
    uint32_t m_nativeScratchLoadStoreSuccesses = 0;
    uint32_t m_nativeScratchLoadStoreGuards = 0;
    uint32_t m_nativeReadStoreFragmentProbes = 0;
    uint32_t m_nativeReadStoreFragmentSuccesses = 0;
    uint32_t m_nativeReadStoreFragmentFailures = 0;
    uint32_t m_nativeReadStoreFragmentBytes = 0;
    struct NativeFinalStoreObjectSnapshot
    {
        uint32_t table14Begin = 0;
        uint32_t table14End = 0;
        uint32_t table1cBegin = 0;
        uint32_t table1cEnd = 0;
        uint32_t table24Begin = 0;
        uint32_t table24End = 0;
        uint32_t childEntries = 0;
        uint32_t attrTokens = 0;
        uint32_t finalBytes = 0;
        uint32_t rootNodeIndex = 0;
        uint8_t flags = 0;
        std::uintptr_t streamOwner = 0;
        std::uintptr_t currentChunk = 0;
        bool valid = false;
    };
    NativeFinalStoreObjectSnapshot m_nativeFinalStoreObjectSnapshot;
    std::string m_activeNativeFinalStreamSaveName = "-";
    std::string m_lastNativeFinalStreamDumpPath = "-";
    std::string m_lastNativeFinalStoreObjectEvent = "-";
    std::string m_lastNativeWriteNodeOracleEvent = "-";
    std::string m_lastNativeWriteStoreProbeEvent = "-";
    std::string m_lastNativeWriteAllocatorProbeEvent = "-";
    std::string m_lastNativeLoadStoreInitEvent = "-";
    std::string m_lastNativeReadStoreGraphEvent = "-";
    std::string m_lastNativeScratchLoadStoreEvent = "-";
    std::string m_lastNativeReadStoreFragmentEvent = "-";
    std::vector<uint8_t> m_nativeFinalStreamBuffer;
    uint32_t m_nativeSaveStoreApiChecks = 0;
    uint32_t m_nativeSaveStoreApiCheckSuccesses = 0;
    uint32_t m_nativeSaveStoreApiCheckFailures = 0;
    uint32_t m_nativeGameStatePlayerFragmentCandidates = 0;
    uint32_t m_nativeGameStateInventoryFragmentCandidates = 0;
    uint32_t m_nativeGameStateItemFragmentCandidates = 0;
    uint32_t m_nativeGameStateFragmentReadCandidates = 0;
    uint32_t m_nativeGameStateFragmentWriteCandidates = 0;
    uint32_t m_nativeFragmentPayloadExportAttempts = 0;
    uint32_t m_nativeFragmentPayloadExportSuccesses = 0;
    uint32_t m_nativeFragmentPayloadExportFailures = 0;
    uint32_t m_nativeFragmentPayloadExportBytes = 0;
    uint32_t m_nativeLoadFragmentSectionReads = 0;
    uint32_t m_nativeLoadFragmentSectionSuccesses = 0;
    uint32_t m_nativeLoadFragmentSectionFailures = 0;
    uint32_t m_nativeLoadFragmentSectionBytes = 0;
    uint64_t m_lastNativeFragmentPayloadSchemaHash = 0;
    uint64_t m_lastNativeFragmentPayloadContentHash = 0;
    uint64_t m_nextNativeFragmentRunId = 1;
    uint64_t m_activeNativeFragmentCaptureRunId = 0;
    uint32_t m_nativeInventoryScopeEnters = 0;
    uint32_t m_nativeInventoryScopeExits = 0;
    uint32_t m_nativeItemScopeEnters = 0;
    uint32_t m_nativeItemScopeExits = 0;
    uint32_t m_nativeItemScopeFallbacks = 0;
    uint64_t m_nextNativeInventoryScopeSeq = 1;
    uint64_t m_nextNativeItemScopeSeq = 1;
    NativeInventorySerializeScopeState m_nativeInventoryScope;
    std::vector<NativeInventorySerializeScopeState> m_nativeInventoryScopeStack;
    CoopNativeGameStateFragmentLocator m_nativeFragmentLocator;
    CoopNativeSaveBridge::TargetReadFragmentCapture m_nativeTargetFragmentCapture;
    uint32_t m_nativeFragmentApplyAttempts = 0;
    uint32_t m_nativeFragmentApplySuccesses = 0;
    uint32_t m_nativeFragmentApplyFailures = 0;
    std::string m_lastNativeFragmentApplyEvent = "-";
    uint32_t m_nativePreloadSaveMergeAttempts = 0;
    uint32_t m_nativePreloadSaveMergeReady = 0;
    uint32_t m_nativePreloadSaveMergeFailures = 0;
    uint32_t m_nativePreloadSaveMergeSourceItems = 0;
    uint32_t m_nativePreloadSaveMergePayloadBytes = 0;
    uint32_t m_nativePreloadSaveMergeSaveBytes = 0;
    uint32_t m_nativePreloadSaveMergeSaveChecksum = 0;
    uint32_t m_nativePreloadSaveMergePatched = 0;
    uint64_t m_nativePreloadSaveMergeSchemaHash = 0;
    uint64_t m_nativePreloadSaveMergeContentHash = 0;
    std::string m_lastNativePreloadSaveMergeEvent = "-";
    std::string m_nativePreloadSaveMergeOutputPath;
    std::vector<uint8_t> m_lastNativeFragmentImportPayload;
    uint32_t m_clientAreaAuthorityActivations = 0;
    uint32_t m_clientAreaAuthorityScans = 0;
    uint32_t m_clientAreaAuthorityHits = 0;
    uint32_t m_clientLevelStateBridgeSeeds = 0;
    uint32_t m_clientLevelStateBridgeSyncs = 0;
    uint32_t m_clientLevelStateBridgeCleanups = 0;
    uint32_t m_clientLevelStateBridgeFailures = 0;
    uint32_t m_dialogPlayTransitionSuppressions = 0;
    uint32_t m_visualPerceptionTransitionSuppressions = 0;
    uint32_t m_visualPerceptionAcquireEntries = 0;
    uint32_t m_visualPerceptionAcquireSkips = 0;
    uint32_t m_visualPerceptionAcquireSuppressions = 0;
    uint32_t m_postLoadVisualPerceptionSuppressFrames = 0;
    uint32_t m_nativePerceptionLevelLoadStarts = 0;
    EntityId m_lastLocalAttentionClearEnemy = INVALID_ENTITYID;
    int m_lastTimeScaleHandle = 0;
    unsigned m_lastTimeScaleTimers = 0;
    float m_lastTimeScaleValue = 1.0f;
    std::string m_lastTimeScaleEvent = "-";
    std::string m_lastLocalAttentionClearStage = "-";
    std::string m_lastSuppressedDownedAction = "-";
    std::string m_lastProxyNoPropCollisionEvent = "-";
    std::string m_lastProxyNativeAiActionEvent = "-";
    std::string m_lastRemoteProxyTransformEvent = "-";
    std::string m_lastRemoteEnemyPhysicsEvent = "-";
    std::string m_lastRemoteProxyVisualEvent = "-";
    std::string m_lastAnimationTestEvent = "-";
    std::string m_animationTestArchetypeText = "10739735956144685611";
    std::string m_animationTestModelPath = "objects/characters/humansfinal/sylvianbellamy.cdf";
    std::string m_animationTestName = "idle";
    int m_animationTestSlot = 0;
    int m_animationTestLayer = 0;
    float m_animationTestBlend = 0.15f;
    float m_animationTestSpeed = 1.0f;
    float m_animationTestTime = 0.0f;
    bool m_animationTestNativeHitReactionsEnabled = false;
    bool m_animationTestTraceActive = false;
    float m_animationTestTraceRemaining = 0.0f;
    float m_animationTestTraceElapsed = 0.0f;
    float m_animationTestTraceAccumulator = 0.0f;
    float m_animationTestTraceInterval = 0.1f;
    uint32_t m_animationTestTraceSamples = 0;
    std::string m_animationTestTraceLabel = "-";
    std::string m_animationTestTraceSummary = "-";
    bool m_animationTestPoseHoldActive = false;
    EntityId m_animationTestPoseHoldEntityId = INVALID_ENTITYID;
    std::string m_animationTestPoseHoldName = "-";
    std::string m_animationTestPoseHoldClip = "-";
    int m_animationTestPoseHoldSlot = 0;
    int m_animationTestPoseHoldLayer = 0;
    float m_animationTestPoseHoldTime = 0.0f;
    float m_animationTestPoseHoldBlend = 0.05f;
    float m_animationTestPoseHoldAccumulator = 0.0f;
    float m_animationTestPoseHoldInterval = 0.05f;
    uint32_t m_animationTestPoseHoldTicks = 0;
    std::string m_animationTestPoseHoldLast = "-";
    bool m_remoteProxyPoseHoldActive = false;
    EntityId m_remoteProxyPoseHoldEntityId = INVALID_ENTITYID;
    std::string m_remoteProxyPoseHoldName = "-";
    std::string m_remoteProxyPoseHoldClip = "-";
    int m_remoteProxyPoseHoldSlot = 0;
    int m_remoteProxyPoseHoldLayer = 0;
    float m_remoteProxyPoseHoldTime = 0.0f;
    float m_remoteProxyPoseHoldBlend = 0.05f;
    float m_remoteProxyPoseHoldAccumulator = 0.0f;
    float m_remoteProxyPoseHoldInterval = 0.05f;
    float m_remoteProxyPoseHoldDuration = 1.0f;
    bool m_remoteProxyPoseHoldLoop = false;
    uint32_t m_remoteProxyPoseHoldTicks = 0;
    std::string m_remoteProxyPoseHoldLast = "-";
    float m_localPoseFireImpulseSeconds = 0.0f;
    float m_localPoseReloadImpulseSeconds = 0.0f;
    float m_localPoseMeleeImpulseSeconds = 0.0f;
    float m_localPoseSwitchImpulseSeconds = 0.0f;
    float m_localPosePsiImpulseSeconds = 0.0f;
    float m_localPosePsiImpactImpulseSeconds = 0.0f;
    float m_localPoseZeroGMoveHoldSeconds = 0.0f;
    bool m_localPoseAttackHeld = false;
    bool m_localPoseReloadHeld = false;
    bool m_localPoseSwitchHeld = false;
    bool m_localPosePsiHeld = false;
    uint16_t m_localPoseFireSerial = 0;
    uint16_t m_localPoseReloadSerial = 0;
    uint16_t m_localPoseMeleeSerial = 0;
    uint16_t m_localPoseSwitchSerial = 0;
    uint16_t m_localPosePsiSerial = 0;
    uint16_t m_localPosePsiFxKind = 0;
    int m_localPosePsiPower = 0;
    uint16_t m_localPosePsiImpactSerial = 0;
    uint16_t m_localPosePsiImpactFxKind = 0;
    int m_localPosePsiImpactPower = 0;
    Vec3 m_localPosePsiImpactPosition = Vec3(ZERO);
    struct QueuedLocalPosePsiImpact
    {
        uint16_t fxKind = 0;
        int power = 0;
        Vec3 position = Vec3(ZERO);
        std::string reason;
    };
    std::vector<QueuedLocalPosePsiImpact> m_queuedLocalPosePsiImpacts;
    bool m_hasQueuedLocalPosePsiImpact = false;
    uint16_t m_queuedLocalPosePsiImpactFxKind = 0;
    int m_queuedLocalPosePsiImpactPower = 0;
    Vec3 m_queuedLocalPosePsiImpactPosition = Vec3(ZERO);
    std::string m_queuedLocalPosePsiImpactReason;
    Vec3 m_localPoseShiftStartPosition = Vec3(ZERO);
    bool m_hasLocalPoseShiftStartPosition = false;
    uint16_t m_lastLocalPosePsiReleaseFxKind = 0;
    int m_lastLocalPosePsiReleasePower = 0;
    float m_localPosePsiCancelSuppressSeconds = 0.0f;
    unsigned m_lastLocalPoseFireWeaponId = 0;
    int m_lastLocalPoseFireAmmoLoaded = -1;
    bool m_hasLastLocalPoseFireState = false;
    bool m_lastLocalPoseFireAttacking = false;
    unsigned m_lastLocalPoseReloadWeaponId = 0;
    bool m_hasLastLocalPoseReloadState = false;
    bool m_lastLocalPoseReloading = false;
    bool m_hasLastLocalPosePsiState = false;
    bool m_lastLocalPosePsiEngaged = false;
    int m_lastLocalPosePsiPower = 0;
    bool m_localPoseMimicActive = false;
    uint16_t m_localPoseMimicVisualSerial = 0;
    EntityId m_localPoseMimicSourceEntityId = INVALID_ENTITYID;
    uint64_t m_localPoseMimicSourceGuid = 0;
    std::string m_localPoseMimicModelPath;
    Vec3 m_localPoseMimicPosition = Vec3(ZERO);
    Quat m_localPoseMimicRotation = Quat(IDENTITY);
    Vec3 m_localPoseMimicScale = Vec3Constants<float>::fVec3_One;
    bool m_remotePoseMimicActive = false;
    uint16_t m_remotePoseMimicVisualSerial = 0;
    EntityId m_remoteMimicVisualEntityId = INVALID_ENTITYID;
    int m_remoteMimicVisualSlotIndex = -1;
    int m_remoteMimicHiddenSlotIndex = -1;
    uint32_t m_remoteMimicHiddenSlotFlags = 0;
    bool m_remoteMimicHiddenSlotValid = false;
    std::string m_remoteMimicVisualModelPath;
    std::string m_lastRemoteMimicVisualEvent = "-";
    float m_remotePoseFireCooldownSeconds = 0.0f;
    float m_remotePoseReloadCooldownSeconds = 0.0f;
    float m_remotePoseMeleeCooldownSeconds = 0.0f;
    float m_remotePoseSwitchCooldownSeconds = 0.0f;
    float m_remotePosePsiCooldownSeconds = 0.0f;
    float m_remotePoseBaseActionSeconds = 0.0f;
    float m_remotePoseZeroGTravelSpeed = 0.0f;
    float m_remotePoseZeroGTravelAngle = 0.0f;
    uint16_t m_lastRemotePoseFireSerial = 0;
    uint16_t m_lastRemotePoseReloadSerial = 0;
    uint16_t m_lastRemotePoseMeleeSerial = 0;
    uint16_t m_lastRemotePoseSwitchSerial = 0;
    uint16_t m_lastRemotePosePsiSerial = 0;
    uint16_t m_lastRemotePosePsiImpactSerial = 0;
    bool m_remotePoseActionOverlayActive = false;
    EntityId m_remotePoseActionOverlayEntityId = INVALID_ENTITYID;
    int m_remotePoseActionOverlayLayer = 1;
    uint32_t m_lastLocalPoseWeaponClass = 0;
    bool m_hasLastLocalPoseWeaponClass = false;
    uint32_t m_stableLocalPoseWeaponClass = 0;
    uint8_t m_missingLocalPoseWeaponSamples = 0;
    std::string m_lastPoseActionEvent = "-";
    std::string m_lastNpcMannequinActionTraceEvent = "-";
    std::string m_lastNpcMannequinConstructTraceEvent = "-";
    std::string m_lastNpcSemanticTraceEvent = "-";
    std::deque<std::string> m_enemySyncTrace;
    std::string m_lastEnemyMannequinStateEvent = "-";
    std::string m_lastAnimationQueueTraceEvent = "-";
    std::string m_lastAnimationQueueTracePath = "-";
    std::string m_lastSaveLoadEvent = "-";
    std::string m_lastSaveLoadPath = "-";
    std::string m_lastNativeGameStateFragmentEvent = "-";
    std::string m_lastNativeSaveStoreApiEvent = "-";
    std::string m_lastNativeFragmentPayloadEvent = "-";
    std::string m_lastNativeLoadFragmentSectionEvent = "-";
    std::string m_lastNativeFragmentPayloadPath = "-";
    std::string m_lastTransitionPhaseEvent = "-";
    std::string m_postLoadWaitReason = "-";
    std::string m_lastSessionWorldEvent = "-";
    std::string m_lastReliableEvent = "-";
    std::string m_lastDamageDedupeEvent = "-";
    std::string m_lastPlayerSignalDamageEvent = "-";
    std::string m_lastRuntimeSaveGuardEvent = "-";
    std::string m_lastRuntimeTransitionCleanupEvent = "-";
    std::string m_lastRuntimePerceptionEvent = "-";
    std::string m_lastRuntimeEntityFlagsEvent = "-";
    std::string m_lastRuntimeEntitySerializeEvent = "-";
    std::string m_lastDialogEvent = "-";
    std::string m_lastVisualPerceptionEvent = "-";
    std::string m_lastNativePerceptionEvent = "-";
    std::string m_lastNativeEntityLifecycleEvent = "-";
    std::string m_lastArkNpcAbilityOwnerRemovalGuardEvent = "-";
    std::string m_lastNativeNpcSpawnTraceEvent = "-";
    std::string m_pendingHostWorldLevel;
    std::string m_pendingHostWorldSavePath;
    std::string m_lastWorldSyncEvent = "-";
    std::string m_saveTransferSourcePath;
    std::string m_saveTransferReceivePath;
    std::string m_playerStateTransferSourcePath;
    std::string m_clientDisconnectTransferSourcePath;
    std::string m_playerStateTransferReceivePath;
    std::string m_areaJournalTransferSourcePath;
    std::string m_areaJournalTransferReceivePath;
    std::string m_playerStateTransferUsername;
    uint64_t m_playerStateTransferAccountToken = 0;
    std::string m_playerStateTransferSaveKey;
    std::string m_areaJournalTransferUsername;
    std::string m_areaJournalTransferLevel;
    std::string m_deferredAreaJournalTransferSourcePath;
    std::string m_deferredAreaJournalTransferLevel;
    std::string m_deferredAreaJournalTransferReason;
    std::string m_pendingAreaOverlayApplyLevel;
    std::string m_pendingAreaOverlayApplyPath;
    std::string m_pendingAreaOverlayApplyReason;
    std::string m_pendingRemoteAreaHandoffRequestLevel;
    std::string m_clientLocalAreaEnteredByTransitionLevel;
    std::string m_hostPlayerStateSentUsername;
    std::string m_currentHostSaveStateKey;
    struct RecentHostSaveKeyHash
    {
        uint64_t hash = 0;
        float expiresAt = -1.0f;
    };
    std::deque<RecentHostSaveKeyHash> m_recentHostSaveKeyHashes;
    std::string m_pendingHostPlayerStateReason;
    std::string m_pendingHostPlayerStateSaveKey;
    std::string m_pendingClientPlayerStateUploadReason;
    std::string m_pendingClientPlayerStateUploadSaveKey;
    std::string m_lastSaveTransferEvent = "-";
    std::string m_lastPlayerStateTransferEvent = "-";
    std::string m_lastAreaJournalTransferEvent = "-";
    std::string m_lastServerAreaStateEvent = "-";
    std::string m_lastAreaOverlayEvent = "-";
    std::string m_lastLivePropEvent = "-";
    std::string m_lastLivePropEventTrace = "-";
    std::string m_lastLivePropCarryEvent = "-";
    std::string m_lastLivePropDebugTrace = "-";
    std::string m_joinOverlayStageOverride;
    std::string m_lastPlayerSidecarEvent = "-";
    std::string m_lastNativePlayerEvent = "-";
    std::string m_lastNativeInventoryEvent = "-";
    std::string m_lastNativeItemEvent = "-";
    std::string m_lastNativeSideBlobEvent = "-";
    std::string m_lastNativeSideBlobCapturedEvent = "-";
    std::string m_lastNativePlayerXmlPatchEvent = "-";
    std::string m_lastCoopSaveMergeEvent = "-";
    std::string m_lastDebugInventoryEvent = "-";
    std::string m_lastRuntimeInteropEvent = "-";
    std::string m_lastPlayerSidecarPath = "-";
    std::string m_pendingPlayerSidecarSaveReason;
    std::vector<PlayerInventoryItemState> m_pendingPlayerSidecarInventoryItems;
    std::vector<PlayerChipsetState> m_pendingPlayerSidecarChipsets;
    uint64_t m_pendingPlayerSidecarEquippedWeaponArchetypeId = 0;
    std::vector<PlayerQuickSelectState> m_pendingPlayerSidecarQuickSelect;
    bool m_pendingPlayerSidecarHasQuickSelect = false;
    NativeSideBlobCaptureState m_nativeSideBlobCapture;
    NativeSideBlobCaptureState m_lastNativePlayerSaveCapture;
    uint32_t m_nativePlayerSaveCaptureGeneration = 0;
    std::string m_autoLoadSavePath;
    std::string m_lastAutoSkipLoadingScreenEvent = "-";
    std::string m_autoTestMode;
    std::string m_autoTravelLevel;
    std::string m_lastAutoTestEvent = "-";
    std::string m_lastFlashLoadingEvent = "-";
    std::string m_lastClientSaveRedirectEvent = "-";
    std::string m_lastClientLevelStateBridgeEvent = "-";
    std::string m_activeClientCoopSaveSlotDirectory;
    std::string m_activeClientCoopLevelStateDirectory;
    std::string m_clientCoopArkTempBridgePath;
    std::string m_lastLevelTransitionEvent = "-";
    std::string m_lastLevelTransitionPresentationEvent = "-";
    std::string m_lastLevelTransitionScreenLocationEvent = "-";
    std::string m_lastLevelStateEvent = "-";
    std::string m_lastLevelStateSerializeEvent = "-";
    std::string m_lastLevelStateSerializerVtableEvent = "-";
    std::string m_lastLevelStateSerializerTraceEvent = "-";
    std::string m_lastSerializerNodeProbeEvent = "-";
    std::string m_lastSaveStoreMapEvent = "-";
    std::string m_lastActiveSaveWriteCompleteTraceEvent = "-";
    std::string m_lastNativeFinalStreamEvent = "-";
    std::string m_lastSaveSchemaTraceEvent = "-";
    std::string m_lastSaveLoadSectionTraceEvent = "-";
    std::string m_lastSaveStateBridgeEvent = "-";
    std::string m_lastAreaAuthorityEvent = "-";
    std::string m_lastProxyLifecycleEvent = "-";
    std::unordered_map<std::uintptr_t, std::string> m_levelStateSerializerVtableLabels;
    std::unordered_map<std::string, uint32_t> m_levelStateSerializerTraceUniqueKeys;
    std::unordered_set<unsigned> m_gameStateLocalPlayerInventoryItemIds;
    std::unordered_map<unsigned, size_t> m_gameStateLocalPlayerInventoryItemIndexById;
    std::uintptr_t m_gameStateInventoryReferenceSerializer = 0;
    std::unordered_map<unsigned, uint32_t> m_gameStateInventoryReferenceCounts;
    std::unordered_map<unsigned, uint32_t> m_gameStateInventoryExternalReferenceCounts;
    std::unordered_set<unsigned> m_gameStateInventoryLocalReferenceIds;
    uint32_t m_nativeGameStateInventoryReferencePasses = 0;
    uint32_t m_nativeGameStateInventoryReferenceObservations = 0;
    uint32_t m_nativeGameStateInventoryReferenceLocalIds = 0;
    uint32_t m_nativeGameStateInventoryReferenceConflicts = 0;
    std::string m_lastNativeGameStateInventoryReferenceEvent = "-";
    std::string m_lastRequestedServerAreaStateLevel;
    std::string m_nativeNpcSpawnTraceFilter = "arkhuman";
    CoopAreaStateOverlayApplyStats m_lastAreaOverlayStats;
    uint64_t m_debugInventoryArchetypeId = 0;
    int m_debugInventoryCount = -1;
    float m_lastDownedHudOverlayLogTime = -1000.0f;
    float m_lastPeerThrottleHudOverlayLogTime = -1000.0f;
    float m_lastPeerTimeoutHudOverlayLogTime = -1000.0f;
    float m_peerConnectionThrottleStartTime = -1.0f;
    float m_peerTimeoutWarningStartTime = -1.0f;
    float m_peerTimeoutResumeCandidateStartTime = -1.0f;
    float m_joinOverlayStartTime = -1.0f;
    float m_joinOverlayLastProgressTime = -1.0f;
    float m_queuedHudFeedbackDuration = 3.0f;
    float m_postLoadInteractiveSeconds = 0.0f;
    float m_flashLoadtimeUpdateSeconds = 0.0f;
    int m_lastFlashLoadingProgress = -1;
    float m_worldSyncRequestAccumulator = 0.0f;
    float m_saveTransferSnapshotWaitSeconds = 0.0f;
    float m_hostInternalSnapshotRetrySeconds = 0.0f;
    float m_playerStateUploadAccumulator = 0.0f;
    float m_playerSidecarSaveAccumulator = 0.0f;
    float m_playerSidecarApplyAccumulator = 0.0f;
    float m_playerSidecarDeferredSaveAccumulator = 0.0f;
    float m_playerSidecarInventoryRestoreAccumulator = 0.0f;
    float m_pendingAreaOverlayApplyDelaySeconds = 0.0f;
    float m_livePropTickAccumulator = 0.0f;
    float m_levelTransitionPresentationAccumulator = 0.0f;
    float m_hugePhysicsBoundsRepairAccumulator = 0.0f;
    float m_lastHugePhysicsBoundsEventRepairTime = -1000.0f;
    bool m_pendingHugePhysicsBoundsRepair = false;
    unsigned m_pendingHugePhysicsBoundsRequestedCells = 0;
    Vec3 m_pendingHugePhysicsBoundsQueryMin = Vec3(ZERO);
    Vec3 m_pendingHugePhysicsBoundsQueryMax = Vec3(ZERO);
    float m_pendingNetworkRuntimeCleanupDelaySeconds = 0.0f;
    float m_autoTestAccumulator = 0.0f;
    float m_autoReengageAfterLoadingAccumulator = 0.0f;
    float m_autoTravelDelaySeconds = 0.0f;
    float m_remoteReviveHoldSeconds = 0.0f;
    float m_remoteReviveHoldProgress = 0.0f;
    float m_remoteReviveDistance = 0.0f;
    float m_remoteReviveSuppressDownedPoseSeconds = 0.0f;
    float m_localReviveSuppressDownedStatusSeconds = 0.0f;
    EntityId m_proxyReviveInteractionEntityId = INVALID_ENTITYID;
    std::unordered_set<EntityId> m_proxyReviveInteractionEntityIds;
    Vec3 m_lastMimicAuthorityPos = Vec3(ZERO);
    float m_aiTargetDebugAccumulator = 0.0f;
    float m_aiDebugNearestHostDistance = 0.0f;
    float m_aiDebugNearestProxyDistance = 0.0f;
    CArkUIHUD* m_lastArkUIHUD = nullptr;
    std::uintptr_t m_lastArkUIHUDPointer = 0;
    std::uintptr_t m_flashUIPointer = 0;
    std::uintptr_t m_hudUIElementPointer = 0;
    std::uintptr_t m_markerUIElementPointer = 0;
    uint32_t m_uiHudUpdateHookCalls = 0;
    uint32_t m_uiHudPreRenderHookCalls = 0;
    uint32_t m_coopRenderEndFrameCalls = 0;
    uint32_t m_coopMainUpdateHudDrawCalls = 0;
    uint32_t m_coopMainUpdateImGuiDrawCalls = 0;
    uint32_t m_uiLayerElementCount = 0;
    uint32_t m_uiLayerSortedCount = 0;
    uint32_t m_uiLayerVisibleCount = 0;
    uint32_t m_uiLayerHudFlagCount = 0;
    int m_uiDebugTargetId = -1;
    std::string m_uiDebugStatus = "not scanned";
    std::vector<std::string> m_uiDebugRows;
    std::string m_postFxDebugStatus = "not scanned";
    std::vector<std::string> m_postFxDebugRows;
    std::vector<LevelTransitionDoorDebugRow> m_levelTransitionDebugRows;
    std::string m_levelTransitionPresentationLevel;
    uint32_t m_levelTransitionPresentationNativeCompleteSeen = UINT32_MAX;
    int m_postFxSelectedCandidate = 0;
    int m_selectedLevelTransitionDoor = 0;
    float m_downedPostFxSaturation = 0.516f;
    float m_downedPostFxBrightness = 1.109f;
    float m_downedPostFxContrast = 1.268f;
    float m_downedPostFxBlur = 0.719f;
    float m_downedPostFxChroma = 0.500f;
    float m_downedPostFxVignetteBorder = 1.200f;
    bool m_enableDownedPostFx = true;
    bool m_previewDownedPostFx = false;
    bool m_downedPostFxApplied = false;
    std::unique_ptr<ProxyReviveInteractionListener> m_proxyReviveInteractionListener;
    std::unique_ptr<ILogCallback> m_animationQueueLogCallback;
    std::string m_lastHugePhysicsBoundsRepairEvent = "-";
    std::string m_lastPhysicalWorldBoxHookEvent = "-";
    std::unordered_map<uint64_t, float> m_hugePhysicsBoundsRepairFailureCooldownUntil;
    std::deque<PendingReliablePacket> m_reliableSendQueue;
    std::unordered_map<uint64_t, ReliableEndpointState> m_reliableEndpointStates;
    struct ChatTextRateState
    {
        float windowStart = -1.0f;
        float lastMessage = -1000.0f;
        uint32_t count = 0;
    };
    std::unordered_map<uint64_t, RemotePeerSession> m_remotePeers;
    std::unordered_map<uint64_t, ChatTextRateState> m_chatTextRates;
    std::unordered_map<uint64_t, HostPlayerStateUploadReceive> m_hostPlayerStateUploadReceives;
    std::unordered_set<uint64_t> m_pendingHostPlayerStateUploadRequests;
    std::unordered_set<uint64_t> m_kickedAccountTokens;
    std::vector<ServerBrowserEntry> m_serverBrowserEntries;
    uint64_t m_activeRemotePeerToken = 0;
    uint64_t m_primaryRemotePeerToken = 0;
    uint64_t m_activePacketSourceAccountToken = 0;
    int m_maxSessionPlayers = 4;
    CoopNetworkTelemetry m_networkTelemetry;
    CoopChat* m_chat = nullptr;
    CoopCoverageDiscovery m_coverageDiscovery;
    std::atomic<uint64_t> m_runtimeLogEmissions{0};
    uint64_t m_runtimeTransformHookCalls = 0;
    uint64_t m_runtimeTransformRewrites = 0;
    uint64_t m_runtimeEntityScanCalls = 0;
    uint64_t m_runtimeEntityScanCandidates = 0;
    uint64_t m_runtimeEntityScanTotalMicroseconds = 0;
    uint64_t m_runtimeEntityScanMaxMicroseconds = 0;
    std::deque<DamageDedupeEntry> m_damageDedupeEntries;
    PendingLocalDamageSignal m_pendingLocalDamageSignal;
    std::unordered_map<EntityId, PendingEnemyDamageSignal> m_pendingEnemyDamageSignals;
    std::unordered_map<EntityId, uint64_t> m_enemyNetIdsByEntity;
    std::unordered_map<EntityId, uint64_t> m_enemyStableSpawnIdsByEntity;
    std::unordered_map<EntityId, uint64_t> m_enemyRaisedFromCorpseSourcesByEntity;
    std::unordered_map<EntityId, uint64_t> m_enemyEthericDoppelgangerSourcesByEntity;
    std::unordered_map<EntityId, uint32_t> m_enemyEthericDoppelgangerGenerationsByEntity;
    std::unordered_set<EntityId> m_enemyEthericDoppelgangerRequestsSentByEntity;
    std::unordered_map<EntityId, uint32_t> m_pendingEthericDoppelgangerInitFrames;
    std::unordered_map<uint64_t, EnemyAuthorityState> m_enemyAuthorities;
    std::unordered_map<uint64_t, EnemyRosterRecord> m_enemyRosterByNetId;
    std::deque<PendingRemoteCorpsePhantomResult> m_pendingRemoteCorpsePhantomResults;
    std::deque<PendingCorpsePhantomSpawnRequest> m_pendingCorpsePhantomSpawnRequests;
    std::unordered_map<uint64_t, RemoteEnemyMovementDesireState> m_remoteEnemyMovementDesires;
    std::unordered_map<uint64_t, EnemyPuppetState> m_enemyPuppets;
    std::unordered_map<uint64_t, PendingEnemyDeathCommit> m_pendingEnemyDeathCommits;
    std::unordered_map<uint64_t, CoopProtocol::EnemyDeathPresentationPacket> m_enemyDeathPresentations;
    std::unordered_map<uint64_t, uint32_t> m_enemyDeathPresentationLastSequences;
    std::unordered_map<ArkOperatorLaserHelper*, OperatorLaserBinding> m_operatorLaserBindings;
    std::unordered_map<uint64_t, EntityId> m_remoteTurretReplicasByStableKey;
    std::unordered_set<EntityId> m_remoteTurretNativeAiDisabledEntityIds;
    std::unordered_set<uint64_t> m_turretCarriedStableKeys;
    std::unordered_map<uint64_t, TurretAuthorityState> m_turretAuthorities;
    uint32_t m_turretAuthorityClaimsRequested = 0;
    uint32_t m_turretAuthorityClaimsGranted = 0;
    uint32_t m_turretAuthorityClaimsRejected = 0;
    uint32_t m_turretAuthorityReclaims = 0;
    std::string m_lastTurretAuthorityEvent = "-";
    std::unordered_map<uint64_t, uint32_t> m_enemyLocomotionLastSequences;
    std::unordered_map<uint64_t, uint32_t> m_enemyProjectileLastSequences;
    std::unordered_map<uint64_t, uint32_t> m_enemyAbilityFxLastSequences;
    std::unordered_map<uint64_t, uint32_t> m_turretStateLastSequences;
    std::vector<EntityId> m_debugSpawnedEnemyEntityIds;
    std::unordered_set<EntityId> m_pendingEnemyRegistryEntityIds;
    std::unordered_map<uint64_t, LivePropState> m_liveProps;
    std::unordered_set<uint64_t> m_areaOverlayLivePropCandidates;
    float m_areaOverlayLivePropCandidateUntilTime = -1000.0f;
    uint64_t m_nextEnemyNetId = 100;
    uint32_t m_enemyRosterSequence = 0;
    uint32_t m_enemyRosterSentPackets = 0;
    uint32_t m_enemyRosterReceivedPackets = 0;
    uint32_t m_enemyRosterAppliedPackets = 0;
    uint32_t m_enemyRosterDroppedPackets = 0;
    std::string m_lastEnemyRosterEvent = "-";
    float m_enemyRegistryScanAccumulator = 0.0f;
    float m_enemyAuthorityTickAccumulator = 0.0f;
    float m_clientEnemyCullAccumulator = 0.0f;
    bool m_enemyRegistryNeedsScan = true;
    bool m_clientEnemyCullNeedsScan = true;
    float m_proxyCombatStimulusAccumulator = 0.0f;
    float m_proxyAttentionClearAccumulator = 0.0f;
    float m_remoteDownedRecoveryAccumulator = 0.0f;
    Vec3 m_fallRecoverySafePosition = Vec3(ZERO);
    Quat m_fallRecoverySafeRotation = Quat::CreateIdentity();
    Vec3 m_fallRecoveryLastObservedPosition = Vec3(ZERO);
    std::string m_fallRecoveryTrackingLevel;
    uint32_t m_fallRecoveryTrackingLevelEpoch = 0;
    float m_fallRecoveryGroundedSeconds = 0.0f;
    float m_fallRecoveryAirSeconds = 0.0f;
    float m_fallRecoveryCooldownSeconds = 0.0f;
    float m_fallRecoveryLastDropMeters = 0.0f;
    float m_fallRecoveryLastVerticalSpeed = 0.0f;
    int m_fallRecoveryMovementStateId = 0;
    uint32_t m_fallRecoverySafeCaptures = 0;
    uint32_t m_fallRecoveryRecoveries = 0;
    uint32_t m_fallRecoveryPolicyRejects = 0;
    uint32_t m_fallRecoverySelfTestPasses = 0;
    bool m_fallRecoverySafeValid = false;
    bool m_fallRecoveryHasLastObservedPosition = false;
    std::string m_lastFallRecoveryEvent = "-";
    float m_localDownedAttentionClearAccumulator = 0.0f;
    float m_localDownedFocusCleanupAccumulator = 0.0f;
    float m_downedEngineHealthFloor = 1.0f;
    float m_reviveHealth = 10.0f;
    int m_localDownedAttentionClearMode = 1;
    int m_localDownedAttentionMaxEnemiesPerTick = 1;
    uint32_t m_localDownedAttentionScanCursor = 0;
    uint32_t m_enemyLocomotionBinds = 0;
    uint32_t m_enemyLocomotionApplies = 0;
    uint32_t m_enemyLocomotionDrops = 0;
    uint32_t m_enemyHealthReconciles = 0;
    uint32_t m_enemyHealthReconcileFailures = 0;
    uint32_t m_enemyLocomotionLastFlags = 0;
    uint32_t m_enemyLocomotionLastLevel = 0;
    uint32_t m_enemyLocomotionLastAttackKind = 0;
    uint32_t m_enemyLocomotionLastMannequinSequence = 0;
    int32_t m_enemyLocomotionLastMannequinFragmentId = -1;
    float m_enemyLocomotionLastSpeed = 0.0f;
    uint32_t m_enemyAuthorityClaimsSent = 0;
    uint32_t m_enemyAuthorityReleasesSent = 0;
    uint32_t m_enemyAuthoritySnapshotsSent = 0;
    uint32_t m_enemyAuthorityClaimsAccepted = 0;
    uint32_t m_enemyAuthorityClaimsDenied = 0;
    uint32_t m_enemyAuthorityRemoteApplies = 0;
    uint32_t m_enemyAuthorityCandidateRouteAttempts = 0;
    uint32_t m_enemyAuthorityCandidateRouteSends = 0;
    uint32_t m_enemyAuthorityCandidateReceives = 0;
    uint32_t m_enemyAttentionGainedEdges = 0;
    uint32_t m_enemyAttentionLostEdges = 0;
    uint32_t m_enemyAttentionLocalGainedEdges = 0;
    uint32_t m_enemyAttentionLocalLostEdges = 0;
    bool m_followLocalPlayer = false;
    bool m_keepDistractionsSuppressed = true;
    bool m_configureOnSpawn = true;
    bool m_showRemoteNameplate = true;
    bool m_showCoopHudOverlay = true;
    bool m_tintDownedHudOverlay = true;
    bool m_previewCoopHudOverlay = false;
    bool m_enableExpensiveAiDebug = false;
    bool m_damageSyncEnabled = true;
    bool m_damageDedupeEnabled = true;
    bool m_downedModeEnabled = true;
    bool m_lockLocalInputWhileDowned = true;
    bool m_forceLocalCrawlWhileDowned = true;
    bool m_tiltRemoteProxyWhileDowned = false;
    int m_debugPostProxySpawnTraceFrames = 0;
    bool m_useNativeReviveInteraction = true;
    bool m_slowLocalMovementWhileDowned = false;
    bool m_autoTeleportRemoteDownedToHost = false;
    bool m_useHookedNpcDamage = true;
    bool m_syncTestMimicSpawn = false;
    bool m_overrideProxyCharacterName = true;
    bool m_proxyUsePlayerControlledFlag = false;
    bool m_proxyLifecycleDisableSensesPushed = false;
    bool m_proxyLifecycleDisableVisiblePushed = false;
    bool m_proxyLifecycleDisableAudiblePushed = false;
    bool m_enemyLocomotionSyncEnabled = true;
    bool m_enemyAttentionAuthoritySyncEnabled = true;
    bool m_adoptClientLocalEnemiesAsPuppets = false;
    bool m_stimulateHostEnemySimpleAttentionOnProxy = false;
    bool m_clearEnemyAttentionOnDownedProxy = true;
    bool m_clearLocalPlayerAttentionWhileDowned = false;
    bool m_disableLocalPlayerAttentionWhileDowned = true;
    bool m_allowNativeDeathFeedbackForDowned = true;
    bool m_rebindProxyAfterLocalDownedAttentionClear = false;
    bool m_verboseLocalDownedAttentionLog = false;
    bool m_blockNativeTimeScaleWhileDowned = true;
    bool m_useProxyComplexAttention = false;
    bool m_forceHostEnemiesTargetProxy = false;
    bool m_stimulateHostEnemyAbilitiesOnProxy = false;
    bool m_pendingProxyAttentionClear = false;
    bool m_proxyComplexAttentionRegistered = false;
    bool m_proxyComplexVisualRegistered = false;
    bool m_proxyComplexAuralRegistered = false;
    bool m_proxyComplexRoomRegistered = false;
    bool m_applyingRemotePlayerDamage = false;
    bool m_mimicIsPuppet = false;
    bool m_hadAuthoritativeMimic = false;
    bool m_sentMimicDeadState = false;
    bool m_mimicHealthAvailable = false;
    std::string m_lastEnemyLocomotionEvent = "-";
    std::string m_lastEnemyHealthEvent = "-";
    std::string m_lastEnemyAuthorityEvent = "-";
    std::string m_lastEnemyLookEvent = "-";
    std::string m_lastDebugEnemyAttentionEvent = "-";
    bool m_proxyWasConfigured = false;
    bool m_winsockStarted = false;
    bool m_hasLastLocalPlayerPos = false;
    bool m_hasRemoteEndpoint = false;
    bool m_hasRemoteSession = false;
    bool m_sessionGameplayReady = false;
    bool m_hasProxyHealthBaseline = false;
    bool m_proxyHealthAvailable = false;
    bool m_remoteProxySlotVisualStateValid = false;
    bool m_remoteProxySlotVisualDowned = false;
    bool m_proxyStandStanceApplied = false;
    bool m_animationQueueLogCallbackRegistered = false;
    EntityId m_remoteProxySlotVisualEntityId = INVALID_ENTITYID;
    int m_remoteProxySlotVisualSlotIndex = -1;
    EntityId m_proxyWeaponVisualEntityId = INVALID_ENTITYID;
    int m_proxyWeaponVisualSlotIndex = -1;
    uint32_t m_proxyWeaponVisualClass = 0;
    bool m_proxyWeaponVisualCrouched = false;
    bool m_proxyWeaponVisualLowCrouched = false;
    bool m_proxyWeaponVisualZeroG = false;
    std::string m_proxyWeaponVisualPath;
    Vec3 m_proxyWeaponVisualOffset = Vec3(0.20f, 0.52f, 1.32f);
    Ang3 m_proxyWeaponVisualAngles = Ang3(0.0f, 0.0f, 0.0f);
    float m_proxyWeaponVisualScale = 1.0f;
    std::string m_lastProxyWeaponVisualEvent = "-";
    EntityEffects::CEffectsController m_proxyWeaponMuzzleEffects;
    EntityId m_proxyWeaponMuzzleEffectEntityId = INVALID_ENTITYID;
    std::vector<unsigned> m_proxyWeaponMuzzleEffectIds;
    float m_proxyWeaponMuzzleEffectSeconds = 0.0f;
    std::string m_lastProxyWeaponMuzzleEvent = "-";
    struct PendingWorldParticleEffect
    {
        std::string effectName;
        Vec3 position = Vec3(ZERO);
        Vec3 direction = Vec3(0.0f, 1.0f, 0.0f);
        float scale = 1.0f;
        float delaySeconds = 0.0f;
        float repeatSeconds = 0.0f;
        float intervalSeconds = 0.0f;
        std::string reason;
    };
    std::vector<PendingWorldParticleEffect> m_pendingWorldParticleEffects;
    struct ManagedWorldParticleEffect
    {
        std::unique_ptr<ManagedArkLooseEffect> effect;
        _smart_ptr<IParticleEmitter> emitter;
        Vec3 position = Vec3(ZERO);
        float lifeSeconds = 0.0f;
        std::string reason;
    };
    std::vector<ManagedWorldParticleEffect> m_managedWorldParticleEffects;
    struct RemoteThermalTrapMarker
    {
        Vec3 position = Vec3(ZERO);
        Vec3 direction = Vec3(0.0f, 1.0f, 0.0f);
        float lifeSeconds = 8.0f;
        float pulseSeconds = 0.0f;
    };
    std::vector<RemoteThermalTrapMarker> m_remoteThermalTrapMarkers;
    uint32_t m_psiFxSniperSequence = 0;
    std::string m_lastPsiFxSniperEvent = "-";
    std::vector<std::string> m_psiFxSniperTrace;
    uint16_t m_psiFxSniperLastSentPsiSerial = 0;
    uint16_t m_psiFxSniperLastSentImpactSerial = 0;
    uint32_t m_recyclerGrenadeTraceEvents = 0;
    std::string m_lastRecyclerGrenadeTraceEvent = "-";
    uint32_t m_gooProjectileTraceEvents = 0;
    std::string m_lastGooProjectileTraceEvent = "-";
    std::string m_lastGooResultEvent = "-";
    std::string m_lastEnemyProjectileEvent = "-";
    std::string m_lastEnemyAbilityFxEvent = "-";
    std::string m_lastEnemyFxEvent = "-";
    std::string m_lastOperatorLaserEvent = "-";
    std::string m_lastCorpsePhantomEvent = "-";
    bool m_hasLastMimicAuthorityPos = false;
    bool m_localPlayerDowned = false;
    bool m_remotePlayerDowned = false;
    bool m_debugEnemyAttentionOverrideActive = false;
    uint8_t m_debugEnemyAttentionOverrideLevel = CoopEnemyAuthorityPolicy::kUnknownAttention;
    uint64_t m_debugEnemyAttentionOverrideNetId = 0;
    uint64_t m_debugPlayerFollowEnemyNetId = 0;
    float m_debugPlayerFollowEnemyDistance = 0.9f;
    float m_debugPlayerFollowEnemyZOffset = 0.0f;
    float m_debugPlayerFollowEnemyAzimuthDegrees = 0.0f;
    Vec3 m_debugPlayerFollowEnemyDirection = Vec3(1.0f, 0.0f, 0.0f);
    bool m_teamWipe = false;
    bool m_pendingForceLocalDown = false;
    bool m_pendingReviveLocal = false;
    float m_pendingReviveLocalHealth = 0.0f;
    float m_pendingReviveLocalMaxHealth = 0.0f;
    bool m_pendingReviveRemote = false;
    bool m_pendingUnstuckToRemote = false;
    bool m_remoteRevivePromptActive = false;
    bool m_remoteReviveNativeCompleteThisFrame = false;
    bool m_localPlayerHealthWritesDisabled = false;
    bool m_internalPlayerHealthWrite = false;
    bool m_localPlayerStanceWritesDisabled = false;
    bool m_localDownedInputDisabled = false;
    bool m_localDownedWeaponDisabled = false;
    bool m_localDownedAttentionObjectDisabled = false;
    bool m_nativeDeathFeedbackActive = false;
    bool m_saveLoadGuardActive = false;
    bool m_waitingForPostLoadContinue = false;
    bool m_pendingPostLoadResync = false;
    bool m_systemEventListenerRegistered = false;
    bool m_entitySystemSinkRegistered = false;
    bool m_coopRenderListenerRegistered = false;
    bool m_pendingHostWorldRequest = false;
    bool m_hasPendingHostWorldOffer = false;
    bool m_pendingHostSaveLoadBroadcastAfterLoad = false;
    bool m_hostInternalSnapshotSaveActive = false;
    bool m_hostInternalSnapshotWriteCompleteReceived = false;
    bool m_hostInternalSnapshotWriteCompleteObserved = false;
    bool m_hostInternalSnapshotRetryPending = false;
    bool m_clientInternalPlayerSnapshotSaveActive = false;
    bool m_clientPlayerSnapshotForUploadPending = false;
    bool m_saveTransferSending = false;
    bool m_saveTransferReceiving = false;
    bool m_saveTransferStarted = false;
    bool m_saveTransferComplete = false;
    bool m_saveTransferSnapshotPending = false;
    bool m_playerStateTransferSending = false;
    bool m_playerStateTransferReceiving = false;
    bool m_playerStateTransferStarted = false;
    bool m_playerStateTransferComplete = false;
    bool m_areaJournalTransferSending = false;
    bool m_areaJournalTransferReceiving = false;
    bool m_areaJournalTransferStarted = false;
    bool m_areaJournalTransferComplete = false;
    bool m_deferredAreaJournalTransferPending = false;
    bool m_pendingHostPlayerStateSend = false;
    bool m_pendingClientPlayerStateUpload = false;
    bool m_pendingReceivedPlayerStateApply = false;
    // The client can upload its recovery journal before applying a completed
    // host transfer. Keep the received host file addressable while the shared
    // transfer fields are temporarily used for that upload.
    std::string m_pendingHostAuthoritativePlayerStateReceivePath;
    bool m_pendingNativePlayerStateOverrideValid = false;
    PlayerSidecarState m_pendingNativePlayerStateOverride;
    bool m_clientAwaitingHostPlayerState = false;
    bool m_pendingHostWorldLoadAfterPlayerState = false;
    bool m_hostPlayerStatePreloadSent = false;
    bool m_receivedPlayerStateInventoryPreparedForNativeLoad = false;
    bool m_skipNextHostAuthoritativeInventoryApply = false;
    bool m_receivedPlayerStateMergedDuringNativeLoad = false;
    bool m_forceNextPlayerSidecarFullApply = false;
    bool m_forceNextPlayerSidecarPositionApply = false;
    bool m_forceNextPlayerSidecarVitalsApply = false;
    bool m_forceNextPlayerSidecarAbilitiesApply = false;
    bool m_forceNextPlayerSidecarInventoryApply = false;
    bool m_forceNextPlayerSidecarResetTransientState = false;
    bool m_pendingPostJoinModalCleanup = false;
    bool m_suppressNextPlayerSidecarInventoryApply = false;
    uint32_t m_hostPlayerStateSentWorldEpoch = 0;
    float m_receivedPlayerStateApplyDelaySeconds = 0.0f;
    bool m_pendingPlayerSidecarSave = false;
    bool m_localInventoryDirty = false;
    std::string m_localInventoryDirtySaveKey;
    uint64_t m_localInventoryDirtyRevision = 0;
    uint64_t m_localInventoryJournalRevision = 0;
    float m_localInventoryJournalAccumulator = 0.0f;
    bool m_clientRecoveryJournalPending = false;
    bool m_clientRecoveryJournalAwaitingStoredAck = false;
    bool m_clientRecoveryJournalStoredAckReceived = false;
    uint32_t m_clientRecoveryJournalTransferId = 0;
    uint32_t m_clientRecoveryJournalChecksum = 0;
    uint64_t m_clientRecoveryJournalRevision = 0;
    std::string m_clientRecoveryJournalSourcePath;
    std::string m_clientRecoveryJournalSaveKey;
    bool m_pendingPlayerSidecarApply = false;
    bool m_pendingPlayerSidecarInventoryRestore = false;
    bool m_pendingPlayerSidecarInventoryRestoreNeedsClear = false;
    bool m_pendingPlayerSidecarChipsetRestore = false;
    bool m_playerSidecarInventoryNativeRestoreReady = false;
    bool m_playerSidecarInventoryNativeRestoreActive = false;
    bool m_receivedPlayerStateAbilitiesPreparedForNativeLoad = false;
    bool m_lastNativeInventoryLocalSerializeWasRead = false;
    float m_clientPlayerSnapshotCooldownSeconds = 0.0f;
    float m_clientPlayerSnapshotForUploadWaitSeconds = 0.0f;
    bool m_clientPlayerSnapshotFragmentWaitStarted = false;
    uint32_t m_clientPlayerSnapshotForUploadStartGeneration = 0;
    bool m_nativeSideBlobProbeEnabled = false;
    bool m_nativeSideBlobActiveMirrorEnabled = false;
    bool m_levelStateSerializerTraceEnabled = false;
    bool m_levelStateSerializerTraceUniqueOnly = true;
    bool m_saveLoadSectionTraceEnabled = false;
    CoopSaveStateBridge m_saveStateBridge;
    bool m_enablePlayerSidecar = true;
    bool m_applyPlayerSidecarPosition = true;
    bool m_applyPlayerSidecarInventory = false;
    bool m_applyPlayerSidecarAbilities = false;
    bool m_applyPlayerSidecarVitals = true;
    bool m_applyPlayerSidecarChipsets = true;
    bool m_enableExperimentalInventoryRestore = false;
    bool m_localDownedEquipmentWritesDisabled = false;
    bool m_localDownedSavedPsiPowers = false;
    int m_localDownedSavedSelectedPower = 15;
    int m_localDownedSavedEquippedPower = 15;
    bool m_remoteProxySlotVisualsDisabled = false;
    bool m_proxyWeaponVisualDisabled = false;
    bool m_proxyWeaponMuzzleEffectsDisabled = false;
    bool m_proxyDownedSideEffectsDisabled = false;
    bool m_proxyReviveInteractionRegistered = false;
    bool m_autoStartApplied = false;
    std::string m_pendingAutoStartMode;
    bool m_autoLoadPending = false;
    bool m_autoLoadAttempted = false;
    float m_autoLoadStartupSeconds = 0.0f;
    uint32_t m_autoLoadStartupTicks = 0;
    bool m_autoSkipAfterLoadingScreenRequested = false;
    bool m_autoSkipAfterLoadingScreenApplied = false;
    bool m_deferredAutoReengageAfterLoadingScreen = false;
    bool m_traceNativeInventory = false;
    uint32_t m_autoReengageAfterLoadingAttempts = 0;
    bool m_autoTestConfigured = false;
    bool m_autoSidecarTestDone = false;
    bool m_autoTravelDone = false;
    bool m_allowDirectAutoTravelLoad = false;
    bool m_allowExperimentalNativeTransitionInvoke = false;
    bool m_arkLevelTransitionLoadActive = false;
    bool m_runtimeTransitionCleanupPrepared = false;
    bool m_nativeEntityLifecycleTraceEnabled = false;
    bool m_nativeNpcSpawnTraceEnabled = false;
    bool m_clientCoopArkTempBridgeActive = false;
    bool m_clientCoopArkTempBridgeNeedsCleanup = false;
    uint32_t m_activeClientCoopSaveTransferId = 0;
    uint64_t m_pendingArkTransitionLocationId = 0;
    bool m_clientAreaAuthorityActive = false;
    bool m_pendingAreaOverlayApply = false;
    bool m_areaOverlayApplyActive = false;
    bool m_livePropApplyActive = false;
    bool m_enableLivePropSync = true;
    bool m_livePropTraceEvents = false;
    bool m_livePropOpenEventFilter = false;
    bool m_suppressDisconnectNotice = false;
    bool m_peerConnectionThrottleActive = false;
    bool m_peerTimeoutWarningActive = false;
    bool m_peerConnectionLostFreezeActive = false;
    bool m_joinOverlayActive = false;
    bool m_joinInputBlocked = false;
    uint64_t m_lastChairloaderDrawTickMs = 0;
    bool m_showMultiplayerUi = false;
    bool m_showDeveloperUi = false;
    bool m_multiplayerRestoreChairloaderGuiOnClose = false;
    bool m_multiplayerOpenedFromNativeMainMenu = false;
    bool m_multiplayerOpenedFromNativePauseMenu = false;
    bool m_nativePauseMultiplayerFocused = false;
    bool m_multiplayerUiFocusPrimaryOnOpen = false;
    bool m_multiplayerLeaveRequested = false;
    bool m_clientDisconnectFlushPending = false;
    bool m_clientDisconnectFlushAwaitingStoredAck = false;
    bool m_clientDisconnectFlushStoredAckReceived = false;
    bool m_clientDisconnectFlushCaptureAttempted = false;
    uint32_t m_clientDisconnectFlushTransferId = 0;
    uint32_t m_clientDisconnectFlushChecksum = 0;
    float m_clientDisconnectFlushRemainingSeconds = 0.0f;
    float m_clientDisconnectFlushCaptureRetrySeconds = 0.0f;
    std::chrono::steady_clock::time_point m_clientDisconnectFlushDeadline;
    std::string m_clientDisconnectFlushReason;
    std::string m_clientDisconnectFlushSaveKey;
    std::string m_clientDisconnectFlushJournalSourcePath;
    uint64_t m_clientDisconnectFlushJournalRevision = 0;
    bool m_nativeWindowCloseDeferred = false;
    bool m_nativeWindowCloseReentry = false;
    std::uintptr_t m_nativeWindowCloseHandle = 0;
    unsigned m_nativeWindowCloseMessage = 0;
    uint64_t m_nativeWindowCloseWParam = 0;
    std::int64_t m_nativeWindowCloseLParam = 0;
    uint64_t m_multiplayerKickRequestedToken = 0;
    uint64_t m_multiplayerInputSuppressUntilMs = 0;
    float m_multiplayerUiMouseX = -1.0f;
    float m_multiplayerUiMouseY = -1.0f;
    uint32_t m_multiplayerUiMouseClicks = 0;
    bool m_multiplayerUiMouseHovered = false;
    bool m_multiplayerUiNavActive = false;
    bool m_multiplayerUiWantCaptureMouse = false;
    bool m_multiplayerRestartRequired = false;
    bool m_playerPortraitTexturesLoaded = false;
    std::array<_smart_ptr<ITexture>, 6> m_playerPortraitTextures;
    bool m_serverBrowserInitialRefresh = false;
    int m_multiplayerUiTab = 0;
    int m_serverBrowserFilter = 0;
    int m_selectedServerIndex = -1;
    CoopNetworkMode m_lastUiNetworkMode = CoopNetworkMode::Off;
    int m_peerConnectionLostTimeScaleHandle = -1;
    uint32_t m_joinOverlayLastProgressBytes = 0;
    bool m_hasQueuedHudFeedback = false;
    bool m_pendingNetworkRuntimeCleanup = false;
    EntityId m_pendingNetworkRuntimeCleanupProxyId = INVALID_ENTITYID;
    std::vector<EntityId> m_pendingNetworkRuntimeCleanupEntityIds;
    std::string m_pendingNetworkRuntimeCleanupReason;
    std::string m_lastGuardedEntityRemoveEvent = "-";
    std::string m_lastSpawnDiagnosticsEvent = "-";
    EntityId m_proxyComplexAttentionEntityId = INVALID_ENTITYID;
    std::unique_ptr<CoopProxyAttentionObject> m_proxyComplexAttentionObject;
    std::unique_ptr<SystemEventListener> m_systemEventListener;
    std::unique_ptr<CoopEntitySystemSink> m_entitySystemSink;
    CoopRenderListener m_coopRenderListener;
    CoopAreaStateJournal m_areaStateJournal;
    CoopAreaObjectJournal m_areaObjectJournal;
    CoopEvents::Router m_eventRouter;
    CoopRuntimeExtractor m_runtimeExtractor;
    NullUi m_nullUi;
};

extern ModMain* gMod;
